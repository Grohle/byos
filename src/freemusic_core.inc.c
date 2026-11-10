/* freemusic_core.inc.c - Vinyl app (freemusic.inc.c; internal id "freemusic"): pure logic with no
 * UI or I/O, so it can be tested on the PC (tests/freemusic_test.c):
 *   - mini JSON reader (fj_*)
 *   - Internet Archive: advancedsearch (search results), /metadata/<id>/metadata and
 *     /metadata/<id>/files (album page: tracks per format, sizes, durations, licence)
 *   - Jamendo v3.0: tracks?groupby=album_id (album search), albums/tracks (album page), errors
 *   - FLAC STREAMINFO from the first bytes of a file (sample rate, bits, channels, samples)
 *   - quality labels ("FLAC 24/96", "MP3 320", "MP3 VBR", "OGG"), licence labels, safe file names,
 *     URL encoding and the search URLs.
 *   - user sources (freemusic_src.inc.c, included at the end): Vinyl catalog JSON, RSS/Atom
 *     podcast feeds, OPML, Subsonic API, archive.org collections, MD5 (Subsonic token).
 * Sample responses: tests/freemusic_samples/ (recorded with curl, see tests/freemusic_test.sh). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define FM_MAXIT 120       /* results kept per list (thumbnails: 6 KB each) */
#define FM_MAXF 700        /* files read from an item (the rest is ignored) */
#define FM_MAXT 300        /* tracks per album */

enum { FQ_FLAC24 = 1, FQ_FLAC = 2, FQ_WAV = 4, FQ_MP3 = 8, FQ_MP3VBR = 16, FQ_OGG = 32 };
#define FQ_LOSSLESS (FQ_FLAC24 | FQ_FLAC | FQ_WAV)
#define FQ_ANYMP3 (FQ_MP3 | FQ_MP3VBR)
enum { FC_LOSSLESS, FC_MP3, FC_OGG, FC_N };          /* what the user downloads */
enum { FM_SRC_IA, FM_SRC_JM, FM_SRC_CAT, FM_SRC_RSS, FM_SRC_SUB };   /* how an item is read (fm_item.src) */

struct fm_item {
    uint8_t src, qstate, jm_dl, etree, thumb_state, est;
    uint8_t srank; short rank, page;                 /* merged lists: source order, position in its answer, page */
    uint32_t sid;                                    /* source (fm_sources) it comes from */
    char id[80], title[140], artist[100], year[12];
    long long size; long downloads;
    unsigned fq; short mp3k;                         /* best constant MP3 bitrate (0: VBR only) */
    int sr, bits, ch;                                /* lossless quality (metadata or STREAMINFO) */
    short ntr; float tlen; long long dsize;          /* from the files list: tracks, duration, size of the best format */
    char img[300];                                   /* cover / tile URL */
    char lic[120];                                   /* licence URL (may be empty) */
    char probe[300];                                 /* Jamendo: FLAC URL to read STREAMINFO from; OPML: the feed URL */
    uint16_t *thumb;                                 /* FM_TH x FM_TH RGB565 (UI) */
};
struct fm_file {
    char name[170];                                  /* IA: file name; Jamendo: full URL */
    char title[110], artist[72], orig[170];
    uint8_t fq, priv, nodl, est; short kbps, track; long long size; float len;
    int uo;                                          /* user sources: offset + 1 of the full URL in fm_album.ub */
};
struct fm_track { char title[120], artist[80]; short no; float len; short f[FC_N]; };
struct fm_album {
    struct fm_item it;
    char desc[700], coll[48], venue[100], cover[170];
    int nf, nt; struct fm_file *f; struct fm_track *t;
    long long csize[FC_N]; short ccount[FC_N]; unsigned cq[FC_N]; short ckbps[FC_N]; uint8_t cdl[FC_N], cest[FC_N];
    int restricted;                                  /* "access-restricted-item" / stream_only */
    char page[200];                                  /* web page of the album (user sources) */
    char *ub; int ubn, ubcap;                        /* URL pool of the files (user sources) */
};
static void fm_album_parts_free(struct fm_album *a) { free(a->f); free(a->t); free(a->ub); a->f = 0; a->t = 0; a->ub = 0; a->nf = a->nt = 0; a->ubn = a->ubcap = 0; }
/* adds a URL to the pool; returns offset + 1 (0 = no memory) */
static int fm_pool_add(struct fm_album *a, const char *s)
{
    int n = (int)strlen(s) + 1;
    if (a->ubn + n > a->ubcap) {
        int nc = a->ubcap ? a->ubcap * 2 : 16384; while (nc < a->ubn + n) nc *= 2;
        char *b = realloc(a->ub, nc); if (!b) return 0;
        a->ub = b; a->ubcap = nc;
    }
    memcpy(a->ub + a->ubn, s, n); a->ubn += n;
    return a->ubn - n + 1;
}
static const char *fm_pool(const struct fm_album *a, int uo) { return uo > 0 && a->ub && uo - 1 < a->ubn ? a->ub + uo - 1 : ""; }

/* ------------------------------------------------------------------ JSON (walk the text) */
static const char *fj_w(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }
static const char *fj_skip_str(const char *p)
{
    if (!p || *p != '"') return 0;
    for (p++; *p; p++) { if (*p == '\\') { if (!*++p) return 0; } else if (*p == '"') return p + 1; }
    return 0;
}
static const char *fj_skip(const char *p)
{
    p = fj_w(p); if (!p || !*p) return 0;
    if (*p == '"') return fj_skip_str(p);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (*p) {
            if (*p == '"') { p = fj_skip_str(p); if (!p) return 0; continue; }
            if (*p == '{' || *p == '[') d++; else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
            p++;
        }
        return 0;
    }
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p;
}
static void fj_put(char **o, char *end, unsigned cp)
{
    char b[4]; int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (*o + n <= end) { memcpy(*o, b, n); *o += n; }
}
static unsigned fj_hex4(const char *v)
{
    unsigned cp = 0;
    for (int i = 0; i < 4; i++) {
        char c = v[i]; cp <<= 4;
        if (c >= '0' && c <= '9') cp |= c - '0'; else if ((c | 32) >= 'a' && (c | 32) <= 'f') cp |= (c | 32) - 'a' + 10; else return 0xFFFFFFFF;
    }
    return cp;
}
/* value -> text: strings unescaped (control characters -> space), numbers/booleans as they are,
 * null -> "", arrays -> their first element. Never cuts a UTF-8 character. */
static void fj_str(const char *v, char *out, int cap)
{
    char *o = out, *end = out + cap - 1; *out = 0;
    v = fj_w(v); if (!v || !*v || cap < 2) return;
    if (*v == '[') { v = fj_w(v + 1); if (!v || *v == ']') return; }
    if (*v != '"') {
        if (!strncmp(v, "null", 4) || *v == '{') return;
        const char *e = fj_skip(v); if (!e) return;
        int n = (int)(e - v); if (n > cap - 1) n = cap - 1; memcpy(out, v, n); out[n] = 0; return;
    }
    for (v++; *v && *v != '"'; v++) {
        if (*v != '\\') { if (o < end) *o++ = (*v == '\n' || *v == '\r' || *v == '\t') ? ' ' : *v; continue; }
        v++;
        if (*v == 'n' || *v == 't' || *v == 'r') { if (o < end) *o++ = ' '; }
        else if (*v == 'u') {
            unsigned cp = fj_hex4(v + 1); if (cp == 0xFFFFFFFF) break;
            v += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
                unsigned lo = fj_hex4(v + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); v += 6; }
            }
            if (cp < 0x20) cp = ' ';
            fj_put(&o, end, cp);
        } else if (!*v) break;
        else if (*v != 'b' && *v != 'f' && o < end) *o++ = *v;
    }
    /* do not leave half a UTF-8 character at the end */
    char *q = o; while (q > out && ((unsigned char)q[-1] & 0xC0) == 0x80) q--;
    if (q > out) {
        unsigned char lead = (unsigned char)q[-1]; int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (o - (q - 1) < need) o = q - 1;
    }
    *o = 0;
}
static const char *fj_get(const char *obj, const char *key)
{
    obj = fj_w(obj); if (!obj || *obj != '{') return 0;
    const char *p = fj_w(obj + 1); char k[64];
    while (p && *p == '"') {
        fj_str(p, k, sizeof k);
        p = fj_w(fj_skip_str(p)); if (!p || *p != ':') return 0;
        p = fj_w(p + 1);
        if (!strcmp(k, key)) return p;
        p = fj_w(fj_skip(p)); if (!p) return 0;
        if (*p == ',') p = fj_w(p + 1); else break;
    }
    return 0;
}
static const char *fj_first(const char *a) { a = fj_w(a); if (!a || *a != '[') return 0; a = fj_w(a + 1); return (!a || !*a || *a == ']') ? 0 : a; }
static const char *fj_next(const char *el) { const char *p = fj_w(fj_skip(el)); if (!p || *p != ',') return 0; p = fj_w(p + 1); return (!*p || *p == ']') ? 0 : p; }
static void fj_gs(const char *obj, const char *key, char *out, int cap) { const char *v = fj_get(obj, key); if (v) fj_str(v, out, cap); else *out = 0; }
static long long fj_gl(const char *obj, const char *key) { char b[32]; fj_gs(obj, key, b, sizeof b); return atoll(b); }
static int fj_gb(const char *obj, const char *key) { char b[16]; fj_gs(obj, key, b, sizeof b); return !strcmp(b, "true") || !strcmp(b, "1"); }
/* any string of a value that is a string or an array of strings contains "needle" (case-insensitive) */
static int fj_has(const char *v, const char *needle)
{
    v = fj_w(v); if (!v) return 0;
    char b[160];
    if (*v == '[') { for (const char *e = fj_first(v); e; e = fj_next(e)) { fj_str(e, b, sizeof b); if (strcasestr(b, needle)) return 1; } return 0; }
    fj_str(v, b, sizeof b); return strcasestr(b, needle) != 0;
}

/* ------------------------------------------------------------------ small helpers */
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
/* "806", "39.8", "13:26", "1:02:03" -> seconds */
static float fm_len(const char *s)
{
    if (!s || !*s) return 0;
    if (!strchr(s, ':')) return (float)atof(s);
    float t = 0; const char *p = s;
    for (;;) { t = t * 60 + (float)atof(p); p = strchr(p, ':'); if (!p) break; p++; }
    return t;
}
/* IA file format -> FQ_* (0: not audio we offer); kbps of constant MP3s */
static unsigned fm_fmt(const char *f, int *kbps)
{
    if (kbps) *kbps = 0;
    if (!strcasecmp(f, "24bit Flac")) return FQ_FLAC24;
    if (!strcasecmp(f, "Flac")) return FQ_FLAC;
    if (!strcasecmp(f, "WAVE")) return FQ_WAV;
    if (!strcasecmp(f, "VBR MP3")) return FQ_MP3VBR;
    if (!strcasecmp(f, "Ogg Vorbis")) return FQ_OGG;
    if (!strcasecmp(f, "MP3")) return FQ_MP3;
    int k = 0; char unit[16] = "";
    if (sscanf(f, "%d%15s", &k, unit) == 2 && !strncasecmp(unit, "Kbps", 4) && strcasestr(f, "MP3") && k > 0 && k <= 512) { if (kbps) *kbps = k; return FQ_MP3; }
    return 0;
}
static const char *fm_khz(int sr, char *b, int n)
{
    if (sr % 1000 == 0) snprintf(b, n, "%d", sr / 1000);
    else snprintf(b, n, "%.1f", sr / 1000.0);
    return b;
}
/* "FLAC 24/96", "FLAC 16/44.1", "FLAC 24-bit", "FLAC", "WAV 16/44.1", "MP3 320", "MP3 VBR", "OGG" */
static void fm_qlabel(unsigned fq, int sr, int bits, int kbps, char *o, int n)
{
    char k[12];
    if (fq & (FQ_FLAC24 | FQ_FLAC | FQ_WAV)) {
        const char *nm = (fq & (FQ_FLAC24 | FQ_FLAC)) ? "FLAC" : "WAV";
        if (sr > 0 && bits > 0) snprintf(o, n, "%s %d/%s", nm, bits, fm_khz(sr, k, sizeof k));
        else if (fq & FQ_FLAC24) snprintf(o, n, "%s 24-bit", nm);
        else snprintf(o, n, "%s", nm);
    } else if (fq & FQ_MP3) { if (kbps > 0) snprintf(o, n, "MP3 %d", kbps); else snprintf(o, n, "MP3"); }
    else if (fq & FQ_MP3VBR) snprintf(o, n, "MP3 VBR");
    else if (fq & FQ_OGG) snprintf(o, n, "OGG");
    else *o = 0;
}
/* 1 = high resolution (more than CD) */
static int fm_hires(unsigned fq, int sr, int bits) { return (fq & FQ_LOSSLESS) && (bits > 16 || sr > 48000 || (!bits && (fq & FQ_FLAC24))); }
/* extension of the file a class downloads */
static const char *fm_ext(unsigned fq) { return (fq & (FQ_FLAC | FQ_FLAC24)) ? "flac" : (fq & FQ_WAV) ? "wav" : (fq & FQ_OGG) ? "ogg" : "mp3"; }
static int fm_class_of(unsigned fq) { return (fq & FQ_LOSSLESS) ? FC_LOSSLESS : (fq & FQ_ANYMP3) ? FC_MP3 : (fq & FQ_OGG) ? FC_OGG : -1; }

/* licence URL -> short label ("CC BY-NC-SA 3.0", "CC0 1.0", "Public Domain Mark"); "" = unknown */
static void fm_lic_label(const char *url, char *o, int n)
{
    *o = 0; if (!url || !*url) return;
    const char *p = strstr(url, "creativecommons.org/");
    if (!p) { snprintf(o, n, "%s", url); return; }
    p += 20;
    char kind[40] = "", ver[16] = "";
    if (!strncmp(p, "licenses/", 9)) { sscanf(p + 9, "%39[^/]/%15[^/]", kind, ver); }
    else if (!strncmp(p, "publicdomain/zero/", 18)) { sscanf(p + 18, "%15[^/]", ver); snprintf(o, n, "CC0 %s", ver); return; }
    else if (!strncmp(p, "publicdomain/mark/", 18)) { snprintf(o, n, "Public Domain Mark"); return; }
    else if (!strncmp(p, "publicdomain", 12)) { snprintf(o, n, "Public Domain"); return; }
    if (!kind[0]) { snprintf(o, n, "Creative Commons"); return; }
    for (char *c = kind; *c; c++) *c = (char)toupper((unsigned char)*c);
    snprintf(o, n, "CC %s%s%s", kind, ver[0] ? " " : "", ver);
}

/* safe file/folder name (FAT/exFAT): no / \ : * ? " < > |, no control characters, no leading
 * or trailing dots/spaces, at most "max" bytes (never half a UTF-8 character); empty -> "Unknown" */
static void fm_safe_name(const char *in, char *o, int max)
{
    int k = 0, sp = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s && k < max - 1; s++) {
        unsigned c = *s;
        if (c < 0x20 || strchr("/\\:*?\"<>|", (int)c)) c = ' ';
        if (c == ' ') { if (!k || sp) continue; sp = 1; } else sp = 0;
        o[k++] = (char)c;
    }
    o[k] = 0;
    fm_utf8_fix(o); k = (int)strlen(o);
    while (k > 0 && (o[k - 1] == ' ' || o[k - 1] == '.')) o[--k] = 0;
    char *a = o; while (*a == '.' || *a == ' ') a++;
    if (a != o) memmove(o, a, strlen(a) + 1);
    if (!*o) snprintf(o, max, "Unknown");
}
static void fm_urlenc(const char *s, char *o, int cap)
{
    static const char hx[] = "0123456789ABCDEF"; int k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k < cap - 4; p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) o[k++] = (char)*p;
        else if (*p == ' ') o[k++] = '+';
        else { o[k++] = '%'; o[k++] = hx[*p >> 4]; o[k++] = hx[*p & 15]; }
    }
    o[k] = 0;
}
/* free text for the Lucene query of archive.org: only letters, digits and spaces (syntax out) */
static void fm_ia_words(const char *s, char *o, int cap)
{
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k < cap - 1; p++) {
        unsigned c = *p;
        if (c < 0x80 && !isalnum((int)c) && c != '\'') c = ' ';
        if (c < 0x80) c = (unsigned)tolower((int)c);      /* lower case: AND/OR/NOT stay plain words */
        if (c == ' ' && (!k || o[k - 1] == ' ')) continue;
        o[k++] = (char)c;
    }
    while (k > 0 && o[k - 1] == ' ') k--;
    o[k] = 0;
}

/* ------------------------------------------------------------------ filters */
struct fm_lang { const char *name, *jm, *ia; };
static const struct fm_lang FM_LANGS[] = {
    { "English", "en", "eng OR English" }, { "Español", "es", "spa OR Spanish OR Español" }, { "Français", "fr", "fre OR fra OR French" },
    { "Deutsch", "de", "ger OR deu OR German" }, { "Italiano", "it", "ita OR Italian" }, { "Português", "pt", "por OR Portuguese" },
    { "日本語", "ja", "jpn OR Japanese" }, { "한국어", "ko", "kor OR Korean" }, { "Polski", "pl", "pol OR Polish" },
    { "Русский", "ru", "rus OR Russian" }, { "中文", "zh", "chi OR zho OR Chinese" }, { "ไทย", "th", "tha OR Thai" },
    { "Українська", "uk", "ukr OR Ukrainian" },
};
#define FM_NLANG ((int)(sizeof FM_LANGS / sizeof FM_LANGS[0]))
struct fm_genre { const char *key, *jm, *ia; };     /* key: Spanish literal for tr() */
static const struct fm_genre FM_GENRES[] = {
    { "Rock", "rock", "rock" }, { "Jazz", "jazz", "jazz" }, { "Blues", "blues", "blues" },
    { "Electrónica", "electronic", "electronic OR electronica" }, { "Clásica", "classical", "classical" },
    { "Folk", "folk", "folk" }, { "Hip hop", "hiphop", "\"hip hop\" OR hiphop" }, { "Ambient", "ambient", "ambient" },
    { "Pop", "pop", "pop" }, { "Metal", "metal", "metal" }, { "Reggae", "reggae", "reggae" },
    { "Experimental", "experimental", "experimental" }, { "Músicas del mundo", "world", "world" }, { "Bandas sonoras", "soundtrack", "soundtrack" },
};
#define FM_NGENRE ((int)(sizeof FM_GENRES / sizeof FM_GENRES[0]))
/* archive.org collections offered */
enum { FCOL_ALL, FCOL_LIVE, FCOL_NETLABEL, FCOL_COMMUNITY, FCOL_N };
static const char *const FM_COLQ[FCOL_N] = {
    "collection:(etree OR netlabels OR audio_music)", "collection:etree", "collection:netlabels", "collection:(audio_music OR opensource_audio)" };
enum { FSORT_POP, FSORT_NEW, FSORT_REL };

#define FM_IA_FIELDS "fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D=creator&fl%5B%5D=date&fl%5B%5D=downloads&fl%5B%5D=format&fl%5B%5D=item_size&fl%5B%5D=licenseurl&fl%5B%5D=collection"
/* collq: "collection:(...)" part of the query (FM_COLQ, or a user's archive.org collection) */
static void fm_ia_search_url2(char *o, int n, const char *text, const char *collq, int lossless, int genre, int lang, int sort, int rows, int page)
{
    char w[160], q[700], e[1400];
    fm_ia_words(text ? text : "", w, sizeof w);
    int k = 0;
    if (w[0]) k += snprintf(q + k, sizeof q - k, "(%s) AND ", w);
    k += snprintf(q + k, sizeof q - k, "%s AND mediatype:(audio OR etree)", collq);
    if (lossless) k += snprintf(q + k, sizeof q - k, " AND format:(Flac OR \"24bit Flac\")");
    else k += snprintf(q + k, sizeof q - k, " AND format:(Flac OR \"24bit Flac\" OR \"VBR MP3\" OR MP3 OR \"Ogg Vorbis\" OR \"320Kbps MP3\" OR \"256Kbps MP3\" OR \"192Kbps MP3\" OR \"128Kbps MP3\")");
    if (genre >= 0 && genre < FM_NGENRE) k += snprintf(q + k, sizeof q - k, " AND subject:(%s)", FM_GENRES[genre].ia);
    if (lang >= 0 && lang < FM_NLANG) k += snprintf(q + k, sizeof q - k, " AND language:(%s)", FM_LANGS[lang].ia);
    fm_urlenc(q, e, sizeof e);
    const char *so = sort == FSORT_NEW ? "&sort%5B%5D=publicdate+desc" : sort == FSORT_REL && w[0] ? "" : "&sort%5B%5D=downloads+desc";
    snprintf(o, n, "https://archive.org/advancedsearch.php?q=%s&%s%s&rows=%d&page=%d&output=json", e, FM_IA_FIELDS, so, rows, page < 1 ? 1 : page);
}
static void fm_ia_search_url(char *o, int n, const char *text, int coll, int lossless, int genre, int lang, int sort, int rows, int page)
{
    fm_ia_search_url2(o, n, text, FM_COLQ[coll >= 0 && coll < FCOL_N ? coll : 0], lossless, genre, lang, sort, rows, page);
}
#define FM_JM_API "https://api.jamendo.com/v3.0"
static void fm_jm_search_url(char *o, int n, const char *key, const char *text, int lossless, int genre, int lang, int sort, int featured, int rows, int offset)
{
    char e[300] = "", ke[120];
    fm_urlenc(key, ke, sizeof ke);
    int k = snprintf(o, n, FM_JM_API "/tracks/?client_id=%s&format=json&limit=%d&offset=%d&groupby=album_id&include=musicinfo&audioformat=mp32&audiodlformat=flac&imagesize=200", ke, rows, offset);
    if (text && *text) { fm_urlenc(text, e, sizeof e); k += snprintf(o + k, n - k, "&search=%s", e); }
    if (genre >= 0 && genre < FM_NGENRE) k += snprintf(o + k, n - k, "&tags=%s", FM_GENRES[genre].jm);
    if (lang >= 0 && lang < FM_NLANG) k += snprintf(o + k, n - k, "&lang=%s", FM_LANGS[lang].jm);
    if (featured) k += snprintf(o + k, n - k, "&featured=1");
    const char *so = sort == FSORT_NEW ? "releasedate_desc" : sort == FSORT_REL && text && *text ? "relevance" : featured ? "popularity_week" : "popularity_total";
    k += snprintf(o + k, n - k, "&order=%s", so);
    (void)lossless;                       /* every album can be downloaded in FLAC when allowed (filtered when parsing) */
}
static void fm_jm_album_url(char *o, int n, const char *key, const char *album_id)
{
    char ke[120]; fm_urlenc(key, ke, sizeof ke);
    snprintf(o, n, FM_JM_API "/albums/tracks/?client_id=%s&format=json&id=%s&audioformat=mp32&audiodlformat=flac&imagesize=300", ke, album_id);
}
static void fm_jm_test_url(char *o, int n, const char *key) { char ke[120]; fm_urlenc(key, ke, sizeof ke); snprintf(o, n, FM_JM_API "/tracks/?client_id=%s&format=json&limit=1", ke); }

/* ------------------------------------------------------------------ Internet Archive: search */
static void fm_year(const char *date, char *o, int n) { int k = 0; while (date[k] && k < 4 && isdigit((unsigned char)date[k])) k++; if (k == 4) snprintf(o, n, "%.4s", date); else *o = 0; }
/* fills o[0..max) from advancedsearch JSON; *found = numFound; returns the number of items, -1 if bad */
static int fm_ia_parse_search(const char *js, struct fm_item *o, int max, long *found)
{
    const char *resp = fj_get(js, "response"); if (found) *found = 0;
    if (!resp) return -1;
    if (found) *found = (long)fj_gl(resp, "numFound");
    int n = 0;
    for (const char *d = fj_first(fj_get(resp, "docs")); d && n < max; d = fj_next(d)) {
        struct fm_item *it = &o[n]; memset(it, 0, sizeof *it);
        it->src = FM_SRC_IA;
        fj_gs(d, "identifier", it->id, sizeof it->id);
        if (!it->id[0] || strpbrk(it->id, "/?#& \"")) continue;
        fj_gs(d, "title", it->title, sizeof it->title); fm_trim(it->title);
        fj_gs(d, "creator", it->artist, sizeof it->artist); fm_trim(it->artist);
        if (!it->title[0]) fm_cpy(it->title, (int)sizeof it->title, it->id);
        char dt[40]; fj_gs(d, "date", dt, sizeof dt); fm_year(dt, it->year, sizeof it->year);
        it->downloads = (long)fj_gl(d, "downloads");
        it->size = fj_gl(d, "item_size");
        fj_gs(d, "licenseurl", it->lic, sizeof it->lic);
        const char *c = fj_get(d, "collection"); it->etree = c && fj_has(c, "etree");
        for (const char *f = fj_first(fj_get(d, "format")); f; f = fj_next(f)) {
            char fs[48]; int kb; fj_str(f, fs, sizeof fs);
            unsigned q = fm_fmt(fs, &kb); it->fq |= q;
            if ((q & FQ_MP3) && kb > it->mp3k) it->mp3k = (short)kb;
        }
        if (!(it->fq & (FQ_LOSSLESS | FQ_ANYMP3 | FQ_OGG))) continue;      /* nothing we can play */
        snprintf(it->img, sizeof it->img, "https://archive.org/services/img/%s", it->id);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ Internet Archive: item */
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
/* the "metadata" object (from /metadata/<id>/metadata = {"result": {...}} or the full item) */
static int fm_ia_parse_meta(const char *js, struct fm_album *a)
{
    const char *m = fj_get(js, "result"); if (!m) m = fj_get(js, "metadata");
    if (!m || *fj_w(m) != '{') return -1;
    struct fm_item *it = &a->it;
    char id[80]; fj_gs(m, "identifier", id, sizeof id); if (id[0] && !it->id[0]) fm_cpy(it->id, (int)sizeof it->id, id);
    it->src = FM_SRC_IA;
    char t[140]; fj_gs(m, "title", t, sizeof t); fm_trim(t); if (t[0]) fm_cpy(it->title, (int)sizeof it->title, t);
    char c[100]; fj_gs(m, "creator", c, sizeof c); fm_trim(c); if (c[0]) fm_cpy(it->artist, (int)sizeof it->artist, c);
    char dt[40]; fj_gs(m, "date", dt, sizeof dt); if (!dt[0]) fj_gs(m, "year", dt, sizeof dt); fm_year(dt, it->year, sizeof it->year);
    fj_gs(m, "licenseurl", it->lic, sizeof it->lic);
    fj_gs(m, "venue", a->venue, sizeof a->venue);
    fj_gs(m, "description", a->desc, sizeof a->desc); fm_strip_html(a->desc);
    const char *co = fj_get(m, "collection");
    if (co) { it->etree = fj_has(co, "etree"); if (fj_has(co, "stream_only")) a->restricted = 1; fj_str(co, a->coll, sizeof a->coll); }
    char ar[16]; fj_gs(m, "access-restricted-item", ar, sizeof ar); if (!strcmp(ar, "true")) a->restricted = 1;
    if (!it->title[0]) fm_cpy(it->title, (int)sizeof it->title, it->id);
    if (!it->img[0] && it->id[0]) snprintf(it->img, sizeof it->img, "https://archive.org/services/img/%s", it->id);
    return 0;
}
static int fm_track_no(const char *s) { while (*s && !isdigit((unsigned char)*s)) { if (*s != ' ' && *s != '(') return 0; s++; } return atoi(s); }
static const char *fm_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
/* natural compare: "d1t2" < "d1t10" */
static int fm_natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
/* best file of a class among two: higher quality wins */
static int fm_better(const struct fm_file *x, const struct fm_file *y)
{
    if (!y) return 1;
    int qx = (x->fq & FQ_FLAC24) ? 4 : (x->fq & FQ_FLAC) ? 3 : (x->fq & FQ_WAV) ? 2 : (x->fq & FQ_MP3) ? 1 + (x->kbps >= 256) : 1;
    int qy = (y->fq & FQ_FLAC24) ? 4 : (y->fq & FQ_FLAC) ? 3 : (y->fq & FQ_WAV) ? 2 : (y->fq & FQ_MP3) ? 1 + (y->kbps >= 256) : 1;
    if (qx != qy) return qx > qy;
    return x->kbps > y->kbps;
}
static struct fm_album *fm_sort_ctx;
static int fm_cmp_no(const void *a, const void *b) { const struct fm_track *x = a, *y = b; return x->no - y->no; }
static int fm_cmp_name(const void *a, const void *b)
{
    const struct fm_track *x = a, *y = b;
    int ix = x->f[FC_LOSSLESS] >= 0 ? x->f[FC_LOSSLESS] : x->f[FC_MP3] >= 0 ? x->f[FC_MP3] : x->f[FC_OGG];
    int iy = y->f[FC_LOSSLESS] >= 0 ? y->f[FC_LOSSLESS] : y->f[FC_MP3] >= 0 ? y->f[FC_MP3] : y->f[FC_OGG];
    return fm_natcmp(fm_sort_ctx->f[ix].orig[0] ? fm_sort_ctx->f[ix].orig : fm_sort_ctx->f[ix].name,
                     fm_sort_ctx->f[iy].orig[0] ? fm_sort_ctx->f[iy].orig : fm_sort_ctx->f[iy].name);
}
/* totals per class (size, count, quality) once the tracks are known */
static void fm_album_totals(struct fm_album *a)
{
    for (int c = 0; c < FC_N; c++) { a->csize[c] = 0; a->ccount[c] = 0; a->cq[c] = 0; a->ckbps[c] = 0; a->cdl[c] = 1; a->cest[c] = 0; }
    for (int t = 0; t < a->nt; t++) for (int c = 0; c < FC_N; c++) {
        int i = a->t[t].f[c]; if (i < 0) continue;
        const struct fm_file *f = &a->f[i];
        a->csize[c] += f->size; a->ccount[c]++; a->cq[c] |= f->fq;
        if (f->nodl) a->cdl[c] = 0;
        if (f->est) a->cest[c] = 1;
        if (!a->ckbps[c] || (f->kbps && f->kbps < a->ckbps[c])) a->ckbps[c] = f->kbps;   /* the lowest: honest */
    }
    for (int c = 0; c < FC_N; c++) if (!a->ccount[c]) a->cdl[c] = 0;
}
/* the files of an item (/metadata/<id>/files = {"result": [...]} or the full item) -> tracks */
static int fm_ia_parse_files(const char *js, struct fm_album *a)
{
    const char *arr = fj_get(js, "result"); if (!arr || *fj_w(arr) != '[') arr = fj_get(js, "files");
    if (!arr) return -1;
    fm_album_parts_free(a);
    a->f = calloc(FM_MAXF, sizeof *a->f); a->t = calloc(FM_MAXT, sizeof *a->t);
    if (!a->f || !a->t) return -1;
    char cover[170] = ""; int cover_score = -1;
    for (const char *e = fj_first(arr); e; e = fj_next(e)) {
        char fmt[48], name[170]; fj_gs(e, "format", fmt, sizeof fmt); fj_gs(e, "name", name, sizeof name);
        if (!name[0] || strlen(name) >= sizeof name - 1) continue;
        if (!strcasecmp(fmt, "JPEG") || !strcasecmp(fmt, "PNG") || !strcasecmp(fmt, "Item Image")) {
            char src[16]; fj_gs(e, "source", src, sizeof src);
            if (strcasestr(name, "spectrogram") || strcmp(src, "original")) continue;
            const char *b = fm_base(name);
            int s = (strcasestr(b, "cover") || strcasestr(b, "front") || strcasestr(b, "folder")) ? 3 : !strcasecmp(fmt, "JPEG") ? 2 : 1;
            if (s > cover_score) { cover_score = s; fm_cpy(cover, (int)sizeof cover, name); }
            continue;
        }
        int kb; unsigned q = fm_fmt(fmt, &kb);
        if (!q || a->nf >= FM_MAXF) continue;
        char pv[8]; fj_gs(e, "private", pv, sizeof pv);
        if (!strcmp(pv, "true")) continue;                           /* not downloadable */
        if (strcasestr(name, "_sample.") || strcasestr(name, "/sample")) continue;
        struct fm_file *f = &a->f[a->nf++]; memset(f, 0, sizeof *f);
        fm_cpy(f->name, (int)sizeof f->name, name);
        f->fq = (uint8_t)q; f->kbps = (short)kb;
        if (!kb) { char br[16]; fj_gs(e, "bitrate", br, sizeof br); f->kbps = (short)atoi(br); }
        fj_gs(e, "title", f->title, sizeof f->title); fm_trim(f->title);
        fj_gs(e, "artist", f->artist, sizeof f->artist); if (!f->artist[0]) fj_gs(e, "creator", f->artist, sizeof f->artist); fm_trim(f->artist);
        char src[16]; fj_gs(e, "source", src, sizeof src);
        if (!strcmp(src, "derivative")) fj_gs(e, "original", f->orig, sizeof f->orig);
        char ln[24]; fj_gs(e, "length", ln, sizeof ln); f->len = fm_len(ln);
        char tk[16]; fj_gs(e, "track", tk, sizeof tk); f->track = (short)fm_track_no(tk);
        f->size = fj_gl(e, "size");
    }
    fm_cpy(a->cover, (int)sizeof a->cover, cover);
    /* group: a track = a key (the original name or the file's own name) */
    char (*key)[170] = calloc(FM_MAXT, 170);
    if (!key) return -1;
    for (int i = 0; i < a->nf; i++) {
        struct fm_file *f = &a->f[i];
        /* key: the original's name without extension (alternates uploaded side by side, e.g.
         * x.flac + x.ogg, are the same track) */
        char k[170]; fm_cpy(k, sizeof k, f->orig[0] ? f->orig : f->name);
        { char *dot = strrchr(k, '.'), *sl = strrchr(k, '/'); if (dot && (!sl || dot > sl)) *dot = 0; }
        int t; for (t = 0; t < a->nt; t++) if (!strcmp(key[t], k)) break;
        if (t == a->nt) {
            if (a->nt >= FM_MAXT) continue;
            fm_cpy(key[a->nt], 170, k);
            struct fm_track *T = &a->t[a->nt++]; memset(T, 0, sizeof *T);
            for (int c = 0; c < FC_N; c++) T->f[c] = -1;
        }
        struct fm_track *T = &a->t[t];
        int c = fm_class_of(f->fq); if (c < 0) continue;
        if (T->f[c] < 0 || fm_better(f, &a->f[T->f[c]])) T->f[c] = (short)i;
        if (f->title[0] && (!T->title[0] || !f->orig[0])) fm_cpy(T->title, (int)sizeof T->title, f->title);
        if (f->artist[0] && (!T->artist[0] || !f->orig[0])) fm_cpy(T->artist, (int)sizeof T->artist, f->artist);
        if (f->track && (!T->no || !f->orig[0])) T->no = f->track;
        if (f->len > 0 && (T->len <= 0 || !f->orig[0])) T->len = f->len;
    }
    free(key);
    /* drop tracks with no playable file */
    int w = 0;
    for (int t = 0; t < a->nt; t++) if (a->t[t].f[0] >= 0 || a->t[t].f[1] >= 0 || a->t[t].f[2] >= 0) a->t[w++] = a->t[t];
    a->nt = w;
    /* titles from the file name when the metadata has none */
    for (int t = 0; t < a->nt; t++) if (!a->t[t].title[0]) {
        struct fm_track *T = &a->t[t];
        int i = T->f[FC_LOSSLESS] >= 0 ? T->f[FC_LOSSLESS] : T->f[FC_MP3] >= 0 ? T->f[FC_MP3] : T->f[FC_OGG];
        const char *b = fm_base(a->f[i].orig[0] ? a->f[i].orig : a->f[i].name);
        fm_cpy(T->title, (int)sizeof T->title, b);
        char *dot = strrchr(T->title, '.'); if (dot && dot != T->title) *dot = 0;
        for (char *c = T->title; *c; c++) if (*c == '_') *c = ' ';
    }
    /* order: track numbers when they are all present and different, otherwise natural name order */
    int bynum = a->nt > 0;
    for (int t = 0; t < a->nt && bynum; t++) { if (a->t[t].no <= 0) bynum = 0; for (int u = 0; u < t && bynum; u++) if (a->t[u].no == a->t[t].no) bynum = 0; }
    fm_sort_ctx = a;
    if (bynum) qsort(a->t, a->nt, sizeof *a->t, fm_cmp_no); else qsort(a->t, a->nt, sizeof *a->t, fm_cmp_name);
    for (int t = 0; t < a->nt; t++) a->t[t].no = (short)(t + 1);
    /* lossless quality known from the format name */
    fm_album_totals(a);
    struct fm_item *it = &a->it;
    it->fq = a->cq[0] | a->cq[1] | a->cq[2];
    if (!it->size) for (int c = 0; c < FC_N; c++) it->size += a->csize[c];
    return a->nt;
}
/* first lossless file (to read its STREAMINFO); -1 if none */
static int fm_album_first_lossless(const struct fm_album *a)
{
    for (int t = 0; t < a->nt; t++) { int i = a->t[t].f[FC_LOSSLESS]; if (i >= 0 && (a->f[i].fq & (FQ_FLAC | FQ_FLAC24))) return i; }
    return -1;
}

/* ------------------------------------------------------------------ Jamendo */
/* <0: API error (-code, e.g. -5 = invalid client id); >=0 = number of results */
static int fm_jm_status(const char *js, char *msg, int mcap)
{
    const char *h = fj_get(js, "headers");
    if (msg && mcap) *msg = 0;
    if (!h) return -1000;
    char st[24]; fj_gs(h, "status", st, sizeof st);
    if (msg) fj_gs(h, "error_message", msg, mcap);
    if (strcmp(st, "success")) { int c = (int)fj_gl(h, "code"); return c > 0 ? -c : -999; }
    return (int)fj_gl(h, "results_count");
}
/* tracks?groupby=album_id -> one item per album */
static int fm_jm_parse_search(const char *js, struct fm_item *o, int max, int lossless_only)
{
    int st = fm_jm_status(js, 0, 0); if (st < 0) return st;
    int n = 0;
    for (const char *r = fj_first(fj_get(js, "results")); r && n < max; r = fj_next(r)) {
        struct fm_item *it = &o[n]; memset(it, 0, sizeof *it);
        it->src = FM_SRC_JM;
        fj_gs(r, "album_id", it->id, sizeof it->id);
        if (!it->id[0] || !strcmp(it->id, "0") || strspn(it->id, "0123456789") != strlen(it->id)) continue;
        int dup = 0; for (int k = 0; k < n; k++) if (!strcmp(o[k].id, it->id)) dup = 1;
        if (dup) continue;
        fj_gs(r, "album_name", it->title, sizeof it->title); fm_trim(it->title);
        if (!it->title[0]) fj_gs(r, "name", it->title, sizeof it->title);
        fj_gs(r, "artist_name", it->artist, sizeof it->artist);
        char dt[24]; fj_gs(r, "releasedate", dt, sizeof dt); fm_year(dt, it->year, sizeof it->year);
        fj_gs(r, "album_image", it->img, sizeof it->img); if (!it->img[0]) fj_gs(r, "image", it->img, sizeof it->img);
        fj_gs(r, "license_ccurl", it->lic, sizeof it->lic);
        it->jm_dl = (uint8_t)fj_gb(r, "audiodownload_allowed");
        it->fq = FQ_MP3VBR | (it->jm_dl ? FQ_FLAC : 0);
        if (it->jm_dl) fj_gs(r, "audiodownload", it->probe, sizeof it->probe);
        it->est = 1;
        if (lossless_only && !it->jm_dl) continue;
        n++;
    }
    return n;
}
/* albums/tracks -> album page. FLAC (audiodownload) when the artist allows downloads; the MP3
 * stream (audio, VBR "mp32") is always there for listening. Sizes are estimates. */
static int fm_jm_parse_album(const char *js, struct fm_album *a)
{
    int st = fm_jm_status(js, 0, 0); if (st < 0) return st;
    const char *r = fj_first(fj_get(js, "results")); if (!r) return -1;
    struct fm_item *it = &a->it;
    it->src = FM_SRC_JM; it->est = 1;
    fj_gs(r, "id", it->id, sizeof it->id);
    fj_gs(r, "name", it->title, sizeof it->title); fm_trim(it->title);
    fj_gs(r, "artist_name", it->artist, sizeof it->artist);
    char dt[24]; fj_gs(r, "releasedate", dt, sizeof dt); fm_year(dt, it->year, sizeof it->year);
    fj_gs(r, "image", it->img, sizeof it->img);
    fm_album_parts_free(a);
    a->f = calloc(FM_MAXF, sizeof *a->f); a->t = calloc(FM_MAXT, sizeof *a->t);
    if (!a->f || !a->t) return -1;
    for (const char *t = fj_first(fj_get(r, "tracks")); t && a->nt < FM_MAXT && a->nf + 2 <= FM_MAXF; t = fj_next(t)) {
        struct fm_track *T = &a->t[a->nt]; memset(T, 0, sizeof *T);
        for (int c = 0; c < FC_N; c++) T->f[c] = -1;
        fj_gs(t, "name", T->title, sizeof T->title); fm_trim(T->title);
        fm_cpy(T->artist, (int)sizeof T->artist, it->artist);
        T->len = (float)fj_gl(t, "duration");
        T->no = (short)fj_gl(t, "position");
        if (!it->lic[0]) fj_gs(t, "license_ccurl", it->lic, sizeof it->lic);
        int dl = fj_gb(t, "audiodownload_allowed");
        char u[200];
        fj_gs(t, "audiodownload", u, sizeof u);
        if (dl && !strncmp(u, "http", 4)) {
            struct fm_file *f = &a->f[a->nf]; memset(f, 0, sizeof *f);
            fm_cpy(f->name, (int)sizeof f->name, u); f->fq = FQ_FLAC; f->len = T->len;
            f->size = (long long)(T->len * 110000);                    /* ~880 kbit/s */
            fm_cpy(f->title, (int)sizeof f->title, T->title);
            T->f[FC_LOSSLESS] = (short)a->nf++; it->jm_dl = 1;
            if (!it->probe[0]) fm_cpy(it->probe, (int)sizeof it->probe, u);
        }
        fj_gs(t, "audio", u, sizeof u);
        if (!strncmp(u, "http", 4)) {
            struct fm_file *f = &a->f[a->nf]; memset(f, 0, sizeof *f);
            fm_cpy(f->name, (int)sizeof f->name, u); f->fq = FQ_MP3VBR; f->len = T->len; f->nodl = (uint8_t)!dl;
            f->size = (long long)(T->len * 24000);                     /* ~190 kbit/s */
            fm_cpy(f->title, (int)sizeof f->title, T->title);
            T->f[FC_MP3] = (short)a->nf++;
        }
        if (T->f[FC_LOSSLESS] < 0 && T->f[FC_MP3] < 0) continue;
        if (!T->title[0]) snprintf(T->title, sizeof T->title, "Track %d", a->nt + 1);
        a->nt++;
    }
    int bynum = a->nt > 0;
    for (int t = 0; t < a->nt && bynum; t++) { if (a->t[t].no <= 0) bynum = 0; for (int u = 0; u < t && bynum; u++) if (a->t[u].no == a->t[t].no) bynum = 0; }
    if (bynum) qsort(a->t, a->nt, sizeof *a->t, fm_cmp_no);
    for (int t = 0; t < a->nt; t++) a->t[t].no = (short)(t + 1);
    fm_album_totals(a);
    it->fq = a->cq[0] | a->cq[1];
    it->size = a->csize[0] ? a->csize[0] : a->csize[1];
    return a->nt;
}

/* ------------------------------------------------------------------ FLAC STREAMINFO */
/* b[0..n) = first bytes of the file. 1 = ok; 0 = need more bytes (*need = how many); -1 = not FLAC */
static int fm_flac_info(const uint8_t *b, int n, int *sr, int *bits, int *ch, long long *ns, int *need)
{
    int off = 0;
    if (need) *need = 42;
    if (n >= 10 && !memcmp(b, "ID3", 3)) {            /* ID3v2 in front (some taggers): skip it */
        off = 10 + ((b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127)) + ((b[5] & 0x10) ? 10 : 0);
        if (need) *need = off + 42;
    } else if (n < 10 && n >= 3 && !memcmp(b, "ID3", 3)) return 0;
    if (n < off + 42) return n >= off + 4 && memcmp(b + off, "fLaC", 4) ? -1 : 0;
    if (memcmp(b + off, "fLaC", 4)) return -1;
    const uint8_t *h = b + off + 4;
    int len = h[1] << 16 | h[2] << 8 | h[3];
    if ((h[0] & 127) != 0 || len < 34) return -1;
    const uint8_t *p = h + 4;
    int r = p[10] << 12 | p[11] << 4 | p[12] >> 4;
    int c = ((p[12] >> 1) & 7) + 1, bt = (((p[12] & 1) << 4) | (p[13] >> 4)) + 1;
    long long s = ((long long)(p[13] & 15) << 32) | (uint32_t)(p[14] << 24 | p[15] << 16 | p[16] << 8 | p[17]);
    if (r <= 0 || r > 768000) return -1;
    if (sr) *sr = r;
    if (bits) *bits = bt;
    if (ch) *ch = c;
    if (ns) *ns = s;
    return 1;
}

#include "freemusic_src.inc.c"
