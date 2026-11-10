/* cc.inc.c - Centro de control de HiByOS (estilo iOS 26).
 *
 * Se abre deslizando desde arriba (o desde el borde configurado) siguiendo al dedo:
 * la pantalla de debajo se desenfoca y oscurece y los controles aparecen con ella.
 *   - Reproduciendo (2x2): abre el reproductor.
 *   - Brillo (1x2): deslizador vertical (sysfs del backlight).
 *   - Botones configurables (Personalizar > Centro de control): Solo USB, modo de energia,
 *     DAC USB, panel de HiBy, Inalambrico, filtro del DAC, Archivos, Personalizar.
 * Se cierra deslizando hacia arriba, tocando fuera o con la barra de inicio.
 * Rendimiento: el panel completo (fondo desenfocado + controles) se compone UNA vez en
 * img_base y cada fotograma es solo una mezcla lineal con la pantalla de debajo. */

enum { CT_NOCHARGE, CT_POWER, CT_DAC, CT_HIBY, CT_WIRELESS, CT_FILTER, CT_FILES, CT_SETTINGS, CT_N };
static const char *const ct_id[CT_N] = { "nocharge", "power", "dac", "hiby", "wireless", "filter", "files", "settings" };
static const char *const ct_name[CT_N] = { "Solo USB", "Modo de energía", "DAC USB", "Panel de HiBy", "Inalámbrico", "Filtro digital", "Archivos", "Personalizar" };
static int cc_tiles[CT_N] = { CT_NOCHARGE, CT_POWER, CT_DAC, CT_HIBY, CT_WIRELESS, CT_FILTER, CT_FILES, CT_SETTINGS };
static int cc_ntiles = CT_N;

static void cc_conf_line(char *rest)      /* "cc id id id..." de launcher.conf */
{
    cc_ntiles = 0; char *t;
    while ((t = strtok(rest, " \t\r\n"))) { rest = 0;
        for (int i = 0; i < CT_N; i++) if (!strcmp(t, ct_id[i])) { int dup = 0; for (int k = 0; k < cc_ntiles; k++) dup |= cc_tiles[k] == i; if (!dup) cc_tiles[cc_ntiles++] = i; } }
}
static void cc_conf_save(FILE *f) { fprintf(f, "cc"); for (int i = 0; i < cc_ntiles; i++) fprintf(f, " %s", ct_id[cc_tiles[i]]); fprintf(f, "\n"); }

/* ---- geometria ---- */
#define CC_M 14
#define CC_T 64
#define CC_G 12
#define CC_Y 68
static void cc_tile_rect(int slot, int *x, int *y)
{
    /* huecos 0 y 1: columna derecha junto a los modulos grandes; despues filas de 4 */
    if (slot < 2) { *x = CC_M + 3 * (CC_T + CC_G); *y = CC_Y + slot * (CC_T + CC_G); return; }
    int s = slot - 2; *x = CC_M + (s % 4) * (CC_T + CC_G); *y = CC_Y + 2 * (CC_T + CC_G) + (s / 4) * (CC_T + CC_G);
}
#define NP_X CC_M
#define NP_W (2 * CC_T + CC_G)
#define BR_X (CC_M + 2 * (CC_T + CC_G))
#define BR_H (2 * CC_T + CC_G)

static struct sprite cc_ic[12]; static int cc_ic_ok;
static const char *const cc_ic_name[12] = { "plug", "leaf", "gauge", "bolt", "usb", "sliders", "wifi", "wave", "folder", "gear", "sun", 0 };
static const struct sprite *cc_icon(const char *n) { for (int i = 0; cc_ic_name[i]; i++) if (!strcmp(n, cc_ic_name[i])) return &cc_ic[i]; return 0; }

static int cc_filter_short(void) { return au_filt_cur; }
static const char *cc_caption; static double cc_caption_t;

static uint16_t blend565(uint16_t a, uint16_t b, int k)    /* k 0..32: 0 = a, 32 = b */
{
    int r = ((a >> 11) * (32 - k) + (b >> 11) * k) >> 5, g = (((a >> 5) & 63) * (32 - k) + ((b >> 5) & 63) * k) >> 5, bl = ((a & 31) * (32 - k) + (b & 31) * k) >> 5;
    return (uint16_t)(r << 11 | g << 5 | bl);
}
/* desenfoque fuerte barato: media de bloques 8x8 (40x60) + bilineal, oscurecido y algo desaturado */
static void cc_blur(const uint16_t *src, uint16_t *dst)
{
    /* enteros: en MIPS la version en coma flotante tardaba ~150 ms */
    static int acc[61][41][3];
    for (int by = 0; by < 60; by++) for (int bx = 0; bx < 40; bx++) {
        int r = 0, g = 0, b = 0;
        for (int y = 0; y < 8; y += 2) for (int x = 0; x < 8; x += 2) { uint16_t q = src[(by * 8 + y) * W + bx * 8 + x]; r += q >> 11; g += (q >> 5) & 63; b += q & 31; }
        r = r * 255 / 496; g = g * 255 / 1008; b = b * 255 / 496;          /* 0..255 */
        int l = (77 * r + 151 * g + 28 * b) >> 8;
        acc[by][bx][0] = ((r * 166 + l * 90) >> 8) * 107 >> 8;            /* 65 % color, x0.42 */
        acc[by][bx][1] = ((g * 166 + l * 90) >> 8) * 107 >> 8;
        acc[by][bx][2] = ((b * 166 + l * 90) >> 8) * 107 >> 8;
    }
    for (int by = 0; by < 60; by++) for (int k = 0; k < 3; k++) acc[by][40][k] = acc[by][39][k];
    for (int bx = 0; bx <= 40; bx++) for (int k = 0; k < 3; k++) acc[60][bx][k] = acc[59][bx][k];
    static short xi[W], xw[W];
    for (int x = 0; x < W; x++) { int f = (x * 2 + 1) * 16 - 128; if (f < 0) f = 0; xi[x] = f >> 8; xw[x] = f & 255; }   /* 8.8 */
    for (int y = 0; y < H; y++) {
        int f = (y * 2 + 1) * 16 - 128; if (f < 0) f = 0; int y0 = f >> 8, wy = f & 255;
        const int (*r0)[3] = acc[y0], (*r1)[3] = acc[y0 + 1];
        uint16_t *d = dst + y * W;
        for (int x = 0; x < W; x++) {
            int i = xi[x], wx = xw[x], c[3];
            for (int k = 0; k < 3; k++) {
                int top = r0[i][k] * (256 - wx) + r0[i + 1][k] * wx, bot = r1[i][k] * (256 - wx) + r1[i + 1][k] * wx;
                c[k] = (top * (256 - wy) + bot * wy) >> 16;
            }
            d[x] = rgb(c[0], c[1], c[2]);
        }
    }
}

static void cc_tile(uint16_t *d, int x, int y, int tile)
{
    int on = 0; uint16_t acc = rgb(10, 132, 255); const char *ic = "gear";
    switch (tile) {
    case CT_NOCHARGE: on = pw_nocharge; acc = rgb(48, 209, 88); ic = "plug"; break;
    case CT_POWER: on = pw_mode != PW_NORMAL; acc = pw_mode == PW_SAVE ? rgb(255, 204, 0) : rgb(255, 69, 58); ic = pw_mode == PW_SAVE ? "leaf" : pw_mode == PW_PERF ? "bolt" : "gauge"; break;
    case CT_DAC: ic = "usb"; break;
    case CT_HIBY: ic = "sliders"; break;
    case CT_WIRELESS: ic = "wifi"; break;
    case CT_FILTER: on = cc_filter_short() > 0; acc = rgb(191, 90, 242); ic = "wave"; break;
    case CT_FILES: ic = "folder"; break;
    case CT_SETTINGS: ic = "gear"; break;
    }
    if (on) fill_rrect(d, x, y, CC_T, CC_T, CC_T / 2.f, acc, 255);                 /* iOS 26: circulo de color */
    else fill_rrect(d, x, y, CC_T, CC_T, CC_T / 2.f, rgb(255, 255, 255), 46);      /* cristal */
    const struct sprite *s = cc_icon(ic);
    if (s && s->w) blit(d, s, x + (CC_T - s->w) / 2, y + (CC_T - s->h) / 2, 32, 255);
}
static int cc_bl_level(void) { int b = sysfs_get(BL_PATH, 60); return b < 0 ? 60 : b; }
/* compone el panel completo en dst a partir del fondo desenfocado */
static const uint16_t *cc_under;
static void cc_render(uint16_t *dst, const uint16_t *blur, int pressed)
{
    memcpy(dst, blur, HM_FRAME_BYTES);
    /* barra de estado nitida encima (como iOS): la del reproductor es negra con glifos
     * claros, asi que su brillo sirve de alfa premultiplicado sobre el desenfoque */
    if (cc_under && has_statusbar(fr1)) for (int i = 0; i < 30 * W; i++) {
        uint16_t q = fr1[i]; int r = q >> 11, g = (q >> 5) & 63, b = q & 31, a = r * 2 > g ? r * 2 : g; if (b * 2 > a) a = b * 2;
        if (!a) continue;
        uint16_t bg = dst[i]; int na = 63 - a;
        int br = (bg >> 11) * na / 63 + r, bgc = ((bg >> 5) & 63) * na / 63 + g, bb = (bg & 31) * na / 63 + b;
        dst[i] = (uint16_t)((br > 31 ? 31 : br) << 11 | (bgc > 63 ? 63 : bgc) << 5 | (bb > 31 ? 31 : bb));
    }
    /* Reproduciendo */
    fill_rrect(dst, NP_X, CC_Y, NP_W, BR_H, 22, rgb(255, 255, 255), pressed == 100 ? 80 : 46);
    int a = app_by_id("nowplaying");
    if (a >= 0) blit_scaled(dst, &apps[a].icon[2], NP_X + 16, CC_Y + 16, 44, 44, 255);
    draw_text(dst, NP_X + 14, CC_Y + 92, tr("Reproduciendo"), 15, 0xFFFF, NP_W - 22, 0, H);
    draw_text(dst, NP_X + 14, CC_Y + 113, tr(music_on ? "Sonando" : "En pausa"), 13, rgb(200, 200, 206), NP_W - 22, 0, H);
    /* brillo */
    int bl = cc_bl_level(); float lv = bl / 101.f; if (lv > 1) lv = 1;
    fill_rrect(dst, BR_X, CC_Y, CC_T, BR_H, 22, rgb(255, 255, 255), 46);
    int fh = (int)(BR_H * lv); if (fh > 0) {
        /* relleno blanco recortado por abajo: se dibuja el redondeado y se tapa lo de arriba */
        static uint16_t tmp[CC_T * (2 * CC_T + CC_G)];
        for (int y = 0; y < BR_H; y++) memcpy(tmp + y * CC_T, dst + (CC_Y + y) * W + BR_X, CC_T * 2);
        fill_rrect(dst, BR_X, CC_Y, CC_T, BR_H, 22, rgb(245, 245, 247), 255);
        for (int y = 0; y < BR_H - fh; y++) memcpy(dst + (CC_Y + y) * W + BR_X, tmp + y * CC_T, CC_T * 2);
    }
    const struct sprite *sun = cc_icon("sun");
    if (sun && sun->w) {
        int sx = BR_X + (CC_T - sun->w) / 2, sy = CC_Y + BR_H - sun->h - 14;
        if (fh > 26) { /* sobre blanco: icono gris oscuro */
            for (int y = 0; y < sun->h; y++) for (int x = 0; x < sun->w; x++) { int al = sun->a[y * sun->w + x]; if (al) { uint16_t *p = dst + (sy + y) * W + sx + x; *p = blend565(*p, rgb(60, 60, 64), al * 32 / 255); } }
        } else blit(dst, sun, sx, sy, 32, 255);
    }
    for (int i = 0; i < cc_ntiles; i++) { int x, y; cc_tile_rect(i, &x, &y); cc_tile(dst, x, y, cc_tiles[i]); if (pressed == i) fill_rrect(dst, x, y, CC_T, CC_T, CC_T / 2.f, 0xFFFF, 50); }
    /* rotulo del ultimo cambio (como el aviso de iOS al tocar un control) */
    if (cc_caption && now_ms() - cc_caption_t < 1800) {
        int w = tw_n(cc_caption, 13, -1) + 28; if (w > W - 40) w = W - 40;
        fill_rrect(dst, (W - w) / 2, 36, w, 24, 12, rgb(255, 255, 255), 56);
        draw_text(dst, (W - w) / 2 + 14, 53, cc_caption, 13, 0xFFFF, w - 28, 0, H);
    }
    /* barra de inicio (cerrar) */
    fill_rrect(dst, W / 2 - 56, H - 11, 112, 5, 2.5f, 0xFFFF, 200);
}
/* -1 nada, 100 Reproduciendo, 101 brillo, 0.. boton */
static int cc_hit(int x, int y)
{
    if (x >= NP_X && x < NP_X + NP_W && y >= CC_Y && y < CC_Y + BR_H) return 100;
    if (x >= BR_X && x < BR_X + CC_T && y >= CC_Y - 6 && y < CC_Y + BR_H + 6) return 101;
    for (int i = 0; i < cc_ntiles; i++) { int tx, ty; cc_tile_rect(i, &tx, &ty); if (x >= tx - 4 && x < tx + CC_T + 4 && y >= ty - 4 && y < ty + CC_T + 4) return i; }
    return -1;
}
static void cc_mix(const uint16_t *under, const uint16_t *panel, double e)
{
    int k = (int)(e * 32 + .5); if (k <= 0) { memcpy(fr0, under, HM_FRAME_BYTES); return; } if (k >= 32) { memcpy(fr0, panel, HM_FRAME_BYTES); return; }
    for (int i = 0; i < W * H; i++) fr0[i] = blend565(under[i], panel[i], k);
}
static void cc_animate(const uint16_t *under, const uint16_t *panel, double from, double to, int ms)
{
    double t0 = now_ms();
    for (;;) { double t = (now_ms() - t0) / ms; if (t > 1) t = 1; cc_mix(under, panel, from + (to - from) * ease_out(t)); present(); if (t >= 1) break; }
}
static void cc_set_caption(const char *s) { cc_caption = s; cc_caption_t = now_ms(); }
static void launch_app(int a, int ix, int iy);
/* abre una app del inicio por su id (desde el launcher, con la animacion del icono) */
static void cc_open_app(const char *id)
{
    int a = app_by_id(id); if (a < 0) return;
    if (!launcher_on) { nav_start(NAV_HOME); nav_wait(3000); if (!launcher_resume()) { cmd('R'); return; } }
    int ix = W / 2 - L.icon / 2, iy = H / 2 - L.icon / 2;
    for (int i = 0; i < L.dn; i++) if (L.dock[i] == a) dock_xy(i, L.dn, &ix, &iy);
    for (int i = 0; i < L.pn[cur_page]; i++) if (L.page[cur_page][i] == a) cell_xy(i, &ix, &iy);
    launch_app(a, ix, iy);
}

/* start_y >= 0: se abre siguiendo al dedo desde ahi; < 0: se abre solo (animado) */
static void cc_open(int start_y)
{
    if (!cc_ic_ok) { for (int i = 0; cc_ic_name[i]; i++) { char n[32]; snprintf(n, sizeof n, "cc_%s", cc_ic_name[i]); load_sprite(n, &cc_ic[i]); } cc_ic_ok = 1; }
    int from_launcher = launcher_on;
    uint16_t *under = img_under, *panel = img_base, *blur = img_sheet;
    if (from_launcher) render_launcher(under, now_ms());
    else {
        memcpy(under, visible(), HM_FRAME_BYTES);
        if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
            memcpy(under + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
        st->ov_on = 0; ov_cur = 0;
        cmd('C');
        double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
        if (!st->capture) return;
    }
    have_under = 0;
    double tb = now_ms();
    cc_blur(under, blur); cc_under = under;
    double tb2 = now_ms();
    cc_caption = 0;
    cc_render(panel, blur, -1);
    lg("centro de control: desenfoque %.0f ms, panel %.0f ms", tb2 - tb, now_ms() - tb2);
    double e = 0;
    struct pkt p;
    if (start_y >= 0) {           /* tirar hacia abajo */
        double vel = 0, lt = now_ms(); int ly = start_y;
        while (cur_down) {
            int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
            double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lt = t; }
            e = (cur_y - start_y) / 260.0; if (e < 0) e = 0; if (e > 1) e = 1;
            cc_mix(under, panel, e); present();
            if (got && !p.down) break;
        }
        lg("cc: suelta e=%.2f vel=%.2f", e, vel);
        if (e < 0.3 && vel < 0.3) { cc_animate(under, panel, e, 0, 160); goto out; }
    }
    cc_animate(under, panel, e, 1, start_y >= 0 ? 200 : 260);
    for (;;) {
        if (!read_pkt(&p, cc_caption && now_ms() - cc_caption_t < 1900 ? 100 : 1000)) {
            if (cc_caption && now_ms() - cc_caption_t >= 1800) { cc_caption = 0; cc_render(panel, blur, -1); memcpy(fr0, panel, HM_FRAME_BYTES); present(); }
            continue;
        }
        if (!p.down) continue;
        int sx = p.x, sy = p.y, hit = cc_hit(sx, sy);
        lg("cc: toque %d,%d -> %d", sx, sy, hit);
        if (hit == 101) {                 /* brillo: arrastrar */
            int b0 = cc_bl_level(), y0 = sy;
            while (cur_down) {
                int nb = b0 + (y0 - cur_y) * 101 / BR_H; if (nb < 4) nb = 4; if (nb > 101) nb = 101;
                if (pw_mode == PW_SAVE && nb > 40) nb = 40;
                sysfs_put(BL_PATH, nb);
                cc_render(panel, blur, -1); memcpy(fr0, panel, HM_FRAME_BYTES); present();
                read_pkt(&p, 40);
            }
            continue;
        }
        if (hit >= 0) { cc_render(fr0, blur, hit); present(); }
        int moved = 0;
        while (cur_down && read_pkt(&p, 3000)) if (abs(cur_y - sy) > 14 || abs(cur_x - sx) > 14) moved = 1;
        int up = sy - cur_y;
        if (moved && (up > 30 || sy >= 450)) break;          /* deslizar hacia arriba: cerrar */
        if (moved) { cc_render(panel, blur, -1); memcpy(fr0, panel, HM_FRAME_BYTES); present(); continue; }
        if (hit < 0) break;                                   /* toque fuera: cerrar */
        if (hit == 100) { cc_animate(under, panel, 1, 0, 180); cc_open_app("nowplaying"); return; }
        int tile = cc_tiles[hit];
        switch (tile) {
        case CT_NOCHARGE:
            pw_set_nocharge(!pw_nocharge);
            cc_set_caption(tr(pw_nocharge ? "Solo USB: la batería no se carga" : "Carga de la batería activada")); break;
        case CT_POWER:
            pw_mode = (pw_mode + 1) % 3; pw_apply();
            cc_set_caption(tr(pw_mode == PW_SAVE ? "Modo Ahorro" : pw_mode == PW_PERF ? "Modo Rendimiento" : "Modo Normal")); break;
        case CT_FILTER: {
            int n; const int *vv = filt_vals(&n), cur = au_filt_cur; int i = 0;
            for (int k = 0; k < n; k++) if (vv[k] == cur) i = k;
            au_filt_i = (i + 1) % n; apply_filter();
            cc_set_caption(tr(n == 7 ? O_FILT8[au_filt_i] : O_FILT2[au_filt_i])); break; }
        case CT_HIBY:
            cc_animate(under, panel, 1, 0, 160);
            if (!from_launcher) cmd('R'); else launcher_present();
            open_cc(); return;
        case CT_DAC: cc_animate(under, panel, 1, 0, 160); cc_open_app("dac"); return;
        case CT_WIRELESS: cc_animate(under, panel, 1, 0, 160); cc_open_app("wireless"); return;
        case CT_FILES: cc_animate(under, panel, 1, 0, 160); cc_open_app("files"); return;
        case CT_SETTINGS: cc_animate(under, panel, 1, 0, 160); cc_open_app("personalize"); return;
        }
        cc_render(panel, blur, -1); memcpy(fr0, panel, HM_FRAME_BYTES); present();
    }
    cc_animate(under, panel, 1, 0, 200);
out:
    if (from_launcher) launcher_present(); else cmd('R');
}
