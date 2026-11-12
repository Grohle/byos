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
 *
 * POSICION, COLA Y VOLUMEN (v8, orden 'Q'): saltar a un punto de la pista, leer posicion y
 * duracion, "reproducir despues"/"anadir a la cola" sin cortar lo que suena y volumen absoluto.
 * Ver la ultima seccion y docs/research_seek_queue_volume.md. Inerte mientras nadie mande 'Q'.
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
#include "topbar_layout.h"

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
    /* solo debajo de la barra de estado. Antes tambien se componia la barra (transparente sobre
     * el fondo) EN el bufer del reproductor; el reproductor luego solo repinta sus cifras, asi que
     * su barra se quedaba con el fondo rosa del inicio y las apps de ByOS, que copian la barra del
     * reproductor, la mostraban asi. La barra transparente del inicio la pone el shell. */
    memcpy(b + 30 * fline, homeimg + 30 * fline, fsize - 30 * fline);
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


/* ---------------------------------------------------------------- "%" de la bateria a la derecha */
/* HiBy dibuja el porcentaje pegado a la IZQUIERDA de su caja (x = TOPBAR_PCT_X) y coloca el
 * Wi-Fi a la izquierda de donde quedaria el texto alineado a la derecha (pila - 2 - ancho -
 * Wi-Fi). Antes se empujaban esas columnas a la derecha y se arrastraba medio Wi-Fi (icono roto y
 * mas ancho, "%" cortado). Ahora el tema le da a HiBy cifras TRANSPARENTES de los mismos anchos
 * (topbar\batt\, su colocacion no cambia) y aqui se pinta "NN%" alineado a la derecha con los
 * glifos reales (batt_glyphs.h, generados por theme/topbar.py). Solo se escribe dentro de la caja
 * del texto actual y con el fondo de la fila 2 de cada columna: se puede repetir sin acumular. */
#include "batt_glyphs.h"
static uint32_t np_now(void);
static int sb_cap = -1; static uint32_t sb_t;
static uint16_t sb_raw[TOPBAR_Y1 - TOPBAR_Y0][TOPBAR_PCT_W];   /* el bloque del "%" tal como lo pinto HiBy */
static uint16_t sb_mix(uint16_t bg, uint16_t fg, unsigned a)
{
    unsigned r = ((bg >> 11) * (255 - a) + (fg >> 11) * a) / 255, g = (((bg >> 5) & 63) * (255 - a) + ((fg >> 5) & 63) * a) / 255;
    unsigned bl = ((bg & 31) * (255 - a) + (fg & 31) * a) / 255;
    return (uint16_t)(r << 11 | g << 5 | bl);
}
static void sb_fix(unsigned char *b)
{
    if (!b || fline < 320u * 2) return;
    uint32_t now = np_now();
    if (now - sb_t > 2000 || !sb_t) {            /* nivel de bateria: cada 2 s como mucho */
        sb_t = now ? now : 1;
        char v[8] = ""; int f = r_open("/sys/class/power_supply/battery/capacity", O_RDONLY);
        if (f >= 0) { ssize_t n = read(f, v, sizeof v - 1); if (n > 0) v[n] = 0; r_close(f); }
        sb_cap = v[0] ? atoi(v) : -1; if (sb_cap > 100) sb_cap = 100;
    }
    if (sb_cap < 0) return;
    /* solo si hay barra de estado (la pila arriba a la derecha): no en Reproduciendo ni el arranque */
    int bright = 0, red = 0;
    for (int y = 8; y < 22; y++) {
        const uint16_t *r = (const uint16_t *)(b + y * fline);
        for (int x = 286; x < 318; x++) {
            uint16_t p = r[x]; int R = p >> 11, G = (p >> 5) & 63, B = p & 31;
            if (R * 3 + G + B > 120) bright++;
            if (R >= 26 && G < 30 && B < 16) red++;   /* marco rojo de bateria baja */
        }
    }
    if (bright <= 12) return;
    char t[4]; int n = snprintf(t, sizeof t, "%d", sb_cap);
    int x1 = TOPBAR_PCT_X + TOPBAR_PCT_W, x0 = x1 - n * TOPBAR_DW - BG_W[10];
    uint16_t col = red > 6 ? BG_RED : 0xFFFF;
    const uint16_t *bgr = (const uint16_t *)(b + 2 * fline);
    for (int y = 0; y < BG_H && TOPBAR_Y0 + y < TOPBAR_Y1; y++) {
        uint16_t *r = (uint16_t *)(b + (TOPBAR_Y0 + y) * fline);
        int x = x0;
        for (int i = 0; i <= n; i++) {
            int g = i < n ? t[i] - '0' : 10;
            for (int gx = 0; gx < BG_W[g]; gx++) {
                unsigned a = BG_A[g][y][gx]; uint16_t bg = bgr[x + gx];
                r[x + gx] = a ? sb_mix(bg, col, a) : bg;
            }
            x += BG_W[g];
        }
    }
}

/* Rayo de carga legible: HiBy pinta el relleno blanco de la pila ENCIMA del marco de carga (que
 * lleva el rayo blanco), asi que el rayo desaparecia. Si se ve el marco verde de carga, se pinta
 * un rayo oscuro alli donde hay relleno claro (fuera del relleno sigue el blanco del marco). */
static unsigned char bolt_a[20][27]; static int bolt_ok;
static void bolt_mask(void)
{
    static const float px[] = { 13.4f, 9.6f, 12.4f, 11.4f, 15.4f, 12.6f }, py[] = { 3.0f, 10.6f, 10.6f, 17.0f, 9.2f, 9.2f };
    for (int y = 0; y < 20; y++) for (int x = 0; x < 27; x++) {
        int in = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float X = x + (sx + 0.5f) / 4, Y = y + (sy + 0.5f) / 4; int c = 0;
            for (int i = 0, j = 5; i < 6; j = i++)
                if ((py[i] > Y) != (py[j] > Y) && X < (px[j] - px[i]) * (Y - py[i]) / (py[j] - py[i]) + px[i]) c = !c;
            in += c;
        }
        bolt_a[y][x] = (unsigned char)(in * 255 / 16);
    }
    bolt_ok = 1;
}
static void sb_bolt(unsigned char *b)
{
    if (!b || fline < (TOPBAR_BATT_X + 27) * 2u) return;
    /* marco de carga: el borde superior del cuerpo es verde (GREEN de topbar.py) */
    uint16_t o = ((const uint16_t *)(b + (TOPBAR_Y0 + 4) * fline))[TOPBAR_BATT_X + 5];
    int r = o >> 11, g = (o >> 5) & 63, bl = o & 31;
    if (!(g > 2 * r + 10 && g > 2 * bl + 6)) return;
    if (!bolt_ok) bolt_mask();
    for (int y = 0; y < 20; y++) {
        uint16_t *row = (uint16_t *)(b + (TOPBAR_Y0 + y) * fline) + TOPBAR_BATT_X;
        for (int x = 0; x < 27; x++) {
            unsigned a = bolt_a[y][x]; if (!a) continue;
            uint16_t p = row[x];
            if (((p >> 11) < 26) || (((p >> 5) & 63) < 52) || ((p & 31) < 26)) continue;   /* solo sobre el relleno claro */
            unsigned na = 255 - a, d = 3;            /* casi negro */
            row[x] = (uint16_t)((((d * a + (p >> 11) * na) / 255) << 11) | (((2 * d * a + ((p >> 5) & 63) * na) / 255) << 5) | ((d * a + (p & 31) * na) / 255));
        }
    }
}

/* ---------------------------------------------------------------- esquinas redondeadas
 * Mascara negra con borde suavizado en las cuatro esquinas, aplicada a cada imagen justo antes
 * de que llegue al panel (fotogramas del reproductor, imagen del shell, capa). Solo se tocan
 * r*r pixeles por esquina; la tabla de cobertura se rehace si cambia el radio. */
#define CORNER_MAX 40
static unsigned char corner_cov[CORNER_MAX][CORNER_MAX];    /* 0 fuera .. 255 dentro (esquina sup. izq.) */
static int corner_tab_r;
static void corner_table(int r)
{
    for (int y = 0; y < r; y++) for (int x = 0; x < r; x++) {
        int in = 0;                                          /* 4x4 muestras por pixel */
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float dx = r - (x + (sx + 0.5f) / 4), dy = r - (y + (sy + 0.5f) / 4);
            if (dx * dx + dy * dy <= (float)r * r) in++;
        }
        corner_cov[y][x] = (unsigned char)(in * 255 / 16);
    }
    corner_tab_r = r;
}
static inline uint16_t corner_px(uint16_t p, unsigned a)
{
    if (a >= 255) return p;
    if (!a) return 0;
    unsigned rr = (p >> 11) * a / 255, g = ((p >> 5) & 63) * a / 255, b = (p & 31) * a / 255;
    return (uint16_t)(rr << 11 | g << 5 | b);
}
static void vol_publish(void);              /* al final del fichero */
static void corners_apply(unsigned char *buf)
{
    uint32_t r = st->corner_r;
    if (!r || !buf) return;
    if (r > CORNER_MAX) r = CORNER_MAX;
    if ((int)r != corner_tab_r) corner_table((int)r);
    for (uint32_t y = 0; y < r; y++) {
        uint16_t *top = (uint16_t *)(buf + y * fline), *bot = (uint16_t *)(buf + (HM_H - 1 - y) * fline);
        for (uint32_t x = 0; x < r; x++) {
            unsigned a = corner_cov[y][x];
            if (a >= 255) break;                             /* el resto de la fila ya esta dentro */
            top[x] = corner_px(top[x], a); top[HM_W - 1 - x] = corner_px(top[HM_W - 1 - x], a);
            bot[x] = corner_px(bot[x], a); bot[HM_W - 1 - x] = corner_px(bot[HM_W - 1 - x], a);
        }
    }
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
static void qc_kick(void);     /* v8 posicion/cola/volumen, al final del fichero */
static void lu_kick(void);     /* actualizar la biblioteca ('U'), al final del fichero */
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
            if (c == 'Q') { qc_kick(); continue; }   /* v8: posicion/salto, cola, volumen, idem */
            if (c == 'U') { lu_kick(); continue; }   /* actualizar la biblioteca de HiBy */
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
                    corners_apply(buf_at(last_var.yoffset));
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
                    corners_apply(d);
                }
                st->presents++;
            } else if (c == 'O') {
                if (!st->capture && !st->held && map_fb()) {
                    unsigned char *d = buf_at(last_var.yoffset);
                    frame_edge();            /* al empezar el vblank: quitar y poner seguidos */
                    ov_restore(d);           /* (decenas de us), nunca se barre a medias */
                    ov_apply(d);
                    corners_apply(d);
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

/* Shell caido: durante SHELL_GRACE_S se mantiene su ultima imagen (la captura sigue: el
 * reproductor sigue sonando y alternando bufers sin que se vea) y la tactil no llega al
 * reproductor; asi la interfaz de fabrica no asoma mientras el supervisor lo relanza. Si no
 * vuelve en ese tiempo, se suelta todo (red de seguridad: el aparato sigue siendo usable). */
#define SHELL_GRACE_S 20
static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void shell_release(void)
{
    struct input_event up[3]; memset(up, 0, sizeof up);
    up[0].type = EV_KEY; up[0].code = BTN_TOUCH; up[0].value = 0;
    up[1].type = EV_SYN; up[1].code = SYN_MT_REPORT;
    up[2].type = EV_SYN; up[2].code = SYN_REPORT;
    write(uin, up, sizeof up);
    pthread_mutex_lock(&mu);
    if (st->capture || st->held) {
        st->capture = 0; st->held = 0;
        if (st->waiting) pthread_cond_broadcast(&cv);
        else if (fbmem && frames) { frame_edge(); memcpy(buf_at(last_var.yoffset), frames + fsize, fsize); st->ov_saved = 0; corners_apply(buf_at(last_var.yoffset)); }
    }
    st->ov_on = 0;
    if (fbmem) { frame_edge(); ov_restore(buf_at(last_var.yoffset)); }
    pthread_mutex_unlock(&mu);
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
    int cl = -1; double down_t = 0;          /* down_t > 0: el shell se cayo en ese instante */
    struct input_event ev[64];
    for (;;) {
        if (down_t && mono_s() - down_t > SHELL_GRACE_S) { down_t = 0; shell_release(); }
        struct pollfd p[3]; int n = 0;
        int irf = -1, ils = -1, icl = -1;
        if (rf >= 0) { p[n].fd = rf; p[n].events = POLLIN; irf = n++; }
        if (ls >= 0 && cl < 0) { p[n].fd = ls; p[n].events = POLLIN; ils = n++; }
        if (cl >= 0) { p[n].fd = cl; p[n].events = POLLIN; icl = n++; }
        if (poll(p, n, down_t ? 500 : -1) < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        if (irf >= 0 && (p[irf].revents & POLLIN)) {
            ssize_t r = read(rf, ev, sizeof ev);
            if (r > 0 && cl < 0 && !down_t) write(uin, ev, r);   /* paso directo (no mientras el shell vuelve) */
        }
        if (ils >= 0 && (p[ils].revents & POLLIN)) {
            cl = accept(ls, 0, 0);
            if (cl >= 0) {
                down_t = 0;                                    /* el shell volvio: sigue con su imagen */
                struct ucred cr; socklen_t cl_len = sizeof cr;
                st->shell_pid = getsockopt(cl, SOL_SOCKET, SO_PEERCRED, &cr, &cl_len) == 0 ? (uint32_t)cr.pid : 0;
                st->input_mode = 2;
            }
        }
        if (icl >= 0 && (p[icl].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(cl, ev, sizeof ev);
            if (r <= 0) {
                close(cl); cl = -1; st->input_mode = 1;
                /* el shell murio: soltar el toque a medias; su imagen se queda SHELL_GRACE_S */
                { struct input_event up[3]; memset(up, 0, sizeof up);
                  up[0].type = EV_KEY; up[0].code = BTN_TOUCH; up[0].value = 0;
                  up[1].type = EV_SYN; up[1].code = SYN_MT_REPORT;
                  up[2].type = EV_SYN; up[2].code = SYN_REPORT;
                  write(uin, up, sizeof up); }
                if (st->capture || st->held) down_t = mono_s(); else shell_release();
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
        if (++still >= 10) {             /* 10 s: con poca memoria el shell puede ir lento sin estar colgado */
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
 * ioctl WRITEI de tinyalsa; con BT/USB, snd_pcm_writei) esta en SCHED_FIFO 10 SOLO durante la
 * escritura (rt_enter/rt_leave alrededor de la llamada real): gana la latencia de despertar
 * cuando el DMA deja sitio. Antes se quedaba en tiempo real para siempre y ese hilo tambien
 * abre, analiza y decodifica: al empezar una pista (bufer vacio, las escrituras no bloquean)
 * ocupaba la CPU ~1 s y la interfaz se congelaba (medido: pintar un toque 974 ms en vez de
 * 12 ms). Fuera de la escritura va como en el firmware de fabrica. El kernel limita el tiempo real al 95 %
 * (sched_rt_runtime_us = 950000), asi que un fallo suyo no congela el sistema.
 * Se desactiva con /usr/data/hibymod/no_rt. El decodificador ("lg_music_player") no se toca. */
#define RT_PRIO 10
static __thread int rt_done;
static int rt_boost(void)                /* 1 = este hilo queda en tiempo real */
{
    rt_done = 1;
    if (!access("/usr/data/hibymod/no_rt", F_OK)) return 0;
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
    return !e;
}

static __thread int rt_on;               /* 1 = este hilo puede usar tiempo real (sin no_rt) */
static void rt_enter(void)
{
    if (!rt_done) { rt_on = rt_boost(); return; }
    if (rt_on) { struct sched_param sp = { .sched_priority = RT_PRIO }; pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); }
}
static void rt_leave(void)
{
    if (rt_on) { struct sched_param sp = { .sched_priority = 0 }; pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp); }
}
static void pcm_wrote(long n)
{
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
        static uint32_t shown_y = 0xffffffffu;
        if (last_var.yoffset == shown_y) st->same_pans++;
        unsigned char *restore = 0;              /* bufer que deja de verse: vuelve el fotograma de HiBy */
        if (st->capture && frames) {
            unsigned char *b = buf_at(last_var.yoffset);
            /* el bloque del "%" tal cual lo dibujo HiBy (se le devuelve en "restore"), luego la barra
             * retocada EN SU BUFER y despues la copia a frames[1]: retocar la copia dejaba un hueco
             * en el que el shell (pildora, barra de las apps) leia la barra sin el "%" ni el rayo y la
             * mostraba un fotograma (medido con el probador) */
            for (int y = TOPBAR_Y0; y < TOPBAR_Y1; y++) memcpy(sb_raw[y - TOPBAR_Y0], (const uint16_t *)(b + y * fline) + TOPBAR_PCT_X, TOPBAR_PCT_W * 2);
            sb_fix(b);
            sb_bolt(b);
            memcpy(frames + fsize, b, fsize);   /* fotograma del reproductor (ya retocado) -> frames[1] */
            wait_even(&st->fr0_seq);
            memcpy(b, frames, fsize);           /* el panel sigue con la imagen del shell */
            corners_apply(b);
            st->cap_frames++;
            if (shown_y != 0xffffffffu && shown_y != last_var.yoffset && shown_y * fline + fsize <= 2 * fsize) restore = buf_at(shown_y);
        } else {
            if (map_fb()) sb_fix(buf_at(last_var.yoffset));
            if (map_fb()) sb_bolt(buf_at(last_var.yoffset));
            if (st->home_sub_on && map_fb()) home_substitute(buf_at(last_var.yoffset));
            if (st->ov_on && map_fb()) ov_apply(buf_at(last_var.yoffset));
            else st->ov_saved = 0;
            if (map_fb()) corners_apply(buf_at(last_var.yoffset));
        }
        st->frames++;
        vol_publish();
        int r = r_ioctl(fd, req, arg);
        shown_y = last_var.yoffset;
        /* Capturando, HiBy recibe de vuelta sus buferes con la imagen del shell dentro y luego solo
         * repinta lo que cambio ENCIMA de eso: la barra de estado salia mezclada (el Wi-Fi roto, el
         * "%" empujado dos veces bajo la pila). Tras el pan (ese bufer ya no se ve) se le devuelve
         * su ultimo fotograma, con el "%" donde el lo dibujo. ~1 ms, sin memoria extra. */
        if (restore) {
            memcpy(restore, frames + fsize, fsize);
            for (int y = TOPBAR_Y0; y < TOPBAR_Y1; y++) memcpy((uint16_t *)(restore + y * fline) + TOPBAR_PCT_X, sb_raw[y - TOPBAR_Y0], TOPBAR_PCT_W * 2);
        }
        pthread_mutex_unlock(&mu);
        return r;
    }
    if (active && fd >= 0 && fd == pcm_fd) {
        int w = !in_alsa && (req == SNDRV_PCM_IOCTL_WRITEI_FRAMES || req == SNDRV_PCM_IOCTL_WRITEN_FRAMES);
        if (w) rt_enter();
        int r = r_ioctl(fd, req, arg);
        if (w) rt_leave();
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
    if (active && !sv) rt_enter();
    long k = r(pcm, buf, n);
    if (active && !sv) rt_leave();
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
    SF_OPT    = 128,   /* v8: se comprueba por separado; si su caso no casa, solo esa opcion falta */
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
    /* v8 (docs/research_settings_api.md §9): pantalla y apagado por inactividad. Mismo camino
     * con cerrojo (system_lister) que sleep/sleepmin. bkltime sin control de rango dentro del
     * reproductor: lo pone lo/hi aqui. */
    { "bklon",     0x0e,   0,   1, SF_OPT,      0,     0,     0x42a96c, 0x42c728 },
    { "bkltime",   0x0f,  10, 120, SF_OPT,      0,     0,     0x42a964, 0x42c184 },
    { "idle",      0x0b,   0,   1, SF_OPT,      0,     0,     0x42a984, 0x42c6c8 },
    { "idlemin",   0x0c,   1,  10, SF_OPT,      0,     0,     0x42a97c, 0x42c6a8 },
    { "update",    0x07,   0,   1, SF_OPT,      0,     0,     0x42a9a4, 0x42c540 },
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
        if (st_opt[i].flags & SF_OPT) continue;                         /* v8: st_opt_ok() */
        if (gt[st_opt[i].id] != st_opt[i].get_case) return -ENOEXEC;
        if (st_opt[i].set_case && ss[st_opt[i].id] != st_opt[i].set_case) return -ENOEXEC;
    }
    if (!pl_mapped(HP_SETTINGS, 4, 0)) return -ENOEXEC;
    uint32_t s = *(const uint32_t *)(uintptr_t)HP_SETTINGS;
    if (!s || !pl_mapped(s, 0xad0, 0)) return -EAGAIN;              /* ajustes aun sin cargar */
    return 0;
}

/* una opcion v8 solo se usa si sus casos de las tablas de saltos son los esperados */
static int st_opt_ok(const struct st_opt *o)
{
    if (!(o->flags & SF_OPT)) return 1;
    const uint32_t *gt = (const uint32_t *)(uintptr_t)HP_SGET_TBL, *ss = (const uint32_t *)(uintptr_t)HP_SSET_TBL;
    return gt[o->id] == o->get_case && (!o->set_case || ss[o->id] == o->set_case);
}
static const struct st_opt *st_find(const char *k)
{
    for (int i = 0; i < ST_NOPT; i++) if (!strcmp(k, st_opt[i].name)) return st_opt_ok(&st_opt[i]) ? &st_opt[i] : 0;
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
            if (st_opt_ok(&st_opt[i])) k += (size_t)snprintf(extra + k, sizeof extra - k, "%s=%d\n", st_opt[i].name, st_get(st_opt[i].id));
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

/* ================================================================ v8: posicion, cola y volumen ('Q')
 * Detalles, evidencia y riesgos: docs/research_seek_queue_volume.md. INERTE si nadie manda 'Q'.
 *
 *   SALTAR  seek_locked(ms) 0x437780 = cerrojo del reproductor + 0x4376a0(ms) + soltar. 0x4376a0 es
 *     lo que llaman el arrastre de la barra de progreso de fabrica (0x50c924), el avance rapido con
 *     teclas (0x510004) y setProgress de HiBy Link (0x457778, desde su hilo); el mensaje 3 de la
 *     cola del reproductor (DLNA "seek") usa 0x437780. Unidad: ms ABSOLUTOS en el fichero; la pista
 *     va de get_param(0xd) (inicio, >0 en pistas de .cue) a get_param(0xe) (fin), como hace la
 *     interfaz de fabrica: posicion = get_param(6) - inicio, duracion = fin - inicio.
 *   COLA    la lista que suena se lee de list_tb_<id> CADA VEZ que hace falta una pista (0x4341c0 ->
 *     0x71de60, sin cache), y la siguiente es indice+1 sobre n = get_param(8) (0x435fc0). Si la
 *     lista es nuestra (hibymod\q1/q2, orden = rowid) se inserta la fila detras de la actual
 *     moviendo las de detras (o al final) y despues set_param(8, n+1): no se corta nada. En
 *     aleatorio el orden es un vector de n entradas (P+0xec): solo con reshuffle=1 se rehace con
 *     0x435ce0(n+1) bajo el cerrojo del reproductor (como hace la radio de fabrica, 0x4ce554) y la
 *     nueva va justo despues de la actual. Lista ajena (album de fabrica...): rc=-ENOTSUP, o con
 *     rebuild=1 se copia a nuestra ranura 1 + la nueva y se continua en la misma pista y posicion
 *     (play_index con pos = ms absolutos, como al reanudar al encender; corte breve).
 *   VOLUMEN la secuencia de setVolume de HiBy Link (st_set de la seccion 'S'), maximo setting 0x10;
 *     como mucho una escritura cada 50 ms (cada una reescribe user.ini).
 *
 * Peticion (QC_REQ, rename atomico):  HMQ1 / id=N / op=pos|seek|next|append|vol|volget|check /
 *   [ms=MS] [val=V] [path=RUTA o N|RUTA (pista N de un .cue)] [dup=1] [unscanned=1] [reshuffle=1] [rebuild=1]
 * Respuesta (QC_RES): "id=N rc=R op=OP pos=MS dur=MS state=S index=I count=C val=V max=M ms=T mensaje"
 *   state: 0 en pausa, 1 sonando, 2 parado (0x437260). Una linea "qc: ..." en disp.log. */
#define QC_REQ "/tmp/hibymod_qreq"
#define QC_RES "/tmp/hibymod_qres"
#define HP_GET_TBL 0x7f1cb0u              /* tabla de saltos de get_param (0x20 entradas) */
#define HP_SET_TBL 0x7f1c34u              /* tabla de saltos de set_param (0x1f entradas) */
/* 16 bytes en la entrada + 16 en una segunda ventana (varias de estas funciones empiezan igual) */
static const struct { uint32_t addr, off2, w[4], w2[4]; } qc_sig[] = {
    { 0x437780, 0x20, { 0x27bdffd8, 0xafb00020, 0x3c100097, 0x26109ac8 }, { 0x0c10dda8, 0x8fa40018, 0x8e19004c, 0x0320f809 } }, /* 0 seek_locked(ms) */
    { 0x4376a0, 0x80, { 0x27bdffd8, 0xafb0001c, 0x3c100097, 0x26109ac8 }, { 0x0c10f768, 0x02202025, 0x2404000a, 0x0c11ed68 } }, /* 1 seek(ms), la llama 0 */
    { 0x437420, 0x2c, { 0x27bdffd8, 0xafb0001c, 0x3c100097, 0x26109ac8 }, { 0x0c10dce8, 0x00000000, 0x00408825, 0x8e19004c } }, /* 2 track_active() */
    { 0x437260, 0x2c, { 0x27bdffd8, 0xafb0001c, 0x3c100097, 0x26109ac8 }, { 0x0c10f790, 0x00000000, 0x24030001, 0x1043000e } }, /* 3 play_state() */
    { 0x435ce0, 0x40, { 0x27bdffd0, 0xafb0001c, 0x3c100097, 0x26109ac8 }, { 0xae0000ec, 0x0c11ec70, 0x001320c0, 0x10400077 } }, /* 4 shuffle_build(n, otra) */
    { 0x43dda0, 0x00, { 0x00802825, 0x00003025, 0x0819d559, 0x24040205 }, { 0x00802825, 0x00003025, 0x0819d559, 0x24040205 } }, /* 5 motor: seek, la llama 1 */
    { 0x4341c0, 0x40, { 0x27bdfc40, 0xafbf03bc, 0xafb403b8, 0xafb303b4 }, { 0x00002825, 0x26041828, 0x0c1098c0, 0x3c120097 } }, /* 6 read_item(i, &buf) */
};
static const struct { uint32_t tbl; int id; uint32_t dst; } qc_jt[] = {   /* casos de get/set_param usados */
    { HP_GET_TBL, 0x06, 0x436c28 }, { HP_GET_TBL, 0x08, 0x436bd8 }, { HP_GET_TBL, 0x0a, 0x436bc4 },
    { HP_GET_TBL, 0x0b, 0x436bb0 }, { HP_GET_TBL, 0x0d, 0x436b88 }, { HP_GET_TBL, 0x0e, 0x436b74 },
    { HP_GET_TBL, 0x15, 0x436b18 }, { HP_GET_TBL, 0x18, 0x436a7c },
    { HP_SET_TBL, 0x08, 0x436848 }, { HP_SET_TBL, 0x10, 0x4367ec }, { HP_SET_TBL, 0x15, 0x4367a4 },
    { HP_SET_TBL, 0x17, 0x436798 },
};
typedef void (*qc_seek_t)(int);
typedef int (*qc_i0_t)(void);
typedef void *(*qc_shuf_t)(int, int);
typedef int (*qc_item_t)(int, char **);
#define QCF(i, t) ((t)(uintptr_t)qc_sig[i].addr)
#define HP_ITEM_SIZE 0xa88                /* elemento de lista: +4 ruta UTF-16 (0x104), +0x424 cue_id */

static volatile int qc_busy, qc_again;
static struct stat qc_last;
static uint32_t qc_vol_ms;

/* La parte fija (codigo y tablas de solo lectura del binario) se comprueba entera la primera vez;
 * despues solo el estado que cambia en marcha (cerrojo del reproductor y contexto de la base, como
 * pl_check). Asi un deslizador de volumen no relee /proc/self/maps ~40 veces por peticion. */
static int qc_ok_static, qc_ok_settings;
static uint32_t qc_ok_ctx;
static int qc_check(void)
{
    if (qc_ok_static) {
        const uint32_t *p = (const uint32_t *)(uintptr_t)HP_P;
        if (!p[0x48 / 4] || !p[0x4c / 4]) return -EAGAIN;
        uint32_t c = *(const uint32_t *)(uintptr_t)HP_DBCTX;
        if (c && c == qc_ok_ctx) {
            const uint32_t *ctx = (const uint32_t *)(uintptr_t)c;
            if (ctx[0] && ctx[0x564 / 4] == 0x70eee0 && ctx[0x568 / 4] == 0x70ef00) return 0;
        }
        int r = pl_check();                  /* el contexto cambio: comprobarlo entero otra vez */
        if (!r) qc_ok_ctx = *(const uint32_t *)(uintptr_t)HP_DBCTX;
        return r;
    }
    int r = pl_check();
    if (r) return r;
    for (size_t i = 0; i < sizeof qc_sig / sizeof qc_sig[0]; i++) {
        uint32_t a = qc_sig[i].addr, b = a + qc_sig[i].off2;
        if (!pl_mapped(a, 16, 1) || memcmp((const void *)(uintptr_t)a, qc_sig[i].w, 16)) return -ENOEXEC;
        if (!pl_mapped(b, 16, 1) || memcmp((const void *)(uintptr_t)b, qc_sig[i].w2, 16)) return -ENOEXEC;
    }
    for (size_t i = 0; i < sizeof qc_jt / sizeof qc_jt[0]; i++) {
        uint32_t a = qc_jt[i].tbl + (uint32_t)qc_jt[i].id * 4;
        if (!pl_mapped(a, 4, 0) || *(const uint32_t *)(uintptr_t)a != qc_jt[i].dst) return -ENOEXEC;
    }
    qc_ok_ctx = *(const uint32_t *)(uintptr_t)HP_DBCTX;
    qc_ok_static = 1;
    return 0;
}

/* st_check (seccion 'S') una vez; despues basta con que la estructura de ajustes siga siendo la misma */
static int qc_check_settings(void)
{
    static uint32_t s0;
    if (qc_ok_settings) {                    /* solo tras un st_check bueno: HP_SETTINGS esta proyectado */
        uint32_t s = *(const uint32_t *)(uintptr_t)HP_SETTINGS;
        if (s && s == s0) return 0;
    }
    int r = st_check();
    if (!r) { qc_ok_settings = 1; s0 = *(const uint32_t *)(uintptr_t)HP_SETTINGS; }
    return r;
}

static int qc_geti(int id) { int v = 0; PLF(1, pl_getp_t)(id, &v, 0); return v; }

struct qc_out { int pos, dur, state, index, count, val, max; };

/* lista que suena (UTF-16, 0x104 unidades); devuelve 1/2 si es hibymod\q1/q2, 0 si no */
static int qc_curlist(hw16 *cur)
{
    memset(cur, 0, 0x104 * sizeof *cur);
    PLF(1, pl_getp_t)(0x15, cur, 0);
    static const hw16 q[] = { 'h','i','b','y','m','o','d','\\','q' };
    if (memcmp(cur, q, sizeof q) || cur[10] || (cur[9] != '1' && cur[9] != '2')) return 0;
    return cur[9] - '0';
}

static void qc_fill(struct qc_out *o)
{
    int beg = qc_geti(0x0d), end = qc_geti(0x0e), pos = qc_geti(0x06);
    o->state = QCF(3, qc_i0_t)();
    o->dur = end > beg ? end - beg : 0;
    o->pos = pos > beg ? pos - beg : 0;
    o->index = qc_geti(0x0b);
    o->count = qc_geti(0x08);
}

static int qc_seek(int ms, struct qc_out *o, const char **msg)
{
    static hw16 cur[0x104];
    if (!QCF(2, qc_i0_t)()) { *msg = "nada cargado (parado)"; return -ENODATA; }
    qc_curlist(cur);
    if (cur[0] == 'd' && cur[1] == 'l' && cur[2] == 'n' && cur[3] == 'a') { *msg = "modo DLNA: lo controla dmrd"; return -ENOTSUP; }
    int beg = qc_geti(0x0d), end = qc_geti(0x0e), dur = end - beg;
    if (dur <= 0) { *msg = "duracion desconocida (radio/stream)"; return -ENOTSUP; }
    if (ms < 0 || ms > dur) { *msg = "fuera de la pista"; return -ERANGE; }
    int lim = dur > 1500 ? dur - 1000 : 0;   /* no saltar al ultimo segundo (fin de pista inmediato) */
    *msg = "ok";
    if (ms > lim) { ms = lim; *msg = "ok (recortado al ultimo segundo)"; }
    QCF(0, qc_seek_t)(beg + ms);             /* = 0x4376a0 bajo el cerrojo del reproductor */
    qc_fill(o);
    return 0;
}

#define QC_SEL "COALESCE(m.name,r.path),COALESCE(m.album,''),COALESCE(m.artist,''),COALESCE(m.genre,''),"         \
    "COALESCE(m.year,0),COALESCE(m.dis_id,0),COALESCE(m.ck_id,0),COALESCE(m.has_child_file,0),COALESCE(m.begin_time,0)," \
    "COALESCE(m.end_time,0),COALESCE(m.cue_id,-1),COALESCE(m.character,''),COALESCE(m.size,0),COALESCE(m.sample_rate,0),"  \
    "COALESCE(m.bit_rate,0),COALESCE(m.bit,0),COALESCE(m.channel,0),COALESCE(m.format,0),COALESCE(m.quality,''),"         \
    "COALESCE(m.album_pic_path,''),COALESCE(m.lrc_path,''),COALESCE(m.track_gain,0),COALESCE(m.track_peak,0),"           \
    "COALESCE(m.ctime,0),COALESCE(m.mtime,0) "

/* dentro de BEGIN..COMMIT: mete la pista en list_tb_<tb> en la posicion "at" (0..n; las de detras
 * se mueven) y deja count/music_count de MANAGER_TABLE en n+1. Devuelve filas o <0 (d->rc/err). */
static int qc_put(struct pl_db *d, int tb, int at, int n, const struct pl_ent *e, int dup, int unscanned)
{
    static hw16 w[1024];
    char hp[1024], q[1700];
    int R = -1, rows = -1;
    if (at > 0) {                          /* rowid de la fila en la posicion at-1 (orden = rowid) */
        snprintf(q, sizeof q, "SELECT rowid FROM list_tb_%d ORDER BY rowid LIMIT 1 OFFSET ?3", tb);
        pl_sql(d, q, 0, 0, 0, 0, at - 1, &R);
        if (!d->rc && R < 0) { d->rc = -1; d->err = "posicion fuera de la lista"; }
    } else R = 0;
    if (at < n) {                          /* hueco en R+1: dos pasos para no chocar con el rowid unico */
        snprintf(q, sizeof q, "UPDATE list_tb_%d SET rowid = -rowid WHERE rowid > ?3", tb);   pl_sql(d, q, 0, 0, 0, 0, R, 0);
        snprintf(q, sizeof q, "UPDATE list_tb_%d SET rowid = 1 - rowid WHERE rowid < 0", tb); pl_sql(d, q, 0, 0, 0, 0, INT32_MIN, 0);
    }
    pl_sql(d, "CREATE TEMP TABLE IF NOT EXISTS hm_req(pos INTEGER PRIMARY KEY, path TEXT COLLATE NOCASE, cue INT)", 0, 0, 0, 0, INT32_MIN, 0);
    pl_sql(d, "DELETE FROM hm_req", 0, 0, 0, 0, INT32_MIN, 0);
    pl_hpath(e->path, hp, sizeof hp);
    int k = pl_w16(hp, w, 1024);
    snprintf(q, sizeof q, "INSERT INTO hm_req(pos, path, cue) VALUES (0, ?1, %d)", e->cue);
    pl_sql(d, q, w, k, 0, 0, INT32_MIN, 0);
    /* sin OR IGNORE: una pista que ya esta en la cola da SQLITE_CONSTRAINT (19) -> -EEXIST;
     * con dup=1 entra con un id negativo propio (la clave es id+path+cue_id) */
    snprintf(q, sizeof q,
        "INSERT INTO list_tb_%d(rowid," PL_COLS ") SELECT ?3,%s,r.path," QC_SEL
        "FROM hm_req r LEFT JOIN MEDIA_TABLE m ON m.path = r.path AND (r.cue < 0 OR m.cue_id = r.cue) "
        "WHERE m.path IS NOT NULL OR %d ORDER BY m.cue_id LIMIT 1",
        tb, dup ? "-1000000-?3" : "COALESCE(m.id,-1)", unscanned ? 1 : 0);
    pl_sql(d, q, 0, 0, 0, 0, R + 1, 0);
    snprintf(q, sizeof q, "SELECT count(*) FROM list_tb_%d", tb);   pl_sql(d, q, 0, 0, 0, 0, INT32_MIN, &rows);
    if (!d->rc && rows != n + 1) { d->rc = -1; d->err = "no esta en la biblioteca (MEDIA_TABLE); unscanned=1 la mete igual"; }
    snprintf(q, sizeof q, "UPDATE MANAGER_TABLE SET count = ?3, music_count = ?3 WHERE id = %d", tb);
    pl_sql(d, q, 0, 0, 0, 0, rows, 0);
    return d->rc ? -1 : rows;
}

/* "reproducir despues" (append=0) o "anadir al final" (append=1) */
static int qc_queue(const struct pl_ent *e, int append, int dup, int unscanned, int reshuffle, int rebuild,
                    struct qc_out *o, const char **msg)
{
    static hw16 cur[0x104], lw[16];
    static char cmsg[160], item[HP_ITEM_SIZE];
    int slot = qc_curlist(cur), mode = qc_geti(0x0a), ci = qc_geti(0x0b), n = qc_geti(0x08);
    if (!cur[0]) { *msg = "no hay lista cargada: usa 'L' (pc_play_list)"; return -ENODATA; }
    if (ci < 0 || ci >= n) append = 1;
    int at = append ? n : ci + 1;
    if (!slot && !rebuild) {
        int k = 0; for (; k < 60 && cur[k]; k++) cmsg[k] = cur[k] < 0x80 ? (char)cur[k] : '?';
        cmsg[k] = 0;
        snprintf(cmsg + k, sizeof cmsg - (size_t)k, ": no es una cola de hibymod (rebuild=1 la copia)");
        *msg = cmsg; return -ENOTSUP;
    }
    if (slot && mode == 2 && !reshuffle) { *msg = "aleatorio: reshuffle=1 rehace el orden aleatorio"; return -EPERM; }
    { struct statvfs v; if (!statvfs("/usr/data", &v) && (unsigned long long)v.f_bavail * v.f_frsize < (6ull << 20) + (unsigned long long)n * 2048)
        { *msg = "poco espacio en /usr/data: no se escribe la cola"; return -ENOSPC; } }
    /* lista ajena: su orden puede no ser el rowid (vistas ordenadas, carpetas). Antes de copiar se
     * leen varias posiciones con el lector del propio reproductor (0x4341c0, el de "siguiente", bajo
     * su cerrojo y fuera del de la base) para comprobar despues que la copia coincide */
#define QC_NSAMP 6
    static hw16 samp[QC_NSAMP][0x104];
    int sidx[QC_NSAMP], ns = 0;
    if (!slot) {
        const int cand[QC_NSAMP] = { ci, ci + 1, 0, n / 2, n - 1, ci - 1 };
        const uint32_t *p = (const uint32_t *)(uintptr_t)HP_P;
        for (int i = 0; i < QC_NSAMP; i++) {
            int x = cand[i], seen = 0;
            if (x < 0 || x >= n) continue;
            for (int j = 0; j < ns; j++) if (sidx[j] == x) seen = 1;
            if (seen) continue;
            char *bp = item;
            memset(item, 0, sizeof item);
            ((pl_void_t)(uintptr_t)p[0x48 / 4])();
            int r = QCF(6, qc_item_t)(x, &bp);
            ((pl_void_t)(uintptr_t)p[0x4c / 4])();
            if (r || !item[4]) { *msg = "no se pudo leer la lista actual con el lector del reproductor"; return -ENOTSUP; }
            memcpy(samp[ns], item + 4, 0x103 * sizeof(hw16));
            samp[ns][0x103] = 0;
            sidx[ns++] = x;
        }
    }
    struct pl_db d = { *(void **)*(uint32_t *const *)(uintptr_t)HP_DBCTX, 0, 0 };
    char q[1200];                            /* CREATE ... PL_CREATE ~540, copia ~500 */
    int rows = -1, src = -1, same = 0;
    int tb = slot ? PL_TB0 + slot - 1 : PL_TB0;   /* lista ajena: se copia a la ranura 1 (no suena) */
    PLF(4, pl_void_t)();
    pl_sql(&d, "BEGIN", 0, 0, 0, 0, INT32_MIN, 0);
    if (d.rc) { PLF(5, pl_void_t)(); *msg = "BEGIN (transaccion abierta?)"; return -EBUSY; }
    if (slot) {                              /* la nuestra: debe seguir siendo la que conoce el reproductor */
        snprintf(q, sizeof q, "SELECT count(*) FROM list_tb_%d", tb);   pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, &rows);
        if (!d.rc && rows != n) { d.rc = -1; d.err = "la tabla no coincide con el reproductor (n distinto)"; }
    } else {
        int lpn = 0; while (lpn < 0x103 && cur[lpn]) lpn++;
        pl_sql(&d, "SELECT id FROM MANAGER_TABLE WHERE path = ?1", cur, lpn, 0, 0, INT32_MIN, &src);
        if (!d.rc && (src < 0 || src == PL_TB0 || src == PL_TB0 + 1)) { d.rc = -1; d.err = "lista virtual (sin list_tb): no se puede copiar"; }
        snprintf(q, sizeof q, "DROP TABLE IF EXISTS list_tb_%d", tb);                    pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, 0);
        snprintf(q, sizeof q, "CREATE TABLE IF NOT EXISTS list_tb_%d" PL_CREATE, tb);      pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, 0);
        if (!d.rc) {
            snprintf(q, sizeof q, "INSERT INTO list_tb_%d(" PL_COLS ") SELECT " PL_COLS " FROM list_tb_%d ORDER BY rowid", tb, src);
            pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, 0);
        }
        snprintf(q, sizeof q, "SELECT count(*) FROM list_tb_%d", tb);   pl_sql(&d, q, 0, 0, 0, 0, INT32_MIN, &rows);
        if (!d.rc && rows != n) { d.rc = -1; d.err = "la copia no tiene las n filas del reproductor"; }
        /* cada posicion leida antes por el reproductor debe tener la misma ruta en la copia (orden
         * rowid); si no, se aborta sin tocar nada */
        snprintf(q, sizeof q, "SELECT count(*) FROM (SELECT path FROM list_tb_%d ORDER BY rowid LIMIT 1 OFFSET ?3) WHERE path = ?1", tb);
        for (int i = 0; i < ns && !d.rc; i++) {
            int ipn = 0; while (ipn < 0x103 && samp[i][ipn]) ipn++;
            same = 0;
            pl_sql(&d, q, samp[i], ipn, 0, 0, sidx[i], &same);
            if (!d.rc && same != 1) { d.rc = -1; d.err = "orden distinto al del reproductor: no se copia"; }
        }
        int lpn2 = pl_w16("hibymod\\q1", lw, 16);
        pl_sql(&d, "DELETE FROM MANAGER_TABLE WHERE id = ?3 OR path = ?1", lw, lpn2, 0, 0, tb, 0);
        snprintf(q, sizeof q, "INSERT INTO MANAGER_TABLE(id, path, count, music_count, dir_count, m3u_count) VALUES (?3, ?1, %d, %d, 0, 0)", n, n);
        pl_sql(&d, q, lw, lpn2, 0, 0, tb, 0);
    }
    if (!d.rc) rows = qc_put(&d, tb, at, n, e, dup, unscanned);
    if (d.rc) {
        int keep = d.rc; const char *ke = d.err;
        d.rc = 0; pl_sql(&d, "ROLLBACK", 0, 0, 0, 0, INT32_MIN, 0);
        PLF(5, pl_void_t)();
        *msg = ke ? ke : "sql";
        if (keep == 19) { *msg = "ya esta en la cola (dup=1 para repetirla)"; return -EEXIST; }
        return keep > 0 ? -1000 - keep : (ke && !strncmp(ke, "no esta", 7)) ? -ENOENT : -ENOTSUP;
    }
    pl_sql(&d, "COMMIT", 0, 0, 0, 0, INT32_MIN, 0);
    PLF(5, pl_void_t)();
    if (d.rc) { *msg = "COMMIT"; return -1000 - d.rc; }
    if (slot) {
        if (mode == 2) {                     /* vector aleatorio de n+1 bajo el cerrojo; la nueva, la siguiente */
            const uint32_t *p = (const uint32_t *)(uintptr_t)HP_P;
            ((pl_void_t)(uintptr_t)p[0x48 / 4])();
            QCF(4, qc_shuf_t)(rows, 0);
            int32_t *arr = (int32_t *)(uintptr_t)p[0xec / 4];
            int f0 = (int)p[0xf0 / 4], sz = (int)p[0xf4 / 4];
            if (arr && sz == rows && !append && f0 >= 0 && f0 + 1 < sz)
                for (int i = 0; i < sz; i++)
                    if (arr[2 * i] == at) {      /* {indice, ya sonada}: ponerla justo tras la actual */
                        int32_t t0 = arr[2 * (f0 + 1)], t1 = arr[2 * (f0 + 1) + 1];
                        arr[2 * (f0 + 1)] = arr[2 * i]; arr[2 * (f0 + 1) + 1] = 0;
                        arr[2 * i] = t0; arr[2 * i + 1] = t1;
                        break;
                    }
            ((pl_void_t)(uintptr_t)p[0x4c / 4])();
        }
        PLF(0, pl_setp_t)(0x08, (const void *)(intptr_t)rows, 0);    /* despues de la tabla: n+1 */
        *msg = "ok";
    } else {                                 /* cambiar a la copia en la misma pista y posicion */
        int pos = qc_geti(0x06);
        pl_w16("hibymod\\q1", lw, 16);
        PLF(0, pl_setp_t)(0x17, 0, 0);
        PLF(0, pl_setp_t)(0x15, lw, 0);
        PLF(0, pl_setp_t)(0x08, (const void *)(intptr_t)rows, 0);
        PLF(0, pl_setp_t)(0x10, (const void *)(intptr_t)ci, 0);
        int r = PLF(2, pl_play_t)(ci < 0 ? 0 : ci, pos > 0 ? pos : -1, 0x10002);
        if (r) { *msg = "copiada; play_index fallo"; return -EIO; }
        *msg = "ok (lista copiada a hibymod\\q1)";
        /* pos con estas banderas: SIN VERIFICAR (al encender se usa 0x40000). Si al cargar la pista
         * no esta cerca de pos, se salta como la barra de fabrica */
        if (pos > 3000) {
            for (int t = 0; t < 40 && !QCF(2, qc_i0_t)(); t++) usleep(50000);
            if (QCF(2, qc_i0_t)() && qc_geti(0x06) + 3000 < pos) {
                QCF(0, qc_seek_t)(pos);
                *msg = "ok (lista copiada a hibymod\\q1; posicion con seek)";
            }
        }
    }
    qc_fill(o);
    o->index = at;                           /* donde quedo la nueva */
    o->count = rows;
    return 0;
}

static void qc_result(const char *l)
{
    int f = r_open(QC_RES ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { if (write(f, l, strlen(l)) < 0) {} r_close(f); rename(QC_RES ".tmp", QC_RES); }
    f = r_open("/usr/data/hibymod/disp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f >= 0) { if (write(f, "qc: ", 4) < 0 || write(f, l, strlen(l)) < 0) {} r_close(f); }
}

static void qc_serve(void)
{
    uint32_t t0 = np_now();
    int id = -1, rc = 0, ms = -1, val = INT32_MIN, dup = 0, uns = 0, resh = 0, reb = 0;
    char buf[1400], op[16] = "";
    static char path[1100];
    const char *msg = "";
    struct qc_out o = { 0, 0, -1, -1, -1, 0, 0 };
    path[0] = 0;
    int f = r_open(QC_REQ, O_RDONLY);
    if (f < 0) { rc = -ENOENT; msg = QC_REQ; goto out; }
    struct stat sb;
    if (fstat(f, &sb)) memset(&sb, 0, sizeof sb);
    ssize_t got = read(f, buf, sizeof buf - 1); r_close(f);
    if (got <= 0) { rc = -EINVAL; msg = "peticion vacia"; goto out; }
    buf[got] = 0;
    if (strncmp(buf, "HMQ1\n", 5)) { rc = -EINVAL; msg = "falta HMQ1"; goto out; }
    for (char *l = buf + 5, *nx; l && *l; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        size_t L = strlen(l); if (L && l[L - 1] == '\r') l[--L] = 0;
        if (!strncmp(l, "id=", 3)) id = atoi(l + 3);
        else if (!strncmp(l, "op=", 3)) snprintf(op, sizeof op, "%s", l + 3);
        else if (!strncmp(l, "ms=", 3)) ms = atoi(l + 3);
        else if (!strncmp(l, "val=", 4)) val = atoi(l + 4);
        else if (!strncmp(l, "path=", 5)) snprintf(path, sizeof path, "%s", l + 5);
        else if (!strcmp(l, "dup=1")) dup = 1;
        else if (!strcmp(l, "unscanned=1")) uns = 1;
        else if (!strcmp(l, "reshuffle=1")) resh = 1;
        else if (!strcmp(l, "rebuild=1")) reb = 1;
    }
    /* ya atendida (otra 'Q' sin peticion nueva; cada peticion es un inodo nuevo por el rename) */
    if (sb.st_ino && sb.st_ino == qc_last.st_ino && sb.st_mtim.tv_sec == qc_last.st_mtim.tv_sec &&
        sb.st_mtim.tv_nsec == qc_last.st_mtim.tv_nsec && sb.st_size == qc_last.st_size) return;
    qc_last = sb;
    int isvol = !strcmp(op, "vol") || !strcmp(op, "volget");
    if ((rc = qc_check()) || (isvol && (rc = qc_check_settings()))) {
        msg = rc == -ENOEXEC ? "firmware distinto: no se llama a nada" : "reproductor sin iniciar"; goto out;
    }
    if (!strcmp(op, "check")) {
        static hw16 cur[0x104]; static char cm[200];
        qc_curlist(cur);
        int k = 0; char cu[64]; for (; k < 63 && cur[k]; k++) cu[k] = cur[k] < 0x80 ? (char)cur[k] : '?';
        cu[k] = 0;
        qc_fill(&o);
        snprintf(cm, sizeof cm, "firmas ok; modo=%d lista='%s' activa=%d", qc_geti(0x0a), cu, QCF(2, qc_i0_t)());
        msg = cm; goto out;
    }
    if (!strcmp(op, "pos")) { qc_fill(&o); msg = "ok"; goto out; }
    if (!strcmp(op, "seek")) { rc = qc_seek(ms, &o, &msg); goto out; }
    if (!strcmp(op, "next") || !strcmp(op, "append")) {
        if (!path[0]) { rc = -EINVAL; msg = "falta path="; goto out; }
        struct pl_ent e = { path, -1 };
        char *bar = strchr(path, '|');
        if (bar && bar - path < 6 && bar > path && ((path[0] >= '0' && path[0] <= '9') || path[0] == '-')) { e.cue = atoi(path); e.path = bar + 1; }
        if (__sync_lock_test_and_set(&pl_busy, 1)) { rc = -EBUSY; msg = "hay una orden 'L' en curso"; goto out; }
        rc = qc_queue(&e, op[0] == 'a', dup, uns, resh, reb, &o, &msg);
        __sync_synchronize();
        pl_busy = 0;
        goto out;
    }
    if (isvol) {
        const struct st_opt *vo = st_find("volume");
        o.max = st_get(ST_ID_VOLMAX);
        if (!vo) { rc = -ENOENT; msg = "volume"; goto out; }
        if (!strcmp(op, "volget")) { o.val = st_get(vo->id); msg = st_get(ST_ID_LINEOUT) == 1 ? "ok (line out: volumen fijo)" : "ok"; goto out; }
        if (val == INT32_MIN) { rc = -EINVAL; msg = "falta val="; goto out; }
        uint32_t now = np_now();             /* como mucho una escritura de user.ini cada 50 ms */
        if (qc_vol_ms && now - qc_vol_ms < 50) usleep((50 - (now - qc_vol_ms)) * 1000);
        int v = 0;
        rc = st_set(vo, val, 0, &v, &msg);
        qc_vol_ms = np_now();
        o.val = v;
        goto out;
    }
    rc = -EINVAL; msg = "op desconocida";
out:;
    char l[400];
    snprintf(l, sizeof l, "id=%d rc=%d op=%s pos=%d dur=%d state=%d index=%d count=%d val=%d max=%d ms=%u %s\n",
             id, rc, op[0] ? op : "-", o.pos, o.dur, o.state, o.index, o.count, o.val, o.max, np_now() - t0, msg);
    qc_result(l);
}

static void *qc_thread(void *arg)
{
    (void)arg;
    for (;;) {
        while (__sync_lock_test_and_set(&qc_again, 0)) qc_serve();
        __sync_lock_release(&qc_busy);
        __sync_synchronize();
        if (!qc_again || __sync_lock_test_and_set(&qc_busy, 1)) break;   /* una 'Q' justo al soltar */
    }
    return 0;
}

static void qc_kick(void)
{
    qc_again = 1;
    __sync_synchronize();
    if (__sync_lock_test_and_set(&qc_busy, 1)) return;    /* el hilo en curso la atendera */
    pthread_attr_t a; pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);    /* play_index usa marcos de ~3 KB, SQLite mas */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, qc_thread, 0)) {
        qc_again = 0; __sync_lock_release(&qc_busy);
        qc_result("id=-1 rc=-11 op=- pos=0 dur=0 state=-1 index=-1 count=-1 val=0 max=0 ms=0 pthread_create\n");
    }
    pthread_attr_destroy(&a);
}

/* volumen actual del reproductor para el shell: lectura directa de su estructura de ajustes
 * (volume 0x23c, volmax 0x238; docs/research_settings_api.md). La comprobacion de que la
 * estructura esta mapeada se hace una vez (leer /proc/self/maps en cada fotograma es caro). */
static uint32_t vol_base;
static void vol_publish(void)
{
    if (!vol_base) {
        static int tries; if (tries > 400) return;
        if (++tries % 25) return;                                     /* unos 2 intentos por segundo */
        if (!pl_mapped(HP_SETTINGS, 4, 0)) return;
        uint32_t s = *(const uint32_t *)(uintptr_t)HP_SETTINGS;
        if (!s || !pl_mapped(s, 0xad0, 0)) return;
        vol_base = s;
    }
    int v = *(const volatile int *)(uintptr_t)(vol_base + 0x23c), m = *(const volatile int *)(uintptr_t)(vol_base + 0x238);
    if (v >= 0 && v <= 255) st->vol_now = (uint32_t)v + 1;
    if (m > 0 && m <= 255) st->vol_max = (uint32_t)m + 1;
}

/* ================================================================ actualizar la biblioteca ('U')
 * HiBy no reescanea la tarjeta salvo con "Actualizar base de datos" (Ajustes de reproduccion) o con
 * la actualizacion automatica al insertar la SD. Ese mismo escaneo se pide con el mensaje de la
 * interfaz 0x125 (lo que hace "startScanMusic" de HiBy Link; docs/research_library_rescan.md):
 * su manejador abre music_update.dlg y lanza el hilo de escaneo EN EL HILO DE LA INTERFAZ (llamar
 * a las funciones desde aqui no es seguro). Estado del escaneo: palabra 0x93bed8 (1 libre, 0
 * escaneando). OJO: el escaneo para la musica y vacia la cola (lo hace HiBy).
 * Resultado en LU_RES: "posted" / "busy" / "nosd" / "nosig" y, al acabar, "done" (o "timeout"). */
#define LU_RES "/tmp/hibymod_libscan"
#define LU_IDLE 0x93bed8u
static void lu_write(const char *m)
{
    int f = r_open(LU_RES ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { if (write(f, m, strlen(m)) < 0) {} r_close(f); rename(LU_RES ".tmp", LU_RES); }
    int g = r_open("/usr/data/hibymod/disp.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g >= 0) { char l[80]; int n = snprintf(l, sizeof l, "lu: %s\n", m); if (write(g, l, n) < 0) {} r_close(g); }
}
static void *lu_watch(void *a)
{
    (void)a;
    volatile const int32_t *idle = (volatile const int32_t *)(uintptr_t)LU_IDLE;
    int started = 0;
    for (int i = 0; i < 100 && !started; i++) { if (*idle == 0) started = 1; else usleep(30000); }   /* 3 s para empezar */
    if (!started) { lu_write("done"); return 0; }                 /* tan rapido que no se vio (o no arranco) */
    for (int i = 0; i < 18000 && *idle == 0; i++) usleep(100000);  /* hasta 30 min */
    lu_write(*idle == 0 ? "timeout" : "done");
    return 0;
}
static void lu_kick(void)
{
    static const uint32_t sig_idle[4] = { 0x3c020094, 0x03e00008, 0x8c42bed8, 0x00000000 };   /* scan_idle() 0x507620 */
    if (st_check() != 0 || !pl_mapped(0x507620, 16, 1) || memcmp((const void *)(uintptr_t)0x507620, sig_idle, 16) ||
        !pl_mapped(0x7f88c0, 8, 0) || ((const uint32_t *)(uintptr_t)0x7f88c0)[0] != 0x125 || ((const uint32_t *)(uintptr_t)0x7f88c0)[1] != 0x464c40 ||
        !pl_mapped(LU_IDLE, 4, 0)) { lu_write("nosig"); return; }
    if (*(volatile const int32_t *)(uintptr_t)LU_IDLE != 1) { lu_write("busy"); return; }
    { /* sin tarjeta montada el escaneo encuentra 0 pistas y vacia la biblioteca */
        int ok = 0; FILE *m = r_fopen64("/proc/mounts", "r");
        if (m) { char l[256]; while (fgets(l, sizeof l, m)) if (strstr(l, " /usr/data/mnt/sd_0 ")) ok = 1; fclose(m); }
        if (!ok) { lu_write("nosd"); return; }
    }
    STF(3, st_v1_t)(0x125);
    lu_write("posted");
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, lu_watch, 0)) lu_write("done");
    pthread_attr_destroy(&at);
}
