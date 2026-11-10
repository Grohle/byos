/* books_abcore.inc.c - nucleo sin interfaz de la tienda de Libros (audiolibros de LibriVox y
 * libros EPUB de Project Gutenberg). Lo usan books_ab.inc.c (interfaz) y tests/ab_test.c (PC).
 *
 * Fuentes (verificado 2026-11-10, ejemplos reales en tests/ab_samples/):
 *  - Catalogo de LibriVox: la coleccion "librivoxaudio" de archive.org (donde LibriVox aloja sus
 *    MP3). advancedsearch.php filtra por idioma (codigos ISO 639-2: spa, eng, fre/fra, deu...),
 *    titulo y autor, y ordena por descargas o fecha; la API propia de LibriVox
 *    (librivox.org/api/feed/audiobooks) no filtra por idioma ni da tamanos.
 *    Ficha: archive.org/metadata/<id> (metadatos + ficheros: nombre, formato, tamano, duracion,
 *    titulo, pista). Se usan los "64Kbps MP3" (voz: la mitad de espacio); si no hay, los MP3.
 *    Portada: archive.org/services/img/<id> (__ia_thumb.jpg). Descarga:
 *    archive.org/download/<id>/<fichero> (302 a dnNNN.us.archive.org; hibynet sigue la redireccion
 *    y reanuda con Range).
 *  - Gutenberg: gutenberg.org/ebooks/search.opds/?query=.. l.es (catalogo OPDS, Atom).
 *
 * JSON: lector en flujo (abj_parse) que recorre un FILE* caracter a caracter y avisa de cada valor
 * con su ruta (claves e indices); nunca carga el documento entero. Cadenas recortadas a ABJ_VAL.
 *
 * Descargas: cola en BK_DIR/abdl.q (una linea por fichero) que procesa un proceso aparte
 * ("hibyshell --abdl", ab_worker_main) con nice 19 e E/S en clase idle: sigue al salir de Libros,
 * descarga de uno en uno, reanuda los .part, reintenta 3 veces, comprueba el espacio libre y deja
 * su estado en /tmp/hibymod/abdl.st (tmpfs: nada de escrituras frecuentes en la flash). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>

extern char **environ;

/* ================================================================== lector JSON en flujo */
#define ABJ_DEPTH 10
#define ABJ_KEY 40
#define ABJ_VAL 4096
enum { ABJ_STR, ABJ_NUM, ABJ_END };   /* END: fin de un objeto (la ruta es la del objeto) */
struct abj_path { int depth; char key[ABJ_DEPTH][ABJ_KEY]; int idx[ABJ_DEPTH]; };   /* idx >= 0: elemento de lista */
typedef void (*abj_cb)(void *ctx, const struct abj_path *p, int type, const char *v);
struct abj { FILE *f; int c, err; struct abj_path p; abj_cb cb; void *ctx; char v[ABJ_VAL]; };

static void abj_nx(struct abj *j) { j->c = getc(j->f); }
static void abj_ws(struct abj *j) { while (j->c == ' ' || j->c == '\t' || j->c == '\n' || j->c == '\r') abj_nx(j); }
static int abj_hex(struct abj *j)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        abj_nx(j); int c = j->c;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else { j->err = 1; return 0; }
    }
    return v;
}
static int abj_u8(char *o, unsigned cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}
/* lee una cadena (j->c == '"') en out (cap, recortada en un limite de caracter UTF-8) */
static void abj_str(struct abj *j, char *out, int cap)
{
    int n = 0;
    for (;;) {
        abj_nx(j); int c = j->c;
        if (c == EOF) { j->err = 1; break; }
        if (c == '"') { abj_nx(j); break; }
        char t[8]; int k = 1; t[0] = (char)c;
        if (c == '\\') {
            abj_nx(j); c = j->c;
            switch (c) {
            case 'n': t[0] = '\n'; break; case 't': t[0] = '\t'; break; case 'r': t[0] = '\r'; break;
            case 'b': t[0] = '\b'; break; case 'f': t[0] = '\f'; break;
            case 'u': {
                unsigned cp = (unsigned)abj_hex(j);
                if (cp >= 0xD800 && cp < 0xDC00) {   /* par sustituto */
                    abj_nx(j); if (j->c != '\\') { j->err = 1; break; }
                    abj_nx(j); if (j->c != 'u') { j->err = 1; break; }
                    unsigned lo = (unsigned)abj_hex(j);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                k = abj_u8(t, cp ? cp : ' ');
                break;
            }
            case EOF: j->err = 1; break;
            default: t[0] = (char)c;
            }
            if (j->err) break;
        }
        if (n + k < cap) { memcpy(out + n, t, k); n += k; }
    }
    /* no dejar media secuencia UTF-8 al recortar */
    while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
        int s = n - 1; while (s > 0 && ((unsigned char)out[s] & 0xC0) == 0x80) s--;
        int need = ((unsigned char)out[s] >= 0xF0) ? 4 : ((unsigned char)out[s] >= 0xE0) ? 3 : 2;
        if (n - s >= need) break;
        n = s;
    }
    if (n > 0 && (unsigned char)out[n - 1] >= 0xC0) n--;
    if (cap) out[n] = 0;
}
static void abj_value(struct abj *j);
static void abj_push(struct abj *j, const char *key, int idx)
{
    int d = j->p.depth;
    if (d < ABJ_DEPTH) { snprintf(j->p.key[d], ABJ_KEY, "%s", key ? key : ""); j->p.idx[d] = idx; }
    j->p.depth++;
}
static void abj_value(struct abj *j)
{
    abj_ws(j);
    if (j->err) return;
    int c = j->c;
    if (c == '{') {
        abj_nx(j);
        for (;;) {
            abj_ws(j);
            if (j->c == '}') { abj_nx(j); break; }
            if (j->c != '"') { j->err = 1; return; }
            char key[ABJ_KEY]; abj_str(j, key, sizeof key);
            abj_ws(j); if (j->c != ':') { j->err = 1; return; }
            abj_nx(j);
            abj_push(j, key, -1);
            abj_value(j);
            j->p.depth--;
            if (j->err) return;
            abj_ws(j);
            if (j->c == ',') { abj_nx(j); continue; }
            if (j->c == '}') { abj_nx(j); break; }
            j->err = 1; return;
        }
        if (j->p.depth <= ABJ_DEPTH) j->cb(j->ctx, &j->p, ABJ_END, "");
    } else if (c == '[') {
        abj_nx(j);
        for (int i = 0;; i++) {
            abj_ws(j);
            if (j->c == ']') { abj_nx(j); break; }
            abj_push(j, 0, i);
            abj_value(j);
            j->p.depth--;
            if (j->err) return;
            abj_ws(j);
            if (j->c == ',') { abj_nx(j); continue; }
            if (j->c == ']') { abj_nx(j); break; }
            j->err = 1; return;
        }
    } else if (c == '"') {
        abj_str(j, j->v, sizeof j->v);
        if (!j->err && j->p.depth <= ABJ_DEPTH) j->cb(j->ctx, &j->p, ABJ_STR, j->v);
    } else if (c == EOF) {
        j->err = 1;
    } else {   /* numero, true, false, null */
        int n = 0;
        while (j->c != EOF && !strchr(",}] \t\r\n", j->c)) { if (n < 63) j->v[n++] = (char)j->c; abj_nx(j); }
        j->v[n] = 0;
        if (!n) { j->err = 1; return; }
        if (strcmp(j->v, "null") && j->p.depth <= ABJ_DEPTH) j->cb(j->ctx, &j->p, ABJ_NUM, j->v);
    }
}
/* 0 = documento valido */
static int abj_parse(FILE *f, abj_cb cb, void *ctx)
{
    struct abj *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->f = f; j->cb = cb; j->ctx = ctx;
    abj_nx(j); abj_value(j);
    int e = j->err;
    free(j);
    return e ? -1 : 0;
}
static int abj_file(const char *path, abj_cb cb, void *ctx)
{
    FILE *f = fopen(path, "r"); if (!f) return -1;
    int r = abj_parse(f, cb, ctx); fclose(f); return r;
}
/* la ruta coincide con la lista de nombres ("#" = cualquier elemento de lista) */
static int abj_is(const struct abj_path *p, int n, const char *const *names)
{
    if (p->depth != n) return 0;
    for (int i = 0; i < n; i++) {
        if (names[i][0] == '#' && !names[i][1]) { if (p->idx[i] < 0) return 0; }
        else if (p->idx[i] >= 0 || strcmp(p->key[i], names[i])) return 0;
    }
    return 1;
}
#define ABJ_IS(p, ...) abj_is(p, (int)(sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(const char *)), (const char *[]){ __VA_ARGS__ })

/* ================================================================== textos */
static void ab_cpy(char *o, int cap, const char *s) { snprintf(o, cap, "%s", s ? s : ""); }
/* UTF-8 de 2 bytes (0x80..0x7FF) en p; devuelve el codigo o -1 (y *len) */
static int ab_u8_1(const unsigned char *p, int *len)
{
    if (p[0] < 0x80) { *len = 1; return p[0]; }
    if (p[0] >= 0xC2 && (p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *len = 2; return (p[0] & 0x1F) << 6 | (p[1] & 0x3F); }   /* (C0/C1: sobrelargas) */
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[0] != 0xE0 || p[1] >= 0xA0)) { *len = 3; return (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { *len = 4; return 0x10000; }
    *len = 1; return -1;
}
static int ab_u8_valid(const unsigned char *p) { while (*p) { int l; if (ab_u8_1(p, &l) < 0) return 0; p += l; } return 1; }
/* texto codificado en UTF-8 varias veces ("Ã©" en vez de "é"; algunos titulos de archive.org
 * vienen asi, hasta 10 veces): mientras todo sea Latin-1 reinterpretado, se deshace una capa */
static void ab_unmojibake(char *s)
{
    for (int round = 0; round < 12; round++) {
        int hi = 0, n = 0, ok = 1;
        const unsigned char *p = (const unsigned char *)s;
        char *o = malloc(strlen(s) + 1); if (!o) return;
        while (*p) {
            int l, c = ab_u8_1(p, &l);
            if (c < 0) { if (!p[1] || (l == 1 && strlen((const char *)p) < 4)) break; ok = 0; break; }   /* cola cortada: se ignora */
            if (c > 0xFF) {   /* Windows-1252 (0x80..0x9F: "€", "‚"...) */
                static const unsigned short w1252[32] = { 0x20AC, 0, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17D, 0,
                    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0, 0x17E, 0x178 };
                int b = -1; for (int k = 0; k < 32; k++) if (w1252[k] == c) b = 0x80 + k;
                if (b < 0) { ok = 0; break; }
                c = b;
            }
            if (c >= 0x80) hi++;
            o[n++] = (char)c; p += l;
        }
        o[n] = 0;
        /* el resultado tiene que ser UTF-8 valido (salvo una secuencia cortada al final) */
        if (ok && n > 0) {
            int k = n - 1; while (k > 0 && n - k < 4 && ((unsigned char)o[k] & 0xC0) == 0x80) k--;
            unsigned char lead = (unsigned char)o[k];
            int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (need > n - k) { o[k] = 0; n = k; }
            if (!ab_u8_valid((unsigned char *)o)) ok = 0;
        }
        if (!ok || !hi) { free(o); return; }
        strcpy(s, o); free(o);
    }
}
static void ab_cpy_fix(char *o, int cap, const char *v)
{
    if (!strstr(v, "\xC3\x83") && !strstr(v, "\xC3\x82")) { ab_cpy(o, cap, v); return; }   /* sin "Ã"/"Â": nada que arreglar */
    char *t = strdup(v); if (!t) { ab_cpy(o, cap, v); return; }
    ab_unmojibake(t);
    int k = 0; for (int i = 0; t[i]; i++) {   /* fuera los controles C1 que quedan (U+0080..U+009F) */
        if ((unsigned char)t[i] == 0xC2 && (unsigned char)t[i + 1] >= 0x80 && (unsigned char)t[i + 1] <= 0x9F) { i++; continue; }
        t[k++] = t[i];
    }
    t[k] = 0;
    ab_cpy(o, cap, t); free(t);
}
/* recorta espacios por los dos lados (en sitio) */
static void ab_trim(char *s)
{
    int n = (int)strlen(s), a = 0;
    while (a < n && (unsigned char)s[a] <= ' ') a++;
    while (n > a && (unsigned char)s[n - 1] <= ' ') n--;
    memmove(s, s + a, n - a); s[n - a] = 0;
}
/* HTML de las descripciones de archive.org -> texto (parrafos con \n, entidades, espacios) */
static void ab_html_text(const char *in, char *out, int cap)
{
    int n = 0, sp = 1, nl = 0;
    for (const char *p = in; *p && n < cap - 5; ) {
        if (*p == '<') {
            const char *e = strchr(p, '>'); if (!e) break;
            char tag[8] = ""; int k = 0; const char *t = p + 1; if (*t == '/') t++;
            while (k < 7 && isalpha((unsigned char)*t)) tag[k++] = (char)tolower((unsigned char)*t++);
            tag[k] = 0;
            if (!strcmp(tag, "br") || !strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "li")) nl = 1;
            p = e + 1; continue;
        }
        int c = (unsigned char)*p, len = 1; char ent[8]; int el = 0;
        if (c == '&') {
            const char *e = strchr(p, ';');
            if (e && e - p < 10) {
                char nm[10]; int l = (int)(e - p - 1); memcpy(nm, p + 1, l); nm[l] = 0;
                unsigned cp = 0;
                if (nm[0] == '#') cp = (unsigned)(nm[1] == 'x' || nm[1] == 'X' ? strtoul(nm + 2, 0, 16) : strtoul(nm + 1, 0, 10));
                else if (!strcmp(nm, "amp")) cp = '&'; else if (!strcmp(nm, "lt")) cp = '<'; else if (!strcmp(nm, "gt")) cp = '>';
                else if (!strcmp(nm, "quot")) cp = '"'; else if (!strcmp(nm, "apos")) cp = '\''; else if (!strcmp(nm, "nbsp")) cp = ' ';
                else if (!strcmp(nm, "mdash")) cp = 0x2014; else if (!strcmp(nm, "ndash")) cp = 0x2013; else if (!strcmp(nm, "hellip")) cp = 0x2026;
                else if (!strcmp(nm, "rsquo")) cp = 0x2019; else if (!strcmp(nm, "lsquo")) cp = 0x2018; else if (!strcmp(nm, "ldquo")) cp = 0x201C; else if (!strcmp(nm, "rdquo")) cp = 0x201D;
                else {   /* letras latinas: &aacute; &Ntilde; &uuml; &ccedil; &egrave; &ocirc; ... */
                    static const char *const ent[] = { "iexcl", "laquo", "ordf", "ordm", "raquo", "iquest", "szlig" };
                    static const unsigned ecp[] = { 0xA1, 0xAB, 0xAA, 0xBA, 0xBB, 0xBF, 0xDF };
                    for (unsigned q = 0; q < sizeof ent / sizeof *ent; q++) if (!strcmp(nm, ent[q])) cp = ecp[q];
                    static const char vow[] = "aeiouAEIOU";
                    static const unsigned acu[] = { 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA }, gra[] = { 0xE0, 0xE8, 0xEC, 0xF2, 0xF9, 0xC0, 0xC8, 0xCC, 0xD2, 0xD9 },
                                          cir[] = { 0xE2, 0xEA, 0xEE, 0xF4, 0xFB, 0xC2, 0xCA, 0xCE, 0xD4, 0xDB }, uml[] = { 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xC4, 0xCB, 0xCF, 0xD6, 0xDC };
                    const char *v = nm[0] ? strchr(vow, nm[0]) : 0;
                    if (!cp && v) { const char *r = nm + 1; int vi = (int)(v - vow);
                        cp = !strcmp(r, "acute") ? acu[vi] : !strcmp(r, "grave") ? gra[vi] : !strcmp(r, "circ") ? cir[vi] : !strcmp(r, "uml") ? uml[vi] : 0; }
                    if (!cp && !strcmp(nm, "ntilde")) cp = 0xF1; if (!cp && !strcmp(nm, "Ntilde")) cp = 0xD1;
                    if (!cp && !strcmp(nm, "ccedil")) cp = 0xE7; if (!cp && !strcmp(nm, "Ccedil")) cp = 0xC7;
                }
                if (cp) { el = abj_u8(ent, cp); len = (int)(e - p + 1); c = cp < 0x80 ? (int)cp : 0x100; }
            }
        }
        if (c == '\n' || c == '\r') c = ' ';
        if (c == ' ' || c == '\t') { sp = 1; p += len; continue; }
        if (n) { if (nl) { if (out[n - 1] != '\n') out[n++] = '\n'; } else if (sp) out[n++] = ' '; }
        sp = 0; nl = 0;
        if (el) { if (n + el < cap - 1) { memcpy(out + n, ent, el); n += el; } }
        else out[n++] = (char)*p;
        p += len;
    }
    out[n] = 0;
    ab_trim(out);
}
/* "Verne, Jules" -> "Jules Verne"; "A; tr. B" -> "A" */
static void ab_author_norm(char *s)
{
    char *semi = strchr(s, ';'); if (semi) *semi = 0;
    ab_trim(s);
    char *c = strchr(s, ',');
    if (c && !strchr(c + 1, ',') && c[1]) {
        char last[160], first[160];
        int l = (int)(c - s); if (l > 159) l = 159;
        memcpy(last, s, l); last[l] = 0; ab_cpy(first, sizeof first, c + 1); ab_trim(first); ab_trim(last);
        /* "Twain, 1835-1910" y similares: no es un nombre */
        if (first[0] && !isdigit((unsigned char)first[0])) snprintf(s, strlen(s) + 1, "%s %s", first, last);
        else snprintf(s, strlen(s) + 1, "%s", last);
    }
}
/* "Alice's Adventures in Wonderland, by Lewis Carroll" -> sin el ", by ..." */
static void ab_title_norm(char *t)
{
    char *b = strstr(t, ", by ");
    if (b && b > t) *b = 0;
    ab_trim(t);
}
/* duracion: "2:58:51", "41:37", "640.61" (s) -> segundos */
static int ab_parse_dur(const char *s)
{
    if (!s || !*s) return 0;
    if (!strchr(s, ':')) return (int)(atof(s) + 0.5);
    int v = 0;
    for (const char *p = s; *p; ) { v = v * 60 + atoi(p); p = strchr(p, ':'); if (!p) break; p++; }
    return v;
}
/* nombre de fichero seguro para FAT/exFAT (sin / \ : * ? " < > | ni control), recortado a max bytes */
static void ab_sanitize(const char *in, char *out, int max)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n < max; p++) {
        unsigned c = *p;
        if (c == ':') { if (n && out[n - 1] != ' ' && n < max - 1) out[n++] = ' '; if (n < max) out[n++] = '-'; continue; }   /* "A: B" -> "A - B" */
        if (c < 0x20 || strchr("/\\*?\"<>|", (int)c)) { if (n && out[n - 1] != ' ') out[n++] = ' '; continue; }
        out[n++] = (char)c;
    }
    out[n] = 0;
    /* cortar en un limite de caracter */
    while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) { int s = n - 1; while (s > 0 && ((unsigned char)out[s] & 0xC0) == 0x80) s--;
        int need = (unsigned char)out[s] >= 0xF0 ? 4 : (unsigned char)out[s] >= 0xE0 ? 3 : 2; if (n - s >= need) break; n = s; out[n] = 0; }
    if (n > 0 && (unsigned char)out[n - 1] >= 0xC0) out[--n] = 0;
    ab_trim(out);
    n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) out[--n] = 0;
    if (!out[0]) snprintf(out, max + 1, "%s", "_");
}
/* %-codificacion de una parte de URL (keep: caracteres que se dejan, p. ej. "/") */
static void ab_urlenc(const char *s, char *out, int cap, const char *keep)
{
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 4 < cap; p++) {
        if (isalnum(*p) || strchr("-_.~", *p) || (keep && strchr(keep, *p))) out[k++] = (char)*p;
        else k += snprintf(out + k, cap - k, "%%%02X", *p);
    }
    out[k] = 0;
}
static uint64_t ab_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

/* ================================================================== idiomas */
struct ab_lang { const char *code, *q, *gut, *name; };   /* code: clave guardada; q: archive.org; gut: gutenberg.org (l.xx) */
static const struct ab_lang ab_langs[] = {
    { "spa", "spa", "es", "Español" }, { "eng", "eng", "en", "English" }, { "fra", "fre OR fra", "fr", "Français" },
    { "deu", "deu OR ger", "de", "Deutsch" }, { "ita", "ita", "it", "Italiano" }, { "por", "por", "pt", "Português" },
    { "nld", "nld OR dut", "nl", "Nederlands" }, { "rus", "rus", "ru", "Русский" }, { "pol", "pol", "pl", "Polski" },
    { "jpn", "jpn", "ja", "日本語" }, { "zho", "zho OR chi", "zh", "中文" }, { "ukr", "ukr", "uk", "Українська" },
    { "lat", "lat", "la", "Latina" }, { "all", "", "", 0 },
};
#define AB_NLANG ((int)(sizeof ab_langs / sizeof ab_langs[0]))
static int ab_lang_find(const char *code) { for (int i = 0; i < AB_NLANG; i++) if (!strcmp(ab_langs[i].code, code)) return i; return -1; }

/* ================================================================== resultados de busqueda */
#define AB_MAXHIT 60
struct ab_hit { char id[96], title[160], author[120], runtime[16], lang[8], cover[200], desc[700]; };
struct ab_hits { struct ab_hit h[AB_MAXHIT]; int n; long found; };
static void ab_search_cb(void *c, const struct abj_path *p, int type, const char *v)
{
    struct ab_hits *r = c;
    if (type == ABJ_NUM && ABJ_IS(p, "response", "numFound")) { r->found = atol(v); return; }
    if (p->depth < 3 || strcmp(p->key[0], "response") || strcmp(p->key[1], "docs") || p->idx[2] < 0) return;
    int i = p->idx[2]; if (i >= AB_MAXHIT) return;
    if (i >= r->n) { memset(&r->h[i], 0, sizeof r->h[i]); r->n = i + 1; }
    struct ab_hit *h = &r->h[i];
    if (type == ABJ_END) { ab_title_norm(h->title); ab_author_norm(h->author); return; }
    if (p->depth == 4 && p->idx[3] < 0) {
        const char *k = p->key[3];
        if (!strcmp(k, "identifier")) ab_cpy(h->id, sizeof h->id, v);
        else if (!strcmp(k, "title")) ab_cpy_fix(h->title, sizeof h->title, v);
        else if (!strcmp(k, "creator")) ab_cpy_fix(h->author, sizeof h->author, v);
        else if (!strcmp(k, "runtime")) ab_cpy(h->runtime, sizeof h->runtime, v);
        else if (!strcmp(k, "language")) ab_cpy(h->lang, sizeof h->lang, v);
    } else if (p->depth == 5 && p->idx[4] == 0 && !strcmp(p->key[3], "creator")) ab_cpy(h->author, sizeof h->author, v);
    else if (p->depth == 5 && p->idx[4] == 0 && !strcmp(p->key[3], "language")) ab_cpy(h->lang, sizeof h->lang, v);
}
/* quita entradas sin id (y las colecciones que no son un libro) */
static void ab_hits_clean(struct ab_hits *r)
{
    int k = 0;
    for (int i = 0; i < r->n; i++) {
        struct ab_hit *h = &r->h[i];
        if (!h->id[0] || strstr(h->id, "M4bCollection")) continue;
        if (!h->title[0]) ab_cpy(h->title, sizeof h->title, h->id);
        if (k != i) r->h[k] = *h;
        k++;
    }
    r->n = k;
}
static int ab_search_parse(const char *path, struct ab_hits *r)
{
    memset(r, 0, sizeof *r);
    int e = abj_file(path, ab_search_cb, r);
    ab_hits_clean(r);
    return e;
}
/* URL de busqueda en archive.org. sort: 0 populares, 1 recientes, 2 relevancia */
static void ab_search_url(char *o, int cap, int lang, const char *query, int sort, int page)
{
    char q[700]; int n = snprintf(q, sizeof q, "collection:librivoxaudio AND mediatype:audio");
    if (lang >= 0 && lang < AB_NLANG && ab_langs[lang].q[0]) n += snprintf(q + n, sizeof q - n, " AND language:(%s)", ab_langs[lang].q);
    if (query && *query) {
        char w[200]; int k = 0;
        for (const unsigned char *p = (const unsigned char *)query; *p && k < 190; p++)   /* sin sintaxis de Lucene */
        {
            char ch = (*p < 0x80 && !isalnum(*p) && *p != '\'') ? ' ' : (char)*p;
            if (ch != ' ' || (k && w[k - 1] != ' ')) w[k++] = ch;
        }
        w[k] = 0; ab_trim(w);
        if (w[0]) n += snprintf(q + n, sizeof q - n, " AND (title:(%s) OR creator:(%s))", w, w);
    }
    char eq[2100]; ab_urlenc(q, eq, sizeof eq, 0);
    snprintf(o, cap, "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title&fl%%5B%%5D=creator"
             "&fl%%5B%%5D=runtime&fl%%5B%%5D=language%s&rows=40&page=%d&output=json", eq,
             sort == 0 ? "&sort%5B%5D=downloads+desc" : sort == 1 ? "&sort%5B%5D=publicdate+desc" : "", page < 1 ? 1 : page);
}
static void ab_cover_url(const char *id, char *o, int cap) { char e[200]; ab_urlenc(id, e, sizeof e, 0); snprintf(o, cap, "https://archive.org/services/img/%s", e); }
static void ab_meta_url(const char *id, char *o, int cap) { char e[200]; ab_urlenc(id, e, sizeof e, 0); snprintf(o, cap, "https://archive.org/metadata/%s", e); }

/* ---------------------------------------------- Gutenberg (catalogo OPDS de gutenberg.org) */
/* El catalogo oficial responde en menos de un segundo (el espejo Gutendex tardaba minutos o daba
 * 503). search.opds: 25 libros por pagina; "l.xx" en la consulta filtra por idioma; sort_order
 * downloads | release_date. sort: 0 populares, 1 novedades. */
static char *ab_read_small(const char *path, int max);
static void ab_gut_url(char *o, int cap, int lang, const char *query, int sort, int page)
{
    char q[300]; int k = 0;
    for (const unsigned char *p = (const unsigned char *)(query ? query : ""); *p && k < 200; p++) {   /* sin operadores del buscador */
        char ch = (*p < 0x80 && !isalnum(*p) && *p != '\'') ? ' ' : (char)*p;
        if (ch != ' ' || (k && q[k - 1] != ' ')) q[k++] = ch;
    }
    q[k] = 0; ab_trim(q); k = (int)strlen(q);
    if (lang >= 0 && lang < AB_NLANG && ab_langs[lang].gut[0]) snprintf(q + k, sizeof q - k, "%sl.%s", k ? " " : "", ab_langs[lang].gut);
    char eq[1000]; ab_urlenc(q, eq, sizeof eq, 0);
    int n = snprintf(o, cap, "https://www.gutenberg.org/ebooks/search.opds/?sort_order=%s", sort == 1 ? "release_date" : "downloads");
    if (eq[0] && n < cap) n += snprintf(o + n, cap - n, "&query=%s", eq);
    if (page > 1 && n < cap) snprintf(o + n, cap - n, "&start_index=%d", (page - 1) * 25 + 1);
}
/* texto de <tag>...</tag> dentro de [s, fin de cadena): sin marcas, entidades resueltas y en una linea */
static int ab_xml_field(const char *s, const char *tag, char *out, int cap)
{
    out[0] = 0;
    char open[24], close[24]; snprintf(open, sizeof open, "<%s", tag); snprintf(close, sizeof close, "</%s>", tag);
    int ol = (int)strlen(open);
    const char *a = s;
    while ((a = strstr(a, open)) && a[ol] != '>' && a[ol] != ' ') a += ol;
    if (!a) return -1;
    a = strchr(a, '>'); if (!a) return -1;
    a++;
    const char *e = strstr(a, close); if (!e) return -1;
    int n = (int)(e - a); if (n > 2000) n = 2000;
    char *t = malloc(n + 1); if (!t) return -1;
    memcpy(t, a, n); t[n] = 0;
    ab_html_text(t, out, cap); free(t);
    int k = 0;
    for (int i = 0; out[i]; i++) { char ch = (unsigned char)out[i] < ' ' ? ' ' : out[i]; if (ch != ' ' || (k && out[k - 1] != ' ')) out[k++] = ch; }
    out[k] = 0; ab_trim(out);
    return 0;
}
/* el catalogo anade el idioma al titulo de los libros que no estan en ingles: "Don Quijote (Spanish)" */
static void ab_gut_title_fix(char *t)
{
    static const char *const ln[] = { "Spanish", "French", "German", "Italian", "Portuguese", "Dutch", "Finnish", "Chinese", "Japanese", "Latin",
        "Greek", "Tagalog", "Esperanto", "Swedish", "Hungarian", "Polish", "Russian", "Catalan", "Danish", "Norwegian", "Czech", "English",
        "Korean", "Ukrainian", "Thai", "Welsh", "Icelandic", "Bulgarian", "Hebrew", "Serbian", "Romanian", "Afrikaans", "Frisian", "Irish",
        "Arabic", "Slovenian", "Lithuanian", "Turkish", "Galician", "Breton", "Occitan", "Scots", "Friulian", "Middle English", "Old English",
        "Interlingua", "Telugu", "Sanskrit", "Nahuatl", "Estonian", "Latvian", "Slovak", "Croatian", "Basque", "Yiddish", "Persian", "Hindi",
        "Bengali", "Vietnamese", "Indonesian", "Malay", "Cebuano", "Iloko", "Maori", "Scottish Gaelic", "Low German", "Neapolitan" };
    for (;;) {
        int n = (int)strlen(t), hit = 0;
        if (n < 4 || t[n - 1] != ')') return;
        for (unsigned i = 0; i < sizeof ln / sizeof ln[0] && !hit; i++) {
            int l = (int)strlen(ln[i]);
            if (n > l + 3 && t[n - l - 2] == '(' && t[n - l - 3] == ' ' && !strncmp(t + n - l - 1, ln[i], l)) { t[n - l - 3] = 0; hit = 1; }
        }
        if (!hit) return;
    }
}
static int ab_gut_parse(const char *path, struct ab_hits *r)
{
    memset(r, 0, sizeof *r);
    char *b = ab_read_small(path, 320 * 1024); if (!b) return -1;
    if (!strstr(b, "<feed")) { free(b); return -1; }
    for (char *e = strstr(b, "<entry>"); e && r->n < AB_MAXHIT; ) {
        char *end = strstr(e, "</entry>"); if (!end) break;
        *end = 0;
        /* solo los libros (<id>.../ebooks/N.opds</id>); el resto son enlaces de navegacion */
        const char *id = strstr(e, "<id>"), *x = id ? strstr(id, "/ebooks/") : 0;
        int k = 0;
        if (x) { x += 8; while (isdigit((unsigned char)x[k])) k++; }
        if (x && k > 0 && k < 12 && !strncmp(x + k, ".opds<", 6)) {
            struct ab_hit *h = &r->h[r->n]; memset(h, 0, sizeof *h);
            snprintf(h->id, sizeof h->id, "%.*s", k, x);
            ab_xml_field(e, "title", h->title, sizeof h->title); ab_gut_title_fix(h->title);
            ab_xml_field(e, "content", h->author, sizeof h->author);
            { const char *q = h->author; while (isdigit((unsigned char)*q)) q++; if (q != h->author && !strcmp(q, " downloads")) h->author[0] = 0; }   /* sin autor: "157 downloads" */
            snprintf(h->cover, sizeof h->cover, "https://www.gutenberg.org/cache/epub/%s/pg%s.cover.medium.jpg", h->id, h->id);
            ab_cpy(h->runtime, sizeof h->runtime, "epub");
            if (h->title[0]) r->n++;
        }
        e = strstr(end + 1, "<entry>");
    }
    free(b);
    return 0;
}
/* EPUB de Gutenberg: la version con imagenes (epub3), como la de formats */
static void ab_gut_epub_url(const char *id, char *o, int cap) { snprintf(o, cap, "https://www.gutenberg.org/ebooks/%s.epub3.images", id); }
/* ficha OPDS de un libro (/ebooks/N.opds): de ella sale el resumen ("Summary: ...") */
static void ab_gut_detail_url(const char *id, char *o, int cap) { snprintf(o, cap, "https://www.gutenberg.org/ebooks/%s.opds", id); }
static int ab_gut_detail_parse(const char *path, char *desc, int cap)
{
    desc[0] = 0;
    char *b = ab_read_small(path, 96 * 1024); if (!b) return -1;
    int ok = strstr(b, "<entry") != 0;
    char *s = strstr(b, "Summary:");
    if (s) {
        s += 8; char *e = strstr(s, "</p>");
        if (e) {
            *e = 0; ab_html_text(s, desc, cap);
            int k = 0;
            for (int i = 0; desc[i]; i++) { char ch = (unsigned char)desc[i] < ' ' ? ' ' : desc[i]; if (ch != ' ' || (k && desc[k - 1] != ' ')) desc[k++] = ch; }
            desc[k] = 0;
            char *x = strstr(desc, "(This is an automatically generated summary.)"); if (x) *x = 0;
            ab_trim(desc);
        }
    }
    free(b);
    return ok ? 0 : -1;
}

/* ================================================================== ficha de un audiolibro */
#define AB_MAXSEC 400
struct ab_sec { char title[96], remote[160], file[140]; long long size; int dur, track; };
struct ab_book {
    char id[96], title[160], author[120], lang[8], runtime[16];
    char desc[1600];
    int nsec; struct ab_sec *sec;
    long long total; int totdur;
};
struct ab_mctx { struct ab_book *b; struct ab_sec cur; struct ab_sec *alt; int nalt; char fmt[32], desc[ABJ_VAL]; int nauth; };
static int ab_fmt_rank(const char *f)
{
    if (!strcasecmp(f, "64Kbps MP3")) return 1;
    if (!strcasecmp(f, "VBR MP3") || !strcasecmp(f, "128Kbps MP3") || !strcasecmp(f, "MP3")) return 2;
    return 0;
}
static void ab_meta_cb(void *c, const struct abj_path *p, int type, const char *v)
{
    struct ab_mctx *m = c; struct ab_book *b = m->b;
    if (p->depth >= 2 && !strcmp(p->key[0], "metadata") && p->idx[0] < 0) {
        const char *k = p->key[1]; int first = p->depth == 2 || (p->depth == 3 && p->idx[2] == 0);
        if (type == ABJ_END || !first) return;
        if (!strcmp(k, "identifier")) ab_cpy(b->id, sizeof b->id, v);
        else if (!strcmp(k, "title")) ab_cpy_fix(b->title, sizeof b->title, v);
        else if (!strcmp(k, "creator")) ab_cpy_fix(b->author, sizeof b->author, v);
        else if (!strcmp(k, "runtime")) ab_cpy(b->runtime, sizeof b->runtime, v);
        else if (!strcmp(k, "language")) ab_cpy(b->lang, sizeof b->lang, v);
        else if (!strcmp(k, "description")) ab_cpy_fix(m->desc, sizeof m->desc, v);
        return;
    }
    if (p->depth < 2 || strcmp(p->key[0], "files") || p->idx[1] < 0) return;
    if (p->depth == 2 && type == ABJ_END) {   /* fin de un fichero */
        int rk = ab_fmt_rank(m->fmt);
        if (rk && m->cur.remote[0] && !strchr(m->cur.remote, '/')) {
            if (rk == 1 && b->nsec < AB_MAXSEC) b->sec[b->nsec++] = m->cur;
            else if (rk == 2 && m->nalt < AB_MAXSEC) m->alt[m->nalt++] = m->cur;
        }
        memset(&m->cur, 0, sizeof m->cur); m->fmt[0] = 0;
        return;
    }
    if (p->depth != 3) return;
    const char *k = p->key[2];
    if (!strcmp(k, "name")) ab_cpy(m->cur.remote, sizeof m->cur.remote, v);
    else if (!strcmp(k, "format")) ab_cpy(m->fmt, sizeof m->fmt, v);
    else if (!strcmp(k, "size")) m->cur.size = atoll(v);
    else if (!strcmp(k, "length")) m->cur.dur = ab_parse_dur(v);
    else if (!strcmp(k, "title")) ab_cpy_fix(m->cur.title, sizeof m->cur.title, v);
    else if (!strcmp(k, "track")) m->cur.track = atoi(v);
}
static int ab_sec_cmp(const void *a, const void *b)
{
    const struct ab_sec *x = a, *y = b;
    if (x->track && y->track && x->track != y->track) return x->track - y->track;
    if (!x->track != !y->track) return x->track ? -1 : 1;
    return strcmp(x->remote, y->remote);
}
/* titulo de capitulo: sin "01: " / "01 - "; si es el nombre del fichero, vacio (la UI pone "Capitulo N") */
static void ab_sec_title(struct ab_sec *s)
{
    char *t = s->title;
    if (strchr(t, '&') || strchr(t, '<')) { char h[sizeof s->title]; ab_html_text(t, h, sizeof h); ab_cpy(t, sizeof s->title, h); }   /* "&aacute;" */
    ab_trim(t);
    char stem[160]; ab_cpy(stem, sizeof stem, s->remote);
    char *d = strrchr(stem, '.'); if (d) *d = 0;
    int l = (int)strlen(stem); if (l > 5 && !strcmp(stem + l - 5, "_64kb")) stem[l - 5] = 0;
    if (!strcmp(t, stem) || (!strchr(t, ' ') && strchr(t, '_'))) { t[0] = 0; return; }
    char *p = t; while (isdigit((unsigned char)*p)) p++;
    if (p > t && p - t <= 3) {
        char *q = p; while (isalpha((unsigned char)*q) && q - p < 1) q++;   /* "06a:" */
        while (*q == ' ') q++;                                              /* "01 - " */
        if (*q == ':' || *q == '-' || *q == '.') { q++; while (*q == ' ' || *q == '-') q++; if (*q) memmove(t, q, strlen(q) + 1); }
        else if (p - t >= 2 && *p == ' ' && q > p && *q) memmove(t, q, strlen(q) + 1);   /* "002 El aguila" (numero de pista) */
    }
    ab_trim(t);
}
static void ab_book_free(struct ab_book *b) { free(b->sec); memset(b, 0, sizeof *b); }
/* nombre local del capitulo i: "NN - titulo.mp3" */
static void ab_sec_local(const struct ab_sec *s, int i, int n, const char *fallback, char *o, int cap)
{
    char t[100]; ab_sanitize(s->title[0] ? s->title : fallback, t, 60);
    snprintf(o, cap, n >= 100 ? "%03d - %s.mp3" : "%02d - %s.mp3", i + 1, t);
}
static void ab_book_finish(struct ab_book *b)
{
    qsort(b->sec, b->nsec, sizeof *b->sec, ab_sec_cmp);
    b->total = 0; b->totdur = 0;
    for (int i = 0; i < b->nsec; i++) {
        ab_sec_title(&b->sec[i]);
        char fb[32]; snprintf(fb, sizeof fb, "%d", i + 1);
        ab_sec_local(&b->sec[i], i, b->nsec, fb, b->sec[i].file, sizeof b->sec[i].file);
        b->total += b->sec[i].size; b->totdur += b->sec[i].dur;
    }
    if (!b->totdur) b->totdur = ab_parse_dur(b->runtime);
}
/* ficha de archive.org (metadata/<id>) -> b. 0 = bien y con capitulos */
static int ab_meta_parse(const char *path, struct ab_book *b)
{
    memset(b, 0, sizeof *b);
    struct ab_mctx *m = calloc(1, sizeof *m);
    b->sec = malloc(AB_MAXSEC * sizeof *b->sec);
    if (m) m->alt = malloc(AB_MAXSEC * sizeof *m->alt);
    if (!m || !b->sec || !m->alt) { if (m) free(m->alt); free(m); ab_book_free(b); return -1; }
    m->b = b;
    int e = abj_file(path, ab_meta_cb, m);
    if (!b->nsec && m->nalt) { memcpy(b->sec, m->alt, m->nalt * sizeof *b->sec); b->nsec = m->nalt; }
    ab_html_text(m->desc, b->desc, sizeof b->desc);
    free(m->alt); free(m);
    ab_title_norm(b->title); ab_author_norm(b->author);
    ab_book_finish(b);
    if (e || !b->nsec) return e ? -1 : -2;
    return 0;
}

/* ---------------------------------------------- book.json (junto a los MP3) */
static void ab_jesc(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
static int ab_book_save(const struct ab_book *b, const char *path)
{
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    fputs("{\"format\":\"hibyos-audiobook-1\",\"source\":\"librivox\",\"id\":", f); ab_jesc(f, b->id);
    fputs(",\n\"title\":", f); ab_jesc(f, b->title);
    fputs(",\n\"author\":", f); ab_jesc(f, b->author);
    fputs(",\n\"language\":", f); ab_jesc(f, b->lang);
    fputs(",\n\"runtime\":", f); ab_jesc(f, b->runtime);
    fputs(",\n\"description\":", f); ab_jesc(f, b->desc);
    fputs(",\n\"sections\":[", f);
    for (int i = 0; i < b->nsec; i++) {
        const struct ab_sec *s = &b->sec[i];
        fputs(i ? ",\n{\"title\":" : "\n{\"title\":", f); ab_jesc(f, s->title);
        fputs(",\"file\":", f); ab_jesc(f, s->file);
        fputs(",\"remote\":", f); ab_jesc(f, s->remote);
        fprintf(f, ",\"size\":%lld,\"duration\":%d}", s->size, s->dur);
    }
    fputs("]}\n", f);
    int ok = !ferror(f); ok &= !fclose(f);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
struct ab_lctx { struct ab_book *b; int full; };
static void ab_local_cb(void *c, const struct abj_path *p, int type, const char *v)
{
    struct ab_lctx *x = c; struct ab_book *b = x->b;
    if (p->depth == 1 && type != ABJ_END) {
        const char *k = p->key[0];
        if (!strcmp(k, "id")) ab_cpy(b->id, sizeof b->id, v);
        else if (!strcmp(k, "title")) ab_cpy(b->title, sizeof b->title, v);
        else if (!strcmp(k, "author")) ab_cpy(b->author, sizeof b->author, v);
        else if (!strcmp(k, "language")) ab_cpy(b->lang, sizeof b->lang, v);
        else if (!strcmp(k, "runtime")) ab_cpy(b->runtime, sizeof b->runtime, v);
        else if (!strcmp(k, "description") && x->full) ab_cpy(b->desc, sizeof b->desc, v);
        return;
    }
    if (p->depth < 2 || strcmp(p->key[0], "sections") || p->idx[1] < 0) return;
    int i = p->idx[1]; if (i >= AB_MAXSEC) return;
    if (i >= b->nsec) { b->nsec = i + 1; if (b->sec) memset(&b->sec[i], 0, sizeof b->sec[i]); }
    if (p->depth != 3 || type == ABJ_END) return;
    if (!strcmp(p->key[2], "duration")) b->totdur += atoi(v);
    else if (!strcmp(p->key[2], "size")) b->total += atoll(v);
    if (!b->sec) return;
    struct ab_sec *s = &b->sec[i];
    if (!strcmp(p->key[2], "title")) ab_cpy(s->title, sizeof s->title, v);
    else if (!strcmp(p->key[2], "file")) ab_cpy(s->file, sizeof s->file, v);
    else if (!strcmp(p->key[2], "remote")) ab_cpy(s->remote, sizeof s->remote, v);
    else if (!strcmp(p->key[2], "size")) s->size = atoll(v);
    else if (!strcmp(p->key[2], "duration")) s->dur = atoi(v);
}
/* full = 0: solo cabecera, total y numero de capitulos (para la cuadricula, sin memoria) */
static int ab_book_load(const char *path, struct ab_book *b, int full)
{
    memset(b, 0, sizeof *b);
    if (full && !(b->sec = calloc(AB_MAXSEC, sizeof *b->sec))) return -1;
    struct ab_lctx x = { b, full };
    int e = abj_file(path, ab_local_cb, &x);
    if (b->nsec > AB_MAXSEC) b->nsec = AB_MAXSEC;
    if (e || !b->id[0] || !b->nsec) { ab_book_free(b); return -1; }
    return 0;
}

/* ================================================================== cola de descargas */
#define AB_ROOT_NAME "Audiobooks"
#define AB_QMAX (512 << 10)
#define AB_STAT "/tmp/hibymod/abdl.st"
#define AB_PIDF "/tmp/hibymod/abdl.pid"
#define AB_ERRF "/tmp/hibymod/abdl.err"
#define AB_MARGIN (16LL << 20)               /* espacio que se deja libre en la SD */
/* linea: book \t kind \t size \t url \t dest     kind: m (MP3), c (portada), e (EPUB) */
struct ab_qe { char book[96], kind, url[400], dest[600]; long long size; };
static int ab_q_parse_line(char *l, struct ab_qe *e)
{
    char *f[5]; int n = 0; f[n++] = l;
    for (char *p = l; *p && n < 5; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    if (n < 5) return -1;
    char *nl = strchr(f[4], '\n'); if (nl) *nl = 0;
    ab_cpy(e->book, sizeof e->book, f[0]); e->kind = f[1][0]; e->size = atoll(f[2]);
    ab_cpy(e->url, sizeof e->url, f[3]); ab_cpy(e->dest, sizeof e->dest, f[4]);
    return e->url[0] && e->dest[0] == '/' ? 0 : -1;
}
/* lee la cola entera (cap AB_QMAX); *n entradas, devuelve el vector (liberar) */
static struct ab_qe *ab_q_load(const char *qpath, int *n)
{
    *n = 0;
    FILE *f = fopen(qpath, "r"); if (!f) return 0;
    struct ab_qe *v = 0; int cap = 0; char l[1100]; long tot = 0;
    while (fgets(l, sizeof l, f) && tot < AB_QMAX) {
        tot += (long)strlen(l);
        struct ab_qe e; if (ab_q_parse_line(l, &e)) continue;
        if (*n == cap) { int nc = cap ? cap * 2 : 32; struct ab_qe *nv = realloc(v, nc * sizeof *v); if (!nv) break; v = nv; cap = nc; }
        v[(*n)++] = e;
    }
    fclose(f);
    return v;
}
static int ab_q_save(const char *qpath, const struct ab_qe *v, int n)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", qpath);
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    for (int i = 0; i < n; i++) fprintf(f, "%s\t%c\t%lld\t%s\t%s\n", v[i].book, v[i].kind, v[i].size, v[i].url, v[i].dest);
    int ok = !ferror(f); ok &= !fclose(f);
    if (!ok || rename(tmp, qpath)) { unlink(tmp); return -1; }
    return 0;
}
static int ab_mkdirs(const char *dir)
{
    char p[600]; ab_cpy(p, sizeof p, dir);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}
static long long ab_free_bytes(const char *path)
{
    char d[600]; ab_cpy(d, sizeof d, path);
    for (;;) {
        struct statvfs v; if (!statvfs(d, &v)) return (long long)v.f_bavail * v.f_frsize;
        char *s = strrchr(d, '/'); if (!s || s == d) return -1; *s = 0;
    }
}
static int ab_in_list(const char *list, const char *id)   /* lista de ids separados por \n */
{
    size_t l = strlen(id);
    for (const char *p = list; p && *p; ) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == l && !strncmp(p, id, l)) return 1;
        p = e ? e + 1 : 0;
    }
    return 0;
}
static char *ab_read_small(const char *path, int max)
{
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char *b = malloc(max + 1); if (!b) { fclose(f); return 0; }
    int n = (int)fread(b, 1, max, f); fclose(f); b[n < 0 ? 0 : n] = 0;
    return b;
}

/* ---------------------------------------------- proceso de descarga */
static volatile sig_atomic_t abw_term; static volatile pid_t abw_child;
static void abw_sig(int s) { (void)s; abw_term = 1; if (abw_child > 0) kill(abw_child, SIGTERM); }
static void abw_stat(const char *book, const char *dest, long long got, long long total, const char *state, int idx, int n)
{
    char b[900]; int l = snprintf(b, sizeof b, "%d\t%s\t%s\t%lld\t%lld\t%d\t%d\t%s\n", (int)getpid(), state, book, got, total, idx, n, dest);
    int f = open(AB_STAT ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { int ok = write(f, b, l) == l; close(f); if (ok) rename(AB_STAT ".tmp", AB_STAT); }
}
static pid_t abw_spawn(char *const argv[], int *efd)
{
    int fd[2] = { -1, -1 }; if (efd && pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (efd) { posix_spawn_file_actions_adddup2(&fa, fd[1], 2); posix_spawn_file_actions_addclose(&fa, fd[0]); posix_spawn_file_actions_addclose(&fa, fd[1]); }
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid; int r = posix_spawn(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (efd) { close(fd[1]); if (r) { close(fd[0]); return -1; } *efd = fd[0]; }
    return r ? -1 : pid;
}
/* descarga una entrada; 0 bien, 1 error de red/HTTP, 2 terminar, 3 tamano incorrecto, 4 no existe (404) */
static int abw_get(const char *net, const struct ab_qe *e, int idx, int n)
{
    char url[400]; ab_cpy(url, sizeof url, e->url); char *bar = strchr(url, '|'); if (bar) *bar = 0;
    char *argv[] = { (char *)net, "get", url, (char *)e->dest, 0 };
    int efd = -1; pid_t pid = abw_spawn(argv, &efd);
    if (pid <= 0) return 1;
    abw_child = pid;
    long long got = 0, total = e->size; char line[128]; int ln = 0; struct timespec lt = { 0, 0 };
    abw_stat(e->book, e->dest, 0, total, "dl", idx, n);
    for (;;) {
        struct pollfd pf = { efd, POLLIN, 0 };
        int pr = poll(&pf, 1, 500);
        if (pr > 0) {
            char b[256]; ssize_t r = read(efd, b, sizeof b);
            if (r <= 0) break;
            for (ssize_t k = 0; k < r; k++) {
                if (b[k] != '\n') { if (ln < (int)sizeof line - 1) line[ln++] = b[k]; continue; }
                line[ln] = 0; ln = 0;
                long long g, t; if (sscanf(line, "progress %lld %lld", &g, &t) == 2) { got = g; if (t > 0) total = t; }
            }
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec - lt.tv_sec) * 1000 + (now.tv_nsec - lt.tv_nsec) / 1000000 >= 400) { lt = now; abw_stat(e->book, e->dest, got, total, "dl", idx, n); }
        } else if (pr < 0 && errno != EINTR) break;
        if (abw_term) break;
    }
    close(efd);
    int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    abw_child = 0;
    if (abw_term) return 2;
    if (!WIFEXITED(st) || WEXITSTATUS(st)) return WIFEXITED(st) && WEXITSTATUS(st) == 1 ? 4 : 1;
    struct stat sb;
    if (e->kind == 'm' && e->size > 0 && (stat(e->dest, &sb) || sb.st_size != e->size)) { unlink(e->dest); return 3; }
    return 0;
}
/* portada: cover.jpg (ya descargada) -> RGB565 cuadrado para la cuadricula (url de la entrada:
 * la ruta de salida va en el campo url tras un "|") */
static void abw_cover_raw(const char *wall, const struct ab_qe *e)
{
    const char *bar = strchr(e->url, '|'); if (!bar) return;
    char *argv[] = { (char *)wall, (char *)e->dest, (char *)(bar + 1), "160", "160", 0 };
    pid_t pid = abw_spawn(argv, 0); if (pid <= 0) return;
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}
/* bucle principal del proceso de descarga. bindir: carpeta de hibynet/hibywall */
static int ab_worker_run(const char *qpath, const char *paused_path, const char *bindir)
{
    char net[300], wall[300];
    snprintf(net, sizeof net, "%s/hibynet", bindir); snprintf(wall, sizeof wall, "%s/hibywall", bindir);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = abw_sig;
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_DFL);
    setenv("HIBYNET_RESUME", "1", 1);
    mkdir("/tmp/hibymod", 0755);
    { char b[24]; int l = snprintf(b, sizeof b, "%d\n", (int)getpid()); int f = open(AB_PIDF, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) { if (write(f, b, l) != l) {} close(f); } }
    char failed[4096] = ""; int nfail_tot = 0;
    struct { char dest[600]; int n; } tries[8]; memset(tries, 0, sizeof tries);
    int rc = 0;
    while (!abw_term) {
        int n; struct ab_qe *q = ab_q_load(qpath, &n);
        char *paused = ab_read_small(paused_path, 8192);
        int pick = -1, left = 0, done = 0;
        for (int i = 0; i < n; i++) {
            struct stat sb;
            if (!stat(q[i].dest, &sb)) { done++; continue; }
            if (paused && ab_in_list(paused, q[i].book)) continue;
            if (ab_in_list(failed, q[i].dest)) continue;
            left++; if (pick < 0) pick = i;
        }
        free(paused);
        if (pick < 0) {
            /* todo descargado (o en pausa / fallido): la cola se queda solo con lo pendiente */
            if (n && done == n) ab_q_save(qpath, 0, 0);
            abw_stat("", "", 0, 0, nfail_tot ? "err" : "idle", 0, 0);
            free(q); break;
        }
        struct ab_qe e = q[pick]; free(q);
        long long need = (e.size > 0 ? e.size : (4LL << 20)) + AB_MARGIN;
        char dir[600]; ab_cpy(dir, sizeof dir, e.dest); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
        long long fb = ab_free_bytes(dir);
        if (fb >= 0 && fb < need) { abw_stat(e.book, e.dest, 0, e.size, "nospace", 0, left); rc = 1; break; }
        if (ab_mkdirs(dir)) { abw_stat(e.book, e.dest, 0, e.size, "nosd", 0, left); rc = 1; break; }
        int r = abw_get(net, &e, done + 1, done + left);
        if (r == 2) break;
        if (r == 0) { if (e.kind == 'c') abw_cover_raw(wall, &e); continue; }
        /* fallo: hasta 3 intentos (5, 15 s) y despues se salta en esta pasada */
        int t = -1; for (int k = 0; k < 8; k++) if (!strcmp(tries[k].dest, e.dest)) t = k;
        if (t < 0) { t = 0; for (int k = 1; k < 8; k++) if (tries[k].n < tries[t].n) t = k; ab_cpy(tries[t].dest, sizeof tries[t].dest, e.dest); tries[t].n = 0; }
        if (++tries[t].n >= 3 || e.kind == 'c' || r == 4) {
            size_t l = strlen(failed); if (l + strlen(e.dest) + 2 < sizeof failed) { strcat(failed, e.dest); strcat(failed, "\n"); }
            nfail_tot++;
            FILE *ef = fopen(AB_ERRF, "a"); if (ef) { fprintf(ef, "%s\t%s\n", e.book, e.dest); fclose(ef); }
            continue;
        }
        abw_stat(e.book, e.dest, 0, e.size, "retry", 0, left);
        for (int s = 0; s < tries[t].n * 5 * 10 && !abw_term; s++) { struct timespec d = { 0, 100000000L }; nanosleep(&d, 0); }
    }
    if (abw_term) abw_stat("", "", 0, 0, "stopped", 0, 0);
    unlink(AB_PIDF);
    return rc;
}
