/* radio_stream.inc.c - piezas puras de la radio por internet (sin red ni ficheros):
 *   - que es una respuesta (MP3, AAC, lista .m3u/.pls, HLS, HTML...) por Content-Type, URL y
 *     primeros bytes
 *   - listas .m3u / .pls -> URLs
 *   - metadatos ICY (StreamTitle='...';) y el desentrelazado audio/metadatos
 *   - cabeceras de trama MPEG audio y ADTS, y busqueda del primer punto de sincronismo
 *   - nombres de fichero seguros y Latin-1 -> UTF-8
 * Lo incluyen hibynet (subcomando radio), el shell (radio_core.inc.c) y tests/radio_test.c.
 * Todos los nombres empiezan por rs_ / RS_. */
#ifndef RADIO_STREAM_INC
#define RADIO_STREAM_INC
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { RS_UNKNOWN = 0, RS_MP3, RS_AAC, RS_OGG, RS_FLAC, RS_HLS, RS_PLAYLIST, RS_HTML };
#define RS_URL 1024

static int rs_has(const char *s, const char *w) { return s && strcasestr(s, w) != 0; }

/* Content-Type -> RS_* (RS_UNKNOWN: hay que mirar la URL o los bytes) */
static int rs_ctype_kind(const char *ct)
{
    if (!ct || !*ct) return RS_UNKNOWN;
    if (rs_has(ct, "mpegurl") && (rs_has(ct, "apple") || rs_has(ct, "x-mpegurl"))) return RS_PLAYLIST;   /* se decide al leerla: HLS o lista */
    if (rs_has(ct, "mpegurl") || rs_has(ct, "scpls") || rs_has(ct, "pls+xml")) return RS_PLAYLIST;
    if (rs_has(ct, "audio/mpeg") || rs_has(ct, "audio/mp3") || rs_has(ct, "audio/x-mp3") || rs_has(ct, "audio/x-mpeg")) return RS_MP3;
    if (rs_has(ct, "aac")) return RS_AAC;                       /* audio/aac, audio/aacp, audio/x-aac */
    if (rs_has(ct, "ogg") || rs_has(ct, "opus") || rs_has(ct, "vorbis")) return RS_OGG;
    if (rs_has(ct, "flac")) return RS_FLAC;
    if (rs_has(ct, "text/html") || rs_has(ct, "xhtml")) return RS_HTML;
    return RS_UNKNOWN;
}

/* extension de la ruta de una URL (sin ?query) -> RS_* */
static int rs_url_kind(const char *url)
{
    char p[RS_URL]; snprintf(p, sizeof p, "%s", url ? url : "");
    p[strcspn(p, "?#")] = 0;
    const char *sl = strrchr(p, '/'), *d = strrchr(p, '.');
    if (!d || (sl && d < sl)) return RS_UNKNOWN;
    if (!strcasecmp(d, ".m3u8")) return RS_HLS;
    if (!strcasecmp(d, ".m3u") || !strcasecmp(d, ".pls")) return RS_PLAYLIST;
    if (!strcasecmp(d, ".mp3")) return RS_MP3;
    if (!strcasecmp(d, ".aac") || !strcasecmp(d, ".aacp")) return RS_AAC;
    if (!strcasecmp(d, ".ogg") || !strcasecmp(d, ".opus") || !strcasecmp(d, ".oga")) return RS_OGG;
    if (!strcasecmp(d, ".flac")) return RS_FLAC;
    return RS_UNKNOWN;
}

/* primeros bytes del cuerpo -> RS_* (RS_UNKNOWN: puede ser audio; se busca sincronismo) */
static int rs_sniff_kind(const unsigned char *b, int n)
{
    int i = 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;
    while (i < n && (b[i] == ' ' || b[i] == '\r' || b[i] == '\n' || b[i] == '\t')) i++;
    const char *s = (const char *)b + i; int m = n - i;
    if (m >= 4 && !memcmp(s, "OggS", 4)) return RS_OGG;
    if (m >= 4 && !memcmp(s, "fLaC", 4)) return RS_FLAC;
    if (m >= 7 && !strncasecmp(s, "#EXTM3U", 7)) return RS_PLAYLIST;
    if (m >= 10 && !strncasecmp(s, "[playlist]", 10)) return RS_PLAYLIST;
    if (m >= 7 && (!strncasecmp(s, "http://", 7) || (m >= 8 && !strncasecmp(s, "https://", 8)))) return RS_PLAYLIST;
    if (m >= 5 && (!strncasecmp(s, "<html", 5) || !strncasecmp(s, "<!doc", 5) || !strncasecmp(s, "<?xml", 5))) return RS_HTML;
    return RS_UNKNOWN;
}

static int rs_url_ok(const char *u) { return u && (!strncasecmp(u, "http://", 7) || !strncasecmp(u, "https://", 8)) && u[7] && u[8]; }

/* quita espacios y saltos de los extremos; devuelve el nuevo largo */
static int rs_trim(char *s)
{
    int n = (int)strlen(s), a = 0;
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') n--;
    while (a < n && (unsigned char)s[a] <= ' ') a++;
    memmove(s, s + a, n - a); s[n - a] = 0;
    return n - a;
}

/* Lista .m3u / .pls -> URLs http(s) en orden. Devuelve cuantas (<= max); -2 si es HLS
 * (#EXT-X-...: segmentos, no una emision continua). */
static int rs_playlist_parse(const char *body, int len, char urls[][RS_URL], int max)
{
    int n = 0, i = 0;
    if (len >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF) i = 3;
    while (i < len) {
        char l[RS_URL + 32]; int k = 0;
        while (i < len && body[i] != '\n' && body[i] != '\r') { if (k < (int)sizeof l - 1) l[k++] = body[i]; i++; }
        while (i < len && (body[i] == '\n' || body[i] == '\r')) i++;
        l[k] = 0; rs_trim(l);
        if (!l[0]) continue;
        if (!strncasecmp(l, "#EXT-X-", 7)) return -2;
        if (l[0] == '#' || l[0] == '[') continue;
        const char *u = l;
        if (!strncasecmp(l, "File", 4)) {                       /* .pls: FileN=URL */
            const char *e = strchr(l, '='); if (!e) continue;
            u = e + 1; while (*u == ' ') u++;
        }
        if (!rs_url_ok(u) || strlen(u) >= RS_URL) continue;
        if (n < max) snprintf(urls[n++], RS_URL, "%s", u);
    }
    return n;
}

/* ------------------------------------------------------------------ texto */
static int rs_utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 && *p >= 0xC2 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 && *p <= 0xF4 ? 3 : -1;
        if (k < 0) return 0;
        p++;
        while (k-- > 0) { if ((*p & 0xC0) != 0x80) return 0; p++; }
    }
    return 1;
}
/* si no es UTF-8 valido se toma por Latin-1 (lo habitual en emisoras antiguas) */
static void rs_to_utf8(const char *s, char *o, int cap)
{
    if (cap <= 0) return;
    if (rs_utf8_ok(s)) { snprintf(o, cap, "%s", s); return; }
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x80) { if (n + 1 >= cap) break; o[n++] = (char)*p; }
        else { if (n + 2 >= cap) break; o[n++] = (char)(0xC0 | *p >> 6); o[n++] = (char)(0x80 | (*p & 0x3F)); }
    }
    o[n] = 0;
}
/* corta s (UTF-8) a como mucho cap-1 bytes sin partir un caracter */
static void rs_cpy(char *d, int cap, const char *s)
{
    if (cap <= 0) return;
    int n = (int)strlen(s); if (n > cap - 1) n = cap - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    memcpy(d, s, n); d[n] = 0;
}

/* bloque de metadatos ICY -> titulo (UTF-8). 1 si trae StreamTitle (aunque sea vacio). */
static int rs_icy_title(const char *meta, int len, char *out, int cap)
{
    static const char K[] = "StreamTitle='";
    if (cap > 0) out[0] = 0;
    for (int i = 0; i + (int)sizeof K - 1 <= len; i++) {
        if (memcmp(meta + i, K, sizeof K - 1)) continue;
        int a = i + (int)sizeof K - 1, e = -1;
        for (int j = a; j < len && meta[j]; j++)               /* el titulo puede traer ' : acaba en "';" */
            if (meta[j] == '\'' && (j + 1 >= len || meta[j + 1] == ';' || !meta[j + 1])) { e = j; if (j + 1 < len && meta[j + 1] == ';') break; }
        if (e < 0) return 0;
        char raw[512]; int k = e - a; if (k > (int)sizeof raw - 1) k = sizeof raw - 1;
        memcpy(raw, meta + a, k); raw[k] = 0; rs_trim(raw);
        char u[768]; rs_to_utf8(raw, u, sizeof u);
        rs_cpy(out, cap, u);
        return 1;
    }
    return 0;
}

/* Desentrelazado ICY: cada metaint bytes de audio viene 1 byte L y L*16 de metadatos.
 * rs_icy_feed copia el audio de in a out (out puede ser in) y devuelve cuantos bytes de audio;
 * *got = 1 si en esta tanda se completo un bloque con StreamTitle (queda en st->title). */
struct rs_icy { int metaint, left, mlen, mgot; char meta[4096]; char title[256]; };
static void rs_icy_init(struct rs_icy *st, int metaint) { memset(st, 0, sizeof *st); st->metaint = metaint > 0 ? metaint : 0; st->left = st->metaint; st->mlen = -1; }
static int rs_icy_feed(struct rs_icy *st, const unsigned char *in, int n, unsigned char *out, int *got)
{
    if (got) *got = 0;
    if (!st->metaint) { if (out != in) memmove(out, in, n); return n; }
    int o = 0, i = 0;
    while (i < n) {
        if (st->left > 0) {
            int k = n - i; if (k > st->left) k = st->left;
            memmove(out + o, in + i, k); o += k; i += k; st->left -= k;
        } else if (st->mlen < 0) {
            st->mlen = in[i++] * 16; st->mgot = 0;
            if (!st->mlen) { st->mlen = -1; st->left = st->metaint; }
        } else {
            int k = n - i; if (k > st->mlen - st->mgot) k = st->mlen - st->mgot;
            memcpy(st->meta + st->mgot, in + i, k); st->mgot += k; i += k;
            if (st->mgot == st->mlen) {
                char t[256];
                if (rs_icy_title(st->meta, st->mlen, t, sizeof t) && strcmp(t, st->title)) { snprintf(st->title, sizeof st->title, "%s", t); if (got) *got = 1; }
                st->mlen = -1; st->left = st->metaint;
            }
        }
    }
    return o;
}

/* ------------------------------------------------------------------ tramas */
struct rs_frame { int kind, len, kbps, hz, ver, layer; };

/* cabecera MPEG audio (capas I-III, MPEG 1/2/2.5) en p[0..3]; 1 si es valida */
static int rs_mp3_hdr(const unsigned char *p, struct rs_frame *f)
{
    static const short BR[5][16] = {
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },   /* V1 L1 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },      /* V1 L2 */
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },       /* V1 L3 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },      /* V2 L1 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } };         /* V2 L2/L3 */
    static const int SR[3] = { 44100, 48000, 32000 };
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int ver = (p[1] >> 3) & 3, lay = (p[1] >> 1) & 3, bi = p[2] >> 4, si = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (ver == 1 || lay == 0 || bi == 0 || bi == 15 || si == 3) return 0;
    int layer = 4 - lay, v1 = ver == 3;
    int kbps = BR[v1 ? layer - 1 : layer == 1 ? 3 : 4][bi];
    int hz = SR[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    int len = layer == 1 ? (12000 * kbps / hz + pad) * 4 : (layer == 3 && !v1 ? 72000 : 144000) * kbps / hz + pad;
    if (len < 24) return 0;
    f->kind = RS_MP3; f->len = len; f->kbps = kbps; f->hz = hz; f->ver = ver; f->layer = layer;
    return 1;
}
/* cabecera ADTS (AAC) en p[0..5] */
static int rs_adts_hdr(const unsigned char *p, struct rs_frame *f)
{
    static const int SR[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    int si = (p[2] >> 2) & 15, len = (p[3] & 3) << 11 | p[4] << 3 | p[5] >> 5;
    if (si > 12 || len < 7) return 0;
    f->kind = RS_AAC; f->len = len; f->kbps = 0; f->hz = SR[si]; f->ver = (p[1] >> 3) & 1; f->layer = 0;
    return 1;
}
static int rs_hdr(const unsigned char *p, int kind, struct rs_frame *f)
{
    if (kind == RS_MP3) return rs_mp3_hdr(p, f);
    if (kind == RS_AAC) return rs_adts_hdr(p, f);
    return rs_mp3_hdr(p, f) || rs_adts_hdr(p, f);
}
/* Primer punto de b[0..n) donde empiezan RS_NSYNC tramas seguidas del mismo tipo y frecuencia
 * (kind = RS_MP3, RS_AAC o RS_UNKNOWN para cualquiera). Devuelve el desplazamiento y rellena f;
 * -1 si en n bytes no lo hay (o faltan datos para comprobarlo). */
#define RS_NSYNC 3
static int rs_sync(const unsigned char *b, int n, int kind, struct rs_frame *f)
{
    for (int i = 0; i + 8 <= n; i++) {
        if (b[i] != 0xFF) continue;
        struct rs_frame f0, g;
        if (!rs_hdr(b + i, kind, &f0)) continue;
        int p = i + f0.len, ok = 1;
        for (int k = 1; k < RS_NSYNC; k++) {
            if (p + 8 > n) { ok = 0; break; }
            if (!rs_hdr(b + p, f0.kind, &g) || g.hz != f0.hz || g.layer != f0.layer) { ok = 0; break; }
            p += g.len;
        }
        if (ok) { *f = f0; return i; }
    }
    return -1;
}

/* nombre de emisora -> nombre de fichero (sin / \ : * ? " < > | ni controles, <= 60 bytes) */
static void rs_safe_name(const char *name, char *o, int cap)
{
    char t[64]; int n = 0;
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    for (; *p && n < (int)sizeof t - 4; p++)
        t[n++] = (*p < 32 || strchr("/\\:*?\"<>|'%", *p)) ? ' ' : (char)*p;
    if ((*p & 0xC0) == 0x80) {                                  /* cortado a mitad de un caracter */
        while (n > 0 && ((unsigned char)t[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
    }
    t[n] = 0;
    rs_trim(t);
    while (t[0] == '.') memmove(t, t + 1, strlen(t));
    snprintf(o, cap, "%s", t[0] ? t : "Radio");
}
#endif
