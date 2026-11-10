/* power.inc.c - modos de energia y "Solo USB (no cargar la bateria)".
 * Diseno y medidas: docs/research_overclock_power.md. Sin overclock: la CPU ya va a
 * 1104 MHz (un 10 % sobre la hoja de datos) y la interfaz la limita el panel.
 *
 *  Ahorro:       pantalla a 30 Hz (30 fps), brillo como mucho 40, vm.dirty_expire 60 s
 *  Normal:       44 fps (el maximo del panel), brillo del usuario, 30 s
 *  Rendimiento:  pantalla a 60 Hz (reloj del LCD), sin topes; pensado para ir enchufado
 *
 * Solo USB: el AXP2101 deja de cargar (axp_battery/charger = 0) y el aparato se alimenta
 * del USB. El driver vuelve a activar la carga ~5 s despues de cada enchufado, y "cat
 * charger" no es fiable, asi que el estado deseado vive aqui y se reafirma: 7 s despues de
 * enchufar y cada 30 s mientras siga enchufado. Configuracion: /usr/data/hibymod/power.conf */
#define POWER_CONF "/usr/data/hibymod/power.conf"
#define BL_PATH "/sys/class/backlight/backlight_pwm0/brightness"
#define CHG_PATH "/sys/class/power_supply/axp_battery/charger"
#define USB_ONLINE "/sys/class/power_supply/usb/online"
enum { PW_SAVE, PW_NORMAL, PW_PERF };
static int pw_mode = PW_NORMAL, pw_nocharge, pw_user_bl = -1;
static void lcd_apply(void); static int lcd_hz;
/* refresco en reposo: sin tocar la pantalla 1,5 s se baja al minimo viable; cualquier toque o
 * tecla lo devuelve al del modo ANTES del primer fotograma (lcd_boost, microsegundos).
 * hz_hold > 0: una pantalla pide refresco completo (video, navegador, letra). */
static int lcd_idle_hz = 30, hz_hold, lcd_cur_hz;
static double lcd_last_input;
static void lcd_boost(void);
static double pw_plug_t, pw_chg_t, pw_tick_t;
static int pw_online = -1;

static int sysfs_get(const char *p, int def)
{
    char b[24]; int f = open(p, O_RDONLY); if (f < 0) return def;
    int n = (int)read(f, b, sizeof b - 1); close(f); if (n <= 0) return def; b[n] = 0; return atoi(b);
}
static int sysfs_put(const char *p, int v)
{
    char b[24]; int n = snprintf(b, sizeof b, "%d\n", v), f = open(p, O_WRONLY); if (f < 0) return -1;
    int r = (int)write(f, b, n); close(f); return r == n ? 0 : -1;
}
static void pw_save(void)
{
    FILE *f = fopen(POWER_CONF ".tmp", "w"); if (!f) return;
    fprintf(f, "mode %d\nnocharge %d\nuser_bl %d\n", pw_mode, pw_nocharge, pw_user_bl);
    fclose(f); rename(POWER_CONF ".tmp", POWER_CONF);
}
static void pw_apply(void)
{
    lcd_hz = pw_mode == PW_PERF ? 48 : pw_mode == PW_SAVE ? 30 : 45; lcd_apply();      /* Ahorro 30, Normal 45, Rendimiento 48,7 Hz (60 Hz: rayas, fuera de especificacion) */
    int bl = sysfs_get(BL_PATH, -1);
    if (pw_mode == PW_SAVE) {
        if (bl > 40) { pw_user_bl = bl; sysfs_put(BL_PATH, 40); }
        sysfs_put("/proc/sys/vm/dirty_expire_centisecs", 6000);
    } else {
        if (pw_user_bl > 0 && bl >= 0 && bl <= 40) sysfs_put(BL_PATH, pw_user_bl);
        pw_user_bl = -1;
        sysfs_put("/proc/sys/vm/dirty_expire_centisecs", 3000);
    }
    pw_save();
}
static void pw_set_nocharge(int on)
{
    pw_nocharge = on;
    if (!on) sysfs_put(CHG_PATH, 1);
    else if (sysfs_get(USB_ONLINE, 0)) { sysfs_put(CHG_PATH, 0); pw_chg_t = now_ms(); }
    pw_save();
}
static void pw_load(void)
{
    FILE *f = fopen(POWER_CONF, "r"); char k[32]; int v;
    while (f && fscanf(f, "%31s %d", k, &v) == 2) {
        if (!strcmp(k, "mode") && v >= 0 && v <= 2) pw_mode = v;
        else if (!strcmp(k, "nocharge")) pw_nocharge = v != 0;
        else if (!strcmp(k, "user_bl")) pw_user_bl = v;
        else if (!strcmp(k, "lcd_hz")) lcd_hz = v == 60 ? 60 : 45;
    }
    if (f) fclose(f);
    lcd_hz = pw_mode == PW_PERF ? 48 : pw_mode == PW_SAVE ? 30 : 45;
    if (pw_mode == PW_SAVE) pw_apply();
    lcd_apply();
}
/* llamado en cada vuelta del bucle principal (barato: lee sysfs como mucho 1 vez/s) */
static void pw_tick(void)
{
    double t = now_ms(); if (t - pw_tick_t < 1000) return; pw_tick_t = t;
    lcd_apply();                                /* reposo o el driver lo restauro al encender la pantalla */
    int on = sysfs_get(USB_ONLINE, 0);
    if (on != pw_online) { pw_online = on; pw_plug_t = t; pw_chg_t = 0; }
    if (pw_nocharge && on && t - pw_plug_t > 7000 && (!pw_chg_t || t - pw_chg_t > 30000)) { sysfs_put(CHG_PATH, 0); pw_chg_t = t; }
    /* el reproductor reescribe el brillo al despertar: mantener el tope de Ahorro */
    if (pw_mode == PW_SAVE && ((int)(t / 1000) % 5) == 0) { int bl = sysfs_get(BL_PATH, -1); if (bl > 40) { pw_user_bl = bl; sysfs_put(BL_PATH, 40); } }
}
/* tope de fotogramas: 30 fps en Ahorro (llamado desde present) */
static void pw_frame_cap(void)
{
    static double last;
    if (pw_mode == PW_SAVE) { double w = last + 33.3 - now_ms(); if (w > 0 && w < 40) msleep((int)w); }
    last = now_ms();
}
static const char *pw_name(int m) { return m == PW_SAVE ? "Ahorro" : m == PW_PERF ? "Rendimiento" : "Normal"; }

/* ------------------------------------------------------------------ frecuencia de la pantalla
 * El panel (ST77903) se barre continuamente desde la DDR; HiBy lo configura a 45 Hz
 * (reloj LCD = MPLL/33, CPM LPCDR 0x10000064 = 0x44000020). Con el divisor /24 da 60,9 Hz
 * (medido en el aparato: 16,5 ms por fotograma). Secuencia de Ingenic: escribir con CE,
 * esperar a que BUSY se limpie y quitar CE. El driver puede restaurarlo al encender la
 * pantalla: pw_tick lo comprueba cada segundo y lo reaplica. docs/research_60fps.md */
#define LPCDR_PHYS 0x10000064u
static int lcd_hz = 45;                   /* 30 (Ahorro), 45 (de serie) o 60 (Rendimiento) */
static volatile uint32_t *lpcdr;
static uint32_t hz_div(int hz) { return hz >= 60 ? 0x17 : hz >= 48 ? 0x1d : hz <= 20 ? 0x48 : hz <= 24 ? 0x3c : hz <= 30 ? 0x30 : 0x20; }   /* MPLL/(div+1): 60,9 / 44,3 / 29,8 / 24,0 / 19,8 Hz */
static int lcd_want_hz(void)
{
    int idle = !hz_hold && now_ms() - lcd_last_input > 1500;
    return idle && lcd_idle_hz < lcd_hz ? lcd_idle_hz : lcd_hz;
}
static uint32_t lcd_div(void) { return hz_div(lcd_cur_hz = lcd_want_hz()); }
/* DESACTIVADO (2026-11-10): cambiar el reloj del LCD en caliente coincidio con corrupcion de la
 * memoria del kernel (oops en ubi_eba_write_leb y /usr/data colgado) en dos ocasiones, con el
 * refresco en reposo alternando 45/30 Hz. Hasta entender la causa, el LCD se queda en el valor
 * de fabrica (45 Hz) en todos los modos; el modo Ahorro limita fotogramas solo por software. */
#define LCD_CLOCK_CHANGES 0
static void lcd_apply(void)
{
    if (!LCD_CLOCK_CHANGES) return;
    if (!lpcdr) {
        int f = open("/dev/mem", O_RDWR | O_SYNC); if (f < 0) return;
        void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, LPCDR_PHYS & ~0xFFFu); close(f);
        if (m == MAP_FAILED) return;
        lpcdr = (volatile uint32_t *)((char *)m + (LPCDR_PHYS & 0xFFF));
    }
    uint32_t v = *lpcdr;
    if ((v & 0xFF) == lcd_div() || (v & 0xC0000000u) != 0x40000000u) return;   /* ya esta / fuente inesperada: no tocar */
    uint32_t base = (v & 0xC4000000u);          /* fuente (MPLL) y bit 26 del driver */
    *lpcdr = base | 0x20000000u | lcd_div();    /* CE */
    for (int i = 0; i < 1000 && (*lpcdr & 0x10000000u); i++) {}   /* BUSY */
    *lpcdr = base | lcd_div();
    if (lcd_cur_hz != lcd_hz) {} else lg("pantalla: %d Hz (LPCDR %08x)", lcd_hz, *lpcdr);
}
/* llamado con cada evento de entrada: vuelve al refresco del modo al instante */
static void lcd_boost(void)
{
    lcd_last_input = now_ms();
    if (lcd_cur_hz != lcd_hz) lcd_apply();
}
