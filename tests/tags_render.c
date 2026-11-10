/* tags_render.c - fotogramas del editor de etiquetas (src/tageditor.inc.c) en el PC, en PNG, para
 * revisar el diseno sin el aparato: formulario de una cancion y de un album (8 campos, "Mas
 * etiquetas", "Archivo"), desplazado, campo cambiado/borrado, pantalla "Renombrar archivos" con
 * vista previa y conflicto, y el editor de varias lineas, en es/en/fr/ja/ru.
 *   bash tests/tags_render.sh  ->  out/tags_render/ (o $TAGS_OUT)
 * Usa tageditor.inc.c y hibytag.c tal cual; el teclado y la entrada tactil son de pega. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <spawn.h>
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
#define ASSETS "/tmp"
extern char **environ;
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
static double now_ms(void) { return 100000; }
static void msleep(int ms) { (void)ms; }
static void lg(const char *f, ...) { (void)f; }
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
static inline uint16_t dim565(uint16_t p, int k) { unsigned r = (p >> 11) * k >> 5, g = ((p >> 5) & 63) * k >> 5, b = (p & 31) * k >> 5; return (uint16_t)(r << 11 | g << 5 | b); }
static inline uint16_t mix565(uint16_t c, uint16_t d, unsigned a)
{
    unsigned na = 255 - a;
    return (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
}
static double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
static uint16_t fb0[W * H], fb1[W * H], *fr0 = fb0, *fr1 = fb1;
static int cur_x, cur_y, cur_down;
struct pkt { int n; int x, y, down; };
static int read_pkt(struct pkt *p, int t) { (void)p; (void)t; return 0; }
static void present(void) {}
static double toast_until;
static void toast(const char *m) { (void)m; }
static void draw_toast(uint16_t *d) { (void)d; }
static int back_zone(int sx, int sy) { (void)sx; (void)sy; return 0; }
static int app_back_gesture(int sx, int sy) { (void)sx; (void)sy; return 0; }
static int confirm(const char *t, const char *b) { (void)t; (void)b; return 0; }
static int action_sheet(const char *t, const char **o, int n, int d) { (void)t; (void)o; (void)n; (void)d; return -1; }
static void cover_search(char *const *p, int n, const char *a, const char *b) { (void)p; (void)n; (void)a; (void)b; }
static void lyrics_search(const char *p, const char *a, const char *t, const char *b) { (void)p; (void)a; (void)t; (void)b; }
#include "hm_shared.h"
static struct hm_state st_mem, *st = &st_mem;
/* teclado de pega (oculto) */
struct ebuf { char *b; int len, cap, cur; };
enum { K_ENTER = -5 };
static int kb_off, kb_layer, kb_shift;
#define KB_Y (H + kb_off)
static const char *le_title, *le_hint; static struct ebuf *le_buf;
static void le_redraw(uint16_t *d, int p) { (void)d; (void)p; }
static void kb_layout(const char *l) { (void)l; }
static void kb_begin(struct ebuf *e, uint16_t *d, void (*r)(uint16_t *, int)) { (void)e; (void)d; (void)r; }
static void kb_reveal(uint16_t *d, void (*r)(uint16_t *, int)) { (void)d; (void)r; }
static int kb_touch(struct pkt *p, struct ebuf *e, uint16_t *d, void (*r)(uint16_t *, int), int m) { (void)p; (void)e; (void)d; (void)r; (void)m; return 0; }
static void kb_end(void) {}
static void kb_draw(uint16_t *d, int p) { (void)d; (void)p; }
#include "i18n.inc.c"
#include "text.inc.c"
#include "tags_render_extract.inc"       /* fill_rrect, draw_wrapped... (del shell, tal cual) */
#include "tageditor.inc.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
static const char *outdir(void) { return getenv("TAGS_OUT") ? getenv("TAGS_OUT") : "out/tags_render"; }
static void save(const char *name)
{
    static unsigned char px[W * H * 3];
    for (int i = 0; i < W * H; i++) { uint16_t p = fr0[i]; px[i * 3] = (unsigned char)((p >> 11) * 255 / 31); px[i * 3 + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63); px[i * 3 + 2] = (unsigned char)((p & 31) * 255 / 31); }
    char p[300]; snprintf(p, sizeof p, "%s/%s.png", outdir(), name);
    stbi_write_png(p, W, H, 3, px, W * 3);
}
static void status_bar(void) { for (int i = 0; i < 30 * W; i++) fr1[i] = 0; draw_text(fr1, 20, 21, "9:41", 15, 0xFFFF, 0, 0, H); }
/* FLAC minimo valido (STREAMINFO + comentarios + relleno + "audio") */
static void mk_flac(const char *path, const char *const *cm)
{
    FILE *f = fopen(path, "wb");
    unsigned char si[38] = { 0, 0, 0, 34 };
    fwrite("fLaC", 1, 4, f); fwrite(si, 1, 38, f);
    unsigned len = 4 + 7 + 4; int n = 0;
    for (; cm[n]; n++) len += 4 + (unsigned)strlen(cm[n]);
    unsigned char h[4] = { 4, (unsigned char)(len >> 16), (unsigned char)(len >> 8), (unsigned char)len };
    fwrite(h, 1, 4, f);
    unsigned char b[4]; htx_put_le32(b, 7); fwrite(b, 1, 4, f); fwrite("hibytag", 1, 7, f);
    htx_put_le32(b, (uint32_t)n); fwrite(b, 1, 4, f);
    for (int i = 0; i < n; i++) { htx_put_le32(b, (uint32_t)strlen(cm[i])); fwrite(b, 1, 4, f); fwrite(cm[i], 1, strlen(cm[i]), f); }
    unsigned char p[4] = { 0x81, 0, 0x10, 0 }; fwrite(p, 1, 4, f);
    static unsigned char z[4096]; fwrite(z, 1, sizeof z, f);
    fwrite("\xFF\xF8" "audio", 1, 7, f);
    fclose(f);
}
/* ------------------------------------------------------------------ flujo de guardar de verdad
 * te_save() con ficheros, .lrc, cover.jpg y una base SQLite: etiquetas + renombrado en dos fases +
 * carpeta Artista/Album + "hibytag dbrename" (el binario de verdad en ASSETS=/tmp); y la vuelta
 * atras cuando la base falla. */
#include "sqlite3.h"
static int ffails, fchecks;
#define FCHECK(c, ...) do { fchecks++; if (!(c)) { ffails++; printf("FALLO flujo %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int fexists(const char *p) { return !access(p, F_OK); }
static void ftouch(const char *p) { FILE *f = fopen(p, "w"); if (f) { fputs("x", f); fclose(f); } }
static const char *const FT[3] = { "Give Life Back to Music", "The Game of Love", "Get Lucky" };
static void flow_album(const char *root, char pth[3][400])
{
    char d[400], c[700];
    snprintf(c, sizeof c, "rm -rf '%s' && mkdir -p '%s/Daft Punk/RAM'", root, root); if (system(c)) return;
    snprintf(d, sizeof d, "%s/Daft Punk/RAM", root);
    for (int i = 0; i < 3; i++) {
        char t[100], tn[32]; snprintf(t, sizeof t, "TITLE=%s", FT[i]); snprintf(tn, sizeof tn, "TRACKNUMBER=%d", (i + 1) % 10);
        const char *cm[] = { t, tn, "ARTIST=Daft Punk", "ALBUM=RAM", "ALBUMARTIST=Daft Punk", 0 };
        snprintf(pth[i], 400, "%s/%02d %s.flac", d, i + 1, FT[i]); mk_flac(pth[i], cm);
    }
    snprintf(c, sizeof c, "%s/03 Get Lucky.lrc", d); ftouch(c);
    snprintf(c, sizeof c, "%s/cover.jpg", d); ftouch(c);
}
static void flow_db(const char *db, char pth[3][400])
{
    unlink(db);
    sqlite3 *h; sqlite3_open(db, &h);
    sqlite3_exec(h, "CREATE TABLE MEDIA_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,"
                    "genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,"
                    "size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,"
                    "lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id))", 0, 0, 0);
    for (int i = 0; i < 3; i++) {
        char dp[500]; const char *m = strstr(pth[i], "/sd_0/");
        snprintf(dp, sizeof dp, "a:\\%s", m + 6); for (char *c = dp; *c; c++) if (*c == '/') *c = '\\';
        sqlite3_stmt *q; sqlite3_prepare_v2(h, "INSERT INTO MEDIA_TABLE(id,path,name,album,artist,cue_id) VALUES(?,?,'n','RAM','Daft Punk',-1)", -1, &q, 0);
        sqlite3_bind_int(q, 1, i + 1); sqlite3_bind_text(q, 2, dp, (int)strlen(dp) + 1, SQLITE_TRANSIENT);
        sqlite3_step(q); sqlite3_finalize(q);
    }
    sqlite3_close(h);
}
static int flow_db_count(const char *db, const char *like)
{
    sqlite3 *h; int c = -1; sqlite3_stmt *q;
    if (sqlite3_open(db, &h)) return -1;
    if (!sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM MEDIA_TABLE WHERE path LIKE ?", -1, &q, 0)) {
        sqlite3_bind_text(q, 1, like, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) c = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
    }
    sqlite3_close(h);
    return c;
}
static void test_flow(void)
{
    const char *root = "/tmp/tags_flow/mnt/sd_0", *db = "/tmp/tags_flow/media.db";
    char pth[3][400], *pp[3] = { pth[0], pth[1], pth[2] }, p[700];
    ui_lang = LANG_ES;
    setenv("HIBYTAG_SD", root, 1); setenv("HIBYTAG_DB", db, 1);
    /* 1. album: compositor + artista del album + patron "Pista - Titulo" + carpeta Artista/Album */
    flow_album(root, pth); flow_db(db, pth);
    te_load(pp, 3);
    te_set(te_find("ALBUMARTIST"), "Daft Punk & Friends");
    int i = te_add("COMPOSER", "COMPOSER"); te_fs[i].added = 1; te_fs[i].val = strdup(""); te_set(i, "Thomas Bangalter");
    te_pat = HT_PAT_NUM_TITLE; te_fold = TE_FOLD_ARTIST;
    FCHECK(te_save() == 1, "te_save album");
    const char *nd = "/tmp/tags_flow/mnt/sd_0/Daft Punk & Friends/RAM";
    for (int k = 0; k < 3; k++) {
        snprintf(p, sizeof p, "%s/%02d - %s.flac", nd, k + 1, FT[k]);
        FCHECK(fexists(p), "renombrado + movido: %s", p);
        ht_all a; ht_read_all(p, &a);
        const ht_item *it = ht_all_find(&a, "COMPOSER"), *aa = ht_all_find(&a, "ALBUMARTIST");
        FCHECK(it && !strcmp(it->val, "Thomas Bangalter") && aa && !strcmp(aa->val, "Daft Punk & Friends"), "etiquetas escritas en %d", k);
        ht_all_free(&a);
    }
    snprintf(p, sizeof p, "%s/03 - Get Lucky.lrc", nd); FCHECK(fexists(p), ".lrc renombrado");
    snprintf(p, sizeof p, "%s/cover.jpg", nd); FCHECK(fexists(p), "cover.jpg movida");
    FCHECK(!fexists("/tmp/tags_flow/mnt/sd_0/Daft Punk"), "carpeta vieja del artista borrada (vacia)");
    FCHECK(flow_db_count(db, "a:\\Daft Punk & Friends\\RAM\\0_ - %.flac%") == 3, "base: 3 rutas nuevas (%d)", flow_db_count(db, "a:\\Daft Punk & Friends\\RAM\\0_ - %.flac%"));
    FCHECK(flow_db_count(db, "a:\\Daft Punk\\%") == 0, "base: sin rutas viejas");
    te_free();
    /* 2. la base falla (fichero que no es SQLite): todo se deshace */
    flow_album(root, pth);
    FILE *g = fopen(db, "w"); fputs("esto no es una base de datos sqlite, solo texto para que falle la transaccion ...........", g); fclose(g);
    te_load(pp, 3);
    te_pat = HT_PAT_ARTIST_TITLE; te_fold = TE_FOLD_ALBUM; te_set(HT_ALBUM, "RAM (Deluxe)");
    te_save();
    for (int k = 0; k < 3; k++) FCHECK(fexists(pth[k]), "vuelta atras: %s sigue", pth[k]);
    snprintf(p, sizeof p, "%s/Daft Punk/RAM/03 Get Lucky.lrc", root); FCHECK(fexists(p), "vuelta atras: .lrc en su sitio");
    FCHECK(!fexists("/tmp/tags_flow/mnt/sd_0/Daft Punk/RAM (Deluxe)"), "vuelta atras: carpeta nueva no queda");
    { DIR *dd = opendir("/tmp/tags_flow/mnt/sd_0/Daft Punk/RAM"); struct dirent *e; int tmp = 0; while (dd && (e = readdir(dd))) if (strstr(e->d_name, ".hibytag-")) tmp = 1; if (dd) closedir(dd); FCHECK(!tmp, "sin temporales"); }
    te_free();
    /* 3. sin base (no existe): se renombra y se avisa de actualizar la biblioteca */
    flow_album(root, pth); unlink(db);
    te_load(pp + 2, 1);
    snprintf(te_fname, sizeof te_fname, "Get Lucky (Radio Edit).flac"); te_fname_on = 1;
    te_save();
    snprintf(p, sizeof p, "%s/Daft Punk/RAM/Get Lucky (Radio Edit).flac", root); FCHECK(fexists(p), "una cancion renombrada");
    snprintf(p, sizeof p, "%s/Daft Punk/RAM/Get Lucky (Radio Edit).lrc", root); FCHECK(fexists(p), "su .lrc tambien");
    te_free();
    /* 4. intercambio de nombres dentro del album (fase 1 -> temporales, fase 2 -> definitivos) */
    flow_album(root, pth); flow_db(db, pth);
    te_load(pp, 3);
    te_pat = HT_PAT_NUMTITLE;                       /* "01 Give...", "02 The Game...", "03 Get Lucky" = nombres de ahora */
    FCHECK(te_save() == 1, "sin cambios de nombre");
    for (int k = 0; k < 3; k++) FCHECK(fexists(pth[k]), "nombres iguales: %s", pth[k]);
    te_free();
    printf("flujo: %d comprobaciones, %d fallos\n", fchecks, ffails);
}

int main(void)
{
    if (!text_init()) { printf("no se pudo cargar default.otf\n"); return 2; }
    if (getenv("TAGS_FLOW_ONLY")) { test_flow(); return ffails != 0; }
    mkdir("out", 0755); mkdir(outdir(), 0755);
    system("rm -rf /tmp/tags_render_sd && mkdir -p '/tmp/tags_render_sd/mnt/sd_0/Daft Punk/Random Access Memories'");
    const char *dir = "/tmp/tags_render_sd/mnt/sd_0/Daft Punk/Random Access Memories";
    static const char *const titles[3] = { "Give Life Back to Music", "The Game of Love", "Get Lucky" };
    char pth[3][400]; char *pp[3];
    for (int i = 0; i < 3; i++) {
        char t[200], tn[20];
        snprintf(t, sizeof t, "TITLE=%s", titles[i]); snprintf(tn, sizeof tn, "TRACKNUMBER=%d", i + 1);
        const char *cm[] = { t, "ARTIST=Daft Punk", i == 2 ? "ARTIST=Pharrell Williams" : "ALBUM=Random Access Memories",
                             "ALBUM=Random Access Memories", "ALBUMARTIST=Daft Punk", "GENRE=Electronic", "DATE=2013", tn, "DISCNUMBER=1",
                             "TRACKTOTAL=13", "COMPOSER=Thomas Bangalter", "LABEL=Columbia", i == 2 ? "ISRC=USQX91300108" : "ISRC=USQX9130010",
                             "COMMENT=Remastered 2023\nfrom the original tapes", "LYRICS=Like the legend of the phoenix\nAll ends with beginnings",
                             "REPLAYGAIN_TRACK_GAIN=-8.21 dB", "MUSICBRAINZ_ALBUMID=7c3218d7-75e0-4e8c-971f-f097b6c308c5", 0 };
        snprintf(pth[i], sizeof pth[i], "%s/%02d %s.flac", dir, i + 1, titles[i]); pp[i] = pth[i];
        mk_flac(pth[i], cm);
    }
    status_bar();
    static const int langs[] = { LANG_ES, LANG_EN, LANG_FR, LANG_JA, LANG_RU };
    for (unsigned li = 0; li < sizeof langs / sizeof langs[0]; li++) {
        ui_lang = langs[li]; const char *code = LANG_CODES[ui_lang]; char n[64];
        struct te_scr s; memset(&s, 0, sizeof s);
        /* una cancion */
        te_load(pp + 2, 1);
        te_build_main(&s); s.sc = 0; te_render(&s, -1); snprintf(n, sizeof n, "song_%s", code); save(n);
        te_set(te_find("COMPOSER"), "Nile Rodgers"); te_set(te_find("LABEL"), "");
        snprintf(te_fname, sizeof te_fname, "03 - Get Lucky (feat. Pharrell).flac"); te_fname_on = 1;
        te_build_main(&s); s.sc = te_maxsc(&s); te_render(&s, -1); snprintf(n, sizeof n, "song_scrolled_edited_%s", code); save(n);
        te_render(&s, 9); snprintf(n, sizeof n, "song_pressed_%s", code); save(n);
        /* album */
        te_load(pp, 3);
        te_build_main(&s); s.sc = 0; te_render(&s, -1); snprintf(n, sizeof n, "album_%s", code); save(n);
        s.sc = te_maxsc(&s); te_render(&s, -1); snprintf(n, sizeof n, "album_bottom_%s", code); save(n);
        /* renombrar: patron + Artista/Album con el artista cambiado */
        te_set(HT_ALBUMARTIST, "Daft Punk & Friends");
        rn_load(); rn_pat = 0; rn_fold = TE_FOLD_ARTIST;
        struct te_scr r; memset(&r, 0, sizeof r); te_build_ren(&r); r.sc = 0; te_render(&r, -1); snprintf(n, sizeof n, "rename_%s", code); save(n);
        r.sc = te_maxsc(&r); te_render(&r, -1); snprintf(n, sizeof n, "rename_bottom_%s", code); save(n);
        /* conflicto: un fichero ajeno con el nombre de destino */
        char clash[500]; snprintf(clash, sizeof clash, "%s/02 - The Game of Love.flac", dir); FILE *cf = fopen(clash, "w"); fclose(cf);
        te_build_ren(&r); r.sc = te_maxsc(&r); te_render(&r, -1); snprintf(n, sizeof n, "rename_conflict_%s", code); save(n);
        unlink(clash);
        rn_free(); free(r.r); free(s.r);
        /* editor de varias lineas (teclado oculto) */
        static char big[2000];
        snprintf(big, sizeof big, "Like the legend of the phoenix\nAll ends with beginnings\nWhat keeps the planet spinning\nThe force from the beginning\n\nWe've come too far to give up who we are\nSo let's raise the bar and our cups to the stars");
        tt = (struct ebuf){ big, (int)strlen(big), sizeof big, 40 }; tt_title = tr("Letra"); tt_top = 0; tt_sc = 0;
        tt_redraw(fr0, -1); snprintf(n, sizeof n, "multiline_%s", code); save(n);
        te_free();
    }
    printf("PNG en %s/\n", outdir());
    test_flow();
    return ffails != 0;
}
