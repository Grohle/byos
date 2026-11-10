/* keyboard.inc.c - teclado estilo iOS (oscuro), ligero y fluido.
 *  - Distribuciones ES (con ñ), EN y FR (AZERTY); paginas 123 y #+=; fila de numeros opcional.
 *  - Burbuja al pulsar, mayusculas (un toque) y bloqueo (doble toque), mayuscula automatica al
 *    empezar frase, doble espacio -> ". ", borrar con repeticion que acelera (y por palabras),
 *    pulsacion larga para tildes y variantes. Cursor: deslizar en la barra espaciadora (Gboard) o
 *    mantenerla ~300 ms (trackpad de iOS: las teclas se apagan, el cursor flota y sigue al dedo con
 *    aceleracion, tambien por lineas en los editores de varias lineas).
 *  - Entrada y salida deslizandose (curva de iOS, ~300 ms) a partir de capas precalculadas: la
 *    pagina se fotografia una vez y el teclado vive en un sprite (uno por distribucion/tamano/estado):
 *    cada fotograma solo copia filas. Deslizar hacia abajo sobre el teclado lo oculta (sigue al dedo).
 *  - El campo con el foco siempre a la vista (iOS): si el teclado lo taparia, la pagina entera (bajo
 *    la barra de estado) sube lo justo, con la misma curva, y baja al cerrarlo. Ver kb_begin_ex.  *  - Barra inferior fija: globo (menu: distribuciones y ajustes) y boton para ocultar el teclado.
 *  - Hoja de ajustes dentro del propio teclado; la altura se cambia arrastrando su borde.
 *  - Rendimiento: las teclas se pintan con mascaras en cache (sin raices por pixel), la pulsacion
 *    solo repinta el teclado y la burbuja (presentacion parcial) y nada reserva memoria al teclear.
 * Incluye el editor de una linea (renombrar, nueva carpeta...) y el motor de edicion que usa el
 * editor de texto multilinea de Archivos. */

/* configuracion (Personalizar > Teclado y la hoja del propio teclado) */
static int kb_cfg_height = 1, kb_cfg_numrow = 0, kb_cfg_layout = 0;   /* layout: 0 ES, 1 EN, 2 FR */
static int kb_cfg_rowh;                       /* altura continua de tecla (px); 0 = segun kb_cfg_height */
static int kb_cfg_autocap = 1, kb_cfg_period = 1;
static int kb_rowh_coarse = -1, kb_conf_loaded;
#define KB_CONF ASSETS "/keyboard.conf"
#define KR_MIN 32
#define KR_MAX 48
#define KB_BB 28                              /* barra inferior (globo y ocultar) */
#define KB_SH 165                             /* hoja de ajustes */
static int kb_kr_h(void)
{
    if (kb_cfg_rowh >= KR_MIN && kb_cfg_rowh <= KR_MAX) return kb_cfg_rowh;
    return kb_cfg_height == 0 ? 36 : kb_cfg_height == 2 ? 48 : 42;
}
static int kb_rows(void) { return 4 + (kb_cfg_numrow ? 1 : 0); }
#define KR_H (kb_kr_h())
/* estado de presentacion: desplazamiento (animacion/oculto), relleno de arrastre, hoja, menu */
static int kb_off, kb_hpad, kb_hadj, kb_sheet, kb_sheet_h, kb_menu, kb_menu_sel = -1, kb_trackpad, kb_bar_press;
static int kb_keys_h(void) { return kb_rows() * (KR_H + 8) + 6 + KB_BB + kb_hpad; }
#define KB_H (kb_keys_h() + kb_sheet_h + kb_hadj)
/* Campo a la vista: la pagina del llamante vive en "coordenadas de pagina". Con kb_pg > 0, la fila y
 * (>= KB_SB) de la pagina se ve en la fila y - kb_pg de la pantalla. KB_YR es el borde superior real
 * del teclado (pantalla); KB_Y, el que ve el llamante (pagina): con el desplazamiento, KB_YR + kb_pg.
 * Asi el llamante pinta y recorta como siempre (hasta KB_Y) y compara toques con KB_Y tras kb_map(). */
static int kb_pg, kb_vy, kb_nodraw;           /* desplazamiento de la pagina; extra de las instantaneas */
static int kb_fy0 = -1, kb_fy1 = -1;          /* campo con el foco (coordenadas de pagina) */
static int kb_area;                           /* > 0: editor de varias lineas; su texto (desde esta y) se desplaza solo */
static int kb_c_seen, kb_cx, kb_cy0, kb_cy1;  /* cursor que el llamante ha pintado (kb_caret) */
static int (*kb_vmove)(int cur, int dir);     /* linea visual arriba/abajo (editores de varias lineas) */
#define KB_SB 30                              /* barra de estado: nunca se desplaza */
#define KB_MARGIN 10                          /* aire entre el campo y el teclado */
#define KB_YR (H - KB_H + kb_off)
#define KB_Y (KB_YR + kb_pg + kb_vy)
static int kb_keys_top(void) { return H - kb_keys_h() + kb_off; }
static int kb_keys_y0(void) { return kb_keys_top() + kb_hpad; }
static int kb_bar_y(void) { return kb_keys_y0() + kb_rows() * (KR_H + 8) + 6; }
static int kb_row_y(int r) { return 8 + r * (KR_H + 8); }      /* relativa a kb_keys_y0() */

struct key { int x, y, w, h; const char *lbl; int code; };   /* code: >0 caracter, <0 especial; y relativa */
enum { K_SHIFT = -1, K_BACK = -2, K_LAYER = -3, K_SPACE = -4, K_ENTER = -5, K_HOME = -6 };
/* K_HOME: deslizar hacia arriba desde el borde inferior (entre el globo y "ocultar"), con el
 * teclado abierto: el gesto de inicio de iOS. El dedo sigue apoyado; el llamante lo pasa a
 * own_home_gesture. Quien no lo atiende lo ignora (como antes: no hacia nada). */
static struct key kbk[64]; static int nkeys;
static int kb_layer, kb_shift;            /* capa: 0 letras, 1 123, 2 #+= */
static int kb_caps, kb_shift_auto, kb_pressed = -1;
static const char *kb_enter_lbl = "";

#define KB_C_BG rgb(32, 32, 34)
#define KB_C_KEY rgb(108, 108, 112)
#define KB_C_SPEC rgb(72, 72, 76)
#define KB_C_BLUE rgb(10, 132, 255)
#define KB_C_SHEET rgb(22, 22, 24)

/* ------------------------------------------------------------------ configuracion propia */
static void kb_conf_load(void)
{
    if (kb_conf_loaded) return;
    kb_conf_loaded = 1;
    FILE *f = fopen(KB_CONF, "r"); if (!f) return;
    char l[64]; int a, b;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "rowh %d %d", &a, &b) == 2) { kb_cfg_rowh = a; kb_rowh_coarse = b; }
        else if (sscanf(l, "autocap %d", &a) == 1) kb_cfg_autocap = a != 0;
        else if (sscanf(l, "period %d", &a) == 1) kb_cfg_period = a != 0;
    }
    fclose(f);
}
static void kb_conf_save(void)
{
    FILE *f = fopen(KB_CONF ".tmp", "w"); if (!f) return;
    fprintf(f, "rowh %d %d\nautocap %d\nperiod %d\n", kb_cfg_rowh, kb_cfg_height, kb_cfg_autocap, kb_cfg_period);
    fclose(f); rename(KB_CONF ".tmp", KB_CONF);
    kb_rowh_coarse = kb_cfg_height;
}

/* ------------------------------------------------------------------ distribucion */
static void kb_row_keys(const char *row, int y, int x_left, int x_right, int maxkw)
{
    static char lbls[64][5];
    int n = 0; const char *s = row; while (utf8_next(&s)) n++;
    int avail = x_right - x_left, gap = 4, kw = (avail - (n - 1) * gap) / n; if (kw > maxkw) kw = maxkw;
    int tot = n * kw + (n - 1) * gap, x0 = x_left + (avail - tot) / 2;
    s = row;
    for (int i = 0; i < n && nkeys < 64; i++) {
        const char *st0 = s; int c = utf8_next(&s);
        int k = nkeys, len = (int)(s - st0); memcpy(lbls[k], st0, len); lbls[k][len] = 0;
        kbk[nkeys++] = (struct key){ x0 + i * (kw + gap), y, kw, KR_H, lbls[k], c };
    }
}
static unsigned kb_gen;                       /* cambia con cada distribucion: invalida los sprites */
static void kb_relayout(void)
{
    kb_gen++;
    static const char *LET[3][3] = {
        { "qwertyuiop", "asdfghjklñ", "zxcvbnm" },        /* espanol */
        { "qwertyuiop", "asdfghjkl", "zxcvbnm" },         /* ingles */
        { "azertyuiop", "qsdfghjklm", "wxcvbn'" },        /* frances (AZERTY) */
    };
    int L = kb_cfg_layout < 0 || kb_cfg_layout > 2 ? 0 : kb_cfg_layout;
    const char *top = 0, *r0, *r1, *r2;
    if (kb_layer == 0) { r0 = LET[L][0]; r1 = LET[L][1]; r2 = LET[L][2]; if (kb_cfg_numrow) top = "1234567890"; }
    else if (kb_layer == 1) { r0 = "1234567890"; r1 = L == 1 ? "-/:;()$&@\"" : "-/:;()€&@\""; r2 = ".,?!'"; if (kb_cfg_numrow) top = "[]{}#%^*+="; }
    else { r0 = "[]{}#%^*+="; r1 = L == 1 ? "_\\|~<>€£¥•" : "_\\|~<>$£¥•"; r2 = ".,?!'"; if (kb_cfg_numrow) top = "1234567890"; }
    nkeys = 0; int r = 0;
    if (top) kb_row_keys(top, kb_row_y(r++), 3, W - 3, 30);
    kb_row_keys(r0, kb_row_y(r++), 3, W - 3, 30);
    kb_row_keys(r1, kb_row_y(r++), 3, W - 3, 30);
    kbk[nkeys++] = (struct key){ 3, kb_row_y(r), 42, KR_H, kb_layer == 0 ? "" : kb_layer == 1 ? "#+=" : "123", K_SHIFT };
    kb_row_keys(r2, kb_row_y(r), 49, W - 49, kb_layer ? 46 : 30);
    kbk[nkeys++] = (struct key){ W - 45, kb_row_y(r), 42, KR_H, "", K_BACK };
    r++;
    kbk[nkeys++] = (struct key){ 3, kb_row_y(r), 64, KR_H, kb_layer ? "ABC" : "123", K_LAYER };
    kbk[nkeys++] = (struct key){ 71, kb_row_y(r), 166, KR_H, tr("espacio"), K_SPACE };
    kbk[nkeys++] = (struct key){ 241, kb_row_y(r), 76, KR_H, kb_enter_lbl, K_ENTER };
}
/* llamada por cada editor al empezar: estado limpio (capa y mayusculas las pone el llamante) */
static void kb_layout(const char *enter_label)
{
    kb_conf_load();
    if (kb_cfg_rowh && kb_rowh_coarse != kb_cfg_height) kb_cfg_rowh = 0;   /* cambiada en Ajustes */
    kb_enter_lbl = enter_label;
    kb_off = kb_hpad = kb_hadj = kb_sheet = kb_sheet_h = kb_menu = kb_trackpad = kb_bar_press = 0;
    kb_menu_sel = -1; kb_caps = 0; kb_shift_auto = 0; kb_pressed = -1;
    kb_pg = kb_vy = kb_nodraw = 0; kb_fy0 = kb_fy1 = -1; kb_area = 0; kb_vmove = 0; kb_c_seen = 0;
    kb_relayout();
    kb_off = KB_H;                            /* oculto hasta kb_begin: nada lo pinta antes de tiempo */
}
/* el llamante informa del campo con el foco (y0..y1, coordenadas de pagina); antes de kb_begin o
 * kb_reveal, o cuando el campo se mueve */
static void kb_field(int y0, int y1) { kb_fy0 = y0; kb_fy1 = y1; }
/* editores de varias lineas: su contenido (desde la fila top) se desplaza solo para seguir al cursor;
 * la pagina no se mueve y la animacion de entrada desliza ese contenido con el teclado */
static void kb_scroll_area(int top) { kb_area = top; }
static void kb_set_vmove(int (*fn)(int, int)) { kb_vmove = fn; }
/* el redibujado del llamante dice donde ha pintado el cursor (x, y0..y1 en coordenadas de pagina) */
static void kb_caret(int x, int y0, int y1) { kb_c_seen = 1; kb_cx = x; kb_cy0 = y0; kb_cy1 = y1; }
/* toque leido por el llamante -> coordenadas de pagina (no hace nada sin desplazamiento) */
static void kb_map(struct pkt *p) { if (kb_pg) p->y += kb_pg; }
/* presentacion parcial de filas de pagina (p. ej. el parpadeo del cursor) */
static void kb_present_rows(int y0, int y1)
{
    if (kb_pg) { y0 = y0 >= KB_SB ? y0 - kb_pg : y0; y1 = y1 > KB_SB ? y1 - kb_pg : y1; if (y0 < 0) y0 = 0; if (y1 <= y0) return; }
    present_rows(y0, y1);
}
/* desplazamiento que necesita el campo para quedar sobre el teclado (sin contar la hoja de ajustes) */
static int kb_need(void)
{
    if (kb_fy1 <= 0 || kb_area > 0) return 0;
    int top = H - (kb_keys_h() + kb_hadj) + kb_off;
    int n = kb_fy1 + KB_MARGIN - top, mx = kb_fy0 - KB_SB - 8;   /* sin meter el campo bajo la barra */
    if (n > mx) n = mx;
    return n > 0 ? n : 0;
}

static int kb_hit(int x, int y)
{
    if (y < kb_keys_top() || y >= kb_bar_y()) return -1;
    int best = -1, bd = 1 << 30, ry = y - kb_keys_y0();
    for (int i = 0; i < nkeys; i++) {   /* la tecla mas cercana (sin huecos muertos) */
        struct key *k = &kbk[i];
        int dx = x < k->x ? k->x - x : x >= k->x + k->w ? x - k->x - k->w + 1 : 0;
        int dy = ry < k->y ? k->y - ry : ry >= k->y + k->h ? ry - k->y - k->h + 1 : 0;
        int d = dx * dx + dy * dy * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* ------------------------------------------------------------------ caracteres */
static void put_utf8(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; o[1] = 0; }
    else if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); o[2] = 0; }
    else { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); o[3] = 0; }
}
static int up_case(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & 1 ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c : c - 1;
    return c;
}
static int kb_upper(void) { return kb_layer == 0 && (kb_shift || kb_caps); }
/* variantes de la pulsacion larga (como iOS, segun la distribucion) */
static const char *kb_alts(int c)
{
    int L = kb_cfg_layout;
    switch (c) {
    case 'a': return L == 2 ? "àâæáäãåā" : L == 1 ? "àáâäæãåā" : "áàäâãåæª";
    case 'e': return L == 2 ? "éèêëēėę" : L == 1 ? "èéêëēėę" : "éèëêęėē";
    case 'i': return L == 2 ? "îïìíįī" : L == 1 ? "îïíīįì" : "íïìîįī";
    case 'o': return L == 2 ? "ôœöòóõøō" : L == 1 ? "ôöòóœøōõ" : "óòöôõøœº";
    case 'u': return L == 2 ? "ùûüúū" : L == 1 ? "ûüùúū" : "úüùûū";
    case 'c': return "çćč";
    case 'n': return "ñń";
    case 's': return "ßśš";
    case 'y': return "ÿ";
    case 'z': return "žźż";
    case 'l': return "ł";
    case '?': return "¿";
    case '!': return "¡";
    case '-': return "–—•";
    case '/': return "\\";
    case '$': return "€£¥¢";
    case 0x20AC: return "$£¥¢";
    case '&': return "§";
    case '.': return "…";
    case '\'': return "‘’`";
    case '"': return "“”„«»";
    case '0': return "°";
    case '%': return "‰";
    }
    return 0;
}

/* ------------------------------------------------------------------ dibujo rapido */
/* mascaras de rectangulos redondeados en cache: una por tamano, de un bloque estatico */
struct kb_mk { short w, h, r; int off; };
static struct kb_mk kb_mks[24]; static int kb_nmk, kb_mk_used;
static uint8_t kb_mk_pool[64 * 1024];
static const uint8_t *kb_mask(int w, int h, int r)
{
    for (int i = 0; i < kb_nmk; i++) if (kb_mks[i].w == w && kb_mks[i].h == h && kb_mks[i].r == r) return kb_mk_pool + kb_mks[i].off;
    if (w <= 0 || h <= 0 || w * h > (int)sizeof kb_mk_pool) return 0;
    if (kb_nmk == 24 || kb_mk_used + w * h > (int)sizeof kb_mk_pool) kb_nmk = kb_mk_used = 0;
    uint8_t *m = kb_mk_pool + kb_mk_used;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float px = x + .5f, py = y + .5f;
        float qx = fabsf(px - w / 2.f) - (w / 2.f - r), qy = fabsf(py - h / 2.f) - (h / 2.f - r);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
        float cov = .5f - d;
        m[y * w + x] = cov <= 0 ? 0 : cov >= 1 ? 255 : (uint8_t)(cov * 255);
    }
    kb_mks[kb_nmk++] = (struct kb_mk){ (short)w, (short)h, (short)r, kb_mk_used }; kb_mk_used += w * h;
    return m;
}
static inline uint16_t kb_mix(uint16_t d, uint16_t c, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)(((((c >> 11) * a + (d >> 11) * na) / 255) << 11) | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
static inline void kb_px(uint16_t *d, int x, int y, uint16_t c, float cov)
{
    if (cov <= 0 || (unsigned)x >= W || (unsigned)y >= H) return;
    unsigned a = cov >= 1 ? 255 : (unsigned)(cov * 255);
    d[y * W + x] = a >= 255 ? c : kb_mix(d[y * W + x], c, a);
}
static void kb_blit(uint16_t *d, int x, int y, int w, int h, int r, uint16_t c, int alpha, int cy0, int cy1)
{
    const uint8_t *m = kb_mask(w, h, r); if (!m) return;
    if (cy0 < 0) cy0 = 0; if (cy1 > H) cy1 = H;
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy; if (dy < cy0 || dy >= cy1) continue;
        uint16_t *row = d + dy * W; const uint8_t *mr = m + yy * w;
        for (int xx = 0; xx < w; xx++) {
            int dx = x + xx; if ((unsigned)dx >= W) continue;
            unsigned a = mr[xx]; if (!a) continue;
            if (alpha < 255) a = a * alpha / 255;
            row[dx] = a >= 255 ? c : kb_mix(row[dx], c, a);
        }
    }
}
static void kb_fill(uint16_t *d, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; } if (x + w > W) w = W - x;
    if (y < 0) { h += y; y = 0; } if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) { uint16_t *row = d + yy * W + x; for (int i = 0; i < w; i++) row[i] = c; }
}
/* trazo suavizado de una polilinea (distancia minima a los segmentos: un solo fundido por pixel) */
static void kb_stroke(uint16_t *d, const float *p, int n, int closed, float cx, float cy, float s, float w, uint16_t c)
{
    float q[32]; if (n > 16) n = 16;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        q[2 * i] = cx + p[2 * i] * s; q[2 * i + 1] = cy + p[2 * i + 1] * s;
        x0 = fminf(x0, q[2 * i]); x1 = fmaxf(x1, q[2 * i]); y0 = fminf(y0, q[2 * i + 1]); y1 = fmaxf(y1, q[2 * i + 1]);
    }
    int ns = closed ? n : n - 1;
    for (int y = (int)(y0 - w - 1); y <= (int)(y1 + w + 1); y++) for (int x = (int)(x0 - w - 1); x <= (int)(x1 + w + 1); x++) {
        float px = x + .5f, py = y + .5f, best = 1e9f;
        for (int i = 0; i < ns; i++) {
            float ax = q[2 * i], ay = q[2 * i + 1], bx = q[2 * ((i + 1) % n)], by = q[2 * ((i + 1) % n) + 1];
            float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
            float t = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
            float dx = px - ax - t * vx, dy = py - ay - t * vy, dd = dx * dx + dy * dy;
            if (dd < best) best = dd;
        }
        kb_px(d, x, y, c, w * .5f + .5f - sqrtf(best));
    }
}
/* poligono convexo relleno y suavizado */
static void kb_fillpoly(uint16_t *d, const float *p, int n, float cx, float cy, float s, uint16_t c)
{
    float q[32]; if (n > 16) n = 16;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, area = 0;
    for (int i = 0; i < n; i++) {
        q[2 * i] = cx + p[2 * i] * s; q[2 * i + 1] = cy + p[2 * i + 1] * s;
        x0 = fminf(x0, q[2 * i]); x1 = fmaxf(x1, q[2 * i]); y0 = fminf(y0, q[2 * i + 1]); y1 = fmaxf(y1, q[2 * i + 1]);
    }
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; area += q[2 * i] * q[2 * j + 1] - q[2 * j] * q[2 * i + 1]; }
    float sg = area > 0 ? 1.f : -1.f;
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++) for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
        float px = x + .5f, py = y + .5f, m = 1e9f;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n; float ex = q[2 * j] - q[2 * i], ey = q[2 * j + 1] - q[2 * i + 1], l = sqrtf(ex * ex + ey * ey);
            if (l <= 0) continue;
            float sd = sg * (ex * (py - q[2 * i + 1]) - ey * (px - q[2 * i])) / l;
            if (sd < m) m = sd;
        }
        kb_px(d, x, y, c, m + .5f);
    }
}
static void kb_ellipse(uint16_t *d, float cx, float cy, float rx, float ry, float w, uint16_t c)
{
    for (int y = (int)(cy - ry - w - 1); y <= (int)(cy + ry + w + 1); y++) for (int x = (int)(cx - rx - w - 1); x <= (int)(cx + rx + w + 1); x++) {
        float dx = x + .5f - cx, dy = y + .5f - cy, u = dx / rx, v = dy / ry, f = sqrtf(u * u + v * v);
        if (f <= 0) continue;
        float gx = dx / (rx * rx) / f, gy = dy / (ry * ry) / f, g = sqrtf(gx * gx + gy * gy);
        kb_px(d, x, y, c, w * .5f + .5f - fabsf(f - 1) / (g > 0 ? g : 1));
    }
}
/* iconos vectoriales (no dependen de la fuente) */
static void kb_icon_shift(uint16_t *d, float cx, float cy, float s, int filled, int caps, uint16_t c)
{
    static const float A[] = { 0, -7.5f, 7.5f, 0, 3.6f, 0, 3.6f, 6.5f, -3.6f, 6.5f, -3.6f, 0, -7.5f, 0 };
    static const float T[] = { 0, -8.3f, 8.3f, .4f, -8.3f, .4f };
    static const float R[] = { -4.3f, -.3f, 4.3f, -.3f, 4.3f, 7.2f, -4.3f, 7.2f };
    static const float U[] = { -4.3f, 9.3f, 4.3f, 9.3f, 4.3f, 10.9f, -4.3f, 10.9f };
    if (caps) cy -= 1.5f * s;
    if (!filled) { kb_stroke(d, A, 7, 1, cx, cy, s, 1.5f, c); return; }
    kb_fillpoly(d, T, 3, cx, cy, s, c); kb_fillpoly(d, R, 4, cx, cy, s, c);
    if (caps) kb_fillpoly(d, U, 4, cx, cy, s, c);
}
static void kb_icon_back(uint16_t *d, float cx, float cy, float s, uint16_t c)
{
    static const float B[] = { -10, 0, -4, -7, 10, -7, 10, 7, -4, 7 };
    static const float X1[] = { -.5f, -3, 5.5f, 3 }, X2[] = { -.5f, 3, 5.5f, -3 };
    kb_stroke(d, B, 5, 1, cx, cy, s, 1.5f, c);
    kb_stroke(d, X1, 2, 0, cx, cy, s, 1.5f, c); kb_stroke(d, X2, 2, 0, cx, cy, s, 1.5f, c);
}
static void kb_icon_globe(uint16_t *d, float cx, float cy, uint16_t c)
{
    static const float E[] = { -9, 0, 9, 0 }, L1[] = { -7.6f, -4.6f, 7.6f, -4.6f }, L2[] = { -7.6f, 4.6f, 7.6f, 4.6f };
    kb_ellipse(d, cx, cy, 9, 9, 1.4f, c); kb_ellipse(d, cx, cy, 4, 9, 1.3f, c);
    kb_stroke(d, E, 2, 0, cx, cy, 1, 1.3f, c); kb_stroke(d, L1, 2, 0, cx, cy, 1, 1.2f, c); kb_stroke(d, L2, 2, 0, cx, cy, 1, 1.2f, c);
}
static void kb_icon_hide(uint16_t *d, float cx, float cy, uint16_t c)   /* teclado con flecha abajo */
{
    /* Todo en la rejilla de pixeles (bordes en enteros, trazos del marco centrados en .5) para que
     * salga nitido: marco 21x15 con esquinas achaflanadas, dos filas de 5 teclas 2x2 (paso 4) y la
     * barra espaciadora, con ~2 px libres hasta el marco por los cuatro lados. */
    static const float K[] = { -10, -10.5f, 10, -10.5f, 11.5f, -9, 11.5f, 3, 10, 4.5f, -10, 4.5f, -11.5f, 3, -11.5f, -9 };
    static const float V[] = { -4, 7.5f, 0, 11, 4, 7.5f };
    kb_stroke(d, K, 8, 1, cx, cy, 1, 1.3f, c);
    for (int r = 0; r < 2; r++) for (int i = 0; i < 5; i++) {
        float x = -9 + i * 4, y = -8 + r * 4;
        float q[8] = { x, y, x + 2, y, x + 2, y + 2, x, y + 2 };
        kb_fillpoly(d, q, 4, cx, cy, 1, c);
    }
    static const float S[] = { -5, 0, 5, 0, 5, 2, -5, 2 };
    kb_fillpoly(d, S, 4, cx, cy, 1, c);
    kb_stroke(d, V, 3, 0, cx, cy, 1, 1.5f, c);
}
static void kb_icon_check(uint16_t *d, float cx, float cy, uint16_t c)
{
    static const float C[] = { -5, 0, -1.5f, 3.8f, 5.5f, -4.5f };
    kb_stroke(d, C, 3, 0, cx, cy, 1, 1.8f, c);
}

/* iconos en cache: se rasterizan una vez (por tamano y subpixel) y luego solo se funden */
enum { KI_SHIFT, KI_SHIFT_ON, KI_CAPS, KI_BACK, KI_GLOBE, KI_HIDE };
static uint8_t kb_icm[10][32 * 32]; static int kb_ick[10], kb_icn;
static void kb_icon(uint16_t *d, int id, float cx, float cy, float s, uint16_t c, int cy0)
{
    int ix = (int)floorf(cx), iy = (int)floorf(cy), fx = (int)((cx - ix) * 2), fy = (int)((cy - iy) * 2);
    int key = 1 + id * 100000 + (int)(s * 100) * 10 + fx * 3 + fy, slot = -1;
    for (int i = 0; i < 10; i++) if (kb_ick[i] == key) { slot = i; break; }
    if (slot < 0) {
        static uint16_t tile[W * 32];
        memset(tile, 0, sizeof tile);
        float ox = 16 + fx * .5f, oy = 16 + fy * .5f;
        if (id <= KI_CAPS) kb_icon_shift(tile, ox, oy, s, id != KI_SHIFT, id == KI_CAPS, 0xFFFF);
        else if (id == KI_BACK) kb_icon_back(tile, ox, oy, s, 0xFFFF);
        else if (id == KI_GLOBE) kb_icon_globe(tile, ox, oy, 0xFFFF);
        else kb_icon_hide(tile, ox, oy, 0xFFFF);
        slot = kb_icn; kb_icn = (kb_icn + 1) % 10; kb_ick[slot] = key;
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) kb_icm[slot][y * 32 + x] = (uint8_t)(((tile[y * W + x] >> 5) & 63) * 255 / 63);
    }
    const uint8_t *m = kb_icm[slot];
    for (int y = 0; y < 32; y++) {
        int dy = iy - 16 + y; if (dy < cy0 || dy >= H || dy < 0) continue;
        for (int x = 0; x < 32; x++) {
            int dx = ix - 16 + x; unsigned a = m[y * 32 + x];
            if (!a || (unsigned)dx >= W) continue;
            d[dy * W + dx] = a >= 255 ? c : kb_mix(d[dy * W + dx], c, a);
        }
    }
}

/* zona salvada bajo la burbuja (lo de encima del teclado se restaura sin repintar la pantalla) */
static uint16_t kb_ovbuf[36000];
static int kb_ov_on, kb_ov_x, kb_ov_y, kb_ov_w, kb_ov_h; static uint16_t *kb_ov_dst;
static void kb_ov_save(uint16_t *d, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    int w = x1 - x0, h = y1 - y0; if (w <= 0 || h <= 0) return;
    if (w * h > (int)(sizeof kb_ovbuf / 2)) h = (int)(sizeof kb_ovbuf / 2) / w;
    for (int y = 0; y < h; y++) memcpy(kb_ovbuf + y * w, d + (y0 + y) * W + x0, w * 2);
    kb_ov_on = 1; kb_ov_x = x0; kb_ov_y = y0; kb_ov_w = w; kb_ov_h = h; kb_ov_dst = d;
}
static void kb_ov_restore(void)
{
    if (!kb_ov_on) return;
    for (int y = 0; y < kb_ov_h; y++) memcpy(kb_ov_dst + (kb_ov_y + y) * W + kb_ov_x, kb_ovbuf + y * kb_ov_w, kb_ov_w * 2);
    kb_ov_on = 0;
}
/* cuello de la burbuja: de [tl,tr] en la fila ya a [bl,br] en yb, con curva suave */
static void kb_neck(uint16_t *d, float tl, float tr, int ya, float bl, float br, int yb, uint16_t c)
{
    for (int y = ya; y < yb; y++) {
        if (y < 0 || y >= H) continue;
        float t = (y + .5f - ya) / (yb - ya), s = t * t * (3 - 2 * t);
        float l = tl + (bl - tl) * s, r = tr + (br - tr) * s;
        for (int x = (int)l; x <= (int)r; x++) kb_px(d, x, y, c, fminf(x + 1.f, r) - fmaxf((float)x, l));
    }
}

/* variantes abiertas (pulsacion larga) */
static int kb_alt_n, kb_alt_sel, kb_alt_key, kb_alt_dir, kb_alt_cp[8];
static int kb_alt_open(int k)
{
    const char *s = kb_alts(kbk[k].code); if (!s) return 0;
    kb_alt_n = 0; int c;
    while (kb_alt_n < 8 && (c = utf8_next(&s))) { int gi; font_glyph(c, &gi); if (gi) kb_alt_cp[kb_alt_n++] = c; }
    if (!kb_alt_n) return 0;
    kb_alt_key = k; kb_alt_sel = 0; kb_alt_dir = kbk[k].x + kbk[k].w / 2 < W / 2 ? 1 : -1;
    return 1;
}
static void kb_alt_geom(int *px, int *pw, int *cw)
{
    struct key *k = &kbk[kb_alt_key];
    *cw = k->w + 2 < 30 ? 30 : k->w + 2; if (*cw > 36) *cw = 36;
    *pw = kb_alt_n * *cw + 8;
    int c = k->x + k->w / 2 - *cw / 2 - 4;
    *px = kb_alt_dir > 0 ? c : c - (kb_alt_n - 1) * *cw;
    if (*px < 2) *px = 2; if (*px + *pw > W - 2) *px = W - 2 - *pw;
}
static int kb_alt_pick(int x)
{
    int px, pw, cw; kb_alt_geom(&px, &pw, &cw);
    int s = (x - px - 4) / cw; if (x < px + 4) s = 0; if (s >= kb_alt_n) s = kb_alt_n - 1; if (s < 0) s = 0;
    return kb_alt_dir > 0 ? s : kb_alt_n - 1 - s;
}

/* burbuja de la tecla pulsada (o fila de variantes) con su cuello, al estilo iOS */
static void kb_draw_popup(uint16_t *d, int i, int y0)
{
    struct key *k = &kbk[i]; int kx = k->x, ky = y0 + k->y, kw = k->w, kh = k->h;
    int alt = kb_alt_n && kb_alt_key == i;
    int bx, bw, cw = 0, bh = kh + 4, na = ky - 10, nb = ky + 8, by = na - bh;
    if (alt) kb_alt_geom(&bx, &bw, &cw);
    else { bw = kw + 18; if (bw < 44) bw = 44; bx = kx + kw / 2 - bw / 2; if (bx < 2) bx = 2; if (bx + bw > W - 2) bx = W - 2 - bw; }
    kb_ov_save(d, (bx < kx ? bx : kx) - 2, by - 1, (bx + bw > kx + kw ? bx + bw : kx + kw) + 2, ky + kh + 3);
    fill_rrect(d, bx, by + 1, bw, bh + 10, 11, 0, 90);
    kb_blit(d, kx, ky, kw, kh, 6, KB_C_KEY, 255, 0, H);
    fill_rrect(d, bx, by, bw, bh + 10, 11, KB_C_KEY, 255);
    float tl = alt ? fmaxf((float)bx, kx - 8.f) : bx, tr = alt ? fminf((float)(bx + bw), kx + kw + 8.f) : bx + bw;
    kb_neck(d, tl, tr, na, kx, kx + kw, nb, KB_C_KEY);
    char lb[8];
    if (!alt) {
        put_utf8(lb, kb_upper() ? up_case(k->code) : k->code);
        int w = tw_n(lb, 30, -1);
        draw_text(d, bx + (bw - w) / 2, by + bh / 2 + 11, lb, 30, 0xFFFF, 0, 0, H);
        return;
    }
    for (int j = 0; j < kb_alt_n; j++) {
        int s = kb_alt_dir > 0 ? j : kb_alt_n - 1 - j, cx = bx + 4 + s * cw;
        if (j == kb_alt_sel) kb_blit(d, cx, by + 4, cw, bh - 4, 7, KB_C_BLUE, 255, 0, H);
        put_utf8(lb, kb_upper() ? up_case(kb_alt_cp[j]) : kb_alt_cp[j]);
        int w = tw_n(lb, 22, -1);
        draw_text(d, cx + (cw - w) / 2, by + bh / 2 + 9, lb, 22, 0xFFFF, 0, 0, H);
    }
}

static void kb_draw_key(uint16_t *d, int i, int pressed, int y0, int cy0, int shadow)
{
    struct key *k = &kbk[i]; int x = k->x, y = y0 + k->y, w = k->w, h = k->h;
    int special = k->code < 0 && k->code != K_SPACE;
    uint16_t bg = special ? KB_C_SPEC : KB_C_KEY, fg = 0xFFFF;
    int shift_on = k->code == K_SHIFT && kb_layer == 0 && (kb_shift || kb_caps);
    if (pressed && special) bg = KB_C_KEY;                  /* iOS: la especial pulsada se aclara */
    if (pressed && k->code == K_SPACE) bg = KB_C_SPEC;
    if (shift_on) { bg = rgb(235, 235, 240); fg = rgb(20, 20, 22); }
    if (k->code == K_ENTER) bg = pressed ? KB_C_KEY : KB_C_BLUE;
    if (kb_trackpad) bg = rgb(52, 52, 55);
    if (y + h + 1 <= cy0) return;
    if (shadow) kb_blit(d, x, y + 1, w, h, 6, 0, 120, y + h - 5 > cy0 ? y + h - 5 : cy0, H);   /* sombra inferior */
    kb_blit(d, x, y, w, h, 6, bg, 255, cy0, H);
    if (kb_trackpad) return;
    float s = h / 42.f; if (s < .85f) s = .85f; if (s > 1.1f) s = 1.1f;
    if (k->code == K_SHIFT && kb_layer == 0) { kb_icon(d, !shift_on ? KI_SHIFT : kb_caps ? KI_CAPS : KI_SHIFT_ON, x + w / 2.f, y + h / 2.f + .5f, s, fg, cy0); return; }
    if (k->code == K_BACK) { kb_icon(d, KI_BACK, x + w / 2.f - .5f, y + h / 2.f, s, fg, cy0); return; }
    char lb[8]; const char *l = k->lbl;
    if (k->code > 0 && kb_upper()) { put_utf8(lb, up_case(k->code)); l = lb; }
    int size = k->code > 0 ? (h >= 38 ? 21 : 19) : (k->code == K_SPACE ? 14 : 15);
    int tw = tw_n(l, size, -1);
    draw_text(d, x + (w - tw) / 2, y + h / 2 + size * 7 / 20, l, size, fg, 0, cy0, H);
}

/* interruptor iOS (t: 0 apagado .. 1 encendido) */
static void kb_switch(uint16_t *d, int x, int y, float t, int cy0, int cy1)
{
    uint16_t off = rgb(57, 57, 61), on = rgb(48, 209, 88);
    int r = (int)((off >> 11) + ((int)(on >> 11) - (int)(off >> 11)) * t), g = (int)(((off >> 5) & 63) + ((int)((on >> 5) & 63) - (int)((off >> 5) & 63)) * t), b = (int)((off & 31) + ((int)(on & 31) - (int)(off & 31)) * t);
    kb_blit(d, x, y, 38, 22, 11, (uint16_t)(r << 11 | g << 5 | b), 255, cy0, cy1);
    int kx = x + 2 + (int)(t * 16 + .5f);
    kb_blit(d, kx, y + 3, 18, 18, 9, 0, 60, cy0, cy1);
    kb_blit(d, kx, y + 2, 18, 18, 9, 0xFFFF, 255, cy0, cy1);
}
static const char *const KB_LNAMES[3] = { "Español", "English", "Français" };
static float kb_seg_pos = -1, kb_sw_t[3] = { -1, -1, -1 };   /* animaciones de la hoja (-1: en reposo) */
static int kb_sheet_press = -1;
static int kb_sheet_top(void) { return KB_YR + kb_sheet_h - KB_SH; }
static void kb_draw_sheet(uint16_t *d)
{
    int sy0 = KB_YR < 0 ? 0 : KB_YR, sy1 = KB_YR + kb_sheet_h, cy = kb_sheet_top();
    if (sy1 > H) sy1 = H;
    if (sy1 <= sy0) return;
    kb_blit(d, 0, cy, W, 24, 12, KB_C_SHEET, 255, sy0, cy + 12 < sy1 ? cy + 12 : sy1);
    kb_fill(d, 0, cy + 12 > sy0 ? cy + 12 : sy0, W, sy1 - (cy + 12 > sy0 ? cy + 12 : sy0), KB_C_SHEET);
    draw_text(d, 16, cy + 20, tr("Teclado"), 16, 0xFFFF, 0, sy0, sy1);
    const char *ok = tr("Listo");
    draw_text(d, W - 16 - tw_n(ok, 16, -1), cy + 20, ok, 16, kb_sheet_press == 0 ? rgb(10, 70, 140) : KB_C_BLUE, 0, sy0, sy1);
    int tx = 12, ty = cy + 29, tw = W - 24, segw = (tw - 4) / 3;
    kb_blit(d, tx, ty, tw, 30, 8, rgb(44, 44, 47), 255, sy0, sy1);
    float sp = kb_seg_pos >= 0 ? kb_seg_pos : (float)kb_cfg_layout;
    kb_blit(d, tx + 2 + (int)(sp * segw + .5f), ty + 2, segw, 26, 7, rgb(99, 99, 102), 255, sy0, sy1);
    for (int i = 0; i < 3; i++) {
        int w = tw_n(KB_LNAMES[i], 13, -1);
        draw_text(d, tx + 2 + i * segw + (segw - w) / 2, ty + 20, KB_LNAMES[i], 13, 0xFFFF, 0, sy0, sy1);
    }
    const char *lab[3] = { tr("Fila de números"), tr("Mayúsculas automáticas"), tr("Atajo del punto") };
    int val[3] = { kb_cfg_numrow, kb_cfg_autocap, kb_cfg_period };
    for (int i = 0; i < 3; i++) {
        int ry = cy + 64 + i * 26;
        if (i && ry - 1 >= sy0 && ry - 1 < sy1) kb_fill(d, 16, ry - 1, W - 16, 1, rgb(48, 48, 51));
        draw_text(d, 16, ry + 18, lab[i], 14, 0xFFFF, W - 80, sy0, sy1);
        kb_switch(d, W - 16 - 38, ry + 2, kb_sw_t[i] >= 0 ? kb_sw_t[i] : (float)val[i], sy0, sy1);
    }
    int gy = cy + 142;
    kb_blit(d, W / 2 - 18, gy + 4, 36, 5, 2, rgb(120, 120, 126), 255, sy0, sy1);
    const char *hint = tr("Arrastra para cambiar la altura");
    draw_text(d, (W - tw_n(hint, 11, -1)) / 2, gy + 19, hint, 11, rgb(142, 142, 147), 0, sy0, sy1);
}
static int kb_menu_geom(int *mx, int *my, int *mw, int *ih) { *mx = 6; *mw = 214; *ih = 36; *my = kb_bar_y() - 2 - 4 * *ih; return 4; }
static int kb_menu_item(int x, int y)
{
    int mx, my, mw, ih, n = kb_menu_geom(&mx, &my, &mw, &ih);
    if (x < mx || x >= mx + mw || y < my || y >= my + n * ih) return -1;
    return (y - my) / ih;
}
static void kb_draw_menu(uint16_t *d)
{
    int mx, my, mw, ih, n = kb_menu_geom(&mx, &my, &mw, &ih);
    fill_rrect(d, mx, my + 2, mw, n * ih, 13, 0, 110);
    fill_rrect(d, mx, my, mw, n * ih, 13, rgb(48, 48, 51), 255);
    for (int i = 0; i < n; i++) {
        int iy = my + i * ih;
        if (i == kb_menu_sel) kb_blit(d, mx + 4, iy + 3, mw - 8, ih - 6, 8, KB_C_BLUE, 255, 0, H);
        if (i) kb_fill(d, mx + 12, iy, mw - 24, 1, i == 1 ? rgb(24, 24, 26) : rgb(64, 64, 68));
        const char *l = i ? KB_LNAMES[i - 1] : tr("Ajustes del teclado");
        draw_text(d, mx + (i ? 34 : 16), iy + 24, l, 15, 0xFFFF, mw - 44, 0, H);
        if (i && i - 1 == kb_cfg_layout) kb_icon_check(d, mx + 18, iy + ih / 2.f, 0xFFFF);
    }
}

static double kb_t_draw;
/* teclado en reposo (fondo, teclas sin pulsar e iconos de la barra) en d, filas [cy0, H) */
static void kb_paint_base(uint16_t *d, int cy0)
{
    int ky = kb_keys_y0();
    kb_fill(d, 0, cy0, W, H - cy0, KB_C_BG);
    for (int i = 0; i < nkeys; i++) kb_draw_key(d, i, 0, ky, cy0, 1);
    int by = kb_bar_y();
    if (by < H && !kb_trackpad) {
        uint16_t ic = rgb(200, 200, 205);
        kb_icon(d, KI_GLOBE, 25, by + 14, 1, ic, cy0);
        kb_icon(d, KI_HIDE, W - 26, by + 14, 1, ic, cy0);
    }
}
/* sprites del teclado: [0] normal, [1] trackpad (teclas apagadas). Se pintan una vez por
 * distribucion/tamano/mayusculas y luego cada fotograma solo copia filas. Solo existen mientras el
 * teclado esta en pantalla (se liberan al ocultarlo y en kb_end). */
struct kb_spr { uint16_t *px; int rows, cap; unsigned key; };
static struct kb_spr kb_sp[2];
static float kb_tp_t;                         /* 0 teclas normales .. 1 trackpad (fundido) */
static unsigned kb_spr_key(int tp) { return kb_gen * 8u + (kb_shift ? 1u : 0u) + (kb_caps ? 2u : 0u) + (tp ? 4u : 0u); }
static const uint16_t *kb_spr_get(int tp)
{
    struct kb_spr *s = &kb_sp[tp];
    unsigned key = kb_spr_key(tp); int rows = kb_keys_h();
    if (s->px && s->key == key && s->rows == rows) return s->px;
    if (rows > H) return 0;
    if (rows > s->cap) { free(s->px); s->px = malloc((size_t)W * rows * 2); s->cap = s->px ? rows : 0; if (!s->px) return 0; }
    int so = kb_off, st = kb_trackpad;
    kb_off = 0; kb_trackpad = tp;
    kb_paint_base(s->px - (long)(H - rows) * W, H - rows);   /* filas [H-rows, H) del sprite */
    kb_off = so; kb_trackpad = st;
    s->key = key; s->rows = rows;
    return s->px;
}
static void kb_spr_free(void) { for (int i = 0; i < 2; i++) { free(kb_sp[i].px); kb_sp[i] = (struct kb_spr){ 0 }; } }
/* fundido rapido de dos colores RGB565 (k 0..32: a -> b) */
static inline uint16_t kb_bl(uint16_t a, uint16_t b, unsigned k)
{
    uint32_t x = (a | (uint32_t)a << 16) & 0x07E0F81F, y = (b | (uint32_t)b << 16) & 0x07E0F81F;
    uint32_t r = ((x * (32 - k) + y * k) >> 5) & 0x07E0F81F;
    return (uint16_t)(r | r >> 16);
}
/* solo el teclado (y lo que flota sobre el: burbuja, menu, hoja) en sus filas reales */
static void kb_draw_keys(uint16_t *dst, int pressed)
{
    kb_ov_on = 0;                                 /* lo salvado ya no vale: se acaba de repintar */
    if (KB_YR >= H) return;
    double t0 = now_ms();
    int y0 = KB_YR < 0 ? 0 : KB_YR, top = kb_keys_top(), ky = kb_keys_y0(), cy0 = top > y0 ? top : y0;
    if (cy0 > y0) kb_fill(dst, 0, y0, W, cy0 - y0, KB_C_BG);
    float tp = kb_tp_t;
    const uint16_t *a = tp < 1 ? kb_spr_get(0) : 0, *b = tp > 0 ? kb_spr_get(1) : 0;
    if ((tp < 1 && !a) || (tp > 0 && !b)) kb_paint_base(dst, cy0);   /* sin memoria: como antes */
    else {
        unsigned k = (unsigned)(tp * 32 + .5f);
        for (int y = cy0; y < H; y++) {
            uint16_t *o = dst + y * W; size_t r = (size_t)(y - top) * W;
            if (k == 0) memcpy(o, a + r, W * 2);
            else if (k >= 32) memcpy(o, b + r, W * 2);
            else { const uint16_t *pa = a + r, *pb = b + r; for (int x = 0; x < W; x++) o[x] = kb_bl(pa[x], pb[x], k); }
        }
    }
    int tpv = kb_trackpad || tp > 0;
    if (pressed >= 0 && pressed < nkeys && kbk[pressed].code <= 0 && !tpv) kb_draw_key(dst, pressed, 1, ky, cy0, 0);
    int by = kb_bar_y();
    if (by < H && !tpv && (kb_bar_press || kb_menu)) {
        uint16_t ic = rgb(200, 200, 205);
        if (kb_bar_press == 1 || kb_menu) { kb_blit(dst, 6, by + 1, 38, 26, 8, KB_C_SPEC, 255, cy0, H); kb_icon(dst, KI_GLOBE, 25, by + 14, 1, ic, cy0); }
        if (kb_bar_press == 2) { kb_blit(dst, W - 46, by + 1, 40, 26, 8, KB_C_SPEC, 255, cy0, H); kb_icon(dst, KI_HIDE, W - 26, by + 14, 1, ic, cy0); }
    }
    if (kb_sheet_h > 0) kb_draw_sheet(dst);
    if (kb_menu) kb_draw_menu(dst);
    if (pressed >= 0 && pressed < nkeys && kbk[pressed].code > 0 && !tpv) kb_draw_popup(dst, pressed, ky);
    kb_t_draw = now_ms() - t0;
}
/* filas [y0, y1) de la pantalla <- filas desplazadas s de la pagina src (src puede ser dst) */
static void kb_shift_rows(uint16_t *dst, const uint16_t *src, int s, int y0, int y1)
{
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        int sy = y + s;
        if (sy >= 0 && sy < H) { if (dst != src || s) memmove(dst + y * W, src + sy * W, W * 2); }
        else memset(dst + y * W, 0, W * 2);
    }
}
/* cursor flotante del trackpad (iOS): sigue al dedo entre caracteres; el del llamante queda gris */
static int kb_tpmode, kb_tp_res;              /* 0 nada, 1 deslizar (Gboard), 2 trackpad; resto en px */
static int kb_fc_on, kb_fc_x, kb_fc_y, kb_fc_h; static uint16_t kb_fc_save[4 * 72]; static uint16_t *kb_fc_dst;
static void kb_fc_put(uint16_t *d)
{
    if (!kb_c_seen) return;
    int y0 = kb_cy0, y1 = kb_cy1;
    if (y0 >= KB_SB) { y0 -= kb_pg; y1 -= kb_pg; }
    if (y0 < KB_SB + 2) y0 = KB_SB + 2;
    if (y1 > KB_YR - 2) y1 = KB_YR - 2;
    if (y1 - y0 < 4) return;
    int x = kb_cx + kb_tp_res - 1, y = y0 - 2, h = y1 - y0 + 4;
    if (x < 1) x = 1;
    if (x > W - 5) x = W - 5;
    if (h > 72) h = 72;
    for (int r = 0; r < h; r++) memcpy(kb_fc_save + r * 4, d + (y + r) * W + x, 8);
    kb_fc_on = 1; kb_fc_x = x; kb_fc_y = y; kb_fc_h = h; kb_fc_dst = d;
    kb_blit(d, x, y, 3, h, 1, KB_C_BLUE, 255, 0, KB_YR);
}
static void kb_fc_draw(uint16_t *d)
{
    kb_fc_on = 0;
    if (kb_tpmode != 2 || !kb_c_seen) return;
    int y0 = kb_cy0, y1 = kb_cy1, x = kb_cx;
    if (y0 >= KB_SB) { y0 -= kb_pg; y1 -= kb_pg; }
    if (y0 < KB_SB) y0 = KB_SB;
    if (y1 > KB_YR) y1 = KB_YR;
    if (y1 - y0 >= 4 && x >= 0 && x < W - 2) {       /* el cursor real se queda como fantasma gris */
        uint16_t c = d[((y0 + y1) / 2) * W + x];
        if ((c & 31) > 18 && (c >> 11) < 12)
            for (int y = y0; y < y1; y++) for (int xx = x; xx < x + 2; xx++) if (d[y * W + xx] == c) d[y * W + xx] = rgb(110, 110, 116);
    }
    kb_fc_put(d);
}
/* solo se ha movido el cursor flotante: se restaura lo de debajo y se presentan sus filas */
static void kb_fc_move(void)
{
    if (!kb_fc_on) return;
    uint16_t *d = kb_fc_dst; int oy = kb_fc_y, oh = kb_fc_h;
    for (int r = 0; r < kb_fc_h; r++) memcpy(d + (kb_fc_y + r) * W + kb_fc_x, kb_fc_save + r * 4, 8);
    kb_fc_on = 0; kb_fc_put(d);
    int y0 = oy, y1 = oy + oh;
    if (kb_fc_on && kb_fc_y < y0) y0 = kb_fc_y;
    if (kb_fc_on && kb_fc_y + kb_fc_h > y1) y1 = kb_fc_y + kb_fc_h;
    present_rows(y0, y1);
}
/* pinta el teclado en dst: los llamantes lo llaman al final de su redibujado (la pagina ya esta en
 * dst, en coordenadas de pagina). Con desplazamiento, sube la pagina en el sitio y pinta encima. */
static void kb_draw(uint16_t *dst, int pressed)
{
    if (kb_nodraw) return;                        /* instantanea de la pagina (animaciones) */
    if (kb_pg > 0 && KB_YR < H) kb_shift_rows(dst, dst, kb_pg, KB_SB, KB_YR);
    kb_draw_keys(dst, pressed);
    kb_fc_draw(dst);
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
static void eb_back_word(struct ebuf *e)
{
    while (e->cur > 0 && (e->b[e->cur - 1] == ' ' || e->b[e->cur - 1] == '\n')) eb_back(e);
    while (e->cur > 0 && e->b[e->cur - 1] != ' ' && e->b[e->cur - 1] != '\n') eb_back(e);
}
static void eb_move(struct ebuf *e, int n)
{
    for (; n > 0 && e->cur < e->len; n--) { e->cur++; while (e->cur < e->len && ((unsigned char)e->b[e->cur] & 0xC0) == 0x80) e->cur++; }
    for (; n < 0 && e->cur > 0; n++) { e->cur--; while (e->cur > 0 && ((unsigned char)e->b[e->cur] & 0xC0) == 0x80) e->cur--; }
}
/* principio de frase: vacio, tras salto de linea o tras ". " "! " "? " (y tras ¿ ¡ en esa posicion) */
static int kb_sentence_start(const struct ebuf *e)
{
    int p = e->cur; const unsigned char *b = (const unsigned char *)e->b;
    if (p >= 2 && b[p - 2] == 0xC2 && (b[p - 1] == 0xBF || b[p - 1] == 0xA1)) p -= 2;
    if (p <= 0 || b[p - 1] == '\n') return 1;
    if (b[p - 1] != ' ') return 0;
    while (p > 0 && b[p - 1] == ' ') p--;
    if (p == 0) return 1;
    return b[p - 1] == '.' || b[p - 1] == '!' || b[p - 1] == '?' || b[p - 1] == '\n';
}
static void kb_autoshift(const struct ebuf *e)
{
    if (kb_caps) return;
    if (kb_cfg_autocap && kb_layer == 0 && kb_sentence_start(e)) { kb_shift = 1; kb_shift_auto = 1; }
    else if (kb_shift_auto) { kb_shift = 0; kb_shift_auto = 0; }
}

/* ------------------------------------------------------------------ interaccion */
static struct ebuf *kb_e; static uint16_t *kb_dst; static void (*kb_rd)(uint16_t *, int); static int kb_ml;
/* medidas (se escriben en el registro cada 20 teclas y al cerrar el editor) */
static int kb_st_n; static double kb_st_lat, kb_st_latmax, kb_st_draw, kb_st_drawmax, kb_st_com, kb_st_commax;
static void kb_stats_flush(void)
{
    if (!kb_st_n) return;
    lg("teclado: %d teclas | toque->visible media %.1f max %.1f ms | pintar teclado %.2f max %.2f ms | escribir+repintar %.1f max %.1f ms",
       kb_st_n, kb_st_lat / kb_st_n, kb_st_latmax, kb_st_draw / kb_st_n, kb_st_drawmax, kb_st_com / kb_st_n, kb_st_commax);
    kb_st_n = 0; kb_st_lat = kb_st_latmax = kb_st_draw = kb_st_drawmax = kb_st_com = kb_st_commax = 0;
}
static double kb_now_real(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double kb_evt_ms(const struct pkt *p)
{
    if (!p->n) return kb_now_real();
#ifdef input_event_sec
    return p->ev[0].input_event_sec * 1e3 + p->ev[0].input_event_usec / 1e3;
#else
    return p->ev[0].time.tv_sec * 1e3 + p->ev[0].time.tv_usec / 1e3;
#endif
}
/* lee el tactil y se queda con la ultima posicion (sin acumular retraso); 1 si sigue pulsado.
 * kb_last_evt: hora (del nucleo) del ultimo paquete leido, para medir toque -> pantalla */
static double kb_last_evt;
static int kb_pump(struct pkt *p, int wait)
{
    if (read_pkt(p, wait)) { kb_last_evt = kb_evt_ms(p); while (cur_down && read_pkt(p, 0)) kb_last_evt = kb_evt_ms(p); }
    return cur_down;
}
/* solo el teclado (y la burbuja): presentacion parcial, sin repintar la pantalla del llamante */
static void kb_show(void)
{
    int top = KB_YR; if (kb_ov_on && kb_ov_y < top) top = kb_ov_y;
    kb_ov_restore();
    kb_draw_keys(kb_dst, kb_pressed);
    if (kb_ov_on && kb_ov_y < top) top = kb_ov_y;
    present_rows(top, H);
}
/* pantalla completa del llamante (el texto ha cambiado o el teclado cambia de alto) */
static void kb_full(void) { kb_ov_on = 0; kb_c_seen = 0; kb_rd(kb_dst, kb_pressed); present(); }
/* anima una variable de geometria con ease-out (con el redibujado del llamante: el contenido sigue).
 * El desplazamiento de la pagina sigue al borde del teclado (altura de la fila de numeros). */
static void kb_anim(int *v, int to, int ms)
{
    int from = *v, n = 0; double t0 = now_ms() - 16, rs = 0, rmax = 0, ks = 0;
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1;
        *v = from + (int)lround((to - from) * ease_out(t));
        kb_pg = kb_need();
        double a = now_ms(); kb_ov_on = 0; kb_c_seen = 0; kb_rd(kb_dst, -1); a = now_ms() - a; rs += a; ks += kb_t_draw; if (a > rmax) rmax = a;
        present(); n++;
        if (t >= 1) break;
    }
    lg("teclado: animacion %d->%d en %d fotogramas (%.0f ms), repintado medio %.1f max %.1f ms (teclado %.1f)", from, to, n, now_ms() - t0, rs / n, rmax, ks / n);
}
static void kb_ctx(struct ebuf *e, uint16_t *dst, void (*redraw)(uint16_t *, int)) { kb_e = e; kb_dst = dst; kb_rd = redraw; }

/* ------------------------------------------------------------------ entrada / salida */
/* curva de las animaciones del teclado: cubic-bezier(.2, .85, .25, 1), arranque rapido y llegada
 * muy suave, como el teclado de iOS */
static double kb_curve(double t)
{
    const double x1 = .2, y1 = .85, x2 = .25, y2 = 1;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    double cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    double cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    double u = t;
    for (int i = 0; i < 8; i++) {   /* x(u) = t por Newton (y biseccion si se tuerce) */
        double x = ((ax * u + bx) * u + cx) * u - t, dx = (3 * ax * u + 2 * bx) * u + cx;
        if (fabs(x) < 1e-5) break;
        if (fabs(dx) < 1e-6) break;
        u -= x / dx;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
    }
    return ((ay * u + by) * u + cy) * u;
}
#define KB_T_OPEN 300
#define KB_T_CLOSE 260
/* instantanea de la pagina del llamante en buf (sin teclado). away: como si el teclado no tapara
 * nada (KB_Y = H) pero sigue "abierto" para el llamante (cursor visible). Si no, el estado actual. */
static void kb_snap(uint16_t *buf, int away)
{
    int so = kb_off, sp = kb_pg, sv = kb_vy;
    if (away) { kb_off = 0; kb_pg = 0; kb_vy = KB_H; }
    kb_nodraw = 1; kb_c_seen = 0; kb_ov_on = 0;
    kb_rd(buf, -1);
    kb_nodraw = 0; kb_off = so; kb_pg = sp; kb_vy = sv;
}
static uint16_t *kb_pgbuf;                    /* instantanea de la pagina: solo durante las animaciones */
static uint16_t *kb_pgbuf_get(void) { if (!kb_pgbuf) kb_pgbuf = malloc(HM_FRAME_BYTES); return kb_pgbuf; }
static void kb_pgbuf_free(void) { free(kb_pgbuf); kb_pgbuf = 0; }
/* un fotograma: pagina (instantanea) desplazada s filas (o ds en el area de un editor de varias
 * lineas) + teclado con su borde en off. Solo se copian las filas que cambian; devuelve la primera */
static int kb_cmp_top = -1, kb_cmp_s, kb_cmp_ds;
static int kb_compose(const uint16_t *pg, int off, int s, int ds)
{
    uint16_t *d = kb_dst;
    int so = kb_off; kb_off = off;
    int top = KB_YR; if (top < KB_SB) top = KB_SB; if (top > H) top = H;
    int full = kb_cmp_top < 0 || s != kb_cmp_s || ds != kb_cmp_ds;
    int lo = full ? 0 : top < kb_cmp_top ? top : kb_cmp_top;
    if (full) memcpy(d, pg, KB_SB * W * 2);
    int y0 = lo < KB_SB ? KB_SB : lo;
    if (kb_area > 0) {
        int a = kb_area < top ? kb_area : top;
        if (y0 < a) kb_shift_rows(d, pg, 0, y0, a);
        kb_shift_rows(d, pg, ds, y0 > a ? y0 : a, top);
    } else kb_shift_rows(d, pg, s, y0, top);
    kb_draw_keys(d, -1);
    kb_off = so;
    kb_cmp_top = top; kb_cmp_s = s; kb_cmp_ds = ds;
    return lo;
}
/* animacion off0->off1 (borde del teclado), s0->s1 (pagina), ds0->ds1 (area del editor) */
static void kb_slide(const uint16_t *pg, int off0, int off1, int s0, int s1, int ds0, int ds1, int ms, const char *what)
{
    double t0 = now_ms() - 11, cs = 0, cmax = 0, gmax = 0, lastp = 0; int n = 0;
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1;
        double e = kb_curve(t);
        int off = off0 + (int)lround((off1 - off0) * e), s = s0 + (int)lround((s1 - s0) * e), ds = ds0 + (int)lround((ds1 - ds0) * e);
        double a = now_ms();
        int lo = kb_compose(pg, off, s, ds);
        a = now_ms() - a; cs += a; if (a > cmax) cmax = a;
        if (lo > 0) present_rows(lo, H); else present();
        double pt = now_ms(); if (n && pt - lastp > gmax) gmax = pt - lastp; lastp = pt; n++;
        if (t >= 1) break;
    }
    lg("teclado: %s %d fot en %.0f ms, componer media %.2f max %.2f ms, intervalo max %.1f ms, pagina %d->%d px", what, n, now_ms() - t0 - 11, cs / n, cmax, gmax, s0 + ds0, s1 + ds1);
}
/* entrada deslizandose desde abajo; la pagina sube a la vez si el campo quedaria tapado */
static void kb_open(void)
{
    kb_pressed = -1; kb_ov_on = 0; kb_cmp_top = -1;
    int kh = KB_H;
    uint16_t *A = kb_pgbuf_get();
    kb_off = kh; kb_pg = 0;
    if (!A || !kb_spr_get(0)) {                   /* sin memoria: el camino de siempre */
        kb_pgbuf_free(); kb_full(); kb_anim(&kb_off, 0, KB_T_OPEN); kb_full(); return;
    }
    kb_snap(A, 1);
    int c0 = kb_c_seen ? kb_cy0 : -1, ds = 0;
    kb_off = 0;
    int pg1 = kb_need();
    if (kb_area > 0 && c0 >= 0) {                 /* varias lineas: cuanto se desplazara el texto */
        kb_snap(kb_dst, 0);
        if (kb_c_seen && c0 > kb_cy0) ds = c0 - kb_cy0;
        if (ds > kh) ds = kh;
        kb_cmp_top = -1;
    }
    kb_slide(A, kh, 0, 0, pg1, 0, ds, KB_T_OPEN, "abrir");
    kb_off = 0; kb_pg = pg1;
    kb_pgbuf_free();
    kb_full();
}
/* entrada del teclado deslizandose desde abajo (llamar tras kb_layout) */
static void kb_begin(struct ebuf *e, uint16_t *dst, void (*redraw)(uint16_t *, int))
{
    kb_ctx(e, dst, redraw);
    kb_sheet = kb_sheet_h = kb_menu = kb_hadj = kb_hpad = kb_trackpad = kb_bar_press = 0; kb_pressed = -1;
    kb_tp_t = 0; kb_tpmode = 0;
    kb_autoshift(e);
    kb_open();
}
/* igual, con el rect del campo con el foco (y0..y1, coordenadas de pagina): si el teclado lo
 * taparia, la pagina sube lo justo (y el llamante debe pasar sus toques por kb_map) */
static void kb_begin_ex(struct ebuf *e, uint16_t *dst, void (*redraw)(uint16_t *, int), int field_y0, int field_y1)
{
    kb_field(field_y0, field_y1);
    kb_begin(e, dst, redraw);
}
/* vuelve a mostrar el teclado si estaba oculto (al tocar el campo de texto) */
static void kb_reveal(uint16_t *dst, void (*redraw)(uint16_t *, int))
{
    if (!kb_off) return;
    kb_dst = dst; kb_rd = redraw;
    kb_open();
}
static void kb_hide(void)
{
    kb_menu = 0; kb_sheet = 0; kb_sheet_h = 0; kb_pressed = -1; kb_trackpad = 0; kb_tp_t = 0; kb_tpmode = 0;
    kb_ov_on = 0; kb_cmp_top = -1;
    int kh = KB_H;
    if (kb_off >= kh) { kb_off = kh; kb_pg = 0; return; }
    uint16_t *A = kb_pgbuf_get();
    if (!A || !kb_spr_get(0)) { kb_pgbuf_free(); kb_anim(&kb_off, kh, KB_T_CLOSE); kb_pg = 0; return; }
    kb_snap(A, 1);
    kb_slide(A, kb_off, kh, kb_pg, 0, 0, 0, KB_T_CLOSE, "cerrar");
    kb_off = kh; kb_pg = 0;
    kb_pgbuf_free(); kb_spr_free();
    kb_full();
}
/* deslizar hacia abajo sobre el teclado: lo arrastra 1:1 y al soltar se cierra (o vuelve) */
static void kb_drag_dismiss(struct pkt *p, int sy)
{
    kb_pressed = -1; kb_ov_on = 0; kb_menu = 0; kb_bar_press = 0; kb_cmp_top = -1;
    uint16_t *A = kb_pgbuf_get();
    if (!A || !kb_spr_get(0)) { kb_pgbuf_free(); while (kb_pump(p, 3000)) {} kb_hide(); return; }
    kb_snap(A, 1);
    int kh = KB_H, pg0 = kb_pg, off = -1, s = pg0, n = 0; float v = 0; double lt = now_ms(), t0 = lt; int ly = cur_y;
    do {
        int o = cur_y - sy; if (o < 0) o = 0; if (o > kh) o = kh;
        if (o != off) {
            off = o; s = pg0 - pg0 * off / kh;
            int lo = kb_compose(A, off, s, 0);
            if (lo > 0) present_rows(lo, H); else present();
            n++;
        }
        double t = now_ms(); if (t > lt) { v = .6f * v + .4f * (float)(cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
    } while (kb_pump(p, 40));
    if (now_ms() - lt > 80) v = 0;
    lg("teclado: arrastre para cerrar %d fot en %.0f ms, %d px, %.2f px/ms", n, now_ms() - t0, off, v);
    if (off > kh / 4 || v > .3f) {
        int ms = KB_T_CLOSE * (kh - off) / kh; if (ms < 120) ms = 120;
        kb_slide(A, off, kh, s, 0, 0, 0, ms, "cerrar (arrastre)");
        kb_off = kh; kb_pg = 0;
        kb_pgbuf_free(); kb_spr_free();
    } else {
        kb_slide(A, off, 0, s, pg0, 0, 0, 220, "volver (arrastre)");
        kb_off = 0; kb_pg = pg0;
        kb_pgbuf_free();
    }
    kb_full();
}
static void kb_end(void)
{
    kb_stats_flush();
    kb_spr_free(); kb_pgbuf_free();
    kb_pg = 0; kb_fy0 = kb_fy1 = -1; kb_area = 0; kb_vmove = 0; kb_tpmode = 0; kb_tp_t = 0; kb_trackpad = 0;
}

static void kb_set_layout(int l)
{
    if (l == kb_cfg_layout) return;
    kb_cfg_layout = l; kb_relayout(); lconf_save();
}
static void kb_sheet_open(void) { kb_sheet = 1; kb_seg_pos = -1; kb_anim(&kb_sheet_h, KB_SH, 280); }
static void kb_sheet_close(void) { kb_anim(&kb_sheet_h, 0, 250); kb_sheet = 0; }

/* menu del globo: 0 ajustes, 1..3 distribuciones */
static void kb_menu_pick(int i)
{
    kb_menu = 0; kb_menu_sel = -1; kb_bar_press = 0;
    if (i == 0) { kb_show(); kb_sheet_open(); return; }
    if (i > 0) kb_set_layout(i - 1);
    kb_show();
}
static void kb_menu_track(struct pkt *p, int sticky_ok)
{
    int moved = 0, sx = cur_x, sy = cur_y;
    while (kb_pump(p, 40)) {
        if (abs(cur_x - sx) > 6 || abs(cur_y - sy) > 6) moved = 1;
        int s = kb_menu_item(cur_x, cur_y);
        if (s != kb_menu_sel) { kb_menu_sel = s; kb_show(); }
    }
    int s = kb_menu_item(cur_x, cur_y);
    if (s >= 0) { kb_menu_pick(s); return; }
    if (sticky_ok && !moved) { kb_menu_sel = -1; kb_show(); return; }   /* toque en el globo: el menu se queda */
    kb_menu = 0; kb_menu_sel = -1; kb_bar_press = 0; kb_show();
}

/* anima el interruptor i de la hoja y aplica el cambio */
static void kb_sheet_toggle(int i)
{
    int *v = i == 0 ? &kb_cfg_numrow : i == 1 ? &kb_cfg_autocap : &kb_cfg_period;
    float a = (float)*v, b = 1 - a; double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 170; if (t > 1) t = 1;
        kb_sw_t[i] = a + (b - a) * (float)ease_out(t); kb_show();
        if (t >= 1) break;
    }
    kb_sw_t[i] = -1; *v = !*v;
    if (i == 0) {   /* la fila nueva entra por debajo de la hoja */
        int pitch = KR_H + 8;
        kb_relayout(); kb_hadj = *v ? -pitch : pitch;
        kb_anim(&kb_hadj, 0, 240);
        lconf_save();
    } else {
        if (i == 1) { if (kb_cfg_autocap) kb_autoshift(kb_e); else if (kb_shift_auto) { kb_shift = 0; kb_shift_auto = 0; } }
        kb_conf_save(); kb_show();
    }
}
/* arrastre del borde superior: la altura sigue al dedo 1:1 */
static void kb_height_drag(struct pkt *p)
{
    int rows = kb_rows(), h0 = kb_keys_h(), sy = cur_y, base = rows * 8 + 6 + KB_BB, kr = KR_H;
    kb_cfg_rowh = kr;
    while (kb_pump(p, 40)) {
        int want = h0 - (cur_y - sy);
        int nk = (want - base) / rows, hp = 0;
        if (nk < KR_MIN) nk = KR_MIN; else if (nk >= KR_MAX) nk = KR_MAX; else hp = want - base - nk * rows;
        if (nk != kb_cfg_rowh || hp != kb_hpad) { kb_cfg_rowh = nk; kb_hpad = hp; kb_relayout(); kb_pg = kb_need(); kb_full(); }
    }
    if (kb_hpad * 2 >= rows && kb_cfg_rowh < KR_MAX) kb_cfg_rowh++;
    kb_hpad = 0; kb_relayout(); kb_pg = kb_need(); kb_full();
    kb_cfg_height = kb_cfg_rowh < 39 ? 0 : kb_cfg_rowh < 45 ? 1 : 2;
    kb_conf_save(); lconf_save();
    lg("teclado: altura de tecla %d px (preajuste %d)", kb_cfg_rowh, kb_cfg_height);
}
static void kb_sheet_touch(struct pkt *p, int x, int y)
{
    int rel = y - kb_sheet_top();
    if (rel >= 140 || y >= kb_keys_top() - 4) { kb_height_drag(p); return; }
    if (rel < 28) {                                   /* Listo */
        if (x < W - 90) { while (kb_pump(p, 3000)) {} return; }
        kb_sheet_press = 0; kb_show();
        while (kb_pump(p, 3000)) {}
        kb_sheet_press = -1;
        if (cur_x >= W - 110 && cur_y >= kb_sheet_top() - 20 && cur_y < kb_sheet_top() + 50) kb_sheet_close(); else kb_show();
        return;
    }
    while (kb_pump(p, 3000)) {}
    if (abs(cur_y - y) > 24) return;
    if (rel < 62) {                                   /* distribucion (control segmentado) */
        int segw = (W - 28) / 3, s = (cur_x - 14) / segw; if (s < 0) s = 0; if (s > 2) s = 2;
        if (s == kb_cfg_layout) return;
        float a = (float)kb_cfg_layout; double t0 = now_ms();
        for (;;) {
            double t = (now_ms() - t0) / 200; if (t > 1) t = 1;
            kb_seg_pos = a + (s - a) * (float)ease_out(t); kb_show();
            if (t >= 1) break;
        }
        kb_seg_pos = -1; kb_set_layout(s); kb_show();
        return;
    }
    int i = (rel - 64) / 26; if (i < 0) i = 0; if (i > 2) i = 2;
    kb_sheet_toggle(i);
}

/* ------------------------------------------------------------------ cursor con el dedo */
/* linea logica arriba/abajo manteniendo la columna (si el editor no da kb_set_vmove) */
static int kb_eb_vline(const struct ebuf *e, int cur, int dir)
{
    const char *b = e->b; int ls = cur; while (ls > 0 && b[ls - 1] != '\n') ls--;
    int col = 0; for (int i = ls; i < cur; i++) if (((unsigned char)b[i] & 0xC0) != 0x80) col++;
    int ns;
    if (dir < 0) { if (ls == 0) return cur; ns = ls - 1; while (ns > 0 && b[ns - 1] != '\n') ns--; }
    else { ns = cur; while (ns < e->len && b[ns] != '\n') ns++; if (ns >= e->len) return cur; ns++; }
    int p = ns;
    while (p < e->len && b[p] != '\n' && col > 0) { p++; while (p < e->len && ((unsigned char)b[p] & 0xC0) == 0x80) p++; col--; }
    return p;
}
#define KB_TP_STEP 8.f                        /* px de dedo (con ganancia) por caracter */
#define KB_TP_LINE 22.f                       /* px por linea (trackpad, varias lineas) */
static float kb_tp_ax, kb_tp_ay;
/* mueve el cursor segun el dedo, con aceleracion: lento = preciso (1:1), rapido = hasta x4.
 * Devuelve 1 si el cursor cambio de caracter, 2 si solo se movio el cursor flotante */
static int kb_tp_step(struct ebuf *e, int dx, int dy, double dt)
{
    float v = sqrtf((float)(dx * dx + dy * dy)) / (float)dt, g = v < .12f ? 1 : 1 + (v - .12f) * 4.5f;
    if (g > 4) g = 4;
    int r = 0;
    kb_tp_ax += dx * g;
    int n = (int)(kb_tp_ax / KB_TP_STEP);
    if (n) {
        int c0 = e->cur; eb_move(e, n);
        if (e->cur != c0) { r = 1; kb_tp_ax -= n * KB_TP_STEP; }
        if (e->cur == c0 || (e->cur == 0 && kb_tp_ax < 0) || (e->cur == e->len && kb_tp_ax > 0)) kb_tp_ax = 0;   /* tope: sin acumular */
    }
    if (kb_tpmode == 2 && kb_ml) {
        kb_tp_ay += dy * (g > 2 ? 2 : g);
        while (fabsf(kb_tp_ay) >= KB_TP_LINE) {
            int dir = kb_tp_ay > 0 ? 1 : -1;
            int c = kb_vmove ? kb_vmove(e->cur, dir) : kb_eb_vline(e, e->cur, dir);
            if (c == e->cur) { kb_tp_ay = 0; break; }
            e->cur = c; r = 1; kb_tp_ay -= dir * KB_TP_LINE;
        }
    }
    int res = (int)lroundf(kb_tp_ax); if (res > 6) res = 6; if (res < -6) res = -6;
    if (kb_tpmode != 2) res = 0;
    if (res != kb_tp_res) { kb_tp_res = res; if (!r) r = 2; }
    return r;
}
/* fundido de las teclas al entrar/salir del trackpad */
static double kb_fd_t0; static float kb_fd_from, kb_fd_to; static int kb_fd_ms;
static void kb_fade_start(float to, int ms) { kb_fd_from = kb_tp_t; kb_fd_to = to; kb_fd_t0 = now_ms() - 8; kb_fd_ms = ms; }
static int kb_fade_tick(void)
{
    if (kb_tp_t == kb_fd_to) return 0;
    double t = (now_ms() - kb_fd_t0) / kb_fd_ms; if (t > 1) t = 1;
    kb_tp_t = t >= 1 ? kb_fd_to : kb_fd_from + (kb_fd_to - kb_fd_from) * (float)ease_out(t);
    return 1;
}

/* procesa un toque completo en el teclado; devuelve K_ENTER si se pulso intro, 0 si no.
 * Las coordenadas son las reales (cur_x/cur_y): el llamante puede haber pasado p por kb_map. */
static int kb_touch(struct pkt *p, struct ebuf *e, uint16_t *dst, void (*redraw)(uint16_t *, int), int multiline)
{
    kb_ctx(e, dst, redraw); kb_ml = multiline;
    double tev = kb_evt_ms(p);
    int x = cur_x, y = cur_y;
    if (kb_menu) {   /* menu abierto: el toque es para el */
        if (kb_menu_item(x, y) >= 0) { kb_menu_sel = kb_menu_item(x, y); kb_show(); kb_menu_track(p, 0); }
        else { while (kb_pump(p, 3000)) {} kb_menu = 0; kb_menu_sel = -1; kb_bar_press = 0; kb_show(); }
        return 0;
    }
    if (kb_sheet_h > 0 && y < kb_keys_top()) { kb_sheet_touch(p, x, y); return 0; }
    if (y >= kb_bar_y()) {   /* barra inferior: globo y ocultar */
        if (x < 60) { kb_menu = 1; kb_menu_sel = -1; kb_bar_press = 1; kb_show(); kb_menu_track(p, 1); return 0; }
        if (x > W - 64) {
            kb_bar_press = 2; kb_show();
            while (kb_pump(p, 3000)) {}
            kb_bar_press = 0;
            if (cur_x > W - 90 && cur_y > kb_bar_y() - 30) kb_hide(); else kb_show();
            return 0;
        }
        double tw = now_ms();   /* en medio: hacia arriba desde el borde = inicio; hacia abajo = ocultar */
        while (cur_down && now_ms() - tw < 400) {
            read_pkt(p, 20);
            if (y >= 456 && y - cur_y > 12) return K_HOME;
            if (cur_y - y > 8) { kb_drag_dismiss(p, y); return 0; }
        }
        while (kb_pump(p, 3000)) {}
        return 0;
    }
    int k = kb_hit(x, y);
    if (k < 0) { while (kb_pump(p, 3000)) {} return 0; }

    /* pulsacion: visible en el siguiente fotograma */
    kb_pressed = k;
    if (kbk[k].code == K_BACK) { eb_back(e); kb_autoshift(e); kb_full(); }
    else kb_show();
    double lat = kb_now_real() - tev;
    kb_st_n++; kb_st_lat += lat; if (lat > kb_st_latmax) kb_st_latmax = lat;
    kb_st_draw += kb_t_draw; if (kb_t_draw > kb_st_drawmax) kb_st_drawmax = kb_t_draw;

    double t0 = now_ms(), trep = t0, tdown = t0, lt = t0;
    int sx = x, sy = y, alt = 0, lx = x, ly = y;
    kb_alt_n = 0; kb_tpmode = 0; kb_tp_ax = kb_tp_ay = 0; kb_tp_res = 0;
    int tp_n = 0, tp_fr = 0; double tp_lat = 0, tp_latmax = 0, tp_t0 = 0, tp_dr = 0, tp_drmax = 0;
    while (kb_pump(p, kb_tp_t != kb_fd_to ? 1 : 15)) {
        double t = now_ms();
        if (kb_tpmode) {   /* el dedo mueve el cursor (la tecla ya no escribe) */
            int dx = cur_x - lx, dy = cur_y - ly, r = 0;
            if (dx || dy) { r = kb_tp_step(e, dx, dy, t - lt > 1 ? t - lt : 1); lx = cur_x; ly = cur_y; lt = t; }
            int fading = kb_fade_tick();
            double a = now_ms();
            if (r == 1) kb_full();
            else if (r == 2 && !fading) kb_fc_move();
            else if (fading) { if (r == 2) kb_full(); else kb_show(); }
            if (r) {
                double l = kb_now_real() - kb_last_evt, dr = now_ms() - a;
                tp_n++; tp_lat += l; if (l > tp_latmax) tp_latmax = l; tp_dr += dr; if (dr > tp_drmax) tp_drmax = dr;
            }
            if (r || fading) tp_fr++;
            continue;
        }
        if (alt) { int s = kb_alt_pick(cur_x); if (s != kb_alt_sel) { kb_alt_sel = s; kb_show(); } continue; }
        int code = kbk[k].code;
        /* un golpe rapido hacia abajo: el teclado se va con el dedo (y no escribe) */
        int down = cur_y - sy, side = abs(cur_x - sx);
        if (t - tdown < 260 && down > 12 && down > 2 * side) {
            if (down > 30) { kb_pressed = -1; kb_drag_dismiss(p, sy + 30); return 0; }
            continue;                               /* aun puede serlo: no cambia de tecla */
        }
        if (code == K_SPACE) {
            int mx = abs(cur_x - sx), my = abs(cur_y - sy);
            if (mx > 14 && mx > my) {                /* deslizar en el espacio (Gboard): mueve el cursor */
                kb_tpmode = 1; lx = cur_x; ly = cur_y; lt = t; tp_t0 = t; continue;
            }
            if (t - t0 > 300 && mx <= 14 && my <= 14) {   /* mantener: trackpad (iOS) */
                kb_tpmode = 2; kb_trackpad = 1; kb_pressed = -1; lx = cur_x; ly = cur_y; lt = t; tp_t0 = t;
                kb_fade_start(1, 150); kb_fade_tick(); kb_full(); tp_fr++;
                continue;
            }
        }
        if (code == K_BACK) {   /* repeticion que acelera: letras y, tras 1,8 s, palabras */
            double held = t - t0, iv = held > 1800 ? 230 : held > 1100 ? 55 : 90;
            if (held > 480 && t - trep >= iv && e->cur > 0) {
                if (held > 1800) eb_back_word(e); else eb_back(e);
                trep = t; kb_autoshift(e); kb_full();
            }
            continue;
        }
        int nk = kb_hit(cur_x, cur_y);
        if (nk >= 0 && nk != k) { k = nk; kb_pressed = k; t0 = t; sx = cur_x; sy = cur_y; kb_show(); continue; }
        if (code > 0 && t - t0 > 420 && kb_alt_open(k)) {
            alt = 1; kb_alt_sel = kb_alt_pick(cur_x);
            if (kb_alt_dir > 0 ? cur_x < kbk[k].x + kbk[k].w : cur_x >= kbk[k].x) kb_alt_sel = 0;
            kb_show(); continue;
        }
    }

    /* al soltar */
    double tc = now_ms();
    int code = kbk[k].code, r = 0, text = 0;
    kb_pressed = -1;
    char o[8];
    if (kb_tpmode) {   /* fin del cursor con el dedo: no se escribe el espacio */
        int was = kb_tpmode;
        kb_tpmode = 0; kb_tp_res = 0; kb_trackpad = 0;
        kb_autoshift(e);
        kb_full();                                  /* fuera el cursor flotante */
        if (was == 2) {                             /* las teclas vuelven */
            kb_fade_start(0, 130);
            while (kb_fade_tick()) kb_show();
        } else kb_show();
        if (tp_t0 > 0) lg("teclado: %s %.0f ms, %d pasos del cursor, toque->cursor visible media %.1f max %.1f ms, pintar media %.1f max %.1f ms, %d fot",
                          was == 2 ? "trackpad" : "deslizar en espacio", tc - tp_t0, tp_n, tp_n ? tp_lat / tp_n : 0, tp_latmax, tp_n ? tp_dr / tp_n : 0, tp_drmax, tp_fr);
        return 0;
    }
    if (alt) {
        put_utf8(o, kb_upper() ? up_case(kb_alt_cp[kb_alt_sel]) : kb_alt_cp[kb_alt_sel]);
        eb_insert(e, o); kb_alt_n = 0; text = 1;
        if (!kb_caps) { kb_shift = 0; kb_shift_auto = 0; }
    } else if (code > 0) {
        put_utf8(o, kb_upper() ? up_case(code) : code); eb_insert(e, o); text = 1;
        if (!kb_caps) { kb_shift = 0; kb_shift_auto = 0; }
    } else if (code == K_SPACE) {
        static double last_space;
        unsigned char pc = e->cur >= 2 ? (unsigned char)e->b[e->cur - 2] : ' ';
        if (kb_cfg_period && tc - last_space < 800 && e->cur >= 2 && e->b[e->cur - 1] == ' ' && (isalnum(pc) || pc >= 0x80 || pc == ')' || pc == '"')) {
            eb_back(e); eb_insert(e, ". "); last_space = 0;
        } else { eb_insert(e, " "); last_space = tc; }
        text = 1;
    } else if (code == K_SHIFT) {
        static double last_shift;
        if (kb_layer) { kb_layer = kb_layer == 1 ? 2 : 1; kb_relayout(); }
        else if (tc - last_shift < 350 && !kb_caps) { kb_caps = 1; kb_shift = 1; last_shift = 0; }
        else if (kb_caps) { kb_caps = 0; kb_shift = 0; last_shift = 0; }
        else { kb_shift = !kb_shift; last_shift = tc; }
        kb_shift_auto = 0;
    } else if (code == K_LAYER) {
        kb_layer = kb_layer ? 0 : 1; kb_relayout(); kb_autoshift(e);
    } else if (code == K_ENTER) {
        if (multiline) { eb_insert(e, "\n"); text = 1; } else r = K_ENTER;
    }
    if (text) { kb_autoshift(e); kb_full(); }
    else if (r != K_ENTER) kb_show();
    double com = now_ms() - tc;
    if (text) { kb_st_com += com; if (com > kb_st_commax) kb_st_commax = com; }
    if (kb_st_n >= 20) kb_stats_flush();
    return r;
}

/* ------------------------------------------------------------------ editor de una linea */
static const char *le_title, *le_hint;
static struct ebuf *le_buf;
static int le_caret = 1;
#ifndef KB_TEST_FIELD_Y
#define KB_TEST_FIELD_Y 96
#endif
static int le_fy = KB_TEST_FIELD_Y;           /* arriba del campo (96; las pruebas del VM lo bajan) */
/* la pagina del editor de una linea (sin teclado): la usan tambien las sugerencias del editor de etiquetas */
static void le_page(uint16_t *dst)
{
    int top = KB_Y; if (top > H) top = H; if (top < 0) top = 0;
    for (int i = 0; i < W * top; i++) dst[i] = rgb(0, 0, 0);
    memcpy(dst, fr1, 30 * W * 2);
    int tw = tw_n(le_title, 17, -1);
    draw_text(dst, (W - tw) / 2, 64, le_title, 17, 0xFFFF, 0, 0, top);
    draw_text(dst, 14, 64, tr("Cancelar"), 15, rgb(10, 132, 255), 0, 0, top);
    kb_blit(dst, 12, le_fy, W - 24, 44, 10, rgb(44, 44, 48), 255, 0, top);
    int base = le_fy + 28;
    draw_text(dst, 24, base, le_buf->b, 16, 0xFFFF, 0, 0, top);
    if (!le_buf->len && le_hint) draw_text(dst, 24, base, le_hint, 16, rgb(110, 110, 116), 0, 0, top);
    int cx = 24 + tw_n(le_buf->b, 16, le_buf->cur);
    if (le_caret && le_fy + 36 <= top) {
        for (int y = le_fy + 10; y < le_fy + 36; y++) for (int x = cx; x < cx + 2 && x < W; x++) dst[y * W + x] = rgb(10, 132, 255);
        kb_caret(cx, le_fy + 10, le_fy + 36);
    }
}
static void le_redraw(uint16_t *dst, int pressed) { le_page(dst); kb_draw(dst, pressed); }
/* devuelve 1 si se acepto; el texto queda en buf */
static int line_edit(const char *title, const char *hint, char *buf, int cap)
{
    struct ebuf e = { buf, (int)strlen(buf), cap, (int)strlen(buf) };
    le_title = title; le_hint = hint; le_buf = &e; le_caret = 1;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Aceptar"));
    kb_begin_ex(&e, fr0, le_redraw, le_fy, le_fy + 44);
    struct pkt p; double blink = now_ms();
    for (;;) {
        int wait = (int)(blink + 530 - now_ms()); if (wait < 1) wait = 1;
        if (!read_pkt(&p, wait)) {   /* cursor parpadeante (solo se presentan sus filas) */
            if (now_ms() - blink >= 530) { le_caret = !le_caret; blink = now_ms(); le_redraw(fr0, -1); kb_present_rows(le_fy + 8, le_fy + 38); }
            continue;
        }
        if (!p.down) continue;
        kb_map(&p);
        le_caret = 1;
        if (p.y >= KB_Y) {
            int r = kb_touch(&p, &e, fr0, le_redraw, 0);
            blink = now_ms();
            if (r == K_ENTER) { kb_end(); return e.len > 0; }
            continue;
        }
        int x = p.x, y = p.y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 90 && x < 100) { kb_end(); return 0; }          /* Cancelar */
        if (y >= le_fy && y < le_fy + 44) {                      /* colocar el cursor */
            int best = 0, bd = 1 << 30;
            for (int i = 0; i <= e.len; i++) {
                if (i < e.len && ((unsigned char)buf[i] & 0xC0) == 0x80) continue;
                int d = abs(24 + tw_n(buf, 16, i) - x); if (d < bd) { bd = d; best = i; }
            }
            e.cur = best; kb_autoshift(&e); blink = now_ms();
            if (kb_off) kb_reveal(fr0, le_redraw); else { le_redraw(fr0, -1); present(); }
        }
    }
}
