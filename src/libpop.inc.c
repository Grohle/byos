/* libpop.inc.c - aviso del sistema "Actualizando la biblioteca" (iOS), en lugar del dialogo negro
 * a pantalla completa de HiBy. Lo presenta syspop.inc.c como la tarjeta de los auriculares: sube
 * desde abajo sobre cualquier pantalla y se queda mientras dure el escaneo (st->lib_scan, lo vigila
 * la libreria); al acabar dice "Biblioteca actualizada" 2,5 s y se va. La X lo cierra (el escaneo
 * sigue) y ese escaneo ya no lo vuelve a mostrar.
 * API para syspop: lib_popup_needed() / _ack() / _dismiss() / _draw(d, progress) / _hit(x, y, p) /
 * _expired(). */
#define LP_H 92
#define LP_W (W - 16)
static uint32_t lp_seq_dismissed = 0xffffffffu, lp_seq_shown = 0xffffffffu;
static double lp_t0, lp_done_t, lp_force_t;
static int lp_saw_scan;

/* el shell acaba de pedir un escaneo (ajustes o descargas): mostrarlo ya, aunque el reproductor
 * tarde en empezar o acabe antes de que la libreria lo vea (cada 0,7 s) */
static void lib_popup_force(void) { lp_force_t = now_ms(); }
static int lib_popup_needed(void)
{
    if (lp_force_t && now_ms() - lp_force_t < 3000) return 1;
    return st->lib_scan && st->lib_scan_seq != lp_seq_dismissed && st->lib_scan_seq != lp_seq_shown;
}
static void lib_popup_ack(void)
{
    lp_force_t = 0; lp_seq_shown = st->lib_scan_seq; lp_saw_scan = st->lib_scan;
    lp_t0 = now_ms(); lp_done_t = 0;
}
static void lib_popup_dismiss(void)
{
    lp_seq_dismissed = lp_saw_scan ? lp_seq_shown : lp_seq_shown + 1;   /* sin empezar aun: el que viene */
}
/* 1 cuando ya toca cerrarlo (escaneo acabado y "actualizada" visto 2,5 s) */
static int lib_popup_expired(void)
{
    if (st->lib_scan) { lp_saw_scan = 1; lp_seq_shown = st->lib_scan_seq; return 0; }
    if (!lp_saw_scan && now_ms() - lp_t0 < 3000) return 0;        /* pedido, aun sin empezar */
    if (!lp_done_t) lp_done_t = now_ms();
    return now_ms() - lp_done_t > 2500;
}
static int lp_top(float progress) { return H - (int)((LP_H + 8) * progress); }
static void lib_popup_draw(uint16_t *d, float progress)
{
    if (progress <= 0) return;
    if (progress > 1) progress = 1;
    int k = (int)(10 * progress);                               /* lo de detras se oscurece un poco */
    if (k > 0) for (int p = 30 * W; p < W * H; p++) d[p] = dim565(d[p], 32 - k);
    int top = lp_top(progress), x0 = 8;
    fill_rrect(d, x0, top, LP_W, LP_H, 24, rgb(30, 30, 33), 255);
    int done = lp_done_t > 0;
    /* icono de Musica */
    int a = app_by_id("music");
    if (a >= 0) blit_scaled(d, &apps[a].icon[2], x0 + 16, top + 16, 44, 44, 255);
    const char *t1 = done ? tr("Biblioteca actualizada") : tr("Actualizando la biblioteca");
    draw_text(d, x0 + 74, top + 34, t1, 16, 0xFFFF, LP_W - 74 - 44, 0, H);
    char sub[64];
    if (done) snprintf(sub, sizeof sub, "%s", tr("Ya puedes verla en Música"));
    else { int s = (int)((now_ms() - lp_t0) / 1000); snprintf(sub, sizeof sub, "%s · %d s", tr("Buscando música nueva"), s); }
    draw_text(d, x0 + 74, top + 55, sub, 13, rgb(152, 152, 159), LP_W - 74 - 44, 0, H);
    /* barra: indeterminada mientras escanea (un tramo que recorre la pista), llena al acabar */
    int bx = x0 + 74, bw = LP_W - 74 - 20, by = top + 70;
    fill_rrect(d, bx, by, bw, 4, 2, rgb(58, 58, 62), 255);
    if (done) fill_rrect(d, bx, by, bw, 4, 2, rgb(48, 209, 88), 255);
    else {
        double ph = fmod((now_ms() - lp_t0) / 1400.0, 1.0);
        int sw = bw / 3, sx = bx - sw + (int)((bw + sw) * ph);
        int a0 = sx < bx ? bx : sx, a1 = sx + sw > bx + bw ? bx + bw : sx + sw;
        if (a1 > a0) fill_rrect(d, a0, by, a1 - a0, 4, 2, rgb(10, 132, 255), 255);
    }
    /* cerrar (X) */
    int cx = x0 + LP_W - 24, cy = top + 22;
    fill_circle(d, cx, cy, 11, rgb(58, 58, 62), 255);
    /* mientras sube desde abajo la tarjeta empieza fuera de la pantalla: solo las filas visibles
     * (sin esto escribia tras el bufer: SIGSEGV del shell al mostrarse, 12-11) */
    /* trazo de 2 px (el "xmark" de iOS va en negrita; de 1 px ademas desaparecia en la grabacion a
     * media resolucion segun la fila en que cayera mientras la tarjeta se movia) */
    for (int i = -4; i <= 4; i++) {
        int y = cy + i; if (y < 0 || y >= H) continue;
        for (int w = 0; w < 2; w++) { d[y * W + cx + i + w] = rgb(200, 200, 206); d[y * W + cx - i + w] = rgb(200, 200, 206); }
    }
}
/* 1 = cerrar, 2 = tarjeta, 0 = fuera */
static int lib_popup_hit(int x, int y, float progress)
{
    int top = lp_top(progress);
    if (y < top || y > top + LP_H || x < 8 || x > 8 + LP_W) return 0;
    if (x > 8 + LP_W - 50 && y < top + 46) return 1;
    return 2;
}
