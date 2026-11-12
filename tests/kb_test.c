/* kb_test.c - host tests (x86, WSL) of the keyboard input engine (src/kb_ime.inc.c) with the firmware fonts.
 *   bash tests/kb_test.sh
 * Checks:
 *   - layouts of the 13 languages: every key (all layers, shift, number row) and long-press variant has a
 *     glyph; key widths >= 20 px; at most 64 keys;
 *   - up_case: Latin-1, Latin Extended-A, Cyrillic (Russian and Ukrainian);
 *   - Hangul 2-set: sequences, backspace walk (달가 -> 닭 -> 달 -> 다 -> ㄷ -> empty), and all 11172 syllables
 *     typed and deleted key by key, plus random runs of three syllables (composition stays in the text);
 *   - romaji -> kana (Hepburn, kunrei, small kana, doubled consonants, n) and katakana;
 *   - pinyin: segmentation, prefixes, first candidates, every syllable has characters with glyphs,
 *     simplified -> traditional, sorted tables, table sizes;
 *   - Thai: marks do not advance; the cursor never stops between a consonant and its mark;
 *   - edit engine: candidates/preedit never enter the text until committed; external cursor moves commit. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define W 320
#define H 480
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
#include "i18n.inc.c"
#include "text.inc.c"
#include "kb_ime.inc.c"

static int fails, checks;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char buf[512];
static struct ebuf E;
static void clear(void) { buf[0] = 0; E = (struct ebuf){ buf, 0, sizeof buf, 0 }; kbi_drop(); }
/* types a UTF-8 string key by key on layout l ("\b" = backspace, " " = space, "\n" = enter) */
static void type(int l, const char *s)
{
    int c;
    while ((c = utf8_next(&s))) {
        if (c == '\b') kbi_back(&E, l);
        else if (c == ' ') { if (!kbi_space(&E, l)) eb_insert(&E, " "); }
        else if (c == '\n') { if (!kbi_enter(&E, l)) eb_insert(&E, "\n"); }
        else kbi_char(&E, l, c);
    }
}
static int has_glyph(int c) { int gi; font_glyph(c, &gi); return gi != 0; }

static void test_layouts(void)
{
    int nkeys_max = 0, minkw = 99;
    for (int l = 0; l < KB_NLAY; l++) {
        const struct kb_lay *k = kb_lay(l);
        CHK(!strcmp(k->code, LANG_CODES[l]), "layout %d code %s != %s", l, k->code, LANG_CODES[l]);
        const char *t = k->name; int c;
        while ((c = utf8_next(&t))) CHK(has_glyph(c), "[%s] name glyph U+%04X", k->code, c);
        for (t = k->abc; (c = utf8_next(&t));) CHK(has_glyph(c), "[%s] abc glyph U+%04X", k->code, c);
        for (t = k->space; (c = utf8_next(&t));) if (c != ' ') CHK(has_glyph(c), "[%s] space glyph U+%04X", k->code, c);
        for (int layer = 0; layer < 3; layer++) for (int sh = 0; sh < 2; sh++) for (int nr = 0; nr < 2; nr++) {
            const char *r[6]; int n = kb_lay_rows(l, layer, sh, nr, r), tot = 5;
            CHK(n == (k->nrows == 4 ? 4 : 3 + nr), "[%s] layer %d numrow %d: %d rows", k->code, layer, nr, n);
            for (int i = 0; i < n; i++) {
                int cnt = 0; const char *s = r[i];
                while ((c = utf8_next(&s))) {
                    cnt++;
                    CHK(has_glyph(c), "[%s] layer %d shift %d: no glyph U+%04X in \"%s\"", k->code, layer, sh, c, r[i]);
                    if (c > 0xFFFF) CHK(0, "[%s] non-BMP key U+%04X", k->code, c);
                    const char *a = kb_lay_alts(l, sh && (k->flags & KL_CASE) ? c : c); int ac, na = 0;
                    if (a) while ((ac = utf8_next(&a)) && ac != '|') { na++; CHK(has_glyph(ac), "[%s] alt of U+%04X: no glyph U+%04X", k->code, c, ac); if (k->flags & KL_CASE) CHK(has_glyph(up_case(ac)), "[%s] alt upper U+%04X", k->code, up_case(ac)); }
                    CHK(na <= 8, "[%s] %d variants for U+%04X (max 8)", k->code, na, c);
                    if (layer == 0 && (k->flags & KL_CASE)) CHK(has_glyph(up_case(c)), "[%s] no glyph for upper U+%04X", k->code, up_case(c));
                }
                tot += cnt;
                int kw, gap, last = i == n - 1;
                int sw = layer == 0 ? kb_side_w(cnt) : 42, avail = last ? W - 2 * (3 + sw + 4) : W - 6;
                kb_row_geom(cnt, avail, last && layer ? 46 : 30, &kw, &gap);
                CHK(kw >= 20, "[%s] layer %d row %d: %d keys of %d px", k->code, layer, i, cnt, kw);
                if (kw < minkw) minkw = kw;
            }
            CHK(tot <= 64, "[%s] %d keys", k->code, tot);
            if (tot > nkeys_max) nkeys_max = tot;
        }
    }
    printf("layouts: 13, at most %d keys, narrowest key %d px\n", nkeys_max, minkw);
}

static void test_upcase(void)
{
    static const int P[][2] = { { 'a', 'A' }, { 0xE9, 0xC9 }, { 0xFF, 0x178 }, { 0xDF, 0xDF }, { 0xF7, 0xF7 }, { 0x105, 0x104 },
        { 0x107, 0x106 }, { 0x119, 0x118 }, { 0x142, 0x141 }, { 0x144, 0x143 }, { 0x15B, 0x15A }, { 0x17A, 0x179 }, { 0x17C, 0x17B },
        { 0x430, 0x410 }, { 0x44F, 0x42F }, { 0x451, 0x401 }, { 0x454, 0x404 }, { 0x456, 0x406 }, { 0x457, 0x407 }, { 0x491, 0x490 },
        { 0x44A, 0x42A }, { 0x3131, 0x3131 }, { 0xE01, 0xE01 }, { 0x4E2D, 0x4E2D } };
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) CHK(up_case(P[i][0]) == P[i][1], "up_case(U+%04X) = U+%04X, want U+%04X", P[i][0], up_case(P[i][0]), P[i][1]);
}

/* ---------------------------------------------------------------- Hangul */
static int ko_keys(int s, int *k)   /* syllable -> keys (compatibility jamo) */
{
    int n = 0, L = (s - 0xAC00) / 588, V = (s - 0xAC00) % 588 / 28, T = (s - 0xAC00) % 28;
    for (int i = 0; i < 30; i++) if (HG_CHO[i] == L) { k[n++] = 0x3131 + i; break; }
    int v = 0x314F + V, split = 0;
    for (int i = 0; i < 7; i++) if (HG_VV[i][2] == v) { k[n++] = HG_VV[i][0]; k[n++] = HG_VV[i][1]; split = 1; }
    if (!split) k[n++] = v;
    if (T) {
        int t = 0; for (int i = 0; i < 30; i++) if (HG_JONG[i] == T) { t = 0x3131 + i; break; }
        split = 0;
        for (int i = 0; i < 11; i++) if (HG_TT[i][2] == t) { k[n++] = HG_TT[i][0]; k[n++] = HG_TT[i][1]; split = 1; }
        if (!split) k[n++] = t;
    }
    return n;
}
static void type_cps(int l, const int *k, int n) { for (int i = 0; i < n; i++) kbi_char(&E, l, k[i]); }
static void test_hangul(void)
{
    const int K = LANG_KO;
    static const char *const S[][2] = {
        { "ㅎㅏㄴㄱㅡㄹ", "한글" }, { "ㄷㅏㄹㄱㅏ", "달가" }, { "ㅇㅗㅏ", "와" }, { "ㅇㅜㅔ", "웨" }, { "ㄱㅏㅂㅅ", "값" },
        { "ㄱㅏㅂㅅㅣ", "갑시" }, { "ㄲㅏ", "까" }, { "ㄱㄱ", "ㄱㄱ" }, { "ㄱㅏㄸ", "가ㄸ" }, { "ㅏ", "ㅏ" }, { "ㅏㅏ", "ㅏㅏ" },
        { "ㅇㅣㅆㅇㅓㅇㅛ", "있어요" }, { "ㅇㅡㅣㅅㅏ", "의사" }, { "ㄷㅏㄺ", "닭" }, { "ㅅㅏㄹㅁ", "삶" }, { "ㅇㅏㄴㄴㅕㅇ", "안녕" },
        { "ㄱㅏ ㄴㅏ", "가 나" }, { "ㅁㅜㅓ", "뭐" }, { "ㄱㅗㅐㄴ", "괜" }, { "ㅋㅋㅋ", "ㅋㅋㅋ" },
        { "ㄷㅏㄹㄱㅏ\b", "닭" }, { "ㄷㅏㄹㄱㅏ\b\b", "달" }, { "ㄷㅏㄹㄱㅏ\b\b\b", "다" }, { "ㄷㅏㄹㄱㅏ\b\b\b\b", "ㄷ" },
        { "ㄷㅏㄹㄱㅏ\b\b\b\b\b", "" }, { "ㅇㅗㅏ\b", "오" }, { "ㄱㅏㅂㅅ\b", "갑" }, { "ㅎㅏㄴ ㄱ\b\b", "한" }, { "ㅎㅏㄴ \bㄱㅡㄹ", "한글" },
    };
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) {
        clear(); type(K, S[i][0]);
        CHK(!strcmp(buf, S[i][1]), "hangul \"%s\" -> \"%s\", want \"%s\"", S[i][0], buf, S[i][1]);
        CHK(E.cur == E.len, "hangul \"%s\": cursor %d len %d", S[i][0], E.cur, E.len);
    }
    /* a space after a syllable commits it: backspace then deletes whole syllables */
    clear(); type(K, "ㅎㅏㄴ"); type(K, " "); type(K, "\b\b"); CHK(!strcmp(buf, ""), "hangul commit by space then 2 backspaces -> \"%s\"", buf);
    /* cursor moved by the caller (tap in the field): the syllable is committed, typing starts anew */
    clear(); type(K, "ㅎㅏ"); E.cur = 0; type(K, "ㄴ"); CHK(!strcmp(buf, "ㄴ하"), "hangul after external cursor move -> \"%s\"", buf);
    /* every syllable: compose, then backspace key by key down to empty */
    int bad = 0, nk = 0;
    for (int s = 0xAC00; s <= 0xD7A3; s++) {
        int k[8], n = ko_keys(s, k); char want[4]; put_utf8(want, s);
        clear(); type_cps(K, k, n); nk += n;
        if (strcmp(buf, want)) { if (bad++ < 5) printf("FAIL: syllable U+%04X typed as \"%s\"\n", s, buf); continue; }
        if (!has_glyph(s)) { if (bad++ < 5) printf("FAIL: syllable U+%04X has no glyph\n", s); }
        for (int i = n - 1; i >= 0; i--) {
            kbi_back(&E, K);
            if ((i == 0) != (E.len == 0)) { if (bad++ < 5) printf("FAIL: U+%04X backspace %d left \"%s\"\n", s, n - i, buf); break; }
        }
    }
    checks++; if (bad) fails++;
    printf("hangul: 11172 syllables (%d keys) composed and deleted key by key, %d errors\n", nk, bad);
    /* runs of three random syllables (the first ones get committed while typing) */
    srand(1); bad = 0;
    for (int it = 0; it < 30000; it++) {
        int s[3], k[24], n = 0; char want[16] = "";
        for (int j = 0; j < 3; j++) { s[j] = 0xAC00 + rand() % 11172; n += ko_keys(s[j], k + n); put_utf8(want + strlen(want), s[j]); }
        clear(); type_cps(K, k, n);
        if (strcmp(buf, want) && bad++ < 5) printf("FAIL: \"%s\" typed as \"%s\"\n", want, buf);
    }
    checks++; if (bad) fails++;
    printf("hangul: 30000 random runs of 3 syllables, %d errors\n", bad);
}

/* ---------------------------------------------------------------- romaji */
static void test_romaji(void)
{
    const int J = LANG_JA;
    static const char *const R[][2] = {
        { "kyouha", "きょうは" }, { "konnnichiha", "こんにちは" }, { "kitte", "きって" }, { "shinbun", "しんぶん" },
        { "xtu", "っ" }, { "ltsu", "っ" }, { "fa", "ふぁ" }, { "toukyou", "とうきょう" }, { "sakka", "さっか" }, { "matcha", "まっちゃ" },
        { "nihongo", "にほんご" }, { "zenbu", "ぜんぶ" }, { "kanji", "かんじ" }, { "konya", "こにゃ" }, { "konnya", "こんや" },
        { "chotto", "ちょっと" }, { "zutto", "ずっと" }, { "wo", "を" }, { "vu", "ゔ" }, { "dhi", "でぃ" }, { "thi", "てぃ" },
        { "jya", "じゃ" }, { "zya", "じゃ" }, { "ja", "じゃ" }, { "tsu", "つ" }, { "tu", "つ" }, { "si", "し" }, { "shi", "し" },
        { "ti", "ち" }, { "chi", "ち" }, { "hu", "ふ" }, { "fu", "ふ" }, { "hon", "ほん" }, { "xya", "ゃ" }, { "lyo", "ょ" },
        { "kya", "きゃ" }, { "gyu", "ぎゅ" }, { "byo", "びょ" }, { "pyu", "ぴゅ" }, { "myo", "みょ" }, { "ryo", "りょ" },
        { "nyu", "にゅ" }, { "hya", "ひゃ" }, { "cha", "ちゃ" }, { "sha", "しゃ" }, { "she", "しぇ" }, { "tsa", "つぁ" },
        { "watashi", "わたし" }, { "arigatou", "ありがとう" }, { "ongaku", "おんがく" }, { "kippu", "きっぷ" },
        { "gakkou", "がっこう" }, { "rajio", "らじお" }, { "shuu", "しゅう" }, { "denwa", "でんわ" }, { "kon'ya", "こんや" },
        { "ka-do", "かーど" }, { "qa", "くぁ" }, { "xwa", "ゎ" }, { "xka", "ゕ" }, { "wha", "うぁ" },
    };
    for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) {
        ime_reset();
        for (const char *p = R[i][0]; *p; p++) ime_letter(J, *p);
        char o[128]; ime_raw(J, o, sizeof o);
        CHK(!strcmp(o, R[i][1]), "romaji \"%s\" -> \"%s\", want \"%s\"", R[i][0], o, R[i][1]);
    }
    /* through the keyboard: preedit stays out of the text until space/enter/candidate */
    clear(); type(J, "konpyu"); CHK(E.len == 0, "ja preedit entered the text: \"%s\"", buf);
    type(J, "ーta"); kbi_char(&E, J, 0x30FC);
    ime_build(J); CHK(ime_nc >= 2 && !strcmp(ime_cand(0), "こんぴゅーたー") && !strcmp(ime_cand(1), "コンピューター"), "ja candidates \"%s\" \"%s\"", ime_cand(0), ime_cand(1));
    kbi_pick(&E, J, 1); CHK(!strcmp(buf, "コンピューター"), "ja pick katakana -> \"%s\"", buf);
    clear(); type(J, "nihon\n"); CHK(!strcmp(buf, "にほん"), "ja enter -> \"%s\"", buf);
    clear(); type(J, "kyou "); CHK(!strcmp(buf, "きょう"), "ja space -> \"%s\"", buf);
    clear(); type(J, "sushi。"); CHK(!strcmp(buf, "すし。"), "ja punctuation -> \"%s\"", buf);
    clear(); type(J, "kya\b"); ime_build(J); CHK(!strcmp(ime_cand(0), "き") && E.len == 0, "ja backspace -> \"%s\"", ime_cand(0));
    clear(); type(J, "aB"); CHK(!strcmp(buf, "あB"), "ja upper letter -> \"%s\"", buf);
}

/* ---------------------------------------------------------------- pinyin */
static void test_pinyin(void)
{
    const int Z = LANG_ZHS, T = LANG_ZHT;
    for (int i = 1; i < IME_PY_N; i++) CHK(strcmp(IME_PY_SYL[i - 1], IME_PY_SYL[i]) < 0, "syllables not sorted at %s", IME_PY_SYL[i]);
    for (int i = 1; i < IME_S2T_N; i++) CHK(IME_S2T[i - 1][0] < IME_S2T[i][0], "s2t not sorted at %d", i);
    int noglyph = 0, nogt = 0;
    for (int i = 0; i < IME_PY_N; i++) {
        int ok = 0;
        for (int j = IME_PY_OFF[i]; j < IME_PY_OFF[i + 1]; j++) { if (has_glyph(IME_PY_CH[j])) ok++; else noglyph++; if (!has_glyph(ime_s2t(IME_PY_CH[j]))) nogt++; }
        CHK(ok > 0, "syllable %s has no character with a glyph", IME_PY_SYL[i]);
    }
    printf("pinyin: %d syllables, %d entries, %d s2t pairs; without glyph: %d simplified, %d traditional (filtered at run time)\n",
           IME_PY_N, IME_PY_NCH, IME_S2T_N, noglyph, nogt);
    int tab = (int)(sizeof IME_PY_SYL + sizeof IME_PY_OFF + sizeof IME_PY_CH + sizeof IME_PY_RK + sizeof IME_S2T + sizeof IME_S2T_ALT);
    printf("pinyin tables: %.1f KB (plus the alternate strings)\n", tab / 1024.0);
    CHK(tab < 60 * 1024, "pinyin tables %d bytes over the 60 KB budget", tab);

    static const struct { const char *in; int cons; const char *first; } C[] = {
        { "zhongguo", 5, "中" }, { "guo", 3, "国" }, { "xian", 4, 0 }, { "xi'an", 2, 0 }, { "nihao", 2, "你" }, { "hao", 3, "好" },
        { "zh", 2, 0 }, { "x", 1, 0 }, { "n", 1, 0 }, { "women", 2, "我" }, { "men", 3, "们" }, { "shi", 3, "是" }, { "de", 2, "的" },
        { "lv", 2, 0 }, { "nve", 3, "虐" }, { "yinyue", 3, "因" }, { "yue", 3, 0 }, { "zhe", 3, "这" }, { "le", 2, "了" },
        { "lue", 3, "略" }, { "nue", 3, "虐" }, { "lve", 3, "略" },
    };
    for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) {
        ime_reset(); snprintf(ime_pre, sizeof ime_pre, "%s", C[i].in); ime_dirty(); ime_build(Z);
        CHK(ime_nc > 0, "pinyin \"%s\": no candidates", C[i].in);
        CHK(ime_cons == C[i].cons, "pinyin \"%s\": consumes %d, want %d", C[i].in, ime_cons, C[i].cons);
        if (C[i].first) CHK(!strcmp(ime_cand(0), C[i].first), "pinyin \"%s\": first \"%s\", want \"%s\"", C[i].in, ime_cand(0), C[i].first);
    }
    clear(); type(Z, "zhongguo"); CHK(E.len == 0, "zh preedit entered the text");
    type(Z, "  "); CHK(!strcmp(buf, "中国"), "zh two spaces -> \"%s\"", buf);
    clear(); type(Z, "celue  "); CHK(!strcmp(buf, "策略"), "zh celue -> \"%s\"", buf);
    clear(); type(T, "zhongguo  "); CHK(!strcmp(buf, "中國"), "zh-Hant -> \"%s\"", buf);
    clear(); type(T, "women  "); CHK(!strcmp(buf, "我們"), "zh-Hant women -> \"%s\"", buf);
    clear(); type(T, "zhe "); CHK(!strcmp(buf, "這"), "zh-Hant zhe -> \"%s\"", buf);
    clear(); type(Z, "nihao，"); CHK(!strcmp(buf, "你好，"), "zh punctuation converts -> \"%s\"", buf);
    clear(); type(Z, "nihao\n"); CHK(!strcmp(buf, "nihao"), "zh enter keeps letters -> \"%s\"", buf);
    clear(); type(Z, "ni\bi"); ime_build(Z); CHK(!strcmp(ime_cand(0), "你") || ime_nc > 0, "zh backspace in preedit");
    ime_reset(); snprintf(ime_pre, sizeof ime_pre, "fa"); ime_dirty(); ime_build(T);
    int fa = 0, fa2 = 0; for (int i = 0; i < ime_nc; i++) { if (!strcmp(ime_cand(i), "發")) fa = 1; if (!strcmp(ime_cand(i), "髮")) fa2 = 1; }
    CHK(fa && fa2, "zh-Hant fa: 發 and 髮 among the candidates");
    CHK(ime_s2t(0x4EEC) == 0x5011 && ime_s2t(0x8FD9) == 0x9019 && ime_s2t(0x56FD) == 0x570B, "s2t 们这国");
    ime_reset();
}

/* ---------------------------------------------------------------- Thai, edit engine */
static void test_thai(void)
{
    CHK(text_width("ที่", 20) == text_width("ท", 20), "Thai marks advance: %d vs %d", text_width("ที่", 20), text_width("ท", 20));
    clear(); eb_insert(&E, "ที่นี่"); E.cur = E.len;
    eb_move(&E, -1); CHK(E.cur == 9, "eb_move over Thai marks: cur %d, want 9", E.cur);
    eb_move(&E, -1); CHK(E.cur == 0, "eb_move over Thai marks: cur %d, want 0", E.cur);
    eb_move(&E, 1); CHK(E.cur == 9, "eb_move forward over marks: cur %d, want 9", E.cur);
    clear(); type(LANG_TH, "ท"); type(LANG_TH, "ี"); type(LANG_TH, "่"); CHK(!strcmp(buf, "ที่"), "Thai typing -> \"%s\"", buf);
    type(LANG_TH, "\b"); CHK(!strcmp(buf, "ที"), "Thai backspace removes one mark -> \"%s\"", buf);
    /* Latin and Cyrillic: plain insertion */
    clear(); type(LANG_DE, "ßü"); type(LANG_RU, "ё"); CHK(!strcmp(buf, "ßüё"), "plain typing -> \"%s\"", buf);
}

int main(void)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout; (void)kb_cfg_layouts;
    if (!text_init()) { printf("cannot load default.otf (TEXT_FONT_DIRS)\n"); return 2; }
    test_layouts();
    test_upcase();
    test_hangul();
    test_romaji();
    test_pinyin();
    test_thai();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
