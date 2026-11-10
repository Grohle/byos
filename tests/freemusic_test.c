/* freemusic_test.c - PC tests of src/freemusic_core.inc.c against recorded responses
 * (tests/freemusic_samples). Run: bash tests/freemusic_test.sh */
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
static void free_album(struct fm_album *a) { free(a->f); free(a->t); memset(a, 0, sizeof *a); }

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

int main(int argc, char **argv)
{
    if (argc > 1) dir = argv[1];
    t_search(); t_album_ttb(); t_album_gd16(); t_album_shn(); t_album_mixed(); t_flac(); t_labels(); t_jamendo();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
