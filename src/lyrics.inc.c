/* lyrics.inc.c - Buscar caratula y letra para canciones (editor de etiquetas y Archivos).
 *
 * Automatico: bibliotecas publicas a traves de `hibynet` (src/hibynet.c):
 *   caratulas: iTunes Search + MusicBrainz/Cover Art Archive; letras: LRCLIB (sincronizadas).
 * Manual: cambiar los terminos de busqueda con el teclado, o elegir una imagen de la SD
 * (por ejemplo, descargada desde el ordenador o con el navegador).
 * Donde se guarda (lo que lee el reproductor de HiBy): ver docs/LETRAS_CARATULAS.md.
 *   - caratula: cover.jpg en la carpeta del album
 *   - letra: <cancion>.lrc junto al fichero de audio */
#define NETBIN "/usr/data/hibymod/shell/hibynet"
#define NETDIR "/usr/data/hibymod/cache/net"

struct cover_c { char src[16], url[300], artist[96], album[96], year[8]; uint16_t *thumb; };
struct lyr_c { char id[24], artist[96], title[128], album[96]; int dur, synced; };


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
        char *a2[] = { NETBIN, "cover-get", cv[i].url, jp, "500", 0 }; char o[256];
        if (run_capture(a2, o, sizeof o) < 0) continue;
        int fd = open(th, O_RDONLY);
        if (fd >= 0) { cv[i].thumb = malloc(CV_T * CV_T * 2); if (read(fd, cv[i].thumb, CV_T * CV_T * 2) != CV_T * CV_T * 2) { free(cv[i].thumb); cv[i].thumb = 0; } close(fd); }
    }
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
            if (line_edit(tr("Artista"), tr("Artista"), a, sizeof a) && line_edit(tr("Álbum"), tr("Álbum"), b, sizeof b)) cover_fetch(a, b);
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

/* ------------------------------------------------------------------ letras */
#define LY_MAX 12
static struct lyr_c ly[LY_MAX]; static int nly;
static int lyrics_fetch(const char *artist, const char *title, const char *album)
{
    nly = 0;
    progress_card(0, 0, -1); progress_card(tr("Buscando letras"), 0, 0.4);
    char *argv[] = { NETBIN, "lyrics", (char *)artist, (char *)title, (char *)(album ? album : ""), 0 };
    static char out[16384];
    if (run_capture(argv, out, sizeof out) <= 0) return 0;
    for (char *l = strtok(out, "\n"); l && nly < LY_MAX; l = strtok(0, "\n")) {
        char *f[6]; if (split_tabs(l, f, 6) < 6) continue;
        struct lyr_c *c = &ly[nly++]; memset(c, 0, sizeof *c);
        snprintf(c->id, sizeof c->id, "%s", f[0]); snprintf(c->artist, sizeof c->artist, "%s", f[1]);
        snprintf(c->title, sizeof c->title, "%s", f[2]); snprintf(c->album, sizeof c->album, "%s", f[3]);
        c->dur = atoi(f[4]); c->synced = atoi(f[5]);
    }
    return nly;
}
static void lyrics_render(uint16_t *d, const char *q, int sel)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    draw_text(d, 16, 116, q, 12, rgb(142, 142, 147), W - 32, 0, H);
    int y = 128;
    if (nly) fill_rrect(d, 16, y, W - 32, nly * 54 > H - y - 8 ? H - y - 8 : nly * 54, 10, rgb(28, 28, 30), 255);
    for (int i = 0; i < nly && y + 54 <= H; i++, y += 54) {
        if (i == sel) fill_rrect(d, 16, y, W - 32, 54, 0, rgb(58, 58, 60), 255);
        if (i) for (int x = 32; x < W - 16; x++) d[y * W + x] = rgb(56, 56, 58);
        draw_text(d, 32, y + 24, ly[i].title, 15, 0xFFFF, W - 32 - 64, 0, H);
        char sub[200]; snprintf(sub, sizeof sub, "%s · %s · %d:%02d", ly[i].artist, ly[i].album, ly[i].dur / 60, ly[i].dur % 60);
        draw_text(d, 32, y + 43, sub, 12, rgb(142, 142, 147), W - 32 - 64, 0, H);
        if (ly[i].synced) { fill_rrect(d, W - 60, y + 17, 34, 20, 10, rgb(48, 209, 88), 255); draw_text(d, W - 54, y + 32, "LRC", 11, 0, 0, 0, H); }
    }
    if (!nly) draw_wrapped(d, 24, 150, W - 48, tr("No se encontraron letras. Prueba a cambiar la búsqueda."), 14, rgb(152, 152, 159), 19, 1);
    lx_nav(d, tr("Cancelar"), tr("Letra"), tr("Buscar"));
    draw_toast(d);
}
/* vista previa de la letra (sin marcas de tiempo) con "Guardar" */
static char lp_txt[24000];
static int lp_draw(float sc, const char *title)        /* devuelve el alto total */
{
    for (int i = 0; i < W * H; i++) fr0[i] = 0;
    int y = 110 - (int)sc;
    for (char *s = lp_txt; *s; ) {
        char *e = strchr(s, '\n'); char line[512]; int ln = e ? (int)(e - s) : (int)strlen(s); if (ln > 500) ln = 500;
        memcpy(line, s, ln); line[ln] = 0;
        int vis = y > 60 && y < H + 10;
        y += vis ? draw_wrapped(fr0, 20, y, W - 40, line[0] ? line : " ", 15, 0xFFFF, 22, 1) : 22;
        s = e ? e + 1 : s + ln;
    }
    lx_nav(fr0, tr("Atrás"), title, tr("Guardar"));
    present();
    return y + (int)sc;
}
static int lyrics_preview(const char *lrc, const char *title)
{
    int n = 0; FILE *f = fopen(lrc, "r"); if (!f) return 0;
    char l[512];
    while (fgets(l, sizeof l, f) && n < (int)sizeof lp_txt - 600) {
        if ((unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) memmove(l, l + 3, strlen(l + 3) + 1);   /* BOM */
        if (l[0] == '[' && l[1] >= 'a' && l[1] <= 'z') continue;                 /* [ar:] [ti:] [al:]... */
        char *s = l; while (*s == '[') { char *e = strchr(s, ']'); if (!e) break; s = e + 1; }
        if (!*s) continue;
        n += snprintf(lp_txt + n, sizeof lp_txt - n, "%s", s);
    }
    fclose(f);
    float sc = 0; struct pkt p; int total = lp_draw(sc, title);
    for (;;) {
        if (!read_pkt(&p, 400) || !p.down) continue;
        int x = p.x, sy = p.y, moved = 0; float s0 = sc;
        while (cur_down && read_pkt(&p, 30)) {
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved) { float mx = total - H + 20; if (mx < 0) mx = 0; sc = s0 - (cur_y - sy); if (sc < 0) sc = 0; if (sc > mx) sc = mx; total = lp_draw(sc, title); }
        }
        if (moved) continue;
        if (sy < 94 && x < 100) return 0;
        if (sy < 94 && x > W - 100) return 1;
    }
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
        char *argv[] = { NETBIN, "lyrics", (char *)ar, (char *)ti, (char *)al, 0 };
        static char out[8192]; char id[24] = ""; int best_synced = -1;
        if (ti[0] && run_capture(argv, out, sizeof out) > 0)
            for (char *sv = 0, *l = strtok_r(out, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
                char *f[6]; if (split_tabs(l, f, 6) < 6) continue;
                int sy = atoi(f[5]); if (sy > best_synced) { best_synced = sy; snprintf(id, sizeof id, "%s", f[0]); if (sy) break; }
            }
        ht_tags_free(&t);
        if (!id[0]) continue;
        char dst[600]; snprintf(dst, sizeof dst, "%s", v[i]);
        char *dot = strrchr(dst, '.'), *sl = strrchr(dst, '/'); if (dot && (!sl || dot > sl)) *dot = 0;
        strncat(dst, ".lrc", sizeof dst - strlen(dst) - 1);
        char *a2[] = { NETBIN, "lyrics-get", id, NETDIR "/l.lrc", 0 }; char o[256];
        if (run_capture(a2, o, sizeof o) >= 0 && !access(NETDIR "/l.lrc", R_OK) && !copy_file(NETDIR "/l.lrc", dst)) ok++;
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
    char a[128], t[160], b[128], q[300];
    snprintf(a, sizeof a, "%s", artist ? artist : ""); snprintf(t, sizeof t, "%s", title ? title : ""); snprintf(b, sizeof b, "%s", album ? album : "");
    lyrics_fetch(a, t, b);
    for (;;) {
        snprintf(q, sizeof q, "%s — %s", a, t);
        lyrics_render(fr0, q, -1); present();
        struct pkt p;
        if (!read_pkt(&p, now_ms() < toast_until + 40 ? 30 : 400) || !p.down) continue;
        int x = p.x, y = p.y; while (cur_down && read_pkt(&p, 3000)) {}
        if (y >= 456 && cur_y < y - 20) return;
        if (y < 94 && x < 100) return;
        if (y < 94 && x > W - 90) {
            if (line_edit(tr("Título"), tr("Título"), t, sizeof t) && line_edit(tr("Artista"), tr("Artista"), a, sizeof a)) lyrics_fetch(a, t, "");
            continue;
        }
        int i = (y - 128) / 54; if (y < 128 || i < 0 || i >= nly) continue;
        lyrics_render(fr0, q, i); present();
        progress_card(0, 0, -1); progress_card(tr("Descargando letra"), 0, 0.5);
        char *argv[] = { NETBIN, "lyrics-get", ly[i].id, NETDIR "/l.lrc", 0 }; char o[256];
        if (run_capture(argv, o, sizeof o) < 0 || access(NETDIR "/l.lrc", R_OK)) { toast(tr("No se pudo descargar")); continue; }
        if (lyrics_preview(NETDIR "/l.lrc", ly[i].title)) {
            char dst[600]; snprintf(dst, sizeof dst, "%s", path);
            char *dot = strrchr(dst, '.'), *sl = strrchr(dst, '/'); if (dot && (!sl || dot > sl)) *dot = 0;
            strncat(dst, ".lrc", sizeof dst - strlen(dst) - 1);
            int r = copy_file(NETDIR "/l.lrc", dst); sync();
            toast(r ? tr("No se pudo guardar") : tr("Letra guardada"));
            if (!r) return;
        }
    }
}
