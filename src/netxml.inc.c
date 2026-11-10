/* netxml.inc.c - small URL and XML helpers shared by the apps that read feeds from the network:
 * Vinyl (freemusic_core.inc.c / freemusic_src.inc.c: RSS, Atom, OPML, catalogs) and Books
 * (books_abcore.inc.c: OPDS catalogs). Pure logic, no UI and no I/O, so the host tests can include
 * it too (tests/freemusic_test.c, tests/ab_test.c).
 *
 * The fm_ / fx_ / fj_ prefixes are historical: these functions were born in Vinyl and keep their
 * names so that its call sites and tests did not have to change.
 *   - text: fj_put (UTF-8 encoder), fm_utf8_fix, fm_cpy, fm_trim, fm_strip_html
 *   - URLs: fm_url_ok, fm_url_host, fm_url_join (relative reference -> absolute URL)
 *   - XML:  fx_elem, fx_attr, fx_text, fx_child, fx_child_attr, fx_entities (a forgiving reader:
 *           no DTD, no namespaces; malformed or truncated input never reads out of bounds) */
#ifndef NETXML_INC_C
#define NETXML_INC_C
#ifndef _GNU_SOURCE
#define _GNU_SOURCE            /* strcasestr */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ------------------------------------------------------------------ text */
/* append the code point cp as UTF-8 at *o (never past end) */
static void fj_put(char **o, char *end, unsigned cp)
{
    char b[4]; int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (*o + n <= end) { memcpy(*o, b, n); *o += n; }
}
/* cut a trailing incomplete UTF-8 character */
static void fm_utf8_fix(char *s)
{
    int n = (int)strlen(s), q = n;
    while (q > 0 && n - q < 4 && ((unsigned char)s[q - 1] & 0xC0) == 0x80) q--;
    if (q <= 0) return;
    unsigned char lead = (unsigned char)s[q - 1];
    if (lead < 0xC0) return;
    int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    if (n - (q - 1) < need) s[q - 1] = 0;
}
/* bounded copy that never leaves half a UTF-8 character */
static void fm_cpy(char *d, int cap, const char *s)
{
    if (cap <= 0) return;
    int n = (int)strlen(s); if (n > cap - 1) n = cap - 1;
    memcpy(d, s, n); d[n] = 0;
    if (n == cap - 1) fm_utf8_fix(d);
}
static void fm_trim(char *s)
{
    char *a = s; while (*a == ' ') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    int n = (int)strlen(s); while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}
static void fm_strip_html(char *s)
{
    char *o = s; int in = 0;
    for (char *p = s; *p; p++) {
        if (*p == '<') { in = 1; if ((p[1] == 'b' || p[1] == 'B') && (p[2] == 'r' || p[2] == 'R')) *o++ = ' '; if (p[1] == 'p' || p[1] == '/') *o++ = ' '; continue; }
        if (in) { if (*p == '>') in = 0; continue; }
        if (*p == '&') {
            static const struct { const char *e; char c; } E[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' }, { "&nbsp;", ' ' } };
            int done = 0;
            for (unsigned i = 0; i < sizeof E / sizeof E[0]; i++) { int l = (int)strlen(E[i].e); if (!strncmp(p, E[i].e, l)) { *o++ = E[i].c; p += l - 1; done = 1; break; } }
            if (done) continue;
        }
        *o++ = *p;
    }
    *o = 0;
    /* collapse spaces */
    char *w = s; int sp = 1;
    for (char *p = s; *p; p++) { if (*p == ' ') { if (sp) continue; sp = 1; } else sp = 0; *w++ = *p; }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
}

/* ------------------------------------------------------------------ URLs */
static int fm_url_ok(const char *u) { return (!strncasecmp(u, "https://", 8) || !strncasecmp(u, "http://", 7)) && strlen(u) > 10 && !strpbrk(u, " \t\r\n\"'<>"); }
/* "https://user@host:8080/x" -> "host:8080" */
static void fm_url_host(const char *url, char *o, int cap)
{
    const char *p = strstr(url, "://"); p = p ? p + 3 : url;
    const char *at = strchr(p, '@'), *sl = strchr(p, '/');
    if (at && (!sl || at < sl)) p = at + 1;
    if (!strncasecmp(p, "www.", 4)) p += 4;
    int k = 0; while (p[k] && p[k] != '/' && p[k] != '?' && p[k] != '#' && k < cap - 1) { o[k] = p[k]; k++; }
    o[k] = 0;
}
/* relative reference -> absolute URL (base = the catalog / feed URL) */
static void fm_url_join(const char *base, const char *rel, char *o, int cap)
{
    *o = 0; if (!rel || !*rel) return;
    if (strstr(rel, "://")) { fm_cpy(o, cap, rel); return; }
    const char *sch = strstr(base, "://"); if (!sch) { fm_cpy(o, cap, rel); return; }
    if (rel[0] == '/' && rel[1] == '/') { snprintf(o, cap, "%.*s:%s", (int)(sch - base), base, rel); return; }
    const char *hs = sch + 3, *pe = hs; while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    char path[600] = "";
    if (rel[0] == '/') fm_cpy(path, sizeof path, rel);
    else {
        /* directory of the base path + rel */
        const char *q = pe; while (*q && *q != '?' && *q != '#') q++;
        const char *ls = q; while (ls > pe && ls[-1] != '/') ls--;
        int dl = (int)(ls - pe); if (dl <= 0) { snprintf(path, sizeof path, "/%s", rel); }
        else snprintf(path, sizeof path, "%.*s%s", dl, pe, rel);
    }
    /* resolve "./" and "../" (only in the path part) */
    char out[600]; int k = 0; const char *s = path, *qm = strpbrk(path, "?#");
    while (*s && k < (int)sizeof out - 1) {
        if (qm && s >= qm) { out[k++] = *s++; continue; }
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || !s[2] || s[2] == '?')) { s += 2; continue; }
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || !s[3] || s[3] == '?')) { while (k > 0 && out[k - 1] != '/') k--; if (k > 0) k--; s += 3; continue; }
        out[k++] = *s++;
    }
    out[k] = 0;
    if (!out[0] || out[0] != '/') { memmove(out + 1, out, strlen(out) + 1); out[0] = '/'; }
    snprintf(o, cap, "%.*s%s", (int)(pe - base), base, out);
}

/* ------------------------------------------------------------------ XML (RSS, Atom, OPML, OPDS) */
static int fx_namec(char c) { return isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-' || c == '.'; }
/* the ">" closing the tag that starts at t (quoted attribute values skipped) */
static const char *fx_gt(const char *t, const char *e)
{
    char q = 0;
    for (; t < e; t++) { if (q) { if (*t == q) q = 0; } else if (*t == '"' || *t == '\'') q = *t; else if (*t == '>') return t; }
    return 0;
}
static int fx_is(const char *p, const char *e, const char *name)
{
    int n = (int)strlen(name);
    return e - p > n + 1 && p[0] == '<' && !strncasecmp(p + 1, name, n) && !fx_namec(p[1 + n]);
}
/* next element <name ...> in [p, e): *tag = its "<", content [*cs, *ce); returns the position
 * after it (0 if none). CDATA sections and comments are skipped while looking. */
static const char *fx_elem(const char *p, const char *e, const char *name, const char **tag, const char **cs, const char **ce)
{
    while (p && p < e) {
        const char *lt = memchr(p, '<', e - p); if (!lt) return 0;
        if (e - lt > 9 && !strncmp(lt, "<![CDATA[", 9)) { const char *x = strstr(lt + 9, "]]>"); if (!x || x >= e) return 0; p = x + 3; continue; }
        if (e - lt > 4 && !strncmp(lt, "<!--", 4)) { const char *x = strstr(lt + 4, "-->"); if (!x || x >= e) return 0; p = x + 3; continue; }
        if (!fx_is(lt, e, name)) { p = lt + 1; continue; }
        const char *gt = fx_gt(lt, e); if (!gt) return 0;
        if (tag) *tag = lt;
        if (gt[-1] == '/') { if (cs) *cs = gt + 1; if (ce) *ce = gt + 1; return gt + 1; }
        /* the matching close (same-name elements nested: counted) */
        int depth = 1; const char *q = gt + 1;
        while (q < e) {
            const char *l2 = memchr(q, '<', e - q); if (!l2) break;
            if (e - l2 > 9 && !strncmp(l2, "<![CDATA[", 9)) { const char *x = strstr(l2 + 9, "]]>"); if (!x || x >= e) break; q = x + 3; continue; }
            if (e - l2 > 4 && !strncmp(l2, "<!--", 4)) { const char *x = strstr(l2 + 4, "-->"); if (!x || x >= e) break; q = x + 3; continue; }
            if (l2[1] == '/' && !strncasecmp(l2 + 2, name, strlen(name)) && !fx_namec(l2[2 + strlen(name)])) {
                if (--depth == 0) { if (cs) *cs = gt + 1; if (ce) *ce = l2; const char *z = memchr(l2, '>', e - l2); return z ? z + 1 : e; }
            } else if (fx_is(l2, e, name)) { const char *g2 = fx_gt(l2, e); if (g2 && g2[-1] != '/') depth++; }
            q = l2 + 1;
        }
        if (cs) *cs = gt + 1;                                /* not closed: up to the end */
        if (ce) *ce = e;
        return e;
    }
    return 0;
}
/* entities: &amp; &lt; &gt; &quot; &apos; &#N; &#xN; (in place) */
static void fx_entities(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if (*p != '&') { *o++ = *p++; continue; }
        char *sc = strchr(p, ';');
        if (sc && sc - p < 10) {
            unsigned cp = 0; int ok = 1;
            if (p[1] == '#') cp = (unsigned)((p[2] == 'x' || p[2] == 'X') ? strtoul(p + 3, 0, 16) : strtoul(p + 2, 0, 10));
            else if (!strncmp(p, "&amp;", 5)) cp = '&'; else if (!strncmp(p, "&lt;", 4)) cp = '<'; else if (!strncmp(p, "&gt;", 4)) cp = '>';
            else if (!strncmp(p, "&quot;", 6)) cp = '"'; else if (!strncmp(p, "&apos;", 6)) cp = '\''; else if (!strncmp(p, "&nbsp;", 6)) cp = ' ';
            else ok = 0;
            if (ok && cp && cp < 0x110000) {
                char *end = o + 4; if (cp < 0x20) cp = ' ';
                fj_put(&o, end, cp); p = sc + 1; continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
}
/* text of [cs, ce): CDATA unwrapped, entities decoded, tags removed, spaces collapsed */
static void fx_text(const char *cs, const char *ce, char *out, int cap)
{
    *out = 0; if (!cs || ce <= cs || cap < 2) return;
    int k = 0;
    for (const char *p = cs; p < ce && k < cap - 1;) {
        if (ce - p > 9 && !strncmp(p, "<![CDATA[", 9)) {
            const char *x = strstr(p + 9, "]]>"); if (!x || x > ce) x = ce;
            for (const char *q = p + 9; q < x && k < cap - 1; q++) out[k++] = *q == '\n' || *q == '\r' || *q == '\t' ? ' ' : *q;
            p = x + 3; continue;
        }
        out[k++] = *p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p; p++;
    }
    out[k] = 0;
    fx_entities(out);
    if (strstr(out, "</") || strstr(out, "<br") || strstr(out, "<p") || strstr(out, "<a ") || strstr(out, "/>")) fm_strip_html(out);   /* HTML, not text like "<live>" */
    else { char *w = out; int sp = 1; for (char *p = out; *p; p++) { if (*p == ' ') { if (sp) continue; sp = 1; } else sp = 0; *w++ = *p; } while (w > out && w[-1] == ' ') w--; *w = 0; }
    fm_utf8_fix(out);
}
/* attribute of the tag at t ("<name a='x' b="y">") */
static int fx_attr(const char *t, const char *e, const char *name, char *out, int cap)
{
    *out = 0;
    const char *gt = fx_gt(t, e); if (!gt) return 0;
    const char *p = t + 1; while (p < gt && fx_namec(*p)) p++;
    int n = (int)strlen(name);
    while (p < gt) {
        while (p < gt && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '/')) p++;
        const char *an = p; while (p < gt && fx_namec(*p)) p++;
        int al = (int)(p - an);
        while (p < gt && *p == ' ') p++;
        if (p >= gt || *p != '=') { if (p == an) p++; continue; }
        p++; while (p < gt && *p == ' ') p++;
        char q = *p; if (q != '"' && q != '\'') return 0;
        const char *vs = ++p; while (p < gt && *p != q) p++;
        if (al == n && !strncasecmp(an, name, n)) {
            int l = (int)(p - vs); if (l > cap - 1) l = cap - 1;
            memcpy(out, vs, l); out[l] = 0; fx_entities(out); fm_trim(out); fm_utf8_fix(out);
            return 1;
        }
        p++;
    }
    return 0;
}
/* text of the first child <name> in [cs, ce) that is not inside one of the "skip" elements */
static int fx_child(const char *cs, const char *ce, const char *name, char *out, int cap)
{
    *out = 0; const char *t, *a, *b;
    const char *p = cs;
    while ((p = fx_elem(p, ce, name, &t, &a, &b))) {
        /* inside <image>...</image> or <itunes:owner>? (channel title vs image title) */
        int inside = 0; const char *sk[3] = { "image", "itunes:owner", "author" };
        for (int s = 0; s < 3 && !inside; s++) {
            if (!strcasecmp(name, sk[s])) continue;
            const char *it2, *ia, *ib, *q = cs;
            while ((q = fx_elem(q, ce, sk[s], &it2, &ia, &ib))) { if (t > it2 && t < ib) { inside = 1; break; } if (it2 > t) break; }
        }
        if (inside) continue;
        fx_text(a, b, out, cap);
        return 1;
    }
    return 0;
}
static int fx_child_attr(const char *cs, const char *ce, const char *name, const char *attr, char *out, int cap)
{
    *out = 0; const char *t;
    if (!fx_elem(cs, ce, name, &t, 0, 0)) return 0;
    return fx_attr(t, ce, attr, out, cap);
}

#endif /* NETXML_INC_C */
