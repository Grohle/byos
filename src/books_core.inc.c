/* books_core.inc.c - nucleo de Libros sin interfaz (se prueba en PC: tests/books_test.c).
 *  - inflate en streaming (ventana de 32 KB, sin cargar la entrada entera) y lector ZIP;
 *  - EPUB: container.xml -> OPF (titulo, autor, portada, spine) -> indice (nav de EPUB3 o NCX);
 *    XHTML -> "flujo" de parrafos (cabeceras, cursiva y negrita minimas), un capitulo cada vez;
 *  - TXT: UTF-8, UTF-16 (BOM o deteccion), GBK (tabla del firmware) y CP1252 de reserva;
 *    se trocea en bloques de ~64 KB cortados en un salto de parrafo (deterministas);
 *  - maquetacion: corte de lineas (espacios, CJK con kinsoku basico), justificado y paginas.
 * Nada aqui toca la pantalla; las medidas de los glifos llegan por una funcion (bk_advf).
 *
 * Flujo de un capitulo: UTF-8; '\n' termina un parrafo. Codigos de control:
 *   0x01 al inicio de parrafo = cabecera, 0x06 = separador; 0x02/0x03 cursiva on/off,
 *   0x04/0x05 negrita on/off (el estilo se reinicia en cada parrafo); 0x0B salto de linea. */

#define BK_TXT_BLOCK (64 * 1024)          /* bloque de un TXT (bytes en bruto) */
#define BK_FLOW_MAX (2 << 20)             /* tope del flujo de un capitulo (lo demas se recorta) */
#define BK_MAX_CH 8192
#define BK_MAX_TOC 2048
#define BK_SMALL_MAX (1 << 20)            /* container.xml, OPF, NCX, nav: como mucho 1 MB */
#ifndef BK_GBK_TABLE
#define BK_GBK_TABLE "/usr/resource/unicode/gb18030_unicode_tbl.bin"
#endif

enum { BKC_HEAD = 1, BKC_I_ON = 2, BKC_I_OFF = 3, BKC_B_ON = 4, BKC_B_OFF = 5, BKC_HR = 6, BKC_BR = 11 };
enum { BK_TXT, BK_EPUB };
enum { BE_UTF8, BE_U16LE, BE_U16BE, BE_GBK, BE_CP1252, BE_CP1251 };

/* ------------------------------------------------------------------ bufer creciente */
struct bk_buf { char *b; int n, cap, over; };
static void bb_free(struct bk_buf *o) { free(o->b); memset(o, 0, sizeof *o); }
static void bb_put(struct bk_buf *o, const char *s, int n)
{
    if (o->over || n <= 0) return;
    if (o->n + n + 1 > BK_FLOW_MAX) { o->over = 1; return; }
    if (o->n + n + 1 > o->cap) {
        int c = o->cap ? o->cap : 4096; while (c < o->n + n + 1) c *= 2;
        if (c > BK_FLOW_MAX) c = BK_FLOW_MAX;
        char *nb = realloc(o->b, c); if (!nb) { o->over = 1; return; }
        o->b = nb; o->cap = c;
    }
    memcpy(o->b + o->n, s, n); o->n += n; o->b[o->n] = 0;
}
static void bb_c(struct bk_buf *o, int c) { char ch = (char)c; bb_put(o, &ch, 1); }
static int bk_u8enc(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}
static void bb_cp(struct bk_buf *o, int c) { char t[4]; bb_put(o, t, bk_u8enc(t, c)); }
/* decodifica un caracter UTF-8 de [p, e) (tolerante: byte suelto = U+FFFD) */
static int bk_u8dec(const unsigned char *p, const unsigned char *e, int *len)
{
    int c = p[0];
    if (c < 0x80) { *len = 1; return c; }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (!n || p + n > e) { *len = 1; return 0xFFFD; }
    c &= 0x3F >> (n - 1);
    for (int i = 1; i < n; i++) { if ((p[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; } c = c << 6 | (p[i] & 63); }
    *len = n; return c;
}

/* ------------------------------------------------------------------ inflate en streaming */
struct bk_huff { short count[16], sym[288]; uint16_t fast[512]; };
struct bk_inf {
    int fd; long long pos, end;                /* entrada: rango del fichero */
    const uint8_t *mem; long long mlen;        /* o memoria (pruebas) */
    uint8_t in[16384]; int ilen, ipos, pad;
    uint32_t bb; int bc;
    uint8_t win[32768]; uint32_t wp, flushed;
    int (*out)(void *ctx, const uint8_t *b, int n); void *ctx; int stop;
    struct bk_huff lit, dist;
};
static int inf_byte(struct bk_inf *s)
{
    if (s->ipos >= s->ilen) {
        long long left = s->end - s->pos;
        if (left <= 0) { s->pad++; return 0; }
        int n = left > (long long)sizeof s->in ? (int)sizeof s->in : (int)left;
        if (s->mem) memcpy(s->in, s->mem + s->pos, n);
        else { n = (int)pread(s->fd, s->in, n, s->pos); if (n <= 0) { s->pad++; s->end = s->pos; return 0; } }
        s->pos += n; s->ilen = n; s->ipos = 0;
    }
    return s->in[s->ipos++];
}
static inline void inf_need(struct bk_inf *s, int n) { while (s->bc < n) { s->bb |= (uint32_t)inf_byte(s) << s->bc; s->bc += 8; } }
static inline unsigned inf_bits(struct bk_inf *s, int n)
{
    if (!n) return 0;
    inf_need(s, n); unsigned v = s->bb & ((1u << n) - 1); s->bb >>= n; s->bc -= n; return v;
}
static void inf_flush(struct bk_inf *s)
{
    uint32_t n = s->wp - s->flushed; if (!n) return;
    if (!s->stop && s->out(s->ctx, s->win + (s->flushed & 32767), (int)n)) s->stop = 1;
    s->flushed = s->wp;
}
static inline void inf_put(struct bk_inf *s, int c)
{
    s->win[s->wp & 32767] = (uint8_t)c; s->wp++;
    if (!(s->wp & 32767)) inf_flush(s);
}
static int huff_build(struct bk_huff *h, const uint8_t *len, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof h->count); memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    int code = 0, idx = 0;
    for (int l = 1; l < 16; l++) {
        for (int k = 0; k < h->count[l]; k++) {
            int sy = h->sym[idx++];
            if (l <= 9) {
                int rev = 0; for (int b = 0; b < l; b++) if (code >> b & 1) rev |= 1 << (l - 1 - b);
                for (int r = rev; r < 512; r += 1 << l) h->fast[r] = (uint16_t)(l << 12 | sy);
            }
            code++;
        }
        code <<= 1;
    }
    return left;
}
static int huff_dec(struct bk_inf *s, const struct bk_huff *h)
{
    inf_need(s, 9);
    unsigned e = h->fast[s->bb & 511];
    if (e) { int l = e >> 12; s->bb >>= l; s->bc -= l; return e & 4095; }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)inf_bits(s, 1);
        int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}
static const short inf_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short inf_lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short inf_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short inf_dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static int inf_codes(struct bk_inf *s)
{
    for (;;) {
        if (s->stop) return 0;
        if (s->pad > 8) return -1;                       /* datos truncados */
        int sy = huff_dec(s, &s->lit);
        if (sy < 0) return -1;
        if (sy < 256) { inf_put(s, sy); continue; }
        if (sy == 256) return 0;
        sy -= 257; if (sy >= 29) return -1;
        int len = inf_lbase[sy] + (int)inf_bits(s, inf_lext[sy]);
        int ds = huff_dec(s, &s->dist); if (ds < 0 || ds >= 30) return -1;
        uint32_t d = (uint32_t)(inf_dbase[ds] + (int)inf_bits(s, inf_dext[ds]));
        if (d > s->wp || d > 32768) return -1;
        while (len--) inf_put(s, s->win[(s->wp - d) & 32767]);
    }
}
/* descomprime deflate crudo de un fichero (fd, desde off, clen bytes) o de memoria (mem);
 * la salida va a out() en trozos de hasta 32 KB; si out() devuelve !=0 se para. 0 = bien */
static int bk_inflate(int fd, long long off, long long clen, const uint8_t *mem,
                      int (*out)(void *, const uint8_t *, int), void *ctx)
{
    struct bk_inf *s = malloc(sizeof *s); if (!s) return -1;
    s->ilen = s->ipos = s->pad = 0; s->bb = 0; s->bc = 0; s->wp = s->flushed = 0; s->stop = 0;
    s->fd = fd; s->pos = mem ? 0 : off; s->end = mem ? clen : off + clen; s->mem = mem; s->mlen = clen;
    s->out = out; s->ctx = ctx;
    int err = 0, last;
    static const uint8_t ord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    do {
        last = (int)inf_bits(s, 1);
        int type = (int)inf_bits(s, 2);
        if (type == 0) {
            s->bb >>= s->bc & 7; s->bc -= s->bc & 7;
            unsigned len = inf_bits(s, 16), nlen = inf_bits(s, 16);
            if ((len ^ 0xFFFF) != nlen) { err = -1; break; }
            while (len-- && !s->stop) { inf_put(s, (int)inf_bits(s, 8)); if (s->pad > 8) { err = -1; break; } }
        } else if (type == 1) {
            uint8_t l[288 + 30];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&s->lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&s->dist, l, 30);
            err = inf_codes(s);
        } else if (type == 2) {
            int nlen = (int)inf_bits(s, 5) + 257, ndist = (int)inf_bits(s, 5) + 1, ncode = (int)inf_bits(s, 4) + 4;
            if (nlen > 286 || ndist > 30) { err = -1; break; }
            uint8_t l[320]; memset(l, 0, sizeof l);
            for (int i = 0; i < ncode; i++) l[ord[i]] = (uint8_t)inf_bits(s, 3);
            if (huff_build(&s->lit, l, 19) < 0) { err = -1; break; }
            int i = 0;
            while (i < nlen + ndist) {
                int sy = huff_dec(s, &s->lit);
                if (sy < 0 || s->pad > 8) { err = -1; break; }
                if (sy < 16) { l[i++] = (uint8_t)sy; continue; }
                int rep, v = 0;
                if (sy == 16) { if (!i) { err = -1; break; } v = l[i - 1]; rep = 3 + (int)inf_bits(s, 2); }
                else if (sy == 17) rep = 3 + (int)inf_bits(s, 3);
                else rep = 11 + (int)inf_bits(s, 7);
                if (i + rep > nlen + ndist) { err = -1; break; }
                while (rep--) l[i++] = (uint8_t)v;
            }
            if (err) break;
            if (!l[256]) { err = -1; break; }
            if (huff_build(&s->lit, l, nlen) < 0 || huff_build(&s->dist, l + nlen, ndist) < 0) { err = -1; break; }
            err = inf_codes(s);
        } else err = -1;
        if (s->pad > 8) err = -1;
    } while (!last && !err && !s->stop);
    if (!err) inf_flush(s);
    free(s);
    return err;
}

/* ------------------------------------------------------------------ ZIP */
struct bk_zent { uint32_t name, csize, usize, lho, crc; uint16_t nlen, method; };
struct bk_zip { int fd; long long size; struct bk_zent *e; int n; char *names; };
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void bk_zip_close(struct bk_zip *z) { free(z->e); free(z->names); memset(z, 0, sizeof *z); z->fd = -1; }
static int bk_zip_open(struct bk_zip *z, int fd)
{
    memset(z, 0, sizeof *z); z->fd = fd;
    struct stat sb; if (fstat(fd, &sb) || sb.st_size < 22) return -1;
    z->size = sb.st_size;
    int tl = z->size < 65557 ? (int)z->size : 65557;
    uint8_t *t = malloc(tl); if (!t) return -1;
    if (pread(fd, t, tl, z->size - tl) != tl) { free(t); return -1; }
    int eo = -1;
    for (int i = tl - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eo = i; break; }
    if (eo < 0) { free(t); return -1; }
    int n = rd16(t + eo + 10); uint32_t cds = rd32(t + eo + 12), cdo = rd32(t + eo + 16);
    free(t);
    if (n <= 0 || n > 20000 || cds > (8u << 20) || (long long)cdo + cds > z->size) return -1;
    uint8_t *cd = malloc(cds); if (!cd) return -1;
    if (pread(fd, cd, cds, cdo) != (ssize_t)cds) { free(cd); return -1; }
    z->e = calloc(n, sizeof *z->e); z->names = malloc(cds);
    if (!z->e || !z->names) { free(cd); bk_zip_close(z); return -1; }
    uint32_t p = 0, np = 0;
    for (int i = 0; i < n && p + 46 <= cds; i++) {
        if (rd32(cd + p) != 0x02014b50) break;
        struct bk_zent *e = &z->e[z->n];
        e->method = rd16(cd + p + 10); e->crc = rd32(cd + p + 16); e->csize = rd32(cd + p + 20); e->usize = rd32(cd + p + 24);
        int nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        e->lho = rd32(cd + p + 42);
        if (p + 46 + nl > cds) break;
        memcpy(z->names + np, cd + p + 46, nl); e->name = np; e->nlen = (uint16_t)nl; np += nl;
        z->n++;
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return z->n ? 0 : -1;
}
static int bk_zip_find(const struct bk_zip *z, const char *name)
{
    int l = (int)strlen(name);
    for (int i = 0; i < z->n; i++) if (z->e[i].nlen == l && !memcmp(z->names + z->e[i].name, name, l)) return i;
    for (int i = 0; i < z->n; i++) if (z->e[i].nlen == l && !strncasecmp(z->names + z->e[i].name, name, l)) return i;
    return -1;
}
/* manda los bytes descomprimidos de la entrada i a out(); 0 = bien */
static int bk_zip_stream(const struct bk_zip *z, int i, int (*out)(void *, const uint8_t *, int), void *ctx)
{
    if (i < 0 || i >= z->n) return -1;
    const struct bk_zent *e = &z->e[i];
    uint8_t lh[30];
    if (pread(z->fd, lh, 30, e->lho) != 30 || rd32(lh) != 0x04034b50) return -1;
    long long off = (long long)e->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (off + e->csize > z->size) return -1;
    if (e->method == 8) return bk_inflate(z->fd, off, e->csize, 0, out, ctx);
    if (e->method != 0) return -1;
    uint8_t b[16384]; long long left = e->csize;
    while (left > 0) {
        int n = left > (long long)sizeof b ? (int)sizeof b : (int)left;
        if (pread(z->fd, b, n, off) != n) return -1;
        if (out(ctx, b, n)) return 0;
        off += n; left -= n;
    }
    return 0;
}
static int bk_buf_out(void *ctx, const uint8_t *b, int n) { struct bk_buf *o = ctx; bb_put(o, (const char *)b, n); return o->over; }
/* entrada entera en memoria (terminada en 0); solo para ficheros pequenos */
static char *bk_zip_load(const struct bk_zip *z, int i, int max, int *len)
{
    if (i < 0 || i >= z->n || (int)z->e[i].usize > max) return 0;
    struct bk_buf o = { 0 };
    if (bk_zip_stream(z, i, bk_buf_out, &o) || o.over || !o.b) { bb_free(&o); return 0; }
    if (len) *len = o.n;
    return o.b;
}

/* ------------------------------------------------------------------ XML minimo (OPF, NCX, nav) */
struct xtag { const char *name; int nlen; const char *attr; int alen; int close, self; };
/* siguiente etiqueta de [p, e); devuelve el puntero tras '>' (0 si no hay mas) */
static const char *x_next(const char *p, const char *e, struct xtag *t)
{
    for (;;) {
        while (p < e && *p != '<') p++;
        if (p >= e) return 0;
        if (e - p >= 4 && !memcmp(p, "<!--", 4)) { const char *q = p + 4; while (q + 2 < e && memcmp(q, "-->", 3)) q++; p = q + 3; continue; }
        if (p + 1 < e && (p[1] == '?' || p[1] == '!')) { while (p < e && *p != '>') p++; p++; continue; }
        const char *q = p + 1; char quote = 0;
        while (q < e && (quote || *q != '>')) { if (quote) { if (*q == quote) quote = 0; } else if (*q == '"' || *q == '\'') quote = *q; q++; }
        if (q >= e) return 0;
        const char *s = p + 1;
        t->close = *s == '/'; if (t->close) s++;
        const char *ns = s; while (s < q && !isspace((unsigned char)*s) && *s != '/') s++;
        const char *nm = ns; for (const char *c = ns; c < s; c++) if (*c == ':') nm = c + 1;
        t->name = nm; t->nlen = (int)(s - nm);
        t->attr = s; t->alen = (int)(q - s);
        t->self = q > p + 1 && q[-1] == '/';
        return q + 1;
    }
}
static int x_is(const struct xtag *t, const char *n) { int l = (int)strlen(n); return t->nlen == l && !strncasecmp(t->name, n, l); }
/* entidades basicas de XML/HTML; *len = bytes consumidos (desde '&'); -1 si no es una entidad */
static int x_entity(const char *p, const char *e, int *len)
{
    static const struct { const char *n; int c; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
        { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "copy", 0xA9 },
        { "shy", 0xAD }, { "middot", 0xB7 }, { "bull", 0x2022 }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
        { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "iacute", 0xED }, { "oacute", 0xF3 }, { "uacute", 0xFA },
        { "ntilde", 0xF1 }, { "ccedil", 0xE7 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 },
        { "szlig", 0xDF }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "deg", 0xB0 }, { "thinsp", 0x2009 },
        { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "zwnj", 0x200C }, { "zwj", 0x200D }, { "times", 0xD7 },
        { "acirc", 0xE2 }, { "ecirc", 0xEA }, { "icirc", 0xEE }, { "ocirc", 0xF4 }, { "ucirc", 0xFB },
        { "euml", 0xEB }, { "iuml", 0xEF }, { "oelig", 0x153 }, { "aelig", 0xE6 }, { "Eacute", 0xC9 },
    };
    const char *q = p + 1; int n = 0;
    while (q < e && n < 12 && *q != ';' && (isalnum((unsigned char)*q) || *q == '#')) { q++; n++; }
    if (q >= e || *q != ';' || !n) return -1;
    *len = (int)(q - p) + 1;
    if (p[1] == '#') {
        long v = (p[2] == 'x' || p[2] == 'X') ? strtol(p + 3, 0, 16) : strtol(p + 2, 0, 10);
        return v > 0 && v < 0x110000 ? (int)v : 0xFFFD;
    }
    for (unsigned i = 0; i < sizeof ents / sizeof ents[0]; i++)
        if ((int)strlen(ents[i].n) == n && !memcmp(ents[i].n, p + 1, n)) return ents[i].c;
    return -1;
}
/* valor de un atributo (por nombre local), con entidades resueltas; 1 si existe */
static int x_attr(const struct xtag *t, const char *name, char *out, int on)
{
    const char *p = t->attr, *e = t->attr + t->alen; int l = (int)strlen(name);
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == '/')) p++;
        const char *ns = p; while (p < e && *p != '=' && !isspace((unsigned char)*p)) p++;
        const char *nm = ns; for (const char *c = ns; c < p; c++) if (*c == ':') nm = c + 1;
        int nl = (int)(p - nm);
        while (p < e && isspace((unsigned char)*p)) p++;
        if (p >= e || *p != '=') continue;
        p++; while (p < e && isspace((unsigned char)*p)) p++;
        char q = p < e ? *p : 0; const char *vs, *ve;
        if (q == '"' || q == '\'') { vs = ++p; while (p < e && *p != q) p++; ve = p; p++; }
        else { vs = p; while (p < e && !isspace((unsigned char)*p)) p++; ve = p; }
        if (nl == l && !strncasecmp(nm, name, l) && (nm == ns || strncasecmp(ns, "xml:", 4))) {
            int k = 0;
            for (const char *c = vs; c < ve && k < on - 4; ) {
                int el, cp;
                if (*c == '&' && (cp = x_entity(c, ve, &el)) >= 0) { k += bk_u8enc(out + k, cp); c += el; }
                else out[k++] = *c++;
            }
            out[k] = 0; return 1;
        }
    }
    if (on) out[0] = 0;
    return 0;
}
/* texto plano de [p, e) (sin etiquetas, entidades resueltas, espacios colapsados) */
static void x_text(const char *p, const char *e, char *out, int on)
{
    int k = 0, sp = 0;
    while (p < e && k < on - 5) {
        if (*p == '<') { while (p < e && *p != '>') p++; p++; sp = k > 0; continue; }
        if (isspace((unsigned char)*p)) { sp = k > 0; p++; continue; }
        if (sp) { out[k++] = ' '; sp = 0; }
        int el, cp;
        if (*p == '&' && (cp = x_entity(p, e, &el)) >= 0) { if (cp == 0xA0) cp = ' '; k += bk_u8enc(out + k, cp); p += el; }
        else out[k++] = *p++;
    }
    out[k] = 0;
}
/* ruta dentro del zip: "dir" + href relativo (con ../, ./, %XX, sin #fragmento) */
static void bk_resolve(const char *dir, const char *href, char *out, int on, char *frag, int fn)
{
    char tmp[600]; int k = 0;
    if (frag && fn) frag[0] = 0;
    if (href[0] != '/') k = snprintf(tmp, sizeof tmp, "%s", dir);
    if (k >= (int)sizeof tmp) k = (int)sizeof tmp - 1;
    for (const char *h = href[0] == '/' ? href + 1 : href; *h && k < (int)sizeof tmp - 1; h++) {
        if (*h == '#') { if (frag && fn) snprintf(frag, fn, "%s", h + 1); break; }
        if (*h == '%' && isxdigit((unsigned char)h[1]) && isxdigit((unsigned char)h[2])) { char hx[3] = { h[1], h[2], 0 }; tmp[k++] = (char)strtol(hx, 0, 16); h += 2; }
        else tmp[k++] = *h;
    }
    tmp[k] = 0;
    /* normaliza segmentos */
    char *seg[64]; int ns = 0; char *s = tmp;
    while (s && *s && ns < 64) {
        char *sl = strchr(s, '/'); if (sl) *sl = 0;
        if (!strcmp(s, "..")) { if (ns) ns--; }
        else if (*s && strcmp(s, ".")) seg[ns++] = s;
        s = sl ? sl + 1 : 0;
    }
    k = 0; out[0] = 0;
    for (int i = 0; i < ns; i++) k += snprintf(out + k, k < on ? on - k : 0, "%s%s", i ? "/" : "", seg[i]);
}

/* ------------------------------------------------------------------ libro */
struct bk_ch { int zi; long long r0, r1; uint32_t usize; };
struct bk_toc { char title[120]; int ch; int off; uint8_t depth; char frag[64]; };
struct bk_book {
    int fmt, fd; long long fsize;
    char title[160], author[120];
    struct bk_zip z; char opfdir[256];
    int cover_zi;
    int enc, reflow; long long body0;          /* TXT: codificacion, reflujo, inicio tras el BOM */
    struct bk_ch *ch; int nch;
    struct bk_toc *toc; int ntoc;
    long long total;                           /* tamano total (para porcentajes) */
};
static void bk_close(struct bk_book *b)
{
    if (b->fmt == BK_EPUB) bk_zip_close(&b->z);
    if (b->fd >= 0) close(b->fd);
    free(b->ch); free(b->toc);
    memset(b, 0, sizeof *b); b->fd = -1;
}
/* titulo a partir del nombre del fichero (sin extension, "_" -> " ") */
static void bk_title_from_path(const char *path, char *o, int on)
{
    const char *s = strrchr(path, '/'); s = s ? s + 1 : path;
    snprintf(o, on, "%s", s);
    char *d = strrchr(o, '.'); if (d && d != o) *d = 0;
    for (char *c = o; *c; c++) if (*c == '_') *c = ' ';
}

/* --------------------------------------------- codificaciones de TXT */
static const uint16_t *bk_gbk; static int bk_ngbk;            /* pares (gbk, unicode) ordenados */
static void bk_gbk_load(void)
{
    if (bk_gbk || bk_ngbk < 0) return;
    int f = open(BK_GBK_TABLE, O_RDONLY); struct stat sb;
    if (f < 0 || fstat(f, &sb) || sb.st_size < 4) { if (f >= 0) close(f); bk_ngbk = -1; return; }
    void *m = mmap(0, sb.st_size, PROT_READ, MAP_SHARED, f, 0); close(f);
    if (m == MAP_FAILED) { bk_ngbk = -1; return; }
    bk_gbk = m; bk_ngbk = (int)(sb.st_size / 4);
}
static int bk_gbk_lookup(int code)
{
    int lo = 0, hi = bk_ngbk - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1, k = bk_gbk[m * 2];
        if (k == code) return bk_gbk[m * 2 + 1];
        if (k < code) lo = m + 1; else hi = m - 1;
    }
    return 0xFFFD;
}
static const uint16_t cp1252_hi[32] = { 0x20AC, 0xFFFD, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0xFFFD, 0x17D, 0xFFFD,
                                         0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0xFFFD, 0x17E, 0x178 };
static const uint16_t cp1251_hi[64] = {
    0x402, 0x403, 0x201A, 0x453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x409, 0x2039, 0x40A, 0x40C, 0x40B, 0x40F,
    0x452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x459, 0x203A, 0x45A, 0x45C, 0x45B, 0x45F,
    0xA0, 0x40E, 0x45E, 0x408, 0xA4, 0x490, 0xA6, 0xA7, 0x401, 0xA9, 0x404, 0xAB, 0xAC, 0xAD, 0xAE, 0x407,
    0xB0, 0xB1, 0x406, 0x456, 0x491, 0xB5, 0xB6, 0xB7, 0x451, 0x2116, 0x454, 0xBB, 0x458, 0x405, 0x455, 0x457 };
/* un caracter de [p, e) en la codificacion "enc"; *n = bytes consumidos */
static int bk_dec(int enc, const unsigned char *p, const unsigned char *e, int *n)
{
    switch (enc) {
    case BE_U16LE: case BE_U16BE: {
        if (e - p < 2) { *n = (int)(e - p); return 0xFFFD; }
        int c = enc == BE_U16LE ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; *n = 2;
        if (c >= 0xD800 && c < 0xDC00 && e - p >= 4) {
            int d = enc == BE_U16LE ? p[2] | p[3] << 8 : p[2] << 8 | p[3];
            if (d >= 0xDC00 && d < 0xE000) { *n = 4; return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
        }
        return c >= 0xD800 && c < 0xE000 ? 0xFFFD : c;
    }
    case BE_GBK:
        if (p[0] < 0x80) { *n = 1; return p[0]; }
        if (p[0] == 0x80) { *n = 1; return 0x20AC; }
        if (p[0] < 0xFF && e - p >= 4 && p[1] >= 0x30 && p[1] <= 0x39) { *n = 4; return 0xFFFD; }   /* GB18030 de 4 bytes */
        if (p[0] < 0xFF && e - p >= 2 && p[1] >= 0x40 && p[1] != 0x7F && p[1] < 0xFF) { *n = 2; return bk_ngbk > 0 ? bk_gbk_lookup(p[0] << 8 | p[1]) : 0xFFFD; }
        *n = 1; return 0xFFFD;
    case BE_CP1252:
        *n = 1; return p[0] >= 0x80 && p[0] < 0xA0 ? cp1252_hi[p[0] - 0x80] : p[0];
    case BE_CP1251:
        *n = 1; return p[0] >= 0xC0 ? 0x410 + p[0] - 0xC0 : p[0] >= 0x80 ? cp1251_hi[p[0] - 0x80] : p[0];
    default:
        return bk_u8dec(p, e, n);
    }
}
/* deteccion sobre una muestra del principio */
static void bk_txt_detect(struct bk_book *b)
{
    unsigned char s[65536];
    int n = (int)pread(b->fd, s, sizeof s, 0); if (n < 0) n = 0;
    b->body0 = 0; b->enc = BE_UTF8;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { b->body0 = 3; b->enc = BE_UTF8; }
    else if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { b->body0 = 2; b->enc = BE_U16LE; }
    else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { b->body0 = 2; b->enc = BE_U16BE; }
    else {
        int z0 = 0, z1 = 0;
        for (int i = 0; i + 1 < n; i += 2) { z0 += !s[i]; z1 += !s[i + 1]; }
        if (n >= 16 && z1 > n / 8 && z0 < n / 64) b->enc = BE_U16LE;
        else if (n >= 16 && z0 > n / 8 && z1 < n / 64) b->enc = BE_U16BE;
        else {
            int bad = 0, hi = 0;
            for (int i = 0; i < n; ) {
                if (s[i] < 0x80) { i++; continue; }
                hi++; int l; int c = bk_u8dec(s + i, s + n, &l);
                if (c == 0xFFFD && !(n - i < 4 && n == (int)sizeof s)) bad++;
                i += l;
            }
            if (bad) {
                /* GBK: casi todos los bytes altos van por pares (2o byte >= 0x80) y los tramos de
                 * bytes altos entre ASCII tienen longitud par. CP1251 (ruso/ucraniano): tramos
                 * largos (palabras) de letras 0xC0-0xFF. Si no, latin-1/CP1252 (acentos sueltos). */
                int hi2 = 0, tot2 = 0, runs = 0, odd = 0, high = 0, cyr = 0, run = 0;
                for (int i = 0; i + 1 < n; i++) if (s[i] >= 0x81 && s[i] < 0xFF) { tot2++; if (s[i + 1] >= 0x80) { hi2++; i++; } }
                for (int i = 0; i <= n; i++) {
                    if (i < n && s[i] >= 0x80) { run++; high++; if (s[i] >= 0xC0) cyr++; continue; }
                    if (run) { runs++; if (run & 1) odd++; run = 0; }
                }
                bk_gbk_load();
                if (bk_ngbk > 0 && tot2 && hi2 * 10 > tot2 * 7 && odd * 5 < runs) b->enc = BE_GBK;
                else if (runs && cyr * 10 > high * 7 && high >= runs * 3) b->enc = BE_CP1251;
                else b->enc = BE_CP1252;
            }
            (void)hi;
        }
    }
    if (b->enc == BE_GBK) bk_gbk_load();
    /* reflujo (texto partido a ~70 columnas con lineas en blanco entre parrafos, como Gutenberg) */
    int unit = b->enc == BE_U16LE || b->enc == BE_U16BE ? 2 : 1;
    int lines = 0, blank = 0, mid = 0, longl = 0, len = 0, nb = 0;
    for (int i = (int)b->body0; i + unit <= n; i += unit) {
        int c = unit == 2 ? (b->enc == BE_U16LE ? s[i] | s[i + 1] << 8 : s[i] << 8 | s[i + 1]) : s[i];
        if (c == '\n') {
            lines++;
            if (!nb) blank++; else if (len >= 45 && len <= 80) mid++; else if (len > 100) longl++;
            len = 0; nb = 0;
        } else if (c != '\r') { if (unit == 1 && (c & 0xC0) == 0x80 && b->enc == BE_UTF8) continue; len++; if (c != ' ' && c != '\t') nb = 1; }
    }
    int nonblank = lines - blank;
    b->reflow = lines > 20 && blank * 20 >= lines && nonblank > 0 && mid * 2 >= nonblank && longl * 10 < nonblank;
}
/* fin del bloque que empieza en "s": primer final de parrafo tras s + BK_TXT_BLOCK */
static long long bk_txt_block_end(const struct bk_book *b, long long s)
{
    long long e = s + BK_TXT_BLOCK;
    if (e >= b->fsize) return b->fsize;
    int unit = b->enc == BE_U16LE || b->enc == BE_U16BE ? 2 : 1;
    if (unit == 2 && ((e - b->body0) & 1)) e++;
    unsigned char w[8192]; long long pos = e, fallback = -1;
    while (pos < b->fsize && pos - e < 4 * BK_TXT_BLOCK) {
        int n = (int)pread(b->fd, w, sizeof w, pos); if (n < unit) break;
        n -= n % unit;
        for (int i = 0; i + unit <= n; i += unit) {
            int c = unit == 2 ? (b->enc == BE_U16LE ? w[i] | w[i + 1] << 8 : w[i] << 8 | w[i + 1]) : w[i];
            if (c != '\n') continue;
            long long after = pos + i + unit;
            if (!b->reflow) return after;
            if (fallback < 0) fallback = after;
            /* reflujo: hace falta una linea en blanco (siguiente linea vacia) */
            int j = i + unit, blankl = 1;
            while (j + unit <= n) {
                int d = unit == 2 ? (b->enc == BE_U16LE ? w[j] | w[j + 1] << 8 : w[j] << 8 | w[j + 1]) : w[j];
                if (d == '\n') break;
                if (d != '\r' && d != ' ' && d != '\t') { blankl = 0; break; }
                j += unit;
            }
            if (blankl && j + unit <= n) return pos + j + unit;
        }
        pos += n - 2 * unit > 0 ? n - 2 * unit : n;
    }
    return fallback >= 0 ? fallback : b->fsize;
}
/* --------------------------------------------- cabeceras de capitulo en TXT */
static int bk_txt_heading(const char *l, int n)
{
    if (n <= 0 || n > 90) return 0;
    static const char *const pre[] = { "chapter ", "capítulo ", "capitulo ", "chapitre ", "kapitel ", "capitolo ", "rozdział ",
                                       "глава ", "ГЛАВА ", "Глава ", "розділ ", "Розділ ", "РОЗДІЛ ", "prologue", "epilogue",
                                       "prólogo", "epílogo", "prologo", "epilogo", "prolog", "épilogue", 0 };
    for (int i = 0; pre[i]; i++) { int k = (int)strlen(pre[i]); if (n >= k && !strncasecmp(l, pre[i], k)) return n <= 70; }
    if (n >= 6 && !memcmp(l, "第", 3)) {                    /* 第十二章 / 第3回 ... */
        static const char *const suf[] = { "章", "回", "节", "節", "卷", "部", "集", "篇", 0 };
        for (int p = 3; p < n && p <= 3 + 24; ) {
            for (int s = 0; suf[s]; s++) if (p + 3 <= n && !memcmp(l + p, suf[s], 3)) return 1;
            int len; bk_u8dec((const unsigned char *)l + p, (const unsigned char *)l + n, &len); p += len;
        }
    }
    return 0;
}
static int bk_is_cjk(int c)
{
    return (c >= 0x2E80 && c < 0xA000) || (c >= 0xAC00 && c < 0xD7A4) || (c >= 0xF900 && c < 0xFB00) || (c >= 0xFF00 && c < 0xFFF0) || (c >= 0x20000 && c < 0x30000);
}
/* marcas: kind 0 = id de un elemento (EPUB), 1 = cabecera de TXT (texto) */
typedef void (*bk_markf)(void *ctx, int kind, const char *s, int off);

/* bloque de TXT -> flujo */
static int bk_txt_flow(const struct bk_book *b, int ch, struct bk_buf *o, bk_markf mark, void *mctx)
{
    long long r0 = b->ch[ch].r0, r1 = b->ch[ch].r1;
    int n = (int)(r1 - r0); if (n <= 0) return 0;
    unsigned char *raw = malloc(n + 4); if (!raw) return -1;
    if (pread(b->fd, raw, n, r0) != n) { free(raw); return -1; }
    char *line = malloc(65536 + 8); if (!line) { free(raw); return -1; }
    int ln = 0, para = 0;
    const unsigned char *p = raw, *e = raw + n;
    for (;;) {
        int c = -1, k = 0;
        if (p < e) { c = bk_dec(b->enc, p, e, &k); if (k <= 0) k = 1; p += k; }
        if (c == '\n' || c == 0x2029 || c == '\f' || c < 0 || ln > 65536 - 8) {
            int cont = ln > 65536 - 8 && c >= 0;
            int a = 0, z = ln;
            while (z > 0 && (line[z - 1] == ' ' || line[z - 1] == '\t' || line[z - 1] == '\r')) z--;
            if (!cont) while (a < z && (line[a] == ' ' || line[a] == '\t')) a++;
            if (a == z) { if (para && (b->reflow || cont)) { bb_c(o, '\n'); para = 0; } }
            else if (!cont && bk_txt_heading(line + a, z - a)) {
                if (para) { bb_c(o, '\n'); para = 0; }
                if (mark) { char t[120]; int m = z - a < 119 ? z - a : 119; memcpy(t, line + a, m); t[m] = 0; mark(mctx, 1, t, o->n); }
                bb_c(o, BKC_HEAD); bb_put(o, line + a, z - a); bb_c(o, '\n');
            } else if (b->reflow || cont) {
                if (para) {
                    int lc = 0, fc, l2;
                    { int q = o->n - 1; while (q > 0 && ((unsigned char)o->b[q] & 0xC0) == 0x80) q--; lc = bk_u8dec((unsigned char *)o->b + q, (unsigned char *)o->b + o->n, &l2); }
                    fc = bk_u8dec((unsigned char *)line + a, (unsigned char *)line + z, &l2);
                    if (!(bk_is_cjk(lc) && bk_is_cjk(fc)) && !cont) bb_c(o, ' ');
                }
                bb_put(o, line + a, z - a); para = 1;
                if (!b->reflow && !cont) { bb_c(o, '\n'); para = 0; }
            } else { bb_put(o, line + a, z - a); bb_c(o, '\n'); para = 0; }
            ln = 0;
            if (c >= 0 && c != '\n' && c != 0x2029 && c != '\f') { ln = bk_u8enc(line, c); }
            if (c < 0) break;
            continue;
        }
        if (c == '\r' || c == 0xFEFF || c == 0xAD) continue;
        if (c == '\t') c = ' ';
        if (c < 0x20) continue;
        ln += bk_u8enc(line + ln, c);
    }
    if (para) bb_c(o, '\n');
    free(line); free(raw);
    return 0;
}

/* --------------------------------------------- XHTML -> flujo */
struct bk_html {
    struct bk_buf *o;
    int st;                         /* 0 texto, 1 etiqueta, 2 comentario, 3 entidad */
    char tag[512]; int tn; char q; int dash;
    char ent[16]; int en;
    int skip, pre, ital, bold, head, para, sp, bullet, body;
    bk_markf mark; void *mctx;
};
static void hx_endpara(struct bk_html *h)
{
    if (h->para) {
        while (h->o->n > 0 && h->o->b[h->o->n - 1] == BKC_BR) h->o->n--;      /* <br/> final: sobra */
        bb_c(h->o, '\n');
    }
    h->para = 0; h->sp = 0; h->bullet = 0;
}
static void hx_open(struct bk_html *h)
{
    if (h->para) { if (h->sp) { bb_c(h->o, ' '); h->sp = 0; } return; }
    if (h->head) bb_c(h->o, BKC_HEAD);
    if (h->ital) bb_c(h->o, BKC_I_ON);
    if (h->bold) bb_c(h->o, BKC_B_ON);
    if (h->bullet) { bb_cp(h->o, 0x2022); bb_c(h->o, ' '); h->bullet = 0; }
    h->para = 1; h->sp = 0;
}
static void hx_text(struct bk_html *h, const char *s, int n)
{
    if (h->skip || !h->body) return;
    hx_open(h); bb_put(h->o, s, n);
}
static void hx_cp(struct bk_html *h, int c)
{
    if (c == 0xAD || c == 0x200B || c == 0xFEFF) return;
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { if (h->para) h->sp = 1; return; }
    char t[4]; hx_text(h, t, bk_u8enc(t, c));
}
static void hx_tag(struct bk_html *h)
{
    h->tag[h->tn] = 0;
    struct xtag t; const char *e = h->tag + h->tn;
    char *s = h->tag;
    t.close = *s == '/'; if (t.close) s++;
    const char *ns = s; while (s < e && !isspace((unsigned char)*s) && *s != '/') s++;
    const char *nm = ns; for (const char *c = ns; c < s; c++) if (*c == ':') nm = c + 1;
    t.name = nm; t.nlen = (int)(s - nm); t.attr = s; t.alen = (int)(e - s);
    t.self = h->tn > 0 && e[-1] == '/';
    if (!t.nlen || *ns == '!' || *ns == '?') return;
    int open = !t.close, d = open ? 1 : -1;
    if (x_is(&t, "body")) { h->body = open; return; }
    if (x_is(&t, "head") || x_is(&t, "script") || x_is(&t, "style") || x_is(&t, "title") || x_is(&t, "svg") || x_is(&t, "math") || x_is(&t, "rt") || x_is(&t, "rp")) {
        if (!t.self) { h->skip += d; if (h->skip < 0) h->skip = 0; }
        return;
    }
    if (h->mark && open) { char id[80]; if (x_attr(&t, "id", id, sizeof id)) h->mark(h->mctx, 0, id, h->o->n); }
    if (h->skip) return;
    if (x_is(&t, "br")) {
        if (h->para) {
            if (h->o->n > 0 && h->o->b[h->o->n - 1] == BKC_BR) { h->o->n--; hx_endpara(h); }
            else { bb_c(h->o, BKC_BR); h->sp = 0; }
        }
        return;
    }
    if (x_is(&t, "hr")) { hx_endpara(h); if (h->body) { bb_c(h->o, BKC_HR); bb_c(h->o, '\n'); } return; }
    if (t.nlen == 2 && (t.name[0] == 'h' || t.name[0] == 'H') && t.name[1] >= '1' && t.name[1] <= '6') {
        hx_endpara(h); if (!t.self) { h->head += d; if (h->head < 0) h->head = 0; }
        return;
    }
    static const char *const blocks[] = { "p", "div", "li", "blockquote", "section", "article", "header", "footer", "aside", "ul", "ol",
                                          "dl", "dt", "dd", "table", "tr", "figure", "figcaption", "pre", "nav", "center", "address", "caption", "main", 0 };
    for (int i = 0; blocks[i]; i++) if (x_is(&t, blocks[i])) {
        hx_endpara(h);
        if (x_is(&t, "pre") && !t.self) { h->pre += d; if (h->pre < 0) h->pre = 0; }
        if (x_is(&t, "li") && open) h->bullet = 1;
        return;
    }
    if (x_is(&t, "td") || x_is(&t, "th")) { if (h->para) h->sp = 1; return; }
    if (t.self) return;
    if (x_is(&t, "i") || x_is(&t, "em") || x_is(&t, "cite") || x_is(&t, "dfn") || x_is(&t, "var")) {
        if (open) { if (!h->ital++ && h->para) { hx_open(h); bb_c(h->o, BKC_I_ON); } }
        else if (h->ital > 0) { if (!--h->ital && h->para) bb_c(h->o, BKC_I_OFF); }
        return;
    }
    if (x_is(&t, "b") || x_is(&t, "strong")) {
        if (open) { if (!h->bold++ && h->para) { hx_open(h); bb_c(h->o, BKC_B_ON); } }
        else if (h->bold > 0) { if (!--h->bold && h->para) bb_c(h->o, BKC_B_OFF); }
        return;
    }
}
static int hx_feed(void *ctx, const uint8_t *b, int n)
{
    struct bk_html *h = ctx;
    for (int i = 0; i < n; i++) {
        int c = b[i];
        switch (h->st) {
        case 0:
            if (c == '<') { h->st = 1; h->tn = 0; h->q = 0; break; }
            if (c == '&') { h->st = 3; h->en = 0; break; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (h->pre && c == '\n' && h->para && !h->skip && h->body) { bb_c(h->o, BKC_BR); h->sp = 0; }
                else if (h->para) h->sp = 1;
                break;
            }
            if (c < 0x20) break;
            if (h->skip || !h->body) break;
            {   /* tramo de texto seguido */
                int j = i;
                while (j < n && b[j] != '<' && b[j] != '&' && b[j] > ' ') j++;
                /* U+00AD (C2 AD), U+200B (E2 80 8B), U+FEFF (EF BB BF): fuera */
                int a = i;
                for (int k = i; k < j; k++) {
                    int drop = 0;
                    if (b[k] == 0xC2 && k + 1 < j && b[k + 1] == 0xAD) drop = 2;
                    else if (b[k] == 0xE2 && k + 2 < j && b[k + 1] == 0x80 && b[k + 2] == 0x8B) drop = 3;
                    else if (b[k] == 0xEF && k + 2 < j && b[k + 1] == 0xBB && b[k + 2] == 0xBF) drop = 3;
                    if (drop) { hx_text(h, (const char *)b + a, k - a); k += drop - 1; a = k + 1; }
                }
                hx_text(h, (const char *)b + a, j - a);
                i = j - 1;
            }
            break;
        case 1:
            if (h->q) { if (c == h->q) h->q = 0; }
            else if (c == '"' || c == '\'') { if (h->tn > 0 && h->tag[0] != '!') h->q = (char)c; }
            else if (c == '>') { h->st = 0; hx_tag(h); break; }
            if (h->tn < (int)sizeof h->tag - 1) h->tag[h->tn++] = (char)c;
            if (h->tn == 3 && !memcmp(h->tag, "!--", 3)) { h->st = 2; h->dash = 0; }
            break;
        case 2:
            if (c == '-') h->dash++;
            else { if (c == '>' && h->dash >= 2) h->st = 0; h->dash = 0; }
            break;
        case 3:
            if (c == ';' || h->en >= 12 || !(isalnum(c) || c == '#')) {
                char t[20]; t[0] = '&'; memcpy(t + 1, h->ent, h->en); t[h->en + 1] = ';'; int el;
                int cp = c == ';' ? x_entity(t, t + h->en + 2, &el) : -1;
                h->st = 0;
                if (cp >= 0) hx_cp(h, cp);
                else { hx_text(h, t, h->en + 1); if (c != ';') i--; else hx_text(h, ";", 1); }
                break;
            }
            h->ent[h->en++] = (char)c;
            break;
        }
    }
    return h->o->over;
}

/* --------------------------------------------- abrir */
static void bk_toc_add(struct bk_book *b, const char *title, int ch, const char *frag, int depth, int off)
{
    if (b->ntoc >= BK_MAX_TOC) return;
    if (!(b->ntoc & 63)) { struct bk_toc *n = realloc(b->toc, (b->ntoc + 64) * sizeof *n); if (!n) return; b->toc = n; }
    struct bk_toc *t = &b->toc[b->ntoc++];
    memset(t, 0, sizeof *t);
    snprintf(t->title, sizeof t->title, "%s", title); t->ch = ch; t->off = off; t->depth = (uint8_t)(depth > 6 ? 6 : depth);
    if (frag) snprintf(t->frag, sizeof t->frag, "%s", frag);
    if (frag && frag[0]) t->off = -1;
}
struct bk_item { char id[64]; char href[256]; char type[48]; int props; };   /* props: 1 nav, 2 cover-image */
static int bk_epub_open(struct bk_book *b, int meta_only)
{
    if (bk_zip_open(&b->z, b->fd)) return -1;
    char *cx = bk_zip_load(&b->z, bk_zip_find(&b->z, "META-INF/container.xml"), BK_SMALL_MAX, 0);
    char opfp[300] = "";
    if (cx) {
        struct xtag t; const char *p = cx, *e = cx + strlen(cx);
        while ((p = x_next(p, e, &t))) if (!t.close && x_is(&t, "rootfile") && x_attr(&t, "full-path", opfp, sizeof opfp)) break;
        free(cx);
    }
    if (!opfp[0]) for (int i = 0; i < b->z.n; i++) {                 /* sin container: el primer .opf */
        const char *nm = b->z.names + b->z.e[i].name; int l = b->z.e[i].nlen;
        if (l > 4 && l < (int)sizeof opfp && !strncasecmp(nm + l - 4, ".opf", 4)) { memcpy(opfp, nm, l); opfp[l] = 0; break; }
    }
    int oi = bk_zip_find(&b->z, opfp), olen = 0;
    char *opf = bk_zip_load(&b->z, oi, BK_SMALL_MAX, &olen);
    if (!opf) return -1;
    { const char *sl = strrchr(opfp, '/'); int dl = sl ? (int)(sl - opfp) + 1 : 0; if (dl >= (int)sizeof b->opfdir) dl = 0; memcpy(b->opfdir, opfp, dl); b->opfdir[dl] = 0; }
    struct bk_item *it = 0; int nit = 0;
    char cover_id[64] = "", toc_id[64] = "";
    int *spine = 0, nsp = 0;
    struct xtag t; const char *p = opf, *e = opf + olen, *q;
    while ((q = x_next(p, e, &t))) {
        if (!t.close && !t.self && (x_is(&t, "title") || x_is(&t, "creator"))) {
            const char *s = q, *end = q; while (end < e && *end != '<') end++;
            char *dst = x_is(&t, "title") ? b->title : b->author; int dn = x_is(&t, "title") ? (int)sizeof b->title : (int)sizeof b->author;
            if (!dst[0]) x_text(s, end, dst, dn);
        } else if (!t.close && x_is(&t, "meta")) {
            char nm[32]; if (x_attr(&t, "name", nm, sizeof nm) && !strcasecmp(nm, "cover")) x_attr(&t, "content", cover_id, sizeof cover_id);
        } else if (!t.close && x_is(&t, "item")) {
            if (!(nit & 127)) { struct bk_item *ni = realloc(it, (nit + 128) * sizeof *ni); if (!ni) break; it = ni; }
            struct bk_item *m = &it[nit]; char pr[96] = "";
            x_attr(&t, "id", m->id, sizeof m->id); x_attr(&t, "href", m->href, sizeof m->href); x_attr(&t, "media-type", m->type, sizeof m->type);
            x_attr(&t, "properties", pr, sizeof pr);
            m->props = (strstr(pr, "nav") ? 1 : 0) | (strstr(pr, "cover-image") ? 2 : 0);
            if (m->href[0] && nit < 4000) nit++;
        } else if (!t.close && x_is(&t, "spine")) {
            x_attr(&t, "toc", toc_id, sizeof toc_id);
        } else if (!t.close && x_is(&t, "itemref")) {
            char idr[64]; if (!x_attr(&t, "idref", idr, sizeof idr)) { p = q; continue; }
            for (int i = 0; i < nit; i++) if (!strcmp(it[i].id, idr)) {
                if (!(nsp & 127)) { int *ns = realloc(spine, (nsp + 128) * sizeof *ns); if (!ns) break; spine = ns; }
                if (nsp < BK_MAX_CH) spine[nsp++] = i;
                break;
            }
        }
        p = q;
    }
    /* portada */
    b->cover_zi = -1;
    {
        int ci = -1;
        for (int i = 0; i < nit && ci < 0; i++) if (it[i].props & 2) ci = i;
        for (int i = 0; i < nit && ci < 0 && cover_id[0]; i++) if (!strcmp(it[i].id, cover_id) && !strncmp(it[i].type, "image/", 6)) ci = i;
        for (int i = 0; i < nit && ci < 0; i++) if (!strncmp(it[i].type, "image/", 6) && (strcasestr(it[i].id, "cover") || strcasestr(it[i].href, "cover"))) ci = i;
        if (ci >= 0) { char path[600]; bk_resolve(b->opfdir, it[ci].href, path, sizeof path, 0, 0); b->cover_zi = bk_zip_find(&b->z, path); }
    }
    if (!b->title[0]) b->title[0] = 0;
    if (meta_only) { free(it); free(spine); free(opf); return 0; }
    /* capitulos */
    b->ch = calloc(nsp ? nsp : 1, sizeof *b->ch); b->nch = 0; b->total = 0;
    for (int i = 0; b->ch && i < nsp; i++) {
        char path[600]; bk_resolve(b->opfdir, it[spine[i]].href, path, sizeof path, 0, 0);
        int zi = bk_zip_find(&b->z, path); if (zi < 0) continue;
        b->ch[b->nch].zi = zi; b->ch[b->nch].usize = b->z.e[zi].usize; b->ch[b->nch].r0 = b->total;
        b->total += b->z.e[zi].usize + 1; b->ch[b->nch].r1 = b->total; b->nch++;
    }
    /* indice: nav (EPUB3) o NCX (EPUB2) */
    int navi = -1, ncxi = -1;
    for (int i = 0; i < nit; i++) if (it[i].props & 1) navi = i;
    for (int i = 0; i < nit && ncxi < 0; i++) if ((toc_id[0] && !strcmp(it[i].id, toc_id)) || !strcmp(it[i].type, "application/x-dtbncx+xml")) ncxi = i;
    for (int pass = 0; pass < 2 && !b->ntoc; pass++) {
        int ix = pass == 0 ? (navi >= 0 ? navi : ncxi) : (navi >= 0 ? ncxi : -1);
        if (ix < 0) continue;
        char tp[600]; bk_resolve(b->opfdir, it[ix].href, tp, sizeof tp, 0, 0);
        char tdir[300]; { const char *sl = strrchr(tp, '/'); int dl = sl ? (int)(sl - tp) + 1 : 0; if (dl >= (int)sizeof tdir) dl = 0; memcpy(tdir, tp, dl); tdir[dl] = 0; }
        int tl = 0; char *tx = bk_zip_load(&b->z, bk_zip_find(&b->z, tp), BK_SMALL_MAX, &tl);
        if (!tx) continue;
        const char *a = tx, *z = tx + tl, *n2;
        int is_nav = ix == navi;
        int depth = 0, in_toc = !is_nav, navdepth = 0;
        char src[300] = "";
        while ((n2 = x_next(a, z, &t))) {
            if (is_nav) {
                if (x_is(&t, "nav")) {
                    if (!t.close) { char ty[48] = ""; x_attr(&t, "type", ty, sizeof ty); navdepth++; if (strstr(ty, "toc") && !in_toc) in_toc = navdepth; }
                    else { if (in_toc == navdepth) in_toc = -1; navdepth--; }
                } else if (in_toc > 0 && x_is(&t, "ol")) depth += t.close ? -1 : 1;
                else if (in_toc > 0 && x_is(&t, "a") && !t.close) {
                    x_attr(&t, "href", src, sizeof src);
                    const char *s = n2; struct xtag t2; const char *q2 = n2, *end = z;
                    while ((q2 = x_next(q2, z, &t2))) if (t2.close && x_is(&t2, "a")) { end = q2 - 1; while (end > s && *end != '<') end--; break; }
                    char title[120]; x_text(s, end, title, sizeof title);
                    char path[600], frag[64]; bk_resolve(tdir, src, path, sizeof path, frag, sizeof frag);
                    int zi = bk_zip_find(&b->z, path), ci = -1;
                    for (int k = 0; k < b->nch; k++) if (b->ch[k].zi == zi) { ci = k; break; }
                    if (ci >= 0 && title[0]) bk_toc_add(b, title, ci, frag, depth > 0 ? depth - 1 : 0, 0);
                }
            } else {
                if (x_is(&t, "navPoint")) { if (!t.close) depth++; else depth--; }
                else if (x_is(&t, "text") && !t.close && depth > 0) {
                    const char *s = n2, *end = n2; while (end < z && *end != '<') end++;
                    char title[120]; x_text(s, end, title, sizeof title);
                    /* el content src llega despues del text */
                    const char *q2 = end; struct xtag t2; src[0] = 0;
                    while ((q2 = x_next(q2, z, &t2))) { if (x_is(&t2, "content")) { x_attr(&t2, "src", src, sizeof src); break; } if (x_is(&t2, "navPoint")) break; }
                    char path[600], frag[64]; bk_resolve(tdir, src, path, sizeof path, frag, sizeof frag);
                    int zi = bk_zip_find(&b->z, path), ci = -1;
                    for (int k = 0; k < b->nch; k++) if (b->ch[k].zi == zi) { ci = k; break; }
                    if (ci >= 0 && title[0]) bk_toc_add(b, title, ci, frag, depth - 1, 0);
                }
            }
            a = n2;
        }
        (void)in_toc;
        free(tx);
    }
    free(it); free(spine); free(opf);
    return b->nch ? 0 : -1;
}
static int bk_txt_open(struct bk_book *b)
{
    bk_txt_detect(b);
    int cap = 64; b->ch = malloc(cap * sizeof *b->ch); b->nch = 0;
    long long s = b->body0;
    while (b->ch && s < b->fsize && b->nch < BK_MAX_CH) {
        long long e = bk_txt_block_end(b, s);
        if (b->nch == cap) { cap *= 2; struct bk_ch *n = realloc(b->ch, cap * sizeof *n); if (!n) break; b->ch = n; }
        b->ch[b->nch].zi = -1; b->ch[b->nch].r0 = s; b->ch[b->nch].r1 = e; b->ch[b->nch].usize = (uint32_t)(e - s); b->nch++;
        s = e;
    }
    if (b->nch && s < b->fsize) b->ch[b->nch - 1].r1 = b->fsize;
    b->total = b->fsize;
    return b->nch ? 0 : -1;
}
/* abre un libro; meta_only: solo titulo/autor/portada (biblioteca) */
static int bk_open(struct bk_book *b, const char *path, int meta_only)
{
    memset(b, 0, sizeof *b); b->fd = -1; b->cover_zi = -1;
    const char *x = strrchr(path, '.');
    b->fmt = x && !strcasecmp(x, ".epub") ? BK_EPUB : BK_TXT;
    b->fd = open(path, O_RDONLY);
    if (b->fd < 0) return -1;
    struct stat sb; if (fstat(b->fd, &sb)) { bk_close(b); return -1; }
    b->fsize = sb.st_size;
    int r = b->fmt == BK_EPUB ? bk_epub_open(b, meta_only) : (meta_only ? 0 : bk_txt_open(b));
    if (!b->title[0]) bk_title_from_path(path, b->title, sizeof b->title);
    if (r) { bk_close(b); return -1; }
    return 0;
}
/* flujo de un capitulo (o) con marcas de ids/cabeceras; 0 = bien */
static int bk_flow(const struct bk_book *b, int ch, struct bk_buf *o, bk_markf mark, void *mctx)
{
    o->n = 0; o->over = 0; if (o->b) o->b[0] = 0;
    if (ch < 0 || ch >= b->nch) return -1;
    if (b->fmt == BK_TXT) return bk_txt_flow(b, ch, o, mark, mctx);
    struct bk_html *h = calloc(1, sizeof *h); if (!h) return -1;
    h->o = o; h->mark = mark; h->mctx = mctx; h->body = 1;   /* sin <body> (fragmentos): todo vale */
    /* si hay <body>, lo anterior no cuenta: el <head> va en skip de todas formas */
    int r = bk_zip_stream(&b->z, b->ch[ch].zi, hx_feed, h);
    hx_endpara(h);
    free(h);
    return o->over ? 0 : r;
}

/* ------------------------------------------------------------------ maquetacion */
typedef int (*bk_advf)(void *ctx, int cp, int size);     /* avance en 26.6 */
struct bk_lp { int w, h, size, lh, para, hsize, hlh, hbefore, hafter, justify; };
struct bk_line { int a, b, y, x26, sp26, w26; uint8_t kind, center; short size; };
static void bk_lp_make(struct bk_lp *P, int w, int h, int size, int justify)
{
    P->w = w; P->h = h; P->size = size; P->lh = (size * 145 + 50) / 100; P->para = size * 55 / 100;
    P->hsize = size * 13 / 10; P->hlh = P->hsize * 13 / 10; P->hbefore = size; P->hafter = size * 6 / 10; P->justify = justify;
}
static int bk_no_start(int c)   /* no puede empezar linea */
{
    return strchr(",.;:!?)]}", c < 128 ? c : 1) ? c < 128 : c == 0x3001 || c == 0x3002 || c == 0xFF0C || c == 0xFF0E || c == 0xFF01 || c == 0xFF1F || c == 0xFF1A || c == 0xFF1B ||
           c == 0xFF09 || c == 0x300D || c == 0x300F || c == 0x3011 || c == 0x3009 || c == 0x300B || c == 0x201D || c == 0x2019 || c == 0x2026 || c == 0x30FC || c == 0xBB;
}
static int bk_no_end(int c) { return c == '(' || c == '[' || c == 0xFF08 || c == 0x300C || c == 0x300E || c == 0x3010 || c == 0x3008 || c == 0x300A || c == 0x201C || c == 0x2018 || c == 0xAB; }
static int bk_zw(int c) { return c == 0xAD || c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF; }
/* corta una linea desde off; *e = fin (excluido), *nx = inicio de la siguiente; devuelve
 * 1 si termina el parrafo, 2 si es un salto de linea (<br>) */
static int bk_break(const char *f, int off, int len, int maxw, int size, bk_advf adv, void *ac, int *e, int *nx, int *w26, int *nsp)
{
    const unsigned char *u = (const unsigned char *)f;
    int pos = off, w = 0, sp = 0, bk_e = -1, bk_n = 0, bk_w = 0, bk_sp = 0, prev = 0;
    while (pos < len) {
        int c = u[pos];
        if (c == '\n') { *e = pos; *nx = pos; *w26 = w; *nsp = sp; return 1; }
        if (c == BKC_BR) { *e = pos; *nx = pos + 1; *w26 = w; *nsp = sp; return 2; }
        if (c < 0x20) { pos++; continue; }
        int l; int cp = bk_u8dec(u + pos, u + len, &l);
        if (bk_zw(cp)) { pos += l; continue; }
        if (cp == ' ') {
            if (pos > off && prev != ' ') { bk_e = pos; bk_w = w; bk_sp = sp; }
            int q = pos + 1; while (q < len && u[q] == ' ') q++;
            bk_n = q;
            if (pos > off) { w += adv(ac, ' ', size); sp++; }
            pos = q; prev = ' '; continue;
        }
        int a = adv(ac, cp == 0xA0 ? ' ' : cp, size);
        if (bk_is_cjk(cp) && pos > off && !bk_no_start(cp) && !bk_no_end(prev) && prev != ' ') { bk_e = pos; bk_n = pos; bk_w = w; bk_sp = sp; }
        if (w + a > maxw && pos > off) {
            if (bk_e > off) {   /* al ultimo punto de corte */
                *e = bk_e; *nx = bk_n; *w26 = bk_w; *nsp = bk_sp;
                return 0;
            }
            *e = pos; *nx = pos; *w26 = w; *nsp = sp; return 0;   /* palabra mas larga que la linea */
        }
        w += a; pos += l;
        if ((cp == '-' || cp == 0x2014 || cp == 0x2013 || cp == '/') && pos < len && u[pos] > ' ' && prev != ' ' && prev) { bk_e = pos; bk_n = pos; bk_w = w; bk_sp = sp; }
        prev = cp;
    }
    *e = len; *nx = len; *w26 = w; *nsp = sp; return 1;
}
/* parrafo que empieza en off: 0 normal, 1 cabecera, 2 separador */
static int bk_pkind(const char *f, int len, int off) { if (off >= len) return 0; return f[off] == BKC_HEAD ? 1 : f[off] == BKC_HR ? 2 : 0; }
static int bk_para_start(const char *f, int off) { return off == 0 || f[off - 1] == '\n'; }
/* maqueta una pagina desde "off"; devuelve el numero de lineas y *next = inicio de la siguiente.
 * lines puede ser 0 (solo contar). La pagina siempre avanza al menos una linea. */
static int bk_layout_page(const struct bk_lp *P, const char *f, int len, int off, bk_advf adv, void *ac, struct bk_line *lines, int maxl, int *next)
{
    int y = 0, nl = 0, w26max = P->w << 6, pk = 0;
    while (off < len) {
        int ps = bk_para_start(f, off), kind;
        if (ps) kind = bk_pkind(f, len, off);
        else if (nl) kind = pk;                    /* sigue el parrafo de la linea anterior */
        else { int s = off; while (s > 0 && f[s - 1] != '\n') s--; kind = bk_pkind(f, len, s); }   /* pagina a mitad de parrafo */
        pk = kind;
        int lh = kind == 1 ? P->hlh : P->lh, size = kind == 1 ? P->hsize : P->size;
        int gap = 0;
        if (ps && nl) gap = kind == 1 ? P->hbefore : P->para;
        if (kind == 1 && ps && nl && y + gap + lh + P->hafter + 2 * P->lh > P->h) break;   /* cabecera al pie: a la siguiente */
        if (y + gap + lh > P->h && nl) break;
        if (nl >= maxl && lines) break;
        y += gap;
        if (kind == 2) {
            int q = off; while (q < len && f[q] != '\n') q++;
            if (lines) lines[nl] = (struct bk_line){ off, off, y, 0, 0, 0, 2, 1, (short)size };
            nl++; y += lh; off = q < len ? q + 1 : len; continue;
        }
        int e, nx, w26, nsp;
        int end = bk_break(f, off, len, w26max, size, adv, ac, &e, &nx, &w26, &nsp);
        if (lines) {
            struct bk_line *L = &lines[nl];
            L->a = off; L->b = e; L->y = y; L->kind = (uint8_t)kind; L->size = (short)size; L->w26 = w26;
            L->center = kind == 1; L->x26 = 0; L->sp26 = 0;
            if (kind == 1) L->x26 = (w26max - w26) / 2;
            /* justificado sin guiones: si cada espacio creceria mas de 0,4 em, mejor en bandera */
            else if (!end && P->justify && nsp > 0 && w26max - w26 <= nsp * size * 26) L->sp26 = (w26max - w26) / nsp;
        }
        nl++; y += lh;
        off = nx;
        if (end == 1) { off = off < len ? off + 1 : len; if (kind == 1) y += P->hafter; }
    }
    *next = off;
    return nl;
}
/* inicios de pagina de un flujo entero (malloc); devuelve cuantas */
static int bk_paginate(const struct bk_lp *P, const char *f, int len, bk_advf adv, void *ac, int **out)
{
    int cap = 64, n = 0, off = 0; int *pg = malloc(cap * sizeof *pg);
    if (!pg) { *out = 0; return 0; }
    /* saltar parrafos vacios iniciales */
    while (off < len && (f[off] == '\n')) off++;
    while (off < len) {
        if (n == cap) { cap *= 2; int *np = realloc(pg, cap * sizeof *np); if (!np) break; pg = np; }
        pg[n++] = off;
        int nx; bk_layout_page(P, f, len, off, adv, ac, 0, 0, &nx);
        if (nx <= off) nx = off + 1;
        off = nx;
        while (off < len && f[off] == '\n') off++;
    }
    *out = pg;
    return n;
}
/* pagina que contiene el desplazamiento "off" */
static int bk_page_of(const int *pg, int n, int off)
{
    int lo = 0, hi = n - 1, r = 0;
    while (lo <= hi) { int m = (lo + hi) >> 1; if (pg[m] <= off) { r = m; lo = m + 1; } else hi = m - 1; }
    return r;
}
/* estilo (bit0 cursiva, bit1 negrita) en "off" (desde el comienzo de su parrafo) */
static int bk_style_at(const char *f, int off)
{
    int s = off; while (s > 0 && f[s - 1] != '\n') s--;
    int st = 0;
    for (int i = s; i < off; i++) {
        if (f[i] == BKC_I_ON) st |= 1; else if (f[i] == BKC_I_OFF) st &= ~1;
        else if (f[i] == BKC_B_ON) st |= 2; else if (f[i] == BKC_B_OFF) st &= ~2;
    }
    return st;
}

/* ------------------------------------------------------------------ cache de avances */
struct bk_advc { int key[4096], val[4096], n; };
static int bk_adv_cached(struct bk_advc *c, int cp, int size, int (*calc)(int, int))
{
    int k = cp << 7 | (size & 127);
    unsigned h = ((unsigned)k * 2654435761u) >> 20;
    for (int i = 0; i < 4096; i++) {
        int j = (h + i) & 4095;
        if (c->key[j] == k + 1) return c->val[j];
        if (!c->key[j]) {
            if (c->n > 3000) { memset(c->key, 0, sizeof c->key); c->n = 0; return bk_adv_cached(c, cp, size, calc); }
            c->key[j] = k + 1; c->val[j] = calc(cp, size); c->n++;
            return c->val[j];
        }
    }
    return calc(cp, size);
}
