/* sync.inc.c - ByOS Sync: the hibysync daemon seen from the shell (docs/SYNC_PROTOCOL.md, section 18).
 *
 * - Lifecycle: the shell starts `hibysync serve --control 3 [--wifi] [--idle-exit 600]` with
 *   posix_spawn and a socketpair (fd 3 in the child, nothing else inherited), when the Settings
 *   page is open or the Wi-Fi toggle is on (persisted in launcher.conf: "sync 0|1"). It stops it
 *   with QUIT, then SIGTERM after 2 s and SIGKILL after 4 s, without ever waiting on the UI
 *   thread (sync_tick reaps it). Low memory (mem_guard, mem_watch) stops it unless a transfer or
 *   an install is running; shell exit and rejuvenation stop it too.
 * - Control pipe: line based, read without blocking from sync_tick (called from read_pkt, so it
 *   runs inside every app, and from the main loop when idle). Requests that need the user (the
 *   pairing code, an update install, a rescan while music plays) open an iOS alert above
 *   whatever is on screen: a shell screen keeps its image under the dimmed alert, a HiBy screen
 *   is frozen ('C') like the headphones card of syspop.inc.c and released ('R') afterwards.
 * - RESCAN -> lib_post() (sysset.inc.c); UPDATE -> the OTA install of ota_net.inc.c on the files
 *   the daemon verified and staged in SD/ByOS/ota.
 * The SAS code is never logged and is wiped after the alert. */
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SY_BIN "/usr/data/hibymod/shell/hibysync"     /* payload bin/<x> is installed to shell/<x> */
#define SY_BIN2 "/usr/data/hibymod/bin/hibysync"
#define SY_LOG "/tmp/hibysync.log"
#define SY_IDLE_USB 600                               /* s: USB-only daemon exits by itself when idle */
#define SY_MAXC 4
#define SY_MAXP 8
enum { UP_NONE, UP_LOAD, UP_ASK, UP_RUN, UP_DONE };

struct sy_conn { int sid, wifi; char name[68]; };
struct sy_peer { char id[33]; long long when; char name[68]; };
static struct {
    pid_t pid; int fd, wifi, ready; double t_start;
    pid_t dying; double t_quit; int sig;              /* stopping: QUIT sent at t_quit */
    char rx[1024]; int rxn;
    int failed; double fail_t;                        /* consecutive starts without READY */
    struct sy_conn c[SY_MAXC]; int nc;
    int xsid, xput; long long xdone, xtotal; double xt; char xname[72];
    int pair_ui; double pair_t0;
    int sas_sid, sas_pending, sas_showing; volatile int sas_end; char sas[8], sas_name[68]; double sas_t;
    struct sy_peer p[SY_MAXP], pn[SY_MAXP]; int np, npn, peers_ok;
    int rescan_req;
    int up_req, up_phase, up_noconfirm, up_last; char up_ver[24]; double up_t, up_sent;
    int dbstaged, page, modal, in_tick;
    double lowmem_t;
} SY = { .fd = -1 };
static volatile pid_t sy_pid_v;                       /* for mem_watch (another thread) */
static volatile int sy_busy_v, sy_crit;
static int sync_wifi_ui;

static void sy_unpct(const char *s, char *o, int cap)
{
    int k = 0;
    if (!strcmp(s, "-")) s = "";
    for (; *s && k < cap - 1; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 }; int v = (int)strtol(h, 0, 16);
            o[k++] = v >= 0x20 && v != 0x7f ? (char)v : '?'; s += 2;
        } else o[k++] = *s;
    }
    o[k] = 0;
}
static void sy_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sy_send(const char *fmt, ...)
{
    if (SY.fd < 0) return;
    char b[600]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, a); va_end(a);
    if (n < 0) return; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (write(SY.fd, b, (size_t)n) != n) lg("sync: control line not sent (%s)", strerror(errno));
}
static const char *sy_bin(void) { return !access(SY_BIN, X_OK) ? SY_BIN : !access(SY_BIN2, X_OK) ? SY_BIN2 : 0; }
static int sync_xfer_busy(void) { return (SY.xsid && now_ms() - SY.xt < 5000) || SY.up_phase == UP_RUN; }

static void sy_session_reset(void)
{
    SY.nc = 0; SY.xsid = 0; SY.ready = 0; SY.rxn = 0; SY.npn = 0;
    if (SY.sas_sid) { SY.sas_end = 1; SY.sas_pending = 0; if (!SY.sas_showing) SY.sas_sid = 0; }
    if (SY.rescan_req) SY.rescan_req = 0;
}
static void sync_atexit(void);
static int sy_spawn(int wifi)
{
    const char *bin = sy_bin();
    if (!bin) { SY.failed = 3; SY.fail_t = now_ms(); return -1; }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { lg("sync: socketpair: %s", strerror(errno)); return -1; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    /* the child inherits nothing but fd 3 (the grabbed touch screen must not stay open in it) */
    for (int fd = 3; fd < 256; fd++) {
        if (fd == sv[1]) continue;
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0 && !(fl & FD_CLOEXEC)) posix_spawn_file_actions_addclose(&fa, fd);
    }
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, SY_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);   /* musl clears CLOEXEC even when sv[1] == 3 */
    char idle[12]; snprintf(idle, sizeof idle, "%d", SY_IDLE_USB);
    char *av[8]; int n = 0;
    av[n++] = (char *)"hibysync"; av[n++] = (char *)"serve"; av[n++] = (char *)"--control"; av[n++] = (char *)"3";
    if (wifi) av[n++] = (char *)"--wifi"; else { av[n++] = (char *)"--idle-exit"; av[n++] = idle; }
    av[n] = 0;
    pid_t pid; int r = posix_spawn(&pid, bin, &fa, 0, av, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    if (r) { close(sv[0]); lg("sync: hibysync does not start (%s)", strerror(r)); SY.failed++; SY.fail_t = now_ms(); return -1; }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    setpriority(PRIO_PROCESS, (id_t)pid, 19);          /* nice 19 (it sets its idle I/O class itself) */
    SY.pid = pid; sy_pid_v = pid; SY.fd = sv[0]; SY.wifi = wifi; SY.t_start = now_ms();
    sy_session_reset();
    { static int reg; if (!reg) { reg = 1; atexit(sync_atexit); } }   /* shell exit: QUIT it first */
    lg("sync: hibysync %d started (%s)", (int)pid, wifi ? "USB + Wi-Fi" : "USB");
    return 0;
}
/* asynchronous stop: QUIT now, SIGTERM after 2 s and SIGKILL after 4 s from sy_reap */
static void sy_stop(const char *why)
{
    if (SY.pid <= 0) return;
    sy_send("QUIT");
    if (SY.fd >= 0) { close(SY.fd); SY.fd = -1; }
    SY.dying = SY.pid; SY.t_quit = now_ms(); SY.sig = 0;
    SY.pid = 0; sy_pid_v = 0; sy_busy_v = 0;
    sy_session_reset();
    lg("sync: stopping hibysync (%s)", why);
}
static void sy_reap(void)
{
    int s;
    if (SY.dying) {
        pid_t w = waitpid(SY.dying, &s, WNOHANG);
        if (w == SY.dying || (w < 0 && errno == ECHILD)) SY.dying = 0;
        else {
            double t = now_ms() - SY.t_quit;
            if (t > 4000 && SY.sig < 2) { kill(SY.dying, SIGKILL); SY.sig = 2; lg("sync: SIGKILL"); }
            else if (t > 2000 && SY.sig < 1) { kill(SY.dying, SIGTERM); SY.sig = 1; }
        }
    }
    if (SY.pid > 0) {
        pid_t w = waitpid(SY.pid, &s, WNOHANG);
        if (w == SY.pid || (w < 0 && errno == ECHILD)) {
            lg("sync: hibysync exited (%d)%s", w == SY.pid && WIFEXITED(s) ? WEXITSTATUS(s) : -1, SY.ready ? "" : " before READY");
            if (!SY.ready) { SY.failed++; SY.fail_t = now_ms(); }
            if (SY.fd >= 0) { close(SY.fd); SY.fd = -1; }
            SY.pid = 0; sy_pid_v = 0; sy_busy_v = 0;
            sy_session_reset();
        } else if (!SY.ready && now_ms() - SY.t_start > 3000) {
            SY.failed++; SY.fail_t = now_ms();
            sy_stop("no READY after 3 s");
        }
    }
}
static void sy_policy(void)
{
    double t = now_ms();
    if (sy_crit) { sy_crit = 0; SY.lowmem_t = t; }
    if (SY.dying) return;                                          /* the next one waits for the port and the lock */
    int want = sync_wifi_on || SY.page || SY.up_phase != UP_NONE;
    if (SY.pid > 0) {
        /* toggle changed: restart with the other option, but never under a connected client */
        int quiet = SY.nc == 0 && !SY.sas_sid && SY.up_phase == UP_NONE && !sync_xfer_busy();
        if (quiet && SY.wifi != sync_wifi_on && want) sy_stop("Wi-Fi option changed");
        else if (quiet && SY.wifi && !want) sy_stop("Wi-Fi sync turned off");
        return;
    }
    if (!want) return;
    if (SY.failed >= 3) return;                                   /* opening the page tries again */
    if (!SY.page) {
        static double t_first; if (!t_first) t_first = t;
        if (t - t_first < 20000) return;                           /* not during the shell's start */
        if (SY.lowmem_t && t - SY.lowmem_t < 300000) return;      /* stopped for memory: 5 min */
    }
    if (SY.failed && t - SY.fail_t < (SY.page ? 3000 : 30000)) return;
    sy_spawn(sync_wifi_on);
}

/* ------------------------------------------------------------------ control lines (18.3) */
static void sy_conn_del(int sid)
{
    for (int i = 0; i < SY.nc; i++) if (SY.c[i].sid == sid) { SY.c[i] = SY.c[--SY.nc]; i--; }
}
/* the pairing request ended (GONE, PAIRED, PAIRFAIL): the alert closes, or never opens */
static void sy_sas_over(int how)
{
    SY.sas_end = how; SY.sas_pending = 0;
    if (!SY.sas_showing) { SY.sas_sid = 0; memset(SY.sas, 0, sizeof SY.sas); }
}
static void sy_line(char *l)
{
    char *tk[6] = { 0 }, *sv = 0; int n = 0;
    for (char *t = strtok_r(l, " ", &sv); t && n < 6; t = strtok_r(0, " ", &sv)) tk[n++] = t;   /* free text is %-encoded: no spaces */
    if (!n) return;
    const char *c = tk[0]; int sid = tk[1] ? atoi(tk[1]) : 0;
    if (!strcmp(c, "READY")) {
        SY.ready = 1; SY.failed = 0;
        lg("sync: ready (port %s, Wi-Fi %s)", tk[1] ? tk[1] : "?", tk[2] ? tk[2] : "?");
        if (SY.pair_ui && SY.wifi) { sy_send("PAIRING on"); SY.pair_t0 = now_ms(); }
        sy_send("PEERS");
    } else if (!strcmp(c, "CONN") && n >= 4) {
        sy_conn_del(sid);
        if (SY.nc < SY_MAXC) { struct sy_conn *k = &SY.c[SY.nc++]; k->sid = sid; k->wifi = !strcmp(tk[2], "wifi"); sy_unpct(tk[3], k->name, sizeof k->name); }
        lg("sync: session %d open (%s)", sid, tk[2]);
    } else if (!strcmp(c, "GONE")) {
        sy_conn_del(sid);
        if (sid && sid == SY.sas_sid) sy_sas_over(1);
        if (sid && sid == SY.xsid) SY.xsid = 0;
        lg("sync: session %d closed", sid);
    } else if (!strcmp(c, "SAS") && n >= 4) {
        if (strlen(tk[2]) != 6 || strspn(tk[2], "0123456789") != 6) return;
        SY.sas_sid = sid; memcpy(SY.sas, tk[2], 7); sy_unpct(tk[3], SY.sas_name, sizeof SY.sas_name);
        SY.sas_end = 0; SY.sas_pending = 1; SY.sas_t = now_ms();
        lg("sync: pairing request from session %d", sid);          /* never the code */
    } else if (!strcmp(c, "PAIRED") && n >= 4) {
        if (sid == SY.sas_sid) sy_sas_over(2);
        char nm[68], m[120]; sy_unpct(tk[3], nm, sizeof nm);
        snprintf(m, sizeof m, tr("Emparejado con %s"), nm[0] ? nm : "—"); toast(m);
        sy_send("PEERS");
        lg("sync: paired (session %d)", sid);
    } else if (!strcmp(c, "PAIRFAIL")) {
        if (sid == SY.sas_sid) sy_sas_over(1);
        if (tk[2] && strcmp(tk[2], "closed")) toast(tr("Emparejamiento cancelado"));
        lg("sync: pairing failed (%s)", tk[2] ? tk[2] : "?");
    } else if (!strcmp(c, "XFER") && n >= 6) {
        SY.xsid = sid; SY.xput = tk[2][0] == 'p'; SY.xdone = strtoll(tk[3], 0, 10); SY.xtotal = strtoll(tk[4], 0, 10); SY.xt = now_ms();
        char pth[400]; sy_unpct(tk[5], pth, sizeof pth);
        const char *b = strrchr(pth, '/'); snprintf(SY.xname, sizeof SY.xname, "%s", b ? b + 1 : pth);
    } else if (!strcmp(c, "UPDATE") && n >= 3) {
        snprintf(SY.up_ver, sizeof SY.up_ver, "%s", tk[1]); SY.up_noconfirm = !strcmp(tk[2], "noconfirm");
        if (SY.up_phase != UP_NONE) { sy_send("UPDATE_STATE 5 0 8"); return; }   /* E_BUSY */
        SY.up_req = 1;
        lg("sync: install of ByOS %s requested (%s)", SY.up_ver, tk[2]);
    } else if (!strcmp(c, "RESCAN")) {
        SY.rescan_req = 1;
    } else if (!strcmp(c, "DBSTAGED")) {
        SY.dbstaged = 1;
    } else if (!strcmp(c, "ERR")) {
        char m[200]; sy_unpct(tk[1] ? tk[1] : "", m, sizeof m); lg("sync: daemon error: %s", m);
    } else if (!strcmp(c, "BYE")) {
        SY.ready = 0;
    } else if (!strcmp(c, "PEER") && n >= 4) {
        if (SY.npn < SY_MAXP && strlen(tk[1]) == 32) {
            struct sy_peer *q = &SY.pn[SY.npn++];
            memcpy(q->id, tk[1], 33); q->when = strtoll(tk[2], 0, 10); sy_unpct(tk[3], q->name, sizeof q->name);
        }
    } else if (!strcmp(c, "PEERS") && tk[1] && !strcmp(tk[1], "END")) {
        memcpy(SY.p, SY.pn, sizeof SY.p); SY.np = SY.npn; SY.npn = 0; SY.peers_ok = 1;
    }
}
static void sy_io(void)
{
    while (SY.fd >= 0) {
        ssize_t k = read(SY.fd, SY.rx + SY.rxn, sizeof SY.rx - 1 - (size_t)SY.rxn);
        if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
            lg("sync: control pipe closed"); close(SY.fd); SY.fd = -1; sy_session_reset(); return;   /* reaped by sy_reap */
        }
        if (k < 0) return;
        SY.rxn += (int)k; SY.rx[SY.rxn] = 0;
        char *nl;
        while ((nl = memchr(SY.rx, '\n', (size_t)SY.rxn))) {
            *nl = 0; if (nl > SY.rx && nl[-1] == '\r') nl[-1] = 0;
            if (strlen(SY.rx) <= 512) sy_line(SY.rx);
            int used = (int)(nl + 1 - SY.rx); memmove(SY.rx, nl + 1, (size_t)(SY.rxn - used)); SY.rxn -= used;
        }
        if (SY.rxn >= (int)sizeof SY.rx - 1) SY.rxn = 0;                /* overlong line: drop */
    }
}

/* ------------------------------------------------------------------ alert above any screen */
static uint16_t *sy_base; static int sy_cap;
static int sy_ui_ok(void)
{
    return st && !cur_down && !present_direct && !hiby_modal && !sp_on && !hud_active && !fake_home_req && !(st->held && !st->capture);
}
static int sy_grab(void)
{
    if (!sy_base) sy_base = lazy_alloc((void **)&sy_base, HM_FRAME_BYTES, 0);     /* given back when idle */
    if (!sy_base) return -1;
    sy_cap = 0;
    if (!st->capture && !launcher_on) {                 /* a HiBy screen: freeze it, as syspop does */
        st->ov_on = 0; ov_cur = 0;
        cmd('C');
        double t = now_ms(); while (!st->capture && now_ms() - t < 200) msleep(2);
        if (!st->capture) { update_overlay(); return -1; }
        memcpy(sy_base, fr0_shm, HM_FRAME_BYTES);
        sy_cap = 1;
    } else memcpy(sy_base, fr0, HM_FRAME_BYTES);       /* ByOS screen (home or one of its apps) */
    lcd_boost();
    return 0;
}
static void sy_release(void)
{
    if (sy_cap) {
        sy_cap = 0; cmd('R');
        double t = now_ms(); while (st->held && now_ms() - t < 200) msleep(2);
        update_overlay();
    } else { memcpy(fr0, sy_base, HM_FRAME_BYTES); present(); }
}
/* iOS alert: title, optional big code, message, optional countdown, two buttons (no | yes).
 * Returns 1 yes, 0 no, -1 closed by *end or the deadline, -2 the screen could not be taken. */
static int sy_alert(const char *title, const char *code, const char *msg, const char *no, const char *yes, double deadline, volatile int *end)
{
    if (sy_grab()) return -2;
    const int aw = 270, bh = 44;
    int th = draw_wrapped(0, 0, 0, aw - 32, title, 17, 0, 22, 0);
    int ch = code ? 54 : 0, mh = msg ? draw_wrapped(0, 0, 0, aw - 32, msg, 13, 0, 17, 0) : 0, dh = deadline > 0 ? 20 : 0;
    int ah = 20 + th + ch + (msg ? 4 + mh : 0) + dh + 18 + bh, ax = (W - aw) / 2, ay = (H - ah) / 2;
    int sel = -1, res = -1, last_sec = -1, last_sel = -2;
    double t0 = now_ms(), tc = 0;
    struct pkt p; while (read_pkt(&p, 0)) {}
    for (;;) {
        double t = now_ms();
        if (!tc && ((end && *end) || (deadline > 0 && t >= deadline))) { res = -1; tc = t; }
        int anim = tc || t - t0 < 240;
        int sec = deadline > 0 ? (int)((deadline - t + 999) / 1000) : 0; if (sec < 0) sec = 0;
        if (anim || sec != last_sec || sel != last_sel) {
            last_sec = sec; last_sel = sel;
            float e = tc ? 1 - (float)ease_out((t - tc) / 160.0 > 1 ? 1 : (t - tc) / 160.0) : (float)ease_out((t - t0) / 220.0 > 1 ? 1 : (t - t0) / 220.0);
            float sc = tc ? 1 : 1.12f - 0.12f * e;
            dim_buf(fr0, sy_base, W * H, 32 - (int)(13 * e));
            int w2 = (int)(aw * sc), h2 = (int)(ah * sc), x2 = (W - w2) / 2, y2 = (H - h2) / 2, al = (int)(250 * e);
            if (al > 8) {
                fill_rrect(fr0, x2, y2, w2, h2, 14, rgb(44, 44, 46), al);
                if (e > 0.6f) {
                    wrap_cw = aw - 32;
                    int y = ay + 20;
                    y += draw_wrapped(fr0, ax + 16, y - 4, aw - 32, title, 17, 0xFFFF, 22, 1);
                    if (code) {                                    /* "815 365": large, grouped 3 + 3 */
                        int cw = tw_n(code, 40, -1);
                        draw_text(fr0, (W - cw) / 2, y + 42, code, 40, 0xFFFF, 0, 0, H);
                        y += ch;
                    }
                    if (msg) { y += 4; y += draw_wrapped(fr0, ax + 16, y - 3, aw - 32, msg, 13, rgb(235, 235, 245), 17, 1); }
                    wrap_cw = 0;
                    if (dh) { char s[16]; snprintf(s, sizeof s, "%d s", sec); int sw = tw_n(s, 12, -1); draw_text(fr0, (W - sw) / 2, y + 14, s, 12, rgb(142, 142, 147), 0, 0, H); }
                    int by = ay + ah - bh;
                    for (int x = ax; x < ax + aw; x++) fr0[by * W + x] = rgb(70, 70, 74);
                    for (int y3 = by; y3 < ay + ah; y3++) fr0[y3 * W + ax + aw / 2] = rgb(70, 70, 74);
                    if (sel >= 0) fill_rrect(fr0, ax + (sel ? aw / 2 + 1 : 0), by + 1, aw / 2 - 1, bh - 1, 0, rgb(64, 64, 68), 255);
                    int nw = tw_n(no, 17, -1); if (nw > aw / 2 - 8) nw = aw / 2 - 8;
                    draw_text(fr0, ax + aw / 4 - nw / 2, by + 28, no, 17, rgb(10, 132, 255), aw / 2 - 8, 0, H);
                    int yw = tw_n(yes, 17, -1); if (yw > aw / 2 - 8) yw = aw / 2 - 8;
                    draw_text(fr0, ax + 3 * aw / 4 - yw / 2, by + 28, yes, 17, rgb(10, 132, 255), aw / 2 - 8, 0, H);
                    draw_text(fr0, ax + 3 * aw / 4 - yw / 2 + 1, by + 28, yes, 17, rgb(10, 132, 255), aw / 2 - 8, 0, H);   /* default button: bold */
                }
            }
            present();
        }
        if (tc && t - tc >= 160) break;
        if (anim) continue;
        player_poke();                                     /* the player's auto-lock must not turn the screen off */
        if (!read_pkt(&p, 100) || !p.down) continue;
        int x = p.x, y = p.y, by = ay + ah - bh;
        if (y >= by && y < ay + ah && x >= ax && x < ax + aw) {
            sel = x >= ax + aw / 2;
            while (cur_down && read_pkt(&p, 3000)) {}
            res = sel; tc = now_ms();
        } else while (cur_down && read_pkt(&p, 3000)) {}
    }
    sy_release();
    return res;
}

/* ------------------------------------------------------------------ requests from clients */
static void sy_sas_alert(void)
{
    int sid = SY.sas_sid;
    char title[160], code[8], name[68];
    snprintf(name, sizeof name, "%s", SY.sas_name[0] ? SY.sas_name : "—");
    snprintf(title, sizeof title, tr("¿Emparejar con %s?"), name);
    snprintf(code, sizeof code, "%.3s %.3s", SY.sas, SY.sas + 3);
    SY.sas_showing = 1;
    int r = sy_alert(title, code, tr("Comprueba que el mismo código aparece en el otro dispositivo."),
                     tr("Denegar"), tr("Permitir"), SY.sas_t + 120000, &SY.sas_end);
    memset(code, 0, sizeof code);
    SY.sas_showing = 0;
    if (r == -2) { if (!SY.sas_end) SY.sas_pending = 1; else SY.sas_sid = 0; return; }           /* screen busy: again on the next tick */
    if (!SY.sas_end && SY.sas_sid == sid) sy_send(r == 1 ? "ALLOW %d" : "DENY %d", sid);
    memset(SY.sas, 0, sizeof SY.sas); SY.sas_sid = 0;
}
static void sy_do_rescan(void)
{
    SY.rescan_req = 0;
    struct np_info ni; npx_snapshot(&ni, 0, 0);
    if (ni.playing) {
        int r = sy_alert(tr("¿Actualizar la biblioteca?"), 0, tr("La música se detendrá mientras se actualiza la biblioteca."),
                         tr("Cancelar"), tr("Actualizar"), now_ms() + 60000, 0);
        if (r == -2) { SY.rescan_req = 1; return; }
        if (r != 1) { sy_send("RESCAN_RESULT failed"); return; }
    }
    char b[32] = "";
    if (lib_post()) {
        sy_send("RESCAN_RESULT ok");
        lib_popup_force();                                 /* progress: the system card of libpop.inc.c */
    } else {
        lib_read(b, sizeof b);
        sy_send("RESCAN_RESULT %s", !strcmp(b, "nosd") || !strcmp(b, "busy") ? "failed" : "unsupported");
    }
}
static int sy_ota_err(int e)   /* OE_* -> BTP error code (docs/SYNC_PROTOCOL.md section 6) */
{
    switch (e) {
    case OE_NOSD: return 40; case OE_BATT: return 67; case OE_SPACE_SD: case OE_SPACE_DATA: return 38;
    case OE_FIRMWARE: return 68; case OE_DOWNGRADE: return 66; case OE_SIG: case OE_NOKEY: return 64;
    case OE_SHA: case OE_TAR: case OE_FORMAT: case OE_MINVER: return 65;
    default: return 39;
    }
}
static void sy_up_fail(int code, const char *msg)
{
    sy_send("UPDATE_STATE 5 0 %d", code);
    SY.up_phase = UP_NONE;
    toast(msg ? msg : tr("No se pudo instalar la actualización"));
}
/* the daemon verified and staged SD/ByOS/ota/hibyos-update.json and its tar: the OTA worker loads it again */
static void sy_up_begin(void)
{
    if (ota_st.busy) {
        if (ota_op == OTA_OP_INSTALL || ota_op == OTA_OP_ROLLBACK) { SY.up_req = 0; sy_send("UPDATE_STATE 5 0 8"); return; }
        return;                                            /* a check or a load: wait for it */
    }
    SY.up_req = 0;
    ota_paths_init(); ota_versions_refresh();
    if (!ota_inited) { ota_inited = 1; ota_conf_load(); }
    if (ota_start(OTA_OP_LOAD)) { sy_up_fail(8, 0); return; }
    SY.up_phase = UP_LOAD; SY.up_t = now_ms();
}
static void sy_up_start(void)
{
    if (!ota_sd_ok()) { sy_up_fail(40, otr("OTA_E_NOSD")); return; }
    if (!ota_power_ok()) { sy_up_fail(67, ota_err_text(OE_BATT)); return; }
    ota_st.allow_down = ota_st.installed[0] && ota_vercmp(SY.up_ver, ota_st.installed) <= 0;   /* the client asked for it (the daemon checked) */
    ota_last_op = OTA_OP_INSTALL; ota_inst_t0 = 0;
    if (ota_start(OTA_OP_INSTALL)) { sy_up_fail(8, 0); return; }
    SY.up_phase = UP_RUN; SY.up_last = -1; SY.up_t = now_ms();
    sy_send("UPDATE_STATE 3 0 0");
    lg("sync: installing ByOS %s", SY.up_ver);
}
static void sy_up_ask(void)
{
    char t[96], m[600], from[120];
    snprintf(t, sizeof t, otr("¿Instalar ByOS %s?"), SY.up_ver);
    snprintf(from, sizeof from, tr("Enviado desde %s."), SY.nc && SY.c[0].name[0] ? SY.c[0].name : "ByOS Sync");
    snprintf(m, sizeof m, "%s %s", from, ota_note(ota_st.info.restart));
    sy_send("UPDATE_STATE 2 0 0");
    int r = sy_alert(t, 0, m, tr("Cancelar"), tr("Instalar"), 0, 0);
    if (r == -2) return;
    if (r != 1) { sy_send("UPDATE_STATE 6 0 0"); SY.up_phase = UP_NONE; lg("sync: install refused on the device"); return; }
    sy_up_start();
}
static void sync_shutdown(void);
static void sy_up_done(void)
{
    int rs = ota_st.restart;
    if (rs == OTA_RS_NONE) { SY.up_phase = UP_NONE; toast(otr("Actualización instalada")); return; }
    if (rs == OTA_RS_SHELL && SY.up_noconfirm) {           /* confirmed on the computer (USB) */
        SY.up_phase = UP_NONE; lg("sync: restarting the shell after the update");
        sync_shutdown(); act_ota_restart(); return;
    }
    int r = sy_alert(otr("Actualización instalada"), 0, ota_note(rs), tr("Más tarde"),
                     otr(rs == OTA_RS_SHELL ? "Reiniciar la interfaz" : "Reiniciar ahora"), 0, 0);
    if (r == -2) return;
    SY.up_phase = UP_NONE;
    if (r == 1) { if (rs == OTA_RS_SHELL) sync_shutdown(); act_ota_restart(); }
}
/* install progress for UPDATE_STATUS (non-UI, every second) */
static void sy_up_watch(void)
{
    if (SY.up_phase == UP_LOAD && !ota_st.busy) {
        pthread_mutex_lock(&ota_st.mu);
        int ok = ota_st.have_info && !strcmp(ota_st.info.version, SY.up_ver);
        pthread_mutex_unlock(&ota_st.mu);
        if (!ok) { lg("sync: staged update not found or not valid"); sy_up_fail(65, 0); return; }
        if (SY.up_noconfirm) sy_up_start(); else SY.up_phase = UP_ASK;
        return;
    }
    if (SY.up_phase != UP_RUN) return;
    int s = ota_st.state, pm = 0;
    if (!ota_st.busy) {
        if (s == OS_DONE) { sy_send("UPDATE_STATE 4 1000 0"); SY.up_phase = UP_DONE; lg("sync: ByOS %s installed", SY.up_ver); }
        else sy_up_fail(sy_ota_err(ota_st.err), ota_st.err ? ota_err_text(ota_st.err) : 0);
        return;
    }
    if (s == OS_VERIFYING) pm = (int)(300 * ota_st.frac);
    else if (s == OS_EXTRACTING) pm = 300 + (int)(300 * ota_st.frac);
    else if (s == OS_INSTALLING) { pm = 600 + (int)((now_ms() - SY.up_t) / 100); if (pm > 950) pm = 950; }
    if (pm / 10 != SY.up_last) { SY.up_last = pm / 10; sy_send("UPDATE_STATE 3 %d 0", pm); }
}
static void sy_ui_events(void)
{
    if (SY.sas_pending) { SY.sas_pending = 0; if (!SY.sas_end && SY.sas_sid) sy_sas_alert(); else SY.sas_sid = 0; return; }
    if (SY.rescan_req) { sy_do_rescan(); return; }
    if (SY.up_req) { sy_up_begin(); return; }
    if (SY.up_phase == UP_ASK) { sy_up_ask(); return; }
    if (SY.up_phase == UP_DONE) sy_up_done();
}
/* called from read_pkt (every app) and from the main loop when idle: a few syscalls at most */
static void sync_tick(void)
{
    static double next_io, next_pol;
    if (SY.in_tick) return;
    double t = now_ms();
    if (SY.fd < 0 && SY.pid <= 0 && !SY.dying && !SY.up_phase && t < next_pol) return;
    SY.in_tick = 1;
    if (t >= next_io) { next_io = t + 100; sy_io(); }
    if (t >= next_pol) { next_pol = t + 1000; sy_reap(); sy_policy(); sy_up_watch(); sy_busy_v = sync_xfer_busy(); }
    SY.in_tick = 0;
    if (!SY.modal && (SY.sas_pending || SY.rescan_req || SY.up_req || SY.up_phase == UP_ASK || SY.up_phase == UP_DONE) && sy_ui_ok()) {
        SY.modal = 1; sy_ui_events(); SY.modal = 0;
    }
}
/* low memory (mem_guard): stop it unless something is being transferred or installed */
static void sync_lowmem(void)
{
    if (SY.pid <= 0 || sync_xfer_busy() || SY.up_phase != UP_NONE || SY.sas_sid) return;
    sy_stop("low memory"); SY.lowmem_t = now_ms();
}
/* critical memory (mem_watch thread): SIGTERM, the main thread reaps it; 1 = signalled */
static int sync_crit_kill(void)
{
    pid_t p = sy_pid_v;
    if (p <= 0 || sy_busy_v) return 0;
    sy_crit = 1;
    return kill(p, SIGTERM) == 0;
}
/* shell exit or rejuvenation: QUIT, up to 2 s, then SIGTERM (the daemon also exits on EOF) */
static void sync_shutdown(void)
{
    if (SY.pid > 0) {
        pid_t p = SY.pid; sy_stop("shell exit");
        double t = now_ms(); int s;
        while (waitpid(p, &s, WNOHANG) == 0 && now_ms() - t < 2000) msleep(20);
        if (waitpid(p, &s, WNOHANG) == 0) { kill(p, SIGTERM); msleep(50); waitpid(p, &s, WNOHANG); }
        SY.dying = 0;
    }
}
static void sync_atexit(void) { sync_shutdown(); }

/* ------------------------------------------------------------------ Settings > ByOS Sync */
static char sy_v[6][96];
static const char *sy_wlan_ip(void)
{
    static double t; static char ip[24];
    if (now_ms() - t < 3000 && t) return ip[0] ? ip : 0;
    t = now_ms(); ip[0] = 0;
    struct ifaddrs *ia;
    if (getifaddrs(&ia)) return 0;
    for (struct ifaddrs *i = ia; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "wlan0"))
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
    freeifaddrs(ia);
    return ip[0] ? ip : 0;
}
static const char *sy_status(int *tone)   /* tone: 0 off, 1 waiting, 2 connected, 3 error */
{
    *tone = 0;
    if (!sy_bin()) { *tone = 3; return tr("No instalado"); }
    if (SY.pid <= 0 && SY.failed) { *tone = 3; return tr("No se pudo iniciar"); }
    if (SY.pid <= 0) return tr("Desactivado");
    if (!SY.ready) return tr("Iniciando…");
    if (SY.nc) {
        *tone = 2;
        snprintf(sy_v[0], sizeof sy_v[0], tr("Conectado: %s"), SY.c[0].name[0] ? SY.c[0].name : "—");
        if (SY.nc > 1) { size_t l = strlen(sy_v[0]); snprintf(sy_v[0] + l, sizeof sy_v[0] - l, " +%d", SY.nc - 1); }
        return sy_v[0];
    }
    *tone = 1; return tr("Esperando a un ordenador o un teléfono");
}
static void sync_glyph(uint16_t *d, float cx, float cy, float s, uint16_t c);
static void sy_hero(uint16_t *d, int y, int h)
{
    int tone; const char *s = sy_status(&tone);
    int top = NAVH + 30, sz = 58, ix = (W - sz) / 2, iy = y + 10;
    if (iy > top - sz && iy < H) {                        /* the navigation bar is drawn over it afterwards */
        fill_rrect(d, ix, iy, sz, sz, 13, rgb(48, 176, 199), 255);
        sync_glyph(d, ix + sz / 2.f, iy + sz / 2.f, 2.f, 0xFFFF);
    }
    static const uint8_t dot[4][3] = { { 142, 142, 147 }, { 10, 132, 255 }, { 52, 199, 89 }, { 255, 69, 58 } };
    int tw = tw_n(s, 15, -1); if (tw > W - 64) tw = W - 64;
    int ty = y + h - 18, tx = (W - tw - 14) / 2;
    if (ty - 12 >= top) fill_circle(d, tx + 4, ty - 5, 4, rgb(dot[tone][0], dot[tone][1], dot[tone][2]), 255);
    draw_text(d, tx + 14, ty, s, 15, 0xFFFF, W - 64, top, H);
}
static const char *v_sy_xfer(void)
{
    snprintf(sy_v[1], sizeof sy_v[1], "%.1f / %.1f MB", SY.xdone / 1048576.0, SY.xtotal / 1048576.0);
    return sy_v[1];
}
static int sy_xfer_pm;
static const char *v_sy_conn(int i) { return SY.c[i].wifi ? "Wi-Fi" : "USB"; }
static const char *v_sy_c0(void) { return v_sy_conn(0); } static const char *v_sy_c1(void) { return v_sy_conn(1); }
static const char *v_sy_c2(void) { return v_sy_conn(2); } static const char *v_sy_c3(void) { return v_sy_conn(3); }
static const char *(*const v_sy_c[SY_MAXC])(void) = { v_sy_c0, v_sy_c1, v_sy_c2, v_sy_c3 };
static char sy_peer_date[SY_MAXP][48];
static const char *v_sy_peer(int i) { lang_date((time_t)SY.p[i].when, sy_peer_date[i], sizeof sy_peer_date[i]); return sy_peer_date[i]; }
static void sy_forget(int i)
{
    if (i >= SY.np) return;
    char id[33]; memcpy(id, SY.p[i].id, sizeof id);
    const char *o[1] = { tr("Olvidar este dispositivo") };
    char title[240]; snprintf(title, sizeof title, tr("¿Olvidar %s? Tendrá que volver a emparejarse para conectarse por Wi-Fi."), SY.p[i].name[0] ? SY.p[i].name : "—");
    if (action_sheet(title, o, 1, 0) != 0) return;
    if (SY.fd < 0 || !SY.ready) { toast(tr("No disponible ahora")); return; }
    sy_send("FORGET %s", id); sy_send("PEERS");
    for (int k = 0; k < SY.np; k++) if (!strcmp(SY.p[k].id, id)) { SY.p[k] = SY.p[--SY.np]; break; }
}
#define SY_PEER_FN(n) static const char *v_sy_p##n(void) { return v_sy_peer(n); } static void sy_forget##n(void) { sy_forget(n); }
SY_PEER_FN(0) SY_PEER_FN(1) SY_PEER_FN(2) SY_PEER_FN(3) SY_PEER_FN(4) SY_PEER_FN(5) SY_PEER_FN(6) SY_PEER_FN(7)
static const char *(*const v_sy_p[SY_MAXP])(void) = { v_sy_p0, v_sy_p1, v_sy_p2, v_sy_p3, v_sy_p4, v_sy_p5, v_sy_p6, v_sy_p7 };
static void (*const sy_forget_fn[SY_MAXP])(void) = { sy_forget0, sy_forget1, sy_forget2, sy_forget3, sy_forget4, sy_forget5, sy_forget6, sy_forget7 };

static void apply_sync_wifi(void)
{
    sync_wifi_on = sync_wifi_ui; SY.failed = 0; lconf_save();
    if (SY.pid > 0 && SY.nc > 0 && SY.wifi != sync_wifi_on) toast(tr("Se aplicará cuando se desconecte"));
    sy_policy();
}
static void page_sync_pair(struct spage *p);
static void act_sy_reopen(void) { if (SY.ready && SY.wifi) { sy_send("PAIRING on"); SY.pair_t0 = now_ms(); } }
static const char *v_sy_name(void) { return "HiBy R3 II"; }
static const char *v_sy_addr(void) { const char *ip = sy_wlan_ip(); return ip ? ip : tr("Sin conexión Wi-Fi"); }
static const char *v_sy_pstate(void)
{
    if (!SY.ready || !SY.wifi) return tr("Iniciando…");
    return now_ms() - SY.pair_t0 > 300000 ? tr("Tiempo agotado") : tr("Esperando un dispositivo…");
}
static void page_sync_pair(struct spage *p)
{
    *p = (struct spage){ "Nuevo dispositivo" };
    p->secs[0] = (struct ssec){ 0, "SYNC_PAIR_HELP", { ROWV("Nombre", v_sy_name), ROWV("Dirección", v_sy_addr), ROWV("Estado", v_sy_pstate) }, 3 };
    if (SY.ready && SY.wifi && now_ms() - SY.pair_t0 > 300000) { p->secs[0].rows[3] = ROW("Volver a abrir", R_ACTION, 0, 0, 0, act_sy_reopen, 0, 0); p->secs[0].n = 4; }
    p->nsec = 1;
}
static void nav_sync_pair(void)
{
    if (!sync_wifi_on) { sync_wifi_ui = 1; apply_sync_wifi(); toast(tr("Conexiones por Wi-Fi activadas")); }
    SY.pair_ui = 1; SY.pair_t0 = now_ms();
    if (SY.ready && SY.wifi) sy_send("PAIRING on");           /* otherwise on READY */
    open_page(page_sync_pair, "ByOS Sync");
    SY.pair_ui = 0;
    if (SY.ready) sy_send("PAIRING off");
}
static void page_sync(struct spage *p)
{
    *p = (struct spage){ "ByOS Sync" };
    p->hero = 112; p->hero_fn = sy_hero;
    int n = 0;
    struct ssec *a = &p->secs[n]; *a = (struct ssec){ "CONECTADOS" };
    if (SY.xsid && now_ms() - SY.xt < 5000 && SY.xtotal > 0) {
        sy_xfer_pm = (int)(1000 * SY.xdone / SY.xtotal);
        a->rows[a->n++] = (struct srow){ SY.xput ? "Recibiendo" : "Enviando", R_BAR, &sy_xfer_pm, 0, 0, 0, v_sy_xfer, 0 };
        snprintf(sy_v[2], sizeof sy_v[2], "%s", SY.xname); a->footer = sy_v[2];
    }
    for (int i = 0; i < SY.nc && a->n < 6; i++) a->rows[a->n++] = ROWV(SY.c[i].name[0] ? SY.c[i].name : "—", v_sy_c[i]);
    if (SY.dbstaged) a->rows[a->n++] = ROWV("Biblioteca lista: reinicia para aplicarla", 0);
    if (a->n) n++;
    sync_wifi_ui = sync_wifi_on;
    p->secs[n++] = (struct ssec){ "WI-FI", "SYNC_WIFI_HELP", { ROWT("Permitir conexiones por Wi-Fi", &sync_wifi_ui, apply_sync_wifi, 0),
                                                            ROWI("Emparejar un dispositivo nuevo", nav_sync_pair, 0, 0) }, 2 };
    struct ssec *pp = &p->secs[n++]; *pp = (struct ssec){ "DISPOSITIVOS EMPAREJADOS", "SYNC_PEERS_HELP" };
    if (!SY.peers_ok) pp->rows[pp->n++] = ROWV("Cargando…", 0);
    else if (!SY.np) pp->rows[pp->n++] = ROWV("Ninguno", 0);
    else for (int i = 0; i < SY.np && i < SY_MAXP; i++) pp->rows[pp->n++] = ROWI(SY.p[i].name[0] ? SY.p[i].name : "—", sy_forget_fn[i], v_sy_p[i], 0);
    p->secs[n++] = (struct ssec){ "USB", "SYNC_USB_HELP", { { 0 } }, 0 };
    p->nsec = n;
}
static void nav_sync(void)
{
    SY.page = 1; SY.failed = 0;
    sy_policy();
    if (SY.ready) sy_send("PEERS");
    open_page(page_sync, "Ajustes");
    SY.page = 0;
}
static const char *v_sync_root(void) { return SY.nc ? tr("Conectado") : sync_wifi_on ? tr("Activado") : 0; }
