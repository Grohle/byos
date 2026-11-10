/* sysset.inc.c - paginas de sistema de Ajustes: sustituyen a los Ajustes de HiBy y a su app
 * "DAC USB" (incluido desde settings_app.inc.c, despues del marco de paginas).
 *
 * Ajustes del reproductor: API de hibyset.inc.c (comando 'S' de hibymod_disp.so v7, ver
 * docs/research_settings_api.md). La interfaz nunca espera al reproductor:
 *   - al abrir Ajustes se leen los valores de /usr/data/user.ini (la imagen de su estructura de
 *     ajustes: desplazamiento del campo = desplazamiento en el fichero) y, en segundo plano, se
 *     piden todos a la libreria (hs_list); al volver a una pagina se vuelven a pedir;
 *   - un cambio se muestra al instante (optimista) y lo aplica un hilo propio, de uno en uno
 *     (la API solo admite una peticion a la vez) y como mucho 4 escrituras/s (cada cambio
 *     reescribe user.ini en la flash); varios cambios seguidos de la misma opcion se funden
 *     en el ultimo (deslizadores);
 *   - si el reproductor lo rechaza, el valor vuelve al confirmado (el interruptor se desliza de
 *     vuelta) y sale un aviso breve.
 * Claves que la libreria v7 aun no tiene (brillo, apagado de pantalla, apagado por
 * inactividad, modo de actualizacion de la biblioteca): la interfaz ya las usa; mientras la
 * libreria no las conozca (-ENOENT, o no aparecen en hs_list) se muestran atenuadas con el
 * valor guardado. Propuesta para la libreria: docs/research_settings_api.md §9.
 *
 * Prefijos: ps_ (estado del reproductor), si_ (informacion del sistema), ss_ (paginas). */
#include "hibyset.inc.c"
#include <sys/time.h>

/* ------------------------------------------------------------------ estado del reproductor */
enum { PS_BT, PS_WIFI, PS_DLNA, PS_AIRPLAY, PS_HLINK, PS_GAIN, PS_MSEB, PS_LINEOUT, PS_USB, PS_BAL, PS_GAPLESS, PS_FADE,
       PS_FILTER, PS_SLEEP, PS_SLEEPMIN, PS_PLAYMODE, PS_VOL, PS_VOLMAX, PS_SPEED,
       PS_EXT,                                   /* desde aqui: claves propuestas (libreria v8) */
       PS_BRIGHT = PS_EXT, PS_BKLON, PS_BKLTIME, PS_IDLE, PS_IDLEMIN, PS_UPDATE, PS_N };
/* clave de la API, desplazamiento en user.ini (= en la estructura de ajustes) y rango valido */
static const struct { const char *key; short off, lo, hi; } ps_def[PS_N] = {
    { "bt", 0x394, 0, 1 }, { "wifi", 0x3c8, 0, 1 }, { "dlna", 0x3d0, 0, 1 }, { "airplay", 0x3cc, 0, 1 }, { "hibylink", 0x3b4, 0, 1 },
    { "gain", 0x24c, 0, 1 }, { "mseb", 0x420, 0, 1 }, { "lineout", 0x280, 0, 1 }, { "usbmode", 0x274, 0, 2 }, { "balance", 0x254, -10, 10 },
    { "gapless", 0x234, 0, 1 }, { "fade", 0x3c4, 0, 1 }, { "filter", 0x3c0, 0, 1 }, { "sleep", 0x270, 0, 1 }, { "sleepmin", 0x0c, 5, 120 },
    { "playmode", 0x250, 0, 3 }, { "volume", 0x23c, 0, 255 }, { "volmax", 0x238, 0, 255 }, { "speed", 0x710, 50, 200 },
    { "bright", 0x18, 1, 100 }, { "bklon", 0x3d4, 0, 1 }, { "bkltime", 0x10, 10, 120 }, { "idle", 0x26c, 0, 1 }, { "idlemin", 0x14, 1, 10 },
    { "update", 0x20, 0, 1 },
};
static int ps_v[PS_N];          /* lo que se ve (incluye cambios aun sin confirmar) */
static int ps_sh[PS_N];         /* ultimo valor confirmado por el reproductor (o leido) */
static int ps_ok[PS_N];         /* hay valor */
static int ps_unsup[PS_N];      /* la libreria no conoce la clave */
static int ps_api;              /* 1 responde, 0 sin saber aun, -1 no responde (sin libreria v7) */
static struct psjob { int idx, val, flags; } ps_q[16];   /* idx -1: leer todo (hs_list) */
static int ps_qn, ps_inflight = -2, ps_thr;
static volatile int ps_err, ps_err_idx = -1;
static double ps_list_t;
static pthread_mutex_t ps_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ps_cv = PTHREAD_COND_INITIALIZER;

static int ps_queued(int idx) { for (int i = 0; i < ps_qn; i++) if (ps_q[i].idx == idx) return 1; return 0; }
static void ps_push(int idx, int val, int flags)          /* con ps_mu tomado */
{
    for (int i = 0; i < ps_qn; i++) if (ps_q[i].idx == idx) { ps_q[i].val = val; ps_q[i].flags |= flags; return; }   /* se funde */
    if (ps_qn < (int)(sizeof ps_q / sizeof ps_q[0])) { ps_q[ps_qn].idx = idx; ps_q[ps_qn].val = val; ps_q[ps_qn].flags = flags; ps_qn++; }
    pthread_cond_signal(&ps_cv);
}
static int ps_find(const char *k) { for (int i = 0; i < PS_N; i++) if (!strcmp(k, ps_def[i].key)) return i; return -1; }

static void *ps_thread(void *arg)
{
    (void)arg;
    double last_set = 0;
    pthread_mutex_lock(&ps_mu);
    for (;;) {
        while (!ps_qn) pthread_cond_wait(&ps_cv, &ps_mu);
        struct psjob j = ps_q[0];
        memmove(ps_q, ps_q + 1, (size_t)--ps_qn * sizeof ps_q[0]);
        ps_inflight = j.idx;
        pthread_mutex_unlock(&ps_mu);
        if (j.idx < 0) {                                       /* leer todo de una vez */
            struct hs_kv kv[40]; int n = hs_list(kv, 40);
            pthread_mutex_lock(&ps_mu);
            if (n > 0) {
                int seen[PS_N] = { 0 };
                ps_api = 1;
                for (int i = 0; i < n; i++) {
                    int k = ps_find(kv[i].name); if (k < 0) continue;
                    seen[k] = 1; ps_unsup[k] = 0;
                    /* no pisar un cambio del usuario que aun no se ha aplicado */
                    if (!ps_queued(k) && ps_v[k] == ps_sh[k]) ps_v[k] = kv[i].val;
                    if (!ps_queued(k)) ps_sh[k] = kv[i].val;
                    ps_ok[k] = 1;
                }
                for (int k = PS_EXT; k < PS_N; k++) if (!seen[k]) ps_unsup[k] = 1;   /* libreria v7: aun sin estas */
            } else if (n == -ENXIO || n == -ETIMEDOUT || n == -ENOEXEC) ps_api = -1;
            ps_inflight = -2;
            continue;
        }
        double w = 250 - (now_ms() - last_set);               /* como mucho 4 escrituras/s */
        if (w > 0) msleep((int)w);
        struct hs_res r; memset(&r, 0, sizeof r);
        pthread_mutex_lock(&hs_mu);
        int rc = hs_wait_ex(hs_set_send(ps_def[j.idx].key, j.val, j.flags), &r, 0, 0, HS_TIMEOUT_MS);
        pthread_mutex_unlock(&hs_mu);
        last_set = now_ms();
        pthread_mutex_lock(&ps_mu);
        int k = j.idx, newer = ps_queued(k);
        if (rc == 0) {
            ps_api = 1; ps_unsup[k] = 0; ps_ok[k] = 1;
            ps_sh[k] = r.val; if (!newer) ps_v[k] = r.val;
        } else {
            if (rc == -ENOENT) ps_unsup[k] = 1;
            if (rc == -ENXIO || rc == -ENOEXEC) ps_api = -1;
            if (r.key[0] && (rc == -EPERM || rc == -EAGAIN)) ps_sh[k] = r.val;     /* rechazado: el valor que tiene ahora */
            if (!newer) ps_v[k] = ps_sh[k];
            if (k != PS_BRIGHT) { ps_err = rc; ps_err_idx = k; }    /* brillo: ya se aplico por sysfs */
            lg("ajustes: %s=%d -> %d", ps_def[k].key, j.val, rc);
        }
        /* el reproductor cambia otras opciones por su cuenta (sin pausas <-> fundido) */
        if (k == PS_GAPLESS || k == PS_FADE || k == PS_LINEOUT || k == PS_WIFI) ps_push(-1, 0, 0);
        ps_inflight = -2;
    }
    return 0;
}
static void ps_start(void)
{
    if (ps_thr) return;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_t th;
    if (!pthread_create(&th, &at, ps_thread, 0)) { pthread_detach(th); ps_thr = 1; }
    pthread_attr_destroy(&at);
}
static int ps_busy(void) { return ps_qn > 0 || ps_inflight != -2; }
/* pedir todos los valores (al abrir y al volver a una pagina); como mucho uno por segundo */
static void ps_refresh(void)
{
    if (!ps_thr || now_ms() - ps_list_t < 1000) return;
    ps_list_t = now_ms();
    pthread_mutex_lock(&ps_mu); ps_push(-1, 0, 0); pthread_mutex_unlock(&ps_mu);
}
/* valores guardados, al instante (sin esperar a la libreria) */
static void ps_load_ini(void)
{
    unsigned char b[0xad0]; int f = open("/usr/data/user.ini", O_RDONLY); if (f < 0) return;
    int n = (int)read(f, b, sizeof b); close(f);
    pthread_mutex_lock(&ps_mu);
    for (int k = 0; k < PS_N; k++) {
        int o = ps_def[k].off; if (o + 4 > n) continue;
        int32_t v; memcpy(&v, b + o, 4);                       /* MIPS little-endian, como el fichero */
        if (v < ps_def[k].lo || v > ps_def[k].hi) continue;
        if (ps_queued(k) || ps_inflight == k || ps_v[k] != ps_sh[k]) continue;
        ps_v[k] = ps_sh[k] = v; ps_ok[k] = 1;
    }
    pthread_mutex_unlock(&ps_mu);
}
/* aplicar ps_v[idx] (ya cambiado por la interfaz) */
static void ps_apply_f(int idx, int flags)
{
    pthread_mutex_lock(&ps_mu); ps_push(idx, ps_v[idx], flags); pthread_mutex_unlock(&ps_mu);
}
static void ps_set_now(int idx, int v) { ps_v[idx] = v; ps_apply_f(idx, 0); }
static const char *ps_errmsg(int rc, int idx)
{
    switch (rc) {
    case -ENXIO: case -ETIMEDOUT: case -ENOEXEC: return "El reproductor no responde";
    case -ENOENT: return "Requiere actualizar HiByOS";
    case -EAGAIN: return "El reproductor está ocupado";
    case -EPERM: return idx == PS_MSEB ? "MSEB necesita velocidad 1x y sin MQA" : "No disponible con la salida actual";
    default: return "No se pudo cambiar el ajuste";
    }
}
/* en el hilo de la interfaz, cada vuelta: avisos de los rechazos */
static void ps_tick(void)
{
    int e = ps_err;
    if (e) { ps_err = 0; toast(tr(ps_errmsg(e, ps_err_idx))); }
}

/* ------------------------------------------------------------------ informacion del sistema
 * Version, almacenamiento, biblioteca (hibymix stats) y memoria: en un hilo, para que abrir
 * Informacion o Biblioteca sea instantaneo. Se refresca al abrir Ajustes (si tiene > 5 s). */
struct sinfo { char ver[48], mac[24]; long long sd_free, sd_tot, in_free, in_tot; long mem_kb; int songs, albums, artists, napps, lib, ready; };
static struct sinfo si, si_ui;
static pthread_mutex_t si_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int si_busy; static double si_t;
static void *si_thread(void *a)
{
    (void)a;
    static struct sinfo n;
    pthread_mutex_lock(&si_mu); n = si; pthread_mutex_unlock(&si_mu);
    if (!n.ver[0]) {
        FILE *f = popen("/usr/bin/hibymod status 2>/dev/null | head -1", "r"); char l[64] = "";
        if (f) { if (!fgets(l, sizeof l, f)) l[0] = 0; pclose(f); }
        l[strcspn(l, "\n")] = 0;
        snprintf(n.ver, sizeof n.ver, "%s", strncmp(l, "hibymod ", 8) ? l : l + 8);
    }
    struct statvfs v;
    if (!statvfs("/usr/data/mnt/sd_0", &v) && v.f_blocks) { n.sd_free = (long long)v.f_bavail * v.f_frsize; n.sd_tot = (long long)v.f_blocks * v.f_frsize; } else n.sd_tot = n.sd_free = 0;
    if (!statvfs("/usr/data", &v)) { n.in_free = (long long)v.f_bavail * v.f_frsize; n.in_tot = (long long)v.f_blocks * v.f_frsize; }
    FILE *f = fopen("/proc/meminfo", "r"); char l[96];
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "MemAvailable:", 13)) n.mem_kb = atol(l + 13);
    if (f) fclose(f);
    f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) { if (fgets(n.mac, sizeof n.mac, f)) n.mac[strcspn(n.mac, "\n")] = 0; fclose(f); }
    for (char *c = n.mac; *c; c++) *c = (char)toupper((unsigned char)*c);
    n.napps = 0; DIR *d = opendir("/usr/data/hibymod/apps"); struct dirent *e;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') n.napps++;
    if (d) closedir(d);
    char *argv[] = { "/usr/data/hibymod/shell/hibymix", "stats", 0 };
    static char out[2048];
    if (!access(argv[0], X_OK) && run_capture(argv, out, sizeof out) > 0)
        for (char *sv = 0, *ln = strtok_r(out, "\n", &sv); ln; ln = strtok_r(0, "\n", &sv))
            if (!strncmp(ln, "count\t", 6)) { char *q = ln + 6; n.songs = atoi(q); q = strchr(q, '\t');
                if (q) { n.albums = atoi(++q); q = strchr(q, '\t'); if (q) n.artists = atoi(++q); } n.lib = 1; }
    n.ready = 1;
    pthread_mutex_lock(&si_mu); si = n; pthread_mutex_unlock(&si_mu);
    si_busy = 0;
    return 0;
}
static void si_refresh(void)
{
    if (si_busy || (si_t && now_ms() - si_t < 5000)) return;
    si_busy = 1; si_t = now_ms();
    pthread_t th; if (pthread_create(&th, 0, si_thread, 0)) { si_busy = 0; return; }
    pthread_detach(th);
}
static void si_snap(void) { pthread_mutex_lock(&si_mu); si_ui = si; pthread_mutex_unlock(&si_mu); }

/* al abrir la app (Ajustes o DAC USB): todo precargado, nada bloquea la navegacion */
static void ss_open(void)
{
    ps_start(); ps_load_ini(); ps_list_t = 0; ps_refresh();
    si_refresh(); audio_sync();
}

/* ------------------------------------------------------------------ alerta de iOS
 * Tarjeta centrada (titulo, mensaje partido en lineas y dos botones), aparece con escala y
 * fundido como en iOS. Devuelve 1 si se pulsa el boton de aceptar. */
static int ss_alert(const char *title, const char *msg, const char *ok, int destructive)
{
    static uint16_t base[W * H]; memcpy(base, fr0, sizeof base);
    const int aw = 270, bh = 44;
    int th = draw_wrapped(0, 0, 0, aw - 32, title, 17, 0, 22, 0), mh = msg ? draw_wrapped(0, 0, 0, aw - 32, msg, 13, 0, 17, 0) : 0;
    int ah = 20 + th + (msg ? 4 + mh : 0) + 18 + bh, ax = (W - aw) / 2, ay = (H - ah) / 2;
    int sel = -1, res = -1;
    double t0 = now_ms(), tc = 0;
    struct pkt p; while (read_pkt(&p, 0)) {}
    while (cur_down && read_pkt(&p, 3000)) {}
    for (;;) {
        double t = now_ms();
        float e = tc ? 1 - (float)ease_out((t - tc) / 160.0 > 1 ? 1 : (t - tc) / 160.0) : (float)ease_out((t - t0) / 220.0 > 1 ? 1 : (t - t0) / 220.0);
        float sc = tc ? 1 : 1.12f - 0.12f * e;                       /* entra encogiendo hasta 1 */
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(base[i], 32 - (int)(13 * e));
        /* tarjeta a escala: se dibuja en su sitio y, si sc > 1, crece desde el centro */
        int w2 = (int)(aw * sc), h2 = (int)(ah * sc), x2 = (W - w2) / 2, y2 = (H - h2) / 2;
        int al = (int)(250 * e);
        if (al > 8) {
            fill_rrect(fr0, x2, y2, w2, h2, 14, rgb(44, 44, 46), al);
            if (e > 0.6f) {
                wrap_cw = aw - 32;
                int y = ay + 20;
                y += draw_wrapped(fr0, ax + 16, y - 4, aw - 32, title, 17, 0xFFFF, 22, 1);
                if (msg) { y += 4; draw_wrapped(fr0, ax + 16, y - 3, aw - 32, msg, 13, rgb(235, 235, 245), 17, 1); }
                wrap_cw = 0;
                int by = ay + ah - bh;
                for (int x = ax; x < ax + aw; x++) fr0[by * W + x] = rgb(70, 70, 74);
                for (int y3 = by; y3 < ay + ah; y3++) fr0[y3 * W + ax + aw / 2] = rgb(70, 70, 74);
                if (sel >= 0) fill_rrect(fr0, ax + (sel ? aw / 2 + 1 : 0), by + 1, aw / 2 - 1, bh - 1, 0, rgb(64, 64, 68), 255);
                const char *cn = tr("Cancelar");
                draw_text(fr0, ax + aw / 4 - tw_n(cn, 17, -1) / 2, by + 28, cn, 17, rgb(10, 132, 255), aw / 2 - 8, 0, H);
                int ow = tw_n(ok, 17, -1); if (ow > aw / 2 - 8) ow = aw / 2 - 8;
                draw_text(fr0, ax + 3 * aw / 4 - ow / 2, by + 28, ok, 17, destructive ? rgb(255, 69, 58) : rgb(10, 132, 255), aw / 2 - 8, 0, H);
            }
        }
        present();
        if (tc && t - tc >= 160) return res == 1;
        if (tc || t - t0 < 220) continue;
        if (!read_pkt(&p, 300) || !p.down) continue;
        int x = p.x, y = p.y, by = ay + ah - bh;
        if (y >= by && y < ay + ah && x >= ax && x < ax + aw) {
            sel = x >= ax + aw / 2;
            while (cur_down && read_pkt(&p, 3000)) {}
            res = sel; tc = now_ms();
        } else while (cur_down && read_pkt(&p, 3000)) {}
    }
}

/* ------------------------------------------------------------------ valores y acciones */
#define ROWI(t, f, v, ic) (struct srow){ t, R_NAV, 0, 0, 0, f, v, 0, 0, 0, 0, ic }
#define ROWT(t, iv, f, dis) (struct srow){ t, R_TOGGLE, iv, 0, 0, f, 0, 0, 0, 0, dis, 0 }
#define ROWC(t, iv, op, no, f, dis) (struct srow){ t, R_CHOICE, iv, op, no, f, 0, 0, 0, 0, dis, 0 }
#define ROWS(t, iv, lo, hi, st, f, dis) (struct srow){ t, R_SLIDER, iv, 0, st, f, 0, 0, lo, hi, dis, 0 }
#define ROWB(t, iv, v) (struct srow){ t, R_BAR, iv, 0, 0, 0, v, 0, 0, 0, 0, 0 }
#define ROWV(t, v) ROW(t, R_INFO, 0, 0, 0, 0, v, 0)

static const char *v_onoff(int k) { return tr(ps_v[k] ? "Activado" : "Desactivado"); }
static const char *v_wifi(void) { return v_onoff(PS_WIFI); }
static const char *v_bt(void) { return v_onoff(PS_BT); }
static const char *v_usbmode(void) { return !ps_ok[PS_USB] ? 0 : tr(ps_v[PS_USB] == 0 ? "DAC USB" : ps_v[PS_USB] == 1 ? "Almacenamiento" : "Dock"); }

/* opciones con numeros: el texto se rehace en el idioma actual cada vez que se dibuja */
static const int V_SLEEP[] = { 0, 15, 30, 45, 60, 90, 120 };      /* minutos (0 = desactivado) */
static const int V_IDLE[] = { 0, 1, 2, 3, 5, 10 };                /* minutos (0 = nunca) */
static const int V_BKL[] = { 0, 10, 20, 30, 60, 120 };            /* segundos (0 = nunca) */
static char ss_lbl[3][8][24];
static const char *O_SLEEP[7], *O_IDLE[6], *O_BKL[6];
static void ss_mins(char *b, int m) { if (m >= 60 && m % 60 == 0 && m != 60) snprintf(b, 24, tr("%d h"), m / 60); else snprintf(b, 24, tr("%d min"), m); }
static void ss_labels(void)
{
    for (int i = 0; i < 7; i++) { if (i) ss_mins(ss_lbl[0][i], V_SLEEP[i]); O_SLEEP[i] = i ? ss_lbl[0][i] : "Desactivado"; }
    for (int i = 0; i < 6; i++) { if (i) ss_mins(ss_lbl[1][i], V_IDLE[i]); O_IDLE[i] = i ? ss_lbl[1][i] : "Nunca"; }
    for (int i = 0; i < 6; i++) { if (i) { if (V_BKL[i] < 60) snprintf(ss_lbl[2][i], 24, tr("%d s"), V_BKL[i]); else ss_mins(ss_lbl[2][i], V_BKL[i] / 60); } O_BKL[i] = i ? ss_lbl[2][i] : "Nunca"; }
}
static int nearest(const int *v, int n, int x) { int b = 1; for (int i = 1; i < n; i++) if (abs(v[i] - x) < abs(v[b] - x)) b = i; return b; }
static int ui_sleep_i, ui_idle_i, ui_bkl_i, ui_bright = 50;
static double ss_sleep_end;                 /* cuando se apagara (si lo programamos nosotros) */
static void ss_derive(void)                 /* indices de las listas a partir de los valores */
{
    ss_labels();
    ui_sleep_i = ps_v[PS_SLEEP] ? nearest(V_SLEEP, 7, ps_v[PS_SLEEPMIN]) : 0;
    ui_idle_i = ps_v[PS_IDLE] ? nearest(V_IDLE, 6, ps_v[PS_IDLEMIN]) : 0;
    ui_bkl_i = ps_v[PS_BKLON] ? nearest(V_BKL, 6, ps_v[PS_BKLTIME]) : 0;
    if (!ps_v[PS_SLEEP]) ss_sleep_end = 0;
}
static void ssa_sleep(void)
{
    if (ui_sleep_i > 0) { ps_set_now(PS_SLEEPMIN, V_SLEEP[ui_sleep_i]); ps_set_now(PS_SLEEP, 1); ss_sleep_end = (double)time(0) + V_SLEEP[ui_sleep_i] * 60.0; }
    else { ps_set_now(PS_SLEEP, 0); ss_sleep_end = 0; }
}
static void ssa_idle(void) { if (ui_idle_i > 0) { ps_set_now(PS_IDLEMIN, V_IDLE[ui_idle_i]); ps_set_now(PS_IDLE, 1); } else ps_set_now(PS_IDLE, 0); }
static void ssa_bkl(void) { if (ui_bkl_i > 0) { ps_set_now(PS_BKLTIME, V_BKL[ui_bkl_i]); ps_set_now(PS_BKLON, 1); } else ps_set_now(PS_BKLON, 0); }
static void ssa_bright(void)
{
    int b = ui_bright < 1 ? 1 : ui_bright > 100 ? 100 : ui_bright;
    if (pw_mode == PW_SAVE && b > 40) b = ui_bright = 40;
    int nb = b * 101 / 100; if (nb < 2) nb = 2;
    sysfs_put(BL_PATH, nb);                  /* al instante (como el centro de control) */
    if (!ps_unsup[PS_BRIGHT]) ps_set_now(PS_BRIGHT, b);   /* y para el reproductor (que lo reaplica al despertar) */
}
#define PSA(name, IDX) static void ssa_##name(void) { ps_apply_f(IDX, 0); }
PSA(wifi, PS_WIFI) PSA(bt, PS_BT) PSA(dlna, PS_DLNA) PSA(airplay, PS_AIRPLAY) PSA(hlink, PS_HLINK)
PSA(gain, PS_GAIN) PSA(bal, PS_BAL) PSA(playmode, PS_PLAYMODE) PSA(mseb, PS_MSEB) PSA(update, PS_UPDATE)
/* sin pausas y fundido se excluyen (el reproductor apaga el otro): se refleja ya */
static void ssa_gapless(void)
{
    pthread_mutex_lock(&ps_mu); if (ps_v[PS_GAPLESS] && ps_v[PS_FADE]) ps_v[PS_FADE] = ps_sh[PS_FADE] = 0; pthread_mutex_unlock(&ps_mu);
    ps_apply_f(PS_GAPLESS, 0);
}
static void ssa_fade(void)
{
    pthread_mutex_lock(&ps_mu); if (ps_v[PS_FADE] && ps_v[PS_GAPLESS]) ps_v[PS_GAPLESS] = ps_sh[PS_GAPLESS] = 0; pthread_mutex_unlock(&ps_mu);
    ps_apply_f(PS_FADE, 0);
}
static void ssa_lineout(void)
{
    if (ps_v[PS_LINEOUT]) {                  /* volumen fijo al maximo: como el aviso de HiBy */
        if (!ss_alert(tr("¿Activar la salida de línea?"), tr("LINEOUT_CONFIRM"), tr("Activar"), 1)) { ps_v[PS_LINEOUT] = 0; return; }
        ps_apply_f(PS_LINEOUT, HS_FORCE);
    } else ps_apply_f(PS_LINEOUT, 0);
}
static void ssa_usb(void)                    /* R_CHECK: ps_v ya tiene el modo elegido */
{
    int to = ps_v[PS_USB]; ps_v[PS_USB] = ps_sh[PS_USB];
    if (!ss_alert(tr("¿Cambiar el modo USB?"), tr("USB_CONFIRM"), tr("Cambiar"), 0)) return;
    ps_v[PS_USB] = to; ps_apply_f(PS_USB, HS_FORCE);
}
static void ssa_volwarn(void)
{
    if (!au_volwarn && !ss_alert(tr("¿Quitar el aviso de volumen?"), tr("VOLWARN_CONFIRM"), tr("Quitar aviso"), 1)) { au_volwarn = 1; return; }
    if (cfg_set("vol_warn_enable", au_volwarn)) toast(tr("No se pudo guardar"));
}
static void ssa_reset_sound(void)
{
    if (!ss_alert(tr("¿Restablecer los ajustes de sonido?"), tr("RESET_SOUND_HELP"), tr("Restablecer"), 1)) return;
    ps_set_now(PS_LINEOUT, 0); ps_set_now(PS_GAIN, 0); ps_set_now(PS_BAL, 0); ps_set_now(PS_MSEB, 0);
    ps_set_now(PS_FADE, 0); ps_set_now(PS_GAPLESS, 1);
    au_filt_i = 0; apply_filter();
    toast(tr("Ajustes de sonido restablecidos"));
}

/* conexion USB: sysfs como mucho cada 0,5 s (las paginas se dibujan en cada vuelta) */
static const char *v_usb_state(void)
{
    static double t; static int on;
    if (now_ms() - t > 500) { t = now_ms(); on = sysfs_get(USB_ONLINE, 0); }
    return tr(on ? "Conectado" : "Sin conectar");
}
static char ss_buf[12][64];
static const char *ss_count(int i, int n) { if (!si_ui.lib) return "…"; snprintf(ss_buf[i], 64, "%d", n); return ss_buf[i]; }
static const char *v_songs(void) { return ss_count(0, si_ui.songs); }
static const char *v_albums(void) { return ss_count(1, si_ui.albums); }
static const char *v_artists(void) { return ss_count(2, si_ui.artists); }
static const char *v_ver(void) { return si_ui.ver[0] ? si_ui.ver : si_ui.ready ? "?" : "…"; }
static const char *v_model(void) { return "HiBy R3 II"; }
static const char *v_mem2(void) { if (!si_ui.ready) return "…"; snprintf(ss_buf[3], 64, "%.0f MB", si_ui.mem_kb / 1024.0); return ss_buf[3]; }
static const char *v_napps(void) { snprintf(ss_buf[4], 64, "%d", si_ui.napps); return si_ui.ready ? ss_buf[4] : "…"; }
static const char *v_mac(void) { return si_ui.mac[0] ? si_ui.mac : si_ui.ready ? "—" : "…"; }
static void ss_gb(char *b, long long v) { double g = v / 1e9; snprintf(b, 16, g >= 100 ? "%.0f GB" : g >= 10 ? "%.1f GB" : "%.2f GB", g); }
static const char *ss_space(int i, long long fr, long long tot)
{
    if (!si_ui.ready) return "…";
    if (!tot) return tr("Sin tarjeta");
    char a[16], b[16]; ss_gb(a, fr); ss_gb(b, tot); snprintf(ss_buf[i], 64, tr("%s libres de %s"), a, b); return ss_buf[i];
}
static const char *v_sd(void) { return ss_space(5, si_ui.sd_free, si_ui.sd_tot); }
static const char *v_int(void) { return ss_space(6, si_ui.in_free, si_ui.in_tot); }
static int ui_sd_pm, ui_in_pm;
static const char *v_repo2(void) { return "Grohle/hibyos"; }

/* ------------------------------------------------------------------ Wi-Fi y Bluetooth
 * HOOK wireless_app: la app Inalambrico propia (src/wireless.inc.c, de otro agente) se usa si
 * existe al compilar; hibyshell.c tiene que incluir wireless.inc.c (si el fichero existe y no
 * se incluye, el enlazado falla: compilar con -DSS_NO_WIRELESS_APP). Sin ella: paginas
 * sencillas con el interruptor (la eleccion de red sigue en la app Inalambrico). */
#if defined(__has_include) && !defined(SS_NO_WIRELESS_APP)
#if __has_include("wireless.inc.c")
#define SS_HAVE_WIRELESS 1
static void wireless_app(void);
#endif
#endif
static void page_wifi(struct spage *p)
{
    *p = (struct spage){ "Wi-Fi" };
    p->secs[0] = (struct ssec){ 0, "WIFI_FALLBACK_HELP", { ROWT("Wi-Fi", &ps_v[PS_WIFI], ssa_wifi, 0) }, 1 };
    p->nsec = 1;
}
static void page_bt(struct spage *p)
{
    *p = (struct spage){ "Bluetooth" };
    p->secs[0] = (struct ssec){ 0, "BT_FALLBACK_HELP", { ROWT("Bluetooth", &ps_v[PS_BT], ssa_bt, 0) }, 1 };
    p->nsec = 1;
}
#ifdef SS_HAVE_WIRELESS
static void nav_wifi(void) { wireless_app(); }
static void nav_bt(void) { wireless_app(); }
#else
static void nav_wifi(void) { open_page(page_wifi, "Ajustes"); }
static void nav_bt(void) { open_page(page_bt, "Ajustes"); }
#endif

/* ------------------------------------------------------------------ Streaming */
static void page_stream(struct spage *p)
{
    *p = (struct spage){ "Streaming" };
    if (ps_v[PS_WIFI])
        p->secs[0] = (struct ssec){ "AIRPLAY Y DLNA", "STREAM_HELP", { ROWT("AirPlay", &ps_v[PS_AIRPLAY], ssa_airplay, 0),
                                                                     ROWT("DLNA", &ps_v[PS_DLNA], ssa_dlna, 0) }, 2 };
    else p->secs[0] = (struct ssec){ "AIRPLAY Y DLNA", "STREAM_NOWIFI_HELP", { ROWI("Wi-Fi", nav_wifi, v_wifi, 0) }, 1 };
    p->secs[1] = (struct ssec){ "HIBY LINK", "HLINK_HELP", { ROWT("HiBy Link", &ps_v[PS_HLINK], ssa_hlink, 0) }, 1 };
    p->nsec = 2;
}
static void nav_stream(void) { open_page(page_stream, "Ajustes"); }

/* ------------------------------------------------------------------ USB (y la app DAC USB) */
static void nav_root_from_usb(void);
static void ss_usb_secs(struct spage *p)
{
    p->secs[0] = (struct ssec){ "MODO USB", "USB_HELP", { ROW("DAC USB", R_CHECK, &ps_v[PS_USB], 0, 0, ssa_usb, 0, 0),
                                                          ROW("Almacenamiento", R_CHECK, &ps_v[PS_USB], 0, 1, ssa_usb, 0, 0) }, 2 };
    p->secs[1] = (struct ssec){ 0, "NOCHARGE_HELP", { ROWV("Estado", v_usb_state), ROWT("Solo USB (no cargar)", &set_nocharge, apply_nocharge, 0) }, 2 };
    p->nsec = 2;
}
static void page_usb(struct spage *p) { *p = (struct spage){ "USB" }; ss_usb_secs(p); }
static void page_usb_root(struct spage *p)
{
    *p = (struct spage){ "DAC USB" }; ss_usb_secs(p);
    p->secs[2] = (struct ssec){ 0, 0, { ROWI("Todos los ajustes", nav_root_from_usb, 0, 0) }, 1 };
    p->nsec = 3;
}
static void nav_usb(void) { set_nocharge = pw_nocharge; open_page(page_usb, "Ajustes"); }

/* ------------------------------------------------------------------ Pantalla y brillo */
static void page_display(struct spage *p)
{
    ss_derive();
    *p = (struct spage){ "Pantalla y brillo" };
    p->secs[0] = (struct ssec){ "BRILLO", pw_mode == PW_SAVE ? "BRIGHT_SAVE_HELP" : 0, { ROWS("Brillo", &ui_bright, 1, 100, SL_BRIGHT, ssa_bright, 0) }, 1 };
    p->secs[1] = (struct ssec){ 0, ps_unsup[PS_BKLON] ? "PS_NEEDLIB_HELP" : "AUTOLOCK_HELP",
                                { ROWC("Bloqueo automático", &ui_bkl_i, O_BKL, 6, ssa_bkl, ps_unsup[PS_BKLON] || ps_unsup[PS_BKLTIME]) }, 1 };
    p->nsec = 2;
}
static void nav_display(void)
{
    int b = sysfs_get(BL_PATH, -1); ui_bright = b < 0 ? (ps_ok[PS_BRIGHT] ? ps_v[PS_BRIGHT] : 50) : b * 100 / 101;
    if (ui_bright < 1) ui_bright = 1; if (ui_bright > 100) ui_bright = 100;
    open_page(page_display, "Ajustes");
}

/* ------------------------------------------------------------------ Sonido */
static const char *const O_GAIN[] = { "Baja", "Alta" };
static const char *const O_PLAYMODE[] = { "En orden", "Repetir canción", "Aleatorio", "Repetir lista" };
static void page_sound(struct spage *p)
{
    *p = (struct spage){ "Sonido" };
    int n; filt_vals(&n);
    int mseb_off = ps_ok[PS_SPEED] && ps_v[PS_SPEED] != 100 && !ps_v[PS_MSEB];
    p->secs[0] = (struct ssec){ "SALIDA", ps_api < 0 ? "PS_NOAPI_HELP" : "LINEOUT_HELP", {
        ROWC("Ganancia", &ps_v[PS_GAIN], O_GAIN, 2, ssa_gain, ps_v[PS_LINEOUT]),
        ROWT("Salida de línea", &ps_v[PS_LINEOUT], ssa_lineout, 0) }, 2 };
    p->secs[1] = (struct ssec){ "BALANCE", 0, { ROWS("Balance", &ps_v[PS_BAL], -10, 10, SL_BALANCE, ssa_bal, 0) }, 1 };
    p->secs[2] = (struct ssec){ "REPRODUCCIÓN", "GAPLESS_HELP", {
        ROWC("Modo de reproducción", &ps_v[PS_PLAYMODE], O_PLAYMODE, 4, ssa_playmode, 0),
        ROWT("Sin pausas", &ps_v[PS_GAPLESS], ssa_gapless, 0), ROWT("Fundido", &ps_v[PS_FADE], ssa_fade, 0) }, 3 };
    p->secs[3] = (struct ssec){ "DAC ES9219", au_filt_max >= 7 ? "FILT_HELP" : "FILT_HELP_OLD", {
        ROW("Filtro digital", R_CHOICE, &au_filt_i, n == 7 ? O_FILT8 : O_FILT2, n, apply_filter, 0, 0) }, 1 };
    p->secs[4] = (struct ssec){ "MSEB", "MSEB_HELP", { ROWT("MSEB", &ps_v[PS_MSEB], ssa_mseb, mseb_off) }, 1 };
    p->secs[5] = (struct ssec){ "VOLUMEN", "VOLWARN_HELP", { ROWT("Aviso de volumen alto (UE)", &au_volwarn, ssa_volwarn, 0) }, 1 };
    if (au_reboot) { p->secs[5].rows[1] = ROW("Reiniciar para aplicar", R_ACTION, 0, 0, 0, act_reboot, 0, 0); p->secs[5].n = 2; }
    p->nsec = 6;
}
static void nav_sound(void) { audio_sync(); open_page(page_sound, "Ajustes"); }

/* ------------------------------------------------------------------ Biblioteca de musica */
static void page_library(struct spage *p)
{
    si_snap();
    *p = (struct spage){ "Biblioteca de música" };
    p->secs[0] = (struct ssec){ 0, 0, { ROWV("Canciones", v_songs), ROWV("Álbumes", v_albums), ROWV("Artistas", v_artists) }, 3 };
    p->secs[1] = (struct ssec){ "ACTUALIZACIÓN", ps_unsup[PS_UPDATE] ? "PS_NEEDLIB_HELP" : "UPDATE_HELP", {
        ROWT("Buscar música nueva automáticamente", &ps_v[PS_UPDATE], ssa_update, ps_unsup[PS_UPDATE]) }, 1 };
    p->secs[2] = (struct ssec){ "TARJETA SD", "TF_HELP", { ROWT("Biblioteca en la tarjeta SD", &au_tfdb, apply_tfdb, 0),
                                                         ROWT("Carátulas en caché en la SD", &au_tfimg, apply_tfimg, 0) }, 2 };
    if (au_reboot) { p->secs[2].rows[2] = ROW("Reiniciar para aplicar", R_ACTION, 0, 0, 0, act_reboot, 0, 0); p->secs[2].n = 3; }
    p->nsec = 3;
}
static void nav_library(void) { si_refresh(); audio_sync(); open_page(page_library, "Ajustes"); }

/* ------------------------------------------------------------------ Informacion */
static void page_about(struct spage *p)
{
    si_snap();
    ui_sd_pm = si_ui.sd_tot ? (int)((si_ui.sd_tot - si_ui.sd_free) * 1000 / si_ui.sd_tot) : 0;
    ui_in_pm = si_ui.in_tot ? (int)((si_ui.in_tot - si_ui.in_free) * 1000 / si_ui.in_tot) : 0;
    *p = (struct spage){ "Información" };
    p->hero = 96;
    p->secs[0] = (struct ssec){ 0, 0, { ROWV("Nombre del modelo", v_model), ROWV("Versión de HiByOS", v_ver), ROWV("Canciones", v_songs),
                                        ROWV("Álbumes", v_albums), ROWV("Apps instaladas", v_napps) }, 5 };
    p->secs[1] = (struct ssec){ "ALMACENAMIENTO", 0, { ROWB("Tarjeta SD", &ui_sd_pm, v_sd), ROWB("Memoria interna", &ui_in_pm, v_int),
                                                       ROWV("Memoria libre", v_mem2) }, 3 };
    p->secs[2] = (struct ssec){ 0, 0, { ROWV("Dirección Wi-Fi", v_mac) }, 1 };
    p->secs[3] = (struct ssec){ "PROYECTO", "ABOUT_HELP", { ROWV("GitHub", v_repo2) }, 1 };
    p->nsec = 4;
}
static void nav_about(void) { si_refresh(); open_page(page_about, "General"); }

/* ------------------------------------------------------------------ Fecha y hora */
static const char *v_date(void) { lang_date(time(0), ss_buf[7], 64); return ss_buf[7]; }
static const char *v_clock(void)
{
    time_t t = time(0); struct tm tm; localtime_r(&t, &tm);
    snprintf(ss_buf[8], 64, "%02d:%02d", tm.tm_hour, tm.tm_min); return ss_buf[8];
}
static const char *v_tz(void)
{
    static char z[64]; static int done;
    if (!done) {                              /* una vez: el enlace no cambia mientras corre */
        done = 1; char l[200]; ssize_t n = readlink("/usr/data/localtime", l, sizeof l - 1);
        if (n <= 0) n = readlink("/etc/localtime", l, sizeof l - 1);
        if (n > 0) { l[n] = 0; char *q = strstr(l, "zoneinfo/"); snprintf(z, sizeof z, "%s", q ? q + 9 : l); }
        else { FILE *f = fopen("/etc/timezone", "r"); if (f) { if (fgets(z, sizeof z, f)) z[strcspn(z, "\n")] = 0; fclose(f); } }
        for (char *c = z; *c; c++) if (*c == '_') *c = ' ';
    }
    return z[0] ? z : "—";
}
static void ss_time_picker(void);
static void page_datetime(struct spage *p)
{
    *p = (struct spage){ "Fecha y hora" };
    p->secs[0] = (struct ssec){ 0, 0, { ROWV("Fecha", v_date), ROWV("Hora", v_clock), ROWV("Zona horaria", v_tz) }, 3 };
    p->secs[1] = (struct ssec){ 0, "DATETIME_HELP", { ROW("Ajustar fecha y hora", R_ACTION, 0, 0, 0, ss_time_picker, 0, 0) }, 1 };
    p->nsec = 2;
}
static void nav_datetime(void) { open_page(page_datetime, "General"); }

/* Selector de ruedas (como el de iOS): dia, mes, ano | hora, minuto. Arrastre 1:1, inercia y
 * ajuste suave a la fila; las ruedas giran en bucle salvo el ano. "Guardar" fija el reloj del
 * sistema y el del aparato (hwclock -w --utc, como hace el reproductor). */
#define WP_RH 36
#define WP_CY 262
struct wheel { int x, w, lo, hi, loop; float pos, vel, tg; int has_tg; };
static char wp_mon[13][24];          /* nombres cortos de los meses en el idioma actual */
static int wp_mdays(int m, int y) { static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[(m - 1) % 12]; }
static int wp_val(const struct wheel *w)
{
    int n = w->hi - w->lo + 1, i = (int)floorf(w->pos + 0.5f);
    if (w->loop) { i %= n; if (i < 0) i += n; } else { if (i < 0) i = 0; if (i >= n) i = n - 1; }
    return w->lo + i;
}
static void wp_draw(uint16_t *d, struct wheel *wh, int nw, int sel_btn)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    fill_rrect(d, 12, WP_CY - WP_RH / 2, W - 24, WP_RH, 8, rgb(44, 44, 46), 255);       /* franja elegida */
    for (int k = 0; k < nw; k++) {
        struct wheel *w = &wh[k]; int n = w->hi - w->lo + 1;
        int base = (int)floorf(w->pos);
        for (int r = base - 4; r <= base + 5; r++) {
            float off = r - w->pos; float y = WP_CY + off * WP_RH;
            if (y < NAVH + 30 + 10 || y > H - 20) continue;
            int i = r; if (w->loop) { i %= n; if (i < 0) i += n; } else if (i < 0 || i >= n) continue;
            int v = w->lo + i; char b[24];
            if (k == 1) snprintf(b, sizeof b, "%s", wp_mon[v][0] ? wp_mon[v] : "?");
            else if (k >= 3) snprintf(b, sizeof b, "%02d", v); else snprintf(b, sizeof b, "%d", v);
            float a = 1 - fabsf(off) * 0.28f; if (a < 0.12f) a = 0.12f;
            int sz = fabsf(off) < 0.5f ? 21 : 19;
            uint16_t c = sp_mix(0, 0xFFFF, a);
            int tw = tw_n(b, sz, -1);
            draw_text(d, w->x + (w->w - tw) / 2, (int)y + sz / 2 - 1, b, sz, c, w->w, NAVH + 30, H);
        }
    }
    /* desvanecido arriba y abajo de las ruedas (como el cilindro de iOS) */
    for (int y = NAVH + 30; y < WP_CY - 3 * WP_RH && y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = 0;
    for (int y = WP_CY + 3 * WP_RH; y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = 0;
    draw_text(d, wh[3].x + wh[3].w - 2, WP_CY + 8, ":", 21, 0xFFFF, 0, 0, H);
    /* barra: Cancelar | titulo | Guardar */
    memcpy(d, fr1, 30 * W * 2);
    for (int y = 30; y < NAVH + 30; y++) for (int x = 0; x < W; x++) d[y * W + x] = rgb(12, 12, 13);
    for (int x = 0; x < W; x++) d[(NAVH + 29) * W + x] = rgb(38, 38, 40);
    const char *cn = tr("Cancelar"), *sv = tr("Guardar"), *ti = tr("Fecha y hora");
    draw_text(d, 16, 68, cn, 16, sel_btn == 0 ? rgb(10, 80, 160) : rgb(10, 132, 255), 100, 0, H);
    int sw = tw_n(sv, 16, -1); if (sw > 100) sw = 100;
    draw_text(d, W - 16 - sw, 68, sv, 16, sel_btn == 1 ? rgb(10, 80, 160) : rgb(10, 132, 255), 100, 0, H);
    int tw = tw_n(ti, 17, -1); if (tw > 110) tw = 110;
    draw_text(d, (W - tw) / 2, 68, ti, 17, 0xFFFF, 110, 0, H);
    draw_toast(d);
}
static void ss_time_picker(void)
{
    for (int m = 0; m < 12; m++) {                            /* nombres de mes cortos del idioma */
        struct tm q = { 0 }; q.tm_year = 126; q.tm_mon = m; q.tm_mday = 15; q.tm_hour = 12;
        char full[64]; lang_date(mktime(&q), full, sizeof full);
        /* "15 ene 2026" / "Jan 15, 2026" / "2026年1月15日": el mes es la palabra sin cifras */
        char *best = 0, *sv = 0; int bl = 0;
        for (char *w = strtok_r(full, " ,", &sv); w; w = strtok_r(0, " ,", &sv)) {
            int dig = 0; for (char *c = w; *c; c++) if (isdigit((unsigned char)*c)) dig = 1;
            if (!dig && (int)strlen(w) > bl) { best = w; bl = (int)strlen(w); }
        }
        if (best) snprintf(wp_mon[m + 1], sizeof wp_mon[0], "%s", best); else snprintf(wp_mon[m + 1], sizeof wp_mon[0], "%d", m + 1);
    }
    time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
    struct wheel wh[5] = {
        { 14, 46, 1, 31, 1, (float)(tm.tm_mday - 1), 0 }, { 60, 92, 1, 12, 1, (float)tm.tm_mon, 0 },
        { 152, 64, 2024, 2040, 0, (float)(tm.tm_year + 1900 - 2024), 0 },
        { 226, 40, 0, 23, 1, (float)tm.tm_hour, 0 }, { 268, 40, 0, 59, 1, (float)tm.tm_min, 0 } };
    if (wh[2].pos < 0) wh[2].pos = 0; if (wh[2].pos > 16) wh[2].pos = 16;
    memcpy(img_under, fr0, HM_FRAME_BYTES); have_under = 0;
    wp_draw(img_base, wh, 5, -1); fs_transition(img_under, img_base, 1, 0);
    int save = 0, act = -1; double lt = now_ms(); float ly = 0;
    for (;;) {
        /* inercia y ajuste a la fila de las ruedas sueltas */
        double t = now_ms(); float dt = (float)(t - lt) / 1000.f; lt = t; if (dt > 0.05f) dt = 0.05f;
        int moving = 0;
        for (int k = 0; k < 5; k++) {
            struct wheel *w = &wh[k]; if (k == act) continue;
            if (fabsf(w->vel) > 0.6f) { w->pos += w->vel * dt; w->vel *= powf(0.06f, dt); moving = 1; }
            else { float tg = w->has_tg ? w->tg : floorf(w->pos + 0.5f); w->vel = 0; float d = tg - w->pos; if (fabsf(d) > 0.002f) { w->pos += d * (1 - powf(0.0005f, dt)); moving = 1; } else w->pos = tg; }
            if (!w->loop) { float mx = (float)(w->hi - w->lo); if (w->pos < 0) { w->pos = 0; w->vel = 0; } if (w->pos > mx) { w->pos = mx; w->vel = 0; } }
        }
        /* el dia no puede pasar del ultimo del mes */
        int md = wp_mdays(wp_val(&wh[1]), wp_val(&wh[2]));
        if (act != 0 && fabsf(wh[0].vel) < 0.6f && wp_val(&wh[0]) > md) { wh[0].pos = (float)(md - 1); }
        wp_draw(fr0, wh, 5, -1); present();
        struct pkt p;
        if (!read_pkt(&p, moving || act >= 0 ? 0 : (now_ms() < toast_until + 40 ? 30 : 400))) continue;
        if (!p.down) continue;
        int sx = p.x, sy = p.y;
        if (sy < NAVH + 30) {                                  /* botones de la barra */
            int b = sx < 110 ? 0 : sx > W - 110 ? 1 : -1;
            if (b >= 0) { wp_draw(fr0, wh, 5, b); present(); }
            while (cur_down && read_pkt(&p, 3000)) {}
            if (b == 0) break;
            if (b == 1) { save = 1; break; }
            continue;
        }
        if (sy >= 456 && sp_self >= 0) {                       /* desde abajo: inicio */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up && own_home_gesture(sx, sy, sp_self)) { sp_gone = 1; return; }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        { int bk = app_back_gesture(sx, sy); if (bk > 0) break; if (bk < 0) continue; }
        act = -1; for (int k = 0; k < 5; k++) if (sx >= wh[k].x - 4 && sx < wh[k].x + wh[k].w + 4) act = k;
        if (act < 0) { while (cur_down && read_pkt(&p, 3000)) {} continue; }
        struct wheel *w = &wh[act]; w->vel = 0; w->has_tg = 0; float p0 = w->pos; ly = (float)sy; double lt2 = now_ms(); float v = 0; int moved = 0;
        while (cur_down) {
            int got = read_pkt(&p, 20); while (got && p.down && read_pkt(&p, 0)) {}
            double t2 = now_ms(); if (t2 > lt2) { v = 0.6f * v + 0.4f * (ly - cur_y) / WP_RH / (float)((t2 - lt2) / 1000.0); ly = (float)cur_y; lt2 = t2; }
            if (abs(cur_y - sy) > 4) moved = 1;
            w->pos = p0 + (sy - cur_y) / (float)WP_RH;
            if (!w->loop) { float mx = (float)(w->hi - w->lo); if (w->pos < -0.4f) w->pos = -0.4f; if (w->pos > mx + 0.4f) w->pos = mx + 0.4f; }
            wp_draw(fr0, wh, 5, -1); present();
            if (got && !p.down) break;
        }
        if (now_ms() - lt2 > 80) v = 0;
        if (!moved) {                                          /* toque en una fila: girar hasta ella */
            int r = (int)floorf((sy - WP_CY + WP_RH / 2) / (float)WP_RH);
            w->tg = floorf(w->pos + 0.5f) + r; w->has_tg = 1;
            if (!w->loop) { float mx = (float)(w->hi - w->lo); if (w->tg < 0) w->tg = 0; if (w->tg > mx) w->tg = mx; }
        } else w->vel = v > 40 ? 40 : v < -40 ? -40 : v;
        act = -1;
    }
    if (save) {
        struct tm n = { 0 };
        n.tm_mday = wp_val(&wh[0]); n.tm_mon = wp_val(&wh[1]) - 1; n.tm_year = wp_val(&wh[2]) - 1900;
        n.tm_hour = wp_val(&wh[3]); n.tm_min = wp_val(&wh[4]); n.tm_sec = 0; n.tm_isdst = -1;
        int md = wp_mdays(n.tm_mon + 1, n.tm_year + 1900); if (n.tm_mday > md) n.tm_mday = md;
        time_t t = mktime(&n);
        struct timeval tv = { t, 0 };
        if (t != (time_t)-1 && !settimeofday(&tv, 0)) {
            char *argv[] = { "/sbin/hwclock", "-w", "--utc", 0 };
            if (access(argv[0], X_OK)) argv[0] = "/bin/hwclock";
            pid_t pid; posix_spawn(&pid, argv[0], 0, 0, argv, environ);   /* asincrono */
            toast(tr("Hora guardada"));
        } else toast(tr("No se pudo cambiar la hora"));
    }
    /* volver: la pagina de debajo la vuelve a dibujar open_page */
    wp_draw(img_under, wh, 5, -1); have_under = 0;
}

/* ------------------------------------------------------------------ General y Restablecer */
static void page_reset(struct spage *p)
{
    *p = (struct spage){ "Restablecer" };
    p->secs[0] = (struct ssec){ 0, "RESET_SOUND_HELP", { ROW("Restablecer ajustes de sonido", R_ACTION, 0, 0, 0, ssa_reset_sound, 0, 0) }, 1 };
    p->secs[1] = (struct ssec){ 0, 0, { ROW("Restablecer inicio", R_ACTION, 0, 0, 0, act_reset, 0, 0) }, 1 };
    p->nsec = 2;
}
static void nav_reset(void) { open_page(page_reset, "General"); }
static const char *ss_sleep_footer(void)
{
    if (!ps_v[PS_SLEEP] || !ss_sleep_end) return "SLEEP_HELP";
    time_t e = (time_t)ss_sleep_end; struct tm tm; localtime_r(&e, &tm);
    char hm[8]; snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(ss_buf[9], 64, tr("Se apagará a las %s."), hm); return ss_buf[9];
}
static void page_general(struct spage *p)
{
    ss_derive();
    *p = (struct spage){ "General" };
    p->secs[0] = (struct ssec){ 0, 0, { ROWI("Información", nav_about, 0, 0), ROWI("Actualización de software", nav_ota, 0, 0) }, 2 };
    p->secs[1] = (struct ssec){ 0, 0, { ROWI("Fecha y hora", nav_datetime, 0, 0), ROW("Idioma", R_NAV, 0, 0, 0, nav_lang, v_lang, 0) }, 2 };
    p->secs[2] = (struct ssec){ "APAGADO AUTOMÁTICO", ss_sleep_footer(), {
        ROWC("Temporizador", &ui_sleep_i, O_SLEEP, 7, ssa_sleep, 0),
        ROWC("Por inactividad", &ui_idle_i, O_IDLE, 6, ssa_idle, ps_unsup[PS_IDLE] || ps_unsup[PS_IDLEMIN]) }, 2 };
    p->secs[3] = (struct ssec){ 0, 0, { ROWI("Restablecer", nav_reset, 0, 0) }, 1 };
    p->nsec = 4;
}

/* ------------------------------------------------------------------ raiz */
static void page_root(struct spage *p)
{
    *p = (struct spage){ "Ajustes" };
    p->secs[0] = (struct ssec){ 0, 0, { ROWI("Wi-Fi", nav_wifi, v_wifi, IC_WIFI), ROWI("Bluetooth", nav_bt, v_bt, IC_BT),
                                        ROWI("Streaming", nav_stream, 0, IC_STREAM), ROWI("USB", nav_usb, v_usbmode, IC_USB) }, 4 };
    p->secs[1] = (struct ssec){ 0, 0, { ROWI("General", nav_general, 0, IC_GENERAL), ROWI("Pantalla y brillo", nav_display, 0, IC_DISPLAY),
                                        ROWI("Sonido", nav_sound, 0, IC_SOUND), ROWI("Batería y energía", nav_power, 0, IC_BATTERY) }, 4 };
    p->secs[2] = (struct ssec){ 0, 0, { ROWI("Biblioteca de música", nav_library, 0, IC_LIBRARY), ROWI("Inicio", nav_home, 0, IC_HOME),
                                        ROWI("Gestos", nav_gestures, 0, IC_GESTURE), ROWI("Centro de control", nav_cc, 0, IC_CC),
                                        ROWI("Teclado", nav_kb, 0, IC_KB) }, 5 };
    p->secs[3] = (struct ssec){ 0, 0, { ROWI("Desarrollador", nav_dev, 0, IC_DEV) }, 1 };
    p->nsec = 4;
}
static void nav_root_from_usb(void) { open_page(page_root, "DAC USB"); }
