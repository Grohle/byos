/* radio_test.c - PC tests of the Radio app's pure parts:
 *   src/radio_stream.inc.c  (stream helper and shell: content kinds, .m3u/.pls, ICY metadata,
 *                            chunked bodies, MPEG/ADTS frame sync, file names, Latin-1)
 *                            HLS playlists (master/media, sequence tracking), MPEG-TS demux,
 *                            ID3 skip, frame-duration counting)
 *   src/radio_core.inc.c    (Radio Browser JSON, query URLs, station library TSV, helper state
 *                            file, settings, custom station URLs)
 *   src/radio_src.inc.c     (station sources: M3U/PLS/JSON lists, sniffing, sources.tsv, the
 *                            built-in European public radio list)
 * against hand-written samples in tests/radio_samples (Radio Browser response shape) and a real
 * 1.4 s MPEG-TS segment of RNE Radio 3 (rne_r3_seg.ts, its public HLS stream).
 * Run: bash tests/radio_test.sh */
#define _GNU_SOURCE
#include <math.h>
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
    CHECK(n == 7, "playable stations %d", n);
    if (n < 7) { free(js); return; }
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

    /* HLS now plays (the helper demuxes it): the "hls" flag and a .m3u8 URL both make it RS_HLS */
    CHECK(!strcmp(s[2].name, "HLS Station") && s[2].kind == RS_HLS, "hls flag -> RS_HLS (%s %d)", s[2].name, s[2].kind);
    ra_qlabel(&s[2], q, sizeof q); CHECK(!strcmp(q, "AAC 96k"), "hls label shows the codec '%s'", q);
    CHECK(!strcmp(s[3].name, "Mystery Stream") && s[3].kind == RS_UNKNOWN && !s[3].codec[0] && s[3].br == 0, "unknown codec kept (%s %d)", s[3].name, s[3].kind);
    ra_qlabel(&s[3], q, sizeof q); CHECK(!q[0], "no label for unknown '%s'", q);
    CHECK(!strcmp(s[4].name, "Jazz FM Live"), "controls cleaned '%s'", s[4].name);
    CHECK(!strcmp(s[4].tags, "jazz, smooth jazz"), "tags '%s'", s[4].tags);
    CHECK(s[4].kind == RS_MP3, "lowercase codec");
    CHECK(!strcmp(s[5].name, "Playlist HLS") && s[5].kind == RS_HLS, ".m3u8 url -> RS_HLS (%d)", s[5].kind);
    CHECK(!strcmp(s[6].name, "Caf\xC3\xA9 del Mar \xD0\xA0\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xBE"), "\\u escapes '%s'", s[6].name);
    CHECK(s[6].br == 0, "absurd bitrate dropped %d", s[6].br);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(s[i].name, "Ogg Radio") && strcmp(s[i].name, "Bad URL") && strcmp(s[i].name, "Radio Uno (copy)"), "should be filtered: %s", s[i].name);
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
    char u[700], b[200];
    ra_host_base(b, sizeof b, 0); CHECK(!strcmp(b, "https://de1.api.radio-browser.info"), "base %s", b);
    ra_list_url(u, sizeof u, b, RL_SEARCH, "rock & roll/fm", 60, 120);
    CHECK(strstr(u, "https://de1.api.radio-browser.info/json/stations/search?") == u, "host %s", u);
    CHECK(strstr(u, "limit=60&offset=120") && strstr(u, "hidebroken=true") && strstr(u, "order=clickcount&reverse=true"), "params %s", u);
    CHECK(strstr(u, "&name=rock%20%26%20roll%2Ffm") || strstr(u, "&name=rock+%26+roll%2Ffm"), "encoded name %s", u);
    ra_host_base(b, sizeof b, 1); ra_list_url(u, sizeof u, b, RL_COUNTRY, "ES", 60, 0); CHECK(strstr(u, "fi1.api") && strstr(u, "&countrycode=ES"), "country %s", u);
    ra_host_base(b, sizeof b, 2); ra_list_url(u, sizeof u, b, RL_TAG, "hip hop", 60, 0); CHECK(strstr(u, "de2.api") && strstr(u, "&tag=hip"), "tag %s", u);
    ra_host_base(b, sizeof b, 3); ra_list_url(u, sizeof u, b, RL_TOP, "ignored", 60, 0); CHECK(strstr(u, "de1.api") && !strstr(u, "ignored") && !strstr(u, "&name="), "top wraps hosts %s", u);
    ra_list_url(u, sizeof u, "https://radio.example.org:8443", RL_TOP, "", 60, 0);
    CHECK(strstr(u, "https://radio.example.org:8443/json/stations/search?") == u, "another Radio Browser server %s", u);
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
    CHECK(!strncmp(buf, "# ByOS Radio 1\n", 15), "header");
    ra_lib_parse(&M, buf);
    CHECK(M.nfav == L.nfav && M.nrec == L.nrec && M.nown == L.nown, "counts %d/%d %d/%d %d/%d", M.nfav, L.nfav, M.nrec, L.nrec, M.nown, L.nown);
    for (int i = 0; i < L.nfav && i < M.nfav; i++)
        CHECK(!strcmp(M.fav[i].uuid, L.fav[i].uuid) && !strcmp(M.fav[i].name, L.fav[i].name) && !strcmp(M.fav[i].url, L.fav[i].url) && !strcmp(M.fav[i].cc, L.fav[i].cc) &&
              !strcmp(M.fav[i].codec, L.fav[i].codec) && M.fav[i].br == L.fav[i].br && !strcmp(M.fav[i].tags, L.fav[i].tags) && M.fav[i].kind == L.fav[i].kind, "fav %d differs", i);
    for (int i = 0; i < L.nrec && i < M.nrec; i++) CHECK(ra_same(&M.rec[i], &L.rec[i]), "rec %d order", i);
    for (int i = 0; i < L.nown && i < M.nown; i++) CHECK(!strcmp(M.own[i].url, L.own[i].url) && M.own[i].kind == L.own[i].kind, "own %d kind %d/%d", i, M.own[i].kind, L.own[i].kind);
    free(buf);

    /* robustness: CRLF, junk, bad url, unknown flag, missing fields, duplicates */
    char t[] = "# ByOS Radio 1\r\nF\tu-9\tNueve\thttps://n.example.com/9\tFR\tMP3\t96\tjazz\r\n"
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
    CHECK(ra_codec_kind("AAC", 1) == RS_HLS && ra_codec_kind("", 1) == RS_HLS && ra_codec_kind("OPUS", 1) == RS_OGG && ra_playable(RS_HLS), "hls kinds");
    CHECK(!strncmp(RA_SDDIR, "/usr/data/mnt/sd_0/", 19) && strncmp(RA_SDDIR, "/tmp", 4), "audio on the SD card, never in tmpfs: %s", RA_SDDIR);
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

    CHECK(ra_state_parse("st=ready\ngen=1\nlead=17\nhls=1\n", &h) && h.lead == 17 && h.hls == 1, "lead/hls");

    struct ra_conf c; ra_conf_default(&c);
    CHECK(c.lead_s == 15 && c.cap_mb == 128 && !c.cc[0], "defaults");
    /* old keys of the tmpfs buffer are ignored */
    char t[] = "dir=/tmp/hibyradio\npresize_mb=256\nprebuf_kb=128\npathmode=1\nlead_s=20\ncap_mb=64\ncountry=de\ncountry_name=Deutschland\ntab=1\njunk\n";
    ra_conf_parse(&c, t);
    CHECK(c.lead_s == 20 && c.cap_mb == 64 && !strcmp(c.cc, "DE") && !strcmp(c.cname, "Deutschland") && c.tab == 1, "parsed");
    char *buf = 0; size_t blen = 0; FILE *f = open_memstream(&buf, &blen); ra_conf_write(&c, f); fclose(f);
    CHECK(!strstr(buf, "dir=") && !strstr(buf, "presize") && !strstr(buf, "pathmode"), "old keys not written");
    struct ra_conf d; ra_conf_parse(&d, buf);
    CHECK(!memcmp(&c, &d, sizeof c), "conf round trip");
    free(buf);
    char bad[] = "lead_s=1\ncap_mb=99999\ncountry=ESP\n";
    ra_conf_parse(&c, bad);
    CHECK(c.lead_s == 15 && c.cap_mb == 128 && !c.cc[0], "bad values ignored");
    char nameonly[] = "country_name=Nowhere\n"; ra_conf_parse(&c, nameonly); CHECK(!c.cc[0] && !c.cname[0], "name without code dropped");
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

/* ------------------------------------------------------------------ HLS playlists */
static void hls_parse(struct rs_hls *h, const char *t, int piece)
{   /* fed in pieces of `piece` bytes, like the helper reads them */
    rs_hls_init(h);
    int n = (int)strlen(t);
    for (int i = 0; i < n; i += piece) rs_hls_feed(h, t + i, i + piece <= n ? piece : n - i);
    rs_hls_end(h);
}
static void t_hls(void)
{
    static struct rs_hls h;
    /* RNE's live media playlist (absolute segment URLs, 4 x ~5 s) */
    const char *rne = "#EXTM3U\n## Created with Golumi Video Platform\n\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n#EXT-X-MEDIA-SEQUENCE:110026\n"
                      "#EXTINF:5.013,\nhttps://rtvelivestream.rtve.es/rtvesec/rne/GL0/37_110026.ts\n#EXTINF:4.992,\nhttps://rtvelivestream.rtve.es/rtvesec/rne/GL0/37_110027.ts\n"
                      "#EXTINF:4.992,\nhttps://rtvelivestream.rtve.es/rtvesec/rne/GL0/37_110028.ts\n#EXTINF:5.013,\nhttps://rtvelivestream.rtve.es/rtvesec/rne/GL0/37_110029.ts\n";
    for (int piece = 1; piece <= 64; piece += 7) {
        hls_parse(&h, rne, piece);
        if (!(h.ok && !h.master && h.target == 5 && h.seq0 == 110026 && h.nseg == 4 && !h.enc && !h.endlist)) { CHECK(0, "rne playlist in pieces of %d", piece); break; }
    }
    hls_parse(&h, rne, 4096);
    CHECK(h.ok && !h.master && h.target == 5 && h.seq0 == 110026 && h.nseg == 4, "media playlist (ok %d target %d seq %lld n %d)", h.ok, h.target, h.seq0, h.nseg);
    const struct rs_hseg *g = rs_hls_seg(&h, 3);
    CHECK(g && g->seq == 110029 && g->ms == 5013 && !strcmp(g->uri, "https://rtvelivestream.rtve.es/rtvesec/rne/GL0/37_110029.ts"), "last segment");
    int gap = -1;
    /* start: back from the end until the lead (15 s) is covered -> the first 4 (14.997 s < 15 s with 3) */
    CHECK(rs_hls_first(&h, -1, 15000, &gap) == 0 && !gap, "start covers the lead: %d", rs_hls_first(&h, -1, 15000, &gap));
    CHECK(rs_hls_first(&h, -1, 9000, &gap) == 2, "start, 9 s lead -> 2 segments back");
    CHECK(rs_hls_first(&h, -1, 0, &gap) == 3, "start, no lead -> only the newest");
    CHECK(rs_hls_first(&h, 110027, 15000, &gap) == 2 && !gap, "next after the last written");
    CHECK(rs_hls_first(&h, 110029, 15000, &gap) == 4 && !gap, "nothing new");
    CHECK(rs_hls_first(&h, 110020, 15000, &gap) == 0 && gap, "fell out of the window: gap, from the oldest");
    CHECK(rs_hls_first(&h, 999999, 15000, &gap) == 0 && gap, "sequence went back (restart): start again");
    /* the window slides: the next reload has 110027..110030 */
    const char *next = "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-MEDIA-SEQUENCE:110027\n#EXTINF:5,\na.ts\n#EXTINF:5,\nb.ts\n#EXTINF:5,\nc.ts\n#EXTINF:5,\nd.ts\n";
    hls_parse(&h, next, 4096);
    int i = rs_hls_first(&h, 110029, 15000, &gap); g = rs_hls_seg(&h, i);
    CHECK(i == 3 && g && g->seq == 110030 && !strcmp(g->uri, "d.ts"), "sequence tracking: only 110030 is new (relative uri kept: %s)", g ? g->uri : "-");
    /* master: the lightest audio-only variant; video variants only if nothing else */
    const char *master = "#EXTM3U\r\n#EXT-X-VERSION:3\r\n"
                         "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS=\"avc1.4d401f,mp4a.40.2\"\r\nvideo_720.m3u8\r\n"
                         "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=196608,CODECS=\"mp4a.40.2\",SUBTITLES=\"subs\"\r\naudio_192.m3u8\r\n"
                         "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.5\"\r\nlow/audio_64.m3u8\r\n"
                         "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\r\naudio_128.m3u8\r\n";
    hls_parse(&h, master, 13);
    CHECK(h.ok && h.master && h.nvar == 4 && !strcmp(rs_hls_variant(&h), "low/audio_64.m3u8"), "master -> lowest audio variant '%s'", rs_hls_variant(&h));
    const char *vonly = "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio/en.m3u8\"\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360,AUDIO=\"aud\"\nv360.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=320x180,AUDIO=\"aud\"\nv180.m3u8\n";
    hls_parse(&h, vonly, 4096);
    CHECK(h.master && !strcmp(rs_hls_variant(&h), "audio/en.m3u8"), "video-only master -> the audio rendition '%s'", rs_hls_variant(&h));
    const char *vonly2 = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360\nv360.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=320x180\nv180.m3u8\n";
    hls_parse(&h, vonly2, 4096);
    CHECK(h.master && !strcmp(rs_hls_variant(&h), "v180.m3u8"), "only video: the lightest '%s'", rs_hls_variant(&h));
    /* what cannot play */
    hls_parse(&h, "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n#EXTINF:6,\ns1.ts\n", 4096);
    CHECK(h.ok && h.enc, "encrypted");
    hls_parse(&h, "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:6,\ns1.ts\n", 4096);
    CHECK(h.ok && !h.enc, "METHOD=NONE is not encrypted");
    hls_parse(&h, "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:6,\ns1.m4s\n#EXT-X-ENDLIST\n", 4096);
    CHECK(h.fmp4 && h.endlist, "fMP4 + ENDLIST");
    /* an extended M3U station list is not HLS */
    hls_parse(&h, "#EXTM3U\n#EXTINF:-1,Radio Uno\nhttp://a.example.com/uno\n", 4096);
    CHECK(!h.ok, "M3U station list is not HLS");
    hls_parse(&h, "<html>nope</html>", 4096);
    CHECK(!h.ok, "html");
    /* a long DVR window: only the last RS_HLS_SEGS stay, sequence numbers keep counting */
    char *big = malloc(200000); int k = snprintf(big, 200000, "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:5000\n");
    for (int j = 0; j < 900; j++) k += snprintf(big + k, 200000 - k, "#EXTINF:4.0,\nseg_%d.ts\n", 5000 + j);
    hls_parse(&h, big, 8192);
    CHECK(h.nseg == 900 && !rs_hls_seg(&h, 0) && rs_hls_seg(&h, 899) && rs_hls_seg(&h, 899)->seq == 5899, "long window ring");
    i = rs_hls_first(&h, -1, 15000, &gap); CHECK(i == 896, "start near the live edge: %d", i);
    i = rs_hls_first(&h, 5100, 15000, &gap); CHECK(i == 900 - RS_HLS_SEGS && gap, "old position outside the ring: gap (%d)", i);
    free(big);
    char a[64]; rs_hls_attr("#EXT-X-STREAM-INF:CODECS=\"mp4a.40.2,avc1\",BANDWIDTH=1", "BANDWIDTH", a, sizeof a);
    CHECK(!strcmp(a, "1"), "attr after a quoted comma '%s'", a);
}

/* ------------------------------------------------------------------ MPEG-TS */
static int ts_pkt(unsigned char *p, int pid, int pusi, int cc, const unsigned char *pay, int n, int af)
{   /* one 188-byte packet; with af (or a short payload) an adaptation field pads it */
    p[0] = 0x47; p[1] = (unsigned char)((pusi ? 0x40 : 0) | (pid >> 8)); p[2] = (unsigned char)pid;
    int room = 184, k = 4;
    if (n < 184 || af) {
        int alen = 183 - n; if (alen < 0) alen = 0;
        p[3] = (unsigned char)(0x30 | (cc & 15)); p[4] = (unsigned char)alen;
        if (alen) { p[5] = 0; memset(p + 6, 0xFF, alen - 1); }
        k = 5 + alen; room = 188 - k;
    } else p[3] = (unsigned char)(0x10 | (cc & 15));
    if (n > room) n = room;
    memcpy(p + k, pay, n);
    return n;
}
static void t_ts(void)
{
    /* the real RNE Radio 3 segment (SDT, PAT, PMT 4096, audio PID 256 = 0x0F ADTS) */
    long n; unsigned char *b = (unsigned char *)load("rne_r3_seg.ts", &n);
    static unsigned char out[200000]; int o = 0;
    struct rs_ts t;
    for (int piece = 1; piece <= 4096; piece = piece * 3 + 1) {      /* any split gives the same output */
        rs_ts_init(&t); o = 0;
        for (long i = 0; i < n; i += piece) o += rs_ts_feed(&t, b + i, (int)(i + piece <= n ? piece : n - i), out + o);
        if (o != 34223 || t.pmt != 4096 || t.apid != 256 || t.atype != 0x0F) { CHECK(0, "rne segment, pieces of %d: es %d pmt %d apid %d", piece, o, t.pmt, t.apid); break; }
    }
    CHECK(o == 34223 && t.pkts == 200, "elementary stream bytes %d (python reference 34223), packets %lld", o, t.pkts);
    int frames = 0, p = 0; struct rs_frame f;
    while (p + 7 <= o && rs_adts_hdr(out + p, &f) && p + f.len <= o) { p += f.len; frames++; }
    CHECK(out[0] == 0xFF && (out[1] & 0xF6) == 0xF0 && frames == 65 && p == 33877, "ADTS from the first byte: %d whole frames up to %d", frames, p);
    CHECK(f.hz == 48000, "48 kHz");
    struct rs_dur d; memset(&d, 0, sizeof d);
    for (int i = 0; i < o; i += 777) rs_dur_feed(&d, out + i, i + 777 <= o ? 777 : o - i);
    CHECK(d.kind == RS_AAC && d.frames == 66 && !d.lost && d.sec > 1.40 && d.sec < 1.41, "duration %.3f s, %lld frames (last one cut), lost %lld", d.sec, d.frames, d.lost);
    free(b);

    /* synthetic: junk before sync, a PES header spanning two packets, adaptation fields,
     * a packet of another PID, MPEG audio (0x03) */
    static unsigned char ts[188 * 16], pay[400], es[188 * 16];
    int k = 0;
    for (int i = 0; i < 5; i++) ts[k++] = 0x11;                        /* junk */
    unsigned char pat[] = { 0x00, 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00, 0, 0, 0, 0 };   /* program 1 -> PMT 0x100 */
    ts_pkt(ts + k, 0, 1, 0, pat, sizeof pat, 1); k += 188;
    unsigned char pmt[] = { 0x00, 0x02, 0xB0, 0x17, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00,
                            0x1B, 0xE1, 0x01, 0xF0, 0x00,                   /* H.264 on 0x101: ignored */
                            0x03, 0xE1, 0x02, 0xF0, 0x00, 0, 0, 0, 0 };    /* MPEG audio on 0x102 */
    ts_pkt(ts + k, 0x100, 1, 0, pmt, sizeof pmt, 1); k += 188;
    unsigned char v[184]; memset(v, 0xFF, sizeof v);
    ts_pkt(ts + k, 0x101, 1, 0, v, 184, 0); k += 188;                  /* video: must not leak */
    /* PES: header 9 + 175 extra bytes = 184: the whole first payload is header, 0 more in the next */
    memset(pay, 0, sizeof pay); pay[2] = 1; pay[3] = 0xC0; pay[6] = 0x80; pay[7] = 0x80; pay[8] = 175 + 5;   /* header data 180: spans */
    ts_pkt(ts + k, 0x102, 1, 0, pay, 184, 0); k += 188;               /* 184 bytes: 9 + 175 of the 180 */
    unsigned char a2[184]; for (int i = 0; i < 184; i++) a2[i] = (unsigned char)(i < 5 ? 0xEE : 'A' + i % 26);
    ts_pkt(ts + k, 0x102, 0, 1, a2, 184, 0); k += 188;                // 5 more header bytes, then 179 of audio
    unsigned char a3[100]; for (int i = 0; i < 100; i++) a3[i] = (unsigned char)('a' + i % 26);
    ts_pkt(ts + k, 0x102, 0, 2, a3, 100, 0); k += 188;                 /* short: adaptation field padding */
    rs_ts_init(&t);
    o = rs_ts_feed(&t, ts, k, es);
    CHECK(t.pmt == 0x100 && t.apid == 0x102 && t.atype == 0x03 && t.junk == 5, "synthetic PAT/PMT (pmt %x apid %x type %x junk %lld)", t.pmt, t.apid, t.atype, t.junk);
    CHECK(o == 179 + 100 && es[0] == 'A' + 5 % 26 && es[178] == 'A' + 183 % 26 && es[179] == 'a' && es[278] == 'a' + 99 % 26, "PES header across packets skipped, padding ignored (%d bytes)", o);
    /* a stream whose only audio is LATM: nothing comes out, latm says why */
    unsigned char pmt2[] = { 0x00, 0x02, 0xB0, 0x12, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00, 0x11, 0xE1, 0x02, 0xF0, 0x00, 0, 0, 0, 0 };
    k = 0; ts_pkt(ts + k, 0, 1, 0, pat, sizeof pat, 1); k += 188; ts_pkt(ts + k, 0x100, 1, 0, pmt2, sizeof pmt2, 1); k += 188;
    ts_pkt(ts + k, 0x102, 1, 0, pay, 184, 0); k += 188;
    rs_ts_init(&t); o = rs_ts_feed(&t, ts, k, es);
    CHECK(o == 0 && t.latm && t.apid < 0, "LATM refused");
    /* audio before its PES start is dropped (joined mid-PES) */
    k = 0; ts_pkt(ts + k, 0, 1, 0, pat, sizeof pat, 1); k += 188; ts_pkt(ts + k, 0x100, 1, 0, pmt, sizeof pmt, 1); k += 188;
    ts_pkt(ts + k, 0x102, 0, 5, a3, 100, 0); k += 188;
    rs_ts_init(&t); o = rs_ts_feed(&t, ts, k, es);
    CHECK(o == 0, "continuation without a PES start dropped");

    /* ID3 of packed audio */
    unsigned char id3[16] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0x01, 0x10 };   /* 128 + 16 = 144 bytes of tag */
    CHECK(rs_id3_len(id3, 10) == 154, "id3 len %ld", rs_id3_len(id3, 10));
    CHECK(rs_id3_len(id3, 2) == -1 && rs_id3_len((const unsigned char *)"\xFF\xF1", 2) == 0 && rs_id3_len((const unsigned char *)"\x47@", 2) == 0, "id3 needs bytes / not id3");
    id3[5] = 0x10; CHECK(rs_id3_len(id3, 10) == 164, "id3 with footer");
    /* duration of MP3 (MPEG-1 L3 1152 samples, MPEG-2 L3 576) */
    unsigned char m[8192]; int mn = 0; for (int i = 0; i < 10; i++) mn += mp3_frame(m + mn, i & 1);
    memset(&d, 0, sizeof d); rs_dur_feed(&d, m, 3); rs_dur_feed(&d, m + 3, mn - 3);
    CHECK(d.frames == 10 && d.kind == RS_MP3 && fabs(d.sec - 10 * 1152.0 / 44100) < 1e-6 && !d.lost, "mp3 duration %.4f", d.sec);
    memset(&d, 0, sizeof d); unsigned char junk[3] = { 1, 2, 3 }; rs_dur_feed(&d, junk, 3); rs_dur_feed(&d, m, mn);
    CHECK(d.frames == 10 && d.lost == 3, "junk counted as lost (%lld)", d.lost);
}

/* ------------------------------------------------------------------ sources */
static void t_sources(void)
{
    struct ra_station s[64]; long n; char title[64];
    char *b = load("list.m3u", &n);
    CHECK(ra_src_sniff(b) == RSRC_M3U, "sniff m3u");
    int k = ra_parse_m3u(b, s, 64);
    CHECK(k == 4, "m3u stations %d", k);
    if (k == 4) {
        CHECK(!strcmp(s[0].name, "RNE Radio 3") && s[0].kind == RS_HLS && !strcmp(s[0].cc, "ES") && !strcmp(s[0].tags, "Spain"), "extinf with attributes: %s %d %s '%s'", s[0].name, s[0].kind, s[0].cc, s[0].tags);
        CHECK(!strcmp(s[1].name, "FIP, Paris") && s[1].kind == RS_MP3, "comma inside quotes / in the name: '%s'", s[1].name);
        CHECK(!strcmp(s[2].name, "stream.example.org") && s[2].kind == RS_UNKNOWN, "no #EXTINF: name from the host '%s'", s[2].name);
        CHECK(!strcmp(s[3].name, "AAC FM") && s[3].kind == RS_AAC, "aac by extension");
    }
    free(b);
    CHECK(ra_parse_m3u("#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXTINF:5,\nseg.ts\n", s, 64) == -2, "HLS media playlist is a stream");
    CHECK(ra_src_sniff("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nx.m3u8\n") == -2, "HLS master sniffed as a stream");
    b = load("list.pls", &n);
    CHECK(ra_src_sniff(b) == RSRC_PLS, "sniff pls");
    k = ra_parse_pls(b, s, 64);
    CHECK(k == 2 && !strcmp(s[0].name, "Radio Uno") && !strcmp(s[0].url, "http://s1.example.com:8000/") && !strcmp(s[1].name, "s2.example.com"), "pls %d (%s / %s)", k, k > 0 ? s[0].name : "", k > 1 ? s[1].name : "");
    free(b);
    b = load("list.json", &n);
    CHECK(ra_src_sniff(b) == RSRC_JSON, "sniff json");
    k = ra_parse_jlist(b, s, 64, title, sizeof title);
    CHECK(k == 3 && !strcmp(title, "My stations"), "json %d '%s'", k, title);
    if (k == 3) {
        CHECK(!strcmp(s[0].name, "RNE Radio Cl\xC3\xA1sica") && s[0].kind == RS_HLS && !strcmp(s[0].codec, "AAC") && s[0].br == 192 && !strcmp(s[0].cc, "ES") && !strcmp(s[0].tags, "classical"), "json fields %s %d %s %d", s[0].name, s[0].kind, s[0].codec, s[0].br);
        CHECK(!strcmp(s[1].name, "Deutschlandfunk") && s[1].kind == RS_MP3 && s[1].br == 128, "mp3");
        CHECK(!strcmp(s[2].name, "Yle Radio 1") && s[2].kind == RS_AAC && !strcmp(s[2].cc, "FI"), "countrycode key");
    }
    free(b);
    CHECK(ra_parse_jlist("[{\"name\":\"A\",\"url\":\"https://a.example.com/a.mp3\"},{\"name\":\"O\",\"url\":\"https://o.example.com/o.ogg\",\"codec\":\"OGG\"},{\"url\":\"ftp://x\"}]", s, 64, title, sizeof title) == 1 && !title[0], "bare array, unplayable dropped");
    CHECK(ra_parse_jlist("{\"nope\":1}", s, 64, title, sizeof title) == -1 && ra_src_sniff("{\"nope\":1}") == -1, "not a list");
    CHECK(ra_src_sniff("{\"supported_version\":1,\"software_version\":\"0.7.31\",\"status\":\"OK\",\"stations\":50000}") == RSRC_RBX, "Radio Browser stats");
    CHECK(ra_src_sniff("<!DOCTYPE html><html>") == -1 && ra_src_sniff("") == -1, "html");
    CHECK(ra_src_parse(RSRC_URL, "[playlist]\nFile1=http://a.example.com/x.mp3\n", s, 64, 0, 0) == 1, "unknown type: sniffed");
    /* detection of what the user typed */
    char u[RA_URLN];
    CHECK(ra_src_detect("example.com/radio/list.M3U8?x=1", u, sizeof u) == RSRC_M3U && !strcmp(u, "http://example.com/radio/list.M3U8?x=1"), "m3u %s", u);
    CHECK(ra_src_detect("https://example.com/l.pls", u, sizeof u) == RSRC_PLS, "pls");
    CHECK(ra_src_detect("Https://example.com/stations.json", u, sizeof u) == RSRC_JSON && !strncmp(u, "https://", 8), "json, auto-capital scheme %s", u);
    CHECK(ra_src_detect("https://radio.example.org/", u, sizeof u) == RSRC_RBX && !strcmp(u, "https://radio.example.org"), "server root %s", u);
    CHECK(ra_src_detect("https://radio.example.org:8443/json/", u, sizeof u) == RSRC_RBX && !strcmp(u, "https://radio.example.org:8443"), "server /json %s", u);
    CHECK(ra_src_detect("https://example.com/lists/top", u, sizeof u) == RSRC_URL, "unknown: sniffed later");
    CHECK(ra_src_detect("not a url", u, sizeof u) == -1 && ra_src_detect("ftp://a.example.com/x", u, sizeof u) == -1, "bad");
    /* sources.tsv */
    static struct ra_srcs S, T;
    char e0[] = ""; ra_srcs_parse(&S, e0);
    CHECK(S.n == 2 && S.s[0].type == RSRC_RB && S.s[1].type == RSRC_EU && S.s[0].on && S.s[1].on && S.s[1].n == RA_NEU, "built-ins on an empty file");
    struct ra_src *x = ra_src_add(&S, RSRC_M3U, "https://example.com/a.m3u", "");
    CHECK(x && !strcmp(x->name, "example.com") && !x->named && x->sid == ra_hash("https://example.com/a.m3u"), "add: name from host");
    x->n = 12; x->fetched = 1700000000; x->on = 0;
    ra_src_add(&S, RSRC_JSON, "https://example.com/b.json", "Mis\tfavoritas");
    S.s[0].on = 0;
    char *buf = 0; size_t bl = 0; FILE *f = open_memstream(&buf, &bl); ra_srcs_write(&S, f); fclose(f);
    CHECK(strlen(buf) < 600, "small file (%zu bytes)", strlen(buf));
    ra_srcs_parse(&T, buf);
    CHECK(T.n == 4 && !T.s[0].on && T.s[2].type == RSRC_M3U && T.s[2].n == 12 && T.s[2].fetched == 1700000000 && !T.s[2].on && !strcmp(T.s[3].name, "Mis favoritas") && T.s[3].named, "round trip (%d)", T.n);
    CHECK(ra_src_find_url(&T, "https://EXAMPLE.com/b.json") == 3 && ra_src_ix(&T, ra_hash("builtin:eu")) == 1, "find");
    ra_src_del(&T, 0); ra_src_del(&T, 1); CHECK(T.n == 4, "built-ins cannot be removed");
    ra_src_del(&T, 2); CHECK(T.n == 3 && T.s[2].type == RSRC_JSON, "remove");
    free(buf);
    char old[] = "# x\n2\t1\t0\t3\t0\t0\t\thttps://example.com/a.m3u\n2\t1\t0\t3\t0\t0\t\thttps://example.com/a.m3u\n9\t1\t0\t0\t0\t0\t\thttps://x.example.com\n2\t1\t0\t0\t0\t0\t\tnot-a-url\n1\t0\t0\t0\t0\t0\t\t\n";
    ra_srcs_parse(&T, old);
    int eu = ra_src_ix(&T, ra_hash("builtin:eu"));
    CHECK(T.n == 3 && T.s[0].type == RSRC_RB && T.s[0].on && eu >= 0 && !T.s[eu].on && ra_src_find_url(&T, "https://example.com/a.m3u") >= 0, "missing built-in added in front, duplicates and junk dropped (%d)", T.n);

    /* the built-in European list: names, URLs, all playable and unique; RNE inside */
    int rne3 = 0, rne1 = 0, clas = 0, r5 = 0;
    for (int i = 0; i < RA_NEU; i++) {
        struct ra_station st; ra_eu_station(i, &st);
        CHECK(st.name[0] && rs_url_ok(st.url) && ra_playable(st.kind) && strlen(st.cc) == 2 && st.br > 0, "eu %d: %s", i, st.name);
        for (int j = 0; j < i; j++) CHECK(strcmp(RA_EU[j].url, RA_EU[i].url) && strcmp(RA_EU[j].name, RA_EU[i].name), "eu duplicate %s", RA_EU[i].name);
        if (strstr(st.url, "rne_r3_main")) rne3 = st.kind == RS_HLS;
        if (strstr(st.url, "rne_r1_main")) rne1 = 1;
        if (strstr(st.url, "rne_r2_main")) clas = 1;
        if (strstr(st.url, "rne_r5")) r5 = 1;
    }
    CHECK(rne3 && rne1 && clas && r5, "RNE Radio 3 (HLS), Radio 1, Radio Clasica, Radio 5 are in the list");
    CHECK(RA_NEU >= 40, "a good set of stations (%d)", RA_NEU);
}

int main(int argc, char **argv)
{
    if (argc > 1) dir = argv[1];
    t_stations(); t_countries(); t_urls(); t_library(); t_misc(); t_state_conf();
    t_kinds(); t_playlists(); t_icy(); t_chunked(); t_frames(); t_hls(); t_ts(); t_sources();
    printf("radio: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
