/*
 * libnsfb surface "linux" para HiBy R3 II dentro de HiByOS (app .hpk "Navegador").
 *
 * Pantalla: NetSurf pinta en un lienzo en RAM; al presentar se copia ENTERO al bufer oculto de
 * /dev/fb0 (320x480 RGB565, virtual 320x960) y despues se hace FBIOPAN_DISPLAY (que espera al
 * final del fotograma): nunca se escribe en el bufer que se barre (docs/FORMATO_APPS.md §6).
 *
 * Tactil: HM_TOUCH (tactil virtual que alimenta el shell, protocolo A); si no existe, event1.
 *   - toque corto = clic (MOUSE_1)
 *   - arrastre = desplazamiento 1:1 con el dedo (MOUSE_5 con la cantidad en el evento, ver el
 *     parche de gui.c en build_hf.sh) y, al soltar, inercia estilo iOS (deceleracion 0,998/ms)
 * SIGTERM (el shell lo manda al deslizar la barra de inicio) = salir.
 * Licencia MIT (como libnsfb).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"
#include "libnsfb_event.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"

#define QMAX 64
#define DRAG_THRESHOLD 8


struct hiby_priv {
    int fb;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *map; size_t maplen;
    uint8_t *canvas;
    int back;                       /* 0/1: bufer oculto */
    int dirty;
    int in;
    int tx, ty, down, was_down, sx, sy, lastx, lasty, dragging, axis; /* axis: 0 libre, 1 v, 2 h */
    double hist_t[8]; int hist_x[8], hist_y[8], nh;
    double vx, vy, mlast; int moving; double accx, accy;
    nsfb_event_t q[QMAX]; int qh, qt;
};

static volatile sig_atomic_t g_term;
static void on_term(int s) { (void)s; g_term = 1; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void qpush(struct hiby_priv *p, nsfb_event_t e) { int n = (p->qt + 1) % QMAX; if (n != p->qh) { p->q[p->qt] = e; p->qt = n; } }
static bool qpop(struct hiby_priv *p, nsfb_event_t *e) { if (p->qh == p->qt) return false; *e = p->q[p->qh]; p->qh = (p->qh + 1) % QMAX; return true; }
static void qkey(struct hiby_priv *p, int type, int code) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = type; e.value.keycode = code; qpush(p, e); }
static void qmove(struct hiby_priv *p, int x, int y) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = NSFB_EVENT_MOVE_ABSOLUTE; e.value.vector.x = x; e.value.vector.y = y; qpush(p, e); }

/* un desplazamiento en pixeles: MOUSE_5 pulsado con dy en value.vector.y y dx en value.vector.z
 * (keycode solapa vector.x). Un MOUSE_5 normal los trae a 0. Lo interpreta el parche de gui.c
 * (build_hf.sh); varios seguidos se suman en el mismo pan antes de redibujar. */
static void qscroll(struct hiby_priv *p, int dx, int dy)
{
    if (!dx && !dy) return;
    nsfb_event_t e; memset(&e, 0, sizeof e); e.type = NSFB_EVENT_KEY_DOWN; e.value.keycode = NSFB_KEY_MOUSE_5;
    e.value.vector.y = dy; e.value.vector.z = dx;
    qpush(p, e);
}

static void present(struct hiby_priv *p)
{
    if (!p->dirty) return;
    size_t fsz = (size_t)p->fix.line_length * p->var.yres;
    if ((size_t)(p->back + 1) * fsz <= p->maplen) {
        memcpy(p->map + p->back * fsz, p->canvas, fsz);
        p->var.xoffset = 0; p->var.yoffset = p->back * p->var.yres;
        ioctl(p->fb, FBIOPAN_DISPLAY, &p->var);
        if (p->maplen >= 2 * fsz) p->back ^= 1;
    }
    p->dirty = 0;
}

static int hiby_defaults(nsfb_t *nsfb)
{
    nsfb->width = 320; nsfb->height = 480; nsfb->format = NSFB_FMT_RGB565; nsfb->bpp = 16;
    select_plotters(nsfb);
    return 0;
}

static int hiby_initialise(nsfb_t *nsfb)
{
    struct hiby_priv *p = calloc(1, sizeof *p);
    const char *dev = getenv("HM_FB") ? getenv("HM_FB") : "/dev/fb0";
    if (!p) return -1;
    p->fb = open(dev, O_RDWR);
    if (p->fb < 0) { perror(dev); free(p); return -1; }
    ioctl(p->fb, FBIOGET_VSCREENINFO, &p->var);
    ioctl(p->fb, FBIOGET_FSCREENINFO, &p->fix);
    p->maplen = p->fix.smem_len;
    p->map = mmap(NULL, p->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, p->fb, 0);
    if (p->map == MAP_FAILED) { perror("mmap fb"); close(p->fb); free(p); return -1; }
    nsfb->width = p->var.xres; nsfb->height = p->var.yres;
    nsfb->format = NSFB_FMT_RGB565; nsfb->bpp = 16;
    p->canvas = calloc(1, (size_t)p->fix.line_length * p->var.yres);
    if (!p->canvas) { munmap(p->map, p->maplen); close(p->fb); free(p); return -1; }
    p->back = p->var.yoffset >= p->var.yres ? 0 : 1;     /* el que NO se ve */
    nsfb->ptr = p->canvas; nsfb->linelen = p->fix.line_length;
    select_plotters(nsfb);
    const char *ts = getenv("HM_TOUCH");
    p->in = open(ts && *ts ? ts : "/dev/input/event1", O_RDONLY | O_NONBLOCK);
    p->tx = p->ty = -1;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_term;   /* sin SA_RESTART: poll -> EINTR */
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); sigaction(SIGHUP, &sa, 0);
    nsfb->surface_priv = p;
    return 0;
}

static int hiby_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    (void)width; (void)height;
    if (format != NSFB_FMT_ANY && format != NSFB_FMT_RGB565) return -1;
    if (nsfb->surface_priv == NULL) { nsfb->width = 320; nsfb->height = 480; nsfb->format = NSFB_FMT_RGB565; nsfb->bpp = 16; select_plotters(nsfb); }
    return 0;
}

static int hiby_finalise(nsfb_t *nsfb)
{
    struct hiby_priv *p = nsfb->surface_priv;
    if (!p) return 0;
    if (p->in >= 0) close(p->in);
    munmap(p->map, p->maplen); close(p->fb); free(p->canvas); free(p);
    nsfb->surface_priv = NULL; nsfb->ptr = NULL;
    return 0;
}

static void touch_frame(struct hiby_priv *p)
{
    double t = now_ms();
    if (p->down && !p->was_down) {           /* inicio de toque: para la inercia */
        p->moving = 0; p->accx = p->accy = 0;
        p->sx = p->lastx = p->tx; p->sy = p->lasty = p->ty; p->dragging = 0; p->axis = 0; p->nh = 0;
        qmove(p, p->tx, p->ty);
    } else if (p->down && p->was_down) {     /* movimiento */
        if (!p->dragging && (abs(p->ty - p->sy) > DRAG_THRESHOLD || abs(p->tx - p->sx) > DRAG_THRESHOLD)) {
            p->dragging = 1;
            int ax = abs(p->tx - p->sx), ay = abs(p->ty - p->sy);
            p->axis = ay > 2 * ax ? 1 : ax > 2 * ay ? 2 : 0;          /* bloqueo de eje como iOS */
            p->lastx = p->tx; p->lasty = p->ty;
        }
        if (p->dragging) {
            int dx = p->axis == 1 ? 0 : p->lastx - p->tx, dy = p->axis == 2 ? 0 : p->lasty - p->ty;
            qscroll(p, dx, dy);
            p->lastx = p->tx; p->lasty = p->ty;
        }
    } else if (!p->down && p->was_down) {    /* fin: clic si no hubo arrastre; si no, inercia */
        if (!p->dragging) {
            qmove(p, p->sx, p->sy);
            qkey(p, NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_1);
            qkey(p, NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_1);
        } else if (p->nh >= 2) {
            int a = 0; while (a < p->nh - 1 && t - p->hist_t[a] > 90) a++;
            int b = p->nh - 1; double dt = p->hist_t[b] - p->hist_t[a];
            if (dt > 5 && t - p->hist_t[b] < 60) {
                p->vx = p->axis == 1 ? 0 : -(p->hist_x[b] - p->hist_x[a]) / dt;
                p->vy = p->axis == 2 ? 0 : -(p->hist_y[b] - p->hist_y[a]) / dt;
                if (fabs(p->vx) + fabs(p->vy) > 0.15) { p->moving = 1; p->mlast = t; p->accx = p->accy = 0; }
            }
        }
    }
    if (p->down) {
        if (p->nh == 8) { memmove(p->hist_t, p->hist_t + 1, 7 * sizeof(double)); memmove(p->hist_x, p->hist_x + 1, 7 * sizeof(int)); memmove(p->hist_y, p->hist_y + 1, 7 * sizeof(int)); p->nh = 7; }
        p->hist_t[p->nh] = t; p->hist_x[p->nh] = p->tx; p->hist_y[p->nh] = p->ty; p->nh++;
    }
    p->was_down = p->down;
}

/* un paso de inercia (llamado una vez por fotograma mientras dura) */
static void momentum_step(struct hiby_priv *p)
{
    double t = now_ms(), dt = t - p->mlast; if (dt > 50) dt = 50; p->mlast = t;
    double k = pow(0.998, dt);
    p->accx += p->vx * dt; p->accy += p->vy * dt;
    p->vx *= k; p->vy *= k;
    int dx = (int)p->accx, dy = (int)p->accy; p->accx -= dx; p->accy -= dy;
    qscroll(p, dx, dy);
    if (fabs(p->vx) + fabs(p->vy) < 0.02) p->moving = 0;
}

static bool hiby_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct hiby_priv *p = nsfb->surface_priv;
    if (!p) return false;
    present(p);
    if (g_term) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = NSFB_EVENT_CONTROL; e.value.controlcode = NSFB_CONTROL_QUIT; *event = e; return true; }
    if (qpop(p, event)) return true;
    if (p->moving) {
        if (timeout < 0 || timeout > 16) timeout = 16;   /* el pan ya espera al fotograma */
    }
    struct pollfd pf = { p->in, POLLIN, 0 };
    int r = p->in >= 0 ? poll(&pf, 1, timeout < 0 ? -1 : timeout) : (timeout > 0 ? usleep(timeout * 1000), 0 : 0);
    if (g_term) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = NSFB_EVENT_CONTROL; e.value.controlcode = NSFB_CONTROL_QUIT; *event = e; return true; }
    if (r > 0 && (pf.revents & POLLIN)) {
        struct input_event ev[64];
        ssize_t n = read(p->in, ev, sizeof ev);
        for (int j = 0; j < n / (ssize_t)sizeof(struct input_event); j++) {
            struct input_event *e = &ev[j];
            if (e->type == EV_KEY && e->code == BTN_TOUCH) p->down = e->value;
            else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_X) p->tx = e->value;
            else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_Y) p->ty = e->value;
            else if (e->type == EV_SYN && e->code == SYN_REPORT) touch_frame(p);
        }
    }
    if (p->moving) momentum_step(p);
    return qpop(p, event);
}

static int hiby_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if ((cursor != NULL) && (cursor->plotted == true) && (nsfb_plot_bbox_intersect(box, &cursor->loc)))
        nsfb_cursor_clear(nsfb, cursor);
    return 0;
}

static int hiby_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    nsfb_bbox_t sclip;
    if ((cursor != NULL) && (cursor->plotted == true)) {
        sclip = nsfb->clip;
        nsfb_plot_set_clip(nsfb, NULL);
        nsfb_cursor_clear(nsfb, cursor);
        nsfb->clip = sclip;
    }
    return true;
}

static int hiby_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct hiby_priv *p = nsfb->surface_priv;
    (void)box;
    if (p) p->dirty = 1;      /* se presenta en el siguiente hiby_input (una vez por vuelta del bucle) */
    return 0;
}

const nsfb_surface_rtns_t hiby_rtns = {
    .defaults = hiby_defaults,
    .initialise = hiby_initialise,
    .finalise = hiby_finalise,
    .input = hiby_input,
    .claim = hiby_claim,
    .update = hiby_update,
    .cursor = hiby_cursor,
    .geometry = hiby_set_geometry,
};

NSFB_SURFACE_DEF(linux, NSFB_SURFACE_LINUX, &hiby_rtns)
