/* hibyguard - ByOS stability guard.
 *
 * The R3 II has 57 MB of RAM and its kernel has no swap. When memory runs short the kernel does
 * not kill anything: it keeps evicting the program code of every process from the page cache and
 * reading it back from flash (workingset_refault climbs by thousands per second). Everything
 * crawls, adbd cannot fork, the player stutters and the device looks frozen, sometimes for
 * minutes, sometimes for good. HiBy's stock firmware does the same after long uptimes.
 *
 * This daemon is built to keep working while that happens:
 *   - all of it is locked in RAM (mlockall) and it runs at real-time priority, so it is never
 *     paged out or starved;
 *   - every second it reads MemAvailable and the refault rate (pages read back per second);
 *   - under pressure it escalates, each step with its own cool-down:
 *       1. cut ByOS's helpers (covers, tags, mixes, network unless the radio is playing) and free
 *          the reclaimable kernel objects (never the page cache: that is the code everyone needs);
 *       2. ask the shell to restart itself in place (it does so at the home screen) and cut the
 *          headphone helpers;
 *       3. kill the shell if it is still big (its supervisor restarts it; the screen keeps its
 *          last image meanwhile);
 *       4. only if the device is truly frozen (severe pressure AND the shell, which beats at
 *          least every 300 ms, has not beaten for a minute although it runs), let the hardware
 *          watchdog restart it: a restart in ~30 s beats a frozen player. Heavy but working use
 *          never gets here. The reason goes to the SD card.
 *   - the player (and whatever is playing) is never touched; it, adbd and this guard get the
 *     strongest protection from the kernel's OOM killer.
 *
 * Hardware watchdog: fed every second while this runs. Disable it with the file
 * /usr/data/hibymod/guard_nowd. Two watchdog restarts within 30 minutes disable it by themselves
 * (guard_reboots), so a bug can never cause a restart loop. Any signal that stops the guard
 * disarms the watchdog first (magic close).
 *
 *   hibyguard            run (one instance; a second one exits)
 *   hibyguard reboot     restart the device through the watchdog (instead of "reboot", which
 *                        powers the R3 II off)
 *   hibyguard status     print the last status line
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "hm_shared.h"

#define DIR_TMP     "/tmp/hibymod"
#define LOG_TMP     DIR_TMP "/guard.log"
#define STATUS      DIR_TMP "/guard.status"
#define LOCKF       DIR_TMP "/guard.lock"
#define REBOOT_REQ  DIR_TMP "/guard_reboot"
#define REJUV_REQ   DIR_TMP "/rejuv_req"
#define NOWD        "/usr/data/hibymod/guard_nowd"
#define REBOOTS     "/usr/data/hibymod/guard_reboots"
#define SD_LOGDIR   "/usr/data/mnt/sd_0/ByOS/logs"

/* thresholds (KB of MemAvailable, pages refaulted per second) */
#define CALM_KB     6000
#define PRESS_KB    5000
#define SEV_KB      3000
#define PRESS_RR    600
#define SEV_RR      1500
#define L1_AFTER    3       /* s of pressure */
#define L2_AFTER    10
#define L3_AFTER    40      /* s of severe pressure */
#define L4_STALL    60      /* s without a shell heartbeat (while it runs) under severe pressure */
#define SHELL_BIG_KB 6000

static int wd = -1;
static volatile sig_atomic_t stopping;

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void wd_disarm(void)
{
    if (wd >= 0) { if (write(wd, "V", 1) != 1) {} close(wd); wd = -1; }
}
static void on_signal(int s)
{
    wd_disarm();
    if (s == SIGTERM || s == SIGINT || s == SIGHUP) { stopping = 1; _exit(0); }
    signal(s, SIG_DFL); raise(s);
}

static void logl(int to_sd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void logl(int to_sd, const char *fmt, ...)
{
    char msg[400]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    char line[480]; time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
    struct timespec up; clock_gettime(CLOCK_MONOTONIC, &up);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d up %ld %s\n", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long)up.tv_sec, msg);
    struct stat sb;
    if (!stat(LOG_TMP, &sb) && sb.st_size > 96 * 1024) rename(LOG_TMP, LOG_TMP ".old");
    int f = open(LOG_TMP, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (f >= 0) { if (write(f, line, n) != n) {} close(f); }
    if (to_sd && !stat("/usr/data/mnt/sd_0/ByOS", &sb)) mkdir(SD_LOGDIR, 0755);
    if (to_sd && !stat(SD_LOGDIR, &sb)) {
        if (!stat(SD_LOGDIR "/guard.log", &sb) && sb.st_size > 256 * 1024) rename(SD_LOGDIR "/guard.log", SD_LOGDIR "/guard.log.old");
        f = open(SD_LOGDIR "/guard.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (f >= 0) { if (write(f, line, n) != n) {} fsync(f); close(f); }
    }
}

static int read_file(const char *p, char *b, int cap)
{
    int f = open(p, O_RDONLY | O_CLOEXEC); if (f < 0) return -1;
    int n = (int)read(f, b, cap - 1); close(f); if (n < 0) return -1; b[n] = 0; return n;
}
static int write_str(const char *p, const char *s)
{
    int f = open(p, O_WRONLY | O_CLOEXEC); if (f < 0) return -1;
    int r = (int)write(f, s, strlen(s)); close(f); return r;
}
static long field(const char *b, const char *key)
{
    const char *m = strstr(b, key); return m ? atol(m + strlen(key)) : -1;
}

struct sample { long avail, refault; };
static int sample(struct sample *s)
{
    static char b[2048];
    if (read_file("/proc/meminfo", b, sizeof b) <= 0) return -1;
    s->avail = field(b, "MemAvailable:");
    if (read_file("/proc/vmstat", b, sizeof b) <= 0) return -1;
    s->refault = field(b, "workingset_refault ");
    return s->avail < 0 ? -1 : 0;
}

/* processes by name; act() returns 1 when it did something to that pid */
typedef int (*proc_fn)(int pid, const char *comm, void *ctx);
static int for_procs(proc_fn fn, void *ctx)
{
    DIR *d = opendir("/proc"); if (!d) return 0;
    struct dirent *e; int n = 0; char p[64], comm[32];
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        int k = read_file(p, comm, sizeof comm); if (k <= 0) continue;
        if (comm[k - 1] == '\n') comm[k - 1] = 0;
        n += fn(atoi(e->d_name), comm, ctx);
    }
    closedir(d);
    return n;
}
static int oom_adj_of(int pid)
{
    char p[64], b[16]; snprintf(p, sizeof p, "/proc/%d/oom_score_adj", pid);
    return read_file(p, b, sizeof b) > 0 ? atoi(b) : 0;
}
static int rss_kb_of(int pid)
{
    char p[64], b[96]; snprintf(p, sizeof p, "/proc/%d/statm", pid);
    long sz, res; if (read_file(p, b, sizeof b) <= 0 || sscanf(b, "%ld %ld", &sz, &res) != 2) return -1;
    return (int)(res * 4);
}
static int set_adj(int pid, int v)
{
    char p[64], b[16]; snprintf(p, sizeof p, "/proc/%d/oom_score_adj", pid); snprintf(b, sizeof b, "%d", v);
    return write_str(p, b) > 0;
}

static int is_in(const char *c, const char *const *list) { for (; *list; list++) if (!strcmp(c, *list)) return 1; return 0; }

static const char *const HELPERS[] = { "hibywall", "hibytag", "hibymix", 0 };
static const char *const HELPERS2[] = { "hibybudsd", "dbus-monitor", 0 };
static int kill_helpers(int pid, const char *comm, void *ctx)
{
    int lvl = *(int *)ctx;
    int victim = is_in(comm, HELPERS) || (lvl >= 2 && is_in(comm, HELPERS2))
              || (!strcmp(comm, "hibynet") && oom_adj_of(pid) > -500);   /* radio: the shell gives it -700 */
    return victim && kill(pid, SIGKILL) == 0;
}
static int find_shell(int pid, const char *comm, void *ctx) { if (!strcmp(comm, "hibyshell")) { *(int *)ctx = pid; return 1; } return 0; }
static int find_player(int pid, const char *comm, void *ctx)
{
    if (!strcmp(comm, "hiby_player") || !strcmp(comm, "system_main_thr")) { *(int *)ctx = pid; return 1; }
    return 0;
}
/* the strongest OOM protection for what must live: the player, adbd and ourselves */
static int protect(int pid, const char *comm, void *ctx)
{
    (void)ctx;
    if ((!strcmp(comm, "hiby_player") || !strcmp(comm, "system_main_thr")) && oom_adj_of(pid) > -1000) return set_adj(pid, -1000);
    if (!strcmp(comm, "adbd") && oom_adj_of(pid) > -800) return set_adj(pid, -800);
    return 0;
}

/* boot-loop breaker: two watchdog restarts within 30 minutes switch the watchdog off */
static int recent_reboots(time_t now)
{
    char b[512]; if (read_file(REBOOTS, b, sizeof b) <= 0) return 0;
    int n = 0; for (char *s = strtok(b, "\n"); s; s = strtok(0, "\n")) { long t = atol(s); if (t > 0 && now - t < 1800) n++; }
    return n;
}
static void wd_open(void)
{
    if (!access(NOWD, F_OK)) { logl(0, "watchdog: off (%s)", NOWD); return; }
    time_t now = time(0);
    if (recent_reboots(now) >= 2) {
        int f = open(NOWD, O_WRONLY | O_CREAT | O_CLOEXEC, 0644); if (f >= 0) close(f);
        logl(1, "watchdog: two restarts by the guard within 30 min -> watchdog switched off (delete %s to re-enable)", NOWD);
        return;
    }
    wd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (wd < 0) { logl(0, "watchdog: cannot open (%s)", strerror(errno)); return; }
    int to = 30; ioctl(wd, WDIOC_SETTIMEOUT, &to);
    to = 0; ioctl(wd, WDIOC_GETTIMEOUT, &to);
    logl(0, "watchdog: armed, timeout %d s", to);
}
static void wd_feed(void) { if (wd >= 0) { if (ioctl(wd, WDIOC_KEEPALIVE, 0) < 0 && write(wd, "1", 1) != 1) {} } }
static void wd_reboot(const char *why)
{
    logl(1, "RESTART through the watchdog: %s", why);
    if (strstr(why, "requested") == 0) {
        int f = open(REBOOTS, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (f >= 0) { char b[32]; int n = snprintf(b, sizeof b, "%ld\n", (long)time(0)); if (write(f, b, n) != n) {} fsync(f); close(f); }
    }
    sync();
    if (wd < 0) { wd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC); if (wd < 0) { logl(1, "watchdog unavailable: no restart"); return; } }
    int to = 5; ioctl(wd, WDIOC_SETTIMEOUT, &to);
    for (;;) pause();                         /* not fed any more: the hardware restarts the device */
}

static int run(void)
{
    mkdir(DIR_TMP, 0755);
    int lk = open(LOCKF, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) return 0;          /* already running */
    for (int fd = 3; fd < 256; fd++) if (fd != lk) close(fd);
    { int n = open("/dev/null", O_RDWR); if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); if (n > 2) close(n); } }
    setsid();
    char me[16]; snprintf(me, sizeof me, "%d", getpid());
    { int f = open("/proc/self/oom_score_adj", O_WRONLY); if (f >= 0) { if (write(f, "-1000", 5) != 5) {} close(f); } }
    int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGQUIT };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], on_signal);
    signal(SIGPIPE, SIG_IGN);
    struct sched_param sp = { .sched_priority = 20 };
    /* musl's sched_setscheduler() is a stub (ENOSYS) by design: the system call itself */
    int rt = syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) == 0;
    int locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
    logl(0, "start pid %s, real-time %s, locked in RAM %s", me, rt ? "yes" : "no", locked ? "yes" : "no");
    wd_open();

    struct sample prev; while (sample(&prev)) sleep(1);
    double t_prev = mono(), last_l1 = -1e9, last_l2 = -1e9, last_l3 = -1e9, last_status = 0, last_protect = -1e9;
    int press_s = 0, sev_s = 0, stall_s = 0;
    const volatile struct hm_state *hs = 0; uint32_t beat = 0; double beat_t = mono();
    long rr_acc = 0; int rr_n = 0, avail_min = 1 << 30;
    for (;;) {
        struct timespec ts = { 1, 0 }; nanosleep(&ts, 0);
        wd_feed();
        if (!access(REBOOT_REQ, F_OK)) { unlink(REBOOT_REQ); wd_reboot("requested (hibyguard reboot)"); }
        struct sample s; if (sample(&s)) continue;
        double t = mono(), dt = t - t_prev; if (dt <= 0) dt = 1;
        long rr = s.refault >= prev.refault ? (long)((s.refault - prev.refault) / dt) : 0;
        prev = s; t_prev = t;
        rr_acc += rr; rr_n++; if (s.avail < avail_min) avail_min = (int)s.avail;

        int press = s.avail < PRESS_KB || rr > PRESS_RR;
        int sev = s.avail < SEV_KB || rr > SEV_RR;
        if (s.avail >= CALM_KB && rr < PRESS_RR / 2) { press_s = 0; sev_s = 0; }
        else { if (press) press_s++; else if (press_s > 0) press_s--; if (sev) sev_s++; else if (sev_s > 0) sev_s--; }

        if (t - last_protect > 10) { last_protect = t; for_procs(protect, 0); }
        /* boot: everything is loading (many normal re-reads); no action in the first 90 s */
        { struct timespec up; clock_gettime(CLOCK_MONOTONIC, &up); if (up.tv_sec < 90) { press_s = sev_s = 0; continue; } }

        if (press_s >= L1_AFTER && t - last_l1 > 20) {
            last_l1 = t; int lvl = 1;
            int k = for_procs(kill_helpers, &lvl);
            write_str("/proc/sys/vm/drop_caches", "2");
            logl(0, "pressure: %ld KB free, %ld refaults/s -> step 1: %d helpers cut, kernel objects freed", s.avail, rr, k);
        }
        if (press_s >= L2_AFTER && t - last_l2 > 60) {
            last_l2 = t; int lvl = 2;
            int k = for_procs(kill_helpers, &lvl);
            int f = open(REJUV_REQ, O_WRONLY | O_CREAT | O_CLOEXEC, 0644); if (f >= 0) close(f);
            logl(1, "pressure for %d s: %ld KB free, %ld refaults/s -> step 2: shell asked to restart itself, %d helpers cut", press_s, s.avail, rr, k);
        }
        if (sev_s >= L3_AFTER && t - last_l3 > 120) {
            int sh = 0; for_procs(find_shell, &sh);
            int rss = sh ? rss_kb_of(sh) : -1;
            if (sh && rss > SHELL_BIG_KB) {
                last_l3 = t;
                kill(sh, SIGKILL);
                logl(1, "severe for %d s: %ld KB free, %ld refaults/s -> step 3: shell (%d KB) restarted", sev_s, s.avail, rr, rss);
            }
        }
        /* frozen? the shell's heartbeat (shared with the display library) */
        if (!hs) {
            int f = open(HM_STATE, O_RDONLY | O_CLOEXEC);
            if (f >= 0) { void *m = mmap(0, HM_STATE_SIZE, PROT_READ, MAP_SHARED, f, 0); close(f); if (m != MAP_FAILED) { hs = m; beat = hs->shell_beat; beat_t = t; } }
        }
        int sh_alive = 0; if (hs) for_procs(find_shell, &sh_alive);
        if (!hs || !sh_alive || hs->shell_beat != beat) { if (hs) beat = hs->shell_beat; beat_t = t; }
        if (sev && t - beat_t >= L4_STALL) stall_s++; else stall_s = 0;
        if (stall_s >= 5) {
            char why[200]; snprintf(why, sizeof why, "frozen: no shell heartbeat for %.0f s, %ld KB free, %ld refaults/s", t - beat_t, s.avail, rr);
            wd_reboot(why);
        }

        if (t - last_status >= 30) {
            int sh = 0, pl = 0; for_procs(find_shell, &sh); for_procs(find_player, &pl);
            char st[200]; int n = snprintf(st, sizeof st, "free %ld KB (min %d), refaults %ld/s avg, shell %d KB, player %d KB, watchdog %s",
                                           s.avail, avail_min, rr_n ? rr_acc / rr_n : 0, sh ? rss_kb_of(sh) : -1, pl ? rss_kb_of(pl) : -1, wd >= 0 ? "on" : "off");
            int f = open(STATUS, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (f >= 0) { if (write(f, st, n) != n) {} close(f); }
            if (t - last_status >= 300 || press_s) logl(0, "%s", st);
            if (t - last_status >= 300) { rr_acc = 0; rr_n = 0; avail_min = 1 << 30; }
            last_status = t;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "reboot")) {
        mkdir(DIR_TMP, 0755);
        int lk = open(LOCKF, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lk >= 0 && flock(lk, LOCK_EX | LOCK_NB) == 0) {   /* no guard running: do it ourselves */
            wd_reboot("requested (hibyguard reboot, no guard running)");
            return 1;
        }
        int f = open(REBOOT_REQ, O_WRONLY | O_CREAT | O_CLOEXEC, 0644); if (f >= 0) close(f);
        printf("restart requested: the player restarts within a minute (the card is flushed first)\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "status")) {
        char b[256]; if (read_file(STATUS, b, sizeof b) > 0) printf("%s\n", b); else printf("no status yet\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "run")) { fprintf(stderr, "usage: hibyguard [run|reboot|status]\n"); return 2; }
    if (fork() > 0) return 0;                 /* detach: the caller never waits for us */
    return run();
}
