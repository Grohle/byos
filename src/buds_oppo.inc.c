/* buds_oppo.inc.c - OPPO / realme / OnePlus headphones of the "HeyMelody" family over RFCOMM,
 * brand module of hibybudsd. Pure protocol code (bytes in, bytes out through struct bud).
 *
 * Facts from Gadgetbridge's OPPO support (codeberg.org/Freeyourgadget/Gadgetbridge, AGPL-3.0:
 * facts only, no code), in our own words in docs/HEADPHONES.md section 10.3. Verified there on
 * OPPO Enco Air, Enco Air2 and realme Buds T110; others of the family: battery only.
 *   Service UUID 0000079a-d102-11e1-9b23-00025b00a5a5 (SDP).
 *   Frame (little-endian): AA | length (bytes after these two) | 00 00 | command LE16 | seq |
 *          payload length LE16 | payload. No checksum.
 *   0106 battery request -> 8106 {status 0, count, n x {index 1 L / 2 R / 3 case, level | 0x80 charging}}
 *   0204 device info push {1 = battery (same layout), 2 = status}
 *   0105 firmware -> 8105 {0, ?, "part,type,version,..." (type 2 = firmware)}
 *   0108 touch config request {02 03 01} -> 8108 {0, ?, n x {side, type LE16, value}}
 *   0401 touch config set {01, side, type LE16, value} -> 8401;  0400 find {1 on / 0 off} -> 8400
 * Noise control: not documented for this family (not offered). */
#ifndef BUDS_OPPO_INC_C
#define BUDS_OPPO_INC_C
#include "buds_core.inc.c"

#define OP_UUID "0000079a-d102-11e1-9b23-00025b00a5a5"
enum { OPL_REALME = 1 };                          /* voice assistant value 04 instead of 03 */
#define OP_TAPS ((1u << BA_NONE) | (1u << BA_PLAYPAUSE) | (1u << BA_VOICE) | (1u << BA_PREV) | (1u << BA_NEXT) | (1u << BA_VOLUP) | (1u << BA_VOLDN))
#define OP_B (BC_BATT_LR | BC_BATT_CASE | BC_FW)
static const struct buds_model OPPO_MODELS[] = {
    { BB_OPPO, 2, "OPPO Enco Air2", "OPPO Enco Air2", BF_PODS, OP_B | BC_GEST | BC_FIND, 0, 0, 0, 0, { 0, OP_TAPS, OP_TAPS, OP_TAPS }, 0 },
    { BB_OPPO, 1, "OPPO Enco Air", "OPPO Enco Air", BF_PODS, OP_B | BC_GEST | BC_FIND, 0, 0, 0, 0, { 0, OP_TAPS, OP_TAPS, OP_TAPS }, 0 },
    { BB_OPPO, 3, "realme Buds T110", "realme Buds T110", BF_BUDS, OP_B | BC_GEST | BC_FIND, 0, 0, 0, 0, { 0, OP_TAPS, OP_TAPS, OP_TAPS }, OPL_REALME },
};
static const struct buds_model OPPO_GENERIC = { BB_OPPO, 0, "HeyMelody", 0, BF_BUDS, OP_B, 0, 0, 0, 0, { 0 }, 0 };
static const struct buds_model *op_identify(const struct bd_dev *d)
{
    const char *nm = d->name[0] ? d->name : d->alias;
    for (unsigned i = 0; i < sizeof OPPO_MODELS / sizeof *OPPO_MODELS; i++) if (!strcmp(nm, OPPO_MODELS[i].match)) return &OPPO_MODELS[i];
    return bd_has_uuid(d, OP_UUID) ? &OPPO_GENERIC : 0;
}
static const char *op_uuid(const struct bud *b) { (void)b; return OP_UUID; }
struct op_priv { uint8_t seq; };
static int op_frame(int cmd, int seq, const uint8_t *pl, int n, uint8_t *o, int cap)
{
    int tot = 9 + n;
    if (n < 0 || tot > cap || tot - 2 > 255) return -1;
    o[0] = 0xAA; o[1] = (uint8_t)(tot - 2); o[2] = 0; o[3] = 0;
    o[4] = (uint8_t)cmd; o[5] = (uint8_t)(cmd >> 8); o[6] = (uint8_t)seq; o[7] = (uint8_t)n; o[8] = (uint8_t)(n >> 8);
    if (n) memcpy(o + 9, pl, (size_t)n);
    return tot;
}
static int op_send(struct bud *b, int cmd, const uint8_t *pl, int n)
{
    struct op_priv *p = (struct op_priv *)b->priv.raw; uint8_t f[64];
    int k = op_frame(cmd, p->seq++, pl, n, f, sizeof f);
    if (k < 0 || !b->tx) return BE_IO;
    return b->tx(b, f, k) == k ? BE_OK : BE_IO;
}
static void op_start(struct bud *b)
{
    op_send(b, 0x0106, 0, 0);
    op_send(b, 0x0105, 0, 0);
    if (b->s.caps & BC_GEST) { static const uint8_t q[3] = { 0x02, 0x03, 0x01 }; op_send(b, 0x0108, q, 3); }
}
static int op_gesture(int type) { return type == 0x0201 ? BG_DOUBLE : type == 0x0301 ? BG_TRIPLE : type == 0x0401 ? BG_HOLD : -1; }
static int op_type(int g) { return g == BG_DOUBLE ? 0x0201 : g == BG_TRIPLE ? 0x0301 : g == BG_HOLD ? 0x0401 : -1; }
static int op_action(int v)
{
    switch (v) { case 0x00: return BA_NONE; case 0x01: return BA_PLAYPAUSE; case 0x03: case 0x04: return BA_VOICE; case 0x05: return BA_PREV;
                 case 0x06: return BA_NEXT; case 0x0B: return BA_VOLUP; case 0x0C: return BA_VOLDN; default: return -1; }
}
static int op_value(const struct bud *b, int a)
{
    switch (a) { case BA_NONE: return 0x00; case BA_PLAYPAUSE: return 0x01; case BA_VOICE: return b->m && (b->m->layout & OPL_REALME) ? 0x04 : 0x03;
                 case BA_PREV: return 0x05; case BA_NEXT: return 0x06; case BA_VOLUP: return 0x0B; case BA_VOLDN: return 0x0C; default: return -1; }
}
static void op_battery(struct buds_st *s, const uint8_t *d, int n)
{
    for (int i = 2; i + 1 < n; i += 2) {
        if (d[i] == 0xFF) continue;
        int k = d[i] - 1; if (k < 0 || k > 2) break;
        s->batt[k] = (d[i + 1] & 0x7F) <= 100 ? d[i + 1] & 0x7F : -1; s->chg[k] = (d[i + 1] & 0x80) != 0;
    }
}
static void op_msg(struct bud *b, int cmd, const uint8_t *d, int n)
{
    struct buds_st *s = &b->s;
    switch (cmd) {
    case 0x8106: if (n >= 2 && d[0] == 0) { op_battery(s, d, n); b->ready = 1; b->changed = 1; } break;
    case 0x0204: if (n >= 2 && d[0] == 1) { op_battery(s, d, n); b->ready = 1; b->changed = 1; } break;
    case 0x8105:
        if (n > 2 && d[0] == 0) {                  /* "part,type,version,..." : the firmware (type 2) parts */
            char txt[96], out[40] = ""; int k = 0;
            for (int i = 2; i < n && k < 95; i++) if (d[i]) txt[k++] = (char)d[i];
            txt[k] = 0;
            char *f[24]; int nf = 0; char *sv = 0;
            for (char *t = strtok_r(txt, ",", &sv); t && nf < 24; t = strtok_r(0, ",", &sv)) f[nf++] = t;
            for (int i = 0; i + 2 < nf; i += 3) if (!strcmp(f[i + 1], "2")) {
                char *v = f[i + 2]; while (*v == ' ') v++;
                if (out[0] && strlen(out) + strlen(v) + 2 < sizeof out) strcat(out, ".");
                if (strlen(out) + strlen(v) + 1 < sizeof out) strcat(out, v);
                if (strchr(v, '.')) break;
            }
            if (!out[0]) buds_clean(out, sizeof out, txt);
            buds_cpy(s->fw, sizeof s->fw, out); b->changed = 1;
        }
        break;
    case 0x8108:
        if (n >= 2 && d[0] == 0) for (int i = 2; i + 3 < n; i += 4) {
            int side = d[i] == 1 ? 0 : d[i] == 2 ? 1 : -1, g = op_gesture(d[i + 1] | d[i + 2] << 8), a = op_action(d[i + 3]);
            if (side >= 0 && g >= 0) { s->gest[side][g] = a; b->changed = 1; }
        }
        break;
    case 0x8401: case 0x8400: break;               /* acks */
    default: bud_unknown(b, (unsigned)cmd, d, n); break;
    }
    if (b->changed) s->conn = BS_READY;
}
static void op_rx(struct bud *b, const uint8_t *p, int n)
{
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    while (i < b->rn) {
        const uint8_t *f = b->rbuf + i; int left = b->rn - i;
        if (f[0] != 0xAA) { i++; continue; }
        if (left < 9) break;
        int tot = f[1] + 2, pl = f[7] | f[8] << 8;
        if (tot < 9 || pl != tot - 9 || (f[2] | f[3] << 8) > 4) { i++; continue; }   /* the 2nd word is 0 or 4 */
        if (left < tot) break;
        op_msg(b, f[4] | f[5] << 8, f + 9, pl);
        i += tot;
    }
    if (i > 0) { memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
}
static int op_cmd(struct bud *b, const struct bcmd *c)
{
    struct buds_st *s = &b->s; uint8_t pl[5]; int r;
    switch (c->op) {
    case BCMD_GEST: {
        if (!(s->caps & BC_GEST)) return BE_NOTSUP;
        if (c->a < 0 || c->a > 1 || c->b < 0 || c->b >= BG_N || c->c < 0 || c->c >= BA_N || !(s->gest_opts[c->b] & (1u << c->c))) return BE_RANGE;
        int ty = op_type(c->b), v = op_value(b, c->c); if (ty < 0 || v < 0) return BE_RANGE;
        pl[0] = 0x01; pl[1] = (uint8_t)(c->a + 1); pl[2] = (uint8_t)ty; pl[3] = (uint8_t)(ty >> 8); pl[4] = (uint8_t)v;
        if ((r = op_send(b, 0x0401, pl, 5)) == BE_OK) { s->gest[c->a][c->b] = c->c; b->changed = 1; }
        return r;
    }
    case BCMD_FIND:                                /* both buds at once on this family */
        if (!(s->caps & BC_FIND)) return BE_NOTSUP;
        pl[0] = c->b ? 1 : 0;
        if ((r = op_send(b, 0x0400, pl, 1)) == BE_OK) { s->find[0] = s->find[1] = !!c->b; b->changed = 1; }
        return r;
    default: return BE_NOTSUP;
    }
}
static const struct buds_brand BRAND_OPPO = { BB_OPPO, "oppo", op_identify, BT_RFCOMM, 0, op_uuid, op_start, op_rx, op_cmd, 0, 0, 0, 0 };
#endif
