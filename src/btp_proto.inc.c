/* btp_proto.inc.c - BTP/1 encoding rules (docs/SYNC_PROTOCOL.md sections 3, 4, 8, 9, 11, 16).
 * Pure functions, no I/O: frame headers, payload reader/writer, path rules, part-file names,
 * resume offsets, record nonces, the pairing/AUTH key schedules and the discovery packets.
 * Include after btp.h and btp_crypto.inc.c. */

/* ------------------------------------------------------------------ big-endian helpers */
static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

/* ------------------------------------------------------------------ payload reader */
struct brd { const uint8_t *p; size_t n, i; int bad; };
static void brd_init(struct brd *r, const uint8_t *p, size_t n) { r->p = p; r->n = n; r->i = 0; r->bad = 0; }
static const uint8_t *brd_take(struct brd *r, size_t k)
{
    if (r->bad || r->n - r->i < k) { r->bad = 1; return 0; }
    const uint8_t *q = r->p + r->i; r->i += k; return q;
}
static uint32_t brd_u8(struct brd *r) { const uint8_t *q = brd_take(r, 1); return q ? q[0] : 0; }
static uint32_t brd_u16(struct brd *r) { const uint8_t *q = brd_take(r, 2); return q ? rd16(q) : 0; }
static uint32_t brd_u32(struct brd *r) { const uint8_t *q = brd_take(r, 4); return q ? rd32(q) : 0; }
static uint64_t brd_u64(struct brd *r) { const uint8_t *q = brd_take(r, 8); return q ? rd64(q) : 0; }
static const uint8_t *brd_str16(struct brd *r, size_t *len)
{
    size_t l = brd_u16(r); const uint8_t *q = brd_take(r, l); *len = q ? l : 0; return q;
}
static int brd_done(const struct brd *r) { return !r->bad && r->i == r->n; }   /* exact length */

/* ------------------------------------------------------------------ payload writer */
struct bwr { uint8_t *p; size_t cap, n; int bad; };
static void bwr_init(struct bwr *w, uint8_t *p, size_t cap) { w->p = p; w->cap = cap; w->n = 0; w->bad = 0; }
static uint8_t *bwr_room(struct bwr *w, size_t k)
{
    if (w->bad || w->cap - w->n < k) { w->bad = 1; return 0; }
    uint8_t *q = w->p + w->n; w->n += k; return q;
}
static void bwr_u8(struct bwr *w, uint32_t v) { uint8_t *q = bwr_room(w, 1); if (q) q[0] = (uint8_t)v; }
static void bwr_u16(struct bwr *w, uint32_t v) { uint8_t *q = bwr_room(w, 2); if (q) be16(q, v); }
static void bwr_u32(struct bwr *w, uint32_t v) { uint8_t *q = bwr_room(w, 4); if (q) be32(q, v); }
static void bwr_u64(struct bwr *w, uint64_t v) { uint8_t *q = bwr_room(w, 8); if (q) be64(q, v); }
static void bwr_bytes(struct bwr *w, const void *d, size_t k) { uint8_t *q = bwr_room(w, k); if (q && k) memcpy(q, d, k); }
static void bwr_str16(struct bwr *w, const void *s, size_t k) { bwr_u16(w, (uint32_t)k); bwr_bytes(w, s, k); }
static void bwr_tlv(struct bwr *w, int tag, const void *v, size_t k) { bwr_u8(w, (uint32_t)tag); bwr_u16(w, (uint32_t)k); bwr_bytes(w, v, k); }

/* ------------------------------------------------------------------ frame header (3.1) */
static void btp_hdr_build(uint8_t h[BTP_HDR], int type, int flags, uint32_t stream, uint32_t len)
{
    h[0] = 'B'; h[1] = 'T'; h[2] = BTP_VERSION; h[3] = (uint8_t)type; h[4] = (uint8_t)flags; h[5] = 0;
    be16(h + 6, stream); be32(h + 8, len);
}
/* 0 or the error of the table in 3.1 (E_PROTO, E_VERSION, E_TOO_LARGE); unknown types are the
 * caller's business (the payload must be read and discarded) */
static int btp_hdr_parse(const uint8_t h[BTP_HDR], uint32_t max_payload, int *type, int *flags, uint32_t *stream, uint32_t *len)
{
    if (h[0] != 'B' || h[1] != 'T') return E_PROTO;
    if (h[2] != BTP_VERSION) return E_VERSION;
    if ((h[4] & ~BTP_FLAG_END) || h[5]) return E_PROTO;
    *type = h[3]; *flags = h[4]; *stream = rd16(h + 6); *len = rd32(h + 8);
    if (*len > max_payload) return E_TOO_LARGE;
    return 0;
}
static int btp_type_known(int t)
{
    switch (t) {
    case T_HELLO: case T_ERROR: case T_OK: case T_PING: case T_PONG: case T_BYE: case T_CANCEL:
    case T_PAIR_COMMIT: case T_PAIR_DEVICE: case T_PAIR_REVEAL: case T_PAIR_CONFIRM: case T_PAIR_RESULT:
    case T_AUTH_INIT: case T_AUTH_REPLY: case T_AUTH_FINISH: case T_AUTH_OK:
    case T_STAT: case T_STAT_R: case T_LIST: case T_LIST_R: case T_DF: case T_DF_R: case T_MKDIR: case T_MOVE: case T_DELETE:
    case T_PUT_BEGIN: case T_PUT_READY: case T_DATA: case T_PUT_END: case T_PUT_DONE: case T_GET: case T_GET_R: case T_GET_END:
    case T_DB_SNAPSHOT: case T_DB_STAGE: case T_DB_STATUS: case T_DB_STATUS_R: case T_DB_UNSTAGE: case T_LIB_RESCAN:
    case T_UPDATE_STAGE: case T_UPDATE_STAGE_R: case T_UPDATE_INSTALL: case T_UPDATE_STATUS: case T_UPDATE_STATUS_R:
    case T_STATUS: case T_STATUS_R: return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ UTF-8 and paths (4, 9.2) */
/* length of the UTF-8 sequence at s (strict: no overlongs, no surrogates, <= U+10FFFF), 0 = bad */
static int btp_utf8_seq(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint8_t c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int k; uint32_t v, min;
    if (c >= 0xC2 && c <= 0xDF) { k = 2; v = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 3; v = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 4; v = c & 0x07; min = 0x10000; }
    else return 0;
    if (n < (size_t)k) return 0;
    for (int i = 1; i < k; i++) { if ((s[i] & 0xC0) != 0x80) return 0; v = v << 6 | (s[i] & 0x3F); }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v; return k;
}
static int btp_utf8_ok(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; ) { uint32_t cp; int k = btp_utf8_seq(s + i, n - i, &cp); if (!k) return 0; i += (size_t)k; }
    return 1;
}
/* PR_OK or the first broken rule, in the order of the table in 9.2 */
static int btp_path_check(const uint8_t *p, size_t n, int allow_empty)
{
    if (n == 0) return allow_empty ? PR_OK : PR_EMPTY;
    if (n > BTP_PATH_MAX) return PR_TOO_LONG;
    if (!btp_utf8_ok(p, n)) return PR_BAD_UTF8;
    if (p[0] == '/') return PR_ABSOLUTE;
    int comps = 1; for (size_t i = 0; i < n; i++) if (p[i] == '/') comps++;
    if (comps > BTP_DEPTH_MAX) return PR_TOO_DEEP;
    size_t s = 0;
    while (s <= n) {
        size_t e = s; while (e < n && p[e] != '/') e++;
        size_t l = e - s; const uint8_t *c = p + s;
        if (l == 0) return PR_EMPTY_COMPONENT;
        if ((l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) return PR_DOT_COMPONENT;
        if (c[0] == '.') return PR_HIDDEN;
        for (size_t i = 0; i < l; i++) {
            if (c[i] < 0x20 || c[i] == 0x7F) return PR_CONTROL_CHAR;
            if (strchr("\\:*?\"<>|", c[i]) && c[i]) return PR_FORBIDDEN_CHAR;
        }
        if (c[l - 1] == ' ' || c[l - 1] == '.') return PR_TRAILING_DOT_SPACE;
        if (l > BTP_COMP_MAX) return PR_COMPONENT_TOO_LONG;
        s = e + 1;
    }
    return PR_OK;
}
static int btp_name_ok(const uint8_t *s, size_t n) { return n <= BTP_NAME_MAX && btp_utf8_ok(s, n); }

/* ------------------------------------------------------------------ PUT part file and resume (11.2) */
/* component name ".btp-<16 hex>.part" (24 chars + NUL) */
static void btp_part_name(const uint8_t *path, size_t n, const uint8_t sha[32], uint64_t size, char out[32])
{
    struct ota_sha256 s; uint8_t z = 0, sz[8], d[32];
    be64(sz, size);
    ota_sha256_init(&s); ota_sha256_update(&s, path, n); ota_sha256_update(&s, &z, 1);
    ota_sha256_update(&s, sha, 32); ota_sha256_update(&s, sz, 8); ota_sha256_final(&s, d);
    char hx[17]; ota_hex(d, 8, hx); hx[16] = 0;
    snprintf(out, 32, ".btp-%s.part", hx);
}
static uint64_t btp_resume_offset(uint64_t part_size, uint64_t size)
{
    uint64_t o = part_size / BTP_RESUME_BLOCK * BTP_RESUME_BLOCK;
    return o > size ? size : o;
}

/* ------------------------------------------------------------------ records (3.2) */
enum { DIR_C2D = 1, DIR_D2C = 2 };
static void btp_nonce(uint8_t n[24], int dir, uint64_t ctr) { memset(n, 0, 24); n[0] = (uint8_t)dir; be64(n + 16, ctr); }

/* ------------------------------------------------------------------ key schedules (8.2, 8.3) */
static void btp_pair_commit(const uint8_t Ce[32], const uint8_t Nc[16], uint8_t out[32])
{
    struct ota_sha256 s; ota_sha256_init(&s);
    ota_sha256_update(&s, "BTP/1 commit", 12); ota_sha256_update(&s, Ce, 32); ota_sha256_update(&s, Nc, 16);
    ota_sha256_final(&s, out);
}
/* h1 = H(hello_c payload), h2 = H(hello_d payload) */
static void btp_pair_th(const uint8_t h1[32], const uint8_t h2[32], const uint8_t commit[32],
                        const uint8_t De[32], const uint8_t Nd[16], const uint8_t Ce[32], const uint8_t Nc[16], uint8_t th[32])
{
    struct ota_sha256 s; ota_sha256_init(&s);
    ota_sha256_update(&s, "BTP/1 pair", 10); ota_sha256_update(&s, h1, 32); ota_sha256_update(&s, h2, 32);
    ota_sha256_update(&s, commit, 32); ota_sha256_update(&s, De, 32); ota_sha256_update(&s, Nd, 16);
    ota_sha256_update(&s, Ce, 32); ota_sha256_update(&s, Nc, 16); ota_sha256_final(&s, th);
}
static void btp_auth_th(const uint8_t h1[32], const uint8_t h2[32], const uint8_t key_id[16],
                        const uint8_t Ce[32], const uint8_t De[32], const uint8_t Cs[32], const uint8_t Ds[32], uint8_t th[32])
{
    struct ota_sha256 s; ota_sha256_init(&s);
    ota_sha256_update(&s, "BTP/1 auth", 10); ota_sha256_update(&s, h1, 32); ota_sha256_update(&s, h2, 32);
    ota_sha256_update(&s, key_id, 16); ota_sha256_update(&s, Ce, 32); ota_sha256_update(&s, De, 32);
    ota_sha256_update(&s, Cs, 32); ota_sha256_update(&s, Ds, 32); ota_sha256_final(&s, th);
}
static void btp_keys(const uint8_t prk[32], uint8_t k_c2d[32], uint8_t k_d2c[32])
{
    btp_hkdf_expand(prk, "BTP/1 c2d", 9, k_c2d, 32);
    btp_hkdf_expand(prk, "BTP/1 d2c", 9, k_d2c, 32);
}
/* 6 decimal digits + NUL */
static void btp_sas(const uint8_t prk[32], char out[7])
{
    uint8_t b[4]; btp_hkdf_expand(prk, "BTP/1 sas", 9, b, 4);
    snprintf(out, 7, "%06u", (unsigned)(rd32(b) % 1000000u));
    btp_wipe(b, sizeof b);
}
static void btp_key_id(const uint8_t Cs[32], uint8_t id[16]) { uint8_t h[32]; btp_sha256(Cs, 32, h); memcpy(id, h, 16); }
static void btp_device_id(const uint8_t Ds[32], uint8_t id[8]) { uint8_t h[32]; btp_sha256(Ds, 32, h); memcpy(id, h, 8); }

/* ------------------------------------------------------------------ discovery (16) */
/* 0 and the nonce for a valid PROBE; -1 otherwise */
static int btp_probe_parse(const uint8_t *p, size_t n, uint8_t nonce[8])
{
    if (n != 16 || memcmp(p, "BTPD", 4) || p[4] != 1 || p[5] != 1) return -1;
    memcpy(nonce, p + 8, 8); return 0;
}
static size_t btp_announce_build(uint8_t *out, size_t cap, int flags, const uint8_t nonce[8], int port,
                                 const uint8_t device_id[8], const char *name)
{
    struct bwr w; bwr_init(&w, out, cap);
    size_t nl = strlen(name); if (nl > 32) nl = 32;
    while (nl && !btp_utf8_ok((const uint8_t *)name, nl)) nl--;   /* never cut inside a character */
    static const uint8_t z[8];
    bwr_bytes(&w, "BTPD", 4); bwr_u8(&w, 1); bwr_u8(&w, 2); bwr_u16(&w, (uint32_t)flags);
    bwr_bytes(&w, nonce ? nonce : z, 8); bwr_u16(&w, (uint32_t)port); bwr_bytes(&w, device_id, 8);
    bwr_u8(&w, (uint32_t)nl); bwr_bytes(&w, name, nl);
    return w.bad ? 0 : w.n;
}

/* ------------------------------------------------------------------ HELLO (7) */
struct btp_hello_info {
    int flags; uint32_t caps; const char *software, *name, *byos_version, *firmware, *model; const uint8_t *device_id;
};
static size_t btp_hello_device(uint8_t *out, size_t cap, const struct btp_hello_info *h)
{
    struct bwr w; bwr_init(&w, out, cap);
    bwr_u8(&w, 1); bwr_u8(&w, 1); bwr_u8(&w, 1); bwr_u8(&w, (uint32_t)h->flags);
    bwr_u32(&w, BTP_MAX_PAYLOAD); bwr_u32(&w, h->caps);
    bwr_tlv(&w, TLV_SOFTWARE, h->software, strlen(h->software));
    bwr_tlv(&w, TLV_NAME, h->name, strlen(h->name));
    bwr_tlv(&w, TLV_DEVICE_ID, h->device_id, 8);
    if (h->byos_version && *h->byos_version) bwr_tlv(&w, TLV_BYOS_VERSION, h->byos_version, strlen(h->byos_version));
    if (h->firmware && *h->firmware) bwr_tlv(&w, TLV_FIRMWARE, h->firmware, strlen(h->firmware));
    bwr_tlv(&w, TLV_MODEL, h->model, strlen(h->model));
    return w.bad ? 0 : w.n;
}
/* client HELLO: 0 ok, else an error code. NAME copied (at most 64 bytes) into name[65]. */
static int btp_hello_parse(const uint8_t *p, size_t n, int *pmin, int *pmax, int *role, uint32_t *max_payload,
                           uint32_t *caps, char name[BTP_NAME_MAX + 1])
{
    struct brd r; brd_init(&r, p, n);
    *pmin = (int)brd_u8(&r); *pmax = (int)brd_u8(&r); *role = (int)brd_u8(&r); brd_u8(&r);
    *max_payload = brd_u32(&r); *caps = brd_u32(&r);
    if (r.bad) return E_PROTO;
    name[0] = 0;
    while (r.i < r.n) {
        uint32_t tag = brd_u8(&r); size_t l = brd_u16(&r); const uint8_t *v = brd_take(&r, l);
        if (!v) return E_PROTO;
        if (tag == TLV_NAME || tag == TLV_SOFTWARE) {
            if (!btp_utf8_ok(v, l)) return E_PROTO;
            if (tag == TLV_NAME) { if (l > BTP_NAME_MAX) return E_PROTO; memcpy(name, v, l); name[l] = 0; }
        }
    }
    return 0;
}
