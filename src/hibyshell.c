/* hibyshell - gestos del sistema, animaciones y capas de hibymod sobre hiby_player.
 *
 * Funciona junto a hibymod_disp.so (dentro del reproductor, ver hm_shared.h):
 *   - captura la tactil real (EVIOCGRAB) y decide que toques son gestos del sistema;
 *     el resto se reenvia tal cual al reproductor por el socket del puente;
 *   - deslizar desde el borde inferior: la pantalla del reproductor (Now Playing) sube
 *     como una hoja que sigue al dedo; al soltar se completa o vuelve;
 *   - en Now Playing: boton de inicio (arriba a la derecha) y deslizar hacia abajo
 *     para cerrar la hoja;
 *   - flecha hacia arriba en el borde inferior mientras hay musica.
 * El reproductor nunca se toca: para navegar se le envian toques/deslizamientos sinteticos
 * mientras la pantalla esta "congelada" (modo captura), y se anima con sus propias imagenes.
 * Si este proceso muere, la libreria devuelve la tactil y la pantalla al reproductor.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include "hm_shared.h"

#define W HM_W
#define H HM_H
#define ASSETS "/usr/data/hibymod/shell"
#ifndef HIBYOS_VERSION
#define HIBYOS_VERSION "0.1.0"       /* version del shell (Ajustes > Informacion si falta "hibymod status"); -DHIBYOS_VERSION=... */
#endif

/* ------------------------------------------------------------------ utilidades */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
/* hilos de segundo plano: por debajo de la interfaz y, sobre todo, del audio del reproductor */
static void thread_nice(int n) { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), n); }
static FILE *logfp;
static void lg(const char *fmt, ...)
{
    if (!logfp) return;
    va_list a; va_start(a, fmt);
    fprintf(logfp, "%9.0f ", now_ms()); vfprintf(logfp, fmt, a); fputc('\n', logfp); fflush(logfp);
    va_end(a);
}

static struct hm_state *st;
static uint16_t *fr0, *fr1;          /* fr0: imagen del shell (privada); fr1: ultimo fotograma del reproductor */
static uint16_t *fr0_shm;            /* frames[0] compartido (lo que la libreria pone en pantalla) */
static uint16_t *fb;                 /* los dos bufers de pantalla */
static int fifo = -1, sock = -1, ts = -1, keys = -1;
static pthread_mutex_t sock_mu = PTHREAD_MUTEX_INITIALIZER;

static void cmd(char c) { if (write(fifo, &c, 1) != 1) lg("fifo: %s", strerror(errno)); }
static uint16_t *visible(void) { return fb + (size_t)st->yoffset * W; }
/* Presentacion directa: con una app externa en pausa (SIGSTOP) la pantalla esta retenida para
 * ella y la libreria no presenta nada del shell; el shell escribe entonces en la pagina del fb
 * que esta en pantalla (la app cambia de pagina por su cuenta: se pregunta al driver). */
static int fb_fd = -1, present_direct;
static uint16_t *fb_shown(void)
{
    struct fb_var_screeninfo v;
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) || v.yoffset > (unsigned)H) return fb;
    return fb + (size_t)v.yoffset * W;
}

static int dirty_y0, dirty_y1;
static int sp_on;                  /* aviso del sistema en pantalla (syspop.inc.c) */   /* presentacion parcial: solo estas filas cambiaron */
static void present(void);
static void syspop_tick(void);
static void syspop_after_present(void);
struct pkt;
static int syspop_touch(const struct pkt *p);
static void pw_frame_cap(void);
static void cc_open(int start_y);
static void cc_conf_line(char *rest);
static void lcd_boost(void);
static int hz_hold;
static void cc_conf_save(FILE *f);
static int widgets_on;
static void widgets_draw(uint16_t *d, int ox);
static void wg_conf_line(char *rest);
static void wg_conf_save(FILE *f);
static void present_rows(int y0, int y1) { dirty_y0 = y0 < 0 ? 0 : y0; dirty_y1 = y1 > H ? H : y1; present(); }
/* Perfil generico por rafaga de fotogramas (solo si existe /tmp/hibymod/perf al arrancar):
 * dibujo = tiempo entre el final de un present y el siguiente (lo que tarda el shell en
 * componer), espera = lo que present() espera al panel. Una linea por animacion/gesto. */
static int perf_on;
static struct { double last_end, t0, draw, dmax, wait, first; int n, rows; } pf;
static void perf_flush(void)
{
    /* el primer intervalo va aparte: tras el fotograma del toque incluye lo que el dedo
     * sigue apoyado y la preparacion de la animacion */
    if (pf.n >= 3) lg("perf: t0 %.0f %d fot en %.0f ms, dibujo media %.1f max %.1f ms, espera %.1f ms, filas %d, primero %.1f ms",
                      pf.t0, pf.n, pf.last_end - pf.t0, pf.n > 2 ? pf.draw / (pf.n - 2) : 0, pf.dmax, pf.wait / pf.n, pf.rows / pf.n, pf.first);
    pf.n = 0; pf.draw = pf.dmax = pf.wait = pf.first = 0; pf.rows = 0;
}
static void perf_frame(double t_in, double t_out, int rows)
{
    double d = t_in - pf.last_end;
    if (!pf.n || d > 120) { perf_flush(); pf.t0 = t_in; }
    else if (pf.n == 1) pf.first = d;
    else { pf.draw += d; if (d > pf.dmax) pf.dmax = d; }
    pf.n++; pf.wait += t_out - t_in; pf.rows += rows; pf.last_end = t_out;
}
/* presenta frames[0] y espera a que el panel lo tenga */
static void present(void)
{
    if (present_direct) {
        memcpy(fb_shown(), fr0, HM_FRAME_BYTES);
        dirty_y0 = dirty_y1 = 0; st->shell_beat++;
        msleep(12);                                  /* ritmo de unos 60 Hz */
        return;
    }
    pw_frame_cap();
    uint32_t p = st->presents; double t = now_ms();
    int prow = dirty_y1 > dirty_y0 ? dirty_y1 - dirty_y0 : H;
    st->shell_beat++;
    st->fr0_seq++;                                   /* impar: copiando */
    __sync_synchronize();
    if (dirty_y1 > dirty_y0) memcpy(fr0_shm + dirty_y0 * W, fr0 + dirty_y0 * W, (dirty_y1 - dirty_y0) * W * 2);
    else memcpy(fr0_shm, fr0, HM_FRAME_BYTES);
    st->dirty_y0 = dirty_y0; st->dirty_y1 = dirty_y1;
    __sync_synchronize();
    st->fr0_seq++;                                   /* par: lista */
    syspop_after_present();                          /* aviso del sistema encima (syspop.inc.c) */
    dirty_y0 = dirty_y1 = 0;                         /* la siguiente, completa salvo que se diga */
    cmd('P');
    while (st->presents == p && now_ms() - t < 80) msleep(1);
    if (perf_on) perf_frame(t, now_ms(), prow);
}

/* ------------------------------------------------------------------ entrada al reproductor */
static void inj(const struct input_event *e, int n)
{
    pthread_mutex_lock(&sock_mu);
    if (sock >= 0 && write(sock, e, n * sizeof *e) < 0) lg("socket: %s", strerror(errno));
    pthread_mutex_unlock(&sock_mu);
}
static int mkev(struct input_event *e, int type, int code, int value)
{
    memset(e, 0, sizeof *e); e->type = type; e->code = code; e->value = value; return 1;
}
static void inj_contact(int x, int y, int down)
{
    struct input_event e[9]; int n = 0;
    if (down) n += mkev(&e[n], EV_KEY, BTN_TOUCH, 1);
    n += mkev(&e[n], EV_ABS, ABS_MT_TOUCH_MAJOR, 22);
    n += mkev(&e[n], EV_ABS, ABS_MT_WIDTH_MAJOR, 22);
    n += mkev(&e[n], EV_ABS, ABS_MT_POSITION_X, x);
    n += mkev(&e[n], EV_ABS, ABS_MT_POSITION_Y, y);
    n += mkev(&e[n], EV_ABS, ABS_MT_TRACKING_ID, 0);
    n += mkev(&e[n], EV_SYN, SYN_MT_REPORT, 0);
    n += mkev(&e[n], EV_SYN, SYN_REPORT, 0);
    inj(e, n);
}
static void inj_up(void)
{
    struct input_event e[3]; int n = 0;
    n += mkev(&e[n], EV_KEY, BTN_TOUCH, 0);
    n += mkev(&e[n], EV_SYN, SYN_MT_REPORT, 0);
    n += mkev(&e[n], EV_SYN, SYN_REPORT, 0);
    inj(e, n);
}
static void inj_tap(int x, int y) { inj_contact(x, y, 1); msleep(40); inj_contact(x, y, 0); msleep(30); inj_up(); }
static void inj_swipe(int x1, int y1, int x2, int y2, int steps, int ms)
{
    inj_contact(x1, y1, 1);
    for (int i = 1; i <= steps; i++) { msleep(ms / steps); inj_contact(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 0); }
    msleep(15); inj_up();
}

/* ------------------------------------------------------------------ imagenes */
static inline int lum(uint16_t p) { return ((p >> 11) * 8 * 3 + ((p >> 5) & 63) * 4 * 6 + (p & 31) * 8) / 10; }
/* k 0..32: cada canal * k / 32 (truncado). Los tres canales se separan en una palabra de 32
 * bits (g arriba, r y b abajo con huecos) y se multiplican de una vez: un solo producto en
 * vez de tres, mismo resultado bit a bit. Es lo mas usado en las animaciones (fondos). */
static inline uint16_t dim565(uint16_t p, int k)
{
    uint32_t x = ((uint32_t)p | (uint32_t)p << 16) & 0x07E0F81Fu;
    x = (x * (uint32_t)k >> 5) & 0x07E0F81Fu;
    return (uint16_t)(x | x >> 16);
}
/* n pixeles de src oscurecidos en dst (k >= 32: copia; puede ser dst == src) */
static void dim_buf(uint16_t *dst, const uint16_t *src, int n, int k)
{
    if (k >= 32) { if (dst != src) memcpy(dst, src, (size_t)n * 2); return; }
    if (k <= 0) { memset(dst, 0, (size_t)n * 2); return; }
    for (int i = 0; i < n; i++) dst[i] = dim565(src[i], k);
}
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

/* firma de la pantalla de inicio (puntos x y r g b tolerancia) */
static int home_sig[8][6], home_sig_n;
static void load_home_sig(void)
{
    /* firma del espejo cargado por HiBy en este arranque (ver mirror_sig_write) */
    char bid[64] = "", line[80] = "";
    { int fb = open("/proc/sys/kernel/random/boot_id", O_RDONLY); if (fb >= 0) { int k = read(fb, bid, sizeof bid - 1); if (k > 0) bid[k] = 0; close(fb); } }
    for (char *c = bid; *c; c++) if (*c == '\n') *c = 0;
    FILE *nx = fopen(ASSETS "/mirror.next.sig", "r");
    if (nx) {
        if (fgets(line, sizeof line, nx)) { for (char *c = line; *c; c++) if (*c == '\n') *c = 0; }
        fclose(nx);
        if (strcmp(line, bid)) rename(ASSETS "/mirror.next.sig", ASSETS "/mirror.sig");   /* de un arranque anterior */
    }
    FILE *f = fopen(ASSETS "/mirror.sig", "r");
    if (f) { if (!fgets(line, sizeof line, f)) { fclose(f); f = 0; } }
    if (!f) f = fopen(ASSETS "/home.sig", "r");
    if (!f) return;
    while (home_sig_n < 8 && fscanf(f, "%d %d %d %d %d %d", &home_sig[home_sig_n][0], &home_sig[home_sig_n][1],
               &home_sig[home_sig_n][2], &home_sig[home_sig_n][3], &home_sig[home_sig_n][4], &home_sig[home_sig_n][5]) == 6)
        home_sig_n++;
    fclose(f);
}
static int is_home(const uint16_t *img)
{
    if (!home_sig_n) return 0;
    for (int i = 0; i < home_sig_n; i++) {
        uint16_t p = img[home_sig[i][1] * W + home_sig[i][0]];
        int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31, t = home_sig[i][5];
        if (abs(r - home_sig[i][2]) > t || abs(g - home_sig[i][3]) > t || abs(b - home_sig[i][4]) > t) return 0;
    }
    return 1;
}
/* la barra de estado siempre tiene el icono de bateria arriba a la derecha */
static int has_statusbar(const uint16_t *img)
{
    int bright = 0;
    for (int y = 8; y < 22; y++) for (int x = 286; x < 318; x++) if (lum(img[y * W + x]) > 150) bright++;
    return bright > 12;
}
/* Now Playing: sin barra de estado y con el boton circular blanco de reproducir/pausa */
static int is_plane(const uint16_t *img)
{
    if (has_statusbar(img)) return 0;
    int white = 0;
    static const int pts[][2] = { {140, 440}, {180, 440}, {160, 420}, {160, 460} };
    for (int i = 0; i < 4; i++) if (lum(img[pts[i][1] * W + pts[i][0]]) > 200) white++;
    return white >= 3;
}
/* centro de control: asa clara abajo en el centro y oscuro a los lados */
static int is_cc(const uint16_t *img)
{
    int c = 0, s = 0;
    for (int x = 140; x < 180; x++) if (lum(img[460 * W + x]) > 70) c++;
    for (int x = 40; x < 80; x++) if (lum(img[460 * W + x]) > 70) s++;
    return c > 25 && s < 5 && has_statusbar(img);
}

/* pagina de inicio de sesion de fabrica con su teclado T9 (Qobuz sin sesion: es la raiz de la
 * app y no tiene flecha de atras; se sale con "Cancel", que lleva a "Stream media") */
static int is_stock_login(const uint16_t *img)
{
    static const int box[][2] = { {54, 66}, {262, 66}, {54, 123}, {262, 123}, {262, 87}, {160, 236}, {8, 300}, {312, 340}, {160, 440} };   /* fondo de las casillas (sin texto) y teclas */
    for (unsigned i = 0; i < sizeof box / sizeof *box; i++) { int l = lum(img[box[i][1] * W + box[i][0]]); if (l < 15 || l > 40) return 0; }
    return lum(img[200 * W + 5]) < 8 && lum(img[165 * W + 240]) < 8 && has_statusbar(img);
}

/* ------------------------------------------------------------------ capas (sprites) */
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
static struct sprite spr_chev, spr_bar;   /* flecha de musica (inicio) y barra de inicio (apps) */
static int load_sprite(const char *name, struct sprite *s)
{
    char p[128]; snprintf(p, sizeof p, ASSETS "/%s.ov", name);
    FILE *f = fopen(p, "rb");
    if (!f) { lg("falta %s", p); return 0; }
    char mg[4]; uint16_t wh[2];
    if (fread(mg, 1, 4, f) != 4 || memcmp(mg, "HMOV", 4) || fread(wh, 2, 2, f) != 2 || wh[0] * wh[1] > HM_OV_MAX) { fclose(f); return 0; }
    s->w = wh[0]; s->h = wh[1];
    s->rgb = malloc(s->w * s->h * 2); s->a = malloc(s->w * s->h);
    int ok = fread(s->rgb, 2, s->w * s->h, f) == (size_t)(s->w * s->h) && fread(s->a, 1, s->w * s->h, f) == (size_t)(s->w * s->h);
    fclose(f);
    return ok;
}
static const struct sprite *ov_cur; static int ov_cx, ov_cy;
static void overlay(const struct sprite *s, int x, int y)
{
    if (s == ov_cur && (!s || (x == ov_cx && y == ov_cy))) return;
    if (st->capture || st->held) return;    /* se aplicara al devolver */
    st->ov_seq++; __sync_synchronize();               /* impar: la libreria espera */
    if (s && s->w) {
        memcpy(st->ov_rgb, s->rgb, s->w * s->h * 2); memcpy(st->ov_a, s->a, s->w * s->h);
        st->ov_x = x; st->ov_y = y; st->ov_w = s->w; st->ov_h = s->h;
        st->ov_on = 1;
    } else st->ov_on = 0;
    __sync_synchronize(); st->ov_seq++;
    ov_cur = s; ov_cx = x; ov_cy = y;
    lg("capa: %s", s == &spr_chev ? "flecha" : s ? "otra" : "ninguna");
    cmd('O');
}
/* en las composiciones propias (animaciones) se dibuja igual que la libreria */
static void blend_sprite(uint16_t *img, const struct sprite *s, int x, int y)
{
    if (!s || !s->w) return;
    for (int r = 0; r < s->h; r++) {
        uint16_t *row = img + (y + r) * W + x;
        for (int i = 0; i < s->w; i++) {
            unsigned a = s->a[r * s->w + i]; if (!a) continue;
            unsigned d = row[i], c = s->rgb[r * s->w + i], na = 255 - a;
            row[i] = (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
        }
    }
}

/* launcher (launcher.inc.c): declaraciones adelantadas */
static int launcher_on;
static void launcher_present(void);
static void launcher_present_hud(void);
static void home_publish(void);
static void plane_home_drag(int sy);
/* gestos configurables (gestures.inc.c) */
enum { GA_NONE, GA_HOME, GA_PLAYER, GA_CC, GA_BACK };
/* top = mitad izquierda del borde superior, top_r = mitad derecha */
static struct { int bottom_app, bottom_home, top, left, right, chevron, top_r; } G = { GA_HOME, GA_HOME, GA_CC, GA_BACK, GA_BACK, 1, GA_PLAYER };
static int last_app = -1;   /* ultima app abierta desde el launcher */
static void gestures_conf_line(char *k, char *v);
static void gestures_conf_save(FILE *f);
static void player_from_edge(int dir, int start);
static void open_cc(void);
static int launcher_resume(void);
static int open_app_nav(void);

#define CHEV_Y_PLANE 468
#define CHEV_Y 468
#define BAR_Y 469

/* ------------------------------------------------------------------ composicion de la hoja */
static uint16_t img_under[W * H], img_sheet[W * H], img_cache_plane[W * H], img_cache_home[W * H];
static int have_cache_plane, have_cache_home, have_under;

/* hoja con su borde superior en "top" sobre "under" oscurecido (k 32 = sin oscurecer) */
static void compose_sheet(uint16_t *dst, const uint16_t *under, const uint16_t *sheet, int top, int k)
{
    const int R = 14;
    if (top < 0) top = 0;
    if (top > H) top = H;
    for (int y = 0; y < top; y++) {
        const uint16_t *o = under + y * W; uint16_t *d = dst + y * W;
        if (k >= 32) memcpy(d, o, W * 2); else for (int i = 0; i < W; i++) d[i] = dim565(o[i], k);
    }
    for (int y = top; y < H; y++) {
        int r = y - top;
        uint16_t *d = dst + y * W;
        memcpy(d, sheet + r * W, W * 2);
        if (r < R && top > 0) {
            int c = R - r, cut = R - (int)(sqrtf((float)(R * R - c * c)) + 0.5f);
            const uint16_t *o = under + y * W;
            for (int i = 0; i < cut; i++) { d[i] = dim565(o[i], k); d[W - 1 - i] = dim565(o[W - 1 - i], k); }
        }
    }
    /* sombra suave alrededor del borde redondeado de la hoja (no una franja recta) */
    if (top <= 0) return;
    for (int y = top - 6 > 0 ? top - 6 : 0; y < top + R && y < H; y++) {
        uint16_t *d = dst + y * W;
        for (int i = 0; i < W; i++) {
            if (y >= top && i == R) i = W - R;               /* filas de las esquinas: solo las esquinas */
            int cx = i < R ? R : i >= W - R ? W - 1 - R : -1;
            float dist;
            if (cx < 0) dist = (float)(top - y);
            else { float dx = (float)(i - cx), dy = (float)(y - (top + R)); if (dy > 0) dy = 0; dist = sqrtf(dx * dx + dy * dy) - R; }
            if (dist > 0 && dist <= 6) d[i] = dim565(d[i], 32 - (int)((7 - dist) * 2));
        }
    }
}

/* ------------------------------------------------------------------ navegacion en segundo plano */
enum { NAV_NONE, NAV_TO_PLANE, NAV_BACK, NAV_HOME, NAV_APP };
static volatile int nav_req, nav_done, nav_ok;
static pthread_mutex_t nav_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nav_cv = PTHREAD_COND_INITIALIZER;

/* espera a que el reproductor termine de dibujar tras una accion (en captura) */
static void settle(uint32_t c0, int first_ms, int quiet_ms)
{
    double t = now_ms();
    while (st->cap_frames == c0 && now_ms() - t < first_ms) msleep(5);
    uint32_t last = st->cap_frames; double q = now_ms();
    while (now_ms() - t < first_ms + 1500) {
        msleep(8); st->shell_beat++;
        if (st->cap_frames != last) { last = st->cap_frames; q = now_ms(); }
        else if (now_ms() - q > quiet_ms) break;
    }
}
static int back_to_home(void)
{
    int still = 0;
    for (int i = 0; i < 8; i++) {
        if (is_home(fr1)) return 1;
        uint32_t c = st->cap_frames;
        if (is_stock_login(fr1)) { lg("nav: inicio de sesion de fabrica -> Cancel"); inj_tap(80, 165); }
        else if (still >= 2) { lg("nav: la flecha no responde; deslizamiento de atras de HiBy"); inj_swipe(2, 240, 230, 240, 8, 120); still = 0; }
        else inj_tap(22, 48);
        settle(c, 500, 120);
        if (st->cap_frames == c) still++; else still = 0;
    }
    return is_home(fr1);
}
static void *nav_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&nav_mu);
        while (!nav_req) pthread_cond_wait(&nav_cv, &nav_mu);
        int req = nav_req;
        pthread_mutex_unlock(&nav_mu);
        int ok = 0;
        double t0 = now_ms();
        if (req == NAV_TO_PLANE) {
            uint32_t c = st->cap_frames;
            inj_swipe(300, 240, 30, 240, 5, 60);
            settle(c, 700, 150);
            ok = is_plane(fr1);
            if (!ok) {
                lg("nav: el deslizamiento no abrio Now Playing; volviendo al inicio");
                if (back_to_home()) {
                    c = st->cap_frames;
                    inj_swipe(300, 240, 30, 240, 5, 60);
                    settle(c, 700, 150);
                    ok = is_plane(fr1);
                }
            }
        } else if (req == NAV_BACK) {
            uint32_t c = st->cap_frames;
            inj_tap(22, 48);
            settle(c, 600, 70);
            ok = st->cap_frames != c;
        } else if (req == NAV_HOME) {
            ok = back_to_home();
        } else if (req == NAV_APP) {
            ok = open_app_nav();
        }
        lg("nav %d -> %s en %.0f ms", req, ok ? "ok" : "FALLO", now_ms() - t0);
        pthread_mutex_lock(&nav_mu);
        nav_ok = ok; nav_done = 1; nav_req = 0;
        pthread_cond_broadcast(&nav_cv);
        pthread_mutex_unlock(&nav_mu);
    }
    return 0;
}
static void nav_start(int req)
{
    pthread_mutex_lock(&nav_mu);
    nav_done = 0; nav_ok = 0; nav_req = req;
    pthread_cond_broadcast(&nav_cv);
    pthread_mutex_unlock(&nav_mu);
}
static int nav_wait(int ms)
{
    double t = now_ms();
    while (!nav_done && now_ms() - t < ms) { st->shell_beat++; msleep(5); }
    return nav_done ? nav_ok : -1;
}

/* ------------------------------------------------------------------ tactil real */
struct pkt { struct input_event ev[24]; int n; int x, y, down; };
static int cur_x, cur_y, cur_down;
static unsigned touch_drops;          /* veces que el nucleo descarto eventos (SYN_DROPPED) */
/* tras SYN_DROPPED: el estado real de la tactil (dedo y posicion) leido del nucleo */
static void touch_resync(void)
{
    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1]; memset(keys, 0, sizeof keys);
    if (ioctl(ts, EVIOCGKEY(sizeof keys), keys) >= 0)
        cur_down = (keys[BTN_TOUCH / (8 * sizeof(long))] >> (BTN_TOUCH % (8 * sizeof(long)))) & 1;
    struct input_absinfo ai;
    if (!ioctl(ts, EVIOCGABS(ABS_MT_POSITION_X), &ai)) cur_x = ai.value;
    if (!ioctl(ts, EVIOCGABS(ABS_MT_POSITION_Y), &ai)) cur_y = ai.value;
}
static int read_pkt(struct pkt *p, int timeout_ms)
{
    p->n = 0;
    for (;;) {
        struct pollfd pf = { ts, POLLIN, 0 };
        int left = p->n ? 50 : timeout_ms, r;
        do {   /* en tramos cortos para mantener el latido (y mover los avisos del sistema) */
            int lim = sp_on ? 16 : 250, slice = left > lim ? lim : left;
            st->shell_beat++;
            r = poll(&pf, 1, slice);
            left -= slice;
            if (r == 0 && timeout_ms > 0) syspop_tick();
        } while (r == 0 && left > 0);
        if (r <= 0) return 0;
        struct input_event e;
        if (read(ts, &e, sizeof e) != sizeof e) return 0;
        if (e.type == EV_SYN && e.code == SYN_DROPPED) {
            /* el nucleo perdio eventos: descartar hasta el siguiente SYN_REPORT y releer el estado;
             * el paquete que se devuelve lleva ese estado (con la pulsacion si el dedo sigue) */
            struct input_event d;
            while (read(ts, &d, sizeof d) == sizeof d && !(d.type == EV_SYN && d.code == SYN_REPORT)) {}
            int was = cur_down; touch_resync(); touch_drops++;
            lg("tactil: eventos perdidos (%u), dedo %d en %d,%d", touch_drops, cur_down, cur_x, cur_y);
            int n = 0;
            if (cur_down != was) n += mkev(&p->ev[n], EV_KEY, BTN_TOUCH, cur_down);
            if (cur_down) {
                n += mkev(&p->ev[n], EV_ABS, ABS_MT_TOUCH_MAJOR, 22);
                n += mkev(&p->ev[n], EV_ABS, ABS_MT_WIDTH_MAJOR, 22);
                n += mkev(&p->ev[n], EV_ABS, ABS_MT_POSITION_X, cur_x);
                n += mkev(&p->ev[n], EV_ABS, ABS_MT_POSITION_Y, cur_y);
                n += mkev(&p->ev[n], EV_ABS, ABS_MT_TRACKING_ID, 0);
            }
            n += mkev(&p->ev[n], EV_SYN, SYN_MT_REPORT, 0);
            n += mkev(&p->ev[n], EV_SYN, SYN_REPORT, 0);
            p->n = n; p->x = cur_x; p->y = cur_y; p->down = cur_down;
            return 1;
        }
        if (p->n < 24) p->ev[p->n++] = e;
        if (p->n == 1) lcd_boost();                /* refresco completo antes del primer fotograma */
        if (e.type == EV_KEY && e.code == BTN_TOUCH) cur_down = e.value;
        else if (e.type == EV_ABS && e.code == ABS_MT_POSITION_X) cur_x = e.value;
        else if (e.type == EV_ABS && e.code == ABS_MT_POSITION_Y) cur_y = e.value;
        else if (e.type == EV_SYN && e.code == SYN_REPORT) {
            p->x = cur_x; p->y = cur_y; p->down = cur_down;
            if (sp_on && syspop_touch(p)) { p->n = 0; return 0; }      /* el toque era del aviso */
            return 1;
        }
    }
}
static void fwd(const struct pkt *p) { inj(p->ev, p->n); }
/* misma pagina: menos del 3 % de muestras distintas por debajo de la barra de estado */
static int same_page(const uint16_t *a, const uint16_t *b)
{
    int diff = 0, n = 0;
    for (int y = 34; y < H; y += 4) for (int x = 2; x < W; x += 4) { n++; if (abs(lum(a[y * W + x]) - lum(b[y * W + x])) > 6) diff++; }
    return diff * 100 < n * 3;
}

/* descarta toques hasta que se levante el dedo (tras una animacion) */
static void drain_until_up(void)
{
    struct pkt p;
    while (cur_down && read_pkt(&p, 3000)) {}
}

/* ------------------------------------------------------------------ hoja de Now Playing */
static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

/* anima el borde de la hoja de "from" a "to" (px) con oscurecimiento acorde */
static void animate_sheet(const uint16_t *under, const uint16_t *sheet, int from, int to, int ms, int live)
{
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1;
        int top = from + (int)((to - from) * ease_out(t));
        int k = 32 - (11 * (H - top)) / H;
        compose_sheet(fr0, under, live ? fr1 : sheet, top, k);
        present();
        if (t >= 1) break;
    }
}

static int sheet_open_drag(int start_y)
{
    double t0 = now_ms();
    memcpy(img_under, visible(), sizeof img_under);
    if (st->ov_saved) {   /* quitar la flecha de la imagen de fondo */
        /* la libreria guarda lo que tapa la capa: restaurarlo en nuestra copia */
        for (uint32_t r = 0; r < st->ov_uh; r++)
            memcpy(img_under + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    }
    have_under = 1;
    st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double tc = now_ms();
    while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) { lg("captura no disponible"); return 0; }
    nav_start(NAV_TO_PLANE);
    const uint16_t *placeholder = have_cache_plane ? img_cache_plane : 0;
    if (!placeholder) { for (int i = 0; i < W * H; i++) img_sheet[i] = rgb(18, 18, 20); placeholder = img_sheet; }
    struct pkt p; int top = H, lasty = start_y; double lastt = now_ms(); double vel = 0;
    /* la hoja sigue al dedo */
    for (;;) {
        int got = read_pkt(&p, 0);
        while (got && p.down && read_pkt(&p, 0)) {}
        if (got) {
            if (!p.down) break;
            double t = now_ms();
            if (t > lastt) vel = 0.6 * vel + 0.4 * (p.y - lasty) / (t - lastt);
            lasty = p.y; lastt = t;
        }
        int want = lasty - 18; if (want < 0) want = 0;
        top = want;
        int ready = nav_done && nav_ok;
        int k = 32 - (11 * (H - top)) / H;
        compose_sheet(fr0, img_under, ready ? fr1 : placeholder, top, k);
        present();
        if (!cur_down) break;
    }
    int complete = vel < -0.25 || top < H * 0.55;
    lg("hoja: suelta en %d, vel %.2f -> %s", top, vel, complete ? "abrir" : "cancelar");
    if (complete) {
        int dur = 180 + top / 3;
        double ta = now_ms();
        for (;;) {   /* sube con lo que haya; si llega Now Playing a mitad, cambia a la imagen real */
            double t = (now_ms() - ta) / dur; if (t > 1) t = 1;
            int tt = top - (int)(top * ease_out(t));
            int ready = nav_done && nav_ok;
            compose_sheet(fr0, img_under, ready ? fr1 : placeholder, tt, 32 - (11 * (H - tt)) / H);
            present();
            if (t >= 1) break;
        }
        int ok = nav_wait(2500);
        if (ok == 1) {
            memcpy(img_cache_plane, fr1, sizeof img_cache_plane); have_cache_plane = 1;
            if (launcher_on) { memcpy(img_cache_home, img_under, sizeof img_cache_home); have_cache_home = 1; launcher_on = 0; home_publish(); }
            cmd('R');
            lg("Now Playing abierto en %.0f ms", now_ms() - t0);
            return 1;
        }
        /* no hay musica cargada o no se pudo: la hoja vuelve a bajar */
        animate_sheet(img_under, placeholder, 0, H, 260, 0);
        if (ok == -1) nav_wait(3000);
        if (!is_home(fr1) && is_plane(fr1)) { uint32_t c = st->cap_frames; inj_tap(22, 48); settle(c, 500, 100); }
        if (!launcher_resume()) { memcpy(fr1, img_under, sizeof img_under); cmd('R'); }
        return 0;
    }
    animate_sheet(img_under, placeholder, top, H, 200, 0);
    int ok = nav_wait(3000);
    if (ok == 1) {   /* el reproductor ya habia llegado a Now Playing: volver atras sin que se vea */
        memcpy(img_cache_plane, fr1, sizeof img_cache_plane); have_cache_plane = 1;
        nav_start(NAV_BACK); nav_wait(2000);
    }
    if (!launcher_resume()) cmd('R');
    return 0;
}

/* cerrar Now Playing (hacia abajo); home = ir al inicio en vez de a la pantalla anterior */
static void sheet_close(int interactive, int home)
{
    memcpy(img_sheet, visible(), sizeof img_sheet);
    if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)
        memcpy(img_sheet + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    memcpy(img_cache_plane, img_sheet, sizeof img_sheet); have_cache_plane = 1;
    /* debajo: la pantalla desde la que se abrio con el gesto; si no se conoce, el inicio */
    if (!have_cache_home) for (int i = 0; i < W * H; i++) img_cache_home[i] = 0;
    const uint16_t *under = (!home && have_under) ? img_under : img_cache_home;
    st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double tc = now_ms();
    while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return;
    nav_start(home ? NAV_HOME : NAV_BACK);
    int top = 0;
    if (interactive) {
        struct pkt p; int y0 = cur_y, lasty = cur_y; double lastt = now_ms(), vel = 0;
        for (;;) {
            int got = read_pkt(&p, 0);
            while (got && p.down && read_pkt(&p, 0)) {}
            if (got) {
                if (!p.down) break;
                double t = now_ms();
                if (t > lastt) vel = 0.6 * vel + 0.4 * (p.y - lasty) / (t - lastt);
                lasty = p.y; lastt = t;
            }
            top = lasty - y0; if (top < 0) top = 0;
            compose_sheet(fr0, under, img_sheet, top, 32 - (11 * (H - top)) / H);
            present();
            if (!cur_down) break;
        }
        if (!(vel > 0.25 || top > 140)) {          /* no llega: la hoja vuelve arriba */
            /* el reproductor ya esta volviendo atras: hay que deshacerlo */
            animate_sheet(under, img_sheet, top, 0, 180, 0);
            nav_wait(2500);
            uint32_t c = st->cap_frames;
            inj_swipe(300, 240, 30, 240, 5, 60);
            settle(c, 700, 150);
            if (!is_plane(fr1)) memcpy(fr1, img_sheet, sizeof img_sheet);
            cmd('R');
            return;
        }
    }
    double ta = now_ms(); int dur = 230;
    for (;;) {
        double t = (now_ms() - ta) / dur; if (t > 1) t = 1;
        int tt = top + (int)((H - top) * ease_out(t));
        /* si la pantalla de destino ya esta lista, se ve la real debajo */
        const uint16_t *u = (nav_done && nav_ok) ? fr1 : under;
        compose_sheet(fr0, u, img_sheet, tt, 32 - (11 * (H - tt)) / H);
        present();
        if (t >= 1) break;
    }
    nav_wait(3000);
    if (!launcher_resume()) cmd('R');
}

/* ------------------------------------------------------------------ estado de la musica */
static int player_pid;
static int music_session(void)
{
    if (!player_pid) {
        DIR *d = opendir("/proc"); struct dirent *e;
        while (d && (e = readdir(d))) {
            /* el reproductor cambia su nombre de proceso: se busca por la linea de ordenes */
            char p[64], c[128] = ""; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
            FILE *f = fopen(p, "r"); if (!f) continue;
            size_t n = fread(c, 1, sizeof c - 1, f); c[n] = 0;
            fclose(f);
            const char *b = strrchr(c, '/'); b = b ? b + 1 : c;
            if (!strcmp(b, "hiby_player")) { player_pid = atoi(e->d_name); break; }
        }
        if (d) closedir(d);
    }
    char dir[48]; snprintf(dir, sizeof dir, "/proc/%d/fd", player_pid);
    /* cada 700 ms: primero el descriptor que tenia el pcm la ultima vez (un readlink) en vez
     * de recorrer los ~100 descriptores del reproductor */
    static int pcm_fd = -1, pcm_pid;
    if (pcm_fd >= 0 && pcm_pid == player_pid) {
        char p[64], l[64]; snprintf(p, sizeof p, "%s/%d", dir, pcm_fd);
        ssize_t n = readlink(p, l, sizeof l - 1);
        if (n > 0) { l[n] = 0; if (!strncmp(l, "/dev/snd/pcm", 12)) return 1; }
        pcm_fd = -1;
    }
    DIR *d = opendir(dir); if (!d) { player_pid = 0; return 0; }
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) {
        char p[96], l[64]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        ssize_t n = readlink(p, l, sizeof l - 1);
        if (n > 0) { l[n] = 0; if (!strncmp(l, "/dev/snd/pcm", 12)) { found = 1; pcm_fd = atoi(e->d_name); pcm_pid = player_pid; } }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------ aviso de volumen (prototipo) */
/* Pildora de cristal vertical en el borde derecho, nivel blanco desde abajo; aparece
 * ensanchandose (12 -> 16 px), el nivel se anima y se desvanece a los 1,4 s sin teclas. */
#define HUD_X 289
#define HUD_Y 113
#define HUD_W 28
#define HUD_H 160
#define HUD_PILL_H 150
static uint16_t hud_rgb[HUD_W * HUD_H];
static uint8_t hud_a[HUD_W * HUD_H];
static struct sprite spr_hud = { HUD_W, HUD_H, hud_rgb, hud_a };
static double hud_last_key = -1e9, hud_shown_at = -1e9;
static int hud_active;
static double hud_level = -1;            /* nivel dibujado (0..1), se acerca al real */
static int vol_fd = -1;
static int ocr_volume(const uint16_t *img);
static int read_volume(void)
{
    /* la cifra de la barra de estado del ultimo fotograma del reproductor (en vivo).
     * user.ini va con retraso: solo se usa al arrancar si aun no hay ninguna lectura. */
    static int last_ok = -1;
    int v = ocr_volume(st->capture ? fr1 : visible());
    if (v >= 0) { if (v != last_ok) lg("volumen leido: %d", v); last_ok = v; return v; }
    if (last_ok >= 0) return -1;
    if (vol_fd < 0) vol_fd = open("/usr/data/user.ini", O_RDONLY);
    int32_t u = -1;
    if (vol_fd < 0 || pread(vol_fd, &u, 4, 16) != 4) return -1;
    return (u >= 0 && u <= 100) ? u : -1;
}
static int vol_guess = 30;
static double hud_bump;
static void hud_draw(double vis, double width, double level)
{
    memset(hud_a, 0, sizeof hud_a);
    double right = HUD_W - 3, left = right - width, top = (HUD_H - HUD_PILL_H) / 2.0, bot = top + HUD_PILL_H;
    double r = width / 2, fill_top = bot - HUD_PILL_H * level;
    for (int y = 0; y < HUD_H; y++) {
        for (int x = 0; x < HUD_W; x++) {
            /* distancia con signo a la pildora (rectangulo redondeado), con suavizado */
            double cx = x + 0.5, cy = y + 0.5;
            double qx = fabs(cx - (left + right) / 2) - (width / 2 - r);
            double qy = fabs(cy - (top + bot) / 2) - (HUD_PILL_H / 2.0 - r);
            double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            double d = sqrt(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
            double cov = 0.5 - d; if (cov < 0) cov = 0; if (cov > 1) cov = 1;
            /* sombra exterior suave */
            double sh = 0; if (d > 0 && d < 3) sh = (1 - d / 3) * 0.35;
            int i = y * HUD_W + x;
            if (cov <= 0) {
                if (sh > 0) { hud_rgb[i] = 0; hud_a[i] = (uint8_t)(sh * 255 * vis); }
                continue;
            }
            double f = cy - fill_top; if (f < 0) f = 0; if (f > 1) f = 1;    /* parte llena */
            /* cristal: gris translucido con borde claro; relleno blanco */
            int edge = d > -1.2;
            int gr = edge ? 150 : 92, ga = edge ? 150 : 165;
            int rr = (int)(gr + (255 - gr) * f), aa = (int)(ga + (250 - ga) * f);
            hud_rgb[i] = rgb(rr, rr, rr + (f < 1 ? 6 : 0) > 255 ? 255 : rr + (f < 1 ? 6 : 0));
            hud_a[i] = (uint8_t)(aa * cov * vis);
        }
    }
    /* altavoz abajo, dentro de la pildora (oscuro sobre el relleno, claro sobre el cristal) */
    if (width > 13) {
        double cx = (left + right) / 2, cy = bot - 11;
        for (int y = (int)cy - 5; y <= (int)cy + 5; y++) for (int x = (int)cx - 5; x <= (int)cx + 5; x++) {
            if (x < 0 || x >= HUD_W || y < 0 || y >= HUD_H) continue;
            double px = x + 0.5 - cx, py = y + 0.5 - cy, on = 0;
            if (px >= -4.5 && px <= -2 && fabs(py) <= 1.6) on = 1;                       /* cuerpo */
            if (px > -2 && px <= 0.8 && fabs(py) <= 1.6 + (px + 2) * 1.2) on = 1;        /* cono */
            if (level > 0.01) { double rr2 = sqrt((px - 0.5) * (px - 0.5) + py * py); if (px > 1.8 && rr2 > 2.6 && rr2 < 3.8 && fabs(py) < px * 1.3) on = 1; }
            if (!on) continue;
            int i = y * HUD_W + x; int filled = y + 0.5 > fill_top;
            hud_rgb[i] = filled ? rgb(70, 70, 76) : 0xFFFF;
            hud_a[i] = (uint8_t)(235 * vis);
        }
    }
}
static void hud_upload(void)
{
    if (st->capture || st->held) return;
    st->ov_seq++; __sync_synchronize();
    memcpy(st->ov_rgb, hud_rgb, sizeof hud_rgb); memcpy(st->ov_a, hud_a, sizeof hud_a);
    st->ov_x = HUD_X; st->ov_y = HUD_Y; st->ov_w = HUD_W; st->ov_h = HUD_H;
    st->ov_on = 1;
    __sync_synchronize(); st->ov_seq++;
    ov_cur = &spr_hud; ov_cx = HUD_X; ov_cy = HUD_Y;
    uint32_t p = st->presents; double t = now_ms();
    cmd('O');
    while (st->presents == p && now_ms() - t < 60) msleep(1);   /* al ritmo del panel */
}
static void update_overlay(void);
static void hud_key(int code)
{
    double t = now_ms();
    if (!hud_active) { hud_shown_at = t; hud_active = 1; int v = read_volume(); if (v >= 0) vol_guess = v; hud_level = vol_guess / 100.0; }
    hud_last_key = t; lcd_boost();
    hud_bump = 1;                                   /* la pildora se ensancha un instante (iOS) */
    /* sin barra de estado (Now Playing): se sigue el paso de la rueda */
    if (ocr_volume(st->capture ? fr1 : visible()) < 0) { vol_guess += code == KEY_RIGHT ? 1 : -1; if (vol_guess < 0) vol_guess = 0; if (vol_guess > 100) vol_guess = 100; }
    lg("volumen: tecla %d", code);
}
/* un paso de animacion; devuelve 1 mientras siga activo */
static int hud_step(void)
{
    if (!hud_active) return 0;
    double t = now_ms();
    int v = read_volume(); if (v >= 0) vol_guess = v;
    double target = vol_guess / 100.0;
    /* muelle casi critico: llega rapido y sin saltos; el ensanchamiento se relaja */
    static double vel, lt; double dt = lt ? (t - lt) / 16.7 : 1; lt = t; if (dt > 3) dt = 3;
    vel += ((target - hud_level) * 0.30 - vel * 0.55) * dt; hud_level += vel * dt;
    if (fabs(target - hud_level) < 0.003 && fabs(vel) < 0.002) { hud_level = target; vel = 0; }
    if (hud_level < 0) hud_level = 0; if (hud_level > 1) hud_level = 1;
    hud_bump *= pow(0.86, dt); if (hud_bump < 0.01) hud_bump = 0;
    double in = (t - hud_shown_at) / 220.0; if (in > 1) in = 1;
    double idle = t - hud_last_key;
    double out = idle < 1400 ? 1 : 1 - (idle - 1400) / 300.0;
    if (out <= 0) {
        hud_active = 0;
        if (launcher_on) { launcher_present_hud(); return 0; }
        ov_cur = &spr_hud;                 /* forzar: la capa siguiente sustituye al aviso de una vez */
        update_overlay();
        if (ov_cur == &spr_hud) overlay(0, 0, 0);
        return 0;
    }
    double e = ease_out(in);
    double o = out < 1 ? out : 1;
    hud_draw(e * out, (12 + 5 * e + 5 * hud_bump) * (0.7 + 0.3 * o), hud_level);
    if (launcher_on) launcher_present_hud(); else hud_upload();
    return 1;
}

/* decide la capa segun lo que se ve */
static int music_on;
static void update_overlay(void)
{
    if (st->capture || st->held || hud_active || launcher_on) return;
    /* mirar la imagen visible sin la capa */
    const uint16_t *v = visible();
    static int seen_home;
    if (!seen_home) {
        struct timespec up; clock_gettime(CLOCK_BOOTTIME, &up);
        if (launcher_on || is_home(v) || up.tv_sec > 120) seen_home = 1;
        else { overlay(0, 0, 0); return; }                  /* arranque: nada encima de la animacion */
    }
    int plane = is_plane(v), cc = is_cc(v);
    /* barra de inicio de iOS en cualquier app (y en Now Playing): deslizar desde ella = inicio */
    if (plane || (!cc && G.bottom_app == GA_HOME)) overlay(&spr_bar, (W - spr_bar.w) / 2, BAR_Y);
    else if (music_on && !cc && G.chevron && G.bottom_app == GA_PLAYER) overlay(&spr_chev, (W - spr_chev.w) / 2, CHEV_Y);
    else overlay(0, 0, 0);
    if (!plane && !cc && is_home(v)) { memcpy(img_cache_home, v, sizeof img_cache_home); have_cache_home = 1; }
}

/* ------------------------------------------------------------------ launcher */
static void draw_toast(uint16_t *dst);
static int back_zone(int sx, int sy);
#define G_TOP 48        /* zona de arriba (centro de control / reproductor): el dedo real llega a y 34-39 */
static int np_card(uint16_t *d);
static int np_app_run(const uint16_t *home, int ix, int iy);
static int own_home_gesture(int sx, int sy, int self);
static uint16_t *own_card; static int own_card_app;   /* captura de la ultima app propia (nowui.inc.c) */
static int app_back_gesture(int sx, int sy);
#include "i18n.inc.c"
#include "text.inc.c"
#include "launcher.inc.c"
#include "launcher_touch.inc.c"
#include "settings.inc.c"
#include "keyboard.inc.c"
#include "tageditor.inc.c"
#include "nowplaying.inc.c"
#include "playctl.inc.c"
#include "nowplay.inc.c"
#include "files.inc.c"
#include "gestures.inc.c"
#include "power.inc.c"
#include "settings_app.inc.c"
#include "cc.inc.c"
#include "ota.inc.c"
#include "lyrics.inc.c"
#include "music.inc.c"
#include "nowui.inc.c"
#include "widgets.inc.c"
#include "hpk.h"
#include "hpk.inc.c"
#include "store.inc.c"
#include "books.inc.c"
#include "wireless.inc.c"
#include "freemusic.inc.c"
#include "radio.inc.c"                 /* app Radio (emisoras por internet; ayudante hibynet radio) */
#include "buds.inc.c"                  /* app Auriculares (headphones; daemon hibybudsd) */
#include "ccbt.inc.c"                  /* modulo de Bluetooth del centro de control */
#include "ccair.inc.c"                 /* modo avion y control de ruido del centro de control */
#include "syspop.inc.c"                /* avisos del sistema sobre cualquier pantalla */

/* ------------------------------------------------------------------ volumen de la barra */
/* Lee la cifra del volumen de la barra de estado del reproductor (fondo negro): cada
 * digito se compara con los de la fuente del sistema a 16 px (la misma que usa HiBy). */
static float ocr_fg(uint16_t p)
{
    int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3, m = r > g ? r : g; if (b > m) m = b;
    return m / 255.f;
}
#include "digits.h"
/* Plantillas binarias reales de HiBy (digits.h); cada digito de la barra (fondo negro) se
 * compara con desplazamientos de +-1 px y gana el de menos pixeles distintos. */
static int ocr_on(const uint16_t *img, int x, int y) { return ocr_fg(img[y * W + x]) > 0.55f; }
/* cifras del volumen en la barra de HiByOS (theme/topbar.py: topbar_nv_speaker en x=144) */
#define VOL_X0 TOPBAR_VOL_X0
#define VOL_X1 TOPBAR_VOL_X1
static int ocr_volume(const uint16_t *img)
{
    if (!img || !has_statusbar(img)) return -1;
    int x = VOL_X0, val = 0, nd = 0;
    while (x < VOL_X1 && nd < 3) {
        while (x < VOL_X1) { int any = 0; for (int y = 4; y < 25; y++) if (ocr_on(img, x, y)) { any = 1; break; } if (any) break; x++; }
        if (x >= VOL_X1) break;
        int s = x;
        for (;;) { int any = 0; for (int y = 4; y < 25; y++) if (ocr_on(img, x, y)) { any = 1; break; } if (!any || x >= VOL_X1 + 6) break; x++; }
        int e = x, top = 99, bot = -1;
        for (int y = 4; y < 25; y++) for (int xx = s; xx < e; xx++) if (ocr_on(img, xx, y)) { if (y < top) top = y; if (y > bot) bot = y; }
        int hgt = bot - top + 1;
        if (hgt < 10 || hgt > 14 || e - s > 11) break;                 /* icono, no digito */
        int best = -1; float be = 1e9f;
        for (int d = 0; d <= 9; d++) {
            const struct dtpl *T = &DIGITS[d];
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int bw = T->w > e - s ? T->w : e - s, bh = T->h > hgt ? T->h : hgt, diff = 0;
                for (int yy = 0; yy < bh; yy++) for (int xx = 0; xx < bw; xx++) {
                    int tv = (xx < T->w && yy < T->h) ? T->rows[yy][xx] == '#' : 0;
                    int px = s + xx + dx, py = top + yy + dy;
                    int iv = (px >= 0 && px < W && py >= 0 && py < H) ? ocr_on(img, px, py) : 0;
                    diff += tv != iv;
                }
                float sc = diff / (float)(bw * bh);
                if (sc < be) { be = sc; best = d; }
            }
        }
        if (best < 0 || be > 0.22f) break;
        val = val * 10 + best; nd++;
    }
    return nd && val <= 100 ? val : -1;
}

/* ------------------------------------------------------------------ Now Playing -> inicio */
/* Desde la flecha inferior de Now Playing, hacia arriba: la tarjeta se encoge siguiendo al
 * dedo (como cerrar una app en iOS) y deja ver el inicio; al soltar se completa o vuelve. */
static void plane_frame(const uint16_t *home_dim, const uint16_t *plane, double p)
{
    if (p < 0) p = 0; if (p > 1) p = 1;
    double s = 1 - 0.62 * p;
    int w = (int)(W * s), h = (int)(H * s);
    int x = (W - w) / 2, y = (int)((H - h) * 0.42 - 40 * p);
    memcpy(fr0, home_dim, HM_FRAME_BYTES);
    draw_scaled(fr0, plane, x, y, w, h, (float)(28 * p + (p > 0 ? 6 : 0)));
}
static void plane_home_drag(int sy)
{
    uint16_t *plane = img_sheet, *home = img_base, *home_dim = img_app;
    memcpy(plane, visible(), HM_FRAME_BYTES);
    if (st->ov_saved) for (uint32_t r = 0; r < st->ov_uh; r++)        /* sin la flecha */
        memcpy(plane + (st->ov_uy + r) * W + st->ov_ux, st->ov_under + r * st->ov_uw, st->ov_uw * 2);
    if (launcher_enabled) render_launcher(home, now_ms());
    else if (have_cache_home) memcpy(home, img_cache_home, HM_FRAME_BYTES);
    else memset(home, 0, HM_FRAME_BYTES);
    dim_buf(home_dim, home, W * H, 22);
    have_under = 0;
    cmd('C');
    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return;
    ov_cur = 0;
    struct pkt p; double vel = 0, lt = now_ms(); int ly = sy; double prog = 0;
    for (;;) {
        int got = read_pkt(&p, 0);
        while (got && p.down && read_pkt(&p, 0)) {}
        if (got) {
            if (!p.down) break;
            double t = now_ms(); if (t > lt) vel = 0.6 * vel + 0.4 * (p.y - ly) / (t - lt);
            ly = p.y; lt = t;
        }
        prog = (sy - ly) / 340.0;
        plane_frame(home_dim, plane, prog);
        present();
        if (!cur_down) break;
    }
    if (prog > 0.22 || vel < -0.45) {
        lg("Now Playing -> inicio");
        nav_start(NAV_HOME);
        double t0 = now_ms(), p0 = prog < 0 ? 0 : prog;
        for (;;) {   /* la tarjeta se va hacia el centro y el inicio recupera su brillo */
            double t = (now_ms() - t0) / 240.0; if (t > 1) t = 1;
            double e = ease_out(t), pr = p0 + (1.0 - p0) * e;
            int k = 22 + (int)(10 * e);
            dim_buf(fr0, home, W * H, k);
            if (t < 1) {
                double s = (1 - 0.62 * pr) * (1 - 0.7 * e);      /* encoge mas al final */
                int w = (int)(W * s), h = (int)(H * s);
                int x = (W - w) / 2, y = (int)((H - h) * 0.42 - 40 * pr);
                draw_scaled(fr0, plane, x, y, w, h, (float)(34 * pr));
            }
            present();
            if (t >= 1) break;
        }
        memcpy(fr0, home, HM_FRAME_BYTES); present();
        nav_wait(3000);
        if (!launcher_resume()) cmd('R');
        return;
    }
    double t0 = now_ms(), p0 = prog;   /* no llega: vuelve con un pequeno rebote */
    for (;;) {
        double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1;
        double e = 1 - (1 - t) * (1 - t) * (1 - t);
        plane_frame(home_dim, plane, p0 * (1 - e));
        present();
        if (t >= 1) break;
    }
    cmd('R');
}

static int launcher_resume(void)
{
    if (!launcher_enabled || !is_home(fr1)) { launcher_on = 0; return 0; }
    launcher_on = 1; widgets_on = 0;
    V.scroll = cur_page * W; V.pressed_app = -1; V.drag_app = -1; V.edit = 0;
    launcher_present();
    lg("launcher de vuelta");
    return 1;
}

/* ------------------------------------------------------------------ principal */
static void *map_file(const char *path, size_t size, int create)
{
    int f = open(path, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (f < 0) return 0;
    if (create && ftruncate(f, size)) { close(f); return 0; }
    void *m = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    close(f);
    return m == MAP_FAILED ? 0 : m;
}

static int open_touch(void)
{
    for (int i = 0; i < 16; i++) {
        char p[32], name[64] = ""; snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDONLY);
        if (f < 0) continue;
        ioctl(f, EVIOCGNAME(sizeof name), name);
        struct input_id id; ioctl(f, EVIOCGID, &id);
        if (!strcmp(name, "goodix-ts") && id.bustype != BUS_VIRTUAL) return f;
        close(f);
    }
    return -1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (argc > 1 && !strcmp(argv[1], "--abdl")) return ab_worker_main();   /* descargas de Libros (books_ab.inc.c) */
    signal(SIGPIPE, SIG_IGN);
    logfp = fopen("/tmp/hibymod/shell.log", "a");
    /* Sobrecompromiso permitido (lo mismo que hibymod_tune.sh del firmware, para quien aun no lo
     * tenga): en modo heuristico el nucleo rechaza fork() de procesos con pilas de hilos grandes
     * (adbd, este shell) en cuanto quedan pocos MB libres: "adb shell" -> "error: closed" y los
     * ayudantes (hibywall, hibynet, hibytag) no arrancan. */
    { int f = open("/proc/sys/vm/overcommit_memory", O_WRONLY); if (f >= 0) { if (write(f, "1", 1) != 1) lg("overcommit: no se pudo"); close(f); } }
    perf_on = !access("/tmp/hibymod/perf", F_OK);
    for (int i = 0; i < 300 && !(st = map_file(HM_STATE, HM_STATE_SIZE, 0)); i++) msleep(200);
    if (!st) { lg("sin estado compartido"); return 1; }
    for (int i = 0; i < 300 && st->magic != HM_MAGIC; i++) msleep(200);
    if (st->magic != HM_MAGIC || st->input_mode == 0) { lg("libreria no activa (magic %x, entrada %u)", st->magic, st->input_mode); return 1; }
    uint16_t *fr = map_file(HM_FRAMES, 2 * HM_FRAME_BYTES, 1);
    if (!fr) { lg("sin frames"); return 1; }
    fr0_shm = fr; fr1 = fr + W * H;
    fr0 = malloc(HM_FRAME_BYTES);
    if (!fr0) { lg("sin memoria"); return 1; }
    memcpy(fr0, fr0_shm, HM_FRAME_BYTES);
    int fbf = open("/dev/fb0", O_RDWR);
    struct fb_fix_screeninfo x; ioctl(fbf, FBIOGET_FSCREENINFO, &x);
    fb = mmap(0, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbf, 0);
    fb_fd = fbf;
    if (fb == MAP_FAILED) { lg("sin fb"); return 1; }
    fifo = open(HM_FIFO, O_WRONLY);
    if (fifo < 0) { lg("sin fifo"); return 1; }
    load_home_sig();
    load_sprite("chevron", &spr_chev);
    load_sprite("home_bar", &spr_bar);

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, HM_SOCK);
    if (connect(sock, (struct sockaddr *)&a, sizeof a)) { lg("socket: %s", strerror(errno)); return 1; }
    ts = open_touch();
    /* los procesos que lance el shell (hibynet, hibymix, el demonio de auriculares...) no deben
     * heredar la tactil: el "grab" es del fichero abierto y un hijo que sobreviva al shell la
     * dejaria capturada (el shell siguiente no podria leerla) */
    { int fds[] = { ts, fbf, fifo, sock }; for (unsigned i = 0; i < 4; i++) if (fds[i] >= 0) fcntl(fds[i], F_SETFD, FD_CLOEXEC); }
    if (ts < 0 || ioctl(ts, EVIOCGRAB, 1)) { lg("no se pudo capturar la tactil"); return 1; }
    keys = -1;
    for (int i = 0; i < 16 && keys < 0; i++) {
        char pth[32], name[64] = ""; snprintf(pth, sizeof pth, "/dev/input/event%d", i);
        int f = open(pth, O_RDONLY | O_NONBLOCK);
        if (f < 0) continue;
        ioctl(f, EVIOCGNAME(sizeof name), name);
        if (!strcmp(name, "sa-ring-keys")) { keys = f; fcntl(f, F_SETFD, FD_CLOEXEC); } else close(f);
    }
    pthread_t th; pthread_create(&th, 0, nav_thread, 0);
    text_init();
    register_installed_apps();
    {   /* animacion de arranque: pedir el final, esperar al fundido (<= 1,5 s) y soltar al reproductor */
        int f = open("/tmp/hibyos_boot_stop", O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f);
        double tb = now_ms(); while (!access("/tmp/hibyos_boot", F_OK) && now_ms() - tb < 1500) msleep(20);
        if (st->held) cmd('R');
    }
    lconf_load();
    if (lconf_order_migrated) { lconf_order_migrated = 0; lconf_save(); }   /* orden oficial: una vez */
    pw_load();
    npx_start();
    audio_boot();
    buds_preload();                  /* auriculares: hilo + demonio hibybudsd (avisos, widget, app) */
    load_wall();
    load_icons();
    relabel_apps();
    if (!access("/usr/data/hibymod/no_launcher", F_OK)) launcher_enabled = 0;
    /* sustitucion del inicio de fabrica en la libreria */
    home_shm = map_file(HM_HOME, HM_FRAME_BYTES, 1);
    st->nsig = 0;
    for (int i = 0; i < home_sig_n && i < 8; i++) for (int k = 0; k < 6; k++) st->sig[i][k] = home_sig[i][k];
    st->nsig = home_sig_n < 8 ? home_sig_n : 8;
    if (launcher_enabled && home_shm) {
        render_launcher(img_cache_home, now_ms()); have_cache_home = 1; home_publish();
        st->home_sub_on = 1;
    }
    lg("hibyshell activo (firma inicio %d puntos, fuente %s, launcher %s)", home_sig_n, font_ok ? "ok" : "NO", launcher_enabled ? "si" : "no");

    double next_poll = 0, home_t = 1;
    uint32_t home_hits_seen = st->home_hits;
    uint32_t home_frames = st->frames, last_cap = st->cap_frames;
    for (;;) {
        double t = now_ms();
        if (sp_open_req) { sp_open_req = 0; cc_open_app("headphones"); continue; }   /* toque en el aviso */
        if (t >= next_poll) {
            int m = music_session();
            if (m != music_on) lg("musica: %s (pid %d)", m ? "si" : "no", player_pid);
            music_on = m; update_overlay(); next_poll = t + 700;
        }
        /* teclas de volumen (rueda) sin capturarlas: el reproductor sigue cambiando el volumen */
        st->shell_beat++;
        if (perf_on && pf.n && t - pf.last_end > 300) perf_flush();
        pw_tick();
        struct pollfd pf[2] = { { ts, POLLIN, 0 }, { keys, POLLIN, 0 } };
        int pr = poll(pf, keys >= 0 ? 2 : 1, hud_active ? 0 : launcher_on ? (V.edit ? 0 : now_ms() < toast_until ? 30 : 250) : 60);
        if (keys >= 0 && pr > 0 && (pf[1].revents & POLLIN)) {
            struct input_event ke[16];
            ssize_t n = read(keys, ke, sizeof ke);
            for (int i = 0; i < n / (ssize_t)sizeof ke[0]; i++)
                if (ke[i].type == EV_KEY && ke[i].value && (ke[i].code == KEY_LEFT || ke[i].code == KEY_RIGHT)) hud_key(ke[i].code);
        }
        if (hud_active) hud_step();
        if (launcher_on) {
            /* el reproductor dibujo algo: barra de estado nueva, o cambio de pantalla por su cuenta */
            if (st->cap_frames != last_cap) {
                last_cap = st->cap_frames;
                if (!is_home(fr1)) { lg("el reproductor cambio de pantalla"); launcher_leave(); }
                else launcher_statusbar();
            } else if (now_ms() < toast_until + 40 || V.edit) launcher_present();   /* aviso o temblor */
            else if (widgets_on && (wg_fresh || npx_changed || now_ms() - WD.t > 10000 || (int)(time(0) / 60) != wg_cache_min)) launcher_present();
            else if (mirror_dirty && pr == 0 && !hud_active) mirror_save();      /* en reposo */
            else if (!wg_cache_ok && pr == 0 && !hud_active && !widgets_on) { if (!WD.t) wg_refresh(); wg_compose(); }   /* widgets listos de antemano */
        } else if (launcher_enabled && !st->capture && !st->held && st->home_hits != home_hits_seen) {
            /* la libreria ya puso la imagen del launcher en un fotograma del inicio: entrar ya */
            home_hits_seen = st->home_hits;
            launcher_enter_ex(1); last_cap = st->cap_frames;
        } else if (launcher_enabled && !st->capture && !st->held) {
            /* volver al launcher cuando el inicio de fabrica queda quieto en pantalla */
            uint32_t f = st->frames; double tn = now_ms();
            if (f != home_frames) { home_frames = f; home_t = tn; }
            else if (home_t && tn - home_t > 80) {
                home_t = 0;
                const uint16_t *vv = visible();
                if (is_home(vv)) { launcher_enter(); last_cap = st->cap_frames; }
                else if (stream_launched && is_stream_page(vv)) {
                    /* volviendo de Tidal/Qobuz: saltar la pagina intermedia hasta el inicio */
                    stream_launched = 0;
                    st->ov_on = 0; ov_cur = 0;
                    cmd('C');
                    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
                    if (st->capture) {
                        inj_tap(22, 48);
                        double tw = now_ms(); while (!is_home(fr1) && now_ms() - tw < 1500) { msleep(10); st->shell_beat++; }
                        if (!launcher_resume()) cmd('R');
                        last_cap = st->cap_frames;
                    }
                } else if (!is_stream_page(vv) && !is_home(vv)) { /* sigue en la app */ }
            }
        }
        if (pr <= 0 || !(pf[0].revents & POLLIN)) continue;
        struct pkt p;
        if (!read_pkt(&p, 300)) continue;
        if (launcher_on && p.down && p.n) { launcher_touch(&p); last_cap = st->cap_frames; continue; }
        if (!p.down || p.n == 0) { fwd(&p); continue; }
        /* inicio de un toque */
        const uint16_t *v = visible();
        int plane = is_plane(v);
        /* gestos del sistema desde los bordes (configurables) */
        if (edge_dispatch(&p, v, plane)) { while (cur_down && read_pkt(&p, 5000)) fwd(&p); continue; }
        if (plane && p.y > 70 && p.y < 330) {
            /* en Now Playing: deslizar hacia abajo cierra la hoja */
            struct pkt held[16]; int nh = 0; held[nh++] = p;
            int sx = p.x, sy = p.y, decided = 0;
            while (!decided && read_pkt(&p, 1000)) {
                if (!p.down) { decided = 1; break; }
                int dy = p.y - sy, dx = abs(p.x - sx), rx = p.x - sx;
                if (dy > 16 && dy > dx * 2) { decided = 2; break; }
                if (dy < -16 && -dy > dx * 2) { decided = 3; break; }     /* arriba: al inicio */
                if (rx > 16 && rx > abs(dy) * 2) { decided = 4; break; }  /* derecha: centro de control */
                if (dx > 16 || dy < -16) { decided = 1; break; }
                if (nh < 16) held[nh++] = p; else { decided = 1; break; }
            }
            if (decided == 3) { lg("Now Playing: arriba -> inicio"); plane_up(sy); drain_until_up(); update_overlay(); continue; }
            if (decided == 4) { lg("Now Playing -> centro de control"); cc_from_player(sx); drain_until_up(); update_overlay(); continue; }
            if (decided == 2) {
                lg("cerrar Now Playing deslizando");
                cur_y = sy;
                sheet_close(1, 0);
                drain_until_up();
                update_overlay();
                continue;
            }
            for (int i = 0; i < nh; i++) fwd(&held[i]);
            fwd(&p);
            continue;
        }
        fwd(&p);
        /* el resto del toque pasa directo */
        while (cur_down && read_pkt(&p, 5000)) fwd(&p);
        next_poll = 0;
    }
}
