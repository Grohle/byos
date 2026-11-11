/* widgets.inc.c - pagina de widgets a la izquierda del inicio (como "Hoy" en iOS 17).
 *
 * Desde la primera pagina del inicio, deslizar hacia la derecha. Fondo = el del inicio;
 * widgets de iOS en tres tamanos: PEQUENO (137 x 137, dos por fila), MEDIANO (288 x 137) y
 * GRANDE (288 x 288, dos filas). Tarjetas de cristal (el fondo desenfocado de esa zona, con luz
 * de arriba y un filo claro) y, en Reproduciendo, tenidas con el color de la caratula.
 * Datos reales solamente:
 *   Baterias (HiBy + auriculares de la app Auriculares: izquierdo / derecho / estuche, o BlueZ),
 *   Reproduciendo (caratula, titulo, artista, progreso; anterior / play-pausa / siguiente con
 *   playctl), Fecha (calendario), Reloj (analogico; la hora y la fecha), Almacenamiento,
 *   Energia, Biblioteca (hibymix stats), Recien anadido (caratulas de la cache de Musica,
 *   lib.tsv), Escuchas (minutos de hoy y de la semana: los cuenta este fichero, ver "escuchas";
 *   artista mas escuchado de HISTORY_TABLE) y Mezclas (acceso a Musica).
 *
 * Modelo: lista ordenada de {tipo, tamano} (como mucho uno por tipo+tamano), linea de
 * launcher.conf "widgets page=1 battery:s date:s nowplaying:m recent:l ..." (s/m/l; se sigue
 * leyendo la antigua "widgets battery date ..."). Colocacion en flujo: los pequenos se emparejan,
 * los medianos ocupan una fila y los grandes dos.
 *
 * Rendimiento y sin parpadeos: cada widget se dibuja UNA vez en su propio bufer (sprite) a partir
 * de una copia de los datos tomada con cerrojo, con una firma (wg_sig) de lo que muestra: solo se
 * rehace si cambia. La pagina se compone en wg_cache y solo entonces se presenta; si solo cambio
 * una tarjeta se presentan solo sus filas (wg_part_y0/1, ver launcher_present). Durante el
 * deslizamiento de pagina no se rehace ningun sprite (se usa el anterior). Un hilo ligero
 * (wg_tick, 1 s) cuenta las escuchas y avisa (wg_fresh) cuando la barra de progreso avanza un
 * pixel o cambian los minutos, solo con la pagina en pantalla.
 *
 * Edicion como las apps (pulsacion larga): los widgets tiemblan (giro bilineal en coma fija de
 * cada sprite), "−" quita (encoge y se funde), "+" abre la galeria (hoja de iOS con vistas
 * previas reales de cada tamano), arrastrar reordena (los demas se apartan con animacion) y
 * "Listo" sale. Los datos se leen al entrar y cada 10 s mientras la pagina esta visible.
 * Textos: wgt() (lang/widgets_tr.inc.c, y si no esta, tr()).
 * Prueba en PC: tests/widgets_render.sh (todos los widgets y tamanos en los 13 idiomas, con
 * deteccion de textos cortados). Define WG_RENDER_ONLY para incluir solo el dibujo. */

enum { WG_BATT, WG_NOW, WG_DATE, WG_STORAGE, WG_POWER, WG_LIB, WG_NEW, WG_LISTEN, WG_CLOCK, WG_MIX, WG_N };
static const char *const wg_id[WG_N] = { "battery", "nowplaying", "date", "storage", "power", "library", "recent", "listening", "clock", "mixes" };
static const char *const wg_name[WG_N] = { "Baterías", "Reproduciendo", "Fecha", "Almacenamiento", "Energía", "Biblioteca", "Recién añadido", "Escuchas", "Reloj", "Mezclas" };
static const char *const wg_desc[WG_N] = {
    "WG_DESC_BATT", "WG_DESC_NOW", "El día de hoy y el calendario del mes.", "Espacio libre en la tarjeta SD y en el HiBy.",
    "Modo de energía, frecuencia de la pantalla y carga.", "Canciones, álbumes, artistas y favoritas.", "WG_DESC_RECENT", "WG_DESC_LISTEN",
    "WG_DESC_CLOCK", "WG_DESC_MIX" };
static const unsigned char wg_defsz[WG_N] = { 0, 1, 0, 0, 0, 0, 1, 1, 0, 0 };   /* 0 pequeno, 1 mediano, 2 grande */
static const unsigned char wg_sizes[WG_N] = { 7, 7, 3, 3, 3, 3, 7, 3, 3, 3 };   /* tamanos posibles (bit 0 P, 1 M, 2 G) */
static const char wg_szc[3] = { 's', 'm', 'l' };

#define WG_MAX 16
struct witem { unsigned char type, size; float ax, ay, sc, al; int rm; double rm_t; };
static struct witem wg_it[WG_MAX] = { { WG_BATT, 0 }, { WG_DATE, 0 }, { WG_NOW, 1 }, { WG_NEW, 1 }, { WG_LISTEN, 1 },
                                      { WG_CLOCK, 0 }, { WG_MIX, 0 }, { WG_LIB, 0 }, { WG_STORAGE, 0 } };
static int wg_n = 9;
static int wg_page_on = 1;                   /* la pagina de widgets existe (Ajustes > Inicio) */
static int widgets_on;                       /* la pagina de widgets esta en pantalla */

static int wg_has(int type, int size) { for (int i = 0; i < wg_n; i++) if (wg_it[i].type == type && wg_it[i].size == size) return 1; return 0; }
static int wg_has_type(int type) { for (int i = 0; i < wg_n; i++) if (wg_it[i].type == type) return 1; return 0; }
static int wg_size_ok(int type, int sz) { return sz >= 0 && sz < 3 && (wg_sizes[type] >> sz & 1); }
static void wg_conf_line(char *rest)
{
    wg_n = 0; char *t;
    while ((t = strtok(rest, " \t\r\n"))) { rest = 0;
        if (!strncmp(t, "page=", 5)) { wg_page_on = atoi(t + 5) != 0; continue; }
        char *c = strchr(t, ':'); int sz = -1;
        if (c) { *c = 0; sz = c[1] == 'l' ? 2 : c[1] == 'm'; }
        for (int i = 0; i < WG_N; i++) if (!strcmp(t, wg_id[i])) {
            if (sz < 0) sz = wg_defsz[i];
            while (sz > 0 && !wg_size_ok(i, sz)) sz--;
            if (!wg_has(i, sz) && wg_n < WG_MAX) { memset(&wg_it[wg_n], 0, sizeof wg_it[0]); wg_it[wg_n].type = i; wg_it[wg_n++].size = sz; }
        }
    }
}
static void wg_conf_save(FILE *f)
{
    fprintf(f, "widgets page=%d", wg_page_on);
    for (int i = 0; i < wg_n; i++) fprintf(f, " %s:%c", wg_id[wg_it[i].type], wg_szc[wg_it[i].size]);
    fprintf(f, "\n");
}

/* ------------------------------------------------------------------ textos */
#include "lang/widgets_tr.inc.c"
static const char *wgt(const char *s)
{
    const char *r = wgtr_lookup(s, ui_lang);
    if (r && *r) return r;
    if ((r = wgtr_lookup(s, LANG_EN))) return r;
    return tr(s);
}

/* ------------------------------------------------------------------ datos */
struct wg_bt { char name[40]; int pct; };
/* headphones of the Headphones app (buds.inc.c, daemon hibybudsd): battery items. hp = L / R / case
 * of the first device (medium and large widgets), hps = one merged item per device (small widget) */
struct wg_hp { int pct, chg, fam, part; };
static int buds_wg_items(int *pct, int *chg, int *fam, int *part, int max, int split);
static void buds_glyph(uint16_t *d, float cx, float cy, float s, int family, int part, int clip0, int clip1);
#define WG_NREC 6                            /* Recien anadido: albumes */
#define WG_RCS 80                            /* lado de sus caratulas en memoria */
struct wdata { int batt, charging, usb; struct wg_bt bt[3]; int nbt; struct wg_hp hp[3], hps[2]; int nhp, nhps; long long sd_free, sd_tot, in_free, in_tot; double t;
               int songs, albums, artists, favs; long long lib_bytes; char new_artist[64], new_album[96], top_artist[64], last_title[96], last_artist[64]; int top_n;
               int nrec; char rec_alb[WG_NREC][64], rec_art[WG_NREC][48]; unsigned char rec_cov[WG_NREC]; unsigned rc_gen;
               int lis_day[7]; };          /* segundos escuchados cada dia ([6] = hoy) */
static struct wdata WD, WDs;                 /* WD: la publica el hilo (con cerrojo); WDs: copia con la que se dibuja */
static pthread_mutex_t wg_mu = PTHREAD_MUTEX_INITIALIZER;
static uint16_t wg_rcov[WG_NREC][WG_RCS * WG_RCS];   /* caratulas de Recien anadido (se escriben con wg_mu) */
static volatile int wg_busy, wg_fresh;     /* hilo de datos en marcha / datos nuevos sin dibujar */
static int wg_cache_ok;
static volatile int wg_loaded;              /* el hilo ya publico datos al menos una vez */
/* reproduccion: copia propia (con caratula) y estado "optimista" tras pulsar play/pausa */
static struct np_info wg_ni; static uint16_t wg_cov[128 * 128]; static int wg_cok, wg_np_force, wg_tint = -1; static double wg_np_until;
static volatile int wg_np_drawn;            /* clave del progreso con la que se dibujo (ver wg_prog_key) */
static int wg_playing(void) { return now_ms() < wg_np_until ? wg_np_force : wg_ni.playing; }
/* clave del progreso que se ve: segundos si hay un Reproduciendo grande (muestra los tiempos), si
 * no el pixel de la barra mas ancha. Cuando cambia hay que redibujar */
static int wg_prog_key(int pos, int dur)
{
    int have = 0;
    for (int i = 0; i < wg_n; i++) if (wg_it[i].type == WG_NOW) have |= 1 << wg_it[i].size;
    if (!have || dur <= 0 || pos < 0) return 0;
    if (have & 4) return pos / 1000 + 1;
    return (int)((long long)(pos > dur ? dur : pos) * (have & 2 ? 137 : 109) / dur) + 1;
}
/* color de la caratula para tenir la tarjeta: media algo mas saturada, a luminancia ~64 */
static int wg_cover_tint(const uint16_t *c)
{
    int r = 0, g = 0, b = 0, n = 0;
    for (int y = 4; y < 128; y += 8) for (int x = 4; x < 128; x += 8) { uint16_t q = c[y * 128 + x]; r += (q >> 11) * 255 / 31; g += ((q >> 5) & 63) * 255 / 63; b += (q & 31) * 255 / 31; n++; }
    r /= n; g /= n; b /= n;
    int l = (77 * r + 151 * g + 28 * b) >> 8;
    r = l + (r - l) * 3 / 2; g = l + (g - l) * 3 / 2; b = l + (b - l) * 3 / 2;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0; if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    l = (77 * r + 151 * g + 28 * b) >> 8; if (l < 10) l = 10;
    float k = 64.f / l; if (k > 2.4f) k = 2.4f;
    r = (int)(r * k); g = (int)(g * k); b = (int)(b * k);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return r << 16 | g << 8 | b;
}

#ifndef WG_RENDER_ONLY
static void wg_bt_scan(struct wdata *n)
{
    n->nbt = 0;
    char *argv[] = { "/usr/bin/dbus-send", "--system", "--print-reply", "--dest=org.bluez", "/", "org.freedesktop.DBus.ObjectManager.GetManagedObjects", 0 };
    static char out[96 * 1024];        /* con muchos dispositivos cerca 24 KB se cortaba y faltaba el conectado */
    if (access(argv[0], X_OK) || run_capture(argv, out, sizeof out) <= 0) return;
    /* por cada objeto dev_: Alias, Connected y Percentage */
    char alias[40] = ""; int conn = 0, pct = -1, indev = 0, want = 0;   /* 1 alias, 2 conectado, 3 bateria */
    for (char *sv = 0, *l = strtok_r(out, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
        char *op = strstr(l, "object path \"");
        if (op) {
            if (indev && conn && n->nbt < 3) { snprintf(n->bt[n->nbt].name, 40, "%s", alias[0] ? alias : "Bluetooth"); n->bt[n->nbt++].pct = pct; }
            indev = strstr(op, "/dev_") != 0; alias[0] = 0; conn = 0; pct = -1; continue;
        }
        if (!indev) continue;
        if (strstr(l, "string \"Alias\"")) { want = 1; continue; }
        if (strstr(l, "string \"Connected\"")) { want = 2; continue; }
        if (strstr(l, "string \"Percentage\"")) { want = 3; continue; }
        if (want == 1) { char *q = strstr(l, "string \""); if (q) { q += 8; char *e = strchr(q, '"'); if (e) *e = 0; snprintf(alias, sizeof alias, "%s", q); } want = 0; }
        else if (want == 2) { conn = strstr(l, "boolean true") != 0; want = 0; }
        else if (want == 3) { char *q = strstr(l, "byte "); if (q) pct = atoi(q + 5); want = 0; }
    }
    if (indev && conn && n->nbt < 3) { snprintf(n->bt[n->nbt].name, 40, "%s", alias[0] ? alias : "Bluetooth"); n->bt[n->nbt++].pct = pct; }
}

/* ---- escuchas: segundos de reproduccion por dia, contados aqui (la base de HiBy no guarda
 * cuando se escucho). WLS_FILE: lineas "dia segundos" (dia = dias locales desde 1970), 14 dias;
 * se escribe cada 5 min de escucha y al cambiar de dia (un fichero de ~200 bytes) */
#define WLS_FILE "/usr/data/hibymod/listen.dat"
#define WLS_N 14
static int wls_dn[WLS_N], wls_s[WLS_N];
static pthread_mutex_t wls_mu = PTHREAD_MUTEX_INITIALIZER;
static int wls_day(time_t t) { struct tm tm; localtime_r(&t, &tm); return (int)((t + tm.tm_gmtoff) / 86400); }
static int *wls_slot(int day, int make)
{
    int old = 0;
    for (int i = 0; i < WLS_N; i++) { if (wls_dn[i] == day) return &wls_s[i]; if (wls_dn[i] < wls_dn[old]) old = i; }
    if (!make) return 0;
    wls_dn[old] = day; wls_s[old] = 0; return &wls_s[old];
}
static void wls_load(void)
{
    FILE *f = fopen(WLS_FILE, "r"); if (!f) return;
    int d, s; while (fscanf(f, "%d %d", &d, &s) == 2) { int *p = wls_slot(d, 1); if (p) *p = s; }
    fclose(f);
}
static void wls_save(const int *dn, const int *sv)
{
    FILE *f = fopen(WLS_FILE ".tmp", "w"); if (!f) return;
    for (int i = 0; i < WLS_N; i++) if (dn[i] && sv[i]) fprintf(f, "%d %d\n", dn[i], sv[i]);
    if (fclose(f) == 0) rename(WLS_FILE ".tmp", WLS_FILE); else unlink(WLS_FILE ".tmp");
}
static void wls_week(int *o)
{
    int today = wls_day(time(0));
    pthread_mutex_lock(&wls_mu);
    for (int k = 0; k < 7; k++) { int *p = wls_slot(today - 6 + k, 0); o[k] = p ? *p : 0; }
    pthread_mutex_unlock(&wls_mu);
}
/* hilo de 1 s: cuenta las escuchas y, con la pagina en pantalla, avisa cuando el progreso o los
 * minutos de hoy cambian lo que se ve (no dibuja nada) */
static void *wg_tick(void *a)
{
    (void)a; thread_nice(10);
    pthread_mutex_lock(&wls_mu); wls_load(); pthread_mutex_unlock(&wls_mu);
    double last = now_ms(), acc = 0; int unsaved = 0, day = wls_day(time(0)), lmin = -1;
    for (;;) {
        msleep(1000);
        double t = now_ms(), dt = t - last; last = t; if (dt < 0 || dt > 3000) dt = 0;
        pthread_mutex_lock(&npx_mu);
        int pl = npx.source && npx.playing && !npx.stopped, pos = npx.position_ms, dur = npx.duration_ms;
        pthread_mutex_unlock(&npx_mu);
        int d = wls_day(time(0)), dn[WLS_N], sv[WLS_N], save = 0;
        pthread_mutex_lock(&wls_mu);
        if (pl) { acc += dt; int s = (int)(acc / 1000); if (s > 0) { acc -= s * 1000.0; int *p = wls_slot(d, 1); if (p) *p += s; unsaved += s; } }
        int *pt = wls_slot(d, 0), mins = pt ? *pt / 60 : 0;
        if (unsaved && (unsaved >= 300 || d != day)) { memcpy(dn, wls_dn, sizeof dn); memcpy(sv, wls_s, sizeof sv); save = 1; unsaved = 0; }
        pthread_mutex_unlock(&wls_mu);
        if (save) wls_save(dn, sv);
        day = d;
        if (widgets_on) {
            if (wg_prog_key(pos, dur) != wg_np_drawn) wg_fresh = 1;
            if (mins != lmin && wg_has_type(WG_LISTEN)) wg_fresh = 1;
        }
        lmin = mins;
    }
    return 0;
}

/* ---- Recien anadido: los WG_NREC albumes mas nuevos de la cache de Musica (lib.tsv, se lee
 * por lineas: no se carga entera) y sus caratulas ya generadas por Musica (_m.raw, 140 px) */
static int wg_tabs(char *l, char **f, int max)
{
    int n = 0; f[n++] = l;
    for (char *p = l; *p && n < max; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    for (int i = 0; i < n; i++) f[i][strcspn(f[i], "\r\n")] = 0;
    return n;
}
static void wg_recent_scan(struct wdata *n)
{
    static long lt_m = -1, lt_sz = -1; static char rpath[WG_NREC][400];
    struct stat s;
    if (stat(MU_CACHE "/lib.tsv", &s)) { n->nrec = 0; lt_m = -1; return; }
    if ((long)s.st_mtime != lt_m || (long)s.st_size != lt_sz) {
        FILE *f = fopen(MU_CACHE "/lib.tsv", "r"); if (!f) return;
        static char l[2048]; int idx[WG_NREC], nr = 0, ai = 0; long ct[WG_NREC];
        while (fgets(l, sizeof l, f)) {            /* 1a pasada: albumes (A titulo artista ano ? ctime genero) */
            if (l[0] != 'A') continue;
            char *fl[8]; int k = wg_tabs(l, fl, 8), me = ai++;
            if (k < 7) continue;
            long c = atol(fl[5]); int pos = nr;
            while (pos > 0 && ct[pos - 1] < c) pos--;
            if (pos >= WG_NREC) continue;
            if (nr < WG_NREC) nr++;
            for (int j = nr - 1; j > pos; j--) { ct[j] = ct[j - 1]; idx[j] = idx[j - 1]; memcpy(n->rec_alb[j], n->rec_alb[j - 1], 64); memcpy(n->rec_art[j], n->rec_art[j - 1], 48); }
            ct[pos] = c; idx[pos] = me;
            snprintf(n->rec_alb[pos], 64, "%s", fl[1]); snprintf(n->rec_art[pos], 48, "%s", fl[2]);
        }
        for (int j = 0; j < nr; j++) rpath[j][0] = 0;
        rewind(f); int left = nr;
        while (left && fgets(l, sizeof l, f)) {    /* 2a pasada: primera pista de cada uno (T album ... ruta) */
            if (l[0] != 'T') continue;
            char *fl[9]; int k = wg_tabs(l, fl, 9); if (k < 8) continue;
            int a = atoi(fl[1]);
            for (int j = 0; j < nr; j++) if (idx[j] == a && !rpath[j][0]) { snprintf(rpath[j], sizeof rpath[j], "%s", fl[7]); left--; }
        }
        fclose(f);
        n->nrec = nr; memset(n->rec_cov, 0, sizeof n->rec_cov); n->rc_gen++;
        lt_m = (long)s.st_mtime; lt_sz = (long)s.st_size;
    }
    static uint16_t big[140 * 140], sm[WG_RCS * WG_RCS];
    for (int j = 0; j < n->nrec; j++) {
        if (n->rec_cov[j] || !rpath[j][0]) continue;
        char sys[440], dir[440], fn[160]; mu_syspath(rpath[j], sys, sizeof sys);
        uint32_t k = mu_cover_key(sys, dir, sizeof dir);
        snprintf(fn, sizeof fn, MU_CACHE "/%08x_m.raw", k);
        if (mu_read_raw(fn, big, 140)) continue;            /* aun sin caratula (Musica la genera): se reintenta */
        mu_scale(big, 140, sm, WG_RCS);
        pthread_mutex_lock(&wg_mu); memcpy(wg_rcov[j], sm, sizeof sm); pthread_mutex_unlock(&wg_mu);
        n->rec_cov[j] = 1; n->rc_gen++;
    }
}

static void wg_refresh_now(void)
{
    static struct wdata n;                 /* se rellena aparte y se publica de golpe */
    static long db_m = -1, db_sz = -1; static double db_t;
    pthread_mutex_lock(&wg_mu); n = WD; pthread_mutex_unlock(&wg_mu);
    n.batt = sysfs_get("/sys/class/power_supply/battery/capacity", -1);
    char st[24] = ""; int f = open("/sys/class/power_supply/battery/status", O_RDONLY);
    if (f >= 0) { int k = read(f, st, sizeof st - 1); if (k > 0) st[k] = 0; close(f); }
    n.charging = !strncmp(st, "Charging", 8); n.usb = sysfs_get(USB_ONLINE, 0);
    struct statvfs v;
    if (!statvfs("/usr/data/mnt/sd_0", &v) && v.f_blocks) { n.sd_free = (long long)v.f_bavail * v.f_frsize; n.sd_tot = (long long)v.f_blocks * v.f_frsize; } else n.sd_tot = n.sd_free = 0;
    if (!statvfs("/usr/data", &v)) { n.in_free = (long long)v.f_bavail * v.f_frsize; n.in_tot = (long long)v.f_blocks * v.f_frsize; }
    {   /* Bluetooth de BlueZ solo si la app Auriculares no ve ninguno (dbus-send cuesta) */
        int p[3], c[3], fa[3], k[3];
        if (buds_wg_items(p, c, fa, k, 1, 0) > 0) n.nbt = 0; else wg_bt_scan(&n);
    }
    {   /* biblioteca de HiBy (hibymix stats: solo lectura), solo si la base cambio (o cada 5 min) */
        struct stat s; long m = 0, sz = 0; if (!stat(MU_DB, &s)) { m = (long)s.st_mtime; sz = (long)s.st_size; }
        char *argv[] = { "/usr/data/hibymod/shell/hibymix", "stats", 0 };
        static char out[2048];
        if ((m != db_m || sz != db_sz || now_ms() - db_t > 300000) && !access(argv[0], X_OK) && run_capture(argv, out, sizeof out) > 0) {
            db_m = m; db_sz = sz; db_t = now_ms();
            for (char *sv = 0, *l = strtok_r(out, "\n", &sv); l; l = strtok_r(0, "\n", &sv)) {
                char *fl[6]; int k = split_tabs(l, fl, 6);
                if (!strcmp(fl[0], "count") && k >= 6) { n.songs = atoi(fl[1]); n.albums = atoi(fl[2]); n.artists = atoi(fl[3]); n.favs = atoi(fl[4]); n.lib_bytes = atoll(fl[5]); }
                else if (!strcmp(fl[0], "recent") && k >= 3) { snprintf(n.new_artist, sizeof n.new_artist, "%s", fl[1]); snprintf(n.new_album, sizeof n.new_album, "%s", fl[2]); }
                else if (!strcmp(fl[0], "top") && k >= 3) { snprintf(n.top_artist, sizeof n.top_artist, "%s", fl[1]); n.top_n = atoi(fl[2]); }
                else if (!strcmp(fl[0], "last") && k >= 3) { snprintf(n.last_title, sizeof n.last_title, "%s", fl[1]); snprintf(n.last_artist, sizeof n.last_artist, "%s", fl[2]); }
            }
        }
    }
    if (wg_has_type(WG_NEW)) wg_recent_scan(&n);
    pthread_mutex_lock(&wg_mu); n.t = WD.t; WD = n; pthread_mutex_unlock(&wg_mu);
}
static void *wg_thread(void *a)
{
    (void)a; thread_nice(8);
    double t0 = now_ms(); wg_refresh_now();
    if (!wg_loaded) lg("widgets: primeros datos en %.0f ms", now_ms() - t0);
    wg_loaded = 1; wg_fresh = 1; wg_busy = 0; return 0;
}
static void wg_refresh(void)            /* asincrono */
{
    static int tick_on;
    if (!tick_on) { tick_on = 1; pthread_t th; if (!pthread_create(&th, 0, wg_tick, 0)) pthread_detach(th); }
    if (wg_busy) return;
    wg_busy = 1; WD.t = now_ms();
    pthread_t th; if (pthread_create(&th, 0, wg_thread, 0)) { wg_busy = 0; return; } pthread_detach(th);
}
static void wg_snap(void)
{
    pthread_mutex_lock(&wg_mu); double t = WDs.t; WDs = WD; WDs.t = t; pthread_mutex_unlock(&wg_mu);
    if (!wg_loaded) {   /* aun sin datos del hilo: al menos la bateria (sysfs, inmediato), nunca "0 %" */
        WDs.batt = sysfs_get("/sys/class/power_supply/battery/capacity", -1); WDs.usb = sysfs_get(USB_ONLINE, 0);
        char st[24] = ""; int f = open("/sys/class/power_supply/battery/status", O_RDONLY);
        if (f >= 0) { int k = read(f, st, sizeof st - 1); if (k > 0) st[k] = 0; close(f); }
        WDs.charging = !strncmp(st, "Charging", 8);
    }
    uint32_t tid = wg_ni.track_id; int cok = wg_cok;
    npx_snapshot(&wg_ni, wg_cov, &wg_cok);
    if (wg_ni.track_id != tid || wg_cok != cok || wg_tint == -1) wg_tint = wg_cok ? wg_cover_tint(wg_cov) : -2;
    wg_np_drawn = wg_prog_key(wg_ni.position_ms, wg_ni.duration_ms);
    {   /* headphones: copied from the Headphones app's snapshot (no I/O; it changes wg_fresh) */
        int p[3], c[3], f[3], k[3];
        WDs.nhp = buds_wg_items(p, c, f, k, 3, 1);
        for (int i = 0; i < WDs.nhp; i++) { WDs.hp[i].pct = p[i]; WDs.hp[i].chg = c[i]; WDs.hp[i].fam = f[i]; WDs.hp[i].part = k[i]; }
        WDs.nhps = buds_wg_items(p, c, f, k, 1, 0);
        for (int i = 0; i < WDs.nhps; i++) { WDs.hps[i].pct = p[i]; WDs.hps[i].chg = c[i]; WDs.hps[i].fam = f[i]; WDs.hps[i].part = k[i]; }
    }
    wls_week(WDs.lis_day);
}
#endif /* WG_RENDER_ONLY */

/* ------------------------------------------------------------------ geometria */
#define WG_M 16                              /* margen lateral */
#define WG_G 14                              /* separacion */
#define WG_S ((W - 2 * WG_M - WG_G) / 2)     /* pequeno: 137 x 137 */
#define WG_MW (W - 2 * WG_M)                 /* mediano: 288 x 137 */
#define WG_LH (2 * WG_S + WG_G)              /* grande: 288 x 288 */
#define WG_R 20.f                            /* radio (iOS: 22 pt en 158) */
#define WG_TOP 44
#define WG_ESH 30                            /* desplazamiento del contenido en edicion (pildoras) */
#define WG_SH WG_LH                          /* filas del bufer de dibujo */
#define WG_TH 60                             /* miniatura de la galeria */
static int wg_w(int size) { return size ? WG_MW : WG_S; }
static int wg_h(int size) { return size == 2 ? WG_LH : WG_S; }
/* flujo: los pequenos se emparejan, los medianos ocupan la fila, los grandes dos; devuelve el alto */
static int wg_layout(const unsigned char *sz, int n, short *xs, short *ys)
{
    int col = 0, y = 0;
    for (int i = 0; i < n; i++) {
        if (!sz[i]) {
            xs[i] = col ? WG_M + WG_S + WG_G : WG_M; ys[i] = y;
            if (col) { col = 0; y += WG_S + WG_G; } else col = 1;
        } else {
            if (col) { col = 0; y += WG_S + WG_G; }
            xs[i] = WG_M; ys[i] = y; y += wg_h(sz[i]) + WG_G;
        }
    }
    if (col) y += WG_S + WG_G;
    return y;
}
static int wg_layout_items(short *xs, short *ys)
{
    unsigned char sz[WG_MAX]; for (int i = 0; i < wg_n; i++) sz[i] = wg_it[i].size;
    return wg_layout(sz, wg_n, xs, ys);
}

/* ------------------------------------------------------------------ mascaras y cristal */
static uint8_t *wg_mask[4], *wg_rim[3], *wg_shadow[3];     /* [0] pequeno, [1] mediano, [2] grande, [3] miniatura */
static uint8_t *wg_rrmask(int w, int h, float r, float inset)
{
    uint8_t *m = malloc(w * h); if (!m) return 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float ww = w - 2 * inset, hh = h - 2 * inset, rr = r - inset;
        float px = x + .5f - inset, py = y + .5f - inset, qx = fabsf(px - ww / 2.f) - (ww / 2.f - rr), qy = fabsf(py - hh / 2.f) - (hh / 2.f - rr);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - rr;
        float c = 0.5f - d; m[y * w + x] = (uint8_t)(c <= 0 ? 0 : c >= 1 ? 255 : c * 255);
    }
    return m;
}
#define WG_SP 12                             /* margen de la sombra del widget arrastrado */
static void wg_masks_init(void)
{
    if (wg_mask[0]) return;
    for (int s = 0; s < 3; s++) {
        int w = wg_w(s), h = wg_h(s);
        wg_mask[s] = wg_rrmask(w, h, WG_R, 0);
        uint8_t *in = wg_rrmask(w, h, WG_R, 1);
        wg_rim[s] = malloc(w * h);
        if (wg_rim[s] && in && wg_mask[s]) for (int i = 0; i < w * h; i++) { int k = wg_mask[s][i] - in[i]; wg_rim[s][i] = k < 0 ? 0 : k; }
        free(in);
    }
    wg_mask[3] = wg_rrmask(WG_TH, WG_TH, 13, 0);
}
/* sombra suave del widget levantado: mascara desenfocada (dos cajas de 9) en un lienzo con
 * margen; se hace la primera vez que se levanta uno de ese tamano */
static const uint8_t *wg_shadow_get(int s)
{
    if (wg_shadow[s] || !wg_mask[s]) return wg_shadow[s];
    int w = wg_w(s), h = wg_h(s), sw = w + 2 * WG_SP, sh = h + 2 * WG_SP;
    uint8_t *a = calloc(sw * sh, 1), *b = calloc(sw * sh, 1);
    if (a && b) {
        for (int y = 0; y < h; y++) memcpy(a + (y + WG_SP) * sw + WG_SP, wg_mask[s] + y * w, w);
        for (int pass = 0; pass < 2; pass++) {
            for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) { int t = 0; for (int k = -4; k <= 4; k++) { int xx = x + k; if (xx >= 0 && xx < sw) t += a[y * sw + xx]; } b[y * sw + x] = t / 9; }
            for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) { int t = 0; for (int k = -4; k <= 4; k++) { int yy = y + k; if (yy >= 0 && yy < sh) t += b[yy * sw + x]; } a[y * sw + x] = t / 9; }
        }
        wg_shadow[s] = a; a = 0;
    }
    free(a); free(b);
    return wg_shadow[s];
}
/* fondo desenfocado 40x60 (bloques de 8x8) del fondo actual */
static uint8_t wg_bl[61][41][3]; static int wg_bl_gen = -1;
static void wg_blur_build(void)
{
    for (int by = 0; by < 60; by++) for (int bx = 0; bx < 40; bx++) {
        int r = 0, g = 0, b = 0;
        for (int y = 0; y < 8; y += 2) for (int x = 0; x < 8; x += 2) { uint16_t q = wall[(by * 8 + y) * W + bx * 8 + x]; r += q >> 11; g += (q >> 5) & 63; b += q & 31; }
        r = r * 255 / 496; g = g * 255 / 1008; b = b * 255 / 496;
        int l = (77 * r + 151 * g + 28 * b) >> 8;          /* 25 % desaturado */
        wg_bl[by][bx][0] = (uint8_t)((r * 192 + l * 64) >> 8); wg_bl[by][bx][1] = (uint8_t)((g * 192 + l * 64) >> 8); wg_bl[by][bx][2] = (uint8_t)((b * 192 + l * 64) >> 8);
    }
    for (int by = 0; by < 60; by++) memcpy(wg_bl[by][40], wg_bl[by][39], 3);
    memcpy(wg_bl[60], wg_bl[59], sizeof wg_bl[0]);
    wg_bl_gen = wall_gen;
}
/* cristal: fondo desenfocado de esa zona de la pantalla, oscurecido (el texto blanco se lee
 * siempre) con luz de arriba (un poco mas claro arriba que abajo) y un filo claro de 1 px, mas
 * marcado arriba. tint >= 0 (0xRRGGBB): se tine con ese color (caratula, mezcla) */
static void wg_card_bg(uint16_t *d, int w, int h, int sx, int sy, int szi, int tint)
{
    if (wg_bl_gen != wall_gen) wg_blur_build();
    const uint8_t *rim = szi >= 0 ? wg_rim[szi] : 0;
    int tc[3] = { (tint >> 16) & 255, (tint >> 8) & 255, tint & 255 }, ta = tint >= 0 ? 176 : 0;
    for (int y = 0; y < h; y++) {
        int Y = sy + y; if (Y < 0) Y = 0; if (Y >= H) Y = H - 1;
        int f = (Y * 2 + 1) * 16 - 128; if (f < 0) f = 0; int y0 = f >> 8, wy = f & 255;
        int fy = y * 256 / (h > 1 ? h - 1 : 1), k = 84 - (22 * fy >> 8), base = 27 - (9 * fy >> 8);
        int tk = 290 - (96 * fy >> 8);                          /* tinte: 1,13 arriba -> 0,76 abajo */
        int rimk = y < h / 3 ? 62 : y > 2 * h / 3 ? 26 : 40;
        int trow[3]; for (int c = 0; c < 3; c++) { int v = tc[c] * tk >> 8; trow[c] = v > 255 ? 255 : v; }
        uint16_t *row = d + y * W;
        for (int x = 0; x < w; x++) {
            int X = sx + x; if (X < 0) X = 0; if (X >= W) X = W - 1;
            int fx = (X * 2 + 1) * 16 - 128; if (fx < 0) fx = 0; int x0 = fx >> 8, wx = fx & 255, c[3];
            for (int q = 0; q < 3; q++) {
                int top = wg_bl[y0][x0][q] * (256 - wx) + wg_bl[y0][x0 + 1][q] * wx, bot = wg_bl[y0 + 1][x0][q] * (256 - wx) + wg_bl[y0 + 1][x0 + 1][q] * wx;
                int v = (top * (256 - wy) + bot * wy) >> 16;
                c[q] = (v * k >> 8) + base;
                if (ta) c[q] = (c[q] * (256 - ta) + trow[q] * ta) >> 8;
            }
            int e = rim ? rim[y * w + x] : 0;
            if (e) { e = e * rimk / 255; for (int q = 0; q < 3; q++) c[q] += (255 - c[q]) * e / 255; }
            row[x] = rgb(c[0], c[1], c[2]);
        }
    }
}

/* ------------------------------------------------------------------ primitivas */
static int wg_cy0 = 0, wg_cy1 = WG_SH;      /* recorte vertical del texto (bufer de dibujo o pantalla) */
static uint16_t wg_scr[W * WG_SH];           /* bufer de dibujo de un widget (paso W) */
/* mezcla 565 sin divisiones (alfa 0..255 -> 0..32, truco de los campos separados) */
static inline uint16_t wg_mix(uint16_t c, uint16_t d, unsigned a)
{
    unsigned k = (a * 33) >> 8; if (k >= 32) return c; if (!k) return d;
    uint32_t cs = (c | ((uint32_t)c << 16)) & 0x07E0F81Fu, ds = (d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
    uint32_t v = ((cs * k + ds * (32 - k)) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(v | (v >> 16));
}
static inline uint32_t wg_sp(uint16_t p) { return (p | ((uint32_t)p << 16)) & 0x07E0F81Fu; }
static inline void wg_px(uint16_t *d, int x, int y, uint16_t c, int a)
{
    if (x < 0 || x >= W || y < wg_cy0 || y >= wg_cy1 || a <= 0) return;
    uint16_t *p = d + y * W + x; *p = a >= 255 ? c : mix565(c, *p, (unsigned)a);
}
/* arco de grosor th desde el angulo "start" (0 = las 12, horario) de longitud "len", con
 * extremos redondeados y suavizado; track_a > 0 pinta tambien la pista tenue completa */
static void wg_arc(uint16_t *d, float cx, float cy, float r, float th, float start, float len, uint16_t col, int track_a)
{
    const float TAU = 6.2831853f;
    float ht = th / 2; int full = len >= TAU - 0.001f, none = len <= 0.001f;
    float ex = cx + r * sinf(start + len), ey = cy - r * cosf(start + len), bx = cx + r * sinf(start), by = cy - r * cosf(start);
    for (int y = (int)floorf(cy - r - ht - 1); y <= (int)ceilf(cy + r + ht + 1); y++)
        for (int x = (int)floorf(cx - r - ht - 1); x <= (int)ceilf(cx + r + ht + 1); x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, dd = sqrtf(dx * dx + dy * dy);
            float cov = ht + .5f - fabsf(dd - r); if (cov <= 0) continue; if (cov > 1) cov = 1;
            float ca = 0;
            if (full) ca = cov;
            else if (!none) {
                float ang = atan2f(dx, -dy) - start; while (ang < 0) ang += TAU; while (ang >= TAU) ang -= TAU;
                if (ang <= len) ca = cov;
                else {
                    float d1 = sqrtf((x + .5f - ex) * (x + .5f - ex) + (y + .5f - ey) * (y + .5f - ey));
                    float d2 = sqrtf((x + .5f - bx) * (x + .5f - bx) + (y + .5f - by) * (y + .5f - by));
                    float c1 = ht + .5f - (d1 < d2 ? d1 : d2); if (c1 > 1) c1 = 1; if (c1 > ca) ca = c1;
                }
            }
            if (track_a && ca < 1) wg_px(d, x, y, col, (int)(track_a * cov));
            if (ca > 0) wg_px(d, x, y, col, (int)(255 * ca));
        }
}
static void wg_ring(uint16_t *d, float cx, float cy, float r, float th, float frac, uint16_t col)
{
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    wg_arc(d, cx, cy, r, th, 0, frac * 6.2831853f, col, 64);
}
/* segmento con extremos redondeados (agujas, marcas, flechas) */
static void wg_line(uint16_t *d, float x0, float y0, float x1, float y1, float th, uint16_t col, int alpha)
{
    float r = th / 2, vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy;
    int xa = (int)floorf(fminf(x0, x1) - r - 1), xb = (int)ceilf(fmaxf(x0, x1) + r + 1), ya = (int)floorf(fminf(y0, y1) - r - 1), yb = (int)ceilf(fmaxf(y0, y1) + r + 1);
    for (int y = ya; y <= yb; y++) for (int x = xa; x <= xb; x++) {
        float px = x + .5f - x0, py = y + .5f - y0, t = l2 > 0 ? (px * vx + py * vy) / l2 : 0;
        if (t < 0) t = 0; if (t > 1) t = 1;
        float dx = px - t * vx, dy = py - t * vy, cov = r + .5f - sqrtf(dx * dx + dy * dy);
        if (cov <= 0) continue; if (cov > 1) cov = 1;
        wg_px(d, x, y, col, (int)(alpha * cov));
    }
}
/* poligono relleno con 4x4 submuestras (rayo, glifos pequenos); pts en unidades de "s" */
static void wg_poly(uint16_t *d, float x0, float y0, float s, const float *pts, int n, uint16_t col)
{
    for (int y = (int)y0 - 1; y <= (int)(y0 + s) + 1; y++) for (int x = (int)x0 - 1; x <= (int)(x0 + s) + 1; x++) {
        int hit = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float px = (x + (sx + .5f) / 4 - x0) / s, py = (y + (sy + .5f) / 4 - y0) / s; int in = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
                if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
            }
            hit += in;
        }
        if (hit) wg_px(d, x, y, col, hit * 255 / 16);
    }
}
/* rectangulo redondeado con degradado vertical (azulejos de las mezclas, huecos de caratula) */
static void wg_rrect_grad(uint16_t *d, int x0, int y0, int w, int h, float r, uint16_t top, uint16_t bot)
{
    for (int y = 0; y < h; y++) {
        int Y = y0 + y; if (Y < wg_cy0 || Y >= wg_cy1 || Y < 0 || Y >= H) continue;
        uint16_t c = wg_mix(bot, top, (unsigned)(y * 255 / (h > 1 ? h - 1 : 1)));
        float py = y + .5f, qy = fabsf(py - h / 2.f) - (h / 2.f - r), oy = qy > 0 ? qy : 0;
        for (int x = 0; x < w; x++) {
            int X = x0 + x; if (X < 0 || X >= W) continue;
            float qx = fabsf(x + .5f - w / 2.f) - (w / 2.f - r);
            if (qx <= -.5f || qy <= -.5f) { d[Y * W + X] = c; continue; }     /* (x, y) dentro de la caja: solo las esquinas recortan */
            float ox = qx > 0 ? qx : 0, dd = sqrtf(ox * ox + oy * oy) - r, cv = .5f - dd;
            if (cv <= 0) continue; d[Y * W + X] = cv >= 1 ? c : mix565(c, d[Y * W + X], (unsigned)(cv * 255));
        }
    }
}
/* imagen cuadrada ss x ss escalada (bilineal) a s x s en (x, y), esquinas suavizadas de radio r */
static void wg_cover_blit(uint16_t *d, const uint16_t *src, int ss, int x, int y, int s, float r)
{
    for (int yy = 0; yy < s; yy++) {
        int Y = y + yy; if (Y < wg_cy0 || Y >= wg_cy1 || Y < 0 || Y >= H) continue;
        int fy = (int)(((yy + .5f) * ss / s - .5f) * 256); if (fy < 0) fy = 0; int v0 = fy >> 8, wv = (fy & 255) >> 3; if (v0 >= ss - 1) { v0 = ss - 2; wv = 31; }
        float py = yy + .5f, qy = fabsf(py - s / 2.f) - (s / 2.f - r), oy = qy > 0 ? qy : 0;
        for (int xx = 0; xx < s; xx++) {
            int X = x + xx; if (X < 0 || X >= W) continue;
            int fx = (int)(((xx + .5f) * ss / s - .5f) * 256); if (fx < 0) fx = 0; int u0 = fx >> 8, wu = (fx & 255) >> 3; if (u0 >= ss - 1) { u0 = ss - 2; wu = 31; }
            const uint16_t *p = src + v0 * ss + u0;
            uint32_t a = ((wg_sp(p[0]) * (32 - wu) + wg_sp(p[1]) * wu) >> 5) & 0x07E0F81Fu, b = ((wg_sp(p[ss]) * (32 - wu) + wg_sp(p[ss + 1]) * wu) >> 5) & 0x07E0F81Fu;
            uint32_t v = ((a * (32 - wv) + b * wv) >> 5) & 0x07E0F81Fu; uint16_t c = (uint16_t)(v | (v >> 16));
            float qx = fabsf(xx + .5f - s / 2.f) - (s / 2.f - r);
            if (qx <= -.5f || qy <= -.5f) { d[Y * W + X] = c; continue; }
            float ox = qx > 0 ? qx : 0, dd = sqrtf(ox * ox + oy * oy) - r, cv = .5f - dd;
            if (cv <= 0) continue; d[Y * W + X] = cv >= 1 ? c : mix565(c, d[Y * W + X], (unsigned)(cv * 255));
        }
    }
}

/* ---- glifos (todos vectoriales, mismo trazo) ---- */
static void wg_bolt(uint16_t *d, float x, float y, float h, uint16_t col)
{
    static const float b[] = { .62f, 0, .06f, .58f, .44f, .58f, .34f, 1, .94f, .40f, .56f, .40f, .70f, 0 };
    wg_poly(d, x, y, h, b, 7, col);
}
static void wg_glyph_player(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    fill_rrect(d, (int)(cx - s * .3f), (int)(cy - s * .46f), (int)(s * .6f), (int)(s * .92f), s * .13f, col, 255);
    fill_rrect(d, (int)(cx - s * .2f), (int)(cy - s * .36f), (int)(s * .4f), (int)(s * .26f), 1.5f, rgb(28, 28, 32), 255);
    fill_circle(d, cx, cy + s * .2f, s * .15f, rgb(28, 28, 32), 255);
}
static void wg_glyph_phones(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    wg_arc(d, cx, cy + s * .05f, s * .34f, s * .1f, 4.712389f, 3.14159265f, col, 0);
    for (int k = -1; k <= 1; k += 2)
        fill_rrect(d, (int)(cx + k * s * .34f - s * .12f), (int)(cy + s * .02f), (int)(s * .24f), (int)(s * .36f), s * .08f, col, 255);
}
static void wg_note(uint16_t *d, float cx, float cy, float s, uint16_t col)   /* corchea doble (music.note) */
{
    fill_circle(d, cx - s * .26f, cy + s * .26f, s * .15f, col, 255);
    fill_circle(d, cx + s * .2f, cy + s * .16f, s * .15f, col, 255);
    wg_line(d, cx - s * .14f, cy + s * .26f, cx - s * .14f, cy - s * .32f, s * .075f, col, 255);
    wg_line(d, cx + s * .32f, cy + s * .16f, cx + s * .32f, cy - s * .42f, s * .075f, col, 255);
    wg_line(d, cx - s * .14f, cy - s * .32f, cx + s * .32f, cy - s * .42f, s * .13f, col, 255);
}
static void wg_play_glyph(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    static const float t[] = { .1f, .02f, .96f, .5f, .1f, .98f };
    wg_poly(d, cx - s * .46f, cy - s * .5f, s, t, 3, col);
}
static void wg_pause_glyph(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    float bw = s * .3f, g = s * .2f;
    fill_rrect(d, (int)roundf(cx - g / 2 - bw), (int)roundf(cy - s / 2), (int)roundf(bw), (int)roundf(s), bw * .3f, col, 255);
    fill_rrect(d, (int)roundf(cx + g / 2), (int)roundf(cy - s / 2), (int)roundf(bw), (int)roundf(s), bw * .3f, col, 255);
}
static void wg_skip_glyph(uint16_t *d, float cx, float cy, float s, uint16_t col, int dir)   /* forward.fill / backward.fill */
{
    static const float f[] = { 0, .19f, .5f, .5f, 0, .81f }, b[] = { .5f, .19f, 0, .5f, .5f, .81f };
    float x0 = cx - s * .48f, y0 = cy - s * .5f;
    wg_poly(d, x0, y0, s, dir > 0 ? f : b, 3, col);
    wg_poly(d, x0 + s * .47f, y0, s, dir > 0 ? f : b, 3, col);
}
static void wg_leaf(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    static const float l[] = { .14f, .86f, .12f, .5f, .3f, .24f, .6f, .12f, .9f, .1f, .88f, .42f, .72f, .72f, .44f, .86f };
    wg_poly(d, cx - s / 2, cy - s / 2, s, l, 8, col);
}
static void wg_gauge(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    wg_arc(d, cx, cy + s * .08f, s * .4f, s * .12f, -2.2f, 4.4f, col, 0);
    wg_line(d, cx, cy + s * .08f, cx + s * .22f, cy - s * .14f, s * .12f, col, 255);
    fill_circle(d, cx, cy + s * .08f, s * .1f, col, 255);
}
static void wg_sdcard(uint16_t *d, float cx, float cy, float s, uint16_t col)
{
    static const float c[] = { .2f, .04f, .62f, .04f, .82f, .24f, .82f, .96f, .2f, .96f };
    wg_poly(d, cx - s / 2, cy - s / 2, s, c, 5, col);
}

/* ---- texto: un tipo de letra; el grosor (como SF: regular / medio / seminegrita / negrita)
 * se simula fundiendo cada glifo con su copia 1 px a la derecha con mas o menos peso ---- */
enum { W_REG, W_MED, W_SEMI, W_BOLD };
enum { WA_L, WA_C, WA_R };
static int wg_trunc;                         /* etiquetas de la interfaz que no cupieron (pruebas) */
static void (*wg_trunc_hook)(const char *t, int size, int maxw);
static int wg_tw(const char *s, int size, int wt) { return tw_n(s, size, -1) + (wt ? 1 : 0); }
static int wg_text(uint16_t *d, int x, int base, const char *s, int size, uint16_t col, int maxw, int wt)
{
    if (!font_ok || !s || !*s) return 0;
    static const unsigned char K[4] = { 0, 96, 160, 230 };
    unsigned k = K[wt & 3];
    int full = wg_tw(s, size, wt), ell = 0, ellw = 0;
    if (maxw > 0 && full > maxw) { ell = 1; struct glyph *g = glyph_get(0x2026, size); ellw = (g ? g->adv >> 6 : 8) + (k ? 1 : 0); }
    int pen = x << 6, c;
    while ((c = utf8_next(&s))) {
        struct glyph *g = glyph_get(c, size); if (!g) continue;
        if (ell && ((pen + g->adv) >> 6) - x > maxw - ellw) { c = 0x2026; g = glyph_get(c, size); if (!g) break; s = ""; }
        int gx = (pen >> 6) + g->xo, gy = base + g->yo, gw = g->w + (k ? 1 : 0);
        for (int yy = 0; yy < g->h && g->bmp; yy++) {
            int dy = gy + yy; if (dy < wg_cy0 || dy >= wg_cy1 || dy < 0 || dy >= H) continue;
            uint16_t *row = d + dy * W; const uint8_t *m = g->bmp + yy * g->w;
            for (int xx = 0; xx < gw; xx++) {
                int dx = gx + xx; if (dx < 0 || dx >= W) continue;
                unsigned a = xx < g->w ? m[xx] : 0;
                if (k && xx > 0) { unsigned b = m[xx - 1] * k >> 8; a = a + (b * (255 - a) >> 8); }
                if (!a) continue;
                row[dx] = a >= 255 ? col : mix565(col, row[dx], a);
            }
        }
        pen += g->adv;
        if (c == 0x2026 && !*s) break;
    }
    return (pen >> 6) - x + (k ? 1 : 0);
}
static int wg_fitsz(const char *t, int size, int minsz, int maxw, int wt) { int sz = size; while (sz > minsz && wg_tw(t, sz, wt) > maxw) sz--; return sz; }
static int wg_alx(int x, int maxw, int w, int al) { return al == WA_C ? x + (maxw - w) / 2 : al == WA_R ? x + maxw - w : x; }
/* texto de la interfaz (traducido): se reduce hasta el 75 % antes de cortar (un corte = fallo de
 * las pruebas). x = borde izquierdo de la caja de ancho maxw; devuelve el ancho */
static int wlb(uint16_t *d, int x, int base, const char *t, int size, uint16_t col, int maxw, int wt, int al)
{
    if (!t || !*t) return 0;
    int mn = size * 3 / 4; if (mn < 9) mn = 9;
    int sz = wg_fitsz(t, size, mn, maxw, wt), w = wg_tw(t, sz, wt);
    if (w > maxw) { wg_trunc++; if (wg_trunc_hook) wg_trunc_hook(t, size, maxw); w = maxw; }
    return wg_text(d, wg_alx(x, maxw, w, al), base, t, sz, col, maxw, wt);
}
/* dato (titulos, artistas): hasta 2 px menos y despues "…" */
static int wtx(uint16_t *d, int x, int base, const char *t, int size, uint16_t col, int maxw, int wt, int al)
{
    if (!t || !*t) return 0;
    int sz = wg_fitsz(t, size, size - 2 < 9 ? 9 : size - 2, maxw, wt), w = wg_tw(t, sz, wt); if (w > maxw) w = maxw;
    return wg_text(d, wg_alx(x, maxw, w, al), base, t, sz, col, maxw, wt);
}
/* corte de linea: bytes de la 1a linea (*l1) y comienzo de la 2a; en un espacio si se puede,
 * si no entre caracteres (chino, japones, tailandes, palabras largas), nunca antes de una marca
 * combinante ni de un signo que no empieza linea */
static int wg_break(const char *t, int sz, int maxw, int wt, int *l1)
{
    const char *p = t, *sp = 0, *fit = t;
    while (*p) {
        const char *q = p; int c = utf8_next(&q);
        if (tw_n(t, sz, (int)(q - t)) + (wt ? 1 : 0) > maxw) break;
        if (c == ' ') sp = p;
        else if (wrap_cjk(c) && p > t) sp = 0;              /* entre ideogramas: corte por caracter */
        fit = q; p = q;
    }
    if (!*p) { *l1 = (int)(p - t); return *l1; }
    if (sp && sp > t) { *l1 = (int)(sp - t); return *l1 + 1; }
    for (;;) {                                               /* no dejar una marca al principio de la 2a */
        const char *q = fit; int c = utf8_next(&q);
        if (fit == t || !(text_is_mark(c) || wrap_nostart(c))) break;
        do fit--; while (fit > t && (*fit & 0xC0) == 0x80);
    }
    if (fit == t) { const char *q = t; utf8_next(&q); fit = q; }
    *l1 = (int)(fit - t); return *l1;
}
/* hasta dos lineas; reduce el tamano hasta 2 puntos (4 si es de la interfaz) antes de poner "…";
 * devuelve las lineas. ui: un corte cuenta como fallo de las pruebas */
static int wt2(uint16_t *d, int x, int base, const char *t, int size, uint16_t col, int maxw, int lh, int al, int wt, int ui)
{
    if (!t || !*t) return 0;
    if (wg_tw(t, size, wt) <= maxw) { wg_text(d, wg_alx(x, maxw, wg_tw(t, size, wt), al), base, t, size, col, 0, wt); return 1; }
    int sz = size, l1 = 0, s2 = 0, mn = size - (ui ? 4 : 2); if (mn < 9) mn = 9;
    for (; sz >= mn; sz--) { s2 = wg_break(t, sz, maxw, wt, &l1); const char *b = t + s2; while (*b == ' ') b++; if (wg_tw(b, sz, wt) <= maxw) break; }
    if (sz < mn) { sz = mn; s2 = wg_break(t, sz, maxw, wt, &l1); if (ui) { wg_trunc++; if (wg_trunc_hook) wg_trunc_hook(t, size, maxw); } }
    char a[256]; int n = l1 < 255 ? l1 : 255; memcpy(a, t, n); a[n] = 0;
    while (n > 0 && a[n - 1] == ' ') a[--n] = 0;
    const char *b = t + s2; while (*b == ' ') b++;
    int w1 = wg_tw(a, sz, wt), w2 = wg_tw(b, sz, wt); if (w2 > maxw) w2 = maxw;
    wg_text(d, wg_alx(x, maxw, w1, al), base, a, sz, col, maxw, wt);
    if (*b) wg_text(d, wg_alx(x, maxw, w2, al), base + lh, b, sz, col, maxw, wt);
    return *b ? 2 : 1;
}
static int wt2_lines(const char *t, int size, int maxw, int wt)   /* lineas que usara wt2 */
{
    if (!t || !*t) return 0;
    return wg_tw(t, size, wt) <= maxw ? 1 : 2;
}
static int wg_put8(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}
static int wg_upc(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x100 && c <= 0x17F && (c & 1)) return c - 1;          /* latin extendido A (polaco) */
    return c;
}
/* mayusculas (todas, o solo la primera letra si first) */
static const char *wg_upper(const char *s, char *o, int n, int first)
{
    int k = 0, c, i = 0;
    while ((c = utf8_next(&s)) && k < n - 5) { k += wg_put8(o + k, (!first || !i) ? wg_upc(c) : c); i++; }
    o[k] = 0; return o;
}

/* ---- copias de sprites ---- */
/* copia con mascara (sin giro): las filas lejos de las esquinas (radio R) van con memcpy */
static void wg_blit_m(uint16_t *dst, const uint16_t *src, int stride, const uint8_t *m, int w, int h, int x0, int y0, int ylo, int R)
{
    int xa = x0 < 0 ? 0 : x0, xb = x0 + w > W ? W : x0 + w; if (xb <= xa || !m) return;
    for (int y = 0; y < h; y++) {
        int yy = y0 + y; if (yy < ylo || yy >= H) continue;
        uint16_t *row = dst + yy * W; const uint16_t *s = src + y * stride - x0; const uint8_t *mr = m + y * w - x0;
        if (y >= R && y < h - R) { memcpy(row + xa, s + xa, (xb - xa) * 2); continue; }
        for (int x = xa; x < xb; x++) { unsigned a = mr[x]; if (a == 255) row[x] = s[x]; else if (a) row[x] = wg_mix(s[x], row[x], a); }
    }
}
/* k en [0, n) con lo <= a0 + d*k < hi (aritmetica entera exacta, como el bucle) */
static void wg_range(long long a0, long long d, long long lo, long long hi, int n, int *k0, int *k1)
{
    long long r0, r1;
    if (d == 0) { r0 = 0; r1 = a0 >= lo && a0 < hi ? n : 0; }
    else if (d > 0) { r0 = lo - a0 <= 0 ? 0 : (lo - a0 + d - 1) / d; r1 = hi - a0 <= 0 ? 0 : (hi - a0 + d - 1) / d; }
    else { long long e = -d; r1 = a0 - lo < 0 ? 0 : (a0 - lo) / e + 1; r0 = a0 - hi < 0 ? 0 : (a0 - hi) / e + 1; }
    if (r0 < 0) r0 = 0; if (r1 > n) r1 = n; if (r0 > n) r0 = n; if (r1 < r0) r1 = r0;
    *k0 = (int)r0; *k1 = (int)r1;
}
/* un pixel del borde (esquinas suavizadas, fuera de la tarjeta): con la mascara */
static inline void wg_xf_px(uint16_t *row, int x, int U, int V2, const uint16_t *src, int stride, const uint8_t *m, int sw, int sh, int alpha)
{
    int iu = U >> 16, iv = V2 >> 16;
    if (iu < -1 || iv < -1 || iu >= sw || iv >= sh) return;
    unsigned fx = (U >> 11) & 31, fy = (V2 >> 11) & 31;
    int u0 = iu < 0 ? 0 : iu, u1 = iu + 1 >= sw ? sw - 1 : iu + 1, v0 = iv < 0 ? 0 : iv, v1 = iv + 1 >= sh ? sh - 1 : iv + 1;
    int ok_u0 = iu >= 0, ok_u1 = iu + 1 < sw, ok_v0 = iv >= 0, ok_v1 = iv + 1 < sh;
    unsigned a00 = ok_u0 && ok_v0 ? m[v0 * sw + u0] : 0, a01 = ok_u1 && ok_v0 ? m[v0 * sw + u1] : 0;
    unsigned a10 = ok_u0 && ok_v1 ? m[v1 * sw + u0] : 0, a11 = ok_u1 && ok_v1 ? m[v1 * sw + u1] : 0;
    unsigned a = ((a00 * (32 - fx) + a01 * fx) * (32 - fy) + (a10 * (32 - fx) + a11 * fx) * fy) >> 10;
    if (!a) return;
    uint32_t t0 = ((wg_sp(src[v0 * stride + u0]) * (32 - fx) + wg_sp(src[v0 * stride + u1]) * fx) >> 5) & 0x07E0F81Fu;
    uint32_t t1 = ((wg_sp(src[v1 * stride + u0]) * (32 - fx) + wg_sp(src[v1 * stride + u1]) * fx) >> 5) & 0x07E0F81Fu;
    uint32_t v = ((t0 * (32 - fy) + t1 * fy) >> 5) & 0x07E0F81Fu;
    uint16_t col = (uint16_t)(v | (v >> 16));
    if (alpha < 255) a = (a * alpha) >> 8;
    row[x] = a >= 254 ? col : wg_mix(col, row[x], a);
}
/* sprite girado "ang" y escalado "sc" con centro en (cx, cy): bilineal en coma fija (el
 * temblor del modo edicion, el encogerse al quitar o al pulsar, la vista previa grande). Colores
 * con el truco 565 "separado" (una multiplicacion por muestra) y alfa de la mascara; R = radio
 * de la mascara (su interior es opaco: se resuelve una vez por fila) */
static void wg_blit_xf(uint16_t *dst, int ylo, const uint16_t *src, int stride, const uint8_t *m, int sw, int sh,
                       float cx, float cy, float ang, float sc, int alpha, int R)
{
    if (sc < 0.03f || alpha <= 0 || !m) return;
    if (ang == 0 && sc == 1 && alpha >= 255) {
        float fx = cx - sw / 2.f, fy = cy - sh / 2.f;
        if (fx == floorf(fx) && fy == floorf(fy)) { wg_blit_m(dst, src, stride, m, sw, sh, (int)fx, (int)fy, ylo, R); return; }
    }
    float c = cosf(ang), s = sinf(ang), hw = sw * sc / 2, hh = sh * sc / 2;
    float ex = fabsf(c) * hw + fabsf(s) * hh, ey = fabsf(s) * hw + fabsf(c) * hh;
    int xa = (int)floorf(cx - ex) - 1, xb = (int)ceilf(cx + ex) + 1, ya = (int)floorf(cy - ey) - 1, yb = (int)ceilf(cy + ey) + 1;
    if (xa < 0) xa = 0; if (xb > W) xb = W; if (ya < ylo) ya = ylo; if (yb > H) yb = H;
    if (xb <= xa) return;
    float inv = 1.f / sc;
    int du = (int)(c * inv * 65536), dv = (int)(-s * inv * 65536), card = R > 0 && sw > 2 * R + 2 && sh > 2 * R + 2, opaque = alpha >= 255 && card;
    unsigned aint = !opaque && card ? (unsigned)(alpha * 33) >> 8 : 0;
    for (int y = ya; y < yb; y++) {
        float dy = y + .5f - cy, dx = xa + .5f - cx;
        int U = (int)(((c * dx + s * dy) * inv + sw / 2.f - .5f) * 65536), V2 = (int)(((-s * dx + c * dy) * inv + sh / 2.f - .5f) * 65536);
        uint16_t *row = dst + y * W;
        int n = xb - xa, k0 = 0, k1 = 0;
        if (card && (opaque || aint)) {   /* tramo interior (mascara 255): se resuelve una vez por fila */
            int a0, a1, b0, b1;
            wg_range(U, du, 0, (long long)(sw - 1) << 16, n, &a0, &a1);
            wg_range(V2, dv, (long long)R << 16, (long long)(sh - R - 1) << 16, n, &b0, &b1);
            k0 = a0 > b0 ? a0 : b0; k1 = a1 < b1 ? a1 : b1; if (k1 < k0) k1 = k0;
            /* franjas de arriba y abajo: entre las esquinas la mascara tambien es 255 */
            int c0, c1, d0, d1;
            wg_range(U, du, (long long)R << 16, (long long)(sw - R - 1) << 16, n, &c0, &c1);
            wg_range(V2, dv, 0, (long long)(sh - 1) << 16, n, &d0, &d1);
            int e0 = c0 > d0 ? c0 : d0, e1 = c1 < d1 ? c1 : d1;
            if (e1 > e0) { if (k1 <= k0) { k0 = e0; k1 = e1; } else if (e0 <= k1 && e1 >= k0) { if (e0 < k0) k0 = e0; if (e1 > k1) k1 = e1; } }
        }
        for (int k = 0; k < k0; k++) wg_xf_px(row, xa + k, U + du * k, V2 + dv * k, src, stride, m, sw, sh, alpha);
        {
            int Uk = U + du * k0, Vk = V2 + dv * k0; uint16_t *o = row + xa + k0, *oe = row + xa + k1;
            if (opaque) for (; o < oe; o++, Uk += du, Vk += dv) {
                unsigned fx = (Uk >> 11) & 31, fy = (Vk >> 11) & 31;
                const uint16_t *pp = src + (Vk >> 16) * stride + (Uk >> 16);
                uint32_t t0 = ((wg_sp(pp[0]) * (32 - fx) + wg_sp(pp[1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t t1 = ((wg_sp(pp[stride]) * (32 - fx) + wg_sp(pp[stride + 1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t v = ((t0 * (32 - fy) + t1 * fy) >> 5) & 0x07E0F81Fu;
                *o = (uint16_t)(v | (v >> 16));
            } else for (; o < oe; o++, Uk += du, Vk += dv) {
                unsigned fx = (Uk >> 11) & 31, fy = (Vk >> 11) & 31;
                const uint16_t *pp = src + (Vk >> 16) * stride + (Uk >> 16);
                uint32_t t0 = ((wg_sp(pp[0]) * (32 - fx) + wg_sp(pp[1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t t1 = ((wg_sp(pp[stride]) * (32 - fx) + wg_sp(pp[stride + 1]) * fx) >> 5) & 0x07E0F81Fu;
                uint32_t v = ((t0 * (32 - fy) + t1 * fy) >> 5) & 0x07E0F81Fu;
                v = ((v * aint + wg_sp(*o) * (32 - aint)) >> 5) & 0x07E0F81Fu;
                *o = (uint16_t)(v | (v >> 16));
            }
        }
        for (int k = k1; k < n; k++) wg_xf_px(row, xa + k, U + du * k, V2 + dv * k, src, stride, m, sw, sh, alpha);
    }
}

/* ------------------------------------------------------------------ dibujo de cada widget */
#define C_SEC rgb(168, 168, 176)             /* etiqueta secundaria (60 % de blanco sobre el cristal) */
#define C_TER rgb(118, 118, 126)
#define C_RED rgb(255, 69, 58)
#define C_GREEN rgb(48, 209, 88)
#define C_YELLOW rgb(255, 214, 10)
#define C_ORANGE rgb(255, 159, 10)
#define C_BLUE rgb(10, 132, 255)
#define C_PURPLE rgb(191, 90, 242)
#define C_PINK rgb(255, 55, 95)
#define C_TRACK rgb(255, 255, 255)
static const uint16_t wg_mixc[3] = { 0xF8A9, 0x6A1F, 0xFB20 };   /* los de Musica: Mezcla del dia, Descubre, Tus favoritos */
static const char *const wg_mixn[3] = { "Mezcla del día", "Descubre", "Tus favoritos" };
struct wbtn { short x, y, r, k; };           /* boton dentro de un widget (k: 0 anterior, 1 play/pausa, 2 siguiente) */
static struct wbtn wg_bcur[3]; static int wg_nbcur;
static void wg_addbtn(int x, int y, int r, int k) { if (wg_nbcur < 3) wg_bcur[wg_nbcur++] = (struct wbtn){ (short)x, (short)y, (short)r, (short)k }; }
static const char *wg_gb(long long b, char *o, int n)
{
    double g = b / 1073741824.0;
    if (g >= 1024) snprintf(o, n, "%.1f TB", g / 1024); else if (g >= 10) snprintf(o, n, "%.0f GB", g); else if (g >= 1) snprintf(o, n, "%.1f GB", g); else snprintf(o, n, "%.0f MB", b / 1048576.0);
    return o;
}
static const char *wg_mins(int sec, char *o, int n)       /* "45 min" / "3 h 20 min" */
{
    int m = sec / 60;
    if (m < 60) snprintf(o, n, wgt("%d min"), m); else snprintf(o, n, wgt("%d h %d min"), m / 60, m % 60);
    return o;
}
static uint16_t wg_batt_col(int pct, int chg, int self)
{
    if (pct < 0) return C_SEC;
    if (pct <= 20 && !chg) return C_RED;
    if (self && pw_mode == PW_SAVE && !chg) return C_YELLOW;
    return C_GREEN;
}
/* porcentaje con el rayo de carga a la derecha, centrado en cx */
static void wg_pct(uint16_t *d, float cx, int base, int pct, int chg, int size, uint16_t col)
{
    char b[12]; if (pct >= 0) snprintf(b, sizeof b, "%d %%", pct); else snprintf(b, sizeof b, "—");
    int tw = wg_tw(b, size, W_SEMI), bw = chg ? (int)(size * .78f) + 3 : 0, x = (int)roundf(cx - (tw + bw) / 2.f);
    wg_text(d, x, base, b, size, col, 0, W_SEMI);
    if (chg) wg_bolt(d, x + tw + 2, base - size * .8f, size * .82f, C_GREEN);
}
static const char *const wg_dn[7] = { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" };
static const char *const wg_mn[12] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };
static int wg_sunday_first(void) { return ui_lang == LANG_EN || ui_lang == LANG_JA || ui_lang == LANG_KO || ui_lang == LANG_ZHS || ui_lang == LANG_ZHT || ui_lang == LANG_TH; }
/* inicial del dia de la semana (0 = domingo): la primera letra; en chino la ultima; en tailandes la de iOS */
static const char *wg_initial(int wd, char *o)
{
    static const char *const th[7] = { "อา", "จ", "อ", "พ", "พฤ", "ศ", "ส" };
    if (ui_lang == LANG_TH) return th[wd];
    const char *nm = tr(wg_dn[wd]), *q = nm; int ch = utf8_next(&q), last = ch;
    if (ch == 0x661F || ch == 0x5468 || ch == 0x9031 || ch == 0x79AE) { const char *r = nm; int k; while ((k = utf8_next(&r))) last = k; ch = last; }
    int n = wg_put8(o, wg_upc(ch)); o[n] = 0; return o;
}
/* caratula de lo que suena (o el hueco con la nota) */
static void wg_art_ph(uint16_t *d, int x, int y, int s, float r)
{
    wg_rrect_grad(d, x, y, s, s, r, rgb(84, 84, 92), rgb(56, 56, 62));
    wg_note(d, x + s / 2.f, y + s / 2.f, s * .42f, rgb(176, 176, 184));
}
static void wg_np_art(uint16_t *d, int x, int y, int s, float r)
{
    if (wg_ni.source && wg_cok) wg_cover_blit(d, wg_cov, 128, x, y, s, r); else wg_art_ph(d, x, y, s, r);
}
static void wg_mix_tile(uint16_t *d, int x, int y, int s, float r, int k)
{
    uint16_t c = wg_mixc[k];
    wg_rrect_grad(d, x, y, s, s, r, wg_mix(0xFFFF, c, 40), dim565(c, 18));
    wg_note(d, x + s * .5f, y + s * .48f, s * .44f, 0xFFFF);
}
/* esfera de reloj analogico (modo oscuro de iOS): numeros si r >= 40, aguja de horas y minutos */
static void wg_clock(uint16_t *d, float cx, float cy, float r, int hh, int mm)
{
    const float TAU = 6.2831853f;
    fill_circle(d, cx, cy, r, rgb(16, 16, 18), 255);
    wg_arc(d, cx, cy, r - .6f, 1.2f, 0, TAU, rgb(72, 72, 78), 0);
    int nums = r >= 40;
    for (int i = 0; i < 60; i++) {
        if (nums && i % 5 == 0) continue;
        float a = i * TAU / 60, ro = r * .93f, ri = ro - (i % 5 ? r * .045f : r * .09f);
        wg_line(d, cx + ri * sinf(a), cy - ri * cosf(a), cx + ro * sinf(a), cy - ro * cosf(a), i % 5 ? 1.f : 1.8f, i % 5 ? rgb(110, 110, 116) : 0xFFFF, 255);
    }
    if (nums) for (int h = 1; h <= 12; h++) {
        char b[4]; snprintf(b, sizeof b, "%d", h); float a = h * TAU / 12, rr = r * .76f; int sz = (int)(r * .22f);
        int tw = wg_tw(b, sz, W_SEMI); wg_text(d, (int)roundf(cx + rr * sinf(a) - tw / 2.f), (int)roundf(cy - rr * cosf(a) + sz * .36f), b, sz, 0xFFFF, 0, W_SEMI);
    }
    float ah = ((hh % 12) + mm / 60.f) * TAU / 12, am = mm * TAU / 60;
    wg_line(d, cx, cy, cx + r * .5f * sinf(ah), cy - r * .5f * cosf(ah), r * .085f, 0xFFFF, 255);
    wg_line(d, cx, cy, cx + r * .8f * sinf(am), cy - r * .8f * cosf(am), r * .06f, 0xFFFF, 255);
    fill_circle(d, cx, cy, r * .075f, C_ORANGE, 255);
    fill_circle(d, cx, cy, r * .03f, rgb(16, 16, 18), 255);
}
/* barra de progreso de iOS: pista tenue y avance claro, extremos redondos */
static void wg_bar(uint16_t *d, int x, int y, int w, int th, float f, uint16_t col)
{
    if (f < 0) f = 0; if (f > 1) f = 1;
    fill_rrect(d, x, y, w, th, th / 2.f, col, 64);
    int fw = (int)roundf(w * f); if (f > 0 && fw < th) fw = th;
    if (fw > 0) fill_rrect(d, x, y, fw, th, th / 2.f, col, 255);
}
/* barras de la semana (los ultimos 7 dias, hoy a la derecha en rosa) y sus iniciales */
static void wg_week(uint16_t *d, int x, int y, int w, int h, const int *sec, int labels)
{
    int mx = 30 * 60; for (int k = 0; k < 7; k++) if (sec[k] > mx) mx = sec[k];
    float gap = w / 7.f * .38f, bw = (w - 6 * gap) / 7.f;
    int wd = 0; { time_t t = time(0); struct tm tm; localtime_r(&t, &tm); wd = tm.tm_wday; }
    int bh = labels ? h - 16 : h;
    for (int k = 0; k < 7; k++) {
        float bx = x + k * (bw + gap); int v = (int)roundf(bh * (float)sec[k] / mx), today = k == 6;
        fill_rrect(d, (int)roundf(bx), y, (int)roundf(bw), bh, bw / 2, C_TRACK, 22);
        if (v > 0) { if (v < (int)bw) v = (int)bw; fill_rrect(d, (int)roundf(bx), y + bh - v, (int)roundf(bw), v, bw / 2, today ? C_PINK : rgb(150, 150, 158), 255); }
        if (labels) { char l[8]; const char *in = wg_initial((wd + 1 + k) % 7, l); int ww = today ? W_SEMI : W_REG;
            wg_text(d, (int)roundf(bx + bw / 2 - wg_tw(in, 10, ww) / 2.f), y + h, in, 10, today ? 0xFFFF : C_SEC, 0, ww); }
    }
}

static void wg_render(int type, int sz, uint16_t *d, int w, int h)
{
    char b[200], u[200], s1[24], s2[24]; const struct wdata *D = &WDs;
    const int p = sz == 2 ? 16 : 14, cw = w - 2 * p;
    wg_nbcur = 0;
    switch (type) {
    case WG_BATT: {
        struct wbi { int pct, kind, chg, fam, part; const char *name; } dv[5]; int nd = 0;    /* kind 0 HiBy, 1 BlueZ, 2 app Auriculares */
        dv[nd++] = (struct wbi){ D->batt, 0, D->charging, 0, 0, "HiBy R3 II" };
        const struct wg_hp *hp = sz ? D->hp : D->hps; int nh = sz ? D->nhp : D->nhps;
        if (nh) for (int i = 0; i < nh && nd < (sz ? 4 : 2); i++)
            dv[nd++] = (struct wbi){ hp[i].pct, 2, hp[i].chg, hp[i].fam, hp[i].part, tr(hp[i].part == 1 ? "Izquierdo" : hp[i].part == 2 ? "Derecho" : hp[i].part == 3 ? "Estuche" : "Auriculares") };
        else for (int i = 0; i < D->nbt && nd < 4; i++) dv[nd++] = (struct wbi){ D->bt[i].pct, 1, 0, 0, 0, D->bt[i].name };
        /* anillo de iOS: pista tenue, avance de color (verde / rojo <= 20 % / amarillo en ahorro),
         * el glifo del aparato dentro y, cargando, una insignia con el rayo encima del anillo.
         * Sin dato de bateria (aparato que no lo informa): solo la pista y el glifo, atenuado. */
        #define WG_ITEM(e, cx, cy, r, th) do { uint16_t col_ = wg_batt_col((e)->pct, (e)->chg, !(e)->kind); \
            if ((e)->pct >= 0) wg_ring(d, cx, cy, r, th, (e)->pct / 100.f, col_); else wg_arc(d, cx, cy, r, th, 0, 0, C_TRACK, 60); \
            uint16_t gc_ = (e)->pct >= 0 ? 0xFFFF : rgb(150, 150, 158); \
            if ((e)->kind == 2) buds_glyph(d, cx, (cy) - (r) * .04f, ((r) - (th)) / 34.f, (e)->fam, (e)->part, wg_cy0, wg_cy1); \
            else if ((e)->kind) wg_glyph_phones(d, cx, cy, (r) * .78f, gc_); else wg_glyph_player(d, cx, cy, (r) * .8f, 0xFFFF); \
            if ((e)->chg) { float br_ = (th) * .9f + 3; fill_circle(d, cx, (cy) - (r), br_, rgb(28, 28, 30), 255); \
                wg_bolt(d, (cx) - br_ * .42f, (cy) - (r) - br_ * .62f, br_ * 1.25f, C_GREEN); } } while (0)
        if (sz == 0 && nd == 1) {
            WG_ITEM(&dv[0], w / 2.f, 58, 40, 9);
            wg_pct(d, w / 2.f, 124, dv[0].pct, dv[0].chg, 20, 0xFFFF);
        } else if (sz == 0 && nd == 2) {
            for (int i = 0; i < 2; i++) { float cx = i ? w * .73f : w * .27f; WG_ITEM(&dv[i], cx, 54, 25, 7); wg_pct(d, cx, 113, dv[i].pct, dv[i].chg, 15, 0xFFFF); }
        } else if (sz == 0) {
            for (int i = 0; i < 4; i++) {
                float cx = (i & 1) ? w * .73f : w * .27f, cy = (i & 2) ? 99 : 38;
                if (i >= nd) { wg_arc(d, cx, cy, 22, 6, 0, 0, C_TRACK, 40); continue; }
                WG_ITEM(&dv[i], cx, cy, 22, 6);
            }
        } else if (sz == 1) {
            for (int i = 0; i < 4; i++) {
                float sw = cw / 4.f, cx = p + sw * i + sw / 2, cy = 54;
                if (i >= nd) { wg_arc(d, cx, cy, 26, 7, 0, 0, C_TRACK, 40); continue; }
                WG_ITEM(&dv[i], cx, cy, 26, 7);
                wg_pct(d, cx, 116, dv[i].pct, dv[i].chg, 15, 0xFFFF);
            }
        } else if (nd == 1) {
            WG_ITEM(&dv[0], w / 2.f, 118, 76, 15);
            wg_pct(d, w / 2.f, 236, dv[0].pct, dv[0].chg, 30, 0xFFFF);
            const char *stt = D->charging ? tr("Cargando") : D->usb ? tr("Conectado por USB") : tr("Con batería");
            wlb(d, p, 262, stt, 14, C_SEC, cw, W_REG, WA_C);
        } else {   /* filas: anillo con el glifo, nombre, estado y porcentaje */
            int rh = (h - 2 * p) / 4;
            for (int i = 0; i < nd; i++) {
                int cy = p + i * rh + rh / 2, tx = p + 62;
                WG_ITEM(&dv[i], p + 26.f, (float)cy, 24, 6);
                const char *sub = dv[i].chg ? tr("Cargando") : (!dv[i].kind && D->usb) ? tr("Conectado por USB") : "";
                char pc[12]; if (dv[i].pct >= 0) snprintf(pc, sizeof pc, "%d %%", dv[i].pct); else snprintf(pc, sizeof pc, "—");
                int pw = wg_tw(pc, 22, W_SEMI);
                wg_text(d, w - p - pw, cy + 8, pc, 22, 0xFFFF, 0, W_SEMI);
                int nw = w - p - pw - 10 - tx;
                if (dv[i].kind == 1) wtx(d, tx, sub[0] ? cy - 1 : cy + 6, dv[i].name, 16, 0xFFFF, nw, W_SEMI, WA_L);
                else wlb(d, tx, sub[0] ? cy - 1 : cy + 6, dv[i].name, 16, 0xFFFF, nw, W_SEMI, WA_L);
                if (sub[0]) wlb(d, tx, cy + 16, sub, 12, C_SEC, nw, W_REG, WA_L);
                if (i < nd - 1) for (int x = tx; x < w - p; x++) wg_px(d, x, p + (i + 1) * rh, 0xFFFF, 34);
            }
        }
        #undef WG_ITEM
        break; }
    case WG_NOW: {
        int have = wg_ni.source != 0, pl = wg_playing();
        const char *ti = have && wg_ni.title[0] ? wg_ni.title : tr("Reproduciendo"), *ar = have ? wg_ni.artist : "";
        float fr = have && wg_ni.duration_ms > 0 && wg_ni.position_ms >= 0 ? (float)wg_ni.position_ms / wg_ni.duration_ms : 0; if (fr > 1) fr = 1;
        int bar = have && wg_ni.duration_ms > 0;
        const char *stt = tr(!have ? "Sin reproducción" : pl ? "Sonando" : "En pausa");
        if (!have && D->last_title[0]) {   /* nada abierto: lo ultimo que sono (HISTORY_TABLE); tocar abre Reproduciendo */
            const char *lt = tr("Lo último");
            if (sz == 0) { wg_np_art(d, p, p, 62, 10); wlb(d, p, 92, lt, 11, C_SEC, cw, W_SEMI, WA_L); wtx(d, p, 108, D->last_title, 14, 0xFFFF, cw, W_SEMI, WA_L); wtx(d, p, 124, D->last_artist, 12, C_SEC, cw, W_REG, WA_L); }
            else { int cs = sz == 2 ? 116 : 109, x0 = p + cs + 14, tw = w - x0 - p; wg_np_art(d, p, p, cs, 12);
                wlb(d, x0, 28 + (sz == 2) * 4, lt, sz == 2 ? 12 : 11, C_SEC, tw, W_SEMI, WA_L);
                int ln = wt2(d, x0, sz == 2 ? 54 : 47, D->last_title, sz == 2 ? 17 : 15, 0xFFFF, tw, sz == 2 ? 20 : 18, WA_L, W_SEMI, 0);
                wtx(d, x0, (sz == 2 ? 54 : 47) + ln * (sz == 2 ? 20 : 18), D->last_artist, sz == 2 ? 14 : 12, C_SEC, tw, W_REG, WA_L); }
            break;
        }
        if (!have && sz == 2) {             /* grande sin nada: el hueco en el centro */
            wg_art_ph(d, (w - 150) / 2, 48, 150, 16);
            wt2(d, p, 230, stt, 17, 0xFFFF, cw, 21, WA_C, W_SEMI, 1);
            break;
        }
        if (sz == 0) {
            wg_np_art(d, p, p, 62, 10);
            if (have) {
                int bx = w - p - 19, by = p + 19;
                fill_circle(d, bx, by, 19, 0xFFFF, 50);
                if (pl) wg_pause_glyph(d, bx, by, 14, 0xFFFF); else wg_play_glyph(d, bx + 1, by, 15, 0xFFFF);
                wg_addbtn(bx, by, 26, 1);
                wtx(d, p, 96, ti, 14, 0xFFFF, cw, W_SEMI, WA_L);
                if (ar[0]) wtx(d, p, 112, ar, 12, C_SEC, cw, W_REG, WA_L); else wlb(d, p, 112, stt, 12, C_SEC, cw, W_REG, WA_L);
                if (bar) wg_bar(d, p, 120, cw, 3, fr, 0xFFFF);
            } else wt2(d, p, 101, stt, 14, 0xFFFF, cw, 17, WA_L, W_SEMI, 1);
        } else if (sz == 1) {
            int cs = 109; wg_np_art(d, p, p, cs, 12);
            int x0 = p + cs + 14, tw = w - x0 - p;
            if (!have) { wt2(d, x0, 66, stt, 15, 0xFFFF, tw, 19, WA_L, W_SEMI, 1); break; }
            wlb(d, x0, 28, stt, 11, pl ? C_PINK : C_SEC, tw, W_SEMI, WA_L);
            int ln = wt2(d, x0, 47, ti, 15, 0xFFFF, tw, 18, WA_L, W_SEMI, 0);
            wtx(d, x0, 47 + ln * 18 - 1, ar, 12, C_SEC, tw, W_REG, WA_L);
            if (bar) wg_bar(d, x0, 92, tw, 3, fr, 0xFFFF);
            for (int k = 0; k < 3; k++) {
                int cx = x0 + tw * (1 + 2 * k) / 6, cy = 115;
                if (k == 1) { if (pl) wg_pause_glyph(d, cx, cy, 17, 0xFFFF); else wg_play_glyph(d, cx + 1, cy, 19, 0xFFFF); }
                else wg_skip_glyph(d, cx, cy, 17, 0xFFFF, k ? 1 : -1);
                wg_addbtn(cx, cy, 22, k);
            }
        } else {
            int cs = 116; wg_np_art(d, p, p, cs, 12);
            int x0 = p + cs + 14, tw = w - x0 - p;
            if (!have) { wt2(d, x0, 70, stt, 17, 0xFFFF, tw, 21, WA_L, W_SEMI, 1); break; }
            wlb(d, x0, 32, stt, 12, pl ? C_PINK : C_SEC, tw, W_SEMI, WA_L);
            int ln = wt2(d, x0, 54, ti, 17, 0xFFFF, tw, 20, WA_L, W_SEMI, 0), y = 54 + ln * 20;
            wtx(d, x0, y, ar, 14, 0xFFFF, tw, W_REG, WA_L);
            if (wg_ni.album[0]) wtx(d, x0, y + 17, wg_ni.album, 12, C_SEC, tw, W_REG, WA_L);
            if (bar) {
                wg_bar(d, p, 152, cw, 4, fr, 0xFFFF);
                int ps = wg_ni.position_ms / 1000, ds = wg_ni.duration_ms / 1000, rs = ds - ps; if (rs < 0) rs = 0;
                snprintf(s1, sizeof s1, "%d:%02d", ps / 60, ps % 60); snprintf(s2, sizeof s2, "-%d:%02d", rs / 60, rs % 60);
                wg_text(d, p, 172, s1, 11, C_SEC, 0, W_MED); wg_text(d, w - p - wg_tw(s2, 11, W_MED), 172, s2, 11, C_SEC, 0, W_MED);
            }
            int cy = 214;
            wg_skip_glyph(d, w / 2.f - 80, cy, 22, 0xFFFF, -1); wg_addbtn(w / 2 - 80, cy, 26, 0);
            fill_circle(d, w / 2.f, cy, 29, 0xFFFF, 255);
            if (pl) wg_pause_glyph(d, w / 2.f, cy, 20, rgb(24, 24, 28)); else wg_play_glyph(d, w / 2.f + 2, cy, 22, rgb(24, 24, 28));
            wg_addbtn(w / 2, cy, 32, 1);
            wg_skip_glyph(d, w / 2.f + 80, cy, 22, 0xFFFF, 1); wg_addbtn(w / 2 + 80, cy, 26, 2);
            {   /* formato real del fichero: extension, bits y frecuencia del PCM */
                const char *e = strrchr(wg_ni.path, '.'); char fm[12] = ""; int k = 0;
                if (e && strlen(e) < 8) for (const char *q = e + 1; *q && k < 10; q++) fm[k++] = (char)toupper((unsigned char)*q);
                fm[k] = 0; b[0] = 0;
                if (fm[0]) snprintf(b, sizeof b, "%s", fm);
                if (wg_ni.bits > 0) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%d bit", b[0] ? " · " : "", wg_ni.bits);
                if (wg_ni.rate > 0) { if (wg_ni.rate % 1000) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%.1f kHz", b[0] ? " · " : "", wg_ni.rate / 1000.0); else snprintf(b + strlen(b), sizeof b - strlen(b), "%s%d kHz", b[0] ? " · " : "", wg_ni.rate / 1000); }
                if (b[0]) wtx(d, p, 268, b, 12, C_SEC, cw, W_MED, WA_C);
            }
        }
        break; }
    case WG_DATE: {
        time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
        int lw = sz ? 110 : cw;
        wlb(d, p, 28, wg_upper(tr(wg_dn[tm.tm_wday]), u, sizeof u, 0), 12, C_RED, lw, W_SEMI, WA_L);
        snprintf(b, sizeof b, "%d", tm.tm_mday); wg_text(d, p - 2, 86, b, 52, 0xFFFF, 0, W_REG);
        snprintf(b, sizeof b, "%s %d", wg_upper(tr(wg_mn[tm.tm_mon]), u, sizeof u, 1), 1900 + tm.tm_year);
        wt2(d, p, 112, b, 14, 0xFFFF, lw, 16, WA_L, W_SEMI, 1);
        if (sz) {   /* calendario del mes, hoy en un circulo rojo */
            int gx = 134, gw = w - gx - 10; float colw = gw / 7.f;
            int sun = wg_sunday_first();
            int first = (tm.tm_wday - (tm.tm_mday - 1) % 7 + 7) % 7;             /* dia de la semana del 1 */
            int off = sun ? first : (first + 6) % 7;
            static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int y = 1900 + tm.tm_year, nd = mdays[tm.tm_mon] + (tm.tm_mon == 1 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
            int rows = (off + nd + 6) / 7, rh = rows > 5 ? 15 : 17, y0 = 34 + rh;
            for (int c = 0; c < 7; c++) { char l[8]; wlb(d, gx + (int)(colw * c), 28, wg_initial(sun ? c : (c + 1) % 7, l), 10, C_SEC, (int)colw, W_SEMI, WA_C); }
            for (int dd = 1; dd <= nd; dd++) {
                int k = off + dd - 1, c = k % 7, r = k / 7; float cx = gx + colw * c + colw / 2; int base = y0 + r * rh;
                snprintf(b, sizeof b, "%d", dd);
                if (dd == tm.tm_mday) fill_circle(d, cx, base - 4.5f, 8.5f, C_RED, 255);
                wtx(d, (int)(cx - colw / 2 - 1), base, b, 11, dd == tm.tm_mday ? 0xFFFF : rgb(225, 225, 230), (int)colw + 2, dd == tm.tm_mday ? W_SEMI : W_REG, WA_C);
            }
        }
        break; }
    case WG_CLOCK: {
        time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
        if (!sz) { wg_clock(d, w / 2.f, h / 2.f, 58, tm.tm_hour, tm.tm_min); break; }
        wg_clock(d, w - p - 54.f, h / 2.f, 54, tm.tm_hour, tm.tm_min);
        int lw = w - p - 108 - 12 - p;
        if (ui_lang == LANG_EN) { int hh = tm.tm_hour % 12; snprintf(b, sizeof b, "%d:%02d", hh ? hh : 12, tm.tm_min); }
        else snprintf(b, sizeof b, "%d:%02d", tm.tm_hour, tm.tm_min);
        int tw = wtx(d, p - 1, 84, b, 50, 0xFFFF, lw, W_MED, WA_L);
        if (ui_lang == LANG_EN) wg_text(d, p + tw + 3, 84, tm.tm_hour < 12 ? "AM" : "PM", 14, C_SEC, 0, W_SEMI);
        wlb(d, p, 32, wg_upper(tr(wg_dn[tm.tm_wday]), u, sizeof u, 1), 14, C_RED, lw, W_SEMI, WA_L);
        lang_date(now, b, sizeof b); wtx(d, p, 112, b, 14, C_SEC, lw, W_REG, WA_L);
        break; }
    case WG_STORAGE: {
        if (!sz) {
            int sd = D->sd_tot > 0; long long fr = sd ? D->sd_free : D->in_free, tot = sd ? D->sd_tot : D->in_tot;
            uint16_t col = sd ? C_BLUE : C_PURPLE;
            wlb(d, p, 28, tr(sd ? "Tarjeta SD" : "En el HiBy"), 12, col, cw, W_SEMI, WA_L);
            float cx = w / 2.f, cy = 82;
            wg_ring(d, cx, cy, 38, 9, tot ? 1 - (float)fr / tot : 0, col);
            wtx(d, (int)cx - 28, (int)cy + 3, wg_gb(fr, s1, sizeof s1), 16, 0xFFFF, 56, W_SEMI, WA_C);
            wlb(d, (int)cx - 26, (int)cy + 17, tr("libres"), 10, C_SEC, 52, W_REG, WA_C);
        } else {
            wlb(d, p, 28, tr("Almacenamiento"), 12, C_BLUE, cw, W_SEMI, WA_L);
            for (int k = 0; k < 2; k++) {
                int ry = 40 + k * 46; long long fr = k ? D->in_free : D->sd_free, tot = k ? D->in_tot : D->sd_tot;
                uint16_t col = k ? C_PURPLE : C_BLUE; float rcx = p + 18.f, rcy = ry + 20.f;
                wg_ring(d, rcx, rcy, 16, 5, tot ? 1 - (float)fr / tot : 0, col);
                if (k) wg_glyph_player(d, rcx, rcy, 15, 0xFFFF); else wg_sdcard(d, rcx, rcy, 13, 0xFFFF);
                if (!k && !tot) { wlb(d, p + 46, ry + 26, tr("Sin tarjeta SD"), 15, C_SEC, cw - 46, W_MED, WA_L); continue; }
                int pc = tot ? (int)(100 - 100 * fr / tot + .5) : 0; snprintf(b, sizeof b, "%d %%", pc);
                int pw = wg_tw(b, 17, W_SEMI);
                wg_text(d, w - p - pw, ry + 26, b, 17, 0xFFFF, 0, W_SEMI);
                wlb(d, p + 46, ry + 17, tr(k ? "En el HiBy" : "Tarjeta SD"), 15, 0xFFFF, cw - 46 - pw - 8, W_SEMI, WA_L);
                snprintf(u, sizeof u, tr("%s libres de %s"), wg_gb(fr, s1, sizeof s1), wg_gb(tot, s2, sizeof s2));
                wlb(d, p + 46, ry + 34, u, 12, C_SEC, cw - 46 - pw - 8, W_REG, WA_L);
            }
        }
        break; }
    case WG_POWER: {
        uint16_t col = pw_mode == PW_SAVE ? rgb(255, 204, 0) : pw_mode == PW_PERF ? C_RED : C_BLUE;
        fill_circle(d, p + 18, p + 18, 18, col, 255);
        if (pw_mode == PW_SAVE) wg_leaf(d, p + 18.f, p + 18.f, 20, 0xFFFF);
        else if (pw_mode == PW_PERF) wg_bolt(d, p + 12.f, p + 8.f, 20, 0xFFFF);
        else wg_gauge(d, p + 18.f, p + 18.f, 22, 0xFFFF);
        int lw = sz ? 140 : cw;
        wlb(d, p, 88, tr(pw_name(pw_mode)), 22, 0xFFFF, lw, W_SEMI, WA_L);
        snprintf(b, sizeof b, "%s · %d Hz", tr("Pantalla"), lcd_hz); wlb(d, p, 107, b, 12, C_SEC, lw, W_REG, WA_L);
        const char *stt = pw_nocharge && D->usb ? tr("Solo USB") : D->charging ? tr("Cargando") : D->usb ? tr("Conectado por USB") : tr("Con batería");
        wlb(d, p, 124, stt, 12, C_SEC, lw, W_REG, WA_L);
        if (!sz) { snprintf(b, sizeof b, "%d %%", D->batt < 0 ? 0 : D->batt); wtx(d, w - p - 60, 30, b, 13, C_SEC, 60, W_SEMI, WA_R); }
        else {
            float cx = w - p - 46, cy = 68;
            wg_ring(d, cx, cy, 42, 9, D->batt / 100.f, wg_batt_col(D->batt, D->charging, 1));
            snprintf(b, sizeof b, "%d %%", D->batt < 0 ? 0 : D->batt); wtx(d, (int)cx - 32, (int)cy + 4, b, 20, 0xFFFF, 64, W_SEMI, WA_C);
            wlb(d, (int)cx - 30, (int)cy + 19, tr("Batería"), 10, C_SEC, 60, W_REG, WA_C);
        }
        break; }
    case WG_LIB: {
        wg_note(d, p + 7.f, 23, 14, C_PINK);
        wlb(d, p + 18, 28, tr("Biblioteca"), 12, C_PINK, cw - 18, W_SEMI, WA_L);
        float fav = D->songs ? (float)D->favs / D->songs : 0;
        if (!sz) {
            snprintf(b, sizeof b, "%d", D->songs); wtx(d, p, 76, b, 34, 0xFFFF, cw, W_SEMI, WA_L);
            wlb(d, p, 94, tr("canciones"), 13, C_SEC, cw, W_REG, WA_L);
        } else {
            int v[3] = { D->songs, D->albums, D->artists }; const char *l[3] = { "canciones", "álbumes", "artistas" };
            for (int k = 0; k < 3; k++) { int cx = p + k * cw / 3; snprintf(b, sizeof b, "%d", v[k]); wtx(d, cx, 76, b, 28, 0xFFFF, cw / 3 - 6, W_SEMI, WA_L); wlb(d, cx, 94, tr(l[k]), 12, C_SEC, cw / 3 - 6, W_REG, WA_L); }
        }
        wg_ring(d, p + 8, 117, 7, 3.5f, fav, C_PINK);
        if (sz) snprintf(b, sizeof b, "%d %s · %s", D->favs, tr("favoritas"), wg_gb(D->lib_bytes, s1, sizeof s1));
        else snprintf(b, sizeof b, "%d %s", D->favs, tr("favoritas"));
        wlb(d, p + 22, 122, b, 12, C_SEC, cw - 22, W_REG, WA_L);
        break; }
    case WG_NEW: {
        wlb(d, p, sz == 2 ? 30 : 28, tr("Recién añadido"), sz == 2 ? 13 : 12, C_PINK, cw, W_SEMI, WA_L);
        int n = D->nrec; const char *al[WG_NREC], *at[WG_NREC]; int hc[WG_NREC];
        for (int i = 0; i < n; i++) { al[i] = D->rec_alb[i]; at[i] = D->rec_art[i]; hc[i] = D->rec_cov[i]; }
        if (!n && D->new_album[0]) { al[0] = D->new_album; at[0] = D->new_artist; hc[0] = 0; n = 1; }
        if (!n) { wt2(d, p, h / 2 + 6, wgt("Sin álbumes"), 15, C_SEC, cw, 18, WA_L, W_MED, 1); break; }
        pthread_mutex_lock(&wg_mu);
        if (sz == 0) {
            int cs = (cw - 8) / 2;
            for (int i = 0; i < 2; i++) { int x = p + i * (cs + 8); if (i < n && hc[i]) wg_cover_blit(d, wg_rcov[i], WG_RCS, x, 38, cs, 8); else if (i < n) wg_art_ph(d, x, 38, cs, 8); }
            wtx(d, p, 106, al[0], 13, 0xFFFF, cw, W_SEMI, WA_L);
            wtx(d, p, 122, at[0], 12, C_SEC, cw, W_REG, WA_L);
        } else {
            int cols = sz == 1 ? 4 : 3, gap = sz == 1 ? 10 : 14, cs = (cw - (cols - 1) * gap) / cols, rows = sz == 1 ? 1 : 2;
            for (int i = 0; i < cols * rows && i < n; i++) {
                int x = p + (i % cols) * (cs + gap), y = (sz == 1 ? 38 : 42) + (i / cols) * (cs + 44);
                if (hc[i]) wg_cover_blit(d, wg_rcov[i], WG_RCS, x, y, cs, 8); else wg_art_ph(d, x, y, cs, 8);
                wtx(d, x, y + cs + 15, al[i], sz == 1 ? 11 : 12, 0xFFFF, cs, W_SEMI, WA_L);
                wtx(d, x, y + cs + 29, at[i], sz == 1 ? 10 : 11, C_SEC, cs, W_REG, WA_L);
            }
        }
        pthread_mutex_unlock(&wg_mu);
        break; }
    case WG_LISTEN: {
        int tot = 0; for (int k = 0; k < 7; k++) tot += D->lis_day[k];
        int colw = sz ? 118 : cw;
        wlb(d, p, 28, tr("Escuchas"), 12, C_PINK, colw, W_SEMI, WA_L);
        snprintf(b, sizeof b, "%d", D->lis_day[6] / 60);
        wtx(d, p - 1, sz ? 72 : 66, b, sz ? 36 : 34, 0xFFFF, colw, W_SEMI, WA_L);
        wlb(d, p, sz ? 89 : 83, wgt("min hoy"), 12, C_SEC, colw, W_REG, WA_L);
        if (!sz) { wg_week(d, p, 94, cw, 29, D->lis_day, 0); break; }
        for (int y = 16; y < h - 16; y++) wg_px(d, p + colw + 9, y, 0xFFFF, 30);
        wlb(d, p, 108, tr("Artista más escuchado"), 11, C_SEC, colw, W_REG, WA_L);
        wtx(d, p, 124, D->top_artist[0] ? D->top_artist : "—", 14, 0xFFFF, colw, W_SEMI, WA_L);
        int x1 = p + colw + 20, rw = w - p - x1;
        wlb(d, x1, 26, wgt("Esta semana"), 11, C_SEC, rw, W_SEMI, WA_L);
        wtx(d, x1, 42, wg_mins(tot, s1, sizeof s1), 14, 0xFFFF, rw, W_SEMI, WA_L);
        wg_week(d, x1, 52, rw, 72, D->lis_day, 1);
        break; }
    case WG_MIX: {
        if (!sz) {
            fill_circle(d, p + 15, p + 15, 15, 0xFFFF, 52); wg_note(d, p + 15.f, p + 15.f, 16, 0xFFFF);
            const char *nm = tr(wg_mixn[0]), *ds = tr("Lo de siempre y algo nuevo, cada día");
            int l2 = wt2_lines(ds, 11, cw, W_REG), l1 = wt2_lines(nm, 16, cw, W_SEMI), yb = h - p - 1 - (l2 - 1) * 14;
            wt2(d, p, yb, ds, 11, rgb(238, 226, 232), cw, 14, WA_L, W_REG, 1);
            wt2(d, p, yb - 18 - (l1 - 1) * 19, nm, 16, 0xFFFF, cw, 19, WA_L, W_SEMI, 1);
        } else {
            wlb(d, p, 28, tr("Mezclas"), 12, C_PINK, cw, W_SEMI, WA_L);
            int cs = 66;
            for (int k = 0; k < 3; k++) {
                int x = p + k * (cw - cs) / 2; wg_mix_tile(d, x, 36, cs, 12, k);
                wt2(d, x - 6, 36 + cs + 14, tr(wg_mixn[k]), 11, 0xFFFF, cs + 12, 12, WA_C, W_SEMI, 1);
            }
        }
        break; }
    }
}

/* ------------------------------------------------------------------ sprites */
struct wspr { uint16_t *px; uint32_t sig; int nb; struct wbtn b[3]; };
static struct wspr wg_spr[WG_N][3];
static uint32_t wg_hs(uint32_t h, const char *s) { while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static int wg_prog_px(int sz)
{
    if (!wg_ni.source || wg_ni.duration_ms <= 0 || wg_ni.position_ms < 0) return -1;
    if (sz == 2) return wg_ni.position_ms / 1000;
    int pos = wg_ni.position_ms > wg_ni.duration_ms ? wg_ni.duration_ms : wg_ni.position_ms;
    return (int)((long long)pos * (sz ? 137 : 109) / wg_ni.duration_ms);
}
static int wg_card_tint(int type, int sz) { return type == WG_NOW ? (wg_ni.source && wg_tint >= 0 ? wg_tint : -1) : type == WG_MIX && !sz ? 0xD8355E : -1; }
static uint32_t wg_sig(int type, int sz, int sx, int sy)
{
    uint32_t h = 2166136261u; const struct wdata *D = &WDs;
#define HM(v) h = (h ^ (uint32_t)(v)) * 16777619u
    HM(type); HM(sz); HM(sx); HM(sy); HM(ui_lang); HM(wall_gen);
    switch (type) {
    case WG_BATT: HM(D->batt); HM(D->charging); HM(D->usb); HM(D->nbt); HM(pw_mode); for (int i = 0; i < D->nbt; i++) { HM(D->bt[i].pct); h = wg_hs(h, D->bt[i].name); }
        HM(D->nhp); HM(D->nhps); for (int i = 0; i < D->nhp; i++) { HM(D->hp[i].pct); HM(D->hp[i].chg); HM(D->hp[i].fam); HM(D->hp[i].part); }
        for (int i = 0; i < D->nhps; i++) { HM(D->hps[i].pct); HM(D->hps[i].chg); HM(D->hps[i].fam); } break;
    case WG_NOW: HM(wg_ni.source); HM(wg_ni.track_id); HM(wg_playing()); HM(wg_cok); HM(wg_tint); HM(wg_prog_px(sz)); h = wg_hs(h, wg_ni.title); h = wg_hs(h, wg_ni.artist);
        if (sz == 2) { h = wg_hs(h, wg_ni.album); HM(wg_ni.rate); HM(wg_ni.bits); HM(wg_ni.duration_ms / 1000); } break;
    case WG_DATE: { time_t now = time(0); struct tm tm; localtime_r(&now, &tm); HM(tm.tm_yday); HM(tm.tm_year); break; }
    case WG_CLOCK: { time_t now = time(0); struct tm tm; localtime_r(&now, &tm); HM(tm.tm_yday); HM(tm.tm_hour); HM(tm.tm_min); break; }
    case WG_STORAGE: HM(D->sd_free >> 24); HM(D->sd_tot >> 24); HM(D->in_free >> 24); HM(D->in_tot >> 24); break;
    case WG_POWER: HM(pw_mode); HM(lcd_hz); HM(pw_nocharge); HM(D->charging); HM(D->usb); HM(D->batt); break;
    case WG_LIB: HM(D->songs); HM(D->albums); HM(D->artists); HM(D->favs); HM(D->lib_bytes >> 20); break;
    case WG_NEW: HM(D->nrec); HM(D->rc_gen); for (int i = 0; i < D->nrec; i++) h = wg_hs(h, D->rec_alb[i]); h = wg_hs(h, D->new_album); h = wg_hs(h, D->new_artist); break;
    case WG_LISTEN: { time_t now = time(0); struct tm tm; localtime_r(&now, &tm); HM(tm.tm_wday);
        for (int k = 0; k < 7; k++) HM(D->lis_day[k] / 60); h = wg_hs(h, D->top_artist); HM(D->top_n); break; }
    case WG_MIX: break;
    }
#undef HM
    return h ? h : 1;
}
/* dibuja el widget en wg_scr (filas 0..alto) con el cristal de la posicion (sx, sy) */
static void wg_draw_scr(int type, int sz, int sx, int sy)
{
    wg_masks_init();
    int w = wg_w(sz), h = wg_h(sz), c0 = wg_cy0, c1 = wg_cy1; wg_cy0 = 0; wg_cy1 = WG_SH;
    wg_card_bg(wg_scr, w, h, sx, sy, sz, wg_card_tint(type, sz));
    wg_render(type, sz, wg_scr, w, h);
    wg_cy0 = c0; wg_cy1 = c1;
}
static int wg_build(int type, int sz, int sx, int sy)
{
    struct wspr *s = &wg_spr[type][sz]; int w = wg_w(sz), h = wg_h(sz);
    if (!s->px) s->px = malloc(w * h * 2);
    if (!s->px) return 0;
    wg_draw_scr(type, sz, sx, sy);
    for (int y = 0; y < h; y++) memcpy(s->px + y * w, wg_scr + y * W, w * 2);
    s->sig = wg_sig(type, sz, sx, sy);
    s->nb = wg_nbcur; memcpy(s->b, wg_bcur, sizeof s->b);
    return 1;
}
static void wg_free_unused(void)
{
    for (int t = 0; t < WG_N; t++) for (int s = 0; s < 3; s++) if (wg_spr[t][s].px && !wg_has(t, s)) { free(wg_spr[t][s].px); wg_spr[t][s].px = 0; }
}

#ifndef WG_RENDER_ONLY
/* ------------------------------------------------------------------ pagina (estatica) */
static float wg_scroll, wg_cache_scroll; static int wg_cache_min;
static uint16_t *wg_cache;
static int wg_budget = 99;                   /* sprites que se pueden rehacer en esta composicion */
static int wg_part_y0, wg_part_y1;           /* filas cambiadas (presentacion parcial) */
static int wg_press = -1, wg_press_btn = -1, wg_scrolling;
static int wg_snap_ok;
static double wg_pf_build; static int wg_pf_nb;   /* perfil: sprites rehechos y su tiempo */
static int wg_content_h(void) { short xs[WG_MAX], ys[WG_MAX]; return wg_layout_items(xs, ys) + 46; }
/* compone la pagina en dst (con el fondo si copy_wall; si no, dst ya lo tiene) */
/* Fondo de la pagina de widgets (como la vista Hoy de iOS): el MISMO fondo de pantalla, quieto,
 * que se desenfoca y oscurece segun entra la pagina. Antes la pagina traia su copia del fondo y
 * se movia con ella: al deslizar se veia el corte entre los dos fondos. */
static uint16_t *wg_wblur;                         /* fondo desenfocado y oscurecido (se suelta en reposo) */
static uint16_t wg_wblur_of;                       /* firma del fondo del que salio */
static const uint16_t *wg_wall_blur(void)
{
    uint16_t sig = (uint16_t)(wall[40 * W + 7] ^ wall[200 * W + 160] ^ wall[460 * W + 300] ^ wall[300 * W + 31]);
    if (wg_wblur && wg_wblur_of == sig) return wg_wblur;
    if (!wg_wblur) wg_wblur = lazy_alloc((void **)&wg_wblur, HM_FRAME_BYTES, 0);
    if (!wg_wblur) return 0;
    double t0 = now_ms();
    cc_blur(wall, wg_wblur);
    for (int i = 30 * W; i < W * H; i++) wg_wblur[i] = dim565(wg_wblur[i], 25);
    wg_wblur_of = sig;
    lg("widgets: fondo desenfocado en %.0f ms", now_ms() - t0);
    return wg_wblur;
}
/* filas 30..H: fondo con desenfoque k/32 (0 = nitido, 32 = el de la pagina de widgets) */
static void wg_bg(uint16_t *dst, int k)
{
    const uint16_t *b = k > 0 ? wg_wall_blur() : 0;
    if (!b || k <= 0) { memcpy(dst + 30 * W, wall + 30 * W, (H - 30) * W * 2); return; }
    if (k >= 32) memcpy(dst, b, HM_FRAME_BYTES);
    else for (int i = 0; i < W * H; i++) dst[i] = blend565(wall[i], b[i], k);
    compose_statusbar_on(dst, dst);                /* la barra de estado, desenfocada igual (sin costura) */
}
static int wg_ox;                                  /* desplazamiento horizontal de los widgets al deslizar */
static void wg_compose_to(uint16_t *dst, int copy_wall)
{
    wg_masks_init();
    if (wg_fresh || !wg_snap_ok) { wg_fresh = 0; wg_snap(); wg_snap_ok = 1; }
    if (copy_wall) wg_bg(dst, 32);
    short xs[WG_MAX], ys[WG_MAX]; int tot = wg_layout_items(xs, ys);
    int top = WG_TOP - (int)wg_scroll, pending = 0, y0 = H, y1 = 0, c0 = wg_cy0, c1 = wg_cy1;
    for (int i = 0; i < wg_n; i++) {
        int t = wg_it[i].type, s = wg_it[i].size, x = xs[i], y = top + ys[i], hh = wg_h(s);
        if (y >= H || y + hh <= 30) continue;
        struct wspr *sp = &wg_spr[t][s];
        uint32_t sig = wg_sig(t, s, x, WG_TOP + ys[i]);
        if (!sp->px || sp->sig != sig) {
            if (!sp->px || wg_budget > 0) {
                double tb = now_ms();
                wg_build(t, s, x, WG_TOP + ys[i]); wg_budget--;
                wg_pf_build += now_ms() - tb; wg_pf_nb++;
                if (y < y0) y0 = y; if (y + hh > y1) y1 = y + hh;
            } else pending = 1;
        }
        if (sp->px) wg_blit_m(dst, sp->px, wg_w(s), wg_mask[s], wg_w(s), hh, x + wg_ox, y, 30, (int)WG_R);
    }
    /* "Editar" al final (como iOS) */
    int ey = top + tot + 4;
    wg_cy0 = 30; wg_cy1 = H;
    if (!wg_n) wlb(dst, 20 + wg_ox, top + 150, tr("Sin widgets"), 15, rgb(235, 235, 240), W - 40, W_MED, WA_C), ey = top + 170;
    int bw = wg_tw(tr("Editar"), 14, W_SEMI) + 32; if (bw > W - 40) bw = W - 40;
    int bx = (W - bw) / 2 + wg_ox;
    if (ey < H && ey + 30 > 30 && bx > -bw && bx < W) { fill_rrect(dst, bx, ey, bw, 30, 15, rgb(20, 20, 24), 120); wlb(dst, bx + 16, ey + 20, tr("Editar"), 14, 0xFFFF, bw - 32, W_SEMI, WA_C); }
    wg_cy0 = c0; wg_cy1 = c1;
    if (pending) wg_fresh = 1;
    if (y1 > y0) { wg_part_y0 = y0 < 30 ? 30 : y0; wg_part_y1 = y1 > H ? H : y1; } else { wg_part_y0 = 30; wg_part_y1 = 31; }
}
static void wg_compose(void)
{
    if (!wg_loaded && !widgets_on && V.scroll >= 0) { if (!WD.t) wg_refresh(); return; }   /* precomposicion: esperar a los datos */
    if (!wg_cache) wg_cache = malloc(HM_FRAME_BYTES);
    if (!wg_cache) return;
    int pre = !widgets_on && !wg_cache_ok; double t0 = now_ms(); int nb0 = wg_pf_nb;
    wg_compose_to(wg_cache, 1);
    if (pre) lg("widgets: pagina precompuesta (%d sprites, %.0f ms)", wg_pf_nb - nb0, now_ms() - t0);
    wg_cache_min = (int)(time(0) / 60); wg_cache_scroll = wg_scroll; wg_cache_ok = 1;
}
static void wg_np_poll(void)        /* la pista o el estado cambiaron (sin copiar la caratula) */
{
    pthread_mutex_lock(&npx_mu);
    int ch = npx.track_id != wg_ni.track_id || npx.playing != wg_ni.playing || npx.source != wg_ni.source || npx_cov_ok != wg_cok;
    pthread_mutex_unlock(&npx_mu);
    if (ch) wg_fresh = 1;
}
/* dibuja la pagina con su borde izquierdo en ox (ox = -W .. 0) */
static void widgets_draw(uint16_t *d, int ox)
{
    if (ox <= -W || ox >= W) return;
    static double pf_t, pf_acc, pf_cmp; static int pf_n, pf_nc;
    double pf0 = now_ms(), pf1 = pf0;
    /* a la vista: cada 4 s (un auricular recien conectado sale enseguida); si no, cada 10 s */
    if (!WD.t || now_ms() - WD.t > (ox == 0 ? 4000 : 10000)) wg_refresh();
    if (npx_changed && widgets_on) npx_changed = 0;
    wg_np_poll();
    int settled = ox == 0 && !wg_scrolling, sch = wg_cache_scroll != wg_scroll;
    if (ox == 0 && wg_scrolling) {
        /* desplazamiento vertical: los sprites van directos sobre el fondo que ya puso
         * render_launcher (sin la copia a wg_cache y de vuelta: 2 x 290 KB menos por fotograma) */
        wg_bg(d, 32);            /* el fondo desenfocado (antes quedaba el nitido del inicio al desplazar) */
        wg_budget = 0; wg_compose_to(d, 0); wg_budget = 99; pf_cmp += now_ms() - pf0; pf_nc++;
        wg_part_y0 = wg_part_y1 = 0; wg_cache_ok = 0; wg_cache_scroll = wg_scroll;
        goto pressed;
    }
    if (!wg_cache_ok || wg_fresh || sch || (int)(time(0) / 60) != wg_cache_min) {
        int data_only = wg_cache_ok && !sch && settled;
        wg_budget = settled ? 99 : 0;
        wg_compose(); wg_budget = 99; pf1 = now_ms(); pf_cmp += pf1 - pf0; pf_nc++;
        if (!data_only) wg_part_y0 = wg_part_y1 = 0;
    } else wg_part_y0 = wg_part_y1 = 0;
    if (!wg_cache) return;
    if (ox != 0) {
        /* entrando o saliendo: el fondo no se mueve, se desenfoca en proporcion a lo que se ve
         * de la pagina, y solo los widgets se deslizan encima (sin corte entre dos fondos) */
        int vis = W - (ox < 0 ? -ox : ox), k = vis * 32 / W;
        wg_bg(d, k);
        wg_ox = ox; wg_budget = 0; wg_compose_to(d, 0); wg_budget = 99; wg_ox = 0;
        wg_part_y0 = wg_part_y1 = 0;
        goto pressed;
    }
    for (int y = 30; y < H; y++) memcpy(d + y * W, wg_cache + y * W, W * 2);
    { const uint16_t *bl = wg_wall_blur(); if (bl) compose_statusbar_on(d, bl); }   /* barra sobre el fondo desenfocado */
pressed:
    if (wg_press >= 0 && wg_press < wg_n && ox == 0) {   /* pulsado: el widget se hunde un poco (o se ilumina el boton) */
        short xs[WG_MAX], ys[WG_MAX]; wg_layout_items(xs, ys);
        int t = wg_it[wg_press].type, s = wg_it[wg_press].size, w = wg_w(s), h = wg_h(s), x = xs[wg_press], y = WG_TOP - (int)wg_scroll + ys[wg_press];
        struct wspr *sp = &wg_spr[t][s];
        if (wg_press_btn >= 0 && wg_press_btn < sp->nb) { const struct wbtn *b = &sp->b[wg_press_btn]; int c0 = wg_cy0; wg_cy0 = 30; fill_circle(d, x + b->x, y + b->y, b->r - 3, 0xFFFF, 56); wg_cy0 = c0; }
        else if (sp->px) {
            for (int yy = y < 30 ? 30 : y; yy < y + h && yy < H; yy++) memcpy(d + yy * W + x, wall + yy * W + x, w * 2);
            wg_blit_xf(d, 30, sp->px, w, wg_mask[s], w, h, x + w / 2.f, y + h / 2.f, 0, 0.955f, 255, (int)WG_R);
        }
        wg_part_y0 = wg_part_y1 = 0;
    }
    /* perfil (shell.log): cada ~3 s con actividad, tiempo medio por fotograma, composiciones y sprites rehechos */
    pf_acc += now_ms() - pf0; pf_n++;
    if (now_ms() - pf_t > 3000) {
        if (pf_n > 20 || wg_pf_nb) lg("perfil widgets: %d fot, %.2f ms/fot; %d composiciones (%.1f ms); %d sprites rehechos (%.1f ms c/u)", pf_n, pf_acc / pf_n, pf_nc, pf_nc ? pf_cmp / pf_nc : 0,
                                      wg_pf_nb, wg_pf_nb ? wg_pf_build / wg_pf_nb : 0);
        pf_t = now_ms(); pf_acc = pf_cmp = 0; pf_n = pf_nc = 0; wg_pf_build = 0; wg_pf_nb = 0;
    }
}
/* widget en (px, py) en la pagina normal: indice, -2 = Editar, -1 nada; *btn = boton del sprite */
static int wg_hit(int px, int py, int *btn)
{
    short xs[WG_MAX], ys[WG_MAX]; int tot = wg_layout_items(xs, ys), top = WG_TOP - (int)wg_scroll;
    *btn = -1;
    for (int i = 0; i < wg_n; i++) {
        int s = wg_it[i].size, w = wg_w(s), h = wg_h(s), x = xs[i], y = top + ys[i];
        if (px >= x && px < x + w && py >= y && py < y + h) {
            const struct wspr *sp = &wg_spr[wg_it[i].type][s];
            for (int k = 0; k < sp->nb; k++) { int dx = px - (x + sp->b[k].x), dy = py - (y + sp->b[k].y); if (dx * dx + dy * dy <= sp->b[k].r * sp->b[k].r) *btn = k; }
            return i;
        }
    }
    int ey = top + (wg_n ? tot + 4 : 170);
    if (py >= ey - 6 && py < ey + 36 && abs(px - W / 2) < 70) return -2;
    return -1;
}

/* ------------------------------------------------------------------ modo edicion */
static float wg_shift;
static int wg_edit_on;
static double wg_t0;
static void wg_targets(int drag, int slot, short *tx, short *ty, int *tot)
{
    int ord[WG_MAX], n = 0;
    for (int i = 0; i < wg_n; i++) if (i != drag) ord[n++] = i;
    if (drag >= 0) { if (slot < 0) slot = 0; if (slot > n) slot = n; memmove(&ord[slot + 1], &ord[slot], (n - slot) * sizeof(int)); ord[slot] = drag; n++; }
    unsigned char sz[WG_MAX]; short xs[WG_MAX], ys[WG_MAX];
    for (int k = 0; k < n; k++) sz[k] = wg_it[ord[k]].size;
    int t = wg_layout(sz, n, xs, ys); if (tot) *tot = t;
    for (int k = 0; k < n; k++) { tx[ord[k]] = xs[k]; ty[ord[k]] = ys[k]; }
}
static void wg_plus(uint16_t *d, int cx, int cy, int a)
{
    fill_rrect(d, cx - 7, cy - 1, 14, 3, 1.2f, 0xFFFF, a); fill_rrect(d, cx - 1, cy - 7, 3, 14, 1.2f, 0xFFFF, a);
}
static double wg_pf_bg, wg_pf_bl;
static void wg_edit_frame(uint16_t *d, double t, float pa, int drag)
{
    double q0 = now_ms();
    compose_statusbar(d);
    memcpy(d + 30 * W, wall + 30 * W, (H - 30) * W * 2);
    wg_pf_bg += now_ms() - q0;
    int top = WG_TOP + (int)wg_shift - (int)wg_scroll;
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < wg_n; i++) {
        if ((pass == 0) == (i == drag)) continue;
        struct witem *it = &wg_it[i]; struct wspr *sp = &wg_spr[it->type][it->size];
        int w = wg_w(it->size), h = wg_h(it->size); float cx = it->ax + w / 2.f, cy = top + it->ay + h / 2.f;
        if (!sp->px || cy - h > H || cy + h < 30) continue;
        if (i == drag) {   /* levantado: sombra suave y un poco mas grande, sin temblor */
            const uint8_t *sh = wg_shadow_get(it->size); int sw = w + 2 * WG_SP, shh = h + 2 * WG_SP;
            int sx0 = (int)(cx - sw / 2.f), sy0 = (int)(cy - shh / 2.f) + 8;
            if (sh) for (int y = 0; y < shh; y++) { int Y = sy0 + y; if (Y < 30 || Y >= H) continue;
                for (int x = 0; x < sw; x++) { int X = sx0 + x; if (X < 0 || X >= W) continue; int a = sh[y * sw + x] * 100 >> 8; if (a) d[Y * W + X] = wg_mix(0, d[Y * W + X], a); } }
            wg_blit_xf(d, 30, sp->px, w, wg_mask[it->size], w, h, cx, cy, 0, 1.04f, 255, (int)WG_R);
            continue;
        }
        float amp = 1.3f / (w / 2.f);           /* +-1,3 px en el borde: igual para todos los tamanos */
        float ang = pa > 0 ? amp * pa * sinf((float)((t - wg_t0) / 260.0 * 6.2831853) + i * 1.9f) : 0;
        double q1 = now_ms();
        wg_blit_xf(d, 30, sp->px, w, wg_mask[it->size], w, h, cx, cy, ang, it->sc, (int)(255 * it->al), (int)WG_R);
        wg_pf_bl += now_ms() - q1;
        if (pa > 0.01f && !it->rm) {   /* "−" en la esquina, girando con el widget */
            float ox = (-w / 2.f + 9) * it->sc, oy = (-h / 2.f + 9) * it->sc, c = cosf(ang), s = sinf(ang);
            float bx = cx + ox * c - oy * s, by = cy + ox * s + oy * c; int a = (int)(255 * pa * it->al);
            if (by > 40) { fill_circle(d, bx, by, 12 * it->sc, rgb(232, 232, 237), a); fill_rrect(d, (int)(bx - 6), (int)(by - 1), 12, 3, 1.2f, rgb(28, 28, 30), a); }
        }
    }
    if (pa > 0.01f) {   /* pildoras: "+" (galeria) y "Listo" */
        int a = (int)(64 * pa), ty = 34 - (int)(8 * (1 - pa));
        fill_rrect(d, 14, ty, 44, 30, 15, 0xFFFF, a); wg_plus(d, 36, ty + 15, (int)(255 * pa));
        int dw = pill_done.w + 30; fill_rrect(d, W - 14 - dw, ty, dw, 30, 15, 0xFFFF, a);
        blit(d, &pill_done, W - 14 - dw + 15, ty + (30 - pill_done.h) / 2, 32, (int)(255 * pa));
    }
    draw_toast(d);
}
/* toque: >= 0 widget, -1 vacio, -10 "+", -11 "Listo", -100-i "−" del widget i */
static int wg_edit_hit(int x, int y, int drag)
{
    if (y >= 28 && y < 70) { if (x < 66) return -10; if (x > W - 110) return -11; }
    int top = WG_TOP + (int)wg_shift - (int)wg_scroll;
    for (int i = wg_n - 1; i >= 0; i--) {
        if (i == drag || wg_it[i].rm) continue;
        float bx = wg_it[i].ax + 9, by = top + wg_it[i].ay + 9;
        if ((x - bx) * (x - bx) + (y - by) * (y - by) < 20 * 20) return -100 - i;
    }
    for (int i = 0; i < wg_n; i++) {
        if (wg_it[i].rm) continue;
        int w = wg_w(wg_it[i].size), h = wg_h(wg_it[i].size), wx = (int)wg_it[i].ax, wy = top + (int)wg_it[i].ay;
        if (x >= wx && x < wx + w && y >= wy && y < wy + h) return i;
    }
    return -1;
}
static int wg_gallery(int *otype, int *osize);
/* bucle del modo edicion. press: widget pulsado (pulsacion larga en curso, puede arrastrarse
 * sin levantar el dedo); embedded: abierto desde Ajustes (ya entra con las pildoras y sale sin
 * animar: quien llama hace la transicion) */
static void wg_edit(int press, int embedded)
{
    wg_masks_init(); wg_snap(); wg_snap_ok = 1;
    short tx[WG_MAX], ty[WG_MAX]; int tot;
    wg_targets(-1, -1, tx, ty, &tot);
    for (int i = 0; i < wg_n; i++) {
        struct witem *it = &wg_it[i];
        it->ax = tx[i]; it->ay = ty[i]; it->sc = it->al = 1; it->rm = 0;
        if (!wg_spr[it->type][it->size].px) wg_build(it->type, it->size, tx[i], WG_TOP + ty[i]);
    }
    wg_edit_on = 1; wg_press = -1;
    double t0 = now_ms(), tl = t0, exit_t = 0; wg_t0 = t0;
    float pa = embedded ? 1 : 0, sh0 = embedded ? WG_ESH : 0;
    wg_shift = sh0;
    int down = cur_down, tst = 0, target = -1, sx = cur_x, sy = cur_y, drag = -1, slot = -1, exiting = 0, scroll_to0 = 0;
    float gdx = 0, gdy = 0, s0 = 0, vel = 0; double lt = t0, dn_t = t0 - 1000; int ly = sy;   /* dn_t: el toque que abrio la edicion ya es largo */
    if (press >= 0 && down) { tst = 1; target = press; }
    static double prof_t; static int prof_n; static double prof_acc;
    for (;;) {
        double t = now_ms(); float dt = (float)(t - tl); if (dt > 50) dt = 50; tl = t;
        struct pkt p; int prev = down;
        while (read_pkt(&p, 0)) {}
        down = cur_down; int fx = cur_x, fy = cur_y;
        int topc = WG_TOP + (int)wg_shift - (int)wg_scroll;
        float maxs = tot + WG_TOP + WG_ESH + 10 - H; if (maxs < 0) maxs = 0;
        if (!exiting) {
            if (down && !prev) { sx = fx; sy = fy; tst = 1; target = wg_edit_hit(fx, fy, -1); vel = 0; s0 = wg_scroll; ly = fy; lt = t; dn_t = t; }
            /* como en iOS: el widget se levanta al mantenerlo pulsado (350 ms); deslizar antes desplaza la pagina */
            if (down && tst == 1 && target >= 0 && t - dn_t >= 350 && abs(fx - sx) <= 8 && abs(fy - sy) <= 8) {
                drag = target; slot = target; tst = 2;
                gdx = sx - wg_it[drag].ax; gdy = sy - (topc + wg_it[drag].ay);
            }
            if (down && tst == 1 && (abs(fx - sx) > 8 || abs(fy - sy) > 8)) {
                if (sy >= 452 && sy - fy > 6) { exiting = 1; exit_t = t; }   /* gesto de inicio: salir */
                else if (target >= 0 && t - dn_t >= 350) {     /* arrastrar el widget */
                    drag = target; slot = target; tst = 2;
                    gdx = sx - wg_it[drag].ax; gdy = sy - (topc + wg_it[drag].ay);
                } else { tst = 3; s0 = wg_scroll; }
            }
            if (down && tst == 2) {
                struct witem *it = &wg_it[drag];
                if (fy > H - 44) wg_scroll += 0.3f * dt; else if (fy < 84) wg_scroll -= 0.3f * dt;
                if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > maxs) wg_scroll = maxs;
                topc = WG_TOP + (int)wg_shift - (int)wg_scroll;
                it->ax = fx - gdx; it->ay = fy - gdy - topc;
                /* hueco mas cercano al centro del widget arrastrado (con histeresis) */
                float best = 1e9f, cur = 1e9f; int bk = slot;
                for (int k = 0; k < wg_n; k++) {
                    short ax[WG_MAX], ay[WG_MAX]; wg_targets(drag, k, ax, ay, 0);
                    float ddx = ax[drag] - it->ax, ddy = ay[drag] - it->ay, dd = ddx * ddx + ddy * ddy;
                    if (dd < best) { best = dd; bk = k; } if (k == slot) cur = dd;
                }
                if (bk != slot && best + 300 < cur) slot = bk;
            }
            if (down && tst == 3) {
                if (t > lt) { vel = 0.6f * vel + 0.4f * (fy - ly) / (float)(t - lt); ly = fy; lt = t; }
                wg_scroll = s0 - (fy - sy); if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > maxs) wg_scroll = maxs;
            }
            if (!down && prev) {
                if (tst == 2) {            /* soltar: el orden nuevo */
                    int n = 0, ord[WG_MAX]; struct witem tmp[WG_MAX];
                    for (int i = 0; i < wg_n; i++) if (i != drag) ord[n++] = i;
                    int s = slot > n ? n : slot; memmove(&ord[s + 1], &ord[s], (n - s) * sizeof(int)); ord[s] = drag;
                    for (int k = 0; k <= n; k++) tmp[k] = wg_it[ord[k]];
                    memcpy(wg_it, tmp, (n + 1) * sizeof tmp[0]);
                    drag = -1; slot = -1;
                } else if (tst == 3) { if (t - lt > 60) vel = 0; }
                else if (tst == 1) {
                    if (target <= -100) { int k = -100 - target; if (k < wg_n && !wg_it[k].rm) { wg_it[k].rm = 1; wg_it[k].rm_t = t; } }
                    else if (target == -10) {
                        int gt, gs;
                        if (wg_gallery(&gt, &gs) && !wg_has(gt, gs) && wg_n < WG_MAX) {
                            memmove(&wg_it[1], &wg_it[0], wg_n * sizeof wg_it[0]); wg_n++;
                            struct witem *it = &wg_it[0]; memset(it, 0, sizeof *it); it->type = gt; it->size = gs;
                            it->ax = WG_M; it->ay = 0; it->sc = 0.5f; it->al = 0;
                            wg_targets(-1, -1, tx, ty, &tot); wg_build(gt, gs, tx[0], WG_TOP + ty[0]);
                            scroll_to0 = 1;
                        }
                        tl = now_ms(); down = cur_down;
                    }
                    else if (target == -11 || target == -1) { exiting = 1; exit_t = t; }
                }
                tst = 0;
            }
            if (!down && tst == 0 && fabsf(vel) > 0.03f) {   /* inercia */
                wg_scroll -= vel * dt; vel *= powf(0.9975f, dt);
                if (wg_scroll < 0) { wg_scroll = 0; vel = 0; } if (wg_scroll > maxs) { wg_scroll = maxs; vel = 0; }
            }
        }
        /* animacion: posiciones, quitar, aparecer, pildoras */
        wg_targets(drag, slot, tx, ty, &tot);
        float k = 1 - expf(-dt / 65.f);
        for (int i = 0; i < wg_n; i++) {
            struct witem *it = &wg_it[i];
            if (it->rm) { float e = (float)((t - it->rm_t) / 220); if (e > 1) e = 1; float q = (float)ease_out(e); it->sc = 1 - 0.75f * q; it->al = 1 - q; }
            else { it->sc += (1 - it->sc) * k; it->al += (1 - it->al) * k; if (fabsf(1 - it->sc) < 0.002f) it->sc = 1; if (it->al > 0.998f) it->al = 1; }
            if (i != drag) { it->ax += (tx[i] - it->ax) * k; it->ay += (ty[i] - it->ay) * k; if (fabsf(tx[i] - it->ax) < 0.3f) it->ax = tx[i]; if (fabsf(ty[i] - it->ay) < 0.3f) it->ay = ty[i]; }
        }
        for (int i = 0; i < wg_n; i++) if (wg_it[i].rm && t - wg_it[i].rm_t >= 220) {
            memmove(&wg_it[i], &wg_it[i + 1], (wg_n - i - 1) * sizeof wg_it[0]); wg_n--;
            if (drag > i) drag--; if (slot > i) slot--; i--;
        }
        if (scroll_to0) { wg_scroll += (0 - wg_scroll) * k; if (wg_scroll < 0.5f) { wg_scroll = 0; scroll_to0 = 0; } }
        if (exiting) {
            if (embedded) break;
            double e = ease_out((t - exit_t) / 240.0 > 1 ? 1 : (t - exit_t) / 240.0);
            pa = 1 - (float)e; wg_shift = WG_ESH * (1 - (float)e);
        } else if (!embedded) {
            double e = ease_out((t - t0) / 250.0 > 1 ? 1 : (t - t0) / 250.0);
            pa = (float)e; wg_shift = WG_ESH * (float)e;
        }
        double tr0 = now_ms();
        wg_edit_frame(fr0, t, pa, drag);
        prof_acc += now_ms() - tr0; prof_n++;
        present();
        if (now_ms() - prof_t > 3000) { if (prof_n > 20) lg("perfil widgets (edicion): %d fot, dibujo %.1f ms (fondo %.1f, giro %.1f)", prof_n, prof_acc / prof_n, wg_pf_bg / prof_n, wg_pf_bl / prof_n);
            wg_pf_bg = wg_pf_bl = 0; prof_t = now_ms(); prof_n = 0; prof_acc = 0; }
        if (exiting && t - exit_t >= 240) break;
        /* (el cristal de los widgets movidos se rehace al salir: rehacerlo aqui costaria fotogramas) */
    }
    wg_edit_on = 0; wg_shift = 0;
    { float mx = wg_content_h() + WG_TOP - H; if (mx < 0) mx = 0; if (wg_scroll > mx) wg_scroll = mx; }
    wg_free_unused();
    wg_cache_ok = 0;
    lconf_save();
}

/* ------------------------------------------------------------------ galeria de widgets */
#define GS_Y 40                              /* la hoja deja ver arriba la pagina oscurecida */
#define GD_PREV 238                          /* detalle: centro de la vista previa (desde arriba de la hoja) */
#define GD_SEG 346                           /* detalle: control de tamano */
#define GD_ADD 390                           /* detalle: boton Anadir */
#define GD_LSC 0.68f                         /* detalle: escala de la vista previa grande */
static uint16_t wg_th[WG_N][WG_TH * WG_TH]; static int wg_th_ok[WG_N];
static void wg_thumb(int type)
{
    wg_draw_scr(type, 0, WG_M, WG_TOP);
    for (int y = 0; y < WG_TH; y++) for (int x = 0; x < WG_TH; x++) {
        int xa = x * WG_S / WG_TH, xb = (x + 1) * WG_S / WG_TH, ya = y * WG_S / WG_TH, yb = (y + 1) * WG_S / WG_TH, r = 0, g = 0, b = 0, n = 0;
        for (int yy = ya; yy < yb; yy++) for (int xx = xa; xx < xb; xx++) { uint16_t q = wg_scr[yy * W + xx]; r += q >> 11; g += (q >> 5) & 63; b += q & 31; n++; }
        wg_th[type][y * WG_TH + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
    }
    wg_th_ok[type] = 1;
}
static uint8_t wg_corner[14 * 14];
static void wg_x(uint16_t *d, int cx, int cy, int a)     /* boton de cerrar: circulo gris con aspa */
{
    fill_circle(d, cx, cy, 15, rgb(58, 58, 62), a);
    wg_line(d, cx - 5, cy - 5, cx + 5, cy + 5, 2.4f, rgb(200, 200, 206), a); wg_line(d, cx - 5, cy + 5, cx + 5, cy - 5, 2.4f, rgb(200, 200, 206), a);
}
static void wg_chev(uint16_t *d, int x, int cy, uint16_t col) { wg_line(d, x + 8, cy - 8, x, cy, 3, col, 255); wg_line(d, x, cy, x + 8, cy + 8, 3, col, 255); }
struct wg_gal { const uint16_t *bg; int top, view, sel, selsz, press, prev_ok; float ls, push; };
#define GROW 78
#define GLIST 64
static int wg_gal_sizes(int t, int *o) { int n = 0; for (int s = 0; s < 3; s++) if (wg_size_ok(t, s)) o[n++] = s; return n; }
static void wg_gal_list(uint16_t *d, const struct wg_gal *g, int ox)
{
    int top = g->top;
    wg_cy0 = top + GLIST - 4; wg_cy1 = H;
    for (int i = 0; i < WG_N; i++) {
        int ry = top + GLIST + i * GROW - (int)g->ls;
        if (ry + GROW < wg_cy0 || ry > H) continue;
        if (g->press == i) for (int y = ry; y < ry + GROW; y++) { if (y < wg_cy0 || y >= H) continue; for (int x = 0; x < W; x++) { int X = x + ox; if (X >= 0 && X < W) d[y * W + X] = rgb(44, 44, 47); } }
        if (wg_th_ok[i]) wg_blit_m(d, wg_th[i], WG_TH, wg_mask[3], WG_TH, WG_TH, 16 + ox, ry + 9, wg_cy0, 13);
        else { for (int y = 0; y < WG_TH; y++) { int Y = ry + 9 + y; if (Y < wg_cy0 || Y >= H) continue; for (int x = 0; x < WG_TH; x++) { int a = wg_mask[3][y * WG_TH + x], X = 16 + ox + x; if (a && X >= 0 && X < W) d[Y * W + X] = mix565(rgb(58, 58, 62), d[Y * W + X], a); } } }
        int tw = W - 90 - 36;
        wlb(d, 90 + ox, ry + 31, wgt(wg_name[i]), 16, 0xFFFF, tw, W_SEMI, WA_L);
        wt2(d, 90 + ox, ry + 50, wgt(wg_desc[i]), 12, C_SEC, tw, 15, WA_L, W_REG, 0);
        wg_cy0 = top + GLIST - 4;
        if (ry + 39 >= wg_cy0) { wg_line(d, W - 26 + ox, ry + 33, W - 21 + ox, ry + 39, 2, rgb(90, 90, 96), 255); wg_line(d, W - 21 + ox, ry + 39, W - 26 + ox, ry + 45, 2, rgb(90, 90, 96), 255); }
        int sy = ry + GROW - 1; if (sy >= wg_cy0 && sy < H && i < WG_N - 1) for (int x = 90; x < W; x++) { int X = x + ox; if (X >= 0 && X < W) d[sy * W + X] = rgb(56, 56, 58); }
    }
    wg_cy0 = 0; wg_cy1 = H;
    wlb(d, 16 + ox, top + 44, tr("Widgets"), 26, 0xFFFF, W - 80, W_BOLD, WA_L);
    if (ox > -W + 40) wg_x(d, W - 30 + ox, top + 30, 255);
}
static void wg_gal_detail(uint16_t *d, const struct wg_gal *g, int ox)
{
    int top = g->top, t = g->sel; if (t < 0) return;
    wg_cy0 = top; wg_cy1 = H;
    wg_chev(d, 14 + ox, top + 30, C_BLUE); wlb(d, 28 + ox, top + 36, tr("Widgets"), 16, C_BLUE, 120, W_REG, WA_L);
    wg_x(d, W - 30 + ox, top + 30, 255);
    wlb(d, 20 + ox, top + 76, wgt(wg_name[t]), 22, 0xFFFF, W - 40, W_BOLD, WA_C);
    wrap_cw = W - 48; draw_wrapped(d, 24 + ox, top + 86, W - 48, wgt(wg_desc[t]), 13, C_SEC, 17, 1); wrap_cw = 0;
    int sz = g->selsz, w = wg_w(sz), h = wg_h(sz), cy = top + GD_PREV;
    if (g->prev_ok) {
        if (sz == 2) wg_blit_xf(d, top, wg_scr, W, wg_mask[2], w, h, W / 2.f + ox, (float)cy, 0, GD_LSC, 255, (int)WG_R);
        else wg_blit_m(d, wg_scr, W, wg_mask[sz], w, h, (W - w) / 2 + ox, cy - h / 2, top, (int)WG_R);
    }
    /* tamano: control segmentado de iOS (los tamanos que tiene este widget) */
    int szs[3], ns = wg_gal_sizes(t, szs), sel = 0; for (int i = 0; i < ns; i++) if (szs[i] == sz) sel = i;
    static const char *const nm[3] = { "Pequeño", "Mediano", "Grande" };
    int sw = ns == 3 ? 264 : 220, sx = (W - sw) / 2 + ox, sy = top + GD_SEG, seg = sw / ns;
    fill_rrect(d, sx, sy, sw, 32, 9, rgb(52, 52, 56), 255);
    fill_rrect(d, sx + 2 + sel * seg, sy + 2, seg - 4, 28, 7, rgb(99, 99, 102), 255);
    for (int i = 0; i < ns; i++) wlb(d, sx + i * seg + 6, sy + 21, wgt(nm[szs[i]]), 13, 0xFFFF, seg - 12, i == sel ? W_SEMI : W_MED, WA_C);
    int have = wg_has(t, sz), by = top + GD_ADD;
    fill_rrect(d, 24 + ox, by, W - 48, 44, 22, have ? rgb(58, 58, 62) : C_BLUE, g->press == 100 ? 170 : 255);
    if (have) wlb(d, 40 + ox, by + 28, tr("Ya añadido"), 16, C_SEC, W - 80, W_SEMI, WA_C);
    else {
        const char *l = tr("Añadir widget"); int lw = wg_tw(l, 16, W_SEMI); if (lw > W - 110) lw = W - 110; int x = (W - lw - 20) / 2 + ox;
        wg_plus(d, x + 6, by + 22, 255); wlb(d, x + 20, by + 28, l, 16, 0xFFFF, W - 110, W_SEMI, WA_L);
    }
    wg_cy0 = 0; wg_cy1 = H;
}
static void wg_gal_frame(const struct wg_gal *g, float open)
{
    int k = 32 - (int)(13 * open), top = g->top;
    for (int y = 0; y < top && y < H; y++) for (int x = 0; x < W; x++) fr0[y * W + x] = dim565(g->bg[y * W + x], k);
    if (top >= H) { present(); return; }
    /* hoja con esquinas superiores redondeadas */
    uint16_t sc = rgb(28, 28, 30);
    for (int y = top; y < H; y++) {
        uint16_t *row = fr0 + y * W; int r = y - top;
        for (int x = 0; x < W; x++) row[x] = sc;
        if (r < 14) for (int x = 0; x < 14; x++) { int a = wg_corner[r * 14 + x]; if (a < 255) {
            row[x] = mix565(sc, dim565(g->bg[y * W + x], k), a); row[W - 1 - x] = mix565(sc, dim565(g->bg[y * W + W - 1 - x], k), a); } }
    }
    fill_rrect(fr0, W / 2 - 18, top + 6, 36, 5, 2.5f, rgb(90, 90, 96), 255);
    int ox = -(int)(W * g->push);
    if (g->push < 1) wg_gal_list(fr0, g, ox);
    if (g->push > 0) wg_gal_detail(fr0, g, ox + W);
    present();
}
static void wg_gal_preview(struct wg_gal *g)      /* vista previa real (con datos) del tamano elegido */
{
    int w = wg_w(g->selsz), h = wg_h(g->selsz);
    wg_draw_scr(g->sel, g->selsz, (W - w) / 2, GS_Y + GD_PREV - h / 2); g->prev_ok = 1;
}
/* galeria de widgets (hoja de iOS): lista con miniaturas reales -> detalle con vista previa,
 * tamano y "Añadir widget". Devuelve 1 con el elegido */
static int wg_gallery(int *otype, int *osize)
{
    wg_masks_init();
    { uint8_t *m = wg_rrmask(28, 28, 14, 0); if (m) { for (int y = 0; y < 14; y++) memcpy(wg_corner + y * 14, m + y * 28, 14); free(m); } }
    memcpy(img_base, fr0, HM_FRAME_BYTES);
    struct wg_gal g = { img_base, H, 0, -1, 0, -1, 0, 0, 0 };
    for (int i = 0; i < WG_N; i++) wg_th_ok[i] = 0;
    float maxs = GLIST + WG_N * GROW - (H - GS_Y) + 10; if (maxs < 0) maxs = 0;
    int result = 0;
    /* subir la hoja; las miniaturas se dibujan una por fotograma mientras sube */
    double t0 = now_ms();
    for (;;) {
        double e = ease_out((now_ms() - t0) / 300.0 > 1 ? 1 : (now_ms() - t0) / 300.0);
        g.top = GS_Y + (int)((H - GS_Y) * (1 - e));
        wg_gal_frame(&g, (float)e);
        for (int i = 0; i < WG_N; i++) if (!wg_th_ok[i]) { wg_thumb(i); break; }
        if (e >= 1) break;
    }
    struct pkt p; float vel = 0; int redraw = 1;
    for (;;) {
        int built = 0;
        for (int i = 0; i < WG_N && g.view == 0; i++) if (!wg_th_ok[i]) { wg_thumb(i); built = 1; break; }
        if (built || redraw) { wg_gal_frame(&g, 1); redraw = 0; }
        if (!read_pkt(&p, built ? 0 : 300) || !p.down) continue;
        redraw = 1;
        int sx = p.x, sy = p.y, moved = 0, drag_down = 0; float s0 = g.ls; double lt = now_ms(); int ly = sy;
        if (g.view == 0 && sy >= GS_Y + GLIST) g.press = (sy - GS_Y - GLIST + (int)g.ls) / GROW;
        if (g.view == 1 && sy >= GS_Y + GD_ADD && sy < GS_Y + GD_ADD + 44 && abs(sx - W / 2) < W / 2 - 24 && !wg_has(g.sel, g.selsz)) g.press = 100;
        if (g.press >= 0) wg_gal_frame(&g, 1);
        while (cur_down && read_pkt(&p, 3000)) {   /* hasta soltar: una pausa del dedo no es un toque */
            int dx = cur_x - sx, dy = cur_y - sy;
            if (!moved && (abs(dx) > 8 || abs(dy) > 8)) { moved = 1; g.press = -1; drag_down = sy < GS_Y + 56 && dy > 0; }
            if (!moved) continue;
            if (drag_down) { g.top = GS_Y + (dy > 0 ? dy : 0); wg_gal_frame(&g, 1 - (g.top - GS_Y) / (float)(H - GS_Y)); continue; }
            if (g.view == 0) {
                double t = now_ms(); if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_y - ly) / (float)(t - lt); ly = cur_y; lt = t; }
                g.ls = s0 - dy; if (g.ls < 0) g.ls = 0; if (g.ls > maxs) g.ls = maxs; wg_gal_frame(&g, 1);
            } else if (sx < BK_EDGE && dx > 0) { g.push = 1 - dx / (float)W; if (g.push < 0) g.push = 0; wg_gal_frame(&g, 1); }
        }
        int dx = cur_x - sx, dy = cur_y - sy;
        if (drag_down) {
            if (dy > 90) { result = 0; break; }
            float from = g.top; double tb = now_ms();
            for (;;) { double e = ease_out((now_ms() - tb) / 200.0 > 1 ? 1 : (now_ms() - tb) / 200.0); g.top = (int)(from + (GS_Y - from) * e); wg_gal_frame(&g, 1); if (e >= 1) break; }
            continue;
        }
        if (moved) {
            if (g.view == 0) {   /* inercia */
                if (now_ms() - lt > 60) vel = 0;
                double tp = now_ms();
                while (fabsf(vel) > 0.03f) {
                    double t = now_ms(); float ddt = (float)(t - tp); tp = t; if (ddt > 40) ddt = 40;
                    g.ls -= vel * ddt; vel *= powf(0.9975f, ddt);
                    if (g.ls < 0) { g.ls = 0; vel = 0; } if (g.ls > maxs) { g.ls = maxs; vel = 0; }
                    wg_gal_frame(&g, 1);
                    struct pkt q; if (read_pkt(&q, 0) && q.down) { vel = 0; break; }
                }
                vel = 0;
            } else if (sx < BK_EDGE) {   /* atras deslizando desde el borde */
                float from = g.push, to = dx > 60 ? 0 : 1; double tb = now_ms();
                for (;;) { double e = ease_out((now_ms() - tb) / 220.0 > 1 ? 1 : (now_ms() - tb) / 220.0); g.push = from + (to - from) * (float)e; wg_gal_frame(&g, 1); if (e >= 1) break; }
                if (to == 0) g.view = 0;
            } else if (abs(dx) > 40 && abs(dx) > abs(dy)) {   /* deslizar la vista previa: el tamano siguiente / anterior */
                int szs[3], ns = wg_gal_sizes(g.sel, szs), cur = 0; for (int i = 0; i < ns; i++) if (szs[i] == g.selsz) cur = i;
                int ni = cur + (dx < 0 ? 1 : -1); if (ni >= 0 && ni < ns) { g.selsz = szs[ni]; wg_gal_preview(&g); }
            }
            continue;
        }
        int pr = g.press; g.press = -1;
        if (sy >= GS_Y + 12 && sy < GS_Y + 50 && sx > W - 52) { result = 0; break; }   /* cerrar */
        if (g.view == 0) {
            if (pr >= 0 && pr < WG_N) {
                g.sel = pr; int szs[3], ns = wg_gal_sizes(pr, szs); g.selsz = szs[0];
                for (int i = 0; i < ns; i++) if (!wg_has(pr, szs[i])) { g.selsz = szs[i]; break; }
                wg_gal_preview(&g);
                double tb = now_ms(); g.view = 1;
                for (;;) { double e = ease_out((now_ms() - tb) / 300.0 > 1 ? 1 : (now_ms() - tb) / 300.0); g.push = (float)e; wg_gal_frame(&g, 1); if (e >= 1) break; }
            }
        } else {
            int szs[3], ns = wg_gal_sizes(g.sel, szs), sw = ns == 3 ? 264 : 220, sx0 = (W - sw) / 2;
            if (sy < GS_Y + 56 && sx < 130) {
                double tb = now_ms();
                for (;;) { double e = ease_out((now_ms() - tb) / 280.0 > 1 ? 1 : (now_ms() - tb) / 280.0); g.push = 1 - (float)e; wg_gal_frame(&g, 1); if (e >= 1) break; }
                g.view = 0;
            } else if (sy >= GS_Y + GD_SEG && sy < GS_Y + GD_SEG + 32 && sx >= sx0 && sx < sx0 + sw) {
                int ns2 = szs[(sx - sx0) / (sw / ns) < ns ? (sx - sx0) / (sw / ns) : ns - 1]; if (ns2 != g.selsz) { g.selsz = ns2; wg_gal_preview(&g); }
            } else if (pr == 100) { *otype = g.sel; *osize = g.selsz; result = 1; break; }
        }
    }
    /* bajar la hoja */
    float from = g.top; double tb = now_ms();
    for (;;) { double e = ease_out((now_ms() - tb) / 260.0 > 1 ? 1 : (now_ms() - tb) / 260.0); g.top = (int)(from + (H - from) * e); wg_gal_frame(&g, 1 - (float)e); if (e >= 1) break; }
    drain_until_up();
    have_under = 0;
    return result;
}

/* ------------------------------------------------------------------ ajustes */
static int wg_on[WG_N];
static void apply_widgets(void)
{
    for (int t = 0; t < WG_N; t++) {
        if (wg_on[t] && !wg_has_type(t) && wg_n < WG_MAX) { memset(&wg_it[wg_n], 0, sizeof wg_it[0]); wg_it[wg_n].type = t; wg_it[wg_n++].size = wg_defsz[t]; }
        if (!wg_on[t]) for (int i = 0; i < wg_n; i++) if (wg_it[i].type == t) { memmove(&wg_it[i], &wg_it[i + 1], (wg_n - i - 1) * sizeof wg_it[0]); wg_n--; i--; }
    }
    wg_free_unused(); wg_cache_ok = 0; home_prev_ok = 0; lconf_save();
}
/* (los textos nuevos van ya traducidos con wgt: la pagina de Ajustes los pasa por tr(), que
 * devuelve tal cual lo que no conoce) */
static void page_widgets(struct spage *p)
{
    *p = (struct spage){ "Widgets" };
    p->secs[0] = (struct ssec){ wgt("MÚSICA"), 0, {
        ROW("Reproduciendo", R_TOGGLE, &wg_on[WG_NOW], 0, 0, apply_widgets, 0, 0), ROW("Recién añadido", R_TOGGLE, &wg_on[WG_NEW], 0, 0, apply_widgets, 0, 0),
        ROW("Escuchas", R_TOGGLE, &wg_on[WG_LISTEN], 0, 0, apply_widgets, 0, 0), ROW("Biblioteca", R_TOGGLE, &wg_on[WG_LIB], 0, 0, apply_widgets, 0, 0),
        ROW("Mezclas", R_TOGGLE, &wg_on[WG_MIX], 0, 0, apply_widgets, 0, 0) }, 5 };
    p->secs[1] = (struct ssec){ wgt("SISTEMA"), "Mantén pulsado un widget para moverlo o quitarlo, como las apps.", {
        ROW("Baterías", R_TOGGLE, &wg_on[WG_BATT], 0, 0, apply_widgets, 0, 0), ROW("Fecha", R_TOGGLE, &wg_on[WG_DATE], 0, 0, apply_widgets, 0, 0),
        ROW(wgt("Reloj"), R_TOGGLE, &wg_on[WG_CLOCK], 0, 0, apply_widgets, 0, 0), ROW("Almacenamiento", R_TOGGLE, &wg_on[WG_STORAGE], 0, 0, apply_widgets, 0, 0),
        ROW("Energía", R_TOGGLE, &wg_on[WG_POWER], 0, 0, apply_widgets, 0, 0) }, 5 };
    p->nsec = 2;
}
static void nav_wg_list(void)
{
    for (int t = 0; t < WG_N; t++) wg_on[t] = wg_has_type(t);
    open_page(page_widgets, "Inicio");
}
static const char *v_wg_count(void) { static char b[8]; snprintf(b, sizeof b, "%d", wg_n); return b; }
/* "Editar widgets" desde Ajustes: la pagina de widgets en edicion entra como una pagina mas */
static void wg_edit_from_settings(void)
{
    memcpy(img_app, fr0, HM_FRAME_BYTES);            /* la pagina de ajustes (para volver) */
    wg_masks_init(); wg_snap(); wg_snap_ok = 1;
    short tx[WG_MAX], ty[WG_MAX];
    float sv = wg_scroll; wg_scroll = 0;
    wg_targets(-1, -1, tx, ty, 0);
    for (int i = 0; i < wg_n; i++) {
        struct witem *it = &wg_it[i]; it->ax = tx[i]; it->ay = ty[i]; it->sc = it->al = 1; it->rm = 0;
        if (!wg_spr[it->type][it->size].px || wg_spr[it->type][it->size].sig != wg_sig(it->type, it->size, tx[i], WG_TOP + ty[i])) wg_build(it->type, it->size, tx[i], WG_TOP + ty[i]);
    }
    wg_shift = WG_ESH; wg_t0 = now_ms();
    wg_edit_frame(img_base, wg_t0, 1, -1);
    fs_transition(img_app, img_base, 1, 0);
    drain_until_up();
    wg_edit(-1, 1);
    memcpy(img_base, fr0, HM_FRAME_BYTES);
    fs_transition(img_base, img_app, -1, 0);
    drain_until_up();
    (void)sv; wg_scroll = 0; home_prev_ok = 0;
}

static void animate_scroll_to(int page);
static void *wg_pc_th(void *a) { long k = (long)a; if (k == 0) pc_prev(); else if (k == 1) pc_toggle_or_replay(); else pc_next(); return 0; }
/* toque con la pagina de widgets en pantalla (desde launcher_touch) */
static void widgets_touch(struct pkt *p)
{
    int sx = p->x, sy = p->y, moved = 0, horiz = 0; float s0 = wg_scroll, vel = 0, vy = 0; double t0 = now_ms(), lt = t0; int lx = sx, ly = sy;
    int btn, k = wg_hit(sx, sy, &btn);
    if (k >= 0) { wg_press = k; wg_press_btn = btn; launcher_present(); }
    for (;;) {
        int got = read_pkt(p, 30); double t = now_ms();
        if ((got && !p->down) || !cur_down) break;
        int dx = cur_x - sx, dy = cur_y - sy;
        if (!moved && (abs(dx) > 10 || abs(dy) > 10)) { moved = 1; horiz = abs(dx) > abs(dy); if (wg_press >= 0) { wg_press = -1; launcher_present(); } }
        if (t > lt) { vel = 0.6f * vel + 0.4f * (cur_x - lx) / (float)(t - lt); vy = 0.6f * vy + 0.4f * (cur_y - ly) / (float)(t - lt); lx = cur_x; ly = cur_y; lt = t; }
        if (!moved && t - t0 > 500 && k != -2) { wg_press = -1; wg_edit(k, 0); launcher_present(); return; }   /* pulsacion larga: editar */
        if (!got) continue;
        if (moved && horiz) { float s = -W - dx; if (s > 0) s = 0; if (s < -W - 40) s = -W - 40 + (s + W + 40) / 3; V.scroll = s; launcher_present(); }
        else if (moved) { float mx = wg_content_h() + WG_TOP - H; if (mx < 0) mx = 0; wg_scrolling = 1; wg_scroll = s0 - dy; if (wg_scroll < 0) wg_scroll = 0; if (wg_scroll > mx) wg_scroll = mx; launcher_present(); }
    }
    if (moved && horiz) {
        wg_scrolling = 0;
        if (cur_x - sx < -70 || vel < -0.35f) { widgets_on = 0; animate_scroll_to(0); }
        else { animate_scroll_to(-1); }
        return;
    }
    if (moved) {   /* inercia de iOS */
        float mx = wg_content_h() + WG_TOP - H; if (mx < 0) mx = 0;
        if (now_ms() - lt > 60) vy = 0;
        double tp = now_ms();
        while (fabsf(vy) > 0.03f) {
            double t = now_ms(); float dt = (float)(t - tp); tp = t; if (dt > 40) dt = 40;
            wg_scroll -= vy * dt; vy *= powf(0.9975f, dt);
            if (wg_scroll < 0) { wg_scroll = 0; vy = 0; } if (wg_scroll > mx) { wg_scroll = mx; vy = 0; }
            launcher_present();
            struct pkt q; if (read_pkt(&q, 0) && q.down) break;
        }
        wg_scrolling = 0; launcher_present();
        return;
    }
    wg_press = -1;
    if (k == -2) { wg_edit(-1, 0); launcher_present(); return; }
    if (k < 0) { launcher_present(); return; }
    int type = wg_it[k].type; const struct wspr *sp = &wg_spr[type][wg_it[k].size];
    if (btn >= 0 && btn < sp->nb) {          /* boton del widget (Reproduciendo) */
        int act = sp->b[btn].k;
        if (act == 1) { wg_np_force = !wg_playing(); wg_np_until = now_ms() + 2500; }
        pthread_t th; if (!pthread_create(&th, 0, wg_pc_th, (void *)(long)act)) pthread_detach(th);
        wg_fresh = 1; launcher_present(); return;
    }
    short xs[WG_MAX], ys[WG_MAX]; wg_layout_items(xs, ys);
    int ix = xs[k] + 20, iy = WG_TOP - (int)wg_scroll + ys[k] + 20;
    const char *id = 0;
    switch (type) {
    case WG_NOW: id = "nowplaying"; break;
    case WG_STORAGE: id = "files"; break;
    case WG_LIB: case WG_NEW: case WG_LISTEN: case WG_MIX: id = "music"; break;
    case WG_BATT: id = WDs.nhp || WDs.nhps ? "headphones" : "settings"; break;
    case WG_POWER: id = "settings"; break;
    }
    int a = id ? app_by_id(id) : -1;
    if (a < 0) { launcher_present(); return; }
    if (apps[a].act == ACT_FILES) { launch_app(a, ix, iy); return; }      /* apps propias: se vuelve a los widgets */
    widgets_on = 0; V.scroll = cur_page * W; launch_app(a, ix, iy);
}
#endif /* WG_RENDER_ONLY */
