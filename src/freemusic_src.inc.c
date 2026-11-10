/* freemusic_src.inc.c - Vinyl: user sources (included at the end of freemusic_core.inc.c; pure, no
 * UI or I/O, tested on the PC by tests/freemusic_test.c). docs/FREEMUSIC.md, "Sources".
 *   - what the user typed in "Add source" -> kind of source (fm_src_detect) and what a fetched
 *     body is (fm_sniff: Vinyl catalog JSON, RSS/Atom, OPML, Subsonic, web page)
 *   - Vinyl catalog JSON (an open format: docs/FREEMUSIC.md, tests/freemusic_samples/vinyl_catalog.json)
 *   - RSS 2.0 / Atom / Media RSS feeds with audio enclosures (a feed = an album, its episodes or
 *     releases = the tracks) and OPML lists of feeds (each feed = an album)
 *   - Subsonic API (Navidrome, Funkwhale, Airsonic, Gonic...): token auth (MD5), getAlbumList2,
 *     search3, getAlbum, download/stream URLs
 *   - archive.org collections: id validation (the search URL is fm_ia_search_url2)
 *   - text match and filters for the sources that are searched locally (catalog, feeds). */

#include "netxml.inc.c"       /* fm_url_*, fx_* (shared with Books; already included by freemusic_core.inc.c) */

enum { FST_IA, FST_JM, FST_COLL, FST_CAT, FST_RSS, FST_OPML, FST_SUB, FST_URL /* not known yet: sniffed after the first fetch */, FST_N };
/* filters of a list request (snapshot taken by the UI) */
struct fm_filt { int lossless, genre, lang, sort; char q[96]; };

static unsigned fm_hash(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }

/* ------------------------------------------------------------------ MD5 (Subsonic token = md5(password + salt)) */
static const uint32_t FM_MD5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const uint8_t FM_MD5R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
static void fm_md5_blk(uint32_t h[4], const uint8_t *p)
{
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        int r = FM_MD5R[(i >> 4) * 4 + (i & 3)];
        uint32_t x = a + f + FM_MD5K[i] + w[g], t = d;
        d = c; c = b; b = b + (x << r | x >> (32 - r)); a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
static void fm_md5_hex(const char *s, char out[33])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t n = strlen(s), i = 0; uint8_t blk[64];
    for (; i + 64 <= n; i += 64) fm_md5_blk(h, (const uint8_t *)s + i);
    size_t r = n - i; memset(blk, 0, 64); memcpy(blk, s + i, r); blk[r] = 0x80;
    if (r >= 56) { fm_md5_blk(h, blk); memset(blk, 0, 64); }
    uint64_t bits = (uint64_t)n * 8; for (int k = 0; k < 8; k++) blk[56 + k] = (uint8_t)(bits >> (8 * k));
    fm_md5_blk(h, blk);
    static const char hx[] = "0123456789abcdef";
    for (int k = 0; k < 16; k++) { unsigned v = (h[k / 4] >> (8 * (k % 4))) & 255; out[2 * k] = hx[v >> 4]; out[2 * k + 1] = hx[v & 15]; }
    out[32] = 0;
}

/* ------------------------------------------------------------------ URLs */
/* archive.org identifier (of a collection): letters, digits, '_', '-', '.'; 1..79 bytes */
static int fm_ia_id_ok(const char *s)
{
    int n = 0;
    for (; s[n]; n++) if (!isalnum((unsigned char)s[n]) && s[n] != '_' && s[n] != '-' && s[n] != '.') return 0;
    return n > 0 && n < 80 && s[0] != '.' && s[0] != '-';
}
/* /metadata/<collection>/metadata -> its title. 0 = a collection, -2 = no such item, -3 = an
 * item that is not a collection (an album: still searchable, but says so), -1 = bad answer */
static int fm_ia_coll_info(const char *js, char *name, int cap)
{
    if (name && cap) *name = 0;
    if (!js) return -1;
    const char *m = fj_get(js, "result"); if (!m) m = fj_get(js, "metadata");
    if (!m) return fj_get(js, "error") ? -2 : -1;
    if (*fj_w(m) != '{') return -2;                           /* "result": [] or {} for unknown ids */
    char id[80], mt[32]; fj_gs(m, "identifier", id, sizeof id); fj_gs(m, "mediatype", mt, sizeof mt);
    if (!id[0]) return -2;
    if (name) { fj_gs(m, "title", name, cap); fm_trim(name); if (!name[0]) fm_cpy(name, cap, id); }
    return strcmp(mt, "collection") ? -3 : 0;
}
/* what the user typed in "Add source" -> FST_* (or -1 if it cannot be a source) and the
 * normalised URL ("https://archive.org/details/<id>" for a collection, "https://..." otherwise).
 * Accepted: archive.org/details/<collection>, "collection:<id>", a bare collection id
 * ("netlabels"), jamendo(.com) -> the built-in Jamendo, any http(s) URL (with or without the
 * scheme): the kind of a URL is guessed from its extension and confirmed by fm_sniff. */
static int fm_src_detect(const char *in, char *url, int cap)
{
    char s[300]; *url = 0;
    while (*in == ' ' || *in == '\t') in++;
    fm_cpy(s, sizeof s, in);
    int n = (int)strlen(s); while (n > 0 && strchr(" \t\r\n", s[n - 1])) s[--n] = 0;
    if (!s[0]) return -1;
    if (!strncasecmp(s, "collection:", 11)) {
        char id[96]; fm_cpy(id, sizeof id, s + 11); fm_trim(id);
        if (id[0] == '(') { memmove(id, id + 1, strlen(id)); char *e = strchr(id, ')'); if (e) *e = 0; }
        if (!fm_ia_id_ok(id)) return -1;
        snprintf(url, cap, "https://archive.org/details/%s", id); return FST_COLL;
    }
    const char *p = s; int scheme = 0;
    if (!strncasecmp(p, "https://", 8)) { p += 8; scheme = 1; } else if (!strncasecmp(p, "http://", 7)) { p += 7; scheme = 1; }
    const char *h = p; if (!strncasecmp(h, "www.", 4)) h += 4;
    if (!strncasecmp(h, "archive.org/details/", 20)) {
        char id[96]; int k = 0; const char *q = h + 20;
        while (*q && !strchr("/?#&", *q) && k < (int)sizeof id - 1) id[k++] = *q++;
        id[k] = 0;
        if (!fm_ia_id_ok(id)) return -1;
        snprintf(url, cap, "https://archive.org/details/%s", id); return FST_COLL;
    }
    if (!strcasecmp(s, "jamendo") || !strncasecmp(h, "jamendo.com", 11) || !strncasecmp(h, "api.jamendo.com", 15)) { snprintf(url, cap, "https://www.jamendo.com"); return FST_JM; }
    if (!scheme && !strchr(s, '/') && !strchr(s, '.') && !strchr(s, ':')) {
        if (!fm_ia_id_ok(s)) return -1;
        /* "Monotonik": the keyboard capitalises the first letter; collection ids are lower case */
        int upper = 0; for (const char *c = s + 1; *c; c++) if (isupper((unsigned char)*c)) upper = 1;
        if (!upper) s[0] = (char)tolower((unsigned char)s[0]);
        snprintf(url, cap, "https://archive.org/details/%s", s); return FST_COLL;
    }
    if (scheme) { const char *r = strstr(s, "://") + 3; snprintf(url, cap, "%s://%s", tolower((unsigned char)s[4]) == 's' ? "https" : "http", r); }   /* "Https://" (auto-capitals) */
    else {
        /* no scheme: the first letter of the host may be an auto-capital ("Netlabel.org/...") */
        if (isupper((unsigned char)s[0])) { int up = 0; for (const char *c = s + 1; *c && *c != '/'; c++) if (isupper((unsigned char)*c)) up = 1; if (!up) s[0] = (char)tolower((unsigned char)s[0]); }
        snprintf(url, cap, "https://%s", s);
    }
    if (!fm_url_ok(url)) { *url = 0; return -1; }
    char hst[100]; fm_url_host(url, hst, sizeof hst);
    if (!hst[0] || (!strchr(hst, '.') && !strchr(hst, ':') && strcasecmp(hst, "localhost"))) { *url = 0; return -1; }
    /* the path tells the kind most of the time */
    char path[300]; { const char *q = strstr(url, "://") + 3; q = strchr(q, '/'); fm_cpy(path, sizeof path, q ? q : "/"); }
    path[strcspn(path, "?#")] = 0;
    int pl = (int)strlen(path);
    for (int i = 0; i < pl; i++) path[i] = (char)tolower((unsigned char)path[i]);
#define FM_ENDS(x) (pl >= (int)sizeof(x) - 1 && !strcmp(path + pl - (int)sizeof(x) + 1, x))
    if (FM_ENDS(".opml")) return FST_OPML;
    if (FM_ENDS(".rss") || FM_ENDS(".xml") || FM_ENDS(".atom") || FM_ENDS("/feed") || FM_ENDS("/feed/") || FM_ENDS("/rss") || FM_ENDS("/podcast") || strstr(path, "/feed/")) return FST_RSS;
    if (FM_ENDS(".json")) return FST_CAT;
    if (strstr(path, "/rest")) return FST_SUB;
#undef FM_ENDS
    return FST_URL;
}
/* what a fetched body is: FST_CAT, FST_RSS, FST_OPML, FST_SUB, -2 web page, -1 unknown */
static int fm_sniff(const char *b)
{
    if (!b) return -1;
    if ((unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b += 3;
    b = fj_w(b);
    if (*b == '{') {
        if (fj_get(b, "subsonic-response")) return FST_SUB;
        const char *a = fj_get(b, "albums"); if (!a) a = fj_get(b, "items");
        if (fj_get(b, "vinyl") || (a && *fj_w(a) == '[')) return FST_CAT;
        return -1;
    }
    if (*b != '<') return -1;
    char head[2048]; int k = 0;
    for (; b[k] && k < (int)sizeof head - 1; k++) head[k] = (char)tolower((unsigned char)b[k]);
    head[k] = 0;
    if (strstr(head, "<opml")) return FST_OPML;
    if (strstr(head, "<rss") || strstr(head, "<feed") || strstr(head, "<rdf:rdf")) return FST_RSS;
    if (strstr(head, "<subsonic-response")) return FST_SUB;
    if (strstr(head, "<html") || strstr(head, "<!doctype html")) return -2;
    return -1;
}

/* ------------------------------------------------------------------ text match (local search) */
/* every word of q is in hay (ASCII letters without case; other bytes as they are) */
static int fm_match(const char *q, const char *hay)
{
    char w[96];
    while (*q) {
        while (*q == ' ') q++;
        int k = 0; while (*q && *q != ' ' && k < (int)sizeof w - 1) w[k++] = *q++;
        w[k] = 0;
        if (k && !strcasestr(hay, w)) return 0;
    }
    return 1;
}
/* codec of a file: from the declared codec, the MIME type or the URL extension -> FQ_* (0: not a
 * format we offer). bits > 16 = high-resolution FLAC; kbps > 0 = constant bitrate MP3. */
static unsigned fm_codec_name_fq(const char *codec, int bits, int vbr)
{
    char c[40] = ""; int k = 0;
    if (codec) for (; codec[k] && k < (int)sizeof c - 1; k++) c[k] = (char)tolower((unsigned char)codec[k]);
    c[k] = 0;
    if (!strncmp(c, "audio/", 6)) memmove(c, c + 6, strlen(c + 6) + 1);
    if (!strncmp(c, "x-", 2)) memmove(c, c + 2, strlen(c + 2) + 1);
    if (!strcmp(c, "flac")) return bits > 16 ? FQ_FLAC24 : FQ_FLAC;
    if (!strcmp(c, "wav") || !strcmp(c, "wave") || !strcmp(c, "pcm")) return FQ_WAV;
    if (!strcmp(c, "mp3") || !strcmp(c, "mpeg") || !strcmp(c, "mpeg3") || !strcmp(c, "mpga")) return vbr ? FQ_MP3VBR : FQ_MP3;
    if (!strcmp(c, "ogg") || !strcmp(c, "oga") || !strcmp(c, "vorbis")) return FQ_OGG;
    return 0;
}
static unsigned fm_codec_fq(const char *codec, const char *url, int bits, int kbps, int vbr)
{
    (void)kbps;
    unsigned q = fm_codec_name_fq(codec, bits, vbr);
    if (!q && url) {                                          /* the extension of the URL */
        char c[12] = ""; int k = 0;
        const char *e = url + strcspn(url, "?#"), *d = e; while (d > url && d[-1] != '.' && d[-1] != '/') d--;
        if (d > url && d[-1] == '.' && e - d < (int)sizeof c) { while (d < e) c[k++] = *d++; c[k] = 0; q = fm_codec_name_fq(c, bits, vbr); }
    }
    return q;
}
/* genre / language filter on free text ("electronic, ambient" / "en") */
static int fm_filt_ok(const struct fm_filt *f, unsigned fq, const char *genres, const char *lang, const char *hay)
{
    if (!f) return 1;
    if (f->lossless && !(fq & FQ_LOSSLESS)) return 0;
    if (f->genre >= 0 && f->genre < FM_NGENRE) {
        const char *g = FM_GENRES[f->genre].jm;
        if (!strcasestr(genres, g) && !(f->genre == 3 && strcasestr(genres, "electro")) && !(f->genre == 6 && strcasestr(genres, "hip hop"))) return 0;
    }
    if (f->lang >= 0 && f->lang < FM_NLANG && strncasecmp(lang, FM_LANGS[f->lang].jm, 2)) return 0;
    if (f->q[0] && !fm_match(f->q, hay)) return 0;
    return 1;
}
/* album page -> list item (summary: quality, tracks, duration, size of the best format) */
static void fm_album_to_item(const struct fm_album *a, struct fm_item *it)
{
    *it = a->it; it->thumb = 0;
    it->fq = a->cq[0] | a->cq[1] | a->cq[2];
    int c = a->ccount[FC_LOSSLESS] ? FC_LOSSLESS : a->ccount[FC_MP3] ? FC_MP3 : FC_OGG;
    it->ntr = (short)a->nt; it->dsize = a->csize[c]; it->tlen = 0;
    for (int t = 0; t < a->nt; t++) it->tlen += a->t[t].len;
    it->mp3k = (a->cq[FC_MP3] & FQ_MP3) && !(a->cq[FC_MP3] & FQ_MP3VBR) ? a->ckbps[FC_MP3] : 0;
    it->est = a->cest[c];
    it->qstate = 2;
}
/* tracks are filled: best file per class, then totals; drop tracks with nothing playable */
static void fm_album_finish(struct fm_album *a, int keep_order)
{
    int w = 0;
    for (int t = 0; t < a->nt; t++) if (a->t[t].f[0] >= 0 || a->t[t].f[1] >= 0 || a->t[t].f[2] >= 0) a->t[w++] = a->t[t];
    a->nt = w;
    if (!keep_order) {
        int bynum = a->nt > 0;
        for (int t = 0; t < a->nt && bynum; t++) { if (a->t[t].no <= 0) bynum = 0; for (int u = 0; u < t && bynum; u++) if (a->t[u].no == a->t[t].no) bynum = 0; }
        if (bynum) qsort(a->t, a->nt, sizeof *a->t, fm_cmp_no);
    }
    for (int t = 0; t < a->nt; t++) a->t[t].no = (short)(t + 1);
    fm_album_totals(a);
    a->it.fq = a->cq[0] | a->cq[1] | a->cq[2];
    int c = a->ccount[FC_LOSSLESS] ? FC_LOSSLESS : a->ccount[FC_MP3] ? FC_MP3 : FC_OGG;
    a->it.size = a->csize[c];
}
/* adds file f (filled by the caller, URL already in the pool) to track T */
static void fm_track_add_file(struct fm_album *a, struct fm_track *T, int fi)
{
    int c = fm_class_of(a->f[fi].fq); if (c < 0) return;
    if (T->f[c] < 0 || fm_better(&a->f[fi], &a->f[T->f[c]])) T->f[c] = (short)fi;
}
static int fm_album_alloc(struct fm_album *a)
{
    fm_album_parts_free(a);
    a->f = calloc(FM_MAXF, sizeof *a->f); a->t = calloc(FM_MAXT, sizeof *a->t);
    return a->f && a->t ? 0 : -1;
}

/* ------------------------------------------------------------------ Vinyl catalog JSON
 * { "vinyl": 1, "name", "description", "homepage", "license",
 *   "albums": [ { "id", "title", "artist", "year", "license", "cover", "genre", "language",
 *                 "description", "url",
 *                 "tracks": [ { "title", "artist", "duration", "track",
 *                               "files": [ { "url", "codec", "bits", "rate", "channels",
 *                                            "bitrate", "vbr", "size" } ] } ] } ] }
 * Relative URLs are resolved against the catalog's own URL ("base"). */
static const char *fm_cat_albums(const char *js)
{
    const char *a = fj_get(js, "albums"); if (!a) a = fj_get(js, "items");
    return a && *fj_w(a) == '[' ? a : 0;
}
static void fm_cat_album_id(const char *al, int index, char *o, int n)
{
    fj_gs(al, "id", o, n);
    if (!o[0] || strpbrk(o, "/\\\t\n")) {
        char t[300], ar[120]; fj_gs(al, "title", t, sizeof t); fj_gs(al, "artist", ar, sizeof ar);
        size_t l = strlen(t); snprintf(t + l, sizeof t - l, "\x1f%s\x1f%d", ar, t[0] ? 0 : index);
        snprintf(o, n, "%08x", fm_hash(t));
    }
}
/* the genres of an album as one string (string or array) */
static void fm_cat_genres(const char *al, char *o, int n)
{
    *o = 0; int k = 0;
    const char *keys[3] = { "genre", "genres", "tags" };
    for (int i = 0; i < 3; i++) {
        const char *v = fj_get(al, keys[i]); if (!v) continue;
        v = fj_w(v);
        if (*v == '[') { for (const char *e = fj_first(v); e; e = fj_next(e)) { char g[48]; fj_str(e, g, sizeof g); k += snprintf(o + k, n - k > 0 ? n - k : 0, "%s ", g); if (k >= n) { o[n - 1] = 0; return; } } }
        else { char g[120]; fj_str(v, g, sizeof g); k += snprintf(o + k, n - k > 0 ? n - k : 0, "%s ", g); if (k >= n) { o[n - 1] = 0; return; } }
    }
}
/* one file object -> quality fields */
static unsigned fm_cat_file_q(const char *f, int *kbps, int *sr, int *bits, long long *size)
{
    char codec[32], url[400];
    fj_gs(f, "codec", codec, sizeof codec); if (!codec[0]) fj_gs(f, "format", codec, sizeof codec); if (!codec[0]) fj_gs(f, "type", codec, sizeof codec);
    fj_gs(f, "url", url, sizeof url);
    int b = (int)fj_gl(f, "bits"), k = (int)fj_gl(f, "bitrate");
    char rs[24]; fj_gs(f, "rate", rs, sizeof rs); if (!rs[0]) fj_gs(f, "samplerate", rs, sizeof rs);
    double rr = atof(rs); if (rr > 0 && rr < 1000) rr *= 1000;   /* "rate": 44.1 -> 44100 */
    int r = (int)(rr + 0.5);
    if (k > 4000) k /= 1000;                                  /* bits per second -> kbps */
    unsigned q = fm_codec_fq(codec, url, b, k, fj_gb(f, "vbr"));
    if (kbps) *kbps = k;
    if (sr) *sr = r;
    if (bits) *bits = b;
    if (size) *size = fj_gl(f, "size");
    return q;
}
/* summary of an album for the lists (no allocation): quality, tracks, duration, size */
static void fm_cat_summary(const char *al, struct fm_item *it)
{
    long long sz[FC_N] = { 0 }; int cnt[FC_N] = { 0 }, nt = 0; float tl = 0;
    for (const char *t = fj_first(fj_get(al, "tracks")); t; t = fj_next(t)) {
        int has = 0; char d[24]; fj_gs(t, "duration", d, sizeof d); float len = fm_len(d);
        const char *fl = fj_get(t, "files"); int single = !fl || *fj_w(fl) != '[';
        long long bs[FC_N] = { 0 }; int br[FC_N] = { 0 };    /* per class: size of the best file of this track (what the album page offers) */
        for (const char *f = single ? t : fj_first(fl); f; f = single ? 0 : fj_next(f)) {
            int kb, sr, bits; long long s; unsigned q = fm_cat_file_q(f, &kb, &sr, &bits, &s);
            if (!q) continue;
            has = 1; it->fq |= q;
            if ((q & FQ_LOSSLESS) && (bits > it->bits || (bits == it->bits && sr > it->sr))) { it->bits = bits; it->sr = sr; }
            if ((q & FQ_MP3) && kb > it->mp3k) it->mp3k = (short)kb;
            int c = fm_class_of(q);
            if (!s && len > 0) { s = (long long)(len * (c == FC_LOSSLESS ? (bits > 16 ? 330000 : 110000) : kb > 0 ? kb * 125 : 24000)); it->est = 1; }
            int rank = (q & FQ_FLAC24) ? 4000 : (q & FQ_FLAC) ? 3000 : (q & FQ_WAV) ? 2000 : 1000 + kb;
            if (rank > br[c]) { br[c] = rank; bs[c] = s; }
            if (single) break;
        }
        for (int c = 0; c < FC_N; c++) if (br[c]) { sz[c] += bs[c]; cnt[c]++; }
        if (has) { nt++; tl += len; }
    }
    int c = cnt[FC_LOSSLESS] ? FC_LOSSLESS : cnt[FC_MP3] ? FC_MP3 : FC_OGG;
    it->ntr = (short)nt; it->tlen = tl; it->dsize = sz[c]; it->size = sz[0] + sz[1] + sz[2];
}
static void fm_cat_item(const char *js, const char *al, int index, const char *base, struct fm_item *it)
{
    memset(it, 0, sizeof *it);
    it->src = FM_SRC_CAT; it->qstate = 2;
    fm_cat_album_id(al, index, it->id, sizeof it->id);
    fj_gs(al, "title", it->title, sizeof it->title); fm_trim(it->title);
    fj_gs(al, "artist", it->artist, sizeof it->artist); fm_trim(it->artist);
    char dt[40]; fj_gs(al, "year", dt, sizeof dt); if (!dt[0]) fj_gs(al, "date", dt, sizeof dt); fm_year(dt, it->year, sizeof it->year);
    fj_gs(al, "license", it->lic, sizeof it->lic); if (!it->lic[0]) fj_gs(al, "licence", it->lic, sizeof it->lic);
    if (!it->lic[0]) { fj_gs(js, "license", it->lic, sizeof it->lic); if (!it->lic[0]) fj_gs(js, "licence", it->lic, sizeof it->lic); }
    char cv[400]; fj_gs(al, "cover", cv, sizeof cv); if (!cv[0]) fj_gs(al, "image", cv, sizeof cv);
    fm_url_join(base, cv, it->img, sizeof it->img);
    if (!it->title[0]) fm_cpy(it->title, (int)sizeof it->title, it->id);
    fm_cat_summary(al, it);
}
/* name and number of albums of a catalog; -1 if it is not one */
static int fm_cat_info(const char *js, char *name, int cap)
{
    if (name && cap) *name = 0;
    const char *arr = fm_cat_albums(js); if (!arr) return -1;
    if (name) fj_gs(js, "name", name, cap);
    int n = 0; for (const char *a = fj_first(arr); a; a = fj_next(a)) n++;
    return n;
}
/* albums of a catalog that pass the filters, from "offset" on (sorted by year for
 * FSORT_NEW); *found = how many pass. Returns the number written, -1 if not a catalog. */
static int fm_cat_parse_list(const char *js, const char *base, const struct fm_filt *f, struct fm_item *o, int max, int offset, long *found)
{
    if (found) *found = 0;
    const char *arr = fm_cat_albums(js); if (!arr) return -1;
    int total = 0; for (const char *a = fj_first(arr); a; a = fj_next(a)) total++;
    if (!total) return 0;
    const char **pick = malloc(total * sizeof *pick); int *yr = malloc(total * sizeof *yr), *ix = malloc(total * sizeof *ix);
    char *hay = malloc(4096);
    struct fm_item *tmp = malloc(sizeof *tmp);
    int np = 0, i = 0;
    if (!pick || !yr || !ix || !hay || !tmp) { free(pick); free(yr); free(ix); free(hay); free(tmp); return -1; }
    for (const char *a = fj_first(arr); a; a = fj_next(a), i++) {
        fm_cat_item(js, a, i, base, tmp);
        if (!(tmp->fq & (FQ_LOSSLESS | FQ_ANYMP3 | FQ_OGG))) continue;      /* nothing we can play */
        char g[300], lang[16], d[600]; fm_cat_genres(a, g, sizeof g); fj_gs(a, "language", lang, sizeof lang); fj_gs(a, "description", d, sizeof d);
        int k = snprintf(hay, 4096, "%s\n%s\n%s\n%s\n%s", tmp->title, tmp->artist, tmp->year, g, d);
        for (const char *t = fj_first(fj_get(a, "tracks")); t && k < 3900; t = fj_next(t)) { char tt[120]; fj_gs(t, "title", tt, sizeof tt); k += snprintf(hay + k, 4096 - k, "\n%s", tt); }
        if (!fm_filt_ok(f, tmp->fq, g, lang, hay)) continue;
        pick[np] = a; yr[np] = atoi(tmp->year); ix[np] = i; np++;
    }
    if (f && f->sort == FSORT_NEW)                         /* newest first, stable (insertion sort) */
        for (int a = 1; a < np; a++) { const char *pp = pick[a]; int y = yr[a], x = ix[a], b = a; while (b > 0 && yr[b - 1] < y) { pick[b] = pick[b - 1]; yr[b] = yr[b - 1]; ix[b] = ix[b - 1]; b--; } pick[b] = pp; yr[b] = y; ix[b] = x; }
    int n = 0;
    for (int k = offset < 0 ? 0 : offset; k < np && n < max; k++) fm_cat_item(js, pick[k], ix[k], base, &o[n++]);
    if (found) *found = np;
    free(pick); free(yr); free(ix); free(hay); free(tmp);
    return n;
}
/* album "id" of a catalog -> album page; -1 not a catalog, -2 no such album */
static int fm_cat_parse_album(const char *js, const char *base, const char *id, struct fm_album *a)
{
    const char *arr = fm_cat_albums(js); if (!arr) return -1;
    const char *al = 0; int i = 0;
    for (const char *x = fj_first(arr); x; x = fj_next(x), i++) { char aid[80]; fm_cat_album_id(x, i, aid, sizeof aid); if (!strcmp(aid, id)) { al = x; break; } }
    if (!al) return -2;
    uint16_t *th = a->it.thumb; uint32_t sid = a->it.sid;
    fm_cat_item(js, al, i, base, &a->it); a->it.thumb = th; a->it.sid = sid;
    if (fm_album_alloc(a)) return -1;
    fj_gs(al, "description", a->desc, sizeof a->desc); fm_strip_html(a->desc);
    char pg[300]; fj_gs(al, "url", pg, sizeof pg); if (!pg[0]) fj_gs(al, "page", pg, sizeof pg); if (!pg[0]) fj_gs(js, "homepage", pg, sizeof pg);
    char pj[300]; fm_url_join(base, pg, pj, sizeof pj); fm_cpy(a->page, sizeof a->page, pj);
    for (const char *t = fj_first(fj_get(al, "tracks")); t && a->nt < FM_MAXT; t = fj_next(t)) {
        struct fm_track *T = &a->t[a->nt]; memset(T, 0, sizeof *T);
        for (int c = 0; c < FC_N; c++) T->f[c] = -1;
        fj_gs(t, "title", T->title, sizeof T->title); fm_trim(T->title);
        fj_gs(t, "artist", T->artist, sizeof T->artist); fm_trim(T->artist); if (!T->artist[0]) fm_cpy(T->artist, (int)sizeof T->artist, a->it.artist);
        char d[24]; fj_gs(t, "duration", d, sizeof d); T->len = fm_len(d);
        T->no = (short)fj_gl(t, "track"); if (!T->no) T->no = (short)fj_gl(t, "no");
        const char *fl = fj_get(t, "files"); int single = !fl || *fj_w(fl) != '[';
        for (const char *f = single ? t : fj_first(fl); f && a->nf < FM_MAXF; f = single ? 0 : fj_next(f)) {
            int kb, sr, bits; long long s; unsigned q = fm_cat_file_q(f, &kb, &sr, &bits, &s);
            char u[400], ab[600]; fj_gs(f, "url", u, sizeof u);
            if (q && u[0]) {
                fm_url_join(base, u, ab, sizeof ab);
                struct fm_file *F = &a->f[a->nf]; memset(F, 0, sizeof *F);
                F->uo = fm_pool_add(a, ab);
                if (F->uo && fm_url_ok(ab)) {
                    F->fq = (uint8_t)q; F->kbps = (short)kb; F->size = s; F->len = T->len;
                    if (!s && T->len > 0) { F->size = (long long)(T->len * ((q & FQ_LOSSLESS) ? (bits > 16 ? 330000 : 110000) : kb > 0 ? kb * 125 : 24000)); F->est = 1; }
                    fm_cpy(F->name, (int)sizeof F->name, fm_base(ab));
                    fm_cpy(F->title, (int)sizeof F->title, T->title);
                    if ((q & FQ_LOSSLESS) && sr > 0 && (bits > a->it.bits || (bits == a->it.bits && sr > a->it.sr))) { a->it.sr = sr; a->it.bits = bits; }
                    fm_track_add_file(a, T, a->nf++);
                }
            }
            if (single) break;
        }
        if (!T->title[0]) snprintf(T->title, sizeof T->title, "Track %d", a->nt + 1);
        a->nt++;
    }
    fm_album_finish(a, 0);
    return a->nt;
}

/* XML (RSS, Atom, OPML): fx_elem, fx_attr, fx_text, fx_child... live in netxml.inc.c */
/* first year (19xx / 20xx) in a date ("Tue, 03 Jun 2025 10:00:00 GMT", "2025-06-03T...") */
static void fm_any_year(const char *s, char *o, int n)
{
    *o = 0;
    for (const char *p = s; *p; p++) if ((p[0] == '1' && p[1] == '9') || (p[0] == '2' && p[1] == '0')) {
        if (isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) && !isdigit((unsigned char)p[4]) && (p == s || !isdigit((unsigned char)p[-1]))) { snprintf(o, n, "%.4s", p); return; }
    }
}
/* RSS 2.0 / Atom feed -> album: the channel is the album, each item with audio is a track (its
 * <enclosure>s, <media:content>s or Atom enclosure links are the formats). Returns the number of
 * tracks, -1 if it is not a feed. */
static int fm_rss_parse_album(const char *x, const char *base, struct fm_album *a)
{
    const char *e = x + strlen(x), *t, *cs, *ce;
    int atom = 0;
    if (!fx_elem(x, e, "channel", &t, &cs, &ce)) { if (!fx_elem(x, e, "feed", &t, &cs, &ce)) return -1; atom = 1; }
    const char *items = fx_elem(cs, ce, atom ? "entry" : "item", &t, 0, 0) ? t : ce;
    if (!atom) {   /* RSS 1.0 (rdf): the items are siblings of the channel */
        const char *t2; if (items == ce && fx_elem(ce, e, "item", &t2, 0, 0)) { ce = e; items = t2; }
    }
    struct fm_item *it = &a->it;
    uint16_t *th = it->thumb; uint32_t sid = it->sid; char id[80]; fm_cpy(id, sizeof id, it->id);
    memset(it, 0, sizeof *it); it->thumb = th; it->sid = sid; fm_cpy(it->id, sizeof it->id, id);
    it->src = FM_SRC_RSS; it->qstate = 2;
    if (fm_album_alloc(a)) return -1;
    char b[700];
    fx_child(cs, items, "title", it->title, sizeof it->title);
    if (!fx_child(cs, items, "itunes:author", it->artist, sizeof it->artist) || !it->artist[0]) {
        const char *at, *aa, *ab;
        if (fx_elem(cs, items, "author", &at, &aa, &ab)) { if (!fx_child(aa, ab, "name", it->artist, sizeof it->artist)) fx_text(aa, ab, it->artist, sizeof it->artist); }
        if (!it->artist[0]) fx_child(cs, items, "dc:creator", it->artist, sizeof it->artist);
        if (!it->artist[0]) fx_child(cs, items, "managingEditor", it->artist, sizeof it->artist);
    }
    if (!fx_child_attr(cs, items, "itunes:image", "href", b, sizeof b)) {
        const char *it2, *ia, *ib;
        if (fx_elem(cs, items, "image", &it2, &ia, &ib)) fx_child(ia, ib, "url", b, sizeof b);
        if (!b[0]) fx_child_attr(cs, items, "media:thumbnail", "url", b, sizeof b);
        if (!b[0]) fx_child(cs, items, "logo", b, sizeof b);
        if (!b[0]) fx_child(cs, items, "icon", b, sizeof b);
    }
    fm_url_join(base, b, it->img, sizeof it->img);
    if (!fx_child(cs, items, "itunes:summary", a->desc, sizeof a->desc) || !a->desc[0]) { fx_child(cs, items, "description", a->desc, sizeof a->desc); if (!a->desc[0]) fx_child(cs, items, "subtitle", a->desc, sizeof a->desc); }
    b[0] = 0;
    if (atom) { const char *lt, *lp = cs; while ((lp = fx_elem(lp, items, "link", &lt, 0, 0))) { char rel[24]; fx_attr(lt, items, "rel", rel, sizeof rel); if (!rel[0] || !strcmp(rel, "alternate")) { fx_attr(lt, items, "href", b, sizeof b); break; } } }
    else fx_child(cs, items, "link", b, sizeof b);
    fm_url_join(base, b, a->page, sizeof a->page);
    if (!fx_child(cs, items, "creativeCommons:license", it->lic, sizeof it->lic) || !it->lic[0])
        if (!fx_child_attr(cs, items, "cc:license", "rdf:resource", it->lic, sizeof it->lic) || !it->lic[0])
            if (!fx_child(cs, items, "media:license", it->lic, sizeof it->lic) || !it->lic[0]) fx_child(cs, items, "copyright", it->lic, sizeof it->lic);
    /* items */
    const char *p = items, *xt, *xa, *xb; int first = 1;
    while (a->nt < FM_MAXT && (p = fx_elem(p, ce, atom ? "entry" : "item", &xt, &xa, &xb))) {
        struct fm_track *T = &a->t[a->nt]; memset(T, 0, sizeof *T);
        for (int c = 0; c < FC_N; c++) T->f[c] = -1;
        fx_child(xa, xb, "title", T->title, sizeof T->title);
        if (!fx_child(xa, xb, "itunes:author", T->artist, sizeof T->artist) || !T->artist[0]) fm_cpy(T->artist, (int)sizeof T->artist, it->artist);
        char d[40]; if (fx_child(xa, xb, "itunes:duration", d, sizeof d)) T->len = fm_len(d);
        if (first) { char pd[64]; if (!fx_child(xa, xb, "pubDate", pd, sizeof pd) || !pd[0]) if (!fx_child(xa, xb, "published", pd, sizeof pd) || !pd[0]) fx_child(xa, xb, "updated", pd, sizeof pd); fm_any_year(pd, it->year, sizeof it->year); first = 0; }
        /* the formats of this item */
        static const char *const tags[3] = { "media:content", "enclosure", "link" };   /* media:content first: it says the bitrate */
        for (int k = 0; k < 3; k++) {
            const char *q = xa, *ft;
            while (a->nf < FM_MAXF && (q = fx_elem(q, xb, tags[k], &ft, 0, 0))) {
                char u[600], ty[48], ln[24], br[16], du[16], ab[600];
                if (k == 2) { char rel[24]; fx_attr(ft, xb, "rel", rel, sizeof rel); if (strcmp(rel, "enclosure")) continue; fx_attr(ft, xb, "href", u, sizeof u); }
                else fx_attr(ft, xb, "url", u, sizeof u);
                fx_attr(ft, xb, "type", ty, sizeof ty);
                if (!fx_attr(ft, xb, "length", ln, sizeof ln)) fx_attr(ft, xb, "fileSize", ln, sizeof ln);
                fx_attr(ft, xb, "bitrate", br, sizeof br);
                if (fx_attr(ft, xb, "duration", du, sizeof du) && T->len <= 0) T->len = fm_len(du);
                if (!u[0]) continue;
                fm_url_join(base, u, ab, sizeof ab);
                if (!fm_url_ok(ab)) continue;
                int kb = atoi(br); if (kb > 4000) kb /= 1000;
                unsigned fq = fm_codec_fq(ty, ab, 0, kb, 0);
                if (!fq) continue;                                            /* AAC, Opus, video...: not offered */
                int dup = 0; for (int c = 0; c < FC_N; c++) if (T->f[c] >= 0 && !strcmp(fm_pool(a, a->f[T->f[c]].uo), ab)) dup = 1;
                if (dup) continue;
                struct fm_file *F = &a->f[a->nf]; memset(F, 0, sizeof *F);
                F->uo = fm_pool_add(a, ab); if (!F->uo) continue;
                F->fq = (uint8_t)fq; F->kbps = (short)kb; F->size = atoll(ln); F->len = T->len;
                if (F->size <= 0 && T->len > 0) { F->size = (long long)(T->len * ((fq & FQ_LOSSLESS) ? 110000 : kb > 0 ? kb * 125 : 16000)); F->est = 1; }
                fm_cpy(F->name, (int)sizeof F->name, fm_base(ab));
                fm_cpy(F->title, (int)sizeof F->title, T->title);
                fm_track_add_file(a, T, a->nf++);
            }
        }
        for (int c = 0; c < FC_N; c++) if (T->f[c] >= 0 && a->f[T->f[c]].len <= 0) a->f[T->f[c]].len = T->len;
        if (!T->title[0]) snprintf(T->title, sizeof T->title, "Track %d", a->nt + 1);
        a->nt++;
    }
    if (!it->title[0]) fm_cpy(it->title, (int)sizeof it->title, a->page[0] ? a->page : base);
    fm_album_finish(a, 1);                                  /* feed order (newest first in podcasts) */
    return a->nt;
}
/* name and number of entries of a feed / OPML; -1 if it is not one */
static int fm_rss_info(const char *x, char *name, int cap)
{
    const char *e = x + strlen(x), *t, *cs, *ce; int atom = 0;
    if (name && cap) *name = 0;
    if (!fx_elem(x, e, "channel", &t, &cs, &ce)) { if (!fx_elem(x, e, "feed", &t, &cs, &ce)) return -1; atom = 1; }
    const char *items = fx_elem(cs, ce, atom ? "entry" : "item", &t, 0, 0) ? t : ce;
    if (name) fx_child(cs, items, "title", name, cap);
    int n = 0; const char *p = cs; while ((p = fx_elem(p, e, atom ? "entry" : "item", 0, 0, 0))) n++;
    return n;
}
/* OPML: every <outline xmlUrl="..."> is a feed (an album to open); title from title/text */
static int fm_opml_parse(const char *x, const char *base, const struct fm_filt *f, struct fm_item *o, int max, int offset, long *found)
{
    const char *e = x + strlen(x), *t;
    if (found) *found = 0;
    if (!fx_elem(x, e, "opml", 0, 0, 0)) return -1;
    int n = 0, seen = 0; const char *p = x;
    unsigned *hs = calloc(512, sizeof *hs); int nh = 0;     /* feeds already seen (hash of the URL) */
    while (p && (p = fx_elem(p, e, "outline", &t, 0, 0))) {
        const char *gt = fx_gt(t, e); p = gt ? gt + 1 : 0;   /* nested outlines are visited too */
        char u[400], ab[300], ti[140];
        if (!fx_attr(t, e, "xmlUrl", u, sizeof u) || !u[0]) continue;
        fm_url_join(base, u, ab, sizeof ab);
        if (!fm_url_ok(ab)) continue;
        if (!fx_attr(t, e, "title", ti, sizeof ti) || !ti[0]) fx_attr(t, e, "text", ti, sizeof ti);
        const char *hay = ti;
        if (f) { struct fm_filt f2 = *f; f2.lossless = 0; f2.genre = -1; f2.lang = -1; if (!fm_filt_ok(&f2, 0, "", "", hay)) continue; }
        unsigned h = fm_hash(ab); int dup = 0;
        for (int k = 0; k < nh; k++) if (hs[k] == h) dup = 1;
        if (dup) continue;
        if (hs && nh < 512) hs[nh++] = h;
        if (seen++ >= offset && n < max) {
            struct fm_item *it = &o[n++]; memset(it, 0, sizeof *it);
            it->src = FM_SRC_RSS;
            fm_cpy(it->probe, (int)sizeof it->probe, ab);
            snprintf(it->id, sizeof it->id, "%08x", h);
            fm_cpy(it->title, (int)sizeof it->title, ti[0] ? ti : ab);
        }
    }
    free(hs);
    if (found) *found = seen;
    return n;
}
static int fm_opml_info(const char *x, char *name, int cap)
{
    const char *e = x + strlen(x), *t, *cs, *ce;
    if (name && cap) *name = 0;
    if (!fx_elem(x, e, "opml", 0, 0, 0)) return -1;
    if (name && fx_elem(x, e, "head", &t, &cs, &ce)) fx_child(cs, ce, "title", name, cap);
    long found = 0; struct fm_item tmp;
    int r = fm_opml_parse(x, "https://x/", 0, &tmp, 0, 0, &found);
    return r < 0 ? -1 : (int)found;
}

/* ------------------------------------------------------------------ Subsonic API */
/* "https://music.example.org/", ".../rest/" -> "https://music.example.org" */
static void fm_sub_base(const char *in, char *o, int cap)
{
    fm_cpy(o, cap, in);
    o[strcspn(o, "?#")] = 0;
    int n = (int)strlen(o);
    while (n > 0 && o[n - 1] == '/') o[--n] = 0;
    if (n >= 5 && !strcasecmp(o + n - 5, "/rest")) { o[n - 5] = 0; n -= 5; }
    while (n > 0 && o[n - 1] == '/') o[--n] = 0;
}
/* authentication parameters: mode 0 = token (t = md5(password + salt), s = salt), mode 1 =
 * hex-encoded password ("p=enc:...", servers without token auth, e.g. Funkwhale, LDAP users) */
static void fm_sub_auth(const char *user, const char *pw, const char *salt, int mode, char *o, int n)
{
    char ue[100]; fm_urlenc(user, ue, sizeof ue);
    if (mode == 1) {
        char hx[130]; int k = 0; static const char HX[] = "0123456789abcdef";
        for (const unsigned char *p = (const unsigned char *)pw; *p && k < (int)sizeof hx - 3; p++) { hx[k++] = HX[*p >> 4]; hx[k++] = HX[*p & 15]; }
        hx[k] = 0;
        snprintf(o, n, "u=%s&p=enc:%s&v=1.16.1&c=Vinyl&f=json", ue, hx);
        return;
    }
    char cat[160], md[33]; snprintf(cat, sizeof cat, "%s%s", pw, salt); fm_md5_hex(cat, md);
    snprintf(o, n, "u=%s&t=%s&s=%s&v=1.16.1&c=Vinyl&f=json", ue, md, salt);
}
static void fm_sub_url(char *o, int n, const char *base, const char *auth, const char *method, const char *params)
{
    snprintf(o, n, "%s/rest/%s.view?%s%s%s", base, method, auth, params && *params ? "&" : "", params ? params : "");
}
/* 0 = ok; -code of the Subsonic error (40 wrong user/password, 41 token auth not supported,
 * 10 missing parameter, 70 not found); -1000 = not a Subsonic answer */
static int fm_sub_status(const char *js, char *msg, int cap)
{
    if (msg && cap) *msg = 0;
    const char *r = fj_get(js, "subsonic-response"); if (!r) return -1000;
    char st[16]; fj_gs(r, "status", st, sizeof st);
    if (!strcmp(st, "ok")) return 0;
    const char *er = fj_get(r, "error");
    if (msg && er) fj_gs(er, "message", msg, cap);
    int c = er ? (int)fj_gl(er, "code") : 0;
    return c > 0 ? -c : -999;
}
static void fm_sub_cover(const char *base, const char *auth, const char *cover, int size, char *o, int n)
{
    if (!cover || !*cover) { *o = 0; return; }
    char ce[120], p[200]; fm_urlenc(cover, ce, sizeof ce); snprintf(p, sizeof p, "id=%s&size=%d", ce, size);
    fm_sub_url(o, n, base, auth, "getCoverArt", p);
}
static void fm_sub_album_item(const char *al, const char *base, const char *auth, struct fm_item *it)
{
    memset(it, 0, sizeof *it);
    it->src = FM_SRC_SUB;
    fj_gs(al, "id", it->id, sizeof it->id);
    fj_gs(al, "name", it->title, sizeof it->title); if (!it->title[0]) fj_gs(al, "title", it->title, sizeof it->title); fm_trim(it->title);
    fj_gs(al, "artist", it->artist, sizeof it->artist); fm_trim(it->artist);
    char y[16]; fj_gs(al, "year", y, sizeof y); fm_year(y, it->year, sizeof it->year);
    char cv[100]; fj_gs(al, "coverArt", cv, sizeof cv); fm_sub_cover(base, auth, cv, 200, it->img, sizeof it->img);
    it->ntr = (short)fj_gl(al, "songCount"); it->tlen = (float)fj_gl(al, "duration");
    if (!it->title[0]) fm_cpy(it->title, (int)sizeof it->title, it->id);
}
/* getAlbumList2 / getAlbumList / search3 / search2 -> items (quality unknown until the album is
 * read: fq = 0, qstate 0 so the UI probes it with getAlbum) */
static int fm_sub_parse_list(const char *js, const char *base, const char *auth, struct fm_item *o, int max)
{
    int st = fm_sub_status(js, 0, 0); if (st) return st == -1000 ? -1 : st;
    const char *r = fj_get(js, "subsonic-response");
    static const char *const keys[4] = { "albumList2", "albumList", "searchResult3", "searchResult2" };
    const char *box = 0; for (int i = 0; i < 4 && !box; i++) box = fj_get(r, keys[i]);
    if (!box) return 0;
    int n = 0;
    for (const char *a = fj_first(fj_get(box, "album")); a && n < max; a = fj_next(a)) {
        fm_sub_album_item(a, base, auth, &o[n]);
        if (!o[n].id[0] || strpbrk(o[n].id, "/\t\n")) continue;
        n++;
    }
    return n;
}
/* getAlbum -> album page. The original file (download) and, when it is not MP3, a 320 kbps MP3
 * transcode (stream?format=mp3) for listening light: its size is an estimate. */
static int fm_sub_parse_album(const char *js, const char *base, const char *auth, struct fm_album *a)
{
    int st = fm_sub_status(js, 0, 0); if (st) return st == -1000 ? -1 : st;
    const char *al = fj_get(fj_get(js, "subsonic-response"), "album"); if (!al) return -1;
    uint16_t *th = a->it.thumb; uint32_t sid = a->it.sid;
    fm_sub_album_item(al, base, auth, &a->it); a->it.thumb = th; a->it.sid = sid; a->it.qstate = 2;
    char cv[100]; fj_gs(al, "coverArt", cv, sizeof cv); fm_sub_cover(base, auth, cv, 400, a->it.img, sizeof a->it.img);
    if (fm_album_alloc(a)) return -1;
    fm_url_host(base, a->page, sizeof a->page);
    for (const char *s = fj_first(fj_get(al, "song")); s && a->nt < FM_MAXT && a->nf + 2 <= FM_MAXF; s = fj_next(s)) {
        struct fm_track *T = &a->t[a->nt]; memset(T, 0, sizeof *T);
        for (int c = 0; c < FC_N; c++) T->f[c] = -1;
        char id[80], sfx[16], ct[40], u[600], ide[160], p[260];
        fj_gs(s, "id", id, sizeof id); if (!id[0]) continue;
        fj_gs(s, "title", T->title, sizeof T->title); fm_trim(T->title);
        fj_gs(s, "artist", T->artist, sizeof T->artist); if (!T->artist[0]) fm_cpy(T->artist, (int)sizeof T->artist, a->it.artist);
        T->len = (float)fj_gl(s, "duration");
        T->no = (short)(fj_gl(s, "discNumber") * 1000 + fj_gl(s, "track"));
        fj_gs(s, "suffix", sfx, sizeof sfx); fj_gs(s, "contentType", ct, sizeof ct);
        int bits = (int)fj_gl(s, "bitDepth"), sr = (int)fj_gl(s, "samplingRate"), kb = (int)fj_gl(s, "bitRate");
        unsigned q = fm_codec_fq(sfx, 0, bits, kb, 0); if (!q) q = fm_codec_fq(ct, 0, bits, kb, 0);
        fm_urlenc(id, ide, sizeof ide);
        if (q) {
            snprintf(p, sizeof p, "id=%s", ide); fm_sub_url(u, sizeof u, base, auth, "download", p);
            struct fm_file *F = &a->f[a->nf]; memset(F, 0, sizeof *F);
            F->uo = fm_pool_add(a, u);
            if (F->uo) {
                F->fq = (uint8_t)q; F->kbps = (short)((q & FQ_LOSSLESS) ? 0 : kb); F->size = fj_gl(s, "size"); F->len = T->len;
                snprintf(F->name, sizeof F->name, "%s.%s", id, sfx[0] ? sfx : fm_ext(q));
                fm_cpy(F->title, (int)sizeof F->title, T->title);
                if ((q & FQ_LOSSLESS) && sr > 0 && (bits > a->it.bits || (bits == a->it.bits && sr > a->it.sr))) { a->it.sr = sr; a->it.bits = bits; }
                fm_track_add_file(a, T, a->nf++);
            }
        }
        if (!(q & FQ_ANYMP3)) {                               /* transcoded MP3 (lossless, AAC, Opus...) */
            snprintf(p, sizeof p, "id=%s&format=mp3&maxBitRate=320", ide); fm_sub_url(u, sizeof u, base, auth, "stream", p);
            struct fm_file *F = &a->f[a->nf]; memset(F, 0, sizeof *F);
            F->uo = fm_pool_add(a, u);
            if (F->uo) {
                F->fq = FQ_MP3; F->kbps = 320; F->len = T->len; F->size = (long long)(T->len * 40000); F->est = 1;
                snprintf(F->name, sizeof F->name, "%s.mp3", id);
                fm_cpy(F->title, (int)sizeof F->title, T->title);
                fm_track_add_file(a, T, a->nf++);
            }
        }
        if (!T->title[0]) snprintf(T->title, sizeof T->title, "Track %d", a->nt + 1);
        a->nt++;
    }
    fm_album_finish(a, 0);
    return a->nt;
}
