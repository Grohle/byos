/* kb_render.c - the on-screen keyboard (src/keyboard.inc.c + src/kb_ime.inc.c) on the PC, as PNG, with the
 * firmware fonts: every layout of the 13 languages (letters, shift, 123, #+=) at three key heights,
 * long-press popups, Hangul being composed, Thai marks, the candidate bar (Chinese and Japanese), the
 * globe menu and the keyboard sheet with many keyboards, and the Latin-only menu. Touch input is
 * scripted on a virtual clock and drives the real kb_touch(), so the same run also checks behaviour
 * (typing, long-press, globe tap/hold, candidates).
 *   bash tests/kb_render.sh  ->  out/kb_render/ (or $KB_OUT) */
#define _GNU_SOURCE
#include <ctype.h>
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
#include <linux/input.h>
#define W 320
#define H 480
#define ASSETS "/tmp/kb_render_assets"
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
#include "hm_shared.h"
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
static void lg(const char *f, ...) { (void)f; }
/* ---------------------------------------------------------------- virtual clock and scripted touch */
static double vt = 1000;
static double now_ms(void) { return vt; }
struct pkt { struct input_event ev[24]; int n; int x, y, down; };
static int cur_x, cur_y, cur_down;
struct tev { double t; int x, y, down; };
static struct tev Q[256]; static int qn, qi;
static void q_add(double t, int x, int y, int down) { if (qn < 256) Q[qn++] = (struct tev){ t, x, y, down }; }
static int read_pkt(struct pkt *p, int wait)
{
    if (qi >= qn) {                               /* script over: lift the finger */
        if (cur_down) { cur_down = 0; p->n = 0; p->x = cur_x; p->y = cur_y; p->down = 0; return 1; }
        vt += wait; return 0;
    }
    if (Q[qi].t > vt + wait) { vt += wait; return 0; }
    if (Q[qi].t > vt) vt = Q[qi].t;
    cur_x = Q[qi].x; cur_y = Q[qi].y; cur_down = Q[qi].down; qi++;
    p->n = 0; p->x = cur_x; p->y = cur_y; p->down = cur_down;
    return 1;
}
static uint16_t fb0[W * H], fb1[W * H], *fr0 = fb0, *fr1 = fb1;
static int (*snap_when)(void); static const char *snap_name;
static void save(const uint16_t *src, const char *name);
static void present(void)
{
    vt += 16;
    if (snap_when && snap_when()) { save(fr0, snap_name); snap_when = 0; }
}
static void present_rows(int y0, int y1) { (void)y0; (void)y1; present(); }
static int lconf_saves;
static void lconf_save(void) { lconf_saves++; }
#include "i18n.inc.c"
#include "text.inc.c"
#include "kb_render_extract.inc"         /* fill_rrect, fill_circle (from the shell, as is) */
#include "kb_ime.inc.c"
#include "keyboard.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static const char *outdir(void) { return getenv("KB_OUT") ? getenv("KB_OUT") : "out/kb_render"; }
static void save(const uint16_t *src, const char *name)
{
    static unsigned char px[W * H * 3];
    for (int i = 0; i < W * H; i++) { uint16_t p = src[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, W, H, 3, px, W * 3);
}
/* contact sheet: n frames side by side (rows of up to 4) */
static uint16_t *sheet_fr[16]; static int sheet_n;
static void sheet_add(void) { if (sheet_n < 16) { sheet_fr[sheet_n] = malloc(W * H * 2); memcpy(sheet_fr[sheet_n++], fr0, W * H * 2); } }
static void sheet_save(const char *name)
{
    int cols = sheet_n < 4 ? sheet_n : 4, rows = (sheet_n + 3) / 4, gw = cols * W + (cols + 1) * 8, gh = rows * H + (rows + 1) * 8;
    unsigned char *px = calloc((size_t)gw * gh * 3, 1);
    for (int i = 0; i < gw * gh * 3; i++) px[i] = 60;
    for (int f = 0; f < sheet_n; f++) {
        int ox = 8 + (f % 4) * (W + 8), oy = 8 + (f / 4) * (H + 8);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint16_t p = sheet_fr[f][y * W + x]; unsigned char *o = px + ((size_t)(oy + y) * gw + ox + x) * 3;
            o[0] = (unsigned char)((p >> 11) * 255 / 31); o[1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); o[2] = (unsigned char)((p & 31) * 255 / 31);
        }
        free(sheet_fr[f]);
    }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, gw, gh, 3, px, gw * 3);
    free(px); sheet_n = 0;
}

static int fails, checks;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- page and keyboard session */
static char buf[256];
static struct ebuf E;
static void status_bar(void)
{
    for (int i = 0; i < 30 * W; i++) fr1[i] = 0;
    draw_text(fr1, 20, 21, "9:41", 15, 0xFFFF, 0, 0, H); draw_text(fr1, 270, 21, "87 %", 13, 0xFFFF, 0, 0, H);
}
/* a fresh editor session on layout l with the text t already typed (through the engine) */
static void session(int l, const char *title, const char *t)
{
    buf[0] = 0; E = (struct ebuf){ buf, 0, sizeof buf, 0 };
    kb_cfg_layout = l; kb_layer = 0; kb_shift = 0; kb_caps = 0; qn = qi = 0; cur_down = 0;
    le_title = title; le_hint = 0; le_buf = &E; le_caret = 1;
    kb_layout(tr("Aceptar"));
    kb_begin_ex(&E, fr0, le_redraw, le_fy, le_fy + 44);
    for (const char *s = t; *s; ) { int c = utf8_next(&s); kbi_char(&E, l, c); }
    kb_autoshift(&E);
}
static void frame(void) { le_redraw(fr0, -1); }
static int key_of(int code) { kb_sh_sync(); for (int i = 0; i < nkeys; i++) if (kbk[i].code == code) return i; return -1; }
static int kx(int k) { return kbk[k].x + kbk[k].w / 2; }
static int ky(int k) { return kb_keys_y0() + kbk[k].y + kbk[k].h / 2; }
/* scripted touch: down at (x, y), optional move to (x2, y2) at t_move, up after hold ms */
static int touch(int x, int y, int hold, int x2, int y2, int t_move)
{
    qn = qi = 0;
    q_add(vt + 1, x, y, 1);
    if (t_move >= 0) for (int i = 1; i <= 6; i++) q_add(vt + 1 + t_move + i * 20, x + (x2 - x) * i / 6, y + (y2 - y) * i / 6, 1);
    q_add(vt + 1 + hold, t_move >= 0 ? x2 : x, t_move >= 0 ? y2 : y, 0);
    struct pkt p; read_pkt(&p, 100);
    return kb_touch(&p, &E, fr0, le_redraw, 0);
}
static void tap_code(int code) { int k = key_of(code); if (k < 0) { CHK(0, "no key U+%04X", code); return; } touch(kx(k), ky(k), 60, 0, 0, -1); }
static void tap_str(const char *s) { int c; while ((c = utf8_next(&s))) tap_code(c); }

/* ---------------------------------------------------------------- frames */
static const char *SAMPLE[KB_NLAY] = {
    [LANG_ES] = "Canción", [LANG_EN] = "Song", [LANG_FR] = "Chanson", [LANG_DE] = "Grüße", [LANG_IT] = "Città",
    [LANG_JA] = "", [LANG_KO] = "ㅎㅏㄴㄱㅡㄹ", [LANG_PL] = "Piosenka żółć", [LANG_RU] = "Песня", [LANG_ZHS] = "",
    [LANG_TH] = "เพลง", [LANG_ZHT] = "", [LANG_UK] = "Пісня ґ",
};
static void layouts(void)
{
    kb_cfg_layouts = (1 << LANG_N) - 1;
    for (int i = 0; i < KB_NLAY; i++) {
        int l = KB_ORD[i]; const struct kb_lay *ly = kb_lay(l);
        for (int hh = 0; hh < 3; hh++) {
            kb_cfg_rowh = hh == 0 ? 36 : hh == 1 ? 42 : 48; kb_rowh_coarse = kb_cfg_height = hh;
            session(l, ly->name, SAMPLE[l]);
            if (ly->flags & KL_PINYIN) { for (const char *s = "nihao"; *s; s++) kbi_char(&E, l, *s); }
            if (ly->flags & KL_KANA) { for (const char *s = "nihongo"; *s; s++) kbi_char(&E, l, *s); }
            char nm[96];
            kb_shift = 0; kb_caps = 0; kb_autoshift(&E); frame(); snprintf(nm, sizeof nm, "%s_letters_h%d", ly->code, kb_cfg_rowh); save(fr0, nm);
            if (hh != 1) continue;
            sheet_add();
            kb_shift = 1; frame(); snprintf(nm, sizeof nm, "%s_shift", ly->code); save(fr0, nm); sheet_add(); kb_shift = 0;
            kb_layer = 1; kb_relayout(); frame(); snprintf(nm, sizeof nm, "%s_123", ly->code); save(fr0, nm); sheet_add();
            kb_layer = 2; kb_relayout(); frame(); snprintf(nm, sizeof nm, "%s_symbols", ly->code); save(fr0, nm); sheet_add();
            kb_layer = 0; kb_relayout();
            snprintf(nm, sizeof nm, "sheet_%02d_%s", i + 1, ly->code); sheet_save(nm);
            /* geometry: keys inside the screen, no overlaps in a row */
            for (int a = 0; a < nkeys; a++) {
                CHK(kbk[a].x >= 0 && kbk[a].x + kbk[a].w <= W, "[%s] key %d outside", ly->code, a);
                for (int b = a + 1; b < nkeys; b++) if (kbk[a].y == kbk[b].y)
                    CHK(kbk[a].x + kbk[a].w <= kbk[b].x || kbk[b].x + kbk[b].w <= kbk[a].x, "[%s] keys %d and %d overlap", ly->code, a, b);
            }
            kb_end();
        }
    }
    kb_cfg_rowh = 42; kb_cfg_height = kb_rowh_coarse = 1;
}
static void popup(int l, int c, int sel, const char *name)
{
    session(l, kb_lay(l)->name, "");
    kb_shift = 0; kb_shift_auto = 0;
    int k = key_of(c); if (k < 0) { CHK(0, "%s: no key U+%04X", name, c); return; }
    if (sel >= 0) { CHK(kb_alt_open(k), "%s: no variants", name); kb_alt_sel = sel; }
    kb_pressed = k; le_redraw(fr0, k); save(fr0, name); sheet_add();
    kb_pressed = -1; kb_alt_n = 0; kb_end();
}
static void popups(void)
{
    popup(LANG_DE, 'a', 0, "popup_de_a"); popup(LANG_PL, 'z', 0, "popup_pl_z"); popup(LANG_RU, 0x435, 0, "popup_ru_e");
    popup(LANG_UK, 0x433, 0, "popup_uk_g"); popup(LANG_IT, 'e', 1, "popup_it_e"); popup(LANG_ES, 'n', 0, "popup_es_n");
    popup(LANG_TH, 0xE38, -1, "popup_th_mark"); popup(LANG_TH, 0xE02, 0, "popup_th_kho"); popup(LANG_KO, 0x3131, -1, "popup_ko");
    sheet_save("sheet_popups");
}
/* ---------------------------------------------------------------- behaviour through kb_touch */
static int alt_open_now(void) { return kb_alt_n > 0; }
static int menu_open_now(void) { return kb_menu > 0; }
static void behaviour(void)
{
    kb_cfg_layouts = 1 << LANG_ES | 1 << LANG_EN | 1 << LANG_DE;
    /* German: autocap on ü, long-press a -> ä */
    session(LANG_DE, "Deutsch", ""); tap_code(0xFC); CHK(!strcmp(buf, "Ü"), "de: first letter \"%s\"", buf);
    int k = key_of('a'); snap_when = alt_open_now; snap_name = "touch_de_longpress_a";
    touch(kx(k), ky(k), 700, kx(k) + 2, ky(k) - 40, 500);
    CHK(!strcmp(buf, "Üä"), "de: long-press a -> \"%s\"", buf);
    kb_end();
    /* Russian: shift + letters, Cyrillic upper case, double space period */
    session(LANG_RU, "Русский", ""); tap_str("при"); tap_code(K_SPACE); tap_code(K_SPACE);
    CHK(!strcmp(buf, "При. "), "ru: \"%s\"", buf); kb_end();
    /* Korean through the keys: ㅎㅏㄴ + shift ㄲ ㅏ */
    session(LANG_KO, "한국어", ""); tap_str("ㅎㅏㄴ"); tap_code(K_SHIFT); tap_code(0x3132); tap_code(0x314F);
    CHK(!strcmp(buf, "한까"), "ko: \"%s\"", buf);
    tap_code(K_BACK); CHK(!strcmp(buf, "한ㄲ"), "ko backspace: \"%s\"", buf);
    frame(); save(fr0, "ko_composing"); kb_end();
    /* Thai: shift layer gives Thai digits; marks */
    session(LANG_TH, "ไทย", ""); tap_str("ท"); tap_code(0xE35); tap_code(0xE48); tap_code(K_SHIFT); tap_code(0xE51);
    CHK(!strcmp(buf, "ที่๑"), "th: \"%s\"", buf); kb_end();
    /* Chinese: preedit stays out of the text; candidate tap; space */
    session(LANG_ZHS, "简体拼音", ""); tap_str("nihao");
    CHK(E.len == 0 && !strcmp(ime_pre, "nihao"), "zh: preedit \"%s\" text \"%s\"", ime_pre, buf);
    frame(); save(fr0, "zh_hans_nihao");
    kb_cand_geom(); touch(kb_cand_x0[0] + 10, kb_cb_y() + 30, 60, 0, 0, -1);
    CHK(!strcmp(buf, "你"), "zh: candidate tap -> \"%s\"", buf);
    tap_code(K_SPACE); CHK(!strcmp(buf, "你好"), "zh: space -> \"%s\"", buf);
    tap_str("zhongguo"); frame(); save(fr0, "zh_hans_zhongguo"); kb_end();
    session(LANG_ZHT, "繁體拼音", "我們"); tap_str("zhongguo"); frame(); save(fr0, "zh_hant_zhongguo");
    tap_code(K_SPACE); tap_code(K_SPACE); CHK(!strcmp(buf, "我們中國"), "zh-Hant: \"%s\"", buf); kb_end();
    session(LANG_JA, "日本語", ""); tap_str("kyou"); frame(); save(fr0, "ja_kyou");
    tap_code(K_ENTER); CHK(!strcmp(buf, "きょう"), "ja: enter -> \"%s\"", buf);
    tap_str("ra"); kb_layer = 1; kb_relayout(); tap_code(0x30FC); kb_layer = 0; kb_relayout(); tap_str("men");
    frame(); save(fr0, "ja_ramen"); kb_end();
    /* Thai field with stacked marks */
    session(LANG_TH, "ไทย", "ที่นี่ น้ำ สวัสดี"); frame(); save(fr0, "th_marks_field"); kb_end();

    /* globe: tap -> next keyboard; hold -> menu */
    kb_cfg_layouts = 1 << LANG_ES | 1 << LANG_EN;
    session(LANG_ES, "Español", ""); int by = kb_bar_y() + 14;
    touch(25, by, 80, 0, 0, -1); CHK(kb_cfg_layout == LANG_EN, "globe tap: layout %d", kb_cfg_layout);
    touch(25, by, 80, 0, 0, -1); CHK(kb_cfg_layout == LANG_ES, "globe tap back: layout %d", kb_cfg_layout);
    kb_end();
    kb_cfg_layouts = (1 << LANG_N) - 1;
    session(LANG_ES, "Español", ""); by = kb_bar_y() + 14;
    snap_when = menu_open_now; snap_name = "globe_hold_menu_13";
    touch(25, by, 500, 25, by, -1);                /* hold, release on the globe: menu stays */
    CHK(kb_menu == 1, "globe hold: menu %d", kb_menu);
    frame(); save(fr0, "globe_menu_13"); sheet_add();
    int mx, my, mw, ih, vis, fh; kb_menu_geom(&mx, &my, &mw, &ih, &vis, &fh);
    touch(mx + 60, my + ih * 2 + 10, 300, mx + 60, my + 4, 50);   /* drag the list */
    frame(); save(fr0, "globe_menu_13_scrolled"); sheet_add();
    int it = kb_menu_item(mx + 60, my + ih + 10), want = it > 0 ? kb_lst[it - 1] : -1;
    touch(mx + 60, my + ih + 10, 60, 0, 0, -1);
    CHK(kb_menu == 0 && kb_cfg_layout == want, "menu pick: layout %d want %d", kb_cfg_layout, want);
    kb_menu_pick(0); frame(); save(fr0, "sheet_chips_13"); sheet_add();
    kb_sheet = 0; kb_sheet_h = 0; kb_end();
    kb_cfg_layouts = 1 << LANG_ES | 1 << LANG_EN | 1 << LANG_FR;
    session(LANG_FR, "Français", ""); kb_menu_pick(0); frame(); save(fr0, "sheet_segmented_3"); sheet_add();
    kb_sheet = 0; kb_sheet_h = 0;
    kb_menu_open(); frame(); save(fr0, "globe_menu_3"); kb_menu = 0; kb_end();
    /* Latin-only field (Wi-Fi password): non-Latin keyboards greyed, current one swapped */
    kb_cfg_layouts = 1 << LANG_KO | 1 << LANG_EN | 1 << LANG_ZHS | 1 << LANG_DE;
    kb_latin_only = 1; session(LANG_KO, "Wi-Fi", "");
    CHK(kb_cur_lay() == LANG_EN, "latin-only: current %d", kb_cur_lay());
    CHK(kb_cfg_layout == LANG_KO, "latin-only: saved layout changed to %d", kb_cfg_layout);
    kb_menu_open(); frame(); save(fr0, "latin_only_menu"); sheet_add(); kb_menu = 0;
    kb_end(); CHK(!kb_latin_only && kb_lay_tmp < 0, "latin-only reset by kb_end");
    sheet_save("sheet_menus");
}

int main(void)
{
    (void)ui_lang_sys;
    if (!text_init()) { printf("cannot load default.otf (TEXT_FONT_DIRS)\n"); return 2; }
    mkdir(ASSETS, 0755);
    ui_lang = LANG_EN; status_bar();
    layouts();
    popups();
    behaviour();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
