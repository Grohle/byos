/* lrc_core.inc.c - letras .lrc sin dependencias del shell (tests/lyrics_test.c lo incluye tal cual).
 *
 * Lectura (lrc_parse): UTF-8 (con o sin BOM), UTF-16 LE/BE (con BOM o sin el), Latin-1 si no es
 * UTF-8 valido; finales de linea LF, CRLF o CR; marcas [mm:ss], [mm:ss.x], [mm:ss.xx],
 * [mm:ss.xxx], [mm:ss:xx], [h:mm:ss.xx], varias por linea ([00:10.00][01:20.00]texto);
 * metadatos ([ar:] [ti:] [al:] [by:] [length:] [re:] [ve:] [offset:] ...); LRC mejorado
 * (marcas por palabra <mm:ss.xx>: se quitan) y versos ordenados por tiempo (estable).
 *
 * [offset:N] (ms) sigue la especificacion LRC: positivo = la letra sale ANTES. El verso i se
 * muestra cuando posicion >= t_i - offset. El ajuste del usuario se guarda en ese mismo tag.
 *
 * Resultados de LRCLIB (lrc_rank): sincronizadas primero, luego por diferencia de duracion
 * con la cancion (tramos <=2 s, <=5 s, <=15 s, mas), luego por coincidencia de titulo/artista. */
#ifndef LRC_CORE_INC
#define LRC_CORE_INC
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <strings.h>

struct lrc_line { int t, off; };                /* t en ms (-1: sin marca); off: texto en buf */
struct lrc {
    char *buf; int blen;                        /* textos UTF-8 terminados en 0 */
    struct lrc_line *ln; int n;
    int synced;                                 /* 1: con marcas de tiempo */
    int offset, has_offset;                     /* [offset:] en ms */
};

/* ------------------------------------------------------------------ codificacion */
static int lrc_utf8_ok(const unsigned char *s, int n)
{
    for (int i = 0; i < n; ) {
        unsigned c = s[i];
        int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xC2) || i + k >= n) return 0;
        for (int j = 1; j <= k; j++) if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}
static int lrc_put8(char *o, unsigned cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}
/* cualquier .lrc -> UTF-8 sin BOM y sin bytes 0 (malloc; *on = longitud) */
static char *lrc_to_utf8(const char *src, int n, int *on)
{
    const unsigned char *s = (const unsigned char *)src;
    int le = -1, k = 0;
    if (n < 0) n = 0;
    if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { le = 1; s += 2; n -= 2; }
    else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { le = 0; s += 2; n -= 2; }
    else if (n >= 4 && s[0] && !s[1] && s[2] && !s[3]) le = 1;            /* UTF-16 sin BOM */
    else if (n >= 4 && !s[0] && s[1] && !s[2] && s[3]) le = 0;
    char *o;
    if (le >= 0) {
        o = malloc((size_t)n / 2 * 3 + 8); if (!o) return 0;
        for (int i = 0; i + 1 < n; i += 2) {
            unsigned c = le ? (unsigned)(s[i] | s[i + 1] << 8) : (unsigned)(s[i] << 8 | s[i + 1]);
            if (c >= 0xD800 && c < 0xDC00) {
                unsigned d = i + 3 < n ? (le ? (unsigned)(s[i + 2] | s[i + 3] << 8) : (unsigned)(s[i + 2] << 8 | s[i + 3])) : 0;
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; } else c = 0xFFFD;
            } else if (c >= 0xDC00 && c < 0xE000) c = 0xFFFD;
            if (!c || c == 0xFEFF) continue;
            k += lrc_put8(o + k, c);
        }
    } else {
        if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; n -= 3; }
        int ok = lrc_utf8_ok(s, n);
        o = malloc((size_t)n * 2 + 8); if (!o) return 0;
        for (int i = 0; i < n; i++) {
            if (!s[i]) continue;
            if (ok || s[i] < 0x80) o[k++] = (char)s[i];
            else k += lrc_put8(o + k, s[i]);                                 /* Latin-1 */
        }
    }
    o[k] = 0; *on = k;
    return o;
}

/* ------------------------------------------------------------------ marcas */
/* marca de tiempo entre p y e (sin corchetes): ms, o -1 si no lo es */
static int lrc_ts(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
    int neg = 0; if (p < e && *p == '-') { neg = 1; p++; }
    long f[3] = { 0, 0, 0 }; int nd[3] = { 0, 0, 0 }, nf = 0, frac = 0, hasfrac = 0;
    for (;;) {
        if (p >= e || *p < '0' || *p > '9') return -1;
        long v = 0; int d = 0;
        while (p < e && *p >= '0' && *p <= '9') { if (d < 7) v = v * 10 + (*p - '0'); d++; p++; }
        f[nf] = v; nd[nf] = d; nf++;
        if (p == e) break;
        if (*p == ':' && nf < 3) { p++; continue; }
        if (*p == '.' || *p == ',') {
            p++; int m = 100, d2 = 0;
            while (p < e && *p >= '0' && *p <= '9') { if (d2++ < 3) { frac += (*p - '0') * m; m /= 10; } p++; }
            if (p != e || !d2) return -1;
            hasfrac = 1; break;
        }
        return -1;
    }
    long ms;
    if (nf < 2) return -1;
    if (nf == 2) ms = (f[0] * 60 + f[1]) * 1000 + frac;
    else if (hasfrac) ms = ((f[0] * 60 + f[1]) * 60 + f[2]) * 1000 + frac;       /* h:mm:ss.xx */
    else {                                                                        /* mm:ss:xx */
        long v = f[2]; int d = nd[2] < 7 ? nd[2] : 7;
        while (d > 3) { v /= 10; d--; }
        ms = (f[0] * 60 + f[1]) * 1000 + (d == 1 ? v * 100 : d == 2 ? v * 10 : v);
    }
    if (ms > 0x3FFFFFFF) ms = 0x3FFFFFFF;
    return neg ? 0 : (int)ms;
}
/* [clave:valor] de metadatos (clave conocida o de 2 letras minusculas); *val = el valor */
static int lrc_meta(const char *p, const char *e, const char **val)
{
    static const char *const keys[] = { "offset", "length", "lang", "tool", "author", "kana", "sign", "qq", "total", "id", "la", "#", 0 };
    const char *c = memchr(p, ':', (size_t)(e - p));
    if (*p == '#') { *val = c ? c + 1 : e; return 1; }
    if (!c || c == p || c - p > 8) return 0;
    int kl = (int)(c - p), two = kl == 2 && islower((unsigned char)p[0]) && islower((unsigned char)p[1]);
    for (int i = 0; keys[i] && !two; i++) if ((int)strlen(keys[i]) == kl && !strncasecmp(p, keys[i], kl)) two = 1;
    if (!two) return 0;
    *val = c + 1; return 1;
}
/* clasifica una linea (ya sin espacios delante): 1 = metadatos (offset en *off si es [offset:]) */
static int lrc_is_meta_line(const char *l, int *is_offset, int *off)
{
    *is_offset = 0;
    if (*l != '[') return 0;
    const char *e = strchr(l, ']'), *v; if (!e) return 0;
    if (lrc_ts(l + 1, e) >= 0) return 0;
    if (!lrc_meta(l + 1, e, &v)) return 0;
    if (!strncasecmp(l + 1, "offset:", 7)) {
        *is_offset = 1;
        while (v < e && (*v == ' ' || *v == '\t')) v++;
        long x = strtol(v, 0, 10); if (x > 600000) x = 600000; if (x < -600000) x = -600000;
        *off = (int)x;
    }
    return 1;
}

/* ------------------------------------------------------------------ lectura */
static void lrc_free(struct lrc *o) { free(o->buf); free(o->ln); memset(o, 0, sizeof *o); }
static int lrc_emp(const struct lrc *o, int i, const char *empty) { const char *s = o->buf + o->ln[i].off; return !*s || (empty && *empty && !strcmp(s, empty)); }
static int lrc_cmp(const void *a, const void *b)
{
    const struct lrc_line *x = a, *y = b;
    return x->t != y->t ? (x->t < y->t ? -1 : 1) : (x->off != y->off ? (x->off < y->off ? -1 : 1) : 0);
}
/* lee el contenido de un .lrc. empty: texto para los versos vacios (p. ej. "…"; 0 = ""). 0 ok */
static int lrc_parse(struct lrc *o, const char *data, int n, const char *empty)
{
    memset(o, 0, sizeof *o);
    int ul; char *u = lrc_to_utf8(data, n, &ul); if (!u) return -1;
    int nlines = 1; for (int i = 0; i < ul; i++) if (u[i] == '\n' || u[i] == '\r') nlines++;
    int el = empty ? (int)strlen(empty) : 0, cap = 64;
    o->buf = malloc((size_t)ul + (size_t)nlines * (el + 1) + 16);
    o->ln = malloc(cap * sizeof *o->ln);
    if (!o->buf || !o->ln) { free(u); lrc_free(o); return -1; }
    int bo = 0;
    char *l = u;
    while (l) {
        char *nl = l; while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char *next = 0;
        if (*nl) { next = nl + 1; if (*nl == '\r' && *next == '\n') next++; *nl = 0; }
        while (*l == ' ' || *l == '\t') l++;
        int isoff, off;
        if (lrc_is_meta_line(l, &isoff, &off)) { if (isoff) { o->offset = off; o->has_offset = 1; } l = next; continue; }
        int ts[64], nt = 0; char *t = l;
        while (*t == '[') {
            char *e = strchr(t, ']'); if (!e) break;
            int ms = lrc_ts(t + 1, e); if (ms < 0) break;
            if (nt < 64) ts[nt++] = ms;
            t = e + 1;
            char *q = t; while (*q == ' ' || *q == '\t') q++;
            if (*q == '[') t = q;
        }
        /* texto: sin <mm:ss.xx> (LRC mejorado), espacios simples, sin espacios en los bordes */
        int start = bo, sp = 0;
        for (char *p = t; *p; p++) {
            if (*p == '<') { char *e = strchr(p, '>'); if (e && lrc_ts(p + 1, e) >= 0) { p = e; sp = sp || bo > start; continue; } }
            if (*p == ' ' || *p == '\t') { if (bo > start) sp = 1; continue; }
            if (sp) { o->buf[bo++] = ' '; sp = 0; }
            o->buf[bo++] = *p;
        }
        if (bo == start && el) { memcpy(o->buf + bo, empty, el); bo += el; }
        o->buf[bo++] = 0;
        for (int k = 0; k < (nt ? nt : 1); k++) {
            if (o->n == cap) { struct lrc_line *x = realloc(o->ln, cap * 2 * sizeof *x); if (!x) break; o->ln = x; cap *= 2; }
            o->ln[o->n].t = nt ? ts[k] : -1; o->ln[o->n].off = start; o->n++;
        }
        if (nt) o->synced = 1;
        l = next;
    }
    free(u);
    o->blen = bo;
    if (o->synced) {                         /* sin las lineas sin marca; orden estable por tiempo */
        int k = 0; for (int i = 0; i < o->n; i++) if (o->ln[i].t >= 0) o->ln[k++] = o->ln[i];
        o->n = k;
        qsort(o->ln, o->n, sizeof *o->ln, lrc_cmp);
    } else {                                 /* texto: sin vacias al principio/final ni repetidas */
        int k = 0;
        for (int i = 0; i < o->n; i++) {
            if (lrc_emp(o, i, empty) && (!k || lrc_emp(o, k - 1, empty))) continue;
            o->ln[k++] = o->ln[i];
        }
        while (k && lrc_emp(o, k - 1, empty)) k--;
        o->n = k;
    }
    return 0;
}
/* verso actual: el ultimo con t - offset <= pos (-1 antes del primero o sin marcas) */
static int lrc_at(const struct lrc *l, int pos, int offset)
{
    if (!l->synced || !l->n) return -1;
    int a = 0, b = l->n - 1, r = -1;
    while (a <= b) { int m = (a + b) / 2; if (l->ln[m].t - offset <= pos) { r = m; a = m + 1; } else b = m - 1; }
    return r;
}
/* ms hasta el siguiente cambio de verso desde pos (-1: no hay) */
static int lrc_next_in(const struct lrc *l, int pos, int offset)
{
    if (!l->synced || !l->n) return -1;
    int i = lrc_at(l, pos, offset) + 1;
    if (i >= l->n) return -1;
    int d = l->ln[i].t - offset - pos;
    return d > 0 ? d : 1;
}
/* el primer verso con texto (para la vista previa de un resultado) */
static const char *lrc_first_text(const struct lrc *l, const char *empty)
{
    for (int i = 0; i < l->n; i++) { const char *s = l->buf + l->ln[i].off; if (*s && !(empty && !strcmp(s, empty))) return s; }
    return "";
}
/* el .lrc con [offset:] puesto (0: quitado), en UTF-8 con BOM y LF; malloc, *on = longitud */
static char *lrc_with_offset(const char *data, int n, int offset, int *on)
{
    int ul; char *u = lrc_to_utf8(data, n, &ul); if (!u) return 0;
    char *o = malloc((size_t)ul + 64); if (!o) { free(u); return 0; }
    int k = 0, done = offset == 0;
    memcpy(o, "\xEF\xBB\xBF", 3); k = 3;
    char *l = u;
    while (l) {
        char *nl = l; while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char *next = 0;
        if (*nl) { next = nl + 1; if (*nl == '\r' && *next == '\n') next++; *nl = 0; }
        if (!next && !*l) break;                                   /* tras el ultimo salto */
        const char *t = l; while (*t == ' ' || *t == '\t') t++;
        int isoff, off, meta = lrc_is_meta_line(t, &isoff, &off);
        if (isoff) { l = next; continue; }
        if (!done && !meta) { k += sprintf(o + k, "[offset:%+d]\n", offset); done = 1; }
        int len = (int)strlen(l); memcpy(o + k, l, len); k += len; o[k++] = '\n';
        l = next;
    }
    if (!done) k += sprintf(o + k, "[offset:%+d]\n", offset);
    o[k] = 0; *on = k;
    free(u);
    return o;
}

/* ------------------------------------------------------------------ resultados */
struct lrc_cand {
    char id[24], artist[96], title[128], album[96], first[120];
    int dur, synced;                            /* duracion en s (0 = desconocida) */
    int ord, match, dd;                         /* orden de llegada, coincidencia 0..4, |diferencia| s (-1) */
};
static void lrc_cpy(char *d, int cap, const char *s)
{
    int n = (int)strlen(s); if (n >= cap) { n = cap - 1; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    memcpy(d, s, n); d[n] = 0;
}
/* salida de "hibynet lyrics" (id \t artista \t titulo \t album \t dur \t sync [\t primer verso]); modifica txt */
static int lrc_parse_results(char *txt, struct lrc_cand *c, int max)
{
    int n = 0;
    for (char *l = txt, *nl; l && *l && n < max; l = nl) {
        nl = strchr(l, '\n'); if (nl) *nl++ = 0;
        char *f[7] = { 0 }; int nf = 0; f[nf++] = l;
        for (char *p = l; *p && nf < 7; p++) if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        for (int i = 0; i < nf; i++) f[i][strcspn(f[i], "\r")] = 0;
        if (nf < 6 || !f[0][0]) continue;
        struct lrc_cand *x = &c[n]; memset(x, 0, sizeof *x);
        lrc_cpy(x->id, sizeof x->id, f[0]); lrc_cpy(x->artist, sizeof x->artist, f[1]);
        lrc_cpy(x->title, sizeof x->title, f[2]); lrc_cpy(x->album, sizeof x->album, f[3]);
        x->dur = atoi(f[4]); x->synced = atoi(f[5]) != 0;
        if (nf > 6) lrc_cpy(x->first, sizeof x->first, f[6]);
        x->ord = n; x->dd = -1; n++;
    }
    return n;
}
/* forma comparable: sin (...) [...], sin " - ..." ni " feat. ...", minusculas, sin acentos ni signos */
static void lrc_norm(const char *s, char *o, int cap)
{
    char t[256]; int k = 0, depth = 0;
    for (const char *p = s; *p && k < (int)sizeof t - 1; p++) {
        if (*p == '(' || *p == '[') { depth++; continue; }
        if ((*p == ')' || *p == ']') && depth) { depth--; continue; }
        if (depth) continue;
        if (!strncmp(p, " - ", 3) && k) break;
        if (k && (!strncasecmp(p, " feat", 5) || !strncasecmp(p, " ft.", 4) || !strncasecmp(p, " featuring ", 11))) break;
        t[k++] = *p;
    }
    t[k] = 0;
    int j = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p && j < cap - 4; p++) {
        unsigned c = *p;
        if (c < 0x80) { if (isalnum(c)) o[j++] = (char)tolower(c); continue; }
        if (c == 0xC3 && p[1]) {                                     /* Latin-1: sin acentos */
            static const char map[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            unsigned d = p[1]; p++;
            if (d >= 0x80 && d <= 0x9F) d += 0x20;
            if (d >= 0xA0 && d <= 0xBF) { char m = map[d - 0xA0]; if (m != '/') o[j++] = m; }
            continue;
        }
        o[j++] = (char)c;                                             /* resto de UTF-8 tal cual */
    }
    o[j] = 0;
}
static int lrc_match1(const char *a, const char *b)
{
    char x[256], y[256]; lrc_norm(a, x, sizeof x); lrc_norm(b, y, sizeof y);
    if (!x[0] || !y[0]) return 0;
    if (!strcmp(x, y)) return 2;
    return strstr(x, y) || strstr(y, x) ? 1 : 0;
}
static int lrc_bucket(const struct lrc_cand *c, int want)
{
    if (want <= 0) return 0;
    if (c->dd < 0) return 3;
    return c->dd <= 2 ? 0 : c->dd <= 5 ? 1 : c->dd <= 15 ? 2 : 3;
}
static int lrc_rank_want;
static int lrc_rank_cmp(const void *a, const void *b)
{
    const struct lrc_cand *x = a, *y = b;
    if (x->synced != y->synced) return y->synced - x->synced;
    int bx = lrc_bucket(x, lrc_rank_want), by = lrc_bucket(y, lrc_rank_want);
    if (bx != by) return bx - by;
    if (x->match != y->match) return y->match - x->match;
    if (x->dd != y->dd) return (x->dd < 0 ? 1 << 20 : x->dd) - (y->dd < 0 ? 1 << 20 : y->dd);
    return x->ord - y->ord;
}
/* ordena los resultados (want_s: duracion de la cancion en s, 0 = desconocida) y quita repetidos */
static int lrc_rank(struct lrc_cand *c, int n, int want_s, const char *artist, const char *title)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0; for (int j = 0; j < k; j++) if (!strcmp(c[j].id, c[i].id)) dup = 1;
        if (dup) continue;
        if (k != i) c[k] = c[i];
        struct lrc_cand *x = &c[k++];
        x->dd = want_s > 0 && x->dur > 0 ? abs(x->dur - want_s) : -1;
        x->match = (title && *title ? lrc_match1(x->title, title) : 0) + (artist && *artist ? lrc_match1(x->artist, artist) : 0);
    }
    lrc_rank_want = want_s;
    qsort(c, k, sizeof *c, lrc_rank_cmp);
    return k;
}
/* insignia de duracion: 0 = sin datos ("3:42" gris), 1 = coincide (+-2 s, "3:42"), 2 = no ("+12 s") */
static int lrc_dur_badge(int cand_s, int want_s, char *txt, int cap)
{
    if (cand_s <= 0) { txt[0] = 0; return 0; }
    if (want_s <= 0) { snprintf(txt, cap, "%d:%02d", cand_s / 60, cand_s % 60); return 0; }
    int d = cand_s - want_s;
    if (d >= -2 && d <= 2) { snprintf(txt, cap, "%d:%02d", cand_s / 60, cand_s % 60); return 1; }
    if (d >= 60 || d <= -60) snprintf(txt, cap, "%+d:%02d", d / 60, abs(d) % 60);
    else snprintf(txt, cap, "%+d s", d);
    return 2;
}
/* "+0.5 s", "-1.25 s", "0 s" (ms con signo) */
static void lrc_fmt_offset(int ms, char *txt, int cap)
{
    if (!ms) { snprintf(txt, cap, "0 s"); return; }
    int a = abs(ms);
    if (a % 100 == 0) snprintf(txt, cap, "%c%d.%d s", ms > 0 ? '+' : '-', a / 1000, a / 100 % 10);
    else snprintf(txt, cap, "%c%d.%02d s", ms > 0 ? '+' : '-', a / 1000, a / 10 % 100);
}
#endif
