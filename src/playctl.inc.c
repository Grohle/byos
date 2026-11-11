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
 *   pc_list_send_pos(..., pos_ms)               idem empezando en pos_ms de la pista (pos= de 'L';
 *                                               la unidad del reproductor esta SIN VERIFICAR)
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
static int pc_req_pos = -1;                       /* pos= de la proxima peticion (ms; -1 = no) */

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
    if (pc_req_pos > 0) fprintf(f, "pos=%d\n", pc_req_pos);
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
#ifdef NP_COVER
/* play/pausa que tambien sirve cuando la cola se acabo: con la ultima pista terminada el
 * reproductor queda parado y su tecla de play no hace nada; entonces se vuelve a poner la
 * ultima pista conocida (desde el principio) */
static int pc_toggle_or_replay(void)
{
    struct np_info i; memset(&i, 0, sizeof i);
    if (np_get(&i) > 0 && i.stopped && !i.playing && i.path[0] && !access(i.path, R_OK)) {
        const char *pp = i.path;
        lg("play: la cola habia terminado; otra vez %s", i.path);
        /* unscanned: lo ultimo puede ser un audiolibro, la radio o Vinyl (no estan en MEDIA_TABLE:
         * sin esto la libreria contestaba "lista vacia" y no sonaba nada) */
        return pc_list_send(&pp, 1, 0, i.title[0] ? i.title : 0, 0 | PC_UNSCANNED) > 0 ? 0 : -1;
    }
    return pc_toggle();
}
#else
static int pc_toggle_or_replay(void) { return pc_toggle(); }
#endif
/* lo mismo empezando en pos_ms dentro de la pista "index" (audiolibros: reanudar) */
static int pc_list_send_pos(const char *const *paths, int n, int index, const char *title, int mode, int pos_ms)
{
    pc_req_pos = pos_ms;
    int r = pc_list_send(paths, n, index, title, mode);
    pc_req_pos = -1;
    return r;
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

/* ------------------------------------------------------------------ posicion, cola y volumen
 * Libreria v8, orden 'Q' (ver docs/research_seek_queue_volume.md). Peticion en /tmp/hibymod_qreq,
 * respuesta en /tmp/hibymod_qres. UNA peticion en vuelo cada vez (la libreria atiende la ultima
 * que encuentre): las funciones _send devuelven -EBUSY si hay otra sin contestar.
 *
 *   pc_position(&pos, &dur, &state)  ms dentro de la pista (pistas de .cue incluidas); state 0 pausa,
 *                                    1 sonando, 2 parado. Lo mismo que usa la barra de fabrica.
 *   pc_seek(ms)                      saltar a ms (absoluto en la pista); se recorta al ultimo segundo
 *   pc_queue_next(path)              reproducir despues de la actual, sin cortar lo que suena
 *   pc_queue_append(path)            anadir al final de la cola
 *   pc_queue_add(path, append, fl)   idem con PC_Q_DUP (repetir una que ya esta), PC_Q_UNSCANNED (fuera
 *                                    de la biblioteca), PC_Q_RESHUFFLE (en aleatorio: rehace el orden),
 *                                    PC_Q_REBUILD (la lista es de fabrica: copiarla a hibymod\q1 y seguir
 *                                    en el mismo punto; corte breve)
 *       -> indice donde quedo (>= 0) o <0: -ENOTSUP lista de fabrica (sin PC_Q_REBUILD), -EPERM
 *          aleatorio (sin PC_Q_RESHUFFLE), -EEXIST ya en la cola, -ENOENT no esta en la biblioteca,
 *          -ENODATA no hay lista, -EBUSY orden 'L' en curso
 *   pc_volume_set(v) / pc_volume_get(&v, &max)   volumen absoluto (secuencia de HiBy Link)
 * Para la interfaz (sin bloquear):
 *   pc_seek_async(ms), pc_volume_set_async(v): guardan el ultimo valor; pc_q_pump(&res) en cada vuelta
 *   del bucle lo manda cuando no hay nada en vuelo (volumen: como mucho 1 cada 100 ms; cada cambio
 *   reescribe user.ini) y devuelve 1 cuando llega una respuesta.
 *   pc_position_send(), pc_seek_send(ms), pc_queue_send(path, append, fl), pc_volume_send(v),
 *   pc_volume_get_send() -> id; luego pc_q_pump(&res) (o pc_q_poll(id, &res)).
 * Errores comunes: -ENXIO libreria sin cargar, -ETIMEDOUT sin respuesta (tambien libreria < v8),
 * -ENOEXEC firmware distinto (no se llama a nada). */
#include <limits.h>
#define PC_QREQ "/tmp/hibymod_qreq"
#define PC_QRES "/tmp/hibymod_qres"
#define PC_Q_DUP        1
#define PC_Q_UNSCANNED  2
#define PC_Q_RESHUFFLE  4
#define PC_Q_REBUILD    8
#define PC_Q_TIMEOUT_MS 4000
#define PC_VOL_GAP_MS   100
struct pc_q_res { int id, rc, pos, dur, state, index, count, val, max, ms; char op[12], msg[120]; };

static struct {
    int id; long sent;                   /* peticion en vuelo (0 = ninguna) */
    int want_seek, seek_ms;              /* ultimos valores pedidos por la interfaz */
    int want_vol, vol; long vol_sent;
} pc_q;

static long pc_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* escribe y avisa; id > 0 o < 0 (no marca nada en vuelo) */
static int pc_q_write(const char *op, int ms, int val, const char *path, int flags)
{
    static int seq;
    if (path && (!*path || strchr(path, '\n') || strlen(path) > 1000)) return -EINVAL;
    /* con el pid: dos procesos en el mismo segundo no repiten id (la respuesta anterior no vale) */
    int id = (int)(time(0) % 20000) * 100000 + (int)(getpid() % 1000) * 100 + (++seq % 100);
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.%d", PC_QREQ, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -errno;
    fprintf(f, "HMQ1\nid=%d\nop=%s\n", id, op);
    if (ms != INT_MIN) fprintf(f, "ms=%d\n", ms);
    if (val != INT_MIN) fprintf(f, "val=%d\n", val);
    if (path) fprintf(f, "path=%s\n", path);
    if (flags & PC_Q_DUP) fprintf(f, "dup=1\n");
    if (flags & PC_Q_UNSCANNED) fprintf(f, "unscanned=1\n");
    if (flags & PC_Q_RESHUFFLE) fprintf(f, "reshuffle=1\n");
    if (flags & PC_Q_REBUILD) fprintf(f, "rebuild=1\n");
    if (fclose(f)) { unlink(tmp); return -EIO; }
    if (rename(tmp, PC_QREQ)) { int e = -errno; unlink(tmp); return e; }
    int fd = open(HM_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -ENXIO;                   /* nadie lee la FIFO: reproductor sin la libreria */
    int w = (int)write(fd, "Q", 1);
    close(fd);
    return w == 1 ? id : -EIO;
}

/* 1 = respuesta de esa peticion en *r, 0 = aun no */
static int pc_q_poll(int id, struct pc_q_res *r)
{
    FILE *f = fopen(PC_QRES, "r");
    if (!f) return 0;
    char l[320] = "";
    if (!fgets(l, sizeof l, f)) l[0] = 0;
    fclose(f);
    memset(r, 0, sizeof *r);
    int used = 0;
    if (sscanf(l, "id=%d rc=%d op=%11s pos=%d dur=%d state=%d index=%d count=%d val=%d max=%d ms=%d %n",
               &r->id, &r->rc, r->op, &r->pos, &r->dur, &r->state, &r->index, &r->count, &r->val, &r->max, &r->ms, &used) < 11) return 0;
    if (r->id != id && r->id != -1) return 0;    /* -1: rechazada antes de leerla */
    snprintf(r->msg, sizeof r->msg, "%s", l + used);
    char *nl = strchr(r->msg, '\n'); if (nl) *nl = 0;
    return 1;
}

/* manda y la marca en vuelo (una cada vez) */
static int pc_q_send(const char *op, int ms, int val, const char *path, int flags)
{
    if (pc_q.id && pc_ms() - pc_q.sent < PC_Q_TIMEOUT_MS) return -EBUSY;
    int id = pc_q_write(op, ms, val, path, flags);
    if (id > 0) { pc_q.id = id; pc_q.sent = pc_ms(); }
    return id;
}

static int pc_position_send(void) { return pc_q_send("pos", INT_MIN, INT_MIN, 0, 0); }
static int pc_seek_send(int ms) { return ms < 0 ? -ERANGE : pc_q_send("seek", ms, INT_MIN, 0, 0); }
static int pc_queue_send(const char *path, int append, int flags)
{
    if (!path) return -EINVAL;
    return pc_q_send(append ? "append" : "next", INT_MIN, INT_MIN, path, flags);
}
static int pc_volume_send(int v) { return v < 0 ? -ERANGE : pc_q_send("vol", INT_MIN, v, 0, 0); }
static int pc_volume_get_send(void) { return pc_q_send("volget", INT_MIN, INT_MIN, 0, 0); }

static void pc_seek_async(int ms) { pc_q.seek_ms = ms < 0 ? 0 : ms; pc_q.want_seek = 1; }
static void pc_volume_set_async(int v) { pc_q.vol = v < 0 ? 0 : v; pc_q.want_vol = 1; }

/* llamar en cada vuelta del bucle de la interfaz: 1 = llego una respuesta (en *r) */
static int pc_q_pump(struct pc_q_res *r)
{
    int got = 0;
    if (pc_q.id) {
        if (pc_q_poll(pc_q.id, r)) { pc_q.id = 0; got = 1; }
        else if (pc_ms() - pc_q.sent >= PC_Q_TIMEOUT_MS) {
            memset(r, 0, sizeof *r); r->id = pc_q.id; r->rc = -ETIMEDOUT; snprintf(r->msg, sizeof r->msg, "sin respuesta");
            pc_q.id = 0; got = 1;
        }
    }
    if (!pc_q.id) {
        if (pc_q.want_seek) { if (pc_seek_send(pc_q.seek_ms) != -EBUSY) pc_q.want_seek = 0; }
        else if (pc_q.want_vol && pc_ms() - pc_q.vol_sent >= PC_VOL_GAP_MS) {
            if (pc_volume_send(pc_q.vol) != -EBUSY) { pc_q.want_vol = 0; pc_q.vol_sent = pc_ms(); }
        }
    }
    return got;
}

/* bloqueante: espera a que no haya nada en vuelo, manda y espera su respuesta; devuelve r->rc */
static int pc_q_call(const char *op, int ms, int val, const char *path, int flags, struct pc_q_res *r)
{
    long t0 = pc_ms();
    struct pc_q_res tmp;
    while (pc_q.id && pc_ms() - t0 < PC_Q_TIMEOUT_MS) {
        pc_q_pump(&tmp);
        struct timespec d = { 0, 20 * 1000000L }; nanosleep(&d, 0);
    }
    pc_q.id = 0;
    int id = pc_q_send(op, ms, val, path, flags);
    if (id < 0) { memset(r, 0, sizeof *r); r->rc = id; return id; }
    for (long t = pc_ms(); pc_ms() - t < PC_Q_TIMEOUT_MS + 2000;) {   /* + margen: rebuild escribe la base */
        if (pc_q_poll(id, r)) { pc_q.id = 0; return r->rc; }
        struct timespec d = { 0, 20 * 1000000L }; nanosleep(&d, 0);
    }
    pc_q.id = 0;
    memset(r, 0, sizeof *r); r->rc = -ETIMEDOUT;
    return -ETIMEDOUT;
}

static int pc_position(int *pos_ms, int *dur_ms, int *state)
{
    struct pc_q_res r;
    int rc = pc_q_call("pos", INT_MIN, INT_MIN, 0, 0, &r);
    if (rc) return rc;
    if (pos_ms) *pos_ms = r.pos;
    if (dur_ms) *dur_ms = r.dur;
    if (state) *state = r.state;
    return 0;
}
static int pc_seek(int ms)
{
    struct pc_q_res r;
    return ms < 0 ? -ERANGE : pc_q_call("seek", ms, INT_MIN, 0, 0, &r);
}
static int pc_queue_add(const char *path, int append, int flags)
{
    struct pc_q_res r;
    if (!path) return -EINVAL;
    int rc = pc_q_call(append ? "append" : "next", INT_MIN, INT_MIN, path, flags, &r);
    return rc < 0 ? rc : r.index;
}
static int pc_queue_next(const char *path) { return pc_queue_add(path, 0, 0); }
static int pc_queue_append(const char *path) { return pc_queue_add(path, 1, 0); }
static int pc_volume_set(int v)
{
    struct pc_q_res r;
    return v < 0 ? -ERANGE : pc_q_call("vol", INT_MIN, v, 0, 0, &r);
}
static int pc_volume_get(int *v, int *max)
{
    struct pc_q_res r;
    int rc = pc_q_call("volget", INT_MIN, INT_MIN, 0, 0, &r);
    if (rc) return rc;
    if (v) *v = r.val;
    if (max) *max = r.max;
    return 0;
}
