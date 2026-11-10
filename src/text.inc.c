/* text.inc.c - texto con stb_truetype sobre la fuente del sistema (mapeada, sin copiar).
 * Incluido desde hibyshell.c. Genera sprites (RGB565 + alfa) con sombra opcional. */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static stbtt_fontinfo font;       /* principal: default.otf (latin, cirilico basico, kana y CJK) */
static int font_ok;
static float font_em_k;           /* px de em por px de alto de la principal (iguala las de reserva) */

/* Fuentes de reserva del firmware, por glifo: solo se mapean (mmap, sin copiar a RAM) la primera
 * vez que hace falta un caracter que la principal no tiene. msyh: latin extendido (polaco) y
 * cirilico completo (ucraniano); Korean: hangul; Thai: tailandes. */
#ifndef TEXT_FONT_DIRS
#define TEXT_FONT_DIRS "/usr/data/hibymod/fast/", "/usr/resource/fonts/"
#endif
static const char *const font_dirs[] = { TEXT_FONT_DIRS, 0 };
static struct { const char *file; stbtt_fontinfo fi; int st; long len; } font_fb[] = { { "msyh.ttf", { 0 }, 0, 0 }, { "Korean.ttf", { 0 }, 0, 0 }, { "Thai.ttf", { 0 }, 0, 0 } };
#define FONT_NFB ((int)(sizeof font_fb / sizeof font_fb[0]))

static long font_map(const char *file, stbtt_fontinfo *fi)
{
    for (int i = 0; font_dirs[i]; i++) {
        char p[160]; snprintf(p, sizeof p, "%s%s", font_dirs[i], file);
        int f = open(p, O_RDONLY);
        if (f < 0) continue;
        struct stat s; if (fstat(f, &s) || s.st_size < 64) { close(f); continue; }
        void *m = mmap(0, s.st_size, PROT_READ, MAP_SHARED, f, 0);
        close(f);
        if (m == MAP_FAILED) continue;
        if (stbtt_InitFont(fi, m, stbtt_GetFontOffsetForIndex(m, 0))) return (long)s.st_size;
        munmap(m, s.st_size);
    }
    return 0;
}

static int text_init(void)
{
    if (!font_map("default.otf", &font)) return 0;
    font_ok = 1;
    font_em_k = stbtt_ScaleForPixelHeight(&font, 1) / stbtt_ScaleForMappingEmToPixels(&font, 1);
    return 1;
}

/* fuente y glifo de un codigo: la principal o, si no lo tiene, la primera de reserva que si */
static const stbtt_fontinfo *font_glyph(int cp, int *gi)
{
    int g = stbtt_FindGlyphIndex(&font, cp);
    if (g || cp < 0x80) { *gi = g; return &font; }
    for (int i = 0; i < FONT_NFB; i++) {
        if (!font_fb[i].st) { font_fb[i].len = font_map(font_fb[i].file, &font_fb[i].fi); font_fb[i].st = font_fb[i].len ? 1 : -1; }
        if (font_fb[i].st > 0 && (g = stbtt_FindGlyphIndex(&font_fb[i].fi, cp))) { *gi = g; return &font_fb[i].fi; }
    }
    *gi = 0; return &font;
}
/* escala de una fuente para "px" de alto de la principal (las de reserva, al mismo tamano de em) */
static float font_scale(const stbtt_fontinfo *f, float px)
{
    return f == &font ? stbtt_ScaleForPixelHeight(&font, px) : stbtt_ScaleForMappingEmToPixels(f, px * font_em_k);
}
/* marcas combinantes (diacriticos, vocales y tonos tailandeses): no avanzan */
static int text_is_mark(int cp) { return (cp >= 0x300 && cp < 0x370) || cp == 0xE31 || (cp >= 0xE34 && cp <= 0xE3A) || (cp >= 0xE47 && cp <= 0xE4E); }
static float glyph_adv_px(const stbtt_fontinfo *f, int gi, int cp, float sc)
{
    if (text_is_mark(cp)) return 0;
    int adv, lsb; stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb); return adv * sc;
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p;
    if (!c) return 0;
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n == 1) { *s += 1; return c; }
    c &= (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) { if ((p[i] & 0xC0) != 0x80) { *s += i; return 0xFFFD; } c = (c << 6) | (p[i] & 0x3F); }
    *s += n;
    return c;
}

static int text_width(const char *t, float px)
{
    if (!font_ok) return 0;
    float w = 0; int prev = 0, c; const stbtt_fontinfo *pf = 0;
    while ((c = utf8_next(&t))) {
        int gi; const stbtt_fontinfo *f = font_glyph(c, &gi); float sc = font_scale(f, px);
        if (prev && f == pf) w += stbtt_GetGlyphKernAdvance(f, prev, gi) * sc;
        w += glyph_adv_px(f, gi, c, sc); prev = gi; pf = f;
    }
    return (int)(w + 0.5f);
}

/* rasteriza "t" en una mascara de cobertura (w x h), linea base a la altura de ascent
 * (mas alta si un glifo de reserva, p. ej. un tono tailandes, sale de la caja de la principal) */
static uint8_t *text_mask(const char *t, float px, int *ow, int *oh, int *obase)
{
    if (!font_ok) return 0;
    float sc0 = stbtt_ScaleForPixelHeight(&font, px);
    int asc, desc, gap; stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    int base = (int)(asc * sc0 + 0.5f), h = (int)((asc - desc) * sc0 + 0.999f) + 1;
    int w = text_width(t, px) + 2;
    if (w <= 2) return 0;
    int top = 0, bot = h, c; const char *s = t;
    while ((c = utf8_next(&s))) {
        int gi; const stbtt_fontinfo *f = font_glyph(c, &gi); if (f == &font) continue;
        float sc = font_scale(f, px); int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(f, gi, sc, sc, &x0, &y0, &x1, &y1);
        if (base + y0 < top) top = base + y0;
        if (base + y1 + 1 > bot) bot = base + y1 + 1;
    }
    base -= top; h = bot - top;
    uint8_t *m = calloc(w * h, 1);
    float x = 1; int prev = 0; const stbtt_fontinfo *pf = 0;
    s = t;
    while ((c = utf8_next(&s))) {
        int gi; const stbtt_fontinfo *f = font_glyph(c, &gi); float sc = font_scale(f, px);
        if (prev && f == pf) x += stbtt_GetGlyphKernAdvance(f, prev, gi) * sc;
        int x0, y0, x1, y1;
        float fx = x - (int)x;
        stbtt_GetGlyphBitmapBoxSubpixel(f, gi, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            uint8_t *g = malloc(gw * gh);
            stbtt_MakeGlyphBitmapSubpixel(f, g, gw, gh, gw, sc, sc, fx, 0, gi);
            for (int yy = 0; yy < gh; yy++) {
                int ty = base + y0 + yy; if (ty < 0 || ty >= h) continue;
                for (int xx = 0; xx < gw; xx++) {
                    int tx = (int)x + x0 + xx; if (tx < 0 || tx >= w) continue;
                    int v = m[ty * w + tx] + g[yy * gw + xx]; m[ty * w + tx] = v > 255 ? 255 : v;
                }
            }
            free(g);
        }
        x += glyph_adv_px(f, gi, c, sc); prev = gi; pf = f;
    }
    *ow = w; *oh = h; *obase = base;
    return m;
}

/* ------------------------------------------------------------------ cache de glifos */
/* Para listas y texto editable: cada glifo (codigo, tamano) se rasteriza una vez y se
 * dibuja despues como mascara con el color que se pida. */
struct glyph { int cp, size, w, h, xo, yo, adv; uint8_t *bmp; };
#define GCACHE 2048
static struct glyph gcache[GCACHE];
static int gcache_n;
static struct glyph *glyph_get(int cp, int size)
{
    unsigned hsh = ((unsigned)cp * 2654435761u ^ (unsigned)size * 40503u) % GCACHE;
    for (int i = 0; i < GCACHE; i++) {
        struct glyph *g = &gcache[(hsh + i) % GCACHE];
        if (g->size == 0) {
            if (gcache_n > GCACHE * 3 / 4) {   /* llena: vaciar (raro) */
                for (int k = 0; k < GCACHE; k++) { free(gcache[k].bmp); gcache[k].bmp = 0; gcache[k].size = 0; }
                gcache_n = 0; return glyph_get(cp, size);
            }
            int gi; const stbtt_fontinfo *f = font_glyph(cp, &gi);
            float sc = font_scale(f, (float)size);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(f, gi, sc, sc, &x0, &y0, &x1, &y1);
            g->cp = cp; g->size = size; g->w = x1 - x0; g->h = y1 - y0; g->xo = x0; g->yo = y0;
            g->adv = (int)(glyph_adv_px(f, gi, cp, sc) * 64);           /* 26.6 */
            g->bmp = 0;
            if (g->w > 0 && g->h > 0) { g->bmp = malloc(g->w * g->h); stbtt_MakeGlyphBitmap(f, g->bmp, g->w, g->h, g->w, sc, sc, gi); }
            gcache_n++;
            return g;
        }
        if (g->cp == cp && g->size == size) return g;
    }
    return 0;
}
static int font_ascent(int size) { int a, d, l; stbtt_GetFontVMetrics(&font, &a, &d, &l); return (int)(a * stbtt_ScaleForPixelHeight(&font, (float)size) + .5f); }
/* ancho en px de los primeros n bytes (n < 0: todo) */
static int tw_n(const char *s, int size, int n)
{
    if (!font_ok) return 0;
    const char *e = n < 0 ? 0 : s + n; int w = 0, c;
    while ((!e || s < e) && (c = utf8_next(&s))) { struct glyph *g = glyph_get(c, size); if (g) w += g->adv; }
    return w >> 6;
}
/* dibuja texto con linea base en "base"; recorta a [clip_x0, clip_x1) y con "…" si no cabe en maxw */
static int draw_text(uint16_t *dst, int x, int base, const char *s, int size, uint16_t col, int maxw, int clip_y0, int clip_y1)
{
    if (!font_ok || !s) return 0;
    int full = tw_n(s, size, -1), ell = 0, ellw = 0;
    if (maxw > 0 && full > maxw) { ell = 1; struct glyph *g = glyph_get(0x2026, size); ellw = g ? g->adv >> 6 : 8; }
    int pen = x << 6, c;
    unsigned cr = col >> 11, cg = (col >> 5) & 63, cb = col & 31;
    while ((c = utf8_next(&s))) {
        struct glyph *g = glyph_get(c, size); if (!g) continue;
        if (ell && ((pen + g->adv) >> 6) - x > maxw - ellw) { c = 0x2026; g = glyph_get(c, size); if (!g) break; s = ""; }
        int gx = (pen >> 6) + g->xo, gy = base + g->yo;
        for (int yy = 0; yy < g->h && g->bmp; yy++) {
            int dy = gy + yy; if (dy < clip_y0 || dy >= clip_y1 || dy < 0 || dy >= H) continue;
            uint16_t *row = dst + dy * W;
            const uint8_t *m = g->bmp + yy * g->w;
            for (int xx = 0; xx < g->w; xx++) {
                int dx = gx + xx; if (dx < 0 || dx >= W) continue;
                unsigned a = m[xx]; if (!a) continue;
                unsigned d = row[dx], na = 255 - a;
                row[dx] = (uint16_t)(((cr * a + (d >> 11) * na) / 255) << 11 | (((cg * a + ((d >> 5) & 63) * na) / 255) << 5) | ((cb * a + (d & 31) * na) / 255));
            }
        }
        pen += g->adv;
        if (!*s && c == 0x2026) break;
    }
    return (pen >> 6) - x;
}

/* sprite de texto en color "col"; con sombra = sombra suave desplazada 1 px (estilo iOS) */
static int text_sprite(struct sprite *sp, const char *t, float px, uint16_t col, int shadow)
{
    int w, h, base;
    uint8_t *m = text_mask(t, px, &w, &h, &base);
    if (!m) { sp->w = sp->h = 0; return 0; }
    int pad = shadow ? 2 : 0, W2 = w + 2 * pad, H2 = h + 2 * pad;
    sp->w = W2; sp->h = H2;
    sp->rgb = calloc(W2 * H2, 2); sp->a = calloc(W2 * H2, 1);
    if (shadow) {   /* sombra: caja 3x3 de la mascara, desplazada 1 px abajo, 45 % */
        for (int y = 0; y < H2; y++) for (int x = 0; x < W2; x++) {
            int sum = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int sx = x - pad + dx, sy = y - pad - 1 + dy;
                if (sx >= 0 && sy >= 0 && sx < w && sy < h) sum += m[sy * w + sx];
            }
            sp->a[y * W2 + x] = (uint8_t)(sum / 9 * 115 / 255);
        }
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int a = m[y * w + x]; if (!a) continue;
        int i = (y + pad) * W2 + x + pad;
        /* texto sobre su sombra: mezcla del color con negro segun la cobertura */
        int sa = sp->a[i], na = a + sa * (255 - a) / 255;
        int k = na ? a * 255 / na : 255;             /* fraccion de color de texto */
        unsigned r = (col >> 11) * k / 255, g = ((col >> 5) & 63) * k / 255, b = (col & 31) * k / 255;
        sp->rgb[i] = (uint16_t)(r << 11 | g << 5 | b);
        sp->a[i] = (uint8_t)na;
    }
    free(m);
    return 1;
}
