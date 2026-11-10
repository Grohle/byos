/* gestures.inc.c - gestos del sistema configurables y multitarea estilo iOS.
 *
 * Cada borde tiene una accion (Personalizar > Gestos, guardada en launcher.conf):
 *   abajo (en apps):   Inicio y multitarea (iOS) | Reproductor | Centro de control | Nada
 *   abajo (en inicio): Reproductor | Centro de control | Nada
 *   arriba:            Centro de control (HiBy) | Reproductor | Nada
 *   izquierda:         Atras (HiBy) | Reproductor | Centro de control
 *   derecha:           Reproductor | Centro de control | Nada
 * Multitarea: desde abajo, deslizar y MANTENER = dos tarjetas ("donde estabas" y
 * Reproduciendo); deslizar rapido = la app se encoge hasta su icono y vuelve al inicio. */

static const char *ga_names[] = { "none", "home", "player", "cc", "back" };
static int ga_parse(const char *s) { for (int i = 0; i < 5; i++) if (s && !strcmp(s, ga_names[i])) return i; return -1; }

/* lineas "gesture CLAVE VALOR" de launcher.conf */
static int g_conf_v2;   /* los valores guardados antes de la v2 se ignoran: nuevos por defecto */
static void gestures_conf_line(char *k, char *v)
{
    int a = ga_parse(v);
    if (!k || !v) return;
    if (!strcmp(k, "v")) { g_conf_v2 = atoi(v) >= 3; return; }
    if (!g_conf_v2) return;
    if (!strcmp(k, "chevron")) { G.chevron = atoi(v) != 0; return; }
    if (a < 0) return;
    if (!strcmp(k, "bottom_app") && a != GA_BACK) G.bottom_app = a;
    else if (!strcmp(k, "bottom_home") && a != GA_BACK) G.bottom_home = a;
    else if (!strcmp(k, "top") && a != GA_HOME && a != GA_BACK) G.top = a;
    else if (!strcmp(k, "left") && a != GA_HOME) G.left = a;
    else if (!strcmp(k, "right") && a != GA_HOME) G.right = a;
    else if (!strcmp(k, "top_r") && a != GA_HOME && a != GA_BACK) G.top_r = a;
}
static void gestures_conf_save(FILE *f)
{
    fprintf(f, "gesture v 3\ngesture top_r %s\n", ga_names[G.top_r]);
    fprintf(f, "gesture bottom_app %s\ngesture bottom_home %s\ngesture top %s\ngesture left %s\ngesture right %s\ngesture chevron %d\n",
            ga_names[G.bottom_app], ga_names[G.bottom_home], ga_names[G.top], ga_names[G.left], ga_names[G.right], G.chevron);
}

/* centro de control de HiBy: se le simula el deslizamiento desde arriba */
static void open_cc(void)
{
    if (launcher_on) { launcher_leave(); double tw = now_ms(); while (st->capture && now_ms() - tw < 300) msleep(2); }
    inj_swipe(160, 3, 160, 330, 8, 180);
}

/* ------------------------------------------------------------------ reproductor desde un borde */
/* la hoja entra por el lado del gesto siguiendo al dedo: dir 1 derecha, 2 izquierda, 3 arriba */
static void compose_slide(uint16_t *dst, const uint16_t *under, const uint16_t *sheet, int dx, int dy, int k)
{
    for (int y = 0; y < H; y++) {
        uint16_t *d = dst + y * W; const uint16_t *u = under + y * W;
        int sy = y - dy;
        if (sy < 0 || sy >= H) { for (int x = 0; x < W; x++) d[x] = dim565(u[x], k); continue; }
        const uint16_t *s = sheet + sy * W;
        for (int x = 0; x < W; x++) { int sx = x - dx; d[x] = (sx >= 0 && sx < W) ? s[sx] : dim565(u[x], k); }
    }
    /* sombra en el borde de la hoja */
    if (dx > 0) for (int y = 0; y < H; y++) for (int s = 1; s <= 6 && dx - s >= 0; s++) dst[y * W + dx - s] = dim565(dst[y * W + dx - s], 20 + s * 2);
    if (dx < 0) for (int y = 0; y < H; y++) for (int s = 0; s < 6 && W + dx + s < W; s++) dst[y * W + W + dx + s] = dim565(dst[y * W + W + dx + s], 20 + s * 2);
    if (dy < 0) for (int s = 0; s < 6 && H + dy + s < H; s++) for (int x = 0; x < W; x++) dst[(H + dy + s) * W + x] = dim565(dst[(H + dy + s) * W + x], 20 + s * 2);
}
static void player_from_edge(int dir, int start)
{
    if (launcher_on) { render_launcher(img_under, now_ms()); }
    else memcpy(img_under, visible(), HM_FRAME_BYTES);
    have_under = 1;
    const uint16_t *ph = have_cache_plane ? img_cache_plane : 0;
    if (!ph) { memset(img_sheet, 0, HM_FRAME_BYTES); ph = img_sheet; }
    st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return;
    nav_start(NAV_TO_PLANE);
    int span = dir == 3 ? H : W, rev = 0; double vel = 0, lt = now_ms(); int last = start;
    struct pkt p;
    for (;;) {
        int got = read_pkt(&p, 0);
        while (got && p.down && read_pkt(&p, 0)) {}
        if (got && !p.down) break;
        int cur = dir == 3 ? cur_y : cur_x;
        double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur - last) / (t - lt); last = cur; lt = t; }
        rev = dir == 1 ? start - cur + 12 : cur - start + 12; if (rev < 0) rev = 0; if (rev > span) rev = span;
        int k = 32 - 11 * rev / span;
        int ready = nav_done && nav_ok;
        compose_slide(fr0, img_under, ready ? fr1 : ph, dir == 1 ? W - rev : dir == 2 ? rev - W : 0, dir == 3 ? rev - H : 0, k);
        present();
        if (!cur_down) break;
    }
    double v = dir == 1 ? -vel : vel;
    int complete = rev > span * 0.4 || v > 0.35;
    int from = rev, to = complete ? span : 0; double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 240.0; if (t > 1) t = 1;
        int r = from + (int)((to - from) * ease_out(t));
        int ready = nav_done && nav_ok;
        compose_slide(fr0, img_under, ready ? fr1 : ph, dir == 1 ? W - r : dir == 2 ? r - W : 0, dir == 3 ? r - H : 0, 32 - 11 * r / span);
        present();
        if (t >= 1) break;
    }
    int ok = nav_wait(2500);
    if (complete && ok == 1) {
        memcpy(img_cache_plane, fr1, HM_FRAME_BYTES); have_cache_plane = 1;
        if (launcher_on) { launcher_on = 0; home_publish(); }
        cmd('R');
        return;
    }
    if (ok == 1) { nav_start(NAV_BACK); nav_wait(2000); }
    if (!launcher_resume()) cmd('R');
}

/* ------------------------------------------------------------------ multitarea */
#define CARD_S 0.62f
/* tarjeta i en el carrusel (0 = donde estabas, 1 = Reproduciendo a su izquierda) */
static void card_geom(int i, double off, float s, double lift, int *x, int *y, int *w, int *h)
{
    *w = (int)(W * s); *h = (int)(H * s);
    double gap = W * CARD_S + 18;
    double cx = W / 2.0 + (off - i) * gap;
    *x = (int)(cx - *w / 2.0); *y = (int)((H - *h) / 2.0 + 14 - lift);
}
static void card_label(uint16_t *dst, int x, int y, const struct sprite *ic, const char *name)
{
    int iw = 22, tw = tw_n(name, 13, -1), tot = (ic ? iw + 6 : 0) + tw, x0 = x - tot / 2;
    if (ic) blit_scaled(dst, ic, x0, y - 26, iw, iw, 255);
    draw_text(dst, x0 + (ic ? iw + 6 : 0), y - 10, name, 13, 0xFFFF, 0, 0, H);
}
static int sw_player_first;   /* la tarjeta 0 es el reproductor (no hay app reciente) */
#define SW_HOLD_MS 1000   /* mantener el gesto de inicio este tiempo: multitarea */
static int sw_lift_card;      /* tarjeta que se levanta con "lift" (0 = la de la app) */
static void switcher_frame(const uint16_t *bg, const uint16_t *c0, const uint16_t *c1, double off, float s0, double lift, int show1, int labels)
{
    memcpy(fr0, bg, HM_FRAME_BYTES);
    const uint16_t *cards[2] = { c0, c1 };
    for (int i = 1; i >= 0; i--) {
        if (i == 1 && !show1) continue;
        float s = i == 0 ? s0 : CARD_S;
        int x, y, w, h; card_geom(i, off, s, i == sw_lift_card ? lift : 0, &x, &y, &w, &h);
        if (x > W || x + w < 0 || y + h < 0) continue;
        /* sombra barata: solo una franja difusa bajo y a los lados de la tarjeta */
        for (int k = 1; k <= 8; k++) {
            int yy = y + h + k - 2, a = 22 + k + (k * k) / 8; if (a > 32) a = 32;
            if (yy >= 0 && yy < H) for (int xx = x + 10; xx < x + w - 10; xx++) if (xx >= 0 && xx < W) fr0[yy * W + xx] = dim565(fr0[yy * W + xx], a);
        }
        draw_scaled(fr0, cards[i], x, y, w, h, 20 * s / CARD_S + (1 - s) * 10);
        if (labels) {
            int pl = i == 1 || sw_player_first;
            const struct sprite *ic = pl ? &apps[app_by_id("nowplaying")].icon[0] : (last_app >= 0 ? &apps[last_app].icon[0] : 0);
            card_label(fr0, x + w / 2, y - 4, ic, pl ? tr("Reproduciendo") : (last_app >= 0 ? app_label(last_app) : tr("HiBy")));
        }
    }
    present();
}
/* la tarjeta vuelve a ocupar la pantalla (abrirla) */
static void card_zoom_in(const uint16_t *bg, const uint16_t *img, int i, double off)
{
    int x0, y0, w0, h0; card_geom(i, off, CARD_S, 0, &x0, &y0, &w0, &h0);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 240.0; if (t > 1) t = 1;
        double e = ease_out(t);
        int w = (int)(w0 + (W - w0) * e), h = (int)(h0 + (H - h0) * e);
        int x = (int)(x0 + (0 - x0) * e), y = (int)(y0 + (0 - y0) * e);
        memcpy(fr0, bg, HM_FRAME_BYTES);
        draw_scaled(fr0, img, x, y, w, h, (float)(20 * (1 - e)));
        present();
        if (t >= 1) break;
    }
}
/* la tarjeta se va al inicio: se encoge hacia el icono de su app (o al centro) */
static void card_to_home(const uint16_t *home, const uint16_t *img, int x0, int y0, int w0, int h0)
{
    int tx = W / 2 - L.icon / 2, ty = H / 2 - L.icon / 2;
    if (last_app >= 0) {
        for (int i = 0; i < L.dn; i++) if (L.dock[i] == last_app) dock_xy(i, L.dn, &tx, &ty);
        for (int i = 0; i < L.pn[cur_page]; i++) if (L.page[cur_page][i] == last_app) cell_xy(i, &tx, &ty);
    }
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 280.0; if (t > 1) t = 1;
        double e = ease_out(t);
        int w = (int)(w0 + (L.icon - w0) * e), h = (int)(h0 + (L.icon - h0) * e);
        int x = (int)(x0 + (tx - x0) * e), y = (int)(y0 + (ty - y0) * e);
        int k = 22 + (int)(10 * e);
        for (int i = 0; i < W * H; i++) fr0[i] = k >= 32 ? home[i] : dim565(home[i], k);
        if (t < 0.92) {
            draw_scaled(fr0, img, x, y, w, h, win_radius(w, h, 1 - e));
            if (last_app >= 0) { int ia = (int)(255 * (e - 0.55) / 0.45); if (ia > 0) blit_scaled(fr0, &apps[last_app].icon[sz_idx()], x, y, w, h, ia > 255 ? 255 : ia); }
        }
        present();
        if (t >= 1) break;
    }
}


static void player_card(uint16_t *d)
{
    if (np_card(d)) return;                 /* la pagina real de Reproduciendo (nowui) */
    static uint16_t cov[128 * 128]; struct np_info ni; int cok = 0;
    npx_snapshot(&ni, cov, &cok);
    for (int i = 0; i < W * H; i++) d[i] = rgb(22, 22, 24);
    if (cok) npx_blit_cover(d, cov, 40, 70, 240, 16);
    else { int a = app_by_id("nowplaying"); if (a >= 0) blit_scaled(d, &apps[a].icon[2], W / 2 - 64, 120, 128, 128, 255); }
    const char *ti = ni.source && ni.title[0] ? ni.title : tr("Reproduciendo");
    int sz = 22; while (sz > 14 && tw_n(ti, sz, -1) > W - 60) sz--;
    draw_text(d, 30, 352, ti, sz, 0xFFFF, W - 60, 0, H);
    if (ni.source && ni.artist[0]) draw_text(d, 30, 384, ni.artist, 18, rgb(200, 200, 206), W - 60, 0, H);
}

/* ------------------------------------------------------------------ multitarea comun */
/* Carrusel con la app (tarjeta 0, en el centro) y el reproductor a su izquierda (tarjeta 1).
 * Deslizar de lado cambia de tarjeta; arrastrar la de la app hacia arriba la cierra (solo hay
 * una app abierta a la vez); tocar una tarjeta la abre (con su zoom); tocar fuera o subir desde
 * la barra: inicio. Devuelve SW_*; off: tarjeta centrada al salir. */
enum { SW_HOME, SW_APP, SW_PLAYER, SW_CLOSED };
static int sw_loop(const uint16_t *bg, const uint16_t *c0, const uint16_t *c1, int has_app, double *poff)
{
    double off = *poff; struct pkt p; int ncard = c1 ? 2 : 1, res = -1;
    while (res < 0) {
        sw_lift_card = 0;
        switcher_frame(bg, c0, c1, off, CARD_S, 0, c1 != 0, 1);
        if (!read_pkt(&p, 300) || !p.down) continue;
        int tx = p.x, ty = p.y, mode = 0, lx = tx, ly = ty, hit = -1;
        double o0 = off, vx = 0, vy = 0, lt = now_ms(), lift = 0;
        if (ty >= 456) { while (cur_down && read_pkt(&p, 1000)) {} res = SW_HOME; break; }
        for (int i = 0; i < ncard; i++) { int x, y, w, h; card_geom(i, off, CARD_S, 0, &x, &y, &w, &h); if (tx >= x && tx < x + w && ty >= y - 30 && ty < y + h) hit = i; }
        int closable = hit == 0 && has_app;
        while (cur_down && read_pkt(&p, 30)) {
            int dx = cur_x - tx, dy = cur_y - ty;
            if (!mode && (abs(dx) > 10 || abs(dy) > 10)) mode = abs(dx) >= abs(dy) ? 1 : (dy < 0 && hit >= 0 ? 2 : 3);
            double t = now_ms(); if (t > lt) { vx = 0.6 * vx + 0.4 * (cur_x - lx) / (t - lt); vy = 0.6 * vy + 0.4 * (cur_y - ly) / (t - lt); lx = cur_x; ly = cur_y; lt = t; }
            if (mode == 1 && ncard > 1) {
                off = o0 + dx / (W * CARD_S + 18.0); if (off < -0.25) off = -0.25; if (off > 1.25) off = 1.25;
                switcher_frame(bg, c0, c1, off, CARD_S, 0, 1, 1);
            } else if (mode == 2) {          /* la tarjeta sube con el dedo (la del reproductor, con goma) */
                lift = -dy; if (lift < 0) lift = 0; if (!closable) lift = 40 * (1 - exp(-lift / 80.0));
                sw_lift_card = hit; switcher_frame(bg, c0, c1, off, CARD_S, lift, c1 != 0, 1);
            }
        }
        if (now_ms() - lt > 80) { vx = 0; vy = 0; }
        if (mode == 2) {
            sw_lift_card = hit;
            int gone = closable && (lift > 110 || vy < -0.6);
            double from = lift, to = gone ? H : 0, t0 = now_ms(), dur = gone ? 200 : 220;
            for (;;) {
                double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
                double e = gone ? t * t : ease_out(t);
                switcher_frame(bg, c0, c1, off, CARD_S, from + (to - from) * e, c1 != 0, 1);
                if (t >= 1) break;
            }
            sw_lift_card = 0;
            if (gone) { lg("multitarea: app cerrada"); res = SW_CLOSED; }
            continue;
        }
        if (mode == 1 && ncard > 1) {
            double target = (off > 0.5 || vx > 0.3) && !(vx < -0.3) ? 1 : 0, f = off, t1 = now_ms();
            for (;;) { double t = (now_ms() - t1) / 200.0; if (t > 1) t = 1; off = f + (target - f) * ease_out(t); switcher_frame(bg, c0, c1, off, CARD_S, 0, 1, 1); if (t >= 1) break; }
            continue;
        }
        if (mode) continue;
        if (hit < 0) { res = SW_HOME; break; }
        card_zoom_in(bg, hit == 0 ? c0 : c1, hit, off);
        res = hit == 0 && has_app ? SW_APP : SW_PLAYER;
    }
    sw_lift_card = 0; *poff = off;
    return res;
}
/* sin la tarjeta cerrada: el inicio aparece (el fondo oscurecido se aclara) */
static void sw_reveal_home(const uint16_t *home, int k0)
{
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 200.0; if (t > 1) t = 1;
        int k = k0 + (int)((32 - k0) * ease_out(t));
        for (int i = 0; i < W * H; i++) fr0[i] = k >= 32 ? home[i] : dim565(home[i], k);
        present(); if (t >= 1) break;
    }
}

static void app_switch_gesture(int sy, int sx)
{
    sw_player_first = 0;
    uint16_t *app = img_sheet, *home = img_base, *bg = img_app, *bg2 = img_under;
    memcpy(app, visible(), HM_FRAME_BYTES);
    if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
        memcpy(app + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    if (launcher_enabled) render_launcher(home, now_ms()); else if (have_cache_home) memcpy(home, img_cache_home, HM_FRAME_BYTES); else memset(home, 0, HM_FRAME_BYTES);
    for (int i = 0; i < W * H; i++) { bg[i] = dim565(home[i], 22); bg2[i] = dim565(home[i], 11); }
    have_under = 0;
    /* tarjeta del reproductor (el propio): se prepara solo si se mantiene el gesto */
    static uint16_t *ph_card; if (!ph_card) ph_card = malloc(W * H * 2);
    const uint16_t *ph = 0;
    st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return;
    struct pkt p; double vel = 0, lt = now_ms(), still_t = 0; int ly = sy, lx = sx, armed = 0; double prog = 0, arm_t = 0;
    /* velocidad real: posicion de hace ~80 ms (historial corto) */
    double ht[16]; int hy[16], hn = 0;
    for (;;) {
        int got = read_pkt(&p, 0);
        while (got && p.down && read_pkt(&p, 0)) {}
        if (got && !p.down) break;
        double t = now_ms();
        if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lx = cur_x; lt = t; }
        ht[hn % 16] = t; hy[hn % 16] = cur_y; hn++;
        double spd = 0;
        for (int i = 1; i < 16 && i < hn; i++) { int j = (hn - 1 - i) % 16; if (t - ht[j] >= 80) { spd = abs(cur_y - hy[j]) / (t - ht[j]); break; } }
        prog = (sy - cur_y) / 300.0; if (prog < 0) prog = 0; if (prog > 1.2) prog = 1.2;
        /* mantener la tarjeta quieta 1 s = multitarea (antes se asoma el reproductor) */
        if (!armed && prog > 0.12 && hn > 3 && spd < 0.15) { if (!still_t) still_t = t; else if (t - still_t > SW_HOLD_MS) { armed = 1; arm_t = t; lg("multitarea armada"); } }
        else if (spd >= 0.15) still_t = 0;
        float s = 1 - 0.40f * (float)(prog > 1 ? 1 : prog);
        double a = armed ? (t - arm_t) / 200.0 : 0; if (a > 1) a = 1;
        if (!ph && ph_card && (armed || (still_t && t - still_t > 200))) { player_card(ph_card); ph = ph_card; }
        int peek = ph && (armed || (still_t && t - still_t > 300));
        /* la tarjeta sigue al dedo (tambien de lado) */
        double off = (lx - sx) / (W * CARD_S + 18.0);
        switcher_frame(armed && a > 0.5 ? bg2 : bg, app, ph, off, s, (sy - cur_y) * 0.35, peek, armed && a >= 1);
        if (!cur_down) break;
    }
    if (armed) {
        /* asentar en la multitarea y elegir */
        double off = 0;
        { double t0 = now_ms(); int ly0 = sy - cur_y; for (;;) { double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1; double e = ease_out(t);
            float s = (float)((1 - 0.40 * (prog > 1 ? 1 : prog)) + (CARD_S - (1 - 0.40 * (prog > 1 ? 1 : prog))) * e);
            switcher_frame(bg2, app, ph, off, s, ly0 * 0.35 * (1 - e), 1, 1); if (t >= 1) break; } }
        lg("multitarea");
        int r = sw_loop(bg2, app, ph, 1, &off);
        if (r == SW_APP) { cmd('R'); lg("multitarea: donde estabas"); return; }
        if (r == SW_HOME) {
            int x, y, w, h; card_geom(off < 0.5 ? 0 : 1, off, CARD_S, 0, &x, &y, &w, &h);
            nav_start(NAV_HOME); card_to_home(home, off < 0.5 ? app : ph, x, y, w, h);
            nav_wait(3000); if (!launcher_resume()) cmd('R'); return;
        }
        if (r == SW_CLOSED) {   /* cerrar la app de HiBy: el reproductor vuelve a su inicio */
            nav_start(NAV_HOME); sw_reveal_home(home, 11); last_app = -1;
            nav_wait(3000); if (!launcher_resume()) cmd('R'); return;
        }
        /* reproductor: el propio (la app de HiBy se cierra: solo una a la vez) */
        nav_start(NAV_HOME);
        int rr = np_app_run(home, -1, -1);
        nav_wait(3000);
        (void)rr;
        if (!launcher_resume()) cmd('R');
        return;
    }
    if (prog > 0.28 || vel < -0.5) {
        lg("gesto: inicio");
        float s = 1 - 0.40f * (float)(prog > 1 ? 1 : prog);
        int x, y, w, h; card_geom(0, (lx - sx) / (W * CARD_S + 18.0), s, (sy - cur_y) * 0.35, &x, &y, &w, &h);
        nav_start(NAV_HOME); card_to_home(home, app, x, y, w, h);
        nav_wait(3000); if (!launcher_resume()) cmd('R');
        return;
    }
    /* no llega: vuelve a su sitio */
    double t0 = now_ms(), p0 = prog, o0 = (lx - sx) / (W * CARD_S + 18.0);
    for (;;) {
        double t = (now_ms() - t0) / 200.0; if (t > 1) t = 1;
        double e = ease_out(t), pr = p0 * (1 - e);
        switcher_frame(bg, app, ph, o0 * (1 - e), 1 - 0.40f * (float)(pr > 1 ? 1 : pr), (sy - cur_y) * 0.35 * (1 - e), 0, 0);
        if (t >= 1) break;
    }
    cmd('R');
}

/* ------------------------------------------------------------------ atras (estilo Xiaomi / Android) */
/* Burbuja oscura que nace del borde siguiendo al dedo (perfil de campana, bordes suaves) con
 * una flecha que se marca al pasar el umbral. Al soltar armada: la pantalla anterior entra
 * con transicion completa (la actual sale hacia el lado del gesto, la nueva con paralaje).
 * Solo se redibujan las filas de la burbuja (present_rows): el coste por fotograma es minimo. */
#define BK_HH 96          /* media altura de la burbuja */
#define BK_WMAX 34.f      /* ancho maximo */
#define BK_ARM 64         /* recorrido del dedo para armar */
static void back_bubble(uint16_t *d, int right, int cy, float w, float arrow, int armed)
{
    if (w < 0.5f) return;
    for (int y = cy - BK_HH; y <= cy + BK_HH; y++) {
        if (y < 30 || y >= H) continue;
        float t = (y - cy) / (float)BK_HH, prof = 0.5f + 0.5f * cosf(3.14159f * t);
        float ww = w * prof; int full = (int)ww; float frac = ww - full;
        uint16_t *row = d + y * W;
        for (int i = 0; i <= full && i < W; i++) {
            int x = right ? W - 1 - i : i;
            /* gris translucido: oscurece lo claro y aclara lo oscuro (se ve sobre cualquier fondo) */
            int k = i < full ? 26 : (int)(26 * frac);
            uint16_t q = row[x];
            int r = ((q >> 11) * (32 - k) + 7 * k) >> 5, g = (((q >> 5) & 63) * (32 - k) + 15 * k) >> 5, bl = ((q & 31) * (32 - k) + 8 * k) >> 5;
            row[x] = (uint16_t)(r << 11 | g << 5 | bl);
        }
    }
    if (w > 10 && arrow > 0.02f) {             /* flecha "<" o ">" (apunta al borde) */
        float ax = w * 0.52f; int a = (int)(255 * arrow); if (a > 255) a = 255;
        uint16_t col = armed ? 0xFFFF : rgb(200, 200, 205);
        for (int i = 0; i <= 7; i++) {
            float fx = ax - 3.5f + i * 0.55f;
            float x = right ? W - 1 - fx : fx;
            fill_circle(d, x, cy - i, 1.15f, col, a); fill_circle(d, x, cy + i, 1.15f, col, a);
        }
    }
}
static float back_w(float dist) { return BK_WMAX * (1 - expf(-dist / 70.f)); }      /* goma */
static void back_frame(const uint16_t *old, int right, int cy, float w, float arrow, int armed, int *py0, int *py1)
{
    int y0 = cy - BK_HH - 2, y1 = cy + BK_HH + 3; if (y0 < 30) y0 = 30; if (y1 > H) y1 = H;
    int a0 = y0 < *py0 ? y0 : *py0, a1 = y1 > *py1 ? y1 : *py1;          /* union con la anterior */
    memcpy(fr0 + a0 * W, old + a0 * W, (a1 - a0) * W * 2);
    back_bubble(fr0, right, cy, w, arrow, armed);
    present_rows(a0, a1);
    *py0 = y0; *py1 = y1;
}
/* transicion de vuelta: right=0 (desde la izquierda): la actual sale hacia la derecha */
static void back_transition(const uint16_t *old, const uint16_t *neu, int right)
{
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 270.0; if (t > 1) t = 1;
        float e = (float)ease_out(t);
        int off = (int)(W * e);
        int tx = right ? -off : off;                       /* x de la pantalla que se va */
        int bx = right ? (W - off) / 3 : -(W - off) / 3;     /* la nueva, con paralaje */
        int k = 22 + (int)(10 * e);
        memcpy(fr0, neu, 30 * W * 2);
        for (int y = 30; y < H; y++) {
            uint16_t *dr = fr0 + y * W; const uint16_t *o = old + y * W, *n = neu + y * W;
            for (int x = 0; x < W; x++) {
                int ox = x - tx;
                if (ox >= 0 && ox < W) dr[x] = o[ox];
                else { int nx = x - bx; dr[x] = nx >= 0 && nx < W ? dim565(n[nx], k) : 0; }
            }
            int ex = right ? W + tx : tx;                  /* sombra en el borde de la que se va */
            for (int s = 1; s <= 5; s++) { int x = right ? ex + s - 1 : ex - s; if (x >= 0 && x < W) dr[x] = dim565(dr[x], 20 + s * 2); }
        }
        present();
        if (t >= 1) break;
    }
}
static void back_gesture(int right, int sx, int sy)
{
    uint16_t *old = img_sheet, *neu = img_app;
    memcpy(old, visible(), HM_FRAME_BYTES);
    if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
        memcpy(old + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    have_under = 0;
    st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return;
    memcpy(fr0, old, HM_FRAME_BYTES);
    struct pkt p; int py0 = H, py1 = 0, armed = 0, cy = sy; float w = 0, ar = 0;
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        if (!got) { msleep(3); if (!cur_down) break; continue; }
        float dist = right ? sx - cur_x : cur_x - sx; if (dist < 0) dist = 0;
        cy = cur_y < 30 + BK_HH / 2 ? 30 + BK_HH / 2 : cur_y > H - BK_HH / 2 ? H - BK_HH / 2 : cur_y;
        armed = dist > BK_ARM; w = back_w(dist); ar = dist / BK_ARM; if (ar > 1) ar = 1;
        back_frame(old, right, cy, w, ar, armed, &py0, &py1);
        if (!p.down) break;
    }
    lg("atras: %s", armed ? "si" : "no");
    if (armed) nav_start(NAV_BACK);
    double t0 = now_ms(); float w0 = w;                   /* la burbuja se recoge */
    for (;;) {
        double t = (now_ms() - t0) / 120.0; if (t > 1) t = 1;
        back_frame(old, right, cy, w0 * (1 - (float)ease_out(t)), ar * (float)(1 - t), armed, &py0, &py1);
        if (t >= 1) break;
    }
    if (!armed) { cmd('R'); return; }
    int ok = nav_wait(1500);
    memcpy(neu, fr1, HM_FRAME_BYTES);
    int home = is_home(neu) && launcher_enabled;
    if (home) render_launcher(neu, now_ms());
    if (ok == 1) back_transition(old, neu, right);
    if (!home || !launcher_resume()) cmd('R');
}


/* ------------------------------------------------------------------ atras en las apps propias */
/* Zona comun de "atras" (apps de HiBy y apps propias, los dos bordes): el dedo tiene que poder
 * empezar algo dentro de la pantalla (la tactil apenas da x < 6 o x > 313), y un deslizamiento
 * claramente horizontal hacia dentro tambien vale desde mas lejos. */
#define BK_EDGE 34
#define BK_WIDE 72
/* 0 nada; 1/2 borde izquierdo/derecho; 3/4 zona ancha izquierda/derecha */
static int back_zone(int sx, int sy)
{
    if (sy < 30 || sy >= 456) return 0;
    if (sx < BK_WIDE) return sx < BK_EDGE ? 1 : 3;
    if (sx >= W - BK_WIDE) return sx >= W - BK_EDGE ? 2 : 4;
    return 0;
}
/* Llamar al bajar el dedo en una app propia, con fr0 = lo que hay en pantalla. Decide si es el
 * gesto de atras (deslizar hacia dentro desde un borde), dibuja la burbuja siguiendo al dedo y
 * devuelve 1 si se solto armada (la app hace su "volver" con su transicion), -1 si era el gesto
 * pero se cancelo (toque consumido) y 0 si no era el gesto: la app sigue con el toque (cur_x,
 * cur_y, cur_down siguen al dia; si ya se solto, era un toque). */
static int app_back_gesture(int sx, int sy)
{
    int z = back_zone(sx, sy); if (!z) return 0;
    int right = z == 2 || z == 4, wide = z >= 3;
    struct pkt p; double t0 = now_ms(); int decided = 0;
    while (!decided) {
        if (!cur_down) return 0;
        int dx = cur_x - sx, dy = cur_y - sy, along = right ? -dx : dx;
        if (along > (wide ? 16 : 10) && along > abs(dy) * (wide ? 2 : 1.2f)) { decided = 1; break; }
        if (abs(dy) > 14 || along < -8) return 0;
        if (now_ms() - t0 > 600) return 0;                   /* quieto: pulsacion normal */
        read_pkt(&p, 30);
    }
    static uint16_t *old; if (!old) old = malloc(HM_FRAME_BYTES);
    if (!old) { drain_until_up(); return 1; }
    memcpy(old, fr0, HM_FRAME_BYTES);
    int py0 = H, py1 = 0, armed = 0, cy = sy; float w = 0, ar = 0;
    while (cur_down) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        float dist = right ? sx - cur_x : cur_x - sx; if (dist < 0) dist = 0;
        if (wide) dist -= 8;                                   /* el arranque ya conto */
        if (dist < 0) dist = 0;
        cy = cur_y < 30 + BK_HH / 2 ? 30 + BK_HH / 2 : cur_y > H - BK_HH / 2 ? H - BK_HH / 2 : cur_y;
        armed = dist > BK_ARM; w = back_w(dist); ar = dist / BK_ARM; if (ar > 1) ar = 1;
        back_frame(old, right, cy, w, ar, armed, &py0, &py1);
        if (got && !p.down) break;
        if (!got) msleep(3);
    }
    double tc = now_ms(); float w0 = w;                       /* la burbuja se recoge */
    for (;;) {
        double t = (now_ms() - tc) / 120.0; if (t > 1) t = 1;
        back_frame(old, right, cy, w0 * (1 - (float)ease_out(t)), ar * (float)(1 - t), armed, &py0, &py1);
        if (t >= 1) break;
    }
    memcpy(fr0, old, HM_FRAME_BYTES);
    lg("atras (app): %s", armed ? "si" : "no");
    return armed ? 1 : -1;
}

/* ------------------------------------------------------------------ multitarea desde el inicio */
/* Deslizar hacia arriba desde la barra en el inicio (iOS): las tarjetas de la ultima app y del
 * reproductor suben desde abajo siguiendo al dedo; al soltar a media altura quedan en el
 * carrusel; tocar una la abre; tocar fuera o deslizar de nuevo vuelve al inicio. */
static void launch_app(int a, int ix, int iy);
static void home_switcher(int sy)
{
    uint16_t *home = img_base, *bg = img_under;
    static uint16_t *appc; if (!appc) appc = malloc(HM_FRAME_BYTES);
    if (!appc) return;
    render_launcher(home, now_ms());
    int have_app = 0;
    if (last_app >= 0 && last_app == own_card_app && own_card) { memcpy(appc, own_card, HM_FRAME_BYTES); have_app = 1; }   /* app propia */
    else if (last_app >= 0 && app_cache(last_app, 0)) { memcpy(appc, img_app, HM_FRAME_BYTES); have_app = 1; }
    const uint16_t *ph = 0;   /* siempre el reproductor propio */
    static uint16_t *ph_card;
    if (!ph) {
        if (!ph_card) ph_card = malloc(W * H * 2);
        if (ph_card) player_card(ph_card);
        ph = ph_card;
    }
    /* sin ultima app: el reproductor es la unica tarjeta (en la posicion 0) */
    const uint16_t *c0 = have_app ? appc : ph, *c1 = have_app ? ph : 0;
    sw_player_first = !have_app;
    have_under = 0;
    struct pkt p; double vel = 0, lt = now_ms(); int ly = sy; double prog = 0;
    for (;;) {
        int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
        double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lt = t; }
        prog = (sy - cur_y) / 260.0; if (prog < 0) prog = 0; if (prog > 1.1) prog = 1.1;
        int k = 32 - (int)(21 * (prog > 1 ? 1 : prog));
        for (int i = 0; i < W * H; i++) bg[i] = dim565(home[i], k);
        switcher_frame(bg, c0, c1, 0, CARD_S, -(1 - (prog > 1 ? 1 : prog)) * H * 0.75, c1 != 0, prog > 0.8);
        if (got && !p.down) break;
        if (!cur_down) break;
        if (!got) msleep(2);
    }
    int open = prog > 0.3 || vel < -0.45;
    double t0 = now_ms(), p0 = prog;
    for (;;) {
        double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1;
        double e = ease_out(t), pr = open ? p0 + (1 - p0) * e : p0 * (1 - e); if (pr > 1) pr = 1;
        int k = 32 - (int)(21 * pr);
        for (int i = 0; i < W * H; i++) bg[i] = dim565(home[i], k);
        switcher_frame(bg, c0, c1, 0, CARD_S, -(1 - pr) * H * 0.75, c1 != 0, open && t >= 1);
        if (t >= 1) break;
    }
    if (!open) { sw_player_first = 0; launcher_present(); return; }
    lg("multitarea (inicio)");
    double off = 0;
    int r = sw_loop(bg, c0, c1, have_app, &off);
    sw_player_first = 0;
    if (r == SW_APP) { launch_app(last_app, W / 2 - L.icon / 2, H / 2 - L.icon / 2); return; }
    if (r == SW_PLAYER) { np_app_run(home, -1, -1); launcher_present(); return; }
    if (r == SW_CLOSED) { last_app = -1; own_card_app = -1; sw_reveal_home(home, 11); launcher_present(); return; }
    /* volver al inicio: las tarjetas bajan */
    double t1 = now_ms();
    for (;;) {
        double t = (now_ms() - t1) / 220.0; if (t > 1) t = 1; double e = ease_out(t);
        int k = 11 + (int)(21 * e);
        for (int i = 0; i < W * H; i++) bg[i] = dim565(home[i], k);
        switcher_frame(bg, c0, c1, off, CARD_S, -e * H * 0.75, c1 != 0, 0);
        if (t >= 1) break;
    }
    launcher_present();
}

/* ------------------------------------------------------------------ despacho */
/* Llamado al empezar un toque fuera del launcher. Devuelve 1 si era un gesto del sistema. */
static int edge_dispatch(struct pkt *p, const uint16_t *v, int plane)
{
    int sx = p->x, sy = p->y, zone = 0;
    /* bordes de 24 px; atras tambien desde cerca del borde izquierdo (60 px) si el gesto es
     * claramente hacia la derecha: asi el "atras" antiguo de HiBy nunca llega a dispararse */
    int wide = 0;
    if (sy >= 456) zone = 1; else if (sy < 30) zone = 3; else if (sx < BK_EDGE) zone = 4; else if (sx >= W - BK_EDGE) zone = 5;
    else if (sx < BK_WIDE && G.left == GA_BACK) { zone = 4; wide = 1; }
    else if (sx >= W - BK_WIDE && G.right == GA_BACK) { zone = 5; wide = 1; }
    if (!zone) return 0;
    int act = zone == 1 ? (plane ? GA_HOME : G.bottom_app) : zone == 3 ? (sx < W / 2 ? G.top : G.top_r) : zone == 4 ? G.left : G.right;
    if (zone == 1 && is_cc(v)) return 0;                     /* cerrar el centro de control: es de HiBy */
    if (act == GA_NONE) return 0;
    /* retener el toque hasta saber si es el gesto (alejarse del borde) o un toque normal */
    struct pkt held[16]; int nh = 0; held[nh++] = *p; int decided = 0;
    while (!decided && read_pkt(p, 1000)) {
        if (!p->down) { decided = 1; break; }
        int dx = p->x - sx, dy = p->y - sy;
        int along = zone == 1 ? -dy : zone == 3 ? dy : zone == 4 ? dx : -dx;
        int across = zone == 1 || zone == 3 ? abs(dx) : abs(dy);
        if (along > (wide ? 18 : 12) && along > across * (wide ? 2 : 1)) { decided = 2; break; }
        if (across > 24 || along < -10) { decided = 1; break; }
        if (nh < 16) held[nh++] = *p; else { decided = 1; break; }
    }
    if (decided != 2) { for (int i = 0; i < nh; i++) fwd(&held[i]); if (p->n) fwd(p); return 1; }
    if (plane && zone == 1) { plane_home_drag(sy); drain_until_up(); update_overlay(); return 1; }
    if (act == GA_BACK) back_gesture(zone == 5, sx, sy);
    else if (act == GA_HOME) app_switch_gesture(sy, sx);
    else if (act == GA_PLAYER) { if (zone == 1) sheet_open_drag(sy); else player_from_edge(zone == 5 ? 1 : zone == 4 ? 2 : 3, zone == 3 ? sy : sx); }
    else if (act == GA_CC) { if (zone == 3) cc_open(sy); else { drain_until_up(); cc_open(-1); } }
    drain_until_up();
    update_overlay();
    return 1;
}
