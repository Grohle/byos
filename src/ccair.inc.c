/* ccair.inc.c - Centro de control: modo avion y control de ruido de los auriculares.
 *
 * Modo avion (como iOS): apaga Wi-Fi y Bluetooth recordando cual estaba encendido; al quitarlo
 * vuelve a encender solo esos. El estado se guarda en AIR_FILE ("wifi bt" anteriores), asi
 * sobrevive a un reinicio. Si el usuario enciende Wi-Fi o Bluetooth a mano, el modo avion se
 * da por terminado (cc_air_radio_on). Con ADB por Wi-Fi pide confirmacion antes de cortar.
 * Los ajustes del reproductor se cambian en un hilo: el panel nunca se bloquea.
 *
 * Control de ruido: recorre los modos que admiten los auriculares conectados (desactivado,
 * cancelacion, ambiente/transparencia, adaptativo) con buds_set_mode(). Nunca falla en
 * silencio: sin auriculares, con el canal de control aun abriendose, o con un modelo sin
 * control de ruido, el boton se ve atenuado y al tocarlo explica el motivo; si el daemon
 * rechaza el cambio, el rotulo muestra su error y el boton vuelve al modo real. */
#define AIR_FILE "/usr/data/hibymod/shell/airplane"

static int air_on = -1, air_prev_wifi, air_prev_bt;
static volatile int air_busy;

static void air_load(void)
{
    if (air_on >= 0) return;
    air_on = 0;
    FILE *f = fopen(AIR_FILE, "r"); if (!f) return;
    int w = 0, b = 0;
    if (fscanf(f, "%d %d", &w, &b) == 2) { air_on = 1; air_prev_wifi = w != 0; air_prev_bt = b != 0; }
    fclose(f);
}
static void air_save(void)
{
    if (!air_on) { unlink(AIR_FILE); return; }
    FILE *f = fopen(AIR_FILE, "w"); if (!f) return;
    fprintf(f, "%d %d\n", air_prev_wifi, air_prev_bt); fclose(f);
}
static int cc_air_state(void) { air_load(); return air_on > 0; }
static void cc_air_radio_on(void) { air_load(); if (air_on > 0) { air_on = 0; air_save(); lg("modo avion: fin (radio encendida a mano)"); } }

/* a: bit0 cambiar Wi-Fi, bit1 su valor, bit2 cambiar Bluetooth, bit3 su valor */
static void *air_thread(void *a)
{
    int v = (int)(intptr_t)a;
    if (v & 4) {
        int bv = v >> 3 & 1;
        if (wl_hs_set("bt", bv) < 0) { int r = -1; if (!hs_get("bt", &r) && r >= 0) ccbt_on = r; }
    }
    if (v & 1) wl_hs_set("wifi", v >> 1 & 1);
    if (wl_started) { wl_push(&wl_qw, J_HSREAD, 0, 0, 0); wl_push(&wl_qw, J_REFRESH, 0, 0, 0); wl_push(&wl_qb, J_REFRESH, 0, 0, 0); }
    air_busy = 0;
    return 0;
}
static int air_wifi_now(void)
{
    int w = 0;
    if (hs_ini_get("wifi", &w)) w = 0;
    if (wl_started) { pthread_mutex_lock(&wl_mu); if (WS.wpend) w = WS.wpend - 1; else if (WS.wset >= 0) w = WS.wset; pthread_mutex_unlock(&wl_mu); }
    return w != 0;
}
static int cc_air_toggle(const char **why)
{
    air_load();
    if (why) *why = 0;
    if (air_busy || ccbt_busy) { if (why) *why = tr("Espera a que termine la operación"); return -1; }
    int on = !air_on, job;
    if (on) {
        int w = air_wifi_now(), b = cc_bt_state();
        /* ADB por Wi-Fi: apagar el Wi-Fi corta la sesion; hay que confirmarlo (sin rotulo si se cancela) */
        if (w && wl_adb_wifi() && !confirm(tr("WL_ADB_WARN"), tr("Desactivar Wi-Fi"))) return -1;
        air_prev_wifi = w; air_prev_bt = b;
        job = (w ? 1 : 0) | (b ? 4 : 0);                   /* apagar lo que este encendido */
        if (b) ccbt_on = 0;
    } else {
        job = (air_prev_wifi ? 1 | 2 : 0) | (air_prev_bt ? 4 | 8 : 0);   /* encender lo que lo estaba */
        if (air_prev_bt) ccbt_on = 1;
    }
    air_on = on; air_save();
    lg("modo avion: %s (wifi %d, bt %d)", on ? "activado" : "desactivado", air_prev_wifi, air_prev_bt);
    if (job) {
        air_busy = 1;
        pthread_t t;
        if (pthread_create(&t, 0, air_thread, (void *)(intptr_t)job)) {      /* sin hilo no se cambia nada: deshacer */
            air_busy = 0; air_on = !on; air_save(); ccbt_on = -1;
            if (why) *why = tr("No se pudo cambiar el ajuste");
            return -1;
        }
        pthread_detach(t);
    }
    return on;
}

/* ---- control de ruido ---- */
enum { CN_OK, CN_NONE, CN_WAIT, CN_UNSUP };
static unsigned ccn_gen, ccn_errn; static int ccn_wait; static double ccn_t;
/* estado del boton: CN_OK (s->noise valido), sin auriculares, canal aun no listo, o sin control de ruido */
static int ccn_state(struct buds_st *s)
{
    if (!buds_status(s) || s->conn == BS_NONE) return CN_NONE;
    if (s->conn == BS_AUDIO || s->conn == BS_LINKING) return CN_WAIT;
    if (s->conn != BS_READY || !(s->caps & BC_NOISE) || (s->caps & BC_RO)) return CN_UNSUP;
    int n = 0; for (int m = 0; m < BN_N; m++) if (s->noise_mask & BUDS_MASK(m)) n++;
    return n >= 2 ? CN_OK : CN_UNSUP;
}
static void cc_noise_tile(uint16_t *d, int x, int y)
{
    struct buds_st s; int st = ccn_state(&s);
    int m = st == CN_OK && s.noise >= 0 && s.noise < BN_N ? s.noise : -1;
    int on = m == BN_ANC || m == BN_AMBIENT || m == BN_ADAPTIVE;
    if (on) cc_rrect(d, x, y, CC_T, CC_T, CC_T / 2.f, rgb(10, 132, 255), 255);
    else cc_rrect(d, x, y, CC_T, CC_T, CC_T / 2.f, rgb(255, 255, 255), st == CN_OK ? 46 : 26);
    uint16_t bg = d[(y + CC_T / 2) * W + x + CC_T / 2];
    /* no disponible: la figura sola y atenuada (sin marcas de modo) */
    bu_mode_icon(d, x + CC_T / 2.f, y + CC_T / 2.f + 1, st == CN_OK ? (m < 0 ? BN_OFF : m) : BN_N, st == CN_OK ? 0xFFFF : rgb(150, 150, 156), bg);
}
static const char *cc_noise_tap(void)
{
    struct buds_st s; int st = ccn_state(&s);
    if (st == CN_NONE) return bu_err_text(BE_NOTCONN);
    if (st == CN_WAIT) return bu_err_text(BE_BUSY);
    if (st == CN_UNSUP) return bu_err_text(BE_NOTSUP);
    int cur = s.noise >= 0 && s.noise < BN_N ? s.noise : BN_OFF, nx = -1;
    for (int k = 1; k < BN_N && nx < 0; k++) { int c = (cur + k) % BN_N; if (s.noise_mask & BUDS_MASK(c)) nx = c; }
    if (nx < 0) return bu_err_text(BE_NOTSUP);
    ccn_errn = bu_err_n;
    if (buds_set_mode(nx) < 0) return bu_err_text(BE_IO);                /* generico: "No se pudo cambiar el ajuste" */
    ccn_wait = 1; ccn_t = now_ms();
    return bu_mode_name(&s, nx);
}
/* en reposo: los auriculares cambiaron (conexion, modo confirmado o devuelto) o el daemon
 * rechazo el ultimo cambio de modo -> repintar; el error sale como rotulo */
static int cc_ext_poll(void)
{
    int ch = 0;
    if (ccn_wait) {
        if (bu_err_n != ccn_errn) { ccn_wait = 0; cc_set_caption(bu_err_text(bu_err_code)); ch = 1; }
        else if (now_ms() - ccn_t > 6000) ccn_wait = 0;
    }
    unsigned g = bu_gen;
    if (g != ccn_gen) { ccn_gen = g; ch = 1; }
    return ch;
}
