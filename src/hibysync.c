/* hibysync - ByOS sync daemon: BTP/1 server (docs/SYNC_PROTOCOL.md).
 *
 * Started on demand by the ByOS shell (never at boot):
 *     hibysync serve --control 3 [--wifi] [--idle-exit SECONDS] [--name NAME]
 *     hibysync serve --idle-exit 300 --detach          (USB fallback started over adb, no shell)
 *
 * - TCP 127.0.0.1:7340 always (USB through `adb forward tcp:7340 tcp:7340`, cleartext allowed);
 *   TCP <wlan0>:7340 and UDP discovery only with --wifi (pairing or AUTH, then encrypted records).
 * - One thread, one poll() loop, at most 2 sessions, static buffers (no heap growth per file).
 * - Files confined to the SD root with openat()/O_NOFOLLOW; PUT goes to a hidden .part with a
 *   streaming SHA-256, fdatasync every 4 MiB, fsync + rename + fsync(dir); GET uses sendfile()
 *   when nothing has to be hashed or encrypted.
 * - Updates: the same Ed25519 check and JSON parser as the OTA page (ota_net.inc.c with
 *   OTA_CORE_ONLY); installing is handed to the shell over the control pipe.
 *
 * Test builds (-DBTP_TEST) take their paths from HIBYSYNC_* environment variables (SD root,
 * /usr/data, hibymod folder, /proc/mounts, helpers, port...). Release builds have fixed paths.
 * -DBTP_NO_MAIN leaves out main() so tests/btp/btp_test.c can include this file. */
#define _GNU_SOURCE
#define OTA_CORE_ONLY 1
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
    char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    time_t t = time(0); struct tm tm; localtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d hibysync: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, b);
}
#include "ota_net.inc.c"
#include "btp.h"
#include "btp_crypto.inc.c"
#include "btp_proto.inc.c"

#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>

#define SYNC_SOFTWARE "hibysync 0.1.0"
#define SYNC_MODEL "R3II-2025"
#define MAX_SESS 2
#define MAX_PEERS 8
#define T_HELLO_S 5
#define T_IDLE_S 120
#define T_PUT_IDLE_S 30
#define T_SECURE_S 15      /* a Wi-Fi session must finish AUTH or reach PAIR_REVEAL within this */
#define T_PAIR_S 120
#define HASH_STEP (2u << 20)   /* bytes hashed per poll turn by the incremental hash jobs */
#define HOLD_MAX 4             /* pipelined requests kept aside while a job runs */
#define HOLDCAP 16384          /* their payload bytes in total (bigger ones wait in the RX buffer) */
#define PGA 16384              /* alignment of the big buffers, so madvise() can drop them (4K or 16K pages) */
#define PAIR_WINDOW_S 300
#define SYNC_EVERY (4u << 20)

/* ================================================================== paths */
struct sync_paths {
    char sd[256], data[256], hm[256], sync[300], dbdir[320], ota[300], mounts[256], hibytag[256], cli[256],
         batt[256], usb[256], lock[256], resconf[256], wifi_if[IFNAMSIZ];
    int port; uint32_t wifi_fixed;   /* test: fixed "Wi-Fi" address instead of the interface */
    int t_idle, t_put_idle, t_secure;   /* timeouts (s); only test builds change them */
};
static struct sync_paths SP;
static void paths_init(void)
{
    snprintf(SP.sd, sizeof SP.sd, "/usr/data/mnt/sd_0");
    snprintf(SP.data, sizeof SP.data, "/usr/data");
    snprintf(SP.hm, sizeof SP.hm, "/usr/data/hibymod");
    snprintf(SP.mounts, sizeof SP.mounts, "/proc/mounts");
    snprintf(SP.hibytag, sizeof SP.hibytag, "/usr/data/hibymod/shell/hibytag");
    snprintf(SP.cli, sizeof SP.cli, "/usr/bin/hibymod");
    snprintf(SP.batt, sizeof SP.batt, "/sys/class/power_supply/battery/capacity");
    snprintf(SP.usb, sizeof SP.usb, "/sys/class/power_supply/usb/online");
    snprintf(SP.lock, sizeof SP.lock, "/tmp/hibysync.pid");
    snprintf(SP.resconf, sizeof SP.resconf, "/usr/resource/config.json");
    snprintf(SP.wifi_if, sizeof SP.wifi_if, "wlan0");
    SP.port = BTP_PORT;
    SP.t_idle = T_IDLE_S; SP.t_put_idle = T_PUT_IDLE_S; SP.t_secure = T_SECURE_S;
#ifdef BTP_TEST
    const char *e;
#define SENV(k, f) if ((e = getenv(k)) && *e) snprintf(SP.f, sizeof SP.f, "%s", e)
    SENV("HIBYSYNC_SD", sd); SENV("HIBYSYNC_DATA", data); SENV("HIBYSYNC_HM", hm); SENV("HIBYSYNC_MOUNTS", mounts);
    SENV("HIBYSYNC_HIBYTAG", hibytag); SENV("HIBYSYNC_CLI", cli); SENV("HIBYSYNC_BATT", batt); SENV("HIBYSYNC_USB", usb);
    SENV("HIBYSYNC_LOCK", lock); SENV("HIBYSYNC_RESCONF", resconf);
#undef SENV
    if ((e = getenv("HIBYSYNC_PORT")) && *e) SP.port = atoi(e);
    if ((e = getenv("HIBYSYNC_WIFI_ADDR")) && *e) SP.wifi_fixed = inet_addr(e);
    if ((e = getenv("HIBYSYNC_IDLE_S")) && atoi(e) > 0) SP.t_idle = atoi(e);
    if ((e = getenv("HIBYSYNC_PUT_IDLE_S")) && atoi(e) > 0) SP.t_put_idle = atoi(e);
    if ((e = getenv("HIBYSYNC_SECURE_S")) && atoi(e) > 0) SP.t_secure = atoi(e);
#endif
    snprintf(SP.sync, sizeof SP.sync, "%s/sync", SP.hm);
    snprintf(SP.dbdir, sizeof SP.dbdir, "%s/db", SP.sync);
    snprintf(SP.ota, sizeof SP.ota, "%s/ByOS/ota", SP.sd);
}

/* ================================================================== global state */
struct peer { uint8_t Cs[32], key_id[16]; long long when; char name[BTP_NAME_MAX + 1]; };
struct ratel { uint32_t addr; time_t start, blocked_until; int fails; };
static struct {
    int ctl;                  /* control pipe/socket to the shell, -1 = none */
    int wifi, idle_exit, detach;
    char name[BTP_NAME_MAX + 1];
    uint8_t ds[32], Ds[32], device_id[8]; int have_key;
    struct peer peers[MAX_PEERS]; int npeers;
    int pairing_open; time_t pairing_until; int pair_fails; time_t pair_fail_start; int pairing_sid;
    int sd_shell;             /* SD state reported by the shell (-1 = not reported) */
    int up_state, up_permille, up_error; char up_version[24];
    int rescan_sid;
    volatile sig_atomic_t term;
    int quit;
    int lst_lo, lst_wifi, udp; uint32_t wifi_addr, wifi_mask; time_t wifi_checked;
    time_t idle_since; int next_sid;
    struct ratel rl[8];
    time_t probe_sec; int probe_n;
    int oom_high;
    char byos_version[32], firmware[32];
} G = { .ctl = -1, .lst_lo = -1, .lst_wifi = -1, .udp = -1, .sd_shell = -1 };

/* ================================================================== sessions */
enum { SS_FREE, SS_HELLO, SS_SECURING, SS_OPEN };
enum { PS_NONE, PS_COMMITTED, PS_WAIT_USER, PS_AUTH_WAIT };
enum { JOB_NONE, JOB_GET, JOB_LIST, JOB_RESCAN, JOB_PUTHASH };
enum { HJ_SKIP = 1, HJ_RESUME };   /* JOB_PUTHASH: SKIP_IDENTICAL compare, or rehash of a kept part */
#define RXCAP (4 + BTP_MAX_RECORD)
#define TXCAP (4 + BTP_MAX_RECORD + 1024)
#define PG_ROUND(n) ((((n) + PGA - 1) / PGA) * PGA)
#define RXSTRIDE PG_ROUND(RXCAP)
#define TXSTRIDE PG_ROUND(TXCAP)
/* Page-aligned, page-multiple strides: sess_close() gives the pages back with MADV_DONTNEED,
 * so the idle daemon does not keep the buffers of its last transfer resident. */
static uint8_t RXB[MAX_SESS][RXSTRIDE] __attribute__((aligned(PGA)));
static uint8_t TXB[MAX_SESS][TXSTRIDE] __attribute__((aligned(PGA)));
static uint8_t HOLDB[MAX_SESS][PG_ROUND(HOLDCAP)] __attribute__((aligned(PGA)));
static uint8_t FBUF[65536] __attribute__((aligned(PGA)));   /* file hashing */

struct put_state {
    int active, fd, dfd, ns; uint32_t stream; int mode;
    uint64_t size, next, since_sync; int64_t mtime;
    uint8_t sha[32]; struct ota_sha256 h;
    char part[32], leaf[BTP_COMP_MAX + 1], shown[160];
    time_t last;
};
struct get_state {
    int fd, want_sha, step, unlink_after; uint64_t size, off, pre, pre_end; int64_t mtime; struct ota_sha256 h;   /* resume with sha: prefix [0,pre_end) hashed up to pre */
    char unlink_path[400], shown[160];
};
struct list_state { DIR *d; int hidden; int have_pend; uint8_t pkind; uint64_t psize; int64_t pmtime; char pname[256]; };
struct sess {
    int st, fd, sid, local, enc; uint32_t peer_addr;
    uint32_t caps;
    uint8_t k_rx[32], k_tx[32]; uint64_t ctr_rx, ctr_tx;
    uint8_t hc[32], hd[32];
    char cname[BTP_NAME_MAX + 1];
    uint8_t *rx; size_t rx_have, rx_need; int rx_phase;   /* 0 = header, 1 = body, 2 = frame ready */
    int f_type, f_flags; uint32_t f_stream, f_len; uint8_t *f_pay;
    struct { int type, flags; uint32_t stream, len, off; } hq[HOLD_MAX]; int hq_n; uint32_t hq_bytes;   /* pipelined requests (FIFO) */
    struct { int fd, kind; uint64_t pos, len; } hj;   /* JOB_PUTHASH */
    uint8_t *tx; size_t tx_off, tx_len; int sf_fd; off_t sf_off; size_t sf_left; int close_after_tx;
    int ps; uint8_t commit[32], de[32], De[32], Nd[16], Cs[32]; int confirmed, allowed; time_t pair_deadline;
    char pname[BTP_NAME_MAX + 1];
    uint8_t key_id[16]; int peer_idx;
    int job; uint32_t job_stream;
    struct put_state put; struct get_state get; struct list_state list;
    int db_put_ok;
    time_t last_rx, hello_deadline, secure_deadline, xfer_last_ms;
    long long xfer_ms;
};
static struct sess S[MAX_SESS];

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static time_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

/* ================================================================== control pipe */
static void pct(const char *s, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 4 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7E || c == '%') k += (size_t)snprintf(out + k, cap - k, "%%%02X", c);
        else out[k++] = (char)c;
    }
    out[k] = 0;
}
static void ctl_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ctl_send(const char *fmt, ...)
{
    if (G.ctl < 0) return;
    char b[520]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, a); va_end(a);
    if (n < 0) return;
    if (n > 511) n = 511;
    b[n++] = '\n';
    for (int off = 0; off < n; ) {
        ssize_t w = write(G.ctl, b + off, (size_t)(n - off));
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) { struct pollfd p = { G.ctl, POLLOUT, 0 }; if (poll(&p, 1, 1000) <= 0) return; continue; }
        if (w <= 0) return;
        off += (int)w;
    }
}

/* ================================================================== small file helpers */
static int read_small(const char *p, char *b, int cap)
{
    int f = open(p, O_RDONLY | O_CLOEXEC); if (f < 0) return -1;
    int n = (int)read(f, b, (size_t)cap - 1); close(f); if (n < 0) return -1; b[n] = 0; return n;
}
static long read_long(const char *p, long def) { char b[32]; return read_small(p, b, sizeof b) > 0 ? strtol(b, 0, 10) : def; }
static int write_atomic(const char *path, const void *d, size_t n, int mode)
{
    char tmp[400]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode); if (f < 0) return -1;
    const uint8_t *p = d; size_t off = 0;
    while (off < n) { ssize_t w = write(f, p + off, n - off); if (w <= 0) { if (w < 0 && errno == EINTR) continue; close(f); unlink(tmp); return -1; } off += (size_t)w; }
    if (fsync(f) || close(f)) { unlink(tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    char dir[400]; snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; int df = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (df >= 0) { fsync(df); close(df); } }
    return 0;
}
static void fsync_fd_dir(int dfd) { if (dfd >= 0) fsync(dfd); }
static int sha256_fd_range(int fd, uint64_t len, struct ota_sha256 *h)   /* hash [0,len) of fd into h */
{
    uint64_t off = 0;
    while (off < len) {
        if (G.term) return -1;   /* SIGTERM: give up, the main loop shuts down */
        size_t want = len - off > sizeof FBUF ? sizeof FBUF : (size_t)(len - off);
        ssize_t r = pread(fd, FBUF, want, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        ota_sha256_update(h, FBUF, (size_t)r); off += (uint64_t)r;
    }
    return 0;
}
/* one step of an incremental hash: at most HASH_STEP bytes of [*pos, end) */
static int sha256_fd_step(int fd, uint64_t *pos, uint64_t end, struct ota_sha256 *h)
{
    uint64_t done = 0;
    while (done < HASH_STEP && *pos < end) {
        size_t want = end - *pos > sizeof FBUF ? sizeof FBUF : (size_t)(end - *pos);
        ssize_t r = pread(fd, FBUF, want, (off_t)*pos);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        ota_sha256_update(h, FBUF, (size_t)r); *pos += (uint64_t)r; done += (uint64_t)r;
    }
    return 0;
}
static int sha256_path(const char *p, uint8_t out[32], uint64_t *size)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    struct stat st; if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    struct ota_sha256 h; ota_sha256_init(&h);
    int r = sha256_fd_range(fd, (uint64_t)st.st_size, &h); close(fd);
    if (r) return -1;
    ota_sha256_final(&h, out); if (size) *size = (uint64_t)st.st_size;
    return 0;
}
static int mkdirs(const char *path, int mode)
{
    char b[400]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(b, (mode_t)mode) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(b, (mode_t)mode) && errno != EEXIST ? -1 : 0;
}
static long long free_bytes(const char *p) { struct statvfs v; return statvfs(p, &v) ? -1LL : (long long)v.f_bavail * (long long)v.f_frsize; }

/* ================================================================== SD state (9.3) */
enum { SD_MOUNTED = 0, SD_NOCARD = 1, SD_USB = 2, SD_RO = 3 };
static int sd_state(int *fs)
{
    if (fs) *fs = 0;
    if (G.sd_shell == SD_USB || G.sd_shell == SD_NOCARD) return G.sd_shell;
    FILE *f = fopen(SP.mounts, "r"); if (!f) return SD_NOCARD;
    char l[512], dev[128], mp[300], type[32], opts[256]; int st = SD_NOCARD;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "%127s %299s %31s %255s", dev, mp, type, opts) != 4) continue;
        if (strcmp(mp, SP.sd) && strcmp(mp, "/data/mnt/sd_0")) continue;
        st = !strncmp(opts, "ro", 2) && (opts[2] == ',' || !opts[2]) ? SD_RO : SD_MOUNTED;
        if (fs) *fs = !strcmp(type, "vfat") ? 1 : !strcmp(type, "exfat") ? 2 : 0;
    }
    fclose(f);
#ifndef BTP_TEST
    if (st == SD_MOUNTED || st == SD_RO) {   /* really a mount point (not the empty internal folder) */
        struct stat a, b; char parent[300]; snprintf(parent, sizeof parent, "%s/..", SP.sd);
        if (stat(SP.sd, &a) || stat(parent, &b) || a.st_dev == b.st_dev) st = SD_NOCARD;
    }
#endif
    if (st == SD_NOCARD && G.sd_shell == SD_RO) st = SD_RO;
    return st;
}
static int sd_ok(void) { return sd_state(0) == SD_MOUNTED; }

/* ================================================================== versions, battery */
static void first_line(const char *p, char *out, int cap)
{
    out[0] = 0; char b[256]; if (read_small(p, b, sizeof b) <= 0) return;
    int k = 0; for (int i = 0; b[i] && b[i] != '\n' && k < cap - 1; i++) if (!isspace((unsigned char)b[i])) out[k++] = b[i];
    out[k] = 0;
}
static void firmware_version(char *out, int cap)   /* "ByOS X.Y.Z" line of /usr/bin/hibymod, as the OTA page */
{
    out[0] = 0;
    size_t n; char *b = ota_read_file(SP.cli, 256 * 1024, &n); if (!b) return;
    char *p = strstr(b, "echo \"ByOS ");
    if (p) p += 11; else if ((p = strstr(b, "echo \"HiByOS "))) p += 13;
    if (p) { int k = 0; while (k < cap - 1 && (isalnum((unsigned char)p[k]) || p[k] == '.' || p[k] == '-')) { out[k] = p[k]; k++; } out[k] = 0; }
    free(b);
}
static void versions_refresh(void)
{
    char p[300]; snprintf(p, sizeof p, "%s/payload_version", SP.hm);
    first_line(p, G.byos_version, sizeof G.byos_version);
    if (!G.firmware[0]) firmware_version(G.firmware, sizeof G.firmware);   /* the rootfs changes only with a reboot */
}

/* ================================================================== keys and peers (8.1) */
static void hexs(const uint8_t *b, int n, char *out) { ota_hex(b, n, out); out[2 * n] = 0; }
static int key_load_or_create(void)
{
    char p[400]; snprintf(p, sizeof p, "%s/device.key", SP.sync);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        ssize_t r = read(fd, G.ds, 32); close(fd);
        if (r == 32) goto done;
        lg("device.key unreadable, making a new one");
    }
    if (mkdirs(SP.sync, 0700)) return -1;
    if (btp_random(G.ds, 32)) return -1;
    if (write_atomic(p, G.ds, 32, 0600)) return -1;
done:
    btp_x25519_base(G.Ds, G.ds);
    btp_device_id(G.Ds, G.device_id);
    G.have_key = 1;
    return 0;
}
static void unpct(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = { s[1], s[2], 0 }; out[k++] = (char)strtol(h, 0, 16); s += 2; }
        else out[k++] = *s;
    }
    out[k] = 0;
}
static void peers_load(void)
{
    char p[400]; snprintf(p, sizeof p, "%s/peers", SP.sync);
    G.npeers = 0;
    FILE *f = fopen(p, "r"); if (!f) return;
    char l[400], hx[80], nm[300]; long long t;
    while (G.npeers < MAX_PEERS && fgets(l, sizeof l, f)) {
        nm[0] = 0;
        if (sscanf(l, "%79s %lld %299s", hx, &t, nm) < 2 || strlen(hx) != 64) continue;
        struct peer *e = &G.peers[G.npeers];
        if (ota_unhex(hx, e->Cs, 32)) continue;
        e->when = t; unpct(nm, e->name, sizeof e->name);
        if (!btp_utf8_ok((const uint8_t *)e->name, strlen(e->name))) e->name[0] = 0;
        btp_key_id(e->Cs, e->key_id);
        G.npeers++;
    }
    fclose(f);
}
static int peers_save(void)
{
    char p[400], b[MAX_PEERS * 300]; size_t k = 0;
    snprintf(p, sizeof p, "%s/peers", SP.sync);
    for (int i = 0; i < G.npeers; i++) {
        char hx[65], nm[200]; hexs(G.peers[i].Cs, 32, hx); pct(G.peers[i].name, strlen(G.peers[i].name), nm, sizeof nm);
        k += (size_t)snprintf(b + k, sizeof b - k, "%s %lld %s\n", hx, G.peers[i].when, nm[0] ? nm : "-");
    }
    if (mkdirs(SP.sync, 0700)) return -1;
    return write_atomic(p, b, k, 0600);
}
static int peer_find(const uint8_t key_id[16])
{
    for (int i = 0; i < G.npeers; i++) if (btp_ct_eq(G.peers[i].key_id, key_id, 16)) return i;
    return -1;
}
static int peer_add(const uint8_t Cs[32], const char *name)
{
    uint8_t id[16]; btp_key_id(Cs, id);
    int i = peer_find(id);
    if (i < 0) {
        if (G.npeers == MAX_PEERS) {   /* replace the oldest (the user confirmed on the device) */
            int o = 0; for (int k = 1; k < G.npeers; k++) if (G.peers[k].when < G.peers[o].when) o = k;
            G.peers[o] = G.peers[--G.npeers];
        }
        i = G.npeers++;
    }
    struct peer *e = &G.peers[i];
    memcpy(e->Cs, Cs, 32); memcpy(e->key_id, id, 16); e->when = (long long)time(0);
    snprintf(e->name, sizeof e->name, "%s", name);
    if (peers_save()) lg("could not save peers");
    return i;
}

/* ================================================================== frames out */
static uint8_t *tx_payload(struct sess *s) { return s->tx + s->tx_len + (s->enc ? BTP_REC_HDR : 0) + BTP_HDR; }
static size_t tx_room(struct sess *s) { size_t used = s->tx_len + (s->enc ? BTP_REC_HDR : 0) + BTP_HDR; return used >= TXCAP ? 0 : TXCAP - used; }
static void tx_commit(struct sess *s, int type, int flags, uint32_t stream, size_t plen)
{
    uint8_t *base = s->tx + s->tx_len;
    if (!s->enc) { btp_hdr_build(base, type, flags, stream, (uint32_t)plen); s->tx_len += BTP_HDR + plen; return; }
    uint8_t *fr = base + BTP_REC_HDR, nonce[24];
    btp_hdr_build(fr, type, flags, stream, (uint32_t)plen);
    size_t flen = BTP_HDR + plen;
    btp_nonce(nonce, DIR_D2C, s->ctr_tx++);
    btp_box_seal(base + 4, fr, flen, nonce, s->k_tx);
    be32(base, (uint32_t)(BTP_TAG + flen));
    s->tx_len += BTP_REC_HDR + flen;
}
static void tx_frame(struct sess *s, int type, int flags, uint32_t stream, const void *p, size_t n)
{
    if (tx_room(s) < n || s->sf_left) { lg("tx overflow"); s->close_after_tx = 1; return; }
    if (n) memmove(tx_payload(s), p, n);
    tx_commit(s, type, flags, stream, n);
}
static void tx_end(struct sess *s, int type, uint32_t stream, const void *p, size_t n) { tx_frame(s, type, BTP_FLAG_END, stream, p, n); }
static void tx_ok(struct sess *s, uint32_t stream) { tx_end(s, T_OK, stream, 0, 0); }
static const char *err_text(int code)
{
    switch (code) {
    case E_PROTO: return "malformed frame or payload"; case E_VERSION: return "no common protocol version";
    case E_UNKNOWN_TYPE: return "unknown message type"; case E_AUTH_REQUIRED: return "authentication required";
    case E_AUTH_FAILED: return "authentication failed"; case E_PAIR_CLOSED: return "pairing is not open";
    case E_PAIR_REJECTED: return "pairing rejected"; case E_BUSY: return "busy"; case E_TOO_LARGE: return "frame too large";
    case E_RATE_LIMIT: return "too many attempts"; case E_TIMEOUT: return "timeout"; case E_CANCELLED: return "cancelled";
    case E_UNSUPPORTED: return "unsupported"; case E_INTERNAL: return "internal error"; case E_BAD_PATH: return "bad path";
    case E_NOT_FOUND: return "no such file"; case E_EXISTS: return "already exists"; case E_NOT_DIR: return "not a folder";
    case E_IS_DIR: return "is a folder"; case E_NOT_EMPTY: return "folder not empty"; case E_NO_SPACE: return "not enough space";
    case E_IO: return "i/o error"; case E_SD_UNAVAILABLE: return "sd card unavailable"; case E_HASH_MISMATCH: return "content does not match";
    case E_OFFSET: return "bad offset"; case E_FILE_TOO_BIG: return "file too big"; case E_NOT_ALLOWED: return "not allowed";
    case E_SIGNATURE: return "bad signature"; case E_UPDATE_INVALID: return "invalid update"; case E_DOWNGRADE: return "downgrade";
    case E_BATTERY: return "battery low"; case E_NEEDS_FIRMWARE: return "needs newer firmware"; case E_NOT_STAGED: return "nothing staged";
    case E_DB_INVALID: return "invalid database"; case E_DB_STALE: return "library changed";
    }
    return "error";
}
static void tx_err(struct sess *s, uint32_t stream, int code)
{
    uint8_t p[2 + 2 + 200]; const char *m = err_text(code); size_t l = strlen(m);
    be16(p, (uint32_t)code); be16(p + 2, (uint32_t)l); memcpy(p + 4, m, l);
    tx_end(s, T_ERROR, stream, p, 4 + l);
}
static void fatal(struct sess *s, uint32_t stream, int code) { tx_err(s, stream, code); s->close_after_tx = 1; }

/* ================================================================== namespaces and confinement (9) */
enum { OP_STAT, OP_LIST, OP_MKDIR, OP_MOVE, OP_DELETE, OP_PUT, OP_GET };
struct target { char root[400]; char rel[BTP_PATH_MAX + 1]; long long limit; int fs; };
/* map (ns, path) to a root folder and a relative path; 0 or an error code */
static int ns_resolve(int ns, int op, const uint8_t *p, size_t n, struct target *t)
{
    int allow_empty = op == OP_STAT || op == OP_LIST;
    t->limit = -1; t->fs = 0;
    if (ns == NS_SD) {
        if (btp_path_check(p, n, allow_empty)) return E_BAD_PATH;
        if (sd_state(&t->fs) != SD_MOUNTED) return E_SD_UNAVAILABLE;
        snprintf(t->root, sizeof t->root, "%s", SP.sd);
        memcpy(t->rel, p, n); t->rel[n] = 0;
        if (t->fs == 1) t->limit = 4294967295LL;
        return 0;
    }
    if (ns != NS_UPDATE && ns != NS_DB) return E_NOT_ALLOWED;
    if (op != OP_PUT && op != OP_STAT && op != OP_DELETE) return E_NOT_ALLOWED;
    if (btp_path_check(p, n, 0) || memchr(p, '/', n)) return E_BAD_PATH;
    char name[BTP_COMP_MAX + 1]; memcpy(name, p, n); name[n] = 0;
    if (ns == NS_UPDATE) {
        if (!strcmp(name, "hibyos-update.json")) { snprintf(t->rel, sizeof t->rel, "sync-update.json"); t->limit = BTP_NS_UPDATE_JSON_MAX; }
        else if (!strncmp(name, "hibyos-payload-", 15) && ota_name_ok(name)) { snprintf(t->rel, sizeof t->rel, "%s", name); t->limit = BTP_NS_UPDATE_TAR_MAX; }
        else return E_BAD_PATH;
        if (sd_state(&t->fs) != SD_MOUNTED) return E_SD_UNAVAILABLE;
        snprintf(t->root, sizeof t->root, "%s", SP.ota);
        if (op == OP_PUT && mkdirs(t->root, 0755)) return E_IO;
        return 0;
    }
    if (strcmp(name, "staged.db")) return E_BAD_PATH;
    snprintf(t->rel, sizeof t->rel, "incoming.db"); t->limit = BTP_NS_DB_MAX;
    snprintf(t->root, sizeof t->root, "%s", SP.dbdir);
    if (op == OP_PUT && mkdirs(t->root, 0700)) return E_IO;
    return 0;
}
static int errno_code(int e)
{
    switch (e) {
    case ENOENT: return E_NOT_FOUND; case EEXIST: return E_EXISTS; case ENOTDIR: return E_NOT_DIR; case EISDIR: return E_IS_DIR;
    case ENOTEMPTY: return E_NOT_EMPTY; case ENOSPC: return E_NO_SPACE; case ELOOP: return E_BAD_PATH; case EFBIG: return E_FILE_TOO_BIG;
    case EROFS: return E_SD_UNAVAILABLE;
    }
    return E_IO;
}
/* opens the parent folder of t->rel, one component at a time without following links.
 * *leaf points into t->rel (empty for the root). mkparents: create missing folders (0755). */
static int walk_parent(struct target *t, int mkparents, int *dfd_out, char **leaf)
{
    int dfd = open(t->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return errno == ENOENT ? E_NOT_FOUND : E_IO;
    char *c = t->rel, *sl;
    while ((sl = strchr(c, '/'))) {
        *sl = 0;
        int nd = openat(dfd, c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nd < 0 && errno == ENOENT && mkparents) {
            if (mkdirat(dfd, c, 0755) && errno != EEXIST) { int e = errno; *sl = '/'; close(dfd); return errno_code(e); }
            fsync(dfd);
            nd = openat(dfd, c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (nd >= 0) fsync(nd);
        }
        if (nd < 0) {
            int e = errno, code = errno_code(e);
            if (e == ENOTDIR || e == ELOOP) { struct stat st; code = !fstatat(dfd, c, &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode) ? E_BAD_PATH : E_NOT_DIR; }
            *sl = '/'; close(dfd); return code;
        }
        *sl = '/';
        close(dfd); dfd = nd; c = sl + 1;
    }
    *dfd_out = dfd; *leaf = c;
    return 0;
}
/* lstat of the leaf: 0, E_NOT_FOUND, E_BAD_PATH (symlink) or E_IO */
static int leaf_stat(int dfd, const char *leaf, struct stat *st)
{
    if (!*leaf) return fstat(dfd, st) ? E_IO : 0;
    if (fstatat(dfd, leaf, st, AT_SYMLINK_NOFOLLOW)) return errno_code(errno);
    return S_ISLNK(st->st_mode) ? E_BAD_PATH : 0;
}
static int st_kind(const struct stat *st) { return S_ISREG(st->st_mode) ? K_FILE : S_ISDIR(st->st_mode) ? K_DIR : K_OTHER; }
/* rename without replacing (renameat2 when the kernel has it, else check + rename: the daemon is
 * single threaded and the only writer that uses this, see section 10) */
static int rename_noreplace(int sdfd, const char *s, int ddfd, const char *d)
{
#ifdef SYS_renameat2
    if (!syscall(SYS_renameat2, sdfd, s, ddfd, d, 1 /* RENAME_NOREPLACE */)) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    struct stat st;
    if (!fstatat(ddfd, d, &st, AT_SYMLINK_NOFOLLOW)) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    return renameat(sdfd, s, ddfd, d);
}

/* ================================================================== oom / progress */
static void oom_adjust(void)
{
    int busy = 0;
    for (int i = 0; i < MAX_SESS; i++) if (S[i].st && (S[i].put.active || S[i].job == JOB_GET)) busy = 1;
    if (busy == G.oom_high) return;
    G.oom_high = busy;
#ifndef BTP_TEST
    int f = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (f >= 0) { const char *v = busy ? "-700" : "0"; if (write(f, v, strlen(v)) < 0) {} close(f); }
#endif
}
static void xfer(struct sess *s, const char *kind, uint64_t done, uint64_t total, const char *shown, int force)
{
    long long t = now_ms();
    if (!force && t - s->xfer_ms < 250) return;
    s->xfer_ms = t;
    ctl_send("XFER %d %s %llu %llu %s", s->sid, kind, (unsigned long long)done, (unsigned long long)total, shown);
}

/* ================================================================== requests: files (10) */
static int need(struct sess *s, uint32_t cap) { return (s->caps & cap) != 0; }

static void do_stat(struct sess *s, uint32_t st_, struct brd *r)
{
    int ns = (int)brd_u8(r); size_t n; const uint8_t *p = brd_str16(r, &n);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    struct target t; int e = ns_resolve(ns, OP_STAT, p, n, &t), dfd = -1; char *leaf;
    if (!e) e = walk_parent(&t, 0, &dfd, &leaf);
    struct stat st;
    if (!e) e = leaf_stat(dfd, leaf, &st);
    if (dfd >= 0) close(dfd);
    if (e) { tx_err(s, st_, e); return; }
    uint8_t o[17]; o[0] = (uint8_t)st_kind(&st);
    be64(o + 1, S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size); be64(o + 9, (uint64_t)(int64_t)st.st_mtime);
    tx_end(s, T_STAT_R, st_, o, sizeof o);
}
static void do_df(struct sess *s, uint32_t st_, struct brd *r)
{
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    int fs = 0, sst = sd_state(&fs);
    uint64_t st = 0, sf = 0, dt = 0, df = 0; struct statvfs v;
    if ((sst == SD_MOUNTED || sst == SD_RO) && !statvfs(SP.sd, &v)) { st = (uint64_t)v.f_blocks * v.f_frsize; sf = (uint64_t)v.f_bavail * v.f_frsize; }
    if (!statvfs(SP.data, &v)) { dt = (uint64_t)v.f_blocks * v.f_frsize; df = (uint64_t)v.f_bavail * v.f_frsize; }
    if (sst != SD_MOUNTED && sst != SD_RO) fs = 0;
    uint8_t o[34]; o[0] = (uint8_t)sst; o[1] = (uint8_t)fs; be64(o + 2, st); be64(o + 10, sf); be64(o + 18, dt); be64(o + 26, df);
    tx_end(s, T_DF_R, st_, o, sizeof o);
}
static void do_list(struct sess *s, uint32_t st_, struct brd *r)
{
    int ns = (int)brd_u8(r), fl = (int)brd_u8(r); size_t n; const uint8_t *p = brd_str16(r, &n);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    struct target t; int e = ns == NS_SD ? ns_resolve(ns, OP_LIST, p, n, &t) : E_NOT_ALLOWED, dfd = -1; char *leaf;
    if (!e) e = walk_parent(&t, 0, &dfd, &leaf);
    int d = -1;
    if (!e) {
        if (!*leaf) { d = dfd; dfd = -1; }
        else {
            d = openat(dfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (d < 0) { struct stat st; e = leaf_stat(dfd, leaf, &st); if (!e) e = S_ISDIR(st.st_mode) ? E_IO : E_NOT_DIR; }
        }
    }
    if (dfd >= 0) close(dfd);
    if (e) { tx_err(s, st_, e); return; }
    s->list.d = fdopendir(d);
    if (!s->list.d) { close(d); tx_err(s, st_, E_IO); return; }
    s->list.hidden = fl & 1; s->list.have_pend = 0;
    s->job = JOB_LIST; s->job_stream = st_;
}
static void job_end(struct sess *s);
static void list_step(struct sess *s)
{
    uint8_t *pay = tx_payload(s); size_t cap = BTP_MAX_PAYLOAD, n = 2; int count = 0, end = 0;
    for (;;) {
        if (!s->list.have_pend) {
            struct dirent *de = readdir(s->list.d);
            if (!de) { end = 1; break; }
            const char *nm = de->d_name; size_t l = strlen(nm);
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            if (nm[0] == '.' && !s->list.hidden) continue;
            if (!btp_utf8_ok((const uint8_t *)nm, l) || l > 255) continue;
            struct stat st; if (fstatat(dirfd(s->list.d), nm, &st, AT_SYMLINK_NOFOLLOW)) continue;
            s->list.pkind = (uint8_t)st_kind(&st); s->list.psize = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
            s->list.pmtime = (int64_t)st.st_mtime; memcpy(s->list.pname, nm, l + 1); s->list.have_pend = 1;
        }
        size_t l = strlen(s->list.pname), need_ = 1 + 8 + 8 + 2 + l;
        if (n + need_ > cap || count == 65535) break;
        uint8_t *q = pay + n; q[0] = s->list.pkind; be64(q + 1, s->list.psize); be64(q + 9, (uint64_t)s->list.pmtime);
        be16(q + 17, (uint32_t)l); memcpy(q + 19, s->list.pname, l);
        n += need_; count++; s->list.have_pend = 0;
    }
    be16(pay, (uint32_t)count);
    tx_commit(s, T_LIST_R, end ? BTP_FLAG_END : 0, s->job_stream, n);
    if (end) job_end(s);
}
static void do_mkdir(struct sess *s, uint32_t st_, struct brd *r)
{
    int ns = (int)brd_u8(r), fl = (int)brd_u8(r); size_t n; const uint8_t *p = brd_str16(r, &n);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    struct target t; int e = ns == NS_SD ? ns_resolve(ns, OP_MKDIR, p, n, &t) : E_NOT_ALLOWED, dfd = -1; char *leaf;
    if (!e) e = walk_parent(&t, fl & 1, &dfd, &leaf);
    if (!e) {
        if (mkdirat(dfd, leaf, 0755)) {
            e = errno_code(errno);
            struct stat st;
            if (e == E_EXISTS && (fl & 1)) e = leaf_stat(dfd, leaf, &st) ? E_EXISTS : S_ISDIR(st.st_mode) ? 0 : E_EXISTS;
        } else {
            int nd = openat(dfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); if (nd >= 0) { fsync(nd); close(nd); }
            fsync_fd_dir(dfd);
        }
    }
    if (dfd >= 0) close(dfd);
    if (e) tx_err(s, st_, e); else tx_ok(s, st_);
}
static void do_move(struct sess *s, uint32_t st_, struct brd *r)
{
    int ns = (int)brd_u8(r), fl = (int)brd_u8(r); size_t sn, dn; const uint8_t *sp = brd_str16(r, &sn), *dp = brd_str16(r, &dn);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    static struct target a, b; int e = ns == NS_SD ? ns_resolve(ns, OP_MOVE, sp, sn, &a) : E_NOT_ALLOWED;
    if (!e) e = ns_resolve(ns, OP_MOVE, dp, dn, &b);
    if (!e && dn > sn && !memcmp(dp, sp, sn) && dp[sn] == '/') e = E_BAD_PATH;   /* into itself */
    int sd = -1, dd = -1; char *sl, *dl; struct stat st, dt;
    if (!e) e = walk_parent(&a, 0, &sd, &sl);
    if (!e) e = leaf_stat(sd, sl, &st);
    if (!e) e = walk_parent(&b, 0, &dd, &dl);
    if (!e) {
        int de = leaf_stat(dd, dl, &dt);
        if (de == E_BAD_PATH) e = E_BAD_PATH;
        else if (!de && !(fl & 1)) e = E_EXISTS;
        else if (!de && S_ISDIR(dt.st_mode) != S_ISDIR(st.st_mode)) e = S_ISDIR(dt.st_mode) ? E_IS_DIR : E_NOT_DIR;
    }
    if (!e) {
        int rc = (fl & 1) ? renameat(sd, sl, dd, dl) : rename_noreplace(sd, sl, dd, dl);
        if (rc) e = errno_code(errno);
        else { fsync_fd_dir(sd); fsync_fd_dir(dd); }
    }
    if (sd >= 0) close(sd);
    if (dd >= 0) close(dd);
    if (e) tx_err(s, st_, e); else tx_ok(s, st_);
}
static int rm_tree(int dfd, const char *name, int depth)
{
    if (depth > 64) return E_IO;
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return errno_code(errno);
    DIR *d = fdopendir(fd); if (!d) { close(fd); return E_IO; }
    int e = 0;
    for (int pass = 0; pass < 2; pass++) {
        struct dirent *de; int any = 0;
        rewinddir(d);
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            struct stat st; if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            any = 1;
            int x = S_ISDIR(st.st_mode) ? rm_tree(dirfd(d), de->d_name, depth + 1) : (unlinkat(dirfd(d), de->d_name, 0) ? errno_code(errno) : 0);
            if (x && !e) e = x;
        }
        if (!any || e) break;
    }
    closedir(d);
    if (!e && unlinkat(dfd, name, AT_REMOVEDIR)) e = errno_code(errno);
    return e;
}
static void do_delete(struct sess *s, uint32_t st_, struct brd *r)
{
    int ns = (int)brd_u8(r), fl = (int)brd_u8(r); size_t n; const uint8_t *p = brd_str16(r, &n);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    struct target t; int e = ns_resolve(ns, OP_DELETE, p, n, &t), dfd = -1; char *leaf; struct stat st;
    if (!e) e = walk_parent(&t, 0, &dfd, &leaf);
    if (!e && !*leaf) e = E_BAD_PATH;
    if (!e) e = leaf_stat(dfd, leaf, &st);
    if (!e) {
        if (S_ISDIR(st.st_mode)) {
            if (fl & 1) e = rm_tree(dfd, leaf, 0);
            else if (unlinkat(dfd, leaf, AT_REMOVEDIR)) e = errno == EEXIST ? E_NOT_EMPTY : errno_code(errno);
        } else if (unlinkat(dfd, leaf, 0)) e = errno_code(errno);
        if (!e) fsync_fd_dir(dfd);
        if (!e && ns == NS_DB) s->db_put_ok = 0;
    }
    if (dfd >= 0) close(dfd);
    if (e) tx_err(s, st_, e); else tx_ok(s, st_);
}

/* ================================================================== PUT (11) */
static void put_close(struct sess *s, int remove_part)
{
    struct put_state *u = &s->put;
    if (!u->active) return;
    if (u->fd >= 0) { if (!remove_part) fdatasync(u->fd); close(u->fd); }
    if (remove_part) unlinkat(u->dfd, u->part, 0);
    if (u->dfd >= 0) close(u->dfd);
    u->fd = u->dfd = -1; u->active = 0;
    oom_adjust();
}
static void stale_parts(int dfd, const char *keep)   /* .btp-*.part older than 7 days in this folder */
{
    int fd = openat(dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (fd < 0) return;
    DIR *d = fdopendir(fd); if (!d) { close(fd); return; }
    struct dirent *de; time_t lim = time(0) - 7 * 86400;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l != 26 || strncmp(de->d_name, ".btp-", 5) || strcmp(de->d_name + 21, ".part") || !strcmp(de->d_name, keep)) continue;
        struct stat st;
        if (!fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && st.st_mtime < lim) unlinkat(dirfd(d), de->d_name, 0);
    }
    closedir(d);
}
/* Another session holding the same .part (same folder inode, same name): a client that
 * reconnected before its old session timed out, or two apps uploading the same file. The new
 * PUT_BEGIN takes the part over: the old put is closed (data synced, part kept) and gets
 * E_CANCELLED, so it can never rename a part that the new one is truncating. */
static void part_takeover(struct sess *s, int dfd, const char *part)
{
    struct stat a; if (fstat(dfd, &a)) return;
    for (int i = 0; i < MAX_SESS; i++) {
        struct sess *o = &S[i];
        if (o == s || !o->st) continue;
        int holds = o->put.active || (o->job == JOB_PUTHASH && o->hj.kind == HJ_RESUME);
        if (!holds || o->put.dfd < 0 || strcmp(o->put.part, part)) continue;
        struct stat b; if (fstat(o->put.dfd, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) continue;
        lg("session %d takes over the part of session %d", s->sid, o->sid);
        if (o->put.active) { uint32_t ps = o->put.stream; put_close(o, 0); tx_err(o, ps, E_CANCELLED); }
        else { uint32_t js = o->job_stream; job_end(o); tx_err(o, js, E_CANCELLED); }
    }
}
/* the put is ready for DATA: PUT_READY with the resume offset */
static void put_activate(struct sess *s)
{
    struct put_state *u = &s->put;
    u->active = 1; u->last = mono();
    uint8_t o[8]; be64(o, u->next); tx_frame(s, T_PUT_READY, 0, u->stream, o, 8);
    oom_adjust();
    xfer(s, "put", u->next, u->size, u->shown, 1);
}
static void do_put_begin(struct sess *s, uint32_t st_, struct brd *r)
{
    struct put_state *u = &s->put;
    int ns = (int)brd_u8(r), mode = (int)brd_u8(r); uint64_t size = brd_u64(r); int64_t mtime = (int64_t)brd_u64(r);
    const uint8_t *sha = brd_take(r, 32); size_t n; const uint8_t *p = brd_str16(r, &n);
    if (!brd_done(r) || mode > PM_SKIP_IDENTICAL) { tx_err(s, st_, E_PROTO); return; }
    static struct target t; int e = ns_resolve(ns, OP_PUT, p, n, &t), dfd = -1; char *leaf = 0;
    if (!e && t.limit >= 0 && size > (uint64_t)t.limit) e = E_FILE_TOO_BIG;
    if (!e) e = walk_parent(&t, 0, &dfd, &leaf);
    struct stat st;
    if (!e) {
        int le = leaf_stat(dfd, leaf, &st);
        if (le == E_BAD_PATH) e = E_BAD_PATH;
        else if (!le && S_ISDIR(st.st_mode)) e = E_IS_DIR;
        else if (!le && !S_ISREG(st.st_mode)) e = E_EXISTS;
        else if (!le && mode == PM_CREATE) e = E_EXISTS;
        else if (!le && mode == PM_SKIP_IDENTICAL) {
            /* same size: compare the content as a job (a few MiB per poll turn), then PUT_DONE(1) or E_EXISTS */
            int fd = (uint64_t)st.st_size == size ? openat(dfd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
            struct stat fs;
            if (fd >= 0 && !fstat(fd, &fs) && S_ISREG(fs.st_mode) && (uint64_t)fs.st_size == size) {
                memset(u, 0, sizeof *u); u->fd = -1; u->dfd = dfd; u->stream = st_; u->size = size; memcpy(u->sha, sha, 32);
                ota_sha256_init(&u->h);
                s->hj.fd = fd; s->hj.kind = HJ_SKIP; s->hj.pos = 0; s->hj.len = size;
                s->job = JOB_PUTHASH; s->job_stream = st_;
                return;
            }
            if (fd >= 0) close(fd);
            e = E_EXISTS;
        } else if (le && le != E_NOT_FOUND) e = le;
    }
    char part[32]; if (!e) btp_part_name(p, n, sha, size, part);
    if (!e) part_takeover(s, dfd, part);
    uint64_t psize = 0; int have_part = 0;
    if (!e) {
        struct stat ps;
        if (!fstatat(dfd, part, &ps, AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISREG(ps.st_mode)) e = E_IO; else { have_part = 1; psize = (uint64_t)ps.st_size; }
        }
    }
    if (!e) {
        struct statvfs v; uint64_t needb = (size > psize ? size - psize : 0) + (1u << 20);
        if (fstatvfs(dfd, &v) || (uint64_t)v.f_bavail * v.f_frsize < needb) e = E_NO_SPACE;
    }
    uint64_t off = 0; int fd = -1;
    if (!e) {
        stale_parts(dfd, part);
        if (have_part) {
            off = btp_resume_offset(psize, size);
            fd = openat(dfd, part, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0 || ftruncate(fd, (off_t)off)) e = E_IO;
        } else {
            fd = openat(dfd, part, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (fd < 0) e = errno_code(errno);
        }
    }
    if (e) { if (fd >= 0) close(fd); if (dfd >= 0) close(dfd); tx_err(s, st_, e); return; }
    u->active = 0; u->fd = fd; u->dfd = dfd; u->ns = ns; u->stream = st_; u->mode = mode;
    u->size = size; u->next = off; u->since_sync = 0; u->mtime = mtime; memcpy(u->sha, sha, 32);
    snprintf(u->part, sizeof u->part, "%s", part); snprintf(u->leaf, sizeof u->leaf, "%s", leaf);
    pct((const char *)p, n, u->shown, sizeof u->shown);
    ota_sha256_init(&u->h);
    if (off) {   /* rehash the kept part as a job, then PUT_READY */
        s->hj.fd = fd; s->hj.kind = HJ_RESUME; s->hj.pos = 0; s->hj.len = off;
        s->job = JOB_PUTHASH; s->job_stream = st_;
        return;
    }
    put_activate(s);
}
/* one step of JOB_PUTHASH; at the end: PUT_DONE(1) / E_EXISTS (skip) or PUT_READY (resume) */
static void puthash_step(struct sess *s)
{
    struct put_state *u = &s->put; uint32_t js = s->job_stream;
    if (sha256_fd_step(s->hj.fd, &s->hj.pos, s->hj.len, &u->h)) {
        int kind = s->hj.kind; job_end(s); tx_err(s, js, kind == HJ_SKIP ? E_EXISTS : E_IO); return;
    }
    if (s->hj.pos < s->hj.len) return;
    if (s->hj.kind == HJ_SKIP) {
        uint8_t d[32]; ota_sha256_final(&u->h, d); int same = btp_ct_eq(d, u->sha, 32); uint64_t size = u->size;
        job_end(s);
        if (same) { uint8_t o[9]; o[0] = 1; be64(o + 1, size); tx_end(s, T_PUT_DONE, js, o, 9); }
        else tx_err(s, js, E_EXISTS);
        return;
    }
    s->hj.fd = -1; s->job = JOB_NONE; s->last_rx = mono();   /* the fds now belong to the put */
    put_activate(s);
}
static void do_data(struct sess *s, uint32_t st_, const uint8_t *p, size_t n)
{
    struct put_state *u = &s->put;
    if (!u->active || u->stream != st_) { tx_err(s, st_, E_PROTO); return; }
    u->last = mono();
    if (n < 9 || n > 8 + BTP_MAX_DATA) { put_close(s, 0); tx_err(s, st_, E_PROTO); return; }
    uint64_t off = rd64(p); const uint8_t *d = p + 8; size_t l = n - 8;
    if (off != u->next) { put_close(s, 0); tx_err(s, st_, E_OFFSET); return; }
    if (u->next + l > u->size) { put_close(s, 1); tx_err(s, st_, E_HASH_MISMATCH); return; }
    size_t w = 0;
    while (w < l) {
        ssize_t k = pwrite(u->fd, d + w, l - w, (off_t)(off + w));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { int e = k < 0 ? errno_code(errno) : E_IO; if (e != E_NO_SPACE && e != E_SD_UNAVAILABLE) e = E_IO; put_close(s, 0); tx_err(s, st_, e); return; }
        w += (size_t)k;
    }
    ota_sha256_update(&u->h, d, l);
    u->next += l; u->since_sync += l;
    if (u->since_sync >= SYNC_EVERY) { fdatasync(u->fd); u->since_sync = 0; }
    xfer(s, "put", u->next, u->size, u->shown, 0);
}
static void do_put_end(struct sess *s, uint32_t st_, size_t n)
{
    struct put_state *u = &s->put;
    if (!u->active || u->stream != st_ || n) { if (u->active && u->stream == st_) put_close(s, 0); tx_err(s, st_, E_PROTO); return; }
    uint8_t d[32]; ota_sha256_final(&u->h, d);
    if (u->next != u->size || !btp_ct_eq(d, u->sha, 32)) { put_close(s, 1); tx_err(s, st_, E_HASH_MISMATCH); return; }
    int e = 0;
    if (fsync(u->fd)) e = E_IO;
    if (!e && u->ns != NS_DB && !sd_ok()) e = E_SD_UNAVAILABLE;
    if (!e) {
        int rc;
        if (u->mode == PM_REPLACE) {
            struct stat st;
            if (!fstatat(u->dfd, u->leaf, &st, AT_SYMLINK_NOFOLLOW) && (S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))) { errno = S_ISDIR(st.st_mode) ? EISDIR : ELOOP; rc = -1; }
            else rc = renameat(u->dfd, u->part, u->dfd, u->leaf);
        } else rc = rename_noreplace(u->dfd, u->part, u->dfd, u->leaf);
        if (rc) e = errno_code(errno);
    }
    if (e) { put_close(s, 0); tx_err(s, st_, e); return; }
    close(u->fd); u->fd = -1;
    fsync_fd_dir(u->dfd);
    if (u->mtime) {
        struct timespec ts[2] = { { (time_t)u->mtime, 0 }, { (time_t)u->mtime, 0 } };
        utimensat(u->dfd, u->leaf, ts, AT_SYMLINK_NOFOLLOW);
    }
    if (u->ns == NS_DB) s->db_put_ok = 1;
    xfer(s, "put", u->size, u->size, u->shown, 1);
    uint64_t size = u->size;
    close(u->dfd); u->dfd = -1; u->active = 0; oom_adjust();
    uint8_t o[9]; o[0] = 0; be64(o + 1, size); tx_end(s, T_PUT_DONE, st_, o, 9);
}

/* ================================================================== GET (12) and DB_SNAPSHOT (13.1) */
static int get_open(struct sess *s, uint32_t st_, int fd, uint64_t offset, int want_sha, const char *unlink_after, const char *shown)
{
    struct get_state *g = &s->get; struct stat st;
    if (fstat(fd, &st)) { close(fd); return E_IO; }
    if (S_ISDIR(st.st_mode)) { close(fd); return E_IS_DIR; }
    if (!S_ISREG(st.st_mode)) { close(fd); return E_NOT_ALLOWED; }
    if (offset > (uint64_t)st.st_size) { close(fd); return E_OFFSET; }
    g->fd = fd; g->size = (uint64_t)st.st_size; g->mtime = (int64_t)st.st_mtime; g->off = offset; g->want_sha = want_sha; g->step = 0;
    ota_sha256_init(&g->h); g->pre = 0; g->pre_end = want_sha ? offset : 0;   /* the prefix is hashed by get_step(), a few MiB per turn */
    g->unlink_after = unlink_after != 0;
    snprintf(g->unlink_path, sizeof g->unlink_path, "%s", unlink_after ? unlink_after : "");
    snprintf(g->shown, sizeof g->shown, "%s", shown);
    s->job = JOB_GET; s->job_stream = st_;
    oom_adjust();
    return 0;
}
static void do_get(struct sess *s, uint32_t st_, struct brd *r)
{
    int ns = (int)brd_u8(r), fl = (int)brd_u8(r); uint64_t off = brd_u64(r); size_t n; const uint8_t *p = brd_str16(r, &n);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    struct target t; int e = ns == NS_SD ? ns_resolve(ns, OP_GET, p, n, &t) : E_NOT_ALLOWED, dfd = -1; char *leaf;
    if (!e) e = walk_parent(&t, 0, &dfd, &leaf);
    if (!e && !*leaf) e = E_IS_DIR;
    int fd = -1;
    if (!e) {
        fd = openat(dfd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) { struct stat st; e = leaf_stat(dfd, leaf, &st); if (!e) e = errno_code(errno); }
    }
    if (dfd >= 0) close(dfd);
    if (!e) { char shown[160]; pct((const char *)p, n, shown, sizeof shown); e = get_open(s, st_, fd, off, fl & 1, 0, shown); }
    if (e) tx_err(s, st_, e);
}
/* one step of a GET; 1 = a hashing step was done (yield to the poll loop) */
static int get_step(struct sess *s)
{
    struct get_state *g = &s->get;
    if (g->step == 0) {
        uint8_t o[24]; be64(o, g->size); be64(o + 8, (uint64_t)g->mtime); be64(o + 16, g->off);
        tx_frame(s, T_GET_R, 0, s->job_stream, o, 24); g->step = 1;
        xfer(s, "get", g->off, g->size, g->shown, 1);
        return 0;
    }
    struct stat st;
    if (fstat(g->fd, &st) || (uint64_t)st.st_size != g->size) { uint32_t js = s->job_stream; job_end(s); tx_err(s, js, E_IO); return 0; }
    if (g->pre < g->pre_end) {   /* resume with a hash: the whole-file SHA-256 needs the prefix first */
        if (sha256_fd_step(g->fd, &g->pre, g->pre_end, &g->h)) { uint32_t js = s->job_stream; job_end(s); tx_err(s, js, E_IO); }
        return 1;
    }
    if (g->off >= g->size) {
        uint8_t d[32] = { 0 }; if (g->want_sha) ota_sha256_final(&g->h, d);
        xfer(s, "get", g->size, g->size, g->shown, 1);
        uint32_t js = s->job_stream; job_end(s); tx_end(s, T_GET_END, js, d, 32);
        return 0;
    }
    size_t l = g->size - g->off > BTP_MAX_DATA ? BTP_MAX_DATA : (size_t)(g->size - g->off);
    if (!s->enc && !g->want_sha) {   /* header now, the bytes with sendfile() */
        uint8_t *q = tx_payload(s); be64(q, g->off);
        btp_hdr_build(s->tx + s->tx_len, T_DATA, 0, s->job_stream, (uint32_t)(8 + l));
        s->tx_len += BTP_HDR + 8;
        s->sf_fd = g->fd; s->sf_off = (off_t)g->off; s->sf_left = l;
        g->off += l;
    } else {
        uint8_t *q = tx_payload(s); be64(q, g->off);
        size_t got = 0;
        while (got < l) {
            ssize_t k = pread(g->fd, q + 8 + got, l - got, (off_t)(g->off + got));
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { uint32_t js = s->job_stream; job_end(s); tx_err(s, js, E_IO); return 0; }
            got += (size_t)k;
        }
        if (g->want_sha) ota_sha256_update(&g->h, q + 8, l);
        tx_commit(s, T_DATA, 0, s->job_stream, 8 + l);
        g->off += l;
    }
    xfer(s, "get", g->off, g->size, g->shown, 0);
    return 0;
}
static int cfg_tf_db(void)   /* tf_music_db_enable of the effective config.json, as Settings reads it */
{
    char p[400], buf[8192]; snprintf(p, sizeof p, "%s/override/config.json", SP.hm);
    if (read_small(p, buf, sizeof buf) <= 0 && read_small(SP.resconf, buf, sizeof buf) <= 0) return 0;
    char *q = strstr(buf, "\"tf_music_db_enable\""); if (!q) return 0;
    q = strchr(q + 20, ':'); if (!q) return 0;
    while (*++q == ' ') {}
    return atoi(q) == 1;
}
static void live_db_path(char *out, int cap)
{
    if (cfg_tf_db()) snprintf(out, cap, "%s/.temp/usrlocal_media.db", SP.sd);
    else snprintf(out, cap, "%s/usrlocal_media.db", SP.data);
}
/* run a helper and wait; exit code, 127 = could not run, -1 = killed */
static int run_wait(char *const argv[])
{
    if (access(argv[0], X_OK)) return 127;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); }
        for (int fd = 3; fd < 256; fd++) close(fd);
        execv(argv[0], argv); _exit(127);
    }
    /* blocking wait; the caller credits the time back to the other sessions (stall_credit).
     * SIGTERM interrupts waitpid (no SA_RESTART): stop the helper so the daemon exits in time. */
    int stt;
    while (waitpid(pid, &stt, 0) < 0) {
        if (errno != EINTR) return -1;
        if (G.term) { kill(pid, SIGKILL); while (waitpid(pid, &stt, 0) < 0 && errno == EINTR) {} return -1; }
    }
    return WIFEXITED(stt) ? WEXITSTATUS(stt) : -1;
}
static void do_db_snapshot(struct sess *s, uint32_t st_, struct brd *r)
{
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    char live[400], tmp[400]; live_db_path(live, sizeof live);
    struct stat st; if (stat(live, &st)) { tx_err(s, st_, E_NOT_FOUND); return; }
    if (access(SP.hibytag, X_OK)) { tx_err(s, st_, E_UNSUPPORTED); return; }
    mkdirs(SP.dbdir, 0700);
    if (free_bytes(SP.data) >= 2 * (long long)st.st_size + (1 << 20)) snprintf(tmp, sizeof tmp, "%s/snap.db", SP.dbdir);
    else {
        if (!sd_ok()) { tx_err(s, st_, E_NO_SPACE); return; }
        snprintf(tmp, sizeof tmp, "%s/ByOS/sync", SP.sd); mkdirs(tmp, 0755);
        snprintf(tmp, sizeof tmp, "%s/ByOS/sync/snap.db", SP.sd);
    }
    unlink(tmp);
    char *av[] = { SP.hibytag, (char *)"dbsnapshot", live, tmp, 0 };
    int rc = run_wait(av);
    if (rc) { unlink(tmp); tx_err(s, st_, rc == 127 ? E_UNSUPPORTED : E_IO); return; }
    int fd = open(tmp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { tx_err(s, st_, E_IO); return; }
    int e = get_open(s, st_, fd, 0, 1, tmp, "usrlocal_media.db");
    if (e) { unlink(tmp); tx_err(s, st_, e); }
}

/* ================================================================== DB staging (13.2) */
static void db_status(struct sess *s, uint32_t st_)
{
    char p[400], live[400], js[1024]; uint8_t o[1 + 32 + 32 + 8 + 1 + 8] = { 0 };
    snprintf(p, sizeof p, "%s/staged.db", SP.dbdir);
    struct stat st; int staged = !stat(p, &st) && S_ISREG(st.st_mode);
    o[0] = (uint8_t)staged;
    if (staged) {
        snprintf(p, sizeof p, "%s/staged.json", SP.dbdir);
        char hx[65];
        if (read_small(p, js, sizeof js) > 0 && oj_str(oj_get(js, "sha256"), hx, sizeof hx) == 64) ota_unhex(hx, o + 1, 32);
    }
    live_db_path(live, sizeof live);
    uint64_t lsize = 0; if (sha256_path(live, o + 33, &lsize)) memset(o + 33, 0, 32);
    be64(o + 65, lsize);
    snprintf(p, sizeof p, "%s/apply.result", SP.dbdir);
    char ar[16]; o[73] = read_small(p, ar, sizeof ar) > 0 && ar[0] >= '0' && ar[0] <= '4' ? (uint8_t)(ar[0] - '0') : 0;
    long long fb = free_bytes(SP.data); be64(o + 74, fb > 0 ? (uint64_t)fb : 0);
    tx_end(s, T_DB_STATUS_R, st_, o, sizeof o);
}
static int sqlite_header_ok(const char *p, uint64_t *size)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) return 0;
    uint8_t h[100]; struct stat st; int ok = 0;
    if (!fstat(fd, &st) && pread(fd, h, 100, 0) == 100 && !memcmp(h, "SQLite format 3", 16)) {
        uint32_t ps = rd16(h + 16); if (ps == 1) ps = 65536;
        ok = ps >= 512 && ps <= 65536 && !(ps & (ps - 1)) && st.st_size > 0 && st.st_size % ps == 0;
        *size = (uint64_t)st.st_size;
    }
    close(fd); return ok;
}
static void do_db_stage(struct sess *s, uint32_t st_, struct brd *r)
{
    int fl = (int)brd_u8(r); const uint8_t *base = brd_take(r, 32);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    char inc[400], stg[400], live[400];
    snprintf(inc, sizeof inc, "%s/incoming.db", SP.dbdir); snprintf(stg, sizeof stg, "%s/staged.db", SP.dbdir);
    struct stat st;
    if (!s->db_put_ok || stat(inc, &st)) { tx_err(s, st_, E_NOT_STAGED); return; }
    uint64_t size = 0;
    if (!sqlite_header_ok(inc, &size)) { tx_err(s, st_, E_DB_INVALID); return; }
    char *av[] = { SP.hibytag, (char *)"dbcheck", inc, 0 };
    int rc = run_wait(av);
    if (rc == 127) { tx_err(s, st_, E_UNSUPPORTED); return; }
    if (rc) { tx_err(s, st_, E_DB_INVALID); return; }
    live_db_path(live, sizeof live);
    uint8_t lsha[32]; uint64_t lsize = 0; int have_live = !sha256_path(live, lsha, &lsize);
    if (!(fl & 1) && (!have_live || !btp_ct_eq(lsha, base, 32))) { tx_err(s, st_, E_DB_STALE); return; }
    uint64_t mx = lsize > size ? lsize : size;
    if (free_bytes(SP.data) < (long long)(2 * mx + (1 << 20))) { tx_err(s, st_, E_NO_SPACE); return; }
    uint8_t ssha[32]; if (sha256_path(inc, ssha, 0)) { tx_err(s, st_, E_IO); return; }
    if (rename(inc, stg)) { tx_err(s, st_, E_IO); return; }
    char js[400], h1[65], h2[65], jp[400]; hexs(ssha, 32, h1); hexs(base, 32, h2);
    int n = snprintf(js, sizeof js, "{\"sha256\": \"%s\", \"base_sha256\": \"%s\", \"size\": %llu, \"time\": %lld}\n",
                     h1, h2, (unsigned long long)size, (long long)time(0));
    snprintf(jp, sizeof jp, "%s/staged.json", SP.dbdir);
    if (write_atomic(jp, js, (size_t)n, 0600)) { tx_err(s, st_, E_IO); return; }
    s->db_put_ok = 0;
    ctl_send("DBSTAGED %llu", (unsigned long long)size);
    db_status(s, st_);
}
static void do_db_unstage(struct sess *s, uint32_t st_, struct brd *r)
{
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    const char *names[3] = { "staged.db", "staged.json", "incoming.db" }; char p[400];
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", SP.dbdir, names[i]); unlink(p); }
    s->db_put_ok = 0;
    db_status(s, st_);
}

/* ================================================================== LIB_RESCAN (13.3) */
static void do_rescan(struct sess *s, uint32_t st_, struct brd *r)
{
    int mode = (int)brd_u8(r); uint32_t count = brd_u16(r);
    if (r->bad) { tx_err(s, st_, E_PROTO); return; }
    if (mode == 0) {
        if (count || !brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
        if (G.ctl < 0) { tx_err(s, st_, E_UNSUPPORTED); return; }
        if (G.rescan_sid) { tx_err(s, st_, E_BUSY); return; }
        ctl_send("RESCAN"); G.rescan_sid = s->sid; s->job = JOB_RESCAN; s->job_stream = st_;
        return;
    }
    if (mode != 1 || count < 1 || count > 256) { tx_err(s, st_, mode == 1 ? E_PROTO : E_UNSUPPORTED); return; }
    /* validate everything first */
    size_t save = r->i;
    for (uint32_t i = 0; i < count; i++) { size_t n; const uint8_t *p = brd_str16(r, &n); if (!p || btp_path_check(p, n, 0)) { tx_err(s, st_, r->bad ? E_PROTO : E_BAD_PATH); return; } }
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    if (!sd_ok()) { tx_err(s, st_, E_SD_UNAVAILABLE); return; }
    if (access(SP.hibytag, X_OK)) { tx_err(s, st_, E_UNSUPPORTED); return; }
    r->i = save;
    static char pathbuf[16][BTP_PATH_MAX + 300]; char *av[20]; int e = 0;
    for (uint32_t i = 0; i < count && !e; ) {
        int k = 0; av[k++] = SP.hibytag; av[k++] = (char *)"dbupdate";
        for (int j = 0; j < 16 && i < count; j++, i++) {
            size_t n; const uint8_t *p = brd_str16(r, &n);
            snprintf(pathbuf[j], sizeof pathbuf[j], "%s/%.*s", SP.sd, (int)n, (const char *)p);
            av[k++] = pathbuf[j];
        }
        av[k] = 0;
        int rc = run_wait(av); if (rc) e = rc == 127 ? E_UNSUPPORTED : E_IO;
    }
    if (e) tx_err(s, st_, e); else tx_ok(s, st_);
}

/* ================================================================== updates (14) */
static int update_check(const char *json_path, int allow_down, struct ota_info *in, int *missing)
{
    *missing = 0;
    size_t n; char *js = ota_read_file(json_path, OTA_JSON_MAX, &n);
    if (!js) { *missing = 1; return E_NOT_STAGED; }
    int e = 0;
    if (ota_sig_check(js, n) != OTA_SIG_OK) e = E_SIGNATURE;
    else if (ota_parse(js, in)) e = E_UPDATE_INVALID;
    free(js);
    if (e) return e;
    versions_refresh();
    if (in->needs_firmware || (in->min_firmware[0] && G.firmware[0] && ota_vercmp(G.firmware, in->min_firmware) < 0)) return E_NEEDS_FIRMWARE;
    if (in->min_version[0] && G.byos_version[0] && ota_vercmp(G.byos_version, in->min_version) < 0) return E_UPDATE_INVALID;
    if (G.byos_version[0] && ota_vercmp(in->version, G.byos_version) <= 0 && !allow_down) return E_DOWNGRADE;
    char tar[400]; snprintf(tar, sizeof tar, "%s/%s", SP.ota, in->name);
    struct stat st; if (stat(tar, &st) || !S_ISREG(st.st_mode)) { *missing = 1; return E_NOT_STAGED; }
    if ((long long)st.st_size != in->size) return E_UPDATE_INVALID;
    char hx[65]; long long sz;
    if (ota_file_sha256(tar, hx, &sz, 0, (volatile int *)&G.term)) return E_IO;   /* SIGTERM stops it */
    if (strcmp(hx, in->sha256)) return E_UPDATE_INVALID;
    return 0;
}
static void do_update_stage(struct sess *s, uint32_t st_, struct brd *r)
{
    int fl = (int)brd_u8(r);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    if (!sd_ok()) { tx_err(s, st_, E_SD_UNAVAILABLE); return; }
    char sj[400], hj[400]; snprintf(sj, sizeof sj, "%s/sync-update.json", SP.ota); snprintf(hj, sizeof hj, "%s/hibyos-update.json", SP.ota);
    struct ota_info in; int missing; int e = update_check(sj, fl & 1, &in, &missing);
    if (!e && rename(sj, hj)) e = E_IO;
    if (e) { tx_err(s, st_, e); return; }
    int dfd = open(SP.ota, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dfd >= 0) { fsync(dfd); close(dfd); }
    G.up_state = 1; snprintf(G.up_version, sizeof G.up_version, "%s", in.version); G.up_permille = 0; G.up_error = 0;
    uint8_t o[64]; struct bwr w; bwr_init(&w, o, sizeof o);
    bwr_str16(&w, in.version, strlen(in.version)); bwr_u8(&w, (uint32_t)in.restart); bwr_u8(&w, (uint32_t)in.needs_firmware); bwr_u64(&w, (uint64_t)in.size);
    tx_end(s, T_UPDATE_STAGE_R, st_, o, w.n);
}
static void do_update_install(struct sess *s, uint32_t st_, struct brd *r)
{
    int fl = (int)brd_u8(r);
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    if (!sd_ok()) { tx_err(s, st_, E_SD_UNAVAILABLE); return; }
    char hj[400]; snprintf(hj, sizeof hj, "%s/hibyos-update.json", SP.ota);
    struct ota_info in; int missing; int e = update_check(hj, fl & 2, &in, &missing);
    if (e) { tx_err(s, st_, e); return; }
    if (G.ctl < 0) { tx_err(s, st_, E_UNSUPPORTED); return; }
    int noconfirm = (fl & 1) && s->local;
    ctl_send("UPDATE %s %s", in.version, noconfirm ? "noconfirm" : "confirm");
    G.up_state = noconfirm ? 3 : 2; G.up_permille = 0; G.up_error = 0;
    snprintf(G.up_version, sizeof G.up_version, "%s", in.version);
    tx_ok(s, st_);
}
static void do_update_status(struct sess *s, uint32_t st_, struct brd *r)
{
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    uint8_t o[64]; struct bwr w; bwr_init(&w, o, sizeof o);
    bwr_u8(&w, (uint32_t)G.up_state); bwr_u16(&w, (uint32_t)G.up_permille); bwr_u16(&w, (uint32_t)G.up_error);
    bwr_str16(&w, G.up_version, strlen(G.up_version));
    tx_end(s, T_UPDATE_STATUS_R, st_, o, w.n);
}
static void do_status(struct sess *s, uint32_t st_, struct brd *r)
{
    if (!brd_done(r)) { tx_err(s, st_, E_PROTO); return; }
    versions_refresh();
    long b = read_long(SP.batt, -1);
    uint8_t o[128]; struct bwr w; bwr_init(&w, o, sizeof o);
    bwr_u8(&w, b >= 0 && b <= 100 ? (uint32_t)b : 255); bwr_u8(&w, read_long(SP.usb, 0) == 1); bwr_u8(&w, (uint32_t)sd_state(0));
    if (G.byos_version[0]) bwr_tlv(&w, TLV_BYOS_VERSION, G.byos_version, strlen(G.byos_version));
    if (G.firmware[0]) bwr_tlv(&w, TLV_FIRMWARE, G.firmware, strlen(G.firmware));
    tx_end(s, T_STATUS_R, st_, o, w.n);
}

/* ================================================================== session control */
static int hello_flags(struct sess *s)
{
    return (s->local ? HF_LOCAL : 0) | (G.pairing_open ? HF_PAIRING_OPEN : 0) | (sd_ok() ? HF_SD_MOUNTED : 0) | (G.ctl >= 0 ? HF_SHELL_ATTACHED : 0);
}
static void sess_open_done(struct sess *s, const char *name)
{
    s->st = SS_OPEN; s->ps = PS_NONE;
    char nm[200]; pct(name, strlen(name), nm, sizeof nm);
    ctl_send("CONN %d %s %s", s->sid, s->local ? "usb" : "wifi", nm[0] ? nm : "-");
}
static void job_end(struct sess *s)
{
    if (s->job == JOB_GET) {
        if (s->get.fd >= 0) close(s->get.fd);
        s->get.fd = -1;
        if (s->get.unlink_after) unlink(s->get.unlink_path);
        s->get.unlink_after = 0;
        s->sf_left = 0; s->sf_fd = -1;
    } else if (s->job == JOB_LIST) {
        if (s->list.d) closedir(s->list.d);
        s->list.d = 0;
    } else if (s->job == JOB_RESCAN) {
        if (G.rescan_sid == s->sid) G.rescan_sid = 0;
    } else if (s->job == JOB_PUTHASH) {   /* cancelled or failed before PUT_READY: the part is kept */
        if (s->hj.kind == HJ_SKIP && s->hj.fd >= 0) close(s->hj.fd);
        if (s->hj.kind == HJ_RESUME && s->put.fd >= 0) close(s->put.fd);
        if (s->put.dfd >= 0) close(s->put.dfd);
        s->hj.fd = s->put.fd = s->put.dfd = -1;
    }
    s->job = JOB_NONE;
    s->last_rx = mono();   /* a long job is not idleness: the idle clock starts again now */
    oom_adjust();
}
/* count = 1: a real attempt (SAS shown, then mismatch, deny, timeout); 3 of them close the window.
 * A commit that is never revealed shows nothing to the user and only frees the pairing slot. */
static void pair_fail_n(struct sess *s, const char *why, int count)
{
    ctl_send("PAIRFAIL %d %s", s->sid, why);
    if (count) {
        time_t t = mono();
        if (t - G.pair_fail_start > 600) { G.pair_fail_start = t; G.pair_fails = 0; }
        if (++G.pair_fails >= 3) { G.pairing_open = 0; lg("pairing window closed after 3 failures"); }
    }
    if (G.pairing_sid == s->sid) G.pairing_sid = 0;
    s->ps = PS_NONE;
}
static void pair_fail(struct sess *s, const char *why) { pair_fail_n(s, why, 1); }
static void sess_close(struct sess *s)
{
    if (!s->st) return;
    if (s->ps == PS_WAIT_USER) pair_fail(s, "closed");
    else if (s->ps == PS_COMMITTED) pair_fail_n(s, "closed", 0);
    if (G.pairing_sid == s->sid) G.pairing_sid = 0;
    if (s->job) job_end(s);
    put_close(s, 0);
    if (s->fd >= 0) close(s->fd);
    if (s->st == SS_OPEN || s->st == SS_SECURING) ctl_send("GONE %d", s->sid);
    btp_wipe(s->k_rx, 32); btp_wipe(s->k_tx, 32); btp_wipe(s->de, 32);
    s->st = SS_FREE; s->fd = -1; s->hq_n = 0; s->hq_bytes = 0;
    /* give the session's buffer pages back (they are zero-filled again on next use) */
    int idx = (int)(s - S);
    madvise(RXB[idx], RXSTRIDE, MADV_DONTNEED); madvise(TXB[idx], TXSTRIDE, MADV_DONTNEED);
    madvise(HOLDB[idx], sizeof HOLDB[idx], MADV_DONTNEED);
    int any = 0; for (int i = 0; i < MAX_SESS; i++) if (S[i].st) any = 1;
    if (!any) { G.idle_since = mono(); madvise(FBUF, sizeof FBUF, MADV_DONTNEED); }
    oom_adjust();
}
/* Time spent blocked inside one request (helper processes, hashing a payload tar, deleting a
 * tree...) is not the other side's idleness: push every session clock forward by it. */
static void stall_credit(time_t d)
{
    if (d <= 0) return;
    for (int i = 0; i < MAX_SESS; i++) {
        struct sess *o = &S[i]; if (!o->st) continue;
        o->last_rx += d; o->hello_deadline += d; o->secure_deadline += d;
        if (o->ps == PS_WAIT_USER) o->pair_deadline += d;
        if (o->put.active) o->put.last += d;
    }
}
static struct ratel *rl_get(uint32_t addr)
{
    time_t t = mono(); struct ratel *o = &G.rl[0];
    for (int i = 0; i < 8; i++) { if (G.rl[i].addr == addr) return &G.rl[i]; if (G.rl[i].start < o->start) o = &G.rl[i]; }
    memset(o, 0, sizeof *o); o->addr = addr; o->start = t; return o;
}
static int rl_blocked(uint32_t addr) { struct ratel *r = rl_get(addr); return r->blocked_until > mono(); }
static void rl_fail(uint32_t addr)
{
    struct ratel *r = rl_get(addr); time_t t = mono();
    if (t - r->start > 60) { r->start = t; r->fails = 0; }
    if (++r->fails >= 5) { r->blocked_until = t + 60; r->fails = 0; r->start = t; }
}

/* ---------- pairing (8.2) */
static void pair_finish(struct sess *s)
{
    s->peer_idx = peer_add(s->Cs, s->pname);
    btp_key_id(s->Cs, s->key_id);
    uint8_t o[1 + 32 + 8 + 2 + BTP_NAME_MAX]; struct bwr w; bwr_init(&w, o, sizeof o);
    bwr_u8(&w, 0); bwr_bytes(&w, G.Ds, 32); bwr_bytes(&w, G.device_id, 8); bwr_str16(&w, G.name, strlen(G.name));
    tx_end(s, T_PAIR_RESULT, 0, o, w.n);
    uint8_t id[16]; char hx[33], nm[200]; btp_key_id(s->Cs, id); hexs(id, 16, hx); pct(s->pname, strlen(s->pname), nm, sizeof nm);
    ctl_send("PAIRED %d %s %s", s->sid, hx, nm[0] ? nm : "-");
    G.pairing_sid = 0;
    sess_open_done(s, s->pname);
}
static void on_pair_commit(struct sess *s, const uint8_t *p, size_t n)
{
    if (n != 32 || s->enc || s->ps != PS_NONE) { fatal(s, 0, E_PROTO); return; }
    if (!G.pairing_open || G.ctl < 0) { fatal(s, 0, E_PAIR_CLOSED); return; }
    if (G.pairing_sid && G.pairing_sid != s->sid) { fatal(s, 0, E_BUSY); return; }
    memcpy(s->commit, p, 32);
    if (btp_random(s->de, 32) || btp_random(s->Nd, 16)) { fatal(s, 0, E_INTERNAL); return; }
    btp_x25519_base(s->De, s->de);
    uint8_t o[48]; memcpy(o, s->De, 32); memcpy(o + 32, s->Nd, 16);
    tx_frame(s, T_PAIR_DEVICE, 0, 0, o, 48);
    s->ps = PS_COMMITTED; G.pairing_sid = s->sid;
}
static void on_pair_reveal(struct sess *s, const uint8_t *p, size_t n)
{
    if (n != 48 || s->ps != PS_COMMITTED) { fatal(s, 0, E_PROTO); return; }
    const uint8_t *Ce = p, *Nc = p + 32; uint8_t c[32];
    btp_pair_commit(Ce, Nc, c);
    if (!btp_ct_eq(c, s->commit, 32)) { pair_fail(s, "commit"); fatal(s, 0, E_AUTH_FAILED); return; }
    uint8_t ss[32], th[32], prk[32];
    if (btp_x25519_dh(ss, s->de, Ce)) { pair_fail(s, "commit"); fatal(s, 0, E_AUTH_FAILED); return; }
    btp_pair_th(s->hc, s->hd, s->commit, s->De, s->Nd, Ce, Nc, th);
    btp_hkdf_extract(th, 32, ss, 32, prk);
    char sas[7]; btp_sas(prk, sas);
    btp_keys(prk, s->k_rx, s->k_tx);
    btp_wipe(ss, 32); btp_wipe(prk, 32); btp_wipe(s->de, 32);
    s->enc = 1; s->ctr_rx = s->ctr_tx = 0;
    s->ps = PS_WAIT_USER; s->confirmed = s->allowed = 0; s->pair_deadline = mono() + T_PAIR_S;
    char nm[200]; pct(s->cname, strlen(s->cname), nm, sizeof nm);
    ctl_send("SAS %d %s %s", s->sid, sas, nm[0] ? nm : "-");
    btp_wipe(sas, sizeof sas);
}
static void on_pair_confirm(struct sess *s, const uint8_t *p, size_t n)
{
    if (s->ps != PS_WAIT_USER || !s->enc || s->confirmed) { fatal(s, 0, E_PROTO); return; }
    struct brd r; brd_init(&r, p, n);
    const uint8_t *Cs = brd_take(&r, 32); size_t nl; const uint8_t *nm = brd_str16(&r, &nl);
    if (!brd_done(&r) || !btp_name_ok(nm, nl)) { fatal(s, 0, E_PROTO); return; }
    memcpy(s->Cs, Cs, 32); memcpy(s->pname, nm, nl); s->pname[nl] = 0;
    s->confirmed = 1;
    if (s->allowed) pair_finish(s);
}
/* ---------- AUTH (8.3) */
static void on_auth_init(struct sess *s, const uint8_t *p, size_t n)
{
    if (n != 48 || s->enc || s->ps != PS_NONE) { fatal(s, 0, E_PROTO); return; }
    if (rl_blocked(s->peer_addr)) { fatal(s, 0, E_RATE_LIMIT); return; }
    const uint8_t *key_id = p, *Ce = p + 16;
    int i = peer_find(key_id);
    if (i < 0) { rl_fail(s->peer_addr); fatal(s, 0, E_AUTH_FAILED); return; }
    uint8_t de[32], De[32], ikm[96], th[32], prk[32];
    if (btp_random(de, 32)) { fatal(s, 0, E_INTERNAL); return; }
    btp_x25519_base(De, de);
    if (btp_x25519_dh(ikm, de, Ce) || btp_x25519_dh(ikm + 32, de, G.peers[i].Cs) || btp_x25519_dh(ikm + 64, G.ds, Ce)) {
        btp_wipe(de, 32); btp_wipe(ikm, 96); rl_fail(s->peer_addr); fatal(s, 0, E_AUTH_FAILED); return;
    }
    btp_auth_th(s->hc, s->hd, key_id, Ce, De, G.peers[i].Cs, G.Ds, th);
    btp_hkdf_extract(th, 32, ikm, 96, prk);
    btp_keys(prk, s->k_rx, s->k_tx);
    btp_wipe(de, 32); btp_wipe(ikm, 96); btp_wipe(prk, 32);
    tx_frame(s, T_AUTH_REPLY, 0, 0, De, 32);
    s->enc = 1; s->ctr_rx = s->ctr_tx = 0; s->ps = PS_AUTH_WAIT; s->peer_idx = i;
    memcpy(s->key_id, key_id, 16); snprintf(s->pname, sizeof s->pname, "%s", G.peers[i].name);
}
static void on_auth_finish(struct sess *s, size_t n)
{
    if (s->ps != PS_AUTH_WAIT || n) { fatal(s, 0, E_PROTO); return; }
    tx_end(s, T_AUTH_OK, 0, 0, 0);
    if (s->st == SS_OPEN) { s->ps = PS_NONE; return; }   /* local session that chose encryption */
    sess_open_done(s, s->pname);
}

/* ---------- HELLO */
static void on_hello(struct sess *s, const uint8_t *p, size_t n)
{
    int pmin, pmax, role; uint32_t mp, caps;
    if (btp_hello_parse(p, n, &pmin, &pmax, &role, &mp, &caps, s->cname) || role != 0 || mp < BTP_MAX_PAYLOAD) { fatal(s, 0, E_PROTO); return; }
    int v = pmax < BTP_VERSION ? pmax : BTP_VERSION, lo = pmin > 1 ? pmin : 1;
    if (v < lo) { fatal(s, 0, E_VERSION); return; }
    btp_sha256(p, n, s->hc);
    versions_refresh();
    struct btp_hello_info h = { hello_flags(s), CAP_ALL, SYNC_SOFTWARE, G.name, G.byos_version, G.firmware, SYNC_MODEL, G.device_id };
    uint8_t *q = tx_payload(s); size_t l = btp_hello_device(q, tx_room(s), &h);
    btp_sha256(q, l, s->hd);
    tx_commit(s, T_HELLO, 0, 0, l);
    s->caps = caps & CAP_ALL;
    if (s->local) sess_open_done(s, s->cname); else s->st = SS_SECURING;
}

/* ---------- pipelined requests (2.3): kept aside while a job runs, so the socket is still read
 * and a later PING or CANCEL is handled at once. FIFO, payloads packed in HOLDB[session]. */
static void hold_drop(struct sess *s, int i)
{
    uint8_t *hb = HOLDB[s - S];
    uint32_t off = s->hq[i].off, len = s->hq[i].len, end = s->hq_bytes;
    memmove(hb + off, hb + off + len, end - off - len);
    for (int k = i; k + 1 < s->hq_n; k++) { s->hq[k] = s->hq[k + 1]; s->hq[k].off -= len; }
    s->hq_n--; s->hq_bytes -= len;
}
/* copy the frame in the RX buffer to the queue; 0 = no room (it stays in the RX buffer) */
static int hold_push(struct sess *s)
{
    if (s->hq_n >= HOLD_MAX || s->hq_bytes + s->f_len > HOLDCAP) return 0;
    int i = s->hq_n++;
    s->hq[i].type = s->f_type; s->hq[i].flags = s->f_flags; s->hq[i].stream = s->f_stream; s->hq[i].len = s->f_len; s->hq[i].off = s->hq_bytes;
    if (s->f_len) memcpy(HOLDB[s - S] + s->hq_bytes, s->f_pay, s->f_len);
    s->hq_bytes += s->f_len;
    return 1;
}

/* ---------- dispatch */
static int cap_of(int t)
{
    switch (t) {
    case T_STAT: case T_LIST: case T_DF: case T_MKDIR: case T_MOVE: case T_DELETE: case T_PUT_BEGIN: case T_GET: return CAP_FILES;
    case T_DB_SNAPSHOT: return CAP_DB_SNAPSHOT;
    case T_DB_STAGE: case T_DB_STATUS: case T_DB_UNSTAGE: return CAP_DB_STAGE;
    case T_LIB_RESCAN: return CAP_LIB_RESCAN;
    case T_UPDATE_STAGE: case T_UPDATE_INSTALL: case T_UPDATE_STATUS: return CAP_UPDATE;
    case T_STATUS: return CAP_STATUS;
    }
    return 0;
}
static void request(struct sess *s)
{
    int t = s->f_type; uint32_t st_ = s->f_stream; const uint8_t *p = s->f_pay; size_t n = s->f_len;
    struct brd r; brd_init(&r, p, n);
    if (s->st == SS_HELLO) { if (t != T_HELLO || st_) { s->close_after_tx = 1; return; } on_hello(s, p, n); return; }
    if (!btp_type_known(t)) { if (!st_ && t == T_ERROR) return; tx_err(s, st_, E_UNKNOWN_TYPE); return; }
    switch (t) {   /* stream 0 messages */
    case T_PING: if (st_ || n != 8) { fatal(s, st_, E_PROTO); return; } tx_frame(s, T_PONG, 0, 0, p, 8); return;
    case T_PONG: return;
    case T_BYE: if (s->ps == PS_WAIT_USER) pair_fail(s, "mismatch"); s->close_after_tx = 1; return;
    case T_CANCEL:
        if (n) { tx_err(s, st_, E_PROTO); return; }
        for (int i = 0; i < s->hq_n; i++) if (s->hq[i].stream == st_ && st_) {   /* a queued request: drop it */
            hold_drop(s, i); tx_err(s, st_, E_CANCELLED); return;
        }
        if (s->job && s->job_stream == st_) { job_end(s); tx_err(s, st_, E_CANCELLED); }
        else if (s->put.active && s->put.stream == st_) { put_close(s, 0); tx_err(s, st_, E_CANCELLED); }
        return;
    case T_PAIR_COMMIT: case T_PAIR_REVEAL: case T_PAIR_CONFIRM: case T_AUTH_INIT: case T_AUTH_FINISH:
        if (st_) { tx_err(s, st_, E_PROTO); return; }
        if (s->st == SS_OPEN && !(s->local && (t == T_AUTH_INIT || t == T_AUTH_FINISH))) { tx_err(s, 0, E_PROTO); return; }
        if (t == T_PAIR_COMMIT) on_pair_commit(s, p, n);
        else if (t == T_PAIR_REVEAL) on_pair_reveal(s, p, n);
        else if (t == T_PAIR_CONFIRM) on_pair_confirm(s, p, n);
        else if (t == T_AUTH_INIT) on_auth_init(s, p, n);
        else on_auth_finish(s, n);
        return;
    }
    if (s->st != SS_OPEN) { fatal(s, st_, E_AUTH_REQUIRED); return; }
    if (!st_) { fatal(s, 0, E_PROTO); return; }
    if (t == T_DATA) { do_data(s, st_, p, n); return; }
    if (t == T_PUT_END) { do_put_end(s, st_, n); return; }
    int cap = cap_of(t);
    if (!cap) { tx_err(s, st_, E_PROTO); return; }   /* a device-only message sent by the client */
    if (!need(s, (uint32_t)cap)) { tx_err(s, st_, E_UNSUPPORTED); return; }
    if (s->put.active) { tx_err(s, st_, E_BUSY); return; }
    if ((G.up_state == 2 || G.up_state == 3) && t != T_STATUS && t != T_UPDATE_STATUS && t != T_DF) { tx_err(s, st_, E_BUSY); return; }
    switch (t) {
    case T_STAT: do_stat(s, st_, &r); break;
    case T_LIST: do_list(s, st_, &r); break;
    case T_DF: do_df(s, st_, &r); break;
    case T_MKDIR: do_mkdir(s, st_, &r); break;
    case T_MOVE: do_move(s, st_, &r); break;
    case T_DELETE: do_delete(s, st_, &r); break;
    case T_PUT_BEGIN: do_put_begin(s, st_, &r); break;
    case T_GET: do_get(s, st_, &r); break;
    case T_DB_SNAPSHOT: do_db_snapshot(s, st_, &r); break;
    case T_DB_STAGE: do_db_stage(s, st_, &r); break;
    case T_DB_STATUS: if (!brd_done(&r)) tx_err(s, st_, E_PROTO); else db_status(s, st_); break;
    case T_DB_UNSTAGE: do_db_unstage(s, st_, &r); break;
    case T_LIB_RESCAN: do_rescan(s, st_, &r); break;
    case T_UPDATE_STAGE: do_update_stage(s, st_, &r); break;
    case T_UPDATE_INSTALL: do_update_install(s, st_, &r); break;
    case T_UPDATE_STATUS: do_update_status(s, st_, &r); break;
    case T_STATUS: do_status(s, st_, &r); break;
    }
}

/* ================================================================== socket I/O */
static void rx_reset(struct sess *s) { s->rx_have = 0; s->rx_phase = 0; s->rx_need = s->enc ? 4 : BTP_HDR; }
/* read what is available; 1 = a frame is ready, 0 = need more, -1 = close (silently) */
static int rx_pump(struct sess *s)
{
    for (;;) {
        if (s->rx_phase == 2) return 1;
        uint8_t *base = s->enc ? s->rx : s->rx + BTP_REC_HDR;
        while (s->rx_have < s->rx_need) {
            ssize_t k = read(s->fd, base + s->rx_have, s->rx_need - s->rx_have);
            if (k > 0) { s->rx_have += (size_t)k; continue; }
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
            return -1;
        }
        if (s->rx_phase == 0) {
            if (s->enc) {
                uint32_t bl = rd32(s->rx);
                if (bl < BTP_TAG + BTP_HDR || bl > BTP_MAX_RECORD) return -1;
                s->rx_need = 4 + bl; s->rx_phase = 1; continue;
            }
            int type, flags; uint32_t stream, len;
            int e = btp_hdr_parse(base, BTP_MAX_PAYLOAD, &type, &flags, &stream, &len);
            if (e) { fatal(s, e == E_TOO_LARGE ? stream : 0, e); return -2; }
            s->f_type = type; s->f_flags = flags; s->f_stream = stream; s->f_len = len;
            s->rx_need = BTP_HDR + len; s->rx_phase = 1;
            if (len) continue;
        } else if (s->enc) {
            uint8_t nonce[24]; size_t flen = s->rx_have - BTP_REC_HDR;
            btp_nonce(nonce, DIR_C2D, s->ctr_rx);
            if (btp_box_open(s->rx + 4, s->rx + BTP_REC_HDR, flen, nonce, s->k_rx)) {
                if (s->ps == PS_AUTH_WAIT) rl_fail(s->peer_addr);
                return -1;
            }
            if (++s->ctr_rx >= (1ULL << 32)) return -1;
            int type, flags; uint32_t stream, len;
            if (btp_hdr_parse(s->rx + BTP_REC_HDR, BTP_MAX_PAYLOAD, &type, &flags, &stream, &len) || BTP_HDR + len != flen) return -1;
            s->f_type = type; s->f_flags = flags; s->f_stream = stream; s->f_len = len;
        }
        s->f_pay = s->rx + BTP_REC_HDR + BTP_HDR;
        s->rx_phase = 2;
        return 1;
    }
}
/* 1 = everything sent, 0 = would block, -1 = error */
static int tx_flush(struct sess *s)
{
    while (s->tx_off < s->tx_len) {
        ssize_t k = send(s->fd, s->tx + s->tx_off, s->tx_len - s->tx_off, MSG_NOSIGNAL);
        if (k > 0) { s->tx_off += (size_t)k; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return -1;
    }
    while (s->sf_left) {
        ssize_t k = sendfile(s->fd, s->sf_fd, &s->sf_off, s->sf_left);
        if (k > 0) { s->sf_left -= (size_t)k; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return -1;
    }
    s->tx_off = s->tx_len = 0;
    if (s->enc && s->ctr_tx >= (1ULL << 32)) return -1;
    return 1;
}
/* run the frame described by s->f_*; a long synchronous request is credited back to every
 * session clock, so the timers do not kill sessions that were only waiting for us */
static void run_frame(struct sess *s)
{
    time_t t0 = mono();
    request(s);
    stall_credit(mono() - t0);
}
/* run the oldest queued request (the RX buffer may hold a newer frame: its f_* are kept) */
static void run_held(struct sess *s)
{
    int ft = s->f_type, ff = s->f_flags; uint32_t fs = s->f_stream, fl = s->f_len; uint8_t *fp = s->f_pay;
    s->f_type = s->hq[0].type; s->f_flags = s->hq[0].flags; s->f_stream = s->hq[0].stream; s->f_len = s->hq[0].len;
    s->f_pay = HOLDB[s - S] + s->hq[0].off;
    run_frame(s);
    hold_drop(s, 0);
    s->f_type = ft; s->f_flags = ff; s->f_stream = fs; s->f_len = fl; s->f_pay = fp;
}
static int frame_immediate(const struct sess *s)   /* handled even while a job runs (2.3) */
{
    return s->f_type == T_PING || s->f_type == T_PONG || s->f_type == T_CANCEL || s->f_type == T_BYE;
}
/* one turn of a session's state machine */
static void sess_service(struct sess *s, short rev)
{
    if (rev & (POLLERR | POLLNVAL)) { sess_close(s); return; }
    for (int guard = 0; guard < 64 && s->st; guard++) {
        int fl = tx_flush(s);
        if (fl < 0) { sess_close(s); return; }
        if (!fl) return;                                    /* wait for POLLOUT */
        if (s->close_after_tx) { sess_close(s); return; }
        if (!s->job && s->hq_n) { run_held(s); continue; }  /* serial execution: queued requests first */
        if (s->rx_phase != 2) {                             /* read what is there (CANCEL/PING during a job too) */
            int r = rx_pump(s);
            if (r == -2) continue;                          /* fatal error queued */
            if (r < 0) { sess_close(s); return; }
        }
        if (s->rx_phase == 2) {                             /* a frame is waiting */
            int imm = frame_immediate(s);
            /* only an open session's traffic counts as activity: an unauthenticated one cannot
             * keep itself alive with PINGs (it has its own deadline, see timers()) */
            if (s->st == SS_OPEN || !imm) s->last_rx = mono();
            if (imm || (!s->job && !s->hq_n)) { run_frame(s); rx_reset(s); continue; }
            if (hold_push(s)) { rx_reset(s); continue; }    /* queued: keep reading */
            /* queue full: it stays in the RX buffer and reading pauses until the job ends */
        }
        if (s->job == JOB_GET) { if (get_step(s)) return; continue; }
        if (s->job == JOB_LIST) { list_step(s); continue; }
        if (s->job == JOB_PUTHASH) { puthash_step(s); return; }   /* one hashing step per turn */
        return;                                             /* wait for input or the shell */
    }
}
static short sess_events(struct sess *s)
{
    if (s->tx_off < s->tx_len || s->sf_left) return POLLOUT;
    short in = s->rx_phase == 2 ? 0 : POLLIN;
    if (s->job == JOB_GET || s->job == JOB_LIST || s->job == JOB_PUTHASH) return POLLOUT | in;
    if (!s->job && (s->hq_n || s->rx_phase == 2)) return POLLOUT | in;   /* work is waiting */
    return in;
}

/* ================================================================== listeners */
static int listen_on(uint32_t addr, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = addr;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4)) { close(fd); return -1; }
    return fd;
}
static int wifi_addr(uint32_t *addr, uint32_t *mask)
{
    if (SP.wifi_fixed) { *addr = SP.wifi_fixed; *mask = htonl(0xff000000); return 0; }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return -1;
    struct ifreq r; memset(&r, 0, sizeof r); snprintf(r.ifr_name, sizeof r.ifr_name, "%s", SP.wifi_if);
    int ok = !ioctl(fd, SIOCGIFADDR, &r);
    if (ok) *addr = ((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr;
    if (ok && !ioctl(fd, SIOCGIFNETMASK, &r)) *mask = ((struct sockaddr_in *)&r.ifr_netmask)->sin_addr.s_addr; else ok = 0;
    close(fd);
    return ok && *addr ? 0 : -1;
}
static void announce(const uint8_t *nonce, const struct sockaddr_in *to)
{
    if (G.udp < 0) return;
    uint8_t pk[64];
    int fl = (G.pairing_open ? 1 : 0) | (sd_ok() ? 2 : 0) | (G.npeers ? 4 : 0);
    size_t n = btp_announce_build(pk, sizeof pk, fl, nonce, SP.port, G.device_id, G.name);
    struct sockaddr_in b;
    if (!to) { memset(&b, 0, sizeof b); b.sin_family = AF_INET; b.sin_port = htons(SP.port == BTP_PORT ? BTP_ANNOUNCE_PORT : SP.port + 1); b.sin_addr.s_addr = htonl(INADDR_BROADCAST); to = &b; }
    sendto(G.udp, pk, n, MSG_NOSIGNAL, (const struct sockaddr *)to, sizeof *to);
}
static void wifi_refresh(void)
{
    if (!G.wifi) return;
    time_t t = mono(); if (G.wifi_checked && t - G.wifi_checked < 5) return;
    G.wifi_checked = t;
    uint32_t a = 0, m = 0; int up = !wifi_addr(&a, &m);
    if (up && G.lst_wifi >= 0 && a == G.wifi_addr) return;
    if (G.lst_wifi >= 0) { close(G.lst_wifi); G.lst_wifi = -1; }
    if (G.udp >= 0) { close(G.udp); G.udp = -1; }
    if (!up) return;
    G.wifi_addr = a; G.wifi_mask = m;
    G.lst_wifi = listen_on(a, SP.port);
    int u = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (u >= 0) {
        int one = 1; setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(u, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        struct sockaddr_in b = { 0 }; b.sin_family = AF_INET; b.sin_port = htons((uint16_t)SP.port); b.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(u, (struct sockaddr *)&b, sizeof b)) { close(u); u = -1; }
    }
    G.udp = u;
    char ip[INET_ADDRSTRLEN]; struct in_addr ia = { a }; inet_ntop(AF_INET, &ia, ip, sizeof ip);
    lg("wifi listener on %s:%d %s", ip, SP.port, G.lst_wifi >= 0 ? "ok" : "FAILED");
    announce(0, 0);
}
static int addr_private(uint32_t a_n)
{
    uint32_t a = ntohl(a_n);
#ifdef BTP_TEST
    if ((a >> 24) == 127) return 1;
#endif
    return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168) || (a >> 16) == (169u << 8 | 254);
}
static void udp_in(void)
{
    uint8_t b[64], nonce[8]; struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t n = recvfrom(G.udp, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
    if (n < 0 || btp_probe_parse(b, (size_t)n, nonce)) return;
    if (!addr_private(from.sin_addr.s_addr) || (from.sin_addr.s_addr & G.wifi_mask) != (G.wifi_addr & G.wifi_mask)) return;
    time_t t = mono(); if (t != G.probe_sec) { G.probe_sec = t; G.probe_n = 0; }
    if (++G.probe_n > 10) return;
    announce(nonce, &from);
}
static void accept_on(int lfd, int local)
{
    struct sockaddr_in a; socklen_t al = sizeof a;
    int fd = accept4(lfd, (struct sockaddr *)&a, &al, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    /* Slots are shared, so a LAN host must not be able to hold them:
     * - at most one unauthenticated Wi-Fi session at a time (it also has T_SECURE_S to finish);
     * - an address that keeps failing or timing out gets E_RATE_LIMIT for a while;
     * - a USB client that finds the slots full evicts the oldest unauthenticated Wi-Fi session
     *   (one waiting for the user's Allow only as a last resort). */
    int refuse = 0;
    if (!local) {
        if (rl_blocked(a.sin_addr.s_addr)) refuse = E_RATE_LIMIT;
        for (int i = 0; i < MAX_SESS && !refuse; i++) if (S[i].st && !S[i].local && S[i].st != SS_OPEN) refuse = E_BUSY;
    }
    struct sess *s = 0; for (int i = 0; i < MAX_SESS && !refuse; i++) if (!S[i].st) { s = &S[i]; break; }
    if (!s && !refuse && local) {
        struct sess *v = 0;
        for (int pass = 0; pass < 2 && !v; pass++)
            for (int i = 0; i < MAX_SESS; i++) {
                struct sess *o = &S[i];
                if (!o->st || o->local || o->st == SS_OPEN || (pass == 0 && o->ps == PS_WAIT_USER)) continue;
                if (!v || o->sid < v->sid) v = o;
            }
        if (v) {
            lg("USB client: evicting unauthenticated session %d", v->sid);
            if (!v->sf_left) tx_err(v, 0, E_BUSY);
            tx_flush(v); sess_close(v); s = v;
        }
    }
    if (!s) {   /* E_BUSY (or E_RATE_LIMIT) on stream 0, best effort, then close */
        int code = refuse ? refuse : E_BUSY; const char *m = err_text(code); size_t ml = strlen(m);
        uint8_t f[BTP_HDR + 4 + 64]; btp_hdr_build(f, T_ERROR, BTP_FLAG_END, 0, (uint32_t)(4 + ml));
        be16(f + BTP_HDR, (uint32_t)code); be16(f + BTP_HDR + 2, (uint32_t)ml); memcpy(f + BTP_HDR + 4, m, ml);
        if (send(fd, f, BTP_HDR + 4 + ml, MSG_NOSIGNAL) < 0) {}
        close(fd); return;
    }
    int idx = (int)(s - S);
    memset(s, 0, sizeof *s);
    s->fd = fd; s->st = SS_HELLO; s->local = local; s->peer_addr = a.sin_addr.s_addr; s->sid = ++G.next_sid;
    s->rx = RXB[idx]; s->tx = TXB[idx]; s->sf_fd = -1; s->peer_idx = -1; s->put.fd = s->put.dfd = -1; s->get.fd = -1;
    s->hello_deadline = mono() + T_HELLO_S; s->secure_deadline = mono() + SP.t_secure; s->last_rx = mono();
    s->hj.fd = -1;
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    rx_reset(s);
}

/* ================================================================== control input (18.3) */
static struct sess *sess_by_sid(int sid) { for (int i = 0; i < MAX_SESS; i++) if (S[i].st && S[i].sid == sid) return &S[i]; return 0; }
static char ctl_buf[1024]; static size_t ctl_len;
static void ctl_line(char *l)
{
    char cmd[32] = "", a1[160] = "", a2[64] = "", a3[64] = "";
    sscanf(l, "%31s %159s %63s %63s", cmd, a1, a2, a3);
    if (!strcmp(cmd, "ALLOW") || !strcmp(cmd, "DENY")) {
        struct sess *s = sess_by_sid(atoi(a1));
        if (!s || s->ps != PS_WAIT_USER) return;
        if (cmd[0] == 'A') { s->allowed = 1; if (s->confirmed) pair_finish(s); }
        else { pair_fail(s, "rejected"); fatal(s, 0, E_PAIR_REJECTED); }
    } else if (!strcmp(cmd, "PAIRING")) {
        G.pairing_open = !strcmp(a1, "on") && G.wifi;
        if (G.pairing_open) { G.pairing_until = mono() + PAIR_WINDOW_S; G.pair_fails = 0; G.pair_fail_start = mono(); announce(0, 0); }
    } else if (!strcmp(cmd, "FORGET")) {
        uint8_t id[16]; if (strlen(a1) != 32 || ota_unhex(a1, id, 16)) return;
        int i = peer_find(id); if (i < 0) return;
        G.peers[i] = G.peers[--G.npeers]; peers_save();
        for (int k = 0; k < MAX_SESS; k++) if (S[k].st && S[k].peer_idx >= 0 && btp_ct_eq(S[k].key_id, id, 16)) sess_close(&S[k]);
    } else if (!strcmp(cmd, "PEERS")) {
        for (int i = 0; i < G.npeers; i++) {
            char hx[33], nm[200]; hexs(G.peers[i].key_id, 16, hx); pct(G.peers[i].name, strlen(G.peers[i].name), nm, sizeof nm);
            ctl_send("PEER %s %lld %s", hx, G.peers[i].when, nm[0] ? nm : "-");
        }
        ctl_send("PEERS END");
    } else if (!strcmp(cmd, "UPDATE_STATE")) {
        G.up_state = atoi(a1); G.up_permille = atoi(a2); G.up_error = atoi(a3);
    } else if (!strcmp(cmd, "RESCAN_RESULT")) {
        struct sess *s = sess_by_sid(G.rescan_sid); G.rescan_sid = 0;
        if (!s || s->job != JOB_RESCAN) return;
        uint32_t js = s->job_stream; job_end(s);
        if (!strcmp(a1, "ok")) tx_ok(s, js); else tx_err(s, js, !strcmp(a1, "unsupported") ? E_UNSUPPORTED : E_IO);
    } else if (!strcmp(cmd, "SD")) {
        G.sd_shell = atoi(a1);
    } else if (!strcmp(cmd, "QUIT")) {
        G.quit = 1;
    } else lg("control: unknown line");
}
static int ctl_in(void)   /* -1 = EOF (the shell is gone) */
{
    ssize_t k = read(G.ctl, ctl_buf + ctl_len, sizeof ctl_buf - 1 - ctl_len);
    if (k == 0) return -1;
    if (k < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    ctl_len += (size_t)k; ctl_buf[ctl_len] = 0;
    char *nl;
    while ((nl = memchr(ctl_buf, '\n', ctl_len))) {
        *nl = 0; if (nl > ctl_buf && nl[-1] == '\r') nl[-1] = 0;
        if (strlen(ctl_buf) <= 512) ctl_line(ctl_buf);
        size_t used = (size_t)(nl + 1 - ctl_buf); memmove(ctl_buf, nl + 1, ctl_len - used); ctl_len -= used;
    }
    if (ctl_len >= sizeof ctl_buf - 1) ctl_len = 0;   /* overlong line: drop */
    return 0;
}

/* ================================================================== timers, shutdown, main loop */
static void timers(void)
{
    time_t t = mono();
    if (G.pairing_open && t > G.pairing_until) G.pairing_open = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        struct sess *s = &S[i]; if (!s->st || s->close_after_tx) continue;
        if (s->st == SS_HELLO && t > s->hello_deadline) { if (!s->local) rl_fail(s->peer_addr); sess_close(s); continue; }
        if (s->ps == PS_WAIT_USER && t > s->pair_deadline) { pair_fail(s, "timeout"); fatal(s, 0, E_PAIR_REJECTED); continue; }
        /* unauthenticated Wi-Fi: AUTH done or PAIR_REVEAL reached within T_SECURE_S (the user's
         * Allow/Deny has its own 120 s); a timeout counts against the address like a failed AUTH */
        if (!s->local && s->st == SS_SECURING && s->ps != PS_WAIT_USER && t > s->secure_deadline) {
            rl_fail(s->peer_addr);
            if (s->ps == PS_COMMITTED) pair_fail_n(s, "timeout", 0);
            fatal(s, 0, E_TIMEOUT); continue;
        }
        if (s->put.active && t - s->put.last > SP.t_put_idle) { uint32_t ps = s->put.stream; put_close(s, 0); fatal(s, ps, E_TIMEOUT); continue; }
        if (!s->job && s->ps != PS_WAIT_USER && t - s->last_rx > SP.t_idle) { fatal(s, 0, E_TIMEOUT); continue; }
    }
}
static void on_term(int sig) { (void)sig; G.term = 1; }
static void shutdown_all(int code_streams)
{
    if (code_streams) {
        for (int i = 0; i < MAX_SESS; i++) {
            struct sess *s = &S[i];
            if (!s->st || (s->st != SS_OPEN && s->st != SS_SECURING) || tx_flush(s) != 1) continue;
            if (s->job) { uint32_t js = s->job_stream; job_end(s); tx_err(s, js, E_CANCELLED); }
            if (s->put.active) { uint32_t ps = s->put.stream; put_close(s, 0); tx_err(s, ps, E_CANCELLED); }
            for (int k = 0; k < s->hq_n; k++) tx_err(s, s->hq[k].stream, E_CANCELLED);
            s->hq_n = 0; s->hq_bytes = 0;
            tx_frame(s, T_BYE, 0, 0, 0, 0);
        }
        /* one deadline for all sessions (18.2: exit within 1 s), their sockets polled together */
        long long until = now_ms() + 800;
        int done[MAX_SESS] = { 0 };
        for (;;) {
            struct pollfd pf[MAX_SESS]; int np = 0;
            for (int i = 0; i < MAX_SESS; i++) {
                struct sess *s = &S[i]; if (!s->st || done[i]) continue;
                if (tx_flush(s)) { done[i] = 1; continue; }   /* sent (1) or broken (-1) */
                pf[np].fd = s->fd; pf[np].events = POLLOUT; pf[np++].revents = 0;
            }
            long long left = until - now_ms();
            if (!np || left <= 0) break;
            poll(pf, (nfds_t)np, left > 100 ? 100 : (int)left);
        }
    }
    for (int i = 0; i < MAX_SESS; i++) if (S[i].st) sess_close(&S[i]);
}
static int serve(void)
{
    G.lst_lo = listen_on(htonl(INADDR_LOOPBACK), SP.port);
    if (G.lst_lo < 0) { lg("cannot listen on 127.0.0.1:%d: %s", SP.port, strerror(errno)); return 1; }
    G.idle_since = mono();
    if (G.wifi) wifi_refresh();
    char hx[17]; hexs(G.device_id, 8, hx);
    ctl_send("READY %d %d %s", SP.port, G.wifi, hx);
    lg("listening on 127.0.0.1:%d%s", SP.port, G.wifi ? " + wifi" : "");
    while (!G.quit && !G.term) {
        struct pollfd pf[8]; int map[8], np = 0;
        pf[np].fd = G.lst_lo; pf[np].events = POLLIN; map[np++] = -1;
        if (G.lst_wifi >= 0) { pf[np].fd = G.lst_wifi; pf[np].events = POLLIN; map[np++] = -2; }
        if (G.udp >= 0) { pf[np].fd = G.udp; pf[np].events = POLLIN; map[np++] = -3; }
        if (G.ctl >= 0) { pf[np].fd = G.ctl; pf[np].events = POLLIN; map[np++] = -4; }
        for (int i = 0; i < MAX_SESS; i++) if (S[i].st) { pf[np].fd = S[i].fd; pf[np].events = sess_events(&S[i]); map[np++] = i; }
        int r = poll(pf, (nfds_t)np, 1000);
        if (r < 0 && errno != EINTR) { lg("poll: %s", strerror(errno)); break; }
        for (int k = 0; r > 0 && k < np; k++) {
            if (!pf[k].revents) continue;
            if (map[k] == -1) accept_on(G.lst_lo, 1);
            else if (map[k] == -2) accept_on(G.lst_wifi, 0);
            else if (map[k] == -3) udp_in();
            else if (map[k] == -4) { if (ctl_in() < 0) { lg("control pipe closed: exiting"); shutdown_all(0); return 0; } }
            else if (S[map[k]].st && S[map[k]].fd == pf[k].fd) sess_service(&S[map[k]], pf[k].revents);
        }
        /* sessions whose state changed from outside (control lines, timers) */
        timers();
        for (int i = 0; i < MAX_SESS; i++) if (S[i].st && (S[i].tx_len > S[i].tx_off || S[i].close_after_tx)) sess_service(&S[i], 0);
        wifi_refresh();
        int any = 0; for (int i = 0; i < MAX_SESS; i++) if (S[i].st) any = 1;
        if (!any && G.idle_exit > 0 && mono() - G.idle_since >= G.idle_exit) { lg("idle for %d s: exiting", G.idle_exit); break; }
    }
    shutdown_all(1);
    ctl_send("BYE");
    return 0;
}

#ifndef BTP_NO_MAIN
static void usage(void)
{
    fprintf(stderr, "usage: hibysync serve [--control FD] [--wifi] [--idle-exit SECONDS] [--name NAME] [--detach]\n"
                    "       hibysync id            print the device id (creates the device key if needed)\n");
}
int main(int argc, char **argv)
{
    paths_init();
    if (argc < 2) { usage(); return 2; }
    snprintf(G.name, sizeof G.name, "HiBy R3 II");
    if (!strcmp(argv[1], "id")) {
        if (key_load_or_create()) { fprintf(stderr, "no device key\n"); return 1; }
        char hx[17]; hexs(G.device_id, 8, hx); printf("%s\n", hx); return 0;
    }
    if (strcmp(argv[1], "serve")) { usage(); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--control") && i + 1 < argc) G.ctl = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wifi")) G.wifi = 1;
        else if (!strcmp(argv[i], "--idle-exit") && i + 1 < argc) G.idle_exit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            char nm[200]; unpct(argv[++i], nm, sizeof nm);
            size_t nl = strlen(nm); if (nl <= BTP_NAME_MAX && btp_utf8_ok((const uint8_t *)nm, nl) && nl) memcpy(G.name, nm, nl + 1);
        }
        else if (!strcmp(argv[i], "--detach")) G.detach = 1;
        else { usage(); return 2; }
    }
    if (G.detach && (G.ctl >= 0 || G.wifi)) { fprintf(stderr, "--detach is for the USB fallback only (no --control, no --wifi)\n"); return 2; }
    if (G.ctl >= 0) {
        struct stat st; if (fstat(G.ctl, &st)) { fprintf(stderr, "--control %d: not open\n", G.ctl); return 2; }
        fcntl(G.ctl, F_SETFD, FD_CLOEXEC); fcntl(G.ctl, F_SETFL, fcntl(G.ctl, F_GETFL) | O_NONBLOCK);
    }
    if (G.detach) {
        /* adb closes the session as soon as the parent returns and HUPs its process group: a
         * child that had not called setsid() yet died with it (measured: 'byos-cli --start'
         * never got a daemon). Ignore SIGHUP first, then the classic double fork. */
        signal(SIGHUP, SIG_IGN);
        pid_t p = fork(); if (p < 0) return 1; if (p > 0) return 0;
        setsid();
        p = fork(); if (p < 0) return 1; if (p > 0) _exit(0);
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
    }
    int lk = open(SP.lock, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) { lg("another hibysync is running"); return 1; }
    char pb[16]; int pn = snprintf(pb, sizeof pb, "%d\n", (int)getpid()); if (ftruncate(lk, 0) || write(lk, pb, (size_t)pn) < 0) {}
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_term; sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0);
#ifdef SYS_ioprio_set
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
#endif
    if (key_load_or_create()) { lg("cannot create the device key in %s", SP.sync); return 1; }
    peers_load();
    versions_refresh();
    int rc = serve();
    unlink(SP.lock);
    return rc;
}
#endif
