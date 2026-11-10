/* widgets.inc.c - pagina de widgets a la izquierda del inicio (como "Hoy" en iOS).
 *
 * Desde la primera pagina del inicio, deslizar hacia la derecha. Fondo = fondo de pantalla
 * desenfocado (se calcula una vez por fondo); tarjetas de cristal con:
 *   Baterias  - el HiBy (%, cargando, Solo USB) y dispositivos Bluetooth conectados con su
 *               bateria (BlueZ: org.bluez.Battery1, por dbus-send)
 *   Reproduciendo, Fecha, Almacenamiento (SD e interna), Energia (modo y estado)
 * Configurables en Personalizar > Widgets (linea "widgets" de launcher.conf). Los datos se
 * leen al entrar y cada 10 s mientras la pagina esta visible; nunca en segundo plano. */

enum { WG_BATT, WG_NOW, WG_DATE, WG_STORAGE, WG_POWER, WG_N };
static const char *const wg_id[WG_N] = { "battery", "nowplaying", "date", "storage", "power" };
static const char *const wg_name[WG_N] = { "Baterías", "Reproduciendo", "Fecha", "Almacenamiento", "Energía" };
static int wg_list[WG_N] = { WG_BATT, WG_NOW, WG_DATE, WG_STORAGE, WG_POWER }, wg_n = WG_N;
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
static struct { int batt, charging, usb; struct wg_bt bt[3]; int nbt; long long sd_free, sd_tot, in_free, in_tot; double t; } WD;
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
    if (!wg_bg) wg_bg = malloc(HM_FRAME_BYTES);
    if (!wg_bg) return wall;
    if (!wg_bg_ok || strcmp(wg_bg_for, L.wall)) { cc_blur(wall, wg_bg); snprintf(wg_bg_for, sizeof wg_bg_for, "%s", L.wall); wg_bg_ok = 1; }
    return wg_bg;
}
static int wg_height(int w) { return w == WG_BATT ? 60 + 34 * (1 + WD.nbt) : w == WG_DATE ? 96 : w == WG_STORAGE ? 112 : 84; }
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
    int x = ox + 14, cw = W - 28, h = wg_height(w);
    if (y > H || y + h < 30) return h;
    cc_rrect(d, x, y, cw, h, 20, rgb(255, 255, 255), 44);
    char b[96];
    switch (w) {
    case WG_BATT: {
        draw_text(d, x + 16, y + 26, tr("Baterías"), 13, rgb(220, 220, 226), cw - 32, 30, H);
        int ry = y + 46;
        uint16_t col = WD.batt >= 0 && WD.batt <= 20 ? rgb(255, 69, 58) : (WD.charging ? rgb(48, 209, 88) : 0xFFFF);
        wg_ring(d, x + 30, ry + 14, 12, WD.batt < 0 ? 0 : WD.batt / 100.f, col);
        draw_text(d, x + 54, ry + 20, "HiBy R3 II", 15, 0xFFFF, cw - 130, 30, H);
        snprintf(b, sizeof b, "%d %%", WD.batt < 0 ? 0 : WD.batt);
        draw_text(d, x + cw - 16 - tw_n(b, 15, -1), ry + 20, b, 15, 0xFFFF, 0, 30, H);
        const char *sub = pw_nocharge && WD.usb ? tr("Solo USB") : WD.charging ? tr("Cargando") : WD.usb ? tr("Conectado por USB") : 0;
        if (sub) draw_text(d, x + 54, ry + 36, sub, 11, rgb(170, 170, 176), cw - 130, 30, H);
        for (int i = 0; i < WD.nbt; i++) {
            ry += 34;
            wg_ring(d, x + 30, ry + 14, 12, WD.bt[i].pct < 0 ? 0 : WD.bt[i].pct / 100.f, 0xFFFF);
            draw_text(d, x + 54, ry + 20, WD.bt[i].name, 15, 0xFFFF, cw - 130, 30, H);
            if (WD.bt[i].pct >= 0) { snprintf(b, sizeof b, "%d %%", WD.bt[i].pct); draw_text(d, x + cw - 16 - tw_n(b, 15, -1), ry + 20, b, 15, 0xFFFF, 0, 30, H); }
            else draw_text(d, x + cw - 16 - tw_n("—", 15, -1), ry + 20, "—", 15, rgb(170, 170, 176), 0, 30, H);
        }
        if (!WD.nbt) draw_text(d, x + 54, ry + 54, tr("Sin dispositivos Bluetooth"), 12, rgb(170, 170, 176), cw - 70, 30, H);
        break; }
    case WG_NOW: {
        int a = app_by_id("nowplaying");
        if (a >= 0) blit_scaled(d, &apps[a].icon[2], x + 14, y + 16, 52, 52, 255);
        draw_text(d, x + 80, y + 36, tr("Reproduciendo"), 15, 0xFFFF, cw - 96, 30, H);
        draw_text(d, x + 80, y + 56, tr(music_on ? "Sonando" : "En pausa"), 13, rgb(190, 190, 196), cw - 96, 30, H);
        break; }
    case WG_DATE: {
        time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
        static const char *const dn[7] = { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" };
        static const char *const mn[12] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
        draw_text(d, x + 16, y + 28, tr(dn[tm.tm_wday]), 14, rgb(255, 69, 58), cw - 32, 30, H);
        snprintf(b, sizeof b, "%d", tm.tm_mday); draw_text(d, x + 16, y + 74, b, 40, 0xFFFF, 0, 30, H);
        snprintf(b, sizeof b, "%s %d", tr(mn[tm.tm_mon]), 1900 + tm.tm_year); draw_text(d, x + 90, y + 70, b, 15, rgb(220, 220, 226), cw - 106, 30, H);
        snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min); draw_text(d, x + cw - 16 - tw_n(b, 22, -1), y + 40, b, 22, 0xFFFF, 0, 30, H);
        break; }
    case WG_STORAGE: {
        char s1[24], s2[24];
        draw_text(d, x + 16, y + 26, tr("Almacenamiento"), 13, rgb(220, 220, 226), cw - 32, 30, H);
        if (WD.sd_tot) { snprintf(b, sizeof b, "%s  ·  %s %s %s", tr("Tarjeta SD"), wg_size(WD.sd_free, s1, 24), tr("libres de"), wg_size(WD.sd_tot, s2, 24));
            draw_text(d, x + 16, y + 50, b, 13, 0xFFFF, cw - 32, 30, H); wg_bar(d, x + 16, y + 58, cw - 32, 1 - (float)WD.sd_free / WD.sd_tot, rgb(10, 132, 255)); }
        else draw_text(d, x + 16, y + 50, tr("Sin tarjeta SD"), 13, rgb(170, 170, 176), cw - 32, 30, H);
        snprintf(b, sizeof b, "%s  ·  %s %s %s", tr("En el HiBy"), wg_size(WD.in_free, s1, 24), tr("libres de"), wg_size(WD.in_tot, s2, 24));
        draw_text(d, x + 16, y + 86, b, 13, 0xFFFF, cw - 32, 30, H);
        if (WD.in_tot) wg_bar(d, x + 16, y + 94, cw - 32, 1 - (float)WD.in_free / WD.in_tot, rgb(191, 90, 242));
        break; }
    case WG_POWER: {
        draw_text(d, x + 16, y + 26, tr("Energía"), 13, rgb(220, 220, 226), cw - 32, 30, H);
        snprintf(b, sizeof b, "%s %s", tr("Modo"), tr(pw_name(pw_mode)));
        draw_text(d, x + 16, y + 52, b, 17, pw_mode == PW_SAVE ? rgb(255, 204, 0) : pw_mode == PW_PERF ? rgb(255, 69, 58) : 0xFFFF, cw - 32, 30, H);
        draw_text(d, x + 16, y + 72, tr(pw_nocharge ? "Solo USB: la batería no se carga" : "Carga normal"), 12, rgb(170, 170, 176), cw - 32, 30, H);
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
        ROW("Energía", R_TOGGLE, &wg_on[WG_POWER], 0, 0, apply_widgets, 0, 0) }, 5 };
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
