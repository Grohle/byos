/* buds_sim.inc.c - simulated headphones for hibybudsd --fake (QEMU harness, PC tests).
 *
 * A simulator plays the HEADPHONE side of the vendor protocol: it parses what the brand module
 * sends (AAP packets, Samsung frames) and answers with notifications the way the real buds do
 * (as described in docs/HEADPHONES.md). So the fake daemon runs the real brand modules end to
 * end: handshake, parsers, command builders, CRC. Nothing here is used with real hardware. */
#ifndef BUDS_SIM_INC_C
#define BUDS_SIM_INC_C
#include "buds_apple.inc.c"
#include "buds_samsung.inc.c"
#include "buds_nothing.inc.c"

struct bsim {
    int brand, layout, legacy;
    struct buds_st t;            /* the "truth" inside the headphones */
    uint8_t q[4096]; int qn;     /* output: Apple = [len lo][len hi][packet]..., Samsung = byte stream */
    int got_hello, got_notify, host_msgs;
};
static void bsim_push(struct bsim *m, const uint8_t *p, int n)
{
    if (m->brand == BB_APPLE) {
        if (m->qn + n + 2 > (int)sizeof m->q) return;
        m->q[m->qn++] = (uint8_t)(n & 0xFF); m->q[m->qn++] = (uint8_t)(n >> 8);
    } else if (m->qn + n > (int)sizeof m->q) return;
    memcpy(m->q + m->qn, p, (size_t)n); m->qn += n;
}
/* next unit for the module's rx(): one AAP packet, or all pending Samsung bytes */
static int bsim_take(struct bsim *m, uint8_t *out, int cap)
{
    if (!m->qn) return 0;
    int n;
    if (m->brand == BB_APPLE) {
        n = m->q[0] | m->q[1] << 8;
        if (n > cap) n = cap;
        memcpy(out, m->q + 2, (size_t)n);
        memmove(m->q, m->q + 2 + n, (size_t)(m->qn - 2 - n)); m->qn -= 2 + n;
        return n;
    }
    n = m->qn > cap ? cap : m->qn;
    memcpy(out, m->q, (size_t)n);
    memmove(m->q, m->q + n, (size_t)(m->qn - n)); m->qn -= n;
    return n;
}
static void bsim_init(struct bsim *m, const struct buds_model *mod, const char *name)
{
    memset(m, 0, sizeof *m);
    m->brand = mod->brand; m->layout = mod->layout; m->legacy = mod->brand == BB_SAMSUNG && SAM_LAYOUTS[mod->layout].legacy;
    buds_st_init(&m->t);
    buds_cpy(m->t.name, sizeof m->t.name, name);
    m->t.batt[0] = 84; m->t.batt[1] = 79; m->t.batt[2] = 56; m->t.chg[0] = m->t.chg[1] = 0; m->t.chg[2] = 1;
    m->t.wear[0] = m->t.wear[1] = BW_IN;
    m->t.noise = mod->noise_mask & BUDS_MASK(BN_ANC) ? BN_ANC : BN_OFF;
    m->t.conv = 1; m->t.adapt_lvl = 50; m->t.cycle = mod->noise_mask & (BUDS_MASK(BN_ANC) | BUDS_MASK(BN_AMBIENT)); m->t.eardet = 1;
    m->t.eq = 0; m->t.lock = 0; m->t.amb_lvl = mod->amb_max > 0 ? 1 : 0; m->t.anc_lvl = 1;
    m->t.gest[0][BG_HOLD] = BA_NOISE; m->t.gest[1][BG_HOLD] = BA_VOICE;
    if (mod->layout == SL_PLUS) m->t.gest[0][BG_HOLD] = BA_AMBIENT;
    if (mod->layout == SL_LIVE) m->t.gest[0][BG_HOLD] = BA_ANC;
}
/* ---- Apple side */
static void bsim_ap_ctl(struct bsim *m, int id, int v)
{
    uint8_t p[11]; aap_control(p, id, v, 0, 0, 0); bsim_push(m, p, 11);
}
static void bsim_ap_battery(struct bsim *m, int single)
{
    uint8_t p[32] = { 0x04, 0x00, 0x04, 0x00, 0x04, 0x00 }; int k = 7;
    static const uint8_t comp[3] = { 0x04, 0x02, 0x08 };
    int n = single ? 1 : 3;
    for (int i = 0; i < n; i++) {
        p[k++] = single ? 0x01 : comp[i]; p[k++] = 0x01;
        p[k++] = (uint8_t)(m->t.batt[i] < 0 ? 0 : m->t.batt[i]);
        p[k++] = m->t.batt[i] < 0 ? 0x04 : m->t.chg[i] == 1 ? 0x01 : 0x02; p[k++] = 0x01;
    }
    p[6] = (uint8_t)n;
    bsim_push(m, p, k);
}
static void bsim_ap_ear(struct bsim *m)
{
    uint8_t p[8] = { 0x04, 0x00, 0x04, 0x00, 0x06, 0x00 };
    for (int i = 0; i < 2; i++) p[6 + i] = m->t.wear[i] == BW_IN ? 0 : m->t.wear[i] == BW_CASE ? 2 : 1;
    bsim_push(m, p, 8);
}
static void bsim_ap_meta(struct bsim *m, const char *modelno)
{
    uint8_t p[200] = { 0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xED, 0x00 }; int k = 9;
    const char *f[6] = { m->t.name, modelno, "Apple Inc.", "H3KQ2XYZ0C6L", "7A305", "7A305" };
    for (int i = 0; i < 6; i++) { int l = (int)strlen(f[i]); if (k + l + 1 > (int)sizeof p) break; memcpy(p + k, f[i], (size_t)l + 1); k += l + 1; }
    bsim_push(m, p, k);
}
/* ---- Samsung side */
static void bsim_sam_send(struct bsim *m, int id, const uint8_t *pl, int n)
{
    uint8_t f[300]; int k = sam_frame(m->legacy, 1, id, pl, n, 0, f, sizeof f);
    if (k > 0) bsim_push(m, f, k);
}
static int bsim_sam_place(int w) { return w == BW_IN ? 1 : w == BW_CASE ? 3 : 2; }
static int bsim_sam_tcode(int a) { int c = sam_code(a); return c < 0 ? 1 : c; }
static void bsim_sam_ext(struct bsim *m)
{
    const struct sam_layout *L = &SAM_LAYOUTS[m->layout];
    uint8_t p[40]; memset(p, 0, sizeof p);
    int n = 24;
    p[0] = 0x0B; p[1] = 0x02;
    p[L->ex_bl] = (uint8_t)m->t.batt[0]; p[L->ex_br] = (uint8_t)m->t.batt[1];
    if (L->ex_case >= 0) p[L->ex_case] = (uint8_t)m->t.batt[2];
    p[4] = 1;
    if (L->legacy) p[L->ex_place] = (uint8_t)((m->t.wear[0] == BW_IN ? 0x10 : 0) | (m->t.wear[1] == BW_IN ? 0x01 : 0));
    else p[L->ex_place] = (uint8_t)(bsim_sam_place(m->t.wear[0]) << 4 | bsim_sam_place(m->t.wear[1]));
    if (L->ex_eq >= 0) p[L->ex_eq] = (uint8_t)m->t.eq;
    if (L->ex_lock >= 0) p[L->ex_lock] = (uint8_t)m->t.lock;
    if (L->ex_touch >= 0) p[L->ex_touch] = (uint8_t)(bsim_sam_tcode(m->t.gest[0][BG_HOLD]) << 4 | bsim_sam_tcode(m->t.gest[1][BG_HOLD]));
    if (L->ex_noise >= 0) p[L->ex_noise] = (uint8_t)m->t.noise;
    if (L->ex_anc_on >= 0) p[L->ex_anc_on] = m->t.noise == BN_ANC;
    if (L->ex_amb_on >= 0) p[L->ex_amb_on] = m->t.noise == BN_AMBIENT;
    if (L->ex_amb_lvl >= 0) p[L->ex_amb_lvl] = (uint8_t)m->t.amb_lvl;
    bsim_sam_send(m, SAM_EXT_STATUS, p, n);
}
static void bsim_sam_status(struct bsim *m)
{
    const struct sam_layout *L = &SAM_LAYOUTS[m->layout];
    uint8_t p[16]; memset(p, 0, sizeof p);
    p[L->st_bl] = (uint8_t)m->t.batt[0]; p[L->st_br] = (uint8_t)m->t.batt[1];
    if (L->st_case >= 0) p[L->st_case] = (uint8_t)m->t.batt[2];
    if (L->legacy) p[L->st_place] = (uint8_t)((m->t.wear[0] == BW_IN ? 0x10 : 0) | (m->t.wear[1] == BW_IN ? 0x01 : 0));
    else p[L->st_place] = (uint8_t)(bsim_sam_place(m->t.wear[0]) << 4 | bsim_sam_place(m->t.wear[1]));
    bsim_sam_send(m, SAM_STATUS, p, 8);
}
/* the buds were connected: what they push by themselves */
static void bsim_connected(struct bsim *m)
{
    if (m->brand == BB_SAMSUNG) {
        bsim_sam_ext(m);
        static const char v[] = "\x01\x02R510XXUAAXK1\x00R510XXUAAXK1";
        bsim_sam_send(m, SAM_VERSION, (const uint8_t *)v, (int)sizeof v - 1);
    }
}
/* something changed inside the headphones (battery, wear): notify like the real ones */
static void bsim_nt_battery(struct bsim *m, uint16_t cmd, uint8_t seq);
static void bsim_changed(struct bsim *m, int single)
{
    if (m->brand == BB_NOTHING) { bsim_nt_battery(m, 0xE001, 0); return; }
    if (m->brand == BB_APPLE) { if (m->got_notify) { bsim_ap_battery(m, single); bsim_ap_ear(m); } }
    else bsim_sam_status(m);
}
/* ---- Nothing side (frames with CRC, control 0x0160) */
static void bsim_nt_send(struct bsim *m, uint16_t cmd, const uint8_t *pl, int n, uint8_t seq)
{
    uint8_t f[300]; int k = nt_frame(0x0160, cmd, seq, pl, n, f, sizeof f);
    if (k > 0) bsim_push(m, f, k);
}
static void bsim_nt_battery(struct bsim *m, uint16_t cmd, uint8_t seq)
{
    uint8_t p[7] = { 3, 0x02, 0, 0x03, 0, 0x04, 0 };
    for (int i = 0; i < 3; i++) p[2 + i * 2] = (uint8_t)((m->t.batt[i] < 0 ? 0 : m->t.batt[i]) | (m->t.chg[i] == 1 ? 0x80 : 0));
    bsim_nt_send(m, cmd, p, 7, seq);
}
static void bsim_nothing(struct bsim *m, const uint8_t *p, int n, const struct buds_model *mod)
{
    const struct nt_model *x = 0;
    for (unsigned i = 0; i < sizeof NOTHING_MODELS / sizeof *NOTHING_MODELS; i++) if (mod == &NOTHING_MODELS[i].m) x = &NOTHING_MODELS[i];
    if (!x) x = &NOTHING_GENERIC;
    for (int i = 0; i + 8 <= n; ) {
        if (p[i] != 0x55) { i++; continue; }
        uint16_t ctl = (uint16_t)(p[i + 1] | p[i + 2] << 8), cmd = (uint16_t)(p[i + 3] | p[i + 4] << 8); int len = p[i + 5] | p[i + 6] << 8;
        int tot = 8 + len + (ctl & 0x20 ? 2 : 0); if (i + tot > n) break;
        const uint8_t *d = p + i + 8; uint8_t seq = p[i + 7], r[64]; int lv[3], nl = nt_levels(x, lv);
        uint16_t rep = (uint16_t)(cmd & 0x7FFF);
        switch (cmd) {
        case 0xC042: bsim_nt_send(m, rep, (const uint8_t *)"1.0.1.101", 9, seq); break;
        case 0xC007: bsim_nt_battery(m, rep, seq); break;
        case 0xC01E: {
            int w = m->t.noise == BN_OFF ? NW_OFF : m->t.noise == BN_AMBIENT ? NW_TRANSP : m->t.noise == BN_ADAPTIVE ? NW_ADAPT : nl ? lv[nl - 1] : NW_OFF;
            r[0] = 1; r[1] = (uint8_t)w; r[2] = 0; bsim_nt_send(m, rep, r, 3, seq); break;
        }
        case 0xC01F: case 0xC050: r[0] = x->cmf_eq ? 7 : 0; bsim_nt_send(m, rep, r, 1, seq); break;
        case 0xC044: { int g[3] = { 3, 0, -2 }; int k = nt_eq3(r, g); bsim_nt_send(m, rep, r, k, seq); break; }
        case 0xC04E: r[0] = 1; r[1] = 4; bsim_nt_send(m, rep, r, 2, seq); break;
        case 0xC00E: r[0] = 1; r[1] = 1; r[2] = 1; bsim_nt_send(m, rep, r, 3, seq); break;
        case 0xC041: r[0] = 2; bsim_nt_send(m, rep, r, 1, seq); break;
        case 0xC018: {
            static const uint8_t g[] = { 6, 2, 1, 2, 0x09, 3, 1, 2, 0x09, 2, 1, 3, 0x08, 3, 1, 3, 0x08, 2, 1, 7, 0x0A, 3, 1, 7, 0x0A };
            bsim_nt_send(m, rep, g, (int)sizeof g, seq); break;
        }
        case 0xF00F: if (len >= 2) { r[0] = 1; r[1] = d[1]; bsim_nt_send(m, 0xE003, r, 2, seq); } break;
        case 0xF014: r[0] = 0; r[1] = 1; bsim_nt_send(m, 0xE00D, r, 2, seq); break;
        default: bsim_nt_send(m, rep, 0, 0, seq); break;          /* echo of a set */
        }
        i += tot;
    }
}
/* bytes from the host (one tx() call = one AAP packet or whole Samsung frames) */
static void bsim_from_host(struct bsim *m, const uint8_t *p, int n, const struct buds_model *mod)
{
    m->host_msgs++;
    if (m->brand == BB_NOTHING) { bsim_nothing(m, p, n, mod); return; }
    if (m->brand == BB_APPLE) {
        if (n >= 16 && p[0] == 0x00 && p[4] == 0x01) { static const uint8_t ack[6] = { 0x01, 0x00, 0x04, 0x00, 0x00, 0x00 }; bsim_push(m, ack, 6); m->got_hello = 1; return; }
        if (n < 6 || p[0] != 0x04) return;
        unsigned op = p[4] | p[5] << 8;
        if (op == AAP_OP_NOTIFY && m->got_hello) {
            m->got_notify = 1;
            int single = (mod->caps & BC_BATT_ONE) != 0;
            bsim_ap_battery(m, single); bsim_ap_ear(m);
            if (mod->caps & BC_NOISE) bsim_ap_ctl(m, AAP_CTL_LISTEN, AAP_MODE[m->t.noise]);
            if (mod->caps & BC_CONV) bsim_ap_ctl(m, AAP_CTL_CONV, m->t.conv ? 1 : 2);
            if (mod->caps & BC_ADAPT_LVL) bsim_ap_ctl(m, AAP_CTL_ADAPT, m->t.adapt_lvl);
            if (mod->caps & BC_CYCLE) { int v = 0; for (int i = 0; i < BN_N; i++) if (m->t.cycle & (1 << i)) v |= AAP_CYCLE_BIT[i]; bsim_ap_ctl(m, AAP_CTL_CYCLE, v); }
            bsim_ap_ctl(m, AAP_CTL_EARDET, m->t.eardet ? 1 : 2);
            bsim_ap_meta(m, mod->id == 0x200A ? "A2096" : "A3048");
        } else if (op == AAP_OP_CONTROL && n >= 8) {
            int id = p[6], v = p[7];
            if (id == AAP_CTL_LISTEN) for (int i = 0; i < BN_N; i++) { if (AAP_MODE[i] == v) m->t.noise = i; }
            if (id == AAP_CTL_CONV) m->t.conv = v == 1;
            if (id == AAP_CTL_ADAPT) m->t.adapt_lvl = v;
            if (id == AAP_CTL_EARDET) m->t.eardet = v == 1;
            if (id == AAP_CTL_CYCLE) { m->t.cycle = 0; for (int i = 0; i < BN_N; i++) if (v & AAP_CYCLE_BIT[i]) m->t.cycle |= 1 << i; }
            bsim_ap_ctl(m, id, v);                          /* the buds confirm the new value */
        }
        return;
    }
    /* Samsung: decode the host's frames */
    for (int i = 0; i + 4 <= n; ) {
        int legacy = p[i] == 0xFE, size = legacy ? p[i + 2] : ((p[i + 1] | p[i + 2] << 8) & 0x3FF), tot = 3 + size + 1;
        if ((p[i] != 0xFD && p[i] != 0xFE) || size < 3 || i + tot > n) break;
        int id = p[i + 3]; const uint8_t *pl = p + i + 4; int pn = size - 3;
        uint8_t r[4];
        switch (id) {
        case SAM_NOISE_SET: if (pn >= 1) { m->t.noise = pl[0]; r[0] = pl[0]; bsim_sam_send(m, SAM_NOISE_UPDATE, r, 1); } break;
        case SAM_NR_SET: if (pn >= 1) { m->t.noise = pl[0] ? BN_ANC : BN_OFF; r[0] = pl[0]; bsim_sam_send(m, SAM_NR_UPDATE, r, 1); } break;
        case SAM_AMBIENT_SET: if (pn >= 1) m->t.noise = pl[0] ? BN_AMBIENT : BN_OFF; break;
        case SAM_AMBIENT_LEVEL: if (pn >= 1) m->t.amb_lvl = pl[0]; break;
        case SAM_ANC_LEVEL: if (pn >= 1) m->t.anc_lvl = pl[0]; break;
        case SAM_EQ: if (pn >= 1) m->t.eq = pl[0]; break;
        case SAM_LOCK: if (pn >= 1) m->t.lock = pl[0]; break;
        case SAM_TOUCH_OPT: if (pn >= 2) { m->t.gest[0][BG_HOLD] = pl[0]; m->t.gest[1][BG_HOLD] = pl[1]; } break;
        case SAM_FIND_START: m->t.find[0] = m->t.find[1] = 1; break;
        case SAM_FIND_STOP: m->t.find[0] = m->t.find[1] = 0; break;
        default: break;
        }
        i += tot;
    }
}
#endif
