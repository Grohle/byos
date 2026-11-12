/* syspop.inc.c - capa de avisos del sistema sobre CUALQUIER pantalla: la tarjeta de "auriculares
 * conectados" (buds.inc.c dibuja la tarjeta; aqui se presenta).
 *
 * - En pantallas del shell (inicio, apps propias: st->capture) la tarjeta se compone encima de
 *   lo que la app presenta (gancho en present(); fr0 de la app no se toca).
 * - En pantallas de HiBy se congela un momento su imagen ('C'), se anima encima y se suelta ('R')
 *   al acabar; un toque fuera de la tarjeta la cierra y sigue hacia HiBy.
 * - Entra con un muelle desde abajo, se va sola a los 6 s, la X o deslizar hacia abajo la cierran
 *   y tocarla abre Auriculares (desde el inicio o una pantalla de HiBy; en una app propia, al
 *   volver al inicio).
 * Se mueve desde read_pkt (cada 16 ms mientras esta activa) y desde present(). */
#define SP_IN_MS 420.0
#define SP_OUT_MS 260.0
#define SP_HOLD_MS 6000.0
static int sp_cap, sp_closing, sp_open_req, sp_in_tick, sp_kind;   /* sp_kind: 0 auriculares, 1 biblioteca */
static double sp_t0, sp_tc, sp_last_check; static float sp_close_from = 1;
static uint16_t *sp_tmp, *sp_base;

static float sp_progress(void)
{
    double t = now_ms();
    if (sp_closing) { double u = (t - sp_tc) / SP_OUT_MS; if (u > 1) u = 1; return (float)(sp_close_from * (1 - u * u)); }
    double u = (t - sp_t0) / SP_IN_MS; if (u > 1) u = 1;
    return (float)ease_out(u);
}
static void sp_close(void)
{
    if (!sp_on || sp_closing) return;
    sp_close_from = sp_progress(); sp_closing = 1; sp_tc = now_ms();
    if (sp_kind == 1) lib_popup_dismiss(); else buds_popup_dismiss();
}
/* compone la tarjeta sobre src (fotograma completo) en el bufer compartido y lo presenta */
static void sp_present_over(const uint16_t *src)
{
    if (!sp_tmp) return;
    memcpy(sp_tmp, src, HM_FRAME_BYTES);
    if (!sp_cap) { statusbar_tint(sp_tmp); int a = 0, b = 0; homebar_layer(sp_tmp, src, &a, &b); }   /* como los de la app */
    if (sp_kind == 1) lib_popup_draw(sp_tmp, sp_progress()); else buds_popup_draw(sp_tmp, sp_progress());
    st->fr0_seq++; __sync_synchronize();
    memcpy(fr0_shm, sp_tmp, HM_FRAME_BYTES);
    st->dirty_y0 = 0; st->dirty_y1 = H;
    __sync_synchronize(); st->fr0_seq++;
}
static void sp_finish(void)
{
    sp_on = 0; sp_closing = 0;
    if (sp_cap) {                     /* pantalla de HiBy: soltar al reproductor */
        sp_cap = 0; cmd('R');
        double t = now_ms(); while (st->held && now_ms() - t < 200) msleep(2);
        update_overlay();
    } else if (st->capture) {         /* pantalla del shell: volver a la imagen de la app tal cual */
        uint32_t p = st->presents;
        hud_compose_present_ex(fr0, 0);              /* con barra de estado y de inicio, sin parpadeo */
        cmd('P'); double t = now_ms(); while (st->presents == p && now_ms() - t < 80) msleep(1);
    }
    lg("aviso: %s cerrado", sp_kind == 1 ? "biblioteca" : "auriculares");
}
static void sp_start(int kind)
{
    sp_kind = kind;
    if (!sp_tmp) sp_tmp = lazy_alloc((void **)&sp_tmp, HM_FRAME_BYTES, 0);    /* se devuelven en reposo */
    if (!sp_base) sp_base = lazy_alloc((void **)&sp_base, HM_FRAME_BYTES, 0);
    if (!sp_tmp || !sp_base) { if (kind == 1) { lib_popup_ack(); lib_popup_dismiss(); } else buds_popup_dismiss(); return; }
    if (kind == 1) lib_popup_ack(); else buds_popup_ack();
    sp_cap = 0;
    if (!st->capture && !st->held && !launcher_on) {      /* pantalla de HiBy: congelarla */
        st->ov_on = 0; ov_cur = 0;
        cmd('C');
        double t = now_ms(); while (!st->capture && now_ms() - t < 200) msleep(2);
        if (!st->capture) return;
        memcpy(sp_base, fr0_shm, HM_FRAME_BYTES);
        sp_cap = 1;
    }
    sp_on = 1; sp_closing = 0; sp_t0 = now_ms();
    lcd_boost();
    lg("aviso: %s (%s)", kind == 1 ? "biblioteca actualizandose" : "auriculares conectados", sp_cap ? "sobre HiBy" : "sobre el shell");
}
/* un paso: lo llama read_pkt mientras espera */
static void syspop_tick(void)
{
    if (sp_in_tick) return;
    sp_in_tick = 1;
    double t = now_ms();
    if (!sp_on && t - sp_last_check > 300) {
        sp_last_check = t;
        if (buds_popup_needed()) sp_start(0);
        else if (lib_popup_needed()) sp_start(1);
    }
    if (sp_on) {
        if (!sp_closing && (sp_kind == 1 ? lib_popup_expired() : t - sp_t0 > SP_HOLD_MS)) sp_close();
        if (sp_closing && t - sp_tc >= SP_OUT_MS) sp_finish();
        else {
            uint32_t p = st->presents;
            sp_present_over(sp_cap ? sp_base : fr0);
            cmd('P'); while (st->presents == p && now_ms() - t < 80) msleep(1);
        }
    }
    sp_in_tick = 0;
}
/* present() con el aviso sobre una app: lo compone entero sobre fr0 (0 si no hay aviso) */
static int syspop_owns_frame(void)
{
    if (!sp_on || sp_cap || sp_in_tick) return 0;
    sp_in_tick = 1; sp_present_over(fr0); sp_in_tick = 0;
    return 1;
}
/* gancho de present(): la app acaba de presentar su fotograma; se pone la tarjeta encima */
static void syspop_after_present(void)
{
    if (!sp_on || sp_cap || sp_in_tick) return;
    sp_in_tick = 1; sp_present_over(fr0); sp_in_tick = 0;
}
/* gancho de read_pkt: un toque que empieza en la tarjeta es suyo. Devuelve 1 si se lo queda. */
static int syspop_touch(const struct pkt *p)
{
    if (!sp_on || sp_closing || !p->down) return 0;
    int hit = sp_kind == 1 ? lib_popup_hit(p->x, p->y, sp_progress()) : buds_popup_hit(p->x, p->y, sp_progress());
    if (!hit) {
        if (sp_cap) sp_close();       /* en HiBy: el toque fuera la cierra y sigue hacia HiBy */
        return 0;
    }
    int sy = p->y, moved = 0;
    struct input_event e;
    while (cur_down) {                /* consumir el gesto hasta levantar el dedo */
        struct pollfd pf = { ts, POLLIN, 0 };
        if (poll(&pf, 1, 30) <= 0) { syspop_tick(); continue; }
        if (read(ts, &e, sizeof e) != sizeof e) break;
        if (e.type == EV_KEY && e.code == BTN_TOUCH) cur_down = e.value;
        else if (e.type == EV_ABS && e.code == ABS_MT_POSITION_Y) { cur_y = e.value; if (cur_y - sy > 24) moved = 1; }
        else if (e.type == EV_ABS && e.code == ABS_MT_POSITION_X) cur_x = e.value;
    }
    if (moved || hit == 1) sp_close();
    else if (sp_kind == 1) { if (sp_cap) sp_close(); }   /* biblioteca: tocar la tarjeta no hace nada */
    else { sp_close(); sp_open_req = 1; }      /* tocar la tarjeta: abrir Auriculares */
    return 1;
}

/* ------------------------------------------------------------------ guardian de memoria (modulos)
 * Lienzos de trabajo que se rellenan enteros en cada uso y caches con su bandera de validez.
 * Solo se llama en reposo desde mem_guard (hibyshell.c). */
static void shell_trim_extra(void)
{
    drop_pages(bk_cscr, sizeof bk_cscr);                     /* Libros: lienzo de portadas generadas */
    drop_pages(ab_gen_buf, sizeof ab_gen_buf);               /* audiolibros: idem */
    drop_pages(wg_scr, sizeof wg_scr);                       /* widgets: lienzo de un widget */
    st_tabbar_key = -1; drop_pages(st_tabbar, sizeof st_tabbar);   /* tienda: barra de pestanas en cache */
    if (!widgets_on) wg_wall_blur_release();                 /* widgets: fondo desenfocado (fuera de su pagina) */
    pc_have = 0; drop_pages(pc_base, sizeof pc_base);         /* tarjeta de progreso (no hay ninguna en reposo) */
}

/* Al cerrar una app: sus lienzos y pilas de paginas vuelven al sistema. Las paginas devueltas se
 * leen como ceros, igual que al arrancar el shell, y cada app las rehace al abrirse (raiz nueva,
 * profundidad 0); las caches con indice se vacian a la vez. Sin esto el shell crecia de 5 a 8 MB
 * despues de recorrer las apps y el aparato quedaba con 3 MB libres. */
static void app_exit_release(void)
{
    double t0 = now_ms();
    shell_trim_extra();
    drop_pages(wl_pages, sizeof wl_pages);                   /* Inalambrico: pila de paginas */
    drop_pages(bu_pages, sizeof bu_pages);                   /* Auriculares: idem */
    drop_pages(kb_ovbuf, sizeof kb_ovbuf);                   /* teclado: lo que tapaba */
    kb_nmk = kb_mk_used = 0; drop_pages(kb_mk_pool, sizeof kb_mk_pool);   /* teclado: mascaras */
    ab_feed_drop(); drop_pages(&ab_feed, sizeof ab_feed);     /* Libros: catalogo abierto */
    ab_res.n = 0; ab_res_state = 0; ab_res_key[0] = 0; drop_pages(&ab_res, sizeof ab_res);   /* busqueda */
    drop_pages(img_app, sizeof img_app); drop_pages(img_base, sizeof img_base);   /* zoom de apertura: se rehacen */
    have_under = 0; drop_pages(img_under, sizeof img_under); drop_pages(img_sheet, sizeof img_sheet);
    have_cache_plane = 0; drop_pages(img_cache_plane, sizeof img_cache_plane);

    lg("app cerrada: memoria devuelta en %.0f ms (%d KB libres)", now_ms() - t0, mem_avail_kb());
}
