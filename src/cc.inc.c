/* cc.inc.c - Centro de control de ByOS (estilo iOS 26).
 *
 * Se abre deslizando desde arriba (o desde el borde configurado) siguiendo al dedo:
 * la pantalla de debajo se desenfoca y oscurece y los controles aparecen con ella.
 *   - Reproduciendo (2x2): abre el reproductor.
 *   - Brillo (1x2): deslizador vertical (sysfs del backlight).
 *   - Botones configurables (Ajustes > Centro de control): modo avion, control de ruido de
 *     los auriculares conectados, Solo USB, modo de energia, DAC USB (Ajustes > USB), Bluetooth,
 *     Wi-Fi, filtro del DAC, Archivos, Ajustes. (Modo avion y control de ruido: ccair.inc.c)
 * Se cierra deslizando hacia arriba, tocando fuera o con la barra de inicio.
 * Rendimiento: el panel completo (fondo desenfocado + controles) se compone UNA vez en
 * img_base y cada fotograma es solo una mezcla lineal con la pantalla de debajo. */

/* los tipos de control (CT_*) y CC_CELLS estan en settings_app.inc.c (se incluye antes) */
static const char *const ct_id[CT_N] = { "nocharge", "power", "dac", "bt", "wireless", "filter", "files", "settings", "airplane", "noise", "gain", "mseb", "eq", "sleep" };
static const char *const ct_name[CT_N] = { "Solo USB", "Modo de energía", "DAC USB", "Bluetooth", "Wi-Fi", "Filtro digital", "Archivos", "Ajustes", "Modo avión", "Control de ruido",
                                          "Ganancia", "MSEB", "Ecualizador", "Temporizador" };
/* por defecto: modo avion y control de ruido en la columna derecha, junto a los modulos grandes */
static int cc_tiles[CT_N] = { CT_AIRPLANE, CT_NOISE, CT_BT, CT_NOCHARGE, CT_POWER, CT_DAC, CT_WIRELESS, CT_FILTER, CT_FILES, CT_SETTINGS };
static int cc_ntiles = 10;                 /* los de antes; los nuevos se anaden a mano */

static void cc_conf_line(char *rest)      /* "cc id id id..." de launcher.conf */
{
    cc_ntiles = 0; char *t; int v2 = 0;
    while ((t = strtok(rest, " \t\r\n"))) { rest = 0;
        if (!strcmp(t, "v2")) { v2 = 1; continue; }
        if (!strcmp(t, "hiby")) t = "bt";        /* el boton del panel antiguo de HiBy ya no existe */
        for (int i = 0; i < CT_N; i++) if (!strcmp(t, ct_id[i])) { int dup = 0; for (int k = 0; k < cc_ntiles; k++) dup |= cc_tiles[k] == i; if (!dup) cc_tiles[cc_ntiles++] = i; } }
    /* una configuracion anterior a "v2" no conocia el modo avion ni el control de ruido: entran
     * los primeros (una sola vez; despues se pueden quitar en Ajustes > Centro de control) */
    if (!v2) for (int n = CT_NOISE; n >= CT_AIRPLANE; n--) {
        int have = 0; for (int k = 0; k < cc_ntiles; k++) have |= cc_tiles[k] == n;
        if (!have && cc_ntiles < CT_N) { memmove(cc_tiles + 1, cc_tiles, sizeof(int) * (size_t)cc_ntiles); cc_tiles[0] = n; cc_ntiles++; }
    }
}
static void cc_conf_save(FILE *f) { fprintf(f, "cc v2"); for (int i = 0; i < cc_ntiles; i++) fprintf(f, " %s", ct_id[cc_tiles[i]]); fprintf(f, "\n"); }

/* ---- geometria ---- */
#define CC_M 14
#define CC_T 64
#define CC_G 12
#define CC_Y 68
/* posicion y ancho del control i: huecos 0 y 1 en la columna derecha junto a los modulos grandes;
 * despues filas de 4. El modulo de Bluetooth ocupa 2 celdas (nunca en la columna derecha ni
 * partido entre dos filas). */
static void cc_tile_geom(int idx, int *x, int *y, int *w)
{
    int right = 0, cell = 0;
    for (int i = 0; i <= idx && i < cc_ntiles; i++) {
        int wide = cc_tiles[i] == CT_BT, c;
        if (!wide && right < 2) { c = -1 - right; right++; }
        else { if (wide && cell % 4 == 3) cell++; c = cell; cell += wide ? 2 : 1; }
        if (i == idx) {
            if (c < 0) { *x = CC_M + 3 * (CC_T + CC_G); *y = CC_Y + (-1 - c) * (CC_T + CC_G); *w = CC_T; }
            else { *x = CC_M + (c % 4) * (CC_T + CC_G); *y = CC_Y + 2 * (CC_T + CC_G) + (c / 4) * (CC_T + CC_G); *w = wide ? 2 * CC_T + CC_G : CC_T; }
        }
    }
}
static void cc_tile_rect(int slot, int *x, int *y) { int w; cc_tile_geom(slot, x, y, &w); }
#define NP_X CC_M
#define NP_W (2 * CC_T + CC_G)
#define BR_X (CC_M + 2 * (CC_T + CC_G))
#define BR_H (2 * CC_T + CC_G)

static struct sprite cc_ic[16]; static int cc_ic_ok;
static const char *const cc_ic_name[16] = { "plug", "leaf", "gauge", "bolt", "usb", "sliders", "wifi", "wave", "folder", "gear", "sun", "bt", "buds", "plane", 0 };
static const struct sprite *cc_icon(const char *n) { for (int i = 0; cc_ic_name[i]; i++) if (!strcmp(n, cc_ic_name[i])) return &cc_ic[i]; return 0; }

static void cc_bt_module(uint16_t *d, int x, int y, int w, int h, int pressed);   /* ccbt.inc.c */
static int cc_bt_toggle(void);
static int cc_bt_connected(void);
/* ccair.inc.c: modo avion y control de ruido de los auriculares */
static int cc_air_state(void);
static int cc_air_toggle(const char **why);       /* 1 activado, 0 desactivado, -1 no se hizo (why: motivo o 0) */
static void cc_air_radio_on(void);                /* el usuario enciende Wi-Fi o Bluetooth a mano: fuera el modo avion */
static void cc_noise_tile(uint16_t *d, int x, int y);
static const char *cc_noise_tap(void);            /* cambia de modo; devuelve el rotulo (modo nuevo o el error) */
static int cc_ext_poll(void);                     /* 1: algo cambio (auriculares, error de un cambio): repintar */
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

/* rectangulos redondeados con mascara de cobertura cacheada: sin raiz cuadrada por pixel
 * (el panel se compone en ~1/4 del tiempo: importa cuando el reproductor ocupa la CPU) */
static struct { int w, h; float r; uint8_t *m; } cc_masks[6]; static int cc_nmask;
static const uint8_t *cc_mask(int w, int h, float r)
{
    for (int i = 0; i < cc_nmask; i++) if (cc_masks[i].w == w && cc_masks[i].h == h && cc_masks[i].r == r) return cc_masks[i].m;
    uint8_t *m = malloc(w * h); if (!m) return 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float px = x + .5f, py = y + .5f, qx = fabsf(px - w / 2.f) - (w / 2.f - r), qy = fabsf(py - h / 2.f) - (h / 2.f - r);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
        float c = 0.5f - d; m[y * w + x] = (uint8_t)(c <= 0 ? 0 : c >= 1 ? 255 : c * 255);
    }
    if (cc_nmask < 6) { cc_masks[cc_nmask].w = w; cc_masks[cc_nmask].h = h; cc_masks[cc_nmask].r = r; cc_masks[cc_nmask++].m = m; }
    return m;
}
static uint16_t blend565(uint16_t a, uint16_t b, int k);
static void cc_rrect(uint16_t *d, int x0, int y0, int w, int h, float r, uint16_t col, int alpha)
{
    const uint8_t *m = cc_mask(w, h, r); if (!m) { fill_rrect(d, x0, y0, w, h, r, col, alpha); return; }
    for (int y = 0; y < h; y++) {
        int yy = y0 + y; if (yy < 0 || yy >= H) continue;
        uint16_t *row = d + yy * W; const uint8_t *mr = m + y * w;
        for (int x = 0; x < w; x++) {
            int c = mr[x]; if (!c) continue; int xx = x0 + x; if (xx < 0 || xx >= W) continue;
            int k = (c * alpha + 127) / 255 * 32 / 255; if (k) row[xx] = blend565(row[xx], col, k);
        }
    }
}
/* estado real para los botones (iOS: color = encendido). Wi-Fi: lo pedido o lo guardado (ccair);
 * DAC USB: modo USB 0 (audio) en los ajustes guardados del reproductor */
static int air_wifi_now(void);
static int cc_wifi_on(void) { return air_wifi_now(); }
static int cc_usb_dac_on(void) { int v = 1; return !hs_ini_get("usbmode", &v) && v == 0; }
static void cc_tile(uint16_t *d, int x, int y, int tile)
{
    int on = 0; uint16_t acc = rgb(10, 132, 255); const char *ic = "gear";
    if (tile == CT_NOISE) { cc_noise_tile(d, x, y); return; }
    switch (tile) {
    case CT_AIRPLANE: on = cc_air_state(); acc = rgb(255, 149, 0); ic = "plane"; break;
    case CT_NOCHARGE: on = pw_nocharge; acc = rgb(48, 209, 88); ic = "plug"; break;
    case CT_POWER: on = pw_mode != PW_NORMAL; acc = pw_mode == PW_SAVE ? rgb(255, 204, 0) : rgb(255, 69, 58); ic = pw_mode == PW_SAVE ? "leaf" : pw_mode == PW_PERF ? "bolt" : "gauge"; break;
    case CT_DAC: on = cc_usb_dac_on(); ic = "usb"; break;
    case CT_BT: ic = "wifi"; break;          /* (se dibuja como modulo ancho: cc_bt_module) */
    case CT_WIRELESS: on = cc_wifi_on(); ic = "wifi"; break;
    case CT_FILTER: on = cc_filter_short() > 0; acc = rgb(191, 90, 242); ic = "wave"; break;
    case CT_FILES: ic = "folder"; break;
    case CT_SETTINGS: ic = "gear"; break;
    case CT_GAIN: on = ps_v[PS_GAIN] == 1; acc = rgb(255, 149, 0); ic = 0; break;
    case CT_MSEB: on = ps_v[PS_MSEB] == 1; acc = rgb(191, 90, 242); ic = 0; break;
    case CT_EQ: on = ps_v[PS_EQ] == 1; acc = rgb(255, 55, 95); ic = "sliders"; break;
    case CT_SLEEP: on = ps_v[PS_SLEEP] == 1; acc = rgb(94, 92, 230); ic = 0; break;
    }
    if (on) cc_rrect(d, x, y, CC_T, CC_T, CC_T / 2.f, acc, 255);                 /* iOS 26: circulo de color */
    else cc_rrect(d, x, y, CC_T, CC_T, CC_T / 2.f, rgb(255, 255, 255), 46);      /* cristal */
    const struct sprite *s = ic ? cc_icon(ic) : 0;
    if (s && s->w) blit(d, s, x + (CC_T - s->w) / 2, y + (CC_T - s->h) / 2, 32, 255);
    int cx = x + CC_T / 2, cy = y + CC_T / 2;
    if (tile == CT_GAIN) {                       /* "Hi" / "Lo", como el selector de ganancia */
        const char *g = ps_v[PS_GAIN] == 1 ? "Hi" : "Lo"; int tw = tw_n(g, 20, -1);
        draw_text(d, cx - tw / 2, cy + 7, g, 20, 0xFFFF, 0, 0, H);
    } else if (tile == CT_MSEB) {                /* tres mandos de ajuste fino */
        for (int i = 0; i < 3; i++) { int bx = cx - 10 + i * 10, ky = cy - 8 + (i == 1 ? 10 : i == 0 ? 3 : 6);
            fill_rrect(d, bx - 1, cy - 11, 3, 22, 1, 0xFFFF, 200); fill_circle(d, bx + .5f, ky + 2, 4, 0xFFFF, 255); }
    } else if (tile == CT_SLEEP) {               /* luna; con el temporizador puesto, los minutos */
        float my = on ? cy - 6 : cy;
        fill_circle(d, cx, my, 10, 0xFFFF, 255); fill_circle(d, cx + 6, my - 4, 8.5f, on ? acc : rgb(70, 70, 74), 255);
        if (on) { char t[12]; snprintf(t, sizeof t, "%d", ps_v[PS_SLEEPMIN]); int tw = tw_n(t, 12, -1); draw_text(d, cx - tw / 2, cy + 22, t, 12, 0xFFFF, 0, 0, H); }
    }
}
static int cc_bl_level(void) { int b = sysfs_get(BL_PATH, 60); return b < 0 ? 60 : b; }
/* compone el panel completo en dst a partir del fondo desenfocado */
static void cc_slider_paint(uint16_t *d, const uint16_t *blur, float lv, float scale, float stretch);
/* triangulo de reproduccion: base vertical en x0 (dir 1 apunta a la derecha) o punta en x0+w (dir -1) */
static void cc_tri(uint16_t *d, int x0, int cy, int w, int half, int dir)
{
    for (int i = 0; i < w; i++) {
        int col = dir > 0 ? x0 + i : x0 - i;               /* dir -1: la base esta a la derecha (en x0) */
        float hh = half * (1 - (i + .5f) / w);
        for (int y = (int)(cy - hh); y <= (int)(cy + hh); y++) if (col >= 0 && col < W && y >= 0 && y < H) d[y * W + col] = 0xFFFF;
    }
}
static const uint16_t *cc_under;
/* modulo de reproduccion: centro del boton play/pausa (a la derecha de la caratula de 48 px) */
#define CC_NP_PLAY_X (NP_X + 14 + 48 + (NP_W - 14 - 48) / 2)
#define CC_NP_PLAY_Y (CC_Y + 14 + 24)
static int cc_np_pressed;                 /* 1 play, 2 anterior, 3 siguiente: resaltado al pulsar */
/* Estado optimista de play/pausa (como widgets y Reproduciendo): el estado real lo deduce la
 * libreria de las escrituras de audio y tarda de 1,5 s (pausa) a >10 s (arrancar una cola nueva,
 * medido) en cambiar. Tras pulsar se muestra lo pedido hasta que el estado real coincide o pasan
 * 8 s (si la orden fallo, vuelve el real). Un segundo toque se decide con este estado, no con el
 * real atrasado (antes: pausar justo despues de arrancar mostraba "pausa" al reves). */
static int cc_pp_force; static double cc_pp_until, cc_pp_t;
static int cc_playing(const struct np_info *ni)
{
    if (!cc_pp_until) return ni->playing;
    double t = now_ms();
    if (t >= cc_pp_until || (ni->playing == cc_pp_force && t - cc_pp_t > 400)) { cc_pp_until = 0; return ni->playing; }
    return cc_pp_force;
}
/* zona del modulo de reproduccion bajo (x, y): 1 play/pausa, 2 anterior, 3 siguiente, 0 nada */
static int cc_np_zone(int x, int y);
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
    /* Reproduciendo (iOS): caratula, titulo, artista y controles */
    {
        static uint16_t cov[128 * 128]; struct np_info ni; int cok = 0;
        npx_snapshot(&ni, cov, &cok);
        cc_rrect(dst, NP_X, CC_Y, NP_W, BR_H, 22, rgb(255, 255, 255), pressed == 100 ? 80 : 46);
        if (cok) npx_blit_cover(dst, cov, NP_X + 14, CC_Y + 14, 48, 9);
        else { int a = app_by_id("nowplaying"); if (a >= 0) blit_scaled(dst, &apps[a].icon[2], NP_X + 14, CC_Y + 14, 48, 48, 255); }
        const char *ti = ni.source && ni.title[0] ? ni.title : tr("Reproduciendo");
        const char *ar = ni.source && ni.artist[0] ? ni.artist : tr(cc_playing(&ni) ? "Sonando" : "En pausa");
        { int sz = 14; while (sz > 11 && tw_n(ti, sz, -1) > NP_W - 24) sz--; draw_text(dst, NP_X + 12, CC_Y + 84, ti, sz, 0xFFFF, NP_W - 24, 0, H); }
        draw_text(dst, NP_X + 12, CC_Y + 102, ar, 12, rgb(200, 200, 206), NP_W - 24, 0, H);
        /* controles (el modulo es pequeno: zonas grandes). Play/pausa a la derecha de la caratula;
         * abajo solo anterior (mitad izquierda) y siguiente (mitad derecha). Pulsado: un circulo
         * claro detras del boton (cc_np_pressed). */
        uint16_t c = 0xFFFF;
        int px = CC_NP_PLAY_X, py = CC_NP_PLAY_Y;
        if (cc_np_pressed == 1) fill_circle(dst, px, py, 21, 0xFFFF, 60);
        if (cc_playing(&ni)) { fill_rrect(dst, px - 8, py - 11, 6, 22, 2, c, 255); fill_rrect(dst, px + 2, py - 11, 6, 22, 2, c, 255); }
        else cc_tri(dst, px - 7, py, 18, 11, 1);
        int cy = CC_Y + BR_H - 22, lx = NP_X + NP_W / 4, rx = NP_X + NP_W * 3 / 4;
        if (cc_np_pressed == 2) fill_circle(dst, lx, cy, 18, 0xFFFF, 60);
        if (cc_np_pressed == 3) fill_circle(dst, rx, cy, 18, 0xFFFF, 60);
        /* anterior |<<  y siguiente >>| */
        fill_rrect(dst, lx - 12, cy - 6, 2, 12, 1, c, 255);
        cc_tri(dst, lx - 2, cy, 7, 6, -1); cc_tri(dst, lx + 5, cy, 7, 6, -1);
        cc_tri(dst, rx - 5, cy, 7, 6, 1); cc_tri(dst, rx + 2, cy, 7, 6, 1);
        fill_rrect(dst, rx + 10, cy - 6, 2, 12, 1, c, 255);
    }
    /* brillo: el mismo dibujo que durante el arrastre */
    { float lv = cc_bl_level() / 101.f; cc_slider_paint(dst, blur, lv > 1 ? 1 : lv, 1, 0); }
    for (int i = 0; i < cc_ntiles; i++) {
        int x, y, w; cc_tile_geom(i, &x, &y, &w);
        if (cc_tiles[i] == CT_BT) { cc_bt_module(dst, x, y, w, CC_T, pressed == i ? 1 : pressed == 200 + i ? 2 : 0); continue; }
        cc_tile(dst, x, y, cc_tiles[i]); if (pressed == i) cc_rrect(dst, x, y, CC_T, CC_T, CC_T / 2.f, 0xFFFF, 50);
    }
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
    for (int i = 0; i < cc_ntiles; i++) {
        int tx, ty, tw; cc_tile_geom(i, &tx, &ty, &tw);
        if (x >= tx - 4 && x < tx + tw + 4 && y >= ty - 4 && y < ty + CC_T + 4)
            return cc_tiles[i] == CT_BT && x < tx + CC_T - 6 ? 200 + i : i;   /* modulo BT: el circulo enciende/apaga */
    }
    return -1;
}

/* ---- animacion de iOS 26: el fondo real se desenfoca (mezcla bajo -> desenfoque) y la capa
 * de controles aparece bajando unos pixeles. El fondo nunca se desplaza. Coste por fotograma:
 * una mezcla de pantalla + los pixeles de controles (mascara), ~5 ms. */
static uint8_t *cc_ctl; static const uint16_t *cc_blur_buf;                       /* 1 = pixel de un control (panel != desenfoque) */
static void cc_mask_build(const uint16_t *panel, const uint16_t *blur)
{
    if (!cc_ctl) cc_ctl = malloc(W * H);
    if (!cc_ctl) return;
    for (int i = 0; i < W * H; i++) cc_ctl[i] = panel[i] != blur[i];
}
static void cc_frame(const uint16_t *under, const uint16_t *blur, const uint16_t *panel, double e)
{
    if (e < 0) e = 0; if (e > 1) e = 1;
    int k = (int)(e * 32 + .5), kc = (int)(e * e * 32 + .5);   /* los controles entran algo despues */
    int dy = (int)((1 - e) * 28);                     /* bajan 28 px mientras aparecen */
    for (int y = 0; y < H; y++) {
        uint16_t *d = fr0 + y * W; const uint16_t *u = under + y * W, *b = blur + y * W;
        if (k <= 0) memcpy(d, u, W * 2); else if (k >= 32) memcpy(d, b, W * 2); else for (int x = 0; x < W; x++) d[x] = blend565(u[x], b[x], k);
        int sy = y + dy; if (!cc_ctl || sy < 0 || sy >= H || kc <= 0) continue;
        const uint8_t *m = cc_ctl + sy * W; const uint16_t *pp = panel + sy * W;
        if (y < 30) continue;                         /* la barra de estado no se desplaza */
        for (int x = 0; x < W; x++) if (m[x]) d[x] = kc >= 32 ? pp[x] : blend565(d[x], pp[x], kc);
    }
    if (k >= 32) memcpy(fr0, panel, 30 * W * 2); else for (int i = 0; i < 30 * W; i++) fr0[i] = blend565(under[i], panel[i], k);
}

static void cc_mix(const uint16_t *under, const uint16_t *panel, double e)
{
    int k = (int)(e * 32 + .5); if (k <= 0) { memcpy(fr0, under, HM_FRAME_BYTES); return; } if (k >= 32) { memcpy(fr0, panel, HM_FRAME_BYTES); return; }
    for (int i = 0; i < W * H; i++) fr0[i] = blend565(under[i], panel[i], k);
}
static void cc_animate(const uint16_t *under, const uint16_t *panel, double from, double to, int ms)
{
    double t0 = now_ms();
    for (;;) { double t = (now_ms() - t0) / ms; if (t > 1) t = 1; double e = from + (to - from) * ease_out(t);
        if (cc_ctl && cc_blur_buf) cc_frame(under, cc_blur_buf, panel, e); else cc_mix(under, panel, e); present(); if (t >= 1) break; }
}
static void cc_set_caption(const char *s) { cc_caption = s; cc_caption_t = now_ms(); }
static void launch_app(int a, int ix, int iy);
/* abre una app del inicio por su id (desde el launcher, con la animacion del icono) */
static void cc_open_app(const char *id)
{
    int a = app_by_id(id); if (a < 0) { settings_start = SET_ROOT; return; }
    if (!launcher_on) { nav_start(NAV_HOME); nav_wait(3000); if (!launcher_resume()) { settings_start = SET_ROOT; cmd('R'); return; } }
    int ix = W / 2 - L.icon / 2, iy = H / 2 - L.icon / 2;
    for (int i = 0; i < L.dn; i++) if (L.dock[i] == a) dock_xy(i, L.dn, &ix, &iy);
    for (int i = 0; i < L.pn[cur_page]; i++) if (L.page[cur_page][i] == a) cell_xy(i, &ix, &iy);
    launch_app(a, ix, iy);
}
/* Ajustes abierto directamente en una de sus paginas (SET_USB, SET_WIRELESS) */
static void cc_open_settings(int start)
{
    if (start == SET_WIRELESS && app_find("wireless") >= 0) { cc_open_app("wireless"); return; }   /* su propio icono */
    settings_start = start; cc_open_app("settings");
}

/* orden de reproduccion en un hilo: pc_toggle_or_replay espera la respuesta de la libreria
 * cuando hay que volver a poner la cola. Una a la vez (pc_* comparte ficheros de peticion). */
static volatile int cc_pc_busy;
static void *cc_pc_th(void *a)
{
    int n = (int)(intptr_t)a;
    if (n == 1) pc_toggle_or_replay(); else if (n == 2) pc_prev(); else pc_next();
    cc_pc_busy = 0; return 0;
}
static void cc_pc_async(int n)
{
    for (int i = 0; i < 300 && cc_pc_busy; i++) msleep(10);   /* la anterior aun en curso (max 3 s) */
    cc_pc_busy = 1; pthread_t th;
    if (pthread_create(&th, 0, cc_pc_th, (void *)(intptr_t)n)) cc_pc_th((void *)(intptr_t)n); else pthread_detach(th);
}
static int cc_np_zone(int x, int y)
{
    if (x < NP_X || x >= NP_X + NP_W || y < CC_Y || y >= CC_Y + BR_H) return 0;
    if (y > CC_Y + BR_H - 44) return x < NP_X + NP_W / 2 ? 2 : 3;
    return x > NP_X + 68 && y < CC_Y + 72 ? 1 : 0;
}

/* ---- paginas: Centro de control <-> Reproductor (horizontal) e inicio (hacia arriba) ---- */
static int cc_from_launcher, cc_player_right;   /* de donde venimos; el reproductor esta a la derecha */
/* abierto desde una app propia (gesto de arriba dentro de la app): la pantalla ya es del shell,
 * no se captura ni se suelta al reproductor; al cerrar se vuelve a dejar la imagen de la app */
static int cc_in_app;
static uint16_t *cc_app_frame;
static void cc_app_restore(void) { if (cc_app_frame) memcpy(fr0, cc_app_frame, HM_FRAME_BYTES); present(); }
/* abrir una app (o Ajustes en una pagina) desde el Centro de control. Desde el inicio, directo.
 * Dentro de una app de ByOS (solo una a la vez): se apunta, se cierra el panel sobre la app, y la
 * app se va con su gesto de inicio (sintetico, read_pkt); el bucle principal abre la pedida. Antes
 * decia "Vuelve al inicio para abrirlo". */
static void cc_go_app(const uint16_t *under, uint16_t *panel, const char *id)
{
    cc_animate(under, panel, 1, 0, 160);
    if (!cc_in_app) { cc_open_app(id); return; }
    snprintf(pend_id, sizeof pend_id, "%s", id); pend_kind = 1; fake_home_req = 1; fk_step = 0;
    lg("cc: abrir %s al cerrar la app", id);
    cc_app_restore();
}
static void cc_go_settings(const uint16_t *under, uint16_t *panel, int start)
{
    cc_animate(under, panel, 1, 0, 160);
    if (!cc_in_app) { cc_open_settings(start); return; }
    pend_start = start; pend_kind = 2; fake_home_req = 1; fk_step = 0;
    lg("cc: abrir Ajustes (%d) al cerrar la app", start);
    cc_app_restore();
}

/* dos pantallas lado a lado: a en x = o, b en x = o + W (o en [-W, 0]) */
static void cc_hpage(const uint16_t *a, const uint16_t *b, int o)
{
    if (o > 0) o = 0; if (o < -W) o = -W;
    for (int y = 0; y < H; y++) {
        uint16_t *d = fr0 + y * W; const uint16_t *ra = a + y * W, *rb = b + y * W;
        int na = W + o;                                   /* columnas visibles de a */
        if (na > 0) memcpy(d, ra - o, na * 2);
        if (na < W) memcpy(d + (na > 0 ? na : 0), rb, (W - (na > 0 ? na : 0)) * 2);
    }
}
static const uint16_t *cc_player_img(void)
{
    if (cc_player_right) return cc_under;               /* venimos del reproductor: es el fondo */
    if (have_cache_plane) return img_cache_plane;
    static uint16_t *ph; if (!ph) ph = lazy_alloc((void **)&ph, HM_FRAME_BYTES, 0);
    if (!ph) return cc_under ? cc_under : img_under;     /* sin memoria: sin marcador */
    for (int i = 0; i < W * H; i++) ph[i] = rgb(18, 18, 20);
    int a = app_by_id("nowplaying"); if (a >= 0) blit_scaled(ph, &apps[a].icon[2], W / 2 - 48, H / 2 - 70, 96, 96, 255);
    return ph;
}
/* arrastre hacia la izquierda desde el centro de control: entra el reproductor.
 * Devuelve 1 si se completo (y la pantalla ya es del reproductor). */
static int np_from_screen(const uint16_t *under);
static int cc_to_player(const uint16_t *panel, int sx)
{
    /* el reproductor propio (nowui): su pagina entra por la derecha siguiendo al dedo */
    static uint16_t *npimg; if (!npimg) npimg = lazy_alloc((void **)&npimg, HM_FRAME_BYTES, 0);
    if (npimg && np_card(npimg)) {
        struct pkt p; double vel = 0, lt = now_ms(); int lx = sx, o = 0;
        while (cur_down) {
            int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
            double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_x - lx) / (t - lt); lx = cur_x; lt = t; }
            o = cur_x - sx; if (o > 0) o = 0; cc_hpage(panel, npimg, o); present();
            if (got && !p.down) break;
            if (!got) msleep(2);
        }
        int done = -o > W * 0.35 || vel < -0.35;
        double t0 = now_ms(); int o0 = o, to = done ? -W : 0;
        for (;;) {
            double t = (now_ms() - t0) / 230.0; if (t > 1) t = 1;
            cc_hpage(panel, npimg, o0 + (int)((to - o0) * ease_out(t))); present();
            if (t >= 1) break;
        }
        if (!done) { memcpy(fr0, panel, HM_FRAME_BYTES); present(); return 0; }
        static uint16_t *under; if (!under) under = lazy_alloc((void **)&under, HM_FRAME_BYTES, 0);
        if (under) { if (cc_under) memcpy(under, cc_under, HM_FRAME_BYTES); else memcpy(under, panel, HM_FRAME_BYTES); }
        int home = under ? np_from_screen(under) : 0;
        if (cc_in_app) { cc_app_restore(); return 1; }
        if (cc_from_launcher || home) { if (!cc_from_launcher) { nav_start(NAV_HOME); nav_wait(3000); } if (!launcher_resume()) cmd('R'); }
        else cmd('R');
        return 1;
    }
    if (cc_in_app) return 0;                              /* dentro de una app: solo el reproductor propio */
    const uint16_t *pl = cc_player_img();
    if (!cc_player_right) nav_start(NAV_TO_PLANE);       /* se adelanta mientras se arrastra */
    struct pkt p; double vel = 0, lt = now_ms(); int lx = sx, o = 0;
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_x - lx) / (t - lt); lx = cur_x; lt = t; }
        o = cur_x - sx; cc_hpage(panel, nav_done && nav_ok && !cc_player_right ? fr1 : pl, o); present();
        if (got && !p.down) break;
        if (!got) msleep(2);
    }
    int done = -o > W * 0.35 || vel < -0.35;
    double t0 = now_ms(); int o0 = o, to = done ? -W : 0;
    for (;;) {
        double t = (now_ms() - t0) / 230.0; if (t > 1) t = 1;
        cc_hpage(panel, nav_done && nav_ok && !cc_player_right ? fr1 : pl, o0 + (int)((to - o0) * ease_out(t))); present();
        if (t >= 1) break;
    }
    if (!done) {
        if (!cc_player_right && nav_wait(3000) == 1) { nav_start(NAV_BACK); nav_wait(2000); }   /* deshacer */
        memcpy(fr0, panel, HM_FRAME_BYTES); present();
        return 0;
    }
    if (!cc_player_right) {
        int ok = nav_wait(3000);
        if (ok == 1) { memcpy(fr0, fr1, HM_FRAME_BYTES); present(); memcpy(img_cache_plane, fr1, HM_FRAME_BYTES); have_cache_plane = 1; }
        if (cc_from_launcher) { launcher_on = 0; launcher_park_publish(); }
    }
    cmd('R');
    return 1;
}
/* panel (centro de control o reproductor) hacia arriba siguiendo al dedo: aparece el inicio */
static void hpanel_frame(const uint16_t *home, const uint16_t *top, int off)
{
    if (off < 0) off = 0; if (off > H) off = H;
    int k = 18 + 14 * off / H;
    for (int y = 0; y < H; y++) {
        uint16_t *d = fr0 + y * W; int sy = y + off;
        if (sy < H) memcpy(d, top + sy * W, W * 2);
        else { const uint16_t *h = home + y * W; for (int x = 0; x < W; x++) d[x] = dim565(h[x], k); }
    }
    for (int s = 1; s <= 8; s++) { int y = H - off + s - 1; if (y >= 0 && y < H) for (int x = 0; x < W; x++) fr0[y * W + x] = dim565(fr0[y * W + x], 18 + s * 2); }
}
/* devuelve 1 si se fue al inicio */
static int panel_up_home(const uint16_t *top, int sy, int from_launcher)
{
    uint16_t *home = img_app;
    if (launcher_enabled) render_launcher(home, now_ms()); else if (have_cache_home) memcpy(home, img_cache_home, HM_FRAME_BYTES); else memset(home, 0, HM_FRAME_BYTES);
    if (!from_launcher) nav_start(NAV_HOME);
    struct pkt p; double vel = 0, lt = now_ms(); int ly = sy, off = 0;
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lt = t; }
        off = sy - cur_y; hpanel_frame(home, top, off); present();
        if (got && !p.down) break;
        if (!got) msleep(2);
    }
    int done = off > 110 || vel < -0.4;
    double t0 = now_ms(); int o0 = off, to = done ? H : 0;
    for (;;) {
        double t = (now_ms() - t0) / (done ? 260.0 : 200.0); if (t > 1) t = 1;
        hpanel_frame(home, top, o0 + (int)((to - o0) * ease_out(t))); present();
        if (t >= 1) break;
    }
    if (!done) {
        if (!from_launcher && nav_wait(3000) == 1) { /* ya navego al inicio: no se puede deshacer, se acepta */ if (launcher_resume()) return 1; }
        memcpy(fr0, top, HM_FRAME_BYTES); present();
        return 0;
    }
    if (from_launcher) { launcher_present(); return 1; }
    nav_wait(3000);
    if (!launcher_resume()) cmd('R');
    return 1;
}

static void cc_session(uint16_t *under, uint16_t *panel, uint16_t *blur, double e, int start_y);
/* start_y >= 0: se abre siguiendo al dedo desde ahi; < 0: se abre solo (animado) */
static int cc_prepare(void)
{
    if (!cc_ic_ok) { for (int i = 0; cc_ic_name[i]; i++) { char n[32]; snprintf(n, sizeof n, "cc_%s", cc_ic_name[i]); load_sprite(n, &cc_ic[i]); } cc_ic_ok = 1; }
    cc_from_launcher = launcher_on && !cc_in_app;
    uint16_t *under = img_under;
    if (cc_in_app) memcpy(under, cc_app_frame, HM_FRAME_BYTES);
    else if (cc_from_launcher) render_launcher(under, now_ms());
    else {
        memcpy(under, visible(), HM_FRAME_BYTES);
        if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
            memcpy(under + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
        st->ov_on = 0; ov_cur = 0;
        cmd('C');
        double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
        if (!st->capture) return 0;
    }
    have_under = 0;
    double tb = now_ms();
    /* La franja de la barra de estado recibe el MISMO desenfoque y oscurecido que el resto:
     * la fuente del desenfoque no lleva ni el oscurecido propio de la franja del launcher ni
     * los glifos (dejaban una banda mas oscura y un halo: un corte a la altura de la barra).
     * Launcher: el fondo tal cual. Otras pantallas con barra: el contenido de debajo reflejado. */
    {
        static uint16_t sb_keep[30 * W];
        int fix = cc_from_launcher ? 1 : has_statusbar(under) ? 2 : 0;
        if (fix) memcpy(sb_keep, under, sizeof sb_keep);
        if (fix == 1) memcpy(under, wall, sizeof sb_keep);
        else if (fix == 2) for (int y = 0; y < 30; y++) memcpy(under + y * W, under + (59 - y) * W, W * 2);
        cc_blur(under, img_sheet);
        if (fix) memcpy(under, sb_keep, sizeof sb_keep);
    }
    cc_under = under;
    double tb2 = now_ms();
    cc_caption = 0;
    cc_render(img_base, img_sheet, -1);
    cc_mask_build(img_base, img_sheet); cc_blur_buf = img_sheet;
    lg("centro de control: desenfoque %.0f ms, panel %.0f ms", tb2 - tb, now_ms() - tb2);
    return 1;
}
static void cc_open(int start_y)
{
    cc_player_right = 0;
    ps_start(); ps_refresh();                    /* ganancia, MSEB, ecualizador, temporizador */
    if (!cc_prepare()) return;
    cc_session(img_under, img_base, img_sheet, 0, start_y);
}
/* desde Now Playing deslizando hacia la derecha: el centro de control entra por la izquierda */
static void cc_from_player(int sx)
{
    cc_player_right = 1;
    if (!cc_prepare()) return;
    uint16_t *under = img_under, *panel = img_base;
    struct pkt p; double vel = 0, lt = now_ms(); int lx = sx, o = 0;
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_x - lx) / (t - lt); lx = cur_x; lt = t; }
        o = cur_x - sx; if (o < 0) o = 0;
        cc_hpage(panel, under, o - W); present();
        if (got && !p.down) break;
        if (!got) msleep(2);
    }
    int done = o > W * 0.35 || vel > 0.35;
    double t0 = now_ms(); int o0 = o, to = done ? W : 0;
    for (;;) {
        double t = (now_ms() - t0) / 230.0; if (t > 1) t = 1;
        cc_hpage(panel, under, o0 + (int)((to - o0) * ease_out(t)) - W); present();
        if (t >= 1) break;
    }
    if (!done) { cmd('R'); return; }
    cc_session(under, panel, img_sheet, 1, -2);
}


/* ---- deslizador de brillo (iOS): crece al tocarlo, el relleno sigue al dedo 1:1 y el brillo
 * real cambia en el mismo fotograma; goma en los extremos; el sol crece con el nivel.
 * Solo se redibuja su zona (present_rows): cabe de sobra en 16 ms a 60 Hz. */
static void cc_sun(uint16_t *d, float cx, float cy, float lv, uint16_t col)
{
    float r = 4.2f + 1.6f * lv, r0 = r + 2.6f, r1 = r0 + 2.2f + 2.6f * lv;
    fill_circle(d, cx, cy, r, col, 255);
    for (int i = 0; i < 8; i++) {
        float a = i * 0.7854f, c = cosf(a), sn = sinf(a);
        for (float t = r0; t <= r1; t += 0.8f) fill_circle(d, cx + c * t, cy + sn * t, 1.05f, col, 255);
    }
}
static void cc_slider_paint(uint16_t *d, const uint16_t *blur, float lv, float scale, float stretch)
{
    /* zona: el modulo con margen para el crecimiento y la goma */
    /* margen de 24 filas: crece como mucho BR_H * 0.02 + 10 por cada lado (antes se salia
     * por arriba y la punta se cortaba al estirar) */
    int zy0 = CC_Y - 24, zy1 = CC_Y + BR_H + 24;
    for (int y = zy0; y < zy1; y++) memcpy(d + y * W + BR_X - 10, blur + y * W + BR_X - 10, (CC_T + 20) * 2);
    float w = CC_T * scale, h = BR_H * scale + fabsf(stretch);
    float cx = BR_X + CC_T / 2.f, cy = CC_Y + BR_H / 2.f - (stretch > 0 ? stretch / 2 : stretch < 0 ? stretch / 2 : 0);
    int x0 = (int)(cx - w / 2 + .5f), y0 = (int)(cy - h / 2 + .5f), iw = (int)(w + .5f), ih = (int)(h + .5f);
    float rad = 22 * scale;
    fill_rrect(d, x0, y0, iw, ih, rad, rgb(255, 255, 255), 46);
    int fh = (int)(ih * lv + .5f);
    if (fh > 0) {
        static uint16_t tmp[(CC_T + 20) * (2 * CC_T + CC_G + 40)];
        int cut = ih - fh;                               /* filas de arriba que no se llenan */
        for (int y = 0; y < cut; y++) memcpy(tmp + y * iw, d + (y0 + y) * W + x0, iw * 2);
        fill_rrect(d, x0, y0, iw, ih, rad, rgb(245, 245, 247), 255);
        for (int y = 0; y < cut; y++) memcpy(d + (y0 + y) * W + x0, tmp + y * iw, iw * 2);
    }
    float sy = y0 + ih - 22 * scale;
    cc_sun(d, cx, sy, lv, sy > y0 + ih - fh ? rgb(70, 70, 76) : 0xFFFF);
}
static void cc_slider_draw(uint16_t *d, const uint16_t *blur, float lv, float scale, float stretch)
{
    cc_slider_paint(d, blur, lv, scale, stretch); present_rows(CC_Y - 24, CC_Y + BR_H + 24);
}
/* 1 = se mantuvo pulsado sin mover (abrir Pantalla y brillo) */
static int cc_brightness_drag(uint16_t *panel, const uint16_t *blur, int sy)
{
    float lv0 = cc_bl_level() / 101.f, lv = lv0, scale = 1, stretch = 0; int last = -1, moved = 0, held = 0;
    struct pkt p; double lt = now_ms(), tstart = lt;
    memcpy(fr0, panel, HM_FRAME_BYTES);
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        if (!moved && abs(cur_y - sy) > 8) moved = 1;
        if (!moved && now_ms() - tstart > 550) { held = 1; break; }
        float raw = lv0 + (sy - cur_y) / (float)BR_H;          /* 1:1 con el dedo */
        float over = raw > 1 ? raw - 1 : raw < 0 ? raw : 0;
        lv = raw < 0 ? 0 : raw > 1 ? 1 : raw;
        if (pw_mode == PW_SAVE && lv > 40 / 101.f) lv = 40 / 101.f;
        int nb = (int)(lv * 101 + .5f); if (nb < 2) nb = 2;
        if (nb != last) { sysfs_put(BL_PATH, nb); last = nb; }
        double t = now_ms(); float k = (float)((t - lt) / 16.7); lt = t; if (k > 3) k = 3;
        scale += (1.04f - scale) * (1 - powf(0.65f, k));          /* crece con muelle */
        float st_t = over * BR_H; st_t = st_t > 0 ? 10 * (1 - expf(-st_t / 40)) : -10 * (1 - expf(st_t / 40));   /* goma (tope 10 px) */
        stretch += (st_t - stretch) * (1 - powf(0.5f, k));
        cc_slider_draw(fr0, blur, lv, scale, stretch);
        if (got && !p.down) break;
        if (!got) msleep(2);
    }
    double t0 = now_ms(); float s0 = scale, g0 = stretch;          /* vuelve a su tamano */
    for (;;) {
        double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1;
        float e = (float)ease_out(t);
        cc_slider_draw(fr0, blur, lv, s0 + (1 - s0) * e, g0 * (1 - e));
        if (t >= 1) break;
    }
    cc_render(panel, blur, -1); cc_mask_build(panel, blur); memcpy(fr0, panel, HM_FRAME_BYTES); present();
    return held;
}


/* deslizar hacia arriba en el centro de control = al inicio (especificacion del usuario): los
 * controles suben y se desvanecen y el desenfoque pasa al inicio real, que no se mueve. */
static int cc_close_drag(const uint16_t *under, uint16_t *panel, const uint16_t *blur, int sy, int from_launcher, int to_home)
{
    (void)under; (void)to_home;
    uint16_t *home = img_app;
    if (cc_in_app) memcpy(home, cc_app_frame, HM_FRAME_BYTES);
    else if (launcher_enabled) render_launcher(home, now_ms()); else if (have_cache_home) memcpy(home, img_cache_home, HM_FRAME_BYTES); else memset(home, 0, HM_FRAME_BYTES);
    struct pkt p; double vel = 0, lt = now_ms(); int ly = sy; double e = 1;
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lt = t; }
        e = 1 - (sy - cur_y) / 200.0; if (e > 1) e = 1; if (e < 0) e = 0;
        cc_frame(home, blur, panel, e); present();
        if (got && !p.down) break;
        if (!got) msleep(2);
    }
    if (e > 0.7 && vel > -0.4) {                              /* no llega: el panel vuelve */
        double t0 = now_ms(), e0 = e;
        for (;;) { double t = (now_ms() - t0) / 160.0; if (t > 1) t = 1; cc_frame(home, blur, panel, e0 + (1 - e0) * ease_out(t)); present(); if (t >= 1) break; }
        return 0;
    }
    if (!from_launcher && !cc_in_app) nav_start(NAV_HOME);  /* el reproductor va al inicio durante el fundido */
    double t0 = now_ms(), e0 = e;
    for (;;) { double t = (now_ms() - t0) / 180.0; if (t > 1) t = 1; cc_frame(home, blur, panel, e0 * (1 - ease_out(t))); present(); if (t >= 1) break; }
    if (cc_in_app) { cc_app_restore(); return 1; }
    if (from_launcher) { launcher_present(); return 1; }
    nav_wait(3000); if (!launcher_resume()) cmd('R');
    return 1;
}
static void cc_session_body(uint16_t *under, uint16_t *panel, uint16_t *blur, double e, int start_y);
static void cc_session(uint16_t *under, uint16_t *panel, uint16_t *blur, double e, int start_y)
{
    hb_block++; cc_session_body(under, panel, blur, e, start_y); hb_block--;
}
static void cc_session_body(uint16_t *under, uint16_t *panel, uint16_t *blur, double e, int start_y)
{
    int from_launcher = cc_from_launcher;
    struct pkt p;
    if (start_y >= 0) {           /* tirar hacia abajo */
        double vel = 0, lt = now_ms(); int ly = start_y;
        while (cur_down) {
            int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
            double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lt = t; }
            e = (cur_y - start_y) / 240.0; if (e < 0) e = 0; if (e > 1) e = 1;
            cc_frame(under, blur, panel, e); present();
            if (got && !p.down) break;
        }
        lg("cc: suelta e=%.2f vel=%.2f", e, vel);
        if (e < 0.3 && vel < 0.3) { cc_animate(under, panel, e, 0, 160); goto out; }
    }
    if (start_y != -2) cc_animate(under, panel, e, 1, start_y >= 0 ? 200 : 260);
    for (;;) {
        if (!read_pkt(&p, cc_caption && now_ms() - cc_caption_t < 1900 ? 100 : 500)) {
            int ext = cc_ext_poll();              /* puede dejar un rotulo (error de los auriculares) */
            if (cc_pp_until && now_ms() >= cc_pp_until) npx_changed = 1;   /* fin del estado optimista: repintar con el real */
            if (ext || (cc_caption && now_ms() - cc_caption_t >= 1800) || npx_changed) { npx_changed = 0; if (cc_caption && now_ms() - cc_caption_t >= 1800) cc_caption = 0; cc_render(panel, blur, -1); cc_mask_build(panel, blur); memcpy(fr0, panel, HM_FRAME_BYTES); present(); }
            continue;
        }
        if (!p.down) continue;
        int sx = p.x, sy = p.y, hit = cc_hit(sx, sy);
        if (hit == 101) {                                    /* brillo (iOS); mantener: sus ajustes */
            if (cc_brightness_drag(panel, blur, sy) && !cc_in_app) {
                while (cur_down && read_pkt(&p, 3000)) {}
                cc_animate(under, panel, 1, 0, 160); cc_open_settings(SET_DISPLAY); return;
            }
            continue;
        }
        /* al pulsar un control del modulo de reproduccion solo se marca su circulo (no toda la tarjeta) */
        int npd = hit == 100 ? cc_np_zone(sx, sy) : 0;
        double cc_t_down = now_ms();
        if (npd) { cc_np_pressed = npd; cc_render(fr0, blur, -1); present(); lg("cc: control %d pulsado (%.0f ms)", npd, now_ms() - cc_t_down); }
        else if (hit >= 0) { cc_render(fr0, blur, hit); present(); }
        /* decidir pronto: toque, pagina horizontal o subir al inicio */
        int dir = 0; double tdown = now_ms();
        while (cur_down) {
            if (!dir && hit >= 0 && hit != 100 && now_ms() - tdown > 550) { dir = 4; break; }   /* mantener pulsado */
            if (!read_pkt(&p, 50)) continue;
            int dx = cur_x - sx, dy = cur_y - sy;
            if (dx < -14 && -dx > abs(dy)) { dir = 1; break; }        /* izquierda: reproductor */
            if (dy < -14 && -dy > abs(dx)) { dir = 2; break; }        /* arriba: inicio */
            if (abs(dx) > 14 || dy > 14) { dir = 3; }
        }
        if (dir) cc_np_pressed = 0;          /* arrastre o mantener: sin circulo de pulsado */
        if (dir == 4) {
            /* mantener pulsado: la pagina de Ajustes de ese control (como en iOS) */
            int tl = hit == 101 ? -1 : cc_tiles[hit >= 200 ? hit - 200 : hit];
            lg("cc: mantener pulsado %d", tl);
            while (cur_down && read_pkt(&p, 3000)) {}
            switch (tl) {
            case -1: cc_go_settings(under, panel, SET_DISPLAY); return;
            case CT_AIRPLANE: case CT_BT: case CT_WIRELESS: cc_go_settings(under, panel, SET_WIRELESS); return;
            case CT_NOISE: cc_go_app(under, panel, "headphones"); return;
            case CT_NOCHARGE: case CT_POWER: cc_go_settings(under, panel, SET_POWER); return;
            case CT_DAC: cc_go_settings(under, panel, SET_USB); return;
            case CT_FILTER: cc_go_settings(under, panel, SET_SOUND); return;
            case CT_FILES: cc_go_app(under, panel, "files"); return;
            case CT_GAIN: case CT_MSEB: case CT_EQ: cc_go_settings(under, panel, SET_SOUND); return;
            case CT_SLEEP: cc_go_settings(under, panel, SET_ROOT); return;
            default: cc_go_settings(under, panel, SET_CCPAGE); return;
            }
        }
        if (dir == 1) { if (cc_to_player(panel, sx)) return; continue; }
        if (dir == 2) { if (cc_close_drag(under, panel, blur, sy, from_launcher, sy >= 440)) return; continue; }
        if (dir == 3) { while (cur_down && read_pkt(&p, 3000)) {} cc_render(panel, blur, -1); cc_mask_build(panel, blur); memcpy(fr0, panel, HM_FRAME_BYTES); present(); continue; }
        if (hit < 0) break;                                   /* toque fuera: cerrar */
        /* controles: play/pausa = todo lo que hay a la derecha de la caratula (arriba); abajo,
         * mitad izquierda anterior, mitad derecha siguiente */
        int npc = npd;
        if (npc) {
            if (npc == 1) {                     /* optimista: se muestra ya lo pedido */
                struct np_info cur; static uint16_t cv[128 * 128]; int ok = 0; npx_snapshot(&cur, cv, &ok);
                cc_pp_force = !cc_playing(&cur); cc_pp_t = now_ms(); cc_pp_until = cc_pp_t + 8000;
            }
            double t_up = now_ms();
            msleep(70); cc_np_pressed = 0;
            cc_render(panel, blur, -1); cc_mask_build(panel, blur); memcpy(fr0, panel, HM_FRAME_BYTES); present();
            lg("cc: control %d soltado %.0f ms tras pulsar, pintado en %.0f ms", npc, t_up - cc_t_down, now_ms() - t_up);
            cc_pc_async(npc);                   /* la orden (volver a poner la cola tarda ~1 s) fuera del hilo de la interfaz */
            npx_kick_until = now_ms() + 4000;
            continue;
        }
        if (hit == 100) {
            if (cc_player_right) { cc_hpage(panel, under, 0); double t0 = now_ms();   /* volver al reproductor */
                for (;;) { double t = (now_ms() - t0) / 230.0; if (t > 1) t = 1; cc_hpage(panel, under, -(int)(W * ease_out(t))); present(); if (t >= 1) break; }
                cmd('R'); return; }
            cc_animate(under, panel, 1, 0, 180);
            if (cc_in_app) { np_from_screen(cc_app_frame); cc_app_restore(); return; }
            cc_open_app("nowplaying"); return;
        }
        if (hit >= 200) {                                      /* Bluetooth: encender / apagar */
            int on = cc_bt_toggle();
            if (on) cc_air_radio_on();
            cc_set_caption(tr(on ? "Bluetooth activado" : "Bluetooth desactivado"));
            cc_render(panel, blur, -1); cc_mask_build(panel, blur); memcpy(fr0, panel, HM_FRAME_BYTES); present();
            continue;
        }
        int tile = cc_tiles[hit];
        switch (tile) {
        case CT_AIRPLANE: {
            const char *why = 0; int r = cc_air_toggle(&why);
            if (r >= 0) cc_set_caption(tr(r ? "Modo avión activado" : "Modo avión desactivado"));
            else if (why) cc_set_caption(why);
            break; }
        case CT_NOISE: cc_set_caption(cc_noise_tap()); break;
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
        case CT_BT:                                            /* el modulo: auriculares conectados o Inalambrico */
            if (cc_bt_connected()) cc_go_app(under, panel, "headphones"); else cc_go_settings(under, panel, SET_WIRELESS); return;
        case CT_DAC: cc_go_settings(under, panel, SET_USB); return;
        case CT_WIRELESS: cc_go_settings(under, panel, SET_WIRELESS); return;
        case CT_FILES: cc_go_app(under, panel, "files"); return;
        case CT_SETTINGS: cc_go_settings(under, panel, SET_ROOT); return;
        case CT_GAIN:
            if (ps_v[PS_LINEOUT]) { cc_set_caption(tr("No disponible con la salida de línea")); break; }
            ps_set_now(PS_GAIN, !ps_v[PS_GAIN]); cc_set_caption(tr(ps_v[PS_GAIN] ? "Ganancia alta" : "Ganancia baja")); break;
        case CT_MSEB:
            ps_set_now(PS_MSEB, !ps_v[PS_MSEB]); cc_set_caption(tr(ps_v[PS_MSEB] ? "MSEB activado" : "MSEB desactivado")); break;
        case CT_EQ:
            ps_set_now(PS_EQ, !ps_v[PS_EQ]); cc_set_caption(tr(ps_v[PS_EQ] ? "Ecualizador activado" : "Ecualizador desactivado")); break;
        case CT_SLEEP: {                                       /* no -> 15 -> 30 -> 45 -> 60 -> 90 -> 120 -> no */
            ss_derive(); ui_sleep_i = (ui_sleep_i + 1) % 7; ssa_sleep();
            if (ui_sleep_i) { char t[64]; snprintf(t, sizeof t, tr("Apagado en %d min"), V_SLEEP[ui_sleep_i]); cc_set_caption(t); }
            else cc_set_caption(tr("Temporizador desactivado"));
            break; }
        }
        cc_render(panel, blur, -1); cc_mask_build(panel, blur); memcpy(fr0, panel, HM_FRAME_BYTES); present();
    }
    cc_animate(under, panel, 1, 0, 200);
out:
    if (cc_in_app) cc_app_restore();
    else if (from_launcher) launcher_present(); else cmd('R');
}
/* gesto de arriba dentro de una app propia (fr0 = la pantalla de la app): centro de control o
 * reproductor siguiendo al dedo; al cerrar, la app vuelve a estar como estaba */
static void own_top_gesture(int sx, int sy)
{
    static uint16_t *keep; if (!keep) keep = lazy_alloc((void **)&keep, HM_FRAME_BYTES, 0);
    if (!keep) { drain_until_up(); return; }
    memcpy(keep, fr0, HM_FRAME_BYTES);
    int act = sx < W / 2 ? G.top : G.top_r;
    lg("gesto de arriba en app: %s", act == GA_CC ? "centro de control" : act == GA_PLAYER ? "reproductor" : "nada");
    if (act == GA_CC) {
        cc_in_app = 1; cc_app_frame = keep; cc_player_right = 0;
        if (cc_prepare()) cc_session(img_under, img_base, img_sheet, 0, sy);
        cc_in_app = 0; cc_app_frame = 0;
    } else if (act == GA_PLAYER) {
        /* la hoja del reproductor baja siguiendo al dedo; soltada pasado el 40 %, se abre suelto */
        memcpy(img_under, keep, HM_FRAME_BYTES); have_under = 1;
        if (!np_card(img_sheet)) memset(img_sheet, 0, HM_FRAME_BYTES);
        struct pkt p; double vel = 0, lt = now_ms(); int last = sy, rev = 0;
        for (;;) {
            int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
            if (got && !p.down) break;
            double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - last) / (t - lt); last = cur_y; lt = t; }
            rev = cur_y - sy + 12; if (rev < 0) rev = 0; if (rev > H) rev = H;
            compose_slide(fr0, img_under, img_sheet, 0, rev - H, 32 - 11 * rev / H); present();
            if (!cur_down) break;
            if (!got) msleep(2);
        }
        int open = rev > H * 0.4 || vel > 0.35, from = rev, to = open ? H : 0; double t0 = now_ms();
        for (;;) {
            double t = (now_ms() - t0) / 240.0; if (t > 1) t = 1;
            int r = from + (int)((to - from) * ease_out(t));
            compose_slide(fr0, img_under, img_sheet, 0, r - H, 32 - 11 * r / H); present();
            if (t >= 1) break;
        }
        if (open) np_from_screen(keep);
    } else drain_until_up();
    drain_until_up();
    memcpy(fr0, keep, HM_FRAME_BYTES); present();
}

/* Now Playing hacia arriba (desde el centro de la pantalla): el panel sube y aparece el inicio */
static void plane_up(int sy)
{
    memcpy(img_sheet, visible(), HM_FRAME_BYTES);
    if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
        memcpy(img_sheet + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    have_under = 0; st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return;
    if (!panel_up_home(img_sheet, sy, 0)) cmd('R');
}
