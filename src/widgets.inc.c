/* widgets.inc.c - pagina de widgets a la izquierda del inicio (como "Hoy" en iOS).
 *
 * Desde la primera pagina del inicio, deslizar hacia la derecha. Fondo = el del inicio;
 * tarjetas de cristal oscuro con:
 *   Baterias  - el HiBy (%, cargando, Solo USB) y dispositivos Bluetooth conectados con su
 *               bateria (BlueZ: org.bluez.Battery1, por dbus-send)
 *   Reproduciendo, Fecha, Almacenamiento (SD e interna), Energia (modo y estado),
 *   Biblioteca, Recien anadido y Escuchas (hibymix stats sobre la base de HiBy, solo lectura)
 * Configurables en Personalizar > Widgets (linea "widgets" de launcher.conf). Los datos se
 * leen al entrar y cada 10 s mientras la pagina esta visible; nunca en segundo plano. */

enum { WG_BATT, WG_NOW, WG_DATE, WG_STORAGE, WG_POWER, WG_LIB, WG_NEW, WG_LISTEN, WG_N };
static const char *const wg_id[WG_N] = { "battery", "nowplaying", "date", "storage", "power", "library", "recent", "listening" };
static const char *const wg_name[WG_N] = { "Baterías", "Reproduciendo", "Fecha", "Almacenamiento", "Energía", "Biblioteca", "Recién añadido", "Escuchas" };
static int wg_list[WG_N] = { WG_BATT, WG_DATE, WG_NOW, WG_LISTEN, WG_NEW, WG_LIB, WG_STORAGE, WG_POWER }, wg_n = WG_N;
static int widgets_on;                       /* la pagina de widgets esta en pantalla */

static void wg_conf_line(char *rest)
{
    wg_n = 0; char *t;
    while ((t = strtok(rest, " \t\r\n"))) { rest = 0;
        for (int i = 0; i < WG_N; i++) if (!strcmp(t, wg_id[i])) { int dup = 0; for (int k = 0; k < wg_n; k++) dup |= wg_list[k] == i; if (!dup) wg_list[wg_n++] = i; } }
}
static void wg_conf_save(FILE *f) { fprintf(f, "widgets"); for (int i = 0; i < wg_n; i++) fprintf(f, " %s", wg_id[wg_list[i]]); fprintf(f, "\n"); }

/* ---- datos ---- */
struct wg_bt { char name[40]; int pct; };
static struct { int batt, charging, usb; struct wg_bt bt[3]; int nbt; long long sd_free, sd_tot, in_free, in_tot; double t;
                int songs, albums, artists, favs; long long lib_bytes; char new_artist[64], new_album[96], top_artist[64], last_title[96], last_artist[64]; int top_n; } WD;
static void wg_bt_scan(void)
{
    WD.nbt = 0;
    char *argv[] = { "/usr/bin/dbus-send", "--system", "--print-reply", "--dest=org.bluez", "/", "org.freedesktop.DBus.ObjectManager.GetManagedObjects", 0 };
    static char out[24000];
    if (access(argv[0], X_OK) || run_capture(argv, out, sizeof out) <= 0) return;
    /* por cada objeto dev_: Alias, Connected y Percentage */
    char alias[40] = ""; int conn = 0, pct = -1, indev = 0, want = 0;   /* 1 alias, 2 conectado, 3 bateria */
    for (char *l = strtok(out, "\n"); l; l = strtok(0, "\n")) {
        char *op = strstr(l, "object path \"");
        if (op) {
            if (indev && conn && WD.nbt < 3) { snprintf(WD.bt[WD.nbt].name, 40, "%s", alias[0] ? alias : "Bluetooth"); WD.bt[WD.nbt++].pct = pct; }
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
    if (indev && conn && WD.nbt < 3) { snprintf(WD.bt[WD.nbt].name, 40, "%s", alias[0] ? alias : "Bluetooth"); WD.bt[WD.nbt++].pct = pct; }
}
static volatile int wg_busy, wg_fresh;     /* hilo de datos en marcha / datos nuevos sin dibujar */
static int wg_cache_ok;
static void wg_refresh_now(void)
{
    WD.batt = sysfs_get("/sys/class/power_supply/battery/capacity", -1);
    char st[24] = ""; int f = open("/sys/class/power_supply/battery/status", O_RDONLY);
    if (f >= 0) { int n = read(f, st, sizeof st - 1); if (n > 0) st[n] = 0; close(f); }
    WD.charging = !strncmp(st, "Charging", 8); WD.usb = sysfs_get(USB_ONLINE, 0);
    struct statvfs v;
    if (!statvfs("/usr/data/mnt/sd_0", &v)) { WD.sd_free = (long long)v.f_bavail * v.f_frsize; WD.sd_tot = (long long)v.f_blocks * v.f_frsize; } else WD.sd_tot = 0;
    if (!statvfs("/usr/data", &v)) { WD.in_free = (long long)v.f_bavail * v.f_frsize; WD.in_tot = (long long)v.f_blocks * v.f_frsize; }
    wg_bt_scan();
    {   /* biblioteca de HiBy (hibymix stats: solo lectura) */
        char *argv[] = { "/usr/data/hibymod/shell/hibymix", "stats", 0 };
        static char out[2048];
        if (!access(argv[0], X_OK) && run_capture(argv, out, sizeof out) > 0)
            for (char *sv = 0, *l = strtok_r(out, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
                char *f[6]; int n = split_tabs(l, f, 6);
                if (!strcmp(f[0], "count") && n >= 6) { WD.songs = atoi(f[1]); WD.albums = atoi(f[2]); WD.artists = atoi(f[3]); WD.favs = atoi(f[4]); WD.lib_bytes = atoll(f[5]); }
                else if (!strcmp(f[0], "recent") && n >= 3) { snprintf(WD.new_artist, sizeof WD.new_artist, "%s", f[1]); snprintf(WD.new_album, sizeof WD.new_album, "%s", f[2]); }
                else if (!strcmp(f[0], "top") && n >= 3) { snprintf(WD.top_artist, sizeof WD.top_artist, "%s", f[1]); WD.top_n = atoi(f[2]); }
                else if (!strcmp(f[0], "last") && n >= 3) { snprintf(WD.last_title, sizeof WD.last_title, "%s", f[1]); snprintf(WD.last_artist, sizeof WD.last_artist, "%s", f[2]); }
            }
    }
    WD.t = now_ms();
}
static void *wg_thread(void *a) { (void)a; wg_refresh_now(); wg_fresh = 1; wg_busy = 0; return 0; }
static void wg_refresh(void)            /* asincrono */
{
    if (wg_busy) return;
    wg_busy = 1; WD.t = now_ms();
    pthread_t th; if (pthread_create(&th, 0, wg_thread, 0)) { wg_busy = 0; return; } pthread_detach(th);
}

/* ---- dibujo ---- */
static uint16_t *wg_bg; static char wg_bg_for[200]; static int wg_bg_ok;
static void cc_blur(const uint16_t *src, uint16_t *dst);
static void cc_rrect(uint16_t *d, int x0, int y0, int w, int h, float r, uint16_t col, int alpha);
static const uint16_t *wg_background(void)
{
    return wall;                         /* el mismo fondo que el inicio */
    if (!wg_bg) wg_bg = malloc(HM_FRAME_BYTES);
    if (!wg_bg) return wall;
    if (!wg_bg_ok || strcmp(wg_bg_for, L.wall)) { cc_blur(wall, wg_bg); snprintf(wg_bg_for, sizeof wg_bg_for, "%s", L.wall); wg_bg_ok = 1; }
    return wg_bg;
}
static void wg_text(uint16_t *d, int x, int y, const char *t, int size, uint16_t col, int maxw)
{
    int sz = size; while (sz > 11 && tw_n(t, sz, -1) > maxw) sz--;
    draw_text(d, x, y, t, sz, col, maxw, 30, H);
}
static int wg_height(int w) { return w == WG_BATT ? 58 + 40 * (1 + (WD.nbt ? WD.nbt : 0)) + (WD.nbt ? 0 : 20) : w == WG_DATE ? 96 : w == WG_STORAGE ? 126 : w == WG_LIB ? 104 : w == WG_LISTEN ? 116 : w == WG_NEW ? 92 : 84; }
static void wg_ring(uint16_t *d, int cx, int cy, float r, float frac, uint16_t col)
{
    /* anillo de progreso (como el widget de baterias de iOS): fondo tenue y arco del nivel */
    for (int y = (int)(cy - r - 3); y <= cy + r + 3; y++) for (int x = (int)(cx - r - 3); x <= cx + r + 3; x++) {
        if (x < 0 || x >= W || y < 30 || y >= H) continue;
        float dx = x + .5f - cx, dy = y + .5f - cy, dd = sqrtf(dx * dx + dy * dy), e = fabsf(dd - r);
        if (e > 2.6f) continue;
        float cov = e < 1.6f ? 1 : (2.6f - e);
        float ang = atan2f(dx, -dy); if (ang < 0) ang += 6.2832f;
        int on = ang <= frac * 6.2832f;
        uint16_t c = on ? col : rgb(90, 90, 96);
        uint16_t *p = d + y * W + x; int k = (int)(cov * (on ? 32 : 18));
        int rr = ((*p >> 11) * (32 - k) + (c >> 11) * k) >> 5, g = (((*p >> 5) & 63) * (32 - k) + ((c >> 5) & 63) * k) >> 5, b = ((*p & 31) * (32 - k) + (c & 31) * k) >> 5;
        *p = (uint16_t)(rr << 11 | g << 5 | b);
    }
}
static const char *wg_size(long long b, char *o, int n)
{
    if (b >= 1LL << 30) snprintf(o, n, "%.1f GB", b / 1073741824.0); else snprintf(o, n, "%.0f MB", b / 1048576.0);
    return o;
}
static void wg_bar(uint16_t *d, int x, int y, int w, float frac, uint16_t col)
{
    cc_rrect(d, x, y, w, 8, 4, rgb(255, 255, 255), 40);
    int fw = (int)(w * frac); if (fw > 8) fill_rrect(d, x, y, fw, 8, 4, col, 255);
}
static int wg_draw_one(uint16_t *d, int ox, int y, int w)
{
    int x = ox + 14, cw = W - 28, h = wg_height(w), tx = x + 16, tw = cw - 32;
    if (y > H || y + h < 30) return h;
    cc_rrect(d, x, y, cw, h, 20, rgb(16, 16, 20), 120);           /* cristal oscuro */
    cc_rrect(d, x, y, cw, h, 20, rgb(255, 255, 255), 22);
    char b[160], s1[24], s2[24];
    uint16_t sub = rgb(200, 200, 206), head = rgb(235, 235, 240);
    switch (w) {
    case WG_BATT: {
        wg_text(d, tx, y + 26, tr("Baterías"), 13, head, tw);
        int ry = y + 40;
        uint16_t col = WD.batt >= 0 && WD.batt <= 20 ? rgb(255, 69, 58) : (WD.charging ? rgb(48, 209, 88) : 0xFFFF);
        wg_ring(d, x + 32, ry + 18, 13, WD.batt < 0 ? 0 : WD.batt / 100.f, col);
        snprintf(b, sizeof b, "%d %%", WD.batt < 0 ? 0 : WD.batt); int pw = tw_n(b, 17, -1);
        draw_text(d, x + cw - 16 - pw, ry + 24, b, 17, 0xFFFF, 0, 30, H);
        wg_text(d, x + 56, ry + 17, "HiBy R3 II", 15, 0xFFFF, cw - 56 - pw - 24);
        const char *st = pw_nocharge && WD.usb ? tr("Solo USB: la batería no se carga") : WD.charging ? tr("Cargando") : WD.usb ? tr("Conectado por USB") : tr("Con batería");
        wg_text(d, x + 56, ry + 34, st, 12, sub, cw - 56 - pw - 24);
        for (int i = 0; i < WD.nbt; i++) {
            ry += 40;
            wg_ring(d, x + 32, ry + 18, 13, WD.bt[i].pct < 0 ? 0 : WD.bt[i].pct / 100.f, 0xFFFF);
            if (WD.bt[i].pct >= 0) snprintf(b, sizeof b, "%d %%", WD.bt[i].pct); else snprintf(b, sizeof b, "—");
            pw = tw_n(b, 17, -1); draw_text(d, x + cw - 16 - pw, ry + 24, b, 17, 0xFFFF, 0, 30, H);
            wg_text(d, x + 56, ry + 17, WD.bt[i].name, 15, 0xFFFF, cw - 56 - pw - 24);
            wg_text(d, x + 56, ry + 34, tr("Bluetooth"), 12, sub, cw - 56 - pw - 24);
        }
        if (!WD.nbt) wg_text(d, tx, ry + 66, tr("Sin dispositivos Bluetooth conectados"), 12, sub, tw);
        break; }
    case WG_NOW: {
        static uint16_t cov[128 * 128]; struct np_info ni; int cok = 0;
        npx_snapshot(&ni, cov, &cok);
        if (cok) npx_blit_cover(d, cov, x + 14, y + 16, 52, 10);
        else { int a = app_by_id("nowplaying"); if (a >= 0) blit_scaled(d, &apps[a].icon[2], x + 14, y + 16, 52, 52, 255); }
        wg_text(d, x + 80, y + 30, tr(ni.playing ? "Sonando" : "En pausa"), 11, rgb(160, 160, 166), cw - 96);
        wg_text(d, x + 80, y + 50, ni.source && ni.title[0] ? ni.title : tr("Reproduciendo"), 15, 0xFFFF, cw - 96);
        wg_text(d, x + 80, y + 68, ni.source ? ni.artist : "", 13, sub, cw - 96);
        break; }
    case WG_DATE: {
        time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
        static const char *const dn[7] = { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" };
        static const char *const mn[12] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
        wg_text(d, tx, y + 28, tr(dn[tm.tm_wday]), 14, rgb(255, 99, 88), tw - 90);
        snprintf(b, sizeof b, "%d", tm.tm_mday); int dw = tw_n(b, 40, -1); draw_text(d, tx, y + 76, b, 40, 0xFFFF, 0, 30, H);
        snprintf(b, sizeof b, "%s %d", tr(mn[tm.tm_mon]), 1900 + tm.tm_year); wg_text(d, tx + dw + 12, y + 72, b, 15, head, tw - dw - 12);
        snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min); draw_text(d, x + cw - 16 - tw_n(b, 22, -1), y + 40, b, 22, 0xFFFF, 0, 30, H);
        break; }
    case WG_STORAGE: {
        wg_text(d, tx, y + 26, tr("Almacenamiento"), 13, head, tw);
        if (WD.sd_tot) {
            wg_text(d, tx, y + 50, tr("Tarjeta SD"), 14, 0xFFFF, tw / 2);
            snprintf(b, sizeof b, "%s / %s", wg_size(WD.sd_free, s1, 24), wg_size(WD.sd_tot, s2, 24)); int vw = tw_n(b, 12, -1);
            draw_text(d, x + cw - 16 - vw, y + 50, b, 12, sub, 0, 30, H);
            wg_bar(d, tx, y + 58, tw, 1 - (float)WD.sd_free / WD.sd_tot, rgb(10, 132, 255));
        } else wg_text(d, tx, y + 50, tr("Sin tarjeta SD"), 14, sub, tw);
        wg_text(d, tx, y + 92, tr("En el HiBy"), 14, 0xFFFF, tw / 2);
        snprintf(b, sizeof b, "%s / %s", wg_size(WD.in_free, s1, 24), wg_size(WD.in_tot, s2, 24)); int vw = tw_n(b, 12, -1);
        draw_text(d, x + cw - 16 - vw, y + 92, b, 12, sub, 0, 30, H);
        if (WD.in_tot) wg_bar(d, tx, y + 100, tw, 1 - (float)WD.in_free / WD.in_tot, rgb(191, 90, 242));
        wg_text(d, tx, y + 121, tr("libres / total"), 11, rgb(160, 160, 166), tw);
        break; }
    case WG_POWER: {
        wg_text(d, tx, y + 26, tr("Energía"), 13, head, tw);
        snprintf(b, sizeof b, "%s %s · %d Hz", tr("Modo"), tr(pw_name(pw_mode)), lcd_hz);
        wg_text(d, tx, y + 52, b, 17, pw_mode == PW_SAVE ? rgb(255, 214, 10) : pw_mode == PW_PERF ? rgb(255, 99, 88) : 0xFFFF, tw);
        wg_text(d, tx, y + 72, tr(pw_nocharge ? "Solo USB: la batería no se carga" : "Carga normal"), 12, sub, tw);
        break; }
    case WG_LIB: {
        wg_text(d, tx, y + 26, tr("Biblioteca"), 13, head, tw);
        snprintf(b, sizeof b, "%d", WD.songs); int nw = tw_n(b, 30, -1); draw_text(d, tx, y + 64, b, 30, 0xFFFF, 0, 30, H);
        wg_text(d, tx + nw + 8, y + 62, tr("canciones"), 14, head, tw - nw - 8);
        snprintf(b, sizeof b, "%d %s · %d %s · %s", WD.albums, tr("álbumes"), WD.artists, tr("artistas"), wg_size(WD.lib_bytes, s1, 24));
        wg_text(d, tx, y + 88, b, 13, sub, tw);
        break; }
    case WG_NEW: {
        wg_text(d, tx, y + 26, tr("Recién añadido"), 13, head, tw);
        wg_text(d, tx, y + 52, WD.new_album[0] ? WD.new_album : "—", 16, 0xFFFF, tw);
        wg_text(d, tx, y + 72, WD.new_artist, 13, sub, tw);
        break; }
    case WG_LISTEN: {
        wg_text(d, tx, y + 26, tr("Escuchas"), 13, head, tw);
        wg_text(d, tx, y + 48, tr("Lo último"), 11, rgb(160, 160, 166), tw);
        snprintf(b, sizeof b, "%s — %s", WD.last_title[0] ? WD.last_title : "—", WD.last_artist);
        wg_text(d, tx, y + 66, b, 14, 0xFFFF, tw);
        wg_text(d, tx, y + 86, tr("Artista más escuchado"), 11, rgb(160, 160, 166), tw);
        if (WD.top_n) snprintf(b, sizeof b, "%s · %d %s", WD.top_artist, WD.top_n, tr("escuchas")); else snprintf(b, sizeof b, "—");
        wg_text(d, tx, y + 104, b, 14, 0xFFFF, tw);
        break; }
    }
    return h;
}
static float wg_scroll, wg_cache_scroll; static int wg_cache_min;
static uint16_t *wg_cache;
static void wg_compose(void)
{
    if (!wg_cache) wg_cache = malloc(HM_FRAME_BYTES);
    if (!wg_cache) return;
    memcpy(wg_cache + 30 * W, wg_background() + 30 * W, (H - 30) * W * 2);
    int y = 44 - (int)wg_scroll;
    for (int i = 0; i < wg_n; i++) y += wg_draw_one(wg_cache, 0, y, wg_list[i]) + 12;
    int bw = tw_n(tr("Editar"), 14, -1) + 30;         /* boton Editar (al final, como iOS) */
    if (y < H - 4) { cc_rrect(wg_cache, (W - bw) / 2, y + 2, bw, 30, 15, rgb(255, 255, 255), 50); draw_text(wg_cache, (W - bw) / 2 + 15, y + 22, tr("Editar"), 14, 0xFFFF, 0, 30, H); }
    time_t now = time(0); wg_cache_min = (int)(now / 60); wg_cache_scroll = wg_scroll; wg_cache_ok = 1;
}
/* dibuja la pagina con su borde izquierdo en ox (ox = -W .. 0) */
static void widgets_draw(uint16_t *d, int ox)
{
    if (ox <= -W || ox >= W) return;
    if (!WD.t || now_ms() - WD.t > 10000) wg_refresh();
    if (npx_changed && widgets_on) { npx_changed = 0; wg_fresh = 1; }
    if (!wg_cache_ok || wg_fresh || wg_cache_scroll != wg_scroll || (int)(time(0) / 60) != wg_cache_min) { wg_fresh = 0; wg_compose(); }
    if (!wg_cache) return;
    int x0 = ox < 0 ? 0 : ox, x1 = ox + W > W ? W : ox + W;
    if (x1 > x0) for (int y = 30; y < H; y++) memcpy(d + y * W + x0, wg_cache + y * W + (x0 - ox), (x1 - x0) * 2);
}
static int wg_edit_y(void) { int y = 44 - (int)wg_scroll; for (int i = 0; i < wg_n; i++) y += wg_height(wg_list[i]) + 12; return y; }
static int wg_at(int py)             /* indice del widget en y, -2 = Editar, -1 nada */
{
    int y = 44 - (int)wg_scroll;
    for (int i = 0; i < wg_n; i++) { int h = wg_height(wg_list[i]); if (py >= y && py < y + h) return i; y += h + 12; }
    if (py >= y && py < y + 34) return -2;
    return -1;
}

/* ---- ajustes (Personalizar > Widgets, y "Editar" en la propia pagina) ---- */
static int wg_on[WG_N];
static void apply_widgets(void)
{
    int keep[WG_N], n = 0;
    for (int i = 0; i < wg_n; i++) if (wg_on[wg_list[i]]) keep[n++] = wg_list[i];
    for (int t = 0; t < WG_N; t++) if (wg_on[t]) { int have = 0; for (int i = 0; i < n; i++) have |= keep[i] == t; if (!have) keep[n++] = t; }
    memcpy(wg_list, keep, sizeof keep); wg_n = n; lconf_save();
}
static void page_widgets(struct spage *p)
{
    *p = (struct spage){ "Widgets" };
    p->secs[0] = (struct ssec){ "WIDGETS", "WIDGETS_HELP", {
        ROW("Baterías", R_TOGGLE, &wg_on[WG_BATT], 0, 0, apply_widgets, 0, 0), ROW("Reproduciendo", R_TOGGLE, &wg_on[WG_NOW], 0, 0, apply_widgets, 0, 0),
        ROW("Fecha", R_TOGGLE, &wg_on[WG_DATE], 0, 0, apply_widgets, 0, 0), ROW("Almacenamiento", R_TOGGLE, &wg_on[WG_STORAGE], 0, 0, apply_widgets, 0, 0),
        ROW("Energía", R_TOGGLE, &wg_on[WG_POWER], 0, 0, apply_widgets, 0, 0), ROW("Biblioteca", R_TOGGLE, &wg_on[WG_LIB], 0, 0, apply_widgets, 0, 0),
        ROW("Recién añadido", R_TOGGLE, &wg_on[WG_NEW], 0, 0, apply_widgets, 0, 0), ROW("Escuchas", R_TOGGLE, &wg_on[WG_LISTEN], 0, 0, apply_widgets, 0, 0) }, 8 };
    p->nsec = 1;
}
static void widgets_settings(const char *back)
{
    memset(wg_on, 0, sizeof wg_on); for (int i = 0; i < wg_n; i++) wg_on[wg_list[i]] = 1;
    open_page(page_widgets, back);
}
static void animate_scroll_to(int page);
/* toque con la pagina de widgets en pantalla (desde launcher_touch) */
static void widgets_touch(struct pkt *p)
{
    int sx = p->x, sy = p->y, moved = 0, horiz = 0; float s0 = wg_scroll, vel = 0; double lt = now_ms(); int lx = sx;
    while (cur_down && read_pkt(p, 40)) {
        int dx = cur_x - sx, dy = cur_y - sy;
        if (!moved && (abs(dx) > 10 || abs(dy) > 10)) { moved = 1; horiz = abs(dx) > abs(dy); }
        double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_x - lx) / (float)(t - lt); lx = cur_x; lt = t; }
        if (moved && horiz) { float s = -W - dx; if (s > 0) s = 0; if (s < -W - 40) s = -W - 40 + (s + W + 40) / 3; V.scroll = s; launcher_present(); }
        else if (moved) { float mx = wg_edit_y() + (int)wg_scroll + 40 - H; if (mx < 0) mx = 0; wg_scroll = s0 - dy; if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > mx) wg_scroll = mx; launcher_present(); }
    }
    if (moved && horiz) {
        if (cur_x - sx < -70 || vel < -0.35f) { widgets_on = 0; animate_scroll_to(0); }
        else { animate_scroll_to(-1); }
        return;
    }
    if (moved) return;
    int k = wg_at(sy);
    if (k == -2) { widgets_settings("Inicio"); launcher_present(); return; }
    if (k >= 0 && wg_list[k] == WG_NOW) { int a = app_by_id("nowplaying"); if (a >= 0) { widgets_on = 0; V.scroll = cur_page * W; launch_app(a, W / 2 - L.icon / 2, 120); } return; }
}
