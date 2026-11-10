/* keyboard.inc.c - teclado estilo iOS (oscuro), muy ligero: QWERTY espanol con n, capa de
 * numeros y simbolos, mayusculas, tildes con pulsacion larga, burbuja al pulsar.
 * Incluye un editor de una linea (renombrar, nueva carpeta) y el motor de edicion que
 * usa el editor de texto multilinea de Archivos. */

/* configuracion (Personalizar > Teclado): altura, fila de numeros y distribucion */
static int kb_cfg_height = 1, kb_cfg_numrow = 0, kb_cfg_layout = 0;   /* layout: 0 ES, 1 EN, 2 FR */
static int kb_kr_h(void) { return kb_cfg_height == 0 ? 36 : kb_cfg_height == 2 ? 48 : 42; }
static int kb_rows(void) { return 4 + (kb_cfg_numrow ? 1 : 0); }
#define KR_H (kb_kr_h())
#define KB_H (kb_rows() * (KR_H + 8) + 6)
#define KB_Y (H - KB_H)
static int kb_row_y(int r) { return KB_Y + 8 + r * (KR_H + 8); }

struct key { int x, y, w, h; const char *lbl; int code; };   /* code: >0 caracter, <0 especial */
enum { K_SHIFT = -1, K_BACK = -2, K_LAYER = -3, K_SPACE = -4, K_ENTER = -5 };
static struct key kbk[64]; static int nkeys;
static int kb_layer, kb_shift;            /* 0 letras, 1 numeros */

static const char *sym_shift = "⇧", *sym_back = "⌫";
static void kb_symbols(void)
{
    if (!stbtt_FindGlyphIndex(&font, 0x21E7)) sym_shift = "↑";
    if (!stbtt_FindGlyphIndex(&font, 0x232B)) sym_back = "←";
}
static void kb_row_keys(const char *row, int y, int x_left, int x_right)
{
    static char lbls[64][5];
    int n = 0; const char *s = row; while (utf8_next(&s)) n++;
    int avail = x_right - x_left, gap = 4, kw = (avail - (n - 1) * gap) / n; if (kw > 30) kw = 30;
    int tot = n * kw + (n - 1) * gap, x0 = x_left + (avail - tot) / 2;
    s = row;
    for (int i = 0; i < n && nkeys < 64; i++) {
        const char *st0 = s; int c = utf8_next(&s);
        int k = nkeys, len = (int)(s - st0); memcpy(lbls[k], st0, len); lbls[k][len] = 0;
        kbk[nkeys++] = (struct key){ x0 + i * (kw + gap), y, kw, KR_H, lbls[k], c };
    }
}
static void kb_layout(const char *enter_label)
{
    static int once; if (!once) { once = 1; kb_symbols(); }
    static const char *LET[3][3] = {
        { "qwertyuiop", "asdfghjklñ", "zxcvbnm" },        /* espanol */
        { "qwertyuiop", "asdfghjkl", "zxcvbnm" },         /* ingles */
        { "azertyuiop", "qsdfghjklm", "wxcvbn'" },        /* frances (AZERTY) */
    };
    static const char *SYM[3] = { "1234567890", "-/:;()€&@\"", ".,?!'_" };
    const char **rows = kb_layer ? SYM : LET[kb_cfg_layout < 0 || kb_cfg_layout > 2 ? 0 : kb_cfg_layout];
    nkeys = 0;
    int r0 = 0;
    if (kb_cfg_numrow && !kb_layer) { kb_row_keys("1234567890", kb_row_y(0), 3, W - 3); r0 = 1; }
    kb_row_keys(rows[0], kb_row_y(r0), 3, W - 3);
    kb_row_keys(rows[1], kb_row_y(r0 + 1), 3, W - 3);
    kbk[nkeys++] = (struct key){ 3, kb_row_y(r0 + 2), 42, KR_H, kb_layer ? "#+=" : sym_shift, K_SHIFT };
    kb_row_keys(rows[2], kb_row_y(r0 + 2), 49, W - 49);
    kbk[nkeys++] = (struct key){ W - 45, kb_row_y(r0 + 2), 42, KR_H, sym_back, K_BACK };
    kbk[nkeys++] = (struct key){ 3, kb_row_y(r0 + 3), 64, KR_H, kb_layer ? "ABC" : "123", K_LAYER };
    kbk[nkeys++] = (struct key){ 71, kb_row_y(r0 + 3), 166, KR_H, ui_lang == LANG_EN ? "space" : ui_lang == LANG_FR ? "espace" : "espacio", K_SPACE };
    kbk[nkeys++] = (struct key){ 241, kb_row_y(r0 + 3), 76, KR_H, enter_label, K_ENTER };
}

static int kb_hit(int x, int y)
{
    if (y < KB_Y) return -1;
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < nkeys; i++) {   /* la tecla mas cercana (sin huecos muertos) */
        struct key *k = &kbk[i];
        int dx = x < k->x ? k->x - x : x >= k->x + k->w ? x - k->x - k->w + 1 : 0;
        int dy = y < k->y ? k->y - y : y >= k->y + k->h ? y - k->y - k->h + 1 : 0;
        int d = dx * dx + dy * dy * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void put_utf8(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; o[1] = 0; }
    else if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); o[2] = 0; }
    else { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); o[3] = 0; }
}
static int up_case(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c == 0xF1) return 0xD1;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    return c;
}
static int accent_of(int c)
{
    switch (c) { case 'a': return 0xE1; case 'e': return 0xE9; case 'i': return 0xED; case 'o': return 0xF3;
                 case 'u': return 0xFA; case 'n': return 0xF1; case 'c': return 0xE7; case '?': return 0xBF; case '!': return 0xA1; }
    return 0;
}

static void kb_draw(uint16_t *dst, int pressed)
{
    for (int y = KB_Y; y < H; y++) for (int x = 0; x < W; x++) dst[y * W + x] = rgb(32, 32, 34);
    for (int i = 0; i < nkeys; i++) {
        struct key *k = &kbk[i];
        int special = k->code < 0 && k->code != K_SPACE;
        uint16_t bg = special ? rgb(72, 72, 76) : rgb(108, 108, 112);
        if (k->code == K_SHIFT && kb_shift) bg = rgb(235, 235, 240);
        if (k->code == K_ENTER) bg = rgb(10, 132, 255);
        if (i == pressed && (special || k->code == K_SPACE)) bg = rgb(150, 150, 156);
        fill_rrect(dst, k->x, k->y + 1, k->w, k->h, 6, rgb(0, 0, 0), 120);   /* sombra */
        fill_rrect(dst, k->x, k->y, k->w, k->h, 6, bg, 255);
        char lb[8];
        const char *l = k->lbl;
        if (k->code > 0 && kb_shift && !kb_layer) { put_utf8(lb, up_case(k->code)); l = lb; }
        int size = k->code > 0 ? 20 : (k->code == K_SPACE ? 14 : 15);
        uint16_t fg = (k->code == K_SHIFT && kb_shift) ? rgb(20, 20, 22) : 0xFFFF;
        int w = tw_n(l, size, -1);
        draw_text(dst, k->x + (k->w - w) / 2, k->y + k->h / 2 + size * 7 / 20, l, size, fg, 0, 0, H);
    }
    if (pressed >= 0 && kbk[pressed].code > 0) {   /* burbuja */
        struct key *k = &kbk[pressed];
        int bw = 44, bh = 56, bx = k->x + k->w / 2 - bw / 2, by = k->y - bh + 4;
        if (bx < 1) bx = 1; if (bx + bw > W - 1) bx = W - 1 - bw;
        fill_rrect(dst, bx, by + 2, bw, bh, 10, 0, 140);
        fill_rrect(dst, bx, by, bw, bh, 10, rgb(130, 130, 136), 255);
        char lb[8]; put_utf8(lb, kb_shift && !kb_layer ? up_case(k->code) : k->code);
        int w = tw_n(lb, 28, -1);
        draw_text(dst, bx + (bw - w) / 2, by + 36, lb, 28, 0xFFFF, 0, 0, H);
    }
}

/* ------------------------------------------------------------------ motor de edicion */
struct ebuf { char *b; int len, cap, cur; };
static void eb_insert(struct ebuf *e, const char *s)
{
    int n = (int)strlen(s);
    if (e->len + n >= e->cap) return;
    memmove(e->b + e->cur + n, e->b + e->cur, e->len - e->cur + 1);
    memcpy(e->b + e->cur, s, n); e->len += n; e->cur += n;
}
static void eb_back(struct ebuf *e)
{
    if (e->cur <= 0) return;
    int p = e->cur - 1;
    while (p > 0 && ((unsigned char)e->b[p] & 0xC0) == 0x80) p--;
    memmove(e->b + p, e->b + e->cur, e->len - e->cur + 1);
    e->len -= e->cur - p; e->cur = p;
}

/* procesa un toque completo en el teclado; devuelve K_ENTER si se pulso intro, 0 si no */
static int kb_touch(struct pkt *p, struct ebuf *e, uint16_t *dst, void (*redraw)(uint16_t *, int), int multiline)
{
    int k = kb_hit(p->x, p->y);
    if (k < 0) { while (cur_down && read_pkt(p, 3000)) {} return 0; }
    double t0 = now_ms(); int accented = 0;
    redraw(dst, k); present();
    double rep = 0;
    while (cur_down) {
        if (read_pkt(p, 30) && !p->down) break;
        int nk = kb_hit(cur_x, cur_y);
        if (nk != k && nk >= 0) { k = nk; t0 = now_ms(); redraw(dst, k); present(); }
        double t = now_ms();
        if (kbk[k].code > 0 && !accented && t - t0 > 450 && accent_of(kbk[k].code)) {   /* tilde */
            char o[8]; int c = accent_of(kbk[k].code); put_utf8(o, kb_shift ? up_case(c) : c);
            eb_insert(e, o); accented = 1; kb_shift = 0; redraw(dst, -1); present();
        }
        if (kbk[k].code == K_BACK && t - t0 > 450 && t - rep > 70) { eb_back(e); rep = t; redraw(dst, k); present(); }
    }
    int code = kbk[k].code, r = 0;
    if (!accented) {
        if (code > 0) { char o[8]; put_utf8(o, kb_shift && !kb_layer ? up_case(code) : code); eb_insert(e, o); kb_shift = 0; }
        else if (code == K_BACK) { if (now_ms() - t0 <= 450) eb_back(e); }
        else if (code == K_SPACE) eb_insert(e, " ");
        else if (code == K_SHIFT) { if (kb_layer) {} else kb_shift = !kb_shift; }
        else if (code == K_LAYER) { kb_layer = !kb_layer; kb_shift = 0; kb_layout(kbk[nkeys - 1].lbl); }
        else if (code == K_ENTER) { if (multiline) eb_insert(e, "\n"); else r = K_ENTER; }
    }
    redraw(dst, -1); present();
    return r;
}

/* ------------------------------------------------------------------ editor de una linea */
static const char *le_title, *le_hint;
static struct ebuf *le_buf;
static void le_redraw(uint16_t *dst, int pressed)
{
    for (int i = 0; i < W * KB_Y; i++) dst[i] = rgb(0, 0, 0);
    memcpy(dst, fr1, 30 * W * 2);
    int tw = tw_n(le_title, 17, -1);
    draw_text(dst, (W - tw) / 2, 64, le_title, 17, 0xFFFF, 0, 0, H);
    draw_text(dst, 14, 64, tr("Cancelar"), 15, rgb(10, 132, 255), 0, 0, H);
    fill_rrect(dst, 12, 96, W - 24, 44, 10, rgb(44, 44, 48), 255);
    int base = 124;
    int w = draw_text(dst, 24, base, le_buf->b, 16, 0xFFFF, 0, 0, H);
    if (!le_buf->len && le_hint) draw_text(dst, 24, base, le_hint, 16, rgb(110, 110, 116), 0, 0, H);
    int cx = 24 + tw_n(le_buf->b, 16, le_buf->cur); (void)w;
    for (int y = 106; y < 132; y++) for (int x = cx; x < cx + 2 && x < W; x++) dst[y * W + x] = rgb(10, 132, 255);
    kb_draw(dst, pressed);
}
/* devuelve 1 si se acepto; el texto queda en buf */
static int line_edit(const char *title, const char *hint, char *buf, int cap)
{
    struct ebuf e = { buf, (int)strlen(buf), cap, (int)strlen(buf) };
    le_title = title; le_hint = hint; le_buf = &e;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Aceptar"));
    le_redraw(fr0, -1); present();
    struct pkt p;
    for (;;) {
        if (!read_pkt(&p, 300) || !p.down) continue;
        if (p.y >= KB_Y) { if (kb_touch(&p, &e, fr0, le_redraw, 0) == K_ENTER) return e.len > 0; continue; }
        int x = p.x, y = p.y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 90 && x < 100) return 0;                        /* Cancelar */
        if (y >= 96 && y < 140) {                                /* colocar el cursor */
            int best = 0, bd = 1 << 30;
            for (int i = 0; i <= e.len; i++) {
                if (i < e.len && ((unsigned char)buf[i] & 0xC0) == 0x80) continue;
                int d = abs(24 + tw_n(buf, 16, i) - x); if (d < bd) { bd = d; best = i; }
            }
            e.cur = best; le_redraw(fr0, -1); present();
        }
    }
}
