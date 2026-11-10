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
static void gestures_conf_line(char *k, char *v)
{
    int a = ga_parse(v);
    if (!k || !v) return;
    if (!strcmp(k, "chevron")) { G.chevron = atoi(v) != 0; return; }
    if (a < 0) return;
    if (!strcmp(k, "bottom_app") && a != GA_BACK) G.bottom_app = a;
    else if (!strcmp(k, "bottom_home") && a != GA_BACK && a != GA_HOME) G.bottom_home = a;
    else if (!strcmp(k, "top") && a != GA_HOME && a != GA_BACK) G.top = a;
    else if (!strcmp(k, "left") && a != GA_HOME) G.left = a;
    else if (!strcmp(k, "right") && a != GA_HOME && a != GA_BACK) G.right = a;
}
static void gestures_conf_save(FILE *f)
{
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
static void switcher_frame(const uint16_t *bg, const uint16_t *c0, const uint16_t *c1, double off, float s0, double lift, int show1, int labels)
{
    memcpy(fr0, bg, HM_FRAME_BYTES);
    const uint16_t *cards[2] = { c0, c1 };
    for (int i = 1; i >= 0; i--) {
        if (i == 1 && !show1) continue;
        float s = i == 0 ? s0 : CARD_S;
        int x, y, w, h; card_geom(i, off, s, i == 0 ? lift : 0, &x, &y, &w, &h);
        if (x > W || x + w < 0) continue;
        /* sombra barata: solo una franja difusa bajo y a los lados de la tarjeta */
        for (int k = 1; k <= 8; k++) {
            int yy = y + h + k - 2, a = 22 + k + (k * k) / 8; if (a > 32) a = 32;
            if (yy >= 0 && yy < H) for (int xx = x + 10; xx < x + w - 10; xx++) if (xx >= 0 && xx < W) fr0[yy * W + xx] = dim565(fr0[yy * W + xx], a);
        }
        draw_scaled(fr0, cards[i], x, y, w, h, 20 * s / CARD_S + (1 - s) * 10);
        if (labels) {
            const struct sprite *ic = i == 1 ? &apps[app_by_id("nowplaying")].icon[0] : (last_app >= 0 ? &apps[last_app].icon[0] : 0);
            card_label(fr0, x + w / 2, y - 4, ic, i == 1 ? tr("Reproduciendo") : (last_app >= 0 ? apps[last_app].label : tr("HiBy")));
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

static void app_switch_gesture(int sy, int sx)
{
    uint16_t *app = img_sheet, *home = img_base, *bg = img_app, *bg2 = img_under;
    memcpy(app, visible(), HM_FRAME_BYTES);
    if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
        memcpy(app + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    if (launcher_enabled) render_launcher(home, now_ms()); else if (have_cache_home) memcpy(home, img_cache_home, HM_FRAME_BYTES); else memset(home, 0, HM_FRAME_BYTES);
    for (int i = 0; i < W * H; i++) { bg[i] = dim565(home[i], 22); bg2[i] = dim565(home[i], 11); }
    have_under = 0;
    const uint16_t *ph = have_cache_plane ? img_cache_plane : 0;
    /* sin imagen guardada del reproductor: tarjeta oscura con su icono grande en el centro */
    static uint16_t *ph_card;
    if (!ph) {
        if (!ph_card) ph_card = malloc(W * H * 2);
        if (ph_card) {
            for (int i = 0; i < W * H; i++) ph_card[i] = rgb(22, 22, 24);
            int a = app_by_id("nowplaying");
            if (a >= 0) blit_scaled(ph_card, &apps[a].icon[2], W / 2 - 48, H / 2 - 70, 96, 96, 255);
            int tw = tw_n(tr("Reproduciendo"), 20, -1);
            draw_text(ph_card, (W - tw) / 2, H / 2 + 62, tr("Reproduciendo"), 20, rgb(235, 235, 240), 0, 0, H);
        }
        ph = ph_card;
    }
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
        /* mantener quieto a media altura = multitarea */
        if (!armed && prog > 0.16 && hn > 3 && spd < 0.15) { if (!still_t) still_t = t; else if (t - still_t > 120) { armed = 1; arm_t = t; lg("multitarea armada"); } }
        else if (spd >= 0.15) still_t = 0;
        float s = 1 - 0.40f * (float)(prog > 1 ? 1 : prog);
        double a = armed ? (t - arm_t) / 200.0 : 0; if (a > 1) a = 1;
        /* la tarjeta sigue al dedo (tambien de lado) */
        double off = (lx - sx) / (W * CARD_S + 18.0);
        switcher_frame(armed && a > 0.5 ? bg2 : bg, app, ph, off, s, (sy - cur_y) * 0.35, armed, armed && a >= 1);
        if (!cur_down) break;
    }
    if (armed) {
        /* asentar en la multitarea y elegir */
        double off = 0;
        { double t0 = now_ms(); int ly0 = sy - cur_y; for (;;) { double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1; double e = ease_out(t);
            float s = (float)((1 - 0.40 * (prog > 1 ? 1 : prog)) + (CARD_S - (1 - 0.40 * (prog > 1 ? 1 : prog))) * e);
            switcher_frame(bg2, app, ph, off, s, ly0 * 0.35 * (1 - e), 1, 1); if (t >= 1) break; } }
        lg("multitarea");
        for (;;) {
            switcher_frame(bg2, app, ph, off, CARD_S, 0, 1, 1);
            if (!read_pkt(&p, 300) || !p.down) continue;
            int tx = p.x, ty = p.y; double o0 = off, v2 = 0, l2 = now_ms(); int lx2 = tx, moved = 0;
            if (ty >= 456) {   /* desde abajo otra vez: inicio */
                while (cur_down && read_pkt(&p, 1000)) {}
                int x, y, w, h; card_geom(0, off, CARD_S, 0, &x, &y, &w, &h);
                nav_start(NAV_HOME); card_to_home(home, off < 0.5 ? app : ph, x, y, w, h);
                nav_wait(3000); if (!launcher_resume()) cmd('R'); return;
            }
            while (cur_down && read_pkt(&p, 30)) {
                if (abs(cur_x - tx) > 8) moved = 1;
                double t = now_ms(); if (t > l2) { v2 = 0.6 * v2 + 0.4 * (cur_x - lx2) / (t - l2); lx2 = cur_x; l2 = t; }
                if (moved) { off = o0 + (cur_x - tx) / (W * CARD_S + 18.0); if (off < -0.25) off = -0.25; if (off > 1.25) off = 1.25; switcher_frame(bg2, app, ph, off, CARD_S, 0, 1, 1); }
            }
            if (moved) {   /* encajar en una tarjeta */
                double target = (off > 0.5 || v2 > 0.3) && !(v2 < -0.3) ? 1 : 0;
                double f = off, t0 = now_ms();
                for (;;) { double t = (now_ms() - t0) / 200.0; if (t > 1) t = 1; off = f + (target - f) * ease_out(t); switcher_frame(bg2, app, ph, off, CARD_S, 0, 1, 1); if (t >= 1) break; }
                continue;
            }
            /* toque: en una tarjeta = abrirla; fuera = inicio */
            int hit = -1;
            for (int i = 0; i < 2; i++) { int x, y, w, h; card_geom(i, off, CARD_S, 0, &x, &y, &w, &h); if (tx >= x && tx < x + w && ty >= y - 30 && ty < y + h) hit = i; }
            if (hit < 0) {
                int x, y, w, h; card_geom(0, off, CARD_S, 0, &x, &y, &w, &h);
                nav_start(NAV_HOME); card_to_home(home, app, x, y, w, h);
                nav_wait(3000); if (!launcher_resume()) cmd('R'); return;
            }
            if (hit == 0) { card_zoom_in(bg2, app, 0, off); cmd('R'); lg("multitarea: donde estabas"); return; }
            nav_start(NAV_TO_PLANE);
            card_zoom_in(bg2, ph, 1, off);
            int ok = nav_wait(3000);
            if (ok == 1) { memcpy(img_cache_plane, fr1, HM_FRAME_BYTES); have_cache_plane = 1; }
            cmd('R'); lg("multitarea: reproductor (%s)", ok == 1 ? "ok" : "fallo");
            return;
        }
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

/* ------------------------------------------------------------------ despacho */
/* Llamado al empezar un toque fuera del launcher. Devuelve 1 si era un gesto del sistema. */
static int edge_dispatch(struct pkt *p, const uint16_t *v, int plane)
{
    int sx = p->x, sy = p->y, zone = 0;
    if (sy >= 456) zone = 1; else if (sy < 14) zone = 3; else if (sx < 10) zone = 4; else if (sx > W - 10) zone = 5;
    if (!zone) return 0;
    int act = zone == 1 ? (plane ? GA_HOME : G.bottom_app) : zone == 3 ? G.top : zone == 4 ? G.left : G.right;
    if (zone == 1 && is_cc(v)) return 0;                     /* cerrar el centro de control: es de HiBy */
    if ((zone == 4 && act == GA_BACK) || act == GA_NONE) return 0;   /* lo hace HiBy */
    /* retener el toque hasta saber si es el gesto (alejarse del borde) o un toque normal */
    struct pkt held[16]; int nh = 0; held[nh++] = *p; int decided = 0;
    while (!decided && read_pkt(p, 1000)) {
        if (!p->down) { decided = 1; break; }
        int dx = p->x - sx, dy = p->y - sy;
        int along = zone == 1 ? -dy : zone == 3 ? dy : zone == 4 ? dx : -dx;
        int across = zone == 1 || zone == 3 ? abs(dx) : abs(dy);
        if (along > 12 && along > across) { decided = 2; break; }
        if (across > 24 || along < -10) { decided = 1; break; }
        if (nh < 16) held[nh++] = *p; else { decided = 1; break; }
    }
    if (decided != 2) { for (int i = 0; i < nh; i++) fwd(&held[i]); if (p->n) fwd(p); return 1; }
    if (plane && zone == 1) { plane_home_drag(sy); drain_until_up(); update_overlay(); return 1; }
    if (act == GA_HOME) app_switch_gesture(sy, sx);
    else if (act == GA_PLAYER) { if (zone == 1) sheet_open_drag(sy); else player_from_edge(zone == 5 ? 1 : zone == 4 ? 2 : 3, zone == 3 ? sy : sx); }
    else if (act == GA_CC) { if (zone == 3) cc_open(sy); else { drain_until_up(); cc_open(-1); } }
    drain_until_up();
    update_overlay();
    return 1;
}
