/* radio_stream.inc.c - piezas puras de la radio por internet (sin red ni ficheros):
 *   - que es una respuesta (MP3, AAC, lista .m3u/.pls, HLS, HTML...) por Content-Type, URL y
 *     primeros bytes
 *   - listas .m3u / .pls -> URLs
 *   - metadatos ICY (StreamTitle='...';) y el desentrelazado audio/metadatos
 *   - el cuerpo HTTP "chunked"
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
        if (n < max) { size_t ul = strlen(u); memcpy(urls[n], u, ul + 1); n++; }
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

/* ------------------------------------------------------------------ HTTP chunked
 * Quita las cabeceras de trozo ("1a2;ext\r\n" ... "\r\n") de b[0..n) en el sitio; devuelve
 * los bytes de datos que quedan. El estado sigue entre llamadas (un trozo puede venir partido);
 * c->end = 1 al llegar el trozo de largo 0. Empezar con memset(c, 0, ...). */
struct rs_chunk { int st, ext, end; long long left, acc; };
static int rs_dechunk(struct rs_chunk *c, unsigned char *b, int n)
{
    int o = 0;
    for (int i = 0; i < n && !c->end; ) {
        if (c->left > 0) {
            int k = n - i; if (k > c->left) k = (int)c->left;
            memmove(b + o, b + i, k); o += k; i += k; c->left -= k;
            if (!c->left) c->st = 2;
            continue;
        }
        unsigned char ch = b[i++];
        if (c->st == 2) { if (ch == '\n') c->st = 0; }                   /* CRLF tras los datos */
        else if (ch == '\n') { c->left = c->acc; c->acc = 0; c->ext = 0; if (!c->left) c->end = 1; }
        else if (ch == ';') c->ext = 1;
        else if (!c->ext && isxdigit(ch)) c->acc = c->acc * 16 + (isdigit(ch) ? ch - '0' : (ch | 32) - 'a' + 10);
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

/* ------------------------------------------------------------------ duracion de lo escrito
 * Cuenta las tramas MPEG audio / ADTS de un flujo que llega a trozos (una trama puede quedar
 * partida entre dos trozos) y suma su duracion: el ayudante sabe asi cuantos segundos de audio
 * hay en el fichero y cuantos le quedan al reproductor por delante. Lo que no es trama (basura
 * tras un corte) se salta byte a byte (lost). Empezar con memset(d, 0, ...). */
struct rs_dur { unsigned char h[8]; int hn, kind; long long skip, frames, lost; double sec; };
static void rs_dur_feed(struct rs_dur *d, const unsigned char *b, int n)
{
    for (int i = 0; i < n; ) {
        if (d->skip > 0) {
            long long k = n - i; if (k > d->skip) k = d->skip;
            d->skip -= k; i += (int)k; continue;
        }
        d->h[d->hn++] = b[i++];
        if (d->hn == 1 && d->h[0] != 0xFF) { d->hn = 0; d->lost++; continue; }
        if (d->hn < 7) continue;
        struct rs_frame f;
        if (rs_hdr(d->h, d->kind ? d->kind : RS_UNKNOWN, &f) && f.hz > 0) {
            int smp = f.kind == RS_AAC ? 1024 * ((d->h[6] & 3) + 1) : f.layer == 1 ? 384 : f.layer == 3 && f.ver != 3 ? 576 : 1152;
            d->kind = f.kind; d->frames++; d->sec += (double)smp / f.hz;
            d->skip = f.len - 7; d->hn = 0;
        } else { memmove(d->h, d->h + 1, 6); d->hn = 6; d->lost++; }
    }
}

/* ------------------------------------------------------------------ HLS (listas .m3u8)
 * Lector de listas HLS por lineas, a trozos segun llegan (sin guardar el cuerpo entero):
 *   - maestra (#EXT-X-STREAM-INF): se queda con la variante de MENOS ancho de banda que sea solo
 *     audio (si todas llevan video y hay una pista #EXT-X-MEDIA TYPE=AUDIO con URI, esa; si no,
 *     la variante mas ligera: del TS solo se saca el audio)
 *   - de medios: TARGETDURATION, MEDIA-SEQUENCE, ENDLIST y los ultimos RS_HLS_SEGS segmentos
 *     (numero de secuencia, duracion, URI tal cual: relativa o absoluta) en un anillo
 *   - lo que no se sabe tocar: cifrado (#EXT-X-KEY distinto de NONE), fMP4 (#EXT-X-MAP) y
 *     trozos de fichero (#EXT-X-BYTERANGE)
 * Uso: rs_hls_init, rs_hls_feed (las veces que haga falta), rs_hls_end. ok = empezaba por #EXTM3U
 * y tiene etiquetas #EXT-X- (una lista .m3u de emisoras no es HLS). */
#define RS_HLS_SEGS 24
struct rs_hseg { long long seq; int ms; char uri[RS_URL]; };
struct rs_hls {
    int ok, ext, master, target, endlist, enc, fmp4, brange, nseg, nvar; long long seq0;
    struct rs_hseg seg[RS_HLS_SEGS];             /* el segmento i (0..nseg-1) esta en seg[i % RS_HLS_SEGS] */
    long long vbw; int vvideo; char var[RS_URL], amedia[RS_URL];
    /* lector */
    char l[RS_URL + 768]; int ln, lines, inf_ms, sinf, svideo; long long sbw;
};
static void rs_hls_init(struct rs_hls *h) { memset(h, 0, sizeof *h); h->vbw = -1; }
/* valor de ATRIBUTO=valor o ATRIBUTO="valor" de una etiqueta */
static int rs_hls_attr(const char *l, const char *key, char *o, int cap)
{
    int kl = (int)strlen(key);
    for (const char *p = strchr(l, ':'); p && *p; p++) {
        if ((*p == ':' || *p == ',') && !strncasecmp(p + 1, key, kl) && p[1 + kl] == '=') {
            const char *v = p + 2 + kl; int n = 0;
            if (*v == '"') { v++; while (v[n] && v[n] != '"') n++; }
            else while (v[n] && v[n] != ',') n++;
            if (n > cap - 1) n = cap - 1;
            memcpy(o, v, n); o[n] = 0;
            return 1;
        }
        if (*p == '"') { p = strchr(p + 1, '"'); if (!p) break; }
    }
    if (cap > 0) o[0] = 0;
    return 0;
}
static int rs_hls_video(const char *l)
{
    char c[160]; rs_hls_attr(l, "CODECS", c, sizeof c);
    if (rs_has(c, "avc") || rs_has(c, "hvc") || rs_has(c, "hev") || rs_has(c, "vp09") || rs_has(c, "av01")) return 1;
    return !c[0] && rs_hls_attr(l, "RESOLUTION", c, sizeof c);
}
static void rs_hls_line(struct rs_hls *h, char *l)
{
    rs_trim(l);
    if (!l[0]) return;
    if (!h->lines++) {
        const char *s = l; if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        h->ok = !strncmp(s, "#EXTM3U", 7);
        return;
    }
    if (!h->ok) return;
    if (l[0] == '#') {
        if (strncmp(l, "#EXT-X-", 7)) { if (!strncmp(l, "#EXTINF:", 8)) h->inf_ms = (int)(atof(l + 8) * 1000 + 0.5); return; }
        h->ext = 1;
        const char *t = l + 7;
        if (!strncmp(t, "TARGETDURATION:", 15)) h->target = atoi(t + 15);
        else if (!strncmp(t, "MEDIA-SEQUENCE:", 15)) h->seq0 = atoll(t + 15);
        else if (!strncmp(t, "ENDLIST", 7)) h->endlist = 1;
        else if (!strncmp(t, "KEY:", 4)) { char m[24]; rs_hls_attr(l, "METHOD", m, sizeof m); if (strcasecmp(m, "NONE")) h->enc = 1; }
        else if (!strncmp(t, "MAP:", 4)) h->fmp4 = 1;
        else if (!strncmp(t, "BYTERANGE:", 10)) h->brange = 1;
        else if (!strncmp(t, "STREAM-INF:", 11)) {
            char bw[24]; rs_hls_attr(l, "BANDWIDTH", bw, sizeof bw);
            h->sinf = 1; h->sbw = atoll(bw); h->svideo = rs_hls_video(l);
        } else if (!strncmp(t, "MEDIA:", 6)) {
            char ty[16], u[RS_URL], df[8]; rs_hls_attr(l, "TYPE", ty, sizeof ty);
            if (!strcasecmp(ty, "AUDIO") && rs_hls_attr(l, "URI", u, sizeof u) && u[0] && strlen(u) < RS_URL - 1) {
                rs_hls_attr(l, "DEFAULT", df, sizeof df);
                if (!h->amedia[0] || !strcasecmp(df, "YES")) snprintf(h->amedia, sizeof h->amedia, "%s", u);
            }
        }
        return;
    }
    if ((int)strlen(l) >= RS_URL) { h->sinf = 0; h->inf_ms = 0; return; }   /* URI imposible: se ignora */
    if (h->sinf) {                                   /* variante de una lista maestra */
        h->nvar++;
        int better = h->vbw < 0 || (h->vvideo && !h->svideo) || (h->vvideo == h->svideo && h->sbw < h->vbw);
        if (better) { h->vbw = h->sbw; h->vvideo = h->svideo; snprintf(h->var, sizeof h->var, "%s", l); }
        h->master = 1; h->sinf = 0;
        return;
    }
    struct rs_hseg *s = &h->seg[h->nseg % RS_HLS_SEGS];
    s->seq = h->seq0 + h->nseg; s->ms = h->inf_ms > 0 ? h->inf_ms : h->target * 1000; snprintf(s->uri, sizeof s->uri, "%s", l);
    h->nseg++; h->inf_ms = 0;
}
static void rs_hls_feed(struct rs_hls *h, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\n' || c == '\r') { h->l[h->ln] = 0; rs_hls_line(h, h->l); h->ln = 0; continue; }
        if (h->ln < (int)sizeof h->l - 1) h->l[h->ln++] = c;
    }
}
static void rs_hls_end(struct rs_hls *h)
{
    if (h->ln) { h->l[h->ln] = 0; rs_hls_line(h, h->l); h->ln = 0; }
    if (!h->ext) h->ok = 0;                          /* sin #EXT-X-: una lista .m3u normal */
}
/* la URI que hay que abrir de una maestra ("" si no hay) */
static const char *rs_hls_variant(const struct rs_hls *h) { return h->vvideo && h->amedia[0] ? h->amedia : h->var; }
/* segmento numero i (0..nseg-1); 0 si ya no esta en el anillo */
static const struct rs_hseg *rs_hls_seg(const struct rs_hls *h, int i)
{
    if (i < 0 || i >= h->nseg || i < h->nseg - RS_HLS_SEGS) return 0;
    return &h->seg[i % RS_HLS_SEGS];
}
/* Primer segmento (indice para rs_hls_seg) que toca bajar; h->nseg si no hay nada nuevo.
 *   last < 0 (arranque): hacia atras desde el ultimo hasta juntar need_ms de audio (asi el
 *   prebufer se llena en el acto con lo que el servidor ya tiene), al menos uno.
 *   last >= 0: el siguiente a last. *gap = 1 si last ya salio de la ventana (se pierde un trozo)
 *   o si la secuencia volvio muy atras (la emision se reinicio: se arranca de nuevo). */
static int rs_hls_first(const struct rs_hls *h, long long last, int need_ms, int *gap)
{
    *gap = 0;
    if (h->nseg <= 0) return 0;
    int lo = h->nseg > RS_HLS_SEGS ? h->nseg - RS_HLS_SEGS : 0;
    long long newest = h->seq0 + h->nseg - 1;
    if (last >= 0 && newest < last - RS_HLS_SEGS) { *gap = 1; last = -1; }
    if (last < 0) {
        int i = h->nseg - 1, acc = h->seg[i % RS_HLS_SEGS].ms;
        while (i > lo && acc < need_ms) { i--; acc += h->seg[i % RS_HLS_SEGS].ms; }
        return i;
    }
    if (last >= newest) return h->nseg;
    if (last + 1 < h->seq0 + lo) { *gap = 1; return lo; }
    return (int)(last + 1 - h->seq0);
}

/* ------------------------------------------------------------------ MPEG-TS -> audio
 * Demultiplexor de un transport stream que llega a trozos: PAT -> PMT -> la primera pista de
 * audio que suena (0x0F AAC ADTS, 0x03/0x04 MPEG audio) y de sus PES solo la carga util. Lo
 * que sale es el flujo ADTS / MP3 tal cual, listo para el fichero. Memoria: un paquete (188).
 * out debe tener sitio para n + 188 bytes (sale menos de lo que entra) y no puede ser in.
 * latm = 1 si la unica pista de audio es AAC LATM (0x11): no sirve. */
struct rs_ts { unsigned char pk[188]; int pn, pmt, apid, atype, latm, skip, started; long long pkts, junk; };
static void rs_ts_init(struct rs_ts *t) { memset(t, 0, sizeof *t); t->pmt = t->apid = -1; }
/* al empezar un segmento nuevo: el paquete a medias no sirve; las pistas se conservan */
static void rs_ts_cut(struct rs_ts *t) { t->pn = 0; }
static int rs_ts_packet(struct rs_ts *t, const unsigned char *p, unsigned char *out)
{
    t->pkts++;
    if (p[1] & 0x80) return 0;                       /* error de transporte */
    int pid = (p[1] & 0x1F) << 8 | p[2], pusi = p[1] & 0x40, afc = (p[3] >> 4) & 3, ps = 4;
    if (afc & 2) ps += 1 + p[4];
    if (!(afc & 1) || ps >= 188) return 0;
    if ((pid == 0 || pid == t->pmt) && pusi) {
        int s = ps + 1 + p[ps];
        if (s + 12 > 188) return 0;
        int len = (p[s + 1] & 0x0F) << 8 | p[s + 2], end = s + 3 + len - 4;
        if (end > 188) end = 188;
        if (pid == 0 && p[s] == 0x00) {
            for (int k = s + 8; k + 4 <= end; k += 4)
                if ((p[k] << 8 | p[k + 1]) != 0) { t->pmt = (p[k + 2] & 0x1F) << 8 | p[k + 3]; break; }
        } else if (pid == t->pmt && p[s] == 0x02 && t->apid < 0) {
            int k = s + 12 + ((p[s + 10] & 0x0F) << 8 | p[s + 11]);
            while (k + 5 <= end) {
                int st = p[k], ep = (p[k + 1] & 0x1F) << 8 | p[k + 2], el = (p[k + 3] & 0x0F) << 8 | p[k + 4];
                if (st == 0x0F || st == 0x03 || st == 0x04) { t->apid = ep; t->atype = st; t->latm = 0; break; }
                if (st == 0x11) t->latm = 1;
                k += 5 + el;
            }
        }
        return 0;
    }
    if (pid != t->apid || pid < 0) return 0;
    if (pusi) {
        if (ps + 9 > 188 || p[ps] || p[ps + 1] || p[ps + 2] != 1) { t->started = 0; return 0; }
        t->skip = 9 + p[ps + 8]; t->started = 1;
    }
    if (!t->started) return 0;                       /* el principio de este PES no lo vimos */
    if (t->skip > 0) { int k = 188 - ps; if (k > t->skip) k = t->skip; ps += k; t->skip -= k; }
    memcpy(out, p + ps, 188 - ps);
    return 188 - ps;
}
static int rs_ts_feed(struct rs_ts *t, const unsigned char *in, int n, unsigned char *out)
{
    int o = 0;
    for (int i = 0; i < n; ) {
        if (!t->pn) {
            if (in[i] != 0x47) { t->junk++; i++; continue; }
            if (n - i >= 188) { o += rs_ts_packet(t, in + i, out + o); i += 188; continue; }
        }
        int k = 188 - t->pn; if (k > n - i) k = n - i;
        memcpy(t->pk + t->pn, in + i, k); t->pn += k; i += k;
        if (t->pn == 188) { o += rs_ts_packet(t, t->pk, out + o); t->pn = 0; }
    }
    return o;
}
/* largo de una etiqueta ID3v2 al principio de b (audio "empaquetado" de HLS); 0 = no hay,
 * -1 = faltan bytes para saberlo */
static long rs_id3_len(const unsigned char *b, int n)
{
    if (n < 3) return n && b[0] != 'I' ? 0 : -1;
    if (memcmp(b, "ID3", 3)) return 0;
    if (n < 10) return -1;
    if ((b[6] | b[7] | b[8] | b[9]) & 0x80) return 0;
    long sz = (long)b[6] << 21 | (long)b[7] << 14 | (long)b[8] << 7 | b[9];
    return 10 + sz + (b[5] & 0x10 ? 10 : 0);
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
