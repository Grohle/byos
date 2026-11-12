/* vollimit.inc.c - limite de volumen seguro (UE, EN 50332-3), al estilo de iOS.
 *
 * - Activado por defecto. Con el limite, el volumen maximo del reproductor (volmax, ajuste 0x10)
 *   se pone a 50: la propia rueda no pasa de ahi, sin saltos audibles ni correcciones a destiempo.
 * - Al intentar subir por encima (rueda hacia arriba estando en 50) aparece una tarjeta como la de
 *   los auriculares y la de la biblioteca: "Volumen alto" con Mantener / Subir el volumen.
 *   Subir devuelve volmax a 100 hasta que se acumulen 20 horas de escucha; entonces se vuelve a
 *   limitar (el reproductor baja a 50) y se vuelve a preguntar, como pide la norma.
 * - Quitar el limite (Ajustes > Sonido) pide confirmacion. El aviso negro de HiBy
 *   (vol_warn_enable, volume_long_time_high.dlg) queda apagado: este lo sustituye.
 * Estado en VL_STATE ("on ack segundos"), escrito con rename. */
#define VL_LIMIT 50
#define VL_FULL 100
#define VL_STATE "/usr/data/hibymod/vlim.state"
#define VL_REARM_S (20 * 3600)
#define VP_H 158
#define VP_W (W - 16)
static int vl_on = 1, vl_ack, vl_loaded;
static double vl_listen;                 /* segundos escuchados desde que se acepto subir */
static double vl_pop_req, vl_saved_at;
static int vl_pop_rearm;                 /* el aviso sale porque se cumplieron las 20 h */

static void vl_save(void)
{
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", VL_STATE);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "%d %d %.0f\n", vl_on, vl_ack, vl_listen);
    int ok = !ferror(f); if (fclose(f)) ok = 0;
    if (ok) rename(tmp, VL_STATE); else unlink(tmp);
    vl_saved_at = now_ms();
}
static void vl_load(void)
{
    vl_loaded = 1;
    FILE *f = fopen(VL_STATE, "r");
    if (f) { int on = 1, ack = 0; double ls = 0; if (fscanf(f, "%d %d %lf", &on, &ack, &ls) == 3) { vl_on = !!on; vl_ack = !!ack; vl_listen = ls; } fclose(f); }
}
/* el limite esta puesto ahora mismo (la pildora mide entonces sobre 100, no sobre volmax) */
static int vl_in_effect(void) { return vl_on && !vl_ack; }
static int vl_want_max(void) { return vl_in_effect() ? VL_LIMIT : VL_FULL; }
/* volmax del reproductor = lo que pide el estado (por la cola de ajustes: no bloquea) */
static void vl_apply(void)
{
    ps_start();
    if (!ps_ok[PS_VOLMAX]) { ps_refresh(); return; }            /* sin leer aun: en el siguiente paso */
    if (ps_unsup[PS_VOLMAX] || ps_v[PS_LINEOUT]) return;          /* salida de linea: volumen fijo */
    int want = vl_want_max();
    if (ps_sh[PS_VOLMAX] != want && !ps_busy()) { lg("volumen: limite %s (volmax %d -> %d)", vl_in_effect() ? "puesto" : "quitado", ps_sh[PS_VOLMAX], want); ps_set_now(PS_VOLMAX, want); }
}
static void vl_init(void)
{
    vl_load();
    if (cfg_get("vol_warn_enable", 1)) cfg_set("vol_warn_enable", 0);   /* el aviso negro de HiBy: lo sustituye este */
    vl_apply();
}
/* rueda hacia arriba (hud_key): en el limite, la tarjeta */
static void vl_key_up(void)
{
    if (!vl_in_effect() || ps_v[PS_LINEOUT]) return;
    /* nivel crudo del reproductor (0..volmax): el que publica la libreria o, si aun no, la cifra
     * de la barra de estado */
    int v = st->vol_now ? (int)st->vol_now - 1 : ocr_volume(st->capture ? fr1 : visible());
    if (v >= VL_LIMIT && now_ms() - vl_pop_req > 1200) { vl_pop_req = now_ms(); vl_pop_rearm = 0; lg("volumen: en el limite (%d): aviso", v); }
    else if (v < 0) lg("volumen: sin lectura del nivel para el limite");
}
/* cada 10 s (guard_tick): tiempo de escucha, las 20 h y que volmax siga como debe */
static void vl_tick(void)
{
    if (!vl_loaded) return;
    if (vl_on && vl_ack && bu_playing()) {
        vl_listen += 10;
        if (vl_listen >= VL_REARM_S) {
            vl_ack = 0; vl_listen = 0; vl_save();
            lg("volumen: 20 h de escucha por encima del limite: se vuelve a limitar");
            vl_pop_req = now_ms(); vl_pop_rearm = 1;
        } else if (now_ms() - vl_saved_at > 300000) vl_save();
    }
    vl_apply();
}
static void vl_accept(void)
{
    vl_ack = 1; vl_listen = 0; vl_save();
    lg("volumen: el usuario acepta pasar del limite");
    vl_apply();
}
/* Ajustes > Sonido: activar o quitar el limite */
static void vl_set_on(int on)
{
    vl_on = !!on; vl_ack = 0; vl_listen = 0; vl_save(); vl_apply();
}

/* ---- tarjeta (la presenta syspop.inc.c, sp_kind 2) */
static uint32_t vp_shown;
static int vol_popup_needed(void) { return vl_pop_req && now_ms() - vl_pop_req < 2000 && vp_shown != (uint32_t)vl_pop_req; }
static void vol_popup_ack(void) { vp_shown = (uint32_t)vl_pop_req; }
static int vol_popup_expired(double t_open) { return now_ms() - t_open > 10000; }   /* sin respuesta: se mantiene */
static int vp_top(float progress) { return H - (int)((VP_H + 8) * progress); }
static void vp_speaker(uint16_t *d, int cx, int cy)
{
    fill_circle(d, cx, cy, 22, rgb(255, 159, 10), 255);              /* naranja de aviso de iOS */
    uint16_t w = 0xFFFF;
    fill_rrect(d, cx - 11, cy - 5, 6, 10, 1, w, 255);                 /* altavoz: caja + cono */
    for (int i = 0; i < 9; i++) fill_rrect(d, cx - 6 + i, cy - 5 - i * 6 / 9, 1, 10 + i * 12 / 9, 0, w, 255);
    fill_rrect(d, cx + 7, cy - 7, 3, 9, 1, w, 255);                   /* "!" */
    fill_circle(d, cx + 8.5f, cy + 6, 1.8f, w, 255);
}
static void vol_popup_draw(uint16_t *d, float progress)
{
    if (progress <= 0) return;
    if (progress > 1) progress = 1;
    int k = (int)(10 * progress);
    if (k > 0) for (int p = 30 * W; p < W * H; p++) d[p] = dim565(d[p], 32 - k);
    int top = vp_top(progress), x0 = 8;
    fill_rrect(d, x0, top, VP_W, VP_H, 24, rgb(30, 30, 33), 255);
    vp_speaker(d, x0 + 38, top + 38);
    draw_text(d, x0 + 74, top + 34, tr(vl_pop_rearm ? "Volumen limitado de nuevo" : "Volumen alto"), 16, 0xFFFF, VP_W - 90, 0, H);
    draw_text(d, x0 + 74, top + 54, tr("Por encima de 50"), 13, rgb(152, 152, 159), VP_W - 90, 0, H);
    /* texto en dos lineas como mucho */
    const char *body = tr(vl_pop_rearm ? "VOLPOP_REARM" : "VOLPOP_BODY");
    char l1[96], l2[96]; int n = (int)strlen(body), cut = n, maxw = VP_W - 32;
    if (tw_n(body, 13, n) > maxw) { for (cut = n; cut > 0 && (body[cut] != ' ' || tw_n(body, 13, cut) > maxw); cut--) {} if (cut <= 0) cut = n; }
    snprintf(l1, sizeof l1, "%.*s", cut, body); snprintf(l2, sizeof l2, "%s", cut < n ? body + cut + 1 : "");
    draw_text(d, x0 + 16, top + 86, l1, 13, rgb(220, 220, 225), maxw, 0, H);
    if (l2[0]) draw_text(d, x0 + 16, top + 104, l2, 13, rgb(220, 220, 225), maxw, 0, H);
    /* botones */
    int by = top + VP_H - 46, bw = (VP_W - 16 * 2 - 10) / 2;
    fill_rrect(d, x0 + 16, by, bw, 34, 17, rgb(58, 58, 62), 255);
    fill_rrect(d, x0 + 16 + bw + 10, by, bw, 34, 17, rgb(10, 132, 255), 255);
    const char *b1 = tr("Mantener"), *b2 = tr("Subir el volumen");
    int w1 = tw_n(b1, 15, (int)strlen(b1)), w2 = tw_n(b2, 15, (int)strlen(b2));
    draw_text(d, x0 + 16 + (bw - w1) / 2, by + 23, b1, 15, 0xFFFF, bw - 8, 0, H);
    draw_text(d, x0 + 16 + bw + 10 + (bw - (w2 < bw - 8 ? w2 : bw - 8)) / 2, by + 23, b2, 15, 0xFFFF, bw - 8, 0, H);
}
/* 1 = mantener (cerrar), 3 = subir, 2 = resto de la tarjeta, 0 = fuera */
static int vol_popup_hit(int x, int y, float progress)
{
    int top = vp_top(progress);
    if (y < top || y > top + VP_H || x < 8 || x > 8 + VP_W) return 0;
    int by = top + VP_H - 46, bw = (VP_W - 16 * 2 - 10) / 2;
    if (y >= by - 6 && y <= by + 40) return x < 8 + 16 + bw + 5 ? 1 : 3;
    return 2;
}
