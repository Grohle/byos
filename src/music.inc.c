/* music.inc.c - "Musica": biblioteca al estilo de Apple Music (iOS, tema oscuro).
 *  - Biblioteca: Listas, Artistas, Albumes, Canciones, Generos, Mezclas y "Biblioteca de HiBy"
 *    (la de fabrica, de reserva); debajo, "Anadido recientemente" en cuadricula.
 *  - Album: caratula grande, titulo, artista, genero y ano, Reproducir / Aleatorio y pistas.
 *  - Artista: sus albumes; Buscar: teclado con resultados mientras se escribe.
 *  - Pulsacion larga en una cancion: menu contextual de iOS (nowui.inc.c: fondo desenfocado,
 *    tarjeta de la fila y lista con iconos): Reproducir, Crear radio, Ir al album, Ir al artista,
 *    Editar etiquetas, Buscar letra y Buscar caratula (lo mismo que Archivos: "solo esta cancion
 *    / todo el album"). En un album: Reproducir, Aleatorio, Ir al artista, Editar, Buscar caratula.
 *  - Mini reproductor abajo (npx_snapshot + pc_*): tocarlo o arrastrarlo hacia arriba abre el
 *    Reproduciendo propio (nowui.inc.c), no el de HiBy.
 * Datos: `hibymix lib|playlists|playlist|make|radio` (SQLite en otro proceso, solo lectura);
 * la biblioteca se guarda en MU_CACHE/lib.tsv y se rehace cuando cambia la base.
 * Caratulas: un hilo (nice 10) las saca de cada album con hibywall a 140x140 (cuadricula) y
 * 48x48 (listas) en disco, y 232x232 para la pagina del album; la interfaz nunca decodifica.
 * En RAM: cache LRU de ranuras; las que llegan aparecen con un fundido.
 * Rendimiento: nada reserva memoria por fotograma; los glifos van en la cache de text.inc.c;
 * el contenido se pinta solo en la franja visible (busqueda binaria de la primera fila). */
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>

#define MU_ACC rgb(250, 45, 72)          /* rosa de Apple Music */
#define MU_GRAY rgb(142, 142, 147)
#define MU_SEP rgb(38, 38, 40)
#define MU_NAV 74                        /* fin de la barra de navegacion */
#define MU_MINI_H 60
#define MU_GS 140                        /* caratula de la cuadricula */
#define MU_SS 48                         /* caratula de las listas */
#define MU_BS 232                        /* caratula de la pagina del album */
#define MU_CACHE "/usr/data/mnt/sd_0/ByOS/cache/music"   /* en la SD: /usr/data va justo */
#define MU_CAP (48LL << 20)                               /* tope de la cache de caratulas */
#define MU_BIN ASSETS "/hibymix"
#define MU_DB "/usr/data/usrlocal_media.db"
#define MU_SD "/usr/data/mnt/sd_0/"


/* ------------------------------------------------------------------ biblioteca */
struct mu_alb { char *title, *artist, *genre, *fk, *afk; int year, n, first, art, mins; long ctime; };
struct mu_trk { char *title, *artist, *path, *fk; int alb, dur; short disc, num; };
struct mu_art { char *name, *fk; int first, n; };
struct mu_lib {
    char *buf, *fold;
    struct mu_alb *A; struct mu_trk *T; struct mu_art *R;
    int nA, nT, nR, ng;
    int *albs, *artalb, *songs, *recent, *ph;       /* albumes por titulo, por artista, canciones, recientes, hash de rutas */
    unsigned hmask;
    char **gen;
};
static struct mu_lib *ML;
static struct mu_lib *volatile mu_lib_new;
static volatile int mu_lib_state;                  /* 0 nada, 1 cargando, 2 lista para adoptar, 3 error */
static pthread_mutex_t mu_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mu_cv = PTHREAD_COND_INITIALIZER;
static volatile int mu_open, mu_dirty;
static volatile double mu_last_input;
static volatile int mu_busy;                       /* desplazando / animando: el hilo no adelanta trabajo */

/* minusculas y sin tildes (latin-1); el resto se copia: buscar y ordenar */
static int mu_fold(const char *s, char *o, int cap)
{
    static const char up[] = "aaaaaaaceeeeiiiidnooooo*ouuuuyts", lo[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < cap - 1; p++) {
        unsigned c = *p;
        if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { c = p[1] < 0xA0 ? up[p[1] - 0x80] : lo[p[1] - 0xA0]; p++; }
        else if (c >= 'A' && c <= 'Z') c += 32;
        o[n++] = (char)c;
    }
    o[n] = 0;
    return n;
}
static const char *mu_skey(const char *fk)     /* para ordenar: sin signos delante ni "the " */
{
    while (*fk && (unsigned char)*fk < 0x80 && !((*fk >= 'a' && *fk <= 'z') || (*fk >= '0' && *fk <= '9'))) fk++;
    if (!strncmp(fk, "the ", 4)) fk += 4;
    return fk;
}
static int mu_kcmp(const char *a, const char *b)   /* como iOS: numeros y simbolos al final */
{
    a = mu_skey(a); b = mu_skey(b);
    int ga = !((*a >= 'a' && *a <= 'z') || (unsigned char)*a >= 0x80), gb = !((*b >= 'a' && *b <= 'z') || (unsigned char)*b >= 0x80);
    if (ga != gb) return ga - gb;
    return strcmp(a, b);
}
static struct mu_lib *mu_cmp_lib;
static int mu_cmp_alb(const void *x, const void *y)
{
    const struct mu_alb *a = &mu_cmp_lib->A[*(const int *)x], *b = &mu_cmp_lib->A[*(const int *)y];
    int c = mu_kcmp(a->fk, b->fk); return c ? c : mu_kcmp(a->afk, b->afk);
}
static int mu_cmp_artalb(const void *x, const void *y)
{
    const struct mu_alb *a = &mu_cmp_lib->A[*(const int *)x], *b = &mu_cmp_lib->A[*(const int *)y];
    int c = mu_kcmp(a->afk, b->afk); if (c) return c;
    if (a->year != b->year) return b->year - a->year;            /* lo mas nuevo primero (iOS) */
    return mu_kcmp(a->fk, b->fk);
}
static int mu_cmp_song(const void *x, const void *y)
{
    const struct mu_trk *a = &mu_cmp_lib->T[*(const int *)x], *b = &mu_cmp_lib->T[*(const int *)y];
    int c = mu_kcmp(a->fk, b->fk); return c ? c : *(const int *)x - *(const int *)y;
}
static int mu_cmp_recent(const void *x, const void *y)
{
    long a = mu_cmp_lib->A[*(const int *)x].ctime, b = mu_cmp_lib->A[*(const int *)y].ctime;
    return a < b ? 1 : a > b ? -1 : 0;
}
static int mu_cmp_str(const void *x, const void *y) { return strcasecmp(*(char *const *)x, *(char *const *)y); }
static unsigned mu_phash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { unsigned char c = *s; if (c >= 'A' && c <= 'Z') c += 32; if (c == '/') c = '\\'; h = (h ^ c) * 16777619u; }
    return h;
}
static void mu_lib_free(struct mu_lib *L)
{
    if (!L) return;
    free(L->buf); free(L->fold); free(L->A); free(L->T); free(L->R);
    free(L->albs); free(L->artalb); free(L->songs); free(L->recent); free(L->ph); free(L->gen);
    free(L);
}
/* "a:\Artista\Album\x.flac" -> "/usr/data/mnt/sd_0/Artista/Album/x.flac" */
static void mu_syspath(const char *db, char *o, int cap)
{
    const char *p = db; if (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) p += 3;
    int n = snprintf(o, cap, MU_SD "%s", p); (void)n;
    for (char *c = o; *c; c++) if (*c == '\\') *c = '/';
}
static int mu_find_path(const struct mu_lib *L, const char *dbpath)
{
    if (!L || !L->ph) return -1;
    const char *q = dbpath; if (q[0] && q[1] == ':' && (q[2] == '\\' || q[2] == '/')) q += 3;
    for (unsigned h = mu_phash(q) & L->hmask; L->ph[h] >= 0; h = (h + 1) & L->hmask) {
        const char *p = L->T[L->ph[h]].path; if (p[0] && p[1] == ':') p += 3;
        const char *a = p, *b = q;
        for (; *a && *b; a++, b++) { if ((*a == '\\' || *a == '/') && (*b == '\\' || *b == '/')) continue; if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) break; }
        if (!*a && !*b) return L->ph[h];
    }
    return -1;
}
/* ruta del sistema (/usr/data/mnt/sd_0/...) -> pista */
static int mu_find_sys(const struct mu_lib *L, const char *sys)
{
    if (!strncmp(sys, MU_SD, strlen(MU_SD))) return mu_find_path(L, sys + strlen(MU_SD));
    if (!strncmp(sys, "/data/mnt/sd_0/", 15)) return mu_find_path(L, sys + 15);
    return -1;
}

static int mu_sd_mounted(void)
{
    FILE *f = fopen("/proc/mounts", "r"); char l[256]; int ok = 0;
    while (f && fgets(l, sizeof l, f)) if (strstr(l, " /usr/data/mnt/sd_0 ") || strstr(l, " /data/mnt/sd_0 ")) { ok = 1; break; }
    if (f) fclose(f);
    return ok;
}
static int mu_split(char *l, char **f, int max)
{
    int n = 0; f[n++] = l;
    for (char *p = l; *p && n < max; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    return n;
}
static struct mu_lib *mu_parse(char *buf, int len)
{
    struct mu_lib *L = calloc(1, sizeof *L); if (!L) { free(buf); return 0; }
    L->buf = buf;
    int na = 0, nt = 0;
    for (char *p = buf; p < buf + len; p++) if (*p == '\n') { if (p + 1 < buf + len && p[1] == 'A') na++; else if (p + 1 < buf + len && p[1] == 'T') nt++; }
    if (buf[0] == 'A') na++; else if (buf[0] == 'T') nt++;
    L->A = calloc(na + 1, sizeof *L->A); L->T = calloc(nt + 1, sizeof *L->T);
    L->fold = malloc(len + 16);
    if (!L->A || !L->T || !L->fold) { mu_lib_free(L); return 0; }
    char *fp = L->fold, *fe = L->fold + len + 16;
    #define MU_FOLD(dst, s) do { int k_ = mu_fold((s), fp, (int)(fe - fp)); (dst) = fp; fp += k_ + 1; } while (0)
    for (char *l = buf, *nl; l < buf + len; l = nl + 1) {
        nl = memchr(l, '\n', buf + len - l); if (!nl) nl = buf + len;
        *nl = 0;
        char *f[9]; int k = mu_split(l, f, 9);
        if (l[0] == 'A' && k >= 7 && L->nA < na) {
            struct mu_alb *a = &L->A[L->nA++];
            a->title = f[1]; a->artist = f[2]; a->year = atoi(f[3]); a->ctime = atol(f[5]); a->genre = f[6]; a->first = -1;
            MU_FOLD(a->fk, a->title); MU_FOLD(a->afk, a->artist);
        } else if (l[0] == 'T' && k >= 8 && L->nT < nt) {
            int ai = atoi(f[1]); if (ai < 0 || ai >= L->nA) continue;
            struct mu_trk *t = &L->T[L->nT];
            t->alb = ai; t->disc = (short)atoi(f[2]); t->num = (short)atoi(f[3]); t->dur = atoi(f[4]);
            t->title = f[5]; t->artist = f[6]; t->path = f[7];
            MU_FOLD(t->fk, t->title);
            struct mu_alb *a = &L->A[ai];
            if (a->first < 0) a->first = L->nT;
            a->n++; a->mins += t->dur;
            L->nT++;
        }
        if (nl >= buf + len) break;
    }
    #undef MU_FOLD
    for (int i = 0; i < L->nA; i++) { L->A[i].mins = (L->A[i].mins + 30) / 60; if (L->A[i].first < 0) L->A[i].first = 0; }
    mu_cmp_lib = L;
    /* albumes por titulo (con tildes bien ordenadas) */
    L->albs = malloc((L->nA + 1) * sizeof(int)); L->artalb = malloc((L->nA + 1) * sizeof(int)); L->recent = malloc((L->nA + 1) * sizeof(int));
    L->songs = malloc((L->nT + 1) * sizeof(int)); L->R = calloc(L->nA + 1, sizeof *L->R);
    unsigned hs = 1024; while (hs < (unsigned)L->nT * 2 + 64) hs <<= 1;
    L->ph = malloc(hs * sizeof(int)); L->hmask = hs - 1;
    L->gen = calloc(L->nA + 1, sizeof(char *));
    if (!L->albs || !L->artalb || !L->recent || !L->songs || !L->R || !L->ph || !L->gen) { mu_lib_free(L); return 0; }
    for (int i = 0; i < L->nA; i++) L->albs[i] = L->artalb[i] = L->recent[i] = i;
    qsort(L->albs, L->nA, sizeof(int), mu_cmp_alb);
    qsort(L->artalb, L->nA, sizeof(int), mu_cmp_artalb);
    qsort(L->recent, L->nA, sizeof(int), mu_cmp_recent);
    for (int i = 0; i < L->nT; i++) L->songs[i] = i;
    qsort(L->songs, L->nT, sizeof(int), mu_cmp_song);
    /* artistas (del album) */
    for (int i = 0; i < L->nA; i++) {
        int a = L->artalb[i];
        if (!L->nR || strcmp(L->A[L->artalb[i - 1]].afk, L->A[a].afk)) { struct mu_art *r = &L->R[L->nR++]; r->name = L->A[a].artist; r->fk = L->A[a].afk; r->first = i; r->n = 0; }
        L->R[L->nR - 1].n++; L->A[a].art = L->nR - 1;
    }
    /* generos */
    for (int i = 0; i < L->nA; i++) {
        const char *g = L->A[i].genre; if (!g[0]) continue;
        int dup = 0; for (int k = 0; k < L->ng; k++) if (!strcasecmp(L->gen[k], g)) { dup = 1; break; }
        if (!dup) L->gen[L->ng++] = (char *)g;
    }
    qsort(L->gen, L->ng, sizeof(char *), mu_cmp_str);
    /* rutas -> pista */
    memset(L->ph, 0xff, hs * sizeof(int));
    for (int i = 0; i < L->nT; i++) {
        const char *p = L->T[i].path; if (p[0] && p[1] == ':') p += 3;
        unsigned h = mu_phash(p) & L->hmask; while (L->ph[h] >= 0) h = (h + 1) & L->hmask;
        L->ph[h] = i;
    }
    return L;
}

/* ejecuta argv y devuelve su salida en un bufer nuevo (crece); -1 si no se pudo */
static int mu_capture(char *const argv[], char **out)
{
    *out = 0;
    int fd[2]; if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    int n = 0, cap = 0; char *b = 0;
    if (!r) for (;;) {
        if (cap - n < 16384) { int nc = cap ? cap * 2 : 65536; char *nb = realloc(b, nc); if (!nb) break; b = nb; cap = nc; }
        ssize_t k = read(fd[0], b + n, cap - n - 1); if (k <= 0) break; n += (int)k;
    }
    close(fd[0]);
    int stt = 0; if (!r) waitpid(pid, &stt, 0);
    if (r || !b) { free(b); return -1; }
    b[n] = 0; *out = b;
    return WIFEXITED(stt) && WEXITSTATUS(stt) == 0 ? n : (n ? n : -1);
}
static void mu_mkcache(void) { mkdir("/usr/data/mnt/sd_0/ByOS", 0755); mkdir("/usr/data/mnt/sd_0/ByOS/cache", 0755); mkdir(MU_CACHE, 0755); }
static void *mu_load_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
    struct stat s; long m = 0, sz = 0;
    if (!stat(MU_DB, &s)) { m = (long)s.st_mtime; sz = (long)s.st_size; }
    char hdr[64]; snprintf(hdr, sizeof hdr, "#mulib1 %ld %ld\n", m, sz);
    char *buf = 0; int len = -1;
    int f = open(MU_CACHE "/lib.tsv", O_RDONLY);
    if (f >= 0) {
        struct stat cs;
        if (!fstat(f, &cs) && cs.st_size > (off_t)strlen(hdr) && cs.st_size < (64 << 20) && (buf = malloc(cs.st_size + 1))) {
            if (read(f, buf, cs.st_size) == cs.st_size && !memcmp(buf, hdr, strlen(hdr))) {
                int hl = (int)strlen(hdr); len = (int)cs.st_size - hl; memmove(buf, buf + hl, len); buf[len] = 0;
            } else { free(buf); buf = 0; }
        }
        close(f);
    }
    double t0 = now_ms();
    if (len < 0) {
        char *argv[] = { MU_BIN, "lib", 0 };
        len = mu_capture(argv, &buf);
        lg("musica: hibymix lib %d bytes en %.0f ms", len, now_ms() - t0);
        if (len > 0) {
            mu_mkcache();
            int o = open(MU_CACHE "/lib.tsv.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (o >= 0) {
                int ok = write(o, hdr, strlen(hdr)) == (ssize_t)strlen(hdr) && write(o, buf, len) == len;
                close(o); if (ok) rename(MU_CACHE "/lib.tsv.tmp", MU_CACHE "/lib.tsv"); else unlink(MU_CACHE "/lib.tsv.tmp");
            }
        }
    }
    struct mu_lib *L = len >= 0 && buf ? mu_parse(buf, len) : (free(buf), (struct mu_lib *)0);
    if (L) lg("musica: biblioteca %d albumes, %d pistas, %d artistas (%.0f ms)", L->nA, L->nT, L->nR, now_ms() - t0);
    mu_lib_new = L;
    __sync_synchronize();
    mu_lib_state = L ? 2 : 3; mu_dirty = 1;
    return 0;
}

/* ------------------------------------------------------------------ caratulas */
enum { MC_M, MC_S, MC_B };
static const int MU_CSZ[3] = { MU_GS, MU_SS, MU_BS };
#define MU_NM 14
#define MU_NSM 40
struct mu_slot { int alb, state, cls; unsigned use; double t_ready; uint16_t *px; };   /* state: 0 libre, 1 pedida, 2 en curso, 3 lista, 4 sin caratula */
static struct mu_slot mu_sl[MU_NM + MU_NSM + 1];
static unsigned mu_clock;
static volatile int mu_cov_new;
static int mu_wk_started;
static long long mu_cache_bytes = -1;
static int mu_pref_i;                              /* siguiente album a adelantar en reposo */

static struct mu_slot *mu_slots(int cls, int *n)
{
    if (cls == MC_M) { *n = MU_NM; return mu_sl; }
    if (cls == MC_S) { *n = MU_NSM; return mu_sl + MU_NM; }
    *n = 1; return mu_sl + MU_NM + MU_NSM;
}
/* caratula lista del album (o NULL; la pide). *t: cuando llego; *none: no tiene */
static const uint16_t *mu_cover(int alb, int cls, double *t, int *none)
{
    int n; struct mu_slot *s = mu_slots(cls, &n), *victim = 0;
    const uint16_t *r = 0; *none = 0; *t = 0;
    pthread_mutex_lock(&mu_mu);
    for (int i = 0; i < n; i++) if (s[i].state && s[i].alb == alb) {
        s[i].use = ++mu_clock;
        if (s[i].state == 3) { r = s[i].px; *t = s[i].t_ready; }
        else if (s[i].state == 4) *none = 1;
        pthread_mutex_unlock(&mu_mu);
        return r;
    }
    for (int i = 0; i < n; i++) {
        if (s[i].state == 2) continue;
        if (!s[i].state) { victim = &s[i]; break; }
        if (!victim || s[i].use < victim->use) victim = &s[i];
    }
    if (victim) {
        victim->alb = alb; victim->state = 1; victim->cls = cls; victim->use = ++mu_clock;
        pthread_cond_signal(&mu_cv);
    }
    pthread_mutex_unlock(&mu_mu);
    return 0;
}
static void mu_cover_forget(int alb)
{
    pthread_mutex_lock(&mu_mu);
    for (unsigned i = 0; i < sizeof mu_sl / sizeof *mu_sl; i++) if (mu_sl[i].state && mu_sl[i].state != 2 && mu_sl[i].alb == alb) mu_sl[i].state = 0;
    pthread_mutex_unlock(&mu_mu);
}

static int mu_spawn_wait(char *const argv[])
{
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r) return -1;
    int stt = 0; while (waitpid(pid, &stt, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(stt) && WEXITSTATUS(stt) == 0 ? 0 : -1;
}
/* memoria libre del sistema (KB). El aparato va muy justo (~13 MB): hibywall con una JPEG
 * progresiva grande pide > 8 MB y el OOM acababa matando al shell. */
static int mu_mem_avail_kb(void)
{
    char b[512]; int f = open("/proc/meminfo", O_RDONLY); if (f < 0) return 0;
    int n = (int)read(f, b, sizeof b - 1); close(f); if (n <= 0) return 0; b[n] = 0;
    char *m = strstr(b, "MemAvailable:"); return m ? atoi(m + 13) : 0;
}
/* hibywall con un tope de memoria virtual (ulimit -v): si no le basta, falla sin tumbar a nadie */
static int mu_wall(const char *src, const char *out, int s, int lim_kb)
{
    char sz[8], cmd[64]; snprintf(sz, sizeof sz, "%d", s);
    snprintf(cmd, sizeof cmd, "ulimit -v %d; exec \"$0\" \"$@\"", lim_kb);
    char *argv[] = { "/bin/sh", "-c", cmd, NP_WALLBIN, (char *)src, (char *)out, sz, sz, 0 };
    return mu_spawn_wait(argv);
}
static int mu_read_raw(const char *p, uint16_t *o, int s)
{
    int f = open(p, O_RDONLY); if (f < 0) return -1;
    ssize_t n = read(f, o, s * s * 2); close(f);
    return n == s * s * 2 ? 0 : -1;
}
static int mu_write_raw(const char *p, const uint16_t *px, int s)
{
    char t[300]; snprintf(t, sizeof t, "%s.tmp", p);
    int f = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f < 0) return -1;
    int ok = write(f, px, s * s * 2) == s * s * 2; close(f);
    if (!ok || rename(t, p)) { unlink(t); return -1; }
    if (mu_cache_bytes >= 0) mu_cache_bytes += s * s * 2;
    return 0;
}
/* reduccion por promedio de area (cuadrada) */
static void mu_scale(const uint16_t *src, int sw, uint16_t *dst, int dw)
{
    for (int y = 0; y < dw; y++) {
        int y0 = y * sw / dw, y1 = (y + 1) * sw / dw; if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw; if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, c = 0;
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { uint16_t p = src[yy * sw + xx]; r += p >> 11; g += (p >> 5) & 63; b += p & 31; c++; }
            dst[y * dw + x] = (uint16_t)((r / c) << 11 | (g / c) << 5 | (b / c));
        }
    }
}
/* imagen de origen del album: la incrustada (a un fichero temporal) o la de la carpeta */
static int mu_cover_src(const char *track, const char *dir, char *out, int cap)
{
    struct np_pic pic = { 0, 0, 0 }; int have = 0;
    int fd = open(track, O_RDONLY);
    if (fd >= 0) {
        struct stat s; unsigned char h[12] = { 0 };
        if (!fstat(fd, &s)) {
            uint64_t b = np_id3_skip(fd);
            if (pread(fd, h, 12, (off_t)b) < 4) memset(h, 0, sizeof h);
            if (!memcmp(h, "fLaC", 4)) have = np_pic_flac(fd, b, &pic);
            else if (!memcmp(h + 4, "ftyp", 4)) have = np_pic_m4a(fd, (uint64_t)s.st_size, &pic);
            if (!have) have = np_pic_id3(fd, &pic);
            if (have && (pic.len < 64 || pic.len > (16u << 20) || pic.off + pic.len > (uint64_t)s.st_size)) have = 0;
        }
        if (have) {
            snprintf(out, cap, MU_CACHE "/w.img");
            int w = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644), ok = w >= 0;
            char buf[16384]; uint64_t left = pic.len, o = pic.off;
            while (ok && left) {
                size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
                if (pread(fd, buf, n, (off_t)o) != (ssize_t)n || write(w, buf, n) != (ssize_t)n) ok = 0;
                left -= n; o += n;
            }
            if (w >= 0) close(w);
            close(fd);
            if (ok) return 1;
        } else close(fd);
    }
    static const char *names[] = { "cover", "folder", "front", "Cover", "Folder", "Front", "AlbumArt", 0 };
    static const char *exts[] = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG", 0 };
    for (int i = 0; names[i]; i++) for (int j = 0; exts[j]; j++) {
        snprintf(out, cap, "%s/%s%s", dir, names[i], exts[j]);
        if (!access(out, R_OK)) return 2;
    }
    return 0;
}
/* clave del album en disco: carpeta + fecha de la pista y de la carpeta (cambian al poner caratula) */
static uint32_t mu_cover_key(const char *track, char *dir, int cap)
{
    snprintf(dir, cap, "%s", track); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    struct stat a, b; uint32_t m1 = 0, m2 = 0;
    if (!stat(track, &a)) m1 = (uint32_t)a.st_mtime ^ (uint32_t)a.st_size * 2654435761u;
    if (!stat(dir, &b)) m2 = (uint32_t)b.st_mtime;
    return np_hash(dir, m1 * 31u + m2);
}
/* genera/lee la caratula cls del album en px; 0 bien, -1 no tiene */
/* mu_cover_make usa ficheros temporales comunes (w.img, *.tmp2): un solo hilo a la vez
 * (el de Musica y el de la caratula de Reproduciendo sin biblioteca, nowui.inc.c) */
static pthread_mutex_t mu_make_mu = PTHREAD_MUTEX_INITIALIZER;
static int mu_cover_make_unlocked(const char *track, int cls, uint16_t *px, uint16_t *tmp);
static int mu_cover_make(const char *track, int cls, uint16_t *px, uint16_t *tmp)
{
    pthread_mutex_lock(&mu_make_mu);
    int r = mu_cover_make_unlocked(track, cls, px, tmp);
    pthread_mutex_unlock(&mu_make_mu);
    return r;
}
static int mu_cover_make_unlocked(const char *track, int cls, uint16_t *px, uint16_t *tmp)
{
    char dir[420], base[96], f[128], src[480];
    uint32_t k = mu_cover_key(track, dir, sizeof dir);
    snprintf(base, sizeof base, MU_CACHE "/%08x", k);
    static const char cc[3] = { 'm', 's', 'b' };
    snprintf(f, sizeof f, "%s_%c.raw", base, cc[cls]);
    if (!mu_read_raw(f, px, MU_CSZ[cls])) return 0;
    char none[128]; snprintf(none, sizeof none, "%s.none", base);
    if (!access(none, F_OK)) return -1;
    if (cls == MC_S) {                                /* de la de 140, si ya esta */
        char m[128]; snprintf(m, sizeof m, "%s_m.raw", base);
        if (!mu_read_raw(m, tmp, MU_GS)) { mu_scale(tmp, MU_GS, px, MU_SS); mu_write_raw(f, px, MU_SS); return 0; }
    }
    /* en el aparato suelen quedar 7-11 MB libres con el reproductor sonando: con el umbral
     * antiguo (12 MB) las caratulas de albumes aun sin cache (p. ej. las filas de una mezcla)
     * no se generaban nunca y salia siempre la nota. hibywall va limitado (ulimit -v). */
    int avail = mu_mem_avail_kb(), lim = avail - 4096; if (lim > 10240) lim = 10240;
    if (lim < 4096) return -2;                        /* poca memoria ahora: se reintentara (una sola decodificacion a la vez, nice 10) */
    mu_mkcache();
    int kind = mu_cover_src(track, dir, src, sizeof src);
    if (!kind) { int o = open(none, O_WRONLY | O_CREAT, 0644); if (o >= 0) close(o); return -1; }
    int s = cls == MC_B ? MU_BS : MU_GS;
    char out[140]; snprintf(out, sizeof out, "%s_%c.tmp2", base, cls == MC_B ? 'b' : 'm');
    uint16_t *dst = cls == MC_B ? px : tmp;
    int r = mu_wall(src, out, s, lim);
    if (kind == 1) unlink(src);
    if (r && kind == 1) {                             /* la incrustada no cabe: la de la carpeta */
        static const char *names[] = { "cover", "folder", "front", "Cover", "Folder", "Front", 0 };
        for (int i = 0; r && names[i]; i++) { snprintf(src, sizeof src, "%s/%s.jpg", dir, names[i]); if (!access(src, R_OK)) r = mu_wall(src, out, s, lim); }
    }
    if (r || mu_read_raw(out, dst, s)) {
        unlink(out);
        if (lim >= 10240) { int o = open(none, O_WRONLY | O_CREAT, 0644); if (o >= 0) close(o); }   /* demasiado grande: no reintentar */
        return lim >= 10240 ? -1 : -2;
    }
    char fin[128]; snprintf(fin, sizeof fin, "%s_%c.raw", base, cls == MC_B ? 'b' : 'm');
    rename(out, fin);
    if (mu_cache_bytes >= 0) mu_cache_bytes += s * s * 2;
    if (cls == MC_B) return 0;
    char fs[128]; snprintf(fs, sizeof fs, "%s_s.raw", base);
    if (cls == MC_M) { memcpy(px, tmp, MU_GS * MU_GS * 2); uint16_t sm[MU_SS * MU_SS]; mu_scale(tmp, MU_GS, sm, MU_SS); mu_write_raw(fs, sm, MU_SS); }
    else { mu_scale(tmp, MU_GS, px, MU_SS); mu_write_raw(fs, px, MU_SS); }
    return 0;
}
/* tamano de la cache (se mide una vez por sesion y se suma lo que se escribe) */
static void mu_cache_scan(void)
{
    long long tot = 0; DIR *d = opendir(MU_CACHE); struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[300]; snprintf(p, sizeof p, MU_CACHE "/%s", e->d_name); struct stat s;
        if (e->d_name[0] != '.' && !stat(p, &s)) tot += s.st_size;
    }
    if (d) closedir(d);
    if (tot > MU_CAP) {                                 /* pasada: fuera las grandes (se rehacen al abrir el album) */
        d = opendir(MU_CACHE);
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 6 && !strcmp(e->d_name + l - 6, "_b.raw")) { char p[300]; snprintf(p, sizeof p, MU_CACHE "/%s", e->d_name); if (!unlink(p)) tot -= MU_BS * MU_BS * 2; }
        }
        if (d) closedir(d);
    }
    mu_cache_bytes = tot;
}
static int mu_disk_ok(void)
{
    if (mu_cache_bytes < 0) mu_cache_scan();
    if (mu_cache_bytes > MU_CAP) return 0;
    struct statvfs v; if (statvfs(MU_SD, &v)) return 0;
    return (double)v.f_bavail * v.f_frsize > 256.0 * 1048576;
}
static void *mu_cover_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* hibywall hereda la prioridad baja */
    mu_cache_scan();
    for (;;) {
        pthread_mutex_lock(&mu_mu);
        struct mu_slot *best = 0; int pref = -1;
        for (;;) {
            best = 0;
            for (unsigned i = 0; i < sizeof mu_sl / sizeof *mu_sl; i++)
                if (mu_sl[i].state == 1 && (!best || mu_sl[i].use > best->use)) best = &mu_sl[i];
            if (best) break;
            /* en reposo y con la app abierta: adelantar las miniaturas del siguiente album */
            if (mu_open && ML && !mu_busy && now_ms() - mu_last_input > 1200 && mu_pref_i < ML->nA && mu_mem_avail_kb() > 12288) { pref = mu_pref_i++; break; }
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
            pthread_cond_timedwait(&mu_cv, &mu_mu, &ts);
        }
        char track[600] = ""; int cls = MC_M, alb = -1;
        if (best) { best->state = 2; cls = best->cls; alb = best->alb; }
        else alb = pref;
        if (ML && alb >= 0 && alb < ML->nA) mu_syspath(ML->T[ML->A[alb].first].path, track, sizeof track);
        pthread_mutex_unlock(&mu_mu);
        if (!best) {                                    /* adelantar: solo si falta y hay sitio */
            if (track[0] && mu_disk_ok()) {
                char dir[420], f[128]; uint32_t k = mu_cover_key(track, dir, sizeof dir);
                snprintf(f, sizeof f, MU_CACHE "/%08x_s.raw", k);
                if (access(f, F_OK)) { snprintf(f, sizeof f, MU_CACHE "/%08x.none", k);
                    if (access(f, F_OK)) { uint16_t *px = malloc(MU_GS * MU_GS * 2), *tmp = malloc(MU_GS * MU_GS * 2); if (px && tmp) mu_cover_make(track, MC_M, px, tmp); free(px); free(tmp); } }
            }
            continue;
        }
        int s = MU_CSZ[cls];
        uint16_t *px = malloc(s * s * 2), *tmp = malloc(MU_GS * MU_GS * 2);
        int r = track[0] && px && tmp ? mu_cover_make(track, cls, px, tmp) : -1;
        free(tmp);
        pthread_mutex_lock(&mu_mu);
        if (best->state == 2 && best->alb == alb && mu_open) {
            if (!r) { if (!best->px) best->px = malloc(s * s * 2); if (best->px) { memcpy(best->px, px, s * s * 2); best->state = 3; best->t_ready = now_ms(); } else best->state = 0; }
            else best->state = r == -2 ? 0 : 4;          /* -2: sin memoria ahora, se vuelve a pedir */
            mu_cov_new = 1;
        } else if (best->state == 2) best->state = 0;
        pthread_mutex_unlock(&mu_mu);
        free(px);
        if (r == -2) msleep(1000);
    }
    return 0;
}
static void mu_covers_free(void)
{
    pthread_mutex_lock(&mu_mu);
    for (unsigned i = 0; i < sizeof mu_sl / sizeof *mu_sl; i++) { if (mu_sl[i].state != 2) { free(mu_sl[i].px); mu_sl[i].px = 0; } mu_sl[i].state = mu_sl[i].state == 2 ? 2 : 0; }
    pthread_mutex_unlock(&mu_mu);
}

/* ------------------------------------------------------------------ dibujo basico */
static uint8_t *mu_ctab[MU_GS / 2 + 1];
static const uint8_t *mu_corner(int r)          /* cobertura del cuadrante de esquina (r x r) */
{
    if (r <= 0 || r > MU_GS / 2) return 0;
    if (mu_ctab[r]) return mu_ctab[r];
    uint8_t *t = malloc(r * r); if (!t) return 0;
    for (int y = 0; y < r; y++) for (int x = 0; x < r; x++) {
        float dx = r - (x + .5f), dy = r - (y + .5f), d = sqrtf(dx * dx + dy * dy);
        float c = r - d + .5f; if (c < 0) c = 0; if (c > 1) c = 1;
        t[y * r + x] = (uint8_t)(c * 255);
    }
    return mu_ctab[r] = t;
}
/* caratula s x s (nativa) en (x,y), esquinas de radio r, opacidad alpha */
static void mu_blit(uint16_t *d, const uint16_t *src, int s, int x, int y, int r, int alpha)
{
    const uint8_t *ct = mu_corner(r); if (!ct) r = 0;
    for (int yy = 0; yy < s; yy++) {
        int dy = y + yy; if (dy < 30 || dy >= H) continue;
        uint16_t *row = d + dy * W; const uint16_t *sr = src + yy * s;
        int cy = yy < r ? yy : yy >= s - r ? s - 1 - yy : -1;
        int xa = x < 0 ? -x : 0, xb = x + s > W ? W - x : s;
        if (cy < 0 && alpha >= 255) { if (xb > xa) memcpy(row + x + xa, sr + xa, (xb - xa) * 2); continue; }
        for (int xx = xa; xx < xb; xx++) {
            unsigned a = alpha;
            if (cy >= 0) { int cx = xx < r ? xx : xx >= s - r ? s - 1 - xx : -1; if (cx >= 0) a = a * ct[cy * r + cx] / 255; }
            if (!a) continue;
            row[x + xx] = a >= 255 ? sr[xx] : mix565(sr[xx], row[x + xx], a);
        }
    }
}
static void mu_note(uint16_t *d, int cx, int cy, int s, uint16_t col)   /* corchea */
{
    float k = s / 24.f;
    fill_circle(d, cx - 4 * k, cy + 6 * k, 4.2f * k, col, 255);
    fill_rrect(d, (int)(cx - 0.6f * k), (int)(cy - 9 * k), (int)(2.4f * k + 1), (int)(15 * k), 1, col, 255);
    fill_rrect(d, (int)(cx - 0.6f * k), (int)(cy - 9 * k), (int)(8 * k), (int)(3.2f * k + 1), 1.5f, col, 255);
}
/* hueco gris con la nota: se pinta una vez por tamano (fill_rrect hace una raiz por pixel) y
 * luego se copia como una caratula mas */
static struct { int s; uint16_t *px; } mu_ph[4];
static void mu_placeholder(uint16_t *d, int x, int y, int s, int r)
{
    int k = 0; while (k < 4 && mu_ph[k].px && mu_ph[k].s != s) k++;
    if (k == 4) k = 0;
    if (!mu_ph[k].px || mu_ph[k].s != s) {
        free(mu_ph[k].px); mu_ph[k].px = 0;
        if (s > H - 30 || !(mu_ph[k].px = malloc(s * s * 2))) { fill_rrect(d, x, y, s, s, (float)r, rgb(44, 44, 46), 255); return; }
        mu_ph[k].s = s;
        /* se dibuja en una zona de la pantalla de trabajo y se copia (las primitivas pintan sobre W x H) */
        uint16_t *t = malloc((30 + s) * W * 2); if (!t) { free(mu_ph[k].px); mu_ph[k].px = 0; return; }
        for (int yy = 0; yy < s; yy++) for (int xx = 0; xx < s; xx++) t[(30 + yy) * W + xx] = rgb(44, 44, 46);
        mu_note(t, s / 2 + s / 14, 30 + s / 2, s * 2 / 5, rgb(99, 99, 102));
        for (int yy = 0; yy < s; yy++) memcpy(mu_ph[k].px + yy * s, t + (30 + yy) * W, s * 2);
        free(t);
    }
    mu_blit(d, mu_ph[k].px, s, x, y, r, 255);
}
static int mu_fading;
static uint16_t *mu_up; static int mu_up_alb = -1;    /* 140 ampliada mientras llega la grande */
static double mu_page_t;                          /* lo que ya estaba al entrar en la pagina no se funde */
/* caratula del album (o su hueco) con fundido al llegar */
static void mu_album_art(uint16_t *d, int alb, int cls, int x, int y, int r)
{
    int s = MU_CSZ[cls];
    if (y + s < 30 || y >= H) return;
    double t; int none;
    const uint16_t *px = alb >= 0 ? mu_cover(alb, cls, &t, &none) : 0;
    if (px && t < mu_page_t) { mu_blit(d, px, s, x, y, r, 255); return; }
    int a = px ? (int)((now_ms() - t) * 255 / 220) : 0;
    if (a < 255) mu_placeholder(d, x, y, s, r);
    if (px) { if (a < 255) mu_fading = 1; mu_blit(d, px, s, x, y, r, a > 255 ? 255 : a); }
}
static void mu_text_bold(uint16_t *d, int x, int base, const char *s, int size, uint16_t col, int maxw)
{
    draw_text(d, x, base, s, size, col, maxw, 0, H);
    draw_text(d, x + 1, base, s, size, col, maxw, 0, H);
}
static void mu_chevron(uint16_t *d, int x, int cy)
{
    for (int i = 0; i < 6; i++) {
        int ya = cy - 6 + i, yb = cy + 6 - i;
        if (ya >= 30 && ya < H) { d[ya * W + x + i] = rgb(90, 90, 96); d[ya * W + x + 1 + i] = rgb(90, 90, 96); }
        if (yb >= 30 && yb < H) { d[yb * W + x + i] = rgb(90, 90, 96); d[yb * W + x + 1 + i] = rgb(90, 90, 96); }
    }
}
static void mu_hline(uint16_t *d, int x0, int y, uint16_t c) { if (y >= 30 && y < H) for (int x = x0; x < W; x++) d[y * W + x] = c; }
static void mu_tri(uint16_t *d, int x, int cy, int h, uint16_t col)    /* triangulo hacia la derecha */
{
    for (int yy = -h / 2; yy <= h / 2; yy++) {
        float w = (h * 0.9f) * (1 - fabsf((float)yy) / (h / 2.f + .5f));
        int dy = cy + yy; if (dy < 0 || dy >= H) continue;
        int xe = (int)w; float fr = w - xe;
        for (int xx = 0; xx < xe && x + xx < W; xx++) d[dy * W + x + xx] = col;
        if (x + xe < W && fr > 0) d[dy * W + x + xe] = mix565(col, d[dy * W + x + xe], (unsigned)(fr * 255));
    }
}
static void mu_line(uint16_t *d, float x0, float y0, float x1, float y1, float th, uint16_t col)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy); int n = (int)len + 1;
    for (int i = 0; i <= n; i++) fill_circle(d, x0 + dx * i / n, y0 + dy * i / n, th / 2, col, 255);
}
static void mu_shuffle_icon(uint16_t *d, int x, int cy, uint16_t col)
{
    mu_line(d, x, cy - 5, x + 6, cy - 5, 2, col); mu_line(d, x + 6, cy - 5, x + 12, cy + 5, 2, col); mu_line(d, x + 12, cy + 5, x + 16, cy + 5, 2, col);
    mu_line(d, x, cy + 5, x + 6, cy + 5, 2, col); mu_line(d, x + 6, cy + 5, x + 12, cy - 5, 2, col); mu_line(d, x + 12, cy - 5, x + 16, cy - 5, 2, col);
    mu_line(d, x + 14, cy - 8, x + 17, cy - 5, 2, col); mu_line(d, x + 14, cy - 2, x + 17, cy - 5, 2, col);
    mu_line(d, x + 14, cy + 2, x + 17, cy + 5, 2, col); mu_line(d, x + 14, cy + 8, x + 17, cy + 5, 2, col);
}
static void mu_spinner(uint16_t *d, int cx, int cy)   /* indicador de actividad de iOS */
{
    int ph = (int)(now_ms() / 83) % 8;
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 6.2831853f / 8;
        int k = (ph - i + 8) % 8;                       /* 0 = la mas clara */
        mu_line(d, cx + cosf(a) * 5, cy + sinf(a) * 5, cx + cosf(a) * 9.5f, cy + sinf(a) * 9.5f, 2.4f, dim565(rgb(200, 200, 205), 32 - k * 3));
    }
}

/* ------------------------------------------------------------------ paginas */
enum { PG_LIB, PG_ALBUMS, PG_ARTISTS, PG_ARTIST, PG_ALBUM, PG_SONGS, PG_GENRES, PG_LIST, PG_PLAYLISTS, PG_MIXES, PG_SEARCH };
enum { IT_TITLE, IT_MENU, IT_SECTION, IT_GRID, IT_SONG, IT_ARTIST, IT_HERO, IT_BUTTONS, IT_TRACK, IT_DISC, IT_FOOTER, IT_TEXT, IT_PLAYLIST, IT_MIX, IT_EMPTY, IT_SEARCH, IT_ALBUMROW };
enum { M_PLAYLISTS, M_ARTISTS, M_ALBUMS, M_SONGS, M_GENRES, M_MIXES, M_HIBY };
struct mit { int y; short type, h; int a, b; const char *s; };
struct mpl { char kind[4]; char arg[300]; char name[128]; int n; };
struct mu_job { char *argv[8]; char a[4][420]; char *out; int len; volatile int done, refs; };
struct mpg {
    int kind, arg; float scroll; char title[160];
    struct mit *it; int n, cap, ch;
    int *tl, ntl, tl_own;              /* pistas de la pagina (para reproducir desde un indice) */
    struct mpl *pl; int npl;
    struct mu_job *job; int jkind;     /* 1 listas, 2 pistas de una lista, 3 mezcla */
    const char *empty;                 /* mensaje si no hay nada */
};
#define MU_DEPTH 10
static struct mpg *mu_st[MU_DEPTH]; static int mu_sp;
static char mu_q[128], mu_qlast[128] = "\x01";
static int mu_editing;
static int mu_press = -1, mu_press_cell, mu_spin_on;
static int mu_pend_id = -1, mu_pend_row = -1, mu_pend_cell; static double mu_pend_t;
static struct mpg *mu_pend_pg;
static struct np_info mu_np; static uint16_t mu_npcov[128 * 128]; static int mu_npcov_ok, mu_np_trk = -1;

static void it_add(struct mpg *p, int type, int h, int a, int b, const char *s)
{
    if (p->n >= p->cap) { int nc = p->cap ? p->cap * 2 : 64; struct mit *ni = realloc(p->it, nc * sizeof *ni); if (!ni) return; p->it = ni; p->cap = nc; }
    struct mit *m = &p->it[p->n++]; m->y = p->ch; m->type = (short)type; m->h = (short)h; m->a = a; m->b = b; m->s = s;
    p->ch += h;
}
static void mu_job_release(struct mu_job *j) { if (j && __sync_sub_and_fetch(&j->refs, 1) == 0) { free(j->out); free(j); } }
static void mu_page_free(struct mpg *p)
{
    if (!p) return;
    if (p == mu_pend_pg) mu_pend_pg = 0;
    free(p->it); if (p->tl_own) free(p->tl); free(p->pl);
    mu_job_release(p->job);
    free(p);
}
static void *mu_job_thread(void *a)
{
    struct mu_job *j = a;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
    j->len = mu_capture(j->argv, &j->out);
    __sync_synchronize(); j->done = 1; mu_dirty = 1;
    mu_job_release(j);
    return 0;
}
static void mu_job_start(struct mpg *p, int kind, const char *a0, const char *a1, const char *a2, const char *a3)
{
    struct mu_job *j = calloc(1, sizeof *j); if (!j) return;
    const char *src[4] = { a0, a1, a2, a3 }; int n = 0;
    j->argv[n++] = MU_BIN;
    for (int i = 0; i < 4 && src[i]; i++) { snprintf(j->a[i], sizeof j->a[i], "%s", src[i]); j->argv[n++] = j->a[i]; }
    j->argv[n++] = "--lang"; j->argv[n++] = (char *)(lang3() == LANG_ES ? "es" : lang3() == LANG_FR ? "fr" : "en"); j->argv[n] = 0;
    j->refs = 2; p->job = j; p->jkind = kind;
    pthread_t th;
    if (pthread_create(&th, 0, mu_job_thread, j)) { j->done = 1; j->len = -1; j->refs = 1; return; }
    pthread_detach(th);
}

/* --------- construccion de las paginas */
static void mu_add_grid(struct mpg *p, const int *albs, int n)
{
    for (int i = 0; i < n; i += 2) it_add(p, IT_GRID, MU_GS + 50, albs[i], i + 1 < n ? albs[i + 1] : -1, 0);
}
static void mu_add_songs(struct mpg *p)
{
    if (!p->ntl) { it_add(p, IT_EMPTY, 160, 0, 0, p->empty ? p->empty : tr("Lista vacía")); return; }
    it_add(p, IT_BUTTONS, 64, 0, 0, 0);
    for (int i = 0; i < p->ntl; i++) it_add(p, IT_SONG, 60, p->tl[i], i, 0);
}
static void mu_search_items(struct mpg *p);
static void mu_build(struct mpg *p)
{
    p->n = 0; p->ch = 0;
    struct mu_lib *L = ML;
    if (p->kind == PG_SEARCH) { mu_search_items(p); return; }
    if (p->kind == PG_LIB) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        static const char *const names[] = { "Listas", "Artistas", "Álbumes", "Canciones", "Géneros", "Mezclas", "Biblioteca de HiBy" };
        for (int i = 0; i < 7; i++) it_add(p, IT_MENU, 46, i, i == 6, names[i]);
        it_add(p, IT_SECTION, 50, 0, 0, tr("Añadido recientemente"));
        if (!L) { it_add(p, IT_EMPTY, 120, mu_lib_state == 3 ? 0 : 1, 0, mu_lib_state == 3 ? tr("No se pudo leer la biblioteca") : 0); }
        else if (!L->nA) it_add(p, IT_EMPTY, 120, 0, 0, tr("No hay música en la biblioteca"));
        else mu_add_grid(p, L->recent, L->nA < 12 ? L->nA : 12);
    } else if (!L && p->kind != PG_MIXES) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        it_add(p, IT_EMPTY, 160, 1, 0, 0);
    } else if (p->kind == PG_ALBUMS) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        mu_add_grid(p, L->albs, L->nA);
    } else if (p->kind == PG_ARTISTS) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        for (int i = 0; i < L->nR; i++) it_add(p, IT_ARTIST, 60, i, 0, 0);
    } else if (p->kind == PG_ARTIST) {
        const struct mu_art *r = &L->R[p->arg];
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        if (!p->tl) {
            int n = 0; for (int i = 0; i < r->n; i++) n += L->A[L->artalb[r->first + i]].n;
            p->tl = malloc((n + 1) * sizeof(int)); p->tl_own = 1; p->ntl = 0;
            for (int i = 0; p->tl && i < r->n; i++) { const struct mu_alb *a = &L->A[L->artalb[r->first + i]]; for (int k = 0; k < a->n; k++) p->tl[p->ntl++] = a->first + k; }
        }
        it_add(p, IT_BUTTONS, 64, 0, 0, 0);
        it_add(p, IT_SECTION, 50, 0, 0, tr("Álbumes"));
        mu_add_grid(p, L->artalb + r->first, r->n);
    } else if (p->kind == PG_ALBUM) {
        const struct mu_alb *a = &L->A[p->arg];
        if (!p->tl) { p->tl = malloc((a->n + 1) * sizeof(int)); p->tl_own = 1; p->ntl = 0; for (int k = 0; p->tl && k < a->n; k++) p->tl[p->ntl++] = a->first + k; }
        it_add(p, IT_HERO, 8 + MU_BS + 84, p->arg, 0, 0);
        it_add(p, IT_BUTTONS, 64, 0, 0, 0);
        int multi = a->n && L->T[a->first].disc != L->T[a->first + a->n - 1].disc, last = -999;
        for (int k = 0; k < a->n; k++) {
            const struct mu_trk *t = &L->T[a->first + k];
            if (multi && t->disc != last) { it_add(p, IT_DISC, 40, t->disc, 0, 0); last = t->disc; }
            it_add(p, IT_TRACK, 46, a->first + k, k, 0);
        }
        it_add(p, IT_FOOTER, 64, p->arg, 0, 0);
    } else if (p->kind == PG_SONGS) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        p->tl = L->songs; p->ntl = L->nT; p->tl_own = 0;
        mu_add_songs(p);
    } else if (p->kind == PG_GENRES) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        if (!L->ng) it_add(p, IT_EMPTY, 160, 0, 0, tr("Las canciones no tienen género"));
        for (int i = 0; i < L->ng; i++) it_add(p, IT_TEXT, 46, i, 0, L->gen[i]);
    } else if (p->kind == PG_PLAYLISTS) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        if (!p->pl) it_add(p, IT_EMPTY, 160, p->job ? 1 : 0, 0, p->job ? 0 : tr("No hay listas"));
        for (int i = 0; i < p->npl; i++) it_add(p, IT_PLAYLIST, 64, i, 0, 0);
    } else if (p->kind == PG_MIXES) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        for (int i = 0; i < 6; i++) if (i < 5 || (mu_np.source && mu_np.path[0])) it_add(p, IT_MIX, 76, i, 0, 0);
    } else if (p->kind == PG_LIST) {
        it_add(p, IT_TITLE, 52, 0, 0, p->title);
        if (p->job && !p->tl) it_add(p, IT_EMPTY, p->jkind == 3 ? 260 : 160, p->jkind == 3 ? 2 : 1, 0, p->jkind == 3 ? tr("Creando mezcla…") : 0);
        else mu_add_songs(p);
    }
    it_add(p, IT_EMPTY, 16, 0, 0, "");                /* margen final */
}
static struct mpg *mu_page_new(int kind, int arg, const char *title)
{
    struct mpg *p = calloc(1, sizeof *p); if (!p) return 0;
    p->kind = kind; p->arg = arg; snprintf(p->title, sizeof p->title, "%s", title ? title : "");
    struct mu_lib *L = ML;
    if (kind == PG_LIST && arg >= 0 && L && !p->job) {   /* genero: canciones de sus albumes */
        int n = 0; for (int i = 0; i < L->nA; i++) if (!strcasecmp(L->A[i].genre, L->gen[arg])) n += L->A[i].n;
        p->tl = malloc((n + 1) * sizeof(int)); p->tl_own = 1;
        for (int k = 0; p->tl && k < L->nA; k++) { const struct mu_alb *a = &L->A[L->albs[k]]; if (strcasecmp(a->genre, L->gen[arg])) continue; for (int j = 0; j < a->n; j++) p->tl[p->ntl++] = a->first + j; }
    }
    if (kind == PG_PLAYLISTS) mu_job_start(p, 1, "playlists", 0, 0, 0);
    mu_build(p);
    return p;
}
/* resultado de un trabajo de hibymix: lista de rutas (primer campo) -> pistas */
static void mu_job_done(struct mpg *p)
{
    struct mu_job *j = p->job; if (!j || !j->done) return;
    if (p->jkind == 1) {
        int n = 0; for (char *c = j->out; c && *c; c++) if (*c == '\n') n++;
        p->pl = calloc(n + 1, sizeof *p->pl); p->npl = 0;
        for (char *l = j->out, *nl; p->pl && l && *l; l = nl + 1) {
            nl = strchr(l, '\n'); if (nl) *nl = 0;
            char *f[6]; int k = mu_split(l, f, 6);
            if (k >= 5 && f[0][0] == 'P') {
                struct mpl *e = &p->pl[p->npl++];
                snprintf(e->kind, sizeof e->kind, "%s", f[1]); snprintf(e->arg, sizeof e->arg, "%s", f[2]);
                snprintf(e->name, sizeof e->name, "%s", f[3]); e->n = atoi(f[4]);
            }
            if (!nl) break;
        }
    } else if (ML) {
        int n = 0; for (char *c = j->out; c && *c; c++) if (*c == '\n') n++;
        p->tl = malloc((n + 2) * sizeof(int)); p->tl_own = 1; p->ntl = 0;
        for (char *l = j->out, *nl; p->tl && l && *l; l = nl + 1) {
            nl = strchr(l, '\n'); if (nl) *nl = 0;
            char *tab = strchr(l, '\t'); if (tab) *tab = 0;
            int t = mu_find_path(ML, l); if (t >= 0) p->tl[p->ntl++] = t;
            if (!nl) break;
        }
        if (!p->ntl) {
            p->empty = p->jkind == 3 ? tr("No se pudo crear la mezcla") : tr("Lista vacía");
            /* la causa real, para no dejar un "no se pudo" sin explicacion */
            if (access(MU_BIN, X_OK)) p->empty = tr("Falta el componente de mezclas (hibymix)");
            else if (access("/usr/data/mnt/sd_0/.", F_OK) || !mu_sd_mounted()) p->empty = tr("No hay tarjeta SD");
            else if (j->len < 0) p->empty = tr("No se pudo leer la biblioteca de HiBy");
            lg("musica: mezcla vacia (len %d, salida %.80s)", j->len, j->out ? j->out : "");
        }
        if (!p->tl) { p->tl = malloc(sizeof(int)); p->ntl = 0; }
    }
    mu_job_release(j); p->job = 0;
    mu_build(p);
}

/* --------- busqueda (en memoria: sin base de datos) */
static int mu_sr_tracks[60], mu_sn_tracks;
static void mu_search_items(struct mpg *p)
{
    it_add(p, IT_SEARCH, 52, 0, 0, 0);
    struct mu_lib *L = ML;
    char q[128]; mu_fold(mu_q, q, sizeof q);
    char *s = q; while (*s == ' ') s++;
    int ql = (int)strlen(s); while (ql > 0 && s[ql - 1] == ' ') s[--ql] = 0;
    p->tl = mu_sr_tracks; p->ntl = 0; p->tl_own = 0;
    if (!L || !ql) { if (!L) it_add(p, IT_EMPTY, 120, 1, 0, 0); return; }
    int na = 0;
    for (int i = 0; i < L->nR && na < 3; i++) if (strstr(L->R[i].fk, s)) { if (!na) it_add(p, IT_SECTION, 46, 0, 0, tr("Artistas")); it_add(p, IT_ARTIST, 60, i, 0, 0); na++; }
    int nb = 0;
    for (int k = 0; k < L->nA && nb < 5; k++) { int i = L->albs[k]; if (strstr(L->A[i].fk, s) || (!na && strstr(L->A[i].afk, s))) { if (!nb) it_add(p, IT_SECTION, 46, 0, 0, tr("Álbumes")); it_add(p, IT_ALBUMROW, 60, i, 0, 0); nb++; } }
    for (int k = 0; k < L->nT && p->ntl < 60; k++) { int i = L->songs[k]; if (strstr(L->T[i].fk, s)) mu_sr_tracks[p->ntl++] = i; }
    if (p->ntl) { it_add(p, IT_SECTION, 46, 0, 0, tr("Canciones")); for (int i = 0; i < p->ntl; i++) it_add(p, IT_SONG, 60, p->tl[i], i, 0); }
    if (!na && !nb && !p->ntl) it_add(p, IT_EMPTY, 120, 0, 0, tr("Sin resultados"));
    it_add(p, IT_EMPTY, 16, 0, 0, "");
}

/* --------- dibujo de las filas */

static void mu_draw_item(uint16_t *d, struct mpg *p, int idx, int y)
{
    const struct mit *m = &p->it[idx];
    struct mu_lib *L = ML;
    int pressed = idx == mu_press;
    if (pressed && m->type != IT_GRID && m->type != IT_BUTTONS && m->type != IT_SEARCH && m->type != IT_HERO && m->type != IT_MIX)
        for (int yy = y; yy < y + m->h; yy++) if (yy >= 30 && yy < H) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(44, 44, 46);
    switch (m->type) {
    case IT_TITLE: mu_text_bold(d, 16, y + 40, m->s, 28, 0xFFFF, W - 32); break;
    case IT_SECTION: mu_text_bold(d, 16, y + m->h - 14, m->s, 20, 0xFFFF, W - 32); break;
    case IT_MENU: {
        int cy = y + 23, x = 18;
        uint16_t c = m->b ? MU_GRAY : MU_ACC;
        switch (m->a) {
        case M_PLAYLISTS: for (int i = 0; i < 3; i++) fill_rrect(d, x, cy - 7 + i * 6, i == 2 ? 9 : 14, 2, 1, c, 255); mu_note(d, x + 17, cy + 1, 13, c); break;
        case M_ARTISTS: fill_rrect(d, x + 6, cy - 9, 9, 13, 4.5f, c, 255); fill_rrect(d, x + 9, cy + 4, 3, 7, 1, c, 255); fill_rrect(d, x + 5, cy + 9, 11, 2, 1, c, 255); break;
        case M_ALBUMS: fill_rrect(d, x, cy - 9, 20, 18, 3, c, 255); fill_rrect(d, x + 2, cy - 7, 16, 14, 2, 0, 255); fill_circle(d, x + 10, cy, 4, c, 255); break;
        case M_SONGS: mu_note(d, x + 11, cy, 20, c); break;
        case M_GENRES: for (int i = 0; i < 4; i++) fill_rrect(d, x + (i % 2) * 11, cy - 9 + (i / 2) * 10, 8, 8, 2, c, 255); break;
        case M_MIXES: fill_circle(d, x + 10, cy, 9.5f, c, 255); fill_circle(d, x + 10, cy, 7.5f, 0, 255); fill_circle(d, x + 10, cy, 5, c, 255); fill_circle(d, x + 10, cy, 3, 0, 255); fill_circle(d, x + 10, cy, 1.6f, c, 255); break;
        case M_HIBY: fill_rrect(d, x, cy - 10, 20, 20, 5, rgb(72, 72, 76), 255); draw_text(d, x + 5, cy + 6, "H", 15, 0xFFFF, 0, 0, H); break;
        }
        draw_text(d, 54, y + 30, tr(m->s), 17, m->b ? rgb(200, 200, 205) : 0xFFFF, W - 90, 0, H);
        mu_chevron(d, W - 24, cy);
        mu_hline(d, 54, y + m->h - 1, MU_SEP);
        break; }
    case IT_GRID:
        for (int c = 0; c < 2; c++) {
            int a = c ? m->b : m->a; if (a < 0 || !L) continue;
            int x = c ? 166 : 14;
            mu_album_art(d, a, MC_M, x, y, 6);
            if (pressed && mu_press_cell == c) fill_rrect(d, x, y, MU_GS, MU_GS, 6, 0, 110);
            draw_text(d, x, y + MU_GS + 18, L->A[a].title, 13, 0xFFFF, MU_GS, 0, H);
            draw_text(d, x, y + MU_GS + 35, L->A[a].artist, 13, MU_GRAY, MU_GS, 0, H);
        }
        break;
    case IT_SONG: {
        if (!L) break;
        const struct mu_trk *t = &L->T[m->a];
        mu_album_art(d, t->alb, MC_S, 16, y + 6, 4);
        int now = m->a == mu_np_trk;
        draw_text(d, 76, y + 27, t->title, 15, now ? MU_ACC : 0xFFFF, W - 76 - 16, 0, H);
        char sub[300];
        if (p->kind == PG_SEARCH) snprintf(sub, sizeof sub, tr("Canción · %s"), t->artist); else snprintf(sub, sizeof sub, "%s", t->artist);
        draw_text(d, 76, y + 46, sub, 13, MU_GRAY, W - 76 - 16, 0, H);
        mu_hline(d, 76, y + m->h - 1, MU_SEP);
        break; }
    case IT_ALBUMROW: {
        if (!L) break;
        const struct mu_alb *a = &L->A[m->a];
        mu_album_art(d, m->a, MC_S, 16, y + 6, 4);
        draw_text(d, 76, y + 27, a->title, 15, 0xFFFF, W - 76 - 34, 0, H);
        char sub[300]; snprintf(sub, sizeof sub, tr("Álbum · %s"), a->artist);
        draw_text(d, 76, y + 46, sub, 13, MU_GRAY, W - 76 - 34, 0, H);
        mu_chevron(d, W - 24, y + 30);
        mu_hline(d, 76, y + m->h - 1, MU_SEP);
        break; }
    case IT_ARTIST: {
        if (!L) break;
        const struct mu_art *r = &L->R[m->a];
        int alb = L->artalb[r->first];
        mu_album_art(d, alb, MC_S, 16, y + 6, MU_SS / 2);
        draw_text(d, 76, y + 36, r->name, 16, 0xFFFF, W - 76 - 34, 0, H);
        mu_chevron(d, W - 24, y + 30);
        mu_hline(d, 76, y + m->h - 1, MU_SEP);
        break; }
    case IT_HERO: {
        if (!L) break;
        const struct mu_alb *a = &L->A[m->a];
        int x = (W - MU_BS) / 2, cy = y + 8;
        double t; int none;
        const uint16_t *big = mu_cover(m->a, MC_B, &t, &none);
        const uint16_t *mid = mu_cover(m->a, MC_M, &t, &none);
        /* mientras llega la grande, la de 140 ampliada (suave); luego se funde la nitida */
        uint16_t *up = mu_up; int up_alb = mu_up_alb;
        if (!big && mid) {
            if (!up) up = mu_up = malloc(MU_BS * MU_BS * 2);
            if (up && up_alb != m->a) {
                for (int yy = 0; yy < MU_BS; yy++) for (int xx = 0; xx < MU_BS; xx++) {
                    float sx = (xx + .5f) * MU_GS / MU_BS - .5f, sy = (yy + .5f) * MU_GS / MU_BS - .5f;
                    int x0 = sx < 0 ? 0 : (int)sx, y0 = sy < 0 ? 0 : (int)sy, x1 = x0 + 1 < MU_GS ? x0 + 1 : x0, y1 = y0 + 1 < MU_GS ? y0 + 1 : y0;
                    unsigned fx = (unsigned)((sx - x0) * 255), fy = (unsigned)((sy - y0) * 255); if (sx < 0) fx = 0; if (sy < 0) fy = 0;
                    uint16_t a0 = mix565(mid[y0 * MU_GS + x1], mid[y0 * MU_GS + x0], fx), a1 = mix565(mid[y1 * MU_GS + x1], mid[y1 * MU_GS + x0], fx);
                    up[yy * MU_BS + xx] = mix565(a1, a0, fy);
                }
                up_alb = mu_up_alb = m->a;
            }
        }
        if (big) {
            double tb; int nb; mu_cover(m->a, MC_B, &tb, &nb);
            int al = tb < mu_page_t ? 255 : (int)((now_ms() - tb) * 255 / 260);
            if (al < 255) { if (up && up_alb == m->a) mu_blit(d, up, MU_BS, x, cy, 8, 255); else mu_placeholder(d, x, cy, MU_BS, 8); mu_fading = 1; }
            mu_blit(d, big, MU_BS, x, cy, 8, al > 255 ? 255 : al);
        } else if (up && up_alb == m->a && mid) mu_blit(d, up, MU_BS, x, cy, 8, 255);
        else mu_placeholder(d, x, cy, MU_BS, 8);
        if (!big && none) { /* sin caratula: el hueco se queda */ }
        int ty = cy + MU_BS + 30;
        int tw = tw_n(a->title, 19, -1); if (tw > W - 32) tw = W - 32;
        mu_text_bold(d, (W - tw) / 2, ty, a->title, 19, 0xFFFF, W - 32);
        tw = tw_n(a->artist, 17, -1); if (tw > W - 32) tw = W - 32;
        draw_text(d, (W - tw) / 2, ty + 25, a->artist, 17, MU_ACC, W - 32, 0, H);
        char meta[200];
        if (a->genre[0] && a->year) snprintf(meta, sizeof meta, "%s · %d", a->genre, a->year);
        else if (a->year) snprintf(meta, sizeof meta, "%d", a->year); else snprintf(meta, sizeof meta, "%s", a->genre);
        tw = tw_n(meta, 12, -1); if (tw > W - 32) tw = W - 32;
        if (meta[0]) draw_text(d, (W - tw) / 2, ty + 45, meta, 12, MU_GRAY, W - 32, 0, H);
        break; }
    case IT_BUTTONS: {
        for (int c = 0; c < 2; c++) {
            int x = c ? 166 : 16, bw = 138, by = y + 10;
            fill_rrect(d, x, by, bw, 44, 10, pressed && mu_press_cell == c ? rgb(58, 58, 60) : rgb(28, 28, 30), 255);
            const char *l = c ? tr("Aleatorio") : tr("Reproducir");
            int lw = tw_n(l, 16, -1); if (lw > bw - 44) lw = bw - 44;
            int ix = x + (bw - lw - 26) / 2;
            if (p == mu_pend_pg && idx == mu_pend_row && mu_pend_cell == c) { mu_spinner(d, ix + 8, by + 22); mu_spin_on = 1; }
            else if (c) mu_shuffle_icon(d, ix - 2, by + 22, MU_ACC); else mu_tri(d, ix + 2, by + 22, 14, MU_ACC);
            draw_text(d, ix + 24, by + 28, l, 16, MU_ACC, bw - 44, 0, H);
            draw_text(d, ix + 25, by + 28, l, 16, MU_ACC, bw - 44, 0, H);
        }
        break; }
    case IT_DISC: { char t[48]; snprintf(t, sizeof t, tr("Disco %d"), m->a); draw_text(d, 16, y + 28, t, 14, MU_GRAY, 0, 0, H); mu_hline(d, 16, y + m->h - 1, MU_SEP); break; }
    case IT_TRACK: {
        if (!L) break;
        const struct mu_trk *t = &L->T[m->a];
        const struct mu_alb *a = &L->A[t->alb];
        int now = m->a == mu_np_trk;
        if (now) { for (int i = 0; i < 3; i++) { int hh = i == 1 ? 12 : 7 + i; fill_rrect(d, 18 + i * 5, y + 30 - hh, 3, hh, 1, MU_ACC, 255); } }
        else { char n[12]; snprintf(n, sizeof n, "%d", t->num > 0 ? t->num : m->b + 1); int w = tw_n(n, 15, -1); draw_text(d, 36 - w, y + 29, n, 15, MU_GRAY, 0, 0, H); }
        int other = strcasecmp(t->artist, a->artist) != 0;
        char du[16] = ""; if (t->dur > 0) snprintf(du, sizeof du, "%d:%02d", t->dur / 60, t->dur % 60);
        int dw = tw_n(du, 13, -1);
        draw_text(d, 52, y + (other ? 22 : 29), t->title, 15, now ? MU_ACC : 0xFFFF, W - 52 - 24 - dw, 0, H);
        if (other) draw_text(d, 52, y + 39, t->artist, 12, MU_GRAY, W - 52 - 24 - dw, 0, H);
        if (du[0]) draw_text(d, W - 16 - dw, y + 29, du, 13, MU_GRAY, 0, 0, H);
        mu_hline(d, 52, y + m->h - 1, MU_SEP);
        break; }
    case IT_FOOTER: {
        if (!L) break;
        const struct mu_alb *a = &L->A[m->a]; char t[96];
        if (a->n == 1) snprintf(t, sizeof t, tr("1 canción, %d min"), a->mins); else snprintf(t, sizeof t, tr("%d canciones, %d min"), a->n, a->mins);
        draw_text(d, 16, y + 28, t, 13, MU_GRAY, 0, 0, H);
        break; }
    case IT_TEXT:
        draw_text(d, 16, y + 30, m->s, 17, 0xFFFF, W - 56, 0, H);
        mu_chevron(d, W - 24, y + 23); mu_hline(d, 16, y + m->h - 1, MU_SEP);
        break;
    case IT_PLAYLIST: {
        const struct mpl *e = &p->pl[m->a];
        int fav = !strcmp(e->kind, "fav");
        fill_rrect(d, 16, y + 8, 48, 48, 6, fav ? rgb(255, 55, 95) : rgb(58, 58, 64), 255);
        if (fav) { fill_circle(d, 34, y + 28, 6, 0xFFFF, 255); fill_circle(d, 46, y + 28, 6, 0xFFFF, 255); for (int i = 0; i < 9; i++) fill_rrect(d, 29 + i, y + 30 + i, 18 - 2 * i, 2, 1, 0xFFFF, 255); }
        else mu_note(d, 41, y + 32, 22, rgb(200, 200, 206));
        draw_text(d, 76, y + (e->n ? 29 : 37), e->name, 16, 0xFFFF, W - 76 - 34, 0, H);
        if (e->n) { char t[48]; snprintf(t, sizeof t, e->n == 1 ? tr("1 canción") : tr("%d canciones"), e->n); draw_text(d, 76, y + 48, t, 13, MU_GRAY, 0, 0, H); }
        mu_chevron(d, W - 24, y + 32); mu_hline(d, 76, y + m->h - 1, MU_SEP);
        break; }
    case IT_MIX: {
        static const uint16_t c0[6] = { 0xF8A9, 0x6A1F, 0xFB20, 0x2E9F, 0x9C1F, 0xF9C8 };
        static const char *const nm[6] = { "Mezcla del día", "Descubre", "Tus favoritos", "Recién llegadas", "Tu década", "Radio de lo que suena" };
        static const char *const ds[6] = { "Lo de siempre y algo nuevo, cada día", "Artistas que no escuchas últimamente", "Lo que más escuchas", "Lo último que has añadido", "La década que más escuchas", "Canciones parecidas a la que suena" };
        uint16_t c = c0[m->a];
        fill_rrect(d, 16, y + 8, 60, 60, 8, c, 255);
        fill_rrect(d, 16, y + 38, 60, 30, 8, dim565(c, 20), 120);
        if (m->a == 5) { fill_circle(d, 46, y + 38, 13, 0xFFFF, 255); fill_circle(d, 46, y + 38, 10.5f, c, 255); fill_circle(d, 46, y + 38, 7, 0xFFFF, 255); fill_circle(d, 46, y + 38, 4.5f, c, 255); fill_circle(d, 46, y + 38, 2.2f, 0xFFFF, 255); }
        else mu_note(d, 48, y + 38, 26, 0xFFFF);
        if (pressed) fill_rrect(d, 16, y + 8, 60, 60, 8, 0, 100);
        draw_text(d, 90, y + 34, tr(nm[m->a]), 16, 0xFFFF, W - 90 - 34, 0, H);
        draw_text(d, 90, y + 53, tr(ds[m->a]), 12, MU_GRAY, W - 90 - 34, 0, H);
        mu_chevron(d, W - 24, y + 38); mu_hline(d, 90, y + m->h - 1, MU_SEP);
        break; }
    case IT_SEARCH: {
        int fx = 16, fw = W - 32 - (mu_editing ? 76 : 0);
        fill_rrect(d, fx, y + 8, fw, 36, 10, rgb(28, 28, 30), 255);
        fill_circle(d, fx + 17, y + 24, 6, MU_GRAY, 255); fill_circle(d, fx + 17, y + 24, 4.2f, rgb(28, 28, 30), 255); mu_line(d, fx + 21, y + 28, fx + 25, y + 32, 2.2f, MU_GRAY);
        if (mu_q[0]) {
            int w = draw_text(d, fx + 34, y + 32, mu_q, 16, 0xFFFF, fw - 44, 0, H);
            if (mu_editing) { for (int yy = y + 15; yy < y + 37; yy++) for (int xx = fx + 35 + w; xx < fx + 37 + w && xx < W; xx++) d[yy * W + xx] = MU_ACC; kb_caret(fx + 35 + w, y + 15, y + 37); }
        } else {
            draw_text(d, fx + 34, y + 32, tr("Artistas, álbumes y canciones"), 16, rgb(110, 110, 116), fw - 44, 0, H);
            if (mu_editing) { for (int yy = y + 15; yy < y + 37; yy++) for (int xx = fx + 33; xx < fx + 35; xx++) d[yy * W + xx] = MU_ACC; kb_caret(fx + 33, y + 15, y + 37); }
        }
        if (mu_editing) draw_text(d, W - 16 - tw_n(tr("Cancelar"), 16, -1), y + 32, tr("Cancelar"), 16, MU_ACC, 0, 0, H);
        break; }
    case IT_EMPTY:
        if (m->a == 2) {   /* generando una mezcla: portada de degradado que gira y late (como iOS) */
            double t = now_ms() / 1000.0; int s = 132, cx = W / 2, cy = y + m->h / 2 - 22, x0 = cx - s / 2, y0 = cy - s / 2;
            float pulse = 1 + 0.035f * sinf((float)(t * 3.2));
            int ss = (int)(s * pulse); x0 = cx - ss / 2; y0 = cy - ss / 2;
            float ca = cosf((float)t * 0.9f), sa = sinf((float)t * 0.9f);
            for (int yy = 0; yy < ss; yy++) {
                int Y = y0 + yy; if (Y < 30 || Y >= H) continue;
                for (int xx = 0; xx < ss; xx++) {
                    /* esquinas redondeadas (r = 22) */
                    int dx = xx < 22 ? 22 - xx : xx > ss - 23 ? xx - (ss - 23) : 0, dy = yy < 22 ? 22 - yy : yy > ss - 23 ? yy - (ss - 23) : 0;
                    if (dx * dx + dy * dy > 22 * 22) continue;
                    float u = (xx - ss / 2.f) / ss, v = (yy - ss / 2.f) / ss, k = u * ca + v * sa + 0.5f;
                    if (k < 0) k = 0; if (k > 1) k = 1;
                    int r = (int)(255 * (1 - k) + 94 * k), g = (int)(45 * (1 - k) + 92 * k), bl = (int)(85 * (1 - k) + 230 * k);
                    d[Y * W + x0 + xx] = rgb(r, g, bl);
                }
            }
            mu_note(d, cx + ss / 14, cy, ss * 2 / 5, 0xFFFF);
            mu_spinner(d, W / 2, y0 + ss + 26); mu_spin_on = 1;
            if (m->s && m->s[0]) { int w = tw_n(m->s, 15, -1); if (w > W - 40) w = W - 40; draw_text(d, (W - w) / 2, y0 + ss + 44, m->s, 15, MU_GRAY, W - 40, 0, H); }
            break;
        }
        if (m->a) { mu_spinner(d, W / 2, y + m->h / 2 - (m->s ? 12 : 0)); mu_spin_on = 1; }
        if (m->s && m->s[0]) { int w = tw_n(m->s, 15, -1); if (w > W - 40) w = W - 40; draw_text(d, (W - w) / 2, y + m->h / 2 + (m->a ? 22 : 5), m->s, 15, MU_GRAY, W - 40, 0, H); }
        break;
    }
    if ((m->type == IT_SONG || m->type == IT_TRACK) && p == mu_pend_pg && idx == mu_pend_row) {   /* pidiendo al reproductor */
        fill_rrect(d, W - 44, y + m->h / 2 - 14, 30, 28, 8, pressed ? rgb(44, 44, 46) : 0, 255);
        mu_spinner(d, W - 29, y + m->h / 2); mu_spin_on = 1;
    }
}

/* --------- composicion de la pantalla */
static int mu_mini_on(void) { return mu_np.source != NP_SRC_NONE && mu_np.path[0]; }
static int mu_view_bot(void) { return mu_mini_on() ? H - MU_MINI_H : H; }
static float mu_maxscroll(const struct mpg *p)
{
    float m = p->ch - (mu_view_bot() - MU_NAV); return m > 0 ? m : 0;
}
static int mu_item_at(const struct mpg *p, int cy)      /* fila con esa y de contenido (busqueda binaria) */
{
    int lo = 0, hi = p->n - 1, r = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (p->it[mid].y <= cy) { r = mid; lo = mid + 1; } else hi = mid - 1; }
    return r;
}
static void mu_draw_mini(uint16_t *d)
{
    if (!mu_mini_on()) return;
    int y0 = H - MU_MINI_H;
    for (int y = y0; y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = dim565(d[y * W + x], 6) + rgb(16, 16, 18);
    mu_hline(d, 0, y0, rgb(48, 48, 50));
    int cy = y0 + 26;
    if (mu_npcov_ok) npx_blit_cover(d, mu_npcov, 16, cy - 20, 40, 5);
    else mu_placeholder(d, 16, cy - 20, 40, 5);
    draw_text(d, 68, cy + 6, mu_np.title, 15, 0xFFFF, W - 68 - 92, 0, H);
    int px = W - 82;                                  /* reproducir / pausa */
    if (mu_np.playing) { fill_rrect(d, px, cy - 9, 5, 18, 1.5f, 0xFFFF, 255); fill_rrect(d, px + 9, cy - 9, 5, 18, 1.5f, 0xFFFF, 255); }
    else mu_tri(d, px, cy, 19, 0xFFFF);
    int nx = W - 42;                                  /* siguiente */
    mu_tri(d, nx, cy, 15, 0xFFFF); mu_tri(d, nx + 11, cy, 15, 0xFFFF); fill_rrect(d, nx + 24, cy - 7, 3, 15, 1, 0xFFFF, 255);
}
static const char *mu_back_label(void)
{
    if (mu_sp < 1) return 0;
    const struct mpg *q = mu_st[mu_sp - 1];
    return q->kind == PG_SEARCH ? tr("Buscar") : q->title;
}
/* barra de estado sobre negro: debajo esta el inicio de fabrica, cuya barra es transparente
 * sobre el fondo (compose_statusbar: fondo oscurecido * (1 - a) + icono). Se recupera la
 * cobertura a de cada pixel y se pinta en gris sobre negro, como en las apps de iOS. */
static void mu_statusbar(uint16_t *d)
{
    if (!is_home(fr1)) { memcpy(d, fr1, 30 * W * 2); return; }
    for (int y = 0; y < 30; y++) {
        int k = 22 + y * 10 / 30;
        const uint16_t *s = fr1 + y * W, *w = wall + y * W; uint16_t *o = d + y * W;
        for (int x = 0; x < W; x++) {
            uint16_t p = s[x], b = dim565(w[x], k);
            int pr = p >> 11, pg = (p >> 5) & 63, pb = p & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
            int a = 0;                                 /* 0..255 */
            if (pr > br && br < 31) { int t = (pr - br) * 255 / (31 - br); if (t > a) a = t; }
            if (pg > bg && bg < 63) { int t = (pg - bg) * 255 / (63 - bg); if (t > a) a = t; }
            if (pb > bb && bb < 31) { int t = (pb - bb) * 255 / (31 - bb); if (t > a) a = t; }
            if (a > 255) a = 255;
            o[x] = rgb(a, a, a);
        }
    }
}
static void mu_render(uint16_t *d, struct mpg *p, int mini)
{
    mu_fading = 0; mu_spin_on = 0;
    memset(d + 30 * W, 0, (H - 30) * W * 2);
    int top = MU_NAV - (int)p->scroll;
    int first = mu_item_at(p, (int)p->scroll - (MU_NAV - 30)); if (first < 0) first = 0;
    for (int i = first; i < p->n; i++) {
        int y = top + p->it[i].y;
        if (y >= H) break;
        if (y + p->it[i].h < 30) continue;
        if (p->it[i].type == IT_EMPTY && p->it[i].a) mu_spin_on = 1;
        mu_draw_item(d, p, i, y);
    }
    /* barra de navegacion translucida */
    int scrolled = p->scroll > 4;
    for (int y = 30; y < MU_NAV; y++) for (int x = 0; x < W; x++) d[y * W + x] = scrolled ? dim565(d[y * W + x], 3) + rgb(14, 14, 15) : 0;
    if (scrolled) mu_hline(d, 0, MU_NAV - 1, MU_SEP);
    mu_statusbar(d);
    const char *back = mu_sp > 0 && mu_st[mu_sp] == p ? mu_back_label() : 0;
    if (p->kind != PG_LIB && p != mu_st[0]) {
        if (!back && mu_sp > 0) for (int k = 1; k <= mu_sp; k++) if (mu_st[k] == p) back = mu_st[k - 1]->kind == PG_SEARCH ? tr("Buscar") : mu_st[k - 1]->title;
        for (int i = 0; i < 9; i++) { fill_rrect(d, 12 + i, 52 - i, 3, 3, 1.2f, MU_ACC, 255); fill_rrect(d, 12 + i, 52 + i, 3, 3, 1.2f, MU_ACC, 255); }
        /* titulo pequeno: aparece al subir el grande (o el del album) */
        int th = p->kind == PG_ALBUM ? 8 + MU_BS + 20 : 34;
        const char *t = p->kind == PG_ALBUM && ML ? ML->A[p->arg].title : p->title;
        int small = p->kind != PG_SEARCH && p->scroll > th;
        int tw = small ? tw_n(t, 16, -1) : 0; if (tw > 150) tw = 150;
        int bmax = small ? (W - tw) / 2 - 28 - 10 : 160; if (bmax < 20) bmax = 20;    /* la etiqueta de atras no pisa el titulo */
        if (back && (bmax >= 44 || tw_n(back, 16, -1) <= bmax)) draw_text(d, 28, 58, back, 16, MU_ACC, bmax, 0, H);   /* sin sitio: solo la flecha (iOS) */
        if (small) {
            float a = (p->scroll - th) / 18.f; if (a > 1) a = 1;
            uint16_t c = mix565(0xFFFF, rgb(12, 12, 13), (unsigned)(a * 255));
            draw_text(d, (W - tw) / 2, 58, t, 16, c, 150, 0, H);
            draw_text(d, (W - tw) / 2 + 1, 58, t, 16, c, 150, 0, H);
        }
    } else if (p->kind == PG_LIB) {                  /* lupa: buscar */
        fill_circle(d, W - 30, 48, 8, MU_ACC, 255); fill_circle(d, W - 30, 48, 6, d[48 * W + W - 4], 255);
        mu_line(d, W - 24, 54, W - 18, 60, 2.6f, MU_ACC);
        if (p->scroll > 34) { int tw = tw_n(p->title, 16, -1); draw_text(d, (W - tw) / 2, 58, p->title, 16, 0xFFFF, 0, 0, H); draw_text(d, (W - tw) / 2 + 1, 58, p->title, 16, 0xFFFF, 0, 0, H); }
    }
    if (mini) mu_draw_mini(d);
}
static void mu_present(void)
{
    static double acc_r, acc_p, t_last; static int nfr;
    double t0 = now_ms();
    struct mpg *p = mu_st[mu_sp];
    mu_render(fr0, p, 1);
    if (mu_editing) kb_draw(fr0, -1);
    draw_toast(fr0);
    double t1 = now_ms();
    present();
    double t2 = now_ms();
    acc_r += t1 - t0; acc_p += t2 - t1; nfr++;
    if (t2 - t_last > 3000) { if (nfr > 20) lg("perfil musica: %d fot, dibujo %.1f ms, presentar %.1f ms", nfr, acc_r / nfr, acc_p / nfr); acc_r = acc_p = 0; nfr = 0; t_last = t2; }
}

/* --------- transicion de pila (iOS): la de encima en x = off, la de debajo a -(W-off)/3 */
static void mu_slide_frame(const uint16_t *top, const uint16_t *bot, int off)
{
    mu_statusbar(fr0);
    int bx = -(W - off) / 3, k = 22 + 10 * off / W;
    for (int y = 30; y < H; y++) {
        uint16_t *dr = fr0 + y * W; const uint16_t *tr_ = top + y * W, *br = bot + y * W;
        int xs = off < 0 ? 0 : off > W ? W : off;
        for (int x = 0; x < xs; x++) { int sx = x - bx; dr[x] = sx >= 0 && sx < W ? dim565(br[sx], k) : 0; }
        if (xs < W) memcpy(dr + xs, tr_, (W - xs) * 2);
        for (int s = 1; s <= 6 && xs - s >= 0; s++) dr[xs - s] = dim565(dr[xs - s], 20 + s * 2);
    }
    mu_draw_mini(fr0);
}
static void mu_slide(const uint16_t *top, const uint16_t *bot, int from, int to, int ms)
{
    double t0 = now_ms(); mu_busy = 1;
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1;
        mu_slide_frame(top, bot, from + (int)((to - from) * ease_out(t)));
        present();
        if (t >= 1) break;
    }
    mu_busy = 0;
}
static void mu_push(struct mpg *np_)
{
    if (!np_) return;
    if (mu_sp + 1 >= MU_DEPTH) { mu_page_free(mu_st[1]); memmove(mu_st + 1, mu_st + 2, (MU_DEPTH - 2) * sizeof *mu_st); mu_sp--; }
    mu_press = -1;
    mu_render(img_under, mu_st[mu_sp], 0);
    mu_st[++mu_sp] = np_;
    mu_page_t = now_ms();
    mu_render(img_sheet, np_, 0);
    mu_slide(img_sheet, img_under, W, 0, 320);
    mu_present();
}
static void mu_pop(void)
{
    if (mu_sp < 1) return;
    mu_press = -1;
    mu_render(img_under, mu_st[mu_sp], 0);
    mu_render(img_sheet, mu_st[mu_sp - 1], 0);
    mu_slide(img_under, img_sheet, 0, W, 300);
    mu_page_free(mu_st[mu_sp--]);
    mu_present();
}

/* --------- reproducir */
/* sin bloquear: la peticion va a la libreria (v6) y la respuesta se mira en mu_poll; mientras,
 * un indicador de actividad en la fila o el boton pulsado (mu_pend_*) */
static void mu_play_result(int r)
{
    if (r >= 0) return;
    lg("musica: reproducir -> error %d", r);
    if (r == -ENOSYS || r == -ENXIO || r == -ENOEXEC || r == -ETIMEDOUT)   /* sin respuesta: libreria sin la v6 */ toast(tr("Reproducción directa no disponible todavía"));
    else toast(tr("No se pudo reproducir"));
}
static void mu_play(const int *tl, int n, int idx, int shuffle, const char *title)
{
    if (!ML || n <= 0) return;
    if (mu_pend_id >= 0) return;                     /* ya hay una en curso */
    const char **v = malloc(n * sizeof(char *)); int *ord = malloc(n * sizeof(int));
    if (!v || !ord) { free(v); free(ord); return; }
    for (int i = 0; i < n; i++) ord[i] = tl[i];
    if (shuffle) {
        unsigned r = (unsigned)(now_ms() * 1000);
        for (int i = n - 1; i > 0; i--) { r = r * 1103515245u + 12345u; int j = (int)((r >> 8) % (unsigned)(i + 1)); int x = ord[i]; ord[i] = ord[j]; ord[j] = x; }
        idx = 0;
    }
    for (int i = 0; i < n; i++) v[i] = ML->T[ord[i]].path;      /* a:\... tal cual (lo acepta la v6) */
    unlink(PC_RES);
    int id = pc_list_send(v, n, idx < n ? idx : 0, title, shuffle ? 2 : 0);
    lg("musica: reproducir %d pistas desde %d%s -> %d", n, idx, shuffle ? " (aleatorio)" : "", id);
    free(v); free(ord);
    if (id < 0) { mu_play_result(id); return; }
    mu_pend_id = id; mu_pend_t = now_ms(); mu_pend_pg = mu_st[mu_sp]; mu_pend_row = mu_press; mu_pend_cell = mu_press_cell;
}
static int mu_play_poll(void)
{
    if (mu_pend_id < 0) return 0;
    struct pc_list_res r; int k = pc_list_poll(mu_pend_id, &r);
    if (!k && now_ms() - mu_pend_t < 5000) return 0;
    mu_play_result(k ? (r.rc < 0 ? r.rc : r.rows) : -ETIMEDOUT);
    if (k && r.rc >= 0) npx_changed = 1;
    mu_pend_id = -1; mu_pend_row = -1; mu_pend_pg = 0;
    return 1;
}

/* --------- abrir lo de fabrica: la biblioteca de HiBy */
static int mu_open_stock(void)
{
    int a = app_by_id("music"); if (a < 0) return 0;
    memcpy(img_under, fr0, HM_FRAME_BYTES);
    nav_app = a; nav_start(NAV_APP);
    double t0 = now_ms();
    while (!nav_done && now_ms() - t0 < 3000) {   /* mientras, la pantalla actual con un indicador */
        memcpy(fr0, img_under, HM_FRAME_BYTES);
        fill_rrect(fr0, W / 2 - 36, H / 2 - 36, 72, 72, 16, rgb(28, 28, 30), 230);
        mu_spinner(fr0, W / 2, H / 2);
        present(); msleep(30);
    }
    int ok = nav_wait(2500);
    if (ok != 1) { toast(tr("No se pudo abrir")); return 0; }
    double t1 = now_ms();
    for (;;) {                                         /* fundido a la de HiBy */
        double t = (now_ms() - t1) / 200; if (t > 1) t = 1;
        unsigned al = (unsigned)(255 * ease_out(t));
        for (int i = 0; i < W * H; i++) fr0[i] = mix565(fr1[i], img_under[i], al);
        present();
        if (t >= 1) break;
    }
    last_app = a; app_cache(a, 1);
    have_under = 0; launcher_on = 0; V.edit = 0; home_publish();
    cmd('R');
    lg("musica -> biblioteca de HiBy");
    return 1;
}

/* --------- menus contextuales (dibujo y Reproduciendo en nowui.inc.c, incluido despues) */
struct nu_mi { const char *t; int ic, grp; };    /* texto, icono (NI_*), empieza un grupo (separador ancho) */
enum { NI_PLAY, NI_SHUF, NI_RADIO, NI_ALB, NI_ART, NI_TAGS, NI_LYR, NI_COV, NI_ALBTAGS };
enum { NU_NONE, NU_CLOSED, NU_HOME, NU_ALBUM, NU_ARTIST, NU_RADIO };
static int nu_menu(const struct nu_mi *it, int n, const char *t1, const char *t2, int alb, const uint16_t *cov128, int ry, int rh);
static int nu_open(int sx, int sy, int *arg);
static int mu_exit_np;                            /* se salio al inicio desde Reproduciendo */
static int mu_gone;                               /* salio con el gesto de inicio: el inicio ya esta en pantalla */
static int mu_artist_of_album(int alb) { return ML ? ML->A[alb].art : -1; }
static struct mpg *mu_album_page(int alb) { return mu_page_new(PG_ALBUM, alb, ML ? ML->A[alb].title : ""); }
static struct mpg *mu_artist_page(int r) { return mu_page_new(PG_ARTIST, r, ML ? ML->R[r].name : ""); }
static char **mu_album_paths(int alb, int *n)
{
    const struct mu_alb *a = &ML->A[alb];
    char **v = calloc(a->n + 1, sizeof(char *)); *n = 0;
    for (int k = 0; v && k < a->n; k++) { char b[700]; mu_syspath(ML->T[a->first + k].path, b, sizeof b); if ((v[*n] = strdup(b))) (*n)++; }
    return v;
}
static void mu_radio(int trk)
{
    char t[200]; snprintf(t, sizeof t, tr("Radio de %s"), ML->T[trk].title);
    struct mpg *p = calloc(1, sizeof *p); if (!p) return;
    p->kind = PG_LIST; p->arg = -1; snprintf(p->title, sizeof p->title, "%s", t);
    mu_job_start(p, 3, "radio", ML->T[trk].path, "--list", 0);
    mu_build(p); mu_push(p);
}
static void mu_song_menu(int trk, struct mpg *pg, int idx, int ry, int rh)
{
    const struct mu_trk *t = &ML->T[trk];
    const struct mu_alb *al = &ML->A[t->alb];
    struct nu_mi it[8]; int n = 0, g = 0;
    it[n++] = (struct nu_mi){ tr("Reproducir"), NI_PLAY, 0 };
    it[n++] = (struct nu_mi){ tr("Crear radio"), NI_RADIO, 0 };
    if (pg->kind != PG_ALBUM) { it[n++] = (struct nu_mi){ tr("Ir al álbum"), NI_ALB, 1 }; g = 1; }
    it[n++] = (struct nu_mi){ tr("Ir al artista"), NI_ART, !g };
    it[n++] = (struct nu_mi){ tr("Editar canción"), NI_TAGS, 1 };
    it[n++] = (struct nu_mi){ tr("Editar álbum"), NI_ALBTAGS, 0 };
    it[n++] = (struct nu_mi){ tr("Buscar letra"), NI_LYR, 0 };
    it[n++] = (struct nu_mi){ tr("Buscar carátula"), NI_COV, 0 };
    char sub[300]; snprintf(sub, sizeof sub, "%s · %s", t->artist, al->title);
    int a = nu_menu(it, n, t->title, sub, t->alb, 0, ry, rh);
    int act = a >= 0 ? it[a].ic : -1;
    char path[700]; mu_syspath(t->path, path, sizeof path);
    if (act == NI_PLAY) mu_play(pg->tl, pg->ntl, idx, 0, pg->title);
    else if (act == NI_RADIO) { mu_present(); mu_radio(trk); return; }
    else if (act == NI_TAGS) { char *v[1] = { path }; tag_editor(v, 1); }
    else if (act == NI_ALBTAGS) { int na; char **v = mu_album_paths(t->alb, &na); if (v && na) tag_editor(v, na); if (v) free_list(v, na); }
    else if (act == NI_LYR) lyrics_search(path, t->artist, t->title, al->title);
    else if (act == NI_COV) { char *v[1] = { path }; cover_search(v, 1, al->artist, al->title); mu_cover_forget(t->alb); }
    else if (act == NI_ALB) { mu_present(); mu_push(mu_album_page(t->alb)); return; }
    else if (act == NI_ART) { int r = mu_artist_of_album(t->alb); if (r >= 0) { mu_present(); mu_push(mu_artist_page(r)); return; } }
    mu_page_t = now_ms();
    mu_present();
}
static void mu_album_menu(int alb, int ry, int rh)
{
    const struct mu_alb *al = &ML->A[alb];
    struct nu_mi it[5] = { { tr("Reproducir"), NI_PLAY, 0 }, { tr("Aleatorio"), NI_SHUF, 0 }, { tr("Ir al artista"), NI_ART, 1 },
                           { tr("Editar etiquetas"), NI_TAGS, 1 }, { tr("Buscar carátula"), NI_COV, 0 } };
    char sub[200]; if (al->year) snprintf(sub, sizeof sub, "%s · %d", al->artist, al->year); else snprintf(sub, sizeof sub, "%s", al->artist);
    int a = nu_menu(it, 5, al->title, sub, alb, 0, ry, rh);
    int act = a >= 0 ? it[a].ic : -1;
    if (act == NI_PLAY || act == NI_SHUF) {
        int *v = malloc(al->n * sizeof(int)); if (v) { for (int k = 0; k < al->n; k++) v[k] = al->first + k; mu_play(v, al->n, 0, act == NI_SHUF, al->title); free(v); }
    } else if (act == NI_TAGS || act == NI_COV) {
        int n; char **v = mu_album_paths(alb, &n);
        if (v && n) { if (act == NI_TAGS) tag_editor(v, n); else { cover_search(v, n, al->artist, al->title); mu_cover_forget(alb); } }
        for (int i = 0; i < n; i++) free(v[i]); free(v);
    } else if (act == NI_ART) { mu_present(); mu_push(mu_artist_page(al->art)); return; }
    mu_page_t = now_ms();
    mu_present();
}

/* --------- toque en una fila */
static void mu_tap(struct mpg *p, int idx, int x, int longp)
{
    const struct mit *m = &p->it[idx];
    struct mu_lib *L = ML;
    int ry = MU_NAV - (int)p->scroll + m->y;         /* la fila en pantalla (tarjeta del menu) */
    switch (m->type) {
    case IT_MENU:
        if (longp) return;
        if (m->a == M_HIBY) { if (mu_open_stock()) mu_open = -1; return; }
        if (m->a == M_PLAYLISTS) mu_push(mu_page_new(PG_PLAYLISTS, 0, tr("Listas")));
        else if (m->a == M_ARTISTS) mu_push(mu_page_new(PG_ARTISTS, 0, tr("Artistas")));
        else if (m->a == M_ALBUMS) mu_push(mu_page_new(PG_ALBUMS, 0, tr("Álbumes")));
        else if (m->a == M_SONGS) mu_push(mu_page_new(PG_SONGS, 0, tr("Canciones")));
        else if (m->a == M_GENRES) mu_push(mu_page_new(PG_GENRES, 0, tr("Géneros")));
        else if (m->a == M_MIXES) mu_push(mu_page_new(PG_MIXES, 0, tr("Mezclas")));
        break;
    case IT_GRID: {
        int a = x >= W / 2 ? m->b : m->a; if (a < 0 || !L) return;
        if (longp) mu_album_menu(a, ry, m->h); else mu_push(mu_album_page(a));
        break; }
    case IT_ALBUMROW: if (longp) mu_album_menu(m->a, ry, m->h); else mu_push(mu_album_page(m->a)); break;
    case IT_ARTIST: if (!longp) mu_push(mu_artist_page(m->a)); break;
    case IT_SONG: case IT_TRACK: if (longp) mu_song_menu(m->a, p, m->b, ry, m->h); else { mu_play(p->tl, p->ntl, m->b, 0, p->title); mu_present(); } break;
    case IT_BUTTONS: if (!longp) { mu_play(p->tl, p->ntl, 0, x >= W / 2, p->title); mu_present(); } break;
    case IT_TEXT: if (!longp && p->kind == PG_GENRES && L) mu_push(mu_page_new(PG_LIST, m->a, L->gen[m->a])); break;
    case IT_PLAYLIST: {
        if (longp) return;
        const struct mpl *e = &p->pl[m->a];
        struct mpg *q = calloc(1, sizeof *q); if (!q) return;
        q->kind = PG_LIST; q->arg = -1; snprintf(q->title, sizeof q->title, "%s", e->name);
        mu_job_start(q, 2, "playlist", e->kind, e->arg, 0);
        mu_build(q); mu_push(q);
        break; }
    case IT_MIX: {
        if (longp) return;
        static const char *const kinds[5] = { "dia", "descubre", "favoritos", "nuevas", "decada" };
        static const char *const nm[6] = { "Mezcla del día", "Descubre", "Tus favoritos", "Recién llegadas", "Tu década", "Radio de lo que suena" };
        struct mpg *q = calloc(1, sizeof *q); if (!q) return;
        q->kind = PG_LIST; q->arg = -1; snprintf(q->title, sizeof q->title, "%s", tr(nm[m->a]));
        if (m->a < 5) mu_job_start(q, 3, "make", kinds[m->a], "--list", 0);
        else {
            int t = L ? mu_find_sys(L, mu_np.path) : -1;
            if (t >= 0) snprintf(q->title, sizeof q->title, tr("Radio de %s"), L->T[t].title);
            mu_job_start(q, 3, "radio", mu_np.path, "--list", 0);
        }
        mu_build(q); mu_push(q);
        break; }
    case IT_SEARCH: break;   /* lo gestiona el bucle (teclado) */
    }
}

/* --------- buscar con el teclado: resultados mientras se escribe */
static void mu_search_redraw(uint16_t *d, int pressed)
{
    struct mpg *p = mu_st[mu_sp];
    if (strcmp(mu_qlast, mu_q)) { snprintf(mu_qlast, sizeof mu_qlast, "%s", mu_q); p->scroll = 0; mu_build(p); }
    mu_render(d, p, 0);
    kb_draw(d, pressed);
    draw_toast(d);
}
/* devuelve: 0 sigue en la pagina (sin teclado), -1 cancelar (volver), >0 fila tocada + 1,
 * -3 se fue al inicio con el gesto (la app sale sin su animacion) */
static int mu_search_edit(int *tap_x)
{
    struct ebuf e = { mu_q, (int)strlen(mu_q), sizeof mu_q, (int)strlen(mu_q) };
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Buscar"));
    mu_editing = 1;
    {   /* el campo de busqueda (fila IT_SEARCH) en coordenadas de pagina: el teclado nunca lo tapa */
        struct mpg *sp = mu_st[mu_sp]; int fy = -1;
        for (int i = 0; sp && i < sp->n; i++) if (sp->it[i].type == IT_SEARCH) { fy = MU_NAV - (int)sp->scroll + sp->it[i].y + 8; break; }
        kb_begin_ex(&e, fr0, mu_search_redraw, fy, fy >= 0 ? fy + 36 : -1);
    }
    struct pkt p; int res = 0;
    for (;;) {
        if (!read_pkt(&p, 300)) { if (mu_cov_new) { mu_cov_new = 0; mu_search_redraw(fr0, -1); present(); } continue; }
        if (!p.down) continue;
        mu_last_input = now_ms();
        kb_map(&p);
        if (p.y >= KB_Y) {
            int kx = p.x, ky = p.y - kb_pg, r = kb_touch(&p, &e, fr0, mu_search_redraw, 0);
            if (r == K_ENTER) { res = 0; break; }
            if (r == K_HOME) {                            /* inicio con el teclado abierto (iOS) */
                mu_search_redraw(fr0, -1);
                if (own_home_gesture(kx, ky, app_by_id("music"))) { res = -3; break; }
                mu_search_redraw(fr0, -1); present();
            }
            continue;
        }
        int x = p.x, y = p.y, moved = 0;
        while (cur_down && read_pkt(&p, 3000)) if (abs(cur_y - y) > 12) moved = 1;
        struct mpg *pg = mu_st[mu_sp];
        if (y < MU_NAV + 52 && y >= MU_NAV && x > W - 90) { res = -1; break; }
        if (y < MU_NAV && x < 110) { res = -1; break; }
        if (moved) { res = 0; break; }                 /* desplazar: se esconde el teclado (iOS) */
        int i = mu_item_at(pg, y - MU_NAV + (int)pg->scroll);
        if (i > 0 && y >= MU_NAV) { res = i + 1; *tap_x = x; break; }
    }
    mu_editing = 0;
    kb_dst = fr0; kb_rd = mu_search_redraw;
    if (res >= 0) kb_hide();
    kb_end();
    return res;
}

/* --------- refresco de datos en el hilo de la interfaz */
static int mu_poll(void)
{
    int dirty = 0;
    if (mu_lib_state == 2) {
        __sync_synchronize();
        pthread_mutex_lock(&mu_mu); mu_lib_free(ML); ML = mu_lib_new; mu_lib_new = 0; mu_pref_i = 0; pthread_mutex_unlock(&mu_mu);
        mu_lib_state = 4;
        for (int i = 0; i <= mu_sp; i++) { if (mu_st[i]->kind == PG_LIST && mu_st[i]->arg >= 0 && !mu_st[i]->tl) {} mu_build(mu_st[i]); }
        mu_np_trk = mu_np.path[0] && ML ? mu_find_sys(ML, mu_np.path) : -1;
        dirty = 1;
    } else if (mu_lib_state == 3) { mu_lib_state = 5; for (int i = 0; i <= mu_sp; i++) mu_build(mu_st[i]); dirty = 1; }
    for (int i = 0; i <= mu_sp; i++) if (mu_st[i]->job && mu_st[i]->job->done) { mu_job_done(mu_st[i]); dirty = 1; }
    if (mu_cov_new) { mu_cov_new = 0; dirty = 1; }
    if (mu_play_poll()) dirty = 1;
    static double last_np;
    if (npx_changed || now_ms() - last_np > 1000) {
        last_np = now_ms(); npx_changed = 0;
        struct np_info n; static uint16_t cov[128 * 128]; int ok;
        npx_snapshot(&n, cov, &ok);
        if (n.track_id != mu_np.track_id || n.playing != mu_np.playing || n.source != mu_np.source || ok != mu_npcov_ok || strcmp(n.title, mu_np.title)) {
            int was = mu_mini_on();
            mu_np = n; memcpy(mu_npcov, cov, sizeof cov); mu_npcov_ok = ok;
            mu_np_trk = n.path[0] && ML ? mu_find_sys(ML, n.path) : -1;
            if (was != mu_mini_on()) for (int i = 0; i <= mu_sp; i++) if (mu_st[i]->kind == PG_MIXES) mu_build(mu_st[i]);
            dirty = 1;
        }
    }
    return dirty;
}

/* --------- app */
static void music_free_all(void)
{
    for (int i = 0; i <= mu_sp; i++) { mu_page_free(mu_st[i]); mu_st[i] = 0; }
    mu_sp = 0;
    mu_open = 0;
    pthread_mutex_lock(&mu_mu); mu_lib_free(ML); ML = 0; pthread_mutex_unlock(&mu_mu);
    if (mu_lib_state == 2) { mu_lib_free(mu_lib_new); mu_lib_new = 0; }
    if (mu_lib_state != 1) mu_lib_state = 0;
    mu_covers_free();
    for (int r = 0; r <= MU_GS / 2; r++) { free(mu_ctab[r]); mu_ctab[r] = 0; }
    free(mu_up); mu_up = 0; mu_up_alb = -1;
    for (int k = 0; k < 4; k++) { free(mu_ph[k].px); mu_ph[k].px = 0; mu_ph[k].s = 0; }
}
static int mu_autonp;      /* abrir directamente Reproduciendo (icono "Reproduciendo" del inicio) */
static void music_app(int ix, int iy)
{
    have_under = 0;                                    /* img_under / img_sheet se reutilizan */
    mu_open = 1; mu_last_input = now_ms(); mu_q[0] = 0; snprintf(mu_qlast, sizeof mu_qlast, "\x01");
    mu_mkcache();
    if (!mu_wk_started) { pthread_t th; if (!pthread_create(&th, 0, mu_cover_thread, 0)) { pthread_detach(th); mu_wk_started = 1; } }
    if (mu_lib_state != 1) { mu_lib_state = 1; pthread_t th; if (!pthread_create(&th, 0, mu_load_thread, 0)) pthread_detach(th); else mu_lib_state = 3; }
    npx_snapshot(&mu_np, mu_npcov, &mu_npcov_ok);
    mu_sp = 0; mu_st[0] = mu_page_new(PG_LIB, 0, tr("Biblioteca"));
    if (!mu_st[0]) { mu_open = 0; return; }
    /* la biblioteca suele estar en cache: esperar un poco para abrir ya con contenido */
    { double tw = now_ms(); while (mu_lib_state == 1 && now_ms() - tw < 120) msleep(5); mu_poll(); }
    mu_page_t = now_ms();
    render_launcher(img_base, now_ms());
    mu_render(img_app, mu_st[0], 1);
    double t0 = now_ms();
    for (;;) {                                         /* zoom desde el icono */
        double t = (now_ms() - t0) / 300; if (t > 1) t = 1;
        double e = ease_out(t);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), (int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), win_radius((int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), e));
        present();
        if (t >= 1) break;
    }
    mu_present();
    float vel = 0; int bounce = 0; double lt = 0, spin_t = 0;
    struct pkt p;
    int pend_np = mu_autonp; mu_autonp = 0;
    for (;;) {
        struct mpg *pg = mu_st[mu_sp];
        if (pend_np) {   /* entrar directamente en Reproduciendo */
            pend_np = 0;
            int arg = -1, r = nu_open(W / 2, H - 20, &arg);
            drain_until_up();
            if (r == NU_HOME) { mu_exit_np = 1; break; }
            npx_changed = 1; mu_poll();
            if (r == NU_ALBUM && arg >= 0) { mu_present(); mu_push(mu_album_page(arg)); }
            else if (r == NU_ARTIST && arg >= 0) { mu_present(); mu_push(mu_artist_page(arg)); }
            else if (r == NU_RADIO && arg >= 0) { mu_present(); mu_radio(arg); }
            else mu_present();
            continue;
        }
        if (mu_poll()) mu_present();
        float maxs = mu_maxscroll(pg);
        /* inercia (deceleracion de iOS) y rebote en los extremos */
        if (fabsf(vel) > 0.02f || bounce) {
            mu_busy = 1;
            double now = now_ms(); double dt = lt ? now - lt : 16; if (dt > 50) dt = 16; lt = now;
            if (bounce) {
                float target = pg->scroll < 0 ? 0 : maxs;
                pg->scroll += (target - pg->scroll) * (1 - powf(0.80f, (float)dt / 16.f));
                if (fabsf(pg->scroll - target) < 0.5f) { pg->scroll = target; bounce = 0; }
            } else {
                pg->scroll -= vel * (float)dt; vel *= powf(0.9975f, (float)dt);
                if (pg->scroll < 0 || pg->scroll > maxs) { if (pg->scroll < -30) pg->scroll = -30; if (pg->scroll > maxs + 30) pg->scroll = maxs + 30; vel = 0; bounce = 1; }
            }
            mu_present();
            if (!read_pkt(&p, 0)) continue;
            vel = 0; bounce = 0; lt = 0;
        } else {
            mu_busy = 0; lt = 0;
            int anim = mu_fading || mu_spin_on || now_ms() < toast_until + 40;
            if (!read_pkt(&p, anim ? 0 : 200)) {
                if (anim) { if (!mu_spin_on || mu_fading || now_ms() - spin_t > 80) { spin_t = now_ms(); mu_present(); } else msleep(8); }
                continue;
            }
        }
        if (!p.down) continue;
        mu_last_input = now_ms();
        pg = mu_st[mu_sp]; maxs = mu_maxscroll(pg);
        int sx = p.x, sy = p.y; double ts = now_ms();
        /* desde abajo: inicio (la ventana sigue al dedo; mantener 1 s: multitarea) */
        if (sy >= 456) {
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { mu_render(fr0, pg, 1); if (own_home_gesture(sx, sy, app_by_id("music"))) { mu_gone = 1; break; } mu_present(); continue; }
        }
        /* mini reproductor */
        if (mu_mini_on() && sy >= H - MU_MINI_H) {
            if (sx < W - 100) {                       /* Reproduciendo propio: sube desde aqui (toque o dedo) */
                int arg = -1, r = nu_open(sx, sy, &arg);
                drain_until_up();
                if (r == NU_HOME) { mu_exit_np = 1; break; }
                npx_changed = 1; mu_poll();
                if (r == NU_ALBUM && arg >= 0) { mu_present(); mu_push(mu_album_page(arg)); }
                else if (r == NU_ARTIST && arg >= 0) { mu_present(); mu_push(mu_artist_page(arg)); }
                else if (r == NU_RADIO && arg >= 0) { mu_present(); mu_radio(arg); }
                else mu_present();
                continue;
            }
            while (cur_down && read_pkt(&p, 3000)) {}
            if (abs(cur_x - sx) > 30) continue;
            if (sx >= W - 54) { pc_next(); continue; }
            if (sx >= W - 100) { pc_toggle(); mu_np.playing = !mu_np.playing; mu_present(); continue; }
            mu_present(); continue;
        }
        if (sy >= 456) continue;
        /* desde cualquiera de los dos bordes: atras (gesto comun, con la burbuja) */
        if (back_zone(sx, sy)) {                       /* en la raiz: salir de la app */
            int bk = app_back_gesture(sx, sy);
            if (bk > 0 && mu_sp == 0) break;
            if (bk > 0) { mu_pop(); continue; }
            if (bk < 0) { mu_present(); continue; }
        }
        /* barra de navegacion */
        if (sy < MU_NAV) {
            while (cur_down && read_pkt(&p, 3000)) {}
            if (mu_sp > 0 && sx < 110) { mu_pop(); continue; }
            if (pg->kind == PG_LIB && sx > W - 56) {
                struct mpg *s = mu_page_new(PG_SEARCH, 0, tr("Buscar"));
                if (s) { mu_q[0] = 0; snprintf(mu_qlast, sizeof mu_qlast, "%s", ""); mu_build(s); mu_push(s);
                    int tx = 0, r = mu_search_edit(&tx);
                    if (r == -3) { mu_gone = 1; break; } if (r == -1) mu_pop(); else if (r > 0) mu_tap(mu_st[mu_sp], r - 1, tx, 0); else mu_present(); }
                if (mu_open < 0) break;
                continue;
            }
            /* toque en la barra: volver arriba (iOS) */
            if (pg->scroll > 0) { float s0 = pg->scroll; double t1 = now_ms(); for (;;) { double t = (now_ms() - t1) / 320; if (t > 1) t = 1; pg->scroll = s0 * (float)(1 - ease_out(t)); mu_present(); if (t >= 1) break; } }
            continue;
        }
        /* contenido: tocar, pulsacion larga o arrastrar */
        int cy = sy - MU_NAV + (int)pg->scroll;
        int row = mu_item_at(pg, cy);
        if (row >= 0 && cy >= pg->it[row].y + pg->it[row].h) row = -1;
        int tappable = row >= 0 && pg->it[row].type != IT_TITLE && pg->it[row].type != IT_SECTION && pg->it[row].type != IT_EMPTY && pg->it[row].type != IT_FOOTER && pg->it[row].type != IT_DISC && pg->it[row].type != IT_HERO;
        if (tappable && pg->it[row].type == IT_GRID) { mu_press_cell = sx >= W / 2; if ((mu_press_cell ? pg->it[row].b : pg->it[row].a) < 0) tappable = 0; }
        if (tappable && pg->it[row].type == IT_BUTTONS) mu_press_cell = sx >= W / 2;
        float s0 = pg->scroll; int moved = 0, lastyy = sy, longp = 0; double ltt = now_ms(); vel = 0;
        int pressed_shown = 0;
        while (cur_down) {
            int got = read_pkt(&p, 30);
            if (got && !p.down) break;
            double tn = now_ms();
            if (!pressed_shown && tappable && !moved && tn - ts > 70) { mu_press = row; mu_present(); pressed_shown = 1; }
            if (abs(cur_y - sy) > 8) { if (!moved && mu_press >= 0) { mu_press = -1; } moved = 1; }
            if (moved) {
                mu_busy = 1;
                if (tn > ltt) vel = 0.6f * vel + 0.4f * (cur_y - lastyy) / (float)(tn - ltt);
                lastyy = cur_y; ltt = tn;
                float s = s0 - (cur_y - sy);
                if (s < 0) s = s / 3; else if (s > maxs) s = maxs + (s - maxs) / 3;   /* goma */
                pg->scroll = s;
                mu_present();
            }
            if (!moved && tappable && tn - ts > 450) { longp = 1; break; }
        }
        mu_busy = 0;
        if (moved) { mu_press = -1; if (pg->scroll < 0 || pg->scroll > maxs) { vel = 0; bounce = 1; } if (fabsf(vel) < 0.05f) vel = 0; continue; }
        vel = 0;
        if (!tappable) { if (pressed_shown) { mu_press = -1; mu_present(); } continue; }
        if (longp) {
            mu_press = row; mu_present();
            int t_ = pg->it[row].type;                /* menu contextual: se puede elegir sin soltar (iOS) */
            if (t_ != IT_SONG && t_ != IT_TRACK && t_ != IT_GRID && t_ != IT_ALBUMROW) while (cur_down && read_pkt(&p, 3000)) {}
        }
        else if (!pressed_shown) { mu_press = row; mu_present(); msleep(40); }
        int type = pg->it[row].type;
        if (type == IT_SEARCH) {
            mu_press = -1;
            int tx = 0, r = mu_search_edit(&tx);
            if (r == -1) mu_pop(); else if (r > 0) mu_tap(mu_st[mu_sp], r - 1, tx, 0); else mu_present();
        } else {
            if (longp && (type == IT_SONG || type == IT_TRACK || type == IT_GRID || type == IT_ALBUMROW)) { /* la hoja se abre sobre la fila marcada */ }
            mu_tap(pg, row, sx, longp);
            if (mu_open < 0) break;
            mu_press = -1;
            if (mu_st[mu_sp] == pg) mu_present();
        }
    }
    mu_busy = 0;
    if (mu_open < 0) { music_free_all(); drain_until_up(); return; }   /* se fue a la pantalla de HiBy */
    /* salir: la ventana se encoge hacia su icono */
    mu_press = -1;
    if (mu_exit_np) memcpy(img_app, fr0, HM_FRAME_BYTES);   /* desde Reproduciendo: se encoge ella */
    else if (!mu_gone) mu_render(img_app, mu_st[mu_sp], 1);
    mu_exit_np = 0;
    if (!mu_gone) render_launcher(img_base, now_ms());
    t0 = now_ms();
    if (!mu_gone) for (;;) {
        double t = (now_ms() - t0) / 280; if (t > 1) t = 1;
        double e = 1 - ease_out(t);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), (int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), win_radius((int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), e));
        present();
        if (t >= 1) break;
    }
    mu_gone = 0;
    music_free_all();
    drain_until_up();
    launcher_present();
}
