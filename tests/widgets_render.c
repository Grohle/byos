/* widgets_render.c - the Home Screen widgets (src/widgets.inc.c, WG_RENDER_ONLY) on the PC, as PNG:
 * every widget in every size it has, on the real default wallpaper, in the 13 languages, with
 * realistic data (headphones from the real fake daemon hibybudsd --fake, like buds_render).
 * Fails (exit 1) if any interface text had to be cut ("…") in any language.
 *   bash tests/widgets_render.sh  ->  out/widgets_render/<lang>.png (+ $WG_OUT) */
#define _GNU_SOURCE
#include <ctype.h>
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
#include "i18n.inc.c"
#include "text.inc.c"
#include "wg_render_extract.inc"          /* fill_rrect, fill_circle, draw_wrapped, wrap_cjk... (from the shell, as is) */
/* what widgets.inc.c (WG_RENDER_ONLY) needs from the rest of the shell */
struct np_info { int source, playing, stopped; char path[400]; char title[160], artist[128], album[128]; int duration_ms, position_ms, rate, bits, channels; uint32_t track_id; };
static uint16_t wall[W * H]; static int wall_gen = 1;
enum { PW_SAVE, PW_NORMAL, PW_PERF };
static int pw_mode = PW_NORMAL, lcd_hz = 45, pw_nocharge;
static const char *pw_name(int m) { return m == PW_SAVE ? "Ahorro" : m == PW_PERF ? "Rendimiento" : "Normal"; }
#define WG_RENDER_ONLY 1
#include "widgets.inc.c"
#include "buds.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static const char *outdir(void) { return getenv("WG_OUT") ? getenv("WG_OUT") : "out/widgets_render"; }
#define CW (W * 4)                              /* contact sheet: 4 pages side by side */
static uint16_t sheet[CW * H];
static void save_sheet(const char *name)
{
    static unsigned char px[CW * H * 3];
    for (int i = 0; i < CW * H; i++) { uint16_t p = sheet[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, CW, H, 3, px, CW * 3);
}
static void cmd(const char *c) { int f = open(BUDS_CMD, O_WRONLY | O_NONBLOCK); if (f >= 0) { if (write(f, c, strlen(c)) < 0) {} close(f); } msleep(150); }
static void refresh(void) { bu_read_state(); bv_gen = ~0u; bu_snap(); }
static const char *cur_lang = "";
static int nfail;
static void on_trunc(const char *t, int size, int maxw) { nfail++; printf("CUT [%s] \"%s\" (%d px in %d px)\n", cur_lang, t, size, maxw); }
/* one page: wallpaper + the items in flow layout (the page's real composition, sprites included) */
static double worst; static char worst_name[64];
static void page(int slot, const unsigned char *types, const unsigned char *sizes, int n)
{
    static uint16_t pg[W * H];
    memcpy(pg, wall, sizeof pg);
    for (int y = 0; y < 30; y++) for (int x = 0; x < W; x++) pg[y * W + x] = dim565(pg[y * W + x], 22);
    short xs[WG_MAX], ys[WG_MAX]; wg_layout(sizes, n, xs, ys);
    for (int i = 0; i < n; i++) {
        int s = sizes[i], w = wg_w(s), h = wg_h(s), y = WG_TOP + ys[i];
        double t0 = now_ms(); wg_draw_scr(types[i], s, xs[i], y); double dt = now_ms() - t0;
        if (dt > worst) { worst = dt; snprintf(worst_name, sizeof worst_name, "%s %s:%c", cur_lang, wg_id[types[i]], wg_szc[s]); }
        wg_blit_m(pg, wg_scr, W, wg_mask[s], w, h, xs[i], y, 30, (int)WG_R);
    }
    for (int y = 0; y < H; y++) memcpy(sheet + y * CW + slot * W, pg + y * W, W * 2);
}
static void fake_cover(uint16_t *c, int k)
{
    static const int pal[][6] = { { 236, 226, 205, 60, 60, 70 }, { 30, 60, 120, 240, 170, 60 }, { 150, 30, 40, 250, 220, 200 }, { 20, 90, 70, 200, 230, 120 },
                                  { 90, 40, 140, 250, 120, 180 }, { 40, 40, 44, 230, 90, 40 }, { 200, 120, 40, 60, 30, 20 } };
    const int *q = pal[k % 7];
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) {
        float t = (x + y) / 254.f, s = (k & 1) ? sinf(x * .09f + k) * cosf(y * .07f) : ((x / 16 + y / 16) & 1) * .25f;
        int r = (int)(q[0] + (q[3] - q[0]) * t * .6f + 40 * s), g = (int)(q[1] + (q[4] - q[1]) * t * .6f + 40 * s), b = (int)(q[2] + (q[5] - q[2]) * t * .6f + 40 * s);
        if ((k % 3) == 0 && abs(x - 64) < 30 && abs(y - 70) < 4) { r = q[3]; g = q[4]; b = q[5]; }
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
        c[y * 128 + x] = rgb(r, g, b);
    }
}
int main(int argc, char **argv)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout; (void)kb_cfg_layouts; (void)argc;
    if (!text_init()) { printf("no font\n"); return 2; }
    mkdir(outdir(), 0755);
    {   /* default wallpaper of the shell (RGB565, 320x480) */
        FILE *f = fopen(argv[1] ? argv[1] : "out/sd/ByOS/payload/shell/wall.raw", "rb");
        if (!f || fread(wall, 2, W * H, f) != W * H) { printf("no wall.raw\n"); return 2; }
        fclose(f);
    }
    wg_trunc_hook = on_trunc;
    bu_t0 = now_ms() - 10000; refresh();
    if (!bvn) { printf("fake daemon state not found (%s)\n", BUDS_STATE); return 2; }
    cmd("1 F0:5C:77:A1:02:14 connect 0 0 0\n"); msleep(1900); refresh();   /* AirPods Pro 2 */
    /* data */
    struct wdata *D = &WDs;
    D->batt = 76; D->charging = 0; D->usb = 0;
    { int p[3], c[3], f[3], k[3];
      D->nhp = buds_wg_items(p, c, f, k, 3, 1); for (int i = 0; i < D->nhp; i++) D->hp[i] = (struct wg_hp){ p[i], c[i], f[i], k[i] };
      D->nhps = buds_wg_items(p, c, f, k, 1, 0); for (int i = 0; i < D->nhps; i++) D->hps[i] = (struct wg_hp){ p[i], c[i], f[i], k[i] }; }
    printf("headphones items: %d split, %d merged\n", D->nhp, D->nhps);
    D->sd_tot = 128LL << 30; D->sd_free = 41LL << 30; D->in_tot = 45LL << 20; D->in_free = 11LL << 20;
    D->songs = 2488; D->albums = 212; D->artists = 143; D->favs = 87; D->lib_bytes = 61LL << 30;
    snprintf(D->top_artist, 64, "Kimiko Ishizaka"); D->top_n = 23;
    snprintf(D->last_title, 96, "Prelude No. 1 in C Major, BWV 846"); snprintf(D->last_artist, 64, "Kimiko Ishizaka");
    static const char *const ra[WG_NREC][2] = { { "Gymnopédies & Gnossiennes", "Erik Satie" }, { "The Open Goldberg Variations", "Kimiko Ishizaka" }, { "Blue Train", "John Coltrane" },
        { "Kind of Blue", "Miles Davis" }, { "Random Access Memories", "Daft Punk" }, { "In Rainbows", "Radiohead" } };
    D->nrec = WG_NREC;
    for (int i = 0; i < WG_NREC; i++) {
        snprintf(D->rec_alb[i], 64, "%s", ra[i][0]); snprintf(D->rec_art[i], 48, "%s", ra[i][1]); D->rec_cov[i] = i != 4;
        static uint16_t c[128 * 128]; fake_cover(c, i + 1);
        for (int y = 0; y < WG_RCS; y++) for (int x = 0; x < WG_RCS; x++) wg_rcov[i][y * WG_RCS + x] = c[(y * 128 / WG_RCS) * 128 + x * 128 / WG_RCS];
    }
    static const int week[7] = { 42, 75, 18, 0, 96, 64, 37 };
    for (int k = 0; k < 7; k++) D->lis_day[k] = week[k] * 60;
    wg_ni.source = 1; wg_ni.playing = 1; snprintf(wg_ni.title, 160, "Gymnopédie No. 1"); snprintf(wg_ni.artist, 128, "Erik Satie");
    snprintf(wg_ni.album, 128, "Gymnopédies & Gnossiennes"); snprintf(wg_ni.path, 400, "/usr/data/mnt/sd_0/Satie/Gymnopedies/01.flac");
    wg_ni.duration_ms = 214000; wg_ni.position_ms = 83000; wg_ni.rate = 96000; wg_ni.bits = 24; wg_ni.track_id = 7;
    fake_cover(wg_cov, 0); wg_cok = 1; wg_tint = wg_cover_tint(wg_cov);
    /* pages: every type in every size */
    static const unsigned char t1[] = { WG_BATT, WG_CLOCK, WG_NOW, WG_NEW, WG_LISTEN }, s1[] = { 0, 0, 1, 1, 1 };
    static const unsigned char t2[] = { WG_NOW, WG_DATE, WG_MIX }, s2[] = { 2, 0, 0 };
    static const unsigned char t3[] = { WG_NEW, WG_BATT }, s3[] = { 2, 1 };
    static const unsigned char t4[] = { WG_BATT, WG_DATE, WG_CLOCK }, s4[] = { 2, 1, 1 };
    static const unsigned char t5[] = { WG_MIX, WG_LISTEN, WG_STORAGE, WG_LIB, WG_POWER }, s5[] = { 1, 1, 1, 1, 1 };
    static const unsigned char t6[] = { WG_NOW, WG_NEW, WG_STORAGE, WG_LIB, WG_POWER, WG_LISTEN }, s6[] = { 0, 0, 0, 0, 0, 0 };
    for (int l = 0; l < LANG_N; l++) {
        ui_lang = l; cur_lang = LANG_CODES[l]; wg_bl_gen = -1;
        memset(sheet, 0, sizeof sheet);
        page(0, t1, s1, 5); page(1, t2, s2, 3); page(2, t3, s3, 2); page(3, t4, s4, 3);
        char n[64]; snprintf(n, sizeof n, "%s_a", LANG_CODES[l]); save_sheet(n);
        memset(sheet, 0, sizeof sheet);
        page(0, t5, s5, 5); page(1, t6, s6, 6);
        /* headphones only in small (merged) + no playback, BlueZ-less HiBy alone */
        { struct wdata sv = *D; struct np_info ni = wg_ni; D->nhp = D->nhps = 0; D->nbt = 0; D->charging = 1; wg_ni.source = 0;
          D->last_title[0] = 0;
          static const unsigned char t7[] = { WG_BATT, WG_NOW, WG_NOW }, s7[] = { 0, 0, 2 }; page(2, t7, s7, 3); *D = sv; wg_ni = ni; }
        { struct np_info ni = wg_ni; wg_ni.playing = 0; snprintf(wg_ni.title, 160, "Prelude No. 1 in C Major, BWV 846 (Well-Tempered Clavier)"); snprintf(wg_ni.artist, 128, "Kimiko Ishizaka");
          D->nrec = 0; D->new_album[0] = 0;
          static const unsigned char t8[] = { WG_NOW, WG_NEW, WG_NOW }, s8[] = { 1, 1, 2 }; page(3, t8, s8, 3);
          snprintf(n, sizeof n, "%s_b", LANG_CODES[l]); save_sheet(n);
          /* nothing open: the last played track in the three sizes */
          memset(sheet, 0, sizeof sheet); wg_ni.source = 0;
          static const unsigned char t9[] = { WG_NOW, WG_NEW, WG_NOW, WG_NOW }, s9[] = { 0, 0, 1, 2 }; page(0, t9, s9, 4);
          snprintf(n, sizeof n, "%s_c", LANG_CODES[l]); save_sheet(n);
          wg_ni = ni; D->nrec = WG_NREC; }
    }
    printf("slowest widget render on the PC: %.2f ms (%s)\n", worst, worst_name);
    printf("%s: %d cut interface texts\n", nfail ? "FAIL" : "OK", nfail);
    return nfail ? 1 : 0;
}
