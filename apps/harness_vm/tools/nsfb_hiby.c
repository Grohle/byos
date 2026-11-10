/*
 * libnsfb surface "linux" para HiBy R3 II: /dev/fb0 (320x480 RGB565, virtual 320x960)
 * + touch goodix-ts (protocolo A, /dev/input/event1) + teclas (event0, event2, event3).
 *
 * Gestos: toque corto = click (MOUSE_1); arrastre vertical = scroll (MOUSE_4/5 por cada 16 px);
 * Teclas: VOL+ / VOL- = PageUp/PageDown, PREV/NEXT = Izq/Der (scroll horizontal),
 *         PLAYPAUSE = Escape, POWER = salir. (Atras/adelante: botones de la barra del frontend.)
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

#define QMAX 32
#define DRAG_THRESHOLD 10
#define SCROLL_STEP 16

struct hiby_priv {
    int fb;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *map; size_t maplen;
    int in[4]; int nin;
    int tx, ty, down, was_down, sx, sy, lasty, dragging;
    nsfb_event_t q[QMAX]; int qh, qt;
};

static void qpush(struct hiby_priv *p, nsfb_event_t e) { int n = (p->qt + 1) % QMAX; if (n != p->qh) { p->q[p->qt] = e; p->qt = n; } }
static bool qpop(struct hiby_priv *p, nsfb_event_t *e) { if (p->qh == p->qt) return false; *e = p->q[p->qh]; p->qh = (p->qh + 1) % QMAX; return true; }
static void qkey(struct hiby_priv *p, int type, int code) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = type; e.value.keycode = code; qpush(p, e); }
static void qmove(struct hiby_priv *p, int x, int y) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = NSFB_EVENT_MOVE_ABSOLUTE; e.value.vector.x = x; e.value.vector.y = y; qpush(p, e); }

static int hiby_defaults(nsfb_t *nsfb)
{
    nsfb->width = 320; nsfb->height = 480; nsfb->format = NSFB_FMT_RGB565; nsfb->bpp = 16;
    select_plotters(nsfb);
    return 0;
}

static int hiby_initialise(nsfb_t *nsfb)
{
    struct hiby_priv *p = calloc(1, sizeof *p);
    const char *dev = getenv("NSFB_FBDEV") ? getenv("NSFB_FBDEV") : "/dev/fb0";
    if (!p) return -1;
    p->fb = open(dev, O_RDWR);
    if (p->fb < 0) { perror(dev); free(p); return -1; }
    ioctl(p->fb, FBIOGET_VSCREENINFO, &p->var);
    ioctl(p->fb, FBIOGET_FSCREENINFO, &p->fix);
    if (p->var.bits_per_pixel != 16) { fprintf(stderr, "hiby surface: se esperaba 16 bpp\n"); }
    p->maplen = p->fix.smem_len;
    p->map = mmap(NULL, p->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, p->fb, 0);
    if (p->map == MAP_FAILED) { perror("mmap fb"); close(p->fb); free(p); return -1; }
    /* mostrar el primer buffer (y=0) y pintar ahi directamente */
    p->var.xoffset = 0; p->var.yoffset = 0; ioctl(p->fb, FBIOPAN_DISPLAY, &p->var);
    nsfb->width = p->var.xres; nsfb->height = p->var.yres;
    nsfb->format = NSFB_FMT_RGB565; nsfb->bpp = 16;
    nsfb->ptr = p->map; nsfb->linelen = p->fix.line_length;
    select_plotters(nsfb);
    const char *ev[] = {"/dev/input/event1", "/dev/input/event0", "/dev/input/event2", "/dev/input/event3"};
    for (int i = 0; i < 4; i++) { int fd = open(ev[i], O_RDONLY | O_NONBLOCK); if (fd >= 0) p->in[p->nin++] = fd; }
    p->tx = p->ty = -1;
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
    for (int i = 0; i < p->nin; i++) close(p->in[i]);
    munmap(p->map, p->maplen); close(p->fb); free(p);
    nsfb->surface_priv = NULL; nsfb->ptr = NULL;
    return 0;
}

static void touch_frame(struct hiby_priv *p)
{
    if (p->down && !p->was_down) {           /* inicio de toque */
        p->sx = p->tx; p->sy = p->ty; p->lasty = p->ty; p->dragging = 0;
        qmove(p, p->tx, p->ty);
    } else if (p->down && p->was_down) {     /* movimiento */
        if (!p->dragging && (abs(p->ty - p->sy) > DRAG_THRESHOLD || abs(p->tx - p->sx) > DRAG_THRESHOLD)) p->dragging = 1;
        if (p->dragging) {
            while (p->ty - p->lasty >= SCROLL_STEP) { qkey(p, NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_4); qkey(p, NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_4); p->lasty += SCROLL_STEP; }
            while (p->lasty - p->ty >= SCROLL_STEP) { qkey(p, NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_5); qkey(p, NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_5); p->lasty -= SCROLL_STEP; }
        }
    } else if (!p->down && p->was_down) {    /* fin: click si no hubo arrastre */
        if (!p->dragging) {
            qmove(p, p->sx, p->sy);
            qkey(p, NSFB_EVENT_KEY_DOWN, NSFB_KEY_MOUSE_1);
            qkey(p, NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_1);
        }
    }
    p->was_down = p->down;
}

static void key_event(struct hiby_priv *p, int code, int value)
{
    int k = -1;
    switch (code) {
    case KEY_VOLUMEUP: k = NSFB_KEY_PAGEUP; break;
    case KEY_VOLUMEDOWN: k = NSFB_KEY_PAGEDOWN; break;
    case KEY_PREVIOUSSONG: k = NSFB_KEY_LEFT; break;   /* scroll horizontal (atras = boton de la barra) */
    case KEY_NEXTSONG: k = NSFB_KEY_RIGHT; break;
    case KEY_PLAYPAUSE: k = NSFB_KEY_ESCAPE; break;
    case KEY_POWER: if (value == 1) { nsfb_event_t e; memset(&e, 0, sizeof e); e.type = NSFB_EVENT_CONTROL; e.value.controlcode = NSFB_CONTROL_QUIT; qpush(p, e); } return;
    default: return;
    }
    if (value == 2) return;
    qkey(p, value ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, k);
}

static bool hiby_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct hiby_priv *p = nsfb->surface_priv;
    if (!p) return false;
    if (qpop(p, event)) return true;
    struct pollfd pf[4];
    for (int i = 0; i < p->nin; i++) { pf[i].fd = p->in[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
    int r = poll(pf, p->nin, timeout < 0 ? -1 : timeout);
    if (r <= 0) return false;
    for (int i = 0; i < p->nin; i++) {
        if (!(pf[i].revents & POLLIN)) continue;
        struct input_event ev[64];
        ssize_t n = read(p->in[i], ev, sizeof ev);
        for (int j = 0; j < n / (ssize_t)sizeof(struct input_event); j++) {
            struct input_event *e = &ev[j];
            if (i == 0) {
                if (e->type == EV_KEY && e->code == BTN_TOUCH) p->down = e->value;
                else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_X) p->tx = e->value;
                else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_Y) p->ty = e->value;
                else if (e->type == EV_SYN && e->code == SYN_REPORT) touch_frame(p);
            } else if (e->type == EV_KEY) key_event(p, e->code, e->value);
        }
    }
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
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    (void)box;
    /* en pantalla tactil no dibujamos puntero (opcional: NSFB_HIBY_CURSOR=1) */
    if (cursor != NULL && cursor->plotted == false && getenv("NSFB_HIBY_CURSOR")) nsfb_cursor_plot(nsfb, cursor);
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
