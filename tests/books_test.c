/* books_test.c - pruebas en PC (x86, WSL) del nucleo de Libros (src/books_core.inc.c).
 * Compilar/ejecutar: bash tests/books_test.sh  (genera los libros con books_samples.py y usa las
 * fuentes y la tabla GBK del firmware de ~/hiby/rootfs). Comprueba:
 *   - inflate: CRC32 de todas las entradas de los EPUB (deflate y stored, un capitulo de ~300 KB);
 *     datos corruptos/truncados no cuelgan ni salen de los limites (ASan/UBSan);
 *   - EPUB3 (nav, cover-image) y EPUB2 (NCX, meta cover, ../ y %20): titulo, autor, portada,
 *     spine, indice con profundidad y fragmentos resueltos a desplazamientos;
 *   - XHTML -> flujo: cabeceras, cursiva/negrita, entidades, <br>, <pre>, <li>, <hr>, sin
 *     <head>/<script>/<style>;
 *   - TXT: UTF-8 (reflujo Gutenberg), UTF-16LE/BE con BOM, CP1252, GBK, CP1251; cabeceras;
 *     los bloques de 64 KB concatenados dan exactamente el texto entero;
 *   - maquetacion con las fuentes reales: paginas crecientes, lineas dentro del ancho, todo el
 *     texto cubierto, pagina de un desplazamiento; tiempo de paginacion. */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define W 320
#define H 480
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
#include "i18n.inc.c"
#include "text.inc.c"
#include "books_core.inc.c"
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
#include "books_draw.inc.c"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ONLY_PNG
#include "stb_image_write.h"

static int fails, passes;
static const char *D;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static const char *P(const char *n) { static char b[4][600]; static int k; k = (k + 1) & 3; snprintf(b[k], sizeof b[k], "%s/%s", D, n); return b[k]; }

static uint32_t crc_tab[256];
static void crc_init(void) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; } }
struct crcctx { uint32_t c; long n; };
static int crc_out(void *x, const uint8_t *b, int n) { struct crcctx *c = x; for (int i = 0; i < n; i++) c->c = crc_tab[(c->c ^ b[i]) & 255] ^ (c->c >> 8); c->n += n; return 0; }

/* avance real (como glyph_get del shell) */
static int adv_calc(int cp, int size) { int gi; const stbtt_fontinfo *f = font_glyph(cp, &gi); float sc = font_scale(f, (float)size); return (int)(glyph_adv_px(f, gi, cp, sc) * 64); }
static struct bk_advc AC;
static int adv(void *c, int cp, int size) { (void)c; return bk_adv_cached(&AC, cp, size, adv_calc); }

/* marcas */
struct marks { int n; char s[64][120]; int off[64]; int kind[64]; };
static void mark(void *c, int kind, const char *s, int off) { struct marks *m = c; if (m->n < 64) { snprintf(m->s[m->n], 120, "%s", s); m->off[m->n] = off; m->kind[m->n] = kind; m->n++; } }
static int mark_off(const struct marks *m, const char *id) { for (int i = 0; i < m->n; i++) if (!strcmp(m->s[i], id)) return m->off[i]; return -1; }

static int count_c(const char *s, int n, char c) { int k = 0; for (int i = 0; i < n; i++) k += s[i] == c; return k; }

/* paginar un flujo y comprobar invariantes */
static int check_layout(const char *name, const char *f, int len, int size)
{
    struct bk_lp L; bk_lp_make(&L, W - 48, H - 76, size, 1);
    int *pg = 0; double t0 = ms();
    int np = bk_paginate(&L, f, len, adv, 0, &pg);
    double dt = ms() - t0;
    int ok = np > 0 || len == 0;
    struct bk_line lines[128];
    for (int i = 0; i < np; i++) {
        if (i && pg[i] <= pg[i - 1]) { ok = 0; printf("  %s: pagina %d no avanza\n", name, i); break; }
        int nx, nl = bk_layout_page(&L, f, len, pg[i], adv, 0, lines, 128, &nx);
        if (nl <= 0) { ok = 0; printf("  %s: pagina %d vacia\n", name, i); break; }
        for (int k = 0; k < nl; k++) {
            if (lines[k].w26 > (L.w << 6)) { ok = 0; printf("  %s: linea demasiado ancha p%d l%d\n", name, i, k); }
            if (lines[k].y + (lines[k].kind == 1 ? L.hlh : L.lh) > L.h) { ok = 0; printf("  %s: linea fuera de la pagina p%d l%d\n", name, i, k); }
        }
        int expect = i + 1 < np ? pg[i + 1] : len;
        int nx2 = nx; while (nx2 < len && f[nx2] == '\n') nx2++;
        if (nx2 != expect) { ok = 0; printf("  %s: la pagina %d acaba en %d y la siguiente empieza en %d\n", name, i, nx2, expect); break; }
        if (bk_page_of(pg, np, pg[i]) != i || (expect - 1 > pg[i] && bk_page_of(pg, np, expect - 1) != i)) { ok = 0; printf("  %s: bk_page_of\n", name); }
    }
    printf("  %-22s %7d bytes -> %4d paginas (%d px) en %.1f ms\n", name, len, np, size, dt);
    free(pg);
    CHECK(ok, "maquetacion de %s", name);
    return np;
}

static void test_epub3(void)
{
    struct bk_book b;
    CHECK(!bk_open(&b, P("alice3.epub"), 0), "abrir alice3.epub");
    if (b.fd < 0) return;
    CHECK(!strcmp(b.title, "Alice's Adventures in Wonderland"), "titulo: %s", b.title);
    CHECK(!strcmp(b.author, "Lewis Carroll"), "autor: %s", b.author);
    CHECK(b.cover_zi >= 0 && !strncmp(b.z.names + b.z.e[b.cover_zi].name + b.z.e[b.cover_zi].nlen - 9, "cover.png", 9), "portada");
    CHECK(b.nch == 4, "capitulos: %d", b.nch);
    /* CRC de todas las entradas */
    for (int i = 0; i < b.z.n; i++) {
        struct crcctx c = { 0xFFFFFFFFu, 0 };
        int r = bk_zip_stream(&b.z, i, crc_out, &c);
        CHECK(!r && (c.c ^ 0xFFFFFFFFu) == b.z.e[i].crc && c.n == (long)b.z.e[i].usize, "crc de %.*s (r=%d, %ld bytes)", b.z.e[i].nlen, b.z.names + b.z.e[i].name, r, c.n);
    }
    CHECK(b.ntoc == 4, "indice: %d", b.ntoc);
    if (b.ntoc == 4) {
        CHECK(!strcmp(b.toc[0].title, "Down the Rabbit-Hole") && b.toc[0].depth == 0 && b.toc[0].ch == 1 && b.toc[0].off == 0, "toc0 %s d%d ch%d", b.toc[0].title, b.toc[0].depth, b.toc[0].ch);
        CHECK(!strcmp(b.toc[1].title, "The Rabbit speaks") && b.toc[1].depth == 1 && !strcmp(b.toc[1].frag, "p1_2") && b.toc[1].off == -1, "toc1 %s d%d %s", b.toc[1].title, b.toc[1].depth, b.toc[1].frag);
        CHECK(b.toc[2].ch == 2 && b.toc[3].ch == 3 && !strcmp(b.toc[3].frag, "mid"), "toc2/3");
    }
    struct bk_buf o = { 0 }; struct marks m = { 0 };
    CHECK(!bk_flow(&b, 0, &o, 0, 0) && o.n == 0, "la pagina de portada no tiene texto (%d)", o.n);
    CHECK(!bk_flow(&b, 1, &o, mark, &m), "flujo ch1");
    CHECK(o.n > 0 && o.b[0] == BKC_HEAD && !strncmp(o.b + 1, "CHAPTER I. Down the Rabbit-Hole\n", 32), "cabecera ch1: %.40s", o.b);
    CHECK(strstr(o.b, "\x02White\x03 \x04Rabbit\x05 with pink eyes") != 0, "cursiva/negrita");
    CHECK(!strstr(o.b, "no</p>") && !strstr(o.b, "text-indent") && !strstr(o.b, "var x"), "sin head/script/style");
    CHECK(strstr(o.b, "“and what is the use of a book,” thought Alice") != 0, "comillas UTF-8");
    CHECK(count_c(o.b, o.n, '\n') == 4, "parrafos ch1: %d", count_c(o.b, o.n, '\n'));
    int off = mark_off(&m, "p1_2");
    CHECK(off >= 0 && !strncmp(o.b + off, "There was nothing", 17), "ancla p1_2 en %d", off);
    CHECK(bk_style_at(o.b, (int)(strstr(o.b, "White") - o.b) + 2) == 1, "estilo en mitad de la cursiva");
    check_layout("alice3 ch1", o.b, o.n, 17);
    /* capitulo grande */
    m.n = 0; double t0 = ms();
    CHECK(!bk_flow(&b, 3, &o, mark, &m), "flujo grande");
    printf("  flujo del capitulo grande: %d bytes en %.1f ms\n", o.n, ms() - t0);
    CHECK(strstr(o.b, "MIDDLE MARKER \xe2\x80\x94 caf\xc3\xa9 \xe4\xb8\xad \xc2\xa0x &unknown; 5 < 6\n") != 0, "entidades");
    CHECK(o.n > 30 && !strcmp(o.b + o.n - 27, "THE END OF THE BIG CHAPTER\n"), "final del grande");
    CHECK(count_c(o.b, o.n, '\n') == 1503, "parrafos grande: %d", count_c(o.b, o.n, '\n'));
    off = mark_off(&m, "mid"); CHECK(off > 0 && !strncmp(o.b + off, "MIDDLE", 6), "ancla mid");
    check_layout("alice3 grande 17px", o.b, o.n, 17);
    check_layout("alice3 grande 27px", o.b, o.n, 27);
    check_layout("alice3 grande 14px", o.b, o.n, 14);
    /* corrupcion: inflate desde memoria con bytes cambiados (no debe colgar ni salirse) */
    {
        int zi = b.ch[3].zi; struct bk_zent *e = &b.z.e[zi]; uint8_t lh[30];
        pread(b.fd, lh, 30, e->lho);
        long long doff = e->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
        uint8_t *d = malloc(e->csize); pread(b.fd, d, e->csize, doff);
        struct crcctx c = { 0xFFFFFFFFu, 0 };
        CHECK(!bk_inflate(-1, 0, e->csize, d, crc_out, &c) && (c.c ^ 0xFFFFFFFFu) == e->crc, "inflate desde memoria");
        srand(3); int errs = 0; double t1 = ms();
        for (int k = 0; k < 300; k++) {
            uint8_t *x = malloc(e->csize); memcpy(x, d, e->csize);
            for (int j = 0; j < 1 + k % 8; j++) x[rand() % e->csize] ^= (uint8_t)(1 + rand() % 255);
            long long cl = k % 5 == 0 ? e->csize / (2 + k % 7) : e->csize;
            struct crcctx c2 = { 0, 0 };
            if (bk_inflate(-1, 0, cl, x, crc_out, &c2)) errs++;
            CHECK(c2.n < (64 << 20), "salida acotada");
            free(x);
        }
        printf("  300 inflates corruptos: %d errores limpios, %.0f ms\n", errs, ms() - t1);
        free(d);
    }
    bb_free(&o); bk_close(&b);
}

static void test_epub2(void)
{
    struct bk_book b;
    CHECK(!bk_open(&b, P("alice2.epub"), 0), "abrir alice2.epub");
    if (b.fd < 0) return;
    CHECK(!strcmp(b.title, "Don Quijote") && !strcmp(b.author, "Miguel de Cervantes"), "metadatos %s / %s", b.title, b.author);
    CHECK(b.cover_zi >= 0 && !strncmp(b.z.names + b.z.e[b.cover_zi].name, "book/img/portada q.jpg", 22), "portada con ../ y %%20");
    CHECK(b.nch == 1, "spine con %%20: %d", b.nch);
    CHECK(b.ntoc == 2 && b.toc[1].depth == 1 && !strcmp(b.toc[1].frag, "olla") && !strcmp(b.toc[0].title, "Capítulo primero"), "NCX: %d", b.ntoc);
    struct bk_buf o = { 0 }; struct marks m = { 0 };
    CHECK(!bk_flow(&b, 0, &o, mark, &m), "flujo epub2");
    CHECK(o.b && !strncmp(o.b, "\x01" "Capítulo primero\n", 18), "cabecera");
    CHECK(strstr(o.b ? o.b : "", "Verso uno\x0bverso dos\nEstrofa dos\n") != 0, "<br> y <br><br>");
    CHECK(strstr(o.b ? o.b : "", "linea 1\x0blinea 2\n") != 0, "<pre>");
    CHECK(strstr(o.b ? o.b : "", "\xe2\x80\xa2 uno\n\xe2\x80\xa2 dos\n\x06\nfin\n") != 0, "<li> y <hr>");
    int off = mark_off(&m, "olla"); CHECK(off > 0 && !strncmp(o.b + off, "Una olla", 8), "ancla olla");
    check_layout("alice2", o.b, o.n, 17);
    bb_free(&o); bk_close(&b);
    CHECK(!bk_open(&b, P("alice2.epub"), 1) && !strcmp(b.title, "Don Quijote") && b.cover_zi >= 0, "solo metadatos");
    bk_close(&b);
}

static void test_broken(void)
{
    struct bk_book b; int r = bk_open(&b, P("broken.epub"), 0);
    if (!r) { struct bk_buf o = { 0 }; for (int i = 0; i < b.nch; i++) bk_flow(&b, i, &o, 0, 0); bb_free(&o); bk_close(&b); }
    CHECK(1, "broken.epub sin romper nada (r=%d)", r);
    r = bk_open(&b, P("expected_big.txt"), 0);   /* no es un epub: como TXT */
    CHECK(!r && b.fmt == BK_TXT, "fichero de texto"); if (!r) bk_close(&b);
    r = bk_open(&b, P("no_existe.epub"), 0); CHECK(r, "inexistente");
}

/* texto entero de un TXT: concatenacion de sus bloques */
static char *txt_all(const char *name, struct bk_book *b, int *len, struct marks *m)
{
    if (bk_open(b, P(name), 0)) { CHECK(0, "abrir %s", name); return 0; }
    struct bk_buf all = { 0 }, o = { 0 };
    for (int i = 0; i < b->nch; i++) { bk_flow(b, i, &o, mark, m); bb_put(&all, o.b, o.n); }
    bb_free(&o); *len = all.n;
    return all.b;
}
static void test_txt(void)
{
    struct bk_book b; struct marks m = { 0 }; int n;
    char *t = txt_all("alice_gutenberg.txt", &b, &n, &m);
    if (t) {
        CHECK(b.enc == BE_UTF8 && b.reflow, "alice: utf8 %d reflujo %d", b.enc, b.reflow);
        CHECK(strstr(t, "\nAlice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to do: once or twice she had peeped into the book her sister was reading, but it had no pictures or conversations in it, “and what is the use of a book,” thought Alice “without pictures or conversations?”\n") != 0, "reflujo de parrafos");
        CHECK(m.n == 2 && m.kind[0] == 1 && !strcmp(m.s[0], "CHAPTER I. Down the Rabbit-Hole") && t[m.off[0]] == BKC_HEAD, "cabeceras TXT: %d", m.n);
        CHECK(!strchr(t, '\r'), "sin CR");
        check_layout("alice txt", t, n, 17);
        free(t); bk_close(&b);
    }
    static const char *const q[] = { "quijote_utf16le.txt", "quijote_utf16be.txt", "quijote_cp1252.txt" };
    static const int qe[] = { BE_U16LE, BE_U16BE, BE_CP1252 };
    for (int i = 0; i < 3; i++) {
        m.n = 0; t = txt_all(q[i], &b, &n, &m); if (!t) continue;
        CHECK(b.enc == qe[i], "%s: codificacion %d", q[i], b.enc);
        CHECK(!b.reflow && !strncmp(t, "\x01" "Capítulo primero\n", 18), "%s: cabecera", q[i]);
        CHECK(strstr(t, "\nEn un lugar de la Mancha, de cuyo nombre no quiero acordarme,") && strstr(t, "los sábados,") && strstr(t, "añadidura"), "%s: texto", q[i]);
        CHECK(count_c(t, n, '\n') == 3, "%s: parrafos %d", q[i], count_c(t, n, '\n'));
        free(t); bk_close(&b);
    }
    m.n = 0; t = txt_all("lunyu_gbk.txt", &b, &n, &m);
    if (t) {
        CHECK(b.enc == BE_GBK, "lunyu: GBK (%d)", b.enc);
        CHECK(strstr(t, "　　子曰：“学而时习之，不亦说乎？") != 0, "texto GBK");
        CHECK(m.n == 2 && !strcmp(m.s[1], "第二章 为政"), "cabeceras 第N章: %d", m.n);
        check_layout("lunyu gbk", t, n, 17);
        free(t); bk_close(&b);
    }
    m.n = 0; t = txt_all("chekhov_cp1251.txt", &b, &n, &m);
    if (t) {
        CHECK(b.enc == BE_CP1251, "chekhov: CP1251 (%d)", b.enc);
        CHECK(strstr(t, "Все счастливые семьи похожи друг на друга") && strstr(t, "Всё смешалось"), "texto CP1251");
        CHECK(m.n == 1 && !strcmp(m.s[0], "Глава первая"), "cabecera Глава");
        free(t); bk_close(&b);
    }
    /* bloques: la concatenacion es identica al texto procesado entero */
    m.n = 0; t = txt_all("big_utf8.txt", &b, &n, &m);
    if (t) {
        CHECK(b.nch >= 3, "bloques: %d", b.nch);
        FILE *f = fopen(P("big_utf8.txt"), "rb"); char line[8192]; struct bk_buf ex = { 0 };
        while (fgets(line, sizeof line, f)) {
            int l = (int)strlen(line); while (l && line[l - 1] == '\n') l--;
            if (!strncmp(line, "Chapter ", 8)) bb_c(&ex, BKC_HEAD);
            bb_put(&ex, line, l); bb_c(&ex, '\n');
        }
        fclose(f);
        CHECK(ex.n == n && !memcmp(ex.b, t, n), "bloques concatenados (%d vs %d)", n, ex.n);
        CHECK(m.n == 12, "12 cabeceras: %d", m.n);
        for (int i = 0; i + 1 < b.nch; i++) CHECK(b.ch[i].r1 == b.ch[i + 1].r0, "bloques contiguos");
        int tot = 0; double t0 = ms(); struct bk_buf o = { 0 };
        struct bk_lp L; bk_lp_make(&L, W - 48, H - 76, 17, 1);
        for (int i = 0; i < b.nch; i++) { bk_flow(&b, i, &o, 0, 0); int *pg; tot += bk_paginate(&L, o.b, o.n, adv, 0, &pg); free(pg); }
        printf("  big_utf8: %d bloques, %d paginas, %.1f ms (flujo + maquetacion)\n", b.nch, tot, ms() - t0);
        bb_free(&o); bb_free(&ex); free(t); bk_close(&b);
    }
}

/* textos de la app (lang/books_tr.inc.c): 13 idiomas, mismos %d, glifos en las fuentes, tr() los usa */
static void fmt_of(const char *s, char *o) { int k = 0; for (; *s && k < 15; s++) if (*s == '%' && s[1]) { o[k++] = s[1]; s++; } o[k] = 0; }
static void test_i18n(void)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout;
    static const char *const used[] = { "Biblioteca", "%d de %d", "Eliminar", "Eliminado", "No se pudo eliminar del todo", "La SD está conectada al ordenador",
                                        "¿Eliminar «%s»? No se puede deshacer.", 0 };
    for (unsigned i = 0; i < sizeof BTR / sizeof BTR[0]; i++) {
        char f0[16], f1[16]; fmt_of(BTR[i][1], f0);
        for (int j = 0; j < (int)i; j++) CHECK(strcmp(BTR[j][0], BTR[i][0]), "clave repetida %s", BTR[i][0]);
        for (int l = 0; l < LANG_N; l++) {
            const char *v = BTR[i][1 + l];
            CHECK(v && *v, "[%s] sin traduccion: %s", LANG_CODES[l], BTR[i][0]);
            if (!v) continue;
            fmt_of(v, f1); CHECK(!strcmp(f0, f1), "[%s] formato: %s", LANG_CODES[l], v);
            ui_lang = l; CHECK(!strcmp(tr(BTR[i][0]), v), "[%s] tr() no usa BTR: %s", LANG_CODES[l], BTR[i][0]);
            const char *s = v; int c;
            while ((c = utf8_next(&s))) { if (c <= ' ' || c == 0xA0) continue; int gi; font_glyph(c, &gi); CHECK(gi, "[%s] sin glifo U+%04X en %s", LANG_CODES[l], c, v); }
            if (strncmp(BTR[i][0], "BOOKS_", 6)) CHECK(text_width(v, 15) < 280, "[%s] demasiado ancho: %s", LANG_CODES[l], v);
        }
    }
    for (int i = 0; used[i]; i++) for (int l = 0; l < LANG_N; l++) { ui_lang = l; CHECK(tr(used[i]) != used[i] || l == LANG_ES, "[%s] falta %s", LANG_CODES[l], used[i]); }
    ui_lang = LANG_ES;
}

/* paginas reales a PNG (mismo codigo de dibujo que el aparato) para revisarlas a ojo */
static const char *PNGDIR;
static void page_png(const char *name, const char *book, int ch, int page, int size, uint16_t bg, uint16_t fg, uint16_t dim, int latin)
{
    if (!PNGDIR) return;
    struct bk_book b; if (bk_open(&b, P(book), 0)) return;
    struct bk_buf o = { 0 }; bk_flow(&b, ch, &o, 0, 0);
    struct bk_lp L; bk_lp_make(&L, W - 48, H - 38 - 42, size, 1);
    bk_latinq = latin; memset(&bk_uiac, 0, sizeof bk_uiac); bk_qn = 0;
    int *pg; int np = bk_paginate(&L, o.b, o.n, bk_adv_ui, 0, &pg);
    if (page < np) {
        static uint16_t d[W * H]; static uint8_t rgbb[W * H * 3];
        for (int i = 0; i < W * H; i++) d[i] = bg;
        double t0 = ms();
        bk_draw_text_page(d, &L, o.b, o.n, pg[page], 24, 38, fg, dim);
        double dt = ms() - t0;
        t0 = ms(); for (int i = 0; i < W * H; i++) d[i] = bg; bk_draw_text_page(d, &L, o.b, o.n, pg[page], 24, 38, fg, dim);
        printf("  png %-14s pagina %d/%d: %.2f ms (glifos nuevos) / %.2f ms (en cache)\n", name, page + 1, np, dt, ms() - t0);
        for (int i = 0; i < W * H; i++) { uint16_t p = d[i]; rgbb[i * 3] = (p >> 11) * 255 / 31; rgbb[i * 3 + 1] = ((p >> 5) & 63) * 255 / 63; rgbb[i * 3 + 2] = (p & 31) * 255 / 31; }
        char fn[400]; snprintf(fn, sizeof fn, "%s/%s.png", PNGDIR, name);
        stbi_write_png(fn, W, H, 3, rgbb, W * 3);
    }
    free(pg); bb_free(&o); bk_close(&b);
}
static void test_png(void)
{
    page_png("alice_ch1", "alice3.epub", 1, 0, 17, rgb(255, 255, 255), rgb(22, 22, 24), rgb(142, 142, 147), 1);
    page_png("big_p2_sepia", "alice3.epub", 3, 1, 17, rgb(248, 241, 227), rgb(79, 50, 28), rgb(160, 136, 110), 1);
    page_png("quijote_noche", "alice2.epub", 0, 0, 19, rgb(18, 18, 18), rgb(212, 212, 214), rgb(118, 118, 124), 1);
    page_png("quijote_p2", "alice2.epub", 0, 1, 19, rgb(18, 18, 18), rgb(212, 212, 214), rgb(118, 118, 124), 1);
    page_png("lunyu_gbk", "lunyu_gbk.txt", 0, 0, 17, rgb(255, 255, 255), rgb(22, 22, 24), rgb(142, 142, 147), 0);
    page_png("alice_txt_27", "alice_gutenberg.txt", 0, 0, 27, rgb(255, 255, 255), rgb(22, 22, 24), rgb(142, 142, 147), 1);
    page_png("chekhov_14", "chekhov_cp1251.txt", 0, 0, 14, rgb(248, 241, 227), rgb(79, 50, 28), rgb(160, 136, 110), 1);
}

int main(int argc, char **argv)
{
    PNGDIR = getenv("BOOKS_PNG");
    D = argc > 1 ? argv[1] : "/tmp/books_samples";
    crc_init();
    if (!text_init()) { printf("no se pudo cargar default.otf (TEXT_FONT_DIRS)\n"); return 2; }
    font_glyph(0x10FFFD, &(int){ 0 });   /* como el shell: mapear las de reserva antes de los hilos */
    test_epub3();
    test_epub2();
    test_broken();
    test_txt();
    test_i18n();
    test_png();
    printf("%d bien, %d fallos\n", passes, fails);
    return fails ? 1 : 0;
}
