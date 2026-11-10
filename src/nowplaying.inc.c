/* nowplaying.inc.c - "lo que suena" en el reproductor de HiBy, para el modulo Now Playing
 * del Centro de control, los widgets y la app Musica. Ver docs/research_playback.md.
 *
 *   struct np_info i;  if (np_get(&i) > 0) ...   (barato: llamar cada 250-1000 ms)
 *   uint16_t cov[128*128];  np_cover(i.path, cov) (0 = hay caratula; ver abajo el coste)
 *
 * De donde sale (por orden):
 *   1. NP_SRC_SHIM: hibymod_disp.so v5 dentro del reproductor anota los ficheros de audio que
 *      abre/cierra y las tramas que escribe al PCM (hm_state.np_*). Pista, reproduciendo/pausa
 *      y posicion exactos (posicion = tramas escritas / frecuencia, +- el bufer del PCM).
 *   2. NP_SRC_PROC: sin libreria v5 (la v4 que hay instalada hasta reiniciar), se mira
 *      /proc/<pid>/fd del reproductor: el fichero de audio abierto, y si su posicion de lectura
 *      (fdinfo) avanza, esta sonando. Posicion aproximada por bytes.
 *   3. NP_SRC_LAST: nada abierto (recien encendido, sin haber pulsado play): la ultima pista
 *      que guarda el reproductor en /usr/data/user.ini; playing = 0.
 * Titulo, artista y album: con hibytag (ht_read) si se ha incluido antes (en el shell lo
 * incluye tageditor.inc.c); si no, el nombre del fichero. Duracion: cabecera del fichero
 * (FLAC, MP3 Xing/VBRI/CBR, M4A, WAV, DSF). Todo se cachea por pista: solo se lee el fichero
 * al cambiar de cancion.
 *
 * Requisitos: incluir despues de hm_shared.h. Todos los nombres empiezan por np_ / NP_. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "hm_shared.h"

enum { NP_SRC_NONE, NP_SRC_SHIM, NP_SRC_PROC, NP_SRC_LAST };

struct np_info {
    int source;                  /* NP_SRC_* (NONE = no se sabe nada) */
    int playing;                 /* 1 sonando; 0 en pausa o parado */
    int stopped;                 /* 1 = ningun fichero abierto (solo la ultima pista conocida) */
    char path[400];              /* ruta del sistema: /usr/data/mnt/sd_0/Artista/Album/x.flac */
    char title[160], artist[128], album[128];
    int duration_ms;             /* 0 = desconocida */
    int position_ms;             /* -1 = desconocida */
    int rate, bits, channels;    /* del PCM (0 = desconocido; bits 0 por Bluetooth) */
    uint32_t track_id;           /* cambia cuando cambia la pista (para animar / pedir caratula) */
};

#define NP_COVER 128
#ifndef NP_CACHE
#define NP_CACHE "/usr/data/hibymod/cache/np"
#endif
#ifndef NP_WALLBIN
#define NP_WALLBIN "/usr/data/hibymod/shell/hibywall"
#endif

static uint32_t np_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000);
}
static uint32_t np_hash(const char *s, uint32_t h)
{
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ pid del reproductor */
static int np_pid;
static int np_find_pid(void)
{
    if (np_pid > 0) {
        char p[32]; snprintf(p, sizeof p, "/proc/%d/fd", np_pid);
        if (!access(p, F_OK)) return np_pid;
        np_pid = 0;
    }
    DIR *d = opendir("/proc"); struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        /* cambia su nombre de proceso: se busca por la linea de ordenes */
        char p[64], c[128] = ""; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
        int f = open(p, O_RDONLY); if (f < 0) continue;
        ssize_t n = read(f, c, sizeof c - 1); close(f);
        if (n <= 0) continue;
        c[n] = 0;
        const char *b = strrchr(c, '/'); b = b ? b + 1 : c;
        if (!strcmp(b, "hiby_player")) { np_pid = atoi(e->d_name); break; }
    }
    if (d) closedir(d);
    return np_pid;
}

/* /data/mnt/... -> /usr/data/mnt/... (las dos valen; se da siempre la segunda) */
static void np_norm(const char *in, char *out, size_t n)
{
    if (!strncmp(in, "/data/", 6)) snprintf(out, n, "/usr%s", in);
    else snprintf(out, n, "%s", in);
}

static int np_is_audio(const char *p)
{
    const char *d = strrchr(p, '.');
    if (!d || strchr(d, '/')) return 0;
    static const char *ext[] = { ".flac", ".mp3", ".m4a", ".aac", ".alac", ".wav", ".aif", ".aiff", ".ape",
        ".wv", ".ogg", ".oga", ".opus", ".dsf", ".dff", ".wma", ".mp2", ".iso", ".mqa", ".tak", ".tta", 0 };
    for (int i = 0; ext[i]; i++) if (!strcasecmp(d, ext[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ duracion y datos de audio */
struct np_meta {
    char key[400];
    char title[160], artist[128], album[128];
    int duration_ms;
    uint64_t data_start, data_end;   /* zona de audio, para pasar de bytes a tiempo */
};

static int np_pread(int fd, void *b, size_t n, uint64_t off)
{
    return pread(fd, b, n, (off_t)off) == (ssize_t)n;
}
static uint32_t np_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t np_le32(const unsigned char *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint64_t np_le64(const unsigned char *p) { return (uint64_t)np_le32(p + 4) << 32 | np_le32(p); }
static uint64_t np_be64(const unsigned char *p) { return (uint64_t)np_be32(p) << 32 | np_be32(p + 4); }

static uint64_t np_id3_skip(int fd)
{
    unsigned char h[10];
    if (!np_pread(fd, h, 10, 0) || memcmp(h, "ID3", 3)) return 0;
    uint32_t sz = (h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127);
    return 10 + (uint64_t)sz + ((h[5] & 0x10) ? 10 : 0);
}

static void np_dur_flac(int fd, uint64_t base, uint64_t size, struct np_meta *m)
{
    uint64_t o = base + 4; unsigned char h[4], si[18];
    for (int i = 0; i < 128; i++) {
        if (!np_pread(fd, h, 4, o)) return;
        uint32_t len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 127) == 0 && len >= 18 && np_pread(fd, si, 18, o + 4)) {
            uint32_t sr = (uint32_t)si[10] << 12 | si[11] << 4 | si[12] >> 4;
            uint64_t tot = (uint64_t)(si[13] & 15) << 32 | np_be32(si + 14);
            if (sr) m->duration_ms = (int)(tot * 1000 / sr);
        }
        o += 4 + len;
        if (h[0] & 128) break;
    }
    m->data_start = o; m->data_end = size;
}

static void np_dur_mp3(int fd, uint64_t size, struct np_meta *m)
{
    static const int br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int srt[3] = { 44100, 48000, 32000 };
    uint64_t o = np_id3_skip(fd);
    unsigned char b[4096];
    for (int tries = 0; tries < 16; tries++, o += sizeof b - 4) {
        if (pread(fd, b, sizeof b, (off_t)o) < 64) return;
        for (int i = 0; i + 4 < (int)sizeof b - 200; i++) {
            if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
            int ver = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, bri = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3;
            if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue;   /* solo layer III */
            int mpeg1 = ver == 3, sr = srt[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
            int kbps = mpeg1 ? br1[bri] : br2[bri], spf = mpeg1 ? 1152 : 576;
            int mono = (b[i + 3] >> 6) == 3;
            int side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
            m->data_start = o + i; m->data_end = size;
            const unsigned char *x = b + i + 4 + side, *v = b + i + 4 + 32;
            if (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) {
                uint32_t fl = np_be32(x + 4);
                if (fl & 1) { m->duration_ms = (int)((uint64_t)np_be32(x + 8) * spf * 1000 / sr); return; }
            } else if (!memcmp(v, "VBRI", 4)) {
                m->duration_ms = (int)((uint64_t)np_be32(v + 14) * spf * 1000 / sr); return;
            }
            uint64_t end = size;                    /* CBR; sin la etiqueta ID3v1 del final */
            unsigned char t[3];
            if (size > 128 && np_pread(fd, t, 3, size - 128) && !memcmp(t, "TAG", 3)) end -= 128;
            if (kbps) m->duration_ms = (int)((end - m->data_start) * 8 / kbps);
            return;
        }
    }
}

/* busca la caja fcc dentro de [a, b); devuelve su desplazamiento y tamano */
static int np_box(int fd, uint64_t a, uint64_t b, const char *fcc, uint64_t *off, uint64_t *sz)
{
    unsigned char h[16];
    for (int i = 0; a + 8 <= b && i < 4096; i++) {
        if (!np_pread(fd, h, 8, a)) return 0;
        uint64_t s = np_be32(h); int hl = 8;
        if (s == 1) { if (!np_pread(fd, h + 8, 8, a + 8)) return 0; s = np_be64(h + 8); hl = 16; }
        else if (s == 0) s = b - a;
        if (s < (uint64_t)hl || a + s > b) return 0;
        if (!memcmp(h + 4, fcc, 4)) { *off = a; *sz = s; return hl; }
        a += s;
    }
    return 0;
}

static void np_dur_m4a(int fd, uint64_t size, struct np_meta *m)
{
    uint64_t mo, ms, ho, hs, dO, dS;
    int hl = np_box(fd, 0, size, "moov", &mo, &ms);
    if (!hl) return;
    int hl2 = np_box(fd, mo + hl, mo + ms, "mvhd", &ho, &hs);
    unsigned char b[32];
    if (hl2 && np_pread(fd, b, 32, ho + hl2)) {
        uint32_t ts; uint64_t du;
        if (b[0] == 1) { ts = np_be32(b + 20); du = np_be64(b + 24); }
        else { ts = np_be32(b + 12); du = np_be32(b + 16); }
        if (ts) m->duration_ms = (int)(du * 1000 / ts);
    }
    int hl3 = np_box(fd, 0, size, "mdat", &dO, &dS);
    if (hl3) { m->data_start = dO + hl3; m->data_end = dO + dS; }
}

static void np_dur_wav(int fd, uint64_t size, struct np_meta *m)
{
    unsigned char h[8], f[16]; uint64_t o = 12; uint32_t byterate = 0;
    for (int i = 0; i < 64 && o + 8 <= size; i++) {
        if (!np_pread(fd, h, 8, o)) return;
        uint32_t len = np_le32(h + 4);
        if (!memcmp(h, "fmt ", 4) && np_pread(fd, f, 16, o + 8)) byterate = np_le32(f + 8);
        if (!memcmp(h, "data", 4)) {
            uint64_t dl = len; if (o + 8 + dl > size) dl = size - o - 8;
            m->data_start = o + 8; m->data_end = o + 8 + dl;
            if (byterate) m->duration_ms = (int)(dl * 1000 / byterate);
            return;
        }
        o += 8 + len + (len & 1);
    }
}

static void np_dur_dsf(int fd, uint64_t size, struct np_meta *m)
{
    unsigned char b[52];
    if (!np_pread(fd, b, 52, 28) || memcmp(b, "fmt ", 4)) return;
    uint32_t sr = np_le32(b + 28); uint64_t cnt = np_le64(b + 36);
    if (sr) m->duration_ms = (int)(cnt * 1000 / sr);
    m->data_start = 28 + np_le64(b + 4) + 12; m->data_end = size;
}

static void np_read_meta(const char *path, struct np_meta *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->key, sizeof m->key, "%s", path);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat s; unsigned char h[12] = { 0 };
        if (!fstat(fd, &s)) {
            uint64_t size = (uint64_t)s.st_size, b = np_id3_skip(fd);
            if (pread(fd, h, 12, (off_t)b) < 4) memset(h, 0, sizeof h);
            if (!memcmp(h, "fLaC", 4)) np_dur_flac(fd, b, size, m);
            else if (!memcmp(h + 4, "ftyp", 4)) np_dur_m4a(fd, size, m);
            else if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) np_dur_wav(fd, size, m);
            else if (!memcmp(h, "DSD ", 4)) np_dur_dsf(fd, size, m);
            else np_dur_mp3(fd, size, m);
            if (!m->data_end) { m->data_start = b; m->data_end = size; }
        }
        close(fd);
    }
#ifdef HIBYTAG_H
    ht_tags t;
    if (ht_read(path, &t) == HT_OK) {
        if (t.v[HT_TITLE]) snprintf(m->title, sizeof m->title, "%s", t.v[HT_TITLE]);
        if (t.v[HT_ARTIST]) snprintf(m->artist, sizeof m->artist, "%s", t.v[HT_ARTIST]);
        else if (t.v[HT_ALBUMARTIST]) snprintf(m->artist, sizeof m->artist, "%s", t.v[HT_ALBUMARTIST]);
        if (t.v[HT_ALBUM]) snprintf(m->album, sizeof m->album, "%s", t.v[HT_ALBUM]);
    }
    ht_tags_free(&t);
#endif
    if (!m->title[0]) {                             /* sin etiqueta: el nombre del fichero */
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        snprintf(m->title, sizeof m->title, "%s", b);
        char *d = strrchr(m->title, '.'); if (d) *d = 0;
    }
    if (!m->album[0] || !m->artist[0]) {          /* .../Artista/Album/pista */
        char tmp[400]; snprintf(tmp, sizeof tmp, "%s", path);
        char *s1 = strrchr(tmp, '/'); if (s1) *s1 = 0;
        char *s2 = s1 ? strrchr(tmp, '/') : 0;
        if (s2 && !m->album[0]) snprintf(m->album, sizeof m->album, "%s", s2 + 1);
        if (s2) { *s2 = 0; char *s3 = strrchr(tmp, '/');
            if (s3 && !m->artist[0] && strcmp(s3 + 1, "sd_0")) snprintf(m->artist, sizeof m->artist, "%s", s3 + 1); }
    }
}

static struct np_meta np_mc;                        /* cache: la pista actual */
static const struct np_meta *np_meta_for(const char *path)
{
    if (strcmp(np_mc.key, path)) np_read_meta(path, &np_mc);
    return &np_mc;
}
static int np_bytes_to_ms(const struct np_meta *m, uint64_t off)
{
    if (!m->duration_ms || m->data_end <= m->data_start) return -1;
    if (off <= m->data_start) return 0;
    if (off >= m->data_end) return m->duration_ms;
    return (int)((double)(off - m->data_start) / (double)(m->data_end - m->data_start) * m->duration_ms);
}

/* ------------------------------------------------------------------ fuente 1: la libreria */
static struct hm_state *np_st;
static struct hm_state *np_map(void)
{
    if (np_st) return np_st->np_magic == HM_NP_MAGIC ? np_st : 0;
    int f = open(HM_STATE, O_RDONLY);
    if (f < 0) return 0;
    struct stat s;
    if (fstat(f, &s) || s.st_size < (off_t)sizeof(struct hm_state)) { close(f); return 0; }
    void *m = mmap(0, sizeof(struct hm_state), PROT_READ, MAP_SHARED, f, 0);
    close(f);
    if (m == MAP_FAILED) return 0;
    np_st = m;
    return np_st->np_magic == HM_NP_MAGIC ? np_st : 0;
}

/* el fd sigue abierto en el reproductor y es ese fichero (por si no vimos un cierre) */
static int np_fd_alive(int pid, int fd, const char *path)
{
    if (pid <= 0) return 1;
    char p[48], l[420]; snprintf(p, sizeof p, "/proc/%d/fd/%d", pid, fd);
    ssize_t n = readlink(p, l, sizeof l - 1);
    if (n <= 0) return 0;
    l[n] = 0;
    const char *a = strstr(l, "/mnt/"), *b = strstr(path, "/mnt/");
    return a && b && !strcmp(a, b);
}

static struct { uint32_t gen, anchor_frames, seek_ms; int anchor_pos; } np_tr;

static int np_from_shim(struct np_info *o)
{
    struct hm_state *s = np_map();
    if (!s) return 0;
    struct hm_np_open sl[HM_NP_SLOTS];
    for (int t = 0; t < 20; t++) {
        uint32_t q = s->np_seq;
        if (q & 1) { usleep(200); continue; }
        __sync_synchronize();
        memcpy(sl, (const void *)s->np_open, sizeof sl);
        __sync_synchronize();
        if (s->np_seq == q) break;
        if (t == 19) return 0;
    }
    uint32_t now = np_ms();
    int pid = np_find_pid(), cur = -1, stopped = 0;
    /* la pista = el fichero abierto mas antiguo que lleve abierto > 600 ms (lo que lee el
     * decodificador; las lecturas de etiquetas abren y cierran en < 0,5 s). Con el gapless
     * la siguiente se abre antes de acabar esta: se pasa a ella cuando esta se cierra. */
    for (int i = 0; i < HM_NP_SLOTS; i++) {
        if (!sl[i].gen || sl[i].fd < 0 || now - sl[i].open_ms < 600) continue;
        sl[i].path[sizeof sl[i].path - 1] = 0;
        if (!np_fd_alive(pid, sl[i].fd, sl[i].path)) continue;
        if (cur < 0 || sl[i].gen < sl[cur].gen) cur = i;
    }
    if (cur < 0) {                                  /* parado: la ultima que se leyo entera */
        for (int i = 0; i < HM_NP_SLOTS; i++)
            if (sl[i].gen && sl[i].fd < 0 && sl[i].close_ms - sl[i].open_ms >= 1500 && (cur < 0 || sl[i].gen > sl[cur].gen)) cur = i;
        stopped = 1;
    }
    if (cur < 0) return 0;
    struct hm_np_open *e = &sl[cur];
    e->path[sizeof e->path - 1] = 0;
    np_norm(e->path, o->path, sizeof o->path);
    const struct np_meta *m = np_meta_for(o->path);
    o->source = NP_SRC_SHIM; o->stopped = stopped;
    o->rate = (int)s->pcm_rate; o->bits = (int)s->pcm_bits; o->channels = (int)s->pcm_channels;
    o->playing = !stopped && s->pcm_open && !s->pcm_paused && now - s->pcm_last_ms < 1500;   /* a 176,4/192 kHz escribe bloques de > 0,5 s */
    o->track_id = np_hash(o->path, e->gen * 2654435761u);
    /* posicion: tramas escritas desde que se abrio la pista; si el usuario salto (lseek del
     * decodificador junto a un vaciado del PCM), desde ese punto (bytes -> tiempo) */
    if (np_tr.gen != e->gen) { np_tr.gen = e->gen; np_tr.anchor_frames = e->frames_at_open; np_tr.anchor_pos = 0; np_tr.seek_ms = 0; }
    if (e->nseeks && e->seek_ms != np_tr.seek_ms && e->seek_ms - e->open_ms > 1500) {
        int32_t d = (int32_t)(e->seek_ms - s->pcm_drop_ms);
        if (d > -1500 && d < 1500) {
            int p = np_bytes_to_ms(m, (uint64_t)e->seek_hi << 32 | e->seek_lo);
            if (p >= 0) { np_tr.anchor_pos = p; np_tr.anchor_frames = e->seek_frames; }
        }
        np_tr.seek_ms = e->seek_ms;
    }
    if (stopped) o->position_ms = 0;
    else if (s->pcm_rate) {
        uint32_t fr = s->pcm_frames - np_tr.anchor_frames;
        o->position_ms = np_tr.anchor_pos + (int)((uint64_t)fr * 1000 / s->pcm_rate);
    } else o->position_ms = -1;
    if (m->duration_ms && o->position_ms > m->duration_ms) o->position_ms = m->duration_ms;
    return 1;
}

/* ------------------------------------------------------------------ fuente 2: /proc */
static struct { char path[400]; long long pos; uint32_t moved_ms; } np_pr;

static int np_from_proc(struct np_info *o)
{
    int pid = np_find_pid();
    if (pid <= 0) return 0;
    char dir[32]; snprintf(dir, sizeof dir, "/proc/%d/fd", pid);
    DIR *d = opendir(dir); if (!d) { np_pid = 0; return 0; }
    struct dirent *e; char best[400] = ""; int bestfd = 1 << 30, pcm = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[64], l[400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        ssize_t n = readlink(p, l, sizeof l - 1);
        if (n <= 0) continue;
        l[n] = 0;
        if (!strncmp(l, "/dev/snd/pcm", 12)) pcm = 1;
        else if (strstr(l, "/mnt/") && np_is_audio(l) && atoi(e->d_name) < bestfd) {   /* el de menor fd: el que lleva mas */
            bestfd = atoi(e->d_name); snprintf(best, sizeof best, "%s", l);
        }
    }
    closedir(d);
    if (!best[0]) return 0;
    long long pos = -1;
    char p[64]; snprintf(p, sizeof p, "/proc/%d/fdinfo/%d", pid, bestfd);
    FILE *f = fopen(p, "r");
    if (f) { char l[64]; while (fgets(l, sizeof l, f)) if (!strncmp(l, "pos:", 4)) pos = atoll(l + 4); fclose(f); }
    uint32_t now = np_ms();
    np_norm(best, o->path, sizeof o->path);
    if (strcmp(np_pr.path, o->path)) { snprintf(np_pr.path, sizeof np_pr.path, "%s", o->path); np_pr.pos = pos; np_pr.moved_ms = now; }
    else if (pos != np_pr.pos) { np_pr.pos = pos; np_pr.moved_ms = now; }
    const struct np_meta *m = np_meta_for(o->path);
    o->source = NP_SRC_PROC;
    /* el decodificador lee a rafagas (~1 s de audio por lectura de 128-400 KB) */
    o->playing = pcm && now - np_pr.moved_ms < 2500;
    o->position_ms = pos >= 0 ? np_bytes_to_ms(m, (uint64_t)pos) : -1;
    if (o->position_ms > 0) o->position_ms -= o->position_ms > 2000 ? 2000 : o->position_ms;  /* va por delante (bufer) */
    o->track_id = np_hash(o->path, 1);
    return 1;
}

/* ------------------------------------------------------------------ fuente 3: user.ini */
static int np_from_userini(struct np_info *o)
{
    unsigned char b[0x2a + 1024];
    int f = open("/usr/data/user.ini", O_RDONLY);
    if (f < 0) return 0;
    ssize_t n = read(f, b, sizeof b); close(f);
    if (n < 0x40) return 0;
    /* UTF-16LE desde 0x28: "?:\Artista\Album\pista.flac" (la letra de unidad puede venir a 0) */
    char rel[400]; size_t k = 0;
    for (ssize_t i = 0x28; i + 1 < n && k + 4 < sizeof rel; i += 2) {
        unsigned c = b[i] | b[i + 1] << 8;
        if (!c) { if (i == 0x28) continue; break; }
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            unsigned c2 = b[i + 2] | b[i + 3] << 8; i += 2;
            c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
        }
        if (c == '\\') c = '/';
        if (c < 0x80) rel[k++] = (char)c;
        else if (c < 0x800) { rel[k++] = (char)(0xC0 | c >> 6); rel[k++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { rel[k++] = (char)(0xE0 | c >> 12); rel[k++] = (char)(0x80 | ((c >> 6) & 63)); rel[k++] = (char)(0x80 | (c & 63)); }
        else { rel[k++] = (char)(0xF0 | c >> 18); rel[k++] = (char)(0x80 | ((c >> 12) & 63)); rel[k++] = (char)(0x80 | ((c >> 6) & 63)); rel[k++] = (char)(0x80 | (c & 63)); }
    }
    rel[k] = 0;
    char *c = strchr(rel, ':');
    if (!c || c[1] != '/') return 0;
    snprintf(o->path, sizeof o->path, "/usr/data/mnt/sd_0%s", c + 1);
    if (access(o->path, R_OK)) { o->path[0] = 0; return 0; }
    o->source = NP_SRC_LAST; o->stopped = 1; o->playing = 0; o->position_ms = -1;
    o->track_id = np_hash(o->path, 3);
    np_meta_for(o->path);
    return 1;
}

/* ------------------------------------------------------------------ API */
/* Devuelve 1 si hay pista (o->source != NONE), 0 si no se sabe nada. */
static int np_get(struct np_info *o)
{
    memset(o, 0, sizeof *o);
    o->position_ms = -1;
    if (!np_from_shim(o) && !np_from_proc(o) && !np_from_userini(o)) { o->source = NP_SRC_NONE; return 0; }
    const struct np_meta *m = np_meta_for(o->path);
    snprintf(o->title, sizeof o->title, "%s", m->title);
    snprintf(o->artist, sizeof o->artist, "%s", m->artist);
    snprintf(o->album, sizeof o->album, "%s", m->album);
    o->duration_ms = m->duration_ms;
    return 1;
}

/* ------------------------------------------------------------------ caratula 128x128 RGB565 */
/* Busca la portada incrustada (FLAC PICTURE, ID3v2 APIC/PIC, M4A covr) y si no hay, la de la
 * carpeta (cover/folder/front .jpg/.png). La convierte con hibywall (libjpeg con reduccion al
 * decodificar: una portada de 3000x3000 no pasa de ~2 MB y en otro proceso, no en el shell)
 * a 128x128 y la guarda en NP_CACHE por (carpeta, tamano de la imagen): la siguiente pista del
 * mismo album sale de la cache (32 KB, < 5 ms). Sin cache: 150-500 ms -> llamar desde un hilo
 * o justo al cambiar track_id, nunca en el bucle de animacion.
 * Devuelve 0 si out tiene la caratula, -1 si no hay. */
struct np_pic { uint64_t off, len; int png; };

static int np_pic_flac(int fd, uint64_t base, struct np_pic *pic)
{
    uint64_t o = base + 4; unsigned char h[4], b[8]; int found = 0;
    for (int i = 0; i < 128; i++) {
        if (!np_pread(fd, h, 4, o)) break;
        uint32_t len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 127) == 6 && np_pread(fd, b, 8, o + 4)) {
            uint32_t type = np_be32(b), ml = np_be32(b + 4);
            uint64_t q = o + 12 + ml; unsigned char dl[4];
            if (ml < 256 && np_pread(fd, dl, 4, q)) {
                uint32_t desc = np_be32(dl); q += 4 + desc + 16;
                if (desc < 65536 && np_pread(fd, dl, 4, q)) {
                    uint32_t n = np_be32(dl);
                    if (n && n <= len && (!found || type == 3)) {
                        pic->off = q + 4; pic->len = n; found = 1;
                        if (type == 3) return 1;
                    }
                }
            }
        }
        o += 4 + len;
        if (h[0] & 128) break;
    }
    return found;
}

static int np_pic_id3(int fd, struct np_pic *pic)
{
    unsigned char h[10];
    if (!np_pread(fd, h, 10, 0) || memcmp(h, "ID3", 3)) return 0;
    int v = h[3];
    uint64_t end = 10 + ((h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127)), o = 10;
    if (h[5] & 0x40 && v >= 3) {                     /* cabecera extendida */
        unsigned char x[4]; if (!np_pread(fd, x, 4, 10)) return 0;
        o += v == 4 ? ((x[0] & 127) << 21 | (x[1] & 127) << 14 | (x[2] & 127) << 7 | (x[3] & 127)) : np_be32(x) + 4;
    }
    int found = 0;
    while (o + 10 < end) {
        unsigned char f[10]; uint32_t sz; int hl;
        if (v == 2) { if (!np_pread(fd, f, 6, o)) break; sz = (uint32_t)f[3] << 16 | f[4] << 8 | f[5]; hl = 6; }
        else {
            if (!np_pread(fd, f, 10, o)) break;
            sz = v == 4 ? (uint32_t)((f[4] & 127) << 21 | (f[5] & 127) << 14 | (f[6] & 127) << 7 | (f[7] & 127)) : np_be32(f + 4);
            hl = 10;
        }
        if (!f[0] || !sz || o + hl + sz > end) break;
        if ((v == 2 && !memcmp(f, "PIC", 3)) || (v >= 3 && !memcmp(f, "APIC", 4))) {
            unsigned char b[300]; uint32_t n = sz < sizeof b ? sz : sizeof b;
            if (np_pread(fd, b, n, o + hl)) {
                int enc = b[0]; uint32_t p = 1, type;
                if (v == 2) p = 4; else { while (p < n && b[p]) p++; p++; }
                type = p < n ? b[p] : 0; p++;
                if (enc == 1 || enc == 2) { while (p + 1 < n && (b[p] || b[p + 1])) p += 2; p += 2; }
                else { while (p < n && b[p]) p++; p++; }
                if (p < n && (!found || type == 3)) {
                    pic->off = o + hl + p; pic->len = sz - p; found = 1;
                    if (type == 3) return 1;
                }
            }
        }
        o += hl + sz;
    }
    return found;
}

static int np_pic_m4a(int fd, uint64_t size, struct np_pic *pic)
{
    uint64_t a, as, b, bs, c, cs, d, ds, e, es, f, fs;
    int h1 = np_box(fd, 0, size, "moov", &a, &as); if (!h1) return 0;
    int h2 = np_box(fd, a + h1, a + as, "udta", &b, &bs); if (!h2) return 0;
    int h3 = np_box(fd, b + h2, b + bs, "meta", &c, &cs); if (!h3) return 0;
    int h4 = np_box(fd, c + h3 + 4, c + cs, "ilst", &d, &ds); if (!h4) return 0;
    int h5 = np_box(fd, d + h4, d + ds, "covr", &e, &es); if (!h5) return 0;
    int h6 = np_box(fd, e + h5, e + es, "data", &f, &fs); if (!h6 || fs < (uint64_t)h6 + 8) return 0;
    pic->off = f + h6 + 8; pic->len = fs - h6 - 8;
    return 1;
}

static int np_run_wall(const char *in, const char *out)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (!p) {
        int n = open("/dev/null", O_WRONLY); if (n >= 0) { dup2(n, 1); dup2(n, 2); }
        /* tope de memoria: una JPEG progresiva grande pide > 8 MB y el OOM mataba al shell */
        execl("/bin/sh", "sh", "-c", "ulimit -v 7168; exec \"$0\" \"$@\"", NP_WALLBIN, in, out, "128", "128", (char *)0);
        _exit(127);
    }
    int stv = 0;
    while (waitpid(p, &stv, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(stv) && WEXITSTATUS(stv) == 0 ? 0 : -1;
}

static int np_read_raw(const char *p, uint16_t *out)
{
    int f = open(p, O_RDONLY); if (f < 0) return -1;
    ssize_t n = read(f, out, NP_COVER * NP_COVER * 2); close(f);
    return n == NP_COVER * NP_COVER * 2 ? 0 : -1;
}

static int np_cover(const char *path, uint16_t *out)
{
    if (!path || !path[0]) return -1;
    char dir[400]; snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (!sl) return -1;
    *sl = 0;
    if (access(NP_CACHE, W_OK)) {                   /* crea la carpeta de la cache y sus padres */
        char d[128]; snprintf(d, sizeof d, "%s", NP_CACHE);
        for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }
        mkdir(d, 0755);
    }
    struct np_pic pic = { 0, 0, 0 };
    int fd = open(path, O_RDONLY), have = 0;
    if (fd >= 0) {
        struct stat s; unsigned char h[12] = { 0 };
        if (!fstat(fd, &s)) {
            uint64_t b = np_id3_skip(fd);
            if (pread(fd, h, 12, (off_t)b) < 4) memset(h, 0, sizeof h);
            if (!memcmp(h, "fLaC", 4)) have = np_pic_flac(fd, b, &pic);
            else if (!memcmp(h + 4, "ftyp", 4)) have = np_pic_m4a(fd, (uint64_t)s.st_size, &pic);
            if (!have) have = np_pic_id3(fd, &pic);              /* MP3 (y FLAC/DSF con ID3) */
            if (have && (pic.len < 64 || pic.len > (16u << 20) || pic.off + pic.len > (uint64_t)s.st_size)) have = 0;
        }
    }
    char raw[480], img[480];
    if (have) {
        snprintf(raw, sizeof raw, NP_CACHE "/%08x.raw", np_hash(dir, (uint32_t)pic.len * 2654435761u));
        if (!np_read_raw(raw, out)) { close(fd); return 0; }
        snprintf(img, sizeof img, NP_CACHE "/tmp.img");
        int w = open(img, O_WRONLY | O_CREAT | O_TRUNC, 0644), ok = w >= 0;
        char buf[16384]; uint64_t left = pic.len, o = pic.off;
        while (ok && left) {
            size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
            if (pread(fd, buf, n, (off_t)o) != (ssize_t)n || write(w, buf, n) != (ssize_t)n) ok = 0;
            left -= n; o += n;
        }
        if (w >= 0) close(w);
        close(fd);
        int r = ok ? np_run_wall(img, raw) : -1;
        unlink(img);
        if (!r && !np_read_raw(raw, out)) return 0;
        unlink(raw);
    } else if (fd >= 0) close(fd);
    /* la imagen de la carpeta */
    static const char *names[] = { "cover", "folder", "front", "Cover", "Folder", "Front", "AlbumArt", 0 };
    static const char *exts[] = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG", 0 };
    for (int i = 0; names[i]; i++)
        for (int j = 0; exts[j]; j++) {
            char p[480]; snprintf(p, sizeof p, "%s/%s%s", dir, names[i], exts[j]);
            struct stat s;
            if (stat(p, &s)) continue;
            snprintf(raw, sizeof raw, NP_CACHE "/%08x.raw", np_hash(p, (uint32_t)s.st_mtime ^ (uint32_t)s.st_size));
            if (!np_read_raw(raw, out)) return 0;
            if (!np_run_wall(p, raw) && !np_read_raw(raw, out)) return 0;
            unlink(raw);
            return -1;
        }
    return -1;
}
