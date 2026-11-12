/* settings_app.inc.c - "Ajustes": la app de ajustes de ByOS con listas agrupadas estilo iOS.
 * Sustituye a los Ajustes de sistema y al "DAC USB" de HiBy (las paginas del sistema estan en
 * sysset.inc.c, incluido al final). Paginas propias: General (idioma), Inicio, Gestos (con
 * animacion de ejemplo), Teclado, Desarrollador (ADB) y Acerca de. Navegacion con transicion
 * de iOS y "atras" deslizando desde el borde. Todo el texto pasa por tr() (13 idiomas). */

static int cc_tiles[10], cc_ntiles;   /* definidos en cc.inc.c */
enum { R_NAV, R_TOGGLE, R_CHOICE, R_ACTION, R_INFO, R_CHECK,   /* R_CHECK: opcion con marca; nopts = su valor */
       R_SLIDER,                       /* deslizador: *ival entre lo y hi; nopts = estilo (SL_*) */
       R_BAR };                        /* barra de ocupacion (almacenamiento): *ival en milesimas */
enum { SL_PLAIN, SL_BALANCE, SL_BRIGHT };
struct srow {
    const char *title; int type;
    int *ival;                         /* TOGGLE / CHOICE / SLIDER / BAR */
    const char *const *opts; int nopts;
    void (*fn)(void);                  /* NAV: abre pagina; ACTION: ejecuta; CHOICE/TOGGLE/SLIDER: al cambiar */
    const char *(*val)(void);          /* INFO / valor a la derecha */
    int gesture;                       /* CHOICE de gesto: borde (para la animacion) */
    int lo, hi;                        /* SLIDER: rango */
    int dis;                           /* atenuada: no responde (aviso breve) */
    int icon;                          /* icono de color a la izquierda (IC_*), 0 = sin icono */
};
struct ssec { const char *header, *footer; struct srow rows[8]; int n; };
struct spage { const char *title; struct ssec secs[6]; int nsec; int hero; void (*hero_fn)(uint16_t *d, int y, int h); };   /* hero: alto de la cabecera (logo, o hero_fn la dibuja) */
static int wrap_cw;                   /* > 0: draw_wrapped centra cada linea en ese ancho (alertas) */

#define NAVH 64           /* barra de navegacion (bajo la barra de estado) */
#define ROWH 44
static float sp_scroll;

/* chino/japones: se puede partir entre dos caracteres cualesquiera */
static int wrap_cjk(int c) { return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF); }
/* signos que no empiezan linea (se pegan a lo anterior) */
static int wrap_nostart(int c)
{
    static const int k[] = { 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F, 0xFF09, 0x300D, 0x300F, 0x3011, 0x3009, 0x300B,
                             0x201D, 0x2019, 0x30FC, 0x2026, 0x22EF, 0x30FB, ',', '.', ':', ';', '!', '?', ')', 0 };
    for (int i = 0; k[i]; i++) if (c == k[i]) return 1;
    return 0;
}
static void wrap_emit(uint16_t *dst, int x, int *yy, const char *a, const char *b, int size, uint16_t col, int lh, int draw)
{
    if (draw) {
        char line[512]; int n = (int)(b - a); if (n > (int)sizeof line - 1) n = sizeof line - 1;
        while (n > 0 && a[n - 1] == ' ') n--;
        memcpy(line, a, n); line[n] = 0;
        if (wrap_cw > 0) x += (wrap_cw - tw_n(line, size, -1)) / 2;
        draw_text(dst, x, *yy + size, line, size, col, 0, 0, H);
    }
    *yy += lh;
}
static int draw_wrapped(uint16_t *dst, int x, int y, int w, const char *s, int size, uint16_t col, int lh, int draw)
{
    /* parte en lineas sin pasarse de w: en los espacios y, en chino/japones, entre caracteres;
     * un trozo sin espacios mas ancho que w (tailandes, rutas) se parte por caracteres sin
     * separar las marcas combinantes. Devuelve la altura usada */
    int yy = y; const char *ls = 0, *le = 0, *p = s;      /* linea en curso: [ls, le) del original */
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *ts = p, *q = p; int c = utf8_next(&q);
        p = q;
        if (!wrap_cjk(c)) while (*p && *p != ' ') { const char *r = p; int d = utf8_next(&r); if (wrap_cjk(d) && !wrap_nostart(d)) break; p = r; }
        while (*p) { const char *r = p; int d = utf8_next(&r); if (!wrap_nostart(d) && !text_is_mark(d)) break; p = r; }
        if (!ls) { ls = ts; le = p; }
        else if (tw_n(ls, size, (int)(p - ls)) <= w) le = p;
        else { wrap_emit(dst, x, &yy, ls, le, size, col, lh, draw); ls = ts; le = p; }
        while (tw_n(ls, size, (int)(le - ls)) > w) {      /* trozo solo que no cabe: por caracteres */
            const char *cut = ls, *r = ls;
            while (r < le) {
                const char *r2 = r; utf8_next(&r2);
                while (r2 < le) { const char *r3 = r2; int d = utf8_next(&r3); if (!text_is_mark(d)) break; r2 = r3; }
                if (cut > ls && tw_n(ls, size, (int)(r2 - ls)) > w) break;
                cut = r = r2;
            }
            if (cut >= le) break;
            wrap_emit(dst, x, &yy, ls, cut, size, col, lh, draw); ls = cut;
        }
    }
    if (ls) wrap_emit(dst, x, &yy, ls, le, size, col, lh, draw);
    return yy - y;
}

/* ------------------------------------------------------------------ interruptor con muelle (iOS)
 * Cada interruptor tiene su estado de animacion, identificado por la variable que muestra: al
 * cambiar el valor (toque, o el reproductor lo revierte) el boton se desliza con un muelle.
 * Un estado sin dibujar 1,5 s se descarta (al volver a una pagina no se anima lo viejo). */
struct swanim { const int *iv; float x, v, tg; double t; };
static struct swanim sw_a[24];
static float sw_pos(const int *iv)
{
    double now = now_ms(); float tg = *iv ? 1.f : 0.f;
    struct swanim *e = 0, *lru = &sw_a[0];
    for (int i = 0; i < 24; i++) { if (sw_a[i].iv == iv) { e = &sw_a[i]; break; } if (sw_a[i].t < lru->t) lru = &sw_a[i]; }
    if (!e || now - e->t > 1500) { if (!e) { e = lru; e->iv = iv; } e->x = e->tg = tg; e->v = 0; e->t = now; return tg; }
    float dt = (float)(now - e->t) / 1000.f; e->t = now; e->tg = tg; if (dt > 0.05f) dt = 0.05f;
    /* respuesta 0,28 s, amortiguamiento 0,82: llega en ~250 ms con un rebote apenas visible */
    const float k = 503.6f, c = 36.8f;
    for (float s = 0; s < dt; s += 0.004f) { float h = dt - s < 0.004f ? dt - s : 0.004f; e->v += (k * (tg - e->x) - c * e->v) * h; e->x += e->v * h; }
    if (fabsf(tg - e->x) < 0.002f && fabsf(e->v) < 0.02f) { e->x = tg; e->v = 0; }
    return e->x;
}
static int sw_busy(void)
{
    double now = now_ms();
    for (int i = 0; i < 24; i++) if (sw_a[i].iv && now - sw_a[i].t < 1500 && (sw_a[i].x != sw_a[i].tg || sw_a[i].v != 0)) return 1;
    return 0;
}
static uint16_t sp_mix(uint16_t a, uint16_t b, float k)    /* k 0 = a, 1 = b */
{
    if (k <= 0) return a; if (k >= 1) return b;
    int r = (int)((a >> 11) + ((b >> 11) - (a >> 11)) * k), g = (int)(((a >> 5) & 63) + (((b >> 5) & 63) - ((a >> 5) & 63)) * k), bl = (int)((a & 31) + ((b & 31) - (a & 31)) * k);
    return (uint16_t)(r << 11 | g << 5 | bl);
}
static void sp_switch(uint16_t *d, int x, int y, float k, int dim)
{
    float kk = k < -0.03f ? -0.03f : k > 1.03f ? 1.03f : k, kc = k < 0 ? 0 : k > 1 ? 1 : k;
    int al = dim ? 110 : 255;
    fill_rrect(d, x, y, 51, 31, 15.5f, sp_mix(rgb(57, 57, 61), rgb(52, 199, 89), kc), al);
    float cx = x + 15.5f + 20.f * kk;
    fill_circle(d, cx, y + 16.5f, 13.5f, 0, dim ? 20 : 60);           /* sombra */
    fill_circle(d, cx, y + 15.5f, 13.5f, dim ? rgb(150, 150, 154) : 0xFFFF, 255);
}

/* ------------------------------------------------------------------ deslizador y barra */
#define SL_X0 58
#define SL_X1 (W - 58)
static void sp_sun(uint16_t *d, float cx, float cy, float r, uint16_t c)
{
    fill_circle(d, cx, cy, r, c, 255);
    for (int i = 0; i < 8; i++) { float a = i * 0.785398f; fill_circle(d, cx + cosf(a) * (r + 3.2f), cy + sinf(a) * (r + 3.2f), r > 4 ? 1.2f : 0.9f, c, 255); }
}
static void sp_slider(uint16_t *d, const struct srow *rw, int ry)
{
    int lo = rw->lo, hi = rw->hi > rw->lo ? rw->hi : rw->lo + 1, v = *rw->ival; if (v < lo) v = lo; if (v > hi) v = hi;
    float f = (v - lo) / (float)(hi - lo), kx = SL_X0 + f * (SL_X1 - SL_X0), cy = ry + 22;
    uint16_t blue = rw->dis ? rgb(60, 80, 110) : rgb(10, 132, 255), grey = rgb(142, 142, 147);
    fill_rrect(d, SL_X0, (int)cy - 2, SL_X1 - SL_X0, 4, 2, rgb(72, 72, 76), 255);
    if (rw->nopts == SL_BALANCE) {
        int mx = (SL_X0 + SL_X1) / 2;
        fill_rrect(d, mx - 1, (int)cy - 7, 2, 14, 1, rgb(99, 99, 102), 255);          /* centro */
        int a = (int)kx < mx ? (int)kx : mx, b = (int)kx < mx ? mx : (int)kx;
        if (b > a) fill_rrect(d, a, (int)cy - 2, b - a, 4, 2, blue, 255);
        const char *l = tr("BAL_L"), *r = tr("BAL_R");
        draw_text(d, 37 - tw_n(l, 13, -1) / 2, ry + 27, l, 13, grey, 0, NAVH + 30, H);
        draw_text(d, W - 37 - tw_n(r, 13, -1) / 2, ry + 27, r, 13, grey, 0, NAVH + 30, H);
    } else {
        if (kx > SL_X0) fill_rrect(d, SL_X0, (int)cy - 2, (int)kx - SL_X0 + 2, 4, 2, blue, 255);
        if (rw->nopts == SL_BRIGHT) { sp_sun(d, 37, cy, 2.5f, grey); sp_sun(d, W - 37, cy, 4.5f, grey); }
    }
    fill_circle(d, kx, cy + 1.5f, 14, 0, 70);
    fill_circle(d, kx, cy, 13.5f, rw->dis ? rgb(150, 150, 154) : 0xFFFF, 255);
}
static int sl_value(const struct srow *rw, int v0, int dx)      /* arrastre relativo (como iOS) */
{
    int hi = rw->hi > rw->lo ? rw->hi : rw->lo + 1;
    float v = v0 + dx * (float)(hi - rw->lo) / (SL_X1 - SL_X0);
    int iv = (int)floorf(v + 0.5f); return iv < rw->lo ? rw->lo : iv > hi ? hi : iv;
}

/* ------------------------------------------------------------------ iconos de las filas (iOS) */
enum { IC_NONE, IC_WIFI, IC_BT, IC_STREAM, IC_USB, IC_GENERAL, IC_DISPLAY, IC_SOUND, IC_BATTERY, IC_LIBRARY,
       IC_HOME, IC_GESTURE, IC_CC, IC_KB, IC_DEV, IC_N };
static struct sprite ic_spr[IC_N]; static int ic_loaded;
static void ic_load(void)
{
    static const struct { int ic; const char *n; } m[] = { { IC_WIFI, "wifi" }, { IC_USB, "usb" }, { IC_GENERAL, "gear" },
        { IC_DISPLAY, "sun" }, { IC_BATTERY, "bolt" }, { IC_CC, "sliders" } };
    ic_loaded = 1;
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
        char n[32]; snprintf(n, sizeof n, "cc_%s", m[i].n);
        struct sprite s = { 0 };
        if (!load_sprite(n, &s) || s.w < 4) { free(s.rgb); free(s.a); continue; }
        /* 26 px -> 19 px con media por area (se ve fino, no a saltos) */
        const int o = 19; struct sprite *t = &ic_spr[m[i].ic];
        t->rgb = malloc(o * o * 2); t->a = malloc(o * o);
        if (t->rgb && t->a) {
            t->w = t->h = o;
            for (int y = 0; y < o; y++) for (int x = 0; x < o; x++) {
                int y0 = y * s.h / o, y1 = (y + 1) * s.h / o, x0 = x * s.w / o, x1 = (x + 1) * s.w / o, acc = 0, n2 = 0;
                if (y1 <= y0) y1 = y0 + 1; if (x1 <= x0) x1 = x0 + 1;
                for (int yy = y0; yy < y1 && yy < s.h; yy++) for (int xx = x0; xx < x1 && xx < s.w; xx++) { acc += s.a[yy * s.w + xx]; n2++; }
                t->rgb[y * o + x] = 0xFFFF; t->a[y * o + x] = (uint8_t)(n2 ? acc / n2 : 0);
            }
        } else { free(t->rgb); free(t->a); memset(t, 0, sizeof *t); }
        free(s.rgb); free(s.a);
    }
}
static void sp_line(uint16_t *d, float x0, float y0, float x1, float y1, float r, uint16_t c)
{
    float dx = x1 - x0, dy = y1 - y0; int n = (int)(sqrtf(dx * dx + dy * dy) / 0.9f) + 1;
    for (int i = 0; i <= n; i++) fill_circle(d, x0 + dx * i / n, y0 + dy * i / n, r, c, 255);
}
static void sp_icon(uint16_t *d, int x, int y, int ic)
{
    static const uint8_t col[IC_N][3] = { { 0, 0, 0 }, { 10, 132, 255 }, { 10, 132, 255 }, { 52, 199, 89 }, { 88, 86, 214 }, { 142, 142, 147 },
        { 10, 132, 255 }, { 255, 45, 85 }, { 52, 199, 89 }, { 252, 61, 57 }, { 88, 86, 214 }, { 255, 149, 0 }, { 142, 142, 147 },
        { 120, 120, 126 }, { 99, 99, 102 } };
    if (ic <= 0 || ic >= IC_N) return;
    if (!ic_loaded) ic_load();
    uint16_t bg = rgb(col[ic][0], col[ic][1], col[ic][2]), w = 0xFFFF;
    fill_rrect(d, x, y, 29, 29, 7, bg, 255);
    float cx = x + 14.5f, cy = y + 14.5f;
    if (ic_spr[ic].w) { blit(d, &ic_spr[ic], x + 5, y + 5, 32, 255); return; }
    switch (ic) {
    case IC_BT:
        sp_line(d, cx - 4, cy - 4, cx + 4, cy + 4, 0.9f, w); sp_line(d, cx + 4, cy + 4, cx, cy + 8, 0.9f, w);
        sp_line(d, cx, cy + 8, cx, cy - 8, 0.9f, w); sp_line(d, cx, cy - 8, cx + 4, cy - 4, 0.9f, w);
        sp_line(d, cx + 4, cy - 4, cx - 4, cy + 4, 0.9f, w); break;
    case IC_STREAM:                                            /* pantalla con triangulo (AirPlay) */
        sp_line(d, cx - 4, cy + 3, cx - 8, cy + 3, 0.9f, w); sp_line(d, cx - 8, cy + 3, cx - 8, cy - 7, 0.9f, w);
        sp_line(d, cx - 8, cy - 7, cx + 8, cy - 7, 0.9f, w); sp_line(d, cx + 8, cy - 7, cx + 8, cy + 3, 0.9f, w);
        sp_line(d, cx + 8, cy + 3, cx + 4, cy + 3, 0.9f, w);
        for (int k = 0; k < 7; k++) fill_rrect(d, (int)cx - k, (int)cy + 1 + k, 2 * k + 1, 1, 0, w, 255);
        break;
    case IC_SOUND:                                             /* altavoz con ondas */
        fill_rrect(d, (int)cx - 8, (int)cy - 3, 4, 7, 1, w, 255);
        for (int i = 0; i < 5; i++) fill_rrect(d, (int)cx - 4 + i, (int)cy - 3 - i, 1, 7 + 2 * i, 0, w, 255);
        for (int a = -40; a <= 40; a += 8) { float r = a * 0.01745f; fill_circle(d, cx + 3 + cosf(r) * 3.5f, cy + 0.5f + sinf(r) * 3.5f, 0.8f, w, 255);
                                             fill_circle(d, cx + 3 + cosf(r) * 7.f, cy + 0.5f + sinf(r) * 7.f, 0.8f, w, 255); }
        break;
    case IC_LIBRARY:                                           /* nota musical */
        sp_line(d, cx + 2.5f, cy - 8, cx + 2.5f, cy + 4, 0.9f, w); sp_line(d, cx + 2.5f, cy - 8, cx + 7, cy - 5, 0.9f, w);
        fill_circle(d, cx - 1, cy + 5, 3.4f, w, 255); break;
    case IC_HOME:
        for (int i = 0; i < 4; i++) fill_rrect(d, (int)cx - 7 + (i & 1) * 8, (int)cy - 7 + (i >> 1) * 8, 6, 6, 1.6f, w, 255);
        break;
    case IC_GESTURE:
        fill_circle(d, cx, cy, 7.6f, w, 255); fill_circle(d, cx, cy, 6.2f, bg, 255); fill_circle(d, cx, cy, 3.6f, w, 255); break;
    case IC_KB:
        fill_rrect(d, (int)cx - 10, (int)cy - 6, 20, 13, 2.5f, w, 255); fill_rrect(d, (int)cx - 9, (int)cy - 5, 18, 11, 2, bg, 255);
        for (int r = 0; r < 2; r++) for (int k = 0; k < 5; k++) fill_rrect(d, (int)cx - 8 + k * 3 + r, (int)cy - 3 + r * 3, 2, 2, 0, w, 255);
        fill_rrect(d, (int)cx - 5, (int)cy + 3, 10, 2, 0, w, 255); break;
    case IC_DEV:                                               /* </> */
        sp_line(d, cx - 4, cy - 5, cx - 8.5f, cy, 0.95f, w); sp_line(d, cx - 8.5f, cy, cx - 4, cy + 5, 0.95f, w);
        sp_line(d, cx + 4, cy - 5, cx + 8.5f, cy, 0.95f, w); sp_line(d, cx + 8.5f, cy, cx + 4, cy + 5, 0.95f, w);
        sp_line(d, cx + 1.8f, cy - 6, cx - 1.8f, cy + 6, 0.95f, w); break;
    default: break;
    }
}

static const char *row_value(const struct srow *r)
{
    if (r->val) return r->val();
    if (r->type == R_CHOICE && r->ival && *r->ival >= 0 && *r->ival < r->nopts) return tr(r->opts[*r->ival]);
    return 0;
}

/* dibuja la pagina; sel = fila pulsada (indice global) */
static int sp_content_h(const struct spage *pg)
{
    int h = 12 + pg->hero;
    for (int s = 0; s < pg->nsec; s++) {
        const struct ssec *sc = &pg->secs[s];
        if (sc->header) h += 28; else h += 14;
        h += sc->n * ROWH;
        if (sc->footer) h += 8 + draw_wrapped(0, 0, 0, W - 48, tr(sc->footer), 12, 0, 16, 0);
        h += 14;
    }
    return h;
}
static void sp_render(uint16_t *d, const struct spage *pg, int sel, const char *back)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    int y = NAVH + 30 + 12 - (int)sp_scroll + pg->hero, gi = 0;
    if (pg->hero && pg->hero_fn) pg->hero_fn(d, y - pg->hero, pg->hero);
    else if (pg->hero) {
        static struct sprite logo; if (!logo.w) load_sprite("hibyos_logo", &logo);
        int ly = y - pg->hero + (pg->hero - 16 - logo.h) / 2;
        if (logo.w && ly >= NAVH + 30 - logo.h) blit(d, &logo, (W - logo.w) / 2, ly, 32, 255);
    }
    for (int s = 0; s < pg->nsec; s++) {
        const struct ssec *sc = &pg->secs[s];
        if (sc->header) { draw_text(d, 32, y + 20, tr(sc->header), 12, rgb(142, 142, 147), 0, NAVH + 30, H); y += 28; } else y += 14;
        int top = y;
        fill_rrect(d, 16, top, W - 32, sc->n * ROWH, 10, rgb(28, 28, 30), 255);
        /* si en alguna fila no caben titulo y valor a 15, los valores de toda la seccion van a 13 */
        int vsz = 15;
        if (top < H && top + sc->n * ROWH > NAVH + 30) for (int r = 0; r < sc->n && vsz == 15; r++) {
            const struct srow *rw = &sc->rows[r];
            if (rw->type != R_NAV && rw->type != R_CHOICE && rw->type != R_INFO && rw->type != R_ACTION) continue;
            const char *v = row_value(rw); if (!v) continue;
            int room = W - 16 - 12 - (rw->type == R_NAV || rw->type == R_CHOICE ? 14 : 0) - (rw->icon ? 70 : 32) - 12;
            if (tw_n(tr(rw->title), 15, -1) + tw_n(v, 15, -1) > room) vsz = 13;
        }
        for (int r = 0; r < sc->n; r++, gi++) {
            const struct srow *rw = &sc->rows[r];
            int ry = top + r * ROWH;
            if (ry > H || ry + ROWH < NAVH + 30) { if (rw->type == R_TOGGLE && rw->ival) sw_pos(rw->ival); continue; }   /* fuera de la vista */
            int tx = rw->icon ? 70 : 32;
            if (gi == sel && rw->type != R_SLIDER) fill_rrect(d, 16, ry, W - 32, ROWH, (r == 0 || r == sc->n - 1) ? 10 : 0, rgb(58, 58, 60), 255);
            if (r) for (int x = tx; x < W - 16; x++) if (ry >= NAVH + 30 && ry < H) d[ry * W + x] = rgb(56, 56, 58);
            if (rw->icon) sp_icon(d, 28, ry + 7, rw->icon);
            if (rw->type == R_SLIDER && rw->ival) { sp_slider(d, rw, ry); continue; }
            if (rw->type == R_BAR) {                         /* almacenamiento: titulo, valor y barra */
                const char *v = row_value(rw); int vw = v ? tw_n(v, 13, -1) : 0; if (vw > 170) vw = 170;
                draw_text(d, tx, ry + 20, tr(rw->title), 15, 0xFFFF, W - 32 - tx - vw - 12, NAVH + 30, H);
                if (v) draw_text(d, W - 32 - vw, ry + 20, v, 13, rgb(142, 142, 147), 170, NAVH + 30, H);
                int pm = rw->ival ? *rw->ival : 0; if (pm < 0) pm = 0; if (pm > 1000) pm = 1000;
                fill_rrect(d, tx, ry + 29, W - 32 - tx, 6, 3, rgb(58, 58, 60), 255);
                int fw = (W - 32 - tx) * pm / 1000; if (fw > 0) fill_rrect(d, tx, ry + 29, fw < 6 ? 6 : fw, 6, 3, pm > 900 ? rgb(255, 159, 10) : rgb(10, 132, 255), 255);
                continue;
            }
            uint16_t tc = rw->dis ? rgb(99, 99, 102) : rw->type == R_ACTION && rw->fn && !rw->val ? rgb(10, 132, 255) : 0xFFFF;
            /* el valor (derecha) manda; el titulo usa el resto y se recorta con "…" */
            const char *v = rw->type == R_TOGGLE ? 0 : row_value(rw);
            int chev = rw->type == R_NAV || rw->type == R_CHOICE;
            int vx = W - 16 - 12 - (chev ? 14 : 0);
            /* el valor ocupa lo que deje el titulo (128 como minimo) */
            int vcap = vx - tx - 12 - tw_n(tr(rw->title), 15, -1); if (vcap < 128) vcap = 128;
            int vw = v ? tw_n(v, vsz, -1) : 0; if (vw > vcap) vw = vcap;
            int tmax = (rw->type == R_TOGGLE ? W - 16 - 12 - 51 - 10 - tx : vx - tx - (v ? vw + 12 : 0));
            int tsz = 15; while (tsz > 13 && tw_n(tr(rw->title), tsz, -1) > tmax) tsz--;   /* antes de recortar con "…", algo mas pequeno */
            draw_text(d, tx, ry + 28, tr(rw->title), tsz, tc, tmax, NAVH + 30, H);
            if (rw->type == R_TOGGLE && rw->ival) { float k = sw_pos(rw->ival); if (ry + 6 >= NAVH + 30) sp_switch(d, W - 16 - 12 - 51, ry + 6, k, rw->dis); }
            else if (rw->type == R_CHECK) {                 /* marca azul de iOS en la opcion elegida */
                if (rw->ival && *rw->ival == rw->nopts && ry + 14 >= NAVH + 30) {
                    for (int i = 0; i < 7; i++) fill_rrect(d, W - 46 + i, ry + 22 + i / 2, 2, 2, 1, rgb(10, 132, 255), 255);
                    for (int i = 0; i < 12; i++) fill_rrect(d, W - 40 + i, ry + 25 - i, 2, 2, 1, rgb(10, 132, 255), 255);
                }
            } else {
                if (v) draw_text(d, vx - vw, ry + 28, v, vsz, rgb(142, 142, 147), vcap, NAVH + 30, H);
                if (chev && ry + 16 >= NAVH + 30) for (int i = 0; i < 6; i++) {   /* (recortado abajo: no escribir fuera) */
                    int cx = W - 16 - 18 + i - 2, y1 = ry + 16 + i, y2 = ry + 28 - i;
                    if (y1 < H) { d[y1 * W + cx] = rgb(90, 90, 96); d[y1 * W + cx + 1] = rgb(90, 90, 96); }
                    if (y2 < H) { d[y2 * W + cx] = rgb(90, 90, 96); d[y2 * W + cx + 1] = rgb(90, 90, 96); }
                }
            }
        }
        y = top + sc->n * ROWH;
        if (sc->footer) { y += 8; y += draw_wrapped(d, 32, y, W - 48, tr(sc->footer), 12, rgb(142, 142, 147), 16, 1); }
        y += 14;
    }
    /* barra de navegacion (fija) */
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < NAVH + 30; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(12, 12, 13);
    for (int x = 0; x < W; x++) d[(NAVH + 29) * W + x] = rgb(38, 38, 40);
    if (back) {
        for (int i = 0; i < 9; i++) { fill_rrect(d, 12 + i, 62 - i, 3, 3, 1.2f, rgb(10, 132, 255), 255); fill_rrect(d, 12 + i, 62 + i, 3, 3, 1.2f, rgb(10, 132, 255), 255); }
        int tw = tw_n(tr(pg->title), 17, -1); if (tw > 216) tw = 216;
        int bmax = (W - tw) / 2 - 28 - 8; if (bmax < 0) bmax = 0;        /* la etiqueta de atras no pisa el titulo */
        /* como iOS: si el titulo anterior no cabe entero se pone "Atras", y si tampoco, solo la flecha */
        const char *bl = tr(back);
        if (tw_n(bl, 16, -1) > bmax) { bl = tr("Atrás"); if (tw_n(bl, 16, -1) > bmax) bl = 0; }
        if (bl) draw_text(d, 28, 68, bl, 16, rgb(10, 132, 255), bmax, 0, H);
        draw_text(d, (W - tw) / 2, 68, tr(pg->title), 17, 0xFFFF, 216, 0, H);
    } else draw_text(d, 16, 80, tr(pg->title), 26, 0xFFFF, 0, 0, H);
    draw_toast(d);
}
/* fila en (x,y) o -1 */
static int sp_hit(const struct spage *pg, int x, int y, const struct srow **out)
{
    (void)x;
    int yy = NAVH + 30 + 12 - (int)sp_scroll + pg->hero, gi = 0;
    for (int s = 0; s < pg->nsec; s++) {
        const struct ssec *sc = &pg->secs[s];
        yy += sc->header ? 28 : 14;
        for (int r = 0; r < sc->n; r++, gi++) if (y >= yy + r * ROWH && y < yy + (r + 1) * ROWH && y >= NAVH + 30) { *out = &sc->rows[r]; return gi; }
        yy += sc->n * ROWH;
        if (sc->footer) yy += 8 + draw_wrapped(0, 0, 0, W - 48, tr(sc->footer), 12, 0, 16, 0);
        yy += 14;
    }
    return -1;
}

/* ------------------------------------------------------------------ animacion de ejemplo de gestos */
/* HiBy dibujado en pequeno; el dedo hace el gesto y se ve el resultado (hoja, panel, inicio) */
static void gesture_demo(uint16_t *d, int cx, int y0, int edge, int act, double t)
{
    int w = 96, h = 144, x0 = cx - w / 2;
    fill_rrect(d, x0 - 4, y0 - 4, w + 8, h + 8, 16, rgb(70, 70, 76), 255);        /* carcasa */
    fill_rrect(d, x0, y0, w, h, 12, rgb(10, 10, 12), 255);                          /* pantalla */
    double ph = fmod(t / 1700.0, 1.0), e = ph < 0.55 ? ease_out(ph / 0.55) : 1;
    /* contenido: "app" (filas grises) */
    for (int i = 0; i < 5; i++) fill_rrect(d, x0 + 10, y0 + 18 + i * 22, w - 20, 14, 4, rgb(40, 40, 44), 255);
    /* resultado de la accion */
    if (act == GA_PLAYER) {
        int off = (int)((1 - e) * (edge == 1 || edge == 3 ? h : w));
        int sx = x0 + (edge == 5 ? off : edge == 4 ? -off : 0), sy = y0 + (edge == 1 ? off : edge == 3 ? -off : 0);
        fill_rrect(d, sx < x0 ? x0 : sx, sy < y0 ? y0 : sy, w - abs(sx - x0), h - abs(sy - y0), 10, rgb(220, 60, 100), 255);
        if (e > 0.6) fill_circle(d, x0 + w / 2.f, y0 + h - 30, 10, 0xFFFF, 255);
    } else if (act == GA_CC) {
        int ch = (int)(h * 0.6 * e);
        fill_rrect(d, x0 + 6, y0 + 4, w - 12, ch, 8, rgb(70, 70, 80), 220);
    } else if (act == GA_HOME) {
        float s = 1 - 0.55f * (float)e;
        int ww = (int)(w * s), hh = (int)(h * s);
        for (int i = 0; i < 6; i++) fill_rrect(d, x0 + 10 + (i % 3) * 28, y0 + 14 + (i / 3) * 30, 18, 18, 5, rgb(80 + i * 20, 120, 220 - i * 20), 255);
        fill_rrect(d, cx - ww / 2, y0 + (h - hh) / 2, ww, hh, 10, rgb(40, 40, 44), 255);
    } else if (act == GA_BACK) {
        int off = (int)(w * e);
        fill_rrect(d, x0 + off, y0, w - off, h, 10, rgb(60, 60, 66), 255);
    }
    /* dedo: desde el borde hacia dentro */
    float fx = cx, fy = y0 + h / 2.f, tx = fx, ty = fy;
    if (edge == 1) { fy = y0 + h - 4; ty = y0 + h * 0.45f; }
    else if (edge == 3) { fy = y0 + 4; ty = y0 + h * 0.6f; }
    else if (edge == 4) { fx = x0 + 4; tx = x0 + w * 0.7f; }
    else if (edge == 5) { fx = x0 + w - 4; tx = x0 + w * 0.3f; }
    float px = fx + (tx - fx) * (float)e, py = fy + (ty - fy) * (float)e;
    int alpha = ph < 0.75 ? 200 : (int)(200 * (1 - (ph - 0.75) / 0.25));
    for (int k = 1; k <= 6; k++) { float q = (float)e - k * 0.05f; if (q < 0) break;
        fill_circle(d, fx + (tx - fx) * q, fy + (ty - fy) * q, 7 - k * 0.6f, 0xFFFF, alpha / (k + 1)); }
    fill_circle(d, px, py, 9, 0xFFFF, alpha);
}

/* ------------------------------------------------------------------ paginas */
static int set_labels, set_dots, set_dockbg, set_dim, set_wgpage;
static int wg_page_on, wg_cache_ok, home_prev_ok;
static void wg_free_all(void);
static int set_lang_idx, set_icon_idx, set_cols_idx, set_kb_h = 1, set_kb_num = 0, set_kb_layout = 0, set_adb_mode;
static int g_bottom_app_i, g_bottom_home_i, g_top_i, g_topr_i, g_left_i, g_right_i, g_chev;
static const char *const O_ICON[] = { "Pequeños", "Normales", "Grandes" };
static const char *const O_COLS[] = { "3", "4", "5" };
static const char *const O_KBH[] = { "Compacta", "Normal", "Alta" };
static const char *const O_KBL[] = { "Español (QWERTY)", "Inglés (QWERTY)", "Francés (AZERTY)" };
static const char *const O_ADB[] = { "Desactivado", "Wi-Fi", "USB" };
static const char *const O_G_BOTTOM_APP[] = { "Inicio y multitarea", "Reproductor", "Centro de control", "Nada" };
static const int V_G_BOTTOM_APP[] = { GA_HOME, GA_PLAYER, GA_CC, GA_NONE };
static const char *const O_G_BOTTOM_HOME[] = { "Multitarea", "Reproductor", "Centro de control", "Nada" };
static const int V_G_BOTTOM_HOME[] = { GA_HOME, GA_PLAYER, GA_CC, GA_NONE };
static const char *const O_G_TOP[] = { "Centro de control", "Reproductor", "Nada" };
static const int V_G_TOP[] = { GA_CC, GA_PLAYER, GA_NONE };
static const char *const O_G_LEFT[] = { "Atrás", "Reproductor", "Centro de control", "Nada" };
static const int V_G_LEFT[] = { GA_BACK, GA_PLAYER, GA_CC, GA_NONE };
static const char *const O_G_RIGHT[] = { "Atrás", "Reproductor", "Centro de control", "Nada" };
static const int V_G_RIGHT[] = { GA_BACK, GA_PLAYER, GA_CC, GA_NONE };
static int idx_of(const int *v, int n, int x) { for (int i = 0; i < n; i++) if (v[i] == x) return i; return 0; }

static void sync_from_state(void)
{
    set_lang_idx = ui_lang_sys ? -1 : ui_lang; set_icon_idx = sz_idx(); set_cols_idx = L.cols - 3;
    g_bottom_app_i = idx_of(V_G_BOTTOM_APP, 4, G.bottom_app); g_bottom_home_i = idx_of(V_G_BOTTOM_HOME, 4, G.bottom_home);
    g_top_i = idx_of(V_G_TOP, 3, G.top); g_topr_i = idx_of(V_G_TOP, 3, G.top_r); g_left_i = idx_of(V_G_LEFT, 4, G.left); g_right_i = idx_of(V_G_RIGHT, 4, G.right);
    g_chev = G.chevron;
    set_labels = L.labels; set_dots = L.dots; set_dockbg = L.dockbg; set_dim = L.dim; set_wgpage = wg_page_on;
    FILE *f = fopen("/data/.hibymod_adbmode", "r"); char m[16] = "";
    if (f) { if (!fgets(m, sizeof m, f)) m[0] = 0; fclose(f); } else if (!access("/data/.hibymod_adbwifi", F_OK)) strcpy(m, "wifi");
    set_adb_mode = !strncmp(m, "usb", 3) ? 2 : !strncmp(m, "wifi", 4) ? 1 : 0;
}
static void relabel_apps(void);
/* idioma: -1 = igual que el sistema (el del reproductor de HiBy) */
static void apply_lang(void)
{
    ui_lang_sys = set_lang_idx < 0;
    ui_lang = ui_lang_sys ? lang_system() : lang_ok(set_lang_idx) ? set_lang_idx : LANG_EN;
    relabel_apps(); lconf_save();
}
static const char *v_lang(void) { return LANG_NAMES[lang_ok(ui_lang) ? ui_lang : LANG_EN]; }
/* reacomodar si ahora caben menos apps por pagina */
static void home_reflow(void)
{
    int pp = per_page();
    for (int pg = 0; pg < L.npages; pg++) while (L.pn[pg] > pp) {
        if (pg + 1 >= L.npages) { if (L.npages >= MAXPAGES) break; L.pn[L.npages++] = 0; }
        int last = L.page[pg][--L.pn[pg]];
        memmove(&L.page[pg + 1][1], &L.page[pg + 1][0], L.pn[pg + 1] * sizeof(int)); L.page[pg + 1][0] = last; L.pn[pg + 1]++;
    }
    if (cur_page >= L.npages) cur_page = L.npages - 1;
}
static void apply_home(void) { L.icon = 48 + 8 * set_icon_idx; L.cols = 3 + set_cols_idx; home_reflow(); relabel_apps(); home_prev_ok = 0; lconf_save(); }   /* etiquetas: al ancho de la celda nueva */
static void apply_look(void)
{
    int dim = L.dim != set_dim;
    L.labels = set_labels; L.dots = set_dots; L.dockbg = set_dockbg; L.dim = set_dim;
    if (dim) load_wall();
    if (wg_page_on != set_wgpage) { wg_page_on = set_wgpage; if (!wg_page_on && (widgets_on || V.scroll < 0)) { widgets_on = 0; V.scroll = cur_page * W; } if (!wg_page_on) wg_free_all(); }
    wg_cache_ok = 0; home_prev_ok = 0; lconf_save();
    launcher_park_publish();                  /* la imagen del inicio que pone la libreria, al dia */
}
static void apply_gestures(void)
{
    G.bottom_app = V_G_BOTTOM_APP[g_bottom_app_i]; G.bottom_home = V_G_BOTTOM_HOME[g_bottom_home_i];
    G.top = V_G_TOP[g_top_i]; G.top_r = V_G_TOP[g_topr_i]; G.left = V_G_LEFT[g_left_i]; G.right = V_G_RIGHT[g_right_i]; G.chevron = g_chev;
    lconf_save();
}
static void apply_kb(void) { kb_cfg_height = set_kb_h; kb_cfg_numrow = set_kb_num; kb_cfg_layout = set_kb_layout; lconf_save(); }
static void apply_adb(void)
{
    static const char *m[] = { "off", "wifi", "usb" };
    char *argv[] = { "/usr/data/hibymod/bin/hibymod_adb.sh", (char *)m[set_adb_mode], 0 };
    if (access(argv[0], X_OK)) argv[0] = "/usr/bin/hibymod_adb.sh";
    pid_t pid; posix_spawn(&pid, argv[0], 0, 0, argv, environ);   /* asincrono: cambia el USB */
    toast(tr("Aplicando…"));
}
static void act_wallpaper(void) { wallpaper_picker(); }
static void act_reset(void)
{
    if (!confirm(tr("¿Restablecer el inicio? Se pierde la colocación de las apps."), tr("Restablecer"))) return;
    lconf_default(); unlink(ASSETS "/wall_custom.raw"); load_wall(); cur_page = 0; V.scroll = 0; lconf_save();
    sync_from_state(); wg_cache_ok = 0; home_prev_ok = 0;
}
static void act_wallpaper_h(void) { wallpaper_picker(); home_prev_ok = 0; wg_cache_ok = 0; }
static const char *v_wall(void)
{
    /* los fondos de serie, con su nombre traducido (tr cachea por puntero: siempre literales) */
    static const char *const std[] = { "Negro", "Aurora", "Medianoche", "Océano", "Brasa", "Bosque", "Grafito", "Plata", "Atardecer" };
    /* nombres de fichero: solo ASCII (las actualizaciones por Internet no admiten otros); "Oceano.png"
     * se muestra como "Océano" (los de antes con tilde siguen valiendo) */
    static const char *const file[] = { "Negro", "Aurora", "Medianoche", "Oceano", "Brasa", "Bosque", "Grafito", "Plata", "Atardecer" };
    if (!L.wall[0]) return tr(std[1]);
    static char b[64]; const char *n = strrchr(L.wall, '/'); snprintf(b, sizeof b, "%s", n ? n + 1 : L.wall);
    char *e = strrchr(b, '.'); if (e && e != b) *e = 0;
    for (unsigned i = 0; i < sizeof std / sizeof *std; i++) if (!strcmp(b, std[i]) || !strcmp(b, file[i])) return tr(std[i]);
    return b;
}
static void page_gestures(struct spage *p);
static void nav_gestures_h(void) { open_page(page_gestures, "Inicio"); }
static void wg_edit_from_settings(void), nav_wg_list(void);
static const char *v_wg_count(void);
static void home_hero(uint16_t *d, int y, int h);
static char sv_buf[8][64];
static const char *v_adb_addr(void)
{
    FILE *f = fopen("/tmp/hibymod/adb_wifi_ip", "r");
    if (f) { if (fgets(sv_buf[0], 64, f)) { sv_buf[0][strcspn(sv_buf[0], "\n")] = 0; fclose(f); return sv_buf[0]; } fclose(f); }
    return tr("Sin conexión Wi-Fi");
}
static const char *v_version(void)
{
    if (sv_buf[1][0]) goto done;               /* popen cuesta ~100 ms: una sola vez */
    FILE *f = popen("/usr/bin/hibymod status 2>/dev/null | head -1", "r"); if (f) { if (!fgets(sv_buf[1], 64, f)) sv_buf[1][0] = 0; pclose(f); }
    sv_buf[1][strcspn(sv_buf[1], "\n")] = 0;
done:
    return sv_buf[1][0] ? sv_buf[1] + (strncmp(sv_buf[1], "hibymod ", 8) ? 0 : 8) : HIBYOS_VERSION;
}
static const char *v_mem(void)
{
    FILE *f = fopen("/proc/meminfo", "r"); char l[96]; long kb = 0;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "MemAvailable:", 13)) kb = atol(l + 13);
    if (f) fclose(f);
    snprintf(sv_buf[2], 64, "%.1f MB", kb / 1024.0); return sv_buf[2];
}

static void page_general(struct spage *p), page_home(struct spage *p), page_gestures(struct spage *p),
            page_kb(struct spage *p), page_dev(struct spage *p), page_about(struct spage *p);
static void open_page(void (*build)(struct spage *), const char *back);
static void nav_general(void) { open_page(page_general, "Ajustes"); }
static void page_lang(struct spage *p);
static void nav_lang(void) { open_page(page_lang, "General"); }   /* desde General */
/* titulo de la pagina raiz (el "atras" de sus hijas): "Ajustes" o, abierta desde el icono
 * Personalizar, "Personalizar" */
static const char *sp_root_name = "Ajustes";
static void nav_home(void) { home_prev_ok = 0; open_page(page_home, sp_root_name); }
static void nav_gestures(void) { open_page(page_gestures, sp_root_name); }
static void nav_kb(void) { set_kb_h = kb_cfg_height; set_kb_num = kb_cfg_numrow; set_kb_layout = kb_cfg_layout; open_page(page_kb, sp_root_name); }
static void nav_dev(void) { open_page(page_dev, "Ajustes"); }
static void nav_about(void);   /* sysset.inc.c (General > Informacion) */
static void nav_audio(void);
static void nav_cc(void), nav_power(void), nav_ota(void);

#define ROW(t, ty, iv, op, no, f, v, g) (struct srow){ t, ty, iv, op, no, f, v, g }
static void page_root(struct spage *p);   /* raiz y General: sysset.inc.c */
/* idiomas por su propio nombre (como iOS); los 13 del firmware de HiBy */
static void page_lang(struct spage *p)
{
    static const int ord[] = { LANG_EN, LANG_ES, LANG_FR, LANG_DE, LANG_IT, LANG_PL, LANG_RU,
                               LANG_UK, LANG_JA, LANG_KO, LANG_ZHS, LANG_ZHT, LANG_TH };
    *p = (struct spage){ "Idioma" };
    p->secs[0] = (struct ssec){ 0, "LANG_SYS_HELP", { ROW("Igual que el sistema", R_CHECK, &set_lang_idx, 0, -1, apply_lang, 0, 0) }, 1 };
    p->secs[1] = (struct ssec){ 0, 0, { { 0 } }, 7 };
    p->secs[2] = (struct ssec){ 0, 0, { { 0 } }, 6 };
    for (int i = 0; i < 13; i++) {
        struct srow *r = i < 7 ? &p->secs[1].rows[i] : &p->secs[2].rows[i - 7];
        *r = ROW(LANG_NAMES[ord[i]], R_CHECK, &set_lang_idx, 0, ord[i], apply_lang, 0, 0);
    }
    p->nsec = 3;
}
/* Pantalla de inicio (Personalizar > Inicio y la pildora "Ajustes" del modo edicion) */
static void page_home(struct spage *p)
{
    *p = (struct spage){ "Inicio" };
    p->hero = 186; p->hero_fn = home_hero;
    p->secs[0] = (struct ssec){ "DISEÑO", 0, { ROW("Tamaño de iconos", R_CHOICE, &set_icon_idx, O_ICON, 3, apply_home, 0, 0),
                                               ROW("Columnas", R_CHOICE, &set_cols_idx, O_COLS, 3, apply_home, 0, 0),
                                               ROW("Nombres de las apps", R_TOGGLE, &set_labels, 0, 0, apply_look, 0, 0),
                                               ROW("Puntos de página", R_TOGGLE, &set_dots, 0, 0, apply_look, 0, 0) }, 4 };
    p->secs[1] = (struct ssec){ "FONDO", 0, { ROW("Fondo de pantalla", R_NAV, 0, 0, 0, act_wallpaper_h, v_wall, 0),
                                              ROW("Oscurecer fondo", R_TOGGLE, &set_dim, 0, 0, apply_look, 0, 0) }, 2 };
    p->secs[2] = (struct ssec){ "WIDGETS", "WG_PAGE_HELP", {
                                              ROW("Página de widgets", R_TOGGLE, &set_wgpage, 0, 0, apply_look, 0, 0),
                                              ROW("Editar widgets", R_NAV, 0, 0, 0, wg_edit_from_settings, 0, 0),
                                              ROW("Widgets", R_NAV, 0, 0, 0, nav_wg_list, v_wg_count, 0) }, 3 };
    p->secs[3] = (struct ssec){ "DOCK Y GESTOS", 0, { ROW("Fondo del dock", R_TOGGLE, &set_dockbg, 0, 0, apply_look, 0, 0),
                                                      ROW("Flecha de música en el inicio", R_TOGGLE, &g_chev, 0, 0, apply_gestures, 0, 0),
                                                      ROW("Gestos", R_NAV, 0, 0, 0, nav_gestures_h, 0, 0) }, 3 };
    p->secs[4] = (struct ssec){ 0, 0, { ROW("Restablecer inicio", R_ACTION, 0, 0, 0, act_reset, 0, 0) }, 1 };
    p->nsec = 5;
}
static void page_gestures(struct spage *p)
{
    *p = (struct spage){ "Gestos" };
    p->secs[0] = (struct ssec){ "BORDES", "Desliza desde el borde para ver la animación de ejemplo.", {
        ROW("Abajo, en apps", R_CHOICE, &g_bottom_app_i, O_G_BOTTOM_APP, 4, apply_gestures, 0, 1),
        ROW("Abajo, en el inicio", R_CHOICE, &g_bottom_home_i, O_G_BOTTOM_HOME, 4, apply_gestures, 0, 2),
        ROW("Arriba, izquierda", R_CHOICE, &g_top_i, O_G_TOP, 3, apply_gestures, 0, 3),
        ROW("Arriba, derecha", R_CHOICE, &g_topr_i, O_G_TOP, 3, apply_gestures, 0, 6),
        ROW("Borde izquierdo", R_CHOICE, &g_left_i, O_G_LEFT, 4, apply_gestures, 0, 4),
        ROW("Borde derecho", R_CHOICE, &g_right_i, O_G_RIGHT, 4, apply_gestures, 0, 5) }, 6 };
    p->nsec = 1;
}
static void page_kb(struct spage *p)
{
    *p = (struct spage){ "Teclado" };
    p->secs[0] = (struct ssec){ 0, 0, { ROW("Altura", R_CHOICE, &set_kb_h, O_KBH, 3, apply_kb, 0, 0),
                                        ROW("Fila de números", R_TOGGLE, &set_kb_num, 0, 0, apply_kb, 0, 0),
                                        ROW("Distribución", R_CHOICE, &set_kb_layout, O_KBL, 3, apply_kb, 0, 0) }, 3 };
    p->nsec = 1;
}
static const char *adb_footer(void) { return set_adb_mode == 2 ? "ADB_HELP_USB" : set_adb_mode == 1 ? "ADB_HELP_WIFI" : "ADB_HELP_OFF"; }
static void page_dev(struct spage *p)
{
    *p = (struct spage){ "Desarrollador" };
    p->secs[0] = (struct ssec){ "ADB", adb_footer(), { ROW("ADB", R_CHOICE, &set_adb_mode, O_ADB, 3, apply_adb, 0, 0),
                                                         ROW("Dirección", R_INFO, 0, 0, 0, 0, v_adb_addr, 0) }, set_adb_mode == 1 ? 2 : 1 };
    p->nsec = 1;
}
/* ------------------------------------------------------------------ Audio y funciones ocultas
 * - Filtro digital del ES9219: 8 formas con el modulo parcheado (0.5.4+); sin parche solo 0/6.
 *   Se aplica con amixer y se guarda en audio.conf (se reaplica al arrancar el shell).
 * - config.json (aviso de volumen UE, biblioteca y caratulas en la SD): override en
 *   /usr/data/hibymod/override/config.json, que hiby_player.sh monta al arrancar -> reinicio.
 * Ver docs/research_hidden_firmware.md. */
#define AUDIO_CONF "/usr/data/hibymod/audio.conf"
#define OVR_DIR "/usr/data/hibymod/override"
static const char *const O_FILT8[] = { "Rápido, fase lineal", "Lento, fase lineal", "Rápido, fase mínima", "Lento, fase mínima",
                                       "Apodizante", "Híbrido, fase mínima", "Brickwall" };
static const int V_FILT8[] = { 0, 1, 2, 3, 4, 6, 7 };
static const char *const O_FILT2[] = { "Rápido, fase lineal", "Híbrido, fase mínima" };
static const int V_FILT2[] = { 0, 6 };
static int au_filt_i, au_filt_max = -1, au_volwarn = 1, au_tfdb, au_tfimg, au_reboot;

static int amixer_filter(int set)        /* set < 0: solo lee; devuelve el valor actual o -1 */
{
    char v[8]; snprintf(v, sizeof v, "%d", set);
    char *ga[] = { "/usr/bin/amixer", "-c", "0", "cget", "name=Digital Filter", 0 };
    char *sa[] = { "/usr/bin/amixer", "-c", "0", "-q", "cset", "name=Digital Filter", v, 0 };
    char out[512];
    if (set >= 0) { run_capture(sa, out, sizeof out); }
    if (run_capture(ga, out, sizeof out) <= 0) return -1;
    char *m = strstr(out, "max="); if (m) au_filt_max = atoi(m + 4);
    char *c = strstr(out, ": values="); return c ? atoi(c + 9) : -1;
}
static const int *filt_vals(int *n) { if (au_filt_max >= 7) { *n = 7; return V_FILT8; } *n = 2; return V_FILT2; }
static volatile int au_filt_cur = -1;
static void *audio_boot_th(void *a)      /* amixer tarda: en segundo plano, nunca en la interfaz */
{
    (void)a;
    FILE *f = fopen(AUDIO_CONF, "r"); int v = -1; char l[64];
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "filter=", 7)) v = atoi(l + 7);
    if (f) fclose(f);
    au_filt_cur = amixer_filter(v);       /* reaplica el elegido (o solo lee) y aprende max */
    return 0;
}
static void audio_boot(void) { pthread_t th; if (!pthread_create(&th, 0, audio_boot_th, 0)) pthread_detach(th); }
/* lee "clave":N del config.json efectivo (override si existe) */
static int cfg_get(const char *key, int def)
{
    char buf[8192]; FILE *f = fopen(OVR_DIR "/config.json", "r"); if (!f) f = fopen("/usr/resource/config.json", "r");
    if (!f) return def;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    char *p = strstr(buf, pat); if (!p) return def;
    p = strchr(p + strlen(pat), ':'); if (!p) return def;
    while (*++p == ' ') {}
    return isdigit((unsigned char)*p) ? atoi(p) : def;
}
static int cfg_set(const char *key, int val)
{
    char buf[8192]; FILE *f = fopen(OVR_DIR "/config.json", "r"); if (!f) f = fopen("/usr/resource/config.json", "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    char *p = strstr(buf, pat); if (!p) return -1;
    p = strchr(p + strlen(pat), ':'); if (!p) return -1;
    while (*++p == ' ') {}
    if (!isdigit((unsigned char)*p)) return -1;
    *p = (char)('0' + (val ? 1 : 0));
    mkdir(OVR_DIR, 0755);
    /* "w" trunca el mismo inodo: si ya esta montado encima de /usr/resource se ve al instante */
    f = fopen(OVR_DIR "/config.json", "w"); if (!f) return -1;
    fwrite(buf, 1, n, f); fclose(f); sync();
    au_reboot = 1;
    return 0;
}
static void audio_sync(void)
{
    int cur = au_filt_cur, n; const int *vv = filt_vals(&n);
    au_filt_i = 0; for (int i = 0; i < n; i++) if (vv[i] == cur) au_filt_i = i;
    au_volwarn = cfg_get("vol_warn_enable", 1); au_tfdb = cfg_get("tf_music_db_enable", 0); au_tfimg = cfg_get("tf_image_cache_enable", 0);
}
static void apply_filter(void)
{
    int n; const int *vv = filt_vals(&n); if (au_filt_i >= n) au_filt_i = 0;
    int v = vv[au_filt_i], got = amixer_filter(v); au_filt_cur = got;
    FILE *f = fopen(AUDIO_CONF, "w"); if (f) { fprintf(f, "filter=%d\n", v); fclose(f); }
    if (got != v) toast(tr("No se pudo aplicar el filtro"));
}
static void apply_volwarn(void)
{
    if (!au_volwarn && !confirm(tr("VOLWARN_CONFIRM"), tr("Quitar aviso"))) { au_volwarn = 1; return; }
    if (cfg_set("vol_warn_enable", au_volwarn)) toast(tr("No se pudo guardar"));
}
static void apply_tfdb(void) { if (cfg_set("tf_music_db_enable", au_tfdb)) toast(tr("No se pudo guardar")); }
static void apply_tfimg(void) { if (cfg_set("tf_image_cache_enable", au_tfimg)) toast(tr("No se pudo guardar")); }
static void act_reboot(void)
{
    if (!confirm(tr("¿Reiniciar ahora? La música se detendrá."), tr("Reiniciar"))) return;
    sync(); char *argv[] = { "/sbin/reboot", 0 }; pid_t pid; posix_spawn(&pid, argv[0], 0, 0, argv, environ);
}
/* las paginas Sonido y Biblioteca (sysset.inc.c) usan lo de arriba */

/* ------------------------------------------------------------------ Centro de control (configurable) */
static int cc_on[10];
static void cc_sync(void) { memset(cc_on, 0, sizeof cc_on); for (int i = 0; i < cc_ntiles; i++) cc_on[cc_tiles[i]] = 1; }
static void apply_cc(void)
{
    /* conserva el orden actual y anade los nuevos al final */
    int keep[10], n = 0;
    for (int i = 0; i < cc_ntiles; i++) if (cc_on[cc_tiles[i]]) keep[n++] = cc_tiles[i];
    for (int t = 0; t < 10; t++) if (cc_on[t]) { int have = 0; for (int i = 0; i < n; i++) have |= keep[i] == t; if (!have) keep[n++] = t; }
    memcpy(cc_tiles, keep, sizeof keep); cc_ntiles = n; lconf_save();
}
static void apply_power(void) { pw_apply(); }
static int set_nocharge;
static void apply_nocharge(void) { pw_set_nocharge(set_nocharge); }
static const char *const O_POWER[] = { "Ahorro", "Normal", "Rendimiento" };
static void page_cc(struct spage *p)
{
    *p = (struct spage){ "Centro de control" };
    p->secs[0] = (struct ssec){ "CONTROLES", 0, {
        ROW("Modo avión", R_TOGGLE, &cc_on[8], 0, 0, apply_cc, 0, 0), ROW("Control de ruido", R_TOGGLE, &cc_on[9], 0, 0, apply_cc, 0, 0),
        ROW("Bluetooth", R_TOGGLE, &cc_on[3], 0, 0, apply_cc, 0, 0), ROW("Wi-Fi", R_TOGGLE, &cc_on[4], 0, 0, apply_cc, 0, 0),
        ROW("Solo USB", R_TOGGLE, &cc_on[0], 0, 0, apply_cc, 0, 0), ROW("Modo de energía", R_TOGGLE, &cc_on[1], 0, 0, apply_cc, 0, 0) }, 6 };
    p->secs[1] = (struct ssec){ 0, "CC_HELP", {
        ROW("DAC USB", R_TOGGLE, &cc_on[2], 0, 0, apply_cc, 0, 0), ROW("Filtro digital", R_TOGGLE, &cc_on[5], 0, 0, apply_cc, 0, 0),
        ROW("Archivos", R_TOGGLE, &cc_on[6], 0, 0, apply_cc, 0, 0), ROW("Ajustes", R_TOGGLE, &cc_on[7], 0, 0, apply_cc, 0, 0) }, 4 };
    p->nsec = 2;
}
static const char *const O_HZ[] = { "45 Hz (de serie)", "60 Hz" };
static int set_hz_i;
static void apply_hz(void) { lcd_hz = set_hz_i ? 60 : 45; lcd_apply(); pw_save(); }
static void page_power(struct spage *p)
{
    *p = (struct spage){ "Batería y energía" };
    p->secs[0] = (struct ssec){ 0, "POWER_HELP", { ROW("Modo de energía", R_CHOICE, &pw_mode, O_POWER, 3, apply_power, 0, 0) }, 1 };
    p->secs[1] = (struct ssec){ 0, "NOCHARGE_HELP", { ROW("Solo USB (no cargar)", R_TOGGLE, &set_nocharge, 0, 0, apply_nocharge, 0, 0) }, 1 };
    p->nsec = 2;
    (void)set_hz_i; (void)O_HZ; (void)apply_hz;
}
static void nav_cc(void) { cc_sync(); open_page(page_cc, sp_root_name); }
static void nav_power(void) { set_nocharge = pw_nocharge; set_hz_i = lcd_hz == 60; open_page(page_power, "Ajustes"); }

static const char *v_apps(void) { int n = 0; DIR *d = opendir("/usr/data/hibymod/apps"); struct dirent *e; while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++; if (d) closedir(d); snprintf(sv_buf[3], 64, "%d", n); return sv_buf[3]; }
/* sp_gone: se salio al inicio con el gesto de la barra; sp_self: app (launcher) que somos */
static int sp_gone, sp_self = -1;
static void ps_tick(void); static int ps_busy(void); static void ps_refresh(void); static volatile int ps_gen;
/* pagina de eleccion (lista con marca) con animacion de ejemplo si es un gesto */
static void choice_page(const struct srow *rw, const char *back)
{
    struct spage pg = { rw->title }; int demo = rw->gesture;
    pg.secs[0].n = rw->nopts; pg.secs[0].header = 0;
    for (int i = 0; i < rw->nopts; i++) pg.secs[0].rows[i] = ROW(rw->opts[i], R_INFO, 0, 0, 0, 0, 0, 0);
    pg.nsec = 1;
    int top_extra = demo ? 176 : 0;
    float saved = sp_scroll; sp_scroll = -top_extra;
    double t0 = now_ms();
    for (;;) {
        sp_render(fr0, &pg, -1, back);
        /* marca de la opcion elegida */
        int y = NAVH + 30 + 12 + top_extra + 14 + *rw->ival * ROWH;
        for (int i = 0; i < 7; i++) { fill_rrect(fr0, W - 46 + i, y + 22 + i / 2, 2, 2, 1, rgb(10, 132, 255), 255); }
        for (int i = 0; i < 12; i++) fill_rrect(fr0, W - 40 + i, y + 25 - i, 2, 2, 1, rgb(10, 132, 255), 255);
        if (demo) {
            int ed = demo == 1 || demo == 2 ? 1 : demo == 6 ? 3 : demo;
            int act = demo == 1 ? V_G_BOTTOM_APP[*rw->ival] : demo == 2 ? V_G_BOTTOM_HOME[*rw->ival] : demo == 3 || demo == 6 ? V_G_TOP[*rw->ival] : demo == 4 ? V_G_LEFT[*rw->ival] : V_G_RIGHT[*rw->ival];
            if (demo == 2 && act == GA_PLAYER) act = GA_PLAYER;
            gesture_demo(fr0, W / 2, NAVH + 30 + 16, ed, act, now_ms() - t0);
        }
        present();
        ps_tick();
        struct pkt p;
        if (!read_pkt(&p, demo ? 0 : (ps_busy() || now_ms() < toast_until + 40) ? 30 : 300)) continue;
        if (!p.down) continue;
        int x = p.x, yy = p.y;
        if (yy >= 456 && sp_self >= 0) {                  /* desde abajo: inicio */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (yy - cur_y > 12) up = 1; }
            if (up && own_home_gesture(x, yy, sp_self)) { sp_gone = 1; break; }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        { int bk = app_back_gesture(x, yy); if (bk > 0) break; if (bk < 0) continue; }
        while (cur_down && read_pkt(&p, 3000)) {}
        if (yy < NAVH + 30 && x < 110) break;
        const struct srow *hr; int i = sp_hit(&pg, x, yy, &hr);
        if (i >= 0) { *rw->ival = i; if (rw->fn) rw->fn(); t0 = now_ms(); }
    }
    sp_scroll = saved;
}

/* bucle de una pagina; vuelve al pulsar "atras" o deslizar desde el borde.
 * sp_gone: se salio al inicio con el gesto de la barra (desde cualquier pagina): todas las
 * paginas abiertas vuelven sin transicion y la app no hace su animacion de cierre. */
static void open_page(void (*build)(struct spage *), const char *back)
{
    if (sp_gone) return;
    struct spage pg; build(&pg);
    float saved = sp_scroll; sp_scroll = 0;
    if (back) {   /* fs_transition no puede leer de fr0 mientras escribe en el: copia antes */
        memcpy(img_under, fr0, HM_FRAME_BYTES); have_under = 0;
        sp_render(img_base, &pg, -1, back); fs_transition(img_under, img_base, 1, 0);
    }
    float vel = 0;
    /* repintar solo si algo cambio: un toque, un interruptor animandose, un aviso, valores
     * nuevos de la libreria (ps_gen) o cada ~400 ms (memoria, almacenamiento...). Antes se
     * repintaba la pagina entera cada 30 ms mientras la libreria contestaba (hasta segundos). */
    int need = 1, seen_gen = ps_gen; double drawn = 0;
    for (;;) {
        ps_tick();
        double tn = now_ms();
        if (need || sw_busy() || ps_gen != seen_gen || tn < toast_until + 40 || tn - drawn >= 390) {
            seen_gen = ps_gen; need = 0;
            build(&pg);
            sp_render(fr0, &pg, -1, back); present(); drawn = now_ms();
        }
        struct pkt p;
        int tmo = sw_busy() ? 0 : (ps_busy() || now_ms() < toast_until + 40) ? 30 : 400;
        if (!read_pkt(&p, tmo)) continue;
        need = 1;
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms(); float s0 = sp_scroll; int moved = 0, ly = sy; double lt = ts;
        const struct srow *rw = 0; int hi = sp_hit(&pg, sx, sy, &rw);
        /* desde abajo: inicio (la ventana sigue al dedo; mantener 1 s: multitarea) */
        if (sy >= 456 && sp_self >= 0) {   /* (sp_self < 0: abierta desde el modo edicion del inicio) */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) {
                sp_render(fr0, &pg, -1, back);
                if (own_home_gesture(sx, sy, sp_self)) { sp_gone = 1; break; }
                continue;
            }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        /* un deslizador recibe el arrastre horizontal; el "atras" solo desde el filo de la pantalla */
        int on_slider = hi >= 0 && rw && rw->type == R_SLIDER && rw->ival && !rw->dis && sx > 20 && sx < W - 20;
        if ((back || sp_self >= 0) && !on_slider) {   /* atras desde los dos bordes (burbuja); en la raiz, al inicio */
            int bk = app_back_gesture(sx, sy);
            if (bk > 0) break;
            if (bk < 0) continue;
            if (!cur_down) { sx = cur_x; sy = cur_y; hi = sp_hit(&pg, sx, sy, &rw); on_slider = 0; }
        }
        /* resaltado de la fila a los 70 ms con el dedo quieto (iOS): al pintarlo al tocar, cada
         * desplazamiento encendia la fila un fotograma (lo encontro el probador) */
        int hl_shown = 0;
        int sliding = 0, v0 = on_slider ? *rw->ival : 0;
        while (cur_down) {   /* hasta soltar: una pausa del dedo no es un toque */
            int wait_hl = hi >= 0 && !on_slider && !hl_shown && !moved;
            if (!read_pkt(&p, wait_hl ? 20 : 3000)) {
                if (!wait_hl) break;
                if (now_ms() - ts >= 70) { sp_render(fr0, &pg, hi, back); present(); hl_shown = 1; }
                continue;
            }
            if (on_slider && !moved && !sliding && abs(cur_x - sx) > 6 && abs(cur_x - sx) > abs(cur_y - sy)) sliding = 1;
            if (sliding) {
                int nv = sl_value(rw, v0, cur_x - sx);
                if (nv != *rw->ival) { *rw->ival = nv; if (rw->fn) rw->fn(); }
                ps_tick(); sp_render(fr0, &pg, -1, back); present();
                continue;
            }
            if (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 10) moved = 1;
            if (moved) {
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                float maxs = sp_content_h(&pg) - (H - NAVH - 30); if (maxs < 0) maxs = 0;
                sp_scroll = s0 - (cur_y - sy); if (sp_scroll < 0) sp_scroll = 0; if (sp_scroll > maxs) sp_scroll = maxs;
                sp_render(fr0, &pg, -1, back); present();
            }
        }
        if (sliding) continue;
        if (moved) {
            /* inercia de iOS: decelera ~0.998/ms; se para al tocar o en los extremos */
            float maxs = sp_content_h(&pg) - (H - NAVH - 30); if (maxs < 0) maxs = 0;
            if (now_ms() - lt > 60) vel = 0;          /* el dedo se paro antes de soltar */
            double tp = now_ms();
            while (fabsf(vel) > 0.03f) {
                double t = now_ms(); float dt = (float)(t - tp); tp = t; if (dt > 40) dt = 40;
                sp_scroll -= vel * dt; vel *= powf(0.9975f, dt);
                if (sp_scroll < 0) { sp_scroll = 0; vel = 0; } if (sp_scroll > maxs) { sp_scroll = maxs; vel = 0; }
                sp_render(fr0, &pg, -1, back); present();
                struct pkt q; if (read_pkt(&q, 0) && q.down) { vel = 0; drain_until_up(); break; }   /* el toque que frena no activa nada (iOS) */
            }
            vel = 0;
            continue;
        }
        if (back && sy < NAVH + 30 && sx < 110) break;
        if (hi < 0 || !rw || now_ms() - ts > 700) continue;           /* pulsacion corta y quieta */
        if (!hl_shown && !on_slider) { sp_render(fr0, &pg, hi, back); present(); msleep(40); }   /* toque rapido: se ve igual */
        if (rw->dis) { toast(tr("No disponible ahora")); continue; }
        if (rw->type == R_SLIDER && rw->ival) {                      /* toque en los extremos: un paso */
            int st = rw->nopts == SL_BRIGHT ? 10 : 1, nv = *rw->ival;
            if (sx < SL_X0 - 6) nv -= st; else if (sx > SL_X1 + 6) nv += st; else continue;
            if (nv < rw->lo) nv = rw->lo; if (nv > rw->hi) nv = rw->hi;
            if (nv != *rw->ival) { *rw->ival = nv; if (rw->fn) rw->fn(); }
            continue;
        }
        if (rw->type == R_NAV && rw->fn) { rw->fn(); if (sp_gone) break; ps_refresh(); }
        else if (rw->type == R_TOGGLE && rw->ival) { *rw->ival = !*rw->ival; if (rw->fn) rw->fn(); }
        else if (rw->type == R_CHECK && rw->ival) { if (*rw->ival != rw->nopts) { *rw->ival = rw->nopts; if (rw->fn) rw->fn(); } }
        else if (rw->type == R_CHOICE && rw->ival) {
            struct spage cur; build(&cur); sp_render(img_under, &cur, -1, back); have_under = 0;
            choice_page(rw, pg.title);
            if (sp_gone) break;
        } else if (rw->type == R_ACTION && rw->fn) rw->fn();
        if (sp_gone) break;
    }
    sp_scroll = saved;
    if (sp_gone) return;
    /* volver: transicion inversa */
    sp_render(img_under, &pg, -1, back); have_under = 0;
    (void)vel;
}

/* textos ya rasterizados en el idioma anterior: rehacerlos */
static void sprite_free(struct sprite *s) { free(s->rgb); free(s->a); memset(s, 0, sizeof *s); }
static void relabel_apps(void)
{
    for (int a = 0; a < NAPPS; a++) { sprite_free(&apps[a].lbl); app_label_sprite(a); }
    sprite_free(&pill_done); sprite_free(&pill_cfg);
    text_sprite(&pill_done, tr("Listo"), 14.f, 0xFFFF, 0);
    text_sprite(&pill_cfg, tr("Ajustes"), 14.f, 0xFFFF, 0);
    sprite_free(&s_title); sprite_free(&s_pick_title);          /* hojas antiguas: se rehacen al abrirse */
    edit_bg_ok = 0; mirror_dirty = 1;
}

/* ------------------------------------------------------------------ Apps (instalador) */
static void store_page(struct spage *p)
{
    *p = (struct spage){ "Apps" };
    p->secs[0] = (struct ssec){ 0, "HPK_HELP", { ROW("Apps instaladas", R_INFO, 0, 0, 0, 0, v_apps, 0) }, 1 };
    p->nsec = 1;
}
static void store_app_ui(int ix, int iy);
static void store_app(int ix, int iy) { (void)store_page; store_app_ui(ix, iy); }

/* ------------------------------------------------------------------ paginas del sistema */
#include "sysset.inc.c"

/* abre la app con zoom desde (ix, iy). mode 0: la raiz de Ajustes; 1: directamente en USB (el
 * boton "DAC USB" del Centro de control: una raiz propia, sin "atras"); 2: la pagina de
 * personalizacion (el icono "Personalizar" del inicio) */
static void settings_run(int ix, int iy, int mode)
{
    sync_from_state(); ss_open();
    /* la tarjeta de multitarea es la del icono que nos abrio (launch_app pone last_app) */
    int a = last_app >= 0 && last_app < NAPPS ? last_app : app_by_id("settings");
    sp_self = a; sp_gone = 0;
    void (*root_fn)(struct spage *) = mode == 1 ? page_usb_root : mode == 2 ? page_pers
        : mode == 3 ? page_power : mode == 4 ? page_sound : mode == 5 ? page_display : mode == 6 ? page_cc : page_root;
    /* paginas abiertas directamente (mantener pulsado en el Centro de control): lo mismo que
     * hace su fila de Ajustes antes de abrirla */
    if (mode == 3) set_nocharge = pw_nocharge;
    if (mode == 4) audio_sync();
    if (mode == 5) { int b = sysfs_get(BL_PATH, -1); ui_bright = b < 0 ? (ps_ok[PS_BRIGHT] ? ps_v[PS_BRIGHT] : 50) : b * 100 / 101;
                     if (ui_bright < 1) ui_bright = 1; if (ui_bright > 100) ui_bright = 100; }
    sp_root_name = mode == 2 ? "Personalizar" : "Ajustes";
    struct spage root; root_fn(&root);
    sp_scroll = 0;
    sp_render(img_app, &root, -1, 0);
    render_launcher(img_base, now_ms());
    double t0 = now_ms();
    for (;;) {   /* apertura con zoom desde el icono */
        double t = (now_ms() - t0) / 300; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    open_page(root_fn, 0);
    if (!sp_gone) {   /* salir: encoger hacia el icono (el gesto de inicio ya lo hizo si sp_gone) */
        struct spage pg; root_fn(&pg); sp_render(img_app, &pg, -1, 0);
        render_launcher(img_base, now_ms());
        t0 = now_ms();
        for (;;) {
            double t = (now_ms() - t0) / 280; if (t > 1) t = 1; double e = 1 - ease_out(t);
            int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
            zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
            present(); if (t >= 1) break;
        }
    }
    sp_gone = 0; sp_self = -1; sp_root_name = "Ajustes";
    drain_until_up();
    launcher_present();
}
/* Ajustes: lo tiene todo. "Personalizar" es un acceso directo a su grupo de personalizacion */
static void settings_app(int ix, int iy) { settings_run(ix, iy, 0); }
static void personalize_app(int ix, int iy) { settings_run(ix, iy, 2); }
/* una pagina concreta como raiz (SET_POWER, SET_SOUND, SET_DISPLAY, SET_CCPAGE) */
static void settings_deep_app(int ix, int iy, int start) { settings_run(ix, iy, 3 + (start - SET_POWER)); }
/* DAC USB: abre directamente la pagina USB (sustituye a la app DAC USB de HiBy) */
static void usbdac_app_at(int ix, int iy) { settings_run(ix, iy, 1); }
static void usbdac_app(void) { usbdac_app_at((W - L.icon) / 2, (H - L.icon) / 2); }
