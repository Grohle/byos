/* freemusic_test.c - PC tests of src/freemusic_core.inc.c + freemusic_src.inc.c (Vinyl app)
 * against recorded responses and the example sources (tests/freemusic_samples).
 * Run: bash tests/freemusic_test.sh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freemusic_core.inc.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *dir = "tests/freemusic_samples";

static char *load(const char *name, long *len)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { printf("missing %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) exit(2); b[n] = 0; fclose(f);
    if (len) *len = n;
    return b;
}
static void free_album(struct fm_album *a) { fm_album_parts_free(a); memset(a, 0, sizeof *a); }

static void t_search(void)
{
    static struct fm_item it[FM_MAXIT]; long found;
    char *js = load("ia_search_24bit.json", 0);
    int n = fm_ia_parse_search(js, it, FM_MAXIT, &found);
    CHECK(n == 20 && found > 50000, "24bit search: n=%d found=%ld", n, found);
    CHECK(!strcmp(it[0].id, "ttb2017-07-03.akgck2261-24"), "first id %s", it[0].id);
    CHECK(!strcmp(it[0].artist, "Tedeschi Trucks Band"), "artist %s", it[0].artist);
    CHECK(!strcmp(it[0].year, "2017"), "year %s", it[0].year);
    CHECK((it[0].fq & FQ_FLAC24) && (it[0].fq & FQ_MP3VBR) && (it[0].fq & FQ_OGG), "fq %x", it[0].fq);
    CHECK(it[0].etree == 1, "etree flag");
    char q[32]; fm_qlabel(it[0].fq, 0, 0, 0, q, sizeof q); CHECK(!strcmp(q, "FLAC 24-bit"), "label %s", q);
    CHECK(fm_hires(it[0].fq, 0, 0), "24-bit counts as hi-res before probing");
    free(js);

    js = load("ia_search_netlabels.json", 0);
    n = fm_ia_parse_search(js, it, FM_MAXIT, &found);
    CHECK(n >= 15, "netlabels n=%d", n);
    int candy = -1; for (int i = 0; i < n; i++) if (!strcmp(it[i].id, "CANDY032")) candy = i;
    CHECK(candy >= 0 && it[candy].mp3k == 320, "CANDY032 best MP3 320 (got %d)", candy >= 0 ? it[candy].mp3k : -1);
    int ns = -1; for (int i = 0; i < n; i++) if (!strcmp(it[i].id, "NS050")) ns = i;
    CHECK(ns >= 0 && strstr(it[ns].lic, "by-nc-sa/4.0"), "licence of NS050");
    char l[64]; fm_lic_label(it[ns].lic, l, sizeof l); CHECK(!strcmp(l, "CC BY-NC-SA 4.0"), "licence label %s", l);
    free(js);

    js = load("ia_search_jazz.json", 0);
    n = fm_ia_parse_search(js, it, FM_MAXIT, &found);
    CHECK(n >= 15, "jazz n=%d", n);
    /* SHN-only items still have MP3/OGG derivatives */
    CHECK(it[0].fq & FQ_MP3VBR, "gd shn item offers VBR MP3");
    free(js);
    CHECK(fm_ia_parse_search("{\"bad\":1}", it, 5, &found) == -1, "bad json -> -1");
    CHECK(fm_ia_parse_search("", it, 5, &found) == -1, "empty -> -1");
}

static void t_album_ttb(void)
{
    struct fm_album a; memset(&a, 0, sizeof a);
    char *js = load("ia_meta_ttb2017-07-03.akgck2261-24.json", 0);
    CHECK(fm_ia_parse_meta(js, &a) == 0, "meta");
    int n = fm_ia_parse_files(js, &a);
    CHECK(n == 17, "ttb tracks %d", n);
    CHECK(!strcmp(a.t[0].title, "Anyday >"), "first title '%s'", a.t[0].title);
    CHECK(a.t[0].f[FC_LOSSLESS] >= 0 && (a.f[a.t[0].f[FC_LOSSLESS]].fq & FQ_FLAC24), "24-bit flac file");
    CHECK(a.t[0].f[FC_MP3] >= 0 && a.f[a.t[0].f[FC_MP3]].kbps == 189, "vbr bitrate %d", a.t[0].f[FC_MP3] >= 0 ? a.f[a.t[0].f[FC_MP3]].kbps : -1);
    CHECK(a.t[0].len > 800 && a.t[0].len < 810, "len %f", a.t[0].len);
    CHECK(a.ccount[FC_LOSSLESS] == 17 && a.ccount[FC_MP3] == 17 && a.ccount[FC_OGG] == 17, "counts %d %d %d", a.ccount[0], a.ccount[1], a.ccount[2]);
    CHECK(a.csize[FC_LOSSLESS] > a.csize[FC_MP3] * 4, "lossless bigger");
    CHECK(strstr(a.cover, ".jpg") != 0, "cover %s", a.cover);
    CHECK(a.it.etree && !strcmp(a.it.artist, "Tedeschi Trucks Band"), "meta etree/artist");
    CHECK(fm_album_first_lossless(&a) >= 0, "first lossless");
    for (int t = 1; t < a.nt; t++) CHECK(a.t[t].no == t + 1, "renumbered");
    free(js); free_album(&a);
}
static void t_album_gd16(void)
{
    struct fm_album a; memset(&a, 0, sizeof a);
    char *js = load("ia_meta_gd1977-05-08.shure57.stevenson.29303.flac16.json", 0);
    fm_ia_parse_meta(js, &a);
    int n = fm_ia_parse_files(js, &a);
    CHECK(n == 23, "gd16 tracks %d", n);
    CHECK(!strcmp(a.t[0].title, "Turning"), "gd16 first '%s'", a.t[0].title);
    CHECK(a.f[a.t[0].f[FC_LOSSLESS]].fq & FQ_FLAC, "gd16 flac");
    CHECK(!a.cover[0] || strstr(a.cover, ".jpg") || strstr(a.cover, ".png"), "cover ok");
    free(js); free_album(&a);
}
static void t_album_shn(void)
{
    struct fm_album a; memset(&a, 0, sizeof a);
    char *m = load("ia_metaonly_gd_shn.json", 0), *f = load("ia_files_gd_shn.json", 0);
    CHECK(fm_ia_parse_meta(m, &a) == 0, "meta subpath");
    CHECK(a.restricted == 1, "stream_only detected");
    int n = fm_ia_parse_files(f, &a);
    CHECK(n == 20, "shn tracks %d (private shn skipped, mp3/ogg grouped by original)", n);
    CHECK(a.ccount[FC_LOSSLESS] == 0 && a.ccount[FC_MP3] == 20, "no lossless (%d), 20 mp3 (%d)", a.ccount[0], a.ccount[1]);
    CHECK(!strcmp(a.t[0].title, "Minglewood Blues"), "shn first '%s'", a.t[0].title);
    CHECK(a.t[0].len > 380 && a.t[0].len < 382, "6:21 -> %f", a.t[0].len);
    free(m); free(f); free_album(&a);
}
static void t_album_mixed(void)
{
    struct fm_album a; memset(&a, 0, sizeof a);
    char *js = load("ia_meta_CANDY032.json", 0);
    fm_ia_parse_meta(js, &a);
    int n = fm_ia_parse_files(js, &a);
    CHECK(n >= 20, "candy tracks %d", n);
    int k320 = 0; for (int t = 0; t < a.nt; t++) { int i = a.t[t].f[FC_MP3]; if (i >= 0 && a.f[i].kbps == 320) k320++; }
    CHECK(k320 >= 1, "a 320 track");
    CHECK(a.t[0].no == 1 && strstr(a.t[0].title, "Sleigh"), "by track number: '%s'", a.t[0].title);
    free(js); free_album(&a);
    js = load("ia_files_badpanda018.json", 0);
    memset(&a, 0, sizeof a);
    n = fm_ia_parse_files(js, &a);
    CHECK(n >= 9, "badpanda tracks %d", n);
    CHECK(a.ccount[FC_LOSSLESS] == 1, "badpanda: one lossless (flac from wav) -> %d", a.ccount[FC_LOSSLESS]);
    int t1 = -1; for (int t = 0; t < a.nt; t++) if (a.t[t].f[FC_LOSSLESS] >= 0) t1 = t;
    CHECK(t1 >= 0 && (a.f[a.t[t1].f[FC_LOSSLESS]].fq & FQ_FLAC), "flac preferred over wav for that track");
    free(js); free_album(&a);
}
static void t_flac(void)
{
    long n; int sr, bits, ch, need; long long ns;
    char *b = load("flac_head_24.bin", &n);
    CHECK(fm_flac_info((uint8_t *)b, (int)n, &sr, &bits, &ch, &ns, &need) == 1 && sr == 48000 && bits == 24 && ch == 2, "24: %d/%d/%d", sr, bits, ch);
    char q[32]; fm_qlabel(FQ_FLAC24, sr, bits, 0, q, sizeof q); CHECK(!strcmp(q, "FLAC 24/48"), "%s", q);
    CHECK(fm_flac_info((uint8_t *)b, 20, &sr, &bits, &ch, &ns, &need) == 0, "short -> need more");
    free(b);
    b = load("flac_head_16.bin", &n);
    CHECK(fm_flac_info((uint8_t *)b, (int)n, &sr, &bits, &ch, &ns, &need) == 1 && bits == 16, "16: %d/%d", sr, bits);
    fm_qlabel(FQ_FLAC, sr, bits, 0, q, sizeof q); printf("  gd16 header: %s, %d ch, %lld samples\n", q, ch, ns);
    free(b);
    b = load("flac_head_bp.bin", &n);
    CHECK(fm_flac_info((uint8_t *)b, (int)n, &sr, &bits, &ch, &ns, &need) == 1, "badpanda flac");
    fm_qlabel(FQ_FLAC, sr, bits, 0, q, sizeof q); printf("  badpanda header: %s\n", q);
    free(b);
    uint8_t junk[64] = "<html>"; CHECK(fm_flac_info(junk, 64, &sr, &bits, &ch, &ns, &need) == -1, "html -> -1");
    /* ID3v2 in front: 10 + 100 bytes of tag, then the FLAC header */
    static uint8_t id[200]; memset(id, 0, sizeof id); memcpy(id, "ID3\3\0\0\0\0\0\x64", 10);
    long n2; char *b2 = load("flac_head_24.bin", &n2); memcpy(id + 110, b2, 90); free(b2);
    CHECK(fm_flac_info(id, 60, &sr, &bits, &ch, &ns, &need) == 0 && need == 152, "id3: need %d", need);
    CHECK(fm_flac_info(id, 200, &sr, &bits, &ch, &ns, &need) == 1 && bits == 24, "id3 skipped");
}
static void t_labels(void)
{
    char q[32];
    fm_qlabel(FQ_FLAC, 44100, 16, 0, q, sizeof q); CHECK(!strcmp(q, "FLAC 16/44.1"), "%s", q);
    fm_qlabel(FQ_FLAC24, 96000, 24, 0, q, sizeof q); CHECK(!strcmp(q, "FLAC 24/96"), "%s", q);
    fm_qlabel(FQ_FLAC24, 88200, 24, 0, q, sizeof q); CHECK(!strcmp(q, "FLAC 24/88.2"), "%s", q);
    fm_qlabel(FQ_MP3, 0, 0, 320, q, sizeof q); CHECK(!strcmp(q, "MP3 320"), "%s", q);
    fm_qlabel(FQ_MP3VBR, 0, 0, 0, q, sizeof q); CHECK(!strcmp(q, "MP3 VBR"), "%s", q);
    fm_qlabel(FQ_OGG, 0, 0, 0, q, sizeof q); CHECK(!strcmp(q, "OGG"), "%s", q);
    CHECK(!fm_hires(FQ_FLAC, 44100, 16) && fm_hires(FQ_FLAC, 96000, 24) && !fm_hires(FQ_MP3, 0, 0), "hires");
    char l[64];
    fm_lic_label("http://creativecommons.org/publicdomain/zero/1.0/", l, sizeof l); CHECK(!strcmp(l, "CC0 1.0"), "%s", l);
    fm_lic_label("http://creativecommons.org/publicdomain/mark/1.0/", l, sizeof l); CHECK(!strcmp(l, "Public Domain Mark"), "%s", l);
    fm_lic_label("https://creativecommons.org/licenses/by/3.0/de/", l, sizeof l); CHECK(!strcmp(l, "CC BY 3.0"), "%s", l);
    fm_lic_label("", l, sizeof l); CHECK(!l[0], "empty");
    CHECK(fm_len("13:26") == 806 && fm_len("806") == 806 && fm_len("1:00:00") == 3600, "len");
    char s[64];
    fm_safe_name("AC/DC: \"Live\" <1991>?", s, sizeof s); CHECK(!strchr(s, '/') && !strchr(s, ':') && !strchr(s, '"') && !strchr(s, '?'), "safe '%s'", s);
    fm_safe_name("...", s, sizeof s); CHECK(!strcmp(s, "Unknown"), "dots '%s'", s);
    fm_safe_name("Café.", s, sizeof s); CHECK(!strcmp(s, "Café"), "trailing dot '%s'", s);
    fm_safe_name("ÁÁÁÁÁ", s, 6); CHECK(!strcmp(s, "ÁÁ"), "utf8 cut '%s'", s);
    char u[2000];
    fm_ia_search_url(u, sizeof u, "Grateful Dead: (live)", FCOL_LIVE, 1, 0, 1, FSORT_POP, 40, 2);
    CHECK(strstr(u, "grateful+dead") && strstr(u, "collection%3Aetree") && strstr(u, "24bit+Flac") && strstr(u, "subject%3A%28rock") && strstr(u, "language%3A%28spa") && strstr(u, "page=2") && strstr(u, "downloads+desc"), "ia url %s", u);
    CHECK(!strstr(u, "%3A%28live"), "user parentheses removed");
    fm_ia_search_url(u, sizeof u, "", FCOL_ALL, 0, -1, -1, FSORT_NEW, 40, 1);
    CHECK(strstr(u, "publicdate+desc") && !strstr(u, "subject"), "featured url %s", u);
    fm_jm_search_url(u, sizeof u, "abc", "jazz piano", 1, 1, 2, FSORT_POP, 0, 30, 0);
    CHECK(strstr(u, "client_id=abc") && strstr(u, "search=jazz+piano") && strstr(u, "tags=jazz") && strstr(u, "lang=fr") && strstr(u, "audiodlformat=flac") && strstr(u, "groupby=album_id"), "jm url %s", u);
}
static void t_jamendo(void)
{
    static struct fm_item it[16]; char msg[160];
    char *js = load("jm_err_nokey.json", 0);
    CHECK(fm_jm_status(js, msg, sizeof msg) == -5 && strstr(msg, "Client Id"), "no key -> -5 (%s)", msg);
    CHECK(fm_jm_parse_search(js, it, 16, 0) == -5, "search with bad key -> -5");
    free(js);
    js = load("jm_err_badkey.json", 0); CHECK(fm_jm_status(js, 0, 0) == -5, "bad key -5"); free(js);
    js = load("jm_search_doc.json", 0);
    int n = fm_jm_parse_search(js, it, 16, 0);
    CHECK(n == 2, "jm search n=%d (single without album skipped)", n);
    CHECK(!strcmp(it[0].id, "215421") && !strcmp(it[0].title, "Classical Moods") && it[0].jm_dl && (it[0].fq & FQ_FLAC) && strstr(it[0].probe, "/flac/"), "jm item 0");
    CHECK(!strcmp(it[1].artist, "Les Étoiles") && !it[1].jm_dl && !(it[1].fq & FQ_FLAC), "jm item 1 (no download)");
    CHECK(fm_jm_parse_search(js, it, 16, 1) == 1, "lossless only");
    free(js);
    struct fm_album a; memset(&a, 0, sizeof a);
    js = load("jm_album_doc.json", 0);
    n = fm_jm_parse_album(js, &a);
    CHECK(n == 2 && !strcmp(a.t[0].title, "Sunny Day"), "jm album n=%d first '%s'", n, n > 0 ? a.t[0].title : "");
    CHECK(a.ccount[FC_LOSSLESS] == 2 && a.ccount[FC_MP3] == 2 && a.cdl[FC_LOSSLESS] && a.cdl[FC_MP3], "jm classes");
    CHECK(strstr(a.it.lic, "by-nc-nd") != 0, "jm licence");
    free(js); free_album(&a);
}

/* ------------------------------------------------------------------ user sources */
static const char *CAT_BASE = "https://netlabel.example.org/vinyl/catalog.json";
static void t_md5(void)
{
    char h[33];
    fm_md5_hex("", h); CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"), "md5 '' %s", h);
    fm_md5_hex("abc", h); CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"), "md5 abc %s", h);
    fm_md5_hex("The quick brown fox jumps over the lazy dog", h); CHECK(!strcmp(h, "9e107d9d372bb6826bd81d3542a419d6"), "md5 fox %s", h);
    fm_md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890", h);
    CHECK(!strcmp(h, "57edf4a22be3c955ac49da2e2107b67a"), "md5 80 digits %s", h);
    /* Subsonic API documentation: password "sesame", salt "c19b2d" -> t=26719a1196d2a940705a59634eb18eab */
    char a[300]; fm_sub_auth("joe", "sesame", "c19b2d", 0, a, sizeof a);
    CHECK(!strcmp(a, "u=joe&t=26719a1196d2a940705a59634eb18eab&s=c19b2d&v=1.16.1&c=Vinyl&f=json"), "token auth %s", a);
    fm_sub_auth("a b", "sesame", "x", 1, a, sizeof a);
    CHECK(!strcmp(a, "u=a+b&p=enc:736573616d65&v=1.16.1&c=Vinyl&f=json"), "enc auth %s", a);
}
static void t_detect(void)
{
    char u[300];
    CHECK(fm_src_detect("https://archive.org/details/bad-panda", u, sizeof u) == FST_COLL && !strcmp(u, "https://archive.org/details/bad-panda"), "ia details %s", u);
    CHECK(fm_src_detect("  archive.org/details/netlabels?tab=collection ", u, sizeof u) == FST_COLL && !strcmp(u, "https://archive.org/details/netlabels"), "ia no scheme %s", u);
    CHECK(fm_src_detect("http://www.archive.org/details/etree/", u, sizeof u) == FST_COLL && !strcmp(u, "https://archive.org/details/etree"), "ia www %s", u);
    CHECK(fm_src_detect("bad-panda", u, sizeof u) == FST_COLL && !strcmp(u, "https://archive.org/details/bad-panda"), "bare id %s", u);
    CHECK(fm_src_detect("collection:(netlabels)", u, sizeof u) == FST_COLL && strstr(u, "/netlabels"), "collection: %s", u);
    CHECK(fm_src_detect("archive.org/details/bad id", u, sizeof u) == -1, "bad id");
    CHECK(fm_src_detect("Jamendo", u, sizeof u) == FST_JM, "jamendo");
    CHECK(fm_src_detect("https://www.jamendo.com/album/1234", u, sizeof u) == FST_JM, "jamendo url");
    CHECK(fm_src_detect("netlabel.example.org/vinyl/catalog.json", u, sizeof u) == FST_CAT && !strcmp(u, "https://netlabel.example.org/vinyl/catalog.json"), "catalog %s", u);
    CHECK(fm_src_detect("http://10.0.2.2:8766/vinyl.json", u, sizeof u) == FST_CAT, "catalog ip:port");
    CHECK(fm_src_detect("https://podcast.example.org/feed.xml", u, sizeof u) == FST_RSS, "rss xml");
    CHECK(fm_src_detect("https://example.org/feed/", u, sizeof u) == FST_RSS, "rss /feed/");
    CHECK(fm_src_detect("https://example.org/my.opml", u, sizeof u) == FST_OPML, "opml");
    CHECK(fm_src_detect("https://music.example.org/rest/", u, sizeof u) == FST_SUB, "subsonic /rest");
    CHECK(fm_src_detect("https://music.example.org", u, sizeof u) == FST_URL, "unknown url");
    CHECK(fm_src_detect("", u, sizeof u) == -1 && fm_src_detect("http://x", u, sizeof u) == -1 && fm_src_detect("foo bar", u, sizeof u) == -1, "invalid");
    CHECK(fm_src_detect("localhost:8080/c.json", u, sizeof u) == FST_CAT, "localhost");
    CHECK(fm_src_detect("Monotonik", u, sizeof u) == FST_COLL && !strcmp(u, "https://archive.org/details/monotonik"), "auto-capital id %s", u);
    CHECK(fm_src_detect("GratefulDead", u, sizeof u) == FST_COLL && !strcmp(u, "https://archive.org/details/GratefulDead"), "mixed-case id kept %s", u);
    CHECK(fm_src_detect("Https://Example.org/Feed.xml", u, sizeof u) == FST_RSS && !strcmp(u, "https://Example.org/Feed.xml"), "scheme lower %s", u);
    CHECK(fm_src_detect("Netlabel.example.org/vinyl.json", u, sizeof u) == FST_CAT && !strcmp(u, "https://netlabel.example.org/vinyl.json"), "host auto-capital %s", u);
    /* sniffing fetched bodies */
    char *b;
    b = load("vinyl_catalog.json", 0); CHECK(fm_sniff(b) == FST_CAT, "sniff catalog"); free(b);
    b = load("rss_podcast.xml", 0); CHECK(fm_sniff(b) == FST_RSS, "sniff rss"); free(b);
    b = load("atom_feed.xml", 0); CHECK(fm_sniff(b) == FST_RSS, "sniff atom"); free(b);
    b = load("feeds.opml", 0); CHECK(fm_sniff(b) == FST_OPML, "sniff opml"); free(b);
    b = load("sub_err.json", 0); CHECK(fm_sniff(b) == FST_SUB, "sniff subsonic"); free(b);
    CHECK(fm_sniff("<!DOCTYPE html><html><body>x</body></html>") == -2, "sniff html");
    CHECK(fm_sniff("{\"a\":1}") == -1 && fm_sniff("") == -1, "sniff unknown");
    /* URLs */
    char o[300];
    fm_url_join(CAT_BASE, "covers/a.jpg", o, sizeof o); CHECK(!strcmp(o, "https://netlabel.example.org/vinyl/covers/a.jpg"), "join rel %s", o);
    fm_url_join(CAT_BASE, "/x/y.mp3", o, sizeof o); CHECK(!strcmp(o, "https://netlabel.example.org/x/y.mp3"), "join abs %s", o);
    fm_url_join(CAT_BASE, "../music/2.mp3", o, sizeof o); CHECK(!strcmp(o, "https://netlabel.example.org/music/2.mp3"), "join .. %s", o);
    fm_url_join(CAT_BASE, "./a/./b.flac", o, sizeof o); CHECK(!strcmp(o, "https://netlabel.example.org/vinyl/a/b.flac"), "join . %s", o);
    fm_url_join(CAT_BASE, "//cdn.example.org/z.ogg", o, sizeof o); CHECK(!strcmp(o, "https://cdn.example.org/z.ogg"), "join // %s", o);
    fm_url_join("http://10.0.2.2:8766", "a.mp3", o, sizeof o); CHECK(!strcmp(o, "http://10.0.2.2:8766/a.mp3"), "join no path %s", o);
    fm_url_host("https://user@www.example.org:8443/x?y", o, sizeof o); CHECK(!strcmp(o, "example.org:8443"), "host %s", o);
    fm_sub_base("https://music.example.org/rest/", o, sizeof o); CHECK(!strcmp(o, "https://music.example.org"), "sub base %s", o);
    /* archive.org collections */
    char nm[100];
    b = load("ia_coll_meta_badpanda.json", 0); CHECK(fm_ia_coll_info(b, nm, sizeof nm) == 0 && !strcmp(nm, "Bad Panda Records"), "coll info '%s'", nm); free(b);
    b = load("ia_coll_meta_missing.json", 0); CHECK(fm_ia_coll_info(b, nm, sizeof nm) == -2, "coll missing"); free(b);
    b = load("ia_meta_badpanda018.json", 0); CHECK(fm_ia_coll_info(b, nm, sizeof nm) == -3, "an album is not a collection"); free(b);
    long found = 0; static struct fm_item it[8];
    b = load("ia_coll_count_badpanda.json", 0); CHECK(fm_ia_parse_search(b, it, 8, &found) == 0 && found == 106, "coll count %ld", found); free(b);
    char lu[2000]; fm_ia_search_url2(lu, sizeof lu, "panda", "collection:(bad-panda)", 0, -1, -1, FSORT_NEW, 40, 1);
    CHECK(strstr(lu, "collection%3A%28bad-panda%29") && strstr(lu, "panda%29+AND") && strstr(lu, "publicdate+desc"), "coll url %s", lu);
}
static void t_catalog(void)
{
    char *js = load("vinyl_catalog.json", 0);
    char nm[64]; CHECK(fm_cat_info(js, nm, sizeof nm) == 4 && !strcmp(nm, "Example Netlabel"), "cat info %s", nm);
    static struct fm_item it[16]; long found = 0;
    struct fm_filt f; memset(&f, 0, sizeof f); f.genre = f.lang = -1; f.sort = FSORT_POP;
    int n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found);
    CHECK(n == 3 && found == 3, "cat list n=%d found=%ld (AAC-only album skipped)", n, found);
    CHECK(!strcmp(it[0].id, "exn001") && !strcmp(it[0].title, "Northern Lights") && !strcmp(it[0].year, "2024"), "cat item 0");
    CHECK(it[0].src == FM_SRC_CAT && it[0].qstate == 2, "cat src/qstate");
    CHECK((it[0].fq & FQ_FLAC24) && it[0].bits == 24 && it[0].sr == 96000 && it[0].mp3k == 320 && (it[0].fq & FQ_OGG), "cat quality fq=%x %d/%d", it[0].fq, it[0].bits, it[0].sr);
    char q[32]; fm_qlabel(FQ_FLAC24, it[0].sr, it[0].bits, 0, q, sizeof q); CHECK(!strcmp(q, "FLAC 24/96"), "badge %s", q);
    CHECK(it[0].ntr == 2 && it[0].tlen > 777 && it[0].tlen < 778 && it[0].dsize == 98765432LL + 87654321LL, "cat summary ntr=%d tlen=%f dsize=%lld", it[0].ntr, it[0].tlen, it[0].dsize);
    CHECK(!strcmp(it[0].img, "https://netlabel.example.org/vinyl/covers/exn001.jpg"), "cover %s", it[0].img);
    CHECK(strstr(it[0].lic, "licenses/by/4.0") != 0, "album licence");
    CHECK(!strcmp(it[1].year, "2019") && strstr(it[1].lic, "by-sa/4.0") && (it[1].fq & FQ_ANYMP3) && !(it[1].fq & FQ_LOSSLESS), "item 1: default licence, mp3 only");
    CHECK(it[2].sr == 44100 && it[2].bits == 16 && (it[2].fq & FQ_FLAC) && (it[2].fq & FQ_WAV) && it[2].est, "item 2: 16/44.1 (rate 44.1), wav size estimated");
    /* filters */
    f.lossless = 1; n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 2, "lossless %d", n);
    f.lossless = 0; f.genre = 7; n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 1 && !strcmp(it[0].id, "exn001"), "genre ambient %d", n);
    f.genre = -1; f.lang = 1; n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 1 && !strcmp(it[0].id, "exn002"), "lang es %d", n);
    f.lang = -1; snprintf(f.q, sizeof f.q, "aurora"); n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 2, "search artist %d", n);
    snprintf(f.q, sizeof f.q, "solar WIND"); n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 1 && !strcmp(it[0].id, "exn001"), "search track title %d", n);
    snprintf(f.q, sizeof f.q, "Café"); n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 1, "search utf-8 %d", n);
    snprintf(f.q, sizeof f.q, "nothing-matches"); n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found); CHECK(n == 0 && found == 0, "no match");
    f.q[0] = 0; f.sort = FSORT_NEW; n = fm_cat_parse_list(js, CAT_BASE, &f, it, 16, 0, &found);
    CHECK(n == 3 && !strcmp(it[0].id, "exn001") && !strcmp(it[1].id, "exn003") && !strcmp(it[2].id, "exn002"), "sort by year");
    n = fm_cat_parse_list(js, CAT_BASE, &f, it, 1, 1, &found); CHECK(n == 1 && found == 3 && !strcmp(it[0].id, "exn003"), "offset/page");
    /* album page */
    struct fm_album a; memset(&a, 0, sizeof a);
    a.it.sid = 77;
    n = fm_cat_parse_album(js, CAT_BASE, "exn001", &a);
    CHECK(n == 2 && a.it.sid == 77, "album n=%d", n);
    CHECK(a.ccount[FC_LOSSLESS] == 2 && a.ccount[FC_MP3] == 2 && a.ccount[FC_OGG] == 1, "classes %d %d %d", a.ccount[0], a.ccount[1], a.ccount[2]);
    CHECK(a.csize[FC_LOSSLESS] == 98765432LL + 87654321LL && !a.cest[FC_LOSSLESS], "lossless size");
    CHECK(!strcmp(fm_pool(&a, a.f[a.t[0].f[FC_LOSSLESS]].uo), "https://netlabel.example.org/vinyl/audio/exn001/01.flac"), "file url %s", fm_pool(&a, a.f[a.t[0].f[FC_LOSSLESS]].uo));
    CHECK(a.it.sr == 96000 && a.it.bits == 24 && a.t[1].len > 364 && a.t[1].len < 366, "album quality / m:ss duration");
    CHECK(strstr(a.desc, "96 kHz & mastered") && !strchr(a.desc, '<'), "desc html stripped '%s'", a.desc);
    CHECK(!strcmp(a.page, "https://netlabel.example.org/releases/exn001"), "page %s", a.page);
    free_album(&a);
    n = fm_cat_parse_album(js, CAT_BASE, "exn002", &a);
    CHECK(n == 2 && !strcmp(a.t[0].title, "Primero") && !strcmp(a.t[1].title, "Segundo"), "ordered by track number, AAC track dropped (n=%d)", n);
    CHECK(n == 2 && a.f[a.t[0].f[FC_MP3]].kbps == 192 && a.f[a.t[1].f[FC_MP3]].fq == FQ_MP3VBR, "bitrate in bit/s, vbr");
    CHECK(n == 2 && !strcmp(fm_pool(&a, a.f[a.t[1].f[FC_MP3]].uo), "https://netlabel.example.org/music/exn002/2.mp3"), "../ url");
    CHECK(!strcmp(a.page, "https://netlabel.example.org/"), "homepage fallback %s", a.page);
    CHECK(a.cest[FC_MP3], "vbr track without size: estimated");
    free_album(&a);
    CHECK(fm_cat_parse_album(js, CAT_BASE, "nope", &a) == -2, "missing album");
    CHECK(fm_cat_parse_album("{}", CAT_BASE, "x", &a) == -1 && fm_cat_info("[1]", 0, 0) == -1, "not a catalog");
    free_album(&a);
    free(js);
}
static void t_rss(void)
{
    struct fm_album a; memset(&a, 0, sizeof a);
    const char *base = "https://podcast.example.org/feed.xml";
    char *x = load("rss_podcast.xml", 0);
    char nm[100]; CHECK(fm_rss_info(x, nm, sizeof nm) == 4 && !strcmp(nm, "Free Sounds Weekly & Friends"), "rss info %s", nm);
    snprintf(a.it.id, sizeof a.it.id, "feed1"); a.it.sid = 5;
    int n = fm_rss_parse_album(x, base, &a);
    CHECK(n == 2, "podcast episodes %d (video and AAC skipped, commented item ignored)", n);
    CHECK(!strcmp(a.it.title, "Free Sounds Weekly & Friends") && !strcmp(a.it.artist, "Radio Libre") && !strcmp(a.it.id, "feed1") && a.it.sid == 5, "channel '%s' '%s'", a.it.title, a.it.artist);
    CHECK(!strcmp(a.it.img, "https://podcast.example.org/art.jpg"), "itunes:image %s", a.it.img);
    CHECK(!strcmp(a.it.year, "2025") && strstr(a.it.lic, "by-nc/3.0"), "year/licence");
    CHECK(!strcmp(a.desc, "A weekly show of Creative Commons music."), "cdata html desc '%s'", a.desc);
    if (n < 2) { free(x); free_album(&a); return; }
    CHECK(!strcmp(a.t[0].title, "Episode 3: Ambient \xe2\x80\x9cspecial\xe2\x80\x9d"), "entity title '%s'", a.t[0].title);
    CHECK(a.t[0].len == 3723 && a.t[1].len == 2730, "durations %f %f", a.t[0].len, a.t[1].len);
    CHECK(!strcmp(a.t[1].title, "Episode 2: Jazz <live>"), "cdata title '%s'", a.t[1].title);
    const struct fm_file *f0 = &a.f[a.t[0].f[FC_MP3]], *f1 = &a.f[a.t[1].f[FC_MP3]];
    CHECK(!strcmp(fm_pool(&a, f0->uo), "https://cdn.example.org/ep3.mp3?src=rss&id=3") && f0->size == 59600000 && f0->fq == FQ_MP3, "enclosure 0 %s", fm_pool(&a, f0->uo));
    CHECK(!strcmp(fm_pool(&a, f1->uo), "https://podcast.example.org/media/ep2.mp3") && f1->est && f1->size > 0, "relative enclosure, estimated size");
    CHECK(!strcmp(a.page, "https://podcast.example.org/"), "link %s", a.page);
    char q[32]; fm_qlabel(f0->fq, 0, 0, f0->kbps, q, sizeof q); CHECK(!strcmp(q, "MP3"), "unknown bitrate label %s", q);
    struct fm_item it; fm_album_to_item(&a, &it);
    CHECK(it.ntr == 2 && it.tlen == 3723 + 2730 && (it.fq & FQ_MP3) && it.qstate == 2, "feed as a list item");
    free(x); free_album(&a);

    x = load("rss_netlabel.xml", 0);
    n = fm_rss_parse_album(x, "https://label.example.net/rss", &a);
    CHECK(n == 2 && !strcmp(a.it.artist, "Example Netlabel") && !strcmp(a.it.lic, "CC BY-SA 4.0") && !strcmp(a.it.img, "https://label.example.net/logo.png"), "netlabel feed n=%d '%s' '%s'", n, a.it.artist, a.it.lic);
    if (n == 2) {
        CHECK(a.t[0].f[FC_LOSSLESS] >= 0 && a.t[0].f[FC_MP3] >= 0 && a.f[a.t[0].f[FC_MP3]].kbps == 320 && a.t[0].len == 300, "media:content formats");
        CHECK(a.nf == 3, "enclosure equal to a media:content not duplicated (nf=%d)", a.nf);
        CHECK(a.t[1].f[FC_OGG] >= 0 && a.ccount[FC_OGG] == 1 && a.ccount[FC_LOSSLESS] == 1, "ogg item");
    }
    free(x); free_album(&a);

    x = load("atom_feed.xml", 0);
    n = fm_rss_parse_album(x, "https://atom.example.com/feed.atom", &a);
    CHECK(n == 1 && !strcmp(a.it.title, "Atom Sounds") && !strcmp(a.it.artist, "Atom Artist") && !strcmp(a.it.year, "2023"), "atom n=%d '%s' '%s'", n, a.it.title, a.it.artist);
    CHECK(!strcmp(a.page, "https://atom.example.com/") && !strcmp(a.it.img, "https://atom.example.com/logo.jpg"), "atom link/logo %s %s", a.page, a.it.img);
    CHECK(n == 1 && a.t[0].f[FC_LOSSLESS] >= 0 && a.f[a.t[0].f[FC_LOSSLESS]].size == 30000000, "atom enclosure");
    free(x); free_album(&a);
    CHECK(fm_rss_parse_album("{\"no\":1}", "https://x/", &a) == -1, "not a feed");
    free_album(&a);

    x = load("feeds.opml", 0);
    static struct fm_item o[8]; long found = 0;
    CHECK(fm_opml_info(x, nm, sizeof nm) == 2 && !strcmp(nm, "My netlabels"), "opml info %s", nm);
    n = fm_opml_parse(x, "https://me.example.org/lists/feeds.opml", 0, o, 8, 0, &found);
    CHECK(n == 2 && found == 2, "opml feeds %d (duplicate and folder skipped)", n);
    CHECK(!strcmp(o[0].title, "Free Sounds Weekly") && !strcmp(o[0].probe, "https://podcast.example.org/feed.xml") && o[0].src == FM_SRC_RSS, "opml 0");
    CHECK(!strcmp(o[1].title, "Example & Co releases") && !strcmp(o[1].probe, "https://me.example.org/lists/releases.rss"), "opml 1 '%s' %s", o[1].title, o[1].probe);
    struct fm_filt f; memset(&f, 0, sizeof f); f.genre = f.lang = -1; snprintf(f.q, sizeof f.q, "example");
    n = fm_opml_parse(x, "https://me.example.org/", &f, o, 8, 0, &found); CHECK(n == 1 && found == 1, "opml search %d", n);
    free(x);
}
static void t_subsonic(void)
{
    const char *base = "https://music.example.org", *auth = "u=joe&t=abc&s=salt&v=1.16.1&c=Vinyl&f=json";
    static struct fm_item it[8];
    char *js = load("sub_list.json", 0);
    int n = fm_sub_parse_list(js, base, auth, it, 8);
    CHECK(n == 2 && !strcmp(it[0].id, "al-1") && !strcmp(it[0].title, "Hi-Res Dreams") && !strcmp(it[0].year, "2023") && it[0].ntr == 2 && it[0].tlen == 615, "sub list n=%d", n);
    CHECK(!strcmp(it[0].img, "https://music.example.org/rest/getCoverArt.view?u=joe&t=abc&s=salt&v=1.16.1&c=Vinyl&f=json&id=al-1_0&size=200"), "cover %s", it[0].img);
    CHECK(it[0].src == FM_SRC_SUB && !it[0].fq && !it[0].qstate, "quality unknown until probed");
    free(js);
    js = load("sub_err.json", 0); char m[80];
    CHECK(fm_sub_status(js, m, sizeof m) == -40 && strstr(m, "Wrong username"), "error 40 '%s'", m);
    CHECK(fm_sub_parse_list(js, base, auth, it, 8) == -40, "list error");
    free(js);
    CHECK(fm_sub_status("{\"x\":1}", 0, 0) == -1000, "not subsonic");
    struct fm_album a; memset(&a, 0, sizeof a);
    js = load("sub_album.json", 0);
    n = fm_sub_parse_album(js, base, auth, &a);
    CHECK(n == 3 && !strcmp(a.t[0].title, "First") && !strcmp(a.t[1].title, "Second"), "sub album n=%d order", n);
    if (n == 3) {
        CHECK(!strcmp(a.t[0].artist, "Guest") && !strcmp(a.t[1].artist, "Studio Band"), "artists");
        CHECK(a.it.sr == 96000 && a.it.bits == 24 && (a.cq[FC_LOSSLESS] & FQ_FLAC24), "hi-res from bitDepth/samplingRate");
        CHECK(a.ccount[FC_LOSSLESS] == 2 && a.ccount[FC_MP3] == 3 && a.cest[FC_MP3] && !a.cest[FC_LOSSLESS], "classes: originals + mp3 transcode for every track");
        CHECK(!strcmp(fm_pool(&a, a.f[a.t[0].f[FC_LOSSLESS]].uo), "https://music.example.org/rest/download.view?u=joe&t=abc&s=salt&v=1.16.1&c=Vinyl&f=json&id=s-1"), "download url %s", fm_pool(&a, a.f[a.t[0].f[FC_LOSSLESS]].uo));
        CHECK(strstr(fm_pool(&a, a.f[a.t[2].f[FC_MP3]].uo), "/rest/stream.view?") && strstr(fm_pool(&a, a.f[a.t[2].f[FC_MP3]].uo), "format=mp3&maxBitRate=320"), "aac track: transcode only");
        CHECK(a.csize[FC_LOSSLESS] == 220000000LL && !strcmp(a.page, "music.example.org"), "sizes / page");
        CHECK(strstr(a.it.img, "size=400") != 0, "big cover");
    }
    free(js); free_album(&a);
}

int main(int argc, char **argv)
{
    if (argc > 1) dir = argv[1];
    t_search(); t_album_ttb(); t_album_gd16(); t_album_shn(); t_album_mixed(); t_flac(); t_labels(); t_jamendo();
    t_md5(); t_detect(); t_catalog(); t_rss(); t_subsonic();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
