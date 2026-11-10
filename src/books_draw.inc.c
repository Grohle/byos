/* books_draw.inc.c - dibujo del texto de una pagina de Libros (RGB565, paso W). Solo depende de
 * text.inc.c (cache de glifos) y books_core.inc.c, para que tests/books_test.c pinte paginas reales
 * en PC (PNG) con el mismo codigo que el aparato. */

static inline uint32_t bk_sp(uint16_t p) { return (p | ((uint32_t)p << 16)) & 0x07E0F81Fu; }
static inline uint16_t bk_mix(uint16_t c, uint16_t d, unsigned a5)   /* a5 0..32 */
{
    uint32_t cs = bk_sp(c), ds = bk_sp(d);
    uint32_t r = (ds + (((cs - ds) * a5) >> 5)) & 0x07E0F81Fu;
    return (uint16_t)(r | r >> 16);
}
/* La fuente principal (default.otf) dibuja las comillas tipograficas a ancho completo (estilo CJK):
 * bien en chino o japones, pero en un libro latino dejan huecos enormes. En esos libros se toman
 * de las de reserva Korean.ttf / Thai.ttf (comillas proporcionales; msyh tambien las tiene anchas). bk_latinq se fija al abrir el libro
 * (antes de arrancar el hilo de paginacion) y las caches de avances se vacian. */
static int bk_latinq;
static const stbtt_fontinfo *bk_qfont(int cp)
{
    if (!bk_latinq || !(cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D)) return 0;
    if (!font_fb[FONT_NFB - 1].st) { int gi; font_glyph(0x10FFFD, &gi); }
    for (int i = 1; i < FONT_NFB; i++)    /* Korean.ttf, Thai.ttf: comillas proporcionales (msyh no) */
        if (font_fb[i].st > 0 && stbtt_FindGlyphIndex(&font_fb[i].fi, cp)) return &font_fb[i].fi;
    return 0;
}
static int bk_adv_calc(int cp, int size)
{
    if (cp == 0xA0) cp = ' ';
    const stbtt_fontinfo *q = bk_qfont(cp);
    if (q) { int adv, lsb; stbtt_GetGlyphHMetrics(q, stbtt_FindGlyphIndex(q, cp), &adv, &lsb); return (int)(adv * font_scale(q, (float)size) * 64); }
    int gi; const stbtt_fontinfo *f = font_glyph(cp, &gi);
    return (int)(glyph_adv_px(f, gi, cp, font_scale(f, (float)size)) * 64);
}
static struct bk_advc bk_uiac;                 /* avances del hilo de la interfaz */
static int bk_adv_ui(void *c, int cp, int size) { (void)c; return bk_adv_cached(&bk_uiac, cp, size, bk_adv_calc); }
/* glifos de las comillas proporcionales (pocos: 4 codigos x tamanos) */
static struct glyph bk_qg[24]; static int bk_qn;
static struct glyph *bk_qglyph(int cp, int size)
{
    const stbtt_fontinfo *q = bk_qfont(cp); if (!q) return 0;
    for (int i = 0; i < bk_qn; i++) if (bk_qg[i].cp == cp && bk_qg[i].size == size) return &bk_qg[i];
    if (bk_qn == 24) { for (int i = 0; i < 24; i++) free(bk_qg[i].bmp); bk_qn = 0; }
    struct glyph *g = &bk_qg[bk_qn++]; int gi = stbtt_FindGlyphIndex(q, cp); float sc = font_scale(q, (float)size);
    int x0, y0, x1, y1; stbtt_GetGlyphBitmapBox(q, gi, sc, sc, &x0, &y0, &x1, &y1);
    g->cp = cp; g->size = size; g->w = x1 - x0; g->h = y1 - y0; g->xo = x0; g->yo = y0; g->adv = bk_adv_calc(cp, size); g->bmp = 0;
    if (g->w > 0 && g->h > 0 && (g->bmp = malloc(g->w * g->h))) stbtt_MakeGlyphBitmap(q, g->bmp, g->w, g->h, g->w, sc, sc, gi);
    return g;
}

/* glifo con cursiva sintetica (inclinacion 0,2) y negrita (doble pasada) */
static void bk_glyph(uint16_t *d, int x, int base, int cp, int size, uint16_t col, int style, int y0, int y1)
{
    struct glyph *g = bk_qglyph(cp, size); if (!g) g = glyph_get(cp, size);
    if (!g || !g->bmp) return;
    int gx = x + g->xo, gy = base + g->yo;
    for (int yy = 0; yy < g->h; yy++) {
        int dy = gy + yy; if (dy < y0 || dy >= y1 || dy < 0 || dy >= H) continue;
        int sh = style & 1 ? ((base - dy) * 13) >> 6 : 0;
        uint16_t *row = d + dy * W; const uint8_t *m = g->bmp + yy * g->w;
        for (int xx = 0; xx < g->w; xx++) {
            unsigned a = m[xx]; if (!a) continue;
            int dx = gx + xx + sh; if (dx < 0 || dx >= W) continue;
            row[dx] = bk_mix(col, row[dx], (a + 4) >> 3);
            if ((style & 2) && dx + 1 < W) row[dx + 1] = bk_mix(col, row[dx + 1], (a + 4) >> 3);
        }
    }
}
/* texto de una sola linea con el motor del libro (sin elipsis); y1 = limite inferior */
static int bk_text_clip(uint16_t *d, int x, int base, const char *s, int size, uint16_t col, int style, int maxw, int y1)
{
    int pen = x << 6, c; const char *p = s;
    while ((c = utf8_next(&p))) {
        if (c < 0x20 || bk_zw(c)) continue;
        int a = bk_adv_ui(0, c, size);
        if (maxw > 0 && ((pen + a) >> 6) - x > maxw) break;
        if (c != ' ' && c != 0xA0 && base > -50) bk_glyph(d, pen >> 6, base, c, size, col, style, 0, y1);
        pen += a;
    }
    return (pen >> 6) - x;
}
static int bk_text(uint16_t *d, int x, int base, const char *s, int size, uint16_t col, int style, int maxw) { return bk_text_clip(d, x, base, s, size, col, style, maxw, H); }
static int bk_text_w(const char *s, int size) { return bk_text_clip(0, 0, -100, s, size, 0, 0, 0, 0); }

/* texto de la pagina que empieza en "start" del flujo f (len), con la maquetacion P, en el area
 * (ml, top); separadores como tres puntos. Devuelve el inicio de la pagina siguiente. */
static int bk_draw_text_page(uint16_t *d, const struct bk_lp *P, const char *f, int len, int start, int ml, int top, uint16_t fg, uint16_t dim)
{
    struct bk_line ln[64]; int nx;
    int nl = bk_layout_page(P, f, len, start, bk_adv_ui, 0, ln, 64, &nx);
    const unsigned char *u = (const unsigned char *)f;
    int st = bk_style_at(f, start);
    for (int k = 0; k < nl; k++) {
        struct bk_line *L = &ln[k];
        int size = L->size, lh = L->kind == 1 ? P->hlh : P->lh;
        int base = top + L->y + (lh - size) / 2 + font_ascent(size);
        if (L->kind == 2) {
            for (int i = -1; i <= 1; i++) for (int yy = -2; yy <= 2; yy++) for (int xx = -2; xx <= 2; xx++) {
                int r2 = xx * xx + yy * yy; if (r2 > 5) continue;
                int x = W / 2 + i * 14 + xx, y = top + L->y + lh / 2 + yy;
                if (x >= 0 && x < W && y >= 0 && y < H) d[y * W + x] = bk_mix(dim, d[y * W + x], r2 > 3 ? 14 : 32);
            }
            continue;
        }
        if (L->a == 0 || u[L->a - 1] == '\n') st = 0;
        int pen = (ml << 6) + L->x26, prev_sp = 0, sty = st | (L->kind == 1 ? 2 : 0);
        for (int i = L->a; i < L->b; ) {
            int c = u[i];
            if (c < 0x20) {
                if (c == BKC_I_ON) st |= 1; else if (c == BKC_I_OFF) st &= ~1; else if (c == BKC_B_ON) st |= 2; else if (c == BKC_B_OFF) st &= ~2;
                sty = st | (L->kind == 1 ? 2 : 0); i++; continue;
            }
            int l; int cp = bk_u8dec(u + i, u + len, &l); i += l;
            if (bk_zw(cp)) continue;
            if (cp == ' ') { if (!prev_sp && i - l > L->a) pen += bk_adv_ui(0, ' ', size) + L->sp26; prev_sp = 1; continue; }
            prev_sp = 0;
            if (cp != 0xA0) bk_glyph(d, pen >> 6, base, cp, size, fg, sty, top - 8, top + P->h + 8);
            pen += bk_adv_ui(0, cp, size);
        }
        /* estilo para la linea siguiente (codigos entre el final y la linea siguiente) */
        int nxa = k + 1 < nl ? ln[k + 1].a : nx;
        for (int i = L->b; i < nxa && i < len; i++) {
            int c = u[i];
            if (c == BKC_I_ON) st |= 1; else if (c == BKC_I_OFF) st &= ~1; else if (c == BKC_B_ON) st |= 2; else if (c == BKC_B_OFF) st &= ~2;
            else if (c == '\n') st = 0;
        }
    }
    return nx;
}
