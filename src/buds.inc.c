/* buds.inc.c - "Auriculares" (Headphones): the shell side of the headphone management
 * (docs/HEADPHONES.md). iOS AirPods-settings style.
 *
 * The protocols live in the daemon hibybudsd (src/hibybudsd.c): this file only reads its state
 * file and writes commands to its FIFO, so nothing here can block the UI or touch Bluetooth.
 *   - a reader thread (bu_thread) sleeps on the daemon's event FIFO; when the state changes it
 *     re-reads /tmp/hibybuds.state, resolves pending commands, runs the auto-pause (ear
 *     detection) and raises the connect popup; it also (re)starts the daemon if it is gone;
 *   - commands are optimistic: the value changes on screen at once (a local override) and goes
 *     back if the daemon answers with an error or nothing within 4 s.
 *
 * Entry points for the shell owner (launcher icon, Control Centre card, popup, widgets):
 *   buds_preload()                    start the thread (+ daemon) once at shell start
 *   buds_app_at(ix, iy) / buds_app()  open the app (zoom from the icon / from the centre)
 *   buds_status(&st)                  current headphones (1 if connected); for the CC card
 *   buds_set_mode(BN_*)               noise control of the current headphones (optimistic)
 *   buds_popup_pending(id, cap)       1 while a "headphones connected" card is due (id = address)
 *   buds_popup_needed()               the same without the id
 *   buds_popup_draw(d, progress)      draws it sliding from the bottom (progress 0..1)
 *   buds_popup_hit(x, y, progress)    1 = close button, 2 = card (open the app), 0 = outside
 *   buds_popup_ack() / _dismiss()     the coordinator showed / closed it
 *   buds_wg_items(...), buds_wg_gen(), buds_glyph()  Batteries widget items (widgets.inc.c)
 *   buds_draw_product(d, cx, cy, s, family, color)  product drawing (any size)
 * Strings: tr() keys, translations in src/lang/buds_tr.inc.c. */
#include <spawn.h>
#include <sys/wait.h>
#include "buds_all.inc.c"

#define BU_BIN ASSETS "/hibybudsd"
#define BU_FAKE ASSETS "/hibybuds.fake"         /* exists: start the daemon with --fake (QEMU harness) */
#define BU_PREFS BUDS_DATA "/shell.conf"

/* ------------------------------------------------------------------ state (thread) */
static struct buds_st BU[BUDS_MAXDEV]; static int BUn; static struct buds_hdr BUh;
static pthread_mutex_t bu_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned bu_gen, bu_wgen;      /* any change / battery-widget relevant change */
static int bu_started;
static volatile int bu_daemon_ok;
struct bu_pend { char addr[18]; int op, a, b, c, val; unsigned seq; double t; };
static struct bu_pend bu_p[12]; static int bu_np;
static unsigned bu_seq;
static volatile int bu_err_code; static volatile unsigned bu_err_n;    /* last command error (the UI shows it) */
static char bu_pop_addr[18]; static double bu_pop_t; static volatile int bu_pop_on; static volatile unsigned bu_pop_gen;
struct bu_pq { char addr[18]; int seq; };
static struct bu_pq bu_pq[BUDS_MAXDEV * 2]; static int bu_npq, bu_pq_init;
static double bu_t0;
/* preferences: auto-pause per device (default on) */
struct bu_pref { char addr[18]; int autopause; };
static struct bu_pref bu_pf[12]; static int bu_npf;
static void bu_prefs_load(void)
{
    FILE *f = fopen(BU_PREFS, "r"); if (!f) return;
    char l[80], a[18]; int v;
    while (fgets(l, sizeof l, f) && bu_npf < 12) if (sscanf(l, "autopause %17s %d", a, &v) == 2) { buds_cpy(bu_pf[bu_npf].addr, 18, a); bu_pf[bu_npf++].autopause = v; }
    fclose(f);
}
static void bu_prefs_save(void)
{
    mkdir(BUDS_DATA, 0755);
    FILE *f = fopen(BU_PREFS ".tmp", "w"); if (!f) return;
    for (int i = 0; i < bu_npf; i++) fprintf(f, "autopause %s %d\n", bu_pf[i].addr, bu_pf[i].autopause);
    fclose(f); rename(BU_PREFS ".tmp", BU_PREFS);
}
static int bu_autopause(const char *addr) { for (int i = 0; i < bu_npf; i++) if (!strcmp(bu_pf[i].addr, addr)) return bu_pf[i].autopause; return 1; }
static void bu_set_autopause(const char *addr, int v)
{
    pthread_mutex_lock(&bu_mu);
    int i; for (i = 0; i < bu_npf; i++) if (!strcmp(bu_pf[i].addr, addr)) break;
    if (i == bu_npf && bu_npf < 12) { buds_cpy(bu_pf[bu_npf].addr, 18, addr); bu_npf++; }
    if (i < 12) bu_pf[i].autopause = v;
    pthread_mutex_unlock(&bu_mu);
    bu_prefs_save();
}

/* daemon: alive? (pid in the state header); start it if not */
static void bu_daemon_start(void)
{
    if (access(BU_BIN, X_OK)) return;
    char *fk = !access(BU_FAKE, F_OK) ? "--fake" : 0;
    char *argv[] = { BU_BIN, fk, 0 };
    posix_spawnattr_t at; posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#endif
    /* el demonio sobrevive al shell: que no herede NINGUN descriptor (la tactil capturada
     * se quedaria bloqueada para el shell siguiente) */
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    for (int fd = 3; fd < 256; fd++) if (fcntl(fd, F_GETFD) >= 0) posix_spawn_file_actions_addclose(&fa, fd);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    lg("auriculares: hibybudsd %s", r ? "no arranca" : "arrancado");
}
/* auto-pause like iOS: taking a bud out pauses (if it was playing); putting it back resumes
 * if we paused it (within 5 min). Only the playing flag of the shim is read here (thread-safe). */
static int bu_playing(void)
{
    struct hm_state *s = np_map();
    return s && s->pcm_open && !s->pcm_paused && np_ms() - s->pcm_last_ms < 1500;
}
static char bu_ap_addr[18]; static int bu_ap_paused; static double bu_ap_t; static int bu_ap_in[BUDS_MAXDEV];
static int bu_inear(const struct buds_st *s) { int n = 0; for (int i = 0; i < 2; i++) if (s->wear[i] == BW_IN) n++; return n; }
static void bu_autopause_step(const struct buds_st *old, int nold, const struct buds_st *nw, int n)
{
    for (int i = 0; i < n; i++) {
        const struct buds_st *s = &nw[i];
        if (!(s->caps & BC_WEAR) || s->conn != BS_READY || !bu_autopause(s->addr)) continue;
        int prev = -1;
        for (int k = 0; k < nold; k++) if (!strcmp(old[k].addr, s->addr) && old[k].conn == BS_READY) prev = bu_inear(&old[k]);
        int cur = bu_inear(s);
        if (prev < 0) continue;
        if (cur < prev && bu_playing()) {
            pc_toggle(); bu_ap_paused = 1; bu_ap_t = now_ms(); buds_cpy(bu_ap_addr, 18, s->addr); bu_ap_in[i] = prev;
            lg("auriculares: fuera del oido -> pausa");
        } else if (cur > prev && bu_ap_paused && !strcmp(bu_ap_addr, s->addr) && cur >= bu_ap_in[i] && now_ms() - bu_ap_t < 300000) {
            if (!bu_playing()) pc_toggle();
            bu_ap_paused = 0; lg("auriculares: de vuelta al oido -> reanudar");
        }
    }
}
static unsigned bu_wsig(const struct buds_st *s, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h = (h ^ (unsigned)s[i].conn) * 16777619u; for (int k = 0; k < 3; k++) h = (h ^ (unsigned)(s[i].batt[k] + 7 * s[i].chg[k])) * 16777619u; h = (h ^ (unsigned)s[i].family) * 16777619u; }
    return h;
}
static void bu_read_state(void)
{
    static char txt[16384]; static struct buds_st nw[BUDS_MAXDEV], old[BUDS_MAXDEV];
    int f = open(BUDS_STATE, O_RDONLY | O_CLOEXEC);
    int k = f >= 0 ? (int)read(f, txt, sizeof txt - 1) : -1;
    if (f >= 0) close(f);
    struct buds_hdr h; int n = 0;
    if (k > 0) { txt[k] = 0; n = buds_state_parse(txt, &h, nw, BUDS_MAXDEV); } else memset(&h, 0, sizeof h);
    bu_daemon_ok = k > 0 && h.pid > 0 && !kill(h.pid, 0);
    if (!bu_daemon_ok) n = 0;
    pthread_mutex_lock(&bu_mu);
    int nold = BUn; memcpy(old, BU, sizeof old);
    unsigned w0 = bu_wsig(BU, BUn);
    memcpy(BU, nw, sizeof BU); BUn = n; BUh = h;
    for (int i = 0; i < bu_np; ) {                       /* pending commands answered? */
        struct bu_pend *p = &bu_p[i];
        int done = h.ack >= p->seq && h.ack - p->seq < 100000, err = done && h.err == p->seq;
        if (err) { bu_err_code = h.err_code; bu_err_n++; }
        if (done || now_ms() - p->t > 4000) {
            if (!done) { bu_err_code = BE_IO; bu_err_n++; }
            bu_p[i] = bu_p[--bu_np]; continue;
        }
        i++;
    }
    /* connect popup: the daemon counts one event per connection (popseq, debounced 30 s there);
     * the values seen at the first read only set the baseline (no popup at shell start) */
    for (int i = 0; i < n; i++) {
        int k; for (k = 0; k < bu_npq; k++) if (!strcmp(bu_pq[k].addr, nw[i].addr)) break;
        if (k == bu_npq) { if (bu_npq >= BUDS_MAXDEV * 2) continue; buds_cpy(bu_pq[k].addr, 18, nw[i].addr); bu_pq[k].seq = bu_pq_init ? 0 : nw[i].popseq; bu_npq++; }
        if (nw[i].popseq > bu_pq[k].seq) {
            bu_pq[k].seq = nw[i].popseq;
            if (nw[i].conn == BS_READY) {
                buds_cpy(bu_pop_addr, 18, nw[i].addr); bu_pop_t = now_ms(); bu_pop_on = 1; bu_pop_gen++;
                lg("auriculares: conectados %s (aviso)", nw[i].name);
            }
        }
    }
    bu_pq_init = 1;
    unsigned w1 = bu_wsig(BU, BUn);
    bu_gen++;
    if (w0 != w1) bu_wgen++;
    pthread_mutex_unlock(&bu_mu);
    if (w0 != w1) wg_fresh = 1;                          /* the Batteries widget redraws (its signature decides) */
    bu_autopause_step(old, nold, nw, n);
}
static void *bu_thread(void *arg)
{
    (void)arg;
    bu_prefs_load();
    mkfifo(BUDS_EVT, 0666);
    int ev = open(BUDS_EVT, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    double last_start = -1e9, last_read = 0; struct stat s0; memset(&s0, 0, sizeof s0);
    bu_read_state();
    for (;;) {
        if (!bu_daemon_ok && now_ms() - last_start > 30000) { last_start = now_ms(); bu_daemon_start(); }
        struct pollfd pf = { ev, POLLIN, 0 };
        int r = ev >= 0 ? poll(&pf, 1, bu_np ? 500 : 3000) : (msleep(2000), 0);
        if (r > 0) { char b[256]; while (read(ev, b, sizeof b) > 0) {} }
        struct stat s1;
        int st_ok = !stat(BUDS_STATE, &s1);
        int changed = st_ok != (s0.st_ino != 0) || (st_ok && (s1.st_ino != s0.st_ino || s1.st_mtime != s0.st_mtime || s1.st_size != s0.st_size));
        if (r > 0 || changed || bu_np || now_ms() - last_read > 30000) { bu_read_state(); last_read = now_ms(); }
        if (st_ok) s0 = s1; else memset(&s0, 0, sizeof s0);
    }
    return 0;
}
static void buds_preload(void)
{
    if (bu_started) return;
    bu_started = 1; bu_t0 = now_ms();
    pthread_t t; if (!pthread_create(&t, 0, bu_thread, 0)) pthread_detach(t);
}

/* ------------------------------------------------------------------ commands */
static int bu_send(const char *addr, int op, int a, int b, int c, const char *s)
{
    struct bcmd cm; memset(&cm, 0, sizeof cm);
    pthread_mutex_lock(&bu_mu); cm.seq = ++bu_seq + (unsigned)(getpid() % 1000) * 100000u; pthread_mutex_unlock(&bu_mu);
    buds_cpy(cm.addr, sizeof cm.addr, addr ? addr : ""); cm.op = op; cm.a = a; cm.b = b; cm.c = c; if (s) buds_cpy(cm.s, sizeof cm.s, s);
    char line[160]; int n = bcmd_format(line, sizeof line, &cm);
    int f = open(BUDS_CMD, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    int ok = f >= 0 && n > 0 && write(f, line, (size_t)n) == n;
    if (f >= 0) close(f);
    if (!ok) { toast(tr("El servicio de auriculares no responde")); return -1; }
    pthread_mutex_lock(&bu_mu);
    if (bu_np < 12) { struct bu_pend *p = &bu_p[bu_np++]; buds_cpy(p->addr, 18, cm.addr); p->op = op; p->a = a; p->b = b; p->c = c; p->seq = cm.seq; p->t = now_ms(); }
    pthread_mutex_unlock(&bu_mu);
    lg("auriculares: orden %s", line);
    return 0;
}
/* the published state with the optimistic overrides of the pending commands */
static void bu_apply(struct buds_st *s, const struct bu_pend *p)
{
    switch (p->op) {
    case BCMD_NOISE: s->noise = p->a; break;
    case BCMD_ANCLVL: s->anc_lvl = p->a; if (s->noise_mask & BUDS_MASK(BN_ANC)) s->noise = BN_ANC; break;
    case BCMD_AMBLVL: s->amb_lvl = p->a; break;
    case BCMD_ADAPTLVL: s->adapt_lvl = p->a; break;
    case BCMD_CONV: s->conv = p->a; break;
    case BCMD_EQ: s->eq = p->a; break;
    case BCMD_LOCK: s->lock = p->a; break;
    case BCMD_EARDET: s->eardet = p->a; break;
    case BCMD_GAME: s->game = p->a; break;
    case BCMD_CYCLE: s->cycle = p->a; break;
    case BCMD_BASS: s->bass = p->a; break;
    case BCMD_GEST: if (p->a >= 0 && p->a < 2 && p->b >= 0 && p->b < BG_N) s->gest[p->a][p->b] = p->c; break;
    case BCMD_FIND: if (p->b) { s->find[0] = p->a != 1; s->find[1] = p->a != 0; } else s->find[0] = s->find[1] = 0; break;
    case BCMD_CONNECT: case BCMD_DISCONNECT: s->busy = 1; break;
    }
}
static int bu_view(struct buds_st *out, int max, struct buds_hdr *h)
{
    pthread_mutex_lock(&bu_mu);
    int n = BUn < max ? BUn : max;
    memcpy(out, BU, sizeof *out * (size_t)n); if (h) *h = BUh;
    for (int i = 0; i < bu_np; i++) for (int k = 0; k < n; k++) if (!strcmp(out[k].addr, bu_p[i].addr)) bu_apply(&out[k], &bu_p[i]);
    pthread_mutex_unlock(&bu_mu);
    return n;
}
static int bu_find_dev(const struct buds_st *v, int n, const char *addr) { for (int i = 0; i < n; i++) if (!strcmp(v[i].addr, addr)) return i; return -1; }
static const char *bu_err_text(int code)
{
    switch (code) {
    case BE_NOTCONN: return tr("Los auriculares no están conectados");
    case BE_NOTSUP: return tr("Este modelo no permite ese ajuste");
    case BE_RANGE: return tr("Valor no válido para este modelo");
    case BE_BUSY: return tr("Espera a que termine la operación");
    default: return tr("No se pudo cambiar el ajuste");
    }
}

/* ------------------------------------------------------------------ public API (CC card, popup, widgets) */
/* the "current" headphones: READY first, then any connected; 1 if one is connected */
static int buds_status(struct buds_st *out)
{
    struct buds_st v[BUDS_MAXDEV]; int n = bu_view(v, BUDS_MAXDEV, 0), best = -1;
    for (int i = 0; i < n; i++) if (v[i].conn == BS_READY) { best = i; break; }
    if (best < 0) for (int i = 0; i < n; i++) if (v[i].conn != BS_NONE) { best = i; break; }
    if (best < 0) { if (out) buds_st_init(out); return 0; }
    if (out) *out = v[best];
    return 1;
}
static int buds_set_mode(int mode)
{
    struct buds_st s;
    if (!buds_status(&s) || s.conn != BS_READY || !(s.caps & BC_NOISE) || (s.caps & BC_RO)) return -1;
    if (mode < 0 || mode >= BN_N || !(s.noise_mask & BUDS_MASK(mode))) return -1;
    return bu_send(s.addr, BCMD_NOISE, mode, 0, 0, 0);
}
/* Batteries widget items (connected headphones). part: 1 left, 2 right, 3 case, 4 both / single.
 * max_hp 1: one merged item per device (small widget); otherwise L/R/case of the first device. */
static int buds_wg_items(int *pct, int *chg, int *fam, int *part, int max, int split)
{
    struct buds_st v[BUDS_MAXDEV]; int n = bu_view(v, BUDS_MAXDEV, 0), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const struct buds_st *s = &v[i];
        if (s->conn == BS_NONE || (s->batt[0] < 0 && s->batt[1] < 0 && s->batt[2] < 0)) continue;
        int single = (s->caps & BC_BATT_ONE) || s->batt[1] < 0 || s->batt[0] < 0;
        if (!split || single || max - k < 2) {
            int a = s->batt[0], b = s->batt[1], p = a < 0 ? b : b < 0 ? a : a < b ? a : b;
            pct[k] = p; chg[k] = (s->chg[0] == 1 || s->chg[1] == 1); fam[k] = s->family; part[k++] = 4;
        } else {
            for (int j = 0; j < 2 && k < max; j++) { pct[k] = s->batt[j]; chg[k] = s->chg[j] == 1; fam[k] = s->family; part[k++] = 1 + j; }
        }
        if (split && s->batt[2] >= 0 && k < max) { pct[k] = s->batt[2]; chg[k] = s->chg[2] == 1; fam[k] = s->family; part[k++] = 3; }
    }
    return k;
}
static unsigned buds_wg_gen(void) { return bu_wgen; }
static int buds_popup_needed(void)
{
    if (!bu_pop_on) return 0;
    if (now_ms() - bu_pop_t > 20000) { bu_pop_on = 0; return 0; }
    struct buds_st v[BUDS_MAXDEV]; int n = bu_view(v, BUDS_MAXDEV, 0), i = bu_find_dev(v, n, bu_pop_addr);
    if (i < 0 || v[i].conn != BS_READY) { bu_pop_on = 0; return 0; }
    return 1;
}
static void buds_popup_dismiss(void) { bu_pop_on = 0; }
static void buds_popup_ack(void) { bu_pop_on = 0; }
static int buds_popup_pending(char *id, int cap)
{
    if (!buds_popup_needed()) return 0;
    if (id && cap > 0) buds_cpy(id, (size_t)cap, bu_pop_addr);
    return 1;
}

/* ------------------------------------------------------------------ drawing primitives */
#define BU_NAVH 64
#define BU_TOP (BU_NAVH + 30)
#define BU_ROWH 44
#define BU_CARD rgb(28, 28, 30)
#define BU_SEL rgb(58, 58, 60)
#define BU_SEP rgb(56, 56, 58)
#define BU_GRAY rgb(142, 142, 147)
#define BU_BLUE rgb(10, 132, 255)
#define BU_GREEN rgb(52, 199, 89)
#define BU_RED rgb(255, 69, 58)
#define BU_YELLOW rgb(255, 214, 10)
static int bu_y0 = 0, bu_y1 = H;                 /* vertical clip of the primitives */
static inline void bu_px(uint16_t *d, int x, int y, uint16_t c, float cov)
{
    if (x < 0 || x >= W || y < bu_y0 || y >= bu_y1 || cov <= 0) return;
    if (cov >= 1) { d[y * W + x] = c; return; }
    d[y * W + x] = mix565(c, d[y * W + x], (unsigned)(cov * 255));
}
/* rounded rectangle: row spans, anti-aliased edges, interior at memory speed */
static void bu_card(uint16_t *d, int x, int y, int w, int h, float r, uint16_t c)
{
    if (r > h / 2.f) r = h / 2.f;
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < bu_y0 || yy >= bu_y1 || yy < 0 || yy >= H) continue;
        float dy = j + .5f < r ? r - (j + .5f) : (j + .5f > h - r ? (j + .5f) - (h - r) : 0);
        float edge = dy > 0 ? r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0) : 0;
        int full = (int)ceilf(edge);
        uint16_t *row = d + yy * W;
        for (int i = full; i < w - full; i++) { int xx = x + i; if (xx >= 0 && xx < W) row[xx] = c; }
        if (edge > 0 && full > 0) { float cov = full - edge; bu_px(d, x + full - 1, yy, c, cov); bu_px(d, x + w - full, yy, c, cov); }
    }
}
static void bu_line(uint16_t *d, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
    float hw = w / 2, dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = (int)(fminf(y0, y1) - hw - 1); y <= (int)(fmaxf(y0, y1) + hw + 1); y++) {
        if (y < bu_y0 || y >= bu_y1) continue;
        for (int x = (int)(fminf(x0, x1) - hw - 1); x <= (int)(fmaxf(x0, x1) + hw + 1); x++) {
            float px = x + .5f - x0, py = y + .5f - y0, t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
            if (t < 0) t = 0; if (t > 1) t = 1;
            float ex = px - t * dx, ey = py - t * dy;
            bu_px(d, x, y, c, hw + .5f - sqrtf(ex * ex + ey * ey));
        }
    }
}
/* ring / arc: a0..a1 radians, 0 = 12 o'clock, clockwise; only the band is evaluated */
static void bu_arc(uint16_t *d, float cx, float cy, float r, float w, float a0, float a1, uint16_t c)
{
    float ro = r + w / 2 + 1, ri = r - w / 2 - 1; if (ri < 0) ri = 0;
    int full = a1 - a0 >= 6.2831f;
    for (int y = (int)(cy - ro); y <= (int)(cy + ro); y++) {
        if (y < bu_y0 || y >= bu_y1) continue;
        float py = y + .5f - cy;
        for (int x = (int)(cx - ro); x <= (int)(cx + ro); x++) {
            float px = x + .5f - cx, d2 = px * px + py * py;
            if (d2 > ro * ro || d2 < ri * ri) continue;
            float dist = sqrtf(d2), cov = w / 2 + .5f - fabsf(dist - r);
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            if (!full) {
                float a = atan2f(px, -py); if (a < a0 - 3.1416f) a += 6.2832f;
                float ca = fminf(a - a0, a1 - a) * dist + .5f; if (ca <= 0) continue; if (ca < 1) cov *= ca;
            }
            bu_px(d, x, y, c, cov);
        }
    }
}
/* filled ellipse, rotated by "ang" (radians), anti-aliased */
static void bu_ellipse(uint16_t *d, float cx, float cy, float rx, float ry, float ang, uint16_t c, float alpha)
{
    float m = rx > ry ? rx : ry, ca = cosf(ang), sa = sinf(ang);
    for (int y = (int)(cy - m - 1); y <= (int)(cy + m + 1); y++) {
        if (y < bu_y0 || y >= bu_y1) continue;
        for (int x = (int)(cx - m - 1); x <= (int)(cx + m + 1); x++) {
            float px = x + .5f - cx, py = y + .5f - cy, u = px * ca + py * sa, v = -px * sa + py * ca;
            float k = sqrtf(u * u / (rx * rx) + v * v / (ry * ry));
            if (k > 1.3f) continue;
            float cov = (1 - k) * (rx < ry ? rx : ry) + .5f; if (cov > 1) cov = 1;
            bu_px(d, x, y, c, cov * alpha);
        }
    }
}
static void bu_chev(uint16_t *d, float x, float cy, uint16_t c) { bu_line(d, x, cy - 5.5f, x + 5.5f, cy, 2, c); bu_line(d, x + 5.5f, cy, x, cy + 5.5f, 2, c); }
static void bu_check(uint16_t *d, float x, float y, uint16_t c) { bu_line(d, x, y + 6, x + 4, y + 10, 2.2f, c); bu_line(d, x + 4, y + 10, x + 11, y, 2.2f, c); }
static void bu_switch(uint16_t *d, int x, int y, float p, int dim)
{
    uint16_t off = rgb(57, 57, 61), c = mix565(BU_GREEN, off, (unsigned)(p * 255));
    if (dim) c = mix565(c, BU_CARD, 110);
    fill_rrect(d, x, y, 51, 31, 15.5f, c, 255);
    fill_circle(d, x + 15.5f + 20 * p, y + 15.5f, 13.5f, dim ? rgb(170, 170, 174) : 0xFFFF, 255);
}
static void bu_spinner(uint16_t *d, float cx, float cy, float r, double t)
{
    int step = (int)(t / 83.0) % 12;
    for (int i = 0; i < 12; i++) {
        float a = i * (float)M_PI / 6; int age = (step - i + 12) % 12, k = 255 - age * 17; if (k < 70) k = 70;
        bu_line(d, cx + sinf(a) * r * .48f, cy - cosf(a) * r * .48f, cx + sinf(a) * r, cy - cosf(a) * r, r * .2f, rgb(k * 200 / 255, k * 200 / 255, k * 205 / 255));
    }
}
static void bu_bolt(uint16_t *d, float x, float y, float h, uint16_t c)
{
    bu_line(d, x + h * .55f, y, x + h * .2f, y + h * .55f, h * .16f, c);
    bu_line(d, x + h * .2f, y + h * .55f, x + h * .62f, y + h * .45f, h * .16f, c);
    bu_line(d, x + h * .62f, y + h * .45f, x + h * .3f, y + h, h * .16f, c);
}
static int bu_fit(const char *t, int size, int maxw) { int s = size; while (s > 9 && tw_n(t, s, -1) > maxw) s--; return s; }
static void bu_textc(uint16_t *d, int cx, int base, const char *t, int size, uint16_t c, int maxw)
{
    int s = bu_fit(t, size, maxw), w = tw_n(t, s, -1); if (w > maxw) w = maxw;
    draw_text(d, cx - w / 2, base, t, s, c, maxw, bu_y0, bu_y1);
}

/* ------------------------------------------------------------------ layout checks */
/* Every element that must not collide (product parts, rings, texts, buttons, nav labels) is
 * recorded here while bu_rec is set (tests/buds_render.c checks overlaps and alignment). */
enum { BT_PART = 1, BT_RING, BT_RTXT, BT_RLBL, BT_BOLT, BT_TITLE, BT_BACK, BT_SEGI, BT_SEGL, BT_LICON, BT_LTXT,
       BT_PTITLE, BT_PCLOSE, BT_EQAX, BT_FINDBTN, BT_EMPTYTXT, BT_LBOX };
/* grp = page row (-1 nav bar, -2 popup); lay = product layout the element belongs to (0 none) */
struct bu_el { short x0, y0, x1, y1; unsigned char tag, col, lay; short grp; };
#define BU_MAXEL 240
static struct bu_el bu_els[BU_MAXEL]; static int bu_nel, bu_rec, bu_grp, bu_lay, bu_lay_n;
static void bu_mark(int tag, int col, float x0, float y0, float x1, float y1)
{
    if (!bu_rec || bu_nel >= BU_MAXEL) return;
    struct bu_el *e = &bu_els[bu_nel++];
    e->x0 = (short)floorf(x0); e->y0 = (short)floorf(y0); e->x1 = (short)ceilf(x1); e->y1 = (short)ceilf(y1);
    e->tag = (unsigned char)tag; e->col = (unsigned char)col; e->grp = (short)bu_grp; e->lay = (unsigned char)bu_lay;
}
/* text box from its baseline (ascender ~0.78, descender ~0.22 of the size) */
static void bu_mark_text(int tag, int col, int x, int base, int w, int size) { bu_mark(tag, col, (float)x, base - size * .78f, (float)(x + w), base + size * .22f); }

/* ------------------------------------------------------------------ product drawings */
/* colours: white (Apple, Nothing), silver (Galaxy), black (Sony, Beats) */
static void bu_colors(int family, int color, uint16_t *body, uint16_t *shade, uint16_t *accent)
{
    (void)color;
    switch (family) {
    case BF_PODS: case BF_PODS_PRO: case BF_NOTHING: *body = rgb(246, 246, 248); *shade = rgb(196, 196, 202); *accent = rgb(120, 120, 128); break;
    case BF_PODS_MAX: *body = rgb(214, 216, 220); *shade = rgb(150, 152, 158); *accent = rgb(236, 236, 240); break;
    case BF_GBUDS: case BF_GBUDS_LIVE: case BF_GBUDS_STEM: *body = rgb(214, 216, 222); *shade = rgb(150, 152, 160); *accent = rgb(120, 122, 132); break;
    case BF_BEATS_BUDS: case BF_BEATS_EAR: case BF_BEATS_HEAD: *body = rgb(40, 40, 44); *shade = rgb(22, 22, 24); *accent = rgb(230, 30, 50); break;
    default: *body = rgb(196, 198, 204); *shade = rgb(132, 134, 140); *accent = rgb(96, 98, 106); break;
    }
}
/* one part: 1 left bud, 2 right bud, 3 case, 4 the whole headphone (over-ear) / single bud.
 * (cx, cy) = drawing origin; s = scale. Its real extent comes from bu_part_bb() (measured). */
static void bu_part(uint16_t *d, float cx, float cy, float s, int family, int color, int part)
{
    uint16_t body, shade, acc; bu_colors(family, color, &body, &shade, &acc);
    float m = part == 1 ? -1 : 1;                       /* left bud = mirrored */
    if (part == 3) {                                    /* case */
        if (family == BF_PODS || family == BF_PODS_PRO) {
            float w = 62 * s, h = (family == BF_PODS_PRO ? 50 : 64) * s;
            fill_rrect(d, (int)(cx - w / 2), (int)(cy - h / 2), (int)w, (int)h, 14 * s, shade, 255);
            fill_rrect(d, (int)(cx - w / 2 + 1.5f * s), (int)(cy - h / 2), (int)(w - 3 * s), (int)(h - 2 * s), 13 * s, body, 255);
            for (int x = (int)(cx - w / 2 + 3 * s); x < (int)(cx + w / 2 - 3 * s); x++) bu_px(d, x, (int)(cy - h / 2 + h * .32f), shade, .8f);
            fill_circle(d, cx, cy + h * .08f, 2.2f * s, BU_GREEN, 255);
        } else if (family == BF_NOTHING) {              /* transparent square case, white cradle */
            float w = 60 * s, h = 60 * s;
            fill_rrect(d, (int)(cx - w / 2), (int)(cy - h / 2), (int)w, (int)h, 12 * s, rgb(150, 150, 156), 255);
            fill_rrect(d, (int)(cx - w / 2 + 2 * s), (int)(cy - h / 2 + 2 * s), (int)(w - 4 * s), (int)(h - 4 * s), 10 * s, rgb(54, 54, 60), 255);
            fill_rrect(d, (int)(cx - w * .34f), (int)(cy - h * .1f), (int)(w * .68f), (int)(h * .36f), 8 * s, rgb(236, 236, 240), 255);
            fill_circle(d, cx - w * .17f, cy + h * .08f, 5 * s, rgb(70, 70, 76), 255); fill_circle(d, cx + w * .17f, cy + h * .08f, 5 * s, rgb(70, 70, 76), 255);
            for (int x = (int)(cx - w / 2 + 4 * s); x < (int)(cx + w / 2 - 4 * s); x++) bu_px(d, x, (int)(cy - h * .22f), rgb(150, 150, 156), .9f);
        } else if (family == BF_GBUDS_STEM) {
            float w = 70 * s, h = 46 * s;
            fill_rrect(d, (int)(cx - w / 2), (int)(cy - h / 2), (int)w, (int)h, 18 * s, shade, 255);
            fill_rrect(d, (int)(cx - w / 2 + 2 * s), (int)(cy - h / 2 + 1 * s), (int)(w - 4 * s), (int)(h * .45f), 14 * s, body, 255);
        } else {
            float w = 72 * s, h = 48 * s;                    /* clamshell (Galaxy, Beats, generic) */
            bu_ellipse(d, cx, cy, w / 2, h / 2, 0, shade, 1);
            bu_ellipse(d, cx, cy - 1.5f * s, w / 2 - 1.5f * s, h / 2 - 2 * s, 0, body, 1);
            for (int x = (int)(cx - w / 2 + 4 * s); x < (int)(cx + w / 2 - 4 * s); x++) bu_px(d, x, (int)(cy - 2 * s), shade, .9f);
            fill_circle(d, cx, cy + h * .22f, 1.8f * s, BU_GREEN, 255);
        }
        return;
    }
    switch (family) {
    case BF_PODS: case BF_PODS_PRO: {
        int pro = family == BF_PODS_PRO;
        float hx = cx - m * 6 * s, hy = cy - (pro ? 14 : 24) * s;
        float sl = (pro ? 30 : 46) * s;
        bu_line(d, cx + m * 2 * s, hy + 6 * s, cx + m * 6 * s, hy + 6 * s + sl, 9 * s, shade);    /* stem */
        bu_line(d, cx + m * 1 * s, hy + 6 * s, cx + m * 5 * s, hy + 6 * s + sl, 7.5f * s, body);
        bu_ellipse(d, hx, hy, 15 * s, 12 * s, m * .35f, shade, 1);                                   /* head */
        bu_ellipse(d, hx, hy - .8f * s, 14 * s, 11 * s, m * .35f, body, 1);
        if (pro) bu_ellipse(d, hx - m * 10 * s, hy + 1 * s, 6 * s, 8 * s, m * .4f, rgb(170, 170, 176), 1);   /* ear tip */
        bu_ellipse(d, hx + m * 3 * s, hy - 3 * s, 3 * s, 2 * s, 0, rgb(70, 70, 76), .9f);             /* grille */
        fill_circle(d, cx + m * 5.5f * s, hy + 6 * s + sl, 2 * s, acc, 255);
        break;
    }
    case BF_NOTHING: {                                  /* white head, transparent stem with its parts */
        float hx = cx - m * 6 * s, hy = cy - 18 * s, sl = 36 * s;
        bu_line(d, cx + m * 2 * s, hy + 6 * s, cx + m * 5 * s, hy + 6 * s + sl, 10 * s, rgb(150, 150, 156));
        bu_line(d, cx + m * 2 * s, hy + 6 * s, cx + m * 5 * s, hy + 6 * s + sl, 7 * s, rgb(54, 54, 60));     /* clear stem */
        bu_line(d, cx + m * 2.6f * s, hy + 12 * s, cx + m * 3.6f * s, hy + 22 * s, 3 * s, rgb(236, 236, 240));  /* inner parts */
        fill_circle(d, cx + m * 4.6f * s, hy + 34 * s, 2 * s, part == 2 ? rgb(230, 40, 40) : rgb(236, 236, 240), 255);   /* R red, L white */
        bu_ellipse(d, hx, hy, 15 * s, 12 * s, m * .35f, shade, 1);
        bu_ellipse(d, hx, hy - .8f * s, 14 * s, 11 * s, m * .35f, body, 1);
        bu_ellipse(d, hx - m * 10 * s, hy + 1 * s, 6 * s, 8 * s, m * .4f, rgb(170, 170, 176), 1);
        break;
    }
    case BF_GBUDS_STEM: {
        float hx = cx - m * 7 * s, hy = cy - 16 * s;
        bu_line(d, cx + m * 1 * s, hy + 4 * s, cx + m * 7 * s, hy + 40 * s, 9 * s, shade);
        bu_line(d, cx + m * 2 * s, hy + 6 * s, cx + m * 7.5f * s, hy + 38 * s, 2 * s, rgb(235, 235, 240));   /* light blade */
        bu_ellipse(d, hx, hy, 14 * s, 11 * s, m * .3f, shade, 1);
        bu_ellipse(d, hx, hy - .8f * s, 13 * s, 10 * s, m * .3f, body, 1);
        bu_ellipse(d, hx - m * 9 * s, hy + 2 * s, 5 * s, 7 * s, m * .4f, rgb(110, 110, 118), 1);
        break;
    }
    case BF_GBUDS_LIVE:
        bu_ellipse(d, cx, cy, 26 * s, 15 * s, m * .5f, shade, 1);
        bu_ellipse(d, cx - m * 2 * s, cy - 1.5f * s, 24 * s, 13 * s, m * .5f, body, 1);
        bu_ellipse(d, cx + m * 8 * s, cy - 6 * s, 7 * s, 4 * s, m * .5f, acc, .7f);
        break;
    case BF_PODS_MAX: case BF_BEATS_HEAD: case BF_HEAD: {
        int max = family == BF_PODS_MAX;
        bu_arc(d, cx, cy - 2 * s, 40 * s, 7 * s, -1.75f, 1.75f, max ? acc : shade);            /* headband */
        for (int k = -1; k <= 1; k += 2) {
            float x = cx + k * 37 * s, y = cy + 18 * s;
            if (max) { fill_rrect(d, (int)(x - 13 * s), (int)(y - 22 * s), (int)(26 * s), (int)(44 * s), 11 * s, shade, 255); fill_rrect(d, (int)(x - 12 * s), (int)(y - 21 * s), (int)(24 * s), (int)(42 * s), 10 * s, body, 255); }
            else { bu_ellipse(d, x, y, 15 * s, 21 * s, 0, shade, 1); bu_ellipse(d, x, y - 1 * s, 13.5f * s, 19.5f * s, 0, body, 1); }
            if (family == BF_BEATS_HEAD) { fill_circle(d, x, y, 6 * s, acc, 255); fill_circle(d, x, y, 3 * s, body, 255); }
        }
        break;
    }
    case BF_BEATS_EAR:
        bu_arc(d, cx + m * 2 * s, cy - 6 * s, 20 * s, 5 * s, m > 0 ? -2.6f : 0.6f, m > 0 ? -0.6f : 2.6f, shade);    /* ear hook */
        bu_ellipse(d, cx, cy + 8 * s, 16 * s, 16 * s, 0, shade, 1);
        bu_ellipse(d, cx, cy + 7 * s, 14.5f * s, 14.5f * s, 0, body, 1);
        fill_circle(d, cx, cy + 7 * s, 5.5f * s, acc, 255); fill_circle(d, cx, cy + 7 * s, 2.7f * s, body, 255);
        break;
    case BF_BEATS_BUDS:
        bu_ellipse(d, cx, cy, 18 * s, 16 * s, m * .2f, shade, 1);
        bu_ellipse(d, cx, cy - 1 * s, 16.5f * s, 14.5f * s, m * .2f, body, 1);
        fill_circle(d, cx, cy - 1 * s, 6 * s, acc, 255); fill_circle(d, cx, cy - 1 * s, 3 * s, body, 255);
        bu_ellipse(d, cx - m * 13 * s, cy + 4 * s, 6 * s, 7 * s, m * .5f, rgb(90, 90, 96), 1);
        break;
    default:                                            /* Galaxy round bud and generic buds */
        bu_ellipse(d, cx, cy, 20 * s, 17 * s, m * .25f, shade, 1);
        bu_ellipse(d, cx, cy - 1.2f * s, 18.5f * s, 15.5f * s, m * .25f, body, 1);
        bu_ellipse(d, cx + m * 3 * s, cy - 3 * s, 9 * s, 7 * s, m * .25f, acc, .55f);             /* touch pad */
        bu_ellipse(d, cx - m * 4 * s, cy - 9 * s, 7 * s, 3 * s, m * .25f, 0xFFFF, .35f);           /* highlight */
        bu_ellipse(d, cx - m * 14 * s, cy + 6 * s, 6 * s, 7 * s, m * .6f, rgb(110, 110, 118), 1);  /* ear tip */
        break;
    }
}
static int bu_single(int family) { return family == BF_PODS_MAX || family == BF_BEATS_HEAD || family == BF_HEAD; }
/* measured extent of a part (pixels actually drawn, relative to its origin), cached */
struct bu_bbc { int fam, part, s100; short x0, y0, x1, y1; };
static struct bu_bbc bu_bbcs[64]; static int bu_nbbc;
#define BU_MB_H 260
static void bu_part_bb(int fam, int part, float s, float *x0, float *y0, float *x1, float *y1)
{
    int s100 = (int)(s * 100 + .5f);
    for (int i = 0; i < bu_nbbc; i++) if (bu_bbcs[i].fam == fam && bu_bbcs[i].part == part && bu_bbcs[i].s100 == s100) {
        *x0 = bu_bbcs[i].x0; *y0 = bu_bbcs[i].y0; *x1 = bu_bbcs[i].x1; *y1 = bu_bbcs[i].y1; return;
    }
    float ms = s100 / 100.f;
    if (ms > 2.f) ms = 2.f;                         /* the scratch buffer holds up to scale 2 */
    uint16_t *b = calloc((size_t)W * BU_MB_H, 2);
    int bx0 = W, by0 = BU_MB_H, bx1 = -1, by1 = -1;
    if (b) {
        int o0 = bu_y0, o1 = bu_y1; bu_y0 = 0; bu_y1 = BU_MB_H;
        bu_part(b, W / 2.f, BU_MB_H / 2.f, ms, fam, -1, part);
        bu_y0 = o0; bu_y1 = o1;
        for (int y = 0; y < BU_MB_H; y++) for (int x = 0; x < W; x++) if (b[y * W + x]) { if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
        free(b);
    }
    if (bx1 < 0) { bx0 = W / 2 - 10; bx1 = W / 2 + 10; by0 = BU_MB_H / 2 - 10; by1 = BU_MB_H / 2 + 10; }
    struct bu_bbc c = { fam, part, s100, (short)(bx0 - W / 2), (short)(by0 - BU_MB_H / 2), (short)(bx1 + 1 - W / 2), (short)(by1 + 1 - BU_MB_H / 2) };
    if (bu_nbbc < 64) bu_bbcs[bu_nbbc++] = c; else bu_bbcs[(unsigned)s100 % 64] = c;
    *x0 = c.x0; *y0 = c.y0; *x1 = c.x1; *y1 = c.y1;
}
/* parts of a layout: hero = buds pair + case (or the single unit), pair = the buds, one = one part */
enum { BL_HERO, BL_PAIR, BL_ONE };
#define BU_GAP_PAIR 8                               /* px between the two buds */
#define BU_GAP_CASE 18                              /* px between the buds and the case (>= 14) */
/* draws the product fitted in the box, centred as a whole, all parts on the same baseline (their
 * bottoms) and the case scaled to the buds' height. Gaps are fixed in pixels. */
static void bu_layout(uint16_t *d, float bx0, float by0, float bx1, float by1, int fam, int color, int mode, int one, float smax)
{
    int parts[3], np = 0;
    if (mode == BL_ONE) parts[np++] = one;
    else if (bu_single(fam)) parts[np++] = 4;
    else { parts[np++] = 1; parts[np++] = 2; if (mode == BL_HERO && fam != BF_BEATS_EAR) parts[np++] = 3; }
    /* natural sizes at scale 1, the case matched to the buds' height */
    float w[3], h[3], k[3], hb = 0, x0, y0, x1, y1;
    for (int i = 0; i < np; i++) { bu_part_bb(fam, parts[i], 1, &x0, &y0, &x1, &y1); w[i] = x1 - x0; h[i] = y1 - y0; if (parts[i] != 3 && h[i] > hb) hb = h[i]; }
    for (int i = 0; i < np; i++) k[i] = parts[i] == 3 && hb > 0 ? hb / h[i] : 1;
    float gaps = 0, wsum = 0, hmax = 0;
    for (int i = 0; i < np; i++) { wsum += w[i] * k[i]; if (h[i] * k[i] > hmax) hmax = h[i] * k[i]; if (i) gaps += parts[i] == 3 ? BU_GAP_CASE : BU_GAP_PAIR; }
    float s = fminf((bx1 - bx0 - gaps) / wsum, (by1 - by0) / hmax);
    if (s > smax) s = smax;
    for (int i = 0; i < np; i++) if (s * k[i] > 2) s = 2 / k[i];      /* the measured range of bu_part_bb */
    if (s < .2f) s = .2f;
    bu_lay = ++bu_lay_n & 255 ? bu_lay_n & 255 : ++bu_lay_n & 255;
    bu_mark(BT_LBOX, 0, bx0, by0, bx1, by1);
    /* exact extents at the final scales */
    float ex0[3], ey0[3], ex1[3], ey1[3], tot = gaps, hh = 0;
    for (int i = 0; i < np; i++) { bu_part_bb(fam, parts[i], s * k[i], &ex0[i], &ey0[i], &ex1[i], &ey1[i]); tot += ex1[i] - ex0[i]; if (ey1[i] - ey0[i] > hh) hh = ey1[i] - ey0[i]; }
    float x = roundf((bx0 + bx1) / 2 - tot / 2), base = roundf((by0 + by1) / 2 + hh / 2);
    for (int i = 0; i < np; i++) {
        if (i) x += parts[i] == 3 ? BU_GAP_CASE : BU_GAP_PAIR;
        float ox = x - ex0[i], oy = base - ey1[i];
        bu_part(d, ox, oy, s * k[i], fam, color, parts[i]);
        bu_mark(BT_PART, (unsigned char)parts[i], ox + ex0[i], oy + ey0[i], ox + ex1[i], oy + ey1[i]);
        x += ex1[i] - ex0[i];
    }
    bu_lay = 0;
}
/* the product as in Apple's settings (both buds and the case, or the headphones) in a box of
 * about 230 x 110 px at s = 1, centred on (cx, cy) */
static void buds_draw_product(uint16_t *d, float cx, float cy, float s, int family, int color)
{
    bu_layout(d, cx - 115 * s, cy - 55 * s, cx + 115 * s, cy + 55 * s, family, color, BL_HERO, 0, 3);
}
/* small glyph (widgets, rows) in a box of +-36 s around (cx, cy): one part, or the pair (4) */
static void buds_glyph(uint16_t *d, float cx, float cy, float s, int family, int part, int clip0, int clip1)
{
    int o0 = bu_y0, o1 = bu_y1; bu_y0 = clip0; bu_y1 = clip1;
    float r = 36 * s;
    if (part == 4 || bu_single(family)) bu_layout(d, cx - r, cy - r * .8f, cx + r, cy + r * .8f, family, -1, BL_PAIR, 0, 3);
    else bu_layout(d, cx - r * .8f, cy - r * .8f, cx + r * .8f, cy + r * .8f, family, -1, BL_ONE, part, 3);
    bu_y0 = o0; bu_y1 = o1;
}
/* battery ring with the percentage inside and a bolt above it when charging; col = column for
 * the layout checks. alpha fades the value in (popup) */
#define BU_RING_R 24
static void bu_ring(uint16_t *d, float cx, float cy, float r, int pct, int chg, int dim, int col, float alpha, uint16_t bg)
{
    uint16_t trk = rgb(52, 52, 56), c = pct < 0 ? BU_GRAY : pct <= 20 && chg != 1 ? BU_RED : BU_GREEN;
    if (dim) c = rgb(110, 110, 116);
    bu_arc(d, cx, cy, r, 4.5f, 0, 6.2832f, trk);
    bu_mark(BT_RING, col, cx - r - 2.25f, cy - r - 2.25f, cx + r + 2.25f, cy + r + 2.25f);
    if (alpha <= 0) return;
    unsigned a = (unsigned)(alpha * 255);
    if (pct > 0) bu_arc(d, cx, cy, r, 4.5f, 0, 6.2832f * (pct > 100 ? 100 : pct) / 100.f, a < 255 ? mix565(c, bg, a) : c);
    char b[12]; if (pct >= 0) snprintf(b, sizeof b, "%d%%", pct); else snprintf(b, sizeof b, "—");
    int sz = r > 22 ? 14 : 12, tw = tw_n(b, sz, -1), tx = (int)roundf(cx - tw / 2.f), base = (int)roundf(cy + sz * .36f);
    uint16_t tc = dim ? BU_GRAY : 0xFFFF; if (a < 255) tc = mix565(tc, bg, a);
    draw_text(d, tx, base, b, sz, tc, 0, bu_y0, bu_y1);
    bu_mark_text(BT_RTXT, col, tx, base, tw, sz);
    if (chg == 1 && !dim) {
        float bh = 10, bx = cx - bh * .31f - .5f, by = cy - r - 2.25f - 5 - bh;     /* 5 px above the ring */
        bu_bolt(d, bx, by, bh, a < 255 ? mix565(BU_GREEN, bg, a) : BU_GREEN);
        bu_mark(BT_BOLT, col, bx - .8f, by - .8f, bx + bh * .62f + .8f, by + bh + .8f);
    }
}
/* the ring slots of a device (same columns in the hero and the popup): L, R, case or one */
static int bu_ring_slots(const struct buds_st *s, int *part)
{
    int n = 0;
    if ((s->caps & BC_BATT_ONE) && (!(s->caps & BC_BATT_LR) || (s->batt[1] < 0 && s->batt[0] >= 0))) part[n++] = 10;
    else { part[n++] = 0; part[n++] = 1; if (s->caps & BC_BATT_CASE) part[n++] = 2; }
    return n;
}
#define BU_RING_STEP 92
static float bu_ring_x(float cx, int i, int n) { return roundf(cx + (i - (n - 1) / 2.f) * BU_RING_STEP); }
/* row of rings with their labels: centre cx, ring centre cy */
static void bu_rings_row(uint16_t *d, float cx, float cy, const struct buds_st *s, int dim)
{
    static const char *const lb[3] = { "Izquierdo", "Derecho", "Estuche" };
    int part[3], n = bu_ring_slots(s, part);
    for (int i = 0; i < n; i++) {
        float x = bu_ring_x(cx, i, n); int idx = part[i] >= 10 ? 0 : part[i];
        bu_ring(d, x, cy, BU_RING_R, s->batt[idx], s->chg[idx], dim, i, 1, 0);
        const char *t = part[i] >= 10 ? tr("Batería") : tr(lb[part[i]]);
        int sz = bu_fit(t, 12, BU_RING_STEP - 8), tw = tw_n(t, sz, -1), tx = (int)roundf(x - tw / 2.f), base = (int)(cy + BU_RING_R + 18);
        draw_text(d, tx, base, t, sz, BU_GRAY, 0, bu_y0, bu_y1);
        bu_mark_text(BT_RLBL, i, tx, base, tw, sz);
    }
}

/* ------------------------------------------------------------------ texts */
static const char *bu_mode_name(const struct buds_st *s, int m)
{
    switch (m) {
    case BN_OFF: return tr("Desactivado");
    case BN_ANC: return tr("Cancelación de ruido");
    case BN_AMBIENT: return s->brand == BB_APPLE || s->brand == BB_NOTHING ? tr("Transparencia") : tr("Sonido ambiente");
    default: return tr("Adaptativo");
    }
}
static const char *bu_mode_short(const struct buds_st *s, int m)
{
    switch (m) {
    case BN_OFF: return tr("No");
    case BN_ANC: return tr("Cancelación");
    case BN_AMBIENT: return s->brand == BB_APPLE || s->brand == BB_NOTHING ? tr("Transparencia") : tr("Ambiente");
    default: return tr("Adaptativo");
    }
}
static const char *const BU_ACTIONS[BA_N] = { "Ninguna", "Reproducir/pausa", "Pista siguiente", "Pista anterior", "Asistente de voz", "Control de ruido",
                                              "Sonido ambiente", "Cancelación de ruido", "Volumen", "Spotify", "Subir volumen", "Bajar volumen" };
static const char *const BU_GESTURES[BG_N] = { "Tocar", "Doble toque", "Triple toque", "Mantener pulsado" };
static const char *bu_action(const struct buds_st *s, int a)
{
    if (a < 0 || a >= BA_N) return tr("Otra");
    if (a == BA_VOICE && s->brand == BB_APPLE) return "Siri";
    return tr(BU_ACTIONS[a]);
}
static const char *bu_eq_name(const struct buds_st *s, int i)
{
    int n; const char *const *t = buds_eq_names(s->eq_set, &n);
    return i >= 0 && i < n ? tr(t[i]) : "—";
}
static const char *bu_display_name(const struct buds_st *s) { return s->name[0] ? s->name : s->model[0] ? s->model : s->addr; }
/* "L 80 % · R 75 % · Case 40 %" */
static void bu_batt_line(const struct buds_st *s, char *o, int n)
{
    o[0] = 0;
    if (s->caps & BC_BATT_ONE && s->batt[1] < 0) { if (s->batt[0] >= 0) snprintf(o, (size_t)n, "%d %%", s->batt[0]); return; }
    static const char *const lb[3] = { "I", "D", "Estuche" };
    for (int i = 0; i < 3; i++) if (s->batt[i] >= 0) { int l = (int)strlen(o); snprintf(o + l, (size_t)(n - l), "%s%s %d %%", l ? " · " : "", tr(lb[i]), s->batt[i]); }
}

/* ------------------------------------------------------------------ page model */
enum { BR_NAV, BR_TOGGLE, BR_INFO, BR_ACTION, BR_CHECK, BR_TEXT, BR_HERO, BR_SEG, BR_SLIDER, BR_DEV, BR_SUPP, BR_EQVIS, BR_FIND, BR_EMPTY };
enum { BFL_RED = 1, BFL_DIM = 2, BFL_BUSY = 4 };
enum { BP_LIST, BP_DEV, BP_GEST, BP_PICK, BP_EQ, BP_FINDP, BP_SUPP, BP_CYCLE };
/* row ids */
enum { BI_NONE, BI_DEV0 = 10, BI_SUPP = 30, BI_SEG, BI_ANCLVL, BI_AMBLVL, BI_ADAPTLVL, BI_CONV, BI_GEST, BI_EQ, BI_EARDET, BI_AUTOPAUSE,
       BI_LOCK, BI_GAME, BI_FIND, BI_NAME, BI_CONNECT, BI_DISCONNECT, BI_BASS, BI_FIT, BI_CYCLE, BI_PICK0 = 100, BI_GEST0 = 200, BI_EQ0 = 300,
       BI_BAND0 = 400, BI_CYC0 = 500, BI_FINDL = 600, BI_FINDR, BI_FINDSTOP };
struct brow { int kind, id, h, on, flags, val, mn, mx, fam, part; char t[64], v[64]; };
struct bsec { const char *hdr; char foot[240]; int first, n, plain; };
struct bpage { int kind; char title[64]; const char *back; char addr[18]; int p1, p2; struct bsec s[10]; int ns; struct brow r[64]; int nr; };
static struct bsec *bu_sec(struct bpage *pg, const char *hdr)
{
    if (pg->ns >= 10) return &pg->s[9];
    struct bsec *s = &pg->s[pg->ns++]; memset(s, 0, sizeof *s); s->hdr = hdr; s->first = pg->nr;
    return s;
}
static struct brow *bu_row(struct bpage *pg, int kind, int id, const char *t)
{
    static struct brow dummy;
    if (pg->nr >= 64 || !pg->ns) return &dummy;
    struct brow *r = &pg->r[pg->nr++]; memset(r, 0, sizeof *r);
    r->kind = kind; r->id = id; r->h = BU_ROWH;
    if (t) buds_cpy(r->t, sizeof r->t, t);
    pg->s[pg->ns - 1].n++;
    return r;
}
static void bu_foot(struct bpage *pg, const char *t) { if (pg->ns) buds_cpy(pg->s[pg->ns - 1].foot, sizeof pg->s[0].foot, t); }

static struct buds_st bv[BUDS_MAXDEV]; static int bvn; static struct buds_hdr bvh;
static unsigned bv_gen = ~0u;
static int bu_snap(void)
{
    unsigned g = bu_gen; int pend; pthread_mutex_lock(&bu_mu); pend = bu_np; pthread_mutex_unlock(&bu_mu);
    static int last_pend;
    if (g == bv_gen && pend == last_pend) return 0;
    bv_gen = g; last_pend = pend;
    bvn = bu_view(bv, BUDS_MAXDEV, &bvh);
    return 1;
}
static const struct buds_st *bu_dev(const char *addr) { int i = bu_find_dev(bv, bvn, addr); return i >= 0 ? &bv[i] : 0; }
static int bu_slide_id = -1, bu_slide_val;      /* slider being dragged: its live value */
static int bu_tog_id; static double bu_tog_t0; static int bu_tog_to;
static double bu_find_ok_t;                     /* the find warning was accepted (for 10 min) */

static void bu_build(struct bpage *pg)
{
    pg->ns = pg->nr = 0;
    const struct buds_st *s = pg->kind == BP_LIST || pg->kind == BP_SUPP ? 0 : bu_dev(pg->addr);
    char b[240];
    if (pg->kind == BP_LIST) {
        buds_cpy(pg->title, sizeof pg->title, tr("Auriculares"));
        if (!bvn) {
            bu_sec(pg, 0)->plain = 1;
            struct brow *r = bu_row(pg, BR_EMPTY, 0, 0); r->h = 230;
            bu_sec(pg, 0);
        } else {
            bu_sec(pg, "MIS AURICULARES");
            for (int i = 0; i < bvn; i++) {
                const struct buds_st *d = &bv[i];
                struct brow *r = bu_row(pg, BR_DEV, BI_DEV0 + i, bu_display_name(d)); r->h = 64; r->fam = d->family;
                if (d->busy) { r->flags |= BFL_BUSY; buds_cpy(r->v, sizeof r->v, tr("Un momento…")); }
                else if (d->conn == BS_NONE) buds_cpy(r->v, sizeof r->v, tr("No conectado"));
                else if (d->conn == BS_READY) { bu_batt_line(d, r->v, sizeof r->v); if (!r->v[0]) buds_cpy(r->v, sizeof r->v, tr("Conectado")); }
                else if (d->conn == BS_LINKING || d->conn == BS_AUDIO) { r->flags |= BFL_BUSY; buds_cpy(r->v, sizeof r->v, tr("Conectando con los controles…")); }
                else buds_cpy(r->v, sizeof r->v, tr("Conectado"));
                r->on = d->conn != BS_NONE;
            }
            bu_foot(pg, tr("BUDS_LIST_HELP"));
        }
        if (!bvh.pid) { bu_sec(pg, 0); struct brow *r = bu_row(pg, BR_TEXT, 0, tr("Iniciando el servicio de auriculares…")); r->flags |= BFL_BUSY; }
        bu_sec(pg, 0); bu_row(pg, BR_NAV, BI_SUPP, tr("Dispositivos compatibles"));
        return;
    }
    if (pg->kind == BP_SUPP) {
        buds_cpy(pg->title, sizeof pg->title, tr("Dispositivos compatibles"));
        static struct buds_cat c[BUDS_CAT_MAX]; static int nc;
        if (!nc) nc = buds_catalog(c, BUDS_CAT_MAX);
        int last = -1;
        for (int i = 0; i < nc; i++) {
            if (c[i].s.brand != last) {
                last = c[i].s.brand; bu_sec(pg, buds_brand_name(last));
                if (last == BB_SONY) bu_foot(pg, tr("BUDS_SONY_RO"));
                if (last == BB_OTHER) bu_foot(pg, tr("BUDS_FASTPAIR_HELP"));
                if (last == BB_APPLE) bu_foot(pg, tr("BUDS_SPOOF_NOTE"));
            }
            struct brow *r = bu_row(pg, BR_SUPP, 0, c[i].m->name[0] ? c[i].m->name : tr("Otros auriculares (Fast Pair)"));
            r->h = 52; r->fam = c[i].s.family;
            for (int f = 0; f < BX_N; f++) if (buds_has(&c[i].s, f)) r->val |= 1 << f;
            if (c[i].s.caps & BC_RO) r->flags |= BFL_DIM;
        }
        bu_sec(pg, 0); bu_foot(pg, tr("BUDS_SUPP_LEGEND"));
        return;
    }
    if (!s) { buds_cpy(pg->title, sizeof pg->title, tr("Auriculares")); bu_sec(pg, 0); bu_row(pg, BR_TEXT, 0, tr("Dispositivo no disponible")); return; }
    int ready = s->conn == BS_READY, ro = (s->caps & BC_RO) != 0, dim = (!ready || ro) ? BFL_DIM : 0;
    buds_cpy(pg->title, sizeof pg->title, pg->kind == BP_DEV ? bu_display_name(s) : "");
    switch (pg->kind) {
    case BP_DEV: {
        bu_sec(pg, 0)->plain = 1;
        struct brow *r = bu_row(pg, BR_HERO, 0, 0); r->h = 216; r->fam = s->family;
        if (s->conn == BS_NONE) {
            bu_sec(pg, 0);
            r = bu_row(pg, BR_ACTION, BI_CONNECT, s->busy ? tr("Conectando…") : tr("Conectar")); if (s->busy) r->flags |= BFL_BUSY;
            bu_foot(pg, tr("BUDS_CONNECT_HELP"));
        } else if (s->conn != BS_READY) {
            bu_sec(pg, 0);
            if (s->conn == BS_NOCTL || (s->brand == BB_OTHER && !(s->caps & BC_BATT_LR))) { bu_row(pg, BR_TEXT, 0, tr("Controles no disponibles")); bu_foot(pg, tr("BUDS_NOCTL_HELP")); }
            else { r = bu_row(pg, BR_TEXT, 0, tr("Conectando con los controles…")); r->flags |= BFL_BUSY; }
        }
        if (s->caps & BC_NOISE) {
            bu_sec(pg, "CONTROL DE RUIDO");
            r = bu_row(pg, BR_SEG, BI_SEG, 0); r->h = 100; r->val = s->noise; r->flags |= dim;
            if (s->noise == BN_ANC && (s->caps & BC_ANC_LVL)) { r = bu_row(pg, BR_SLIDER, BI_ANCLVL, tr("Intensidad de la cancelación")); r->h = 66; r->val = s->anc_lvl; r->mx = s->anc_max; r->flags |= dim; }
            if (s->noise == BN_AMBIENT && (s->caps & BC_AMB_LVL)) { r = bu_row(pg, BR_SLIDER, BI_AMBLVL, tr("Nivel de sonido ambiente")); r->h = 66; r->val = s->amb_lvl; r->mx = s->amb_max; r->flags |= dim; }
            if (s->noise == BN_ADAPTIVE && (s->caps & BC_ADAPT_LVL)) { r = bu_row(pg, BR_SLIDER, BI_ADAPTLVL, tr("Personalizar el modo adaptativo")); r->h = 66; r->val = s->adapt_lvl; r->mx = 100; r->flags |= dim; }
            if (s->caps & BC_CONV) { r = bu_row(pg, BR_TOGGLE, BI_CONV, s->brand == BB_APPLE ? tr("Detección de conversación") : tr("Enfoque en la voz")); r->on = s->conv > 0; r->flags |= dim; }
            if (s->caps & BC_SPOOF) bu_foot(pg, tr("BUDS_SPOOF_NOTE"));
        }
        if (s->caps & (BC_GEST | BC_CYCLE | BC_LOCK)) {
            bu_sec(pg, s->brand == BB_APPLE ? "MANTENER PULSADO" : "CONTROLES TÁCTILES");
            r = bu_row(pg, BR_NAV, BI_GEST, s->brand == BB_APPLE ? tr("Modos de control de ruido") : tr("Gestos")); r->flags |= dim & ~BFL_DIM;
        }
        if (s->caps & (BC_EQ | BC_BASS)) {
            bu_sec(pg, "SONIDO");
            if (s->caps & BC_EQ) { r = bu_row(pg, BR_NAV, BI_EQ, tr("Ecualizador")); buds_cpy(r->v, sizeof r->v, s->eq >= 0 ? bu_eq_name(s, s->eq) : ""); }
            if (s->caps & BC_BASS) { r = bu_row(pg, BR_SLIDER, BI_BASS, s->brand == BB_SONY ? "Clear Bass" : tr("Graves extra")); r->h = 66; r->val = s->bass; r->mx = s->bass_max; r->flags |= dim; }
            if (s->caps & BC_GAME) { r = bu_row(pg, BR_TOGGLE, BI_GAME, tr("Modo de baja latencia")); r->on = s->game > 0; r->flags |= dim; }
        }
        bu_sec(pg, 0);
        if (s->caps & (BC_WEAR | BC_EARDET)) {
            if (s->caps & BC_EARDET) { r = bu_row(pg, BR_TOGGLE, BI_EARDET, tr("Detección automática de oído")); r->on = s->eardet != 0; r->flags |= dim; }
            if (s->caps & BC_WEAR) { r = bu_row(pg, BR_TOGGLE, BI_AUTOPAUSE, tr("Pausar al quitártelos")); r->on = bu_autopause(s->addr); }
        }
        if (s->caps & BC_LOCK) { r = bu_row(pg, BR_TOGGLE, BI_LOCK, tr("Bloquear controles táctiles")); r->on = s->lock > 0; r->flags |= dim; }
        if (s->caps & BC_FIT) { r = bu_row(pg, BR_NAV, BI_FIT, tr("Prueba de ajuste de las almohadillas")); if (s->fit[0] >= 0) buds_cpy(r->v, sizeof r->v, s->fit[0] || s->fit[1] ? tr("Mejorable") : tr("Buen ajuste")); }
        if (s->caps & BC_FIND) bu_row(pg, BR_NAV, BI_FIND, tr("Buscar mis auriculares"));
        r = bu_row(pg, BR_NAV, BI_NAME, tr("Nombre")); buds_cpy(r->v, sizeof r->v, bu_display_name(s));
        if (!pg->s[pg->ns - 1].n) pg->ns--;
        if (ro && ready) { bu_sec(pg, 0); bu_row(pg, BR_TEXT, 0, tr("Solo lectura")); bu_foot(pg, tr("BUDS_RO_HELP")); }
        bu_sec(pg, "INFORMACIÓN");
        r = bu_row(pg, BR_INFO, 0, tr("Modelo")); buds_cpy(r->v, sizeof r->v, s->model[0] ? s->model : tr("Desconocido"));
        if (s->modelno[0]) { r = bu_row(pg, BR_INFO, 0, tr("Número de modelo")); buds_cpy(r->v, sizeof r->v, s->modelno); }
        if (s->fw[0]) { r = bu_row(pg, BR_INFO, 0, tr("Versión del firmware")); buds_cpy(r->v, sizeof r->v, s->fw); }
        if (s->serial[0]) { r = bu_row(pg, BR_INFO, 0, tr("Número de serie")); buds_cpy(r->v, sizeof r->v, s->serial); }
        r = bu_row(pg, BR_INFO, 0, tr("Dirección")); buds_cpy(r->v, sizeof r->v, s->addr);
        if (s->conn != BS_NONE) { bu_sec(pg, 0); r = bu_row(pg, BR_ACTION, BI_DISCONNECT, s->busy ? tr("Un momento…") : tr("Desconectar")); if (s->busy) r->flags |= BFL_BUSY; }
        break;
    }
    case BP_GEST: {
        buds_cpy(pg->title, sizeof pg->title, s->brand == BB_APPLE ? tr("Mantener pulsado") : tr("Gestos"));
        struct brow *r;
        if (s->caps & BC_CYCLE) {
            bu_sec(pg, "CAMBIAR ENTRE");
            for (int m = 0; m < BN_N; m++) if (s->noise_mask & BUDS_MASK(m)) { r = bu_row(pg, BR_CHECK, BI_CYC0 + m, bu_mode_name(s, m)); r->on = s->cycle >= 0 && (s->cycle & (1 << m)); r->flags |= dim; }
            bu_foot(pg, tr("BUDS_CYCLE_HELP"));
        }
        if (s->caps & BC_LOCK) { bu_sec(pg, 0); r = bu_row(pg, BR_TOGGLE, BI_LOCK, tr("Bloquear controles táctiles")); r->on = s->lock > 0; r->flags |= dim; bu_foot(pg, tr("BUDS_LOCK_HELP")); }
        if (s->caps & BC_GEST) for (int side = 0; side < 2; side++) {
            bu_sec(pg, side ? "DERECHO" : "IZQUIERDO");
            for (int g = 0; g < BG_N; g++) if (s->gest_opts[g]) {
                r = bu_row(pg, BR_NAV, BI_GEST0 + side * 10 + g, tr(BU_GESTURES[g])); buds_cpy(r->v, sizeof r->v, s->gest[side][g] >= 0 ? bu_action(s, s->gest[side][g]) : "");
                r->flags |= (s->lock > 0 ? BFL_DIM : 0);
            }
        }
        break;
    }
    case BP_PICK: {
        int side = pg->p1, g = pg->p2;
        snprintf(b, sizeof b, "%s · %s", tr(side ? "Derecho" : "Izquierdo"), tr(BU_GESTURES[g]));
        buds_cpy(pg->title, sizeof pg->title, b);
        bu_sec(pg, 0);
        for (int a = 0; a < BA_N; a++) if (s->gest_opts[g] & (1u << a)) { struct brow *r = bu_row(pg, BR_CHECK, BI_PICK0 + a, bu_action(s, a)); r->on = s->gest[side][g] == a; r->flags |= dim; }
        bu_foot(pg, tr("BUDS_GEST_HELP"));
        break;
    }
    case BP_EQ: {
        buds_cpy(pg->title, sizeof pg->title, tr("Ecualizador"));
        bu_sec(pg, 0)->plain = 1;
        struct brow *r = bu_row(pg, BR_EQVIS, 0, 0); r->h = 118; r->val = s->eq;
        bu_sec(pg, "PREAJUSTES");
        int n; buds_eq_names(s->eq_set, &n); if (s->eq_n < n) n = s->eq_n;
        for (int i = 0; i < n; i++) { r = bu_row(pg, BR_CHECK, BI_EQ0 + i, bu_eq_name(s, i)); r->on = s->eq == i; r->flags |= dim; }
        if ((s->caps & BC_EQ_BANDS) && s->brand == BB_NOTHING) {
            bu_sec(pg, "PERSONALIZADO");
            static const char *const bn[3] = { "Graves", "Medios", "Agudos" };
            for (int i = 0; i < 3; i++) { r = bu_row(pg, BR_SLIDER, BI_BAND0 + i, tr(bn[i])); r->h = 66; r->mn = s->band_min; r->mx = s->band_max; r->val = s->nbands >= 3 ? s->bands[i] : 0; r->flags |= dim; }
            bu_foot(pg, tr("BUDS_BANDS_HELP"));
        } else if (s->nbands > 0) {
            bu_sec(pg, "BANDAS");
            for (int i = 0; i < s->nbands; i++) { struct brow *x = bu_row(pg, BR_INFO, 0, 0); snprintf(x->t, sizeof x->t, "%d", i + 1); snprintf(x->v, sizeof x->v, "%+d dB", s->bands[i]); }
        }
        break;
    }
    case BP_FINDP: {
        buds_cpy(pg->title, sizeof pg->title, tr("Buscar mis auriculares"));
        bu_sec(pg, 0)->plain = 1;
        struct brow *r = bu_row(pg, BR_FIND, 0, 0); r->h = 196; r->fam = s->family; r->flags |= dim;
        r->val = (s->find[0] ? 1 : 0) | (s->find[1] ? 2 : 0);
        bu_sec(pg, 0); bu_foot(pg, tr("BUDS_FIND_HELP"));
        break;
    }
    }
}

/* ------------------------------------------------------------------ rendering */
static float bu_scroll;
static double bu_render_ms, bu_render_max; static int bu_render_n;
static int bu_foot_h(const char *f) { return f[0] ? 8 + draw_wrapped(0, 0, 0, W - 48, f, 12, 0, 16, 0) : 0; }
static int bu_sec_h(const struct bpage *pg, int si)
{
    const struct bsec *s = &pg->s[si]; int h = s->plain ? 0 : s->hdr ? 28 : 14;
    for (int k = 0; k < s->n; k++) h += pg->r[s->first + k].h;
    return h + bu_foot_h(s->foot) + (s->plain ? 0 : 14);
}
static int bu_content_h(const struct bpage *pg) { int h = 12; for (int s = 0; s < pg->ns; s++) h += bu_sec_h(pg, s); return h + 24; }
/* noise mode glyph: a person (head + shoulders) with the mode's marks */
static void bu_mode_icon(uint16_t *d, float cx, float cy, int m, uint16_t c, uint16_t bg)
{
    fill_circle(d, cx, cy - 5, 4.2f, c, 255);
    bu_arc(d, cx, cy + 9, 7.5f, 3, -1.45f, 1.45f, c);
    if (m == BN_AMBIENT) for (int k = -1; k <= 1; k += 2) { bu_arc(d, cx, cy, 13, 1.8f, k > 0 ? .55f : -2.6f, k > 0 ? 2.6f : -.55f, c); }
    if (m == BN_ANC) { bu_arc(d, cx, cy, 14, 3, 0, 6.2832f, c); }
    if (m == BN_ADAPTIVE) {
        bu_arc(d, cx, cy, 14, 1.8f, -2.6f, -.55f, c);
        float sx = cx + 11, sy = cy - 9; bu_line(d, sx - 5, sy, sx + 5, sy, 1.6f, c); bu_line(d, sx, sy - 5, sx, sy + 5, 1.6f, c);
    }
    if (m == BN_OFF) bu_line(d, cx - 10, cy + 10, cx + 10, cy - 10, 2, bg), bu_line(d, cx - 11, cy + 11, cx + 11, cy - 11, 1.4f, c);
}
/* feature chip glyphs of the Supported Devices rows (battery, noise, EQ, gestures, ear, find) */
static void bu_feat(uint16_t *d, float x, float cy, int f, uint16_t c)
{
    switch (f) {
    case BX_BATT: fill_rrect(d, (int)x - 7, (int)cy - 4, 13, 8, 2, c, 255); fill_rrect(d, (int)x + 6, (int)cy - 2, 2, 4, 1, c, 255); fill_rrect(d, (int)x - 5, (int)cy - 2, 7, 4, 1, BU_CARD, 255); break;
    case BX_ANC: bu_arc(d, x, cy, 6, 1.6f, 0, 6.2832f, c); fill_circle(d, x, cy, 2.5f, c, 255); break;
    case BX_EQ: for (int k = 0; k < 3; k++) { bu_line(d, x - 5 + k * 5, cy - 6, x - 5 + k * 5, cy + 6, 1.4f, c); fill_circle(d, x - 5 + k * 5, cy + (k == 1 ? -2 : 3), 2.2f, c, 255); } break;
    case BX_GEST: fill_circle(d, x, cy - 3, 2.5f, c, 255); bu_arc(d, x, cy - 3, 6, 1.4f, -1.2f, 1.2f, c); bu_line(d, x, cy, x, cy + 7, 2, c); break;
    case BX_EAR: bu_arc(d, x, cy - 1, 5.5f, 1.8f, -1.7f, 2.2f, c); bu_line(d, x - 2, cy + 5, x - 3, cy + 7, 1.8f, c); break;
    case BX_FIND: bu_arc(d, x - 1.5f, cy - 1.5f, 4.5f, 1.8f, 0, 6.2832f, c); bu_line(d, x + 2, cy + 2, x + 6, cy + 6, 2, c); break;
    }
}
static void bu_slider(uint16_t *d, int x, int w, int cy, int val, int mn, int mx, int dim)
{
    float f = mx > mn ? (float)((val < mn ? mn : val > mx ? mx : val) - mn) / (mx - mn) : 0;
    int tx = x + 14, tw = w - 28;
    fill_rrect(d, tx, cy - 2, tw, 4, 2, rgb(72, 72, 76), 255);
    if (val >= mn) fill_rrect(d, tx, cy - 2, (int)(tw * f) + 2, 4, 2, dim ? BU_GRAY : BU_BLUE, 255);
    if (mx - mn <= 8) for (int k = 0; k <= mx - mn; k++) fill_circle(d, tx + tw * (float)k / (mx - mn), cy, 2, rgb(110, 110, 116), 255);
    if (val >= mn) { fill_circle(d, tx + tw * f, cy + 1.5f, 14.5f, rgb(10, 10, 12), 80); fill_circle(d, tx + tw * f, cy, 13.5f, dim ? rgb(170, 170, 174) : 0xFFFF, 255); }
}
/* EQ curve of a preset (shape only, as a picture of the sound) */
static void bu_eqvis(uint16_t *d, int x, int y, int w, int h, const struct buds_st *s)
{
    static const signed char shapes[ES_N][10][5] = {
        { { 0, 0, 0, 0, 0 }, { 5, 3, 0, 0, 0 }, { -1, 0, 0, -1, -2 }, { 3, 1, -1, 1, 3 }, { -1, 0, 1, 3, 2 }, { 0, 0, 0, 2, 5 } },
        { { 0, 0, 0, 0, 0 }, { -2, 0, 3, 2, 0 }, { 0, 0, 0, 2, 4 }, { 4, 2, 0, 0, 0 }, { 0, 0, 0, 0, 0 } },
        { { 3, 1, -1, 2, 3 }, { 4, 2, 0, 2, 4 }, { 2, 1, 2, 1, 2 }, { -1, 0, 3, 2, 0 }, { 1, 0, 0, 1, 2 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 } },
        { { 0, 0, 0, 0, 0 }, { -1, 0, 1, 3, 2 }, { 3, 2, 0, 1, 2 }, { 1, 1, 0, -1, -1 }, { 2, 1, 0, 0, -1 }, { -1, 1, 3, 1, 0 }, { 0, 0, 0, 2, 5 }, { 5, 3, 0, 0, 0 }, { -2, 0, 3, 1, -1 }, { 0, 0, 0, 0, 0 } },
    };
    bu_card(d, x, y, w, h, 12, BU_CARD);
    int set = s->eq_set >= 0 && s->eq_set < ES_N ? s->eq_set : 0, e = s->eq < 0 ? 0 : s->eq > 9 ? 9 : s->eq;
    float g[5]; for (int i = 0; i < 5; i++) g[i] = shapes[set][e][i];
    int custom = (s->brand == BB_NOTHING && s->nbands >= 3 && ((set == ES_NOTHING && e == 4) || (set == ES_CMF && e == 5)));
    if (custom) { g[0] = (float)s->bands[0]; g[1] = (s->bands[0] + s->bands[1]) / 2.f; g[2] = (float)s->bands[1]; g[3] = (s->bands[1] + s->bands[2]) / 2.f; g[4] = (float)s->bands[2]; }
    float my = y + (h - 14) / 2.f, sc = ((h - 14) / 2.f - 10) / 6.f;
    for (int k = -1; k <= 1; k++) for (int xx = x + 16; xx < x + w - 16; xx += 3) bu_px(d, xx, (int)(my + k * 6 * sc), rgb(70, 70, 74), 1);
    float px = 0, py = 0;
    for (int i = 0; i <= 64; i++) {
        float t = i / 64.f * 4, f = t - (int)t; int k = (int)t; if (k > 3) { k = 3; f = 1; }
        float a = g[k], bb = g[k + 1], v = a + (bb - a) * (3 * f * f - 2 * f * f * f);
        float X = x + 16 + (w - 32) * i / 64.f, Y = my - v * sc;
        if (i) bu_line(d, px, py, X, Y, 3, BU_BLUE);
        px = X; py = Y;
    }
    static const char *const fr[5] = { "60", "250", "1k", "4k", "12k" };
    for (int i = 0; i < 5; i++) {
        int tw = tw_n(fr[i], 10, -1), X = (int)roundf(x + 16 + (w - 32) * i / 4.f - tw / 2.f);
        if (X < x + 8) X = x + 8; if (X + tw > x + w - 8) X = x + w - 8 - tw;
        draw_text(d, X, y + h - 7, fr[i], 10, BU_GRAY, 0, bu_y0, bu_y1); bu_mark_text(BT_EQAX, i, X, y + h - 7, tw, 10);
    }
}
/* noise control segments: fixed width, the group centred (same spacing for 3 and 4 modes) */
#define BU_SEG_W 68
static int bu_seg_modes(const struct buds_st *s, int *modes)
{
    static const int order[BN_N] = { BN_OFF, BN_AMBIENT, BN_ADAPTIVE, BN_ANC };
    int n = 0; for (int i = 0; i < BN_N; i++) if (s->noise_mask & BUDS_MASK(order[i])) modes[n++] = order[i];
    return n;
}
static float bu_seg_cx(int i, int n) { return roundf(W / 2.f + (i - (n - 1) / 2.f) * BU_SEG_W); }
/* label of a segment: shrinks to 9 px, then two lines (split at the space nearest the middle) */
static void bu_seg_label(uint16_t *d, float cx, int base, const char *t, uint16_t c, int col)
{
    int maxw = BU_SEG_W - 6, sz = 11;
    while (sz > 9 && tw_n(t, sz, -1) > maxw) sz--;
    if (tw_n(t, sz, -1) <= maxw) {
        int tw = tw_n(t, sz, -1), tx = (int)roundf(cx - tw / 2.f);
        draw_text(d, tx, base, t, sz, c, 0, bu_y0, bu_y1); bu_mark_text(BT_SEGL, col, tx, base, tw, sz);
        return;
    }
    int n = (int)strlen(t), best = -1;
    for (int i = 1; i < n; i++) if (t[i] == ' ' && (best < 0 || abs(i - n / 2) < abs(best - n / 2))) best = i;
    char a[64], b[64];
    if (best < 0) { int tw = maxw, tx = (int)roundf(cx - tw / 2.f); draw_text(d, tx, base, t, 9, c, maxw, bu_y0, bu_y1); bu_mark_text(BT_SEGL, col, tx, base, tw, 9); return; }
    snprintf(a, sizeof a, "%.*s", best, t); snprintf(b, sizeof b, "%s", t + best + 1);
    for (int k = 0; k < 2; k++) {
        const char *l = k ? b : a; int s2 = bu_fit(l, 10, maxw), tw = tw_n(l, s2, -1); if (tw > maxw) tw = maxw;
        int tx = (int)roundf(cx - tw / 2.f), bl = base - 6 + k * 12;
        draw_text(d, tx, bl, l, s2, c, maxw, bu_y0, bu_y1); bu_mark_text(BT_SEGL, col, tx, bl, tw, s2);
    }
}
/* iOS navigation bar: the title centred and truncated only by itself; the back label shows the
 * previous title if it fits left of the title, else "Back", else only the chevron */
static void bu_navbar(uint16_t *d, const char *title, const char *back)
{
    if (!back) { draw_text(d, 16, 80, title, 26, 0xFFFF, W - 32, 0, H); bu_mark_text(BT_TITLE, 0, 16, 80, tw_n(title, 26, -1) > W - 32 ? W - 32 : tw_n(title, 26, -1), 26); return; }
    const int tmax = W - 2 * 44;
    int tw = tw_n(title, 17, -1); if (tw > tmax) tw = tmax;
    int tx = (W - tw) / 2;
    draw_text(d, tx, 68, title, 17, 0xFFFF, tmax, 0, H); bu_mark_text(BT_TITLE, 0, tx, 68, tw, 17);
    bu_line(d, 21, 55, 14.5f, 62, 2.6f, BU_BLUE); bu_line(d, 14.5f, 62, 21, 69, 2.6f, BU_BLUE);
    int avail = tx - 28 - 10, bw = 0;
    const char *bl = tr(back);
    if (tw_n(bl, 16, -1) > avail) bl = tr("Atrás");
    if (tw_n(bl, 16, -1) <= avail) { bw = tw_n(bl, 16, -1); draw_text(d, 28, 68, bl, 16, BU_BLUE, 0, 0, H); }
    bu_mark(BT_BACK, 0, 12, 53, 28 + bw, 71);
}
static void bu_render(uint16_t *d, const struct bpage *pg, int sel)
{
    double t0 = now_ms();
    int ytop = BU_TOP;
    memset(d + ytop * W, 0, (size_t)(H - ytop) * W * 2);
    bu_y0 = ytop; bu_y1 = H;
    const struct buds_st *s = bu_dev(pg->addr);
    int y = ytop + 12 - (int)bu_scroll;
    for (int si = 0; si < pg->ns; si++) {
        const struct bsec *sc = &pg->s[si];
        if (sc->hdr && !sc->plain) { if (y + 20 > ytop && y < H) draw_text(d, 32, y + 20, tr(sc->hdr), 12, BU_GRAY, W - 64, bu_y0, bu_y1); y += 28; }
        else if (!sc->plain) y += 14;
        int top = y, ch = 0;
        for (int k = 0; k < sc->n; k++) ch += pg->r[sc->first + k].h;
        int card = !sc->plain && sc->n && pg->r[sc->first].kind != BR_HERO;
        if (card && top < H && top + ch > ytop) bu_card(d, 16, top, W - 32, ch, 10, BU_CARD);
        int ry = top;
        for (int k = 0; k < sc->n; k++) {
            const struct brow *r = &pg->r[sc->first + k];
            int rh = r->h, cy = ry + rh / 2, idx = sc->first + k;
            bu_grp = idx;
            if (ry >= H) break;
            if (ry + rh <= ytop) { ry += rh; continue; }
            int tappable = r->kind == BR_NAV || r->kind == BR_TOGGLE || r->kind == BR_ACTION || r->kind == BR_CHECK || r->kind == BR_DEV;
            if (idx == sel && tappable && card) {
                bu_card(d, 16, ry, W - 32, rh, (k == 0 || k == sc->n - 1) ? 10 : 0, BU_SEL);
                if (sc->n > 1 && k == 0) bu_card(d, 16, ry + rh - 10, W - 32, 10, 0, BU_SEL);
                if (sc->n > 1 && k == sc->n - 1) bu_card(d, 16, ry, W - 32, 10, 0, BU_SEL);
            }
            int tx = r->kind == BR_DEV || r->kind == BR_SUPP ? 74 : 32;
            if (k && card && ry >= ytop) for (int x = tx; x < W - 16; x++) d[ry * W + x] = BU_SEP;
            uint16_t tc = r->flags & BFL_DIM ? BU_GRAY : 0xFFFF;
            int right = W - 28;
            switch (r->kind) {
            case BR_EMPTY: {
                bu_layout(d, 40, ry + 16, W - 40, ry + 118, BF_PODS_PRO, -1, BL_HERO, 0, 2);
                bu_textc(d, W / 2, ry + 150, tr("Sin auriculares"), 20, 0xFFFF, W - 40);
                bu_mark(BT_EMPTYTXT, 0, 20, ry + 133, W - 20, ry + 155);
                draw_wrapped(d, 32, ry + 168, W - 64, tr("BUDS_EMPTY_HELP"), 13, BU_GRAY, 17, 1);
                break;
            }
            case BR_HERO: if (s) {
                /* product box above the rings (bolts 5 px above the rings, 10 px clear of the product) */
                bu_layout(d, 24, ry + 8, W - 24, ry + 106, s->family, s->color, BL_HERO, 0, 2);
                bu_rings_row(d, W / 2.f, ry + 160, s, s->conn != BS_READY);
            } break;
            case BR_DEV:
                bu_layout(d, 24, cy - 17, 66, cy + 17, r->fam, -1, BL_PAIR, 0, 2);
                { int nw = tw_n(r->t, 16, -1), vw = tw_n(r->v, 12, -1); if (nw > W - tx - 46) nw = W - tx - 46; if (vw > W - tx - 66) vw = W - tx - 66;
                  draw_text(d, tx, cy - 4, r->t, 16, r->on ? 0xFFFF : rgb(200, 200, 204), W - tx - 46, bu_y0, bu_y1); bu_mark_text(BT_LTXT, 0, tx, cy - 4, nw, 16);
                  if (r->flags & BFL_BUSY) bu_spinner(d, tx + 7, cy + 12, 6, t0);
                  draw_text(d, tx + (r->flags & BFL_BUSY ? 18 : 0), cy + 16, r->v, 12, r->on ? BU_GREEN : BU_GRAY, W - tx - 66, bu_y0, bu_y1); bu_mark_text(BT_LTXT, 1, tx + (r->flags & BFL_BUSY ? 18 : 0), cy + 16, vw, 12); }
                bu_chev(d, W - 34, cy, rgb(90, 90, 96));
                break;
            case BR_SUPP:
                bu_layout(d, 24, cy - 15, 66, cy + 15, r->fam, -1, BL_PAIR, 0, 2);
                { int nw = tw_n(r->t, 15, -1); if (nw > W - tx - 30) nw = W - tx - 30;
                  draw_text(d, tx, cy - 3, r->t, 15, tc, W - tx - 30, bu_y0, bu_y1); bu_mark_text(BT_LTXT, 0, tx, cy - 3, nw, 15); }
                for (int f = 0; f < BX_N; f++) bu_feat(d, tx + 8 + f * 24, cy + 12, f, r->val & (1 << f) ? (r->flags & BFL_DIM ? rgb(190, 190, 196) : BU_GREEN) : rgb(64, 64, 68));
                bu_mark(BT_LTXT, 1, tx, cy + 5, tx + 8 + (BX_N - 1) * 24 + 8, cy + 19);
                break;
            case BR_SEG: if (s) {
                int modes[BN_N], nm = bu_seg_modes(s, modes);
                for (int i = 0; i < nm; i++) {
                    float cx = bu_seg_cx(i, nm); int on = r->val == modes[i];
                    uint16_t bg = on ? (r->flags & BFL_DIM ? rgb(170, 170, 174) : 0xFFFF) : rgb(58, 58, 62), fg = on ? rgb(20, 20, 22) : 0xFFFF;
                    fill_circle(d, cx, ry + 36, 23, bg, 255);
                    bu_mode_icon(d, cx, ry + 36, modes[i], fg, bg);
                    bu_mark(BT_SEGI, i, cx - 23.5f, ry + 12.5f, cx + 23.5f, ry + 59.5f);
                    bu_seg_label(d, cx, ry + 82, bu_mode_short(s, modes[i]), on ? 0xFFFF : BU_GRAY, i);
                }
            } break;
            case BR_SLIDER: {
                int v = bu_slide_id == r->id ? bu_slide_val : r->val;
                draw_text(d, 32, ry + 24, r->t, 13, BU_GRAY, W - 120, bu_y0, bu_y1);
                char b[16];
                if (v >= r->mn && r->mx > 0) {
                    if (r->id >= BI_BAND0 && r->id < BI_BAND0 + 10) snprintf(b, sizeof b, "%+d dB", v);
                    else if (r->id == BI_BASS && v == 0) snprintf(b, sizeof b, "%s", tr("No"));
                    else snprintf(b, sizeof b, "%d", r->mx == 100 ? v : v - r->mn + (r->id == BI_ANCLVL || r->id == BI_AMBLVL ? 1 : 0));
                    int bw = tw_n(b, 13, -1); draw_text(d, W - 32 - bw, ry + 24, b, 13, 0xFFFF, 0, bu_y0, bu_y1);
                }
                bu_slider(d, 16, W - 32, ry + 46, v, r->mn, r->mx, r->flags & BFL_DIM);
                break;
            }
            case BR_EQVIS: if (s) bu_eqvis(d, 16, ry, W - 32, rh - 8, s); break;
            case BR_FIND: if (s) {
                int single = bu_single(s->family), ns = single ? 1 : 2;
                for (int side = 0; side < ns; side++) {
                    int on = (r->val >> side) & 1; float cx = single ? W / 2.f : side ? W * 3 / 4.f : W / 4.f;
                    if (single) bu_layout(d, cx - 70, ry + 14, cx + 70, ry + 104, s->family, s->color, BL_PAIR, 0, 2);
                    else bu_layout(d, cx - 48, ry + 14, cx + 48, ry + 104, s->family, s->color, BL_ONE, side + 1, 2);
                    if (on) for (int k = 1; k <= 2; k++) bu_arc(d, cx, ry + 59, 46 + k * 8 + (float)fmod(t0 / 40, 8), 2, 0, 6.2832f, mix565(BU_BLUE, 0, (unsigned)(200 - k * 60)));
                    int bw = 120, bx = (int)cx - bw / 2, by = ry + 136;
                    bu_card(d, bx, by, bw, 44, 22, on ? BU_RED : (r->flags & BFL_DIM ? rgb(58, 58, 62) : BU_BLUE));
                    bu_mark(BT_FINDBTN, side, bx, by, bx + bw, by + 44);
                    const char *lbl = on ? tr("Detener") : single ? tr("Reproducir sonido") : tr(side ? "Derecho" : "Izquierdo");
                    bu_textc(d, (int)cx, by + 28, lbl, 15, 0xFFFF, bw - 16);
                }
            } break;
            case BR_TOGGLE: {
                float p = r->on;
                if (bu_tog_id == r->id) { double t = (t0 - bu_tog_t0) / 240; if (t < 1) { float e = (float)ease_out(t); p = bu_tog_to ? e : 1 - e; } }
                bu_switch(d, W - 16 - 12 - 51, ry + 6, p, r->flags & BFL_DIM);
                right = W - 16 - 12 - 51 - 8;
                break;
            }
            case BR_CHECK: if (r->on) bu_check(d, W - 46, cy - 5, r->flags & BFL_DIM ? BU_GRAY : BU_BLUE); right = W - 54; break;
            case BR_NAV: bu_chev(d, W - 34, cy, rgb(90, 90, 96)); right = W - 42; /* fallthrough */
            case BR_INFO:
                if (r->v[0]) {
                    int tw0 = tw_n(r->t, 15, -1), maxv = right - tx - (tw0 < 150 ? tw0 : 150) - 14, vw = tw_n(r->v, 15, -1);
                    if (maxv > 190) maxv = 190; if (maxv < 60) maxv = 60; if (vw > maxv) vw = maxv;
                    draw_text(d, right - vw, cy + 6, r->v, 15, BU_GRAY, maxv, bu_y0, bu_y1); right -= vw + 10;
                }
                break;
            case BR_ACTION: tc = r->flags & BFL_RED ? BU_RED : BU_BLUE; if (r->flags & BFL_BUSY) { bu_spinner(d, 40, cy, 7.5f, t0); tx = 56; tc = BU_GRAY; } break;
            case BR_TEXT: tc = BU_GRAY; if (r->flags & BFL_BUSY) { bu_spinner(d, 40, cy, 7.5f, t0); tx = 56; } break;
            }
            if (r->kind == BR_NAV || r->kind == BR_TOGGLE || r->kind == BR_INFO || r->kind == BR_ACTION || r->kind == BR_CHECK || r->kind == BR_TEXT) {
                int tmax = right - tx - 4; if (tmax < 30) tmax = 30;
                draw_text(d, tx, cy + 6, r->t, 15, tc, tmax, bu_y0, bu_y1);
            }
            ry += rh;
        }
        y = top + ch;
        if (sc->foot[0]) { y += 8; y += draw_wrapped(d, 32, y, W - 48, sc->foot, 12, BU_GRAY, 16, y > ytop - 300 && y < H); }
        if (!sc->plain) y += 14;
    }
    /* navigation bar (the status bar is the player's) */
    bu_y0 = 0; bu_y1 = H;
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < ytop; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(12, 12, 13);
    for (int x = 0; x < W; x++) d[(ytop - 1) * W + x] = rgb(38, 38, 40);
    bu_grp = -1;
    bu_navbar(d, pg->title, pg->back);
    draw_toast(d);
    double dt = now_ms() - t0; bu_render_ms += dt; bu_render_n++; if (dt > bu_render_max) bu_render_max = dt;
}
/* row under y (and its top in *ry) or -1 */
static int bu_hit(const struct bpage *pg, int y, int *rowy)
{
    if (y < BU_TOP) return -1;
    int yy = BU_TOP + 12 - (int)bu_scroll;
    for (int si = 0; si < pg->ns; si++) {
        const struct bsec *sc = &pg->s[si];
        if (!sc->plain) yy += sc->hdr ? 28 : 14;
        for (int k = 0; k < sc->n; k++) { int h = pg->r[sc->first + k].h; if (y >= yy && y < yy + h) { if (rowy) *rowy = yy; return sc->first + k; } yy += h; }
        yy += bu_foot_h(sc->foot) + (sc->plain ? 0 : 14);
    }
    return -1;
}
static int bu_animating(const struct bpage *pg)
{
    if (bu_tog_id && now_ms() - bu_tog_t0 < 260) return 1;
    if (now_ms() < toast_until + 40) return 1;
    for (int i = 0; i < pg->nr; i++) if ((pg->r[i].flags & BFL_BUSY) || (pg->r[i].kind == BR_FIND && pg->r[i].val)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ actions */
static int bu_gone;
static void bu_page(int kind, const char *addr, int p1, int p2, const char *back);
static void bu_toggle_anim(int id, int to) { bu_tog_id = id; bu_tog_to = to; bu_tog_t0 = now_ms(); }
static int bu_can_set(const struct buds_st *s, int quiet)
{
    if (!s || s->conn != BS_READY) { if (!quiet) toast(tr("Los auriculares no están conectados")); return 0; }
    if (s->caps & BC_RO) { if (!quiet) toast(tr("Solo lectura: aún no verificado con este modelo")); return 0; }
    return 1;
}
static void bu_cmd_slider(const struct bpage *pg, const struct brow *r, int v)
{
    int op = r->id == BI_ANCLVL ? BCMD_ANCLVL : r->id == BI_AMBLVL ? BCMD_AMBLVL : r->id == BI_ADAPTLVL ? BCMD_ADAPTLVL : r->id == BI_BASS ? BCMD_BASS : BCMD_BANDS;
    if (op == BCMD_BANDS) {
        const struct buds_st *s = bu_dev(pg->addr); if (!s) return;
        int g[3] = { 0, 0, 0 }; for (int i = 0; i < 3 && i < s->nbands; i++) g[i] = s->bands[i];
        g[r->id - BI_BAND0] = v;
        char b[40]; snprintf(b, sizeof b, "%d,%d,%d", g[0], g[1], g[2]);
        bu_send(pg->addr, BCMD_BANDS, 0, 0, 0, b);
        pthread_mutex_lock(&bu_mu);                               /* optimistic: bands of the snapshot */
        int i = bu_find_dev(BU, BUn, pg->addr); if (i >= 0) { BU[i].nbands = 3; for (int k = 0; k < 3; k++) BU[i].bands[k] = g[k]; BU[i].eq = BU[i].eq_set == ES_CMF ? 5 : 4; bu_gen++; }
        pthread_mutex_unlock(&bu_mu);
        return;
    }
    bu_send(pg->addr, op, v, 0, 0, 0);
}
/* tap on a row; x, the row top (for in-row buttons). 1 = leave the page */
static int bu_tap(struct bpage *pg, const struct brow *r, int x, int y, int rowy)
{
    const struct buds_st *s = bu_dev(pg->addr);
    int id = r->id;
    if (pg->kind == BP_LIST) {
        if (id >= BI_DEV0 && id < BI_DEV0 + BUDS_MAXDEV && id - BI_DEV0 < bvn) bu_page(BP_DEV, bv[id - BI_DEV0].addr, 0, 0, pg->title);
        else if (id == BI_SUPP) bu_page(BP_SUPP, "", 0, 0, pg->title);
        return 0;
    }
    if (!s) return 0;
    switch (id) {
    case BI_SEG: {
        if (!bu_can_set(s, 0)) break;
        int modes[BN_N], nm = bu_seg_modes(s, modes), i = -1;
        for (int k = 0; k < nm; k++) if (fabsf(x - bu_seg_cx(k, nm)) <= BU_SEG_W / 2.f) i = k;
        if (i < 0) break;
        if (nm && modes[i] != s->noise) bu_send(s->addr, BCMD_NOISE, modes[i], 0, 0, 0);
        break;
    }
    case BI_CONV: case BI_EARDET: case BI_LOCK: case BI_GAME: {
        if (!bu_can_set(s, 0)) break;
        int v = !r->on, op = id == BI_CONV ? BCMD_CONV : id == BI_EARDET ? BCMD_EARDET : id == BI_LOCK ? BCMD_LOCK : BCMD_GAME;
        bu_toggle_anim(id, v); bu_send(s->addr, op, v, 0, 0, 0);
        break;
    }
    case BI_AUTOPAUSE: bu_toggle_anim(id, !r->on); bu_set_autopause(s->addr, !r->on); break;
    case BI_GEST: bu_page(BP_GEST, s->addr, 0, 0, pg->title); break;
    case BI_EQ: bu_page(BP_EQ, s->addr, 0, 0, pg->title); break;
    case BI_FIND:
        if (s->conn != BS_READY) { toast(tr("Los auriculares no están conectados")); break; }
        bu_page(BP_FINDP, s->addr, 0, 0, pg->title);
        break;
    case BI_FIT:
        if (!bu_can_set(s, 0)) break;
        if (confirm(tr("BUDS_FIT_ASK"), tr("Empezar"))) { bu_send(s->addr, BCMD_FIT, 0, 0, 0, 0); toast(tr("Prueba en marcha…")); }
        break;
    case BI_NAME: {
        char nm[64]; buds_cpy(nm, sizeof nm, bu_display_name(s));
        if (line_edit(tr("Nombre"), s->model, nm, 40) && nm[0] && strcmp(nm, bu_display_name(s))) bu_send(s->addr, BCMD_RENAME, 0, 0, 0, nm);
        break;
    }
    case BI_CONNECT: if (!s->busy) bu_send(s->addr, BCMD_CONNECT, 0, 0, 0, 0); break;
    case BI_DISCONNECT: if (!s->busy) bu_send(s->addr, BCMD_DISCONNECT, 0, 0, 0, 0); break;
    case BI_NONE: break;
    default:
        if (id >= BI_CYC0 && id < BI_CYC0 + BN_N) {
            if (!bu_can_set(s, 0)) break;
            int m = s->cycle < 0 ? 0 : s->cycle, bit = 1 << (id - BI_CYC0), nw = m ^ bit, k = 0;
            for (int i = 0; i < BN_N; i++) if (nw & (1 << i)) k++;
            if (k < 2) { toast(tr("Elige al menos dos modos")); break; }
            bu_send(s->addr, BCMD_CYCLE, nw, 0, 0, 0);
        } else if (id >= BI_GEST0 && id < BI_GEST0 + 20) {
            if (s->lock > 0) { toast(tr("Los controles táctiles están bloqueados")); break; }
            bu_page(BP_PICK, s->addr, (id - BI_GEST0) / 10, (id - BI_GEST0) % 10, pg->title);
        } else if (id >= BI_PICK0 && id < BI_PICK0 + BA_N) {
            if (!bu_can_set(s, 0)) break;
            if (s->gest[pg->p1][pg->p2] != id - BI_PICK0) bu_send(s->addr, BCMD_GEST, pg->p1, pg->p2, id - BI_PICK0, 0);
            return 1;
        } else if (id >= BI_EQ0 && id < BI_EQ0 + 16) {
            if (!bu_can_set(s, 0)) break;
            if (s->eq != id - BI_EQ0) bu_send(s->addr, BCMD_EQ, id - BI_EQ0, 0, 0, 0);
        }
        break;
    }
    if (r->kind == BR_FIND && bu_can_set(s, 0)) {
        int single = bu_single(s->family), side = single ? 2 : x < W / 2 ? 0 : 1;
        (void)rowy; (void)y;
        int on = single ? (s->find[0] || s->find[1]) : s->find[side];
        if (on) { bu_send(s->addr, BCMD_FIND, 2, 0, 0, 0); return 0; }
        if (now_ms() - bu_find_ok_t > 600000) { if (!confirm(tr("BUDS_FIND_WARN"), tr("Reproducir sonido"))) return 0; bu_find_ok_t = now_ms(); }
        bu_send(s->addr, BCMD_FIND, side, 1, 0, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ page loop */
static struct bpage bu_pages[6];
static int bu_depth;
static void bu_present(const struct bpage *pg, int sel) { bu_render(fr0, pg, sel); present(); }
static unsigned bu_err_seen;
static void bu_events(void)
{
    if (bu_err_n != bu_err_seen) { bu_err_seen = bu_err_n; toast(bu_err_text(bu_err_code)); }
}
static void bu_page(int kind, const char *addr, int p1, int p2, const char *back)
{
    if (bu_depth >= 6) return;
    struct bpage *pg = &bu_pages[bu_depth++];
    memset(pg, 0, sizeof *pg);
    pg->kind = kind; buds_cpy(pg->addr, sizeof pg->addr, addr ? addr : ""); pg->p1 = p1; pg->p2 = p2; pg->back = back;
    float scroll0 = bu_scroll; bu_scroll = 0;
    bu_snap(); bu_build(pg);
    if (back) {
        memcpy(img_under, fr0, HM_FRAME_BYTES); have_under = 0;
        bu_render(img_base, pg, -1); fs_transition(img_under, img_base, 1, 0);
    }
    float vel = 0; int first = 1, last_step = -1;
    for (;;) {
        int ch = bu_snap();
        if (ch) bu_build(pg);
        unsigned es = bu_err_seen; bu_events(); if (es != bu_err_seen) ch = 1;
        if (pg->kind != BP_LIST && pg->kind != BP_SUPP && !bu_dev(pg->addr) && bvh.pid) break;    /* forgotten */
        float maxs = bu_content_h(pg) - (H - BU_TOP); if (maxs < 0) maxs = 0;
        if (bu_scroll > maxs) bu_scroll = maxs;
        int fast = bu_tog_id && now_ms() - bu_tog_t0 < 260, anim = bu_animating(pg), step = (int)(now_ms() / 83);
        if (first || ch || fast || (anim && step != last_step)) { bu_present(pg, -1); first = 0; last_step = step; }
        struct pkt p;
        if (!read_pkt(&p, fast ? 1 : anim ? 20 : 250)) continue;
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms(); float s0 = bu_scroll; int moved = 0, ly = sy; double lt = ts;
        if (sy >= 456) {                                     /* from the bottom bar: home / multitasking */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { bu_render(fr0, pg, -1); if (own_home_gesture(sx, sy, app_by_id("headphones"))) { bu_gone = 1; break; } bu_present(pg, -1); continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        if (back_zone(sx, sy)) {                             /* from an edge: back (at the root: leave) */
            bu_render(fr0, pg, -1);
            int bk = app_back_gesture(sx, sy);
            if (bk > 0) break;
            if (bk < 0) continue;
            if (!cur_down) { sx = cur_x; sy = cur_y; }
        }
        int rowy = 0, hi = bu_hit(pg, sy, &rowy);
        const struct brow *hr = hi >= 0 ? &pg->r[hi] : 0;
        /* slider: horizontal drag sets the value live, sent on release */
        if (hr && hr->kind == BR_SLIDER && sy >= rowy + 26) {
            const struct buds_st *s = bu_dev(pg->addr);
            if (!bu_can_set(s, 0)) { drain_until_up(); continue; }
            struct brow row = *hr; int tx = 30, tw = W - 60, v = row.val;
            bu_slide_id = row.id;
            for (;;) {
                float f = (cur_x - tx) / (float)tw; if (f < 0) f = 0; if (f > 1) f = 1;
                v = row.mn + (int)(f * (row.mx - row.mn) + .5f);
                if (v != bu_slide_val || bu_slide_id != row.id) { bu_slide_val = v; bu_present(pg, -1); }
                if (!cur_down) break;
                if (!read_pkt(&p, 40)) continue;
            }
            if (v != row.val) bu_cmd_slider(pg, &row, v);
            bu_slide_id = -1; bu_snap(); bu_build(pg); bu_present(pg, -1);
            continue;
        }
        int tappable = hr && (hr->kind == BR_NAV || hr->kind == BR_TOGGLE || hr->kind == BR_ACTION || hr->kind == BR_CHECK || hr->kind == BR_DEV);
        if (tappable) bu_present(pg, hi);
        while (cur_down && read_pkt(&p, 30)) {
            if (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 10) moved = 1;
            if (!moved) continue;
            double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
            float sc = s0 - (cur_y - sy);
            if (sc < 0) sc = sc * 0.35f; if (sc > maxs) sc = maxs + (sc - maxs) * 0.35f;   /* rubber band */
            bu_scroll = sc;
            bu_render(fr0, pg, -1); present();
        }
        if (moved) {
            if (bu_scroll < 0 || bu_scroll > maxs) {             /* rubber band back */
                float a = bu_scroll, b = bu_scroll < 0 ? 0 : maxs; double t0 = now_ms();
                for (;;) { double t = (now_ms() - t0) / 260; if (t > 1) t = 1; bu_scroll = a + (b - a) * (float)ease_out(t); bu_render(fr0, pg, -1); present(); if (t >= 1) break; }
                vel = 0; continue;
            }
            if (now_ms() - lt > 60) vel = 0;
            double tp = now_ms();
            while (fabsf(vel) > 0.03f) {                       /* iOS inertia */
                double t = now_ms(); float dt = (float)(t - tp); tp = t; if (dt > 40) dt = 40;
                bu_scroll -= vel * dt; vel *= powf(0.9975f, dt);
                if (bu_scroll < 0) { bu_scroll = 0; vel = 0; } if (bu_scroll > maxs) { bu_scroll = maxs; vel = 0; }
                if (bu_snap()) bu_build(pg);
                bu_render(fr0, pg, -1); present();
                struct pkt q; if (read_pkt(&q, 0) && q.down) { vel = 0; drain_until_up(); break; }
            }
            vel = 0;
            continue;
        }
        if (pg->back && sy < BU_TOP && sx < 110) break;                      /* "< Back" */
        if (!hr || now_ms() - ts > 700) { bu_present(pg, -1); continue; }
        struct brow row = *hr;
        if (row.kind == BR_TEXT || row.kind == BR_INFO || row.kind == BR_HERO || row.kind == BR_SUPP || row.kind == BR_EQVIS || row.kind == BR_EMPTY) { bu_present(pg, -1); continue; }
        if (bu_tap(pg, &row, sx, sy, rowy)) { if (!bu_gone) break; }
        if (bu_gone) break;
        bu_snap(); bu_build(pg); bu_present(pg, -1);
    }
    if (pg->back && !bu_gone) {                              /* the previous page comes back from the left */
        bu_render(img_under, pg, -1);
        struct bpage *prev = &bu_pages[bu_depth - 2];
        bu_scroll = scroll0;
        bu_snap(); bu_build(prev);
        bu_render(img_base, prev, -1);
        fs_transition(img_under, img_base, -1, 0);
        have_under = 0;
    }
    bu_scroll = scroll0;
    bu_depth--;
}

/* ------------------------------------------------------------------ entry */
static void buds_app_at(int ix, int iy)
{
    double t_open = now_ms();
    buds_preload();
    have_under = 0; bu_gone = 0; bu_depth = 0; bu_scroll = 0; bu_render_ms = bu_render_max = 0; bu_render_n = 0;
    bv_gen = ~0u; bu_snap(); bu_err_seen = bu_err_n;
    struct bpage *root = &bu_pages[0]; memset(root, 0, sizeof *root); root->kind = BP_LIST;
    bu_build(root);
    bu_render(img_app, root, -1);
    render_launcher(img_base, now_ms());
    double t0 = now_ms();
    for (;;) {                                               /* zoom from the icon */
        double t = (now_ms() - t0) / 300; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    lg("auriculares: abierta en %.0f ms", now_ms() - t_open);
    bu_page(BP_LIST, "", 0, 0, 0);
    lg("auriculares: pintado medio %.1f ms, maximo %.1f ms (%d fotogramas)", bu_render_n ? bu_render_ms / bu_render_n : 0, bu_render_max, bu_render_n);
    if (!bu_gone) {
        bu_snap(); bu_build(root); bu_render(img_app, root, -1);
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
    bu_gone = 0;
    drain_until_up();
    launcher_present();
}
static void buds_app(void) { buds_app_at((W - L.icon) / 2, (H - L.icon) / 2); }

/* ------------------------------------------------------------------ connect popup */
/* iOS AirPods-style card. Static parts (sheet, name, product drawing, close button, ring tracks and
 * labels) are drawn once per popup into bu_pop; each frame blits it (rounded corners) and draws
 * the battery values, which fade in when they arrive after the card opened (partial data).
 * Ring slots come from the capabilities (Left / Right / Case, or one), so nothing moves.
 * Vertical layout: title (baseline 34), >= 12 px, product box 50..134, >= 10 px, bolts, rings
 * (centre 188), labels (baseline 230). progress 0 = hidden, 1 = fully up. */
#define BU_POP_H 248
#define BU_POP_W (W - 16)
#define BU_POP_RY 188
static uint16_t *bu_pop; static unsigned bu_pop_sig; static double bu_pop_seen[3]; static unsigned bu_pop_seen_gen;
static struct bu_el bu_pop_els[24]; static int bu_pop_nel;           /* static elements (card coordinates) */
static void bu_pop_build(const struct buds_st *s)
{
    int ogrp = bu_grp; bu_grp = -2;
    if (!bu_pop) bu_pop = malloc(W * BU_POP_H * 2);
    if (!bu_pop) return;
    uint16_t *c = bu_pop;                          /* drawn in place: stride W, BU_POP_H rows */
    int oy0 = bu_y0, oy1 = bu_y1, orec = bu_rec, on0 = bu_nel; bu_y0 = 0; bu_y1 = BU_POP_H;
    bu_rec = 1;                                    /* collect the static elements for the checks */
    uint16_t bg = rgb(30, 30, 33);
    for (int i = 0; i < W * BU_POP_H; i++) c[i] = bg;
    const char *nm = bu_display_name(s); int tmax = BU_POP_W - 2 * 50, sz = bu_fit(nm, 17, tmax), tw = tw_n(nm, sz, -1); if (tw > tmax) tw = tmax;
    int tx = (BU_POP_W - tw) / 2;
    draw_text(c, tx, 34, nm, sz, 0xFFFF, tmax, 0, BU_POP_H); bu_mark_text(BT_PTITLE, 0, tx, 34, tw, sz);
    float kx = BU_POP_W - 26, ky = 27;
    fill_circle(c, kx, ky, 14, rgb(58, 58, 62), 255);
    bu_line(c, kx - 5, ky - 5, kx + 5, ky + 5, 2.2f, rgb(175, 175, 180)); bu_line(c, kx - 5, ky + 5, kx + 5, ky - 5, 2.2f, rgb(175, 175, 180));
    bu_mark(BT_PCLOSE, 0, kx - 14.5f, ky - 14.5f, kx + 14.5f, ky + 14.5f);
    bu_layout(c, 28, 50, BU_POP_W - 28, 134, s->family, s->color, BL_HERO, 0, 2);
    static const char *const lb[3] = { "Izquierdo", "Derecho", "Estuche" };
    int part[3], n = bu_ring_slots(s, part);
    for (int i = 0; i < n; i++) {
        float x = bu_ring_x(BU_POP_W / 2.f, i, n);
        bu_arc(c, x, BU_POP_RY, BU_RING_R, 4.5f, 0, 6.2832f, rgb(52, 52, 56));   /* empty track until the value arrives */
        bu_mark(BT_RING, i, x - BU_RING_R - 2.25f, BU_POP_RY - BU_RING_R - 2.25f, x + BU_RING_R + 2.25f, BU_POP_RY + BU_RING_R + 2.25f);
        const char *t = part[i] >= 10 ? tr("Batería") : tr(lb[part[i]]);
        int ls = bu_fit(t, 12, BU_RING_STEP - 8), lw = tw_n(t, ls, -1), lx = (int)roundf(x - lw / 2.f), base = BU_POP_RY + BU_RING_R + 18;
        draw_text(c, lx, base, t, ls, BU_GRAY, 0, 0, BU_POP_H); bu_mark_text(BT_RLBL, i, lx, base, lw, ls);
    }
    bu_pop_nel = 0;
    for (int i = on0; i < bu_nel && bu_pop_nel < 24; i++) bu_pop_els[bu_pop_nel++] = bu_els[i];
    bu_nel = on0; bu_rec = orec; bu_grp = ogrp;
    bu_y0 = oy0; bu_y1 = oy1;
}
static void buds_popup_draw(uint16_t *d, float progress)
{
    struct buds_st v[BUDS_MAXDEV]; int n = bu_view(v, BUDS_MAXDEV, 0), i = bu_find_dev(v, n, bu_pop_addr);
    if (i < 0 || progress <= 0) return;
    const struct buds_st *s = &v[i];
    int part[3], ns = bu_ring_slots(s, part);
    unsigned sig = 2166136261u;
    sig = (sig ^ (unsigned)s->family) * 16777619u; sig = (sig ^ (unsigned)ns) * 16777619u; sig = (sig ^ bu_pop_gen) * 16777619u; sig = (sig ^ (unsigned)ui_lang) * 16777619u;
    for (const char *c = s->name; *c; c++) sig = (sig ^ (unsigned char)*c) * 16777619u;
    if (!bu_pop || sig != bu_pop_sig) { bu_pop_build(s); bu_pop_sig = sig; }
    if (!bu_pop) return;
    int ogrp = bu_grp; bu_grp = -2;
    double t = now_ms();
    if (bu_pop_seen_gen != bu_pop_gen) {                    /* new popup: what is known now needs no fade */
        bu_pop_seen_gen = bu_pop_gen;
        for (int j = 0; j < 3; j++) { int idx = j < ns ? (part[j] >= 10 ? 0 : part[j]) : 0; bu_pop_seen[j] = j < ns && s->batt[idx] >= 0 ? -1e9 : 0; }
    }
    if (progress > 1) progress = 1;
    int k = (int)(12 * progress);                            /* the screen behind dims a little */
    if (k > 0) for (int p = 30 * W; p < W * H; p++) d[p] = dim565(d[p], 32 - k);
    int top = H - (int)((BU_POP_H + 8) * progress);
    const float R = 26;
    for (int y = 0; y < BU_POP_H; y++) {
        int yy = top + y; if (yy < 0 || yy >= H) continue;
        float dy = y + .5f < R ? R - (y + .5f) : y + .5f > BU_POP_H - R ? (y + .5f) - (BU_POP_H - R) : 0;
        float edge = dy > 0 ? R - sqrtf(R * R - dy * dy) : 0; int full = (int)ceilf(edge);
        uint16_t *row = d + yy * W + 8; const uint16_t *src = bu_pop + y * W;
        memcpy(row + full, src + full, (size_t)(BU_POP_W - 2 * full) * 2);
        if (full > 0) { unsigned a = (unsigned)((full - edge) * 255); row[full - 1] = mix565(src[full - 1], row[full - 1], a); row[BU_POP_W - full] = mix565(src[BU_POP_W - full], row[BU_POP_W - full], a); }
    }
    for (int e = 0; e < bu_pop_nel; e++) {                    /* static elements, in screen coordinates */
        struct bu_el x = bu_pop_els[e];
        if (x.tag == BT_RING) continue;                       /* re-marked with its value below */
        bu_mark(x.tag, x.col, x.x0 + 8, x.y0 + top, x.x1 + 8, x.y1 + top);
    }
    /* values: fade in over 300 ms when they arrive */
    int oy0 = bu_y0, oy1 = bu_y1; bu_y0 = top < 0 ? 0 : top; bu_y1 = H;
    uint16_t bg = rgb(30, 30, 33);
    for (int j = 0; j < ns; j++) {
        int idx = part[j] >= 10 ? 0 : part[j], pct = s->batt[idx];
        float a = 0;
        if (pct >= 0) {
            if (!bu_pop_seen[j]) bu_pop_seen[j] = t;
            a = (float)((t - bu_pop_seen[j]) / 300.0); if (a > 1) a = 1; if (a < 0) a = 0;
        }
        bu_ring(d, 8 + bu_ring_x(BU_POP_W / 2.f, j, ns), top + BU_POP_RY, BU_RING_R, pct, s->chg[idx], 0, j, a, bg);
    }
    bu_y0 = oy0; bu_y1 = oy1; bu_grp = ogrp;
}
/* where a touch falls on the card (same geometry as buds_popup_draw) */
static int buds_popup_hit(int x, int y, float progress)
{
    int top = H - (int)((BU_POP_H + 8) * (progress > 1 ? 1 : progress));
    if (y < top || y >= top + BU_POP_H || x < 8 || x >= 8 + BU_POP_W) return 0;
    int cx = 8 + BU_POP_W - 26, cy = top + 27;
    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < 26 * 26) return 1;
    return 2;
}
