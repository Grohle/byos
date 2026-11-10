/* tageditor.inc.c - editor de etiquetas (una o varias canciones a la vez).
 * Lectura/escritura en el propio shell con hibytag.c (sin base de datos); autocompletado y
 * actualizacion de la biblioteca de HiBy con el programa hibytag (SQLite) como proceso aparte.
 * Formulario estilo iOS; al escribir, sugerencias de la biblioteca en un desplegable. */
#define HT_API static
#define HIBYTAG_NO_DB
#include "hibytag.c"

#define TAGBIN ASSETS "/hibytag"
static int confirm(const char *title, const char *button);
static void cover_search(const char *path, const char *artist, const char *album);
static void lyrics_search(const char *path, const char *artist, const char *title, const char *album);
static const char *const TE_FIELDS[HT_NFIELDS] = { "Título", "Artista", "Álbum", "Artista del álbum", "Género", "Año", "Pista", "Disco" };
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
    le_redraw(dst, pressed);
    int y = 148;
    for (int i = 0; i < nsg && y + 38 <= KB_Y - 4; i++, y += 40) {
        fill_rrect(dst, 12, y, W - 24, 36, 9, rgb(36, 36, 40), 255);
        draw_text(dst, 26, y + 24, sg[i], 15, 0xFFFF, W - 52, 0, H);
    }
}
static int line_edit_suggest(const char *title, const char *hint, char *buf, int cap, const char *field)
{
    struct ebuf e = { buf, (int)strlen(buf), cap, (int)strlen(buf) };
    le_title = title; le_hint = hint; le_buf = &e; sg_field = field; nsg = 0;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Aceptar"));
    char last[128] = "\x01"; double typed = 0;
    sg_redraw(fr0, -1); present();
    struct pkt p;
    for (;;) {
        /* sugerencias tras una pausa al escribir (no en cada tecla) */
        if (field && strcmp(last, buf) && now_ms() - typed > 140) { snprintf(last, sizeof last, "%s", buf); sg_fetch(buf); sg_redraw(fr0, -1); present(); }
        if (!read_pkt(&p, 60) || !p.down) continue;
        if (p.y >= KB_Y) { int r = kb_touch(&p, &e, fr0, sg_redraw, 0); typed = now_ms(); if (r == K_ENTER) return 1; continue; }
        int x = p.x, y = p.y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 90 && x < 100) return 0;
        if (y >= 148 && nsg) {
            int i = (y - 148) / 40;
            if (i >= 0 && i < nsg) { snprintf(buf, cap, "%s", sg[i]); return 1; }
        }
        if (y >= 96 && y < 140) { e.cur = e.len; sg_redraw(fr0, -1); present(); }
    }
}

/* ------------------------------------------------------------------ formulario */
#define TE_MAX 400
#define TE_Y 120
#define TE_RH 36
static char *te_val[HT_NFIELDS];          /* valor comun (NULL = varios valores) */
static int te_multi[HT_NFIELDS];          /* distintos entre ficheros */
static int te_dirty[HT_NFIELDS];
static char te_new[HT_NFIELDS][256];

static void te_render(uint16_t *d, int n, int sel)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < 94; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(12, 12, 13);
    draw_text(d, 14, 68, tr("Cancelar"), 16, rgb(10, 132, 255), 0, 0, H);
    const char *tt = n > 1 ? tr("Editar canciones") : tr("Editar canción");
    int tw = tw_n(tt, 17, -1); draw_text(d, (W - tw) / 2, 68, tt, 17, 0xFFFF, 0, 0, H);
    int any = 0; for (int f = 0; f < HT_NFIELDS; f++) any |= te_dirty[f];
    draw_text(d, W - 14 - tw_n(tr("Guardar"), 16, -1), 68, tr("Guardar"), 16, any ? rgb(10, 132, 255) : rgb(70, 70, 76), 0, 0, H);
    if (n > 1) { char c[64]; snprintf(c, sizeof c, tr("%d canciones"), n); draw_text(d, 32, 116, c, 12, rgb(142, 142, 147), 0, 0, H); }
    int y = TE_Y, rh = TE_RH;
    fill_rrect(d, 16, y, W - 32, HT_NFIELDS * rh, 10, rgb(28, 28, 30), 255);
    for (int f = 0; f < HT_NFIELDS; f++) {
        int ry = y + f * rh;
        if (f == sel) fill_rrect(d, 16, ry, W - 32, rh, (f == 0 || f == HT_NFIELDS - 1) ? 10 : 0, rgb(58, 58, 60), 255);
        if (f) for (int x = 32; x < W - 16; x++) d[ry * W + x] = rgb(56, 56, 58);
        int locked = n > 1 && (f == HT_TITLE || f == HT_TRACK);
        draw_text(d, 32, ry + 24, tr(TE_FIELDS[f]), 13, locked ? rgb(90, 90, 96) : rgb(152, 152, 159), 112, 0, H);
        const char *v = te_dirty[f] ? te_new[f] : te_multi[f] ? tr("Varios valores") : (te_val[f] ? te_val[f] : "");
        uint16_t vc = te_dirty[f] ? rgb(10, 132, 255) : te_multi[f] ? rgb(110, 110, 116) : 0xFFFF;
        if (locked) { v = tr("Distinto en cada una"); vc = rgb(90, 90, 96); }
        draw_text(d, 150, ry + 24, v, 14, vc, W - 16 - 12 - 150, 0, H);
    }
    /* buscar en internet: caratula (album) y letra (una cancion) */
    int by = TE_Y + HT_NFIELDS * TE_RH + 10, bw = (W - 32 - 10) / 2;
    fill_rrect(d, 16, by, bw, 36, 12, rgb(44, 44, 48), 255);
    int w1 = tw_n(tr("Buscar carátula"), 14, -1); draw_text(d, 16 + (bw - w1) / 2, by + 24, tr("Buscar carátula"), 14, rgb(10, 132, 255), bw - 8, 0, H);
    fill_rrect(d, 26 + bw, by, bw, 36, 12, rgb(44, 44, 48), 255);
    int w2 = tw_n(tr("Buscar letra"), 14, -1); draw_text(d, 26 + bw + (bw - w2) / 2, by + 24, tr("Buscar letra"), 14, n == 1 ? rgb(10, 132, 255) : rgb(70, 70, 76), bw - 8, 0, H);
    draw_toast(d);
}

/* tarjeta de progreso: barra que avanza con un brillo que la recorre */
static double te_last_draw;
static void progress_card(const char *title, const char *sub, double frac)
{
    static uint16_t base[W * H]; static int have;
    if (frac < 0) { memcpy(base, fr0, sizeof base); for (int i = 0; i < W * H; i++) base[i] = dim565(base[i], 14); have = 1; return; }
    if (!have) return;
    memcpy(fr0, base, sizeof base);
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
struct te_prog { int n; };
static int te_batch_cb(void *ud, int idx, int n, uint64_t done, uint64_t total)
{
    (void)ud; st->shell_beat++;
    if (now_ms() - te_last_draw < 30 && idx < n) return 0;
    char sub[64]; snprintf(sub, sizeof sub, tr("%d de %d"), idx + 1 > n ? n : idx + 1, n);
    progress_card(tr("Guardando etiquetas"), sub, total ? (double)done / total : (double)idx / n);
    return 0;
}

static void tag_editor(char **paths, int n)
{
    for (int f = 0; f < HT_NFIELDS; f++) { free(te_val[f]); te_val[f] = 0; te_multi[f] = 0; te_dirty[f] = 0; te_new[f][0] = 0; }
    /* leer: valores comunes (con muchos ficheros solo se leen los primeros 60) */
    progress_card(0, 0, -1);
    int nr = n < 60 ? n : 60;
    for (int i = 0; i < nr; i++) {
        ht_tags t; ht_read(paths[i], &t);
        for (int f = 0; f < HT_NFIELDS; f++) {
            const char *v = t.v[f] ? t.v[f] : "";
            if (i == 0) te_val[f] = strdup(v);
            else if (!te_multi[f] && strcmp(te_val[f], v)) te_multi[f] = 1;
        }
        ht_tags_free(&t);
        if (n > 8) { char sub[48]; snprintf(sub, sizeof sub, tr("%d de %d"), i + 1, nr); progress_card(tr("Leyendo etiquetas"), sub, (double)(i + 1) / nr); }
    }
    te_render(fr0, n, -1); present();
    struct pkt p;
    for (;;) {
        if (!read_pkt(&p, now_ms() < toast_until + 40 ? 30 : 400)) { if (now_ms() < toast_until + 40) { te_render(fr0, n, -1); present(); } continue; }
        if (!p.down) continue;
        int x = p.x, y = p.y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 94 && x < 100) {
            int any = 0; for (int f = 0; f < HT_NFIELDS; f++) any |= te_dirty[f];
            if (!any || confirm(tr("Hay cambios sin guardar"), tr("Descartar cambios"))) return;
            te_render(fr0, n, -1); present(); continue;
        }
        if (y < 94 && x > W - 100) {
            int any = 0; ht_changes ch; memset(&ch, 0, sizeof ch);
            for (int f = 0; f < HT_NFIELDS; f++) if (te_dirty[f]) { ch.v[f] = te_new[f]; any = 1; }
            if (!any) continue;
            progress_card(0, 0, -1);
            progress_card(tr("Guardando etiquetas"), 0, 0);
            int *res = calloc(n, sizeof(int));
            int bad = ht_batch((const char *const *)paths, n, &ch, te_batch_cb, 0, res);
            /* biblioteca de HiBy: sin reescanear, solo las filas de estas canciones */
            progress_card(tr("Actualizando la biblioteca"), 0, 0.99);
            int na = 2; char **argv = calloc(n + 3, sizeof(char *)); argv[0] = TAGBIN; argv[1] = "dbupdate";
            for (int i = 0; i < n; i++) if (!res[i]) argv[na++] = paths[i];
            if (na > 2) { char o[256]; run_capture(argv, o, sizeof o); }
            free(argv); free(res);
            toast(bad ? tr("Algunas no se pudieron guardar") : tr("Cambios guardados"));
            return;
        }
        int by = TE_Y + HT_NFIELDS * TE_RH + 10;
        if (y >= by - 4 && y < by + 44) {
            const char *ar = te_dirty[HT_ARTIST] ? te_new[HT_ARTIST] : te_val[HT_ARTIST] ? te_val[HT_ARTIST] : "";
            const char *al = te_dirty[HT_ALBUM] ? te_new[HT_ALBUM] : te_val[HT_ALBUM] ? te_val[HT_ALBUM] : "";
            const char *ti = te_dirty[HT_TITLE] ? te_new[HT_TITLE] : te_val[HT_TITLE] ? te_val[HT_TITLE] : "";
            if (x < W / 2) cover_search(paths[0], ar, al);
            else if (n == 1) lyrics_search(paths[0], ar, ti, al);
            te_render(fr0, n, -1); present(); continue;
        }
        int f = (y - TE_Y) / TE_RH;
        if (y < TE_Y || f < 0 || f >= HT_NFIELDS) continue;
        if (n > 1 && (f == HT_TITLE || f == HT_TRACK)) continue;
        te_render(fr0, n, f); present();
        char buf[256]; snprintf(buf, sizeof buf, "%s", te_dirty[f] ? te_new[f] : te_multi[f] ? "" : (te_val[f] ? te_val[f] : ""));
        if (line_edit_suggest(tr(TE_FIELDS[f]), te_multi[f] ? tr("Varios valores") : "", buf, sizeof buf, TE_SUGG[f])) {
            if (strcmp(buf, te_val[f] ? te_val[f] : "") || te_multi[f]) { snprintf(te_new[f], sizeof te_new[f], "%s", buf); te_dirty[f] = 1; }
        }
        te_render(fr0, n, -1); present();
    }
}
