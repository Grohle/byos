/* launcher.inc.c - inicio propio de hibyshell (incluido desde hibyshell.c).
 *
 * Mientras el reproductor esta en su pantalla de inicio, el shell la cubre (modo captura) con
 * este launcher: varias paginas que se deslizan, dock de cristal, puntos de pagina, iconos y
 * etiquetas a tamano configurable, edicion estilo iOS (pulsacion larga, temblor, arrastrar a
 * otra posicion, al dock o a otra pagina) y apertura de apps con zoom desde el icono.
 * Las apps de HiBy se abren tocando por debajo su icono en el inicio de fabrica (invisible);
 * la barra de estado (hora, bateria, volumen) es la del reproductor, copiada en vivo.
 * Configuracion: /usr/data/hibymod/shell/launcher.conf (texto). */

#define LCONF ASSETS "/launcher.conf"
#define MAXAPPS 16
#define MAXPAGES 6
#define DOCK_X 10
#define DOCK_W 300
#define DOCK_H 84
#define DOCK_Y (H - 18 - DOCK_H)          /* 18 px abajo: espacio para la flecha de musica */
#define GRID_TOP 46
#define DOTS_Y (DOCK_Y - 13)

enum { ACT_TAP, ACT_PLANE, ACT_STREAM, ACT_FILES, ACT_SETTINGS, ACT_STORE, ACT_EXT, ACT_MUSIC, ACT_BOOKS, ACT_FREEMUSIC, ACT_BUDS,
       ACT_WIRELESS, ACT_RADIO, ACT_SOON };
struct hpk_info;
/* definidas en launcher_touch.inc.c (definiciones provisionales de C, mismo objeto) */
static double toast_until;
static uint16_t img_app[W * H], img_base[W * H];
struct lapp {
    const char *id, *label; int act, ax, ay;
    struct sprite icon[3];          /* 48, 56, 64 */
    struct sprite lbl;
    const char *names3[3];          /* apps instaladas: nombre en es/en/fr */
    struct hpk_info *ext;           /* apps instaladas (.hpk) */
};
static struct lapp apps[40] = {
    { "music",      "Música",        ACT_MUSIC,  116, 425 },   /* app propia; (ax, ay) = la de fabrica */
    /* Ajustes lo tiene TODO (Wi-Fi, Bluetooth, USB/DAC y la personalizacion son paginas suyas).
     * Los antiguos iconos "Inalambrico", "Personalizar" y "DAC USB" ya no existen ("wireless" y
     * "dac" valen como alias de "settings": ver app_by_id). El codigo de Inalambrico sigue: es la
     * pagina Wi-Fi/Bluetooth de Ajustes (wireless_sub). */
    { "settings",   "Ajustes",       ACT_SETTINGS, 202, 425 },
    { "radio",      "Radio",         ACT_RADIO,    0,   0 },   /* emisoras por internet (Radio Browser + propias) */
    { "books",      "Libros",        ACT_BOOKS,  117,  74 },   /* lector propio (TXT/EPUB) */
    { "tidal",      "Tidal",         ACT_STREAM,   0,   0 },
    { "qobuz",      "Qobuz",         ACT_STREAM,   1,   0 },
    { "nowplaying", "Reproduciendo", ACT_MUSIC,    0,   0 },   /* Reproduciendo propio (nowui) */
    { "files",      "Archivos",      ACT_FILES,    0,   0 },
    { "appstore",   "Apps",          ACT_STORE,    0,   0 },
    { "freemusic",  "Vinyl",         ACT_FREEMUSIC, 0,  0 },   /* Vinyl: musica libre (Internet Archive, Jamendo, repositorios) */
    { "headphones", "Auriculares",   ACT_BUDS,     0,   0 },   /* AirPods, Galaxy Buds, Nothing... */
    { "gutenberg",  "Gutenberg",     ACT_BOOKS,    1,   0 },   /* Libros abierto en su tienda (audiolibros + libros) */
};
static int napps = 12;              /* internas + instaladas (register_installed_apps) */
#define NAPPS napps
static const char *app_label(int a) { int l = lang3(); return apps[a].names3[l] && apps[a].names3[l][0] ? apps[a].names3[l] : tr(apps[a].label); }

static struct {
    int icon, cols;
    int npages, pn[MAXPAGES], page[MAXPAGES][MAXAPPS];
    int dn, dock[4];
    char wall[200];
    int labels, dots, dockbg, dim;   /* nombres de las apps, puntos de pagina, fondo del dock, oscurecer el fondo */
} L;

/* version del orden oficial de las apps: un launcher.conf de una version anterior se reordena
 * UNA vez al orden oficial (las apps instaladas desde Apps se quedan al final, en su orden) */
#define LCONF_ORDER 3
static int launcher_enabled = 1, cur_page;
static uint16_t wall[W * H];
static struct sprite dock_glass;
static int sz_idx(void) { return L.icon <= 48 ? 0 : L.icon >= 64 ? 2 : 1; }

/* app_find: id exacto (lo que hay en launcher.conf). app_by_id: ademas el id de la app que
 * ahora es una pagina de Ajustes, para el zoom, el gesto de inicio y la tarjeta de multitarea. */
static int app_find(const char *id) { for (int i = 0; i < NAPPS; i++) if (!strcmp(apps[i].id, id)) return i; return -1; }
static int app_by_id(const char *id)
{
    if (!strcmp(id, "dac") || !strcmp(id, "wireless")) id = "settings";
    return app_find(id);
}
/* Apps incluidas que NO son del sistema base: vienen instaladas, pero se pueden desinstalar (salen
 * del inicio) y volver a instalar desde Apps, como las que se bajan de un repositorio. */
static const char *const sys_removable[] = { "gutenberg", "freemusic", "headphones" };
#define NSYSREM ((int)(sizeof sys_removable / sizeof *sys_removable))
static unsigned char app_removed[40];        /* por indice de apps[] (solo internas: indices fijos) */
static int app_sys_removable(int a)
{
    if (a < 0 || a >= NAPPS) return 0;
    for (int k = 0; k < NSYSREM; k++) if (!strcmp(apps[a].id, sys_removable[k])) return 1;
    return 0;
}
static int app_removable(int a) { return a >= 0 && a < NAPPS && (apps[a].act == ACT_EXT || app_sys_removable(a)); }
/* pagina en la que se abre Ajustes la proxima vez (la pone el Centro de control y se consume
 * en launch_app): 0 raiz, 1 USB, 2 Wi-Fi y Bluetooth, 3 personalizacion (icono "Personalizar") */
enum { SET_ROOT, SET_USB, SET_WIRELESS, SET_PERS, SET_POWER, SET_SOUND, SET_DISPLAY, SET_CCPAGE };
static int settings_start;

static int per_page(void);
/* como iOS: las apps llenan los huecos desde arriba a la izquierda, pagina a pagina,
 * y no hay paginas vacias (una segunda pagina solo existe si la primera esta llena) */
static void lconf_compact(void)
{
    int all[MAXPAGES * MAXAPPS], n = 0, pp = per_page(); if (pp > MAXAPPS) pp = MAXAPPS; if (pp < 1) pp = 1;
    for (int p = 0; p < L.npages; p++) for (int i = 0; i < L.pn[p]; i++) if (L.page[p][i] >= 0) all[n++] = L.page[p][i];
    memset(L.pn, 0, sizeof L.pn); L.npages = 1;
    for (int i = 0; i < n; i++) {
        int p = L.npages - 1;
        if (L.pn[p] >= pp) { if (L.npages >= MAXPAGES) break; p = L.npages++; }
        L.page[p][L.pn[p]++] = all[i];
    }
}
/* fuera del inicio lo desinstalado, y sin paginas vacias */
static void lconf_drop_empty_pages(void)
{
    for (int p = 0; p < L.npages; p++) if (!L.pn[p] && L.npages > 1) {
        for (int q = p; q < L.npages - 1; q++) { L.pn[q] = L.pn[q + 1]; memcpy(L.page[q], L.page[q + 1], sizeof L.page[q]); }
        L.npages--; p--;
    }
}
static void lconf_strip_removed(void)
{
    for (int p = 0; p < L.npages; p++) for (int i = 0; i < L.pn[p]; i++) if (app_removed[L.page[p][i]]) {
        memmove(&L.page[p][i], &L.page[p][i + 1], (L.pn[p] - i - 1) * sizeof(int)); L.pn[p]--; i--;
    }
    for (int i = 0; i < L.dn; i++) if (app_removed[L.dock[i]]) { memmove(&L.dock[i], &L.dock[i + 1], (L.dn - i - 1) * sizeof(int)); L.dn--; i--; }
    lconf_drop_empty_pages();
}
static void lconf_default(void)
{
    memset(&L, 0, sizeof L);
    memset(app_removed, 0, sizeof app_removed);      /* "Restablecer inicio" las vuelve a instalar */
    L.icon = 56; L.cols = 4; L.npages = 1;
    L.labels = L.dots = L.dockbg = 1; L.dim = 0;
    /* ORDEN OFICIAL de una instalacion nueva (LCONF_ORDER lo aplica una vez a las ya instaladas) */
    static const char *const p0[] = { "settings", "files", "radio", "appstore",
                                      "tidal", "qobuz", "gutenberg", "freemusic" };
    static const char *const d0[] = { "books", "headphones", "nowplaying", "music" };
    for (unsigned i = 0; i < sizeof p0 / sizeof *p0; i++) { int a = app_find(p0[i]); if (a >= 0) L.page[0][L.pn[0]++] = a; }
    for (unsigned i = 0; i < sizeof d0 / sizeof *d0; i++) { int a = app_find(d0[i]); if (a >= 0) L.dock[L.dn++] = a; }
}

/* ------------------------------------------------------------------ espejo del inicio */
/* El inicio de fabrica tiene como fondo launcher/mirror.png (sus iconos son transparentes):
 * es la pagina 1 de este launcher, para que al volver deslizando desde una app lo que asoma
 * sea identico. HiBy carga esa imagen al arrancar: se regenera al cambiar el inicio. */
#define STBIW_ASSERT(x)
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define MIRROR_PNG "/usr/data/hibymod/theme/litegui/theme3/launcher/mirror.png"
static void render_launcher(uint16_t *dst, double t);
static int mirror_dirty;
static void mirror_save(void);

/* Firma del inicio de fabrica = puntos del espejo: centros de iconos de la pagina 1 y del
 * dock (unicos aunque el fondo sea negro) + dos puntos de fondo. HiBy carga el espejo al
 * arrancar, asi que la firma nueva ("next") se activa en el SIGUIENTE arranque (boot_id). */
static void boot_id(char *b, int n)
{
    b[0] = 0; int f = open("/proc/sys/kernel/random/boot_id", O_RDONLY);
    if (f >= 0) { int k = read(f, b, n - 1); if (k > 0) b[k] = 0; close(f); }
    for (char *c = b; *c; c++) if (*c == '\n') *c = 0;
}
static void cell_xy(int idx, int *x, int *y);
static void dock_xy(int idx, int n, int *x, int *y);
static void mirror_sig_write(const uint16_t *img)
{
    int pts[8][2], n = 0;
    for (int i = 0; i < L.pn[0] && n < 3; i++) { int x, y; cell_xy(i, &x, &y); pts[n][0] = x + L.icon / 2; pts[n][1] = y + L.icon / 4; n++; }
    for (int i = 0; i < L.dn && n < 5; i++) { int x, y; dock_xy(i, L.dn, &x, &y); pts[n][0] = x + L.icon / 2; pts[n][1] = y + L.icon / 4; n++; }
    pts[n][0] = 6; pts[n][1] = DOCK_Y - 30; n++;              /* fondo (borde izquierdo) */
    pts[n][0] = W - 6; pts[n][1] = DOCK_Y - 60; n++;          /* fondo (borde derecho) */
    char bid[64]; boot_id(bid, sizeof bid);
    FILE *f = fopen(ASSETS "/mirror.next.sig", "w"); if (!f) return;
    fprintf(f, "%s\n", bid);
    for (int i = 0; i < n; i++) {
        uint16_t p = img[pts[i][1] * W + pts[i][0]];
        fprintf(f, "%d %d %d %d %d 26\n", pts[i][0], pts[i][1], (p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31);
    }
    fclose(f);
}

static int edit_bg_ok;
static int wg_part_y0, wg_part_y1;   /* widgets.inc.c */
/* esquinas redondeadas de toda la pantalla (Ajustes > Pantalla y brillo; "corners" en la
 * configuracion, activadas por defecto). Las dibuja la libreria de pantalla en cada imagen. */
#define CORNER_R 16
static int ui_corners = 1;
static void corners_push(void)
{
    if (!st) return;
    st->corner_r = ui_corners ? CORNER_R : 0;
    cmd('O');                                     /* repintar ya lo que se ve */
}
static void lconf_save(void)
{
    mirror_dirty = 1; edit_bg_ok = 0;
    FILE *f = fopen(LCONF ".tmp", "w");
    if (!f) return;
    fprintf(f, "# launcher de hibymod\nlayout 2\norder %d\nicon %d\ncols %d\nwall %s\ndock", LCONF_ORDER, L.icon, L.cols, L.wall[0] ? L.wall : "-");
    for (int i = 0; i < L.dn; i++) fprintf(f, " %s", apps[L.dock[i]].id);
    for (int p = 0; p < L.npages; p++) {
        if (!L.pn[p]) continue;
        fprintf(f, "\npage");
        for (int i = 0; i < L.pn[p]; i++) fprintf(f, " %s", apps[L.page[p][i]].id);
    }
    int nrem = 0; for (int a = 0; a < NAPPS; a++) if (app_removed[a]) { fprintf(f, nrem++ ? " %s" : "\nremoved %s", apps[a].id); }
    fprintf(f, "\nlook %d %d %d %d\n", L.labels, L.dots, L.dockbg, L.dim);
    gestures_conf_save(f);
    cc_conf_save(f);
    wg_conf_save(f);
    fprintf(f, "lang %s\nkb %d %d %d\nkbs %d\ncorners %d\nvolui %d\n", ui_lang_sys ? "sys" : lang_code(), kb_cfg_height, kb_cfg_numrow, kb_cfg_layout,
            kb_cfg_layouts, ui_corners, vol_ui);
    fprintf(f, "sync %d\n", sync_wifi_on);        /* ByOS Sync: conexiones por Wi-Fi (sync.inc.c) */
    /* a prueba de cortes (UBIFS casi lleno, reinicio justo despues): solo se sustituye el
     * fichero si se escribio entero y llego al disco; el anterior queda como .bak */
    int bad = ferror(f) || fflush(f) || fsync(fileno(f));
    if (fclose(f) || bad) { unlink(LCONF ".tmp"); lg("launcher.conf: no se pudo guardar (se conserva el anterior)"); return; }
    if (!access(LCONF, F_OK)) { unlink(LCONF ".bak"); if (link(LCONF, LCONF ".bak")) {} }
    rename(LCONF ".tmp", LCONF);
    { int d = open(ASSETS, O_RDONLY); if (d >= 0) { fsync(d); close(d); } }
}

/* teclado por defecto segun el idioma: el suyo (cada idioma tiene el suyo; su id es el LANG_*) */
static int kb_layout_for(int l) { return lang_ok(l) ? l : LANG_EN; }
/* idioma nuevo (Ajustes o "igual que el sistema"): su teclado se activa y pasa a ser el actual (iOS) */
static void kb_lang_added(int l) { l = kb_layout_for(l); kb_cfg_layouts |= 1 << l; kb_cfg_layout = l; }
static int lconf_order_migrated;     /* lconf_load reordeno al orden oficial: hay que guardarlo */
static void relabel_apps(void);
/* "Igual que el sistema": si el usuario cambio el idioma en los ajustes de HiBy, seguirlo */
static void lang_sys_poll(void)
{
    if (!ui_lang_sys) return;
    int l = lang_system();
    if (l != ui_lang) { ui_lang = l; relabel_apps(); kb_lang_added(l); lconf_save(); }
}
static void lconf_load0(void)
{
    lconf_default();
    ui_lang_sys = 1; ui_lang = lang_system();        /* sin "lang" (instalacion nueva): el del sistema */
    kb_cfg_layout = kb_layout_for(ui_lang); kb_cfg_layouts = 0;
    FILE *f = fopen(LCONF, "r");
    if (!f) { kb_cfg_layouts = 1 << kb_cfg_layout | 1 << LANG_EN; return; }
    char line[512]; int pages = 0, seen[40] = {0}, got_dock = 0, layout2 = 0, got_kb = 0, order = 0;
    int npn[MAXPAGES] = {0}, np[MAXPAGES][MAXAPPS], nd = 0, dk[4];
    while (fgets(line, sizeof line, f)) {
        char *t = strtok(line, " \t\r\n");
        if (!t || t[0] == '#') continue;
        if (!strcmp(t, "layout")) { char *v = strtok(0, " \t\r\n"); layout2 = v && atoi(v) >= 2; }
        else if (!strcmp(t, "order")) { char *v = strtok(0, " \t\r\n"); order = v ? atoi(v) : 0; }
        else if (!strcmp(t, "icon")) { char *v = strtok(0, " \t\r\n"); if (v) { int s = atoi(v); L.icon = s <= 48 ? 48 : s >= 64 ? 64 : 56; } }
        else if (!strcmp(t, "cols")) { char *v = strtok(0, " \t\r\n"); if (v) { int c = atoi(v); L.cols = c < 3 ? 3 : c > 5 ? 5 : c; } }
        else if (!strcmp(t, "corners")) { char *v = strtok(0, " \t\r\n"); ui_corners = v ? atoi(v) != 0 : 1; }
        else if (!strcmp(t, "volui")) { char *v = strtok(0, " \t\r\n"); vol_ui = v ? atoi(v) != 0 : 0; }
        else if (!strcmp(t, "sync")) { char *v = strtok(0, " \t\r\n"); sync_wifi_on = v ? atoi(v) != 0 : 0; }
        else if (!strcmp(t, "lang")) { char *v = strtok(0, " \t\r\n"); int l = v ? lang_from_code(v) : -1; if (l >= 0) { ui_lang = l; ui_lang_sys = 0; } }
        else if (!strcmp(t, "kb")) { char *a = strtok(0, " \t\r\n"), *b = strtok(0, " \t\r\n"), *c = strtok(0, " \t\r\n");
            if (a && b && c) { int lv = atoi(c); kb_cfg_height = atoi(a) % 3; kb_cfg_numrow = atoi(b) != 0; kb_cfg_layout = lang_ok(lv) ? lv : LANG_EN; got_kb = 1; } }
        else if (!strcmp(t, "kbs")) { char *v = strtok(0, " \t\r\n"); kb_cfg_layouts = v ? atoi(v) & ((1 << LANG_N) - 1) : 0; }
        else if (!strcmp(t, "look")) { char *a = strtok(0, " \t\r\n"), *b = strtok(0, " \t\r\n"), *c = strtok(0, " \t\r\n"), *d = strtok(0, " \t\r\n");
            if (a && b && c && d) { L.labels = atoi(a) != 0; L.dots = atoi(b) != 0; L.dockbg = atoi(c) != 0; L.dim = atoi(d) != 0; } }
        else if (!strcmp(t, "widgets")) wg_conf_line(strtok(0, "\r\n"));
        else if (!strcmp(t, "cc")) cc_conf_line(strtok(0, "\r\n"));
        else if (!strcmp(t, "gesture")) { char *k = strtok(0, " \t\r\n"); char *v = strtok(0, " \t\r\n"); gestures_conf_line(k, v); }
        else if (!strcmp(t, "wall")) { char *v = strtok(0, "\r\n"); if (v && strcmp(v, "-")) snprintf(L.wall, sizeof L.wall, "%s", v); }
        else if (!strcmp(t, "removed")) { while ((t = strtok(0, " \t\r\n"))) { int a = app_find(t); if (app_sys_removable(a)) app_removed[a] = 1; } }
        else if (!strcmp(t, "dock")) { got_dock = 1; while ((t = strtok(0, " \t\r\n"))) { int a = app_find(t); if (a < 0) lconf_order_migrated = 1; if (a >= 0 && !seen[a] && nd < 4) { dk[nd++] = a; seen[a] = 1; } } }
        else if (!strcmp(t, "page") && pages < MAXPAGES) {
            while ((t = strtok(0, " \t\r\n"))) { int a = app_find(t); if (a < 0) lconf_order_migrated = 1; /* app que ya no existe (p.ej. "wireless"): guardar sin ella */ if (a >= 0 && !seen[a] && npn[pages] < MAXAPPS) { np[pages][npn[pages]++] = a; seen[a] = 1; } }
            if (npn[pages]) pages++;
        }
    }
    fclose(f);
    if (!got_kb) kb_cfg_layout = kb_layout_for(ui_lang);
    /* sin "kbs" (instalacion nueva o conf anterior): el del idioma, el ingles y el que estaba elegido */
    if (!kb_cfg_layouts) kb_cfg_layouts = 1 << kb_layout_for(ui_lang) | 1 << LANG_EN | 1 << kb_cfg_layout;
    if (!(kb_cfg_layouts >> kb_cfg_layout & 1)) kb_cfg_layouts |= 1 << kb_cfg_layout;
    if (!got_dock && !pages) return;
    if (order < 2) {
        /* conf anterior al orden oficial: se queda el orden oficial (ya puesto por lconf_default)
         * y detras, en el orden en que estaban, las apps que no son del sistema (instaladas) */
        int in[40] = {0}, pp = per_page(); if (pp > MAXAPPS) pp = MAXAPPS; if (pp < 1) pp = 1;
        for (int i = 0; i < L.dn; i++) in[L.dock[i]] = 1;
        for (int i = 0; i < L.pn[0]; i++) in[L.page[0][i]] = 1;
        for (int k = 0; k < nd + pages; k++) {
            const int *v = k < nd ? &dk[k] : np[k - nd]; int n = k < nd ? 1 : npn[k - nd];
            for (int i = 0; i < n; i++) if (!in[v[i]]) {
                int p = L.npages - 1;
                if (L.pn[p] >= pp) { if (L.npages >= MAXPAGES) break; p = L.npages++; L.pn[p] = 0; }
                L.page[p][L.pn[p]++] = v[i]; in[v[i]] = 1;
            }
        }
        for (int a = 0; a < NAPPS; a++) if (!in[a]) {
            int p = L.npages - 1;
            if (L.pn[p] >= pp) { if (L.npages >= MAXPAGES) break; p = L.npages++; L.pn[p] = 0; }
            L.page[p][L.pn[p]++] = a;
        }
        lconf_order_migrated = 1;
        return;
    }
    if (order < 3 && nd < 4) {   /* orden 3: Libros pasa al dock, el primero por la izquierda (una vez, si cabe) */
        int b = app_find("books"), at = 0;
        for (int i = 0; i < nd; i++) if (dk[i] == b) at = 1;
        if (b >= 0 && !at) {
            for (int q = 0; q < pages; q++) for (int i = 0; i < npn[q]; i++) if (np[q][i] == b) {
                memmove(&np[q][i], &np[q][i + 1], (npn[q] - i - 1) * sizeof np[q][0]); npn[q]--; i--;
            }
            memmove(&dk[1], &dk[0], nd * sizeof dk[0]); dk[0] = b; nd++; seen[b] = 1;
        }
    }
    if (order < LCONF_ORDER) lconf_order_migrated = 1;
    L.dn = nd; memcpy(L.dock, dk, sizeof dk);
    L.npages = pages; memcpy(L.pn, npn, sizeof npn); memcpy(L.page, np, sizeof np);
    /* apps nuevas (no listadas) en el primer hueco libre al final */
    int pp = per_page(); if (pp > MAXAPPS) pp = MAXAPPS;
    for (int a = 0; a < NAPPS; a++) if (!seen[a] && !app_removed[a]) {
        if (!L.npages) L.npages = 1;
        int p = L.npages - 1;
        if (L.pn[p] >= pp && L.npages < MAXPAGES) { p = L.npages++; L.pn[p] = 0; }
        if (L.pn[p] < pp) L.page[p][L.pn[p]++] = a;
    }
    if (!layout2) lconf_compact();                 /* una vez: todo arriba a la izquierda */
    lconf_drop_empty_pages();
}
static void lconf_load(void)
{
    /* sin launcher.conf (apartado por la auto-reparacion o un guardado cortado): la copia buena anterior */
    if (access(LCONF, F_OK) && !access(LCONF ".bak", F_OK) && !link(LCONF ".bak", LCONF)) lg("launcher.conf recuperado de la copia anterior");
    lconf_load0(); lconf_strip_removed(); corners_push(); vol_ui_apply(); hiby_dlgs_quiet(); vl_init(); }

/* ------------------------------------------------------------------ geometria */
static int per_page(void)
{
    int rowh = L.icon + 5 + 14 + 16;
    int rows = (DOTS_Y - 8 - GRID_TOP + 16) / rowh;
    return rows * L.cols;
}
static void cell_xy(int idx, int *x, int *y)
{
    int cw = (W - 36) / L.cols, col = idx % L.cols, row = idx / L.cols;
    *x = 18 + col * cw + (cw - L.icon) / 2;
    *y = GRID_TOP + row * (L.icon + 5 + 14 + 16);
}
/* Dock como el de iOS: el cristal ABRAZA a los iconos con el mismo margen por los cuatro lados
 * (el vertical: (DOCK_H - icono) / 2) y su esquina es concentrica con la de los iconos. La
 * separacion es la que deja 4 iconos de lado a lado, la misma de la rejilla. */
static int dock_pad(void) { return (DOCK_H - L.icon) / 2; }
static int dock_gap(void) { int g = (DOCK_W - 2 * dock_pad() - 4 * L.icon) / 3; return g < 6 ? 6 : g; }
static int dock_width(int n) { int w = n > 0 ? n * L.icon + (n - 1) * dock_gap() + 2 * dock_pad() : 0; return w > DOCK_W ? DOCK_W : w; }
static void dock_xy(int idx, int n, int *x, int *y)
{
    int gap = dock_gap(), tot = n * L.icon + (n - 1) * gap;
    *x = (W - tot) / 2 + idx * (L.icon + gap);
    *y = DOCK_Y + (DOCK_H - L.icon) / 2;
}

/* ------------------------------------------------------------------ dibujo */
static void blit(uint16_t *dst, const struct sprite *s, int x, int y, int k, int alpha)
{
    if (!s || !s->w) return;
    for (int r = 0; r < s->h; r++) {
        int yy = y + r; if (yy < 0 || yy >= H) continue;
        uint16_t *row = dst + yy * W;
        const uint16_t *src = s->rgb + r * s->w; const uint8_t *al = s->a + r * s->w;
        for (int i = 0; i < s->w; i++) {
            int xx = x + i; if (xx < 0 || xx >= W) continue;
            unsigned a = al[i] * alpha / 255; if (!a) continue;
            unsigned c = k < 32 ? dim565(src[i], k) : src[i];
            if (a >= 250) { row[xx] = c; continue; }
            unsigned d = row[xx], na = 255 - a;
            row[xx] = (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
        }
    }
}
/* Primitivas suavizadas. La cobertura de cada pixel es la de siempre (distancia con signo,
 * centro del pixel), pero solo se calcula en los bordes: la distancia es convexa y simetrica
 * en cada fila, asi que desde el primer pixel lleno por la izquierda hasta su simetrico todo
 * esta lleno y se rellena sin coma flotante (alpha 255: se escribe el color tal cual). Mismo
 * resultado que el pixel a pixel, ~10x menos trabajo en las tarjetas grandes de las listas. */
#define AA_BLEND(dp, a) do { unsigned a_ = (a), na_ = 255 - a_, d0_ = *(dp); \
    *(dp) = (uint16_t)((((col >> 11) * a_ + (d0_ >> 11) * na_) / 255) << 11 | (((((col >> 5) & 63) * a_ + ((d0_ >> 5) & 63) * na_) / 255) << 5) | (((col & 31) * a_ + (d0_ & 31) * na_) / 255)); } while (0)
static void aa_span(uint16_t *row, int xa, int xb, uint16_t col, int alpha)
{
    if (xa >= xb) return;
    if (alpha >= 255) { for (int x = xa; x < xb; x++) row[x] = col; return; }
    for (int x = xa; x < xb; x++) AA_BLEND(row + x, (unsigned)alpha);
}
static void fill_circle(uint16_t *dst, float cx, float cy, float r, uint16_t col, int alpha)
{
    const int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1);
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        if (y < 0 || y >= H) continue;
        uint16_t *row = dst + y * W;
        const float dy2 = (y + .5f - cy) * (y + .5f - cy);
        #define CI_COV(x) (r + .5f - sqrtf(((x) + .5f - cx) * ((x) + .5f - cx) + dy2))
        int fl = x1 + 1, fr = x1;
        for (int x = x0; x <= x1; x++) {
            float cov = CI_COV(x); if (cov <= 0) continue;
            if (cov >= 1) { fl = x; break; }
            if (x >= 0 && x < W) AA_BLEND(row + x, (unsigned)(alpha * cov));
        }
        if (fl > x1) continue;
        for (; fr > fl; fr--) {
            float cov = CI_COV(fr); if (cov >= 1) break;
            if (cov > 0 && fr >= 0 && fr < W) AA_BLEND(row + fr, (unsigned)(alpha * cov));
        }
        #undef CI_COV
        aa_span(row, fl < 0 ? 0 : fl, fr + 1 > W ? W : fr + 1, col, alpha);
    }
}
/* rectangulo redondeado relleno (con suavizado) */
static void fill_rrect(uint16_t *dst, int x0, int y0, int w, int h, float r, uint16_t col, int alpha)
{
    if (w <= 0 || h <= 0) return;
    const float hw = w / 2.f, hh = h / 2.f;
    const int xa = x0 < 0 ? 0 : x0, xb = x0 + w > W ? W : x0 + w;
    if (xa >= xb) return;
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < H; y++) {
        uint16_t *row = dst + y * W;
        const float py = y + .5f - y0;
        const float qy = fabsf(py - hh) - (hh - r), oy = qy > 0 ? qy : 0;
        #define RR_COV(x) ({ float qx_ = fabsf((x) + .5f - x0 - hw) - (hw - r), ox_ = qx_ > 0 ? qx_ : 0; \
            .5f - (sqrtf(ox_ * ox_ + oy * oy) + (qx_ > qy ? (qx_ < 0 ? qx_ : 0) : (qy < 0 ? qy : 0)) - r); })
        int fl = x0 + w, fr = x0 + w - 1;
        for (int x = x0; x < x0 + w; x++) {           /* primer pixel lleno por la izquierda */
            float cov = RR_COV(x); if (cov <= 0) continue;
            if (cov >= 1) { fl = x; break; }
            if (x >= xa && x < xb) AA_BLEND(row + x, (unsigned)(alpha * cov));
        }
        if (fl >= x0 + w) continue;                   /* fila de borde sin pixeles llenos: ya esta */
        for (; fr > fl; fr--) {                       /* ultimo lleno por la derecha */
            float cov = RR_COV(fr); if (cov >= 1) break;
            if (cov > 0 && fr >= xa && fr < xb) AA_BLEND(row + fr, (unsigned)(alpha * cov));
        }
        #undef RR_COV
        aa_span(row, fl < xa ? xa : fl, fr + 1 > xb ? xb : fr + 1, col, alpha);
    }
}

/* ---- esquina continua de iOS (la curva de los iconos: theme/launcher_icons.js, squircle())
 * Perfil para un radio r: g[k] = y de la curva en x = k / 4 px, medido desde el vertice de la
 * esquina y solo en la rama "plana" (x >= y; la otra es su simetrica). SQ_OUT = fuera. */
#define SQ_N 208
#define SQ_OUT 1e6f
static void sq_profile(float r, float *g)
{
    static const float C[3][6] = { { 1.08849323f, 0, .86840689f, .02196619f, .63149399f, .07491100f },
                                   { .37282392f, .16905732f, .16905732f, .37282392f, .07491100f, .63149399f },
                                   { .02196619f, .86840689f, 0, 1.08849323f, 0, 1.52866483f } };
    for (int k = 0; k < SQ_N; k++) g[k] = SQ_OUT;
    float x0 = 1.52866483f * r, px = x0, py = 0;
    for (int k = 0; k < SQ_N; k++) if (k / 4.f >= x0) g[k] = 0;      /* tramo recto */
    for (int c = 0; c < 3; c++) {
        float ax = px, ay = py;
        for (int i = 1; i <= 48; i++) {
            float t = i / 48.f, m = 1 - t;
            float x = m * m * m * ax + 3 * m * m * t * C[c][0] * r + 3 * m * t * t * C[c][2] * r + t * t * t * C[c][4] * r;
            float y = m * m * m * ay + 3 * m * m * t * C[c][1] * r + 3 * m * t * t * C[c][3] * r + t * t * t * C[c][5] * r;
            if (px >= py && px > x) {                          /* x baja: se rellena [x, px] */
                for (int k = (int)ceilf(x * 4); k <= (int)floorf(px * 4); k++)
                    if (k >= 0 && k < SQ_N) g[k] = py + (y - py) * (px - k / 4.f) / (px - x);
            }
            px = x; py = y;
        }
    }
}
/* (x, y) relativo a un rectangulo w x h de esquinas continuas con perfil g (extension ext) */
static int sq_in(float x, float y, float w, float h, const float *g, float ext)
{
    if (x < 0 || y < 0 || x > w || y > h) return 0;
    float cx = x < w - x ? x : w - x, cy = y < h - y ? y : h - y;
    float u = cx > cy ? cx : cy, v = cx > cy ? cy : cx;
    if (u >= ext) return 1;
    float f = u * 4; int k = (int)f; f -= k;
    if (k + 1 >= SQ_N) return 1;
    if (g[k] >= SQ_OUT || g[k + 1] >= SQ_OUT) return 0;
    return v >= g[k] + (g[k + 1] - g[k]) * f;
}

/* Cristal del dock = fondo desenfocado + velo claro (dock_base, una vez por fondo) recortado a
 * la forma (dock_shape: una vez por numero de iconos y tamano de icono). La forma mide lo que
 * ocupan los iconos mas el margen, centrada en el sprite de DOCK_W x DOCK_H; su radio es el del
 * icono mas el margen (concentrico) y la curva, la misma esquina continua que los iconos, con
 * un filo claro de ~1.3 px algo mas vivo arriba que abajo. */
static uint16_t dock_base[DOCK_W * DOCK_H];
static int wg_page_enabled(void);            /* widgets.inc.c: la pagina de widgets existe */
static int dock_sh_n = -1, dock_sh_icon;
static void dock_shape(int n)
{
    if (n == dock_sh_n && L.icon == dock_sh_icon && dock_glass.rgb) return;
    int w = DOCK_W, h = DOCK_H;
    if (!dock_glass.rgb) { dock_glass.rgb = malloc(w * h * 2); dock_glass.a = malloc(w * h); }
    dock_glass.w = w; dock_glass.h = h;
    dock_sh_n = n; dock_sh_icon = L.icon;
    memset(dock_glass.a, 0, w * h);
    int sw = dock_width(n); if (sw <= 0) return;
    float x0 = (w - sw) / 2.f, in = 1.3f;
    float r = L.icon * 0.2237f + dock_pad();                  /* concentrico con el icono */
    float rmax = (h / 2.f) / 1.52866483f; if (r > rmax) r = rmax;   /* la curva cabe en media altura */
    static float g0[SQ_N], g1[SQ_N];
    sq_profile(r, g0); sq_profile(r - in, g1);
    float e0 = 1.52866483f * r, e1 = 1.52866483f * (r - in);
    for (int y = 0; y < h; y++) {
        int kk = 86 - 34 * y / (h - 1);                       /* filo: mas luz arriba */
        for (int x = (int)x0; x < (int)(x0 + sw + .999f) && x < w; x++) {
            int i = y * w + x; uint16_t p = dock_base[i];
            float lx = x - x0, cx = lx < sw - 1 - lx ? lx : sw - 1 - lx; int cy = y < h - 1 - y ? y : h - 1 - y;
            int no = 16, ni = 16;
            if (cx < 3 || cy < 3 || (cx < e0 + 1 && cy < e0 + 1)) {      /* borde o esquina: 4x4 muestras */
                no = ni = 0;
                for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                    float fx = lx + (sx + .5f) / 4, fy = y + (sy + .5f) / 4;
                    if (!sq_in(fx, fy, sw, h, g0, e0)) continue;
                    no++; ni += sq_in(fx - in, fy - in, sw - 2 * in, h - 2 * in, g1, e1);
                }
            }
            if (!no) continue;
            if (no > ni) {
                int k = kk * (no - ni) / no;
                int cr = (p >> 11) * 255 / 31, cg = ((p >> 5) & 63) * 255 / 63, cb = (p & 31) * 255 / 31;
                p = rgb(cr + (255 - cr) * k / 255, cg + (255 - cg) * k / 255, cb + (255 - cb) * k / 255);
            }
            dock_glass.rgb[i] = p; dock_glass.a[i] = (uint8_t)(255 * no / 16);
        }
    }
}
static void build_dock_glass(void)
{
    int w = DOCK_W, h = DOCK_H;
    static int acc[3][DOCK_W * DOCK_H];
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint16_t p = wall[(DOCK_Y + y) * W + DOCK_X + x];
        acc[0][y * w + x] = (p >> 11) << 3; acc[1][y * w + x] = ((p >> 5) & 63) << 2; acc[2][y * w + x] = (p & 31) << 3;
    }
    static int tmp[DOCK_W * DOCK_H];
    const int R = 7;
    for (int pass = 0; pass < 2; pass++) for (int c = 0; c < 3; c++) {
        for (int y = 0; y < h; y++) { int s = 0, n = 0;   /* horizontal */
            for (int x = -R; x < w; x++) {
                if (x + R < w) { s += acc[c][y * w + x + R]; n++; }
                if (x - R - 1 >= 0) { s -= acc[c][y * w + x - R - 1]; n--; }
                if (x >= 0) tmp[y * w + x] = s / n;
            } }
        for (int x = 0; x < w; x++) { int s = 0, n = 0;    /* vertical */
            for (int y = -R; y < h; y++) {
                if (y + R < h) { s += tmp[(y + R) * w + x]; n++; }
                if (y - R - 1 >= 0) { s -= tmp[(y - R - 1) * w + x]; n--; }
                if (y >= 0) acc[c][y * w + x] = s / n;
            } }
    }
    for (int i = 0; i < w * h; i++) {
        int r = acc[0][i] + (255 - acc[0][i]) * 22 / 100, g = acc[1][i] + (255 - acc[1][i]) * 22 / 100, b = acc[2][i] + (255 - acc[2][i]) * 22 / 100;
        dock_base[i] = rgb(r, g, b);
    }
    drop_pages(acc, sizeof acc); drop_pages(tmp, sizeof tmp);   /* 390 KB de trabajo: fuera */
    int n = dock_sh_n < 0 ? L.dn : dock_sh_n;
    dock_sh_n = -1; dock_shape(n);                            /* fondo nuevo: se recorta otra vez */
}

static int wall_gen;                 /* cambia con cada fondo cargado (widgets: cristal precalculado) */
static int load_wall(void)
{
    wall_gen++;
    const char *p = ASSETS "/wall_custom.raw";
    int f = L.wall[0] ? open(p, O_RDONLY) : -1;
    int def = f < 0;
    if (f < 0) f = open(ASSETS "/wall.raw", O_RDONLY);
    if (f < 0) { for (int i = 0; i < W * H; i++) wall[i] = rgb(40, 30, 90); build_dock_glass(); return 0; }
    ssize_t n = read(f, wall, sizeof wall); close(f);
    /* el fondo de serie trae en las filas 0-29 (detras de la barra de estado) una COPIA de las
     * 30-59: en un degradado se veia un corte en la fila 30. Reflejadas (29 = 30, 28 = 31...)
     * el degradado sigue sin salto. */
    if (def) for (int y = 0; y < 30; y++) memcpy(wall + y * W, wall + (59 - y) * W, W * 2);
    if (L.dim) dim_buf(wall, wall, W * H, 21);   /* "Oscurecer fondo" (~65 %) */
    build_dock_glass();
    mirror_dirty = 1; edit_bg_ok = 0;
    return n == (ssize_t)sizeof wall;
}

/* etiqueta bajo el icono: como en iOS, recortada con "…" al ancho de su celda (los nombres
 * largos como "Reproduciendo" o "Personalizar" ya no pisan a los vecinos) */
static void app_label_sprite(int a)
{
    const char *s = app_label(a);
    int maxw = (W - 36) / (L.cols > 0 ? L.cols : 4) - 4;
    char b[128]; snprintf(b, sizeof b, "%s", s);
    if (text_width(b, 12.5f) > maxw) {
        int n = (int)strlen(b);
        while (n > 0) {
            do n--; while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80);
            while (n > 0 && b[n - 1] == ' ') n--;
            b[n] = 0;
            char t[136]; snprintf(t, sizeof t, "%s…", b);
            if (text_width(t, 12.5f) <= maxw) { snprintf(b, sizeof b, "%s", t); break; }
        }
    }
    text_sprite(&apps[a].lbl, b, 12.5f, 0xFFFF, 1);
}
static int load_icons(void)
{
    static const int sizes[3] = { 48, 56, 64 };
    for (int a = 0; a < NAPPS; a++) {
        for (int s = 0; s < 3; s++) {
            free(apps[a].icon[s].rgb); free(apps[a].icon[s].a); memset(&apps[a].icon[s], 0, sizeof apps[a].icon[s]);
            char n[96]; if (apps[a].act == ACT_EXT) snprintf(n, sizeof n, "../apps/%s/icon_%d", apps[a].id + 2, sizes[s]); else snprintf(n, sizeof n, "icons/%s_%d", apps[a].id, sizes[s]);
            load_sprite(n, &apps[a].icon[s]);
        }
        app_label_sprite(a);
    }
    return 1;
}

/* estado de interaccion que afecta al dibujo */
static struct {
    int pressed_app;           /* app pulsada (oscurecida), -1 ninguna */
    float scroll;              /* desplazamiento horizontal en px (pagina * W) */
    int edit;                  /* modo edicion (temblor) */
    int drag_app, drag_x, drag_y, drag_slot_page, drag_slot, drag_dock;  /* arrastre */
} V = { -1, 0, 0, -1, 0, 0, -1, -1, -1 };

static struct sprite pill_done, pill_cfg;
static void draw_pill(uint16_t *dst, int x, int y, const struct sprite *lbl, int w)
{
    fill_rrect(dst, x, y, w, 30, 15, rgb(255, 255, 255), 64);
    blit(dst, lbl, x + (w - lbl->w) / 2, y + (30 - lbl->h) / 2, 32, 255);
}

/* posicion de una app en la pagina p al dibujar (con hueco del arrastre) */
/* Icono girado "ang" radianes alrededor de su centro, con muestreo bilineal en coma fija
 * (bordes suaves, sin saltos de pixel): el temblor del modo edicion de iOS. */
static void blit_rotated(uint16_t *dst, const struct sprite *s, int x0, int y0, float ang, int alpha)
{
    if (!s || !s->w) return;
    int w = s->w, h = s->h, pad = 3;
    float c = cosf(ang), sn = sinf(ang), cx = w / 2.f, cy = h / 2.f;
    int fc = (int)(c * 65536), fs = (int)(sn * 65536);
    for (int dy = -pad; dy < h + pad; dy++) {
        int yy = y0 + dy; if (yy < 0 || yy >= H) continue;
        uint16_t *row = dst + yy * W;
        float ry = dy + .5f - cy;
        /* coordenadas de origen en 16.16 para el primer pixel de la fila */
        float rx0 = -pad + .5f - cx;
        int sx = (int)((rx0 * c + ry * sn + cx - .5f) * 65536), sy = (int)((-rx0 * sn + ry * c + cy - .5f) * 65536);
        for (int dx = -pad; dx < w + pad; dx++, sx += fc, sy -= fs) {
            int xx = x0 + dx; if (xx < 0 || xx >= W) continue;
            int ix = sx >> 16, iy = sy >> 16;
            if (ix < -1 || iy < -1 || ix >= w || iy >= h) continue;
            unsigned fx = (sx >> 8) & 255, fy = (sy >> 8) & 255;
            unsigned acc_a = 0, ar = 0, ag = 0, ab = 0;
            for (int k = 0; k < 4; k++) {
                int px = ix + (k & 1), py = iy + (k >> 1);
                unsigned wgt = ((k & 1) ? fx : 256 - fx) * ((k >> 1) ? fy : 256 - fy);   /* 0..65536 */
                if (px < 0 || py < 0 || px >= w || py >= h || !wgt) continue;
                unsigned a = s->a[py * w + px] * wgt >> 8;                            /* 0..65280 */
                if (!a) continue;
                uint16_t p = s->rgb[py * w + px];
                acc_a += a; ar += (p >> 11) * a; ag += ((p >> 5) & 63) * a; ab += (p & 31) * a;
            }
            if (!acc_a) continue;
            unsigned aa = (acc_a >> 8) * alpha / 255; if (aa > 255) aa = 255;
            uint16_t col = (uint16_t)((ar / acc_a) << 11 | (ag / acc_a) << 5 | (ab / acc_a));
            unsigned d = row[xx], na = 255 - aa;
            row[xx] = aa >= 252 ? col : (uint16_t)((((col >> 11) * aa + (d >> 11) * na) / 255) << 11 | (((((col >> 5) & 63) * aa + ((d >> 5) & 63) * na) / 255) << 5) | (((col & 31) * aa + (d & 31) * na) / 255));
        }
    }
}

/* modo de dibujo del launcher: 0 todo, 1 sin iconos (fondo del modo edicion), 2 solo iconos */
static int rl_mode;
static void draw_app_at(uint16_t *dst, int a, int x, int y, int show_label, double t)
{
    const struct sprite *ic = &apps[a].icon[sz_idx()];
    if (rl_mode == 1) {
        if (show_label && L.labels && apps[a].lbl.w && a != V.drag_app) { const struct sprite *l = &apps[a].lbl; blit(dst, l, x + (L.icon - l->w) / 2, y + L.icon + 3, 32, 255); }
        return;
    }
    int k = (a == V.pressed_app) ? 22 : 32;
    int alpha = (a == V.drag_app) ? 64 : 255;
    if (V.edit && a != V.drag_app) {
        /* temblor de iOS: giro senoidal de +-1,5 grados (periodo ~0,26 s), desfasado por icono */
        float ang = (1.5f / (L.icon / 2.f)) * sinf((float)(t / 260.0 * 6.2831853) + a * 1.9f);   /* +-1,5 px en el borde, como los widgets */
        blit_rotated(dst, ic, x, y, ang, alpha);
    } else blit(dst, ic, x, y, k, alpha);
    if (rl_mode != 2 && show_label && L.labels && apps[a].lbl.w && alpha == 255) {
        const struct sprite *l = &apps[a].lbl;
        blit(dst, l, x + (L.icon - l->w) / 2, y + L.icon + 3, 32, 255);
    }
}

/* barra de estado transparente (ver render_launcher) sobre el fondo, filas 0-29 */
static void compose_statusbar_on(uint16_t *dst, const uint16_t *bg);
static void compose_statusbar(uint16_t *dst) { compose_statusbar_on(dst, wall); }
/* la misma barra sobre otro fondo (filas 0-29 de bg): la pagina de widgets la desenfoca */
/* la barra del reproductor (negra opaca con el texto encima) es la fuente del reloj y los
 * iconos. Tras cerrar una app propia, fr1 puede llevar aun la barra de la app (el reproductor
 * recibe de vuelta sus buferes con la imagen del shell) hasta que redibuja la suya: se usa la
 * ultima buena (si no, la barra del inicio salia negra ~0,6 s, revisor 12-11) */
static uint16_t sb_good[30 * W]; static int sb_good_ok;
static const uint16_t *sb_source(void)
{
    int black = 0, n = 0;
    for (int y = 1; y < 30; y += 4) for (int x = 2; x < W; x += 6) { uint16_t q = fr1[y * W + x]; n++; if (q < 0x0841) black++; }
    if (black * 100 >= n * 80) { memcpy(sb_good, fr1, sizeof sb_good); sb_good_ok = 1; return fr1; }
    return sb_good_ok ? sb_good : fr1;
}
static void compose_statusbar_on(uint16_t *dst, const uint16_t *bg)
{
    if (bg != dst) memcpy(dst, bg, 30 * W * 2);
    const uint16_t *src = sb_source();
    for (int y = 0; y < 30; y++) {
        uint16_t *d = dst + y * W; const uint16_t *s = src + y * W;
        int k = 22 + y * 10 / 30;
        for (int x = 0; x < W; x++) {
            uint16_t p = s[x];
            unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
            unsigned a = r * 2 > g ? r * 2 : g; if (b * 2 > a) a = b * 2;
            uint16_t bg = dim565(d[x], k);
            if (!a) { d[x] = bg; continue; }
            unsigned na = 63 - a;
            unsigned br = (bg >> 11) * na / 63 + r, bgc = ((bg >> 5) & 63) * na / 63 + g, bb = (bg & 31) * na / 63 + b;
            if (br > 31) br = 31; if (bgc > 63) bgc = 63; if (bb > 31) bb = 31;
            d[x] = (uint16_t)(br << 11 | bgc << 5 | bb);
        }
    }
}

static void render_launcher(uint16_t *dst, double t)
{
    /* fondo a pantalla completa y barra de estado TRANSPARENTE: la del reproductor es negra
     * opaca con iconos/texto encima, asi que su brillo es la cobertura (alfa premultiplicado):
     * resultado = fondo * (1 - a) + pixel. La franja se oscurece un poco para leerse siempre. */
    if (rl_mode != 2) {
        memcpy(dst + 30 * W, wall + 30 * W, (H - 30) * W * 2);
        compose_statusbar(dst);
        if (V.edit) {
            draw_pill(dst, 12, 34, &pill_cfg, 92);
            draw_pill(dst, W - 12 - 70, 34, &pill_done, 70);
        }
    }
    int top_shift = V.edit ? 22 : 0;
    /* pagina de widgets (izquierda de la primera). Desactivada en Ajustes: no existe; el rebote
     * del borde (40 px) solo deja ver el fondo, sin dibujar ni cargar nada de los widgets */
    if (V.scroll < 0 && rl_mode != 2 && wg_page_enabled()) widgets_draw(dst, -W - (int)V.scroll);
    /* paginas visibles */
    int p0 = (int)floorf(V.scroll / W);
    for (int p = p0; p <= p0 + 1; p++) {
        if (p < 0 || p >= L.npages) continue;
        int ox = p * W - (int)V.scroll;
        int n = L.pn[p], slot = 0;
        for (int i = 0; i <= n; i++) {
            /* hueco donde caeria la app arrastrada */
            if (V.drag_app >= 0 && V.drag_dock < 0 && p == V.drag_slot_page && slot == V.drag_slot) slot++;
            if (i == n) break;
            int a = L.page[p][i];
            if (a == V.drag_app) continue;
            int x, y; cell_xy(slot, &x, &y);
            draw_app_at(dst, a, x + ox, y + top_shift, 1, t);
            slot++;
        }
    }
    /* puntos de pagina */
    if (L.npages > 1 && L.dots && rl_mode != 2 && V.scroll > -W / 2) {
        int tot = L.npages * 12 - 6, x0 = (W - tot) / 2;
        float cur = V.scroll / W;
        for (int p = 0; p < L.npages; p++) {
            float d = fabsf(cur - p); if (d > 1) d = 1;
            fill_circle(dst, x0 + p * 12 + 3, DOTS_Y, 3.2f, 0xFFFF, (int)(255 - 150 * d));
        }
    }
    /* dock */
    /* el dock se va con la primera pagina hacia los widgets; sin pagina de widgets, el
     * desplazamiento es solo el rebote del borde y el dock se queda quieto (iOS; revisor 12-11) */
    int dkx = V.scroll < 0 && wg_page_enabled() ? (int)-V.scroll : 0;
    if (dkx < W) {
        int n = L.dn, slot = 0, total = n + (V.drag_app >= 0 && V.drag_dock >= 0 ? 1 : 0);
        for (int i = 0; i < n; i++) if (L.dock[i] == V.drag_app) total--;
        if (rl_mode != 2 && L.dockbg) { dock_shape(total); blit(dst, &dock_glass, DOCK_X + dkx, DOCK_Y, 32, 255); }
        for (int i = 0; i <= n; i++) {
            if (V.drag_app >= 0 && V.drag_dock >= 0 && slot == V.drag_dock) slot++;
            if (i == n) break;
            int a = L.dock[i]; if (a == V.drag_app) continue;
            int x, y; dock_xy(slot, total, &x, &y);
            draw_app_at(dst, a, x + dkx, y, 0, t);
            slot++;
        }
    }
    /* app arrastrada, sobre todo y con sombra */
    if (V.drag_app >= 0) {
        int x = V.drag_x - L.icon / 2, y = V.drag_y - L.icon / 2 - 10;
        fill_rrect(dst, x + 2, y + 6, L.icon, L.icon, L.icon * 0.2237f, 0, 70);
        blit(dst, &apps[V.drag_app].icon[sz_idx()], x, y, 32, 255);
    }
    /* flecha de musica y aviso de volumen (en modo captura la libreria no pone capas) */
    if (music_on && G.chevron && !V.edit && rl_mode != 2) blend_sprite(dst, &spr_chev, (W - spr_chev.w) / 2, CHEV_Y);
    draw_toast(dst);
}

/* Imagen "limpia" del launcher (sin aviso de volumen) en img_cache_home: el aviso y la
 * barra de estado se actualizan despues copiando y presentando solo sus filas. */
static int lclean_ok;
static int edit_bg_ok;          /* fondo guardado del modo edicion (invalidado al cambiar el inicio) */
static void launcher_present(void)
{
    static double acc_r, acc_p; static int nfr; static double t_last;
    double t0 = now_ms();
    /* modo edicion en reposo: fondo guardado (img_app) + solo los iconos que tiemblan */
    static uint32_t edit_bg_cap;
    if (V.edit && V.drag_app < 0 && V.scroll == cur_page * W && !hud_active && t0 >= toast_until) {
        if (!edit_bg_ok || edit_bg_cap != st->cap_frames) {
            rl_mode = 1; render_launcher(img_app, t0); rl_mode = 0;
            edit_bg_ok = 1; edit_bg_cap = st->cap_frames;
        }
        memcpy(fr0, img_app, HM_FRAME_BYTES);
        rl_mode = 2; render_launcher(fr0, t0); rl_mode = 0;
        lclean_ok = 0;
        double t1 = now_ms(); present(); double t2 = now_ms();
        acc_r += t1 - t0; acc_p += t2 - t1; nfr++;
        if (t2 - t_last > 3000) { if (nfr > 20) lg("perfil launcher (edicion): %d fot, dibujo %.1f ms, presentar %.1f ms", nfr, acc_r / nfr, acc_p / nfr); acc_r = acc_p = 0; nfr = 0; t_last = t2; }
        return;
    }
    edit_bg_ok = 0;
    render_launcher(img_cache_home, t0); have_cache_home = 1; lclean_ok = 1;
    memcpy(fr0, img_cache_home, HM_FRAME_BYTES);
    if (hud_active) hud_overlay(fr0);
    /* widgets: si solo cambiaron unas tarjetas (datos nuevos) y lo ultimo presentado fue esta
     * misma pagina, se presentan solo sus filas */
    static uint32_t lp_presents;
    if (wg_part_y1 > wg_part_y0 && widgets_on && V.scroll == -W && !hud_active && t0 >= toast_until + 40 && st->presents == lp_presents) { dirty_y0 = wg_part_y0; dirty_y1 = wg_part_y1; }
    wg_part_y0 = wg_part_y1 = 0;
    double t1 = now_ms();
    /* the pill is already in fr0: present()'s hook must not blend it a second time (one darker
     * pill frame per volume key on the home screen, the "pill flickering by itself") */
    { int hi = hud_in; hud_in = 1; present(); hud_in = hi; }
    lp_presents = st->presents;
    double t2 = now_ms();
    /* perfil: cada ~3 s con animacion continua, tiempo medio de dibujo y de presentacion */
    acc_r += t1 - t0; acc_p += t2 - t1; nfr++;
    if (t2 - t_last > 3000) { if (nfr > 20) lg("perfil launcher: %d fot, dibujo %.1f ms, presentar %.1f ms", nfr, acc_r / nfr, acc_p / nfr); acc_r = acc_p = 0; nfr = 0; t_last = t2; }
}
static void launcher_present_hud(void)
{
    if (!lclean_ok || V.edit || V.drag_app >= 0) { launcher_present(); return; }
    int y0 = vol_ui ? HUD_Y : VS_Y, h = vol_ui ? HUD_H : VS_H;   /* pildora de ByOS o dialogo de HiBy */
    memcpy(fr0 + y0 * W, img_cache_home + y0 * W, h * W * 2);
    if (hud_active) hud_overlay(fr0);
    { int hi = hud_in; hud_in = 1; present_rows(y0, y0 + h); hud_in = hi; }   /* drawn here, once */
}
static void launcher_statusbar(void)
{
    /* en la pagina de widgets la barra va sobre el fondo desenfocado: se recompone todo (si no,
     * la barra nitida del fondo normal aparecia sobre el oscurecido y no iban a la par) */
    if (!lclean_ok || V.scroll < 0) { launcher_present(); return; }
    compose_statusbar(img_cache_home);
    memcpy(fr0, img_cache_home, 30 * W * 2);
    { int hi = hud_in; hud_in = 1; present_rows(0, 30); hud_in = hi; }   /* the pill already in fr0 stays as is */
}

/* ------------------------------------------------------------------ entrar / salir */
static void launcher_enter_ex(int instant);
static void launcher_enter(void) { launcher_enter_ex(0); }
static void launcher_enter_ex(int instant)
{
    if (launcher_on || !launcher_enabled) return;
    lang_sys_poll();
    st->ov_on = 0; ov_cur = 0;
    cmd('C');
    double t = now_ms();
    while (!st->capture && now_ms() - t < 200) msleep(2);
    if (!st->capture) return;
    memcpy(fr0, fr0_shm, HM_FRAME_BYTES);        /* la imagen congelada (inicio de fabrica) */
    launcher_on = 1;
    V.scroll = cur_page * W; V.pressed_app = -1; V.drag_app = -1; V.edit = 0;
    if (instant) { launcher_present(); lg("launcher activo (directo)"); return; }
    /* fundido corto desde el inicio de fabrica */
    uint16_t *from = img_under, *to = img_sheet;          /* reutilizados: no hay hoja abierta */
    have_under = 0;
    memcpy(from, fr0, W * H * 2);
    render_launcher(to, now_ms());
    for (int s = 1; s <= 4; s++) {
        int a = s * 64; if (a > 255) a = 255;
        for (int i = 0; i < W * H; i++) {
            unsigned c = to[i], d = from[i], na = 255 - a;
            fr0[i] = (uint16_t)((((c >> 11) * a + (d >> 11) * na) / 255) << 11 | (((((c >> 5) & 63) * a + ((d >> 5) & 63) * na) / 255) << 5) | (((c & 31) * a + (d & 31) * na) / 255));
        }
        present();
    }
    launcher_present();
    lg("launcher activo");
}

/* imagen del launcher para la libreria (sustituye al inicio de fabrica al volver):
 * filas 0-29 = fondo sin barra (la libreria compone encima la barra transparente) */
static uint16_t *home_shm;
static void home_publish(void)
{
    if (!home_shm || !have_cache_home) return;
    memcpy(home_shm + 30 * W, img_cache_home + 30 * W, (H - 30) * W * 2);
    memcpy(home_shm, wall, 30 * W * 2);
}

/* home_publish dejando antes el inicio en su pagina de iconos: si se sale con la pagina de
 * widgets a la vista (Reproduciendo, panel -> reproductor, Musica -> biblioteca de HiBy), la
 * libreria seguia poniendo esa imagen cada vez que aparecia el inicio de fabrica, aunque la
 * pagina de widgets estuviera desactivada. */
static void launcher_park_publish(void)
{
    if (widgets_on || V.scroll != cur_page * W || !have_cache_home) {
        widgets_on = 0; V.scroll = cur_page * W;
        int h = hud_active; hud_active = 0; V.pressed_app = -1; render_launcher(img_cache_home, now_ms()); hud_active = h;
        have_cache_home = 1;
    }
    home_publish();
}

static void launcher_leave(void)
{
    if (!launcher_on) return;
    if (widgets_on || V.scroll < 0) { widgets_on = 0; V.scroll = cur_page * W; }   /* el inicio vuelve a su pagina */
    { int h = hud_active; hud_active = 0; V.pressed_app = -1; render_launcher(img_cache_home, now_ms()); hud_active = h; }
    have_cache_home = 1;
    home_publish();
    launcher_on = 0; V.edit = 0; V.drag_app = -1; V.pressed_app = -1;
    cmd('R');
    lg("launcher fuera");
}

static void mirror_save(void)
{
    if (access("/usr/data/hibymod/theme/litegui/theme3/launcher", F_OK)) return;
    float s = V.scroll; int e = V.edit, pa = V.pressed_app, da = V.drag_app, mo = music_on, ha = hud_active;
    V.scroll = 0; V.edit = 0; V.pressed_app = -1; V.drag_app = -1; music_on = 0; hud_active = 0;
    render_launcher(img_sheet, now_ms());
    V.scroll = s; V.edit = e; V.pressed_app = pa; V.drag_app = da; music_on = mo; hud_active = ha;
    have_under = 0;                                    /* img_sheet reutilizado */
    unsigned char *rgbb = malloc(W * (H - 30) * 3);
    if (!rgbb) return;
    for (int i = 0; i < W * (H - 30); i++) {
        uint16_t p = img_sheet[30 * W + i];
        rgbb[i * 3] = (uint8_t)((p >> 11) * 255 / 31); rgbb[i * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63); rgbb[i * 3 + 2] = (uint8_t)((p & 31) * 255 / 31);
    }
    stbi_write_png_compression_level = 3;
    int len = 0;
    unsigned char *png = stbi_write_png_to_mem(rgbb, W * 3, W, H - 30, 3, &len);
    free(rgbb);
    mirror_dirty = 0;
    if (!png) return;
    /* solo se escribe si cambio (la flash no se gasta en cada arranque) */
    int same = 0, f = open(MIRROR_PNG, O_RDONLY);
    if (f >= 0) {
        struct stat sb;
        if (!fstat(f, &sb) && sb.st_size == len) {
            unsigned char *old = malloc(len);
            if (old && read(f, old, len) == len && !memcmp(old, png, len)) same = 1;
            free(old);
        }
        close(f);
    }
    if (!same) {
        f = open(MIRROR_PNG ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0 && write(f, png, len) == len && !fsync(f) && !close(f)) {
            rename(MIRROR_PNG ".tmp", MIRROR_PNG);
            mirror_sig_write(img_sheet);
            lg("espejo del inicio actualizado (se vera tras reiniciar)");
        } else if (f >= 0) close(f);
    } else if (access(ASSETS "/mirror.sig", F_OK) && access(ASSETS "/mirror.next.sig", F_OK)) mirror_sig_write(img_sheet);
    STBIW_FREE(png);
}
