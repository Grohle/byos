/* buds_fastpair.inc.c - generic module: Google Fast Pair "Message Stream" over RFCOMM.
 * Fallback for any headphones without a vendor module (most Nothing/CMF, recent Sony, JBL and
 * many Android-oriented buds expose it). Public specification (developers.google.com/nearby/
 * fast-pair, Message Stream + battery notification + device information); our own code.
 *
 * Service UUID df21fe2c-2515-4fdb-8886-f12c4d67927c. Message: group u8 | code u8 |
 * length u16 BIG-endian | data. The provider pushes on connect and on change:
 *   03/01 model id (3 bytes)   03/03 battery L, R, case (bit 7 charging, 0..100, 0x7F unknown)
 *   03/09 firmware (UTF-8)     FF/01 ack, FF/02 nak
 * Read-only: ringing (04/01) may need an account-key MAC on newer revisions [D], so it is
 * not offered. */
#ifndef BUDS_FASTPAIR_INC_C
#define BUDS_FASTPAIR_INC_C
#include "buds_core.inc.c"

#define FP_UUID "df21fe2c-2515-4fdb-8886-f12c4d67927c"
static const struct buds_model FP_BUDS = { BB_OTHER, 0, "", 0, BF_BUDS, BC_BATT_LR | BC_BATT_CASE | BC_FW, 0, 0, 0, 0, { 0 }, 0 };
static const struct buds_model FP_HEAD = { BB_OTHER, 0, "", 0, BF_HEAD, BC_BATT_LR | BC_BATT_CASE | BC_FW, 0, 0, 0, 0, { 0 }, 0 };
/* Google Pixel Buds: battery through this module (their own "Maestro" protocol - pigweed RPC
 * over HDLC on RFCOMM 25e97ff7-24ce-4c4c-8951-f764a708f7b5, documented by pbpctrl - is not
 * implemented yet: ANC / EQ pending, docs/HEADPHONES.md section 10.4) */
#define FP_PB (BC_BATT_LR | BC_BATT_CASE | BC_FW)
static const struct buds_model GOOGLE_MODELS[] = {
    { BB_GOOGLE, 4, "Google Pixel Buds Pro 2", "Pixel Buds Pro 2", BF_BUDS, FP_PB, 0, 0, 0, 0, { 0 }, 0 },
    { BB_GOOGLE, 3, "Google Pixel Buds Pro", "Pixel Buds Pro", BF_BUDS, FP_PB, 0, 0, 0, 0, { 0 }, 0 },
    { BB_GOOGLE, 2, "Google Pixel Buds A-Series", "Pixel Buds A", BF_BUDS, FP_PB, 0, 0, 0, 0, { 0 }, 0 },
    { BB_GOOGLE, 5, "Google Pixel Buds 2a", "Pixel Buds 2a", BF_BUDS, FP_PB, 0, 0, 0, 0, { 0 }, 0 },
    { BB_GOOGLE, 1, "Google Pixel Buds", "Pixel Buds", BF_BUDS, FP_PB, 0, 0, 0, 0, { 0 }, 0 },
};
static const struct buds_model *fp_identify(const struct bd_dev *d)
{
    if (!bd_has_uuid(d, FP_UUID)) return 0;
    const char *nm = d->name[0] ? d->name : d->alias;
    for (unsigned i = 0; i < sizeof GOOGLE_MODELS / sizeof *GOOGLE_MODELS; i++) if (strstr(nm, GOOGLE_MODELS[i].match)) return &GOOGLE_MODELS[i];
    return strstr(d->icon, "headphones") ? &FP_HEAD : &FP_BUDS;
}
static const char *fp_uuid(const struct bud *b) { (void)b; return FP_UUID; }
static void fp_start(struct bud *b) { (void)b; }          /* the provider speaks first */
static int fp_batt(int v, int *chg) { if ((v & 0x7F) == 0x7F || (v & 0x7F) > 100) { *chg = -1; return -1; } *chg = (v & 0x80) != 0; return v & 0x7F; }
static void fp_msg(struct bud *b, int g, int c, const uint8_t *d, int n)
{
    struct buds_st *s = &b->s;
    if (g == 0x03 && c == 0x03 && n >= 3) {
        for (int i = 0; i < 3; i++) s->batt[i] = fp_batt(d[i], &s->chg[i]);
        /* single unit (headphones): the case byte is unknown and L == R or one of them unknown */
        b->ready = 1; b->changed = 1;
    } else if (g == 0x03 && c == 0x01 && n >= 3) {
        snprintf(s->modelno, sizeof s->modelno, "%02X%02X%02X", d[0], d[1], d[2]); b->changed = 1;
    } else if (g == 0x03 && c == 0x09 && n >= 1) {
        char v[40]; int k = 0; for (int i = 0; i < n && k < 39; i++) if (d[i] >= 32 && d[i] < 127) v[k++] = (char)d[i];
        v[k] = 0; if (k) { buds_cpy(s->fw, sizeof s->fw, v); b->changed = 1; }
    } else if (g != 0xFF && !(g == 0x03 && (c == 0x04 || c == 0x06 || c == 0x0A))) bud_unknown(b, (unsigned)(g << 8 | c), d, n);
    if (b->changed) s->conn = BS_READY;
}
static void fp_rx(struct bud *b, const uint8_t *p, int n)
{
    if (n > (int)sizeof b->rbuf - b->rn) { b->rn = 0; if (n > (int)sizeof b->rbuf) return; }
    memcpy(b->rbuf + b->rn, p, (size_t)n); b->rn += n;
    int i = 0;
    while (b->rn - i >= 4) {
        int g = b->rbuf[i], c = b->rbuf[i + 1], len = b->rbuf[i + 2] << 8 | b->rbuf[i + 3];
        if (len > 512) { bud_log(b, "fastpair: bad length, resync"); i = b->rn; break; }   /* no start byte: drop all */
        if (b->rn - i < 4 + len) break;
        fp_msg(b, g, c, b->rbuf + i + 4, len);
        i += 4 + len;
    }
    if (i > 0) { memmove(b->rbuf, b->rbuf + i, (size_t)(b->rn - i)); b->rn -= i; }
}
static int fp_cmd(struct bud *b, const struct bcmd *c) { (void)b; (void)c; return BE_NOTSUP; }
static const struct buds_brand BRAND_FASTPAIR = { BB_OTHER, "fastpair", fp_identify, BT_RFCOMM, 0, fp_uuid, fp_start, fp_rx, fp_cmd, 0, 0, 0, 0 };
#endif
