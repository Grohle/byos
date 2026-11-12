/* lyrics.inc.c - Buscar caratula y letra para canciones (editor de etiquetas y Archivos).
 *
 * Automatico: bibliotecas publicas a traves de `hibynet` (src/hibynet.c):
 *   caratulas: iTunes Search + MusicBrainz/Cover Art Archive; letras: LRCLIB (sincronizadas).
 * Manual: cambiar los terminos de busqueda con el teclado, o elegir una imagen de la SD
 * (por ejemplo, descargada desde el ordenador o con el navegador).
 * Donde se guarda (lo que lee el reproductor de HiBy): ver docs/LYRICS_AND_COVERS.md.
 *   - caratula: cover.jpg en la carpeta del album
 *   - letra: <cancion>.lrc junto al fichero de audio */
#define NETBIN "/usr/data/hibymod/shell/hibynet"
#define NETDIR "/usr/data/hibymod/cache/net"

struct cover_c { char src[16], url[300], artist[96], album[96], year[8]; uint16_t *thumb; };


static void dir_of(const char *path, char *out, int cap);
/* pistas de audio de la carpeta (el album): para aplicar a todo el album */
static int is_audio_ext(const char *n)
{
    const char *x = strrchr(n, '.'); if (!x) return 0;
    return !strcasecmp(x, ".flac") || !strcasecmp(x, ".mp3") || !strcasecmp(x, ".m4a") || !strcasecmp(x, ".mp4") || !strcasecmp(x, ".aac");
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char **album_files(const char *path, int *n)
{
    char dir[600]; dir_of(path, dir, sizeof dir);
    DIR *d = opendir(dir); struct dirent *e; char **v = 0; int c = 0, cap = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_ext(e->d_name)) continue;
        if (c == cap) { cap = cap ? cap * 2 : 32; v = realloc(v, cap * sizeof *v); }
        char b[900]; snprintf(b, sizeof b, "%s/%s", dir, e->d_name); v[c++] = strdup(b);
    }
    if (d) closedir(d);
    if (c) qsort(v, c, sizeof *v, cmp_str);
    *n = c; return v;
}
static void free_list(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }
/* "Solo esta cancion" / "Todo el album (N)": 0, 1 o -1 (cancelar) */
static int ask_scope(int nalbum)
{
    char t[80]; snprintf(t, sizeof t, tr("Todo el álbum (%d canciones)"), nalbum);
    const char *o[2] = { tr("Solo esta canción"), t };
    return action_sheet(tr("¿Dónde aplicarlo?"), o, 2, -1);
}

static int net_ready(void)
{
    if (access(NETBIN, X_OK)) { toast(tr("Falta el componente de red (hibynet)")); return 0; }
    mkdir("/usr/data/hibymod/cache", 0755); mkdir(NETDIR, 0755);
    return 1;
}
/* parte una linea "a\tb\tc" en campos (modifica la linea) */
static int split_tabs(char *l, char **f, int max)
{
    int n = 0; f[n++] = l;
    for (char *p = l; *p && n < max; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    for (int i = 0; i < n; i++) f[i][strcspn(f[i], "\r\n")] = 0;
    return n;
}
static void dir_of(const char *path, char *out, int cap)
{
    snprintf(out, cap, "%s", path); char *s = strrchr(out, '/'); if (s) *s = 0;
}
static int copy_file(const char *src, const char *dst)
{
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.part", dst);
    int in = open(src, O_RDONLY); if (in < 0) return -1;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return -1; }
    char b[16384]; ssize_t n; int r = 0;
    while ((n = read(in, b, sizeof b)) > 0) if (write(out, b, n) != n) { r = -1; break; }
    if (n < 0) r = -1;
    if (fsync(out)) r = -1;
    close(in); close(out);
    if (r || rename(tmp, dst)) { unlink(tmp); return -1; }
    return 0;
}

/* barra de navegacion comun: izquierda / titulo / derecha */
static void lx_nav(uint16_t *d, const char *left, const char *title, const char *right)
{
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < 94; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(12, 12, 13);
    for (int x = 0; x < W; x++) d[93 * W + x] = rgb(38, 38, 40);
    if (left) draw_text(d, 14, 68, left, 16, rgb(10, 132, 255), 90, 0, H);
    int tw = tw_n(title, 17, -1); if (tw > 140) tw = 140;
    draw_text(d, (W - tw) / 2, 68, title, 17, 0xFFFF, 140, 0, H);
    if (right) draw_text(d, W - 14 - tw_n(right, 16, -1), 68, right, 16, rgb(10, 132, 255), 0, 0, H);
}

/* ------------------------------------------------------------------ caratulas */
#define CV_MAX 9
#define CV_T 96
static struct cover_c cv[CV_MAX]; static int ncv;
static int cover_fetch(const char *artist, const char *album)
{
    for (int i = 0; i < ncv; i++) free(cv[i].thumb);
    ncv = 0;
    progress_card(0, 0, -1); progress_card(tr("Buscando carátulas"), 0, 0.15);
    char *argv[] = { NETBIN, "covers", (char *)artist, (char *)album, 0 };
    static char out[16384];
    if (run_capture(argv, out, sizeof out) <= 0) return 0;
    for (char *l = strtok(out, "\n"); l && ncv < CV_MAX; l = strtok(0, "\n")) {
        char *f[5]; if (split_tabs(l, f, 5) < 2 || strncmp(f[1], "http", 4)) continue;
        struct cover_c *c = &cv[ncv]; memset(c, 0, sizeof *c);
        snprintf(c->src, sizeof c->src, "%s", f[0]); snprintf(c->url, sizeof c->url, "%s", f[1]);
        if (f[2]) snprintf(c->artist, sizeof c->artist, "%s", f[2]);
        if (f[3]) snprintf(c->album, sizeof c->album, "%s", f[3]);
        if (f[4]) snprintf(c->year, sizeof c->year, "%s", f[4]);
        ncv++;
    }
    /* descargar cada candidata (jpg 500 px + miniatura RGB565 96x96) */
    for (int i = 0; i < ncv; i++) {
        char sub[48]; snprintf(sub, sizeof sub, tr("%d de %d"), i + 1, ncv);
        progress_card(tr("Descargando carátulas"), sub, 0.2 + 0.8 * i / ncv);
        char jp[128], th[140]; snprintf(jp, sizeof jp, NETDIR "/c%d.jpg", i); snprintf(th, sizeof th, NETDIR "/c%d.thumb.raw", i);
        /* sin los de la busqueda anterior: si esta descarga falla no debe aparecer (ni guardarse)
         * la caratula vieja del mismo hueco */
        unlink(jp); unlink(th);
        char *a2[] = { NETBIN, "cover-get", cv[i].url, jp, "500", 0 }; char o[256];
        if (run_capture(a2, o, sizeof o) < 0) continue;
        int fd = open(th, O_RDONLY);
        if (fd >= 0) { cv[i].thumb = malloc(CV_T * CV_T * 2); if (read(fd, cv[i].thumb, CV_T * CV_T * 2) != CV_T * CV_T * 2) { free(cv[i].thumb); cv[i].thumb = 0; } close(fd); }
        if (cv[i].thumb && access(jp, R_OK)) { free(cv[i].thumb); cv[i].thumb = 0; }
    }
    /* fuera las que no se pudieron descargar (los ficheros c%d siguen el indice: renombrar) */
    int k = 0;
    for (int i = 0; i < ncv; i++) {
        if (!cv[i].thumb) continue;
        if (k != i) {
            char a1[128], b1[128], a2[140], b2[140];
            snprintf(a1, sizeof a1, NETDIR "/c%d.jpg", i); snprintf(b1, sizeof b1, NETDIR "/c%d.jpg", k); rename(a1, b1);
            snprintf(a2, sizeof a2, NETDIR "/c%d.thumb.raw", i); snprintf(b2, sizeof b2, NETDIR "/c%d.thumb.raw", k); rename(a2, b2);
            cv[k] = cv[i];
        }
        k++;
    }
    lg("caratulas: %d candidatas, %d descargadas", ncv, k);
    ncv = k;
    return ncv;
}
static void cover_render(uint16_t *d, const char *q, int sel)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    draw_text(d, 16, 116, q, 12, rgb(142, 142, 147), W - 32, 0, H);
    for (int i = 0; i < ncv; i++) {
        int x = 8 + (i % 3) * (CV_T + 8), y = 128 + (i / 3) * (CV_T + 26);
        if (cv[i].thumb) for (int yy = 0; yy < CV_T; yy++) memcpy(d + (y + yy) * W + x, cv[i].thumb + yy * CV_T, CV_T * 2);
        else fill_rrect(d, x, y, CV_T, CV_T, 6, rgb(44, 44, 48), 255);
        if (i == sel) fill_rrect(d, x, y, CV_T, CV_T, 6, 0xFFFF, 70);
        char cap[48]; snprintf(cap, sizeof cap, "%s%s%s", cv[i].src, cv[i].year[0] ? " · " : "", cv[i].year);
        draw_text(d, x, y + CV_T + 15, cap, 11, rgb(142, 142, 147), CV_T, 0, H);
    }
    if (!ncv) draw_wrapped(d, 24, 150, W - 48, tr("No se encontraron carátulas. Cambia la búsqueda o elige una imagen de la tarjeta SD."), 14, rgb(152, 152, 159), 19, 1);
    fill_rrect(d, 16, H - 52, W - 32, 40, 12, rgb(44, 44, 48), 255);
    int w = tw_n(tr("Elegir imagen de la tarjeta SD"), 15, -1);
    draw_text(d, (W - w) / 2, H - 27, tr("Elegir imagen de la tarjeta SD"), 15, rgb(10, 132, 255), W - 40, 0, H);
    lx_nav(d, tr("Cancelar"), tr("Carátula"), tr("Buscar"));
    draw_toast(d);
}
static char *const *cv_paths; static int cv_n;
/* incrusta la portada en cada cancion (escritura segura de hibytag: temporal + fsync + rename) */
static int cover_save(const char *img, const char *dir)
{
    (void)dir;
    const char *ext = strrchr(img, '.');
    if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) { toast(tr("Usa una imagen JPG")); return -1; }
    char **alb = 0; int nalb = 0; char *const *keep_p = cv_paths; int keep_n = cv_n;
    if (cv_n == 1) {
        alb = album_files(cv_paths[0], &nalb);
        if (nalb > 1) { int sc = ask_scope(nalb); if (sc < 0) { free_list(alb, nalb); return -1; } if (sc == 1) { cv_paths = alb; cv_n = nalb; } }
    }
    progress_card(0, 0, -1);
    int bad = 0, big = 0;
    for (int i = 0; i < cv_n; i++) {
        char sub[48]; snprintf(sub, sizeof sub, tr("%d de %d"), i + 1, cv_n);
        progress_card(tr("Incrustando carátula"), sub, (double)i / cv_n);
        int r = ht_set_cover(cv_paths[i], img);
        if (r) { bad++; if (r == HT_ETOOBIG) big = 1; }
    }
    /* biblioteca de HiBy: actualizar las filas de estas canciones */
    progress_card(tr("Actualizando la biblioteca"), 0, 0.99);
    char **argv = calloc(cv_n + 3, sizeof(char *)); int na = 2; argv[0] = TAGBIN; argv[1] = "dbupdate";
    for (int i = 0; i < cv_n; i++) argv[na++] = cv_paths[i];
    char o[256]; run_capture(argv, o, sizeof o); free(argv);
    sync();
    toast(big ? tr("La imagen pasa de 1 MB") : bad == cv_n ? tr("No se pudo guardar") : bad ? tr("Algunas no se pudieron guardar") : tr("Carátula guardada"));
    int r = bad == cv_n ? -1 : 0;
    cv_paths = keep_p; cv_n = keep_n; if (alb) free_list(alb, nalb);
    return r;
}
static void cover_search(char *const *paths, int n, const char *artist, const char *album)
{
    const char *path = paths[0]; cv_paths = paths; cv_n = n;
    if (!net_ready()) return;
    char a[128], b[128], q[300], dir[600];
    snprintf(a, sizeof a, "%s", artist ? artist : ""); snprintf(b, sizeof b, "%s", album ? album : "");
    dir_of(path, dir, sizeof dir);
    cover_fetch(a, b);
    for (;;) {
        snprintf(q, sizeof q, "%s — %s", a, b);
        cover_render(fr0, q, -1); present();
        struct pkt p;
        if (!read_pkt(&p, now_ms() < toast_until + 40 ? 30 : 400) || !p.down) continue;
        int x = p.x, y = p.y; while (cur_down && read_pkt(&p, 3000)) {}
        if (y >= 456 && cur_y < y - 20) return;
        if (y < 94 && x < 100) return;
        if (y < 94 && x > W - 90) {                       /* buscar a mano: otros terminos */
            /* sobre copias: Cancelar no deja texto a medias; un campo vacio vale (buscar solo
             * por album o por artista); aceptar siempre busca de nuevo */
            char na[128], nb[128]; snprintf(na, sizeof na, "%s", a); snprintf(nb, sizeof nb, "%s", b);
            line_edit(tr("Artista"), tr("Artista"), na, sizeof na); if (!le_accepted) continue;
            line_edit(tr("Álbum"), tr("Álbum"), nb, sizeof nb); if (!le_accepted) continue;
            if (!na[0] && !nb[0]) continue;
            snprintf(a, sizeof a, "%s", na); snprintf(b, sizeof b, "%s", nb);
            cover_fetch(a, b);
            continue;
        }
        if (y >= H - 56) { char img[300]; if (image_picker(img, sizeof img) && !cover_save(img, dir)) return; continue; }
        int i = -1;
        for (int k = 0; k < ncv; k++) { int cx = 8 + (k % 3) * (CV_T + 8), cy = 128 + (k / 3) * (CV_T + 26); if (x >= cx && x < cx + CV_T && y >= cy && y < cy + CV_T) i = k; }
        if (i < 0) continue;
        cover_render(fr0, q, i); present();
        char t[200]; snprintf(t, sizeof t, "%s — %s (%s)", cv[i].artist, cv[i].album, cv[i].src);
        const char *o[1] = { tr("Usar esta carátula") };
        if (action_sheet(t, o, 1, -1) == 0) { char jp[128]; snprintf(jp, sizeof jp, NETDIR "/c%d.jpg", i); if (!cover_save(jp, dir)) return; }
    }
}

/* ------------------------------------------------------------------ letras
 * Busqueda: `hibynet lyrics ARTISTA TITULO ALBUM DURACION_S DIR` en segundo plano (la interfaz
 * sigue a 44 Hz con su indicador); hibynet deja cada resultado en DIR/<id>.lrc, asi que la vista
 * previa es inmediata. Resultados ordenados por lrc_rank (lrc_core.inc.c): sincronizadas,
 * diferencia de duracion con la cancion, coincidencia de titulo/artista.
 * Vista previa sincronizada: si la cancion es la que suena sigue su posicion real; si no, un
 * reloj propio desde 0 (reproducir/pausa). Ajuste de sincronizacion: se guarda como [offset:]
 * (positivo = la letra sale antes, como dice la especificacion LRC).
 * Reproduciendo (nowui.inc.c) usa estas mismas piezas dentro de su vista de letra. */
#include "lrc_core.inc.c"
static void mu_line(uint16_t *d, float x0, float y0, float x1, float y1, float th, uint16_t col);
static void mu_text_bold(uint16_t *d, int x, int base, const char *s, int size, uint16_t col, int maxw);
static int spn_phase(double t);
static void spn_draw(uint16_t *d, int cx, int cy, int phase, int n, int c0, int c1);

#define LX_ELL "\xE2\x80\xA6"                   /* verso vacio (pausa) */
#define LX_DIR NETDIR "/lc"
#define LC_MAX 16
#define LX_ROW 76                               /* alto de una fila de resultado */
#define LX_GREEN rgb(48, 209, 88)
#define LX_ORANGE rgb(255, 159, 10)
#define LX_BLUE rgb(10, 132, 255)
#define LX_OFFMAX 10000                         /* ajuste maximo: +-10 s */

/* ---- hibynet en segundo plano */
static pid_t lx_spawn(char *const argv[], const char *out)
{
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, out ? out : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    return r ? -1 : pid;
}
/* -1: sigue; si no, su codigo de salida (0 ok, 1 nada, 2 red; 99 no se pudo lanzar) */
static int lx_wait(pid_t *pid)
{
    if (*pid <= 0) return 99;
    int st; pid_t w = waitpid(*pid, &st, WNOHANG);
    if (w == 0) return -1;
    *pid = 0;
    return w < 0 ? 99 : WIFEXITED(st) ? WEXITSTATUS(st) : 98;
}
static void lx_kill(pid_t *pid) { if (*pid > 0) { kill(*pid, SIGTERM); int st; waitpid(*pid, &st, 0); } *pid = 0; }

/* ---- busqueda (estado compartido por la pantalla suelta y por Reproduciendo) */
static struct lrc_cand lc[LC_MAX];
static int nlc, lc_want, lc_rc = 1;             /* lc_rc: -1 buscando, 0 hay, 1 nada, 2 red, 99 sin hibynet */
static pid_t lc_pid, lc_get_pid; static int lc_get_i = -1;
static char lc_artist[128], lc_title[160], lc_album[128];
static void lc_cancel(void) { lx_kill(&lc_pid); lx_kill(&lc_get_pid); lc_get_i = -1; if (lc_rc < 0) lc_rc = 1; }
static void lc_start(const char *artist, const char *title, const char *album, int want_s)
{
    lc_cancel(); nlc = 0; lc_want = want_s;
    snprintf(lc_artist, sizeof lc_artist, "%s", artist ? artist : ""); snprintf(lc_title, sizeof lc_title, "%s", title ? title : "");
    snprintf(lc_album, sizeof lc_album, "%s", album ? album : "");
    if (access(NETBIN, X_OK)) { lc_rc = 99; return; }
    mkdir("/usr/data/hibymod/cache", 0755); mkdir(NETDIR, 0755); mkdir(LX_DIR, 0755);
    DIR *d = opendir(LX_DIR); struct dirent *e;                 /* resultados de la busqueda anterior */
    while (d && (e = readdir(d))) if (e->d_name[0] != '.') { char p[300]; snprintf(p, sizeof p, LX_DIR "/%s", e->d_name); unlink(p); }
    if (d) closedir(d);
    char ds[16]; snprintf(ds, sizeof ds, "%d", want_s > 0 ? want_s : 0);
    char *argv[] = { NETBIN, "lyrics", lc_artist, lc_title, lc_album, ds, LX_DIR, 0 };
    lc_pid = lx_spawn(argv, NETDIR "/lc.tsv");
    lc_rc = lc_pid > 0 ? -1 : 99;
    lg("letras: buscando \"%s\" / \"%s\" (%d s)", lc_artist, lc_title, want_s);
}
/* 1 cuando la busqueda acaba de terminar */
static int lc_poll(void)
{
    if (lc_rc != -1) return 0;
    int r = lx_wait(&lc_pid); if (r < 0) return 0;
    static char out[16384]; int n = 0, f = open(NETDIR "/lc.tsv", O_RDONLY);
    if (f >= 0) { n = (int)read(f, out, sizeof out - 1); close(f); }
    out[n > 0 ? n : 0] = 0;
    nlc = lrc_parse_results(out, lc, LC_MAX);
    nlc = lrc_rank(lc, nlc, lc_want, lc_artist, lc_title);
    lc_rc = nlc ? 0 : r == 0 || r == 1 ? 1 : r == 99 || r == 98 ? 99 : 2;
    lg("letras: %d resultados (salida %d)", nlc, r);
    return 1;
}
static void lc_path(int i, char *p, int cap) { snprintf(p, cap, LX_DIR "/%s.lrc", lc[i].id); }
/* el .lrc del resultado i esta listo (1), descargandose (0) o fallo (-1); lo pide si falta */
static int lc_ready(int i)
{
    char p[160]; lc_path(i, p, sizeof p);
    if (lc_get_i == i) {
        int r = lx_wait(&lc_get_pid); if (r < 0) return 0;
        lc_get_i = -1;
        return !r && !access(p, R_OK) ? 1 : -1;
    }
    if (!access(p, R_OK)) return 1;
    lx_kill(&lc_get_pid);
    char *argv[] = { NETBIN, "lyrics-get", lc[i].id, p, 0 };
    lc_get_pid = lx_spawn(argv, 0); lc_get_i = lc_get_pid > 0 ? i : -1;
    return lc_get_pid > 0 ? 0 : -1;
}

/* ---- ficheros */
static int lx_dur_s(const char *path) { static struct np_meta m; np_read_meta(path, &m); return (m.duration_ms + 500) / 1000; }
static char *lx_read(const char *p, int *n)
{
    int f = open(p, O_RDONLY); if (f < 0) return 0;
    struct stat s; if (fstat(f, &s) || s.st_size <= 0 || s.st_size > (512 << 10)) { close(f); return 0; }
    char *b = malloc(s.st_size + 1); if (!b) { close(f); return 0; }
    int k = (int)read(f, b, s.st_size); close(f);
    if (k <= 0) { free(b); return 0; }
    b[k] = 0; *n = k; return b;
}
static int lx_load(const char *p, struct lrc *l)
{
    memset(l, 0, sizeof *l);
    int n; char *b = lx_read(p, &n); if (!b) return -1;
    int r = lrc_parse(l, b, n, LX_ELL); free(b);
    return r;
}
/* .lrc junto a la cancion (el que ya exista, .lrc o .LRC) */
static void lx_lrc_of(const char *audio, char *dst, int cap)
{
    snprintf(dst, cap, "%s", audio);
    char *dot = strrchr(dst, '.'), *sl = strrchr(dst, '/'); if (dot && (!sl || dot > sl)) *dot = 0;
    int k = (int)strlen(dst);
    snprintf(dst + k, cap - k, ".lrc");
    if (access(dst, F_OK)) { snprintf(dst + k, cap - k, ".LRC"); if (access(dst, F_OK)) snprintf(dst + k, cap - k, ".lrc"); }
}
/* guarda src como la letra de "audio" con [offset:off] (temporal + fsync + rename). 0 ok */
static int lx_save(const char *src, const char *audio, int off)
{
    char dst[600], tmp[620]; lx_lrc_of(audio, dst, sizeof dst); snprintf(tmp, sizeof tmp, "%s.part", dst);
    int n; char *b = lx_read(src, &n); if (!b) return -1;
    int on; char *o = lrc_with_offset(b, n, off, &on); free(b); if (!o) return -1;
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644), r = f < 0 ? -1 : 0;
    if (f >= 0) { if (write(f, o, on) != on || fsync(f)) r = -1; close(f); }
    free(o);
    if (r || rename(tmp, dst)) { unlink(tmp); return -1; }
    sync();
    lg("letras: guardada %s (offset %+d ms)", dst, off);
    return 0;
}

/* ---- dibujo comun */
static void lx_check(uint16_t *d, float cx, float cy, uint16_t col) { mu_line(d, cx - 4, cy, cx - 1.3f, cy + 3, 1.7f, col); mu_line(d, cx - 1.3f, cy + 3, cx + 4.3f, cy - 3.5f, 1.7f, col); }
static void lx_neq(uint16_t *d, float cx, float cy, uint16_t col)
{
    fill_rrect(d, (int)cx - 4, (int)cy - 3, 9, 2, 0, col, 255); fill_rrect(d, (int)cx - 4, (int)cy + 1, 9, 2, 0, col, 255);
    mu_line(d, cx + 2.6f, cy - 5, cx - 2.2f, cy + 5, 1.3f, col);
}
/* insignia redondeada con su borde derecho en xr; ic 1 = marca, 2 = distinto. Devuelve su x */
static int lx_pill(uint16_t *d, int xr, int y, const char *t, int ic, uint16_t bg, uint16_t fg)
{
    int tw = tw_n(t, 11, -1), w = tw + 14 + (ic ? 12 : 0), x = xr - w;
    fill_rrect(d, x, y, w, 18, 9, bg, 255);
    int tx = x + 7;
    if (ic == 1) { lx_check(d, tx + 4, y + 9, fg); tx += 12; } else if (ic == 2) { lx_neq(d, tx + 4, y + 9, fg); tx += 12; }
    draw_text(d, tx, y + 13, t, 11, fg, 0, 0, H);
    return x;
}
/* una fila de resultado (alto LX_ROW) sobre un fondo de color bgc */
static void lx_row(uint16_t *d, int x, int y, int w, const struct lrc_cand *c, int want, uint16_t bgc, uint16_t ct, uint16_t cs, uint16_t cq, int busy)
{
    int xr = x + w - 12, x1;
    if (c->synced) x1 = lx_pill(d, xr, y + 11, "LRC", 0, blend565(bgc, LX_GREEN, 9), LX_GREEN);
    else x1 = lx_pill(d, xr, y + 11, tr("Texto"), 0, blend565(bgc, 0xFFFF, 5), cs);
    char dt[24]; int k = lrc_dur_badge(c->dur, want, dt, sizeof dt), x2 = xr;
    if (dt[0]) x2 = lx_pill(d, xr, y + 33, dt, k, k == 1 ? blend565(bgc, LX_GREEN, 9) : k == 2 ? blend565(bgc, LX_ORANGE, 9) : blend565(bgc, 0xFFFF, 5), k == 1 ? LX_GREEN : k == 2 ? LX_ORANGE : cs);
    if (busy) spn_draw(d, x1 - 18, y + 20, spn_phase(now_ms()), 8, 0, H);
    mu_text_bold(d, x + 16, y + 25, c->title, 16, ct, x1 - x - 16 - (busy ? 34 : 8));
    char sub[220];
    int k2 = snprintf(sub, sizeof sub, "%s%s%s", c->artist, c->artist[0] && c->album[0] ? " \xC2\xB7 " : "", c->album);
    if (c->dur > 0 && k2 < (int)sizeof sub - 16) snprintf(sub + k2, sizeof sub - k2, "%s%d:%02d", k2 ? " \xC2\xB7 " : "", c->dur / 60, c->dur % 60);
    draw_text(d, x + 16, y + 45, sub, 13, cs, x2 - x - 24, 0, H);
    if (c->first[0]) { char q[140]; snprintf(q, sizeof q, "\xE2\x80\x9C%s\xE2\x80\x9D", c->first); draw_text(d, x + 16, y + 65, q, 13, cq, w - 28, 0, H); }
}
/* rectangulo redondeado opaco y rapido (filas enteras; solo las esquinas con suavizado) */
static void lx_card(uint16_t *d, int x, int y, int w, int h, int r, uint16_t col)
{
    int ya = y < 0 ? 0 : y, yb = y + h > H ? H : y + h;
    for (int yy = ya; yy < yb; yy++) {
        int j = yy - y, k = j < r ? j : h - 1 - j < r ? h - 1 - j : r;
        uint16_t *row = d + yy * W;
        if (k >= r) { for (int xx = x < 0 ? 0 : x; xx < x + w && xx < W; xx++) row[xx] = col; continue; }
        float dy = r - (k + .5f), e = r - sqrtf((float)r * r - dy * dy);
        int e0 = (int)e; unsigned a = (unsigned)((1 - (e - e0)) * 255);
        for (int xx = x + e0 + 1; xx < x + w - e0 - 1; xx++) if (xx >= 0 && xx < W) row[xx] = col;
        if (x + e0 >= 0 && x + e0 < W) row[x + e0] = mix565(col, row[x + e0], a);
        if (x + w - 1 - e0 >= 0 && x + w - 1 - e0 < W) row[x + w - 1 - e0] = mix565(col, row[x + w - 1 - e0], a);
    }
}
/* boton rectangular redondeado con texto centrado */
static void lx_button(uint16_t *d, int x, int y, int w, int h, const char *t, uint16_t bg, uint16_t fg, int pressed)
{
    fill_rrect(d, x, y, w, h, 12, pressed ? blend565(bg, 0, 8) : bg, 255);
    int tw = tw_n(t, 16, -1); if (tw > w - 16) tw = w - 16;
    mu_text_bold(d, x + (w - tw) / 2, y + h / 2 + 6, t, 16, fg, w - 16);
}
/* ajuste de sincronizacion en [y, y+64): (-) valor y regla deslizable (+). press: 0 menos, 1 mas, 2 regla */
#define LX_ADJ_H 64
#define LX_TRK0 78
#define LX_TRK1 (W - 78)
static void lx_adjust(uint16_t *d, int y, int off, int press, uint16_t btn, uint16_t fg, uint16_t sub)
{
    for (int i = 0; i < 2; i++) {
        int cx = i ? W - 42 : 42, cy = y + 30;
        fill_rrect(d, cx - 21, cy - 21, 42, 42, 21, press == i ? blend565(btn, 0xFFFF, 8) : btn, 255);
        fill_rrect(d, cx - 8, cy - 1, 16, 3, 1.5f, fg, 255);
        if (i) fill_rrect(d, cx - 1, cy - 8, 3, 16, 1.5f, fg, 255);
    }
    char v[24]; lrc_fmt_offset(off, v, sizeof v);
    int tw = tw_n(v, 18, -1); mu_text_bold(d, (W - tw) / 2, y + 17, v, 18, off ? fg : sub, 0);
    /* regla: +-3 s a lo ancho; marca en 0 y mando en el valor */
    int x0 = LX_TRK0, x1 = LX_TRK1, cxm = (x0 + x1) / 2, ty = y + 32;
    fill_rrect(d, x0, ty - 1, x1 - x0, 3, 1.5f, blend565(btn, 0xFFFF, 6), 255);
    for (int s = -3; s <= 3; s++) { int tx = cxm + s * (x1 - x0) / 6; fill_rrect(d, tx, ty - (s ? 3 : 5), 1, s ? 7 : 11, 0, sub, 255); }
    float k = off / 3000.f; if (k > 1) k = 1; if (k < -1) k = -1;
    int kx = cxm + (int)(k * (x1 - x0) / 2);
    if (off) fill_rrect(d, off > 0 ? cxm : kx, ty - 1, abs(kx - cxm), 3, 1.5f, LX_BLUE, 255);
    fill_rrect(d, kx - (press == 2 ? 9 : 7), ty - (press == 2 ? 9 : 7), press == 2 ? 18 : 14, press == 2 ? 18 : 14, press == 2 ? 9 : 7, 0xFFFF, 255);
    const char *h = off > 0 ? tr("La letra sale antes") : off < 0 ? tr("La letra sale después") : tr("Arrastra para afinar");
    tw = tw_n(h, 12, -1); draw_text(d, (W - tw) / 2, y + 58, h, 12, sub, W - 40, 0, H);
}
static int lx_adjust_hit(int x, int y, int y0) { if (y < y0 - 6 || y >= y0 + LX_ADJ_H + 4) return -1; return x < LX_TRK0 - 12 ? 0 : x >= LX_TRK1 + 12 ? 1 : 2; }
/* arrastre en la regla: offset desde el de partida (pasos de 50 ms) */
static int lx_adjust_drag(int off0, int dx)
{
    int v = off0 + dx * 6000 / (LX_TRK1 - LX_TRK0);
    v = (v >= 0 ? v + 25 : v - 25) / 50 * 50;
    return v > LX_OFFMAX ? LX_OFFMAX : v < -LX_OFFMAX ? -LX_OFFMAX : v;
}
static int lx_adjust_step(int off, int dir) { off += dir * 500; off = (off >= 0 ? off + 250 : off - 250) / 500 * 500; return off > LX_OFFMAX ? LX_OFFMAX : off < -LX_OFFMAX ? -LX_OFFMAX : off; }

/* ---- letra sincronizada en una franja (vista previa de la pantalla suelta) */
struct lx_view { struct lrc L; int *y, *h, tot, cur; float sc, sc0, sc1; double st; uint16_t *scr; };
static void lx_view_free(struct lx_view *v) { lrc_free(&v->L); free(v->y); free(v->h); free(v->scr); memset(v, 0, sizeof *v); }
static int lx_view_load(struct lx_view *v, const char *p)
{
    memset(v, 0, sizeof *v);
    if (lx_load(p, &v->L) || !v->L.n) { lx_view_free(v); return -1; }
    v->y = malloc(v->L.n * sizeof(int)); v->h = malloc(v->L.n * sizeof(int)); v->scr = malloc(HM_FRAME_BYTES);
    if (!v->y || !v->h || !v->scr) { lx_view_free(v); return -1; }
    int y = 0;
    for (int i = 0; i < v->L.n; i++) { v->y[i] = y; v->h[i] = draw_wrapped(0, 0, 0, W - 48, v->L.buf + v->L.ln[i].off, 20, 0, 26, 0) + 14; y += v->h[i]; }
    v->tot = y; v->cur = -2;
    return 0;
}
/* verso actual y desplazamiento objetivo (como Reproduciendo); 1 = cambio */
static int lx_view_tick(struct lx_view *v, int pos, int off)
{
    if (!v->L.synced) return 0;
    int cur = lrc_at(&v->L, pos, off);
    if (cur == v->cur) return 0;
    float t = cur < 0 ? 0 : v->y[cur] - 34;
    v->cur = cur; v->sc0 = v->sc; v->sc1 = t; v->st = now_ms();
    return 1;
}
static int lx_view_step(struct lx_view *v)
{
    if (!v->st) return 0;
    double t = (now_ms() - v->st) / 420; if (t >= 1) { v->sc = v->sc1; v->st = 0; return 1; }
    v->sc = v->sc0 + (v->sc1 - v->sc0) * (float)ease_out(t);
    return 1;
}
static void lx_view_draw(uint16_t *d, struct lx_view *v, int y0, int y1, uint16_t bg)
{
    uint16_t *s = v->scr;
    for (int i = y0 * W; i < y1 * W; i++) s[i] = bg;
    int base = y0 + 18 - (int)v->sc;
    for (int i = 0; i < v->L.n; i++) {
        int y = base + v->y[i];
        if (y + v->h[i] < y0 - 30) continue;
        if (y > y1) break;
        if (y - 6 < 0 || y + v->h[i] > H) continue;
        int on = v->L.synced && v->cur >= 0 && v->L.ln[i].t == v->L.ln[v->cur].t;
        const char *t = v->L.buf + v->L.ln[i].off;
        uint16_t c = !v->L.synced ? rgb(214, 214, 220) : on ? 0xFFFF : rgb(98, 98, 104);
        draw_wrapped(s, 24, y - 6, W - 48, t, 20, c, 26, 1);
        if (on) draw_wrapped(s, 25, y - 6, W - 48, t, 20, 0xFFFF, 26, 1);
    }
    for (int y = y0; y < y1; y++) {
        int k = y - y0 < 22 ? (y - y0) * 32 / 22 : y1 - 1 - y < 26 ? (y1 - 1 - y) * 32 / 26 : 32;
        uint16_t *o = d + y * W; const uint16_t *a = s + y * W;
        if (k >= 32) memcpy(o, a, W * 2); else for (int x = 0; x < W; x++) o[x] = blend565(bg, a[x], k);
    }
}

/* ---- posicion de la cancion que suena (extrapolada entre lecturas, como Reproduciendo) */
struct lx_clock { int raw, anc, playing, dur; double anc_t; };
static int lx_clock_pos(const struct lx_clock *c)
{
    int p = c->anc;
    if (c->playing) { double e = now_ms() - c->anc_t; p += (int)(e > 4000 ? 4000 : e); }
    if (c->dur > 0 && p > c->dur) p = c->dur;
    return p < 0 ? 0 : p;
}
/* lee lo publicado por el hilo de nowplay; 0 si ya no suena esa cancion */
static int lx_clock_np(struct lx_clock *c, const char *path)
{
    struct np_info n; npx_snapshot(&n, 0, 0);
    if (n.source == NP_SRC_NONE || strcmp(n.path, path)) return 0;
    double now = now_ms();
    if (n.position_ms >= 0 && n.position_ms != c->raw) { c->raw = n.position_ms; c->anc = n.position_ms; c->anc_t = now; }
    if (c->playing && !n.playing) { c->anc = lx_clock_pos(c); c->anc_t = now; }
    c->playing = n.playing; c->dur = n.duration_ms;
    return 1;
}

/* ---- pantalla suelta (Archivos, Musica, editor de etiquetas) */
static void lx_spinner_msg(uint16_t *d, int y, const char *msg)
{
    spn_draw(d, W / 2, y, spn_phase(now_ms()), 8, 0, H);
    int tw = tw_n(msg, 14, -1); draw_text(d, (W - tw) / 2, y + 36, msg, 14, rgb(142, 142, 147), W - 40, 0, H);
}
/* mensaje y boton (vacio / sin red); devuelve la y del boton */
static int lx_empty(uint16_t *d, int y, const char *msg, const char *btn, uint16_t col, uint16_t bbg)
{
    wrap_cw = W - 64; int h = draw_wrapped(d, 32, y, W - 64, msg, 15, col, 20, 1); wrap_cw = 0;   /* centrado */
    int bw = tw_n(btn, 15, -1) + 40, by = y + h + 14;
    fill_rrect(d, (W - bw) / 2, by, bw, 36, 18, bbg, 255);
    draw_text(d, (W - bw) / 2 + 20, by + 24, btn, 15, LX_BLUE, 0, 0, H);
    return by;
}
#define LX_LIST_Y0 128
static void lyrics_render(uint16_t *d, const char *q, float sc, int sel)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    if (lc_rc == -1) lx_spinner_msg(d, 220, tr("Buscando letras"));
    else if (!nlc) {
        const char *m = lc_rc == 2 ? tr("No se pudo conectar. Comprueba la Wi-Fi.") : lc_rc == 99 ? tr("Falta el componente de red (hibynet)") : tr("No se encontraron letras. Prueba a cambiar la búsqueda.");
        lx_empty(d, 170, m, lc_rc == 2 ? tr("Reintentar") : tr("Editar búsqueda"), rgb(152, 152, 159), rgb(28, 28, 30));
    } else {
        int y = LX_LIST_Y0 - (int)sc, h = nlc * LX_ROW;
        lx_card(d, 12, y, W - 24, h, 12, rgb(28, 28, 30));
        for (int i = 0; i < nlc; i++, y += LX_ROW) {
            if (y + LX_ROW < LX_LIST_Y0 - 4 || y > H) continue;
            uint16_t bg = rgb(28, 28, 30);
            if (i == sel) { lx_card(d, 12, y, W - 24, LX_ROW, 12, rgb(58, 58, 60)); bg = rgb(58, 58, 60); }
            if (i && y >= 0 && y < H) for (int x = 28; x < W - 12; x++) d[y * W + x] = rgb(56, 56, 58);
            lx_row(d, 12, y, W - 24, &lc[i], lc_want, bg, 0xFFFF, rgb(142, 142, 147), rgb(110, 110, 116), lc_get_i == i);
        }
    }
    for (int y = 94; y < LX_LIST_Y0 - 4; y++) for (int x = 0; x < W; x++) d[y * W + x] = 0;
    lx_nav(d, tr("Cancelar"), tr("Letra"), tr("Buscar"));
    draw_text(d, 16, 116, q, 12, rgb(142, 142, 147), W - 32, 0, H);
    draw_toast(d);
}
/* vista previa sincronizada del resultado i; 1 = guardada */
static int lyrics_preview(const char *audio, int i)
{
    char src[160]; lc_path(i, src, sizeof src);
    struct lx_view v; if (lx_view_load(&v, src)) { toast(tr("No se pudo descargar")); return 0; }
    int off = v.L.offset, saved = 0;
    struct lx_clock ck = { -1, 0, 0, 0, now_ms() };
    int follow = lx_clock_np(&ck, audio);
    if (!follow) { ck.playing = 1; ck.anc = 0; ck.anc_t = now_ms(); ck.dur = (lc_want > 0 ? lc_want : lc[i].dur) * 1000; }
    const int ly0 = 100, card = H - (v.L.synced ? 186 : 120), ly1 = card - 4;
    float psc = 0;                                     /* sin sincronizar: desplazamiento a mano */
    int press = -1; struct pkt p;
    hz_hold++;
    for (;;) {
        if (follow && !lx_clock_np(&ck, audio)) { follow = 0; ck.anc = lx_clock_pos(&ck); ck.anc_t = now_ms(); }
        int pos = lx_clock_pos(&ck);
        if (!follow && ck.dur > 0 && pos >= ck.dur) { ck.playing = 0; ck.anc = ck.dur; }
        lx_view_tick(&v, pos, off); lx_view_step(&v);
        if (!v.L.synced) v.sc = psc;
        /* fotograma */
        for (int k = 94 * W; k < W * H; k++) fr0[k] = 0;
        lx_view_draw(fr0, &v, ly0, ly1, 0);
        lx_card(fr0, 0, card, W, H - card + 20, 16, rgb(28, 28, 30));
        int y = card + 12;
        if (v.L.synced) {
            char a[16], b[16]; snprintf(a, sizeof a, "%d:%02d", pos / 60000, pos / 1000 % 60); snprintf(b, sizeof b, "%d:%02d", ck.dur / 60000, ck.dur / 1000 % 60);
            if (follow) {
                fill_rrect(fr0, 20, y + 6, 8, 8, 4, LX_GREEN, 255);
                draw_text(fr0, 34, y + 15, tr("Sonando ahora"), 13, rgb(152, 152, 159), 150, 0, H);
            } else {
                fill_rrect(fr0, 16, y - 2, 26, 26, 13, press == 3 ? rgb(84, 84, 88) : rgb(58, 58, 62), 255);
                if (ck.playing) { fill_rrect(fr0, 24, y + 5, 3, 11, 1, 0xFFFF, 255); fill_rrect(fr0, 31, y + 5, 3, 11, 1, 0xFFFF, 255); }
                else for (int k = 0; k < 6; k++) fill_rrect(fr0, 25 + k, y + 5 + k / 2, 1, 11 - (k / 2) * 2, 0, 0xFFFF, 255);
                int bx = 50, bw = W - 50 - 96, fw = ck.dur > 0 ? (int)((long long)bw * pos / ck.dur) : 0;
                fill_rrect(fr0, bx, y + 10, bw, 3, 1.5f, rgb(72, 72, 76), 255);
                if (fw > 2) fill_rrect(fr0, bx, y + 10, fw, 3, 1.5f, rgb(200, 200, 205), 255);
            }
            char tt[40]; snprintf(tt, sizeof tt, "%s / %s", a, b);
            draw_text(fr0, W - 16 - tw_n(tt, 13, -1), y + 15, tt, 13, rgb(152, 152, 159), 0, 0, H);
            lx_adjust(fr0, card + 40, off, press, rgb(58, 58, 62), 0xFFFF, rgb(142, 142, 147));
        } else {
            int tw = tw_n(tr("Letra sin sincronizar"), 13, -1);
            draw_text(fr0, (W - tw) / 2, y + 15, tr("Letra sin sincronizar"), 13, rgb(152, 152, 159), W - 32, 0, H);
        }
        lx_button(fr0, 16, H - 58, W - 32, 46, tr("Usar esta letra"), LX_BLUE, 0xFFFF, press == 4);
        lx_nav(fr0, tr("Resultados"), tr("Vista previa"), 0);
        draw_toast(fr0);
        present();
        /* entrada: a ritmo del panel mientras algo se mueve; si no, hasta el siguiente verso */
        int anim = v.st || (!follow && ck.playing) || press >= 0 || now_ms() < toast_until + 40;
        int wait = 100; if (v.L.synced && ck.playing) { int nx = lrc_next_in(&v.L, pos, off); if (nx >= 0 && nx < wait) wait = nx; }
        if (!read_pkt(&p, anim ? 0 : wait) || !p.down) continue;
        int sx = p.x, sy = p.y, h = -1, moved = 0, off0 = off; float psc0 = psc;
        if (sy >= 456 && sx > 60 && sx < W - 60) { while (cur_down && read_pkt(&p, 1000)) {} if (sy - cur_y > 20) break; continue; }
        if (sy < 94 && sx < 120) { drain_until_up(); break; }
        if (sy >= H - 62) h = 4;
        else if (v.L.synced && !follow && sy >= card && sy < card + 36 && sx < 50) h = 3;
        else if (v.L.synced) { int a = lx_adjust_hit(sx, sy, card + 40); if (a >= 0) h = a; }
        press = h;
        if (h >= 0) {                                   /* que se vea pulsado */
            if (h == 4) lx_button(fr0, 16, H - 58, W - 32, 46, tr("Usar esta letra"), LX_BLUE, 0xFFFF, 1);
            else if (h <= 2) { for (int k = (card + 40) * W; k < (card + 40 + LX_ADJ_H) * W; k++) fr0[k] = rgb(28, 28, 30); lx_adjust(fr0, card + 40, off, h, rgb(58, 58, 62), 0xFFFF, rgb(142, 142, 147)); }
            present();
        }
        while (cur_down) {
            if (!read_pkt(&p, 16)) continue;
            if (h == 2 && abs(cur_x - sx) > 3) moved = 1;
            else if (h < 0 && !v.L.synced && abs(cur_y - sy) > 6) moved = 1;
            if (moved) break;
        }
        if (moved) {                                    /* seguir al dedo a ritmo del panel */
            while (cur_down) {
                int ps = lx_clock_pos(&ck);
                if (h == 2) { off = lx_adjust_drag(off0, cur_x - sx); v.cur = -2; lx_view_tick(&v, ps, off); v.sc = v.sc1; v.st = 0; }
                else { psc = psc0 - (cur_y - sy); float mx = v.tot - (ly1 - ly0) + 40; if (psc > mx) psc = mx; if (psc < 0) psc = 0; v.sc = psc; }
                for (int k = 94 * W; k < card * W; k++) fr0[k] = 0;
                lx_view_draw(fr0, &v, ly0, ly1, 0);
                if (v.L.synced) { for (int k = (card + 40) * W; k < (card + 40 + LX_ADJ_H) * W; k++) fr0[k] = rgb(28, 28, 30); lx_adjust(fr0, card + 40, off, 2, rgb(58, 58, 62), 0xFFFF, rgb(142, 142, 147)); }
                lx_nav(fr0, tr("Resultados"), tr("Vista previa"), 0);
                present();
                while (read_pkt(&p, 0)) {}
            }
            press = -1; continue;
        }
        int up = h;
        if (h == 4 && cur_y < H - 66) up = -1;
        press = -1;
        if (up == 0 || up == 1) off = lx_adjust_step(off, up ? 1 : -1);
        else if (up == 3) { if (ck.playing) { ck.anc = lx_clock_pos(&ck); ck.playing = 0; } else { if (ck.dur > 0 && ck.anc >= ck.dur) ck.anc = 0; ck.playing = 1; } ck.anc_t = now_ms(); }
        else if (up == 4) {
            if (!lx_save(src, audio, off)) { saved = 1; toast(tr("Letra guardada")); break; }
            toast(tr("No se pudo guardar"));
        }
    }
    hz_hold--;
    lx_view_free(&v);
    return saved;
}

static void lyrics_album(char **v, int n)
{
    int ok = 0;
    progress_card(0, 0, -1);
    for (int i = 0; i < n; i++) {
        char sub[64]; snprintf(sub, sizeof sub, tr("%d de %d"), i + 1, n);
        progress_card(tr("Buscando letras"), sub, (double)i / n);
        ht_tags t; if (ht_read(v[i], &t)) { ht_tags_free(&t); continue; }
        const char *ar = t.v[HT_ARTIST] ? t.v[HT_ARTIST] : "", *ti = t.v[HT_TITLE] ? t.v[HT_TITLE] : "", *al = t.v[HT_ALBUM] ? t.v[HT_ALBUM] : "";
        int has = ti[0] != 0;
        if (has) {                                       /* en segundo plano: la tarjeta sigue animada */
            lc_start(ar, ti, al, lx_dur_s(v[i]));
            while (lc_rc == -1) { if (!lc_poll()) { progress_card(tr("Buscando letras"), sub, (i + 0.5) / n); msleep(30); } }
        }
        ht_tags_free(&t);
        if (!has || !nlc) continue;
        /* la primera: lrc_rank ya pone delante la sincronizada de la misma duracion */
        char src[160]; lc_path(0, src, sizeof src);
        int r; while (!(r = lc_ready(0))) { progress_card(tr("Buscando letras"), sub, (i + 0.8) / n); msleep(30); }
        if (r > 0 && !lx_save(src, v[i], 0)) ok++;
    }
    sync();
    char m[96]; snprintf(m, sizeof m, tr("Letras guardadas: %d de %d"), ok, n);
    toast(m);
}

static void lyrics_search(const char *path, const char *artist, const char *title, const char *album)
{
    if (!net_ready()) return;
    {   /* una cancion o todo el album */
        int nalb = 0; char **alb = album_files(path, &nalb);
        if (nalb > 1) { int sc = ask_scope(nalb); if (sc < 0) { free_list(alb, nalb); return; } if (sc == 1) { lyrics_album(alb, nalb); free_list(alb, nalb); return; } }
        free_list(alb, nalb);
    }
    char a[128], t[160], b[128], q[320];
    snprintf(a, sizeof a, "%s", artist ? artist : ""); snprintf(t, sizeof t, "%s", title ? title : ""); snprintf(b, sizeof b, "%s", album ? album : "");
    int want = lx_dur_s(path);
    lc_start(a, t, b, want);
    float sc = 0; struct pkt p; int done = 0, open_i = -1;
    while (!done) {
        lc_poll();
        char ds[32] = ""; if (want > 0) snprintf(ds, sizeof ds, " \xC2\xB7 %d:%02d", want / 60, want % 60);
        snprintf(q, sizeof q, "%s \xE2\x80\x94 %s%s", a, t, ds);
        if (open_i >= 0) {                               /* esperando el .lrc de una fila */
            int r = lc_ready(open_i);
            if (r < 0) { toast(tr("No se pudo descargar")); open_i = -1; }
            else if (r > 0) { int i = open_i; open_i = -1; lyrics_render(fr0, q, sc, i); present(); if (lyrics_preview(path, i)) done = 1; continue; }
        }
        lyrics_render(fr0, q, sc, open_i); present();
        int busy = lc_rc == -1 || open_i >= 0 || now_ms() < toast_until + 40;
        if (!read_pkt(&p, busy ? 0 : 400) || !p.down) continue;
        int x = p.x, y = p.y, moved = 0; float sc0 = sc;
        while (cur_down) {
            if (!read_pkt(&p, 16)) continue;
            if (y >= LX_LIST_Y0 && nlc && abs(cur_y - y) > 8) moved = 1;
            if (moved) {
                float mx = nlc * LX_ROW - (H - LX_LIST_Y0) + 16; if (mx < 0) mx = 0;
                sc = sc0 - (cur_y - y); if (sc < 0) sc = 0; if (sc > mx) sc = mx;
                lyrics_render(fr0, q, sc, -1); present();
                while (read_pkt(&p, 0)) {}
            }
        }
        if (moved) continue;
        if (y >= 456 && cur_y < y - 20) break;
        if (y < 94 && x < 100) break;
        if ((y < 94 && x > W - 90) || (!nlc && lc_rc != -1 && lc_rc != 2 && lc_rc != 99 && y > 180 && y < 320)) {
            if (line_edit(tr("Título"), tr("Título"), t, sizeof t) && line_edit(tr("Artista"), tr("Artista"), a, sizeof a)) { lc_start(a, t, "", want); sc = 0; }
            continue;
        }
        if (!nlc && lc_rc == 2 && y > 180 && y < 320) { lc_start(a, t, b, want); continue; }
        if (lc_rc == -1 || !nlc || y < LX_LIST_Y0) continue;
        int i = (int)((y - LX_LIST_Y0 + sc) / LX_ROW); if (i < 0 || i >= nlc) continue;
        open_i = i;
    }
    lc_cancel();
}
