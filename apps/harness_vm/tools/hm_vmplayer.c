/*
 * hm_vmplayer - stand-in for hiby_player + hibymod_disp.so inside the QEMU harness, so that the
 * real hibyshell runs unmodified in the VM (docs/VM_SHELL.md).
 *
 * It provides exactly the shared interface of src/hm_shared.h:
 *   - /dev/shm/hibymod_fbstate (struct hm_state, magic HMF4, input_mode 2 once the shell connects)
 *   - /dev/shm/hibymod_frames  ([0] shell image, [1] "player" frame)
 *   - FIFO /tmp/hibymod_disp   (C, P, H, R, O; L and S are acknowledged and ignored)
 *   - socket /tmp/hibymod_input.sock (touches the shell forwards to the "player": discarded)
 * and emulates the panel: /dev/fb0 is scanned out at PANEL_HZ (44.3 Hz like the ST77903), a
 * 'P' waits for the next frame edge, copies frames[0] (dirty rows) into the visible buffer and
 * bumps presents, so present() in the shell blocks like on the device.
 *
 * The "player" only ever shows its stock home screen (painted so that the shell's home
 * signature matches); every time the shell releases the screen it "draws" that home again and
 * bumps home_hits, so the shell enters (or re-enters) its launcher like on the HiBy.
 *
 * Measurement (cheap, so it steals little CPU from the shell on the single vCPU):
 *   /tmp/vmplayer.log   one line per present: "P <ms> <n> <changed> <y0> <y1> <sum>"
 *                       ms = CLOCK_MONOTONIC (same clock as hibyinput's event log),
 *                       changed = 1 if the visible image differs from the previous present
 *   /tmp/vmrec          if it exists and contains "N /path/out.bin", the next N presents are
 *                       recorded (160x240 RGB565 + ms + yoffset per frame: the format of
 *                       `hibyctl rec`, viewable with tools/recsheet.py), then the file is removed.
 *
 * Build (static, hard-float like the shell; run the VM with --cpu 24Kf):
 *   zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -O2 -static -s -o hm_vmplayer hm_vmplayer.c -lpthread
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include "hm_shared.h"

#define PANEL_HZ 44.3
#define ASSETS "/usr/data/hibymod/shell"

static struct hm_state *st;
static uint16_t *frames, *vis;          /* frames[0], frames[1]; visible fb buffer (yoffset 0) */
static uint16_t home_img[HM_W * HM_H];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static double t_base;
static FILE *plog;
static uint32_t last_sum;
static int npres;
static volatile int need_home = 1;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void sleep_ms(double ms) { if (ms <= 0) return; struct timespec t = { (time_t)(ms / 1000), (long)((ms - (time_t)(ms / 1000) * 1000) * 1e6) }; while (nanosleep(&t, &t) && errno == EINTR) {} }
/* sleep until the next scan-out frame edge (like FBIOPAN_DISPLAY on the real panel) */
static void frame_edge(void)
{
    double per = 1000.0 / PANEL_HZ, t = now_ms() - t_base;
    double next = (double)((long long)(t / per) + 1) * per;
    sleep_ms(next - t);
}
static void *map_file(const char *path, size_t size)
{
    int f = open(path, O_RDWR | O_CREAT, 0600);
    if (f < 0) return 0;
    if (ftruncate(f, size)) { close(f); return 0; }
    void *m = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    close(f);
    return m == MAP_FAILED ? 0 : m;
}
static uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

/* stock home stand-in: dark gradient plus the signature points of home.sig, so is_home() holds */
static void paint_home(void)
{
    for (int y = 0; y < HM_H; y++) for (int x = 0; x < HM_W; x++) home_img[y * HM_W + x] = rgb(20 + y / 24, 22 + y / 24, 30 + y / 16);
    for (int y = 0; y < 30; y++) for (int x = 0; x < HM_W; x++) home_img[y * HM_W + x] = 0;
    for (int y = 8; y < 22; y++) for (int x = 290; x < 316; x++) home_img[y * HM_W + x] = 0xFFFF;   /* "battery": has_statusbar() */
    FILE *f = fopen(ASSETS "/mirror.sig", "r"); char line[96];
    if (f && !fgets(line, sizeof line, f)) { fclose(f); f = 0; }     /* first line = boot id */
    if (!f) f = fopen(ASSETS "/home.sig", "r");
    int v[6], n = 0;
    while (f && fscanf(f, "%d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        if (v[0] >= 0 && v[0] < HM_W && v[1] >= 0 && v[1] < HM_H) {
            uint16_t c = rgb(v[2], v[3], v[4]);
            for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
                int x = v[0] + dx, y = v[1] + dy;
                if (x >= 0 && x < HM_W && y >= 0 && y < HM_H) home_img[y * HM_W + x] = c;
            }
            n++;
        }
    }
    if (f) fclose(f);
    fprintf(stderr, "hm_vmplayer: home signature points painted: %d\n", n);
}

/* ---------------------------------------------------------------- recording */
static uint8_t *rec_buf; static int rec_n, rec_max; static char rec_path[200];
static void rec_check(void)
{
    if (rec_max) return;
    FILE *f = fopen("/tmp/vmrec", "r"); if (!f) return;
    int n = 0; char p[200] = "";
    if (fscanf(f, "%d %199s", &n, p) == 2 && n > 0 && n <= 400) {
        rec_buf = malloc((size_t)n * (8 + 160 * 240 * 2));
        if (rec_buf) { rec_max = n; rec_n = 0; snprintf(rec_path, sizeof rec_path, "%s", p); }
    }
    fclose(f); unlink("/tmp/vmrec");
}
static void rec_frame(double t)
{
    if (!rec_max) return;
    uint8_t *o = rec_buf + (size_t)rec_n * (8 + 160 * 240 * 2);
    uint32_t hd[2] = { (uint32_t)t, 0 }; memcpy(o, hd, 8);
    uint16_t *px = (uint16_t *)(o + 8);
    for (int y = 0; y < 240; y++) for (int x = 0; x < 160; x++) px[y * 160 + x] = vis[(y * 2) * HM_W + x * 2];
    if (++rec_n >= rec_max) {
        FILE *f = fopen(rec_path, "wb");
        if (f) { fwrite(rec_buf, 1, (size_t)rec_n * (8 + 160 * 240 * 2), f); fclose(f); }
        free(rec_buf); rec_buf = 0; rec_max = 0;
        if (plog) { fprintf(plog, "R saved %d frames to %s\n", rec_n, rec_path); fflush(plog); }
    }
}

/* sampled checksum of the visible image (every 7th pixel): detects a changed frame */
static uint32_t vis_sum(void)
{
    uint32_t s = 2166136261u;
    for (int i = 0; i < HM_W * HM_H; i += 7) { s ^= vis[i]; s *= 16777619u; }
    return s;
}
static void log_present(int y0, int y1)
{
    double t = now_ms();
    uint32_t s = vis_sum();
    npres++;
    if (plog) { fprintf(plog, "P %.1f %d %d %d %d %08x\n", t, npres, s != last_sum, y0, y1, s); fflush(plog); }
    last_sum = s;
    rec_check(); rec_frame(t);
}

/* the "player" draws its home screen (with the launcher substituted, like the real library) */
static void player_home_frame(void)
{
    memcpy(frames + HM_W * HM_H, home_img, HM_FRAME_BYTES);
    if (st->home_sub_on) st->home_hits++;
}

/* ---------------------------------------------------------------- FIFO commands */
static void *control(void *arg)
{
    (void)arg;
    for (;;) {
        int f = open(HM_FIFO, O_RDONLY);
        if (f < 0) { sleep(1); continue; }
        char c;
        while (read(f, &c, 1) == 1) {
            if (c == 'L' || c == 'S') continue;
            pthread_mutex_lock(&mu);
            if (c == 'C') {
                if (!st->held && !st->capture) {
                    memcpy(frames, vis, HM_FRAME_BYTES);
                    memcpy(frames + HM_W * HM_H, frames, HM_FRAME_BYTES);
                    st->cap_frames = 0;
                    st->capture = 1;
                }
            } else if (c == 'P') {
                if (st->capture) {
                    frame_edge();
                    for (int i = 0; i < 300 && (st->fr0_seq & 1); i++) usleep(10);
                    uint32_t y0 = st->dirty_y0, y1 = st->dirty_y1;
                    if (y1 > y0 && y1 <= HM_H) memcpy(vis + y0 * HM_W, frames + y0 * HM_W, (y1 - y0) * HM_W * 2);
                    else { memcpy(vis, frames, HM_FRAME_BYTES); y0 = 0; y1 = HM_H; }
                    log_present((int)y0, (int)y1);
                }
                st->presents++;
            } else if (c == 'H') {
                st->capture = 0; st->held = 1;
            } else if (c == 'R') {
                int was = st->capture;
                st->capture = 0; st->held = 0;
                frame_edge();
                if (was) memcpy(vis, frames + HM_W * HM_H, HM_FRAME_BYTES);
                st->presents++;
                log_present(0, HM_H);
                need_home = 1;          /* back to the player: it redraws its home screen */
            } else if (c == 'O') {
                st->presents++;
            }
            pthread_mutex_unlock(&mu);
        }
        close(f);
    }
    return 0;
}

/* ---------------------------------------------------------------- input socket */
static void *input_thread(void *arg)
{
    (void)arg;
    unlink(HM_SOCK);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, HM_SOCK);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { perror("socket"); return 0; }
    for (;;) {
        int cl = accept(ls, 0, 0);
        if (cl < 0) continue;
        st->input_mode = 2;
        fprintf(stderr, "hm_vmplayer: shell connected\n");
        char b[1024];
        while (read(cl, b, sizeof b) > 0) {}      /* touches for the player: nobody to deliver them to */
        close(cl);
        st->input_mode = 1;
        pthread_mutex_lock(&mu);
        st->capture = 0; st->held = 0; st->ov_on = 0;
        pthread_mutex_unlock(&mu);
        fprintf(stderr, "hm_vmplayer: shell disconnected\n");
    }
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    t_base = now_ms();
    mkdir("/dev/shm", 0777);
    st = map_file(HM_STATE, HM_STATE_SIZE);
    frames = map_file(HM_FRAMES, 2 * HM_FRAME_BYTES);
    if (!st || !frames) { perror("shm"); return 1; }
    memset(st, 0, HM_STATE_SIZE);
    int fbf = open("/dev/fb0", O_RDWR);
    if (fbf < 0) { perror("/dev/fb0"); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo fx;
    ioctl(fbf, FBIOGET_VSCREENINFO, &v); ioctl(fbf, FBIOGET_FSCREENINFO, &fx);
    uint16_t *fb = mmap(0, fx.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbf, 0);
    if (fb == MAP_FAILED) { perror("mmap fb"); return 1; }
    v.xoffset = 0; v.yoffset = 0; ioctl(fbf, FBIOPAN_DISPLAY, &v);
    vis = fb;
    paint_home();
    memcpy(vis, home_img, HM_FRAME_BYTES);
    memcpy(frames, home_img, HM_FRAME_BYTES); memcpy(frames + HM_W * HM_H, home_img, HM_FRAME_BYTES);
    unlink(HM_FIFO); mkfifo(HM_FIFO, 0666);
    plog = fopen("/tmp/vmplayer.log", "w");
    st->yoffset = 0; st->input_mode = 1;
    __sync_synchronize();
    st->magic = HM_MAGIC;
    pthread_t a, b;
    pthread_create(&a, 0, control, 0);
    pthread_create(&b, 0, input_thread, 0);
    fprintf(stderr, "hm_vmplayer: ready (panel %.1f Hz)\n", PANEL_HZ);
    /* the player keeps "drawing" its home while the screen is its own: when the shell has asked
     * for the home substitution, the next frame is a home hit (it re-enters its launcher) */
    for (;;) {
        sleep_ms(100);
        pthread_mutex_lock(&mu);
        if (need_home && !st->capture && !st->held && st->home_sub_on) {
            player_home_frame();
            memcpy(vis, frames + HM_W * HM_H, HM_FRAME_BYTES);
            need_home = 0;
        }
        pthread_mutex_unlock(&mu);
    }
}
