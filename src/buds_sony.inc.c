/* buds_sony.inc.c - Sony headphones (MDR protocol) over RFCOMM, brand module of hibybudsd.
 * Written from docs/research_headphones_nothing_sony_jbl.md section 3 (facts only).
 *
 * READ-ONLY by default: battery, noise control state, EQ preset, firmware are read and shown;
 * every setting is refused (BE_NOTSUP, capability BC_RO) until one real capture per dialect
 * confirms the payloads. Writes can be enabled for tests with sony_writes = 1
 * (hibybudsd: environment HIBYBUDS_SONY_WRITES=1).
 *
 * UUIDs: V1 96CC203E-5068-46AD-B32D-E316F5E069BA (XM3/XM4 class), V2 956C7B26-D49A-4BA8-B03F-
 * B17D393CB6E2 (XM5, LinkBuds). The payload dialect comes from the size of the init reply.
 * Frame: 3E | type | seq | len u32 BE | payload | checksum | 3C, with 3C/3D/3E escaped as
 * 3D (b & 0xEF); checksum = sum of type..payload mod 256. Every data frame (type 0C/0E) is
 * acknowledged with type 01 and seq 1-s; one command in flight, the next after its ACK. */
#ifndef BUDS_SONY_INC_C
#define BUDS_SONY_INC_C
#include "buds_core.inc.c"

#define SONY_UUID_V1 "96cc203e-5068-46ad-b32d-e316f5e069ba"
#define SONY_UUID_V2 "956c7b26-d49a-4ba8-b03f-b17d393cb6e2"
static int sony_writes;
enum { SY_ACK = 0x01, SY_CMD = 0x0C, SY_CMD2 = 0x0E };
/* layout bits */
enum { SYL_WIND = 1, SYL_V2 = 2, SYL_TWS = 4, SYL_NONC = 8 };
#define SY_HEAD (BC_BATT_ONE | BC_FW | BC_EQ | BC_RO)
#define SY_TWS (BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_EQ | BC_RO)
#define SY_NC (BC_NOISE | BC_AMB_LVL | BC_CONV)
#define SY_M3 (BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC) | BUDS_MASK(BN_AMBIENT))
static const struct buds_model SONY_MODELS[] = {
    { BB_SONY, 13, "Sony WH-1000XM3", "WH-1000XM3", BF_HEAD, SY_HEAD | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_WIND },
    { BB_SONY, 14, "Sony WH-1000XM4", "WH-1000XM4", BF_HEAD, SY_HEAD | SY_NC, SY_M3, 20, 0, 10, { 0 }, 0 },
    { BB_SONY, 15, "Sony WH-1000XM5", "WH-1000XM5", BF_HEAD, SY_HEAD | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_V2 },
    { BB_SONY, 16, "Sony WH-1000XM6", "WH-1000XM6", BF_HEAD, SY_HEAD | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_V2 },
    { BB_SONY, 23, "Sony WF-1000XM3", "WF-1000XM3", BF_BUDS, SY_TWS | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_WIND | SYL_TWS },
    { BB_SONY, 24, "Sony WF-1000XM4", "WF-1000XM4", BF_BUDS, SY_TWS | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_WIND | SYL_TWS },
    { BB_SONY, 25, "Sony WF-1000XM5", "WF-1000XM5", BF_BUDS, SY_TWS | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_V2 | SYL_TWS },
    { BB_SONY, 31, "Sony LinkBuds S", "LinkBuds S", BF_BUDS, SY_TWS | SY_NC, SY_M3, 20, 0, 10, { 0 }, SYL_V2 | SYL_TWS },
    { BB_SONY, 30, "Sony LinkBuds", "LinkBuds", BF_BUDS, SY_TWS, 0, 0, 0, 10, { 0 }, SYL_V2 | SYL_TWS | SYL_NONC },
};
static const struct buds_model SONY_GENERIC_HEAD = { BB_SONY, 1, "Sony", 0, BF_HEAD, BC_BATT_ONE | BC_FW | BC_RO, 0, 0, 0, 0, { 0 }, 0 };
static const struct buds_model SONY_GENERIC_TWS = { BB_SONY, 2, "Sony", 0, BF_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_RO, 0, 0, 0, 0, { 0 }, SYL_TWS };
static const struct buds_model *sony_identify(const struct bd_dev *d)
{
    const char *nm = d->name[0] ? d->name : d->alias;
    int v1 = bd_has_uuid(d, SONY_UUID_V1), v2 = bd_has_uuid(d, SONY_UUID_V2);
    for (unsigned i = 0; i < sizeof SONY_MODELS / sizeof *SONY_MODELS; i++) if (strstr(nm, SONY_MODELS[i].match) && (v1 || v2 || strstr(nm, "1000X") || strstr(nm, "LinkBuds"))) return &SONY_MODELS[i];
    if (!v1 && !v2) return 0;
    return !strncmp(nm, "WF-", 3) || strstr(nm, "LinkBuds") ? &SONY_GENERIC_TWS : &SONY_GENERIC_HEAD;
}
struct sy_priv { int dialect, seq, await, retries, qn, qi, uuid_v2; uint8_t q[12][12]; int ql[12]; uint8_t qt[12]; double t_sent; };
static struct sy_priv *syp(struct bud *b) { return (struct sy_priv *)b->priv.raw; }
static const char *sony_uuid(const struct bud *b) { return b->m && (b->m->layout & SYL_V2) ? SONY_UUID_V2 : SONY_UUID_V1; }

/* frame builder with escaping; returns length or -1 */
static int sy_frame(int type, int seq, const uint8_t *pl, int n, uint8_t *o, int cap)
{
    uint8_t raw[300]; int k = 0;
    if (n < 0 || n > 280) return -1;
    raw[k++] = (uint8_t)type; raw[k++] = (uint8_t)seq; raw[k++] = 0; raw[k++] = 0; raw[k++] = (uint8_t)(n >> 8); raw[k++] = (uint8_t)n;
    if (n) memcpy(raw + k, pl, (size_t)n);
    k += n;
    unsigned sum = 0; for (int i = 0; i < k; i++) sum += raw[i];
    raw[k++] = (uint8_t)sum;
    int m = 0;
    if (cap < 2) return -1;
    o[m++] = 0x3E;
    for (int i = 0; i < k; i++) {
        if (m + 3 > cap) return -1;
        if (raw[i] == 0x3C || raw[i] == 0x3D || raw[i] == 0x3E) { o[m++] = 0x3D; o[m++] = raw[i] & 0xEF; }
        else o[m++] = raw[i];
    }
    o[m++] = 0x3C;
    return m;
}
static int sy_tx(struct bud *b, int type, int seq, const uint8_t *pl, int n)
{
    uint8_t f[600]; int k = sy_frame(type, seq, pl, n, f, sizeof f);
    if (k < 0 || !b->tx) return BE_IO;
    return b->tx(b, f, k) == k ? BE_OK : BE_IO;
}
static void sy_queue(struct bud *b, int type, const uint8_t *pl, int n)
{
    struct sy_priv *p = syp(b);
    if (p->qn >= 12 || n > 12) return;
    memcpy(p->q[p->qn], pl, (size_t)n); p->ql[p->qn] = n; p->qt[p->qn] = (uint8_t)type; p->qn++;
}
static void sy_pump(struct bud *b)
{
    struct sy_priv *p = syp(b);
    if (p->await || p->qi >= p->qn) return;
    sy_tx(b, p->qt[p->qi], p->seq, p->q[p->qi], p->ql[p->qi]);
    p->await = 1; p->t_sent = b->now; p->retries = 0;
}
static void sony_start(struct bud *b)
{
    struct sy_priv *p = syp(b); memset(p, 0, sizeof *p);
    static const uint8_t init[2] = { 0x00, 0x00 };
    sy_queue(b, SY_CMD, init, 2);
    sy_pump(b);
}
/* reads after the init reply told us the dialect */
static void sony_reads(struct bud *b)
{
    struct sy_priv *p = syp(b); int v2 = p->dialect == 2, tws = b->m && (b->m->layout & SYL_TWS);
    uint8_t q[4];
    q[0] = 0x04; q[1] = 0x02; sy_queue(b, SY_CMD, q, 2);                                      /* firmware */
    q[0] = v2 ? 0x22 : 0x10;
    if (tws) { q[1] = v2 ? 0x01 : 0x01; sy_queue(b, SY_CMD, q, 2); q[1] = v2 ? 0x0A : 0x02; sy_queue(b, SY_CMD, q, 2); }
    else { q[1] = 0x00; sy_queue(b, SY_CMD, q, 2); }
    if (b->s.caps & BC_NOISE) { q[0] = 0x66; q[1] = v2 ? 0x15 : 0x02; sy_queue(b, SY_CMD, q, 2); }
    q[0] = 0x56; q[1] = v2 ? 0x00 : 0x01; sy_queue(b, SY_CMD, q, 2);                       /* EQ */
    sy_pump(b);
}
static const uint8_t SY_EQ[10] = { 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0xA0 };
static void sy_battery(struct buds_st *s, const uint8_t *d, int n)
{
    if (n < 4) return;
    int type = d[1];
    if (n >= 6 && (type == 0x01 || type == 0x09)) {
        s->batt[0] = d[2] ? d[2] : -1; s->chg[0] = d[2] ? d[3] == 1 : -1;
        s->batt[1] = d[4] ? d[4] : -1; s->chg[1] = d[4] ? d[5] == 1 : -1;
    } else if (type == 0x02 || type == 0x0A) { s->batt[2] = d[2]; s->chg[2] = d[3] == 1; }
    else { s->batt[0] = d[2]; s->chg[0] = d[3] == 1; }
}
/* ambient sound control notification / reply (67 / 69) */
static void sy_asm(struct bud *b, const uint8_t *d, int n)
{
    struct buds_st *s = &b->s; struct sy_priv *p = syp(b);
    if (p->dialect == 2) {
        if (n < 6) return;
        int wind = d[1] == 0x17, en = d[3], amb = d[4] == 0x01;
        s->noise = !en ? BN_OFF : amb ? BN_AMBIENT : BN_ANC;
        int vi = wind ? 6 : 5;
        if (vi < n - 1) s->conv = d[vi] != 0;
        s->amb_lvl = d[n - 1] <= 20 ? d[n - 1] : -1;
    } else {
        if (n < 8) return;
        int on = d[2] == 0x11, wind = d[3] == 0x02, mode = d[4];
        s->noise = !on ? BN_OFF : (wind ? mode >= 1 : mode == 1) ? BN_ANC : BN_AMBIENT;
        s->conv = d[6] != 0; s->amb_lvl = d[7] <= 20 ? d[7] : -1;
    }
    b->changed = 1;
}
static void sy_payload(struct bud *b, const uint8_t *d, int n)
{
    struct buds_st *s = &b->s; struct sy_priv *p = syp(b);
    if (n < 1) return;
    int id = d[0], v2 = p->dialect == 2;
    if (id == 0x01) {                                     /* init reply: 4 bytes V1, 8 bytes V2 */
        p->dialect = n >= 8 ? 2 : 1;
        bud_log(b, "sony: protocol V%d", p->dialect);
        sony_reads(b);
        b->ready = 1; b->changed = 1; s->conn = BS_READY;
        return;
    }
    if (id == 0x05 && n >= 3) {                           /* firmware {05, 02, len, ascii} */
        char v[40]; int k = 0;
        for (int i = 3; i < n && k < 39; i++) if (d[i] >= 32 && d[i] < 127) v[k++] = (char)d[i];
        v[k] = 0; if (k) buds_cpy(s->fw, sizeof s->fw, v);
        b->changed = 1;
    } else if ((!v2 && (id == 0x11 || id == 0x13)) || (v2 && (id == 0x23 || id == 0x25))) { sy_battery(s, d, n); b->changed = 1; }
    else if (id == 0x67 || id == 0x69) sy_asm(b, d, n);
    else if ((id == 0x57 || id == 0x59) && n >= 3) {
        s->eq = -1; for (int i = 0; i < 10; i++) if (SY_EQ[i] == d[2]) s->eq = i;
        if (n >= 10) { s->bass = d[4] <= 20 ? d[4] : -1; s->nbands = 5; for (int i = 0; i < 5; i++) s->bands[i] = d[5 + i] - 10; }
        b->changed = 1;
    } else bud_unknown(b, (unsigned)id, d, n);
    if (b->changed) s->conn = BS_READY;
}
/* stream: frames between 3E and 3C, unescaped */
static void sony_rx(struct bud *b, const uint8_t *p, int n)
{
    struct sy_priv *sp = syp(b);
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    for (;;) {
        while (i < b->rn && b->rbuf[i] != 0x3E) i++;
        int e = i + 1; while (e < b->rn && b->rbuf[e] != 0x3C) e++;
        if (e >= b->rn) break;
        uint8_t raw[1100]; int k = 0, bad = 0;
        for (int j = i + 1; j < e && k < (int)sizeof raw; j++) {
            if (b->rbuf[j] == 0x3D) { if (j + 1 >= e) { bad = 1; break; } raw[k++] = b->rbuf[++j] | 0x10; }
            else raw[k++] = b->rbuf[j];
        }
        i = e + 1;
        if (bad || k < 7) continue;
        int len = (int)((uint32_t)raw[2] << 24 | raw[3] << 16 | raw[4] << 8 | raw[5]);
        if (len != k - 7) continue;
        unsigned sum = 0; for (int j = 0; j < k - 1; j++) sum += raw[j];
        if ((uint8_t)sum != raw[k - 1]) { bud_log(b, "sony: bad checksum"); continue; }
        int type = raw[0], seq = raw[1];
        if (type == SY_ACK) {
            sp->seq = seq;                                    /* our next command uses the ACK's seq */
            if (sp->await) { sp->await = 0; sp->qi++; }
            sy_pump(b);
        } else if (type == SY_CMD || type == SY_CMD2) {
            sy_tx(b, SY_ACK, 1 - (seq & 1), 0, 0);
            if (type == SY_CMD) sy_payload(b, raw + 6, len);
        }
    }
    if (i > 0) { if (i > b->rn) i = b->rn; memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
}
static void sony_tick(struct bud *b, double now)
{
    struct sy_priv *p = syp(b);
    if (p->await && now - p->t_sent > 1250) {
        if (p->retries < 2) { p->retries++; p->t_sent = now; sy_tx(b, p->qt[p->qi], p->seq, p->q[p->qi], p->ql[p->qi]); }
        else { bud_log(b, "sony: command %02x unanswered, skipped", p->q[p->qi][0]); p->await = 0; p->qi++; sy_pump(b); }
    }
}
/* setting payloads (used only when sony_writes; checked by the PC tests) */
static int sy_asm_set(int dialect, int wind, int mode, int voice, int level, uint8_t *o)
{
    if (dialect == 2) {
        int k = 0; o[k++] = 0x68; o[k++] = wind ? 0x17 : 0x15; o[k++] = 0x01; o[k++] = mode != BN_OFF; o[k++] = mode == BN_AMBIENT;
        if (wind) o[k++] = 0x02;
        o[k++] = (uint8_t)voice; o[k++] = (uint8_t)level;
        return k;
    }
    o[0] = 0x68; o[1] = 0x02; o[2] = mode == BN_OFF ? 0x00 : 0x11; o[3] = wind ? 0x02 : 0x00;
    o[4] = mode == BN_ANC ? (wind ? 0x02 : 0x01) : 0x00; o[5] = 0x01; o[6] = (uint8_t)voice; o[7] = (uint8_t)level;
    return 8;
}
static int sony_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; struct sy_priv *p = syp(b); uint8_t pl[12]; int n;
    if (!sony_writes) return BE_NOTSUP;
    int wind = b->m && (b->m->layout & SYL_WIND);
    switch (c->op) {
    case BCMD_NOISE: case BCMD_AMBLVL: {
        if (!(s->caps & BC_NOISE)) return BE_NOTSUP;
        int mode = c->op == BCMD_NOISE ? c->a : BN_AMBIENT, lvl = c->op == BCMD_AMBLVL ? c->a : (s->amb_lvl < 0 ? 10 : s->amb_lvl);
        if (mode < 0 || mode >= BN_N || !(s->noise_mask & BUDS_MASK(mode)) || lvl < 0 || lvl > 20) return BE_RANGE;
        n = sy_asm_set(p->dialect, wind, mode, s->conv > 0, lvl, pl);
        sy_queue(b, SY_CMD, pl, n); sy_pump(b);
        s->noise = mode; s->amb_lvl = lvl; b->changed = 1;
        return BE_OK;
    }
    case BCMD_EQ:
        if (c->a < 0 || c->a >= 10) return BE_RANGE;
        pl[0] = 0x58; pl[1] = p->dialect == 2 ? 0x00 : 0x01; pl[2] = SY_EQ[c->a]; pl[3] = 0;
        sy_queue(b, SY_CMD, pl, 4); sy_pump(b); s->eq = c->a; b->changed = 1;
        return BE_OK;
    default: return BE_NOTSUP;
    }
}
static void sony_fill(struct bud *b) { b->s.eq_set = ES_SONY; if (b->s.caps & BC_EQ) { b->s.caps |= BC_BASS; b->s.bass_max = 20; } if (sony_writes) b->s.caps &= ~BC_RO; }
static const struct buds_brand BRAND_SONY = { BB_SONY, "sony", sony_identify, BT_RFCOMM, 0, sony_uuid, sony_start, sony_rx, sony_cmd, sony_tick, sony_fill, 0, 0 };
#endif
