/* fbtouch - app de prueba/plantilla: abre /dev/fb0 (320x480 RGB565, doble buffer por panning)
 * y /dev/input/event1 (goodix-ts protocolo A); dibuja un punto donde se toca; teclas por stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
static struct fb_var_screeninfo v; static struct fb_fix_screeninfo f; static uint16_t *fbm; static int fb, back = 1;
static uint16_t rgb(int r, int g, int b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
static uint16_t canvas[320 * 480];
static void flip(void) {
    memcpy((char *)fbm + (size_t)f.line_length * v.yres * back, canvas, sizeof canvas);
    v.yoffset = v.yres * back; ioctl(fb, FBIOPAN_DISPLAY, &v); back ^= 1;
}
int main(void) {
    fb = open("/dev/fb0", O_RDWR); if (fb < 0) { perror("fb0"); return 1; }
    ioctl(fb, FBIOGET_VSCREENINFO, &v); ioctl(fb, FBIOGET_FSCREENINFO, &f);
    printf("fb %ux%u virt %ux%u bpp %u\n", v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel);
    fbm = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    for (int i = 0; i < 320 * 480; i++) canvas[i] = rgb(20, 20, 40);
    for (int x = 0; x < 320; x++) for (int y = 0; y < 24; y++) canvas[y * 320 + x] = rgb(200, 60, 60);
    flip();
    struct pollfd p[5]; char path[32];
    for (int i = 0; i < 5; i++) { snprintf(path, sizeof path, "/dev/input/event%d", i); p[i].fd = open(path, O_RDONLY); p[i].events = POLLIN; }
    int x = -1, y = -1, down = 0;
    for (;;) {
        poll(p, 5, -1);
        for (int i = 0; i < 5; i++) if (p[i].revents & POLLIN) {
            struct input_event e; if (read(p[i].fd, &e, sizeof e) != sizeof e) continue;
            if (i != 1) { if (e.type == EV_KEY) { printf("key dev%d code %d val %d\n", i, e.code, e.value); fflush(stdout); if (e.code == 116 && e.value == 1) return 0; } continue; }
            if (e.type == EV_KEY && e.code == BTN_TOUCH) down = e.value;
            if (e.type == EV_ABS && e.code == ABS_MT_POSITION_X) x = e.value;
            if (e.type == EV_ABS && e.code == ABS_MT_POSITION_Y) y = e.value;
            if (e.type == EV_SYN && e.code == SYN_REPORT && down && x >= 0) {
                for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) { int xx = x + dx, yy = y + dy; if (xx >= 0 && xx < 320 && yy >= 0 && yy < 480) canvas[yy * 320 + xx] = rgb(255, 255, 0); }
                flip(); printf("touch %d %d\n", x, y); fflush(stdout);
            }
        }
    }
}
