/*
 * hibyinput - daemon del arnes de emulacion HiBy R3 II (corre DENTRO del guest MIPS).
 *
 * 1) Crea via /dev/uinput 5 dispositivos con los mismos nombres/orden que el HiBy real:
 *      event0 "md-gpio-keys"     KEY_POWER(116)
 *      event1 "goodix-ts"        multitouch protocolo A (BTN_TOUCH, ABS_MT_*)
 *      event2 "jz adc keyboard"  teclas configurables
 *      event3 "sa-ring-keys"     teclas configurables (por defecto VOLUP/VOLDOWN/PLAYPAUSE/NEXT/PREV)
 *      event4 "earpods_adc"      teclas configurables
 * 2) Escucha en TCP (puerto 5555 por defecto) comandos de texto, uno por linea:
 *      tap X Y [ms]              toque (down, espera ms (def 60), up)
 *      down X Y | move X Y | up  toque manual
 *      swipe X1 Y1 X2 Y2 [ms]    arrastre con reportes cada ~10 ms
 *      key NAME|CODE [click|down|up] [devidx]
 *      sleep MS
 *      shot                      -> "shot W H BYTES\n" + RGB565 crudo del area VISIBLE (respeta yoffset)
 *      fbinfo                    -> var/fix screeninfo
 *      sh CMD...                 -> ejecuta CMD con /bin/sh, devuelve salida y "\n.\n"
 *      quit
 * Configuracion opcional: /etc/hibyinput.conf
 *      keys <devidx> <code> [<code>...]   (sustituye la lista de teclas del dispositivo 0,2,3,4)
 *      route <NAME|code> <devidx>         (a que dispositivo va una tecla por defecto)
 *      name <NAME> <code>                 (alias nuevo)
 *      release_mt_report 0|1              (si al soltar se envia SYN_MT_REPORT vacio; def 1)
 *
 * Compilar: zig cc -target mipsel-linux-musleabi -mcpu=mips32r2 -msoft-float -Os -static
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/fb.h>

#define NDEV 5
static const char *devnames[NDEV] = {"md-gpio-keys", "goodix-ts", "jz adc keyboard", "sa-ring-keys", "earpods_adc"};
static int devkeys[NDEV][32];
static int ndevkeys[NDEV];
static int fds[NDEV];
static int release_mt_report = 1;

struct alias { char name[24]; int code; int route; };
static struct alias aliases[64] = {
    {"POWER", 116, 0}, {"VOLUMEUP", 115, 3}, {"VOLUMEDOWN", 114, 3},
    {"PLAYPAUSE", 164, 3}, {"NEXTSONG", 163, 3}, {"PREVIOUSSONG", 165, 3},
    {"VOLUP", 115, 3}, {"VOLDOWN", 114, 3}, {"PLAY", 164, 3}, {"NEXT", 163, 3}, {"PREV", 165, 3},
};
static int naliases = 11;

static void msleep(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; while (nanosleep(&ts, &ts) && errno == EINTR); }

static void emit(int fd, int type, int code, int val) {
    struct input_event ev; memset(&ev, 0, sizeof ev);
    ev.type = type; ev.code = code; ev.value = val;
    if (write(fd, &ev, sizeof ev) != sizeof ev) perror("uinput write");
}

static int mkdev(int idx) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); exit(1); }
    struct uinput_user_dev u; memset(&u, 0, sizeof u);
    snprintf(u.name, sizeof u.name, "%s", devnames[idx]);
    u.id.bustype = (idx == 1) ? BUS_I2C : BUS_HOST; u.id.vendor = 0x0416; u.id.product = 0x1001 + idx; u.id.version = 1;
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    if (idx == 1) {
        ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        int axes[] = {ABS_MT_TOUCH_MAJOR, ABS_MT_WIDTH_MAJOR, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID};
        int mx[] = {255, 255, 319, 479, 255};
        for (int i = 0; i < 5; i++) { ioctl(fd, UI_SET_ABSBIT, axes[i]); u.absmin[axes[i]] = 0; u.absmax[axes[i]] = mx[i]; }
#ifdef UI_SET_PROPBIT
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
#endif
    } else {
        for (int i = 0; i < ndevkeys[idx]; i++) ioctl(fd, UI_SET_KEYBIT, devkeys[idx][i]);
    }
    if (write(fd, &u, sizeof u) != sizeof u) { perror("uinput dev write"); exit(1); }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); exit(1); }
    return fd;
}

/* --- registro de eventos tactiles (CLOCK_MONOTONIC, el mismo reloj que hm_vmplayer):
 * /tmp/hibyinput.log "D|M|U ms x y" para medir toque -> respuesta (docs/VM_SHELL.md) --- */
static int touching;
static void evlog_put(char k, int x, int y) {
    int fd = open("/tmp/hibyinput.log", O_WRONLY | O_APPEND | O_CREAT, 0644);   /* reabierto: se puede vaciar o borrar */
    if (fd < 0) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    char b[64]; int n = snprintf(b, sizeof b, "%c %.1f %d %d\n", k, t.tv_sec * 1e3 + t.tv_nsec / 1e6, x, y);
    if (write(fd, b, n) < 0) {}
    close(fd);
}
/* --- touch, protocolo A identico al goodix-ts del HiBy --- */
static void touch_report(int x, int y) {
    int fd = fds[1];
    if (x < 0) x = 0; if (x > 319) x = 319; if (y < 0) y = 0; if (y > 479) y = 479;
    evlog_put(touching ? 'M' : 'D', x, y); touching = 1;
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, x);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 22);
    emit(fd, EV_ABS, ABS_MT_WIDTH_MAJOR, 22);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 0);
    emit(fd, EV_SYN, SYN_MT_REPORT, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}
static void touch_up(void) {
    int fd = fds[1];
    evlog_put('U', -1, -1); touching = 0;
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    if (release_mt_report) emit(fd, EV_SYN, SYN_MT_REPORT, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int lookup_key(const char *s, int *route) {
    char *e; long v = strtol(s, &e, 0);
    if (*s && !*e) { *route = -1; for (int i = 0; i < naliases; i++) if (aliases[i].code == v) { *route = aliases[i].route; break; } return (int)v; }
    const char *n = s; if (!strncasecmp(n, "KEY_", 4)) n += 4;
    for (int i = 0; i < naliases; i++) if (!strcasecmp(aliases[i].name, n)) { *route = aliases[i].route; return aliases[i].code; }
    return -1;
}
static int dev_has_key(int d, int code) { for (int i = 0; i < ndevkeys[d]; i++) if (devkeys[d][i] == code) return 1; return 0; }

static void load_conf(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *t = strtok(line, " \t\r\n"); if (!t || *t == '#') continue;
        if (!strcmp(t, "keys")) {
            char *d = strtok(NULL, " \t\r\n"); if (!d) continue; int di = atoi(d); if (di < 0 || di >= NDEV || di == 1) continue;
            ndevkeys[di] = 0; char *c;
            while ((c = strtok(NULL, " \t\r\n")) && ndevkeys[di] < 32) { int r; int k = lookup_key(c, &r); if (k > 0) devkeys[di][ndevkeys[di]++] = k; }
        } else if (!strcmp(t, "route")) {
            char *k = strtok(NULL, " \t\r\n"), *d = strtok(NULL, " \t\r\n"); if (!k || !d) continue;
            int r; int code = lookup_key(k, &r); for (int i = 0; i < naliases; i++) if (aliases[i].code == code) aliases[i].route = atoi(d);
        } else if (!strcmp(t, "name")) {
            char *k = strtok(NULL, " \t\r\n"), *c = strtok(NULL, " \t\r\n"); if (!k || !c || naliases >= 64) continue;
            snprintf(aliases[naliases].name, 24, "%s", k); aliases[naliases].code = strtol(c, 0, 0); aliases[naliases].route = 3; naliases++;
        } else if (!strcmp(t, "release_mt_report")) {
            char *v = strtok(NULL, " \t\r\n"); if (v) release_mt_report = atoi(v);
        }
    }
    fclose(f);
}

static int wr(int s, const void *b, size_t n) { const char *p = b; while (n) { ssize_t w = write(s, p, n); if (w <= 0) return -1; p += w; n -= w; } return 0; }
static void say(int s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void say(int s, const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); wr(s, b, n); }

static void do_shot(int s, int info_only) {
    int fb = open("/dev/fb0", O_RDONLY);
    if (fb < 0) { say(s, "err no fb0\n"); return; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fb, FBIOGET_VSCREENINFO, &v); ioctl(fb, FBIOGET_FSCREENINFO, &f);
    if (info_only) {
        say(s, "id=%s res=%ux%u virt=%ux%u off=%u,%u bpp=%u line=%u smem=%u R=%u/%u G=%u/%u B=%u/%u\n", f.id, v.xres, v.yres, v.xres_virtual, v.yres_virtual,
            v.xoffset, v.yoffset, v.bits_per_pixel, f.line_length, f.smem_len, v.red.offset, v.red.length, v.green.offset, v.green.length, v.blue.offset, v.blue.length);
        close(fb); return;
    }
    size_t len = (size_t)f.line_length * v.yres;
    char *buf = malloc(len);
    if (lseek(fb, (off_t)f.line_length * v.yoffset, SEEK_SET) < 0 || read(fb, buf, len) != (ssize_t)len) { say(s, "err read\n"); free(buf); close(fb); return; }
    say(s, "shot %u %u %u %u\n", v.xres, v.yres, (unsigned)len, v.bits_per_pixel);
    wr(s, buf, len); free(buf); close(fb);
}

static void do_sh(int s, const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) { say(s, "err popen\n.\n"); return; }
    char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, p)) > 0) wr(s, b, n);
    int st = pclose(p);
    say(s, "\n.%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static void handle(int s, char *line) {
    char *argv[16]; int argc = 0; char copy[1024]; snprintf(copy, sizeof copy, "%s", line);
    for (char *t = strtok(line, " \t\r\n"); t && argc < 16; t = strtok(NULL, " \t\r\n")) argv[argc++] = t;
    if (!argc) return;
    const char *c = argv[0];
    if (!strcmp(c, "tap") && argc >= 3) {
        int ms = argc > 3 ? atoi(argv[3]) : 60;
        touch_report(atoi(argv[1]), atoi(argv[2])); msleep(ms); touch_up(); say(s, "ok\n");
    } else if (!strcmp(c, "down") && argc >= 3) { touch_report(atoi(argv[1]), atoi(argv[2])); say(s, "ok\n"); }
    else if (!strcmp(c, "move") && argc >= 3) { touch_report(atoi(argv[1]), atoi(argv[2])); say(s, "ok\n"); }
    else if (!strcmp(c, "up")) { touch_up(); say(s, "ok\n"); }
    else if (!strcmp(c, "swipe") && argc >= 5) {
        int x1 = atoi(argv[1]), y1 = atoi(argv[2]), x2 = atoi(argv[3]), y2 = atoi(argv[4]), ms = argc > 5 ? atoi(argv[5]) : 300;
        int steps = ms / 10; if (steps < 2) steps = 2;
        for (int i = 0; i <= steps; i++) { touch_report(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps); msleep(10); }
        touch_up(); say(s, "ok\n");
    } else if (!strcmp(c, "key") && argc >= 2) {
        int route; int code = lookup_key(argv[1], &route);
        const char *mode = argc > 2 ? argv[2] : "click";
        int d = argc > 3 ? atoi(argv[3]) : route;
        if (d < 0) for (int i = 0; i < NDEV; i++) if (i != 1 && dev_has_key(i, code)) { d = i; break; }
        if (code <= 0 || d < 0 || d >= NDEV || d == 1 || !dev_has_key(d, code)) { say(s, "err key %s not on dev %d\n", argv[1], d); return; }
        if (strcmp(mode, "up")) { emit(fds[d], EV_KEY, code, 1); emit(fds[d], EV_SYN, SYN_REPORT, 0); }
        if (!strcmp(mode, "click")) msleep(80);
        if (strcmp(mode, "down")) { emit(fds[d], EV_KEY, code, 0); emit(fds[d], EV_SYN, SYN_REPORT, 0); }
        say(s, "ok\n");
    } else if (!strcmp(c, "sleep") && argc >= 2) { msleep(atoi(argv[1])); say(s, "ok\n"); }
    else if (!strcmp(c, "shot")) do_shot(s, 0);
    else if (!strcmp(c, "fbinfo")) do_shot(s, 1);
    else if (!strcmp(c, "sh")) { char *p = strstr(copy, "sh"); do_sh(s, p + 2); }
    else say(s, "err unknown\n");
}

int main(int argc, char **argv) {
    int port = 5555;
    if (argc > 1) port = atoi(argv[1]);
    signal(SIGPIPE, SIG_IGN);
    int defk[] = {115, 114, 164, 163, 165};
    devkeys[0][0] = 116; ndevkeys[0] = 1;
    for (int d = 2; d <= 3; d++) { memcpy(devkeys[d], defk, sizeof defk); ndevkeys[d] = 5; }
    devkeys[4][0] = 164; devkeys[4][1] = 115; devkeys[4][2] = 114; ndevkeys[4] = 3;
    load_conf(getenv("HIBYINPUT_CONF") ? getenv("HIBYINPUT_CONF") : "/etc/hibyinput.conf");
    for (int i = 0; i < NDEV; i++) { fds[i] = mkdev(i); msleep(50); }
    fprintf(stderr, "hibyinput: 5 dispositivos creados, escuchando en tcp/%d\n", port);
    int ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (void *)&a, sizeof a) || listen(ls, 4)) { perror("bind/listen"); return 1; }
    for (;;) {
        int s = accept(ls, 0, 0); if (s < 0) continue;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        char buf[2048]; size_t have = 0;
        for (;;) {
            ssize_t n = read(s, buf + have, sizeof buf - 1 - have); if (n <= 0) break;
            have += n; buf[have] = 0; char *nl;
            while ((nl = memchr(buf, '\n', have))) {
                *nl = 0; char line[1024]; snprintf(line, sizeof line, "%s", buf);
                size_t used = nl - buf + 1; memmove(buf, nl + 1, have - used); have -= used; buf[have] = 0;
                if (!strncmp(line, "quit", 4)) goto done;
                handle(s, line);
            }
            if (have >= sizeof buf - 1) have = 0;
        }
    done:
        close(s);
    }
}
