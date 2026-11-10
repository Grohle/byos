/* lyrics_test.c - pruebas en el PC del lector .lrc y del orden de resultados (src/lrc_core.inc.c).
 * bash tests/lyrics_test.sh */
#include <stdio.h>
#include "../src/lrc_core.inc.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQS(a, b) do { checks++; if (strcmp((a), (b))) { fails++; printf("FALLO %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)
#define EQI(a, b) do { checks++; long _a = (a), _b = (b); if (_a != _b) { fails++; printf("FALLO %s:%d: %s = %ld, esperado %ld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static const char *T(const struct lrc *l, int i) { return l->buf + l->ln[i].off; }
static int P(struct lrc *l, const char *s) { return lrc_parse(l, s, (int)strlen(s), 0); }

static void t_timestamps(void)
{
    EQI(lrc_ts("00:12.34", "00:12.34" + 8), 12340);
    EQI(lrc_ts("00:12.345", "00:12.345" + 9), 12345);
    EQI(lrc_ts("00:12.3", "00:12.3" + 7), 12300);
    EQI(lrc_ts("01:02", "01:02" + 5), 62000);
    EQI(lrc_ts("1:02.5", "1:02.5" + 6), 62500);
    EQI(lrc_ts("00:12:34", "00:12:34" + 8), 12340);         /* mm:ss:xx */
    EQI(lrc_ts("1:00:01.50", "1:00:01.50" + 10), 3601500);   /* h:mm:ss.xx */
    EQI(lrc_ts(" 00:05.00 ", " 00:05.00 " + 10), 5000);
    EQI(lrc_ts("00:12,34", "00:12,34" + 8), 12340);
    EQI(lrc_ts("103:00.00", "103:00.00" + 9), 6180000);
    EQI(lrc_ts("00:01.2345", "00:01.2345" + 10), 1234);
    EQI(lrc_ts("-00:01.00", "-00:01.00" + 9), 0);
    EQI(lrc_ts("ar:Queen", "ar:Queen" + 8), -1);
    EQI(lrc_ts("12", "12" + 2), -1);
    EQI(lrc_ts("00:1x", "00:1x" + 5), -1);
    EQI(lrc_ts("00:01.", "00:01." + 6), -1);
}

static void t_basic(void)
{
    struct lrc l;
    const char *s = "[ar:Artist]\n[ti:Title]\n[al:Album]\n[by:me]\n[length:03:20]\n[re:LRCLIB]\n[ve:1]\n"
                    "[00:01.00]one\n[00:02.50]two\n[00:04]three\n[00:05.123]four\n";
    CHECK(!P(&l, s));
    EQI(l.synced, 1); EQI(l.n, 4); EQI(l.offset, 0); EQI(l.has_offset, 0);
    EQI(l.ln[0].t, 1000); EQS(T(&l, 0), "one");
    EQI(l.ln[1].t, 2500); EQI(l.ln[2].t, 4000); EQI(l.ln[3].t, 5123); EQS(T(&l, 3), "four");
    lrc_free(&l);
}

static void t_multi_sort(void)
{
    struct lrc l;
    const char *s = "[00:30.00]late\n[00:10.00][00:50.00]chorus\n[00:20.00]mid\n[00:20.00]mid2 same time\n";
    CHECK(!P(&l, s));
    EQI(l.n, 5);
    EQI(l.ln[0].t, 10000); EQS(T(&l, 0), "chorus");
    EQI(l.ln[1].t, 20000); EQS(T(&l, 1), "mid");
    EQI(l.ln[2].t, 20000); EQS(T(&l, 2), "mid2 same time");       /* estable */
    EQI(l.ln[3].t, 30000); EQS(T(&l, 3), "late");
    EQI(l.ln[4].t, 50000); EQS(T(&l, 4), "chorus");
    lrc_free(&l);
    /* marcas separadas por espacios */
    CHECK(!P(&l, "[00:01.00] [00:03.00] x\n"));
    EQI(l.n, 2); EQS(T(&l, 0), "x"); EQI(l.ln[1].t, 3000);
    lrc_free(&l);
}

static void t_offset(void)
{
    struct lrc l;
    CHECK(!P(&l, "[offset:+500]\n[00:10.00]a\n[00:20.00]b\n"));
    EQI(l.offset, 500); EQI(l.has_offset, 1);
    /* positivo = antes: el verso de 10 s sale a los 9,5 s */
    EQI(lrc_at(&l, 9499, l.offset), -1);
    EQI(lrc_at(&l, 9500, l.offset), 0);
    EQI(lrc_at(&l, 19499, l.offset), 0);
    EQI(lrc_at(&l, 19500, l.offset), 1);
    EQI(lrc_next_in(&l, 9000, l.offset), 500);
    EQI(lrc_next_in(&l, 9500, l.offset), 10000);
    EQI(lrc_next_in(&l, 19500, l.offset), -1);
    lrc_free(&l);
    CHECK(!P(&l, "[offset: -250]\n[00:10.00]a\n"));
    EQI(l.offset, -250);
    EQI(lrc_at(&l, 10249, l.offset), -1); EQI(lrc_at(&l, 10250, l.offset), 0);
    lrc_free(&l);
}

static void t_switch_exact(void)
{
    struct lrc l;
    CHECK(!P(&l, "[00:01.00]a\n[00:02.00]b\n[00:03.00]c\n"));
    EQI(lrc_at(&l, 0, 0), -1);
    EQI(lrc_at(&l, 999, 0), -1);
    EQI(lrc_at(&l, 1000, 0), 0);          /* justo en la marca */
    EQI(lrc_at(&l, 1999, 0), 0);
    EQI(lrc_at(&l, 2000, 0), 1);
    EQI(lrc_at(&l, 99999, 0), 2);
    lrc_free(&l);
}

static void t_enhanced(void)
{
    struct lrc l;
    CHECK(!P(&l, "[00:12.00]<00:12.00>Hello <00:12.50>big   <00:13.00>world<00:13.50>\n[00:14.00] <00:14.00> spaced <00:14.40>out \n"));
    EQI(l.n, 2);
    EQS(T(&l, 0), "Hello big world");
    EQS(T(&l, 1), "spaced out");
    lrc_free(&l);
    /* <...> que no es marca: se queda */
    CHECK(!P(&l, "[00:01.00]a <3 b <tag>\n"));
    EQS(T(&l, 0), "a <3 b <tag>");
    lrc_free(&l);
}

static void t_line_endings_bom(void)
{
    struct lrc l;
    const char *s = "\xEF\xBB\xBF[ti:x]\r\n[00:01.00]crlf\r\n[00:02.00]cr only\r[00:03.00]lf\n[00:04.00]last no newline";
    CHECK(!P(&l, s));
    EQI(l.n, 4);
    EQS(T(&l, 0), "crlf"); EQS(T(&l, 1), "cr only"); EQS(T(&l, 2), "lf"); EQS(T(&l, 3), "last no newline");
    lrc_free(&l);
}

static int u16(char *o, const char *ascii_or_utf8, int le, int bom)
{
    int k = 0;
    if (bom) { o[k++] = le ? (char)0xFF : (char)0xFE; o[k++] = le ? (char)0xFE : (char)0xFF; }
    for (const char *s = ascii_or_utf8; *s; ) {
        unsigned c = (unsigned char)*s++;
        if (c >= 0xC0) { int n = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3; c &= 0x3F >> n; while (n--) c = c << 6 | ((unsigned char)*s++ & 63); }
        if (c >= 0x10000) { unsigned h = 0xD800 + ((c - 0x10000) >> 10), lo = 0xDC00 + ((c - 0x10000) & 0x3FF);
            o[k++] = (char)(le ? h & 255 : h >> 8); o[k++] = (char)(le ? h >> 8 : h & 255); c = lo; }
        o[k++] = (char)(le ? c & 255 : c >> 8); o[k++] = (char)(le ? c >> 8 : c & 255);
    }
    return k;
}
static void t_utf16(void)
{
    char b[512]; struct lrc l;
    const char *src = "[ar:Bj\xC3\xB6rk]\r\n[00:01.00]caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB5\r\n[00:02.00]two\r\n";
    for (int le = 0; le < 2; le++) for (int bom = 0; bom < 2; bom++) {
        int n = u16(b, src, le, bom);
        CHECK(!lrc_parse(&l, b, n, 0));
        EQI(l.n, 2); EQI(l.synced, 1);
        if (l.n == 2) { EQS(T(&l, 0), "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB5"); EQS(T(&l, 1), "two"); }
        lrc_free(&l);
    }
}

static void t_latin1(void)
{
    struct lrc l;
    CHECK(!P(&l, "[00:01.00]Coraz\xF3n espa\xF1ol\n"));
    EQS(T(&l, 0), "Coraz\xC3\xB3n espa\xC3\xB1ol");
    lrc_free(&l);
}

static void t_plain(void)
{
    struct lrc l;
    CHECK(!lrc_parse(&l, "\n\n[Chorus]\nLine one\n\n\n\nLine two\n[ar:x]\n\n", 40, "\xE2\x80\xA6"));
    EQI(l.synced, 0);
    EQI(l.n, 4);
    EQS(T(&l, 0), "[Chorus]");                     /* no es metadato: se queda */
    EQS(T(&l, 1), "Line one"); EQS(T(&l, 2), "\xE2\x80\xA6"); EQS(T(&l, 3), "Line two");
    EQI(lrc_at(&l, 5000, 0), -1);
    lrc_free(&l);
    /* sincronizada: los versos sin marca se descartan; vacios -> "…" */
    CHECK(!lrc_parse(&l, "Title line\n[00:01.00]a\n[00:02.00]\n[00:03.00]b\n", 46, "\xE2\x80\xA6"));
    EQI(l.n, 3); EQS(T(&l, 1), "\xE2\x80\xA6"); EQS(lrc_first_text(&l, "\xE2\x80\xA6"), "a");
    lrc_free(&l);
    CHECK(!P(&l, ""));
    EQI(l.n, 0);
    lrc_free(&l);
}

static void t_with_offset(void)
{
    const char *s = "\xEF\xBB\xBF[ar:A]\r\n[ti:T]\r\n[offset:+100]\r\n[00:01.00]a\r\n[00:02.00]b";
    int n; char *o = lrc_with_offset(s, (int)strlen(s), 500, &n);
    EQS(o, "\xEF\xBB\xBF[ar:A]\n[ti:T]\n[offset:+500]\n[00:01.00]a\n[00:02.00]b\n");
    struct lrc l; CHECK(!lrc_parse(&l, o, n, 0)); EQI(l.offset, 500); EQI(l.n, 2); lrc_free(&l);
    free(o);
    o = lrc_with_offset(s, (int)strlen(s), 0, &n);
    EQS(o, "\xEF\xBB\xBF[ar:A]\n[ti:T]\n[00:01.00]a\n[00:02.00]b\n");
    free(o);
    const char *s2 = "[00:01.00]a\n";
    o = lrc_with_offset(s2, (int)strlen(s2), -1250, &n);
    EQS(o, "\xEF\xBB\xBF[offset:-1250]\n[00:01.00]a\n");
    free(o);
    o = lrc_with_offset("[ar:x]\n", 7, 300, &n);
    EQS(o, "\xEF\xBB\xBF[ar:x]\n[offset:+300]\n");
    free(o);
    char txt[16];
    lrc_fmt_offset(500, txt, sizeof txt); EQS(txt, "+0.5 s");
    lrc_fmt_offset(-1250, txt, sizeof txt); EQS(txt, "-1.25 s");
    lrc_fmt_offset(0, txt, sizeof txt); EQS(txt, "0 s");
    lrc_fmt_offset(2000, txt, sizeof txt); EQS(txt, "+2.0 s");
}

static void t_sample_file(void)
{
    FILE *f = fopen("tests/lyrics_samples/get_lucky_986804.lrc", "rb"); CHECK(f != 0); if (!f) return;
    static char b[65536]; int n = (int)fread(b, 1, sizeof b, f); fclose(f);
    struct lrc l; CHECK(!lrc_parse(&l, b, n, "\xE2\x80\xA6"));
    EQI(l.synced, 1); CHECK(l.n > 40);
    EQI(l.ln[0].t, 30640); EQS(T(&l, 0), "Like the legend of the phoenix, huh");
    for (int i = 1; i < l.n; i++) CHECK(l.ln[i].t >= l.ln[i - 1].t);
    lrc_free(&l);
}

static int load(const char *p, char *b, int cap) { FILE *f = fopen(p, "rb"); if (!f) return -1; int n = (int)fread(b, 1, cap - 1, f); fclose(f); b[n] = 0; return n; }
static void t_rank(void)
{
    static char b[65536]; struct lrc_cand c[40]; char d[32];
    CHECK(load("tests/lyrics_samples/get_lucky_248.tsv", b, sizeof b) > 0);
    int n = lrc_parse_results(b, c, 40);
    EQI(n, 16);
    EQS(c[0].first, "Like the legend of the phoenix, huh");
    /* la cancion dura 6:09 (version del album): primero las de 369/370 s */
    n = lrc_rank(c, n, 369, "Daft Punk", "Get Lucky");
    CHECK(c[0].synced); CHECK(c[0].dd >= 0 && c[0].dd <= 2);
    EQI(lrc_dur_badge(c[0].dur, 369, d, sizeof d), 1);
    EQI(lrc_dur_badge(c[n - 1].dur, 369, d, sizeof d), 2);
    for (int i = 1; i < n; i++) CHECK(c[i - 1].synced >= c[i].synced);
    /* radio edit (248 s) */
    load("tests/lyrics_samples/get_lucky_248.tsv", b, sizeof b); n = lrc_parse_results(b, c, 40);
    n = lrc_rank(c, n, 248, "Daft Punk", "Get Lucky");
    CHECK(abs(c[0].dur - 248) <= 2);
    /* sin artista ni duracion: no rompe */
    CHECK(load("tests/lyrics_samples/despacito_noartist.tsv", b, sizeof b) > 0);
    n = lrc_parse_results(b, c, 40); CHECK(n > 0);
    n = lrc_rank(c, n, 0, "", "Despacito"); CHECK(n > 0);

    /* orden sintetico: sincronizada > duracion > coincidencia > orden */
    struct lrc_cand s[6]; memset(s, 0, sizeof s);
    const char *ids[] = { "1", "2", "3", "4", "5", "1" };
    const char *ti[] = { "Song", "Song (Live)", "Other", "Song", "Song", "Song" };
    const char *ar[] = { "Band", "Band", "Band", "Band feat. X", "Band", "Band" };
    int du[] = { 200, 201, 200, 230, 199, 200 }, sy[] = { 0, 1, 1, 1, 1, 0 };
    for (int i = 0; i < 6; i++) { strcpy(s[i].id, ids[i]); strcpy(s[i].title, ti[i]); strcpy(s[i].artist, ar[i]); s[i].dur = du[i]; s[i].synced = sy[i]; s[i].ord = i; }
    n = lrc_rank(s, 6, 200, "Band", "Song");
    EQI(n, 5);                                        /* id 1 repetido */
    EQS(s[0].id, "2");                                /* sync, 1 s, "Song (Live)" -> "song": exacto (4), llego antes */
    EQS(s[1].id, "5");                                /* sync, 1 s, titulo+artista exactos (4) */
    EQS(s[2].id, "3");                                /* sync, 0 s, titulo distinto */
    EQS(s[3].id, "4");                                /* sync, 30 s */
    EQS(s[4].id, "1");                                /* sin sincronizar */
    EQI(lrc_dur_badge(212, 200, d, sizeof d), 2); EQS(d, "+12 s");
    EQI(lrc_dur_badge(130, 200, d, sizeof d), 2); EQS(d, "-1:10");
    EQI(lrc_dur_badge(222, 222, d, sizeof d), 1); EQS(d, "3:42");
    EQI(lrc_dur_badge(222, 0, d, sizeof d), 0); EQS(d, "3:42");
    char x[64]; lrc_norm("Get Lucky (Radio Edit) - Remastered", x, sizeof x); EQS(x, "getlucky");
    lrc_norm("Beyonc\xC3\xA9 feat. Jay-Z", x, sizeof x); EQS(x, "beyonce");
    EQI(lrc_match1("Daft Punk feat. Pharrell", "Daft Punk"), 2);
    EQI(lrc_match1("Daft Punk, Pharrell Williams", "daft punk"), 1);
}

int main(void)
{
    t_timestamps(); t_basic(); t_multi_sort(); t_offset(); t_switch_exact(); t_enhanced();
    t_line_endings_bom(); t_utf16(); t_latin1(); t_plain(); t_with_offset(); t_sample_file(); t_rank();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
