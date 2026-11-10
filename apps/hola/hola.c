/* hola - app de ejemplo de hibymod (paquete .hpk). Fondo degradado, saludo en el idioma del
 * shell (HM_LANG) con una fuente de mapa de bits 8x16 ampliada, y un circulo que sigue al dedo
 * (aparece con muelle y se desvanece al soltar). Sale limpiamente con SIGTERM.
 * Compilar: ~/hiby/tools/zbuild_app.sh hola   ->  out/apps/hola.hpk */
#include <math.h>
#include <stdio.h>
#include "hm_app.h"
#include "font8x16.h"

static uint16_t bg[HM_W * HM_H], img[HM_W * HM_H];

static inline uint16_t blend(uint16_t d, int r, int g, int b, int a)   /* a 0..256 */
{
    int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    dr += (((r >> 3) - dr) * a) >> 8; dg += (((g >> 2) - dg) * a) >> 8; db += (((b >> 3) - db) * a) >> 8;
    return (uint16_t)(dr << 11 | dg << 5 | db);
}

/* degradado diagonal de tres paradas con tramado ordenado 4x4 (sin bandas en RGB565) */
static void paint_bg(void)
{
    static const float st[3][3] = { { 24, 28, 74 }, { 110, 58, 214 }, { 255, 112, 128 } };
    static const int bay[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (int y = 0; y < HM_H; y++) for (int x = 0; x < HM_W; x++) {
        float t = (y * 0.85f + x * 0.35f) / (HM_H * 0.85f + HM_W * 0.35f), c[3];
        int k = t < .55f ? 0 : 1; float u = k ? (t - .55f) / .45f : t / .55f;
        u = u * u * (3 - 2 * u);
        for (int i = 0; i < 3; i++) c[i] = st[k][i] + (st[k + 1][i] - st[k][i]) * u;
        /* halo suave arriba a la izquierda */
        float dx = x - 70, dy = y - 110, gl = expf(-(dx * dx + dy * dy) / (2 * 120.f * 120.f)) * 40;
        float th = (bay[y & 3][x & 3] + .5f) / 16.f;
        int r = (int)(c[0] + gl), g = (int)(c[1] + gl), b = (int)(c[2] + gl);
        r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
        int R = (r >> 3) + ((r & 7) / 8.f > th), G = (g >> 2) + ((g & 3) / 4.f > th), B = (b >> 3) + ((b & 7) / 8.f > th);
        bg[y * HM_W + x] = (uint16_t)((R > 31 ? 31 : R) << 11 | (G > 63 ? 63 : G) << 5 | (B > 31 ? 31 : B));
    }
}

/* ------------------------------------------------------------------ texto */
static const unsigned char *glyph(unsigned cp)
{
    for (int i = 0; i < FONT_N; i++) if (font_cp[i] == cp) return font_bits[i];
    return font_bits['?' - 32];
}
static unsigned utf8(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s; unsigned c = *p++;
    if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p++; }
    *s = (const char *)p; return c;
}
static int text_len(const char *s) { int n = 0; while (*s) { utf8(&s); n++; } return n; }

static void text(uint16_t *dst, const char *s, int x, int y, int sc, int r, int g, int b, int a)
{
    while (*s) {
        const unsigned char *gl = glyph(utf8(&s));
        for (int j = 0; j < 16; j++) for (int i = 0; i < 8; i++) {
            if (!(gl[j] & (0x80 >> i))) continue;
            for (int v = 0; v < sc; v++) for (int u = 0; u < sc; u++) {
                int px = x + i * sc + u, py = y + j * sc + v;
                if (px >= 0 && px < HM_W && py >= 0 && py < HM_H) dst[py * HM_W + px] = blend(dst[py * HM_W + px], r, g, b, a);
            }
        }
        x += 8 * sc;
    }
}
/* centrado, con sombra suave (dos pasadas desplazadas) */
static void text_c(const char *s, int y, int sc, int a)
{
    int x = (HM_W - text_len(s) * 8 * sc) / 2;
    text(bg, s, x + 1, y + sc, sc, 0, 0, 0, 50);
    text(bg, s, x, y + sc + 1, sc, 0, 0, 0, 40);
    text(bg, s, x, y, sc, 255, 255, 255, a);
}

/* ------------------------------------------------------------------ circulo */
static void disc(int cx, int cy, float rad, float op)
{
    if (op <= 0.01f || rad < 1) return;
    int R = (int)rad + 14;
    for (int y = cy - R; y <= cy + R; y++) {
        if (y < 0 || y >= HM_H) continue;
        for (int x = cx - R; x <= cx + R; x++) {
            if (x < 0 || x >= HM_W) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy, d = sqrtf(dx * dx + dy * dy) - rad;
            float a;
            if (d < -2.5f) a = .78f - .18f * (-d / rad);                 /* relleno con leve volumen */
            else if (d < .5f) a = .95f;                                  /* borde nitido */
            else a = .32f * expf(-(d - .5f) / 4.f);                      /* halo */
            float cov = d < 1.5f && d > .5f ? 1.5f - d : 1;
            if (d >= .5f && d <= 1.5f) a = .95f * cov + a * (1 - cov);
            int al = (int)(a * op * 256);
            if (al > 0) img[y * HM_W + x] = blend(img[y * HM_W + x], 255, 255, 255, al);
        }
    }
}

struct finger { int x, y, down; long t_down, t_up; };
static void on_touch(int ev, int x, int y, void *c)
{
    struct finger *f = c;
    f->x = x; f->y = y;
    if (ev == HM_DOWN) { f->down = 1; f->t_down = hm_now_ms(); }
    if (ev == HM_UP) { f->down = 0; f->t_up = hm_now_ms(); }
}

int main(void)
{
    hm_fb fb; hm_touch t;
    hm_app_init();
    if (hm_fb_open(&fb)) { fprintf(stderr, "hola: sin framebuffer\n"); return 1; }
    if (hm_touch_open(&t)) fprintf(stderr, "hola: sin tactil\n");
    paint_bg();
    text_c(hm_tr("Hola", "Hello", "Bonjour"), 150, 5, 256);
    text_c(hm_tr("Toca la pantalla", "Touch the screen", "Touchez l'écran"), 250, 2, 230);
    text_c(hm_tr("Desliza hacia arriba para salir", "Swipe up to exit", "Glissez vers le haut pour quitter"), 420, 1, 170);
    struct finger f = { HM_W / 2, HM_H / 2, 0, 0, -10000 };
    int dirty = 1;
    while (!hm_quit) {
        long now = hm_now_ms();
        float sc = 0, op = 0;
        if (f.down) {                                     /* entra con muelle (sobrepasa un poco) */
            float u = (now - f.t_down) / 320.f; if (u > 1) u = 1;
            sc = 1 - expf(-6 * u) * cosf(7 * u); op = u * 4 > 1 ? 1 : u * 4;
        } else {                                          /* sale con ease-out cubico en 250 ms */
            float u = (now - f.t_up) / 250.f;
            if (u < 1) { float e = 1 - (1 - u) * (1 - u) * (1 - u); sc = 1 + .25f * e; op = 1 - e; }
        }
        int anim = (f.down && now - f.t_down < 320) || (!f.down && now - f.t_up < 250);
        if (dirty || anim) {
            memcpy(img, bg, sizeof img);
            disc(f.x, f.y, 30 * sc, op);
            hm_fb_present(&fb, img);                      /* lienzo completo -> bufer oculto -> pan */
            dirty = 0;
        }
        hm_wait(&t, 0, anim ? 22 : 1000);                 /* parado: no gasta CPU */
        if (hm_touch_read(&t, on_touch, &f)) dirty = 1;
    }
    hm_touch_close(&t);
    hm_fb_close(&fb);
    return 0;
}
