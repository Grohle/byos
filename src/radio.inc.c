/* Radio: emisoras por internet.
 *
 * Fuentes (radio_src.inc.c): el directorio Radio Browser (api.radio-browser.info, comunitario,
 * sin clave; la de siempre), la lista integrada "Radio publica europea" y las que anade el
 * usuario (una URL de una lista M3U/PLS/JSON u otro servidor compatible con Radio Browser),
 * en RA_DATA/sources.tsv; las listas bajadas se guardan en la SD (RA_SDDIR/lists). Favoritas,
 * recientes y propias (nombre + URL) en RA_DATA/stations.tsv.
 *
 * Sonido: el motor de fabrica no abre URLs, asi que "hibynet radio" baja el flujo (redirecciones,
 * .m3u/.pls, ICY, HLS) a un fichero que crece en la tarjeta SD (el motor no toca ficheros de
 * /tmp, y la RAM no da para un bufer) y el reproductor lo abre como una cancion mas (lista "sin
 * escanear" de playctl) cuando ya hay 15 s escritos. Asi el volumen, el ecualizador, el DAC y
 * los botones siguen siendo los de siempre, y poner musica local corta la radio sola: el
 * ayudante ve que el reproductor solto el fichero y termina. Sin tarjeta no hay radio.
 *
 * Hilos: uno de red para el directorio y las listas (trabajos en cola, hibynet get) y uno que
 * vigila al ayudante (lee su fichero de estado y manda la lista al reproductor). El bucle
 * tactil solo recoge resultados (ra_poll). La radio sigue sonando al salir de la app; al volver,
 * la barra de abajo la para.
 *
 * radio_ui.inc.c dibuja; radio_app.inc.c son los toques y el bucle. */
#include <sys/wait.h>
#include "radio_stream.inc.c"
#include "radio_core.inc.c"

#define RA_DATA "/usr/data/hibymod/appdata/radio"
#define RA_UIDIR "/tmp/hibyradio_ui"
#define RA_LISTS RA_SDDIR "/lists"
#define RA_PAGE 60               /* emisoras por peticion */
#define RA_MAXST 240             /* tope por lista */
#define RA_MAXOFF 1500           /* no se pagina mas alla */
#define RA_MAXCT 250
#define RA_LIST_TTL (24 * 3600)  /* una lista bajada vale un dia (Actualizar la vuelve a pedir) */

enum { RY_IDLE, RY_CONNECTING, RY_BUFFERING, RY_PLAYING, RY_ERROR };

/* la biblioteca (~110 KB) solo vive con la app abierta: radio_app_at la pide y la suelta al salir;
 * los ajustes y las fuentes (pequenos) se quedan */
static struct ra_lib *ra_lb; static struct ra_conf ra_cf; static int ra_loaded;
static struct ra_srcs ra_sr;

/* ------------------------------------------------------------------ disco */
static void ra_save_file(const char *name, int what)
{
    char p[200], t[208]; snprintf(p, sizeof p, RA_DATA "/%s", name); snprintf(t, sizeof t, "%s.tmp", p);
    mkdir("/usr/data/hibymod/appdata", 0755); mkdir(RA_DATA, 0755);
    if (what == 0 && !ra_lb) return;
    FILE *f = fopen(t, "w"); if (!f) return;
    if (what == 1) ra_conf_write(&ra_cf, f); else if (what == 2) ra_srcs_write(&ra_sr, f); else ra_lib_write(ra_lb, f);
    int bad = ferror(f); bad |= fclose(f);
    if (bad) unlink(t); else rename(t, p);
}
static void ra_save_lib(void) { ra_save_file("stations.tsv", 0); }
static void ra_save_conf(void) { ra_save_file("config", 1); }
static void ra_save_srcs(void) { ra_save_file("sources.tsv", 2); }
/* 0 = sin memoria para la biblioteca */
static int ra_load(void)
{
    if (!ra_loaded) {
        ra_loaded = 1;
        ra_conf_default(&ra_cf);
        char *t = fm_slurp(RA_DATA "/config", 4096, 0); if (t) { ra_conf_parse(&ra_cf, t); free(t); }
        if (!ra_cf.cc[0]) ra_lang_country(LANG_CODES[ui_lang], ra_cf.cc, ra_cf.cname, sizeof ra_cf.cname);
        char e[] = "";
        t = fm_slurp(RA_DATA "/sources.tsv", 32768, 0); ra_srcs_parse(&ra_sr, t ? t : e); free(t);
    }
    if (ra_lb) return 1;
    if (!(ra_lb = calloc(1, sizeof *ra_lb))) return 0;
    char *t = fm_slurp(RA_DATA "/stations.tsv", 400000, 0); if (t) { ra_lib_parse(ra_lb, t); free(t); }
    return 1;
}
static void ra_unload(void) { free(ra_lb); ra_lb = 0; }
static int ra_sd_ok(void) { return !access("/usr/data/mnt/sd_0/.", F_OK) && mu_sd_mounted(); }
/* donde se guarda la lista bajada de una fuente (en la SD: la memoria interna va justa) */
static void ra_list_cache(unsigned sid, char *o, int n) { snprintf(o, n, RA_LISTS "/%08x.txt", sid); }

/* ------------------------------------------------------------------ red: directorio y listas */
enum { RJ_LIST, RJ_CTRY, RJ_CLICK, RJ_SRC };
struct ra_job {
    struct ra_job *next;
    int type, slot, gen, kind, offset, force; char arg[100], base[200], url[256]; unsigned sid; int stype;
    void *res; int n, seen, ok, rtype; char title[56];
};
struct ra_list {
    int kind; char arg[100], title[100], base[200]; unsigned sid;   /* base: otro servidor Radio Browser; sid: fuente (lista) */
    struct ra_station *it; int n, state /* 0 nada, 1 cargando, 2 hecho, 3 error */, more, offset, err;
};
static pthread_mutex_t ra_nmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t ra_ncv = PTHREAD_COND_INITIALIZER;
static struct ra_job *ra_jq, *ra_jq_tail, *ra_done;
static int ra_net_on, ra_host, ra_slot_gen[2];
static struct ra_list ra_ls[2];                      /* 0 = busqueda (raiz), 1 = lista abierta */
static struct ra_country *ra_ct; static int ra_nct, ra_ct_state;

static int ra_get(const char *url, const char *out)
{
    char *argv[] = { NETBIN, "get", (char *)url, (char *)out, 0 };
    unlink(out);
    return fm_wait(fm_spawn(argv, 0, "HIBYNET_UA=" RA_UA));
}
/* prueba los servidores en orden desde el ultimo que contesto (o solo j->base); devuelve el JSON (un array) */
static char *ra_fetch(const struct ra_job *j)
{
    char url[800], base[200]; const char *out = RA_UIDIR "/q.json";
    mkdir(RA_UIDIR, 0755);
    int nh = j->base[0] ? 1 : RA_NHOST;
    for (int k = 0; k < nh; k++) {
        int h = (ra_host + k) % RA_NHOST;
        if (j->base[0]) snprintf(base, sizeof base, "%s", j->base); else ra_host_base(base, sizeof base, h);
        if (j->type == RJ_CTRY) ra_countries_url(url, sizeof url, h);
        else ra_list_url(url, sizeof url, base, j->kind, j->arg, RA_PAGE, j->offset);
        if (ra_get(url, out)) continue;
        char *js = fm_slurp(out, 2 << 20, 0); unlink(out);
        if (!js) continue;
        const char *p = fj_w(js);
        if (p && *p == '[') { if (!j->base[0]) ra_host = h; return js; }
        free(js);
    }
    unlink(out);
    return 0;
}
/* una fuente de lista (M3U/PLS/JSON) o un servidor Radio Browser por probar: la lista de la
 * cache si es reciente, si no se baja (a la SD si esta). Rellena j->res/j->n, j->rtype (tipo
 * visto: RSRC_*, -2 = era una emision HLS) y j->ok (0 sin red, 1 bien, -1 no es una lista) */
static void ra_run_src(struct ra_job *j)
{
    char cache[200], tmp[220]; int sd = ra_sd_ok();
    ra_list_cache(j->sid, cache, sizeof cache);
    struct stat sb; char *b = 0;
    if (!j->force && sd && !stat(cache, &sb) && time(0) - sb.st_mtime < RA_LIST_TTL) b = fm_slurp(cache, 2 << 20, 0);
    int fresh = 0;
    if (!b) {
        if (sd) { mkdir(RA_SDDIR, 0755); mkdir(RA_LISTS, 0755); snprintf(tmp, sizeof tmp, "%s.part", cache); }
        else { mkdir(RA_UIDIR, 0755); snprintf(tmp, sizeof tmp, RA_UIDIR "/src.txt"); }
        if (j->stype == RSRC_RBX) {                  /* un servidor: /json/stats dice si lo es */
            char u[300]; snprintf(u, sizeof u, "%s/json/stats", j->url);
            if (!ra_get(u, tmp)) b = fm_slurp(tmp, 64 << 10, 0);
            unlink(tmp);
            j->ok = !b ? 0 : ra_src_sniff(b) == RSRC_RBX ? 1 : -1; j->rtype = RSRC_RBX;
            free(b);
            return;
        }
        if (!ra_get(j->url, tmp)) b = fm_slurp(tmp, 2 << 20, 0);
        if (!b) { unlink(tmp); j->ok = 0; return; }
        fresh = 1;
    }
    struct ra_station *s = malloc(RA_MAXST * sizeof *s);
    if (!s) { free(b); unlink(tmp); j->ok = 0; return; }
    int n = ra_src_parse(j->stype, b, s, RA_MAXST, j->title, sizeof j->title);
    j->rtype = n == -2 ? -2 : ra_src_sniff(b);
    if (n < 0 && j->stype == RSRC_URL) {            /* la raiz de algo que no es una lista: un servidor Radio Browser? */
        free(b); b = 0;
        char u[300]; snprintf(u, sizeof u, "%s/json/stats", j->url);
        if (!ra_get(u, tmp)) b = fm_slurp(tmp, 64 << 10, 0);
        if (b && ra_src_sniff(b) == RSRC_RBX) { j->rtype = RSRC_RBX; j->ok = 1; free(s); free(b); unlink(tmp); return; }
    }
    if (n >= 0) {
        j->res = s; j->n = n; j->ok = 1;
        if (fresh && sd) rename(tmp, cache);
    } else { free(s); j->ok = -1; }
    if (fresh) unlink(tmp);
    free(b);
}
static void *ra_net_worker(void *a)
{
    (void)a; fm_lowprio(5);
    for (;;) {
        pthread_mutex_lock(&ra_nmu);
        while (!ra_jq) pthread_cond_wait(&ra_ncv, &ra_nmu);
        struct ra_job *j = ra_jq; ra_jq = j->next; if (!ra_jq) ra_jq_tail = 0;
        int stale = j->type == RJ_LIST && j->slot >= 0 && j->gen != ra_slot_gen[j->slot];
        pthread_mutex_unlock(&ra_nmu);
        if (stale) { free(j); continue; }
        if (j->type == RJ_CLICK) {                   /* cortesia con el directorio: cuenta la escucha */
            char url[300];
            mkdir(RA_UIDIR, 0755);
            if (!ra_click_url(url, sizeof url, ra_host, j->arg)) ra_get(url, RA_UIDIR "/c.json");
            unlink(RA_UIDIR "/c.json"); free(j);
            continue;
        }
        if (j->type == RJ_SRC) ra_run_src(j);
        else {
            char *js = ra_fetch(j);
            if (js && j->type == RJ_CTRY) {
                struct ra_country *c = malloc(RA_MAXCT * sizeof *c);
                if (c) { j->n = ra_parse_countries(js, c, RA_MAXCT); j->res = c; j->ok = 1; }
            } else if (js) {
                struct ra_station *s = malloc(RA_PAGE * sizeof *s);
                if (s) {
                    j->n = ra_parse_stations(js, s, RA_PAGE, &j->seen); j->res = s; j->ok = 1;
                    if (j->base[0]) for (int i = 0; i < j->n; i++) s[i].uuid[0] = 0;   /* otro servidor: sin cuenta de clics del directorio */
                }
            }
            free(js);
        }
        pthread_mutex_lock(&ra_nmu); j->next = ra_done; ra_done = j; pthread_mutex_unlock(&ra_nmu);
    }
    return 0;
}
static struct ra_job *ra_job_new(int type, int slot, int kind, const char *arg, int offset)
{
    if (!ra_net_on) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&t, &at, ra_net_worker, 0)) ra_net_on = 1;
        pthread_attr_destroy(&at);
        if (!ra_net_on) return 0;
    }
    struct ra_job *j = calloc(1, sizeof *j); if (!j) return 0;
    j->type = type; j->slot = slot; j->kind = kind; j->offset = offset; snprintf(j->arg, sizeof j->arg, "%s", arg ? arg : "");
    return j;
}
static void ra_job_put(struct ra_job *j)
{
    pthread_mutex_lock(&ra_nmu);
    j->gen = j->slot >= 0 ? ra_slot_gen[j->slot] : 0;
    if (ra_jq_tail) ra_jq_tail->next = j; else ra_jq = j;
    ra_jq_tail = j;
    pthread_cond_signal(&ra_ncv);
    pthread_mutex_unlock(&ra_nmu);
}
static int ra_job_add(int type, int slot, int kind, const char *arg, int offset)
{
    struct ra_job *j = ra_job_new(type, slot, kind, arg, offset); if (!j) return -1;
    if (slot >= 0 && type == RJ_LIST) snprintf(j->base, sizeof j->base, "%s", ra_ls[slot].base);
    ra_job_put(j);
    return 0;
}
/* bajar (o leer de la cache) la lista de la fuente i; slot 1 = para la lista abierta, -1 = solo probarla */
static int ra_src_fetch(int i, int slot, int force)
{
    if (i < 0 || i >= ra_sr.n) return -1;
    const struct ra_src *s = &ra_sr.s[i];
    struct ra_job *j = ra_job_new(RJ_SRC, slot, 0, "", 0); if (!j) return -1;
    j->sid = s->sid; j->stype = s->type; j->force = force; snprintf(j->url, sizeof j->url, "%s", s->url);
    ra_sr.s[i].busy = 1;
    ra_job_put(j);
    return 0;
}
static void ra_list_more(int slot)
{
    struct ra_list *l = &ra_ls[slot];
    l->state = ra_job_add(RJ_LIST, slot, l->kind, l->arg, l->offset) ? 3 : 1;
}
static void ra_list_reset(int slot)
{
    struct ra_list *l = &ra_ls[slot];
    pthread_mutex_lock(&ra_nmu); ra_slot_gen[slot]++; pthread_mutex_unlock(&ra_nmu);
    l->n = 0; l->state = 0; l->more = 0; l->offset = 0; l->err = 0; l->sid = 0; l->base[0] = 0;
}
static int ra_list_alloc(struct ra_list *l) { if (!l->it && !(l->it = malloc(RA_MAXST * sizeof *l->it))) { l->state = 3; return 0; } return 1; }
static void ra_list_open(int slot, int kind, const char *arg, const char *title)
{
    struct ra_list *l = &ra_ls[slot];
    ra_list_reset(slot);
    l->kind = kind; snprintf(l->arg, sizeof l->arg, "%s", arg ? arg : ""); snprintf(l->title, sizeof l->title, "%s", title ? title : "");
    if (!ra_list_alloc(l)) return;
    ra_list_more(slot);
}
/* la lista de la fuente i en la lista abierta (slot 1) */
static void ra_src_open(int i, int force)
{
    struct ra_list *l = &ra_ls[1];
    if (i < 0 || i >= ra_sr.n) return;
    const struct ra_src *s = &ra_sr.s[i];
    ra_list_reset(1);
    snprintf(l->title, sizeof l->title, "%s", s->type == RSRC_EU ? tr("Radio pública europea") : s->name);
    l->sid = s->sid;
    if (!ra_list_alloc(l)) return;
    if (s->type == RSRC_EU) {
        for (int k = 0; k < RA_NEU && l->n < RA_MAXST; k++) ra_eu_station(k, &l->it[l->n++]);
        l->state = 2;
        return;
    }
    if (s->type == RSRC_RBX) {
        l->kind = RL_TOP; snprintf(l->base, sizeof l->base, "%s", s->url);
        ra_list_more(1);
        l->sid = s->sid;
        return;
    }
    l->state = ra_src_fetch(i, 1, force) ? 3 : 1;
}
static void ra_ctry_fetch(void) { ra_ct_state = ra_job_add(RJ_CTRY, -1, 0, "", 0) ? 3 : 1; }
/* lo que dijo una prueba o carga de la fuente; 1 = cambio algo que se guarda */
static int ra_src_result(struct ra_job *d)
{
    int i = ra_src_ix(&ra_sr, d->sid); if (i < 0) return 0;
    struct ra_src *s = &ra_sr.s[i];
    s->busy = 0;
    if (d->ok == 0) { s->err = RSE_NET; return 1; }
    if (d->ok < 0 || d->rtype == -2) { s->err = RSE_NOTLIST; return 1; }
    if (d->rtype >= RSRC_M3U && d->rtype <= RSRC_RBX) s->type = (unsigned char)d->rtype;
    if (s->type != RSRC_RBX) { s->n = d->n; s->err = d->n > 0 ? RSE_OK : RSE_EMPTY; }
    else s->err = RSE_OK;
    if (!s->named && d->title[0]) fm_cpy(s->name, sizeof s->name, d->title);
    s->fetched = (long long)time(0);
    return 1;
}
/* resultados del hilo de red; 1 = algo cambio */
static int ra_net_poll(void)
{
    pthread_mutex_lock(&ra_nmu); struct ra_job *d = ra_done; ra_done = 0; pthread_mutex_unlock(&ra_nmu);
    int ch = 0, save = 0;
    while (d) {
        struct ra_job *nx = d->next;
        if (d->type == RJ_CTRY) {
            if (d->ok && d->n > 0) { free(ra_ct); ra_ct = d->res; d->res = 0; ra_nct = d->n; ra_ct_state = 2; }
            else ra_ct_state = ra_nct ? 2 : 3;
            ch = 1;
        } else if (d->type == RJ_SRC) {
            int si = ra_src_ix(&ra_sr, d->sid);
            if (si >= 0 && d->rtype == -2 && !ra_sr.s[si].fetched && ra_lb) {   /* recien anadida y era una emision HLS: una emisora */
                const struct ra_src *s = &ra_sr.s[si];
                int r = ra_own_add(ra_lb, s->named ? s->name : "", s->url);
                ra_src_del(&ra_sr, si); save = 1;
                if (r >= 0) { ra_save_lib(); toast(tr("Es una emisora: añadida a Mis emisoras")); }
                else toast(r == -2 ? tr("Esa emisora ya está en la lista") : r == -3 ? tr("No caben más emisoras") : tr("URL no válida"));
                ch = 1; free(d->res); free(d); d = nx; continue;
            }
            save |= ra_src_result(d);
            if (d->slot >= 0 && d->gen == ra_slot_gen[d->slot] && ra_ls[d->slot].sid == d->sid) {
                struct ra_list *l = &ra_ls[d->slot];
                const struct ra_station *s = d->res;
                for (int i = 0; i < d->n && l->n < RA_MAXST; i++) l->it[l->n++] = s[i];
                l->state = d->ok > 0 ? 2 : 3; l->err = d->ok == 0 ? RSE_NET : d->ok < 0 ? RSE_NOTLIST : d->n ? 0 : RSE_EMPTY;
                int si = ra_src_ix(&ra_sr, d->sid);
                if (d->ok > 0 && d->rtype == RSRC_RBX && si >= 0) ra_src_open(si, 0);   /* resulto ser un servidor */
            }
            ch = 1;
        } else if (d->gen == ra_slot_gen[d->slot]) {
            struct ra_list *l = &ra_ls[d->slot];
            if (d->ok) {
                const struct ra_station *s = d->res;
                for (int i = 0; i < d->n && l->n < RA_MAXST; i++) {
                    if (!ra_playable(s[i].kind)) continue;          /* Ogg, FLAC, video...: no suenan */
                    int dup = 0; for (int k = 0; k < l->n && !dup; k++) dup = ra_same(&l->it[k], &s[i]);
                    if (!dup) l->it[l->n++] = s[i];
                }
                l->offset += d->seen;
                l->more = d->seen >= RA_PAGE && l->n < RA_MAXST && l->offset < RA_MAXOFF;
                l->state = 2;
            } else { l->state = l->n ? 2 : 3; l->more = 0; }
            ch = 1;
        }
        free(d->res); free(d); d = nx;
    }
    if (save) ra_save_srcs();
    return ch;
}

/* ------------------------------------------------------------------ sonido: vigilante del ayudante */
static struct {
    pthread_mutex_t mu; int on, pfd[2];
    int cmd;                                         /* 0, 1 = poner "want", 2 = parar */
    struct ra_station want; int lead, cap;
    /* lo que ve la interfaz */
    int state, pct, br, have, ver;
    char err[24], title[256], codec[8]; struct ra_station cur;
} ra_pl = { .mu = PTHREAD_MUTEX_INITIALIZER };
/* copia para dibujar (hilo de la interfaz) */
static struct { int state, pct, br, have, ver; char err[24], title[256], codec[8]; struct ra_station cur; } ra_np;

static void ra_pub(int state, int pct, const struct ra_hstate *h, const char *err)
{
    pthread_mutex_lock(&ra_pl.mu);
    if (!ra_pl.cmd) {                                /* con una orden pendiente manda la interfaz */
        int ch = ra_pl.state != state || ra_pl.pct != pct || strcmp(ra_pl.err, err);
        if (h) ch |= ra_pl.br != h->br || strcmp(ra_pl.title, h->title) || strcmp(ra_pl.codec, h->codec);
        if (ch) {
            ra_pl.state = state; ra_pl.pct = pct; snprintf(ra_pl.err, sizeof ra_pl.err, "%s", err);
            if (h) { ra_pl.br = h->br; snprintf(ra_pl.title, sizeof ra_pl.title, "%s", h->title); snprintf(ra_pl.codec, sizeof ra_pl.codec, "%s", h->codec); }
            ra_pl.ver++;
        }
    }
    pthread_mutex_unlock(&ra_pl.mu);
}
static void ra_kill(pid_t *pid)
{
    if (*pid <= 0) return;
    kill(*pid, SIGTERM);                             /* el ayudante borra su fichero al salir */
    for (int i = 0; i < 75; i++) {
        int s; if (waitpid(*pid, &s, WNOHANG) != 0) { *pid = 0; return; }
        usleep(20000);
    }
    kill(*pid, SIGKILL); waitpid(*pid, 0, 0); *pid = 0;
}
/* restos de una sesion: el estado (tmpfs) y el audio de la carpeta de la radio en la SD (solo
 * nuestra: alli no hay nada mas que .mp3/.aac del ayudante y la subcarpeta de listas) */
static void ra_sweep(void)
{
    char p[480];
    unlink(RA_STDIR "/state"); unlink(RA_STDIR "/state.tmp"); unlink(RA_STDIR "/pid");
    DIR *d = opendir(RA_SDDIR); if (!d) return;
    for (struct dirent *e; (e = readdir(d)); ) {
        const char *x = strrchr(e->d_name, '.');
        if (x && (!strcasecmp(x, ".mp3") || !strcasecmp(x, ".aac"))) { snprintf(p, sizeof p, RA_SDDIR "/%s", e->d_name); unlink(p); }
    }
    closedir(d);
}
static void *ra_ctl(void *arg)
{
    (void)arg;
    pid_t pid = 0; struct ra_station st, nst; int lead = 15, cap = 128, gen_sent = 0, resend = 0; double t_sent = 0;
    memset(&st, 0, sizeof st); memset(&nst, 0, sizeof nst);
    for (;;) {
        struct pollfd pf = { ra_pl.pfd[0], POLLIN, 0 };
        poll(&pf, 1, pid > 0 ? 300 : -1);
        if (pf.revents & POLLIN) { char b[32]; while (read(ra_pl.pfd[0], b, sizeof b) > 0) { } }
        pthread_mutex_lock(&ra_pl.mu);
        int cmd = ra_pl.cmd; ra_pl.cmd = 0;
        if (cmd == 1) { nst = ra_pl.want; lead = ra_pl.lead; cap = ra_pl.cap; }
        pthread_mutex_unlock(&ra_pl.mu);
        if (cmd) { ra_kill(&pid); ra_sweep(); }
        if (cmd == 2) { lg("radio: parada"); ra_pub(RY_IDLE, 0, 0, ""); continue; }
        if (cmd == 1) {
            st = nst; gen_sent = 0; t_sent = 0; resend = 0;
            if (!ra_sd_ok()) { lg("radio: sin tarjeta SD"); ra_pub(RY_ERROR, 0, 0, "nosd"); continue; }
            mkdir(RA_STDIR, 0755);
            char a3[16], a4[16]; snprintf(a3, sizeof a3, "%d", lead); snprintf(a4, sizeof a4, "%d", cap);
            char *argv[] = { NETBIN, "radio", st.url, RA_SDDIR, st.name[0] ? st.name : "radio", a3, a4, RA_STDIR, 0 };
            pid = fm_spawn(argv, 0, 0);
            lg("radio: %s -> pid %d", st.url, (int)pid);
            if (pid <= 0) { pid = 0; ra_pub(RY_ERROR, 0, 0, "io"); }
            continue;
        }
        if (pid <= 0) continue;

        int ws; pid_t w = waitpid(pid, &ws, WNOHANG);
        struct ra_hstate h; char *t = fm_slurp(RA_STDIR "/state", 4000, 0); int ok = t && ra_state_parse(t, &h); free(t);
        if (w != 0) {                                /* el ayudante termino (o alguien lo recogio) */
            pid = 0;
            lg("radio: ayudante fuera, st=%s err=%s", ok ? h.st : "?", ok ? h.err : "");
            if (ok && !strcmp(h.st, "error")) ra_pub(RY_ERROR, 0, 0, h.err[0] ? h.err : "io");
            else if (ok && (!strcmp(h.err, "cap") || !strcmp(h.err, "end"))) ra_pub(RY_ERROR, 0, 0, h.err);   /* fin claro (gris) */
            else if (!ok) ra_pub(RY_ERROR, 0, 0, "io");
            else ra_pub(RY_IDLE, 0, 0, "");
            ra_sweep();
            continue;
        }
        if (!ok) continue;
        if (!strcmp(h.st, "connecting")) ra_pub(RY_CONNECTING, 0, &h, "");
        else if (!strcmp(h.st, "buffering")) ra_pub(RY_BUFFERING, h.pct, &h, "");
        else if (!strcmp(h.st, "ready")) {
            if (h.gen != gen_sent && h.file[0]) {    /* fichero nuevo: al reproductor (ruta Linux tal cual: esta en la SD) */
                const char *pp = h.file; struct pc_list_res r; memset(&r, 0, sizeof r);
                unlink(PC_RES);
                int id = pc_list_send(&pp, 1, 0, st.name, PC_UNSCANNED | 0xff);
                int rc = id > 0 ? pc_list_wait(id, 5000, &r) : id;
                lg("radio: lista gen %d -> %d (%s)", h.gen, rc, r.msg);
                if (rc < 0 && rc != -ETIMEDOUT) { ra_kill(&pid); ra_sweep(); ra_pub(RY_ERROR, 0, 0, "player"); }
                else { gen_sent = h.gen; t_sent = now_ms(); resend = 0; ra_pub(RY_BUFFERING, 100, &h, ""); }
            } else if (!h.open) {
                ra_pub(RY_BUFFERING, 100, &h, "");
                if (!resend && now_ms() - t_sent > 6000) { resend = 1; gen_sent = 0; }   /* una vez mas, por si la lista se perdio */
            } else ra_pub(RY_PLAYING, 100, &h, "");
        }
    }
    return 0;
}
static int ra_ctl_start(void)
{
    if (ra_pl.on) return 1;
    if (pipe(ra_pl.pfd)) return 0;
    for (int i = 0; i < 2; i++) { fcntl(ra_pl.pfd[i], F_SETFL, O_NONBLOCK); fcntl(ra_pl.pfd[i], F_SETFD, FD_CLOEXEC); }
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&t, &at, ra_ctl, 0)) ra_pl.on = 1;
    pthread_attr_destroy(&at);
    if (!ra_pl.on) { close(ra_pl.pfd[0]); close(ra_pl.pfd[1]); }
    return ra_pl.on;
}
static void ra_wake(void) { char c = 1; if (ra_pl.on && write(ra_pl.pfd[1], &c, 1) < 0) { } }
/* copia lo publicado; 1 = cambio */
static int ra_pl_sync(void)
{
    int ch = 0;
    pthread_mutex_lock(&ra_pl.mu);
    if (ra_pl.ver != ra_np.ver) {
        ra_np.state = ra_pl.state; ra_np.pct = ra_pl.pct; ra_np.br = ra_pl.br; ra_np.have = ra_pl.have; ra_np.ver = ra_pl.ver;
        memcpy(ra_np.err, ra_pl.err, sizeof ra_np.err); memcpy(ra_np.title, ra_pl.title, sizeof ra_np.title); memcpy(ra_np.codec, ra_pl.codec, sizeof ra_np.codec);
        ra_np.cur = ra_pl.cur; ch = 1;
    }
    pthread_mutex_unlock(&ra_pl.mu);
    return ch;
}
static int ra_active(void) { return ra_np.have && (ra_np.state == RY_CONNECTING || ra_np.state == RY_BUFFERING || ra_np.state == RY_PLAYING); }
static int ra_is_cur(const struct ra_station *s) { return ra_np.have && ra_same(&ra_np.cur, s); }

static void ra_play(const struct ra_station *s0)
{
    struct ra_station s = *s0;                       /* puede apuntar a la lista de recientes, que se reordena */
    if (!ra_ctl_start()) { toast(tr("No se pudo reproducir")); return; }
    pc_pause();                                      /* la musica calla ya, no cuando llegue el flujo */
    pthread_mutex_lock(&ra_pl.mu);
    ra_pl.want = s; ra_pl.cmd = 1; ra_pl.lead = ra_cf.lead_s; ra_pl.cap = ra_cf.cap_mb;
    ra_pl.cur = s; ra_pl.have = 1; ra_pl.state = RY_CONNECTING; ra_pl.pct = ra_pl.br = 0;
    ra_pl.err[0] = ra_pl.title[0] = ra_pl.codec[0] = 0; ra_pl.ver++;
    pthread_mutex_unlock(&ra_pl.mu);
    ra_wake();
    if (ra_lb && (!ra_lb->nrec || !ra_same(&ra_lb->rec[0], &s))) { ra_rec_add(ra_lb, &s); ra_save_lib(); }
    if (s.uuid[0]) ra_job_add(RJ_CLICK, -1, 0, s.uuid, 0);
    ra_pl_sync();
}
static void ra_stop(void)
{
    if (!ra_pl.on) return;
    pc_pause();                                      /* antes de quitarle el fichero */
    pthread_mutex_lock(&ra_pl.mu);
    ra_pl.cmd = 2; ra_pl.state = RY_IDLE; ra_pl.pct = 0; ra_pl.err[0] = 0; ra_pl.ver++;
    pthread_mutex_unlock(&ra_pl.mu);
    ra_wake();
    ra_pl_sync();
}

#include "radio_ui.inc.c"
#include "radio_app.inc.c"
