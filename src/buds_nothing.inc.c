/* buds_nothing.inc.c - Nothing and CMF headphones over RFCOMM, brand module of hibybudsd.
 * Pure protocol code: bytes in, bytes out through struct bud (buds_core.inc.c).
 *
 * Written from docs/research_headphones_nothing_sony_jbl.md section 2 (our own spec, facts
 * only; Gadgetbridge / ear (web) are AGPL and none of their code is used).
 *
 * Service UUID aeac4a03-dff5-498f-843a-34487cf133eb (RFCOMM channel from SDP).
 * Frame (all little-endian):
 *   55 | control u16 | command u16 | payload length u16 | sequence u8 | payload | CRC u16
 *   the CRC (CRC-16/MODBUS over everything before it) is present when control & 0x0020.
 * Commands: 0xCxxx host get -> 0x4xxx reply; 0xFxxx host set -> 0x7xxx echo; 0xExxx push.
 * One get in flight at a time during the initial read-out; replies matched by number. */
#ifndef BUDS_NOTHING_INC_C
#define BUDS_NOTHING_INC_C
#include "buds_core.inc.c"

#define NT_UUID "aeac4a03-dff5-498f-843a-34487cf133eb"
/* CRC-16/MODBUS: poly 0x8005 reflected (0xA001), init 0xFFFF, no final xor */
static uint16_t nt_crc16(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    while (n-- > 0) { c ^= *p++; for (int i = 0; i < 8; i++) c = (uint16_t)(c & 1 ? (c >> 1) ^ 0xA001 : c >> 1); }
    return c;
}
/* ANC capability bits (per model) and the wire values of the noise modes */
enum { NA_HIGH = 1, NA_MID = 2, NA_LOW = 4, NA_ADAPT = 8, NA_TRANSP = 16, NA_OFF = 32 };
enum { NW_HIGH = 0x01, NW_MID = 0x02, NW_LOW = 0x03, NW_ADAPT = 0x04, NW_OFF = 0x05, NW_TRANSP = 0x07 };
/* extra description per model */
struct nt_model {
    struct buds_model m;
    uint16_t ctl;          /* control word of host frames: 0x0160 (CRC + counter + TWS) or 0x0120 [D per model] */
    uint8_t anc;           /* NA_* */
    uint8_t cmf_eq;        /* 1: "listening modes" (C050/F01D), 0: Ear presets (C01F/F010) */
    uint8_t find;          /* 0 per side {02|03, on}, 1 Ear (1) {on}, 2 over-ear {06, on} */
    uint8_t bass3;         /* Ultra Bass in 3 steps (wire 0/5/10) instead of 1..5 (x2) */
    uint8_t adv_eq;        /* 8-band advanced EQ */
};
#define NT_G_TAPS ((1u << BA_NONE) | (1u << BA_PLAYPAUSE) | (1u << BA_NEXT) | (1u << BA_PREV) | (1u << BA_VOICE) | (1u << BA_VOLUP) | (1u << BA_VOLDN))
#define NT_G_HOLD ((1u << BA_NONE) | (1u << BA_NOISE) | (1u << BA_ANC) | (1u << BA_AMBIENT) | (1u << BA_VOICE))
#define NT_GO { 0, NT_G_TAPS, NT_G_TAPS, NT_G_HOLD }
#define NT_B (BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_EQ | BC_GEST | BC_GAME | BC_FIND)
#define NT_ALL6 (NA_HIGH | NA_MID | NA_LOW | NA_ADAPT | NA_TRANSP | NA_OFF)
/* noise_mask / anc_max / eq_n are derived from anc and the EQ line in nt_fill() */
static const struct nt_model NOTHING_MODELS[] = {
    { { BB_NOTHING, 0xB172, "CMF Buds Pro 2", "CMF Buds Pro 2", BF_BUDS, NT_B | BC_EARDET | BC_EQ_BANDS | BC_BASS | BC_FIT, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 1, 0, 0, 0 },
    { { BB_NOTHING, 0xB163, "CMF Buds Pro", "CMF Buds Pro", BF_BUDS, NT_B | BC_EARDET, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NA_HIGH | NA_MID | NA_LOW | NA_TRANSP | NA_OFF, 0, 0, 0, 0 },
    { { BB_NOTHING, 0xB184, "CMF Buds 2 Plus", "CMF Buds 2 Plus", BF_BUDS, NT_B | BC_EARDET | BC_EQ_BANDS | BC_BASS, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 1, 0, 0, 0 },
    { { BB_NOTHING, 0xB185, "CMF Buds 2a", "CMF Buds 2a", BF_BUDS, NT_B | BC_BASS, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NA_HIGH | NA_TRANSP | NA_OFF, 1, 0, 0, 0 },
    { { BB_NOTHING, 0xB179, "CMF Buds 2", "CMF Buds 2", BF_BUDS, NT_B | BC_EARDET | BC_EQ_BANDS | BC_BASS, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 1, 0, 0, 0 },
    { { BB_NOTHING, 0xB168, "CMF Buds", "CMF Buds", BF_BUDS, NT_B | BC_EARDET | BC_BASS, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NA_HIGH | NA_TRANSP | NA_OFF, 1, 0, 0, 0 },
    { { BB_NOTHING, 0xB157, "Nothing Ear (stick)", "Ear (stick)", BF_NOTHING, BC_BATT_LR | BC_BATT_CASE | BC_FW | BC_EQ | BC_EARDET | BC_GEST, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, 0, 0, 0, 0, 1 },
    { { BB_NOTHING, 0xB174, "Nothing Ear (open)", "Ear (open)", BF_BUDS, NT_B | BC_EQ_BANDS, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, 0, 0, 0, 0, 1 },
    { { BB_NOTHING, 0xB181, "Nothing Ear (1)", "Ear (1)", BF_NOTHING, NT_B | BC_EARDET, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NA_HIGH | NA_LOW | NA_TRANSP | NA_OFF, 0, 1, 0, 0 },
    { { BB_NOTHING, 0xB155, "Nothing Ear (2)", "Ear (2)", BF_NOTHING, NT_B | BC_EARDET | BC_EQ_BANDS | BC_FIT, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 0, 0, 0, 1 },
    { { BB_NOTHING, 0xB190, "Nothing Ear (3a)", "Ear (3a)", BF_NOTHING, NT_B | BC_EARDET | BC_EQ_BANDS | BC_FIT, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 0, 0, 0, 1 },
    { { BB_NOTHING, 0xB173, "Nothing Ear (3)", "Ear (3)", BF_NOTHING, NT_B | BC_EARDET | BC_EQ_BANDS | BC_BASS | BC_FIT, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 0, 0, 0, 1 },
    { { BB_NOTHING, 0xB162, "Nothing Ear (a)", "Ear (a)", BF_NOTHING, NT_B | BC_EARDET | BC_EQ_BANDS | BC_BASS | BC_FIT, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 0, 0, 0, 0 },
    { { BB_NOTHING, 0xB170, "Nothing Headphone (1)", "Headphone (1)", BF_HEAD, BC_BATT_ONE | BC_FW | BC_EQ | BC_GAME | BC_FIND | BC_EARDET | BC_EQ_BANDS | BC_BASS, 0, 0, 0, 0, { 0 }, 0 }, 0x0160, NT_ALL6, 0, 2, 0, 1 },
    { { BB_NOTHING, 0xB186, "Nothing Headphone (a)", "Headphone (a)", BF_HEAD, BC_BATT_ONE | BC_FW | BC_EQ | BC_GAME | BC_FIND | BC_EQ_BANDS | BC_BASS, 0, 0, 0, 0, { 0 }, 0 }, 0x0160, NT_ALL6, 0, 2, 1, 1 },
    { { BB_NOTHING, 0xB171, "Nothing Ear", "Nothing Ear", BF_NOTHING, NT_B | BC_EARDET | BC_EQ_BANDS | BC_BASS | BC_FIT, 0, 0, 0, 0, NT_GO, 0 }, 0x0160, NT_ALL6, 0, 0, 0, 1 },
};
/* UUID present, name unknown: what every model has */
static const struct nt_model NOTHING_GENERIC = { { BB_NOTHING, 0, "Nothing", 0, BF_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_FW, 0, 0, 0, 0, { 0 }, 0 }, 0x0160, 0, 0, 0, 0, 0 };

static const struct nt_model *nt_x(const struct bud *b)
{
    for (unsigned i = 0; i < sizeof NOTHING_MODELS / sizeof *NOTHING_MODELS; i++) if (b->m == &NOTHING_MODELS[i].m) return &NOTHING_MODELS[i];
    return &NOTHING_GENERIC;
}
static const struct buds_model *nt_identify(const struct bd_dev *d)
{
    const char *nm = d->name[0] ? d->name : d->alias;
    int uuid = bd_has_uuid(d, NT_UUID), named = !strncmp(nm, "Nothing", 7) || !strncmp(nm, "CMF", 3);
    if (!uuid && !named) return 0;
    for (unsigned i = 0; i < sizeof NOTHING_MODELS / sizeof *NOTHING_MODELS; i++) if (strstr(nm, NOTHING_MODELS[i].m.match)) return &NOTHING_MODELS[i].m;
    return uuid ? &NOTHING_GENERIC.m : 0;
}
static const char *nt_uuid(const struct bud *b) { (void)b; return NT_UUID; }

/* ANC levels of the model, low to high (wire values) */
static int nt_levels(const struct nt_model *x, int *w)
{
    int n = 0;
    if (x->anc & NA_LOW) w[n++] = NW_LOW;
    if (x->anc & NA_MID) w[n++] = NW_MID;
    if (x->anc & NA_HIGH) w[n++] = NW_HIGH;
    return n;
}
static const uint8_t NT_EQ_EAR[5] = { 0, 1, 2, 3, 5 };            /* Balanced, Voice, More treble, More bass, Custom */
static const uint8_t NT_EQ_CMF[7] = { 1, 2, 3, 4, 5, 6, 7 };      /* Rock ... Custom, Balanced (Dirac) */
/* fill the published capabilities from the extra description */
static void nt_fill(struct bud *b)
{
    const struct nt_model *x = nt_x(b); struct buds_st *s = &b->s; int w[3];
    int nl = nt_levels(x, w);
    s->noise_mask = 0;
    if (nl) s->noise_mask |= BUDS_MASK(BN_ANC);
    if (x->anc & NA_TRANSP) s->noise_mask |= BUDS_MASK(BN_AMBIENT);
    if (x->anc & NA_ADAPT) s->noise_mask |= BUDS_MASK(BN_ADAPTIVE);
    if (x->anc & NA_OFF) s->noise_mask |= BUDS_MASK(BN_OFF);
    if (s->noise_mask) s->caps |= BC_NOISE;
    if (nl > 1) { s->caps |= BC_ANC_LVL; s->anc_max = nl - 1; }
    s->eq_set = x->cmf_eq ? ES_CMF : ES_NOTHING; s->eq_n = x->cmf_eq ? 7 : 5;
    if (s->caps & BC_BASS) s->bass_max = x->bass3 ? 2 : 5;
    s->band_min = -6; s->band_max = 6;
}
/* private state */
struct nt_priv { uint8_t seq; uint16_t ctl; int q[16], qn, qi; uint16_t inflight; double t_sent, t_batt; int retries, ctl_switched, got_any; };
static struct nt_priv *ntp(struct bud *b) { return (struct nt_priv *)b->priv.raw; }

/* frame builder: returns length or -1 */
static int nt_frame(uint16_t ctl, uint16_t cmd, uint8_t seq, const uint8_t *pl, int n, uint8_t *o, int cap)
{
    int tot = 8 + n + (ctl & 0x20 ? 2 : 0);
    if (n < 0 || n > 512 || tot > cap) return -1;
    o[0] = 0x55; o[1] = (uint8_t)ctl; o[2] = (uint8_t)(ctl >> 8); o[3] = (uint8_t)cmd; o[4] = (uint8_t)(cmd >> 8);
    o[5] = (uint8_t)n; o[6] = (uint8_t)(n >> 8); o[7] = seq;
    if (n) memcpy(o + 8, pl, (size_t)n);
    if (ctl & 0x20) { uint16_t c = nt_crc16(o, 8 + n); o[8 + n] = (uint8_t)c; o[9 + n] = (uint8_t)(c >> 8); }
    return tot;
}
static int nt_send(struct bud *b, uint16_t cmd, const uint8_t *pl, int n)
{
    struct nt_priv *p = ntp(b); uint8_t f[600];
    int k = nt_frame(p->ctl ? p->ctl : nt_x(b)->ctl, cmd, ++p->seq, pl, n, f, sizeof f);
    if (k < 0 || !b->tx) return BE_IO;
    return b->tx(b, f, k) == k ? BE_OK : BE_IO;
}
/* initial read-out, one get in flight */
static void nt_next(struct bud *b, double now)
{
    struct nt_priv *p = ntp(b);
    if (p->inflight || p->qi >= p->qn) return;
    p->inflight = (uint16_t)p->q[p->qi++]; p->t_sent = now; p->retries = 0;
    nt_send(b, p->inflight, 0, 0);
}
static void nt_start(struct bud *b)
{
    struct nt_priv *p = ntp(b); const struct nt_model *x = nt_x(b); unsigned c = b->s.caps;
    memset(p, 0, sizeof *p); p->ctl = x->ctl;
    nt_fill(b);
    int *q = p->q, n = 0;
    q[n++] = 0xC042; q[n++] = 0xC007;
    if (x->anc) q[n++] = 0xC01E;
    if (c & BC_EQ) q[n++] = x->cmf_eq ? 0xC050 : 0xC01F;
    if (c & BC_EQ_BANDS) q[n++] = 0xC044;
    if (c & BC_BASS) q[n++] = 0xC04E;
    if (c & BC_EARDET) q[n++] = 0xC00E;
    if (c & BC_GAME) q[n++] = 0xC041;
    if (c & BC_GEST) q[n++] = 0xC018;
    p->qn = n;
    nt_next(b, b->now);
}
static float nt_f32(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }     /* LE hosts (MIPSel, x86) */
static void nt_put_f32(uint8_t *o, float f) { memcpy(o, &f, 4); }
static int nt_action(int code)
{
    switch (code) {
    case 0x01: return BA_NONE; case 0x02: return BA_PLAYPAUSE; case 0x08: return BA_PREV; case 0x09: return BA_NEXT;
    case 0x0A: return BA_NOISE; case 0x0B: return BA_VOICE; case 0x12: return BA_VOLUP; case 0x13: return BA_VOLDN;
    case 0x14: return BA_ANC; case 0x15: return BA_AMBIENT; case 0x17: return BA_VOLUME; default: return -1;
    }
}
static int nt_action_code(int a)
{
    static const int8_t c[BA_N] = { 0x01, 0x02, 0x09, 0x08, 0x0B, 0x0A, 0x15, 0x14, 0x17, -1, 0x12, 0x13 };
    return a >= 0 && a < BA_N ? c[a] : -1;
}
static const uint8_t NT_GESTURE[BG_N] = { 0x01, 0x02, 0x03, 0x07 };
static void nt_noise_wire(struct bud *b, int w)
{
    struct buds_st *s = &b->s; int lv[3], nl = nt_levels(nt_x(b), lv);
    if (w == NW_OFF) s->noise = BN_OFF;
    else if (w == NW_TRANSP) s->noise = BN_AMBIENT;
    else if (w == NW_ADAPT) s->noise = BN_ADAPTIVE;
    else { s->noise = BN_ANC; for (int i = 0; i < nl; i++) if (lv[i] == w) s->anc_lvl = i; }
    b->changed = 1;
}
static void nt_msg(struct bud *b, uint16_t cmd, const uint8_t *d, int n)
{
    struct buds_st *s = &b->s; struct nt_priv *p = ntp(b); const struct nt_model *x = nt_x(b);
    p->got_any = 1;
    if (p->inflight && (cmd & 0xF000) == 0x4000 && (cmd & 0x0FFF) == (p->inflight & 0x0FFF)) p->inflight = 0;
    switch (cmd) {
    case 0x4007: case 0xE001: {                                  /* battery */
        if (n < 1) break;
        int seen[3] = { 0, 0, 0 };
        for (int i = 0; i < d[0] && 2 + i * 2 <= n; i++) {
            int id = d[1 + i * 2], v = d[2 + i * 2], k = id == 0x02 ? 0 : id == 0x03 ? 1 : id == 0x04 ? 2 : id == 0x06 ? 0 : -1;
            if (k < 0) continue;
            s->batt[k] = (v & 0x7F) <= 100 ? v & 0x7F : -1; s->chg[k] = (v & 0x80) != 0; seen[k] = 1;
        }
        for (int k = 0; k < 3; k++) if (!seen[k] && !(k == 0 && (s->caps & BC_BATT_ONE))) { s->batt[k] = -1; s->chg[k] = -1; }
        b->ready = 1; b->changed = 1;
        break;
    }
    case 0x401E: if (n >= 2) nt_noise_wire(b, d[1]); break;
    case 0xE003: if (n >= 2) nt_noise_wire(b, d[1]); else if (n == 1) nt_noise_wire(b, d[0]); break;
    case 0x401F: case 0x4050: {
        const uint8_t *t = x->cmf_eq ? NT_EQ_CMF : NT_EQ_EAR; int tn = x->cmf_eq ? 7 : 5;
        if (n >= 1) { s->eq = -1; for (int i = 0; i < tn; i++) if (t[i] == d[0]) s->eq = i; b->changed = 1; }
        break;
    }
    case 0x4044:                                                 /* custom 3-band EQ */
        if (n >= 5 && d[0] >= 1 && d[0] <= 10) {
            int nb = d[0];
            if (5 + nb * 13 <= n) { s->nbands = nb; for (int i = 0; i < nb; i++) { float g = nt_f32(d + 5 + i * 13 + 1); s->bands[i] = (int)(g < 0 ? g - .5f : g + .5f); } b->changed = 1; }
        }
        break;
    case 0x404E:                                                 /* Ultra Bass {on, level} */
        if (n >= 2) { s->bass = !d[0] ? 0 : x->bass3 ? (d[1] >= 10 ? 2 : d[1] >= 5 ? 1 : 0) : d[1] / 2; if (s->bass > s->bass_max) s->bass = s->bass_max; b->changed = 1; }
        break;
    case 0x400E: if (n >= 3) { s->eardet = d[2] != 0; b->changed = 1; } break;
    case 0x4041: if (n >= 1) { s->game = d[0] == 1; b->changed = 1; } break;
    case 0x4018:                                                 /* gestures */
        if (n >= 1) for (int i = 0; i < d[0] && 1 + i * 4 + 4 <= n; i++) {
            const uint8_t *e = d + 1 + i * 4;
            int side = e[0] == 0x02 ? 0 : e[0] == 0x03 ? 1 : -1, g = -1;
            for (int k = 0; k < BG_N; k++) if (NT_GESTURE[k] == e[2]) g = k;
            if (side >= 0 && g >= 0) { s->gest[side][g] = nt_action(e[3]); b->changed = 1; }
        }
        break;
    case 0xE00D: if (n >= 2) { s->fit[0] = d[0] ? 1 : 0; s->fit[1] = d[1] ? 1 : 0; b->changed = 1; } break;
    case 0x4042: {
        char v[40]; int k = 0;
        for (int i = 0; i < n && k < 39; i++) if (d[i] >= 32 && d[i] < 127) v[k++] = (char)d[i];
        v[k] = 0; if (k) { buds_cpy(s->fw, sizeof s->fw, v); b->changed = 1; }
        break;
    }
    default:
        if ((cmd & 0xF000) != 0x7000) bud_unknown(b, cmd, d, n);      /* 0x7xxx = echo of our sets */
        break;
    }
    if (b->changed) s->conn = BS_READY;
}
static void nt_rx(struct bud *b, const uint8_t *p, int n)
{
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    while (i < b->rn) {
        const uint8_t *f = b->rbuf + i; int left = b->rn - i;
        if (f[0] != 0x55) { i++; continue; }
        if (left < 8) break;
        uint16_t ctl = (uint16_t)(f[1] | f[2] << 8), cmd = (uint16_t)(f[3] | f[4] << 8); int len = f[5] | f[6] << 8;
        int tot = 8 + len + (ctl & 0x20 ? 2 : 0);
        if (len > 1024 || tot > (int)sizeof b->rbuf) { i++; continue; }
        if (left < tot) break;
        if (ctl & 0x20) {
            uint16_t c = nt_crc16(f, 8 + len);
            if (f[8 + len] != (c & 0xFF) || f[9 + len] != (c >> 8)) { i++; continue; }
        }
        if (ctl & 0x1F) bud_log(b, "nothing: 0x%04x answered with error %d", cmd, ctl & 0x1F);
        else nt_msg(b, cmd, f + 8, len);
        i += tot;
    }
    if (i > 0) { memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
    nt_next(b, b->now);              /* next get of the initial read-out */
}
static void nt_tick(struct bud *b, double now)
{
    struct nt_priv *p = ntp(b);
    if (p->inflight && now - p->t_sent > 2000) {
        if (p->retries++ < 1) { p->t_sent = now; nt_send(b, p->inflight, 0, 0); }
        else if (!p->got_any && !p->ctl_switched) {          /* nothing at all: try the other control word [D] */
            p->ctl_switched = 1; p->ctl = p->ctl == 0x0160 ? 0x0120 : 0x0160;
            bud_log(b, "nothing: no answer, trying control 0x%04x", p->ctl);
            p->qi = 0; p->inflight = 0;
        } else { bud_log(b, "nothing: 0x%04x unanswered, skipped", p->inflight); p->inflight = 0; }
    }
    if (!p->inflight) nt_next(b, now);
    if (b->ready && now - p->t_batt > 300000) { p->t_batt = now; if (!p->inflight) nt_send(b, 0xC007, 0, 0); }
    if (!p->t_batt) p->t_batt = now;
}
/* 3-band custom EQ payload (F041): count, total gain, 3 x {type, gain, freq, Q}, 3 x {00 00 00} */
static int nt_eq3(uint8_t *o, const int *g)
{
    static const float fr[3] = { 140, 980, 3400 }, q[3] = { 0.8f, 0.7f, 1.0f };
    int k = 0; float mx = 0;
    for (int i = 0; i < 3; i++) if (g[i] > mx) mx = (float)g[i];
    o[k++] = 3; nt_put_f32(o + k, -mx); k += 4;
    for (int i = 0; i < 3; i++) { o[k++] = (uint8_t)i; nt_put_f32(o + k, (float)g[i]); k += 4; nt_put_f32(o + k, fr[i]); k += 4; nt_put_f32(o + k, q[i]); k += 4; }
    memset(o + k, 0, 9); k += 9;
    return k;
}
/* 8-band advanced EQ payload (F050) */
static int nt_eq8(uint8_t *o, const int *g)
{
    static const float fr[8] = { 55, 110, 220, 440, 1320, 3300, 6600, 13200 };
    int k = 0; float mx = 0;
    for (int i = 0; i < 8; i++) if (g[i] > mx) mx = (float)g[i];
    o[k++] = 0; o[k++] = 8; nt_put_f32(o + k, -mx); k += 4;
    for (int i = 0; i < 8; i++) { o[k++] = 1; nt_put_f32(o + k, (float)g[i]); k += 4; nt_put_f32(o + k, fr[i]); k += 4; nt_put_f32(o + k, 1.0f); k += 4; }
    return k;
}
static int nt_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; const struct nt_model *x = nt_x(b);
    uint8_t pl[160]; int r, lv[3], nl = nt_levels(x, lv);
    switch (c->op) {
    case BCMD_NOISE: case BCMD_ANCLVL: {
        if (!(s->caps & BC_NOISE)) return BE_NOTSUP;
        int mode = c->op == BCMD_NOISE ? c->a : BN_ANC, lvl = c->op == BCMD_ANCLVL ? c->a : s->anc_lvl;
        if (mode < 0 || mode >= BN_N || !(s->noise_mask & BUDS_MASK(mode))) return BE_RANGE;
        if (mode == BN_ANC) { if (lvl < 0 || lvl >= nl) lvl = nl - 1; }
        int w = mode == BN_OFF ? NW_OFF : mode == BN_AMBIENT ? NW_TRANSP : mode == BN_ADAPTIVE ? NW_ADAPT : lv[lvl];
        pl[0] = 0x01; pl[1] = (uint8_t)w; pl[2] = 0;
        if ((r = nt_send(b, 0xF00F, pl, 3)) == BE_OK) { s->noise = mode; if (mode == BN_ANC) s->anc_lvl = lvl; b->changed = 1; }
        return r;
    }
    case BCMD_EQ: {
        if (!(s->caps & BC_EQ)) return BE_NOTSUP;
        if (c->a < 0 || c->a >= s->eq_n) return BE_RANGE;
        pl[0] = x->cmf_eq ? NT_EQ_CMF[c->a] : NT_EQ_EAR[c->a]; pl[1] = 0;
        if ((r = nt_send(b, x->cmf_eq ? 0xF01D : 0xF010, pl, 2)) == BE_OK) { s->eq = c->a; b->changed = 1; }
        return r;
    }
    case BCMD_BANDS: {
        if (!(s->caps & BC_EQ_BANDS)) return BE_NOTSUP;
        int g[10], n = buds_parse_ints(c->s, g, 10);
        if (n != 3 && !(n == 8 && x->adv_eq)) return BE_RANGE;
        for (int i = 0; i < n; i++) if (g[i] < s->band_min || g[i] > s->band_max) return BE_RANGE;
        if (n == 3) r = nt_send(b, 0xF041, pl, nt_eq3(pl, g));
        else { uint8_t on[2] = { 1, 0 }; nt_send(b, 0xF04F, on, 2); r = nt_send(b, 0xF050, pl, nt_eq8(pl, g)); }
        if (r == BE_OK) {
            s->nbands = n; for (int i = 0; i < n; i++) s->bands[i] = g[i];
            int cus = x->cmf_eq ? 5 : 4; s->eq = cus; b->changed = 1;   /* the "Custom" preset becomes active */
        }
        return r;
    }
    case BCMD_BASS:
        if (!(s->caps & BC_BASS)) return BE_NOTSUP;
        if (c->a < 0 || c->a > s->bass_max) return BE_RANGE;
        pl[0] = c->a > 0; pl[1] = (uint8_t)(x->bass3 ? c->a * 5 : c->a * 2);
        if ((r = nt_send(b, 0xF051, pl, 2)) == BE_OK) { s->bass = c->a; b->changed = 1; }
        return r;
    case BCMD_EARDET:
        if (!(s->caps & BC_EARDET)) return BE_NOTSUP;
        pl[0] = 1; pl[1] = 1; pl[2] = c->a ? 1 : 0;
        if ((r = nt_send(b, 0xF004, pl, 3)) == BE_OK) { s->eardet = !!c->a; b->changed = 1; }
        return r;
    case BCMD_GAME:
        if (!(s->caps & BC_GAME)) return BE_NOTSUP;
        pl[0] = c->a ? 1 : 2; pl[1] = 0;
        if ((r = nt_send(b, 0xF040, pl, 2)) == BE_OK) { s->game = !!c->a; b->changed = 1; }
        return r;
    case BCMD_GEST: {
        if (!(s->caps & BC_GEST)) return BE_NOTSUP;
        if (c->a < 0 || c->a > 1 || c->b < 0 || c->b >= BG_N || c->c < 0 || c->c >= BA_N || !(s->gest_opts[c->b] & (1u << c->c))) return BE_RANGE;
        int code = nt_action_code(c->c); if (code < 0) return BE_RANGE;
        pl[0] = 0x01; pl[1] = c->a ? 0x03 : 0x02; pl[2] = 0x01; pl[3] = NT_GESTURE[c->b]; pl[4] = (uint8_t)code;
        if ((r = nt_send(b, 0xF003, pl, 5)) == BE_OK) { s->gest[c->a][c->b] = c->c; b->changed = 1; }
        return r;
    }
    case BCMD_FIND:
        if (!(s->caps & BC_FIND)) return BE_NOTSUP;
        if (c->a < 0 || c->a > 2) return BE_RANGE;
        if (x->find == 1) { pl[0] = c->b ? 1 : 0; r = nt_send(b, 0xF002, pl, 1); }
        else if (x->find == 2) { pl[0] = 0x06; pl[1] = c->b ? 1 : 0; r = nt_send(b, 0xF002, pl, 2); }
        else {
            r = BE_OK;
            for (int side = 0; side < 2; side++) if (c->a == 2 || c->a == side || !c->b) { pl[0] = side ? 0x03 : 0x02; pl[1] = c->b && (c->a == 2 || c->a == side); if (nt_send(b, 0xF002, pl, 2) != BE_OK) r = BE_IO; }
        }
        if (r == BE_OK) { s->find[0] = c->b && c->a != 1; s->find[1] = c->b && c->a != 0; b->changed = 1; }
        return r;
    case BCMD_FIT:
        if (!(s->caps & BC_FIT)) return BE_NOTSUP;
        pl[0] = 1;
        if ((r = nt_send(b, 0xF014, pl, 1)) == BE_OK) { s->fit[0] = s->fit[1] = -1; b->changed = 1; }
        return r;
    default: return BE_NOTSUP;
    }
}
static const struct buds_brand BRAND_NOTHING = { BB_NOTHING, "nothing", nt_identify, BT_RFCOMM, 0, nt_uuid, nt_start, nt_rx, nt_cmd, nt_tick, nt_fill };
#endif
