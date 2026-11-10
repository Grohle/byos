/* buds_apple.inc.c - AirPods / Beats over AAP (Apple Accessory Protocol), brand module of
 * hibybudsd. Pure protocol code: bytes in, bytes out through struct bud (buds_core.inc.c).
 *
 * Written from our own notes (docs/HEADPHONES.md section 3, docs/research_codecs_earbuds.md
 * section 3.2), not from any GPL implementation. Facts marked [I] there must be confirmed with
 * captures of the owner's AirPods.
 *
 * Transport: L2CAP connection-oriented channel, PSM 0x1001, on the ACL that A2DP already holds
 * (SOCK_SEQPACKET: one read = one AAP packet).
 * Packet: "04 00 04 00" + opcode (LE16) + data. The host first sends a hello, then asks for
 * notifications; the buds then push battery, ear state, settings and metadata. */
#ifndef BUDS_APPLE_INC_C
#define BUDS_APPLE_INC_C
#include "buds_core.inc.c"

#define AAP_PSM 0x1001
enum {
    AAP_OP_BATTERY = 0x0004, AAP_OP_EAR = 0x0006, AAP_OP_CONTROL = 0x0009, AAP_OP_NOTIFY = 0x000F,
    AAP_OP_METADATA = 0x001D, AAP_OP_RENAME = 0x001E, AAP_OP_CA_LEVEL = 0x004B, AAP_OP_FEATURES = 0x004D,
};
/* settings carried by opcode 0x0009 (1-byte id + 4 data bytes) */
enum {
    AAP_CTL_MIC = 0x01, AAP_CTL_EARDET = 0x0A, AAP_CTL_LISTEN = 0x0D, AAP_CTL_CYCLE = 0x1A,
    AAP_CTL_CONV = 0x28, AAP_CTL_ADAPT = 0x2E,
};
/* listening mode values (setting 0x0D) and bits of the press-and-hold cycle (setting 0x1A) [I] */
static const uint8_t AAP_MODE[BN_N] = { 0x01, 0x02, 0x03, 0x04 };   /* off, ANC, transparency, adaptive */
static const uint8_t AAP_CYCLE_BIT[BN_N] = { 0x01, 0x02, 0x04, 0x08 };

#define AP_BAT (BC_BATT_LR | BC_BATT_CASE | BC_WEAR | BC_EARDET | BC_FW)
#define AP_NC3 (BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC) | BUDS_MASK(BN_AMBIENT))
#define AP_NC4 (AP_NC3 | BUDS_MASK(BN_ADAPTIVE))
#define AP_PRO2 (AP_BAT | BC_NOISE | BC_CYCLE | BC_CONV | BC_ADAPT_LVL | BC_SPOOF)
/* product ids: DID / BLE proximity code (docs/research_codecs_earbuds.md 3.2) [I] */
static const struct buds_model APPLE_MODELS[] = {
    { BB_APPLE, 0x2002, "AirPods", 0, BF_PODS, AP_BAT, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x200F, "AirPods 2", 0, BF_PODS, AP_BAT, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2013, "AirPods 3", 0, BF_PODS, AP_BAT, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2019, "AirPods 4", 0, BF_PODS, AP_BAT, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x201B, "AirPods 4 (ANC)", 0, BF_PODS, AP_PRO2, AP_NC4, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x200E, "AirPods Pro", 0, BF_PODS_PRO, AP_BAT | BC_NOISE | BC_CYCLE | BC_SPOOF, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2014, "AirPods Pro 2", 0, BF_PODS_PRO, AP_PRO2, AP_NC4, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2024, "AirPods Pro 2 (USB-C)", 0, BF_PODS_PRO, AP_PRO2, AP_NC4, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2027, "AirPods Pro 3", 0, BF_PODS_PRO, AP_PRO2, AP_NC4, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x200A, "AirPods Max", 0, BF_PODS_MAX, BC_BATT_ONE | BC_WEAR | BC_EARDET | BC_FW | BC_NOISE | BC_CYCLE | BC_SPOOF, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x201F, "AirPods Max (USB-C)", 0, BF_PODS_MAX, BC_BATT_ONE | BC_WEAR | BC_EARDET | BC_FW | BC_NOISE | BC_CYCLE | BC_SPOOF, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2003, "Powerbeats3", 0, BF_BEATS_EAR, BC_BATT_ONE | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2005, "BeatsX", 0, BF_BEATS_EAR, BC_BATT_ONE | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2006, "Beats Solo3", 0, BF_BEATS_HEAD, BC_BATT_ONE | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2009, "Beats Studio3", 0, BF_BEATS_HEAD, BC_BATT_ONE | BC_FW | BC_NOISE, BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC), 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x200B, "Powerbeats Pro", 0, BF_BEATS_EAR, BC_BATT_LR | BC_BATT_CASE | BC_WEAR | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x200C, "Beats Solo Pro", 0, BF_BEATS_HEAD, BC_BATT_ONE | BC_FW | BC_NOISE | BC_CYCLE, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2010, "Beats Flex", 0, BF_BEATS_EAR, BC_BATT_ONE | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2011, "Beats Studio Buds", 0, BF_BEATS_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_NOISE | BC_CYCLE, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2012, "Beats Fit Pro", 0, BF_BEATS_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_WEAR | BC_FW | BC_NOISE | BC_CYCLE, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2016, "Beats Studio Buds+", 0, BF_BEATS_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_NOISE | BC_CYCLE, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x2017, "Beats Studio Pro", 0, BF_BEATS_HEAD, BC_BATT_ONE | BC_FW | BC_NOISE | BC_CYCLE, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0x201D, "Powerbeats Pro 2", 0, BF_BEATS_EAR, BC_BATT_LR | BC_BATT_CASE | BC_WEAR | BC_FW | BC_NOISE | BC_CYCLE, AP_NC3, 0, 0, 0, {0}, 0 },
};
/* name fallbacks (no Modalias): conservative capabilities */
static const struct buds_model APPLE_GENERIC[] = {
    { BB_APPLE, 0, "AirPods Max", "AirPods Max", BF_PODS_MAX, BC_BATT_ONE | BC_WEAR | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0, "AirPods Pro", "AirPods Pro", BF_PODS_PRO, AP_BAT | BC_NOISE, AP_NC3, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0, "AirPods", "AirPods", BF_PODS, AP_BAT, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0, "Beats", "Beats", BF_BEATS_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_BATT_ONE | BC_FW, 0, 0, 0, 0, {0}, 0 },
    { BB_APPLE, 0, "Beats", "Powerbeats", BF_BEATS_EAR, BC_BATT_LR | BC_BATT_CASE | BC_BATT_ONE | BC_FW, 0, 0, 0, 0, {0}, 0 },
};
/* model numbers printed on the buds ("A2084") -> product id, to refine a name-only match [I] */
static const struct { const char *a; uint16_t id; } APPLE_ANUM[] = {
    { "A1523", 0x2002 }, { "A1722", 0x2002 }, { "A2031", 0x200F }, { "A2032", 0x200F },
    { "A2564", 0x2013 }, { "A2565", 0x2013 }, { "A3050", 0x2019 }, { "A3053", 0x2019 }, { "A3054", 0x2019 },
    { "A3055", 0x201B }, { "A3056", 0x201B }, { "A3057", 0x201B },
    { "A2083", 0x200E }, { "A2084", 0x200E }, { "A2698", 0x2014 }, { "A2699", 0x2014 }, { "A2931", 0x2014 },
    { "A3047", 0x2024 }, { "A3048", 0x2024 }, { "A3049", 0x2024 }, { "A2096", 0x200A }, { "A3184", 0x201F },
};
static const struct buds_model *aap_model_by_id(unsigned id)
{
    for (unsigned i = 0; i < sizeof APPLE_MODELS / sizeof *APPLE_MODELS; i++) if (APPLE_MODELS[i].id == id) return &APPLE_MODELS[i];
    return 0;
}
static const struct buds_model *aap_identify(const struct bd_dev *d)
{
    unsigned v, p;
    if (bd_modalias(d->modalias, &v, &p) && v == 0x004C) {
        const struct buds_model *m = aap_model_by_id(p);
        if (m) return m;
    }
    const char *nm = d->name[0] ? d->name : d->alias;
    for (unsigned i = 0; i < sizeof APPLE_GENERIC / sizeof *APPLE_GENERIC; i++) if (strstr(nm, APPLE_GENERIC[i].match)) return &APPLE_GENERIC[i];
    if (bd_modalias(d->modalias, &v, &p) && v == 0x004C && bd_audio(d)) return &APPLE_GENERIC[2];
    return 0;
}

/* ------------------------------------------------------------------ packets */
static const uint8_t AAP_HELLO[16] = { 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t AAP_FEATURES[14] = { 0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF, 0x00, 0, 0, 0, 0, 0, 0 };
static const uint8_t AAP_NOTIFY[10] = { 0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
/* settings packet: 04 00 04 00 09 00 ID v0 v1 v2 v3 (11 bytes) */
static int aap_control(uint8_t *out, int id, int v0, int v1, int v2, int v3)
{
    static const uint8_t h[6] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00 };
    memcpy(out, h, 6);
    out[6] = (uint8_t)id; out[7] = (uint8_t)v0; out[8] = (uint8_t)v1; out[9] = (uint8_t)v2; out[10] = (uint8_t)v3;
    return 11;
}
static int aap_send_control(struct bud *b, int id, int v0)
{
    uint8_t p[11]; int n = aap_control(p, id, v0, 0, 0, 0);
    return b->tx && b->tx(b, p, n) == n ? BE_OK : BE_IO;
}
static void aap_start(struct bud *b)
{
    b->tx(b, AAP_HELLO, sizeof AAP_HELLO);
    /* "host capabilities": unlocks conversational awareness while playing and adaptive mode on
     * Pro 2-class models; harmless on the others [I] */
    if (b->m && (b->m->noise_mask & BUDS_MASK(BN_ADAPTIVE))) b->tx(b, AAP_FEATURES, sizeof AAP_FEATURES);
    b->tx(b, AAP_NOTIFY, sizeof AAP_NOTIFY);
}

static int aap_wear(int v) { return v == 0 ? BW_IN : v == 1 ? BW_OUT : v == 2 ? BW_CASE : -1; }
static int aap_is_ver(const char *s)
{
    int n = (int)strlen(s), dig = 0, alpha = 0;
    if (n < 3 || n > 12) return 0;
    for (int i = 0; i < n; i++) { if (s[i] >= '0' && s[i] <= '9') dig++; else if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '.') alpha++; else return 0; }
    return dig && dig + alpha == n;
}
/* metadata: NUL-separated strings (name, model number, manufacturer, serial, firmware...) after
 * a few binary bytes; parsed by shape, not by position, so extra fields do not break it */
static void aap_metadata(struct bud *b, const uint8_t *p, int n)
{
    char str[12][48]; int ns = 0, k = 0; char cur[48];
    for (int i = 0; i <= n && ns < 12; i++) {
        int c = i < n ? p[i] : 0;
        if (c >= 32 && c < 127) { if (k < 47) cur[k++] = (char)c; }
        else if (c >= 0xC0 && i < n) { if (k < 46) cur[k++] = (char)c; }        /* UTF-8 lead/continuation */
        else if (c >= 0x80 && c < 0xC0) { if (k > 0 && k < 47) cur[k++] = (char)c; }
        else { if (k >= 2) { cur[k] = 0; memcpy(str[ns++], cur, (size_t)k + 1); } k = 0; }
    }
    struct buds_st *s = &b->s; int manu = -1;
    for (int i = 0; i < ns; i++) {
        const char *t = str[i];
        if (strlen(t) == 5 && t[0] == 'A' && t[1] >= '0' && t[1] <= '9' && t[4] >= '0' && t[4] <= '9') { buds_cpy(s->modelno, sizeof s->modelno, t); continue; }
        if (strstr(t, "Apple") || strstr(t, "Beats")) { if (manu < 0 && i > 0) { manu = i; continue; } }
    }
    if (ns > 0 && !s->name[0]) buds_cpy(s->name, sizeof s->name, str[0]);
    if (manu > 0) {
        if (manu + 1 < ns && strlen(str[manu + 1]) >= 8) buds_cpy(s->serial, sizeof s->serial, str[manu + 1]);
        for (int i = manu + 2; i < ns; i++) if (aap_is_ver(str[i])) { buds_cpy(s->fw, sizeof s->fw, str[i]); break; }
    } else for (int i = 1; i < ns; i++) if (aap_is_ver(str[i]) && strcmp(str[i], s->modelno)) { buds_cpy(s->fw, sizeof s->fw, str[i]); break; }
    /* refine a name-only identification with the model number */
    if (s->modelno[0] && (!b->m || !b->m->id))
        for (unsigned i = 0; i < sizeof APPLE_ANUM / sizeof *APPLE_ANUM; i++)
            if (!strcmp(APPLE_ANUM[i].a, s->modelno)) { const struct buds_model *m = aap_model_by_id(APPLE_ANUM[i].id); if (m) bud_set_model(b, m); break; }
    b->changed = 1;
}
static void aap_rx(struct bud *b, const uint8_t *p, int n)
{
    struct buds_st *s = &b->s;
    if (n >= 4 && p[0] == 0x01 && p[1] == 0x00 && p[2] == 0x04 && p[3] == 0x00) { bud_log(b, "aap: hello answered"); return; }
    if (n < 6 || p[0] != 0x04 || p[1] != 0x00 || p[2] != 0x04 || p[3] != 0x00) { bud_unknown(b, 0xFFFF, p, n); return; }
    unsigned op = p[4] | p[5] << 8;
    const uint8_t *d = p + 6; int dn = n - 6;
    switch (op) {
    case AAP_OP_BATTERY: {
        if (dn < 1) break;
        int cnt = d[0];
        for (int i = 0; i < cnt && 1 + i * 5 + 3 < dn; i++) {
            const uint8_t *e = d + 1 + i * 5;
            int comp = e[0], lvl = e[2], stt = e[3], idx;
            idx = comp == 0x04 ? 0 : comp == 0x02 ? 1 : comp == 0x08 ? 2 : comp == 0x01 ? 0 : -1;
            if (idx < 0) continue;
            if (stt == 0x04 || lvl > 100) { s->batt[idx] = -1; s->chg[idx] = -1; }      /* not connected */
            else { s->batt[idx] = lvl; s->chg[idx] = stt == 0x01; }
        }
        b->ready = 1; b->changed = 1;
        break;
    }
    case AAP_OP_EAR:
        if (dn >= 2) { s->wear[0] = aap_wear(d[0]); s->wear[1] = aap_wear(d[1]); b->changed = 1; }
        else if (dn == 1) { s->wear[0] = aap_wear(d[0]); b->changed = 1; }
        break;
    case AAP_OP_CONTROL: {
        if (dn < 2) break;
        int id = d[0], v = d[1];
        switch (id) {
        case AAP_CTL_LISTEN:
            for (int m = 0; m < BN_N; m++) if (AAP_MODE[m] == v) { s->noise = m; b->changed = 1; }
            break;
        case AAP_CTL_CONV: s->conv = v == 1; b->changed = 1; break;
        case AAP_CTL_ADAPT: if (v <= 100) { s->adapt_lvl = v; b->changed = 1; } break;
        case AAP_CTL_EARDET: s->eardet = v == 1; b->changed = 1; break;
        case AAP_CTL_CYCLE: { int c = 0; for (int m = 0; m < BN_N; m++) if (v & AAP_CYCLE_BIT[m]) c |= 1 << m; s->cycle = c; b->changed = 1; break; }
        default: bud_unknown(b, 0x0900 | (unsigned)id, d, dn); break;
        }
        break;
    }
    case AAP_OP_METADATA: aap_metadata(b, d, dn); break;
    case AAP_OP_CA_LEVEL: break;                     /* "user is speaking" level: not used yet */
    default: bud_unknown(b, op, p, n); break;
    }
    if (b->changed) s->conn = BS_READY;
}
static int aap_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; int r;
    switch (c->op) {
    case BCMD_NOISE:
        if (!(s->caps & BC_NOISE)) return BE_NOTSUP;
        if (c->a < 0 || c->a >= BN_N || !(s->noise_mask & BUDS_MASK(c->a))) return BE_RANGE;
        if ((r = aap_send_control(b, AAP_CTL_LISTEN, AAP_MODE[c->a])) == BE_OK) { s->noise = c->a; b->changed = 1; }
        return r;
    case BCMD_ADAPTLVL:
        if (!(s->caps & BC_ADAPT_LVL)) return BE_NOTSUP;
        if (c->a < 0 || c->a > 100) return BE_RANGE;
        if ((r = aap_send_control(b, AAP_CTL_ADAPT, c->a)) == BE_OK) { s->adapt_lvl = c->a; b->changed = 1; }
        return r;
    case BCMD_CONV:
        if (!(s->caps & BC_CONV)) return BE_NOTSUP;
        if ((r = aap_send_control(b, AAP_CTL_CONV, c->a ? 1 : 2)) == BE_OK) { s->conv = !!c->a; b->changed = 1; }
        return r;
    case BCMD_EARDET:
        if (!(s->caps & BC_EARDET)) return BE_NOTSUP;
        if ((r = aap_send_control(b, AAP_CTL_EARDET, c->a ? 1 : 2)) == BE_OK) { s->eardet = !!c->a; b->changed = 1; }
        return r;
    case BCMD_CYCLE: {
        if (!(s->caps & BC_CYCLE)) return BE_NOTSUP;
        int v = 0, k = 0;
        for (int m = 0; m < BN_N; m++) if (c->a & (1 << m)) { if (!(s->noise_mask & BUDS_MASK(m))) return BE_RANGE; v |= AAP_CYCLE_BIT[m]; k++; }
        if (k < 2) return BE_RANGE;                      /* iOS: at least two modes */
        if ((r = aap_send_control(b, AAP_CTL_CYCLE, v)) == BE_OK) { s->cycle = c->a; b->changed = 1; }
        return r;
    }
    default: return BE_NOTSUP;
    }
}
static const struct buds_brand BRAND_APPLE = { BB_APPLE, "apple", aap_identify, BT_L2CAP, AAP_PSM, 0, aap_start, aap_rx, aap_cmd, 0, 0 };
#endif
