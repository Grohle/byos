/* widgets.inc.c - pagina de widgets a la izquierda del inicio (como "Hoy" en iOS).
 *
 * Desde la primera pagina del inicio, deslizar hacia la derecha. Fondo = el del inicio;
 * widgets de iOS en dos tamanos: PEQUENO (cuadrado, dos por fila) y MEDIANO (ancho completo),
 * tarjetas de cristal oscuro (fondo desenfocado y tenido: se leen sobre cualquier fondo) con
 * graficas circulares: Baterias (HiBy + Bluetooth via BlueZ), Reproduciendo (con controles),
 * Fecha (calendario), Almacenamiento, Energia, Biblioteca, Recien anadido y Escuchas.
 *
 * Modelo: lista ordenada de {tipo, tamano} (como mucho uno por tipo+tamano), linea de
 * launcher.conf "widgets page=1 battery:s date:s nowplaying:m ..." (se sigue leyendo la antigua
 * "widgets battery date ..." con tamanos por defecto). Colocacion en flujo: los pequenos se
 * emparejan, los medianos ocupan la fila.
 *
 * Sin parpadeos: cada widget se dibuja UNA vez en su propio bufer (sprite) a partir de una
 * copia de los datos tomada con cerrojo (el hilo de datos nunca escribe lo que se esta
 * dibujando); la pagina se compone entera en wg_cache y solo entonces se presenta; si solo
 * cambio una tarjeta se presentan solo sus filas (wg_part_y0/1, ver launcher_present).
 *
 * Edicion como las apps (pulsacion larga): los widgets tiemblan (giro bilineal en coma fija de
 * cada sprite), "−" quita (encoge y se funde), "+" abre la galeria (hoja de iOS con vistas
 * previas reales y tamano P/M), arrastrar reordena (los demas se apartan con animacion) y
 * "Listo" sale. Los datos se leen al entrar y cada 10 s mientras la pagina esta visible. */

enum { WG_BATT, WG_NOW, WG_DATE, WG_STORAGE, WG_POWER, WG_LIB, WG_NEW, WG_LISTEN, WG_N };
static const char *const wg_id[WG_N] = { "battery", "nowplaying", "date", "storage", "power", "library", "recent", "listening" };
static const char *const wg_name[WG_N] = { "Baterías", "Reproduciendo", "Fecha", "Almacenamiento", "Energía", "Biblioteca", "Recién añadido", "Escuchas" };
static const char *const wg_desc[WG_N] = {
    "Nivel de batería del HiBy y de tus auriculares Bluetooth.",
    "Lo que suena ahora, con controles de reproducción.",
    "El día de hoy y el calendario del mes.",
    "Espacio libre en la tarjeta SD y en el HiBy.",
    "Modo de energía, frecuencia de la pantalla y carga.",
    "Canciones, álbumes, artistas y favoritas.",
    "El último álbum que has añadido a la biblioteca.",
    "Lo último que has escuchado y tu artista favorito." };
static const unsigned char wg_defsz[WG_N] = { 0, 1, 0, 0, 0, 0, 0, 1 };   /* 0 pequeno, 1 mediano */

#define WG_MAX 16
struct witem { unsigned char type, size; float ax, ay, sc, al; int rm; double rm_t; };
static struct witem wg_it[WG_MAX] = { { WG_BATT, 0 }, { WG_DATE, 0 }, { WG_NOW, 1 }, { WG_LISTEN, 1 },
                                      { WG_NEW, 0 }, { WG_LIB, 0 }, { WG_STORAGE, 0 }, { WG_POWER, 0 } };
static int wg_n = 8;
static int wg_page_on = 1;                   /* la pagina de widgets existe (Ajustes > Inicio) */
static int widgets_on;                       /* la pagina de widgets esta en pantalla */

static int wg_has(int type, int size) { for (int i = 0; i < wg_n; i++) if (wg_it[i].type == type && wg_it[i].size == size) return 1; return 0; }
static int wg_has_type(int type) { for (int i = 0; i < wg_n; i++) if (wg_it[i].type == type) return 1; return 0; }
static void wg_conf_line(char *rest)
{
    wg_n = 0; char *t;
    while ((t = strtok(rest, " \t\r\n"))) { rest = 0;
        if (!strncmp(t, "page=", 5)) { wg_page_on = atoi(t + 5) != 0; continue; }
        char *c = strchr(t, ':'); int sz = -1;
        if (c) { *c = 0; sz = c[1] == 'm'; }
        for (int i = 0; i < WG_N; i++) if (!strcmp(t, wg_id[i])) {
            if (sz < 0) sz = wg_defsz[i];
            if (!wg_has(i, sz) && wg_n < WG_MAX) { memset(&wg_it[wg_n], 0, sizeof wg_it[0]); wg_it[wg_n].type = i; wg_it[wg_n++].size = sz; }
        }
    }
}
static void wg_conf_save(FILE *f)
{
    fprintf(f, "widgets page=%d", wg_page_on);
    for (int i = 0; i < wg_n; i++) fprintf(f, " %s:%c", wg_id[wg_it[i].type], wg_it[i].size ? 'm' : 's');
    fprintf(f, "\n");
}

/* ------------------------------------------------------------------ datos */
struct wg_bt { char name[40]; int pct; };
struct wdata { int batt, charging, usb; struct wg_bt bt[3]; int nbt; long long sd_free, sd_tot, in_free, in_tot; double t;
               int songs, albums, artists, favs; long long lib_bytes; char new_artist[64], new_album[96], top_artist[64], last_title[96], last_artist[64]; int top_n; };
static struct wdata WD, WDs;                 /* WD: la publica el hilo (con cerrojo); WDs: copia con la que se dibuja */
static pthread_mutex_t wg_mu = PTHREAD_MUTEX_INITIALIZER;
static void wg_bt_scan(struct wdata *n)
{
    n->nbt = 0;
    char *argv[] = { "/usr/bin/dbus-send", "--system", "--print-reply", "--dest=org.bluez", "/", "org.freedesktop.DBus.ObjectManager.GetManagedObjects", 0 };
    static char out[24000];
    if (access(argv[0], X_OK) || run_capture(argv, out, sizeof out) <= 0) return;
    /* por cada objeto dev_: Alias, Connected y Percentage */
    char alias[40] = ""; int conn = 0, pct = -1, indev = 0, want = 0;   /* 1 alias, 2 conectado, 3 bateria */
    for (char *sv = 0, *l = strtok_r(out, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
        char *op = strstr(l, "object path \"");
        if (op) {
            if (indev && conn && n->nbt < 3) { snprintf(n->bt[n->nbt].name, 40, "%s", alias[0] ? alias : "Bluetooth"); n->bt[n->nbt++].pct = pct; }
            indev = strstr(op, "/dev_") != 0; alias[0] = 0; conn = 0; pct = -1; continue;
        }
        if (!indev) continue;
        if (strstr(l, "string \"Alias\"")) { want = 1; continue; }
        if (strstr(l, "string \"Connected\"")) { want = 2; continue; }
        if (strstr(l, "string \"Percentage\"")) { want = 3; continue; }
        if (want == 1) { char *q = strstr(l, "string \""); if (q) { q += 8; char *e = strchr(q, '"'); if (e) *e = 0; snprintf(alias, sizeof alias, "%s", q); } want = 0; }
        else if (want == 2) { conn = strstr(l, "boolean true") != 0; want = 0; }
        else if (want == 3) { char *q = strstr(l, "byte "); if (q) pct = atoi(q + 5); want = 0; }
    }
    if (indev && conn && n->nbt < 3) { snprintf(n->bt[n->nbt].name, 40, "%s", alias[0] ? alias : "Bluetooth"); n->bt[n->nbt++].pct = pct; }
}
static volatile int wg_busy, wg_fresh;     /* hilo de datos en marcha / datos nuevos sin dibujar */
static int wg_cache_ok;
static void wg_refresh_now(void)
{
    static struct wdata n;                 /* se rellena aparte y se publica de golpe */
    pthread_mutex_lock(&wg_mu); n = WD; pthread_mutex_unlock(&wg_mu);
    n.batt = sysfs_get("/sys/class/power_supply/battery/capacity", -1);
    char st[24] = ""; int f = open("/sys/class/power_supply/battery/status", O_RDONLY);
    if (f >= 0) { int k = read(f, st, sizeof st - 1); if (k > 0) st[k] = 0; close(f); }
    n.charging = !strncmp(st, "Charging", 8); n.usb = sysfs_get(USB_ONLINE, 0);
    struct statvfs v;
    if (!statvfs("/usr/data/mnt/sd_0", &v) && v.f_blocks) { n.sd_free = (long long)v.f_bavail * v.f_frsize; n.sd_tot = (long long)v.f_blocks * v.f_frsize; } else n.sd_tot = n.sd_free = 0;
    if (!statvfs("/usr/data", &v)) { n.in_free = (long long)v.f_bavail * v.f_frsize; n.in_tot = (long long)v.f_blocks * v.f_frsize; }
    wg_bt_scan(&n);
    {   /* biblioteca de HiBy (hibymix stats: solo lectura) */
        char *argv[] = { "/usr/data/hibymod/shell/hibymix", "stats", 0 };
        static char out[2048];
        if (!access(argv[0], X_OK) && run_capture(argv, out, sizeof out) > 0)
            for (char *sv = 0, *l = strtok_r(out, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
                char *fl[6]; int k = split_tabs(l, fl, 6);
                if (!strcmp(fl[0], "count") && k >= 6) { n.songs = atoi(fl[1]); n.albums = atoi(fl[2]); n.artists = atoi(fl[3]); n.favs = atoi(fl[4]); n.lib_bytes = atoll(fl[5]); }
                else if (!strcmp(fl[0], "recent") && k >= 3) { snprintf(n.new_artist, sizeof n.new_artist, "%s", fl[1]); snprintf(n.new_album, sizeof n.new_album, "%s", fl[2]); }
                else if (!strcmp(fl[0], "top") && k >= 3) { snprintf(n.top_artist, sizeof n.top_artist, "%s", fl[1]); n.top_n = atoi(fl[2]); }
                else if (!strcmp(fl[0], "last") && k >= 3) { snprintf(n.last_title, sizeof n.last_title, "%s", fl[1]); snprintf(n.last_artist, sizeof n.last_artist, "%s", fl[2]); }
            }
    }
    pthread_mutex_lock(&wg_mu); n.t = WD.t; WD = n; pthread_mutex_unlock(&wg_mu);
}
static volatile int wg_loaded;              /* el hilo ya publico datos al menos una vez */
static void *wg_thread(void *a) { (void)a; wg_refresh_now(); wg_loaded = 1; wg_fresh = 1; wg_busy = 0; return 0; }
static void wg_refresh(void)            /* asincrono */
{
    if (wg_busy) return;
    wg_busy = 1; WD.t = now_ms();
    pthread_t th; if (pthread_create(&th, 0, wg_thread, 0)) { wg_busy = 0; return; } pthread_detach(th);
}
/* reproduccion: copia propia (con caratula) y estado "optimista" tras pulsar play/pausa */
static struct np_info wg_ni; static uint16_t wg_cov[128 * 128]; static int wg_cok, wg_np_force; static double wg_np_until;
static int wg_playing(void) { return now_ms() < wg_np_until ? wg_np_force : wg_ni.playing; }
static void wg_snap(void)
{
    pthread_mutex_lock(&wg_mu); double t = WDs.t; WDs = WD; WDs.t = t; pthread_mutex_unlock(&wg_mu);
    if (!wg_loaded) {   /* aun sin datos del hilo: al menos la bateria (sysfs, inmediato), nunca "0 %" */
        WDs.batt = sysfs_get("/sys/class/power_supply/battery/capacity", -1); WDs.usb = sysfs_get(USB_ONLINE, 0);
        char st[24] = ""; int f = open("/sys/class/power_supply/battery/status", O_RDONLY);
        if (f >= 0) { int k = read(f, st, sizeof st - 1); if (k > 0) st[k] = 0; close(f); }
        WDs.charging = !strncmp(st, "Charging", 8);
    }
    npx_snapshot(&wg_ni, wg_cov, &wg_cok);
}

/* ------------------------------------------------------------------ geometria */
#define WG_M 16                              /* margen lateral */
#define WG_G 14                              /* separacion */
#define WG_S ((W - 2 * WG_M - WG_G) / 2)     /* pequeno: 137 x 137 */
#define WG_MW (W - 2 * WG_M)                 /* mediano: 288 x 137 */
#define WG_H WG_S
#define WG_R 20.f
#define WG_TOP 44
#define WG_ESH 30                            /* desplazamiento del contenido en edicion (pildoras) */
#define WG_SH 140                            /* filas del bufer de dibujo */
#define WG_TH 60                             /* miniatura de la galeria */
static int wg_w(int size) { return size ? WG_MW : WG_S; }
/* flujo: los pequenos se emparejan, los medianos ocupan la fila; devuelve el alto total */
static int wg_layout(const unsigned char *sz, int n, short *xs, short *ys)
{
    int col = 0, y = 0;
    for (int i = 0; i < n; i++) {
        if (!sz[i]) {
            xs[i] = col ? WG_M + WG_S + WG_G : WG_M; ys[i] = y;
            if (col) { col = 0; y += WG_H + WG_G; } else col = 1;
        } else {
            if (col) { col = 0; y += WG_H + WG_G; }
            xs[i] = WG_M; ys[i] = y; y += WG_H + WG_G;
        }
    }
    if (col) y += WG_H + WG_G;
    return y;
}
static int wg_layout_items(short *xs, short *ys)
{
    unsigned char sz[WG_MAX]; for (int i = 0; i < wg_n; i++) sz[i] = wg_it[i].size;
    return wg_layout(sz, wg_n, xs, ys);
}

/* ------------------------------------------------------------------ mascaras y cristal */
static uint8_t *wg_mask[3], *wg_rim[2], *wg_shadow[2];     /* [0] pequeno, [1] mediano, [2] miniatura */
static uint8_t *wg_rrmask(int w, int h, float r, float inset)
{
    uint8_t *m = malloc(w * h); if (!m) return 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float ww = w - 2 * inset, hh = h - 2 * inset, rr = r - inset;
        float px = x + .5f - inset, py = y + .5f - inset, qx = fabsf(px - ww / 2.f) - (ww / 2.f - rr), qy = fabsf(py - hh / 2.f) - (hh / 2.f - rr);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - rr;
        float c = 0.5f - d; m[y * w + x] = (uint8_t)(c <= 0 ? 0 : c >= 1 ? 255 : c * 255);
    }
    return m;
}
#define WG_SP 12                             /* margen de la sombra del widget arrastrado */
static void wg_masks_init(void)
{
    if (wg_mask[0]) return;
    for (int s = 0; s < 2; s++) {
        int w = wg_w(s);
        wg_mask[s] = wg_rrmask(w, WG_H, WG_R, 0);
        uint8_t *in = wg_rrmask(w, WG_H, WG_R, 1);
        wg_rim[s] = malloc(w * WG_H);
        if (wg_rim[s] && in && wg_mask[s]) for (int i = 0; i < w * WG_H; i++) { int k = wg_mask[s][i] - in[i]; wg_rim[s][i] = k < 0 ? 0 : k; }
        free(in);
        /* sombra suave: mascara desenfocada (dos cajas de 9) en un lienzo con margen */
        int sw = w + 2 * WG_SP, sh = WG_H + 2 * WG_SP;
        uint8_t *a = calloc(sw * sh, 1), *b = calloc(sw * sh, 1);
        if (a && b && wg_mask[s]) {
            for (int y = 0; y < WG_H; y++) memcpy(a + (y + WG_SP) * sw + WG_SP, wg_mask[s] + y * w, w);
            for (int pass = 0; pass < 2; pass++) {
                for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) { int t = 0; for (int k = -4; k <= 4; k++) { int xx = x + k; if (xx >= 0 && xx < sw) t += a[y * sw + xx]; } b[y * sw + x] = t / 9; }
                for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) { int t = 0; for (int k = -4; k <= 4; k++) { int yy = y + k; if (yy >= 0 && yy < sh) t += b[yy * sw + x]; } a[y * sw + x] = t / 9; }
            }
            wg_shadow[s] = a; a = 0;
        }
        free(a); free(b);
    }
    wg_mask[2] = wg_rrmask(WG_TH, WG_TH, 13, 0);
}
/* fondo desenfocado 40x60 (bloques de 8x8) del fondo actual */
static uint8_t wg_bl[61][41][3]; static int wg_bl_gen = -1;
static void wg_blur_build(void)
{
    for (int by = 0; by < 60; by++) for (int bx = 0; bx < 40; bx++) {
        int r = 0, g = 0, b = 0;
        for (int y = 0; y < 8; y += 2) for (int x = 0; x < 8; x += 2) { uint16_t q = wall[(by * 8 + y) * W + bx * 8 + x]; r += q >> 11; g += (q >> 5) & 63; b += q & 31; }
        r = r * 255 / 496; g = g * 255 / 1008; b = b * 255 / 496;
        int l = (77 * r + 151 * g + 28 * b) >> 8;          /* 25 % desaturado */
        wg_bl[by][bx][0] = (uint8_t)((r * 192 + l * 64) >> 8); wg_bl[by][bx][1] = (uint8_t)((g * 192 + l * 64) >> 8); wg_bl[by][bx][2] = (uint8_t)((b * 192 + l * 64) >> 8);
    }
    for (int by = 0; by < 60; by++) memcpy(wg_bl[by][40], wg_bl[by][39], 3);
    memcpy(wg_bl[60], wg_bl[59], sizeof wg_bl[0]);
    wg_bl_gen = wall_gen;
}
/* cristal oscuro: fondo desenfocado de esa zona de la pantalla, oscurecido (blanco -> gris 93,
 * negro -> gris 22: el texto blanco se lee siempre) y un filo claro de 1 px */
static void wg_card_bg(uint16_t *d, int w, int h, int sx, int sy, int szi)
{
    if (wg_bl_gen != wall_gen) wg_blur_build();
    const uint8_t *rim = wg_rim[szi];
    for (int y = 0; y < h; y++) {
        int Y = sy + y; if (Y < 0) Y = 0; if (Y >= H) Y = H - 1;
        int f = (Y * 2 + 1) * 16 - 128; if (f < 0) f = 0; int y0 = f >> 8, wy = f & 255;
        uint16_t *row = d + y * W;
        for (int x = 0; x < w; x++) {
            int X = sx + x; if (X < 0) X = 0; if (X >= W) X = W - 1;
            int fx = (X * 2 + 1) * 16 - 128; if (fx < 0) fx = 0; int x0 = fx >> 8, wx = fx & 255, c[3];
            for (int k = 0; k < 3; k++) {
                int top = wg_bl[y0][x0][k] * (256 - wx) + wg_bl[y0][x0 + 1][k] * wx, bot = wg_bl[y0 + 1][x0][k] * (256 - wx) + wg_bl[y0 + 1][x0 + 1][k] * wx;
                int v = (top * (256 - wy) + bot * wy) >> 16;
                c[k] = (v * 72 >> 8) + 22;
            }
            int e = rim ? rim[y * w + x] : 0;
            if (e) { e = e * 40 / 255; for (int k = 0; k < 3; k++) c[k] += (255 - c[k]) * e / 255; }
            row[x] = rgb(c[0], c[1], c[2]);
        }
    }
}

/* ------------------------------------------------------------------ primitivas */
static int wg_cy0 = 0, wg_cy1 = WG_SH;      /* recorte vertical del texto (bufer de dibujo o pantalla) */
static uint16_t wg_scr[W * WG_SH];           /* bufer de dibujo de un widget (paso W) */
/* mezcla 565 sin divisiones (alfa 0..255 -> 0..32, truco de los campos separados) */
static inline uint16_t wg_mix(uint16_t c, uint16_t d, unsigned a)
{
    unsigned k = (a * 33) >> 8; if (k >= 32) return c; if (!k) return d;
    uint32_t cs = (c | ((uint32_t)c << 16)) & 0x07E0F81Fu, ds = (d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
    uint32_t v = ((cs * k + ds * (32 - k)) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(v | (v >> 16));
}
static inline void wg_px(uint16_t *d, int x, int y, uint16_t c, int a)
{
    if (x < 0 || x >= W || y < wg_cy0 || y >= wg_cy1 || a <= 0) return;
    uint16_t *p = d + y * W + x; *p = a >= 255 ? c : mix565(c, *p, (unsigned)a);
}
/* arco de grosor th desde el angulo "start" (0 = las 12, horario) de longitud "len", con
 * extremos redondeados y suavizado; track_a > 0 pinta tambien la pista tenue completa */
static void wg_arc(uint16_t *d, float cx, float cy, float r, float th, float start, float len, uint16_t col, int track_a)
{
    const float TAU = 6.2831853f;
    float ht = th / 2; int full = len >= TAU - 0.001f, none = len <= 0.001f;
    float ex = cx + r * sinf(start + len), ey = cy - r * cosf(start + len), bx = cx + r * sinf(start), by = cy - r * cosf(start);
    for (int y = (int)floorf(cy - r - ht - 1); y <= (int)ceilf(cy + r + ht + 1); y++)
        for (int x = (int)floorf(cx - r - ht - 1); x <= (int)ceilf(cx + r + ht + 1); x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, dd = sqrtf(dx * dx + dy * dy);
            float cov = ht + .5f - fabsf(dd - r); if (cov <= 0) continue; if (cov > 1) cov = 1;
            float ca = 0;
            if (full) ca = cov;
            else if (!none) {
                float ang = atan2f(dx, -dy) - start; while (ang < 0) ang += TAU; while (ang >= TAU) ang -= TAU;
                if (ang <= len) ca = cov;
                else {
                    float d1 = sqrtf((x + .5f - ex) * (x + .5f - ex) + (y + .5f - ey) * (y + .5f - ey));
                    float d2 = sqrtf((x + .5f - bx) * (x + .5f - bx) + (y + .5f - by) * (y + .5f - by));
                    float c1 = ht + .5f - (d1 < d2 ? d1 : d2); if (c1 > 1) c1 = 1; if (c1 > ca) ca = c1;
                }
            }
            if (track_a && ca < 1) wg_px(d, x, y, col, (int)(track_a * cov));
            if (ca > 0) wg_px(d, x, y, col, (int)(255 * ca));
        }
}
static void wg_ring(uint16_t *d, float cx, float cy, float r, float th, float frac, uint16_t col)
{
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    wg_arc(d, cx, cy, r, th, 0, frac * 6.2831853f, col, 66);
}
/* poligono relleno con 4x4 submuestras (rayo, glifos pequenos); pts en unidades de "s" */
static void wg_poly(uint16_t *d, float x0, float y0, float s, const float *pts, int n, uint16_t col)
{
    for (int y = (int)y0 - 1; y <= (int)(y0 + s) + 1; y++) for (int x = (int)x0 - 1; x <= (int)(x0 + s) + 1; x++) {
        int hit = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float px = (x + (sx + .5f) / 4 - x0) / s, py = (y + (sy + .5f) / 4 - y0) / s; int in = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
                if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
            }
            hit += in;
        }
        if (hit) wg_px(d, x, y, col, hit * 255 / 16);
    }
}
static void wg_bolt(uint16_t *d, float x, float y, float h, uint16_t col)
{
    static const float b[] = { .62f, 0, .06f, .58f, .44f, .58f, .34f, 1, .94f, .40f, .56f, .40f, .70f, 0 };
    wg_poly(d, x, y, h, b, 7, col);
}
static void wg_glyph_player(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    fill_rrect(d, (int)(cx - s * .3f), (int)(cy - s * .46f), (int)(s * .6f), (int)(s * .92f), s * .13f, col, 255);
    fill_rrect(d, (int)(cx - s * .2f), (int)(cy - s * .36f), (int)(s * .4f), (int)(s * .26f), 1.5f, rgb(28, 28, 32), 255);
    fill_circle(d, cx, cy + s * .2f, s * .15f, rgb(28, 28, 32), 255);
}
static void wg_glyph_phones(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    wg_arc(d, cx, cy + s * .05f, s * .34f, s * .1f, 4.712389f, 3.14159265f, col, 0);
    for (int k = -1; k <= 1; k += 2)
        fill_rrect(d, (int)(cx + k * s * .34f - s * .12f), (int)(cy + s * .02f), (int)(s * .24f), (int)(s * .36f), s * .08f, col, 255);
}

/* ---- texto ---- */
static int wg_fit(const char *t, int size, int maxw) { int sz = size; while (sz > 11 && tw_n(t, sz, -1) > maxw) sz--; return sz; }
static void wt(uint16_t *d, int x, int base, const char *t, int size, uint16_t col, int maxw)
{
    if (!t || !*t) return;
    draw_text(d, x, base, t, wg_fit(t, size, maxw), col, maxw, wg_cy0, wg_cy1);
}
static void wtc(uint16_t *d, int cx, int base, const char *t, int size, uint16_t col, int maxw)   /* centrado */
{
    if (!t || !*t) return;
    int sz = wg_fit(t, size, maxw), w = tw_n(t, sz, -1); if (w > maxw) w = maxw;
    draw_text(d, cx - w / 2, base, t, sz, col, maxw, wg_cy0, wg_cy1);
}
static void wtr(uint16_t *d, int rx, int base, const char *t, int size, uint16_t col, int maxw)   /* alineado a la derecha */
{
    if (!t || !*t) return;
    int sz = wg_fit(t, size, maxw), w = tw_n(t, sz, -1); if (w > maxw) w = maxw;
    draw_text(d, rx - w, base, t, sz, col, maxw, wg_cy0, wg_cy1);
}
/* corte de linea: bytes de la 1a linea (*l1) y comienzo de la 2a; en un espacio si se puede,
 * si no entre caracteres (chino, japones, palabras largas) */
static int wg_break(const char *t, int sz, int maxw, int *l1)
{
    const char *p = t, *sp = 0, *fit = t;
    while (*p) {
        const char *q = p; int c = utf8_next(&q);
        if (tw_n(t, sz, (int)(q - t)) > maxw) break;
        if (c == ' ') sp = p;
        fit = q; p = q;
    }
    if (!*p) { *l1 = (int)(p - t); return *l1; }
    if (sp && sp > t) { *l1 = (int)(sp - t); return *l1 + 1; }
    if (fit == t) { const char *q = t; utf8_next(&q); fit = q; }
    *l1 = (int)(fit - t); return *l1;
}
/* hasta dos lineas; reduce el tamano hasta 2 puntos antes de poner "…"; devuelve las lineas */
static int wt2(uint16_t *d, int x, int base, const char *t, int size, uint16_t col, int maxw, int lh, int center)
{
    if (!t || !*t) return 0;
    if (tw_n(t, size, -1) <= maxw) { if (center) wtc(d, x + maxw / 2, base, t, size, col, maxw); else draw_text(d, x, base, t, size, col, 0, wg_cy0, wg_cy1); return 1; }
    int sz = size, l1 = 0, s2 = 0;
    for (; sz >= size - 2 && sz >= 11; sz--) { s2 = wg_break(t, sz, maxw, &l1); if (tw_n(t + s2, sz, -1) <= maxw) break; }
    if (sz < size - 2 || sz < 11) { sz++; s2 = wg_break(t, sz, maxw, &l1); }
    char a[256]; int n = l1 < 255 ? l1 : 255; memcpy(a, t, n); a[n] = 0;
    const char *b = t + s2; while (*b == ' ') b++;
    if (center) {
        int w1 = tw_n(a, sz, -1), w2 = tw_n(b, sz, -1); if (w2 > maxw) w2 = maxw;
        draw_text(d, x + (maxw - w1) / 2, base, a, sz, col, maxw, wg_cy0, wg_cy1);
        if (*b) draw_text(d, x + (maxw - w2) / 2, base + lh, b, sz, col, maxw, wg_cy0, wg_cy1);
    } else {
        draw_text(d, x, base, a, sz, col, maxw, wg_cy0, wg_cy1);
        if (*b) draw_text(d, x, base + lh, b, sz, col, maxw, wg_cy0, wg_cy1);
    }
    return *b ? 2 : 1;
}
static int wg_put8(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}
static int wg_upc(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x100 && c <= 0x17F && (c & 1)) return c - 1;          /* latin extendido A (polaco) */
    return c;
}
/* mayusculas (todas, o solo la primera letra si first) */
static const char *wg_upper(const char *s, char *o, int n, int first)
{
    int k = 0, c, i = 0;
    while ((c = utf8_next(&s)) && k < n - 5) { k += wg_put8(o + k, (!first || !i) ? wg_upc(c) : c); i++; }
    o[k] = 0; return o;
}

/* ---- copias de sprites ---- */
/* copia con mascara (sin giro): las filas lejos de las esquinas van con memcpy */
static void wg_blit_m(uint16_t *dst, const uint16_t *src, int stride, const uint8_t *m, int w, int h, int x0, int y0, int ylo)
{
    int xa = x0 < 0 ? 0 : x0, xb = x0 + w > W ? W : x0 + w; if (xb <= xa || !m) return;
    for (int y = 0; y < h; y++) {
        int yy = y0 + y; if (yy < ylo || yy >= H) continue;
        uint16_t *row = dst + yy * W; const uint16_t *s = src + y * stride - x0; const uint8_t *mr = m + y * w - x0;
        if (y >= WG_R && y < h - WG_R && h == WG_H) { memcpy(row + xa, s + xa, (xb - xa) * 2); continue; }
        for (int x = xa; x < xb; x++) { unsigned a = mr[x]; if (a == 255) row[x] = s[x]; else if (a) row[x] = wg_mix(s[x], row[x], a); }
    }
}
static inline uint32_t wg_sp(uint16_t p) { return (p | ((uint32_t)p << 16)) & 0x07E0F81Fu; }
/* k en [0, n) con lo <= a0 + d*k < hi (aritmetica entera exacta, como el bucle) */
static void wg_range(long long a0, long long d, long long lo, long long hi, int n, int *k0, int *k1)
{
    long long r0, r1;
    if (d == 0) { r0 = 0; r1 = a0 >= lo && a0 < hi ? n : 0; }
    else if (d > 0) { r0 = lo - a0 <= 0 ? 0 : (lo - a0 + d - 1) / d; r1 = hi - a0 <= 0 ? 0 : (hi - a0 + d - 1) / d; }
    else { long long e = -d; r1 = a0 - lo < 0 ? 0 : (a0 - lo) / e + 1; r0 = a0 - hi < 0 ? 0 : (a0 - hi) / e + 1; }
    if (r0 < 0) r0 = 0; if (r1 > n) r1 = n; if (r0 > n) r0 = n; if (r1 < r0) r1 = r0;
    *k0 = (int)r0; *k1 = (int)r1;
}
/* un pixel del borde (esquinas suavizadas, fuera de la tarjeta): con la mascara */
static inline void wg_xf_px(uint16_t *row, int x, int U, int V2, const uint16_t *src, int stride, const uint8_t *m, int sw, int sh, int alpha)
{
    int iu = U >> 16, iv = V2 >> 16;
    if (iu < -1 || iv < -1 || iu >= sw || iv >= sh) return;
    unsigned fx = (U >> 11) & 31, fy = (V2 >> 11) & 31;
    int u0 = iu < 0 ? 0 : iu, u1 = iu + 1 >= sw ? sw - 1 : iu + 1, v0 = iv < 0 ? 0 : iv, v1 = iv + 1 >= sh ? sh - 1 : iv + 1;
    int ok_u0 = iu >= 0, ok_u1 = iu + 1 < sw, ok_v0 = iv >= 0, ok_v1 = iv + 1 < sh;
    unsigned a00 = ok_u0 && ok_v0 ? m[v0 * sw + u0] : 0, a01 = ok_u1 && ok_v0 ? m[v0 * sw + u1] : 0;
    unsigned a10 = ok_u0 && ok_v1 ? m[v1 * sw + u0] : 0, a11 = ok_u1 && ok_v1 ? m[v1 * sw + u1] : 0;
    unsigned a = ((a00 * (32 - fx) + a01 * fx) * (32 - fy) + (a10 * (32 - fx) + a11 * fx) * fy) >> 10;
    if (!a) return;
    uint32_t t0 = ((wg_sp(src[v0 * stride + u0]) * (32 - fx) + wg_sp(src[v0 * stride + u1]) * fx) >> 5) & 0x07E0F81Fu;
    uint32_t t1 = ((wg_sp(src[v1 * stride + u0]) * (32 - fx) + wg_sp(src[v1 * stride + u1]) * fx) >> 5) & 0x07E0F81Fu;
    uint32_t v = ((t0 * (32 - fy) + t1 * fy) >> 5) & 0x07E0F81Fu;
    uint16_t col = (uint16_t)(v | (v >> 16));
    if (alpha < 255) a = (a * alpha) >> 8;
    row[x] = a >= 254 ? col : wg_mix(col, row[x], a);
}
/* sprite girado "ang" y escalado "sc" con centro en (cx, cy): bilineal en coma fija (el
 * temblor del modo edicion y el encogerse al quitar). Colores con el truco 565 "separado"
 * (una multiplicacion por muestra) y alfa de la mascara */
static void wg_blit_xf(uint16_t *dst, int ylo, const uint16_t *src, int stride, const uint8_t *m, int sw, int sh,
                       float cx, float cy, float ang, float sc, int alpha)
{
    if (sc < 0.03f || alpha <= 0 || !m) return;
    if (ang == 0 && sc == 1 && alpha >= 255) {
        float fx = cx - sw / 2.f, fy = cy - sh / 2.f;
        if (fx == floorf(fx) && fy == floorf(fy)) { wg_blit_m(dst, src, stride, m, sw, sh, (int)fx, (int)fy, ylo); return; }
    }
    float c = cosf(ang), s = sinf(ang), hw = sw * sc / 2, hh = sh * sc / 2;
    float ex = fabsf(c) * hw + fabsf(s) * hh, ey = fabsf(s) * hw + fabsf(c) * hh;
    int xa = (int)floorf(cx - ex) - 1, xb = (int)ceilf(cx + ex) + 1, ya = (int)floorf(cy - ey) - 1, yb = (int)ceilf(cy + ey) + 1;
    if (xa < 0) xa = 0; if (xb > W) xb = W; if (ya < ylo) ya = ylo; if (yb > H) yb = H;
    float inv = 1.f / sc;
    int du = (int)(c * inv * 65536), dv = (int)(-s * inv * 65536), opaque = alpha >= 255 && sh == WG_H;
    unsigned aint = !opaque && sh == WG_H ? (unsigned)(alpha * 33) >> 8 : 0;
    for (int y = ya; y < yb; y++) {
        float dy = y + .5f - cy, dx = xa + .5f - cx;
        int U = (int)(((c * dx + s * dy) * inv + sw / 2.f - .5f) * 65536), V2 = (int)(((-s * dx + c * dy) * inv + sh / 2.f - .5f) * 65536);
        uint16_t *row = dst + y * W;
        int n = xb - xa, k0 = 0, k1 = 0;
        if (sh == WG_H && (opaque || aint)) {   /* tramo interior (mascara 255): se resuelve una vez por fila */
            int a0, a1, b0, b1;
            wg_range(U, du, 0, (long long)(sw - 1) << 16, n, &a0, &a1);
            wg_range(V2, dv, (long long)WG_R << 16, (long long)(sh - (int)WG_R - 1) << 16, n, &b0, &b1);
            k0 = a0 > b0 ? a0 : b0; k1 = a1 < b1 ? a1 : b1; if (k1 < k0) k1 = k0;
            /* franjas de arriba y abajo: entre las esquinas la mascara tambien es 255 */
            int c0, c1, d0, d1;
            wg_range(U, du, (long long)WG_R << 16, (long long)(sw - (int)WG_R - 1) << 16, n, &c0, &c1);
            wg_range(V2, dv, 0, (long long)(sh - 1) << 16, n, &d0, &d1);
            int e0 = c0 > d0 ? c0 : d0, e1 = c1 < d1 ? c1 : d1;
            if (e1 > e0) { if (k1 <= k0) { k0 = e0; k1 = e1; } else if (e0 <= k1 && e1 >= k0) { if (e0 < k0) k0 = e0; if (e1 > k1) k1 = e1; } }
        }
        for (int k = 0; k < k0; k++) wg_xf_px(row, xa + k, U + du * k, V2 + dv * k, src, stride, m, sw, sh, alpha);
        {
            int Uk = U + du * k0, Vk = V2 + dv * k0; uint16_t *o = row + xa + k0, *oe = row + xa + k1;
            if (opaque) for (; o < oe; o++, Uk += du, Vk += dv) {
                unsigned fx = (Uk >> 11) & 31, fy = (Vk >> 11) & 31;
                const uint16_t *pp = src + (Vk >> 16) * stride + (Uk >> 16);
                uint32_t t0 = ((wg_sp(pp[0]) * (32 - fx) + wg_sp(pp[1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t t1 = ((wg_sp(pp[stride]) * (32 - fx) + wg_sp(pp[stride + 1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t v = ((t0 * (32 - fy) + t1 * fy) >> 5) & 0x07E0F81Fu;
                *o = (uint16_t)(v | (v >> 16));
            } else for (; o < oe; o++, Uk += du, Vk += dv) {
                unsigned fx = (Uk >> 11) & 31, fy = (Vk >> 11) & 31;
                const uint16_t *pp = src + (Vk >> 16) * stride + (Uk >> 16);
                uint32_t t0 = ((wg_sp(pp[0]) * (32 - fx) + wg_sp(pp[1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t t1 = ((wg_sp(pp[stride]) * (32 - fx) + wg_sp(pp[stride + 1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t v = ((t0 * (32 - fy) + t1 * fy) >> 5) & 0x07E0F81Fu;
                v = ((v * aint + wg_sp(*o) * (32 - aint)) >> 5) & 0x07E0F81Fu;
                *o = (uint16_t)(v | (v >> 16));
            }
        }
        for (int k = k1; k < n; k++) wg_xf_px(row, xa + k, U + du * k, V2 + dv * k, src, stride, m, sw, sh, alpha);
    }
}

/* ------------------------------------------------------------------ dibujo de cada widget */
#define C_SEC rgb(160, 160, 168)
#define C_RED rgb(255, 69, 58)
#define C_GREEN rgb(48, 209, 88)
#define C_YELLOW rgb(255, 214, 10)
#define C_BLUE rgb(10, 132, 255)
#define C_PURPLE rgb(191, 90, 242)
#define C_PINK rgb(255, 55, 95)
static const char *wg_gb(long long b, char *o, int n)
{
    double g = b / 1073741824.0;
    if (g >= 1024) snprintf(o, n, "%.1f TB", g / 1024); else if (g >= 10) snprintf(o, n, "%.0f GB", g); else if (g >= 1) snprintf(o, n, "%.1f GB", g); else snprintf(o, n, "%.0f MB", b / 1048576.0);
    return o;
}
static uint16_t wg_batt_col(int pct, int chg, int self)
{
    if (pct < 0) return C_SEC;
    if (pct <= 20 && !chg) return C_RED;
    if (self && pw_mode == PW_SAVE && !chg) return C_YELLOW;
    return C_GREEN;
}
static void wg_icon(uint16_t *d, const char *name, int cx, int cy)
{
    if (!cc_ic_ok) { for (int i = 0; cc_ic_name[i]; i++) { char n[32]; snprintf(n, sizeof n, "cc_%s", cc_ic_name[i]); load_sprite(n, &cc_ic[i]); } cc_ic_ok = 1; }
    const struct sprite *s = cc_icon(name); if (s && s->w) blit(d, s, cx - s->w / 2, cy - s->h / 2, 32, 255);
}
static void wg_ctrl(uint16_t *d, int c, int cy, int kind, int playing)   /* 0 anterior, 1 play/pausa, 2 siguiente */
{
    if (kind == 0) { fill_rrect(d, c - 11, cy - 7, 3, 14, 1, 0xFFFF, 255); cc_tri(d, c, cy, 8, 7, -1); cc_tri(d, c + 8, cy, 8, 7, -1); }
    else if (kind == 2) { cc_tri(d, c - 8, cy, 8, 7, 1); cc_tri(d, c, cy, 8, 7, 1); fill_rrect(d, c + 8, cy - 7, 3, 14, 1, 0xFFFF, 255); }
    else if (playing) { fill_rrect(d, c - 7, cy - 9, 5, 18, 1.5f, 0xFFFF, 255); fill_rrect(d, c + 2, cy - 9, 5, 18, 1.5f, 0xFFFF, 255); }
    else cc_tri(d, c - 6, cy, 15, 9, 1);
}
static const char *const wg_dn[7] = { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" };
static const char *const wg_mn[12] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
static void wg_render(int type, int sz, uint16_t *d, int w, int h)
{
    char b[200], u[200], s1[24], s2[24]; const struct wdata *D = &WDs;
    const int p = 14, cw = w - 2 * p;
    (void)h;
    switch (type) {
    case WG_BATT: {
        struct { int pct, kind, chg; } dv[4]; int nd = 0;
        dv[nd].pct = D->batt; dv[nd].kind = 0; dv[nd++].chg = D->charging;
        for (int i = 0; i < D->nbt && nd < 4; i++) { dv[nd].pct = D->bt[i].pct; dv[nd].kind = 1; dv[nd++].chg = 0; }
        if (!sz && nd == 1) {
            uint16_t col = wg_batt_col(dv[0].pct, dv[0].chg, 1);
            wg_ring(d, w / 2.f, 60, 38, 8, dv[0].pct / 100.f, col);
            wg_glyph_player(d, w / 2.f, 60, 30, 0xFFFF);
            snprintf(b, sizeof b, "%d %%", dv[0].pct < 0 ? 0 : dv[0].pct);
            int tw = tw_n(b, 22, -1), bw = dv[0].chg ? 12 : 0, x = (w - tw - bw) / 2;
            draw_text(d, x, 124, b, 22, 0xFFFF, 0, wg_cy0, wg_cy1);
            if (bw) wg_bolt(d, x + tw + 3, 108, 16, C_GREEN);
        } else {
            int slots = sz ? 4 : (nd == 2 ? 2 : 4);
            for (int i = 0; i < slots; i++) {
                float cx, cy, r, th; int pct_txt;
                if (sz) { float sw = cw / 4.f; cx = p + sw * i + sw / 2; cy = 58; r = 25; th = 7; pct_txt = 1; }
                else if (slots == 2) { cx = i ? w * .75f - 3 : w * .25f + 3; cy = 56; r = 25; th = 7; pct_txt = 1; }
                else { cx = (i & 1) ? w * .75f - 4 : w * .25f + 4; cy = (i & 2) ? 99 : 38; r = 22; th = 6; pct_txt = 0; }
                if (i >= nd) { wg_arc(d, cx, cy, r, th, 0, 0, rgb(120, 120, 128), 70); continue; }   /* hueco libre: pista tenue */
                uint16_t col = wg_batt_col(dv[i].pct, dv[i].chg, !dv[i].kind);
                wg_ring(d, cx, cy, r, th, dv[i].pct / 100.f, col);
                if (dv[i].kind) wg_glyph_phones(d, cx, cy, r * .8f, 0xFFFF); else wg_glyph_player(d, cx, cy, r * .8f, 0xFFFF);
                if (pct_txt) {
                    if (dv[i].pct >= 0) snprintf(b, sizeof b, "%d %%", dv[i].pct); else snprintf(b, sizeof b, "—");
                    int tw = tw_n(b, 15, -1), bw = dv[i].chg ? 9 : 0, x = (int)cx - (tw + bw) / 2;
                    draw_text(d, x, (int)cy + (int)r + 26, b, 15, 0xFFFF, 0, wg_cy0, wg_cy1);
                    if (bw) wg_bolt(d, x + tw + 2, cy + r + 14, 12, C_GREEN);
                }
            }
        }
        break; }
    case WG_NOW: {
        int have = wg_ni.source != 0, cs = sz ? 109 : 64, a = app_by_id("nowplaying");
        if (have && wg_cok) npx_blit_cover(d, wg_cov, p, p, cs, sz ? 12 : 10);
        else {
            fill_rrect(d, p, p, cs, cs, sz ? 12 : 10, rgb(58, 58, 64), 255);
            if (a >= 0) blit_scaled(d, &apps[a].icon[1], p + (cs - 40) / 2, p + (cs - 40) / 2, 40, 40, 255);
        }
        const char *ti = have && wg_ni.title[0] ? wg_ni.title : tr("Reproduciendo");
        const char *ar = have ? wg_ni.artist : "";
        int pl = wg_playing();
        if (!sz) {
            if (have && pl) for (int i = 0; i < 3; i++) { int hh = 6 + (i == 1 ? 6 : i * 2); fill_rrect(d, w - p - 16 + i * 6, p + 16 - hh, 3, hh, 1.5f, C_PINK, 255); }
            wt(d, p, 103, ti, 14, 0xFFFF, cw);
            wt(d, p, 121, ar[0] ? ar : tr(have ? "En pausa" : "Sin reproducción"), 12, C_SEC, cw);
        } else {
            int x0 = p + cs + 14, tw = w - x0 - p;
            wt(d, x0, 29, tr(!have ? "Sin reproducción" : pl ? "Sonando" : "En pausa"), 11, pl ? C_PINK : C_SEC, tw);
            int ln = wt2(d, x0, 49, ti, 15, 0xFFFF, tw, 18, 0);
            wt(d, x0, 49 + ln * 18 + 1, ar, 12, C_SEC, tw);
            for (int k = 0; k < 3; k++) wg_ctrl(d, x0 + 18 + k * (tw - 36) / 2, 109, k, pl);
        }
        break; }
    case WG_DATE: {
        time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
        int lw = sz ? 112 : cw;
        wt(d, p, 30, wg_upper(tr(wg_dn[tm.tm_wday]), u, sizeof u, 0), 13, C_RED, lw);
        snprintf(b, sizeof b, "%d", tm.tm_mday); draw_text(d, p - 2, 88, b, 52, 0xFFFF, 0, wg_cy0, wg_cy1);
        snprintf(b, sizeof b, "%s %d", wg_upper(tr(wg_mn[tm.tm_mon]), u, sizeof u, 1), 1900 + tm.tm_year);
        wt(d, p, 116, b, 15, 0xFFFF, lw);
        if (sz) {   /* calendario del mes, hoy en un circulo rojo */
            int gx = 132, gw = w - gx - 10; float colw = gw / 7.f;
            int sun = ui_lang == LANG_EN || ui_lang == LANG_JA || ui_lang == LANG_KO || ui_lang == LANG_ZHS || ui_lang == LANG_ZHT || ui_lang == LANG_TH;
            int first = (tm.tm_wday - (tm.tm_mday - 1) % 7 + 7) % 7;             /* dia de la semana del 1 */
            int off = sun ? first : (first + 6) % 7;
            static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int y = 1900 + tm.tm_year, nd = mdays[tm.tm_mon] + (tm.tm_mon == 1 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
            int rows = (off + nd + 6) / 7, rh = rows > 5 ? 15 : 17, y0 = 34 + rh;
            for (int c = 0; c < 7; c++) {   /* iniciales: primera letra del dia (en chino, la ultima) */
                const char *nm = tr(wg_dn[sun ? c : (c + 1) % 7]); const char *q = nm; int ch = utf8_next(&q), last = ch;
                if (ch >= 0x0E00 && ch <= 0x0E7F) break;                         /* tailandes: sin iniciales */
                if (ch == 0x661F || ch == 0x5468 || ch == 0x9031 || ch == 0x79AE) { const char *r = nm; int k; while ((k = utf8_next(&r))) last = k; ch = last; }
                char l[8]; int n = wg_put8(l, wg_upc(ch)); l[n] = 0;
                wtc(d, gx + (int)(colw * c + colw / 2), 28, l, 10, C_SEC, (int)colw);
            }
            for (int dd = 1; dd <= nd; dd++) {
                int k = off + dd - 1, c = k % 7, r = k / 7; float cx = gx + colw * c + colw / 2; int base = y0 + r * rh;
                snprintf(b, sizeof b, "%d", dd);
                if (dd == tm.tm_mday) fill_circle(d, cx, base - 4.5f, 8.5f, C_RED, 255);
                wtc(d, (int)cx, base, b, 11, dd == tm.tm_mday ? 0xFFFF : rgb(225, 225, 230), (int)colw + 2);
            }
        }
        break; }
    case WG_STORAGE: {
        if (!sz) {
            int sd = D->sd_tot > 0; long long fr = sd ? D->sd_free : D->in_free, tot = sd ? D->sd_tot : D->in_tot;
            wt(d, p, 30, tr(sd ? "Tarjeta SD" : "En el HiBy"), 13, 0xFFFF, cw);
            uint16_t col = sd ? C_BLUE : C_PURPLE; float cx = w / 2.f, cy = 84;
            wg_ring(d, cx, cy, 36, 8, tot ? 1 - (float)fr / tot : 0, col);
            wtc(d, (int)cx, (int)cy + 3, wg_gb(fr, s1, sizeof s1), 16, 0xFFFF, 56);
            wtc(d, (int)cx, (int)cy + 17, tr("libres"), 10, C_SEC, 56);
        } else {
            wt(d, p, 30, tr("Almacenamiento"), 13, 0xFFFF, cw);
            for (int k = 0; k < 2; k++) {
                int ry = 42 + k * 46; long long fr = k ? D->in_free : D->sd_free, tot = k ? D->in_tot : D->sd_tot;
                uint16_t col = k ? C_PURPLE : C_BLUE;
                wg_ring(d, p + 17, ry + 20, 15, 5, tot ? 1 - (float)fr / tot : 0, col);
                if (!k && !tot) { wt(d, p + 44, ry + 26, tr("Sin tarjeta SD"), 15, C_SEC, cw - 44); continue; }
                int pc = tot ? (int)(100 - 100 * fr / tot + .5) : 0; snprintf(b, sizeof b, "%d %%", pc);
                int pw = tw_n(b, 15, -1);
                draw_text(d, w - p - pw, ry + 26, b, 15, 0xFFFF, 0, wg_cy0, wg_cy1);
                wt(d, p + 44, ry + 17, tr(k ? "En el HiBy" : "Tarjeta SD"), 15, 0xFFFF, cw - 44 - pw - 8);
                snprintf(u, sizeof u, tr("%s libres de %s"), wg_gb(fr, s1, sizeof s1), wg_gb(tot, s2, sizeof s2));
                wt(d, p + 44, ry + 34, u, 12, C_SEC, cw - 44 - pw - 8);
            }
        }
        break; }
    case WG_POWER: {
        uint16_t col = pw_mode == PW_SAVE ? rgb(255, 204, 0) : pw_mode == PW_PERF ? C_RED : C_BLUE;
        fill_circle(d, p + 19, p + 19, 19, col, 255);
        wg_icon(d, pw_mode == PW_SAVE ? "leaf" : pw_mode == PW_PERF ? "bolt" : "gauge", p + 19, p + 19);
        int lw = sz ? 128 : cw;
        wt(d, p, 90, tr(pw_name(pw_mode)), 22, 0xFFFF, lw);
        snprintf(b, sizeof b, "%s · %d Hz", tr("Pantalla"), lcd_hz); wt(d, p, 109, b, 12, C_SEC, lw);
        const char *stt = pw_nocharge && D->usb ? tr("Solo USB") : D->charging ? tr("Cargando") : D->usb ? tr("Conectado por USB") : tr("Con batería");
        wt(d, p, 126, stt, 12, C_SEC, lw);
        if (!sz) { snprintf(b, sizeof b, "%d %%", D->batt < 0 ? 0 : D->batt); wtr(d, w - p, 31, b, 13, C_SEC, 60); }
        else {
            float cx = w - p - 48, cy = 68;
            wg_ring(d, cx, cy, 42, 9, D->batt / 100.f, wg_batt_col(D->batt, D->charging, 1));
            snprintf(b, sizeof b, "%d %%", D->batt < 0 ? 0 : D->batt); wtc(d, (int)cx, (int)cy + 4, b, 20, 0xFFFF, 64);
            wtc(d, (int)cx, (int)cy + 19, tr("Batería"), 10, C_SEC, 60);
        }
        break; }
    case WG_LIB: {
        wt(d, p, 30, tr("Biblioteca"), 13, C_PINK, cw);
        float fav = D->songs ? (float)D->favs / D->songs : 0;
        if (!sz) {
            snprintf(b, sizeof b, "%d", D->songs); wt(d, p, 74, b, 34, 0xFFFF, cw);
            wt(d, p, 93, tr("canciones"), 13, C_SEC, cw);
        } else {
            int v[3] = { D->songs, D->albums, D->artists }; const char *l[3] = { "canciones", "álbumes", "artistas" };
            for (int k = 0; k < 3; k++) { int cx = p + k * cw / 3; snprintf(b, sizeof b, "%d", v[k]); wt(d, cx, 74, b, 26, 0xFFFF, cw / 3 - 6); wt(d, cx, 92, tr(l[k]), 12, C_SEC, cw / 3 - 6); }
        }
        wg_ring(d, p + 8, 115, 7, 3.5f, fav, C_PINK);
        if (sz) snprintf(b, sizeof b, "%d %s · %s", D->favs, tr("favoritas"), wg_gb(D->lib_bytes, s1, sizeof s1));
        else snprintf(b, sizeof b, "%d %s", D->favs, tr("favoritas"));
        wt(d, p + 22, 120, b, 12, C_SEC, cw - 22);
        break; }
    case WG_NEW: {
        wt(d, p, 30, tr("Recién añadido"), 13, C_PINK, cw);
        int ln = wt2(d, p, 57, D->new_album[0] ? D->new_album : "—", sz ? 18 : 16, 0xFFFF, cw, sz ? 21 : 19, 0);
        wt(d, p, 57 + ln * (sz ? 21 : 19) + 2, D->new_artist, 13, C_SEC, cw);
        break; }
    case WG_LISTEN: {
        wt(d, p, 30, tr("Escuchas"), 13, C_PINK, cw);
        int colw = sz ? (cw - 14) / 2 : cw, x1 = sz ? p + colw + 14 : p;
        if (sz) {
            wt(d, p, 50, tr("Lo último"), 11, C_SEC, colw);
            int ln = wt2(d, p, 70, D->last_title[0] ? D->last_title : "—", 15, 0xFFFF, colw, 18, 0);
            wt(d, p, 70 + ln * 18 + 1, D->last_artist, 12, C_SEC, colw);
            for (int y = 42; y < 126; y++) wg_px(d, p + colw + 7, y, 0xFFFF, 40);
        }
        wt(d, x1, 50, tr("Artista más escuchado"), 11, C_SEC, colw);
        wt2(d, x1, 70, D->top_artist[0] ? D->top_artist : "—", sz ? 15 : 16, 0xFFFF, colw, 18, 0);
        if (D->top_n) { snprintf(b, sizeof b, "%d %s", D->top_n, tr("escuchas")); wt(d, x1, 122, b, sz ? 12 : 13, C_SEC, colw); }
        break; }
    }
}

/* ------------------------------------------------------------------ sprites */
struct wspr { uint16_t *px; uint32_t sig; };
static struct wspr wg_spr[WG_N][2];
static uint32_t wg_hs(uint32_t h, const char *s) { while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static uint32_t wg_sig(int type, int sz, int sx, int sy)
{
    uint32_t h = 2166136261u; const struct wdata *D = &WDs;
#define HM(v) h = (h ^ (uint32_t)(v)) * 16777619u
    HM(type); HM(sz); HM(sx); HM(sy); HM(ui_lang); HM(wall_gen);
    switch (type) {
    case WG_BATT: HM(D->batt); HM(D->charging); HM(D->nbt); HM(pw_mode); for (int i = 0; i < D->nbt; i++) HM(D->bt[i].pct); break;
    case WG_NOW: HM(wg_ni.source); HM(wg_ni.track_id); HM(wg_playing()); HM(wg_cok); h = wg_hs(h, wg_ni.title); h = wg_hs(h, wg_ni.artist); break;
    case WG_DATE: { time_t now = time(0); struct tm tm; localtime_r(&now, &tm); HM(tm.tm_yday); HM(tm.tm_year); break; }
    case WG_STORAGE: HM(D->sd_free >> 24); HM(D->sd_tot >> 24); HM(D->in_free >> 24); HM(D->in_tot >> 24); break;
    case WG_POWER: HM(pw_mode); HM(lcd_hz); HM(pw_nocharge); HM(D->charging); HM(D->usb); HM(D->batt); break;
    case WG_LIB: HM(D->songs); HM(D->albums); HM(D->artists); HM(D->favs); HM(D->lib_bytes >> 20); break;
    case WG_NEW: h = wg_hs(h, D->new_album); h = wg_hs(h, D->new_artist); break;
    case WG_LISTEN: h = wg_hs(h, D->last_title); h = wg_hs(h, D->last_artist); h = wg_hs(h, D->top_artist); HM(D->top_n); break;
    }
#undef HM
    return h ? h : 1;
}
/* dibuja el widget en wg_scr (filas 0..WG_H) con el cristal de la posicion (sx, sy) */
static void wg_draw_scr(int type, int sz, int sx, int sy)
{
    wg_masks_init();
    int w = wg_w(sz), c0 = wg_cy0, c1 = wg_cy1; wg_cy0 = 0; wg_cy1 = WG_SH;
    wg_card_bg(wg_scr, w, WG_H, sx, sy, sz);
    wg_render(type, sz, wg_scr, w, WG_H);
    wg_cy0 = c0; wg_cy1 = c1;
}
static int wg_build(int type, int sz, int sx, int sy)
{
    struct wspr *s = &wg_spr[type][sz]; int w = wg_w(sz);
    if (!s->px) s->px = malloc(w * WG_H * 2);
    if (!s->px) return 0;
    wg_draw_scr(type, sz, sx, sy);
    for (int y = 0; y < WG_H; y++) memcpy(s->px + y * w, wg_scr + y * W, w * 2);
    s->sig = wg_sig(type, sz, sx, sy);
    return 1;
}
static void wg_free_unused(void)
{
    for (int t = 0; t < WG_N; t++) for (int s = 0; s < 2; s++) if (wg_spr[t][s].px && !wg_has(t, s)) { free(wg_spr[t][s].px); wg_spr[t][s].px = 0; }
}

/* ------------------------------------------------------------------ pagina (estatica) */
static float wg_scroll, wg_cache_scroll; static int wg_cache_min;
static uint16_t *wg_cache;
static int wg_budget = 99;                   /* sprites que se pueden rehacer en esta composicion */
static int wg_part_y0, wg_part_y1;           /* filas cambiadas (presentacion parcial) */
static int wg_press = -1, wg_press_btn = -1, wg_scrolling;
static int wg_snap_ok;
static int wg_content_h(void) { short xs[WG_MAX], ys[WG_MAX]; return wg_layout_items(xs, ys) + 46; }
static void wg_compose(void)
{
    if (!wg_loaded && !widgets_on && V.scroll >= 0) { if (!WD.t) wg_refresh(); return; }   /* precomposicion: esperar a los datos */
    if (!wg_cache) wg_cache = malloc(HM_FRAME_BYTES);
    if (!wg_cache) return;
    wg_masks_init();
    if (wg_fresh || !wg_snap_ok) { wg_fresh = 0; wg_snap(); wg_snap_ok = 1; }
    memcpy(wg_cache + 30 * W, wall + 30 * W, (H - 30) * W * 2);
    short xs[WG_MAX], ys[WG_MAX]; int tot = wg_layout_items(xs, ys);
    int top = WG_TOP - (int)wg_scroll, pending = 0, y0 = H, y1 = 0, c0 = wg_cy0, c1 = wg_cy1;
    for (int i = 0; i < wg_n; i++) {
        int t = wg_it[i].type, s = wg_it[i].size, x = xs[i], y = top + ys[i];
        if (y >= H || y + WG_H <= 30) continue;
        struct wspr *sp = &wg_spr[t][s];
        uint32_t sig = wg_sig(t, s, x, WG_TOP + ys[i]);
        if (!sp->px || sp->sig != sig) {
            if (!sp->px || wg_budget > 0) { wg_build(t, s, x, WG_TOP + ys[i]); wg_budget--; if (y < y0) y0 = y; if (y + WG_H > y1) y1 = y + WG_H; }
            else pending = 1;
        }
        if (sp->px) wg_blit_m(wg_cache, sp->px, wg_w(s), wg_mask[s], wg_w(s), WG_H, x, y, 30);
    }
    /* "Editar" al final (como iOS) */
    int ey = top + tot + 4;
    wg_cy0 = 30; wg_cy1 = H;
    if (!wg_n) wtc(wg_cache, W / 2, top + 150, tr("Sin widgets"), 15, rgb(235, 235, 240), W - 40), ey = top + 170;
    int bw = tw_n(tr("Editar"), 14, -1) + 32;
    if (ey < H && ey + 30 > 30) { fill_rrect(wg_cache, (W - bw) / 2, ey, bw, 30, 15, rgb(20, 20, 24), 120); draw_text(wg_cache, (W - bw) / 2 + 16, ey + 20, tr("Editar"), 14, 0xFFFF, 0, 30, H); }
    wg_cy0 = c0; wg_cy1 = c1;
    if (pending) wg_fresh = 1;
    if (y1 > y0) { wg_part_y0 = y0 < 30 ? 30 : y0; wg_part_y1 = y1 > H ? H : y1; } else { wg_part_y0 = 30; wg_part_y1 = 31; }
    wg_cache_min = (int)(time(0) / 60); wg_cache_scroll = wg_scroll; wg_cache_ok = 1;
}
static void wg_np_poll(void)        /* la pista o el estado cambiaron (sin copiar la caratula) */
{
    pthread_mutex_lock(&npx_mu);
    int ch = npx.track_id != wg_ni.track_id || npx.playing != wg_ni.playing || npx.source != wg_ni.source || npx_cov_ok != wg_cok;
    pthread_mutex_unlock(&npx_mu);
    if (ch) wg_fresh = 1;
}
/* dibuja la pagina con su borde izquierdo en ox (ox = -W .. 0) */
static void widgets_draw(uint16_t *d, int ox)
{
    if (ox <= -W || ox >= W) return;
    if (!WD.t || now_ms() - WD.t > 10000) wg_refresh();
    if (npx_changed && widgets_on) npx_changed = 0;
    wg_np_poll();
    int settled = ox == 0 && !wg_scrolling, sch = wg_cache_scroll != wg_scroll;
    if (!wg_cache_ok || wg_fresh || sch || (int)(time(0) / 60) != wg_cache_min) {
        int data_only = wg_cache_ok && !sch && settled;
        wg_budget = settled ? 99 : 0;
        wg_compose(); wg_budget = 99;
        if (!data_only) wg_part_y0 = wg_part_y1 = 0;
    } else wg_part_y0 = wg_part_y1 = 0;
    if (!wg_cache) return;
    int x0 = ox < 0 ? 0 : ox, x1 = ox + W > W ? W : ox + W;
    if (x1 > x0) for (int y = 30; y < H; y++) memcpy(d + y * W + x0, wg_cache + y * W + (x0 - ox), (x1 - x0) * 2);
    if (wg_press >= 0 && wg_press < wg_n && ox == 0) {   /* pulsado: se oscurece (o el boton de control) */
        short xs[WG_MAX], ys[WG_MAX]; wg_layout_items(xs, ys);
        int s = wg_it[wg_press].size, w = wg_w(s), x = xs[wg_press], y = WG_TOP - (int)wg_scroll + ys[wg_press];
        if (wg_press_btn >= 0) { int x0b = x + 14 + 109 + 14, tw = w - (x0b - x) - 14; fill_circle(d, x0b + 18 + wg_press_btn * (tw - 36) / 2.f, y + 109, 20, 0xFFFF, 46); }
        else for (int yy = 0; yy < WG_H; yy++) { int Y = y + yy; if (Y < 30 || Y >= H) continue; for (int xx = 0; xx < w; xx++) { int a = wg_mask[s][yy * w + xx]; if (a) d[Y * W + x + xx] = wg_mix(0, d[Y * W + x + xx], a * 70 >> 8); } }
        wg_part_y0 = wg_part_y1 = 0;
    }
}
/* widget en (px, py) en la pagina normal: indice, -2 = Editar, -1 nada; *btn = control */
static int wg_hit(int px, int py, int *btn)
{
    short xs[WG_MAX], ys[WG_MAX]; int tot = wg_layout_items(xs, ys), top = WG_TOP - (int)wg_scroll;
    *btn = -1;
    for (int i = 0; i < wg_n; i++) {
        int w = wg_w(wg_it[i].size), x = xs[i], y = top + ys[i];
        if (px >= x && px < x + w && py >= y && py < y + WG_H) {
            if (wg_it[i].type == WG_NOW && wg_it[i].size && wg_ni.source && py >= y + 84) {
                int x0b = x + 137, tw = w - 137 - 14;
                for (int k = 0; k < 3; k++) if (abs(px - (x0b + 18 + k * (tw - 36) / 2)) < 24) *btn = k;
            }
            return i;
        }
    }
    int ey = top + (wg_n ? tot + 4 : 170);
    if (py >= ey - 6 && py < ey + 36 && abs(px - W / 2) < 70) return -2;
    return -1;
}

/* ------------------------------------------------------------------ modo edicion */
static float wg_shift;
static int wg_edit_on;
static double wg_t0;
static void wg_targets(int drag, int slot, short *tx, short *ty, int *tot)
{
    int ord[WG_MAX], n = 0;
    for (int i = 0; i < wg_n; i++) if (i != drag) ord[n++] = i;
    if (drag >= 0) { if (slot < 0) slot = 0; if (slot > n) slot = n; memmove(&ord[slot + 1], &ord[slot], (n - slot) * sizeof(int)); ord[slot] = drag; n++; }
    unsigned char sz[WG_MAX]; short xs[WG_MAX], ys[WG_MAX];
    for (int k = 0; k < n; k++) sz[k] = wg_it[ord[k]].size;
    int t = wg_layout(sz, n, xs, ys); if (tot) *tot = t;
    for (int k = 0; k < n; k++) { tx[ord[k]] = xs[k]; ty[ord[k]] = ys[k]; }
}
static void wg_plus(uint16_t *d, int cx, int cy, int a)
{
    fill_rrect(d, cx - 7, cy - 1, 14, 3, 1.2f, 0xFFFF, a); fill_rrect(d, cx - 1, cy - 7, 3, 14, 1.2f, 0xFFFF, a);
}
static double wg_pf_bg, wg_pf_bl;
static void wg_edit_frame(uint16_t *d, double t, float pa, int drag)
{
    double q0 = now_ms();
    compose_statusbar(d);
    memcpy(d + 30 * W, wall + 30 * W, (H - 30) * W * 2);
    wg_pf_bg += now_ms() - q0;
    int top = WG_TOP + (int)wg_shift - (int)wg_scroll;
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < wg_n; i++) {
        if ((pass == 0) == (i == drag)) continue;
        struct witem *it = &wg_it[i]; struct wspr *sp = &wg_spr[it->type][it->size];
        int w = wg_w(it->size); float cx = it->ax + w / 2.f, cy = top + it->ay + WG_H / 2.f;
        if (!sp->px || cy - WG_H > H || cy + WG_H < 30) continue;
        if (i == drag) {   /* levantado: sombra suave y un poco mas grande, sin temblor */
            const uint8_t *sh = wg_shadow[it->size]; int sw = w + 2 * WG_SP, shh = WG_H + 2 * WG_SP;
            int sx0 = (int)(cx - sw / 2.f), sy0 = (int)(cy - shh / 2.f) + 8;
            if (sh) for (int y = 0; y < shh; y++) { int Y = sy0 + y; if (Y < 30 || Y >= H) continue;
                for (int x = 0; x < sw; x++) { int X = sx0 + x; if (X < 0 || X >= W) continue; int a = sh[y * sw + x] * 100 >> 8; if (a) d[Y * W + X] = wg_mix(0, d[Y * W + X], a); } }
            wg_blit_xf(d, 30, sp->px, w, wg_mask[it->size], w, WG_H, cx, cy, 0, 1.04f, 255);
            continue;
        }
        float amp = 1.3f / (w / 2.f);           /* +-1,3 px en el borde: igual para pequenos y medianos */
        float ang = pa > 0 ? amp * pa * sinf((float)((t - wg_t0) / 260.0 * 6.2831853) + i * 1.9f) : 0;
        double q1 = now_ms();
        wg_blit_xf(d, 30, sp->px, w, wg_mask[it->size], w, WG_H, cx, cy, ang, it->sc, (int)(255 * it->al));
        wg_pf_bl += now_ms() - q1;
        if (pa > 0.01f && !it->rm) {   /* "−" en la esquina, girando con el widget */
            float ox = (-w / 2.f + 9) * it->sc, oy = (-WG_H / 2.f + 9) * it->sc, c = cosf(ang), s = sinf(ang);
            float bx = cx + ox * c - oy * s, by = cy + ox * s + oy * c; int a = (int)(255 * pa * it->al);
            if (by > 40) { fill_circle(d, bx, by, 12 * it->sc, rgb(232, 232, 237), a); fill_rrect(d, (int)(bx - 6), (int)(by - 1), 12, 3, 1.2f, rgb(28, 28, 30), a); }
        }
    }
    if (pa > 0.01f) {   /* pildoras: "+" (galeria) y "Listo" */
        int a = (int)(64 * pa), ty = 34 - (int)(8 * (1 - pa));
        fill_rrect(d, 14, ty, 44, 30, 15, 0xFFFF, a); wg_plus(d, 36, ty + 15, (int)(255 * pa));
        int dw = pill_done.w + 30; fill_rrect(d, W - 14 - dw, ty, dw, 30, 15, 0xFFFF, a);
        blit(d, &pill_done, W - 14 - dw + 15, ty + (30 - pill_done.h) / 2, 32, (int)(255 * pa));
    }
    draw_toast(d);
}
/* toque: >= 0 widget, -1 vacio, -10 "+", -11 "Listo", -100-i "−" del widget i */
static int wg_edit_hit(int x, int y, int drag)
{
    if (y >= 28 && y < 70) { if (x < 66) return -10; if (x > W - 110) return -11; }
    int top = WG_TOP + (int)wg_shift - (int)wg_scroll;
    for (int i = wg_n - 1; i >= 0; i--) {
        if (i == drag || wg_it[i].rm) continue;
        float bx = wg_it[i].ax + 9, by = top + wg_it[i].ay + 9;
        if ((x - bx) * (x - bx) + (y - by) * (y - by) < 20 * 20) return -100 - i;
    }
    for (int i = 0; i < wg_n; i++) {
        if (wg_it[i].rm) continue;
        int w = wg_w(wg_it[i].size), wx = (int)wg_it[i].ax, wy = top + (int)wg_it[i].ay;
        if (x >= wx && x < wx + w && y >= wy && y < wy + WG_H) return i;
    }
    return -1;
}
static int wg_gallery(int *otype, int *osize);
/* bucle del modo edicion. press: widget pulsado (pulsacion larga en curso, puede arrastrarse
 * sin levantar el dedo); embedded: abierto desde Ajustes (ya entra con las pildoras y sale sin
 * animar: quien llama hace la transicion) */
static void wg_edit(int press, int embedded)
{
    wg_masks_init(); wg_snap(); wg_snap_ok = 1;
    short tx[WG_MAX], ty[WG_MAX]; int tot;
    wg_targets(-1, -1, tx, ty, &tot);
    for (int i = 0; i < wg_n; i++) {
        struct witem *it = &wg_it[i];
        it->ax = tx[i]; it->ay = ty[i]; it->sc = it->al = 1; it->rm = 0;
        if (!wg_spr[it->type][it->size].px) wg_build(it->type, it->size, tx[i], WG_TOP + ty[i]);
    }
    wg_edit_on = 1; wg_press = -1;
    double t0 = now_ms(), tl = t0, exit_t = 0; wg_t0 = t0;
    float pa = embedded ? 1 : 0, sh0 = embedded ? WG_ESH : 0;
    wg_shift = sh0;
    int down = cur_down, tst = 0, target = -1, sx = cur_x, sy = cur_y, drag = -1, slot = -1, exiting = 0, scroll_to0 = 0;
    float gdx = 0, gdy = 0, s0 = 0, vel = 0; double lt = t0; int ly = sy;
    if (press >= 0 && down) { tst = 1; target = press; }
    static double prof_t; static int prof_n; static double prof_acc;
    for (;;) {
        double t = now_ms(); float dt = (float)(t - tl); if (dt > 50) dt = 50; tl = t;
        struct pkt p; int prev = down;
        while (read_pkt(&p, 0)) {}
        down = cur_down; int fx = cur_x, fy = cur_y;
        int topc = WG_TOP + (int)wg_shift - (int)wg_scroll;
        float maxs = tot + WG_TOP + WG_ESH + 10 - H; if (maxs < 0) maxs = 0;
        if (!exiting) {
            if (down && !prev) { sx = fx; sy = fy; tst = 1; target = wg_edit_hit(fx, fy, -1); vel = 0; s0 = wg_scroll; ly = fy; lt = t; }
            if (down && tst == 1 && (abs(fx - sx) > 8 || abs(fy - sy) > 8)) {
                if (sy >= 452 && sy - fy > 6) { exiting = 1; exit_t = t; }   /* gesto de inicio: salir */
                else if (target >= 0) {     /* arrastrar el widget */
                    drag = target; slot = target; tst = 2;
                    gdx = sx - wg_it[drag].ax; gdy = sy - (topc + wg_it[drag].ay);
                } else { tst = 3; s0 = wg_scroll; }
            }
            if (down && tst == 2) {
                struct witem *it = &wg_it[drag];
                if (fy > H - 44) wg_scroll += 0.3f * dt; else if (fy < 84) wg_scroll -= 0.3f * dt;
                if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > maxs) wg_scroll = maxs;
                topc = WG_TOP + (int)wg_shift - (int)wg_scroll;
                it->ax = fx - gdx; it->ay = fy - gdy - topc;
                /* hueco mas cercano al centro del widget arrastrado (con histeresis) */
                float best = 1e9f, cur = 1e9f; int bk = slot;
                for (int k = 0; k < wg_n; k++) {
                    short ax[WG_MAX], ay[WG_MAX]; wg_targets(drag, k, ax, ay, 0);
                    float ddx = ax[drag] - it->ax, ddy = ay[drag] - it->ay, dd = ddx * ddx + ddy * ddy;
                    if (dd < best) { best = dd; bk = k; } if (k == slot) cur = dd;
                }
                if (bk != slot && best + 300 < cur) slot = bk;
            }
            if (down && tst == 3) {
                if (t > lt) { vel = 0.6f * vel + 0.4f * (fy - ly) / (float)(t - lt); ly = fy; lt = t; }
                wg_scroll = s0 - (fy - sy); if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > maxs) wg_scroll = maxs;
            }
            if (!down && prev) {
                if (tst == 2) {            /* soltar: el orden nuevo */
                    int n = 0, ord[WG_MAX]; struct witem tmp[WG_MAX];
                    for (int i = 0; i < wg_n; i++) if (i != drag) ord[n++] = i;
                    int s = slot > n ? n : slot; memmove(&ord[s + 1], &ord[s], (n - s) * sizeof(int)); ord[s] = drag;
                    for (int k = 0; k <= n; k++) tmp[k] = wg_it[ord[k]];
                    memcpy(wg_it, tmp, (n + 1) * sizeof tmp[0]);
                    drag = -1; slot = -1;
                } else if (tst == 3) { if (t - lt > 60) vel = 0; }
                else if (tst == 1) {
                    if (target <= -100) { int k = -100 - target; if (k < wg_n && !wg_it[k].rm) { wg_it[k].rm = 1; wg_it[k].rm_t = t; } }
                    else if (target == -10) {
                        int gt, gs;
                        if (wg_gallery(&gt, &gs) && !wg_has(gt, gs) && wg_n < WG_MAX) {
                            memmove(&wg_it[1], &wg_it[0], wg_n * sizeof wg_it[0]); wg_n++;
                            struct witem *it = &wg_it[0]; memset(it, 0, sizeof *it); it->type = gt; it->size = gs;
                            it->ax = WG_M; it->ay = 0; it->sc = 0.5f; it->al = 0;
                            wg_targets(-1, -1, tx, ty, &tot); wg_build(gt, gs, tx[0], WG_TOP + ty[0]);
                            scroll_to0 = 1;
                        }
                        tl = now_ms(); down = cur_down;
                    }
                    else if (target == -11 || target == -1) { exiting = 1; exit_t = t; }
                }
                tst = 0;
            }
            if (!down && tst == 0 && fabsf(vel) > 0.03f) {   /* inercia */
                wg_scroll -= vel * dt; vel *= powf(0.9975f, dt);
                if (wg_scroll < 0) { wg_scroll = 0; vel = 0; } if (wg_scroll > maxs) { wg_scroll = maxs; vel = 0; }
            }
        }
        /* animacion: posiciones, quitar, aparecer, pildoras */
        wg_targets(drag, slot, tx, ty, &tot);
        float k = 1 - expf(-dt / 65.f);
        for (int i = 0; i < wg_n; i++) {
            struct witem *it = &wg_it[i];
            if (it->rm) { float e = (float)((t - it->rm_t) / 220); if (e > 1) e = 1; float q = (float)ease_out(e); it->sc = 1 - 0.75f * q; it->al = 1 - q; }
            else { it->sc += (1 - it->sc) * k; it->al += (1 - it->al) * k; if (fabsf(1 - it->sc) < 0.002f) it->sc = 1; if (it->al > 0.998f) it->al = 1; }
            if (i != drag) { it->ax += (tx[i] - it->ax) * k; it->ay += (ty[i] - it->ay) * k; if (fabsf(tx[i] - it->ax) < 0.3f) it->ax = tx[i]; if (fabsf(ty[i] - it->ay) < 0.3f) it->ay = ty[i]; }
        }
        for (int i = 0; i < wg_n; i++) if (wg_it[i].rm && t - wg_it[i].rm_t >= 220) {
            memmove(&wg_it[i], &wg_it[i + 1], (wg_n - i - 1) * sizeof wg_it[0]); wg_n--;
            if (drag > i) drag--; if (slot > i) slot--; i--;
        }
        if (scroll_to0) { wg_scroll += (0 - wg_scroll) * k; if (wg_scroll < 0.5f) { wg_scroll = 0; scroll_to0 = 0; } }
        if (exiting) {
            if (embedded) break;
            double e = ease_out((t - exit_t) / 240.0 > 1 ? 1 : (t - exit_t) / 240.0);
            pa = 1 - (float)e; wg_shift = WG_ESH * (1 - (float)e);
        } else if (!embedded) {
            double e = ease_out((t - t0) / 250.0 > 1 ? 1 : (t - t0) / 250.0);
            pa = (float)e; wg_shift = WG_ESH * (float)e;
        }
        double tr0 = now_ms();
        wg_edit_frame(fr0, t, pa, drag);
        prof_acc += now_ms() - tr0; prof_n++;
        present();
        if (now_ms() - prof_t > 3000) { if (prof_n > 20) lg("perfil widgets (edicion): %d fot, dibujo %.1f ms (fondo %.1f, giro %.1f)", prof_n, prof_acc / prof_n, wg_pf_bg / prof_n, wg_pf_bl / prof_n);
            wg_pf_bg = wg_pf_bl = 0; prof_t = now_ms(); prof_n = 0; prof_acc = 0; }
        if (exiting && t - exit_t >= 240) break;
        /* (el cristal de los widgets movidos se rehace al salir: rehacerlo aqui costaria fotogramas) */
    }
    wg_edit_on = 0; wg_shift = 0;
    { float mx = wg_content_h() + WG_TOP - H; if (mx < 0) mx = 0; if (wg_scroll > mx) wg_scroll = mx; }
    wg_free_unused();
    wg_cache_ok = 0;
    lconf_save();
}

/* ------------------------------------------------------------------ galeria de widgets */
#define GS_Y 40                              /* la hoja deja ver arriba la pagina oscurecida */
static uint16_t wg_th[WG_N][WG_TH * WG_TH]; static int wg_th_ok[WG_N];
static void wg_thumb(int type)
{
    wg_draw_scr(type, 0, WG_M, WG_TOP);
    for (int y = 0; y < WG_TH; y++) for (int x = 0; x < WG_TH; x++) {
        int xa = x * WG_S / WG_TH, xb = (x + 1) * WG_S / WG_TH, ya = y * WG_H / WG_TH, yb = (y + 1) * WG_H / WG_TH, r = 0, g = 0, b = 0, n = 0;
        for (int yy = ya; yy < yb; yy++) for (int xx = xa; xx < xb; xx++) { uint16_t q = wg_scr[yy * W + xx]; r += q >> 11; g += (q >> 5) & 63; b += q & 31; n++; }
        wg_th[type][y * WG_TH + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
    }
    wg_th_ok[type] = 1;
}
static uint8_t wg_corner[14 * 14];
static void wg_x(uint16_t *d, int cx, int cy, int a)     /* boton de cerrar: circulo gris con aspa */
{
    fill_circle(d, cx, cy, 15, rgb(58, 58, 62), a);
    for (int i = -5; i <= 5; i++) { fill_circle(d, cx + i, cy + i, 1.3f, rgb(200, 200, 206), a); fill_circle(d, cx + i, cy - i, 1.3f, rgb(200, 200, 206), a); }
}
static void wg_chev(uint16_t *d, int x, int cy, uint16_t col)
{
    for (int i = 0; i < 8; i++) { fill_rrect(d, x + i, cy - i, 3, 3, 1.2f, col, 255); fill_rrect(d, x + i, cy + i, 3, 3, 1.2f, col, 255); }
}
struct wg_gal { const uint16_t *bg; int top, view, sel, selsz, press, prev_ok; float ls, push; };
#define GROW 78
#define GLIST 64
static void wg_gal_list(uint16_t *d, const struct wg_gal *g, int ox)
{
    int top = g->top;
    wg_cy0 = top + GLIST - 4; wg_cy1 = H;
    for (int i = 0; i < WG_N; i++) {
        int ry = top + GLIST + i * GROW - (int)g->ls;
        if (ry + GROW < wg_cy0 || ry > H) continue;
        if (g->press == i) for (int y = ry; y < ry + GROW; y++) { if (y < wg_cy0 || y >= H) continue; for (int x = 0; x < W; x++) { int X = x + ox; if (X >= 0 && X < W) d[y * W + X] = rgb(44, 44, 47); } }
        if (wg_th_ok[i]) wg_blit_m(d, wg_th[i], WG_TH, wg_mask[2], WG_TH, WG_TH, 16 + ox, ry + 9, wg_cy0);
        else { for (int y = 0; y < WG_TH; y++) { int Y = ry + 9 + y; if (Y < wg_cy0 || Y >= H) continue; for (int x = 0; x < WG_TH; x++) { int a = wg_mask[2][y * WG_TH + x], X = 16 + ox + x; if (a && X >= 0 && X < W) d[Y * W + X] = mix565(rgb(58, 58, 62), d[Y * W + X], a); } } }
        wt(d, 90 + ox, ry + 31, tr(wg_name[i]), 16, 0xFFFF, W - 90 - 36);
        wt2(d, 90 + ox, ry + 50, tr(wg_desc[i]), 12, C_SEC, W - 90 - 36, 15, 0);
        wg_cy0 = top + GLIST - 4;
        if (ry + 39 >= wg_cy0) for (int k = 0; k < 6; k++) { int X = W - 24 + k - 2 + ox; for (int q = 0; q < 2; q++) { int y1 = ry + 33 + k, y2 = ry + 45 - k; if (X + q >= 0 && X + q < W) { if (y1 >= wg_cy0 && y1 < H) d[y1 * W + X + q] = rgb(90, 90, 96); if (y2 >= wg_cy0 && y2 < H) d[y2 * W + X + q] = rgb(90, 90, 96); } } }
        int sy = ry + GROW - 1; if (sy >= wg_cy0 && sy < H && i < WG_N - 1) for (int x = 90; x < W; x++) { int X = x + ox; if (X >= 0 && X < W) d[sy * W + X] = rgb(56, 56, 58); }
    }
    wg_cy0 = 0; wg_cy1 = H;
    wt(d, 16 + ox, top + 44, tr("Widgets"), 26, 0xFFFF, W - 80);
    if (ox > -W + 40) wg_x(d, W - 30 + ox, top + 30, 255);
}
static void wg_gal_detail(uint16_t *d, const struct wg_gal *g, int ox)
{
    int top = g->top, t = g->sel; if (t < 0) return;
    wg_cy0 = top; wg_cy1 = H;
    wg_chev(d, 14 + ox, top + 30, C_BLUE); draw_text(d, 28 + ox, top + 36, tr("Widgets"), 16, C_BLUE, 120, wg_cy0, H);
    wg_x(d, W - 30 + ox, top + 30, 255);
    wtc(d, W / 2 + ox, top + 80, tr(wg_name[t]), 22, 0xFFFF, W - 40);
    wt2(d, 24 + ox, top + 104, tr(wg_desc[t]), 13, C_SEC, W - 48, 17, 1);
    int w = wg_w(g->selsz), px = (W - w) / 2 + ox, py = top + 140;
    if (g->prev_ok) wg_blit_m(d, wg_scr, W, wg_mask[g->selsz], w, WG_H, px, py, top);
    /* tamano: control segmentado de iOS */
    int sw = 220, sx = (W - sw) / 2 + ox, sy = top + 296;
    fill_rrect(d, sx, sy, sw, 32, 9, rgb(52, 52, 56), 255);
    fill_rrect(d, sx + 2 + g->selsz * (sw / 2), sy + 2, sw / 2 - 4, 28, 7, rgb(99, 99, 102), 255);
    wtc(d, sx + sw / 4, sy + 21, tr("Pequeño"), 13, 0xFFFF, sw / 2 - 12); wtc(d, sx + 3 * sw / 4, sy + 21, tr("Mediano"), 13, 0xFFFF, sw / 2 - 12);
    int have = wg_has(t, g->selsz), by = top + 344;
    fill_rrect(d, 24 + ox, by, W - 48, 48, 24, have ? rgb(58, 58, 62) : C_BLUE, g->press == 100 ? 170 : 255);
    if (have) wtc(d, W / 2 + ox, by + 30, tr("Ya añadido"), 16, C_SEC, W - 80);
    else {
        const char *l = tr("Añadir widget"); int lw = tw_n(l, 16, -1); if (lw > W - 110) lw = W - 110; int x = (W - lw - 20) / 2 + ox;
        wg_plus(d, x + 6, by + 24, 255); draw_text(d, x + 20, by + 30, l, 16, 0xFFFF, W - 110, wg_cy0, H);
    }
    wg_cy0 = 0; wg_cy1 = H;
}
static void wg_gal_frame(const struct wg_gal *g, float open)
{
    int k = 32 - (int)(13 * open), top = g->top;
    for (int y = 0; y < top && y < H; y++) for (int x = 0; x < W; x++) fr0[y * W + x] = dim565(g->bg[y * W + x], k);
    if (top >= H) { present(); return; }
    /* hoja con esquinas superiores redondeadas */
    uint16_t sc = rgb(28, 28, 30);
    for (int y = top; y < H; y++) {
        uint16_t *row = fr0 + y * W; int r = y - top;
        for (int x = 0; x < W; x++) row[x] = sc;
        if (r < 14) for (int x = 0; x < 14; x++) { int a = wg_corner[r * 14 + x]; if (a < 255) {
            row[x] = mix565(sc, dim565(g->bg[y * W + x], k), a); row[W - 1 - x] = mix565(sc, dim565(g->bg[y * W + W - 1 - x], k), a); } }
    }
    fill_rrect(fr0, W / 2 - 18, top + 6, 36, 5, 2.5f, rgb(90, 90, 96), 255);
    int ox = -(int)(W * g->push);
    if (g->push < 1) wg_gal_list(fr0, g, ox);
    if (g->push > 0) wg_gal_detail(fr0, g, ox + W);
    present();
}
/* galeria de widgets (hoja de iOS): lista con miniaturas reales -> detalle con vista previa,
 * tamano y "Añadir widget". Devuelve 1 con el elegido */
static int wg_gallery(int *otype, int *osize)
{
    wg_masks_init();
    { uint8_t *m = wg_rrmask(28, 28, 14, 0); if (m) { for (int y = 0; y < 14; y++) memcpy(wg_corner + y * 14, m + y * 28, 14); free(m); } }
    memcpy(img_base, fr0, HM_FRAME_BYTES);
    struct wg_gal g = { img_base, H, 0, -1, 0, -1, 0, 0, 0 };
    for (int i = 0; i < WG_N; i++) wg_th_ok[i] = 0;
    float maxs = GLIST + WG_N * GROW - (H - GS_Y) + 10; if (maxs < 0) maxs = 0;
    int result = 0;
    /* subir la hoja; las miniaturas se dibujan una por fotograma mientras sube */
    double t0 = now_ms();
    for (;;) {
        double e = ease_out((now_ms() - t0) / 300.0 > 1 ? 1 : (now_ms() - t0) / 300.0);
        g.top = GS_Y + (int)((H - GS_Y) * (1 - e));
        wg_gal_frame(&g, (float)e);
        for (int i = 0; i < WG_N; i++) if (!wg_th_ok[i]) { wg_thumb(i); break; }
        if (e >= 1) break;
    }
    struct pkt p; float vel = 0; int redraw = 1;
    for (;;) {
        int built = 0;
        for (int i = 0; i < WG_N && g.view == 0; i++) if (!wg_th_ok[i]) { wg_thumb(i); built = 1; break; }
        if (built || redraw) { wg_gal_frame(&g, 1); redraw = 0; }
        if (!read_pkt(&p, built ? 0 : 300) || !p.down) continue;
        redraw = 1;
        int sx = p.x, sy = p.y, moved = 0, drag_down = 0; float s0 = g.ls; double lt = now_ms(); int ly = sy;
        if (g.view == 0 && sy >= GS_Y + GLIST) g.press = (sy - GS_Y - GLIST + (int)g.ls) / GROW;
        if (g.view == 1 && sy >= GS_Y + 344 && sy < GS_Y + 392 && abs(sx - W / 2) < W / 2 - 24 && !wg_has(g.sel, g.selsz)) g.press = 100;
        if (g.press >= 0) wg_gal_frame(&g, 1);
        while (cur_down && read_pkt(&p, 40)) {
            int dx = cur_x - sx, dy = cur_y - sy;
            if (!moved && (abs(dx) > 8 || abs(dy) > 8)) { moved = 1; g.press = -1; drag_down = sy < GS_Y + 56 && dy > 0; }
            if (!moved) continue;
            if (drag_down) { g.top = GS_Y + (dy > 0 ? dy : 0); wg_gal_frame(&g, 1 - (g.top - GS_Y) / (float)(H - GS_Y)); continue; }
            if (g.view == 0) {
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                g.ls = s0 - dy; if (g.ls < 0) g.ls = 0; if (g.ls > maxs) g.ls = maxs; wg_gal_frame(&g, 1);
            } else if (sx < 24 && dx > 0) { g.push = 1 - dx / (float)W; if (g.push < 0) g.push = 0; wg_gal_frame(&g, 1); }
        }
        int dx = cur_x - sx, dy = cur_y - sy;
        if (drag_down) {
            if (dy > 90) { result = 0; break; }
            float from = g.top; double tb = now_ms();
            for (;;) { double e = ease_out((now_ms() - tb) / 200.0 > 1 ? 1 : (now_ms() - tb) / 200.0); g.top = (int)(from + (GS_Y - from) * e); wg_gal_frame(&g, 1); if (e >= 1) break; }
            continue;
        }
        if (moved) {
            if (g.view == 0) {   /* inercia */
                if (now_ms() - lt > 60) vel = 0;
                double tp = now_ms();
                while (fabsf(vel) > 0.03f) {
                    double t = now_ms(); float ddt = (float)(t - tp); tp = t; if (ddt > 40) ddt = 40;
                    g.ls -= vel * ddt; vel *= powf(0.9975f, ddt);
                    if (g.ls < 0) { g.ls = 0; vel = 0; } if (g.ls > maxs) { g.ls = maxs; vel = 0; }
                    wg_gal_frame(&g, 1);
                    struct pkt q; if (read_pkt(&q, 0) && q.down) { vel = 0; break; }
                }
                vel = 0;
            } else if (sx < 24) {   /* atras deslizando desde el borde */
                float from = g.push, to = dx > 60 ? 0 : 1; double tb = now_ms();
                for (;;) { double e = ease_out((now_ms() - tb) / 220.0 > 1 ? 1 : (now_ms() - tb) / 220.0); g.push = from + (to - from) * (float)e; wg_gal_frame(&g, 1); if (e >= 1) break; }
                if (to == 0) g.view = 0;
            } else if (abs(dx) > 40 && abs(dx) > abs(dy)) {   /* deslizar la vista previa: otro tamano */
                int ns = dx < 0 ? 1 : 0; if (ns != g.selsz) { g.selsz = ns; wg_draw_scr(g.sel, ns, (W - wg_w(ns)) / 2, GS_Y + 140); }
            }
            continue;
        }
        int pr = g.press; g.press = -1;
        if (sy >= GS_Y + 12 && sy < GS_Y + 50 && sx > W - 52) { result = 0; break; }   /* cerrar */
        if (g.view == 0) {
            if (pr >= 0 && pr < WG_N) {
                g.sel = pr; g.selsz = wg_has(pr, 0) && !wg_has(pr, 1) ? 1 : 0;
                wg_draw_scr(g.sel, g.selsz, (W - wg_w(g.selsz)) / 2, GS_Y + 140); g.prev_ok = 1;
                double tb = now_ms(); g.view = 1;
                for (;;) { double e = ease_out((now_ms() - tb) / 300.0 > 1 ? 1 : (now_ms() - tb) / 300.0); g.push = (float)e; wg_gal_frame(&g, 1); if (e >= 1) break; }
            }
        } else {
            if (sy < GS_Y + 56 && sx < 130) {
                double tb = now_ms();
                for (;;) { double e = ease_out((now_ms() - tb) / 280.0 > 1 ? 1 : (now_ms() - tb) / 280.0); g.push = 1 - (float)e; wg_gal_frame(&g, 1); if (e >= 1) break; }
                g.view = 0;
            } else if (sy >= GS_Y + 296 && sy < GS_Y + 328 && abs(sx - W / 2) < 110) {
                int ns = sx >= W / 2; if (ns != g.selsz) { g.selsz = ns; wg_draw_scr(g.sel, ns, (W - wg_w(ns)) / 2, GS_Y + 140); }
            } else if (pr == 100) { *otype = g.sel; *osize = g.selsz; result = 1; break; }
        }
    }
    /* bajar la hoja */
    float from = g.top; double tb = now_ms();
    for (;;) { double e = ease_out((now_ms() - tb) / 260.0 > 1 ? 1 : (now_ms() - tb) / 260.0); g.top = (int)(from + (H - from) * e); wg_gal_frame(&g, 1 - (float)e); if (e >= 1) break; }
    drain_until_up();
    have_under = 0;
    return result;
}

/* ------------------------------------------------------------------ ajustes */
static int wg_on[WG_N];
static void apply_widgets(void)
{
    for (int t = 0; t < WG_N; t++) {
        if (wg_on[t] && !wg_has_type(t) && wg_n < WG_MAX) { memset(&wg_it[wg_n], 0, sizeof wg_it[0]); wg_it[wg_n].type = t; wg_it[wg_n++].size = wg_defsz[t]; }
        if (!wg_on[t]) for (int i = 0; i < wg_n; i++) if (wg_it[i].type == t) { memmove(&wg_it[i], &wg_it[i + 1], (wg_n - i - 1) * sizeof wg_it[0]); wg_n--; i--; }
    }
    wg_free_unused(); wg_cache_ok = 0; home_prev_ok = 0; lconf_save();
}
static void page_widgets(struct spage *p)
{
    *p = (struct spage){ "Widgets" };
    p->secs[0] = (struct ssec){ "WIDGETS", "Mantén pulsado un widget para moverlo o quitarlo, como las apps.", {
        ROW("Baterías", R_TOGGLE, &wg_on[WG_BATT], 0, 0, apply_widgets, 0, 0), ROW("Reproduciendo", R_TOGGLE, &wg_on[WG_NOW], 0, 0, apply_widgets, 0, 0),
        ROW("Fecha", R_TOGGLE, &wg_on[WG_DATE], 0, 0, apply_widgets, 0, 0), ROW("Almacenamiento", R_TOGGLE, &wg_on[WG_STORAGE], 0, 0, apply_widgets, 0, 0),
        ROW("Energía", R_TOGGLE, &wg_on[WG_POWER], 0, 0, apply_widgets, 0, 0), ROW("Biblioteca", R_TOGGLE, &wg_on[WG_LIB], 0, 0, apply_widgets, 0, 0),
        ROW("Recién añadido", R_TOGGLE, &wg_on[WG_NEW], 0, 0, apply_widgets, 0, 0), ROW("Escuchas", R_TOGGLE, &wg_on[WG_LISTEN], 0, 0, apply_widgets, 0, 0) }, 8 };
    p->nsec = 1;
}
static void nav_wg_list(void)
{
    for (int t = 0; t < WG_N; t++) wg_on[t] = wg_has_type(t);
    open_page(page_widgets, "Inicio");
}
static const char *v_wg_count(void) { static char b[8]; snprintf(b, sizeof b, "%d", wg_n); return b; }
/* "Editar widgets" desde Ajustes: la pagina de widgets en edicion entra como una pagina mas */
static void wg_edit_from_settings(void)
{
    memcpy(img_app, fr0, HM_FRAME_BYTES);            /* la pagina de ajustes (para volver) */
    wg_masks_init(); wg_snap(); wg_snap_ok = 1;
    short tx[WG_MAX], ty[WG_MAX];
    float sv = wg_scroll; wg_scroll = 0;
    wg_targets(-1, -1, tx, ty, 0);
    for (int i = 0; i < wg_n; i++) {
        struct witem *it = &wg_it[i]; it->ax = tx[i]; it->ay = ty[i]; it->sc = it->al = 1; it->rm = 0;
        if (!wg_spr[it->type][it->size].px || wg_spr[it->type][it->size].sig != wg_sig(it->type, it->size, tx[i], WG_TOP + ty[i])) wg_build(it->type, it->size, tx[i], WG_TOP + ty[i]);
    }
    wg_shift = WG_ESH; wg_t0 = now_ms();
    wg_edit_frame(img_base, wg_t0, 1, -1);
    fs_transition(img_app, img_base, 1, 0);
    drain_until_up();
    wg_edit(-1, 1);
    memcpy(img_base, fr0, HM_FRAME_BYTES);
    fs_transition(img_base, img_app, -1, 0);
    drain_until_up();
    (void)sv; wg_scroll = 0; home_prev_ok = 0;
}

static void animate_scroll_to(int page);
static void *wg_pc_th(void *a) { long k = (long)a; if (k == 0) pc_prev(); else if (k == 1) pc_toggle(); else pc_next(); return 0; }
/* toque con la pagina de widgets en pantalla (desde launcher_touch) */
static void widgets_touch(struct pkt *p)
{
    int sx = p->x, sy = p->y, moved = 0, horiz = 0; float s0 = wg_scroll, vel = 0, vy = 0; double t0 = now_ms(), lt = t0; int lx = sx, ly = sy;
    int btn, k = wg_hit(sx, sy, &btn);
    if (k >= 0) { wg_press = k; wg_press_btn = btn; launcher_present(); }
    for (;;) {
        int got = read_pkt(p, 30); double t = now_ms();
        if ((got && !p->down) || !cur_down) break;
        int dx = cur_x - sx, dy = cur_y - sy;
        if (!moved && (abs(dx) > 10 || abs(dy) > 10)) { moved = 1; horiz = abs(dx) > abs(dy); if (wg_press >= 0) { wg_press = -1; launcher_present(); } }
        if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_x - lx) / (float)(t - lt); vy = 0.6f * vy + 0.4f * (cur_y - ly) / (float)(t - lt); lx = cur_x; ly = cur_y; lt = t; }
        if (!moved && t - t0 > 500 && k != -2) { wg_press = -1; wg_edit(k, 0); launcher_present(); return; }   /* pulsacion larga: editar */
        if (!got) continue;
        if (moved && horiz) { float s = -W - dx; if (s > 0) s = 0; if (s < -W - 40) s = -W - 40 + (s + W + 40) / 3; V.scroll = s; launcher_present(); }
        else if (moved) { float mx = wg_content_h() + WG_TOP - H; if (mx < 0) mx = 0; wg_scrolling = 1; wg_scroll = s0 - dy; if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > mx) wg_scroll = mx; launcher_present(); }
    }
    if (moved && horiz) {
        wg_scrolling = 0;
        if (cur_x - sx < -70 || vel < -0.35f) { widgets_on = 0; animate_scroll_to(0); }
        else { animate_scroll_to(-1); }
        return;
    }
    if (moved) {   /* inercia de iOS */
        float mx = wg_content_h() + WG_TOP - H; if (mx < 0) mx = 0;
        if (now_ms() - lt > 60) vy = 0;
        double tp = now_ms();
        while (fabsf(vy) > 0.03f) {
            double t = now_ms(); float dt = (float)(t - tp); tp = t; if (dt > 40) dt = 40;
            wg_scroll -= vy * dt; vy *= powf(0.9975f, dt);
            if (wg_scroll < 0) { wg_scroll = 0; vy = 0; } if (wg_scroll > mx) { wg_scroll = mx; vy = 0; }
            launcher_present();
            struct pkt q; if (read_pkt(&q, 0) && q.down) break;
        }
        wg_scrolling = 0; launcher_present();
        return;
    }
    wg_press = -1;
    if (k == -2) { wg_edit(-1, 0); launcher_present(); return; }
    if (k < 0) { launcher_present(); return; }
    int type = wg_it[k].type;
    if (type == WG_NOW && btn >= 0) {
        if (btn == 1) { wg_np_force = !wg_playing(); wg_np_until = now_ms() + 2500; }
        pthread_t th; if (!pthread_create(&th, 0, wg_pc_th, (void *)(long)btn)) pthread_detach(th);
        wg_fresh = 1; launcher_present(); return;
    }
    short xs[WG_MAX], ys[WG_MAX]; wg_layout_items(xs, ys);
    int ix = xs[k] + 20, iy = WG_TOP - (int)wg_scroll + ys[k] + 20;
    const char *id = type == WG_NOW ? "nowplaying" : type == WG_STORAGE ? "files" : type == WG_LIB || type == WG_NEW || type == WG_LISTEN ? "music" : 0;
    int a = id ? app_by_id(id) : -1;
    if (a < 0) { launcher_present(); return; }
    if (apps[a].act == ACT_FILES) { launch_app(a, ix, iy); return; }      /* apps propias: se vuelve a los widgets */
    widgets_on = 0; V.scroll = cur_page * W; launch_app(a, ix, iy);
}
