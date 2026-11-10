/* buds_samsung.inc.c - Samsung Galaxy Buds over RFCOMM (SPP), brand module of hibybudsd.
 * Pure protocol code: bytes in, bytes out through struct bud (buds_core.inc.c).
 *
 * Written from our own notes (docs/HEADPHONES.md section 4); no code from GPL projects.
 * Facts marked [I] there (message layouts beyond the common prefix, touch action codes, some
 * ids) must be confirmed with btmon captures of each model: the parsers log every unknown id
 * and every status payload in hex for that.
 *
 * Framing (one message):
 *   legacy (Galaxy Buds 2019):  FE  type  size        id payload crc16  EE
 *   new (Buds+ and later):      FD  hdr(LE16)         id payload crc16  DD
 *     hdr: bits 0-9 size, bit 12 response, bit 13 fragment (more fragments follow)
 *   size = 1 (id) + payload + 2 (crc); crc = CRC-16/XMODEM over id + payload.
 * The byte order of the CRC is learnt from the first valid frame the buds send (they speak
 * first: extended status at connect); until then we send it little-endian. */
#ifndef BUDS_SAMSUNG_INC_C
#define BUDS_SAMSUNG_INC_C
#include "buds_core.inc.c"

#define SAM_UUID_NEW "2e73a4ad-332d-41fc-90e2-16bef06523f2"
#define SAM_UUID_LEGACY "00001102-0000-1000-8000-00805f9b34fd"
enum {
    SAM_STATUS = 0x60, SAM_EXT_STATUS = 0x61, SAM_VERSION = 0x63, SAM_NOISE_UPDATE = 0x77, SAM_NOISE_SET = 0x78,
    SAM_AMBIENT_SET = 0x80, SAM_ANC_LEVEL = 0x83, SAM_AMBIENT_LEVEL = 0x84, SAM_EQ = 0x86, SAM_MANAGER_INFO = 0x88,
    SAM_LOCK = 0x90, SAM_TOUCH_OPT = 0x92, SAM_NR_SET = 0x98, SAM_NR_UPDATE = 0x9B,
    SAM_FIND_START = 0xA0, SAM_FIND_STOP = 0xA1, SAM_MUTE = 0xA2,
};
/* layouts (struct buds_model.layout): framing + where the fields of the status messages are */
enum { SL_LEGACY, SL_PLUS, SL_LIVE, SL_PRO, SL_N };
struct sam_layout {
    uint8_t legacy;                   /* FE/EE framing, legacy UUID */
    int8_t st_bl, st_br, st_place, st_case;          /* STATUS_UPDATED (0x60) */
    int8_t ex_bl, ex_br, ex_place, ex_case;          /* EXTENDED_STATUS_UPDATED (0x61) */
    int8_t ex_amb_on, ex_amb_lvl, ex_game, ex_eq, ex_lock, ex_touch, ex_noise, ex_anc_on;
    uint8_t noise_cmd;                /* 0x80 ambient on/off, 0x98 ANC on/off, 0x78 modes */
};
/* [I]: only the common prefix (battery, placement, case) is well established; the rest is our
 * best reading of public notes and is checked for range before use. */
static const struct sam_layout SAM_LAYOUTS[SL_N] = {
    /* legacy Buds: status = battL battR coupled main wear; ext = rev type battL battR coupled main wear ambOn ambMode ambVol eq lock touch */
    { 1, 0, 1, 4, -1,   2, 3, 6, -1,   7, 9, -1, 10, 11, 12, -1, -1, SAM_AMBIENT_SET },
    /* Buds+: ext = rev type battL battR coupled main place case ambOn ambLvl game eq lock touch */
    { 0, 1, 2, 5, 6,    2, 3, 6, 7,    8, 9, 10, 11, 12, 13, -1, -1, SAM_AMBIENT_SET },
    /* Buds Live: ext = ... case game eq lock touch ancOn */
    { 0, 1, 2, 5, 6,    2, 3, 6, 7,    -1, -1, 8, 9, 10, 11, -1, 12, SAM_NR_SET },
    /* Buds Pro / Buds2 / Buds2 Pro / FE / Buds3 family: ext = ... case game eq lock touch noise */
    { 0, 1, 2, 5, 6,    2, 3, 6, 7,    -1, -1, 8, 9, 10, 11, 12, -1, SAM_NOISE_SET },
};
#define SG_BAT (BC_BATT_LR | BC_BATT_CASE | BC_WEAR | BC_EQ | BC_LOCK | BC_GEST | BC_FIND | BC_FW)
#define SG_NC3 (BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC) | BUDS_MASK(BN_AMBIENT))
#define SG_HOLD_A ((1u << BA_VOICE) | (1u << BA_AMBIENT) | (1u << BA_VOLUME) | (1u << BA_SPOTIFY))
#define SG_HOLD_L ((1u << BA_VOICE) | (1u << BA_ANC) | (1u << BA_VOLUME) | (1u << BA_SPOTIFY))
#define SG_HOLD_N ((1u << BA_VOICE) | (1u << BA_NOISE) | (1u << BA_VOLUME) | (1u << BA_SPOTIFY))
/* order matters: longer names first ("Buds2 Pro" before "Buds2") */
static const struct buds_model SAMSUNG_MODELS[] = {
    { BB_SAMSUNG, 11, "Galaxy Buds3 Pro", "Buds3 Pro", BF_GBUDS_STEM, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3 | BUDS_MASK(BN_ADAPTIVE), 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 12, "Galaxy Buds3 FE", "Buds3 FE", BF_GBUDS_STEM, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3, 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 10, "Galaxy Buds3", "Buds3", BF_GBUDS_STEM, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3 | BUDS_MASK(BN_ADAPTIVE), 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 7, "Galaxy Buds2 Pro", "Buds2 Pro", BF_GBUDS, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3, 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 6, "Galaxy Buds2", "Buds2", BF_GBUDS, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3, 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 8, "Galaxy Buds FE", "Buds FE", BF_GBUDS, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3, 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 9, "Galaxy Buds Core", "Buds Core", BF_GBUDS, SG_BAT | BC_NOISE | BC_AMB_LVL, SG_NC3, 2, 0, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 5, "Galaxy Buds Pro", "Buds Pro", BF_GBUDS, SG_BAT | BC_NOISE | BC_AMB_LVL | BC_ANC_LVL, SG_NC3, 3, 1, 6, { 0, 0, 0, SG_HOLD_N }, SL_PRO },
    { BB_SAMSUNG, 4, "Galaxy Buds Live", "Buds Live", BF_GBUDS_LIVE, SG_BAT | BC_NOISE, BUDS_MASK(BN_OFF) | BUDS_MASK(BN_ANC), 0, 0, 6, { 0, 0, 0, SG_HOLD_L }, SL_LIVE },
    { BB_SAMSUNG, 3, "Galaxy Buds+", "Buds+", BF_GBUDS, SG_BAT | BC_NOISE | BC_AMB_LVL, BUDS_MASK(BN_OFF) | BUDS_MASK(BN_AMBIENT), 2, 0, 6, { 0, 0, 0, SG_HOLD_A }, SL_PLUS },
    { BB_SAMSUNG, 2, "Galaxy Buds", "Galaxy Buds (", BF_GBUDS, BC_BATT_LR | BC_WEAR | BC_EQ | BC_LOCK | BC_FIND | BC_NOISE | BC_AMB_LVL, BUDS_MASK(BN_OFF) | BUDS_MASK(BN_AMBIENT), 4, 0, 6, { 0 }, SL_LEGACY },
};
/* Samsung service UUID but unknown name: battery, wear, find, and whatever the buds report */
static const struct buds_model SAMSUNG_GENERIC = { BB_SAMSUNG, 1, "Galaxy Buds", 0, BF_GBUDS, BC_BATT_LR | BC_BATT_CASE | BC_WEAR | BC_FIND, 0, 0, 0, 0, { 0 }, SL_PRO };
static const struct buds_model SAMSUNG_GENERIC_LEGACY = { BB_SAMSUNG, 2, "Galaxy Buds", 0, BF_GBUDS, BC_BATT_LR | BC_WEAR | BC_FIND, 0, 0, 0, 0, { 0 }, SL_LEGACY };

static const struct buds_model *sam_model_by_id(int id)
{
    for (unsigned i = 0; i < sizeof SAMSUNG_MODELS / sizeof *SAMSUNG_MODELS; i++) if (SAMSUNG_MODELS[i].id == id) return &SAMSUNG_MODELS[i];
    return id == 1 ? &SAMSUNG_GENERIC : 0;
}
static const struct buds_model *sam_identify(const struct bd_dev *d)
{
    int is_new = bd_has_uuid(d, SAM_UUID_NEW), is_old = bd_has_uuid(d, SAM_UUID_LEGACY);
    unsigned v, p; int samsung_vendor = bd_modalias(d->modalias, &v, &p) && v == 0x0075;
    const char *nm = d->name[0] ? d->name : d->alias;
    int named = strstr(nm, "Galaxy Buds") || strstr(nm, "Buds2") || strstr(nm, "Buds3") || strstr(nm, "Buds Live") || strstr(nm, "Buds Pro") || strstr(nm, "Buds FE");
    if (!is_new && !is_old && !named) return 0;
    if (!is_new && !is_old && !samsung_vendor && !strstr(nm, "Galaxy")) return 0;   /* "Buds2" of another brand */
    for (unsigned i = 0; i < sizeof SAMSUNG_MODELS / sizeof *SAMSUNG_MODELS; i++) {
        const struct buds_model *m = &SAMSUNG_MODELS[i];
        if (strstr(nm, m->match)) {
            if (m->layout == SL_LEGACY && is_new && !is_old) continue;            /* renamed newer model */
            return m;
        }
    }
    return is_old && !is_new ? &SAMSUNG_GENERIC_LEGACY : &SAMSUNG_GENERIC;
}
static const struct sam_layout *sam_lay(const struct bud *b) { return &SAM_LAYOUTS[b->m && b->m->layout < SL_N ? b->m->layout : SL_PRO]; }
static const char *sam_uuid(const struct bud *b) { return sam_lay(b)->legacy ? SAM_UUID_LEGACY : SAM_UUID_NEW; }

/* ------------------------------------------------------------------ framing */
/* one message; returns its length or -1. crc_be: CRC byte order (1 = big-endian) */
static int sam_frame(int legacy, int response, int id, const uint8_t *pl, int n, int crc_be, uint8_t *out, int cap)
{
    int size = 1 + n + 2, total = (legacy ? 3 : 3) + size + 1;
    if (n < 0 || size > 1023 || (legacy && size > 255) || total > cap) return -1;
    int k = 0;
    if (legacy) { out[k++] = 0xFE; out[k++] = (uint8_t)(response ? 1 : 0); out[k++] = (uint8_t)size; }
    else { unsigned h = (unsigned)size | (response ? 0x1000u : 0); out[k++] = 0xFD; out[k++] = (uint8_t)(h & 0xFF); out[k++] = (uint8_t)(h >> 8); }
    out[k++] = (uint8_t)id;
    if (n) memcpy(out + k, pl, (size_t)n);
    k += n;
    uint16_t crc = buds_crc16(out + 3, 1 + n);
    if (crc_be) { out[k++] = (uint8_t)(crc >> 8); out[k++] = (uint8_t)(crc & 0xFF); }
    else { out[k++] = (uint8_t)(crc & 0xFF); out[k++] = (uint8_t)(crc >> 8); }
    out[k++] = legacy ? 0xEE : 0xDD;
    return k;
}
static int sam_send(struct bud *b, int id, const uint8_t *pl, int n)
{
    uint8_t f[300];
    int k = sam_frame(sam_lay(b)->legacy, 0, id, pl, n, b->crc_order == 2, f, sizeof f);
    if (k < 0 || !b->tx) return BE_IO;
    return b->tx(b, f, k) == k ? BE_OK : BE_IO;
}

/* ------------------------------------------------------------------ messages */
static int sam_place(int v) { return v == 1 ? BW_IN : v == 2 ? BW_OUT : v == 3 || v == 4 ? BW_CASE : -1; }
static int sam_batt(int v) { return v >= 0 && v <= 100 ? v : -1; }
static int sam_get(const uint8_t *p, int n, int off) { return off >= 0 && off < n ? p[off] : -1; }
/* touch action code <-> BA_* [I] */
static int sam_action(const struct bud *b, int code)
{
    switch (code) {
    case 1: return BA_VOICE;
    case 2: return b->m && b->m->layout == SL_PLUS ? BA_AMBIENT : b->m && b->m->layout == SL_LIVE ? BA_ANC : BA_NOISE;
    case 3: return BA_VOLUME;
    case 5: return BA_SPOTIFY;
    default: return -1;
    }
}
static int sam_code(int action)
{
    switch (action) {
    case BA_VOICE: return 1;
    case BA_AMBIENT: case BA_ANC: case BA_NOISE: return 2;
    case BA_VOLUME: return 3;
    case BA_SPOTIFY: return 5;
    default: return -1;
    }
}
static void sam_wear_legacy(struct buds_st *s, int w)
{
    if (w < 0) return;
    s->wear[0] = w & 0x10 ? BW_IN : BW_OUT;          /* 0x10 left, 0x01 right [I] */
    s->wear[1] = w & 0x01 ? BW_IN : BW_OUT;
}
static void sam_places(struct buds_st *s, int v)
{
    if (v < 0) return;
    s->wear[0] = sam_place(v >> 4); s->wear[1] = sam_place(v & 15);
}
/* printable runs of the version message ("R190XXU0AUL1"...) */
static void sam_version(struct bud *b, const uint8_t *p, int n)
{
    char best[40] = ""; int k = 0; char cur[40];
    for (int i = 0; i <= n; i++) {
        int c = i < n ? p[i] : 0;
        if (c > 32 && c < 127) { if (k < 39) cur[k++] = (char)c; }
        else { if (k >= 6 && k > (int)strlen(best)) { cur[k] = 0; memcpy(best, cur, (size_t)k + 1); } k = 0; }
    }
    if (best[0]) { buds_cpy(b->s.fw, sizeof b->s.fw, best); b->changed = 1; }
}
static void sam_status(struct bud *b, const uint8_t *p, int n, int ext)
{
    const struct sam_layout *L = sam_lay(b);
    struct buds_st *s = &b->s; int v;
    char h[160]; bud_hex(h, sizeof h, p, n > 48 ? 48 : n);
    bud_log(b, "samsung: %s status (%d bytes): %s", ext ? "extended" : "", n, h);
    int bl = sam_get(p, n, ext ? L->ex_bl : L->st_bl), br = sam_get(p, n, ext ? L->ex_br : L->st_br), bc = sam_get(p, n, ext ? L->ex_case : L->st_case);
    if (bl >= 0) s->batt[0] = sam_batt(bl);
    if (br >= 0) s->batt[1] = sam_batt(br);
    if (bc >= 0 && (s->caps & BC_BATT_CASE)) s->batt[2] = sam_batt(bc);
    v = sam_get(p, n, ext ? L->ex_place : L->st_place);
    if (L->legacy) sam_wear_legacy(s, v); else sam_places(s, v);
    if (ext) {
        b->ext_seen = 1;
        if ((v = sam_get(p, n, L->ex_eq)) >= 0 && (s->caps & BC_EQ)) s->eq = v < s->eq_n ? v : -1;
        if ((v = sam_get(p, n, L->ex_lock)) >= 0) s->lock = v <= 1 ? v : -1;
        if ((v = sam_get(p, n, L->ex_game)) >= 0 && (s->caps & BC_GAME)) s->game = v <= 1 ? v : -1;
        if ((v = sam_get(p, n, L->ex_touch)) >= 0 && (s->caps & BC_GEST)) { s->gest[0][BG_HOLD] = sam_action(b, v >> 4); s->gest[1][BG_HOLD] = sam_action(b, v & 15); }
        if ((v = sam_get(p, n, L->ex_noise)) >= 0) s->noise = v < BN_N && (s->noise_mask & BUDS_MASK(v)) ? v : -1;
        if ((v = sam_get(p, n, L->ex_anc_on)) >= 0) s->noise = v == 1 ? BN_ANC : v == 0 ? BN_OFF : -1;
        if ((v = sam_get(p, n, L->ex_amb_on)) >= 0) s->noise = v == 1 ? BN_AMBIENT : v == 0 ? BN_OFF : -1;
        if ((v = sam_get(p, n, L->ex_amb_lvl)) >= 0) s->amb_lvl = v <= s->amb_max ? v : -1;
    }
    b->ready = 1; b->changed = 1; s->conn = BS_READY;
}
static void sam_msg(struct bud *b, int id, const uint8_t *p, int n)
{
    struct buds_st *s = &b->s;
    switch (id) {
    case SAM_STATUS: sam_status(b, p, n, 0); break;
    case SAM_EXT_STATUS:
        sam_status(b, p, n, 1);
        if (!b->mgr_sent) {                 /* tell the buds who we are (non-Samsung host) [I] */
            static const uint8_t mi[3] = { 0x01, 0x02, 0x1E };
            sam_send(b, SAM_MANAGER_INFO, mi, 3); b->mgr_sent = 1;
        }
        break;
    case SAM_VERSION: sam_version(b, p, n); break;
    case SAM_NOISE_UPDATE:
        if (n >= 1 && p[0] < BN_N) { s->noise = p[0]; b->changed = 1; }
        break;
    case SAM_NR_UPDATE:
        if (n >= 1 && p[0] <= 1) { s->noise = p[0] ? BN_ANC : BN_OFF; b->changed = 1; }
        break;
    default: bud_unknown(b, (unsigned)id, p, n); break;
    }
}
/* complete, valid frame at f (left bytes available)? returns its length or 0 */
static int sam_valid_at(const uint8_t *f, int left)
{
    if (left < 4 || (f[0] != 0xFD && f[0] != 0xFE)) return 0;
    int legacy = f[0] == 0xFE, size = legacy ? f[2] : (int)((f[1] | f[2] << 8) & 0x3FF), total = 3 + size + 1;
    if (size < 3 || left < total || f[total - 1] != (legacy ? 0xEE : 0xDD)) return 0;
    uint16_t crc = buds_crc16(f + 3, size - 2); uint8_t c0 = f[3 + size - 2], c1 = f[3 + size - 1];
    return (c0 == (crc & 0xFF) && c1 == (crc >> 8)) || (c0 == (crc >> 8) && c1 == (crc & 0xFF)) ? total : 0;
}
/* bytes from the RFCOMM stream: reassemble, check, dispatch (handles fragments) */
static void sam_rx(struct bud *b, const uint8_t *p, int n)
{
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    while (i < b->rn) {
        uint8_t *f = b->rbuf + i; int left = b->rn - i;
        if (f[0] != 0xFD && f[0] != 0xFE) { i++; continue; }
        if (left < 4) break;
        int legacy = f[0] == 0xFE, size, frag = 0, hdr = 3;
        if (legacy) size = f[2];
        else { unsigned h = f[1] | f[2] << 8; size = (int)(h & 0x3FF); frag = (h & 0x2000) != 0; }
        if (size < 3) { i++; continue; }
        int total = hdr + size + 1;
        if (total > (int)sizeof b->rbuf) { i++; continue; }
        if (left < total) {                      /* incomplete: a false start byte if a valid frame follows */
            int later = 0;
            for (int j = 1; j < left && !later; j++) later = sam_valid_at(f + j, left - j) > 0;
            if (later) { i++; continue; }
            break;
        }
        if (f[total - 1] != (legacy ? 0xEE : 0xDD)) { i++; continue; }
        const uint8_t *body = f + hdr; int bn = size - 2;               /* id + payload */
        uint16_t crc = buds_crc16(body, bn);
        uint8_t c0 = body[bn], c1 = body[bn + 1];
        int le = c0 == (crc & 0xFF) && c1 == (crc >> 8), be = c0 == (crc >> 8) && c1 == (crc & 0xFF);
        if (!le && !be) { bud_log(b, "samsung: bad CRC (id 0x%02x)", body[0]); i++; continue; }
        if (le != be && b->crc_order != (le ? 1 : 2)) { b->crc_order = le ? 1 : 2; bud_log(b, "samsung: CRC %s-endian", le ? "little" : "big"); }
        int id = body[0];
        if (frag || (b->fragn && b->fragid == id)) {
            if (!b->fragn) b->fragid = id;
            if (b->fragid == id && b->fragn + bn - 1 <= (int)sizeof b->frag) { memcpy(b->frag + b->fragn, body + 1, (size_t)bn - 1); b->fragn += bn - 1; }
            else b->fragn = 0;
            if (!frag && b->fragn) { sam_msg(b, id, b->frag, b->fragn); b->fragn = 0; }
        } else sam_msg(b, id, body + 1, bn - 1);
        i += total;
    }
    if (i > 0) { memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
}
static void sam_start(struct bud *b) { (void)b; }       /* the buds speak first (extended status) */

static int sam_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; const struct sam_layout *L = sam_lay(b);
    uint8_t pl[4]; int r;
    switch (c->op) {
    case BCMD_NOISE:
        if (!(s->caps & BC_NOISE)) return BE_NOTSUP;
        if (c->a < 0 || c->a >= BN_N || !(s->noise_mask & BUDS_MASK(c->a))) return BE_RANGE;
        if (L->noise_cmd == SAM_NOISE_SET) { pl[0] = (uint8_t)c->a; r = sam_send(b, SAM_NOISE_SET, pl, 1); }
        else if (L->noise_cmd == SAM_NR_SET) { pl[0] = c->a == BN_ANC; r = sam_send(b, SAM_NR_SET, pl, 1); }
        else { pl[0] = c->a == BN_AMBIENT; r = sam_send(b, SAM_AMBIENT_SET, pl, 1); }
        if (r == BE_OK) { s->noise = c->a; b->changed = 1; }
        return r;
    case BCMD_AMBLVL:
        if (!(s->caps & BC_AMB_LVL)) return BE_NOTSUP;
        if (c->a < 0 || c->a > s->amb_max) return BE_RANGE;
        pl[0] = (uint8_t)c->a;
        if ((r = sam_send(b, SAM_AMBIENT_LEVEL, pl, 1)) == BE_OK) { s->amb_lvl = c->a; b->changed = 1; }
        return r;
    case BCMD_ANCLVL:
        if (!(s->caps & BC_ANC_LVL)) return BE_NOTSUP;
        if (c->a < 0 || c->a > s->anc_max) return BE_RANGE;
        pl[0] = (uint8_t)c->a;
        if ((r = sam_send(b, SAM_ANC_LEVEL, pl, 1)) == BE_OK) { s->anc_lvl = c->a; b->changed = 1; }
        return r;
    case BCMD_EQ:
        if (!(s->caps & BC_EQ)) return BE_NOTSUP;
        if (c->a < 0 || c->a >= s->eq_n) return BE_RANGE;
        pl[0] = (uint8_t)c->a;
        if ((r = sam_send(b, SAM_EQ, pl, 1)) == BE_OK) { s->eq = c->a; b->changed = 1; }
        return r;
    case BCMD_LOCK:
        if (!(s->caps & BC_LOCK)) return BE_NOTSUP;
        pl[0] = c->a ? 1 : 0;
        if ((r = sam_send(b, SAM_LOCK, pl, 1)) == BE_OK) { s->lock = !!c->a; b->changed = 1; }
        return r;
    case BCMD_GEST: {                        /* a = side (0 L, 1 R), b = gesture, c = action */
        if (!(s->caps & BC_GEST)) return BE_NOTSUP;
        if (c->a < 0 || c->a > 1 || c->b != BG_HOLD || c->c < 0 || c->c >= BA_N || !(s->gest_opts[BG_HOLD] & (1u << c->c))) return BE_RANGE;
        int g[2] = { s->gest[0][BG_HOLD], s->gest[1][BG_HOLD] };
        g[c->a] = c->c;
        for (int i = 0; i < 2; i++) if (g[i] < 0 || sam_code(g[i]) < 0) g[i] = BA_VOICE;   /* the other side unknown: default */
        pl[0] = (uint8_t)sam_code(g[0]); pl[1] = (uint8_t)sam_code(g[1]);
        if ((r = sam_send(b, SAM_TOUCH_OPT, pl, 2)) == BE_OK) { s->gest[0][BG_HOLD] = g[0]; s->gest[1][BG_HOLD] = g[1]; b->changed = 1; }
        return r;
    }
    case BCMD_FIND:                          /* a = side (0 L, 1 R, 2 both), b = on/off */
        if (!(s->caps & BC_FIND)) return BE_NOTSUP;
        if (c->a < 0 || c->a > 2) return BE_RANGE;
        if (c->b) {
            if ((r = sam_send(b, SAM_FIND_START, 0, 0)) != BE_OK) return r;
            pl[0] = c->a == 1; pl[1] = c->a == 0;                 /* mute the other side */
            sam_send(b, SAM_MUTE, pl, 2);
            s->find[0] = c->a != 1; s->find[1] = c->a != 0;
        } else {
            if ((r = sam_send(b, SAM_FIND_STOP, 0, 0)) != BE_OK) return r;
            s->find[0] = s->find[1] = 0;
        }
        b->changed = 1;
        return BE_OK;
    default: return BE_NOTSUP;
    }
}
/* safety: a find sound never runs more than 2 minutes */
static void sam_tick(struct bud *b, double now)
{
    int on = b->s.find[0] || b->s.find[1];
    if (on && !b->find_t) b->find_t = now;
    if (!on) b->find_t = 0;
    if (on && now - b->find_t > 120000) { struct bcmd c; memset(&c, 0, sizeof c); c.op = BCMD_FIND; c.a = 2; c.b = 0; sam_cmd(b, &c); b->find_t = 0; }
}
static const struct buds_brand BRAND_SAMSUNG = { BB_SAMSUNG, "samsung", sam_identify, BT_RFCOMM, 0, sam_uuid, sam_start, sam_rx, sam_cmd, sam_tick, 0, 0, 0 };
#endif
