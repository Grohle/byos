/* buds_soundcore.inc.c - Anker Soundcore headphones over RFCOMM, brand module of hibybudsd.
 * Pure protocol code: bytes in, bytes out through struct bud (buds_core.inc.c).
 *
 * Facts from the public OpenSCQ30 project (github.com/Oppzippy/OpenSCQ30, GPL-3.0: facts only,
 * no code), written in our own words in docs/HEADPHONES.md section 10.1.
 *   Service: a vendor RFCOMM UUID of the family 0cf12d31-fac3-4553-bd80-d6832e7xxxxx (the last
 *   20 bits vary per model: we pick the one BlueZ lists).
 *   Host -> buds: 08 EE 00 00 00 | command (2 bytes) | total length LE16 | body | checksum
 *   Buds -> host: 09 FF 00 00 01 | command | total length LE16 | body | checksum
 *   total length counts the whole packet including the checksum; checksum = sum of all the
 *   previous bytes mod 256.
 *   01 01 state (layout per model: logged), 01 03 battery level (single, or L R), 01 04 charging,
 *   01 05 firmware + serial, 06 01 / 06 81 sound modes {ambient, ANC mode, transparency, custom}.
 * Writes (sound modes) only on the models whose sound-mode block is the common one; the others
 * are read-only (BC_RO) until a capture confirms them. EQ, HearID and buttons: not implemented. */
#ifndef BUDS_SOUNDCORE_INC_C
#define BUDS_SOUNDCORE_INC_C
#include "buds_core.inc.c"

#define SC_UUID_PREFIX "0cf12d31-fac3-4553-bd80-d6832e7"     /* + 5 hex digits */
enum { SC_AMB_ANC = 0, SC_AMB_TRANSP = 1, SC_AMB_NORMAL = 2 };   /* ambient sound mode byte */
enum { SC_NC_TRANSPORT = 0, SC_NC_OUTDOOR = 1, SC_NC_INDOOR = 2, SC_NC_CUSTOM = 3 };
#define SC_M3 (BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC) | BUDS_MASK(BN_AMBIENT))
#define SC_HEAD (BC_BATT_ONE | BC_FW)
#define SC_TWS (BC_BATT_LR | BC_FW)
/* id = model number (A3028 -> 0x3028); layout bit 1 = writes verified (common sound-mode block) */
static const struct buds_model SOUNDCORE_MODELS[] = {
    { BB_SOUNDCORE, 0x3954, "Soundcore Liberty 4 Pro", "Liberty 4 Pro", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3947, "Soundcore Liberty 4 NC", "Liberty 4 NC", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3953, "Soundcore Liberty 4", "Liberty 4", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3957, "Soundcore Liberty 5", "Liberty 5", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3952, "Soundcore Liberty 3 Pro", "Liberty 3 Pro", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3951, "Soundcore Liberty Air 2 Pro", "Liberty Air 2 Pro", BF_PODS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3930, "Soundcore Liberty 2 Pro+", "Liberty 2 Pro+", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3909, "Soundcore Liberty 2 Pro", "Liberty 2 Pro", BF_BUDS, SC_TWS, 0, 0, 0, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3936, "Soundcore Space A40", "Space A40", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3939, "Soundcore Life P3", "Life P3", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL | BC_RO, SC_M3, 0, 2, 0, { 0 }, 0 },
    { BB_SOUNDCORE, 0x3933, "Soundcore Life Note 3", "Life Note 3", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3931, "Soundcore Life Dot 2 NC", "Life Dot 2 NC", BF_BUDS, SC_TWS | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3040, "Soundcore Space Q45", "Space Q45", BF_HEAD, SC_HEAD | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3062, "Soundcore Space One Pro", "Space One Pro", BF_HEAD, SC_HEAD | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3035, "Soundcore Space One", "Space One", BF_HEAD, SC_HEAD | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3027, "Soundcore Life Q35", "Life Q35", BF_HEAD, SC_HEAD | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3028, "Soundcore Life Q30", "Q30", BF_HEAD, SC_HEAD | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
    { BB_SOUNDCORE, 0x3004, "Soundcore Q20i", "Q20i", BF_HEAD, SC_HEAD | BC_NOISE | BC_ANC_LVL, SC_M3, 0, 2, 0, { 0 }, 1 },
};
static const struct buds_model SOUNDCORE_GENERIC = { BB_SOUNDCORE, 0, "Soundcore", 0, BF_BUDS, BC_BATT_LR | BC_BATT_ONE | BC_FW, 0, 0, 0, 0, { 0 }, 0 };
static const char *sc_pick_uuid(const struct bd_dev *d)
{
    for (int i = 0; i < d->nuuid; i++) {
        const char *u = d->uuid[i]; int ok = 1;
        for (int k = 0; SC_UUID_PREFIX[k]; k++) { int c = u[k] >= 'A' && u[k] <= 'Z' ? u[k] + 32 : u[k]; if (c != SC_UUID_PREFIX[k]) { ok = 0; break; } }
        if (ok && strlen(u) == 36) return u;
    }
    return 0;
}
static const struct buds_model *sc_identify(const struct bd_dev *d)
{
    const char *nm = d->name[0] ? d->name : d->alias;
    int uuid = sc_pick_uuid(d) != 0, named = strstr(nm, "Soundcore") || strstr(nm, "soundcore") || !strncmp(nm, "Anker", 5);
    if (!uuid && !named) return 0;
    for (unsigned i = 0; i < sizeof SOUNDCORE_MODELS / sizeof *SOUNDCORE_MODELS; i++) if (strstr(nm, SOUNDCORE_MODELS[i].match)) return &SOUNDCORE_MODELS[i];
    return uuid ? &SOUNDCORE_GENERIC : 0;
}
static const char *sc_uuid(const struct bud *b) { return b->uuid[0] ? b->uuid : SC_UUID_PREFIX "00000"; }
struct sc_priv { int nc, transp, custom; };
static struct sc_priv *scp(struct bud *b) { return (struct sc_priv *)b->priv.raw; }

/* packet builder (direction 0 host -> buds, 1 buds -> host); returns length or -1 */
static int sc_packet(int inbound, int c0, int c1, const uint8_t *body, int n, uint8_t *o, int cap)
{
    int tot = 5 + 2 + 2 + n + 1;
    if (n < 0 || tot > cap || tot > 0xFFFF) return -1;
    static const uint8_t ho[5] = { 0x08, 0xEE, 0x00, 0x00, 0x00 }, hi[5] = { 0x09, 0xFF, 0x00, 0x00, 0x01 };
    memcpy(o, inbound ? hi : ho, 5);
    o[5] = (uint8_t)c0; o[6] = (uint8_t)c1; o[7] = (uint8_t)tot; o[8] = (uint8_t)(tot >> 8);
    if (n) memcpy(o + 9, body, (size_t)n);
    unsigned s = 0; for (int i = 0; i < tot - 1; i++) s += o[i];
    o[tot - 1] = (uint8_t)s;
    return tot;
}
static int sc_send(struct bud *b, int c0, int c1, const uint8_t *body, int n)
{
    uint8_t p[64]; int k = sc_packet(0, c0, c1, body, n, p, sizeof p);
    if (k < 0 || !b->tx) return BE_IO;
    return b->tx(b, p, k) == k ? BE_OK : BE_IO;
}
static void sc_start(struct bud *b)
{
    struct sc_priv *p = scp(b); p->nc = SC_NC_INDOOR; p->transp = 1; p->custom = 0;
    sc_send(b, 0x01, 0x01, 0, 0);                  /* state */
    sc_send(b, 0x01, 0x03, 0, 0);                  /* battery level */
    sc_send(b, 0x01, 0x04, 0, 0);                  /* charging */
    sc_send(b, 0x01, 0x05, 0, 0);                  /* firmware + serial */
}
static int sc_level(int v) { return v <= 5 ? v * 20 : v <= 10 ? v * 10 : v <= 100 ? v : -1; }   /* 0..5 steps (newer: 0..9/10) [I] */
/* our ANC level 0 light .. 2 strongest <-> Soundcore ANC mode (indoor, outdoor, transport) */
static int sc_lvl_from_nc(int nc) { return nc == SC_NC_INDOOR ? 0 : nc == SC_NC_OUTDOOR ? 1 : nc == SC_NC_TRANSPORT ? 2 : -1; }
static int sc_nc_from_lvl(int l) { return l <= 0 ? SC_NC_INDOOR : l == 1 ? SC_NC_OUTDOOR : SC_NC_TRANSPORT; }
static void sc_msg(struct bud *b, int c0, int c1, const uint8_t *d, int n)
{
    struct buds_st *s = &b->s; struct sc_priv *p = scp(b);
    int cmd = c0 << 8 | c1;
    switch (cmd) {
    case 0x0103:
        if (n == 1 || (s->caps & BC_BATT_ONE && !(s->caps & BC_BATT_LR))) { if (n >= 1) s->batt[0] = sc_level(d[0]); }
        else if (n >= 2) { s->batt[0] = sc_level(d[0]); s->batt[1] = sc_level(d[1]); }
        b->ready = 1; b->changed = 1;
        break;
    case 0x0104:
        if (n >= 1) s->chg[0] = d[0] != 0;
        if (n >= 2) s->chg[1] = d[1] != 0;
        b->changed = 1;
        break;
    case 0x0105: {                                 /* firmware version(s) and serial, ASCII */
        char run[40]; int k = 0, firstfw = 1;
        for (int i = 0; i <= n; i++) {
            int c = i < n ? d[i] : 0;
            if (c > 32 && c < 127 && k < 39) { run[k++] = (char)c; continue; }
            run[k] = 0;
            if (k >= 12 && !s->serial[0]) buds_cpy(s->serial, sizeof s->serial, run);
            else if (k >= 3 && strchr(run, '.') && firstfw) { buds_cpy(s->fw, sizeof s->fw, run); firstfw = 0; }
            k = 0;
        }
        /* fixed layout seen in practice: two 5-byte versions then a 16-byte serial [I] */
        if (!s->fw[0] && n >= 10) { char v[6]; memcpy(v, d, 5); v[5] = 0; buds_cpy(s->fw, sizeof s->fw, v); }
        if (!s->serial[0] && n >= 26) { char v[17]; memcpy(v, d + 10, 16); v[16] = 0; buds_cpy(s->serial, sizeof s->serial, v); }
        b->changed = 1;
        break;
    }
    case 0x0601:                                   /* sound modes */
        if (n >= 4) {
            s->noise = d[0] == SC_AMB_ANC ? BN_ANC : d[0] == SC_AMB_TRANSP ? BN_AMBIENT : d[0] == SC_AMB_NORMAL ? BN_OFF : -1;
            p->nc = d[1]; p->transp = d[2]; p->custom = d[3];
            if (sc_lvl_from_nc(d[1]) >= 0) s->anc_lvl = sc_lvl_from_nc(d[1]);
            b->changed = 1;
        }
        break;
    default: bud_unknown(b, (unsigned)cmd, d, n); break;   /* 01 01 state (layout per model) and the rest: logged once */
    }
    if (b->changed) s->conn = BS_READY;
}
static void sc_rx(struct bud *b, const uint8_t *p, int n)
{
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    while (i < b->rn) {
        const uint8_t *f = b->rbuf + i; int left = b->rn - i;
        if (f[0] != 0x09) { i++; continue; }
        if (left < 10) break;
        if (f[1] != 0xFF || f[2] != 0x00 || f[3] != 0x00 || f[4] != 0x01) { i++; continue; }
        int tot = f[7] | f[8] << 8;
        if (tot < 10 || tot > (int)sizeof b->rbuf) { i++; continue; }
        if (left < tot) break;
        unsigned s = 0; for (int k = 0; k < tot - 1; k++) s += f[k];
        if ((uint8_t)s != f[tot - 1]) { bud_log(b, "soundcore: bad checksum"); i++; continue; }
        sc_msg(b, f[5], f[6], f + 9, tot - 10);
        i += tot;
    }
    if (i > 0) { memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
}
static int sc_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; struct sc_priv *p = scp(b);
    if (c->op != BCMD_NOISE && c->op != BCMD_ANCLVL) return BE_NOTSUP;
    if (!(s->caps & BC_NOISE) || (s->caps & BC_RO)) return BE_NOTSUP;
    int mode = c->op == BCMD_NOISE ? c->a : BN_ANC, nc = c->op == BCMD_ANCLVL ? sc_nc_from_lvl(c->a) : (p->nc <= 2 ? p->nc : SC_NC_INDOOR);
    if (mode < 0 || mode >= BN_N || !(s->noise_mask & BUDS_MASK(mode))) return BE_RANGE;
    if (c->op == BCMD_ANCLVL && (c->a < 0 || c->a > 2)) return BE_RANGE;
    uint8_t body[4] = { (uint8_t)(mode == BN_ANC ? SC_AMB_ANC : mode == BN_AMBIENT ? SC_AMB_TRANSP : SC_AMB_NORMAL), (uint8_t)nc, (uint8_t)p->transp, (uint8_t)p->custom };
    int r = sc_send(b, 0x06, 0x81, body, 4);
    if (r == BE_OK) { s->noise = mode; if (c->op == BCMD_ANCLVL) s->anc_lvl = c->a; p->nc = nc; b->changed = 1; }
    return r;
}
/* models whose writes are not verified stay read-only even if BC_RO was cleared elsewhere */
static void sc_fill(struct bud *b) { if (b->m && b->m->noise_mask && !(b->m->layout & 1)) b->s.caps |= BC_RO; }
static const struct buds_brand BRAND_SOUNDCORE = { BB_SOUNDCORE, "soundcore", sc_identify, BT_RFCOMM, 0, sc_uuid, sc_start, sc_rx, sc_cmd, 0, sc_fill, sc_pick_uuid, 0 };
#endif
