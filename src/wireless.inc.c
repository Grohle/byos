/* wireless.inc.c - "Inalambrico": Wi-Fi y Bluetooth al estilo de Ajustes de iOS, sin la app de HiBy.
 *
 * Entrada: wireless_app() (zoom desde el centro) o wireless_app_at(ix, iy) (desde el icono).
 * wireless_preload() arranca los hilos y lee el estado una vez (llamarlo al iniciar el shell
 * para que la pagina abra con datos); wireless_app() lo llama si no se hizo.
 *
 * Como convive con el reproductor (ver docs/WIRELESS.md):
 *   - Wi-Fi encendido/apagado y Bluetooth encendido/apagado, AirPlay, DLNA y HiBy Link: SOLO a
 *     traves del ajuste del reproductor (hibyset.inc.c, hs_set "wifi"/"bt"/...), para que guarde
 *     el estado y llame a sys_server como su propio Centro de control. Nada de wifi_on.sh aqui.
 *   - Redes Wi-Fi: socket de control de wpa_supplicant (/var/run/wpa_supplicant/wlan0), lo mismo
 *     que hace sys_server con wpa_cli. Al conectar, sys_server ve CTRL-EVENT-CONNECTED, lanza
 *     "udhcpc -b -i wlan0 -q" y avisa al reproductor (WIFI:STATUS): no hace falta DHCP propio.
 *     Las redes se guardan con SAVE_CONFIG en /data/wpa_supplicant.conf (el mismo archivo).
 *   - Bluetooth: bluez 5.54 por D-Bus con dbus-send (dispositivos, Pair, Connect, Disconnect,
 *     RemoveDevice); la busqueda la mantiene un bluetoothctl con "scan on" (bluez la para si su
 *     cliente se va). El agente de emparejado es el bt-agent de HiBy (NoInputNoOutput, PIN 0000).
 *     Al conectar se escribe /data/bt_lastused.txt como sys_server (reconexion al arrancar).
 * Todo lo lento va en dos hilos (Wi-Fi y BT) con su cola; la interfaz solo copia una foto del
 * estado cuando cambia y nunca espera. Solo se sondea mientras la app esta visible. */
#include <spawn.h>
#include <sys/wait.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "hibyset.inc.c"
#include "wireless_parse.inc.c"

#define WL_IF "wlan0"
#define WL_CTRL "/var/run/wpa_supplicant/" WL_IF
#define WL_DBUS "/usr/bin/dbus-send"
#define WL_BTCTL "/usr/bin/bluetoothctl"
#define WL_MAXNET 40
#define WL_MAXSAVED 32
#define WL_MAXDEV 40

/* ------------------------------------------------------------------ estado compartido */
enum { WJ_NONE, WJ_OK, WJ_WRONGKEY, WJ_FAIL };
struct wl_state {
    unsigned gen;
    /* Wi-Fi */
    int wset, wup, wpend;                 /* ajuste del reproductor (-1 ?), wpa responde, cambio en curso (valor+1) */
    char wstate[24], ssid[33], bssid[18], ip[20], mask[20], gw[20], dns[48], mac[18], keymgmt[32];
    int wid, rssi, speed, freq, scanning;
    struct wl_net nets[WL_MAXNET]; int nnets;
    struct wl_saved saved[WL_MAXSAVED]; int nsaved;
    char joining[33];                     /* red a la que se esta uniendo */
    unsigned join_seq; int join_res; char join_ssid[33];
    /* Bluetooth */
    int bset, bavail, bpend;
    struct wl_adapter ad; struct wl_dev devs[WL_MAXDEV]; int ndevs;
    char bbusy[18];                       /* direccion con una operacion en curso */
    /* ajustes del reproductor */
    int hs_ok, airplay, dlna, hibylink;
    /* aviso para la interfaz (clave de tr() + argumento) */
    unsigned msg_seq; const char *msg_key; char msg_arg[64];
};
static struct wl_state WS;                /* lo escriben los hilos (con wl_mu) */
static struct wl_state wv;                /* foto para la interfaz */
static pthread_mutex_t wl_mu = PTHREAD_MUTEX_INITIALIZER, wl_hs_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int wl_vis;               /* 0 oculta, 1 raiz, 2 Wi-Fi, 3 Bluetooth */
static volatile double wl_disc_until;     /* buscar dispositivos BT hasta (ms) */
static int wl_started;

static void wl_msg(const char *key, const char *arg)
{
    pthread_mutex_lock(&wl_mu);
    WS.msg_key = key; wlp_cpy(WS.msg_arg, sizeof WS.msg_arg, arg ? arg : ""); WS.msg_seq++; WS.gen++;
    pthread_mutex_unlock(&wl_mu);
}
static void wl_touch(void) { pthread_mutex_lock(&wl_mu); WS.gen++; pthread_mutex_unlock(&wl_mu); }

/* colas de trabajos (una por hilo) */
enum { J_REFRESH = 1, J_HSREAD, J_HSSET, J_WTOGGLE, J_WSCAN, J_WJOIN, J_WFORGET,
       J_BTOGGLE, J_BCONNECT, J_BDISCONNECT, J_BFORGET };
struct wl_job { int op, iv; char a[72], b[72]; };
struct wl_q { pthread_mutex_t mu; pthread_cond_t cv; struct wl_job j[8]; int h, t; };
static struct wl_q wl_qw = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{0}}, 0, 0 };
static struct wl_q wl_qb = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{0}}, 0, 0 };
static void wl_push(struct wl_q *q, int op, int iv, const char *a, const char *b)
{
    pthread_mutex_lock(&q->mu);
    int dup = 0;
    if (op == J_REFRESH || op == J_WSCAN || op == J_HSREAD)
        for (int i = q->h; i != q->t; i = (i + 1) % 8) if (q->j[i].op == op) dup = 1;
    if (!dup && (q->t + 1) % 8 != q->h) {
        struct wl_job *j = &q->j[q->t];
        j->op = op; j->iv = iv;
        wlp_cpy(j->a, sizeof j->a, a ? a : ""); wlp_cpy(j->b, sizeof j->b, b ? b : "");
        q->t = (q->t + 1) % 8;
        pthread_cond_signal(&q->cv);
    }
    pthread_mutex_unlock(&q->mu);
}
static int wl_pop(struct wl_q *q, struct wl_job *out, int wait_ms)
{
    pthread_mutex_lock(&q->mu);
    if (q->h == q->t && wait_ms != 0) {
        if (wait_ms < 0) pthread_cond_wait(&q->cv, &q->mu);
        else {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_ms / 1000; ts.tv_nsec += (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&q->cv, &q->mu, &ts);
        }
    }
    int got = q->h != q->t;
    if (got) { *out = q->j[q->h]; memset(&q->j[q->h], 0, sizeof q->j[0]); q->h = (q->h + 1) % 8; }   /* sin contrasenas en la cola */
    pthread_mutex_unlock(&q->mu);
    return got;
}

/* ------------------------------------------------------------------ procesos auxiliares */
/* ejecuta argv con stdout+stderr en out; mata el proceso si tarda mas de timeout_ms.
 * Devuelve el codigo de salida, o -1 si no se pudo lanzar o se agoto el tiempo. */
static int wl_run(char *const argv[], char *out, int cap, int timeout_ms)
{
    int fd[2]; if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    int n = 0, killed = 0; double t0 = now_ms();
    if (!r) for (;;) {
        int left = timeout_ms - (int)(now_ms() - t0);
        if (left <= 0) { kill(pid, SIGKILL); killed = 1; break; }
        struct pollfd pf = { fd[0], POLLIN, 0 };
        if (poll(&pf, 1, left > 200 ? 200 : left) <= 0) continue;
        char tmp[1024]; ssize_t k = read(fd[0], tmp, sizeof tmp);
        if (k <= 0) break;
        int c = (int)k; if (c > cap - 1 - n) c = cap - 1 - n;
        if (c > 0) { memcpy(out + n, tmp, (size_t)c); n += c; }
    }
    if (cap > 0) out[n] = 0;
    close(fd[0]);
    if (r) return -1;
    int stt = 0; waitpid(pid, &stt, 0);
    if (killed) return -1;
    return WIFEXITED(stt) ? WEXITSTATUS(stt) : -1;
}

/* ------------------------------------------------------------------ wpa_supplicant */
static int wl_ctrl_open(void)
{
    static int seq;
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un lo = { .sun_family = AF_UNIX }, de = { .sun_family = AF_UNIX };
    snprintf(lo.sun_path, sizeof lo.sun_path, "/tmp/wpa_ctrl_%d-wl%d", (int)getpid(), __sync_add_and_fetch(&seq, 1));
    unlink(lo.sun_path);
    snprintf(de.sun_path, sizeof de.sun_path, "%s", WL_CTRL);
    if (bind(s, (struct sockaddr *)&lo, sizeof lo) || connect(s, (struct sockaddr *)&de, sizeof de)) {
        close(s); unlink(lo.sun_path); return -1;
    }
    return s;
}
static void wl_ctrl_close(int s)
{
    struct sockaddr_un lo; socklen_t l = sizeof lo;
    if (s < 0) return;
    if (!getsockname(s, (struct sockaddr *)&lo, &l) && lo.sun_path[0]) unlink(lo.sun_path);
    close(s);
}
/* una orden y su respuesta (ignora los avisos "<N>..."); longitud o -1 */
static int wl_req_s(int s, const char *cmd, char *out, int cap, int timeout_ms)
{
    if (send(s, cmd, strlen(cmd), 0) < 0) return -1;
    double t0 = now_ms();
    for (;;) {
        int left = timeout_ms - (int)(now_ms() - t0); if (left <= 0) return -1;
        struct pollfd pf = { s, POLLIN, 0 };
        if (poll(&pf, 1, left) <= 0) return -1;
        ssize_t n = recv(s, out, (size_t)cap - 1, 0);
        if (n <= 0) return -1;
        out[n] = 0;
        if (out[0] != '<') return (int)n;
    }
}
static int wl_req(const char *cmd, char *out, int cap)
{
    int s = wl_ctrl_open(); if (s < 0) { out[0] = 0; return -1; }
    int n = wl_req_s(s, cmd, out, cap, strncmp(cmd, "SAVE_CONFIG", 11) ? 1500 : 4000);
    wl_ctrl_close(s);
    if (n < 0) out[0] = 0;
    return n;
}
static int wl_ok(const char *cmd) { char r[64]; return wl_req(cmd, r, sizeof r) > 0 && !strncmp(r, "OK", 2); }

static void wl_ipinfo(char *mask, char *gw, char *dns)
{
    mask[0] = gw[0] = dns[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s >= 0) {
        struct ifreq ifr; memset(&ifr, 0, sizeof ifr); wlp_cpy(ifr.ifr_name, sizeof ifr.ifr_name, WL_IF);
        if (!ioctl(s, SIOCGIFNETMASK, &ifr)) wlp_cpy(mask, 20, inet_ntoa(((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr));
        close(s);
    }
    char buf[2048]; FILE *f = fopen("/proc/net/route", "r");
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); wlp_route_gw(buf, WL_IF, gw, 20); }
    f = fopen("/etc/resolv.conf", "r");
    char l[128];
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "nameserver ", 11)) {
        l[strcspn(l, "\r\n")] = 0;
        if (dns[0]) { if (strlen(dns) + strlen(l + 11) + 3 < 48) { strcat(dns, ", "); strcat(dns, l + 11); } }
        else wlp_cpy(dns, 48, l + 11);
    }
    if (f) fclose(f);
}

/* lee todo el estado Wi-Fi (unos pocos ms: solo el socket de control) */
static void wl_wifi_refresh(void)
{
    static struct wl_state t;             /* solo la parte Wi-Fi; estatico (tamano) */
    static char buf[8192];
    double t0 = now_ms();
    memset(&t, 0, sizeof t); t.wid = -1;
    int s = wl_ctrl_open();
    if (s >= 0 && wl_req_s(s, "STATUS", buf, sizeof buf, 1500) > 0) {
        char v[80];
        t.wup = 1;
        wlp_kv(buf, "wpa_state", t.wstate, sizeof t.wstate);
        if (wlp_kv(buf, "ssid", v, sizeof v)) wlp_unescape(v, t.ssid, sizeof t.ssid);
        wlp_kv(buf, "bssid", t.bssid, sizeof t.bssid);
        wlp_kv(buf, "ip_address", t.ip, sizeof t.ip);
        wlp_kv(buf, "address", t.mac, sizeof t.mac);
        wlp_kv(buf, "key_mgmt", t.keymgmt, sizeof t.keymgmt);
        if (wlp_kv(buf, "id", v, sizeof v)) t.wid = atoi(v);
        if (wlp_kv(buf, "freq", v, sizeof v)) t.freq = atoi(v);
        if (!strcmp(t.wstate, "COMPLETED")) {
            if (wl_req_s(s, "SIGNAL_POLL", buf, sizeof buf, 1000) > 0) {
                if (wlp_kv(buf, "RSSI", v, sizeof v)) t.rssi = atoi(v);
                if (wlp_kv(buf, "LINKSPEED", v, sizeof v)) t.speed = atoi(v);
            }
        } else t.ssid[0] = 0;
        if (wl_req_s(s, "LIST_NETWORKS", buf, sizeof buf, 1500) > 0) {
            t.nsaved = wlp_list_networks(buf, t.saved, WL_MAXSAVED);
            for (int i = 0; i < t.nsaved; i++) {   /* el SSID exacto (bytes), no el texto escapado */
                char cmd[48], rep[160], ss[33];
                snprintf(cmd, sizeof cmd, "GET_NETWORK %d ssid", t.saved[i].id);
                if (wl_req_s(s, cmd, rep, sizeof rep, 1000) > 0 && wlp_ssid_value(rep, ss, sizeof ss) > 0) wlp_cpy(t.saved[i].ssid, sizeof t.saved[i].ssid, ss);
            }
        }
        if (wl_req_s(s, "SCAN_RESULTS", buf, sizeof buf, 1500) > 0) t.nnets = wlp_scan_results(buf, t.nets, WL_MAXNET);
        for (int i = 0; i < t.nnets; i++) {
            for (int k = 0; k < t.nsaved; k++) if (!strcmp(t.saved[k].ssid, t.nets[i].ssid)) t.nets[i].saved_id = t.saved[k].id;
            t.nets[i].current = t.ssid[0] && !strcmp(t.nets[i].ssid, t.ssid);
        }
        if (t.ip[0]) wl_ipinfo(t.mask, t.gw, t.dns);
    }
    wl_ctrl_close(s);
    pthread_mutex_lock(&wl_mu);
    WS.wup = t.wup; WS.wid = t.wid; WS.rssi = t.rssi; WS.speed = t.speed; WS.freq = t.freq;
    memcpy(WS.wstate, t.wstate, sizeof WS.wstate); memcpy(WS.ssid, t.ssid, sizeof WS.ssid);
    memcpy(WS.bssid, t.bssid, sizeof WS.bssid); memcpy(WS.ip, t.ip, sizeof WS.ip);
    memcpy(WS.mask, t.mask, sizeof WS.mask); memcpy(WS.gw, t.gw, sizeof WS.gw); memcpy(WS.dns, t.dns, sizeof WS.dns);
    memcpy(WS.mac, t.mac, sizeof WS.mac); memcpy(WS.keymgmt, t.keymgmt, sizeof WS.keymgmt);
    memcpy(WS.nets, t.nets, sizeof WS.nets); WS.nnets = t.nnets;
    memcpy(WS.saved, t.saved, sizeof WS.saved); WS.nsaved = t.nsaved;
    WS.gen++;
    pthread_mutex_unlock(&wl_mu);
    lg("wifi: estado en %.0f ms (%s, %d redes, %d guardadas)", now_ms() - t0, t.wup ? t.wstate : "sin wpa", t.nnets, t.nsaved);
}

/* ajustes del reproductor (bloqueante: solo en los hilos) */
static int wl_hs_set(const char *name, int v)
{
    pthread_mutex_lock(&wl_hs_mu);
    int rc = hs_set(name, v);
    pthread_mutex_unlock(&wl_hs_mu);
    lg("wireless: hs_set %s=%d -> %d", name, v, rc);
    return rc;
}
static void wl_hs_read(void)
{
    struct hs_kv kv[32];
    pthread_mutex_lock(&wl_hs_mu);
    int n = hs_list(kv, 32);
    pthread_mutex_unlock(&wl_hs_mu);
    pthread_mutex_lock(&wl_mu);
    WS.hs_ok = n > 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(kv[i].name, "wifi")) WS.wset = kv[i].val;
        else if (!strcmp(kv[i].name, "bt")) WS.bset = kv[i].val;
        else if (!strcmp(kv[i].name, "airplay")) WS.airplay = kv[i].val;
        else if (!strcmp(kv[i].name, "dlna")) WS.dlna = kv[i].val;
        else if (!strcmp(kv[i].name, "hibylink")) WS.hibylink = kv[i].val;
    }
    WS.gen++;
    pthread_mutex_unlock(&wl_mu);
    if (n <= 0) lg("wireless: hs_list -> %d (sin libreria v7: los interruptores no funcionaran)", n);
}

/* unirse a una red: sec = WSEC_*, hidden = scan_ssid. psk vacio = red guardada / abierta */
static void wl_join(const char *ssid, const char *psk, int sec, int hidden)
{
    char cmd[256], rep[256], hex[72];
    int id = -1, added = 0, res = WJ_FAIL;
    double t0 = now_ms();
    pthread_mutex_lock(&wl_mu);
    wlp_cpy(WS.joining, sizeof WS.joining, ssid);
    for (int k = 0; k < WS.nsaved; k++) if (!strcmp(WS.saved[k].ssid, ssid)) id = WS.saved[k].id;
    WS.gen++;
    pthread_mutex_unlock(&wl_mu);
    if (id < 0 || psk[0]) {
        if (id < 0) {
            if (wl_req("ADD_NETWORK", rep, sizeof rep) <= 0 || !isdigit((unsigned char)rep[0])) goto done;
            id = atoi(rep); added = 1;
            wlp_hex(ssid, hex, sizeof hex);
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, hex);
            if (!wl_ok(cmd)) goto done;
        }
        if (sec == WSEC_OPEN) { snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id); wl_ok(cmd); }
        else if (sec == WSEC_WEP) {
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id); wl_ok(cmd);
            int n = (int)strlen(psk), hexk = (n == 10 || n == 26); for (int i = 0; hexk && i < n; i++) if (wlp_hexv(psk[i]) < 0) hexk = 0;
            snprintf(cmd, sizeof cmd, hexk ? "SET_NETWORK %d wep_key0 %s" : "SET_NETWORK %d wep_key0 \"%s\"", id, psk);
            if (!wl_ok(cmd)) goto done;
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d auth_alg OPEN SHARED", id); wl_ok(cmd);
        } else if (sec == WSEC_SAE) {
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt SAE", id); wl_ok(cmd);
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d sae_password \"%s\"", id, psk); wl_ok(cmd);
            snprintf(cmd, sizeof cmd, "SET_NETWORK %d ieee80211w 2", id); wl_ok(cmd);
        } else {
            snprintf(cmd, sizeof cmd, wlp_psk_ok(psk) == 2 ? "SET_NETWORK %d psk %s" : "SET_NETWORK %d psk \"%s\"", id, psk);
            if (!wl_ok(cmd)) goto done;
        }
        if (hidden) { snprintf(cmd, sizeof cmd, "SET_NETWORK %d scan_ssid 1", id); wl_ok(cmd); }
    }
    {
        /* avisos de wpa_supplicant mientras se conecta (contrasena incorrecta, conectado...) */
        int mon = wl_ctrl_open();
        if (mon >= 0 && (wl_req_s(mon, "ATTACH", rep, sizeof rep, 1000) <= 0 || strncmp(rep, "OK", 2))) { wl_ctrl_close(mon); mon = -1; }
        snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d", id); wl_ok(cmd);
        snprintf(cmd, sizeof cmd, "SELECT_NETWORK %d", id);
        if (!wl_ok(cmd)) { if (mon >= 0) wl_ctrl_close(mon); goto done; }
        int notfound = 0; double tc = now_ms(), tst = 0;
        while (now_ms() - tc < 25000 && res == WJ_FAIL) {
            if (mon >= 0) {
                struct pollfd pf = { mon, POLLIN, 0 };
                if (poll(&pf, 1, 250) > 0) {
                    char ev[512]; ssize_t n = recv(mon, ev, sizeof ev - 1, 0);
                    if (n > 0) {
                        ev[n] = 0; int eid, e = wlp_event(ev, &eid);
                        if (e == WE_CONNECTED) res = WJ_OK;
                        else if (e == WE_WRONG_KEY && (eid < 0 || eid == id)) { res = WJ_WRONGKEY; break; }
                        else if (e == WE_NOT_FOUND && ++notfound >= 4) break;
                    }
                }
            } else msleep(250);
            if (now_ms() - tst > 1000) {           /* por si se perdio el aviso */
                tst = now_ms();
                char st[1024], v[40];
                if (wl_req("STATUS", st, sizeof st) > 0 && wlp_kv(st, "wpa_state", v, sizeof v) && !strcmp(v, "COMPLETED") &&
                    wlp_kv(st, "id", v, sizeof v) && atoi(v) == id) res = WJ_OK;
            }
        }
        if (mon >= 0) { wl_req_s(mon, "DETACH", rep, sizeof rep, 500); wl_ctrl_close(mon); }
    }
done:
    if (res == WJ_OK) {
        wl_ok("ENABLE_NETWORK all");                /* SELECT_NETWORK desactiva las demas (como sys_server) */
        if (!wl_ok("SAVE_CONFIG")) lg("wifi: SAVE_CONFIG fallo (update_config=1 en /data/wpa_supplicant.conf?)");
        for (int i = 0; i < 24; i++) {             /* sys_server pide la IP (udhcpc): esperar un poco */
            wl_wifi_refresh();
            pthread_mutex_lock(&wl_mu); int ip = WS.ip[0] != 0; pthread_mutex_unlock(&wl_mu);
            if (ip) break;
            msleep(500);
        }
    } else {
        if (added && id >= 0) { snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id); wl_ok(cmd); }
        wl_ok("ENABLE_NETWORK all");                /* vuelve a la red de antes */
        wl_ok("REASSOCIATE");
    }
    lg("wifi: unirse a \"%s\" -> %d en %.0f ms", ssid, res, now_ms() - t0);
    pthread_mutex_lock(&wl_mu);
    WS.joining[0] = 0; WS.join_res = res; wlp_cpy(WS.join_ssid, sizeof WS.join_ssid, ssid); WS.join_seq++; WS.gen++;
    pthread_mutex_unlock(&wl_mu);
    wl_wifi_refresh();
}

static void *wl_wifi_thread(void *arg)
{
    (void)arg;
    double last = 0, last_scan = 0, scan_t = 0;
    for (;;) {
        struct wl_job j;
        int vis = wl_vis;
        if (wl_pop(&wl_qw, &j, vis || scan_t ? 500 : -1)) {
            switch (j.op) {
            case J_REFRESH: wl_wifi_refresh(); last = now_ms(); break;
            case J_HSREAD: wl_hs_read(); break;
            case J_HSSET:
                if (wl_hs_set(j.a, j.iv) < 0) wl_msg("No se pudo cambiar el ajuste", 0);
                wl_hs_read();
                break;
            case J_WTOGGLE: {
                pthread_mutex_lock(&wl_mu); WS.wpend = j.iv + 1; WS.gen++; pthread_mutex_unlock(&wl_mu);
                int rc = wl_hs_set("wifi", j.iv);
                if (rc < 0) wl_msg("No se pudo cambiar el ajuste", 0);
                else {
                    pthread_mutex_lock(&wl_mu); WS.wset = j.iv; pthread_mutex_unlock(&wl_mu);
                    for (double t0 = now_ms(); now_ms() - t0 < 12000; ) {   /* la radio sigue al ajuste */
                        msleep(600); wl_wifi_refresh();
                        pthread_mutex_lock(&wl_mu); int up = WS.wup; pthread_mutex_unlock(&wl_mu);
                        if (up == j.iv) break;
                    }
                    if (j.iv) { wl_ok("SCAN"); scan_t = now_ms(); }
                }
                pthread_mutex_lock(&wl_mu); WS.wpend = 0; WS.gen++; pthread_mutex_unlock(&wl_mu);
                last = now_ms();
                break;
            }
            case J_WSCAN: {
                char r[64];
                if (wl_req("SCAN", r, sizeof r) > 0) {      /* OK o FAIL-BUSY (ya buscando): da igual */
                    scan_t = now_ms(); last_scan = scan_t;
                    pthread_mutex_lock(&wl_mu); WS.scanning = 1; WS.gen++; pthread_mutex_unlock(&wl_mu);
                }
                break;
            }
            case J_WJOIN: wl_join(j.a, j.b, j.iv & 255, j.iv >> 8); last = now_ms(); break;
            case J_WFORGET: {
                char cmd[40]; snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", j.iv);
                if (!wl_ok(cmd)) wl_msg("No se pudo olvidar la red", 0);
                else if (!wl_ok("SAVE_CONFIG")) lg("wifi: SAVE_CONFIG fallo");
                wl_wifi_refresh(); last = now_ms();
                break;
            }
            }
            memset(&j, 0, sizeof j);             /* no dejar la contrasena en la pila */
        }
        double t = now_ms();
        if (scan_t && t - scan_t > 3500) {          /* resultados de la busqueda */
            scan_t = 0; wl_wifi_refresh(); last = t;
            pthread_mutex_lock(&wl_mu); WS.scanning = 0; WS.gen++; pthread_mutex_unlock(&wl_mu);
        }
        vis = wl_vis;
        if (vis && t - last > (vis == 2 ? 3000 : 6000)) { wl_wifi_refresh(); last = now_ms(); }
        if (vis == 2 && !scan_t && t - last_scan > 15000) {        /* como iOS: busca mientras se ve */
            pthread_mutex_lock(&wl_mu); int up = WS.wup; pthread_mutex_unlock(&wl_mu);
            if (up) wl_push(&wl_qw, J_WSCAN, 0, 0, 0);
            last_scan = t;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ Bluetooth (bluez por D-Bus) */
static char wl_dbus_out[96 * 1024];
static int wl_bt_call(char *out, int cap, int timeout_ms, const char *path, const char *method, const char *arg1, const char *arg2, const char *arg3)
{
    char to[32]; snprintf(to, sizeof to, "--reply-timeout=%d", timeout_ms);
    char *argv[12]; int n = 0;
    argv[n++] = (char *)WL_DBUS; argv[n++] = "--system"; argv[n++] = "--print-reply"; argv[n++] = to;
    argv[n++] = "--dest=org.bluez"; argv[n++] = (char *)path; argv[n++] = (char *)method;
    if (arg1) argv[n++] = (char *)arg1;
    if (arg2) argv[n++] = (char *)arg2;
    if (arg3) argv[n++] = (char *)arg3;
    argv[n] = 0;
    int rc = wl_run(argv, out, cap, timeout_ms + 2000);
    char en[96];
    if (rc != 0 || wlp_dbus_error(out, en, sizeof en)) { lg("bt: %s %s -> %d %s", method, path, rc, out[0] ? strtok(out, "\n") : ""); return -1; }
    return 0;
}
static void wl_bt_refresh(void)
{
    static struct wl_dev dv[WL_MAXDEV];
    struct wl_adapter ad; int n = 0;
    double t0 = now_ms();
    int rc = wl_bt_call(wl_dbus_out, sizeof wl_dbus_out, 3000, "/", "org.freedesktop.DBus.ObjectManager.GetManagedObjects", 0, 0, 0);
    if (!rc) n = wlp_bluez_objects(wl_dbus_out, &ad, dv, WL_MAXDEV); else memset(&ad, 0, sizeof ad);
    pthread_mutex_lock(&wl_mu);
    WS.bavail = ad.found; WS.ad = ad;
    memcpy(WS.devs, dv, sizeof WS.devs); WS.ndevs = n;
    WS.gen++;
    pthread_mutex_unlock(&wl_mu);
    lg("bt: estado en %.0f ms (%s, %d dispositivos)", now_ms() - t0, ad.found ? (ad.powered ? "encendido" : "apagado") : "sin bluez", n);
}

/* busqueda: un bluetoothctl con "scan on" mientras haga falta */
static pid_t wl_disc_pid; static int wl_disc_fd = -1;
static void wl_disc_start(void)
{
    if (wl_disc_pid > 0 || access(WL_BTCTL, X_OK)) return;
    int fd[2]; if (pipe(fd)) return;
    int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[0], 0);
    if (nul >= 0) { posix_spawn_file_actions_adddup2(&fa, nul, 1); posix_spawn_file_actions_adddup2(&fa, nul, 2); }
    posix_spawn_file_actions_addclose(&fa, fd[1]);
    char *argv[] = { (char *)WL_BTCTL, 0 };
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[0]); if (nul >= 0) close(nul);
    if (r) { close(fd[1]); return; }
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    wl_disc_pid = pid; wl_disc_fd = fd[1];
    /* su propio agente fuera: el de HiBy (bt-agent) sigue siendo el que empareja */
    static const char c[] = "agent off\nscan on\n";
    if (write(wl_disc_fd, c, sizeof c - 1) < 0) lg("bt: bluetoothctl: %s", strerror(errno));
    lg("bt: busqueda iniciada (bluetoothctl %d)", (int)pid);
}
static void wl_disc_stop(void)
{
    if (wl_disc_pid <= 0) return;
    static const char c[] = "scan off\nquit\n";
    if (write(wl_disc_fd, c, sizeof c - 1) < 0) {}
    close(wl_disc_fd); wl_disc_fd = -1;
    int stt; double t0 = now_ms();
    while (waitpid(wl_disc_pid, &stt, WNOHANG) == 0) {
        if (now_ms() - t0 > 1500) { kill(wl_disc_pid, SIGKILL); waitpid(wl_disc_pid, &stt, 0); break; }
        msleep(50);
    }
    lg("bt: busqueda parada");
    wl_disc_pid = 0;
}
static void wl_bt_lastused(const char *addr)
{
    /* el mismo archivo que sys_server lee al arrancar ("%17s") para reconectar */
    FILE *f = fopen("/data/bt_lastused.txt", "w");
    if (f) { fprintf(f, "%s", addr); fclose(f); }
}
static void wl_dev_path(const char *addr, char *path, int cap)
{
    char a[18]; wlp_cpy(a, sizeof a, addr);
    for (char *p = a; *p; p++) if (*p == ':') *p = '_';
    snprintf(path, (size_t)cap, "/org/bluez/hci0/dev_%s", a);
}
static void wl_bt_busy(const char *addr) { pthread_mutex_lock(&wl_mu); wlp_cpy(WS.bbusy, sizeof WS.bbusy, addr ? addr : ""); WS.gen++; pthread_mutex_unlock(&wl_mu); }

static void *wl_bt_thread(void *arg)
{
    (void)arg;
    double last = 0;
    for (;;) {
        struct wl_job j;
        int vis = wl_vis;
        if (wl_pop(&wl_qb, &j, vis ? 500 : -1)) {
            char path[64], out[512];
            switch (j.op) {
            case J_REFRESH: wl_bt_refresh(); last = now_ms(); break;
            case J_BTOGGLE: {
                pthread_mutex_lock(&wl_mu); WS.bpend = j.iv + 1; WS.gen++; pthread_mutex_unlock(&wl_mu);
                if (!j.iv) wl_disc_stop();
                if (wl_hs_set("bt", j.iv) < 0) wl_msg("No se pudo cambiar el ajuste", 0);
                else {
                    pthread_mutex_lock(&wl_mu); WS.bset = j.iv; pthread_mutex_unlock(&wl_mu);
                    for (double t0 = now_ms(); now_ms() - t0 < 15000; ) {     /* bt_resume tarda ~8 s */
                        msleep(700); wl_bt_refresh();
                        pthread_mutex_lock(&wl_mu); int p = WS.ad.powered; pthread_mutex_unlock(&wl_mu);
                        if (p == j.iv) break;
                    }
                    if (j.iv) wl_disc_until = now_ms() + 45000;
                }
                pthread_mutex_lock(&wl_mu); WS.bpend = 0; WS.gen++; pthread_mutex_unlock(&wl_mu);
                last = now_ms();
                break;
            }
            case J_BCONNECT: {                     /* a = direccion, b = nombre, iv = ya emparejado */
                int was_disc = wl_disc_pid > 0;
                wl_disc_stop();                      /* buscar mientras se conecta lo hace fallar */
                wl_bt_busy(j.a);
                wl_dev_path(j.a, path, sizeof path);
                int ok = 1;
                if (!j.iv) {
                    if (wl_bt_call(out, sizeof out, 40000, path, "org.bluez.Device1.Pair", 0, 0, 0)) {
                        char en[96]; wlp_dbus_error(out, en, sizeof en);
                        if (!strstr(en, "AlreadyExists")) ok = 0;
                    }
                    if (ok) wl_bt_call(out, sizeof out, 3000, path, "org.freedesktop.DBus.Properties.Set",
                                       "string:org.bluez.Device1", "string:Trusted", "variant:boolean:true");
                }
                if (ok && wl_bt_call(out, sizeof out, 30000, path, "org.bluez.Device1.Connect", 0, 0, 0)) ok = 0;
                if (ok) wl_bt_lastused(j.a);
                else wl_msg(j.iv ? "No se pudo conectar a «%s»" : "No se pudo emparejar con «%s»", j.b);
                wl_bt_refresh();
                wl_bt_busy(0);
                if (was_disc && wl_vis == 3) wl_disc_until = now_ms() + 20000;
                last = now_ms();
                break;
            }
            case J_BDISCONNECT:
                wl_bt_busy(j.a); wl_dev_path(j.a, path, sizeof path);
                if (wl_bt_call(out, sizeof out, 10000, path, "org.bluez.Device1.Disconnect", 0, 0, 0)) wl_msg("No se pudo desconectar", 0);
                wl_bt_refresh(); wl_bt_busy(0); last = now_ms();
                break;
            case J_BFORGET: {
                wl_bt_busy(j.a); wl_dev_path(j.a, path, sizeof path);
                char op[80]; snprintf(op, sizeof op, "objpath:%s", path);
                if (wl_bt_call(out, sizeof out, 10000, "/org/bluez/hci0", "org.bluez.Adapter1.RemoveDevice", op, 0, 0)) wl_msg("No se pudo olvidar el dispositivo", 0);
                wl_bt_refresh(); wl_bt_busy(0); last = now_ms();
                break;
            }
            }
        }
        /* busqueda solo con la pagina de Bluetooth a la vista (y un tiempo maximo) */
        vis = wl_vis;
        pthread_mutex_lock(&wl_mu); int pw = WS.ad.powered, busy = WS.bbusy[0] != 0, pend = WS.bpend; pthread_mutex_unlock(&wl_mu);
        int want = vis == 3 && pw && !busy && !pend && now_ms() < wl_disc_until;
        if (want && wl_disc_pid <= 0) { wl_disc_start(); wl_touch(); }
        else if (!want && wl_disc_pid > 0) { wl_disc_stop(); wl_touch(); }
        if (wl_disc_pid > 0) { int stt; if (waitpid(wl_disc_pid, &stt, WNOHANG) == wl_disc_pid) { wl_disc_pid = 0; close(wl_disc_fd); wl_disc_fd = -1; } }
        double t = now_ms();
        if (vis && t - last > (vis == 3 ? (wl_disc_pid > 0 ? 2000 : 4000) : 8000)) { wl_bt_refresh(); last = now_ms(); }
    }
    return 0;
}

static void wireless_preload(void)
{
    if (wl_started) return;
    wl_started = 1;
    WS.wset = WS.bset = -1;
    pthread_t a, b;
    if (!pthread_create(&a, 0, wl_wifi_thread, 0)) pthread_detach(a);
    if (!pthread_create(&b, 0, wl_bt_thread, 0)) pthread_detach(b);
    wl_push(&wl_qw, J_HSREAD, 0, 0, 0);
    wl_push(&wl_qw, J_REFRESH, 0, 0, 0);
    wl_push(&wl_qb, J_REFRESH, 0, 0, 0);
}
/* foto del estado para la interfaz; 1 si cambio */
static int wl_snap(void)
{
    if (WS.gen == wv.gen) return 0;
    pthread_mutex_lock(&wl_mu); wv = WS; pthread_mutex_unlock(&wl_mu);
    return 1;
}

/* ------------------------------------------------------------------ dibujo */
#define WL_NAVH 64
#define WL_TOP (WL_NAVH + 30)
#define WL_ROWH 44
#define WC_CARD rgb(28, 28, 30)
#define WC_SEL rgb(58, 58, 60)
#define WC_SEP rgb(56, 56, 58)
#define WC_GRAY rgb(142, 142, 147)
#define WC_BLUE rgb(10, 132, 255)
#define WC_GREEN rgb(52, 199, 89)
#define WC_RED rgb(255, 69, 58)
static inline void wl_px(uint16_t *d, int x, int y, uint16_t c, float cov)
{
    if (x < 0 || x >= W || y < 0 || y >= H || cov <= 0) return;
    if (cov >= 1) { d[y * W + x] = c; return; }
    d[y * W + x] = mix565(c, d[y * W + x], (unsigned)(cov * 255));
}
/* tarjeta con esquinas redondeadas (borde suavizado, interior a velocidad de memoria) */
static void wl_card(uint16_t *d, int x, int y, int w, int h, float r, uint16_t c, int ytop)
{
    if (r > h / 2.f) r = h / 2.f;
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < ytop || yy >= H || yy < 0) continue;
        float dy = j + .5f < r ? r - (j + .5f) : (j + .5f > h - r ? (j + .5f) - (h - r) : 0);
        float edge = dy > 0 ? r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0) : 0;
        int full = (int)ceilf(edge);
        uint16_t *row = d + yy * W;
        for (int i = full; i < w - full; i++) { int xx = x + i; if (xx >= 0 && xx < W) row[xx] = c; }
        if (edge > 0 && full > 0) {
            float cov = full - edge;
            wl_px(d, x + full - 1, yy, c, cov); wl_px(d, x + w - full, yy, c, cov);
        }
    }
}
/* linea gruesa con extremos redondos (suavizada) */
static void wl_line(uint16_t *d, float x0, float y0, float x1, float y1, float w, uint16_t c, int ytop)
{
    float hw = w / 2, minx = fminf(x0, x1) - hw - 1, maxx = fmaxf(x0, x1) + hw + 1, miny = fminf(y0, y1) - hw - 1, maxy = fmaxf(y0, y1) + hw + 1;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++) {
        if (y < ytop) continue;
        for (int x = (int)minx; x <= (int)maxx; x++) {
            float px = x + .5f - x0, py = y + .5f - y0, t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
            if (t < 0) t = 0; if (t > 1) t = 1;
            float ex = px - t * dx, ey = py - t * dy, dist = sqrtf(ex * ex + ey * ey);
            float cov = hw + .5f - dist; if (cov > 1) cov = 1;
            wl_px(d, x, y, c, cov);
        }
    }
}
/* arco (anillo) entre angulos a0..a1 (radianes, 0 = arriba, positivo a la derecha) */
static void wl_arc(uint16_t *d, float cx, float cy, float r, float w, float a0, float a1, uint16_t c, int ytop)
{
    float ro = r + w / 2 + 1;
    for (int y = (int)(cy - ro); y <= (int)(cy + ro); y++) {
        if (y < ytop) continue;
        for (int x = (int)(cx - ro); x <= (int)(cx + ro); x++) {
            float px = x + .5f - cx, py = y + .5f - cy, dist = sqrtf(px * px + py * py);
            float cov = w / 2 + .5f - fabsf(dist - r); if (cov <= 0) continue; if (cov > 1) cov = 1;
            float a = atan2f(px, -py);
            float ca = fminf(a - a0, a1 - a) * dist + .5f; if (ca <= 0) continue; if (ca < 1) cov *= ca;
            wl_px(d, x, y, c, cov);
        }
    }
}
/* simbolo de Wi-Fi: lvl 0..3 arcos encendidos (-1: todos, para iconos); s = escala */
static void wl_wifi_glyph(uint16_t *d, float cx, float by, float s, int lvl, uint16_t on, uint16_t off, int ytop)
{
    fill_circle(d, cx, by - 1.6f * s, 1.7f * s, lvl >= 0 && lvl < 1 ? off : on, 255);
    for (int i = 0; i < 3; i++) wl_arc(d, cx, by - 1.2f * s, (4.4f + 4.0f * i) * s, 2.0f * s, -0.78f, 0.78f, lvl < 0 || i < lvl ? on : off, ytop);
}
static void wl_lock(uint16_t *d, float x, float y, uint16_t c, int ytop)
{
    wl_arc(d, x + 4.5f, y + 4.5f, 2.8f, 1.5f, -1.58f, 1.58f, c, ytop);
    wl_line(d, x + 1.7f, y + 4.5f, x + 1.7f, y + 6, 1.5f, c, ytop);
    wl_line(d, x + 7.3f, y + 4.5f, x + 7.3f, y + 6, 1.5f, c, ytop);
    wl_card(d, (int)x, (int)y + 6, 9, 7, 1.5f, c, ytop);
}
static void wl_check(uint16_t *d, float x, float y, uint16_t c, int ytop)
{
    wl_line(d, x, y + 6, x + 4, y + 10, 2.2f, c, ytop);
    wl_line(d, x + 4, y + 10, x + 11, y, 2.2f, c, ytop);
}
static void wl_chev(uint16_t *d, float x, float cy, uint16_t c, int ytop)
{
    wl_line(d, x, cy - 5.5f, x + 5.5f, cy, 2.0f, c, ytop);
    wl_line(d, x + 5.5f, cy, x, cy + 5.5f, 2.0f, c, ytop);
}
static void wl_infobtn(uint16_t *d, float cx, float cy, int ytop)
{
    wl_arc(d, cx, cy, 9.5f, 1.5f, -3.15f, 3.15f, WC_BLUE, ytop);
    if (cy - 6 >= ytop) fill_circle(d, cx, cy - 4.8f, 1.3f, WC_BLUE, 255);
    wl_line(d, cx, cy - 1.5f, cx, cy + 4.8f, 2.0f, WC_BLUE, ytop);
}
/* indicador de actividad de iOS: 12 radios que se apagan en circulo (avanza a 12 pasos/s) */
static void wl_spinner(uint16_t *d, float cx, float cy, float r, double t, int nvis, int ytop)
{
    int step = (int)(t / 83.0) % 12;
    for (int i = 0; i < 12 && i < nvis; i++) {
        float a = i * (float)M_PI / 6, sx = sinf(a), sy = -cosf(a);
        int age = (step - i + 12) % 12;
        int k = 255 - age * 17; if (k < 70) k = 70;
        uint16_t c = rgb(k * 200 / 255, k * 200 / 255, k * 205 / 255);
        wl_line(d, cx + sx * r * .48f, cy + sy * r * .48f, cx + sx * r, cy + sy * r, r * .2f, c, ytop);
    }
}
static void wl_switch(uint16_t *d, int x, int y, float p, int ytop)
{
    if (y < ytop) return;
    uint16_t off = rgb(57, 57, 61);
    uint16_t c = mix565(WC_GREEN, off, (unsigned)(p * 255));
    fill_rrect(d, x, y, 51, 31, 15.5f, c, 255);
    fill_circle(d, x + 15.5f + 20 * p, y + 15.5f, 13.5f, 0xFFFF, 255);
}
/* iconos de la raiz (cuadrado de color con simbolo blanco, como Ajustes de iOS) */
enum { WI_NONE, WI_WIFI, WI_BT, WI_AIRPLAY, WI_DLNA, WI_LINK };
static void wl_icon(uint16_t *d, int x, int y, int ic, int ytop)
{
    static const int col[][3] = { {0, 0, 0}, {10, 132, 255}, {10, 132, 255}, {255, 55, 95}, {48, 176, 199}, {175, 82, 222} };
    wl_card(d, x, y, 29, 29, 7, rgb(col[ic][0], col[ic][1], col[ic][2]), ytop);
    float cx = x + 14.5f, cy = y + 14.5f;
    switch (ic) {
    case WI_WIFI: wl_wifi_glyph(d, cx, cy + 7.5f, 0.95f, -1, 0xFFFF, 0xFFFF, ytop); break;
    case WI_BT: {
        float p[][2] = { {-5, -5}, {5, 5}, {0, 10}, {0, -10}, {5, -5}, {-5, 5} };
        for (int i = 0; i < 5; i++) wl_line(d, cx + p[i][0] * .8f, cy + p[i][1] * .8f, cx + p[i + 1][0] * .8f, cy + p[i + 1][1] * .8f, 1.8f, 0xFFFF, ytop);
        break;
    }
    case WI_AIRPLAY:
        wl_line(d, cx - 7, cy - 6, cx + 7, cy - 6, 1.8f, 0xFFFF, ytop); wl_line(d, cx + 7, cy - 6, cx + 7, cy + 3, 1.8f, 0xFFFF, ytop);
        wl_line(d, cx - 7, cy - 6, cx - 7, cy + 3, 1.8f, 0xFFFF, ytop);
        wl_line(d, cx - 7, cy + 3, cx - 4, cy + 3, 1.8f, 0xFFFF, ytop); wl_line(d, cx + 7, cy + 3, cx + 4, cy + 3, 1.8f, 0xFFFF, ytop);
        for (int i = 0; i < 6; i++) wl_line(d, cx - 5 + i, cy + 7, cx, cy + 1 + i * 0.2f, 1.2f, 0xFFFF, ytop);
        wl_line(d, cx - 5, cy + 7.5f, cx + 5, cy + 7.5f, 1.4f, 0xFFFF, ytop); wl_line(d, cx + 5, cy + 7.5f, cx, cy + 1.5f, 1.4f, 0xFFFF, ytop);
        break;
    case WI_DLNA:
        wl_line(d, cx - 6, cy + 5, cx, cy - 5, 1.6f, 0xFFFF, ytop); wl_line(d, cx + 6, cy + 5, cx, cy - 5, 1.6f, 0xFFFF, ytop);
        wl_line(d, cx - 6, cy + 5, cx + 6, cy + 5, 1.6f, 0xFFFF, ytop);
        fill_circle(d, cx, cy - 5, 3, 0xFFFF, 255); fill_circle(d, cx - 6, cy + 5, 3, 0xFFFF, 255); fill_circle(d, cx + 6, cy + 5, 3, 0xFFFF, 255);
        break;
    case WI_LINK:
        wl_arc(d, cx - 3.5f, cy, 5, 1.8f, -3.15f, 3.15f, 0xFFFF, ytop); wl_arc(d, cx + 3.5f, cy, 5, 1.8f, -3.15f, 3.15f, 0xFFFF, ytop);
        break;
    }
}

/* ------------------------------------------------------------------ paginas (modelo) */
enum { WR_NAV, WR_TOGGLE, WR_NET, WR_DEV, WR_ACTION, WR_INFO, WR_TEXT };
enum { WF_LOCK = 1, WF_CHECK = 2, WF_INFOBTN = 4, WF_RED = 8, WF_BUSY = 16, WF_DIM = 32 };
enum { RID_NONE, RID_WIFI_TOG, RID_BT_TOG, RID_NAV_WIFI, RID_NAV_BT, RID_AIRPLAY, RID_DLNA, RID_LINK, RID_OTHER,
       RID_NAV_SAVED, RID_FORGET, RID_JOIN, RID_CONNECT, RID_DISCONNECT, RID_SAVED0 = 100, RID_NET0 = 1000, RID_DEV0 = 2000 };
enum { P_ROOT, P_WIFI, P_BT, P_NET, P_DEV, P_SAVED };
struct wrow { int kind, id, on, flags, lvl, icon; char t[72], v[64]; };
struct wsec { const char *hdr; int spin; char foot[200]; int first, n; };
struct wpage { int kind; char title[72]; const char *back; struct wsec s[6]; int ns; struct wrow r[96]; int nr; char param[72]; };

static struct wsec *wp_sec(struct wpage *pg, const char *hdr)
{
    if (pg->ns >= 6) return &pg->s[5];
    struct wsec *s = &pg->s[pg->ns++]; memset(s, 0, sizeof *s);
    s->hdr = hdr; s->first = pg->nr;
    return s;
}
static struct wrow *wp_row(struct wpage *pg, int kind, int id, const char *t)
{
    static struct wrow dummy;
    if (pg->nr >= 96 || !pg->ns) return &dummy;
    struct wrow *r = &pg->r[pg->nr++]; memset(r, 0, sizeof *r);
    r->kind = kind; r->id = id; wlp_display(t ? t : "", r->t, sizeof r->t);
    pg->s[pg->ns - 1].n++;
    return r;
}
static void wp_foot(struct wpage *pg, const char *txt) { if (pg->ns) wlp_cpy(pg->s[pg->ns - 1].foot, sizeof pg->s[0].foot, txt); }

static const char *wl_secname(int sec)
{
    return sec == WSEC_OPEN ? tr("Ninguna") : sec == WSEC_WEP ? "WEP" : sec == WSEC_SAE ? "WPA3" : sec == WSEC_EAP ? "WPA2 Enterprise" : "WPA/WPA2";
}
static int wl_net_by_ssid(const char *ssid) { for (int i = 0; i < wv.nnets; i++) if (!strcmp(wv.nets[i].ssid, ssid)) return i; return -1; }
static int wl_saved_by_ssid(const char *ssid) { for (int i = 0; i < wv.nsaved; i++) if (!strcmp(wv.saved[i].ssid, ssid)) return i; return -1; }
static int wl_dev_by_addr(const char *a) { for (int i = 0; i < wv.ndevs; i++) if (!strcmp(wv.devs[i].addr, a)) return i; return -1; }
/* Wi-Fi "encendido" para la interfaz: el cambio pendiente manda; luego el ajuste; si no, wpa */
static int wl_wifi_on(void) { return wv.wpend ? wv.wpend - 1 : wv.wset >= 0 ? wv.wset : wv.wup; }
static int wl_bt_on(void) { return wv.bpend ? wv.bpend - 1 : wv.ad.found ? wv.ad.powered : wv.bset > 0; }
static int wl_connected(void) { return wv.wup && !strcmp(wv.wstate, "COMPLETED") && wv.ssid[0]; }

static void wl_net_row(struct wpage *pg, const struct wl_net *n, int id)
{
    struct wrow *r = wp_row(pg, WR_NET, id, n->ssid);
    r->lvl = wlp_bars(n->level);
    if (n->sec != WSEC_OPEN) r->flags |= WF_LOCK;
    if (n->current) r->flags |= WF_CHECK | WF_INFOBTN;
    else if (n->saved_id >= 0) r->flags |= WF_INFOBTN;
    if (wv.joining[0] && !strcmp(wv.joining, n->ssid)) r->flags = (r->flags & ~WF_CHECK) | WF_BUSY;
}
static void wl_build(struct wpage *pg, int kind, const char *param)
{
    pg->kind = kind; pg->ns = pg->nr = 0;
    if (param) wlp_cpy(pg->param, sizeof pg->param, param);
    char b[200];
    if (kind == P_ROOT) {
        wlp_cpy(pg->title, sizeof pg->title, tr("Inalámbrico"));
        wp_sec(pg, 0);
        struct wrow *r = wp_row(pg, WR_NAV, RID_NAV_WIFI, "Wi-Fi"); r->icon = WI_WIFI;
        if (!wl_wifi_on()) wlp_cpy(r->v, sizeof r->v, tr("Desactivado"));
        else if (wl_connected()) wlp_display(wv.ssid, r->v, sizeof r->v);
        else wlp_cpy(r->v, sizeof r->v, tr("No conectado"));
        r = wp_row(pg, WR_NAV, RID_NAV_BT, "Bluetooth"); r->icon = WI_BT;
        int con = -1; for (int i = 0; i < wv.ndevs; i++) if (wv.devs[i].connected && wv.devs[i].paired) con = i;
        if (!wl_bt_on()) wlp_cpy(r->v, sizeof r->v, tr("Desactivado"));
        else if (con >= 0) wlp_display(wv.devs[con].name[0] ? wv.devs[con].name : wv.devs[con].addr, r->v, sizeof r->v);
        else wlp_cpy(r->v, sizeof r->v, tr("Activado"));
        if (wv.hs_ok) {
            wp_sec(pg, "TRANSMISIÓN");
            r = wp_row(pg, WR_TOGGLE, RID_AIRPLAY, "AirPlay"); r->icon = WI_AIRPLAY; r->on = wv.airplay;
            r = wp_row(pg, WR_TOGGLE, RID_DLNA, "DLNA"); r->icon = WI_DLNA; r->on = wv.dlna;
            if (!wl_wifi_on()) { pg->r[pg->nr - 1].flags |= WF_DIM; pg->r[pg->nr - 2].flags |= WF_DIM; }
            wp_foot(pg, tr("WL_STREAM_HELP"));
            wp_sec(pg, 0);
            r = wp_row(pg, WR_TOGGLE, RID_LINK, "HiBy Link"); r->icon = WI_LINK; r->on = wv.hibylink;
            wp_foot(pg, tr("WL_LINK_HELP"));
        }
    } else if (kind == P_WIFI) {
        wlp_cpy(pg->title, sizeof pg->title, "Wi-Fi");
        int on = wl_wifi_on();
        wp_sec(pg, 0);
        struct wrow *r = wp_row(pg, WR_TOGGLE, RID_WIFI_TOG, "Wi-Fi"); r->on = on; if (wv.wpend) r->flags |= WF_BUSY;
        if (on && wl_connected()) {
            int i = wl_net_by_ssid(wv.ssid);
            struct wl_net cur; memset(&cur, 0, sizeof cur);
            if (i >= 0) cur = wv.nets[i]; else { wlp_cpy(cur.ssid, sizeof cur.ssid, wv.ssid); cur.level = wv.rssi; cur.sec = strstr(wv.keymgmt, "NONE") ? WSEC_OPEN : WSEC_PSK; }
            if (wv.rssi) cur.level = wv.rssi;
            cur.current = 1;
            wl_net_row(pg, &cur, RID_NET0 - 1);
        }
        if (!on) { wp_foot(pg, tr("WL_WIFI_OFF_HELP")); return; }
        if (!wv.wup) { wp_foot(pg, tr("Activando Wi-Fi…")); return; }
        /* mis redes (guardadas y al alcance) y otras redes */
        int nmine = 0;
        for (int i = 0; i < wv.nnets; i++) if (wv.nets[i].saved_id >= 0 && !wv.nets[i].current) nmine++;
        if (nmine) {
            wp_sec(pg, "MIS REDES");
            for (int i = 0; i < wv.nnets; i++) if (wv.nets[i].saved_id >= 0 && !wv.nets[i].current) wl_net_row(pg, &wv.nets[i], RID_NET0 + i);
        }
        struct wsec *s = wp_sec(pg, nmine ? "OTRAS REDES" : "REDES"); s->spin = wv.scanning || !wv.nnets;
        for (int i = 0; i < wv.nnets; i++) if (wv.nets[i].saved_id < 0 && !wv.nets[i].current) wl_net_row(pg, &wv.nets[i], RID_NET0 + i);
        wp_row(pg, WR_ACTION, RID_OTHER, tr("Otra…"));
        if (wv.nsaved) {
            wp_sec(pg, 0);
            r = wp_row(pg, WR_NAV, RID_NAV_SAVED, tr("Redes guardadas")); snprintf(r->v, sizeof r->v, "%d", wv.nsaved);
        }
        wp_foot(pg, tr("WL_WIFI_HELP"));
    } else if (kind == P_NET) {
        const char *ssid = pg->param;
        wlp_display(ssid, pg->title, sizeof pg->title);
        int cur = wl_connected() && !strcmp(wv.ssid, ssid), si = wl_saved_by_ssid(ssid), ni = wl_net_by_ssid(ssid);
        wp_sec(pg, 0);
        if (!cur && ni >= 0) wp_row(pg, WR_ACTION, RID_JOIN, tr("Conectarse a esta red"));
        if (si >= 0) wp_row(pg, WR_ACTION, RID_FORGET, tr("Olvidar esta red"));
        if (!pg->s[pg->ns - 1].n) pg->ns--;
        if (cur) {
            wp_sec(pg, "IPV4");
            struct wrow *r = wp_row(pg, WR_INFO, 0, tr("Dirección IP")); wlp_cpy(r->v, sizeof r->v, wv.ip[0] ? wv.ip : tr("Obteniendo…"));
            if (wv.mask[0]) { r = wp_row(pg, WR_INFO, 0, tr("Máscara de subred")); wlp_cpy(r->v, sizeof r->v, wv.mask); }
            if (wv.gw[0]) { r = wp_row(pg, WR_INFO, 0, tr("Router")); wlp_cpy(r->v, sizeof r->v, wv.gw); }
            if (wv.dns[0]) { r = wp_row(pg, WR_INFO, 0, "DNS"); wlp_cpy(r->v, sizeof r->v, wv.dns); }
            wp_sec(pg, "CONEXIÓN");
            r = wp_row(pg, WR_INFO, 0, tr("Señal")); snprintf(r->v, sizeof r->v, "%d dBm", wv.rssi);
            if (wv.speed) { r = wp_row(pg, WR_INFO, 0, tr("Velocidad")); snprintf(r->v, sizeof r->v, "%d Mb/s", wv.speed); }
            if (wv.freq) { r = wp_row(pg, WR_INFO, 0, tr("Frecuencia")); snprintf(r->v, sizeof r->v, "%.1f GHz", wv.freq / 1000.0); }
            r = wp_row(pg, WR_INFO, 0, tr("Seguridad")); wlp_cpy(r->v, sizeof r->v, wv.keymgmt[0] ? wv.keymgmt : "-");
            if (wv.mac[0]) { r = wp_row(pg, WR_INFO, 0, tr("Dirección Wi-Fi")); wlp_cpy(r->v, sizeof r->v, wv.mac); }
        } else if (ni >= 0) {
            wp_sec(pg, 0);
            struct wrow *r = wp_row(pg, WR_INFO, 0, tr("Seguridad")); wlp_cpy(r->v, sizeof r->v, wl_secname(wv.nets[ni].sec));
            r = wp_row(pg, WR_INFO, 0, tr("Señal")); snprintf(r->v, sizeof r->v, "%d dBm", wv.nets[ni].level);
        } else if (si < 0) {
            wp_sec(pg, 0); wp_row(pg, WR_TEXT, 0, tr("Red no disponible"));
        }
    } else if (kind == P_SAVED) {
        wlp_cpy(pg->title, sizeof pg->title, tr("Redes guardadas"));
        wp_sec(pg, 0);
        for (int i = 0; i < wv.nsaved && i < 60; i++) {
            struct wrow *r = wp_row(pg, WR_NET, RID_SAVED0 + i, wv.saved[i].ssid);
            r->lvl = -2; r->flags = WF_INFOBTN | (wv.saved[i].current ? WF_CHECK : 0);
        }
        if (!wv.nsaved) wp_row(pg, WR_TEXT, 0, tr("No hay redes guardadas"));
        wp_foot(pg, tr("WL_SAVED_HELP"));
    } else if (kind == P_BT) {
        wlp_cpy(pg->title, sizeof pg->title, "Bluetooth");
        int on = wl_bt_on();
        wp_sec(pg, 0);
        struct wrow *r = wp_row(pg, WR_TOGGLE, RID_BT_TOG, "Bluetooth"); r->on = on; if (wv.bpend) r->flags |= WF_BUSY;
        if (!wv.ad.found && !wv.bpend && wv.bset >= 0 && on) { wp_foot(pg, tr("WL_BT_NA")); }
        else if (on && wv.ad.alias[0]) { snprintf(b, sizeof b, tr("Visible como «%s»."), wv.ad.alias); wp_foot(pg, b); }
        if (!on || !wv.ad.powered) return;
        int nmine = 0;
        for (int i = 0; i < wv.ndevs; i++) if (wv.devs[i].paired) nmine++;
        if (nmine) {
            wp_sec(pg, "MIS DISPOSITIVOS");
            for (int c = 1; c >= 0; c--) for (int i = 0; i < wv.ndevs; i++) {     /* conectados primero */
                const struct wl_dev *dv = &wv.devs[i];
                if (!dv->paired || dv->connected != c) continue;
                r = wp_row(pg, WR_DEV, RID_DEV0 + i, dv->name[0] ? dv->name : dv->addr);
                r->flags = WF_INFOBTN;
                if (!strcmp(wv.bbusy, dv->addr)) r->flags |= WF_BUSY;
                wlp_cpy(r->v, sizeof r->v, dv->connected ? tr("Conectado") : tr("No conectado"));
            }
        }
        struct wsec *s = wp_sec(pg, "OTROS DISPOSITIVOS"); s->spin = wv.ad.discovering || wl_disc_pid > 0;
        int idx[WL_MAXDEV], n = 0;
        for (int i = 0; i < wv.ndevs; i++) if (!wv.devs[i].paired && wv.devs[i].name[0]) idx[n++] = i;
        for (int i = 1; i < n; i++) {                /* por senal (sin RSSI al final) */
            int t = idx[i], j = i - 1;
            #define WL_RS(k) (wv.devs[k].has_rssi ? wv.devs[k].rssi : -200)
            while (j >= 0 && WL_RS(idx[j]) < WL_RS(t)) { idx[j + 1] = idx[j]; j--; }
            idx[j + 1] = t;
        }
        for (int k = 0; k < n; k++) {
            const struct wl_dev *dv = &wv.devs[idx[k]];
            r = wp_row(pg, WR_DEV, RID_DEV0 + idx[k], dv->name);
            if (!strcmp(wv.bbusy, dv->addr)) r->flags |= WF_BUSY;
        }
        if (!n) wp_row(pg, WR_TEXT, 0, s->spin ? tr("Buscando…") : tr("Desliza hacia abajo para buscar"));
        wp_foot(pg, tr("WL_BT_HELP"));
    } else if (kind == P_DEV) {
        int i = wl_dev_by_addr(pg->param);
        const struct wl_dev *dv = i >= 0 ? &wv.devs[i] : 0;
        wlp_display(dv && dv->name[0] ? dv->name : pg->param, pg->title, sizeof pg->title);
        wp_sec(pg, 0);
        if (dv && !strcmp(wv.bbusy, dv->addr)) { struct wrow *r = wp_row(pg, WR_TEXT, 0, tr("Un momento…")); r->flags |= WF_BUSY; }
        else if (dv && dv->connected) wp_row(pg, WR_ACTION, RID_DISCONNECT, tr("Desconectar"));
        else if (dv) wp_row(pg, WR_ACTION, RID_CONNECT, tr("Conectar"));
        if (dv && dv->paired) wp_row(pg, WR_ACTION, RID_FORGET, tr("Olvidar este dispositivo"));
        if (dv) {
            wp_sec(pg, 0);
            struct wrow *r = wp_row(pg, WR_INFO, 0, tr("Dirección")); wlp_cpy(r->v, sizeof r->v, dv->addr);
            r = wp_row(pg, WR_INFO, 0, tr("Tipo"));
            wlp_cpy(r->v, sizeof r->v, dv->audio ? tr("Audio") : !strncmp(dv->icon, "phone", 5) ? tr("Teléfono") :
                    !strncmp(dv->icon, "computer", 8) ? tr("Ordenador") : !strncmp(dv->icon, "input", 5) ? tr("Entrada") : tr("Otro"));
            if (dv->connected && dv->has_rssi) { r = wp_row(pg, WR_INFO, 0, tr("Señal")); snprintf(r->v, sizeof r->v, "%d dBm", dv->rssi); }
            if (!dv->audio) wp_foot(pg, tr("WL_BT_NOAUDIO"));
        } else { wp_row(pg, WR_TEXT, 0, tr("Dispositivo no disponible")); }
    }
}

/* ------------------------------------------------------------------ dibujo de la pagina */
static float wl_scroll, wl_pull;           /* desplazamiento y estirado de "tirar para buscar" */
static int wl_tog_id; static double wl_tog_t0; static int wl_tog_to;
static double wl_render_ms, wl_render_max; static int wl_render_n;
static int wl_foot_h(const char *f) { return f[0] ? 8 + draw_wrapped(0, 0, 0, W - 48, f, 12, 0, 16, 0) : 0; }
static int wl_content_h(const struct wpage *pg)
{
    int h = 12;
    for (int s = 0; s < pg->ns; s++) h += (pg->s[s].hdr ? 28 : 14) + pg->s[s].n * WL_ROWH + wl_foot_h(pg->s[s].foot) + 14;
    return h + 20;
}
/* anima algo? (indicadores de actividad, interruptor moviendose, aviso) */
static int wl_animating(const struct wpage *pg)
{
    if (wl_tog_id && now_ms() - wl_tog_t0 < 260) return 1;
    if (now_ms() < toast_until + 40) return 1;
    for (int s = 0; s < pg->ns; s++) if (pg->s[s].spin) return 1;
    for (int i = 0; i < pg->nr; i++) if (pg->r[i].flags & WF_BUSY) return 1;
    return 0;
}
static void wl_render(uint16_t *d, const struct wpage *pg, int sel)
{
    double t0 = now_ms();
    int ytop = WL_TOP;
    memset(d + ytop * W, 0, (size_t)(H - ytop) * W * 2);
    float off = wl_pull > 0 ? wl_pull : -wl_scroll;
    int y = ytop + 12 + (int)off;
    if (wl_pull > 2) {                         /* tirar para buscar: aparece el indicador */
        int nv = (int)(wl_pull / 60 * 12); if (nv > 12) nv = 12;
        wl_spinner(d, W / 2.f, ytop + wl_pull / 2, 9, wl_pull >= 60 ? t0 : 0, nv, ytop);
    }
    for (int s = 0; s < pg->ns; s++) {
        const struct wsec *sc = &pg->s[s];
        if (sc->hdr) {
            const char *h = tr(sc->hdr);
            if (y + 20 > ytop) draw_text(d, 32, y + 20, h, 12, WC_GRAY, 0, ytop, H);
            if (sc->spin) wl_spinner(d, 32 + tw_n(h, 12, -1) + 14, y + 15.5f, 6.5f, t0, 12, ytop);
            y += 28;
        } else y += 14;
        int top = y;
        if (top < H && top + sc->n * WL_ROWH > ytop) wl_card(d, 16, top, W - 32, sc->n * WL_ROWH, 10, WC_CARD, ytop);
        for (int k = 0; k < sc->n; k++) {
            const struct wrow *r = &pg->r[sc->first + k];
            int ry = top + k * WL_ROWH, cy = ry + WL_ROWH / 2;
            if (ry >= H) break;
            if (ry + WL_ROWH <= ytop) continue;
            if (sc->first + k == sel && r->kind != WR_TEXT && r->kind != WR_INFO)
                wl_card(d, 16, ry, W - 32, WL_ROWH, (k == 0 || k == sc->n - 1) ? 10 : 0, WC_SEL, ytop);
            int tx = r->icon ? 68 : r->kind == WR_NET ? 48 : 32;
            if (k && ry >= ytop) for (int x = tx; x < W - 16; x++) d[ry * W + x] = WC_SEP;
            if (r->icon) wl_icon(d, 28, ry + 7, r->icon, ytop);
            uint16_t tc = r->flags & WF_DIM ? WC_GRAY : 0xFFFF;
            int right = W - 28;                    /* borde derecho del contenido */
            switch (r->kind) {
            case WR_TOGGLE: {
                float p = r->on;
                if (wl_tog_id == r->id) { double t = (t0 - wl_tog_t0) / 240; if (t < 1) { float e = (float)ease_out(t); p = wl_tog_to ? e : 1 - e; } }
                wl_switch(d, W - 16 - 12 - 51, ry + 6, p, ytop);
                if (r->flags & WF_BUSY) wl_spinner(d, W - 16 - 12 - 51 - 18, cy, 8, t0, 12, ytop);
                right = W - 16 - 12 - 51 - (r->flags & WF_BUSY ? 32 : 8);
                break;
            }
            case WR_NET:
                if (r->flags & WF_INFOBTN) { wl_infobtn(d, W - 40, cy, ytop); right = W - 56; }
                if (r->lvl >= 0) { wl_wifi_glyph(d, right - 10, cy + 6, 0.85f, r->lvl, 0xFFFF, rgb(84, 84, 88), ytop); right -= 26; }
                if (r->flags & WF_LOCK) { wl_lock(d, right - 9, cy - 7, 0xFFFF, ytop); right -= 16; }
                if (r->flags & WF_BUSY) wl_spinner(d, 33, cy, 7.5f, t0, 12, ytop);
                else if (r->flags & WF_CHECK) wl_check(d, 27, cy - 5, WC_BLUE, ytop);
                break;
            case WR_DEV:
                if (r->flags & WF_INFOBTN) { wl_infobtn(d, W - 40, cy, ytop); right = W - 56; }
                if (r->flags & WF_BUSY) { wl_spinner(d, right - 8, cy, 7.5f, t0, 12, ytop); right -= 26; }
                else if (r->v[0]) { int vw = tw_n(r->v, 15, -1); if (vw > 120) vw = 120; draw_text(d, right - vw, cy + 6, r->v, 15, WC_GRAY, 120, ytop, H); right -= vw + 10; }
                break;
            case WR_NAV:
                wl_chev(d, W - 34, cy, rgb(90, 90, 96), ytop); right = W - 42;
                /* fallthrough */
            case WR_INFO:
                if (r->v[0]) {
                    int maxv = r->kind == WR_INFO ? 170 : 130, vw = tw_n(r->v, 15, -1); if (vw > maxv) vw = maxv;
                    draw_text(d, right - vw, cy + 6, r->v, 15, WC_GRAY, maxv, ytop, H); right -= vw + 10;
                }
                break;
            case WR_ACTION: tc = r->flags & WF_RED ? WC_RED : WC_BLUE; break;
            case WR_TEXT:
                tc = WC_GRAY;
                if (r->flags & WF_BUSY) { wl_spinner(d, 33, cy, 7.5f, t0, 12, ytop); tx = 50; }
                break;
            }
            int tmax = right - tx - 4; if (tmax < 30) tmax = 30;
            draw_text(d, tx, cy + 6, r->t, 15, tc, tmax, ytop, H);
        }
        y = top + sc->n * WL_ROWH;
        if (sc->foot[0]) { y += 8; y += draw_wrapped(d, 32, y, W - 48, sc->foot, 12, WC_GRAY, 16, y > ytop - 200 && y < H); }
        y += 14;
    }
    /* barra de navegacion fija (la de estado es la del reproductor) */
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < ytop; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(12, 12, 13);
    for (int x = 0; x < W; x++) d[(ytop - 1) * W + x] = rgb(38, 38, 40);
    if (pg->back) {
        wl_line(d, 21, 55, 14.5f, 62, 2.6f, WC_BLUE, 0);   /* "<" de iOS */
        wl_line(d, 14.5f, 62, 21, 69, 2.6f, WC_BLUE, 0);
        int tw = tw_n(pg->title, 17, -1); if (tw > 150) tw = 150;
        int bmax = (W - tw) / 2 - 28 - 8; if (bmax < 24) bmax = 24;
        draw_text(d, 28, 68, tr(pg->back), 16, WC_BLUE, bmax, 0, H);
        draw_text(d, (W - tw) / 2, 68, pg->title, 17, 0xFFFF, 150, 0, H);
    } else draw_text(d, 16, 80, pg->title, 26, 0xFFFF, W - 32, 0, H);
    draw_toast(d);
    double dt = now_ms() - t0; wl_render_ms += dt; wl_render_n++; if (dt > wl_render_max) wl_render_max = dt;
}
/* fila bajo (x,y) o -1 */
static int wl_hit(const struct wpage *pg, int y)
{
    int yy = WL_TOP + 12 - (int)wl_scroll;
    if (y < WL_TOP) return -1;
    for (int s = 0; s < pg->ns; s++) {
        const struct wsec *sc = &pg->s[s];
        yy += sc->hdr ? 28 : 14;
        if (y >= yy && y < yy + sc->n * WL_ROWH) return sc->first + (y - yy) / WL_ROWH;
        yy += sc->n * WL_ROWH + wl_foot_h(sc->foot) + 14;
    }
    return -1;
}
static void wl_present(const struct wpage *pg, int sel) { wl_render(fr0, pg, sel); present(); }

/* ------------------------------------------------------------------ contrasena (teclado propio) */
static const char *wpw_title, *wpw_err; static struct ebuf *wpw_buf; static int wpw_show, wpw_caret = 1;
static void wpw_redraw(uint16_t *dst, int pressed)
{
    int top = KB_Y; if (top > H) top = H; if (top < 0) top = 0;
    for (int i = 0; i < W * top; i++) dst[i] = 0;
    memcpy(dst, fr1, 30 * W * 2);
    const char *tt = tr("Contraseña");
    int tw = tw_n(tt, 17, -1);
    draw_text(dst, (W - tw) / 2, 64, tt, 17, 0xFFFF, 0, 0, top);
    draw_text(dst, 14, 64, tr("Cancelar"), 15, WC_BLUE, 0, 0, top);
    int jw = tw_n(tr("Unirse"), 15, -1);
    draw_text(dst, W - 14 - jw, 64, tr("Unirse"), 15, wpw_buf->len ? WC_BLUE : rgb(70, 70, 76), 0, 0, top);
    int ty = 100 + draw_wrapped(dst, 16, 100, W - 32, wpw_title, 13, WC_GRAY, 17, 1);
    int fy = ty + 8;
    if (fy + 44 <= top) {
        wl_card(dst, 12, fy, W - 24, 44, 10, rgb(44, 44, 48), 0);
        char shown[260]; int n = 0;
        if (wpw_show) wlp_cpy(shown, sizeof shown, wpw_buf->b);
        else { for (const char *p = wpw_buf->b; *p && n < 200; ) { utf8_next(&p); memcpy(shown + n, "\xe2\x80\xa2", 3); n += 3; } shown[n] = 0; }
        int base = fy + 28, maxw = W - 24 - 52;
        const char *vis = shown;                  /* el final siempre visible: se recorta el principio */
        while (*vis && tw_n(vis, 16, -1) > maxw) utf8_next(&vis);
        int sw = draw_text(dst, 24, base, vis, 16, 0xFFFF, 0, fy + 4, fy + 40);
        if (!wpw_buf->len) draw_text(dst, 24, base, tr("Obligatoria"), 16, rgb(110, 110, 116), 0, 0, H);
        int cx = 24 + sw;
        if (wpw_caret && cx < W - 52) for (int y = fy + 10; y < fy + 34; y++) for (int x = cx; x < cx + 2; x++) dst[y * W + x] = WC_BLUE;
        /* ojo: mostrar / ocultar */
        float ex = W - 36, ey = fy + 22;
        wl_arc(dst, ex, ey + 6, 9, 1.5f, -1.0f, 1.0f, WC_GRAY, 0); wl_arc(dst, ex, ey - 6, 9, 1.5f, 2.14f, 3.15f, WC_GRAY, 0);
        wl_arc(dst, ex, ey - 6, 9, 1.5f, -3.15f, -2.14f, WC_GRAY, 0);
        fill_circle(dst, ex, ey, 2.6f, WC_GRAY, 255);
        if (!wpw_show) wl_line(dst, ex - 8, ey - 7, ex + 8, ey + 7, 1.5f, WC_GRAY, 0);
        if (wpw_err && fy + 70 <= top) draw_text(dst, 16, fy + 66, wpw_err, 13, WC_RED, W - 32, 0, top);
    }
    kb_draw(dst, pressed);
}
static int wpw_field_y(void) { return 108 + draw_wrapped(0, 16, 100, W - 32, wpw_title, 13, 0, 17, 0); }
/* 1 = aceptada (en buf); 0 = cancelada. min = longitud minima (8 WPA, 5 WEP) */
static int wl_password(const char *ssid, const char *err, char *buf, int cap, int min)
{
    char title[200], disp[72]; wlp_display(ssid, disp, sizeof disp);
    snprintf(title, sizeof title, tr("Introduce la contraseña de «%s»"), disp);
    struct ebuf e = { buf, (int)strlen(buf), cap, (int)strlen(buf) };
    wpw_title = title; wpw_err = err; wpw_buf = &e; wpw_caret = 1; wpw_show = 0;
    int ac = kb_cfg_autocap, pe = kb_cfg_period;
    kb_cfg_autocap = 0; kb_cfg_period = 0;            /* contrasenas: sin mayuscula ni ". " automaticos */
    kb_layer = 0; kb_shift = 0; kb_caps = 0; kb_layout(tr("Unirse"));
    kb_begin(&e, fr0, wpw_redraw);
    struct pkt p; double blink = now_ms(); int ok = 0;
    for (;;) {
        int wait = (int)(blink + 530 - now_ms()); if (wait < 1) wait = 1;
        if (!read_pkt(&p, wait)) {
            if (now_ms() - blink >= 530) { wpw_caret = !wpw_caret; blink = now_ms(); wpw_redraw(fr0, -1); present_rows(wpw_field_y(), wpw_field_y() + 44); }
            continue;
        }
        if (!p.down) continue;
        wpw_caret = 1;
        if (p.y >= KB_Y) {
            int r = kb_touch(&p, &e, fr0, wpw_redraw, 0);
            blink = now_ms();
            if (r == K_ENTER) {
                if (e.len >= min) { ok = 1; break; }
                wpw_err = tr("La contraseña es demasiado corta"); wpw_redraw(fr0, -1); present();
            }
            continue;
        }
        int x = p.x, y = p.y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 90 && x < 110) break;                                    /* Cancelar */
        if (y < 90 && x > W - 110 && e.len >= min) { ok = 1; break; }    /* Unirse */
        int fy = wpw_field_y();
        if (y >= fy && y < fy + 44) {
            if (x > W - 60) wpw_show = !wpw_show;
            if (kb_off) kb_reveal(fr0, wpw_redraw); else { wpw_redraw(fr0, -1); present(); }
        }
    }
    kb_end();
    kb_cfg_autocap = ac; kb_cfg_period = pe;
    wpw_buf = 0;
    return ok;
}

/* ------------------------------------------------------------------ acciones */
static int wl_gone;                        /* salio con el gesto de inicio */
static char wl_last_join[33]; static int wl_last_sec, wl_last_hidden; static unsigned wl_join_seen, wl_msg_seen;
static void wl_page(int kind, const char *param, const char *back);

static void wl_join_ui(const char *ssid, int sec, int hidden, const char *err)
{
    if (sec == WSEC_EAP) { toast(tr("Las redes empresariales (802.1X) no son compatibles")); return; }
    char pw[72] = "";
    int known = wl_saved_by_ssid(ssid) >= 0 && !err;
    if (sec != WSEC_OPEN && !known) {
        if (!wl_password(ssid, err, pw, sizeof pw, sec == WSEC_WEP ? 5 : 8)) return;
        if (sec == WSEC_PSK && !wlp_psk_ok(pw)) { toast(tr("La contraseña debe tener entre 8 y 63 caracteres")); memset(pw, 0, sizeof pw); return; }
    }
    wlp_cpy(wl_last_join, sizeof wl_last_join, ssid); wl_last_sec = sec; wl_last_hidden = hidden;
    wl_push(&wl_qw, J_WJOIN, sec | hidden << 8, ssid, pw);
    memset(pw, 0, sizeof pw);
    /* respuesta inmediata en la lista (el hilo pone "joining" enseguida) */
    pthread_mutex_lock(&wl_mu); wlp_cpy(WS.joining, sizeof WS.joining, ssid); WS.gen++; pthread_mutex_unlock(&wl_mu);
}
static void wl_other(void)
{
    char name[33] = "";
    if (!line_edit(tr("Otra red"), tr("Nombre"), name, sizeof name) || !name[0]) return;
    const char *o[2] = { tr("Ninguna"), "WPA/WPA2/WPA3" };
    int s = action_sheet(tr("Seguridad"), o, 2, -1);
    if (s < 0) return;
    wl_join_ui(name, s == 0 ? WSEC_OPEN : WSEC_PSK, 1, 0);
}
static int wl_adb_wifi(void)
{
    FILE *f = fopen("/data/.hibymod_adbmode", "r"); char m[16] = "";
    if (f) { if (!fgets(m, sizeof m, f)) m[0] = 0; fclose(f); } else if (!access("/data/.hibymod_adbwifi", F_OK)) strcpy(m, "wifi");
    return !strncmp(m, "wifi", 4);
}
static void wl_toggle_anim(int id, int to) { wl_tog_id = id; wl_tog_to = to; wl_tog_t0 = now_ms(); }
/* tocar una fila; x para el boton (i). Devuelve 1 si hay que salir de la pagina */
static int wl_tap(struct wpage *pg, const struct wrow *r, int x)
{
    int id = r->id;
    switch (pg->kind) {
    case P_ROOT:
        if (id == RID_NAV_WIFI) wl_page(P_WIFI, 0, pg->title);
        else if (id == RID_NAV_BT) wl_page(P_BT, 0, pg->title);
        else if (id == RID_AIRPLAY || id == RID_DLNA || id == RID_LINK) {
            if (r->flags & WF_DIM) { toast(tr("Activa el Wi-Fi primero")); break; }
            const char *nm = id == RID_AIRPLAY ? "airplay" : id == RID_DLNA ? "dlna" : "hibylink";
            int v = !r->on;
            pthread_mutex_lock(&wl_mu);                 /* optimista: el hilo lo confirma */
            if (id == RID_AIRPLAY) WS.airplay = v; else if (id == RID_DLNA) WS.dlna = v; else WS.hibylink = v;
            WS.gen++; pthread_mutex_unlock(&wl_mu);
            wl_toggle_anim(id, v);
            wl_push(&wl_qw, J_HSSET, v, nm, 0);
        }
        break;
    case P_WIFI:
        if (id == RID_WIFI_TOG) {
            if (wv.wpend) break;
            int v = !r->on;
            if (!v && wl_adb_wifi() && !confirm(tr("WL_ADB_WARN"), tr("Desactivar Wi-Fi"))) break;
            pthread_mutex_lock(&wl_mu); WS.wpend = v + 1; WS.gen++; pthread_mutex_unlock(&wl_mu);
            wl_toggle_anim(id, v);
            wl_push(&wl_qw, J_WTOGGLE, v, 0, 0);
        } else if (id == RID_OTHER) wl_other();
        else if (id == RID_NAV_SAVED) wl_page(P_SAVED, 0, "Wi-Fi");
        else if (id >= RID_NET0 - 1 && id < RID_NET0 + WL_MAXNET) {
            if (id == RID_NET0 - 1) { wl_page(P_NET, wv.ssid, "Wi-Fi"); break; }
            const struct wl_net *n = &wv.nets[id - RID_NET0];
            if ((r->flags & WF_INFOBTN) && x > W - 64) { wl_page(P_NET, n->ssid, "Wi-Fi"); break; }
            if (wv.joining[0]) { toast(tr("Espera a que termine la conexión")); break; }
            wl_join_ui(n->ssid, n->sec, 0, 0);
        }
        break;
    case P_SAVED:
        if (id >= RID_SAVED0 && id < RID_SAVED0 + WL_MAXSAVED) wl_page(P_NET, wv.saved[id - RID_SAVED0].ssid, pg->title);
        break;
    case P_NET: {
        int si = wl_saved_by_ssid(pg->param), ni = wl_net_by_ssid(pg->param);
        if (id == RID_FORGET && si >= 0) {
            char q[160], disp[72]; wlp_display(pg->param, disp, sizeof disp);
            snprintf(q, sizeof q, tr("¿Olvidar la red «%s»? El HiBy dejará de conectarse a ella."), disp);
            if (wl_connected() && !strcmp(wv.ssid, pg->param) && wl_adb_wifi()) snprintf(q + strlen(q), sizeof q - strlen(q), " %s", tr("ADB por Wi-Fi se desconectará."));
            if (!confirm(q, tr("Olvidar"))) break;
            wl_push(&wl_qw, J_WFORGET, wv.saved[si].id, 0, 0);
            return 1;
        }
        if (id == RID_JOIN && ni >= 0) {
            if (wv.joining[0]) { toast(tr("Espera a que termine la conexión")); break; }
            wl_join_ui(wv.nets[ni].ssid, wv.nets[ni].sec, 0, 0);
            return 1;
        }
        break;
    }
    case P_BT:
        if (id == RID_BT_TOG) {
            if (wv.bpend) break;
            int v = !r->on;
            pthread_mutex_lock(&wl_mu); WS.bpend = v + 1; WS.gen++; pthread_mutex_unlock(&wl_mu);
            wl_toggle_anim(id, v);
            wl_push(&wl_qb, J_BTOGGLE, v, 0, 0);
        } else if (id >= RID_DEV0 && id < RID_DEV0 + WL_MAXDEV) {
            const struct wl_dev *dv = &wv.devs[id - RID_DEV0];
            if (wv.bbusy[0]) { toast(tr("Espera a que termine la conexión")); break; }
            if (dv->paired && ((r->flags & WF_INFOBTN) && x > W - 64)) { wl_page(P_DEV, dv->addr, "Bluetooth"); break; }
            if (dv->paired && dv->connected) { wl_page(P_DEV, dv->addr, "Bluetooth"); break; }
            pthread_mutex_lock(&wl_mu); wlp_cpy(WS.bbusy, sizeof WS.bbusy, dv->addr); WS.gen++; pthread_mutex_unlock(&wl_mu);
            wl_push(&wl_qb, J_BCONNECT, dv->paired, dv->addr, dv->name[0] ? dv->name : dv->addr);
        }
        break;
    case P_DEV: {
        int i = wl_dev_by_addr(pg->param); if (i < 0) break;
        const struct wl_dev *dv = &wv.devs[i];
        if (wv.bbusy[0]) { toast(tr("Espera a que termine la conexión")); break; }
        if (id == RID_CONNECT || id == RID_DISCONNECT) {
            pthread_mutex_lock(&wl_mu); wlp_cpy(WS.bbusy, sizeof WS.bbusy, dv->addr); WS.gen++; pthread_mutex_unlock(&wl_mu);
            wl_push(&wl_qb, id == RID_CONNECT ? J_BCONNECT : J_BDISCONNECT, dv->paired, dv->addr, dv->name);
        } else if (id == RID_FORGET) {
            char q[160], disp[72]; wlp_display(dv->name[0] ? dv->name : dv->addr, disp, sizeof disp);
            snprintf(q, sizeof q, tr("¿Olvidar «%s»? Tendrás que volver a emparejarlo."), disp);
            if (!confirm(q, tr("Olvidar dispositivo"))) break;
            pthread_mutex_lock(&wl_mu); wlp_cpy(WS.bbusy, sizeof WS.bbusy, dv->addr); WS.gen++; pthread_mutex_unlock(&wl_mu);
            wl_push(&wl_qb, J_BFORGET, 0, dv->addr, 0);
            return 1;
        }
        break;
    }
    }
    return 0;
}

/* avisos de los hilos: errores (toast) y resultado de unirse (contrasena incorrecta: otra vez) */
static void wl_events(int kind)
{
    if (wv.msg_seq != wl_msg_seen) {
        wl_msg_seen = wv.msg_seq;
        if (wv.msg_key) { char m[160]; char a[64]; wlp_display(wv.msg_arg, a, sizeof a); snprintf(m, sizeof m, tr(wv.msg_key), a); toast(m); }
    }
    if (wv.join_seq != wl_join_seen) {
        wl_join_seen = wv.join_seq;
        char disp[72]; wlp_display(wv.join_ssid, disp, sizeof disp);
        if (wv.join_res == WJ_WRONGKEY && kind == P_WIFI && !strcmp(wv.join_ssid, wl_last_join))
            wl_join_ui(wl_last_join, wl_last_sec, wl_last_hidden, tr("Contraseña incorrecta"));
        else if (wv.join_res == WJ_WRONGKEY) { char m[160]; snprintf(m, sizeof m, tr("Contraseña incorrecta para «%s»"), disp); toast(m); }
        else if (wv.join_res == WJ_FAIL) { char m[160]; snprintf(m, sizeof m, tr("No se pudo conectar a «%s»"), disp); toast(m); }
    }
}

/* ------------------------------------------------------------------ bucle de una pagina */
static struct wpage wl_pages[5];
static int wl_depth;
static void wl_pull_refresh(int kind)
{
    if (kind == P_BT) { wl_disc_until = now_ms() + 45000; wl_push(&wl_qb, J_REFRESH, 0, 0, 0); }
    else { wl_push(&wl_qw, J_WSCAN, 0, 0, 0); if (kind == P_ROOT) wl_push(&wl_qb, J_REFRESH, 0, 0, 0); }
}
static void wl_page(int kind, const char *param, const char *back)
{
    if (wl_depth >= 5) return;
    struct wpage *pg = &wl_pages[wl_depth++];
    int vis0 = wl_vis;
    wl_vis = kind == P_ROOT ? 1 : kind == P_BT || kind == P_DEV ? 3 : 2;
    if (kind == P_BT) { wl_disc_until = now_ms() + 45000; wl_push(&wl_qb, J_REFRESH, 0, 0, 0); }
    if (kind == P_WIFI) wl_push(&wl_qw, J_WSCAN, 0, 0, 0);
    float scroll0 = wl_scroll; wl_scroll = 0; wl_pull = 0;
    wl_snap();
    wl_build(pg, kind, param); pg->back = back;
    if (back) {                                 /* entrada: la nueva pagina desde la derecha */
        memcpy(img_under, fr0, HM_FRAME_BYTES); have_under = 0;
        wl_render(img_base, pg, -1); fs_transition(img_under, img_base, 1, 0);
    }
    float vel = 0; int first = 1, last_step = -1;
    for (;;) {
        int ch = wl_snap();
        if (ch) wl_build(pg, kind, 0);
        unsigned ms0 = wl_msg_seen, js0 = wl_join_seen;
        wl_events(kind);
        if (ms0 != wl_msg_seen || js0 != wl_join_seen) ch = 1;
        float maxs = wl_content_h(pg) - (H - WL_TOP); if (maxs < 0) maxs = 0;
        if (wl_scroll > maxs) wl_scroll = maxs;
        /* se repinta solo si algo cambia: datos, interruptor animandose (cada fotograma) o el
         * indicador de actividad (12 pasos/s, como iOS); en reposo no se gasta CPU */
        int fast = wl_tog_id && now_ms() - wl_tog_t0 < 260, anim = wl_animating(pg), step = (int)(now_ms() / 83);
        if (first || ch || fast || (anim && step != last_step)) { wl_present(pg, -1); first = 0; last_step = step; }
        struct pkt p;
        if (!read_pkt(&p, fast ? 1 : anim ? 20 : 250)) continue;
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms(); float s0 = wl_scroll; int moved = 0, ly = sy; double lt = ts;
        /* desde abajo: inicio (la ventana sigue al dedo; mantener 1 s: multitarea) */
        if (sy >= 456) {
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { wl_render(fr0, pg, -1); if (own_home_gesture(sx, sy, app_by_id("wireless"))) { wl_gone = 1; break; } wl_present(pg, -1); continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        /* desde un borde: atras (en la raiz, salir) */
        if (back_zone(sx, sy)) {
            wl_render(fr0, pg, -1);
            int bk = app_back_gesture(sx, sy);
            if (bk > 0) break;
            if (bk < 0) continue;
            if (!cur_down) { sx = cur_x; sy = cur_y; }
        }
        int hi = wl_hit(pg, sy);
        if (hi >= 0 && pg->r[hi].kind != WR_TEXT && pg->r[hi].kind != WR_INFO) wl_present(pg, hi);
        int pulled = 0;
        while (cur_down && read_pkt(&p, 30)) {
            if (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 10) moved = 1;
            if (!moved) continue;
            double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
            float sc = s0 - (cur_y - sy);
            if (sc < 0) { wl_pull = kind == P_ROOT || kind == P_WIFI || kind == P_BT ? -sc * 0.55f : 0; if (wl_pull > 110) wl_pull = 110 + (wl_pull - 110) * 0.3f; sc = 0; }
            else wl_pull = 0;
            if (sc > maxs) sc = maxs;
            wl_scroll = sc;
            if (wl_pull >= 60 && !pulled) pulled = 1;
            wl_render(fr0, pg, -1); present();
        }
        if (wl_pull > 0) {                         /* soltar: vuelve con muelle; si paso el umbral, busca */
            if (wl_pull >= 60) { wl_pull_refresh(kind); lg("wireless: tirar para buscar"); }
            float p0 = wl_pull; double tr0 = now_ms();
            for (;;) {
                double t = (now_ms() - tr0) / 280; if (t > 1) t = 1;
                wl_pull = p0 * (1 - (float)ease_out(t));
                wl_render(fr0, pg, -1); present();
                if (t >= 1) break;
            }
            wl_pull = 0; vel = 0; (void)pulled;
            continue;
        }
        if (moved) {
            /* inercia de iOS: decelera ~0.998/ms; se para al tocar o en los extremos */
            if (now_ms() - lt > 60) vel = 0;
            double tp = now_ms();
            while (fabsf(vel) > 0.03f) {
                double t = now_ms(); float dt = (float)(t - tp); tp = t; if (dt > 40) dt = 40;
                wl_scroll -= vel * dt; vel *= powf(0.9975f, dt);
                if (wl_scroll < 0) { wl_scroll = 0; vel = 0; } if (wl_scroll > maxs) { wl_scroll = maxs; vel = 0; }
                if (wl_snap()) wl_build(pg, kind, 0);
                wl_render(fr0, pg, -1); present();
                struct pkt q; if (read_pkt(&q, 0) && q.down) { vel = 0; drain_until_up(); break; }
            }
            vel = 0;
            continue;
        }
        if (back && sy < WL_TOP && sx < 110) break;                     /* "< Atras" */
        if (hi < 0 || now_ms() - ts > 700) { wl_present(pg, -1); continue; }
        struct wrow row = pg->r[hi];
        if (row.kind == WR_TEXT || row.kind == WR_INFO) continue;
        if (wl_tap(pg, &row, sx)) { if (!wl_gone) break; }
        if (wl_gone) break;
        wl_snap(); wl_build(pg, kind, 0); wl_present(pg, -1);
    }
    if (back && !wl_gone) {                     /* salida: la pagina anterior vuelve desde la izquierda */
        wl_render(img_under, pg, -1);
        struct wpage *prev = &wl_pages[wl_depth - 2];
        wl_scroll = scroll0;
        wl_snap(); wl_build(prev, prev->kind, 0);
        wl_render(img_base, prev, -1);
        fs_transition(img_under, img_base, -1, 0);
        have_under = 0;
    }
    wl_scroll = scroll0;
    wl_vis = vis0;
    wl_depth--;
}

/* ------------------------------------------------------------------ entrada */
static void wireless_app_at(int ix, int iy)
{
    double t_open = now_ms();
    wireless_preload();
    wl_push(&wl_qw, J_HSREAD, 0, 0, 0);
    wl_push(&wl_qw, J_REFRESH, 0, 0, 0);
    wl_push(&wl_qb, J_REFRESH, 0, 0, 0);
    have_under = 0;
    wl_gone = 0; wl_depth = 0; wl_scroll = 0; wl_pull = 0;
    wl_render_ms = wl_render_max = 0; wl_render_n = 0;
    wl_snap(); wl_msg_seen = wv.msg_seq; wl_join_seen = wv.join_seq;
    struct wpage *root = &wl_pages[0];
    wl_build(root, P_ROOT, 0); root->back = 0;
    wl_render(img_app, root, -1);
    render_launcher(img_base, now_ms());
    double t0 = now_ms();
    for (;;) {   /* apertura con zoom desde el icono */
        double t = (now_ms() - t0) / 300; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    lg("wireless: abierta en %.0f ms", now_ms() - t_open);
    wl_page(P_ROOT, 0, 0);
    wl_vis = 0;
    lg("wireless: pintado medio %.1f ms, maximo %.1f ms (%d fotogramas)", wl_render_n ? wl_render_ms / wl_render_n : 0, wl_render_max, wl_render_n);
    if (!wl_gone) {   /* salir: la ventana se encoge hacia su icono */
        wl_snap(); wl_build(root, P_ROOT, 0); wl_render(img_app, root, -1);
        render_launcher(img_base, now_ms());
        t0 = now_ms();
        for (;;) {
            double t = (now_ms() - t0) / 280; if (t > 1) t = 1; double e = 1 - ease_out(t);
            int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
            for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
            draw_scaled(fr0, img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
            present(); if (t >= 1) break;
        }
    }
    wl_gone = 0;
    drain_until_up();
    launcher_present();
}
static void wireless_app(void) { wireless_app_at((W - L.icon) / 2, (H - L.icon) / 2); }
