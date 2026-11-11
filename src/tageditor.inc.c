/* tageditor.inc.c - editor de etiquetas (una o varias canciones a la vez).
 * Lectura/escritura en el propio shell con hibytag.c (sin base de datos); autocompletado y
 * actualizacion de la biblioteca de HiBy con el programa hibytag (SQLite) como proceso aparte.
 *
 * Formulario estilo iOS (desplazamiento con inercia, gesto de atras desde el borde):
 *   - las 8 etiquetas de siempre (titulo, artista, album...);
 *   - "Mas etiquetas": todas las demas que tenga el fichero (compositor, comentario, letra, sello,
 *     ISRC, MusicBrainz, ReplayGain (solo lectura)...), con nombre legible; "Anadir campo";
 *   - "Archivo": nombre del fichero (una cancion) o "Renombrar archivos" (album: patron con vista
 *     previa y, si se quiere, la carpeta "Album" o "Artista/Album").
 * Con varias canciones: valor comun o "Varios valores"; solo se escriben los campos cambiados.
 * Los textos largos (letra, comentario) se editan en un editor de varias lineas.
 * Seguridad: hibytag escribe en sitio si cabe en el relleno o con temporal + fsync + rename; el
 * renombrado es en dos fases (con vuelta atras) y las rutas de la base se cambian en una sola
 * transaccion (hibytag dbrename); si la base falla, se deshace el renombrado. */
#define HT_API static
#define HIBYTAG_NO_DB
#include "hibytag.c"

#define TAGBIN ASSETS "/hibytag"
#define N_(s) s                  /* texto para tr() guardado en una tabla */
static int confirm(const char *title, const char *button);
static int action_sheet(const char *title, const char **opts, int n, int destructive_idx);
static int draw_wrapped(uint16_t *dst, int x, int y, int w, const char *s, int size, uint16_t col, int lh, int draw);
static void cover_search(char *const *paths, int n, const char *artist, const char *album);
static void lyrics_search(const char *path, const char *artist, const char *title, const char *album);
static const char *const TE_FIELDS[HT_NFIELDS] = { N_("Título"), N_("Artista"), N_("Álbum"), N_("Artista del álbum"), N_("Género"), N_("Año"), N_("Pista"), N_("Disco") };
static const char *const TE_SUGG[HT_NFIELDS] = { 0, "artist", "album", "albumartist", "genre", 0, 0, 0 };

/* ejecuta TAGBIN con argv y devuelve su salida (stdout) en buf */
static int run_capture(char *const argv[], char *buf, int cap)
{
    int fd[2]; if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    int n = 0;
    if (!r) { ssize_t k; while (n < cap - 1 && (k = read(fd[0], buf + n, cap - 1 - n)) > 0) n += (int)k; }
    buf[n] = 0; close(fd[0]);
    if (!r) { int stt; waitpid(pid, &stt, 0); }
    return r ? -1 : n;
}
/* como run_capture con stdout y stderr juntos; devuelve el codigo de salida (-1 si no arranco) */
static int run_status(char *const argv[], char *buf, int cap)
{
    int fd[2]; if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    int n = 0, stt = 0;
    if (!r) {
        char tmp[256]; ssize_t k;
        while ((k = read(fd[0], tmp, sizeof tmp)) > 0) { int c = (int)k; if (c > cap - 1 - n) c = cap - 1 - n; if (c > 0) { memcpy(buf + n, tmp, c); n += c; } }
    }
    buf[n] = 0; close(fd[0]);
    if (r) return -1;
    waitpid(pid, &stt, 0);
    return WIFEXITED(stt) ? WEXITSTATUS(stt) : -1;
}

/* ------------------------------------------------------------------ edicion con sugerencias */
static char sg[5][128]; static int nsg;
static const char *sg_field;
static void sg_fetch(const char *prefix)
{
    nsg = 0; if (!sg_field || !*prefix) return;
    char out[1024];
    char *argv[] = { TAGBIN, "suggest", (char *)sg_field, (char *)prefix, "5", 0 };
    if (run_capture(argv, out, sizeof out) <= 0) return;
    char *s = out, *nl;
    while (nsg < 5 && (nl = strchr(s, '\n'))) { *nl = 0; if (*s) snprintf(sg[nsg++], 128, "%s", s); s = nl + 1; }
}
static void sg_redraw(uint16_t *dst, int pressed)
{
    le_page(dst);
    int y = le_fy + 52;
    for (int i = 0; i < nsg && y + 38 <= KB_Y - 4; i++, y += 40) {
        fill_rrect(dst, 12, y, W - 24, 36, 9, rgb(36, 36, 40), 255);
        draw_text(dst, 26, y + 24, sg[i], 15, 0xFFFF, W - 52, 0, H);
    }
    kb_draw(dst, pressed);
}
static int line_edit_suggest(const char *title, const char *hint, char *buf, int cap, const char *field)
{
    struct ebuf e = { buf, (int)strlen(buf), cap, (int)strlen(buf) };
    le_title = title; le_hint = hint; le_buf = &e; sg_field = field; nsg = 0;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Aceptar"));
    char last[128] = "\x01"; double typed = 0;
    kb_begin_ex(&e, fr0, sg_redraw, le_fy, le_fy + 44);
    struct pkt p;
    for (;;) {
        /* sugerencias tras una pausa al escribir (no en cada tecla) */
        if (field && strcmp(last, buf) && now_ms() - typed > 140) { snprintf(last, sizeof last, "%s", buf); sg_fetch(buf); sg_redraw(fr0, -1); present(); }
        if (!read_pkt(&p, 60) || !p.down) continue;
        kb_map(&p);
        if (p.y >= KB_Y) { int r = kb_touch(&p, &e, fr0, sg_redraw, 0); typed = now_ms(); if (r == K_ENTER) { kb_end(); return 1; } continue; }
        int x = p.x, y = p.y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 90 && x < 100) { kb_end(); return 0; }
        if (y >= le_fy + 52 && nsg) {
            int i = (y - le_fy - 52) / 40;
            if (i >= 0 && i < nsg) { snprintf(buf, cap, "%s", sg[i]); kb_end(); return 1; }
        }
        if (y >= le_fy && y < le_fy + 44) { e.cur = e.len; if (kb_off) kb_reveal(fr0, sg_redraw); else { sg_redraw(fr0, -1); present(); } }
    }
}

/* tarjeta de progreso: barra que avanza con un brillo que la recorre */
static double te_last_draw;
static uint16_t pc_base[W * H]; static int pc_have;   /* fondo de la tarjeta de progreso */
static void progress_card(const char *title, const char *sub, double frac)
{
    uint16_t *base = pc_base;
    if (frac < 0) { memcpy(base, fr0, sizeof pc_base); dim_buf(base, base, W * H, 14); pc_have = 1; return; }
    int have = pc_have;
    if (!have) return;
    memcpy(fr0, base, sizeof pc_base);
    int cw = 250, ch = 120, cx = (W - cw) / 2, cy = (H - ch) / 2;
    fill_rrect(fr0, cx, cy, cw, ch, 16, rgb(38, 38, 42), 245);
    int tw = tw_n(title, 16, -1); draw_text(fr0, cx + (cw - tw) / 2, cy + 36, title, 16, 0xFFFF, cw - 20, 0, H);
    if (sub) { int sw = tw_n(sub, 12, -1); draw_text(fr0, cx + (cw - sw) / 2, cy + 98, sub, 12, rgb(152, 152, 159), cw - 20, 0, H); }
    int bx = cx + 24, by = cy + 62, bw = cw - 48;
    fill_rrect(fr0, bx, by, bw, 8, 4, rgb(70, 70, 76), 255);
    if (frac > 1) frac = 1;
    int fw = (int)(bw * frac); if (fw < 8) fw = 8;
    fill_rrect(fr0, bx, by, fw, 8, 4, rgb(10, 132, 255), 255);
    int gx = bx + (int)fmod(now_ms() / 4.0, bw + 60) - 30;            /* brillo */
    for (int x = gx; x < gx + 30; x++) if (x >= bx && x < bx + fw) for (int y = by + 1; y < by + 7; y++) fr0[y * W + x] = mix565(rgb(160, 205, 255), fr0[y * W + x], (unsigned)(120 * (1 - fabs((x - gx - 15) / 15.0))));
    present();
    te_last_draw = now_ms();
}
static int te_batch_cb(void *ud, int idx, int n, uint64_t done, uint64_t total)
{
    (void)ud; st->shell_beat++;
    if (now_ms() - te_last_draw < 30 && idx < n) return 0;
    char sub[64]; snprintf(sub, sizeof sub, tr("%d de %d"), idx + 1 > n ? n : idx + 1, n);
    progress_card(tr("Guardando etiquetas"), sub, total ? (double)done / total : (double)idx / n);
    return 0;
}

/* ------------------------------------------------------------------ nombres legibles */
/* orden de "Mas etiquetas" = orden de esta tabla; despues las desconocidas (alfabetico),
 * MusicBrainz/AcoustID y por ultimo ReplayGain. ml = se edita con el editor de varias lineas */
static const struct { const char *key, *name; int ml; } TE_KNOWN[] = {
    { "TRACKTOTAL", N_("Total de pistas"), 0 }, { "DISCTOTAL", N_("Total de discos"), 0 },
    { "COMPOSER", N_("Compositor"), 0 }, { "CONDUCTOR", N_("Director"), 0 }, { "LYRICIST", N_("Letrista"), 0 },
    { "REMIXER", N_("Remezclador"), 0 }, { "COMMENT", N_("Comentario"), 1 }, { "DESCRIPTION", N_("Descripción"), 1 },
    { "LYRICS", N_("Letra"), 1 }, { "LABEL", N_("Sello"), 0 }, { "ORGANIZATION", N_("Organización"), 0 },
    { "ISRC", "ISRC", 0 }, { "CATALOGNUMBER", N_("Nº de catálogo"), 0 }, { "BARCODE", N_("Código de barras"), 0 },
    { "BPM", "BPM", 0 }, { "KEY", N_("Tonalidad"), 0 }, { "MOOD", N_("Estado de ánimo"), 0 },
    { "GROUPING", N_("Agrupación"), 0 }, { "WORK", N_("Obra"), 0 }, { "SUBTITLE", N_("Subtítulo"), 0 },
    { "DISCSUBTITLE", N_("Subtítulo del disco"), 0 }, { "COPYRIGHT", "Copyright", 0 }, { "LANGUAGE", N_("Idioma"), 0 },
    { "MEDIA", N_("Soporte"), 0 }, { "ORIGINALDATE", N_("Fecha original"), 0 }, { "RELEASEDATE", N_("Fecha de publicación"), 0 },
    { "ORIGINALARTIST", N_("Artista original"), 0 }, { "ORIGINALALBUM", N_("Álbum original"), 0 },
    { "COMPILATION", N_("Recopilatorio"), 0 }, { "ENCODEDBY", N_("Codificado por"), 0 }, { "ENCODER", N_("Codificador"), 0 },
    { "ARTISTSORT", N_("Artista (orden)"), 0 }, { "ALBUMARTISTSORT", N_("Artista del álbum (orden)"), 0 },
    { "ALBUMSORT", N_("Álbum (orden)"), 0 }, { "TITLESORT", N_("Título (orden)"), 0 }, { "COMPOSERSORT", N_("Compositor (orden)"), 0 } };
#define TE_NKNOWN ((int)(sizeof TE_KNOWN / sizeof *TE_KNOWN))
/* campos de "Anadir campo" */
static const char *const TE_ADDABLE[] = { "COMPOSER", "CONDUCTOR", "LYRICIST", "COMMENT", "LYRICS", "LABEL", "ISRC",
                                         "CATALOGNUMBER", "TRACKTOTAL", "DISCTOTAL", "BPM", "GROUPING", "MOOD", "COPYRIGHT", "DESCRIPTION" };
#define TE_NADD ((int)(sizeof TE_ADDABLE / sizeof *TE_ADDABLE))

static int te_known(const char *key) { for (int i = 0; i < TE_NKNOWN; i++) if (!strcmp(TE_KNOWN[i].key, key)) return i; return -1; }
static void te_name(const char *key, const char *orig, char *out, int cap)
{
    int b = ht_key_base(key), k = te_known(key);
    if (b >= 0) { snprintf(out, cap, "%s", tr(TE_FIELDS[b])); return; }
    if (k >= 0) { snprintf(out, cap, "%s", tr(TE_KNOWN[k].name)); return; }
    const char *c = strchr(orig, ':');
    if (c && (!strncmp(key, "COMMENT:", 8) || !strncmp(key, "LYRICS:", 7))) { snprintf(out, cap, "%s (%s)", tr(key[0] == 'C' ? "Comentario" : "Letra"), c + 1); return; }
    snprintf(out, cap, "%s", orig);
}
static int te_rank(const char *key)
{
    int k = te_known(key);
    if (k >= 0) return k;
    if (!strncmp(key, "REPLAYGAIN_", 11)) return 3000;
    if (!strncmp(key, "MUSICBRAINZ_", 12) || !strncmp(key, "ACOUSTID", 8) || !strncmp(key, "RELEASE", 7)) return 2000;
    return 1000;
}

/* ------------------------------------------------------------------ modelo */
#define TE_MAXF 160
#define TE_LOCKED(f, n) ((n) > 1 && ((f) == HT_TITLE || (f) == HT_TRACK))
struct te_f { char key[128], orig[128]; char *val, *nv; int cnt, multi, flags, dirty, added; };
static struct te_f *te_fs; static int te_nf, te_n, te_nm4a;
static char **te_paths;
/* renombrado pendiente: una cancion (te_fname) o el album (patron + carpeta) */
static char te_fname[256]; static int te_fname_on;
static int te_pat = -1, te_fold;                      /* te_pat -1 = sin cambiar; te_fold 0 no, 1 Album, 2 Artista/Album */
enum { TE_FOLD_NONE, TE_FOLD_ALBUM, TE_FOLD_ARTIST };

static int te_find(const char *key) { for (int i = 0; i < te_nf; i++) if (!strcmp(te_fs[i].key, key)) return i; return -1; }
static int te_add(const char *key, const char *orig)
{
    if (te_nf >= TE_MAXF) return -1;
    struct te_f *f = &te_fs[te_nf]; memset(f, 0, sizeof *f);
    snprintf(f->key, sizeof f->key, "%s", key); snprintf(f->orig, sizeof f->orig, "%s", orig);
    return te_nf++;
}
static void te_free(void)
{
    for (int i = 0; i < te_nf; i++) { free(te_fs[i].val); free(te_fs[i].nv); }
    free(te_fs); te_fs = 0; te_nf = 0;
}
static const char *te_cur(const struct te_f *f) { return f->dirty ? f->nv : f->val ? f->val : ""; }
static int te_multiline(const struct te_f *f)
{
    int k = te_known(f->key);
    if (k >= 0 && TE_KNOWN[k].ml) return 1;
    if (!strncmp(f->key, "COMMENT:", 8) || !strncmp(f->key, "LYRICS:", 7)) return 1;
    const char *v = te_cur(f);
    return strchr(v, '\n') || strlen(v) > 120;
}
static int te_any_dirty(void)
{
    for (int i = 0; i < te_nf; i++) if (te_fs[i].dirty) return 1;
    return te_fname_on || te_pat >= 0 || te_fold;
}
static int te_cmp_more(const void *a, const void *b)
{
    const struct te_f *x = a, *y = b;
    int rx = te_rank(x->key), ry = te_rank(y->key);
    return rx != ry ? rx - ry : strcmp(x->key, y->key);
}
static void te_load(char **paths, int n)
{
    te_free();
    te_fs = calloc(TE_MAXF, sizeof *te_fs); te_nf = 0; te_n = n; te_nm4a = 0; te_paths = paths;
    te_fname_on = 0; te_pat = -1; te_fold = 0;
    if (!te_fs) return;
    for (int f = 0; f < HT_NFIELDS; f++) { static const char *const k[HT_NFIELDS] = { "TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "GENRE", "DATE", "TRACKNUMBER", "DISCNUMBER" }; te_add(k[f], k[f]); }
    /* con muchos ficheros solo se leen los primeros 60 (valores comunes) */
    progress_card(0, 0, -1);
    int nr = n < 60 ? n : 60;
    for (int i = 0; i < nr; i++) {
        ht_all a; ht_read_all(paths[i], &a);
        if (a.fmt == HT_FMT_M4A) te_nm4a++;
        for (int k = 0; k < a.n; k++) {
            int j = te_find(a.it[k].key);
            if (j < 0) j = te_add(a.it[k].key, a.it[k].orig);
            if (j < 0) continue;
            struct te_f *f = &te_fs[j];
            f->flags |= a.it[k].flags;
            if (!f->cnt) { f->val = strdup(a.it[k].val); if (i) f->multi = 1; }
            else if (!f->multi && strcmp(f->val, a.it[k].val)) f->multi = 1;
            f->cnt++;
        }
        ht_all_free(&a);
        st->shell_beat++;
        if (n > 8) { char sub[48]; snprintf(sub, sizeof sub, tr("%d de %d"), i + 1, nr); progress_card(tr("Leyendo etiquetas"), sub, (double)(i + 1) / nr); }
    }
    for (int i = 0; i < te_nf; i++) {
        if (te_fs[i].cnt && te_fs[i].cnt < nr) te_fs[i].multi = 1;
        if (!te_fs[i].val) te_fs[i].val = strdup("");
    }
    if (te_nf > HT_NFIELDS + 1) qsort(te_fs + HT_NFIELDS, te_nf - HT_NFIELDS, sizeof *te_fs, te_cmp_more);
    if (n > 1) te_nm4a = te_nm4a * n / nr;                  /* estimacion si no se leyeron todos */
}
/* nuevo valor del campo i (o vuelve al original si es igual) */
static void te_set(int i, const char *v)
{
    struct te_f *f = &te_fs[i];
    if (f->added && !*v) {                                     /* campo anadido y vaciado: fuera */
        free(f->val); free(f->nv);
        memmove(f, f + 1, (te_nf - i - 1) * sizeof *f); te_nf--;
        return;
    }
    free(f->nv); f->nv = 0; f->dirty = 0;
    if (f->multi || f->added || strcmp(v, f->val ? f->val : "")) { f->nv = strdup(v); f->dirty = f->nv != 0; }
}

/* ------------------------------------------------------------------ dibujo */
#define TE_TOP 94
#define C_BLUE rgb(10, 132, 255)
#define C_GRAY rgb(142, 142, 147)
#define C_CARD rgb(28, 28, 30)
#define C_SEP rgb(56, 56, 58)
#define C_PRESS rgb(58, 58, 60)
/* caja con esquinas redondeadas solo arriba (top) y/o abajo (bot), recortada a [cy0, cy1) */
static void te_rbox(uint16_t *d, int x0, int y0, int w, int h, int r, int top, int bot, uint16_t col, int cy0, int cy1)
{
    int ya = y0 > cy0 ? y0 : cy0, yb = y0 + h < cy1 ? y0 + h : cy1;
    if (ya < 0) ya = 0; if (yb > H) yb = H;
    for (int y = ya; y < yb; y++) {
        uint16_t *row = d + y * W;
        float dy = 0;
        if (top && y < y0 + r) dy = (y0 + r) - (y + .5f);
        else if (bot && y >= y0 + h - r) dy = (y + .5f) - (y0 + h - r);
        if (dy <= 0) { for (int x = x0; x < x0 + w; x++) row[x] = col; continue; }
        for (int x = x0; x < x0 + w; x++) {
            float dx = x < x0 + r ? (x0 + r) - (x + .5f) : x >= x0 + w - r ? (x + .5f) - (x0 + w - r) : 0;
            if (dx <= 0) { row[x] = col; continue; }
            float cov = r + .5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0) continue;
            if (cov >= 1) row[x] = col; else row[x] = mix565(col, row[x], (unsigned)(cov * 255));
        }
    }
}
static void te_chev(uint16_t *d, int x, int cy, uint16_t col, int y0, int y1)
{
    for (int i = 0; i < 6; i++) for (int t = 0; t < 2; t++) {
        int ya = cy - 6 + i, yb = cy + 6 - i, xx = x + i + t;
        if (ya >= y0 && ya < y1) d[ya * W + xx] = col;
        if (yb >= y0 && yb < y1) d[yb * W + xx] = col;
    }
}
static void te_check(uint16_t *d, int x, int cy, uint16_t col, int y0, int y1)
{
    for (int i = 0; i < 5; i++) for (int t = 0; t < 2; t++) { int y = cy + i - t; if (y >= y0 && y < y1) d[y * W + x + i] = col; }
    for (int i = 0; i < 10; i++) for (int t = 0; t < 2; t++) { int y = cy + 4 - i - t; if (y >= y0 && y < y1) d[y * W + x + 4 + i] = col; }
}
static void te_nav(uint16_t *d, const char *left, const char *title, const char *right, int right_on, int sep)
{
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < TE_TOP; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(12, 12, 13);
    if (sep) for (int x = 0; x < W; x++) d[(TE_TOP - 1) * W + x] = rgb(38, 38, 40);
    /* el titulo se centra en el hueco que dejan los dos botones (idiomas con textos largos) */
    int lw = left ? tw_n(left, 16, -1) : 0, rw = right ? tw_n(right, 16, -1) : 0;
    if (lw > 100) lw = 100; if (rw > 100) rw = 100;
    int side = (lw > rw ? lw : rw) + 14 + 8, tmax = W - 2 * side;
    if (tmax < 96) tmax = 96;
    if (left) draw_text(d, 14, 68, left, 16, C_BLUE, 100, 0, H);
    int tw = tw_n(title, 17, -1); if (tw > tmax) tw = tmax;
    draw_text(d, (W - tw) / 2, 68, title, 17, 0xFFFF, tmax, 0, H);
    if (right) draw_text(d, W - 14 - rw, 68, right, 16, right_on ? C_BLUE : rgb(70, 70, 76), 100, 0, H);
}

/* ------------------------------------------------------------------ pantalla con lista que se desplaza */
enum { R_GAP, R_CAPTION, R_FIELD, R_BTNS, R_HDR, R_ADD, R_FILE, R_RENAME, R_NOTE, R_OPT, R_PREV, R_PICK };
struct te_row { unsigned char type, grp, sel, bad; short idx; int y, h; const char *txt, *sub; };
struct te_scr {
    const char *left, *title, *right; int right_on;
    struct te_row *r; int n, cap, ch; float sc;
    void (*draw)(uint16_t *d, const struct te_row *r, int sy, int pressed);
};
static struct te_row *te_row_add(struct te_scr *s, int type, int grp, int h)
{
    if (s->n >= s->cap) { int nc = s->cap ? s->cap * 2 : 64; struct te_row *q = realloc(s->r, nc * sizeof *q); if (!q) return 0; s->r = q; s->cap = nc; }
    struct te_row *r = &s->r[s->n++]; memset(r, 0, sizeof *r);
    r->type = (unsigned char)type; r->grp = (unsigned char)grp; r->h = h; r->y = s->ch; r->idx = -1;
    s->ch += h;
    return r;
}
static int te_tappable(int t) { return t == R_FIELD || t == R_ADD || t == R_FILE || t == R_RENAME || t == R_OPT || t == R_PICK || t == R_BTNS; }
static float te_maxsc(const struct te_scr *s) { float m = s->ch - (H - TE_TOP); return m > 0 ? m : 0; }
static void te_render(const struct te_scr *s, int pressed)
{
    uint16_t *d = fr0;
    for (int i = TE_TOP * W; i < W * H; i++) d[i] = 0;
    int off = TE_TOP - (int)lroundf(s->sc);
    for (int i = 0; i < s->n; i++) {
        const struct te_row *r = &s->r[i];
        int sy = off + r->y;
        if (sy + r->h <= TE_TOP || sy >= H) continue;
        if (r->grp) {
            int first = !i || s->r[i - 1].grp != r->grp, last = i == s->n - 1 || s->r[i + 1].grp != r->grp;
            te_rbox(d, 16, sy, W - 32, r->h, 10, first, last, i == pressed && r->type != R_BTNS ? C_PRESS : C_CARD, TE_TOP, H);
            if (!first) for (int x = 32; x < W - 16; x++) if (sy >= TE_TOP && sy < H) d[sy * W + x] = C_SEP;
        }
        s->draw(d, r, sy, i == pressed);
    }
    te_nav(d, s->left, s->title, s->right, s->right_on, s->sc > 1);
    draw_toast(d);
}
static void te_paint(const struct te_scr *s, int pressed) { te_render(s, pressed); present(); }
static int te_row_at(const struct te_scr *s, int y)
{
    int cy = y - TE_TOP + (int)lroundf(s->sc);
    for (int i = 0; i < s->n; i++) if (cy >= s->r[i].y && cy < s->r[i].y + s->r[i].h) return i;
    return -1;
}
/* transicion de pila (dir > 0 entra la nueva por la derecha) */
static void te_slide(const uint16_t *from, const uint16_t *to, int dir)
{
    double t0 = now_ms(); int dur = 300;
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        float e = (float)ease_out(t);
        int o = (int)(W * e);
        const uint16_t *top = dir > 0 ? to : from, *bot = dir > 0 ? from : to;
        int tx = dir > 0 ? W - o : o, bx = dir > 0 ? -o / 3 : -(W - o) / 3;
        int k = dir > 0 ? 32 - (int)(10 * e) : 22 + (int)(10 * e);
        memcpy(fr0, to, 30 * W * 2);
        for (int y = 30; y < H; y++) {
            uint16_t *dr = fr0 + y * W; const uint16_t *tr0 = top + y * W, *br = bot + y * W;
            for (int x = 0; x < W; x++) {
                if (x >= tx) dr[x] = tr0[x - tx];
                else { int sx = x - bx; dr[x] = sx >= 0 && sx < W ? dim565(br[sx], k) : 0; }
            }
        }
        present();
        if (t >= 1) break;
    }
}
/* entra/sale una pantalla: from = lo que hay ahora en fr0, to = s ya construida */
static void te_push(const struct te_scr *s, int dir)
{
    uint16_t *a = malloc(W * H * 2), *b = malloc(W * H * 2);
    if (a && b) { memcpy(a, fr0, W * H * 2); te_render(s, -1); memcpy(b, fr0, W * H * 2); te_slide(a, b, dir); }
    free(a); free(b);
    te_paint(s, -1);
}
enum { EV_LEFT = 1, EV_RIGHT, EV_ROW, EV_LONG };
/* bucle de una pantalla: desplazamiento 1:1 con inercia y rebote, fila pulsada, pulsacion larga,
 * gesto de atras desde el borde y deslizar desde abajo (= atras) */
static int te_loop(struct te_scr *s, int *row, int *tx)
{
    float vel = 0; double lt = 0;
    float mx = te_maxsc(s); if (s->sc > mx) s->sc = mx; if (s->sc < 0) s->sc = 0;
    te_paint(s, -1);
    struct pkt p;
    for (;;) {
        if (fabsf(vel) > 0.02f) {                              /* inercia */
            double t = now_ms(); if (!lt || t - lt > 100) lt = t - 16;
            float dt = (float)(t - lt); lt = t;
            s->sc -= vel * dt; vel *= powf(0.9972f, dt);
            if (s->sc < 0) { s->sc = 0; vel = 0; } if (s->sc > mx) { s->sc = mx; vel = 0; }
            te_paint(s, -1);
            if (!read_pkt(&p, 0)) continue;
            vel = 0;
            if (!p.down) continue;
        } else if (!read_pkt(&p, now_ms() < toast_until + 40 ? 30 : 400)) { if (now_ms() < toast_until + 40) te_paint(s, -1); continue; }
        if (!p.down) continue;
        int sx = p.x, sy = p.y;
        if (sy >= 456) {                                        /* deslizar desde abajo: cerrar */
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            while (cur_down && read_pkt(&p, 1000)) {}
            if (up) return EV_LEFT;
            continue;
        }
        if (back_zone(sx, sy)) { int bk = app_back_gesture(sx, sy); if (bk > 0) return EV_LEFT; if (bk < 0) { te_paint(s, -1); continue; } }
        if (sy < TE_TOP) {
            while (cur_down && read_pkt(&p, 3000)) {}
            if (sx < 100) return EV_LEFT;
            if (sx > W - 100 && s->right && s->right_on) return EV_RIGHT;
            continue;
        }
        int r = te_row_at(s, sy);
        int pr = r >= 0 && te_tappable(s->r[r].type) && s->r[r].type != R_BTNS ? r : -1;
        if (pr >= 0) te_paint(s, pr);
        int moved = 0, ly = sy; float s0 = s->sc; double t0 = now_ms(); lt = t0; vel = 0;
        while (cur_down) {
            read_pkt(&p, 30);
            if (!moved && abs(cur_y - sy) > 8) { moved = 1; if (pr >= 0) pr = -1; }
            if (moved) {
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                float raw = s0 - (cur_y - sy);
                s->sc = raw < 0 ? raw / 3 : raw > mx ? mx + (raw - mx) / 3 : raw;     /* rebote al pasarse */
                te_paint(s, -1);
            } else if (r >= 0 && s->r[r].type == R_FIELD && now_ms() - t0 > 480) {
                *row = r; *tx = cur_x;                         /* pulsacion larga */
                te_paint(s, -1);
                return EV_LONG;
            }
        }
        if (moved) {
            if (now_ms() - lt > 60) vel = 0;
            if (s->sc < 0 || s->sc > mx) {                     /* vuelve a su sitio */
                float a = s->sc, b = s->sc < 0 ? 0 : mx; double tb = now_ms();
                for (;;) { double t = (now_ms() - tb) / 260; if (t > 1) t = 1; s->sc = a + (b - a) * (float)ease_out(t); te_paint(s, -1); if (t >= 1) break; }
                vel = 0;
            }
            lt = 0;
            continue;
        }
        if (r < 0 || !te_tappable(s->r[r].type)) { te_paint(s, -1); continue; }
        *row = r; *tx = sx;
        return EV_ROW;
    }
}

/* ------------------------------------------------------------------ editor de varias lineas */
#define TT_LINES 4096
static struct ebuf tt; static int tt_top, tt_ln[TT_LINES], tt_nl, tt_sc; static const char *tt_title;
static void tt_layout(int maxw)
{
    tt_nl = 0; int i = 0;
    while (i <= tt.len && tt_nl < TT_LINES) {
        tt_ln[tt_nl++] = i;
        int w = 0, last_sp = -1, j = i;
        while (j < tt.len && tt.b[j] != '\n') {
            const char *s = tt.b + j; int c = utf8_next(&s);
            struct glyph *g = glyph_get(c, 15); int gw = g ? g->adv : 7 << 6;   /* 26.6, como draw_text */
            if ((w + gw) >> 6 > maxw) break;
            if (c == ' ') last_sp = (int)(s - tt.b);
            w += gw; j = (int)(s - tt.b);
        }
        if (j >= tt.len) break;
        if (tt.b[j] == '\n') i = j + 1;
        else i = last_sp > i ? last_sp : j;
    }
}
static int tt_line_of(int pos) { int l = 0; for (int k = 0; k < tt_nl; k++) if (tt_ln[k] <= pos) l = k; else break; return l; }
#define TT_LH 21
static void tt_redraw(uint16_t *d, int pressed)
{
    int bottom = KB_Y < H ? KB_Y : H;
    for (int i = TE_TOP * W; i < W * H; i++) d[i] = 0;
    tt_layout(W - 28);
    int vis = (bottom - TE_TOP - 8) / TT_LH;
    if (tt_sc) tt_sc = 0;
    else { int cl = tt_line_of(tt.cur); if (cl < tt_top) tt_top = cl; if (cl >= tt_top + vis) tt_top = cl - vis + 1; }
    if (tt_top > tt_nl - 1) tt_top = tt_nl - 1; if (tt_top < 0) tt_top = 0;
    char line[1024];
    for (int l = tt_top; l < tt_nl && l <= tt_top + vis; l++) {
        int a = tt_ln[l], b = l + 1 < tt_nl ? tt_ln[l + 1] : tt.len;
        int n = b - a; if (n > (int)sizeof line - 1) n = sizeof line - 1;
        memcpy(line, tt.b + a, n); line[n] = 0;
        for (int k = 0; k < n; k++) if (line[k] == '\n' || line[k] == '\r' || line[k] == '\t') line[k] = line[k] == '\t' ? ' ' : 0;
        int base = TE_TOP + 8 + (l - tt_top) * TT_LH + 15;
        draw_text(d, 14, base, line, 15, rgb(235, 235, 240), 0, TE_TOP, bottom);
        if (tt.cur >= a && (tt.cur < b || l == tt_nl - 1)) {
            int cx = 14 + tw_n(tt.b + a, 15, tt.cur - a);
            for (int y = base - 15; y < base + 4; y++) if (y >= TE_TOP && y < bottom) for (int x = cx; x < cx + 2 && x < W; x++) d[y * W + x] = C_BLUE;
            kb_caret(cx, base - 15, base + 4);
        }
    }
    te_nav(d, tr("Cancelar"), tt_title, tr("Aceptar"), 1, 1);
    kb_draw(d, pressed);
}
/* trackpad del teclado: linea visual arriba/abajo con la misma x */
static int tt_vmove(int cur, int dir)
{
    int l = tt_line_of(cur), nl = l + dir;
    if (nl < 0 || nl >= tt_nl) return cur;
    int x = tw_n(tt.b + tt_ln[l], 15, cur - tt_ln[l]);
    int a = tt_ln[nl], b = nl + 1 < tt_nl ? tt_ln[nl + 1] : tt.len, best = a, bd = 1 << 30;
    for (int i = a; i <= b; i++) {
        if (i < tt.len && ((unsigned char)tt.b[i] & 0xC0) == 0x80) continue;
        int dd = abs(tw_n(tt.b + a, 15, i - a) - x); if (dd < bd) { bd = dd; best = i; }
    }
    if (best > a && best == b && b < tt.len && tt.b[b - 1] == '\n') best = b - 1;
    return best;
}
/* 1 si se acepto (texto en buf) */
static int te_text_edit(const char *title, char *buf, int cap)
{
    /* \r\n -> \n */
    int o = 0; for (int i = 0; buf[i]; i++) if (!(buf[i] == '\r' && buf[i + 1] == '\n')) buf[o++] = buf[i] == '\r' ? '\n' : buf[i];
    buf[o] = 0;
    char *orig = strdup(buf);
    tt = (struct ebuf){ buf, o, cap, o }; tt_title = title; tt_top = 0; tt_sc = 0;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("intro"));
    kb_scroll_area(TE_TOP); kb_set_vmove(tt_vmove);   /* el texto sigue al cursor (se desplaza solo) */
    kb_begin(&tt, fr0, tt_redraw);
    struct pkt p; int r = 0;
    for (;;) {
        if (!read_pkt(&p, 400) || !p.down) continue;
        kb_map(&p);
        if (p.y >= KB_Y) { kb_touch(&p, &tt, fr0, tt_redraw, 1); continue; }
        int sx = p.x, sy = p.y, top0 = tt_top, moved = 0;
        while (cur_down && read_pkt(&p, 3000)) {   /* hasta soltar: una pausa del dedo no es un toque */
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved && sy >= TE_TOP) {
                int nt = top0 - (cur_y - sy) / TT_LH; if (nt < 0) nt = 0; if (nt > tt_nl - 1) nt = tt_nl - 1;
                if (nt != tt_top) { tt_top = nt; tt_sc = 1; tt_redraw(fr0, -1); present(); }
            }
        }
        if (moved) continue;
        if (sy < TE_TOP && sx < 100) {
            if (strcmp(orig, buf) && !confirm(tr("Hay cambios sin guardar"), tr("Descartar cambios"))) { tt_redraw(fr0, -1); present(); continue; }
            r = 0; break;
        }
        if (sy < TE_TOP && sx > W - 100) { r = 1; break; }
        if (sy >= TE_TOP) {                                     /* colocar el cursor */
            int l = tt_top + (sy - TE_TOP - 8) / TT_LH; if (l >= tt_nl) l = tt_nl - 1; if (l < 0) l = 0;
            int a = tt_ln[l], b = l + 1 < tt_nl ? tt_ln[l + 1] : tt.len, best = a, bd = 1 << 30;
            for (int i = a; i <= b; i++) {
                if (i < tt.len && ((unsigned char)tt.b[i] & 0xC0) == 0x80) continue;
                int d = abs(14 + tw_n(tt.b + a, 15, i - a) - sx); if (d < bd) { bd = d; best = i; }
            }
            if (best > a && best == b && b < tt.len && tt.b[b - 1] == '\n') best = b - 1;
            tt.cur = best; tt_sc = 1;
            if (kb_off) kb_reveal(fr0, tt_redraw); else { tt_redraw(fr0, -1); present(); }
        }
    }
    kb_end();
    if (!r) snprintf(buf, cap, "%s", orig ? orig : "");
    free(orig);
    return r;
}

/* ------------------------------------------------------------------ formulario principal */
static char te_cap[64];
static void te_draw_main(uint16_t *d, const struct te_row *r, int sy, int pressed)
{
    (void)pressed;
    int cy = sy + r->h / 2 + 5;
    switch (r->type) {
    case R_CAPTION: draw_text(d, 32, sy + 20, r->txt, 12, C_GRAY, W - 64, TE_TOP, H); break;
    case R_HDR: draw_text(d, 32, sy + r->h - 9, r->txt, 12, C_GRAY, W - 64, TE_TOP, H); break;
    case R_NOTE: draw_wrapped(d, 32, sy + 4, W - 64, r->txt, 12, C_GRAY, 16, 1); break;
    case R_FIELD: {
        const struct te_f *f = &te_fs[r->idx];
        int b = ht_key_base(f->key), locked = b >= 0 && TE_LOCKED(b, te_n), ro = f->flags & (HT_ITEM_RO | HT_ITEM_TRUNC);
        char nm[160]; te_name(f->key, f->orig, nm, sizeof nm);
        int lw = tw_n(nm, 13, -1); if (lw > 136) lw = 136;
        int vx = 32 + lw + 10 > 150 ? 32 + lw + 10 : 150;         /* etiquetas largas empujan el valor */
        draw_text(d, 32, cy, nm, 13, locked || ro ? rgb(90, 90, 96) : rgb(152, 152, 159), 136, TE_TOP, H);
        const char *v = f->dirty ? f->nv : f->multi ? tr("Varios valores") : f->val;
        uint16_t vc = f->dirty ? C_BLUE : f->multi ? rgb(110, 110, 116) : ro ? rgb(150, 150, 156) : 0xFFFF;
        if (locked) { v = tr("Distinto en cada una"); vc = rgb(90, 90, 96); }
        if (f->dirty && !*v) { v = tr("(se borrará)"); vc = rgb(255, 69, 58); }
        char one[200]; int k = 0;
        while (v[k] && v[k] != '\n' && v[k] != '\r' && k < (int)sizeof one - 4) { one[k] = v[k]; k++; }
        one[k] = 0;
        if (v[k] && k < (int)sizeof one - 4) strcat(one, "…");
        int ml = te_multiline(f) && !locked;
        draw_text(d, vx, cy, one, 14, vc, W - 16 - 12 - vx - (ml ? 12 : 0), TE_TOP, H);
        if (ml) te_chev(d, W - 30, sy + r->h / 2, rgb(90, 90, 96), TE_TOP, H);
        break;
    }
    case R_BTNS: {
        int by = sy + 6, bw = (W - 32 - 10) / 2;
        te_rbox(d, 16, by, bw, 36, 12, 1, 1, rgb(44, 44, 48), TE_TOP, H);
        int w1 = tw_n(tr("Buscar carátula"), 14, -1); draw_text(d, 16 + (bw - w1) / 2, by + 24, tr("Buscar carátula"), 14, C_BLUE, bw - 8, TE_TOP, H);
        te_rbox(d, 26 + bw, by, bw, 36, 12, 1, 1, rgb(44, 44, 48), TE_TOP, H);
        int w2 = tw_n(tr("Buscar letra"), 14, -1); draw_text(d, 26 + bw + (bw - w2) / 2, by + 24, tr("Buscar letra"), 14, te_n == 1 ? C_BLUE : rgb(70, 70, 76), bw - 8, TE_TOP, H);
        break;
    }
    case R_ADD: {
        int cx = 40, ccy = sy + r->h / 2;
        te_rbox(d, cx - 10, ccy - 10, 20, 20, 10, 1, 1, rgb(48, 209, 88), TE_TOP, H);
        for (int t = -5; t <= 5; t++) for (int w = 0; w < 2; w++) {
            int y1 = ccy + t, y2 = ccy - 1 + w;
            if (y1 >= TE_TOP && y1 < H) d[y1 * W + cx - 1 + w] = 0xFFFF;
            if (y2 >= TE_TOP && y2 < H) d[y2 * W + cx + t] = 0xFFFF;
        }
        draw_text(d, 60, cy, tr("Añadir campo"), 15, C_BLUE, W - 100, TE_TOP, H);
        break;
    }
    case R_FILE: {
        draw_text(d, 32, sy + 19, tr("Nombre del archivo"), 12, rgb(152, 152, 159), W - 64, TE_TOP, H);   /* dos lineas: los nombres son largos */
        const char *b = strrchr(te_paths[0], '/'); b = b ? b + 1 : te_paths[0];
        draw_text(d, 32, sy + 40, te_fname_on ? te_fname : b, 15, te_fname_on ? C_BLUE : 0xFFFF, W - 64, TE_TOP, H);
        break;
    }
    case R_RENAME: {
        draw_text(d, 32, cy, tr("Renombrar archivos"), 15, 0xFFFF, W - 120, TE_TOP, H);
        if (r->sub) { int w = tw_n(r->sub, 13, -1); if (w > 100) w = 100; draw_text(d, W - 40 - w, cy, r->sub, 13, C_BLUE, 100, TE_TOP, H); }
        te_chev(d, W - 30, sy + r->h / 2, rgb(90, 90, 96), TE_TOP, H);
        break;
    }
    }
}
static void te_build_main(struct te_scr *s)
{
    s->n = 0; s->ch = 0;
    s->left = tr("Cancelar"); s->title = te_n > 1 ? tr("Editar canciones") : tr("Editar canción");
    int tags = 0; for (int i = 0; i < te_nf; i++) tags |= te_fs[i].dirty;
    s->right = tr("Guardar"); s->draw = te_draw_main;
    s->right_on = (tags && te_nm4a < te_n) || te_fname_on || te_pat >= 0 || te_fold;   /* M4A: renombrar si, etiquetas no */
    struct te_row *r;
    if (te_n > 1) { snprintf(te_cap, sizeof te_cap, tr("%d canciones"), te_n); r = te_row_add(s, R_CAPTION, 0, 28); if (r) r->txt = te_cap; }
    else te_row_add(s, R_GAP, 0, 22);
    for (int i = 0; i < HT_NFIELDS && i < te_nf; i++) { r = te_row_add(s, R_FIELD, 1, 40); if (r) r->idx = (short)i; }
    te_row_add(s, R_BTNS, 0, 52);
    r = te_row_add(s, R_HDR, 0, 34); if (r) r->txt = tr("Más etiquetas");
    for (int i = HT_NFIELDS; i < te_nf; i++) { r = te_row_add(s, R_FIELD, 2, 40); if (r) r->idx = (short)i; }
    if (te_nm4a < te_n) te_row_add(s, R_ADD, 2, 40);
    r = te_row_add(s, R_HDR, 0, 34); if (r) r->txt = tr("Archivo");
    if (te_n == 1) te_row_add(s, R_FILE, 3, 54);
    else {
        r = te_row_add(s, R_RENAME, 3, 40);
        static char sum[64];
        if (r && (te_pat >= 0 || te_fold)) { snprintf(sum, sizeof sum, "%s", te_pat >= 0 && te_fold ? tr("Nombres y carpeta") : te_pat >= 0 ? tr("Nombres") : tr("Carpeta")); r->sub = sum; }
    }
    const char *note = te_nm4a >= te_n ? tr("TAGS_M4A_RO") : te_nm4a ? tr("TAGS_M4A_SOME") : 0;
    if (note) { int h = draw_wrapped(fr0, 32, 0, W - 64, note, 12, C_GRAY, 16, 0); r = te_row_add(s, R_NOTE, 0, h + 14); if (r) r->txt = note; }
    te_row_add(s, R_GAP, 0, 24);
}

/* edita el campo i */
static void te_edit_field(int i)
{
    struct te_f *f = &te_fs[i];
    int b = ht_key_base(f->key);
    if (b >= 0 && TE_LOCKED(b, te_n)) { toast(tr("Se cambia canción por canción")); return; }
    if (f->flags & HT_ITEM_TRUNC) { toast(tr("Demasiado largo para editarlo aquí")); return; }
    if ((f->flags & HT_ITEM_RO) || te_nm4a >= te_n) { toast(tr("Este campo es de solo lectura")); return; }
    char nm[160]; te_name(f->key, f->orig, nm, sizeof nm);
    if (te_multiline(f)) {
        static char big[HT_LONGMAX + 8];
        snprintf(big, sizeof big, "%s", f->dirty ? f->nv : f->multi ? "" : f->val);
        if (te_text_edit(nm, big, sizeof big)) te_set(i, big);
        return;
    }
    char buf[512]; snprintf(buf, sizeof buf, "%s", f->dirty ? f->nv : f->multi ? "" : f->val);
    if (line_edit_suggest(nm, f->multi ? tr("Varios valores") : "", buf, sizeof buf, b >= 0 ? TE_SUGG[b] : 0)) te_set(i, buf);
}

/* ------------------------------------------------------------------ anadir campo */
static const char *te_pick_key[TE_NADD + 1];
static void te_draw_pick(uint16_t *d, const struct te_row *r, int sy, int pressed)
{
    (void)pressed;
    int cy = sy + r->h / 2 + 5;
    if (r->type == R_HDR) { draw_text(d, 32, sy + r->h - 9, r->txt, 12, C_GRAY, W - 64, TE_TOP, H); return; }
    if (r->type != R_PICK) return;
    draw_text(d, 32, cy, r->txt, 15, r->idx < 0 ? C_BLUE : 0xFFFF, W - 80, TE_TOP, H);
    te_chev(d, W - 30, sy + r->h / 2, rgb(90, 90, 96), TE_TOP, H);
}
static void te_build_main(struct te_scr *s);
static void te_add_field(struct te_scr *main)
{
    struct te_scr s; memset(&s, 0, sizeof s);
    static char names[TE_NADD][96];
    s.left = tr("Cancelar"); s.title = tr("Añadir campo"); s.draw = te_draw_pick;
    struct te_row *r = te_row_add(&s, R_GAP, 0, 22);
    int np = 0;
    for (int k = 0; k < TE_NADD; k++) {
        if (te_find(TE_ADDABLE[k]) >= 0) continue;
        te_name(TE_ADDABLE[k], TE_ADDABLE[k], names[np], sizeof names[np]);
        te_pick_key[np] = TE_ADDABLE[k];
        r = te_row_add(&s, R_PICK, 1, 44); if (r) { r->txt = names[np]; r->idx = (short)np; }
        np++;
    }
    r = te_row_add(&s, R_PICK, 1, 44); if (r) { r->txt = tr("Campo personalizado…"); r->idx = -1; }
    te_row_add(&s, R_GAP, 0, 24);
    te_push(&s, 1);
    char key[128] = "";
    for (;;) {
        int row, x, ev = te_loop(&s, &row, &x);
        if (ev == EV_LEFT) break;
        if (ev != EV_ROW || s.r[row].type != R_PICK) continue;
        if (s.r[row].idx >= 0) { snprintf(key, sizeof key, "%s", te_pick_key[s.r[row].idx]); break; }
        char kb[100] = "";
        if (!line_edit_suggest(tr("Nombre del campo"), tr("p. ej. MOOD"), kb, sizeof kb, 0) || !kb[0]) { te_paint(&s, -1); continue; }
        char c[128]; ht_key_canon(kb, c, sizeof c);
        if (!ht_key_valid(kb) || ht_key_base(c) >= 0 || !strcmp(c, "METADATA_BLOCK_PICTURE")) { toast(tr("Nombre de campo no válido")); te_paint(&s, -1); continue; }
        if (te_find(c) >= 0) { toast(tr("Ese campo ya existe")); te_paint(&s, -1); continue; }
        snprintf(key, sizeof key, "%s", c);
        break;
    }
    free(s.r);
    int i = key[0] ? te_add(key, key) : -1;
    if (i >= 0) { te_fs[i].added = 1; te_fs[i].val = strdup(""); }
    { float sc = main->sc; te_build_main(main); main->sc = sc; }   /* vuelta al formulario (con la fila nueva) */
    te_push(main, -1);
    if (i < 0) return;
    te_edit_field(i);
    if (i < te_nf && !strcmp(te_fs[i].key, key) && !te_fs[i].dirty) {     /* cancelado: fuera */
        free(te_fs[i].val); memmove(&te_fs[i], &te_fs[i + 1], (te_nf - i - 1) * sizeof *te_fs); te_nf--;
    }
}

/* ------------------------------------------------------------------ nombre del archivo (una cancion) */
static void te_split(const char *path, char *dir, int dcap, char *stem, int scap, char *ext, int ecap)
{
    const char *sl = strrchr(path, '/'), *b = sl ? sl + 1 : path, *dot = strrchr(b, '.');
    snprintf(dir, dcap, "%.*s", sl ? (int)(sl - path) : 0, path);
    if (dot && dot > b) { snprintf(stem, scap, "%.*s", (int)(dot - b), b); snprintf(ext, ecap, "%s", dot); }
    else { snprintf(stem, scap, "%s", b); ext[0] = 0; }
}
/* 1 si a y b son el mismo fichero (FAT: solo cambian mayusculas) */
static int te_same(const char *a, const char *b)
{
    struct stat x, y;
    return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
static void te_edit_fname(void)
{
    char dir[600], stem[300], ext[32];
    te_split(te_paths[0], dir, sizeof dir, stem, sizeof stem, ext, sizeof ext);
    char buf[256];
    if (te_fname_on) { char d2[8], e2[32]; te_split(te_fname, d2, sizeof d2, buf, sizeof buf, e2, sizeof e2); }
    else snprintf(buf, sizeof buf, "%s", stem);
    if (!line_edit_suggest(tr("Nombre del archivo"), stem, buf, sizeof buf - (int)strlen(ext) - 1, 0)) return;
    char nn[300]; snprintf(nn, sizeof nn, "%s%s", buf, ext);
    if (!buf[0] || !ht_name_valid(nn)) { toast(tr("TAGS_BADNAME")); return; }
    const char *b = strrchr(te_paths[0], '/'); b = b ? b + 1 : te_paths[0];
    if (!strcmp(nn, b)) { te_fname_on = 0; return; }
    char full[900]; snprintf(full, sizeof full, "%s/%s", dir, nn);
    if (!access(full, F_OK) && !te_same(full, te_paths[0])) { toast(tr("Ya existe un archivo con ese nombre")); return; }
    snprintf(te_fname, sizeof te_fname, "%s", nn); te_fname_on = 1;
}

/* ------------------------------------------------------------------ renombrar el album */
struct te_rn { char *title, *track, *disc, *artist, *album, *aartist; char nn[256]; int change, bad; };
static struct te_rn *rn; static int rn_n, rn_samedir, rn_multidisc;
static char rn_dir[600], rn_target[700], rn_tshow[300]; static int rn_tbad;
static char *rn_dup(const ht_all *a, const char *k) { const ht_item *it = ht_all_find(a, k); return strdup(it ? it->val : ""); }
static void rn_free(void)
{
    for (int i = 0; i < rn_n; i++) { free(rn[i].title); free(rn[i].track); free(rn[i].disc); free(rn[i].artist); free(rn[i].album); free(rn[i].aartist); }
    free(rn); rn = 0; rn_n = 0;
}
/* valor de una cancion con los cambios pendientes del formulario */
static const char *rn_eff(int i, const char *key)
{
    int j = te_find(key);
    if (j >= 0 && te_fs[j].dirty) return te_fs[j].nv;
    if (!strcmp(key, "TITLE")) return rn[i].title;
    if (!strcmp(key, "TRACKNUMBER")) return rn[i].track;
    if (!strcmp(key, "DISCNUMBER")) return rn[i].disc;
    if (!strcmp(key, "ARTIST")) return rn[i].artist;
    if (!strcmp(key, "ALBUM")) return rn[i].album;
    return rn[i].aartist;
}
static int rn_load(void)
{
    rn_free();
    rn = calloc(te_n, sizeof *rn); if (!rn) return 0;
    rn_n = te_n; rn_samedir = 1; rn_multidisc = 0;
    char d0[600], s[300], e[32];
    te_split(te_paths[0], rn_dir, sizeof rn_dir, s, sizeof s, e, sizeof e);
    if (te_n > 12) progress_card(0, 0, -1);
    for (int i = 0; i < te_n; i++) {
        ht_all a; ht_read_all(te_paths[i], &a);
        rn[i].title = rn_dup(&a, "TITLE"); rn[i].track = rn_dup(&a, "TRACKNUMBER"); rn[i].disc = rn_dup(&a, "DISCNUMBER");
        rn[i].artist = rn_dup(&a, "ARTIST"); rn[i].album = rn_dup(&a, "ALBUM"); rn[i].aartist = rn_dup(&a, "ALBUMARTIST");
        const ht_item *dt = ht_all_find(&a, "DISCTOTAL");
        if (dt && atoi(dt->val) > 1) rn_multidisc = 1;
        ht_all_free(&a);
        te_split(te_paths[i], d0, sizeof d0, s, sizeof s, e, sizeof e);
        if (strcmp(d0, rn_dir)) rn_samedir = 0;
        st->shell_beat++;
        if (te_n > 12) { char sub[48]; snprintf(sub, sizeof sub, tr("%d de %d"), i + 1, te_n); progress_card(tr("Leyendo etiquetas"), sub, (double)(i + 1) / te_n); }
    }
    for (int i = 1; i < te_n; i++) if (atoi(rn[i].disc) != atoi(rn[0].disc)) rn_multidisc = 1;
    return 1;
}
static int rn_is_root(const char *dir)           /* la raiz de la tarjeta (/.../mnt/sd_0) */
{
    char p[600]; snprintf(p, sizeof p, "%s", dir);
    char *sl = strrchr(p, '/'); if (!sl || sl == p) return 1;
    *sl = 0; sl = strrchr(p, '/');
    return !strcmp(sl ? sl + 1 : p, "mnt");
}
/* calcula nombres nuevos, conflictos y la carpeta destino con el patron y la opcion dados */
static void rn_plan(int pat, int fold)
{
    for (int i = 0; i < rn_n; i++) {
        char dir[600], stem[300], ext[32];
        te_split(te_paths[i], dir, sizeof dir, stem, sizeof stem, ext, sizeof ext);
        const char *base = strrchr(te_paths[i], '/'); base = base ? base + 1 : te_paths[i];
        rn[i].bad = 0; rn[i].change = 0;
        if (pat < 0 || !ht_pattern_name(pat, rn_eff(i, "TRACKNUMBER"), rn_eff(i, "DISCNUMBER"), rn_multidisc, rn_eff(i, "ARTIST"),
                                         rn_eff(i, "TITLE"), ext, rn[i].nn, sizeof rn[i].nn)) { snprintf(rn[i].nn, sizeof rn[i].nn, "%s", base); continue; }
        rn[i].change = strcmp(rn[i].nn, base) != 0;
    }
    for (int i = 0; i < rn_n; i++) {
        if (!rn[i].change) continue;
        char dir[600], s[300], e[32]; te_split(te_paths[i], dir, sizeof dir, s, sizeof s, e, sizeof e);
        for (int j = 0; j < rn_n; j++) {
            if (j == i) continue;
            char dj[600]; te_split(te_paths[j], dj, sizeof dj, s, sizeof s, e, sizeof e);
            if (!strcmp(dir, dj) && !strcasecmp(rn[i].nn, rn[j].nn)) rn[i].bad = 1;
        }
        char full[900]; snprintf(full, sizeof full, "%s/%s", dir, rn[i].nn);
        if (!access(full, F_OK)) {                             /* existe: vale solo si es una de estas canciones */
            int ours = 0;
            for (int j = 0; j < rn_n; j++) if (te_same(full, te_paths[j])) ours = 1;
            if (!ours) rn[i].bad = 1;
        }
    }
    /* carpeta */
    rn_target[0] = rn_tshow[0] = 0; rn_tbad = 0;
    if (!fold || !rn_samedir) return;
    const char *al = rn_eff(0, "ALBUM"), *aa = rn_eff(0, "ALBUMARTIST");
    if (!*aa) aa = rn_eff(0, "ARTIST");
    char cal[256], car[256], par[600], gp[600];
    if (!*al) { rn_tbad = 1; snprintf(rn_tshow, sizeof rn_tshow, "%s", tr("Falta el álbum")); return; }
    ht_name_clean(al, cal, sizeof cal); ht_name_clean(*aa ? aa : "", car, sizeof car);
    snprintf(par, sizeof par, "%s", rn_dir); char *sl = strrchr(par, '/'); if (sl) *sl = 0;
    if (fold == TE_FOLD_ALBUM) snprintf(rn_target, sizeof rn_target, "%s/%s", par, cal);
    else {
        if (!*aa) { rn_tbad = 1; snprintf(rn_tshow, sizeof rn_tshow, "%s", tr("Falta el artista")); return; }
        /* la madre es ya una carpeta de artista (se llama como el artista viejo o el nuevo)? */
        const char *pb = strrchr(par, '/'); pb = pb ? pb + 1 : par;
        char o1[256], o2[256]; ht_name_clean(rn[0].artist, o1, sizeof o1); ht_name_clean(rn[0].aartist, o2, sizeof o2);
        int artdir = !rn_is_root(par) && (!strcasecmp(pb, o1) || !strcasecmp(pb, o2) || !strcasecmp(pb, car));
        snprintf(gp, sizeof gp, "%s", par);
        if (artdir) { sl = strrchr(gp, '/'); if (sl) *sl = 0; }
        snprintf(rn_target, sizeof rn_target, "%s/%s/%s", gp, car, cal);
    }
    const char *mn = strstr(rn_target, "/mnt/");
    if (mn) { mn = strchr(mn + 5, '/'); }
    snprintf(rn_tshow, sizeof rn_tshow, "%s", mn ? mn + 1 : rn_target);
    if (!strcmp(rn_target, rn_dir)) { rn_target[0] = 0; return; }
    if (!access(rn_target, F_OK) && !te_same(rn_target, rn_dir)) { rn_tbad = 2; }
}
static int rn_pat, rn_fold;
static char rn_ex[HT_PAT_N][256];
static void te_draw_ren(uint16_t *d, const struct te_row *r, int sy, int pressed)
{
    (void)pressed;
    switch (r->type) {
    case R_HDR: draw_text(d, 32, sy + r->h - 9, r->txt, 12, C_GRAY, W - 64, TE_TOP, H); break;
    case R_NOTE: draw_wrapped(d, 32, sy + 4, W - 64, r->txt, 12, r->bad ? rgb(255, 69, 58) : C_GRAY, 16, 1); break;
    case R_OPT:
        if (r->sub) {
            draw_text(d, 32, sy + 20, r->txt, 15, 0xFFFF, W - 90, TE_TOP, H);
            draw_text(d, 32, sy + 38, r->sub, 12, r->bad ? rgb(255, 69, 58) : C_GRAY, W - 90, TE_TOP, H);
        } else draw_text(d, 32, sy + r->h / 2 + 5, r->txt, 15, 0xFFFF, W - 90, TE_TOP, H);
        if (r->sel) te_check(d, W - 46, sy + r->h / 2, C_BLUE, TE_TOP, H);
        break;
    case R_PREV: {
        const struct te_rn *q = &rn[r->idx];
        const char *ob = strrchr(te_paths[r->idx], '/'); ob = ob ? ob + 1 : te_paths[r->idx];
        draw_text(d, 32, sy + 20, q->nn, 14, q->bad ? rgb(255, 69, 58) : q->change ? 0xFFFF : rgb(150, 150, 156), W - 64, TE_TOP, H);
        draw_text(d, 32, sy + 37, q->bad ? tr("Ya existe un archivo con ese nombre") : q->change ? ob : tr("Sin cambios"), 11, q->bad ? rgb(255, 69, 58) : C_GRAY, W - 64, TE_TOP, H);
        break;
    }
    }
}
static int rn_conflict(void)
{
    for (int i = 0; i < rn_n; i++) if (rn[i].bad) return 1;
    return rn_tbad != 0;
}
static void te_build_ren(struct te_scr *s)
{
    s->n = 0; s->ch = 0;
    s->left = tr("Cancelar"); s->title = tr("Renombrar"); s->right = tr("Aceptar"); s->draw = te_draw_ren;
    rn_plan(rn_pat, rn_fold);
    int any = 0; for (int i = 0; i < rn_n; i++) any |= rn[i].change;
    s->right_on = !rn_conflict() && (any || rn_target[0] || (rn_pat < 0 && !rn_fold));
    struct te_row *r = te_row_add(s, R_HDR, 0, 40); if (r) r->txt = tr("Nombre de los archivos");
    r = te_row_add(s, R_OPT, 1, 44); if (r) { r->txt = tr("Sin cambiar"); r->idx = -1; r->sel = rn_pat < 0; }
    static const char *const pn[HT_PAT_N] = { N_("Pista - Título"), N_("Artista - Título"), N_("Pista Título") };
    for (int p = 0; p < HT_PAT_N; p++) {
        char dir[600], stem[300], ext[32]; te_split(te_paths[0], dir, sizeof dir, stem, sizeof stem, ext, sizeof ext);
        if (!ht_pattern_name(p, rn_eff(0, "TRACKNUMBER"), rn_eff(0, "DISCNUMBER"), rn_multidisc, rn_eff(0, "ARTIST"), rn_eff(0, "TITLE"), ext, rn_ex[p], sizeof rn_ex[p]))
            snprintf(rn_ex[p], sizeof rn_ex[p], "%s", tr("Falta el título"));
        r = te_row_add(s, R_OPT, 1, 52); if (r) { r->txt = tr(pn[p]); r->sub = rn_ex[p]; r->idx = (short)p; r->sel = rn_pat == p; }
    }
    r = te_row_add(s, R_HDR, 0, 40); if (r) r->txt = tr("Carpeta del álbum");
    if (!rn_samedir) {
        const char *t = tr("TAGS_DIFFDIRS");
        int h = draw_wrapped(fr0, 32, 0, W - 64, t, 12, C_GRAY, 16, 0); r = te_row_add(s, R_NOTE, 0, h + 10); if (r) r->txt = t;
    } else {
        static const char *const fo[3] = { N_("Sin cambiar"), N_("Álbum"), N_("Artista/Álbum") };
        for (int k = 0; k < 3; k++) {
            r = te_row_add(s, R_OPT, 2, k && rn_fold == k ? 52 : 44);
            if (!r) continue;
            r->txt = tr(fo[k]); r->idx = (short)(100 + k); r->sel = rn_fold == k;
            if (k && rn_fold == k) { r->sub = rn_tbad == 2 ? tr("Ya existe una carpeta con ese nombre") : rn_target[0] || rn_tbad ? rn_tshow : tr("Sin cambios"); r->bad = rn_tbad != 0; }
        }
        const char *t = tr("TAGS_FOLDER_NOTE");
        int h = draw_wrapped(fr0, 32, 0, W - 64, t, 12, C_GRAY, 16, 0); r = te_row_add(s, R_NOTE, 0, h + 12); if (r) r->txt = t;
    }
    if (rn_pat >= 0) {
        r = te_row_add(s, R_HDR, 0, 40); if (r) r->txt = tr("Vista previa");
        for (int i = 0; i < rn_n; i++) { r = te_row_add(s, R_PREV, 3, 48); if (r) r->idx = (short)i; }
    }
    te_row_add(s, R_GAP, 0, 24);
}
static void te_rename_screen(void)
{
    if (!rn_load()) return;
    rn_pat = te_pat; rn_fold = rn_samedir ? te_fold : 0;
    struct te_scr s; memset(&s, 0, sizeof s);
    te_build_ren(&s);
    te_push(&s, 1);
    for (;;) {
        int row, x, ev = te_loop(&s, &row, &x);
        if (ev == EV_LEFT) break;
        if (ev == EV_RIGHT) { te_pat = rn_pat; te_fold = rn_fold; break; }
        if (ev != EV_ROW || s.r[row].type != R_OPT) continue;
        int id = s.r[row].idx;
        if (id >= 100) rn_fold = id - 100; else rn_pat = id;
        float sc = s.sc; te_build_ren(&s); s.sc = sc;
    }
    free(s.r);
}

/* ------------------------------------------------------------------ guardar */
/* la cancion esta abierta por el reproductor? */
static int te_playing(const char *path)
{
    if (st->np_magic != HM_NP_MAGIC) return 0;
    const char *a = strstr(path, "/mnt/");
    if (!a) return 0;
    for (int k = 0; k < HM_NP_SLOTS; k++) {
        if (st->np_open[k].fd < 0 || !st->np_open[k].gen) continue;
        const char *b = strstr((const char *)st->np_open[k].path, "/mnt/");
        if (b && !strcmp(a, b)) return 1;
    }
    return 0;
}
/* rutas viejas -> nuevas para la base; deshace el renombrado (orden inverso) si hace falta */
struct te_mv { char from[700], to[700]; int dir; };
static void te_undo(struct te_mv *mv, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (mv[i].dir) ht_rename_dir(mv[i].to, mv[i].from, 1);
        else ht_rename_song(mv[i].to, mv[i].from);
    }
}
/* ejecuta el renombrado pendiente y actualiza la base. 1 = hecho (o nada que hacer), 0 = fallo (deshecho) */
static int te_do_rename(char *msg, int mcap)
{
    struct te_mv *mv = calloc(te_n * 2 + 2, sizeof *mv);      /* fase 1 + fase 2 + carpeta */
    struct te_mv *db = calloc(te_n + 2, sizeof *db);
    int nm = 0, nd = 0, ok = 1;
    msg[0] = 0;
    if (!mv || !db) { free(mv); free(db); return 0; }
    if (te_n == 1 && te_fname_on) {
        char dir[600], s[300], e[32]; te_split(te_paths[0], dir, sizeof dir, s, sizeof s, e, sizeof e);
        snprintf(mv[0].from, 700, "%s", te_paths[0]); snprintf(mv[0].to, 700, "%s/%s", dir, te_fname);
        int r = ht_rename_song(mv[0].from, mv[0].to);
        if (r) { snprintf(msg, mcap, "%s", r == HT_EEXIST ? tr("Ya existe un archivo con ese nombre") : tr("No se pudo renombrar")); ok = 0; }
        else { nm = 1; db[nd++] = mv[0]; }
    } else if (te_n > 1 && (te_pat >= 0 || te_fold) && rn_load()) {
        rn_plan(te_pat, te_fold);
        if (rn_conflict()) { snprintf(msg, mcap, "%s", tr("Ya existe un archivo con ese nombre")); ok = 0; }
        /* fase 1: a nombres temporales (asi se pueden intercambiar nombres dentro del album) */
        for (int i = 0; ok && i < te_n; i++) {
            if (!rn[i].change) continue;
            char dir[600], s[300], e[32]; te_split(te_paths[i], dir, sizeof dir, s, sizeof s, e, sizeof e);
            struct te_mv *m = &mv[nm];
            snprintf(m->from, 700, "%s", te_paths[i]); snprintf(m->to, 700, "%s/.hibytag-ren-%d%s", dir, i, e);
            if (ht_rename_song(m->from, m->to)) ok = 0; else nm++;
        }
        /* fase 2: a los nombres definitivos */
        for (int i = 0, k = 0; ok && i < te_n; i++) {
            if (!rn[i].change) continue;
            char dir[600], s[300], e[32]; te_split(te_paths[i], dir, sizeof dir, s, sizeof s, e, sizeof e);
            struct te_mv *m = &mv[nm];
            snprintf(m->from, 700, "%s", mv[k++].to); snprintf(m->to, 700, "%s/%s", dir, rn[i].nn);
            if (ht_rename_song(m->from, m->to)) ok = 0;
            else { nm++; snprintf(db[nd].from, 700, "%s", te_paths[i]); snprintf(db[nd].to, 700, "%s", m->to); nd++; }
        }
        if (!ok && !msg[0]) snprintf(msg, mcap, "%s", tr("No se pudo renombrar"));
        if (ok && rn_target[0]) {
            struct te_mv *m = &mv[nm];
            snprintf(m->from, 700, "%s", rn_dir); snprintf(m->to, 700, "%s", rn_target); m->dir = 1;
            int r = ht_rename_dir(m->from, m->to, te_fold == TE_FOLD_ARTIST);
            if (r) { ok = 0; snprintf(msg, mcap, "%s", r == HT_EEXIST ? tr("Ya existe una carpeta con ese nombre") : tr("No se pudo renombrar la carpeta")); }
            else { nm++; db[nd++] = *m; }
        }
        if (!ok) { te_undo(mv, nm); nm = 0; nd = 0; }
    }
    /* biblioteca de HiBy: rutas nuevas en una transaccion; si falla, se deshace todo */
    if (ok && nd) {
        progress_card(tr("Actualizando la biblioteca"), 0, 0.99);
        char **argv = calloc(2 * nd + 3, sizeof *argv); int na = 0;
        argv[na++] = TAGBIN; argv[na++] = "dbrename";
        for (int i = 0; i < nd; i++) { argv[na++] = db[i].from; argv[na++] = db[i].to; }
        char out[512]; int rc = run_status(argv, out, sizeof out);
        free(argv);
        lg("tags: dbrename %d rutas -> %d (%s)", nd, rc, out);
        if (rc != 0 && strstr(out, "no se pudo abrir")) snprintf(msg, mcap, "%s", tr("TAGS_DB_RESCAN"));    /* sin base: la reconstruira HiBy */
        else if (rc != 0) { te_undo(mv, nm); ok = 0; snprintf(msg, mcap, "%s", tr("No se pudo actualizar la biblioteca: no se ha renombrado nada")); }
    }
    if (ok) sync();
    free(mv); free(db); rn_free();
    return ok;
}
/* 1 = cerrar el editor */
static int te_save(void)
{
    if (!te_any_dirty()) return 0;
    if (te_fname_on || te_pat >= 0 || te_fold) {
        int busy = 0; for (int i = 0; i < te_n && !busy; i++) busy = te_playing(te_paths[i]);
        if (busy && !confirm(tr("TAGS_PLAYING"), tr("Renombrar igualmente"))) return 0;
    }
    ht_kv *kv = calloc(te_nf + 1, sizeof *kv); int nkv = 0;
    for (int i = 0; kv && i < te_nf; i++) if (te_fs[i].dirty) { kv[nkv].key = te_fs[i].key; kv[nkv].val = te_fs[i].nv; nkv++; }
    int bad = 0;
    if (nkv) {
        progress_card(0, 0, -1);
        progress_card(tr("Guardando etiquetas"), 0, 0);
        int *res = calloc(te_n, sizeof(int));
        bad = ht_batch_kv((const char *const *)te_paths, te_n, kv, nkv, te_batch_cb, 0, res);
        /* biblioteca de HiBy: sin reescanear, solo las filas de estas canciones */
        progress_card(tr("Actualizando la biblioteca"), 0, 0.99);
        int na = 2; char **argv = calloc(te_n + 3, sizeof(char *)); argv[0] = TAGBIN; argv[1] = "dbupdate";
        for (int i = 0; i < te_n; i++) if (res && !res[i]) argv[na++] = te_paths[i];
        if (na > 2) { char o[256]; run_capture(argv, o, sizeof o); }
        free(argv); free(res);
    } else { progress_card(0, 0, -1); progress_card(tr("Renombrando"), 0, 0.5); }
    free(kv);
    char msg[200] = "";
    int rok = (te_fname_on || te_pat >= 0 || te_fold) ? te_do_rename(msg, sizeof msg) : 1;
    if (msg[0]) toast(msg);
    else toast(bad ? tr("Algunas no se pudieron guardar") : tr("Cambios guardados"));
    (void)rok;
    return 1;
}

/* ------------------------------------------------------------------ entrada */
static void tag_editor(char **paths, int n)
{
    if (!n) return;
    te_load(paths, n);
    if (!te_fs) return;
    struct te_scr s; memset(&s, 0, sizeof s);
    te_build_main(&s);
    for (;;) {
        int row, x, ev = te_loop(&s, &row, &x);
        if (ev == EV_LEFT) {
            if (!te_any_dirty() || confirm(tr("Hay cambios sin guardar"), tr("Descartar cambios"))) break;
        } else if (ev == EV_RIGHT) {
            if (te_save()) break;
        } else if (ev == EV_LONG && s.r[row].type == R_FIELD) {
            int i = s.r[row].idx; struct te_f *f = &te_fs[i]; int b = ht_key_base(f->key);
            if (!(f->flags & (HT_ITEM_RO | HT_ITEM_TRUNC)) && te_nm4a < te_n && !(b >= 0 && TE_LOCKED(b, te_n)) && (*te_cur(f) || f->multi)) {
                char nm[160]; te_name(f->key, f->orig, nm, sizeof nm);
                const char *o[1] = { tr("Eliminar campo") };
                if (action_sheet(nm, o, 1, 0) == 0) te_set(i, "");
            }
        } else if (ev == EV_ROW) {
            const struct te_row *r = &s.r[row];
            if (r->type == R_FIELD) te_edit_field(r->idx);
            else if (r->type == R_BTNS) {
                const char *ar = te_cur(&te_fs[HT_ARTIST]), *al = te_cur(&te_fs[HT_ALBUM]), *ti = te_cur(&te_fs[HT_TITLE]);
                if (x < W / 2) cover_search(paths, n, ar, al);
                else if (n == 1) lyrics_search(paths[0], ar, ti, al);
            } else if (r->type == R_ADD) te_add_field(&s);
            else if (r->type == R_FILE) te_edit_fname();
            else if (r->type == R_RENAME) {
                te_rename_screen();
                float sc = s.sc; te_build_main(&s); s.sc = sc;
                te_push(&s, -1);
                continue;
            }
        }
        float sc = s.sc; te_build_main(&s); s.sc = sc;
    }
    free(s.r);
    te_free();
}
