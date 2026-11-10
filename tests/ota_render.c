/* ota_render.c - frames of Settings > General > Software Update (src/ota.inc.c) on the PC, as
 * PNG, in every state of the worker and several languages, with the firmware font and the
 * shell's own page renderer (sp_render, extracted as is by tests/ota_render.sh). Also checks
 * that no row title or value is cut with "…" and that footers fit.
 *   bash tests/ota_render.sh  ->  out/ota_render/ (or $OTA_OUT) */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define W 320
#define H 480
extern char **environ;
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void msleep(int ms) { usleep(ms * 1000); }
static void lg(const char *f, ...) { (void)f; }
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static inline uint16_t mix565(uint16_t c, uint16_t d, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
#include "hm_shared.h"
static struct hm_state st_mem, *st = &st_mem;
static uint16_t fb0[W * H], fb1[W * H], *fr0 = fb0, *fr1 = fb1;
static double toast_until;
static void toast(const char *m) { (void)m; }
static void draw_toast(uint16_t *d) { (void)d; }
static int load_sprite(const char *n, struct sprite *s) { (void)n; memset(s, 0, sizeof *s); return 0; }
static void blit(uint16_t *d, const struct sprite *s, int x, int y, int k, int a) { (void)d; (void)s; (void)x; (void)y; (void)k; (void)a; }
static int confirm(const char *t, const char *b) { (void)t; (void)b; return 0; }
static void progress_card(const char *t, const char *s, double f) { (void)t; (void)s; (void)f; }
static int ss_alert(const char *t, const char *m, const char *ok, int d) { (void)t; (void)m; (void)ok; (void)d; return 0; }
static const char *v_version(void) { return "HiByOS 0.6.1"; }
#include "i18n.inc.c"
#include "text.inc.c"
#include "ota_render_extract.inc"        /* fill_rrect, fill_circle, struct spage, draw_wrapped, sp_render... (from the shell, as is) */
#define ROW(t, ty, iv, op, no, f, v, g) (struct srow){ t, ty, iv, op, no, f, v, g }
static void open_page(void (*b)(struct spage *), const char *back) { (void)b; (void)back; }
#include "ota.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static const char *outdir(void) { return getenv("OTA_OUT") ? getenv("OTA_OUT") : "out/ota_render"; }
static int nfail, nshot;
static void save(const uint16_t *src, const char *name)
{
    static unsigned char px[W * H * 3];
    for (int i = 0; i < W * H; i++) { uint16_t p = src[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, W, H, 3, px, W * 3);
    nshot++;
}
static void status_bar(void) { for (int i = 0; i < 30 * W; i++) fr1[i] = 0; draw_text(fr1, 20, 21, "9:41", 15, 0xFFFF, 0, 0, H); draw_text(fr1, 270, 21, "87 %", 13, 0xFFFF, 0, 0, H); }
/* the same widths sp_render gives each title and value */
static void fit_check(const char *name, const struct spage *pg)
{
    for (int s = 0; s < pg->nsec; s++) for (int r = 0; r < pg->secs[s].n; r++) {
        const struct srow *rw = &pg->secs[s].rows[r];
        int tx = rw->icon ? 70 : 32;
        const char *t = tr(rw->title), *v = rw->type == R_TOGGLE ? 0 : row_value(rw);
        if (rw->type == R_BAR) {
            int vw = v ? tw_n(v, 13, -1) : 0;
            if (vw > 170) { nfail++; printf("CUT %s: bar value '%s' (%d px)\n", name, v, vw); }
            if (tw_n(t, 15, -1) > W - 32 - tx - (vw > 170 ? 170 : vw) - 12) { nfail++; printf("CUT %s: bar title '%s'\n", name, t); }
            continue;
        }
        int chev = rw->type == R_NAV || rw->type == R_CHOICE, vx = W - 16 - 12 - (chev ? 14 : 0);
        int vw = v ? tw_n(v, 15, -1) : 0;
        if (vw > 128) { nfail++; printf("CUT %s: value '%s' (%d px)\n", name, v, vw); vw = 128; }
        int tmax = rw->type == R_TOGGLE ? W - 16 - 12 - 51 - 10 - tx : vx - tx - (v ? vw + 12 : 0);
        if (tw_n(t, 15, -1) > tmax) { nfail++; printf("CUT %s: '%s' (%d px > %d)\n", name, t, tw_n(t, 15, -1), tmax); }
    }
    int tw = tw_n(tr(pg->title), 17, -1);
    if (tw > 150) { nfail++; printf("CUT %s: nav title '%s' (%d px)\n", name, tr(pg->title), tw); }
    int bmax = (W - (tw > 150 ? 150 : tw)) / 2 - 28 - 8; if (bmax < 24) bmax = 24;     /* as sp_render */
    int bw = tw_n(tr("General"), 16, -1);
    if (bw > bmax) { nfail++; printf("CUT %s: back label '%s' (%d px > %d)\n", name, tr("General"), bw, bmax); }
}
static void shot(const char *name)
{
    struct spage pg; page_ota(&pg);
    sp_scroll = 0;
    sp_render(fr0, &pg, -1, "General");
    fit_check(name, &pg);
    save(fr0, name);
    float maxs = sp_content_h(&pg) - (H - NAVH - 30);
    if (maxs > 8) {   /* long page: also the bottom */
        char n2[128]; snprintf(n2, sizeof n2, "%s_end", name);
        sp_scroll = maxs; page_ota(&pg); sp_render(fr0, &pg, -1, "General"); save(fr0, n2); sp_scroll = 0;
    }
}
static const char *CL =
    "{\"format\":1,\"product\":\"hibyos\",\"version\":\"0.6.2\",\"changelog\":{"
    "\"en\":[\"Smoother scrolling in long lists\",\"New Software Update page with online updates\",\"Fixes for the Control Center\"],"
    "\"es\":[\"Desplazamiento más fluido en listas largas\",\"Nueva página de Actualización de software con actualizaciones por Internet\",\"Arreglos en el Centro de control\"],"
    "\"fr\":[\"Défilement plus fluide dans les longues listes\",\"Nouvelle page Mise à jour logicielle avec mises à jour en ligne\",\"Corrections du Centre de contrôle\"],"
    "\"ja\":[\"長いリストのスクロールがより滑らかに\",\"オンラインアップデート対応の新しいソフトウェア・アップデート画面\",\"コントロールセンターの修正\"],"
    "\"de\":\"Flüssigeres Scrollen, neue Seite Softwareupdate mit Online-Updates, Korrekturen im Kontrollzentrum.\"}}";

static void set_info(const char *ver, int needs_fw, const char *minv)
{
    memset(&ota_st.info, 0, sizeof ota_st.info);
    snprintf(ota_st.info.version, sizeof ota_st.info.version, "%s", ver);
    snprintf(ota_st.info.min_version, sizeof ota_st.info.min_version, "%s", minv);
    snprintf(ota_st.info.min_firmware, sizeof ota_st.info.min_firmware, "0.6.0");
    ota_st.info.size = 5 * 1048576 + 300000; ota_st.info.needs_firmware = needs_fw; ota_st.info.restart = OTA_RS_SHELL;
    ota_st.have_info = 1;
    free(ota_st.json); ota_st.json = strdup(CL);
}
static void state(int s, int e) { ota_st.state = s; ota_st.err = e; ota_st.busy = 0; }

int main(int argc, char **argv)
{
    if (!text_init()) { printf("no font\n"); return 2; }
    if (argc > 2 && !strcmp(argv[1], "measure")) {   /* ota_render measure PX TEXT...: widths, to pick short titles */
        for (int i = 3; i < argc; i++) printf("%4d px  %s\n", tw_n(argv[i], atoi(argv[2]), -1), argv[i]);
        return 0;
    }
    /* every character of the 13-language table exists in the firmware fonts (no tofu) */
    int miss = 0;
    for (unsigned i = 0; i < sizeof OTR / sizeof OTR[0]; i++) for (int l = 1; l <= LANG_N; l++) {
        const char *q = OTR[i][l];
        while (*q) {
            int cp = utf8_next(&q), gi = 0;
            if (cp <= ' ') continue;
            if (!font_glyph(cp, &gi) || !gi) { miss++; printf("GLYPH missing U+%04X in %s (language %d)\n", cp, OTR[i][0], l - 1); }
        }
    }
    nfail += miss;
    status_bar();
    static const int langs[] = { LANG_EN, LANG_ES, LANG_FR, LANG_DE, LANG_JA, LANG_RU, LANG_ZHS, LANG_TH };
    static const char *const ln[] = { "en", "es", "fr", "de", "ja", "ru", "zhs", "th" };
    char n[96];
    for (unsigned li = 0; li < sizeof langs / sizeof langs[0]; li++) {
        ui_lang = langs[li];
        snprintf(ota_st.installed, sizeof ota_st.installed, "0.6.1"); snprintf(ota_st.firmware, sizeof ota_st.firmware, "0.6.1");
        snprintf(ota_st.prev, sizeof ota_st.prev, "0.6.0");
        ota_st.last_check = 1794000000; nfw = 0;
        ota_st.have_info = 0;
#define SHOT(tag) do { snprintf(n, sizeof n, "%s_%s", ln[li], tag); shot(n); } while (0)
        state(OS_IDLE, 0); SHOT("01_idle");
        state(OS_CHECKING, 0); ota_st.busy = 1; ota_op = OTA_OP_CHECK; SHOT("02_checking");
        state(OS_UPTODATE, 0); SHOT("03_uptodate");
        state(OS_ERROR, OE_NONET); ota_last_op = OTA_OP_CHECK; SHOT("04_err_nonet");
        state(OS_ERROR, OE_SIG); SHOT("05_err_sig");
        set_info("0.6.2", 0, "0.6.0");
        state(OS_AVAILABLE, 0); SHOT("06_available");
        state(OS_DOWNLOADING, 0); ota_st.busy = 1; ota_op = OTA_OP_INSTALL; ota_st.got = 2 * 1048576 + 400000; ota_st.total = ota_st.info.size; SHOT("07_downloading");
        state(OS_PAUSED, OE_NET); SHOT("08_paused_net");
        state(OS_PAUSED, 0); SHOT("09_paused");
        state(OS_VERIFYING, 0); ota_st.busy = 1; ota_st.frac = 0.6; SHOT("10_verifying");
        state(OS_INSTALLING, 0); ota_st.busy = 1; ota_inst_t0 = now_ms() - 9000; SHOT("11_installing");
        ota_last_op = OTA_OP_INSTALL;
        state(OS_ERROR, OE_INSTALL); SHOT("12_err_install");
        state(OS_ERROR, OE_SPACE_SD); ota_st.need = 12 * 1048576; SHOT("13_err_space");
        state(OS_ERROR, OE_BATT); SHOT("14_err_batt");
        state(OS_DONE, 0); ota_st.restart = OTA_RS_SHELL; snprintf(ota_st.installed, sizeof ota_st.installed, "0.6.2"); SHOT("15_done_shell");
        ota_st.restart = OTA_RS_REBOOT; SHOT("16_done_reboot");
        snprintf(ota_st.installed, sizeof ota_st.installed, "0.6.1");
        set_info("0.7.0", 1, "0.6.0"); state(OS_AVAILABLE, 0); SHOT("17_needs_patcher");
        set_info("0.7.0", 0, "0.6.5"); state(OS_AVAILABLE, 0); SHOT("18_minver");
        set_info("0.6.0", 0, "0.6.0"); state(OS_NEWER, 0); ota_last_op = OTA_OP_CHECK; SHOT("19_newer");
        ota_st.have_info = 0; state(OS_RESTORING, 0); ota_st.busy = 1; ota_op = OTA_OP_ROLLBACK; ota_inst_t0 = now_ms() - 4000; SHOT("20_restoring");
        state(OS_ERROR, OE_NOKEY); ota_last_op = OTA_OP_CHECK; SHOT("21_err_nokey");
    }
    printf("%d frames in %s, %d cut texts\n", nshot, outdir(), nfail);
    return nfail != 0;
}
