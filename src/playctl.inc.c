/* playctl.inc.c - controlar la reproduccion del reproductor de HiBy desde el shell.
 * Ver docs/investigacion_reproduccion.md.
 *
 * Como: se escriben pulsaciones en los nodos evdev de los botones fisicos. Un write() en
 * /dev/input/eventN inyecta el evento en ese dispositivo (input_inject_event) y le llega a
 * todos los que lo tienen abierto, el reproductor incluido: es exactamente como pulsar el
 * boton (funciona con la pantalla apagada y en cualquier vista). Verificado 2026-11-10:
 * KEY_PLAYPAUSE (164) en "jz adc keyboard" arranca y pausa.
 *   KEY_PLAYPAUSE 164, KEY_NEXTSONG 163: "jz adc keyboard"; KEY_PREVIOUSSONG 165: "md-gpio-keys"
 * (tambien "earpods_adc", pero se usa el primero que tenga la tecla).
 *
 *   pc_toggle() pc_play() pc_pause() pc_next() pc_prev()      0 = enviado, <0 error
 *   pc_play_file(path)  -> -ENOSYS: el reproductor no expone una orden "reproduce esto"
 *                          (ver el documento: opciones y su coste)
 * pc_play/pc_pause necesitan nowplaying.inc.c (incluirlo antes) para saber el estado. */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define PC_BITS_LONG (8 * sizeof(long))
static char pc_path[3][24];                       /* cache por tecla: 163, 164, 165 */

static int pc_has_key(int fd, int code)
{
    unsigned long b[(KEY_MAX + PC_BITS_LONG) / PC_BITS_LONG];
    memset(b, 0, sizeof b);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof b), b) < 0) return 0;
    return (b[code / PC_BITS_LONG] >> (code % PC_BITS_LONG)) & 1;
}

/* dispositivo fisico con esa tecla (no los virtuales: uinput del shell o de la libreria) */
static int pc_open_for(int code)
{
    int slot = code - KEY_NEXTSONG;
    if (slot >= 0 && slot < 3 && pc_path[slot][0]) {
        int f = open(pc_path[slot], O_WRONLY | O_CLOEXEC);
        if (f >= 0) return f;
        pc_path[slot][0] = 0;
    }
    for (int i = 0; i < 16; i++) {
        char p[24]; snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDWR | O_CLOEXEC);
        if (f < 0) continue;
        struct input_id id; memset(&id, 0, sizeof id);
        ioctl(f, EVIOCGID, &id);
        if (id.bustype != BUS_VIRTUAL && pc_has_key(f, code)) {
            if (slot >= 0 && slot < 3) snprintf(pc_path[slot], sizeof pc_path[slot], "%s", p);
            return f;
        }
        close(f);
    }
    return -ENODEV;
}

static int pc_send(int f, int code, int value)
{
    struct input_event e[2]; memset(e, 0, sizeof e);
    /* la hora la pone el kernel al inyectar */
    e[0].type = EV_KEY; e[0].code = (unsigned short)code; e[0].value = value;
    e[1].type = EV_SYN; e[1].code = SYN_REPORT;
    return write(f, e, sizeof e) == (ssize_t)sizeof e ? 0 : -EIO;   /* evdev: eventos enteros */
}

/* pulsa la tecla hold_ms (80 = pulsacion normal; el reproductor puede tratar > ~800 ms como
 * pulsacion larga, p. ej. avance rapido con NEXT: SIN VERIFICAR) */
static int pc_key(int code, int hold_ms)
{
    int f = pc_open_for(code);
    if (f < 0) return f;
    int r = pc_send(f, code, 1);
    struct timespec d = { hold_ms / 1000, (hold_ms % 1000) * 1000000L };
    nanosleep(&d, 0);
    if (pc_send(f, code, 0)) r = -EIO;
    close(f);
    return r;
}

static int pc_toggle(void) { return pc_key(KEY_PLAYPAUSE, 80); }
static int pc_next(void) { return pc_key(KEY_NEXTSONG, 80); }
static int pc_prev(void) { return pc_key(KEY_PREVIOUSSONG, 80); }   /* > 3 s de cancion: suele volver al inicio */

#ifdef NP_COVER
/* solo pulsa si hace falta (play/pause sobre una sola tecla); 1 = ya estaba asi */
static int pc_play(void)
{
    struct np_info i;
    if (np_get(&i) && i.playing) return 1;
    return pc_toggle();
}
static int pc_pause(void)
{
    struct np_info i;
    if (!np_get(&i) || !i.playing) return 1;
    return pc_toggle();
}
#endif

static int pc_play_file(const char *path) { (void)path; return -ENOSYS; }
