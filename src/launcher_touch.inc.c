/* launcher_touch.inc.c - apertura de apps (zoom), avisos y gestos del launcher. */
static void files_app(int ix, int iy);
static int wg_page_on_now(void);
static void settings_app(int ix, int iy);
static void usbdac_app_at(int ix, int iy);
static void wireless_app_at(int ix, int iy);
static void personalize_app(int ix, int iy);
static void settings_deep_app(int ix, int iy, int start);
static void books_app_at(int ix, int iy);
static int bk_gut;
static void freemusic_app_at(int ix, int iy);
static void buds_app_at(int ix, int iy);
static void radio_app_at(int ix, int iy);
static void store_app(int ix, int iy);
static void run_ext_app(int a, int ix, int iy);
static void music_app(int ix, int iy);
static int mu_autonp;

/* ------------------------------------------------------------------ aviso breve (toast) */
static struct sprite toast_lbl; static double toast_until;
static void toast(const char *msg)
{
    free(toast_lbl.rgb); free(toast_lbl.a); memset(&toast_lbl, 0, sizeof toast_lbl);
    text_sprite(&toast_lbl, msg, 14.f, 0xFFFF, 0);
    toast_until = now_ms() + 1600;
}
static void draw_toast(uint16_t *dst)
{
    if (now_ms() > toast_until || !toast_lbl.w) return;
    int w = toast_lbl.w + 32, x = (W - w) / 2, y = 40;
    fill_rrect(dst, x, y, w, 36, 18, rgb(28, 28, 32), 225);
    blit(dst, &toast_lbl, x + 16, y + (36 - toast_lbl.h) / 2, 32, 255);
}

/* ------------------------------------------------------------------ abrir apps */
static int nav_app;
/* pagina intermedia "Stream media" (TIDAL / Qobuz): fichas grises con iconos negros */
static int is_stream_page(const uint16_t *img)
{
    static const int blk[][2] = { {58, 150}, {210, 150}, {58, 100}, {210, 100}, {80, 380} };
    for (unsigned i = 0; i < 5; i++) if (lum(img[blk[i][1] * W + blk[i][0]]) > 8) return 0;
    int g = lum(img[95 * W + 30]), g2 = lum(img[95 * W + 190]);
    return g > 14 && g < 40 && g2 > 14 && g2 < 40 && has_statusbar(img);
}
static int stream_launched;
/* espera a que el reproductor deje el inicio (la primera vez puede tardar en cargar) */
static int wait_left_home(int timeout_ms)
{
    double t0 = now_ms();
    while (now_ms() - t0 < timeout_ms) {
        if (!is_home(fr1)) { uint32_t c = st->cap_frames; settle(c, 60, 120); return 1; }
        msleep(10); st->shell_beat++;
    }
    return 0;
}
static int open_app_nav(void)   /* en el hilo de navegacion; pantalla congelada */
{
    struct lapp *ap = &apps[nav_app];
    uint32_t c = st->cap_frames;
    if (ap->act == ACT_TAP || ap->act == ACT_MUSIC) { inj_tap(ap->ax, ap->ay); return wait_left_home(2500); }
    if (ap->act == ACT_STREAM) {
        inj_tap(319, 479);                                   /* "Stream media" (oculto en el tema) */
        if (!wait_left_home(6000)) return 0;                 /* la primera vez tras arrancar tarda >2,5 s ("No se pudo abrir") */
        /* la pagina intermedia entra deslizando: tocar la casilla a mitad de la animacion se perdia
         * o se comprobaba la pagina antes de que estuviera ("No se pudo abrir" la primera vez tras
         * arrancar, aunque luego se abria). Esperar a que este quieta, tocar, y esperar a que se vaya
         * (la primera vez Qobuz tarda > 1 s); si a los 3 s sigue ahi, otro toque. */
        { double tw = now_ms(); while (!is_stream_page(fr1) && now_ms() - tw < 4000) { msleep(20); st->shell_beat++; } }
        c = st->cap_frames; settle(c, 300, 120);
        int tx = ap->ax ? 236 : 84;
        inj_tap(tx, 140);
        /* abierta = una pantalla que no es ni la intermedia ni el inicio y que sigue asi al asentarse
         * (tras tocar, HiBy puede tapar un momento la intermedia con un aviso de carga y volver a
         * ella antes de abrir el servicio: medido, fallaba a los 1,1 s y se abria a los 3 s) */
        double tw = now_ms(); int again = 0;
        while (now_ms() - tw < 12000) {
            if (!is_stream_page(fr1) && !is_home(fr1)) {
                uint32_t c2 = st->cap_frames; settle(c2, 600, 250);
                if (!is_stream_page(fr1) && !is_home(fr1)) { lg("stream: abierto en %.0f ms", now_ms() - tw); return 1; }
            }
            msleep(20); st->shell_beat++;
            if (!again && now_ms() - tw > 4000 && is_stream_page(fr1)) { again = 1; lg("stream: la casilla no respondio; otro toque"); inj_tap(tx, 140); }
        }
        lg("stream: sigue en %s tras 12 s", is_stream_page(fr1) ? "la pagina intermedia" : "el inicio");
        return 0;
    }
    if (ap->act == ACT_PLANE) { inj_swipe(300, 240, 30, 240, 5, 60); settle(c, 700, 150); return is_plane(fr1); }
    return 0;
}

static uint16_t img_app[W * H], img_base[W * H];
static int app_cache(int a, int save)
{
    char p[128]; snprintf(p, sizeof p, ASSETS "/cache/%s.raw", apps[a].id);
    if (!save) {
        int f = open(p, O_RDONLY); if (f < 0) return 0;
        ssize_t n = read(f, img_app, sizeof img_app); close(f);
        return n == (ssize_t)sizeof img_app;
    }
    /* guardar solo si cambio (evita escribir la flash en cada apertura) */
    static uint16_t old[W * H];
    int f = open(p, O_RDONLY);
    if (f >= 0) {
        ssize_t n = read(f, old, sizeof old); close(f);
        int same = n == (ssize_t)sizeof old && !memcmp(old + 30 * W, fr1 + 30 * W, (H - 30) * W * 2);
        drop_pages(old, sizeof old);           /* solo servia para comparar: fuera de la memoria */
        if (same) return 1;
    }
    mkdir(ASSETS "/cache", 0755);
    f = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f < 0) return 0;
    write(f, fr1, sizeof img_app); close(f);
    return 1;
}

/* dibuja "src" escalado en el rectangulo (rx,ry,rw,rh) con esquinas de radio r */
/* "src" (pantalla completa) escalado al rectangulo con esquinas redondeadas SUAVIZADAS:
 * el borde curvo mezcla el pixel con el fondo segun la cobertura (sin dientes ni halos). */
static inline uint16_t mix565(uint16_t c, uint16_t d, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
static void draw_scaled(uint16_t *dst, const uint16_t *src, int rx, int ry, int rw, int rh, float r)
{
    static int xm[W];
    if (rw < 2 || rh < 2) return;
    if (r > rw / 2.f) r = rw / 2.f; if (r > rh / 2.f) r = rh / 2.f;
    for (int x = 0; x < rw && x < W; x++) xm[x] = x * W / rw;
    for (int y = 0; y < rh; y++) {
        int yy = ry + y; if (yy < 0 || yy >= H) continue;
        const uint16_t *s = src + (y * H / rh) * W;
        uint16_t *d = dst + yy * W;
        float edge = 0;                                /* x del borde curvo (con decimales) */
        if (r > 0.5f) {
            float dy = y + .5f < r ? r - (y + .5f) : (y + .5f > rh - r ? (y + .5f) - (rh - r) : 0);
            if (dy > 0) edge = r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0);
        }
        int full = (int)ceilf(edge);
        int xs = full > -rx ? full : -rx, xe = rw - full < W - rx ? rw - full : W - rx;   /* recortado a la pantalla */
        for (int x = xs; x < xe; x++) d[rx + x] = s[xm[x]];
        if (edge > 0) {                                /* pixeles del borde: cobertura parcial */
            int e0 = (int)edge; unsigned a = (unsigned)((1.f - (edge - e0)) * 255);
            int xl = rx + e0, xr = rx + rw - 1 - e0;
            if (e0 < rw / 2) {
                if (xl >= 0 && xl < W) d[xl] = mix565(s[xm[e0]], d[xl], a);
                if (xr >= 0 && xr < W && xr != xl) d[xr] = mix565(s[xm[rw - 1 - e0]], d[xr], a);
            }
        }
    }
}

/* radio de la ventana que crece desde un icono: empieza igual que el icono (squircle de
 * iOS = 22,37 % del lado) y se reduce con curva suave hasta las esquinas de la pantalla */
static float win_radius(int w, int h, double e)
{
    float side = w < h ? w : h;
    float r0 = side * 0.2237f;
    double k = (1 - e) * (1 - e);
    return (float)(r0 * k + 18.0 * (1 - k) * (1 - e));
}

/* Un fotograma del zoom de una app (abrir/cerrar desde su icono): el fondo "bg" oscurecido
 * (k 0..32) y encima "src" escalado al rectangulo. Solo se oscurece lo que la ventana no
 * tapa (las filas de las esquinas redondeadas, enteras): al final del zoom la ventana cubre
 * casi toda la pantalla y el fondo apenas cuesta. Mismo resultado que oscurecer todo. */
static void zoom_frame(uint16_t *d, const uint16_t *bg, int k, const uint16_t *src, int rx, int ry, int rw, int rh, float r)
{
    if (rw < 2 || rh < 2) { dim_buf(d, bg, W * H, k); return; }
    float rr = r; if (rr > rw / 2.f) rr = rw / 2.f; if (rr > rh / 2.f) rr = rh / 2.f;
    int rc = rr > 0.5f ? (int)ceilf(rr) + 1 : 0;
    int y0 = ry + rc, y1 = ry + rh - rc;
    int xa = rx < 0 ? 0 : rx, xb = rx + rw > W ? W : rx + rw;
    for (int y = 0; y < H; y++) {
        uint16_t *dr = d + y * W; const uint16_t *s = bg + y * W;
        if (y < y0 || y >= y1 || xa >= xb) dim_buf(dr, s, W, k);
        else { dim_buf(dr, s, xa, k); dim_buf(dr + xb, s + xb, W - xb, k); }
    }
    draw_scaled(d, src, rx, ry, rw, rh, r);
}

/* sprite escalado (vecino mas cercano) con opacidad global; para el icono que crece */
static void blit_scaled(uint16_t *dst, const struct sprite *s, int rx, int ry, int rw, int rh, int alpha)
{
    if (!s || !s->w || rw < 1 || rh < 1 || alpha <= 0) return;
    for (int y = 0; y < rh; y++) {
        int yy = ry + y; if (yy < 0 || yy >= H) continue;
        int sy = y * s->h / rh;
        for (int x = 0; x < rw; x++) {
            int xx = rx + x; if (xx < 0 || xx >= W) continue;
            int i = sy * s->w + x * s->w / rw;
            unsigned a = s->a[i] * alpha / 255; if (!a) continue;
            unsigned c = s->rgb[i], d = dst[yy * W + xx], na = 255 - a;
            dst[yy * W + xx] = (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
        }
    }
}

/* zoom de una app propia: como zoom_frame y, mientras la ventana es pequena, su icono encima
 * fundiendose (igual que al abrir las apps de HiBy y al volver al inicio con el gesto).
 * El icono es el de last_app (launch_app lo pone antes de abrir la app). */
static void zoom_frame_app(uint16_t *d, const uint16_t *bg, int k, const uint16_t *src, int rx, int ry, int rw, int rh, float r)
{
    zoom_frame(d, bg, k, src, rx, ry, rw, rh, r);
    if (last_app < 0 || last_app >= NAPPS || W <= L.icon) return;
    double e = (double)(rw - L.icon) / (W - L.icon);
    int ia = (int)(255 * (1 - e * 3.2)); if (ia > 255) ia = 255;
    if (ia > 0) blit_scaled(d, &apps[last_app].icon[sz_idx()], rx, ry, rw, rh, ia);
}

static void launch_app_body(int a, int ix, int iy);
/* abrir una app: mientras esta abierta (las propias corren aqui dentro) se ve la barra de inicio,
 * que entra al acabar el zoom de apertura */
static void app_exit_release(void);    /* syspop.inc.c: lo que la app usaba vuelve al sistema */
static void launch_app(int a, int ix, int iy)
{
    hb_apps++; hb_after = now_ms() + 330;
    launch_app_body(a, ix, iy);
    hb_apps--;
    app_exit_release();
}
static void launch_app_body(int a, int ix, int iy)
{
    struct lapp *ap = &apps[a];
    /* apps propias: solo una abierta a la vez (la ultima queda como tarjeta de la multitarea) */
    if (ap->act == ACT_MUSIC && !strcmp(ap->id, "nowplaying")) {
        V.pressed_app = -1; render_launcher(img_base, now_ms());
        np_app_run(img_base, ix, iy); launcher_present(); return;
    }
    if (ap->act == ACT_MUSIC) { V.pressed_app = -1; last_app = a; music_app(ix, iy); return; }
    if (ap->act == ACT_FILES) { V.pressed_app = -1; last_app = a; files_app(ix, iy); return; }
    if (ap->act == ACT_SETTINGS) {       /* una sola app: el Centro de control puede pedir una pagina */
        int st = settings_start; settings_start = SET_ROOT;
        V.pressed_app = -1; last_app = a;
        if (st == SET_USB) usbdac_app_at(ix, iy); else if (st == SET_WIRELESS) wireless_app_at(ix, iy);
        else if (st == SET_PERS) personalize_app(ix, iy);
        else if (st >= SET_POWER && st <= SET_CCPAGE) settings_deep_app(ix, iy, st);
        else settings_app(ix, iy);
        return;
    }
    if (ap->act == ACT_WIRELESS) { V.pressed_app = -1; last_app = a; wireless_app_at(ix, iy); return; }
    if (ap->act == ACT_SOON) {
        toast(tr("Radio llega en la próxima versión"));
        V.pressed_app = -1;
        double t = now_ms(); while (now_ms() - t < 1700) launcher_present();
        launcher_present();
        return;
    }
    if (ap->act == ACT_BOOKS) { V.pressed_app = -1; last_app = a; bk_gut = ap->ax == 1; books_app_at(ix, iy); return; }
    if (ap->act == ACT_FREEMUSIC) { V.pressed_app = -1; last_app = a; freemusic_app_at(ix, iy); return; }
    if (ap->act == ACT_BUDS) { V.pressed_app = -1; last_app = a; buds_app_at(ix, iy); return; }
    if (ap->act == ACT_RADIO) { V.pressed_app = -1; last_app = a; radio_app_at(ix, iy); return; }
    if (ap->act == ACT_STORE) { V.pressed_app = -1; last_app = a; store_app(ix, iy); return; }
    if (ap->act == ACT_EXT) { V.pressed_app = -1; run_ext_app(a, ix, iy); return; }
    lg("abrir %s", ap->id);
    V.pressed_app = -1;
    render_launcher(img_base, now_ms());
    int have = app_cache(a, 0);
    if (!have) memset(img_app, 0, sizeof img_app);   /* las pantallas de HiBy son negras */
    nav_app = a;
    nav_start(NAV_APP);
    int base_dim_ok = 0;
    double t0 = now_ms(); const int dur = 320;
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        double e = ease_out(t);
        int rx = (int)(ix + (0 - ix) * e), ry = (int)(iy + (0 - iy) * e);
        int rw = (int)(L.icon + (W - L.icon) * e), rh = (int)(L.icon + (H - L.icon) * e);
        float r = win_radius(rw, rh, e);
        if (t == 0 || !base_dim_ok) { dim_buf(img_under, img_base, W * H, 24); base_dim_ok = 1; have_under = 0; }
        memcpy(fr0, e < 0.15 ? img_base : img_under, HM_FRAME_BYTES);
        /* toda la animacion con la imagen guardada de la app (o negro): cambiar a la pantalla en
         * vivo a mitad del zoom daba un tiron mientras la app aun se dibujaba */
        draw_scaled(fr0, img_app, rx, ry, rw, rh, r);
        /* el icono crece con la ventana y se funde en ella (como iOS) */
        int ia = (int)(255 * (1 - e * 3.2)); if (ia > 0) blit_scaled(fr0, &ap->icon[sz_idx()], rx, ry, rw, rh, ia);
        present();
        if (t >= 1) break;
    }
    int ok = nav_wait(ap->act == ACT_STREAM ? 16000 : 3000);
    if (ok == 1) {
        /* de la imagen guardada a la app en vivo con un fundido corto */
        { double tf = now_ms(); for (;;) { double t = (now_ms() - tf) / 140.0; if (t > 1) t = 1; unsigned k = (unsigned)(255 * t);
              for (int i = 0; i < W * H; i++) fr0[i] = mix565(fr1[i], img_app[i], k);
              present(); if (t >= 1) break; } }
        app_cache(a, 1);
        last_app = a;
        stream_launched = ap->act == ACT_STREAM;
        memcpy(img_cache_home, img_base, sizeof img_cache_home); have_cache_home = 1;
        home_publish();
        launcher_on = 0; V.edit = 0;
        cmd('R');
        lg("app %s abierta en %.0f ms", ap->id, now_ms() - t0);
        return;
    }
    /* no se pudo: volver al icono */
    lg("no se pudo abrir %s", ap->id);
    double t1 = now_ms();
    for (;;) {
        double t = (now_ms() - t1) / 220; if (t > 1) t = 1;
        double e = 1 - ease_out(t);
        int rx = (int)(ix * (1 - e)), ry = (int)(iy * (1 - e));
        int rw = (int)(L.icon + (W - L.icon) * e), rh = (int)(L.icon + (H - L.icon) * e);
        memcpy(fr0, img_base, sizeof img_base);
        draw_scaled(fr0, img_app, rx, ry, rw, rh, win_radius(rw, rh, 1 - e));
        present();
        if (t >= 1) break;
    }
    if (!is_home(fr1)) { uint32_t c = st->cap_frames; inj_tap(22, 48); settle(c, 600, 120); }
    toast(tr("No se pudo abrir"));
    launcher_present();
}

/* ------------------------------------------------------------------ busqueda por posicion */
/* devuelve app bajo (x,y) en la pagina actual o dock; *ix,*iy = esquina del icono */
static int hit_app(int x, int y, int *ix, int *iy, int *in_dock, int *index)
{
    int top_shift = V.edit ? 22 : 0;
    for (int i = 0; i < L.dn; i++) {
        int dx, dy; dock_xy(i, L.dn, &dx, &dy);
        if (x >= dx - 6 && x < dx + L.icon + 6 && y >= dy - 6 && y < dy + L.icon + 6) { *ix = dx; *iy = dy; *in_dock = 1; *index = i; return L.dock[i]; }
    }
    int p = cur_page;
    for (int i = 0; i < L.pn[p]; i++) {
        int cx, cy; cell_xy(i, &cx, &cy); cy += top_shift;
        if (x >= cx - 8 && x < cx + L.icon + 8 && y >= cy - 6 && y < cy + L.icon + 18) { *ix = cx; *iy = cy; *in_dock = 0; *index = i; return L.page[p][i]; }
    }
    return -1;
}
/* hueco de destino para soltar una app en (x,y) */
static void drop_target(int x, int y, int *page, int *slot, int *dock)
{
    if (y >= DOCK_Y - 6) {
        int n = L.dn; for (int i = 0; i < L.dn; i++) if (L.dock[i] == V.drag_app) n--;
        if (n >= 4) { *dock = -1; *page = cur_page; *slot = L.pn[cur_page]; return; }
        int best = 0; float bd = 1e9;
        for (int i = 0; i <= n; i++) { int dx, dy; dock_xy(i, n + 1, &dx, &dy); float d = fabsf(x - (dx + L.icon / 2.f)); if (d < bd) { bd = d; best = i; } }
        *dock = best; *page = -1; *slot = -1; return;
    }
    *dock = -1; *page = cur_page;
    int top_shift = 22, cw = (W - 36) / L.cols, rowh = L.icon + 5 + 14 + 16;
    int col = (x - 18) / cw; if (col < 0) col = 0; if (col >= L.cols) col = L.cols - 1;
    int row = (y - GRID_TOP - top_shift + 8) / rowh; if (row < 0) row = 0;
    int n = L.pn[cur_page]; for (int i = 0; i < L.pn[cur_page]; i++) if (L.page[cur_page][i] == V.drag_app) n--;
    int s = row * L.cols + col; if (s > n) s = n; if (s >= per_page()) s = per_page() - 1;
    *slot = s;
}
static void remove_app_everywhere(int a)
{
    for (int p = 0; p < L.npages; p++) for (int i = 0; i < L.pn[p]; i++) if (L.page[p][i] == a) { memmove(&L.page[p][i], &L.page[p][i + 1], (L.pn[p] - i - 1) * sizeof(int)); L.pn[p]--; i--; }
    for (int i = 0; i < L.dn; i++) if (L.dock[i] == a) { memmove(&L.dock[i], &L.dock[i + 1], (L.dn - i - 1) * sizeof(int)); L.dn--; i--; }
}
static void commit_drop(void)
{
    int a = V.drag_app;
    if (a < 0) return;
    remove_app_everywhere(a);
    if (V.drag_dock >= 0 && L.dn < 4) {
        int s = V.drag_dock > L.dn ? L.dn : V.drag_dock;
        memmove(&L.dock[s + 1], &L.dock[s], (L.dn - s) * sizeof(int)); L.dock[s] = a; L.dn++;
    } else {
        int p = V.drag_slot_page < 0 ? cur_page : V.drag_slot_page;
        if (p >= L.npages) { p = L.npages; L.npages++; L.pn[p] = 0; }
        if (L.pn[p] >= per_page()) {   /* pagina llena: la ultima app pasa a la siguiente */
            int last = L.page[p][--L.pn[p]];
            int q = p + 1; if (q >= L.npages && L.npages < MAXPAGES) { L.npages++; L.pn[q] = 0; }
            if (q < L.npages) { memmove(&L.page[q][1], &L.page[q][0], L.pn[q] * sizeof(int)); L.page[q][0] = last; L.pn[q]++; }
        }
        int s = V.drag_slot > L.pn[p] ? L.pn[p] : V.drag_slot;
        memmove(&L.page[p][s + 1], &L.page[p][s], (L.pn[p] - s) * sizeof(int)); L.page[p][s] = a; L.pn[p]++;
    }
    /* quitar paginas vacias */
    for (int p = 0; p < L.npages; p++) if (!L.pn[p] && L.npages > 1) {
        for (int q = p; q < L.npages - 1; q++) { L.pn[q] = L.pn[q + 1]; memcpy(L.page[q], L.page[q + 1], sizeof L.page[q]); }
        L.npages--; p--;
    }
    if (cur_page >= L.npages) cur_page = L.npages - 1;
    lconf_save();
}

/* ------------------------------------------------------------------ desplazamiento de pagina */
static void animate_scroll_to(int page)
{
    if (page < -1) page = -1; if (page >= L.npages) page = L.npages - 1;
    if (page < 0 && !wg_page_on_now()) page = 0;    /* pagina de widgets desactivada: un lanzamiento no la abre */
    float from = V.scroll, to = page * W;
    double t0 = now_ms(); int dur = 260;
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        V.scroll = from + (to - from) * (float)ease_out(t);
        launcher_present();
        if (t >= 1) break;
    }
    if (page < 0) { widgets_on = 1; cur_page = 0; V.scroll = -W; }
    else { widgets_on = 0; cur_page = page; V.scroll = to; }
}

static void launcher_settings(void);
static void home_switcher(int sy);
static int action_sheet(const char *title, const char **opts, int n, int destructive_idx);
static int app_uninstall_home(int a);       /* store.inc.c */
/* menu de una app del inicio (pulsacion larga y soltar, como en iOS) */
static void app_menu(int a, int ix, int iy)
{
    V.pressed_app = -1; launcher_present();
    const char *o[3]; int n = 0, rem = app_removable(a);
    o[n++] = tr("Abrir"); o[n++] = tr("Editar pantalla de inicio");
    if (rem) o[n++] = tr("Desinstalar");
    lg("menu de app: %s", apps[a].id);
    int k = action_sheet(app_label(a), o, n, rem ? 2 : -1);
    if (k == 0) { launch_app(a, ix, iy); return; }
    if (k == 1) V.edit = 1;
    if (k == 2 && rem) app_uninstall_home(a);
    launcher_present();
}
static void widgets_touch(struct pkt *p);
static int wg_page_on;
static int wg_page_on_now(void) { return wg_page_on; }

/* ------------------------------------------------------------------ toque en el launcher */
/* p: primer paquete (dedo abajo). Gestiona el gesto completo. */
static void launcher_touch(struct pkt *p)
{
    int sx = p->x, sy = p->y; double t0 = now_ms();
    if (widgets_on && sy >= G_TOP) { widgets_touch(p); return; }
    /* arriba del todo: centro de control del reproductor (se le devuelve la pantalla) */
    int top_act = sx < W / 2 ? G.top : G.top_r;           /* mitad izquierda / derecha */
    if (sy < G_TOP && !V.edit && top_act == GA_PLAYER) {
        int moved = 0; while (read_pkt(p, 1000)) { if (!p->down) break; if (p->y - sy > 12) { moved = 1; break; } }
        if (moved) { player_from_edge(3, sy); drain_until_up(); }
        return;
    }
    if (sy < G_TOP && !V.edit && top_act == GA_NONE) { while (cur_down && read_pkt(p, 3000)) {} return; }
    if (sy < G_TOP && !V.edit) {
        /* centro de control de ByOS, siguiendo al dedo */
        int moved = 0; while (read_pkt(p, 1000)) { if (!p->down) break; if (p->y - sy > 10) { moved = 1; break; } }
        if (moved) cc_open(sy);
        drain_until_up();
        return;
    }
    /* desde abajo: Now Playing */
    if (sy >= 456 && !V.edit) {
        int decided = 0;
        while (!decided && read_pkt(p, 1000)) {
            if (!p->down) { decided = 1; break; }
            if (sy - p->y > 12) { decided = 2; break; }
            if (abs(p->x - sx) > 24) { decided = 1; break; }
        }
        if (decided == 2) {
            lg("gesto desde abajo (launcher)");
            if (G.bottom_home == GA_HOME) home_switcher(sy);
            else if (G.bottom_home == GA_PLAYER) sheet_open_drag(sy);
            else if (G.bottom_home == GA_CC) { drain_until_up(); cc_open(-1); }
            drain_until_up();
        }
        return;
    }
    /* botones del modo edicion */
    if (V.edit && sy >= 30 && sy < 70) {
        while (cur_down && read_pkt(p, 3000)) {}
        if (sx > W - 90) { V.edit = 0; lconf_save(); launcher_present(); return; }
        if (sx < 110) { V.edit = 0; launcher_settings(); return; }
        return;
    }
    int ix, iy, dock, idx;
    int a = hit_app(sx, sy, &ix, &iy, &dock, &idx);
    if (a >= 0 && !V.edit) { V.pressed_app = a; launcher_present(); }
    int moved = 0, longp = 0, entered = 0, held = 0; float vel = 0; int lastx = sx; double lastt = t0;
    for (;;) {
        int got = read_pkt(p, 30);
        double t = now_ms();
        if (got && !p->down) break;
        if (got) {
            if (t > lastt) vel = 0.6f * vel + 0.4f * (p->x - lastx) / (float)(t - lastt);
            lastx = p->x; lastt = t;
            if (!moved && (abs(p->x - sx) > 10 || abs(p->y - sy) > 10)) {
                moved = 1;
                if (V.pressed_app >= 0) { V.pressed_app = -1; launcher_present(); }
            }
        }
        /* pulsacion larga sobre una app: queda "armada" (el icono se suelta). Al levantar el dedo sin
         * moverlo sale su menu, como en iOS; al moverlo (o, ya editando, con solo moverla) se arrastra */
        if (!held && !longp && a >= 0 && !V.edit && !moved && t - t0 > 450) { held = 1; V.pressed_app = -1; launcher_present(); }
        if (!longp && a >= 0 && moved && (held || V.edit)) {
            longp = 1; V.edit = 1; V.pressed_app = -1;
            V.drag_app = a; V.drag_x = cur_x; V.drag_y = cur_y; V.drag_dock = -1;
            drop_target(cur_x, cur_y, &V.drag_slot_page, &V.drag_slot, &V.drag_dock);
            lg("editar: arrastrando %s", apps[a].id);
        }
        if (longp) {
            V.drag_x = cur_x; V.drag_y = cur_y;
            drop_target(cur_x, cur_y, &V.drag_slot_page, &V.drag_slot, &V.drag_dock);
            static double edge_t; int edge = cur_x < 26 ? -1 : cur_x > W - 26 ? 1 : 0;
            if (!edge) edge_t = 0; else if (!edge_t) edge_t = t;
            else if (t - edge_t > 480) {   /* flotando en el borde: pasar de pagina (se repite) */
                int np = cur_page + edge;
                if (np >= 0 && np < MAXPAGES && (np < L.npages || L.pn[cur_page] > 1)) {
                    if (np >= L.npages) { L.npages++; L.pn[np] = 0; }
                    animate_scroll_to(np); edge_t = 0;
                }
            }
            launcher_present();
            continue;
        }
        if (moved && abs(cur_x - sx) > abs(cur_y - sy)) {   /* deslizar paginas */
            float s = cur_page * W - (cur_x - sx);
            float lo = cur_page == 0 && !V.edit && wg_page_on ? -W : -40;          /* a la izquierda: widgets */
            if (s < lo) s = lo + (s - lo) / 3; if (s > (L.npages - 1) * W + 40) s = (L.npages - 1) * W + 40 + (s - (L.npages - 1) * W - 40) / 3;
            V.scroll = s;
            launcher_present();
        }
        if (V.edit || a >= 0 || moved) { if (V.edit) launcher_present(); continue; }
        /* pulsacion larga en vacio: editar */
        if (!moved && t - t0 > 550 && !V.edit) { V.edit = 1; entered = 1; launcher_present(); }
    }
    if (longp) {           /* soltar la app arrastrada */
        commit_drop();
        V.drag_app = -1; V.drag_dock = -1;
        V.scroll = cur_page * W;
        launcher_present();
        return;
    }
    if (moved && abs(cur_x - sx) > abs(cur_y - sy)) {
        int dx = cur_x - sx, np = cur_page;
        if (dx < -70 || vel < -0.35f) np++; else if (dx > 70 || vel > 0.35f) np--;
        if (np < 0 && V.edit) np = 0;
        animate_scroll_to(np);
        return;
    }
    if (held && !moved && a >= 0) { app_menu(a, ix, iy); return; }
    if (!moved && a >= 0 && !V.edit) { launch_app(a, ix, iy); return; }
    if (!moved && V.edit && a < 0 && !entered) { V.edit = 0; lconf_save(); }   /* tocar en vacio sale de edicion */
    V.pressed_app = -1;
    launcher_present();
}
