/* radio_render.c - frames of the Radio app (src/radio_ui.inc.c) on the PC, as PNG, to review the
 * design without the device: Explore (top, genres), Favourites (favourites, recents, own stations),
 * a directory list with the now-playing card in its states (connecting, buffering, playing with a
 * title, error), search results, the country list, loading and error rows, in es/en/fr/de/ja/ru/th.
 * The state is set by hand here (what the network thread and the helper watcher would publish);
 * touch input, keyboard and the rest of the shell are stubs.
 *   bash tests/radio_render.sh  ->  out/radio_render/ (or $RADIO_OUT) */
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
static double fake_ms = 1000;                          /* frozen clock: spinners and bars in a fixed phase */
static double now_ms(void) { return fake_ms; }
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static inline uint16_t dim565(uint16_t p, int k) { unsigned r = (p >> 11) * k >> 5, g = ((p >> 5) & 63) * k >> 5, b = (p & 31) * k >> 5; return (uint16_t)(r << 11 | g << 5 | b); }
static inline uint16_t mix565(uint16_t c, uint16_t d, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
static uint16_t fb0[W * H], fb1[W * H], *fr0 = fb0, *fr1 = fb1;
static void draw_toast(uint16_t *d) { (void)d; }
static void kb_draw(uint16_t *d, int pressed) { (void)d; (void)pressed; }
#include "i18n.inc.c"
#include "text.inc.c"
#include "radio_render_extract.inc"      /* fill_rrect, fill_circle, draw_wrapped, spinner, fm_* drawing (from the shell, as is) */
#include "freemusic_core.inc.c"
#include "radio_stream.inc.c"
#include "radio_core.inc.c"

/* ---- what radio.inc.c declares for the drawing code (same names and types) */
enum { RY_IDLE, RY_CONNECTING, RY_BUFFERING, RY_PLAYING, RY_ERROR };
#define RA_MAXST 240
static struct ra_lib *ra_lb; static struct ra_conf ra_cf;
struct ra_list {
    int kind; char arg[100], title[100], base[200]; unsigned sid;
    struct ra_station *it; int n, state, more, offset, err;
};
static struct ra_list ra_ls[2];
static struct ra_srcs ra_sr;
static struct ra_country *ra_ct; static int ra_nct, ra_ct_state;
static struct { int state, pct, br, have, ver; char err[24], title[256], codec[8]; struct ra_station cur; } ra_np;
static int ra_active(void) { return ra_np.have && (ra_np.state == RY_CONNECTING || ra_np.state == RY_BUFFERING || ra_np.state == RY_PLAYING); }
static int ra_is_cur(const struct ra_station *s) { return ra_np.have && ra_same(&ra_np.cur, s); }
#include "radio_ui.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static const char *outdir(void) { return getenv("RADIO_OUT") ? getenv("RADIO_OUT") : "out/radio_render"; }
static int lfails, frames;
static void save(const char *name)
{
    static unsigned char px[W * H * 3];
    for (int i = 0; i < W * H; i++) { uint16_t p = fr0[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, W, H, 3, px, W * 3);
    frames++;
    /* touch zones: inside the screen (below the status bar); rows of a list may touch, never overlap */
    for (int i = 0; i < fm_nhit; i++) {
        const struct fm_hit *a = &fm_hits[i];
        if (a->x0 < 0 || a->x1 > W || a->y0 < 30 || a->y1 > H || a->x0 >= a->x1 || a->y0 >= a->y1) { lfails++; printf("LAYOUT %s: zone %d kind %d (%d,%d)-(%d,%d) outside\n", name, i, a->kind, a->x0, a->y0, a->x1, a->y1); }
        for (int j = i + 1; j < fm_nhit; j++) {
            const struct fm_hit *b = &fm_hits[j];
            int ov = a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
            /* later zones win (fm_hit_at searches backwards): the card over the list, the header over rows */
            int layered = b->kind == RH_MINI || b->kind == RH_MSTAR || b->kind == RH_MPLAY || a->kind == RH_MINI ||
                          b->kind == FH_BAR || b->kind == FH_CLEAR || b->kind == FH_CANCEL || b->kind == RH_TAB || b->kind == FH_BACK;
            if (ov && !layered) { lfails++; printf("LAYOUT %s: zones %d (kind %d) and %d (kind %d) overlap\n", name, i, a->kind, j, b->kind); }
        }
    }
}
static void status_bar(void) { for (int i = 0; i < 30 * W; i++) fr1[i] = 0; draw_text(fr1, 20, 21, "9:41", 15, 0xFFFF, 0, 0, H); draw_text(fr1, 270, 21, "87 %", 13, 0xFFFF, 0, 0, H); }

static struct ra_station st(const char *uuid, const char *name, const char *url, const char *cc, const char *codec, int br, const char *tags)
{
    struct ra_station s; memset(&s, 0, sizeof s);
    snprintf(s.uuid, sizeof s.uuid, "%s", uuid); snprintf(s.name, sizeof s.name, "%s", name); snprintf(s.url, sizeof s.url, "%s", url);
    snprintf(s.cc, sizeof s.cc, "%s", cc); snprintf(s.codec, sizeof s.codec, "%s", codec); s.br = (short)br; snprintf(s.tags, sizeof s.tags, "%s", tags);
    s.kind = (unsigned char)ra_codec_kind(codec, 0);
    return s;
}
static void library(void)
{
    memset(ra_lb, 0, sizeof *ra_lb);
    ra_lb->fav[ra_lb->nfav++] = st("u1", "Radio Clásica", "https://rne.example.com/clasica.mp3", "ES", "MP3", 128, "classical, música clásica");
    ra_lb->fav[ra_lb->nfav++] = st("u2", "FIP", "https://fip.example.com/fip-hifi.aac", "FR", "AAC", 192, "jazz, eclectic");
    ra_lb->fav[ra_lb->nfav++] = st("u3", "SomaFM Groove Salad", "https://ice.example.com/groovesalad-128-mp3", "US", "MP3", 128, "ambient, chillout, downtempo");
    ra_lb->rec[ra_lb->nrec++] = ra_lb->fav[1];
    ra_lb->rec[ra_lb->nrec++] = st("u4", "BBC World Service", "http://bbc.example.com/ws.mp3", "GB", "MP3", 56, "news, talk");
    ra_own_add(ra_lb, "Radio del barrio", "http://192.168.1.20:8000/live.mp3");
}
static void list(int slot, const char *title, int nmore)
{
    struct ra_list *l = &ra_ls[slot];
    if (!l->it) l->it = calloc(RA_MAXST, sizeof *l->it);
    snprintf(l->title, sizeof l->title, "%s", title);
    l->n = 0; l->state = 2; l->more = 0;
    static const char *const N[] = { "Los 40", "Cadena SER", "Radio Nacional de España — Radio 1", "Kiss FM", "Europa FM", "Rock FM", "COPE", "Cadena Dial", "Radio 3", "Onda Cero", "Flaix FM", "Melodía FM" };
    static const char *const T[] = { "pop, top 40, hits", "news, talk", "news, public radio", "pop, 80s, 90s", "pop, dance", "rock, classic rock", "news, sports", "spanish pop", "indie, alternative", "news, talk", "dance, electronic", "oldies" };
    for (int i = 0; i < 12 && i < nmore; i++) {
        char u[64], url[96]; snprintf(u, sizeof u, "d-%d", i); snprintf(url, sizeof url, "https://s%d.example.com/live.%s", i, i % 3 == 2 ? "aac" : "mp3");
        l->it[l->n++] = st(u, N[i], url, "ES", i % 3 == 2 ? "AAC" : i == 7 ? "" : "MP3", i % 3 == 2 ? 64 : i == 7 ? 0 : 128, T[i]);
    }
}
static void play(const struct ra_station *s, int state, int pct, const char *title, const char *err)
{
    ra_np.have = 1; ra_np.cur = *s; ra_np.state = state; ra_np.pct = pct;
    snprintf(ra_np.title, sizeof ra_np.title, "%s", title ? title : ""); snprintf(ra_np.err, sizeof ra_np.err, "%s", err ? err : "");
}
static void frame(int pg, const char *name)
{
    ra_sp = pg == RG_ROOT ? 0 : 1; ra_stack[0] = RG_ROOT; ra_stack[1] = pg;
    ra_render(); save(name);
}
/* a list page opened from the sources page (back label "Sources") */
static void frame_from_srcs(const char *name)
{
    ra_sp = 2; ra_stack[0] = RG_ROOT; ra_stack[1] = RG_SRCS; ra_stack[2] = RG_LIST;
    ra_render(); save(name);
}
static void sources(void)
{
    char e[] = ""; ra_srcs_parse(&ra_sr, e);
    struct ra_src *s = ra_src_add(&ra_sr, RSRC_M3U, "https://lists.example.org/spain.m3u", "");
    s->n = 42; s->fetched = 1700000000;
    s = ra_src_add(&ra_sr, RSRC_JSON, "https://example.com/my-stations.json", "Mis emisoras de jazz");
    s->n = 1; s->fetched = 1700000000;
    s = ra_src_add(&ra_sr, RSRC_PLS, "http://old.example.net/radio.pls", ""); s->err = RSE_NET; s->on = 0;
    s = ra_src_add(&ra_sr, RSRC_RBX, "https://radio.example.org", ""); s->fetched = 1700000000;
    s = ra_src_add(&ra_sr, RSRC_URL, "https://new.example.com/list", ""); s->busy = 1;
}
static void eu_list(void)
{
    struct ra_list *l = &ra_ls[1];
    if (!l->it) l->it = calloc(RA_MAXST, sizeof *l->it);
    l->n = 0; l->state = 2; l->more = 0; l->err = 0; l->sid = ra_sr.s[1].sid;
    snprintf(l->title, sizeof l->title, "%s", tr("Radio pública europea"));
    for (int k = 0; k < RA_NEU; k++) ra_eu_station(k, &l->it[l->n++]);
}

int main(void)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout; (void)kb_cfg_layouts;
    if (!text_init()) { printf("no font (TEXT_FONT_DIRS)\n"); return 2; }
    ra_lb = calloc(1, sizeof *ra_lb);
    { char e[] = ""; ra_srcs_parse(&ra_sr, e); }
    status_bar();
    static const int langs[] = { LANG_ES, LANG_EN, LANG_FR, LANG_DE, LANG_JA, LANG_RU, LANG_TH };
    static const char *const lc[] = { "es", "en", "fr", "de", "ja", "ru", "th" };
    ra_ct = calloc(8, sizeof *ra_ct);
    const char *cn[8][2] = { { "US", "The United States Of America" }, { "DE", "Germany" }, { "ES", "Spain" }, { "FR", "France" }, { "GB", "The United Kingdom Of Great Britain And Northern Ireland" }, { "IT", "Italy" }, { "RU", "The Russian Federation" }, { "JP", "Japan" } };
    for (int i = 0; i < 8; i++) { snprintf(ra_ct[i].cc, 4, "%s", cn[i][0]); snprintf(ra_ct[i].name, 64, "%s", cn[i][1]); ra_ct[i].n = 6000 - i * 700; }
    for (int li = 0; li < (int)(sizeof langs / sizeof langs[0]); li++) {
        ui_lang = langs[li]; char n[64];
        memset(&ra_np, 0, sizeof ra_np);
        ra_conf_default(&ra_cf); ra_lang_country(LANG_CODES[ui_lang], ra_cf.cc, ra_cf.cname, sizeof ra_cf.cname);
        memset(ra_pos, 0, sizeof ra_pos); ra_edit_e = 0; ra_editing = 0; ra_q[0] = 0; ra_tab_t0 = -1e9;
        /* first run: nothing saved, Explore */
        memset(ra_lb, 0, sizeof *ra_lb); ra_tab = 0;
        snprintf(n, sizeof n, "%s_01_favs_empty", lc[li]); frame(RG_ROOT, n);
        ra_tab = 1;
        snprintf(n, sizeof n, "%s_02_explore", lc[li]); frame(RG_ROOT, n);
        ra_pos[RG_ROOT] = 420; snprintf(n, sizeof n, "%s_03_explore_genres", lc[li]); frame(RG_ROOT, n);
        ra_pos[RG_ROOT] = 9999; ra_render(); ra_pos[RG_ROOT] = ra_maxpos(RG_ROOT); snprintf(n, sizeof n, "%s_04_explore_end", lc[li]); frame(RG_ROOT, n);
        /* favourites with something playing */
        library(); ra_tab = 0; ra_pos[RG_ROOT] = 0;
        play(&ra_lb->fav[1], RY_PLAYING, 100, "Miles Davis - So What", "");
        snprintf(n, sizeof n, "%s_05_favs_playing", lc[li]); frame(RG_ROOT, n);
        ra_pos[RG_ROOT] = 9999; ra_render(); ra_pos[RG_ROOT] = ra_maxpos(RG_ROOT); snprintf(n, sizeof n, "%s_06_favs_end", lc[li]); frame(RG_ROOT, n);
        /* a list: connecting, buffering, error, loading more */
        list(1, tr("Más escuchadas"), 12); ra_pos[RG_LIST] = 0;
        play(&ra_ls[1].it[1], RY_CONNECTING, 0, "", ""); snprintf(n, sizeof n, "%s_07_list_connecting", lc[li]); frame(RG_LIST, n);
        play(&ra_ls[1].it[1], RY_BUFFERING, 63, "", ""); snprintf(n, sizeof n, "%s_08_list_buffering", lc[li]); frame(RG_LIST, n);
        play(&ra_ls[1].it[2], RY_ERROR, 0, "", "hls"); ra_pos[RG_LIST] = 60; snprintf(n, sizeof n, "%s_09_list_error_scrolled", lc[li]); frame(RG_LIST, n);
        ra_ls[1].state = 1; ra_pos[RG_LIST] = 9999; ra_render(); ra_pos[RG_LIST] = ra_maxpos(RG_LIST); snprintf(n, sizeof n, "%s_10_list_loading_more", lc[li]); frame(RG_LIST, n);
        ra_ls[1].n = 0; ra_ls[1].state = 3; ra_pos[RG_LIST] = 0; memset(&ra_np, 0, sizeof ra_np); snprintf(n, sizeof n, "%s_11_list_failed", lc[li]); frame(RG_LIST, n);
        ra_ls[1].state = 2; snprintf(n, sizeof n, "%s_12_list_empty", lc[li]); frame(RG_LIST, n);
        /* search */
        list(0, "", 6); ra_edit_e = 1; snprintf(ra_q, sizeof ra_q, "rock"); ra_pos[RG_ROOT] = 0;
        play(&ra_ls[0].it[0], RY_IDLE, 0, "", ""); snprintf(n, sizeof n, "%s_13_search_results", lc[li]); frame(RG_ROOT, n);
        ra_q[0] = 0; ra_ls[0].n = 0; ra_ls[0].state = 0; memset(&ra_np, 0, sizeof ra_np); snprintf(n, sizeof n, "%s_14_search_empty", lc[li]); frame(RG_ROOT, n);
        ra_edit_e = 0;
        /* countries */
        ra_nct = 8; ra_ct_state = 2; ra_pos[RG_CTRY] = 0; snprintf(ra_cf.cc, sizeof ra_cf.cc, "ES");
        snprintf(n, sizeof n, "%s_15_countries", lc[li]); frame(RG_CTRY, n);
        ra_nct = 0; ra_ct_state = 1; snprintf(n, sizeof n, "%s_16_countries_loading", lc[li]); frame(RG_CTRY, n);
        ra_ct_state = 2; ra_nct = 8;
        /* sources: Explore with them, the sources page, the European list, a list that failed */
        sources(); memset(&ra_np, 0, sizeof ra_np); ra_tab = 1; ra_pos[RG_ROOT] = 0;
        snprintf(n, sizeof n, "%s_17_explore_sources", lc[li]); frame(RG_ROOT, n);
        ra_pos[RG_SRCS] = 0; snprintf(n, sizeof n, "%s_18_sources", lc[li]); frame(RG_SRCS, n);
        ra_pos[RG_SRCS] = 9999; ra_render(); ra_pos[RG_SRCS] = ra_maxpos(RG_SRCS); snprintf(n, sizeof n, "%s_19_sources_end", lc[li]); frame(RG_SRCS, n);
        eu_list(); ra_pos[RG_LIST] = 0;
        play(&ra_ls[1].it[2], RY_PLAYING, 100, "", ""); snprintf(n, sizeof n, "%s_20_eu_list_playing", lc[li]); frame(RG_LIST, n);
        ra_pos[RG_LIST] = 9999; ra_render(); ra_pos[RG_LIST] = ra_maxpos(RG_LIST); snprintf(n, sizeof n, "%s_21_eu_list_end", lc[li]); frame(RG_LIST, n);
        ra_ls[1].n = 0; ra_ls[1].state = 3; ra_ls[1].err = RSE_NOTLIST; ra_ls[1].sid = ra_sr.s[4].sid; snprintf(ra_ls[1].title, sizeof ra_ls[1].title, "%s", ra_sr.s[4].name);
        ra_pos[RG_LIST] = 0; memset(&ra_np, 0, sizeof ra_np); snprintf(n, sizeof n, "%s_22_source_not_a_list", lc[li]); frame_from_srcs(n);
        ra_ls[1].state = 2; ra_ls[1].err = RSE_EMPTY; snprintf(n, sizeof n, "%s_23_source_empty", lc[li]); frame_from_srcs(n);
        /* ends of a session */
        eu_list(); ra_pos[RG_LIST] = 0;
        play(&ra_ls[1].it[2], RY_ERROR, 0, "", "cap"); snprintf(n, sizeof n, "%s_24_session_cap", lc[li]); frame(RG_LIST, n);
        play(&ra_ls[1].it[2], RY_ERROR, 0, "", "nosd"); snprintf(n, sizeof n, "%s_25_no_sd", lc[li]); frame(RG_LIST, n);
        ra_sr.s[0].on = 0; memset(&ra_np, 0, sizeof ra_np); ra_pos[RG_ROOT] = 0;
        snprintf(n, sizeof n, "%s_26_explore_without_radio_browser", lc[li]); frame(RG_ROOT, n);
        char e0[] = ""; ra_srcs_parse(&ra_sr, e0);
    }
    printf("radio render: %d frames in %s, %d layout problems\n", frames, outdir(), lfails);
    return lfails != 0;
}
