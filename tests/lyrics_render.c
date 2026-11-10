/* lyrics_render.c - fotogramas de la busqueda de letras (src/lyrics.inc.c) en el PC, en PNG, para
 * revisar el diseno sin el aparato: lista de resultados (con insignias LRC / duracion / primer
 * verso), vista previa sincronizada con el ajuste y estados vacio / sin red, en es/en/fr/ja/ru.
 *   bash tests/lyrics_render.sh  ->  out/lyrics_render/ (o $LYR_OUT), un PNG por pantalla e idioma
 * Usa las funciones de dibujo de verdad del shell (extraidas por el .sh) y lyrics.inc.c tal cual. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define W 320
#define H 480
#define HM_FRAME_BYTES (W * H * 2)
extern char **environ;
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
static double fake_now = 100000;
static double now_ms(void) { return fake_now; }
static void msleep(int ms) { (void)ms; }
static void lg(const char *f, ...) { (void)f; }
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static inline uint16_t dim565(uint16_t p, int k) { unsigned r = (p >> 11) * k >> 5, g = ((p >> 5) & 63) * k >> 5, b = (p & 31) * k >> 5; return (uint16_t)(r << 11 | g << 5 | b); }
static uint16_t blend565(uint16_t a, uint16_t b, int k)
{
    int r = ((a >> 11) * (32 - k) + (b >> 11) * k) >> 5, g = (((a >> 5) & 63) * (32 - k) + ((b >> 5) & 63) * k) >> 5, bl = ((a & 31) * (32 - k) + (b & 31) * k) >> 5;
    return (uint16_t)(r << 11 | g << 5 | bl);
}
static inline uint16_t mix565(uint16_t c, uint16_t d, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
static uint16_t fb0[W * H], fb1[W * H], *fr0 = fb0, *fr1 = fb1;
static int hz_hold, cur_x, cur_y, cur_down;
struct pkt { int n; int x, y, down; };
static int read_pkt(struct pkt *p, int t) { (void)p; (void)t; return 0; }
static void drain_until_up(void) {}
static void present(void) {}
static double toast_until;
static void toast(const char *m) { (void)m; }
static void draw_toast(uint16_t *d) { (void)d; }
static void progress_card(const char *t, const char *s, double f) { (void)t; (void)s; (void)f; }
static int run_capture(char *const argv[], char *buf, int cap) { (void)argv; (void)buf; (void)cap; return -1; }
static int action_sheet(const char *t, const char **o, int n, int d) { (void)t; (void)o; (void)n; (void)d; return -1; }
static int line_edit(const char *t, const char *h, char *b, int c) { (void)t; (void)h; (void)b; (void)c; return 0; }
static int image_picker(char *o, int c) { (void)o; (void)c; return 0; }
#define TAGBIN "/bin/false"
#include "hibytag.h"
int ht_read(const char *p, ht_tags *t) { (void)p; memset(t, 0, sizeof *t); return -1; }
void ht_tags_free(ht_tags *t) { (void)t; }
int ht_set_cover(const char *p, const char *img) { (void)p; (void)img; return -1; }
enum { NP_SRC_NONE, NP_SRC_SHIM };
struct np_info { int source, playing, stopped; char path[400]; char title[160], artist[128], album[128]; int duration_ms, position_ms; };
struct np_meta { int duration_ms; };
static void np_read_meta(const char *p, struct np_meta *m) { (void)p; m->duration_ms = 369000; }
static void npx_snapshot(struct np_info *o, uint16_t *c, int *k) { (void)c; (void)k; memset(o, 0, sizeof *o); }
#include "i18n.inc.c"
#include "text.inc.c"
#include "lyrics_render_extract.inc"     /* fill_rrect, fill_circle, mu_line, mu_text_bold, draw_wrapped... (del shell) */
static int spn_phase(double t) { (void)t; return 0; }
static void spn_draw(uint16_t *d, int cx, int cy, int phase, int n, int c0, int c1)
{
    (void)phase; (void)n;
    for (int i = 0; i < 8; i++) { float a = i * 0.785398f; if (cy + 10 > c0 && cy - 10 < c1) mu_line(d, cx + cosf(a) * 5, cy + sinf(a) * 5, cx + cosf(a) * 9.5f, cy + sinf(a) * 9.5f, 2.4f, dim565(rgb(200, 200, 205), 32 - i * 3)); }
}
#include "lyrics.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static void save(const char *name)
{
    static unsigned char px[W * H * 3];
    for (int i = 0; i < W * H; i++) { uint16_t p = fr0[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[256]; snprintf(p, sizeof p, "%s/%s.png", getenv("LYR_OUT") ? getenv("LYR_OUT") : "out/lyrics_render", name);
    stbi_write_png(p, W, H, 3, px, W * 3);
}
static void status_bar(void) { for (int i = 0; i < 30 * W; i++) fr1[i] = rgb(0, 0, 0); draw_text(fr1, 20, 21, "9:41", 15, 0xFFFF, 0, 0, H); }
static void load_results(const char *f, int want)
{
    static char b[65536]; FILE *fp = fopen(f, "rb"); int n = fp ? (int)fread(b, 1, sizeof b - 1, fp) : 0; if (fp) fclose(fp); b[n] = 0;
    nlc = lrc_parse_results(b, lc, LC_MAX); lc_want = want;
    nlc = lrc_rank(lc, nlc, want, "Daft Punk", "Get Lucky"); lc_rc = nlc ? 0 : 1;
}
/* el fotograma de lyrics_preview (misma composicion) */
static void preview_frame(struct lx_view *v, int pos, int off, int follow, int press)
{
    const int ly0 = 100, card = H - (v->L.synced ? 186 : 120), ly1 = card - 4;
    v->cur = -2; lx_view_tick(v, pos, off); v->sc = v->sc1; v->st = 0;
    for (int k = 94 * W; k < W * H; k++) fr0[k] = 0;
    lx_view_draw(fr0, v, ly0, ly1, 0);
    lx_card(fr0, 0, card, W, H - card + 20, 16, rgb(28, 28, 30));
    int y = card + 12;
    char tt[40]; snprintf(tt, sizeof tt, "%d:%02d / 6:09", pos / 60000, pos / 1000 % 60);
    if (follow) { fill_rrect(fr0, 20, y + 6, 8, 8, 4, LX_GREEN, 255); draw_text(fr0, 34, y + 15, tr("Sonando ahora"), 13, rgb(152, 152, 159), 150, 0, H); }
    else {
        fill_rrect(fr0, 16, y - 2, 26, 26, 13, rgb(58, 58, 62), 255);
        fill_rrect(fr0, 24, y + 5, 3, 11, 1, 0xFFFF, 255); fill_rrect(fr0, 31, y + 5, 3, 11, 1, 0xFFFF, 255);
        int bx = 50, bw = W - 50 - 96; fill_rrect(fr0, bx, y + 10, bw, 3, 1.5f, rgb(72, 72, 76), 255); fill_rrect(fr0, bx, y + 10, bw * pos / 369000, 3, 1.5f, rgb(200, 200, 205), 255);
    }
    draw_text(fr0, W - 16 - tw_n(tt, 13, -1), y + 15, tt, 13, rgb(152, 152, 159), 0, 0, H);
    lx_adjust(fr0, card + 40, off, press, rgb(58, 58, 62), 0xFFFF, rgb(142, 142, 147));
    lx_button(fr0, 16, H - 58, W - 32, 46, tr("Usar esta letra"), LX_BLUE, 0xFFFF, press == 4);
    lx_nav(fr0, tr("Resultados"), tr("Vista previa"), 0);
}
int main(void)
{
    if (!text_init()) { printf("no se pudo cargar default.otf\n"); return 2; }
    mkdir("out", 0755); mkdir(getenv("LYR_OUT") ? getenv("LYR_OUT") : "out/lyrics_render", 0755);
    status_bar();
    static const int langs[] = { LANG_ES, LANG_EN, LANG_FR, LANG_JA, LANG_RU };
    for (unsigned li = 0; li < sizeof langs / sizeof langs[0]; li++) {
        ui_lang = langs[li]; const char *code = LANG_CODES[ui_lang]; char n[64];
        load_results("tests/lyrics_samples/get_lucky_248.tsv", 369);
        lyrics_render(fr0, "Daft Punk \xE2\x80\x94 Get Lucky \xC2\xB7 6:09", 0, -1); snprintf(n, sizeof n, "list_%s", code); save(n);
        lyrics_render(fr0, "Daft Punk \xE2\x80\x94 Get Lucky \xC2\xB7 6:09", 230, 4); snprintf(n, sizeof n, "list_scrolled_%s", code); save(n);
        nlc = 0; lc_rc = 2; lyrics_render(fr0, "Daft Punk \xE2\x80\x94 Get Lucky", 0, -1); snprintf(n, sizeof n, "nonet_%s", code); save(n);
        lc_rc = 1; lyrics_render(fr0, "Daft Punk \xE2\x80\x94 Get Lucky", 0, -1); snprintf(n, sizeof n, "none_%s", code); save(n);
        lc_rc = -1; lyrics_render(fr0, "Daft Punk \xE2\x80\x94 Get Lucky", 0, -1); snprintf(n, sizeof n, "searching_%s", code); save(n);
        struct lx_view v;
        if (lx_view_load(&v, "tests/lyrics_samples/get_lucky_986804.lrc")) { printf("no se pudo leer la muestra\n"); return 1; }
        preview_frame(&v, 47500, 500, 0, -1); snprintf(n, sizeof n, "preview_%s", code); save(n);
        preview_frame(&v, 47500, -1250, 1, 2); snprintf(n, sizeof n, "preview_follow_drag_%s", code); save(n);
        lx_view_free(&v);
    }
    printf("PNG en out/lyrics_render/\n");
    return 0;
}
