/* Radio: toques, busqueda con teclado y bucle de la app (el mismo esquema que Vinyl). */
static int ra_conf_dirty;

static int ra_poll(void) { int ch = ra_net_poll(); ch |= ra_pl_sync(); return ch; }
/* pide la pagina siguiente cuando el final de la lista se acerca */
static void ra_page_more(void)
{
    int pg = ra_stack[ra_sp], slot = pg == RG_LIST ? 1 : pg == RG_ROOT && ra_edit_e > 0.5f ? 0 : -1;
    if (slot < 0) return;
    struct ra_list *l = &ra_ls[slot];
    if (l->state == 2 && l->more && ra_pos[pg] > ra_maxpos(pg) - 500) ra_list_more(slot);
}

/* ------------------------------------------------------------------ paginas: entrar y volver */
static void ra_push(int pg)
{
    if (ra_sp >= 3) return;
    memcpy(fm_pa, fr0, HM_FRAME_BYTES);              /* lo que hay en pantalla, con la fila aun marcada */
    fm_press_kind = 0; fm_press_idx = -1;
    ra_stack[++ra_sp] = pg; ra_pos[pg] = 0; ra_vel = 0;
    ra_render_page(fm_pb, pg, 0);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 320.0; if (t > 1) t = 1;
        fm_compose(fm_pa, fm_pb, (int)(W * (1 - ease_out(t))));
        present(); if (t >= 1) break;
    }
}
static void ra_pop_from(int x0)
{
    if (ra_sp <= 0) return;
    fm_press_kind = 0; fm_press_idx = -1;
    ra_render_page(fm_pb, ra_stack[ra_sp], 0);
    ra_render_page(fm_pa, ra_stack[ra_sp - 1], 0);
    double t0 = now_ms(), ms = 260.0 * (W - x0) / W + 60;
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1;
        fm_compose(fm_pa, fm_pb, x0 + (int)((W - x0) * ease_out(t)));
        present(); if (t >= 1) break;
    }
    ra_sp--; ra_vel = 0;
}

/* ------------------------------------------------------------------ busqueda */
static void ra_search_now(void)
{
    ra_q_typed = 0;
    if (!strcmp(ra_q, ra_q_sent)) return;
    snprintf(ra_q_sent, sizeof ra_q_sent, "%s", ra_q);
    ra_pos[RG_ROOT] = 0; ra_vel = 0;
    const char *q = ra_q; while (*q == ' ') q++;
    if (strlen(q) < 2) { ra_list_reset(0); return; }
    /* primero lo que hay en la lista europea (sin red), luego el directorio si esta activado */
    struct ra_list *l = &ra_ls[0];
    ra_list_reset(0);
    l->kind = RL_SEARCH; snprintf(l->arg, sizeof l->arg, "%s", q); l->title[0] = 0;
    if (!ra_list_alloc(l)) return;
    int eu = 0; for (int i = 0; i < ra_sr.n; i++) if (ra_sr.s[i].type == RSRC_EU && ra_sr.s[i].on) eu = 1;
    for (int k = 0; eu && k < RA_NEU && l->n < RA_MAXST; k++) {
        struct ra_station st; ra_eu_station(k, &st);
        if (strcasestr(st.name, q) || strcasestr(st.tags, q)) l->it[l->n++] = st;
    }
    if (ra_rb_on()) ra_list_more(0); else l->state = 2;
}
static void ra_edit_redraw(uint16_t *d, int pressed)
{
    ra_render_page(d, RG_ROOT, 1);
    kb_draw(d, pressed);
}
static void ra_edit_anim(int on)
{
    double t0 = now_ms(); float from = ra_edit_e, to = on ? 1.f : 0.f;
    if (from == to) return;
    for (;;) {
        double t = (now_ms() - t0) / 240.0; if (t > 1) t = 1;
        ra_edit_e = from + (to - from) * (float)ease_out(t);
        ra_render_page(fr0, RG_ROOT, 1); present();
        if (t >= 1) break;
    }
}
static void ra_search_close(void)
{
    ra_q[0] = ra_q_sent[0] = 0; ra_q_typed = 0;
    ra_list_reset(0);
    ra_pos[RG_ROOT] = 0; ra_vel = 0;
    ra_edit_anim(0);
}
/* barra de busqueda con nuestro teclado: resultados al escribir (con espera), tecla Buscar,
 * cancelar y borrar. Si se toco algo de la lista mientras tanto, lo devuelve en *rk / *ri. */
static void ra_search_edit(int *rk, int *ri)
{
    struct ebuf e = { ra_q, (int)strlen(ra_q), sizeof ra_q - 1, (int)strlen(ra_q) };
    *rk = 0; *ri = -1;
    if (ra_edit_e < 0.5f) { ra_pos[RG_ROOT] = 0; ra_vel = 0; }
    ra_editing = 1;
    ra_edit_anim(1);
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Buscar"));
    kb_begin(&e, fr0, ra_edit_redraw);
    struct pkt p; char last[96]; snprintf(last, sizeof last, "%s", ra_q);
    double blink = now_ms();
    for (;;) {
        int dirty = ra_poll();
        ra_page_more();
        if (strcmp(last, ra_q)) { snprintf(last, sizeof last, "%s", ra_q); ra_q_typed = now_ms(); dirty = 1; }
        if (ra_q_typed > 0 && now_ms() - ra_q_typed > 450) { ra_search_now(); dirty = 1; }
        if (now_ms() - blink > 530) { blink = now_ms(); dirty = 1; }
        int spin = ra_ls[0].state == 1 || ra_active();
        if (dirty || spin) { ra_edit_redraw(fr0, -1); present(); }
        if (!read_pkt(&p, spin ? 40 : 120)) continue;
        if (!p.down) continue;
        if (p.y >= KB_Y) {
            int r = kb_touch(&p, &e, fr0, ra_edit_redraw, 0);
            if (r == K_ENTER) { ra_search_now(); break; }
            continue;
        }
        int y = p.y, moved = 0, idx, k = fm_hit_at(p.x, p.y, &idx);
        while (cur_down && read_pkt(&p, 3000)) if (abs(cur_y - y) > 12) moved = 1;
        if (k == FH_CANCEL) { ra_q[0] = 0; e.len = e.cur = 0; break; }
        if (k == FH_CLEAR) { ra_q[0] = 0; e.len = e.cur = 0; kb_autoshift(&e); if (kb_off) kb_reveal(fr0, ra_edit_redraw); continue; }
        if (k == FH_BAR) { if (kb_off) kb_reveal(fr0, ra_edit_redraw); continue; }
        if (moved) { ra_search_now(); break; }       /* arrastrar la lista guarda el teclado (iOS) */
        if (k >= RH_ST && y < KB_Y) { *rk = k; *ri = idx; break; }
    }
    kb_dst = fr0; kb_rd = ra_edit_redraw;
    if (!kb_off) kb_hide();
    kb_end();
    ra_editing = 0;
    if (!ra_q[0]) ra_search_close();                 /* con texto, la cabecera de busqueda se queda (Cancelar la cierra) */
}

/* ------------------------------------------------------------------ acciones */
static void ra_fav_tap(const struct ra_station *s0)
{
    struct ra_station s = *s0;
    if (ra_fav_toggle(ra_lb, &s) < 0) { toast(tr("No caben más favoritas")); return; }
    ra_save_lib();
}
static void ra_add_station(void)
{
    char url[RA_URLN] = "", norm[RA_URLN], name[96], host[96];
    if (!fm_edit_plain(tr("URL de la emisora"), "http://", url, sizeof url - 1)) return;
    if (ra_url_norm(url, norm, sizeof norm)) { toast(tr("URL no válida")); return; }
    ra_url_host(norm, host, sizeof host);
    snprintf(name, sizeof name, "%s", host);
    if (!line_edit(tr("Nombre"), host, name, sizeof name - 1)) return;
    int r = ra_own_add(ra_lb, name, norm);
    if (r == -2) toast(tr("Esa emisora ya está en la lista"));
    else if (r == -3) toast(tr("No caben más emisoras"));
    else if (r < 0) toast(tr("URL no válida"));
    else { ra_save_lib(); toast(tr("Emisora añadida")); }
}
static void ra_own_menu(int i)
{
    if (i < 0 || i >= ra_lb->nown) return;
    struct ra_station *s = &ra_lb->own[i];
    const char *o[3] = { tr("Renombrar"), tr("Editar URL"), tr("Eliminar") };
    int r = action_sheet(s->name, o, 3, 2);
    if (r == 0) {
        char n[96]; snprintf(n, sizeof n, "%s", s->name);
        if (line_edit(tr("Nombre"), s->name, n, sizeof n - 1)) { ra_clean(n); if (n[0]) { snprintf(s->name, sizeof s->name, "%s", n); ra_save_lib(); } }
    } else if (r == 1) {
        char u[RA_URLN], norm[RA_URLN]; snprintf(u, sizeof u, "%s", s->url);
        if (fm_edit_plain(tr("Editar URL"), "http://", u, sizeof u - 1)) {
            if (ra_url_norm(u, norm, sizeof norm)) toast(tr("URL no válida"));
            else { snprintf(s->url, sizeof s->url, "%s", norm); ra_save_lib(); }
        }
    } else if (r == 2) { ra_own_del(ra_lb, i); ra_save_lib(); }
}
/* ------------------------------------------------------------------ fuentes */
/* el interruptor de una fila: el pomo se desliza y luego cambia */
static void ra_src_toggle(int i)
{
    if (i < 0 || i >= ra_sr.n) return;
    struct ra_src *s = &ra_sr.s[i];
    double t0 = now_ms(); int to = !s->on, ry = ra_srcs_top() + i * RA_SRH;
    for (;;) {
        double t = (now_ms() - t0) / 180.0; if (t > 1) t = 1;
        float e = (float)ease_out(t), k = to ? e : 1 - e;
        ra_render_page(fr0, RG_SRCS, 1);
        if (ry + 48 < RA_MINI_TOP || !ra_np.have) {
            fill_rrect(fr0, W - 16 - 12 - 51 - 2, ry + 13, 55, 35, 17, FMC_CARD, 255);
            sp_switch(fr0, W - 16 - 12 - 51, ry + 15, k, 0);
        }
        present(); if (t >= 1) break;
    }
    s->on = (unsigned char)to;
    ra_save_srcs();
    if (s->on && s->type > RSRC_EU && s->type != RSRC_RBX && (s->err || !s->fetched)) ra_src_fetch(i, -1, 0);
}
static void ra_src_add_ui(void)
{
    if (ra_sr.n >= RA_MAXSRC) { toast(tr("No caben más fuentes")); return; }
    char in[RA_URLN] = "", url[RA_URLN];
    if (!fm_edit_plain(tr("Añadir fuente"), tr("URL de una lista de emisoras"), in, sizeof in - 1)) return;
    int t = ra_src_detect(in, url, sizeof url);
    if (t < 0 || strlen(url) >= sizeof ra_sr.s[0].url) { toast(tr("URL no válida")); return; }
    if (ra_src_find_url(&ra_sr, url) >= 0) { toast(tr("Esa fuente ya está")); return; }
    struct ra_src *s = ra_src_add(&ra_sr, t, url, "");
    if (!s) return;
    ra_save_srcs();
    lg("radio: fuente nueva %s (tipo %d)", url, t);
    toast(tr("Fuente añadida"));
    ra_src_fetch(ra_sr.n - 1, -1, 1);
}
static void ra_src_menu(int i)
{
    if (i < 0 || i >= ra_sr.n) return;
    struct ra_src *s = &ra_sr.s[i];
    if (s->type == RSRC_RB) return;                  /* el directorio: solo el interruptor */
    enum { A_OPEN, A_REFRESH, A_RENAME, A_URL, A_DEL };
    const char *o[5]; int act[5], n = 0, del = -1, user = s->type > RSRC_EU;
    if (s->on) { o[n] = tr("Abrir"); act[n++] = A_OPEN; }
    if (user && s->type != RSRC_RBX) { o[n] = tr("Actualizar"); act[n++] = A_REFRESH; }
    if (user) { o[n] = tr("Renombrar"); act[n++] = A_RENAME; o[n] = tr("Editar URL"); act[n++] = A_URL; del = n; o[n] = tr("Eliminar fuente"); act[n++] = A_DEL; }
    if (!n) return;
    char title[300]; snprintf(title, sizeof title, "%s", user ? s->url : ra_src_name(s));
    int a = action_sheet(title, o, n, del);
    if (a < 0 || a >= n) return;
    switch (act[a]) {
    case A_OPEN: ra_src_open(i, 0); ra_push(RG_LIST); break;
    case A_REFRESH: ra_src_fetch(i, -1, 1); break;
    case A_RENAME: {
        char nm[56]; snprintf(nm, sizeof nm, "%s", s->name);
        if (line_edit(tr("Renombrar"), tr("Nombre"), nm, sizeof nm - 1)) { ra_clean(nm); if (nm[0]) { snprintf(s->name, sizeof s->name, "%s", nm); s->named = 1; ra_save_srcs(); } }
        break; }
    case A_URL: {
        char in[RA_URLN], url[RA_URLN]; snprintf(in, sizeof in, "%s", s->url);
        if (!fm_edit_plain(tr("Editar URL"), tr("URL de una lista de emisoras"), in, sizeof in - 1)) break;
        int t = ra_src_detect(in, url, sizeof url);
        if (t < 0 || strlen(url) >= sizeof s->url) { toast(tr("URL no válida")); break; }
        if (!strcmp(url, s->url)) break;
        int k = ra_src_find_url(&ra_sr, url); if (k >= 0 && k != i) { toast(tr("Esa fuente ya está")); break; }
        char c[200]; ra_list_cache(s->sid, c, sizeof c); unlink(c);
        snprintf(s->url, sizeof s->url, "%s", url); s->sid = ra_hash(url); s->type = (unsigned char)t; s->err = 0; s->fetched = 0; s->n = 0;
        if (!s->named) ra_url_host(url, s->name, sizeof s->name);
        ra_save_srcs();
        ra_src_fetch(i, -1, 1);
        break; }
    case A_DEL:
        if (confirm(tr("¿Eliminar la fuente?"), tr("Eliminar"))) {
            char c[200]; ra_list_cache(s->sid, c, sizeof c); unlink(c);
            ra_src_del(&ra_sr, i); ra_save_srcs();
        }
        break;
    }
}
static void ra_tap(int k, int i);
static void ra_search_ui(void)
{
    int k, i; ra_search_edit(&k, &i);
    if (k >= RH_ST) ra_tap(k, i);
}
static void ra_tap(int k, int i)
{
    struct ra_station *s;
    switch (k) {
    case FH_BACK: if (ra_sp > 0) ra_pop_from(0); break;
    case FH_BAR: ra_search_ui(); break;
    case FH_CLEAR: ra_q[0] = 0; ra_search_ui(); break;
    case FH_CANCEL: ra_search_close(); break;
    case RH_ST: if ((s = ra_station_at(i)) && !(ra_is_cur(s) && ra_active())) ra_play(s); break;
    case RH_STAR: if ((s = ra_station_at(i))) ra_fav_tap(s); break;
    case RH_OWNMORE: ra_own_menu(i & 0xffff); break;
    case RH_TAB:
        if (ra_tab != i) { ra_tab = i; ra_tab_t0 = now_ms(); ra_pos[RG_ROOT] = 0; ra_vel = 0; if (ra_cf.tab != i) { ra_cf.tab = i; ra_conf_dirty = 1; } }
        break;
    case RH_NAV:
        if (i == 0) { ra_list_open(1, RL_TOP, "", tr("Más escuchadas")); ra_push(RG_LIST); }
        else if (i == 1) { ra_list_open(1, RL_COUNTRY, ra_cf.cc, ra_cf.cname); ra_push(RG_LIST); }
        else { if (ra_ct_state != 1 && ra_ct_state != 2) ra_ctry_fetch(); ra_push(RG_CTRY); }
        break;
    case RH_GENRE: if (i >= 0 && i < RA_NGENRE) { ra_list_open(1, RL_TAG, RA_GENRES[i].tag, tr(RA_GENRES[i].key)); ra_push(RG_LIST); } break;
    case RH_CTRY:
        if (i >= 0 && i < ra_nct) {
            snprintf(ra_cf.cc, sizeof ra_cf.cc, "%s", ra_ct[i].cc); snprintf(ra_cf.cname, sizeof ra_cf.cname, "%s", ra_ct[i].name);
            ra_save_conf(); ra_conf_dirty = 0;
            ra_render(); present();
            ra_pop_from(0);
        }
        break;
    case RH_ADD: ra_add_station(); break;
    case RH_RETRY:
        if (i == 2) ra_ctry_fetch();
        else if (i == 1 && ra_ls[1].sid && !ra_ls[1].base[0]) ra_src_open(ra_src_ix(&ra_sr, ra_ls[1].sid), 1);
        else if (i == 0 || i == 1) ra_list_more(i);
        break;
    case RH_SRC: if (i >= 0 && i < ra_sr.n) { ra_src_open(i, 0); ra_push(RG_LIST); } break;
    case RH_SRCMGR: ra_pos[RG_SRCS] = 0; ra_push(RG_SRCS); break;
    case RH_SRCROW: ra_src_menu(i); break;
    case RH_SRCSW: ra_src_toggle(i); break;
    case RH_SRCADD: ra_src_add_ui(); break;
    case RH_MSTAR: ra_fav_tap(&ra_np.cur); break;
    case RH_MPLAY: if (ra_active()) ra_stop(); else { struct ra_station c = ra_np.cur; ra_play(&c); } break;
    }
}

/* ------------------------------------------------------------------ la app */
static void radio_app_at(int ix, int iy)
{
    if (!ra_load()) {
        toast(tr("No hay memoria suficiente"));
        double t = now_ms(); while (now_ms() - t < 1700) launcher_present();
        launcher_present();
        return;
    }
    fm_pa = img_under; fm_pb = img_sheet; have_under = 0;
    ra_sp = 0; ra_stack[0] = RG_ROOT; memset(ra_pos, 0, sizeof ra_pos); ra_vel = 0;
    ra_editing = 0; ra_edit_e = 0; ra_q[0] = ra_q_sent[0] = 0; ra_q_typed = 0; ra_tab_t0 = 0; ra_conf_dirty = 0;
    ra_tab = (ra_lb->nfav || ra_lb->nrec || ra_lb->nown) ? ra_cf.tab : 1;      /* sin nada guardado: a explorar */
    fm_press_kind = 0; fm_press_idx = -1;
    ra_list_reset(0); ra_list_reset(1);
    ra_pl_sync();
    int gone = 0;
    render_launcher(img_base, now_ms());
    ra_render_page(fm_pb, RG_ROOT, 1);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 300.0; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), fm_pb, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    enum { RT_IDLE, RT_PRESS, RT_DRAG, RT_BOTTOM };
    struct pkt p; int quit = 0, dirty = 1, ts = RT_IDLE, was_down = cur_down, moving = 0;
    int sx = 0, sy = 0, kind = 0, idx = -1; float raw0 = 0, vy = 0;
    double tmove = 0, tlast = now_ms(), tdown = 0, tbusy = 0; int ly = 0;
    if (cur_down) ts = RT_BOTTOM;                    /* el dedo que abrio la app no cuenta */
    while (!quit) {
        if (ra_poll()) dirty = 1;
        ra_page_more();
        int pg = ra_stack[ra_sp]; float *pos = &ra_pos[pg], lo = 0, hi = ra_maxpos(pg);
        double now = now_ms(), dt = now - tlast; tlast = now; if (dt > 50) dt = 50;
        if (ts != RT_DRAG && ts != RT_PRESS && !moving && (*pos < lo - .5f || *pos > hi + .5f)) moving = 1;
        if (moving) {
            if (*pos < lo || *pos > hi) {
                float tgt = *pos < lo ? lo : hi;
                *pos += (tgt - *pos) * (float)(1 - exp(-dt / 70.0)); ra_vel *= 0.5f;
                if (fabsf(tgt - *pos) < 0.5f) { *pos = tgt; ra_vel = 0; moving = 0; }
            } else {
                *pos += ra_vel * (float)dt; ra_vel *= (float)pow(0.998, dt);
                if (fabsf(ra_vel) < 0.02f) { ra_vel = 0; moving = 0; }
            }
            dirty = 1;
        }
        int anim = moving || now < toast_until + 40 || now - ra_tab_t0 < 240;
        int busy = ra_ls[0].state == 1 || ra_ls[1].state == 1 || ra_ct_state == 1 || ra_active();
        for (int k = 0; k < ra_sr.n && !busy; k++) busy = ra_sr.s[k].busy && pg == RG_SRCS;
        if (busy && now - tbusy > 95) dirty = 1;
        if (dirty || anim) { ra_render(); present(); dirty = 0; tbusy = now; }
        int wait = anim ? 0 : busy ? 60 : 250;
        while (!quit && read_pkt(&p, wait)) {
            wait = 0;
            int down_edge = p.down && !was_down, up_edge = !p.down && was_down; was_down = p.down;
            if (down_edge) {
                sx = cur_x; sy = cur_y; tdown = tmove = now_ms(); ly = sy; vy = 0; moving = 0; ra_vel = 0;
                kind = fm_hit_at(sx, sy, &idx);
                if (sy >= 456) { ts = RT_BOTTOM; break; }
                if (back_zone(sx, sy)) {                 /* en la raiz: salir de la app */
                    fm_press_kind = kind; fm_press_idx = idx;
                    if (kind) { ra_render(); present(); }
                    fm_press_kind = 0; fm_press_idx = -1; ra_render();
                    int bk = app_back_gesture(sx, sy);
                    was_down = cur_down;
                    if (bk > 0 && ra_sp == 0) { quit = 1; break; }
                    if (bk > 0) { ra_pop_from(0); ts = RT_IDLE; dirty = 1; break; }
                    if (bk < 0) { ts = RT_IDLE; dirty = 1; break; }
                }
                fm_press_kind = kind; fm_press_idx = idx; ts = RT_PRESS;
                if (kind) dirty = 1;
                if (!cur_down) up_edge = 1; else break;
            }
            if (p.down && !up_edge) {
                if (ts == RT_BOTTOM) {
                    if (sy >= 456 && sy - cur_y > 12 && now_ms() - tdown < 500) {
                        ra_render();
                        if (own_home_gesture(sx, sy, app_by_id("radio"))) { gone = 1; quit = 1; break; }
                        was_down = cur_down; ts = RT_IDLE; dirty = 1; break;
                    }
                    continue;
                }
                if (ts == RT_PRESS && (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 12)) {
                    ts = RT_DRAG; raw0 = fm_unrb(*pos, lo, hi) + (float)(cur_y - sy);
                    fm_press_kind = 0; fm_press_idx = -1;
                }
                if (ts == RT_DRAG) {
                    *pos = fm_rb(raw0 - (float)(cur_y - sy), lo, hi);
                    double t = now_ms();
                    if (t > tmove) { vy = 0.6f * vy + 0.4f * (float)((ly - cur_y) / (t - tmove)); tmove = t; ly = cur_y; }
                    dirty = 1;
                }
                continue;
            }
            if (up_edge) {
                int t0s = ts; ts = RT_IDLE;
                if (t0s == RT_DRAG) { if (now_ms() - tmove > 80) vy = 0; ra_vel = vy; moving = 1; tlast = now_ms(); dirty = 1; continue; }
                if (t0s == RT_BOTTOM) { dirty = 1; continue; }
                int k = fm_press_kind, i = fm_press_idx;
                fm_press_kind = 0; fm_press_idx = -1; dirty = 1;
                if (t0s == RT_PRESS && k) { ra_render(); present(); ra_tap(k, i); tlast = now_ms(); }
                was_down = cur_down;
                break;
            }
        }
    }
    drain_until_up();
    /* al salir: la radio sigue (hilo vigilante); lo de pantalla se suelta */
    if (ra_conf_dirty) { ra_save_conf(); ra_conf_dirty = 0; }
    ra_list_reset(0); ra_list_reset(1);
    free(ra_ls[0].it); ra_ls[0].it = 0; free(ra_ls[1].it); ra_ls[1].it = 0;
    free(ra_ct); ra_ct = 0; ra_nct = 0; ra_ct_state = 0;
    ra_unload();
    ra_sp = 0; ra_editing = 0; ra_edit_e = 0;
    have_under = 0;
    if (gone) { launcher_present(); return; }
    memcpy(fm_pb, fr0, HM_FRAME_BYTES);
    render_launcher(img_base, now_ms());
    double t0b = now_ms();
    for (;;) {
        double t = (now_ms() - t0b) / 280.0; if (t > 1) t = 1; double e = 1 - ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), fm_pb, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    launcher_present();
}
