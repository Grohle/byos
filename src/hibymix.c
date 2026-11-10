/* hibymix - mezclas a peticion para HiBy (nada en segundo plano).
 *   hibymix probe              estado de las listas en la base del reproductor
 *   hibymix make TIPO          genera la mezcla y la registra como lista .m3u
 *   hibymix radio RUTA         "Radio desde cancion": 45 pistas sembradas por una cancion
 *                              (RUTA como en la base, a:\Artista\Album\pista.flac; tambien vale
 *                               /usr/data/mnt/sd_0/..., "Artista - Titulo" o parte del titulo)
 *   hibymix radio-artist NOMBRE   radio sembrada por un artista
 *   hibymix remove             quita las mezclas de hibymod de la base (los .m3u se quedan)
 * Opciones (en cualquier posicion):
 *   --list        no escribe nada: imprime "ruta<TAB>artista<TAB>titulo" por pista (la ruta tal
 *                 como esta en la base, a:\...); el titulo de la mezcla sale por stderr
 *   --out FICHERO igual que --list pero a un fichero (escritura atomica), no toca SD ni base
 *   --lang es|en|fr  idioma de los nombres de las mezclas (tambien HIBYMIX_LANG; por defecto es)
 * Tipos: dia (Mezcla del dia), descubre (artistas que no has escuchado ultimamente),
 *        favoritos (lo que mas escuchas y lo que lo rodea), nuevas (lo ultimo anadido, variado),
 *        decada (la decada que mas escuchas).
 * Lee usrlocal_media.db (MEDIA_TABLE, HISTORY_TABLE, ctime) y escribe
 *   /usr/data/mnt/sd_0/Mezclas/<Nombre>.m3u   y una fila en M3U_TABLE (sin reescanear).
 * Las cadenas de esa base llevan el NUL final incluido (asi las guarda el reproductor).
 *
 * Radio: la metadata es pobre (genero "Unknown" casi siempre), asi que se puntua por artista con
 *  - grafo de artistas: creditos compartidos ("A, B", "feat. X", album_artist), recopilatorios
 *    (mismo album+ano con varios artistas) y escuchas seguidas en HISTORY_TABLE (+2 saltos);
 *  - perfil de contenido: idioma de titulos/albumes (es/en/eu), ano medio, genero si se conoce;
 *  y por pista: cercania de ano a la semilla, genero, formato, un poco de azar estable por dia.
 * Seleccion voraz con penalizacion por repetir artista (tope por artista y 2 por album),
 * sin las ultimas 15 escuchas, sin la misma cancion duplicada, y orden suave por anos. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "sqlite3.h"

static const char *DB = "/usr/data/usrlocal_media.db";
static const char *MIXDIR = "/usr/data/mnt/sd_0/Mezclas";
static int listmode; static const char *outpath; static int lang;     /* 0 es, 1 en, 2 fr */

struct trk {
    int id; char *path, *name, *artist, *album, *aa, *genre;
    int year; long ctime; int heard; double score;
    int aid, sr, bits; unsigned th, dh; unsigned char lg[3], ost;
};
static struct trk *T; static int nT;
static int hist[64], nhist;                         /* indices de T en el historial (0 = la mas reciente) */

size_t arena_total;
static char *dupz(const unsigned char *s)          /* copia hasta el NUL, en bloques de 32 KB (sin coste por malloc) */
{
    static char *blk; static size_t left; extern size_t arena_total;
    const char *p = s ? (const char *)s : ""; size_t n = strlen(p) + 1;
    if (n == 1) return "";
    if (n > left) { left = n > 32768 ? n : 32768; blk = malloc(left); if (!blk) { left = 0; return ""; } arena_total += left; }
    char *r = blk; memcpy(r, p, n); blk += n; left -= n;
    return r;
}
static unsigned rnd_state;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static double jitter(void) { return (rnd() % 1000) / 1000.0; }

/* ---------- nombres en tres idiomas ---------- */
static const char *L3(const char *es, const char *en, const char *fr) { return lang == 1 ? en : lang == 2 ? fr : es; }

/* ---------- utilidades de texto ---------- */
static unsigned hnorm(const char *s, int len)       /* hash sin mayusculas */
{
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) { unsigned char c = s[i]; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 16777619u; }
    return h;
}
static unsigned htitle(const char *s)                /* titulo sin "(...)", "[...]", " - ..." ni signos */
{
    unsigned h = 2166136261u;
    for (; *s; s++) {
        if (*s == '(' || *s == '[' || (s[0] == ' ' && s[1] == '-' && s[2] == ' ')) break;
        unsigned char c = *s; if (c >= 'A' && c <= 'Z') c += 32;
        if (c < 0x80 && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) continue;
        h = (h ^ c) * 16777619u;
    }
    return h;
}
static unsigned hdir(const char *p)                 /* carpeta de la pista (0 = raiz de la tarjeta) */
{
    const char *e = strrchr(p, '\\'); if (!e) e = strrchr(p, '/');
    if (!e || e - p <= 3) return 0;
    return hnorm(p, (int)(e - p));
}
static int is_ost(const char *album)                /* banda sonora: su "ano" y su estilo no dicen nada del artista */
{
    static const char *k[] = { "soundtrack", "bso", "banda sonora", "o.s.t", "motion picture", "the movie", 0 };
    for (int i = 0; k[i]; i++) if (strcasestr(album, k[i])) return 1;
    return 0;
}
static const char *shortname(const char *s)        /* "Award Tour (feat. X) (LP Version)" -> "Award Tour" */
{
    static char b[128]; int n = 0;
    while (s[n] && n < 127 && !(n > 0 && s[n] == ' ' && (s[n + 1] == '(' || s[n + 1] == '[' || s[n + 1] == '-'))) { b[n] = s[n]; n++; }
    b[n] = 0;
    return b;
}

/* idioma aproximado de un texto: tildes/enes, palabras frecuentes, "tx"/"tz" vascos */
static const char *W_ES = " de la el que y en los las mi no te un una por con para lo se tu yo es del al mas sin su nos le muy como todo nada amor cuando quien donde ya hay vida noche cosas otra otro solo siempre nunca ti mis tus algo bien mal dia hoy mañana corazón canción más así también nadie ";
static const char *W_EN = " the you i my your love of to in on it is and don't i'm we be all night with for this what baby girl heart away home time can't me oh just like out up get down got are was it's she he they there go when one good never let ";
static void langof(const char *s, int *v)
{
    char w[40]; int n = 0, acc = 0;
    for (const unsigned char *p = (const unsigned char *)s; ; p++) {
        if (p[0] == 0xC3 && strchr("\xA1\xA9\xAD\xB3\xBA\xB1\x81\x89\x8D\x93\x9A\x91", p[1]) && p[1] && acc < 2) { v[0]++; acc++; }
        if (p[0] == 0xC2 && (p[1] == 0xBF || p[1] == 0xA1)) v[0]++;
        int c = *p; if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || c == '\'' || c >= 0x80) { if (n < 38) w[n++] = (char)c; continue; }
        if (n > 0) {
            w[n] = 0; char k[44]; snprintf(k, sizeof k, " %s ", w);
            if (strstr(W_ES, k)) v[0]++;
            else if (strstr(W_EN, k)) v[1]++;
            if (n > 2 && (strstr(w, "tx") || strstr(w, "tz"))) v[2]++;
        }
        n = 0; if (!c) break;
    }
}

/* ---------- artistas y grafo de relaciones ---------- */
struct art { char *name; int nfull, n, heard, ysum, yn, hi, ost, lg[3]; const char *genre; float a1, a2, sc; int cnt; };
static struct art *A; static int nA, capA;
static int *HT; static unsigned hmask;
struct edge { int a, b; float w; };
static struct edge *E; static int nE, capE;

static int art_get(const char *s, int len, int create)
{
    while (len > 0 && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len <= 0) return -1;
    unsigned h = hnorm(s, len) & hmask;
    for (; HT[h] >= 0; h = (h + 1) & hmask) {
        const char *q = A[HT[h]].name;
        if (!strncasecmp(q, s, len) && !q[len]) return HT[h];
    }
    if (!create || (unsigned)nA * 2 >= hmask) return -1;
    if (nA >= capA) { capA = capA ? capA * 2 : 512; A = realloc(A, capA * sizeof *A); }
    memset(&A[nA], 0, sizeof *A);
    A[nA].name = malloc(len + 1); memcpy(A[nA].name, s, len); A[nA].name[len] = 0;
    HT[h] = nA;
    return nA++;
}
static void edge(int a, int b, float w)
{
    if (a < 0 || b < 0 || a == b) return;
    if (nE >= capE) { capE = capE ? capE * 2 : 1024; E = realloc(E, capE * sizeof *E); }
    E[nE].a = a; E[nE].b = b; E[nE].w = w; nE++;
}
/* separa "A, B feat. C" en partes; devuelve cuantas */
static int rawsplit(const char *s, int *st, int *ln, int max)
{
    static const char *sep[] = { ", ", " & ", " feat. ", " feat ", " ft. ", " featuring ", 0 };
    int k = 0, b = 0, i = 0;
    while (s[i]) {
        int m = 0;
        for (int j = 0; sep[j]; j++) if (!strncasecmp(s + i, sep[j], strlen(sep[j]))) { m = (int)strlen(sep[j]); break; }
        if (m && k < max - 1) { st[k] = b; ln[k++] = i - b; i += m; b = i; } else i++;
    }
    st[k] = b; ln[k++] = i - b;
    return k;
}
/* partes de un credito; "Daniel, Me Estás Matando" se queda entero (nombre de grupo con coma) */
static int credit(const char *s, int *st, int *ln, int max)
{
    int k = rawsplit(s, st, ln, max);
    if (k > 1) {
        int full = art_get(s, (int)strlen(s), 0), f0 = art_get(s + st[0], ln[0], 0);
        if (full >= 0 && A[full].nfull >= 3 && !(f0 >= 0 && A[f0].nfull > 0)) { st[0] = 0; ln[0] = (int)strlen(s); k = 1; }
    }
    return k;
}

static int cmp2(const void *x, const void *y) { unsigned a = *(const unsigned *)x, b = *(const unsigned *)y; return a < b ? -1 : a > b; }

static void build_artists(void)
{
    unsigned sz = 1024; while (sz < (unsigned)nT * 2 + 64) sz <<= 1;
    hmask = sz - 1; HT = malloc(sz * sizeof *HT); memset(HT, 0xff, sz * sizeof *HT);
    for (int i = 0; i < nT; i++) { int a = art_get(T[i].artist, (int)strlen(T[i].artist), 1); if (a >= 0) A[a].nfull++; }
    int st[8], ln[8];
    for (int i = 0; i < nT; i++) {
        struct trk *t = &T[i];
        int k = credit(t->artist, st, ln, 8), p0 = -1;
        for (int j = 0; j < k; j++) {
            int a = art_get(t->artist + st[j], ln[j], 1);
            if (j == 0) p0 = a; else { edge(p0, a, 1.0f); for (int q = 1; q < j; q++) edge(art_get(t->artist + st[q], ln[q], 0), a, 0.5f); }
        }
        if (p0 < 0) p0 = art_get("?", 1, 1); if (p0 < 0) p0 = 0;
        t->aid = p0;
        struct art *r = &A[p0]; r->n++;
        if (t->heard) r->heard++;
        if (t->year > 1900 && t->year < 2200) { r->ysum += t->year; r->yn++; }
        if (t->bits > 16 || t->sr > 48000) r->hi++;
        if (t->genre[0] && strcasecmp(t->genre, "Unknown") && !r->genre) r->genre = t->genre;
        int v[3] = {0, 0, 0}; langof(t->name, v); for (int j = 0; j < 3; j++) t->lg[j] = (unsigned char)(v[j] > 9 ? 9 : v[j]);
        langof(t->album, v); for (int j = 0; j < 3; j++) r->lg[j] += v[j];
        /* album_artist con varios nombres, e invitados "(feat. X)" en el titulo */
        if (t->aa[0] && strcasecmp(t->aa, t->artist)) {
            int ka = credit(t->aa, st, ln, 8);
            for (int j = 0; j < ka; j++) edge(p0, art_get(t->aa + st[j], ln[j], 1), 0.7f);
        }
        const char *f = strcasestr(t->name, "feat. "); if (!f) f = strcasestr(t->name, "(with ");
        if (f) {
            f = strchr(f, ' ') + 1;
            char g[160]; int n = 0; while (f[n] && f[n] != ')' && f[n] != ']' && n < 159) { g[n] = f[n]; n++; } g[n] = 0;
            int kf = rawsplit(g, st, ln, 8);
            for (int j = 0; j < kf; j++) edge(p0, art_get(g + st[j], ln[j], 0), 0.8f);
        }
        t->th = htitle(t->name);                      /* misma cancion = mismo titulo, aunque sea otra version */
        t->dh = hdir(t->path);
        if ((t->ost = is_ost(t->album))) r->ost++;
    }
    /* recopilatorios: mismo album y ano con varios artistas */
    unsigned *ab = malloc(nT * 2 * sizeof *ab); int nab = 0;
    for (int i = 0; i < nT; i++) {
        if (!T[i].album[0] || !strcasecmp(T[i].album, "Unknown")) continue;
        ab[2 * nab] = hnorm(T[i].album, (int)strlen(T[i].album)) * 2654435761u + (unsigned)T[i].year; ab[2 * nab + 1] = (unsigned)T[i].aid; nab++;
    }
    qsort(ab, nab, 2 * sizeof *ab, cmp2);
    for (int i = 0, j; i < nab; i = j) {
        for (j = i; j < nab && ab[2 * j] == ab[2 * i]; j++) ;
        int d[32], nd = 0;
        for (int q = i; q < j && nd < 32; q++) { int a = (int)ab[2 * q + 1], s = 0; for (int z = 0; z < nd; z++) if (d[z] == a) s = 1; if (!s) d[nd++] = a; }
        for (int x = 0; x < nd; x++) for (int y = x + 1; y < nd; y++) edge(d[x], d[y], 0.5f);
    }
    free(ab);
    /* escuchas seguidas en el historial */
    for (int i = 0; i < nhist; i++) for (int d = 1; d <= 2 && i + d < nhist; d++)
        if (hist[i] >= 0 && hist[i + d] >= 0) edge(T[hist[i]].aid, T[hist[i + d]].aid, 0.3f / d);
}

static int load(sqlite3 *db)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT id, path, name, artist, album, year, ctime, album_artist, genre, sample_rate, bit FROM MEDIA_TABLE WHERE has_child_file = 0 OR has_child_file IS NULL", -1, &st, 0)) return 0;
    int cap = 64; sqlite3_stmt *cn;                  /* tamano exacto: sin realloc que duplique */
    if (!sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM MEDIA_TABLE", -1, &cn, 0)) { if (sqlite3_step(cn) == SQLITE_ROW) cap = sqlite3_column_int(cn, 0) + 64; sqlite3_finalize(cn); }
    T = malloc(cap * sizeof *T);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (nT >= cap) { cap *= 2; T = realloc(T, cap * sizeof *T); }
        struct trk *t = &T[nT++]; memset(t, 0, sizeof *t);
        t->id = sqlite3_column_int(st, 0);
        t->path = dupz(sqlite3_column_text(st, 1)); t->name = dupz(sqlite3_column_text(st, 2));
        t->artist = dupz(sqlite3_column_text(st, 3)); t->album = dupz(sqlite3_column_text(st, 4));
        t->year = sqlite3_column_int(st, 5); t->ctime = (long)sqlite3_column_int64(st, 6);
        const char *aa = (const char *)sqlite3_column_text(st, 7), *g = (const char *)sqlite3_column_text(st, 8);
        t->aa = !aa || !strcmp(aa, t->artist) ? t->artist : dupz((const unsigned char *)aa);   /* casi siempre igual */
        t->genre = !g || !strcasecmp(g, "Unknown") ? "" : dupz((const unsigned char *)g);
        t->sr = sqlite3_column_int(st, 9); t->bits = sqlite3_column_int(st, 10);
    }
    sqlite3_finalize(st);
    /* historial: ultimas escuchas (posicion 0 = la mas reciente) */
    if (!sqlite3_prepare_v2(db, "SELECT path FROM HISTORY_TABLE ORDER BY rowid DESC", -1, &st, 0)) {
        int pos = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(st, 0); pos++;
            int f = -1;
            for (int i = 0; i < nT; i++) if (p && !strcmp(T[i].path, p)) { if (!T[i].heard) T[i].heard = pos; f = i; break; }
            if (nhist < 64) hist[nhist++] = f;
        }
        sqlite3_finalize(st);
    }
    if (nT) build_artists();
    return nT;
}

static int artist_heard(int i) { return A[T[i].aid].heard > 0; }
static int artist_heard_count(int i) { return A[T[i].aid].heard; }
static int cmp_score(const void *a, const void *b) { double d = ((const struct trk *)b)->score - ((const struct trk *)a)->score; return d > 0 ? 1 : d < 0 ? -1 : 0; }

/* elige N pistas por puntuacion con variedad: max "per_album" por album y "per_artist" por artista */
static int pick(int *out, int n, int per_album, int per_artist)
{
    qsort(T, nT, sizeof *T, cmp_score);
    int k = 0;
    for (int i = 0; i < nT && k < n; i++) {
        if (T[i].score <= 0) break;
        int na = 0, nr = 0;
        for (int j = 0; j < k; j++) {
            if (!strcasecmp(T[out[j]].album, T[i].album)) na++;
            if (T[out[j]].aid == T[i].aid) nr++;
        }
        if (na >= per_album || nr >= per_artist) continue;
        out[k++] = i;
    }
    /* orden final: mezclado sin dos seguidas del mismo artista si se puede */
    for (int i = k - 1; i > 0; i--) { int j = rnd() % (i + 1); int x = out[i]; out[i] = out[j]; out[j] = x; }
    for (int i = 1; i < k; i++) if (T[out[i]].aid == T[out[i - 1]].aid)
        for (int j = i + 1; j < k; j++) if (T[out[j]].aid != T[out[i - 1]].aid) { int x = out[i]; out[i] = out[j]; out[j] = x; break; }
    return k;
}

/* ---------- salida: .m3u + M3U_TABLE, o lista para la interfaz ---------- */
static void putclean(FILE *f, const char *s) { for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f); }

static int emit(sqlite3 *db, const char *title0, const int *out, int k)
{
    char title[160]; int n = 0;                       /* nombre valido como fichero */
    for (const char *c = title0; *c && n < 120; c++) title[n++] = strchr("/\\:*?\"<>|\t\n", *c) ? ' ' : *c;
    while (n > 0 && ((title[n - 1] & 0xC0) == 0x80 || title[n - 1] == ' ')) {   /* no cortar a mitad de un caracter */
        if (title[n - 1] == ' ') { n--; continue; }
        int m = n; while (m > 0 && (title[m - 1] & 0xC0) == 0x80) m--;
        if (m > 0) { unsigned char lead = (unsigned char)title[m - 1]; int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2; if (n - m + 1 == need) break; n = m - 1; } else n = 0;
    }
    title[n] = 0;
    if (listmode || outpath) {
        char tmp[1040]; FILE *f = stdout;
        if (outpath) { snprintf(tmp, sizeof tmp, "%s.tmp", outpath); f = fopen(tmp, "w"); if (!f) { perror(outpath); return 4; } }
        for (int i = 0; i < k; i++) {
            const struct trk *t = &T[out[i]];
            putclean(f, t->path); fputc('\t', f); putclean(f, t->artist); fputc('\t', f); putclean(f, t->name); fputc('\n', f);
        }
        if (outpath) { fclose(f); if (rename(tmp, outpath)) { perror(outpath); return 4; } } else fflush(stdout);
        fprintf(stderr, "%s: %d\n", title, k);
        return 0;
    }
    /* fichero .m3u (rutas relativas a la carpeta Mezclas) */
    mkdir(MIXDIR, 0755);
    char m3u[512]; snprintf(m3u, sizeof m3u, "%s/%s.m3u", MIXDIR, title);
    char tmp[520]; snprintf(tmp, sizeof tmp, "%s.tmp", m3u);
    FILE *f = fopen(tmp, "w"); if (!f) { perror(m3u); return 4; }
    fprintf(f, "#EXTM3U\n");
    for (int i = 0; i < k; i++) {
        const char *p = T[out[i]].path;
        if (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) p += 3;        /* a:\Artista\... */
        char rel[1024]; snprintf(rel, sizeof rel, "../%s", p);
        for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
        fprintf(f, "#EXTINF:-1,%s - %s\n%s\n", T[out[i]].artist, T[out[i]].name, rel);
    }
    fclose(f); rename(tmp, m3u);
    /* fila en M3U_TABLE, con el mismo formato que el reproductor (NUL final incluido) */
    char dbpath[512]; snprintf(dbpath, sizeof dbpath, "a:\\Mezclas\\%s.m3u", title);
    char name[256]; snprintf(name, sizeof name, "%s.m3u", title);
    char ch[2] = { (char)((title[0] >= 'a' && title[0] <= 'z') ? title[0] - 32 : title[0]), 0 };
    sqlite3_stmt *st; int id = 1;
    if (!sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0)+1 FROM M3U_TABLE", -1, &st, 0)) { if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0); sqlite3_finalize(st); }
    if (!sqlite3_prepare_v2(db, "DELETE FROM M3U_TABLE WHERE path = ?", -1, &st, 0)) { sqlite3_bind_text(st, 1, dbpath, (int)strlen(dbpath) + 1, SQLITE_STATIC); sqlite3_step(st); sqlite3_finalize(st); }
    int rc = sqlite3_prepare_v2(db, "INSERT INTO M3U_TABLE(id, path, name, character) VALUES (?,?,?,?)", -1, &st, 0);
    if (!rc) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_bind_text(st, 2, dbpath, (int)strlen(dbpath) + 1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, name, (int)strlen(name) + 1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, ch, 1, SQLITE_STATIC);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : 1;
        sqlite3_finalize(st);
    }
    printf("%s: %d pistas -> %s (base: %s)\n", title, k, m3u, rc ? sqlite3_errmsg(db) : "ok");
    for (int i = 0; i < k && i < 6; i++) printf("  %s - %s\n", T[out[i]].artist, T[out[i]].name);
    return rc ? 5 : 0;
}

static int make(sqlite3 *db, const char *kind)
{
    const char *title = 0;
    time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
    rnd_state = (unsigned)now;
    long newest = 0; for (int i = 0; i < nT; i++) if (T[i].ctime > newest) newest = T[i].ctime;
    if (!strcmp(kind, "dia")) {
        title = L3("Mezcla del día", "Daily Mix", "Mix du jour");
        rnd_state = (unsigned)(tm.tm_year * 400 + tm.tm_yday) * 2654435761u;     /* la misma todo el dia */
        for (int i = 0; i < nT; i++) T[i].score = 0.2 + jitter() + (T[i].heard ? (T[i].heard < 15 ? -0.6 : 0.3) : 0) + (artist_heard(i) ? 0.25 : 0);
    } else if (!strcmp(kind, "descubre")) {
        title = L3("Descubre", "Discover", "Découvertes");
        for (int i = 0; i < nT; i++) T[i].score = artist_heard(i) ? 0 : 0.5 + jitter();
    } else if (!strcmp(kind, "favoritos")) {
        title = L3("Tus favoritos", "Your Favorites", "Vos favoris");
        for (int i = 0; i < nT; i++) { int c = artist_heard_count(i); T[i].score = c ? 0.3 + c * 0.15 + jitter() * 0.6 + (T[i].heard ? 0.2 : 0) : 0; }
    } else if (!strcmp(kind, "nuevas")) {
        title = L3("Recién llegadas", "Just Added", "Nouveautés");
        long span = 60L * 86400; if (span <= 0) span = 1;
        for (int i = 0; i < nT; i++) { double age = (double)(newest - T[i].ctime) / span; T[i].score = age < 3 ? 1.5 - age * 0.4 + jitter() * 0.5 : 0; }
    } else if (!strcmp(kind, "decada")) {
        int dec[30] = {0}, best = -1, bc = 0;
        for (int i = 0; i < nT; i++) if (T[i].heard && T[i].year > 1900 && T[i].year < 2200) dec[(T[i].year - 1900) / 10]++;
        for (int d = 0; d < 30; d++) if (dec[d] > bc) { bc = dec[d]; best = d; }
        if (best < 0) for (int i = 0; i < nT; i++) if (T[i].year > 1900 && T[i].year < 2200) dec[(T[i].year - 1900) / 10]++;
        if (best < 0) for (int d = 0; d < 30; d++) if (dec[d] > bc) { bc = dec[d]; best = d; }
        static char t[48]; snprintf(t, sizeof t, L3("Los %d", "The %ds", "Les années %d"), 1900 + best * 10); title = t;
        for (int i = 0; i < nT; i++) T[i].score = (T[i].year >= 1900 + best * 10 && T[i].year < 1910 + best * 10) ? 0.5 + jitter() : 0;
    } else { fprintf(stderr, "tipo desconocido\n"); return 2; }

    int out[60], k = pick(out, 40, 2, 4);
    if (k < 5) { fprintf(stderr, "pocas pistas (%d): mezcla no creada\n", k); return 3; }
    return emit(db, title, out, k);
}

/* ---------- radio ---------- */
#define RADIO_N 45
#define ALBK(i) (hnorm(T[i].album, (int)strlen(T[i].album)) * 31u + (unsigned)T[i].aid)   /* album de ese artista: los recopilatorios no topan */
static double lsim(const int *a, const int *b)       /* interseccion de histogramas de idioma (suavizados) */
{
    double sa = a[0] + a[1] + a[2] + 1.5, sb = b[0] + b[1] + b[2] + 1.5, s = 0;
    for (int j = 0; j < 3; j++) { double x = (a[j] + 0.5) / sa, y = (b[j] + 0.5) / sb; s += x < y ? x : y; }
    return s;
}
static int is_path_char(char c) { return c == '\\' || c == '/'; }
static int path_eq(const char *a, const char *b)   /* ignora "a:\", "/usr/data/mnt/sd_0/", / frente a \ y mayusculas */
{
    if (a[0] && a[1] == ':' && is_path_char(a[2])) a += 3;
    if (!strncmp(b, "/usr/data/mnt/sd_0/", 19)) b += 19; else if (b[0] && b[1] == ':' && is_path_char(b[2])) b += 3;
    for (; *a && *b; a++, b++) {
        if (is_path_char(*a) && is_path_char(*b)) continue;
        unsigned char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}
static int find_seed(const char *q)
{
    for (int i = 0; i < nT; i++) if (path_eq(T[i].path, q)) return i;
    const char *d = strstr(q, " - ");                 /* "Artista - Titulo" */
    int best = -1;
    for (int i = 0; i < nT; i++) {
        if (d) { if (strncasecmp(T[i].artist, q, d - q) || !strcasestr(T[i].name, d + 3)) continue; }
        else if (!strcasestr(T[i].name, q)) continue;
        if (best < 0 || strlen(T[i].name) < strlen(T[best].name)) best = i;   /* la coincidencia mas exacta */
    }
    return best;
}

static int radio(sqlite3 *db, const char *q, int by_artist)
{
    int seed = -1, sa = -1;
    if (by_artist) {
        sa = art_get(q, (int)strlen(q), 0);
        if (sa < 0 || !A[sa].n) for (int i = 0; i < nA; i++) if (A[i].n && strcasestr(A[i].name, q)) { sa = i; break; }
        if (sa < 0 || !A[sa].n) { fprintf(stderr, "artista no encontrado: %s\n", q); return 3; }
    } else {
        seed = find_seed(q);
        if (seed < 0) { fprintf(stderr, "cancion no encontrada: %s\n", q); return 3; }
        sa = T[seed].aid;
    }
    time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
    rnd_state = (hnorm(q, (int)strlen(q)) ^ (unsigned)(tm.tm_year * 400 + tm.tm_yday) * 2654435761u) | 1;   /* estable en el dia */
    if (getenv("HIBYMIX_SALT")) rnd_state ^= (unsigned)atoi(getenv("HIBYMIX_SALT")) * 2246822519u;

    /* conjunto semilla: artista principal + coautores de la pista semilla */
    char *inS = calloc(nA, 1); inS[sa] = 1;
    if (seed >= 0) {
        int st[8], ln[8], k = credit(T[seed].artist, st, ln, 8);
        for (int j = 1; j < k; j++) { int a = art_get(T[seed].artist + st[j], ln[j], 0); if (a >= 0) inS[a] = 1; }
    }
    /* afinidad por grafo: vecinos directos y a dos saltos */
    for (int i = 0; i < nA; i++) A[i].a1 = A[i].a2 = 0, A[i].cnt = 0;
    for (int e = 0; e < nE; e++) {
        if (inS[E[e].a] && !inS[E[e].b]) A[E[e].b].a1 += E[e].w * (E[e].a == sa ? 1.0f : 0.6f);
        if (inS[E[e].b] && !inS[E[e].a]) A[E[e].a].a1 += E[e].w * (E[e].b == sa ? 1.0f : 0.6f);
    }
    for (int e = 0; e < nE; e++) {
        int a = E[e].a, b = E[e].b;
        if (inS[a] || inS[b]) continue;
        float fa = A[a].a1 > 1 ? 1 : A[a].a1, fb = A[b].a1 > 1 ? 1 : A[b].a1;
        A[b].a2 += 0.35f * fa * E[e].w; A[a].a2 += 0.35f * fb * E[e].w;
    }
    /* perfil de la semilla */
    struct art *S = &A[sa];
    int slg[3] = { S->lg[0], S->lg[1], S->lg[2] };
    if (seed >= 0) for (int j = 0; j < 3; j++) slg[j] += 2 * T[seed].lg[j];
    double sy = S->yn ? (double)S->ysum / S->yn : 0;
    int syt = seed >= 0 && T[seed].year > 1900 ? T[seed].year : (int)(sy + 0.5);
    const char *sg = seed >= 0 && T[seed].genre[0] && strcasecmp(T[seed].genre, "Unknown") ? T[seed].genre : S->genre;
    int sost = seed >= 0 ? T[seed].ost : S->ost * 2 > S->n;
    double shi = S->n ? (double)S->hi / S->n : 0;
    /* puntuacion por artista: grafo + idioma + epoca + genero + formato */
    for (int i = 0; i < nA; i++) {
        struct art *r = &A[i]; if (!r->n) continue;
        double g = 1 - exp(-(r->a1 + r->a2));
        double ay = r->yn ? (double)r->ysum / r->yn : 0;
        double ys = (sy && ay) ? exp(-fabs(ay - sy) / 7.0) : 0.35;
        double gs = (sg && r->genre) ? (strcasecmp(sg, r->genre) ? -0.3 : 1.0) : 0;   /* raro, pero fiable */
        double hs = 1 - fabs(shi - (double)r->hi / r->n);
        double of = (double)r->ost / r->n, os = sost ? 0.6 * of : -0.9 * of;     /* bandas sonoras con bandas sonoras */
        r->sc = (float)(2.0 * g + 1.4 * lsim(slg, r->lg) + 0.7 * ys + gs + os + 0.1 * hs + (r->heard ? 0.12 : 0));
        if (inS[i] && i != sa) r->sc += 0.8f;
    }
    /* puntuacion por pista */
    for (int i = 0; i < nT; i++) {
        struct trk *t = &T[i];
        double s = A[t->aid].sc;
        if (t->aid == sa) s = 4.0;
        s += (syt && t->year > 1900) ? 0.6 * exp(-abs(t->year - syt) / 5.0) : 0.15;
        if (t->lg[0] + t->lg[1] + t->lg[2]) s += 0.25 * lsim(slg, (int[]){ t->lg[0], t->lg[1], t->lg[2] });
        if (sg && t->genre[0] && !strcasecmp(t->genre, sg)) s += 0.4;
        if (seed >= 0 && t->sr == T[seed].sr && t->bits == T[seed].bits) s += 0.06;
        if (t->heard > 15) s += 0.1;                   /* conocida, pero no reciente */
        s += 0.8 * jitter();
        if (t->heard && t->heard <= 15) s = -1e9;       /* sonaron hace nada */
        if (strcasestr(t->name, "instrumental") || strcasestr(t->name, "(take ") || strcasestr(t->name, "interlude") || strcasestr(t->name, "intro")) s -= 0.6;
        else if (strcasestr(t->name, "live]") || strcasestr(t->name, "live)") || strcasestr(t->name, "live version") || strcasestr(t->name, "directo") || strcasestr(t->name, "demo") || strcasestr(t->name, "remix")) s -= 0.3;
        t->score = s;
    }
    if (by_artist) {                                  /* la mejor del artista abre la radio */
        for (int i = 0; i < nT; i++) if (T[i].aid == sa && T[i].score > -1e8 && (seed < 0 || T[i].score > T[seed].score)) seed = i;
        if (seed < 0) for (int i = 0; i < nT; i++) if (T[i].aid == sa) { seed = i; break; }
    }
    /* seleccion voraz: cada pista mas del mismo artista cuesta; topes por artista y album */
    int out[RADIO_N], k = 0, capS = by_artist ? 7 : 5, capO = 3;
    unsigned *alb = malloc(RADIO_N * sizeof *alb), *tt = malloc(RADIO_N * sizeof *tt);
    char *used = calloc(nT, 1);
    out[k] = seed; used[seed] = 1; A[sa].cnt = 1;
    alb[k] = ALBK(seed); tt[k] = T[seed].th; k++;
    while (k < RADIO_N) {
        int b = -1; double bs = -1e8;
        for (int i = 0; i < nT; i++) {
            if (used[i] || T[i].score < -1e8) continue;
            struct art *r = &A[T[i].aid];
            if (r->cnt >= (T[i].aid == sa ? capS : capO)) continue;
            double s = T[i].score - (T[i].aid == sa ? 0.55 : 0.6) * r->cnt;
            if (s <= bs) continue;
            unsigned ah = ALBK(i); int na = 0, dup = 0;
            for (int j = 0; j < k; j++) { if (alb[j] == ah) na++; if (tt[j] == T[i].th) dup = 1; }
            if (na >= 2 || dup) { used[i] = 1; continue; }   /* nunca podra entrar */
            b = i; bs = s;
        }
        if (b < 0) break;
        out[k] = b; used[b] = 1; A[T[b].aid].cnt++;
        alb[k] = ALBK(b); tt[k] = T[b].th; k++;
    }
    /* orden: se respeta la relevancia (ventana de 6) pero con anos vecinos y sin repetir artista seguido */
    for (int p = 1; p < k; p++) {
        int best = p; double bc = 1e9;
        for (int j = p; j < k && j < p + 6; j++) {
            const struct trk *x = &T[out[p - 1]], *y = &T[out[j]];
            double c = 0.3 * (j - p);
            c += (x->year > 1900 && y->year > 1900) ? (abs(x->year - y->year) > 12 ? 12 : abs(x->year - y->year)) / 6.0 : 0.4;
            if (x->aid == y->aid) c += 3;
            if (p >= 2 && T[out[p - 2]].aid == y->aid) c += 1.5;
            if (p >= 3 && T[out[p - 3]].aid == y->aid) c += 0.8;
            if (!strcasecmp(x->album, y->album)) c += 1;
            if (c < bc) { bc = c; best = j; }
        }
        int v = out[best]; for (int j = best; j > p; j--) out[j] = out[j - 1]; out[p] = v;
    }
    if (getenv("HIBYMIX_DEBUG")) for (int i = 0; i < k; i++)
        fprintf(stderr, "%5.2f g=%.2f/%.2f %4d %-26.26s %s\n", T[out[i]].score, A[T[out[i]].aid].a1, A[T[out[i]].aid].a2, T[out[i]].year, T[out[i]].artist, T[out[i]].name);
    char title[300];
    if (by_artist) snprintf(title, sizeof title, L3("Radio de %s", "%s Radio", "Radio %s"), A[sa].name);
    else snprintf(title, sizeof title, L3("Radio de %s", "%s Radio", "Radio %s"), shortname(T[seed].name));
    free(inS); free(alb); free(tt); free(used);
    if (k < 5) { fprintf(stderr, "pocas pistas (%d): radio no creada\n", k); return 3; }
    return emit(db, title, out, k);
}

int main(int argc, char **argv)
{
    const char *pos[4] = {0}; int np = 0;
    const char *lg = getenv("HIBYMIX_LANG");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) listmode = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) lg = argv[++i];
        else if (np < 4) pos[np++] = argv[i];
    }
    if (lg) lang = !strncmp(lg, "en", 2) ? 1 : !strncmp(lg, "fr", 2) ? 2 : 0;
    if (!np) { fprintf(stderr, "uso: hibymix probe | make TIPO | radio RUTA | radio-artist NOMBRE | remove  [--list] [--out FICHERO] [--lang es|en|fr]\n"); return 1; }
    if (getenv("HIBYMIX_DB")) DB = getenv("HIBYMIX_DB");
    if (getenv("HIBYMIX_DIR")) MIXDIR = getenv("HIBYMIX_DIR");
    int ro = listmode || outpath || !strcmp(pos[0], "stats");
    sqlite3 *db;
    if (sqlite3_open_v2(DB, &db, ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, 0)) { fprintf(stderr, "no se pudo abrir la base\n"); return 2; }
    sqlite3_busy_timeout(db, 4000);
    if (!strcmp(pos[0], "stats")) {          /* para los widgets: una linea por dato, separada por tabuladores */
        const char *q[][2] = {
            { "count", "SELECT COUNT(*), COUNT(DISTINCT album), COUNT(DISTINCT artist), (SELECT COUNT(*) FROM COLLECT_TABLE), COALESCE(SUM(size),0) FROM MEDIA_TABLE WHERE has_child_file = 0 OR has_child_file IS NULL" },
            { "recent", "SELECT REPLACE(artist,char(0),''), REPLACE(album,char(0),'') FROM MEDIA_TABLE GROUP BY album ORDER BY MAX(ctime) DESC LIMIT 1" },
            { "top", "SELECT REPLACE(artist,char(0),''), COUNT(*) c FROM HISTORY_TABLE GROUP BY artist ORDER BY c DESC LIMIT 1" },
            { "last", "SELECT REPLACE(name,char(0),''), REPLACE(artist,char(0),'') FROM HISTORY_TABLE ORDER BY rowid DESC LIMIT 1" },
        };
        for (unsigned k = 0; k < sizeof q / sizeof q[0]; k++) {
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, q[k][1], -1, &st, 0)) continue;
            if (sqlite3_step(st) == SQLITE_ROW) {
                printf("%s", q[k][0]);
                for (int c = 0; c < sqlite3_column_count(st); c++) printf("\t%s", sqlite3_column_text(st, c) ? (const char *)sqlite3_column_text(st, c) : "");
                printf("\n");
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
        return 0;
    }
    if (!strcmp(pos[0], "probe")) {
        sqlite3_stmt *st;
        if (!sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, 0)) { if (sqlite3_step(st) == SQLITE_ROW) printf("journal: %s\n", sqlite3_column_text(st, 0)); sqlite3_finalize(st); }
        const char *q[] = { "SELECT id, path, name, character FROM M3U_TABLE", "SELECT id, path, name, character, cn FROM PLAYLIST_TABLE", 0 };
        for (int k = 0; q[k]; k++) {
            printf("%s\n", q[k]);
            if (sqlite3_prepare_v2(db, q[k], -1, &st, 0)) continue;
            while (sqlite3_step(st) == SQLITE_ROW) printf("  %d | %s | %s | %s\n", sqlite3_column_int(st, 0), sqlite3_column_text(st, 1), sqlite3_column_text(st, 2), sqlite3_column_text(st, 3));
            sqlite3_finalize(st);
        }
        printf("pistas: %d\n", load(db));
        return 0;
    }
    if (!strcmp(pos[0], "remove")) {
        char *err = 0;
        int rc = sqlite3_exec(db, "DELETE FROM M3U_TABLE WHERE path LIKE 'a:\\Mezclas\\%'", 0, 0, &err);
        printf("quitadas: %s\n", rc ? err : "ok");
        return rc;
    }
    int r = 1;
    if (np >= 2 && (!strcmp(pos[0], "make") || !strcmp(pos[0], "radio") || !strcmp(pos[0], "radio-artist"))) {
        if (!load(db)) { fprintf(stderr, "biblioteca vacia\n"); return 3; }
        r = !strcmp(pos[0], "make") ? make(db, pos[1]) : radio(db, pos[1], !strcmp(pos[0], "radio-artist"));
    }
    if (getenv("HIBYMIX_DEBUG")) {                    /* memoria: pico del proceso y lo que pide SQLite */
        FILE *f = fopen("/proc/self/status", "r"); char l[128];
        while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmHWM", 5) || !strncmp(l, "VmRSS", 5)) fprintf(stderr, "%s", l);
        if (f) fclose(f);
        fprintf(stderr, "sqlite pico: %lld KB, pistas %d (%zu KB + textos %zu KB), artistas %d, enlaces %d\n", (long long)sqlite3_memory_highwater(0) / 1024,
                nT, nT * sizeof *T / 1024, arena_total / 1024, nA, nE);
    }
    sqlite3_close(db);
    return r;
}
