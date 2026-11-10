/* power.inc.c - modos de energia y "Solo USB (no cargar la bateria)".
 * Diseno y medidas: docs/investigacion_overclock_energia.md. Sin overclock: la CPU ya va a
 * 1104 MHz (un 10 % sobre la hoja de datos) y la interfaz la limita el panel.
 *
 *  Ahorro:       30 fps en las animaciones, brillo como mucho 40, vm.dirty_expire 60 s
 *  Normal:       44 fps (el maximo del panel), brillo del usuario, 30 s
 *  Rendimiento:  como Normal; pensado para ir enchufado (sin topes)
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
    }
    if (f) fclose(f);
    if (pw_mode == PW_SAVE) pw_apply();
}
/* llamado en cada vuelta del bucle principal (barato: lee sysfs como mucho 1 vez/s) */
static void pw_tick(void)
{
    double t = now_ms(); if (t - pw_tick_t < 1000) return; pw_tick_t = t;
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
