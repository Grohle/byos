/* hibymod_disp.so v5 - puente de pantalla y entrada entre hiby_player y hibyshell
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

/* El panel (SLCD con memoria propia) recibe la imagen en cada FBIOPAN: se escribe el
 * bufer y DESPUES se presenta. Escribir tras presentar corrompe la transferencia en curso. */
static void present_now(void) { r_ioctl(fb_fd, FBIOPAN_DISPLAY, &last_var); }

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

/* ---------------------------------------------------------------- ordenes */
static void *control(void *arg)
{
    (void)arg;
    for (;;) {
        int f = r_open(HM_FIFO, O_RDONLY);
        if (f < 0) { sleep(1); continue; }
        char c;
        while (read(f, &c, 1) == 1) {
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
                    wait_even(&st->fr0_seq);
                    uint32_t y0 = st->dirty_y0, y1 = st->dirty_y1;
                    if (y1 > y0 && y1 <= HM_H) memcpy(buf_at(last_var.yoffset) + y0 * fline, frames + y0 * fline, (y1 - y0) * fline);
                    else memcpy(buf_at(last_var.yoffset), frames, fsize);
                    present_now();
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
                    if (was_capture && frames) memcpy(d, frames + fsize, fsize);
                    st->ov_saved = 0;
                    ov_apply(d);
                    present_now();
                }
                st->presents++;
            } else if (c == 'O') {
                if (!st->capture && !st->held && map_fb()) {
                    unsigned char *d = buf_at(last_var.yoffset);
                    ov_restore(d);           /* quitar y poner la capa antes de presentar: */
                    ov_apply(d);             /* nunca se ve un fotograma "sin capa" */
                    present_now();
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
                    else if (fbmem && frames) { memcpy(buf_at(last_var.yoffset), frames + fsize, fsize); r_ioctl(fb_fd, FBIOPAN_DISPLAY, &last_var); }
                }
                st->ov_on = 0;
                if (fbmem) { ov_restore(buf_at(last_var.yoffset)); r_ioctl(fb_fd, FBIOPAN_DISPLAY, &last_var); }
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
    unlink(HM_FIFO);
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
            until.tv_sec += HOLD_MAX_S;
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
