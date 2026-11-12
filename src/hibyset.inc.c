/* hibyset.inc.c - control of the stock player's settings from the shell, without its UI.
 *
 * Talks to hibymod_disp.so v7 (command 'S' on HM_FIFO, request/response files in /tmp).
 * The library calls the player's own functions with the same sequence its Control Centre
 * uses, so the change is applied (not just stored). See docs/research_settings_api.md.
 *
 * Option names (values):
 *   bt wifi dlna airplay hibylink gapless fade sleep mseb gain filter   0 / 1
 *   lineout 0/1 (1 needs HS_FORCE: sets the output to fixed max level)
 *   usbmode 0 audio (USB DAC) / 1 storage (needs HS_FORCE: reconfigures the USB gadget)
 *   balance -10..10 (negative = left)     playmode 0 order, 1 loop track, 2 shuffle, 3 loop list
 *   sleepmin 5..120 (minutes)             volume 0..volmax
 *   volmax, speed: read only
 *   Proposed for library v8 (NOT in v7: answer -ENOENT and are missing from hs_list; the
 *   ids/offsets are in docs/research_settings_api.md §9):
 *     bright 1..100 (%)   bklon 0/1 (1 = screen turns off after bkltime)   bkltime 10..120 (s)
 *     idle 0/1 (power off when idle)   idlemin 1..10 (min)   update 0 manual / 1 auto (library scan)
 *   hs_ini_get(name, &v): the SAVED value of any of these keys, read from /usr/data/user.ini
 *   (instant, no library needed; may lag behind a change not yet written by the player).
 *
 * Blocking API (do not call from an animation frame):
 *   int hs_get(const char *name, int *val);        0 or <0
 *   int hs_set(const char *name, int val);         0 or <0 (value really applied)
 *   int hs_set_force(const char *name, int val);   same, for lineout=1 / usbmode
 *   int hs_list(struct hs_kv *kv, int max);        number of options read, or <0
 *   int hs_check(void);                            0 = firmware/signatures ok
 * Non-blocking (for the UI loop): id = hs_set_send(name, val, flags) / hs_get_send(name),
 *   then hs_poll(id, &res) every frame until it returns 1 (res.rc, res.val = value now).
 *   Only ONE request may be in flight: wait for the answer (or HS_TIMEOUT) before the next.
 * Errors: -ENXIO library not loaded (no reader on the FIFO), -ETIMEDOUT, -ENOEXEC other firmware
 *   (nothing was called), -EAGAIN settings not loaded yet or the player refused/was busy,
 *   -ENOENT unknown name, -ERANGE, -EPERM (greyed out in the stock UI, line out fixed volume,
 *   MSEB preconditions), -EACCES (needs HS_FORCE), -EROFS (read only).
 * Prefix hs_ / HS_. Needs <stdio.h> <string.h> <errno.h> <fcntl.h> <unistd.h> <time.h> and
 * hm_shared.h (HM_FIFO) included before. */
#ifndef HIBYSET_INC_C
#define HIBYSET_INC_C
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HS_REQ "/tmp/hibymod_setreq"
#define HS_RES "/tmp/hibymod_setres"
#define HS_FORCE 1
#define HS_TIMEOUT_MS 6000               /* hibylink off waits up to ~3.1 s inside the player */
/* turning the radios on can take much longer: after a while off, Bluetooth reloads the chip's
 * firmware (brcm_patchram) before the player answers. With 6 s the switch said "couldn't
 * change the setting" although Bluetooth came on a few seconds later. */
static int hs_timeout_for(const char *name)
{
    return name && (!strcmp(name, "bt") || !strcmp(name, "wifi")) ? 20000 : HS_TIMEOUT_MS;
}

struct hs_res { int id, rc, val, ms; char key[24]; char msg[120]; };
struct hs_kv { char name[16]; int val; };

static int hs_write(const char *op, const char *key, int val, int flags)
{
    static int seq;
    /* unique across processes too (several CLI runs in the same second) */
    int id = (int)(getpid() & 0x3fff) * 100000 + (int)(time(0) % 1000) * 100 + (++seq % 100);
    if (key && (strchr(key, '\n') || strlen(key) > 20)) return -EINVAL;
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.%d", HS_REQ, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return -errno;
    fprintf(f, "HMSET1\nid=%d\nop=%s\n", id, op);
    if (key) fprintf(f, "key=%s\n", key);
    fprintf(f, "val=%d\n", val);
    if (flags & HS_FORCE) fprintf(f, "force=1\n");
    if (fclose(f)) { unlink(tmp); return -EIO; }
    unlink(HS_RES);                       /* a stale answer must never match */
    if (rename(tmp, HS_REQ)) { int e = -errno; unlink(tmp); return e; }
    int fd = open(HM_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -ENXIO;            /* nobody reads the FIFO: player without the library */
    int w = (int)write(fd, "S", 1);
    close(fd);
    return w == 1 ? id : -EIO;
}

static int hs_set_send(const char *name, int val, int flags) { return name ? hs_write("set", name, val, flags) : -EINVAL; }
static int hs_get_send(const char *name) { return name ? hs_write("get", name, 0, 0) : -EINVAL; }

/* 1 = answer in *r (and, for list, the "k=v" lines in extra if given), 0 = not yet */
static int hs_poll_ex(int id, struct hs_res *r, char *extra, size_t extra_n)
{
    FILE *f = fopen(HS_RES, "r");
    if (!f) return 0;
    char l[256] = "";
    if (!fgets(l, sizeof l, f)) l[0] = 0;
    memset(r, 0, sizeof *r);
    int used = 0;
    if (sscanf(l, "id=%d rc=%d key=%23s val=%d ms=%d %n", &r->id, &r->rc, r->key, &r->val, &r->ms, &used) < 5 ||
        (r->id != id && r->id != -1)) { fclose(f); return 0; }   /* -1: rejected before reading the request */
    snprintf(r->msg, sizeof r->msg, "%s", l + used);
    char *nl = strchr(r->msg, '\n'); if (nl) *nl = 0;
    if (extra && extra_n) {
        size_t k = fread(extra, 1, extra_n - 1, f);
        extra[k] = 0;
    }
    fclose(f);
    return 1;
}
static int hs_poll(int id, struct hs_res *r) { return hs_poll_ex(id, r, 0, 0); }

static int hs_wait_ex(int id, struct hs_res *r, char *extra, size_t extra_n, int timeout_ms)
{
    if (id < 0) return id;
    for (int t = 0; t < timeout_ms; t += 20) {
        if (hs_poll_ex(id, r, extra, extra_n)) return r->rc;
        struct timespec d = { 0, 20 * 1000000L }; nanosleep(&d, 0);
    }
    return -ETIMEDOUT;
}

/* la libreria atiende una peticion a la vez: todo el shell (Ajustes, Inalambrico, CC) pasa por aqui */
static pthread_mutex_t hs_mu = PTHREAD_MUTEX_INITIALIZER;
static int hs_get(const char *name, int *val)
{
    struct hs_res r;
    pthread_mutex_lock(&hs_mu);
    int rc = hs_wait_ex(hs_get_send(name), &r, 0, 0, 2000);
    pthread_mutex_unlock(&hs_mu);
    if (rc == 0 && val) *val = r.val;
    return rc;
}

static int hs_set_flags(const char *name, int val, int flags)
{
    struct hs_res r;
    pthread_mutex_lock(&hs_mu);
    int rc = hs_wait_ex(hs_set_send(name, val, flags), &r, 0, 0, hs_timeout_for(name));
    pthread_mutex_unlock(&hs_mu);
    return rc;
}
static int hs_set(const char *name, int val) { return hs_set_flags(name, val, 0); }
static int hs_set_force(const char *name, int val) { return hs_set_flags(name, val, HS_FORCE); }

static int hs_check(void)
{
    struct hs_res r;
    return hs_wait_ex(hs_write("check", 0, 0, 0), &r, 0, 0, 2000);
}

/* saved value from /usr/data/user.ini (the player's settings struct verbatim: field offset =
 * file offset, little-endian int32). 0 or -ENOENT (unknown key) / -EIO / -ERANGE */
static const struct { const char *k; short off, lo, hi; } hs_ini_tab[] = {
    { "bt", 0x394, 0, 1 }, { "wifi", 0x3c8, 0, 1 }, { "dlna", 0x3d0, 0, 1 }, { "airplay", 0x3cc, 0, 1 }, { "hibylink", 0x3b4, 0, 1 },
    { "gain", 0x24c, 0, 1 }, { "mseb", 0x420, 0, 1 }, { "lineout", 0x280, 0, 1 }, { "usbmode", 0x274, 0, 2 }, { "balance", 0x254, -10, 10 },
    { "gapless", 0x234, 0, 1 }, { "fade", 0x3c4, 0, 1 }, { "filter", 0x3c0, 0, 1 }, { "sleep", 0x270, 0, 1 }, { "sleepmin", 0x0c, 5, 120 },
    { "playmode", 0x250, 0, 3 }, { "volume", 0x23c, 0, 255 }, { "volmax", 0x238, 0, 255 }, { "speed", 0x710, 50, 200 },
    { "bright", 0x18, 1, 100 }, { "bklon", 0x3d4, 0, 1 }, { "bkltime", 0x10, 10, 120 }, { "idle", 0x26c, 0, 1 }, { "idlemin", 0x14, 1, 10 },
    { "update", 0x20, 0, 1 }, { "lang", 0x08, 0, 12 },
};
static int hs_ini_get(const char *name, int *val)
{
    for (unsigned i = 0; name && i < sizeof hs_ini_tab / sizeof hs_ini_tab[0]; i++) {
        if (strcmp(name, hs_ini_tab[i].k)) continue;
        int fd = open("/usr/data/user.ini", O_RDONLY | O_CLOEXEC); if (fd < 0) return -EIO;
        int32_t v; ssize_t n = pread(fd, &v, 4, hs_ini_tab[i].off); close(fd);
        if (n != 4) return -EIO;
        if (v < hs_ini_tab[i].lo || v > hs_ini_tab[i].hi) return -ERANGE;
        if (val) *val = v;
        return 0;
    }
    return -ENOENT;
}

/* every option at once (one request); returns how many were filled */
static int hs_list(struct hs_kv *kv, int max)
{
    struct hs_res r;
    char ex[2048];
    pthread_mutex_lock(&hs_mu);
    int rc = hs_wait_ex(hs_write("list", 0, 0, 0), &r, ex, sizeof ex, 2000);
    pthread_mutex_unlock(&hs_mu);
    if (rc < 0) return rc;
    int n = 0;
    for (char *l = ex, *nx; l && *l && n < max; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        char *eq = strchr(l, '=');
        if (!eq || eq - l >= (int)sizeof kv[0].name) continue;
        memcpy(kv[n].name, l, (size_t)(eq - l)); kv[n].name[eq - l] = 0;
        kv[n].val = atoi(eq + 1);
        n++;
    }
    return n;
}
#endif
