/* ccbt.inc.c - modulo ancho de Bluetooth del centro de control (2 celdas, estilo iOS).
 *
 * Izquierda: circulo de Bluetooth (azul encendido / cristal apagado) que enciende y apaga el
 * Bluetooth del reproductor (ajuste 'bt' de la API de ajustes, en un hilo: no bloquea).
 * Con unos auriculares conectados el circulo muestra su dibujo (buds_glyph) y a la derecha su
 * nombre y la bateria (I 83 % · D 78 % · Estuche 56 %); sin ellos, "Bluetooth" y su estado.
 * Tocar el resto del modulo abre Auriculares (conectados) o Inalambrico. */
static int ccbt_on = -1;                /* -1 sin leer; se lee de user.ini (instantaneo) */
static volatile int ccbt_busy;
static void *ccbt_thread(void *a)
{
    int v = (int)(intptr_t)a;
    if (hs_set("bt", v) < 0) { int r = -1; if (!hs_get("bt", &r) && r >= 0) ccbt_on = r; }
    ccbt_busy = 0;
    return 0;
}
static int cc_bt_state(void)
{
    if (ccbt_on < 0) { int v = -1; if (!hs_ini_get("bt", &v) && v >= 0) ccbt_on = v != 0; }
    return ccbt_on > 0;
}
static int cc_bt_toggle(void)
{
    int on = !cc_bt_state();
    if (ccbt_busy) return on ? 0 : 1;
    ccbt_on = on; ccbt_busy = 1;                            /* optimista, como iOS */
    pthread_t t;
    if (pthread_create(&t, 0, ccbt_thread, (void *)(intptr_t)on)) { ccbt_busy = 0; return !on; }
    pthread_detach(t);
    return on;
}
static int cc_bt_connected(void) { struct buds_st s; return buds_status(&s) && s.conn != BS_NONE; }

static void ccbt_rune(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    /* runa de Bluetooth: dos triangulos abiertos sobre un trazo vertical */
    float w = 1.9f * s;
    bu_line(d, cx - 5 * s, cy - 5 * s, cx + 5 * s, cy + 4.5f * s, w, col);
    bu_line(d, cx + 5 * s, cy + 4.5f * s, cx, cy + 9.5f * s, w, col);
    bu_line(d, cx, cy + 9.5f * s, cx, cy - 9.5f * s, w, col);
    bu_line(d, cx, cy - 9.5f * s, cx + 5 * s, cy - 4.5f * s, w, col);
    bu_line(d, cx + 5 * s, cy - 4.5f * s, cx - 5 * s, cy + 5 * s, w, col);
}
static void cc_bt_module(uint16_t *d, int x, int y, int w, int h, int pressed)
{
    struct buds_st s; int con = buds_status(&s) && s.conn != BS_NONE, on = cc_bt_state() || con;
    cc_rrect(d, x, y, w, h, 22, rgb(255, 255, 255), pressed == 1 ? 80 : 46);         /* cristal */
    /* circulo: interruptor */
    int cr = 22, cx = x + 10 + cr, cy = y + h / 2;
    if (on) cc_rrect(d, cx - cr, cy - cr, 2 * cr, 2 * cr, (float)cr, rgb(10, 132, 255), 255);
    else cc_rrect(d, cx - cr, cy - cr, 2 * cr, 2 * cr, (float)cr, rgb(255, 255, 255), 46);
    if (pressed == 2) cc_rrect(d, cx - cr, cy - cr, 2 * cr, 2 * cr, (float)cr, 0xFFFF, 50);
    if (con) buds_glyph(d, (float)cx, (float)cy, 0.48f, s.family, 4, y, y + h);
    else { const struct sprite *g = cc_icon("bt"); if (g && g->w) blit(d, g, cx - g->w / 2, cy - g->h / 2, 32, 255); else ccbt_rune(d, (float)cx, (float)cy, 1.0f, 0xFFFF); }
    /* texto */
    int tx = cx + cr + 10, tw = x + w - 10 - tx;
    const char *t1 = con ? s.name : tr("Bluetooth");
    char t2[80];
    if (con) {
        t2[0] = 0;
        if (s.batt[0] >= 0 && s.batt[1] >= 0) snprintf(t2, sizeof t2, "%d%% · %d%%", s.batt[0], s.batt[1]);
        else if (s.batt[0] >= 0) snprintf(t2, sizeof t2, "%d%%", s.batt[0]);
        if (s.batt[2] >= 0) { size_t n = strlen(t2); snprintf(t2 + n, sizeof t2 - n, "%s%d%%", n ? " · " : "", s.batt[2]); }
        if (!t2[0]) snprintf(t2, sizeof t2, "%s", tr("Conectado"));
    } else snprintf(t2, sizeof t2, "%s", tr(on ? "Activado" : "Desactivado"));
    int sz = 13; while (sz > 11 && tw_n(t1, sz, -1) > tw) sz--;
    draw_text(d, tx, cy - 3, t1, sz, 0xFFFF, tw, 0, H);
    draw_text(d, tx, cy + 14, t2, 11, rgb(200, 200, 206), tw, 0, H);
}
