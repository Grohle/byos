/* hibyctl: utilidades de bajo nivel para hibymod (estatico, MIPS32 little endian).
 *   hibyctl tap X Y                 toque corto (pixeles de pantalla 320x480)
 *   hibyctl hold X Y MS             pulsacion larga
 *   hibyctl swipe X1 Y1 X2 Y2 [MS]  deslizamiento
 *   hibyctl key CODE [eventN]       pulsa una tecla (por defecto event0; 116 = encendido)
 *   hibyctl grabtest /dev/input/eventN
 *   hibyctl info
 * El protocolo tactil replica el del driver goodix-ts capturado en el aparato:
 *   BTN_TOUCH, ABS_MT_POSITION_X/Y, ABS_MT_TOUCH_MAJOR, ABS_MT_WIDTH_MAJOR,
 *   ABS_MT_TRACKING_ID, SYN_MT_REPORT, SYN_REPORT.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <sys/mman.h>

/* Captura el fotograma visible (doble bufer) a un fichero RGB565 crudo de 320x480. */
static int shot(const char *out, int which)
{
    int f = open("/dev/fb0", O_RDONLY);
    if (f < 0) { perror("fb0"); return 2; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
    if (ioctl(f, FBIOGET_VSCREENINFO, &v) || ioctl(f, FBIOGET_FSCREENINFO, &x)) { perror("ioctl fb"); return 2; }
    size_t frame = (size_t)x.line_length * v.yres;
    unsigned char *m = mmap(0, x.smem_len, PROT_READ, MAP_SHARED, f, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    size_t off = which >= 0 ? (size_t)which * frame : (size_t)v.yoffset * x.line_length;
    if (off + frame > x.smem_len) off = 0;
    FILE *o = fopen(out, "wb");
    if (!o) { perror(out); return 2; }
    fwrite(m + off, 1, frame, o); fclose(o);
    fprintf(stderr, "smem=%u line=%u yres=%u yoffset=%u frames=%u -> offset %u\n",
            x.smem_len, x.line_length, v.yres, v.yoffset, (unsigned)(x.smem_len / frame), (unsigned)off);
    return 0;
}

#define TS "/dev/input/event1"

static int fd = -1;
static struct input_event buf[32];
static int nbuf;

static void ev(int type, int code, int value)
{
    struct input_event *e = &buf[nbuf++];
    memset(e, 0, sizeof *e);
    e->type = type; e->code = code; e->value = value;
}
static void flush(void)
{
    if (nbuf && write(fd, buf, nbuf * sizeof buf[0]) != (ssize_t)(nbuf * sizeof buf[0]))
        perror("write");
    nbuf = 0;
}
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }

static void contact(int x, int y, int down)
{
    if (down) ev(EV_KEY, BTN_TOUCH, 1);
    ev(EV_ABS, ABS_MT_POSITION_X, x); ev(EV_ABS, ABS_MT_POSITION_Y, y);
    ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 22); ev(EV_ABS, ABS_MT_WIDTH_MAJOR, 22);
    ev(EV_ABS, ABS_MT_TRACKING_ID, 0);
    ev(EV_SYN, SYN_MT_REPORT, 0); ev(EV_SYN, SYN_REPORT, 0);
    flush();
}
static void release(void)
{
    ev(EV_KEY, BTN_TOUCH, 0); ev(EV_SYN, SYN_MT_REPORT, 0); ev(EV_SYN, SYN_REPORT, 0);
    flush();
}

static int open_dev(const char *dev)
{
    fd = open(dev, O_WRONLY);
    if (fd < 0) { perror(dev); return 0; }
    return 1;
}

static int grabtest(const char *dev)
{
    int f = open(dev, O_RDONLY);
    if (f < 0) { perror(dev); return 2; }
    char name[64] = "?";
    ioctl(f, EVIOCGNAME(sizeof name), name);
    if (ioctl(f, EVIOCGRAB, 1) == 0) { printf("%s (%s): libre\n", dev, name); ioctl(f, EVIOCGRAB, 0); }
    else printf("%s (%s): EN EXCLUSIVA (%s)\n", dev, name, strerror(errno));
    close(f);
    return 0;
}

/* Mide el coste de leer N paginas de 4 KB repartidas por un fichero (tras vaciar la cache
 * si se pide). Simula fallos de pagina del codigo del reproductor. */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int readbench(const char *path, int n, int drop)
{
    if (drop) { sync(); int f = open("/proc/sys/vm/drop_caches", O_WRONLY); if (f >= 0) { write(f, "3", 1); close(f); } }
    int fd2 = open(path, O_RDONLY);
    if (fd2 < 0) { perror(path); return 2; }
    off_t size = lseek(fd2, 0, SEEK_END);
    long pages = size / 4096;
    static char b[4096];
    double worst = 0, t0 = now_ms();
    for (int i = 0; i < n; i++) {
        off_t off = (off_t)((i * 7919L) % pages) * 4096;
        double a = now_ms();
        pread(fd2, b, 4096, off);
        double d = now_ms() - a;
        if (d > worst) worst = d;
    }
    double tot = now_ms() - t0;
    printf("%s: %d paginas en %.1f ms -> media %.2f ms, peor %.2f ms%s\n", path, n, tot, tot / n, worst, drop ? " (en frio)" : " (en cache)");
    close(fd2);
    return 0;
}

/* Paginas de un fichero presentes en la cache (mincore). Con -v lista los rangos. */
static int resident(const char *path, int verbose)
{
    int f = open(path, O_RDONLY);
    if (f < 0) { perror(path); return 2; }
    off_t size = lseek(f, 0, SEEK_END);
    if (size <= 0) { close(f); return 0; }
    void *m = mmap(0, size, PROT_READ, MAP_SHARED, f, 0);
    if (m == MAP_FAILED) { perror("mmap"); close(f); return 2; }
    long pages = (size + 4095) / 4096;
    unsigned char *v = malloc(pages);
    if (!v || mincore(m, size, v)) { perror("mincore"); return 2; }
    long n = 0;
    for (long i = 0; i < pages; i++) n += v[i] & 1;
    printf("%-44s %6ld/%6ld paginas residentes (%ld KB de %ld KB)\n", path, n, pages, n * 4, pages * 4);
    if (verbose)
        for (long i = 0; i < pages; ) {
            if (!(v[i] & 1)) { i++; continue; }
            long j = i; while (j < pages && (v[j] & 1)) j++;
            printf("  %ld-%ld\n", i, j - 1); i = j;
        }
    munmap(m, size); close(f); free(v);
    return 0;
}

/* ---- fijador de paginas calientes (como el "pinner service" de Android) ----
 * pinprofile OUT MAXKB FICHERO...  guarda los rangos residentes ("ruta inicio fin" en paginas),
 *                                   uniendo huecos < 3 paginas, hasta MAXKB en total
 * pin LISTA                         mapea y bloquea (mlock) esos rangos y se queda dormido */
static int pinprofile(const char *out, long maxkb, int n, char **files)
{
    FILE *o = fopen(out, "w");
    if (!o) { perror(out); return 2; }
    long total = 0;
    for (int k = 0; k < n; k++) {
        int f = open(files[k], O_RDONLY);
        if (f < 0) continue;
        off_t size = lseek(f, 0, SEEK_END);
        void *m = size > 0 ? mmap(0, size, PROT_READ, MAP_SHARED, f, 0) : MAP_FAILED;
        if (m == MAP_FAILED) { close(f); continue; }
        long pages = (size + 4095) / 4096;
        unsigned char *v = malloc(pages);
        if (v && !mincore(m, size, v)) {
            for (long i = 0; i < pages; ) {
                if (!(v[i] & 1)) { i++; continue; }
                long j = i, last = i;
                while (j < pages && (j - last) < 3) { if (v[j] & 1) last = j; j++; }
                long len = last - i + 1;
                if ((total + len) * 4 > maxkb) break;
                fprintf(o, "%s %ld %ld\n", files[k], i, last);
                total += len; i = last + 1;
            }
        }
        free(v); munmap(m, size); close(f);
    }
    fclose(o);
    printf("perfil: %ld KB en %s\n", total * 4, out);
    return 0;
}

static int pin(const char *list)
{
    FILE *l = fopen(list, "r");
    if (!l) { perror(list); return 2; }
    char path[256]; long a, b, total = 0, failed = 0;
    while (fscanf(l, "%255s %ld %ld", path, &a, &b) == 3) {
        int f = open(path, O_RDONLY);
        if (f < 0) { failed++; continue; }
        off_t size = lseek(f, 0, SEEK_END);
        if ((b + 1) * 4096L > size + 4095) { close(f); failed++; continue; }
        size_t len = (size_t)(b - a + 1) * 4096;
        void *m = mmap(0, len, PROT_READ, MAP_SHARED, f, (off_t)a * 4096);
        close(f);
        if (m == MAP_FAILED) { failed++; continue; }
        if (mlock(m, len)) { failed++; munmap(m, len); continue; }
        total += len / 1024;
    }
    fclose(l);
    printf("fijados %ld KB (%ld rangos fallidos)\n", total, failed);
    fflush(stdout);
    for (;;) pause();
}

/* Pinta un fichero RGB565 crudo de 320x480 en los dos bufers de pantalla (prueba de
 * convivencia con hiby_player). Con "watch S" vigila S segundos y cuenta cuantas veces
 * el reproductor sobrescribe la imagen (compara un hash del bufer visible). */
static unsigned fbhash(const unsigned char *p, size_t n) { unsigned h = 2166136261u; for (size_t i = 0; i < n; i += 64) h = (h ^ p[i]) * 16777619u; return h; }
static int paint(const char *rawfile, int watch)
{
    int f = open("/dev/fb0", O_RDWR);
    if (f < 0) { perror("fb0"); return 2; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
    ioctl(f, FBIOGET_VSCREENINFO, &v); ioctl(f, FBIOGET_FSCREENINFO, &x);
    size_t frame = (size_t)x.line_length * v.yres;
    unsigned char *m = mmap(0, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    FILE *r = fopen(rawfile, "rb");
    if (!r) { perror(rawfile); return 2; }
    static unsigned char img[320 * 480 * 2];
    size_t got = fread(img, 1, sizeof img, r); fclose(r);
    if (got != frame) { fprintf(stderr, "tamano %zu != %zu\n", got, frame); return 2; }
    for (unsigned k = 0; k * frame < x.smem_len; k++) memcpy(m + k * frame, img, frame);
    unsigned want = fbhash(img, frame);
    printf("pintado (yoffset=%u)\n", v.yoffset); fflush(stdout);
    double t0 = now_ms(); int over = 0; unsigned last = want;
    while (now_ms() - t0 < watch * 1000.0) {
        ioctl(f, FBIOGET_VSCREENINFO, &v);
        unsigned h = fbhash(m + (size_t)v.yoffset * x.line_length, frame);
        if (h != last) { if (h != want) { over++; printf("  sobrescrito a los %.1f s\n", (now_ms() - t0) / 1000); fflush(stdout); } last = h; }
        struct timespec s = {0, 20000000}; nanosleep(&s, 0);
    }
    printf("veces que el reproductor repinto: %d\n", over);
    return 0;
}

/* Prueba del control de pantalla (hibymod_disp.so v2): toma la pantalla, pinta RAW en el
 * bufer que el reproductor no esta usando, lo presenta, espera S segundos y la devuelve. */
struct fbstate2 { volatile uint32_t magic, held, waiting, yoffset, frames; };
static int takeover(const char *rawfile, int secs)
{
    int s = open("/dev/shm/hibymod_fbstate", O_RDWR);
    if (s < 0) { perror("hibymod_fbstate (esta cargada la libreria?)"); return 2; }
    struct fbstate2 *st = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s, 0);
    close(s);
    if (st == MAP_FAILED || (st->magic != 0x32464d48 && st->magic != 0x33464d48)) { fprintf(stderr, "estado no valido\n"); return 2; }
    int fifo = open("/tmp/hibymod_disp", O_WRONLY | O_NONBLOCK);
    if (fifo < 0) { perror("fifo"); return 2; }
    unsigned f0 = st->frames;
    write(fifo, "H", 1);
    double t0 = now_ms();
    while (!st->waiting && now_ms() - t0 < 2500) { struct timespec q = {0, 10000000}; nanosleep(&q, 0); }
    printf("hold: reproductor %s (yoffset pedido %u, fotogramas %u)\n",
           st->waiting ? "en espera" : "sin redibujar", st->yoffset, st->frames - f0);
    int f = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
    ioctl(f, FBIOGET_VSCREENINFO, &v); ioctl(f, FBIOGET_FSCREENINFO, &x);
    size_t frame = (size_t)x.line_length * v.yres;
    unsigned char *m = mmap(0, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    unsigned busy = st->waiting ? st->yoffset : 0xffffffff;      /* bufer con el fotograma pendiente */
    unsigned target = (busy == 0xffffffff) ? v.yoffset : (busy ? 0 : v.yres);
    static unsigned char img[320 * 480 * 2];
    FILE *r = fopen(rawfile, "rb"); fread(img, 1, sizeof img, r); fclose(r);
    memcpy(m + (size_t)target * x.line_length, img, frame);
    v.yoffset = target;
    double a = now_ms();
    int rc = ioctl(f, FBIOPAN_DISPLAY, &v);
    printf("launcher presentado en el bufer y=%u (rc=%d, %.1f ms)\n", target, rc, now_ms() - a);
    fflush(stdout);
    sleep(secs);
    write(fifo, "R", 1);
    t0 = now_ms();
    while (st->waiting && now_ms() - t0 < 2000) { struct timespec q = {0, 5000000}; nanosleep(&q, 0); }
    printf("devuelto: reproductor %s, fotogramas durante la prueba %u\n", st->waiting ? "AUN EN ESPERA" : "continua", st->frames - f0);
    return 0;
}

/* Prueba del modo captura (hibymod_disp.so v3): congela la pantalla, toca X,Y, espera a que
 * el reproductor termine de dibujar la pantalla nueva (a escondidas) y la anima subiendo
 * desde abajo sobre la anterior, que se oscurece. Al final devuelve el control. */
struct fbstate3 {
    volatile uint32_t magic, held, waiting, yoffset, frames, input_mode, view_seq;
    char view[96];
    volatile uint32_t capture, cap_frames;
};
static inline uint16_t dim565(uint16_t p, int k)   /* k = 0..32 (32 = sin cambio) */
{
    unsigned r = (p >> 11) * k >> 5, g = ((p >> 5) & 63) * k >> 5, b = (p & 31) * k >> 5;
    return (uint16_t)(r << 11 | g << 5 | b);
}
static int sheet(int tx, int ty, int dur)
{
    const size_t W = 320, H = 480, FS = W * H * 2;
    int s = open("/dev/shm/hibymod_fbstate", O_RDWR);
    if (s < 0) { perror("hibymod_fbstate"); return 2; }
    struct fbstate3 *st = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s, 0);
    close(s);
    if (st == MAP_FAILED || st->magic != 0x33464d48) { fprintf(stderr, "libreria v3 no cargada\n"); return 2; }
    int ff = open("/dev/shm/hibymod_frames", O_RDWR | O_CREAT, 0600);
    if (ff < 0 || ftruncate(ff, 2 * FS)) { perror("frames"); return 2; }
    uint16_t *frames = mmap(0, 2 * FS, PROT_READ | PROT_WRITE, MAP_SHARED, ff, 0);
    close(ff);
    int f = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
    ioctl(f, FBIOGET_VSCREENINFO, &v); ioctl(f, FBIOGET_FSCREENINFO, &x);
    uint16_t *fb = mmap(0, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    if (frames == MAP_FAILED || fb == MAP_FAILED) { perror("mmap"); return 2; }
    int fifo = open("/tmp/hibymod_disp", O_WRONLY | O_NONBLOCK);
    if (fifo < 0) { perror("fifo"); return 2; }

    double t0 = now_ms();
    write(fifo, "C", 1);
    while (!st->capture && now_ms() - t0 < 300) msleep(5);
    if (!st->capture) { fprintf(stderr, "el modo captura no arranco\n"); return 2; }
    if (!open_dev(TS)) { write(fifo, "R", 1); return 2; }
    contact(tx, ty, 1); msleep(40); contact(tx, ty, 0); msleep(40); release();
    /* esperar a que el reproductor deje de dibujar: 150 ms sin fotogramas nuevos */
    double t1 = now_ms(), quiet = now_ms(); unsigned last = st->cap_frames;
    while (now_ms() - t1 < 2000) {
        msleep(10);
        if (st->cap_frames != last) { last = st->cap_frames; quiet = now_ms(); }
        else if (last && now_ms() - quiet > 150) break;
    }
    double t_ready = now_ms() - t0;
    write(fifo, "H", 1);
    msleep(20);
    static uint16_t oldi[320 * 480], newi[320 * 480];
    memcpy(oldi, frames, FS); memcpy(newi, frames + W * H, FS);
    unsigned T = st->yoffset;                 /* bufer que el reproductor cree visible */
    unsigned bufs[2] = { T ? 0 : H, T };
    int n = 0; double a0 = now_ms(), worst = 0, prev = a0;
    for (;;) {
        double t = (now_ms() - a0) / dur; if (t > 1) t = 1;
        double e = 1 - (1 - t) * (1 - t) * (1 - t);           /* ease-out cubico */
        int y0 = (int)(H * (1 - e) + 0.5);
        int k = 32 - (int)(11 * e);                             /* fondo hasta ~65 % */
        unsigned yo = bufs[n & 1];
        uint16_t *d = fb + yo * W;
        for (int y = 0; y < (int)H; y++) {
            uint16_t *row = d + y * W;
            if (y < y0) { const uint16_t *o = oldi + y * W; for (size_t i = 0; i < W; i++) row[i] = dim565(o[i], k); continue; }
            int r = y - y0; const uint16_t *nw = newi + r * W;
            memcpy(row, nw, W * 2);
            if (r < 14 && y0 > 0) {                              /* esquinas redondeadas de la hoja */
                int c = 14 - r, cut = 14 - (int)(__builtin_sqrt((double)(14 * 14 - c * c)) + 0.5);
                const uint16_t *o = oldi + y * W;
                for (int i = 0; i < cut; i++) { row[i] = dim565(o[i], k); row[W - 1 - i] = dim565(o[W - 1 - i], k); }
            }
        }
        v.yoffset = yo; ioctl(f, FBIOPAN_DISPLAY, &v);
        double now = now_ms(); if (now - prev > worst) worst = now - prev; prev = now; n++;
        if (t >= 1) break;
    }
    /* final: la pantalla nueva en los dos bufers y visible el que espera el reproductor */
    memcpy(fb + bufs[0] * W, newi, FS); memcpy(fb + bufs[1] * W, newi, FS);
    v.yoffset = T; ioctl(f, FBIOPAN_DISPLAY, &v);
    write(fifo, "R", 1);
    printf("pantalla nueva lista en %.0f ms (%u fotogramas del reproductor); animacion %d fotogramas en %.0f ms (%.1f fps, peor %.1f ms)\n",
           t_ready, last, n, now_ms() - a0 , n * 1000.0 / (now_ms() - a0), worst);
    return 0;
}

/* Graba cada fotograma presentado (reproductor o shell) a media resolucion durante MS:
 * fichero = N x { uint32 t_ms, uint32 yoffset, uint16 px[160*240] }. Para revisar animaciones. */
static int rec(int ms, const char *out, int maxn)
{
    int s = open("/dev/shm/hibymod_fbstate", O_RDONLY);
    if (s < 0) { perror("estado"); return 2; }
    volatile uint32_t *stt = mmap(0, 4096, PROT_READ, MAP_SHARED, s, 0);
    int f = open("/dev/fb0", O_RDONLY);
    struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
    ioctl(f, FBIOGET_VSCREENINFO, &v); ioctl(f, FBIOGET_FSCREENINFO, &x);
    const uint16_t *fbm = mmap(0, x.smem_len, PROT_READ, MAP_SHARED, f, 0);
    FILE *o = fopen(out, "wb"); if (!o) { perror(out); return 2; }
    static uint16_t half[160 * 240];
    uint32_t lf = stt[4], lp = stt[8], ly = stt[3]; int n = 0;
    double t0 = now_ms();
    while (now_ms() - t0 < ms && n < maxn) {
        uint32_t fr = stt[4], pr = stt[8];
        struct fb_var_screeninfo vv; ioctl(f, FBIOGET_VSCREENINFO, &vv); uint32_t yo = vv.yoffset;
        if (fr != lf || pr != lp || yo != ly || n == 0) {
            lf = fr; lp = pr; ly = yo;
            const uint16_t *b = fbm + (size_t)yo * 320;
            for (int yy = 0; yy < 240; yy++) for (int xx = 0; xx < 160; xx++) half[yy * 160 + xx] = b[(yy * 2) * 320 + xx * 2];
            uint32_t hd[2] = { (uint32_t)(now_ms() - t0), yo };
            fwrite(hd, 4, 2, o); fwrite(half, 2, 160 * 240, o); n++;
        }
        struct timespec q = { 0, 1500000 }; nanosleep(&q, 0);
    }
    fclose(o);
    printf("%d fotogramas en %d ms\n", n, ms);
    return 0;
}

/* Mide fotogramas presentados: cuenta cambios del bufer visible (yoffset) durante MS. */
static int fps(int ms)
{
    int f = open("/dev/fb0", O_RDONLY);
    if (f < 0) { perror("fb0"); return 2; }
    struct fb_var_screeninfo v;
    ioctl(f, FBIOGET_VSCREENINFO, &v);
    unsigned last = v.yoffset; int flips = 0, n = 0;
    static double iv[4096]; static struct timeval at[4096];
    double t0 = now_ms(), lastflip = t0;
    while (now_ms() - t0 < ms) {
        ioctl(f, FBIOGET_VSCREENINFO, &v);
        if (v.yoffset != last) {
            double t = now_ms();
            if (flips && n < 4096) { gettimeofday(&at[n], 0); iv[n++] = t - lastflip; }
            flips++; last = v.yoffset; lastflip = t;
        }
        struct timespec s = {0, 500000}; nanosleep(&s, 0);
    }
    /* solo intervalos "en movimiento" (< 250 ms); los mayores son pausas entre gestos */
    double act[4096]; int m = 0; double sum = 0;
    for (int i = 0; i < n; i++) if (iv[i] < 250) { act[m++] = iv[i]; sum += iv[i]; }
    for (int i = 1; i < m; i++) { double x = act[i]; int j = i - 1; while (j >= 0 && act[j] > x) { act[j + 1] = act[j]; j--; } act[j + 1] = x; }
    double med = m ? act[m / 2] : 0, p95 = m ? act[(m * 95) / 100] : 0, worst = m ? act[m - 1] : 0;
    int hitches = 0; for (int i = 0; i < m; i++) if (act[i] > 2.5 * med && act[i] > 40) hitches++;
    printf("en movimiento: %d fotogramas, %.1f fps (mediana %.1f ms, p95 %.1f ms, peor %.0f ms), tirones: %d\n",
           m, m ? 1000.0 * m / sum : 0, med, p95, worst, hitches);
    if (getenv("FPS_ALL"))
        for (int i = 0; i < n; i++) {
            struct tm tmv; time_t sec = at[i].tv_sec; localtime_r(&sec, &tmv);
            printf("  %02d:%02d.%03d  %5.1f ms\n", tmv.tm_min, tmv.tm_sec, (int)(at[i].tv_usec / 1000), iv[i]);
        }
    if (getenv("FPS_TIMES"))
        for (int i = 0; i < n; i++)
            if (iv[i] < 250 && iv[i] > 2.5 * med && iv[i] > 40) {
                struct tm tmv; time_t sec = at[i].tv_sec; localtime_r(&sec, &tmv);
                printf("  tiron %3.0f ms terminado a las %02d:%02d:%02d.%03d\n", iv[i], tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(at[i].tv_usec / 1000));
            }
    close(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) goto usage;
    const char *c = argv[1];
    if (!strcmp(c, "info")) { printf("hibyctl ok (input_event=%d bytes)\n", (int)sizeof(struct input_event)); return 0; }
    if (!strcmp(c, "grabtest") && argc >= 3) return grabtest(argv[2]);
    if (!strcmp(c, "takeover") && argc >= 3) return takeover(argv[2], argc >= 4 ? atoi(argv[3]) : 5);
    /* pantime N: duracion de N presentaciones del mismo bufer con esperas aleatorias entre medias.
     * Constante ~= transferencia a un panel con memoria (SLCD); repartida 0..periodo = espera de vsync. */
    if (!strcmp(c, "pantime") && argc >= 3) {
        int f = open("/dev/fb0", O_RDWR), n = atoi(argv[2]);
        struct fb_var_screeninfo v; ioctl(f, FBIOGET_VSCREENINFO, &v);
        unsigned seed = 12345;
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u; msleep(5 + (seed >> 16) % 23);
            double a = now_ms(); ioctl(f, FBIOPAN_DISPLAY, &v); printf("%.1f ", now_ms() - a);
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "rec") && argc >= 4) return rec(atoi(argv[2]), argv[3], argc >= 5 ? atoi(argv[4]) : 80);
    if (!strcmp(c, "sheet") && argc >= 4) return sheet(atoi(argv[2]), atoi(argv[3]), argc >= 5 ? atoi(argv[4]) : 320);
    if (!strcmp(c, "paint") && argc >= 3) return paint(argv[2], argc >= 4 ? atoi(argv[3]) : 0);
    if (!strcmp(c, "fps") && argc >= 3) return fps(atoi(argv[2]));
    if (!strcmp(c, "pinprofile") && argc >= 5) return pinprofile(argv[2], atol(argv[3]), argc - 4, argv + 4);
    if (!strcmp(c, "pin") && argc >= 3) return pin(argv[2]);
    if (!strcmp(c, "resident") && argc >= 3) { int r = 0; for (int i = 2; i < argc; i++) if (strcmp(argv[i], "-v")) r |= resident(argv[i], !strcmp(argv[argc - 1], "-v")); return r; }
    if (!strcmp(c, "readbench") && argc >= 4) return readbench(argv[2], atoi(argv[3]), argc >= 5 && !strcmp(argv[4], "cold"));
    if (!strcmp(c, "shot") && argc >= 3) return shot(argv[2], argc >= 4 ? atoi(argv[3]) : -1);
    if (!strcmp(c, "tap") && argc >= 4) {
        int x = atoi(argv[2]), y = atoi(argv[3]);
        if (!open_dev(TS)) return 2;
        contact(x, y, 1); msleep(40); contact(x, y, 0); msleep(40); release();
        return 0;
    }
    if (!strcmp(c, "hold") && argc >= 5) {
        int x = atoi(argv[2]), y = atoi(argv[3]), ms = atoi(argv[4]);
        if (!open_dev(TS)) return 2;
        contact(x, y, 1);
        for (int t = 0; t < ms; t += 30) { msleep(30); contact(x, y, 0); }
        release();
        return 0;
    }
    if (!strcmp(c, "swipe") && argc >= 6) {
        int x1 = atoi(argv[2]), y1 = atoi(argv[3]), x2 = atoi(argv[4]), y2 = atoi(argv[5]);
        int ms = argc >= 7 ? atoi(argv[6]) : 250, steps = ms / 16; if (steps < 4) steps = 4;
        if (!open_dev(TS)) return 2;
        contact(x1, y1, 1);
        for (int i = 1; i <= steps; i++) { msleep(ms / steps); contact(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 0); }
        /* argumento opcional: mantener el dedo quieto al final (para capturar a mitad de gesto) */
        for (int t = 0, hold = argc >= 8 ? atoi(argv[7]) : 0; t < hold; t += 30) { msleep(30); contact(x2, y2, 0); }
        msleep(20); release();
        return 0;
    }
    /* drag X1 Y1 X2 Y2 ESPERA_MS MOVER_MS: pulsar, esperar quieto y luego arrastrar (edicion del inicio) */
    if (!strcmp(c, "drag") && argc >= 8) {
        int x1 = atoi(argv[2]), y1 = atoi(argv[3]), x2 = atoi(argv[4]), y2 = atoi(argv[5]), wait = atoi(argv[6]), ms = atoi(argv[7]);
        if (!open_dev(TS)) return 2;
        contact(x1, y1, 1);
        for (int t = 0; t < wait; t += 30) { msleep(30); contact(x1, y1, 0); }
        int steps = ms / 20; if (steps < 4) steps = 4;
        for (int i = 1; i <= steps; i++) { msleep(ms / steps); contact(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 0); }
        for (int t = 0; t < 200; t += 30) { msleep(30); contact(x2, y2, 0); }
        release();
        return 0;
    }
    if (!strcmp(c, "key") && argc >= 3) {
        char dev[64] = "/dev/input/event0";
        if (argc >= 4) snprintf(dev, sizeof dev, "/dev/input/%s", argv[3]);
        if (!open_dev(dev)) return 2;
        int code = atoi(argv[2]);
        ev(EV_KEY, code, 1); ev(EV_SYN, SYN_REPORT, 0); flush(); msleep(80);
        ev(EV_KEY, code, 0); ev(EV_SYN, SYN_REPORT, 0); flush();
        return 0;
    }
usage:
    fprintf(stderr, "uso: hibyctl tap X Y | hold X Y MS | swipe X1 Y1 X2 Y2 [MS] | key CODE [eventN] | grabtest DEV | info\n");
    return 1;
}
