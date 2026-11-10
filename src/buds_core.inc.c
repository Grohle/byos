/* buds_core.inc.c - headphone management, common part (see docs/HEADPHONES.md).
 *
 * Shared, without changes, by:
 *   - src/hibybudsd.c        the daemon that talks to the headphones (vendor channels);
 *   - src/buds.inc.c         the shell app "Auriculares" (reads the state, sends commands);
 *   - tests/buds_test.c      PC tests.
 * No I/O here except formatting into a FILE* (state file). Prefixes buds_ / bd_ / bcmd_.
 *
 * Daemon <-> shell:
 *   BUDS_STATE  text file "key=value" lines, rewritten atomically (tmp + rename) by the daemon
 *               whenever something changes. Header, then one block per device ("dev=" opens it).
 *   BUDS_CMD    FIFO, one command per line "SEQ ADDR VERB ARGS" (bcmd_format / bcmd_parse).
 *               The daemon answers in the state header: ack=SEQ (sent to the headphones) or
 *               err=SEQ CODE (not sent: not connected, not supported, I/O error...).
 *   BUDS_EVT    FIFO, the daemon writes one byte per state change: wakes the shell's reader
 *               thread (no polling while nothing happens).
 *
 * Brand modules (Apple AAP, Samsung SPP, ... Nothing/Sony/JBL later) implement struct
 * buds_brand: pure protocol code that only sees bytes in / bytes out through struct bud. */
#ifndef BUDS_CORE_INC_C
#define BUDS_CORE_INC_C
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUDS_STATE "/tmp/hibybuds.state"
#define BUDS_CMD "/tmp/hibybuds.cmd"
#define BUDS_EVT "/tmp/hibybuds.evt"
#define BUDS_DATA "/usr/data/hibymod/buds"        /* known devices, preferences, log */
#define BUDS_MAXDEV 6

/* ------------------------------------------------------------------ vocabulary */
enum { BB_NONE, BB_APPLE, BB_SAMSUNG, BB_NOTHING, BB_SONY, BB_JBL, BB_SOUNDCORE, BB_HUAWEI, BB_OPPO, BB_GOOGLE, BB_OTHER = 15 };
/* product family: decides the drawing (buds.inc.c) */
enum { BF_NONE, BF_PODS, BF_PODS_PRO, BF_PODS_MAX, BF_BEATS_BUDS, BF_BEATS_EAR, BF_BEATS_HEAD,
       BF_GBUDS, BF_GBUDS_LIVE, BF_GBUDS_STEM, BF_BUDS, BF_HEAD, BF_NOTHING, BF_N };
/* noise control modes (AMBIENT = Apple "Transparency" / Samsung "Ambient sound") */
enum { BN_OFF, BN_ANC, BN_AMBIENT, BN_ADAPTIVE, BN_N };
/* connection: NONE = not connected (known device), AUDIO = connected, vendor channel not tried
 * yet, LINKING = opening it, READY = talking, NOCTL = connected but no control channel */
enum { BS_NONE, BS_AUDIO, BS_LINKING, BS_READY, BS_NOCTL };
enum { BW_OUT = 0, BW_IN = 1, BW_CASE = 2 };             /* wear state; -1 unknown */
enum { BG_TAP, BG_DOUBLE, BG_TRIPLE, BG_HOLD, BG_N };    /* gestures */
enum { BA_NONE, BA_PLAYPAUSE, BA_NEXT, BA_PREV, BA_VOICE, BA_NOISE, BA_AMBIENT, BA_ANC, BA_VOLUME, BA_SPOTIFY, BA_VOLUP, BA_VOLDN, BA_N };
/* EQ preset sets (struct buds_st.eq_set): the names are in buds_eq_names() */
enum { ES_SAMSUNG, ES_NOTHING, ES_CMF, ES_SONY, ES_N };

/* capabilities (struct buds_st.caps) */
#define BC_BATT_LR (1u << 0)      /* left / right battery */
#define BC_BATT_CASE (1u << 1)    /* case battery */
#define BC_BATT_ONE (1u << 2)     /* single battery (over-ear, neckband): batt[0] */
#define BC_WEAR (1u << 3)         /* in-ear / on-head detection (auto-pause) */
#define BC_NOISE (1u << 4)        /* noise control (modes in noise_mask) */
#define BC_ANC_LVL (1u << 5)      /* ANC level 0..anc_max */
#define BC_AMB_LVL (1u << 6)      /* ambient level 0..amb_max */
#define BC_ADAPT_LVL (1u << 7)    /* adaptive level 0..100 */
#define BC_CONV (1u << 8)         /* conversation awareness / voice detect */
#define BC_EQ (1u << 9)           /* EQ presets 0..eq_n-1 */
#define BC_EQ_BANDS (1u << 10)    /* custom bands (nbands) */
#define BC_LOCK (1u << 11)        /* touch lock */
#define BC_GEST (1u << 12)        /* per side gesture -> action (gest_opts) */
#define BC_CYCLE (1u << 13)       /* modes cycled by press and hold (Apple) */
#define BC_EARDET (1u << 14)      /* device-side ear detection setting */
#define BC_FIND (1u << 15)        /* play a sound per side */
#define BC_GAME (1u << 16)        /* low latency mode */
#define BC_FW (1u << 17)          /* firmware version reported */
#define BC_SPOOF (1u << 18)       /* has features that need an Apple host (shown as unavailable) */
#define BC_BASS (1u << 19)        /* bass boost level 0 (off)..bass_max (Nothing "Ultra Bass", Sony "Clear Bass") */
#define BC_FIT (1u << 20)         /* ear tip fit test */
#define BC_RO (1u << 21)          /* protocol not verified on hardware yet: settings shown read-only */

/* ------------------------------------------------------------------ state of one headphone */
struct buds_st {
    char addr[18], name[64], model[48], modelno[16], fw[40], serial[24];
    int brand, family, model_id, color;  /* model_id: brand-specific product id */
    int conn;                            /* BS_* */
    unsigned caps;
    int batt[3], chg[3];                 /* L R case (single: batt[0]); -1 unknown */
    int wear[2];                         /* BW_*; Apple: primary / secondary bud */
    int noise, noise_mask;               /* BN_*, -1 unknown; mask of (1 << BN_*) */
    int anc_lvl, anc_max, amb_lvl, amb_max, adapt_lvl;
    int conv;                            /* -1 n/a */
    int eq, eq_n;
    int nbands, bands[10];
    int lock, eardet, game, cycle;       /* cycle: mask of (1 << BN_*) */
    int gest[2][BG_N];                   /* [L/R][gesture] = BA_*, -1 unknown */
    unsigned gest_opts[BG_N];            /* configurable actions per gesture (mask of 1 << BA_*) */
    int find[2];                         /* sound playing L / R */
    int eq_set, band_min, band_max;      /* ES_*; custom band range (dB) */
    int bass, bass_max;                  /* -1 unknown */
    int fit[2];                          /* fit test: -1 unknown, 0 good seal, 1 poor */
    int busy;                            /* connect / disconnect in progress */
    int popseq;                          /* +1 per "connected" event worth a popup (daemon, debounced 30 s) */
    double seen;                         /* daemon: wall time (s) of the last data from the device */
};

static void buds_st_init(struct buds_st *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 3; i++) s->batt[i] = s->chg[i] = -1;
    s->wear[0] = s->wear[1] = -1;
    s->noise = s->anc_lvl = s->amb_lvl = s->adapt_lvl = s->conv = s->eq = -1;
    s->lock = s->eardet = s->game = s->cycle = -1;
    for (int i = 0; i < 2; i++) for (int g = 0; g < BG_N; g++) s->gest[i][g] = -1;
    s->color = -1;
    s->bass = -1; s->fit[0] = s->fit[1] = -1; s->band_min = -6; s->band_max = 6;
}
static void buds_cpy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src); if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
/* printable single-line text (state file values): control characters become spaces */
static void buds_clean(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < cap; p++) dst[n++] = *p < 32 || *p == 127 ? ' ' : (char)*p;
    dst[n] = 0;
}

/* ------------------------------------------------------------------ state file */
struct buds_hdr { int v, pid, fake; unsigned gen, ack, err; int err_code; };
enum { BE_OK = 0, BE_NOTSUP = -1, BE_NOTCONN = -2, BE_IO = -3, BE_RANGE = -4, BE_BUSY = -5, BE_UNKNOWN = -6 };

static void buds_ints(FILE *f, const char *k, const int *v, int n)
{
    fprintf(f, "%s=", k);
    for (int i = 0; i < n; i++) fprintf(f, i ? ",%d" : "%d", v[i]);
    fputc('\n', f);
}
static void buds_st_write(FILE *f, const struct buds_st *s)
{
    char b[80];
    fprintf(f, "dev=%s\n", s->addr);
    buds_clean(b, sizeof b, s->name); fprintf(f, "name=%s\n", b);
    buds_clean(b, sizeof b, s->model); fprintf(f, "model=%s\n", b);
    if (s->modelno[0]) { buds_clean(b, sizeof b, s->modelno); fprintf(f, "modelno=%s\n", b); }
    if (s->fw[0]) { buds_clean(b, sizeof b, s->fw); fprintf(f, "fw=%s\n", b); }
    if (s->serial[0]) { buds_clean(b, sizeof b, s->serial); fprintf(f, "serial=%s\n", b); }
    fprintf(f, "brand=%d\nfamily=%d\nmodel_id=%d\ncolor=%d\nconn=%d\ncaps=%u\n", s->brand, s->family, s->model_id, s->color, s->conn, s->caps);
    buds_ints(f, "batt", s->batt, 3); buds_ints(f, "chg", s->chg, 3); buds_ints(f, "wear", s->wear, 2);
    fprintf(f, "noise=%d,%d\nanc=%d,%d\namb=%d,%d\nadapt=%d\nconv=%d\neq=%d,%d\n", s->noise, s->noise_mask, s->anc_lvl, s->anc_max,
            s->amb_lvl, s->amb_max, s->adapt_lvl, s->conv, s->eq, s->eq_n);
    if (s->nbands > 0) { int v[11]; v[0] = s->nbands; for (int i = 0; i < s->nbands && i < 10; i++) v[i + 1] = s->bands[i]; buds_ints(f, "bands", v, 1 + (s->nbands > 10 ? 10 : s->nbands)); }
    fprintf(f, "lock=%d\neardet=%d\ngame=%d\ncycle=%d\n", s->lock, s->eardet, s->game, s->cycle);
    buds_ints(f, "gestl", s->gest[0], BG_N); buds_ints(f, "gestr", s->gest[1], BG_N);
    fprintf(f, "gopts=%u,%u,%u,%u\n", s->gest_opts[0], s->gest_opts[1], s->gest_opts[2], s->gest_opts[3]);
    buds_ints(f, "find", s->find, 2);
    fprintf(f, "eqset=%d,%d,%d\nbass=%d,%d\n", s->eq_set, s->band_min, s->band_max, s->bass, s->bass_max);
    buds_ints(f, "fit", s->fit, 2);
    fprintf(f, "busy=%d\npopseq=%d\nend\n", s->busy, s->popseq);
}
static int buds_parse_ints(const char *v, int *out, int max)
{
    int n = 0;
    while (*v && n < max) {
        char *e; long x = strtol(v, &e, 10);
        if (e == v) break;
        out[n++] = (int)x;
        v = *e == ',' ? e + 1 : e;
        if (*e != ',') break;
    }
    return n;
}
static void buds_st_kv(struct buds_st *s, const char *k, const char *v)
{
    int t[12];
    if (!strcmp(k, "name")) buds_cpy(s->name, sizeof s->name, v);
    else if (!strcmp(k, "model")) buds_cpy(s->model, sizeof s->model, v);
    else if (!strcmp(k, "modelno")) buds_cpy(s->modelno, sizeof s->modelno, v);
    else if (!strcmp(k, "fw")) buds_cpy(s->fw, sizeof s->fw, v);
    else if (!strcmp(k, "serial")) buds_cpy(s->serial, sizeof s->serial, v);
    else if (!strcmp(k, "brand")) s->brand = atoi(v);
    else if (!strcmp(k, "family")) s->family = atoi(v);
    else if (!strcmp(k, "model_id")) s->model_id = atoi(v);
    else if (!strcmp(k, "color")) s->color = atoi(v);
    else if (!strcmp(k, "conn")) s->conn = atoi(v);
    else if (!strcmp(k, "caps")) s->caps = (unsigned)strtoul(v, 0, 10);
    else if (!strcmp(k, "batt")) buds_parse_ints(v, s->batt, 3);
    else if (!strcmp(k, "chg")) buds_parse_ints(v, s->chg, 3);
    else if (!strcmp(k, "wear")) buds_parse_ints(v, s->wear, 2);
    else if (!strcmp(k, "noise") && buds_parse_ints(v, t, 2) == 2) { s->noise = t[0]; s->noise_mask = t[1]; }
    else if (!strcmp(k, "anc") && buds_parse_ints(v, t, 2) == 2) { s->anc_lvl = t[0]; s->anc_max = t[1]; }
    else if (!strcmp(k, "amb") && buds_parse_ints(v, t, 2) == 2) { s->amb_lvl = t[0]; s->amb_max = t[1]; }
    else if (!strcmp(k, "adapt")) s->adapt_lvl = atoi(v);
    else if (!strcmp(k, "conv")) s->conv = atoi(v);
    else if (!strcmp(k, "eq") && buds_parse_ints(v, t, 2) == 2) { s->eq = t[0]; s->eq_n = t[1]; }
    else if (!strcmp(k, "bands")) { int n = buds_parse_ints(v, t, 11); if (n >= 1) { s->nbands = t[0] < 0 ? 0 : t[0] > 10 ? 10 : t[0]; for (int i = 0; i < s->nbands; i++) s->bands[i] = i + 1 < n ? t[i + 1] : 0; } }
    else if (!strcmp(k, "lock")) s->lock = atoi(v);
    else if (!strcmp(k, "eardet")) s->eardet = atoi(v);
    else if (!strcmp(k, "game")) s->game = atoi(v);
    else if (!strcmp(k, "cycle")) s->cycle = atoi(v);
    else if (!strcmp(k, "gestl")) buds_parse_ints(v, s->gest[0], BG_N);
    else if (!strcmp(k, "gestr")) buds_parse_ints(v, s->gest[1], BG_N);
    else if (!strcmp(k, "gopts")) { int n = buds_parse_ints(v, t, BG_N); for (int i = 0; i < n; i++) s->gest_opts[i] = (unsigned)t[i]; }
    else if (!strcmp(k, "find")) buds_parse_ints(v, s->find, 2);
    else if (!strcmp(k, "eqset") && buds_parse_ints(v, t, 3) == 3) { s->eq_set = t[0]; s->band_min = t[1]; s->band_max = t[2]; }
    else if (!strcmp(k, "bass") && buds_parse_ints(v, t, 2) == 2) { s->bass = t[0]; s->bass_max = t[1]; }
    else if (!strcmp(k, "fit")) buds_parse_ints(v, s->fit, 2);
    else if (!strcmp(k, "busy")) s->busy = atoi(v);
    else if (!strcmp(k, "popseq")) s->popseq = atoi(v);
}
/* EQ preset names (keys for tr(), Spanish literals) of each set */
static const char *const BUDS_EQ_SAMSUNG[] = { "Normal", "Graves potenciados", "Suave", "Dinámico", "Claro", "Agudos potenciados" };
static const char *const BUDS_EQ_NOTHING[] = { "Equilibrado", "Voz", "Más agudos", "Más graves", "Personalizado" };
static const char *const BUDS_EQ_CMF[] = { "Rock", "Electrónica", "Pop", "Voces realzadas", "Clásica", "Personalizado", "Equilibrado" };
static const char *const BUDS_EQ_SONY[] = { "Desactivado", "Brillante", "Animado", "Suave", "Relajado", "Voz", "Agudos potenciados", "Graves potenciados", "Habla", "Manual" };
static const char *const *buds_eq_names(int set, int *n)
{
    switch (set) {
    case ES_NOTHING: *n = 5; return BUDS_EQ_NOTHING;
    case ES_CMF: *n = 7; return BUDS_EQ_CMF;
    case ES_SONY: *n = 10; return BUDS_EQ_SONY;
    default: *n = 6; return BUDS_EQ_SAMSUNG;
    }
}
static void buds_hdr_write(FILE *f, const struct buds_hdr *h)
{
    fprintf(f, "v=1\npid=%d\nfake=%d\ngen=%u\nack=%u\nerr=%u,%d\n", h->pid, h->fake, h->gen, h->ack, h->err, h->err_code);
}
/* whole state file -> header + devices (returns the number of devices) */
static int buds_state_parse(const char *txt, struct buds_hdr *h, struct buds_st *out, int max)
{
    int n = 0, in = 0; char k[24], v[96];
    memset(h, 0, sizeof *h);
    for (const char *l = txt; l && *l; ) {
        const char *e = strchr(l, '\n'); if (!e) e = l + strlen(l);
        const char *eq = memchr(l, '=', (size_t)(e - l));
        if (e - l == 3 && !strncmp(l, "end", 3)) in = 0;
        else if (eq && eq - l < (int)sizeof k) {
            memcpy(k, l, (size_t)(eq - l)); k[eq - l] = 0;
            int vl = (int)(e - eq - 1); if (vl > (int)sizeof v - 1) vl = sizeof v - 1;
            memcpy(v, eq + 1, (size_t)vl); v[vl] = 0;
            if (!strcmp(k, "dev")) {
                in = n < max;
                if (in) { buds_st_init(&out[n]); buds_cpy(out[n].addr, sizeof out[n].addr, v); n++; }
            } else if (in) buds_st_kv(&out[n - 1], k, v);
            else if (!strcmp(k, "v")) h->v = atoi(v);
            else if (!strcmp(k, "pid")) h->pid = atoi(v);
            else if (!strcmp(k, "fake")) h->fake = atoi(v);
            else if (!strcmp(k, "gen")) h->gen = (unsigned)strtoul(v, 0, 10);
            else if (!strcmp(k, "ack")) h->ack = (unsigned)strtoul(v, 0, 10);
            else if (!strcmp(k, "err")) { int t[2]; if (buds_parse_ints(v, t, 2) == 2) { h->err = (unsigned)t[0]; h->err_code = t[1]; } }
        }
        l = *e ? e + 1 : 0;
    }
    return n;
}

/* ------------------------------------------------------------------ commands */
enum { BCMD_NONE, BCMD_NOISE, BCMD_ANCLVL, BCMD_AMBLVL, BCMD_ADAPTLVL, BCMD_CONV, BCMD_EQ, BCMD_LOCK, BCMD_EARDET,
       BCMD_GAME, BCMD_CYCLE, BCMD_GEST, BCMD_FIND, BCMD_RENAME, BCMD_DISCONNECT, BCMD_CONNECT, BCMD_REFRESH,
       BCMD_FAKE, BCMD_BANDS, BCMD_BASS, BCMD_FIT, BCMD_N };
static const char *const BCMD_NAMES[BCMD_N] = { "", "noise", "anclvl", "amblvl", "adaptlvl", "conv", "eq", "lock", "eardet",
                                                "game", "cycle", "gest", "find", "rename", "disconnect", "connect", "refresh", "fake", "bands", "bass", "fit" };
/* rename: s = name; bands: s = "g1,g2,..." (dB) */
struct bcmd { unsigned seq; char addr[18]; int op, a, b, c; char s[64]; };

/* "SEQ ADDR VERB a b c" or "SEQ ADDR rename text" */
static int bcmd_format(char *out, int cap, const struct bcmd *c)
{
    if (c->op <= 0 || c->op >= BCMD_N) return -1;
    if (c->op == BCMD_RENAME || c->op == BCMD_BANDS) { char t[64]; buds_clean(t, sizeof t, c->s); return snprintf(out, (size_t)cap, "%u %s %s %s\n", c->seq, c->addr[0] ? c->addr : "-", BCMD_NAMES[c->op], t); }
    return snprintf(out, (size_t)cap, "%u %s %s %d %d %d\n", c->seq, c->addr[0] ? c->addr : "-", BCMD_NAMES[c->op], c->a, c->b, c->c);
}
static int bcmd_parse(const char *line, struct bcmd *c)
{
    memset(c, 0, sizeof *c);
    char verb[16], addr[24]; int pos = 0;
    if (sscanf(line, "%u %23s %15s %n", &c->seq, addr, verb, &pos) < 3) return -1;
    if (strcmp(addr, "-")) buds_cpy(c->addr, sizeof c->addr, addr);
    for (int i = 1; i < BCMD_N; i++) if (!strcmp(verb, BCMD_NAMES[i])) c->op = i;
    if (!c->op) return -1;
    if (c->op == BCMD_RENAME || c->op == BCMD_BANDS) {
        const char *s = line + pos; char t[64]; int n = 0;
        while (s[n] && s[n] != '\n' && s[n] != '\r' && n < 63) { t[n] = s[n]; n++; }
        t[n] = 0;
        while (n > 0 && t[n - 1] == ' ') t[--n] = 0;
        buds_cpy(c->s, sizeof c->s, t);
        return t[0] ? 0 : -1;
    }
    c->a = c->b = c->c = 0;
    sscanf(line + pos, "%d %d %d", &c->a, &c->b, &c->c);
    return 0;
}

/* ------------------------------------------------------------------ CRC-16/XMODEM (CCITT, poly 0x1021, init 0) */
static uint16_t buds_crc16(const uint8_t *p, int n)
{
    uint16_t c = 0;
    while (n-- > 0) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* ------------------------------------------------------------------ bluez (dbus-send dump) */
#define BD_MAXUUID 16
struct bd_dev {
    char path[64], addr[18], name[64], alias[64], icon[32], modalias[48];
    int paired, connected, resolved, nuuid;
    char uuid[BD_MAXUUID][37];
};
static const char *bd_skipsp(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int bd_quoted(const char *l, const char *e, char *out, int cap)
{
    const char *a = memchr(l, '"', (size_t)(e - l)); if (!a) return 0;
    const char *b = e - 1; while (b > a && *b != '"') b--;
    if (b <= a) return 0;
    int n = (int)(b - a - 1); if (n > cap - 1) n = cap - 1;
    memcpy(out, a + 1, (size_t)n); out[n] = 0;
    return 1;
}
static int bd_path_addr(const char *path, char *out)
{
    const char *d = strstr(path, "/dev_");
    if (!d || strlen(d + 5) != 17 || strchr(d + 5, '/')) return 0;
    for (int i = 0; i < 17; i++) out[i] = d[5 + i] == '_' ? ':' : d[5 + i];
    out[17] = 0;
    return 1;
}
/* "GetManagedObjects" as printed by dbus-send --print-reply: devices of hci0 with the
 * properties we need (Modalias and the whole UUID list, which the Wireless parser skips) */
static int bd_bluez_parse(const char *txt, struct bd_dev *dv, int max)
{
    int n = 0, iface = 0, in_uuids = 0;      /* iface 2 = org.bluez.Device1 */
    struct bd_dev *cur = 0;
    char key[64] = "", val[96];
    for (const char *l = txt; l && *l; ) {
        const char *e = strchr(l, '\n'); if (!e) e = l + strlen(l);
        const char *s = bd_skipsp(l);
        int len = (int)(e - s);
        if (len >= 12 && !strncmp(s, "object path ", 12)) {
            char p[96] = "", a[18]; bd_quoted(s, e, p, sizeof p);
            cur = 0; iface = 0; in_uuids = 0;
            if (!strncmp(p, "/org/bluez/hci0/", 16) && bd_path_addr(p, a)) {
                int k;
                for (k = 0; k < n; k++) if (!strcmp(dv[k].path, p)) break;
                if (k == n && n < max) { memset(&dv[n], 0, sizeof dv[n]); buds_cpy(dv[n].path, sizeof dv[n].path, p); memcpy(dv[n].addr, a, 18); n++; }
                if (k < n) cur = &dv[k];
            }
        } else if (len >= 7 && !strncmp(s, "string ", 7)) {
            char q[96] = ""; bd_quoted(s, e, q, sizeof q);
            if (in_uuids) { if (cur && iface == 2 && cur->nuuid < BD_MAXUUID) buds_cpy(cur->uuid[cur->nuuid++], 37, q); }
            else buds_cpy(key, sizeof key, q);
        } else if (len >= 7 && !strncmp(s, "array [", 7) && len <= 8) {
            if (!strncmp(key, "org.", 4)) { iface = !strcmp(key, "org.bluez.Device1") ? 2 : 0; key[0] = 0; }
        } else if (len >= 1 && *s == ']') {
            in_uuids = 0;
        } else if (len >= 8 && !strncmp(s, "variant ", 8)) {
            const char *v = bd_skipsp(s + 8);
            int vb = !strncmp(v, "boolean true", 12), isstr = !strncmp(v, "string ", 7);
            val[0] = 0; if (isstr) bd_quoted(v, e, val, sizeof val);
            if (cur && iface == 2) {
                if (!strncmp(v, "array [", 7) && !strcmp(key, "UUIDs")) in_uuids = 1;
                else if (!strcmp(key, "Name") && isstr) buds_cpy(cur->name, sizeof cur->name, val);
                else if (!strcmp(key, "Alias") && isstr) buds_cpy(cur->alias, sizeof cur->alias, val);
                else if (!strcmp(key, "Icon") && isstr) buds_cpy(cur->icon, sizeof cur->icon, val);
                else if (!strcmp(key, "Modalias") && isstr) buds_cpy(cur->modalias, sizeof cur->modalias, val);
                else if (!strcmp(key, "Paired")) cur->paired = vb;
                else if (!strcmp(key, "Connected")) cur->connected = vb;
                else if (!strcmp(key, "ServicesResolved")) cur->resolved = vb;
            }
        }
        l = *e ? e + 1 : 0;
    }
    return n;
}
/* "bluetooth:v004Cp2014d0B10" / "usb:v05ACp..." -> vendor, product (1 if parsed) */
static int bd_modalias(const char *m, unsigned *vendor, unsigned *product)
{
    const char *v = strchr(m, ':'); if (!v) return 0;
    unsigned a, b;
    if (sscanf(v + 1, "v%4xp%4x", &a, &b) != 2) return 0;
    *vendor = a; *product = b;
    return 1;
}
static int bd_has_uuid(const struct bd_dev *d, const char *u)
{
    for (int i = 0; i < d->nuuid; i++) {
        const char *a = d->uuid[i], *b = u; int same = 1;
        for (; *a && *b; a++, b++) { int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b; if (x != y) { same = 0; break; } }
        if (same && !*a && !*b) return 1;
    }
    return 0;
}
static int bd_audio(const struct bd_dev *d)
{
    if (!strncmp(d->icon, "audio-", 6)) return 1;
    for (int i = 0; i < d->nuuid; i++) if (!strncmp(d->uuid[i], "0000110b", 8) || !strncmp(d->uuid[i], "0000110B", 8)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ brand modules */
/* Model description (static tables in each brand file). gest_opts: per gesture, the actions
 * (1 << BA_*) that can be chosen; 0 = that gesture is not configurable. */
struct buds_model {
    uint8_t brand; uint16_t id; const char *name, *match;
    uint8_t family; uint32_t caps; uint8_t noise_mask, amb_max, anc_max, eq_n;
    uint16_t gest_opts[BG_N];
    uint8_t layout;              /* brand-specific (Samsung: framing / status layout) */
};
struct bud;
enum { BT_L2CAP = 1, BT_RFCOMM = 2 };
struct buds_brand {
    int brand; const char *tag;
    /* model for a bluez device, or 0 if not this brand (no I/O) */
    const struct buds_model *(*identify)(const struct bd_dev *d);
    int transport; uint16_t psm;                        /* BT_L2CAP: fixed PSM */
    const char *(*uuid)(const struct bud *b);           /* BT_RFCOMM: service UUID (SDP) */
    void (*start)(struct bud *b);                       /* channel open: handshake */
    void (*rx)(struct bud *b, const uint8_t *p, int n); /* bytes (stream) or one packet (L2CAP) */
    int (*cmd)(struct bud *b, const struct bcmd *c);    /* BE_* */
    void (*tick)(struct bud *b, double now_ms);         /* every second while open (may be 0) */
    void (*fill)(struct bud *b);                         /* optional: derive capabilities after bud_set_model() */
    const char *(*pick_uuid)(const struct bd_dev *d);   /* optional: the service UUID of this device (UUID families) */
    int (*channel)(const struct bud *b);                 /* optional: fixed RFCOMM channel (> 0: no SDP) */
};
/* one device as seen by a brand module: protocol state + the published state */
struct bud {
    struct buds_st s;
    const struct buds_model *m;
    const struct buds_brand *br;
    int (*tx)(struct bud *b, const uint8_t *p, int n);  /* send (daemon socket or test capture) */
    void (*log)(struct bud *b, const char *msg);
    void *io;
    int changed;                 /* modules set it when s changed (the daemon republishes) */
    int ready;                   /* first status received */
    /* stream reassembly (RFCOMM) and fragments */
    uint8_t rbuf[2048]; int rn;
    uint8_t frag[2048]; int fragn, fragid;
    int crc_order;               /* Samsung: 0 unknown, 1 little-endian, 2 big-endian (learnt from the buds) */
    int mgr_sent, ext_seen;
    unsigned unknown_ids[8];     /* message ids already logged as unknown */
    double t_open, find_t;
    double now;                  /* monotonic ms, set by the daemon before every rx/cmd/tick call */
    char uuid[37];               /* service UUID chosen by pick_uuid() at identification */
    union { uint8_t raw[768]; double align; } priv;   /* brand module private state */
};
static void bud_log(struct bud *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bud_log(struct bud *b, const char *fmt, ...)
{
    if (!b->log) return;
    char m[400]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);
    b->log(b, m);
}
static void bud_hex(char *out, int cap, const uint8_t *p, int n)
{
    int k = 0;
    for (int i = 0; i < n && k + 4 < cap; i++) k += snprintf(out + k, (size_t)(cap - k), "%02x ", p[i]);
    if (k > 0) out[k - 1] = 0; else if (cap) out[0] = 0;
}
/* log an unknown message id once per connection (for later captures) */
static void bud_unknown(struct bud *b, unsigned id, const uint8_t *p, int n)
{
    for (int i = 0; i < 8; i++) if (b->unknown_ids[i] == id + 1) return;
    for (int i = 0; i < 8; i++) if (!b->unknown_ids[i]) { b->unknown_ids[i] = id + 1; break; }
    char h[200]; bud_hex(h, sizeof h, p, n > 60 ? 60 : n);
    bud_log(b, "unknown 0x%04x (%d bytes): %s", id, n, h);
}
/* apply a model description to the published state (keeps the values already known) */
static void bud_set_model(struct bud *b, const struct buds_model *m)
{
    b->m = m;
    struct buds_st *s = &b->s;
    s->brand = m->brand; s->family = m->family; s->model_id = m->id; s->caps = m->caps;
    buds_cpy(s->model, sizeof s->model, m->name);
    s->noise_mask = m->noise_mask; s->amb_max = m->amb_max; s->anc_max = m->anc_max; s->eq_n = m->eq_n;
    for (int g = 0; g < BG_N; g++) s->gest_opts[g] = m->gest_opts[g];
    b->changed = 1;
}
#define BUDS_MASK(m) (1u << (m))
#endif
