/* hibybudsd - headphone control daemon of HiByOS (docs/HEADPHONES.md).
 *
 * Watches BlueZ for connected headphones, recognises brand and model, opens the vendor control
 * channel next to the A2DP stream (AirPods/Beats: L2CAP PSM 0x1001 "AAP"; Galaxy Buds: RFCOMM
 * on the Samsung SPP service found by SDP), keeps their state and publishes it for the shell:
 *   /tmp/hibybuds.state   state file (atomic rename), format in src/buds_core.inc.c
 *   /tmp/hibybuds.cmd     command FIFO (one line per command)
 *   /tmp/hibybuds.evt     one byte per state change (wakes the shell's reader thread)
 *
 * Rules (audio first): no inquiry, no scan, no discovery, ever. Only the existing ACL is used
 * (SDP query + one L2CAP/RFCOMM channel). BlueZ is watched with one dbus-monitor child (signals
 * only) and re-read with dbus-send only when a device property changes (plus once a minute).
 * Reconnection attempts back off (2 s .. 5 min) and stop after 6 failures until the device
 * reconnects. Idle CPU: zero (poll() with long timeouts).
 *
 *   hibybudsd                 normal (started by the shell; one instance, /tmp/hibybudsd.pid)
 *   hibybudsd --fake          simulated AirPods Pro 2 + Galaxy Buds2 Pro + AirPods Max (no BT)
 *   hibybudsd --matrix        print the supported-models matrix (docs/HEADPHONES.md)
 *   hibybudsd --list          print the recognised BlueZ devices and exit (read-only)
 *   hibybudsd --probe MAC     open the control channel of MAC, print the state for 15 s, exit
 *   -v                        verbose log (every packet in hex)
 * Environment: HIBYBUDS_DIR (state/cmd/evt/pid directory, default /tmp), HIBYBUDS_DATA. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "buds_all.inc.c"          /* brand modules + model catalog */
#include "buds_sim.inc.c"          /* simulated headphones (--fake) */

extern char **environ;
#define DBUS_SEND "/usr/bin/dbus-send"
#define DBUS_MON "/usr/bin/dbus-monitor"
#define SDPTOOL "/usr/bin/sdptool"

/* brand registry: other brands (Nothing, Sony, JBL...) add their struct buds_brand here */
#define BRANDS BUDS_BRANDS
#define NBRANDS BUDS_NBRANDS

/* ------------------------------------------------------------------ utilities */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double wall_s(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
static int verbose, fake, quit;
static char P_STATE[160], P_CMD[160], P_EVT[160], P_PIDF[160], P_DATA[160], P_LOG[160];
static FILE *lgf;
static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
    if (!lgf) return;
    static int lines;
    if (++lines > 4000) {                      /* /tmp is RAM: keep the log small */
        lines = 0; fclose(lgf); char old[200]; snprintf(old, sizeof old, "%s.1", P_LOG); rename(P_LOG, old);
        lgf = fopen(P_LOG, "a"); if (!lgf) return;
    }
    va_list a; va_start(a, fmt);
    fprintf(lgf, "%10.0f ", now_ms()); vfprintf(lgf, fmt, a); fputc('\n', lgf); fflush(lgf);
    va_end(a);
}
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }

/* run argv, stdout+stderr into out (NUL-terminated), kill after timeout. Exit code or -1 */
static int run(char *const argv[], char *out, int cap, int timeout_ms)
{
    int fd[2]; if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1); posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    int n = 0; double t0 = now_ms();
    if (!r) for (;;) {
        int left = timeout_ms - (int)(now_ms() - t0);
        if (left <= 0) { kill(pid, SIGKILL); break; }
        struct pollfd pf = { fd[0], POLLIN, 0 };
        if (poll(&pf, 1, left) <= 0) continue;
        int k = (int)read(fd[0], out + n, (size_t)(cap - 1 - n));
        if (k <= 0) break;
        n += k; if (n >= cap - 1) break;
    }
    close(fd[0]); out[n] = 0;
    if (r) return -1;
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void dev_path(const char *addr, char *p, int cap)
{
    char a[18]; buds_cpy(a, sizeof a, addr);
    for (char *c = a; *c; c++) if (*c == ':') *c = '_';
    snprintf(p, (size_t)cap, "/org/bluez/hci0/dev_%s", a);
}

/* ------------------------------------------------------------------ Bluetooth sockets
 * (musl has no bluetooth headers: the kernel ABI, as in linux/include/net/bluetooth) */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
#define BTPROTO_RFCOMM 3
#define SOL_BLUETOOTH 274
#define BT_SECURITY 4
#define BT_SECURITY_MEDIUM 2
typedef struct { uint8_t b[6]; } __attribute__((packed)) bt_addr;
struct bt_security_s { uint8_t level, key_size; };
struct sockaddr_l2_s { sa_family_t l2_family; unsigned short l2_psm; bt_addr l2_bdaddr; unsigned short l2_cid; uint8_t l2_bdaddr_type; };
struct sockaddr_rc_s { sa_family_t rc_family; bt_addr rc_bdaddr; uint8_t rc_channel; };
static int str2ba(const char *s, bt_addr *a)
{
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6) return -1;
    for (int i = 0; i < 6; i++) a->b[i] = (uint8_t)v[i];
    return 0;
}
/* non-blocking connect with a timeout; returns the socket (blocking mode restored) or -errno */
static int bt_connect(int type, int proto, const void *sa, socklen_t sl, int timeout_ms, int secure)
{
    int s = socket(AF_BLUETOOTH, type | SOCK_CLOEXEC, proto);
    if (s < 0) return -errno;
    if (secure) { struct bt_security_s sec = { BT_SECURITY_MEDIUM, 0 }; setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof sec); }
    int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (const struct sockaddr *)sa, sl);
    if (r < 0 && errno != EINPROGRESS) { int e = errno; close(s); return -e; }
    if (r < 0) {
        struct pollfd pf = { s, POLLOUT, 0 };
        if (poll(&pf, 1, timeout_ms) <= 0) { close(s); return -ETIMEDOUT; }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(s); return -err; }
    }
    fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    return s;
}
static int l2cap_open(const char *addr, int psm, int timeout_ms)
{
    struct sockaddr_l2_s a; memset(&a, 0, sizeof a);
    a.l2_family = AF_BLUETOOTH; a.l2_psm = (unsigned short)psm;          /* little-endian host (MIPSel/x86) */
    if (str2ba(addr, &a.l2_bdaddr)) return -EINVAL;
    return bt_connect(SOCK_SEQPACKET, BTPROTO_L2CAP, &a, sizeof a, timeout_ms, psm != 1);
}
static int rfcomm_open(const char *addr, int ch, int timeout_ms)
{
    struct sockaddr_rc_s a; memset(&a, 0, sizeof a);
    a.rc_family = AF_BLUETOOTH; a.rc_channel = (uint8_t)ch;
    if (str2ba(addr, &a.rc_bdaddr)) return -EINVAL;
    return bt_connect(SOCK_STREAM, BTPROTO_RFCOMM, &a, sizeof a, timeout_ms, 1);
}

/* ------------------------------------------------------------------ SDP (RFCOMM channel of a service) */
static int uuid_bytes(const char *s, uint8_t *u)
{
    int n = 0;
    for (; *s && n < 32; s++) {
        int c = *s, v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (c == '-') continue;
        if (v < 0) return -1;
        if (n & 1) u[n / 2] |= (uint8_t)v; else u[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    return n == 32 ? 0 : -1;
}
/* ServiceSearchAttributeRequest: pattern {uuid128}, attributes {0x0004 ProtocolDescriptorList} */
static int sdp_req(uint8_t *o, uint16_t tid, const uint8_t uuid[16], const uint8_t *cont, int contn)
{
    int k = 0;
    o[k++] = 0x06; o[k++] = (uint8_t)(tid >> 8); o[k++] = (uint8_t)tid; k += 2;   /* length later */
    o[k++] = 0x35; o[k++] = 17; o[k++] = 0x1C; memcpy(o + k, uuid, 16); k += 16;   /* DES { UUID128 } */
    o[k++] = 0xFF; o[k++] = 0xFF;                                                  /* max bytes */
    o[k++] = 0x35; o[k++] = 3; o[k++] = 0x09; o[k++] = 0x00; o[k++] = 0x04;         /* DES { uint16 0x0004 } */
    o[k++] = (uint8_t)contn; if (contn) { memcpy(o + k, cont, (size_t)contn); k += contn; }
    o[3] = (uint8_t)((k - 5) >> 8); o[4] = (uint8_t)(k - 5);
    return k;
}
/* one response PDU: appends the attribute bytes to acc; returns continuation length or -1 */
static int sdp_rsp(const uint8_t *p, int n, uint8_t *acc, int *accn, int cap, uint8_t *cont)
{
    if (n < 7 || p[0] != 0x07) return -1;
    int bc = p[5] << 8 | p[6];
    if (7 + bc + 1 > n) return -1;
    if (*accn + bc > cap) return -1;
    memcpy(acc + *accn, p + 7, (size_t)bc); *accn += bc;
    int cn = p[7 + bc];
    if (cn > 16 || 8 + bc + cn > n) return -1;
    memcpy(cont, p + 8 + bc, (size_t)cn);
    return cn;
}
/* data element header: type, value offset and value length; returns total length or -1 */
static int sdp_de(const uint8_t *p, int n, int *type, int *voff, int *vlen)
{
    if (n < 1) return -1;
    int t = p[0] >> 3, sz = p[0] & 7, l, o = 1;
    if (t == 0) l = 0;
    else if (sz <= 4) l = 1 << sz;
    else if (sz == 5) { if (n < 2) return -1; l = p[1]; o = 2; }
    else if (sz == 6) { if (n < 3) return -1; l = p[1] << 8 | p[2]; o = 3; }
    else { if (n < 5) return -1; l = (int)((uint32_t)p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4]); o = 5; }
    if (l < 0 || o + l > n) return -1;
    *type = t; *voff = o; *vlen = l;
    return o + l;
}
/* walks every element; after a UUID 0x0003 (RFCOMM) the next uint8 is the channel */
static int sdp_walk(const uint8_t *p, int n, int *after_rfcomm, int depth)
{
    for (int i = 0; i < n; ) {
        int t, vo, vl, len = sdp_de(p + i, n - i, &t, &vo, &vl);
        if (len < 0) return -1;
        const uint8_t *v = p + i + vo;
        if (t == 6 || t == 7) { if (depth < 8) { int c = sdp_walk(v, vl, after_rfcomm, depth + 1); if (c > 0) return c; } }
        else if (t == 3) {
            static const uint8_t base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB };
            unsigned u = vl == 2 ? (unsigned)(v[0] << 8 | v[1]) : vl == 4 ? (unsigned)(v[2] << 8 | v[3]) : vl == 16 && !v[0] && !v[1] && !memcmp(v + 4, base, 12) ? (unsigned)(v[2] << 8 | v[3]) : 0xFFFFFFFFu;
            *after_rfcomm = u == 0x0003;
        } else if (t == 1 && vl == 1 && *after_rfcomm) return v[0];
        else *after_rfcomm = 0;
        i += len;
    }
    return 0;
}
static int sdp_find_rfcomm(const uint8_t *p, int n) { int f = 0; int c = sdp_walk(p, n, &f, 0); return c > 0 && c <= 30 ? c : -1; }
static int sdp_channel(const char *addr, const char *uuid)
{
    uint8_t u[16]; if (uuid_bytes(uuid, u)) return -1;
    int s = l2cap_open(addr, 1, 6000);
    if (s < 0) { lg("%s: SDP: %s", addr, strerror(-s)); return -1; }
    static uint8_t acc[4096]; int accn = 0, ch = -1; uint8_t cont[17]; int contn = 0;
    for (int it = 0; it < 8; it++) {
        uint8_t q[64], r[1024]; int qn = sdp_req(q, (uint16_t)(it + 1), u, cont, contn);
        if (send(s, q, (size_t)qn, MSG_NOSIGNAL) != qn) break;
        struct pollfd pf = { s, POLLIN, 0 };
        if (poll(&pf, 1, 5000) <= 0) break;
        int rn = (int)recv(s, r, sizeof r, 0);
        if (rn <= 0) break;
        contn = sdp_rsp(r, rn, acc, &accn, sizeof acc, cont);
        if (contn < 0) break;
        if (!contn) { ch = sdp_find_rfcomm(acc, accn); break; }
    }
    close(s);
    return ch;
}
/* fallback: "sdptool browse MAC" text; the record that names the UUID and its "Channel: N" */
static int sdptool_channel_parse(const char *txt, const char *uuid)
{
    const char *rec = txt;
    for (;;) {
        const char *next = strstr(rec + 1, "Service Name:"), *alt = strstr(rec + 1, "Service RecHandle:");
        if (!next || (alt && alt < next)) next = alt;
        size_t len = next ? (size_t)(next - rec) : strlen(rec);
        char *r = strndup(rec, len);
        if (r) {
            for (char *c = r; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
            char lu[40]; buds_cpy(lu, sizeof lu, uuid); for (char *c = lu; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
            char *ch = strstr(r, "channel: ");
            int found = strstr(r, lu) && ch;
            int v = found ? atoi(ch + 9) : -1;
            free(r);
            if (found && v > 0 && v <= 30) return v;
        }
        if (!next) return -1;
        rec = next;
    }
}
static int sdptool_channel(const char *addr, const char *uuid)
{
    if (access(SDPTOOL, X_OK)) return -1;
    static char out[32768];
    char *argv[] = { SDPTOOL, "browse", (char *)addr, 0 };
    if (run(argv, out, sizeof out, 12000) != 0) return -1;
    return sdptool_channel_parse(out, uuid);
}

/* ------------------------------------------------------------------ devices */
struct dev {
    struct bud b;
    int used, bz_conn, bz_paired, fd, tries, rfch, sdp_tried, busy_job;
    double next_try, opened, last_rx, last_down;
    int ready_flag;               /* the first status of this connection was seen */
    struct bsim *sim;            /* --fake */
    double sim_conn_at;
};
static struct dev D[BUDS_MAXDEV];
static int dirty;
static struct buds_hdr HDR;

static void dev_log(struct bud *b, const char *m) { lg("%s: %s", b->s.addr, m); }
static int dev_tx(struct bud *b, const uint8_t *p, int n)
{
    struct dev *d = (struct dev *)b->io;
    if (verbose) { char h[200]; bud_hex(h, sizeof h, p, n > 60 ? 60 : n); lg("%s > %s", b->s.addr, h); }
    if (d->sim) { bsim_from_host(d->sim, p, n, b->m); return n; }
    if (d->fd < 0) return -1;
    int k = (int)send(d->fd, p, (size_t)n, MSG_NOSIGNAL);
    if (k != n) lg("%s: send: %s", b->s.addr, k < 0 ? strerror(errno) : "short");
    return k;
}
static struct dev *dev_find(const char *addr)
{
    for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && !strcasecmp(D[i].b.s.addr, addr)) return &D[i];
    return 0;
}
static struct dev *dev_add(const char *addr)
{
    struct dev *d = dev_find(addr);
    if (d) return d;
    for (int i = 0; i < BUDS_MAXDEV; i++) if (!D[i].used) {
        d = &D[i]; memset(d, 0, sizeof *d);
        d->used = 1; d->fd = -1; d->rfch = -1;
        buds_st_init(&d->b.s); buds_cpy(d->b.s.addr, sizeof d->b.s.addr, addr);
        d->b.tx = dev_tx; d->b.log = dev_log; d->b.io = d;
        return d;
    }
    return 0;
}
static void dev_close(struct dev *d, const char *why)
{
    if (d->fd >= 0) { close(d->fd); d->fd = -1; lg("%s: channel closed (%s)", d->b.s.addr, why); }
    struct bud *b = &d->b;
    b->rn = b->fragn = 0; b->ready = 0; b->mgr_sent = 0; b->ext_seen = 0; memset(b->unknown_ids, 0, sizeof b->unknown_ids);
    b->s.find[0] = b->s.find[1] = 0;
    b->s.conn = d->bz_conn ? BS_AUDIO : BS_NONE;
    if (d->ready_flag) d->last_down = now_ms();
    d->ready_flag = 0;
    if (!d->bz_conn) for (int i = 0; i < 2; i++) b->s.wear[i] = -1;
    dirty = 1;
}

/* ------------------------------------------------------------------ known devices (RFCOMM channel, refined model) */
static void known_load(void)
{
    char p[200]; snprintf(p, sizeof p, "%s/known.txt", P_DATA);
    FILE *f = fopen(p, "r"); if (!f) return;
    char l[200];
    while (fgets(l, sizeof l, f)) {
        char a[18]; int br, id, ch;
        if (sscanf(l, "%17s %d %d %d", a, &br, &id, &ch) != 4) continue;
        struct dev *d = dev_add(a); if (!d) break;
        d->rfch = ch;
        if (br == BB_APPLE && id) { const struct buds_model *m = aap_model_by_id((unsigned)id); if (m) { bud_set_model(&d->b, m); d->b.br = &BRAND_APPLE; } }
    }
    fclose(f);
}
static void known_save(void)
{
    char p[200], t[210]; snprintf(p, sizeof p, "%s/known.txt", P_DATA); snprintf(t, sizeof t, "%s.tmp", p);
    mkdir(P_DATA, 0755);
    FILE *f = fopen(t, "w"); if (!f) return;
    for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && D[i].b.br) fprintf(f, "%s %d %d %d\n", D[i].b.s.addr, D[i].b.s.brand, D[i].b.s.model_id, D[i].rfch);
    fclose(f); rename(t, p);
}

/* ------------------------------------------------------------------ state publication */
static int evt_fd = -1;
static void publish(void)
{
    HDR.gen++;
    char t[200]; snprintf(t, sizeof t, "%s.tmp", P_STATE);
    FILE *f = fopen(t, "w"); if (!f) { lg("state: %s", strerror(errno)); return; }
    buds_hdr_write(f, &HDR);
    for (int c = 1; c >= 0; c--)                                   /* connected first */
        for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && (D[i].b.br || D[i].b.s.brand == BB_OTHER) && ((D[i].b.s.conn != BS_NONE) == c)) {
            struct buds_st s = D[i].b.s;
            s.busy = D[i].busy_job;
            buds_st_write(f, &s);
        }
    fclose(f);
    rename(t, P_STATE);
    if (evt_fd >= 0) { char c = 'u'; if (write(evt_fd, &c, 1) < 0) {} }
    dirty = 0;
}
static void reply(unsigned seq, int code)
{
    if (seq > HDR.ack) HDR.ack = seq;
    if (code) { HDR.err = seq; HDR.err_code = code; lg("command %u: error %d", seq, code); }
    dirty = 1;
}

/* ------------------------------------------------------------------ BlueZ */
static char dbus_out[98304];
static double next_refresh, last_refresh;
static void bluez_refresh(void)
{
    static struct bd_dev bd[24];
    char *argv[] = { DBUS_SEND, "--system", "--print-reply", "--reply-timeout=3000", "--dest=org.bluez", "/",
                     "org.freedesktop.DBus.ObjectManager.GetManagedObjects", 0 };
    double t0 = now_ms();
    last_refresh = t0;
    if (run(argv, dbus_out, sizeof dbus_out, 5000) != 0) { lg("bluez: GetManagedObjects failed: %.80s", dbus_out); return; }
    int n = bd_bluez_parse(dbus_out, bd, 24);
    for (int i = 0; i < BUDS_MAXDEV; i++) D[i].bz_conn = -1;          /* -1 = not seen */
    for (int i = 0; i < n; i++) {
        const struct bd_dev *x = &bd[i];
        if (!x->paired && !x->connected) continue;
        const struct buds_model *m = 0; const struct buds_brand *br = 0;
        for (int k = 0; k < NBRANDS && !m; k++) if ((m = BRANDS[k]->identify(x))) br = BRANDS[k];
        if (!m && !bd_audio(x)) continue;
        struct dev *d = dev_find(x->addr);
        if (!d) { d = dev_add(x->addr); if (!d) continue; lg("bluez: %s \"%s\" -> %s", x->addr, x->name, m ? m->name : "other audio"); }
        struct buds_st *s = &d->b.s;
        const char *nm = x->alias[0] ? x->alias : x->name;
        if (strcmp(s->name, nm)) { buds_cpy(s->name, sizeof s->name, nm); dirty = 1; }
        if (m && (!d->b.m || (d->b.br != br) || (!d->b.m->id && m->id) || (d->b.m->id == 0 && !m->id && d->b.m != m))) {
            if (!(d->b.m && d->b.br == br && d->b.m->id && !m->id)) { bud_set_model(&d->b, m); d->b.br = br; if (br->fill) br->fill(&d->b); known_save(); }
        }
        if (!m && !d->b.br) {
            s->brand = BB_OTHER; s->family = strstr(x->icon, "headphones") ? BF_HEAD : BF_BUDS; s->caps = 0;
            buds_cpy(s->model, sizeof s->model, "");
        }
        int was = d->bz_conn;
        d->bz_conn = x->connected; d->bz_paired = x->paired;
        if (x->connected && s->conn == BS_NONE) {
            s->conn = BS_AUDIO; dirty = 1; d->tries = 0; d->sdp_tried = 0;
            d->next_try = now_ms() + (x->resolved ? 800 : 2500);       /* let A2DP settle first */
            lg("bluez: %s connected", s->addr);
        } else if (!x->connected && s->conn != BS_NONE) {
            dev_close(d, "disconnected"); s->conn = BS_NONE; dirty = 1;
            lg("bluez: %s disconnected", s->addr);
        }
        (void)was;
    }
    for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && D[i].bz_conn == -1) {      /* removed from BlueZ */
        if (D[i].fd >= 0) dev_close(&D[i], "removed");
        if (D[i].b.s.conn != BS_NONE) { D[i].b.s.conn = BS_NONE; dirty = 1; }
        D[i].bz_conn = 0;
    }
    if (verbose) lg("bluez: %d objects in %.0f ms", n, now_ms() - t0);
}
/* dbus-monitor child: wakes us when a device property changes */
static pid_t mon_pid; static int mon_fd = -1; static double mon_retry;
static void monitor_start(void)
{
    if (mon_pid > 0 || access(DBUS_MON, X_OK) || now_ms() < mon_retry) return;
    int fd[2]; if (pipe(fd)) return;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    char *argv[] = { DBUS_MON, "--system", "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                     "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'", 0 };
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa); close(fd[1]);
    if (r) { close(fd[0]); mon_retry = now_ms() + 60000; return; }
    fcntl(fd[0], F_SETFD, FD_CLOEXEC); fcntl(fd[0], F_SETFL, O_NONBLOCK);
    mon_pid = pid; mon_fd = fd[0];
    lg("bluez: dbus-monitor %d", (int)pid);
}
static void monitor_read(void)
{
    char b[4096]; int k, hit = 0;
    static char tail[24];
    while ((k = (int)read(mon_fd, b, sizeof b - 1)) > 0) {
        b[k] = 0;
        if (strstr(b, "\"Connected\"") || strstr(b, "\"ServicesResolved\"") || strstr(b, "\"Paired\"") || strstr(b, "\"Alias\"") ||
            strstr(b, "InterfacesAdded") || strstr(b, "InterfacesRemoved") || strstr(tail, "Connec")) hit = 1;
        buds_cpy(tail, sizeof tail, k > 20 ? b + k - 20 : b);
    }
    if (k == 0 || (k < 0 && errno != EAGAIN)) {
        close(mon_fd); mon_fd = -1; int st; waitpid(mon_pid, &st, 0); mon_pid = 0; mon_retry = now_ms() + 10000;
        lg("bluez: dbus-monitor ended");
    }
    if (hit) { double t = now_ms() + 400; if (!next_refresh || t < next_refresh) next_refresh = t; }
}
/* asynchronous dbus-send jobs (Connect can take 30 s) */
struct job { pid_t pid; int fd; unsigned seq; int op; char addr[18]; double t0; };
static struct job J[4];
static void job_start(unsigned seq, int op, const char *addr, char *const argv[])
{
    int slot = -1; for (int i = 0; i < 4; i++) if (!J[i].pid) slot = i;
    if (slot < 0) { reply(seq, BE_BUSY); return; }
    int fd[2]; if (pipe(fd)) { reply(seq, BE_IO); return; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1); posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa); close(fd[1]);
    if (r) { close(fd[0]); reply(seq, BE_IO); return; }
    fcntl(fd[0], F_SETFD, FD_CLOEXEC); fcntl(fd[0], F_SETFL, O_NONBLOCK);
    J[slot].pid = pid; J[slot].fd = fd[0]; J[slot].seq = seq; J[slot].op = op; buds_cpy(J[slot].addr, sizeof J[slot].addr, addr); J[slot].t0 = now_ms();
    struct dev *d = dev_find(addr); if (d && (op == BCMD_CONNECT || op == BCMD_DISCONNECT)) { d->busy_job = 1; dirty = 1; }
}
static void job_read(struct job *j)
{
    char b[512]; int k; static char last[160];
    while ((k = (int)read(j->fd, b, sizeof b - 1)) > 0) { b[k] = 0; if (strstr(b, "Error")) buds_cpy(last, sizeof last, strstr(b, "Error")); }
    if (k == 0 || (k < 0 && errno != EAGAIN) || now_ms() - j->t0 > 45000) {
        if (k < 0 && errno == EAGAIN) kill(j->pid, SIGKILL);
        int st = 0; waitpid(j->pid, &st, 0); close(j->fd);
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && !last[0];
        lg("job %s %s: %s %s", BCMD_NAMES[j->op], j->addr, ok ? "ok" : "failed", last);
        struct dev *d = dev_find(j->addr);
        if (d) { d->busy_job = 0; if (ok && j->op == BCMD_RENAME) next_refresh = now_ms() + 200; }
        reply(j->seq, ok ? BE_OK : BE_IO);
        next_refresh = now_ms() + 300;
        last[0] = 0; j->pid = 0; j->fd = -1;
    }
}

/* ------------------------------------------------------------------ control channel */
static void dev_open(struct dev *d)
{
    struct bud *b = &d->b;
    if (!b->br || d->fd >= 0 || d->sim) return;
    b->s.conn = BS_LINKING; publish();
    int fd = -1; double t0 = now_ms();
    if (b->br->transport == BT_L2CAP) fd = l2cap_open(b->s.addr, b->br->psm, 8000);
    else {
        const char *u = b->br->uuid(b);
        if (d->rfch <= 0 || d->sdp_tried) {
            int ch = sdp_channel(b->s.addr, u);
            if (ch <= 0) ch = sdptool_channel(b->s.addr, u);
            lg("%s: SDP %s -> channel %d (%.0f ms)", b->s.addr, u, ch, now_ms() - t0);
            if (ch > 0 && ch != d->rfch) { d->rfch = ch; known_save(); }
            d->sdp_tried = 1;
        }
        fd = d->rfch > 0 ? rfcomm_open(b->s.addr, d->rfch, 8000) : -ENOENT;
        if (fd < 0 && !d->sdp_tried) d->sdp_tried = 1;          /* cached channel failed: SDP next time */
    }
    if (fd < 0) {
        static const int backoff[] = { 2000, 5000, 15000, 30000, 60000, 300000 };
        d->tries++;
        b->s.conn = d->tries >= 6 ? BS_NOCTL : BS_AUDIO;
        d->next_try = d->tries >= 6 ? 0 : now_ms() + backoff[d->tries - 1 < 5 ? d->tries - 1 : 5];
        lg("%s: open (%s) failed: %s; %s", b->s.addr, b->br->tag, strerror(-fd), d->tries >= 6 ? "giving up until it reconnects" : "will retry");
        dirty = 1;
        return;
    }
    d->fd = fd; d->opened = d->last_rx = now_ms(); b->t_open = d->opened;
    lg("%s: %s channel open in %.0f ms", b->s.addr, b->br->tag, now_ms() - t0);
    b->now = now_ms();
    b->br->start(b);
    dirty = 1;
}
static void dev_input(struct dev *d)
{
    uint8_t buf[2048];
    int n = (int)recv(d->fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        dev_close(d, n == 0 ? "closed by the headphones" : strerror(errno));
        if (d->bz_conn) { d->tries++; d->next_try = now_ms() + 3000; next_refresh = now_ms() + 500; }
        return;
    }
    if (verbose) { char h[200]; bud_hex(h, sizeof h, buf, n > 60 ? 60 : n); lg("%s < %s", d->b.s.addr, h); }
    d->last_rx = now_ms(); d->b.s.seen = wall_s();
    d->b.now = now_ms();
    d->b.br->rx(&d->b, buf, n);
    if (d->b.ready) d->tries = 0;
}

/* ------------------------------------------------------------------ commands */
static void command(const char *line)
{
    struct bcmd c;
    if (bcmd_parse(line, &c)) { lg("bad command: %.60s", line); return; }
    struct dev *d = c.addr[0] ? dev_find(c.addr) : 0;
    if (!d && !c.addr[0]) for (int i = 0; i < BUDS_MAXDEV && !d; i++) if (D[i].used && D[i].b.s.conn == BS_READY) d = &D[i];
    lg("command %u %s %s %d %d %d", c.seq, d ? d->b.s.addr : "-", BCMD_NAMES[c.op], c.a, c.b, c.c);
    if (c.op == BCMD_REFRESH) { next_refresh = now_ms(); reply(c.seq, BE_OK); return; }
    if (!d) { reply(c.seq, BE_NOTCONN); return; }
    char path[64]; dev_path(d->b.s.addr, path, sizeof path);
    if (c.op == BCMD_FAKE) {                        /* --fake only: a = 0 conn (b on/off), 1 wear (b, c), 2 battery */
        if (!d->sim) { reply(c.seq, BE_NOTSUP); return; }
        if (c.a == 0) { if (c.b) { d->bz_conn = 1; d->sim_conn_at = now_ms() + 600; d->b.s.conn = BS_AUDIO; } else { d->bz_conn = 0; dev_close(d, "fake"); } }
        else if (c.a == 1) { d->sim->t.wear[0] = c.b; d->sim->t.wear[1] = c.c; bsim_changed(d->sim, (d->b.s.caps & BC_BATT_ONE) != 0); }
        else if (c.a == 2) { d->sim->t.batt[0] = c.b; d->sim->t.batt[1] = c.c; bsim_changed(d->sim, (d->b.s.caps & BC_BATT_ONE) != 0); }
        reply(c.seq, BE_OK); return;
    }
    if (c.op == BCMD_CONNECT || c.op == BCMD_DISCONNECT || c.op == BCMD_RENAME) {
        if (d->sim) {
            if (c.op == BCMD_RENAME) { buds_cpy(d->b.s.name, sizeof d->b.s.name, c.s); dirty = 1; }
            else if (c.op == BCMD_CONNECT) {
                for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && &D[i] != d && D[i].bz_conn) { D[i].bz_conn = 0; dev_close(&D[i], "fake: other connected"); }
                d->bz_conn = 1; d->sim_conn_at = now_ms() + 1500; d->busy_job = 1;
            } else { d->bz_conn = 0; dev_close(d, "fake disconnect"); }
            reply(c.seq, BE_OK); return;
        }
        if (c.op == BCMD_RENAME) {
            char v[96]; snprintf(v, sizeof v, "variant:string:%s", c.s);
            char *argv[] = { DBUS_SEND, "--system", "--print-reply", "--dest=org.bluez", path, "org.freedesktop.DBus.Properties.Set",
                             "string:org.bluez.Device1", "string:Alias", v, 0 };
            buds_cpy(d->b.s.name, sizeof d->b.s.name, c.s); dirty = 1;   /* optimistic */
            job_start(c.seq, c.op, d->b.s.addr, argv);
        } else {
            char *argv[] = { DBUS_SEND, "--system", "--print-reply", "--reply-timeout=30000", "--dest=org.bluez", path,
                             c.op == BCMD_CONNECT ? "org.bluez.Device1.Connect" : "org.bluez.Device1.Disconnect", 0 };
            job_start(c.seq, c.op, d->b.s.addr, argv);
        }
        return;
    }
    if (d->b.s.conn != BS_READY || !d->b.br || (d->fd < 0 && !d->sim)) { reply(c.seq, BE_NOTCONN); return; }
    d->b.now = now_ms();
    int r = d->b.br->cmd(&d->b, &c);
    reply(c.seq, r);
    if (d->b.changed) { d->b.changed = 0; dirty = 1; }
}

/* ------------------------------------------------------------------ fake devices */
static struct bsim SIM[4];
static void fake_setup(void)
{
    static const struct { const char *addr, *name; int brand, id, conn; } F[4] = {
        { "F0:5C:77:A1:02:14", "AirPods Pro", BB_APPLE, 0x2014, 1 },
        { "F0:5C:77:5A:07:02", "Galaxy Buds2 Pro (A1B2)", BB_SAMSUNG, 7, 0 },
        { "F0:5C:77:A1:0A:0A", "AirPods Max", BB_APPLE, 0x200A, 0 },
        { "F0:5C:77:4E:B1:62", "Nothing Ear (a)", BB_NOTHING, 0xB162, 0 },
    };
    for (int i = 0; i < 4; i++) {
        const struct buds_model *m = F[i].brand == BB_APPLE ? aap_model_by_id((unsigned)F[i].id) : F[i].brand == BB_SAMSUNG ? sam_model_by_id(F[i].id) : 0;
        if (F[i].brand == BB_NOTHING) for (unsigned k = 0; k < sizeof NOTHING_MODELS / sizeof *NOTHING_MODELS; k++) if (NOTHING_MODELS[k].m.id == F[i].id) m = &NOTHING_MODELS[k].m;
        struct dev *d = dev_add(F[i].addr);
        if (!d || !m) continue;
        bud_set_model(&d->b, m); d->b.br = F[i].brand == BB_APPLE ? &BRAND_APPLE : F[i].brand == BB_SAMSUNG ? &BRAND_SAMSUNG : &BRAND_NOTHING; if (d->b.br->fill) d->b.br->fill(&d->b);
        buds_cpy(d->b.s.name, sizeof d->b.s.name, F[i].name);
        bsim_init(&SIM[i], m, F[i].name);
        if (F[i].brand == BB_APPLE && m->caps & BC_BATT_ONE) { SIM[i].t.batt[0] = 72; SIM[i].t.chg[0] = 0; }
        d->sim = &SIM[i];
        if (F[i].conn) { d->bz_conn = 1; d->sim_conn_at = now_ms() + 300; d->b.s.conn = BS_AUDIO; }
    }
}
static void fake_step(double t)
{
    static double drain;
    for (int i = 0; i < BUDS_MAXDEV; i++) {
        struct dev *d = &D[i];
        if (!d->used || !d->sim) continue;
        if (d->bz_conn && d->sim_conn_at && t >= d->sim_conn_at) {      /* "connected": the channel opens */
            d->sim_conn_at = 0; d->busy_job = 0;
            struct bsim *m = d->sim; int keep[3]; memcpy(keep, m->t.batt, sizeof keep);
            m->qn = 0; m->got_hello = m->got_notify = 0; memcpy(m->t.batt, keep, sizeof keep);
            d->b.s.conn = BS_LINKING; d->b.t_open = t; d->b.now = t;
            d->b.br->start(&d->b);
            bsim_connected(m);
            dirty = 1;
        }
        uint8_t u[1024]; int n;
        while (d->bz_conn && !d->sim_conn_at && (n = bsim_take(d->sim, u, sizeof u)) > 0) {
            d->b.now = now_ms(); d->b.br->rx(&d->b, u, n); d->b.s.seen = wall_s();
            if (verbose) { char h[200]; bud_hex(h, sizeof h, u, n > 60 ? 60 : n); lg("%s < %s", d->b.s.addr, h); }
        }
        if (!d->bz_conn) d->sim->qn = 0;
    }
    if (t - drain > 90000) {                       /* batteries drain slowly while "worn" */
        drain = t;
        for (int i = 0; i < BUDS_MAXDEV; i++) {
            struct dev *d = &D[i]; if (!d->used || !d->sim || !d->bz_conn) continue;
            for (int k = 0; k < 2; k++) if (d->sim->t.wear[k] == BW_IN && d->sim->t.batt[k] > 5) d->sim->t.batt[k]--;
            bsim_changed(d->sim, (d->b.s.caps & BC_BATT_ONE) != 0);
        }
    }
}

/* ------------------------------------------------------------------ main */
static void on_sig(int s) { (void)s; quit = 1; }
static void paths(void)
{
    const char *dir = getenv("HIBYBUDS_DIR"), *data = getenv("HIBYBUDS_DATA");
    if (!dir) { snprintf(P_STATE, sizeof P_STATE, "%s", BUDS_STATE); snprintf(P_CMD, sizeof P_CMD, "%s", BUDS_CMD); snprintf(P_EVT, sizeof P_EVT, "%s", BUDS_EVT);
                snprintf(P_PIDF, sizeof P_PIDF, "/tmp/hibybudsd.pid"); snprintf(P_LOG, sizeof P_LOG, "/tmp/hibybudsd.log"); }
    else { snprintf(P_STATE, sizeof P_STATE, "%s/hibybuds.state", dir); snprintf(P_CMD, sizeof P_CMD, "%s/hibybuds.cmd", dir); snprintf(P_EVT, sizeof P_EVT, "%s/hibybuds.evt", dir);
           snprintf(P_PIDF, sizeof P_PIDF, "%s/hibybudsd.pid", dir); snprintf(P_LOG, sizeof P_LOG, "%s/hibybudsd.log", dir); }
    snprintf(P_DATA, sizeof P_DATA, "%s", data ? data : BUDS_DATA);
}
static void print_state(FILE *o)
{
    for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used) {
        const struct buds_st *s = &D[i].b.s;
        fprintf(o, "%s  %-28s %-26s conn=%d batt=%d/%d/%d chg=%d/%d/%d wear=%d/%d noise=%d mask=%d eq=%d lock=%d fw=%s\n", s->addr, s->name,
                s->model[0] ? s->model : "-", s->conn, s->batt[0], s->batt[1], s->batt[2], s->chg[0], s->chg[1], s->chg[2], s->wear[0], s->wear[1], s->noise, s->noise_mask, s->eq, s->lock, s->fw);
    }
}
static void log_stdout(struct bud *b, const char *m) { printf("%s: %s\n", b->s.addr, m); }
int main(int argc, char **argv)
{
    const char *probe = 0; int list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fake")) fake = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--matrix")) { buds_matrix_md(stdout); return 0; }
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc) probe = argv[++i];
        else { fprintf(stderr, "usage: hibybudsd [--fake] [-v] [--list] [--probe MAC]\n"); return 2; }
    }
    paths();
    signal(SIGPIPE, SIG_IGN);
    sony_writes = getenv("HIBYBUDS_SONY_WRITES") && atoi(getenv("HIBYBUDS_SONY_WRITES")) == 1;
    if (list || probe) {
        lgf = stdout; verbose = verbose || probe;
        bluez_refresh();
        if (list) { print_state(stdout); for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used) printf("  %s brand=%s model_id=0x%04x caps=0x%x\n", D[i].b.s.addr, D[i].b.br ? D[i].b.br->tag : "other", D[i].b.s.model_id, D[i].b.s.caps); return 0; }
        struct dev *d = dev_find(probe);
        if (!d || !d->b.br) { printf("%s: not a recognised paired device\n", probe); return 1; }
        if (!d->bz_conn) { printf("%s: not connected\n", probe); return 1; }
        d->b.log = log_stdout;
        dev_open(d);
        if (d->fd < 0) return 1;
        double t0 = now_ms();
        while (now_ms() - t0 < 15000) {
            struct pollfd pf = { d->fd, POLLIN, 0 };
            if (poll(&pf, 1, 500) > 0) { dev_input(d); if (d->fd < 0) break; }
            if (d->b.br->tick) d->b.br->tick(&d->b, now_ms());
        }
        print_state(stdout);
        return 0;
    }
    /* one instance */
    int pf = open(P_PIDF, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (pf < 0 || flock(pf, LOCK_EX | LOCK_NB)) { fprintf(stderr, "hibybudsd: already running\n"); return 0; }
    char pb[16]; int pl = snprintf(pb, sizeof pb, "%d\n", (int)getpid()); if (ftruncate(pf, 0) || write(pf, pb, (size_t)pl) < 0) {}
    lgf = fopen(P_LOG, "a");
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig);
    lg("hibybudsd started%s (pid %d)", fake ? " in fake mode" : "", (int)getpid());
    mkfifo(P_CMD, 0666); mkfifo(P_EVT, 0666);
    int cmd_fd = open(P_CMD, O_RDWR | O_NONBLOCK | O_CLOEXEC);          /* RDWR: never EOF */
    evt_fd = open(P_EVT, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cmd_fd < 0) { lg("command fifo: %s", strerror(errno)); return 1; }
    HDR.pid = (int)getpid(); HDR.fake = fake; HDR.v = 1;
    if (fake) fake_setup();
    else { known_load(); next_refresh = now_ms(); }
    publish();
    char cbuf[1024]; int cn = 0;
    while (!quit) {
        double t = now_ms();
        if (!fake) {
            monitor_start();
            if ((next_refresh && t >= next_refresh) || t - last_refresh > (mon_fd >= 0 ? 60000 : 8000)) { next_refresh = 0; bluez_refresh(); }
            for (int i = 0; i < BUDS_MAXDEV; i++) {
                struct dev *d = &D[i];
                if (d->used && d->b.br && d->bz_conn == 1 && d->fd < 0 && d->next_try && now_ms() >= d->next_try) { d->next_try = 0; dev_open(d); }
                /* channel open but silent for 12 s: the model does not answer (no controls) */
                if (d->used && d->fd >= 0 && !d->b.ready && now_ms() - d->opened > 12000 && d->b.s.conn == BS_LINKING) { d->b.s.conn = BS_NOCTL; dirty = 1; lg("%s: no answer on the control channel", d->b.s.addr); }
            }
        } else fake_step(t);
        int any_open = 0;
        for (int i = 0; i < BUDS_MAXDEV; i++) {
            struct dev *d = &D[i];
            if (!d->used) continue;
            if ((d->fd >= 0 || (d->sim && d->bz_conn)) && d->b.br && d->b.br->tick) { d->b.now = now_ms(); d->b.br->tick(&d->b, d->b.now); any_open = 1; }
            if (d->fd >= 0 || (d->sim && d->bz_conn)) any_open = 1;
            if (d->b.changed) { d->b.changed = 0; dirty = 1; }
            /* popup event: first status of a connection, not for a reconnect blip (< 30 s) */
            if (d->b.ready && !d->ready_flag) {
                d->ready_flag = 1;
                if (!d->last_down || now_ms() - d->last_down > 30000) { d->b.s.popseq++; dirty = 1; lg("%s: popup event %d", d->b.s.addr, d->b.s.popseq); }
            }
        }
        if (dirty) publish();
        /* sleep until something happens */
        struct pollfd p[4 + BUDS_MAXDEV + 4]; int np = 0, map[BUDS_MAXDEV + 4];
        p[np].fd = cmd_fd; p[np].events = POLLIN; np++;
        int mon_i = -1; if (mon_fd >= 0) { mon_i = np; p[np].fd = mon_fd; p[np].events = POLLIN; np++; }
        int dev0 = np;
        for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && D[i].fd >= 0) { map[np - dev0] = i; p[np].fd = D[i].fd; p[np].events = POLLIN; np++; }
        int devn = np - dev0, job0 = np;
        for (int i = 0; i < 4; i++) if (J[i].pid) { p[np].fd = J[i].fd; p[np].events = POLLIN; np++; }
        int timeout = fake ? (any_open ? 50 : 500) : any_open ? 1000 : 30000;
        double tt = now_ms();
        if (next_refresh) { int w = (int)(next_refresh - tt); if (w < timeout) timeout = w < 0 ? 0 : w; }
        for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && D[i].next_try && D[i].bz_conn == 1) { int w = (int)(D[i].next_try - tt); if (w < timeout) timeout = w < 0 ? 0 : w; }
        for (int i = 0; i < 4; i++) if (J[i].pid && timeout > 1000) timeout = 1000;
        int r = poll(p, (nfds_t)np, timeout);
        if (r < 0) { if (errno == EINTR) continue; lg("poll: %s", strerror(errno)); msleep(200); continue; }
        if (p[0].revents & POLLIN) {
            int k;
            while ((k = (int)read(cmd_fd, cbuf + cn, sizeof cbuf - 1 - (size_t)cn)) > 0) {
                cn += k; cbuf[cn] = 0;
                char *s = cbuf, *nl;
                while ((nl = strchr(s, '\n'))) { *nl = 0; if (*s) command(s); s = nl + 1; }
                cn = (int)strlen(s); memmove(cbuf, s, (size_t)cn + 1);
                if (cn >= (int)sizeof cbuf - 1) cn = 0;
            }
        }
        if (mon_i >= 0 && p[mon_i].revents) monitor_read();
        for (int k = 0; k < devn; k++) if (p[dev0 + k].revents) { struct dev *d = &D[map[k]]; if (d->fd >= 0) dev_input(d); }
        for (int k = job0, i = 0; i < 4; i++) if (J[i].pid) { if (k < np && p[k].revents) job_read(&J[i]); else if (now_ms() - J[i].t0 > 45000) job_read(&J[i]); k++; }
    }
    lg("hibybudsd: exit");
    for (int i = 0; i < BUDS_MAXDEV; i++) if (D[i].used && D[i].fd >= 0) close(D[i].fd);
    if (mon_pid > 0) { kill(mon_pid, SIGTERM); waitpid(mon_pid, 0, 0); }
    unlink(P_STATE);
    if (evt_fd >= 0) { char c = 'q'; if (write(evt_fd, &c, 1) < 0) {} }
    return 0;
}
