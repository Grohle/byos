/* hibyboot - animacion de arranque de ByOS (formato HBA1, ver tools/bootanim.py).
 *
 * Lo lanza S11jpeg_display_shell en cuanto existe /dev/fb0. Reproduce la intro una vez
 * y repite el bucle hasta que el inicio esta listo (el shell crea /tmp/hibyos_boot_stop; la
 * libreria de pantalla retiene al reproductor mientras tanto); entonces funde hacia el fotograma de
 * reposo (identico al boot_animation/0.png del reproductor) y sale, sin salto visible.
 * Panel SLCD con GRAM propia: SIEMPRE escribir el bufer y despues hacer el pan.
 * Uso: hibyboot [fichero.hba]   (por defecto /usr/share/hibyos/hibyos.hba) */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int player_running(void)
{
    DIR *d = opendir("/proc"); struct dirent *e; int found = 0;
    while (d && !found && (e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[64], b[128]; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        int f = open(p, O_RDONLY); if (f < 0) continue;
        int n = (int)read(f, b, sizeof b - 1); close(f);
        if (n > 0) { b[n] = 0; if (strstr(b, "hiby_player") && !strstr(b, ".sh")) found = 1; }
    }
    if (d) closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/usr/share/hibyos/hibyos.hba";
    double test_s = argc > 2 ? atof(argv[2]) : 0;   /* prueba: ignora el reproductor y para a los N s */
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) return 1;
    const uint8_t *m = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED || memcmp(m, "HBA1", 4)) return 1;
    const uint16_t *h = (const uint16_t *)(m + 4);
    int bw = h[0], bh = h[1], by = h[2], fps = h[3], n = h[4], loop = h[5];
    const uint32_t *off = (const uint32_t *)(m + 4 + 14);

    int fb = open("/dev/fb0", O_RDWR); if (fb < 0) return 1;
    struct fb_var_screeninfo v; struct fb_fix_screeninfo fx;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &v) || ioctl(fb, FBIOGET_FSCREENINFO, &fx)) return 1;
    int W = v.xres, H = v.yres, stride = fx.line_length / 2;
    int nbuf = (int)(fx.smem_len / (fx.line_length * H)); if (nbuf > 2) nbuf = 2; if (nbuf < 1) nbuf = 1;
    uint16_t *fbm = mmap(0, fx.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbm == MAP_FAILED || bw > W || by + bh > H) return 1;
    for (int b = 0; b < nbuf; b++) memset(fbm + b * stride * H, 0, fx.line_length * H);

    uint16_t *cur = malloc(bw * bh * 2), *rest = malloc(bw * bh * 2);
    int ox = (W - bw) / 2;
    /* reposo = primer fotograma del bucle */
    { int len; char *d = stbi_zlib_decode_malloc((const char *)m + off[loop], off[loop + 1] - off[loop], &len);
      if (d && len == bw * bh * 2) memcpy(rest, d, len); else memset(rest, 0, bw * bh * 2); free(d); }

    double frame_ms = 1000.0 / (fps ? fps : 24), t0 = now_ms(), next = t0, last_check = 0;
    { int f = open("/tmp/hibyos_boot", O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f); }   /* la libreria retiene al reproductor */
    int i = 0, back = nbuf > 1 ? 1 : 0, ending = 0, fade = 0;
    for (;;) {
        if (!ending) {
            int len; char *d = stbi_zlib_decode_malloc((const char *)m + off[i], off[i + 1] - off[i], &len);
            if (d && len == bw * bh * 2) memcpy(cur, d, len);
            free(d);
        } else {
            /* fundido hacia el fotograma de reposo en 8 pasos (~330 ms) */
            int k = ++fade * 32 / 8;
            for (int p = 0; p < bw * bh; p++) {
                uint16_t a = cur[p], b = rest[p];
                int r = ((a >> 11) * (32 - k) + (b >> 11) * k) >> 5;
                int g = (((a >> 5) & 63) * (32 - k) + ((b >> 5) & 63) * k) >> 5;
                int bl = ((a & 31) * (32 - k) + (b & 31) * k) >> 5;
                cur[p] = (uint16_t)(r << 11 | g << 5 | bl);
            }
        }
        uint16_t *dst = fbm + back * stride * H;
        for (int y = 0; y < bh; y++) memcpy(dst + (by + y) * stride + ox, cur + y * bw, bw * 2);
        v.yoffset = back * H; v.xoffset = 0;
        ioctl(fb, FBIOPAN_DISPLAY, &v);                  /* escribir, despues pan */
        if (nbuf > 1) back ^= 1;
        if (ending && fade >= 8) break;
        if (!ending && ++i >= n) i = loop;
        double t = now_ms();
        if (!ending && t - last_check > 250) {
            last_check = t;
            /* fin: lo pide el shell (inicio listo); o el reproductor arranco SIN la libreria de
             * pantalla (nadie lo retiene: dibujaria encima); o 32 s como maximo */
            int player = !test_s && player_running();
            static double player_t; if (player && !player_t) player_t = t;
            int no_shim = player && t - player_t > 1500 && access("/tmp/hibymod_disp", F_OK);
            if ((test_s && t - t0 > test_s * 1000) || !access("/tmp/hibyos_boot_stop", F_OK) || no_shim || t - t0 > 32000) ending = 1;
        }
        next += frame_ms;
        double w = next - now_ms();
        if (w > 0) usleep((useconds_t)(w * 1000)); else next = now_ms();
    }
    unlink("/tmp/hibyos_boot");
    /* deja el reposo en los dos buferes: el reproductor parte de una imagen identica */
    for (int b = 0; b < nbuf; b++) { uint16_t *dst = fbm + b * stride * H;
        for (int y = 0; y < bh; y++) memcpy(dst + (by + y) * stride + ox, rest + y * bw, bw * 2); }
    return 0;
}
