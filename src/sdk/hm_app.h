/* hm_app.h - SDK minimo (solo cabecera) para apps de hibymod en el HiBy R3 II.
 *
 * Pantalla, tactil, teclas, idioma y salida limpia. Nada mas: el dibujo es cosa de la app.
 * Contrato completo en docs/APP_FORMAT.md. Uso tipico:
 *
 *   #include "hm_app.h"
 *   static uint16_t img[HM_W * HM_H];                  // lienzo en RAM
 *   static void tocar(int ev, int x, int y, void *c) { ... HM_DOWN / HM_MOVE / HM_UP ... }
 *   int main(void) {
 *       hm_fb fb; hm_touch t; hm_keys k;
 *       hm_app_init();                                  // SIGTERM -> hm_quit
 *       if (hm_fb_open(&fb)) return 1;
 *       hm_touch_open(&t); hm_keys_open(&k);
 *       while (!hm_quit) {
 *           hm_wait(&t, &k, 23);                        // ~44 fps como maximo
 *           hm_touch_read(&t, tocar, 0);
 *           ... dibujar TODO el lienzo ...
 *           hm_fb_present(&fb, img);                    // copia al bufer oculto y lo muestra
 *       }
 *       hm_fb_close(&fb); hm_touch_close(&t); hm_keys_close(&k);
 *       return 0;
 *   }
 *
 * Panel SLCD con memoria propia: cada FBIOPAN_DISPLAY transfiere la imagen. Se escribe el bufer
 * oculto ENTERO y despues se presenta; nunca se escribe en el bufer que se esta mostrando.
 * hm_fb_present() ya lo hace bien: no escribas en fb.mem directamente salvo que sepas por que. */
#ifndef HM_APP_H
#define HM_APP_H

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

#define HM_W 320
#define HM_H 480
#define HM_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* ---------------------------------------------------------------- salida limpia */
/* El shell manda SIGTERM al deslizar hacia arriba desde la barra de inicio: hay que salir en
 * menos de 1 s (si no, llega SIGKILL). */
static volatile sig_atomic_t hm_quit;
static void hm__on_term(int s) { (void)s; hm_quit = 1; }
static inline void hm_app_init(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = hm__on_term;                   /* sin SA_RESTART: poll() vuelve con EINTR */
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
}

/* ---------------------------------------------------------------- idioma */
enum { HM_ES, HM_EN, HM_FR };
static inline int hm_lang(void)
{
    const char *l = getenv("HM_LANG");
    if (!l) return HM_ES;
    return !strncmp(l, "en", 2) ? HM_EN : !strncmp(l, "fr", 2) ? HM_FR : HM_ES;
}
static inline const char *hm_tr(const char *es, const char *en, const char *fr)
{
    int l = hm_lang(); return l == HM_EN ? en : l == HM_FR ? fr : es;
}
static inline const char *hm_app_dir(void) { const char *d = getenv("HM_APP_DIR"); return d ? d : "."; }
static inline const char *hm_data_dir(void) { const char *d = getenv("HM_DATA_DIR"); return d ? d : "."; }

static inline long hm_now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* ---------------------------------------------------------------- pantalla */
typedef struct {
    int fd, back, nbuf;               /* back: indice del bufer oculto (0/1); nbuf 1 o 2 */
    uint8_t *mem; size_t len, line;   /* mmap del framebuffer y bytes por fila */
    struct fb_var_screeninfo v;
} hm_fb;

static inline int hm_fb_open(hm_fb *f)
{
    const char *p = getenv("HM_FB");
    struct fb_fix_screeninfo fx;
    memset(f, 0, sizeof *f);
    f->fd = open(p ? p : "/dev/fb0", O_RDWR | O_CLOEXEC);
    if (f->fd < 0) return -1;
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->v) || ioctl(f->fd, FBIOGET_FSCREENINFO, &fx) ||
        f->v.bits_per_pixel != 16 || f->v.xres != HM_W || f->v.yres != HM_H) { close(f->fd); return -1; }
    f->len = fx.smem_len; f->line = fx.line_length;
    f->mem = mmap(0, f->len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->mem == MAP_FAILED) { close(f->fd); return -1; }
    f->nbuf = (f->v.yres_virtual >= 2 * HM_H && f->len >= 2 * f->line * HM_H) ? 2 : 1;
    f->back = f->nbuf == 2 && f->v.yoffset == 0 ? 1 : 0;   /* el que NO se esta mostrando */
    return 0;
}

/* copia el lienzo completo (HM_W*HM_H RGB565) al bufer oculto y lo presenta */
static inline void hm_fb_present(hm_fb *f, const uint16_t *img)
{
    uint8_t *dst = f->mem + f->line * HM_H * f->back;
    if (f->line == HM_W * 2) memcpy(dst, img, HM_W * HM_H * 2);
    else for (int y = 0; y < HM_H; y++) memcpy(dst + f->line * y, img + y * HM_W, HM_W * 2);
    f->v.yoffset = HM_H * f->back;
    ioctl(f->fd, FBIOPAN_DISPLAY, &f->v);
    if (f->nbuf == 2) f->back ^= 1;
}

static inline void hm_fb_close(hm_fb *f)
{
    if (f->mem && f->mem != MAP_FAILED) munmap(f->mem, f->len);
    if (f->fd > 0) close(f->fd);
    f->mem = 0; f->fd = -1;
}

/* ---------------------------------------------------------------- tactil (protocolo A) */
enum { HM_DOWN, HM_MOVE, HM_UP };
typedef void (*hm_touch_cb)(int ev, int x, int y, void *ctx);
typedef struct { int fd, x, y, nx, ny, down, ndown, got; } hm_touch;

static inline int hm_touch_open(hm_touch *t)
{
    const char *p = getenv("HM_TOUCH");
    memset(t, 0, sizeof *t);
    t->fd = open(p ? p : "/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    t->nx = t->ny = -1;
    return t->fd < 0 ? -1 : 0;
}

/* lee todo lo pendiente (no bloquea) y llama a cb con DOWN/MOVE/UP del primer dedo */
static inline int hm_touch_read(hm_touch *t, hm_touch_cb cb, void *ctx)
{
    struct input_event e[32]; int n, evs = 0;
    if (t->fd < 0) return 0;
    while ((n = read(t->fd, e, sizeof e)) > 0) {
        for (int i = 0; i < n / (int)sizeof e[0]; i++) {
            if (e[i].type == EV_KEY && e[i].code == BTN_TOUCH) t->ndown = e[i].value != 0;
            else if (e[i].type == EV_ABS && e[i].code == ABS_MT_POSITION_X && !t->got) t->nx = e[i].value;
            else if (e[i].type == EV_ABS && e[i].code == ABS_MT_POSITION_Y && !t->got) t->ny = e[i].value;
            else if (e[i].type == EV_SYN && e[i].code == SYN_MT_REPORT) { if (t->nx >= 0) t->got = 1; }
            else if (e[i].type == EV_SYN && e[i].code == SYN_REPORT) {
                int dn = t->ndown && t->got;             /* trama sin contactos = dedo levantado */
                if (dn && !t->down) { t->x = t->nx; t->y = t->ny; if (cb) cb(HM_DOWN, t->x, t->y, ctx); evs++; }
                else if (dn && (t->nx != t->x || t->ny != t->y)) { t->x = t->nx; t->y = t->ny; if (cb) cb(HM_MOVE, t->x, t->y, ctx); evs++; }
                else if (!dn && t->down) { if (cb) cb(HM_UP, t->x, t->y, ctx); evs++; }
                t->down = dn; t->got = 0; t->nx = t->ny = -1;
            }
        }
    }
    return evs;
}
static inline void hm_touch_close(hm_touch *t) { if (t->fd >= 0) close(t->fd); t->fd = -1; }

/* ---------------------------------------------------------------- teclas */
/* codigos linux/input.h: KEY_VOLUMEUP/DOWN, KEY_PLAYPAUSE, KEY_NEXTSONG, KEY_PREVIOUSSONG,
 * KEY_POWER... value: 1 pulsada, 0 soltada, 2 repeticion */
typedef void (*hm_key_cb)(int code, int value, void *ctx);
typedef struct { int fd; } hm_keys;
static inline int hm_keys_open(hm_keys *k)
{
    const char *p = getenv("HM_KEYS");
    k->fd = p ? open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : -1;
    return k->fd < 0 ? -1 : 0;
}
static inline int hm_keys_read(hm_keys *k, hm_key_cb cb, void *ctx)
{
    struct input_event e[16]; int n, evs = 0;
    if (k->fd < 0) return 0;
    while ((n = read(k->fd, e, sizeof e)) > 0)
        for (int i = 0; i < n / (int)sizeof e[0]; i++)
            if (e[i].type == EV_KEY) { if (cb) cb(e[i].code, e[i].value, ctx); evs++; }
    return evs;
}
static inline void hm_keys_close(hm_keys *k) { if (k->fd >= 0) close(k->fd); k->fd = -1; }

/* espera entrada (tactil o teclas) hasta ms milisegundos (-1 = sin limite). 1 si hay algo */
static inline int hm_wait(hm_touch *t, hm_keys *k, int ms)
{
    struct pollfd p[2]; int n = 0;
    if (t && t->fd >= 0) { p[n].fd = t->fd; p[n].events = POLLIN; n++; }
    if (k && k->fd >= 0) { p[n].fd = k->fd; p[n].events = POLLIN; n++; }
    if (!n) { if (ms > 0) usleep(ms * 1000); return 0; }
    return poll(p, n, ms) > 0;
}

#endif
