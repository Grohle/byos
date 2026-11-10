/* buds_huawei.inc.c - Huawei FreeBuds / FreeLace / FreeClip and Honor Earbuds over RFCOMM,
 * brand module of hibybudsd. Pure protocol code (bytes in, bytes out through struct bud).
 *
 * Facts from the public OpenFreebuds project (github.com/melianmiko/OpenFreebuds, GPL-3.0:
 * facts only, no code), in our own words in docs/HEADPHONES.md section 10.2.
 *   RFCOMM on a fixed channel per model (1 or 16), no SDP needed.
 *   Frame: 5A | length BE16 (= 1 + command + parameters) | 00 | command (2 bytes) |
 *          parameters as TLV {type, length, value} | CRC-16/XMODEM BE over everything before.
 *   A read sends the wanted parameter types with empty values; the buds answer with the same
 *   command and the values.
 *   01 07 device info (7 firmware, 9 serial, 15 model), 01 08 battery read / 01 27 notify
 *   (1 global, 2 = L R case, 3 charging), 2B 2A ANC read (1 = {level, mode}), 2B 04 ANC set,
 *   2B 03 events (in-ear 8/9, ANC changed by the buds' own control).
 * ANC writes: mode {mode, FF (keep level) or 00 for off}; level {current mode, level}. */
#ifndef BUDS_HUAWEI_INC_C
#define BUDS_HUAWEI_INC_C
#include "buds_core.inc.c"

/* layout bits: channel 16 (else 1), ANC modes, cancellation levels, dynamic level */
enum { HWL_CH16 = 1, HWL_ANC = 2, HWL_LVL = 4, HWL_DYN = 8 };
#define HW_TWS (BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_WEAR)
#define HW_ONE (BC_BATT_ONE | BC_FW)
#define HW_M3 (BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC) | BUDS_MASK(BN_AMBIENT))
static const struct buds_model HUAWEI_MODELS[] = {
    { BB_HUAWEI, 15, "HUAWEI FreeBuds Pro 4", "FreeBuds Pro 4", BF_PODS_PRO, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 14, "HUAWEI FreeBuds Pro 3", "FreeBuds Pro 3", BF_PODS_PRO, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 13, "HUAWEI FreeBuds Pro 2", "FreeBuds Pro 2", BF_PODS_PRO, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3, 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL },
    { BB_HUAWEI, 12, "HUAWEI FreeBuds Pro", "FreeBuds Pro", BF_PODS_PRO, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 11, "HUAWEI FreeBuds Studio", "FreeBuds Studio", BF_HEAD, HW_ONE | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 10, "HUAWEI FreeBuds 6i", "FreeBuds 6i", BF_PODS, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 9, "HUAWEI FreeBuds 5i", "FreeBuds 5i", BF_PODS, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_CH16 | HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 8, "HUAWEI FreeBuds 4i", "FreeBuds 4i", BF_PODS, HW_TWS | BC_NOISE, HW_M3, 0, 0, 0, { 0 }, HWL_CH16 | HWL_ANC },
    { BB_HUAWEI, 16, "HONOR Earbuds 2", "Earbuds 2", BF_PODS, HW_TWS | BC_NOISE, HW_M3, 0, 0, 0, { 0 }, HWL_CH16 | HWL_ANC },
    { BB_HUAWEI, 7, "HUAWEI FreeBuds SE 4 ANC", "FreeBuds SE 4", BF_PODS, HW_TWS | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_CH16 | HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 6, "HUAWEI FreeBuds SE 2", "FreeBuds SE 2", BF_PODS, HW_TWS, 0, 0, 0, 0, { 0 }, 0 },
    { BB_HUAWEI, 5, "HUAWEI FreeBuds SE", "FreeBuds SE", BF_PODS, HW_TWS, 0, 0, 0, 0, { 0 }, HWL_CH16 },
    { BB_HUAWEI, 4, "HUAWEI FreeBuds 3", "FreeBuds 3", BF_PODS, HW_TWS, 0, 0, 0, 0, { 0 }, 0 },
    { BB_HUAWEI, 3, "HUAWEI FreeClip 2", "FreeClip 2", BF_BUDS, HW_TWS, 0, 0, 0, 0, { 0 }, 0 },
    { BB_HUAWEI, 2, "HUAWEI FreeLace Pro 2", "FreeLace Pro 2", BF_BUDS, HW_ONE | BC_NOISE | BC_ANC_LVL, HW_M3 | BUDS_MASK(BN_ADAPTIVE), 0, 2, 0, { 0 }, HWL_ANC | HWL_LVL | HWL_DYN },
    { BB_HUAWEI, 1, "HUAWEI FreeLace Pro", "FreeLace Pro", BF_BUDS, HW_ONE | BC_NOISE | BC_ANC_LVL, HW_M3, 0, 2, 0, { 0 }, HWL_CH16 | HWL_ANC | HWL_LVL },
};
static const struct buds_model *hw_identify(const struct bd_dev *d)
{
    const char *nm = d->name[0] ? d->name : d->alias;
    unsigned v, p; int huawei_vendor = bd_modalias(d->modalias, &v, &p) && v == 0x027D;
    int named = strstr(nm, "HUAWEI") || strstr(nm, "FreeBuds") || strstr(nm, "FreeLace") || strstr(nm, "FreeClip") || strstr(nm, "HONOR Earbuds");
    if (!named && !huawei_vendor) return 0;
    for (unsigned i = 0; i < sizeof HUAWEI_MODELS / sizeof *HUAWEI_MODELS; i++) if (strstr(nm, HUAWEI_MODELS[i].match)) return &HUAWEI_MODELS[i];
    return 0;                                      /* unknown model: channel unknown, let Fast Pair / others try */
}
static const char *hw_uuid(const struct bud *b) { (void)b; return "00001101-0000-1000-8000-00805f9b34fb"; }
static int hw_channel(const struct bud *b) { return b->m && (b->m->layout & HWL_CH16) ? 16 : 1; }

/* frame: 5A len(BE16) 00 cmd TLVs crc(BE16); params = n triples (type, len, value*) */
struct hw_tlv { int type, len; const uint8_t *v; };
static int hw_frame(int c0, int c1, const struct hw_tlv *t, int nt, uint8_t *o, int cap)
{
    int body = 2; for (int i = 0; i < nt; i++) body += 2 + t[i].len;
    int tot = 4 + body + 2;
    if (tot > cap || body + 1 > 0xFFFF) return -1;
    o[0] = 0x5A; o[1] = (uint8_t)((body + 1) >> 8); o[2] = (uint8_t)(body + 1); o[3] = 0;
    o[4] = (uint8_t)c0; o[5] = (uint8_t)c1;
    int k = 6;
    for (int i = 0; i < nt; i++) { o[k++] = (uint8_t)t[i].type; o[k++] = (uint8_t)t[i].len; if (t[i].len) memcpy(o + k, t[i].v, (size_t)t[i].len); k += t[i].len; }
    uint16_t c = buds_crc16(o, k);
    o[k++] = (uint8_t)(c >> 8); o[k++] = (uint8_t)c;
    return k;
}
static int hw_send(struct bud *b, int c0, int c1, const struct hw_tlv *t, int nt)
{
    uint8_t f[128]; int k = hw_frame(c0, c1, t, nt, f, sizeof f);
    if (k < 0 || !b->tx) return BE_IO;
    return b->tx(b, f, k) == k ? BE_OK : BE_IO;
}
static int hw_read(struct bud *b, int c0, int c1, const int *types, int n)
{
    struct hw_tlv t[32]; for (int i = 0; i < n && i < 32; i++) { t[i].type = types[i]; t[i].len = 0; t[i].v = 0; }
    return hw_send(b, c0, c1, t, n < 32 ? n : 32);
}
static void hw_start(struct bud *b)
{
    static const int info[] = { 7, 9, 15 }, batt[] = { 1, 2, 3 }, anc[] = { 1, 2 };
    hw_read(b, 0x01, 0x07, info, 3);
    hw_read(b, 0x01, 0x08, batt, 3);
    if (b->m && (b->m->layout & HWL_ANC)) hw_read(b, 0x2B, 0x2A, anc, 2);
}
/* ANC level byte <-> ours: comfort 1 -> 0, normal 0 -> 1, ultra 2 -> 2; dynamic 3 = adaptive */
static int hw_lvl_ours(int v) { return v == 1 ? 0 : v == 0 ? 1 : v == 2 ? 2 : -1; }
static int hw_lvl_wire(int l) { return l <= 0 ? 1 : l == 1 ? 0 : 2; }
static void hw_msg(struct bud *b, int cmd, const struct hw_tlv *t, int nt)
{
    struct buds_st *s = &b->s;
    const struct hw_tlv *p1 = 0, *p2 = 0, *p3 = 0, *p7 = 0, *p8 = 0, *p9 = 0, *p15 = 0;
    for (int i = 0; i < nt; i++) switch (t[i].type) {
        case 1: p1 = &t[i]; break; case 2: p2 = &t[i]; break; case 3: p3 = &t[i]; break; case 7: p7 = &t[i]; break;
        case 8: p8 = &t[i]; break; case 9: p9 = &t[i]; break; case 15: p15 = &t[i]; break;
    }
    switch (cmd) {
    case 0x0108: case 0x0127:
        if (p2 && p2->len == 3 && (s->caps & BC_BATT_LR)) for (int k = 0; k < 3; k++) s->batt[k] = p2->v[k] <= 100 ? p2->v[k] : -1;
        else if (p1 && p1->len == 1) s->batt[0] = p1->v[0] <= 100 ? p1->v[0] : -1;
        if (p3 && p3->len >= 1) {
            if (p3->len == 3) for (int k = 0; k < 3; k++) s->chg[k] = p3->v[k] == 1;
            else { int any = 0; for (int k = 0; k < p3->len; k++) any |= p3->v[k] == 1; s->chg[0] = any; }
        }
        b->ready = 1; b->changed = 1;
        break;
    case 0x2B2A:
        if (p1 && p1->len == 2) {
            int lvl = p1->v[0], mode = p1->v[1];
            if (mode == 0) s->noise = BN_OFF;
            else if (mode == 2) s->noise = BN_AMBIENT;
            else if (mode == 1) { if (lvl == 3 && (s->noise_mask & BUDS_MASK(BN_ADAPTIVE))) s->noise = BN_ADAPTIVE; else { s->noise = BN_ANC; if (hw_lvl_ours(lvl) >= 0) s->anc_lvl = hw_lvl_ours(lvl); } }
            b->changed = 1;
        }
        break;
    case 0x2B03: {
        const struct hw_tlv *w = p8 ? p8 : p9;
        if (w && w->len == 1 && (s->caps & BC_WEAR)) { s->wear[0] = s->wear[1] = w->v[0] == 1 ? BW_IN : BW_OUT; b->changed = 1; }
        if (p1 && p1->len == 1 && p1->v[0] <= 2 && b->m && (b->m->layout & HWL_ANC)) { static const int anc[] = { 1, 2 }; hw_read(b, 0x2B, 0x2A, anc, 2); }
        break;
    }
    case 0x0107: {
        char v[40];
        if (p7 && p7->len > 0) { int n = p7->len < 39 ? p7->len : 39; memcpy(v, p7->v, (size_t)n); v[n] = 0; buds_clean(s->fw, sizeof s->fw, v); }
        if (p9 && p9->len > 0) { int n = p9->len < 23 ? p9->len : 23; memcpy(v, p9->v, (size_t)n); v[n] = 0; buds_clean(s->serial, sizeof s->serial, v); }
        if (p15 && p15->len > 0) { int n = p15->len < 15 ? p15->len : 15; memcpy(v, p15->v, (size_t)n); v[n] = 0; buds_clean(s->modelno, sizeof s->modelno, v); }
        b->changed = 1;
        break;
    }
    case 0x2B04: break;                            /* write acknowledged */
    default: { uint8_t raw[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd }; bud_unknown(b, (unsigned)cmd, raw, 2); break; }
    }
    if (b->changed) s->conn = BS_READY;
}
static void hw_rx(struct bud *b, const uint8_t *p, int n)
{
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    while (i < b->rn) {
        const uint8_t *f = b->rbuf + i; int left = b->rn - i;
        if (f[0] != 0x5A) { i++; continue; }
        if (left < 6) break;
        int len = f[1] << 8 | f[2], tot = 3 + len + 2;
        if (f[3] != 0 || len < 3 || tot > (int)sizeof b->rbuf) { i++; continue; }
        if (left < tot) break;
        uint16_t c = buds_crc16(f, tot - 2);
        if (f[tot - 2] != (c >> 8) || f[tot - 1] != (c & 0xFF)) { bud_log(b, "huawei: bad CRC"); i++; continue; }
        struct hw_tlv t[40]; int nt = 0, k = 6, end = 3 + len, ok = 1;
        while (k < end) {
            if (k + 2 > end) { ok = 0; break; }
            int ty = f[k], l = f[k + 1];
            if (k + 2 + l > end) { ok = 0; break; }
            if (nt < 40) { t[nt].type = ty; t[nt].len = l; t[nt].v = f + k + 2; nt++; }
            k += 2 + l;
        }
        if (ok) hw_msg(b, f[4] << 8 | f[5], t, nt); else bud_log(b, "huawei: bad parameters");
        i += tot;
    }
    if (i > 0) { memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
}
static int hw_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; uint8_t v[2]; struct hw_tlv t = { 1, 2, v };
    if (c->op != BCMD_NOISE && c->op != BCMD_ANCLVL) return BE_NOTSUP;
    if (!(s->caps & BC_NOISE)) return BE_NOTSUP;
    if (c->op == BCMD_NOISE) {
        if (c->a < 0 || c->a >= BN_N || !(s->noise_mask & BUDS_MASK(c->a))) return BE_RANGE;
        if (c->a == BN_ADAPTIVE) { v[0] = 1; v[1] = 3; }                 /* cancellation, dynamic level */
        else { v[0] = (uint8_t)(c->a == BN_ANC ? 1 : c->a == BN_AMBIENT ? 2 : 0); v[1] = c->a == BN_OFF ? 0 : 0xFF; }
    } else {
        if (!(s->caps & BC_ANC_LVL) || c->a < 0 || c->a > 2) return BE_RANGE;
        v[0] = 1; v[1] = (uint8_t)hw_lvl_wire(c->a);
    }
    int r = hw_send(b, 0x2B, 0x04, &t, 1);
    if (r == BE_OK) {
        if (c->op == BCMD_NOISE) s->noise = c->a; else { s->noise = BN_ANC; s->anc_lvl = c->a; }
        b->changed = 1;
        static const int anc[] = { 1, 2 }; hw_read(b, 0x2B, 0x2A, anc, 2);     /* read back the real state */
    }
    return r;
}
static const struct buds_brand BRAND_HUAWEI = { BB_HUAWEI, "huawei", hw_identify, BT_RFCOMM, 0, hw_uuid, hw_start, hw_rx, hw_cmd, 0, 0, 0, hw_channel };
#endif
