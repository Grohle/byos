/* buds_render.c - frames of the Headphones app (src/buds.inc.c) on the PC, as PNG, to review the
 * design without the device: list, device pages (AirPods Pro 2, Galaxy Buds2 Pro, Nothing Ear (a),
 * AirPods Max), gestures, action picker, EQ (presets and custom bands), find, supported devices,
 * the connect popup (partial and complete data) and the Batteries widget items, in es/en/fr/ja/de.
 * The state comes from the real fake daemon (hibybudsd --fake, HIBYBUDS_DIR=/tmp) started by
 * tests/buds_render.sh; touch input and the rest of the shell are stubs.
 *   bash tests/buds_render.sh  ->  out/buds_render/ (or $BUDS_OUT) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define W 320
#define H 480
#define ASSETS "/tmp/buds_render_assets"
extern char **environ;
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void msleep(int ms) { usleep(ms * 1000); }
static void lg(const char *f, ...) { (void)f; }
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static inline uint16_t dim565(uint16_t p, int k) { unsigned r = (p >> 11) * k >> 5, g = ((p >> 5) & 63) * k >> 5, b = (p & 31) * k >> 5; return (uint16_t)(r << 11 | g << 5 | b); }
static inline uint16_t mix565(uint16_t c, uint16_t d, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
#include "hm_shared.h"
static uint16_t fb0[W * H], fb1[W * H], *fr0 = fb0, *fr1 = fb1;
static uint16_t img_base[W * H], img_app[W * H], img_under[W * H];
static int have_under;
static int cur_x, cur_y, cur_down;
struct pkt { int n; int x, y, down; };
static int read_pkt(struct pkt *p, int t) { (void)p; (void)t; return 0; }
static void present(void) {}
static double toast_until;
static void toast(const char *m) { (void)m; }
static void draw_toast(uint16_t *d) { (void)d; }
static void drain_until_up(void) {}
static int back_zone(int sx, int sy) { (void)sx; (void)sy; return 0; }
static int app_back_gesture(int sx, int sy) { (void)sx; (void)sy; return 0; }
static int own_home_gesture(int sx, int sy, int s) { (void)sx; (void)sy; (void)s; return 0; }
static int confirm(const char *t, const char *b) { (void)t; (void)b; return 0; }
static int line_edit(const char *t, const char *h, char *b, int c) { (void)t; (void)h; (void)b; (void)c; return 0; }
static void fs_transition(const uint16_t *a, const uint16_t *b, int d, float s) { (void)a; (void)b; (void)d; (void)s; }
static int app_by_id(const char *id) { (void)id; return -1; }
static void render_launcher(uint16_t *d, double t) { (void)d; (void)t; }
static void draw_scaled(uint16_t *d, const uint16_t *s, int x, int y, int w, int h, float r) { (void)d; (void)s; (void)x; (void)y; (void)w; (void)h; (void)r; }
static float win_radius(int w, int h, double e) { (void)w; (void)h; (void)e; return 0; }
static void launcher_present(void) {}
static struct { int icon; } L = { 56 };
static int pc_toggle(void) { return 0; }
static struct hm_state *np_map(void) { return 0; }
static uint32_t np_ms(void) { return 0; }
static volatile int wg_fresh;
#include "i18n.inc.c"
#include "text.inc.c"
#include "buds_render_extract.inc"       /* fill_rrect, fill_circle, draw_wrapped (from the shell, as is) */
#include "buds.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static const char *outdir(void) { return getenv("BUDS_OUT") ? getenv("BUDS_OUT") : "out/buds_render"; }
static void save(const uint16_t *src, const char *name)
{
    static unsigned char px[W * H * 3];
    for (int i = 0; i < W * H; i++) { uint16_t p = src[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, W, H, 3, px, W * 3);
}
static void status_bar(void) { for (int i = 0; i < 30 * W; i++) fr1[i] = 0; draw_text(fr1, 20, 21, "9:41", 15, 0xFFFF, 0, 0, H); draw_text(fr1, 270, 21, "87 %", 13, 0xFFFF, 0, 0, H); }
static void cmd(const char *c) { int f = open(BUDS_CMD, O_WRONLY | O_NONBLOCK); if (f >= 0) { if (write(f, c, strlen(c)) < 0) {} close(f); } msleep(150); }
static void refresh(void) { bu_read_state(); bv_gen = ~0u; bu_snap(); }

/* ------------------------------------------------------------------ automatic layout checks
 * Every element recorded by buds.inc.c (bu_mark) on the frame just drawn:
 *   - no two elements intersect (except the % text inside its ring), none leaves the screen;
 *   - product parts: buds >= 6 px apart, case >= 14 px from the buds, same baseline, the group
 *     centred in its layout box (+-2 px); >= 8 px clear of rings / bolts (10 in the popup) and
 *     >= 10 px below the popup title;
 *   - ring columns: ring, %, label and bolt on the same centre (+-1 px); the row centred;
 *   - noise segments: equal steps, the group centred, label centred under its icon;
 *   - list rows: the icon in the same column (x centre 45) and centred on the text block (+-3);
 *   - nav bar: title centred, back label clear of it. */
static int lfails, lchecks; static char lname[64];
#define LCHK(c, ...) do { lchecks++; if (!(c)) { lfails++; printf("LAYOUT %s: ", lname); printf(__VA_ARGS__); printf("\n"); } } while (0)
static float ecx(const struct bu_el *e) { return (e->x0 + e->x1) / 2.f; }
static float ecy(const struct bu_el *e) { return (e->y0 + e->y1) / 2.f; }
static int inter(const struct bu_el *a, const struct bu_el *b) { return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1; }
static float vgap(const struct bu_el *a, const struct bu_el *b) { return a->y1 <= b->y0 ? b->y0 - a->y1 : b->y1 <= a->y0 ? a->y0 - b->y1 : -1; }
static const char *tagn(int t) { static const char *n[] = { "", "part", "ring", "ring%", "ringlabel", "bolt", "title", "back", "segicon", "seglabel", "licon", "ltext", "ptitle", "pclose", "eqaxis", "findbtn", "emptytext", "lbox" }; return t < 18 ? n[t] : "?"; }
static void layout_check(const char *name, int page_top)
{
    snprintf(lname, sizeof lname, "%s", name);
    struct bu_el *E = bu_els; int n = bu_nel;
    int vis[BU_MAXEL];
    for (int i = 0; i < n; i++) vis[i] = E[i].grp < 0 || (E[i].y0 >= page_top && E[i].y1 <= H);   /* page content under the nav bar is clipped */
    for (int i = 0; i < n; i++) if (vis[i] && E[i].tag != BT_LBOX) LCHK(E[i].x0 >= 0 && E[i].x1 <= W && E[i].y0 >= 0 && E[i].y1 <= H, "%s outside the screen (%d,%d)-(%d,%d)", tagn(E[i].tag), E[i].x0, E[i].y0, E[i].x1, E[i].y1);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        const struct bu_el *a = &E[i], *b = &E[j];
        if (!vis[i] || !vis[j] || a->tag == BT_LBOX || b->tag == BT_LBOX) continue;
        int ring_txt = ((a->tag == BT_RING && b->tag == BT_RTXT) || (a->tag == BT_RTXT && b->tag == BT_RING)) && a->col == b->col && a->grp == b->grp;
        if (ring_txt) continue;
        LCHK(!inter(a, b), "%s %d (%d,%d)-(%d,%d) overlaps %s %d (%d,%d)-(%d,%d)", tagn(a->tag), a->col, a->x0, a->y0, a->x1, a->y1, tagn(b->tag), b->col, b->x0, b->y0, b->x1, b->y1);
    }
    /* product layouts */
    for (int l = 0; l < n; l++) if (E[l].tag == BT_LBOX && vis[l]) {
        const struct bu_el *box = &E[l]; int ux0 = 9999, ux1 = -9999, uy0 = 9999, uy1 = -9999, bottom = -1, np = 0;
        const struct bu_el *pp[3] = { 0, 0, 0 };
        for (int i = 0; i < n; i++) if (E[i].tag == BT_PART && E[i].lay == box->lay) {
            const struct bu_el *e = &E[i]; if (np < 3) pp[np] = e; np++;
            if (e->x0 < ux0) ux0 = e->x0; if (e->x1 > ux1) ux1 = e->x1; if (e->y0 < uy0) uy0 = e->y0; if (e->y1 > uy1) uy1 = e->y1;
            LCHK(e->x0 >= box->x0 - 1 && e->x1 <= box->x1 + 1 && e->y0 >= box->y0 - 1 && e->y1 <= box->y1 + 1, "part %d outside its box", e->col);
            if (bottom < 0) bottom = e->y1; else LCHK(abs(e->y1 - bottom) <= 1, "parts not on the same baseline (%d / %d)", e->y1, bottom);
        }
        if (!np) continue;
        LCHK(fabsf((ux0 + ux1) / 2.f - ecx(box)) <= 2 && fabsf((uy0 + uy1) / 2.f - ecy(box)) <= 2, "product group not centred in its box (%.1f,%.1f vs %.1f,%.1f)", (ux0 + ux1) / 2.f, (uy0 + uy1) / 2.f, ecx(box), ecy(box));
        for (int i = 1; i < np && i < 3; i++) {
            int gap = pp[i]->x0 - pp[i - 1]->x1, need = pp[i]->col == 3 ? 14 : 6;
            LCHK(gap >= need, "gap %d px between parts %d and %d (needs %d)", gap, pp[i - 1]->col, pp[i]->col, need);
        }
        for (int i = 0; i < n; i++) if (vis[i] && (E[i].tag == BT_RING || E[i].tag == BT_BOLT || E[i].tag == BT_PTITLE) && E[i].grp == box->grp) {
            struct bu_el u = { (short)ux0, (short)uy0, (short)ux1, (short)uy1, 0, 0, 0, 0 };
            float need = E[i].tag == BT_PTITLE || box->grp == -2 ? 10 : 8, g = vgap(&u, &E[i]);
            LCHK(g >= need, "%s only %.0f px from the product (needs %.0f)", tagn(E[i].tag), g, need);
        }
    }
    /* ring columns */
    for (int i = 0; i < n; i++) if (E[i].tag == BT_RING && vis[i]) {
        int nr = 0; float sum = 0;
        for (int j = 0; j < n; j++) if (E[j].tag == BT_RING && E[j].grp == E[i].grp && vis[j]) { nr++; sum += ecx(&E[j]); }
        if (E[i].col == 0) LCHK(fabsf(sum / nr - W / 2.f) <= 1, "ring row not centred (%.1f)", sum / nr);
        for (int j = 0; j < n; j++) if ((E[j].tag == BT_RTXT || E[j].tag == BT_RLBL || E[j].tag == BT_BOLT) && E[j].grp == E[i].grp && E[j].col == E[i].col && vis[j])
            LCHK(fabsf(ecx(&E[j]) - ecx(&E[i])) <= 1.5f, "%s of ring %d off its column (%.1f vs %.1f)", tagn(E[j].tag), E[i].col, ecx(&E[j]), ecx(&E[i]));
    }
    /* segments */
    for (int i = 0; i < n; i++) if (E[i].tag == BT_SEGI && E[i].col == 0 && vis[i]) {
        float c[4]; int ns = 0;
        for (int j = 0; j < n; j++) if (E[j].tag == BT_SEGI && E[j].grp == E[i].grp && ns < 4) c[E[j].col < 4 ? E[j].col : 3] = ecx(&E[j]), ns++;
        for (int k = 2; k < ns; k++) LCHK(fabsf((c[k] - c[k - 1]) - (c[1] - c[0])) <= 1, "unequal segment steps");
        LCHK(fabsf((c[0] + c[ns - 1]) / 2 - W / 2.f) <= 1, "segments not centred");
        for (int j = 0; j < n; j++) if (E[j].tag == BT_SEGL && E[j].grp == E[i].grp) LCHK(fabsf(ecx(&E[j]) - c[E[j].col < 4 ? E[j].col : 3]) <= 1.5f, "segment label %d not centred", E[j].col);
    }
    /* list rows: icon column and vertical centring */
    for (int i = 0; i < n; i++) if (E[i].tag == BT_LTXT && E[i].col == 0 && vis[i]) {
        float ty0 = E[i].y0, ty1 = E[i].y1;
        for (int j = 0; j < n; j++) if (E[j].tag == BT_LTXT && E[j].grp == E[i].grp && E[j].col == 1) { if (E[j].y1 > ty1) ty1 = E[j].y1; }
        int ux0 = 9999, ux1 = -9999, uy0 = 9999, uy1 = -9999, found = 0;
        for (int j = 0; j < n; j++) if (E[j].tag == BT_PART && E[j].grp == E[i].grp) { found = 1; if (E[j].x0 < ux0) ux0 = E[j].x0; if (E[j].x1 > ux1) ux1 = E[j].x1; if (E[j].y0 < uy0) uy0 = E[j].y0; if (E[j].y1 > uy1) uy1 = E[j].y1; }
        if (!found) continue;
        LCHK(fabsf((ux0 + ux1) / 2.f - 45) <= 2, "row icon off its column (%.1f)", (ux0 + ux1) / 2.f);
        LCHK(fabsf((uy0 + uy1) / 2.f - (ty0 + ty1) / 2) <= 3, "row icon not centred on its text (%.1f vs %.1f)", (uy0 + uy1) / 2.f, (ty0 + ty1) / 2);
    }
    /* nav bar */
    for (int i = 0; i < n; i++) if (E[i].tag == BT_TITLE) for (int j = 0; j < n; j++) if (E[j].tag == BT_BACK) {
        LCHK(fabsf(ecx(&E[i]) - W / 2.f) <= 1, "title not centred");
        LCHK(E[j].x1 + 6 <= E[i].x0, "back label too close to the title (%d / %d)", E[j].x1, E[i].x0);
    }
}
static double worst; static char worst_name[64];
static void page(int kind, const char *addr, int p1, int p2, const char *back, float scroll, const char *name)
{
    static struct bpage pg; memset(&pg, 0, sizeof pg);
    pg.kind = kind; buds_cpy(pg.addr, sizeof pg.addr, addr); pg.p1 = p1; pg.p2 = p2; pg.back = back;
    bu_build(&pg);
    float maxs = bu_content_h(&pg) - (H - BU_TOP); if (maxs < 0) maxs = 0;
    bu_scroll = scroll < 0 ? maxs : scroll > maxs ? maxs : scroll;
    bu_rec = 1; bu_nel = 0;
    double t0 = now_ms();
    bu_render(fr0, &pg, -1);
    double dt = now_ms() - t0; if (dt > worst) { worst = dt; snprintf(worst_name, sizeof worst_name, "%s", name); }
    bu_rec = 0; layout_check(name, BU_TOP);
    save(fr0, name);
}
static void wallpaper(uint16_t *d)
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = rgb(40 + y / 6, 60 + x / 8, 120 + y / 8);
    memcpy(d, fr1, 30 * W * 2);
}
/* Batteries widget items as widgets.inc.c draws them (ring + glyph), small and medium */
static void widget_mock(const char *name)
{
    wallpaper(fr0);
    int pct[3], chg[3], fam[3], part[3];
    for (int m = 0; m < 2; m++) {
        int n = buds_wg_items(pct, chg, fam, part, m ? 3 : 1, m), y = m ? 250 : 60, w = m ? 288 : 137;
        bu_card(fr0, 16, y, w, 137, 20, rgb(36, 36, 40));
        int slots = m ? 4 : 2;
        for (int i = 0; i < slots; i++) {
            float cx = m ? 16 + 14 + (w - 28) / 4.f * i + (w - 28) / 8.f : (i ? 16 + w * .75f - 3 : 16 + w * .25f + 3), cy = y + 58, r = 25;
            int p = i == 0 ? 87 : i - 1 < n ? pct[i - 1] : -2, c = i == 0 ? 0 : i - 1 < n ? chg[i - 1] : 0;
            if (p == -2) { bu_arc(fr0, cx, cy, r, 7, 0, 6.2832f, rgb(70, 70, 76)); continue; }
            bu_arc(fr0, cx, cy, r, 7, 0, 6.2832f, rgb(60, 60, 66));
            if (p > 0) bu_arc(fr0, cx, cy, r, 7, 0, 6.2832f * p / 100.f, p <= 20 && !c ? BU_RED : BU_GREEN);
            if (i) buds_glyph(fr0, cx, cy - r * .08f, r / 44.f, fam[i - 1], part[i - 1], 0, H);
            else fill_rrect(fr0, (int)cx - 6, (int)cy - 9, 12, 18, 3, 0xFFFF, 255);
            char b[12]; snprintf(b, sizeof b, p >= 0 ? "%d %%" : "—", p); int tw = tw_n(b, 15, -1);
            draw_text(fr0, (int)cx - tw / 2, (int)cy + 51, b, 15, 0xFFFF, 0, 0, H);
            if (c) bu_bolt(fr0, cx + tw / 2 + 2, cy + 38, 12, BU_GREEN);
        }
    }
    save(fr0, name);
}
int main(void)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout;
    if (!text_init()) { printf("no font\n"); return 2; }
    mkdir(outdir(), 0755);
    status_bar(); bu_t0 = now_ms() - 10000;
    const char *A = "F0:5C:77:A1:02:14", *S = "F0:5C:77:5A:07:02", *N = "F0:5C:77:4E:B1:62", *M = "F0:5C:77:A1:0A:0A";
    static const int langs[5] = { LANG_ES, LANG_EN, LANG_FR, LANG_JA, LANG_DE };
    static const char *const lc[5] = { "es", "en", "fr", "ja", "de" };
    refresh();
    if (!bvn) { printf("fake daemon state not found (%s)\n", BUDS_STATE); return 2; }
    for (int li = 0; li < 5; li++) {
        ui_lang = langs[li]; char n[64];
        cmd("1 F0:5C:77:A1:02:14 connect 0 0 0\n"); msleep(1900); refresh();
        snprintf(n, sizeof n, "%s_01_list", lc[li]); page(BP_LIST, "", 0, 0, 0, 0, n);
        snprintf(n, sizeof n, "%s_02_airpods", lc[li]); page(BP_DEV, A, 0, 0, "Auriculares", 0, n);
        snprintf(n, sizeof n, "%s_03_airpods_scrolled", lc[li]); page(BP_DEV, A, 0, 0, "Auriculares", -1, n);
        cmd("2 F0:5C:77:A1:02:14 noise 3 0 0\n"); refresh();
        snprintf(n, sizeof n, "%s_04_airpods_adaptive", lc[li]); page(BP_DEV, A, 0, 0, "Auriculares", 0, n);
        snprintf(n, sizeof n, "%s_05_airpods_hold", lc[li]); page(BP_GEST, A, 0, 0, "AirPods Pro", 0, n);
        cmd("3 F0:5C:77:5A:07:02 connect 0 0 0\n"); msleep(1900); refresh();
        snprintf(n, sizeof n, "%s_06_galaxy", lc[li]); page(BP_DEV, S, 0, 0, "Auriculares", 0, n);
        cmd("4 F0:5C:77:5A:07:02 noise 2 0 0\n"); refresh();
        snprintf(n, sizeof n, "%s_07_galaxy_ambient", lc[li]); page(BP_DEV, S, 0, 0, "Auriculares", 0, n);
        snprintf(n, sizeof n, "%s_08_galaxy_scrolled", lc[li]); page(BP_DEV, S, 0, 0, "Auriculares", -1, n);
        snprintf(n, sizeof n, "%s_09_galaxy_gestures", lc[li]); page(BP_GEST, S, 0, 0, "Galaxy Buds2 Pro", 0, n);
        snprintf(n, sizeof n, "%s_10_galaxy_pick", lc[li]); page(BP_PICK, S, 0, BG_HOLD, "Gestos", 0, n);
        cmd("40 F0:5C:77:5A:07:02 eq 1 0 0\n"); refresh();
        snprintf(n, sizeof n, "%s_11_galaxy_eq", lc[li]); page(BP_EQ, S, 0, 0, "Galaxy Buds2 Pro", 0, n);
        cmd("41 F0:5C:77:5A:07:02 eq 5 0 0\n"); refresh();
        snprintf(n, sizeof n, "%s_11b_galaxy_eq_treble", lc[li]); page(BP_EQ, S, 0, 0, "Galaxy Buds2 Pro", 0, n);
        snprintf(n, sizeof n, "%s_12_galaxy_find", lc[li]); page(BP_FINDP, S, 0, 0, "Galaxy Buds2 Pro", 0, n);
        cmd("5 F0:5C:77:5A:07:02 find 0 1 0\n"); refresh();
        snprintf(n, sizeof n, "%s_13_galaxy_find_left", lc[li]); page(BP_FINDP, S, 0, 0, "Galaxy Buds2 Pro", 0, n);
        cmd("6 F0:5C:77:5A:07:02 find 2 0 0\n");
        cmd("7 F0:5C:77:4E:B1:62 connect 0 0 0\n"); msleep(2600); refresh();
        snprintf(n, sizeof n, "%s_14_nothing", lc[li]); page(BP_DEV, N, 0, 0, "Auriculares", 0, n);
        snprintf(n, sizeof n, "%s_15_nothing_scrolled", lc[li]); page(BP_DEV, N, 0, 0, "Auriculares", 330, n);
        cmd("8 F0:5C:77:4E:B1:62 bands 4,-1,2\n"); refresh();
        snprintf(n, sizeof n, "%s_16_nothing_eq", lc[li]); page(BP_EQ, N, 0, 0, "Nothing Ear (a)", 0, n);
        snprintf(n, sizeof n, "%s_17_nothing_eq_bands", lc[li]); page(BP_EQ, N, 0, 0, "Nothing Ear (a)", -1, n);
        snprintf(n, sizeof n, "%s_18_nothing_gestures", lc[li]); page(BP_GEST, N, 0, 0, "Nothing Ear (a)", 0, n);
        cmd("9 F0:5C:77:A1:0A:0A connect 0 0 0\n"); msleep(1900); refresh();
        snprintf(n, sizeof n, "%s_19_max", lc[li]); page(BP_DEV, M, 0, 0, "Auriculares", 0, n);
        snprintf(n, sizeof n, "%s_20_list_all", lc[li]); page(BP_LIST, "", 0, 0, 0, 0, n);
        snprintf(n, sizeof n, "%s_21_supported", lc[li]); page(BP_SUPP, "", 0, 0, "Auriculares", 0, n);
        { static const int br[6] = { BB_APPLE, BB_SAMSUNG, BB_SOUNDCORE, BB_HUAWEI, BB_OPPO, BB_SONY };
          for (int k = 0; k < 6; k++) { snprintf(n, sizeof n, "%s_22_supported_%d", lc[li], br[k]); page(BP_SUPPB, "", br[k], 0, "Dispositivos compatibles", 0, n); } }
        snprintf(n, sizeof n, "%s_23_supported_apple_end", lc[li]); page(BP_SUPPB, "", BB_APPLE, 0, "Dispositivos compatibles", -1, n);
        /* popup: complete data, then the moment right after connecting (values fading in) */
        cmd("10 F0:5C:77:A1:02:14 connect 0 0 0\n"); msleep(1900); refresh();
        buds_cpy(bu_pop_addr, 18, A); bu_pop_t = now_ms() - 2000; bu_pop_on = 1; bu_pop_gen++;
        double best = 1e9, wsum = 0; int frames = 0;
        for (int f = 0; f <= 12; f++) {
            wallpaper(fr0);
            bu_rec = f == 12; bu_nel = 0;
            double t0 = now_ms(); buds_popup_draw(fr0, f / 12.f); double dt = now_ms() - t0;
            if (f == 12) { bu_rec = 0; snprintf(n, sizeof n, "%s_24_popup_full", lc[li]); layout_check(n, 0); }
            if (f) { wsum += dt; frames++; } if (dt < best) best = dt;
            if (f == 6 || f == 12) { snprintf(n, sizeof n, "%s_24_popup_%s", lc[li], f == 6 ? "half" : "full"); save(fr0, n); }
        }
        printf("[%s] popup: %.2f ms per frame (PC)\n", lc[li], wsum / frames);
        buds_cpy(bu_pop_addr, 18, M); bu_pop_t = now_ms(); bu_pop_gen++;
        wallpaper(fr0); bu_rec = 1; bu_nel = 0; buds_popup_draw(fr0, 1); bu_rec = 0; snprintf(n, sizeof n, "%s_25_popup_max", lc[li]); layout_check(n, 0); save(fr0, n);
        buds_cpy(bu_pop_addr, 18, S); bu_pop_t = now_ms(); bu_pop_gen++;
        wallpaper(fr0); bu_rec = 1; bu_nel = 0; buds_popup_draw(fr0, 1); bu_rec = 0; snprintf(n, sizeof n, "%s_25b_popup_galaxy", lc[li]); layout_check(n, 0); save(fr0, n);
        buds_cpy(bu_pop_addr, 18, N); bu_pop_t = now_ms(); bu_pop_gen++;
        wallpaper(fr0); bu_rec = 1; bu_nel = 0; buds_popup_draw(fr0, 1); bu_rec = 0; snprintf(n, sizeof n, "%s_25c_popup_nothing", lc[li]); layout_check(n, 0); save(fr0, n);
        snprintf(n, sizeof n, "%s_26_widget", lc[li]); widget_mock(n);
    }
    /* empty state */
    ui_lang = LANG_EN; bvn = 0; { static struct bpage pg; memset(&pg, 0, sizeof pg); pg.kind = BP_LIST; bvh.pid = 1; bu_build(&pg); bu_scroll = 0; bu_rec = 1; bu_nel = 0; bu_render(fr0, &pg, -1); bu_rec = 0; layout_check("en_00_empty", BU_TOP); save(fr0, "en_00_empty"); }
    printf("slowest page: %s %.2f ms (PC)\n", worst_name, worst);
    /* the checker itself must catch bad layouts: overlapping products, a ring row on a product,
     * a misplaced ring label */
    {
        int f0 = lfails, c0 = lchecks; struct buds_st x; buds_st_init(&x); x.caps = BC_BATT_LR | BC_BATT_CASE; x.batt[0] = 50; x.batt[1] = 60; x.batt[2] = 70; x.chg[2] = 1;
        printf("checker self-test (the next LAYOUT lines are expected):\n");
        bu_rec = 1; bu_nel = 0; bu_grp = 5;
        bu_layout(fr0, 20, 100, 200, 200, BF_PODS_PRO, -1, BL_HERO, 0, 2);
        bu_layout(fr0, 120, 100, 300, 200, BF_GBUDS, -1, BL_HERO, 0, 2);
        bu_rings_row(fr0, W / 2.f, 190, &x, 0);
        bu_els[bu_nel - 1].x0 += 9; bu_els[bu_nel - 1].x1 += 9;
        bu_rec = 0; layout_check("self-test", 0);
        int caught = lfails - f0;
        lfails = f0; lchecks = c0;
        printf("checker self-test: %d faults caught (%s)\n", caught, caught >= 3 ? "ok" : "FAILED");
        if (caught < 3) lfails++;
    }
    printf("layout: %d checks, %d failures\n", lchecks, lfails);
    return lfails != 0;
}
