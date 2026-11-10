/* tags_test.c - pruebas en PC (x86, WSL) de hibytag: todas las etiquetas (ht_read_all /
 * ht_write_kv), renombrar canciones y carpetas y las rutas de la base (ht_db_rename).
 * Las muestras se generan aqui mismo (FLAC con PICTURE y comentarios repetidos, MP3 ID3v2.3 y
 * v2.4 con APIC, COMM con idioma, USLT, TXXX, PRIV e ID3v1, M4A con ilst) y en cada escritura se
 * comprueba, con un lector independiente del de hibytag, que el audio, las imagenes y las tramas
 * no tocadas quedan byte a byte, que la estructura sigue siendo valida y que no quedan temporales.
 * Ejecutar: bash tests/tags_test.sh */
#define HT_API static
#include "hibytag.c"
#include <dirent.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char D[512];                       /* carpeta de trabajo */

/* ------------------------------------------------------------------ utilidades */
typedef struct { unsigned char *p; size_t n, cap; } buf;
static void b_add(buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void b_str(buf *b, const char *s) { b_add(b, s, strlen(s)); }
static void b_u8(buf *b, int v) { unsigned char c = (unsigned char)v; b_add(b, &c, 1); }
static void b_be32(buf *b, uint32_t v) { unsigned char c[4]; htx_put_be32(c, v); b_add(b, c, 4); }
static void b_le32(buf *b, uint32_t v) { unsigned char c[4]; htx_put_le32(c, v); b_add(b, c, 4); }
static void b_ss32(buf *b, uint32_t v) { unsigned char c[4]; htx_put_ss32(c, v); b_add(b, c, 4); }
static void b_rand(buf *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b_u8(b, (int)(seed >> 16)); }
}
static void put_file(const char *path, const buf *b)
{
    FILE *f = fopen(path, "wb"); fwrite(b->p, 1, b->n, f); fclose(f);
}
static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); if (!f) { *n = 0; return 0; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(l + 1); *n = fread(p, 1, l, f); fclose(f);
    return p;
}
static const unsigned char *memfind(const unsigned char *h, size_t hn, const unsigned char *nd, size_t nn)
{
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) if (h[i] == nd[0] && !memcmp(h + i, nd, nn)) return h + i;
    return 0;
}
static int no_tmp_left(void)
{
    DIR *d = opendir(D); struct dirent *e; int bad = 0;
    while (d && (e = readdir(d))) if (strstr(e->d_name, ".hibytag-")) { printf("  temporal huerfano: %s\n", e->d_name); bad = 1; }
    if (d) closedir(d);
    return !bad;
}
static const char *val(const ht_all *a, const char *k) { const ht_item *it = ht_all_find(a, k); return it ? it->val : 0; }
static int flags(const ht_all *a, const char *k) { const ht_item *it = ht_all_find(a, k); return it ? it->flags : -1; }
static int streq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
#define CHECK_VAL(a, k, v) do { const char *_g = val(a, k); CHECK(streq(_g, v), "%s: esperado \"%s\", leido \"%s\"", k, v, _g ? _g : "(nada)"); } while (0)

/* ------------------------------------------------------------------ FLAC */
static buf audio_flac, pic_jpeg;          /* audio y portada de referencia */
static void flac_block(buf *b, int type, int last, const buf *body)
{
    b_u8(b, type | (last ? 0x80 : 0)); b_u8(b, (int)(body->n >> 16)); b_u8(b, (int)(body->n >> 8)); b_u8(b, (int)body->n);
    b_add(b, body->p, body->n);
}
/* comentarios: lista "CLAVE=valor" terminada en 0; pad = bytes de PADDING (0 = sin bloque) */
static void make_flac(const char *path, const char *const *cm, int pad, int pic_first)
{
    buf f = { 0 }, si = { 0 }, vc = { 0 }, pc = { 0 }, pd = { 0 };
    b_str(&f, "fLaC");
    b_rand(&si, 34, 7);
    flac_block(&f, 0, 0, &si);
    b_le32(&vc, 9); b_str(&vc, "reference");
    int n = 0; while (cm[n]) n++;
    b_le32(&vc, n);
    for (int i = 0; i < n; i++) { b_le32(&vc, strlen(cm[i])); b_str(&vc, cm[i]); }
    b_be32(&pc, 3); b_be32(&pc, 10); b_str(&pc, "image/jpeg"); b_be32(&pc, 0); b_be32(&pc, 500); b_be32(&pc, 500);
    b_be32(&pc, 24); b_be32(&pc, 0); b_be32(&pc, pic_jpeg.n); b_add(&pc, pic_jpeg.p, pic_jpeg.n);
    if (pic_first) { flac_block(&f, 6, 0, &pc); flac_block(&f, 4, !pad, &vc); }
    else { flac_block(&f, 4, 0, &vc); flac_block(&f, 6, !pad, &pc); }
    if (pad) { for (int i = 0; i < pad; i++) b_u8(&pd, 0); flac_block(&f, 1, 1, &pd); }
    b_add(&f, audio_flac.p, audio_flac.n);
    put_file(path, &f);
    free(f.p); free(si.p); free(vc.p); free(pc.p); free(pd.p);
}
/* lector independiente: valida la cadena de bloques; devuelve el desplazamiento del audio y
 * copia STREAMINFO y la imagen del PICTURE tipo 3 */
static long flac_check(const char *path, int *npic, int *nvc)
{
    size_t n; unsigned char *p = slurp(path, &n);
    long r = -1; size_t off = 4; int last = 0, nb = 0;
    *npic = *nvc = 0;
    if (!p || n < 8 || memcmp(p, "fLaC", 4)) goto out;
    while (!last) {
        if (off + 4 > n) goto out;
        int t = p[off] & 0x7F; last = p[off] >> 7; uint32_t l = htx_be24(p + off + 1);
        if (off + 4 + l > n) goto out;
        if (nb == 0 && (t != 0 || l != 34)) goto out;
        if (t == 6) {
            if (memcmp(p + off + 4, "\0\0\0\3", 4)) goto out;
            if (!memfind(p + off + 4, l, pic_jpeg.p, pic_jpeg.n)) goto out;
            (*npic)++;
        }
        if (t == 4) {                                 /* comentario: recuento coherente con la longitud */
            const unsigned char *q = p + off + 4, *e = q + l;
            uint32_t vl = htx_le32(q); q += 4 + vl; if (q + 4 > e) goto out;
            uint32_t cnt = htx_le32(q); q += 4;
            for (uint32_t i = 0; i < cnt; i++) { if (q + 4 > e) goto out; uint32_t cl = htx_le32(q); q += 4 + cl; if (q > e) goto out; }
            if (q != e) goto out;
            (*nvc)++;
        }
        off += 4 + l; nb++;
    }
    if (n - off != audio_flac.n || memcmp(p + off, audio_flac.p, audio_flac.n)) goto out;
    {
        size_t m0; unsigned char *ref = slurp(path, &m0);    /* STREAMINFO intacto */
        buf si = { 0 }; b_rand(&si, 34, 7);
        if (memcmp(ref + 8, si.p, 34)) { free(ref); free(si.p); goto out; }
        free(ref); free(si.p);
    }
    r = (long)off;
out:
    free(p);
    return r;
}

static void test_flac(void)
{
    char a[600]; snprintf(a, sizeof a, "%s/a.flac", D);
    static const char *const cm[] = {
        "TITLE=Canción ñandú 日本", "ARTIST=Uno", "ARTIST=Dos", "artist=Tres", "ALBUM=Disco", "DATE=2008",
        "TRACKNUMBER=3", "TOTALTRACKS=12", "COMPOSER=Bach", "COMMENT=hola", "REPLAYGAIN_TRACK_GAIN=-6.5 dB",
        "MUSICBRAINZ_ALBUMID=abc-123", "Custom Thing=x", "METADATA_BLOCK_PICTURE=AAAA", "LYRICS=línea 1\nlínea 2", 0 };
    make_flac(a, cm, 4096, 0);
    ht_all t;
    CHECK(ht_read_all(a, &t) == HT_OK, "read_all flac");
    CHECK(t.fmt == HT_FMT_FLAC, "fmt");
    CHECK_VAL(&t, "TITLE", "Canción ñandú 日本");
    CHECK_VAL(&t, "ARTIST", "Uno; Dos; Tres");
    CHECK(flags(&t, "ARTIST") & HT_ITEM_MULTI, "ARTIST con varios valores");
    CHECK_VAL(&t, "TRACKTOTAL", "12");
    CHECK_VAL(&t, "totaltracks", "12");
    CHECK(streq(ht_all_find(&t, "TRACKTOTAL")->orig, "TOTALTRACKS"), "orig TOTALTRACKS");
    CHECK(flags(&t, "REPLAYGAIN_TRACK_GAIN") & HT_ITEM_RO, "replaygain solo lectura");
    CHECK_VAL(&t, "CUSTOM THING", "x");
    CHECK(!ht_all_find(&t, "METADATA_BLOCK_PICTURE"), "imagen base64 oculta");
    CHECK_VAL(&t, "LYRICS", "línea 1\nlínea 2");
    CHECK_VAL(&t, "UNSYNCEDLYRICS", "línea 1\nlínea 2");
    ht_all_free(&t);
    /* lectura de siempre (8 campos) sigue igual */
    ht_tags lt; ht_read(a, &lt);
    CHECK(streq(lt.v[HT_ARTIST], "Uno; Dos; Tres") && streq(lt.v[HT_YEAR], "2008") && streq(lt.v[HT_TRACK], "3"), "ht_read legado");
    ht_tags_free(&lt);

    /* editar otros campos: en sitio (cabe en el PADDING); ARTIST repetido intacto byte a byte */
    ht_kv kv[] = { { "composer", "Johann Sebastian Bach" }, { "LABEL", "Sello ñ" }, { "COMMENT", "" },
                   { "TRACKTOTAL", "13" }, { "Custom Thing", "y" }, { "TITLE", 0 } };
    int mode, npic, nvc;
    CHECK(ht_write_kv(a, kv, 6, 0, 0, &mode) == HT_OK, "write_kv flac");
    CHECK(mode == HT_MODE_INPLACE, "en sitio (modo %d)", mode);
    CHECK(flac_check(a, &npic, &nvc) > 0 && npic == 1 && nvc == 1, "estructura/audio/portada flac");
    size_t n; unsigned char *p = slurp(a, &n);
    CHECK(memfind(p, n, (const unsigned char *)"\x0a\0\0\0ARTIST=Uno", 14) && memfind(p, n, (const unsigned char *)"artist=Tres", 11), "ARTIST repetidos intactos");
    CHECK(memfind(p, n, (const unsigned char *)"TOTALTRACKS=13", 14) && !memfind(p, n, (const unsigned char *)"TRACKTOTAL=", 11), "se conserva la forma TOTALTRACKS");
    CHECK(memfind(p, n, (const unsigned char *)"Custom Thing=y", 14), "clave libre con su forma original");
    CHECK(memfind(p, n, (const unsigned char *)"METADATA_BLOCK_PICTURE=AAAA", 27), "imagen base64 intacta");
    free(p);
    CHECK(ht_read_all(a, &t) == HT_OK, "relectura");
    CHECK_VAL(&t, "COMPOSER", "Johann Sebastian Bach");
    CHECK_VAL(&t, "LABEL", "Sello ñ");
    CHECK(!ht_all_find(&t, "COMMENT"), "COMMENT borrado");
    CHECK_VAL(&t, "TRACKTOTAL", "13");
    CHECK_VAL(&t, "TITLE", "Canción ñandú 日本");
    CHECK_VAL(&t, "ARTIST", "Uno; Dos; Tres");
    ht_all_free(&t);

    /* letra larga: no cabe en el relleno -> reescritura completa (temporal + rename) */
    char *big = malloc(20001); for (int i = 0; i < 20000; i++) big[i] = i % 61 == 60 ? '\n' : 'a' + i % 26; big[20000] = 0;
    ht_kv kl[] = { { "LYRICS", big }, { "ARTIST", "Solo uno" } };
    CHECK(ht_write_kv(a, kl, 2, 0, 0, &mode) == HT_OK && mode == HT_MODE_REWRITE, "letra larga: reescritura (modo %d)", mode);
    CHECK(flac_check(a, &npic, &nvc) > 0 && npic == 1 && nvc == 1, "estructura tras reescritura");
    CHECK(no_tmp_left(), "sin temporales");
    CHECK(ht_read_all(a, &t) == HT_OK, "relectura 2");
    CHECK_VAL(&t, "LYRICS", big);
    CHECK_VAL(&t, "ARTIST", "Solo uno");
    CHECK(!(flags(&t, "ARTIST") & HT_ITEM_MULTI), "ARTIST ya no es multiple");
    ht_all_free(&t);
    /* y la siguiente edicion vuelve a caber en sitio */
    ht_kv k3[] = { { "BPM", "120" } };
    CHECK(ht_write_kv(a, k3, 1, 0, 0, &mode) == HT_OK && mode == HT_MODE_INPLACE, "tras reescritura: en sitio");
    CHECK(flac_check(a, &npic, &nvc) > 0, "estructura");

    /* fallo simulado en mitad de la reescritura: el original queda intacto */
    size_t n0; unsigned char *orig = slurp(a, &n0);
    big[19999] = 'Z';
    ht_kv k4[] = { { "LYRICS", big }, { "DESCRIPTION", big } };
    ht_test_fail_at = 70000;
    CHECK(ht_write_kv(a, k4, 2, 0, 0, &mode) == HT_EIO, "fallo simulado");
    ht_test_fail_at = 0;
    size_t n1; unsigned char *now = slurp(a, &n1);
    CHECK(n0 == n1 && !memcmp(orig, now, n0), "original intacto tras el fallo");
    CHECK(no_tmp_left(), "sin temporales tras el fallo");
    free(orig); free(now);
    /* clave no valida: nada cambia */
    ht_kv k5[] = { { "BAD=KEY", "x" } };
    CHECK(ht_write_kv(a, k5, 1, 0, 0, &mode) == HT_EKEY, "clave no valida");
    ht_kv k6[] = { { "METADATA_BLOCK_PICTURE", "x" } };
    CHECK(ht_write_kv(a, k6, 1, 0, 0, &mode) == HT_EKEY, "imagen por clave: no");
    free(big);

    /* comentario detras de una portada grande, PADDING al final: el comentario se muda al relleno */
    char b2[600]; snprintf(b2, sizeof b2, "%s/b.flac", D);
    static const char *const cm2[] = { "TITLE=x", 0 };
    make_flac(b2, cm2, 2048, 1);
    ht_kv k7[] = { { "COMPOSER", "Ñ" }, { "LYRICS", "1\n2\n3" } };
    CHECK(ht_write_kv(b2, k7, 2, 0, 0, &mode) == HT_OK && mode == HT_MODE_INPLACE, "en sitio con portada delante");
    CHECK(flac_check(b2, &npic, &nvc) > 0 && npic == 1 && nvc == 1, "estructura (portada delante)");
    /* sin PADDING */
    make_flac(b2, cm2, 0, 0);
    CHECK(ht_write_kv(b2, k7, 2, 0, 0, &mode) == HT_OK && mode == HT_MODE_REWRITE, "sin relleno: reescritura");
    CHECK(flac_check(b2, &npic, &nvc) > 0 && npic == 1 && nvc == 1, "estructura (sin relleno)");
    CHECK(ht_read_all(b2, &t) == HT_OK, "relectura b");
    CHECK_VAL(&t, "LYRICS", "1\n2\n3"); CHECK_VAL(&t, "TITLE", "x");
    ht_all_free(&t);
}

/* ------------------------------------------------------------------ MP3 */
static buf audio_mp3;
static void id3_frame(buf *b, int ver, const char *id, const buf *body, int flags2)
{
    b_str(b, id);
    if (ver == 4) b_ss32(b, body->n); else b_be32(b, body->n);
    b_u8(b, 0); b_u8(b, flags2);
    b_add(b, body->p, body->n);
}
static void id3_text(buf *b, int ver, const char *id, int enc, const char *s)
{
    buf t = { 0 };
    b_u8(&t, enc);
    if (enc == 1) {            /* UTF-16 con BOM desde UTF-8 */
        b_u8(&t, 0xFF); b_u8(&t, 0xFE);
        const unsigned char *p = (const unsigned char *)s, *e = p + strlen(s);
        while (p < e) { uint32_t u = htx_u8(&p, e); b_u8(&t, u & 0xFF); b_u8(&t, u >> 8); }
    } else b_str(&t, s);
    id3_frame(b, ver, id, &t, 0);
    free(t.p);
}
/* COMM/USLT (lang + desc + texto) o TXXX (desc + valor), en Latin-1/UTF-8 */
static void id3_desc(buf *b, int ver, const char *id, const char *lang, const char *desc, const char *s)
{
    buf t = { 0 };
    b_u8(&t, ver == 4 ? 3 : 0);
    if (lang) b_str(&t, lang);
    b_str(&t, desc); b_u8(&t, 0); b_str(&t, s);
    id3_frame(b, ver, id, &t, 0);
    free(t.p);
}
static void make_mp3(const char *path, int ver, int pad, int v1)
{
    buf fr = { 0 }, f = { 0 }, ap = { 0 }, pv = { 0 };
    id3_text(&fr, ver, "TIT2", 0, "Titulo");
    id3_text(&fr, ver, "TPE1", ver == 4 ? 3 : 1, "Ærtist Ω");
    id3_text(&fr, ver, "TRCK", 0, "3/12");
    id3_text(&fr, ver, "TPOS", 0, "1/2");
    id3_text(&fr, ver, ver == 4 ? "TDRC" : "TYER", 0, "1999");
    id3_text(&fr, ver, "TCON", 0, "(17)");
    id3_text(&fr, ver, "TLEN", 0, "123000");
    id3_desc(&fr, ver, "COMM", "spa", "", "un comentario");
    id3_desc(&fr, ver, "COMM", "eng", "iTunNORM", " 0000 0000");
    id3_desc(&fr, ver, "TXXX", 0, "CATALOGNUMBER", "CAT-1");
    id3_desc(&fr, ver, "TXXX", 0, "MusicBrainz Album Id", "mb-1");
    id3_desc(&fr, ver, "USLT", "eng", "", "verso uno\nverso dos");
    b_u8(&ap, 0); b_str(&ap, "image/jpeg"); b_u8(&ap, 0); b_u8(&ap, 3); b_u8(&ap, 0); b_add(&ap, pic_jpeg.p, pic_jpeg.n);
    id3_frame(&fr, ver, "APIC", &ap, 0);
    b_str(&pv, "owner@x"); b_u8(&pv, 0); b_rand(&pv, 50, 99);
    id3_frame(&fr, ver, "PRIV", &pv, 0);
    b_str(&f, "ID3"); b_u8(&f, ver); b_u8(&f, 0); b_u8(&f, 0); b_ss32(&f, fr.n + pad);
    b_add(&f, fr.p, fr.n);
    for (int i = 0; i < pad; i++) b_u8(&f, 0);
    b_add(&f, audio_mp3.p, audio_mp3.n);
    if (v1) {
        unsigned char t[128] = "TAG";
        memcpy(t + 3, "Titulo", 6); memcpy(t + 33, "Artist", 6); memcpy(t + 63, "Album v1", 8); memcpy(t + 93, "1999", 4);
        t[125] = 0; t[126] = 3; t[127] = 17;
        b_add(&f, t, 128);
    }
    put_file(path, &f);
    free(fr.p); free(f.p); free(ap.p); free(pv.p);
}
/* lector independiente: recorre las tramas, comprueba audio, APIC y PRIV byte a byte */
static int mp3_check(const char *path, int v1, int *napic)
{
    size_t n; unsigned char *p = slurp(path, &n);
    int ok = 0; *napic = 0;
    if (!p || n < 10 || memcmp(p, "ID3", 3)) goto out;
    int ver = p[3]; uint32_t sz = htx_ss32(p + 6); size_t end = 10 + sz, off = 10;
    if (end > n) goto out;
    while (off + 10 <= end && p[off]) {
        uint32_t fs = ver == 4 ? htx_ss32(p + off + 4) : htx_be32(p + off + 4);
        if (off + 10 + fs > end) goto out;
        for (int k = 0; k < 4; k++) if (!((p[off + k] >= 'A' && p[off + k] <= 'Z') || (p[off + k] >= '0' && p[off + k] <= '9'))) goto out;
        if (!memcmp(p + off, "APIC", 4)) { if (!memfind(p + off + 10, fs, pic_jpeg.p, pic_jpeg.n)) goto out; (*napic)++; }
        off += 10 + fs;
    }
    for (; off < end; off++) if (p[off]) goto out;               /* relleno a ceros */
    size_t an = n - end - (v1 ? 128 : 0);
    if (an != audio_mp3.n || memcmp(p + end, audio_mp3.p, an)) goto out;
    if (v1 && memcmp(p + n - 128, "TAG", 3)) goto out;
    buf pv = { 0 }; b_str(&pv, "owner@x"); b_u8(&pv, 0); b_rand(&pv, 50, 99);
    if (!memfind(p, end, pv.p, pv.n)) { free(pv.p); goto out; }
    free(pv.p);
    ok = 1;
out:
    free(p);
    return ok;
}
static int count_frames(const char *path, const char *id)
{
    size_t n; unsigned char *p = slurp(path, &n); int c = 0;
    int ver = p[3]; size_t end = 10 + htx_ss32(p + 6), off = 10;
    while (off + 10 <= end && p[off]) { uint32_t fs = ver == 4 ? htx_ss32(p + off + 4) : htx_be32(p + off + 4); if (!memcmp(p + off, id, 4)) c++; off += 10 + fs; }
    free(p);
    return c;
}

static void test_mp3(int ver)
{
    char a[600]; snprintf(a, sizeof a, "%s/a%d.mp3", D, ver);
    make_mp3(a, ver, 2048, 1);
    ht_all t; int mode, nap;
    CHECK(ht_read_all(a, &t) == HT_OK && t.fmt == HT_FMT_MP3, "read_all mp3 v2.%d", ver);
    CHECK_VAL(&t, "TITLE", "Titulo");
    CHECK_VAL(&t, "ARTIST", "Ærtist Ω");
    CHECK_VAL(&t, "TRACKNUMBER", "3"); CHECK_VAL(&t, "TRACKTOTAL", "12");
    CHECK_VAL(&t, "DISCNUMBER", "1"); CHECK_VAL(&t, "DISCTOTAL", "2");
    CHECK_VAL(&t, "DATE", "1999");
    CHECK_VAL(&t, "GENRE", "Rock");
    CHECK_VAL(&t, "COMMENT", "un comentario");
    CHECK(flags(&t, "COMMENT:ITUNNORM") & HT_ITEM_RO, "iTunNORM solo lectura");
    CHECK_VAL(&t, "CATALOGNUMBER", "CAT-1");
    CHECK_VAL(&t, "MUSICBRAINZ_ALBUMID", "mb-1");
    CHECK_VAL(&t, "LYRICS", "verso uno\nverso dos");
    CHECK(flags(&t, "TLEN") & HT_ITEM_RO, "trama desconocida TLEN solo lectura");
    CHECK(!ht_all_find(&t, "APIC") && !ht_all_find(&t, "PRIV"), "APIC/PRIV no se listan");
    ht_all_free(&t);

    ht_kv kv[] = { { "TRACKNUMBER", "5" }, { "COMMENT", "otro comentario €" }, { "COMPOSER", "Žofie" },
                   { "MOOD", "Calm" }, { "CATALOGNUMBER", "" }, { "LYRICS", "nueva\nletra" }, { "MUSICBRAINZ_ALBUMID", "mb-2" },
                   { "TITLE", "Título nuevo" }, { "ISRC", "ES-ABC-12-34567" } };
    CHECK(ht_write_kv(a, kv, 9, 0, 0, &mode) == HT_OK, "write_kv mp3 v2.%d", ver);
    CHECK(mode == HT_MODE_INPLACE, "mp3 en sitio (modo %d)", mode);
    CHECK(mp3_check(a, 1, &nap) && nap == 1, "estructura/audio/APIC/PRIV mp3");
    CHECK(ht_read_all(a, &t) == HT_OK, "relectura mp3");
    CHECK_VAL(&t, "TRACKNUMBER", "5"); CHECK_VAL(&t, "TRACKTOTAL", "12");
    CHECK_VAL(&t, "COMMENT", "otro comentario €");
    CHECK_VAL(&t, "COMPOSER", "Žofie");
    CHECK_VAL(&t, "MOOD", "Calm");
    CHECK(!ht_all_find(&t, "CATALOGNUMBER"), "CATALOGNUMBER borrado");
    CHECK_VAL(&t, "LYRICS", "nueva\nletra");
    CHECK_VAL(&t, "MUSICBRAINZ_ALBUMID", "mb-2");
    CHECK_VAL(&t, "TITLE", "Título nuevo");
    CHECK_VAL(&t, "ISRC", "ES-ABC-12-34567");
    CHECK(flags(&t, "COMMENT:ITUNNORM") & HT_ITEM_RO, "iTunNORM sigue");
    CHECK_VAL(&t, "ARTIST", "Ærtist Ω");
    ht_all_free(&t);
    CHECK(count_frames(a, "COMM") == 2 && count_frames(a, "TRCK") == 1 && count_frames(a, "TXXX") == (ver == 4 ? 1 : 2), "tramas COMM/TRCK/TXXX");
    /* idioma del COMM conservado y descripcion de Picard conservada */
    size_t n; unsigned char *p = slurp(a, &n);
    CHECK(memfind(p, n, (const unsigned char *)"spa", 3) != 0, "idioma spa conservado");
    CHECK(memfind(p, n, (const unsigned char *)"MusicBrainz Album Id", 20) != 0, "descripcion TXXX conservada");
    CHECK(ver == 4 ? memfind(p, n, (const unsigned char *)"TMOO", 4) != 0 : memfind(p, n, (const unsigned char *)"MOOD\0Calm", 9) != 0, "MOOD: TMOO (v2.4) / TXXX (v2.3)");
    CHECK(!memcmp(p + n - 128 + 3, "T\xEDtulo nuevo", 12), "ID3v1 actualizado (Latin-1)");
    free(p);
    /* total aparte */
    ht_kv k2[] = { { "TRACKTOTAL", "14" }, { "DISCNUMBER", "2" } };
    CHECK(ht_write_kv(a, k2, 2, 0, 0, &mode) == HT_OK, "TRACKTOTAL mp3");
    CHECK(ht_read_all(a, &t) == HT_OK, "relectura 2");
    CHECK_VAL(&t, "TRACKNUMBER", "5"); CHECK_VAL(&t, "TRACKTOTAL", "14"); CHECK_VAL(&t, "DISCNUMBER", "2"); CHECK_VAL(&t, "DISCTOTAL", "2");
    ht_all_free(&t);
    /* legado: ht_write con pista sin total conserva el total */
    ht_changes ch; memset(&ch, 0, sizeof ch); ch.v[HT_TRACK] = "7"; ch.v[HT_YEAR] = "2001";
    CHECK(ht_write(a, &ch, 0, 0, &mode) == HT_OK, "ht_write legado");
    ht_tags lt; ht_read(a, &lt);
    CHECK(streq(lt.v[HT_TRACK], "7/14") && streq(lt.v[HT_YEAR], "2001"), "legado: pista %s ano %s", lt.v[HT_TRACK], lt.v[HT_YEAR]);
    ht_tags_free(&lt);
    CHECK(count_frames(a, ver == 4 ? "TDRC" : "TYER") == 1, "una sola trama de fecha");
    /* letra grande: reescritura (las tramas grandes van delante, el texto al final) */
    char *big = malloc(30001); for (int i = 0; i < 30000; i++) big[i] = i % 50 == 49 ? '\n' : 'A' + i % 26; big[30000] = 0;
    ht_kv k3[] = { { "LYRICS", big } };
    CHECK(ht_write_kv(a, k3, 1, 0, 0, &mode) == HT_OK && mode == HT_MODE_REWRITE, "letra grande mp3 (modo %d)", mode);
    CHECK(mp3_check(a, 1, &nap) && nap == 1, "estructura tras reescritura mp3");
    CHECK(no_tmp_left(), "sin temporales mp3");
    CHECK(ht_read_all(a, &t) == HT_OK, "relectura 3");
    CHECK_VAL(&t, "LYRICS", big);
    CHECK(!(flags(&t, "LYRICS") & HT_ITEM_TRUNC), "letra de 30000: entera");
    ht_all_free(&t);
    /* fallo simulado */
    size_t n0; unsigned char *orig = slurp(a, &n0);
    big[0] = 'z';
    ht_kv k3b[] = { { "LYRICS", big }, { "COMMENT:otro", big } };            /* no cabe: reescritura */
    ht_test_fail_at = 30000;
    CHECK(ht_write_kv(a, k3b, 2, 0, 0, &mode) == HT_EIO, "fallo simulado mp3");
    ht_test_fail_at = 0;
    size_t n1; unsigned char *now = slurp(a, &n1);
    CHECK(n0 == n1 && !memcmp(orig, now, n0), "mp3 intacto tras el fallo");
    free(orig); free(now); free(big);

    /* MP3 sin ID3v2 (solo ID3v1): se crea una v2.3 */
    char b[600]; snprintf(b, sizeof b, "%s/b%d.mp3", D, ver);
    { buf f = { 0 }; b_add(&f, audio_mp3.p, audio_mp3.n); unsigned char t1[128] = "TAG"; memcpy(t1 + 3, "Viejo", 5); t1[127] = 255; b_add(&f, t1, 128); put_file(b, &f); free(f.p); }
    ht_kv k4[] = { { "CONDUCTOR", "Karajan" }, { "TITLE", "Nuevo" } };
    CHECK(ht_write_kv(b, k4, 2, 0, 0, &mode) == HT_OK, "mp3 sin ID3v2");
    CHECK(ht_read_all(b, &t) == HT_OK, "relectura sin v2");
    CHECK_VAL(&t, "CONDUCTOR", "Karajan"); CHECK_VAL(&t, "TITLE", "Nuevo");
    ht_all_free(&t);
    { size_t m; unsigned char *q = slurp(b, &m); CHECK(m > 128 && !memcmp(q + m - 128, "TAGNuevo", 8) && !memcmp(q, "ID3\3", 4), "v2.3 nueva + v1"); free(q); }
}

/* ------------------------------------------------------------------ M4A (solo lectura) */
static void box(buf *b, const char *type, const buf *body)
{
    b_be32(b, 8 + body->n); b_str(b, type); b_add(b, body->p, body->n);
}
static void m4a_text(buf *il, const char *type, const char *s)
{
    buf d = { 0 }, a = { 0 };
    b_be32(&d, 1); b_be32(&d, 0); b_str(&d, s);
    box(&a, "data", &d); box(il, type, &a);
    free(d.p); free(a.p);
}
static void test_m4a(void)
{
    char a[600]; snprintf(a, sizeof a, "%s/a.m4a", D);
    buf il = { 0 }, ilst = { 0 }, meta = { 0 }, udta = { 0 }, moov = { 0 }, f = { 0 }, ft = { 0 };
    m4a_text(&il, "\xA9nam", "Título m4a");
    m4a_text(&il, "\xA9wrt", "Compositor");
    m4a_text(&il, "\xA9lyr", "letra\nm4a");
    { buf d = { 0 }, x = { 0 }; b_be32(&d, 0); b_be32(&d, 0); b_be32(&d, 4); b_be32(&d, 9 << 16); b_u8(&d, 0); b_u8(&d, 0);
      box(&x, "data", &d); box(&il, "trkn", &x); free(d.p); free(x.p); }
    { buf fr = { 0 }, mn = { 0 }, nm = { 0 }, d = { 0 }, x = { 0 };
      b_be32(&mn, 0); b_str(&mn, "com.apple.iTunes"); box(&fr, "mean", &mn);
      b_be32(&nm, 0); b_str(&nm, "LABEL"); box(&fr, "name", &nm);
      b_be32(&d, 1); b_be32(&d, 0); b_str(&d, "Sello m4a"); box(&fr, "data", &d);
      box(&il, "----", &fr); free(fr.p); free(mn.p); free(nm.p); free(d.p); free(x.p); }
    box(&ilst, "ilst", &il);
    b_be32(&meta, 0); b_add(&meta, ilst.p, ilst.n);
    { buf m2 = { 0 }; box(&m2, "meta", &meta); box(&udta, "udta", &m2); free(m2.p); }
    box(&moov, "moov", &udta);
    b_str(&ft, "M4A "); b_be32(&ft, 0); box(&f, "ftyp", &ft);
    b_add(&f, moov.p, moov.n);
    { buf md = { 0 }; b_rand(&md, 1000, 5); box(&f, "mdat", &md); free(md.p); }
    put_file(a, &f);
    ht_all t; int mode;
    CHECK(ht_read_all(a, &t) == HT_OK && t.fmt == HT_FMT_M4A, "read_all m4a");
    CHECK_VAL(&t, "TITLE", "Título m4a"); CHECK_VAL(&t, "COMPOSER", "Compositor"); CHECK_VAL(&t, "LYRICS", "letra\nm4a");
    CHECK_VAL(&t, "TRACKNUMBER", "4"); CHECK_VAL(&t, "TRACKTOTAL", "9"); CHECK_VAL(&t, "LABEL", "Sello m4a");
    CHECK(flags(&t, "TITLE") & HT_ITEM_RO, "m4a solo lectura");
    ht_all_free(&t);
    ht_kv kv[] = { { "TITLE", "x" } };
    size_t n0; unsigned char *o = slurp(a, &n0);
    CHECK(ht_write_kv(a, kv, 1, 0, 0, &mode) == HT_EUNSUP, "m4a: escribir no soportado");
    size_t n1; unsigned char *q = slurp(a, &n1);
    CHECK(n0 == n1 && !memcmp(o, q, n0), "m4a intacto");
    free(o); free(q);
    free(il.p); free(ilst.p); free(meta.p); free(udta.p); free(moov.p); free(f.p); free(ft.p);
}

/* ------------------------------------------------------------------ lotes */
static void test_batch(void)
{
    char p[3][600]; const char *pp[3];
    static const char *const cm[] = { "TITLE=t", "ALBUM=viejo", 0 };
    snprintf(p[0], 600, "%s/l0.flac", D); make_flac(p[0], cm, 1024, 0);
    snprintf(p[1], 600, "%s/l1.mp3", D); make_mp3(p[1], 3, 512, 0);
    snprintf(p[2], 600, "%s/l2.flac", D); make_flac(p[2], cm, 0, 0);
    for (int i = 0; i < 3; i++) pp[i] = p[i];
    ht_kv kv[] = { { "ALBUM", "Nuevo álbum" }, { "COMPOSER", "Varios" }, { "LABEL", "L" } };
    int res[3];
    CHECK(ht_batch_kv(pp, 3, kv, 3, 0, 0, res) == 0 && !res[0] && !res[1] && !res[2], "lote kv");
    for (int i = 0; i < 3; i++) {
        ht_all t; ht_read_all(pp[i], &t);
        CHECK_VAL(&t, "ALBUM", "Nuevo álbum"); CHECK_VAL(&t, "COMPOSER", "Varios"); CHECK_VAL(&t, "LABEL", "L");
        ht_all_free(&t);
    }
    int npic, nvc, nap;
    CHECK(flac_check(p[0], &npic, &nvc) > 0 && flac_check(p[2], &npic, &nvc) > 0 && mp3_check(p[1], 0, &nap), "lote: estructuras");
    ht_kv bad[] = { { "", "x" } };
    CHECK(ht_batch_kv(pp, 3, bad, 1, 0, 0, res) == 3 && res[0] == HT_EKEY, "lote con clave no valida");
}

/* ------------------------------------------------------------------ nombres */
static void test_names(void)
{
    CHECK(ht_name_valid("01 - Canción.flac"), "nombre valido");
    CHECK(ht_name_valid("日本語 ñ.mp3"), "nombre unicode");
    static const char *const bad[] = { "", ".", "..", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "x.", "x ", " x", "a\tb", 0 };
    for (int i = 0; bad[i]; i++) CHECK(!ht_name_valid(bad[i]), "nombre no valido aceptado: \"%s\"", bad[i]);
    char o[256];
    ht_name_clean("AC/DC: Live? \"Best\" <1> | x...", o, sizeof o);
    CHECK(streq(o, "AC_DC_ Live_ _Best_ _1_ _ x"), "limpiar: \"%s\"", o);
    ht_name_clean("  ...  ", o, sizeof o); CHECK(streq(o, "_"), "limpiar vacio: \"%s\"", o);
    ht_name_clean("a\nb", o, sizeof o); CHECK(streq(o, "a b"), "limpiar salto: \"%s\"", o);
    CHECK(ht_pattern_name(HT_PAT_NUM_TITLE, "3/12", "1", 0, "Art", "Tema", ".flac", o, sizeof o) && streq(o, "03 - Tema.flac"), "patron 1: %s", o);
    CHECK(ht_pattern_name(HT_PAT_NUM_TITLE, "3", "2/2", 1, "Art", "Tema", ".flac", o, sizeof o) && streq(o, "2-03 - Tema.flac"), "patron multidisco: %s", o);
    CHECK(ht_pattern_name(HT_PAT_ARTIST_TITLE, "3", "1", 0, "AC/DC", "T?", ".mp3", o, sizeof o) && streq(o, "AC_DC - T_.mp3"), "patron 2: %s", o);
    CHECK(ht_pattern_name(HT_PAT_NUMTITLE, "12", 0, 0, 0, "Tema", ".mp3", o, sizeof o) && streq(o, "12 Tema.mp3"), "patron 3: %s", o);
    CHECK(ht_pattern_name(HT_PAT_NUMTITLE, 0, 0, 0, 0, "Tema", ".mp3", o, sizeof o) && streq(o, "Tema.mp3"), "patron sin pista: %s", o);
    CHECK(!ht_pattern_name(HT_PAT_NUM_TITLE, "1", 0, 0, 0, "", ".mp3", o, sizeof o), "sin titulo: no");
    char k[64]; ht_key_canon(" album artist ", k, sizeof k); CHECK(streq(k, "ALBUMARTIST"), "canon %s", k);
    ht_key_canon("MusicBrainz Album Id", k, sizeof k); CHECK(streq(k, "MUSICBRAINZ_ALBUMID"), "canon %s", k);
    CHECK(!ht_key_valid("A=B") && !ht_key_valid("") && !ht_key_valid(" X") && ht_key_valid("COMMENT:desc"), "claves validas");
}

/* ------------------------------------------------------------------ renombrar + base */
static void touch(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int exists(const char *p) { return !access(p, F_OK); }
static void test_rename(void)
{
    char root[600], alb[700], art[700], s1[800], s2[800], l1[800], l2[800], cv[800];
    snprintf(root, sizeof root, "%s/sd", D); mkdir(root, 0755);
    snprintf(art, sizeof art, "%s/Viejo Artista", root); mkdir(art, 0755);
    snprintf(alb, sizeof alb, "%s/Disco", art); mkdir(alb, 0755);
    snprintf(s1, sizeof s1, "%s/1-01 a.flac", alb); touch(s1, "audio1");
    snprintf(l1, sizeof l1, "%s/1-01 a.lrc", alb); touch(l1, "[00:01]x");
    snprintf(cv, sizeof cv, "%s/cover.jpg", alb); touch(cv, "jpg");
    snprintf(s2, sizeof s2, "%s/01 - Uno.flac", alb);
    snprintf(l2, sizeof l2, "%s/01 - Uno.lrc", alb);
    CHECK(ht_rename_song(s1, s2) == HT_OK && exists(s2) && exists(l2) && !exists(s1) && !exists(l1), "renombrar cancion + lrc");
    char s3[800]; snprintf(s3, sizeof s3, "%s/otra.flac", alb); touch(s3, "audio3");
    CHECK(ht_rename_song(s3, s2) == HT_EEXIST && exists(s3), "destino existente");
    char l3[800]; snprintf(l3, sizeof l3, "%s/nuevo.lrc", alb); touch(l3, "lrc ajeno");
    char s4[800]; snprintf(s4, sizeof s4, "%s/nuevo.flac", alb);
    CHECK(ht_rename_song(s2, s4) == HT_EEXIST && exists(s2) && exists(l2), "lrc de destino existente: nada se mueve");
    unlink(l3);
    char bad[800]; snprintf(bad, sizeof bad, "%s/a:b.flac", alb);
    CHECK(ht_rename_song(s3, bad) == HT_ENAME && exists(s3), "nombre no valido");

    /* base: rutas de ficheros y de carpeta */
    char db[600]; snprintf(db, sizeof db, "%s/media.db", D); unlink(db);
    sqlite3 *h; sqlite3_open(db, &h);
    static const char *cols = "(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,"
        "genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,"
        "size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,"
        "lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id))";
    char sql[2048];
    const char *tabs[] = { "MEDIA_TABLE", "HISTORY_TABLE", "list_tb_1" };
    for (int i = 0; i < 3; i++) { snprintf(sql, sizeof sql, "CREATE TABLE %s%s", tabs[i], cols); sqlite3_exec(h, sql, 0, 0, 0); }
    sqlite3_exec(h, "CREATE TABLE ALBUM_TABLE(id INT, album TEXT COLLATE NOCASE,character TEXT, cn INT, ctime INT, mtime INT, mqa INT, pinyin_charater TEXT, PRIMARY KEY(album))", 0, 0, 0);
    sqlite3_exec(h, "CREATE TABLE M3U_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,PRIMARY KEY(path))", 0, 0, 0);
    /* filas como las guarda el reproductor: ruta con su NUL final */
    struct { int id; const char *p; } rows[] = { { 1, "a:\\Viejo Artista\\Disco\\1-01 a.flac" }, { 2, "a:\\viejo artista\\disco\\otra.flac" },
                                                 { 3, "a:\\Otro\\x.flac" } };
    for (int t = 0; t < 3; t++) for (int i = 0; i < 3; i++) {
        sqlite3_stmt *q; snprintf(sql, sizeof sql, "INSERT INTO %s(id,path,name,album,artist,cue_id,lrc_path) VALUES(?,?,'n','al','ar',-1,?)", tabs[t]);
        sqlite3_prepare_v2(h, sql, -1, &q, 0); sqlite3_bind_int(q, 1, rows[i].id);
        sqlite3_bind_text(q, 2, rows[i].p, (int)strlen(rows[i].p) + 1, SQLITE_TRANSIENT);
        char lp[300]; htx_db_lrc(rows[i].p, lp, sizeof lp);
        sqlite3_bind_text(q, 3, i == 0 ? lp : "", i == 0 ? (int)strlen(lp) + 1 : 1, SQLITE_TRANSIENT);
        sqlite3_step(q); sqlite3_finalize(q);
    }
    sqlite3_exec(h, "INSERT INTO M3U_TABLE VALUES(1,'a:\\Viejo Artista\\Disco\\l.m3u',  'l','L')", 0, 0, 0);
    setenv("HIBYTAG_SD", root, 1);
    const char *f1[1] = { s1 }, *t1[1] = { s2 };
    int nr = 0;
    CHECK(ht_db_rename(h, f1, t1, 1, &nr) == HT_OK && nr == 6, "db: renombrar fichero (%d filas)", nr);
    sqlite3_stmt *q; int c = 0;
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM MEDIA_TABLE WHERE path=? AND lrc_path=?", -1, &q, 0);
    sqlite3_bind_text(q, 1, "a:\\Viejo Artista\\Disco\\01 - Uno.flac", (int)sizeof "a:\\Viejo Artista\\Disco\\01 - Uno.flac", SQLITE_STATIC);
    sqlite3_bind_text(q, 2, "a:\\Viejo Artista\\Disco\\01 - Uno.lrc", (int)sizeof "a:\\Viejo Artista\\Disco\\01 - Uno.lrc", SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) c = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    CHECK(c == 1, "db: ruta nueva con NUL y lrc_path");
    /* carpeta: Viejo Artista/Disco -> Nuevo Artista/Disco Nuevo (con cover.jpg y lrc dentro) */
    char nart[700], nalb[800];
    snprintf(nart, sizeof nart, "%s/Nuevo Artista", root); snprintf(nalb, sizeof nalb, "%s/Disco Nuevo", nart);
    CHECK(ht_rename_dir(alb, nalb, 1) == HT_OK, "renombrar carpeta");
    char ncv[900]; snprintf(ncv, sizeof ncv, "%s/cover.jpg", nalb);
    CHECK(exists(ncv) && !exists(art) && exists(nart), "cover.jpg movida y carpeta vieja vacia borrada");
    const char *f2[1] = { alb }, *t2[1] = { nalb };
    CHECK(ht_db_rename(h, f2, t2, 1, &nr) == HT_OK && nr == 9, "db: carpeta (%d filas: path x6 + lrc_path x3)", nr);
    c = 0;
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM HISTORY_TABLE WHERE path LIKE 'a:\\Nuevo Artista\\Disco Nuevo\\%'", -1, &q, 0);
    if (sqlite3_step(q) == SQLITE_ROW) c = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    CHECK(c == 2, "db: prefijo de carpeta (mayusculas distintas incluidas): %d", c);
    c = 0;
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM list_tb_1 WHERE lrc_path LIKE 'a:\\Nuevo Artista\\Disco Nuevo\\01 - Uno.lrc%'", -1, &q, 0);
    if (sqlite3_step(q) == SQLITE_ROW) c = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    CHECK(c == 1, "db: lrc_path bajo la carpeta nueva");
    c = 0;
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM MEDIA_TABLE WHERE path LIKE 'a:\\Otro\\%'", -1, &q, 0);
    if (sqlite3_step(q) == SQLITE_ROW) c = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    CHECK(c == 1, "db: otras filas intactas");
    /* conflicto de clave primaria: todo o nada */
    sqlite3_exec(h, "INSERT INTO MEDIA_TABLE(id,path,cue_id,name,artist,album) VALUES(9,'a:\\Otro\\y.flac'||char(0),-1,'n','a','b')", 0, 0, 0);
    sqlite3_exec(h, "INSERT INTO MEDIA_TABLE(id,path,cue_id,name,artist,album) VALUES(9,'a:\\Otro\\z.flac'||char(0),-1,'n','a','b')", 0, 0, 0);
    char oy[700], oz[700], ox[700], nx[700];
    snprintf(oy, sizeof oy, "%s/Otro/y.flac", root); snprintf(oz, sizeof oz, "%s/Otro/z.flac", root);
    snprintf(ox, sizeof ox, "%s/Otro/x.flac", root); snprintf(nx, sizeof nx, "%s/Otro/x2.flac", root);
    const char *f3[2] = { ox, oy }, *t3[2] = { nx, oz };
    CHECK(ht_db_rename(h, f3, t3, 2, &nr) == HT_EDB, "db: conflicto -> error");
    c = 0;
    sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM MEDIA_TABLE WHERE path LIKE 'a:\\Otro\\x.flac%'", -1, &q, 0);
    if (sqlite3_step(q) == SQLITE_ROW) c = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    CHECK(c == 1, "db: tras el error no cambio nada (transaccion)");
    sqlite3_close(h);
    /* carpeta destino existente */
    char e1[700], e2[700]; snprintf(e1, sizeof e1, "%s/E1", root); snprintf(e2, sizeof e2, "%s/E2", root);
    mkdir(e1, 0755); mkdir(e2, 0755);
    CHECK(ht_rename_dir(e1, e2, 0) == HT_EEXIST && exists(e1), "carpeta destino existente");
    CHECK(ht_rename_dir(e1, e1, 0) == HT_OK, "misma carpeta");
}

int main(int argc, char **argv)
{
    snprintf(D, sizeof D, "%s", argc > 1 ? argv[1] : "/tmp/hibytag_tags_test");
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", D, D);
    if (system(cmd)) return 2;
    b_rand(&audio_flac, 300000, 1); audio_flac.p[0] = 0xFF; audio_flac.p[1] = 0xF8;
    b_rand(&audio_mp3, 200000, 2); audio_mp3.p[0] = 0xFF; audio_mp3.p[1] = 0xFB;
    b_u8(&pic_jpeg, 0xFF); b_u8(&pic_jpeg, 0xD8); b_rand(&pic_jpeg, 30000, 3);
    test_flac();
    test_mp3(3);
    test_mp3(4);
    test_m4a();
    test_batch();
    test_names();
    test_rename();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
