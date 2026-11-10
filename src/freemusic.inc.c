/* freemusic.inc.c - "Free Music": search, listen to and download free / Creative Commons music.
 *
 * Sources: Internet Archive (archive.org: Live Music Archive, netlabels, community audio; no key)
 * and Jamendo (Creative Commons; needs a free client_id, entered in the app's settings or read
 * from SD/HiByOS/jamendo_client_id.txt). The quality is shown in the results themselves: badges
 * "FLAC 24/96", "FLAC 16/44.1", "MP3 320", "MP3 VBR", "OGG" + size. The format names come with
 * the search; sample rate / bits are read from the FLAC STREAMINFO of the first lossless track
 * (the first few KB of the file, lazily, only for rows on screen, cached on disk).
 *
 * Threads (no network or disk work on the UI thread):
 *   - two network workers (hibynet subprocesses): "hi" = searches, album pages, key test;
 *     "lo" = thumbnails, covers, STREAMINFO probes. Results go back through a done-list.
 *   - one download worker that lives for the whole shell session (keeps going after leaving the
 *     app): one file at a time, nice 19 + idle I/O class (audio first), queue persisted in
 *     appdata (resumed when the app is opened again), free-space check per file, tags written
 *     with hibytag, cover.jpg per album. "Listen" = the same worker caches the track in
 *     Music/Free/.cache (LRU, capped) and hands the album to the stock player (pc_list_send,
 *     unscanned files allowed) as soon as the first track is complete; the next one is fetched
 *     ahead while it plays.
 * Pure parsing/labels: freemusic_core.inc.c (tests/freemusic_test.c). Docs: docs/FREEMUSIC.md. */
#include <sys/resource.h>
#include <sys/syscall.h>
#include <utime.h>
#include "freemusic_core.inc.c"

#define FM_DATA "/usr/data/hibymod/appdata/freemusic"
#define FM_CACHE "/usr/data/hibymod/cache/freemusic"
#define FM_MUSIC SD_ROOT "/Music/Free"
#define FM_LCACHE FM_MUSIC "/.cache"
#define FM_KEYFILE_SD SD_ROOT "/HiByOS/jamendo_client_id.txt"
#define FM_KEYFILE FM_DATA "/jamendo_client_id"
#define FM_TH 56                 /* thumbnail in the rows */
#define FM_BIG 128               /* cover on the album page */
#define FM_RH 76                 /* result row */
#define FM_TRH 54                /* track row */
#define FM_PAGE 40               /* results per request */

/* ------------------------------------------------------------------ settings */
static int fm_src, fm_lossless, fm_coll, fm_genre = -1, fm_lang = -1, fm_sort = FSORT_REL, fm_cache_mb = 500;   /* fm_sort: searches only */
static char fm_key[100];
static void fm_mkdirs(void)
{
    mkdir("/usr/data/hibymod/appdata", 0755); mkdir(FM_DATA, 0755);
    mkdir("/usr/data/hibymod/cache", 0755); mkdir(FM_CACHE, 0755); mkdir(FM_CACHE "/th", 0755); mkdir(FM_CACHE "/cv", 0755);
}
static void fm_conf_load(void)
{
    FILE *f = fopen(FM_DATA "/settings", "r");
    if (f) {
        char l[128];
        while (fgets(l, sizeof l, f)) {
            int v; char k[32];
            if (sscanf(l, "%31[^=]=%d", k, &v) != 2) continue;
            if (!strcmp(k, "src")) fm_src = v == FM_SRC_JM; else if (!strcmp(k, "lossless")) fm_lossless = !!v;
            else if (!strcmp(k, "coll")) fm_coll = v >= 0 && v < FCOL_N ? v : 0; else if (!strcmp(k, "genre")) fm_genre = v >= -1 && v < FM_NGENRE ? v : -1;
            else if (!strcmp(k, "lang")) fm_lang = v >= -1 && v < FM_NLANG ? v : -1; else if (!strcmp(k, "sort")) fm_sort = v >= 0 && v <= FSORT_REL ? v : FSORT_REL;
            else if (!strcmp(k, "cache")) fm_cache_mb = v >= 100 && v <= 8000 ? v : 500;
        }
        fclose(f);
    }
    /* Jamendo key: appdata, or the SD file (copied into appdata once) */
    fm_key[0] = 0;
    const char *paths[2] = { FM_KEYFILE, FM_KEYFILE_SD };
    for (int i = 0; i < 2 && !fm_key[0]; i++) {
        FILE *k = fopen(paths[i], "r"); if (!k) continue;
        char l[128] = ""; if (!fgets(l, sizeof l, k)) l[0] = 0;
        fclose(k);
        int n = 0; for (char *c = l; *c && n < (int)sizeof fm_key - 1; c++) if (isalnum((unsigned char)*c) || *c == '_' || *c == '-') fm_key[n++] = *c;
        fm_key[n] = 0;
        if (fm_key[0] && i == 1) { FILE *o = fopen(FM_KEYFILE, "w"); if (o) { fprintf(o, "%s\n", fm_key); fclose(o); } }
    }
}
static void fm_conf_save(void)
{
    FILE *f = fopen(FM_DATA "/settings.tmp", "w"); if (!f) return;
    fprintf(f, "src=%d\nlossless=%d\ncoll=%d\ngenre=%d\nlang=%d\nsort=%d\ncache=%d\n", fm_src, fm_lossless, fm_coll, fm_genre, fm_lang, fm_sort, fm_cache_mb);
    if (!fclose(f)) rename(FM_DATA "/settings.tmp", FM_DATA "/settings");
}
static void fm_key_save(void)
{
    if (!fm_key[0]) { unlink(FM_KEYFILE); return; }
    FILE *o = fopen(FM_KEYFILE, "w"); if (o) { fprintf(o, "%s\n", fm_key); fclose(o); }
}

/* ------------------------------------------------------------------ processes and files */
static void fm_lowprio(int nice)
{
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice);
#ifdef SYS_ioprio_set
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)syscall(SYS_gettid), nice >= 19 ? 3 << 13 /* idle */ : (2 << 13) | 7 /* best effort, lowest */);
#endif
}
static pid_t fm_spawn(char *const argv[], int *errfd, const char *extra_env)
{
    int fd[2] = { -1, -1 }; if (errfd && pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (errfd) { posix_spawn_file_actions_adddup2(&fa, fd[1], 2); posix_spawn_file_actions_addclose(&fa, fd[0]); posix_spawn_file_actions_addclose(&fa, fd[1]); }
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char **envp = environ, *ev[96]; int ne = 0;
    if (extra_env) {
        for (char **e = environ; *e && ne < 90; e++) ev[ne++] = *e;
        ev[ne++] = (char *)extra_env; ev[ne] = 0; envp = ev;
    }
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    if (errfd) { close(fd[1]); if (r) { close(fd[0]); return -1; } fcntl(fd[0], F_SETFL, O_NONBLOCK); *errfd = fd[0]; }
    return r ? -1 : pid;
}
static int fm_wait(pid_t pid)
{
    if (pid <= 0) return 99;
    int s; while (waitpid(pid, &s, 0) < 0) if (errno != EINTR) return 99;
    return WIFEXITED(s) ? WEXITSTATUS(s) : 98;
}
static int fm_get(const char *url, const char *out)
{
    char *argv[] = { NETBIN, "get", (char *)url, (char *)out, 0 };
    return fm_wait(fm_spawn(argv, 0, 0));
}
static char *fm_slurp(const char *path, long max, long *len)
{
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat sb; char *b = 0;
    if (!fstat(fd, &sb) && sb.st_size > 0 && sb.st_size <= max && (b = malloc(sb.st_size + 1))) {
        long got = 0; while (got < sb.st_size) { ssize_t r = read(fd, b + got, sb.st_size - got); if (r <= 0) break; got += r; }
        if (got != sb.st_size) { free(b); b = 0; } else { b[got] = 0; if (len) *len = got; }
    }
    close(fd);
    return b;
}
static int fm_fresh(const char *path, int max_age_s) { struct stat s; return !stat(path, &s) && s.st_size > 0 && time(0) - s.st_mtime < max_age_s; }
static void fm_hpath(const char *dir, const char *key, const char *ext, char *o, int n) { snprintf(o, n, "%s/%08x%s", dir, st_hash(key), ext); }
/* file path inside an IA item -> URL path (each byte that is not unreserved -> %XX, '/' kept) */
static void fm_pathenc(const char *s, char *o, int cap)
{
    static const char hx[] = "0123456789ABCDEF"; int k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k < cap - 4; p++) {
        if (isalnum(*p) || strchr("-_.~/", *p)) o[k++] = (char)*p;
        else { o[k++] = '%'; o[k++] = hx[*p >> 4]; o[k++] = hx[*p & 15]; }
    }
    o[k] = 0;
}
static void fm_file_url(const struct fm_album *a, const struct fm_file *f, char *o, int n)
{
    if (a->it.src == FM_SRC_JM) { fm_cpy(o, n, f->name); return; }
    char e[600]; fm_pathenc(f->name, e, sizeof e);
    snprintf(o, n, "https://archive.org/download/%s/%s", a->it.id, e);
}
static int fm_mkdir_p(const char *path)
{
    char t[600]; fm_cpy(t, sizeof t, path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    return mkdir(t, 0755) && errno != EEXIST ? -1 : 0;
}
/* Linux path on the SD -> path of the player ("a:\Music\Free\...") */
static void fm_player_path(const char *p, char *o, int n)
{
    const char *r = !strncmp(p, SD_ROOT "/", strlen(SD_ROOT) + 1) ? p + strlen(SD_ROOT) + 1 : 0;
    if (!r) { fm_cpy(o, n, p); return; }
    int k = snprintf(o, n, "a:\\");
    for (; *r && k < n - 1; r++) o[k++] = *r == '/' ? '\\' : *r;
    o[k] = 0;
}
/* 96x96 RGB565 (little endian) -> s x s, area average */
static void fm_scale565(const uint16_t *src, int sw, uint16_t *dst, int s)
{
    for (int y = 0; y < s; y++) for (int x = 0; x < s; x++) {
        int x0 = x * sw / s, x1 = (x + 1) * sw / s, y0 = y * sw / s, y1 = (y + 1) * sw / s;
        if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
        unsigned r = 0, g = 0, b = 0, n = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { uint16_t p = src[yy * sw + xx]; r += p >> 11; g += (p >> 5) & 63; b += p & 31; n++; }
        dst[y * s + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
    }
}
/* bilinear up-scale (placeholder cover until the real one is decoded) */
static void fm_upscale565(const uint16_t *src, int sw, uint16_t *dst, int s)
{
    for (int y = 0; y < s; y++) for (int x = 0; x < s; x++) {
        float fx = (x + .5f) * sw / s - .5f, fy = (y + .5f) * sw / s - .5f;
        int ix = (int)fx, iy = (int)fy; if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (ix > sw - 2) ix = sw - 2; if (iy > sw - 2) iy = sw - 2;
        float ax = fx - ix, ay = fy - iy; if (ax < 0) ax = 0; if (ay < 0) ay = 0;
        uint16_t p[4] = { src[iy * sw + ix], src[iy * sw + ix + 1], src[(iy + 1) * sw + ix], src[(iy + 1) * sw + ix + 1] };
        float w[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay }, r = 0, g = 0, b = 0;
        for (int k = 0; k < 4; k++) { r += w[k] * (p[k] >> 11); g += w[k] * ((p[k] >> 5) & 63); b += w[k] * (p[k] & 31); }
        dst[y * s + x] = (uint16_t)((int)(r + .5f) << 11 | (int)(g + .5f) << 5 | (int)(b + .5f));
    }
}
static uint16_t *fm_load_raw(const char *path, int s)
{
    long n = 0; char *b = fm_slurp(path, (long)s * s * 2, &n);
    if (!b || n != (long)s * s * 2) { free(b); return 0; }
    return (uint16_t *)b;
}

/* ------------------------------------------------------------------ STREAMINFO probe cache */
/* per item: lossless quality (STREAMINFO) + what the files list says (tracks, duration, size) */
struct fm_pc { char key[84]; short bits, ntr; int sr; uint8_t ch; float tlen; long long dsize; };
static struct fm_pc *fm_probes; static int fm_nprobe, fm_probes_loaded;
static pthread_mutex_t fm_pmu = PTHREAD_MUTEX_INITIALIZER;
#define FM_NPROBE 2048
static void fm_probe_key(int src, const char *id, char *o, int n) { snprintf(o, n, "%c:%s", src == FM_SRC_JM ? 'j' : 'a', id); }
static void fm_probes_load(void)
{
    if (fm_probes_loaded) return;
    fm_probes_loaded = 1;
    fm_probes = calloc(FM_NPROBE, sizeof *fm_probes); if (!fm_probes) return;
    FILE *f = fopen(FM_CACHE "/probes.txt", "r"); if (!f) return;
    char l[200];
    while (fgets(l, sizeof l, f)) {
        struct fm_pc p; memset(&p, 0, sizeof p); int b = 0, s = 0, c = 0, nt = 0; float tl = 0; long long ds = 0;
        if (sscanf(l, "%83s %d %d %d %f %lld %d", p.key, &s, &b, &c, &tl, &ds, &nt) < 4) continue;
        p.sr = s; p.bits = (short)b; p.ch = (uint8_t)c; p.tlen = tl; p.dsize = ds; p.ntr = (short)nt;
        int k; for (k = 0; k < fm_nprobe; k++) if (!strcmp(fm_probes[k].key, p.key)) break;   /* the newest line wins */
        if (k < FM_NPROBE) { fm_probes[k] = p; if (k == fm_nprobe) fm_nprobe++; }
    }
    fclose(f);
}
static int fm_probe_get(int src, const char *id, struct fm_pc *o)
{
    char k[84]; fm_probe_key(src, id, k, sizeof k); int r = 0;
    pthread_mutex_lock(&fm_pmu);
    for (int i = 0; i < fm_nprobe; i++) if (!strcmp(fm_probes[i].key, k)) { *o = fm_probes[i]; r = 1; break; }
    pthread_mutex_unlock(&fm_pmu);
    return r;
}
static int fm_probe_find(int src, const char *id, int *sr, int *bits)
{
    struct fm_pc p; if (!fm_probe_get(src, id, &p) || p.sr <= 0) return 0;
    *sr = p.sr; *bits = p.bits; return 1;
}
static void fm_probe_store(int src, const char *id, int sr, int bits, int ch, float tlen, long long dsize, int ntr)
{
    char k[84]; fm_probe_key(src, id, k, sizeof k);
    pthread_mutex_lock(&fm_pmu);
    int i; for (i = 0; i < fm_nprobe; i++) if (!strcmp(fm_probes[i].key, k)) break;
    if (fm_probes && i < FM_NPROBE) {
        struct fm_pc *p = &fm_probes[i]; if (i == fm_nprobe) fm_nprobe++;
        fm_cpy(p->key, sizeof p->key, k); p->sr = sr; p->bits = (short)bits; p->ch = (uint8_t)ch; p->tlen = tlen; p->dsize = dsize; p->ntr = (short)ntr;
    }
    FILE *f = fopen(FM_CACHE "/probes.txt", "a"); if (f) { fprintf(f, "%s %d %d %d %.0f %lld %d\n", k, sr, bits, ch, tlen, dsize, ntr); fclose(f); }
    pthread_mutex_unlock(&fm_pmu);
}

/* ------------------------------------------------------------------ network jobs */
enum { NJ_LIST = 1, NJ_ALBUM, NJ_PROBE, NJ_THUMB, NJ_COVER, NJ_JMTEST };
struct fm_job {
    int kind, gen, slot, append; uint8_t src;
    char id[80], url[1600];
    int rc; struct fm_item *items; int n; long found;
    struct fm_album *alb;
    int sr, bits, ch, ntr; float tlen; long long dsize; uint16_t *px;
    struct fm_job *next;
};
static pthread_mutex_t fm_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fm_cv = PTHREAD_COND_INITIALIZER;
static struct fm_job *fm_qhi, *fm_qlo, *fm_done;
static int fm_workers_on, fm_lo_gen;
static volatile int fm_done_flag;

static void fm_job_free(struct fm_job *j)
{
    if (!j) return;
    free(j->items); free(j->px);
    if (j->alb) { free(j->alb->f); free(j->alb->t); free(j->alb); }
    free(j);
}
static struct fm_job *fm_pop(struct fm_job **q) { struct fm_job *j = *q; if (j) *q = j->next; return j; }
static void fm_push_tail(struct fm_job **q, struct fm_job *j) { j->next = 0; while (*q) q = &(*q)->next; *q = j; }
/* lists: archive.org or Jamendo JSON -> items */
static void fm_run_list(struct fm_job *j)
{
    char out[160]; fm_hpath(FM_CACHE, j->url, ".json", out, sizeof out);
    j->rc = fm_get(j->url, out);
    if (j->rc) { j->rc = j->rc == 1 ? -404 : -1; return; }
    char *js = fm_slurp(out, 4 << 20, 0);
    if (!js) { j->rc = -1; return; }
    j->items = calloc(FM_PAGE + 8, sizeof *j->items);
    if (j->items) {
        if (j->src == FM_SRC_JM) j->n = fm_jm_parse_search(js, j->items, FM_PAGE + 8, fm_lossless), j->found = j->n >= FM_PAGE ? 100000 : j->n;
        else j->n = fm_ia_parse_search(js, j->items, FM_PAGE + 8, &j->found);
        if (j->n < 0) { j->rc = j->n; j->n = 0; }
    }
    free(js);
}
static void fm_run_album(struct fm_job *j)
{
    struct fm_album *a = j->alb;
    if (j->src == FM_SRC_JM) {
        char out[160]; fm_hpath(FM_CACHE, j->url, ".json", out, sizeof out);
        if (!fm_fresh(out, 86400) && (j->rc = fm_get(j->url, out))) { j->rc = -1; return; }
        char *js = fm_slurp(out, 4 << 20, 0); if (!js) { j->rc = -1; return; }
        int n = fm_jm_parse_album(js, a); free(js);
        j->rc = n < 0 ? n : 0;
        if (n < 0) unlink(out);
        return;
    }
    char mu[300], fu[300], mo[160], fo[160];
    snprintf(mu, sizeof mu, "https://archive.org/metadata/%s/metadata", j->id);
    snprintf(fu, sizeof fu, "https://archive.org/metadata/%s/files", j->id);
    fm_hpath(FM_CACHE, mu, ".json", mo, sizeof mo); fm_hpath(FM_CACHE, fu, ".json", fo, sizeof fo);
    if (!fm_fresh(mo, 7 * 86400) && fm_get(mu, mo)) { j->rc = -1; return; }
    if (!fm_fresh(fo, 7 * 86400) && fm_get(fu, fo)) { j->rc = -1; return; }
    char *m = fm_slurp(mo, 4 << 20, 0), *f = fm_slurp(fo, 8 << 20, 0);
    if (!m || !f || fm_ia_parse_meta(m, a) < 0 || fm_ia_parse_files(f, a) < 0) { j->rc = -2; unlink(mo); unlink(fo); }
    else j->rc = 0;
    free(m); free(f);
}
/* reads the first KB of a FLAC (hibynet get, killed as soon as STREAMINFO is in) */
static int fm_probe_url(const char *url, int *sr, int *bits, int *ch)
{
    char out[160], part[200]; fm_hpath(FM_CACHE, url, ".probe", out, sizeof out); snprintf(part, sizeof part, "%s.part", out);   /* one per URL: both workers may probe */
    unlink(out); unlink(part);
    char *argv[] = { NETBIN, "get", (char *)url, (char *)out, 0 };
    pid_t pid = fm_spawn(argv, 0, 0); if (pid <= 0) return -1;
    int r = 0, need = 42; double t0 = now_ms(); long long ns;
    uint8_t *b = malloc(65536);
    for (;;) {
        int st; pid_t w = waitpid(pid, &st, WNOHANG);
        const char *pp = w == pid ? out : part;
        struct stat sb;
        if (b && !stat(pp, &sb) && sb.st_size >= need) {
            int fd = open(pp, O_RDONLY);
            if (fd >= 0) { int n = (int)read(fd, b, 65536); close(fd); if (n > 0) { r = fm_flac_info(b, n, sr, bits, ch, &ns, &need); if (need > 65536) r = -1; } }
        }
        if (w == pid) { pid = 0; if (!r) r = -1; break; }
        if (r || now_ms() - t0 > 25000 || (!stat(part, &sb) && sb.st_size > (1 << 20))) break;
        msleep(40);
    }
    if (pid > 0) { kill(pid, SIGTERM); fm_wait(pid); }
    free(b); unlink(out); unlink(part);
    return r == 1 ? 0 : -1;
}
static void fm_run_probe(struct fm_job *j)
{
    j->rc = -1;
    char url[700] = "";
    if (j->src == FM_SRC_JM) fm_cpy(url, sizeof url, j->url);
    else {
        /* archive.org: the files list (cached, the album page reuses it) gives tracks, duration
         * and size; the first lossless file gives the STREAMINFO */
        char fu[300], fo[160]; snprintf(fu, sizeof fu, "https://archive.org/metadata/%s/files", j->id);
        fm_hpath(FM_CACHE, fu, ".json", fo, sizeof fo);
        if (!fm_fresh(fo, 7 * 86400) && fm_get(fu, fo)) return;
        char *f = fm_slurp(fo, 8 << 20, 0); if (!f) return;
        struct fm_album *a = calloc(1, sizeof *a);
        int nt = a ? fm_ia_parse_files(f, a) : -1;
        free(f);
        if (nt > 0) {
            int c = a->ccount[FC_LOSSLESS] ? FC_LOSSLESS : a->ccount[FC_MP3] ? FC_MP3 : FC_OGG;
            j->ntr = nt; j->dsize = a->csize[c];
            for (int t = 0; t < a->nt; t++) j->tlen += a->t[t].len;
            int i = fm_album_first_lossless(a);
            if (i >= 0) { fm_cpy(a->it.id, sizeof a->it.id, j->id); a->it.src = FM_SRC_IA; fm_file_url(a, &a->f[i], url, sizeof url); }
            j->rc = 0;
        }
        if (a) { free(a->f); free(a->t); free(a); }
        if (nt <= 0) return;
    }
    if (url[0] && !fm_probe_url(url, &j->sr, &j->bits, &j->ch)) j->rc = 0;
    else if (j->src == FM_SRC_JM) return;
    fm_probe_store(j->src, j->id, j->sr, j->bits, j->ch, j->tlen, j->dsize, j->ntr);
}
/* thumbnail (FM_TH) or cover (FM_BIG) through hibynet cover-get (+ hibywall for the big one) */
static void fm_run_image(struct fm_job *j)
{
    int big = j->kind == NJ_COVER, s = big ? FM_BIG : 96;
    char raw[160], jpg[170], th[180];
    fm_hpath(big ? FM_CACHE "/cv" : FM_CACHE "/th", j->url, ".raw", raw, sizeof raw);
    uint16_t *px = fm_load_raw(raw, s);
    if (!px) {
        snprintf(jpg, sizeof jpg, "%.*s.jpg", (int)strlen(raw) - 4, raw);
        snprintf(th, sizeof th, "%.*s.thumb.raw", (int)strlen(raw) - 4, raw);
        char sz[8]; snprintf(sz, sizeof sz, "%d", big ? 400 : 160);
        char *argv[] = { NETBIN, "cover-get", j->url, jpg, sz, 0 };
        int rc = fm_wait(fm_spawn(argv, 0, 0));
        if (!rc) {
            if (big) {
                char ss[8]; snprintf(ss, sizeof ss, "%d", FM_BIG);
                char *a2[] = { ASSETS "/hibywall", jpg, raw, ss, ss, 0 };
                if (access(a2[0], X_OK) || fm_wait(fm_spawn(a2, 0, 0))) {       /* no hibywall: the 96 px thumb, up-scaled */
                    uint16_t *t = fm_load_raw(th, 96), *u = t ? malloc(FM_BIG * FM_BIG * 2) : 0;
                    if (u) { fm_upscale565(t, 96, u, FM_BIG); FILE *o = fopen(raw, "wb"); if (o) { fwrite(u, 2, FM_BIG * FM_BIG, o); fclose(o); } }
                    free(t); free(u);
                }
                unlink(th);
            } else rename(th, raw);
            unlink(jpg);
            px = fm_load_raw(raw, s);
        }
    }
    if (!px) { j->rc = -1; return; }
    if (!big) { uint16_t *t = malloc(FM_TH * FM_TH * 2); if (t) fm_scale565(px, 96, t, FM_TH); free(px); px = t; }
    j->px = px; j->rc = px ? 0 : -1;
}
static void fm_run_test(struct fm_job *j)
{
    char out[160]; snprintf(out, sizeof out, FM_CACHE "/jmtest.json");
    if (fm_get(j->url, out)) { j->rc = -1000; return; }
    char *js = fm_slurp(out, 1 << 20, 0);
    j->rc = js ? fm_jm_status(js, 0, 0) : -1000; if (j->rc >= 0) j->rc = 0;
    free(js); unlink(out);
}
static void *fm_worker(void *arg)
{
    int lo = (int)(intptr_t)arg;
    fm_lowprio(lo ? 15 : 10);
    for (;;) {
        pthread_mutex_lock(&fm_mu);
        struct fm_job *j;
        while (!(j = fm_pop(lo ? &fm_qlo : &fm_qhi))) pthread_cond_wait(&fm_cv, &fm_mu);
        int stale = lo && j->gen != fm_lo_gen && j->kind != NJ_COVER;
        pthread_mutex_unlock(&fm_mu);
        if (stale) j->rc = -3;
        else if (access(NETBIN, X_OK)) j->rc = -1;
        else switch (j->kind) {
        case NJ_LIST: fm_run_list(j); break;
        case NJ_ALBUM: fm_run_album(j); break;
        case NJ_PROBE: fm_run_probe(j); break;
        case NJ_THUMB: case NJ_COVER: fm_run_image(j); break;
        case NJ_JMTEST: fm_run_test(j); break;
        }
        pthread_mutex_lock(&fm_mu); fm_push_tail(&fm_done, j); fm_done_flag = 1; pthread_mutex_unlock(&fm_mu);
    }
    return 0;
}
static void fm_workers_start(void)
{
    if (fm_workers_on) return;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&t, &at, fm_worker, (void *)0) && !pthread_create(&t, &at, fm_worker, (void *)1)) fm_workers_on = 1;
    pthread_attr_destroy(&at);
}
static struct fm_job *fm_job_new(int kind) { struct fm_job *j = calloc(1, sizeof *j); if (j) j->kind = kind; return j; }
static void fm_submit(struct fm_job *j, int lo)
{
    if (!j) return;
    pthread_mutex_lock(&fm_mu);
    if (lo) { j->gen = fm_lo_gen; fm_push_tail(&fm_qlo, j); } else fm_push_tail(&fm_qhi, j);
    pthread_cond_broadcast(&fm_cv);
    pthread_mutex_unlock(&fm_mu);
}
/* new visible set: pending thumbnails/probes of the previous lists are dropped */
static void fm_lo_flush(void) { pthread_mutex_lock(&fm_mu); fm_lo_gen++; pthread_mutex_unlock(&fm_mu); }

/* ------------------------------------------------------------------ downloads (persistent worker) */
enum { DK_FILE, DK_COVER, DK_CACHE };
enum { DS_QUEUED, DS_RUN, DS_DONE, DS_FAIL, DS_CANCEL };
struct fm_dl {
    int uid; uint8_t kind, state, cancel, err;     /* err: 1 network, 2 no space, 3 SD */
    char url[260], dest[260];
    long long size, got, total;
    char title[100], artist[72], album[110], year[8], akey[80]; short track;
};
static struct fm_dl *fm_q; static int fm_nq, fm_qcap, fm_uid;
static pthread_mutex_t fm_dmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fm_dcv = PTHREAD_COND_INITIALIZER;
static int fm_dl_on, fm_dl_loaded; static volatile int fm_dl_gen, fm_dl_space;
/* listening session: an album handed to the player, tracks cached one ahead */
struct fm_listen { int on, n, cur, pref, started; char **url, **path; float *len; char title[64]; double t0; int sent_id; };
static struct fm_listen fm_ls;

static void fm_tsv(FILE *f, const char *s) { for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f); fputc('\t', f); }
static void fm_q_save_locked(void)
{
    FILE *f = fopen(FM_DATA "/queue.tmp", "w"); if (!f) return;
    for (int i = 0; i < fm_nq; i++) {
        struct fm_dl *d = &fm_q[i];
        if (d->kind == DK_CACHE || (d->state != DS_QUEUED && d->state != DS_RUN)) continue;
        fprintf(f, "%d\t%lld\t%d\t", d->kind, d->size, d->track);
        fm_tsv(f, d->url); fm_tsv(f, d->dest); fm_tsv(f, d->title); fm_tsv(f, d->artist); fm_tsv(f, d->album); fm_tsv(f, d->year); fm_tsv(f, d->akey);
        fputc('\n', f);
    }
    if (!fclose(f)) rename(FM_DATA "/queue.tmp", FM_DATA "/queue.tsv");
}
static struct fm_dl *fm_q_add_locked(void)
{
    if (fm_nq >= fm_qcap) {
        int nc = fm_qcap ? fm_qcap * 2 : 64; if (nc > 4096) return 0;
        struct fm_dl *n = realloc(fm_q, nc * sizeof *n); if (!n) return 0;
        fm_q = n; fm_qcap = nc;
    }
    struct fm_dl *d = &fm_q[fm_nq++]; memset(d, 0, sizeof *d); d->uid = ++fm_uid;
    return d;
}
/* forget finished entries (keeps the array small) */
static void fm_q_compact_locked(void)
{
    int w = 0;
    for (int i = 0; i < fm_nq; i++) if (fm_q[i].state == DS_QUEUED || fm_q[i].state == DS_RUN || (fm_q[i].kind != DK_CACHE && fm_q[i].state == DS_DONE && w < 400) || fm_q[i].state == DS_FAIL) fm_q[w++] = fm_q[i];
    fm_nq = w;
}
static void fm_q_load(void)
{
    if (fm_dl_loaded) return;
    fm_dl_loaded = 1;
    FILE *f = fopen(FM_DATA "/queue.tsv", "r"); if (!f) return;
    char *l = malloc(2048);
    pthread_mutex_lock(&fm_dmu);
    while (l && fgets(l, 2048, f)) {
        char *fld[10]; int n = 0; char *p = l;
        while (n < 10) { fld[n++] = p; char *t = strchr(p, '\t'); if (!t) break; *t = 0; p = t + 1; }
        if (n < 10) continue;
        struct fm_dl *d = fm_q_add_locked(); if (!d) break;
        d->kind = (uint8_t)atoi(fld[0]); d->size = atoll(fld[1]); d->track = (short)atoi(fld[2]);
        fm_cpy(d->url, sizeof d->url, fld[3]); fm_cpy(d->dest, sizeof d->dest, fld[4]); fm_cpy(d->title, sizeof d->title, fld[5]);
        fm_cpy(d->artist, sizeof d->artist, fld[6]); fm_cpy(d->album, sizeof d->album, fld[7]); fm_cpy(d->year, sizeof d->year, fld[8]);
        fm_cpy(d->akey, sizeof d->akey, fld[9]);
        d->state = DS_QUEUED;
    }
    pthread_mutex_unlock(&fm_dmu);
    free(l); fclose(f);
}
static long long fm_dir_size(const char *dir, int *nfiles)
{
    DIR *d = opendir(dir); if (!d) return 0;
    long long t = 0; struct dirent *e; if (nfiles) *nfiles = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); struct stat s;
        if (!stat(p, &s) && S_ISREG(s.st_mode)) { t += s.st_size; if (nfiles) (*nfiles)++; }
    }
    closedir(d);
    return t;
}
static int fm_in_session(const char *path)
{
    if (!fm_ls.on) return 0;
    for (int i = fm_ls.cur - 1; i <= fm_ls.pref + 1 && i < fm_ls.n; i++) if (i >= 0 && fm_ls.path[i] && !strcmp(fm_ls.path[i], path)) return 1;
    return 0;
}
/* LRU: delete the oldest cached tracks until "need" more bytes fit under the cap */
static void fm_cache_evict(long long need)
{
    long long cap = (long long)fm_cache_mb << 20;
    for (int guard = 0; guard < 200; guard++) {
        DIR *d = opendir(FM_LCACHE); if (!d) return;
        long long tot = 0; time_t old = 0; char victim[400] = ""; struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[400]; snprintf(p, sizeof p, FM_LCACHE "/%s", e->d_name); struct stat s;
            if (stat(p, &s) || !S_ISREG(s.st_mode)) continue;
            tot += s.st_size;
            pthread_mutex_lock(&fm_dmu); int keep = fm_in_session(p); pthread_mutex_unlock(&fm_dmu);
            if (!keep && strstr(p, ".part") == 0 && (!victim[0] || s.st_mtime < old)) { old = s.st_mtime; fm_cpy(victim, sizeof victim, p); }
        }
        closedir(d);
        if (tot + need <= cap || !victim[0]) return;
        unlink(victim);
    }
}
static void fm_send_play_locked(void)
{
    if (!fm_ls.on || fm_ls.n <= 0) return;
    char **pp = calloc(fm_ls.n, sizeof *pp); if (!pp) return;
    for (int i = 0; i < fm_ls.n; i++) { pp[i] = malloc(420); if (pp[i]) fm_player_path(fm_ls.path[i], pp[i], 420); }
    unlink(PC_RES);
    fm_ls.sent_id = pc_list_send((const char *const *)pp, fm_ls.n, fm_ls.cur, fm_ls.title, PC_UNSCANNED | 0xff);
    lg("musica libre: reproducir %d pistas desde %d -> %d", fm_ls.n, fm_ls.cur, fm_ls.sent_id);
    for (int i = 0; i < fm_ls.n; i++) free(pp[i]);
    free(pp);
    fm_ls.started = 1; fm_ls.t0 = now_ms();
    struct utimbuf ut = { time(0), time(0) }; utime(fm_ls.path[fm_ls.cur], &ut);       /* most recently used */
}
/* make sure track i of the session is (being) cached; 1 = already on disk */
static int fm_ls_ensure_locked(int i)
{
    if (!fm_ls.on || i < 0 || i >= fm_ls.n) return 0;
    if (i > fm_ls.pref) fm_ls.pref = i;
    if (!access(fm_ls.path[i], R_OK)) return 1;
    for (int k = 0; k < fm_nq; k++) if (!strcmp(fm_q[k].dest, fm_ls.path[i]) && (fm_q[k].state == DS_QUEUED || fm_q[k].state == DS_RUN)) return 0;
    struct fm_dl *d = fm_q_add_locked(); if (!d) return 0;
    d->kind = DK_CACHE; d->state = DS_QUEUED; d->track = (short)(i + 1);
    fm_cpy(d->url, sizeof d->url, fm_ls.url[i]); fm_cpy(d->dest, sizeof d->dest, fm_ls.path[i]); fm_cpy(d->album, sizeof d->album, fm_ls.title);
    pthread_cond_broadcast(&fm_dcv);
    return 0;
}
/* prefetch: the track after the last fetched one, a minute before the player needs it */
static void fm_ls_tick_locked(void)
{
    if (!fm_ls.on || !fm_ls.started || fm_ls.pref + 1 >= fm_ls.n) return;
    double due = fm_ls.t0; for (int i = fm_ls.cur; i < fm_ls.pref; i++) due += fm_ls.len[i] * 1000;
    if (now_ms() >= due - 60000) fm_ls_ensure_locked(fm_ls.pref + 1);
}
static void fm_tag_file(const struct fm_dl *d)
{
    if (access(TAGBIN, X_OK)) return;
    const char *ext = strrchr(d->dest, '.');
    if (!ext || (strcasecmp(ext, ".flac") && strcasecmp(ext, ".mp3"))) return;       /* hibytag: FLAC and MP3 */
    char t[160], a[110], al[150], tk[16], yr[16];
    snprintf(t, sizeof t, "title=%s", d->title); snprintf(a, sizeof a, "artist=%s", d->artist); snprintf(al, sizeof al, "album=%s", d->album);
    snprintf(tk, sizeof tk, "track=%d", d->track); snprintf(yr, sizeof yr, "year=%s", d->year);
    char *argv[] = { TAGBIN, "set", (char *)d->dest, t, a, al, tk, d->year[0] ? yr : 0, 0 };
    fm_wait(fm_spawn(argv, 0, "HIBYTAG_NODB=1"));
}
static void fm_run_dl(int uid)
{
    pthread_mutex_lock(&fm_dmu);
    struct fm_dl d; int ix = -1;
    for (int i = 0; i < fm_nq; i++) if (fm_q[i].uid == uid) { ix = i; break; }
    if (ix < 0) { pthread_mutex_unlock(&fm_dmu); return; }
    d = fm_q[ix];
    pthread_mutex_unlock(&fm_dmu);
    int ok = 0, err = 0;
    char dir[300]; fm_cpy(dir, sizeof dir, d.dest); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    if (!sd_present()) err = 3;
    else if (fm_mkdir_p(dir)) err = 3;
    else {
        char part[300]; snprintf(part, sizeof part, "%s.part", d.dest); struct stat ps;
        long long have = stat(part, &ps) ? 0 : ps.st_size, need = d.size > have ? d.size - have : 0;
        if (d.kind == DK_CACHE) fm_cache_evict(d.size > 0 ? d.size : 60 << 20);
        long long fr = free_bytes(dir);
        if (fr >= 0 && fr < need + (8LL << 20)) err = 2;
    }
    if (!err && d.kind == DK_COVER) {
        char *argv[] = { NETBIN, "cover-get", d.url, d.dest, "600", 0 };
        ok = !fm_wait(fm_spawn(argv, 0, 0));
        char th[300]; snprintf(th, sizeof th, "%.*s.thumb.raw", (int)strlen(d.dest) - 4, d.dest); unlink(th);
        if (!ok) err = 1;
    } else if (!err) {
        int efd = -1;
        char *argv[] = { NETBIN, "get", d.url, d.dest, 0 };
        pid_t pid = fm_spawn(argv, &efd, "HIBYNET_RESUME=1");
        char line[128]; int ln = 0, killed = 0;
        while (pid > 0) {
            int st; pid_t w = waitpid(pid, &st, WNOHANG);
            char b[256]; ssize_t n;
            while (efd >= 0 && (n = read(efd, b, sizeof b)) > 0)
                for (int k = 0; k < n; k++) {
                    if (b[k] != '\n') { if (ln < (int)sizeof line - 1) line[ln++] = b[k]; continue; }
                    line[ln] = 0; ln = 0;
                    long long g, t;
                    if (sscanf(line, "progress %lld %lld", &g, &t) == 2) {
                        pthread_mutex_lock(&fm_dmu);
                        for (int i = 0; i < fm_nq; i++) if (fm_q[i].uid == uid) { fm_q[i].got = g; if (t > 0) fm_q[i].total = t; }
                        pthread_mutex_unlock(&fm_dmu);
                        fm_dl_gen++;
                    }
                }
            if (w == pid) { ok = WIFEXITED(st) && WEXITSTATUS(st) == 0; pid = 0; break; }
            pthread_mutex_lock(&fm_dmu);
            int cancel = 0; for (int i = 0; i < fm_nq; i++) if (fm_q[i].uid == uid) cancel = fm_q[i].cancel;
            pthread_mutex_unlock(&fm_dmu);
            if (cancel && !killed) { kill(pid, SIGTERM); killed = 1; }
            msleep(120);
        }
        if (efd >= 0) close(efd);
        if (killed) { char part[300]; snprintf(part, sizeof part, "%s.part", d.dest); unlink(part); ok = 0; }
        else if (!ok) err = 1;
        if (ok && d.kind == DK_FILE) fm_tag_file(&d);
        if (ok && d.kind == DK_CACHE) { struct utimbuf ut = { time(0), time(0) }; utime(d.dest, &ut); }
    }
    pthread_mutex_lock(&fm_dmu);
    for (int i = 0; i < fm_nq; i++) if (fm_q[i].uid == uid) {
        struct fm_dl *q = &fm_q[i];
        q->state = ok ? DS_DONE : q->cancel ? DS_CANCEL : DS_FAIL; q->err = (uint8_t)err;
        if (ok && q->total <= 0) { struct stat s; if (!stat(q->dest, &s)) q->total = q->got = s.st_size; }
        if (ok) q->got = q->total;
        if (err == 2) fm_dl_space = 1;
    }
    /* listening: the requested track is ready -> play; then fetch the next one */
    if (ok && d.kind == DK_CACHE && fm_ls.on) {
        if (!fm_ls.started && fm_ls.path[fm_ls.cur] && !strcmp(fm_ls.path[fm_ls.cur], d.dest)) { fm_send_play_locked(); fm_ls_ensure_locked(fm_ls.cur + 1); }
        else fm_ls_tick_locked();
    }
    fm_q_compact_locked();
    fm_q_save_locked();
    fm_dl_gen++;
    pthread_mutex_unlock(&fm_dmu);
}
static void *fm_dl_worker(void *arg)
{
    (void)arg;
    fm_lowprio(19);
    for (;;) {
        pthread_mutex_lock(&fm_dmu);
        int uid = 0;
        for (int pass = 0; pass < 2 && !uid; pass++)
            for (int i = 0; i < fm_nq; i++) if (fm_q[i].state == DS_QUEUED && (pass || fm_q[i].kind == DK_CACHE)) { fm_q[i].state = DS_RUN; uid = fm_q[i].uid; break; }
        if (!uid) {
            fm_ls_tick_locked();
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
            pthread_cond_timedwait(&fm_dcv, &fm_dmu, &ts);
            pthread_mutex_unlock(&fm_dmu);
            continue;
        }
        fm_dl_gen++;
        pthread_mutex_unlock(&fm_dmu);
        if (access(NETBIN, X_OK)) {
            pthread_mutex_lock(&fm_dmu);
            for (int i = 0; i < fm_nq; i++) if (fm_q[i].uid == uid) { fm_q[i].state = DS_FAIL; fm_q[i].err = 1; }
            pthread_mutex_unlock(&fm_dmu); msleep(500); continue;
        }
        fm_run_dl(uid);
    }
    return 0;
}
static void fm_dl_start(void)
{
    if (fm_dl_on) return;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&t, &at, fm_dl_worker, 0)) fm_dl_on = 1;
    pthread_attr_destroy(&at);
}
static void fm_dl_kick(void) { pthread_mutex_lock(&fm_dmu); pthread_cond_broadcast(&fm_dcv); pthread_mutex_unlock(&fm_dmu); }

/* ------------------------------------------------------------------ lists shown */
enum { L_SEARCH, L_F1, L_F2, L_F3, L_N };
struct fm_list { struct fm_item *it; int n, state, rc, page, gen; long found; char url[1600]; };
static struct fm_list fm_l[L_N];
static int fm_list_gen;
static char fm_q_text[96], fm_q_sent[96];
static double fm_q_typed;
static void fm_list_clear(struct fm_list *l)
{
    for (int i = 0; i < l->n; i++) free(l->it[i].thumb);
    free(l->it); l->it = 0; l->n = 0; l->found = 0; l->state = 0; l->rc = 0; l->page = 0;
}
static void fm_apply_probe_cache(struct fm_item *it)
{
    struct fm_pc p;
    if (!fm_probe_get(it->src, it->id, &p)) return;
    if (p.sr > 0) { it->sr = p.sr; it->bits = p.bits; }
    if (p.ntr) { it->ntr = p.ntr; it->tlen = p.tlen; it->dsize = p.dsize; }
    if (it->src == FM_SRC_JM || p.ntr) it->qstate = 2;          /* archive.org entries without the files info: probe again */
}
static int fm_jm_ready(void) { return fm_key[0] != 0; }
static void fm_list_url(int slot, int page, char *o, int n)
{
    if (fm_src == FM_SRC_JM) {
        if (slot == L_SEARCH) fm_jm_search_url(o, n, fm_key, fm_q_sent, fm_lossless, fm_genre, fm_lang, fm_sort, 0, FM_PAGE, (page - 1) * FM_PAGE);
        else fm_jm_search_url(o, n, fm_key, "", fm_lossless, fm_genre, fm_lang, slot == L_F1 ? FSORT_POP : slot == L_F2 ? FSORT_POP : FSORT_NEW, slot == L_F1, 12, 0);
        return;
    }
    if (slot == L_SEARCH) { fm_ia_search_url(o, n, fm_q_sent, fm_coll, fm_lossless, fm_genre, fm_lang, fm_sort, FM_PAGE, page); return; }
    /* featured: popular live concerts (lossless), netlabel favourites, recently added lossless */
    int coll = slot == L_F1 ? FCOL_LIVE : slot == L_F2 ? FCOL_NETLABEL : (fm_coll ? fm_coll : FCOL_ALL);
    if (fm_coll && slot != L_F3) coll = fm_coll;
    fm_ia_search_url(o, n, "", coll, slot == L_F2 ? fm_lossless : 1, fm_genre, fm_lang, slot == L_F3 ? FSORT_NEW : FSORT_POP, 12, 1);
}
static void fm_list_fetch(int slot, int page)
{
    struct fm_list *l = &fm_l[slot];
    if (fm_src == FM_SRC_JM && !fm_jm_ready()) { fm_list_clear(l); return; }
    struct fm_job *j = fm_job_new(NJ_LIST); if (!j) return;
    fm_list_url(slot, page, j->url, sizeof j->url);
    j->src = (uint8_t)fm_src; j->slot = slot; j->append = page > 1;
    if (page <= 1) l->gen = ++fm_list_gen;
    j->gen = l->gen; j->id[0] = 0; snprintf(j->id, sizeof j->id, "%d", page);
    l->state = 1; l->page = page;
    fm_submit(j, 0);
}
/* lists from the disk cache (instant first screen); 1 = something shown */
static int fm_list_preload(int slot)
{
    struct fm_list *l = &fm_l[slot];
    char url[1600], path[160]; fm_list_url(slot, 1, url, sizeof url);
    fm_hpath(FM_CACHE, url, ".json", path, sizeof path);
    char *js = fm_slurp(path, 4 << 20, 0); if (!js) return 0;
    fm_list_clear(l);
    l->it = calloc(FM_PAGE + 8, sizeof *l->it);
    if (l->it) {
        int n = fm_src == FM_SRC_JM ? fm_jm_parse_search(js, l->it, FM_PAGE + 8, fm_lossless) : fm_ia_parse_search(js, l->it, FM_PAGE + 8, &l->found);
        l->n = n > 0 ? n : 0; l->state = l->n ? 2 : 0;
        for (int i = 0; i < l->n; i++) fm_apply_probe_cache(&l->it[i]);
    }
    free(js);
    return l->n > 0;
}
static void fm_refresh_all(void)
{
    fm_lo_flush();
    for (int s = L_F1; s < L_N; s++) {
        if (fm_src == FM_SRC_JM && s == L_F3) { fm_list_clear(&fm_l[s]); continue; }
        if (!fm_list_preload(s)) fm_list_clear(&fm_l[s]);
        fm_list_fetch(s, 1);
    }
    if (fm_q_sent[0]) { fm_list_clear(&fm_l[L_SEARCH]); fm_list_fetch(L_SEARCH, 1); }
}

/* ------------------------------------------------------------------ album page state */
static struct fm_album *fm_alb; static int fm_alb_state, fm_alb_cls, fm_alb_rc;   /* state: 1 loading, 2 ok, 3 error */
static uint16_t *fm_cover; static int fm_cover_state;
static uint8_t *fm_tstate;            /* per track: 1 in library, 2 cached */
static int fm_tstate_gen = -1;
static int fm_alb_from_list = -1, fm_alb_from_ix = -1;
static void fm_alb_free(void)
{
    if (fm_alb) { free(fm_alb->f); free(fm_alb->t); free(fm_alb->it.thumb); free(fm_alb); fm_alb = 0; }
    free(fm_cover); fm_cover = 0; fm_cover_state = 0; free(fm_tstate); fm_tstate = 0; fm_tstate_gen = -1;
}
static const char *fm_alb_artist(const struct fm_album *a) { return a->it.artist[0] ? a->it.artist : tr("Varios artistas"); }
static void fm_dest(const struct fm_album *a, int t, int cls, char *o, int n)
{
    char ar[96], al[120], ti[110];
    fm_safe_name(fm_alb_artist(a), ar, sizeof ar); fm_safe_name(a->it.title, al, sizeof al); fm_safe_name(a->t[t].title, ti, 90);
    const struct fm_file *f = &a->f[a->t[t].f[cls]];
    snprintf(o, n, FM_MUSIC "/%s/%s/%02d - %s.%s", ar, al, a->t[t].no, ti, fm_ext(f->fq));
}
static void fm_cache_path(const struct fm_album *a, int t, int cls, char *o, int n)
{
    char url[600]; fm_file_url(a, &a->f[a->t[t].f[cls]], url, sizeof url);
    char h[64]; snprintf(h, sizeof h, "%08x%08x", st_hash(url), st_hash(a->it.id));
    snprintf(o, n, FM_LCACHE "/%s.%s", h, fm_ext(a->f[a->t[t].f[cls]].fq));
}
/* class actually used for track t (the chosen one, otherwise the best one it has) */
static int fm_track_cls(const struct fm_album *a, int t, int cls)
{
    if (a->t[t].f[cls] >= 0) return cls;
    for (int c = 0; c < FC_N; c++) if (a->t[t].f[c] >= 0) return c;
    return -1;
}
static void fm_akey(const struct fm_album *a, int cls, char *o, int n) { snprintf(o, n, "%c:%s:%d", a->it.src == FM_SRC_JM ? 'j' : 'a', a->it.id, cls); }
static void fm_tstate_update(void)
{
    if (!fm_alb || fm_alb_state != 2) return;
    if (fm_tstate_gen == fm_dl_gen) return;
    fm_tstate_gen = fm_dl_gen;
    free(fm_tstate); fm_tstate = calloc(fm_alb->nt + 1, 1); if (!fm_tstate) return;
    for (int t = 0; t < fm_alb->nt; t++) {
        int c = fm_track_cls(fm_alb, t, fm_alb_cls); if (c < 0) continue;
        char p[400]; fm_dest(fm_alb, t, c, p, sizeof p);
        if (!access(p, R_OK)) { fm_tstate[t] = 1; continue; }
        fm_cache_path(fm_alb, t, c, p, sizeof p);
        if (!access(p, R_OK)) fm_tstate[t] = 2;
    }
}
/* download progress of this album/class: done, total; *frac of the running file */
static int fm_alb_dl(int *done, int *total, double *frac)
{
    *done = *total = 0; *frac = 0;
    if (!fm_alb) return 0;
    char k[80]; fm_akey(fm_alb, fm_alb_cls, k, sizeof k);
    long long g = 0, t = 0; int active = 0;
    pthread_mutex_lock(&fm_dmu);
    for (int i = 0; i < fm_nq; i++) {
        struct fm_dl *d = &fm_q[i]; if (d->kind != DK_FILE || strcmp(d->akey, k)) continue;
        if (d->state == DS_CANCEL) continue;
        (*total)++;
        if (d->state == DS_DONE) (*done)++;
        if (d->state == DS_QUEUED || d->state == DS_RUN) active = 1;
        g += d->state == DS_DONE ? (d->size > 0 ? d->size : d->total) : d->got;
        t += d->size > 0 ? d->size : d->total;
    }
    pthread_mutex_unlock(&fm_dmu);
    *frac = t > 0 ? (double)g / t : 0;
    return active;
}
/* progress of the cache download of track t (listening): -1 none */
static double fm_track_cache_frac(int t)
{
    if (!fm_alb || !fm_ls.on) return -1;
    int c = fm_track_cls(fm_alb, t, fm_alb_cls); if (c < 0) return -1;
    char p[400]; fm_cache_path(fm_alb, t, c, p, sizeof p);
    double r = -1;
    pthread_mutex_lock(&fm_dmu);
    for (int i = 0; i < fm_nq; i++) if (fm_q[i].kind == DK_CACHE && (fm_q[i].state == DS_QUEUED || fm_q[i].state == DS_RUN) && !strcmp(fm_q[i].dest, p)) {
        long long tt = fm_q[i].total > 0 ? fm_q[i].total : fm_q[i].size; r = tt > 0 ? (double)fm_q[i].got / tt : 0; if (fm_q[i].state == DS_QUEUED) r = 0;
    }
    pthread_mutex_unlock(&fm_dmu);
    return r;
}

/* ------------------------------------------------------------------ actions */
static void fm_listen(int t)
{
    struct fm_album *a = fm_alb; if (!a || t < 0 || t >= a->nt) return;
    if (!sd_present()) { toast(tr("Inserta una tarjeta SD")); return; }
    pthread_mutex_lock(&fm_dmu);
    for (int i = 0; i < fm_ls.n; i++) { free(fm_ls.url[i]); free(fm_ls.path[i]); }
    free(fm_ls.url); free(fm_ls.path); free(fm_ls.len); memset(&fm_ls, 0, sizeof fm_ls);
    /* cancel the previous prefetches */
    for (int i = 0; i < fm_nq; i++) if (fm_q[i].kind == DK_CACHE) { if (fm_q[i].state == DS_QUEUED) fm_q[i].state = DS_CANCEL; else if (fm_q[i].state == DS_RUN) fm_q[i].cancel = 1; }
    int n = 0, cur = 0;
    fm_ls.url = calloc(a->nt, sizeof(char *)); fm_ls.path = calloc(a->nt, sizeof(char *)); fm_ls.len = calloc(a->nt, sizeof(float));
    if (fm_ls.url && fm_ls.path && fm_ls.len) for (int k = 0; k < a->nt; k++) {
        int c = fm_track_cls(a, k, fm_alb_cls); if (c < 0) continue;
        char u[600], p[400]; fm_file_url(a, &a->f[a->t[k].f[c]], u, sizeof u);
        fm_dest(a, k, c, p, sizeof p);
        if (access(p, R_OK)) fm_cache_path(a, k, c, p, sizeof p);       /* not in the library: the cache */
        fm_ls.url[n] = strdup(u); fm_ls.path[n] = strdup(p); fm_ls.len[n] = a->t[k].len > 0 ? a->t[k].len : 240;
        if (k == t) cur = n;
        n++;
    }
    fm_ls.n = n; fm_ls.cur = cur; fm_ls.pref = cur; fm_ls.on = n > 0;
    snprintf(fm_ls.title, sizeof fm_ls.title, "%.63s", a->it.title);
    mkdir(FM_MUSIC, 0755); mkdir(FM_LCACHE, 0755);
    if (fm_ls.on && fm_ls_ensure_locked(cur)) { fm_send_play_locked(); fm_ls_ensure_locked(cur + 1); }
    fm_dl_gen++;
    pthread_mutex_unlock(&fm_dmu);
    fm_dl_start(); fm_dl_kick();
}
static void fm_download_album(void)
{
    struct fm_album *a = fm_alb; if (!a || fm_alb_state != 2) return;
    if (!sd_present()) { toast(tr("Inserta una tarjeta SD")); return; }
    int cls = fm_alb_cls;
    if (!a->cdl[cls]) { toast(tr("El artista no permite descargarlo")); return; }
    long long need = 0; int n = 0;
    for (int t = 0; t < a->nt; t++) {
        if (a->t[t].f[cls] < 0) continue;
        char p[400]; fm_dest(a, t, cls, p, sizeof p);
        if (!access(p, R_OK)) continue;
        need += a->f[a->t[t].f[cls]].size; n++;
    }
    if (!n) { toast(tr("Ya está en tu biblioteca")); return; }
    long long fr = free_bytes(SD_ROOT);
    if (fr >= 0 && fr < need + (16LL << 20)) { toast(tr("No hay espacio suficiente en la tarjeta SD")); return; }
    if (need > (1LL << 30)) {
        char sz[24], t[120]; fmt_bytes(need, sz, sizeof sz); snprintf(t, sizeof t, tr("Se descargarán %s"), sz);
        if (!confirm(t, tr("Descargar"))) return;
    }
    char k[80]; fm_akey(a, cls, k, sizeof k);
    pthread_mutex_lock(&fm_dmu);
    char cover[300] = "";
    for (int t = 0; t < a->nt; t++) {
        if (a->t[t].f[cls] < 0) continue;
        char p[400]; fm_dest(a, t, cls, p, sizeof p);
        if (!access(p, R_OK)) continue;
        int dup = 0; for (int i = 0; i < fm_nq; i++) if ((fm_q[i].state == DS_QUEUED || fm_q[i].state == DS_RUN) && !strcmp(fm_q[i].dest, p)) dup = 1;
        if (dup) continue;
        struct fm_dl *d = fm_q_add_locked(); if (!d) break;
        const struct fm_file *f = &a->f[a->t[t].f[cls]];
        d->kind = DK_FILE; d->state = DS_QUEUED; d->size = f->size; d->track = a->t[t].no;
        char u[600]; fm_file_url(a, f, u, sizeof u); fm_cpy(d->url, sizeof d->url, u); fm_cpy(d->dest, sizeof d->dest, p);
        fm_cpy(d->title, sizeof d->title, a->t[t].title); fm_cpy(d->artist, sizeof d->artist, a->t[t].artist[0] ? a->t[t].artist : fm_alb_artist(a));
        fm_cpy(d->album, sizeof d->album, a->it.title); fm_cpy(d->year, sizeof d->year, a->it.year); fm_cpy(d->akey, sizeof d->akey, k);
        if (!cover[0]) { fm_cpy(cover, sizeof cover, p); char *s = strrchr(cover, '/'); if (s) snprintf(s, cover + sizeof cover - s, "/cover.jpg"); }
    }
    if (cover[0] && access(cover, R_OK)) {
        struct fm_dl *d = fm_q_add_locked();
        if (d) {
            d->kind = DK_COVER; d->state = DS_QUEUED; fm_cpy(d->dest, sizeof d->dest, cover); fm_cpy(d->akey, sizeof d->akey, "");
            if (a->it.src == FM_SRC_IA && a->cover[0]) { char e[400]; fm_pathenc(a->cover, e, sizeof e); snprintf(d->url, sizeof d->url, "https://archive.org/download/%s/%s", a->it.id, e); }
            else fm_cpy(d->url, sizeof d->url, a->it.img);
            fm_cpy(d->title, sizeof d->title, "cover.jpg"); fm_cpy(d->album, sizeof d->album, a->it.title);
        }
    }
    fm_q_save_locked(); fm_dl_gen++;
    pthread_cond_broadcast(&fm_dcv);
    pthread_mutex_unlock(&fm_dmu);
    fm_dl_start();
    lg("musica libre: descargar %d pistas (%lld bytes) de %s", n, need, a->it.id);
}
static void fm_cancel_album(void)
{
    if (!fm_alb) return;
    char k[80]; fm_akey(fm_alb, fm_alb_cls, k, sizeof k);
    pthread_mutex_lock(&fm_dmu);
    for (int i = 0; i < fm_nq; i++) if (!strcmp(fm_q[i].akey, k)) { if (fm_q[i].state == DS_QUEUED) fm_q[i].state = DS_CANCEL; else if (fm_q[i].state == DS_RUN) fm_q[i].cancel = 1; }
    fm_q_save_locked(); fm_dl_gen++;
    pthread_mutex_unlock(&fm_dmu);
}
static void fm_play_library(void)
{
    /* everything in the library already: hand the album to the player */
    if (!fm_alb) return;
    int first = -1; for (int t = 0; t < fm_alb->nt && first < 0; t++) if (fm_tstate && fm_tstate[t] == 1) first = t;
    fm_listen(first < 0 ? 0 : first);
}

/* ------------------------------------------------------------------ drawing helpers */
#define FMC_BLUE rgb(10, 132, 255)
#define FMC_GREY rgb(142, 142, 147)
#define FMC_SEP rgb(44, 44, 46)
#define FMC_CARD rgb(28, 28, 30)
#define FMC_FIELD rgb(44, 44, 48)
static void fm_hline(uint16_t *d, int x0, int x1, int y, uint16_t c) { if (y < 0 || y >= H) return; for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) d[y * W + x] = c; }
/* anti-aliased thick line */
static void fm_line(uint16_t *d, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
    int bx0 = (int)fminf(x0, x1) - (int)w - 1, bx1 = (int)fmaxf(x0, x1) + (int)w + 2, by0 = (int)fminf(y0, y1) - (int)w - 1, by1 = (int)fmaxf(y0, y1) + (int)w + 2;
    float dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy; if (L2 < 1e-3f) L2 = 1e-3f;
    for (int y = by0; y < by1; y++) { if (y < 0 || y >= H) continue;
        for (int x = bx0; x < bx1; x++) { if (x < 0 || x >= W) continue;
            float px = x + .5f - x0, py = y + .5f - y0, t = (px * dx + py * dy) / L2; if (t < 0) t = 0; if (t > 1) t = 1;
            float ex = px - t * dx, ey = py - t * dy, dist = sqrtf(ex * ex + ey * ey), cov = w / 2 + .5f - dist;
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            d[y * W + x] = mix565(c, d[y * W + x], (unsigned)(255 * cov));
        } }
}
static void fm_ringd(uint16_t *d, int cx, int cy, float R, float w, float frac, uint16_t on, uint16_t off, double spin)
{
    int r = (int)R + 3;
    for (int y = cy - r; y <= cy + r; y++) { if (y < 0 || y >= H) continue;
        for (int x = cx - r; x <= cx + r; x++) { if (x < 0 || x >= W) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy, rr = sqrtf(dx * dx + dy * dy), cov = w / 2 + .5f - fabsf(rr - R);
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            float ang = atan2f(dx, -dy); if (ang < 0) ang += 2 * (float)M_PI; float u = ang / (2 * (float)M_PI);
            int lit = spin >= 0 ? fmodf(u - (float)fmod(spin / 900.0, 1.0) + 1, 1) < 0.28f : u <= frac;
            d[y * W + x] = mix565(lit ? on : off, d[y * W + x], (unsigned)(255 * cov));
        } }
}
static void fm_glyph_search(uint16_t *d, int cx, int cy, uint16_t c) { fm_ringd(d, cx - 2, cy - 2, 5.5f, 1.8f, 1, c, c, -1); fm_line(d, cx + 2.2f, cy + 2.2f, cx + 6.5f, cy + 6.5f, 2.2f, c); }
static void fm_glyph_x(uint16_t *d, int cx, int cy, float s, uint16_t c) { fm_line(d, cx - s, cy - s, cx + s, cy + s, 1.6f, c); fm_line(d, cx - s, cy + s, cx + s, cy - s, 1.6f, c); }
static void fm_glyph_chev_down(uint16_t *d, int cx, int cy, uint16_t c) { fm_line(d, cx - 3.5f, cy - 1.5f, cx, cy + 2, 1.5f, c); fm_line(d, cx, cy + 2, cx + 3.5f, cy - 1.5f, 1.5f, c); }
static void fm_glyph_back(uint16_t *d, int x, int cy, uint16_t c) { fm_line(d, x + 9, cy - 9, x, cy, 2.6f, c); fm_line(d, x, cy, x + 9, cy + 9, 2.6f, c); }
static void fm_glyph_play(uint16_t *d, int cx, int cy, float s, uint16_t c)
{
    for (int y = (int)(cy - s); y <= (int)(cy + s); y++) for (int x = (int)(cx - s * .7f); x <= (int)(cx + s); x++) {
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        float u = (x + .5f - (cx - s * .7f)) / (s * 1.7f), half = s * (1 - u), dy = fabsf(y + .5f - cy), cov = half - dy + .5f;
        if (u < 0 || u > 1 || cov <= 0) continue; if (cov > 1) cov = 1;
        d[y * W + x] = mix565(c, d[y * W + x], (unsigned)(255 * cov));
    }
}
static void fm_glyph_check(uint16_t *d, int cx, int cy, uint16_t c) { fm_line(d, cx - 6, cy, cx - 2, cy + 4, 2.2f, c); fm_line(d, cx - 2, cy + 4, cx + 6, cy - 5, 2.2f, c); }
static void fm_glyph_dl(uint16_t *d, int cx, int cy, uint16_t c)
{
    fm_line(d, cx, cy - 9, cx, cy + 3, 2, c); fm_line(d, cx - 5, cy - 2, cx, cy + 3, 2, c); fm_line(d, cx + 5, cy - 2, cx, cy + 3, 2, c);
    fm_line(d, cx - 9, cy + 3, cx - 9, cy + 8, 2, c); fm_line(d, cx - 9, cy + 8, cx + 9, cy + 8, 2, c); fm_line(d, cx + 9, cy + 8, cx + 9, cy + 3, 2, c);
}
static void fm_glyph_more(uint16_t *d, int cx, int cy, uint16_t c) { fm_ringd(d, cx, cy, 10, 1.7f, 1, c, c, -1); for (int k = -1; k <= 1; k++) fill_circle(d, cx + k * 5.f, (float)cy, 1.7f, c, 255); }
static void fm_glyph_bars(uint16_t *d, int cx, int cy, double t, uint16_t c)
{
    for (int k = 0; k < 3; k++) { float h = 4 + 5 * (float)fabs(sin(t / 260.0 + k * 1.7)); fill_rrect(d, cx - 7 + k * 5, cy + 6 - (int)h, 3, (int)h, 1, c, 255); }
}
/* word-wrapped text, at most "lines" lines (the last one with "…"); returns the height. Widths
 * are measured on whole prefixes (tw_n rounds per call), only at word ends */
static int fm_wrap(uint16_t *d, int x, int y, int w, const char *s, int size, uint16_t col, int lh, int lines)
{
    int n = 0;
    while (*s && n < lines) {
        while (*s == ' ') s++;
        if (!*s) break;
        int len = (int)strlen(s), cut;
        if (tw_n(s, size, len) <= w) cut = len;
        else {
            int lastfit = -1;
            for (int k = 1; k <= len; k++) if (k == len || s[k] == ' ') { if (tw_n(s, size, k) <= w) lastfit = k; else break; }
            if (lastfit > 0) cut = lastfit;
            else {                                       /* one long word (or CJK): by characters */
                int c = 0;
                for (int k = 1; k <= len; k++) { if (k < len && ((unsigned char)s[k] & 0xC0) == 0x80) continue; if (tw_n(s, size, k) > w) break; c = k; }
                cut = c;
                if (cut <= 0) { cut = 1; while (cut < len && ((unsigned char)s[cut] & 0xC0) == 0x80) cut++; }
            }
        }
        int vis = y + n * lh > 0 && y + n * lh - size < H;
        if (cut >= len || n == lines - 1) { if (vis) draw_text(d, x, y + n * lh, s, size, col, w, 30, H); n++; break; }
        if (vis) { char line[400]; int c2 = cut < 399 ? cut : 399; memcpy(line, s, c2); line[c2] = 0; draw_text(d, x, y + n * lh, line, size, col, 0, 30, H); }
        s += cut; n++;
    }
    return n * lh;
}
static void fm_thumb(uint16_t *d, const uint16_t *px, int s, int x, int y, int r)
{
    if (px) mu_blit(d, px, s, x, y, r, 255);
    else { fill_rrect(d, x, y, s, s, (float)r, rgb(48, 48, 52), 255); for (int k = 0; k < 2; k++) fill_circle(d, x + s / 2.f, y + s / 2.f, s / (k ? 9.f : 4.f), k ? rgb(48, 48, 52) : rgb(72, 72, 78), 255); }
}
/* quality badge; returns its width */
static int fm_badge(uint16_t *d, int x, int y, const char *t, int kind)
{
    uint16_t bg = kind == 2 ? rgb(66, 52, 8) : kind == 1 ? rgb(16, 58, 32) : rgb(48, 48, 52);
    uint16_t fg = kind == 2 ? rgb(255, 204, 0) : kind == 1 ? rgb(48, 209, 88) : rgb(190, 190, 196);
    int w = tw_n(t, 11, -1) + 10;
    fill_rrect(d, x, y, w, 17, 4, bg, 255);
    draw_text(d, x + 5, y + 13, t, 11, fg, 0, 0, H);
    return w;
}
/* badges of an item: lossless (with bits/rate when known), best MP3, OGG; returns the x after */
static int fm_item_badges(uint16_t *d, int x, int y, const struct fm_item *it, int maxx)
{
    char q[32];
    if (it->fq & FQ_LOSSLESS) {
        unsigned lq = (it->fq & FQ_FLAC24) ? FQ_FLAC24 : (it->fq & FQ_FLAC) ? FQ_FLAC : FQ_WAV;
        fm_qlabel(lq, it->sr, it->bits, 0, q, sizeof q);
        x += fm_badge(d, x, y, q, fm_hires(lq, it->sr, it->bits) ? 2 : 1) + 5;
    }
    if (it->fq & FQ_ANYMP3 && x < maxx - 50) { fm_qlabel(it->mp3k ? FQ_MP3 : FQ_MP3VBR, 0, 0, it->mp3k, q, sizeof q); x += fm_badge(d, x, y, q, 0) + 5; }
    else if ((it->fq & FQ_OGG) && !(it->fq & FQ_LOSSLESS) && x < maxx - 40) x += fm_badge(d, x, y, "OGG", 0) + 5;
    return x;
}

/* ------------------------------------------------------------------ hits */
enum { FH_ROW = 1, FH_CHIP, FH_BAR, FH_CLEAR, FH_CANCEL, FH_DLS, FH_SET, FH_BACK, FH_SEG, FH_DLBTN, FH_TRACK, FH_DLITEM, FH_SETROW, FH_PICK, FH_KEYROW, FH_RETRY, FH_CLRDONE };
struct fm_hit { short x0, y0, x1, y1; int kind, idx; };
static struct fm_hit fm_hits[160]; static int fm_nhit, fm_rec;
static int fm_press_kind, fm_press_idx = -1;
static void fm_hit(int x0, int y0, int x1, int y1, int kind, int idx)
{
    if (!fm_rec || fm_nhit >= (int)(sizeof fm_hits / sizeof fm_hits[0])) return;
    if (y1 <= 30 || y0 >= H) return;
    fm_hits[fm_nhit++] = (struct fm_hit){ (short)x0, (short)(y0 < 30 ? 30 : y0), (short)x1, (short)(y1 > H ? H : y1), kind, idx };
}
static int fm_hit_at(int x, int y, int *idx)
{
    for (int i = fm_nhit - 1; i >= 0; i--) if (x >= fm_hits[i].x0 && x < fm_hits[i].x1 && y >= fm_hits[i].y0 && y < fm_hits[i].y1) { *idx = fm_hits[i].idx; return fm_hits[i].kind; }
    *idx = -1; return 0;
}
static int fm_pressed(int kind, int idx) { return fm_press_kind == kind && fm_press_idx == idx; }

/* ------------------------------------------------------------------ pages */
enum { FP_ROOT, FP_ALBUM, FP_DL, FP_SET, FP_PICK };
static int fm_stack[6], fm_sp;                 /* fm_stack[fm_sp] = current page */
static float fm_pos[5], fm_vel; static int fm_ch[5];
static int fm_top_of(int pg);
static int fm_editing; static float fm_edit_e;   /* search bar active (0..1 animation) */
static float fm_chip_x; static int fm_chips_w;
/* pick page */
static const char *fm_pick_title; static const char *fm_pick_opt[24]; static int fm_pick_n, fm_pick_cur, fm_pick_what;
enum { PK_SRC, PK_COLL, PK_GENRE, PK_LANG, PK_SORT, PK_CACHE };
static int fm_test_state;            /* key test: 0 -, 1 testing, 2 ok, 3 invalid, 4 no connection */
static long long fm_cache_used; static int fm_cache_files;

static void fm_chrome_status(uint16_t *d) { memcpy(d, fr1, 30 * W * 2); }
/* nav bar of the pushed pages: back chevron + label, centred title */
static void fm_navbar(uint16_t *d, const char *back, const char *title, float scrolled)
{
    for (int y = 30; y < 74; y++) for (int x = 0; x < W; x++) d[y * W + x] = 0;
    uint16_t c = fm_pressed(FH_BACK, 0) ? rgb(5, 66, 128) : FMC_BLUE;
    fm_glyph_back(d, 12, 52, c);
    draw_text(d, 28, 58, back, 17, c, 110, 0, H);
    fm_hit(0, 30, 150, 74, FH_BACK, 0);
    if (title && scrolled > 0.01f) {
        /* centred like iOS, but never over the back label */
        int bw = tw_n(back, 17, -1); if (bw > 110) bw = 110;
        int x0 = 28 + bw + 12, avail = W - 12 - x0, tw = tw_n(title, 16, -1); if (tw > avail) tw = avail;
        int tx = (W - tw) / 2; if (tx < x0) tx = x0;
        uint16_t tc = mix565(0xFFFF, 0, (unsigned)(255 * (scrolled > 1 ? 1 : scrolled)));
        draw_text(d, tx, 58, title, 16, tc, avail, 0, H);
    }
    if (scrolled > 0.01f) fm_hline(d, 0, W, 73, FMC_SEP);
}
static const char *fm_src_name(int s) { return s == FM_SRC_JM ? "Jamendo" : "Internet Archive"; }
static const char *fm_coll_name(int c) { static const char *const n[FCOL_N] = { "Todas las colecciones", "Conciertos en directo", "Netlabels", "Comunidad" }; return tr(n[c]); }
static const char *fm_sort_name(int s) { return tr(s == FSORT_NEW ? "Recientes" : s == FSORT_REL ? "Relevancia" : "Populares"); }

/* ---- root: title, search bar, chips, results / featured */
#define FM_BAR_Y0 82
#define FM_BAR_Y1 36
static int fm_bar_y(void) { return FM_BAR_Y0 + (int)((FM_BAR_Y1 - FM_BAR_Y0) * fm_edit_e); }
static int fm_chips_y(void) { return fm_bar_y() + 44; }
static int fm_list_y(void) { return fm_chips_y() + 38; }
struct fm_chip { char t[64]; int on, drop, what; };
static int fm_chip_list(struct fm_chip *c)
{
    int n = 0;
    snprintf(c[n].t, sizeof c[n].t, "%s", fm_src_name(fm_src)); c[n].on = 0; c[n].drop = 1; c[n++].what = PK_SRC;
    snprintf(c[n].t, sizeof c[n].t, "%s", tr("Sin pérdida")); c[n].on = fm_lossless; c[n].drop = 0; c[n++].what = -1;
    if (fm_src == FM_SRC_IA) { snprintf(c[n].t, sizeof c[n].t, "%s", fm_coll ? fm_coll_name(fm_coll) : tr("Colección")); c[n].on = fm_coll != 0; c[n].drop = 1; c[n++].what = PK_COLL; }
    snprintf(c[n].t, sizeof c[n].t, "%s", fm_genre >= 0 ? tr(FM_GENRES[fm_genre].key) : tr("Género")); c[n].on = fm_genre >= 0; c[n].drop = 1; c[n++].what = PK_GENRE;
    snprintf(c[n].t, sizeof c[n].t, "%s", fm_lang >= 0 ? FM_LANGS[fm_lang].name : tr("Idioma")); c[n].on = fm_lang >= 0; c[n].drop = 1; c[n++].what = PK_LANG;
    snprintf(c[n].t, sizeof c[n].t, "%s", fm_sort_name(fm_sort)); c[n].on = 0; c[n].drop = 1; c[n++].what = PK_SORT;
    return n;
}
static void fm_draw_chips(uint16_t *d, int y)
{
    struct fm_chip c[8]; int n = fm_chip_list(c);
    int x = 16 - (int)fm_chip_x;
    for (int i = 0; i < n; i++) {
        int w = tw_n(c[i].t, 13, -1) + 24 + (c[i].drop ? 12 : 0);
        int pr = fm_pressed(FH_CHIP, i);
        uint16_t bg = c[i].on ? (pr ? rgb(6, 80, 160) : rgb(10, 100, 200)) : pr ? rgb(64, 64, 70) : FMC_FIELD;
        fill_rrect(d, x, y, w, 30, 15, bg, 255);
        draw_text(d, x + 12, y + 20, c[i].t, 13, c[i].on ? 0xFFFF : rgb(235, 235, 240), 0, 0, H);
        if (c[i].drop) fm_glyph_chev_down(d, x + w - 15, y + 15, c[i].on ? 0xFFFF : FMC_GREY);
        fm_hit(x, y - 4, x + w, y + 34, FH_CHIP, i);
        x += w + 8;
    }
    fm_chips_w = x + (int)fm_chip_x - 8 + 16;
}
static void fm_spinner_at(uint16_t *d, int cx, int cy) { spn_draw(d, cx, cy, spn_phase(now_ms()), 8, 0, H); }
static void fm_row(uint16_t *d, int y, const struct fm_item *it, int last, int hit_idx)
{
    if (y > H || y + FM_RH < 0) return;
    if (fm_pressed(FH_ROW, hit_idx)) for (int yy = y; yy < y + FM_RH && yy < H; yy++) if (yy >= 0) fm_hline(d, 0, W, yy, rgb(38, 38, 42));
    fm_thumb(d, it->thumb, FM_TH, 16, y + 10, 8);
    draw_text(d, 84, y + 26, it->title, 15, 0xFFFF, W - 84 - 14, 0, H);
    char sub[200], dur[32] = "";
    if (it->tlen > 0) { int m = (int)(it->tlen / 60 + .5f); if (m >= 60) snprintf(dur, sizeof dur, "%d h %02d min", m / 60, m % 60); else snprintf(dur, sizeof dur, "%d min", m < 1 ? 1 : m); }
    snprintf(sub, sizeof sub, "%s%s%s%s%s", it->artist, it->artist[0] && it->year[0] ? " · " : "", it->year, dur[0] && (it->artist[0] || it->year[0]) ? " · " : "", dur);
    draw_text(d, 84, y + 44, sub, 13, FMC_GREY, W - 84 - 14, 0, H);
    int x = fm_item_badges(d, 84, y + 51, it, W - 14);
    if (it->dsize > 0 && x < W - 56) { char sz[24]; fmt_bytes(it->dsize, sz, sizeof sz); draw_text(d, x + 2, y + 64, sz, 12, rgb(110, 110, 116), W - 14 - x, 0, H); }
    if (!last) fm_hline(d, 84, W, y + FM_RH - 1, FMC_SEP);
    fm_hit(0, y, W, y + FM_RH, FH_ROW, hit_idx);
}
/* thumbnails / probes for the rows on screen (queued once) */
static void fm_want(struct fm_item *it)
{
    if (!it->thumb_state && it->img[0]) {
        struct fm_job *j = fm_job_new(NJ_THUMB);
        if (j) { it->thumb_state = 1; fm_cpy(j->url, sizeof j->url, it->img); fm_cpy(j->id, sizeof j->id, it->id); j->src = it->src; fm_submit(j, 1); }
    }
    if (!it->qstate && (it->src == FM_SRC_IA ? !it->ntr : ((it->fq & FQ_FLAC) && !it->sr && it->probe[0]))) {
        struct fm_job *j = fm_job_new(NJ_PROBE);
        if (j) { it->qstate = 1; fm_cpy(j->id, sizeof j->id, it->id); fm_cpy(j->url, sizeof j->url, it->probe); j->src = it->src; fm_submit(j, 1); }
    }
}
static int fm_status_line(uint16_t *d, int y, const struct fm_list *l)
{
    const char *m = 0;
    if (l->state == 3) m = l->rc == -5 ? tr("La clave de Jamendo no es válida") : tr("Sin conexión");
    else if (l->state == 2 && !l->n) m = tr("Sin resultados");
    if (l->state == 1 && !l->n) { fm_spinner_at(d, W / 2, y + 40); return 90; }
    if (!m) return 0;
    int w = tw_n(m, 15, -1); draw_text(d, (W - w) / 2, y + 40, m, 15, FMC_GREY, W - 32, 0, H);
    if (l->state == 3) {
        int key = l->rc == -5;                             /* bad Jamendo key: the button opens the settings */
        const char *r = key ? tr("Ajustes") : tr("Reintentar"); int rw = tw_n(r, 15, -1);
        draw_text(d, (W - rw) / 2, y + 70, r, 15, fm_pressed(key ? FH_KEYROW : FH_RETRY, 0) ? rgb(5, 66, 128) : FMC_BLUE, 0, 0, H);
        fm_hit((W - rw) / 2 - 20, y + 50, (W + rw) / 2 + 20, y + 82, key ? FH_KEYROW : FH_RETRY, 0);
        return 96;
    }
    return 70;
}
static void fm_render_root(uint16_t *d)
{
    int top = fm_list_y(), y = top - (int)fm_pos[FP_ROOT], y0 = y;
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    double t = now_ms();
    if (fm_src == FM_SRC_JM && !fm_jm_ready()) {
        /* no key: a single row that leads to the settings */
        int cy = y + 8;
        fill_rrect(d, 16, cy, W - 32, 64, 12, fm_pressed(FH_KEYROW, 0) ? rgb(44, 44, 48) : FMC_CARD, 255);
        draw_text(d, 32, cy + 27, tr("Añade tu clave gratuita de Jamendo"), 15, 0xFFFF, W - 80, 0, H);
        draw_text(d, 32, cy + 47, "devportal.jamendo.com", 13, FMC_GREY, W - 80, 0, H);
        fm_line(d, W - 38, cy + 26, W - 32, cy + 32, 2, FMC_GREY); fm_line(d, W - 32, cy + 32, W - 38, cy + 38, 2, FMC_GREY);
        fm_hit(16, cy, W - 16, cy + 64, FH_KEYROW, 0);
        y += 90;
    } else if (fm_q_sent[0]) {
        struct fm_list *l = &fm_l[L_SEARCH];
        if (l->n) {
            char c[64]; long nf = l->found > 0 ? l->found : (long)l->n;
            if (l->found > l->n || fm_src == FM_SRC_IA) { if (nf == 1) snprintf(c, sizeof c, "%s", tr("1 resultado")); else snprintf(c, sizeof c, tr("%ld resultados"), nf); } else c[0] = 0;
            if (c[0]) draw_text(d, 16, y + 18, c, 12, FMC_GREY, 0, 0, H);
            y += 28;
        }
        for (int i = 0; i < l->n; i++, y += FM_RH) {
            if (y + FM_RH > top - 40 && y < H + 40) fm_want(&l->it[i]);
            fm_row(d, y, &l->it[i], i == l->n - 1, (L_SEARCH << 16) | i);
        }
        y += fm_status_line(d, y, l);
        if (l->state == 1 && l->n) { fm_spinner_at(d, W / 2, y + 22); y += 44; }
    } else {
        static const char *const sec_ia[3] = { "Conciertos populares (sin pérdida)", "Favoritos de los netlabels", "Añadidos recientemente" };
        static const char *const sec_jm[3] = { "Destacados de la semana", "Populares en Jamendo", "" };
        for (int s = L_F1; s < L_N; s++) {
            struct fm_list *l = &fm_l[s];
            const char *h = fm_src == FM_SRC_JM ? sec_jm[s - L_F1] : sec_ia[s - L_F1];
            if (!h[0]) continue;
            if (s > L_F1 && fm_l[L_F1].state == 3 && fm_l[L_F1].rc == -5) continue;     /* bad key: said once */
            y += fm_wrap(d, 16, y + 26, W - 32, tr(h), 20, 0xFFFF, 25, 2) + 13;
            for (int i = 0; i < l->n; i++, y += FM_RH) {
                if (y + FM_RH > top - 40 && y < H + 40) fm_want(&l->it[i]);
                fm_row(d, y, &l->it[i], i == l->n - 1, (s << 16) | i);
            }
            y += fm_status_line(d, y, l) + 12;
        }
        y += 8;
        const char *note = fm_src == FM_SRC_JM ? tr("FM_NOTE_JM") : tr("FM_NOTE_IA");
        y += draw_wrapped(d, 16, y, W - 32, note, 12, rgb(110, 110, 116), 16, 1) + 24;
    }
    fm_ch[FP_ROOT] = y - y0 + 30;
    /* header (over the list) */
    for (int yy = 30; yy < top && yy < H; yy++) memset(d + yy * W, 0, W * 2);
    fm_chrome_status(d);
    float e = fm_edit_e;
    if (e < 0.99f) {
        int ty = 72 - (int)(40 * e);
        uint16_t tc = mix565(0xFFFF, 0, (unsigned)(255 * (1 - e)));
        draw_text(d, 16, ty, tr("Música libre"), 28, tc, W - 110, 30, H);
        /* downloads (with a ring while something downloads) and the settings */
        int dn = 0, tot = 0; double fr = 0; (void)fr;
        pthread_mutex_lock(&fm_dmu);
        long long g = 0, tt = 0;
        for (int i = 0; i < fm_nq; i++) if (fm_q[i].kind != DK_CACHE && (fm_q[i].state == DS_QUEUED || fm_q[i].state == DS_RUN)) { tot++; g += fm_q[i].got; tt += fm_q[i].size > 0 ? fm_q[i].size : fm_q[i].total; }
        pthread_mutex_unlock(&fm_dmu);
        (void)dn;
        uint16_t ic = fm_pressed(FH_DLS, 0) ? rgb(5, 66, 128) : FMC_BLUE;
        if (tot) { fm_ringd(d, W - 66, ty - 10, 13, 2.4f, tt > 0 ? (float)g / tt : 0, FMC_BLUE, rgb(58, 58, 62), -1); fm_line(d, W - 66, ty - 15, W - 66, ty - 5, 2, ic); fm_line(d, W - 70, ty - 9, W - 66, ty - 5, 2, ic); fm_line(d, W - 62, ty - 9, W - 66, ty - 5, 2, ic); }
        else fm_glyph_dl(d, W - 66, ty - 10, ic);
        fm_glyph_more(d, W - 28, ty - 10, fm_pressed(FH_SET, 0) ? rgb(5, 66, 128) : FMC_BLUE);
        if (e < 0.5f) { fm_hit(W - 90, ty - 34, W - 46, ty + 10, FH_DLS, 0); fm_hit(W - 46, ty - 34, W, ty + 10, FH_SET, 0); }
    }
    int by = fm_bar_y(), bx1 = W - 16 - (int)(70 * e);
    fill_rrect(d, 16, by, bx1 - 16, 36, 10, FMC_FIELD, 255);
    fm_glyph_search(d, 32, by + 18, FMC_GREY);
    const char *q = fm_q_text;
    if (q[0]) draw_text(d, 46, by + 24, q, 16, 0xFFFF, bx1 - 46 - 34, 0, H);
    else draw_text(d, 46, by + 24, tr("Artistas, álbumes, conciertos"), 16, FMC_GREY, bx1 - 46 - 14, 0, H);
    if (fm_editing && ((int)(t / 530) & 1) == 0) {
        int cx = 46 + tw_n(q, 16, -1); if (cx > bx1 - 34) cx = bx1 - 34;
        for (int yy = by + 8; yy < by + 28; yy++) for (int xx = cx; xx < cx + 2; xx++) d[yy * W + xx] = FMC_BLUE;
    }
    fm_hit(16, by - 4, bx1 - 30, by + 40, FH_BAR, 0);
    if (q[0]) { fill_circle(d, bx1 - 16, by + 18, 8, rgb(99, 99, 104), 255); fm_glyph_x(d, bx1 - 16, by + 18, 3, FMC_FIELD); fm_hit(bx1 - 34, by - 4, bx1 + 2, by + 40, FH_CLEAR, 0); }
    if (e > 0.01f) {
        uint16_t cc = mix565(fm_pressed(FH_CANCEL, 0) ? rgb(5, 66, 128) : FMC_BLUE, 0, (unsigned)(255 * e));
        draw_text(d, bx1 + 10, by + 24, tr("Cancelar"), 16, cc, 0, 0, H);
        if (e > 0.5f) fm_hit(bx1, by - 4, W, by + 40, FH_CANCEL, 0);
    }
    fm_draw_chips(d, fm_chips_y());
    if (fm_pos[FP_ROOT] > 2) fm_hline(d, 0, W, top - 1, FMC_SEP);
    draw_toast(d);
}

/* ---- album page */
static struct np_info fm_np;                  /* what the player plays (album page: bars on that track) */
static double fm_np_t;
static void fm_render_album(uint16_t *d)
{
    struct fm_album *a = fm_alb;
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int top = 74, y = top + 10 - (int)fm_pos[FP_ALBUM], y0 = y;
    if (!a) { fm_navbar(d, tr("Música libre"), "", 0); return; }
    /* cover + title */
    if (fm_cover) mu_blit(d, fm_cover, FM_BIG, 16, y, 10, 255);
    else if (a->it.thumb) { static uint16_t up[FM_BIG * FM_BIG]; static const uint16_t *upfor; if (upfor != a->it.thumb) { fm_upscale565(a->it.thumb, FM_TH, up, FM_BIG); upfor = a->it.thumb; } mu_blit(d, up, FM_BIG, 16, y, 10, 255); }
    else fm_thumb(d, 0, FM_BIG, 16, y, 10);
    int tx = 16 + FM_BIG + 14, tw = W - tx - 14;
    int ty = y + 20;
    ty += fm_wrap(d, tx, ty, tw, a->it.title, 16, 0xFFFF, 20, 4);
    ty += 2; draw_text(d, tx, ty + 2, fm_alb_artist(a), 14, FMC_BLUE, tw, 0, H); ty += 20;
    char yv[160]; snprintf(yv, sizeof yv, "%s%s%s", a->it.year, a->it.year[0] && a->venue[0] ? " · " : "", a->venue);
    if (yv[0]) { ty += fm_wrap(d, tx, ty, tw, yv, 12, FMC_GREY, 16, 2); }
    char src[64]; snprintf(src, sizeof src, "%s", a->it.src == FM_SRC_JM ? "Jamendo" : a->it.etree ? "Live Music Archive" : "Internet Archive");
    draw_text(d, tx, ty + 2, src, 12, rgb(110, 110, 116), tw, 0, H);
    y += FM_BIG + 18;
    if (fm_alb_state == 1) { fm_spinner_at(d, W / 2, y + 40); y += 90; }
    else if (fm_alb_state == 3) {
        const char *m = fm_alb_rc == -5 ? tr("La clave de Jamendo no es válida") : fm_alb_rc == -2 ? tr("No se pudo leer el álbum") : tr("Sin conexión");
        int w = tw_n(m, 15, -1); draw_text(d, (W - w) / 2, y + 30, m, 15, FMC_GREY, W - 32, 0, H);
        const char *r = tr("Reintentar"); int rw = tw_n(r, 15, -1);
        draw_text(d, (W - rw) / 2, y + 62, r, 15, fm_pressed(FH_RETRY, 0) ? rgb(5, 66, 128) : FMC_BLUE, 0, 0, H);
        fm_hit((W - rw) / 2 - 20, y + 42, (W + rw) / 2 + 20, y + 74, FH_RETRY, 0);
        y += 100;
    } else if (fm_alb_state == 2) {
        /* format: segmented control */
        int segs[FC_N], ns = 0; for (int c = 0; c < FC_N; c++) if (a->ccount[c]) segs[ns++] = c;
        if (ns) {
            int sw = (W - 32) / ns;
            fill_rrect(d, 16, y, W - 32, 34, 9, FMC_FIELD, 255);
            for (int k = 0; k < ns; k++) {
                int c = segs[k], sx = 16 + k * sw;
                char q[32];
                unsigned cq = a->cq[c];
                if (c == FC_LOSSLESS) { unsigned lq = (cq & FQ_FLAC24) ? FQ_FLAC24 : (cq & FQ_FLAC) ? FQ_FLAC : FQ_WAV; fm_qlabel(lq, a->it.sr, a->it.bits, 0, q, sizeof q); }
                else if (c == FC_MP3) fm_qlabel((cq & FQ_MP3) && !(cq & FQ_MP3VBR) ? FQ_MP3 : (cq & FQ_MP3) && a->ckbps[c] >= 256 ? FQ_MP3 : FQ_MP3VBR, 0, 0, a->ckbps[c], q, sizeof q);
                else snprintf(q, sizeof q, "OGG");
                if (c == fm_alb_cls) fill_rrect(d, sx + 2, y + 2, sw - 4, 30, 7, rgb(99, 99, 104), 255);
                else if (k && segs[k - 1] != fm_alb_cls) for (int yy = y + 9; yy < y + 25; yy++) d[yy * W + sx] = rgb(72, 72, 76);
                int qw = tw_n(q, 13, -1); if (qw > sw - 8) qw = sw - 8;
                draw_text(d, sx + (sw - qw) / 2, y + 22, q, 13, 0xFFFF, sw - 8, 0, H);
                fm_hit(sx, y - 4, sx + sw, y + 38, FH_SEG, c);
            }
            y += 46;
            char info[120], sz[24]; fmt_bytes(a->csize[fm_alb_cls], sz, sizeof sz);
            snprintf(info, sizeof info, tr("%d pistas · %s%s"), a->ccount[fm_alb_cls], a->it.est ? "≈" : "", sz);
            if (a->ccount[fm_alb_cls] < a->nt) { char m[60]; snprintf(m, sizeof m, tr(" (%d de %d)"), a->ccount[fm_alb_cls], a->nt); strncat(info, m, sizeof info - strlen(info) - 1); }
            draw_text(d, 16, y + 4, info, 13, FMC_GREY, W - 32, 0, H);
            y += 18;
            /* download button */
            int done, tot; double fr; int active = fm_alb_dl(&done, &tot, &fr);
            int inlib = 1; for (int t = 0; t < a->nt; t++) if (a->t[t].f[fm_alb_cls] >= 0 && (!fm_tstate || fm_tstate[t] != 1)) inlib = 0;
            int pr = fm_pressed(FH_DLBTN, 0);
            if (active) {
                fill_rrect(d, 16, y, W - 32, 44, 12, FMC_FIELD, 255);
                int fw = (int)((W - 32) * (fr > 1 ? 1 : fr)); if (fw > 12) fill_rrect(d, 16, y, fw, 44, 12, rgb(10, 70, 140), 255);
                char t[96]; snprintf(t, sizeof t, tr("Descargando %d de %d · %d %%"), done + 1 > tot ? tot : done + 1, tot, (int)(fr * 100));
                draw_text(d, 30, y + 28, t, 14, 0xFFFF, W - 130, 0, H);
                const char *cn = tr("Cancelar"); int cw = tw_n(cn, 14, -1);
                draw_text(d, W - 30 - cw, y + 28, cn, 14, pr ? rgb(255, 140, 130) : rgb(255, 105, 97), 0, 0, H);
            } else if (inlib) {
                fill_rrect(d, 16, y, W - 32, 44, 12, pr ? rgb(6, 90, 180) : FMC_BLUE, 255);
                const char *t = tr("Reproducir"); int w = tw_n(t, 16, -1);
                fm_glyph_play(d, (W - w) / 2 - 12, y + 22, 7, 0xFFFF);
                draw_text(d, (W - w) / 2 + 2, y + 28, t, 16, 0xFFFF, 0, 0, H);
            } else if (!a->cdl[fm_alb_cls]) {
                fill_rrect(d, 16, y, W - 32, 44, 12, FMC_CARD, 255);
                const char *t = tr("Solo para escuchar (descarga no permitida)"); int w = tw_n(t, 13, -1); if (w > W - 52) w = W - 52;
                draw_text(d, (W - w) / 2, y + 27, t, 13, FMC_GREY, W - 52, 0, H);
            } else {
                fill_rrect(d, 16, y, W - 32, 44, 12, pr ? rgb(6, 90, 180) : FMC_BLUE, 255);
                const char *t = tr("Descargar"); int w = tw_n(t, 16, -1);
                fm_glyph_dl(d, (W - w) / 2 - 14, y + 21, 0xFFFF);
                draw_text(d, (W - w) / 2 + 6, y + 28, t, 16, 0xFFFF, 0, 0, H);
            }
            fm_hit(16, y, W - 16, y + 44, FH_DLBTN, 0);
            y += 58;
        }
        /* tracks */
        const struct np_info np = fm_np;                     /* refreshed by the loop, never while drawing */
        fm_hline(d, 16, W, y, FMC_SEP);
        for (int t = 0; t < a->nt; t++, y += FM_TRH) {
            if (y > H || y + FM_TRH < 30) continue;
            const struct fm_track *T = &a->t[t];
            if (fm_pressed(FH_TRACK, t)) for (int yy = y; yy < y + FM_TRH; yy++) if (yy >= 0 && yy < H) fm_hline(d, 0, W, yy, rgb(38, 38, 42));
            char no[8]; snprintf(no, sizeof no, "%d", T->no);
            int nw = tw_n(no, 15, -1); draw_text(d, 36 - nw, y + 33, no, 15, FMC_GREY, 0, 0, H);
            draw_text(d, 48, y + 24, T->title, 15, 0xFFFF, W - 48 - 50, 0, H);
            int c = fm_track_cls(a, t, fm_alb_cls);
            char sub[120] = "", ln[16] = "", q[32] = "", sz[24] = "";
            if (T->len > 0) { int s = (int)(T->len + .5f); if (s >= 3600) snprintf(ln, sizeof ln, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60); else snprintf(ln, sizeof ln, "%d:%02d", s / 60, s % 60); }
            if (c >= 0) {
                const struct fm_file *f = &a->f[T->f[c]];
                fm_qlabel(f->fq, c == FC_LOSSLESS ? a->it.sr : 0, c == FC_LOSSLESS ? a->it.bits : 0, f->kbps, q, sizeof q);
                if ((f->fq & FQ_MP3VBR) && f->kbps > 0) snprintf(q, sizeof q, "MP3 VBR %dk", f->kbps);
                if (f->size > 0) { fmt_bytes(f->size, sz, sizeof sz); }
            }
            snprintf(sub, sizeof sub, "%s%s%s%s%s%s", ln, ln[0] && q[0] ? " · " : "", q, sz[0] ? " · " : "", a->it.est && sz[0] ? "≈" : "", sz);
            draw_text(d, 48, y + 42, sub, 12, FMC_GREY, W - 48 - 50, 0, H);
            /* state on the right */
            int cx = W - 28, cy = y + FM_TRH / 2;
            double cf = fm_track_cache_frac(t);
            char pp[400] = ""; if (c >= 0) { if (fm_tstate && fm_tstate[t] == 1) fm_dest(a, t, c, pp, sizeof pp); else fm_cache_path(a, t, c, pp, sizeof pp); }
            int playing = np.path[0] && pp[0] && !strcmp(np.path, pp);
            if (playing) fm_glyph_bars(d, cx, cy, now_ms(), FMC_BLUE);
            else if (cf >= 0) fm_ringd(d, cx, cy, 11, 2.4f, (float)cf, FMC_BLUE, rgb(58, 58, 62), cf <= 0.001 ? now_ms() : -1);
            else if (fm_tstate && fm_tstate[t] == 1) fm_glyph_check(d, cx, cy, rgb(48, 209, 88));
            else fm_glyph_play(d, cx + 1, cy, 7, c >= 0 ? FMC_BLUE : rgb(72, 72, 76));
            fm_hline(d, 48, W, y + FM_TRH - 1, FMC_SEP);
            fm_hit(0, y, W, y + FM_TRH, FH_TRACK, t);
        }
        /* licence and source */
        y += 22;
        draw_text(d, 16, y, tr("LICENCIA"), 12, FMC_GREY, 0, 0, H); y += 8;
        char lab[140]; fm_lic_label(a->it.lic, lab, sizeof lab);
        fill_rrect(d, 16, y, W - 32, 10, 0, 0, 0);
        const char *lic = lab[0] ? lab : a->it.etree ? tr("Live Music Archive: copia libre sin fines comerciales") : tr("Licencia no indicada: consulta la página del elemento");
        y += fm_wrap(d, 16, y + 18, W - 32, lic, 15, 0xFFFF, 20, 3) + 4;
        if (a->it.lic[0]) y += fm_wrap(d, 16, y + 14, W - 32, a->it.lic, 12, FMC_GREY, 16, 2);
        char page[200];
        if (a->it.src == FM_SRC_JM) snprintf(page, sizeof page, "jamendo.com/album/%s", a->it.id); else snprintf(page, sizeof page, "archive.org/details/%s", a->it.id);
        y += fm_wrap(d, 16, y + 16, W - 32, page, 12, FMC_BLUE, 16, 2) + 8;
        if (a->desc[0]) { y += 14; y += fm_wrap(d, 16, y + 14, W - 32, a->desc, 13, rgb(174, 174, 178), 18, 14); }
        y += 40;
    }
    fm_ch[FP_ALBUM] = y - y0 + 10;
    float sc = fm_pos[FP_ALBUM] / 60.f;
    fm_chrome_status(d);
    fm_navbar(d, tr("Música libre"), a->it.title, sc);
    draw_toast(d);
}

/* ---- downloads page */
static void fm_render_dl(uint16_t *d)
{
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int top = 74, y = top - (int)fm_pos[FP_DL], y0 = y;
    draw_text(d, 16, y + 34, tr("Descargas"), 28, 0xFFFF, W - 32, 0, H); y += 52;
    pthread_mutex_lock(&fm_dmu);
    int n = 0, fin = 0;
    for (int i = 0; i < fm_nq; i++) {
        struct fm_dl *q = &fm_q[i]; if (q->kind == DK_CACHE || q->state == DS_CANCEL) continue;
        if (q->state == DS_DONE || q->state == DS_FAIL) fin++;
        if (y > -60 && y < H) {
            const char *t = q->kind == DK_COVER ? tr("Carátula") : q->title;
            char tt[140]; if (q->track > 0 && q->kind == DK_FILE) snprintf(tt, sizeof tt, "%d. %s", q->track, t); else fm_cpy(tt, sizeof tt, t);
            draw_text(d, 16, y + 24, tt, 15, 0xFFFF, W - 16 - 56, 0, H);
            char sub[200], sz[24] = "", gs[24] = "";
            long long tot = q->size > 0 ? q->size : q->total;
            if (tot > 0) fmt_bytes(tot, sz, sizeof sz);
            if (q->state == DS_RUN) { fmt_bytes(q->got, gs, sizeof gs); snprintf(sub, sizeof sub, "%s / %s · %s", gs, sz[0] ? sz : "?", q->album); }
            else if (q->state == DS_FAIL) snprintf(sub, sizeof sub, "%s · %s", q->err == 2 ? tr("Sin espacio") : q->err == 3 ? tr("Sin tarjeta SD") : tr("Error"), q->album);
            else if (q->state == DS_DONE) snprintf(sub, sizeof sub, "%s · %s", tr("Descargado"), q->album);
            else snprintf(sub, sizeof sub, "%s%s%s", tr("En cola"), q->album[0] ? " · " : "", q->album);
            draw_text(d, 16, y + 42, sub, 12, q->state == DS_FAIL ? rgb(255, 105, 97) : FMC_GREY, W - 16 - 56, 0, H);
            int cx = W - 30, cy = y + 27;
            if (q->state == DS_RUN) { float f = tot > 0 ? (float)q->got / tot : 0; fm_ringd(d, cx, cy, 12, 2.4f, f, FMC_BLUE, rgb(58, 58, 62), f <= 0.001f ? now_ms() : -1); fill_rrect(d, cx - 4, cy - 4, 8, 8, 1.5f, FMC_BLUE, 255); }
            else if (q->state == DS_QUEUED) { fm_ringd(d, cx, cy, 12, 1.6f, 1, rgb(72, 72, 76), rgb(72, 72, 76), -1); fm_glyph_x(d, cx, cy, 4, FMC_GREY); }
            else if (q->state == DS_DONE) fm_glyph_check(d, cx, cy, rgb(48, 209, 88));
            else fm_glyph_x(d, cx, cy, 5, rgb(255, 105, 97));
            if (q->state == DS_RUN || q->state == DS_QUEUED) fm_hit(W - 60, y, W, y + 54, FH_DLITEM, q->uid);
            fm_hline(d, 16, W, y + 53, FMC_SEP);
        }
        y += 54; n++;
    }
    pthread_mutex_unlock(&fm_dmu);
    if (!n) { const char *m = tr("No hay descargas"); int w = tw_n(m, 15, -1); draw_text(d, (W - w) / 2, y + 60, m, 15, FMC_GREY, 0, 0, H); y += 100; }
    if (fin) {
        const char *c = tr("Borrar terminadas"); int w = tw_n(c, 15, -1);
        draw_text(d, (W - w) / 2, y + 36, c, 15, fm_pressed(FH_CLRDONE, 0) ? rgb(5, 66, 128) : FMC_BLUE, 0, 0, H);
        fm_hit(0, y + 10, W, y + 50, FH_CLRDONE, 0); y += 60;
    }
    y += 10;
    y += draw_wrapped(d, 16, y, W - 32, tr("FM_LIBRARY_NOTE"), 12, rgb(110, 110, 116), 16, 1) + 30;
    fm_ch[FP_DL] = y - y0;
    fm_chrome_status(d);
    fm_navbar(d, tr("Música libre"), tr("Descargas"), fm_pos[FP_DL] / 40.f);
    draw_toast(d);
}

/* ---- settings page (grouped list) */
struct fm_srow { const char *label; char value[64]; int kind; uint16_t vcol; };
static int fm_set_section(uint16_t *d, int y, const char *head, struct fm_srow *r, int n, int base, const char *foot)
{
    if (head) { draw_text(d, 32, y + 16, head, 12, FMC_GREY, 0, 0, H); y += 24; }
    fill_rrect(d, 16, y, W - 32, n * 44, 10, FMC_CARD, 255);
    for (int i = 0; i < n; i++) {
        int ry = y + i * 44;
        if (fm_pressed(FH_SETROW, base + i)) fill_rrect(d, 16, ry, W - 32, 44, i == 0 || i == n - 1 ? 10 : 0, rgb(50, 50, 54), 255);
        uint16_t lc = r[i].kind == 2 ? rgb(255, 105, 97) : r[i].kind == 3 ? FMC_BLUE : 0xFFFF;
        draw_text(d, 32, ry + 28, r[i].label, 15, lc, W - 64 - (r[i].value[0] ? tw_n(r[i].value, 15, -1) + 10 : 0), 0, H);
        if (r[i].value[0]) { int vw = tw_n(r[i].value, 15, -1); if (vw > 150) vw = 150; draw_text(d, W - 32 - vw - (r[i].kind == 1 ? 14 : 0), ry + 28, r[i].value, 15, r[i].vcol ? r[i].vcol : FMC_GREY, 150, 0, H); }
        if (r[i].kind == 1) { fm_line(d, W - 38, ry + 17, W - 33, ry + 22, 2, rgb(90, 90, 96)); fm_line(d, W - 33, ry + 22, W - 38, ry + 27, 2, rgb(90, 90, 96)); }
        if (i < n - 1) fm_hline(d, 32, W - 16, ry + 43, FMC_SEP);
        fm_hit(16, ry, W - 16, ry + 44, FH_SETROW, base + i);
    }
    y += n * 44;
    if (foot) y += 6 + draw_wrapped(d, 32, y + 4, W - 64, foot, 12, FMC_GREY, 16, 1);
    return y + 18;
}
static void fm_render_set(uint16_t *d)
{
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int y = 74 - (int)fm_pos[FP_SET], y0 = y;
    draw_text(d, 16, y + 34, tr("Ajustes"), 28, 0xFFFF, W - 32, 0, H); y += 48;
    struct fm_srow r[3]; memset(r, 0, sizeof r);
    r[0].label = tr("Clave (client_id)"); r[0].kind = 1;
    if (fm_key[0]) { int n = (int)strlen(fm_key); snprintf(r[0].value, sizeof r[0].value, "%.4s…%s", fm_key, n > 8 ? fm_key + n - 3 : ""); } else snprintf(r[0].value, sizeof r[0].value, "%s", tr("Sin configurar"));
    r[1].label = tr("Probar la clave"); r[1].kind = 3;
    static const char *const ts[5] = { "", "Probando…", "Clave válida", "Clave no válida", "Sin conexión" };
    if (fm_test_state) { snprintf(r[1].value, sizeof r[1].value, "%s", tr(ts[fm_test_state])); r[1].vcol = fm_test_state == 2 ? rgb(48, 209, 88) : fm_test_state == 3 ? rgb(255, 105, 97) : 0; }
    int nr = fm_key[0] ? 2 : 1;
    if (fm_key[0]) { r[2].label = tr("Borrar la clave"); r[2].kind = 2; nr = 3; }
    y = fm_set_section(d, y, "JAMENDO", r, nr, 0, tr("FM_JM_HELP"));
    if (fm_test_state == 1) fm_spinner_at(d, W - 50, y0 + 48 + 24 + 44 + 22 - 0);
    memset(r, 0, sizeof r);
    r[0].label = tr("Límite de la caché"); r[0].kind = 1; snprintf(r[0].value, sizeof r[0].value, "%d MB", fm_cache_mb);
    r[1].label = tr("Vaciar la caché"); r[1].kind = 2; { char sz[24]; fmt_bytes(fm_cache_used, sz, sizeof sz); snprintf(r[1].value, sizeof r[1].value, "%s", sz); }
    y = fm_set_section(d, y, tr("ESCUCHAR"), r, 2, 10, tr("FM_CACHE_HELP"));
    memset(r, 0, sizeof r);
    r[0].label = tr("Carpeta"); snprintf(r[0].value, sizeof r[0].value, "Music/Free");
    y = fm_set_section(d, y, tr("DESCARGAS"), r, 1, 20, tr("FM_LIBRARY_NOTE"));
    y += draw_wrapped(d, 32, y, W - 64, tr("FM_ABOUT"), 12, rgb(110, 110, 116), 16, 1) + 30;
    fm_ch[FP_SET] = y - y0;
    fm_chrome_status(d);
    fm_navbar(d, tr("Música libre"), tr("Ajustes"), fm_pos[FP_SET] / 40.f);
    draw_toast(d);
}
/* ---- pick page (iOS selection list with a check mark) */
static void fm_render_pick(uint16_t *d)
{
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    int y = 74 - (int)fm_pos[FP_PICK], y0 = y;
    draw_text(d, 16, y + 34, fm_pick_title, 28, 0xFFFF, W - 32, 0, H); y += 52;
    fill_rrect(d, 16, y, W - 32, fm_pick_n * 44, 10, FMC_CARD, 255);
    for (int i = 0; i < fm_pick_n; i++) {
        int ry = y + i * 44;
        if (ry > H || ry + 44 < 30) continue;
        if (fm_pressed(FH_PICK, i)) fill_rrect(d, 16, ry, W - 32, 44, i == 0 || i == fm_pick_n - 1 ? 10 : 0, rgb(50, 50, 54), 255);
        draw_text(d, 32, ry + 28, fm_pick_opt[i], 15, 0xFFFF, W - 90, 0, H);
        if (i == fm_pick_cur) fm_glyph_check(d, W - 40, ry + 22, FMC_BLUE);
        if (i < fm_pick_n - 1) fm_hline(d, 32, W - 16, ry + 43, FMC_SEP);
        fm_hit(16, ry, W - 16, ry + 44, FH_PICK, i);
    }
    y += fm_pick_n * 44 + 30;
    fm_ch[FP_PICK] = y - y0;
    fm_chrome_status(d);
    fm_navbar(d, fm_stack[fm_sp - 1] == FP_SET ? tr("Ajustes") : tr("Música libre"), fm_pick_title, fm_pos[FP_PICK] / 40.f);
    draw_toast(d);
}
static void fm_render_page(uint16_t *d, int pg, int rec)
{
    int keep = fm_rec; fm_rec = rec; if (rec) fm_nhit = 0;
    switch (pg) {
    case FP_ROOT: fm_render_root(d); break;
    case FP_ALBUM: fm_render_album(d); break;
    case FP_DL: fm_render_dl(d); break;
    case FP_SET: fm_render_set(d); break;
    case FP_PICK: fm_render_pick(d); break;
    }
    fm_rec = keep;
}
static int fm_top_of(int pg) { return pg == FP_ROOT ? fm_list_y() : 74; }
static double fm_rt_sum, fm_rt_max; static int fm_rt_n; static double fm_rt_t;
static void fm_render(void)
{
    double t0 = now_ms();
    fm_render_page(fr0, fm_stack[fm_sp], 1);
    if (fm_editing) kb_draw(fr0, -1);
    double dt = now_ms() - t0; fm_rt_sum += dt; fm_rt_n++; if (dt > fm_rt_max) fm_rt_max = dt;
    if (t0 - fm_rt_t > 4000) { if (fm_rt_n >= 10) lg("perfil musica libre: %d fot, dibujo %.1f ms (max %.1f)", fm_rt_n, fm_rt_sum / fm_rt_n, fm_rt_max); fm_rt_sum = fm_rt_max = 0; fm_rt_n = 0; fm_rt_t = t0; }
}

/* ------------------------------------------------------------------ transitions */
static void fm_compose(const uint16_t *under, const uint16_t *top, int x)
{
    if (x < 0) x = 0; if (x > W) x = W;
    double e = (double)x / W;
    int ux = -(int)((W - x) * 0.3), k = 32 - (int)(8 * (1 - e));
    for (int y = 0; y < H; y++) {
        uint16_t *o = fr0 + y * W;
        if (y < 30) { memcpy(o, top + y * W, W * 2); continue; }
        const uint16_t *u = under + y * W, *t = top + y * W;
        for (int i = 0; i < x; i++) { int sx = i - ux; o[i] = sx < W ? dim565(u[sx], k) : 0; }
        if (x < W) memcpy(o + x, t, (W - x) * 2);
        for (int s = 1; s <= 8 && x - s >= 0; s++) o[x - s] = dim565(o[x - s], 32 - (9 - s) * 2 * (int)(e * 10 + 1) / 11);
    }
}
static uint16_t *fm_pa, *fm_pb;
static void fm_push(int pg)
{
    if (fm_sp >= 5) return;
    memcpy(fm_pa, fr0, HM_FRAME_BYTES);                 /* what is on screen (row still highlighted, as iOS) */
    fm_press_kind = 0; fm_press_idx = -1;
    fm_stack[++fm_sp] = pg; fm_pos[pg] = 0; fm_vel = 0;
    fm_render_page(fm_pb, pg, 0);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 320.0; if (t > 1) t = 1;
        fm_compose(fm_pa, fm_pb, (int)(W * (1 - ease_out(t))));
        present(); if (t >= 1) break;
    }
}
static void fm_pop_page_free(int pg) { if (pg == FP_ALBUM) { fm_alb_free(); fm_lo_flush(); } }
static void fm_pop_from(int x0)
{
    if (fm_sp <= 0) return;
    fm_press_kind = 0; fm_press_idx = -1;
    fm_render_page(fm_pb, fm_stack[fm_sp], 0);
    fm_render_page(fm_pa, fm_stack[fm_sp - 1], 0);
    double t0 = now_ms(), ms = 260.0 * (W - x0) / W + 60;
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1;
        fm_compose(fm_pa, fm_pb, x0 + (int)((W - x0) * ease_out(t)));
        present(); if (t >= 1) break;
    }
    int pg = fm_stack[fm_sp--]; fm_vel = 0;
    fm_pop_page_free(pg);
}

/* ------------------------------------------------------------------ results from the workers */
static struct fm_item *fm_find_item(int src, const char *id, int which)
{
    (void)which;
    for (int s = 0; s < L_N; s++) for (int i = 0; i < fm_l[s].n; i++) if (fm_l[s].it[i].src == src && !strcmp(fm_l[s].it[i].id, id)) return &fm_l[s].it[i];
    return 0;
}
static int fm_poll(void)
{
    if (!fm_done_flag) return 0;
    pthread_mutex_lock(&fm_mu); struct fm_job *list = fm_done; fm_done = 0; fm_done_flag = 0; pthread_mutex_unlock(&fm_mu);
    int dirty = 0;
    while (list) {
        struct fm_job *j = list; list = j->next;
        switch (j->kind) {
        case NJ_LIST: {
            struct fm_list *l = &fm_l[j->slot];
            if (j->gen != l->gen) break;                               /* superseded */
            if (j->rc) { if (!j->append) { if (!l->n || j->rc == -5) { fm_list_clear(l); l->gen = j->gen; } } l->state = l->n && j->rc != -5 ? 2 : 3; l->rc = j->rc; dirty = 1; break; }
            if (!j->append) { fm_list_clear(l); l->gen = j->gen; l->page = 1; }
            int room = FM_MAXIT - l->n, add = j->n < room ? j->n : room;
            struct fm_item *nit = realloc(l->it, (l->n + add + 1) * sizeof *nit);
            if (!nit) break;
            l->it = nit;
            for (int i = 0; i < add; i++) {
                struct fm_item *it = &j->items[i];
                int dup = 0; for (int k = 0; k < l->n; k++) if (!strcmp(l->it[k].id, it->id)) { dup = 1; break; }
                if (dup) continue;
                fm_apply_probe_cache(it);
                l->it[l->n++] = *it;
            }
            l->found = j->found; l->state = 2; l->page = atoi(j->id);
            dirty = 1;
            break; }
        case NJ_ALBUM:
            if (!fm_alb || strcmp(fm_alb->it.id, j->id) || fm_alb->it.src != j->src) break;
            if (j->rc) { fm_alb_state = 3; fm_alb_rc = j->rc; dirty = 1; break; }
            {
                struct fm_album *a = j->alb; j->alb = 0;
                uint16_t *th = fm_alb->it.thumb; int sr = fm_alb->it.sr, bits = fm_alb->it.bits;
                if (!a->it.img[0]) fm_cpy(a->it.img, sizeof a->it.img, fm_alb->it.img);
                if (!a->it.artist[0]) fm_cpy(a->it.artist, sizeof a->it.artist, fm_alb->it.artist);
                if (!a->it.lic[0]) fm_cpy(a->it.lic, sizeof a->it.lic, fm_alb->it.lic);
                fm_alb->it.thumb = 0; free(fm_alb->f); free(fm_alb->t); free(fm_alb);
                fm_alb = a; a->it.thumb = th;
                if (!a->it.sr && sr) { a->it.sr = sr; a->it.bits = bits; }
                if (!a->it.sr) fm_probe_find(a->it.src, a->it.id, &a->it.sr, &a->it.bits);
                fm_alb_state = 2;
                fm_alb_cls = a->ccount[FC_LOSSLESS] ? FC_LOSSLESS : a->ccount[FC_MP3] ? FC_MP3 : FC_OGG;
                fm_tstate_gen = -1; fm_tstate_update();
                /* the cover (real one) and, if unknown, the lossless quality */
                struct fm_job *c = fm_job_new(NJ_COVER);
                if (c) {
                    if (a->it.src == FM_SRC_IA && a->cover[0]) { char e[400]; fm_pathenc(a->cover, e, sizeof e); snprintf(c->url, sizeof c->url, "https://archive.org/download/%s/%s", a->it.id, e); }
                    else fm_cpy(c->url, sizeof c->url, a->it.img);
                    fm_cpy(c->id, sizeof c->id, a->it.id); c->src = a->it.src; fm_cover_state = 1;
                    fm_submit(c, 0);
                }
                if (!a->it.sr && a->ccount[FC_LOSSLESS] && (a->cq[FC_LOSSLESS] & (FQ_FLAC | FQ_FLAC24))) {
                    struct fm_job *p = fm_job_new(NJ_PROBE);
                    if (p) { fm_cpy(p->id, sizeof p->id, a->it.id); p->src = a->it.src; fm_cpy(p->url, sizeof p->url, a->it.probe); fm_submit(p, 0); }
                }
            }
            dirty = 1;
            break;
        case NJ_COVER:
            if (fm_alb && !strcmp(fm_alb->it.id, j->id) && j->px) { free(fm_cover); fm_cover = j->px; j->px = 0; fm_cover_state = 2; dirty = 1; }
            break;
        case NJ_THUMB: {
            struct fm_item *it = fm_find_item(j->src, j->id, 0);
            for (int s = 0; s < L_N; s++) for (int i = 0; i < fm_l[s].n; i++) {
                struct fm_item *x = &fm_l[s].it[i];
                if (x->src != j->src || strcmp(x->id, j->id)) continue;
                if (j->rc || !j->px) { x->thumb_state = j->rc == -3 ? 0 : 2; continue; }
                if (!x->thumb) { x->thumb = malloc(FM_TH * FM_TH * 2); if (x->thumb) memcpy(x->thumb, j->px, FM_TH * FM_TH * 2); }
                x->thumb_state = 2;
            }
            if (fm_alb && !fm_alb->it.thumb && fm_alb->it.src == j->src && !strcmp(fm_alb->it.id, j->id) && j->px) { fm_alb->it.thumb = j->px; j->px = 0; }
            (void)it;
            dirty = 1;
            break; }
        case NJ_PROBE:
            for (int s = 0; s < L_N; s++) for (int i = 0; i < fm_l[s].n; i++) {
                struct fm_item *x = &fm_l[s].it[i];
                if (x->src != j->src || strcmp(x->id, j->id)) continue;
                if (j->rc == -3) { x->qstate = 0; continue; }
                x->qstate = j->rc ? 3 : 2;
                if (!j->rc) { x->sr = j->sr; x->bits = j->bits; x->ch = j->ch; if (j->ntr) { x->ntr = (short)j->ntr; x->tlen = j->tlen; x->dsize = j->dsize; } }
            }
            if (!j->rc && fm_alb && fm_alb->it.src == j->src && !strcmp(fm_alb->it.id, j->id)) { fm_alb->it.sr = j->sr; fm_alb->it.bits = j->bits; }
            dirty = 1;
            break;
        case NJ_JMTEST:
            fm_test_state = j->rc == 0 ? 2 : j->rc == -1000 ? 4 : 3;
            dirty = 1;
            break;
        }
        fm_job_free(j);
    }
    return dirty;
}

/* ------------------------------------------------------------------ search */
static void fm_search_now(void)
{
    if (!strcmp(fm_q_sent, fm_q_text) && fm_l[L_SEARCH].state) return;
    fm_cpy(fm_q_sent, sizeof fm_q_sent, fm_q_text); fm_trim(fm_q_sent);
    fm_pos[FP_ROOT] = 0; fm_lo_flush();
    if (fm_q_sent[0]) { fm_list_clear(&fm_l[L_SEARCH]); fm_list_fetch(L_SEARCH, 1); }
    else fm_list_clear(&fm_l[L_SEARCH]);
}
static void fm_edit_redraw(uint16_t *d, int pressed)
{
    fm_render_page(d, FP_ROOT, 1);
    kb_draw(d, pressed);
}
static void fm_edit_anim(int on)
{
    double t0 = now_ms(); float from = fm_edit_e, to = on ? 1.f : 0.f;
    for (;;) {
        double t = (now_ms() - t0) / 240.0; if (t > 1) t = 1;
        fm_edit_e = from + (to - from) * (float)ease_out(t);
        fm_render_page(fr0, FP_ROOT, 1); present();
        if (t >= 1) break;
    }
}
/* search bar with our keyboard: results while typing (debounce), search key, cancel, clear.
 * Returns the row (list << 16 | index) tapped, -1 otherwise. */
static int fm_search_edit(void)
{
    struct ebuf e = { fm_q_text, (int)strlen(fm_q_text), sizeof fm_q_text - 1, (int)strlen(fm_q_text) };
    fm_editing = 1; fm_chip_x = 0;
    fm_edit_anim(1);
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Buscar"));
    kb_begin(&e, fr0, fm_edit_redraw);
    struct pkt p; int res = -1, last_len = e.len; char last[96]; fm_cpy(last, sizeof last, fm_q_text);
    double blink = now_ms();
    for (;;) {
        int dirty = fm_poll();
        if (strcmp(last, fm_q_text)) { fm_cpy(last, sizeof last, fm_q_text); fm_q_typed = now_ms(); dirty = 1; }
        if (fm_q_typed > 0 && now_ms() - fm_q_typed > 450) { fm_q_typed = 0; fm_search_now(); dirty = 1; }
        if (now_ms() - blink > 530) { blink = now_ms(); dirty = 1; }
        int spin = fm_l[L_SEARCH].state == 1;
        if (dirty || spin) { fm_edit_redraw(fr0, -1); present(); }
        if (!read_pkt(&p, spin ? 40 : 120)) continue;
        if (!p.down) continue;
        if (p.y >= KB_Y) {
            int r = kb_touch(&p, &e, fr0, fm_edit_redraw, 0);
            if (e.len != last_len) { last_len = e.len; fm_q_typed = now_ms(); }
            if (r == K_ENTER) { fm_q_typed = 0; fm_search_now(); break; }
            continue;
        }
        int x = p.x, y = p.y, moved = 0;
        int idx, k = fm_hit_at(x, y, &idx);
        while (cur_down && read_pkt(&p, 3000)) if (abs(cur_y - y) > 12) moved = 1;
        if (k == FH_CANCEL) { fm_q_text[0] = 0; e.len = e.cur = 0; fm_q_typed = 0; fm_search_now(); break; }
        if (k == FH_CLEAR) { fm_q_text[0] = 0; e.len = e.cur = 0; last_len = 0; fm_q_typed = now_ms(); kb_autoshift(&e); if (kb_off) kb_reveal(fr0, fm_edit_redraw); continue; }
        if (k == FH_BAR) { if (kb_off) kb_reveal(fr0, fm_edit_redraw); continue; }
        if (moved) { if (fm_q_typed) { fm_q_typed = 0; fm_search_now(); } break; }    /* scrolling hides the keyboard (iOS) */
        if (k == FH_ROW && y < KB_Y) { if (fm_q_typed) { fm_q_typed = 0; fm_search_now(); } res = idx; break; }
    }
    kb_dst = fr0; kb_rd = fm_edit_redraw;
    if (!kb_off) kb_hide();
    kb_end();
    fm_editing = 0;
    if (!fm_q_text[0]) fm_edit_anim(0);
    else { fm_edit_e = 0; }
    return res;
}

/* ------------------------------------------------------------------ opening things */
static void fm_open_album(const struct fm_item *it)
{
    fm_alb_free();
    fm_alb = calloc(1, sizeof *fm_alb); if (!fm_alb) return;
    fm_alb->it = *it; fm_alb->it.thumb = 0;
    if (it->thumb) { fm_alb->it.thumb = malloc(FM_TH * FM_TH * 2); if (fm_alb->it.thumb) memcpy(fm_alb->it.thumb, it->thumb, FM_TH * FM_TH * 2); }
    fm_alb_state = 1; fm_alb_cls = (it->fq & FQ_LOSSLESS) ? FC_LOSSLESS : FC_MP3;
    struct fm_job *j = fm_job_new(NJ_ALBUM);
    if (j) {
        j->src = it->src; fm_cpy(j->id, sizeof j->id, it->id);
        j->alb = calloc(1, sizeof *j->alb);
        if (j->alb) { j->alb->it = *it; j->alb->it.thumb = 0; }
        if (it->src == FM_SRC_JM) fm_jm_album_url(j->url, sizeof j->url, fm_key, it->id);
        fm_submit(j, 0);
    }
    fm_push(FP_ALBUM);
}
static void fm_retry_album(void)
{
    if (!fm_alb) return;
    struct fm_item it = fm_alb->it; uint16_t *th = it.thumb;
    fm_alb_state = 1;
    struct fm_job *j = fm_job_new(NJ_ALBUM);
    if (j) {
        j->src = it.src; fm_cpy(j->id, sizeof j->id, it.id);
        j->alb = calloc(1, sizeof *j->alb); if (j->alb) { j->alb->it = it; j->alb->it.thumb = 0; }
        if (it.src == FM_SRC_JM) fm_jm_album_url(j->url, sizeof j->url, fm_key, it.id);
        fm_submit(j, 0);
    }
    (void)th;
}
static void fm_open_pick(int what)
{
    static char lab[24][48];
    fm_pick_what = what; fm_pick_n = 0;
    switch (what) {
    case PK_SRC: fm_pick_title = tr("Fuente"); fm_pick_opt[0] = "Internet Archive"; fm_pick_opt[1] = "Jamendo"; fm_pick_n = 2; fm_pick_cur = fm_src; break;
    case PK_COLL: fm_pick_title = tr("Colección"); for (int i = 0; i < FCOL_N; i++) fm_pick_opt[i] = fm_coll_name(i); fm_pick_n = FCOL_N; fm_pick_cur = fm_coll; break;
    case PK_GENRE: fm_pick_title = tr("Género"); fm_pick_opt[0] = tr("Todos los géneros"); for (int i = 0; i < FM_NGENRE; i++) fm_pick_opt[i + 1] = tr(FM_GENRES[i].key); fm_pick_n = FM_NGENRE + 1; fm_pick_cur = fm_genre + 1; break;
    case PK_LANG: fm_pick_title = tr("Idioma"); fm_pick_opt[0] = tr("Cualquier idioma"); for (int i = 0; i < FM_NLANG; i++) fm_pick_opt[i + 1] = FM_LANGS[i].name; fm_pick_n = FM_NLANG + 1; fm_pick_cur = fm_lang + 1; break;
    case PK_SORT: fm_pick_title = tr("Ordenar"); for (int i = 0; i <= FSORT_REL; i++) fm_pick_opt[i] = fm_sort_name(i); fm_pick_n = 3; fm_pick_cur = fm_sort; break;
    case PK_CACHE: {
        static const int mb[] = { 250, 500, 1000, 2000, 4000 };
        fm_pick_title = tr("Límite de la caché"); fm_pick_cur = 1;
        for (int i = 0; i < 5; i++) { snprintf(lab[i], sizeof lab[i], mb[i] >= 1000 ? "%d GB" : "%d MB", mb[i] >= 1000 ? mb[i] / 1000 : mb[i]); fm_pick_opt[i] = lab[i]; if (mb[i] == fm_cache_mb) fm_pick_cur = i; }
        fm_pick_n = 5; break; }
    }
    fm_push(FP_PICK);
}
static void fm_pick_apply(int i)
{
    int changed = i != fm_pick_cur;
    switch (fm_pick_what) {
    case PK_SRC: fm_src = i ? FM_SRC_JM : FM_SRC_IA; break;
    case PK_COLL: fm_coll = i; break;
    case PK_GENRE: fm_genre = i - 1; break;
    case PK_LANG: fm_lang = i - 1; break;
    case PK_SORT: fm_sort = i; break;
    case PK_CACHE: { static const int mb[] = { 250, 500, 1000, 2000, 4000 }; fm_cache_mb = mb[i]; changed = 0; break; }
    }
    fm_pick_cur = i;
    fm_conf_save();
    if (changed) { fm_pos[FP_ROOT] = 0; fm_refresh_all(); }
}
static void fm_cache_measure(void) { fm_cache_used = fm_dir_size(FM_LCACHE, &fm_cache_files); }
static void fm_set_action(int r)
{
    if (r == 0) {
        char k[100]; fm_cpy(k, sizeof k, fm_key);
        if (line_edit(tr("Clave de Jamendo"), "client_id", k, sizeof k) || !k[0]) {
            /* client ids are lower-case hex: the keyboard capitalises the first letter */
            int n = 0; char c[100]; for (char *s = k; *s && n < 99; s++) if (isalnum((unsigned char)*s) || *s == '_' || *s == '-') c[n++] = (char)tolower((unsigned char)*s); c[n] = 0;
            if (strcmp(c, fm_key)) {
                fm_cpy(fm_key, sizeof fm_key, c); fm_key_save(); fm_test_state = 0;
                if (fm_src == FM_SRC_JM) fm_refresh_all();
                if (fm_key[0]) fm_set_action(1);
            }
        }
    } else if (r == 1) {
        if (!fm_key[0]) { fm_set_action(0); return; }
        struct fm_job *j = fm_job_new(NJ_JMTEST); if (!j) return;
        fm_jm_test_url(j->url, sizeof j->url, fm_key); fm_test_state = 1; fm_submit(j, 0);
    } else if (r == 2) {
        if (confirm(tr("¿Borrar la clave de Jamendo?"), tr("Borrar"))) { fm_key[0] = 0; fm_key_save(); fm_test_state = 0; if (fm_src == FM_SRC_JM) fm_refresh_all(); }
    } else if (r == 10) fm_open_pick(PK_CACHE);
    else if (r == 11) {
        if (confirm(tr("¿Vaciar la caché de escucha?"), tr("Vaciar"))) {
            DIR *d = opendir(FM_LCACHE); struct dirent *e;
            while (d && (e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[400]; snprintf(p, sizeof p, FM_LCACHE "/%s", e->d_name); pthread_mutex_lock(&fm_dmu); int keep = fm_in_session(p); pthread_mutex_unlock(&fm_dmu); if (!keep) unlink(p); }
            if (d) closedir(d);
            fm_cache_measure(); fm_dl_gen++;
        }
    }
}

/* ------------------------------------------------------------------ the app */
static float fm_rb(float x, float lo, float hi) { if (x < lo) { float o = lo - x; return lo - o * 120 / (o + 120); } if (x > hi) { float o = x - hi; return hi + o * 120 / (o + 120); } return x; }
static float fm_unrb(float v, float lo, float hi) { if (v < lo) { float o = lo - v; if (o > 119) o = 119; return lo - o * 120 / (120 - o); } if (v > hi) { float o = v - hi; if (o > 119) o = 119; return hi + o * 120 / (120 - o); } return v; }
static float fm_maxpos(int pg) { float m = (float)(fm_ch[pg] - (H - fm_top_of(pg))); return m < 0 ? 0 : m; }
static int fm_gone;
static void fm_tap(int k, int idx, int x)
{
    int pg = fm_stack[fm_sp];
    switch (k) {
    case FH_BACK: if (fm_sp > 0) fm_pop_from(0); break;
    case FH_ROW: {
        int s = idx >> 16, i = idx & 0xFFFF;
        if (s < L_N && i < fm_l[s].n) fm_open_album(&fm_l[s].it[i]);
        break; }
    case FH_BAR: {
        int r = fm_search_edit();
        if (r >= 0) { int s = r >> 16, i = r & 0xFFFF; if (s < L_N && i < fm_l[s].n) fm_open_album(&fm_l[s].it[i]); }
        break; }
    case FH_CLEAR: fm_q_text[0] = 0; fm_search_now(); break;
    case FH_CHIP: {
        struct fm_chip c[8]; int n = fm_chip_list(c); if (idx < 0 || idx >= n) break;
        if (c[idx].what < 0) { fm_lossless = !fm_lossless; fm_conf_save(); fm_pos[FP_ROOT] = 0; fm_refresh_all(); }
        else fm_open_pick(c[idx].what);
        break; }
    case FH_DLS: fm_pos[FP_DL] = 0; fm_push(FP_DL); break;
    case FH_SET: fm_cache_measure(); fm_push(FP_SET); break;
    case FH_KEYROW: fm_cache_measure(); fm_test_state = 0; fm_push(FP_SET); if (!fm_key[0]) fm_set_action(0); break;
    case FH_RETRY:
        if (pg == FP_ALBUM) fm_retry_album();
        else if (fm_q_sent[0]) fm_list_fetch(L_SEARCH, 1);
        else fm_refresh_all();
        break;
    case FH_SEG: if (fm_alb && idx >= 0 && idx < FC_N && fm_alb->ccount[idx]) { fm_alb_cls = idx; fm_tstate_gen = -1; fm_tstate_update(); } break;
    case FH_DLBTN: {
        int done, tot; double fr;
        if (fm_alb_dl(&done, &tot, &fr)) { if (confirm(tr("¿Cancelar la descarga?"), tr("Cancelar descarga"))) fm_cancel_album(); break; }
        int inlib = fm_alb && fm_tstate; for (int t = 0; inlib && t < fm_alb->nt; t++) if (fm_alb->t[t].f[fm_alb_cls] >= 0 && fm_tstate[t] != 1) inlib = 0;
        if (inlib) fm_play_library(); else fm_download_album();
        break; }
    case FH_TRACK: if (fm_alb_state == 2) { fm_listen(idx); if (fm_ls.on && !fm_ls.started) toast(tr("Preparando la pista…")); } break;
    case FH_DLITEM:
        pthread_mutex_lock(&fm_dmu);
        for (int i = 0; i < fm_nq; i++) if (fm_q[i].uid == idx) { if (fm_q[i].state == DS_QUEUED) fm_q[i].state = DS_CANCEL; else if (fm_q[i].state == DS_RUN) fm_q[i].cancel = 1; }
        fm_q_save_locked(); fm_dl_gen++;
        pthread_mutex_unlock(&fm_dmu);
        break;
    case FH_CLRDONE:
        pthread_mutex_lock(&fm_dmu);
        { int w = 0; for (int i = 0; i < fm_nq; i++) if (fm_q[i].state == DS_QUEUED || fm_q[i].state == DS_RUN) fm_q[w++] = fm_q[i]; fm_nq = w; }
        fm_dl_gen++;
        pthread_mutex_unlock(&fm_dmu);
        break;
    case FH_SETROW: fm_set_action(idx); break;
    case FH_PICK: fm_pick_apply(idx); fm_render_page(fm_pb, FP_PICK, 0); fm_pop_from(0); break;
    }
    (void)x;
}
static void freemusic_app_at(int ix, int iy)
{
    double to = now_ms();
    fm_mkdirs(); fm_conf_load(); fm_probes_load(); fm_q_load();
    fm_workers_start(); fm_dl_start();
    fm_pa = img_under; fm_pb = img_sheet; have_under = 0;
    fm_sp = 0; fm_stack[0] = FP_ROOT; fm_press_kind = 0; fm_press_idx = -1; fm_gone = 0; fm_editing = 0; fm_edit_e = 0; fm_vel = 0;
    if (fm_pos[FP_ROOT] < 0) fm_pos[FP_ROOT] = 0;
    if (fm_dl_space) { fm_dl_space = 0; }
    /* first screen: what the cache has (instant), refreshed in the background */
    if (!fm_l[L_F1].n && !fm_l[L_F2].n) fm_refresh_all();
    else { fm_lo_flush(); for (int s = 0; s < L_N; s++) for (int i = 0; i < fm_l[s].n; i++) { if (fm_l[s].it[i].thumb_state == 1) fm_l[s].it[i].thumb_state = 0; if (fm_l[s].it[i].qstate == 1) fm_l[s].it[i].qstate = 0; } }
    lg("musica libre: abrir en %.1f ms (%d+%d+%d en cache)", now_ms() - to, fm_l[L_F1].n, fm_l[L_F2].n, fm_l[L_F3].n);
    render_launcher(img_base, now_ms());
    fm_render_page(fm_pb, FP_ROOT, 1);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 300.0; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, fm_pb, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    enum { FT_IDLE, FT_PRESS, FT_DRAG, FT_CHIPS, FT_BOTTOM };
    struct pkt p; int quit = 0, dirty = 1, ts = FT_IDLE, was_down = cur_down, moving = 0;
    int sx = 0, sy = 0, kind = 0, idx = -1; float raw0 = 0, vy = 0, chip0 = 0;
    double tmove = 0, tlast = now_ms(), tdown = 0; int ly = 0, last_dlgen = fm_dl_gen;
    if (cur_down) ts = FT_BOTTOM;                                  /* the finger that opened: ignore */
    while (!quit) {
        if (fm_poll()) dirty = 1;
        if (fm_dl_gen != last_dlgen) { last_dlgen = fm_dl_gen; fm_tstate_update(); dirty = 1; }
        if (fm_dl_space) { fm_dl_space = 0; toast(tr("No hay espacio suficiente en la tarjeta SD")); dirty = 1; }
        int pg = fm_stack[fm_sp];
        if (pg == FP_ALBUM && fm_ls.on && now_ms() - fm_np_t > 1000) {
            struct np_info n; np_get(&n); fm_np_t = now_ms();
            if (strcmp(n.path, fm_np.path)) dirty = 1;
            fm_np = n;
            /* the player moved to another track of the session: fetch the one after it */
            pthread_mutex_lock(&fm_dmu);
            for (int i = 0; fm_ls.on && i < fm_ls.n; i++) if (fm_ls.path[i] && !strcmp(fm_ls.path[i], n.path) && i != fm_ls.cur) {
                fm_ls.cur = i; fm_ls.t0 = now_ms() - (n.position_ms > 0 ? n.position_ms : 0);
                if (fm_ls.pref < i) fm_ls.pref = i;
                fm_ls_ensure_locked(i + 1);
                break;
            }
            pthread_mutex_unlock(&fm_dmu);
        }
        /* more results when the end of the list comes into view */
        if (pg == FP_ROOT && fm_q_sent[0]) {
            struct fm_list *l = &fm_l[L_SEARCH];
            if (l->state == 2 && l->n < FM_MAXIT && l->n < l->found && fm_pos[FP_ROOT] > fm_maxpos(FP_ROOT) - 500) fm_list_fetch(L_SEARCH, l->page + 1), l->state = 1;
        }
        double tn = now_ms(), dt = tn - tlast; if (dt > 50) dt = 50; tlast = tn;
        float *pos = &fm_pos[pg], lo = 0, hi = fm_maxpos(pg);
        if (ts != FT_DRAG && ts != FT_PRESS && !moving && (*pos < lo - .5f || *pos > hi + .5f)) moving = 1;
        if (moving && ts != FT_DRAG) {
            if (*pos < lo || *pos > hi) {
                float tgt = *pos < lo ? lo : hi;
                *pos += (float)((tgt - *pos) * (1 - exp(-dt / 70.0))); fm_vel *= 0.5f;
                if (fabsf(*pos - tgt) < 0.5f) { *pos = tgt; fm_vel = 0; }
            } else { *pos += fm_vel * (float)dt; fm_vel *= (float)pow(0.998, dt); }
            if (fabsf(fm_vel) < 0.02f && *pos >= lo && *pos <= hi) { moving = 0; fm_vel = 0; }
            dirty = 1;
        }
        /* things that animate by themselves: spinners, rings, now-playing bars, toast */
        int busy = 0;
        for (int s = 0; s < L_N; s++) if (fm_l[s].state == 1) busy = 1;
        if ((pg == FP_ALBUM && (fm_alb_state == 1 || fm_ls.on)) || (pg == FP_SET && fm_test_state == 1)) busy = 1;
        pthread_mutex_lock(&fm_dmu); for (int i = 0; i < fm_nq; i++) if (fm_q[i].state == DS_RUN) busy = 1; pthread_mutex_unlock(&fm_dmu);
        int anim = moving || now_ms() < toast_until + 40;
        static double last_busy; if (busy && now_ms() - last_busy > 90) { last_busy = now_ms(); dirty = 1; }
        if (dirty || anim) { fm_render(); present(); dirty = 0; }
        int wait = anim ? 0 : busy ? 60 : 250;
        while (!quit && read_pkt(&p, wait)) {
            wait = 0;
            int down_edge = p.down && !was_down, up_edge = !p.down && was_down;
            was_down = p.down;
            if (down_edge) {
                sx = p.x; sy = p.y; tdown = tmove = now_ms(); ly = sy; vy = 0; moving = 0; fm_vel = 0;
                pg = fm_stack[fm_sp]; pos = &fm_pos[pg];
                kind = fm_hit_at(sx, sy, &idx);
                if (sy >= 456) { ts = FT_BOTTOM; break; }
                if (fm_sp > 0 && back_zone(sx, sy)) {
                    fm_press_kind = kind; fm_press_idx = idx;
                    if (kind) { fm_render(); present(); }
                    fm_press_kind = 0; fm_press_idx = -1; fm_render();
                    int bk = app_back_gesture(sx, sy);
                    was_down = cur_down;
                    if (bk > 0) { fm_pop_from(0); ts = FT_IDLE; dirty = 1; break; }
                    if (bk < 0) { ts = FT_IDLE; dirty = 1; break; }
                }
                fm_press_kind = kind; fm_press_idx = idx; ts = kind == FH_CHIP ? FT_CHIPS : FT_PRESS; chip0 = fm_chip_x;
                if (kind) dirty = 1;
                if (!cur_down) up_edge = 1; else break;
            }
            if (p.down && !down_edge) {
                if (ts == FT_BOTTOM) {
                    if (sy >= 456 && sy - cur_y > 12 && now_ms() - tdown < 500) {
                        fm_render();
                        if (own_home_gesture(sx, sy, app_by_id("freemusic"))) { fm_gone = 1; quit = 1; break; }
                        was_down = cur_down; ts = FT_IDLE; dirty = 1; break;
                    }
                    continue;
                }
                if (ts == FT_CHIPS) {
                    if (abs(cur_x - sx) > 10 || fm_press_kind == 0) {
                        fm_press_kind = 0; fm_press_idx = -1;
                        float mx = (float)(fm_chips_w - W); if (mx < 0) mx = 0;
                        fm_chip_x = chip0 - (cur_x - sx); if (fm_chip_x < 0) fm_chip_x = 0; if (fm_chip_x > mx) fm_chip_x = mx;
                        dirty = 1;
                    }
                    continue;
                }
                if (ts == FT_PRESS && (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 12)) {
                    ts = FT_DRAG; fm_press_kind = 0; fm_press_idx = -1;
                    raw0 = fm_unrb(*pos, 0, fm_maxpos(pg)) + (float)(cur_y - sy);
                }
                if (ts == FT_DRAG) {
                    *pos = fm_rb(raw0 - (float)(cur_y - sy), 0, fm_maxpos(pg));
                    double t = now_ms(); if (t > tmove) { vy = 0.6f * vy + 0.4f * (float)((ly - cur_y) / (t - tmove)); tmove = t; ly = cur_y; }
                    dirty = 1;
                }
            }
            if (up_edge) {
                int t0s = ts; ts = FT_IDLE;
                if (t0s == FT_DRAG) { if (now_ms() - tmove > 80) vy = 0; fm_vel = vy; moving = 1; tlast = now_ms(); dirty = 1; continue; }
                if (t0s == FT_BOTTOM) { dirty = 1; continue; }
                int k = fm_press_kind, i = fm_press_idx;
                if (t0s == FT_CHIPS && !k) { dirty = 1; continue; }
                fm_press_kind = 0; fm_press_idx = -1; dirty = 1;
                if (k) { fm_render(); present(); fm_tap(k, i, sx); }
                was_down = cur_down;
                break;
            }
        }
    }
    drain_until_up();
    /* leaving: downloads and listening keep going (download worker); UI-only work stops */
    fm_lo_flush();
    for (int s = 0; s < L_N; s++) for (int i = 0; i < fm_l[s].n; i++) { if (fm_l[s].it[i].thumb_state == 1) fm_l[s].it[i].thumb_state = 0; if (fm_l[s].it[i].qstate == 1) fm_l[s].it[i].qstate = 0; }
    while (fm_sp > 0) fm_pop_page_free(fm_stack[fm_sp--]);
    have_under = 0;
    if (fm_gone) { launcher_present(); return; }
    memcpy(fm_pb, fr0, HM_FRAME_BYTES);
    render_launcher(img_base, now_ms());
    double t0b = now_ms();
    for (;;) {
        double t = (now_ms() - t0b) / 280.0; if (t > 1) t = 1; double e = 1 - ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, fm_pb, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    launcher_present();
}
