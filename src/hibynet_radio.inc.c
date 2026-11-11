/* hibynet_radio.inc.c - subcomando "radio" de hibynet (se incluye desde hibynet.c).
 *
 *   hibynet radio URL DIR NAME [LEAD_S] [CAP_MB] [STATEDIR]
 *
 * Convierte una emisora de internet en un FICHERO que el reproductor de fabrica toca como
 * cualquier otro (lista "sin escanear" de playctl): asi el volumen, el ecualizador, el
 * Bluetooth y el reparto de la salida siguen siendo suyos, y al poner musica local la radio
 * se aparta sola.
 *
 * Lo que se sabe del motor (probado en el aparato) y que manda en este diseño:
 *   - NO toca ficheros de /tmp (tmpfs): acepta la lista y no suena. En la tarjeta SD (exFAT)
 *     si, con la ruta Linux tal cual. DIR es una carpeta de la SD (el shell usa
 *     /usr/data/mnt/sd_0/ByOS/cache/radio). El audio nunca va a la RAM: con 3-6 MB libres un
 *     bufer en tmpfs hundia todo el sistema.
 *   - Sigue un fichero que crece (vuelve a mirar el tamano), PERO si la reproduccion alcanza el
 *     final del fichero se para. Con 5 s escritos antes de empezar se paraba; con 15 s seguia.
 *     Por eso: no se anuncia el fichero hasta tener LEAD_S segundos (15 por defecto) y la
 *     escritura va siempre por delante. Si aun asi se para (la red se atasco), se empieza
 *     limpio: fichero nuevo, prebufer nuevo (5 s mas largo) y el shell vuelve a mandar la lista.
 *   - exFAT no tiene ficheros dispersos ni PUNCH_HOLE: el fichero solo crece. Para no llenar la
 *     tarjeta la sesion tiene un TOPE (CAP_MB, 128 por defecto, y nunca mas de lo libre menos
 *     16 MB): al llegar se deja de bajar, el reproductor acaba lo escrito y el ayudante sale con
 *     st=stopped err=cap. No se rota a otro fichero: mandar otra lista al motor corta el sonido
 *     (un hueco audible), asi que se prefiere un final claro.
 *
 *   1. Resuelve URL: redirecciones, listas .m3u/.pls (prueba sus entradas en orden), el
 *      "ICY 200 OK" de Shoutcast y HLS (.m3u8). Ogg/Opus y FLAC se rechazan (err=codec).
 *   2. Flujo continuo (Icecast/Shoutcast): pide el audio con Icy-MetaData: 1, separa los
 *      metadatos (titulo) y busca el primer punto con 3 tramas MP3 o ADTS seguidas.
 *      HLS: lista maestra -> la variante de audio mas ligera; lista de medios -> se piden los
 *      segmentos por orden de secuencia (al empezar, los ultimos que suman LEAD_S: el prebufer
 *      se llena en el acto), se vuelve a pedir la lista cada TARGETDURATION (la mitad si no
 *      cambio), sin repetir segmentos; URIs relativas o absolutas. Cada segmento se demultiplexa
 *      al vuelo (MPEG-TS -> pista AAC ADTS 0x0F o MPEG audio 0x03/0x04; tambien ADTS/MP3
 *      "empaquetado" con su etiqueta ID3) sin guardarlo entero. Conexion HTTP/1.1 persistente
 *      (un saludo TLS por servidor, no uno por segmento). Cifrado (EXT-X-KEY) -> err=enc;
 *      fMP4 o trozos de fichero -> err=codec.
 *   3. Escribe DIR/NAME.mp3|.aac (NAME (2).aac en la generacion 2...), empezando en una trama
 *      entera, y cuenta las tramas: sabe cuantos segundos de audio hay escritos.
 *   4. Vigila la posicion de lectura del reproductor en /proc/<pid>/fdinfo:
 *        - si no abre el fichero en 20 s                  -> err=player
 *        - si lo cierra con margen por delante (> 3 s)     -> st=stopped (puso otra musica)
 *        - si abre otro audio y no tiene ninguno nuestro   -> st=stopped
 *        - si lo cierra (o se queda quieto 20 s) cuando ya habia leido casi todo: se paro al
 *          llegar al final -> generacion nueva (o, con el tope o el fin de la emision, st=stopped
 *          err=cap|end)
 *        - si tiene mas de HIGH segundos sin leer (pausa)  -> deja de pedir a la red; al volver,
 *          Icecast se reconecta y HLS sigue por donde iba (o salta si ya no esta en la lista)
 *   5. SIGTERM / muerte del padre: trunca y borra sus ficheros y sale. No deja nada atras.
 *
 * Estado para el shell en STATEDIR/state (por defecto DIR; escritura atomica), clave=valor:
 *   st=connecting|buffering|ready|stopped|error  gen=N  file=RUTA  codec=MP3|AAC  br=KBPS
 *   pct=0..100 (llenado del prebufer)  open=0|1 (el reproductor ya abrio esta generacion)
 *   lead=S (segundos escritos por delante del lector)  hls=0|1  title=...
 *   err=net|http NNN|enc|codec|nostream|player|io|space|nosd  (con st=stopped: cap|end)
 *
 * Memoria: unos cientos de KB (bufer de sincronismo 64 KB, una lista HLS de 24 segmentos, un paquete
 * TS). Entorno (pruebas): HIBYRADIO_PLAYER=comm del proceso lector (por defecto hiby_player);
 * HIBYRADIO_NOREADER=1 no vigila a nadie (solo descarga); HIBYRADIO_HIGH_S=tope sin leer;
 * HIBYRADIO_CAP_KB=tope de la sesion en KB. */
#include <dirent.h>
#include <math.h>
#include <sys/statvfs.h>
#include "radio_stream.inc.c"

#define RD_UA "ByOS-Radio/1.0 (https://github.com/Grohle/hibyos)"
#define RD_SNIFF (64 * 1024)
#define RD_MINFREE (16LL << 20)   /* lo que se deja libre en la tarjeta */
static double rd_high = 180;      /* s por delante del lector: mas, y se deja de pedir a la red (pausa) */
#define RD_LOW (rd_high / 2)      /* Icecast: reconectar al bajar de aqui */
#define RD_ATEND 0.75             /* s por delante en su ultima lectura: menos = leyo hasta el final */

typedef struct {
    conn_t c; int open;
    int metaint, kind, br, chunked, gz, ka /* la conexion sigue viva al acabar el cuerpo */;
    long long clen, got; int done;
    struct rs_chunk ck;
    char ctype[96], url[2100], host[256]; int port, tls;
    struct rs_icy icy;
} rd_src;

static struct {
    char dir[300], state[320], tmp[328], pidf[320], name[80];
    char file[420], real[460], old[420], realdir[460];
    int fd, oldfd, gen, br, pct, opened, hls, kind, capped, eos;
    const char *codec, *st;
    char title[256];
    const char *player; int noreader, ppid;
    long long cap, wpos, rpos;
    struct rs_dur dur;
    int need_sync, nsb;            /* el proximo audio debe empezar en una trama (bufer rd_sb) */
} rd = { .fd = -1, .oldfd = -1 };

static double rd_lead(void);
static void rd_state(const char *st, const char *err)
{
    char b[1200];
    rd.st = st;
    int n = snprintf(b, sizeof b, "st=%s\ngen=%d\nfile=%s\ncodec=%s\nbr=%d\npct=%d\nopen=%d\nlead=%d\nhls=%d\ntitle=%s\nerr=%s\n",
                     st, rd.gen, rd.file, rd.codec ? rd.codec : "", rd.br, rd.pct, rd.opened, (int)rd_lead(), rd.hls, rd.title, err ? err : "");
    int f = open(rd.tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return;
    if (write(f, b, n) == n) { close(f); rename(rd.tmp, rd.state); } else close(f);
}
static void rd_drop_old(void)
{
    if (rd.oldfd >= 0) { if (ftruncate(rd.oldfd, 0)) {} close(rd.oldfd); rd.oldfd = -1; }
    if (rd.old[0]) { unlink(rd.old); rd.old[0] = 0; }
}
static void rd_cleanup(void)
{
    rd_drop_old();
    if (rd.fd >= 0) { if (ftruncate(rd.fd, 0)) {} close(rd.fd); rd.fd = -1; }   /* quien lo tenga abierto ve fin de fichero */
    if (rd.file[0]) unlink(rd.file);
    unlink(rd.pidf);
}
static void rd_end(const char *st, const char *err, int rc)
{
    rd_cleanup();
    rd.file[0] = 0;
    rd_state(st, err);
    vlog("radio: %s %s", st, err ? err : "");
    exit(rc);
}
/* solo llamadas seguras en una senal: el bucle puede estar bloqueado en DNS o en el TLS */
static void rd_sig(int sig)
{
    (void)sig;
    if (rd.oldfd >= 0 && ftruncate(rd.oldfd, 0)) {}
    if (rd.old[0]) unlink(rd.old);
    if (rd.fd >= 0 && ftruncate(rd.fd, 0)) {}
    if (rd.file[0]) unlink(rd.file);
    unlink(rd.pidf);
    int f = open(rd.state, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { if (write(f, "st=stopped\n", 11)) {} close(f); }
    _exit(0);
}

/* ------------------------------------------------------------------ HTTP / ICY */
static void rd_close(rd_src *s) { if (s->open || s->ka) conn_close(&s->c); s->open = s->ka = 0; }

/* GET siguiendo redirecciones. 0 = s abierto al principio del cuerpo.
 * hls = 1: HTTP/1.1 persistente (si la conexion anterior acabo su cuerpo y va al mismo
 * servidor, se reutiliza), sin metadatos ICY. 0: como un reproductor de radio (HTTP/1.0). */
static int rd_http(rd_src *s, const char *url0, int hls, char *err, int ecap)
{
    char url[2100]; snprintf(url, sizeof url, "%s", url0);
    for (int hop = 0; hop < 8; hop++) {
        url_t u;
        if (url_parse(url, &u)) { rd_close(s); snprintf(err, ecap, "url"); return -1; }
        int reuse = hls && s->ka && !s->open && s->port == u.port && s->tls == u.tls && !strcasecmp(s->host, u.host);
        if (!reuse) rd_close(s);
        for (int tries = 0; tries < 2; tries++) {
            conn_t keep; if (reuse) keep = s->c;
            memset(s, 0, sizeof *s); s->c.fd = -1;
            if (reuse) s->c = keep;
            snprintf(s->host, sizeof s->host, "%s", u.host); s->port = u.port; s->tls = u.tls;
            deadline_reset();
            vlog("radio: GET %s%s", url, reuse ? " (misma conexion)" : "");
            if (!reuse && conn_open(&s->c, u.host, u.port, u.tls)) { snprintf(err, ecap, "net"); return -1; }
            char req[3072], hostHdr[300];
            if (u.port == (u.tls ? 443 : 80)) snprintf(hostHdr, sizeof hostHdr, "%s", u.host); else snprintf(hostHdr, sizeof hostHdr, "%s:%d", u.host, u.port);
            int rl = hls ? snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " RD_UA "\r\nAccept: */*\r\n"
                                    "Accept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n", u.path, hostHdr)
                         : snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " RD_UA "\r\nAccept: */*\r\n"
                                    "Icy-MetaData: 1\r\nConnection: close\r\n\r\n", u.path, hostHdr);
            char line[2048]; int status = 0, ln = -1;
            if (rl < (int)sizeof req && !conn_write(&s->c, req, rl))
                for (int e = 0; e < 3 && (ln = conn_line(&s->c, line, sizeof line)) == 0; e++) { }   /* el CRLF final de un cuerpo "chunked" */
            if (ln <= 0) {
                conn_close(&s->c);
                if (reuse) { reuse = 0; continue; }          /* el servidor cerro la conexion persistente: otra nueva */
                snprintf(err, ecap, "net"); return -1;
            }
            if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1 && sscanf(line, "ICY %d", &status) != 1) { conn_close(&s->c); snprintf(err, ecap, "nostream"); return -1; }
            int http11 = !strncmp(line, "HTTP/1.1", 8), close_hdr = 0;
            char loc[2048] = ""; s->clen = -1;
            for (;;) {
                int n = conn_line(&s->c, line, sizeof line);
                if (n < 0) { conn_close(&s->c); snprintf(err, ecap, "net"); return -1; }
                if (n == 0) break;
                char *v = strchr(line, ':'); if (!v) continue;
                *v++ = 0; while (*v == ' ' || *v == '\t') v++;
                if (!strcasecmp(line, "location")) snprintf(loc, sizeof loc, "%s", v);
                else if (!strcasecmp(line, "content-type")) snprintf(s->ctype, sizeof s->ctype, "%s", v);
                else if (!strcasecmp(line, "content-length")) s->clen = atoll(v);
                else if (!strcasecmp(line, "icy-metaint")) s->metaint = atoi(v);
                else if (!strcasecmp(line, "icy-br")) s->br = atoi(v);
                else if (!strcasecmp(line, "transfer-encoding") && strcasestr(v, "chunked")) s->chunked = 1;
                else if (!strcasecmp(line, "connection") && strcasestr(v, "close")) close_hdr = 1;
                else if (!strcasecmp(line, "content-encoding") && strcasestr(v, "gzip")) s->gz = 1;
            }
            if (s->chunked) s->clen = -1;
            s->ka = hls && http11 && !close_hdr && (s->chunked || s->clen >= 0);
            vlog("radio: -> %d %s len=%lld metaint=%d br=%d%s", status, s->ctype, s->clen, s->metaint, s->br, s->ka ? " keep-alive" : "");
            if (status >= 300 && status < 400 && loc[0]) {
                char nu[4096]; url_join(url, loc, nu, sizeof nu);
                if (s->ka && s->clen >= 0 && s->clen < 65536) {     /* cuerpo pequeno: se tira y se sigue con la conexion */
                    unsigned char tb[1024]; long long left = s->clen;
                    while (left > 0) { int r = conn_read(&s->c, tb, left < (long long)sizeof tb ? (int)left : (int)sizeof tb); if (r <= 0) break; left -= r; }
                    if (left) { conn_close(&s->c); s->ka = 0; }
                } else { conn_close(&s->c); s->ka = 0; }
                if (strlen(nu) >= sizeof url) { rd_close(s); snprintf(err, ecap, "url"); return -1; }
                snprintf(url, sizeof url, "%s", nu);
                break;                                       /* siguiente salto */
            }
            if (status < 200 || status >= 300) { conn_close(&s->c); s->ka = 0; snprintf(err, ecap, "http %d", status); return -1; }
            if (hls) s->metaint = 0;
            if (s->metaint < 0 || s->metaint > (1 << 20)) s->metaint = 0;
            rs_icy_init(&s->icy, s->metaint);
            snprintf(s->url, sizeof s->url, "%s", url);
            s->open = 1;
            return 0;
        }
    }
    rd_close(s);
    snprintf(err, ecap, "nostream");
    return -1;
}
/* hay datos que leer antes de ms? */
static int rd_wait(rd_src *s, int ms)
{
    conn_t *c = &s->c;
    if (c->pos < c->len) return 1;
    if (c->tls && mbedtls_ssl_get_bytes_avail(&c->ssl) > 0) return 1;
    struct pollfd p = { c->fd, POLLIN, 0 };
    return poll(&p, 1, ms) > 0;
}
/* audio (sin trozos HTTP ni metadatos) a b: > 0 bytes, 0 = aun nada, -1 = fin o error. Con
 * una conexion persistente, al final del cuerpo s->done = 1 y la conexion queda para la
 * siguiente peticion. */
static int rd_read(rd_src *s, unsigned char *b, int n, int *newtitle)
{
    if (newtitle) *newtitle = 0;
    if (s->clen >= 0 && s->got >= s->clen) { s->done = 1; s->open = 0; return -1; }
    if (s->clen >= 0 && n > s->clen - s->got) n = (int)(s->clen - s->got);
    deadline_reset();
    int r = conn_read(&s->c, b, n);
    if (r <= 0) { s->done = s->clen < 0 && !s->chunked; s->ka = 0; s->open = 0; conn_close(&s->c); return -1; }
    s->got += r;
    if (s->chunked) {
        r = rs_dechunk(&s->ck, b, r);
        if (s->ck.end) { s->done = 1; s->open = 0; if (!s->ka) conn_close(&s->c); if (!r) return -1; }
        if (!r) return 0;
    }
    if (s->clen >= 0 && s->got >= s->clen) { s->done = 1; s->open = 0; if (!s->ka) conn_close(&s->c); }
    return rs_icy_feed(&s->icy, b, r, b, newtitle);
}

/* cuerpo gzip (algunas CDN lo mandan aunque se pida "identity") -> texto; el inflado es el de
 * stb_image (ya va dentro de hibynet por los PNG). Devuelve un bufer nuevo o 0. */
static char *rd_gunzip(const unsigned char *b, int n, int *on)
{
    if (n < 18 || b[0] != 0x1F || b[1] != 0x8B || b[2] != 8) return 0;
    int fl = b[3], p = 10;
    if (fl & 4) { if (p + 2 > n) return 0; p += 2 + (b[p] | b[p + 1] << 8); }
    if (fl & 8) { while (p < n && b[p]) p++; p++; }
    if (fl & 16) { while (p < n && b[p]) p++; p++; }
    if (fl & 2) p += 2;
    if (p >= n - 8) return 0;
    for (int cap = 64 << 10; cap <= (4 << 20); cap *= 4) {
        char *o = malloc(cap); if (!o) return 0;
        int r = stbi_zlib_decode_noheader_buffer(o, cap, (const char *)b + p, n - p - 8);
        if (r >= 0) { *on = r; return o; }
        free(o);
    }
    return 0;
}
static unsigned char rd_buf[RD_SNIFF + 8192];        /* lectura de la red */
static unsigned char rd_sb[RD_SNIFF + 8192];         /* audio que espera un punto de sincronismo */
static unsigned char rd_es[8192 + 400];              /* salida del demultiplexor TS */

/* Abre url hasta dar con audio: sigue listas (cada entrada, en orden) y el truco "/;" de
 * Shoutcast 1 cuando la raiz devuelve su pagina HTML. 0 = s abierto y rd_buf[0..*pren) con
 * los primeros bytes de audio; 1 = es HLS (hls_url, la lista ya cerrada). */
static int rd_connect(rd_src *s, const char *url, int depth, int *pren, char *hls_url, int hcap, char *err, int ecap)
{
    if (depth > 4) { snprintf(err, ecap, "nostream"); return -1; }
    if (rd_http(s, url, 0, err, ecap)) return -1;
    int kind = rs_ctype_kind(s->ctype), n = 0;
    if (kind == RS_UNKNOWN) kind = rs_url_kind(s->url);
    while (n < 512) { int r = rd_read(s, rd_buf + n, 1024, 0); if (r < 0) break; n += r; }
    if (s->gz || (n >= 2 && rd_buf[0] == 0x1F && rd_buf[1] == 0x8B)) {   /* una lista comprimida (CDN que no hace caso de "identity") */
        while (n < RD_SNIFF) { int r = rd_read(s, rd_buf + n, 4096, 0); if (r < 0) break; n += r; }
        rd_close(s);
        int tn = 0; char *t = rd_gunzip(rd_buf, n, &tn);
        if (!t) { snprintf(err, ecap, "nostream"); return -1; }
        if (tn > RD_SNIFF) tn = RD_SNIFF;
        memcpy(rd_buf, t, tn); free(t); n = tn;
        if (kind != RS_PLAYLIST && kind != RS_HLS) kind = RS_PLAYLIST;
    }
    int sk = rs_sniff_kind(rd_buf, n);
    if (sk != RS_UNKNOWN) kind = sk;                           /* los bytes mandan sobre la cabecera */
    else if (kind == RS_PLAYLIST || kind == RS_HTML || kind == RS_HLS) { struct rs_frame f; if (n > 64 && rs_sync(rd_buf, n, RS_UNKNOWN, &f) >= 0) kind = f.kind; }
    if (kind == RS_PLAYLIST || kind == RS_HLS) {
        while (n < RD_SNIFF) { int r = rd_read(s, rd_buf + n, 4096, 0); if (r < 0) break; n += r; }
        rd_close(s);
        char urls[4][RS_URL];
        int k = rs_playlist_parse((const char *)rd_buf, n, urls, 4);
        if (k == -2) { snprintf(hls_url, hcap, "%s", s->url); return 1; }
        if (k <= 0) { snprintf(err, ecap, "nostream"); return -1; }
        for (int i = 0; i < k; i++) {
            char abs[2100]; url_join(s->url, urls[i], abs, sizeof abs);
            int r = rd_connect(s, abs, depth + 1, pren, hls_url, hcap, err, ecap);
            if (r >= 0) return r;
            if (!strcmp(err, "codec") || !strcmp(err, "enc")) break;
        }
        return -1;
    }
    if (kind == RS_HTML) {
        url_t u; char alt[2100];
        rd_close(s);
        if (!url_parse(url, &u) && !strcmp(u.path, "/") && snprintf(alt, sizeof alt, "%s://%s:%d/;", u.tls ? "https" : "http", u.host, u.port) < (int)sizeof alt)
            return rd_connect(s, alt, depth + 1, pren, hls_url, hcap, err, ecap);
        snprintf(err, ecap, "nostream"); return -1;
    }
    if (kind == RS_OGG || kind == RS_FLAC) { rd_close(s); snprintf(err, ecap, "codec"); return -1; }
    s->kind = kind;
    *pren = n;
    return 0;
}

/* ------------------------------------------------------------------ el lector (reproductor) */
static int rd_pid;
static int rd_player_pid(void)
{
    char p[64], b[64];
    for (int pass = 0; pass < 2; pass++) {
        if (rd_pid > 0) {
            snprintf(p, sizeof p, "/proc/%d/comm", rd_pid);
            int f = open(p, O_RDONLY); ssize_t n = f >= 0 ? read(f, b, sizeof b - 1) : -1;
            if (f >= 0) close(f);
            if (n > 0) { b[n] = 0; b[strcspn(b, "\n")] = 0; if (!strcmp(b, rd.player)) return rd_pid; }
            rd_pid = 0;
        }
        if (pass) break;
        DIR *d = opendir("/proc"); if (!d) return 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!isdigit((unsigned char)e->d_name[0])) continue;
            snprintf(p, sizeof p, "/proc/%.20s/comm", e->d_name);
            int f = open(p, O_RDONLY); if (f < 0) continue;
            ssize_t n = read(f, b, sizeof b - 1); close(f);
            if (n <= 0) continue;
            b[n] = 0; b[strcspn(b, "\n")] = 0;
            if (!strcmp(b, rd.player) && atoi(e->d_name) != getpid()) { rd_pid = atoi(e->d_name); break; }
        }
        closedir(d);
    }
    return rd_pid;
}
/* fichero de audio por la extension (lo que abre el motor para sonar) */
static int rd_is_audio(const char *t)
{
    static const char *const X[] = { ".mp3", ".flac", ".wav", ".ape", ".m4a", ".aac", ".ogg", ".opus", ".dsf", ".dff", ".wma",
                                     ".aif", ".aiff", ".alac", ".wv", ".mka", ".mp2", ".iso", ".dst", 0 };
    const char *d = strrchr(t, '.'); if (!d) return 0;
    for (int i = 0; X[i]; i++) if (!strcasecmp(d, X[i])) return 1;
    return 0;
}
/* 0 = el reproductor no tiene el fichero abierto; 1 = *rpos es su posicion (la mayor que no
 * pasa de wpos); 2 = solo hay posiciones por delante de la escritura (*rpos la menor).
 * *other = 1 si tiene abierto un fichero de audio que no es nuestro y ninguno de los nuestros
 * (ni el de la generacion anterior): suena otra musica. */
static int rd_reader(const char *real, long long wpos, long long *rpos, int *other)
{
    *other = 0;
    int pid = rd_player_pid(); if (pid <= 0) return 0;
    int ours = 0, foreign = 0; size_t dl = strlen(rd.realdir);
    char d[64]; snprintf(d, sizeof d, "/proc/%d/fd", pid);
    DIR *D = opendir(d); if (!D) return 0;
    long long best = -1, over = -1; struct dirent *e;
    while ((e = readdir(D))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char l[96], t[512];
        snprintf(l, sizeof l, "%s/%.12s", d, e->d_name);
        ssize_t n = readlink(l, t, sizeof t - 1); if (n <= 0) continue;
        t[n] = 0;
        if (!strncmp(t, rd.realdir, dl) && t[dl] == '/') ours = 1;
        else if (t[0] == '/' && rd_is_audio(t)) foreign = 1;
        if (strcmp(t, real)) continue;
        snprintf(l, sizeof l, "/proc/%d/fdinfo/%.12s", pid, e->d_name);
        int f = open(l, O_RDONLY); if (f < 0) continue;
        n = read(f, t, 80); close(f);
        if (n <= 0) continue;
        t[n] = 0;
        const char *q = strstr(t, "pos:"); if (!q) continue;
        long long pos = atoll(q + 4);
        if (pos <= wpos) { if (pos > best) best = pos; } else if (over < 0 || pos < over) over = pos;
    }
    closedir(D);
    *other = foreign && !ours;
    if (best >= 0) { *rpos = best; return 1; }
    if (over >= 0) { *rpos = over; return 2; }
    return 0;
}

/* ------------------------------------------------------------------ fichero de salida */
/* segundos escritos por delante de lo que ya leyo el reproductor (media de bytes/s del fichero) */
static double rd_lead(void)
{
    if (rd.wpos <= 0 || rd.dur.sec <= 0) return 0;
    long long ahead = rd.wpos - (rd.rpos < rd.wpos ? rd.rpos : rd.wpos);
    return rd.dur.sec * (double)ahead / (double)rd.wpos;
}
static int rd_newfile(void)
{
    const char *x = rd.kind == RS_AAC ? "aac" : "mp3";
    if (rd.gen > 1) snprintf(rd.file, sizeof rd.file, "%s/%s (%d).%s", rd.dir, rd.name, rd.gen, x);
    else snprintf(rd.file, sizeof rd.file, "%s/%s.%s", rd.dir, rd.name, x);
    unlink(rd.file);
    rd.fd = open(rd.file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (rd.fd < 0) { rd.file[0] = 0; return -1; }
    char rp[4096];                                              /* /proc/<pid>/fd da la ruta canonica */
    snprintf(rd.real, sizeof rd.real, "%s", realpath(rd.file, rp) ? rp : rd.file);
    rd.wpos = rd.rpos = 0; memset(&rd.dur, 0, sizeof rd.dur);
    return 0;
}
static void rd_write(const unsigned char *b, int n)
{
    if (n <= 0 || rd.capped) return;
    if (rd.wpos + n > rd.cap) { rd.capped = 1; vlog("radio: tope de la sesion (%lld bytes)", rd.wpos); rd_state(rd.st, 0); return; }
    if (rd.fd < 0 && rd_newfile()) rd_end("error", errno == ENOSPC ? "space" : "io", E_IO);
    ssize_t w = pwrite(rd.fd, b, n, rd.wpos);
    if (w != n) rd_end("error", w < 0 && errno == ENOSPC ? "space" : "io", E_IO);
    rd.wpos += n;
    rs_dur_feed(&rd.dur, b, n);
}
/* audio que sale de la red o del demultiplexor. Tras un corte (o al empezar) se espera a 3
 * tramas seguidas; el codec no puede cambiar a mitad. -1 = no es MP3 ni AAC. */
static int rd_out(const unsigned char *b, int n)
{
    if (!rd.need_sync) { rd_write(b, n); return 0; }
    int k = n; if (k > (int)sizeof rd_sb - rd.nsb) k = (int)sizeof rd_sb - rd.nsb;
    memcpy(rd_sb + rd.nsb, b, k); rd.nsb += k;
    struct rs_frame f;
    int off = rs_sync(rd_sb, rd.nsb, rd.kind ? rd.kind : RS_UNKNOWN, &f);
    if (off < 0) {
        if (rd.nsb >= RD_SNIFF) {
            if (!rd.kind) return -1;
            memmove(rd_sb, rd_sb + rd.nsb - 4096, 4096); rd.nsb = 4096;  /* basura tras un corte: se sigue buscando */
        }
        return 0;
    }
    if (!rd.kind) {
        rd.kind = f.kind; rd.codec = f.kind == RS_AAC ? "AAC" : "MP3";
        if (f.kind == RS_MP3 && f.kbps) rd.br = f.kbps;
    }
    rd.need_sync = 0;
    int m = rd.nsb - off; rd.nsb = 0;
    rd_write(rd_sb + off, m);
    if (k < n) rd_write(b + k, n - k);
    return 0;
}
/* una carpeta de la tarjeta solo vale si la tarjeta esta montada: si no, el punto de montaje es
 * una carpeta vacia de la memoria interna (casi llena). Fuera de /usr/data/mnt, /data/mnt y /mnt
 * (pruebas en el PC) no se mira. */
static int rd_mounted_ok(const char *dir)
{
    static const char *const P[] = { "/usr/data/mnt/", "/data/mnt/", "/mnt/" };
    const char *rest = 0;
    for (int i = 0; i < 3 && !rest; i++) if (!strncmp(dir, P[i], strlen(P[i]))) rest = dir + strlen(P[i]);
    if (!rest) return 1;
    char dev[64]; int k = (int)strcspn(rest, "/");
    if (!k || k >= (int)sizeof dev) return 0;
    memcpy(dev, rest, k); dev[k] = 0;                            /* "sd_0" */
    FILE *f = fopen("/proc/mounts", "r"); char l[512]; int ok = 0;
    while (f && !ok && fgets(l, sizeof l, f)) {
        char mp[300]; if (sscanf(l, "%*s %299s", mp) != 1) continue;
        for (int i = 0; i < 3; i++) { size_t pl = strlen(P[i]); if (!strncmp(mp, P[i], pl) && !strcmp(mp + pl, dev)) ok = 1; }
    }
    if (f) fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ HLS */
static struct {
    char purl[2100];               /* lista de medios (tras la maestra y las redirecciones) */
    struct rs_hls pl;
    long long last, seq;           /* ultima secuencia escrita (-1 ninguna); la que se baja */
    int cur, in_seg, segkind, hn, segfails, empty, loaded; long long segout;
    long id3;                      /* bytes de ID3 que quedan por saltar */
    unsigned char hb[10];
    double t_next;
    struct rs_ts ts;
} hl;

/* pide la lista; 0 = cargada en hl.pl (una maestra se sigue sola) */
static int rd_hls_load(rd_src *s, char *err, int ecap)
{
    for (int depth = 0; depth < 3; depth++) {
        if (rd_http(s, hl.purl, 1, err, ecap)) return -1;
        snprintf(hl.purl, sizeof hl.purl, "%s", s->url);
        rs_hls_init(&hl.pl);
        long long tot = 0; unsigned char *gz = 0; int gzn = 0, gzcap = 0;
        while (s->open) {
            if (!rd_wait(s, 15000)) { free(gz); rd_close(s); snprintf(err, ecap, "net"); return -1; }
            int r = rd_read(s, rd_buf, 8192, 0);
            if (r < 0) break;
            if (!tot && r >= 2 && rd_buf[0] == 0x1F && rd_buf[1] == 0x8B) s->gz = 1;
            if ((tot += r) > (4 << 20)) { free(gz); rd_close(s); snprintf(err, ecap, "nostream"); return -1; }
            if (!s->gz) { rs_hls_feed(&hl.pl, (const char *)rd_buf, r); continue; }
            if (gzn + r > gzcap) {                       /* comprimida: se junta entera (son pocos KB) */
                int nc = gzcap ? gzcap * 2 : 16384; while (nc < gzn + r) nc *= 2;
                unsigned char *ng = nc <= (1 << 20) ? realloc(gz, nc) : 0;
                if (!ng) { free(gz); rd_close(s); snprintf(err, ecap, "nostream"); return -1; }
                gz = ng; gzcap = nc;
            }
            memcpy(gz + gzn, rd_buf, r); gzn += r;
        }
        if (!s->done) { free(gz); snprintf(err, ecap, "net"); return -1; }
        if (s->gz) {
            int tn = 0; char *txt = rd_gunzip(gz, gzn, &tn); free(gz);
            if (!txt) { snprintf(err, ecap, "nostream"); return -1; }
            vlog("radio: lista comprimida (%d -> %d bytes)", gzn, tn);
            rs_hls_feed(&hl.pl, txt, tn); free(txt);
        }
        rs_hls_end(&hl.pl);
        if (!hl.pl.ok) { snprintf(err, ecap, "nostream"); return -1; }
        if (!hl.pl.master) return 0;
        const char *v = rs_hls_variant(&hl.pl);
        if (!v[0]) { snprintf(err, ecap, "nostream"); return -1; }
        char nu[2100]; url_join(hl.purl, v, nu, sizeof nu);
        vlog("radio: lista maestra (%d variantes) -> %s", hl.pl.nvar, nu);
        snprintf(hl.purl, sizeof hl.purl, "%s", nu);
    }
    snprintf(err, ecap, "nostream");
    return -1;
}
/* bytes de un segmento: TS -> demultiplexor; ADTS/MP3 empaquetado -> sin su ID3. hl.segout
 * cuenta el audio que salio del segmento (sin nada en 3 seguidos: no es audio que sepamos tocar) */
static int rd_seg_out(const unsigned char *b, int n) { hl.segout += n; return n > 0 ? rd_out(b, n) : 0; }
static int rd_seg_data(const unsigned char *b, int n)
{
    while (n > 0) {
        if (hl.segkind == 0 || hl.segkind == 3) {        /* principio del segmento, o tras una etiqueta ID3 (puede haber otra) */
            while (hl.hn < 10 && n > 0) { hl.hb[hl.hn++] = *b++; n--; }
            if (hl.hn < 10) return 0;
            long l = rs_id3_len(hl.hb, hl.hn);
            if (hl.segkind == 0 && hl.hb[0] == 0x47) { hl.segkind = 1; if (rd_seg_out(rd_es, rs_ts_feed(&hl.ts, hl.hb, hl.hn, rd_es))) return -1; continue; }
            hl.segkind = 2; hl.hn = 0;
            if (l > 0) { hl.id3 = l - 10; if (!hl.id3) hl.segkind = 3; continue; }
            if (rd_seg_out(hl.hb, 10)) return -1;
            continue;
        }
        if (hl.segkind == 2 && hl.id3 > 0) {
            long k = n < hl.id3 ? n : hl.id3; hl.id3 -= k; b += k; n -= (int)k;
            if (!hl.id3) hl.segkind = 3;
            continue;
        }
        if (hl.segkind == 2) return rd_seg_out(b, n);
        int k = n > 8192 ? 8192 : n;
        if (rd_seg_out(rd_es, rs_ts_feed(&hl.ts, b, k, rd_es))) return -1;
        b += k; n -= k;
    }
    return 0;
}

static int cmd_radio(int argc, char **argv)
{
    if (argc < 3 || !rs_url_ok(argv[0])) return E_USAGE;
    const char *url = argv[0];
    double lead_s = argc > 3 ? atof(argv[3]) : 15;
    long long cap = (long long)(argc > 4 ? atoi(argv[4]) : 128) << 20;
    if (lead_s < 2 || lead_s > 120) lead_s = 15;
    if (cap < (8LL << 20) || cap > (4000LL << 20)) cap = 128LL << 20;
    snprintf(rd.dir, sizeof rd.dir, "%s", argv[1]);
    const char *sdir = argc > 5 && argv[5][0] ? argv[5] : rd.dir;
    rs_safe_name(argv[2], rd.name, sizeof rd.name);
    mkdir(sdir, 0755);
    snprintf(rd.state, sizeof rd.state, "%s/state", sdir);
    snprintf(rd.tmp, sizeof rd.tmp, "%s/state.tmp", sdir);
    snprintf(rd.pidf, sizeof rd.pidf, "%s/pid", sdir);
    rd.player = getenv("HIBYRADIO_PLAYER") ? getenv("HIBYRADIO_PLAYER") : "hiby_player";
    rd.noreader = getenv("HIBYRADIO_NOREADER") != NULL;
    if (getenv("HIBYRADIO_HIGH_S") && atof(getenv("HIBYRADIO_HIGH_S")) >= 5) rd_high = atof(getenv("HIBYRADIO_HIGH_S"));
    if (rd_high < lead_s * 2) rd_high = lead_s * 2;
    if (getenv("HIBYRADIO_CAP_KB") && atoll(getenv("HIBYRADIO_CAP_KB")) >= 64) cap = atoll(getenv("HIBYRADIO_CAP_KB")) * 1024;   /* pruebas */
    rd.ppid = getppid();
    { FILE *pf = fopen(rd.pidf, "w"); if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); } }
    signal(SIGTERM, rd_sig); signal(SIGINT, rd_sig); signal(SIGHUP, rd_sig);
    g_timeout = 12; g_idle = 0;
    rd.gen = 0; rd.codec = "";
    rd_state("connecting", 0);
    /* la carpeta: en la tarjeta montada y con sitio */
    if (!rd_mounted_ok(rd.dir)) rd_end("error", "nosd", E_IO);
    { char up[300]; snprintf(up, sizeof up, "%s", rd.dir);          /* crea los padres (ByOS/cache/radio) */
      for (char *p = up + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(up, 0755); *p = '/'; }
      mkdir(up, 0755); }
    { char rp[4096]; snprintf(rd.realdir, sizeof rd.realdir, "%s", realpath(rd.dir, rp) ? rp : rd.dir); }
    struct statvfs vf;
    if (statvfs(rd.dir, &vf)) rd_end("error", "nosd", E_IO);
    long long avail = (long long)vf.f_bavail * (long long)vf.f_frsize;
    if (avail - RD_MINFREE < (8LL << 20)) rd_end("error", "space", E_IO);
    rd.cap = cap < avail - RD_MINFREE ? cap : avail - RD_MINFREE;

    static rd_src s;
    memset(&s, 0, sizeof s); s.c.fd = -1;
    char err[48] = "net"; int n = 0, rc = -1;
    for (int attempt = 0; attempt < 2 && rc < 0; attempt++) {          /* un reintento: muchos servidores fallan a la primera */
        if (attempt) usleep(700 * 1000);
        rc = rd_connect(&s, url, 0, &n, hl.purl, sizeof hl.purl, err, sizeof err);
        if (rc < 0 && strcmp(err, "net")) break;
    }
    if (rc < 0) rd_end("error", err, !strcmp(err, "net") ? E_NET : E_NONE);
    rd.hls = rc == 1;
    if (rd.hls) {
        hl.last = -1; hl.cur = -1; hl.t_next = 0; rs_ts_init(&hl.ts);
        vlog("radio: HLS %s", hl.purl);
    } else {
        if (s.icy.title[0]) snprintf(rd.title, sizeof rd.title, "%s", s.icy.title);
        rd.br = s.br;                                                 /* las tramas MP3 lo corrigen */
    }
    rd.gen = 1; rd.need_sync = 1; rd.nsb = 0;
    rd.pct = 0; rd_state("buffering", 0);
    if (!rd.hls && n > 0 && rd_out(rd_buf, n)) rd_end("error", "codec", E_NONE);
    n = 0;
    g_timeout = 15;

    double need = lead_s;                                             /* segundos para anunciar esta generacion */
    int ready = 0, seen = 0, seen_any = 0, beyond = 0, retries = 0, stall_low = 0;
    double t_ready = 0, t_gone = 0, t_other = 0, t_mon = 0, t_data = now_s(), t_retry = 0, t_pct = 0, t_old = 0, t_move = 0, t_br = 0, t_st = 0;
    double lead_seen = 0;                                             /* lo que tenia por delante en la ultima lectura que vimos */
    long long rlast = -1;

    for (;;) {
        double now = now_s();
        if (getppid() != rd.ppid) rd_end("stopped", 0, E_OK);          /* el shell murio: nadie nos controla */
        double ahead = rd_lead();
        if (rd.noreader && ready) ahead = 0;
        int want = !rd.capped && !rd.eos && ahead < rd_high;
        /* ---- red: flujo continuo */
        if (!rd.hls) {
            if (!s.open) {
                if (want && now >= t_retry && ahead < RD_LOW) {
                    char e2[48]; int k = 0;
                    int r = rd_connect(&s, url, 0, &k, hl.purl, sizeof hl.purl, e2, sizeof e2);
                    if (r == 0) { rd.need_sync = 1; rd.nsb = 0; if (rd_out(rd_buf, k)) rd_end("error", "codec", E_NONE); t_data = now_s(); retries = 0; }
                    else if (r == 1) rd_end("error", "codec", E_NONE);  /* paso a ser HLS: raro; mejor parar */
                    else if (++retries >= 6 || (strcmp(e2, "net") && retries >= 2)) rd_end("error", strcmp(e2, "net") ? e2 : "net", E_NET);
                    else t_retry = now_s() + (retries < 3 ? 1.0 : 3.0);
                } else usleep(150 * 1000);
            } else if (!want) {
                if (rd.capped || rd.eos) rd_close(&s);
                usleep(200 * 1000);                                    /* pausa: que espere la red */
                t_data = now;
            } else if (rd_wait(&s, 200)) {
                int nt = 0, r = rd_read(&s, rd_buf, 8192, &nt);
                if (r < 0) { vlog("radio: conexion cortada"); rd_close(&s); t_retry = now_s() + 0.5; }
                else if (r > 0) { if (rd_out(rd_buf, r)) rd_end("error", "codec", E_NONE); t_data = now; }
                if (nt) { snprintf(rd.title, sizeof rd.title, "%s", s.icy.title); rd_state(rd.st, 0); }
            } else if (now - t_data > 15) { vlog("radio: sin datos 15 s"); rd_close(&s); t_retry = now_s(); }
        } else {
        /* ---- red: HLS */
            if (!want && !hl.in_seg) usleep(200 * 1000);
            else if (!hl.in_seg) {
                if (hl.cur < 0 || hl.cur >= hl.pl.nseg) {
                    if (now < hl.t_next || now < t_retry) usleep(100 * 1000);
                    else {
                        char e2[48] = "net";
                        if (rd_hls_load(&s, e2, sizeof e2)) {
                            vlog("radio: lista HLS: %s", e2);
                            /* una vez cargada, un fallo de la lista es pasajero (hay CDN que a veces la sirven comprimida) */
                            if (++retries >= 6 || (!hl.loaded && strncmp(e2, "http", 4) && strcmp(e2, "net") && retries >= 2)) rd_end("error", hl.loaded ? "net" : e2, E_NET);
                            t_retry = now_s() + (retries < 3 ? 1.0 : 3.0);
                        } else if (hl.pl.enc) rd_end("error", "enc", E_NONE);
                        else if (hl.pl.fmp4 || hl.pl.brange) rd_end("error", "codec", E_NONE);
                        else {
                            retries = 0; hl.loaded = 1;
                            int gap = 0, first = rs_hls_first(&hl.pl, hl.last, (int)(need * 1000), &gap);
                            if (gap && hl.last >= 0) { vlog("radio: HLS: hueco (ultimo %lld, ventana desde %lld)", hl.last, hl.pl.seq0); rd.need_sync = 1; rd.nsb = 0; }
                            hl.cur = first;
                            double td = hl.pl.target > 0 ? hl.pl.target : 6;
                            hl.t_next = now_s() + (first < hl.pl.nseg ? td : td / 2);
                            if (first >= hl.pl.nseg && hl.pl.endlist) { rd.eos = 1; vlog("radio: HLS: fin de la emision"); }
                            vlog("radio: HLS lista: seq %lld..%lld, se baja desde %d", hl.pl.seq0, hl.pl.seq0 + hl.pl.nseg - 1, first);
                        }
                    }
                } else {
                    const struct rs_hseg *sg = rs_hls_seg(&hl.pl, hl.cur);
                    if (!sg) hl.cur++;
                    else {
                        char su[2100], e2[48]; url_join(hl.purl, sg->uri, su, sizeof su);
                        if (rd_http(&s, su, 1, e2, sizeof e2)) {
                            vlog("radio: segmento %lld: %s", sg->seq, e2);
                            if (!strncmp(e2, "http 4", 6) || ++hl.segfails >= 3) {   /* caducado o imposible: se salta */
                                hl.last = sg->seq; hl.cur++; hl.segfails = 0; rd.need_sync = 1; rd.nsb = 0;
                            } else usleep(500 * 1000);
                        } else {
                            hl.in_seg = 1; hl.seq = sg->seq; hl.segkind = 0; hl.hn = 0; hl.id3 = 0; hl.segout = 0;
                            rs_ts_cut(&hl.ts); t_data = now_s();
                        }
                    }
                }
            } else if (!s.open || rd_wait(&s, 200)) {
                int r = s.open ? rd_read(&s, rd_buf, 8192, 0) : -1;
                if (r > 0) { if (rd_seg_data(rd_buf, r)) rd_end("error", "codec", E_NONE); t_data = now; }
                if (r < 0) {
                    hl.in_seg = 0;
                    if (!s.done) { vlog("radio: segmento %lld cortado", hl.seq); rd.need_sync = 1; rd.nsb = 0; }
                    if (!hl.segout) { if (hl.ts.latm || ++hl.empty >= 3) rd_end("error", "codec", E_NONE); }   /* sin audio que sepamos tocar */
                    else hl.empty = 0;
                    hl.last = hl.seq; hl.cur++; hl.segfails = 0;
                }
            } else if (now - t_data > 15) { vlog("radio: segmento sin datos 15 s"); rd_close(&s); hl.in_seg = 0; rd.need_sync = 1; rd.nsb = 0; }
        }
        now = now_s();
        if (rd.kind == RS_AAC && rd.dur.sec > 4 && now - t_br > 10) {   /* AAC: kbit/s de lo escrito */
            t_br = now; rd.br = (int)(rd.wpos * 8 / 1000 / rd.dur.sec + 0.5);
        }

        /* ---- anuncio */
        if (!ready) {
            if (rd.fd >= 0 && (rd.dur.sec >= need || (rd.capped || rd.eos))) {
                ready = 1; rd.pct = 100; t_ready = now; t_mon = 0; rd_state("ready", 0);
                vlog("radio: gen %d lista (%lld bytes, %.1f s)", rd.gen, rd.wpos, rd.dur.sec);
            } else if (now - t_pct > 0.4) { t_pct = now; int p = (int)(rd.dur.sec * 100 / need); if (p > 99) p = 99; if (p != rd.pct) { rd.pct = p; rd_state("buffering", 0); } }
        }
        if (rd.old[0] && t_old && now > t_old) rd_drop_old();

        /* ---- lector (tambien mientras se llena una generacion nueva, si ya sonamos antes) */
        if (rd.noreader || (!ready && !seen_any) || now - t_mon < 0.3) continue;
        t_mon = now;
        long long rp = 0; int other = 0, k = rd_reader(rd.real, rd.wpos, &rp, &other);
        /* el reproductor abrio otra musica y solto la nuestra: se acabo (sin esto, tras un corte
         * el shell mandaria la generacion nueva al reproductor encima de lo que eligio el usuario) */
        if (seen_any && other) { if (!t_other) t_other = now; else if (now - t_other > 2) rd_end("stopped", 0, E_OK); }
        else t_other = 0;
        if (!ready) continue;
        if (k == 0) {
            if (!seen) { if (now - t_ready > 20) rd_end("error", "player", E_NONE); continue; }
            if (lead_seen < RD_ATEND) {                                /* se paro al llegar al final (si no, lo cerro alguien) */
                if (rd.capped) rd_end("stopped", "cap", E_OK);
                if (rd.eos) rd_end("stopped", "end", E_OK);
                goto newgen;
            }
            if (!t_gone) t_gone = now;
            else if (now - t_gone > 3) rd_end("stopped", 0, E_OK);     /* cerro el fichero: hay otra musica */
            continue;
        }
        t_gone = 0;
        if (!seen) { seen = seen_any = 1; rd_drop_old(); rd.opened = 1; rlast = -1; vlog("radio: el reproductor abrio el fichero (pos %lld)", rp); }
        if (k == 2) { if (++beyond >= 3) goto newgen; continue; }      /* leyo mas alla de lo escrito: fichero truncado? */
        beyond = 0;
        rd.rpos = rp;
        double ld = rd_lead();
        if (rlast >= 0 && rp + 65536 < rlast && lead_seen < RD_ATEND) goto newgen;   /* llego al final y volvio a empezar (repetir) */
        if (rp != rlast) { rlast = rp; t_move = now; lead_seen = ld; stall_low = 0; }
        else if (lead_seen < RD_ATEND && now - t_move > 20 && !stall_low) {   /* quieto en el final: parado (no en pausa) */
            stall_low = 1;
            if (rd.capped) rd_end("stopped", "cap", E_OK);
            if (rd.eos) rd_end("stopped", "end", E_OK);
            goto newgen;
        }
        if (now - t_st > 2) { t_st = now; rd_state(rd.st, 0); }
        continue;

    newgen:
        vlog("radio: corte: el lector alcanzo a la escritura (w=%lld r=%lld, %.1f s por delante)", rd.wpos, rd.rpos, lead_seen);
        rd_drop_old();
        snprintf(rd.old, sizeof rd.old, "%s", rd.file); rd.oldfd = rd.fd; rd.fd = -1; rd.file[0] = rd.real[0] = 0;
        if (ftruncate(rd.oldfd, 0)) {}                                  /* ya no lo lee nadie */
        if (need < 40) need += 5;                                       /* la red no da para mas: mas colchon */
        rd.gen++;
        rd.wpos = rd.rpos = 0; memset(&rd.dur, 0, sizeof rd.dur);
        ready = seen = beyond = 0; t_gone = 0; t_old = now_s() + 30; lead_seen = 0; rlast = -1;
        rd.need_sync = 1; rd.nsb = 0;
        if (!rd.hls && s.open) { rd_close(&s); t_retry = 0; }          /* conexion nueva: el servidor manda su rafaga */
        rd.pct = 0; rd.opened = 0; rd_state("buffering", 0);
    }
    return E_OK;
}
