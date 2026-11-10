/* hibynet_radio.inc.c - subcomando "radio" de hibynet (se incluye desde hibynet.c).
 *
 *   hibynet radio URL DIR NAME [PRESIZE_MB] [PREBUF_KB]
 *
 * Convierte una emisora de internet en un FICHERO que el reproductor de fabrica toca como
 * cualquier otro (lista "sin escanear" de playctl): asi el volumen, el ecualizador, el
 * Bluetooth y el reparto de la salida siguen siendo suyos, y al poner musica local la radio
 * se aparta sola.
 *   1. Resuelve URL: redirecciones, listas .m3u/.pls (prueba sus entradas en orden) y el
 *      "ICY 200 OK" de Shoutcast. HLS, Ogg/Opus y FLAC se rechazan (err=hls / err=codec).
 *   2. Pide el audio con Icy-MetaData: 1, separa los metadatos (titulo) y busca el primer
 *      punto con 3 tramas MP3 o ADTS seguidas: el fichero empieza en una trama entera.
 *   3. Escribe DIR/NAME.mp3|.aac. Con PRESIZE_MB > 0 (lo normal: DIR en tmpfs) el fichero se
 *      crea HUECO con ese tamano logico - el motor guarda el tamano al abrir pero lee con
 *      fread, asi que ve los datos segun llegan - y se van perforando (PUNCH_HOLE) los
 *      bloques que el reproductor ya leyo: en RAM solo queda ~1 MB. Con 0 el fichero crece
 *      sin mas (tarjeta SD).
 *   4. Vigila la posicion de lectura del reproductor en /proc/<pid>/fdinfo:
 *        - si no abre el fichero en 20 s            -> err=player
 *        - si lo cierra (> 3 s)                     -> st=stopped (puso otra musica) y sale
 *        - si abre otro audio y no tiene ninguno nuestro (> 2 s; tambien mientras se llena una
 *          generacion nueva)                        -> st=stopped y sale
 *        - si esta en pausa (> 1 MB sin leer)       -> deja de pedir a la red
 *        - si alcanza a la escritura (corte de red) -> generacion nueva: otro fichero y de
 *          nuevo st=buffering -> st=ready con gen+1 (el shell vuelve a mandar la lista)
 *      Igual al agotar el tamano logico (relevo de fichero).
 *   5. SIGTERM / muerte del padre: trunca y borra sus ficheros y sale. No deja nada atras.
 *
 * Estado para el shell en DIR/state (escritura atomica), lineas clave=valor:
 *   st=connecting|buffering|ready|stopped|error  gen=N  file=RUTA  codec=MP3|AAC  br=KBPS
 *   pct=0..100 (llenado del prebufer)  open=0|1 (el reproductor ya abrio esta generacion)
 *   title=...  err=net|http NNN|hls|codec|nostream|player|io
 *
 * Entorno (pruebas): HIBYRADIO_PLAYER=comm del proceso lector (por defecto hiby_player);
 * HIBYRADIO_NOREADER=1 no vigila a nadie (solo descarga); HIBYRADIO_HIGH_KB=tope sin leer. */
#include <dirent.h>
#include "radio_stream.inc.c"

#define RD_UA "HiByOS-Radio/1.0 (https://github.com/Grohle/hibyos)"
static long long rd_high = 1024 * 1024;   /* el tmpfs es RAM (quedan ~5 MB libres): poco por delante; sin leer por delante del lector: se deja de pedir a la red (HIBYRADIO_HIGH_KB) */
#define RD_HIGH rd_high
#define RD_LOW (rd_high / 4)          /* reconectar (tras una pausa larga) cuando quede menos que esto */
#define RD_KEEP (192 * 1024)       /* por detras del lector que no se perfora (pequenos retrocesos) */
#define RD_NOPUNCH_MAX (3 << 20)   /* tmpfs sin PUNCH_HOLE: relevo de fichero a los 3 MB */
#define RD_SNIFF (64 * 1024)
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 1
#define FALLOC_FL_PUNCH_HOLE 2
#endif

typedef struct {
    conn_t c; int open;
    int metaint, kind, br, chunked;
    struct rs_chunk ck;
    char ctype[96], url[2100];
    struct rs_icy icy;
} rd_src;

static struct {
    char dir[300], state[320], tmp[328], pidf[320], name[80];
    char file[420], real[460], old[420], realdir[460];
    int fd, oldfd, gen, br, pct, opened;
    const char *codec, *st;
    char title[256];
    const char *player; int noreader, ppid;
} rd = { .fd = -1, .oldfd = -1 };

static void rd_state(const char *st, const char *err)
{
    char b[1200];
    rd.st = st;
    int n = snprintf(b, sizeof b, "st=%s\ngen=%d\nfile=%s\ncodec=%s\nbr=%d\npct=%d\nopen=%d\ntitle=%s\nerr=%s\n",
                     st, rd.gen, rd.file, rd.codec ? rd.codec : "", rd.br, rd.pct, rd.opened, rd.title, err ? err : "");
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
static int rd_dechunk(rd_src *s, unsigned char *b, int n) { return s->chunked ? rs_dechunk(&s->ck, b, n) : n; }
static void rd_close(rd_src *s) { if (s->open) conn_close(&s->c); s->open = 0; }

/* GET con Icy-MetaData siguiendo redirecciones. 0 = s abierto al principio del cuerpo. */
static int rd_http(rd_src *s, const char *url0, char *err, int ecap)
{
    char url[2100]; snprintf(url, sizeof url, "%s", url0);
    for (int hop = 0; hop < 8; hop++) {
        url_t u;
        memset(s, 0, sizeof *s); s->c.fd = -1;
        if (url_parse(url, &u)) { snprintf(err, ecap, "url"); return -1; }
        deadline_reset();
        vlog("radio: GET %s", url);
        if (conn_open(&s->c, u.host, u.port, u.tls)) { snprintf(err, ecap, "net"); return -1; }
        char req[3072], hostHdr[300];
        if (u.port == (u.tls ? 443 : 80)) snprintf(hostHdr, sizeof hostHdr, "%s", u.host); else snprintf(hostHdr, sizeof hostHdr, "%s:%d", u.host, u.port);
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " RD_UA "\r\nAccept: */*\r\n"
                          "Icy-MetaData: 1\r\nConnection: close\r\n\r\n", u.path, hostHdr);
        char line[2048]; int status = 0;
        if (rl >= (int)sizeof req || conn_write(&s->c, req, rl) || conn_line(&s->c, line, sizeof line) < 0) { conn_close(&s->c); snprintf(err, ecap, "net"); return -1; }
        if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1 && sscanf(line, "ICY %d", &status) != 1) { conn_close(&s->c); snprintf(err, ecap, "nostream"); return -1; }
        char loc[2048] = "";
        for (;;) {
            int n = conn_line(&s->c, line, sizeof line);
            if (n < 0) { conn_close(&s->c); snprintf(err, ecap, "net"); return -1; }
            if (n == 0) break;
            char *v = strchr(line, ':'); if (!v) continue;
            *v++ = 0; while (*v == ' ' || *v == '\t') v++;
            if (!strcasecmp(line, "location")) snprintf(loc, sizeof loc, "%s", v);
            else if (!strcasecmp(line, "content-type")) snprintf(s->ctype, sizeof s->ctype, "%s", v);
            else if (!strcasecmp(line, "icy-metaint")) s->metaint = atoi(v);
            else if (!strcasecmp(line, "icy-br")) s->br = atoi(v);
            else if (!strcasecmp(line, "transfer-encoding") && strcasestr(v, "chunked")) s->chunked = 1;
        }
        vlog("radio: -> %d %s metaint=%d br=%d", status, s->ctype, s->metaint, s->br);
        if (status >= 300 && status < 400 && loc[0]) {
            char nu[4096]; url_join(url, loc, nu, sizeof nu);
            conn_close(&s->c);
            if (strlen(nu) >= sizeof url) { snprintf(err, ecap, "url"); return -1; }
            snprintf(url, sizeof url, "%s", nu);
            continue;
        }
        if (status < 200 || status >= 300) { conn_close(&s->c); snprintf(err, ecap, "http %d", status); return -1; }
        if (s->metaint < 0 || s->metaint > (1 << 20)) s->metaint = 0;
        rs_icy_init(&s->icy, s->metaint);
        snprintf(s->url, sizeof s->url, "%s", url);
        s->open = 1;
        return 0;
    }
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
/* audio (sin trozos HTTP ni metadatos) a b: > 0 bytes, 0 = aun nada, -1 = fin o error */
static int rd_read(rd_src *s, unsigned char *b, int n, int *newtitle)
{
    if (newtitle) *newtitle = 0;
    deadline_reset();
    int r = conn_read(&s->c, b, n);
    if (r <= 0) return -1;
    r = rd_dechunk(s, b, r);
    if (!r) return s->ck.end ? -1 : 0;
    return rs_icy_feed(&s->icy, b, r, b, newtitle);
}

static unsigned char rd_buf[RD_SNIFF + 8192];

/* Abre url hasta dar con audio: sigue listas (cada entrada, en orden) y el truco "/;" de
 * Shoutcast 1 cuando la raiz devuelve su pagina HTML. 0 = s abierto y rd_buf[0..*pren) con
 * los primeros bytes de audio. */
static int rd_connect(rd_src *s, const char *url, int depth, int *pren, char *err, int ecap)
{
    if (depth > 4) { snprintf(err, ecap, "nostream"); return -1; }
    if (rd_http(s, url, err, ecap)) return -1;
    int kind = rs_ctype_kind(s->ctype), n = 0;
    if (kind == RS_UNKNOWN) kind = rs_url_kind(s->url);
    while (n < 512) { int r = rd_read(s, rd_buf + n, 1024, 0); if (r < 0) break; n += r; }
    int sk = rs_sniff_kind(rd_buf, n);
    if (sk != RS_UNKNOWN) kind = sk;                           /* los bytes mandan sobre la cabecera */
    else if (kind == RS_PLAYLIST || kind == RS_HTML || kind == RS_HLS) { struct rs_frame f; if (n > 64 && rs_sync(rd_buf, n, RS_UNKNOWN, &f) >= 0) kind = f.kind; }
    if (kind == RS_PLAYLIST || kind == RS_HLS) {
        while (n < RD_SNIFF) { int r = rd_read(s, rd_buf + n, 4096, 0); if (r < 0) break; n += r; }
        rd_close(s);
        char urls[4][RS_URL];
        int k = rs_playlist_parse((const char *)rd_buf, n, urls, 4);
        if (k == -2 || (k <= 0 && kind == RS_HLS)) { snprintf(err, ecap, "hls"); return -1; }
        if (k <= 0) { snprintf(err, ecap, "nostream"); return -1; }
        for (int i = 0; i < k; i++) {
            char abs[2100]; url_join(url, urls[i], abs, sizeof abs);
            if (!rd_connect(s, abs, depth + 1, pren, err, ecap)) return 0;
            if (!strcmp(err, "hls") || !strcmp(err, "codec")) break;
        }
        return -1;
    }
    if (kind == RS_HTML) {
        url_t u; char alt[2100];
        rd_close(s);
        if (!url_parse(url, &u) && !strcmp(u.path, "/") && snprintf(alt, sizeof alt, "%s://%s:%d/;", u.tls ? "https" : "http", u.host, u.port) < (int)sizeof alt)
            return rd_connect(s, alt, depth + 1, pren, err, ecap);
        snprintf(err, ecap, "nostream"); return -1;
    }
    if (kind == RS_OGG || kind == RS_FLAC) { rd_close(s); snprintf(err, ecap, "codec"); return -1; }
    s->kind = kind;
    *pren = n;
    return 0;
}
/* lee hasta encontrar sincronismo en rd_buf (ya hay *n bytes). Devuelve el desplazamiento
 * de la primera trama; -1 = no es MP3 ni AAC; -2 = se corto. */
static int rd_sync(rd_src *s, int *n, struct rs_frame *f)
{
    for (;;) {
        int off = rs_sync(rd_buf, *n, s->kind == RS_MP3 || s->kind == RS_AAC ? s->kind : RS_UNKNOWN, f);
        if (off < 0 && *n >= RD_SNIFF / 2 && s->kind != RS_UNKNOWN) off = rs_sync(rd_buf, *n, RS_UNKNOWN, f);   /* Content-Type equivocado */
        if (off >= 0) return off;
        if (*n >= RD_SNIFF) return -1;
        int r = rd_read(s, rd_buf + *n, 4096, 0);
        if (r < 0) return -2;
        *n += r;
    }
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
static int rd_newfile(long long presize)
{
    if (rd.gen > 1) snprintf(rd.file, sizeof rd.file, "%s/%s (%d).%s", rd.dir, rd.name, rd.gen, !strcmp(rd.codec, "AAC") ? "aac" : "mp3");
    else snprintf(rd.file, sizeof rd.file, "%s/%s.%s", rd.dir, rd.name, !strcmp(rd.codec, "AAC") ? "aac" : "mp3");
    unlink(rd.file);
    rd.fd = open(rd.file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (rd.fd < 0) { rd.file[0] = 0; return -1; }
    if (presize > 0 && ftruncate(rd.fd, presize)) { logf_("radio: ftruncate %lld: %s", presize, strerror(errno)); return -1; }
    char rp[4096];                                              /* /proc/<pid>/fd da la ruta canonica */
    snprintf(rd.real, sizeof rd.real, "%s", realpath(rd.file, rp) ? rp : rd.file);
    return 0;
}

static int cmd_radio(int argc, char **argv)
{
    if (argc < 3 || !rs_url_ok(argv[0])) return E_USAGE;
    const char *url = argv[0];
    long long presize = (long long)(argc > 3 ? atoi(argv[3]) : 256) << 20;
    int prebuf_arg = argc > 4 ? atoi(argv[4]) * 1024 : 0;
    if (presize < 0 || presize > (2000LL << 20)) presize = 256LL << 20;
    snprintf(rd.dir, sizeof rd.dir, "%s", argv[1]);
    rs_safe_name(argv[2], rd.name, sizeof rd.name);
    snprintf(rd.state, sizeof rd.state, "%s/state", rd.dir);
    snprintf(rd.tmp, sizeof rd.tmp, "%s/state.tmp", rd.dir);
    snprintf(rd.pidf, sizeof rd.pidf, "%s/pid", rd.dir);
    rd.player = getenv("HIBYRADIO_PLAYER") ? getenv("HIBYRADIO_PLAYER") : "hiby_player";
    rd.noreader = getenv("HIBYRADIO_NOREADER") != NULL;
    if (getenv("HIBYRADIO_HIGH_KB") && atoi(getenv("HIBYRADIO_HIGH_KB")) >= 64) rd_high = atoll(getenv("HIBYRADIO_HIGH_KB")) * 1024;
    rd.ppid = getppid();
    mkdir(rd.dir, 0755);
    { char rp[4096]; snprintf(rd.realdir, sizeof rd.realdir, "%s", realpath(rd.dir, rp) ? rp : rd.dir); }
    { FILE *pf = fopen(rd.pidf, "w"); if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); } }
    signal(SIGTERM, rd_sig); signal(SIGINT, rd_sig); signal(SIGHUP, rd_sig);
    g_timeout = 12; g_idle = 0;
    rd.gen = 0; rd.codec = "";
    rd_state("connecting", 0);

    static rd_src s;
    char err[48] = "net"; int n = 0, off = -1; struct rs_frame fr;
    for (int attempt = 0; attempt < 2 && off < 0; attempt++) {          /* un reintento: muchos servidores fallan a la primera */
        if (attempt) usleep(700 * 1000);
        if (rd_connect(&s, url, 0, &n, err, sizeof err)) { if (strcmp(err, "net")) break; continue; }
        off = rd_sync(&s, &n, &fr);
        if (off == -1) { rd_close(&s); snprintf(err, sizeof err, "codec"); break; }
        if (off < 0) { rd_close(&s); snprintf(err, sizeof err, "net"); }
    }
    if (off < 0) rd_end("error", err, !strcmp(err, "net") ? E_NET : E_NONE);
    rd.codec = fr.kind == RS_AAC ? "AAC" : "MP3";
    rd.br = fr.kind == RS_MP3 && fr.kbps ? fr.kbps : s.br;
    if (s.icy.title[0]) snprintf(rd.title, sizeof rd.title, "%s", s.icy.title);
    int bps = (rd.br > 0 ? rd.br : 96) * 125;                         /* bytes por segundo */
    long long prebuf = prebuf_arg > 0 ? prebuf_arg : bps * 8;                  /* ~8 s: el motor lee por delante al abrir */
    if (!prebuf_arg) { if (prebuf < 96 * 1024) prebuf = 96 * 1024; if (prebuf > 256 * 1024) prebuf = 256 * 1024; }
    if (prebuf > RD_HIGH * 3 / 4) prebuf = RD_HIGH * 3 / 4;                  /* si no, nunca se anuncia: la red para en RD_HIGH */

    long long wpos = 0, punched = 0, rpos = 0;
    long long need = prebuf;                                          /* bytes para anunciar esta generacion */
    const char *fill = "buffering";                                   /* "roll" en un relevo: el fichero viejo sigue sonando */
    int ready = 0, seen = 0, seen_any = 0, beyond = 0, nopunch = 0, retries = 0, resync = 0, under = 0;
    double t_ready = 0, t_gone = 0, t_other = 0, t_mon = 0, t_data = now_s(), t_retry = 0, t_pct = 0, t_old = 0;
    rd.gen = 1;
    if (rd_newfile(presize)) rd_end("error", "io", E_IO);
    rd.pct = 0; rd_state("buffering", 0);
    /* lo que ya se leyo, desde la primera trama */
    if (n > off) { if (pwrite(rd.fd, rd_buf + off, n - off, 0) != n - off) rd_end("error", "io", E_IO); wpos = n - off; }
    n = 0;
    g_timeout = 15;

    for (;;) {
        double now = now_s();
        if (getppid() != rd.ppid) rd_end("stopped", 0, E_OK);          /* el shell murio: nadie nos controla */
        long long ahead = wpos - (seen ? rpos : 0);
        if (rd.noreader && ready) ahead = 0;
        /* ---- red */
        if (!s.open) {
            if (now >= t_retry && ahead < RD_LOW) {
                char e2[48]; int k = 0;
                if (!rd_connect(&s, url, 0, &k, e2, sizeof e2)) { n = k; resync = 1; t_data = now_s(); }
                else if (++retries >= 6 || (strcmp(e2, "net") && retries >= 2)) rd_end("error", strcmp(e2, "net") ? e2 : "net", E_NET);
                else t_retry = now_s() + (retries < 3 ? 1.0 : 3.0);
            } else usleep(150 * 1000);
        } else if (ahead >= RD_HIGH) {
            usleep(200 * 1000);                                        /* el reproductor esta en pausa: que espere la red */
            t_data = now;
        } else if (resync) {                                           /* tras reconectar: seguir en una trama entera */
            struct rs_frame f2; int o2 = rd_sync(&s, &n, &f2);
            if (o2 == -2) { rd_close(&s); t_retry = now_s() + 1; if (++retries >= 6) rd_end("error", "net", E_NET); }
            else if (o2 < 0 || f2.kind != fr.kind) rd_end("error", "codec", E_NONE);
            else {
                if (pwrite(rd.fd, rd_buf + o2, n - o2, wpos) != n - o2) rd_end("error", "io", E_IO);
                wpos += n - o2; resync = 0; n = 0; retries = 0; t_data = now_s();
            }
        } else if (rd_wait(&s, 200)) {
            int nt = 0, r = rd_read(&s, rd_buf, 8192, &nt);
            if (r < 0) { vlog("radio: conexion cortada"); rd_close(&s); t_retry = now_s() + 0.5; }
            else if (r > 0) {
                if (pwrite(rd.fd, rd_buf, r, wpos) != r) rd_end("error", "io", E_IO);
                wpos += r; t_data = now; retries = 0;
            }
            if (nt) { snprintf(rd.title, sizeof rd.title, "%s", s.icy.title); rd_state(rd.st, 0); }
        } else if (now - t_data > 15) { vlog("radio: sin datos 15 s"); rd_close(&s); t_retry = now_s(); }

        /* ---- anuncio */
        now = now_s();
        if (!ready) {
            if (wpos >= need) { ready = 1; rd.pct = 100; t_ready = now; t_mon = 0; rd_state("ready", 0); vlog("radio: gen %d lista (%lld bytes)", rd.gen, wpos); }
            else if (now - t_pct > 0.4) { t_pct = now; int p = (int)(wpos * 100 / need); if (p != rd.pct) { rd.pct = p; rd_state(fill, 0); } }
        }
        if (rd.old[0] && t_old && now > t_old) rd_drop_old();
        /* ---- relevo de fichero: se acaba el tamano logico, o crece en RAM sin poder perforar */
        if (ready && ((presize > 0 && wpos > presize - 512 * 1024) || (presize > 0 && nopunch && wpos > RD_NOPUNCH_MAX))) goto newgen;

        /* ---- lector (tambien mientras se llena una generacion nueva, si ya sonamos antes) */
        if (rd.noreader || (!ready && !seen_any) || now - t_mon < 0.3) continue;
        t_mon = now;
        long long rp = 0; int other = 0, k = rd_reader(rd.real, wpos, &rp, &other);
        /* el reproductor abrio otra musica y solto la nuestra: se acabo (sin esto, tras un corte
         * el shell mandaria la generacion nueva al reproductor encima de lo que eligio el usuario) */
        if (seen_any && other) { if (!t_other) t_other = now; else if (now - t_other > 2) rd_end("stopped", 0, E_OK); }
        else t_other = 0;
        if (!ready) continue;
        if (k == 0) {
            if (!seen) { if (now - t_ready > 20) rd_end("error", "player", E_NONE); }
            else if (!t_gone) t_gone = now;
            else if (now - t_gone > 3) rd_end("stopped", 0, E_OK);     /* cerro el fichero: hay otra musica */
            continue;
        }
        t_gone = 0;
        if (!seen) { seen = seen_any = 1; rd_drop_old(); rd.opened = 1; rd_state(rd.st, 0); vlog("radio: el reproductor abrio el fichero (pos %lld)", rp); }
        if (k == 2 || rp < punched) {                                  /* leyo mas alla de lo escrito (o un hueco) */
            if (presize > 0 && ++beyond >= 3) { under = 1; goto newgen; }
            continue;
        }
        beyond = 0; rpos = rp;
        if (presize > 0 && !nopunch) {
            long long to = (rpos - RD_KEEP) & ~4095LL;
            if (to > punched + 65536) {
                if (fallocate(rd.fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, punched, to - punched)) { nopunch = 1; vlog("radio: sin PUNCH_HOLE (%s)", strerror(errno)); }
                else punched = to;
            }
        }
        continue;

    newgen:
        vlog("radio: %s (w=%lld r=%lld)", under ? "corte: el lector alcanzo a la escritura" : "relevo de fichero", wpos, rpos);
        rd_drop_old();
        snprintf(rd.old, sizeof rd.old, "%s", rd.file); rd.oldfd = rd.fd; rd.fd = -1;
        if (under) { if (ftruncate(rd.oldfd, 0)) {} if (!prebuf_arg && prebuf < 512 * 1024 && prebuf * 2 <= RD_HIGH * 3 / 4) prebuf *= 2; }   /* que no recorra 256 MB de ceros */
        rd.gen++;
        if (rd_newfile(presize)) rd_end("error", "io", E_IO);
        wpos = punched = rpos = 0; ready = seen = beyond = 0; t_gone = 0; t_old = now_s() + 30;
        if (s.open) { resync = 1; n = 0; }                             /* el fichero nuevo empieza en una trama */
        need = under ? prebuf : 48 * 1024;                             /* relevo: anunciar pronto */
        fill = under ? "buffering" : "roll";
        under = 0;
        rd.pct = 0; rd.opened = 0; rd_state(fill, 0);
    }
    return E_OK;
}
