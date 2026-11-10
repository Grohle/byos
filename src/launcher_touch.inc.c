/* launcher_touch.inc.c - apertura de apps (zoom), avisos y gestos del launcher. */
static void files_app(int ix, int iy);
static void settings_app(int ix, int iy);
static void store_app(int ix, int iy);
static void run_ext_app(int a, int ix, int iy);

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
    if (ap->act == ACT_TAP) { inj_tap(ap->ax, ap->ay); return wait_left_home(2500); }
    if (ap->act == ACT_STREAM) {
        inj_tap(319, 479);                                   /* "Stream media" (oculto en el tema) */
        if (!wait_left_home(2500)) return 0;
        c = st->cap_frames;
        inj_tap(ap->ax ? 236 : 84, 140); settle(c, 900, 200);
        return 1;
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
    if (f >= 0) { ssize_t n = read(f, old, sizeof old); close(f); if (n == (ssize_t)sizeof old && !memcmp(old + 30 * W, fr1 + 30 * W, (H - 30) * W * 2)) return 1; }
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
        for (int x = full; x < rw - full; x++) { int xx = rx + x; if (xx >= 0 && xx < W) d[xx] = s[xm[x]]; }
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

static void launch_app(int a, int ix, int iy)
{
    struct lapp *ap = &apps[a];
    if (ap->act == ACT_FILES) { V.pressed_app = -1; files_app(ix, iy); return; }
    if (ap->act == ACT_SETTINGS) { V.pressed_app = -1; settings_app(ix, iy); return; }
    if (ap->act == ACT_STORE) { V.pressed_app = -1; store_app(ix, iy); return; }
    if (ap->act == ACT_EXT) { V.pressed_app = -1; run_ext_app(a, ix, iy); return; }
    if (ap->act == ACT_SOON) {
        toast(tr("Radio llega en la próxima versión"));
        V.pressed_app = -1;
        double t = now_ms(); while (now_ms() - t < 1700) launcher_present();
        launcher_present();
        return;
    }
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
        if (t == 0 || !base_dim_ok) { for (int i = 0; i < W * H; i++) img_under[i] = dim565(img_base[i], 24); base_dim_ok = 1; have_under = 0; }
        memcpy(fr0, e < 0.15 ? img_base : img_under, HM_FRAME_BYTES);
        int ready = nav_done && nav_ok;
        draw_scaled(fr0, ready ? fr1 : img_app, rx, ry, rw, rh, r);
        /* el icono crece con la ventana y se funde en ella (como iOS) */
        int ia = (int)(255 * (1 - e * 3.2)); if (ia > 0) blit_scaled(fr0, &ap->icon[sz_idx()], rx, ry, rw, rh, ia);
        present();
        if (t >= 1) break;
    }
    int ok = nav_wait(3000);
    if (ok == 1) {
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
    if (page < 0) page = 0; if (page >= L.npages) page = L.npages - 1;
    float from = V.scroll, to = page * W;
    double t0 = now_ms(); int dur = 260;
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        V.scroll = from + (to - from) * (float)ease_out(t);
        launcher_present();
        if (t >= 1) break;
    }
    cur_page = page; V.scroll = to;
}

static void launcher_settings(void);

/* ------------------------------------------------------------------ toque en el launcher */
/* p: primer paquete (dedo abajo). Gestiona el gesto completo. */
static void launcher_touch(struct pkt *p)
{
    int sx = p->x, sy = p->y; double t0 = now_ms();
    /* arriba del todo: centro de control del reproductor (se le devuelve la pantalla) */
    if (sy < 30 && !V.edit && G.top == GA_PLAYER) {
        int moved = 0; while (read_pkt(p, 1000)) { if (!p->down) break; if (p->y - sy > 12) { moved = 1; break; } }
        if (moved) { player_from_edge(3, sy); drain_until_up(); }
        return;
    }
    if (sy < 30 && !V.edit && G.top == GA_NONE) { while (cur_down && read_pkt(p, 3000)) {} return; }
    if (sy < 30 && !V.edit) {
        /* centro de control de HiByOS, siguiendo al dedo */
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
            if (G.bottom_home == GA_PLAYER) sheet_open_drag(sy);
            else if (G.bottom_home == GA_CC) { drain_until_up(); cc_open(-1); }
            drain_until_up();
        }
        return;
    }
    /* botones del modo edicion */
    if (V.edit && sy >= 30 && sy < 70) {
        while (cur_down && read_pkt(p, 3000)) {}
        if (sx > W - 90) { V.edit = 0; lconf_save(); launcher_present(); return; }
        if (sx < 110) { V.edit = 0; settings_app(W / 2 - L.icon / 2, 40); return; }
        return;
    }
    int ix, iy, dock, idx;
    int a = hit_app(sx, sy, &ix, &iy, &dock, &idx);
    if (a >= 0 && !V.edit) { V.pressed_app = a; launcher_present(); }
    int moved = 0, longp = 0, entered = 0; float vel = 0; int lastx = sx; double lastt = t0;
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
        /* pulsacion larga sobre una app (o, ya editando, moverla): arrastre */
        if (!longp && a >= 0 && ((!moved && t - t0 > 450) || (V.edit && moved))) {
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
            if (s < -40) s = -40 + (s + 40) / 3; if (s > (L.npages - 1) * W + 40) s = (L.npages - 1) * W + 40 + (s - (L.npages - 1) * W - 40) / 3;
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
        animate_scroll_to(np);
        return;
    }
    if (!moved && a >= 0 && !V.edit) { launch_app(a, ix, iy); return; }
    if (!moved && V.edit && a < 0 && !entered) { V.edit = 0; lconf_save(); }   /* tocar en vacio sale de edicion */
    V.pressed_app = -1;
    launcher_present();
}
