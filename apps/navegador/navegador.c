/* navegador.c - lanzador de la app "Navegador" (NetSurf 3.11 estatico, ver apps/netsurf-hiby).
 *
 * El shell ejecuta bin/navegador (contrato de docs/APP_FORMAT.md §6). Este lanzador:
 *   - usa HM_DATA_DIR como HOME (Choices, cookies e historial persisten en appdata/<id>)
 *   - escribe ~/.netsurf/Choices con los ajustes medidos para ~7 MB de RAM libre
 *   - pone oom_score_adj=1000 (si falta memoria, el kernel mata al navegador y no al reproductor)
 *   - lanza bin/nsfb y hace de VIGILANTE DE MEMORIA: sin swap, cuando la RAM se agota el kernel no
 *     llega a matar a nadie sino que expulsa el codigo de todos los procesos (thrashing: el aparato
 *     deja de responder, adb incluido). Dos defensas:
 *       1. tope RLIMIT_AS a nsfb (prlimit) = su VmSize inicial + la RAM disponible menos el margen:
 *          malloc falla y NetSurf aborta la pagina antes de agotar la RAM;
 *       2. cada 50 ms se mira MemAvailable; si baja de MIN_AVAIL_KB se mata a nsfb (SIGKILL) y se
 *          sale con 99 (el shell avisa "sin memoria"). El vigilante va con mlockall + SCHED_FIFO
 *          para que siga respondiendo aunque el sistema empiece a expulsar paginas.
 *   - reenvia SIGTERM (barra de inicio) a nsfb y sale en < 1 s
 * Pagina de inicio: DuckDuckGo Lite (formulario HTML sin JS, unos 10 KB). */
#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define MIN_AVAIL_KB 2800

static volatile sig_atomic_t g_term;
static void on_term(int s) { (void)s; g_term = 1; }
static long mem_avail_kb(void)
{
    char b[512]; int fd = open("/proc/meminfo", O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1; b[n] = 0;
    char *p = strstr(b, "MemAvailable:"); return p ? atol(p + 13) : -1;
}
static long proc_kb(pid_t pid, const char *key)
{
    char p[64], b[1024]; snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    int fd = open(p, O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1; b[n] = 0;
    char *q = strstr(b, key); return q ? atol(q + strlen(key)) : -1;
}
static int is_nsfb(pid_t pid)
{
    char p[64], b[32] = ""; snprintf(p, sizeof p, "/proc/%d/comm", (int)pid);
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n > 0) b[n] = 0;
    return !strncmp(b, "nsfb", 4);
}
static void ms_sleep(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, 0); }

#define HOME_URL "https://lite.duckduckgo.com/lite/"

static void wr(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return;
    ssize_t n = write(fd, s, strlen(s)); (void)n; close(fd);
}

int main(int argc, char **argv)
{
    char app[256], data[256], p[400], res[300], exe[300], ch[2048];
    const char *a = getenv("HM_APP_DIR"), *d = getenv("HM_DATA_DIR");
    if (a && *a) snprintf(app, sizeof app, "%s", a);
    else { snprintf(app, sizeof app, "%s", argv[0]); char *s = strrchr(app, '/'); if (s) *s = 0; s = strrchr(app, '/'); if (s) *s = 0; else strcpy(app, "."); }
    snprintf(data, sizeof data, "%s", d && *d ? d : "/usr/data/hibymod/appdata/org.hibymod.navegador");
    mkdir(data, 0755);
    snprintf(p, sizeof p, "%s/.netsurf", data); mkdir(p, 0755);
    setenv("HOME", data, 1);
    snprintf(res, sizeof res, "%s/res/", app); setenv("NETSURFRES", res, 1);
    setenv("LANG", "en", 1);
    /* raices de confianza: subconjunto (ca.pem, 41 raices) -> mbedTLS analiza 3 veces menos */
    char ca[300]; snprintf(ca, sizeof ca, "%s/ca.pem", app);
    if (getenv("NAV_CA")) snprintf(ca, sizeof ca, "%s", getenv("NAV_CA"));
    else if (access(ca, R_OK)) snprintf(ca, sizeof ca, "/usr/data/hibymod/shell/cacert.pem");
    /* ajustes: cache en RAM pequena, sin cache de disco, sin JS, pocas conexiones a la vez,
     * sin imagenes de fondo (cada bitmap es RGBA de 32 bits en RAM) */
    snprintf(ch, sizeof ch,
        "fb_osk:1\nfb_toolbar_size:30\nfb_furniture_size:14\nfb_depth:16\n"
        "window_width:320\nwindow_height:480\n"
        "memory_cache_size:393216\ndisc_cache_size:0\nanimate_images:0\ncurl_fetch_timeout:20\n"
        "max_fetchers:2\nmax_fetchers_per_host:2\nmax_cached_fetch_handles:1\n"
        "enable_javascript:0\nblock_advertisements:0\nbackground_images:0\n"
        "do_not_track:1\nfont_min_size:100\nfont_size:120\n"
        "homepage_url:" HOME_URL "\n"
        "ca_bundle:%s\n"
        "cookie_file:%s/.netsurf/Cookies\ncookie_jar:%s/.netsurf/Cookies\n", ca, data, data);
    snprintf(p, sizeof p, "%s/.netsurf/Choices", data); wr(p, ch);
    if (chdir(data)) {}
    snprintf(exe, sizeof exe, "%s/bin/nsfb", app);
    char *nargv[] = { "nsfb", "-f", "linux", "-w", "320", "-h", "480", "-b", "16", (char *)(argc > 1 ? argv[1] : HOME_URL), 0 };
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); sigaction(SIGHUP, &sa, 0);
    long av0 = mem_avail_kb();
    pid_t pid = fork();
    if (pid == 0) {
        struct sched_param sp0 = { 0 }; sched_setscheduler(0, SCHED_OTHER, &sp0);
        signal(SIGTERM, SIG_DFL); wr("/proc/self/oom_score_adj", "1000");
        execv(exe, nargv); perror(exe); _exit(127);
    }
    if (pid < 0) return 127;
    /* el vigilante no puede quedarse sin CPU ni sin sus paginas cuando falta memoria:
     * todo bloqueado en RAM (60 KB) y prioridad de tiempo real (solo duerme y lee /proc) */
    mlockall(MCL_CURRENT | MCL_FUTURE);
    struct sched_param sp = { .sched_priority = 10 }; sched_setscheduler(0, SCHED_FIFO, &sp);
    int st = 0, killed = 0, termed = 0, capped = 0;
    long cap_kb = 0;
    for (int k = 0;; k++) {
        if (waitpid(pid, &st, WNOHANG) == pid) break;
        if (g_term && !termed) { kill(pid, SIGTERM); termed = k; }
        if (termed && k - termed > 14) { kill(pid, SIGKILL); waitpid(pid, &st, 0); break; }   /* 0,7 s */
        /* tope de memoria virtual: cuando nsfb ya es nsfb, VmSize actual + lo que se puede gastar
         * sin acercarse al limite (+1,6 MB que no es RAM: mapeo de /dev/fb0 y pilas de hilos).
         * Al llegar al tope malloc devuelve NULL y NetSurf aborta la carga en vez de agotar la RAM */
        if (!capped && k < 100) {
            long vs = proc_kb(pid, "VmSize:"), budget = (av0 > 0 ? av0 : 7000) - MIN_AVAIL_KB - 900;
            if (vs > 0 && is_nsfb(pid)) {
                if (budget < 1500) budget = 1500;
                struct rlimit rl; rl.rlim_cur = rl.rlim_max = (rlim_t)(vs + budget + 1600) * 1024;
                capped = !prlimit(pid, RLIMIT_AS, &rl, 0) ? 1 : -1; cap_kb = vs + budget + 1600;
                fprintf(stderr, "navegador: MemAvailable %ld kB, tope RLIMIT_AS %ld kB\n", av0, cap_kb);
            }
        }
        long av = mem_avail_kb();
        if (av >= 0 && av < MIN_AVAIL_KB) { kill(pid, SIGKILL); waitpid(pid, &st, 0); killed = 1; break; }
        ms_sleep(50);
    }
    if (killed) { fprintf(stderr, "navegador: sin memoria (MemAvailable < %d kB)\n", MIN_AVAIL_KB); return 99; }
    if (WIFSIGNALED(st) && WTERMSIG(st) != SIGTERM && WTERMSIG(st) != SIGKILL && capped == 1) {
        fprintf(stderr, "navegador: nsfb termino por la senal %d con el tope de memoria\n", WTERMSIG(st)); return 99;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 0;
}
