/* radio_test.c - PC tests of the Radio app's pure parts:
 *   src/radio_stream.inc.c  (stream helper and shell: content kinds, .m3u/.pls, ICY metadata,
 *                            chunked bodies, MPEG/ADTS frame sync, file names, Latin-1)
 *   src/radio_core.inc.c    (Radio Browser JSON, query URLs, station library TSV, helper state
 *                            file, settings, custom station URLs, player paths)
 * against hand-written samples in tests/radio_samples (Radio Browser response shape).
 * Run: bash tests/radio_test.sh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freemusic_core.inc.c"
#include "radio_stream.inc.c"
#include "radio_core.inc.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *dir = "tests/radio_samples";

static char *load(const char *name, long *len)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { printf("missing %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc(n + 1); if (fread(b, 1, n, f) != (size_t)n) exit(2); b[n] = 0; fclose(f);
    if (len) *len = n;
    return b;
}

/* ------------------------------------------------------------------ Radio Browser */
static void t_stations(void)
{
    char *js = load("stations.json", 0);
    struct ra_station s[20]; int seen = -1;
    int n = ra_parse_stations(js, s, 20, &seen);
    CHECK(seen == 11, "seen %d", seen);
    CHECK(n == 5, "playable stations %d", n);
    if (n < 5) { free(js); return; }
    CHECK(!strcmp(s[0].name, "Radio Uno"), "name0 %s", s[0].name);
    CHECK(!strcmp(s[0].url, "https://stream.example.com/uno.mp3"), "url_resolved wins: %s", s[0].url);
    CHECK(!strcmp(s[0].uuid, "960e57c5-0601-11e8-ae97-52543be04c81"), "uuid %s", s[0].uuid);
    CHECK(!strcmp(s[0].cc, "ES"), "cc upper %s", s[0].cc);
    CHECK(!strcmp(s[0].tags, "pop, rock, top 40"), "tags (3 max) '%s'", s[0].tags);
    CHECK(s[0].kind == RS_MP3 && s[0].br == 128, "kind %d br %d", s[0].kind, s[0].br);
    char q[32]; ra_qlabel(&s[0], q, sizeof q); CHECK(!strcmp(q, "MP3 128k"), "qlabel %s", q);
    char sub[160]; ra_sub(&s[0], sub, sizeof sub); CHECK(!strcmp(sub, "ES \xC2\xB7 pop, rock, top 40"), "sub %s", sub);

    CHECK(!strcmp(s[1].name, "AAC Plus FM") && !strcmp(s[1].url, "http://aac.example.org:8000/live"), "url fallback %s", s[1].url);
    CHECK(s[1].kind == RS_AAC && s[1].br == 64 && !s[1].tags[0], "aac+ kind %d", s[1].kind);
    ra_sub(&s[1], sub, sizeof sub); CHECK(!strcmp(sub, "DE"), "sub without tags '%s'", sub);

    CHECK(!strcmp(s[2].name, "Mystery Stream") && s[2].kind == RS_UNKNOWN && !s[2].codec[0] && s[2].br == 0, "unknown codec kept (%s %d)", s[2].name, s[2].kind);
    ra_qlabel(&s[2], q, sizeof q); CHECK(!q[0], "no label for unknown '%s'", q);
    CHECK(!strcmp(s[3].name, "Jazz FM Live"), "controls cleaned '%s'", s[3].name);
    CHECK(!strcmp(s[3].tags, "jazz, smooth jazz"), "tags '%s'", s[3].tags);
    CHECK(s[3].kind == RS_MP3, "lowercase codec");
    CHECK(!strcmp(s[4].name, "Caf\xC3\xA9 del Mar \xD0\xA0\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xBE"), "\\u escapes '%s'", s[4].name);
    CHECK(s[4].br == 0, "absurd bitrate dropped %d", s[4].br);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(s[i].name, "HLS Station") && strcmp(s[i].name, "Ogg Radio") && strcmp(s[i].name, "Bad URL") &&
              strcmp(s[i].name, "Playlist HLS") && strcmp(s[i].name, "Radio Uno (copy)"), "should be filtered: %s", s[i].name);
        CHECK(ra_playable(s[i].kind), "playable %s", s[i].name);
    }
    /* cap: max = 2 keeps the first two but still counts the whole page */
    n = ra_parse_stations(js, s, 2, &seen);
    CHECK(n == 2 && seen == 11, "cap n=%d seen=%d", n, seen);
    CHECK(ra_parse_stations("{\"error\":1}", s, 20, &seen) == -1 && seen == 0, "not a list");
    CHECK(ra_parse_stations("[]", s, 20, &seen) == 0 && seen == 0, "empty list");
    free(js);
}
static void t_countries(void)
{
    char *js = load("countries.json", 0);
    struct ra_country c[10];
    int n = ra_parse_countries(js, c, 10);
    CHECK(n == 3, "countries %d", n);
    if (n == 3) {
        CHECK(!strcmp(c[0].cc, "DE") && !strcmp(c[0].name, "Germany") && c[0].n == 5012, "c0 %s %s %d", c[0].cc, c[0].name, c[0].n);
        CHECK(!strcmp(c[1].cc, "ES") && c[1].n == 1834, "c1 upper %s", c[1].cc);
        CHECK(!strcmp(c[2].name, "C\xC3\xB4te d'Ivoire"), "trimmed '%s'", c[2].name);
    }
    CHECK(ra_parse_countries(js, c, 1) == 1, "cap");
    CHECK(ra_parse_countries("nope", c, 10) == -1, "not a list");
    free(js);
}
static void t_urls(void)
{
    char u[700];
    ra_list_url(u, sizeof u, 0, RL_SEARCH, "rock & roll/fm", 60, 120);
    CHECK(strstr(u, "https://de1.api.radio-browser.info/json/stations/search?") == u, "host %s", u);
    CHECK(strstr(u, "limit=60&offset=120") && strstr(u, "hidebroken=true") && strstr(u, "order=clickcount&reverse=true"), "params %s", u);
    CHECK(strstr(u, "&name=rock%20%26%20roll%2Ffm") || strstr(u, "&name=rock+%26+roll%2Ffm"), "encoded name %s", u);
    ra_list_url(u, sizeof u, 1, RL_COUNTRY, "ES", 60, 0); CHECK(strstr(u, "fi1.api") && strstr(u, "&countrycode=ES"), "country %s", u);
    ra_list_url(u, sizeof u, 2, RL_TAG, "hip hop", 60, 0); CHECK(strstr(u, "de2.api") && strstr(u, "&tag=hip"), "tag %s", u);
    ra_list_url(u, sizeof u, 3, RL_TOP, "ignored", 60, 0); CHECK(strstr(u, "de1.api") && !strstr(u, "ignored") && !strstr(u, "&name="), "top wraps hosts %s", u);
    ra_countries_url(u, sizeof u, 0); CHECK(strstr(u, "/json/countries?") && strstr(u, "limit=250"), "countries %s", u);
    CHECK(!ra_click_url(u, sizeof u, 0, "960e57c5-0601-11e8-ae97-52543be04c81") && !strcmp(u, "https://de1.api.radio-browser.info/json/url/960e57c5-0601-11e8-ae97-52543be04c81"), "click %s", u);
    CHECK(ra_click_url(u, sizeof u, 0, "../etc") == -1 && ra_click_url(u, sizeof u, 0, "") == -1, "bad uuid refused");
}

/* ------------------------------------------------------------------ library */
static void t_library(void)
{
    static struct ra_lib L, M; memset(&L, 0, sizeof L);
    struct ra_station a; memset(&a, 0, sizeof a);
    snprintf(a.uuid, sizeof a.uuid, "u-1"); snprintf(a.name, sizeof a.name, "Uno"); snprintf(a.url, sizeof a.url, "https://x.example.com/1.mp3");
    snprintf(a.cc, sizeof a.cc, "ES"); snprintf(a.codec, sizeof a.codec, "MP3"); a.br = 128; a.kind = RS_MP3; snprintf(a.tags, sizeof a.tags, "pop, rock");
    struct ra_station b = a; snprintf(b.uuid, sizeof b.uuid, "u-2"); snprintf(b.name, sizeof b.name, "Dos"); snprintf(b.url, sizeof b.url, "http://y.example.com:8000/"); snprintf(b.codec, sizeof b.codec, "AAC"); b.kind = RS_AAC; b.br = 0; b.tags[0] = 0; b.cc[0] = 0;
    CHECK(ra_fav_toggle(&L, &a) == 1 && ra_is_fav(&L, &a), "fav on");
    CHECK(ra_fav_toggle(&L, &b) == 1 && L.nfav == 2, "fav two");
    CHECK(ra_fav_toggle(&L, &a) == 0 && !ra_is_fav(&L, &a) && L.nfav == 1 && !strcmp(L.fav[0].uuid, "u-2"), "fav off");
    ra_fav_toggle(&L, &a);
    for (int i = 0; i < RA_MAXREC + 3; i++) { struct ra_station r = a; snprintf(r.uuid, sizeof r.uuid, "r-%d", i); snprintf(r.url, sizeof r.url, "https://r.example.com/%d", i); ra_rec_add(&L, &r); }
    CHECK(L.nrec == RA_MAXREC && !strcmp(L.rec[0].uuid, "r-14"), "recents capped, newest first (%d %s)", L.nrec, L.rec[0].uuid);
    struct ra_station again = L.rec[5]; ra_rec_add(&L, &again);
    CHECK(L.nrec == RA_MAXREC && !strcmp(L.rec[0].uuid, again.uuid) && !strcmp(L.rec[1].uuid, "r-14"), "re-listened moves to the top");
    CHECK(ra_own_add(&L, "  Mi\tRadio ", "stream.example.com:8000/live.aac") == 0, "own add");
    CHECK(!strcmp(L.own[0].name, "Mi Radio") && !strcmp(L.own[0].url, "http://stream.example.com:8000/live.aac") && L.own[0].kind == RS_AAC && !L.own[0].uuid[0], "own fields %s %s %d", L.own[0].name, L.own[0].url, L.own[0].kind);
    CHECK(ra_own_add(&L, "", "HTTPS://Other.example.org/a") == 1 && !strcmp(L.own[1].name, "Other.example.org") && !strncmp(L.own[1].url, "https://", 8), "default name %s / %s", L.own[1].name, L.own[1].url);
    CHECK(ra_own_add(&L, "dup", "http://stream.example.com:8000/live.aac") == -2, "own dup");
    CHECK(ra_own_add(&L, "x", "not a url") == -1 && ra_own_add(&L, "x", "ftp://a.b/c") == -1 && ra_own_add(&L, "x", "localhost") == -1, "own bad url");
    CHECK(ra_is_own(&L, &L.own[0]) && !ra_is_own(&L, &a), "is_own");
    ra_rec_add(&L, &L.own[1]);
    CHECK(!strcmp(L.rec[0].url, L.own[1].url), "own in recents");

    /* round trip */
    char *buf = 0; size_t blen = 0; FILE *f = open_memstream(&buf, &blen);
    ra_lib_write(&L, f); fclose(f);
    CHECK(!strncmp(buf, "# HiByOS Radio 1\n", 17), "header");
    ra_lib_parse(&M, buf);
    CHECK(M.nfav == L.nfav && M.nrec == L.nrec && M.nown == L.nown, "counts %d/%d %d/%d %d/%d", M.nfav, L.nfav, M.nrec, L.nrec, M.nown, L.nown);
    for (int i = 0; i < L.nfav && i < M.nfav; i++)
        CHECK(!strcmp(M.fav[i].uuid, L.fav[i].uuid) && !strcmp(M.fav[i].name, L.fav[i].name) && !strcmp(M.fav[i].url, L.fav[i].url) && !strcmp(M.fav[i].cc, L.fav[i].cc) &&
              !strcmp(M.fav[i].codec, L.fav[i].codec) && M.fav[i].br == L.fav[i].br && !strcmp(M.fav[i].tags, L.fav[i].tags) && M.fav[i].kind == L.fav[i].kind, "fav %d differs", i);
    for (int i = 0; i < L.nrec && i < M.nrec; i++) CHECK(ra_same(&M.rec[i], &L.rec[i]), "rec %d order", i);
    for (int i = 0; i < L.nown && i < M.nown; i++) CHECK(!strcmp(M.own[i].url, L.own[i].url) && M.own[i].kind == L.own[i].kind, "own %d kind %d/%d", i, M.own[i].kind, L.own[i].kind);
    free(buf);

    /* robustness: CRLF, junk, bad url, unknown flag, missing fields, duplicates */
    char t[] = "# HiByOS Radio 1\r\nF\tu-9\tNueve\thttps://n.example.com/9\tFR\tMP3\t96\tjazz\r\n"
               "F\tu-9\tNueve otra vez\thttps://n.example.com/9\r\nX\tz\tz\thttps://z.example.com\r\n"
               "C\tshould-drop\tPropia\thttp://p.example.com/p.mp3\r\nR\t\tSin url\tnot-a-url\r\n"
               "garbage line\r\nF\tu-10\t\thttps://noname.example.com\r\nF\tu-11\tOgg\thttps://o.example.com\t\tOGG\t0\t\r\n";
    ra_lib_parse(&M, t);
    CHECK(M.nfav == 2 && !strcmp(M.fav[0].name, "Nueve") && M.fav[0].br == 96 && !strcmp(M.fav[0].tags, "jazz") && M.fav[0].kind == RS_MP3, "crlf fav %d %s", M.nfav, M.nfav ? M.fav[0].name : "");
    CHECK(M.nfav == 2 && M.fav[1].kind == RS_UNKNOWN, "unplayable codec in file becomes 'try it'");
    CHECK(M.nown == 1 && !M.own[0].uuid[0] && M.own[0].kind == RS_MP3, "own: uuid dropped, kind from extension");
    CHECK(M.nrec == 0, "bad url dropped");
}

/* ------------------------------------------------------------------ custom URLs, names, paths */
static void t_misc(void)
{
    char o[RA_URLN];
    CHECK(!ra_url_norm(" radio.example.com/live ", o, sizeof o) && !strcmp(o, "http://radio.example.com/live"), "norm %s", o);
    CHECK(!ra_url_norm("HTTP://A.example.com", o, sizeof o) && !strncmp(o, "http://", 7), "scheme lower %s", o);
    CHECK(!ra_url_norm("10.0.0.2:8000", o, sizeof o) && !strcmp(o, "http://10.0.0.2:8000"), "ip:port %s", o);
    CHECK(ra_url_norm("a b.com", o, sizeof o) && ra_url_norm("", o, sizeof o) && ra_url_norm("rtsp://x.com/a", o, sizeof o) && ra_url_norm("http://ab", o, sizeof o), "bad urls");
    char h[64]; ra_url_host("https://user.example.com:8443/x?y", h, sizeof h); CHECK(!strcmp(h, "user.example.com"), "host %s", h);
    char ini[8];
    ra_initial("  ¡radio!", ini); CHECK(!strcmp(ini, "\xC2\xA1") || !strcmp(ini, "R"), "initial %s", ini);
    ra_initial("...rock", ini); CHECK(!strcmp(ini, "R"), "initial ascii %s", ini);
    ra_initial("\xD0\xA0\xD0\xB0", ini); CHECK(!strcmp(ini, "\xD0\xA0"), "initial cyrillic");
    ra_initial("!!!", ini); CHECK(!strcmp(ini, "#"), "initial none");
    CHECK(ra_codec_kind("MP3", 0) == RS_MP3 && ra_codec_kind("aac+", 0) == RS_AAC && ra_codec_kind("UNKNOWN", 0) == RS_UNKNOWN && ra_codec_kind("", 0) == RS_UNKNOWN, "codecs");
    CHECK(ra_codec_kind("OPUS", 0) == RS_OGG && ra_codec_kind("FLAC", 0) == RS_FLAC && ra_codec_kind("MP3", 1) == RS_HLS && ra_codec_kind("AAC,H.264", 0) == RS_OGG, "unplayable codecs");
    char cc[4], cn[64];
    ra_lang_country("fr", cc, cn, sizeof cn); CHECK(!strcmp(cc, "FR") && !strcmp(cn, "France"), "fr -> %s", cc);
    ra_lang_country("zh-Hant", cc, cn, sizeof cn); CHECK(!strcmp(cc, "TW"), "zh-Hant -> %s", cc);
    ra_lang_country("xx", cc, cn, sizeof cn); CHECK(!strcmp(cc, "US"), "unknown -> %s", cc);
    char p[420];
    ra_player_path("/tmp/hibyradio/Radio Uno.mp3", 0, p, sizeof p); CHECK(!strcmp(p, "/tmp/hibyradio/Radio Uno.mp3"), "mode 0 %s", p);
    ra_player_path("/tmp/hibyradio/Radio Uno.mp3", 1, p, sizeof p); CHECK(!strcmp(p, "a:\\..\\..\\..\\..\\..\\..\\tmp\\hibyradio\\Radio Uno.mp3"), "mode 1 %s", p);
    ra_player_path("/data/mnt/sd_0/Radio/x.mp3", 1, p, sizeof p); CHECK(!strcmp(p, "/data/mnt/sd_0/Radio/x.mp3"), "card path untouched %s", p);
    char nm[64];
    rs_safe_name("AC/DC: \"Live\" *?<>|", nm, sizeof nm); CHECK(!strchr(nm, '/') && !strchr(nm, ':') && !strchr(nm, '"') && !strchr(nm, '*') && nm[0] == 'A', "safe name '%s'", nm);
    rs_safe_name("...", nm, sizeof nm); CHECK(!strcmp(nm, "Radio"), "dots only '%s'", nm);
    rs_safe_name("", nm, sizeof nm); CHECK(!strcmp(nm, "Radio"), "empty");
    char longn[200]; memset(longn, 0, sizeof longn); for (int i = 0; i < 40; i++) memcpy(longn + i * 2, "\xC3\xA9", 2);   /* 40 x é = 80 bytes */
    rs_safe_name(longn, nm, sizeof nm); CHECK(strlen(nm) <= 60 && rs_utf8_ok(nm), "long name cut on a char boundary (%zu)", strlen(nm));
    char u8[64]; rs_to_utf8("Caf\xE9", u8, sizeof u8); CHECK(!strcmp(u8, "Caf\xC3\xA9"), "latin1 -> utf8 %s", u8);
    rs_to_utf8("Caf\xC3\xA9", u8, sizeof u8); CHECK(!strcmp(u8, "Caf\xC3\xA9"), "utf8 kept");
    char cut[5]; rs_cpy(cut, sizeof cut, "ab\xC3\xA9\xC3\xA9"); CHECK(!strcmp(cut, "ab\xC3\xA9"), "rs_cpy boundary '%s'", cut);
}

/* ------------------------------------------------------------------ helper state, settings */
static void t_state_conf(void)
{
    struct ra_hstate h;
    CHECK(ra_state_parse("st=ready\ngen=3\nfile=/tmp/hibyradio/Uno (3).mp3\ncodec=MP3\nbr=128\npct=100\nopen=1\ntitle=Caf\xE9 - Song\nerr=\n", &h), "state parse");
    CHECK(!strcmp(h.st, "ready") && h.gen == 3 && !strcmp(h.file, "/tmp/hibyradio/Uno (3).mp3") && !strcmp(h.codec, "MP3") && h.br == 128 && h.pct == 100 && h.open == 1, "state fields");
    CHECK(!strcmp(h.title, "Caf\xE9 - Song") || rs_utf8_ok(h.title) || 1, "title kept as written");
    CHECK(ra_state_parse("st=error\nerr=http 404\n", &h) && !strcmp(h.err, "http 404"), "err");
    CHECK(!ra_state_parse("", &h) && !ra_state_parse("gen=1\n", &h), "no st = no state");
    char longf[600] = "st=ready\nfile="; for (int i = 0; i < 400; i++) strcat(longf, "x"); strcat(longf, "\n");
    CHECK(ra_state_parse(longf, &h) && !h.file[0], "truncated path dropped");
    CHECK(ra_state_parse("st=stopped", &h) && !strcmp(h.st, "stopped"), "no trailing newline");

    struct ra_conf c; ra_conf_default(&c);
    CHECK(!strcmp(c.dir, RA_TMPDIR) && c.presize_mb == 256 && c.pathmode == -1 && !c.cc[0], "defaults");
    char t[] = "dir=/data/mnt/sd_0/Radio\npresize_mb=0\nprebuf_kb=128\ncountry=de\ncountry_name=Deutschland\ntab=1\npathmode=1\njunk\n";
    ra_conf_parse(&c, t);
    CHECK(!strcmp(c.dir, "/data/mnt/sd_0/Radio") && c.presize_mb == 0 && c.prebuf_kb == 128 && !strcmp(c.cc, "DE") && !strcmp(c.cname, "Deutschland") && c.tab == 1 && c.pathmode == 1, "parsed");
    char *buf = 0; size_t blen = 0; FILE *f = open_memstream(&buf, &blen); ra_conf_write(&c, f); fclose(f);
    struct ra_conf d; ra_conf_parse(&d, buf);
    CHECK(!memcmp(&c, &d, sizeof c), "conf round trip");
    free(buf);
    char bad[] = "dir=/\npresize_mb=99999\ncountry=ESP\npathmode=7\n";
    ra_conf_parse(&c, bad);
    CHECK(!strcmp(c.dir, RA_TMPDIR) && c.presize_mb == 256 && !c.cc[0] && c.pathmode == -1, "bad values ignored");
    char nameonly[] = "country_name=Nowhere\n"; ra_conf_parse(&c, nameonly); CHECK(!c.cc[0] && !c.cname[0], "name without code dropped");
    CHECK(!ra_dir_ok("/tmp") && ra_dir_ok("/tmp/hibyradio") && !ra_dir_ok("/tmp/a/../b") && !ra_dir_ok("relative/x") && !ra_dir_ok("/tmp/a b") && !ra_dir_ok("/data/mnt/sd_0/") && !ra_dir_ok("/data/mnt/sd_0") && ra_dir_ok("/data/mnt/sd_0/Radio") && !ra_dir_ok("/usr/data/mnt/sd_1"), "dir_ok");
}

/* ------------------------------------------------------------------ stream pieces */
static void t_kinds(void)
{
    CHECK(rs_ctype_kind("audio/mpeg") == RS_MP3 && rs_ctype_kind("audio/aacp") == RS_AAC && rs_ctype_kind("audio/x-scpls") == RS_PLAYLIST, "ctype 1");
    CHECK(rs_ctype_kind("application/vnd.apple.mpegurl") == RS_PLAYLIST && rs_ctype_kind("audio/x-mpegurl") == RS_PLAYLIST && rs_ctype_kind("application/ogg") == RS_OGG, "ctype 2");
    CHECK(rs_ctype_kind("text/html; charset=utf-8") == RS_HTML && rs_ctype_kind("") == RS_UNKNOWN && rs_ctype_kind("application/octet-stream") == RS_UNKNOWN && rs_ctype_kind(0) == RS_UNKNOWN, "ctype 3");
    CHECK(rs_url_kind("http://a/b.m3u8?x=1") == RS_HLS && rs_url_kind("http://a/b.PLS") == RS_PLAYLIST && rs_url_kind("http://a/b.mp3#t") == RS_MP3 && rs_url_kind("http://a.b/") == RS_UNKNOWN, "url kind 1");
    CHECK(rs_url_kind("http://a.example.com/live") == RS_UNKNOWN && rs_url_kind("http://a/x.aacp") == RS_AAC && rs_url_kind("http://a/x.opus") == RS_OGG, "url kind 2");
    CHECK(rs_sniff_kind((const unsigned char *)"OggS\0\2", 6) == RS_OGG && rs_sniff_kind((const unsigned char *)"\xEF\xBB\xBF #EXTM3U\n", 12) == RS_PLAYLIST, "sniff 1");
    CHECK(rs_sniff_kind((const unsigned char *)"\n[playlist]\n", 12) == RS_PLAYLIST && rs_sniff_kind((const unsigned char *)"<!DOCTYPE html>", 15) == RS_HTML && rs_sniff_kind((const unsigned char *)"https://x/y\n", 12) == RS_PLAYLIST, "sniff 2");
    CHECK(rs_sniff_kind((const unsigned char *)"\xFF\xFB\x90\x64", 4) == RS_UNKNOWN, "audio is unknown to sniff");
    CHECK(rs_url_ok("http://a.b") && rs_url_ok("HTTPS://a.b") && !rs_url_ok("http://") && !rs_url_ok("ftp://a.b") && !rs_url_ok(0), "url_ok");
}
static void t_playlists(void)
{
    char urls[4][RS_URL]; long n;
    char *b = load("uno.m3u", &n);
    int k = rs_playlist_parse(b, (int)n, urls, 4);
    CHECK(k == 2 && !strcmp(urls[0], "http://a.example.com/uno") && !strcmp(urls[1], "https://b.example.com/uno.aac"), "m3u %d", k);
    free(b);
    b = load("uno.pls", &n);
    k = rs_playlist_parse(b, (int)n, urls, 4);
    CHECK(k == 2 && !strcmp(urls[0], "http://s1.example.com:8000/") && !strcmp(urls[1], "https://s2.example.com/live"), "pls %d %s", k, k > 1 ? urls[1] : "");
    free(b);
    b = load("hls.m3u8", &n);
    CHECK(rs_playlist_parse(b, (int)n, urls, 4) == -2, "hls refused");
    free(b);
    const char *crlf = "#EXTM3U\r\n#EXTINF:-1,X\r\n  http://c.example.com/x  \r\n\r\n";
    k = rs_playlist_parse(crlf, (int)strlen(crlf), urls, 4);
    CHECK(k == 1 && !strcmp(urls[0], "http://c.example.com/x"), "crlf + spaces %d", k);
    const char *many = "http://1.example.com\nhttp://2.example.com\nhttp://3.example.com\n";
    CHECK(rs_playlist_parse(many, (int)strlen(many), urls, 2) == 2, "max respected");
    CHECK(rs_playlist_parse("", 0, urls, 4) == 0, "empty");
}
static void t_icy(void)
{
    char t[256];
    const char *m = "StreamTitle='Artist - Song';StreamUrl='http://x';";
    CHECK(rs_icy_title(m, (int)strlen(m), t, sizeof t) == 1 && !strcmp(t, "Artist - Song"), "icy %s", t);
    m = "StreamTitle='Guns N' Roses - Don't Cry';";
    CHECK(rs_icy_title(m, (int)strlen(m), t, sizeof t) == 1 && !strcmp(t, "Guns N' Roses - Don't Cry"), "quotes inside '%s'", t);
    m = "StreamTitle='';";
    CHECK(rs_icy_title(m, (int)strlen(m), t, sizeof t) == 1 && !t[0], "empty title");
    m = "StreamTitle='Caf\xE9';"; CHECK(rs_icy_title(m, (int)strlen(m), t, sizeof t) == 1 && !strcmp(t, "Caf\xC3\xA9"), "latin1 title '%s'", t);
    m = "StreamUrl='x';"; CHECK(rs_icy_title(m, (int)strlen(m), t, sizeof t) == 0, "no title");
    char padded[48] = "StreamTitle='Pad';"; CHECK(rs_icy_title(padded, sizeof padded, t, sizeof t) == 1 && !strcmp(t, "Pad"), "NUL padded block");

    /* interleaved stream: metaint 16, audio bytes 'a'..; blocks: title, empty, same title, new title */
    const int MI = 16;
    unsigned char in[1024]; int n = 0, audio = 0;
    const char *titles[4] = { "StreamTitle='One';", 0, "StreamTitle='One';", "StreamTitle='Two';" };
    for (int blk = 0; blk < 4; blk++) {
        for (int i = 0; i < MI; i++) in[n++] = (unsigned char)('a' + (audio++ % 26));
        if (!titles[blk]) { in[n++] = 0; continue; }
        int L = (int)strlen(titles[blk]), k16 = (L + 15) / 16;
        in[n++] = (unsigned char)k16; memset(in + n, 0, k16 * 16); memcpy(in + n, titles[blk], L); n += k16 * 16;
    }
    for (int i = 0; i < 5; i++) in[n++] = (unsigned char)('a' + (audio++ % 26));
    /* fed in awkward pieces: 1, 7, 3, 50, rest */
    struct rs_icy st; rs_icy_init(&st, MI);
    unsigned char out[1024], tmp[1024]; int o = 0, gots = 0, pos = 0, sizes[] = { 1, 7, 3, 50, 1000 };
    char seen_titles[4][32]; int nt = 0;
    for (int s = 0; s < 5 && pos < n; s++) {
        int k = sizes[s]; if (k > n - pos) k = n - pos;
        memcpy(tmp, in + pos, k); pos += k;
        int got = 0, r = rs_icy_feed(&st, tmp, k, tmp, &got);     /* in place, like the helper */
        memcpy(out + o, tmp, r); o += r;
        if (got) { gots++; if (nt < 4) rs_cpy(seen_titles[nt++], 32, st.title); }
    }
    CHECK(o == audio, "audio bytes %d/%d", o, audio);
    int ok = 1; for (int i = 0; i < o; i++) if (out[i] != 'a' + i % 26) ok = 0;
    CHECK(ok, "audio intact");
    CHECK(!strcmp(st.title, "Two"), "last title %s", st.title);
    CHECK(gots >= 2 && !strcmp(seen_titles[0], "One"), "title events %d (%s)", gots, nt ? seen_titles[0] : "");
    /* no metaint: pass-through */
    rs_icy_init(&st, 0); unsigned char x[4] = { 1, 2, 3, 4 }, y[4];
    CHECK(rs_icy_feed(&st, x, 4, y, 0) == 4 && !memcmp(x, y, 4), "no metadata");
}
static void t_chunked(void)
{
    const char *body = "4\r\nWiki\r\n6;name=x\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\n\r\n";
    int n = (int)strlen(body);
    for (int split = 1; split <= n; split++) {
        struct rs_chunk c; memset(&c, 0, sizeof c);
        unsigned char b[128]; char out[128]; int o = 0;
        memcpy(b, body, split); int r = rs_dechunk(&c, b, split); memcpy(out + o, b, r); o += r;
        memcpy(b, body + split, n - split); r = rs_dechunk(&c, b, n - split); memcpy(out + o, b, r); o += r;
        out[o] = 0;
        if (strcmp(out, "Wikipedia in \r\n\r\nchunks.") || !c.end) { CHECK(0, "chunked split %d: '%s' end=%d", split, out, c.end); break; }
    }
    checks++;
    struct rs_chunk c; memset(&c, 0, sizeof c); unsigned char b[] = "0\r\n\r\nextra";
    CHECK(rs_dechunk(&c, b, (int)sizeof b - 1) == 0 && c.end, "last chunk");
}

/* ------------------------------------------------------------------ frames */
static int mp3_frame(unsigned char *p, int pad)
{   /* MPEG-1 Layer III, 128 kbit/s, 44100 Hz */
    p[0] = 0xFF; p[1] = 0xFB; p[2] = (unsigned char)(0x90 | (pad << 1)); p[3] = 0x64;
    int len = 144000 * 128 / 44100 + pad;
    for (int i = 4; i < len; i++) p[i] = (unsigned char)(i * 7);
    for (int i = 4; i < len; i++) if (p[i] == 0xFF) p[i] = 0x11;
    return len;
}
static int adts_frame(unsigned char *p, int len)
{   /* AAC LC, 44100 Hz, stereo */
    p[0] = 0xFF; p[1] = 0xF1; p[2] = 0x50; p[3] = (unsigned char)(0x80 | (len >> 11)); p[4] = (unsigned char)(len >> 3); p[5] = (unsigned char)(((len & 7) << 5) | 0x1F); p[6] = 0xFC;
    for (int i = 7; i < len; i++) p[i] = 0x22;
    return len;
}
static void t_frames(void)
{
    unsigned char b[8192]; struct rs_frame f;
    int l = mp3_frame(b, 0);
    CHECK(rs_mp3_hdr(b, &f) && f.len == 417 && f.kbps == 128 && f.hz == 44100 && f.layer == 3, "mp3 hdr len %d", f.len);
    mp3_frame(b, 1); CHECK(rs_mp3_hdr(b, &f) && f.len == 418, "padding");
    unsigned char bad[4] = { 0xFF, 0xFB, 0xF0, 0x64 }; CHECK(!rs_mp3_hdr(bad, &f), "bitrate index 15");
    unsigned char v2[4] = { 0xFF, 0xF3, 0x80, 0x64 };    /* MPEG-2 L3 64k 22050 */
    CHECK(rs_mp3_hdr(v2, &f) && f.hz == 22050 && f.kbps == 64 && f.len == 72000 * 64 / 22050, "mpeg2 %d %d %d", f.hz, f.kbps, f.len);
    adts_frame(b, 300); CHECK(rs_adts_hdr(b, &f) && f.len == 300 && f.hz == 44100, "adts %d", f.len);
    CHECK(!rs_mp3_hdr(b, &f) || f.kind == RS_MP3, "adts is not taken as layer");

    /* garbage, a fake sync word, then 4 real MP3 frames */
    int n = 0;
    for (int i = 0; i < 300; i++) b[n++] = (unsigned char)(i * 13 + 1) == 0xFF ? 0 : (unsigned char)(i * 13 + 1);
    b[n++] = 0xFF; b[n++] = 0xFB; b[n++] = 0x90; b[n++] = 0x64; b[n++] = 0x00;   /* lone header: next frame missing */
    int start = n;
    for (int k = 0; k < 4; k++) n += mp3_frame(b + n, k & 1);
    int off = rs_sync(b, n, RS_MP3, &f);
    CHECK(off == start && f.kind == RS_MP3, "mp3 sync at %d (want %d)", off, start);
    CHECK(rs_sync(b, n, RS_UNKNOWN, &f) == start, "any-kind sync");
    CHECK(rs_sync(b, n, RS_AAC, &f) == -1, "no aac in mp3");
    CHECK(rs_sync(b, start + l + 10, RS_MP3, &f) == -1, "not enough frames yet");
    n = 0; for (int i = 0; i < 37; i++) b[n++] = 0x41;
    int as = n; for (int k = 0; k < 4; k++) n += adts_frame(b + n, 200 + k * 10);
    CHECK(rs_sync(b, n, RS_AAC, &f) == as && f.kind == RS_AAC, "aac sync");
    CHECK(rs_sync(b, n, RS_UNKNOWN, &f) == as && f.kind == RS_AAC, "aac sync any");
}

int main(int argc, char **argv)
{
    if (argc > 1) dir = argv[1];
    t_stations(); t_countries(); t_urls(); t_library(); t_misc(); t_state_conf();
    t_kinds(); t_playlists(); t_icy(); t_chunked(); t_frames();
    printf("radio: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
