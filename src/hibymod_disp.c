/* hibymod_disp.so v6 - puente de pantalla y entrada entre hiby_player y hibyshell
 * (LD_PRELOAD en hiby_player; ver hm_shared.h).
 *
 * PANTALLA: hiby_player compone con su DMA directamente en /dev/fb0 (2 bufers, sin
 * hueco para un tercero), asi que solo se intercepta la PRESENTACION (FBIOPAN_DISPLAY):
 *   - normal: paso directo; si hay capa superpuesta se mezcla en el bufer antes de presentarlo
 *     (guardando lo que tapa para poder quitarla);
 *   - 'H' retener: el hilo que presenta espera (el audio sigue en otros hilos);
 *   - 'C' captura: el panel muestra la imagen del shell (frames[0]); cada fotograma del
 *     reproductor se copia a frames[1] y en su bufer se pone frames[0] antes de presentarlo,
 *     asi el reproductor sigue alternando bufers con normalidad sin que se vea;
 *   - 'P' el shell presenta frames[0] (animaciones), serializado con el reproductor;
 *   - 'R' devolver; 'O' redibujar la capa superpuesta.
 *
 * ENTRADA: el reproductor lee la tactil desde un uinput identico ("goodix-ts"). Sin shell,
 * un hilo reenvia la tactil real (paso directo). Si hibyshell se conecta al socket, el shell
 * captura la real (EVIOCGRAB) y manda por el socket lo que debe recibir el reproductor; si
 * el shell muere se vuelve solo al paso directo. Si algo falla al prepararse, no se
 * redirige nada (comportamiento de fabrica).
 *
 * VISTAS: se anota cada .view que abre el reproductor (registro circular) para que el shell
 * sepa en que pantalla esta.
 *
 * LO QUE SUENA (v5): se anotan los ficheros de audio que abre el reproductor (open/fopen/
 * close/lseek) y lo que escribe al PCM (ioctl WRITEI de tinyalsa o snd_pcm_writei de
 * libasound): tramas, frecuencia, vaciados. El shell lo interpreta (src/nowplaying.inc.c).
 * Coste: unas comparaciones de fd por llamada; nada de esto toca el mutex de la pantalla.
 *
 * REPRODUCIR UNA LISTA (v6, orden 'L'): ver la seccion final y docs/research_play_list.md.
 * Inerte mientras nadie mande 'L'.
 *
 * AJUSTES DEL REPRODUCTOR (v7, orden 'S'): leer/cambiar BT, Wi-Fi, DLNA, AirPlay, ganancia,
 * MSEB, volumen... con la misma secuencia que su centro de control. Ver la ultima seccion y
 * docs/research_settings_api.md. Inerte mientras nadie mande 'S'.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sched.h>
#include <strings.h>
#include <sound/asound.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "hm_shared.h"

#define HOLD_MAX_S 120
#define BOOT_HOLD_S 30         /* retencion maxima durante la animacion de arranque */
static int boot_hold;
_Static_assert(sizeof(struct hm_state) <= HM_STATE_SIZE, "hm_state no cabe en HM_STATE_SIZE");

static int active, fb_fd = -1;
static struct hm_state *st;
static struct fb_var_screeninfo last_var;
static int have_var;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int (*r_open)(const char *, int, ...);
static int (*r_open64)(const char *, int, ...);
static int (*r_openat)(int, const char *, int, ...);
static int (*r_openat64)(int, const char *, int, ...);
static FILE *(*r_fopen)(const char *, const char *);
static FILE *(*r_fopen64)(const char *, const char *);
static int (*r_ioctl)(int, unsigned long, ...);
static int (*r_close)(int);
static off_t (*r_lseek)(int, off_t, int);
static off64_t (*r_lseek64)(int, off64_t, int);

static char real_ts[32], virt_ts[32];
static int uin = -1;

static unsigned char *fbmem, *frames;
static size_t fsize, fline;

static void resolve(void)
{
    if (r_ioctl) return;
    r_open = dlsym(RTLD_NEXT, "open");
    r_open64 = dlsym(RTLD_NEXT, "open64");
    r_openat = dlsym(RTLD_NEXT, "openat");
    r_openat64 = dlsym(RTLD_NEXT, "openat64");
    r_fopen = dlsym(RTLD_NEXT, "fopen");
    r_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    r_close = dlsym(RTLD_NEXT, "close");
    r_lseek = dlsym(RTLD_NEXT, "lseek");
    r_lseek64 = dlsym(RTLD_NEXT, "lseek64");
    __sync_synchronize();
    r_ioctl = dlsym(RTLD_NEXT, "ioctl");      /* el ultimo: resolve() sale pronto si ya esta */
}

/* ---------------------------------------------------------------- memoria de pantalla */
static int map_fb(void)
{
    if (fbmem) return 1;
    if (fb_fd < 0 || !have_var) return 0;
    struct fb_fix_screeninfo x;
    if (r_ioctl(fb_fd, FBIOGET_FSCREENINFO, &x)) return 0;
    fline = x.line_length; fsize = fline * last_var.yres;
    if (fsize != HM_FRAME_BYTES) return 0;
    void *m = mmap(0, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (m == MAP_FAILED) return 0;
    fbmem = m;
    return 1;
}

static int map_frames(void)
{
    if (frames) return 1;
    if (!map_fb()) return 0;
    int f = r_open(HM_FRAMES, O_RDWR);
    if (f < 0) return 0;
    struct stat s;
    if (fstat(f, &s) || (size_t)s.st_size < 2 * fsize) { close(f); return 0; }
    void *m = mmap(0, 2 * fsize, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    close(f);
    if (m == MAP_FAILED) return 0;
    frames = m;
    return 1;
}

static unsigned char *buf_at(uint32_t yoffset) { return fbmem + (size_t)yoffset * fline; }

/* ---------------------------------------------------------------- sustitucion del inicio */
static unsigned char *homeimg;
static int is_home_frame(const unsigned char *b)
{
    uint32_t n = st->nsig; if (!n || n > 8) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int x = st->sig[i][0], y = st->sig[i][1];
        if (x < 0 || x >= HM_W || y < 0 || y >= HM_H) return 0;
        uint16_t p = ((const uint16_t *)(b + y * fline))[x];
        int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, bl = (p & 31) * 255 / 31, t = st->sig[i][5];
        if (abs(r - st->sig[i][2]) > t || abs(g - st->sig[i][3]) > t || abs(bl - st->sig[i][4]) > t) return 0;
    }
    return 1;
}
static void home_substitute(unsigned char *b)
{
    if (!st->home_sub_on || !map_fb()) return;
    if (!homeimg) {
        int f = r_open(HM_HOME, O_RDONLY); if (f < 0) return;
        void *m = mmap(0, HM_FRAME_BYTES, PROT_READ, MAP_SHARED, f, 0); close(f);
        if (m == MAP_FAILED) return;
        homeimg = m;
    }
    if (!is_home_frame(b)) return;
    /* el resto, el launcher; la barra de estado del reproductor (negra con iconos) se vuelve
     * transparente sobre el fondo, igual que la dibuja el shell (filas 0-29 de homeimg = fondo) */
    memcpy(b + 30 * fline, homeimg + 30 * fline, fsize - 30 * fline);
    for (int y = 0; y < 30; y++) {
        uint16_t *d = (uint16_t *)(b + y * fline); const uint16_t *w = (const uint16_t *)(homeimg + y * fline);
        int k = 22 + y * 10 / 30;
        for (int x = 0; x < HM_W; x++) {
            uint16_t p = d[x], q = w[x];
            unsigned r = p >> 11, g = (p >> 5) & 63, bl = p & 31;
            unsigned a = r * 2 > g ? r * 2 : g; if (bl * 2 > a) a = bl * 2;
            unsigned br = (q >> 11) * k >> 5, bg = ((q >> 5) & 63) * k >> 5, bb = (q & 31) * k >> 5, na = 63 - a;
            br = br * na / 63 + r; bg = bg * na / 63 + g; bb = bb * na / 63 + bl;
            if (br > 31) br = 31; if (bg > 63) bg = 63; if (bb > 31) bb = 31;
            d[x] = (uint16_t)(br << 11 | bg << 5 | bb);
        }
    }
    st->home_hits++;
}

/* ---------------------------------------------------------------- capa superpuesta */
static void ov_restore(unsigned char *b)
{
    if (!st->ov_saved) return;
    uint32_t x = st->ov_ux, y = st->ov_uy, w = st->ov_uw, h = st->ov_uh;
    for (uint32_t r = 0; r < h; r++)
        memcpy(b + (y + r) * fline + x * 2, st->ov_under + r * w, w * 2);
    st->ov_saved = 0;
}

/* espera a que un cerrojo de secuencia del shell este par (sin escritura en curso), max ~3 ms */
static void wait_even(volatile uint32_t *seq)
{
    for (int i = 0; i < 300 && (*seq & 1); i++) usleep(10);
}

/* El panel (ST77903) NO tiene memoria: el DPU barre sin parar el bufer de la DDR a 44,3 Hz
 * (docs/research_60fps.md). FBIOPAN_DISPLAY solo cambia la direccion y duerme hasta el
 * fin del fotograma en curso. Para el bufer de otro (el que ya se ve), escribir "y luego
 * presentar" deja un corte donde estuviera el barrido. Por eso, cuando hay que escribir en
 * el bufer VISIBLE: primero frame_edge() (pan del mismo bufer = esperar al fin de fotograma)
 * y justo despues copiar de arriba abajo. La copia (300 KB, ~1 ms) va mas rapida que el
 * barrido (22,6 ms por pantalla) y empieza en el vblank, asi que siempre le va por delante.
 * Los bufers que el reproductor esta a punto de presentar (gancho de ioctl) no se ven aun:
 * ahi se sigue escribiendo antes de su pan. */
static void present_now(void) { r_ioctl(fb_fd, FBIOPAN_DISPLAY, &last_var); }
static void frame_edge(void) { present_now(); }

static void ov_apply(unsigned char *b)
{
    st->ov_saved = 0;
    if (!st->ov_on) return;
    wait_even(&st->ov_seq);
    uint32_t x = st->ov_x, y = st->ov_y, w = st->ov_w, h = st->ov_h;
    if (!w || !h || w * h > HM_OV_MAX || x + w > HM_W || y + h > HM_H) return;
    for (uint32_t r = 0; r < h; r++) {
        uint16_t *row = (uint16_t *)(b + (y + r) * fline) + x;
        const uint16_t *src = st->ov_rgb + r * w;
        const uint8_t *al = st->ov_a + r * w;
        memcpy(st->ov_under + r * w, row, w * 2);
        for (uint32_t i = 0; i < w; i++) {
            unsigned a = al[i];
            if (!a) continue;
            if (a >= 250) { row[i] = src[i]; continue; }
            unsigned d = row[i], s = src[i], na = 255 - a;
            unsigned rr = (((s >> 11) * a + (d >> 11) * na) / 255);
            unsigned gg = ((((s >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255);
            unsigned bb = (((s & 31) * a + (d & 31) * na) / 255);
            row[i] = (uint16_t)(rr << 11 | gg << 5 | bb);
        }
    }
    st->ov_ux = x; st->ov_uy = y; st->ov_uw = w; st->ov_uh = h;
    st->ov_saved = 1;
}

/* copia src -> d (bufer visible, tras frame_edge) y pone la capa en el orden del barrido:
 * filas hasta el final de la capa, la capa, el resto. Asi ninguna fila de la capa se barre
 * antes de estar puesta. */
static void copy_then_ov(unsigned char *d, const unsigned char *src)
{
    uint32_t y = st->ov_y, h = st->ov_h;
    st->ov_saved = 0;
    if (!st->ov_on || !h || y + h > HM_H) { memcpy(d, src, fsize); ov_apply(d); return; }
    size_t cut = (size_t)(y + h) * fline;
    memcpy(d, src, cut);
    ov_apply(d);
    memcpy(d + cut, src + cut, fsize - cut);
}

/* ---------------------------------------------------------------- ordenes */
static void pl_kick(void);     /* v6, al final del fichero */
static void st_kick(void);     /* v7 ajustes, al final del fichero */
static void *control(void *arg)
{
    (void)arg;
    for (;;) {
        int f = r_open(HM_FIFO, O_RDONLY);
        if (f < 0) { sleep(1); continue; }
        char c;
        while (read(f, &c, 1) == 1) {
            if (c == 'L') { pl_kick(); continue; }   /* v6: sin el mutex de la pantalla */
            if (c == 'S') { st_kick(); continue; }   /* v7: ajustes del reproductor, idem */
            pthread_mutex_lock(&mu);
            if (c == 'C') {
                if (!st->held && !st->capture && map_frames()) {
                    unsigned char *d = buf_at(last_var.yoffset);
                    memcpy(frames, d, fsize);           /* la imagen congelada, sin capa: */
                    if (st->ov_saved)                   /* se quita en la copia, no en pantalla */
                        for (uint32_t r = 0; r < st->ov_uh; r++)
                            memcpy(frames + (st->ov_uy + r) * fline + st->ov_ux * 2, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
                    memcpy(frames + fsize, frames, fsize);
                    st->cap_frames = 0;
                    st->capture = 1;
                }
            } else if (c == 'P') {
                if (st->capture && frames) {
                    frame_edge();                       /* primero el fin de fotograma, luego copiar */
                    wait_even(&st->fr0_seq);            /* casi siempre par: no retrasa la copia */
                    uint32_t y0 = st->dirty_y0, y1 = st->dirty_y1;
                    if (y1 > y0 && y1 <= HM_H) memcpy(buf_at(last_var.yoffset) + y0 * fline, frames + y0 * fline, (y1 - y0) * fline);
                    else memcpy(buf_at(last_var.yoffset), frames, fsize);
                }
                st->presents++;
            } else if (c == 'H') {
                st->capture = 0; st->held = 1;
            } else if (c == 'R') {
                int was_capture = st->capture;
                st->capture = 0;
                st->held = 0;
                if (st->waiting) pthread_cond_broadcast(&cv);
                else if (have_var && fbmem) {
                    unsigned char *d = buf_at(last_var.yoffset);
                    frame_edge();
                    if (was_capture && frames) copy_then_ov(d, frames + fsize);
                    else { st->ov_saved = 0; ov_apply(d); }
                }
                st->presents++;
            } else if (c == 'O') {
                if (!st->capture && !st->held && map_fb()) {
                    unsigned char *d = buf_at(last_var.yoffset);
                    frame_edge();            /* al empezar el vblank: quitar y poner seguidos */
                    ov_restore(d);           /* (decenas de us), nunca se barre a medias */
                    ov_apply(d);
                }
                st->presents++;
            }
            pthread_mutex_unlock(&mu);
        }
        close(f);
    }
    return 0;
}

/* ---------------------------------------------------------------- entrada */
static int find_real_touch(void)
{
    for (int i = 0; i < 16; i++) {
        char p[32], name[64] = "";
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = r_open(p, O_RDONLY | O_NONBLOCK);
        if (f < 0) continue;
        r_ioctl(f, EVIOCGNAME(sizeof name), name);
        close(f);
        if (!strcmp(name, "goodix-ts") || !strcmp(name, "ft6x06_ts")) { strcpy(real_ts, p); return 1; }
    }
    return 0;
}

static int create_uinput(void)
{
    int rf = r_open(real_ts, O_RDONLY | O_NONBLOCK);
    if (rf < 0) return 0;
    char name[64] = "";
    r_ioctl(rf, EVIOCGNAME(sizeof name), name);
    uin = r_open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uin < 0) { close(rf); return 0; }
    struct uinput_user_dev d; memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "%s", name);
    d.id.bustype = BUS_VIRTUAL; d.id.vendor = 0x4859; d.id.product = 0x4d44;
    r_ioctl(uin, UI_SET_EVBIT, EV_SYN); r_ioctl(uin, UI_SET_EVBIT, EV_KEY); r_ioctl(uin, UI_SET_EVBIT, EV_ABS);
    r_ioctl(uin, UI_SET_KEYBIT, BTN_TOUCH);
    int axes[] = { ABS_X, ABS_Y, ABS_MT_TOUCH_MAJOR, ABS_MT_WIDTH_MAJOR, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID };
    for (unsigned i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        struct input_absinfo ai;
        if (r_ioctl(rf, EVIOCGABS(axes[i]), &ai) == 0) {
            r_ioctl(uin, UI_SET_ABSBIT, axes[i]);
            d.absmin[axes[i]] = ai.minimum; d.absmax[axes[i]] = ai.maximum;
        }
    }
    close(rf);
    if (write(uin, &d, sizeof d) != sizeof d || r_ioctl(uin, UI_DEV_CREATE)) { close(uin); uin = -1; return 0; }
    char sys[64] = "";
    if (r_ioctl(uin, UI_GET_SYSNAME(sizeof sys), sys) < 0) { r_ioctl(uin, UI_DEV_DESTROY); close(uin); uin = -1; return 0; }
    char dir[96]; snprintf(dir, sizeof dir, "/sys/class/input/%s", sys);
    for (int tries = 0; tries < 200 && !virt_ts[0]; tries++) {
        DIR *dd = opendir(dir);
        if (dd) {
            struct dirent *e;
            while ((e = readdir(dd)))
                if (!strncmp(e->d_name, "event", 5)) snprintf(virt_ts, sizeof virt_ts, "/dev/input/%s", e->d_name);
            closedir(dd);
        }
        if (virt_ts[0]) {
            struct stat s;
            for (int k = 0; k < 200 && stat(virt_ts, &s); k++) usleep(5000);
            if (stat(virt_ts, &s)) virt_ts[0] = 0;
        } else usleep(5000);
    }
    if (!virt_ts[0]) { r_ioctl(uin, UI_DEV_DESTROY); close(uin); uin = -1; return 0; }
    return 1;
}

static void *input_thread(void *arg)
{
    (void)arg;
    int rf = r_open(real_ts, O_RDONLY);
    unlink(HM_SOCK);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, HM_SOCK);
    if (ls >= 0 && (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1))) { close(ls); ls = -1; }
    int cl = -1;
    struct input_event ev[64];
    for (;;) {
        struct pollfd p[3]; int n = 0;
        int irf = -1, ils = -1, icl = -1;
        if (rf >= 0) { p[n].fd = rf; p[n].events = POLLIN; irf = n++; }
        if (ls >= 0 && cl < 0) { p[n].fd = ls; p[n].events = POLLIN; ils = n++; }
        if (cl >= 0) { p[n].fd = cl; p[n].events = POLLIN; icl = n++; }
        if (poll(p, n, -1) < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        if (irf >= 0 && (p[irf].revents & POLLIN)) {
            ssize_t r = read(rf, ev, sizeof ev);
            if (r > 0 && cl < 0) write(uin, ev, r);         /* paso directo */
        }
        if (ils >= 0 && (p[ils].revents & POLLIN)) {
            cl = accept(ls, 0, 0);
            if (cl >= 0) {
                struct ucred cr; socklen_t cl_len = sizeof cr;
                st->shell_pid = getsockopt(cl, SOL_SOCKET, SO_PEERCRED, &cr, &cl_len) == 0 ? (uint32_t)cr.pid : 0;
                st->input_mode = 2;
            }
        }
        if (icl >= 0 && (p[icl].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(cl, ev, sizeof ev);
            if (r <= 0) {
                close(cl); cl = -1; st->input_mode = 1;
                /* el shell murio: soltar cualquier toque a medias y quitar su capa */
                struct input_event up[3]; memset(up, 0, sizeof up);
                up[0].type = EV_KEY; up[0].code = BTN_TOUCH; up[0].value = 0;
                up[1].type = EV_SYN; up[1].code = SYN_MT_REPORT;
                up[2].type = EV_SYN; up[2].code = SYN_REPORT;
                write(uin, up, sizeof up);
                pthread_mutex_lock(&mu);
                if (st->capture || st->held) {
                    st->capture = 0; st->held = 0;
                    if (st->waiting) pthread_cond_broadcast(&cv);
                    else if (fbmem && frames) { frame_edge(); memcpy(buf_at(last_var.yoffset), frames + fsize, fsize); st->ov_saved = 0; }
                }
                st->ov_on = 0;
                if (fbmem) { frame_edge(); ov_restore(buf_at(last_var.yoffset)); }
                pthread_mutex_unlock(&mu);
                continue;
            }
            r -= r % sizeof ev[0];
            if (r > 0) write(uin, ev, r);
        }
    }
    return 0;
}

/* si el shell se cuelga (sin morir) seguiria con la tactil capturada: matarlo */

/* Fin de la animacion de arranque, sin depender de la version del shell: cuando el shell da
 * senales de vida (shell_beat) o pasan BOOT_HOLD_S, se pide el final a hibyboot
 * (/tmp/hibyos_boot_stop), se espera su fundido (<= 1,5 s) y se suelta al reproductor. */
static void *boot_release(void *arg)
{
    (void)arg;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t beat0 = st->shell_beat;
    for (;;) {
        usleep(100000);
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
        if (!st->held) break;                                  /* ya lo solto el shell ('R') */
        if (st->shell_beat != beat0 || el > BOOT_HOLD_S - 2) {
            int f = r_open("/tmp/hibyos_boot_stop", O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f);
            for (int i = 0; i < 15 && !access("/tmp/hibyos_boot", F_OK); i++) usleep(100000);
            pthread_mutex_lock(&mu);
            st->held = 0;
            if (st->waiting) pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            break;
        }
    }
    return 0;
}

static void *watchdog(void *arg)
{
    (void)arg;
    uint32_t last = 0; int still = 0;
    for (;;) {
        sleep(1);
        if (st->input_mode != 2 || !st->shell_pid) { still = 0; continue; }
        uint32_t b = st->shell_beat;
        if (b != last) { last = b; still = 0; continue; }
        if (++still >= 4) {
            kill((pid_t)st->shell_pid, SIGKILL);   /* el cierre del socket hace el resto */
            still = 0;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- inicio */
__attribute__((constructor)) static void init(void)
{
    resolve();
    if (!r_open || !r_ioctl) return;
    char name[32] = {0};
    int f = r_open("/proc/self/comm", O_RDONLY);
    if (f >= 0) { read(f, name, sizeof name - 1); close(f); }
    if (strncmp(name, "hiby_player", 11)) return;
    int s = r_open(HM_STATE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s < 0) return;
    if (ftruncate(s, HM_STATE_SIZE)) { close(s); return; }
    void *m = mmap(0, HM_STATE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, s, 0);
    close(s);
    if (m == MAP_FAILED) return;
    st = m;
    memset((void *)st, 0, sizeof *st);
    /* animacion de arranque (hibyboot) en pantalla: el reproductor espera en su primer pan
     * hasta que el shell lo suelte ('R') o pasen BOOT_HOLD_S; su carga sigue por debajo */
    if (!access("/tmp/hibyos_boot", F_OK) && access("/tmp/hibyos_boot_stop", F_OK)) { st->held = 1; boot_hold = 1; }
    unlink(HM_FIFO);
    if (boot_hold) { pthread_t tb; if (!pthread_create(&tb, 0, boot_release, 0)) pthread_detach(tb); else st->held = 0; }
    if (mkfifo(HM_FIFO, 0600)) return;
    pthread_t t;
    if (pthread_create(&t, 0, control, 0)) return;
    pthread_detach(t);
    if (access("/usr/data/hibymod/no_input_bridge", F_OK) && find_real_touch() && create_uinput()) {
        if (pthread_create(&t, 0, input_thread, 0) == 0) {
            pthread_detach(t); st->input_mode = 1;
            if (pthread_create(&t, 0, watchdog, 0) == 0) pthread_detach(t);
        }
        else { r_ioctl(uin, UI_DEV_DESTROY); close(uin); uin = -1; virt_ts[0] = 0; }
    }
    st->np_magic = HM_NP_MAGIC;
    st->magic = HM_MAGIC;
    active = 1;
}

/* El reproductor recorre /dev/input y abriria tambien el tactil virtual (toques
 * duplicados): si lo abre por su nombre recibe un descriptor mudo. */
static int is_virt(const char *path) { return active && virt_ts[0] && path && !strcmp(path, virt_ts); }

static const char *redirect(const char *path)
{
    if (active && st && st->input_mode && path && real_ts[0] && !strcmp(path, real_ts)) return virt_ts;
    return path;
}

static void note_view(const char *path)
{
    if (!active || !path) return;
    size_t l = strlen(path);
    if (l > 5 && (!strcmp(path + l - 5, ".view") || !strcmp(path + l - 4, ".dlg"))) {
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        uint32_t s = st->vlog_seq;
        snprintf(st->vlog[s % HM_VLOG], sizeof st->vlog[0], "%s", b);
        __sync_synchronize();
        st->vlog_seq = s + 1;
    }
}

/* ---------------------------------------------------------------- lo que suena */
static pthread_mutex_t np_mu = PTHREAD_MUTEX_INITIALIZER;   /* solo escritores de np_open[] */
static volatile int pcm_fd = -1;
static __thread int in_alsa;              /* dentro de libasound: sus ioctl no se cuentan dos veces */

static uint32_t np_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000);
}

static int is_audio(const char *p)
{
    if (strncmp(p, "/data/mnt/", 10) && strncmp(p, "/usr/data/mnt/", 14) && strncmp(p, "/mnt/", 5)) return 0;
    const char *d = strrchr(p, '.');
    if (!d || strchr(d, '/') || strlen(d) > 6) return 0;
    static const char *ext[] = { ".flac", ".mp3", ".m4a", ".aac", ".alac", ".wav", ".aif", ".aiff", ".ape",
        ".wv", ".ogg", ".oga", ".opus", ".dsf", ".dff", ".wma", ".mp2", ".iso", ".mqa", ".tak", ".tta", 0 };
    for (int i = 0; ext[i]; i++) if (!strcasecmp(d, ext[i])) return 1;
    return 0;
}

static void np_begin(void) { pthread_mutex_lock(&np_mu); st->np_seq++; __sync_synchronize(); }
static void np_end(void) { __sync_synchronize(); st->np_seq++; pthread_mutex_unlock(&np_mu); }

static void note_audio_open(const char *path, int fd)
{
    np_begin();
    uint32_t now = np_now();
    /* un fd reutilizado sin que viesemos el cierre (fclose cierra por dentro de libc) */
    for (int i = 0; i < HM_NP_SLOTS; i++)
        if (st->np_open[i].gen && st->np_open[i].fd == fd) { st->np_open[i].fd = -1; st->np_open[i].close_ms = now; }
    /* ranura: una vacia; si no, la cerrada mas antigua; si no, la mas antigua */
    int k = -1;
    for (int i = 0; i < HM_NP_SLOTS && k < 0; i++) if (!st->np_open[i].gen) k = i;
    if (k < 0)
        for (int i = 0; i < HM_NP_SLOTS; i++)
            if (st->np_open[i].fd < 0 && (k < 0 || st->np_open[i].gen < st->np_open[k].gen)) k = i;
    if (k < 0) { k = 0; for (int m = 1; m < HM_NP_SLOTS; m++) if (st->np_open[m].gen < st->np_open[k].gen) k = m; }
    struct hm_np_open *e = &st->np_open[k];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->fd = fd; e->tid = (uint32_t)syscall(SYS_gettid);
    e->open_ms = now; e->close_ms = 0; e->frames_at_open = st->pcm_frames;
    e->nseeks = 0; e->seek_ms = 0; e->seek_frames = 0; e->seek_lo = 0; e->seek_hi = 0;
    e->gen = ++st->np_gen;
    np_end();
}

static int np_slot(int fd)
{
    for (int i = 0; i < HM_NP_SLOTS; i++) if (st->np_open[i].gen && st->np_open[i].fd == fd) return i;
    return -1;
}

static void note_close(int fd)
{
    if (fd == pcm_fd) { pcm_fd = -1; st->pcm_open = 0; }
    if (np_slot(fd) < 0) return;           /* lo normal: sin cerrojo */
    np_begin();
    int i = np_slot(fd);
    if (i >= 0) { st->np_open[i].fd = -1; st->np_open[i].close_ms = np_now(); }
    np_end();
}

static void note_seek(int fd, long long off)
{
    if (np_slot(fd) < 0) return;
    np_begin();
    int i = np_slot(fd);
    if (i >= 0) {
        struct hm_np_open *e = &st->np_open[i];
        e->seek_lo = (uint32_t)off; e->seek_hi = (uint32_t)((unsigned long long)off >> 32);
        e->seek_ms = np_now(); e->seek_frames = st->pcm_frames; e->nseeks++;
    }
    np_end();
}

/* Proteccion del audio: el hilo que escribe al PCM (en este firmware "HiBy_Main_Thread",
 * ioctl WRITEI de tinyalsa; con BT/USB, snd_pcm_writei) pasa a SCHED_FIFO 10 la primera vez
 * que escribe. Casi siempre esta dormido esperando al DMA, asi que no le quita CPU a la UI;
 * solo gana la latencia de despertar. El kernel limita el tiempo real al 95 %
 * (sched_rt_runtime_us = 950000), asi que un fallo suyo no congela el sistema.
 * Se desactiva con /usr/data/hibymod/no_rt. El decodificador ("lg_music_player") no se toca. */
#define RT_PRIO 10
static __thread int rt_done;
static void rt_boost(void)
{
    rt_done = 1;
    if (!access("/usr/data/hibymod/no_rt", F_OK)) return;
    struct sched_param sp = { .sched_priority = RT_PRIO };
    int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    st->rt_err = (uint32_t)e; st->rt_count++;
    if (!e) { st->rt_tid = tid; st->rt_prio = RT_PRIO; }
    int f = r_open("/usr/data/hibymod/disp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f >= 0) {
        char l[96], c[20] = "";
        int k = r_open("/proc/thread-self/comm", O_RDONLY);
        if (k >= 0) { ssize_t n = read(k, c, sizeof c - 1); if (n > 0) c[n - 1] = 0; r_close(k); }
        int n = snprintf(l, sizeof l, "rt: hilo %u (%s) SCHED_FIFO %d: %s\n", tid, c, RT_PRIO, e ? strerror(e) : "ok");
        if (write(f, l, n) < 0) {}
        r_close(f);
    }
}

static void pcm_wrote(long n)
{
    if (!rt_done) rt_boost();
    if (n <= 0) return;
    st->pcm_frames += (uint32_t)n;
    st->pcm_last_ms = np_now();
    st->pcm_paused = 0;
}

/* ioctl de tinyalsa sobre el PCM (despues de la llamada real) */
static void pcm_ioctl(unsigned long req, void *arg, int r)
{
    if (r < 0 || in_alsa) return;
    if (req == SNDRV_PCM_IOCTL_WRITEI_FRAMES && arg) pcm_wrote(((struct snd_xferi *)arg)->result);
    else if (req == SNDRV_PCM_IOCTL_WRITEN_FRAMES && arg) pcm_wrote(((struct snd_xfern *)arg)->result);
    else if (req == SNDRV_PCM_IOCTL_HW_PARAMS && arg) {
        struct snd_pcm_hw_params *p = arg;
        st->pcm_rate = p->intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
        st->pcm_bits = p->intervals[SNDRV_PCM_HW_PARAM_SAMPLE_BITS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
        st->pcm_channels = p->intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    }
    else if (req == SNDRV_PCM_IOCTL_DROP) { st->pcm_drops++; st->pcm_drop_ms = np_now(); }
    else if (req == SNDRV_PCM_IOCTL_PREPARE) st->pcm_prepares++;
    else if (req == SNDRV_PCM_IOCTL_PAUSE) st->pcm_paused = (uintptr_t)arg ? 1 : 0;
}

static int track(const char *path, int fd)
{
    if (active && fd >= 0 && path && !strcmp(path, "/dev/fb0")) fb_fd = fd;
    if (fd >= 0) note_view(path);
    if (active && fd >= 0 && path) {
        if (!strncmp(path, "/dev/snd/pcmC", 13) && path[strlen(path) - 1] == 'p') { pcm_fd = fd; st->pcm_open = 1; }
        else if (is_audio(path)) note_audio_open(path, fd);
    }
    return fd;
}

int open(const char *path, int flags, ...)
{
    resolve();
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, int); va_end(a); }
    if (is_virt(path)) return eventfd(0, (flags & O_NONBLOCK) ? EFD_NONBLOCK : 0);
    return track(path, r_open(redirect(path), flags, m));
}

int open64(const char *path, int flags, ...)
{
    resolve();
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, int); va_end(a); }
    if (is_virt(path)) return eventfd(0, (flags & O_NONBLOCK) ? EFD_NONBLOCK : 0);
    return track(path, r_open64(redirect(path), flags, m));
}

/* openat con ruta relativa a un directorio (recorrido de /dev/input): ruta completa */
static const char *full_at(int dfd, const char *path, char *buf, size_t n)
{
    if (!active || !path || path[0] == '/' || dfd == AT_FDCWD) return path;
    char l[32]; snprintf(l, sizeof l, "/proc/self/fd/%d", dfd);
    ssize_t k = readlink(l, buf, n - 1);
    if (k <= 0 || (size_t)k + strlen(path) + 2 > n) return path;
    buf[k] = 0;
    if (strcmp(buf, "/dev/input")) return path;
    strcat(buf, "/"); strcat(buf, path);
    return buf;
}

int openat64(int dfd, const char *path, int flags, ...)
{
    resolve();
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, int); va_end(a); }
    char b[96]; const char *full = full_at(dfd, path, b, sizeof b);
    if (is_virt(full)) return eventfd(0, (flags & O_NONBLOCK) ? EFD_NONBLOCK : 0);
    const char *r = redirect(full);
    return track(full, r_openat64(dfd, r == full ? path : r, flags, m));
}

int openat(int dfd, const char *path, int flags, ...)
{
    resolve();
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, int); va_end(a); }
    char b[96]; const char *full = full_at(dfd, path, b, sizeof b);
    if (is_virt(full)) return eventfd(0, (flags & O_NONBLOCK) ? EFD_NONBLOCK : 0);
    const char *r = redirect(full);
    return track(full, r_openat(dfd, r == full ? path : r, flags, m));
}

FILE *fopen(const char *path, const char *mode)
{
    resolve();
    FILE *f = r_fopen(path, mode);
    if (f) { note_view(path); if (active && path && is_audio(path)) note_audio_open(path, fileno(f)); }
    return f;
}

int fclose(FILE *f)
{
    static int (*r)(FILE *);
    if (!r) r = dlsym(RTLD_NEXT, "fclose");
    if (active && f && st) { int fd = fileno(f); if (fd >= 0 && np_slot(fd) >= 0) note_close(fd); }
    return r(f);
}

FILE *fopen64(const char *path, const char *mode)
{
    resolve();
    FILE *f = r_fopen64(path, mode);
    if (f) { note_view(path); if (active && path && is_audio(path)) note_audio_open(path, fileno(f)); }
    return f;
}

int ioctl(int fd, unsigned long req, ...)
{
    resolve();
    va_list a; va_start(a, req); void *arg = va_arg(a, void *); va_end(a);
    if (active && fd == fb_fd && req == FBIOPAN_DISPLAY && arg) {
        pthread_mutex_lock(&mu);
        last_var = *(struct fb_var_screeninfo *)arg; have_var = 1;
        st->yoffset = last_var.yoffset;
        if (st->held) {
            struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += boot_hold ? BOOT_HOLD_S : HOLD_MAX_S; boot_hold = 0;
            st->waiting = 1;
            while (st->held)
                if (pthread_cond_timedwait(&cv, &mu, &until) == ETIMEDOUT) { st->held = 0; break; }
            st->waiting = 0;
            last_var = *(struct fb_var_screeninfo *)arg;
            st->yoffset = last_var.yoffset;
        }
        if (st->capture && frames) {
            unsigned char *b = buf_at(last_var.yoffset);
            memcpy(frames + fsize, b, fsize);   /* fotograma del reproductor -> frames[1] */
            wait_even(&st->fr0_seq);
            memcpy(b, frames, fsize);           /* el panel sigue con la imagen del shell */
            st->cap_frames++;
        } else {
            if (st->home_sub_on && map_fb()) home_substitute(buf_at(last_var.yoffset));
            if (st->ov_on && map_fb()) ov_apply(buf_at(last_var.yoffset));
            else st->ov_saved = 0;
        }
        st->frames++;
        int r = r_ioctl(fd, req, arg);
        pthread_mutex_unlock(&mu);
        return r;
    }
    if (active && fd >= 0 && fd == pcm_fd) {
        int r = r_ioctl(fd, req, arg);
        pcm_ioctl(req, arg, r);
        return r;
    }
    return r_ioctl(fd, req, arg);
}

int close(int fd)
{
    resolve();
    if (active && fd >= 0) note_close(fd);
    return r_close(fd);
}

off_t lseek(int fd, off_t off, int wh)
{
    resolve();
    off_t r = r_lseek(fd, off, wh);
    if (active && r >= 0) note_seek(fd, (long long)r);
    return r;
}

off64_t lseek64(int fd, off64_t off, int wh)
{
    resolve();
    off64_t r = r_lseek64(fd, off, wh);
    if (active && r >= 0) note_seek(fd, (long long)r);
    return r;
}

/* libasound (Bluetooth por bluealsa, DAC USB). El reproductor la enlaza, asi que estas
 * definiciones tapan las suyas; se llama a la real con dlsym(RTLD_NEXT). */
long snd_pcm_writei(void *pcm, const void *buf, unsigned long n)
{
    static long (*r)(void *, const void *, unsigned long);
    if (!r) r = dlsym(RTLD_NEXT, "snd_pcm_writei");
    if (!r) return -38;
    int sv = in_alsa; in_alsa = 1;
    long k = r(pcm, buf, n);
    in_alsa = sv;
    if (active && !sv) pcm_wrote(k);
    return k;
}

int snd_pcm_hw_params(void *pcm, void *params)
{
    static int (*r)(void *, void *);
    static int (*get_rate)(const void *, unsigned *, int *);
    static int (*get_ch)(const void *, unsigned *);
    if (!r) {
        get_rate = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
        get_ch = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
        r = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
    }
    if (!r) return -38;
    int sv = in_alsa; in_alsa = 1;
    int k = r(pcm, params);
    in_alsa = sv;
    if (active && !sv && k >= 0) {
        unsigned v = 0; int d = 0;
        if (get_rate && get_rate(params, &v, &d) >= 0) st->pcm_rate = v;
        if (get_ch && get_ch(params, &v) >= 0) st->pcm_channels = v;
        st->pcm_bits = 0;
        st->pcm_open = 1;
    }
    return k;
}

int snd_pcm_drop(void *pcm)
{
    static int (*r)(void *);
    if (!r) r = dlsym(RTLD_NEXT, "snd_pcm_drop");
    if (!r) return -38;
    int sv = in_alsa; in_alsa = 1;
    int k = r(pcm);
    in_alsa = sv;
    if (active && !sv) { st->pcm_drops++; st->pcm_drop_ms = np_now(); }
    return k;
}

/* ---------------------------------------------------------------- v6: reproducir una lista
 * Orden 'L' por la FIFO (ver docs/research_play_list.md). INERTE si nadie la manda.
 * El shell deja la peticion en PL_REQ (texto UTF-8) y manda 'L'; un hilo propio (nunca el de
 * control, que sirve las animaciones) hace lo mismo que la interfaz de fabrica al tocar una
 * cancion de una lista (manejador 0x4b4420) y que el servidor HiBy Link (playUuid, 0x4577a0),
 * que tambien lo llama desde su propio hilo:
 *     set_param(0x17,0,0); set_param(0x15, L"ruta de lista"); set_param(8, n);
 *     get_param(0xb,&cur); set_param(0x10,cur); play_index(i, -1, 0x10002)
 * La "ruta de lista" es la clave de MANAGER_TABLE; su contenido sale de list_tb_<id>. Con
 * "f=" el hilo crea esa tabla (list_tb_7701/7702, ruta L"hibymod\q1"/"q2", alternando para
 * no tocar la que suena) usando la conexion SQLite del propio reproductor y su cerrojo.
 * Todas las direcciones se comprueban (firma de 16 bytes y punteros del contexto) antes de
 * llamar a nada: con otro firmware la orden responde rc=-ENOEXEC y no hace nada.
 * Peticion:  HMPL1 / id=N / index=I / [title=..] [mode=0..3] [pos=MS] [unscanned=1] [check=1] [drop=1]
 *            [list=RUTA_DE_LISTA]  o  f=RUTA (una por linea, en orden; opcional "cue|ruta")
 * Respuesta: PL_RES  "id=N rc=R rows=M index=I slot=S ms=T msg" */
#define PL_REQ "/tmp/hibymod_playreq"
#define PL_RES "/tmp/hibymod_playres"
#define PL_MAX_ENTRIES 5000
#define PL_TB0 7701                       /* list_tb_7701 / list_tb_7702 */

typedef uint16_t hw16;                    /* las cadenas anchas del reproductor son UTF-16LE */
/* hiby_player d1dea413a91ca3c6759c9029744bc52c (EXEC no PIE, base 0x400000) */
#define HP_P            0x969ac8u         /* estado del reproductor; +0x48/+0x4c cerrojo */
#define HP_DBCTX        0x991364u         /* puntero al contexto de la base: +0 sqlite3*, +0x564/+0x568 cerrojo */
static const struct { uint32_t addr, w[4]; } pl_sig[] = {
    { 0x4366a0, { 0x27bdffd0, 0xafb0001c, 0x3c100097, 0x26109ac8 } },   /* 0 set_param(id, ptr, val) */
    { 0x4368e0, { 0x27bdf540, 0xafb10aac, 0x3c110097, 0x26319ac8 } },   /* 1 get_param(id, out, out2) */
    { 0x43e8a0, { 0x27bdf348, 0xafb20cb0, 0xafb10cac, 0xafb00ca8 } },   /* 2 play_index(i, pos, flags) */
    { 0x42baa0, { 0x27bdfdd0, 0xafb10224, 0x3c110097, 0xafb20228 } },   /* 3 setting_set(id, val) */
    { 0x70eee0, { 0x3c020099, 0x8c421364, 0x2405ffff, 0x0811ed18 } },   /* 4 db_lock() */
    { 0x70ef00, { 0x3c020099, 0x8c421364, 0x0811ed20, 0x8c44055c } },   /* 5 db_unlock() */
    { 0x62e740, { 0x27bdffd0, 0x8fa20040, 0xafa70014, 0xafa00010 } },   /* 6 sqlite3_prepare_v2 */
    { 0x618480, { 0x27bdfec0, 0xafbf013c, 0xafbe0138, 0xafb70134 } },   /* 7 sqlite3_step */
    { 0x5fbae0, { 0x10800028, 0x00000000, 0x27bdffd0, 0xafbf002c } },   /* 8 sqlite3_finalize */
    { 0x5db5a0, { 0x3c080083, 0x81083a94, 0x27bdffd8, 0x24030002 } },   /* 9 sqlite3_bind_text16 */
    { 0x5db700, { 0x08176d98, 0x00063fc3, 0x00000000, 0x00000000 } },   /* 10 sqlite3_bind_int */
    { 0x5c9840, { 0x27bdffd8, 0xafbf0024, 0xafb00020, 0x0c16fb68 } },   /* 11 sqlite3_column_int */
    { 0x42a500, { 0x3c020097, 0x8c429590, 0x27bdffe0, 0x1040013b } },   /* 12 setting_get(id) */
};
typedef int (*pl_setp_t)(int, const void *, int);
typedef int (*pl_getp_t)(int, void *, void *);
typedef int (*pl_play_t)(int, int, int);
typedef int (*pl_sset_t)(int, int);
typedef intptr_t (*pl_sget_t)(int);
typedef void (*pl_void_t)(void);
typedef int (*sq_prep_t)(void *, const char *, int, void **, const char **);
typedef int (*sq_stmt_t)(void *);
typedef int (*sq_btxt_t)(void *, int, const void *, int, void *);
typedef int (*sq_bint_t)(void *, int, int);
typedef int (*sq_cint_t)(void *, int);
#define PLF(i, t) ((t)(uintptr_t)pl_sig[i].addr)

static volatile int pl_busy;

/* [a, a+n) dentro de una proyeccion legible (y ejecutable si x) de este proceso: con otro
 * binario, leer una direccion fija sin comprobarlo podria tumbar el reproductor */
static int pl_mapped(uint32_t a, uint32_t n, int x)
{
    FILE *f = r_fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char l[256]; int ok = 0;
    while (!ok && fgets(l, sizeof l, f)) {
        unsigned long lo, hi; char pr[8];
        if (sscanf(l, "%lx-%lx %7s", &lo, &hi, pr) == 3 && a >= lo && a + n <= hi && pr[0] == 'r' && (!x || pr[2] == 'x')) ok = 1;
    }
    fclose(f);
    return ok;
}

static int pl_check(void)
{
    for (size_t i = 0; i < sizeof pl_sig / sizeof pl_sig[0]; i++)
        if (!pl_mapped(pl_sig[i].addr, 16, 1) || memcmp((const void *)(uintptr_t)pl_sig[i].addr, pl_sig[i].w, 16)) return -ENOEXEC;
    if (!pl_mapped(HP_P, 0x100, 0) || !pl_mapped(HP_DBCTX, 4, 0)) return -ENOEXEC;
    const uint32_t *p = (const uint32_t *)(uintptr_t)HP_P;
    if (!p[0x48 / 4] || !p[0x4c / 4]) return -EAGAIN;              /* reproductor sin iniciar */
    const uint32_t *ctx = *(uint32_t *const *)(uintptr_t)HP_DBCTX;
    if (!ctx || !pl_mapped((uint32_t)(uintptr_t)ctx, 0x570, 0) || !ctx[0] || ctx[0x564 / 4] != 0x70eee0 || ctx[0x568 / 4] != 0x70ef00) return -EAGAIN;
    return 0;
}

/* UTF-8 -> UTF-16 (con sustitutos), siempre terminada; devuelve unidades sin el 0 */
static int pl_w16(const char *s, hw16 *o, int max)
{
    int n = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n < max - 2) {
        uint32_t c = *u++;
        if (c >= 0xf0 && (u[0] & 0xc0) == 0x80 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
            c = (c & 7) << 18 | (u[0] & 63) << 12 | (u[1] & 63) << 6 | (u[2] & 63); u += 3;
        } else if (c >= 0xe0 && (u[0] & 0xc0) == 0x80 && (u[1] & 0xc0) == 0x80) {
            c = (c & 15) << 12 | (u[0] & 63) << 6 | (u[1] & 63); u += 2;
        } else if (c >= 0xc0 && (u[0] & 0xc0) == 0x80) {
            c = (c & 31) << 6 | (u[0] & 63); u += 1;
        } else if (c >= 0x80) c = 0xfffd;
        if (c >= 0x10000) { c -= 0x10000; o[n++] = (hw16)(0xd800 | c >> 10); o[n++] = (hw16)(0xdc00 | (c & 1023)); }
        else o[n++] = (hw16)c;
    }
    o[n] = 0;
    return n;
}

/* ruta Linux -> ruta del reproductor ("a:\Artista\Album\x.flac"); las que ya son "x:\" pasan */
static void pl_hpath(const char *in, char *out, size_t n)
{
    static const struct { const char *pre, *drv; } m[] = {
        { "/usr/data/mnt/sd_0/", "a:\\" }, { "/data/mnt/sd_0/", "a:\\" }, { "/mnt/sd_0/", "a:\\" },
        { "/usr/data/mnt/sd_1/", "b:\\" }, { "/data/mnt/sd_1/", "b:\\" }, { "/mnt/sd_1/", "b:\\" },  /* b: = sd_1 [I] */
    };
    const char *rest = in, *drv = "";
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strncmp(in, m[i].pre, strlen(m[i].pre))) { drv = m[i].drv; rest = in + strlen(m[i].pre); break; }
    size_t k = (size_t)snprintf(out, n, "%s", drv);
    for (; *rest && k + 1 < n; rest++) out[k++] = *rest == '/' ? '\\' : *rest;
    out[k < n ? k : n - 1] = 0;
}

struct pl_db { void *db; const char *err; int rc; };

/* prepara, enlaza y ejecuta hasta DONE; si one != NULL devuelve la primera columna entera */
static int pl_sql(struct pl_db *d, const char *sql, const hw16 *t1, int n1, const hw16 *t2, int n2, int i3, int *one)
{
    if (d->rc) return d->rc;
    void *s = 0;
    int r = PLF(6, sq_prep_t)(d->db, sql, -1, &s, 0);
    if (r || !s) { d->rc = r ? r : -1; d->err = sql; return d->rc; }
    /* los textos llevan el 0 final dentro, como los guarda el reproductor (bind de wcslen+1) */
    if (t1) PLF(9, sq_btxt_t)(s, 1, t1, (n1 + 1) * 2, (void *)-1);   /* SQLITE_TRANSIENT */
    if (t2) PLF(9, sq_btxt_t)(s, 2, t2, (n2 + 1) * 2, (void *)-1);
    if (i3 != INT32_MIN) PLF(10, sq_bint_t)(s, 3, i3);
    for (;;) {
        r = PLF(7, sq_stmt_t)(s);
        if (r == 100) { if (one) { *one = PLF(11, sq_cint_t)(s, 0); one = 0; } continue; }   /* SQLITE_ROW */
        break;
    }
    PLF(8, sq_stmt_t)(s);
    if (r != 101) { d->rc = r; d->err = sql; }                    /* SQLITE_DONE */
    return d->rc;
}

#define PL_COLS "id,path,name,album,artist,genre,year,dis_id,ck_id,has_child_file,begin_time,end_time,cue_id,character," \
                "size,sample_rate,bit_rate,bit,channel,format,quality,album_pic_path,lrc_path,track_gain,track_peak,ctime,mtime"
#define PL_CREATE "(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE," \
    "genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT," \
    "character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE," \
    "album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL,ctime INT,mtime INT," \
    "PRIMARY KEY(id, path, cue_id))"      /* = "CREATE TABLE IF NOT EXISTS %s%d(...)" del binario */

struct pl_ent { char *path; int cue; };

/* crea list_tb_<tb> con las entradas (en orden) y su fila de MANAGER_TABLE; devuelve filas o <0 */
static int pl_build(int tb, const hw16 *lpath, int lpn, struct pl_ent *e, int n, int unscanned,
                    int want, int *idx, const char **err)
{
    struct pl_db d = { *(void **)*(uint32_t *const *)(uintptr_t)HP_DBCTX, 0, 0 };
    static hw16 w[1024];
    char q[1400];
    int rows = 0, found = 0;
    PLF(4, pl_void_t)();                                          /* cerrojo de la base del reproductor */
    pl_sql(&d, "BEGIN", 0, 0, 0, 0, INT32_MIN, 0);
    if (d.rc) { *err = "BEGIN (transaccion abierta?)"; PLF(5, pl_void_t)(); return -EBUSY; }
    pl_sql(&d, "CREATE TEMP TABLE IF NOT EXISTS hm_req(pos INTEGER PRIMARY KEY, path TEXT COLLATE NOCASE, cue INT)", 0, 0, 0, 0, INT32_MIN, 0);
    pl_sql(&d, "DELETE FROM hm_req", 0, 0, 0, 0, INT32_MIN, 0);
    for (int i = 0; i < n && !d.rc; i++) {
        char hp[1024]; pl_hpath(e[i].path, hp, sizeof hp);
        int k = pl_w16(hp, w, 1024);
        snprintf(q, sizeof q, "INSERT INTO hm_req(pos, path, cue) VALUES (%d, ?1, %d)", i, e[i].cue);
        pl_sql(&d, q, w, k, 0, 0, INT32_MIN, 0);
    }
    snprintf(q, sizeof q, "DROP TABLE IF EXISTS list_tb_%d", tb);           pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, 0);
    snprintf(q, sizeof q, "CREATE TABLE IF NOT EXISTS list_tb_%d" PL_CREATE, tb); pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, 0);
    /* una fila por cancion de la biblioteca, en el orden pedido (rowid = posicion en la lista).
     * Sin unscanned, lo que no este en MEDIA_TABLE se descarta; con unscanned=1 entra con lo minimo. */
    snprintf(q, sizeof q,
        "INSERT OR IGNORE INTO list_tb_%d(" PL_COLS ") SELECT "
        "COALESCE(m.id,-1-r.pos),r.path,COALESCE(m.name,r.path),COALESCE(m.album,''),COALESCE(m.artist,''),COALESCE(m.genre,''),"
        "COALESCE(m.year,0),COALESCE(m.dis_id,0),COALESCE(m.ck_id,0),COALESCE(m.has_child_file,0),COALESCE(m.begin_time,0),"
        "COALESCE(m.end_time,0),COALESCE(m.cue_id,-1),COALESCE(m.character,''),COALESCE(m.size,0),COALESCE(m.sample_rate,0),"
        "COALESCE(m.bit_rate,0),COALESCE(m.bit,0),COALESCE(m.channel,0),COALESCE(m.format,0),COALESCE(m.quality,''),"
        "COALESCE(m.album_pic_path,''),COALESCE(m.lrc_path,''),COALESCE(m.track_gain,0),COALESCE(m.track_peak,0),"
        "COALESCE(m.ctime,0),COALESCE(m.mtime,0) "
        "FROM hm_req r LEFT JOIN MEDIA_TABLE m ON m.path = r.path AND (r.cue < 0 OR m.cue_id = r.cue) "
        "WHERE m.path IS NOT NULL OR %d ORDER BY r.pos, m.cue_id", tb, unscanned ? 1 : 0);
    pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, 0);
    snprintf(q, sizeof q, "SELECT count(*) FROM list_tb_%d", tb);   pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, &rows);
    /* indice real de la entrada pedida (pudo haber descartes o duplicados) */
    *idx = 0;
    if (want >= 0 && want < n && !d.rc) {
        char hp[1024]; pl_hpath(e[want].path, hp, sizeof hp);
        int k = pl_w16(hp, w, 1024);
        snprintf(q, sizeof q, "SELECT count(*) FROM list_tb_%d WHERE path = ?1", tb);   pl_sql(&d, q, w, k, 0, 0, INT32_MIN, &found);
        snprintf(q, sizeof q, "SELECT count(*) FROM list_tb_%d WHERE rowid < (SELECT min(rowid) FROM list_tb_%d WHERE path = ?1)", tb, tb);
        pl_sql(&d, q, w, k, 0, 0, INT32_MIN, idx);
        if (!found) *idx = 0;
    }
    pl_sql(&d, "DELETE FROM MANAGER_TABLE WHERE id = ?3 OR path = ?1", lpath, lpn, 0, 0, tb, 0);
    snprintf(q, sizeof q, "INSERT INTO MANAGER_TABLE(id, path, count, music_count, dir_count, m3u_count) VALUES (?3, ?1, %d, %d, 0, 0)", rows, rows);
    pl_sql(&d, q, lpath, lpn, 0, 0, tb, 0);
    if (!d.rc && rows <= 0) { d.rc = -1; d.err = "lista vacia (ninguna ruta esta en MEDIA_TABLE)"; }
    if (d.rc) {
        int keep = d.rc; const char *ke = d.err;
        d.rc = 0; pl_sql(&d, "ROLLBACK", 0, 0, 0, 0, INT32_MIN, 0);
        PLF(5, pl_void_t)();
        *err = ke; return keep > 0 ? -1000 - keep : -EIO;
    }
    pl_sql(&d, "COMMIT", 0, 0, 0, 0, INT32_MIN, 0);
    PLF(5, pl_void_t)();
    if (d.rc) { *err = "COMMIT"; return -1000 - d.rc; }
    return rows;
}

static void pl_result(int id, int rc, int rows, int idx, int slot, uint32_t ms, const char *msg)
{
    char l[400];
    int n = snprintf(l, sizeof l, "id=%d rc=%d rows=%d index=%d slot=%d ms=%u %s\n", id, rc, rows, idx, slot, ms, msg ? msg : "");
    int f = r_open(PL_RES ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { if (write(f, l, n) < 0) {} r_close(f); rename(PL_RES ".tmp", PL_RES); }
    f = r_open("/usr/data/hibymod/disp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f >= 0) { if (write(f, "pl: ", 4) < 0 || write(f, l, n) < 0) {} r_close(f); }
}

static void *pl_thread(void *arg)
{
    (void)arg;
    uint32_t t0 = np_now();
    int id = -1, idx = 0, mode = -1, pos = -1, unscanned = 0, rows = 0, slot = 0, rc = 0, n = 0, check = 0, drop = 0;
    static char cmsg[160];
    char title[64] = "", list[1024] = "";
    const char *msg = "";
    char *buf = 0;
    struct pl_ent *e = 0;
    static hw16 lw[1024];
    if ((rc = pl_check())) { msg = rc == -ENOEXEC ? "firmware distinto: no se llama a nada" : "reproductor sin iniciar"; goto out; }
    int f = r_open(PL_REQ, O_RDONLY);
    if (f < 0) { rc = -ENOENT; msg = PL_REQ; goto out; }
    struct stat sb;
    if (fstat(f, &sb) || sb.st_size <= 0 || sb.st_size > (1 << 20) || !(buf = malloc(sb.st_size + 1))) { r_close(f); rc = -EINVAL; msg = "peticion vacia o enorme"; goto out; }
    ssize_t got = read(f, buf, sb.st_size); r_close(f);
    if (got != sb.st_size) { rc = -EIO; msg = "lectura"; goto out; }
    buf[got] = 0;
    if (strncmp(buf, "HMPL1\n", 6)) { rc = -EINVAL; msg = "falta HMPL1"; goto out; }
    if (!(e = calloc(PL_MAX_ENTRIES, sizeof *e))) { rc = -ENOMEM; goto out; }
    for (char *l = buf + 6, *nx; l && *l; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        size_t L = strlen(l); if (L && l[L - 1] == '\r') l[--L] = 0;
        if (!strncmp(l, "id=", 3)) id = atoi(l + 3);
        else if (!strncmp(l, "index=", 6)) idx = atoi(l + 6);
        else if (!strncmp(l, "mode=", 5)) mode = atoi(l + 5);
        else if (!strncmp(l, "pos=", 4)) pos = atoi(l + 4);
        else if (!strncmp(l, "unscanned=", 10)) unscanned = atoi(l + 10);
        else if (!strncmp(l, "title=", 6)) snprintf(title, sizeof title, "%s", l + 6);
        else if (!strncmp(l, "list=", 5)) snprintf(list, sizeof list, "%s", l + 5);
        else if (!strcmp(l, "check=1")) check = 1;
        else if (!strcmp(l, "drop=1")) drop = 1;
        else if (!strncmp(l, "f=", 2) && n < PL_MAX_ENTRIES) {
            char *p = l + 2, *bar = strchr(p, '|');
            e[n].cue = -1;
            if (bar && bar - p < 6 && (bar == p || (p[0] >= '0' && p[0] <= '9') || p[0] == '-')) { e[n].cue = atoi(p); p = bar + 1; }
            e[n++].path = p;
        }
    }
    if (check) {                          /* solo comprobar: firmas ok + estado actual, sin tocar nada */
        static hw16 cur[0x104]; char cu[0x104];
        memset(cur, 0, sizeof cur);
        PLF(1, pl_getp_t)(0x15, cur, 0);
        int k = 0; for (; k < 0x103 && cur[k]; k++) cu[k] = cur[k] < 0x80 ? (char)cur[k] : '?';
        cu[k] = 0;
        int ci = -1, cnt = -1; PLF(1, pl_getp_t)(0x0b, &ci, 0); PLF(1, pl_getp_t)(0x08, &cnt, 0);
        snprintf(cmsg, sizeof cmsg, "firmas ok; modo=%d lista='%s' indice=%d total=%d", (int)PLF(12, pl_sget_t)(0x1a), cu, ci, cnt);
        idx = ci; rows = cnt; msg = cmsg; goto out;
    }
    if (drop) {                           /* quitar list_tb_7701/7702 y sus filas (no la que suena) */
        static hw16 cur[0x104];
        memset(cur, 0, sizeof cur);
        PLF(1, pl_getp_t)(0x15, cur, 0);
        if (cur[0] == 'h' && cur[1] == 'i' && cur[2] == 'b' && cur[3] == 'y' && cur[7] == '\\') { rc = -EBUSY; msg = "suena una lista de hibymod: cambia de lista antes"; goto out; }
        struct pl_db d = { *(void **)*(uint32_t *const *)(uintptr_t)HP_DBCTX, 0, 0 };
        PLF(4, pl_void_t)();
        pl_sql(&d, "DROP TABLE IF EXISTS list_tb_7701", 0, 0, 0, 0, INT32_MIN, 0);
        pl_sql(&d, "DROP TABLE IF EXISTS list_tb_7702", 0, 0, 0, 0, INT32_MIN, 0);
        pl_sql(&d, "DELETE FROM MANAGER_TABLE WHERE id IN (7701, 7702)", 0, 0, 0, 0, INT32_MIN, 0);
        PLF(5, pl_void_t)();
        rc = d.rc ? -1000 - d.rc : 0; msg = d.rc ? d.err : "borradas"; goto out;
    }
    int lpn;
    if (list[0]) {                        /* lista que ya existe en el reproductor: nada que escribir */
        lpn = pl_w16(list, lw, 1024);
        rows = -1;                        /* n de la lista (param 8) = su fila de MANAGER_TABLE, si la hay */
        struct pl_db d = { *(void **)*(uint32_t *const *)(uintptr_t)HP_DBCTX, 0, 0 };
        PLF(4, pl_void_t)();
        pl_sql(&d, "SELECT count FROM MANAGER_TABLE WHERE path = ?1", lw, lpn, 0, 0, INT32_MIN, &rows);
        PLF(5, pl_void_t)();
        if (d.rc) rows = -1;
    } else {
        if (!n) { rc = -EINVAL; msg = "sin entradas f="; goto out; }
        /* la UBIFS casi llena se vuelve lentisima (GC) y ya colgo el aparato: escribir la cola solo con margen */
        { struct statvfs v; if (!statvfs("/usr/data", &v) && (unsigned long long)v.f_bavail * v.f_frsize < (6ull << 20) + (unsigned long long)n * 2048)
            { rc = -ENOSPC; msg = "poco espacio en /usr/data: no se escribe la cola"; goto out; } }
        /* la ranura que no suena */
        static hw16 cur[0x104];
        memset(cur, 0, sizeof cur);
        PLF(1, pl_getp_t)(0x15, cur, 0);
        static const hw16 q1[] = { 'h','i','b','y','m','o','d','\\','q','1',0 };
        slot = memcmp(cur, q1, sizeof q1) ? 1 : 2;
        char lp[16]; snprintf(lp, sizeof lp, "hibymod\\q%d", slot);
        lpn = pl_w16(lp, lw, 1024);
        rows = pl_build(PL_TB0 + slot - 1, lw, lpn, e, n, unscanned, idx, &idx, &msg);
        if (rows < 0) { rc = rows; goto out; }
    }
    /* secuencia de la interfaz de fabrica (0x4b45c8..0x4b4634) */
    if (mode >= 0 && mode <= 3) {         /* 0 en orden, 1 repetir pista, 2 aleatorio, 3 repetir lista */
        PLF(3, pl_sset_t)(0x1a, mode);    /* como setPlayMode de HiBy Link (0x4578e0) */
        PLF(0, pl_setp_t)(0x0a, (const void *)(intptr_t)mode, 0);
    }
    if (title[0]) PLF(0, pl_setp_t)(0x1b, title, 0);
    PLF(0, pl_setp_t)(0x17, 0, 0);
    PLF(0, pl_setp_t)(0x15, lw, 0);
    if (rows > 0) PLF(0, pl_setp_t)(0x08, (const void *)(intptr_t)rows, 0);
    int curi = 0;
    PLF(1, pl_getp_t)(0x0b, &curi, 0);
    PLF(0, pl_setp_t)(0x10, (const void *)(intptr_t)curi, 0);
    rc = PLF(2, pl_play_t)(idx, pos >= 0 ? pos : -1, 0x10002);
    msg = rc ? "play_index fallo" : "ok";
out:
    pl_result(id, rc, rows, idx, slot, np_now() - t0, msg);
    free(e); free(buf);
    __sync_synchronize();
    pl_busy = 0;
    return 0;
}

static void pl_kick(void)
{
    if (__sync_lock_test_and_set(&pl_busy, 1)) {      /* solo al registro: PL_RES es de la peticion en curso */
        int f = r_open("/usr/data/hibymod/disp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f >= 0) { if (write(f, "pl: ocupado, orden 'L' ignorada\n", 32) < 0) {} r_close(f); }
        return;
    }
    pthread_attr_t a; pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);    /* las funciones del reproductor usan marcos de 3-12 KB */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, pl_thread, 0)) { pl_busy = 0; pl_result(-1, -EAGAIN, 0, 0, 0, 0, "pthread_create"); }
    pthread_attr_destroy(&a);
}

/* ================================================================ v7: ajustes del reproductor ('S')
 * Lee y cambia opciones del reproductor (BT, Wi-Fi, DLNA, AirPlay, HiBy Link, ganancia, MSEB,
 * line out, modo USB, balance, gapless, fundido, filtro digital, temporizador, modo de
 * reproduccion y volumen) SIN mostrar su interfaz, con la misma secuencia de llamadas que su
 * centro de control (0x54d0a0..0x54dd40) o, para el volumen, que HiBy Link (0x4578a0).
 * Detalles, evidencia y riesgos: docs/research_settings_api.md.
 *
 *   setting_get(id) 0x42a500: lectura pura de un campo de la estructura de ajustes.
 *   setting_set(id, v) 0x42baa0: guarda el campo, APLICA el cambio (BT:ON a sys_server, control
 *     del mezclador, parametro del motor de audio...) y escribe el fichero de ajustes.
 *   No se toma el cerrojo del reproductor: setting_set(0x1a) llama a set_param, que lo toma, y
 *   HiBy Link / DLNA ya llaman a setting_set desde sus propios hilos sin el.
 *
 * Todo se comprueba antes de llamar a nada (firmas de 16 bytes, tablas de saltos de
 * setting_get/setting_set para cada id usado, /proc/self/maps): con otro binario la orden
 * responde rc=-ENOEXEC y no llama a nada. Un hilo propio atiende las peticiones de una en una.
 *
 * Peticion (ST_REQ, escrita con rename atomico):  HMSET1 / id=N / op=get|set|list|check /
 *            key=NOMBRE / val=V / [force=1]
 * Respuesta (ST_RES): "id=N rc=R key=K val=V ms=T mensaje" y, para list, una linea "k=v" por
 * opcion. Una linea por peticion en disp.log ("st: ..."). */
#define ST_REQ "/tmp/hibymod_setreq"
#define ST_RES "/tmp/hibymod_setres"
#define HP_SETTINGS  0x969590u            /* void* -> estructura de ajustes (0xad0 bytes) */
#define HP_SGET_TBL  0x7f09c0u            /* tabla de saltos de setting_get (0x8f entradas) */
#define HP_SSET_TBL  0x7f0bfcu            /* tabla de saltos de setting_set (0x8f entradas) */
#define HP_NSET      0x8f
static const struct { uint32_t addr, w[4]; } st_sig[] = {
    { 0x42a500, { 0x3c020097, 0x8c429590, 0x27bdffe0, 0x1040013b } },   /* 0 setting_get(id) */
    { 0x42baa0, { 0x27bdfdd0, 0xafb10224, 0x3c110097, 0xafb20228 } },   /* 1 setting_set(id, v) */
    { 0x47d0c0, { 0x3c020097, 0x8c42cc2c, 0x27bdff60, 0xafb00098 } },   /* 2 volume_apply(v) */
    { 0x48a900, { 0x3c020097, 0x8c42e020, 0x10400003, 0x00000000 } },   /* 3 ui_post(msg) */
    { 0x4f62e0, { 0x27bdff60, 0xafb00098, 0xafbf009c, 0x0c11f3d8 } },   /* 4 item_locked(id) */
    { 0x439ea0, { 0x27bdffd8, 0xafbf0024, 0x0c10dc98, 0x00000000 } },   /* 5 mqa_playing() */
};
typedef intptr_t (*st_get_t)(int);
typedef void (*st_set_t)(int, intptr_t);
typedef int (*st_i1_t)(int);
typedef int (*st_i0_t)(void);
typedef void (*st_v1_t)(int);
#define STF(i, t) ((t)(uintptr_t)st_sig[i].addr)

enum {
    SF_LOCK   = 1,     /* se rechaza si item_locked(id) (la interfaz lo muestra en gris) */
    SF_CONF1  = 2,     /* ponerlo a 1 requiere force=1 (la interfaz pide confirmacion) */
    SF_CONFX  = 4,     /* cualquier cambio requiere force=1 */
    SF_MSEB   = 8,     /* condiciones del CC: velocidad 100 %, sin MQA sonando */
    SF_VOLUME = 16,    /* secuencia de setVolume de HiBy Link */
    SF_RO     = 32,    /* solo lectura */
    SF_POST   = 64,    /* tras setting_set, mensaje msg_on/msg_off a la cola de la interfaz (como el CC) */
};
static const struct st_opt {
    const char *name; int id, lo, hi, flags, msg_on, msg_off;
    uint32_t get_case, set_case;          /* destinos esperados en las tablas de saltos */
} st_opt[] = {
    { "bt",        0x23,   0,   1, 0,           0,     0,     0x42a8d4, 0x42c24c },
    { "wifi",      0x33,   0,   1, 0,           0,     0,     0x42a854, 0x42c49c },
    { "dlna",      0x35,   0,   1, SF_POST,     0x12e, 0x12f, 0x42a844, 0x42c4c0 },
    { "airplay",   0x34,   0,   1, SF_POST,     0x130, 0x131, 0x42a84c, 0x42c488 },
    { "hibylink",  0x2f,   0,   1, 0,           0,     0,     0x42a874, 0x42c418 },
    { "gain",      0x15,   0,   1, SF_LOCK,     0,     0,     0x42a934, 0x42c058 },
    { "mseb",      0x3f,   0,   1, SF_MSEB,     0,     0,     0x42a7f4, 0x42bdfc },
    { "lineout",   0x2d,   0,   1, SF_LOCK | SF_CONF1, 0, 0,  0x42a884, 0x42c3a4 },
    { "usbmode",   0x08,   0,   1, SF_CONFX,    0,     0,     0x42a99c, 0x42c520 },
    { "balance",   0x14, -10,  10, SF_LOCK,     0,     0,     0x42a93c, 0x42c078 },
    { "gapless",   0x1b,   0,   1, 0,           0,     0,     0x42a90c, 0x42c2b0 },
    { "fade",      0x32,   0,   1, 0,           0,     0,     0x42a85c, 0x42c354 },
    { "filter",    0x21,   0,   1, SF_LOCK,     0,     0,     0x42a8e4, 0x42c330 },
    { "sleep",     0x09,   0,   1, 0,           0,     0,     0x42a994, 0x42c708 },
    { "sleepmin",  0x0a,   5, 120, 0,           0,     0,     0x42a98c, 0x42c6e8 },
    { "playmode",  0x1a,   0,   3, 0,           0,     0,     0x42a914, 0x42c2f8 },
    { "volume",    0x11,   0, 255, SF_VOLUME,   0,     0,     0x42a954, 0x42c0c0 },
    { "volmax",    0x10,   0, 255, SF_RO,       0,     0,     0x42a95c, 0 },
    { "speed",     0x65,   0, 255, SF_RO,       0,     0,     0x42a698, 0 },   /* % (MSEB necesita 100) */
};
#define ST_NOPT ((int)(sizeof st_opt / sizeof st_opt[0]))
#define ST_ID_LINEOUT 0x2d
#define ST_ID_VOLMAX  0x10
#define ST_ID_SPEED   0x65

static volatile int st_busy, st_again;
static struct stat st_last;               /* fichero de la ultima peticion atendida (ino + mtime) */

static int st_check(void)
{
    for (size_t i = 0; i < sizeof st_sig / sizeof st_sig[0]; i++)
        if (!pl_mapped(st_sig[i].addr, 16, 1) || memcmp((const void *)(uintptr_t)st_sig[i].addr, st_sig[i].w, 16)) return -ENOEXEC;
    if (!pl_mapped(HP_SGET_TBL, HP_NSET * 4, 0) || !pl_mapped(HP_SSET_TBL, HP_NSET * 4, 0)) return -ENOEXEC;
    const uint32_t *gt = (const uint32_t *)(uintptr_t)HP_SGET_TBL, *ss = (const uint32_t *)(uintptr_t)HP_SSET_TBL;
    for (int i = 0; i < ST_NOPT; i++) {
        if (gt[st_opt[i].id] != st_opt[i].get_case) return -ENOEXEC;
        if (st_opt[i].set_case && ss[st_opt[i].id] != st_opt[i].set_case) return -ENOEXEC;
    }
    if (!pl_mapped(HP_SETTINGS, 4, 0)) return -ENOEXEC;
    uint32_t s = *(const uint32_t *)(uintptr_t)HP_SETTINGS;
    if (!s || !pl_mapped(s, 0xad0, 0)) return -EAGAIN;              /* ajustes aun sin cargar */
    return 0;
}

static const struct st_opt *st_find(const char *k)
{
    for (int i = 0; i < ST_NOPT; i++) if (!strcmp(k, st_opt[i].name)) return &st_opt[i];
    return 0;
}

static int st_get(int id) { return (int)STF(0, st_get_t)(id); }

/* aplica v con la secuencia de la interfaz de fabrica; devuelve 0 o <0 y deja el valor en *now */
static int st_set(const struct st_opt *o, int v, int force, int *now, const char **msg)
{
    int cur = st_get(o->id);
    *now = cur;
    if (o->flags & SF_RO) { *msg = "solo lectura"; return -EROFS; }
    if (v < o->lo || v > o->hi) { *msg = "fuera de rango"; return -ERANGE; }
    if (o->flags & SF_VOLUME) {
        if (v > st_get(ST_ID_VOLMAX)) { *msg = "mayor que volmax"; return -ERANGE; }
        if (st_get(ST_ID_LINEOUT) == 1) { *msg = "line out activo: volumen fijo"; return -EPERM; }
    }
    if (cur == v) { *msg = "sin cambios"; return 0; }
    if ((o->flags & SF_LOCK) && STF(4, st_i1_t)(o->id)) { *msg = "bloqueado por la salida actual (gris en la interfaz)"; return -EPERM; }
    if (((o->flags & SF_CONFX) || ((o->flags & SF_CONF1) && v == 1)) && !force) { *msg = "requiere force=1 (la interfaz pide confirmacion)"; return -EACCES; }
    if (o->flags & SF_MSEB) {
        if (st_get(ST_ID_SPEED) != 100) { *msg = "MSEB: velocidad de reproduccion distinta de 100%"; return -EPERM; }
        if (STF(5, st_i0_t)()) { *msg = "MSEB: suena MQA"; return -EPERM; }
    }
    if (o->flags & SF_VOLUME) {                       /* = setVolume de HiBy Link (0x4578a0) */
        STF(2, st_i1_t)(v & 0xff);
        STF(1, st_set_t)(o->id, v);
    } else {
        STF(1, st_set_t)(o->id, v);                   /* guarda + aplica + escribe el fichero */
        if (o->flags & SF_POST) STF(3, st_v1_t)(v ? o->msg_on : o->msg_off);   /* como el CC */
    }
    *now = st_get(o->id);
    if (*now != v) { *msg = "no aplicado (ocupado o rechazado por el reproductor)"; return -EAGAIN; }
    *msg = "ok";
    return 0;
}

static void st_result(const char *head, const char *extra)
{
    int f = r_open(ST_RES ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) {
        if (write(f, head, strlen(head)) < 0 || (extra && write(f, extra, strlen(extra)) < 0)) {}
        r_close(f); rename(ST_RES ".tmp", ST_RES);
    }
    f = r_open("/usr/data/hibymod/disp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f >= 0) { if (write(f, "st: ", 4) < 0 || write(f, head, strlen(head)) < 0) {} r_close(f); }
}

static void st_serve(void)
{
    uint32_t t0 = np_now();
    int id = -1, rc = 0, val = 0, force = 0, now = 0;
    char buf[1024], op[16] = "", key[24] = "";
    static char extra[1024];
    const char *msg = "";
    extra[0] = 0;
    int f = r_open(ST_REQ, O_RDONLY);
    if (f < 0) { rc = -ENOENT; msg = ST_REQ; goto out; }
    struct stat sb;
    if (fstat(f, &sb)) memset(&sb, 0, sizeof sb);
    ssize_t got = read(f, buf, sizeof buf - 1); r_close(f);
    if (got <= 0) { rc = -EINVAL; msg = "peticion vacia"; goto out; }
    buf[got] = 0;
    if (strncmp(buf, "HMSET1\n", 7)) { rc = -EINVAL; msg = "falta HMSET1"; goto out; }
    for (char *l = buf + 7, *nx; l && *l; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        size_t L = strlen(l); if (L && l[L - 1] == '\r') l[--L] = 0;
        if (!strncmp(l, "id=", 3)) id = atoi(l + 3);
        else if (!strncmp(l, "op=", 3)) snprintf(op, sizeof op, "%s", l + 3);
        else if (!strncmp(l, "key=", 4)) snprintf(key, sizeof key, "%s", l + 4);
        else if (!strncmp(l, "val=", 4)) val = atoi(l + 4);
        else if (!strcmp(l, "force=1")) force = 1;
    }
    /* ya atendida (otra 'S' sin peticion nueva): el cliente escribe cada una con rename, asi que
     * una peticion nueva es siempre otro inodo */
    if (sb.st_ino && sb.st_ino == st_last.st_ino && sb.st_mtim.tv_sec == st_last.st_mtim.tv_sec &&
        sb.st_mtim.tv_nsec == st_last.st_mtim.tv_nsec && sb.st_size == st_last.st_size) return;
    st_last = sb;
    if ((rc = st_check())) { msg = rc == -ENOEXEC ? "firmware distinto: no se llama a nada" : "ajustes del reproductor sin cargar"; goto out; }
    if (!strcmp(op, "check")) { msg = "firmas ok"; goto out; }
    if (!strcmp(op, "list") || (!strcmp(op, "get") && !strcmp(key, "all"))) {
        size_t k = 0;
        for (int i = 0; i < ST_NOPT && k + 40 < sizeof extra; i++)
            k += (size_t)snprintf(extra + k, sizeof extra - k, "%s=%d\n", st_opt[i].name, st_get(st_opt[i].id));
        val = ST_NOPT; msg = "ok"; goto out;
    }
    const struct st_opt *o = st_find(key);
    if (!o) { rc = -ENOENT; msg = "opcion desconocida"; goto out; }
    if (!strcmp(op, "get")) { val = st_get(o->id); msg = "ok"; goto out; }
    if (!strcmp(op, "set")) { rc = st_set(o, val, force, &now, &msg); val = now; goto out; }
    rc = -EINVAL; msg = "op desconocida";
out:;
    char head[256];
    snprintf(head, sizeof head, "id=%d rc=%d key=%s val=%d ms=%u %s\n", id, rc, key[0] ? key : "-", val, np_now() - t0, msg);
    st_result(head, extra);
}

static void *st_thread(void *arg)
{
    (void)arg;
    for (;;) {
        while (__sync_lock_test_and_set(&st_again, 0)) st_serve();
        __sync_lock_release(&st_busy);
        __sync_synchronize();
        /* una 'S' que llego justo mientras se soltaba st_busy: atenderla aqui */
        if (!st_again || __sync_lock_test_and_set(&st_busy, 1)) break;
    }
    return 0;
}

static void st_kick(void)
{
    st_again = 1;
    __sync_synchronize();
    if (__sync_lock_test_and_set(&st_busy, 1)) return;    /* el hilo en curso la atendera */
    pthread_attr_t a; pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);    /* marcos de hasta ~2 KB + system() del modo USB */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, st_thread, 0)) {
        st_again = 0; __sync_lock_release(&st_busy);
        st_result("id=-1 rc=-11 key=- val=0 ms=0 pthread_create\n", 0);
    }
    pthread_attr_destroy(&a);
}
