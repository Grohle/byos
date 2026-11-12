/* hibytag - etiquetas de audio para HiBy (FLAC, MP3/ID3v1/v2.2-2.4, M4A solo lectura)
 * y actualizacion de la base del reproductor. Ver hibytag.h para la API.
 *
 * Escritura segura (poca RAM, cortes de luz):
 *   - en sitio: solo si los cambios caben en el relleno existente (PADDING de FLAC / relleno ID3)
 *     y la zona a reescribir es pequena (<= HT_INPLACE_MAX): se arma en memoria y se escribe
 *     de una vez + fsync. El audio y las caratulas no se tocan.
 *   - si no: copia a un temporal en la misma carpeta con un bufer de 64 KB, deja HT_PAD bytes
 *     de relleno (las caratulas van delante y el texto al final, junto al relleno, para que las
 *     proximas ediciones sean en sitio), fsync, comprueba el tamano y rename atomico + fsync
 *     de la carpeta. Si algo falla se borra el temporal y el original queda intacto.
 *
 * CLI (compilar con -DHIBYTAG_MAIN):
 *   hibytag get FICHERO...
 *   hibytag set FICHERO campo=valor...          (campo= vacio borra el campo)
 *   hibytag batch LISTA campo=valor...          (LISTA: una ruta por linea, "-" = stdin)
 *   hibytag suggest CAMPO PREFIJO [N]
 *   hibytag dbupdate FICHERO...
 *   hibytag setcover IMG.jpg FICHERO...         (portada incrustada; una linea OK/ERR por fichero)
 *   hibytag getall FICHERO...                   (todas las etiquetas de texto, claves canonicas)
 *   hibytag setkv FICHERO CLAVE=valor...        (cualquier clave: COMPOSER=..., COMMENT=..., LYRICS=...)
 *   hibytag batchkv LISTA CLAVE=valor...
 *   hibytag rename VIEJO NUEVO                  (cancion + su .lrc; actualiza las rutas de la base)
 *   hibytag renamedir VIEJA NUEVA [rmparent]    (carpeta entera; actualiza las rutas de la base)
 *   hibytag dbrename VIEJO NUEVO [VIEJO NUEVO]... (solo la base: rutas ya renombradas)
 * Variables: HIBYTAG_DB (base), HIBYTAG_SD (raiz de la SD = "a:"), HIBYTAG_NODB=1 (no tocar
 * la base al escribir), HIBYTAG_FAILAT=N (pruebas: fallo simulado tras N bytes).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "hibytag.h"

#define HT_BUF (64 * 1024)          /* bufer de copia */
#define HT_WBUF (16 * 1024)         /* bufer de escritura para trozos pequenos */
#define HT_PAD 8192                 /* relleno nuevo tras una reescritura */
#define HT_INPLACE_MAX (128 * 1024) /* zona maxima reescrita en sitio */
#define HT_MAXVAL 16384             /* tope de un valor leido */
#define HTX_MAXBLK 256              /* bloques FLAC maximos */
#define HTX_BIGFRAME 4096           /* tramas ID3 "grandes" (caratulas...) van delante */

uint64_t ht_test_fail_at;

/* ------------------------------------------------------------------ utilidades */
static uint32_t htx_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t htx_be24(const unsigned char *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static uint32_t htx_le32(const unsigned char *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint32_t htx_ss32(const unsigned char *p) { return (uint32_t)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f); }
static void htx_put_be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void htx_put_le32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void htx_put_ss32(unsigned char *p, uint32_t v) { p[0] = (v >> 21) & 0x7f; p[1] = (v >> 14) & 0x7f; p[2] = (v >> 7) & 0x7f; p[3] = v & 0x7f; }

static int htx_pread(int fd, void *b, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)b + got, n - got, (off_t)(off + got));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static int htx_err_errno(void) { return errno == ENOSPC || errno == EDQUOT ? HT_ENOSPC : HT_EIO; }
static int htx_write_all(int fd, const void *b, size_t n)
{
    while (n) {
        ssize_t r = write(fd, b, n);
        if (r < 0) { if (errno == EINTR) continue; return htx_err_errno(); }
        b = (const char *)b + r; n -= (size_t)r;
    }
    return 0;
}
static int htx_pwrite_all(int fd, const void *b, size_t n, uint64_t off)
{
    while (n) {
        ssize_t r = pwrite(fd, b, n, (off_t)off);
        if (r < 0) { if (errno == EINTR) continue; return htx_err_errno(); }
        b = (const char *)b + r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}

/* cadena dinamica */
typedef struct { char *p; size_t n, cap; int oom; } htx_str;
static void hs_add(htx_str *s, const void *d, size_t n)
{
    if (s->oom) return;
    if (s->n + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 64;
        while (c < s->n + n + 1) c *= 2;
        char *q = realloc(s->p, c);
        if (!q) { s->oom = 1; return; }
        s->p = q; s->cap = c;
    }
    memcpy(s->p + s->n, d, n); s->n += n; s->p[s->n] = 0;
}
static void hs_byte(htx_str *s, int c) { unsigned char b = (unsigned char)c; hs_add(s, &b, 1); }
static void hs_cp(htx_str *s, uint32_t c)
{
    unsigned char b[4]; int n;
    if (c < 0x80) { b[0] = c; n = 1; }
    else if (c < 0x800) { b[0] = 0xC0 | c >> 6; b[1] = 0x80 | (c & 0x3F); n = 2; }
    else if (c < 0x10000) { b[0] = 0xE0 | c >> 12; b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); n = 3; }
    else { b[0] = 0xF0 | c >> 18; b[1] = 0x80 | ((c >> 12) & 0x3F); b[2] = 0x80 | ((c >> 6) & 0x3F); b[3] = 0x80 | (c & 0x3F); n = 4; }
    hs_add(s, b, n);
}
/* siguiente punto de codigo UTF-8 (invalido -> U+FFFD, avanza 1 byte) */
static uint32_t htx_u8(const unsigned char **pp, const unsigned char *e)
{
    const unsigned char *p = *pp;
    uint32_t c = *p++; int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *pp = p; return 0xFFFD; }
    for (int i = 0; i < n; i++) {
        if (p >= e || (*p & 0xC0) != 0x80) { *pp = p; return 0xFFFD; }
        c = c << 6 | (*p++ & 0x3F);
    }
    *pp = p;
    return c;
}
/* corta s a como mucho n bytes sin partir un caracter UTF-8 */
static void htx_u8_trunc(char *s, size_t n)
{
    size_t l = strlen(s);
    if (l <= n) return;
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = 0;
}
/* numero del principio ("03/12" -> 3); 0 si no hay */
static int htx_lead_int0(const char *s)
{
    int n = 0, k = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9' && k < 9) { n = n * 10 + (*s++ - '0'); k++; }
    return n;
}

static const char *const htx_names[HT_NFIELDS] = { "title", "artist", "album", "albumartist", "genre", "year", "track", "disc" };
HT_API const char *ht_field_name(int f) { return f >= 0 && f < HT_NFIELDS ? htx_names[f] : "?"; }
HT_API int ht_field_parse(const char *n)
{
    static const struct { const char *a; int f; } al[] = {
        { "name", HT_TITLE }, { "album_artist", HT_ALBUMARTIST }, { "album artist", HT_ALBUMARTIST },
        { "aa", HT_ALBUMARTIST }, { "date", HT_YEAR }, { "tracknumber", HT_TRACK }, { "trackno", HT_TRACK },
        { "discnumber", HT_DISC }, { "disk", HT_DISC } };
    for (int i = 0; i < HT_NFIELDS; i++) if (!strcasecmp(n, htx_names[i])) return i;
    for (size_t i = 0; i < sizeof al / sizeof *al; i++) if (!strcasecmp(n, al[i].a)) return al[i].f;
    return -1;
}
HT_API const char *ht_strerror(int e)
{
    switch (e) {
    case HT_OK: return "ok";
    case HT_EIO: return "error de lectura/escritura";
    case HT_EFORMAT: return "formato no reconocido o fichero danado";
    case HT_EUNSUP: return "no soportado para este fichero";
    case HT_ENOSPC: return "no hay espacio libre";
    case HT_ECANCEL: return "cancelado";
    case HT_ENOMEM: return "sin memoria";
    case HT_EOPEN: return "no se pudo abrir";
    case HT_EDB: return "error de la base";
    case HT_ETOOBIG: return "imagen demasiado grande (max. 1 MB)";
    case HT_EIMAGE: return "la imagen no es un JPEG valido";
    case HT_EKEY: return "nombre de campo no valido";
    case HT_EEXIST: return "ya existe un fichero con ese nombre";
    case HT_ENAME: return "nombre de fichero no valido";
    }
    return "error";
}
HT_API const char *ht_fmt_name(int f) { return f == HT_FMT_FLAC ? "flac" : f == HT_FMT_MP3 ? "mp3" : f == HT_FMT_M4A ? "m4a" : "?"; }
HT_API void ht_tags_free(ht_tags *t)
{
    for (int i = 0; i < HT_NFIELDS; i++) { free(t->v[i]); t->v[i] = 0; }
}
/* anade un valor leido: titulos/artistas multiples se unen con "; "; ano/pista/disco: el primero */
static void htx_tag_add(ht_tags *t, int f, const char *v, size_t n)
{
    if (f < 0 || !n) return;
    if (t->v[f] && (f == HT_YEAR || f == HT_TRACK || f == HT_DISC)) return;
    size_t old = t->v[f] ? strlen(t->v[f]) : 0;
    if (old >= HT_MAXVAL) return;
    char *q = realloc(t->v[f], old + n + 3);
    if (!q) return;
    if (old) { memcpy(q + old, "; ", 2); old += 2; }
    memcpy(q + old, v, n); q[old + n] = 0;
    htx_u8_trunc(q, HT_MAXVAL);
    t->v[f] = q;
}

/* ------------------------------------------------------------------ claves canonicas */
#define HTX_KEYMAX 128                 /* clave mas larga que se reconoce (mas larga: se conserva tal cual) */
#define HTX_MAXED 64                   /* cambios por escritura */
static const char *const htx_base_keys[HT_NFIELDS] = { "TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "GENRE", "DATE", "TRACKNUMBER", "DISCNUMBER" };
/* alias -> canonica (comparando en mayusculas) */
static const char *const htx_alias[][2] = {
    { "ALBUM ARTIST", "ALBUMARTIST" }, { "ALBUM_ARTIST", "ALBUMARTIST" }, { "YEAR", "DATE" },
    { "TOTALTRACKS", "TRACKTOTAL" }, { "TOTALDISCS", "DISCTOTAL" }, { "UNSYNCEDLYRICS", "LYRICS" },
    { "UNSYNCED LYRICS", "LYRICS" },
    /* descripciones TXXX de Picard <-> claves Vorbis (la primera forma es la que se escribe en MP3) */
    { "MUSICBRAINZ ALBUM ID", "MUSICBRAINZ_ALBUMID" }, { "MUSICBRAINZ ARTIST ID", "MUSICBRAINZ_ARTISTID" },
    { "MUSICBRAINZ ALBUM ARTIST ID", "MUSICBRAINZ_ALBUMARTISTID" }, { "MUSICBRAINZ RELEASE GROUP ID", "MUSICBRAINZ_RELEASEGROUPID" },
    { "MUSICBRAINZ RELEASE TRACK ID", "MUSICBRAINZ_RELEASETRACKID" }, { "MUSICBRAINZ WORK ID", "MUSICBRAINZ_WORKID" },
    { "MUSICBRAINZ ALBUM TYPE", "RELEASETYPE" }, { "MUSICBRAINZ ALBUM STATUS", "RELEASESTATUS" },
    { "MUSICBRAINZ ALBUM RELEASE COUNTRY", "RELEASECOUNTRY" }, { "ACOUSTID ID", "ACOUSTID_ID" } };
#define HTX_NALIAS ((int)(sizeof htx_alias / sizeof *htx_alias))

HT_API void ht_key_canon(const char *key, char *out, size_t outsz)
{
    if (!outsz) return;
    while (*key == ' ') key++;
    size_t n = 0;
    for (; key[n] && n + 1 < outsz; n++) out[n] = key[n] >= 'a' && key[n] <= 'z' ? key[n] - 32 : key[n];
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    for (int i = 0; i < HTX_NALIAS; i++) if (!strcmp(out, htx_alias[i][0])) { snprintf(out, outsz, "%s", htx_alias[i][1]); return; }
}
HT_API int ht_key_valid(const char *k)
{
    size_t n = strlen(k);
    if (!n || n >= HTX_KEYMAX || k[0] == ' ' || k[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)k[i] < 0x20 || (unsigned char)k[i] > 0x7D || k[i] == '=') return 0;
    return 1;
}
HT_API int ht_key_base(const char *canon)
{
    for (int i = 0; i < HT_NFIELDS; i++) if (!strcmp(canon, htx_base_keys[i])) return i;
    return -1;
}
/* claves que no se muestran (imagenes en base64) */
static int htx_key_hidden(const char *c)
{
    return !strcmp(c, "METADATA_BLOCK_PICTURE") || !strcmp(c, "COVERART") || !strcmp(c, "COVERARTMIME");
}
static int htx_key_ro(const char *c)
{
    return !strncmp(c, "REPLAYGAIN_", 11) || !strncmp(c, "COMMENT:ITUN", 12) || !strcmp(c, "ITUNNORM") || !strcmp(c, "ITUNSMPB");
}

/* cambios normalizados: clave canonica + valor ("" = borrar). La ultima repeticion gana. */
typedef struct { char key[HTX_KEYMAX]; const char *val; } htx_ed;
typedef struct { int n; htx_ed e[HTX_MAXED]; } htx_eds;
static int htx_eds_find(const htx_eds *s, const char *canon)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->e[i].key, canon)) return i;
    return -1;
}
static int htx_eds_build(htx_eds *s, const ht_kv *kv, int n)
{
    s->n = 0;
    for (int i = 0; i < n; i++) {
        if (!kv[i].val) continue;
        if (!kv[i].key || !ht_key_valid(kv[i].key)) return HT_EKEY;
        char c[HTX_KEYMAX];
        ht_key_canon(kv[i].key, c, sizeof c);
        if (htx_key_hidden(c)) return HT_EKEY;               /* imagenes: con ht_set_cover */
        int j = htx_eds_find(s, c);
        if (j < 0) { if (s->n >= HTX_MAXED) return HT_EUNSUP; j = s->n++; snprintf(s->e[j].key, HTX_KEYMAX, "%s", c); }
        s->e[j].val = kv[i].val;
    }
    return HT_OK;
}
/* los cambios de las 8 claves de siempre como ht_changes (para ID3v1) */
static void htx_eds_changes(const htx_eds *s, ht_changes *ch)
{
    memset(ch, 0, sizeof *ch);
    for (int i = 0; i < s->n; i++) { int f = ht_key_base(s->e[i].key); if (f >= 0) ch->v[f] = s->e[i].val; }
}

/* ---- ht_all */
HT_API void ht_all_free(ht_all *a)
{
    for (int i = 0; i < a->n; i++) { free(a->it[i].key); free(a->it[i].orig); free(a->it[i].val); }
    free(a->it);
    a->it = 0; a->n = a->cap = 0;
}
HT_API const ht_item *ht_all_find(const ht_all *a, const char *key)
{
    char c[HTX_KEYMAX];
    ht_key_canon(key, c, sizeof c);
    for (int i = 0; i < a->n; i++) if (!strcmp(a->it[i].key, c)) return &a->it[i];
    return 0;
}
/* anade key=val (n bytes); la misma clave canonica otra vez se une con "; " (HT_ITEM_MULTI) */
static void htx_all_add(ht_all *a, const char *key, const char *val, size_t n, int flags)
{
    char c[HTX_KEYMAX];
    if (!*key || strlen(key) >= HTX_KEYMAX) return;
    ht_key_canon(key, c, sizeof c);
    if (!*c || htx_key_hidden(c)) return;
    while (n && (val[n - 1] == 0)) n--;
    if (htx_key_ro(c)) flags |= HT_ITEM_RO;
    for (int i = 0; i < a->n; i++) {
        ht_item *it = &a->it[i];
        if (strcmp(it->key, c)) continue;
        if (!n) return;
        size_t old = strlen(it->val);
        if (!old) { char *q = malloc(n + 1); if (!q) return; memcpy(q, val, n); q[n] = 0; htx_u8_trunc(q, HT_LONGMAX); free(it->val); it->val = q; it->flags |= flags; return; }
        if (old + n + 2 > HT_LONGMAX) { it->flags |= HT_ITEM_TRUNC | HT_ITEM_MULTI; return; }
        char *q = realloc(it->val, old + n + 3);
        if (!q) return;
        memcpy(q + old, "; ", 2); memcpy(q + old + 2, val, n); q[old + 2 + n] = 0;
        it->val = q; it->flags |= flags | HT_ITEM_MULTI;
        return;
    }
    if (a->n >= a->cap) {
        int nc = a->cap ? a->cap * 2 : 32;
        ht_item *q = realloc(a->it, nc * sizeof *q);
        if (!q) return;
        a->it = q; a->cap = nc;
    }
    ht_item *it = &a->it[a->n];
    it->key = strdup(c); it->orig = strdup(key); it->val = malloc(n + 1); it->flags = flags;
    if (!it->key || !it->orig || !it->val) { free(it->key); free(it->orig); free(it->val); return; }
    memcpy(it->val, val, n); it->val[n] = 0;
    if (strlen(it->val) > HT_LONGMAX) { htx_u8_trunc(it->val, HT_LONGMAX); it->flags |= HT_ITEM_TRUNC; }
    a->n++;
}

/* generos ID3v1 (0..191) */
static const char *const htx_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alt. Rock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta Rap", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret",
    "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz",
    "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing",
    "Fast-Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
    "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House",
    "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie", "BritPop", "Afro-Punk",
    "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop",
    "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM",
    "Eclectic", "Electro", "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient",
    "Industro-Goth", "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
    "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical",
    "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
    "Garage Rock", "Psybient" };
#define HTX_NGENRES ((int)(sizeof htx_genres / sizeof *htx_genres))

/* "(17)", "17", "(17)Rock" -> nombre del genero (nueva cadena) o NULL si no aplica */
static char *htx_genre_fix(const char *s)
{
    const char *p = s; int paren = 0, n = 0, dig = 0;
    if (*p == '(') { paren = 1; p++; }
    if (paren && (!strncmp(p, "RX)", 3) || !strncmp(p, "CR)", 3))) {
        if (p[3]) return strdup(p + 3);
        return strdup(p[0] == 'R' ? "Remix" : "Cover");
    }
    while (*p >= '0' && *p <= '9' && dig < 4) { n = n * 10 + (*p++ - '0'); dig++; }
    if (!dig) return 0;
    if (paren) { if (*p != ')') return 0; p++; if (*p) return strdup(p); }
    else if (*p) return 0;
    return n < HTX_NGENRES ? strdup(htx_genres[n]) : 0;
}

/* ------------------------------------------------------------------ sumidero de escritura
 * Destino: un fichero (fd >= 0, con bufer) o memoria limitada (fd < 0). El origen se copia
 * a trozos de 64 KB; el progreso se avisa por trozo. */
typedef struct {
    int fd, src;
    unsigned char *mem; size_t mlen, mcap;
    unsigned char *wb; size_t wlen;
    unsigned char *cb;
    uint64_t pos, total;
    ht_progress_fn prog; void *ud;
    int err;
} htx_sink;

static void sk_check(htx_sink *s)
{
    if (s->err || s->fd < 0) return;
    if (ht_test_fail_at && s->pos >= ht_test_fail_at) { s->err = HT_EIO; return; }
    if (s->prog && s->prog(s->ud, s->pos, s->total)) s->err = HT_ECANCEL;
}
static void sk_flush(htx_sink *s)
{
    if (s->err || s->fd < 0 || !s->wlen) return;
    int e = htx_write_all(s->fd, s->wb, s->wlen);
    s->wlen = 0;
    if (e) s->err = e;
}
static void sk_put(htx_sink *s, const void *p, size_t n)
{
    if (s->err || !n) return;
    if (s->fd < 0) {
        if (s->mlen + n > s->mcap) { s->err = HT_ENOMEM; return; }
        memcpy(s->mem + s->mlen, p, n); s->mlen += n; s->pos += n;
        return;
    }
    if (s->wlen + n > HT_WBUF) {
        sk_flush(s);
        if (s->err) return;
        if (n > HT_WBUF) { int e = htx_write_all(s->fd, p, n); if (e) s->err = e; s->pos += n; sk_check(s); return; }
    }
    memcpy(s->wb + s->wlen, p, n); s->wlen += n; s->pos += n;
}
static void sk_zero(htx_sink *s, uint64_t n)
{
    if (s->fd < 0) {
        if (s->err) return;
        if (s->mlen + n > s->mcap) { s->err = HT_ENOMEM; return; }
        memset(s->mem + s->mlen, 0, (size_t)n); s->mlen += (size_t)n; s->pos += n;
        return;
    }
    sk_flush(s);
    while (n && !s->err) {
        size_t k = n > HT_BUF ? HT_BUF : (size_t)n;
        memset(s->cb, 0, k);
        int e = htx_write_all(s->fd, s->cb, k);
        if (e) { s->err = e; break; }
        s->pos += k; n -= k;
        sk_check(s);
    }
}
static void sk_copy(htx_sink *s, uint64_t off, uint64_t n)
{
    if (s->fd >= 0) sk_flush(s);
    while (n && !s->err) {
        size_t k = n > HT_BUF ? HT_BUF : (size_t)n;
        if (s->fd < 0) {
            if (s->mlen + k > s->mcap) { s->err = HT_ENOMEM; return; }
            if (htx_pread(s->src, s->mem + s->mlen, k, off)) { s->err = HT_EIO; return; }
            s->mlen += k; s->pos += k;
        } else {
            if (htx_pread(s->src, s->cb, k, off)) { s->err = HT_EIO; return; }
            int e = htx_write_all(s->fd, s->cb, k);
            if (e) { s->err = e; return; }
            s->pos += k;
            sk_check(s);
        }
        off += k; n -= k;
    }
}

/* sumidero en memoria para una zona en sitio */
static int htx_memsink(htx_sink *s, int src, size_t cap)
{
    memset(s, 0, sizeof *s);
    s->fd = -1; s->src = src; s->mcap = cap;
    s->mem = malloc(cap ? cap : 1);
    return s->mem ? 0 : HT_ENOMEM;
}

/* escribe una zona ya armada en memoria sobre el fichero original + fsync */
static int htx_inplace_commit(int fd, const unsigned char *m, size_t n, uint64_t off)
{
    int e = htx_pwrite_all(fd, m, n, off);
    if (e) return e;
    if (fsync(fd)) return HT_EIO;
    return 0;
}

typedef void (*htx_emit_fn)(htx_sink *s, void *ctx);

/* nombre del temporal: misma carpeta, nombre fijo por fichero (un temporal huerfano de un
 * corte se sobrescribe en el siguiente intento) */
static void htx_tmpname(const char *path, char *out, size_t outsz)
{
    const char *b = strrchr(path, '/');
    size_t dl = b ? (size_t)(b - path + 1) : 0;
    uint32_t h = 2166136261u;
    for (const char *p = b ? b + 1 : path; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    snprintf(out, outsz, "%.*s.hibytag-%08x.tmp", (int)dl, path, h);
}

/* reescritura completa: temporal + fsync + comprobacion + rename atomico */
static int htx_rewrite(const char *path, int src, const struct stat *st0, uint64_t newsize,
                       htx_emit_fn emit, void *ctx, ht_progress_fn cb, void *ud)
{
    char tmp[PATH_MAX + 32], dir[PATH_MAX];
    if (strlen(path) >= PATH_MAX) return HT_EOPEN;
    htx_tmpname(path, tmp, sizeof tmp);
    const char *sl = strrchr(path, '/');
    if (sl) snprintf(dir, sizeof dir, "%.*s", (int)(sl - path) ? (int)(sl - path) : 1, path); else strcpy(dir, ".");
    struct statvfs vf;
    if (!statvfs(dir, &vf) && (uint64_t)vf.f_bavail * vf.f_frsize < newsize + 256 * 1024) return HT_ENOSPC;

    htx_sink s; memset(&s, 0, sizeof s);
    s.src = src; s.total = newsize; s.prog = cb; s.ud = ud;
    s.wb = malloc(HT_WBUF); s.cb = malloc(HT_BUF);
    if (!s.wb || !s.cb) { free(s.wb); free(s.cb); return HT_ENOMEM; }
    s.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, (st0->st_mode & 0777) | 0600);
    if (s.fd < 0) { free(s.wb); free(s.cb); return errno == ENOSPC ? HT_ENOSPC : HT_EOPEN; }
    if (cb && cb(ud, 0, newsize)) s.err = HT_ECANCEL;
    emit(&s, ctx);
    sk_flush(&s);
    if (!s.err && s.pos != newsize) s.err = HT_EFORMAT;            /* nunca deberia pasar */
    if (!s.err && fsync(s.fd)) s.err = HT_EIO;
    if (close(s.fd) && !s.err) s.err = HT_EIO;
    free(s.wb); free(s.cb);
    /* el original no debe haber cambiado mientras se copiaba */
    struct stat st1;
    if (!s.err && (fstat(src, &st1) || st1.st_size != st0->st_size || st1.st_mtime != st0->st_mtime)) s.err = HT_EIO;
    if (!s.err) { struct stat st2; if (stat(tmp, &st2) || (uint64_t)st2.st_size != newsize) s.err = HT_EIO; }
    if (s.err) { unlink(tmp); return s.err; }
    if (rename(tmp, path)) { int e = htx_err_errno(); unlink(tmp); return e; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    if (cb) cb(ud, newsize, newsize);
    return HT_OK;
}

/* ================================================================== FLAC */
typedef struct { uint8_t type; uint32_t len; uint64_t off; } htx_blk;   /* off = cabecera del bloque */
typedef struct { uint64_t base, audio; int nb; htx_blk b[HTX_MAXBLK]; } htx_flac;
typedef struct { uint64_t off; uint32_t len; int field; char k[HTX_KEYMAX]; } htx_cmt;   /* off = datos; k = clave ("" si no se reconoce) */
typedef struct { uint64_t vend_off; uint32_t vend_len; int n, cap; htx_cmt *c; } htx_vc;

static int htx_flac_parse(int fd, uint64_t base, uint64_t fsize, htx_flac *f)
{
    unsigned char h[4];
    memset(f, 0, sizeof *f);
    f->base = base;
    if (htx_pread(fd, h, 4, base) || memcmp(h, "fLaC", 4)) return HT_EFORMAT;
    uint64_t off = base + 4;
    for (;;) {
        if (f->nb >= HTX_MAXBLK) return HT_EUNSUP;
        if (htx_pread(fd, h, 4, off)) return HT_EFORMAT;
        htx_blk *b = &f->b[f->nb++];
        b->type = h[0] & 0x7f; b->len = htx_be24(h + 1); b->off = off;
        if (b->type == 127 || off + 4 + b->len > fsize) return HT_EFORMAT;
        off += 4 + (uint64_t)b->len;
        if (h[0] & 0x80) break;
    }
    if (f->b[0].type != 0) return HT_EFORMAT;
    f->audio = off;
    return HT_OK;
}

/* clave de un comentario (primeros bytes k) -> out ("" si no hay '=' a tiempo); devuelve el campo (o -1) */
static int htx_vc_key(const unsigned char *k, size_t n, char *out)
{
    size_t e = 0;
    out[0] = 0;
    while (e < n && k[e] != '=') e++;
    if (e == n || !e || e >= HTX_KEYMAX) return -1;
    memcpy(out, k, e); out[e] = 0;
    char c[HTX_KEYMAX];
    ht_key_canon(out, c, sizeof c);
    return ht_key_base(c);
}

static int htx_vc_parse(int fd, const htx_blk *b, htx_vc *vc)
{
    unsigned char h[HTX_KEYMAX + 4];
    memset(vc, 0, sizeof *vc);
    uint64_t p = b->off + 4, end = p + b->len;
    if (p + 4 > end || htx_pread(fd, h, 4, p)) return HT_EFORMAT;
    vc->vend_len = htx_le32(h); vc->vend_off = p + 4;
    p += 4 + (uint64_t)vc->vend_len;
    if (p + 4 > end || htx_pread(fd, h, 4, p)) return HT_EFORMAT;
    uint32_t cnt = htx_le32(h);
    p += 4;
    for (uint32_t i = 0; i < cnt; i++) {
        if (p + 4 > end || htx_pread(fd, h, 4, p)) return HT_EFORMAT;
        uint32_t len = htx_le32(h);
        if (p + 4 + len > end) return HT_EFORMAT;
        size_t k = len < sizeof h ? len : sizeof h;
        if (htx_pread(fd, h, k, p + 4)) return HT_EFORMAT;
        if (vc->n >= vc->cap) {
            int nc = vc->cap ? vc->cap * 2 : 32;
            htx_cmt *q = realloc(vc->c, nc * sizeof *q);
            if (!q) return HT_ENOMEM;
            vc->c = q; vc->cap = nc;
        }
        vc->c[vc->n].off = p + 4; vc->c[vc->n].len = len; vc->c[vc->n].field = htx_vc_key(h, k, vc->c[vc->n].k);
        vc->n++;
        p += 4 + (uint64_t)len;
    }
    return HT_OK;
}

static int htx_flac_find_vc(const htx_flac *f)
{
    for (int i = 0; i < f->nb; i++) if (f->b[i].type == 4) return i;
    return -1;
}

static int htx_flac_read(int fd, uint64_t base, uint64_t fsize, ht_tags *t)
{
    htx_flac *f = malloc(sizeof *f);
    if (!f) return HT_ENOMEM;
    int e = htx_flac_parse(fd, base, fsize, f);
    int iv = e ? -1 : htx_flac_find_vc(f);
    if (iv >= 0) {
        htx_vc vc;
        e = htx_vc_parse(fd, &f->b[iv], &vc);
        char *buf = malloc(HT_MAXVAL + 48);
        if (!buf) e = HT_ENOMEM;
        for (int i = 0; !e && i < vc.n; i++) {
            if (vc.c[i].field < 0) continue;
            size_t n = vc.c[i].len > HT_MAXVAL + 40 ? HT_MAXVAL + 40 : vc.c[i].len;
            if (htx_pread(fd, buf, n, vc.c[i].off)) { e = HT_EIO; break; }
            char *eq = memchr(buf, '=', n);
            if (eq) htx_tag_add(t, vc.c[i].field, eq + 1, n - (size_t)(eq + 1 - buf));
        }
        free(buf); free(vc.c);
    }
    free(f);
    return e;
}

static int htx_flac_read_all(int fd, uint64_t base, uint64_t fsize, ht_all *a)
{
    htx_flac *f = malloc(sizeof *f);
    if (!f) return HT_ENOMEM;
    int e = htx_flac_parse(fd, base, fsize, f);
    int iv = e ? -1 : htx_flac_find_vc(f);
    if (iv >= 0) {
        htx_vc vc;
        e = htx_vc_parse(fd, &f->b[iv], &vc);
        size_t cap = HT_LONGMAX + HTX_KEYMAX + 8;
        char *buf = e ? 0 : malloc(cap);
        if (!e && !buf) e = HT_ENOMEM;
        for (int i = 0; !e && i < vc.n; i++) {
            if (!vc.c[i].k[0]) continue;
            size_t kl = strlen(vc.c[i].k), n = vc.c[i].len > cap ? cap : vc.c[i].len;
            if (htx_pread(fd, buf, n, vc.c[i].off)) { e = HT_EIO; break; }
            htx_all_add(a, vc.c[i].k, buf + kl + 1, n - kl - 1, vc.c[i].len > cap ? HT_ITEM_TRUNC : 0);
        }
        free(buf); free(vc.c);
    }
    free(f);
    return e;
}

/* datos de una escritura FLAC: ok[i] = clave escrita para el cambio i (forma del fichero) */
typedef struct {
    int fd; uint64_t fsize;
    htx_flac *f; int iv; htx_vc vc; const unsigned char *keep;
    const htx_eds *ed; char (*ok)[HTX_KEYMAX]; uint32_t vclen;
} htx_fw;

static void htx_flac_hdr(htx_sink *s, int type, int last, uint32_t len)
{
    unsigned char h[4] = { (unsigned char)(type | (last ? 0x80 : 0)), (unsigned char)(len >> 16), (unsigned char)(len >> 8), (unsigned char)len };
    sk_put(s, h, 4);
}
static void htx_flac_emit_vc(htx_sink *s, htx_fw *w, int last)
{
    unsigned char b[4];
    htx_flac_hdr(s, 4, last, w->vclen);
    uint32_t cnt = 0;
    for (int i = 0; i < w->vc.n; i++) cnt += w->keep[i];
    for (int k = 0; k < w->ed->n; k++) if (*w->ed->e[k].val) cnt++;
    if (w->iv >= 0) { htx_put_le32(b, w->vc.vend_len); sk_put(s, b, 4); sk_copy(s, w->vc.vend_off, w->vc.vend_len); }
    else { htx_put_le32(b, 7); sk_put(s, b, 4); sk_put(s, "hibytag", 7); }
    htx_put_le32(b, cnt); sk_put(s, b, 4);
    for (int i = 0; i < w->vc.n; i++) {
        if (!w->keep[i]) continue;
        htx_put_le32(b, w->vc.c[i].len); sk_put(s, b, 4);
        sk_copy(s, w->vc.c[i].off, w->vc.c[i].len);
    }
    for (int k = 0; k < w->ed->n; k++) {
        const char *v = w->ed->e[k].val;
        if (!*v) continue;
        size_t kl = strlen(w->ok[k]), vl = strlen(v);
        htx_put_le32(b, (uint32_t)(kl + 1 + vl)); sk_put(s, b, 4);
        sk_put(s, w->ok[k], kl); sk_put(s, "=", 1); sk_put(s, v, vl);
    }
}
/* reescritura completa: STREAMINFO y demas bloques (caratulas...) en su orden, luego el
 * comentario y al final el relleno nuevo (asi la proxima edicion cabe en sitio) */
static void htx_flac_emit_all(htx_sink *s, void *ctx)
{
    htx_fw *w = ctx;
    htx_flac *f = w->f;
    sk_copy(s, 0, f->base + 4);
    for (int j = 0; j < f->nb; j++) {
        if (j == w->iv || f->b[j].type == 1) continue;
        htx_flac_hdr(s, f->b[j].type, 0, f->b[j].len);
        sk_copy(s, f->b[j].off + 4, f->b[j].len);
    }
    htx_flac_emit_vc(s, w, 0);
    htx_flac_hdr(s, 1, 1, HT_PAD);
    sk_zero(s, HT_PAD);
    sk_copy(s, f->audio, w->fsize - f->audio);
}

/* en sitio: la zona [lo..hi] (comentario + un PADDING y lo que haya entre medias, si es poco)
 * se rearma en memoria. 1 = hecho, 0 = no cabe, <0 = error */
static int htx_flac_inplace(htx_fw *w)
{
    htx_flac *f = w->f;
    int cand[HTX_MAXBLK + 1], nc = 0;
    for (int j = 0; j < f->nb; j++) if (f->b[j].type == 1) cand[nc++] = j;
    if (w->iv >= 0) cand[nc++] = -1;
    for (int k = 0; k < nc; k++) {
        int ip = cand[k], lo, hi;
        if (w->iv < 0) lo = hi = ip;
        else if (ip < 0) lo = hi = w->iv;
        else { lo = ip < w->iv ? ip : w->iv; hi = ip < w->iv ? w->iv : ip; }
        uint64_t avail = 0, others = 0, start = f->b[lo].off, dirty = start;
        for (int j = lo; j <= hi; j++) {
            const htx_blk *b = &f->b[j];
            avail += 4 + (uint64_t)b->len;
            if (j != w->iv && b->type != 1) others += 4 + (uint64_t)b->len;
            uint64_t e = b->type == 1 ? b->off + 4 : b->off + 4 + b->len;
            if (e > dirty) dirty = e;
        }
        uint64_t need = others + 4 + (uint64_t)w->vclen;
        if (need > avail) continue;
        uint64_t rem = avail - need;
        if (rem && (rem < 4 || rem - 4 > 0xFFFFFF)) continue;
        uint64_t wlen = need + (rem ? 4 : 0);
        if (dirty - start > wlen) wlen = dirty - start;
        if (wlen > HT_INPLACE_MAX) continue;
        htx_sink s;
        if (htx_memsink(&s, w->fd, (size_t)wlen)) return HT_ENOMEM;
        size_t lasth = 0;
        for (int j = lo; j <= hi; j++) {
            const htx_blk *b = &f->b[j];
            if (j == w->iv || b->type == 1) continue;
            lasth = s.mlen;
            htx_flac_hdr(&s, b->type, 0, b->len);
            sk_copy(&s, b->off + 4, b->len);
        }
        lasth = s.mlen;
        htx_flac_emit_vc(&s, w, 0);
        if (rem) { lasth = s.mlen; htx_flac_hdr(&s, 1, 0, (uint32_t)(rem - 4)); }
        if (s.mlen < wlen) sk_zero(&s, wlen - s.mlen);
        if (!s.err && hi == f->nb - 1) s.mem[lasth] |= 0x80;
        int e = s.err ? s.err : htx_inplace_commit(w->fd, s.mem, s.mlen, start);
        free(s.mem);
        return e ? e : 1;
    }
    /* cambio de sitio: el comentario y el PADDING estan separados por bloques grandes
     * (caratulas). El comentario nuevo va dentro del PADDING y el viejo se convierte en
     * PADDING. Primero se escribe el nuevo: si se corta entre medias el FLAC sigue siendo
     * valido (con dos comentarios). */
    if (w->iv < 0) return 0;
    const htx_blk *ov = &f->b[w->iv];
    if (4 + (uint64_t)ov->len > HT_INPLACE_MAX) return 0;
    for (int k = 0; k < nc; k++) {
        int ip = cand[k];
        if (ip < 0) continue;
        const htx_blk *pb = &f->b[ip];
        uint64_t avail = 4 + (uint64_t)pb->len, need = 4 + (uint64_t)w->vclen;
        if (need > avail) continue;
        uint64_t rem = avail - need;
        if (rem && rem < 4) continue;
        uint64_t wlen = need + (rem ? 4 : 0);
        if (wlen > HT_INPLACE_MAX) continue;
        htx_sink s;
        if (htx_memsink(&s, w->fd, (size_t)(wlen > 4 + ov->len ? wlen : 4 + ov->len))) return HT_ENOMEM;
        int lastp = ip == f->nb - 1;
        htx_flac_emit_vc(&s, w, lastp && !rem);
        if (rem) htx_flac_hdr(&s, 1, lastp, (uint32_t)(rem - 4));
        int e = s.err ? s.err : htx_pwrite_all(w->fd, s.mem, s.mlen, pb->off);
        if (!e && fsync(w->fd)) e = HT_EIO;
        if (!e) {
            s.mlen = 0; s.pos = 0;
            htx_flac_hdr(&s, 1, w->iv == f->nb - 1, ov->len);
            sk_zero(&s, ov->len);
            e = s.err ? s.err : htx_inplace_commit(w->fd, s.mem, s.mlen, ov->off);
        }
        free(s.mem);
        return e ? e : 1;
    }
    return 0;
}

static int htx_flac_write(const char *path, int fd, const struct stat *st, uint64_t base, const htx_eds *ed,
                          ht_progress_fn cb, void *ud, int *mode)
{
    htx_fw w; memset(&w, 0, sizeof w);
    w.fd = fd; w.fsize = (uint64_t)st->st_size; w.ed = ed;
    w.f = malloc(sizeof *w.f);
    w.ok = malloc(sizeof *w.ok * (ed->n + 1));
    if (!w.f || !w.ok) { free(w.f); free(w.ok); return HT_ENOMEM; }
    int e = htx_flac_parse(fd, base, w.fsize, w.f);
    unsigned char *keep = 0;
    if (e) goto out;
    w.iv = htx_flac_find_vc(w.f);
    if (w.iv >= 0 && (e = htx_vc_parse(fd, &w.f->b[w.iv], &w.vc))) goto out;
    keep = malloc(w.vc.n + 1);
    if (!keep) { e = HT_ENOMEM; goto out; }
    /* clave escrita: las 8 de siempre en su forma canonica; el resto como ya este en el fichero */
    for (int k = 0; k < ed->n; k++) snprintf(w.ok[k], HTX_KEYMAX, "%s", ed->e[k].key);
    uint64_t len = 4 + (w.iv >= 0 ? w.vc.vend_len : 7) + 4;
    for (int i = 0; i < w.vc.n; i++) {
        int k = -1;
        if (w.vc.c[i].k[0]) { char c[HTX_KEYMAX]; ht_key_canon(w.vc.c[i].k, c, sizeof c); k = htx_eds_find(ed, c); }
        keep[i] = k < 0;
        if (k >= 0 && ht_key_base(ed->e[k].key) < 0 && !strcmp(w.ok[k], ed->e[k].key)) snprintf(w.ok[k], HTX_KEYMAX, "%s", w.vc.c[i].k);
        if (keep[i]) len += 4 + (uint64_t)w.vc.c[i].len;
    }
    for (int k = 0; k < ed->n; k++)
        if (*ed->e[k].val) len += 4 + strlen(w.ok[k]) + 1 + strlen(ed->e[k].val);
    if (len > 0xFFFFFF) { e = HT_EUNSUP; goto out; }
    w.keep = keep; w.vclen = (uint32_t)len;
    int r = htx_flac_inplace(&w);
    if (r < 0) { e = r; goto out; }
    if (r == 1) { *mode = HT_MODE_INPLACE; if (cb) cb(ud, 1, 1); goto out; }
    uint64_t ns = w.f->base + 4;
    for (int j = 0; j < w.f->nb; j++) if (j != w.iv && w.f->b[j].type != 1) ns += 4 + (uint64_t)w.f->b[j].len;
    ns += 4 + (uint64_t)w.vclen + 4 + HT_PAD + (w.fsize - w.f->audio);
    e = htx_rewrite(path, fd, st, ns, htx_flac_emit_all, &w, cb, ud);
    if (!e) *mode = HT_MODE_REWRITE;
out:
    free(keep); free(w.vc.c); free(w.f); free(w.ok);
    return e;
}

/* ================================================================== ID3 */
typedef struct { uint64_t off; uint32_t size; int field; } htx_frm;   /* size: con cabecera */
typedef struct {
    int ver, flags;                 /* ver 0 = sin ID3v2 */
    uint32_t size;                  /* tamano declarado (sin cabecera) */
    uint64_t tag_end;               /* fin de la etiqueta en el fichero (con cabecera/pie) */
    uint64_t frames_off, frames_end;
    int nf, cap; htx_frm *f;
    int v1;                         /* hay ID3v1 al final */
} htx_id3;

/* lector: fichero o memoria (para etiquetas v2.3 con "unsynchronisation" global) */
typedef struct { int fd; const unsigned char *mem; uint64_t mlen; } htx_rd;
static int htx_rd_at(const htx_rd *r, uint64_t off, void *b, size_t n)
{
    if (r->mem) { if (off + n > r->mlen) return -1; memcpy(b, r->mem + off, n); return 0; }
    return htx_pread(r->fd, b, n, off);
}

static int htx_id3_idok(const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) if (!((b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9'))) return 0;
    return 1;
}
/* hay una trama valida (o relleno, o el final) en p? */
static int htx_id3_ok_at(const htx_rd *r, uint64_t p, uint64_t lim)
{
    unsigned char b[4];
    if (p == lim) return 1;
    if (p > lim) return 0;
    if (htx_rd_at(r, p, b, 1)) return 0;
    if (b[0] == 0) return 1;
    if (p + 10 > lim || htx_rd_at(r, p, b, 4)) return 0;
    return htx_id3_idok(b, 4);
}

static int htx_id3_field(const unsigned char *id, int ver)
{
    static const char *const v2[HT_NFIELDS] = { "TT2", "TP1", "TAL", "TP2", "TCO", "TYE", "TRK", "TPA" };
    static const char *const v3[HT_NFIELDS] = { "TIT2", "TPE1", "TALB", "TPE2", "TCON", "TYER", "TRCK", "TPOS" };
    if (ver == 2) { for (int i = 0; i < HT_NFIELDS; i++) if (!memcmp(id, v2[i], 3)) return i; return -1; }
    for (int i = 0; i < HT_NFIELDS; i++) if (!memcmp(id, v3[i], 4)) return i;
    if (!memcmp(id, "TDRC", 4)) return HT_YEAR;
    return -1;
}

static void htx_deunsync(unsigned char *d, size_t *n)
{
    size_t o = 0;
    for (size_t i = 0; i < *n; i++) { d[o++] = d[i]; if (d[i] == 0xFF && i + 1 < *n && d[i + 1] == 0) i++; }
    *n = o;
}

static int htx_id3_walk(const htx_rd *r, htx_id3 *t)
{
    int hl = t->ver == 2 ? 6 : 10;
    uint64_t off = t->frames_off, lim = 10 + (uint64_t)t->size;
    unsigned char b[10];
    while (off + hl <= lim) {
        if (htx_rd_at(r, off, b, hl)) break;
        if (b[0] == 0 || !htx_id3_idok(b, hl == 6 ? 3 : 4)) break;
        uint32_t sz;
        if (t->ver == 2) sz = htx_be24(b + 3);
        else if (t->ver == 3 || ((b[4] | b[5] | b[6] | b[7]) & 0x80)) sz = htx_be32(b + 4);
        else {
            uint32_t ss = htx_ss32(b + 4), pl = htx_be32(b + 4);
            sz = ss;
            if (ss != pl && !htx_id3_ok_at(r, off + 10 + ss, lim) && htx_id3_ok_at(r, off + 10 + pl, lim)) sz = pl;
        }
        if (off + hl + sz > lim) break;
        if (t->nf >= t->cap) {
            int nc = t->cap ? t->cap * 2 : 32;
            htx_frm *q = realloc(t->f, nc * sizeof *q);
            if (!q) return HT_ENOMEM;
            t->f = q; t->cap = nc;
        }
        t->f[t->nf].off = off; t->f[t->nf].size = hl + sz; t->f[t->nf].field = htx_id3_field(b, t->ver);
        t->nf++;
        off += hl + sz;
    }
    t->frames_end = off;
    return HT_OK;
}

/* cabecera ID3v2 + ID3v1. Las tramas se recorren aparte (fichero o memoria). */
static int htx_id3_head(int fd, uint64_t fsize, htx_id3 *t)
{
    unsigned char h[10];
    memset(t, 0, sizeof *t);
    if (fsize >= 10 && !htx_pread(fd, h, 10, 0) && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4 &&
        !((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        t->ver = h[3]; t->flags = h[5]; t->size = htx_ss32(h + 6);
        t->tag_end = 10 + (uint64_t)t->size + (t->ver == 4 && (t->flags & 0x10) ? 10 : 0);
        if (t->tag_end > fsize) return HT_EFORMAT;
        t->frames_off = 10;
        if ((t->flags & 0x40) && t->ver >= 3) {
            unsigned char e[4];
            if (htx_pread(fd, e, 4, 10)) return HT_EFORMAT;
            t->frames_off += t->ver == 3 ? htx_be32(e) + 4 : htx_ss32(e);
            if (t->frames_off > 10 + (uint64_t)t->size) return HT_EFORMAT;
        }
    }
    if (fsize >= t->tag_end + 128) {
        unsigned char v[3];
        if (!htx_pread(fd, v, 3, fsize - 128) && !memcmp(v, "TAG", 3)) t->v1 = 1;
    }
    return HT_OK;
}

/* texto ID3 en la codificacion enc -> UTF-8 nuevo (como mucho cap bytes; *trunc = 1 si no
 * cupo); varias cadenas separadas por NUL -> "; " */
static char *htx_id3_text2(int enc, const unsigned char *d, size_t n, size_t cap, int *trunc)
{
    int be = enc == 2, start = 1, sep = 0;
    htx_str s = { 0 };
    size_t i = 0;
    if (trunc) *trunc = 0;
#define HTX_EMIT(u) do { if (sep && s.n) hs_add(&s, "; ", 2); sep = 0; hs_cp(&s, (u)); } while (0)
    if (enc == 1 || enc == 2) {
        while (i + 1 < n && s.n < cap) {
            if (enc == 1 && start) {
                start = 0;
                if (d[i] == 0xFF && d[i + 1] == 0xFE) { be = 0; i += 2; continue; }
                if (d[i] == 0xFE && d[i + 1] == 0xFF) { be = 1; i += 2; continue; }
            }
            uint32_t u = be ? (uint32_t)(d[i] << 8 | d[i + 1]) : (uint32_t)(d[i] | d[i + 1] << 8);
            i += 2;
            if (!u) { sep = 1; start = 1; continue; }
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) {
                uint32_t u2 = be ? (uint32_t)(d[i] << 8 | d[i + 1]) : (uint32_t)(d[i] | d[i + 1] << 8);
                if (u2 >= 0xDC00 && u2 < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00); i += 2; }
                else u = 0xFFFD;
            } else if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;
            HTX_EMIT(u);
        }
    } else {
        const unsigned char *p = d, *e = d + n;
        while (p < e && s.n < cap) {
            if (!*p) { sep = 1; p++; continue; }
            uint32_t u = enc == 3 ? htx_u8(&p, e) : *p++;
            HTX_EMIT(u);
        }
        i = (size_t)(p - d);
    }
#undef HTX_EMIT
    if (trunc && s.n >= cap) {     /* quedaba texto de verdad (no solo NUL finales)? */
        for (size_t k = i; k < n; k++) if (d[k]) { *trunc = 1; break; }
    }
    if (s.oom || !s.n) { free(s.p); return 0; }
    htx_u8_trunc(s.p, cap);
    return s.p;
}
/* texto de una trama ID3 (byte de codificacion + datos) */
static char *htx_id3_text(const unsigned char *d, size_t n)
{
    if (!n) return 0;
    return htx_id3_text2(d[0], d + 1, n - 1, HT_MAXVAL, 0);
}
/* fin de la primera cadena (posicion del terminador o n) y longitud del terminador */
static size_t htx_id3_strend(int enc, const unsigned char *d, size_t n, size_t *tl)
{
    if (enc == 1 || enc == 2) {
        *tl = 2;
        for (size_t i = 0; i + 1 < n; i += 2) if (!d[i] && !d[i + 1]) return i;
        *tl = 0; return n;
    }
    *tl = 1;
    for (size_t i = 0; i < n; i++) if (!d[i]) return i;
    *tl = 0; return n;
}

static void htx_id3v1_read(int fd, uint64_t fsize, ht_tags *t)
{
    unsigned char v[128];
    if (htx_pread(fd, v, 128, fsize - 128)) return;
    static const int pos[] = { 3, 33, 63, 93 }, len[] = { 30, 30, 30, 4 }, fld[] = { HT_TITLE, HT_ARTIST, HT_ALBUM, HT_YEAR };
    for (int k = 0; k < 4; k++) {
        if (t->v[fld[k]]) continue;
        int n = len[k];
        while (n && (v[pos[k] + n - 1] == ' ' || v[pos[k] + n - 1] == 0)) n--;
        for (int i = 0; i < n; i++) if (!v[pos[k] + i]) { n = i; break; }
        if (!n) continue;
        htx_str s = { 0 };
        for (int i = 0; i < n; i++) hs_cp(&s, v[pos[k] + i]);
        if (s.p) htx_tag_add(t, fld[k], s.p, s.n);
        free(s.p);
    }
    if (!t->v[HT_TRACK] && v[125] == 0 && v[126]) { char b[8]; int n = snprintf(b, sizeof b, "%d", v[126]); htx_tag_add(t, HT_TRACK, b, n); }
    if (!t->v[HT_GENRE] && v[127] < HTX_NGENRES) htx_tag_add(t, HT_GENRE, htx_genres[v[127]], strlen(htx_genres[v[127]]));
}

/* carga la etiqueta a memoria deshaciendo la "unsynchronisation" global (v2.2/v2.3), max 1 MB */
static unsigned char *htx_id3_load_unsync(int fd, htx_id3 *t, uint64_t *mlen)
{
    uint64_t n = 10 + (uint64_t)t->size;
    if (n > 1024 * 1024) n = 1024 * 1024;
    unsigned char *m = malloc((size_t)n);
    if (!m) return 0;
    if (htx_pread(fd, m, (size_t)n, 0)) { free(m); return 0; }
    size_t body = (size_t)n - 10;
    htx_deunsync(m + 10, &body);
    *mlen = 10 + body;
    t->size = (uint32_t)body;
    return m;
}

static int htx_mp3_read(int fd, uint64_t fsize, ht_tags *t)
{
    htx_id3 id; htx_rd r = { fd, 0, 0 };
    int e = htx_id3_head(fd, fsize, &id);
    if (e) return e;
    unsigned char *mem = 0;
    if (id.ver) {
        if ((id.flags & 0x80) && id.ver <= 3) { mem = htx_id3_load_unsync(fd, &id, &r.mlen); r.mem = mem; if (!mem) id.ver = 0; }
        if (id.ver && !(id.ver == 2 && (id.flags & 0x40))) htx_id3_walk(&r, &id);   /* v2.2 con compresion: no */
    }
    unsigned char *buf = malloc(65536);
    for (int i = 0; buf && i < id.nf; i++) {
        htx_frm *fr = &id.f[i];
        if (fr->field < 0 || t->v[fr->field]) continue;
        int hl = id.ver == 2 ? 6 : 10;
        unsigned char fh[10];
        if (htx_rd_at(&r, fr->off, fh, hl)) continue;
        size_t n = fr->size - hl;
        if (n > 65536) n = 65536;
        if (htx_rd_at(&r, fr->off + hl, buf, n)) continue;
        unsigned char *d = buf;
        if (id.ver == 3) {
            if (fh[9] & 0xC0) continue;                     /* comprimida o cifrada */
            if (fh[9] & 0x20) { if (!n) continue; d++; n--; }  /* grupo */
        } else if (id.ver == 4) {
            if (fh[9] & 0x0C) continue;
            if (fh[9] & 0x40) { if (!n) continue; d++; n--; }
            if (fh[9] & 0x01) { if (n < 4) continue; d += 4; n -= 4; }
            if (fh[9] & 0x02) htx_deunsync(d, &n);
        }
        char *s = htx_id3_text(d, n);
        if (!s) continue;
        if (fr->field == HT_GENRE) { char *g = htx_genre_fix(s); if (g) { free(s); s = g; } }
        htx_tag_add(t, fr->field, s, strlen(s));
        free(s);
    }
    free(buf); free(mem); free(id.f);
    if (id.v1) htx_id3v1_read(fd, fsize, t);
    return HT_OK;
}

/* ---- ID3: todas las tramas de texto */
/* id v2.3, id v2.4, id v2.2, clave (0 = no existe en esa version: se escribe como TXXX) */
static const char *const htx_id3map[][4] = {
    { "TIT2", "TIT2", "TT2", "TITLE" }, { "TPE1", "TPE1", "TP1", "ARTIST" }, { "TALB", "TALB", "TAL", "ALBUM" },
    { "TPE2", "TPE2", "TP2", "ALBUMARTIST" }, { "TCON", "TCON", "TCO", "GENRE" }, { "TYER", "TDRC", "TYE", "DATE" },
    { "TRCK", "TRCK", "TRK", "TRACKNUMBER" }, { "TPOS", "TPOS", "TPA", "DISCNUMBER" },
    { "TCOM", "TCOM", "TCM", "COMPOSER" }, { "TPE3", "TPE3", "TP3", "CONDUCTOR" }, { "TPE4", "TPE4", "TP4", "REMIXER" },
    { "TEXT", "TEXT", "TXT", "LYRICIST" }, { "TPUB", "TPUB", "TPB", "LABEL" }, { "TSRC", "TSRC", "TRC", "ISRC" },
    { "TBPM", "TBPM", "TBP", "BPM" }, { "TCOP", "TCOP", "TCR", "COPYRIGHT" }, { "TENC", "TENC", "TEN", "ENCODEDBY" },
    { "TSSE", "TSSE", "TSS", "ENCODER" }, { "TKEY", "TKEY", "TKE", "KEY" }, { "TLAN", "TLAN", "TLA", "LANGUAGE" },
    { 0, "TMOO", 0, "MOOD" }, { "TIT1", "TIT1", "TT1", "GROUPING" }, { "TIT3", "TIT3", "TT3", "SUBTITLE" },
    { "TSOP", "TSOP", 0, "ARTISTSORT" }, { "TSOA", "TSOA", 0, "ALBUMSORT" }, { "TSOT", "TSOT", 0, "TITLESORT" },
    { "TSO2", "TSO2", 0, "ALBUMARTISTSORT" }, { "TSOC", "TSOC", 0, "COMPOSERSORT" }, { "TCMP", "TCMP", "TCP", "COMPILATION" },
    { "TORY", "TDOR", "TOR", "ORIGINALDATE" }, { "TMED", "TMED", "TMT", "MEDIA" }, { "TOPE", "TOPE", "TOA", "ORIGINALARTIST" },
    { "TOAL", "TOAL", "TOT", "ORIGINALALBUM" }, { 0, "TSST", 0, "DISCSUBTITLE" }, { 0, "TDRL", 0, "RELEASEDATE" } };
#define HTX_NID3MAP ((int)(sizeof htx_id3map / sizeof *htx_id3map))

/* clave de una trama de texto por su id (o 0) */
static const char *htx_id3_mapkey(const unsigned char *id, int ver)
{
    for (int i = 0; i < HTX_NID3MAP; i++) {
        if (ver == 2) { if (htx_id3map[i][2] && !memcmp(id, htx_id3map[i][2], 3)) return htx_id3map[i][3]; continue; }
        if ((htx_id3map[i][0] && !memcmp(id, htx_id3map[i][0], 4)) || !memcmp(id, htx_id3map[i][1], 4)) return htx_id3map[i][3];
    }
    if (ver >= 3 && !memcmp(id, "TDRC", 4)) return "DATE";
    if (ver >= 3 && !memcmp(id, "TDOR", 4)) return "ORIGINALDATE";
    return 0;
}
/* id de trama para una clave en la version ver (0 = no hay: TXXX) */
static const char *htx_id3_idof(const char *key, int ver)
{
    for (int i = 0; i < HTX_NID3MAP; i++) if (!strcmp(htx_id3map[i][3], key)) return htx_id3map[i][ver == 4 ? 1 : 0];
    return 0;
}

/* contenido util de una trama (sin cabecera ni bytes de flags; v2.4 sin unsync). 0 = ok */
static int htx_id3_payload(const htx_rd *r, const htx_id3 *t, const htx_frm *fr, unsigned char *buf, size_t cap,
                           unsigned char **d, size_t *n, int *cut)
{
    int hl = t->ver == 2 ? 6 : 10;
    unsigned char fh[10];
    if (htx_rd_at(r, fr->off, fh, hl)) return -1;
    size_t k = fr->size - hl;
    *cut = k > cap;
    if (k > cap) k = cap;
    if (htx_rd_at(r, fr->off + hl, buf, k)) return -1;
    unsigned char *p = buf;
    if (t->ver == 3) {
        if (fh[9] & 0xC0) return -1;                       /* comprimida o cifrada */
        if (fh[9] & 0x20) { if (!k) return -1; p++; k--; }
    } else if (t->ver == 4) {
        if (fh[9] & 0x0C) return -1;
        if (fh[9] & 0x40) { if (!k) return -1; p++; k--; }
        if (fh[9] & 0x01) { if (k < 4) return -1; p += 4; k -= 4; }
        if (fh[9] & 0x02) htx_deunsync(p, &k);
    }
    *d = p; *n = k;
    return 0;
}

/* clave canonica de una trama (TXXX/COMM/USLT: con su descripcion). lang (opcional) = idioma de
 * COMM/USLT. desc (opcional) = descripcion tal cual (TXXX). 1 si es una trama de texto conocida. */
static int htx_id3_frame_key(const unsigned char *id, int ver, const unsigned char *d, size_t n,
                             char *key, size_t ksz, char *lang, char *desc, size_t dsz)
{
    int il = ver == 2 ? 3 : 4;
    int txxx = ver == 2 ? !memcmp(id, "TXX", 3) : !memcmp(id, "TXXX", 4);
    int comm = ver == 2 ? !memcmp(id, "COM", 3) : !memcmp(id, "COMM", 4);
    int uslt = ver == 2 ? !memcmp(id, "ULT", 3) : !memcmp(id, "USLT", 4);
    if (desc && dsz) desc[0] = 0;
    if (!txxx && !comm && !uslt) {
        if (id[0] != 'T') return 0;
        const char *k = htx_id3_mapkey(id, ver);
        if (k) snprintf(key, ksz, "%s", k); else snprintf(key, ksz, "%.*s", il, (const char *)id);
        return 1;
    }
    if (!d || n < 1) return 0;
    int enc = d[0];
    size_t off = 1;
    if (comm || uslt) { if (n < 4) return 0; if (lang) { memcpy(lang, d + 1, 3); lang[3] = 0; } off = 4; }
    size_t tl, e = htx_id3_strend(enc, d + off, n - off, &tl);
    char *ds = e ? htx_id3_text2(enc, d + off, e, HTX_KEYMAX - 16, 0) : 0;
    if (ds) for (char *c = ds; *c; c++) if ((unsigned char)*c < 0x20 || *c == '=') *c = ' ';
    if (txxx) snprintf(key, ksz, "%s", ds && *ds ? ds : "TXXX");
    else snprintf(key, ksz, "%s%s%s", comm ? "COMMENT" : "LYRICS", ds && *ds ? ":" : "", ds ? ds : "");
    if (desc && ds) snprintf(desc, dsz, "%s", ds);
    free(ds);
    return 1;                      /* key tal cual (la descripcion con sus mayusculas); canonica con ht_key_canon */
}

#define HTX_FRAMEMAX (256 * 1024)
static int htx_mp3_read_all(int fd, uint64_t fsize, ht_all *a)
{
    htx_id3 id; htx_rd r = { fd, 0, 0 };
    int e = htx_id3_head(fd, fsize, &id);
    if (e) return e;
    unsigned char *mem = 0;
    if (id.ver) {
        if ((id.flags & 0x80) && id.ver <= 3) { mem = htx_id3_load_unsync(fd, &id, &r.mlen); r.mem = mem; if (!mem) id.ver = 0; }
        if (id.ver && !(id.ver == 2 && (id.flags & 0x40))) htx_id3_walk(&r, &id);
    }
    unsigned char *buf = id.nf ? malloc(HTX_FRAMEMAX) : 0;
    if (id.nf && !buf) e = HT_ENOMEM;
    for (int i = 0; buf && i < id.nf; i++) {
        unsigned char fid[4], *d; size_t n; int cut, tr = 0;
        if (htx_rd_at(&r, id.f[i].off, fid, id.ver == 2 ? 3 : 4)) continue;
        if (fid[0] != 'T' && memcmp(fid, "COM", 3) && memcmp(fid, "ULT", 3) && memcmp(fid, "USLT", 4)) continue;
        if (htx_id3_payload(&r, &id, &id.f[i], buf, HTX_FRAMEMAX, &d, &n, &cut) || !n) continue;
        char key[HTX_KEYMAX];
        if (!htx_id3_frame_key(fid, id.ver, d, n, key, sizeof key, 0, 0, 0)) continue;
        int enc = d[0], txxx = !memcmp(fid, id.ver == 2 ? "TXX" : "TXXX", id.ver == 2 ? 3 : 4);
        size_t off = 1, tl = 0;
        if (fid[0] != 'T') off = 4;                                  /* COMM/USLT: idioma */
        if (fid[0] != 'T' || txxx) { size_t e2 = htx_id3_strend(enc, d + off, n - off, &tl); off += e2 + tl; }
        if (off > n) continue;
        char *s = htx_id3_text2(enc, d + off, n - off, HT_LONGMAX, &tr);
        if (!s) s = strdup("");
        if (!s) continue;
        int fl = tr || cut ? HT_ITEM_TRUNC : 0;
        int known = fid[0] != 'T' || txxx || htx_id3_mapkey(fid, id.ver);
        if (!known) fl |= HT_ITEM_RO;
        if (!strcmp(key, "GENRE")) { char *g = htx_genre_fix(s); if (g) { free(s); s = g; } }
        if (!txxx && (!strcmp(key, "TRACKNUMBER") || !strcmp(key, "DISCNUMBER"))) {   /* "3/12" */
            char *sl = strchr(s, '/');
            if (sl) { *sl = 0; htx_all_add(a, !strcmp(key, "DISCNUMBER") ? "DISCTOTAL" : "TRACKTOTAL", sl + 1, strlen(sl + 1), fl); }
        }
        if (*s) htx_all_add(a, key, s, strlen(s), fl);
        free(s);
    }
    free(buf); free(mem); free(id.f);
    if (id.v1) {   /* ID3v1: solo lo que falte de las 8 de siempre */
        ht_tags t; memset(&t, 0, sizeof t);
        htx_id3v1_read(fd, fsize, &t);
        for (int f = 0; f < HT_NFIELDS; f++) if (t.v[f] && !ht_all_find(a, htx_base_keys[f])) htx_all_add(a, htx_base_keys[f], t.v[f], strlen(t.v[f]), 0);
        ht_tags_free(&t);
    }
    return e;
}

/* trama nueva con ns cadenas (v2.4: UTF-8; v2.3: Latin-1 si se puede, si no UTF-16 con BOM),
 * separadas por su terminador; lang (COMM/USLT) va tras el byte de codificacion */
static void htx_id3_frame(htx_str *o, int ver, const char *id, const char *lang, const char *const *sv, int ns)
{
    htx_str d = { 0 };
    int enc = 3;
    if (ver != 4) {
        enc = 0;
        for (int k = 0; k < ns && !enc; k++) {
            const unsigned char *q = (const unsigned char *)sv[k], *e = q + strlen(sv[k]);
            while (q < e) if (htx_u8(&q, e) > 0xFF) { enc = 1; break; }
        }
    }
    hs_byte(&d, enc);
    if (lang) hs_add(&d, lang, 3);
    for (int k = 0; k < ns; k++) {
        const unsigned char *p = (const unsigned char *)sv[k], *e = p + strlen(sv[k]);
        if (enc == 3) hs_add(&d, sv[k], strlen(sv[k]));
        else if (enc == 0) while (p < e) hs_byte(&d, (int)htx_u8(&p, e));
        else {
            hs_byte(&d, 0xFF); hs_byte(&d, 0xFE);
            while (p < e) {
                uint32_t u = htx_u8(&p, e);
                if (u >= 0x10000) { u -= 0x10000; uint32_t a = 0xD800 + (u >> 10), b = 0xDC00 + (u & 0x3FF); hs_byte(&d, a & 0xFF); hs_byte(&d, a >> 8); hs_byte(&d, b & 0xFF); hs_byte(&d, b >> 8); }
                else { hs_byte(&d, u & 0xFF); hs_byte(&d, u >> 8); }
            }
        }
        if (k + 1 < ns) { hs_byte(&d, 0); if (enc == 1) hs_byte(&d, 0); }
    }
    unsigned char h[10];
    memcpy(h, id, 4);
    if (ver == 4) htx_put_ss32(h + 4, (uint32_t)d.n); else htx_put_be32(h + 4, (uint32_t)d.n);
    h[8] = h[9] = 0;
    hs_add(o, h, 10);
    hs_add(o, d.p, d.n);
    if (d.oom) o->oom = 1;
    free(d.p);
}
static void htx_id3_newframe(htx_str *o, int ver, const char *id, const char *v) { htx_id3_frame(o, ver, id, 0, &v, 1); }

/* ID3v1: actualiza los campos que tiene (Latin-1, 30 bytes) */
static void htx_id3v1_set(unsigned char *v, const ht_changes *ch)
{
    static const int pos[] = { 3, 33, 63 }, fld[] = { HT_TITLE, HT_ARTIST, HT_ALBUM };
    for (int k = 0; k < 3; k++) {
        const char *s = ch->v[fld[k]];
        if (!s) continue;
        memset(v + pos[k], 0, 30);
        const unsigned char *p = (const unsigned char *)s, *e = p + strlen(s);
        for (int i = 0; i < 30 && p < e; i++) { uint32_t u = htx_u8(&p, e); v[pos[k] + i] = u > 0xFF ? '?' : (unsigned char)u; }
    }
    if (ch->v[HT_YEAR]) {
        memset(v + 93, 0, 4);
        for (int i = 0; i < 4 && ch->v[HT_YEAR][i] >= '0' && ch->v[HT_YEAR][i] <= '9'; i++) v[93 + i] = ch->v[HT_YEAR][i];
    }
    if (ch->v[HT_TRACK] && v[125] == 0) { int n = atoi(ch->v[HT_TRACK]); v[126] = n > 0 && n < 256 ? n : 0; }
    if (ch->v[HT_GENRE]) {
        v[127] = 255;
        for (int i = 0; i < HTX_NGENRES; i++) if (!strcasecmp(ch->v[HT_GENRE], htx_genres[i])) { v[127] = i; break; }
    }
}

typedef struct {
    int fd; uint64_t fsize; htx_id3 *t; const unsigned char *drop; htx_str nf; int nver;
    unsigned char v1[128]; uint32_t body;
} htx_mw;

static void htx_mp3_emit_all(htx_sink *s, void *ctx)
{
    htx_mw *w = ctx;
    htx_id3 *t = w->t;
    unsigned char h[10] = { 'I', 'D', '3', (unsigned char)w->nver, 0, (unsigned char)(t->ver ? t->flags & 0x20 : 0) };
    htx_put_ss32(h + 6, w->body);
    sk_put(s, h, 10);
    for (int pass = 0; pass < 2; pass++)            /* primero las tramas grandes (caratulas) */
        for (int i = 0; i < t->nf; i++) {
            if (w->drop[i] || (t->f[i].size > HTX_BIGFRAME) != !pass) continue;
            sk_copy(s, t->f[i].off, t->f[i].size);
        }
    sk_put(s, w->nf.p, w->nf.n);
    sk_zero(s, HT_PAD);
    uint64_t a = t->ver ? t->tag_end : 0, z = w->fsize - (t->v1 ? 128 : 0);
    sk_copy(s, a, z - a);
    if (t->v1) sk_put(s, w->v1, 128);
}

/* numero/total de TRCK o TPOS: nuevos (si se cambian) o los que habia */
static void htx_id3_numtot(htx_str *o, int ver, const char *id, const htx_eds *ed, const char *kn, const char *kt,
                           const char *oldnum, const char *oldtot)
{
    int in = htx_eds_find(ed, kn), it = htx_eds_find(ed, kt);
    if (in < 0 && it < 0) return;
    const char *num = in >= 0 ? ed->e[in].val : oldnum ? oldnum : "";
    const char *tot = it >= 0 ? ed->e[it].val : oldtot ? oldtot : "";
    if (!*num) return;                                          /* sin numero no hay trama */
    char b[256];
    if (strchr(num, '/') || !*tot) snprintf(b, sizeof b, "%s", num);
    else snprintf(b, sizeof b, "%s/%s", num, tot);
    htx_id3_newframe(o, ver, id, b);
}

static int htx_mp3_write(const char *path, int fd, const struct stat *st, const htx_eds *ed,
                         ht_progress_fn cb, void *ud, int *mode)
{
    htx_mw w; memset(&w, 0, sizeof w);
    htx_id3 t;
    w.fd = fd; w.fsize = (uint64_t)st->st_size; w.t = &t;
    int e = htx_id3_head(fd, w.fsize, &t);
    if (e) return e;
    if (t.ver == 2 || (t.ver == 3 && (t.flags & 0x80))) return HT_EUNSUP;   /* v2.2 / unsync global v2.3 */
    htx_rd r = { fd, 0, 0 };
    unsigned char *drop = 0, *buf = 0;
    char (*desc)[HTX_KEYMAX] = 0, (*lang)[4] = 0;
    char *oldv[4] = { 0 };                                      /* numero y total de pista y disco */
    if (t.ver && (e = htx_id3_walk(&r, &t))) goto out;
    w.nver = t.ver ? t.ver : 3;
    drop = calloc(t.nf + 1, 1);
    desc = calloc(ed->n + 1, sizeof *desc);
    lang = calloc(ed->n + 1, sizeof *lang);
    buf = malloc(HTX_FRAMEMAX);
    if (!drop || !desc || !lang || !buf) { e = HT_ENOMEM; goto out; }
    /* que tramas sustituyen los cambios (las ilegibles, cifradas... se conservan) */
    for (int i = 0; i < t.nf; i++) {
        unsigned char fid[4], *d = 0; size_t n = 0; int cut = 0;
        if (htx_pread(fd, fid, 4, t.f[i].off)) { e = HT_EIO; goto out; }
        int txt = fid[0] == 'T' && memcmp(fid, "TXXX", 4);
        if (!txt && memcmp(fid, "TXXX", 4) && memcmp(fid, "COMM", 4) && memcmp(fid, "USLT", 4)) continue;
        int rd = !htx_id3_payload(&r, &t, &t.f[i], buf, HTX_FRAMEMAX, &d, &n, &cut);
        if (!txt && (!rd || cut)) continue;
        char key[HTX_KEYMAX], lg[4] = "", ds[HTX_KEYMAX];
        if (!htx_id3_frame_key(fid, t.ver, rd ? d : 0, n, key, sizeof key, lg, ds, sizeof ds)) continue;
        int numtot = !memcmp(fid, "TRCK", 4) ? 0 : !memcmp(fid, "TPOS", 4) ? 2 : -1;
        if (numtot >= 0) {
            if (rd && !cut && !oldv[numtot]) {
                char *s = htx_id3_text(d, n);
                if (s) { char *sl = strchr(s, '/'); if (sl) { *sl = 0; oldv[numtot + 1] = strdup(sl + 1); } oldv[numtot] = s; }
            }
            if (htx_eds_find(ed, numtot ? "DISCNUMBER" : "TRACKNUMBER") >= 0 || htx_eds_find(ed, numtot ? "DISCTOTAL" : "TRACKTOTAL") >= 0) drop[i] = 1;
            continue;
        }
        char ck[HTX_KEYMAX]; ht_key_canon(key, ck, sizeof ck);
        int k = htx_eds_find(ed, ck);
        if (k < 0) continue;
        drop[i] = 1;
        if (!desc[k][0] && ds[0]) snprintf(desc[k], HTX_KEYMAX, "%s", ds);
        if (!lang[k][0] && lg[0]) memcpy(lang[k], lg, 4);
    }
    w.drop = drop;
    /* tramas nuevas */
    htx_id3_numtot(&w.nf, w.nver, "TRCK", ed, "TRACKNUMBER", "TRACKTOTAL", oldv[0], oldv[1]);
    htx_id3_numtot(&w.nf, w.nver, "TPOS", ed, "DISCNUMBER", "DISCTOTAL", oldv[2], oldv[3]);
    for (int k = 0; k < ed->n; k++) {
        const char *key = ed->e[k].key, *v = ed->e[k].val;
        if (!*v || !strcmp(key, "TRACKNUMBER") || !strcmp(key, "TRACKTOTAL") || !strcmp(key, "DISCNUMBER") || !strcmp(key, "DISCTOTAL")) continue;
        int comm = !strncmp(key, "COMMENT", 7) && (!key[7] || key[7] == ':'), uslt = !strncmp(key, "LYRICS", 6) && (!key[6] || key[6] == ':');
        if (comm || uslt) {
            const char *dsc = desc[k][0] ? desc[k] : strchr(key, ':') ? strchr(key, ':') + 1 : "";
            const char *sv[2] = { dsc, v };
            htx_id3_frame(&w.nf, w.nver, comm ? "COMM" : "USLT", lang[k][0] ? lang[k] : "eng", sv, 2);
            continue;
        }
        const char *fid = htx_id3_idof(key, w.nver);
        if (fid) { htx_id3_newframe(&w.nf, w.nver, fid, v); continue; }
        /* TXXX: la descripcion como estaba en el fichero; si no, la forma de Picard o la clave */
        const char *dsc = desc[k][0] ? desc[k] : key;
        char pic[HTX_KEYMAX];
        if (!desc[k][0]) for (int a = 0; a < HTX_NALIAS; a++)
            if (!strcmp(htx_alias[a][1], key) && !strncmp(htx_alias[a][0], "MUSICBRAINZ ", 12)) {
                /* "MUSICBRAINZ ALBUM ID" -> "MusicBrainz Album Id" */
                snprintf(pic, sizeof pic, "%s", htx_alias[a][0]);
                for (char *c = pic, prev = ' '; *c; prev = *c, c++) if (prev != ' ' && *c >= 'A' && *c <= 'Z') *c += 32;
                memcpy(pic, "MusicBrainz", 11); dsc = pic; break;
            }
        const char *sv[2] = { dsc, v };
        htx_id3_frame(&w.nf, w.nver, "TXXX", 0, sv, 2);
    }
    if (w.nf.oom) { e = HT_ENOMEM; goto out; }
    if (t.v1) {
        ht_changes ch; htx_eds_changes(ed, &ch);
        if (htx_pread(fd, w.v1, 128, w.fsize - 128)) { e = HT_EIO; goto out; }
        htx_id3v1_set(w.v1, &ch);
    }
    /* en sitio: se reescribe desde la primera trama quitada hasta el final de la etiqueta */
    if (t.ver && !(t.flags & 0x40) && !(t.ver == 4 && (t.flags & 0x10))) {
        int i0 = 0;
        while (i0 < t.nf && !drop[i0]) i0++;
        uint64_t start = i0 < t.nf ? t.f[i0].off : t.frames_end, tail = 0;
        for (int i = i0; i < t.nf; i++) if (!drop[i]) tail += t.f[i].size;
        uint64_t avail = 10 + (uint64_t)t.size - start, need = tail + w.nf.n;
        uint64_t wlen = need > t.frames_end - start ? need : t.frames_end - start;
        if (need <= avail && wlen <= HT_INPLACE_MAX) {
            htx_sink s;
            if (htx_memsink(&s, fd, (size_t)wlen)) { e = HT_ENOMEM; goto out; }
            for (int i = i0; i < t.nf; i++) if (!drop[i]) sk_copy(&s, t.f[i].off, t.f[i].size);
            sk_put(&s, w.nf.p, w.nf.n);
            if (s.mlen < wlen) sk_zero(&s, wlen - s.mlen);
            e = s.err;
            if (!e && s.mlen) e = htx_pwrite_all(fd, s.mem, s.mlen, start);
            if (!e && t.v1) e = htx_pwrite_all(fd, w.v1, 128, w.fsize - 128);
            if (!e && fsync(fd)) e = HT_EIO;
            free(s.mem);
            if (!e) { *mode = HT_MODE_INPLACE; if (cb) cb(ud, 1, 1); }
            goto out;
        }
    }
    uint64_t body = w.nf.n + HT_PAD;
    for (int i = 0; i < t.nf; i++) if (!drop[i]) body += t.f[i].size;
    if (body >= (1u << 28)) { e = HT_EUNSUP; goto out; }
    w.body = (uint32_t)body;
    uint64_t a = t.ver ? t.tag_end : 0;
    uint64_t ns = 10 + body + (w.fsize - a);
    e = htx_rewrite(path, fd, st, ns, htx_mp3_emit_all, &w, cb, ud);
    if (!e) *mode = HT_MODE_REWRITE;
out:
    free(drop); free(t.f); free(w.nf.p); free(buf); free(desc); free(lang);
    for (int k = 0; k < 4; k++) free(oldv[k]);
    return e;
}

/* ================================================================== M4A (solo lectura) */
#define HTX_FCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* busca la caja "type" entre [off, end): devuelve inicio de su contenido y fin */
static int htx_m4a_find(int fd, uint64_t off, uint64_t end, uint32_t type, uint64_t *cin, uint64_t *cend)
{
    unsigned char h[16];
    while (off + 8 <= end) {
        if (htx_pread(fd, h, 8, off)) return -1;
        uint64_t sz = htx_be32(h), hl = 8;
        if (sz == 1) { if (off + 16 > end || htx_pread(fd, h + 8, 8, off + 8)) return -1; sz = (uint64_t)htx_be32(h + 8) << 32 | htx_be32(h + 12); hl = 16; }
        else if (sz == 0) sz = end - off;
        if (sz < hl || off + sz > end) return -1;
        if (htx_be32(h + 4) == type) { *cin = off + hl; *cend = off + sz; return 0; }
        off += sz;
    }
    return -1;
}

static int htx_m4a_read(int fd, uint64_t fsize, ht_tags *t)
{
    uint64_t a, b, c, d, e, f, g, h;
    if (htx_m4a_find(fd, 0, fsize, HTX_FCC('m', 'o', 'o', 'v'), &a, &b)) return HT_EFORMAT;
    if (htx_m4a_find(fd, a, b, HTX_FCC('u', 'd', 't', 'a'), &c, &d)) return HT_OK;
    if (htx_m4a_find(fd, c, d, HTX_FCC('m', 'e', 't', 'a'), &e, &f)) return HT_OK;
    if (htx_m4a_find(fd, e + 4, f, HTX_FCC('i', 'l', 's', 't'), &g, &h)) return HT_OK;
    static const struct { uint32_t fcc; int f; } map[] = {
        { HTX_FCC(0xA9, 'n', 'a', 'm'), HT_TITLE }, { HTX_FCC(0xA9, 'A', 'R', 'T'), HT_ARTIST },
        { HTX_FCC(0xA9, 'a', 'l', 'b'), HT_ALBUM }, { HTX_FCC('a', 'A', 'R', 'T'), HT_ALBUMARTIST },
        { HTX_FCC(0xA9, 'g', 'e', 'n'), HT_GENRE }, { HTX_FCC('g', 'n', 'r', 'e'), HT_GENRE },
        { HTX_FCC(0xA9, 'd', 'a', 'y'), HT_YEAR }, { HTX_FCC('t', 'r', 'k', 'n'), HT_TRACK },
        { HTX_FCC('d', 'i', 's', 'k'), HT_DISC } };
    unsigned char *buf = malloc(HT_MAXVAL + 16);
    if (!buf) return HT_ENOMEM;
    uint64_t off = g;
    unsigned char hd[8];
    while (off + 8 <= h) {
        if (htx_pread(fd, hd, 8, off)) break;
        uint64_t sz = htx_be32(hd);
        if (sz < 8 || off + sz > h) break;
        uint32_t fcc = htx_be32(hd + 4);
        int fld = -1;
        for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (map[i].fcc == fcc) fld = map[i].f;
        uint64_t di, de;
        if (fld >= 0 && !t->v[fld] && !htx_m4a_find(fd, off + 8, off + sz, HTX_FCC('d', 'a', 't', 'a'), &di, &de) && de - di >= 8) {
            size_t n = (size_t)(de - di > HT_MAXVAL + 8 ? HT_MAXVAL + 8 : de - di);
            if (!htx_pread(fd, buf, n, di)) {
                const unsigned char *p = buf + 8; size_t pn = n - 8;
                char tmp[32];
                if (fcc == HTX_FCC('t', 'r', 'k', 'n') || fcc == HTX_FCC('d', 'i', 's', 'k')) {
                    if (pn >= 4) {
                        int num = p[2] << 8 | p[3], tot = pn >= 6 ? p[4] << 8 | p[5] : 0;
                        int k = tot ? snprintf(tmp, sizeof tmp, "%d/%d", num, tot) : snprintf(tmp, sizeof tmp, "%d", num);
                        if (num) htx_tag_add(t, fld, tmp, (size_t)k);
                    }
                } else if (fcc == HTX_FCC('g', 'n', 'r', 'e')) {
                    if (pn >= 2) { int gi = (p[0] << 8 | p[1]) - 1; if (gi >= 0 && gi < HTX_NGENRES) htx_tag_add(t, fld, htx_genres[gi], strlen(htx_genres[gi])); }
                } else htx_tag_add(t, fld, (const char *)p, strnlen((const char *)p, pn));
            }
        }
        off += sz;
    }
    free(buf);
    return HT_OK;
}

/* M4A: todas las etiquetas de texto de ilst (solo lectura) */
static int htx_m4a_read_all(int fd, uint64_t fsize, ht_all *a)
{
    uint64_t a0, b0, c, d, e, f, g, h;
    if (htx_m4a_find(fd, 0, fsize, HTX_FCC('m', 'o', 'o', 'v'), &a0, &b0)) return HT_EFORMAT;
    if (htx_m4a_find(fd, a0, b0, HTX_FCC('u', 'd', 't', 'a'), &c, &d)) return HT_OK;
    if (htx_m4a_find(fd, c, d, HTX_FCC('m', 'e', 't', 'a'), &e, &f)) return HT_OK;
    if (htx_m4a_find(fd, e + 4, f, HTX_FCC('i', 'l', 's', 't'), &g, &h)) return HT_OK;
    static const struct { uint32_t fcc; const char *k; } map[] = {
        { HTX_FCC(0xA9, 'n', 'a', 'm'), "TITLE" }, { HTX_FCC(0xA9, 'A', 'R', 'T'), "ARTIST" }, { HTX_FCC(0xA9, 'a', 'l', 'b'), "ALBUM" },
        { HTX_FCC('a', 'A', 'R', 'T'), "ALBUMARTIST" }, { HTX_FCC(0xA9, 'g', 'e', 'n'), "GENRE" }, { HTX_FCC('g', 'n', 'r', 'e'), "GENRE" },
        { HTX_FCC(0xA9, 'd', 'a', 'y'), "DATE" }, { HTX_FCC('t', 'r', 'k', 'n'), "TRACKNUMBER" }, { HTX_FCC('d', 'i', 's', 'k'), "DISCNUMBER" },
        { HTX_FCC(0xA9, 'w', 'r', 't'), "COMPOSER" }, { HTX_FCC(0xA9, 'c', 'm', 't'), "COMMENT" }, { HTX_FCC(0xA9, 'l', 'y', 'r'), "LYRICS" },
        { HTX_FCC(0xA9, 'g', 'r', 'p'), "GROUPING" }, { HTX_FCC('d', 'e', 's', 'c'), "DESCRIPTION" }, { HTX_FCC('c', 'p', 'r', 't'), "COPYRIGHT" },
        { HTX_FCC(0xA9, 't', 'o', 'o'), "ENCODER" }, { HTX_FCC('t', 'm', 'p', 'o'), "BPM" }, { HTX_FCC('c', 'p', 'i', 'l'), "COMPILATION" },
        { HTX_FCC('s', 'o', 'a', 'r'), "ARTISTSORT" }, { HTX_FCC('s', 'o', 'a', 'l'), "ALBUMSORT" }, { HTX_FCC('s', 'o', 'n', 'm'), "TITLESORT" },
        { HTX_FCC('s', 'o', 'a', 'a'), "ALBUMARTISTSORT" }, { HTX_FCC('s', 'o', 'c', 'o'), "COMPOSERSORT" }, { HTX_FCC(0xA9, 'w', 'r', 'k'), "WORK" },
        { HTX_FCC('-', '-', '-', '-'), "" } };
    unsigned char *buf = malloc(HT_LONGMAX + 16);
    if (!buf) return HT_ENOMEM;
    uint64_t off = g;
    unsigned char hd[8];
    while (off + 8 <= h) {
        if (htx_pread(fd, hd, 8, off)) break;
        uint64_t sz = htx_be32(hd);
        if (sz < 8 || off + sz > h) break;
        uint32_t fcc = htx_be32(hd + 4);
        const char *k = 0;
        for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (map[i].fcc == fcc) k = map[i].k;
        uint64_t di, de;
        char name[HTX_KEYMAX] = "";
        if (k && !*k) {                    /* ----: mean + name + data */
            uint64_t ni, ne;
            if (!htx_m4a_find(fd, off + 8, off + sz, HTX_FCC('n', 'a', 'm', 'e'), &ni, &ne) && ne - ni > 4 && ne - ni - 4 < sizeof name) {
                if (htx_pread(fd, name, (size_t)(ne - ni - 4), ni + 4)) name[0] = 0; else name[ne - ni - 4] = 0;
            }
            k = name[0] && ht_key_valid(name) ? name : 0;
        }
        if (k && !htx_m4a_find(fd, off + 8, off + sz, HTX_FCC('d', 'a', 't', 'a'), &di, &de) && de - di >= 8) {
            size_t n = (size_t)(de - di > HT_LONGMAX + 8 ? HT_LONGMAX + 8 : de - di);
            int fl = de - di > HT_LONGMAX + 8 ? HT_ITEM_TRUNC : 0;
            if (!htx_pread(fd, buf, n, di)) {
                const unsigned char *p = buf + 8; size_t pn = n - 8;
                uint32_t type = htx_be32(buf) & 0xFFFFFF;
                char tmp[32]; int kk;
                if (fcc == HTX_FCC('t', 'r', 'k', 'n') || fcc == HTX_FCC('d', 'i', 's', 'k')) {
                    int dk = fcc == HTX_FCC('d', 'i', 's', 'k');
                    if (pn >= 4) {
                        int num = p[2] << 8 | p[3], tot = pn >= 6 ? p[4] << 8 | p[5] : 0;
                        if (num) { kk = snprintf(tmp, sizeof tmp, "%d", num); htx_all_add(a, dk ? "DISCNUMBER" : "TRACKNUMBER", tmp, (size_t)kk, 0); }
                        if (tot) { kk = snprintf(tmp, sizeof tmp, "%d", tot); htx_all_add(a, dk ? "DISCTOTAL" : "TRACKTOTAL", tmp, (size_t)kk, 0); }
                    }
                } else if (fcc == HTX_FCC('g', 'n', 'r', 'e')) {
                    if (pn >= 2) { int gi = (p[0] << 8 | p[1]) - 1; if (gi >= 0 && gi < HTX_NGENRES) htx_all_add(a, k, htx_genres[gi], strlen(htx_genres[gi]), 0); }
                } else if (type == 21 || type == 0 || fcc == HTX_FCC('t', 'm', 'p', 'o') || fcc == HTX_FCC('c', 'p', 'i', 'l')) {
                    if (type == 1) htx_all_add(a, k, (const char *)p, strnlen((const char *)p, pn), fl);
                    else if (pn >= 1 && pn <= 4) { int v = 0; for (size_t i = 0; i < pn; i++) v = v << 8 | p[i]; kk = snprintf(tmp, sizeof tmp, "%d", v); htx_all_add(a, k, tmp, (size_t)kk, 0); }
                } else if (type == 1) htx_all_add(a, k, (const char *)p, strnlen((const char *)p, pn), fl);
            }
        }
        off += sz;
    }
    free(buf);
    return HT_OK;
}

/* ================================================================== deteccion y API */
static int htx_detect(int fd, const char *path, uint64_t fsize, uint64_t *flac_base)
{
    unsigned char h[12] = { 0 };
    *flac_base = 0;
    if (fsize < 4 || htx_pread(fd, h, fsize < 12 ? 4 : 12, 0)) return HT_FMT_NONE;
    if (!memcmp(h, "fLaC", 4)) return HT_FMT_FLAC;
    if (fsize >= 12 && !memcmp(h + 4, "ftyp", 4)) return HT_FMT_M4A;
    if (!memcmp(h, "ID3", 3) && fsize >= 10 && !((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        uint64_t te = 10 + (uint64_t)htx_ss32(h + 6) + (h[3] == 4 && (h[5] & 0x10) ? 10 : 0);
        unsigned char m[4];
        if (te + 4 <= fsize && !htx_pread(fd, m, 4, te) && !memcmp(m, "fLaC", 4)) { *flac_base = te; return HT_FMT_FLAC; }
        return HT_FMT_MP3;
    }
    if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return HT_FMT_MP3;
    const char *x = strrchr(path, '.');
    if (x && !strcasecmp(x, ".mp3")) return HT_FMT_MP3;
    return HT_FMT_NONE;
}

HT_API int ht_read(const char *path, ht_tags *t)
{
    memset(t, 0, sizeof *t);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return HT_EOPEN;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return HT_EOPEN; }
    uint64_t base;
    int e;
    t->fmt = htx_detect(fd, path, (uint64_t)st.st_size, &base);
    switch (t->fmt) {
    case HT_FMT_FLAC: e = htx_flac_read(fd, base, (uint64_t)st.st_size, t); break;
    case HT_FMT_MP3: e = htx_mp3_read(fd, (uint64_t)st.st_size, t); break;
    case HT_FMT_M4A: e = htx_m4a_read(fd, (uint64_t)st.st_size, t); break;
    default: e = HT_EFORMAT;
    }
    close(fd);
    return e;
}

HT_API int ht_read_all(const char *path, ht_all *a)
{
    memset(a, 0, sizeof *a);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return HT_EOPEN;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return HT_EOPEN; }
    uint64_t base;
    int e;
    a->fmt = htx_detect(fd, path, (uint64_t)st.st_size, &base);
    switch (a->fmt) {
    case HT_FMT_FLAC: e = htx_flac_read_all(fd, base, (uint64_t)st.st_size, a); break;
    case HT_FMT_MP3: e = htx_mp3_read_all(fd, (uint64_t)st.st_size, a); break;
    case HT_FMT_M4A: e = htx_m4a_read_all(fd, (uint64_t)st.st_size, a); break;
    default: e = HT_EFORMAT;
    }
    if (a->fmt == HT_FMT_M4A) for (int i = 0; i < a->n; i++) a->it[i].flags |= HT_ITEM_RO;
    close(fd);
    return e;
}

static int htx_write_eds(const char *path, const htx_eds *ed, ht_progress_fn cb, void *ud, int *mode)
{
    int dummy;
    if (!mode) mode = &dummy;
    *mode = HT_MODE_NONE;
    if (!ed->n) return HT_OK;
    int fd = open(path, O_RDWR);
    if (fd < 0) return HT_EOPEN;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return HT_EOPEN; }
    if (strlen(path) < PATH_MAX) {                 /* temporal huerfano de un corte anterior */
        char tmp[PATH_MAX + 32];
        htx_tmpname(path, tmp, sizeof tmp);
        unlink(tmp);
    }
    uint64_t base;
    int e;
    switch (htx_detect(fd, path, (uint64_t)st.st_size, &base)) {
    case HT_FMT_FLAC: e = htx_flac_write(path, fd, &st, base, ed, cb, ud, mode); break;
    case HT_FMT_MP3: e = htx_mp3_write(path, fd, &st, ed, cb, ud, mode); break;
    case HT_FMT_M4A: e = HT_EUNSUP; break;
    default: e = HT_EFORMAT;
    }
    close(fd);
    return e;
}
static void htx_eds_from_changes(htx_eds *s, const ht_changes *ch)
{
    s->n = 0;
    for (int f = 0; f < HT_NFIELDS; f++) if (ch->v[f]) { snprintf(s->e[s->n].key, HTX_KEYMAX, "%s", htx_base_keys[f]); s->e[s->n++].val = ch->v[f]; }
}
HT_API int ht_write(const char *path, const ht_changes *ch, ht_progress_fn cb, void *ud, int *mode)
{
    htx_eds *ed = malloc(sizeof *ed);
    if (!ed) return HT_ENOMEM;
    htx_eds_from_changes(ed, ch);
    int e = htx_write_eds(path, ed, cb, ud, mode);
    free(ed);
    return e;
}
HT_API int ht_write_kv(const char *path, const ht_kv *kv, int nkv, ht_progress_fn cb, void *ud, int *mode)
{
    if (mode) *mode = HT_MODE_NONE;
    htx_eds *ed = malloc(sizeof *ed);
    if (!ed) return HT_ENOMEM;
    int e = htx_eds_build(ed, kv, nkv);
    if (!e) e = htx_write_eds(path, ed, cb, ud, mode);
    free(ed);
    return e;
}

typedef struct { ht_batch_fn cb; void *ud; int idx, n; uint64_t base, fsize, total; } htx_bctx;
static int htx_batch_prog(void *ud, uint64_t done, uint64_t total)
{
    htx_bctx *b = ud;
    /* el tamano nuevo puede diferir algo del original: se escala al tamano original */
    uint64_t d = total ? (uint64_t)((double)done / (double)total * (double)b->fsize) : 0;
    return b->cb ? b->cb(b->ud, b->idx, b->n, b->base + d, b->total) : 0;
}
static int htx_batch_eds(const char *const *paths, int n, const htx_eds *ed, ht_batch_fn cb, void *ud, int *res)
{
    htx_bctx b = { cb, ud, 0, n, 0, 0, 0 };
    uint64_t *sz = calloc(n ? n : 1, sizeof *sz);
    if (!sz) return HT_ENOMEM;
    for (int i = 0; i < n; i++) { struct stat st; if (!stat(paths[i], &st)) sz[i] = (uint64_t)st.st_size; b.total += sz[i]; }
    int fails = 0, cancel = 0;
    for (int i = 0; i < n; i++) {
        int r;
        if (cancel) r = HT_ECANCEL;
        else {
            b.idx = i; b.fsize = sz[i];
            if (cb && cb(ud, i, n, b.base, b.total)) { cancel = 1; r = HT_ECANCEL; }
            else r = htx_write_eds(paths[i], ed, htx_batch_prog, &b, 0);
            if (r == HT_ECANCEL) cancel = 1;
        }
        if (res) res[i] = r;
        if (r) fails++;
        b.base += sz[i];
    }
    if (cb && !cancel) cb(ud, n, n, b.total, b.total);
    free(sz);
    return cancel ? HT_ECANCEL : fails;
}
HT_API int ht_batch(const char *const *paths, int n, const ht_changes *ch, ht_batch_fn cb, void *ud, int *res)
{
    htx_eds *ed = malloc(sizeof *ed);
    if (!ed) return HT_ENOMEM;
    htx_eds_from_changes(ed, ch);
    int r = htx_batch_eds(paths, n, ed, cb, ud, res);
    free(ed);
    return r;
}
HT_API int ht_batch_kv(const char *const *paths, int n, const ht_kv *kv, int nkv, ht_batch_fn cb, void *ud, int *res)
{
    htx_eds *ed = malloc(sizeof *ed);
    if (!ed) return HT_ENOMEM;
    int r = htx_eds_build(ed, kv, nkv);
    if (r) { for (int i = 0; res && i < n; i++) res[i] = r; free(ed); return n; }
    r = htx_batch_eds(paths, n, ed, cb, ud, res);
    free(ed);
    return r;
}

/* ================================================================== nombres de fichero */
HT_API int ht_name_valid(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 240 || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    if (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.') return 0;
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    while (p < e) {
        if (*p < 0x20 || *p == 0x7F || strchr("\\/:*?\"<>|", *p)) return 0;
        if (htx_u8(&p, e) == 0xFFFD) return 0;           /* UTF-8 roto */
    }
    return 1;
}
HT_API void ht_name_clean(const char *in, char *out, size_t outsz)
{
    if (!outsz) return;
    size_t o = 0; int sp = 0;
    while (*in == ' ' || *in == '.') in++;
    const unsigned char *p = (const unsigned char *)in, *e = p + strlen(in);
    while (p < e && o + 5 < outsz) {
        const unsigned char *q = p;
        uint32_t u = htx_u8(&p, e);
        if (u == 0xFFFD && p - q == 1) continue;
        if (u < 0x20 || u == 0x7F || (u < 0x80 && strchr("\\/:*?\"<>|", (int)u))) u = u < 0x20 || u == 0x7F ? ' ' : '_';
        if (u == ' ') { if (sp || !o) continue; sp = 1; } else sp = 0;
        if (u < 0x80) out[o++] = (char)u; else { memcpy(out + o, q, (size_t)(p - q)); o += (size_t)(p - q); }
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = 0;
    htx_u8_trunc(out, 200);
    o = strlen(out);
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
    if (!o) snprintf(out, outsz, "_");
}
HT_API int ht_pattern_name(int pat, const char *track, const char *disc, int multidisc,
                           const char *artist, const char *title, const char *ext, char *out, size_t outsz)
{
    int tn = htx_lead_int0(track), dn = htx_lead_int0(disc);
    if (!title || !*title || !outsz) return 0;
    char num[24] = "", base[600], clean[256];
    if (tn > 0) { if (multidisc && dn > 0) snprintf(num, sizeof num, "%d-%02d", dn, tn); else snprintf(num, sizeof num, "%02d", tn); }
    if (pat == HT_PAT_ARTIST_TITLE) snprintf(base, sizeof base, "%s%s%s", artist && *artist ? artist : "", artist && *artist ? " - " : "", title);
    else if (pat == HT_PAT_NUMTITLE) snprintf(base, sizeof base, "%s%s%s", num, *num ? " " : "", title);
    else snprintf(base, sizeof base, "%s%s%s", num, *num ? " - " : "", title);
    ht_name_clean(base, clean, sizeof clean);
    if ((size_t)snprintf(out, outsz, "%s%s", clean, ext ? ext : "") >= outsz) return 0;
    return 1;
}
/* "carpeta/nombre.ext" -> "carpeta/nombre" + sufijo */
static int htx_stem_with(const char *path, const char *suffix, char *out, size_t outsz)
{
    const char *sl = strrchr(path, '/'), *dot = strrchr(path, '.');
    size_t n = dot && (!sl || dot > sl) ? (size_t)(dot - path) : strlen(path);
    return (size_t)snprintf(out, outsz, "%.*s%s", (int)n, path, suffix) < outsz;
}
/* 1 si a y b son el mismo fichero (renombrar solo mayusculas en FAT) */
static int htx_same(const char *a, const char *b)
{
    struct stat x, y;
    return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
static void htx_fsync_dir_of(const char *path)
{
    char dir[PATH_MAX];
    const char *sl = strrchr(path, '/');
    if (!sl) return;
    snprintf(dir, sizeof dir, "%.*s", (int)(sl - path) ? (int)(sl - path) : 1, path);
    int d = open(dir, O_RDONLY | O_DIRECTORY);
    if (d >= 0) { fsync(d); close(d); }
}
static const char *htx_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
HT_API int ht_rename_song(const char *from, const char *to)
{
    if (!ht_name_valid(htx_base(to))) return HT_ENAME;
    if (!strcmp(from, to)) return HT_OK;
    struct stat st;
    if (lstat(from, &st) || !S_ISREG(st.st_mode)) return HT_EOPEN;
    if (!access(to, F_OK) && !htx_same(from, to)) return HT_EEXIST;
    /* letra: <raiz>.lrc o .LRC */
    char lf[PATH_MAX], lt[PATH_MAX];
    int lrc = 0;
    static const char *const lx[2] = { ".lrc", ".LRC" };
    for (int k = 0; k < 2 && !lrc; k++)
        if (htx_stem_with(from, lx[k], lf, sizeof lf) && !access(lf, F_OK) && htx_stem_with(to, lx[k], lt, sizeof lt)) lrc = 1;
    if (lrc && strcmp(lf, lt) && !access(lt, F_OK) && !htx_same(lf, lt)) return HT_EEXIST;
    if (rename(from, to)) return htx_err_errno();
    if (lrc && strcmp(lf, lt) && rename(lf, lt)) {
        int e = htx_err_errno();
        if (rename(to, from)) return HT_EIO;           /* no deberia pasar */
        return e;
    }
    htx_fsync_dir_of(to);
    return HT_OK;
}
HT_API int ht_rename_dir(const char *from, const char *to, int rm_parent)
{
    if (!ht_name_valid(htx_base(to))) return HT_ENAME;
    if (!strcmp(from, to)) return HT_OK;
    struct stat st;
    if (lstat(from, &st) || !S_ISDIR(st.st_mode)) return HT_EOPEN;
    size_t fl = strlen(from);
    if (!strncmp(to, from, fl) && to[fl] == '/') return HT_ENAME;          /* dentro de si misma */
    if (!access(to, F_OK) && !htx_same(from, to)) return HT_EEXIST;
    char par[PATH_MAX], opar[PATH_MAX];
    const char *sl = strrchr(to, '/');
    int made = 0;
    if (sl && sl > to) {
        snprintf(par, sizeof par, "%.*s", (int)(sl - to), to);
        if (access(par, F_OK)) { if (mkdir(par, 0755)) return htx_err_errno(); made = 1; }
    }
    if (rename(from, to)) { int e = htx_err_errno(); if (made) rmdir(par); return e; }
    htx_fsync_dir_of(to);
    if (rm_parent && (sl = strrchr(from, '/')) && sl > from) {
        snprintf(opar, sizeof opar, "%.*s", (int)(sl - from), from);
        if (rmdir(opar) == 0) htx_fsync_dir_of(opar);                         /* solo si quedo vacia */
    }
    return HT_OK;
}

/* ================================================================== caratula (portada)
 * Siempre por reescritura completa (temporal + fsync + rename): la imagen se copia en
 * streaming desde el JPEG (nunca se carga entera), el audio y el resto de etiquetas se copian
 * byte a byte. Se sustituye solo la portada (FLAC PICTURE tipo 3 / ID3 APIC tipo 3 / M4A covr). */
typedef struct { int fd; uint32_t len, w, h, depth; } htx_img;

/* valida el JPEG y saca ancho/alto/profundidad del SOFn */
static int htx_img_open(const char *jp, htx_img *im)
{
    memset(im, 0, sizeof *im);
    im->fd = open(jp, O_RDONLY);
    if (im->fd < 0) return HT_EOPEN;
    struct stat st;
    if (fstat(im->fd, &st) || !S_ISREG(st.st_mode)) return HT_EOPEN;
    if (st.st_size > HT_COVER_MAX) return HT_ETOOBIG;
    im->len = (uint32_t)st.st_size;
    unsigned char b[10];
    if (im->len < 4 || htx_pread(im->fd, b, 3, 0) || b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) return HT_EIMAGE;
    uint64_t p = 2;
    while (p + 4 <= im->len) {
        if (htx_pread(im->fd, b, 2, p)) return HT_EIMAGE;
        if (b[0] != 0xFF) return HT_EIMAGE;
        if (b[1] == 0xFF) { p++; continue; }                    /* relleno */
        int m = b[1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
        if (m == 0xD9 || m == 0xDA) return HT_EIMAGE;         /* sin SOF antes de los datos */
        if (htx_pread(im->fd, b, 2, p + 2)) return HT_EIMAGE;
        uint32_t l = (uint32_t)b[0] << 8 | b[1];
        if (l < 2) return HT_EIMAGE;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (l < 8 || htx_pread(im->fd, b, 6, p + 4)) return HT_EIMAGE;
            im->h = (uint32_t)b[1] << 8 | b[2]; im->w = (uint32_t)b[3] << 8 | b[4];
            im->depth = (uint32_t)b[0] * b[5];
            if (!im->w || !im->h) return HT_EIMAGE;
            return HT_OK;
        }
        p += 2 + l;
    }
    return HT_EIMAGE;
}

/* copia la imagen al sumidero en trozos (usa el bufer de copia del sumidero) */
static void htx_put_img(htx_sink *s, const htx_img *im)
{
    int src = s->src;
    s->src = im->fd;
    sk_copy(s, 0, im->len);
    s->src = src;
}

/* ---- FLAC: bloque PICTURE nuevo; se quitan las portadas viejas y el relleno */
#define HTX_PICHDR 42u   /* tipo, mime(4+10), desc(4), w, h, depth, colores, longitud */
typedef struct { htx_flac *f; uint64_t fsize; const htx_img *im; const unsigned char *drop; int iv; } htx_fcw;

static void htx_flac_cover_emit(htx_sink *s, void *ctx)
{
    htx_fcw *w = ctx;
    htx_flac *f = w->f;
    unsigned char b[32];
    sk_copy(s, 0, f->base + 4);
    for (int j = 0; j < f->nb; j++) {
        if (j == w->iv || f->b[j].type == 1 || w->drop[j]) continue;
        htx_flac_hdr(s, f->b[j].type, 0, f->b[j].len);
        sk_copy(s, f->b[j].off + 4, f->b[j].len);
    }
    htx_flac_hdr(s, 6, 0, HTX_PICHDR + w->im->len);
    htx_put_be32(b, 3); htx_put_be32(b + 4, 10); sk_put(s, b, 8);
    sk_put(s, "image/jpeg", 10);
    htx_put_be32(b, 0); htx_put_be32(b + 4, w->im->w); htx_put_be32(b + 8, w->im->h);
    htx_put_be32(b + 12, w->im->depth); htx_put_be32(b + 16, 0); htx_put_be32(b + 20, w->im->len);
    sk_put(s, b, 24);
    htx_put_img(s, w->im);
    if (w->iv >= 0) {                 /* el comentario al final, junto al relleno (ediciones en sitio) */
        htx_flac_hdr(s, 4, 0, f->b[w->iv].len);
        sk_copy(s, f->b[w->iv].off + 4, f->b[w->iv].len);
    }
    htx_flac_hdr(s, 1, 1, HT_PAD);
    sk_zero(s, HT_PAD);
    sk_copy(s, f->audio, w->fsize - f->audio);
}

static int htx_flac_cover(const char *path, int fd, const struct stat *st, uint64_t base, const htx_img *im)
{
    htx_fcw w; memset(&w, 0, sizeof w);
    w.fsize = (uint64_t)st->st_size; w.im = im;
    w.f = malloc(sizeof *w.f);
    unsigned char *drop = calloc(HTX_MAXBLK, 1);
    int e = (!w.f || !drop) ? HT_ENOMEM : htx_flac_parse(fd, base, w.fsize, w.f);
    if (e) goto out;
    w.iv = htx_flac_find_vc(w.f);
    uint64_t ns = w.f->base + 4;
    for (int j = 0; j < w.f->nb; j++) {
        htx_blk *b = &w.f->b[j];
        if (b->type == 6 && b->len >= 4) {
            unsigned char t[4];
            if (htx_pread(fd, t, 4, b->off + 4)) { e = HT_EIO; goto out; }
            drop[j] = htx_be32(t) == 3;
        }
        if (b->type != 1 && !drop[j]) ns += 4 + (uint64_t)b->len;   /* incluye el comentario */
    }
    ns += 4 + HTX_PICHDR + im->len + 4 + HT_PAD + (w.fsize - w.f->audio);
    w.drop = drop;
    e = htx_rewrite(path, fd, st, ns, htx_flac_cover_emit, &w, 0, 0);
out:
    free(drop); free(w.f);
    return e;
}

/* ---- MP3: trama APIC nueva (tipo 3); se quitan las APIC de portada que se puedan leer */
typedef struct { htx_id3 *t; uint64_t fsize; const htx_img *im; const unsigned char *drop; int nver; uint32_t body; } htx_mcw;

static void htx_mp3_cover_emit(htx_sink *s, void *ctx)
{
    htx_mcw *w = ctx;
    htx_id3 *t = w->t;
    unsigned char h[10] = { 'I', 'D', '3', (unsigned char)w->nver, 0, (unsigned char)(t->ver ? t->flags & 0x20 : 0) };
    htx_put_ss32(h + 6, w->body);
    sk_put(s, h, 10);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < t->nf; i++) {
            if (w->drop[i] || (t->f[i].size > HTX_BIGFRAME) != !pass) continue;
            sk_copy(s, t->f[i].off, t->f[i].size);
        }
        if (pass == 0) {
            uint32_t dl = 14 + w->im->len;
            memcpy(h, "APIC", 4);
            if (w->nver == 4) htx_put_ss32(h + 4, dl); else htx_put_be32(h + 4, dl);
            h[8] = h[9] = 0;
            sk_put(s, h, 10);
            sk_put(s, "\0image/jpeg\0\3\0", 14);   /* Latin-1, mime, portada, sin descripcion */
            htx_put_img(s, w->im);
        }
    }
    sk_zero(s, HT_PAD);
    uint64_t a = t->ver ? t->tag_end : 0;
    sk_copy(s, a, w->fsize - a);                 /* audio + ID3v1 tal cual */
}

static int htx_mp3_cover(const char *path, int fd, const struct stat *st, const htx_img *im)
{
    htx_id3 t;
    htx_mcw w; memset(&w, 0, sizeof w);
    w.fsize = (uint64_t)st->st_size; w.t = &t; w.im = im;
    int e = htx_id3_head(fd, w.fsize, &t);
    if (e) return e;
    if (t.ver == 2 || (t.ver == 3 && (t.flags & 0x80))) return HT_EUNSUP;
    htx_rd r = { fd, 0, 0 };
    unsigned char *drop = 0;
    if (t.ver && (e = htx_id3_walk(&r, &t))) goto out;
    w.nver = t.ver ? t.ver : 3;
    drop = calloc(t.nf + 1, 1);
    if (!drop) { e = HT_ENOMEM; goto out; }
    uint64_t body = 10 + 14 + (uint64_t)im->len + HT_PAD;
    for (int i = 0; i < t.nf; i++) {
        unsigned char b[80];
        size_t n = t.f[i].size - 10 > sizeof b ? sizeof b : t.f[i].size - 10;
        if (htx_pread(fd, b, 10, t.f[i].off)) { e = HT_EIO; goto out; }
        if (!memcmp(b, "APIC", 4) && n > 4) {
            int fl = b[9], skip = 0, ok = 1;
            if (t.ver == 3) { if (fl & 0xC0) ok = 0; if (fl & 0x20) skip = 1; }
            else { if (fl & 0x0E) ok = 0; if (fl & 0x40) skip++; if (fl & 0x01) skip += 4; }
            if (ok && !htx_pread(fd, b, n, t.f[i].off + 10) && (size_t)skip + 2 < n) {
                const unsigned char *p = b + skip + 1, *z = memchr(p, 0, n - skip - 1);
                if (z && z + 1 < b + n && z[1] == 3) drop[i] = 1;
            }
        }
        if (!drop[i]) body += t.f[i].size;
    }
    if (body >= (1u << 28)) { e = HT_EUNSUP; goto out; }
    w.body = (uint32_t)body; w.drop = drop;
    uint64_t a = t.ver ? t.tag_end : 0;
    e = htx_rewrite(path, fd, st, 10 + body + (w.fsize - a), htx_mp3_cover_emit, &w, 0, 0);
out:
    free(drop); free(t.f);
    return e;
}

/* ---- M4A/MP4: moov/udta/meta/ilst/covr (data tipo 13 = JPEG). Si moov crece y va antes
 * de mdat, se corrigen los desplazamientos de stco/co64 (se recorren en streaming). */
typedef struct { uint64_t off, sz; } htx_box;
typedef struct {
    int fd; uint64_t fsize; const htx_img *im;
    htx_box moov, udta, meta, ilst, covr;      /* sz 0 = no existe */
    uint64_t moov_n, udta_n, meta_n, ilst_n, covr_n, shift_from; int64_t delta;
    unsigned char *pb;
} htx_m4w;

/* cabecera de caja en off (dentro de [off,end)): tipo y tamano; solo cabeceras de 8 bytes */
static int htx_box_at(int fd, uint64_t off, uint64_t end, uint32_t *type, uint64_t *sz, int strict)
{
    unsigned char h[16];
    if (off + 8 > end || htx_pread(fd, h, 8, off)) return -1;
    uint64_t s = htx_be32(h);
    *type = htx_be32(h + 4);
    if (s == 1) {
        if (strict || off + 16 > end || htx_pread(fd, h + 8, 8, off + 8)) return -1;
        s = (uint64_t)htx_be32(h + 8) << 32 | htx_be32(h + 12);
        if (s < 16) return -1;
    } else if (s == 0) { if (strict) return -1; s = end - off; }
    else if (s < 8) return -1;
    if (off + s > end) return -1;
    *sz = s;
    return 0;
}
static int htx_box_find(int fd, uint64_t off, uint64_t end, uint32_t type, htx_box *b)
{
    uint32_t t; uint64_t s;
    b->off = b->sz = 0;
    while (off < end) {
        if (htx_box_at(fd, off, end, &t, &s, 1)) return HT_EUNSUP;
        if (t == type) { b->off = off; b->sz = s; return HT_OK; }
        off += s;
    }
    return HT_OK;
}
static void htx_box_hdr(htx_sink *s, uint64_t sz, const char *type)
{
    unsigned char h[8];
    htx_put_be32(h, (uint32_t)sz); memcpy(h + 4, type, 4);
    sk_put(s, h, 8);
}
/* stco/co64 con desplazamientos corregidos */
static void htx_m4a_chunks(htx_sink *s, htx_m4w *w, uint64_t off, uint64_t sz, int wide)
{
    unsigned char h[16];
    if (sz < 16 || htx_pread(w->fd, h, 16, off)) { s->err = HT_EFORMAT; return; }
    uint64_t cnt = htx_be32(h + 12), es = wide ? 8 : 4;
    if (16 + cnt * es != sz) { s->err = HT_EUNSUP; return; }
    sk_put(s, h, 16);
    uint64_t p = off + 16, left = cnt * es;
    while (left && !s->err) {
        size_t k = left > 16384 ? 16384 : (size_t)left;
        if (htx_pread(w->fd, w->pb, k, p)) { s->err = HT_EIO; return; }
        for (size_t i = 0; i < k; i += es) {
            uint64_t v = wide ? (uint64_t)htx_be32(w->pb + i) << 32 | htx_be32(w->pb + i + 4) : htx_be32(w->pb + i);
            if (v >= w->shift_from) {
                v = (uint64_t)((int64_t)v + w->delta);
                if (!wide && v > 0xFFFFFFFFu) { s->err = HT_EUNSUP; return; }
            }
            if (wide) { htx_put_be32(w->pb + i, (uint32_t)(v >> 32)); htx_put_be32(w->pb + i + 4, (uint32_t)v); }
            else htx_put_be32(w->pb + i, (uint32_t)v);
        }
        sk_put(s, w->pb, k);
        p += k; left -= k;
    }
}
/* copia [off,end) bajando por trak/mdia/minf/stbl para corregir stco/co64 */
static void htx_m4a_tree(htx_sink *s, htx_m4w *w, uint64_t off, uint64_t end)
{
    while (off < end && !s->err) {
        uint32_t t; uint64_t sz;
        if (htx_box_at(w->fd, off, end, &t, &sz, 0)) { s->err = HT_EFORMAT; return; }
        if (t == HTX_FCC('t', 'r', 'a', 'k') || t == HTX_FCC('m', 'd', 'i', 'a') ||
            t == HTX_FCC('m', 'i', 'n', 'f') || t == HTX_FCC('s', 't', 'b', 'l')) {
            unsigned char h[8];
            if (htx_pread(w->fd, h, 8, off) || htx_be32(h) != sz) { s->err = HT_EUNSUP; return; }
            sk_put(s, h, 8);
            htx_m4a_tree(s, w, off + 8, off + sz);
        } else if (t == HTX_FCC('s', 't', 'c', 'o') || t == HTX_FCC('c', 'o', '6', '4')) {
            htx_m4a_chunks(s, w, off, sz, t == HTX_FCC('c', 'o', '6', '4'));
        } else sk_copy(s, off, sz);
        off += sz;
    }
}
static void htx_m4a_covr(htx_sink *s, htx_m4w *w)
{
    unsigned char b[8];
    htx_box_hdr(s, w->covr_n, "covr");
    htx_box_hdr(s, 16 + w->im->len, "data");
    htx_put_be32(b, 13); htx_put_be32(b + 4, 0); sk_put(s, b, 8);
    htx_put_img(s, w->im);
}
/* copia los hijos de [off,end) salvo skip (y, si se da, llama a fn en su lugar) */
static void htx_m4a_kids(htx_sink *s, htx_m4w *w, uint64_t off, uint64_t end, uint64_t skip, void (*fn)(htx_sink *, htx_m4w *))
{
    while (off < end && !s->err) {
        uint32_t t; uint64_t sz;
        if (htx_box_at(w->fd, off, end, &t, &sz, 1)) { s->err = HT_EFORMAT; return; }
        if (skip && off == skip) { if (fn) fn(s, w); }
        else sk_copy(s, off, sz);
        off += sz;
    }
}
static void htx_m4a_ilst(htx_sink *s, htx_m4w *w)
{
    htx_box_hdr(s, w->ilst_n, "ilst");
    if (w->ilst.sz) htx_m4a_kids(s, w, w->ilst.off + 8, w->ilst.off + w->ilst.sz, w->covr.sz ? w->covr.off : 0, 0);
    htx_m4a_covr(s, w);
}
static void htx_m4a_meta(htx_sink *s, htx_m4w *w)
{
    htx_box_hdr(s, w->meta_n, "meta");
    if (w->meta.sz) {
        sk_copy(s, w->meta.off + 8, 4);
        htx_m4a_kids(s, w, w->meta.off + 12, w->meta.off + w->meta.sz, w->ilst.sz ? w->ilst.off : 0, htx_m4a_ilst);
        if (!w->ilst.sz) htx_m4a_ilst(s, w);
    } else {
        static const unsigned char hdlr[37] = { 0, 0, 0, 0, 0, 0, 0, 33, 'h', 'd', 'l', 'r', 0, 0, 0, 0, 0, 0, 0, 0,
                                                'm', 'd', 'i', 'r', 'a', 'p', 'p', 'l', 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        sk_put(s, hdlr, 37);                    /* version/flags de meta + hdlr "mdir/appl" */
        htx_m4a_ilst(s, w);
    }
}
static void htx_m4a_udta(htx_sink *s, htx_m4w *w)
{
    htx_box_hdr(s, w->udta_n, "udta");
    if (w->udta.sz) {
        htx_m4a_kids(s, w, w->udta.off + 8, w->udta.off + w->udta.sz, w->meta.sz ? w->meta.off : 0, htx_m4a_meta);
        if (!w->meta.sz) htx_m4a_meta(s, w);
    } else htx_m4a_meta(s, w);
}
static void htx_m4a_cover_emit(htx_sink *s, void *ctx)
{
    htx_m4w *w = ctx;
    uint64_t mo = w->moov.off, me = mo + w->moov.sz;
    sk_copy(s, 0, mo);
    htx_box_hdr(s, w->moov_n, "moov");
    uint64_t off = mo + 8;
    while (off < me && !s->err) {
        uint32_t t; uint64_t sz;
        if (htx_box_at(w->fd, off, me, &t, &sz, 1)) { s->err = HT_EFORMAT; return; }
        if (w->udta.sz && off == w->udta.off) htx_m4a_udta(s, w);
        else if (t == HTX_FCC('t', 'r', 'a', 'k')) htx_m4a_tree(s, w, off, off + sz);
        else sk_copy(s, off, sz);
        off += sz;
    }
    if (!w->udta.sz) htx_m4a_udta(s, w);
    sk_copy(s, me, w->fsize - me);
}

static int htx_m4a_cover(const char *path, int fd, const struct stat *st, const htx_img *im)
{
    htx_m4w w; memset(&w, 0, sizeof w);
    w.fd = fd; w.fsize = (uint64_t)st->st_size; w.im = im;
    /* cajas de primer nivel: moov (cabecera de 8 bytes) y nada fragmentado */
    uint64_t off = 0; int nmoov = 0, frag = 0;
    while (off < w.fsize) {
        uint32_t t; uint64_t sz;
        if (htx_box_at(fd, off, w.fsize, &t, &sz, 0)) return HT_EFORMAT;
        if (t == HTX_FCC('m', 'o', 'o', 'v')) {
            unsigned char h[4];
            if (htx_pread(fd, h, 4, off) || htx_be32(h) != sz) return HT_EUNSUP;
            w.moov.off = off; w.moov.sz = sz; nmoov++;
        }
        if (t == HTX_FCC('m', 'o', 'o', 'f') || t == HTX_FCC('s', 'i', 'd', 'x')) frag = 1;
        off += sz;
    }
    if (nmoov != 1) return HT_EFORMAT;
    uint64_t mo = w.moov.off, me = mo + w.moov.sz;
    int e;
    if ((e = htx_box_find(fd, mo + 8, me, HTX_FCC('u', 'd', 't', 'a'), &w.udta))) return e;
    if (w.udta.sz && (e = htx_box_find(fd, w.udta.off + 8, w.udta.off + w.udta.sz, HTX_FCC('m', 'e', 't', 'a'), &w.meta))) return e;
    if (w.meta.sz && (e = htx_box_find(fd, w.meta.off + 12, w.meta.off + w.meta.sz, HTX_FCC('i', 'l', 's', 't'), &w.ilst))) return e;
    if (w.ilst.sz && (e = htx_box_find(fd, w.ilst.off + 8, w.ilst.off + w.ilst.sz, HTX_FCC('c', 'o', 'v', 'r'), &w.covr))) return e;
    if (w.meta.sz && w.meta.sz < 12) return HT_EFORMAT;
    w.covr_n = 8 + 16 + (uint64_t)im->len;
    w.ilst_n = w.ilst.sz ? w.ilst.sz - w.covr.sz + w.covr_n : 8 + w.covr_n;
    w.meta_n = w.meta.sz ? w.meta.sz - w.ilst.sz + w.ilst_n : 12 + 33 + w.ilst_n;
    w.udta_n = w.udta.sz ? w.udta.sz - w.meta.sz + w.meta_n : 8 + w.meta_n;
    w.moov_n = w.moov.sz - w.udta.sz + w.udta_n;
    if (w.moov_n > 0xFFFFFFFFu) return HT_EUNSUP;
    w.delta = (int64_t)w.moov_n - (int64_t)w.moov.sz;
    w.shift_from = me;
    if (w.delta && me < w.fsize && frag) return HT_EUNSUP;      /* MP4 fragmentado */
    w.pb = malloc(16384);
    if (!w.pb) return HT_ENOMEM;
    e = htx_rewrite(path, fd, st, (uint64_t)((int64_t)w.fsize + w.delta), htx_m4a_cover_emit, &w, 0, 0);
    free(w.pb);
    return e;
}

HT_API int ht_set_cover(const char *path, const char *jpeg_path)
{
    htx_img im;
    int e = htx_img_open(jpeg_path, &im);
    if (e) { if (im.fd >= 0) close(im.fd); return e; }
    int fd = open(path, O_RDWR);
    if (fd < 0) { close(im.fd); return HT_EOPEN; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); close(im.fd); return HT_EOPEN; }
    if (strlen(path) < PATH_MAX) {
        char tmp[PATH_MAX + 32];
        htx_tmpname(path, tmp, sizeof tmp);
        unlink(tmp);
    }
    uint64_t base;
    unsigned char m[4] = { 0 };
    switch (htx_detect(fd, path, (uint64_t)st.st_size, &base)) {
    case HT_FMT_FLAC: e = htx_flac_cover(path, fd, &st, base, &im); break;
    case HT_FMT_MP3: e = htx_mp3_cover(path, fd, &st, &im); break;
    case HT_FMT_M4A: e = htx_m4a_cover(path, fd, &st, &im); break;
    default:   /* Ogg (Vorbis/Opus): la libreria no reescribe Ogg -> no soportado */
        e = (!htx_pread(fd, m, 4, 0) && !memcmp(m, "OggS", 4)) ? HT_EUNSUP : HT_EFORMAT;
    }
    close(fd); close(im.fd);
    return e;
}

/* ================================================================== base del reproductor */
#ifndef HIBYTAG_NO_DB
static const char *htx_sd_root(void) { const char *e = getenv("HIBYTAG_SD"); return e && *e ? e : "/usr/data/mnt/sd_0"; }

HT_API sqlite3 *ht_db_open(const char *dbpath)
{
    if (!dbpath) { dbpath = getenv("HIBYTAG_DB"); if (!dbpath || !*dbpath) dbpath = "/usr/data/usrlocal_media.db"; }
    sqlite3 *db = 0;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READWRITE, 0)) { sqlite3_close(db); return 0; }
    sqlite3_busy_timeout(db, 5000);
    return db;
}
HT_API void ht_db_close(sqlite3 *db) { if (db) sqlite3_close(db); }

static int htx_db_map(const char *file, const char *root, char *out, size_t outsz)
{
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') rl--;
    if (strncmp(file, root, rl) || file[rl] != '/') return 0;
    if ((size_t)snprintf(out, outsz, "a:%s", file + rl) >= outsz) return 0;
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
    return 1;
}
HT_API int ht_db_path(const char *file, char *out, size_t outsz)
{
    const char *root = htx_sd_root();
    if (htx_db_map(file, root, out, outsz)) return 1;
    char rf[PATH_MAX], rr[PATH_MAX];
    if (!realpath(file, rf)) return 0;
    if (htx_db_map(rf, root, out, outsz)) return 1;
    if (realpath(root, rr) && htx_db_map(rf, rr, out, outsz)) return 1;
    return 0;
}

/* mayuscula de un punto de codigo (ASCII, Latin-1, griego, cirilico) */
static uint32_t htx_upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x100 && c <= 0x17F && (c & 1) && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        /* Latin Extendido-A: pares mayuscula(par)/minuscula(impar), salvo el tramo 0x139-0x148 y 0x179-0x17E */
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c;
        return c - 1;
    }
    if (c >= 0x139 && c <= 0x148 && !(c & 1)) return c - 1;
    if (c >= 0x179 && c <= 0x17E && !(c & 1)) return c - 1;
    return c;
}
/* letra para el indice (columna character): primer caracter en mayuscula, sin NUL */
static void htx_character(const char *name, char *out)
{
    const unsigned char *p = (const unsigned char *)name, *e = p + strlen(name);
    htx_str s = { 0 };
    if (p < e) hs_cp(&s, htx_upper(htx_u8(&p, e)));
    snprintf(out, 8, "%s", s.p ? s.p : "");
    free(s.p);
}
/* pinyin_charater: el nombre con las letras ASCII en mayuscula (asi lo guarda el reproductor) */
static char *htx_pinyin(const char *s)
{
    char *r = strdup(s);
    if (r) for (char *c = r; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    return r;
}

/* conjunto de valores tocados (sin distinguir mayusculas, como COLLATE NOCASE) */
typedef struct { char **v; int n, cap; } htx_vset;
static void htx_vset_add(htx_vset *s, const char *v)
{
    if (!v || !*v) return;
    for (int i = 0; i < s->n; i++) if (!strcasecmp(s->v[i], v)) return;
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        char **q = realloc(s->v, nc * sizeof *q);
        if (!q) return;
        s->v = q; s->cap = nc;
    }
    if ((s->v[s->n] = strdup(v))) s->n++;
}
static void htx_vset_free(htx_vset *s) { for (int i = 0; i < s->n; i++) free(s->v[i]); free(s->v); memset(s, 0, sizeof *s); }

static void htx_bind_z(sqlite3_stmt *q, int i, const char *s) { sqlite3_bind_text(q, i, s, (int)strlen(s) + 1, SQLITE_TRANSIENT); }

/* lista de tablas con el esquema de pistas (path, name, artist, album...) y su UPDATE */
#define HTX_MAXTAB 64
typedef struct { char name[64]; sqlite3_stmt *upd; } htx_tab;
static const char *const htx_cols[] = { "name", "artist", "album", "album_artist", "genre", "year", "character", "pinyin_charater", "ck_id", "dis_id", "size" };
#define HTX_NCOLS ((int)(sizeof htx_cols / sizeof *htx_cols))

static int htx_db_tables(sqlite3 *db, htx_tab *tab)
{
    sqlite3_stmt *q, *qi;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &q, 0)) return 0;
    while (sqlite3_step(q) == SQLITE_ROW && n < HTX_MAXTAB) {
        const char *tn = (const char *)sqlite3_column_text(q, 0);
        if (!tn || strlen(tn) >= sizeof tab[n].name || strchr(tn, '"')) continue;
        char sql[256];
        snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", tn);
        if (sqlite3_prepare_v2(db, sql, -1, &qi, 0)) continue;
        int has[HTX_NCOLS] = { 0 }, haspath = 0;
        while (sqlite3_step(qi) == SQLITE_ROW) {
            const char *cn = (const char *)sqlite3_column_text(qi, 1);
            if (!cn) continue;
            if (!strcmp(cn, "path")) haspath = 1;
            for (int c = 0; c < HTX_NCOLS; c++) if (!strcmp(cn, htx_cols[c])) has[c] = 1;
        }
        sqlite3_finalize(qi);
        if (!haspath || !has[0] || !has[1] || !has[2]) continue;
        htx_str s = { 0 };
        hs_add(&s, "UPDATE \"", 8); hs_add(&s, tn, strlen(tn)); hs_add(&s, "\" SET ", 6);
        int first = 1;
        for (int c = 0; c < HTX_NCOLS; c++) {
            if (!has[c]) continue;
            char b[48];
            int k = snprintf(b, sizeof b, "%s%s=?%d", first ? "" : ",", htx_cols[c], c + 1);
            hs_add(&s, b, (size_t)k); first = 0;
        }
        char wb[32]; int k = snprintf(wb, sizeof wb, " WHERE path=?%d", HTX_NCOLS + 1);
        hs_add(&s, wb, (size_t)k);
        if (!s.oom && !sqlite3_prepare_v2(db, s.p, -1, &tab[n].upd, 0)) { strcpy(tab[n].name, tn); n++; }
        free(s.p);
    }
    sqlite3_finalize(q);
    return n;
}

static int htx_lead_int(const char *s)
{
    int n = 0, k = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9' && k < 9) { n = n * 10 + (*s++ - '0'); k++; }
    return n;
}

static int htx_db_exec1(sqlite3 *db, const char *sql, const char *v, int iv, int *out)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(db, sql, -1, &q, 0)) return -1;
    if (v) htx_bind_z(q, 1, v);
    if (iv >= 0) sqlite3_bind_int(q, v ? 2 : 1, iv);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW && out) *out = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

/* mantiene ARTIST/ALBUM/GENRE/ALBUM_ARTIST(_2)_TABLE para los valores tocados:
 * sin pistas -> se borra la fila; nueva -> se inserta como lo hace el reproductor
 * (id y fechas de la primera pista); ALBUM y GENRE llevan cn = numero de pistas */
static int htx_db_aggr(sqlite3 *db, const char *media, const char *sfx, htx_vset *sets)
{
    static const struct { const char *col, *tab; int mqa, cn_tracks; } K[4] = {
        { "artist", "ARTIST", 0, 0 }, { "album", "ALBUM", 1, 1 }, { "genre", "GENRE", 0, 1 }, { "album_artist", "ALBUM_ARTIST", 1, 0 } };
    for (int k = 0; k < 4; k++) {
        char agg[48], sql[512];
        snprintf(agg, sizeof agg, "%s%s_TABLE", K[k].tab, sfx);
        snprintf(sql, sizeof sql, "SELECT 1 FROM \"%s\" LIMIT 0", agg);
        if (htx_db_exec1(db, sql, 0, -1, 0)) continue;              /* no existe */
        for (int i = 0; i < sets[k].n; i++) {
            const char *v = sets[k].v[i];
            int cnt = 0, minid = 0, exists = 0;
            snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM \"%s\" WHERE %s=?", media, K[k].col);
            if (htx_db_exec1(db, sql, v, -1, &cnt)) return -1;
            if (!cnt) {
                snprintf(sql, sizeof sql, "DELETE FROM \"%s\" WHERE %s=?", agg, K[k].col);
                if (htx_db_exec1(db, sql, v, -1, 0)) return -1;
                continue;
            }
            snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM \"%s\" WHERE %s=?", agg, K[k].col);
            if (htx_db_exec1(db, sql, v, -1, &exists)) return -1;
            if (K[k].cn_tracks || !exists) {
                if (!K[k].cn_tracks) {
                    snprintf(sql, sizeof sql, "SELECT COUNT(DISTINCT album) FROM \"%s\" WHERE %s=?", media, K[k].col);
                    if (htx_db_exec1(db, sql, v, -1, &cnt)) return -1;
                }
            }
            if (exists) {
                if (!K[k].cn_tracks) continue;
                snprintf(sql, sizeof sql, "UPDATE \"%s\" SET cn=?2 WHERE %s=?1", agg, K[k].col);
                if (htx_db_exec1(db, sql, v, cnt, 0)) return -1;
                continue;
            }
            snprintf(sql, sizeof sql, "SELECT MIN(id) FROM \"%s\" WHERE %s=?", media, K[k].col);
            if (htx_db_exec1(db, sql, v, -1, &minid)) return -1;
            snprintf(sql, sizeof sql,
                     "INSERT OR REPLACE INTO \"%s\"(id,%s,character,cn,ctime,mtime,%spinyin_charater) "
                     "SELECT ?2,?1,?3,?4,ctime,mtime,%s?5 FROM \"%s\" WHERE id=?2 LIMIT 1",
                     agg, K[k].col, K[k].mqa ? "mqa," : "", K[k].mqa ? "0," : "", media);
            sqlite3_stmt *q;
            if (sqlite3_prepare_v2(db, sql, -1, &q, 0)) return -1;
            char ch[8]; htx_character(v, ch);
            char *py = htx_pinyin(v);
            htx_bind_z(q, 1, v);
            sqlite3_bind_int(q, 2, minid);
            sqlite3_bind_text(q, 3, ch, (int)strlen(ch), SQLITE_TRANSIENT);
            sqlite3_bind_int(q, 4, cnt);
            htx_bind_z(q, 5, py ? py : v);
            int rc = sqlite3_step(q);
            sqlite3_finalize(q); free(py);
            if (rc != SQLITE_DONE) return -1;
        }
    }
    return 0;
}

HT_API int ht_db_update(sqlite3 *db, const char *const *paths, int n, int *rows)
{
    htx_tab *tab = calloc(HTX_MAXTAB, sizeof *tab);
    if (!tab) return HT_ENOMEM;
    int nt = htx_db_tables(db, tab), missing = 0, nrows = 0, err = 0;
    htx_vset sets[4]; memset(sets, 0, sizeof sets);
    static const char *const oldq[2] = {
        "SELECT artist, album, genre, album_artist FROM MEDIA_TABLE WHERE path=?",
        "SELECT artist, album, genre, album_artist FROM MEDIA2_TABLE WHERE path=?" };
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0)) { err = 1; goto out; }
    for (int i = 0; i < n && !err; i++) {
        char dbp[PATH_MAX + 4];
        ht_tags t; memset(&t, 0, sizeof t);
        if (!ht_db_path(paths[i], dbp, sizeof dbp) || ht_read(paths[i], &t)) { missing++; ht_tags_free(&t); continue; }
        /* valores como los guarda el reproductor */
        char stem[512];
        const char *b = strrchr(paths[i], '/'); b = b ? b + 1 : paths[i];
        snprintf(stem, sizeof stem, "%s", b);
        char *dot = strrchr(stem, '.'); if (dot && dot != stem) *dot = 0;
        const char *name = t.v[HT_TITLE] && *t.v[HT_TITLE] ? t.v[HT_TITLE] : stem;
        const char *artist = t.v[HT_ARTIST] && *t.v[HT_ARTIST] ? t.v[HT_ARTIST] : "Unknown";
        const char *album = t.v[HT_ALBUM] && *t.v[HT_ALBUM] ? t.v[HT_ALBUM] : "Unknown";
        const char *genre = t.v[HT_GENRE] && *t.v[HT_GENRE] ? t.v[HT_GENRE] : "Unknown";
        const char *aa = t.v[HT_ALBUMARTIST] ? t.v[HT_ALBUMARTIST] : "";
        char ch[8]; htx_character(name, ch);
        char *py = htx_pinyin(name);
        struct stat st; long long fsz = stat(paths[i], &st) ? 0 : (long long)st.st_size;
        /* valores viejos (para las tablas de artistas/albumes...) */
        for (int m = 0; m < 2; m++) {
            sqlite3_stmt *q;
            if (sqlite3_prepare_v2(db, oldq[m], -1, &q, 0)) continue;
            htx_bind_z(q, 1, dbp);
            while (sqlite3_step(q) == SQLITE_ROW)
                for (int k = 0; k < 4; k++) htx_vset_add(&sets[k], (const char *)sqlite3_column_text(q, k));
            sqlite3_finalize(q);
        }
        const char *nv[4] = { artist, album, genre, aa };
        for (int k = 0; k < 4; k++) htx_vset_add(&sets[k], nv[k]);
        int found = 0;
        for (int k = 0; k < nt; k++) {
            sqlite3_stmt *q = tab[k].upd;
            sqlite3_reset(q); sqlite3_clear_bindings(q);
            htx_bind_z(q, 1, name); htx_bind_z(q, 2, artist); htx_bind_z(q, 3, album); htx_bind_z(q, 4, aa);
            htx_bind_z(q, 5, genre);
            sqlite3_bind_int(q, 6, htx_lead_int(t.v[HT_YEAR]));
            sqlite3_bind_text(q, 7, ch, (int)strlen(ch), SQLITE_TRANSIENT);
            htx_bind_z(q, 8, py ? py : name);
            sqlite3_bind_int(q, 9, htx_lead_int(t.v[HT_TRACK]));
            sqlite3_bind_int(q, 10, htx_lead_int(t.v[HT_DISC]));
            sqlite3_bind_int64(q, 11, fsz);
            htx_bind_z(q, HTX_NCOLS + 1, dbp);
            if (sqlite3_step(q) != SQLITE_DONE) { err = 1; break; }
            int c = sqlite3_changes(db);
            nrows += c;
            if (c && !strcmp(tab[k].name, "MEDIA_TABLE")) found = 1;
        }
        free(py);
        ht_tags_free(&t);
        if (!found) missing++;
    }
    if (!err && (htx_db_aggr(db, "MEDIA_TABLE", "", sets) || htx_db_aggr(db, "MEDIA2_TABLE", "2", sets))) err = 1;
    if (!err) {
        /* COUNT_TABLE: 1 pistas, 2 albumes, 3 artistas, 4 generos, 5 artistas de album */
        static const char *const cq[] = {
            "UPDATE COUNT_TABLE SET cn=(SELECT COUNT(*) FROM ALBUM_TABLE) WHERE rowid=2",
            "UPDATE COUNT_TABLE SET cn=(SELECT COUNT(*) FROM ARTIST_TABLE) WHERE rowid=3",
            "UPDATE COUNT_TABLE SET cn=(SELECT COUNT(*) FROM GENRE_TABLE) WHERE rowid=4",
            "UPDATE COUNT_TABLE SET cn=(SELECT COUNT(*) FROM ALBUM_ARTIST_TABLE) WHERE rowid=5" };
        int rowsct = 0;
        htx_db_exec1(db, "SELECT COUNT(*) FROM COUNT_TABLE", 0, -1, &rowsct);
        if (rowsct >= 5) for (int k = 0; k < 4; k++) sqlite3_exec(db, cq[k], 0, 0, 0);
    }
    if (err) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    else if (sqlite3_exec(db, "COMMIT", 0, 0, 0)) { sqlite3_exec(db, "ROLLBACK", 0, 0, 0); err = 1; }
out:
    for (int k = 0; k < nt; k++) sqlite3_finalize(tab[k].upd);
    free(tab);
    for (int k = 0; k < 4; k++) htx_vset_free(&sets[k]);
    if (rows) *rows = err ? 0 : nrows;
    return err ? HT_EDB : missing;
}

/* minusculas y sin acentos (para comparar al autocompletar) */
static void htx_fold(const char *s, char *out, size_t outsz)
{
    static const char lat[] = "aaaaaaaceeeeiiiidnooooo\0ouuuuyts" "aaaaaaaceeeeiiiidnooooo\0ouuuuyty";
    const unsigned char *p = (const unsigned char *)s, *e = p + strlen(s);
    htx_str o = { 0 };
    while (p < e && o.n + 4 < outsz) {
        uint32_t c = htx_u8(&p, e);
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0xC0 && c <= 0xFF && lat[c - 0xC0]) c = (unsigned char)lat[c - 0xC0];
        else if (c >= 0x391 && c <= 0x3A9) c += 32;
        else if (c >= 0x410 && c <= 0x42F) c += 32;
        else if (c >= 0x400 && c <= 0x40F) c += 80;
        hs_cp(&o, c);
    }
    snprintf(out, outsz, "%s", o.p ? o.p : "");
    free(o.p);
}

HT_API int ht_db_suggest(sqlite3 *db, int field, const char *prefix, char out[][HT_SUGLEN], int max)
{
    const char *col = field == HT_ARTIST ? "artist" : field == HT_ALBUM ? "album" : field == HT_GENRE ? "genre" :
                      field == HT_ALBUMARTIST ? "album_artist" : 0;
    if (!col || max <= 0) return 0;
    char sql[160], fp[HT_SUGLEN * 2], fv[512];
    snprintf(sql, sizeof sql, "SELECT %s, COUNT(*) AS c FROM MEDIA_TABLE GROUP BY %s ORDER BY c DESC, %s", col, col, col);
    htx_fold(prefix ? prefix : "", fp, sizeof fp);
    size_t pl = strlen(fp);
    char (*cont)[HT_SUGLEN] = malloc((size_t)max * HT_SUGLEN);
    if (!cont) return 0;
    int np = 0, nc = 0;
    sqlite3_stmt *q;
    if (!sqlite3_prepare_v2(db, sql, -1, &q, 0)) {
        while (np < max && sqlite3_step(q) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(q, 0);      /* corta en el NUL final */
            if (!v || !*v || !strcmp(v, "Unknown")) continue;
            htx_fold(v, fv, sizeof fv);
            if (!strncmp(fv, fp, pl)) { snprintf(out[np], HT_SUGLEN, "%s", v); htx_u8_trunc(out[np], HT_SUGLEN - 1); np++; }
            else if (nc < max && strstr(fv, fp)) { snprintf(cont[nc], HT_SUGLEN, "%s", v); htx_u8_trunc(cont[nc], HT_SUGLEN - 1); nc++; }
        }
        sqlite3_finalize(q);
    }
    for (int i = 0; i < nc && np < max; i++) memcpy(out[np++], cont[i], HT_SUGLEN);
    free(cont);
    return np;
}

/* ruta de la base de algo que quiza ya no existe (se resuelve la carpeta madre) */
static int htx_db_path_any(const char *file, char *out, size_t outsz)
{
    if (htx_db_map(file, htx_sd_root(), out, outsz) || ht_db_path(file, out, outsz)) return 1;
    const char *sl = strrchr(file, '/');
    char dir[PATH_MAX], rd[PATH_MAX], full[PATH_MAX * 2];
    if (!sl || sl == file) return 0;
    snprintf(dir, sizeof dir, "%.*s", (int)(sl - file), file);
    if (!realpath(dir, rd)) return 0;
    snprintf(full, sizeof full, "%s%s", rd, sl);
    return ht_db_path(full, out, outsz);
}

/* a:\X\cancion.flac -> a:\X\cancion.lrc */
static void htx_db_lrc(const char *p, char *out, size_t outsz)
{
    const char *bs = strrchr(p, '\\'), *dot = strrchr(p, '.');
    size_t n = dot && (!bs || dot > bs) ? (size_t)(dot - p) : strlen(p);
    snprintf(out, outsz, "%.*s.lrc", (int)n, p);
}

HT_API int ht_db_rename(sqlite3 *db, const char *const *from, const char *const *to, int n, int *rows)
{
    static const char *const cols[3] = { "path", "lrc_path", "album_pic_path" };
    char (*tabs)[64] = calloc(HTX_MAXTAB, 64);
    int has[HTX_MAXTAB][3], nt = 0, err = 0, nrows = 0;
    if (!tabs) return HT_ENOMEM;
    memset(has, 0, sizeof has);
    sqlite3_stmt *q, *qi;
    /* tablas con el esquema de pistas (path, name, artist, album) */
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &q, 0)) { free(tabs); return HT_EDB; }
    while (sqlite3_step(q) == SQLITE_ROW && nt < HTX_MAXTAB) {
        const char *tn = (const char *)sqlite3_column_text(q, 0);
        if (!tn || strlen(tn) >= 64 || strchr(tn, '"')) continue;
        char sql[256]; snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", tn);
        if (sqlite3_prepare_v2(db, sql, -1, &qi, 0)) continue;
        int h[3] = { 0 }, nm = 0, ar = 0, al = 0;
        while (sqlite3_step(qi) == SQLITE_ROW) {
            const char *cn = (const char *)sqlite3_column_text(qi, 1);
            if (!cn) continue;
            for (int c = 0; c < 3; c++) if (!strcmp(cn, cols[c])) h[c] = 1;
            nm |= !strcmp(cn, "name"); ar |= !strcmp(cn, "artist"); al |= !strcmp(cn, "album");
        }
        sqlite3_finalize(qi);
        if (!h[0] || !nm || !ar || !al) continue;
        snprintf(tabs[nt], 64, "%s", tn); memcpy(has[nt], h, sizeof h); nt++;
    }
    sqlite3_finalize(q);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0)) { free(tabs); return HT_EDB; }
    for (int i = 0; i < n && !err; i++) {
        char a[PATH_MAX + 4], b[PATH_MAX + 4];
        if (!htx_db_path_any(from[i], a, sizeof a) || !htx_db_path_any(to[i], b, sizeof b)) continue;
        struct stat st; int dir = !stat(to[i], &st) && S_ISDIR(st.st_mode);
        char la[PATH_MAX + 8], lb[PATH_MAX + 8];                    /* su .lrc (columna lrc_path) */
        if (dir) { strncat(a, "\\", sizeof a - strlen(a) - 1); strncat(b, "\\", sizeof b - strlen(b) - 1); }
        else { htx_db_lrc(a, la, sizeof la); htx_db_lrc(b, lb, sizeof lb); }
        for (int k = 0; k < nt && !err; k++) for (int c = 0; c < 3 && !err; c++) {
            if (!has[k][c]) continue;
            const char *va = !dir && c == 1 ? la : a, *vb = !dir && c == 1 ? lb : b;
            char sql[512];
            if (dir) snprintf(sql, sizeof sql, "UPDATE \"%s\" SET %s=?2||substr(%s,length(?1)+1) WHERE substr(%s,1,length(?1))=?1 COLLATE NOCASE",
                              tabs[k], cols[c], cols[c], cols[c]);
            else snprintf(sql, sizeof sql, "UPDATE \"%s\" SET %s=?2 WHERE %s=?1", tabs[k], cols[c], cols[c]);
            if (sqlite3_prepare_v2(db, sql, -1, &q, 0)) { err = 1; break; }
            if (dir) { sqlite3_bind_text(q, 1, a, -1, SQLITE_TRANSIENT); sqlite3_bind_text(q, 2, b, -1, SQLITE_TRANSIENT); }
            else { htx_bind_z(q, 1, va); htx_bind_z(q, 2, vb); }     /* el reproductor guarda la ruta con su NUL */
            if (sqlite3_step(q) != SQLITE_DONE) err = 1;
            else nrows += sqlite3_changes(db);
            sqlite3_finalize(q);
        }
    }
    if (err) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    else if (sqlite3_exec(db, "COMMIT", 0, 0, 0)) { sqlite3_exec(db, "ROLLBACK", 0, 0, 0); err = 1; }
    free(tabs);
    if (rows) *rows = err ? 0 : nrows;
    return err ? HT_EDB : HT_OK;
}
#endif /* HIBYTAG_NO_DB */

/* ================================================================== CLI */
#ifdef HIBYTAG_MAIN
static int htm_shown;
static int htm_prog(void *ud, uint64_t done, uint64_t total)
{
    (void)ud;
    if (total > 1) { fprintf(stderr, "\r  escribiendo %3d%%", (int)(done * 100 / total)); htm_shown = 1; }
    return 0;
}
static int htm_bprog(void *ud, int idx, int n, uint64_t done, uint64_t total)
{
    (void)ud;
    static int last = -1;
    int pc = total ? (int)(done * 100 / total) : 100;
    if (pc != last || idx >= n) { fprintf(stderr, "\r[%3d%%] %d/%d", pc, idx < n ? idx + 1 : n, n); last = pc; }
    if (idx >= n) fputc('\n', stderr);
    return 0;
}
static int htm_changes(int argc, char **argv, ht_changes *ch)
{
    memset(ch, 0, sizeof *ch);
    for (int i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "falta '=' en %s\n", argv[i]); return -1; }
        *eq = 0;
        int f = ht_field_parse(argv[i]);
        if (f < 0) { fprintf(stderr, "campo desconocido: %s\n", argv[i]); return -1; }
        ch->v[f] = eq + 1;
    }
    return 0;
}
/* "clave=valor" -> ht_kv (modifica argv) */
static int htm_kv(int argc, char **argv, ht_kv *kv)
{
    for (int i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i]) { fprintf(stderr, "falta '=' en %s\n", argv[i]); return -1; }
        *eq = 0; kv[i].key = argv[i]; kv[i].val = eq + 1;
        if (!ht_key_valid(kv[i].key)) { fprintf(stderr, "campo no valido: %s\n", argv[i]); return -1; }
    }
    return 0;
}
#ifndef HIBYTAG_NO_DB
static int htm_dbrename(const char *const *from, const char *const *to, int n)
{
    if (getenv("HIBYTAG_NODB") && *getenv("HIBYTAG_NODB") == '1') return 0;
    sqlite3 *db = ht_db_open(0);
    if (!db) { fprintf(stderr, "base: no se pudo abrir (sin actualizar)\n"); return 2; }
    int rows = 0, r = ht_db_rename(db, from, to, n, &rows);
    if (r < 0) fprintf(stderr, "base: %s (%s)\n", ht_strerror(r), sqlite3_errmsg(db));
    else printf("base: %d rutas cambiadas\n", rows);
    ht_db_close(db);
    return r < 0 ? 2 : 0;
}
static void htm_dbupdate(const char *const *paths, int n)
{
    if (getenv("HIBYTAG_NODB") && *getenv("HIBYTAG_NODB") == '1') return;
    sqlite3 *db = ht_db_open(0);
    if (!db) { fprintf(stderr, "base: no se pudo abrir (sin actualizar)\n"); return; }
    int rows = 0, r = ht_db_update(db, paths, n, &rows);
    if (r < 0) fprintf(stderr, "base: %s (%s)\n", ht_strerror(r), sqlite3_errmsg(db));
    else printf("base: %d filas actualizadas%s\n", rows, r ? ", algunos ficheros no estan en la base" : "");
    ht_db_close(db);
}
#endif

#include <time.h>
#include "hibytag_db.inc.c"
int main(int argc, char **argv)
{
    const char *fa = getenv("HIBYTAG_FAILAT");
    if (fa) ht_test_fail_at = strtoull(fa, 0, 10);
    if (argc >= 2 && !strcmp(argv[1], "apply-staged")) return htdb_apply_staged();
    if (argc >= 4 && !strcmp(argv[1], "dbsnapshot")) return htdb_snapshot(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "dbcheck")) { char why[160] = ""; int ok = htdb_check(argv[2], 1, why, sizeof why); puts(ok ? "ok" : why); return !ok; }
    if (argc < 3) {
        fprintf(stderr, "uso: hibytag get FICHERO... | set FICHERO campo=valor... | batch LISTA campo=valor...\n"
                        "              | suggest CAMPO PREFIJO [N] | dbupdate FICHERO... | setcover IMG.jpg FICHERO...\n"
                        "              | getall FICHERO... | setkv FICHERO CLAVE=valor... | batchkv LISTA CLAVE=valor...\n"
                        "              | rename VIEJO NUEVO | renamedir VIEJA NUEVA [rmparent] | dbrename VIEJO NUEVO...\n"
                        "campos: title artist album albumartist genre year track disc; claves: cualquiera (COMPOSER...)\n");
        return 1;
    }
    if (!strcmp(argv[1], "getall")) {
        int rc = 0;
        for (int i = 2; i < argc; i++) {
            ht_all a;
            int e = ht_read_all(argv[i], &a);
            if (argc > 3) printf("== %s\n", argv[i]);
            if (e) { printf("error=%s\n", ht_strerror(e)); rc = 2; }
            else {
                printf("format=%s\n", ht_fmt_name(a.fmt));
                for (int k = 0; k < a.n; k++) {
                    printf("%s=", a.it[k].key);
                    for (const char *c = a.it[k].val; *c; c++) { if (*c == '\n') fputs("\\n", stdout); else if (*c != '\r') putchar(*c); }
                    if (a.it[k].flags) printf("\t#%s%s%s", a.it[k].flags & HT_ITEM_RO ? " ro" : "", a.it[k].flags & HT_ITEM_TRUNC ? " trunc" : "",
                                              a.it[k].flags & HT_ITEM_MULTI ? " multi" : "");
                    putchar('\n');
                }
            }
            ht_all_free(&a);
        }
        return rc;
    }
    if (!strcmp(argv[1], "setkv")) {
        if (argc < 4) return 1;
        ht_kv *kv = calloc(argc, sizeof *kv);
        if (!kv || htm_kv(argc - 3, argv + 3, kv)) return 1;
        int mode, e = ht_write_kv(argv[2], kv, argc - 3, htm_prog, 0, &mode);
        if (htm_shown) fputc('\n', stderr);
        free(kv);
        if (e) { fprintf(stderr, "error: %s\n", ht_strerror(e)); return 2; }
        printf("ok (%s)\n", mode == HT_MODE_INPLACE ? "en sitio" : mode == HT_MODE_REWRITE ? "reescrito" : "sin cambios");
#ifndef HIBYTAG_NO_DB
        const char *p[1] = { argv[2] };
        htm_dbupdate(p, 1);
#endif
        return 0;
    }
    if (!strcmp(argv[1], "rename") || !strcmp(argv[1], "renamedir")) {
        if (argc < 4) return 1;
        int d = !strcmp(argv[1], "renamedir");
        int e = d ? ht_rename_dir(argv[2], argv[3], argc > 4 && !strcmp(argv[4], "rmparent")) : ht_rename_song(argv[2], argv[3]);
        if (e) { fprintf(stderr, "error: %s\n", ht_strerror(e)); return 2; }
        printf("ok\n");
#ifndef HIBYTAG_NO_DB
        const char *f[1] = { argv[2] }, *t[1] = { argv[3] };
        return htm_dbrename(f, t, 1);
#else
        return 0;
#endif
    }
#ifndef HIBYTAG_NO_DB
    if (!strcmp(argv[1], "dbrename")) {
        if (argc < 4 || (argc - 2) % 2) { fprintf(stderr, "uso: hibytag dbrename VIEJO NUEVO [VIEJO NUEVO]...\n"); return 1; }
        int n = (argc - 2) / 2;
        const char **f = calloc(n, sizeof *f), **t = calloc(n, sizeof *t);
        if (!f || !t) return 3;
        for (int i = 0; i < n; i++) { f[i] = argv[2 + 2 * i]; t[i] = argv[3 + 2 * i]; }
        setenv("HIBYTAG_NODB", "0", 1);
        int r = htm_dbrename(f, t, n);
        free(f); free(t);
        return r;
    }
#endif
    if (!strcmp(argv[1], "get")) {
        int rc = 0;
        for (int i = 2; i < argc; i++) {
            ht_tags t;
            int e = ht_read(argv[i], &t);
            if (argc > 3) printf("== %s\n", argv[i]);
            if (e) { printf("error=%s\n", ht_strerror(e)); rc = 2; }
            else {
                printf("format=%s\n", ht_fmt_name(t.fmt));
                for (int f = 0; f < HT_NFIELDS; f++) if (t.v[f]) printf("%s=%s\n", ht_field_name(f), t.v[f]);
            }
            ht_tags_free(&t);
        }
        return rc;
    }
    if (!strcmp(argv[1], "set")) {
        ht_changes ch;
        if (htm_changes(argc - 3, argv + 3, &ch)) return 1;
        int mode, e = ht_write(argv[2], &ch, htm_prog, 0, &mode);
        if (htm_shown) fputc('\n', stderr);
        if (e) { fprintf(stderr, "error: %s\n", ht_strerror(e)); return 2; }
        printf("ok (%s)\n", mode == HT_MODE_INPLACE ? "en sitio" : mode == HT_MODE_REWRITE ? "reescrito" : "sin cambios");
#ifndef HIBYTAG_NO_DB
        const char *p[1] = { argv[2] };
        htm_dbupdate(p, 1);
#endif
        return 0;
    }
    if (!strcmp(argv[1], "setcover")) {
        if (argc < 4) { fprintf(stderr, "uso: hibytag setcover IMG.jpg FICHERO...\n"); return 1; }
        int bad = 0;
        for (int i = 3; i < argc; i++) {
            int e = ht_set_cover(argv[i], argv[2]);
            if (e) { printf("ERR %s: %s\n", argv[i], ht_strerror(e)); bad++; }
            else printf("OK %s\n", argv[i]);
            fflush(stdout);
            if (e == HT_ETOOBIG || e == HT_EIMAGE || (e == HT_EOPEN && access(argv[2], R_OK))) { bad += argc - i - 1; break; }
        }
        return bad ? 2 : 0;
    }
    if (!strcmp(argv[1], "batch") || !strcmp(argv[1], "batchkv")) {
        ht_changes ch;
        int kvm = !strcmp(argv[1], "batchkv");
        ht_kv *kv = kvm ? calloc(argc, sizeof *kv) : 0;
        if (kvm ? !kv || htm_kv(argc - 3, argv + 3, kv) : htm_changes(argc - 3, argv + 3, &ch)) return 1;
        FILE *lf = strcmp(argv[2], "-") ? fopen(argv[2], "r") : stdin;
        if (!lf) { perror(argv[2]); return 1; }
        char **paths = 0; int n = 0, cap = 0;
        char line[PATH_MAX + 2];
        while (fgets(line, sizeof line, lf)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            if (!l) continue;
            if (n >= cap) { cap = cap ? cap * 2 : 64; paths = realloc(paths, cap * sizeof *paths); if (!paths) return 3; }
            paths[n++] = strdup(line);
        }
        if (lf != stdin) fclose(lf);
        int *res = calloc(n ? n : 1, sizeof *res);
        int r = kvm ? ht_batch_kv((const char *const *)paths, n, kv, argc - 3, htm_bprog, 0, res)
                    : ht_batch((const char *const *)paths, n, &ch, htm_bprog, 0, res);
        free(kv);
        int ok = 0;
        for (int i = 0; i < n; i++) {
            if (res[i]) { printf("ERR %s: %s\n", paths[i], ht_strerror(res[i])); free(paths[i]); }
            else paths[ok++] = paths[i];
        }
        printf("lote: %d ok, %d con error\n", ok, n - ok);
#ifndef HIBYTAG_NO_DB
        if (ok) htm_dbupdate((const char *const *)paths, ok);
#endif
        for (int i = 0; i < ok; i++) free(paths[i]);
        free(paths); free(res);
        return r ? 2 : 0;
    }
#ifndef HIBYTAG_NO_DB
    if (!strcmp(argv[1], "suggest")) {
        int f = ht_field_parse(argv[2]), max = argc > 4 ? atoi(argv[4]) : 8;
        if (max < 1 || max > 100) max = 8;
        sqlite3 *db = ht_db_open(0);
        if (!db) { fprintf(stderr, "no se pudo abrir la base\n"); return 2; }
        char (*out)[HT_SUGLEN] = malloc((size_t)max * HT_SUGLEN);
        int k = ht_db_suggest(db, f, argc > 3 ? argv[3] : "", out, max);
        for (int i = 0; i < k; i++) printf("%s\n", out[i]);
        free(out);
        ht_db_close(db);
        return 0;
    }
    if (!strcmp(argv[1], "dbupdate")) {
        setenv("HIBYTAG_NODB", "0", 1);
        htm_dbupdate((const char *const *)argv + 2, argc - 2);
        return 0;
    }
#endif
    fprintf(stderr, "orden desconocida: %s\n", argv[1]);
    return 1;
}
#endif /* HIBYTAG_MAIN */
