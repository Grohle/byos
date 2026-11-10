/* books.inc.c - "Libros": lector estilo Apple Books que sustituye al de HiBy (que solo leia TXT
 * en SD/book, ver docs). Incluido desde hibyshell.c; el nucleo sin interfaz esta en
 * books_core.inc.c (probado en PC: tests/books_test.sh).
 *  - Biblioteca: cuadricula de portadas (la del EPUB o una generada con el titulo), ordenada por
 *    lectura reciente, con progreso; pulsacion larga / "..." : terminado, no leido, eliminar.
 *    Busca en las carpetas book/Books/eBooks/Libros... de la SD y de "En el HiBy" (y .epub sueltos
 *    en la raiz de la SD). Metadatos y portadas en segundo plano (hibywall), en cache.
 *  - Lector: paginas con el deslizamiento de Apple Books (sigue al dedo, inercia), toque en los
 *    bordes para pasar, toque en el centro para los controles (cerrar, titulo, pagina, barra de
 *    progreso, menu: Contenido / Temas y ajustes: tamano A-/A+, Original/Sepia/Noche).
 *    Deslizar hacia abajo cierra el libro (vuelve a su portada). Posicion por libro en
 *    /usr/data/hibymod/appdata/books/<hash>.pos.
 *  - Rendimiento: la pagina actual, la anterior y la siguiente estan pintadas de antemano; un
 *    pase de pagina solo copia filas (memcpy). La paginacion del libro entero (numero de paginas,
 *    paginas del indice) va en un hilo de baja prioridad que se para durante las animaciones.
 *  - Memoria: un capitulo cada vez (inflate en streaming), 3 capitulos en cache, 3 paginas
 *    pintadas (~0,9 MB, solo con un libro abierto), portadas en LRU de 10. */
#include <sys/resource.h>
#include <sys/syscall.h>
#include "books_core.inc.c"
#include "books_draw.inc.c"

#define BK_DIR "/usr/data/hibymod/appdata/books"
#define BK_CW 132
#define BK_CHT 198
#define BK_ML 24                     /* margen lateral del texto */
#define BK_TOP 38                    /* comienzo del texto */
#define BK_BOT 42                    /* hueco inferior (numero de pagina) */
#define BK_LIST_Y 92
#define BK_PITCH (BK_CHT + 50)
#define BK_MAXB 600

static const int bk_sizes[] = { 14, 15, 16, 17, 19, 21, 24, 27 };
#define BK_NSIZES ((int)(sizeof bk_sizes / sizeof bk_sizes[0]))
static int bk_size_i = 3, bk_theme = 0;
/* Dos apps comparten este modulo: Libros (Biblioteca + Audiolibros) y Gutenberg (Explorar + Descargas) */
static int bk_tab;                           /* Libros: 0 biblioteca, 1 audiolibros, 2 explorar | Gutenberg: 2 explorar, 3 descargas */
static int bk_gut;                           /* 0 = Libros, 1 = Gutenberg */
static int bk_tab_last[2] = { 0, 2 };        /* pestana en la que se dejo cada app */
#define BK_APPID (bk_gut ? "gutenberg" : "books")
/* el lector se abre tambien desde Gutenberg: quien lo aloja pinta el fondo y dice donde esta la portada */
static void (*bk_host_render)(uint16_t *d);
static int bk_host_x, bk_host_y, bk_host_w, bk_host_h, bk_host_c0;
static char ab_langc[16] = "";               /* idioma de la tienda (clave de ab_langs; "" = el de la interfaz) */
struct bk_th { uint16_t bg, fg, dim, chrome, sheet, card, accent; };
static struct bk_th bk_T(void)
{
    if (bk_theme == 1) return (struct bk_th){ rgb(248, 241, 227), rgb(79, 50, 28), rgb(160, 136, 110), rgb(236, 225, 200), rgb(240, 231, 212), rgb(250, 245, 235), rgb(10, 132, 255) };
    if (bk_theme == 2) return (struct bk_th){ rgb(18, 18, 18), rgb(212, 212, 214), rgb(118, 118, 124), rgb(48, 48, 52), rgb(28, 28, 30), rgb(48, 48, 52), rgb(10, 132, 255) };
    return (struct bk_th){ rgb(255, 255, 255), rgb(22, 22, 24), rgb(142, 142, 147), rgb(232, 232, 237), rgb(242, 242, 247), rgb(255, 255, 255), rgb(10, 132, 255) };
}

/* ------------------------------------------------------------------ utilidades */
static uint64_t bk_hash(const char *path, long long size)
{
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    uint64_t h = 1469598103934665603ULL;
    for (; *b; b++) h = (h ^ (unsigned char)*b) * 1099511628211ULL;
    for (int i = 0; i < 8; i++) h = (h ^ (unsigned char)(size >> (i * 8))) * 1099511628211ULL;
    return h;
}
static void bk_mkdirs(void) { mkdir("/usr/data/hibymod/appdata", 0755); mkdir(BK_DIR, 0755); }
static int bk_write_atomic(const char *path, const char *data, int n)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f < 0) return -1;
    int ok = write(f, data, n) == n; ok &= !close(f);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
static void bk_fill(uint16_t *d, int x0, int y0, int w, int h, uint16_t c)
{
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < H; y++) for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < W; x++) d[y * W + x] = c;
}
/* "x" de cerrar en un circulo */
static void bk_xbtn(uint16_t *d, int cx, int cy, uint16_t bg, uint16_t fg)
{
    fill_circle(d, cx, cy, 15, bg, 255);
    for (int i = -5; i <= 5; i++) { fill_rrect(d, cx + i - 1, cy + i - 1, 2, 2, 1, fg, 255); fill_rrect(d, cx + i - 1, cy - i - 1, 2, 2, 1, fg, 255); }
}

/* ------------------------------------------------------------------ ajustes y posiciones */
static void bk_conf_load(void)
{
    FILE *f = fopen(BK_DIR "/settings", "r"); if (!f) return;
    int s, t; char lc[16] = "";
    if (fscanf(f, "%d %d %15s", &s, &t, lc) >= 2) { if (s >= 0 && s < BK_NSIZES) bk_size_i = s; if (t >= 0 && t < 3) bk_theme = t; }
    if (lc[0] && strcmp(lc, "-")) snprintf(ab_langc, sizeof ab_langc, "%s", lc);
    fclose(f);
}
static void bk_conf_save(void) { bk_mkdirs(); char b[48]; int n = snprintf(b, sizeof b, "%d %d %s\n", bk_size_i, bk_theme, ab_langc[0] ? ab_langc : "-"); bk_write_atomic(BK_DIR "/settings", b, n); }
struct bk_pos { int ch, off; double opened; float pct; int done; };
static int bk_pos_load(uint64_t h, struct bk_pos *p)
{
    memset(p, 0, sizeof *p);
    char path[200]; snprintf(path, sizeof path, BK_DIR "/%016llx.pos", (unsigned long long)h);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int ok = fscanf(f, "%d %d %lf %f %d", &p->ch, &p->off, &p->opened, &p->pct, &p->done) == 5;
    fclose(f); if (!ok) memset(p, 0, sizeof *p);
    return ok;
}
static void bk_pos_save(uint64_t h, const struct bk_pos *p)
{
    bk_mkdirs();
    char path[200], b[96]; snprintf(path, sizeof path, BK_DIR "/%016llx.pos", (unsigned long long)h);
    int n = snprintf(b, sizeof b, "%d %d %.0f %.4f %d\n", p->ch, p->off, p->opened, p->pct, p->done);
    bk_write_atomic(path, b, n);
}

/* ------------------------------------------------------------------ biblioteca */
struct bk_li { char path[512]; char title[160], author[120]; int fmt; long long size; time_t mtime; uint64_t h; double opened; float pct; int done, meta, cov; };
static struct bk_li *bl; static int bln;
static pthread_mutex_t bk_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int bk_lib_dirty, bk_meta_stop, bk_meta_run;
static pthread_t bk_meta_th;
static float bk_lscroll;
static void bk_add_file(const char *path, const struct stat *s)
{
    if (bln >= BK_MAXB) return;
    const char *x = strrchr(path, '.'); if (!x) return;
    int fmt = !strcasecmp(x, ".epub") ? BK_EPUB : !strcasecmp(x, ".txt") ? BK_TXT : -1;
    if (fmt < 0 || s->st_size < 16) return;
    struct bk_li *b = &bl[bln];
    memset(b, 0, sizeof *b);
    snprintf(b->path, sizeof b->path, "%s", path); b->fmt = fmt; b->size = s->st_size; b->mtime = s->st_mtime;
    b->h = bk_hash(path, s->st_size);
    bk_title_from_path(path, b->title, sizeof b->title);
    b->meta = fmt == BK_TXT; b->cov = fmt == BK_TXT ? 2 : 0;
    struct bk_pos p; if (bk_pos_load(b->h, &p)) { b->opened = p.opened; b->pct = p.pct; b->done = p.done; }
    bln++;
}
static void bk_scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && bln < BK_MAXB) {
        if (e->d_name[0] == '.') continue;
        char p[800]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat s; if (stat(p, &s)) continue;
        if (S_ISDIR(s.st_mode)) { if (depth < 4) bk_scan_dir(p, depth + 1); }
        else bk_add_file(p, &s);
    }
    closedir(d);
}
/* carpetas de libros en la raiz: book (la de HiBy), Books, eBooks, Libros, Livres... */
static void bk_scan_root(const char *root, int loose_epub)
{
    static const char *const names[] = { "book", "books", "ebook", "ebooks", "e-books", "libros", "livres", "bücher", "buecher", "libri", "książki", "книги", "书籍", "电子书", "본", "หนังสือ", 0 };
    DIR *d = opendir(root); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[800]; snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        struct stat s; if (stat(p, &s)) continue;
        if (S_ISDIR(s.st_mode)) { for (int i = 0; names[i]; i++) if (!strcasecmp(e->d_name, names[i])) { bk_scan_dir(p, 1); break; } }
        else if (loose_epub) { const char *x = strrchr(e->d_name, '.'); if (x && !strcasecmp(x, ".epub")) bk_add_file(p, &s); }
    }
    closedir(d);
}
/* indice en cache: hash  tamano  mtime  portada  titulo  autor */
static void bk_index_apply(void)
{
    FILE *f = fopen(BK_DIR "/index", "r"); if (!f) return;
    char l[600];
    while (fgets(l, sizeof l, f)) {
        unsigned long long h; long long sz; long mt; int cov; int n = 0;
        if (sscanf(l, "%llx\t%lld\t%ld\t%d\t%n", &h, &sz, &mt, &cov, &n) < 4 || !n) continue;
        char *t = l + n, *a = strchr(t, '\t'); if (!a) continue; *a++ = 0;
        char *nl = strchr(a, '\n'); if (nl) *nl = 0;
        for (int i = 0; i < bln; i++) if (bl[i].h == h && bl[i].size == sz && (long)bl[i].mtime == mt) {
            snprintf(bl[i].title, sizeof bl[i].title, "%s", t); snprintf(bl[i].author, sizeof bl[i].author, "%s", a);
            bl[i].cov = cov; bl[i].meta = 1;
            if (cov >= 3 && cov < 6) bl[i].meta = 0;   /* portada que fallo (3..5 = intentos): se reintenta */
        }
    }
    fclose(f);
}
static void bk_index_save(void)
{
    int cap = 256 + bln * 320, n = 0; char *b = malloc(cap); if (!b) return;
    pthread_mutex_lock(&bk_mu);
    for (int i = 0; i < bln; i++) if (bl[i].meta && bl[i].fmt == BK_EPUB)
        n += snprintf(b + n, cap - n, "%016llx\t%lld\t%ld\t%d\t%s\t%s\n", (unsigned long long)bl[i].h, bl[i].size, (long)bl[i].mtime, bl[i].cov, bl[i].title, bl[i].author);
    pthread_mutex_unlock(&bk_mu);
    bk_mkdirs(); bk_write_atomic(BK_DIR "/index", b, n); free(b);
}
static int bk_lcmp(const void *a, const void *b)
{
    const struct bk_li *x = a, *y = b;
    if (x->opened != y->opened) return x->opened < y->opened ? 1 : -1;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcasecmp(x->title, y->title);
}
struct bk_wctx { int fd, err; };
static int bk_wout(void *c, const uint8_t *p, int n) { struct bk_wctx *w = c; if (write(w->fd, p, n) != n) w->err = 1; return w->err; }
/* portada del EPUB -> BK_DIR/<hash>.cov (RGB565 132x198) con hibywall (JPEG reducido / PNG) */
static int bk_make_cover(struct bk_book *b, uint64_t h)
{
    if (b->cover_zi < 0) return 0;
    const struct bk_zent *e = &b->z.e[b->cover_zi];
    if (e->usize > (6u << 20)) return 0;
    char tmp[64]; snprintf(tmp, sizeof tmp, "/tmp/hibymod/bkcov.%d", getpid());
    mkdir("/tmp/hibymod", 0755);
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f < 0) return 0;
    struct bk_wctx wc = { f, 0 };
    int r = bk_zip_stream(&b->z, b->cover_zi, bk_wout, &wc);
    close(f);
    if (r || wc.err) { unlink(tmp); return 0; }
    char out[200], ws[8], hs[8]; snprintf(out, sizeof out, BK_DIR "/%016llx.cov", (unsigned long long)h);
    snprintf(ws, sizeof ws, "%d", BK_CW); snprintf(hs, sizeof hs, "%d", BK_CHT);
    char *argv[] = { ASSETS "/hibywall", tmp, out, ws, hs, 0 };
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid; int st2 = 0, ok = 0;
    if (!posix_spawn(&pid, argv[0], &fa, 0, argv, environ)) { waitpid(pid, &st2, 0); ok = WIFEXITED(st2) && !WEXITSTATUS(st2); }
    posix_spawn_file_actions_destroy(&fa);
    unlink(tmp);
    struct stat sb; if (ok && (stat(out, &sb) || sb.st_size != BK_CW * BK_CHT * 2)) ok = 0;
    if (!ok) unlink(out);
    return ok;
}
static void *bk_meta_thread(void *a)
{
    (void)a;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    int changed = 0;
    while (!bk_meta_stop) {
        /* la interfaz reordena bl (con el cerrojo): se trabaja por hash, nunca por indice */
        char path[512]; uint64_t h = 0; int found = 0;
        pthread_mutex_lock(&bk_mu);
        for (int i = 0; i < bln; i++) if (!bl[i].meta) { snprintf(path, sizeof path, "%s", bl[i].path); h = bl[i].h; found = 1; break; }
        pthread_mutex_unlock(&bk_mu);
        if (!found) break;
        struct bk_book b; char t[160] = "", au[120] = ""; int cov = 2, prev = 0;
        pthread_mutex_lock(&bk_mu);
        for (int i = 0; i < bln; i++) if (bl[i].h == h) prev = bl[i].cov;
        pthread_mutex_unlock(&bk_mu);
        if (!bk_open(&b, path, 1)) {
            snprintf(t, sizeof t, "%s", b.title); snprintf(au, sizeof au, "%s", b.author);
            if (bk_make_cover(&b, h)) cov = 1;
            /* tiene portada pero no se pudo sacar (hibywall, memoria...): no se da por perdida
             * para siempre; se reintenta en los siguientes escaneos (hasta 3 veces) */
            else if (b.cover_zi >= 0) cov = prev >= 3 && prev < 5 ? prev + 1 : prev == 5 ? 2 : 3;
            bk_close(&b);
        }
        pthread_mutex_lock(&bk_mu);
        for (int i = 0; i < bln; i++) if (bl[i].h == h && !bl[i].meta) {
            if (t[0]) snprintf(bl[i].title, sizeof bl[i].title, "%s", t);
            snprintf(bl[i].author, sizeof bl[i].author, "%s", au);
            bl[i].cov = cov; bl[i].meta = 1; bk_lib_dirty = 1;
        }
        pthread_mutex_unlock(&bk_mu);
        changed = 1;
    }
    if (changed) bk_index_save();
    bk_meta_run = 0;
    return 0;
}
static void bk_meta_join(void) { if (bk_meta_run || bk_meta_th) { bk_meta_stop = 1; pthread_join(bk_meta_th, 0); bk_meta_th = 0; bk_meta_run = 0; } }
static void bk_meta_start(void)
{
    bk_meta_join();
    int need = 0; for (int i = 0; i < bln; i++) if (!bl[i].meta) need = 1;
    if (need) { bk_meta_stop = 0; bk_meta_run = 1; if (pthread_create(&bk_meta_th, 0, bk_meta_thread, 0)) { bk_meta_run = 0; bk_meta_th = 0; } }
}
static void bk_lib_scan(void)
{
    bk_meta_join();
    if (!bl) bl = malloc(BK_MAXB * sizeof *bl);
    bln = 0; if (!bl) return;
    bk_scan_root(FS_SD, 1);
    bk_scan_root(FS_INT, 0);
    bk_index_apply();
    qsort(bl, bln, sizeof *bl, bk_lcmp);
    bk_meta_start();
}

/* --------------------------------------------- portadas (LRU) */
static struct { uint64_t h; int cov; uint16_t *px; unsigned use; } bk_cv[10];
static unsigned bk_cv_tick;
static uint16_t bk_cscr[W * BK_CHT];          /* lienzo (paso W) para generar portadas */
static void bk_gen_cover(uint16_t *px, const struct bk_li *b)
{
    static const uint8_t pal[8][3] = { { 32, 92, 100 }, { 120, 36, 48 }, { 30, 52, 96 }, { 36, 84, 56 }, { 84, 44, 96 }, { 160, 106, 38 }, { 64, 72, 86 }, { 146, 66, 40 } };
    const uint8_t *c = pal[b->h % 8];
    for (int y = 0; y < BK_CHT; y++) {
        int k = 40 - y * 22 / BK_CHT;   /* degradado: arriba mas claro */
        uint16_t col = rgb(c[0] * k / 32 > 255 ? 255 : c[0] * k / 32, c[1] * k / 32 > 255 ? 255 : c[1] * k / 32, c[2] * k / 32 > 255 ? 255 : c[2] * k / 32);
        for (int x = 0; x < BK_CW; x++) bk_cscr[y * W + x] = x < 7 ? dim565(col, 22 + x) : x == 7 ? bk_mix(0xFFFF, col, 6) : col;
    }
    /* titulo partido en lineas (el mismo corte que el texto del libro) */
    char t[200]; int tl = snprintf(t, sizeof t, "%s", b->title); if (tl >= (int)sizeof t) tl = sizeof t - 1;
    int off = 0, y = 46, lines = 0;
    while (off < tl && lines < 5) {
        int e, nx, w26, nsp;
        bk_break(t, off, tl, (BK_CW - 26) << 6, 16, bk_adv_ui, 0, &e, &nx, &w26, &nsp);
        if (e <= off) e = nx = off + 1;
        char ln[200]; int n = e - off; memcpy(ln, t + off, n); ln[n] = 0;
        if (lines == 4 && nx < tl) { while (n > 0 && (n > (int)sizeof ln - 4 || bk_text_w(ln, 16) > BK_CW - 40)) { do n--; while (n > 0 && ((unsigned char)ln[n] & 0xC0) == 0x80); ln[n] = 0; } strcat(ln, "…"); }
        int w = bk_text_w(ln, 16);
        bk_text_clip(bk_cscr, 4 + (BK_CW - 4 - w) / 2, y, ln, 16, 0xFFFF, 2, 0, BK_CHT);
        y += 21; lines++; off = nx;
    }
    for (int x = BK_CW / 2 - 14; x < BK_CW / 2 + 18; x++) bk_cscr[(y - 4) * W + x] = bk_mix(0xFFFF, bk_cscr[(y - 4) * W + x], 20);
    if (b->author[0]) {
        int w = tw_n(b->author, 11, -1); if (w > BK_CW - 20) w = BK_CW - 20;
        draw_text(bk_cscr, 4 + (BK_CW - 4 - w) / 2, BK_CHT - 16, b->author, 11, rgb(235, 235, 235), BK_CW - 20, 0, BK_CHT);
    }
    for (int yy = 0; yy < BK_CHT; yy++) memcpy(px + yy * BK_CW, bk_cscr + yy * W, BK_CW * 2);
}
static const uint16_t *bk_cover(const struct bk_li *b)
{
    int slot = -1; unsigned old = ~0u;
    for (int i = 0; i < 10; i++) {
        if (bk_cv[i].px && bk_cv[i].h == b->h && bk_cv[i].cov == b->cov) { bk_cv[i].use = ++bk_cv_tick; return bk_cv[i].px; }
        if (!bk_cv[i].px) { if (old) { old = 0; slot = i; } }
        else if (bk_cv[i].use < old) { old = bk_cv[i].use; slot = i; }
    }
    if (slot < 0) slot = 0;
    if (!bk_cv[slot].px) bk_cv[slot].px = malloc(BK_CW * BK_CHT * 2);
    if (!bk_cv[slot].px) return 0;
    int ok = 0;
    if (b->cov == 1) {
        char p[200]; snprintf(p, sizeof p, BK_DIR "/%016llx.cov", (unsigned long long)b->h);
        int f = open(p, O_RDONLY);
        if (f >= 0) { ok = read(f, bk_cv[slot].px, BK_CW * BK_CHT * 2) == BK_CW * BK_CHT * 2; close(f); }
    }
    if (!ok) bk_gen_cover(bk_cv[slot].px, b);
    bk_cv[slot].h = b->h; bk_cv[slot].cov = b->cov; bk_cv[slot].use = ++bk_cv_tick;
    return bk_cv[slot].px;
}
static void bk_cover_free(void) { for (int i = 0; i < 10; i++) { free(bk_cv[i].px); bk_cv[i].px = 0; } }

/* imagen (sw x sh) escalada al rectangulo, con opacidad a5 (0..32) y esquinas de radio r */
static void bk_blit_scaled(uint16_t *d, const uint16_t *s, int sw, int sh, int rx, int ry, int rw, int rh, int a5, int r, int y0, int y1)
{
    if (rw < 1 || rh < 1 || a5 <= 0) return;
    for (int y = 0; y < rh; y++) {
        int yy = ry + y; if (yy < y0 || yy >= y1 || yy < 0 || yy >= H) continue;
        const uint16_t *sr = s + (y * sh / rh) * sw;
        int cut = 0;
        if (r > 0) { int dy = y < r ? r - y : y >= rh - r ? y - (rh - r - 1) : 0; if (dy > 0) cut = r - (int)sqrtf((float)(r * r - dy * dy)); }
        for (int x = cut; x < rw - cut; x++) {
            int xx = rx + x; if (xx < 0 || xx >= W) continue;
            uint16_t c = sr[x * sw / rw];
            d[yy * W + xx] = a5 >= 32 ? c : bk_mix(c, d[yy * W + xx], a5);
        }
    }
}

#include "books_ab.inc.c"                    /* Audiolibros y la app Gutenberg (Explorar, Descargas) */

/* --------------------------------------------- dibujo de la biblioteca */
static int bk_gbtn_w;                        /* ancho del boton "Abrir Gutenberg" de la biblioteca vacia (0 = no esta) */
static void bk_cell_xy(int i, int *x, int *y) { *x = 20 + (i & 1) * (BK_CW + 16); *y = BK_LIST_Y + 14 + (i >> 1) * BK_PITCH - (int)bk_lscroll; }
static float bk_lmax(void) { float m = ((bln + 1) / 2) * BK_PITCH + 14 - (AB_TABY - BK_LIST_Y); return m < 0 ? 0 : m; }
static void bk_lib_render(uint16_t *d, int pressed)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    memcpy(d, fr1, 30 * W * 2);
    pthread_mutex_lock(&bk_mu);
    for (int i = 0; i < bln; i++) {
        int x, y; bk_cell_xy(i, &x, &y);
        if (y + BK_PITCH < BK_LIST_Y || y > AB_TABY) continue;
        const uint16_t *cv = bk_cover(&bl[i]);
        if (cv) bk_blit_scaled(d, cv, BK_CW, BK_CHT, x, y, BK_CW, BK_CHT, i == pressed ? 22 : 32, 4, BK_LIST_Y, AB_TABY);
        draw_text(d, x, y + BK_CHT + 19, bl[i].title, 13, 0xFFFF, BK_CW - 4, BK_LIST_Y, AB_TABY);
        char st[32]; uint16_t sc = rgb(142, 142, 147);
        if (bl[i].done) snprintf(st, sizeof st, "%s", tr("Terminado"));
        else if (bl[i].opened <= 0) { snprintf(st, sizeof st, "%s", tr("NUEVO")); sc = rgb(10, 132, 255); }
        else snprintf(st, sizeof st, "%d %%", (int)(bl[i].pct * 100 + 0.5f) < 1 ? 1 : (int)(bl[i].pct * 100 + 0.5f));
        draw_text(d, x, y + BK_CHT + 37, st, 12, sc, BK_CW - 30, BK_LIST_Y, AB_TABY);
        for (int k = 0; k < 3; k++) { int cy = y + BK_CHT + 33; if (cy > BK_LIST_Y + 3 && cy < AB_TABY - 3) fill_circle(d, x + BK_CW - 16 + k * 6, cy, 1.6f, rgb(142, 142, 147), 255); }
    }
    int n = bln;
    pthread_mutex_unlock(&bk_mu);
    bk_gbtn_w = 0;
    /* cabecera fija */
    for (int y = 30; y < BK_LIST_Y; y++) for (int x = 0; x < W; x++) d[y * W + x] = 0;
    draw_text(d, 16, 76, tr("Biblioteca"), 28, 0xFFFF, 0, 0, H);
    for (int x = 0; x < W; x++) d[(BK_LIST_Y - 1) * W + x] = rgb(38, 38, 40);
    if (!n) {
        int cy = 196;
        fill_rrect(d, W / 2 - 30, cy - 22, 27, 40, 4, rgb(72, 72, 76), 255); fill_rrect(d, W / 2 + 3, cy - 22, 27, 40, 4, rgb(72, 72, 76), 255);
        fill_rrect(d, W / 2 - 2, cy - 24, 4, 44, 2, rgb(0, 0, 0), 255);
        const char *t = tr("No hay libros"); draw_text(d, (W - tw_n(t, 20, -1)) / 2, cy + 56, t, 20, 0xFFFF, 0, 0, H);
        wrap_cw = W - 56;
        draw_wrapped(d, 28, cy + 78, W - 56, sd_mounted() ? tr("BOOKS_HELP") : tr("La SD está conectada al ordenador"), 14, rgb(142, 142, 147), 19, 1);
        wrap_cw = 0;
        if (!bk_gut && sd_mounted()) {   /* los libros se descargan en la pestana Explorar */
            const char *b = tr("Explorar"); int bw = tw_n(b, 15, -1) + 36;
            fill_rrect(d, (W - bw) / 2, cy + 150, bw, 36, 18, pressed == -2 ? rgb(5, 90, 180) : rgb(10, 132, 255), 255);
            draw_text(d, (W - bw) / 2 + 18, cy + 174, b, 15, 0xFFFF, 0, 0, H);
            bk_gbtn_w = bw;
        }
    }
    ab_tabbar(d, 0, 0);
    draw_toast(d);
}

/* ------------------------------------------------------------------ lector */
static struct bk_book RB; static int rb_ok;
static struct bk_li *rb_li;                  /* entrada de la biblioteca del libro abierto */
static struct bk_lp bk_LP; static volatile int bk_gen;
struct bk_fc { int ch, gen; struct bk_buf f; int *pg, np; unsigned use; };
static struct bk_fc bk_fcs[3];
static unsigned bk_tick;
static int *bk_toc_off;                      /* desplazamiento de cada entrada del indice (UI) */
static int rd_ch, rd_pg, rd_turns;
static double rd_saved_t;
/* paginas pintadas */
struct bk_pi { uint16_t *px; int ch, pg, key; unsigned use; };
static struct bk_pi bk_pis[3];
static volatile int bk_pdone = -1;
static int bk_key(void) { return bk_gen * 8 + bk_theme * 2 + (bk_pdone == bk_gen); }   /* el pie cambia al saber el total */
/* hilo de paginacion */
static pthread_t bk_pth; static int bk_pth_on;
static volatile int bk_pstop, bk_anim;
static int *bk_np;                           /* paginas por capitulo (-1 = sin calcular) */
static int *bk_tpg;                          /* pagina (dentro de su capitulo) de cada entrada del indice */
static struct bk_toc *bk_ttoc; static int bk_nttoc, bk_ttoc_done;   /* indice de un TXT (cabeceras) */
static int *bk_ttpg;

static void bk_lp_set(void) { bk_lp_make(&bk_LP, W - 2 * BK_ML, H - BK_TOP - BK_BOT, bk_sizes[bk_size_i], 1); }
static void bk_ui_mark(void *c, int kind, const char *s, int off)
{
    int ch = *(int *)c;
    if (kind) return;
    for (int i = 0; i < RB.ntoc; i++) if (RB.toc[i].ch == ch && RB.toc[i].frag[0] && !strcmp(RB.toc[i].frag, s) && bk_toc_off[i] < 0) bk_toc_off[i] = off;
}
static struct bk_fc *bk_get(int ch)
{
    if (ch < 0 || ch >= RB.nch) return 0;
    struct bk_fc *f = 0;
    for (int i = 0; i < 3; i++) if (bk_fcs[i].ch == ch && bk_fcs[i].use) { f = &bk_fcs[i]; break; }
    if (!f) {
        f = &bk_fcs[0];
        for (int i = 1; i < 3; i++) if (bk_fcs[i].use < f->use) f = &bk_fcs[i];
        free(f->pg); f->pg = 0; f->np = 0; f->gen = -1; f->ch = ch;
        int c = ch; bk_flow(&RB, ch, &f->f, bk_ui_mark, &c);
        for (int i = 0; i < RB.ntoc; i++) if (RB.toc[i].ch == ch && !RB.toc[i].frag[0]) bk_toc_off[i] = 0;
        if (f->f.n > 4 << 20) f->f.n = 0;
    }
    if (f->gen != bk_gen) {
        free(f->pg); f->pg = 0;
        f->np = f->f.n ? bk_paginate(&bk_LP, f->f.b, f->f.n, bk_adv_ui, 0, &f->pg) : 0;
        f->gen = bk_gen;
    }
    f->use = ++bk_tick;
    return f;
}
static void bk_fc_free(void) { for (int i = 0; i < 3; i++) { bb_free(&bk_fcs[i].f); free(bk_fcs[i].pg); memset(&bk_fcs[i], 0, sizeof bk_fcs[i]); bk_fcs[i].ch = -1; } }
/* pagina vecina (dir +1/-1); 0 si no hay */
static int bk_step(int ch, int pg, int dir, int *och, int *opg)
{
    struct bk_fc *f = bk_get(ch);
    if (f && dir > 0 && pg + 1 < f->np) { *och = ch; *opg = pg + 1; return 1; }
    if (f && dir < 0 && pg > 0 && pg - 1 < f->np) { *och = ch; *opg = pg - 1; return 1; }
    for (int c = ch + dir; c >= 0 && c < RB.nch; c += dir) {
        struct bk_fc *g = bk_get(c);
        if (g && g->np) { *och = c; *opg = dir > 0 ? 0 : g->np - 1; return 1; }
    }
    return 0;
}
/* pagina global (1..) y total, si el hilo ya termino */
static int bk_global(int ch, int pg, int *total)
{
    if (bk_pdone != bk_gen || !bk_np) return 0;
    int g = 0, t = 0;
    for (int i = 0; i < RB.nch; i++) { int n = __atomic_load_n(&bk_np[i], __ATOMIC_ACQUIRE); if (n < 0) return 0; if (i < ch) g += n; t += n; }
    *total = t; return g + pg + 1;
}
static float bk_frac(int ch, int pg)
{
    struct bk_fc *f = bk_get(ch); if (!f || RB.total <= 0) return 0;
    int t; int g = bk_global(ch, pg, &t);
    if (g && t) return t > 1 ? (float)(g - 1) / (t - 1) : 1;
    double in = f->np > 1 ? (double)pg / (f->np - 1) : 0;
    return (float)((RB.ch[ch].r0 + in * (RB.ch[ch].r1 - RB.ch[ch].r0)) / RB.total);
}
static void bk_render_page(uint16_t *d, int ch, int pg)
{
    struct bk_th T = bk_T();
    for (int i = 0; i < W * H; i++) d[i] = T.bg;
    struct bk_fc *f = bk_get(ch);
    if (!f || pg < 0 || pg >= f->np) return;
    bk_draw_text_page(d, &bk_LP, f->f.b, f->f.n, f->pg[pg], BK_ML, BK_TOP, T.fg, T.dim);
    /* pie: pagina */
    char lb[48]; int tot, g = bk_global(ch, pg, &tot);
    if (g) snprintf(lb, sizeof lb, tr("%d de %d"), g, tot);
    else snprintf(lb, sizeof lb, "%d %%", (int)(bk_frac(ch, pg) * 100 + 0.5f));
    int w = tw_n(lb, 11, -1);
    draw_text(d, (W - w) / 2, H - 16, lb, 11, T.dim, 0, 0, H);
}
/* pagina pintada (cache de 3) */
static const uint16_t *bk_page(int ch, int pg)
{
    struct bk_pi *s = 0; int key = bk_key();
    for (int i = 0; i < 3; i++) if (bk_pis[i].px && bk_pis[i].ch == ch && bk_pis[i].pg == pg && bk_pis[i].key == key) { s = &bk_pis[i]; break; }
    if (!s) {
        s = &bk_pis[0];
        for (int i = 1; i < 3; i++) if (bk_pis[i].use < s->use) s = &bk_pis[i];
        if (!s->px) s->px = malloc(HM_FRAME_BYTES);
        if (!s->px) return img_sheet;
        bk_render_page(s->px, ch, pg); s->ch = ch; s->pg = pg; s->key = key;
    }
    s->use = ++bk_tick;
    return s->px;
}
static int bk_have(int ch, int pg) { int key = bk_key(); for (int i = 0; i < 3; i++) if (bk_pis[i].px && bk_pis[i].ch == ch && bk_pis[i].pg == pg && bk_pis[i].key == key) return 1; return 0; }
static void bk_pis_free(void) { for (int i = 0; i < 3; i++) { free(bk_pis[i].px); memset(&bk_pis[i], 0, sizeof bk_pis[i]); } }
static void bk_pis_touch(int ch, int pg) { for (int i = 0; i < 3; i++) if (bk_pis[i].px && bk_pis[i].ch == ch && bk_pis[i].pg == pg) bk_pis[i].use = ++bk_tick; }
/* trabajo pendiente en reposo: pintar las vecinas. 1 si hizo algo */
static int bk_idle_work(void)
{
    int c, p;
    bk_pis_touch(rd_ch, rd_pg);
    if (bk_step(rd_ch, rd_pg, 1, &c, &p) && !bk_have(c, p)) { bk_page(c, p); bk_pis_touch(rd_ch, rd_pg); return 1; }
    if (bk_step(rd_ch, rd_pg, -1, &c, &p) && !bk_have(c, p)) { bk_page(c, p); return 1; }
    return 0;
}

/* --------------------------------------------- hilo de paginacion */
struct bk_tmc { int ch; int *off; int first; };
static void bk_th_mark(void *c, int kind, const char *s, int off)
{
    struct bk_tmc *m = c;
    if (kind == 0) { for (int i = 0; i < RB.ntoc; i++) if (RB.toc[i].ch == m->ch && RB.toc[i].frag[0] && m->off[i] < 0 && !strcmp(RB.toc[i].frag, s)) m->off[i] = off; return; }
    if (!m->first) return;
    pthread_mutex_lock(&bk_mu);
    int ok = bk_nttoc < BK_MAX_TOC;
    if (ok && !(bk_nttoc & 63)) {
        struct bk_toc *n = realloc(bk_ttoc, (bk_nttoc + 64) * sizeof *n); if (n) bk_ttoc = n; else ok = 0;
        int *np = ok ? realloc(bk_ttpg, (bk_nttoc + 64) * sizeof *np) : 0; if (np) bk_ttpg = np; else ok = 0;
    }
    if (ok) {
        struct bk_toc *t = &bk_ttoc[bk_nttoc]; memset(t, 0, sizeof *t);
        snprintf(t->title, sizeof t->title, "%s", s); t->ch = m->ch; t->off = off; bk_ttpg[bk_nttoc] = -1; bk_nttoc++;
    }
    pthread_mutex_unlock(&bk_mu);
}
static struct bk_advc bk_thac;
static int bk_adv_th(void *c, int cp, int size) { (void)c; return bk_adv_cached(&bk_thac, cp, size, bk_adv_calc); }
static void *bk_pag_thread(void *a)
{
    (void)a;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 12);
    memset(&bk_thac, 0, sizeof bk_thac);          /* bk_latinq puede haber cambiado */
    struct bk_buf f = { 0 };
    int *toff = RB.ntoc ? malloc(RB.ntoc * sizeof *toff) : 0;
    while (!bk_pstop) {
        int g = bk_gen; struct bk_lp P = bk_LP;
        if (bk_pdone == g) { msleep(40); continue; }
        int first = !bk_ttoc_done && RB.fmt == BK_TXT;
        for (int i = 0; i < RB.nch; i++) __atomic_store_n(&bk_np[i], -1, __ATOMIC_RELEASE);
        int ok = 1;
        for (int ch = 0; ch < RB.nch && ok; ch++) {
            while (bk_anim && !bk_pstop && g == bk_gen) msleep(4);
            if (bk_pstop || g != bk_gen) { ok = 0; break; }
            struct bk_tmc m = { ch, toff, first };
            if (toff) for (int i = 0; i < RB.ntoc; i++) toff[i] = RB.toc[i].ch == ch && !RB.toc[i].frag[0] ? 0 : -1;
            int nt0 = bk_nttoc;
            bk_flow(&RB, ch, &f, bk_th_mark, &m);
            int *pg = 0, np = f.n ? bk_paginate(&P, f.b, f.n, bk_adv_th, 0, &pg) : 0;
            for (int i = 0; i < RB.ntoc && toff; i++) if (RB.toc[i].ch == ch && toff[i] >= 0) __atomic_store_n(&bk_tpg[i], bk_page_of(pg, np, toff[i]), __ATOMIC_RELEASE);
            pthread_mutex_lock(&bk_mu);
            if (first) for (int i = nt0; i < bk_nttoc; i++) bk_ttpg[i] = np ? bk_page_of(pg, np, bk_ttoc[i].off) : 0;
            else if (RB.fmt == BK_TXT) for (int i = 0; i < bk_nttoc; i++) if (bk_ttoc[i].ch == ch) bk_ttpg[i] = np ? bk_page_of(pg, np, bk_ttoc[i].off) : 0;
            pthread_mutex_unlock(&bk_mu);
            free(pg);
            __atomic_store_n(&bk_np[ch], np, __ATOMIC_RELEASE);
            if (f.cap > (256 << 10)) bb_free(&f);
        }
        if (ok) { if (first) bk_ttoc_done = 1; bk_pdone = g; }
    }
    bb_free(&f); free(toff);
    return 0;
}

/* --------------------------------------------- abrir / cerrar el libro */
static void bk_save(int final)
{
    if (!rb_ok || !rb_li) return;
    struct bk_fc *f = bk_get(rd_ch);
    struct bk_pos p; p.ch = rd_ch; p.off = f && rd_pg < f->np ? f->pg[rd_pg] : 0;
    p.opened = (double)time(0); p.pct = bk_frac(rd_ch, rd_pg);
    int c, q; p.done = !bk_step(rd_ch, rd_pg, 1, &c, &q);
    if (p.done) p.pct = 1;
    bk_pos_save(rb_li->h, &p);
    pthread_mutex_lock(&bk_mu); rb_li->opened = p.opened; rb_li->pct = p.pct; rb_li->done = p.done; pthread_mutex_unlock(&bk_mu);
    rd_saved_t = now_ms(); rd_turns = 0; (void)final;
}
static void bk_close_book(void)
{
    if (!rb_ok) return;
    bk_pstop = 1; if (bk_pth_on) pthread_join(bk_pth, 0); bk_pth_on = 0;
    bk_fc_free(); bk_pis_free();
    free(bk_np); bk_np = 0; free(bk_tpg); bk_tpg = 0; free(bk_toc_off); bk_toc_off = 0;
    free(bk_ttoc); bk_ttoc = 0; free(bk_ttpg); bk_ttpg = 0; bk_nttoc = 0; bk_ttoc_done = 0;
    bk_close(&RB); rb_ok = 0; rb_li = 0;
}
static int bk_open_book(struct bk_li *li)
{
    bk_close_book();
    bk_meta_join();                                /* el hilo de portadas no compite al abrir */
    if (bk_open(&RB, li->path, 0)) { RB.fd = -1; return 0; }
    rb_ok = 1; rb_li = li;
    bk_np = malloc(RB.nch * sizeof *bk_np);
    bk_tpg = malloc((RB.ntoc ? RB.ntoc : 1) * sizeof *bk_tpg);
    bk_toc_off = malloc((RB.ntoc ? RB.ntoc : 1) * sizeof *bk_toc_off);
    if (!bk_np || !bk_tpg || !bk_toc_off) { bk_close_book(); return 0; }
    for (int i = 0; i < RB.nch; i++) bk_np[i] = -1;
    for (int i = 0; i < RB.ntoc; i++) { bk_tpg[i] = -1; bk_toc_off[i] = -1; }
    for (int i = 0; i < 3; i++) bk_fcs[i].ch = -1;
    bk_lp_set(); bk_gen++;
    struct bk_pos p; bk_pos_load(li->h, &p);
    if (p.ch < 0 || p.ch >= RB.nch) { p.ch = 0; p.off = 0; }
    rd_ch = p.ch; rd_pg = 0;
    struct bk_fc *f = bk_get(rd_ch);
    if (f && f->np) rd_pg = bk_page_of(f->pg, f->np, p.off);
    else { int c, q; if (bk_step(rd_ch, 0, 1, &c, &q)) { rd_ch = c; rd_pg = q; } }
    { int c, q; f = bk_get(rd_ch); if ((!f || !f->np) && !bk_step(rd_ch, 0, 1, &c, &q)) { bk_close_book(); return 0; } }
    {   /* libro latino o CJK (comillas proporcionales o de ancho completo, ver books_draw.inc.c) */
        f = bk_get(rd_ch); int cj = 0, n = 0;
        for (int i = 0; f && i < f->f.n && n < 3000; ) { int l, c = bk_u8dec((unsigned char *)f->f.b + i, (unsigned char *)f->f.b + f->f.n, &l); i += l; if (c > 0x40) { n++; if (bk_is_cjk(c)) cj++; } }
        int lq = cj * 5 < n;
        if (lq != bk_latinq) {
            int off = f && rd_pg < f->np ? f->pg[rd_pg] : 0;
            bk_latinq = lq; memset(&bk_uiac, 0, sizeof bk_uiac); bk_gen++;
            f = bk_get(rd_ch); rd_pg = f && f->np ? bk_page_of(f->pg, f->np, off) : 0;
        }
    }
    { int gi; font_glyph(0x10FFFD, &gi); }        /* fuentes de reserva mapeadas antes del hilo */
    bk_pstop = 0; bk_pdone = -1; bk_anim = 0;
    bk_pth_on = !pthread_create(&bk_pth, 0, bk_pag_thread, 0);
    rd_turns = 0; rd_saved_t = now_ms();
    bk_save(0);
    return 1;
}

/* --------------------------------------------- composicion de fotogramas */
static int rd_chrome;                        /* controles visibles */
static void bk_chrome_draw(uint16_t *d, int scrub_ch, int scrub_pg)
{
    struct bk_th T = bk_T();
    memcpy(d, fr1, 30 * W * 2);
    /* bandas del color de la pagina arriba y abajo (como Libros de iOS): los controles no se
     * montan sobre el texto ni sobre el pie "N de M" de la propia pagina */
    for (int y = 30; y < 82; y++) for (int x = 0; x < W; x++) d[y * W + x] = T.bg;
    for (int y = 82; y < 90; y++) { int k = 32 - (y - 81) * 4; for (int x = 0; x < W; x++) d[y * W + x] = bk_mix(T.bg, d[y * W + x], k); }
    for (int y = H - 74; y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = T.bg;
    for (int y = H - 82; y < H - 74; y++) { int k = 32 - (H - 74 - y) * 4; for (int x = 0; x < W; x++) d[y * W + x] = bk_mix(T.bg, d[y * W + x], k); }
    bk_xbtn(d, 34, 56, T.chrome, T.fg);
    int tw = tw_n(RB.title, 13, -1); if (tw > 190) tw = 190;
    draw_text(d, (W - tw) / 2, 61, RB.title, 13, T.dim, 190, 0, H);
    /* barra de progreso y pagina */
    int x0 = 24, x1 = W - 92, ty = H - 40;
    float fr = bk_frac(scrub_ch, scrub_pg); int kx = x0 + (int)((x1 - x0) * fr);
    fill_rrect(d, x0, ty - 1, x1 - x0, 3, 1.5f, T.chrome, 255);
    fill_rrect(d, x0, ty - 1, kx - x0 + 1, 3, 1.5f, T.dim, 255);
    fill_circle(d, kx, ty + 0.5f, 7, T.card, 255); fill_circle(d, kx, ty + 0.5f, 7, T.dim, 60);
    fill_circle(d, kx, ty + 0.5f, 6, T.card, 255);
    char lb[64]; int tot, g = bk_global(scrub_ch, scrub_pg, &tot);
    if (g) snprintf(lb, sizeof lb, tr("%d de %d"), g, tot); else snprintf(lb, sizeof lb, "%d %%", (int)(fr * 100 + 0.5f));
    int w = tw_n(lb, 13, -1); draw_text(d, x0 + (x1 - x0 - w) / 2, ty - 16, lb, 13, T.dim, 0, 0, H);
    /* boton de menu */
    fill_rrect(d, W - 72, H - 58, 52, 36, 18, T.chrome, 255);
    for (int i = 0; i < 3; i++) fill_rrect(d, W - 56, H - 48 + i * 7, 20, 2, 1, T.fg, 255);
}
/* pagina actual (+ controles con opacidad a5) en fr0 */
static void bk_frame(int a5)
{
    const uint16_t *pg = bk_page(rd_ch, rd_pg);
    if (a5 <= 0) { memcpy(fr0, pg, HM_FRAME_BYTES); return; }
    memcpy(img_sheet, pg, HM_FRAME_BYTES); bk_chrome_draw(img_sheet, rd_ch, rd_pg);
    if (a5 >= 32) { memcpy(fr0, img_sheet, HM_FRAME_BYTES); return; }
    for (int i = 0; i < W * H; i++) fr0[i] = pg[i] == img_sheet[i] ? pg[i] : bk_mix(img_sheet[i], pg[i], a5);
}
static void bk_show(void) { bk_frame(rd_chrome ? 32 : 0); draw_toast(fr0); present(); }
static void bk_chrome_fade(int on)
{
    bk_anim = 1;
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 180; if (t > 1) t = 1;
        double e = ease_out(t);
        bk_frame((int)(32 * (on ? e : 1 - e))); present();
        if (t >= 1) break;
    }
    rd_chrome = on; bk_anim = 0;
}
/* desplazamiento: "a" se va (dir>0 a la izquierda) y "b" entra; b == 0: fondo (sin pagina) */
static void bk_slide_frame(const uint16_t *a, const uint16_t *b, int off, int dir)
{
    uint16_t bg = bk_T().bg;
    if (off < 0) off = 0; if (off > W) off = W;
    for (int y = 0; y < H; y++) {
        uint16_t *d = fr0 + y * W; const uint16_t *ar = a + y * W, *br = b ? b + y * W : 0;
        if (dir > 0) {
            memcpy(d, ar + off, (W - off) * 2);
            if (br) memcpy(d + W - off, br, off * 2); else for (int x = W - off; x < W; x++) d[x] = bg;
        } else {
            if (br) memcpy(d, br + W - off, off * 2); else for (int x = 0; x < off; x++) d[x] = bg;
            memcpy(d + off, ar, (W - off) * 2);
        }
    }
    /* sombra suave en el borde de la pagina que se va (profundidad, como en Books) */
    int ex = dir > 0 ? W - off : off;
    for (int s = 1; s <= 5; s++) {
        int x = dir > 0 ? ex - 1 + s : ex - s;
        if (x < 0 || x >= W || off == 0 || off == W) continue;
        for (int y = 0; y < H; y++) fr0[y * W + x] = dim565(fr0[y * W + x], 26 + s);
    }
    present();
}
/* anima de "from" a "to" px; v0 velocidad inicial (px/ms) para empalmar con el dedo */
static void bk_slide_anim(const uint16_t *a, const uint16_t *b, int dir, float from, float to, float v0)
{
    float dist = fabsf(to - from); if (dist < 1) { bk_slide_frame(a, b, (int)to, dir); return; }
    float dur = v0 > 0.05f ? 3 * dist / v0 : 300; if (dur < 150) dur = 150; if (dur > 320) dur = 320;
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        bk_slide_frame(a, b, (int)(from + (to - from) * ease_out(t) + 0.5f), dir);
        if (t >= 1) break;
    }
}
/* pasar pagina con animacion (toque); 0 si no hay mas */
static int bk_turn(int dir)
{
    int c, p; if (!bk_step(rd_ch, rd_pg, dir, &c, &p)) {
        /* rebote corto en el principio/fin del libro */
        bk_anim = 1; const uint16_t *a = bk_page(rd_ch, rd_pg);
        bk_slide_anim(a, 0, dir, 0, 26, 0); bk_slide_anim(a, 0, dir, 26, 0, 0); bk_anim = 0;
        return 0;
    }
    bk_anim = 1;
    const uint16_t *a = bk_page(rd_ch, rd_pg), *b = bk_page(c, p);
    bk_slide_anim(a, b, dir, 0, W, 0);
    rd_ch = c; rd_pg = p; rd_turns++;
    bk_anim = 0;
    return 1;
}

/* --------------------------------------------- menu y hoja de temas */
static int bk_menu(void)
{
    struct bk_th T = bk_T();
    static uint16_t *base; if (!base) base = img_base;
    memcpy(base, fr0, HM_FRAME_BYTES);
    const char *it[2] = { tr("Contenido"), tr("Temas y ajustes") };
    int mw = 214, rh = 46, mh = 2 * rh, mx = W - 16 - mw, my = H - 84 - mh;   /* por encima de la barra y de "N de M" */
    for (int s = 0; s <= 6; s++) {
        float e = (float)ease_out(s / 6.0);
        memcpy(fr0, base, HM_FRAME_BYTES);
        int h = (int)(mh * (0.6f + 0.4f * e)), y = my + mh - h, w = (int)(mw * (0.7f + 0.3f * e)), x = mx + mw - w;
        /* sombra suave y filete: en el tema Original la tarjeta es blanca sobre blanco */
        for (int k = 4; k >= 1; k--) fill_rrect(fr0, x - k, y - k + 2, w + 2 * k, h + 2 * k, 13 + k, 0, (int)(14 * e));
        fill_rrect(fr0, x - 1, y - 1, w + 2, h + 2, 14, T.dim, (int)(50 * e));
        fill_rrect(fr0, x, y, w, h, 13, T.card, (int)(255 * e));
        if (s == 6) {
            for (int i = 0; i < 2; i++) {
                int ry = my + i * rh;
                if (i) for (int xx = mx + 14; xx < mx + mw; xx++) fr0[ry * W + xx] = bk_mix(T.dim, T.card, 10);
                draw_text(fr0, mx + 16, ry + 29, it[i], 16, T.fg, mw - 60, 0, H);
                int ix = mx + mw - 34, iy = ry + 15;
                if (!i) for (int k = 0; k < 3; k++) { fill_circle(fr0, ix + 2, iy + 3 + k * 7, 1.6f, T.fg, 255); fill_rrect(fr0, ix + 7, iy + 2 + k * 7, 14, 2, 1, T.fg, 255); }
                else { bk_text(fr0, ix - 2, iy + 16, "A", 13, T.fg, 0, 0); bk_text(fr0, ix + 8, iy + 16, "A", 19, T.fg, 0, 0); }
            }
        }
        present();
    }
    struct pkt p;
    while (cur_down && read_pkt(&p, 3000)) {}
    for (;;) {
        if (!read_pkt(&p, 400) || !p.down) continue;
        int x = p.x, y = p.y; while (cur_down && read_pkt(&p, 3000)) {}
        if (x >= mx && x < mx + mw && y >= my && y < my + mh) return (y - my) / rh;
        return -1;
    }
}
static void bk_relayout(void)
{
    /* conservar la posicion (desplazamiento del inicio de la pagina) con el nuevo tamano */
    struct bk_fc *f = bk_get(rd_ch); int off = f && rd_pg < f->np ? f->pg[rd_pg] : 0;
    bk_lp_set(); bk_gen++;
    f = bk_get(rd_ch);
    rd_pg = f && f->np ? bk_page_of(f->pg, f->np, off) : 0;
    bk_conf_save();
}
static void bk_sheet_draw(uint16_t *d, int top)
{
    struct bk_th T = bk_T();
    memcpy(d, bk_page(rd_ch, rd_pg), HM_FRAME_BYTES);
    for (int s = 1; s <= 8 && top - s >= 0; s++) for (int x = 0; x < W; x++) d[(top - s) * W + x] = dim565(d[(top - s) * W + x], 24 + s);
    fill_rrect(d, 0, top, W, H - top + 20, 14, T.sheet, 255);
    draw_text(d, 20, top + 36, tr("Temas y ajustes"), 18, T.fg, W - 80, 0, H);
    bk_xbtn(d, W - 34, top + 30, T.chrome, T.fg);
    /* tamano: A pequena | A grande */
    int y = top + 58;
    fill_rrect(d, 20, y, W - 40, 46, 11, T.card, 255);
    for (int yy = y + 10; yy < y + 36; yy++) d[yy * W + W / 2] = bk_mix(T.dim, T.card, 12);
    uint16_t c0 = bk_size_i > 0 ? T.fg : bk_mix(T.dim, T.card, 16), c1 = bk_size_i < BK_NSIZES - 1 ? T.fg : bk_mix(T.dim, T.card, 16);
    bk_text(d, W / 4 - 5, y + 30, "A", 15, c0, 0, 0);
    bk_text(d, 3 * W / 4 - 9, y + 33, "A", 24, c1, 0, 0);
    /* temas */
    static const char *const names[3] = { "Original", "Sepia", "Noche" };
    int tw = 86, gap = (W - 40 - 3 * tw) / 2, ty = top + 120;
    int save = bk_theme;
    for (int i = 0; i < 3; i++) {
        bk_theme = i; struct bk_th U = bk_T(); bk_theme = save;
        int x = 20 + i * (tw + gap);
        if (i == bk_theme) fill_rrect(d, x - 3, ty - 3, tw + 6, 64 + 6, 14, T.accent, 255);
        fill_rrect(d, x, ty, tw, 64, 12, U.bg, 255);
        if (i == 0 && bk_theme != 0) { fill_rrect(d, x, ty, tw, 64, 12, rgb(200, 200, 206), 255); fill_rrect(d, x + 1, ty + 1, tw - 2, 62, 11, U.bg, 255); }
        int w = bk_text_w("Aa", 22);
        bk_text(d, x + (tw - w) / 2, ty + 41, "Aa", 22, U.fg, 0, 0);
        const char *nm = tr(names[i]); int nw = tw_n(nm, 12, -1);
        draw_text(d, x + (tw - nw) / 2, ty + 84, nm, 12, i == bk_theme ? T.accent : T.dim, 0, 0, H);
    }
}
static void bk_sheet(void)
{
    int sh = 236, top0 = H - sh;
    bk_anim = 1;
    for (int pass = 0; pass < 1; pass++) {
        double t0 = now_ms();
        for (;;) { double t = (now_ms() - t0) / 280; if (t > 1) t = 1; bk_sheet_draw(fr0, H - (int)(sh * ease_out(t))); present(); if (t >= 1) break; }
    }
    bk_anim = 0;
    struct pkt p;
    while (cur_down && read_pkt(&p, 3000)) {}
    for (;;) {
        bk_sheet_draw(fr0, top0); present();
        if (!read_pkt(&p, 400) || !p.down) continue;
        int x = p.x, y = p.y, moved = 0;
        while (cur_down && read_pkt(&p, 3000)) if (cur_y - y > 30) moved = 1;
        if (moved || y < top0 || (y < top0 + 52 && x > W - 64)) break;
        if (y >= top0 + 58 && y < top0 + 104) {
            int ni = bk_size_i + (x < W / 2 ? -1 : 1);
            if (ni >= 0 && ni < BK_NSIZES) { bk_size_i = ni; bk_relayout(); }
        } else if (y >= top0 + 112 && y < top0 + 200) {
            int tw = 86, gap = (W - 40 - 3 * tw) / 2, i = (x - 20) / (tw + gap);
            if (i >= 0 && i < 3 && i != bk_theme) { bk_theme = i; bk_conf_save(); }
        }
    }
    bk_anim = 1;
    double t0 = now_ms();
    for (;;) { double t = (now_ms() - t0) / 240; if (t > 1) t = 1; bk_sheet_draw(fr0, top0 + (int)(sh * ease_out(t))); present(); if (t >= 1) break; }
    bk_anim = 0;
}

/* --------------------------------------------- contenido (indice) */
struct bk_te { const char *title; int ch, off, depth, lpg; char buf[24]; };
static struct bk_te *bk_tl; static int bk_ntl;
static float bk_tscroll;
#define BK_TROW 46
#define BK_THEAD 100
static void bk_toc_build(void)
{
    free(bk_tl); bk_tl = 0; bk_ntl = 0;
    pthread_mutex_lock(&bk_mu);
    int n = RB.fmt == BK_EPUB ? (RB.ntoc ? RB.ntoc : RB.nch) : bk_nttoc;
    bk_tl = calloc(n ? n : 1, sizeof *bk_tl);
    for (int i = 0; bk_tl && i < n; i++) {
        struct bk_te *e = &bk_tl[bk_ntl];
        if (RB.fmt == BK_EPUB && RB.ntoc) { e->title = RB.toc[i].title; e->ch = RB.toc[i].ch; e->off = RB.toc[i].frag[0] ? -1 : 0; e->depth = RB.toc[i].depth; e->lpg = __atomic_load_n(&bk_tpg[i], __ATOMIC_ACQUIRE); if (bk_toc_off[i] >= 0) e->off = bk_toc_off[i]; }
        else if (RB.fmt == BK_EPUB) { if (__atomic_load_n(&bk_np[i], __ATOMIC_ACQUIRE) == 0) continue; snprintf(e->buf, sizeof e->buf, tr("Sección %d"), bk_ntl + 1); e->title = e->buf; e->ch = i; e->off = 0; e->lpg = 0; }
        else { e->title = bk_ttoc[i].title; e->ch = bk_ttoc[i].ch; e->off = bk_ttoc[i].off; e->lpg = bk_ttpg ? bk_ttpg[i] : -1; }
        bk_ntl++;
    }
    pthread_mutex_unlock(&bk_mu);
}
/* entrada del indice en la que esta la pagina actual */
static int bk_toc_cur(void)
{
    struct bk_fc *f = bk_get(rd_ch); int off = f && rd_pg < f->np ? f->pg[rd_pg] : 0, best = -1;
    for (int i = 0; i < bk_ntl; i++) {
        int o = bk_tl[i].off < 0 ? 0 : bk_tl[i].off;
        if (bk_tl[i].ch < rd_ch || (bk_tl[i].ch == rd_ch && o <= off + 1)) best = i;
    }
    return best;
}
static void bk_toc_render(uint16_t *d, int pressed, int cur)
{
    struct bk_th T = bk_T();
    for (int i = 0; i < W * H; i++) d[i] = T.sheet;
    int tot = 0, g = bk_global(0, 0, &tot); (void)g;
    for (int i = 0; i < bk_ntl; i++) {
        int y = BK_THEAD + i * BK_TROW - (int)bk_tscroll;
        if (y + BK_TROW < BK_THEAD || y > H) continue;
        if (i == pressed) for (int yy = y; yy < y + BK_TROW; yy++) if (yy >= BK_THEAD && yy < H) for (int x = 0; x < W; x++) d[yy * W + x] = bk_mix(T.dim, T.sheet, 7);
        int x = 20 + bk_tl[i].depth * 16;
        char pn[16] = ""; int ptot;
        if (bk_tl[i].lpg >= 0 && bk_global(bk_tl[i].ch, bk_tl[i].lpg, &ptot)) snprintf(pn, sizeof pn, "%d", bk_global(bk_tl[i].ch, bk_tl[i].lpg, &ptot));
        int pw = pn[0] ? tw_n(pn, 13, -1) : 0;
        draw_text(d, x, y + 29, bk_tl[i].title, 15, i == cur ? T.accent : T.fg, W - x - 30 - pw, BK_THEAD, H);
        if (pn[0]) draw_text(d, W - 20 - pw, y + 29, pn, 13, T.dim, 0, BK_THEAD, H);
        int sy = y + BK_TROW - 1;
        if (sy >= BK_THEAD && sy < H) for (int xx = x; xx < W; xx++) d[sy * W + xx] = bk_mix(T.dim, T.sheet, 7);
    }
    if (!bk_ntl) {
        const char *m = RB.fmt == BK_TXT && !bk_ttoc_done ? tr("Calculando…") : tr("Sin capítulos");
        draw_text(d, (W - tw_n(m, 15, -1)) / 2, 240, m, 15, T.dim, 0, 0, H);
    }
    for (int y = 0; y < BK_THEAD; y++) for (int x = 0; x < W; x++) d[y * W + x] = T.sheet;
    memcpy(d, fr1, 30 * W * 2);
    draw_text(d, 20, 62, RB.title, 18, T.fg, W - 80, 0, H);
    if (RB.author[0]) draw_text(d, 20, 84, RB.author, 13, T.dim, W - 80, 0, H);
    bk_xbtn(d, W - 34, 58, T.chrome, T.fg);
    for (int x = 0; x < W; x++) d[(BK_THEAD - 1) * W + x] = bk_mix(T.dim, T.sheet, 9);
}
/* hoja de contenido sobre la pagina: sube/baja (k = 0..1 de abierta) */
static void bk_toc_anim(int open)
{
    bk_anim = 1;
    double t0 = now_ms();
    const uint16_t *pg = bk_page(rd_ch, rd_pg);
    for (;;) {
        double t = (now_ms() - t0) / (open ? 300 : 260); if (t > 1) t = 1;
        double e = open ? ease_out(t) : 1 - ease_out(t);
        compose_sheet(fr0, pg, img_under, (int)(H - (H - 30) * e), 32 - (int)(10 * e));
        present();
        if (t >= 1) break;
    }
    bk_anim = 0;
}
/* 1 si salto a otro sitio; -1: salir de la app (gesto de inicio) */
static int bk_toc(void)
{
    bk_toc_build();
    int cur = bk_toc_cur();
    bk_tscroll = cur > 3 ? (cur - 3) * BK_TROW : 0;
    float maxs = bk_ntl * BK_TROW - (H - BK_THEAD); if (maxs < 0) maxs = 0; if (bk_tscroll > maxs) bk_tscroll = maxs;
    /* la hoja empieza bajo la barra de estado: se pinta en img_under desplazada 30 px */
    bk_toc_render(img_base, -1, cur); memcpy(img_under, img_base + 30 * W, (H - 30) * W * 2);
    bk_toc_anim(1);
    struct pkt p; int jumped = 0; float vel = 0;
    for (;;) {
        bk_toc_render(fr0, -1, cur); present();
        if (fabsf(vel) > 0.03f) {
            static double lt; double t = now_ms(); if (!lt || t - lt > 100) lt = t - 16;
            float dt = (float)(t - lt); lt = t; bk_tscroll -= vel * dt; vel *= powf(0.9975f, dt);
            if (bk_tscroll < 0) { bk_tscroll = 0; vel = 0; } if (bk_tscroll > maxs) { bk_tscroll = maxs; vel = 0; }
            if (!read_pkt(&p, 0)) continue;
            vel = 0; if (p.down) { drain_until_up(); continue; }
        } else if (!read_pkt(&p, 400)) continue;
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms();
        if (sy >= 456) {
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { bk_toc_render(fr0, -1, cur); if (own_home_gesture(sx, sy, app_by_id(BK_APPID))) return -1; continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        if (back_zone(sx, sy)) { int bk = app_back_gesture(sx, sy); if (bk > 0) break; if (bk < 0) continue; }
        if (sy < BK_THEAD) { while (cur_down && read_pkt(&p, 3000)) {} if (sx > W - 70) break; continue; }
        int row = (int)((sy - BK_THEAD + bk_tscroll) / BK_TROW), moved = 0, ly = sy; double lt = ts; float s0 = bk_tscroll; vel = 0;
        int valid = row >= 0 && row < bk_ntl;
        if (valid) { bk_toc_render(fr0, row, cur); present(); }
        while (cur_down && read_pkt(&p, 30)) {
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved) {
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                bk_tscroll = s0 - (cur_y - sy); if (bk_tscroll < 0) bk_tscroll = 0; if (bk_tscroll > maxs) bk_tscroll = maxs;
                bk_toc_render(fr0, -1, cur); present();
            }
        }
        if (moved) { if (now_ms() - lt > 60) vel = 0; continue; }
        vel = 0;
        if (!valid) continue;
        /* saltar */
        struct bk_te *e = &bk_tl[row];
        struct bk_fc *f = bk_get(e->ch);
        int off = e->off;
        if (off < 0) { for (int i = 0; i < RB.ntoc; i++) if (RB.toc[i].title == e->title) off = bk_toc_off[i]; }
        if (off < 0) off = 0;
        if (f && f->np) { rd_ch = e->ch; rd_pg = bk_page_of(f->pg, f->np, off); }
        else { int c, q; if (bk_step(e->ch, 0, 1, &c, &q)) { rd_ch = c; rd_pg = q; } }
        jumped = 1; rd_chrome = 0;
        break;
    }
    bk_toc_render(img_base, -1, bk_toc_cur()); memcpy(img_under, img_base + 30 * W, (H - 30) * W * 2);
    bk_toc_anim(0);
    if (jumped) bk_save(0);
    return jumped;
}

/* --------------------------------------------- lector: bucle */
/* rectangulo de la portada de la entrada i en la biblioteca */
static void bk_cover_rect(struct bk_li *li, int *x, int *y)
{
    int i = (int)(li - bl); bk_cell_xy(i, x, y);
}
/* zoom portada <-> pagina; open=1 abre. lib = biblioteca de fondo (pantalla completa) */
static void bk_zoom(const uint16_t *lib, const uint16_t *page, const uint16_t *cover, int cx, int cy, int cw, int ch, int open)
{
    bk_anim = 1;
    double t0 = now_ms(), dur = open ? 340 : 300;
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        double e = open ? ease_out(t) : 1 - ease_out(t);
        dim_buf(fr0, lib, W * H, 32 - (int)(10 * e));
        int rx = (int)(cx * (1 - e)), ry = (int)(cy * (1 - e)), rw = (int)(cw + (W - cw) * e), rh = (int)(ch + (H - ch) * e);
        float rr = (float)(4 + 10 * (1 - e)); if (rr > rw / 6.f) rr = rw / 6.f;
        draw_scaled(fr0, page, rx, ry, rw, rh, rr * (e < 0.98 ? 1 : 0));
        int a = (int)(32 * (1 - e * 1.8)); if (a > 0 && cover) bk_blit_scaled(fr0, cover, BK_CW, BK_CHT, rx, ry, rw, rh, a, (int)rr, 0, H);
        present();
        if (t >= 1) break;
    }
    bk_anim = 0;
}
/* escena a la que vuelve el lector al cerrarse: la pinta en d y da el rectangulo de la portada (c0 = recorte superior) */
static void bk_back_scene(uint16_t *d, int *cx, int *cy, int *cw, int *ch, int *c0)
{
    if (bk_host_render) { bk_host_render(d); *cx = bk_host_x; *cy = bk_host_y; *cw = bk_host_w; *ch = bk_host_h; *c0 = bk_host_c0; return; }
    bk_lscroll = 0;
    bk_lib_render(d, -1); bk_cover_rect(rb_li, cx, cy); *cw = BK_CW; *ch = BK_CHT; *c0 = BK_LIST_Y;
}
/* bucle del lector; 0 = volver a la biblioteca, 1 = salir de la app (el inicio ya se mostro) */
static int bk_reader(void)
{
    struct pkt p;
    rd_chrome = 0;
    bk_show();
    for (;;) {
        int timeout = now_ms() < toast_until + 40 ? 30 : 400;
        if (!read_pkt(&p, 0)) {
            if (bk_idle_work()) continue;
            if (rd_turns >= 10 || (rd_turns && now_ms() - rd_saved_t > 60000)) bk_save(0);
            if (!read_pkt(&p, timeout)) { if (now_ms() < toast_until + 40) bk_show(); continue; }
        }
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms();
        /* desde abajo: inicio */
        if (sy >= 456) {
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { bk_save(1); bk_frame(rd_chrome ? 32 : 0); if (own_home_gesture(sx, sy, app_by_id(BK_APPID))) return 1; bk_show(); continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            continue;
        }
        if (rd_chrome) {
            if (sy >= 30 && sy < 86 && sx < 72) { while (cur_down && read_pkt(&p, 3000)) {} return 0; }
            if (sy >= H - 64 && sx >= W - 80) {
                while (cur_down && read_pkt(&p, 3000)) {}
                bk_save(0);
                int m = bk_menu();
                if (m == 0) {
                    int r = bk_toc(); if (r < 0) return 1;
                    if (r > 0) { bk_show(); continue; }
                } else if (m == 1) bk_sheet();
                bk_show(); continue;
            }
            if (sy >= H - 66 && sy < 456 && sx < W - 80) {    /* barra de progreso */
                bk_anim = 1;
                int x0 = 24, x1 = W - 92, tch = rd_ch, tpg = rd_pg;
                while (cur_down) {
                    read_pkt(&p, 30);
                    float fr = (float)(cur_x - x0) / (x1 - x0); if (fr < 0) fr = 0; if (fr > 1) fr = 1;
                    int tot, g = bk_global(0, 0, &tot);
                    if (g && tot) {   /* pagina global -> capitulo y pagina */
                        int target = (int)(fr * (tot - 1) + 0.5f), acc = 0;
                        for (int c = 0; c < RB.nch; c++) { int n = bk_np[c]; if (target < acc + n) { tch = c; tpg = target - acc; break; } acc += n; }
                    } else {
                        long long pos = (long long)(fr * RB.total); tch = 0;
                        for (int c = 0; c < RB.nch; c++) if (RB.ch[c].r0 <= pos) tch = c;
                        struct bk_fc *f = bk_get(tch); tpg = 0;
                        if (f && f->np) { double in = (double)(pos - RB.ch[tch].r0) / (RB.ch[tch].r1 - RB.ch[tch].r0); tpg = (int)(in * (f->np - 1) + 0.5); if (tpg >= f->np) tpg = f->np - 1; }
                    }
                    memcpy(fr0, bk_page(rd_ch, rd_pg), HM_FRAME_BYTES); bk_chrome_draw(fr0, tch, tpg); present();
                }
                bk_anim = 0;
                struct bk_fc *f = bk_get(tch);
                if (f && f->np && (tch != rd_ch || tpg != rd_pg)) { rd_ch = tch; rd_pg = tpg < f->np ? tpg : f->np - 1; bk_save(0); }
                else { int c, q; if ((!f || !f->np) && bk_step(tch, 0, 1, &c, &q)) { rd_ch = c; rd_pg = q; } }
                bk_show(); continue;
            }
        }
        /* pagina: arrastrar, tirar hacia abajo o tocar */
        int moved = 0;
        while (cur_down && !moved) {
            if (abs(cur_x - sx) > 8 || abs(cur_y - sy) > 10) moved = abs(cur_x - sx) >= abs(cur_y - sy) ? 1 : (cur_y > sy ? 2 : 3);
            else read_pkt(&p, 30);
        }
        if (moved == 1) {   /* pasar pagina siguiendo al dedo */
            int dir = cur_x < sx ? 1 : -1, c, q, has = bk_step(rd_ch, rd_pg, dir, &c, &q);
            bk_anim = 1;
            if (rd_chrome) bk_chrome_fade(0), bk_anim = 1;
            const uint16_t *a = bk_page(rd_ch, rd_pg), *b = has ? bk_page(c, q) : 0;
            a = bk_page(rd_ch, rd_pg);                      /* sigue en cache (las dos son las mas recientes) */
            float off = 0, v = 0; int lx = cur_x; double lt = now_ms();
            for (;;) {
                int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
                double t = now_ms(); if (t > lt) { v = 0.6f * v + 0.4f * (lx - cur_x) * dir / (float)(t - lt); lx = cur_x; lt = t; }
                float raw = (sx - cur_x) * dir;
                off = raw < 0 ? 0 : has ? raw : raw * 0.25f;
                if (off > W) off = W;
                bk_slide_frame(a, b, (int)off, dir);
                if ((got && !p.down) || !cur_down) break;
                if (!got) msleep(2);
            }
            if (now_ms() - lt > 80) v = 0;
            if (has && (off > W * 0.33f || v > 0.35f) && v > -0.2f) { bk_slide_anim(a, b, dir, off, W, v); rd_ch = c; rd_pg = q; rd_turns++; }
            else bk_slide_anim(a, b, dir, off, 0, v < 0 ? -v : 0);
            bk_anim = 0;
            bk_show(); continue;
        }
        if (moved == 2) {   /* tirar hacia abajo: cerrar el libro */
            bk_anim = 1;
            bk_lscroll = 0; bk_save(0);
            pthread_mutex_lock(&bk_mu); qsort(bl, bln, sizeof *bl, bk_lcmp); pthread_mutex_unlock(&bk_mu);
            for (int i = 0; i < bln; i++) if (bl[i].h == rb_li->h) rb_li = &bl[i];
            int cx, cy, cw, ch, c0; bk_back_scene(img_base, &cx, &cy, &cw, &ch, &c0);
            const uint16_t *pg = bk_page(rd_ch, rd_pg);
            float pr = 0, v = 0; int ly = cur_y; double lt = now_ms();
            for (;;) {
                int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
                double t = now_ms(); if (t > lt) { v = 0.6f * v + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                pr = (cur_y - sy) / 420.f; if (pr < 0) pr = 0; if (pr > 1) pr = 1;
                float s = 1 - 0.35f * pr; int rw = (int)(W * s), rh = (int)(H * s);
                zoom_frame(fr0, img_base, 22 + (int)(10 * pr), pg, (W - rw) / 2 + (cur_x - sx) / 3, (int)(pr * 160), rw, rh, 6 + 14 * pr);
                present();
                if ((got && !p.down) || !cur_down) break;
                if (!got) msleep(2);
            }
            bk_anim = 0;
            if (pr > 0.22f || v > 0.5f) {
                /* desde donde se solto hasta la portada */
                const uint16_t *cv = bk_cover(rb_li);
                float s = 1 - 0.35f * pr; int x0 = (W - (int)(W * s)) / 2 + (cur_x - sx) / 3, y0 = (int)(pr * 160), w0 = (int)(W * s), h0 = (int)(H * s);
                double t0 = now_ms(); bk_anim = 1;
                for (;;) {
                    double t = (now_ms() - t0) / 280; if (t > 1) t = 1; double e = ease_out(t);
                    dim_buf(fr0, img_base, W * H, 22 + (int)(10 * (pr + (1 - pr) * e)));
                    int rx = x0 + (int)((cx - x0) * e), ry = y0 + (int)((cy - y0) * e), rw = w0 + (int)((cw - w0) * e), rh = h0 + (int)((ch - h0) * e);
                    draw_scaled(fr0, pg, rx, ry, rw, rh, 6 + 14 * pr * (1 - e) + 4 * e);
                    if (cv) bk_blit_scaled(fr0, cv, BK_CW, BK_CHT, rx, ry, rw, rh, (int)(32 * e), 4, c0, H);
                    present(); if (t >= 1) break;
                }
                bk_anim = 0;
                return 2;   /* la escena de vuelta ya esta en pantalla */
            }
            /* volver */
            double t0 = now_ms(); float p0 = pr;
            for (;;) {
                double t = (now_ms() - t0) / 220; if (t > 1) t = 1; float q = p0 * (1 - (float)ease_out(t));
                float s = 1 - 0.35f * q; int rw = (int)(W * s), rh = (int)(H * s);
                zoom_frame(fr0, img_base, 22 + (int)(10 * q), pg, (W - rw) / 2, (int)(q * 160), rw, rh, q > 0.01f ? 6 + 14 * q : 0);
                present(); if (t >= 1) break;
            }
            bk_show(); continue;
        }
        if (moved == 3) { while (cur_down && read_pkt(&p, 3000)) {} continue; }
        /* toque */
        (void)ts;
        if (sx < W / 4) { if (rd_chrome) bk_chrome_fade(0); bk_turn(-1); bk_show(); }
        else if (sx > W * 3 / 4) { if (rd_chrome) bk_chrome_fade(0); bk_turn(1); bk_show(); }
        else { if (!rd_chrome) bk_save(0); bk_chrome_fade(!rd_chrome); }
    }
}

/* --------------------------------------------- app */
static uint64_t bk_resume_hh[2];             /* libro abierto al salir con el gesto de inicio (Libros, Gutenberg) */
#define bk_resume_h bk_resume_hh[bk_gut]
/* abre el libro i de la biblioteca (con zoom desde su portada); 1 = salir de la app */
static int bk_read(int i, int from_icon, int ix, int iy)
{
    struct bk_li *li = &bl[i];
    int cx = 0, cy = 0, cw = BK_CW, ch = BK_CHT, c0 = BK_LIST_Y;
    if (!from_icon) {
        if (bk_host_render) { bk_host_render(img_base); cx = bk_host_x; cy = bk_host_y; cw = bk_host_w; ch = bk_host_h; }
        else { bk_lib_render(img_base, -1); bk_cell_xy(i, &cx, &cy); }
    }
    uint64_t h = li->h;
    if (!bk_open_book(li)) { toast(tr("No se pudo abrir el libro")); bk_meta_start(); return 0; }
    pthread_mutex_lock(&bk_mu); qsort(bl, bln, sizeof *bl, bk_lcmp); pthread_mutex_unlock(&bk_mu);
    for (int k = 0; k < bln; k++) if (bl[k].h == h) rb_li = &bl[k];
    const uint16_t *pg = bk_page(rd_ch, rd_pg);
    if (from_icon) {
        render_launcher(img_base, now_ms());
        double t0 = now_ms();
        for (;;) {
            double t = (now_ms() - t0) / 300; if (t > 1) t = 1; double e = ease_out(t);
            int w = (int)(L.icon + (W - L.icon) * e), hh = (int)(L.icon + (H - L.icon) * e);
            for (int k = 0; k < W * H; k++) fr0[k] = dim565(img_base[k], 32 - (int)(8 * e));
            draw_scaled(fr0, pg, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, hh, win_radius(w, hh, e));
            present(); if (t >= 1) break;
        }
    } else bk_zoom(img_base, pg, bk_cover(rb_li), cx, cy, cw, ch, 1);
    bk_resume_h = rb_li->h;
    int r = bk_reader();
    bk_save(1);
    if (r == 1) { bk_close_book(); return 1; }       /* el libro sigue "abierto" al volver a la app */
    bk_resume_h = 0;
    if (r == 0) {   /* cerrar con la X: zoom a la portada */
        pthread_mutex_lock(&bk_mu); qsort(bl, bln, sizeof *bl, bk_lcmp); pthread_mutex_unlock(&bk_mu);
        for (int j = 0; j < bln; j++) if (bl[j].h == h) rb_li = &bl[j];
        bk_back_scene(img_base, &cx, &cy, &cw, &ch, &c0);
        memcpy(img_app, bk_page(rd_ch, rd_pg), HM_FRAME_BYTES);
        bk_zoom(img_base, img_app, bk_cover(rb_li), cx, cy, cw, ch, 0);
    }
    bk_close_book();
    bk_meta_start();                               /* portadas pendientes */
    return 0;
}
static void bk_lib_present(int pressed) { bk_lib_render(fr0, pressed); present(); }
static void bk_lib_render0(uint16_t *d) { bk_lib_render(d, -1); }
/* menu de un libro (pulsacion larga o los tres puntos); rend pinta el fondo. Lo usa tambien Gutenberg */
static void bk_li_menu(int hit, void (*rend)(uint16_t *d))
{
    rend(fr0);
    struct bk_li *li = &bl[hit];
    const char *o[3] = { li->done ? tr("Marcar como no leído") : tr("Marcar como terminado"), tr("Empezar desde el principio"), tr("Eliminar") };
    int a = action_sheet(li->title, o, 3, 2);
    struct bk_pos ps; bk_pos_load(li->h, &ps);
    if (a == 0) { ps.done = !li->done; ps.pct = ps.done ? 1 : 0; if (!ps.done) { ps.ch = 0; ps.off = 0; ps.opened = 0; } else if (ps.opened <= 0) ps.opened = (double)time(0); bk_pos_save(li->h, &ps); li->done = ps.done; li->pct = ps.pct; li->opened = ps.opened; }
    else if (a == 1) { ps.ch = 0; ps.off = 0; ps.pct = 0; ps.done = 0; bk_pos_save(li->h, &ps); li->done = 0; li->pct = 0; }
    else if (a == 2) {
        char q[300]; snprintf(q, sizeof q, tr("¿Eliminar «%s»? No se puede deshacer."), li->title);
        rend(fr0);
        if (confirm(q, tr("Eliminar"))) {
            bk_meta_join();
            char pp[200];
            if (unlink(li->path)) toast(tr("No se pudo eliminar del todo"));
            else {
                snprintf(pp, sizeof pp, BK_DIR "/%016llx.pos", (unsigned long long)li->h); unlink(pp);
                snprintf(pp, sizeof pp, BK_DIR "/%016llx.cov", (unsigned long long)li->h); unlink(pp);
                pthread_mutex_lock(&bk_mu); memmove(&bl[hit], &bl[hit + 1], (bln - hit - 1) * sizeof *bl); bln--; pthread_mutex_unlock(&bk_mu);
                toast(tr("Eliminado"));
                float mx = bk_lmax(); if (bk_lscroll > mx) bk_lscroll = mx;
            }
        }
    }
    pthread_mutex_lock(&bk_mu); qsort(bl, bln, sizeof *bl, bk_lcmp); pthread_mutex_unlock(&bk_mu);
}
static void books_app_at(int ix, int iy)
{
    have_under = 0;                               /* se reutilizan img_under / img_sheet / img_base */
    bk_conf_load();
    bk_lib_scan();
    bk_lscroll = 0;
    /* cada app recuerda su pestana; al volver a un libro abierto desde Gutenberg, debajo queda Descargas */
    bk_tab = bk_tab_last[bk_gut];
    if (bk_gut ? bk_tab < 2 : bk_tab > 2) bk_tab = bk_gut ? 2 : 0;
    if (bk_gut && bk_resume_h) bk_tab = 3;
    ab_open();
    int gone = 0;
    /* volver al libro que estaba abierto (como iOS) */
    if (bk_resume_h) {
        int i = -1; for (int k = 0; k < bln; k++) if (bl[k].h == bk_resume_h) i = k;
        if (i >= 0) {
            if (bk_gut) { gut_focus_h = bk_resume_h; bk_host_render = gut_host_render; }
            int r = bk_read(i, 1, ix, iy);
            bk_host_render = 0; gut_focus_h = 0;
            if (r) { gone = 1; goto out; }
        }
        bk_resume_h = 0;
        if (i < 0) {   /* el libro ya no esta: la app aparece sin zoom */
            if (bk_tab) ab_render_tab(fr0); else bk_lib_render(fr0, -1);
            present();
        }
    } else {
        if (bk_tab) ab_render_tab(img_app); else bk_lib_render(img_app, -1);
        render_launcher(img_base, now_ms());
        double t0 = now_ms();
        for (;;) {   /* apertura con zoom desde el icono */
            double t = (now_ms() - t0) / 300; if (t > 1) t = 1; double e = ease_out(t);
            int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
            for (int k = 0; k < W * H; k++) fr0[k] = dim565(img_base[k], 32 - (int)(8 * e));
            draw_scaled(fr0, img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
            present(); if (t >= 1) break;
        }
    }
    if (!bk_tab) bk_lib_present(-1);
    float vel = 0; struct pkt p;
    for (;;) {
        if (bk_tab) {   /* Audiolibros | Gutenberg: Explorar y Descargas */
            if (ab_run()) { gone = 1; break; }
            bk_lib_scan();                        /* (puede haber EPUB nuevos de Gutenberg) */
            bk_lib_present(-1); continue;
        }
        if (fabsf(vel) > 0.03f) {
            static double lt; double t = now_ms(); if (!lt || t - lt > 100) lt = t - 16;
            float dt = (float)(t - lt); lt = t; bk_lscroll -= vel * dt; vel *= powf(0.9975f, dt);
            float mx = bk_lmax(); if (bk_lscroll < 0) { bk_lscroll = 0; vel = 0; } if (bk_lscroll > mx) { bk_lscroll = mx; vel = 0; }
            bk_lib_present(-1);
            if (!read_pkt(&p, 0)) continue;
            vel = 0; if (p.down) { drain_until_up(); continue; }
        } else if (!read_pkt(&p, bk_meta_run || now_ms() < toast_until + 40 ? 60 : 400)) {
            if (bk_lib_dirty || now_ms() < toast_until + 40) { bk_lib_dirty = 0; bk_lib_present(-1); }
            ab_np_tick(0);                        /* progreso del audiolibro que suena */
            if (ab_lib_epub_done()) { bk_lib_scan(); bk_lib_present(-1); }   /* EPUB de Gutenberg */
            continue;
        }
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms();
        if (sy >= 456) {   /* desde abajo: inicio (la ventana sigue al dedo; mantener 1 s: multitarea) */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { bk_lib_render(fr0, -1); if (own_home_gesture(sx, sy, app_by_id(BK_APPID))) { gone = 1; break; } bk_lib_present(-1); continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            if (sx * 3 / W) bk_tab = sx * 3 / W;                /* toque en la barra de pestanas */
            continue;
        }
        if (sy >= AB_TABY) {   /* barra de pestanas */
            while (cur_down && read_pkt(&p, 3000)) {}
            if (sx * 3 / W) bk_tab = sx * 3 / W;
            else { bk_lscroll = 0; bk_lib_present(-1); }
            continue;
        }
        if (sy < BK_LIST_Y) { while (cur_down && read_pkt(&p, 3000)) {} continue; }
        if (bk_gbtn_w && sy >= 338 && sy < 390 && abs(sx - W / 2) <= bk_gbtn_w / 2 + 6) {   /* biblioteca vacia: abrir Gutenberg */
            bk_lib_present(-2);
            while (cur_down && read_pkt(&p, 3000)) {}
            if (abs(cur_x - sx) < 20 && abs(cur_y - sy) < 20) { ab_src = 1; bk_tab = 2; continue; }   /* libros en Explorar */
            bk_lib_present(-1); continue;
        }
        /* celda tocada */
        int hit = -1, dots = 0;
        for (int i = 0; i < bln; i++) {
            int x, y; bk_cell_xy(i, &x, &y);
            if (sx >= x - 6 && sx < x + BK_CW + 6 && sy >= y && sy < y + BK_CHT + 44) { hit = i; dots = sy > y + BK_CHT + 14 && sx > x + BK_CW - 34; break; }
        }
        if (hit >= 0) bk_lib_present(hit);
        int moved = 0, ly = sy, longp = 0; double lt = ts; float s0 = bk_lscroll; vel = 0;
        while (cur_down) {
            int got = read_pkt(&p, 30);
            if (got && !p.down) break;
            if (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 12) moved = 1;
            if (moved) {
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                float mx = bk_lmax(); bk_lscroll = s0 - (cur_y - sy); if (bk_lscroll < 0) bk_lscroll = 0; if (bk_lscroll > mx) bk_lscroll = mx;
                bk_lib_present(-1);
            } else if (hit >= 0 && now_ms() - ts > 450) { longp = 1; break; }
        }
        if (moved) { if (now_ms() - lt > 60) vel = 0; continue; }
        vel = 0;
        if (hit < 0) { bk_lib_present(-1); continue; }
        if (longp || dots) {
            while (cur_down && read_pkt(&p, 3000)) {}
            bk_li_menu(hit, bk_lib_render0);
            bk_lib_present(-1); continue;
        }
        if (bk_read(hit, 0, 0, 0)) { gone = 1; break; }
        bk_lib_present(-1);
    }
out:
    if (!gone) {   /* (no se llega: la biblioteca sale siempre con el gesto de inicio) */
        bk_lib_render(img_app, -1); render_launcher(img_base, now_ms());
    }
    bk_meta_join();                               /* (el hilo guarda el indice si cambio algo) */
    bk_tab_last[bk_gut] = bk_tab;
    ab_close();
    bk_cover_free();
    free(bl); bl = 0; bln = 0;
    free(bk_tl); bk_tl = 0; bk_ntl = 0;
    drain_until_up();
    if (!gone) launcher_present();
}
/* punto de entrada: zoom desde el centro (sin coordenadas del icono) */
static void books_app(void) { books_app_at((W - L.icon) / 2, (H - L.icon) / 2); }
