/* playctl.inc.c - controlar la reproduccion del reproductor de HiBy desde el shell.
 * Ver docs/research_playback.md.
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
 *   pc_play_list(paths, n, index, title, mode)  reproduce una lista con el motor de fabrica
 *   pc_play_file(path)                          = lista de 1
 *   pc_list_send(...) / pc_list_poll(id, &res)  lo mismo sin bloquear (para el bucle de la UI)
 *     -> necesitan hibymod_disp.so v6 (orden 'L'); ver docs/research_play_list.md
 * pc_play/pc_pause necesitan nowplaying.inc.c (incluirlo antes) para saber el estado. */
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "hm_shared.h"

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

/* ------------------------------------------------------------------ reproducir una lista
 * La libreria v6 (dentro de hiby_player) crea list_tb_7701/7702 + su fila de MANAGER_TABLE con
 * la conexion SQLite del reproductor y hace la misma secuencia que la interfaz de fabrica al
 * tocar una cancion (set_param 0x17/0x15/8/0x10 + play_index). La cola queda en el reproductor:
 * siguiente/anterior, aleatorio, "reanudar al encender" y su Now Playing siguen funcionando.
 *   paths: rutas Linux (/data/mnt/sd_0/...) o del reproductor (a:\...); "CUE|ruta" elige la
 *          pista de un .cue (cue_id). Solo entran las que esten en MEDIA_TABLE (biblioteca
 *          escaneada), salvo PC_UNSCANNED en mode.
 *   index: entrada por la que empezar (en el orden de paths)
 *   mode:  -1 (o 0xff) no tocar; 0 en orden, 1 repetir pista, 2 aleatorio, 3 repetir lista (es un
 *          ajuste persistente del reproductor, como cambiarlo en su menu) [I]
 * Respuesta: >= 0 filas de la lista; < 0 error (-ENXIO: libreria sin cargar / sin v6,
 * -ETIMEDOUT, -ENOEXEC: firmware distinto, < -1000: error SQLite + 1000). */
#define PC_REQ "/tmp/hibymod_playreq"
#define PC_RES "/tmp/hibymod_playres"
#define PC_UNSCANNED 0x100
struct pc_list_res { int id, rc, rows, index, slot, ms; char msg[120]; };

static int pc_list_write(const char *const *paths, int n, int index, const char *title, int mode, const char *list)
{
    static int seq;
    int id = (int)(time(0) % 100000) * 100 + (++seq % 100);
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.%d", PC_REQ, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -errno;
    fprintf(f, "HMPL1\nid=%d\nindex=%d\n", id, index < 0 ? 0 : index);
    if (title && *title) {                       /* una linea, sin saltos */
        char t[64]; snprintf(t, sizeof t, "%s", title);
        for (char *c = t; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        fprintf(f, "title=%s\n", t);
    }
    if (mode >= 0 && (mode & 0xff) <= 3) fprintf(f, "mode=%d\n", mode & 0xff);   /* 0xff = no tocar */
    if (mode >= 0 && (mode & PC_UNSCANNED)) fprintf(f, "unscanned=1\n");
    if (list && !strcmp(list, "\001check")) fprintf(f, "check=1\n");
    else if (list && !strcmp(list, "\001drop")) fprintf(f, "drop=1\n");
    else if (list) fprintf(f, "list=%s\n", list);
    for (int i = 0; i < n; i++) if (paths[i] && !strchr(paths[i], '\n')) fprintf(f, "f=%s\n", paths[i]);
    if (fclose(f)) { unlink(tmp); return -EIO; }
    if (rename(tmp, PC_REQ)) { int e = -errno; unlink(tmp); return e; }
    int fd = open(HM_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -ENXIO;                   /* nadie lee la FIFO: reproductor sin la libreria */
    int w = (int)write(fd, "L", 1);
    close(fd);
    return w == 1 ? id : -EIO;
}

/* sin bloquear: devuelve el id de la peticion (> 0) o < 0 */
static int pc_list_send(const char *const *paths, int n, int index, const char *title, int mode)
{
    if (!paths || n <= 0) return -EINVAL;
    return pc_list_write(paths, n, index, title, mode, 0);
}
/* lista que ya existe en el reproductor (clave de MANAGER_TABLE, p. ej. "album_artist\ABBA") */
static int pc_listpath_send(const char *listpath, int index)
{
    if (!listpath || !*listpath || *listpath == '\001') return -EINVAL;
    return pc_list_write(0, 0, index, 0, -1, listpath);
}
/* comprobar firmas y leer estado (lista, indice, modo) sin tocar nada / borrar nuestras tablas */
static int pc_list_check_send(void) { return pc_list_write(0, 0, 0, 0, -1, "\001check"); }
static int pc_list_drop_send(void) { return pc_list_write(0, 0, 0, 0, -1, "\001drop"); }

/* 1 = respuesta lista en *r, 0 = aun no, < 0 error */
static int pc_list_poll(int id, struct pc_list_res *r)
{
    FILE *f = fopen(PC_RES, "r");
    if (!f) return 0;
    char l[256] = "";
    if (!fgets(l, sizeof l, f)) l[0] = 0;
    fclose(f);
    memset(r, 0, sizeof *r);
    int used = 0;
    if (sscanf(l, "id=%d rc=%d rows=%d index=%d slot=%d ms=%d %n", &r->id, &r->rc, &r->rows, &r->index, &r->slot, &r->ms, &used) < 6) return 0;
    if (r->id != id && r->id != -1) return 0;     /* -1: rechazada antes de leer (ocupado...) */
    snprintf(r->msg, sizeof r->msg, "%s", l + used);
    char *nl = strchr(r->msg, '\n'); if (nl) *nl = 0;
    return 1;
}

static int pc_list_wait(int id, int timeout_ms, struct pc_list_res *r)
{
    if (id < 0) return id;
    for (int t = 0; t < timeout_ms; t += 20) {
        int k = pc_list_poll(id, r);
        if (k) return r->rc < 0 ? r->rc : r->rows;
        struct timespec d = { 0, 20 * 1000000L }; nanosleep(&d, 0);
    }
    return -ETIMEDOUT;
}

static int pc_play_list(const char *const *paths, int n, int index, const char *title, int mode)
{
    struct pc_list_res r;
    unlink(PC_RES);
    return pc_list_wait(pc_list_send(paths, n, index, title, mode), 5000, &r);
}

static int pc_play_file(const char *path)
{
    const char *p[1] = { path };
    return pc_play_list(p, 1, 0, 0, -1);
}
