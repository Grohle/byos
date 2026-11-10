/* hibynet: letras (LRCLIB) y caratulas (iTunes Search + MusicBrainz/Cover Art Archive)
 * para HiByOS. Binario estatico pequeno: HTTP/1.1 propio + mbedTLS 2.28 + libjpeg + stb.
 *
 *   hibynet lyrics ARTIST TITLE [ALBUM] [DURATION_S] [OUTDIR]
 *       -> id \t artist \t title \t album \t duration \t synced(0/1) \t primer verso
 *          (OUTDIR: guarda tambien cada resultado en OUTDIR/<id>.lrc, sin otra descarga)
 *   hibynet lyrics-get ID OUT.lrc
 *   hibynet covers ARTIST ALBUM
 *       -> source \t url \t artist \t album \t year
 *   hibynet cover-get URL OUT.jpg [SIZE]      (+ OUT.thumb.raw RGB565 LE 96x96)
 *   hibynet get URL OUT                    (HIBYNET_RESUME=1: continua OUT.part con Range)
 *
 * stdout: resultados (UTF-8, uno por linea, separados por TAB). stderr: progreso/errores.
 * Codigos de salida: 0 ok, 1 sin resultados / no encontrado, 2 red/HTTP/TLS,
 *                    3 uso, 4 E/S o imagen.
 * Variables: HIBYNET_CA=ruta a cacert.pem (por defecto junto al binario),
 *            HIBYNET_NOBOM=1 (LRC sin BOM UTF-8), HIBYNET_VERBOSE=1,
 *            HIBYNET_STATS=1 (imprime VmHWM = pico de RSS al terminar),
 *            HIBYNET_TIMEOUT=s (plazo por llamada de red; por defecto 15 s),
 *            HIBYNET_RESUME=1 (get: continuar OUT.part con Range).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <netdb.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/base64.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"

#include <jpeglib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define UA "HiByOS/0.6 (https://github.com/Grohle/hibyos)"
#define NET_TIMEOUT 15.0          /* s por llamada de red */
#define API_CAP (512 * 1024)      /* tope de respuesta JSON */
#define IMG_CAP (16 * 1024 * 1024)/* tope de descarga de imagen */
#define THUMB 96

enum { E_OK = 0, E_NONE = 1, E_NET = 2, E_USAGE = 3, E_IO = 4 };

static int g_verbose;
static double g_deadline;   /* reloj monotonic; 0 = sin limite */
static int g_idle;          /* 1: el plazo se renueva con cada lectura (descargas) */
static char g_ca[1024];

static void logf_(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("hibynet: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
#define vlog(...) do { if (g_verbose) logf_(__VA_ARGS__); } while (0)

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double g_timeout = NET_TIMEOUT;   /* HIBYNET_TIMEOUT=s: servidores lentos (Gutendex tarda > 1 min sin cache) */
static void deadline_reset(void) { g_deadline = now_s() + g_timeout; }

/* ====================================================================== TLS */
static mbedtls_entropy_context g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config g_conf;
static int g_tls_ready;
static int g_clock_bogus;

/* Busca en cacert.pem las raices cuyo subject == issuer del certificado hijo.
 * Solo se decodifican/parsean candidatos: RSS minimo aunque el bundle sea completo. */
static int ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **cands) {
    (void)ctx;
    *cands = NULL;
    FILE *f = fopen(g_ca, "r");
    if (!f) { logf_("no se puede abrir %s", g_ca); return -1; }
    static char line[256];
    static char b64[12288];
    static unsigned char der[9216];
    size_t bl = 0; int in = 0, found = 0;
    mbedtls_x509_crt *chain = NULL;
    while (fgets(line, sizeof line, f)) {
        if (!in) { if (!strncmp(line, "-----BEGIN CERTIFICATE-----", 27)) { in = 1; bl = 0; } continue; }
        if (!strncmp(line, "-----END CERTIFICATE-----", 25)) {
            in = 0;
            size_t dl = 0;
            if (mbedtls_base64_decode(der, sizeof der, &dl, (unsigned char *)b64, bl)) continue;
            if (!memmem(der, dl, child->issuer_raw.p, child->issuer_raw.len)) continue;
            if (!chain) {
                chain = mbedtls_calloc(1, sizeof *chain);
                if (!chain) break;
                mbedtls_x509_crt_init(chain);
            }
            mbedtls_x509_crt tmp; mbedtls_x509_crt_init(&tmp);
            int ok = !mbedtls_x509_crt_parse_der(&tmp, der, dl) &&
                     tmp.subject_raw.len == child->issuer_raw.len &&
                     !memcmp(tmp.subject_raw.p, child->issuer_raw.p, tmp.subject_raw.len);
            mbedtls_x509_crt_free(&tmp);
            if (ok && !mbedtls_x509_crt_parse_der(chain, der, dl)) found++;
            continue;
        }
        for (char *p = line; *p; p++)
            if (*p > ' ' && bl < sizeof b64) b64[bl++] = *p;
    }
    fclose(f);
    vlog("ca: %d raiz(es) candidata(s)", found);
    if (!found && chain) { mbedtls_x509_crt_free(chain); mbedtls_free(chain); chain = NULL; }
    *cands = chain;
    return 0;
}

static int vrfy_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)ctx; (void)crt; (void)depth;
    /* reloj sin ajustar (p.ej. sin RTC/NTP): se toleran SOLO los errores de fecha */
    if (g_clock_bogus) *flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

static int tls_init(void) {
    if (g_tls_ready) return 0;
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm);
    g_clock_bogus = tm.tm_year + 1900 < 2025;
    if (g_clock_bogus) logf_("aviso: reloj sin ajustar (%d), se ignoran fechas de certificados", tm.tm_year + 1900);
    mbedtls_entropy_init(&g_ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_ssl_config_init(&g_conf);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent, (const unsigned char *)"hibynet", 7)) return -1;
    if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) return -1;
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(&g_conf, ca_cb, NULL);
    mbedtls_ssl_conf_verify(&g_conf, vrfy_cb, NULL);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_min_version(&g_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    g_tls_ready = 1;
    return 0;
}

/* ============================================================== conexion */
typedef struct {
    int fd, tls;
    mbedtls_ssl_context ssl;
    unsigned char buf[4096];
    int pos, len, eof;
} conn_t;

static int wait_fd(int fd, short ev) {
    for (;;) {
        double left = g_deadline - now_s();
        if (left <= 0) return 0;
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, (int)(left * 1000) + 1);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

static int bio_send(void *ctx, const unsigned char *b, size_t n) {
    conn_t *c = ctx;
    int w = wait_fd(c->fd, POLLOUT);
    if (w == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (w < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    ssize_t r = send(c->fd, b, n, MSG_NOSIGNAL);
    if (r < 0) return (errno == EAGAIN || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)r;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    conn_t *c = ctx;
    int w = wait_fd(c->fd, POLLIN);
    if (w == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (w < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    ssize_t r = recv(c->fd, b, n, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        return errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)r;
}

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    int e = getaddrinfo(host, ps, &hints, &res);
    if (e) { logf_("DNS %s: %s", host, gai_strerror(e)); return -1; }
    int fd = -1;
    for (int pass = 0; pass < 2 && fd < 0; pass++)          /* IPv4 primero */
        for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
            if ((pass == 0) != (a->ai_family == AF_INET)) continue;
            int s = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (s < 0) continue;
            fcntl(s, F_SETFL, O_NONBLOCK);
            int r = connect(s, a->ai_addr, a->ai_addrlen);
            if (r < 0 && errno == EINPROGRESS) {
                if (wait_fd(s, POLLOUT) > 0) {
                    int err = 0; socklen_t l = sizeof err;
                    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
                    r = err ? -1 : 0;
                } else r = -1;
            }
            if (r == 0) { fcntl(s, F_SETFL, 0); fd = s; }
            else close(s);
        }
    freeaddrinfo(res);
    if (fd < 0) logf_("no se pudo conectar a %s:%d", host, port);
    else { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    return fd;
}

static void tls_err(const char *what, int r) {
    char b[160]; mbedtls_strerror(r, b, sizeof b);
    logf_("%s: -0x%04x %s", what, -r, b);
}

static int conn_open(conn_t *c, const char *host, int port, int tls) {
    memset(c, 0, sizeof *c);
    c->fd = tcp_connect(host, port);
    if (c->fd < 0) return -1;
    c->tls = tls;
    if (!tls) return 0;
    if (tls_init()) { logf_("tls init"); close(c->fd); return -1; }
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &g_conf) || mbedtls_ssl_set_hostname(&c->ssl, host)) {
        logf_("tls setup"); goto fail;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        tls_err(host, r);
        uint32_t fl = mbedtls_ssl_get_verify_result(&c->ssl);
        if (fl && fl != (uint32_t)-1) {
            char vb[256]; mbedtls_x509_crt_verify_info(vb, sizeof vb, "  ", fl);
            fprintf(stderr, "%s", vb);
        }
        goto fail;
    }
    vlog("tls %s %s", mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
    return 0;
fail:
    mbedtls_ssl_free(&c->ssl); close(c->fd); c->fd = -1;
    return -1;
}

static void conn_close(conn_t *c) {
    if (c->fd < 0) return;
    if (c->tls) { mbedtls_ssl_close_notify(&c->ssl); mbedtls_ssl_free(&c->ssl); }
    close(c->fd); c->fd = -1;
}

static int conn_write(conn_t *c, const char *b, int n) {
    while (n > 0) {
        int r;
        if (c->tls) {
            r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)b, n);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r < 0) { tls_err("write", r); return -1; }
        } else {
            r = bio_send(c, (const unsigned char *)b, n);
            if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r < 0) { logf_("write: %s", r == MBEDTLS_ERR_SSL_TIMEOUT ? "timeout" : "error"); return -1; }
        }
        b += r; n -= r;
    }
    return 0;
}

/* lee del socket a b (sin pasar por el buffer). 0 = EOF, <0 error */
static int conn_raw_read(conn_t *c, unsigned char *b, int n) {
    for (;;) {
        int r;
        if (c->tls) {
            r = mbedtls_ssl_read(&c->ssl, b, n);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return 0;
            if (r == MBEDTLS_ERR_NET_CONN_RESET) return 0;   /* cierre sin close_notify */
            if (r < 0) { if (r == MBEDTLS_ERR_SSL_TIMEOUT) logf_("timeout"); else tls_err("read", r); return -1; }
        } else {
            r = bio_recv(c, b, n);
            if (r == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (r == MBEDTLS_ERR_NET_CONN_RESET) return 0;
            if (r < 0) { logf_(r == MBEDTLS_ERR_SSL_TIMEOUT ? "timeout" : "error de lectura"); return -1; }
        }
        if (g_idle && r > 0) deadline_reset();
        return r;
    }
}

static int conn_fill(conn_t *c) {
    if (c->pos < c->len) return 1;
    if (c->eof) return 0;
    int r = conn_raw_read(c, c->buf, sizeof c->buf);
    if (r <= 0) { c->eof = 1; return r; }
    c->pos = 0; c->len = r;
    return 1;
}
static int conn_read(conn_t *c, unsigned char *b, int n) {
    int r = conn_fill(c);
    if (r <= 0) return r;
    int k = c->len - c->pos; if (k > n) k = n;
    memcpy(b, c->buf + c->pos, k); c->pos += k;
    return k;
}
/* linea terminada en \n, sin \r\n. -1 error/EOF */
static int conn_line(conn_t *c, char *out, int cap) {
    int n = 0;
    for (;;) {
        if (conn_fill(c) <= 0) return n ? n : -1;
        char ch = c->buf[c->pos++];
        if (ch == '\n') break;
        if (n < cap - 1) out[n++] = ch;
    }
    if (n && out[n - 1] == '\r') n--;
    out[n] = 0;
    return n;
}

/* =================================================================== URL */
typedef struct { int tls, port; char host[256]; char path[2048]; } url_t;

static int url_parse(const char *u, url_t *o) {
    memset(o, 0, sizeof *o);
    if (!strncasecmp(u, "https://", 8)) { o->tls = 1; o->port = 443; u += 8; }
    else if (!strncasecmp(u, "http://", 7)) { o->tls = 0; o->port = 80; u += 7; }
    else return -1;
    size_t hl = strcspn(u, "/?#");
    const char *h = u, *e = u + hl;
    const char *at = memchr(h, '@', hl); if (at) h = at + 1;          /* sin credenciales */
    const char *colon = NULL;
    if (*h == '[') { const char *rb = memchr(h, ']', e - h); if (!rb) return -1;
        if ((size_t)(rb - h - 1) >= sizeof o->host) return -1;
        memcpy(o->host, h + 1, rb - h - 1); colon = (rb + 1 < e && rb[1] == ':') ? rb + 1 : NULL; }
    else { colon = memchr(h, ':', e - h);
        size_t n = (colon ? colon : e) - h; if (!n || n >= sizeof o->host) return -1;
        memcpy(o->host, h, n); }
    if (colon) o->port = atoi(colon + 1);
    if (o->port <= 0 || o->port > 65535) return -1;
    u = e;
    if (*u == '/' || *u == '?') {
        size_t pl = strcspn(u, "#");
        if (pl + 2 >= sizeof o->path) return -1;
        if (*u == '?') { o->path[0] = '/'; memcpy(o->path + 1, u, pl); }
        else memcpy(o->path, u, pl);
    } else strcpy(o->path, "/");
    /* espacios y bytes no ASCII en la ruta: se escapan */
    char tmp[sizeof o->path]; size_t k = 0;
    for (const unsigned char *p = (unsigned char *)o->path; *p && k + 4 < sizeof tmp; p++) {
        if (*p <= ' ' || *p >= 0x7f) k += sprintf(tmp + k, "%%%02X", *p);
        else tmp[k++] = *p;
    }
    tmp[k] = 0; strcpy(o->path, tmp);
    return 0;
}

/* resuelve Location relativa a la URL base */
static void url_join(const char *base, const char *loc, char *out, size_t cap) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) { snprintf(out, cap, "%s", loc); return; }
    url_t b; url_parse(base, &b);
    const char *sch = b.tls ? "https" : "http";
    int defp = b.tls ? 443 : 80;
    char hp[300];
    if (b.port == defp) snprintf(hp, sizeof hp, "%s", b.host); else snprintf(hp, sizeof hp, "%s:%d", b.host, b.port);
    if (loc[0] == '/' && loc[1] == '/') snprintf(out, cap, "%s:%s", sch, loc);
    else if (loc[0] == '/') snprintf(out, cap, "%s://%s%s", sch, hp, loc);
    else {
        char dir[2048]; snprintf(dir, sizeof dir, "%s", b.path);
        char *q = strchr(dir, '?'); if (q) *q = 0;
        char *s = strrchr(dir, '/'); if (s) s[1] = 0;
        snprintf(out, cap, "%s://%s%s%s", sch, hp, dir, loc);
    }
}

static void urlenc(const char *s, char *out, size_t cap) {
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') out[k++] = *p;
        else k += sprintf(out + k, "%%%02X", *p);
    }
    out[k] = 0;
}

/* ================================================================== HTTP */
typedef int (*sink_fn)(void *ctx, const unsigned char *d, int n);
typedef struct { long long total, got; char ctype[96]; int status; } http_info;
static long long g_range;   /* > 0: pedir "Range: bytes=N-" (reanudar una descarga) */

/* method "GET"/"HEAD". follow: seguir redirecciones. Devuelve status HTTP o -1. */
static int http_req(const char *method, const char *url0, int follow, sink_fn sink, void *ctx,
                    http_info *info, char *loc_out, size_t loc_cap) {
    char url[4096]; snprintf(url, sizeof url, "%s", url0);
    http_info dummy; if (!info) info = &dummy;
    memset(info, 0, sizeof *info); info->total = -1;
    for (int hop = 0; hop < 8; hop++) {
        url_t u;
        if (url_parse(url, &u)) { logf_("URL invalida: %s", url); return -1; }
        deadline_reset();
        vlog("%s %s", method, url);
        conn_t *c = calloc(1, sizeof *c);
        if (!c) return -1;
        if (conn_open(c, u.host, u.port, u.tls)) { free(c); return -1; }
        char req[3072], rng[48] = "";
        if (g_range > 0) snprintf(rng, sizeof rng, "Range: bytes=%lld-\r\n", g_range);
        int defp = u.tls ? 443 : 80;
        char hostHdr[300];
        if (u.port == defp) snprintf(hostHdr, sizeof hostHdr, "%s", u.host); else snprintf(hostHdr, sizeof hostHdr, "%s:%d", u.host, u.port);
        int rl = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA "\r\nAccept: */*\r\n"
            "Accept-Encoding: identity\r\n%sConnection: close\r\n\r\n", method, u.path, hostHdr, rng);
        if (rl >= (int)sizeof req || conn_write(c, req, rl)) { conn_close(c); free(c); return -1; }
        char line[2048];
        if (conn_line(c, line, sizeof line) < 0) { logf_("sin respuesta de %s", u.host); conn_close(c); free(c); return -1; }
        int status = 0;
        if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) { logf_("respuesta no HTTP"); conn_close(c); free(c); return -1; }
        long long clen = -1; int chunked = 0; char loc[2048] = "";
        info->ctype[0] = 0;
        for (;;) {
            int n = conn_line(c, line, sizeof line);
            if (n < 0) { logf_("cabeceras cortadas"); conn_close(c); free(c); return -1; }
            if (n == 0) break;
            char *v = strchr(line, ':'); if (!v) continue;
            *v++ = 0; while (*v == ' ' || *v == '\t') v++;
            if (!strcasecmp(line, "content-length")) clen = atoll(v);
            else if (!strcasecmp(line, "transfer-encoding") && strcasestr(v, "chunked")) chunked = 1;
            else if (!strcasecmp(line, "location")) snprintf(loc, sizeof loc, "%s", v);
            else if (!strcasecmp(line, "content-type")) snprintf(info->ctype, sizeof info->ctype, "%s", v);
        }
        vlog("-> %d len=%lld%s", status, clen, chunked ? " chunked" : "");
        if (status >= 300 && status < 400 && loc[0]) {
            char nu[4096]; url_join(url, loc, nu, sizeof nu);
            conn_close(c); free(c);
            if (loc_out) snprintf(loc_out, loc_cap, "%s", nu);
            if (!follow) return status;
            snprintf(url, sizeof url, "%s", nu);
            continue;
        }
        if (!strcmp(method, "HEAD") || status == 204 || status == 304) { conn_close(c); free(c); return status; }
        info->total = chunked ? -1 : clen;
        info->status = status;
        unsigned char buf[4096];
        int rc = 0;
        if (chunked) {
            for (;;) {
                if (conn_line(c, line, sizeof line) < 0) { rc = -1; break; }
                if (!line[0]) continue;
                long long cs = strtoll(line, NULL, 16);
                if (cs <= 0) break;
                while (cs > 0) {
                    int n = conn_read(c, buf, cs < (long long)sizeof buf ? (int)cs : (int)sizeof buf);
                    if (n <= 0) { rc = -1; break; }
                    cs -= n; info->got += n;
                    if (sink && sink(ctx, buf, n)) { rc = -2; break; }
                }
                if (rc) break;
            }
        } else {
            long long left = clen;
            while (left != 0) {
                int want = sizeof buf;
                if (left > 0 && left < want) want = (int)left;
                int n = conn_read(c, buf, want);
                if (n < 0) { rc = -1; break; }
                if (n == 0) { if (left > 0) rc = -1; break; }
                if (left > 0) left -= n;
                info->got += n;
                if (sink && sink(ctx, buf, n)) { rc = -2; break; }
            }
        }
        conn_close(c); free(c);
        if (rc == -1) { logf_("respuesta incompleta (%lld bytes)", info->got); return -1; }
        if (rc == -2) return -1;
        return status;
    }
    logf_("demasiadas redirecciones");
    return -1;
}

/* --- sinks */
typedef struct { char *p; size_t n, cap, max; } mbuf;
static int sink_mem(void *ctx, const unsigned char *d, int n) {
    mbuf *m = ctx;
    if (m->n + n + 1 > m->max) { logf_("respuesta demasiado grande (>%zu)", m->max); return -1; }
    if (m->n + n + 1 > m->cap) {
        size_t nc = m->cap ? m->cap : 16384;
        while (nc < m->n + n + 1) nc *= 2;
        if (nc > m->max) nc = m->max;
        char *np = realloc(m->p, nc); if (!np) return -1;
        m->p = np; m->cap = nc;
    }
    memcpy(m->p + m->n, d, n); m->n += n; m->p[m->n] = 0;
    return 0;
}
typedef struct { FILE *f; http_info *info; long long last, cap, off; } fsink;
static int sink_file(void *ctx, const unsigned char *d, int n) {
    fsink *s = ctx;
    if (s->info->status < 200 || s->info->status >= 300) return 0;   /* cuerpo de un error: no se guarda */
    if (s->off > 0 && s->info->status != 206) {    /* el servidor ignoro el Range: desde cero */
        if (fflush(s->f) || ftruncate(fileno(s->f), 0) || fseek(s->f, 0, SEEK_SET)) return -1;
        s->off = 0;
    }
    if (fwrite(d, 1, n, s->f) != (size_t)n) { logf_("error escribiendo: %s", strerror(errno)); return -1; }
    if (s->cap > 0 && s->info->got > s->cap) { logf_("descarga demasiado grande (>%lld)", s->cap); return -1; }
    if (s->info->got - s->last >= 65536) {
        s->last = s->info->got;
        fprintf(stderr, "progress %lld %lld\n", s->off + s->info->got, s->info->total < 0 ? -1 : s->off + s->info->total);
    }
    return 0;
}

/* GET a memoria (JSON). Devuelve status; *out = buffer terminado en NUL (liberar). */
static int get_mem(const char *url, mbuf *m) {
    memset(m, 0, sizeof *m); m->max = API_CAP;
    int st = http_req("GET", url, 1, sink_mem, m, NULL, NULL, 0);
    if (st > 0 && !m->p) { m->p = calloc(1, 1); }
    return st;
}

/* GET a fichero, via OUT.part + rename. Devuelve status (200..299 = ok) o -1.
 * HIBYNET_RESUME=1 (descargas largas): si ya hay un OUT.part se pide el resto con Range y se
 * anade (206); si el servidor manda el fichero entero (200) se empieza de cero; 416 = ya estaba
 * completo. Si la red falla, OUT.part se conserva para la proxima vez. Sin la variable, como antes. */
static int get_file(const char *url, const char *out, long long *bytes, long long cap) {
    char part[4096]; snprintf(part, sizeof part, "%s.part", out);
    int resume = getenv("HIBYNET_RESUME") != NULL;
    struct stat ps; long long off = 0;
    if (resume && !stat(part, &ps) && S_ISREG(ps.st_mode)) off = ps.st_size;
    FILE *f = fopen(part, off > 0 ? "ab" : "wb");
    if (!f) { logf_("no se puede crear %s: %s", part, strerror(errno)); return -2; }
    http_info info; fsink s = { f, &info, 0, cap, off };
    g_range = off;
    if (off > 0) vlog("reanudando desde %lld", off);
    int st = http_req("GET", url, 1, sink_file, &s, &info, NULL, 0);
    g_range = 0;
    if (st == 416 && off > 0) st = 200;                        /* ya estaba entero */
    if (fclose(f)) st = -1;
    if (st < 200 || st >= 300) {
        if (st > 0) logf_("HTTP %d", st);
        if (!resume || st > 0) unlink(part);
        return st < 0 ? -1 : st;
    }
    if (rename(part, out)) { logf_("rename: %s", strerror(errno)); unlink(part); return -2; }
    if (bytes) *bytes = s.off + info.got;
    return st;
}

/* ================================================================== JSON
 * Parser minimo de recorrido sobre el buffer (sin arbol de tokens). */
static const char *jw(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }
static const char *jskip_str(const char *p) {
    if (*p != '"') return NULL;
    for (p++; *p; p++) { if (*p == '\\') { if (!*++p) return NULL; } else if (*p == '"') return p + 1; }
    return NULL;
}
static const char *jskip(const char *p) {
    p = jw(p); if (!p || !*p) return NULL;
    if (*p == '"') return jskip_str(p);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (*p) {
            if (*p == '"') { p = jskip_str(p); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') d++;
            else if (*p == '}' || *p == ']') { if (--d == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p;
}
static void put_utf8(char **o, char *end, unsigned cp) {
    char b[4]; int n;
    if (cp < 0x80) { b[0] = cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 63); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); n = 4; }
    if (*o + n < end) { memcpy(*o, b, n); *o += n; }
}
static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return 0xFFFFFFFF;
    }
    return v;
}
/* decodifica valor JSON a texto: strings (con escapes), numeros/bools tal cual, null -> "" */
static int jstr(const char *v, char *out, size_t cap) {
    char *o = out, *end = out + cap - 1;
    v = jw(v);
    if (!v || !*v || cap == 0) { if (cap) *out = 0; return -1; }
    if (*v != '"') {
        if (!strncmp(v, "null", 4)) { *out = 0; return 0; }
        const char *e = jskip(v); if (!e) { *out = 0; return -1; }
        size_t n = e - v; if (n > cap - 1) n = cap - 1;
        memcpy(out, v, n); out[n] = 0; return (int)n;
    }
    for (v++; *v && *v != '"'; v++) {
        if (*v != '\\') { if (o < end) *o++ = *v; continue; }
        v++;
        switch (*v) {
        case 'n': if (o < end) *o++ = '\n'; break;
        case 't': if (o < end) *o++ = '\t'; break;
        case 'r': if (o < end) *o++ = '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = hex4(v + 1);
            if (cp == 0xFFFFFFFF) break;
            v += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
                unsigned lo = hex4(v + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); v += 6; }
            }
            put_utf8(&o, end + 1, cp);
            break;
        }
        case 0: v--; break;
        default: if (o < end) *o++ = *v;
        }
    }
    *o = 0;
    return (int)(o - out);
}
static char *jstrdup(const char *v) {
    v = jw(v); if (!v) return strdup("");
    const char *e = jskip(v); size_t n = e ? (size_t)(e - v) + 1 : 1;
    char *s = malloc(n + 1); if (!s) return NULL;
    jstr(v, s, n + 1);
    return s;
}
static const char *jget(const char *obj, const char *key) {
    obj = jw(obj); if (!obj || *obj != '{') return NULL;
    const char *p = jw(obj + 1);
    char k[128];
    while (p && *p == '"') {
        jstr(p, k, sizeof k);
        p = jskip_str(p); p = jw(p); if (!p || *p != ':') return NULL;
        p = jw(p + 1);
        if (!strcmp(k, key)) return p;
        p = jw(jskip(p)); if (!p) return NULL;
        if (*p == ',') p = jw(p + 1); else break;
    }
    return NULL;
}
static int jnull(const char *v) { v = jw(v); return !v || !strncmp(v, "null", 4); }
static const char *jarr_first(const char *a) {
    a = jw(a); if (!a || *a != '[') return NULL;
    a = jw(a + 1); return (*a == ']' || !*a) ? NULL : a;
}
static const char *jarr_next(const char *el) {
    const char *p = jw(jskip(el)); if (!p) return NULL;
    if (*p != ',') return NULL;
    p = jw(p + 1); return (*p == ']' || !*p) ? NULL : p;
}
static double jnum(const char *v) { char b[48]; jstr(v, b, sizeof b); return atof(b); }

/* imprime campo sin TAB/saltos de linea */
static void outf(const char *s, int last) {
    for (; *s; s++) putchar((*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s);
    putchar(last ? '\n' : '\t');
}

/* =============================================================== LYRICS */
#define LRCLIB "https://lrclib.net"
typedef struct { long id; char artist[160], title[160], album[160], first[160]; double dur; int synced; int score, ord; } lyr_t;
static lyr_t g_lyr[40]; static int g_nlyr;
static const char *g_lyr_dir;                    /* lyrics ... OUTDIR: cada resultado tambien en OUTDIR/<id>.lrc */
static int lyr_save(const char *o, const char *out, int *synced);

/* primera linea con texto (sin [mm:ss.xx], <mm:ss.xx> ni [ar:...]); sin cortar UTF-8 */
static void lyr_first(const char *v, char *out, size_t cap) {
    *out = 0;
    char *s = jstrdup(v); if (!s) return;
    for (char *l = s, *nl; l && *l; l = nl) {
        nl = strpbrk(l, "\r\n"); if (nl) *nl++ = 0;
        size_t k = 0;
        for (const char *p = l; *p; ) {
            if (*p == '[' || *p == '<') { const char *e = strchr(p, *p == '[' ? ']' : '>'); if (e) { p = e + 1; continue; } }
            if (k + 1 < cap && !(k == 0 && (*p == ' ' || *p == '\t'))) out[k++] = *p == '\t' ? ' ' : *p;
            p++;
        }
        while (k && out[k - 1] == ' ') k--;
        out[k] = 0;
        if (k) break;
    }
    free(s);
    size_t n = strlen(out), j = n;
    while (j && ((unsigned char)out[j - 1] & 0xC0) == 0x80) j--;
    if (j) { unsigned char c = (unsigned char)out[j - 1]; size_t need = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; if (j - 1 + need > n) out[j - 1] = 0; }
}
static void lyr_add(const char *o, double want_dur, int bonus) {
    if (g_nlyr >= (int)(sizeof g_lyr / sizeof g_lyr[0])) return;
    const char *v = jget(o, "id"); if (!v) return;
    long id = (long)jnum(v);
    for (int i = 0; i < g_nlyr; i++) if (g_lyr[i].id == id) return;
    const char *sl = jget(o, "syncedLyrics"), *pl = jget(o, "plainLyrics");
    int has_s = sl && !jnull(sl), has_p = pl && !jnull(pl);
    if (!has_s && !has_p) return;                /* instrumental / vacio */
    lyr_t *L = &g_lyr[g_nlyr];
    memset(L, 0, sizeof *L);
    L->id = id; L->ord = g_nlyr++;
    jstr(jget(o, "artistName"), L->artist, sizeof L->artist);
    jstr(jget(o, "trackName"), L->title, sizeof L->title);
    jstr(jget(o, "albumName"), L->album, sizeof L->album);
    v = jget(o, "duration"); L->dur = v ? jnum(v) : 0;
    L->synced = has_s;
    lyr_first(has_s ? sl : pl, L->first, sizeof L->first);
    int s = bonus + (has_s ? 0 : 40);
    if (want_dur > 0) { double d = L->dur - want_dur; if (d < 0) d = -d; s += d > 60 ? 60 : (int)d; }
    L->score = s;
    if (g_lyr_dir) { char p[1200]; snprintf(p, sizeof p, "%s/%ld.lrc", g_lyr_dir, id); lyr_save(o, p, 0); }
}
static int lyr_cmp(const void *a, const void *b) {
    const lyr_t *x = a, *y = b;
    return x->score != y->score ? x->score - y->score : x->ord - y->ord;
}
static void lyr_query(const char *url, double dur, int bonus, int one, int *neterr) {
    mbuf m;
    int st = get_mem(url, &m);
    if (st == 200) { if (one) lyr_add(m.p, dur, bonus); else for (const char *e = jarr_first(m.p); e; e = jarr_next(e)) lyr_add(e, dur, bonus); }
    else if (st < 0) (*neterr)++;
    free(m.p);
}

/* varias consultas (exacta por duracion, artista+titulo, texto libre, solo titulo) sin repetir id */
static int cmd_lyrics(int argc, char **argv) {
    if (argc < 2) return E_USAGE;
    const char *art = argv[0], *tit = argv[1];
    double dur = argc > 3 ? atof(argv[3]) : 0;
    g_lyr_dir = argc > 4 && argv[4][0] ? argv[4] : NULL;
    char ea[600], et[600], url[2400], q[1300], eq[3000];
    urlenc(art, ea, sizeof ea); urlenc(tit, et, sizeof et);
    int neterr = 0;
    if (art[0] && dur > 0) {                     /* /api/get acepta +-2 s de diferencia */
        snprintf(url, sizeof url, LRCLIB "/api/get?artist_name=%s&track_name=%s&duration=%d", ea, et, (int)(dur + 0.5));
        lyr_query(url, dur, -100, 1, &neterr);
    }
    if (art[0]) {
        snprintf(url, sizeof url, LRCLIB "/api/search?track_name=%s&artist_name=%s", et, ea);
        lyr_query(url, dur, 0, 0, &neterr);
    }
    if (g_nlyr < 6) {
        snprintf(q, sizeof q, "%s %s", tit, art); urlenc(q, eq, sizeof eq);
        snprintf(url, sizeof url, LRCLIB "/api/search?q=%s", eq);
        lyr_query(url, dur, 10, 0, &neterr);
    }
    if (g_nlyr < 6 && tit[0]) {
        snprintf(url, sizeof url, LRCLIB "/api/search?track_name=%s", et);
        lyr_query(url, dur, 20, 0, &neterr);
    }
    qsort(g_lyr, g_nlyr, sizeof g_lyr[0], lyr_cmp);
    int n = g_nlyr > 16 ? 16 : g_nlyr;
    for (int i = 0; i < n; i++) {
        lyr_t *L = &g_lyr[i];
        printf("%ld\t", L->id); outf(L->artist, 0); outf(L->title, 0); outf(L->album, 0);
        printf("%d\t%d\t", (int)(L->dur + 0.5), L->synced); outf(L->first, 1);
    }
    if (n) return E_OK;
    if (neterr) return E_NET;
    logf_("sin resultados");
    return E_NONE;
}

static int write_atomic(const char *out, const char *data, size_t n) {
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) { logf_("no se puede crear %s: %s", tmp, strerror(errno)); return -1; }
    int ok = fwrite(data, 1, n, f) == n;
    ok &= fflush(f) == 0; fsync(fileno(f));
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, out)) { logf_("error escribiendo %s", out); unlink(tmp); return -1; }
    return 0;
}

/* objeto JSON de LRCLIB -> fichero .lrc (cabecera [ar:][ti:][al:][length:][re:]). 0 ok, E_NONE sin letra */
static int lyr_save(const char *o, const char *out, int *synced) {
    const char *sl = jget(o, "syncedLyrics"), *pl = jget(o, "plainLyrics");
    const char *src = (sl && !jnull(sl)) ? sl : (pl && !jnull(pl)) ? pl : NULL;
    if (!src) { logf_("sin letra (instrumental)"); return E_NONE; }
    char ar[300], ti[300], al[300];
    jstr(jget(o, "artistName"), ar, sizeof ar);
    jstr(jget(o, "trackName"), ti, sizeof ti);
    jstr(jget(o, "albumName"), al, sizeof al);
    const char *dv = jget(o, "duration"); int dur = dv ? (int)(jnum(dv) + 0.5) : 0;
    char *body = jstrdup(src);
    if (!body) return E_IO;
    for (char *s = ar; *s; s++) if (*s == '\n' || *s == '\r') *s = ' ';
    for (char *s = ti; *s; s++) if (*s == '\n' || *s == '\r') *s = ' ';
    for (char *s = al; *s; s++) if (*s == '\n' || *s == '\r') *s = ' ';
    size_t bl = strlen(body);
    size_t cap = bl + 1200;
    char *b = malloc(cap); if (!b) { free(body); return E_IO; }
    size_t k = 0;
    if (!getenv("HIBYNET_NOBOM")) { memcpy(b, "\xEF\xBB\xBF", 3); k = 3; }
    k += snprintf(b + k, cap - k, "[ar:%s]\n[ti:%s]\n[al:%s]\n", ar, ti, al);
    if (dur > 0) k += snprintf(b + k, cap - k, "[length:%02d:%02d]\n", dur / 60, dur % 60);
    k += snprintf(b + k, cap - k, "[re:LRCLIB]\n");
    for (size_t i = 0; i < bl && k < cap - 2; i++) if (body[i] != '\r') b[k++] = body[i];
    if (k && b[k - 1] != '\n') b[k++] = '\n';
    int rc = write_atomic(out, b, k);
    if (synced) *synced = src == sl;
    free(b); free(body);
    return rc ? E_IO : E_OK;
}
static int cmd_lyrics_get(int argc, char **argv) {
    if (argc < 2) return E_USAGE;
    char url[256]; snprintf(url, sizeof url, LRCLIB "/api/get/%ld", atol(argv[0]));
    mbuf m;
    int st = get_mem(url, &m);
    if (st < 0) { free(m.p); return E_NET; }
    if (st != 200) { logf_("HTTP %d", st); free(m.p); return st == 404 ? E_NONE : E_NET; }
    int syn = 0, rc = lyr_save(m.p, argv[1], &syn);
    if (rc == E_OK) printf("%s\t%d\n", argv[1], syn);
    free(m.p);
    return rc;
}

/* =============================================================== COVERS */
typedef struct { char src[8], url[512], artist[160], album[200], year[8]; int score; } cov_t;
static cov_t g_cov[16]; static int g_ncov;

/* normaliza para comparar: minusculas ASCII, solo alfanumericos (bytes UTF-8 se conservan) */
static void norm(const char *s, char *o, size_t cap) {
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) {
        unsigned char c = *s;
        if (c >= 'A' && c <= 'Z') o[k++] = c + 32;
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) o[k++] = c;
    }
    o[k] = 0;
}
static int g_art_miss;   /* ultimo match_score: 1 si el artista no se parece */
static int match_score(const char *want_art, const char *want_alb, const char *art, const char *alb) {
    char a[256], b[256], c[256], d[256];
    norm(want_art, a, sizeof a); norm(art, c, sizeof c);
    norm(want_alb, b, sizeof b); norm(alb, d, sizeof d);
    int s = 0;
    if (b[0] && !strcmp(b, d)) s += 0; else if (b[0] && (strstr(d, b) || strstr(b, d))) s += 10; else s += 40;
    g_art_miss = 0;
    if (a[0] && !strcmp(a, c)) s += 0; else if (a[0] && c[0] && (strstr(c, a) || strstr(a, c))) s += 10; else { s += 30; g_art_miss = 1; }
    return s;
}
static void cov_add(const char *src, const char *url, const char *art, const char *alb, const char *year, int score) {
    if (g_ncov >= 16 || !url[0]) return;
    for (int i = 0; i < g_ncov; i++) if (!strcmp(g_cov[i].url, url)) return;
    cov_t *c = &g_cov[g_ncov++];
    snprintf(c->src, sizeof c->src, "%s", src); snprintf(c->url, sizeof c->url, "%s", url);
    snprintf(c->artist, sizeof c->artist, "%s", art); snprintf(c->album, sizeof c->album, "%s", alb);
    snprintf(c->year, sizeof c->year, "%.4s", year); c->score = score;
}
static int cov_cmp(const void *a, const void *b) { return ((const cov_t *)a)->score - ((const cov_t *)b)->score; }

/* MusicBrainz exige <= 1 peticion/s por cliente: marca de tiempo compartida entre procesos */
static void mb_throttle(void) {
    const char *st = "/tmp/.hibynet_mb";
    struct stat sb;
    if (!stat(st, &sb)) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        double dt = (ts.tv_sec - sb.st_mtim.tv_sec) + (ts.tv_nsec - sb.st_mtim.tv_nsec) / 1e9;
        if (dt >= 0 && dt < 1.1) { vlog("musicbrainz: espera %.2fs", 1.1 - dt); usleep((useconds_t)((1.1 - dt) * 1e6)); }
    }
    int fd = open(st, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { futimens(fd, NULL); close(fd); }
}

static void lucene_clean(const char *s, char *o, size_t cap) {
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) o[k++] = (*s == '"' || *s == '\\') ? ' ' : *s;
    o[k] = 0;
}

static int cmd_covers(int argc, char **argv) {
    if (argc < 2) return E_USAGE;
    const char *art = argv[0], *alb = argv[1];
    char q[800], eq[2400], url[2800];
    int neterr = 0;
    mbuf m;
    /* --- iTunes Search API */
    snprintf(q, sizeof q, "%s %s", art, alb); urlenc(q, eq, sizeof eq);
    snprintf(url, sizeof url, "https://itunes.apple.com/search?media=music&entity=album&limit=10&term=%s", eq);
    int st = get_mem(url, &m);
    if (st == 200) {
        int n = 0;
        for (const char *e = jarr_first(jget(m.p, "results")); e && n < 6; e = jarr_next(e)) {
            char a[160], b[200], y[32], u[512];
            jstr(jget(e, "artistName"), a, sizeof a);
            jstr(jget(e, "collectionName"), b, sizeof b);
            jstr(jget(e, "releaseDate"), y, sizeof y);
            if (jstr(jget(e, "artworkUrl100"), u, sizeof u) <= 0) continue;
            char *p = strstr(u, "100x100");
            if (p) memcpy(p, "600x600", 7);
            int sc = match_score(art, alb, a, b);
            if (g_art_miss) continue;                   /* artista distinto (tributos, etc.) */
            cov_add("itunes", u, a, b, y, sc);
            n++;
        }
    } else if (st < 0) neterr++;
    free(m.p);
    /* --- MusicBrainz release-group + Cover Art Archive */
    char ca[400], cb[400], lq[1000];
    lucene_clean(art, ca, sizeof ca); lucene_clean(alb, cb, sizeof cb);
    snprintf(lq, sizeof lq, "releasegroup:\"%s\" AND artist:\"%s\"", cb, ca);
    urlenc(lq, eq, sizeof eq);
    snprintf(url, sizeof url, "https://musicbrainz.org/ws/2/release-group/?fmt=json&limit=5&query=%s", eq);
    for (int tr = 0; tr < 2; tr++) {
        mb_throttle();
        st = get_mem(url, &m);
        if (st != 503) break;
        free(m.p); m.p = NULL;
        logf_("musicbrainz 503, reintento");
        usleep(1200000);
    }
    if (st == 200) {
        int n = 0;
        for (const char *e = jarr_first(jget(m.p, "release-groups")); e && n < 3; e = jarr_next(e)) {
            char id[64], b[200], y[32], a[160] = "", sc_s[16];
            jstr(jget(e, "id"), id, sizeof id);
            jstr(jget(e, "title"), b, sizeof b);
            jstr(jget(e, "first-release-date"), y, sizeof y);
            jstr(jget(e, "score"), sc_s, sizeof sc_s);
            if (atoi(sc_s) < 80 || !id[0]) continue;
            size_t al = 0;
            for (const char *c = jarr_first(jget(e, "artist-credit")); c; c = jarr_next(c)) {
                char nm[160], jp[32];
                jstr(jget(c, "name"), nm, sizeof nm); jstr(jget(c, "joinphrase"), jp, sizeof jp);
                al += snprintf(a + al, al < sizeof a ? sizeof a - al : 0, "%s%s", nm, jp);
                if (al >= sizeof a) { a[sizeof a - 1] = 0; break; }
            }
            char cu[256];
            snprintf(cu, sizeof cu, "https://coverartarchive.org/release-group/%s/front-500", id);
            /* comprobar que existe (307 -> imagen; 404 -> sin caratula). No es musicbrainz.org. */
            int hs = http_req("HEAD", cu, 0, NULL, NULL, NULL, NULL, 0);
            vlog("caa %s -> %d", id, hs);
            if (hs < 200 || hs >= 400) continue;
            cov_add("caa", cu, a, b, y, match_score(art, alb, a, b) + 1);
            n++;
        }
    } else if (st < 0) neterr++;
    else logf_("musicbrainz HTTP %d", st);
    free(m.p);
    qsort(g_cov, g_ncov, sizeof g_cov[0], cov_cmp);
    int n = g_ncov > 8 ? 8 : g_ncov;
    for (int i = 0; i < n; i++) {
        cov_t *c = &g_cov[i];
        outf(c->src, 0); outf(c->url, 0); outf(c->artist, 0); outf(c->album, 0); outf(c->year, 1);
    }
    if (n) return E_OK;
    if (neterr) return E_NET;
    logf_("sin resultados");
    return E_NONE;
}

/* ================================================================ IMAGEN
 * Escalado por area (caja) en streaming: se alimentan filas RGB de origen y se
 * acumulan en la fila de destino; memoria = salida + 1 fila de acumuladores. */
typedef struct {
    int sw, sh, ow, oh, y, sy;
    uint32_t *acc; int *x0;
    unsigned char *out;
} scaler;

static int sc_init(scaler *s, int sw, int sh, int ow, int oh, unsigned char *out) {
    memset(s, 0, sizeof *s);
    s->sw = sw; s->sh = sh; s->ow = ow; s->oh = oh; s->out = out;
    s->acc = calloc((size_t)ow * 3, sizeof *s->acc);
    s->x0 = malloc(sizeof(int) * (ow + 1));
    if (!s->acc || !s->x0) return -1;
    for (int x = 0; x <= ow; x++) s->x0[x] = (int)((long long)x * sw / ow);
    return 0;
}
static void sc_row(scaler *s, const unsigned char *row) {
    if (s->y >= s->oh) return;
    for (int x = 0; x < s->ow; x++) {
        uint32_t r = 0, g = 0, b = 0;
        for (int i = s->x0[x]; i < s->x0[x + 1]; i++) { r += row[i * 3]; g += row[i * 3 + 1]; b += row[i * 3 + 2]; }
        s->acc[x * 3] += r; s->acc[x * 3 + 1] += g; s->acc[x * 3 + 2] += b;
    }
    s->sy++;
    int yend = (int)((long long)(s->y + 1) * s->sh / s->oh);
    if (s->sy >= yend) {
        int ys = (int)((long long)s->y * s->sh / s->oh), rows = yend - ys;
        unsigned char *o = s->out + (size_t)s->y * s->ow * 3;
        for (int x = 0; x < s->ow; x++) {
            uint32_t cnt = (uint32_t)rows * (s->x0[x + 1] - s->x0[x]);
            for (int k = 0; k < 3; k++) o[x * 3 + k] = (s->acc[x * 3 + k] + cnt / 2) / cnt;
        }
        memset(s->acc, 0, (size_t)s->ow * 3 * sizeof *s->acc);
        s->y++;
    }
}
static void sc_free(scaler *s) { free(s->acc); free(s->x0); }

static void fit(int sw, int sh, int max, int *ow, int *oh) {
    if (sw <= max && sh <= max) { *ow = sw; *oh = sh; return; }   /* no se amplia */
    if (sw >= sh) { *ow = max; *oh = (int)(((long long)sh * max + sw / 2) / sw); }
    else { *oh = max; *ow = (int)(((long long)sw * max + sh / 2) / sh); }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr ci) {
    char b[JMSG_LENGTH_MAX]; ci->err->format_message(ci, b);
    logf_("jpeg: %s", b);
    longjmp(((struct jerr *)ci->err)->jb, 1);
}
static void jerr_silent(j_common_ptr ci, int lvl) { (void)ci; (void)lvl; }

/* decodifica fichero JPEG directamente a salida escalada (size = lado maximo) */
static unsigned char *load_jpeg_scaled(FILE *f, int size, int *ow, int *oh) {
    struct jpeg_decompress_struct ci;
    struct jerr je;
    unsigned char *out = NULL, *row = NULL;
    scaler sc; memset(&sc, 0, sizeof sc);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit; je.pub.emit_message = jerr_silent;
    if (setjmp(je.jb)) { jpeg_destroy_decompress(&ci); free(out); free(row); sc_free(&sc); return NULL; }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    jpeg_read_header(&ci, TRUE);
    long long px = (long long)ci.image_width * ci.image_height;
    if (ci.progressive_mode && px > 1600LL * 1600) {
        /* progresivo = buffer de coeficientes de la imagen entera (~3 B/px): limite por RAM */
        logf_("jpeg progresivo demasiado grande (%ux%u)", ci.image_width, ci.image_height);
        jpeg_destroy_decompress(&ci); return NULL;
    }
    int cmyk = ci.jpeg_color_space == JCS_CMYK || ci.jpeg_color_space == JCS_YCCK;
    ci.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    /* reduccion en el IDCT (M/8): el mayor factor que aun deja >= size */
    int tw, th; fit(ci.image_width, ci.image_height, size, &tw, &th);
    ci.scale_denom = 8; ci.scale_num = 8;
    for (int n = 1; n <= 8; n++) {
        if ((long long)ci.image_width * n / 8 >= tw && (long long)ci.image_height * n / 8 >= th) { ci.scale_num = n; break; }
    }
    ci.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&ci);
    int sw = ci.output_width, sh = ci.output_height, nc = ci.output_components;
    fit(sw, sh, size, ow, oh);
    if (*ow > tw) *ow = tw;
    if (*oh > th) *oh = th;
    vlog("jpeg %ux%u%s -> idct %dx%d -> %dx%d", ci.image_width, ci.image_height, ci.progressive_mode ? " prog" : "", sw, sh, *ow, *oh);
    out = malloc((size_t)*ow * *oh * 3);
    row = malloc((size_t)sw * (nc > 3 ? nc : 3));
    if (!out || !row || sc_init(&sc, sw, sh, *ow, *oh, out)) longjmp(je.jb, 1);
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW rp = row;
        jpeg_read_scanlines(&ci, &rp, 1);
        if (nc == 4) {          /* CMYK de Adobe (invertido) -> RGB */
            for (int x = 0; x < sw; x++) {
                unsigned c = row[x * 4], m = row[x * 4 + 1], y = row[x * 4 + 2], k = row[x * 4 + 3];
                row[x * 3] = c * k / 255; row[x * 3 + 1] = m * k / 255; row[x * 3 + 2] = y * k / 255;
            }
        } else if (nc == 1) {
            for (int x = sw - 1; x >= 0; x--) row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = row[x];
        }
        sc_row(&sc, row);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(row); sc_free(&sc);
    return out;
}

static unsigned char *load_stb_scaled(FILE *f, int size, int *ow, int *oh) {
    int w, h, n;
    if (!stbi_info_from_file(f, &w, &h, &n)) { logf_("formato de imagen no soportado"); return NULL; }
    /* stb decodifica la imagen entera: pico ~ w*h*(2*canales+3) bytes. Tope ~6 MB (RAM del R3 II) */
    long long est = (long long)w * h * (2 * n + 3) * (stbi_is_16_bit_from_file(f) ? 2 : 1);
    if (est > 6LL * 1024 * 1024) { logf_("imagen demasiado grande (%dx%d, ~%lld MB al decodificar)", w, h, est >> 20); return NULL; }
    unsigned char *img = stbi_load_from_file(f, &w, &h, &n, 3);
    if (!img) { logf_("decodificar: %s", stbi_failure_reason()); return NULL; }
    fit(w, h, size, ow, oh);
    unsigned char *out = malloc((size_t)*ow * *oh * 3);
    scaler sc;
    if (!out || sc_init(&sc, w, h, *ow, *oh, out)) { free(out); stbi_image_free(img); return NULL; }
    for (int y = 0; y < h; y++) sc_row(&sc, img + (size_t)y * w * 3);
    sc_free(&sc); stbi_image_free(img);
    return out;
}

static void wcb(void *ctx, void *data, int size) { fwrite(data, 1, size, (FILE *)ctx); }

static int write_jpeg(const char *out, const unsigned char *img, int w, int h) {
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) { logf_("no se puede crear %s: %s", tmp, strerror(errno)); return -1; }
    int ok = stbi_write_jpg_to_func(wcb, f, w, h, 3, img, 88);
    ok &= !ferror(f);
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, out)) { logf_("error escribiendo %s", out); unlink(tmp); return -1; }
    return 0;
}

static int write_thumb(const char *out, const unsigned char *img, int w, int h) {
    int tw, th;
    if (w >= h) { tw = THUMB; th = (int)(((long long)h * THUMB + w / 2) / w); }
    else { th = THUMB; tw = (int)(((long long)w * THUMB + h / 2) / h); }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    unsigned char *t = malloc((size_t)tw * th * 3);
    if (!t) return -1;
    if (w >= tw && h >= th) {
        scaler sc; if (sc_init(&sc, w, h, tw, th, t)) { free(t); return -1; }
        for (int y = 0; y < h; y++) sc_row(&sc, img + (size_t)y * w * 3);
        sc_free(&sc);
    } else {                      /* origen mas pequeno que 96: vecino mas cercano */
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++)
            memcpy(t + ((size_t)y * tw + x) * 3, img + ((size_t)(y * h / th) * w + x * w / tw) * 3, 3);
    }
    static uint16_t px[THUMB * THUMB];
    memset(px, 0, sizeof px);
    int ox = (THUMB - tw) / 2, oy = (THUMB - th) / 2;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
        const unsigned char *p = t + ((size_t)y * tw + x) * 3;
        uint16_t v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        unsigned char *d = (unsigned char *)&px[(oy + y) * THUMB + ox + x];
        d[0] = v & 0xFF; d[1] = v >> 8;                 /* little-endian explicito */
    }
    free(t);
    return write_atomic(out, (const char *)px, sizeof px);
}

static int cmd_cover_get(int argc, char **argv) {
    if (argc < 2) return E_USAGE;
    const char *url = argv[0], *out = argv[1];
    int size = argc > 2 ? atoi(argv[2]) : 500;
    if (size < 16 || size > 1600) size = 500;
    char raw[4096]; snprintf(raw, sizeof raw, "%s.dl", out);
    g_idle = 1;
    int st = get_file(url, raw, NULL, IMG_CAP);
    g_idle = 0;
    if (st == -2) return E_IO;
    if (st < 200 || st >= 300) return st == 404 ? E_NONE : E_NET;
    FILE *f = fopen(raw, "rb");
    if (!f) { unlink(raw); return E_IO; }
    unsigned char mg[4] = { 0 };
    if (fread(mg, 1, 4, f) != 4) { fclose(f); unlink(raw); logf_("fichero vacio"); return E_IO; }
    rewind(f);
    int w = 0, h = 0;
    unsigned char *img = (mg[0] == 0xFF && mg[1] == 0xD8) ? load_jpeg_scaled(f, size, &w, &h)
                                                          : load_stb_scaled(f, size, &w, &h);
    fclose(f); unlink(raw);
    if (!img) return E_IO;
    int rc = write_jpeg(out, img, w, h);
    if (!rc) {
        char th[4096]; snprintf(th, sizeof th, "%s", out);
        char *dot = strrchr(th, '.'), *sl = strrchr(th, '/');
        if (dot && (!sl || dot > sl) && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))) *dot = 0;
        strncat(th, ".thumb.raw", sizeof th - strlen(th) - 1);
        rc = write_thumb(th, img, w, h);
        if (!rc) printf("%s\t%d\t%d\t%s\n", out, w, h, th);
    }
    free(img);
    return rc ? E_IO : E_OK;
}

static int cmd_get(int argc, char **argv) {
    if (argc < 2) return E_USAGE;
    long long n = 0;
    g_idle = 1;
    int st = get_file(argv[0], argv[1], &n, 0);
    if (st == -2) return E_IO;
    if (st < 200 || st >= 300) return st == 404 ? E_NONE : E_NET;
    printf("%s\t%lld\n", argv[1], n);
    return E_OK;
}

/* ================================================================== main */
static void usage(void) {
    fputs("uso:\n"
          "  hibynet lyrics ARTIST TITLE [ALBUM] [DURATION_S] [OUTDIR]\n"
          "  hibynet lyrics-get ID OUT.lrc\n"
          "  hibynet covers ARTIST ALBUM\n"
          "  hibynet cover-get URL OUT.jpg [SIZE]\n"
          "  hibynet get URL OUT\n", stderr);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    g_verbose = getenv("HIBYNET_VERBOSE") != NULL;
    if (getenv("HIBYNET_TIMEOUT")) { double t = atof(getenv("HIBYNET_TIMEOUT")); if (t >= 1 && t <= 600) g_timeout = t; }
    const char *ca = getenv("HIBYNET_CA");
    if (ca) snprintf(g_ca, sizeof g_ca, "%s", ca);
    else {
        char exe[900]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) { exe[n] = 0; char *s = strrchr(exe, '/'); if (s) *s = 0; snprintf(g_ca, sizeof g_ca, "%s/cacert.pem", exe); }
        else snprintf(g_ca, sizeof g_ca, "cacert.pem");
    }
    if (argc < 2) { usage(); return E_USAGE; }
    const char *c = argv[1];
    int rc;
    if (strcmp(c, "get")) alarm(90);              /* red de seguridad global (DNS colgado, etc.) */
    if (!strcmp(c, "lyrics")) rc = cmd_lyrics(argc - 2, argv + 2);
    else if (!strcmp(c, "lyrics-get")) rc = cmd_lyrics_get(argc - 2, argv + 2);
    else if (!strcmp(c, "covers")) rc = cmd_covers(argc - 2, argv + 2);
    else if (!strcmp(c, "cover-get")) rc = cmd_cover_get(argc - 2, argv + 2);
    else if (!strcmp(c, "get")) rc = cmd_get(argc - 2, argv + 2);
    else rc = E_USAGE;
    if (rc == E_USAGE) usage();
    fflush(stdout);
    if (getenv("HIBYNET_STATS")) {          /* pico de RSS real (VmHWM) */
        FILE *f = fopen("/proc/self/status", "r"); char l[128];
        while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmHWM", 5)) fprintf(stderr, "stats: %s", l);
        if (f) fclose(f);
    }
    return rc;
}
