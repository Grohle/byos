/* radio_core.inc.c - parte pura de la app Radio (sin pantalla, red ni hilos): directorio de
 * Radio Browser (JSON -> emisoras y paises, URLs de las consultas), que codecs suenan, la
 * biblioteca del usuario (favoritas, recientes y emisoras propias en un fichero de texto), el
 * fichero de estado del ayudante (hibynet radio) y los ajustes.
 * La usan radio.inc.c (shell) y tests/radio_test.c (PC). Antes hay que incluir
 * freemusic_core.inc.c (fj_*, fm_cpy, fm_trim, fm_urlenc) y radio_stream.inc.c (rs_*). */
#ifndef RADIO_CORE_INC
#define RADIO_CORE_INC

#define RA_UA "HiByOS-Radio/1.0 (https://github.com/Grohle/hibyos)"
#define RA_URLN 400
struct ra_station {
    char uuid[40];               /* stationuuid del directorio; "" en las propias */
    char name[96], url[RA_URLN];
    char cc[4];                  /* ISO 3166-1 alfa 2 ("" = desconocido) */
    char tags[72];               /* "pop, rock, top 40" (ya para mostrar) */
    char codec[8];               /* "MP3", "AAC", "" */
    short br;                    /* kbit/s (0 = desconocido) */
    unsigned char kind;          /* RS_MP3, RS_AAC, RS_UNKNOWN (se prueba) o lo que no suena */
};
struct ra_country { char cc[4]; char name[64]; int n; };

/* ------------------------------------------------------------------ texto */
/* controles y tabuladores -> espacio, espacios seguidos -> uno, sin espacios en los extremos */
static void ra_clean(char *s)
{
    char *o = s; int sp = 1;
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        int c = *p < 0x20 || *p == 0x7f ? ' ' : *p;
        if (c == ' ') { if (!sp) *o++ = ' '; sp = 1; } else { *o++ = (char)c; sp = 0; }
    }
    while (o > s && o[-1] == ' ') o--;
    *o = 0;
}
/* "pop,rock , top 40,..." -> "pop, rock, top 40" (como mucho max etiquetas) */
static void ra_tags(const char *in, char *o, int cap, int max)
{
    int k = 0, n = 0; o[0] = 0;
    while (*in && n < max) {
        while (*in == ',' || *in == ' ') in++;
        const char *e = in; while (*e && *e != ',') e++;
        const char *z = e; while (z > in && z[-1] == ' ') z--;
        int l = (int)(z - in);
        if (l > 0) {
            if (k + l + 3 >= cap) break;
            if (n) { o[k++] = ','; o[k++] = ' '; }
            memcpy(o + k, in, l); k += l; o[k] = 0; n++;
        }
        in = e;
    }
}
/* inicial para el icono de la fila: primer caracter que no sea puntuacion ASCII (en mayuscula) */
static void ra_initial(const char *name, char o[8])
{
    const unsigned char *p = (const unsigned char *)name;
    while (*p && *p < 0x80 && !isalnum(*p)) p++;
    if (!*p) { o[0] = '#'; o[1] = 0; return; }
    int n = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1, k = 0;
    o[k++] = n == 1 ? (char)toupper(*p) : (char)*p;
    for (int i = 1; i < n && (p[i] & 0xC0) == 0x80; i++) o[k++] = (char)p[i];
    o[k] = 0;
}

/* ------------------------------------------------------------------ codecs */
/* lo que dice el directorio -> RS_*: MP3 y AAC (ADTS) suenan, lo desconocido se prueba (el
 * ayudante mira las tramas), HLS / Ogg / Opus / FLAC / video no */
static int ra_codec_kind(const char *codec, int hls)
{
    if (hls) return RS_HLS;
    if (!codec[0] || !strcasecmp(codec, "UNKNOWN")) return RS_UNKNOWN;
    if (!strcasecmp(codec, "MP3")) return RS_MP3;
    if (!strcasecmp(codec, "AAC") || !strcasecmp(codec, "AAC+")) return RS_AAC;
    if (!strcasecmp(codec, "FLAC")) return RS_FLAC;
    return RS_OGG;                        /* OGG, OPUS, "AAC,H.264", WMA...: no suena */
}
static int ra_playable(int kind) { return kind == RS_MP3 || kind == RS_AAC || kind == RS_UNKNOWN; }
/* "MP3 128k", "AAC", "128k", "" */
static void ra_qlabel(const struct ra_station *s, char *o, int cap)
{
    const char *c = s->kind == RS_MP3 ? "MP3" : s->kind == RS_AAC ? "AAC" : "";
    if (s->br > 0) snprintf(o, cap, "%s%s%dk", c, c[0] ? " " : "", s->br);
    else snprintf(o, cap, "%s", c);
}
/* segunda linea de la fila: "ES · pop, rock" */
static void ra_sub(const struct ra_station *s, char *o, int cap)
{
    snprintf(o, cap, "%s%s%s", s->cc, s->cc[0] && s->tags[0] ? " \xC2\xB7 " : "", s->tags);
}
static int ra_same(const struct ra_station *a, const struct ra_station *b)
{
    if (a->uuid[0] && b->uuid[0]) return !strcmp(a->uuid, b->uuid);
    return !strcmp(a->url, b->url);
}

/* ------------------------------------------------------------------ directorio (Radio Browser) */
static const char *const RA_HOSTS[] = { "de1.api.radio-browser.info", "fi1.api.radio-browser.info", "de2.api.radio-browser.info" };
#define RA_NHOST ((int)(sizeof RA_HOSTS / sizeof RA_HOSTS[0]))
enum { RL_TOP, RL_COUNTRY, RL_TAG, RL_SEARCH };
/* consulta de una lista de emisoras; arg: codigo de pais, etiqueta o texto buscado */
static void ra_list_url(char *o, int n, int host, int kind, const char *arg, int limit, int offset)
{
    char e[300] = ""; fm_urlenc(arg ? arg : "", e, sizeof e);
    const char *f = kind == RL_COUNTRY ? "&countrycode=" : kind == RL_TAG ? "&tag=" : kind == RL_SEARCH ? "&name=" : "";
    snprintf(o, n, "https://%s/json/stations/search?order=clickcount&reverse=true&hidebroken=true&limit=%d&offset=%d%s%s",
             RA_HOSTS[(unsigned)host % RA_NHOST], limit, offset, f, kind == RL_TOP ? "" : e);
}
static void ra_countries_url(char *o, int n, int host)
{
    snprintf(o, n, "https://%s/json/countries?order=stationcount&reverse=true&hidebroken=true&limit=250", RA_HOSTS[(unsigned)host % RA_NHOST]);
}
/* el directorio pide que se le avise de cada emisora que se escucha (cuenta de clics) */
static int ra_click_url(char *o, int n, int host, const char *uuid)
{
    for (const char *p = uuid; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return -1;
    if (!uuid[0]) return -1;
    snprintf(o, n, "https://%s/json/url/%s", RA_HOSTS[(unsigned)host % RA_NHOST], uuid);
    return 0;
}
/* respuesta JSON -> emisoras que pueden sonar, sin URL repetidas. *seen = elementos de la
 * respuesta (para pedir la pagina siguiente). -1 si no es una lista. */
static int ra_parse_stations(const char *js, struct ra_station *o, int max, int *seen)
{
    int n = 0, tot = 0;
    if (seen) *seen = 0;
    const char *p = fj_w(js);
    if (!p || *p != '[') return -1;
    for (const char *e = fj_first(js); e; e = fj_next(e)) {
        tot++;
        if (n >= max) continue;
        struct ra_station s; memset(&s, 0, sizeof s);
        char url[1100], tags[400], num[16];
        fj_gs(e, "url_resolved", url, sizeof url); ra_clean(url);
        if (!rs_url_ok(url)) { fj_gs(e, "url", url, sizeof url); ra_clean(url); }
        if (!rs_url_ok(url) || strlen(url) >= RA_URLN || strchr(url, ' ')) continue;
        fj_gs(e, "codec", s.codec, sizeof s.codec);
        fj_gs(e, "hls", num, sizeof num);
        s.kind = (unsigned char)ra_codec_kind(s.codec, atoi(num) != 0 || rs_url_kind(url) == RS_HLS);
        if (!ra_playable(s.kind)) continue;
        if (s.kind == RS_UNKNOWN) s.codec[0] = 0;
        fj_gs(e, "name", s.name, sizeof s.name); ra_clean(s.name);
        if (!s.name[0]) continue;
        int dup = 0; for (int k = 0; k < n && !dup; k++) if (!strcmp(o[k].url, url)) dup = 1;
        if (dup) continue;
        fm_cpy(s.url, sizeof s.url, url);
        fj_gs(e, "stationuuid", s.uuid, sizeof s.uuid);
        fj_gs(e, "countrycode", s.cc, sizeof s.cc);
        for (char *c = s.cc; *c; c++) *c = (char)toupper((unsigned char)*c);
        fj_gs(e, "tags", tags, sizeof tags); ra_clean(tags); ra_tags(tags, s.tags, sizeof s.tags, 3);
        long long br = fj_gl(e, "bitrate"); s.br = (short)(br > 0 && br < 4000 ? br : 0);
        o[n++] = s;
    }
    if (seen) *seen = tot;
    return n;
}
static int ra_parse_countries(const char *js, struct ra_country *o, int max)
{
    int n = 0;
    const char *p = fj_w(js);
    if (!p || *p != '[') return -1;
    for (const char *e = fj_first(js); e && n < max; e = fj_next(e)) {
        struct ra_country c; memset(&c, 0, sizeof c);
        char cc[16]; fj_gs(e, "iso_3166_1", cc, sizeof cc);
        if (strlen(cc) != 2 || !isalpha((unsigned char)cc[0]) || !isalpha((unsigned char)cc[1])) continue;
        c.cc[0] = (char)toupper((unsigned char)cc[0]); c.cc[1] = (char)toupper((unsigned char)cc[1]);
        fj_gs(e, "name", c.name, sizeof c.name); ra_clean(c.name);
        c.n = (int)fj_gl(e, "stationcount");
        if (!c.name[0] || c.n <= 0) continue;
        o[n++] = c;
    }
    return n;
}
/* pais por defecto segun el idioma del sistema (se puede cambiar en la app) */
static const struct { const char *lang, *cc, *name; } RA_DEFCC[] = {
    { "es", "ES", "España" }, { "en", "US", "United States" }, { "fr", "FR", "France" }, { "de", "DE", "Deutschland" },
    { "it", "IT", "Italia" }, { "ja", "JP", "日本" }, { "ko", "KR", "대한민국" }, { "pl", "PL", "Polska" },
    { "ru", "RU", "Россия" }, { "zh-Hans", "CN", "中国" }, { "th", "TH", "ประเทศไทย" }, { "zh-Hant", "TW", "台灣" },
    { "uk", "UA", "Україна" },
};
static void ra_lang_country(const char *lang, char cc[4], char *name, int cap)
{
    int k = 1;
    for (int i = 0; i < (int)(sizeof RA_DEFCC / sizeof RA_DEFCC[0]); i++) if (!strcmp(lang, RA_DEFCC[i].lang)) k = i;
    snprintf(cc, 4, "%s", RA_DEFCC[k].cc); snprintf(name, cap, "%s", RA_DEFCC[k].name);
}
/* generos del directorio (etiquetas en ingles); la clave es el texto en espanol de tr() */
static const struct { const char *key, *tag; } RA_GENRES[] = {
    { "Pop", "pop" }, { "Rock", "rock" }, { "Noticias", "news" }, { "Clásica", "classical" }, { "Jazz", "jazz" },
    { "Electrónica", "electronic" }, { "Dance", "dance" }, { "Hip hop", "hip hop" }, { "Éxitos de siempre", "oldies" },
    { "Años 80", "80s" }, { "Años 90", "90s" }, { "Latina", "latin" }, { "Country", "country" }, { "Blues", "blues" },
    { "Metal", "metal" }, { "Reggae", "reggae" }, { "Folk", "folk" }, { "Ambient", "ambient" }, { "Chillout", "chillout" },
    { "Deportes", "sports" }, { "Tertulia", "talk" }, { "Infantil", "children" },
};
#define RA_NGENRE ((int)(sizeof RA_GENRES / sizeof RA_GENRES[0]))

/* ------------------------------------------------------------------ biblioteca del usuario */
#define RA_MAXFAV 120
#define RA_MAXREC 12
#define RA_MAXOWN 40
struct ra_lib {
    struct ra_station fav[RA_MAXFAV], rec[RA_MAXREC], own[RA_MAXOWN];
    int nfav, nrec, nown;
};
static int ra_ix(const struct ra_station *a, int n, const struct ra_station *s)
{
    for (int i = 0; i < n; i++) if (ra_same(&a[i], s)) return i;
    return -1;
}
static int ra_is_fav(const struct ra_lib *L, const struct ra_station *s) { return ra_ix(L->fav, L->nfav, s) >= 0; }
static int ra_is_own(const struct ra_lib *L, const struct ra_station *s) { return !s->uuid[0] && ra_ix(L->own, L->nown, s) >= 0; }
/* 1 = ahora es favorita, 0 = ya no, -1 = no caben mas */
static int ra_fav_toggle(struct ra_lib *L, const struct ra_station *s)
{
    int i = ra_ix(L->fav, L->nfav, s);
    if (i >= 0) { memmove(&L->fav[i], &L->fav[i + 1], (size_t)(L->nfav - i - 1) * sizeof *s); L->nfav--; return 0; }
    if (L->nfav >= RA_MAXFAV) return -1;
    L->fav[L->nfav++] = *s;
    return 1;
}
/* la ultima escuchada, la primera */
static void ra_rec_add(struct ra_lib *L, const struct ra_station *s)
{
    struct ra_station t = *s;
    int i = ra_ix(L->rec, L->nrec, &t);
    if (i < 0) { if (L->nrec < RA_MAXREC) L->nrec++; i = L->nrec - 1; }
    memmove(&L->rec[1], &L->rec[0], (size_t)i * sizeof t);
    L->rec[0] = t;
}
/* URL escrita a mano: sin espacios, con http:// si no trae esquema. 0 = valida */
static int ra_url_norm(const char *in, char *o, int cap)
{
    char t[RA_URLN + 16]; fm_cpy(t, sizeof t, in); ra_clean(t);
    if (!t[0] || strchr(t, ' ') || strlen(t) >= RA_URLN - 8) return -1;
    if (!strncasecmp(t, "http://", 7)) memcpy(t, "http://", 7);
    else if (!strncasecmp(t, "https://", 8)) memcpy(t, "https://", 8);
    else if (strstr(t, "://")) return -1;
    else { memmove(t + 7, t, strlen(t) + 1); memcpy(t, "http://", 7); }
    const char *h = strstr(t, "://") + 3;                     /* un servidor de verdad: con punto o puerto */
    int hl = (int)strcspn(h, "/?#");
    if (!rs_url_ok(t) || (int)strlen(t) >= cap || hl < 3 || (!memchr(h, '.', hl) && !memchr(h, ':', hl))) return -1;
    snprintf(o, cap, "%s", t);
    return 0;
}
/* nombre por defecto de una emisora propia: el servidor de su URL */
static void ra_url_host(const char *url, char *o, int cap)
{
    const char *p = strstr(url, "://"); p = p ? p + 3 : url;
    int n = (int)strcspn(p, "/:?#"); if (n > cap - 1) n = cap - 1;
    memcpy(o, p, n); o[n] = 0;
}
/* indice de la nueva; -1 URL no valida, -2 ya esta, -3 no caben mas */
static int ra_own_add(struct ra_lib *L, const char *name, const char *url)
{
    struct ra_station s; memset(&s, 0, sizeof s);
    if (ra_url_norm(url, s.url, sizeof s.url)) return -1;
    fm_cpy(s.name, sizeof s.name, name); ra_clean(s.name);
    if (!s.name[0]) ra_url_host(s.url, s.name, sizeof s.name);
    int k = rs_url_kind(s.url);
    s.kind = (unsigned char)(k == RS_MP3 || k == RS_AAC ? k : RS_UNKNOWN);
    if (ra_ix(L->own, L->nown, &s) >= 0) return -2;
    if (L->nown >= RA_MAXOWN) return -3;
    L->own[L->nown] = s;
    return L->nown++;
}
static void ra_own_del(struct ra_lib *L, int i)
{
    if (i < 0 || i >= L->nown) return;
    struct ra_station s = L->own[i];
    memmove(&L->own[i], &L->own[i + 1], (size_t)(L->nown - i - 1) * sizeof s); L->nown--;
    int r = ra_ix(L->rec, L->nrec, &s);
    if (r >= 0 && !L->rec[r].uuid[0]) { memmove(&L->rec[r], &L->rec[r + 1], (size_t)(L->nrec - r - 1) * sizeof s); L->nrec--; }
}
/* fichero: una linea por emisora, campos separados por tabulador:
 *   F|R|C  uuid  nombre  url  pais  codec  kbps  etiquetas */
static void ra_lib_write(const struct ra_lib *L, FILE *f)
{
    const struct ra_station *a[3] = { L->fav, L->rec, L->own }; const int n[3] = { L->nfav, L->nrec, L->nown };
    fputs("# HiByOS Radio 1\n", f);
    for (int k = 0; k < 3; k++) for (int i = 0; i < n[k]; i++) {
        const struct ra_station *s = &a[k][i];
        fprintf(f, "%c\t%s\t%s\t%s\t%s\t%s\t%d\t%s\n", "FRC"[k], s->uuid, s->name, s->url, s->cc, s->codec, s->br, s->tags);
    }
}
static void ra_lib_parse(struct ra_lib *L, char *text)
{
    memset(L, 0, sizeof *L);
    for (char *l = text, *nx; l && *l; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        l[strcspn(l, "\r")] = 0;
        if (l[0] == '#' || !l[0] || l[1] != '\t') continue;
        char *f[8]; int nf = 0; f[nf++] = l;
        for (char *p = l; *p && nf < 8; p++) if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        if (nf < 4) continue;
        struct ra_station s; memset(&s, 0, sizeof s);
        fm_cpy(s.uuid, sizeof s.uuid, f[1]); fm_cpy(s.name, sizeof s.name, f[2]); ra_clean(s.name);
        if (!rs_url_ok(f[3]) || strlen(f[3]) >= RA_URLN || !s.name[0]) continue;
        fm_cpy(s.url, sizeof s.url, f[3]);
        if (nf > 4) fm_cpy(s.cc, sizeof s.cc, f[4]);
        if (nf > 5) fm_cpy(s.codec, sizeof s.codec, f[5]);
        if (nf > 6) { int b = atoi(f[6]); s.br = (short)(b > 0 && b < 4000 ? b : 0); }
        if (nf > 7) fm_cpy(s.tags, sizeof s.tags, f[7]);
        s.kind = (unsigned char)ra_codec_kind(s.codec, 0);
        if (!ra_playable(s.kind)) s.kind = RS_UNKNOWN;
        if (!s.uuid[0] && s.kind == RS_UNKNOWN) { int k = rs_url_kind(s.url); if (k == RS_MP3 || k == RS_AAC) s.kind = (unsigned char)k; }
        switch (l[0]) {
        case 'F': if (L->nfav < RA_MAXFAV && ra_ix(L->fav, L->nfav, &s) < 0) L->fav[L->nfav++] = s; break;
        case 'R': if (L->nrec < RA_MAXREC && ra_ix(L->rec, L->nrec, &s) < 0) L->rec[L->nrec++] = s; break;
        case 'C': s.uuid[0] = 0; if (L->nown < RA_MAXOWN && ra_ix(L->own, L->nown, &s) < 0) L->own[L->nown++] = s; break;
        }
    }
}

/* ------------------------------------------------------------------ estado del ayudante */
struct ra_hstate { char st[16], file[300], codec[8], title[256], err[24]; int gen, br, pct, open; };
/* fichero "clave=valor" que escribe hibynet radio; 1 = tiene estado */
static int ra_state_parse(const char *text, struct ra_hstate *o)
{
    memset(o, 0, sizeof *o);
    for (const char *l = text; l && *l; ) {
        const char *e = strchr(l, '\n'); int n = e ? (int)(e - l) : (int)strlen(l);
        const char *q = memchr(l, '=', n);
        if (q) {
            int kl = (int)(q - l), vl = n - kl - 1; const char *v = q + 1;
            char *d = 0; int cap = 0;
            if (kl == 2 && !memcmp(l, "st", 2)) { d = o->st; cap = sizeof o->st; }
            else if (kl == 4 && !memcmp(l, "file", 4)) { d = o->file; cap = sizeof o->file; }
            else if (kl == 5 && !memcmp(l, "codec", 5)) { d = o->codec; cap = sizeof o->codec; }
            else if (kl == 5 && !memcmp(l, "title", 5)) { d = o->title; cap = sizeof o->title; }
            else if (kl == 3 && !memcmp(l, "err", 3)) { d = o->err; cap = sizeof o->err; }
            else if (kl == 3 && !memcmp(l, "gen", 3)) o->gen = atoi(v);
            else if (kl == 2 && !memcmp(l, "br", 2)) o->br = atoi(v);
            else if (kl == 3 && !memcmp(l, "pct", 3)) o->pct = atoi(v);
            else if (kl == 4 && !memcmp(l, "open", 4)) o->open = atoi(v) != 0;
            if (d) {
                if (vl >= (int)sizeof o->file && d == o->file) vl = 0;          /* una ruta cortada no sirve */
                if (vl > cap - 1) vl = cap - 1;
                memcpy(d, v, vl); d[vl] = 0;
                if (d == o->title) { char t[256]; rs_cpy(t, sizeof t, d); snprintf(d, cap, "%s", t); }
            }
        }
        l = e ? e + 1 : 0;
    }
    return o->st[0] != 0;
}

/* ------------------------------------------------------------------ ruta para el reproductor
 * El motor de fabrica nombra los ficheros como "a:\Carpeta\x.mp3" (a: = la tarjeta). Lo que
 * esta en la tarjeta lo traduce playctl; para el tmpfs hay dos formas de decirlo y se prueba
 * cual abre (el ayudante avisa con open=1):
 *   0  la ruta Linux tal cual (playctl la manda como "\tmp\hibyradio\x.mp3", sin unidad)
 *   1  saliendo de la tarjeta: "a:\..\..\..\..\..\..\tmp\hibyradio\x.mp3" */
#define RA_NPATHMODE 2
static int ra_on_card(const char *f)
{
    return !strncmp(f, "/usr/data/mnt/", 14) || !strncmp(f, "/data/mnt/", 10) || !strncmp(f, "/mnt/", 5);
}
static void ra_player_path(const char *file, int mode, char *o, int cap)
{
    if (mode != 1 || ra_on_card(file) || file[0] != '/') { snprintf(o, cap, "%s", file); return; }
    int k = snprintf(o, cap, "a:\\..\\..\\..\\..\\..\\..");
    for (const char *p = file; *p && k < cap - 1; p++) o[k++] = *p == '/' ? '\\' : *p;
    o[k < cap ? k : cap - 1] = 0;
}

/* ------------------------------------------------------------------ ajustes */
#define RA_TMPDIR "/tmp/hibyradio"
struct ra_conf {
    char dir[160];               /* donde escribe el ayudante (tmpfs; o una carpeta de la SD) */
    int presize_mb;              /* tamano logico del fichero disperso; 0 = fichero que crece */
    int prebuf_kb;               /* 0 = lo decide el ayudante (unos 5 s de audio) */
    char cc[4], cname[64];       /* pais elegido ("" = el del idioma) */
    int tab;                     /* pestana abierta la ultima vez */
    int pathmode;                /* como entiende el reproductor la ruta (ra_player_path); -1 = por probar */
};
static void ra_conf_default(struct ra_conf *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", RA_TMPDIR); c->presize_mb = 256; c->pathmode = -1;
}
static int ra_dir_ok(const char *d)
{
    if (d[0] != '/' || !d[1] || strlen(d) >= 150 || strstr(d, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)d; *p; p++) if (*p <= ' ' || *p == '\\' || *p == '\'' || *p == '"' || *p == 0x7f) return 0;
    /* nunca la raiz de un almacenamiento: el ayudante borra sus ficheros al acabar */
    int slashes = 0; for (const char *p = d; *p; p++) if (*p == '/' && p[1]) slashes++;
    return slashes >= 2;
}
static void ra_conf_parse(struct ra_conf *c, char *text)
{
    ra_conf_default(c);
    for (char *l = text, *nx; l && *l; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        l[strcspn(l, "\r")] = 0;
        char *v = strchr(l, '='); if (!v || l[0] == '#') continue;
        *v++ = 0;
        if (!strcmp(l, "dir")) { if (ra_dir_ok(v)) snprintf(c->dir, sizeof c->dir, "%s", v); }
        else if (!strcmp(l, "presize_mb")) { int n = atoi(v); if (n >= 0 && n <= 1024) c->presize_mb = n; }
        else if (!strcmp(l, "prebuf_kb")) { int n = atoi(v); if (n >= 0 && n <= 1024) c->prebuf_kb = n; }
        else if (!strcmp(l, "country")) { if (strlen(v) == 2 && isalpha((unsigned char)v[0]) && isalpha((unsigned char)v[1])) { c->cc[0] = (char)toupper((unsigned char)v[0]); c->cc[1] = (char)toupper((unsigned char)v[1]); c->cc[2] = 0; } }
        else if (!strcmp(l, "country_name")) { fm_cpy(c->cname, sizeof c->cname, v); ra_clean(c->cname); }
        else if (!strcmp(l, "tab")) c->tab = atoi(v) == 1;
        else if (!strcmp(l, "pathmode")) { int n = atoi(v); c->pathmode = n >= 0 && n < RA_NPATHMODE ? n : -1; }
    }
    if (!c->cc[0] || !c->cname[0]) c->cc[0] = c->cname[0] = 0;
}
static void ra_conf_write(const struct ra_conf *c, FILE *f)
{
    fprintf(f, "dir=%s\npresize_mb=%d\nprebuf_kb=%d\ncountry=%s\ncountry_name=%s\ntab=%d\npathmode=%d\n", c->dir, c->presize_mb, c->prebuf_kb, c->cc, c->cname, c->tab, c->pathmode);
}
#endif
