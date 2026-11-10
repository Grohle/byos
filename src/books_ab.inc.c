/* books_ab.inc.c - pestanas "Audiolibros" y "Tienda" de Libros (incluido desde books.inc.c).
 *  - Audiolibros: cuadricula de los descargados (SD/Audiobooks/<Autor> - <Titulo>/: los MP3
 *    "NN - capitulo.mp3", cover.jpg y book.json), con progreso y descargas en curso.
 *  - Tienda: audiolibros de dominio publico de LibriVox (catalogo de archive.org) y libros EPUB
 *    de Project Gutenberg (Gutendex): destacados (populares / novedades), barra de busqueda de iOS
 *    con nuestro teclado (busca mientras se escribe, 400 ms de pausa), selector de idioma (por
 *    defecto el de la interfaz; se guarda en los ajustes de Libros) y ficha (portada, autor,
 *    descripcion, capitulos con duracion, tamano) con Descargar (todo o un capitulo), anillo de
 *    progreso, pausa, cancelar y eliminar.
 *  - Descargas: proceso aparte (hibyshell --abdl, ver books_abcore.inc.c), sigue al salir de
 *    Libros; la interfaz solo lee su estado (/tmp/hibymod/abdl.st) cada 500 ms.
 *  - Reproducir: pc_list_send_pos (lista del reproductor de HiBy, en orden, ficheros sin escanear
 *    incluidos) desde el capitulo y la posicion guardados, y se abre nuestro Reproduciendo con la
 *    pestana debajo. Mientras suena un audiolibro: mini reproductor sobre la barra de pestanas y
 *    progreso (capitulo + posicion, np_get) guardado en BK_DIR/ab_<id>.pos.
 *  Red: hibynet en segundo plano (procesos), nada bloquea la interfaz. Cache en
 *  /usr/data/hibymod/cache/ab (listas, fichas, miniaturas RGB565). */
#include <sys/prctl.h>
#include "books_abcore.inc.c"

#define AB_TABY 424                      /* barra de pestanas (como la Tienda de apps) */
#define AB_HDR 92                        /* cabecera grande de las paginas raiz */
#define AB_NAV 76                        /* barra de navegacion de las paginas apiladas */
#define AB_MINIH 52                      /* mini reproductor */
#define AB_CACHE "/usr/data/hibymod/cache/ab"
#define AB_Q BK_DIR "/abdl.q"
#define AB_PAUSED BK_DIR "/abdl.paused"
#define AB_ROW 72
#define AB_SROW 52
#define AB_CS 160                        /* portadas de la cuadricula (RGB565) */
#define AB_GW 132
#define AB_BLUE rgb(10, 132, 255)
#define AB_GRAY rgb(142, 142, 147)
#define AB_SEP rgb(38, 38, 40)


/* ------------------------------------------------------------------ proceso de descarga */
/* hibyshell --abdl: se separa (fork + setsid), baja prioridad de CPU y E/S y procesa la cola */
static int ab_worker_main(void)
{
    pid_t p = fork();
    if (p < 0) return 1;
    if (p > 0) _exit(0);                  /* el shell recoge enseguida a este; el nieto sigue solo */
    setsid();
    for (int fd = 3; fd < 256; fd++) close(fd);
    prctl(PR_SET_NAME, "hibyabdl", 0, 0, 0);
    setpriority(PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
    syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
#endif
    return ab_worker_run(AB_Q, AB_PAUSED, ASSETS);
}
static int ab_worker_pid(void)
{
    FILE *f = fopen(AB_PIDF, "r"); int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid <= 0 || kill(pid, 0)) return 0;
    char p[40], c[32] = ""; snprintf(p, sizeof p, "/proc/%d/comm", pid);
    f = fopen(p, "r"); if (f) { if (!fgets(c, sizeof c, f)) c[0] = 0; fclose(f); }
    return strncmp(c, "hibyabdl", 8) ? 0 : pid;
}
static void ab_worker_start(void)
{
    if (ab_worker_pid()) return;
    char exe[300]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *argv[] = { "hibyabdl", "--abdl", 0 };
    pid_t pid;
    if (!posix_spawn(&pid, exe, 0, 0, argv, environ)) { int s; waitpid(pid, &s, 0); }
    lg("audiolibros: proceso de descarga lanzado");
}
static void ab_worker_stop(void)
{
    int pid = ab_worker_pid(); if (!pid) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50 && !kill(pid, 0); i++) msleep(10);
}

/* ------------------------------------------------------------------ utilidades */
static void ab_fmt_dur(int s, char *o, int cap)
{
    if (s >= 3600) snprintf(o, cap, tr("%d h %d min"), s / 3600, (s % 3600) / 60);
    else if (s >= 60) snprintf(o, cap, tr("%d min"), s / 60);
    else snprintf(o, cap, tr("%d s"), s > 0 ? s : 0);
}
static void ab_fmt_size(long long b, char *o, int cap)
{
    if (b >= (1LL << 30)) snprintf(o, cap, "%.1f GB", b / 1073741824.0);
    else snprintf(o, cap, "%.0f MB", b / 1048576.0 < 1 ? 1 : b / 1048576.0);
}
static int ab_lang_cur(void)
{
    int i = ab_lang_find(ab_langc);
    if (i >= 0) return i;
    static const char *const by_ui[LANG_N] = { "spa", "eng", "fra", "deu", "ita", "jpn", "eng", "pol", "rus", "zho", "eng", "zho", "eng" };
    i = lang_ok(ui_lang) ? ab_lang_find(by_ui[ui_lang]) : -1;
    return i >= 0 ? i : 1;
}
static const char *ab_lang_name(int i) { return ab_langs[i].name ? ab_langs[i].name : tr("Todos los idiomas"); }
static void ab_cache_path(const char *pfx, const char *key, const char *ext, char *o, int cap)
{
    snprintf(o, cap, AB_CACHE "/%s%016llx%s", pfx, (unsigned long long)ab_hash(key), ext);
}
static double ab_age_s(const char *path) { struct stat s; if (stat(path, &s)) return -1; return difftime(time(0), s.st_mtime); }
/* cache: si pasa de 300 ficheros, se borran los mas viejos (la mitad) */
static int ab_mt_cmp(const void *a, const void *b) { long x = *(const long *)a, y = *(const long *)b; return x < y ? -1 : x > y; }
static void ab_cache_prune(void)
{
    DIR *d = opendir(AB_CACHE); if (!d) return;
    long mt[400]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 400) { if (e->d_name[0] == '.') continue; char p[300]; snprintf(p, sizeof p, AB_CACHE "/%s", e->d_name); struct stat s; if (!stat(p, &s)) mt[n++] = (long)s.st_mtime; }
    if (n < 300) { closedir(d); return; }
    qsort(mt, n, sizeof *mt, ab_mt_cmp);
    long cut = mt[n / 2]; rewinddir(d);
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[300]; snprintf(p, sizeof p, AB_CACHE "/%s", e->d_name); struct stat s; if (!stat(p, &s) && (long)s.st_mtime <= cut) unlink(p); }
    closedir(d);
}
/* texto partido en lineas (max lines; la ultima con elipsis); devuelve el alto usado */
static int ab_wrap(uint16_t *d, int x, int y, int w, const char *s, int size, uint16_t col, int lh, int lines, int c0, int c1)
{
    int n = 0;
    while (*s && n < lines) {
        while (*s == ' ') s++;
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s), cut = len;
        if (tw_n(s, size, len) > w) {
            cut = 0; int lastsp = -1;
            for (int k = 0; k <= len; k++) {
                if (k < len && ((unsigned char)s[k] & 0xC0) == 0x80) continue;
                if (tw_n(s, size, k) > w) break;
                cut = k; if (s[k] == ' ') lastsp = k;
            }
            if (lastsp > 0 && n < lines - 1) cut = lastsp;
            if (cut <= 0) cut = len;
        }
        char line[400]; int c2 = cut < 399 ? cut : 399; memcpy(line, s, c2); line[c2] = 0;
        if (n == lines - 1 && (cut < len || nl)) { draw_text(d, x, y + n * lh, s, size, col, w, c0, c1); n++; break; }
        if (d) draw_text(d, x, y + n * lh, line, size, col, w, c0, c1);
        s += cut; n++;
        if (*s == '\n') s++;
    }
    return n * lh;
}
/* indicador de actividad (el de la Tienda de apps) */
static void ab_spin(uint16_t *d, int cx, int cy, int c0, int c1) { spn_draw(d, cx, cy, spn_phase(now_ms()), 8, c0, c1); }
/* anillo de progreso (frac < 0: girando) con el cuadradito de parar */
static void ab_ring(uint16_t *d, int cx, int cy, float R, float frac, int stop, int c0, int c1)
{
    float w = 2.4f; int r = (int)R + 2;
    double t = now_ms();
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < c0 || y >= c1 || y < 0 || y >= H) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= W) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy, rr = sqrtf(dx * dx + dy * dy);
            float cov = w / 2 + .5f - fabsf(rr - R); if (cov <= 0) continue; if (cov > 1) cov = 1;
            float ang = atan2f(dx, -dy); if (ang < 0) ang += 2 * (float)M_PI; float u = ang / (2 * (float)M_PI);
            int on = frac < 0 ? fmodf(u - (float)fmod(t / 900.0, 1.0) + 1, 1) < 0.28f : u <= frac;
            d[y * W + x] = mix565(on ? AB_BLUE : rgb(58, 58, 62), d[y * W + x], (unsigned)(255 * cov));
        }
    }
    if (stop && cy - 4 >= c0 && cy + 4 < c1) fill_rrect(d, cx - 4, cy - 4, 8, 8, 1.5f, AB_BLUE, 255);
}
/* flecha de descarga (nube de iOS simplificada: circulo con flecha) */
static void ab_dl_glyph(uint16_t *d, int cx, int cy, uint16_t col, int c0, int c1)
{
    if (cy - 12 < c0 || cy + 12 >= c1) return;
    fill_circle(d, cx, cy, 11, col, 255); fill_circle(d, cx, cy, 9.4f, rgb(0, 0, 0), 255);
    fill_rrect(d, cx - 1, cy - 6, 2, 10, 1, col, 255);
    for (int k = 0; k < 5; k++) { fill_circle(d, cx - k, cy + 4 - k, 1.0f, col, 255); fill_circle(d, cx + k, cy + 4 - k, 1.0f, col, 255); }
}
static void ab_check_glyph(uint16_t *d, int cx, int cy, uint16_t col, int c0, int c1)
{
    if (cy - 8 < c0 || cy + 8 >= c1) return;
    for (float k = 0; k <= 1.001f; k += .08f) { fill_circle(d, cx - 6 + 4 * k, cy + 4 * k, 1.2f, col, 255); fill_circle(d, cx - 2 + 9 * k, cy + 4 - 10 * k, 1.2f, col, 255); }
}
static void ab_chev_left(uint16_t *d, int x, int y, uint16_t c) { for (float k = -1; k <= 1.001f; k += .1f) fill_circle(d, x + 8 * fabsf(k), y + 9 * k, 1.4f, c, 255); }
static void ab_hline(uint16_t *d, int x0, int x1, int y, uint16_t c, int c0, int c1) { if (y < c0 || y >= c1 || y < 0 || y >= H) return; for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) d[y * W + x] = c; }
static void ab_fill(uint16_t *d, int y0, int y1, uint16_t c) { if (y0 < 0) y0 = 0; if (y1 > H) y1 = H; for (int y = y0; y < y1; y++) for (int x = 0; x < W; x++) d[y * W + x] = c; }
static void ab_play_glyph(uint16_t *d, int cx, int cy, int s, uint16_t c)
{
    for (int y = -s; y <= s; y++) { int w = (s - abs(y)) * 3 / 2; for (int x = 0; x <= w; x++) { int xx = cx - s / 2 + x, yy = cy + y; if (xx >= 0 && xx < W && yy >= 0 && yy < H) d[yy * W + xx] = c; } }
}
static void ab_pause_glyph(uint16_t *d, int cx, int cy, int s, uint16_t c) { fill_rrect(d, cx - s * 2 / 3, cy - s, s / 2 + 1, 2 * s, 1, c, 255); fill_rrect(d, cx + s / 6, cy - s, s / 2 + 1, 2 * s, 1, c, 255); }

/* ------------------------------------------------------------------ imagenes (LRU) */
struct ab_img { uint64_t h; int w, hh; uint16_t *px; unsigned use; int miss; };
static struct ab_img ab_im[40]; static unsigned ab_im_tick;
/* imagen RGB565 de w x h desde un fichero; 0 si no esta (cargar con ab_img_load, nunca al pintar) */
static const uint16_t *ab_img_get(const char *path, int w, int h)
{
    uint64_t k = ab_hash(path) ^ (uint64_t)(w * 1000 + h);
    for (int i = 0; i < 40; i++) if (ab_im[i].px && ab_im[i].h == k) { ab_im[i].use = ++ab_im_tick; return ab_im[i].miss ? 0 : ab_im[i].px; }
    return 0;
}
static int ab_img_known(const char *path, int w, int h)
{
    uint64_t k = ab_hash(path) ^ (uint64_t)(w * 1000 + h);
    for (int i = 0; i < 40; i++) if (ab_im[i].px && ab_im[i].h == k) return 1;
    return 0;
}
static int ab_img_load(const char *path, int w, int h)
{
    if (ab_img_known(path, w, h)) return 0;
    struct stat sb; if (stat(path, &sb) || sb.st_size != w * h * 2) return -1;
    for (;;) {   /* tope de memoria: ~768 KB de imagenes (se libera la menos usada) */
        long tot = 0; int lru = -1;
        for (int i = 0; i < 40; i++) if (ab_im[i].px) { tot += ab_im[i].w * ab_im[i].hh * 2; if (lru < 0 || ab_im[i].use < ab_im[lru].use) lru = i; }
        if (tot + w * h * 2 <= (768 << 10) || lru < 0) break;
        free(ab_im[lru].px); memset(&ab_im[lru], 0, sizeof ab_im[lru]);
    }
    int slot = 0; unsigned old = ~0u;
    for (int i = 0; i < 40; i++) { if (!ab_im[i].px) { slot = i; old = 0; break; } if (ab_im[i].use < old) { old = ab_im[i].use; slot = i; } }
    struct ab_img *m = &ab_im[slot];
    if (m->px && (m->w != w || m->hh != h)) { free(m->px); m->px = 0; }
    if (!m->px) m->px = malloc(w * h * 2);
    if (!m->px) return -1;
    int f = open(path, O_RDONLY); int ok = f >= 0 && read(f, m->px, w * h * 2) == w * h * 2; if (f >= 0) close(f);
    m->h = ab_hash(path) ^ (uint64_t)(w * 1000 + h); m->w = w; m->hh = h; m->use = ++ab_im_tick; m->miss = !ok;
    return ok ? 1 : -1;
}
static void ab_img_forget(const char *path, int w, int h)
{
    uint64_t k = ab_hash(path) ^ (uint64_t)(w * 1000 + h);
    for (int i = 0; i < 40; i++) if (ab_im[i].px && ab_im[i].h == k) { free(ab_im[i].px); memset(&ab_im[i], 0, sizeof ab_im[i]); }
}
static void ab_img_free(void) { for (int i = 0; i < 40; i++) { free(ab_im[i].px); memset(&ab_im[i], 0, sizeof ab_im[i]); } }
/* portada generada (degradado + titulo), cuadrada s x s, en un hueco de la LRU */
static uint16_t ab_gen_buf[AB_CS * AB_CS];
static const uint16_t *ab_gen_cover(const char *title, const char *author)
{
    static uint64_t key; uint64_t k = ab_hash(title) ^ ab_hash(author);
    if (key == k) return ab_gen_buf;
    static const uint8_t pal[6][3] = { { 32, 92, 100 }, { 120, 36, 48 }, { 30, 52, 96 }, { 36, 84, 56 }, { 84, 44, 96 }, { 146, 66, 40 } };
    const uint8_t *c = pal[k % 6];
    uint16_t *tmp = img_sheet;                 /* lienzo de paso W */
    for (int y = 0; y < AB_CS; y++) { int kk = 40 - y * 20 / AB_CS; uint16_t col = rgb(c[0] * kk / 32, c[1] * kk / 32, c[2] * kk / 32); for (int x = 0; x < AB_CS; x++) tmp[y * W + x] = col; }
    /* auriculares pequenos arriba */
    for (int a = 0; a < 40; a++) { float t = (float)M_PI * a / 39; fill_circle(tmp, AB_CS / 2 - 16 * cosf(t), 40 - 16 * sinf(t), 1.6f, 0xFFFF, 200); }
    fill_rrect(tmp, AB_CS / 2 - 20, 38, 8, 14, 3, 0xFFFF, 220); fill_rrect(tmp, AB_CS / 2 + 12, 38, 8, 14, 3, 0xFFFF, 220);
    ab_wrap(tmp, 12, 86, AB_CS - 24, title, 15, 0xFFFF, 19, 3, 0, AB_CS);
    if (author && *author) draw_text(tmp, 12, AB_CS - 14, author, 11, rgb(230, 230, 235), AB_CS - 24, 0, AB_CS);
    for (int y = 0; y < AB_CS; y++) memcpy(ab_gen_buf + y * AB_CS, tmp + y * W, AB_CS * 2);
    key = k;
    return ab_gen_buf;
}

/* ------------------------------------------------------------------ trabajos de red (UI) */
enum { AJ_NONE, AJ_LIST, AJ_META, AJ_THUMB, AJ_WALL };
struct ab_job { pid_t pid; int kind; char out[300], key[300]; int w, h; char wall_out[300]; };
static struct ab_job ab_jl, ab_jm, ab_jt;            /* lista, ficha, miniaturas */
static double ab_jl_t;                                /* comienzo de la carga de la lista */
static pid_t ab_spawn_env(char *const argv[], const char *extra)
{
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *env[64]; int n = 0;
    for (char **e = environ; e && *e && n < 62; e++) env[n++] = *e;
    if (extra) env[n++] = (char *)extra;
    env[n] = 0;
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    return r ? -1 : pid;
}
static pid_t ab_spawn(char *const argv[]) { return ab_spawn_env(argv, 0); }
static int ab_job_get(struct ab_job *j, int kind, const char *url, const char *out, const char *key)
{
    if (j->pid) { kill(j->pid, SIGTERM); int s; waitpid(j->pid, &s, 0); j->pid = 0; }
    mkdir("/usr/data/hibymod/cache", 0755); mkdir(AB_CACHE, 0755); mkdir("/tmp/hibymod", 0755);
    snprintf(j->out, sizeof j->out, "%s", out); snprintf(j->key, sizeof j->key, "%s", key ? key : "");
    char *argv[] = { NETBIN, "get", (char *)url, (char *)out, 0 };
    /* Gutendex responde en > 1 min cuando la consulta no esta en su cache */
    j->pid = ab_spawn_env(argv, strstr(url, "gutendex.com") ? "HIBYNET_TIMEOUT=150" : 0); j->kind = kind;
    if (j->pid <= 0) { j->pid = 0; return -1; }
    return 0;
}
static void ab_job_kill(struct ab_job *j) { if (!j->pid) return; kill(j->pid, SIGTERM); int s; waitpid(j->pid, &s, 0); j->pid = 0; j->kind = AJ_NONE; }
/* 0 en curso / nada; 1 terminado bien; -1 terminado mal */
static int ab_job_done(struct ab_job *j)
{
    if (!j->pid) return 0;
    int s; if (waitpid(j->pid, &s, WNOHANG) != j->pid) return 0;
    j->pid = 0;
    return WIFEXITED(s) && WEXITSTATUS(s) == 0 ? 1 : -1;
}

/* ------------------------------------------------------------------ estado */
enum { AP_MINE, AP_STORE, AP_BOOK, AP_LANG };
struct ab_pg { int kind; float scroll; char id[96]; int src; };   /* src: 0 LibriVox, 2 Gutenberg */
static struct ab_pg ab_stk[3][4]; static int ab_spn[3];
#define AB_PG (&ab_stk[bk_tab][ab_spn[bk_tab] - 1])
/* tienda */
static int ab_src, ab_sort;                     /* 0 audiolibros / 1 libros; 0 populares / 1 novedades */
static struct ab_hits ab_res; static char ab_res_key[700]; static int ab_res_state;   /* 0 nada, 1 cargando, 2 bien, 3 error */
static char ab_want_key[700];
static int ab_searching; static char ab_q[128]; static char ab_qsent[128]; static double ab_deb_t;
static struct ebuf ab_eb;
/* ficha */
static struct ab_book ab_cur; static int ab_cur_state; static char ab_cur_id[96]; static int ab_cur_src;
static char ab_cur_dir[400];                    /* carpeta local (exista o no) */
static struct ab_hit ab_gcur;                   /* ficha de Gutenberg */
static unsigned char ab_sst[AB_MAXSEC];         /* 0 nada, 1 en cola, 2 descargando, 3 descargado */
static long long ab_bytes_done, ab_bytes_target; static int ab_ndone, ab_nqueued, ab_paused, ab_desc_open;
/* descargados */
struct ab_mi { char id[96], title[160], author[120], dir[400]; int nsec, ndone, nq, totdur, ch, pos, done; double opened; float pct; long long total; };
static struct ab_mi *ab_mine; static int ab_nmine;
/* estado del proceso de descarga */
struct ab_ds { int alive; char state[16], book[96], dest[600]; long long got, total; int idx, n; };
static struct ab_ds ab_ds; static double ab_ds_t;
/* mini reproductor / progreso */
static struct np_info ab_np; static int ab_np_ok; static double ab_np_t; static int ab_np_mine = -1;
static int ab_play_id; static double ab_play_t; static char ab_play_book[96];
/* toques */
enum { H_NONE, H_TAB, H_BACK, H_SEARCH, H_SCLEAR, H_SCANCEL, H_LANG, H_SORT, H_SRC, H_ROW, H_CELL, H_MAIN, H_MORE, H_SEC, H_ALL, H_DOTS, H_MINI, H_MINIPP, H_LANGROW, H_RETRY, H_GOSTORE };
struct ab_ht { short x0, y0, x1, y1; char kind; short idx; };
static struct ab_ht ab_ht[128]; static int ab_nht, ab_press_k, ab_press_i = -1;
static void ab_hit(int x0, int y0, int x1, int y1, int kind, int idx, int c0, int c1)
{
    if (y0 < c0) y0 = c0; if (y1 > c1) y1 = c1;
    if (y1 <= y0 || ab_nht >= 128) return;
    ab_ht[ab_nht++] = (struct ab_ht){ (short)x0, (short)y0, (short)x1, (short)y1, (char)kind, (short)idx };
}
static int ab_hit_at(int x, int y, int *idx)
{
    for (int i = ab_nht - 1; i >= 0; i--) if (x >= ab_ht[i].x0 && x < ab_ht[i].x1 && y >= ab_ht[i].y0 && y < ab_ht[i].y1) { *idx = ab_ht[i].idx; return ab_ht[i].kind; }
    *idx = -1; return H_NONE;
}
static int ab_pressed(int k, int i) { return ab_press_k == k && ab_press_i == i; }

/* ------------------------------------------------------------------ progreso por libro */
struct ab_pos { int ch, pos, done; double opened; float pct; };
static void ab_pos_path(const char *id, char *o, int cap) { snprintf(o, cap, BK_DIR "/ab_%016llx.pos", (unsigned long long)ab_hash(id)); }
static void ab_cov_path(const char *id, char *o, int cap) { snprintf(o, cap, BK_DIR "/ab_%016llx.cov", (unsigned long long)ab_hash(id)); }
static int ab_pos_load(const char *id, struct ab_pos *p)
{
    memset(p, 0, sizeof *p);
    char path[200]; ab_pos_path(id, path, sizeof path);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int ok = fscanf(f, "%d %d %lf %f %d", &p->ch, &p->pos, &p->opened, &p->pct, &p->done) == 5;
    fclose(f); if (!ok) memset(p, 0, sizeof *p);
    return ok;
}
static void ab_pos_save(const char *id, const struct ab_pos *p)
{
    bk_mkdirs();
    char path[200], b[96]; ab_pos_path(id, path, sizeof path);
    int n = snprintf(b, sizeof b, "%d %d %.0f %.4f %d\n", p->ch, p->pos, p->opened, p->pct, p->done);
    bk_write_atomic(path, b, n);
}

/* ------------------------------------------------------------------ cola y estado de descargas */
static void ab_ds_read(void)
{
    memset(&ab_ds, 0, sizeof ab_ds);
    ab_ds.alive = ab_worker_pid() != 0;
    FILE *f = fopen(AB_STAT, "r"); if (!f) return;
    char l[1000]; if (fgets(l, sizeof l, f)) {
        char *fl[8]; int n = 0; fl[n++] = l;
        for (char *p = l; *p && n < 8; p++) if (*p == '\t') { *p = 0; fl[n++] = p + 1; }
        if (n == 8) {
            char *nl = strchr(fl[7], '\n'); if (nl) *nl = 0;
            snprintf(ab_ds.state, sizeof ab_ds.state, "%s", fl[1]); snprintf(ab_ds.book, sizeof ab_ds.book, "%s", fl[2]);
            ab_ds.got = atoll(fl[3]); ab_ds.total = atoll(fl[4]); ab_ds.idx = atoi(fl[5]); ab_ds.n = atoi(fl[6]);
            snprintf(ab_ds.dest, sizeof ab_ds.dest, "%s", fl[7]);
        }
    }
    fclose(f);
    if (!ab_ds.alive && !strcmp(ab_ds.state, "dl")) snprintf(ab_ds.state, sizeof ab_ds.state, "stopped");
}
static int ab_is_paused(const char *id) { char *p = ab_read_small(AB_PAUSED, 8192); int r = p && ab_in_list(p, id); free(p); return r; }
static void ab_set_paused(const char *id, int on)
{
    char *p = ab_read_small(AB_PAUSED, 8192); char out[8300]; int n = 0;
    for (const char *s = p; s && *s; ) {
        const char *e = strchr(s, '\n'); int l = e ? (int)(e - s) : (int)strlen(s);
        if (l && !(l == (int)strlen(id) && !strncmp(s, id, l)) && n + l + 2 < (int)sizeof out) { memcpy(out + n, s, l); n += l; out[n++] = '\n'; }
        s = e ? e + 1 : 0;
    }
    free(p);
    if (on && n + (int)strlen(id) + 2 < (int)sizeof out) n += snprintf(out + n, sizeof out - n, "%s\n", id);
    bk_mkdirs(); bk_write_atomic(AB_PAUSED, out, n);
}
/* quita de la cola las entradas del libro (o solo dest si no es 0); borra sus .part */
static void ab_q_remove(const char *id, const char *dest)
{
    int n; struct ab_qe *v = ab_q_load(AB_Q, &n); int k = 0;
    for (int i = 0; i < n; i++) {
        int drop = !strcmp(v[i].book, id) && (!dest || !strcmp(v[i].dest, dest));
        if (drop) { char pp[620]; snprintf(pp, sizeof pp, "%s.part", v[i].dest); unlink(pp); continue; }
        v[k++] = v[i];
    }
    ab_q_save(AB_Q, v, k); free(v);
}
static void ab_q_add(const struct ab_qe *e, int ne)
{
    int n; struct ab_qe *v = ab_q_load(AB_Q, &n);
    struct ab_qe *nv = realloc(v, (n + ne + 1) * sizeof *nv);
    if (!nv) { free(v); return; }
    for (int i = 0; i < ne; i++) {
        int dup = 0; for (int j = 0; j < n; j++) if (!strcmp(nv[j].dest, e[i].dest)) dup = 1;
        if (!dup) nv[n++] = e[i];
    }
    bk_mkdirs(); ab_q_save(AB_Q, nv, n); free(nv);
}

/* ------------------------------------------------------------------ audiolibros descargados */
static void ab_root(char *o, int cap) { snprintf(o, cap, "%s/" AB_ROOT_NAME, FS_SD); }
static int ab_mi_cmp(const void *a, const void *b)
{
    const struct ab_mi *x = a, *y = b;
    if (x->opened != y->opened) return x->opened < y->opened ? 1 : -1;
    return strcasecmp(x->title, y->title);
}
static void ab_mine_scan(void)
{
    if (!ab_mine) ab_mine = calloc(200, sizeof *ab_mine);
    ab_nmine = 0; if (!ab_mine) return;
    char root[300]; ab_root(root, sizeof root);
    int nq; struct ab_qe *q = ab_q_load(AB_Q, &nq);
    DIR *d = opendir(root);
    struct dirent *e;
    while (d && (e = readdir(d)) && ab_nmine < 200) {
        if (e->d_name[0] == '.') continue;
        struct ab_mi *m = &ab_mine[ab_nmine]; memset(m, 0, sizeof *m);
        snprintf(m->dir, sizeof m->dir, "%s/%s", root, e->d_name);
        char bj[460]; snprintf(bj, sizeof bj, "%s/book.json", m->dir);
        struct ab_book b; if (ab_book_load(bj, &b, 0)) continue;
        snprintf(m->id, sizeof m->id, "%s", b.id); snprintf(m->title, sizeof m->title, "%s", b.title); snprintf(m->author, sizeof m->author, "%s", b.author);
        m->nsec = b.nsec; m->totdur = b.totdur; m->total = b.total;
        ab_book_free(&b);
        DIR *dd = opendir(m->dir); struct dirent *f;
        while (dd && (f = readdir(dd))) { int l = (int)strlen(f->d_name); if (l > 4 && !strcasecmp(f->d_name + l - 4, ".mp3")) m->ndone++; }
        if (dd) closedir(dd);
        for (int i = 0; i < nq; i++) if (!strcmp(q[i].book, m->id) && q[i].kind == 'm') { struct stat sb; if (stat(q[i].dest, &sb)) m->nq++; }
        struct ab_pos p; if (ab_pos_load(m->id, &p)) { m->ch = p.ch; m->pos = p.pos; m->done = p.done; m->opened = p.opened; m->pct = p.pct; }
        ab_nmine++;
    }
    if (d) closedir(d);
    free(q);
    qsort(ab_mine, ab_nmine, sizeof *ab_mine, ab_mi_cmp);
}
static int ab_mine_find(const char *id) { for (int i = 0; i < ab_nmine; i++) if (!strcmp(ab_mine[i].id, id)) return i; return -1; }
/* carpeta de un libro: "<Autor> - <Titulo>" (si ya existe con otro id, se le anade el id) */
static void ab_book_dir(const struct ab_book *b, char *o, int cap)
{
    int k = ab_mine_find(b->id);
    if (k >= 0) { snprintf(o, cap, "%s", ab_mine[k].dir); return; }
    char name[200], s[260]; snprintf(s, sizeof s, "%s - %s", b->author[0] ? b->author : "LibriVox", b->title);
    ab_sanitize(s, name, 90);
    char root[300]; ab_root(root, sizeof root);
    snprintf(o, cap, "%s/%s", root, name);
    char bj[460]; snprintf(bj, sizeof bj, "%s/book.json", o);
    struct ab_book x;
    if (!ab_book_load(bj, &x, 0)) { if (strcmp(x.id, b->id)) { char id[40]; ab_sanitize(b->id, id, 36); snprintf(o, cap, "%s/%s (%s)", root, name, id); } ab_book_free(&x); }
}
/* estado de cada capitulo del libro abierto */
static void ab_sec_refresh(void)
{
    ab_bytes_done = ab_bytes_target = 0; ab_ndone = ab_nqueued = 0;
    memset(ab_sst, 0, sizeof ab_sst);
    if (ab_cur_state != 2 || ab_cur_src == 2) return;
    int nq; struct ab_qe *q = ab_q_load(AB_Q, &nq);
    for (int i = 0; i < ab_cur.nsec; i++) {
        char p[600]; snprintf(p, sizeof p, "%s/%s", ab_cur_dir, ab_cur.sec[i].file);
        struct stat sb;
        if (!stat(p, &sb)) { ab_sst[i] = 3; ab_ndone++; ab_bytes_done += ab_cur.sec[i].size; ab_bytes_target += ab_cur.sec[i].size; continue; }
        for (int k = 0; k < nq; k++) if (!strcmp(q[k].dest, p)) { ab_sst[i] = !strcmp(ab_ds.dest, p) && !strcmp(ab_ds.state, "dl") ? 2 : 1; ab_nqueued++; ab_bytes_target += ab_cur.sec[i].size; break; }
    }
    free(q);
    ab_paused = ab_is_paused(ab_cur.id);
}

/* ------------------------------------------------------------------ tienda: listas */
static void ab_list_key(char *o, int cap, const char *query)
{
    snprintf(o, cap, "%d|%d|%s|%s", ab_src, query && *query ? 2 : ab_sort, ab_langs[ab_lang_cur()].code, query ? query : "");
}
static void ab_list_url(char *o, int cap, const char *query)
{
    if (ab_src == 1) ab_gut_url(o, cap, ab_lang_cur(), query, 1);
    else ab_search_url(o, cap, ab_lang_cur(), query, query && *query ? 2 : ab_sort, 1);
}
static int ab_list_parse(const char *path)
{
    int e = ab_src == 1 ? ab_gut_parse(path, &ab_res) : ab_search_parse(path, &ab_res);
    return e && !ab_res.n ? -1 : 0;
}
/* pide la lista que toca (destacados con cache de 6 h; busquedas siempre a la red) */
static void ab_list_want(const char *query)
{
    char key[700]; ab_list_key(key, sizeof key, query);
    if (!strcmp(key, ab_want_key) && ab_res_state) return;
    snprintf(ab_want_key, sizeof ab_want_key, "%s", key);
    char url[1400], out[300]; ab_list_url(url, sizeof url, query);
    int search = query && *query;
    if (search) snprintf(out, sizeof out, "/tmp/hibymod/ab_q.json");
    else ab_cache_path("l_", key, ".json", out, sizeof out);
    if (!search) {
        double age = ab_age_s(out);
        if (age >= 0 && !ab_list_parse(out)) {
            snprintf(ab_res_key, sizeof ab_res_key, "%s", key); ab_res_state = 2;
            if (age < 6 * 3600) { ab_job_kill(&ab_jl); return; }
            ab_job_get(&ab_jl, AJ_LIST, url, out, key);   /* se refresca sin quitar lo que se ve */
            return;
        }
    }
    ab_res.n = 0; ab_res_state = 1; ab_res_key[0] = 0; ab_jl_t = now_ms();
    if (access(NETBIN, X_OK) || ab_job_get(&ab_jl, AJ_LIST, url, out, key)) ab_res_state = 3;
    lg("tienda de libros: %s", url);
}
static void ab_list_retry(void) { ab_want_key[0] = 0; ab_res_state = 0; ab_list_want(ab_searching ? ab_qsent : 0); }
/* miniaturas: la siguiente que falte de la lista (descarga + hibywall) o la portada de la ficha */
static void ab_thumb_spec(int src, int big, int *w, int *h) { if (src == 1) { *w = big ? 100 : 40; *h = big ? 150 : 60; } else { *w = *h = big ? 120 : 56; } }
static void ab_thumb_path(const char *id, int src, int w, int h, char *o, int cap)
{
    char key[160], ext[24]; snprintf(key, sizeof key, "%d:%s", src, id); snprintf(ext, sizeof ext, ".%dx%d", w, h);
    ab_cache_path("t_", key, ext, o, cap);
}
static void ab_thumb_jpg(const char *id, int src, char *o, int cap) { char key[160]; snprintf(key, sizeof key, "%d:%s", src, id); ab_cache_path("t_", key, ".jpg", o, cap); }
static int ab_thumb_need(const char *id, int src, const char *cover_url, int big)
{
    int w, h; ab_thumb_spec(src, big, &w, &h);
    char raw[300]; ab_thumb_path(id, src, w, h, raw, sizeof raw);
    if (!access(raw, R_OK) || ab_jt.pid) return 0;
    char jpg[300]; ab_thumb_jpg(id, src, jpg, sizeof jpg);
    ab_jt.w = w; ab_jt.h = h; snprintf(ab_jt.wall_out, sizeof ab_jt.wall_out, "%s", raw);
    if (!access(jpg, R_OK)) {   /* la imagen ya esta: solo convertir */
        char ws[8], hs[8]; snprintf(ws, sizeof ws, "%d", w); snprintf(hs, sizeof hs, "%d", h);
        char *argv[] = { ASSETS "/hibywall", jpg, raw, ws, hs, 0 };
        ab_jt.pid = ab_spawn(argv); ab_jt.kind = AJ_WALL; if (ab_jt.pid <= 0) ab_jt.pid = 0;
        snprintf(ab_jt.key, sizeof ab_jt.key, "%s", id);
        return 1;
    }
    char url[300];
    if (src == 1) snprintf(url, sizeof url, "%s", cover_url ? cover_url : ""); else ab_cover_url(id, url, sizeof url);
    if (!url[0]) return 0;
    if (ab_job_get(&ab_jt, AJ_THUMB, url, jpg, id)) return 0;
    return 1;
}
static double ab_thumb_fail_t; static char ab_thumb_fail[96];
static void ab_thumbs_next(void)
{
    if (ab_jt.pid) return;
    struct ab_pg *pg = AB_PG;
    if (pg->kind == AP_BOOK && ab_cur_state == 2) { if (ab_thumb_need(ab_cur_id, ab_cur_src == 2, ab_cur_src == 2 ? ab_gcur.cover : 0, 1)) return; }
    if (pg->kind != AP_STORE || ab_res_state != 2) return;
    for (int i = 0; i < ab_res.n; i++) {
        if (!strcmp(ab_res.h[i].id, ab_thumb_fail) && now_ms() - ab_thumb_fail_t < 60000) continue;
        if (ab_thumb_need(ab_res.h[i].id, ab_src, ab_res.h[i].cover, 0)) return;
    }
}

/* ------------------------------------------------------------------ ficha */
static void ab_cur_reset(void) { ab_book_free(&ab_cur); ab_cur_state = 0; ab_cur_id[0] = 0; ab_desc_open = 0; }
static void ab_cur_loaded(void)
{
    ab_cur_state = 2;
    ab_book_dir(&ab_cur, ab_cur_dir, sizeof ab_cur_dir);
    ab_sec_refresh();
}
/* abre la ficha "id": local (book.json) si esta descargado; si no, la de archive.org (cache 1 dia) */
static void ab_cur_open(const char *id, int src)
{
    if (!strcmp(ab_cur_id, id) && ab_cur_src == src && ab_cur_state) return;
    ab_cur_reset(); snprintf(ab_cur_id, sizeof ab_cur_id, "%s", id); ab_cur_src = src;
    if (src == 2) { ab_cur_state = 2; return; }    /* Gutenberg: los datos vienen del resultado */
    int k = ab_mine_find(id);
    if (k >= 0) {
        char bj[460]; snprintf(bj, sizeof bj, "%s/book.json", ab_mine[k].dir);
        if (!ab_book_load(bj, &ab_cur, 1)) { ab_cur_loaded(); return; }
    }
    char out[300]; ab_cache_path("m_", id, ".json", out, sizeof out);
    double age = ab_age_s(out);
    if (age >= 0 && age < 86400 && !ab_meta_parse(out, &ab_cur)) { ab_cur_loaded(); return; }
    ab_book_free(&ab_cur);
    char url[300]; ab_meta_url(id, url, sizeof url);
    ab_cur_state = access(NETBIN, X_OK) || ab_job_get(&ab_jm, AJ_META, url, out, id) ? 3 : 1;
}
static const char *ab_sec_name(int i, char *buf, int cap)
{
    if (ab_cur.sec[i].title[0]) return ab_cur.sec[i].title;
    snprintf(buf, cap, tr("Capítulo %d"), i + 1); return buf;
}

/* ------------------------------------------------------------------ dibujo: barras */
static void ab_tab_glyph(uint16_t *d, int t, int cx, int cy, uint16_t col)
{
    uint16_t bg = rgb(20, 20, 22);
    if (t == 0) {   /* biblioteca: libros en la estanteria */
        fill_rrect(d, cx - 10, cy - 10, 6, 20, 1.5f, col, 255); fill_rrect(d, cx - 3, cy - 10, 6, 20, 1.5f, col, 255);
        for (int k = 0; k < 20; k++) fill_circle(d, cx + 6 + k * 0.22f, cy - 9 + k, 2.2f, col, 255);
        fill_rrect(d, cx - 12, cy + 10, 24, 2, 1, col, 255);
    } else if (t == 1) {   /* audiolibros: auriculares */
        for (int a = 0; a <= 30; a++) { float th = (float)M_PI * a / 30; fill_circle(d, cx - 9.5f * cosf(th), cy + 1 - 9.5f * sinf(th), 1.4f, col, 255); }
        fill_rrect(d, cx - 12, cy, 6, 11, 2.5f, col, 255); fill_rrect(d, cx + 6, cy, 6, 11, 2.5f, col, 255);
    } else {   /* tienda: bolsa */
        fill_rrect(d, cx - 10, cy - 5, 20, 17, 3, col, 255);
        for (int a = 0; a <= 20; a++) { float th = (float)M_PI * a / 20; fill_circle(d, cx - 5 * cosf(th), cy - 5 - 6 * sinf(th), 1.2f, col, 255); }
        fill_circle(d, cx - 5, cy, 1.3f, bg, 255); fill_circle(d, cx + 5, cy, 1.3f, bg, 255);
    }
}
static void ab_tabbar(uint16_t *d, int tab, int rec)
{
    static uint16_t cache[(H - AB_TABY) * W]; static int key = -1;
    int k = tab + (ui_lang << 4);
    if (k != key) {
        ab_fill(d, AB_TABY, H, rgb(20, 20, 22));
        for (int x = 0; x < W; x++) d[AB_TABY * W + x] = rgb(48, 48, 52);
        static const char *tl[3] = { "Biblioteca", "Audiolibros", "Gutenberg" };
        for (int i = 0; i < 3; i++) {
            int cx = W * i / 3 + W / 6; uint16_t col = i == tab ? AB_BLUE : AB_GRAY;
            ab_tab_glyph(d, i, cx, AB_TABY + 18, col);
            const char *l = tr(tl[i]); int lw = tw_n(l, 10, -1); if (lw > W / 3 - 4) lw = W / 3 - 4;
            draw_text(d, cx - lw / 2, AB_TABY + 44, l, 10, col, W / 3 - 4, 0, H);
        }
        memcpy(cache, d + AB_TABY * W, sizeof cache); key = k;
    } else memcpy(d + AB_TABY * W, cache, sizeof cache);
    if (rec) for (int i = 0; i < 3; i++) ab_hit(W * i / 3, AB_TABY, W * (i + 1) / 3, H, H_TAB, i, 0, H);
}
static int ab_np_ours(void);
static void ab_minibar(uint16_t *d, int y)
{
    ab_fill(d, y, y + AB_MINIH, rgb(30, 30, 33));
    ab_hline(d, 0, W, y, rgb(48, 48, 52), 0, H);
    if (ab_pressed(H_MINI, 0)) ab_fill(d, y + 1, y + AB_MINIH, rgb(44, 44, 48));
    const struct ab_mi *m = ab_np_mine >= 0 && ab_np_mine < ab_nmine ? &ab_mine[ab_np_mine] : 0;
    char cp[200]; const uint16_t *cv = 0;
    if (m) { ab_cov_path(m->id, cp, sizeof cp); cv = ab_img_get(cp, AB_CS, AB_CS); if (!cv) cv = ab_gen_cover(m->title, m->author); }
    if (cv) bk_blit_scaled(d, cv, AB_CS, AB_CS, 12, y + 8, 36, 36, 32, 4, y, y + AB_MINIH);
    char t[160]; snprintf(t, sizeof t, "%s", ab_np.title[0] ? ab_np.title : strrchr(ab_np.path, '/') ? strrchr(ab_np.path, '/') + 1 : ab_np.path);
    draw_text(d, 58, y + 23, t, 14, 0xFFFF, W - 58 - 56, 0, H);
    draw_text(d, 58, y + 41, m ? m->title : "", 12, AB_GRAY, W - 58 - 56, 0, H);
    if (ab_np.playing) ab_pause_glyph(d, W - 30, y + 26, 9, 0xFFFF); else ab_play_glyph(d, W - 30, y + 26, 9, 0xFFFF);
    ab_hit(0, y, W - 56, y + AB_MINIH, H_MINI, 0, 0, H);
    ab_hit(W - 56, y, W, y + AB_MINIH, H_MINIPP, 0, 0, H);
}
static int ab_mini_on(void) { return ab_np_ok && ab_np_ours() && !ab_searching; }
static int ab_bottom(void) { int b = AB_TABY - (ab_mini_on() ? AB_MINIH : 0); if (ab_searching && KB_Y < b) b = KB_Y; return b; }

/* ------------------------------------------------------------------ dibujo: audiolibros */
static void ab_cell_xy(int i, float scroll, int *x, int *y) { *x = 20 + (i & 1) * (AB_GW + 16); *y = AB_HDR + 14 + (i >> 1) * (AB_GW + 54) - (int)scroll; }
static float ab_mine_max(void) { float m = ((ab_nmine + 1) / 2) * (AB_GW + 54) + 14 - (ab_bottom() - AB_HDR); return m < 0 ? 0 : m; }
static float ab_mine_frac(const struct ab_mi *m)
{
    if (!m->nq) return 1;
    float f = (float)m->ndone / (m->ndone + m->nq);
    if (!strcmp(ab_ds.book, m->id) && ab_ds.total > 0 && !strcmp(ab_ds.state, "dl")) f += (float)ab_ds.got / ab_ds.total / (m->ndone + m->nq);
    return f;
}
static void ab_render_mine(uint16_t *d, float scroll)
{
    int c0 = AB_HDR, c1 = ab_bottom();
    for (int i = 0; i < ab_nmine; i++) {
        int x, y; ab_cell_xy(i, scroll, &x, &y);
        if (y + AB_GW + 54 < c0 || y > c1) continue;
        struct ab_mi *m = &ab_mine[i];
        char cp[200]; ab_cov_path(m->id, cp, sizeof cp);
        const uint16_t *cv = ab_img_get(cp, AB_CS, AB_CS); if (!cv) cv = ab_gen_cover(m->title, m->author);
        bk_blit_scaled(d, cv, AB_CS, AB_CS, x, y, AB_GW, AB_GW, ab_pressed(H_CELL, i) ? 22 : 32, 5, c0, c1);
        if (m->nq) {   /* descargando: velo y anillo */
            for (int yy = y; yy < y + AB_GW; yy++) if (yy >= c0 && yy < c1) for (int xx = x; xx < x + AB_GW; xx++) d[yy * W + xx] = dim565(d[yy * W + xx], 14);
            int paused = ab_is_paused(m->id);
            fill_circle(d, x + AB_GW / 2, y + AB_GW / 2, 20, rgb(0, 0, 0), 150);
            if (y + AB_GW / 2 - 22 > c0 && y + AB_GW / 2 + 22 < c1) {
                ab_ring(d, x + AB_GW / 2, y + AB_GW / 2, 15, ab_mine_frac(m), !paused, c0, c1);
                if (paused) ab_play_glyph(d, x + AB_GW / 2 + 1, y + AB_GW / 2, 6, AB_BLUE);
            }
        }
        draw_text(d, x, y + AB_GW + 19, m->title, 13, 0xFFFF, AB_GW - 4, c0, c1);
        char st[64]; uint16_t sc = AB_GRAY;
        if (m->nq) snprintf(st, sizeof st, tr("%d de %d"), m->ndone, m->ndone + m->nq);
        else if (m->done) snprintf(st, sizeof st, "%s", tr("Terminado"));
        else if (m->opened <= 0) { snprintf(st, sizeof st, "%s", tr("NUEVO")); sc = AB_BLUE; }
        else snprintf(st, sizeof st, "%d %%", (int)(m->pct * 100 + 0.5f) < 1 ? 1 : (int)(m->pct * 100 + 0.5f));
        draw_text(d, x, y + AB_GW + 37, st, 12, sc, AB_GW - 30, c0, c1);
        for (int k = 0; k < 3; k++) { int cy = y + AB_GW + 33; if (cy > c0 + 3 && cy < c1 - 3) fill_circle(d, x + AB_GW - 16 + k * 6, cy, 1.6f, AB_GRAY, 255); }
        ab_hit(x - 6, y, x + AB_GW + 6, y + AB_GW + 44, H_CELL, i, c0, c1);
    }
    if (!ab_nmine) {
        int cy = 196;
        for (int a = 0; a <= 40; a++) { float th = (float)M_PI * a / 40; fill_circle(d, W / 2 - 26 * cosf(th), cy - 26 * sinf(th), 3, rgb(72, 72, 76), 255); }
        fill_rrect(d, W / 2 - 32, cy - 4, 14, 28, 5, rgb(72, 72, 76), 255); fill_rrect(d, W / 2 + 18, cy - 4, 14, 28, 5, rgb(72, 72, 76), 255);
        const char *t = tr("No hay audiolibros"); draw_text(d, (W - tw_n(t, 20, -1)) / 2, cy + 60, t, 20, 0xFFFF, 0, 0, H);
        ab_wrap(d, 28, cy + 84, W - 56, sd_mounted() ? tr("AB_HELP") : tr("La SD está conectada al ordenador"), 14, AB_GRAY, 19, 4, 0, H);
        if (sd_mounted()) {
            const char *b = tr("Ir a la Tienda"); int bw = tw_n(b, 15, -1) + 36;
            fill_rrect(d, (W - bw) / 2, cy + 150, bw, 36, 18, ab_pressed(H_GOSTORE, 0) ? rgb(5, 90, 180) : AB_BLUE, 255);
            draw_text(d, (W - bw) / 2 + 18, cy + 174, b, 15, 0xFFFF, 0, 0, H);
            ab_hit((W - bw) / 2, cy + 150, (W + bw) / 2, cy + 186, H_GOSTORE, 0, 0, H);
        }
    }
}

/* ------------------------------------------------------------------ dibujo: tienda */
static void ab_glyph_search(uint16_t *d, int cx, int cy, uint16_t col, uint16_t bg) { st_glyph_search(d, cx, cy, 16, col, bg); }
/* control segmentado de iOS (n opciones) */
static void ab_segment(uint16_t *d, int x, int y, int w, int h, const char *const *opt, int n, int sel, int kind, int c0, int c1)
{
    if (y < c0 || y + h > c1) { ab_hit(x, y, x + w, y + h, kind, -1, c0, c1); return; }
    fill_rrect(d, x, y, w, h, 9, rgb(44, 44, 48), 255);
    int sw = w / n;
    fill_rrect(d, x + 2 + sel * sw, y + 2, sw - 4, h - 4, 7, rgb(99, 99, 104), 255);
    for (int i = 0; i < n; i++) {
        int tw = tw_n(opt[i], 13, -1); if (tw > sw - 8) tw = sw - 8;
        draw_text(d, x + i * sw + (sw - tw) / 2, y + h / 2 + 5, opt[i], 13, 0xFFFF, sw - 8, c0, c1);
        if (i && i != sel && i != sel + 1) for (int yy = y + 8; yy < y + h - 8; yy++) d[yy * W + x + i * sw] = rgb(70, 70, 74);
        ab_hit(x + i * sw, y, x + (i + 1) * sw, y + h, kind, i, c0, c1);
    }
}
static void ab_store_row(uint16_t *d, int y, int i, int c0, int c1, int last)
{
    if (y + AB_ROW < c0 || y > c1) return;
    struct ab_hit *h = &ab_res.h[i];
    if (ab_pressed(H_ROW, i)) for (int yy = y; yy < y + AB_ROW; yy++) if (yy >= c0 && yy < c1) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(44, 44, 46);
    int tw, th; ab_thumb_spec(ab_src, 0, &tw, &th);
    char tp[300]; ab_thumb_path(h->id, ab_src, tw, th, tp, sizeof tp);
    const uint16_t *im = ab_img_get(tp, tw, th);
    int ix = 16 + (56 - tw) / 2, iy = y + (AB_ROW - th) / 2;
    if (im) bk_blit_scaled(d, im, tw, th, ix, iy, tw, th, 32, 5, c0, c1);
    else if (iy >= c0 && iy + th <= c1) fill_rrect(d, ix, iy, tw, th, 5, rgb(44, 44, 48), 255);
    int x = 84, mw = W - x - 16;
    int local = ab_src == 0 && ab_mine_find(h->id) >= 0;
    if (local) mw -= 24;
    draw_text(d, x, y + 25, h->title, 15, 0xFFFF, mw, c0, c1);
    draw_text(d, x, y + 44, h->author[0] ? h->author : tr("Anónimo"), 13, AB_GRAY, mw, c0, c1);
    char meta[48] = "";
    if (ab_src == 1) snprintf(meta, sizeof meta, "EPUB · Project Gutenberg");
    else if (h->runtime[0]) ab_fmt_dur(ab_parse_dur(h->runtime), meta, sizeof meta);
    draw_text(d, x, y + 62, meta, 12, rgb(99, 99, 104), mw, c0, c1);
    if (local) ab_check_glyph(d, W - 26, y + AB_ROW / 2, AB_BLUE, c0, c1);
    if (!last) ab_hline(d, x, W, y + AB_ROW - 1, AB_SEP, c0, c1);
    ab_hit(0, y, W, y + AB_ROW, H_ROW, i, c0, c1);
}
/* barra de busqueda: y = su arriba; con "Cancelar" si se esta buscando */
static void ab_searchbar(uint16_t *d, int y, int c0, int c1)
{
    if (y + 36 < c0 || y > c1) return;
    int cancel_w = ab_searching ? tw_n(tr("Cancelar"), 16, -1) + 14 : 0;
    int fw = W - 32 - cancel_w;
    uint16_t bg = ab_pressed(H_SEARCH, 0) ? rgb(44, 44, 48) : rgb(28, 28, 30);
    if (y >= c0 && y + 36 <= c1) {
        fill_rrect(d, 16, y, fw, 36, 10, bg, 255);
        ab_glyph_search(d, 34, y + 18, AB_GRAY, bg);
        const char *txt = ab_searching ? ab_eb.b : ab_q;
        if (txt && txt[0]) draw_text(d, 50, y + 24, txt, 15, 0xFFFF, fw - 64, c0, c1);
        else draw_text(d, 50, y + 24, tr("Buscar"), 15, AB_GRAY, fw - 64, c0, c1);
        if (ab_searching && ab_eb.len && !kb_off) {   /* cursor */
            int cx = 50 + tw_n(ab_eb.b, 15, ab_eb.cur); if (cx > 16 + fw - 30) cx = 16 + fw - 30;
            for (int yy = y + 8; yy < y + 28; yy++) for (int xx = cx; xx < cx + 2; xx++) d[yy * W + xx] = AB_BLUE;
        } else if (ab_searching && !kb_off) for (int yy = y + 8; yy < y + 28; yy++) for (int xx = 50; xx < 52; xx++) d[yy * W + xx] = AB_BLUE;
        if (txt && txt[0]) {   /* borrar */
            int cx = 16 + fw - 18; fill_circle(d, cx, y + 18, 8, AB_GRAY, 255);
            for (int k = -3; k <= 3; k++) { fill_circle(d, cx + k, y + 18 + k, 0.9f, bg, 255); fill_circle(d, cx + k, y + 18 - k, 0.9f, bg, 255); }
            ab_hit(16 + fw - 36, y, 16 + fw, y + 36, H_SCLEAR, 0, c0, c1);
        }
        if (ab_searching) draw_text(d, W - 16 - cancel_w + 10, y + 24, tr("Cancelar"), 16, ab_pressed(H_SCANCEL, 0) ? rgb(5, 66, 128) : AB_BLUE, 0, c0, c1);
    }
    ab_hit(16, y, 16 + fw - (ab_searching ? 36 : 0), y + 36, H_SEARCH, 0, c0, c1);
    if (ab_searching) ab_hit(W - 16 - cancel_w, y, W, y + 36, H_SCANCEL, 0, c0, c1);
}
/* contenido de la tienda desde y0; devuelve el alto */
static int ab_render_store_body(uint16_t *d, int y0, int c0, int c1)
{
    int y = y0;
    if (!ab_searching) {
        static const char *srcs[2] = { "Audiolibros", "Libros" };
        const char *o[2] = { tr(srcs[0]), tr(srcs[1]) };
        ab_segment(d, 16, y + 4, W - 32, 30, o, 2, ab_src, H_SRC, c0, c1);
        y += 44;
        ab_searchbar(d, y, c0, c1);
        y += 46;
    }
    /* idioma (y orden en LibriVox) */
    {
        char lb[80]; snprintf(lb, sizeof lb, "%s", ab_lang_name(ab_lang_cur()));
        int lw = tw_n(lb, 13, -1) + 60; if (lw > 160) lw = 160;
        if (y + 4 >= c0 && y + 34 <= c1) {
            fill_rrect(d, 16, y + 4, lw, 30, 15, ab_pressed(H_LANG, 0) ? rgb(58, 58, 62) : rgb(44, 44, 48), 255);
            /* globo */
            fill_circle(d, 31, y + 19, 7, AB_BLUE, 255); fill_circle(d, 31, y + 19, 5.6f, rgb(44, 44, 48), 255);
            fill_rrect(d, 30, y + 12, 2, 14, 1, AB_BLUE, 255); fill_rrect(d, 24, y + 18, 14, 2, 1, AB_BLUE, 255);
            draw_text(d, 43, y + 24, lb, 13, 0xFFFF, lw - 50, c0, c1);
            for (int k = 0; k < 4; k++) { fill_circle(d, 16 + lw - 16 + k, y + 17 + k, 0.9f, AB_GRAY, 255); fill_circle(d, 16 + lw - 16 + 6 - k, y + 17 + k, 0.9f, AB_GRAY, 255); }
        }
        ab_hit(16, y + 4, 16 + lw, y + 34, H_LANG, 0, c0, c1);
        if (ab_src == 0 && !ab_searching) {
            static const char *so[2] = { "Populares", "Novedades" };
            const char *o[2] = { tr(so[0]), tr(so[1]) };
            int sw = W - 16 - (16 + lw + 10); if (sw > 190) sw = 190;
            ab_segment(d, W - 16 - sw, y + 4, sw, 30, o, 2, ab_sort, H_SORT, c0, c1);
        }
        y += 44;
    }
    char sec[160];
    if (ab_searching && !ab_qsent[0]) {
        ab_wrap(d, 24, y + 30, W - 48, tr(ab_src ? "Busca libros por título o autor." : "Busca audiolibros por título o autor."), 14, AB_GRAY, 20, 3, c0, c1);
        return y + 80 - y0;
    }
    if (ab_searching) snprintf(sec, sizeof sec, "%s", tr("RESULTADOS"));
    else snprintf(sec, sizeof sec, "%s", tr(ab_src ? "LIBROS POPULARES" : ab_sort ? "NOVEDADES" : "AUDIOLIBROS POPULARES"));
    if (y + 20 > c0 && y < c1) draw_text(d, 16, y + 16, sec, 12, AB_GRAY, W - 100, c0, c1);
    if (ab_res_state == 2 && ab_res.found > 0) {
        char nb[32]; snprintf(nb, sizeof nb, "%ld", ab_res.found); int nw = tw_n(nb, 12, -1);
        if (y + 20 > c0 && y < c1) draw_text(d, W - 16 - nw, y + 16, nb, 12, rgb(99, 99, 104), 0, c0, c1);
    }
    y += 24;
    if (ab_res_state == 1 && !ab_res.n) {
        if (y + 60 < c1 && y > c0 - 40) ab_spin(d, W / 2, y + 44, c0, c1);
        const char *m = tr(ab_searching ? "Buscando…" : "Cargando…"); draw_text(d, (W - tw_n(m, 14, -1)) / 2, y + 86, m, 14, AB_GRAY, 0, c0, c1);
        if (ab_src == 1 && now_ms() - ab_jl_t > 6000) { ab_wrap(d, 32, y + 116, W - 64, tr("Project Gutenberg puede tardar hasta un minuto en responder."), 12, rgb(99, 99, 104), 17, 3, c0, c1); return y + 170 - y0; }
        return y + 110 - y0;
    }
    if (ab_res_state == 3 && !ab_res.n) {
        int h = ab_wrap(d, 24, y + 30, W - 48, access(NETBIN, X_OK) ? tr("Falta el componente de red (hibynet)") : tr("No se pudo conectar. Comprueba la WiFi y toca para reintentar."), 14, AB_GRAY, 20, 4, c0, c1);
        ab_hit(0, y, W, y + 40 + h, H_RETRY, 0, c0, c1);
        return y + 50 + h - y0;
    }
    if (ab_res_state == 2 && !ab_res.n) { draw_text(d, 24, y + 30, tr("Sin resultados"), 15, AB_GRAY, W - 48, c0, c1); return y + 50 - y0; }
    for (int i = 0; i < ab_res.n; i++, y += AB_ROW) ab_store_row(d, y, i, c0, c1, i == ab_res.n - 1);
    y += 16;
    const char *foot = tr(ab_src ? "Libros de dominio público de Project Gutenberg (gutenberg.org)." : "Audiolibros de dominio público grabados por voluntarios de LibriVox (librivox.org).");
    y += ab_wrap(d, 16, y + 14, W - 32, foot, 11, rgb(99, 99, 104), 15, 3, c0, c1) + 10;
    return y - y0;
}
static float ab_content_h[8];

/* ------------------------------------------------------------------ dibujo: ficha */
static int ab_render_book(uint16_t *d, int y0, int c0, int c1)
{
    int y = y0 + 10;
    int gut = ab_cur_src == 2;
    const char *title = gut ? ab_gcur.title : ab_cur.title, *author = gut ? ab_gcur.author : ab_cur.author;
    if (ab_cur_state == 1 || ab_cur_state == 3) {
        if (ab_cur_state == 1) { ab_spin(d, W / 2, y + 120, c0, c1); return 200; }
        int h = ab_wrap(d, 24, y + 100, W - 48, tr("No se pudo conectar. Comprueba la WiFi y toca para reintentar."), 14, AB_GRAY, 20, 4, c0, c1);
        ab_hit(0, y + 60, W, y + 120 + h, H_RETRY, 0, c0, c1);
        return 200;
    }
    /* portada */
    int cw, ch; ab_thumb_spec(gut, 1, &cw, &ch);
    char tp[300]; const uint16_t *im = 0;
    int k = gut ? -1 : ab_mine_find(ab_cur_id);
    if (k >= 0) { ab_cov_path(ab_cur_id, tp, sizeof tp); im = ab_img_get(tp, AB_CS, AB_CS); if (im) bk_blit_scaled(d, im, AB_CS, AB_CS, 16, y, cw, ch, 32, 6, c0, c1); }
    if (!im) { ab_thumb_path(ab_cur_id, gut, cw, ch, tp, sizeof tp); im = ab_img_get(tp, cw, ch); if (im) bk_blit_scaled(d, im, cw, ch, 16, y, cw, ch, 32, 6, c0, c1); }
    if (!im) { if (gut) { if (y >= c0 && y + ch <= c1) fill_rrect(d, 16, y, cw, ch, 6, rgb(44, 44, 48), 255); } else bk_blit_scaled(d, ab_gen_cover(title, author), AB_CS, AB_CS, 16, y, cw, ch, 32, 6, c0, c1); }
    int tx = 16 + cw + 14, tw = W - tx - 16;
    int th = ab_wrap(d, tx, y + 18, tw, title, 17, 0xFFFF, 22, 4, c0, c1);
    draw_text(d, tx, y + 18 + th + 2, author[0] ? author : tr("Anónimo"), 14, AB_BLUE, tw, c0, c1);
    char meta[120];
    if (gut) snprintf(meta, sizeof meta, "EPUB · Project Gutenberg");
    else { char du[32], sz[24]; ab_fmt_dur(ab_cur.totdur, du, sizeof du); ab_fmt_size(ab_cur.total, sz, sizeof sz); snprintf(meta, sizeof meta, "%s · %s", du, sz); }
    draw_text(d, tx, y + 18 + th + 22, meta, 12, AB_GRAY, tw, c0, c1);
    if (!gut) { char cs[48]; if (ab_cur.nsec == 1) snprintf(cs, sizeof cs, "%s", tr("1 capítulo")); else snprintf(cs, sizeof cs, tr("%d capítulos"), ab_cur.nsec); draw_text(d, tx, y + 18 + th + 39, cs, 12, AB_GRAY, tw, c0, c1); }
    y += (ch > th + 60 ? ch : th + 60) + 16;
    /* boton principal */
    {
        char lb[120]; int blue = 1; float frac = -2; int spin = 0;
        if (gut) {
            char dest[600]; char nm[200], s2[300]; snprintf(s2, sizeof s2, "%s - %s", author[0] ? author : "Gutenberg", title); ab_sanitize(s2, nm, 90);
            snprintf(dest, sizeof dest, "%s/Books/%s.epub", FS_SD, nm);
            struct stat sb;
            if (!stat(dest, &sb)) { snprintf(lb, sizeof lb, "%s", tr("En la Biblioteca")); blue = 0; }
            else if (!strcmp(ab_ds.dest, dest) && !strcmp(ab_ds.state, "dl")) { snprintf(lb, sizeof lb, "%s", tr("Descargando…")); frac = ab_ds.total > 0 ? (float)ab_ds.got / ab_ds.total : 0; }
            else { char pp[620]; snprintf(pp, sizeof pp, "%s.part", dest); int nq; struct ab_qe *q = ab_q_load(AB_Q, &nq); int inq = 0; for (int i = 0; i < nq; i++) if (!strcmp(q[i].dest, dest)) inq = 1; free(q);
                if (inq && !strcmp(ab_ds.dest, dest) && !ab_ds.alive) snprintf(lb, sizeof lb, "%s", tr(!strcmp(ab_ds.state, "nospace") ? "No hay espacio en la SD" : "Reintentar descarga"));
                else { snprintf(lb, sizeof lb, "%s", inq ? tr("En cola…") : tr("Descargar EPUB")); if (inq) spin = 1; } }
        } else if (ab_play_id && !strcmp(ab_play_book, ab_cur.id)) { snprintf(lb, sizeof lb, "%s", tr("Abriendo…")); spin = 1; }
        else if (ab_nqueued) {
            frac = ab_bytes_target > 0 ? (float)(ab_bytes_done + (!strcmp(ab_ds.book, ab_cur.id) && !strcmp(ab_ds.state, "dl") ? ab_ds.got : 0)) / ab_bytes_target : 0;
            if (ab_paused) snprintf(lb, sizeof lb, tr("En pausa · %d %%"), (int)(frac * 100));
            else if (!strcmp(ab_ds.state, "nospace")) snprintf(lb, sizeof lb, "%s", tr("No hay espacio en la SD"));
            else if (!ab_ds.alive) snprintf(lb, sizeof lb, "%s", tr("Reintentar descarga"));
            else snprintf(lb, sizeof lb, tr("Descargando · %d %%"), (int)(frac * 100));
        } else if (ab_ndone) {
            struct ab_pos p; int has = ab_pos_load(ab_cur.id, &p) && p.opened > 0 && !p.done && (p.ch > 0 || p.pos > 0);
            if (has) snprintf(lb, sizeof lb, tr("Reanudar · Capítulo %d"), p.ch + 1); else snprintf(lb, sizeof lb, "%s", tr("Escuchar"));
        } else { char sz[24]; ab_fmt_size(ab_cur.total, sz, sizeof sz); snprintf(lb, sizeof lb, tr("Descargar · %s"), sz); }
        int bx = 16, bw = W - 32, bh = 44;
        if (y >= c0 && y + bh <= c1) {
            int pr = ab_pressed(H_MAIN, 0);
            if (frac > -1) {   /* barra de progreso dentro del boton */
                fill_rrect(d, bx, y, bw, bh, 12, rgb(44, 44, 48), 255);
                int fw = (int)(bw * (frac < 0 ? 0 : frac > 1 ? 1 : frac)); if (fw > 24) fill_rrect(d, bx, y, fw, bh, 12, rgb(10, 80, 160), 255);
                int lw = tw_n(lb, 15, -1); draw_text(d, bx + (bw - lw) / 2, y + 28, lb, 15, 0xFFFF, bw - 20, c0, c1);
            } else {
                fill_rrect(d, bx, y, bw, bh, 12, blue ? (pr ? rgb(5, 90, 180) : AB_BLUE) : (pr ? rgb(58, 58, 62) : rgb(44, 44, 48)), 255);
                int lw = tw_n(lb, 16, -1), ix = 0;
                int play = !gut && ab_ndone && !ab_nqueued && !spin;
                if (play) ix = 18;
                int sx = bx + (bw - lw - ix) / 2;
                if (play) ab_play_glyph(d, sx + 4, y + 22, 7, 0xFFFF);
                if (spin) spn_draw(d, sx - 18, y + 22, spn_phase(now_ms()), 8, c0, c1);
                draw_text(d, sx + ix, y + 28, lb, 16, blue ? 0xFFFF : AB_BLUE, bw - 20, c0, c1);
            }
        }
        ab_hit(bx, y, bx + bw, y + bh, H_MAIN, 0, c0, c1);
        y += bh + 14;
    }
    /* descripcion */
    const char *desc = gut ? ab_gcur.desc : ab_cur.desc;
    if (desc[0]) {
        int lines = ab_desc_open ? 40 : 5;
        int h = ab_wrap(d, 16, y + 14, W - 32, desc, 13, rgb(200, 200, 205), 18, lines, c0, c1);
        int full = ab_wrap(0, 16, 0, W - 32, desc, 13, 0, 18, 40, 0, 0);
        if (!ab_desc_open && full > h) { const char *m = tr("más"); draw_text(d, W - 16 - tw_n(m, 13, -1), y + 14 + h + 2, m, 13, AB_BLUE, 0, c0, c1); ab_hit(0, y, W, y + h + 24, H_MORE, 0, c0, c1); h += 18; }
        y += h + 14;
    }
    if (gut) return y - y0 + 20;
    /* capitulos */
    ab_hline(d, 16, W, y, AB_SEP, c0, c1);
    y += 10;
    char hd[80]; snprintf(hd, sizeof hd, "%s", tr("CAPÍTULOS"));
    if (y + 22 > c0 && y < c1) draw_text(d, 16, y + 16, hd, 12, AB_GRAY, 0, c0, c1);
    if (ab_ndone + ab_nqueued < ab_cur.nsec && (ab_ndone || ab_nqueued)) {
        const char *a = tr("Descargar todo"); int aw = tw_n(a, 13, -1);
        if (y + 22 > c0 && y < c1) draw_text(d, W - 16 - aw, y + 16, a, 13, ab_pressed(H_ALL, 0) ? rgb(5, 66, 128) : AB_BLUE, 0, c0, c1);
        ab_hit(W - 32 - aw, y - 4, W, y + 26, H_ALL, 0, c0, c1);
    }
    y += 24;
    struct ab_pos pos; int haspos = ab_pos_load(ab_cur.id, &pos) && pos.opened > 0;
    int playing = -1;
    if (ab_np_ok && !strncmp(ab_np.path, ab_cur_dir, strlen(ab_cur_dir)) && ab_np.path[strlen(ab_cur_dir)] == '/') {
        const char *f = ab_np.path + strlen(ab_cur_dir) + 1;
        for (int i = 0; i < ab_cur.nsec; i++) if (!strcmp(ab_cur.sec[i].file, f)) playing = i;
    }
    for (int i = 0; i < ab_cur.nsec; i++, y += AB_SROW) {
        if (y + AB_SROW < c0 || y > c1) continue;
        if (ab_pressed(H_SEC, i)) for (int yy = y; yy < y + AB_SROW; yy++) if (yy >= c0 && yy < c1) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(44, 44, 46);
        char nb[8]; snprintf(nb, sizeof nb, "%d", i + 1);
        uint16_t tc = ab_sst[i] == 3 ? 0xFFFF : rgb(170, 170, 176);
        if (i == playing) tc = AB_BLUE;
        draw_text(d, 16 + (24 - tw_n(nb, 13, -1)) / 2, y + 31, nb, 13, i == playing ? AB_BLUE : AB_GRAY, 0, c0, c1);
        char tb[64]; draw_text(d, 52, y + 23, ab_sec_name(i, tb, sizeof tb), 15, tc, W - 52 - 50, c0, c1);
        char du[32]; ab_fmt_dur(ab_cur.sec[i].dur, du, sizeof du);
        if (haspos && pos.ch == i && pos.pos > 0 && ab_cur.sec[i].dur > 0) { char rest[40]; ab_fmt_dur(ab_cur.sec[i].dur - pos.pos / 1000, rest, sizeof rest); snprintf(du, sizeof du, tr("Quedan %s"), rest); }
        draw_text(d, 52, y + 41, du, 12, AB_GRAY, W - 52 - 50, c0, c1);
        int cx = W - 30, cy = y + AB_SROW / 2;
        if (ab_sst[i] == 2) { float f = ab_ds.total > 0 ? (float)ab_ds.got / ab_ds.total : 0; if (cy - 14 > c0 && cy + 14 < c1) ab_ring(d, cx, cy, 11, f, 1, c0, c1); }
        else if (ab_sst[i] == 1) { if (cy - 14 > c0 && cy + 14 < c1) { if (ab_paused) ab_ring(d, cx, cy, 11, 0, 1, c0, c1); else ab_ring(d, cx, cy, 11, -1, 0, c0, c1); } }
        else if (ab_sst[i] == 0) ab_dl_glyph(d, cx, cy, AB_BLUE, c0, c1);
        else if (i == playing && ab_np.playing && cy - 8 > c0 && cy + 8 < c1) { for (int b = 0; b < 3; b++) { int hb = 4 + (int)(6 * fabs(sin(now_ms() / 180.0 + b * 1.7))); fill_rrect(d, cx - 7 + b * 5, cy + 6 - hb, 3, hb, 1, AB_BLUE, 255); } }
        ab_hline(d, 52, W, y + AB_SROW - 1, AB_SEP, c0, c1);
        ab_hit(0, y, W, y + AB_SROW, H_SEC, i, c0, c1);
    }
    return y - y0 + 20;
}

/* ------------------------------------------------------------------ dibujo: idiomas */
static int ab_render_lang(uint16_t *d, int y0, int c0, int c1)
{
    int y = y0 + 12, cur = ab_lang_cur();
    /* "Todos" primero */
    int order[AB_NLANG]; int n = 0; order[n++] = AB_NLANG - 1; for (int i = 0; i < AB_NLANG - 1; i++) order[n++] = i;
    for (int r = 0; r < n; r++, y += 46) {
        int i = order[r];
        if (y + 46 < c0 || y > c1) continue;
        if (ab_pressed(H_LANGROW, i)) for (int yy = y; yy < y + 46; yy++) if (yy >= c0 && yy < c1) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(44, 44, 46);
        draw_text(d, 20, y + 29, ab_lang_name(i), 16, 0xFFFF, W - 80, c0, c1);
        if (i == cur) ab_check_glyph(d, W - 30, y + 23, AB_BLUE, c0, c1);
        if (r < n - 1) ab_hline(d, 20, W, y + 45, AB_SEP, c0, c1);
        ab_hit(0, y, W, y + 46, H_LANGROW, i, c0, c1);
    }
    return y - y0 + 10;
}

/* ------------------------------------------------------------------ composicion */
static void ab_render(uint16_t *d)
{
    ab_nht = 0;
    struct ab_pg *pg = AB_PG;
    int root = ab_spn[bk_tab] == 1;
    int searching_hdr = pg->kind == AP_STORE && ab_searching;
    int top = searching_hdr ? 78 : root ? AB_HDR : AB_NAV;
    int bot = ab_bottom();
    for (int i = 30 * W; i < W * H; i++) d[i] = 0;
    memcpy(d, fr1, 30 * W * 2);
    int y0 = top - (int)pg->scroll, h = 0;
    if (searching_hdr) {   /* la barra de busqueda sube a la cabecera */
        h = ab_render_store_body(d, top - (int)pg->scroll, top, bot);
        ab_fill(d, 30, top, 0);
        ab_searchbar(d, 36, 30, bot);
        if (pg->scroll > 4) ab_hline(d, 0, W, top - 1, AB_SEP, 0, H);
    } else {
        switch (pg->kind) {
        case AP_MINE: ab_render_mine(d, pg->scroll); h = ((ab_nmine + 1) / 2) * (AB_GW + 54) + 30; break;
        case AP_STORE: h = ab_render_store_body(d, y0, top, bot); break;
        case AP_BOOK: h = ab_render_book(d, y0, top, bot); break;
        case AP_LANG: h = ab_render_lang(d, y0, top, bot); break;
        }
        /* cabecera */
        ab_fill(d, 30, top, 0);
        if (root) {
            draw_text(d, 16, 76, bk_tab == 1 ? tr("Audiolibros") : "Gutenberg", 28, 0xFFFF, W - 32, 0, H);
        } else {
            const struct ab_pg *prev = &ab_stk[bk_tab][ab_spn[bk_tab] - 2];
            const char *bt = prev->kind == AP_MINE ? tr("Audiolibros") : prev->kind == AP_STORE ? "Gutenberg" : tr("Atrás");
            uint16_t c = ab_pressed(H_BACK, 0) ? rgb(5, 66, 128) : AB_BLUE;
            ab_chev_left(d, 16, 53, c); draw_text(d, 30, 59, bt, 17, c, 140, 0, H);
            ab_hit(0, 30, 160, AB_NAV, H_BACK, 0, 0, H);
            if (pg->kind == AP_LANG) { const char *t = tr("Idioma"); draw_text(d, (W - tw_n(t, 17, -1)) / 2, 59, t, 17, 0xFFFF, 0, 0, H); }
            if (pg->kind == AP_BOOK && ab_cur_src == 0 && ab_cur_state == 2 && (ab_ndone || ab_nqueued)) {
                fill_circle(d, W - 30, 53, 14, ab_pressed(H_DOTS, 0) ? rgb(70, 70, 76) : rgb(44, 44, 48), 255);
                for (int k = -1; k <= 1; k++) fill_circle(d, W - 30 + k * 6, 53, 1.7f, AB_BLUE, 255);
                ab_hit(W - 60, 30, W, AB_NAV, H_DOTS, 0, 0, H);
            }
            if (pg->scroll > 4) ab_hline(d, 0, W, top - 1, AB_SEP, 0, H);
        }
        if (root) ab_hline(d, 0, W, AB_HDR - 1, AB_SEP, 0, H);
    }
    ab_content_h[bk_tab * 2 + (root ? 0 : 1)] = (float)h;
    if (ab_mini_on()) ab_minibar(d, AB_TABY - AB_MINIH);
    ab_tabbar(d, bk_tab, 1);
    draw_toast(d);
}
static float ab_maxscroll(void)
{
    struct ab_pg *pg = AB_PG; int root = ab_spn[bk_tab] == 1;
    if (pg->kind == AP_MINE) return ab_mine_max();
    int top = pg->kind == AP_STORE && ab_searching ? 78 : root ? AB_HDR : AB_NAV;
    float m = ab_content_h[bk_tab * 2 + (root ? 0 : 1)] - (ab_bottom() - top);
    return m < 0 ? 0 : m;
}
static void ab_kb_redraw(uint16_t *dst, int pressed) { ab_render(dst); if (ab_searching) kb_draw(dst, pressed); }
static void ab_present(void) { ab_kb_redraw(fr0, -1); present(); }

/* ------------------------------------------------------------------ reproducir y progreso */
static int ab_np_ours(void)
{
    char root[300]; ab_root(root, sizeof root); int l = (int)strlen(root);
    return ab_np.path[0] && !ab_np.stopped && !strncmp(ab_np.path, root, l) && ab_np.path[l] == '/';
}
/* indice del capitulo a partir del nombre "NN - ..." */
static int ab_np_chapter(void) { const char *f = strrchr(ab_np.path, '/'); return f ? atoi(f + 1) - 1 : -1; }
static double ab_saved_t; static int ab_saved_ch = -1, ab_saved_pos = -1;
/* mira lo que suena; guarda el progreso del audiolibro que suena (cada 20 s, al cambiar de
 * capitulo y al salir). Devuelve 1 si cambio algo visible. */
static int ab_np_tick(int force)
{
    double t = now_ms();
    if (!force && t - ab_np_t < 1000) return 0;
    ab_np_t = t;
    struct np_info prev = ab_np; int was = ab_np_ok;
    ab_np_ok = np_get(&ab_np) > 0;
    if (!ab_np_ok || !ab_np_ours()) { ab_np_mine = -1; return was != ab_np_ok || (was && strcmp(prev.path, ab_np.path)); }
    char dir[400]; snprintf(dir, sizeof dir, "%s", ab_np.path); char *s = strrchr(dir, '/'); if (s) *s = 0;
    ab_np_mine = -1; for (int i = 0; i < ab_nmine; i++) if (!strcmp(ab_mine[i].dir, dir)) ab_np_mine = i;
    if (ab_np_mine < 0) return 1;
    struct ab_mi *m = &ab_mine[ab_np_mine];
    int ch = ab_np_chapter(), pos = ab_np.position_ms > 0 ? ab_np.position_ms : 0;
    if (ch < 0 || ch >= m->nsec) return 1;
    if (force || ch != ab_saved_ch || (ab_np.playing && t - ab_saved_t > 20000 && abs(pos - ab_saved_pos) > 3000)) {
        struct ab_pos p; ab_pos_load(m->id, &p);
        p.ch = ch; p.pos = pos; p.opened = (double)time(0);
        float in = ab_np.duration_ms > 0 ? (float)pos / ab_np.duration_ms : 0;
        p.pct = m->nsec ? (ch + in) / m->nsec : 0;
        p.done = ch == m->nsec - 1 && in > 0.98f;
        ab_pos_save(m->id, &p);
        m->ch = p.ch; m->pos = p.pos; m->opened = p.opened; m->pct = p.pct; m->done = p.done;
        ab_saved_t = t; ab_saved_ch = ch; ab_saved_pos = pos;
    }
    return prev.playing != ab_np.playing || strcmp(prev.path, ab_np.path) || prev.position_ms / 1000 != ab_np.position_ms / 1000;
}
/* reproduce el libro abierto desde el capitulo ch (pos_ms dentro de el) */
static void ab_play(int ch, int pos_ms)
{
    if (ab_cur_state != 2 || !ab_ndone) return;
    if (ch < 0) ch = 0; if (ch >= ab_cur.nsec) ch = ab_cur.nsec - 1;
    const char **paths = malloc(ab_cur.nsec * sizeof *paths); char *buf = malloc(ab_cur.nsec * 600);
    if (!paths || !buf) { free(paths); free(buf); return; }
    int n = 0, idx = 0;
    for (int i = 0; i < ab_cur.nsec; i++) {
        if (ab_sst[i] != 3) continue;
        if (i <= ch) idx = n;
        snprintf(buf + n * 600, 600, "%s/%s", ab_cur_dir, ab_cur.sec[i].file); paths[n] = buf + n * 600; n++;
    }
    if (ab_sst[ch] != 3) pos_ms = 0;
    unlink(PC_RES);
    int id = pc_list_send_pos(paths, n, idx, ab_cur.title, 0 | PC_UNSCANNED, pos_ms);
    lg("audiolibro: reproducir %d pistas desde %d (%d ms): peticion %d", n, idx, pos_ms, id);
    free(paths); free(buf);
    if (id < 0) { toast(tr(id == -ENXIO ? "El reproductor no está disponible" : "No se pudo reproducir")); return; }
    ab_play_id = id; ab_play_t = now_ms(); snprintf(ab_play_book, sizeof ab_play_book, "%s", ab_cur.id);
    struct ab_pos p; ab_pos_load(ab_cur.id, &p); int k = ab_mine_find(ab_cur.id);
    if (p.ch != ch) p.pos = 0;
    p.ch = ch; if (pos_ms >= 0) p.pos = pos_ms; p.opened = (double)time(0); p.done = 0;
    ab_pos_save(ab_cur.id, &p);
    if (k >= 0) { ab_mine[k].opened = p.opened; ab_mine[k].ch = p.ch; ab_mine[k].done = 0; }
    ab_saved_ch = ch;
}
static int ab_open_player(void);
/* respuesta del reproductor: 1 si hay que repintar */
static int ab_play_poll(void)
{
    if (!ab_play_id) return 0;
    struct pc_list_res r;
    int k = pc_list_poll(ab_play_id, &r);
    if (!k && now_ms() - ab_play_t < 5000) return 0;
    ab_play_id = 0;
    if (!k) { toast(tr("El reproductor no respondió")); lg("audiolibro: sin respuesta del reproductor"); return 1; }
    lg("audiolibro: respuesta rc=%d filas=%d indice=%d %s", r.rc, r.rows, r.index, r.msg);
    if (r.rc < 0 || r.rows <= 0) { toast(tr("No se pudo reproducir")); return 1; }
    ab_np_t = 0; ab_np_tick(0);
    return 2;   /* abrir Reproduciendo */
}

/* ------------------------------------------------------------------ acciones de descarga */
static int ab_enqueue(int only)   /* only < 0: todo lo que falte */
{
    if (!sd_mounted()) { toast(tr("La SD está conectada al ordenador")); return -1; }
    if (access(NETBIN, X_OK)) { toast(tr("Falta el componente de red (hibynet)")); return -1; }
    long long need = 0; int n = 0;
    struct ab_qe *e = calloc(ab_cur.nsec + 1, sizeof *e); if (!e) return -1;
    char cover[600]; snprintf(cover, sizeof cover, "%s/cover.jpg", ab_cur_dir);
    if (access(cover, F_OK)) {
        struct ab_qe *c = &e[n++]; char cu[300], cv[200];
        ab_cover_url(ab_cur.id, cu, sizeof cu); ab_cov_path(ab_cur.id, cv, sizeof cv);
        snprintf(c->book, sizeof c->book, "%s", ab_cur.id); c->kind = 'c'; snprintf(c->url, sizeof c->url, "%s|%s", cu, cv); snprintf(c->dest, sizeof c->dest, "%s", cover);
    }
    for (int i = 0; i < ab_cur.nsec; i++) {
        if ((only >= 0 && i != only) || ab_sst[i]) continue;
        struct ab_qe *q = &e[n++];
        char rem[400]; ab_urlenc(ab_cur.sec[i].remote, rem, sizeof rem, "/"); char idq[200]; ab_urlenc(ab_cur.id, idq, sizeof idq, 0);
        snprintf(q->book, sizeof q->book, "%s", ab_cur.id); q->kind = 'm'; q->size = ab_cur.sec[i].size;
        snprintf(q->url, sizeof q->url, "https://archive.org/download/%s/%s", idq, rem);
        snprintf(q->dest, sizeof q->dest, "%s/%s", ab_cur_dir, ab_cur.sec[i].file);
        need += ab_cur.sec[i].size;
    }
    long long fb = ab_free_bytes(FS_SD);
    if (fb >= 0 && fb < need + AB_MARGIN) {
        char m[160], a[24], b[24]; ab_fmt_size(need, a, sizeof a); ab_fmt_size(fb > AB_MARGIN ? fb - AB_MARGIN : 0, b, sizeof b);
        snprintf(m, sizeof m, tr("No hay espacio en la SD: hacen falta %s y quedan %s."), a, b);
        ab_render(fr0); confirm(m, tr("Aceptar")); free(e); return -1;
    }
    if (ab_mkdirs(ab_cur_dir) || ab_book_save(&ab_cur, (snprintf(cover, sizeof cover, "%s/book.json", ab_cur_dir), cover))) { toast(tr("No se pudo escribir en la SD")); free(e); return -1; }
    ab_q_add(e, n); free(e);
    ab_set_paused(ab_cur.id, 0);
    ab_worker_start();
    ab_mine_scan(); ab_ds_read(); ab_sec_refresh();
    return 0;
}
static void ab_gut_download(void)
{
    if (!sd_mounted()) { toast(tr("La SD está conectada al ordenador")); return; }
    if (access(NETBIN, X_OK)) { toast(tr("Falta el componente de red (hibynet)")); return; }
    struct ab_qe e; memset(&e, 0, sizeof e);
    char nm[200], s2[300]; snprintf(s2, sizeof s2, "%s - %s", ab_gcur.author[0] ? ab_gcur.author : "Gutenberg", ab_gcur.title); ab_sanitize(s2, nm, 90);
    snprintf(e.book, sizeof e.book, "gutenberg:%s", ab_gcur.id); e.kind = 'e';
    ab_gut_epub_url(ab_gcur.id, e.url, sizeof e.url);
    snprintf(e.dest, sizeof e.dest, "%s/Books/%s.epub", FS_SD, nm);
    ab_q_add(&e, 1); ab_worker_start(); ab_ds_read();
}
/* pausa / sigue / cancela las descargas del libro abierto */
static void ab_dl_restart(void) { ab_worker_stop(); ab_worker_start(); }
static void ab_cancel_book(const char *id)
{
    ab_q_remove(id, 0); ab_set_paused(id, 0); ab_dl_restart();
    ab_mine_scan();
    int k = ab_mine_find(id);
    if (k >= 0 && !ab_mine[k].ndone) {   /* no llego a bajar ningun capitulo: fuera la carpeta */
        rm_rf(ab_mine[k].dir);
        char p[200]; ab_cov_path(id, p, sizeof p); ab_img_forget(p, AB_CS, AB_CS); unlink(p);
        ab_mine_scan();
    }
}
static void ab_delete_book(int k)
{
    if (k < 0 || k >= ab_nmine) return;
    char id[96]; snprintf(id, sizeof id, "%s", ab_mine[k].id);
    ab_q_remove(id, 0); ab_set_paused(id, 0);
    if (!strcmp(ab_ds.book, id)) ab_dl_restart();
    int r = rm_rf(ab_mine[k].dir);
    char p[200]; ab_pos_path(id, p, sizeof p); unlink(p); ab_cov_path(id, p, sizeof p); ab_img_forget(p, AB_CS, AB_CS); unlink(p);
    toast(r ? tr("No se pudo eliminar del todo") : tr("Eliminado"));
    ab_mine_scan();
    if (!strcmp(ab_cur_id, id)) ab_cur_reset();
}

/* ------------------------------------------------------------------ navegacion */
/* transicion de pila de iOS (como fs_transition) pero la barra de pestanas (y el mini
 * reproductor) quedan quietos: solo se desliza el contenido entre la barra de estado y ellos */
static void ab_transition(const uint16_t *from, const uint16_t *to, int dir)
{
    int y1 = AB_TABY - (ab_mini_on() ? AB_MINIH : 0);
    memcpy(fr0 + y1 * W, to + y1 * W, (H - y1) * W * 2);
    memcpy(fr0, from, 30 * W * 2);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 300; if (t > 1) t = 1;
        float e = (float)ease_out(t);
        int off = (int)(W * e);
        const uint16_t *top = dir > 0 ? to : from, *bot = dir > 0 ? from : to;
        int tx = dir > 0 ? W - off : off, bx = dir > 0 ? -off / 3 : -(W - off) / 3;
        int k = dir > 0 ? 32 - (int)(10 * e) : 22 + (int)(10 * e);
        for (int y = 30; y < y1; y++) {
            uint16_t *dr = fr0 + y * W; const uint16_t *tr_ = top + y * W, *br = bot + y * W;
            for (int x = 0; x < W; x++) {
                if (x >= tx) dr[x] = tr_[x - tx];
                else { int sx = x - bx; dr[x] = sx >= 0 && sx < W ? dim565(br[sx], k) : 0; if (x >= tx - 6) dr[x] = dim565(dr[x], 24 + (x - tx + 6)); }
            }
        }
        present();
        if (t >= 1) break;
    }
}
static void ab_push(int kind, const char *id, int src)
{
    if (ab_spn[bk_tab] >= 4) return;
    ab_render(img_base);
    struct ab_pg *pg = &ab_stk[bk_tab][ab_spn[bk_tab]++];
    memset(pg, 0, sizeof *pg); pg->kind = kind; if (id) snprintf(pg->id, sizeof pg->id, "%s", id); pg->src = src;
    if (kind == AP_BOOK) ab_cur_open(id, src);
    ab_press_k = H_NONE; ab_press_i = -1;
    ab_render(img_app);
    ab_transition(img_base, img_app, 1);
    ab_present();
}
static void ab_pop(int animate)
{
    if (ab_spn[bk_tab] <= 1) return;
    if (animate) ab_render(img_base);
    ab_spn[bk_tab]--;
    struct ab_pg *pg = AB_PG;
    if (pg->kind == AP_BOOK) ab_cur_open(pg->id, pg->src);
    if (pg->kind == AP_MINE) ab_mine_scan();
    ab_press_k = H_NONE; ab_press_i = -1;
    if (animate) { ab_render(img_app); ab_transition(img_base, img_app, -1); }
    ab_present();
}
static int ab_open_player(void)
{
    ab_render(fr0);
    static uint16_t *under; if (!under) under = malloc(HM_FRAME_BYTES);
    if (!under) return 0;
    memcpy(under, fr0, HM_FRAME_BYTES);
    int r = np_app_run(under, -1, -1);   /* al cerrarlo se ve la pestana (debajo) */
    (void)r;
    ab_np_t = 0; ab_np_tick(1);
    return 0;
}
/* busqueda: entrar / salir */
static void ab_search_begin(void)
{
    if (ab_searching) { if (kb_off) kb_reveal(fr0, ab_kb_redraw); return; }
    static char buf[128]; snprintf(buf, sizeof buf, "%s", ab_q);
    ab_eb = (struct ebuf){ buf, (int)strlen(buf), (int)sizeof buf, (int)strlen(buf) };
    ab_searching = 1; snprintf(ab_qsent, sizeof ab_qsent, "%s", ab_q); AB_PG->scroll = 0;
    if (ab_qsent[0]) ab_list_want(ab_qsent);
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Buscar"));
    kb_begin(&ab_eb, fr0, ab_kb_redraw);
}
static void ab_search_end(void)
{
    if (!ab_searching) return;
    if (!kb_off) kb_hide();
    kb_end();
    ab_searching = 0; ab_q[0] = 0; ab_qsent[0] = 0; ab_deb_t = 0; AB_PG->scroll = 0;
    ab_list_want(0);
    ab_present();
}
static void ab_search_fire(void)
{
    ab_deb_t = 0;
    char q[128]; snprintf(q, sizeof q, "%s", ab_eb.b); ab_trim(q);
    if (!strcmp(q, ab_qsent)) return;
    snprintf(ab_qsent, sizeof ab_qsent, "%s", q); snprintf(ab_q, sizeof ab_q, "%s", q);
    AB_PG->scroll = 0;
    if (q[0]) ab_list_want(q); else { ab_job_kill(&ab_jl); ab_res.n = 0; ab_res_state = 0; ab_want_key[0] = 0; }
}

/* ------------------------------------------------------------------ trabajo de fondo */
/* revisa red, descargas y reproductor; 1 si hay que repintar */
static int ab_poll(void)
{
    int ch = 0, r;
    if ((r = ab_job_done(&ab_jl))) {
        if (!strcmp(ab_jl.key, ab_want_key)) {
            if (r > 0 && !ab_list_parse(ab_jl.out)) { snprintf(ab_res_key, sizeof ab_res_key, "%s", ab_jl.key); ab_res_state = 2; }
            else if (ab_res_state != 2 || strcmp(ab_res_key, ab_want_key)) { ab_res.n = 0; ab_res_state = 3; }
        }
        ch = 1;
    }
    if ((r = ab_job_done(&ab_jm))) {
        if (!strcmp(ab_jm.key, ab_cur_id) && ab_cur_src == 0) {
            if (r > 0 && !ab_meta_parse(ab_jm.out, &ab_cur)) ab_cur_loaded();
            else { ab_book_free(&ab_cur); ab_cur_state = 3; unlink(ab_jm.out); }
        }
        ch = 1;
    }
    if (ab_jt.pid && (r = ab_job_done(&ab_jt))) {
        if (r > 0 && ab_jt.kind == AJ_THUMB) {   /* imagen descargada: a RGB565 */
            char ws[8], hs[8]; snprintf(ws, sizeof ws, "%d", ab_jt.w); snprintf(hs, sizeof hs, "%d", ab_jt.h);
            char jpg[300]; snprintf(jpg, sizeof jpg, "%s", ab_jt.out);
            char *argv[] = { ASSETS "/hibywall", jpg, ab_jt.wall_out, ws, hs, 0 };
            ab_jt.pid = ab_spawn(argv); ab_jt.kind = AJ_WALL; if (ab_jt.pid <= 0) ab_jt.pid = 0;
        } else {
            if (r < 0) { snprintf(ab_thumb_fail, sizeof ab_thumb_fail, "%s", ab_jt.key); ab_thumb_fail_t = now_ms(); if (ab_jt.kind == AJ_WALL) unlink(ab_jt.out); }
            ab_jt.kind = AJ_NONE; ch = 1;
        }
    }
    if (!ab_jt.pid) ab_thumbs_next();
    /* miniaturas en cache -> memoria (pocas por vuelta) */
    {
        int loaded = 0; struct ab_pg *pg = AB_PG;
        if (pg->kind == AP_STORE && ab_res_state == 2) for (int i = 0; i < ab_res.n && loaded < 4; i++) {
            int w, h; ab_thumb_spec(ab_src, 0, &w, &h); char tp[300]; ab_thumb_path(ab_res.h[i].id, ab_src, w, h, tp, sizeof tp);
            if (!ab_img_known(tp, w, h) && !access(tp, R_OK)) { ab_img_load(tp, w, h); loaded++; }
        }
        if (pg->kind == AP_BOOK && ab_cur_id[0]) {
            int w, h; ab_thumb_spec(ab_cur_src == 2, 1, &w, &h); char tp[300]; ab_thumb_path(ab_cur_id, ab_cur_src == 2, w, h, tp, sizeof tp);
            if (!ab_img_known(tp, w, h) && !access(tp, R_OK)) { ab_img_load(tp, w, h); loaded++; }
        }
        for (int i = 0; i < ab_nmine && loaded < 4; i++) {   /* solo las portadas que se ven */
            int vis = i == ab_np_mine;
            if (pg->kind == AP_MINE) { int x, y; ab_cell_xy(i, pg->scroll, &x, &y); vis |= y + AB_GW > AB_HDR && y < AB_TABY; (void)x; }
            if (pg->kind == AP_BOOK) vis |= !strcmp(ab_mine[i].id, ab_cur_id);
            if (!vis) continue;
            char cp[200]; ab_cov_path(ab_mine[i].id, cp, sizeof cp);
            if (!ab_img_known(cp, AB_CS, AB_CS) && !access(cp, R_OK)) { ab_img_load(cp, AB_CS, AB_CS); loaded++; }
        }
        if (loaded) ch = 1;
    }
    /* descargas */
    if (now_ms() - ab_ds_t > 500) {
        ab_ds_t = now_ms();
        struct ab_ds old = ab_ds; ab_ds_read();
        if (strcmp(old.dest, ab_ds.dest) || strcmp(old.state, ab_ds.state)) {
            ab_mine_scan(); ab_sec_refresh(); ch = 1;
            if (!strcmp(ab_ds.state, "nospace") && strcmp(old.state, "nospace")) toast(tr("No hay espacio en la SD"));
            /* un EPUB terminado: la Biblioteca lo vera al volver */
        } else if (old.got != ab_ds.got) ch = 1;
    }
    if (ab_np_tick(0)) ch = 1;
    if ((r = ab_play_poll())) ch = r;
    return ch;
}

/* ------------------------------------------------------------------ toques */
static void ab_tap(int kind, int idx)
{
    struct ab_pg *pg = AB_PG;
    switch (kind) {
    case H_BACK: ab_pop(1); break;
    case H_GOSTORE: bk_tab = 2; if (!ab_spn[2]) { ab_spn[2] = 1; ab_stk[2][0].kind = AP_STORE; } ab_list_want(ab_searching ? ab_qsent : 0); ab_present(); break;
    case H_SEARCH: ab_search_begin(); break;
    case H_SCANCEL: ab_search_end(); break;
    case H_SCLEAR:
        if (ab_searching) { ab_eb.b[0] = 0; ab_eb.len = ab_eb.cur = 0; ab_deb_t = 0; ab_search_fire(); if (kb_off) kb_reveal(fr0, ab_kb_redraw); else ab_present(); }
        else { ab_q[0] = 0; ab_present(); }
        break;
    case H_SRC: if (idx >= 0 && idx != ab_src) { ab_src = idx; pg->scroll = 0; ab_list_want(0); ab_present(); } break;
    case H_SORT: if (idx >= 0 && idx != ab_sort) { ab_sort = idx; pg->scroll = 0; ab_list_want(0); ab_present(); } break;
    case H_LANG: if (ab_searching && !kb_off) kb_hide(); ab_push(AP_LANG, 0, 0); break;
    case H_LANGROW:
        snprintf(ab_langc, sizeof ab_langc, "%s", ab_langs[idx].code); bk_conf_save();
        ab_pop(1);
        AB_PG->scroll = 0; ab_want_key[0] = 0; ab_res_state = 0; ab_list_want(ab_searching ? ab_qsent : 0); ab_present();
        break;
    case H_RETRY: if (pg->kind == AP_BOOK) { char id[96]; snprintf(id, sizeof id, "%s", pg->id); ab_cur_reset(); ab_cur_open(id, pg->src); } else ab_list_retry(); ab_present(); break;
    case H_ROW:
        if (idx < 0 || idx >= ab_res.n) break;
        if (ab_searching && !kb_off) kb_hide();
        if (ab_src == 1) { ab_gcur = ab_res.h[idx]; ab_cur_reset(); }
        ab_push(AP_BOOK, ab_res.h[idx].id, ab_src == 1 ? 2 : 0);
        break;
    case H_CELL: if (idx >= 0 && idx < ab_nmine) ab_push(AP_BOOK, ab_mine[idx].id, 0); break;
    case H_MORE: ab_desc_open = 1; ab_present(); break;
    case H_MINIPP: pc_toggle(); ab_np.playing = !ab_np.playing; ab_present(); ab_np_t = now_ms() - 600; break;
    case H_MINI: ab_open_player(); ab_present(); break;
    case H_ALL: ab_enqueue(-1); ab_present(); break;
    case H_MAIN:
        if (ab_cur_state != 2) break;
        if (ab_cur_src == 2) {
            char dest[600], nm[200], s2[300]; snprintf(s2, sizeof s2, "%s - %s", ab_gcur.author[0] ? ab_gcur.author : "Gutenberg", ab_gcur.title); ab_sanitize(s2, nm, 90);
            snprintf(dest, sizeof dest, "%s/Books/%s.epub", FS_SD, nm);
            if (!access(dest, F_OK)) { bk_tab = 0; break; }       /* "En la Biblioteca": a la pestana */
            if (!strcmp(ab_ds.dest, dest) && !ab_ds.alive) { unlink(AB_STAT); ab_worker_start(); ab_present(); break; }   /* reintentar */
            ab_gut_download(); ab_present(); break;
        }
        if (ab_play_id) break;
        if (ab_nqueued) {
            int stuck = !ab_paused && !ab_ds.alive;   /* sin espacio, sin red o sin SD: el proceso se paro */
            const char *o[2] = { stuck ? tr("Reintentar descarga") : ab_paused ? tr("Reanudar descarga") : tr("Pausar descarga"), tr("Cancelar descarga") };
            ab_render(fr0);
            int a = action_sheet(ab_cur.title, o, 2, 1);
            if (a == 0 && stuck) { unlink(AB_STAT); ab_worker_start(); }
            else if (a == 0) { ab_set_paused(ab_cur.id, !ab_paused); if (ab_paused) ab_worker_start(); else if (!strcmp(ab_ds.book, ab_cur.id)) ab_dl_restart(); }
            else if (a == 1) ab_cancel_book(ab_cur.id);
            ab_mine_scan(); ab_ds_read(); ab_sec_refresh(); ab_present();
            break;
        }
        if (ab_ndone) { struct ab_pos p; int has = ab_pos_load(ab_cur.id, &p) && !p.done; ab_play(has ? p.ch : 0, has ? p.pos : 0); ab_present(); break; }
        ab_enqueue(-1); ab_present();
        break;
    case H_SEC:
        if (idx < 0 || idx >= ab_cur.nsec) break;
        if (ab_sst[idx] == 3) { struct ab_pos p; int has = ab_pos_load(ab_cur.id, &p) && p.ch == idx; ab_play(idx, has ? p.pos : 0); }
        else if (ab_sst[idx] == 0) ab_enqueue(idx);
        else {   /* en cola o descargando: quitarlo */
            char dest[600]; snprintf(dest, sizeof dest, "%s/%s", ab_cur_dir, ab_cur.sec[idx].file);
            int cur = !strcmp(ab_ds.dest, dest);
            ab_q_remove(ab_cur.id, dest); if (cur) ab_dl_restart();
            ab_mine_scan(); ab_ds_read(); ab_sec_refresh();
        }
        ab_present();
        break;
    case H_DOTS: {
        int k = ab_mine_find(ab_cur.id);
        const char *o[3] = { tr("Empezar desde el principio"), tr("Marcar como terminado"), tr("Eliminar descarga") };
        ab_render(fr0);
        int a = action_sheet(ab_cur.title, o, 3, 2);
        struct ab_pos p; ab_pos_load(ab_cur.id, &p);
        if (a == 0) { memset(&p, 0, sizeof p); ab_pos_save(ab_cur.id, &p); }
        else if (a == 1) { p.done = 1; p.pct = 1; if (p.opened <= 0) p.opened = (double)time(0); ab_pos_save(ab_cur.id, &p); }
        else if (a == 2) {
            char q[300]; snprintf(q, sizeof q, tr("¿Eliminar «%s»? No se puede deshacer."), ab_cur.title);
            ab_render(fr0);
            if (confirm(q, tr("Eliminar")) && k >= 0) { ab_delete_book(k); if (AB_PG->kind == AP_BOOK) ab_pop(1); return; }
        }
        ab_mine_scan(); ab_present();
        break;
    }
    }
}
/* pulsacion larga en una portada de Audiolibros */
static void ab_cell_menu(int i)
{
    if (i < 0 || i >= ab_nmine) return;
    struct ab_mi *m = &ab_mine[i];
    const char *o[3] = { m->done ? tr("Marcar como no leído") : tr("Marcar como terminado"), tr("Empezar desde el principio"), tr("Eliminar") };
    ab_render(fr0);
    int a = action_sheet(m->title, o, 3, 2);
    struct ab_pos p; ab_pos_load(m->id, &p);
    if (a == 0) { p.done = !m->done; p.pct = p.done ? 1 : 0; if (!p.done) { p.ch = 0; p.pos = 0; p.opened = 0; } else if (p.opened <= 0) p.opened = (double)time(0); ab_pos_save(m->id, &p); }
    else if (a == 1) { p.ch = 0; p.pos = 0; p.pct = 0; p.done = 0; ab_pos_save(m->id, &p); }
    else if (a == 2) {
        char q[300]; snprintf(q, sizeof q, tr("¿Eliminar «%s»? No se puede deshacer."), m->title);
        ab_render(fr0);
        if (confirm(q, tr("Eliminar"))) ab_delete_book(i);
    }
    ab_mine_scan(); ab_present();
}

/* ------------------------------------------------------------------ bucle */
static void ab_tab_init(int tab)
{
    if (!ab_spn[tab]) { ab_spn[tab] = 1; memset(&ab_stk[tab][0], 0, sizeof ab_stk[tab][0]); ab_stk[tab][0].kind = tab == 1 ? AP_MINE : AP_STORE; }
    if (tab == 2) ab_list_want(ab_searching ? ab_qsent : 0);
    if (tab == 1) ab_mine_scan();
}
/* al abrir Libros (antes de pintar nada): estado, reanudar descargas pendientes */
static void ab_open(void)
{
    ab_mine_scan(); ab_ds_read(); ab_np_t = 0; ab_np_tick(0);
    ab_cache_prune();
    int n; struct ab_qe *q = ab_q_load(AB_Q, &n); free(q);
    if (n && !ab_ds.alive && sd_mounted()) ab_worker_start();   /* reanudar al volver (vuelve a mirar el espacio) */
    if (bk_tab) ab_tab_init(bk_tab);
}
/* Biblioteca en reposo: 1 si acaba de terminar la descarga de un EPUB (hay que volver a escanear) */
static int ab_lib_epub_done(void)
{
    static double t; static char last[600];
    if (now_ms() - t < 1000) return 0;
    t = now_ms();
    char was[600]; snprintf(was, sizeof was, "%s", last);
    ab_ds_read();
    snprintf(last, sizeof last, "%s", !strcmp(ab_ds.state, "dl") ? ab_ds.dest : "");
    int l = (int)strlen(was);
    return l > 5 && !strcasecmp(was + l - 5, ".epub") && strcmp(was, last) && !access(was, F_OK);
}
static void ab_close(void)
{
    ab_np_tick(1);
    if (ab_searching) { kb_end(); ab_searching = 0; ab_q[0] = ab_qsent[0] = 0; }
    ab_job_kill(&ab_jl); ab_job_kill(&ab_jm); ab_job_kill(&ab_jt);
    ab_img_free(); ab_cur_reset();
    free(ab_mine); ab_mine = 0; ab_nmine = 0;
    ab_want_key[0] = 0; ab_res_state = 0;
    for (int t = 1; t < 3; t++) while (ab_spn[t] > 1) ab_spn[t]--;          /* las fichas no se conservan */
}
/* pinta la pestana actual en d (para las transiciones de books.inc.c) */
static void ab_render_tab(uint16_t *d) { ab_tab_init(bk_tab); ab_render(d); }
/* bucle de las pestanas 1 y 2. Devuelve 0 si se vuelve a la Biblioteca (bk_tab = 0) o 1 si se
 * sale de la app (el inicio ya esta en pantalla) */
static int ab_run(void)
{
    ab_tab_init(bk_tab);
    ab_present();
    float vel = 0; struct pkt p; double lt_fl = 0;
    for (;;) {
        if (!bk_tab) return 0;
        int ch = ab_poll();
        if (ch == 2) { ab_open_player(); ch = 1; }
        if (ab_deb_t && now_ms() >= ab_deb_t) { ab_search_fire(); ch = 1; }
        if (fabsf(vel) > 0.03f) {
            double t = now_ms(); if (!lt_fl || t - lt_fl > 100) lt_fl = t - 16;
            float dt = (float)(t - lt_fl); lt_fl = t; struct ab_pg *pg = AB_PG; pg->scroll -= vel * dt; vel *= powf(0.9975f, dt);
            float mx = ab_maxscroll(); if (pg->scroll < 0) { pg->scroll = 0; vel = 0; } if (pg->scroll > mx) { pg->scroll = mx; vel = 0; }
            ab_present();
            if (!read_pkt(&p, 0)) continue;
            vel = 0; if (p.down) { drain_until_up(); continue; }
        } else {
            int busy = ab_jl.pid || ab_jm.pid || ab_jt.pid || ab_play_id || ab_deb_t || ab_res_state == 1 || ab_cur_state == 1 || (ab_ds.alive && !strcmp(ab_ds.state, "dl"));
            int anim = (ab_res_state == 1 && AB_PG->kind == AP_STORE) || ab_play_id || (ab_nqueued && AB_PG->kind == AP_BOOK && !ab_paused) || (ab_np_ok && ab_np.playing && AB_PG->kind == AP_BOOK);
            if (ch || anim || now_ms() < toast_until + 40) { ab_present(); }
            if (!read_pkt(&p, anim ? 80 : busy ? 100 : 400)) continue;
        }
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms();
        /* teclado de la busqueda */
        if (ab_searching && !kb_off && sy >= KB_Y) {
            char before[128]; snprintf(before, sizeof before, "%s", ab_eb.b);
            int r = kb_touch(&p, &ab_eb, fr0, ab_kb_redraw, 0);
            if (strcmp(before, ab_eb.b)) { ab_deb_t = now_ms() + 400; ab_present(); }
            if (r == K_ENTER) { kb_hide(); ab_search_fire(); ab_present(); }
            if (r == K_HOME) { kb_hide(); ab_render(fr0); if (own_home_gesture(sx, sy, app_by_id("books"))) return 1; ab_present(); }   /* inicio con el teclado abierto */
            continue;
        }
        int tabhit = -1, idx = -1, k = H_NONE;
        if (sy >= 456) {   /* desde abajo: inicio; si no sube, es un toque en la barra de pestanas */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { if (ab_searching && !kb_off) kb_hide(); ab_render(fr0); if (own_home_gesture(sx, sy, app_by_id("books"))) return 1; ab_present(); continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            int i2; if (ab_hit_at(sx, sy, &i2) == H_TAB) tabhit = i2; else continue;
        }
        if (tabhit < 0 && ab_spn[bk_tab] > 1 && back_zone(sx, sy)) {
            ab_render(fr0);
            int bk = app_back_gesture(sx, sy);
            if (bk > 0) { ab_pop(1); continue; }
            if (bk < 0) continue;
        }
        if (tabhit < 0) {
            k = ab_hit_at(sx, sy, &idx);
            if (k == H_TAB) { while (cur_down && read_pkt(&p, 3000)) {} tabhit = idx; }
        }
        if (tabhit >= 0) {
            if (tabhit == bk_tab) {   /* otra vez la misma pestana: al principio */
                if (ab_spn[bk_tab] > 1) { while (ab_spn[bk_tab] > 2) ab_spn[bk_tab]--; ab_pop(1); }
                else { AB_PG->scroll = 0; ab_present(); }
                continue;
            }
            if (ab_searching && !kb_off) kb_hide();
            bk_tab = tabhit;
            if (!bk_tab) return 0;
            ab_tab_init(bk_tab); ab_press_k = H_NONE; ab_present();
            continue;
        }
        ab_press_k = k; ab_press_i = idx;
        if (k != H_NONE) ab_present();
        int moved = 0, ly = sy, longp = 0; double lt = ts; struct ab_pg *pg = AB_PG; float s0 = pg->scroll; vel = 0;
        while (cur_down) {
            int got = read_pkt(&p, 30);
            if (got && !p.down) break;
            if (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 12) {
                if (!moved && ab_searching && !kb_off) { ab_press_k = H_NONE; kb_hide(); }
                moved = 1;
            }
            if (moved) {
                ab_press_k = H_NONE;
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                float mx = ab_maxscroll(); pg->scroll = s0 - (cur_y - sy); if (pg->scroll < 0) pg->scroll = 0; if (pg->scroll > mx) pg->scroll = mx;
                ab_present();
            } else if (k == H_CELL && now_ms() - ts > 450) { longp = 1; break; }
        }
        if (moved) { if (now_ms() - lt > 60) vel = 0; lt_fl = 0; continue; }
        vel = 0;
        ab_press_k = H_NONE; ab_press_i = -1;
        if (longp) { while (cur_down && read_pkt(&p, 3000)) {} ab_cell_menu(idx); continue; }
        if (k == H_CELL) {   /* los tres puntos de la celda: menu */
            int x, y; ab_cell_xy(idx, pg->scroll, &x, &y);
            if (sy > y + AB_GW + 14 && sx > x + AB_GW - 34) { ab_cell_menu(idx); continue; }
        }
        if (k == H_NONE) { ab_present(); continue; }
        ab_tap(k, idx);
        if (!bk_tab) return 0;
        ab_present();
    }
}
