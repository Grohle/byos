/* Radio: dibujo. Reutiliza las piezas de Vinyl (zonas tactiles fm_hit, barra de navegacion,
 * glifos, colores); lo propio lleva el prefijo ra_ y los toques RH_*. */
#define RA_RH 60                 /* alto de una fila de emisora */
#define RA_MINI_Y 392            /* tarjeta de "sonando" */
#define RA_MINI_TOP 386          /* hasta aqui llegan las listas cuando hay tarjeta */
enum { RG_ROOT, RG_LIST, RG_CTRY, RG_N };
enum { RH_ST = 100, RH_STAR, RH_TAB, RH_NAV, RH_GENRE, RH_CTRY, RH_ADD, RH_OWNMORE, RH_RETRY, RH_MINI, RH_MSTAR, RH_MPLAY };

static int ra_stack[4], ra_sp, ra_ch[RG_N], ra_editing, ra_tab;
static float ra_pos[RG_N], ra_vel, ra_edit_e;       /* ra_edit_e: 0 = cabecera normal, 1 = buscando */
static char ra_q[96], ra_q_sent[96]; static double ra_q_typed, ra_tab_t0;
static int ra_clip0, ra_clip1;                      /* ventana de la lista (las zonas tactiles se recortan a ella) */

/* ------------------------------------------------------------------ medidas */
static float ra_hdr_c(void) { float p = ra_pos[RG_ROOT]; if (p < 0) p = 0; if (p > 46) p = 46; return p * (1 - ra_edit_e); }   /* el titulo grande se recoge al bajar */
static int ra_bar_y(void) { float y0 = 82 - ra_hdr_c(); return (int)(y0 + (36 - y0) * ra_edit_e); }
static int ra_hdr_bot(void) { return ra_bar_y() + 44 + (int)(42 * (1 - ra_edit_e)); }
static int ra_bottom(void) { return ra_np.have ? RA_MINI_TOP : H; }
static int ra_top0(int pg) { return pg == RG_ROOT ? 80 + (int)(88 * (1 - ra_edit_e)) : 74; }
static float ra_maxpos(int pg) { float m = (float)(ra_ch[pg] - (ra_bottom() - ra_top0(pg))); return m > 0 ? m : 0; }

static void ra_hit(int x0, int y0, int x1, int y1, int kind, int idx)
{
    if (y0 < ra_clip0) y0 = ra_clip0;
    if (y1 > ra_clip1) y1 = ra_clip1;
    if (y1 - y0 > 6) fm_hit(x0, y0, x1, y1, kind, idx);
}
/* id de fila: lista << 16 | indice. Listas: 0 busqueda, 1 lista abierta, 2 favoritas, 3 recientes, 4 propias */
static struct ra_station *ra_station_at(int id)
{
    int l = id >> 16, i = id & 0xffff;
    if (l == 0 || l == 1) return ra_ls[l].it && i < ra_ls[l].n ? &ra_ls[l].it[i] : 0;
    if (l == 2) return i < ra_lb.nfav ? &ra_lb.fav[i] : 0;
    if (l == 3) return i < ra_lb.nrec ? &ra_lb.rec[i] : 0;
    if (l == 4) return i < ra_lb.nown ? &ra_lb.own[i] : 0;
    return 0;
}

/* ------------------------------------------------------------------ piezas */
/* estrella de 5 puntas en 22x22 (mascaras con 4x4 muestras): [0] contorno, [1] rellena */
static uint8_t ra_star_m[2][22 * 22]; static int ra_star_ok;
static int ra_in_poly(const float *vx, const float *vy, float x, float y)
{
    int in = 0;
    for (int i = 0, j = 9; i < 10; j = i++)
        if ((vy[i] > y) != (vy[j] > y) && x < (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]) + vx[i]) in = !in;
    return in;
}
static void ra_star_build(void)
{
    float ox[10], oy[10], ix[10], iy[10];
    for (int i = 0; i < 10; i++) {
        float a = (float)(-M_PI / 2 + i * M_PI / 5), r = i & 1 ? 4.3f : 9.8f;
        ox[i] = r * cosf(a); oy[i] = r * sinf(a) + 0.8f;
        ix[i] = ox[i] * 0.58f; iy[i] = (oy[i] - 0.8f) * 0.58f + 0.8f;      /* todas las aristas equidistan del centro: encoger = grosor uniforme */
    }
    for (int y = 0; y < 22; y++) for (int x = 0; x < 22; x++) {
        int full = 0, hole = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float px = x + (sx + .5f) / 4 - 11, py = y + (sy + .5f) / 4 - 11;
            int in = ra_in_poly(ox, oy, px, py);
            full += in; hole += in && ra_in_poly(ix, iy, px, py);
        }
        ra_star_m[1][y * 22 + x] = (uint8_t)(full * 255 / 16);
        ra_star_m[0][y * 22 + x] = (uint8_t)((full - hole) * 255 / 16);
    }
    ra_star_ok = 1;
}
static void ra_star(uint16_t *d, int cx, int cy, int on, int pressed)
{
    if (!ra_star_ok) ra_star_build();
    uint16_t c = on ? rgb(255, 204, 0) : rgb(142, 142, 147);
    if (pressed) c = mix565(c, 0, 130);
    const uint8_t *m = ra_star_m[on ? 1 : 0];
    for (int y = 0; y < 22; y++) {
        int yy = cy - 11 + y; if (yy < 30 || yy >= H) continue;
        for (int x = 0; x < 22; x++) {
            int xx = cx - 11 + x, a = m[y * 22 + x];
            if (a && xx >= 0 && xx < W) d[yy * W + xx] = mix565(c, d[yy * W + xx], a);
        }
    }
}
static uint16_t ra_tile_col(const char *name)
{
    static const uint8_t pal[8][3] = { { 191, 54, 48 }, { 201, 112, 20 }, { 160, 130, 12 }, { 36, 140, 66 }, { 20, 130, 142 }, { 28, 98, 198 }, { 92, 80, 200 }, { 168, 58, 146 } };
    unsigned h = 2166136261u; for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = (h ^ *p) * 16777619u;
    const uint8_t *c = pal[(h >> 8) & 7];
    return rgb(c[0], c[1], c[2]);
}
/* cuadro con la inicial; si es la que suena: barras (sonando) o rueda (conectando) */
static void ra_tile(uint16_t *d, int x, int y, int sz, const struct ra_station *s, int act)
{
    int cx = x + sz / 2, cy = y + sz / 2;
    fill_rrect(d, x, y, sz, sz, sz * 0.22f, act ? FMC_FIELD : ra_tile_col(s->name), 255);
    if (act && ra_np.state == RY_PLAYING) fm_glyph_bars(d, cx, cy, now_ms(), FMC_BLUE);
    else if (act) fm_spinner_at(d, cx, cy);
    else { char ini[8]; ra_initial(s->name, ini); int w = tw_n(ini, 18, -1); draw_text(d, cx - w / 2, cy + 7, ini, 18, 0xFFFF, 0, 30, H); }
}
static void ra_center(uint16_t *d, int y, const char *t, int size, uint16_t col)
{
    int w = tw_n(t, size, -1);
    if (w <= W - 40) draw_text(d, (W - w) / 2, y, t, size, col, 0, 30, H);
    else draw_wrapped(d, 24, y - size, W - 48, t, size, col, size + 6, 1);
}
static const char *ra_err_text(const char *e)
{
    if (!strcmp(e, "net")) return tr("Sin conexión");
    if (!strncmp(e, "http", 4) || !strcmp(e, "nostream")) return tr("Emisora no disponible");
    if (!strcmp(e, "hls") || !strcmp(e, "codec")) return tr("Formato no compatible");
    if (!strcmp(e, "player")) return tr("El reproductor no responde");
    return tr("No se pudo reproducir");
}

static void ra_row(uint16_t *d, int y, const struct ra_station *s, int id)
{
    if (y >= ra_clip1 || y + RA_RH <= ra_clip0) return;
    int cur = ra_is_cur(s), own = (id >> 16) == 4;
    if (fm_pressed(RH_ST, id)) for (int yy = y < 30 ? 30 : y; yy < y + RA_RH && yy < H; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(38, 38, 42);
    ra_tile(d, 16, y + 10, 40, s, cur && ra_active());
    draw_text(d, 68, y + 26, s->name, 15, cur ? FMC_BLUE : 0xFFFF, W - 68 - 52, 30, H);
    char b[160]; int x = 68;
    ra_qlabel(s, b, sizeof b);
    if (b[0] && y + 34 >= 30 && y + 51 < H) x += fm_badge(d, 68, y + 34, b, 0) + 6;
    ra_sub(s, b, sizeof b); if (!b[0]) ra_url_host(s->url, b, sizeof b);
    draw_text(d, x, y + 47, b, 12, FMC_GREY, W - 52 - x, 30, H);
    if (own) fm_glyph_more(d, W - 28, y + 30, fm_pressed(RH_OWNMORE, id) ? rgb(5, 66, 128) : FMC_BLUE);
    else ra_star(d, W - 28, y + 30, ra_is_fav(&ra_lb, s), fm_pressed(RH_STAR, id));
    fm_hline(d, 68, W, y + RA_RH - 1, FMC_SEP);
    ra_hit(0, y, W - 52, y + RA_RH, RH_ST, id);
    ra_hit(W - 52, y, W, y + RA_RH, own ? RH_OWNMORE : RH_STAR, id);
}
/* filas de una lista del directorio y su cola (cargando, error, vacia); devuelve el alto */
static int ra_rows(uint16_t *d, int y, const struct ra_list *l, int slot)
{
    int y0 = y;
    for (int i = 0; i < l->n; i++, y += RA_RH) ra_row(d, y, &l->it[i], (slot << 16) | i);
    if (l->state == 1) { if (y + 30 > ra_clip0 && y + 44 < H) fm_spinner_at(d, W / 2, y + 34); y += 70; }
    else if (l->state == 3) {
        ra_center(d, y + 44, tr("No se pudo cargar"), 15, FMC_GREY);
        const char *t = tr("Reintentar"); int w = tw_n(t, 15, -1);
        draw_text(d, (W - w) / 2, y + 76, t, 15, fm_pressed(RH_RETRY, slot) ? rgb(5, 66, 128) : FMC_BLUE, 0, 30, H);
        ra_hit(W / 2 - w / 2 - 24, y + 52, W / 2 + w / 2 + 24, y + 92, RH_RETRY, slot);
        y += 110;
    } else if (l->state == 2 && !l->n && !l->more) { ra_center(d, y + 50, tr("Sin resultados"), 15, FMC_GREY); y += 90; }
    return y - y0;
}
static int ra_section(uint16_t *d, int y, const char *t)
{
    if (y < H && y + 44 > 30) draw_text(d, 16, y + 32, t, 20, 0xFFFF, W - 32, 30, H);
    return 44;
}
static void ra_card_row(uint16_t *d, int y, int n, int i, const char *t, uint16_t col, int chev, int kind, int idx)
{
    int ry = y + i * 44; if (ry >= ra_clip1 || ry + 44 <= ra_clip0) return;
    if (fm_pressed(kind, idx)) fill_rrect(d, 16, ry, W - 32, 44, i == 0 || i == n - 1 ? 10 : 0, rgb(50, 50, 54), 255);
    draw_text(d, 32, ry + 28, t, 15, col, W - 90, 30, H);
    if (chev) { uint16_t c = rgb(99, 99, 104); fm_line(d, W - 37, ry + 16.5f, W - 31.5f, ry + 22, 1.7f, c); fm_line(d, W - 31.5f, ry + 22, W - 37, ry + 27.5f, 1.7f, c); }
    if (i < n - 1) fm_hline(d, 32, W - 16, ry + 43, FMC_SEP);
    ra_hit(16, ry, W - 16, ry + 44, kind, idx);
}
/* tarjeta de n filas de 44: solo se pinta el trozo visible (la de generos mide casi 1000 px) */
static void ra_card(uint16_t *d, int y, int n)
{
    int h = n * 44, a = y, b = y + h;
    if (b <= 30 || a >= H) return;
    if (a >= -20 && b <= H + 20) { fill_rrect(d, 16, a, W - 32, h, 10, FMC_CARD, 255); return; }
    if (a >= -20) { fill_rrect(d, 16, a, W - 32, 40, 10, FMC_CARD, 255); a += 20; }
    if (b <= H + 20) { fill_rrect(d, 16, b - 40, W - 32, 40, 10, FMC_CARD, 255); b -= 20; }
    for (int yy = a < 30 ? 30 : a; yy < b && yy < H; yy++) for (int x = 16; x < W - 16; x++) d[yy * W + x] = FMC_CARD;
}

/* ------------------------------------------------------------------ pestañas de la raiz */
static int ra_draw_favs(uint16_t *d, int y)
{
    int y0 = y;
    if (!ra_lb.nfav && !ra_lb.nrec && !ra_lb.nown) {
        if (y + 40 < H) draw_wrapped(d, 28, y + 24, W - 56, tr("Aún no tienes emisoras. Explora el directorio y toca la estrella, o añade una por su dirección."), 14, FMC_GREY, 20, 1);
        y += 104;
    }
    if (ra_lb.nfav) {
        y += ra_section(d, y, tr("Favoritas"));
        for (int i = 0; i < ra_lb.nfav; i++, y += RA_RH) ra_row(d, y, &ra_lb.fav[i], (2 << 16) | i);
    }
    if (ra_lb.nrec) {
        y += ra_section(d, y, tr("Recientes"));
        for (int i = 0; i < ra_lb.nrec; i++, y += RA_RH) ra_row(d, y, &ra_lb.rec[i], (3 << 16) | i);
    }
    y += ra_section(d, y, tr("Mis emisoras"));
    for (int i = 0; i < ra_lb.nown; i++, y += RA_RH) ra_row(d, y, &ra_lb.own[i], (4 << 16) | i);
    if (y < ra_clip1 && y + 56 > ra_clip0) {         /* fila "añadir" */
        uint16_t c = fm_pressed(RH_ADD, 0) ? rgb(5, 66, 128) : FMC_BLUE;
        fill_rrect(d, 16, y + 8, 40, 40, 9, FMC_CARD, 255);
        fm_line(d, 29, y + 28, 43, y + 28, 2.2f, c); fm_line(d, 36, y + 21, 36, y + 35, 2.2f, c);
        draw_text(d, 68, y + 33, tr("Añadir emisora…"), 15, c, W - 84, 30, H);
        ra_hit(0, y, W, y + 56, RH_ADD, 0);
    }
    y += 56;
    return y - y0;
}
static int ra_draw_explore(uint16_t *d, int y)
{
    int y0 = y;
    y += 8;
    ra_card(d, y, 3);
    ra_card_row(d, y, 3, 0, tr("Más escuchadas"), 0xFFFF, 1, RH_NAV, 0);
    ra_card_row(d, y, 3, 1, ra_cf.cname[0] ? ra_cf.cname : ra_cf.cc, 0xFFFF, 1, RH_NAV, 1);
    ra_card_row(d, y, 3, 2, tr("Cambiar país…"), fm_pressed(RH_NAV, 2) ? rgb(5, 66, 128) : FMC_BLUE, 0, RH_NAV, 2);
    y += 3 * 44 + 6;
    y += ra_section(d, y, tr("Géneros"));
    ra_card(d, y, RA_NGENRE);
    for (int i = 0; i < RA_NGENRE; i++) ra_card_row(d, y, RA_NGENRE, i, tr(RA_GENRES[i].key), 0xFFFF, 1, RH_GENRE, i);
    y += RA_NGENRE * 44 + 14;
    if (y < H && y + 60 > 30) draw_wrapped(d, 16, y, W - 32, tr("RA_NOTE"), 12, rgb(110, 110, 116), 16, 1);
    y += draw_wrapped(0, 16, y, W - 32, tr("RA_NOTE"), 12, 0, 16, 0) + 8;
    return y - y0;
}
static int ra_draw_search(uint16_t *d, int y)
{
    const struct ra_list *l = &ra_ls[0];
    if (!ra_q[0] || (!l->state && !l->n)) { ra_center(d, y + 56, tr("Busca una emisora por su nombre"), 14, rgb(110, 110, 116)); return 100; }
    return ra_rows(d, y, l, 0);
}

/* ------------------------------------------------------------------ tarjeta de lo que suena */
static void ra_status(char *o, int cap, uint16_t *col)
{
    *col = FMC_GREY;
    switch (ra_np.state) {
    case RY_CONNECTING: snprintf(o, cap, "%s", tr("Conectando…")); break;
    case RY_BUFFERING: snprintf(o, cap, tr("Cargando… %d%%"), ra_np.pct); break;
    case RY_PLAYING: snprintf(o, cap, "%s", ra_np.title[0] ? ra_np.title : tr("En directo")); break;
    case RY_ERROR: snprintf(o, cap, "%s", ra_err_text(ra_np.err)); *col = rgb(255, 69, 58); break;
    default: snprintf(o, cap, "%s", tr("Detenida"));
    }
}
static void ra_mini(uint16_t *d)
{
    if (!ra_np.have) return;
    const struct ra_station *s = &ra_np.cur; int y = RA_MINI_Y, act = ra_active(), own = !s->uuid[0] && ra_is_own(&ra_lb, s);
    for (int yy = RA_MINI_TOP - 14; yy < RA_MINI_TOP; yy++) { int k = 32 * (RA_MINI_TOP - yy) / 15; uint16_t *r = d + yy * W; for (int x = 0; x < W; x++) r[x] = dim565(r[x], k); }
    for (int yy = RA_MINI_TOP; yy < H; yy++) memset(d + yy * W, 0, W * 2);
    fill_rrect(d, 8, y, W - 16, 56, 14, rgb(36, 36, 40), 255);
    ra_tile(d, 18, y + 10, 36, s, act);
    int tw = W - 62 - (own ? 66 : 108);
    draw_text(d, 62, y + 24, s->name, 14, 0xFFFF, tw, 30, H);
    char b[300]; uint16_t col; ra_status(b, sizeof b, &col);
    draw_text(d, 62, y + 42, b, 12, col, tw, 30, H);
    if (!own) { ra_star(d, W - 84, y + 28, ra_is_fav(&ra_lb, s), fm_pressed(RH_MSTAR, 0)); }
    fill_circle(d, W - 38, y + 28, 17, fm_pressed(RH_MPLAY, 0) ? rgb(84, 84, 90) : rgb(58, 58, 62), 255);
    if (act) fill_rrect(d, W - 44, y + 22, 12, 12, 2.5f, 0xFFFF, 255);
    else fm_glyph_play(d, W - 37, y + 28, 7, 0xFFFF);
    fm_hit(8, y - 4, W - 8, y + 60, RH_MINI, 0);
    if (!own) fm_hit(W - 106, y - 4, W - 62, y + 60, RH_MSTAR, 0);
    fm_hit(W - 62, y - 4, W, y + 60, RH_MPLAY, 0);
}

/* ------------------------------------------------------------------ paginas */
static void ra_seg(uint16_t *d, int y, float a)
{
    if (a < 0.02f) return;
    int al = (int)(255 * a), pw = (W - 32 - 4) / 2;
    double t = (now_ms() - ra_tab_t0) / 200.0; if (t > 1) t = 1; if (t < 0) t = 0;
    float f = (float)ease_out(t); if (!ra_tab) f = 1 - f;
    fill_rrect(d, 16, y, W - 32, 32, 9, FMC_FIELD, al);
    fill_rrect(d, 18 + (int)(pw * f), y + 2, pw, 28, 7, rgb(99, 99, 104), al);
    for (int k = 0; k < 2; k++) {
        const char *s = k ? tr("Explorar") : tr("Favoritas"); int w = tw_n(s, 13, -1); if (w > pw - 8) w = pw - 8;
        draw_text(d, 18 + k * pw + (pw - w) / 2, y + 21, s, 13, mix565(0xFFFF, 0, al), pw - 8, 30, H);
    }
    if (a > 0.9f) { fm_hit(16, y - 4, W / 2, y + 36, RH_TAB, 0); fm_hit(W / 2, y - 4, W - 16, y + 36, RH_TAB, 1); }
}
static void ra_render_root(uint16_t *d)
{
    float e = ra_edit_e, pos = ra_pos[RG_ROOT], c = ra_hdr_c();
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int hb = ra_hdr_bot(), by = ra_bar_y(), y0 = ra_top0(RG_ROOT) - (int)pos, y = y0;
    ra_clip0 = hb; ra_clip1 = ra_bottom();
    if (e > 0.5f) y += ra_draw_search(d, y); else if (ra_tab == 0) y += ra_draw_favs(d, y); else y += ra_draw_explore(d, y);
    ra_ch[RG_ROOT] = y - y0 + 16;

    /* cabecera, encima de la lista */
    for (int yy = 30; yy < hb && yy < H; yy++) memset(d + yy * W, 0, W * 2);
    float ta = (1 - e) * (1 - c / 30); if (ta < 0) ta = 0;
    if (ta > 0.02f) draw_text(d, 16, 72 - (int)c - (int)(40 * e), "Radio", 28, mix565(0xFFFF, 0, (unsigned)(255 * ta)), W - 32, 30, H);
    fm_chrome_status(d);
    int bx1 = W - 16 - (int)(70 * e);
    fill_rrect(d, 16, by, bx1 - 16, 36, 10, FMC_FIELD, 255);
    fm_glyph_search(d, 32, by + 18, FMC_GREY);
    if (ra_q[0]) draw_text(d, 46, by + 24, ra_q, 16, 0xFFFF, bx1 - 46 - 34, 0, H);
    else draw_text(d, 46, by + 24, tr("Buscar emisoras"), 16, FMC_GREY, bx1 - 46 - 14, 0, H);
    if (ra_editing && ((int)(now_ms() / 530) & 1) == 0) {
        int cx = 46 + tw_n(ra_q, 16, -1); if (cx > bx1 - 34) cx = bx1 - 34;
        for (int yy = by + 8; yy < by + 28; yy++) { d[yy * W + cx] = FMC_BLUE; d[yy * W + cx + 1] = FMC_BLUE; }
    }
    fm_hit(16, by - 4, bx1 - 30, by + 40, FH_BAR, 0);
    if (ra_q[0]) { fill_circle(d, bx1 - 16, by + 18, 8, rgb(99, 99, 104), 255); fm_glyph_x(d, bx1 - 16, by + 18, 3, FMC_FIELD); fm_hit(bx1 - 34, by - 4, bx1 + 2, by + 40, FH_CLEAR, 0); }
    if (e > 0.01f) {
        draw_text(d, bx1 + 10, by + 24, tr("Cancelar"), 16, mix565(fm_pressed(FH_CANCEL, 0) ? rgb(5, 66, 128) : FMC_BLUE, 0, (unsigned)(255 * e)), 0, 0, H);
        if (e > 0.5f) fm_hit(bx1, by - 4, W, by + 40, FH_CANCEL, 0);
    }
    ra_seg(d, by + 44, 1 - e);
    if (pos > 2) fm_hline(d, 0, W, hb - 1, FMC_SEP);
    ra_mini(d);
    draw_toast(d);
}
static void ra_render_list(uint16_t *d)
{
    const struct ra_list *l = &ra_ls[1]; float pos = ra_pos[RG_LIST];
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int y0 = 74 - (int)pos, y = y0;
    ra_clip0 = 74; ra_clip1 = ra_bottom();
    draw_text(d, 16, y + 34, l->title, 28, 0xFFFF, W - 32, 30, H); y += 52;
    y += ra_rows(d, y, l, 1);
    ra_ch[RG_LIST] = y - y0 + 16;
    fm_chrome_status(d);
    fm_navbar(d, "Radio", l->title, pos > 44 ? 1 : pos > 24 ? (pos - 24) / 20 : 0);
    ra_mini(d);
    draw_toast(d);
}
static void ra_render_ctry(uint16_t *d)
{
    float pos = ra_pos[RG_CTRY];
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int y0 = 74 - (int)pos, y = y0;
    ra_clip0 = 74; ra_clip1 = ra_bottom();
    draw_text(d, 16, y + 34, tr("País"), 28, 0xFFFF, W - 32, 30, H); y += 52;
    for (int i = 0; i < ra_nct; i++, y += 44) {
        if (y >= ra_clip1 || y + 44 <= ra_clip0) continue;
        const struct ra_country *c = &ra_ct[i]; int sel = !strcmp(c->cc, ra_cf.cc);
        if (fm_pressed(RH_CTRY, i)) for (int yy = y < 30 ? 30 : y; yy < y + 44 && yy < H; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(38, 38, 42);
        char n[16]; snprintf(n, sizeof n, "%d", c->n); int nw = tw_n(n, 12, -1), rx = W - 16 - (sel ? 30 : 0);
        draw_text(d, 16, y + 28, c->name, 15, sel ? FMC_BLUE : 0xFFFF, rx - nw - 28, 30, H);
        draw_text(d, rx - nw, y + 27, n, 12, FMC_GREY, 0, 30, H);
        if (sel) fm_glyph_check(d, W - 26, y + 22, FMC_BLUE);
        fm_hline(d, 16, W, y + 43, FMC_SEP);
        ra_hit(0, y, W, y + 44, RH_CTRY, i);
    }
    if (ra_ct_state == 1) { if (y + 44 < H) fm_spinner_at(d, W / 2, y + 34); y += 70; }
    else if (ra_ct_state == 3) {
        ra_center(d, y + 44, tr("No se pudo cargar"), 15, FMC_GREY);
        const char *t = tr("Reintentar"); int w = tw_n(t, 15, -1);
        draw_text(d, (W - w) / 2, y + 76, t, 15, fm_pressed(RH_RETRY, 2) ? rgb(5, 66, 128) : FMC_BLUE, 0, 30, H);
        ra_hit(W / 2 - w / 2 - 24, y + 52, W / 2 + w / 2 + 24, y + 92, RH_RETRY, 2);
        y += 110;
    }
    ra_ch[RG_CTRY] = y - y0 + 16;
    fm_chrome_status(d);
    fm_navbar(d, "Radio", tr("País"), pos > 44 ? 1 : pos > 24 ? (pos - 24) / 20 : 0);
    ra_mini(d);
    draw_toast(d);
}
static void ra_render_page(uint16_t *d, int pg, int rec)
{
    int keep = fm_rec; fm_rec = rec; if (rec) fm_nhit = 0;
    switch (pg) {
    case RG_ROOT: ra_render_root(d); break;
    case RG_LIST: ra_render_list(d); break;
    case RG_CTRY: ra_render_ctry(d); break;
    }
    fm_rec = keep;
}
static void ra_render(void) { ra_render_page(fr0, ra_stack[ra_sp], 1); if (ra_editing) kb_draw(fr0, -1); }
