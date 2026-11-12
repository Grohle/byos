/* btp_test.c - PC tests (x86, WSL gcc) of hibysync and the BTP/1 code (docs/SYNC_PROTOCOL.md).
 * Built and run by tests/btp/btp_test.sh. Never touches a device.
 *
 *   btp_test vectors VECTORS.json
 *       Every entry of tests/btp/vectors.json against the C code of src/btp_*.inc.c and
 *       src/hibysync.c: constants, primitives (X25519, HSalsa20, Poly1305, secretbox, HKDF),
 *       all frames (header + decoded fields), invalid frames, path rules, part-file names,
 *       resume offsets, discovery packets, the full pairing and AUTH transcripts (keys, SAS,
 *       records byte for byte, tampered record), SAS examples, and the device HELLO builder.
 *
 *   btp_test daemon HIBYSYNC_X86 WORKDIR
 *       Runs the real daemon (x86 test build) with a temporary folder as SD root and staging
 *       area, and a socketpair as the shell's control pipe, then talks BTP/1 to it over TCP:
 *       HELLO, DF, STAT, LIST (several pages, hidden), MKDIR, PUT (create/replace/skip,
 *       resume after a dropped connection, bad hash, bad offset), GET (sha, resume offset,
 *       sendfile path, cancel), MOVE, DELETE, symlink and '..' confinement, SD unmounted and
 *       USB-storage refusal, NS_UPDATE/NS_DB name rules, DB stage/snapshot with a stub hibytag,
 *       unknown type, busy third session, Wi-Fi session: auth required, pairing with the SAS
 *       read from the control pipe, AUTH reconnect with encrypted PUT/GET, unknown key,
 *       tampered record, UDP discovery, QUIT and control-pipe EOF. Also: CANCEL behind a pipelined
 *       request and CANCEL of a queued one, .part takeover between two sessions, skip-identical
 *       as a hash job, one unauthenticated Wi-Fi session at a time, USB eviction, the securing
 *       deadline (PINGs do not extend it), unrevealed PAIR_COMMIT, and a 5 s blocking helper
 *       that must not time out the other sessions (separate run with 3 s timeouts). */
#define BTP_TEST 1
#define BTP_NO_MAIN 1
#include "../../src/hibysync.c"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ================================================================== tiny JSON reader */
enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ };
struct jv { int t; double num; char *s; size_t sl; struct jv **v; char **k; int n; };
static const char *jp;
static void jws(void) { while (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t') jp++; }
static void jutf8(char **o, unsigned cp)
{
    char *p = *o;
    if (cp < 0x80) *p++ = (char)cp;
    else if (cp < 0x800) { *p++ = (char)(0xC0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { *p++ = (char)(0xE0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
    else { *p++ = (char)(0xF0 | cp >> 18); *p++ = (char)(0x80 | ((cp >> 12) & 63)); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
    *o = p;
}
static char *jstring(size_t *len)
{
    jp++; const char *st = jp; size_t cap = 0; while (st[cap] && st[cap] != '"') cap += st[cap] == '\\' ? 2 : 1;
    char *out = malloc(cap * 2 + 8), *o = out;
    while (*jp != '"') {
        if (*jp != '\\') { *o++ = *jp++; continue; }
        jp++;
        switch (*jp) {
        case 'n': *o++ = '\n'; jp++; break; case 't': *o++ = '\t'; jp++; break; case 'r': *o++ = '\r'; jp++; break;
        case 'b': *o++ = '\b'; jp++; break; case 'f': *o++ = '\f'; jp++; break;
        case 'u': {
            unsigned cp = (unsigned)strtol((char[]){ jp[1], jp[2], jp[3], jp[4], 0 }, 0, 16); jp += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && jp[0] == '\\' && jp[1] == 'u') {
                unsigned lo = (unsigned)strtol((char[]){ jp[2], jp[3], jp[4], jp[5], 0 }, 0, 16); jp += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            jutf8(&o, cp); break;
        }
        default: *o++ = *jp++;
        }
    }
    jp++; *o = 0; *len = (size_t)(o - out); return out;
}
static struct jv *jparse(void)
{
    jws(); struct jv *v = calloc(1, sizeof *v);
    if (*jp == '{' || *jp == '[') {
        int obj = *jp == '{'; v->t = obj ? JOBJ : JARR; jp++; int cap = 0;
        for (;;) {
            jws(); if (*jp == (obj ? '}' : ']')) { jp++; break; }
            if (v->n == cap) { cap = cap ? cap * 2 : 8; v->v = realloc(v->v, sizeof *v->v * cap); v->k = realloc(v->k, sizeof *v->k * cap); }
            v->k[v->n] = 0;
            if (obj) { size_t l; v->k[v->n] = jstring(&l); jws(); jp++; }
            v->v[v->n++] = jparse(); jws(); if (*jp == ',') jp++;
        }
    } else if (*jp == '"') { v->t = JSTR; v->s = jstring(&v->sl); }
    else if (!strncmp(jp, "true", 4)) { v->t = JBOOL; v->num = 1; jp += 4; }
    else if (!strncmp(jp, "false", 5)) { v->t = JBOOL; jp += 5; }
    else if (!strncmp(jp, "null", 4)) { v->t = JNULL; jp += 4; }
    else { v->t = JNUM; char *e; v->num = strtod(jp, &e); jp = e; }
    return v;
}
static struct jv *jget(const struct jv *o, const char *k) { if (!o || o->t != JOBJ) return 0; for (int i = 0; i < o->n; i++) if (!strcmp(o->k[i], k)) return o->v[i]; return 0; }
static const char *jstr(const struct jv *o, const char *k) { struct jv *v = jget(o, k); return v && v->t == JSTR ? v->s : ""; }
static long long jnum(const struct jv *o, const char *k) { struct jv *v = jget(o, k); return v ? (long long)v->num : -1; }
static uint8_t *unhex(const char *h, size_t *n)
{
    size_t l = strlen(h) / 2; uint8_t *b = malloc(l + 1);
    for (size_t i = 0; i < l; i++) { char t[3] = { h[2 * i], h[2 * i + 1], 0 }; b[i] = (uint8_t)strtol(t, 0, 16); }
    *n = l; return b;
}
static uint8_t *jhex(const struct jv *o, const char *k, size_t *n) { return unhex(jstr(o, k), n); }
static int eqhex(const uint8_t *b, size_t n, const char *hex)
{
    static char t[300000]; if (n * 2 + 1 > sizeof t) return 0;
    ota_hex(b, (int)n, t); return !strcmp(t, hex);
}

/* ================================================================== vectors */
static int type_by_name(const char *n)
{
    static const struct { const char *n; int v; } T[] = {
        {"HELLO",T_HELLO},{"ERROR",T_ERROR},{"OK",T_OK},{"PING",T_PING},{"PONG",T_PONG},{"BYE",T_BYE},{"CANCEL",T_CANCEL},
        {"PAIR_COMMIT",T_PAIR_COMMIT},{"PAIR_DEVICE",T_PAIR_DEVICE},{"PAIR_REVEAL",T_PAIR_REVEAL},{"PAIR_CONFIRM",T_PAIR_CONFIRM},
        {"PAIR_RESULT",T_PAIR_RESULT},{"AUTH_INIT",T_AUTH_INIT},{"AUTH_REPLY",T_AUTH_REPLY},{"AUTH_FINISH",T_AUTH_FINISH},{"AUTH_OK",T_AUTH_OK},
        {"STAT",T_STAT},{"STAT_R",T_STAT_R},{"LIST",T_LIST},{"LIST_R",T_LIST_R},{"DF",T_DF},{"DF_R",T_DF_R},{"MKDIR",T_MKDIR},{"MOVE",T_MOVE},
        {"DELETE",T_DELETE},{"PUT_BEGIN",T_PUT_BEGIN},{"PUT_READY",T_PUT_READY},{"DATA",T_DATA},{"PUT_END",T_PUT_END},{"PUT_DONE",T_PUT_DONE},
        {"GET",T_GET},{"GET_R",T_GET_R},{"GET_END",T_GET_END},{"DB_SNAPSHOT",T_DB_SNAPSHOT},{"DB_STAGE",T_DB_STAGE},{"DB_STATUS",T_DB_STATUS},
        {"DB_STATUS_R",T_DB_STATUS_R},{"DB_UNSTAGE",T_DB_UNSTAGE},{"LIB_RESCAN",T_LIB_RESCAN},{"UPDATE_STAGE",T_UPDATE_STAGE},
        {"UPDATE_STAGE_R",T_UPDATE_STAGE_R},{"UPDATE_INSTALL",T_UPDATE_INSTALL},{"UPDATE_STATUS",T_UPDATE_STATUS},
        {"UPDATE_STATUS_R",T_UPDATE_STATUS_R},{"STATUS",T_STATUS},{"STATUS_R",T_STATUS_R} };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, n)) return T[i].v;
    return -1;
}
static int err_by_name(const char *n)
{
    static const struct { const char *n; int v; } E[] = {
        {"E_PROTO",E_PROTO},{"E_VERSION",E_VERSION},{"E_UNKNOWN_TYPE",E_UNKNOWN_TYPE},{"E_AUTH_REQUIRED",E_AUTH_REQUIRED},
        {"E_AUTH_FAILED",E_AUTH_FAILED},{"E_PAIR_CLOSED",E_PAIR_CLOSED},{"E_PAIR_REJECTED",E_PAIR_REJECTED},{"E_BUSY",E_BUSY},
        {"E_TOO_LARGE",E_TOO_LARGE},{"E_RATE_LIMIT",E_RATE_LIMIT},{"E_TIMEOUT",E_TIMEOUT},{"E_CANCELLED",E_CANCELLED},
        {"E_UNSUPPORTED",E_UNSUPPORTED},{"E_INTERNAL",E_INTERNAL},{"E_BAD_PATH",E_BAD_PATH},{"E_NOT_FOUND",E_NOT_FOUND},
        {"E_EXISTS",E_EXISTS},{"E_NOT_DIR",E_NOT_DIR},{"E_IS_DIR",E_IS_DIR},{"E_NOT_EMPTY",E_NOT_EMPTY},{"E_NO_SPACE",E_NO_SPACE},
        {"E_IO",E_IO},{"E_SD_UNAVAILABLE",E_SD_UNAVAILABLE},{"E_HASH_MISMATCH",E_HASH_MISMATCH},{"E_OFFSET",E_OFFSET},
        {"E_FILE_TOO_BIG",E_FILE_TOO_BIG},{"E_NOT_ALLOWED",E_NOT_ALLOWED},{"E_SIGNATURE",E_SIGNATURE},{"E_UPDATE_INVALID",E_UPDATE_INVALID},
        {"E_DOWNGRADE",E_DOWNGRADE},{"E_BATTERY",E_BATTERY},{"E_NEEDS_FIRMWARE",E_NEEDS_FIRMWARE},{"E_NOT_STAGED",E_NOT_STAGED},
        {"E_DB_INVALID",E_DB_INVALID},{"E_DB_STALE",E_DB_STALE} };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++) if (!strcmp(E[i].n, n)) return E[i].v;
    return -1;
}
/* payload layouts: "u8 a,u16 b,u32 c,u64 d,i64 e,b32 f,b16 g,b8 h,s16 i" ('*' = TLV/data tail) */
static const char *layout(int t)
{
    switch (t) {
    case T_HELLO: return "u8 proto_min,u8 proto_max,u8 role,u8 flags,u32 max_payload,u32 caps,*";
    case T_ERROR: return "u16 code,s16 message";
    case T_OK: case T_BYE: case T_CANCEL: case T_DF: case T_PUT_END: case T_DB_SNAPSHOT: case T_DB_STATUS: case T_DB_UNSTAGE:
    case T_UPDATE_STATUS: case T_STATUS: case T_AUTH_FINISH: case T_AUTH_OK: return "";
    case T_PING: case T_PONG: return "b8 bytes";
    case T_PAIR_COMMIT: return "b32 commit";
    case T_PAIR_DEVICE: return "b32 De,b16 Nd";
    case T_PAIR_REVEAL: return "b32 Ce,b16 Nc";
    case T_PAIR_CONFIRM: return "b32 Cs,s16 client_name";
    case T_PAIR_RESULT: return "u8 status,b32 Ds,b8 device_id,s16 device_name";
    case T_AUTH_INIT: return "b16 key_id,b32 Ce";
    case T_AUTH_REPLY: return "b32 De";
    case T_STAT: return "u8 ns,s16 path";
    case T_STAT_R: return "u8 kind,u64 size,i64 mtime";
    case T_LIST: case T_MKDIR: case T_DELETE: return "u8 ns,u8 flags,s16 path";
    case T_LIST_R: return "u16 count,*";
    case T_DF_R: return "u8 sd_state,u8 sd_fs,u64 sd_total,u64 sd_free,u64 data_total,u64 data_free";
    case T_MOVE: return "u8 ns,u8 flags,s16 src,s16 dst";
    case T_PUT_BEGIN: return "u8 ns,u8 mode,u64 size,i64 mtime,b32 sha256,s16 path";
    case T_PUT_READY: return "u64 offset";
    case T_DATA: return "u64 offset,*";
    case T_PUT_DONE: return "u8 result,u64 size";
    case T_GET: return "u8 ns,u8 flags,u64 offset,s16 path";
    case T_GET_R: return "u64 size,i64 mtime,u64 offset";
    case T_GET_END: return "b32 sha256";
    case T_DB_STAGE: return "u8 flags,b32 base_sha256";
    case T_DB_STATUS_R: return "u8 staged,b32 staged_sha256,b32 live_sha256,u64 live_size,u8 last_apply,u64 data_free";
    case T_LIB_RESCAN: return "u8 mode,u16 count,*";
    case T_UPDATE_STAGE: case T_UPDATE_INSTALL: return "u8 flags";
    case T_UPDATE_STAGE_R: return "s16 version,u8 restart,u8 needs_firmware,u64 size";
    case T_UPDATE_STATUS_R: return "u8 state,u16 permille,u16 error,s16 version";
    case T_STATUS_R: return "u8 battery,u8 charging,u8 sd_state,*";
    }
    return 0;
}
/* decodes payload p by the layout of t and compares with the JSON fields present; returns
 * the reader position of the tail ('*') or -1 */
static long check_fields(const char *name, int t, const uint8_t *p, size_t n, const struct jv *f)
{
    const char *lay = layout(t);
    if (!lay) { CHECK(0, "%s: no layout for type %d", name, t); return -1; }
    struct brd r; brd_init(&r, p, n);
    char buf[256]; snprintf(buf, sizeof buf, "%s", lay);
    int tail = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(0, ",")) {
        if (!strcmp(tok, "*")) { tail = 1; break; }
        char kind[8], fname[64]; sscanf(tok, "%7s %63s", kind, fname);
        struct jv *jf = jget(f, fname);
        if (kind[0] == 'u' || kind[0] == 'i') {
            unsigned long long v = !strcmp(kind, "u8") ? brd_u8(&r) : !strcmp(kind, "u16") ? brd_u16(&r) : !strcmp(kind, "u32") ? brd_u32(&r) : brd_u64(&r);
            if (jf) CHECK(jf->t == JNUM && (kind[0] == 'i' ? (long long)v == (long long)jf->num : v == (unsigned long long)jf->num), "%s: field %s = %llu", name, fname, v);
        } else if (kind[0] == 'b') {
            size_t k = (size_t)atoi(kind + 1); const uint8_t *q = brd_take(&r, k);
            if (jf && q) CHECK(eqhex(q, k, jf->s), "%s: field %s", name, fname);
        } else {
            size_t l; const uint8_t *q = brd_str16(&r, &l);
            if (jf && q) CHECK(l == jf->sl && !memcmp(q, jf->s, l), "%s: string %s", name, fname);
        }
    }
    CHECK(!r.bad, "%s: payload too short", name);
    if (!tail) CHECK(brd_done(&r), "%s: payload length", name);
    return r.bad ? -1 : (long)r.i;
}
static void check_frame_vector(const struct jv *fr, const char *dirname)
{
    const char *name = jstr(fr, "name"); int t = type_by_name(jstr(fr, "type"));
    CHECK(t == jnum(fr, "type_code"), "%s%s: type code", dirname, name);
    CHECK(btp_type_known(t), "%s: known type", name);
    size_t fl, pl; uint8_t *f = jhex(fr, "frame", &fl), *p = jhex(fr, "payload", &pl);
    int type, flags; uint32_t stream, len;
    CHECK(fl >= BTP_HDR && !btp_hdr_parse(f, BTP_MAX_PAYLOAD, &type, &flags, &stream, &len), "%s: header parses", name);
    CHECK(type == t && flags == jnum(fr, "flags") && stream == (uint32_t)jnum(fr, "stream") && len == pl && fl == BTP_HDR + pl && !memcmp(f + BTP_HDR, p, pl), "%s: header fields", name);
    uint8_t h[BTP_HDR]; btp_hdr_build(h, t, (int)jnum(fr, "flags"), (uint32_t)jnum(fr, "stream"), (uint32_t)pl);
    CHECK(!memcmp(h, f, BTP_HDR), "%s: header built", name);
    struct jv *fields = jget(fr, "fields");
    long tail = check_fields(name, t, p, pl, fields);
    if (tail >= 0 && t == T_HELLO) {
        int a, b, c; uint32_t mp, caps; char nm[BTP_NAME_MAX + 1];
        CHECK(!btp_hello_parse(p, pl, &a, &b, &c, &mp, &caps, nm), "%s: hello parses", name);
        if (jget(fields, "name")) CHECK(!strcmp(nm, jstr(fields, "name")), "%s: hello name", name);
        if (jget(fields, "device_id")) {
            struct brd r; brd_init(&r, p + tail, pl - (size_t)tail); int found = 0;
            while (r.i < r.n) { uint32_t tg = brd_u8(&r); size_t l = brd_u16(&r); const uint8_t *v = brd_take(&r, l); if (tg == TLV_DEVICE_ID && v) found = eqhex(v, l, jstr(fields, "device_id")); }
            CHECK(found, "%s: device_id TLV", name);
        }
    }
    if (tail >= 0 && t == T_LIST_R) {
        struct jv *ents = jget(fields, "entries"); struct brd r; brd_init(&r, p + tail, pl - (size_t)tail);
        for (int i = 0; ents && i < ents->n; i++) {
            struct jv *e = ents->v[i]; uint32_t k = brd_u8(&r); uint64_t sz = brd_u64(&r); int64_t mt = (int64_t)brd_u64(&r); size_t l; const uint8_t *nm = brd_str16(&r, &l);
            CHECK(nm && k == (uint32_t)jnum(e, "kind") && sz == (uint64_t)jnum(e, "size") && mt == jnum(e, "mtime") && l == strlen(jstr(e, "name")) && !memcmp(nm, jstr(e, "name"), l), "%s: entry %d", name, i);
        }
        CHECK(brd_done(&r), "%s: entries length", name);
    }
    if (tail >= 0 && t == T_LIB_RESCAN) {
        struct jv *ps = jget(fields, "paths"); struct brd r; brd_init(&r, p + tail, pl - (size_t)tail);
        for (int i = 0; ps && i < ps->n; i++) { size_t l; const uint8_t *q = brd_str16(&r, &l); CHECK(q && l == ps->v[i]->sl && !memcmp(q, ps->v[i]->s, l), "%s: path %d", name, i); }
        CHECK(brd_done(&r), "%s: paths length", name);
    }
    if (tail >= 0 && t == T_DATA) CHECK(pl - 8 == (size_t)jnum(fields, "len"), "%s: data len", name);
    if (tail >= 0 && t == T_STATUS_R) {
        struct brd r; brd_init(&r, p + tail, pl - (size_t)tail); char bv[32] = "", fw[32] = "";
        while (r.i < r.n) { uint32_t tg = brd_u8(&r); size_t l = brd_u16(&r); const uint8_t *v = brd_take(&r, l); if (!v || l > 31) break; if (tg == TLV_BYOS_VERSION) memcpy(bv, v, l); if (tg == TLV_FIRMWARE) memcpy(fw, v, l); }
        CHECK(brd_done(&r) && !strcmp(bv, jstr(fields, "byos_version")) && !strcmp(fw, jstr(fields, "firmware")), "%s: status TLVs", name);
    }
    free(f); free(p);
}
static void check_records(const struct jv *arr, const uint8_t kc2d[32], const uint8_t kd2c[32], const char *what)
{
    for (int i = 0; arr && i < arr->n; i++) {
        struct jv *e = arr->v[i]; const char *name = jstr(e, "name");
        int dir = !strcmp(jstr(e, "dir"), "c2d") ? DIR_C2D : DIR_D2C; uint64_t ctr = (uint64_t)jnum(e, "counter");
        uint8_t nonce[24]; btp_nonce(nonce, dir, ctr);
        CHECK(eqhex(nonce, 24, jstr(e, "nonce")), "%s %s: nonce", what, name);
        size_t fl, rl; uint8_t *fr = jhex(e, "inner_frame", &fl), *rec = jhex(e, "record", &rl);
        uint8_t *buf = malloc(BTP_REC_HDR + fl);
        memcpy(buf + BTP_REC_HDR, fr, fl);
        btp_box_seal(buf + 4, buf + BTP_REC_HDR, fl, nonce, dir == DIR_C2D ? kc2d : kd2c);
        be32(buf, (uint32_t)(BTP_TAG + fl));
        CHECK(rl == BTP_REC_HDR + fl && !memcmp(buf, rec, rl), "%s %s: record sealed byte for byte", what, name);
        CHECK(!btp_box_open(rec + 4, rec + BTP_REC_HDR, rl - BTP_REC_HDR, nonce, dir == DIR_C2D ? kc2d : kd2c) && !memcmp(rec + BTP_REC_HDR, fr, fl), "%s %s: record opens", what, name);
        int type, flags; uint32_t st, len;
        CHECK(!btp_hdr_parse(fr, BTP_MAX_PAYLOAD, &type, &flags, &st, &len) && BTP_HDR + len == fl, "%s %s: inner frame", what, name);
        /* the same record with the wrong counter or the other key must not open */
        memcpy(rec + BTP_REC_HDR, buf + BTP_REC_HDR, fl);
        uint8_t n2[24]; btp_nonce(n2, dir, ctr + 1);
        CHECK(btp_box_open(rec + 4, rec + BTP_REC_HDR, rl - BTP_REC_HDR, n2, dir == DIR_C2D ? kc2d : kd2c), "%s %s: replay with another counter rejected", what, name);
        free(fr); free(rec); free(buf);
    }
}
static void vectors(const char *path)
{
    size_t n; char *txt = ota_read_file(path, 4 << 20, &n);
    if (!txt) { CHECK(0, "cannot read %s", path); return; }
    jp = txt; struct jv *V = jparse();
    CHECK(jnum(V, "format") == 1 && !strcmp(jstr(V, "protocol"), "BTP/1"), "format/protocol");

    /* constants */
    struct jv *C = jget(V, "constants"), *o;
    CHECK(jnum(C, "header_len") == BTP_HDR && jnum(C, "max_payload") == BTP_MAX_PAYLOAD && jnum(C, "max_data") == BTP_MAX_DATA &&
          jnum(C, "tag_len") == BTP_TAG && jnum(C, "max_record") == BTP_MAX_RECORD && jnum(C, "tcp_port") == BTP_PORT &&
          jnum(C, "udp_announce_port") == BTP_ANNOUNCE_PORT, "constants");
    o = jget(C, "types"); for (int i = 0; o && i < o->n; i++) CHECK(type_by_name(o->k[i]) == (int)o->v[i]->num && btp_type_known((int)o->v[i]->num), "type %s", o->k[i]);
    CHECK(o && o->n == 46, "46 message types");
    o = jget(C, "errors"); for (int i = 0; o && i < o->n; i++) CHECK(err_by_name(o->k[i]) == (int)o->v[i]->num, "error %s", o->k[i]);
    CHECK(o && o->n == 35, "35 error codes");
    o = jget(C, "caps");
    CHECK(jnum(o, "FILES") == CAP_FILES && jnum(o, "RESUME") == CAP_RESUME && jnum(o, "DB_SNAPSHOT") == CAP_DB_SNAPSHOT && jnum(o, "DB_STAGE") == CAP_DB_STAGE &&
          jnum(o, "LIB_RESCAN") == CAP_LIB_RESCAN && jnum(o, "UPDATE") == CAP_UPDATE && jnum(o, "PAIRING") == CAP_PAIRING &&
          jnum(o, "ENCRYPTION") == CAP_ENCRYPTION && jnum(o, "STATUS") == CAP_STATUS, "caps");
    o = jget(C, "hello_flags");
    CHECK(jnum(o, "LOCAL") == HF_LOCAL && jnum(o, "PAIRING_OPEN") == HF_PAIRING_OPEN && jnum(o, "SD_MOUNTED") == HF_SD_MOUNTED && jnum(o, "SHELL_ATTACHED") == HF_SHELL_ATTACHED, "hello flags");
    o = jget(C, "hello_tlv");
    CHECK(jnum(o, "SOFTWARE") == TLV_SOFTWARE && jnum(o, "NAME") == TLV_NAME && jnum(o, "DEVICE_ID") == TLV_DEVICE_ID && jnum(o, "BYOS_VERSION") == TLV_BYOS_VERSION &&
          jnum(o, "FIRMWARE") == TLV_FIRMWARE && jnum(o, "MODEL") == TLV_MODEL, "hello TLVs");

    /* primitives */
    struct jv *P = jget(V, "primitives"), *a;
    a = jget(P, "x25519");
    for (int i = 0; a && i < a->n; i++) { size_t l; uint8_t *sc = jhex(a->v[i], "scalar", &l), *u = jhex(a->v[i], "u", &l), out[32]; btp_x25519(out, sc, u); CHECK(eqhex(out, 32, jstr(a->v[i], "out")), "x25519 %s", jstr(a->v[i], "source")); free(sc); free(u); }
    { /* RFC 7748 6.1: both public keys from the secrets, and the shared secret */
        uint8_t as[32], bs[32], ap[32], bp[32], s1[32], s2[32]; size_t l; uint8_t *t;
        t = unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", &l); memcpy(as, t, 32); free(t);
        t = unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", &l); memcpy(bs, t, 32); free(t);
        btp_x25519_base(ap, as); btp_x25519_base(bp, bs);
        CHECK(eqhex(ap, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a") && eqhex(bp, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), "x25519 base points (RFC 7748 6.1)");
        btp_x25519_dh(s1, as, bp); btp_x25519_dh(s2, bs, ap);
        CHECK(!memcmp(s1, s2, 32) && eqhex(s1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"), "x25519 shared secret");
        uint8_t zero[32] = { 0 }, o2[32]; CHECK(btp_x25519_dh(o2, as, zero) == -1, "x25519 low-order point rejected");
    }
    a = jget(P, "hsalsa20");
    for (int i = 0; a && i < a->n; i++) { size_t l; uint8_t *k = jhex(a->v[i], "key", &l), *in = jhex(a->v[i], "in", &l), out[32]; btp_hsalsa20(out, k, in); CHECK(eqhex(out, 32, jstr(a->v[i], "out")), "hsalsa20"); free(k); free(in); }
    a = jget(P, "poly1305");
    for (int i = 0; a && i < a->n; i++) { size_t l, ml; uint8_t *k = jhex(a->v[i], "key", &l), *m = jhex(a->v[i], "msg", &ml), t[16]; btp_poly1305(t, m, ml, k); CHECK(eqhex(t, 16, jstr(a->v[i], "tag")), "poly1305 %s", jstr(a->v[i], "source")); free(k); free(m); }
    a = jget(P, "secretbox");
    for (int i = 0; a && i < a->n; i++) {
        size_t l, ml, bl; uint8_t *k = jhex(a->v[i], "key", &l), *nn = jhex(a->v[i], "nonce", &l), *m = jhex(a->v[i], "msg", &ml), *bx = jhex(a->v[i], "box", &bl);
        uint8_t *buf = malloc(16 + ml + 1); memcpy(buf + 16, m, ml);
        btp_box_seal(buf, buf + 16, ml, nn, k);
        CHECK(bl == 16 + ml && !memcmp(buf, bx, bl), "secretbox %d seal (%zu bytes)", i, ml);
        CHECK(!btp_box_open(bx, bx + 16, ml, nn, k) && !memcmp(bx + 16, m, ml), "secretbox %d open", i);
        if (ml) { buf[16] ^= 1; CHECK(btp_box_open(buf, buf + 16, ml, nn, k) == -1, "secretbox %d forged ciphertext rejected", i); }
        buf[0] ^= 1; CHECK(btp_box_open(buf, buf + 16, ml, nn, k) == -1, "secretbox %d forged tag rejected", i);
        free(k); free(nn); free(m); free(bx); free(buf);
    }
    a = jget(P, "hkdf_sha256");
    for (int i = 0; a && i < a->n; i++) {
        size_t il, sl, nl; uint8_t *ikm = jhex(a->v[i], "ikm", &il), *salt = jhex(a->v[i], "salt", &sl), *info = jhex(a->v[i], "info", &nl), prk[32], okm[100];
        btp_hkdf_extract(salt, sl, ikm, il, prk); btp_hkdf_expand(prk, info, nl, okm, (size_t)jnum(a->v[i], "len"));
        CHECK(eqhex(okm, (size_t)jnum(a->v[i], "len"), jstr(a->v[i], "okm")), "hkdf %s", jstr(a->v[i], "source"));
        free(ikm); free(salt); free(info);
    }
    { /* HMAC-SHA256 RFC 4231 test case 2 */
        uint8_t m[32]; struct btp_hmac h; btp_hmac_init(&h, (const uint8_t *)"Jefe", 4); btp_hmac_update(&h, "what do ya want for nothing?", 28); btp_hmac_final(&h, m);
        CHECK(eqhex(m, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac-sha256 RFC 4231 #2");
    }

    /* frames */
    struct jv *F = jget(V, "frames"); int nf = 0, types_seen[256] = { 0 };
    for (int i = 0; F && i < F->n; i++) {
        struct jv *fr = F->v[i];
        if (jget(fr, "frame_sha256")) {   /* the full 64 KiB DATA frame */
            size_t hl; uint8_t *head = jhex(fr, "frame_head", &hl);
            uint8_t *buf = malloc(BTP_HDR + 8 + 65536); struct jv *ff = jget(fr, "fields");
            uint64_t off = (uint64_t)jnum(ff, "offset"); size_t dl = (size_t)jnum(ff, "len");
            btp_hdr_build(buf, T_DATA, (int)jnum(fr, "flags"), (uint32_t)jnum(fr, "stream"), (uint32_t)(8 + dl)); be64(buf + BTP_HDR, off);
            for (size_t k = 0; k < dl; k++) buf[BTP_HDR + 8 + k] = (uint8_t)((off + k) * 7 + 3);
            uint8_t d[32]; btp_sha256(buf, BTP_HDR + 8 + dl, d);
            CHECK((size_t)jnum(fr, "frame_len") == BTP_HDR + 8 + dl && eqhex(d, 32, jstr(fr, "frame_sha256")) && !memcmp(buf, head, hl), "64 KiB DATA frame");
            uint8_t *file = malloc(70000); for (int k = 0; k < 70000; k++) file[k] = (uint8_t)(k * 7 + 3);
            btp_sha256(file, 70000, d); CHECK(eqhex(d, 32, jstr(ff, "file_sha256")), "70000-byte test file sha256");
            free(file); free(buf); free(head); nf++; types_seen[T_DATA] = 1; continue;
        }
        check_frame_vector(fr, ""); types_seen[type_by_name(jstr(fr, "type")) & 255] = 1; nf++;
    }
    CHECK(nf == 38, "38 frame vectors (%d)", nf);
    struct jv *pr = jget(V, "pairing"), *au = jget(V, "auth");
    for (int k = 0; k < 2; k++) { struct jv *cf = jget(k ? au : pr, "clear_frames"); for (int i = 0; cf && i < cf->n; i++) { check_frame_vector(cf->v[i], "clear "); types_seen[type_by_name(jstr(cf->v[i], "type")) & 255] = 1; } }

    /* device HELLO builder: the three device HELLOs of the vectors */
    {
        uint8_t did[8]; size_t l; uint8_t *t = unhex("0acef168ac63fe0f", &l); memcpy(did, t, 8); free(t);
        struct jv *hs[3] = { F->v[1], jget(pr, "clear_frames")->v[1], jget(au, "clear_frames")->v[1] };
        int fl[3] = { 13, 14, 12 };
        for (int i = 0; i < 3; i++) {
            struct btp_hello_info h = { fl[i], CAP_ALL, "hibysync 0.1.0", "HiBy R3 II", "0.8.0", "0.8.0", "R3II-2025", did };
            uint8_t out[512]; size_t n2 = btp_hello_device(out, sizeof out, &h);
            CHECK(eqhex(out, n2, jstr(hs[i], "payload")), "device HELLO built: %s", jstr(hs[i], "name"));
        }
    }

    /* invalid frames */
    struct jv *I = jget(V, "invalid_frames");
    for (int i = 0; I && i < I->n; i++) {
        size_t l; uint8_t *f = jget(I->v[i], "frame") ? jhex(I->v[i], "frame", &l) : jhex(I->v[i], "header", &l);
        int type, flags; uint32_t st, len; int e = btp_hdr_parse(f, BTP_MAX_PAYLOAD, &type, &flags, &st, &len);
        if (!e && !btp_type_known(type)) e = E_UNKNOWN_TYPE;
        CHECK(e == err_by_name(jstr(I->v[i], "error")), "invalid frame '%s': got %d", jstr(I->v[i], "name"), e);
        free(f);
    }

    /* paths */
    struct jv *PA = jget(V, "paths"), *pv = jget(PA, "valid"), *pi = jget(PA, "invalid");
    for (int i = 0; pv && i < pv->n; i++) { size_t l; uint8_t *p = jhex(pv->v[i], "path_hex", &l); CHECK(btp_path_check(p, l, 0) == PR_OK, "valid path %d", i); free(p); }
    for (int i = 0; pi && i < pi->n; i++) {
        size_t l; uint8_t *p = jhex(pi->v[i], "path_hex", &l); int r = btp_path_check(p, l, 0);
        CHECK(!strcmp(BTP_PATH_REASON[r], jstr(pi->v[i], "reason")), "invalid path %d: %s, got %s", i, jstr(pi->v[i], "reason"), BTP_PATH_REASON[r]);
        free(p);
    }
    CHECK(btp_path_check((const uint8_t *)"", 0, 1) == PR_OK && btp_path_check((const uint8_t *)"", 0, 0) == PR_EMPTY, "root path only where allowed");

    /* part files, resume offsets */
    struct jv *PF = jget(V, "part_files");
    for (int i = 0; PF && i < PF->n; i++) {
        struct jv *e = PF->v[i]; size_t l; uint8_t *sha = jhex(e, "sha256", &l); const char *path = jstr(e, "path");
        char part[32], full[600]; btp_part_name((const uint8_t *)path, strlen(path), sha, (uint64_t)jnum(e, "size"), part);
        const char *sl = strrchr(path, '/');
        if (sl) snprintf(full, sizeof full, "%.*s/%s", (int)(sl - path), path, part); else snprintf(full, sizeof full, "%s", part);
        CHECK(!strcmp(full, jstr(e, "part")), "part file %s -> %s (got %s)", path, jstr(e, "part"), full);
        free(sha);
    }
    struct jv *RO = jget(V, "resume_offsets");
    for (int i = 0; RO && i < RO->n; i++) CHECK(btp_resume_offset((uint64_t)jnum(RO->v[i], "part_size"), (uint64_t)jnum(RO->v[i], "size")) == (uint64_t)jnum(RO->v[i], "offset"), "resume offset %d", i);

    /* discovery */
    struct jv *D = jget(V, "discovery");
    for (int i = 0; D && i < D->n; i++) {
        struct jv *e = D->v[i], *f = jget(e, "fields"); size_t l; uint8_t *pk = jhex(e, "packet", &l);
        if (jnum(f, "type") == 1) {
            uint8_t nonce[8]; CHECK(!btp_probe_parse(pk, l, nonce) && eqhex(nonce, 8, jstr(f, "nonce")), "probe parses");
            pk[4] = 2; CHECK(btp_probe_parse(pk, l, nonce), "probe with version 2 ignored");
            CHECK(btp_probe_parse(pk, 15, nonce), "short probe ignored");
        } else {
            size_t nl; uint8_t *nonce = jhex(f, "nonce", &nl), did[8], out[64]; uint8_t *t = unhex("0acef168ac63fe0f", &nl); memcpy(did, t, 8); free(t);
            int zero = 1; for (int k = 0; k < 8; k++) if (nonce[k]) zero = 0;
            size_t n2 = btp_announce_build(out, sizeof out, (int)jnum(f, "flags"), zero ? 0 : nonce, 7340, did, "HiBy R3 II");
            CHECK(n2 == l && !memcmp(out, pk, l), "announce built: %s", jstr(e, "name"));
            free(nonce);
        }
        free(pk);
    }

    /* pairing transcript */
    {
        size_t l; uint8_t *cs = jhex(pr, "client_static_secret", &l), *dsec = jhex(pr, "device_static_secret", &l), *ce = jhex(pr, "client_ephemeral_secret", &l),
            *de = jhex(pr, "device_ephemeral_secret", &l), *Nc = jhex(pr, "client_nonce", &l), *Nd = jhex(pr, "device_nonce", &l);
        uint8_t Cs[32], Ds[32], Ce[32], De[32], did[8], kid[16], commit[32], th[32], ss[32], ss2[32], prk[32], kc[32], kd[32]; char sas[7];
        btp_x25519_base(Cs, cs); btp_x25519_base(Ds, dsec); btp_x25519_base(Ce, ce); btp_x25519_base(De, de);
        btp_device_id(Ds, did); btp_key_id(Cs, kid);
        CHECK(eqhex(Cs, 32, jstr(pr, "client_static_public")) && eqhex(Ds, 32, jstr(pr, "device_static_public")) &&
              eqhex(Ce, 32, jstr(pr, "client_ephemeral_public")) && eqhex(De, 32, jstr(pr, "device_ephemeral_public")), "pairing: public keys");
        CHECK(eqhex(did, 8, jstr(pr, "device_id")) && eqhex(kid, 16, jstr(pr, "client_key_id")), "pairing: device_id and key_id");
        btp_pair_commit(Ce, Nc, commit); CHECK(eqhex(commit, 32, jstr(pr, "commit")), "pairing: commitment");
        struct jv *cf = jget(pr, "clear_frames"); size_t hl1, hl2; uint8_t *hc = jhex(cf->v[0], "payload", &hl1), *hd = jhex(cf->v[1], "payload", &hl2), h1[32], h2[32];
        btp_sha256(hc, hl1, h1); btp_sha256(hd, hl2, h2);
        btp_pair_th(h1, h2, commit, De, Nd, Ce, Nc, th); CHECK(eqhex(th, 32, jstr(pr, "transcript_hash")), "pairing: transcript hash");
        CHECK(!btp_x25519_dh(ss, ce, De) && !btp_x25519_dh(ss2, de, Ce) && !memcmp(ss, ss2, 32) && eqhex(ss, 32, jstr(pr, "shared_secret")), "pairing: shared secret both ways");
        btp_hkdf_extract(th, 32, ss, 32, prk); CHECK(eqhex(prk, 32, jstr(pr, "prk")), "pairing: PRK");
        btp_sas(prk, sas); CHECK(!strcmp(sas, jstr(pr, "sas")), "pairing: SAS %s", sas);
        btp_keys(prk, kc, kd); CHECK(eqhex(kc, 32, jstr(pr, "k_c2d")) && eqhex(kd, 32, jstr(pr, "k_d2c")), "pairing: keys");
        check_records(jget(pr, "encrypted"), kc, kd, "pairing");
        struct jv *tr = jget(pr, "tampered_record"); size_t rl; uint8_t *rec = jhex(tr, "record", &rl), nonce[24];
        btp_nonce(nonce, DIR_C2D, (uint64_t)jnum(tr, "counter"));
        CHECK(btp_box_open(rec + 4, rec + BTP_REC_HDR, rl - BTP_REC_HDR, nonce, kc) == -1, "pairing: tampered record rejected");
        free(rec); free(cs); free(dsec); free(ce); free(de); free(Nc); free(Nd); free(hc); free(hd);
    }
    /* AUTH transcript */
    {
        size_t l; uint8_t *cs = jhex(au, "client_static_secret", &l), *dsec = jhex(au, "device_static_secret", &l), *ce = jhex(au, "client_ephemeral_secret", &l),
            *de = jhex(au, "device_ephemeral_secret", &l);
        uint8_t Cs[32], Ds[32], Ce[32], De[32], kid[16], ikm[96], ikm2[96], th[32], prk[32], kc[32], kd[32];
        btp_x25519_base(Cs, cs); btp_x25519_base(Ds, dsec); btp_x25519_base(Ce, ce); btp_x25519_base(De, de); btp_key_id(Cs, kid);
        CHECK(eqhex(Ce, 32, jstr(au, "client_ephemeral_public")) && eqhex(De, 32, jstr(au, "device_ephemeral_public")) && eqhex(kid, 16, jstr(au, "client_key_id")), "auth: keys");
        btp_x25519_dh(ikm, ce, De); btp_x25519_dh(ikm + 32, cs, De); btp_x25519_dh(ikm + 64, ce, Ds);       /* client side */
        btp_x25519_dh(ikm2, de, Ce); btp_x25519_dh(ikm2 + 32, de, Cs); btp_x25519_dh(ikm2 + 64, dsec, Ce);  /* device side */
        CHECK(!memcmp(ikm, ikm2, 96), "auth: both sides agree on ee||es||se");
        CHECK(eqhex(ikm, 32, jstr(au, "dh_ee")) && eqhex(ikm + 32, 32, jstr(au, "dh_es")) && eqhex(ikm + 64, 32, jstr(au, "dh_se")), "auth: DH values");
        struct jv *cf = jget(au, "clear_frames"); size_t hl1, hl2; uint8_t *hc = jhex(cf->v[0], "payload", &hl1), *hd = jhex(cf->v[1], "payload", &hl2), h1[32], h2[32];
        btp_sha256(hc, hl1, h1); btp_sha256(hd, hl2, h2);
        btp_auth_th(h1, h2, kid, Ce, De, Cs, Ds, th); CHECK(eqhex(th, 32, jstr(au, "transcript_hash")), "auth: transcript hash");
        btp_hkdf_extract(th, 32, ikm, 96, prk); CHECK(eqhex(prk, 32, jstr(au, "prk")), "auth: PRK");
        btp_keys(prk, kc, kd); CHECK(eqhex(kc, 32, jstr(au, "k_c2d")) && eqhex(kd, 32, jstr(au, "k_d2c")), "auth: keys");
        check_records(jget(au, "encrypted"), kc, kd, "auth");
        free(cs); free(dsec); free(ce); free(de); free(hc); free(hd);
    }
    struct jv *SE = jget(V, "sas_examples");
    for (int i = 0; SE && i < SE->n; i++) {
        size_t l; uint8_t *prk = jhex(SE->v[i], "prk", &l), b[4]; char sas[7];
        btp_hkdf_expand(prk, "BTP/1 sas", 9, b, 4); btp_sas(prk, sas);
        CHECK(eqhex(b, 4, jstr(SE->v[i], "sas_bytes")) && !strcmp(sas, jstr(SE->v[i], "sas")), "sas example %d", i);
        free(prk);
    }
    int seen = 0; for (int i = 0; i < 256; i++) seen += types_seen[i];
    printf("  vectors: %d frame types covered\n", seen);
    free(txt);
}

/* ================================================================== daemon tests */
struct cli { int fd, enc; uint8_t ktx[32], krx[32]; uint64_t ctx, crx; };
static uint8_t RB[BTP_MAX_RECORD + 64];
static int cli_connect(const char *ip, int port, struct cli *c)
{
    memset(c, 0, sizeof *c);
    for (int tries = 0; tries < 50; tries++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = inet_addr(ip);
        if (!connect(fd, (struct sockaddr *)&a, sizeof a)) {
            struct timeval tv = { 5, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            c->fd = fd; return 0;
        }
        close(fd); usleep(100000);
    }
    return -1;
}
static int wall(int fd, const void *p, size_t n) { const uint8_t *b = p; while (n) { ssize_t k = send(fd, b, n, MSG_NOSIGNAL); if (k <= 0) return -1; b += k; n -= (size_t)k; } return 0; }
static int rall(int fd, void *p, size_t n) { uint8_t *b = p; while (n) { ssize_t k = recv(fd, b, n, 0); if (k <= 0) return -1; b += k; n -= (size_t)k; } return 0; }
static int cli_send(struct cli *c, int type, int flags, uint32_t stream, const void *p, size_t n)
{
    static uint8_t b[BTP_MAX_RECORD + 64];
    uint8_t *fr = b + BTP_REC_HDR; btp_hdr_build(fr, type, flags, stream, (uint32_t)n); if (n) memcpy(fr + BTP_HDR, p, n);
    if (!c->enc) return wall(c->fd, fr, BTP_HDR + n);
    uint8_t nonce[24]; btp_nonce(nonce, DIR_C2D, c->ctx++);
    btp_box_seal(b + 4, fr, BTP_HDR + n, nonce, c->ktx); be32(b, (uint32_t)(BTP_TAG + BTP_HDR + n));
    return wall(c->fd, b, BTP_REC_HDR + BTP_HDR + n);
}
/* 0 = frame in RB (payload at *pay), -1 = closed / timeout / forged */
static int cli_recv(struct cli *c, int *type, int *flags, uint32_t *stream, uint8_t **pay, uint32_t *len)
{
    uint8_t *fr;
    if (!c->enc) {
        if (rall(c->fd, RB, BTP_HDR)) return -1;
        if (btp_hdr_parse(RB, BTP_MAX_PAYLOAD, type, flags, stream, len)) return -1;
        if (*len && rall(c->fd, RB + BTP_HDR, *len)) return -1;
        fr = RB;
    } else {
        if (rall(c->fd, RB, 4)) return -1;
        uint32_t bl = rd32(RB); if (bl < BTP_TAG + BTP_HDR || bl > BTP_MAX_RECORD) return -1;
        if (rall(c->fd, RB + 4, bl)) return -1;
        uint8_t nonce[24]; btp_nonce(nonce, DIR_D2C, c->crx++);
        if (btp_box_open(RB + 4, RB + BTP_REC_HDR, bl - BTP_TAG, nonce, c->krx)) return -1;
        fr = RB + BTP_REC_HDR;
        if (btp_hdr_parse(fr, BTP_MAX_PAYLOAD, type, flags, stream, len) || BTP_HDR + *len != bl - BTP_TAG) return -1;
    }
    *pay = fr + BTP_HDR; return 0;
}
/* send a request and wait for the terminal frame; returns its type, error code in *err */
static int rpc(struct cli *c, int type, uint32_t st, const void *p, size_t n, int *err, uint8_t *out, size_t *outn)
{
    *err = 0;
    if (cli_send(c, type, 0, st, p, n)) return -1;
    for (;;) {
        int t, f; uint32_t s, l; uint8_t *q;
        if (cli_recv(c, &t, &f, &s, &q, &l)) return -1;
        if (s != st) continue;
        if (t == T_ERROR && l >= 2) *err = (int)rd16(q);
        if (out && l <= 70000) { memcpy(out, q, l); if (outn) *outn = l; }
        if (f & BTP_FLAG_END) return t;
        if (t == T_PUT_READY) return t;
    }
}
static size_t p_path(uint8_t *o, int ns, int flags, const char *path, int with_flags)
{
    struct bwr w; bwr_init(&w, o, 2000); bwr_u8(&w, (uint32_t)ns); if (with_flags) bwr_u8(&w, (uint32_t)flags); bwr_str16(&w, path, strlen(path)); return w.n;
}
static int hello(struct cli *c, int *dflags)
{
    uint8_t p[64]; struct bwr w; bwr_init(&w, p, sizeof p);
    bwr_u8(&w, 1); bwr_u8(&w, 1); bwr_u8(&w, 0); bwr_u8(&w, 0); bwr_u32(&w, BTP_MAX_PAYLOAD); bwr_u32(&w, CAP_ALL);
    bwr_tlv(&w, TLV_SOFTWARE, "btp_test", 8); bwr_tlv(&w, TLV_NAME, "Test PC", 7);
    if (cli_send(c, T_HELLO, 0, 0, p, w.n)) return -1;
    int t, f; uint32_t s, l; uint8_t *q;
    if (cli_recv(c, &t, &f, &s, &q, &l) || t != T_HELLO) return -1;
    if (dflags) *dflags = q[3];
    return 0;
}
static int hello_full(struct cli *c, uint8_t hc[32], uint8_t hd[32], int *dflags)
{
    uint8_t p[64]; struct bwr w; bwr_init(&w, p, sizeof p);
    bwr_u8(&w, 1); bwr_u8(&w, 1); bwr_u8(&w, 0); bwr_u8(&w, 0); bwr_u32(&w, BTP_MAX_PAYLOAD); bwr_u32(&w, CAP_ALL);
    bwr_tlv(&w, TLV_NAME, "Laptop", 6);
    btp_sha256(p, w.n, hc);
    if (cli_send(c, T_HELLO, 0, 0, p, w.n)) return -1;
    int t, f; uint32_t s, l; uint8_t *q;
    if (cli_recv(c, &t, &f, &s, &q, &l) || t != T_HELLO) return -1;
    btp_sha256(q, l, hd); if (dflags) *dflags = q[3];
    return 0;
}
static size_t p_put(uint8_t *o, int ns, int mode, uint64_t size, int64_t mtime, const uint8_t sha[32], const char *path)
{
    struct bwr w; bwr_init(&w, o, 2000); bwr_u8(&w, (uint32_t)ns); bwr_u8(&w, (uint32_t)mode); bwr_u64(&w, size); bwr_u64(&w, (uint64_t)mtime);
    bwr_bytes(&w, sha, 32); bwr_str16(&w, path, strlen(path)); return w.n;
}
/* PUT of data[0..n) (stop after `stop_at` bytes and return -2 to simulate a drop); returns the
 * terminal type, err, *ready = PUT_READY offset */
static int put(struct cli *c, uint32_t st, int ns, int mode, const char *path, const uint8_t *data, size_t n, int64_t mtime, size_t stop_at, uint64_t *ready, int *err, int *result)
{
    uint8_t sha[32], p[2100], o[100]; size_t on = 0; btp_sha256(data, n, sha);
    int t = rpc(c, T_PUT_BEGIN, st, p, p_put(p, ns, mode, n, mtime, sha, path), err, o, &on);
    if (t == T_PUT_DONE) { *result = o[0]; return t; }
    if (t != T_PUT_READY) return t;
    uint64_t off = rd64(o); *ready = off;
    static uint8_t d[8 + 65536];
    while (off < n) {
        size_t l = n - off > 65536 ? 65536 : n - off;
        if (stop_at && off + l > stop_at) l = stop_at - off;
        be64(d, off); memcpy(d + 8, data + off, l);
        if (cli_send(c, T_DATA, 0, st, d, 8 + l)) return -1;
        off += l;
        if (stop_at && off >= stop_at) return -2;
    }
    t = rpc(c, T_PUT_END, st, 0, 0, err, o, &on);
    if (t == T_PUT_DONE) *result = o[0];
    return t;
}
static int get(struct cli *c, uint32_t st, const char *path, uint64_t offset, int want_sha, uint8_t *out, size_t cap, size_t *got, uint8_t sha[32], int *err)
{
    uint8_t p[2100]; struct bwr w; bwr_init(&w, p, sizeof p); bwr_u8(&w, 0); bwr_u8(&w, (uint32_t)want_sha); bwr_u64(&w, offset); bwr_str16(&w, path, strlen(path));
    *err = 0; *got = 0;
    if (cli_send(c, T_GET, 0, st, p, w.n)) return -1;
    for (;;) {
        int t, f; uint32_t s, l; uint8_t *q;
        if (cli_recv(c, &t, &f, &s, &q, &l)) return -1;
        if (s != st) continue;
        if (t == T_ERROR) { *err = (int)rd16(q); return t; }
        if (t == T_DATA) { uint64_t o = rd64(q); if (o - offset + (l - 8) <= cap) memcpy(out + (o - offset), q + 8, l - 8); *got += l - 8; }
        if (t == T_GET_END) { memcpy(sha, q, 32); return t; }
    }
}
static void wfile(const char *p, const void *d, size_t n) { FILE *f = fopen(p, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); } }
static uint8_t *rfile(const char *p, size_t *n) { return (uint8_t *)ota_read_file(p, 64 << 20, n); }
static int exists(const char *p) { struct stat st; return !lstat(p, &st); }
static int ctl_fd = -1;
static int ctl_read_line(char *out, int cap, int timeout_ms)   /* next line from the daemon */
{
    static char b[4096]; static size_t bl;
    long long until = now_ms() + timeout_ms;
    for (;;) {
        char *nl = memchr(b, '\n', bl);
        if (nl) { size_t l = (size_t)(nl - b); if ((int)l >= cap) l = (size_t)cap - 1; memcpy(out, b, l); out[l] = 0; memmove(b, nl + 1, bl - (size_t)(nl + 1 - b)); bl -= (size_t)(nl + 1 - b); return 0; }
        long long left = until - now_ms(); if (left <= 0) return -1;
        struct pollfd p = { ctl_fd, POLLIN, 0 }; if (poll(&p, 1, (int)left) <= 0) return -1;
        ssize_t k = read(ctl_fd, b + bl, sizeof b - bl); if (k <= 0) return -1; bl += (size_t)k;
    }
}
static int ctl_wait(const char *prefix, char *line, int cap)   /* skip other lines (XFER, CONN...) */
{
    for (int i = 0; i < 200; i++) { if (ctl_read_line(line, cap, 5000)) return -1; if (!strncmp(line, prefix, strlen(prefix))) return 0; }
    return -1;
}
static void ctl_write(const char *l) { if (write(ctl_fd, l, strlen(l)) < 0) {} }

static int short_idle;   /* next start_daemon(): idle and PUT-idle timeouts of 3 s */
static pid_t start_daemon(const char *bin, const char *W, int wifi)
{
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t pid = fork();
    if (pid == 0) {
        int keep = fcntl(sv[1], F_DUPFD, 10); close(sv[0]); close(sv[1]); dup2(keep, 3); close(keep);
        char e[600];
        snprintf(e, sizeof e, "%s/sd", W); setenv("HIBYSYNC_SD", e, 1);
        snprintf(e, sizeof e, "%s/data", W); setenv("HIBYSYNC_DATA", e, 1);
        snprintf(e, sizeof e, "%s/data/hibymod", W); setenv("HIBYSYNC_HM", e, 1);
        snprintf(e, sizeof e, "%s/mounts", W); setenv("HIBYSYNC_MOUNTS", e, 1);
        snprintf(e, sizeof e, "%s/hibytag", W); setenv("HIBYSYNC_HIBYTAG", e, 1);
        snprintf(e, sizeof e, "%s/hibymod_cli", W); setenv("HIBYSYNC_CLI", e, 1);
        snprintf(e, sizeof e, "%s/batt", W); setenv("HIBYSYNC_BATT", e, 1);
        snprintf(e, sizeof e, "%s/usb", W); setenv("HIBYSYNC_USB", e, 1);
        snprintf(e, sizeof e, "%s/lock", W); setenv("HIBYSYNC_LOCK", e, 1);
        snprintf(e, sizeof e, "%s/resconf.json", W); setenv("HIBYSYNC_RESCONF", e, 1);
        setenv("HIBYSYNC_PORT", "17340", 1);
        setenv("HIBYSYNC_WIFI_ADDR", "127.0.0.2", 1);
        setenv("HIBYSYNC_SECURE_S", "3", 1);   /* unauthenticated Wi-Fi deadline, short for the test */
        if (short_idle) { setenv("HIBYSYNC_IDLE_S", "3", 1); setenv("HIBYSYNC_PUT_IDLE_S", "3", 1); }
        snprintf(e, sizeof e, "%s/daemon.log", W); int lf = open(e, O_WRONLY | O_CREAT | O_APPEND, 0644); if (lf >= 0) { dup2(lf, 2); close(lf); }
        if (wifi) execl(bin, "hibysync", "serve", "--control", "3", "--wifi", "--name", "Test%20R3", (char *)0);
        else execl(bin, "hibysync", "serve", "--control", "3", "--name", "Test%20R3", (char *)0);
        _exit(127);
    }
    close(sv[1]); ctl_fd = sv[0];
    char line[256]; if (ctl_wait("READY", line, sizeof line)) { CHECK(0, "daemon READY"); return pid; }
    CHECK(!strncmp(line, "READY 17340 ", 12), "READY line: %s", line);
    return pid;
}
static int wait_exit(pid_t pid, int ms)
{
    for (int i = 0; i < ms / 20; i++) { int st; if (waitpid(pid, &st, WNOHANG) == pid) return 0; usleep(20000); }
    return -1;
}
static void daemon_tests(const char *bin, const char *W)
{
    char p[700], line[600];
    snprintf(p, sizeof p, "rm -rf '%s' && mkdir -p '%s/sd/Music' '%s/data/hibymod' '%s/outside'", W, W, W, W); if (system(p)) {}
    char mounts[700]; snprintf(mounts, sizeof mounts, "%s/mounts", W);
    char ml[700]; snprintf(ml, sizeof ml, "/dev/mmcblk0p1 %s/sd exfat rw,relatime 0 0\n", W); wfile(mounts, ml, strlen(ml));
    snprintf(p, sizeof p, "%s/data/hibymod/payload_version", W); wfile(p, "0.8.0\n", 6);
    snprintf(p, sizeof p, "%s/hibymod_cli", W); wfile(p, "case \"$1\" in status) echo \"ByOS 0.8.0\" ;; esac\n", 46);
    snprintf(p, sizeof p, "%s/batt", W); wfile(p, "87\n", 3);
    snprintf(p, sizeof p, "%s/usb", W); wfile(p, "1\n", 2);
    /* stub hibytag: dbsnapshot copies, dbcheck accepts, dbupdate logs */
    snprintf(p, sizeof p, "%s/hibytag", W);
    char stub[900]; snprintf(stub, sizeof stub, "#!/bin/sh\ncase \"$1\" in\n dbsnapshot) cp \"$2\" \"$3\" ;;\n dbcheck) [ -f '%s/slowcheck' ] && sleep 5; [ -f \"$2\" ] ;;\n dbupdate) shift; echo \"$@\" >> '%s/dbupdate.log' ;;\n *) exit 1 ;;\nesac\n", W, W);
    wfile(p, stub, strlen(stub)); chmod(p, 0755);
    /* live DB */
    uint8_t live[8192] = { 0 }; memcpy(live, "SQLite format 3", 16); live[16] = 0x10; live[17] = 0; for (int i = 100; i < 8192; i++) live[i] = (uint8_t)i;
    snprintf(p, sizeof p, "%s/data/usrlocal_media.db", W); wfile(p, live, sizeof live);

    pid_t pid = start_daemon(bin, W, 1);
    struct cli c; int err, df, res; uint8_t buf[70000], sha[32], pl[2100]; size_t on; uint64_t ready;
    CHECK(!cli_connect("127.0.0.1", 17340, &c) && !hello(&c, &df), "local connect + HELLO");
    CHECK(df == (HF_LOCAL | HF_SD_MOUNTED | HF_SHELL_ATTACHED), "device HELLO flags on USB = LOCAL|SD|SHELL (%d)", df);
    CHECK(!ctl_wait("CONN ", line, sizeof line) && strstr(line, " usb Test%20PC"), "CONN line: %s", line);

    /* PING, unknown type */
    uint8_t ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    { int t, f; uint32_t s, l; uint8_t *q; cli_send(&c, T_PING, 0, 0, ping, 8); CHECK(!cli_recv(&c, &t, &f, &s, &q, &l) && t == T_PONG && l == 8 && !memcmp(q, ping, 8), "PING -> PONG"); }
    CHECK(rpc(&c, 0x7f, 3, 0, 0, &err, 0, 0) == T_ERROR && err == E_UNKNOWN_TYPE, "unknown type -> E_UNKNOWN_TYPE");
    CHECK(rpc(&c, T_DF, 4, 0, 0, &err, buf, &on) == T_DF_R && on == 34 && buf[0] == 0 && buf[1] == 2 && rd64(buf + 2) > 0, "DF: SD mounted, exfat, sizes");

    /* MKDIR */
    CHECK(rpc(&c, T_MKDIR, 5, pl, p_path(pl, 0, 0, "Docs", 1), &err, 0, 0) == T_OK, "MKDIR Docs");
    CHECK(rpc(&c, T_MKDIR, 6, pl, p_path(pl, 0, 0, "Docs", 1), &err, 0, 0) == T_ERROR && err == E_EXISTS, "MKDIR existing -> E_EXISTS");
    CHECK(rpc(&c, T_MKDIR, 7, pl, p_path(pl, 0, 1, "Music/New/Album/CD1", 1), &err, 0, 0) == T_OK, "MKDIR -p deep");
    CHECK(rpc(&c, T_MKDIR, 8, pl, p_path(pl, 0, 1, "Music/New/Album", 1), &err, 0, 0) == T_OK, "MKDIR -p existing is fine");
    CHECK(rpc(&c, T_MKDIR, 9, pl, p_path(pl, 0, 0, "No/Such", 1), &err, 0, 0) == T_ERROR && err == E_NOT_FOUND, "MKDIR without parent -> E_NOT_FOUND");
    snprintf(p, sizeof p, "%s/sd/Music/New/Album/CD1", W); CHECK(exists(p), "folders created on disk");

    /* PUT create, vector content */
    const char *hello_txt = "hello world\n";
    CHECK(put(&c, 11, 0, PM_CREATE, "Docs/hello.txt", (const uint8_t *)hello_txt, 12, 1767225600, 0, &ready, &err, &res) == T_PUT_DONE && res == 0, "PUT create");
    snprintf(p, sizeof p, "%s/sd/Docs/hello.txt", W); size_t fl; uint8_t *fc = rfile(p, &fl);
    CHECK(fc && fl == 12 && !memcmp(fc, hello_txt, 12), "PUT content on disk"); free(fc);
    struct stat st; CHECK(!stat(p, &st) && st.st_mtime == 1767225600, "PUT sets mtime");
    snprintf(p, sizeof p, "%s/sd/Docs/.btp-169afa71b857d5a0.part", W); CHECK(!exists(p), "no part file left");
    CHECK(put(&c, 13, 0, PM_CREATE, "Docs/hello.txt", (const uint8_t *)hello_txt, 12, 0, 0, &ready, &err, &res) == T_ERROR && err == E_EXISTS, "PUT create over existing -> E_EXISTS");
    CHECK(put(&c, 15, 0, PM_SKIP_IDENTICAL, "Docs/hello.txt", (const uint8_t *)hello_txt, 12, 0, 0, &ready, &err, &res) == T_PUT_DONE && res == 1, "PUT skip identical -> result 1");
    CHECK(put(&c, 17, 0, PM_SKIP_IDENTICAL, "Docs/hello.txt", (const uint8_t *)"HELLO world\n", 12, 0, 0, &ready, &err, &res) == T_ERROR && err == E_EXISTS, "PUT skip but different -> E_EXISTS");
    CHECK(put(&c, 19, 0, PM_REPLACE, "Docs/hello.txt", (const uint8_t *)"replaced!", 9, 0, 0, &ready, &err, &res) == T_PUT_DONE && res == 0, "PUT replace");
    snprintf(p, sizeof p, "%s/sd/Docs/hello.txt", W); fc = rfile(p, &fl); CHECK(fc && fl == 9 && !memcmp(fc, "replaced!", 9), "replaced content"); free(fc);
    CHECK(put(&c, 21, 0, PM_CREATE, "Nope/x.txt", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_NOT_FOUND, "PUT without parent -> E_NOT_FOUND");
    CHECK(put(&c, 23, 0, PM_CREATE, "Music/../x.txt", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_BAD_PATH, "PUT with .. -> E_BAD_PATH");
    CHECK(put(&c, 25, 0, PM_CREATE, ".hidden", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_BAD_PATH, "PUT hidden name -> E_BAD_PATH");
    CHECK(put(&c, 27, 0, PM_CREATE, "Music", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_IS_DIR, "PUT over a folder -> E_IS_DIR");
    CHECK(put(&c, 29, 0, PM_CREATE, "empty.bin", (const uint8_t *)"", 0, 0, 0, &ready, &err, &res) == T_PUT_DONE, "PUT empty file");

    /* PUT: bad hash (claims other content) and bad offset */
    {
        uint8_t fake[32] = { 0 }; int t = rpc(&c, T_PUT_BEGIN, 31, pl, p_put(pl, 0, PM_CREATE, 5, 0, fake, "bad.bin"), &err, buf, &on);
        uint8_t d[13]; be64(d, 0); memcpy(d + 8, "abcde", 5); cli_send(&c, T_DATA, 0, 31, d, 13);
        CHECK(t == T_PUT_READY && rpc(&c, T_PUT_END, 31, 0, 0, &err, 0, 0) == T_ERROR && err == E_HASH_MISMATCH, "PUT wrong sha -> E_HASH_MISMATCH");
        char part[32]; btp_part_name((const uint8_t *)"bad.bin", 7, fake, 5, part);
        snprintf(p, sizeof p, "%s/sd/%s", W, part); CHECK(!exists(p), "bad part deleted");
        uint8_t s2[32]; btp_sha256("abcde", 5, s2);
        t = rpc(&c, T_PUT_BEGIN, 33, pl, p_put(pl, 0, PM_CREATE, 5, 0, s2, "off.bin"), &err, buf, &on);
        be64(d, 3); CHECK(t == T_PUT_READY && rpc(&c, T_DATA, 33, d, 13, &err, 0, 0) == T_ERROR && err == E_OFFSET, "DATA at wrong offset -> E_OFFSET");
        t = rpc(&c, T_PUT_BEGIN, 35, pl, p_put(pl, 0, PM_CREATE, 5, 0, s2, "off.bin"), &err, buf, &on);
        uint8_t big[8 + 6]; be64(big, 0); memcpy(big + 8, "abcdef", 6);
        CHECK(t == T_PUT_READY && rpc(&c, T_DATA, 35, big, 14, &err, 0, 0) == T_ERROR && err == E_HASH_MISMATCH, "DATA past size -> E_HASH_MISMATCH");
    }

    /* PUT resume after a dropped connection: 200000 bytes, drop at 70000 -> resume at 65536 */
    static uint8_t bigf[200000]; for (int i = 0; i < 200000; i++) bigf[i] = (uint8_t)(i * 7 + 3);
    int t = put(&c, 41, 0, PM_CREATE, "Music/big.bin", bigf, sizeof bigf, 0, 70000, &ready, &err, &res);
    CHECK(t == -2 && ready == 0, "PUT big: started at 0, dropped at 70000");
    close(c.fd); usleep(200000);
    uint8_t bsha[32]; btp_sha256(bigf, sizeof bigf, bsha); char part[32]; btp_part_name((const uint8_t *)"Music/big.bin", 13, bsha, sizeof bigf, part);
    snprintf(p, sizeof p, "%s/sd/Music/%s", W, part);
    CHECK(!stat(p, &st) && st.st_size == 70000, "part file kept after the drop (%lld bytes)", (long long)st.st_size);
    CHECK(!cli_connect("127.0.0.1", 17340, &c) && !hello(&c, 0), "reconnect");
    /* a torn tail: garbage after the durable offset must be dropped */
    t = put(&c, 43, 0, PM_CREATE, "Music/big.bin", bigf, sizeof bigf, 0, 0, &ready, &err, &res);
    CHECK(t == T_PUT_DONE && ready == 65536, "PUT resumed at 65536 and done (ready=%llu)", (unsigned long long)ready);
    snprintf(p, sizeof p, "%s/sd/Music/big.bin", W); fc = rfile(p, &fl); CHECK(fc && fl == sizeof bigf && !memcmp(fc, bigf, fl), "resumed file intact"); free(fc);
    snprintf(p, sizeof p, "%s/sd/Music/%s", W, part); CHECK(!exists(p), "part gone after rename");

    /* STAT */
    CHECK(rpc(&c, T_STAT, 45, pl, p_path(pl, 0, 0, "Music/big.bin", 0), &err, buf, &on) == T_STAT_R && buf[0] == 0 && rd64(buf + 1) == 200000, "STAT file");
    CHECK(rpc(&c, T_STAT, 47, pl, p_path(pl, 0, 0, "", 0), &err, buf, &on) == T_STAT_R && buf[0] == 1, "STAT root is a folder");
    CHECK(rpc(&c, T_STAT, 49, pl, p_path(pl, 0, 0, "Music/none", 0), &err, buf, &on) == T_ERROR && err == E_NOT_FOUND, "STAT missing -> E_NOT_FOUND");
    CHECK(rpc(&c, T_STAT, 51, pl, p_path(pl, 0, 0, "Docs/hello.txt/x", 0), &err, buf, &on) == T_ERROR && err == E_NOT_DIR, "STAT through a file -> E_NOT_DIR");

    /* symlink confinement */
    snprintf(p, sizeof p, "%s/outside/secret.txt", W); wfile(p, "secret", 6);
    snprintf(p, sizeof p, "%s/outside", W); snprintf(line, sizeof line, "%s/sd/link", W); if (symlink(p, line)) {}
    snprintf(p, sizeof p, "%s/outside/secret.txt", W); snprintf(line, sizeof line, "%s/sd/flink.txt", W); if (symlink(p, line)) {}
    CHECK(rpc(&c, T_STAT, 53, pl, p_path(pl, 0, 0, "link/secret.txt", 0), &err, 0, 0) == T_ERROR && err == E_BAD_PATH, "STAT through a dir symlink -> E_BAD_PATH");
    CHECK(rpc(&c, T_STAT, 55, pl, p_path(pl, 0, 0, "flink.txt", 0), &err, 0, 0) == T_ERROR && err == E_BAD_PATH, "STAT of a file symlink -> E_BAD_PATH");
    size_t got; CHECK(get(&c, 57, "flink.txt", 0, 1, buf, sizeof buf, &got, sha, &err) == T_ERROR && err == E_BAD_PATH, "GET of a symlink -> E_BAD_PATH");
    CHECK(rpc(&c, T_LIST, 59, pl, p_path(pl, 0, 0, "link", 1), &err, 0, 0) == T_ERROR && err == E_BAD_PATH, "LIST of a symlinked folder -> E_BAD_PATH");
    CHECK(put(&c, 61, 0, PM_REPLACE, "link/secret.txt", (const uint8_t *)"pwn", 3, 0, 0, &ready, &err, &res) == T_ERROR && err == E_BAD_PATH, "PUT through a symlink -> E_BAD_PATH");
    CHECK(put(&c, 63, 0, PM_REPLACE, "flink.txt", (const uint8_t *)"pwn", 3, 0, 0, &ready, &err, &res) == T_ERROR && err == E_BAD_PATH, "PUT replacing a symlink -> E_BAD_PATH");
    snprintf(p, sizeof p, "%s/outside/secret.txt", W); fc = rfile(p, &fl); CHECK(fc && fl == 6 && !memcmp(fc, "secret", 6), "file outside the SD untouched"); free(fc);

    /* GET */
    CHECK(get(&c, 65, "Music/big.bin", 0, 1, buf, sizeof buf, &got, sha, &err) == T_GET_END && got == 200000 && !memcmp(sha, bsha, 32), "GET with sha (200000 bytes, sha ok)");
    static uint8_t gb[200000];
    CHECK(get(&c, 67, "Music/big.bin", 0, 0, gb, sizeof gb, &got, sha, &err) == T_GET_END && got == 200000 && !memcmp(gb, bigf, 200000), "GET without sha (sendfile) content ok");
    uint8_t zero[32] = { 0 }; CHECK(!memcmp(sha, zero, 32), "GET without sha -> zero hash");
    CHECK(get(&c, 69, "Music/big.bin", 131072, 1, gb, sizeof gb, &got, sha, &err) == T_GET_END && got == 200000 - 131072 && !memcmp(gb, bigf + 131072, got) && !memcmp(sha, bsha, 32), "GET resume at 131072: whole-file sha");
    CHECK(get(&c, 71, "Music/big.bin", 200000, 1, gb, sizeof gb, &got, sha, &err) == T_GET_END && got == 0, "GET at size -> GET_R + GET_END");
    CHECK(get(&c, 73, "Music/big.bin", 200001, 1, gb, sizeof gb, &got, sha, &err) == T_ERROR && err == E_OFFSET, "GET past size -> E_OFFSET");
    CHECK(get(&c, 75, "Music", 0, 1, gb, sizeof gb, &got, sha, &err) == T_ERROR && err == E_IS_DIR, "GET of a folder -> E_IS_DIR");
    /* CANCEL during a GET */
    {
        static uint8_t huge[4 << 20]; for (size_t i = 0; i < sizeof huge; i++) huge[i] = (uint8_t)i;
        snprintf(p, sizeof p, "%s/sd/huge.bin", W); wfile(p, huge, sizeof huge);
        uint8_t q[100]; size_t n = 0; struct bwr w; bwr_init(&w, q, sizeof q); bwr_u8(&w, 0); bwr_u8(&w, 1); bwr_u64(&w, 0); bwr_str16(&w, "huge.bin", 8); n = w.n;
        cli_send(&c, T_GET, 0, 77, q, n);
        int tt, f; uint32_t s, l; uint8_t *pp; int ndata = 0, cancelled = 0, terminal = 0;
        cli_send(&c, T_CANCEL, 0, 77, 0, 0);
        for (int i = 0; i < 200; i++) {
            if (cli_recv(&c, &tt, &f, &s, &pp, &l)) break;
            if (tt == T_DATA) ndata++;
            if (f & BTP_FLAG_END) { terminal = 1; cancelled = tt == T_ERROR && rd16(pp) == E_CANCELLED; break; }
        }
        CHECK(terminal && cancelled && ndata < 64, "CANCEL stops a running GET (%d DATA frames before)", ndata);
        CHECK(rpc(&c, T_STAT, 79, pl, p_path(pl, 0, 0, "huge.bin", 0), &err, buf, &on) == T_STAT_R, "session usable after CANCEL");
    }
    /* 2.3 with pipelining: GET, a pipelined DF, then CANCEL of the GET -> the CANCEL is read at
     * once (the DF is queued aside), the GET stops, then the DF is answered */
    {
        snprintf(p, sizeof p, "%s/sd/huge32.bin", W); FILE *hf = fopen(p, "wb"); if (hf) { if (ftruncate(fileno(hf), 32 << 20)) {} fclose(hf); }
        uint8_t q[100]; struct bwr w; bwr_init(&w, q, sizeof q); bwr_u8(&w, 0); bwr_u8(&w, 1); bwr_u64(&w, 0); bwr_str16(&w, "huge32.bin", 10);
        cli_send(&c, T_GET, 0, 201, q, w.n); cli_send(&c, T_DF, 0, 203, 0, 0); cli_send(&c, T_CANCEL, 0, 201, 0, 0);
        int tt, f; uint32_t s, l; uint8_t *pp; long long bytes = 0; int cancelled = 0, df_after = 0, order_ok = 1, ended = 0;
        for (int i = 0; i < 1000; i++) {
            if (cli_recv(&c, &tt, &f, &s, &pp, &l)) break;
            if (s == 201 && tt == T_DATA) bytes += l - 8;
            if (s == 201 && (f & BTP_FLAG_END)) { ended = 1; cancelled = tt == T_ERROR && rd16(pp) == E_CANCELLED; }
            if (s == 203 && (f & BTP_FLAG_END)) { df_after = tt == T_DF_R; if (!ended) order_ok = 0; break; }
        }
        CHECK(cancelled && bytes < (32 << 20), "pipelined: CANCEL behind a queued DF stops the GET (%lld of 32 MiB sent)", bytes);
        CHECK(df_after && order_ok, "pipelined: the queued DF is answered after the cancelled GET");
        /* CANCEL of a queued request: GET 205, STAT 207 (queued), CANCEL 207, CANCEL 205 */
        bwr_init(&w, q, sizeof q); bwr_u8(&w, 0); bwr_u8(&w, 1); bwr_u64(&w, 0); bwr_str16(&w, "huge32.bin", 10);
        cli_send(&c, T_GET, 0, 205, q, w.n); cli_send(&c, T_STAT, 0, 207, pl, p_path(pl, 0, 0, "huge.bin", 0));
        cli_send(&c, T_CANCEL, 0, 207, 0, 0); cli_send(&c, T_CANCEL, 0, 205, 0, 0);
        int c205 = 0, c207 = 0, stat207 = 0;
        for (int i = 0; i < 1000 && !(c205 && (c207 || stat207)); i++) {
            if (cli_recv(&c, &tt, &f, &s, &pp, &l)) break;
            if (s == 205 && (f & BTP_FLAG_END)) c205 = tt == T_ERROR && rd16(pp) == E_CANCELLED;
            if (s == 207 && (f & BTP_FLAG_END)) { if (tt == T_ERROR && rd16(pp) == E_CANCELLED) c207 = 1; else stat207 = 1; }
        }
        CHECK(c207 && !stat207, "CANCEL of a queued request -> E_CANCELLED, never run");
        CHECK(c205, "then the running GET is cancelled too");
        { int t2, f2; uint32_t s2, l2; uint8_t *p2; cli_send(&c, T_PING, 0, 0, ping, 8); CHECK(!cli_recv(&c, &t2, &f2, &s2, &p2, &l2) && t2 == T_PONG, "session in sync afterwards (PING)"); }
        unlink(p);
    }

    /* LIST: many entries -> several pages; hidden entries */
    for (int i = 0; i < 2500; i++) { snprintf(p, sizeof p, "%s/sd/Music/New/track number %04d with a long name.flac", W, i); wfile(p, "", 0); }
    snprintf(p, sizeof p, "%s/sd/Music/New/.hiddenfile", W); wfile(p, "", 0);
    {
        int pages = 0, total = 0, hidden = 0, ok = 1;
        cli_send(&c, T_LIST, 0, 81, pl, p_path(pl, 0, 0, "Music/New", 1));
        for (;;) {
            int tt, f; uint32_t s, l; uint8_t *pp; if (cli_recv(&c, &tt, &f, &s, &pp, &l) || tt != T_LIST_R) { ok = 0; break; }
            pages++; struct brd r; brd_init(&r, pp, l); int cnt = (int)brd_u16(&r);
            for (int i = 0; i < cnt; i++) { brd_u8(&r); brd_u64(&r); brd_u64(&r); size_t nl; const uint8_t *nm = brd_str16(&r, &nl); if (nm && nm[0] == '.') hidden++; total++; }
            if (!brd_done(&r)) ok = 0;
            if (f & BTP_FLAG_END) break;
        }
        CHECK(ok && pages >= 2 && total == 2501 && hidden == 0, "LIST: %d pages, %d entries (2500 files + 1 folder), hidden not shown", pages, total);
        total = hidden = 0;
        cli_send(&c, T_LIST, 0, 83, pl, p_path(pl, 0, 1, "Music/New", 1));
        for (;;) {
            int tt, f; uint32_t s, l; uint8_t *pp; if (cli_recv(&c, &tt, &f, &s, &pp, &l) || tt != T_LIST_R) break;
            struct brd r; brd_init(&r, pp, l); int cnt = (int)brd_u16(&r);
            for (int i = 0; i < cnt; i++) { brd_u8(&r); brd_u64(&r); brd_u64(&r); size_t nl; const uint8_t *nm = brd_str16(&r, &nl); if (nm && nm[0] == '.') hidden++; total++; }
            if (f & BTP_FLAG_END) break;
        }
        CHECK(total == 2502 && hidden == 1, "LIST HIDDEN shows dot files (%d, %d)", total, hidden);
        CHECK(rpc(&c, T_LIST, 85, pl, p_path(pl, 0, 0, "Docs/hello.txt", 1), &err, 0, 0) == T_ERROR && err == E_NOT_DIR, "LIST of a file -> E_NOT_DIR");
        CHECK(rpc(&c, T_LIST, 87, pl, p_path(pl, 0, 0, "Music/New/Album/CD1", 1), &err, buf, &on) == T_LIST_R && on == 2 && rd16(buf) == 0, "LIST of an empty folder: one page, count 0");
        CHECK(rpc(&c, T_LIST, 89, pl, p_path(pl, 1, 0, "", 1), &err, 0, 0) == T_ERROR && err == E_NOT_ALLOWED, "LIST in NS_UPDATE -> E_NOT_ALLOWED");
    }

    /* MOVE */
    {
        uint8_t q[200]; struct bwr w;
#define MV(st_, fl_, a_, b_) (bwr_init(&w, q, sizeof q), bwr_u8(&w, 0), bwr_u8(&w, fl_), bwr_str16(&w, a_, strlen(a_)), bwr_str16(&w, b_, strlen(b_)), rpc(&c, T_MOVE, st_, q, w.n, &err, 0, 0))
        CHECK(MV(91, 0, "Docs/hello.txt", "Docs/moved.txt") == T_OK, "MOVE rename");
        snprintf(p, sizeof p, "%s/sd/Docs/moved.txt", W); CHECK(exists(p), "moved on disk");
        CHECK(MV(93, 0, "Docs/moved.txt", "Music/big.bin") == T_ERROR && err == E_EXISTS, "MOVE onto existing without REPLACE -> E_EXISTS");
        CHECK(MV(95, 1, "Docs/moved.txt", "empty.bin") == T_OK, "MOVE with REPLACE");
        CHECK(MV(97, 0, "Music/New", "Music/New/Sub") == T_ERROR && err == E_BAD_PATH, "MOVE a folder into itself -> E_BAD_PATH");
        CHECK(MV(99, 0, "Music/none", "Music/x") == T_ERROR && err == E_NOT_FOUND, "MOVE missing -> E_NOT_FOUND");
        CHECK(MV(101, 0, "Docs", "Albums") == T_OK, "MOVE a folder");
    }
    /* DELETE */
    CHECK(rpc(&c, T_DELETE, 103, pl, p_path(pl, 0, 0, "Music/New", 1), &err, 0, 0) == T_ERROR && err == E_NOT_EMPTY, "DELETE non-empty -> E_NOT_EMPTY");
    CHECK(rpc(&c, T_DELETE, 105, pl, p_path(pl, 0, 1, "Music/New", 1), &err, 0, 0) == T_OK, "DELETE recursive (2500 files)");
    snprintf(p, sizeof p, "%s/sd/Music/New", W); CHECK(!exists(p), "folder gone");
    CHECK(rpc(&c, T_DELETE, 107, pl, p_path(pl, 0, 1, "", 1), &err, 0, 0) == T_ERROR && err == E_BAD_PATH, "DELETE of the root -> E_BAD_PATH");
    CHECK(rpc(&c, T_DELETE, 109, pl, p_path(pl, 0, 0, "empty.bin", 1), &err, 0, 0) == T_OK, "DELETE file");
    CHECK(rpc(&c, T_DELETE, 111, pl, p_path(pl, 0, 0, "flink.txt", 1), &err, 0, 0) == T_ERROR && err == E_BAD_PATH, "DELETE of a symlink -> E_BAD_PATH");

    /* namespaces */
    CHECK(put(&c, 113, 1, PM_REPLACE, "evil.sh", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_BAD_PATH, "NS_UPDATE: other names -> E_BAD_PATH");
    CHECK(put(&c, 115, 1, PM_REPLACE, "hibyos-update.json", (const uint8_t *)"{}", 2, 0, 0, &ready, &err, &res) == T_PUT_DONE, "NS_UPDATE: hibyos-update.json accepted");
    snprintf(p, sizeof p, "%s/sd/ByOS/ota/sync-update.json", W); CHECK(exists(p), "stored as ByOS/ota/sync-update.json");
    CHECK(rpc(&c, T_UPDATE_STAGE, 117, "\0", 1, &err, 0, 0) == T_ERROR && err == E_SIGNATURE, "UPDATE_STAGE of an unsigned JSON -> E_SIGNATURE");
    snprintf(p, sizeof p, "%s/sd/ByOS/ota/sync-update.json", W); unlink(p);
    CHECK(rpc(&c, T_UPDATE_STAGE, 119, "\0", 1, &err, 0, 0) == T_ERROR && err == E_NOT_STAGED, "UPDATE_STAGE with nothing uploaded -> E_NOT_STAGED");
    CHECK(rpc(&c, T_UPDATE_INSTALL, 121, "\0", 1, &err, 0, 0) == T_ERROR && err == E_NOT_STAGED, "UPDATE_INSTALL with nothing staged -> E_NOT_STAGED");
    CHECK(put(&c, 123, 1, PM_REPLACE, "hibyos-payload-0.9.0.tar", bigf, 1000, 0, 0, &ready, &err, &res) == T_PUT_DONE, "NS_UPDATE: payload tar name accepted");
    CHECK(rpc(&c, T_UPDATE_STATUS, 125, 0, 0, &err, buf, &on) == T_UPDATE_STATUS_R && buf[0] == 0, "UPDATE_STATUS idle");
    CHECK(put(&c, 127, 2, PM_REPLACE, "other.db", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_BAD_PATH, "NS_DB: only staged.db");
    CHECK(rpc(&c, T_DB_STAGE, 129, pl, 33, &err, 0, 0) == T_ERROR && err == E_NOT_STAGED, "DB_STAGE without upload -> E_NOT_STAGED");
    /* DB: snapshot, stage stale, stage ok */
    {
        uint8_t lsha[32]; btp_sha256(live, sizeof live, lsha);
        cli_send(&c, T_DB_SNAPSHOT, 0, 131, 0, 0);
        size_t gotb = 0; uint8_t gs[32] = { 0 }; int ok = 0;
        for (;;) { int tt, f; uint32_t s, l; uint8_t *pp; if (cli_recv(&c, &tt, &f, &s, &pp, &l)) break; if (tt == T_DATA) gotb += l - 8; if (tt == T_GET_END) { memcpy(gs, pp, 32); ok = 1; break; } if (f & 1) break; }
        CHECK(ok && gotb == sizeof live && !memcmp(gs, lsha, 32), "DB_SNAPSHOT streams the live DB with its sha (revision)");
        snprintf(p, sizeof p, "%s/data/hibymod/sync/db/snap.db", W); CHECK(!exists(p), "snapshot temp file removed");
        uint8_t nd[4096] = { 0 }; memcpy(nd, "SQLite format 3", 16); nd[16] = 0x10; nd[100] = 42;
        CHECK(put(&c, 133, 2, PM_REPLACE, "staged.db", nd, sizeof nd, 0, 0, &ready, &err, &res) == T_PUT_DONE, "NS_DB: staged.db uploaded");
        uint8_t q[33] = { 0 }; memset(q + 1, 0xAA, 32);
        CHECK(rpc(&c, T_DB_STAGE, 135, q, 33, &err, 0, 0) == T_ERROR && err == E_DB_STALE, "DB_STAGE with another base -> E_DB_STALE");
        memcpy(q + 1, gs, 32);
        CHECK(rpc(&c, T_DB_STAGE, 137, q, 33, &err, buf, &on) == T_DB_STATUS_R && buf[0] == 1 && !memcmp(buf + 33, lsha, 32) && rd64(buf + 65) == sizeof live, "DB_STAGE ok -> DB_STATUS_R staged");
        uint8_t nsha[32]; btp_sha256(nd, sizeof nd, nsha); CHECK(!memcmp(buf + 1, nsha, 32), "staged sha reported");
        CHECK(!ctl_wait("DBSTAGED 4096", line, sizeof line), "shell told: DBSTAGED");
        CHECK(rpc(&c, T_DB_UNSTAGE, 139, 0, 0, &err, buf, &on) == T_DB_STATUS_R && buf[0] == 0, "DB_UNSTAGE");
        uint8_t bad[1000] = "not a db";
        CHECK(put(&c, 141, 2, PM_REPLACE, "staged.db", bad, sizeof bad, 0, 0, &ready, &err, &res) == T_PUT_DONE && rpc(&c, T_DB_STAGE, 143, q, 33, &err, 0, 0) == T_ERROR && err == E_DB_INVALID, "DB_STAGE of a non-SQLite file -> E_DB_INVALID");
    }
    /* LIB_RESCAN paths through the stub */
    {
        uint8_t q[200]; struct bwr w; bwr_init(&w, q, sizeof q); bwr_u8(&w, 1); bwr_u16(&w, 2); bwr_str16(&w, "Music/a.flac", 12); bwr_str16(&w, "Music/b.flac", 12);
        CHECK(rpc(&c, T_LIB_RESCAN, 145, q, w.n, &err, 0, 0) == T_OK, "LIB_RESCAN paths");
        snprintf(p, sizeof p, "%s/dbupdate.log", W); fc = rfile(p, &fl); CHECK(fc && strstr((char *)fc, "/sd/Music/a.flac") && strstr((char *)fc, "/sd/Music/b.flac"), "hibytag dbupdate got absolute SD paths"); free(fc);
        bwr_init(&w, q, sizeof q); bwr_u8(&w, 0); bwr_u16(&w, 0);
        cli_send(&c, T_LIB_RESCAN, 0, 147, q, w.n);
        CHECK(!ctl_wait("RESCAN", line, sizeof line), "full rescan asked to the shell");
        ctl_write("RESCAN_RESULT unsupported\n");
        int tt, f; uint32_t s, l; uint8_t *pp; CHECK(!cli_recv(&c, &tt, &f, &s, &pp, &l) && s == 147 && tt == T_ERROR && rd16(pp) == E_UNSUPPORTED, "shell answer -> E_UNSUPPORTED");
    }
    CHECK(rpc(&c, T_STATUS, 149, 0, 0, &err, buf, &on) == T_STATUS_R && buf[0] == 87 && buf[1] == 1 && buf[2] == 0, "STATUS battery 87, charging, SD mounted");

    /* SD gone (USB storage): refused; DF still answers */
    snprintf(ml, sizeof ml, "/dev/root / ext4 rw 0 0\n"); wfile(mounts, ml, strlen(ml));
    CHECK(rpc(&c, T_STAT, 151, pl, p_path(pl, 0, 0, "Music", 0), &err, 0, 0) == T_ERROR && err == E_SD_UNAVAILABLE, "SD not mounted -> E_SD_UNAVAILABLE");
    CHECK(put(&c, 153, 0, PM_CREATE, "x.bin", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_ERROR && err == E_SD_UNAVAILABLE, "PUT refused without SD");
    CHECK(rpc(&c, T_DF, 155, 0, 0, &err, buf, &on) == T_DF_R && buf[0] == 1 && rd64(buf + 2) == 0, "DF works without SD (sd_state 1)");
    snprintf(ml, sizeof ml, "/dev/mmcblk0p1 %s/sd vfat rw 0 0\n", W); wfile(mounts, ml, strlen(ml));
    ctl_write("SD 2\n"); usleep(100000);
    CHECK(rpc(&c, T_STAT, 157, pl, p_path(pl, 0, 0, "Music", 0), &err, 0, 0) == T_ERROR && err == E_SD_UNAVAILABLE, "shell says USB storage -> E_SD_UNAVAILABLE");
    CHECK(rpc(&c, T_DF, 159, 0, 0, &err, buf, &on) == T_DF_R && buf[0] == 2, "DF sd_state 2 (USB storage)");
    ctl_write("SD 0\n"); usleep(100000);
    CHECK(rpc(&c, T_DF, 161, 0, 0, &err, buf, &on) == T_DF_R && buf[0] == 0 && buf[1] == 1, "SD back, vfat");
    CHECK(put(&c, 163, 0, PM_CREATE, "toobig.bin", (const uint8_t *)"x", 1, 0, 0, &ready, &err, &res) == T_PUT_DONE, "small PUT on vfat ok");
    { uint8_t s2[32] = { 0 }; CHECK(rpc(&c, T_PUT_BEGIN, 165, pl, p_put(pl, 0, PM_CREATE, 4294967296ULL, 0, s2, "4g.bin"), &err, 0, 0) == T_ERROR && err == E_FILE_TOO_BIG, "4 GiB on vfat -> E_FILE_TOO_BIG"); }

    /* busy: third session */
    struct cli c2, c3;
    CHECK(!cli_connect("127.0.0.1", 17340, &c2) && !hello(&c2, 0), "second session");
    CHECK(!cli_connect("127.0.0.1", 17340, &c3), "third connect");
    { int tt, f; uint32_t s, l; uint8_t *pp; CHECK(!cli_recv(&c3, &tt, &f, &s, &pp, &l) && tt == T_ERROR && rd16(pp) == E_BUSY, "third session -> E_BUSY"); close(c3.fd); }
    /* two sessions, same PUT (same part): the second takes the part over, the first gets E_CANCELLED */
    {
        static uint8_t tk[300000]; for (size_t i = 0; i < sizeof tk; i++) tk[i] = (uint8_t)(i * 13 + 1);
        t = put(&c, 211, 0, PM_CREATE, "Music/take.bin", tk, sizeof tk, 0, 140000, &ready, &err, &res);
        CHECK(t == -2 && ready == 0, "session A: PUT started, 140000 bytes sent, still open");
        usleep(100000);
        t = put(&c2, 213, 0, PM_CREATE, "Music/take.bin", tk, sizeof tk, 0, 0, &ready, &err, &res);
        CHECK(t == T_PUT_DONE && ready == 131072, "session B: same PUT resumes at 131072 and completes (ready=%llu)", (unsigned long long)ready);
        int tt, f; uint32_t s, l; uint8_t *pp;
        CHECK(!cli_recv(&c, &tt, &f, &s, &pp, &l) && s == 211 && tt == T_ERROR && rd16(pp) == E_CANCELLED, "session A's PUT -> E_CANCELLED (taken over)");
        snprintf(p, sizeof p, "%s/sd/Music/take.bin", W); fc = rfile(p, &fl); CHECK(fc && fl == sizeof tk && !memcmp(fc, tk, fl), "taken-over file intact"); free(fc);
        CHECK(rpc(&c, T_STAT, 215, pl, p_path(pl, 0, 0, "Music/take.bin", 0), &err, buf, &on) == T_STAT_R, "session A still usable");
        /* skip-identical on a bigger file goes through the incremental hash job */
        CHECK(put(&c2, 217, 0, PM_SKIP_IDENTICAL, "Music/take.bin", tk, sizeof tk, 0, 0, &ready, &err, &res) == T_PUT_DONE && res == 1, "PUT skip identical (300000 bytes, hashed as a job)");
        tk[299999] ^= 1;
        CHECK(put(&c2, 219, 0, PM_SKIP_IDENTICAL, "Music/take.bin", tk, sizeof tk, 0, 0, &ready, &err, &res) == T_ERROR && err == E_EXISTS, "PUT skip, last byte differs -> E_EXISTS");
    }
    close(c2.fd);

    /* ---------- Wi-Fi (127.0.0.2 stands for wlan0) */
    struct cli w;
    int dflags;
    usleep(200000);
    CHECK(!cli_connect("127.0.0.2", 17340, &w) && !hello(&w, &dflags), "wifi connect + HELLO");
    CHECK(!(dflags & HF_LOCAL), "remote session is not LOCAL");
    CHECK(rpc(&w, T_DF, 1, 0, 0, &err, 0, 0) == T_ERROR && err == E_AUTH_REQUIRED, "remote DF before AUTH -> E_AUTH_REQUIRED");
    { int tt, f; uint32_t s, l; uint8_t *pp; CHECK(cli_recv(&w, &tt, &f, &s, &pp, &l) == -1, "and the connection is closed"); close(w.fd); }
    /* pairing while closed */
    uint8_t cs[32], Cs[32], ce[32], Ce[32], Nc[16], hc[32], hd[32], commit[32];
    btp_random(cs, 32); btp_x25519_base(Cs, cs);
    CHECK(!cli_connect("127.0.0.2", 17340, &w) && !hello_full(&w, hc, hd, &dflags), "wifi HELLO");
    btp_random(ce, 32); btp_x25519_base(Ce, ce); btp_random(Nc, 16); btp_pair_commit(Ce, Nc, commit);
    CHECK(rpc(&w, T_PAIR_COMMIT, 0, commit, 32, &err, 0, 0) == T_ERROR && err == E_PAIR_CLOSED, "PAIR_COMMIT while pairing closed -> E_PAIR_CLOSED");
    close(w.fd); usleep(100000);
    /* pairing: commit, device key, reveal, SAS from the control pipe, ALLOW, confirm */
    ctl_write("PAIRING on\n"); usleep(100000);
    CHECK(!cli_connect("127.0.0.2", 17340, &w) && !hello_full(&w, hc, hd, &dflags) && (dflags & HF_PAIRING_OPEN), "wifi HELLO with PAIRING_OPEN");
    cli_send(&w, T_PAIR_COMMIT, 0, 0, commit, 32);
    { int tt, f; uint32_t s, l; uint8_t *pp, De[32], Nd[16];
      CHECK(!cli_recv(&w, &tt, &f, &s, &pp, &l) && tt == T_PAIR_DEVICE && l == 48, "PAIR_DEVICE");
      memcpy(De, pp, 32); memcpy(Nd, pp + 32, 16);
      uint8_t rv[48]; memcpy(rv, Ce, 32); memcpy(rv + 32, Nc, 16); cli_send(&w, T_PAIR_REVEAL, 0, 0, rv, 48);
      uint8_t th[32], ss[32], prk[32]; char sas[7];
      btp_pair_th(hc, hd, commit, De, Nd, Ce, Nc, th); btp_x25519_dh(ss, ce, De); btp_hkdf_extract(th, 32, ss, 32, prk); btp_sas(prk, sas);
      btp_keys(prk, w.ktx, w.krx); w.enc = 1;
      CHECK(!ctl_wait("SAS ", line, sizeof line), "SAS line on the control pipe");
      int sid = 0; char code[16] = "", nm[80] = ""; sscanf(line, "SAS %d %15s %79s", &sid, code, nm);
      CHECK(!strcmp(code, sas) && !strcmp(nm, "Laptop"), "the shell gets the same 6-digit code as the client (%s / %s) and the client name", code, sas);
      uint8_t cf[64]; struct bwr bw; bwr_init(&bw, cf, sizeof cf); bwr_bytes(&bw, Cs, 32); bwr_str16(&bw, "Laptop", 6);
      cli_send(&w, T_PAIR_CONFIRM, 0, 0, cf, bw.n);
      usleep(100000);
      char al[40]; snprintf(al, sizeof al, "ALLOW %d\n", sid); ctl_write(al);
      CHECK(!cli_recv(&w, &tt, &f, &s, &pp, &l) && tt == T_PAIR_RESULT && pp[0] == 0, "PAIR_RESULT (encrypted)");
      uint8_t Ds[32]; memcpy(Ds, pp + 1, 32); uint8_t did[8]; btp_device_id(Ds, did);
      CHECK(!memcmp(did, pp + 33, 8), "device_id = SHA-256(Ds)[0..8]");
      CHECK(!ctl_wait("PAIRED ", line, sizeof line), "PAIRED line");
      CHECK(rpc(&w, T_DF, 1, 0, 0, &err, buf, &on) == T_DF_R, "encrypted DF after pairing");
      close(w.fd); usleep(200000);
      snprintf(p, sizeof p, "%s/data/hibymod/sync/peers", W); fc = rfile(p, &fl); char hx[65]; hexs(Cs, 32, hx);
      CHECK(fc && strstr((char *)fc, hx) && strstr((char *)fc, "Laptop"), "peer stored"); free(fc);

      /* AUTH reconnect, encrypted PUT and GET */
      CHECK(!cli_connect("127.0.0.2", 17340, &w) && !hello_full(&w, hc, hd, 0), "wifi reconnect");
      uint8_t kid[16], ai[48]; btp_key_id(Cs, kid); btp_random(ce, 32); btp_x25519_base(Ce, ce);
      memcpy(ai, kid, 16); memcpy(ai + 16, Ce, 32); cli_send(&w, T_AUTH_INIT, 0, 0, ai, 48);
      CHECK(!cli_recv(&w, &tt, &f, &s, &pp, &l) && tt == T_AUTH_REPLY && l == 32, "AUTH_REPLY");
      memcpy(De, pp, 32);
      uint8_t ikm[96]; btp_x25519_dh(ikm, ce, De); btp_x25519_dh(ikm + 32, cs, De); btp_x25519_dh(ikm + 64, ce, Ds);
      btp_auth_th(hc, hd, kid, Ce, De, Cs, Ds, th); btp_hkdf_extract(th, 32, ikm, 96, prk); btp_keys(prk, w.ktx, w.krx); w.enc = 1;
      cli_send(&w, T_AUTH_FINISH, 0, 0, 0, 0);
      CHECK(!cli_recv(&w, &tt, &f, &s, &pp, &l) && tt == T_AUTH_OK, "AUTH_OK opens: the device proved Ds");
      CHECK(put(&w, 3, 0, PM_CREATE, "Music/wifi.bin", bigf, 150000, 0, 0, &ready, &err, &res) == T_PUT_DONE, "encrypted PUT 150000 bytes");
      CHECK(get(&w, 5, "Music/wifi.bin", 0, 0, gb, sizeof gb, &got, sha, &err) == T_GET_END && got == 150000 && !memcmp(gb, bigf, 150000), "encrypted GET (no sendfile) content ok");
      close(w.fd); usleep(100000);

      /* unknown key, tampered record */
      CHECK(!cli_connect("127.0.0.2", 17340, &w) && !hello_full(&w, hc, hd, 0), "wifi connect (unknown key)");
      memset(ai, 0x55, 16); cli_send(&w, T_AUTH_INIT, 0, 0, ai, 48);
      CHECK(!cli_recv(&w, &tt, &f, &s, &pp, &l) && tt == T_ERROR && rd16(pp) == E_AUTH_FAILED, "unknown key_id -> E_AUTH_FAILED");
      close(w.fd); usleep(100000);
      CHECK(!cli_connect("127.0.0.2", 17340, &w) && !hello_full(&w, hc, hd, 0), "wifi connect (tamper)");
      btp_random(ce, 32); btp_x25519_base(Ce, ce); memcpy(ai, kid, 16); memcpy(ai + 16, Ce, 32); cli_send(&w, T_AUTH_INIT, 0, 0, ai, 48);
      cli_recv(&w, &tt, &f, &s, &pp, &l); memcpy(De, pp, 32);
      btp_x25519_dh(ikm, ce, De); btp_x25519_dh(ikm + 32, cs, De); btp_x25519_dh(ikm + 64, ce, Ds);
      btp_auth_th(hc, hd, kid, Ce, De, Cs, Ds, th); btp_hkdf_extract(th, 32, ikm, 96, prk); btp_keys(prk, w.ktx, w.krx); w.enc = 1;
      w.ktx[0] ^= 1;   /* wrong key = forged record */
      cli_send(&w, T_AUTH_FINISH, 0, 0, 0, 0);
      CHECK(cli_recv(&w, &tt, &f, &s, &pp, &l) == -1, "forged record: closed without an answer");
      close(w.fd);
    }
    /* slots and unauthenticated Wi-Fi sessions (c, the USB session, holds one slot) */
    usleep(200000);
    {
        struct cli w1, w2, u4; int tt, f; uint32_t s, l; uint8_t *pp;
        CHECK(!cli_connect("127.0.0.2", 17340, &w1) && !hello(&w1, 0), "wifi: unauthenticated session 1 (HELLO)");
        cli_send(&w1, T_PING, 0, 0, ping, 8); CHECK(!cli_recv(&w1, &tt, &f, &s, &pp, &l) && tt == T_PONG, "it may PING");
        CHECK(!cli_connect("127.0.0.2", 17340, &w2) && !cli_recv(&w2, &tt, &f, &s, &pp, &l) && tt == T_ERROR && rd16(pp) == E_BUSY, "a second unauthenticated wifi session -> E_BUSY");
        close(w2.fd);
        CHECK(!cli_connect("127.0.0.1", 17340, &u4) && !hello(&u4, 0), "slots full: a USB client still gets in");
        CHECK(!cli_recv(&w1, &tt, &f, &s, &pp, &l) && tt == T_ERROR && rd16(pp) == E_BUSY && cli_recv(&w1, &tt, &f, &s, &pp, &l) == -1, "and the unauthenticated wifi session is evicted (E_BUSY, closed)");
        close(w1.fd);
        CHECK(rpc(&u4, T_DF, 1, 0, 0, &err, 0, 0) == T_DF_R, "the USB client works");
        close(u4.fd); usleep(200000);
        /* PINGs do not keep an unauthenticated session alive: closed after 3 s (test deadline) */
        CHECK(!cli_connect("127.0.0.2", 17340, &w1) && !hello(&w1, 0), "wifi: unauthenticated session that only PINGs");
        long long t0 = now_ms(); int closed = 0, timeout = 0;
        while (now_ms() - t0 < 8000) {
            if (cli_send(&w1, T_PING, 0, 0, ping, 8)) { closed = 1; break; }
            int r = cli_recv(&w1, &tt, &f, &s, &pp, &l);
            if (r) { closed = 1; break; }
            if (tt == T_ERROR) { timeout = rd16(pp) == E_TIMEOUT; }
            usleep(400000);
        }
        long long el = now_ms() - t0;
        CHECK(closed && timeout && el >= 2500 && el < 6000, "closed with E_TIMEOUT after the securing deadline (%lld ms)", el);
        close(w1.fd);
        /* PAIR_COMMIT without a reveal does not hold the pairing slot forever */
        ctl_write("PAIRING on\n"); usleep(100000);
        uint8_t cm[32]; memset(cm, 7, 32);
        CHECK(!cli_connect("127.0.0.2", 17340, &w1) && !hello(&w1, 0), "wifi: commit, never reveal");
        cli_send(&w1, T_PAIR_COMMIT, 0, 0, cm, 32);
        CHECK(!cli_recv(&w1, &tt, &f, &s, &pp, &l) && tt == T_PAIR_DEVICE, "PAIR_DEVICE");
        CHECK(!cli_recv(&w1, &tt, &f, &s, &pp, &l) && tt == T_ERROR && rd16(pp) == E_TIMEOUT, "commit without reveal -> E_TIMEOUT at the securing deadline");
        close(w1.fd); usleep(200000);
        CHECK(!cli_connect("127.0.0.2", 17340, &w1) && !hello(&w1, &dflags) && (dflags & HF_PAIRING_OPEN), "the pairing window is still open (an unrevealed commit is not a failure)");
        cli_send(&w1, T_PAIR_COMMIT, 0, 0, cm, 32);
        CHECK(!cli_recv(&w1, &tt, &f, &s, &pp, &l) && tt == T_PAIR_DEVICE, "and the pairing slot is free again");
        close(w1.fd); ctl_write("PAIRING off\n"); usleep(200000);
    }
    /* UDP discovery */
    {
        int u = socket(AF_INET, SOCK_DGRAM, 0); struct timeval tv = { 2, 0 }; setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct sockaddr_in to = { 0 }; to.sin_family = AF_INET; to.sin_port = htons(17340); to.sin_addr.s_addr = inet_addr("127.0.0.2");
        uint8_t pr[16] = { 'B', 'T', 'P', 'D', 1, 1, 0, 0, 9, 8, 7, 6, 5, 4, 3, 2 }, an[128];
        sendto(u, pr, 16, 0, (struct sockaddr *)&to, sizeof to);
        ssize_t n = recv(u, an, sizeof an, 0);
        CHECK(n >= 27 && !memcmp(an, "BTPD", 4) && an[5] == 2 && !memcmp(an + 8, pr + 8, 8) && rd16(an + 16) == 17340 && an[26] == 7 && !memcmp(an + 27, "Test R3", 7), "UDP PROBE -> ANNOUNCE with nonce, port, name");
        CHECK((rd16(an + 6) & 4) != 0, "ANNOUNCE says HAS_PEERS");
        pr[4] = 2; sendto(u, pr, 16, 0, (struct sockaddr *)&to, sizeof to);
        CHECK(recv(u, an, sizeof an, 0) < 0, "bad probe ignored");
        close(u);
    }
    /* PEERS / FORGET */
    ctl_write("PEERS\n");
    CHECK(!ctl_wait("PEER ", line, sizeof line) && strstr(line, "Laptop"), "PEERS lists the paired client");
    { char kh[40] = ""; sscanf(line, "PEER %39s", kh); char fl2[80]; snprintf(fl2, sizeof fl2, "FORGET %s\n", kh); ctl_write(fl2); usleep(100000); }
    snprintf(p, sizeof p, "%s/data/hibymod/sync/peers", W); fc = rfile(p, &fl); CHECK(!fc || !strstr((char *)fc, "Laptop"), "FORGET removes the peer"); free(fc);

    /* QUIT */
    close(c.fd);
    ctl_write("QUIT\n");
    CHECK(!ctl_wait("BYE", line, sizeof line), "QUIT -> BYE");
    CHECK(!wait_exit(pid, 3000), "daemon exits after QUIT");
    close(ctl_fd);
    snprintf(p, sizeof p, "%s/data/hibymod/sync/device.key", W); CHECK(!stat(p, &st) && (st.st_mode & 0777) == 0600 && st.st_size == 32, "device.key 0600");

    /* control EOF: the shell went away */
    pid = start_daemon(bin, W, 0);
    struct cli z; CHECK(!cli_connect("127.0.0.1", 17340, &z) && !hello(&z, 0), "second run: same key, connect");
    close(ctl_fd);
    CHECK(!wait_exit(pid, 3000), "daemon exits when the control pipe closes");
    close(z.fd);
    /* SIGTERM with an open PUT: part kept */
    pid = start_daemon(bin, W, 0);
    CHECK(!cli_connect("127.0.0.1", 17340, &z) && !hello(&z, 0), "third run");
    t = put(&z, 1, 0, PM_CREATE, "Music/term.bin", bigf, sizeof bigf, 0, 131072, &ready, &err, &res);
    usleep(200000); kill(pid, SIGTERM);
    { int tt, f; uint32_t s, l; uint8_t *pp; int sawc = 0, sawbye = 0;
      for (int i = 0; i < 10; i++) { if (cli_recv(&z, &tt, &f, &s, &pp, &l)) break; if (tt == T_ERROR && rd16(pp) == E_CANCELLED) sawc = 1; if (tt == T_BYE) sawbye = 1; }
      CHECK(sawc && sawbye, "SIGTERM: E_CANCELLED on the PUT stream and BYE"); }
    CHECK(!wait_exit(pid, 2000), "SIGTERM: exits within 1-2 s");
    btp_sha256(bigf, sizeof bigf, bsha); btp_part_name((const uint8_t *)"Music/term.bin", 14, bsha, sizeof bigf, part);
    snprintf(p, sizeof p, "%s/sd/Music/%s", W, part); CHECK(!stat(p, &st) && st.st_size == 131072, "SIGTERM: part kept for resume");
    close(z.fd); close(ctl_fd);

    /* a long synchronous request (hibytag dbcheck taking 5 s) with idle and PUT-idle timeouts of
     * 3 s: neither the requesting session nor another session's open PUT may be timed out */
    short_idle = 1; pid = start_daemon(bin, W, 0); short_idle = 0;
    {
        struct cli a, b; int tt, f; uint32_t s, l; uint8_t *pp;
        CHECK(!cli_connect("127.0.0.1", 17340, &a) && !hello(&a, 0) && !cli_connect("127.0.0.1", 17340, &b) && !hello(&b, 0), "stall run: two sessions");
        t = put(&b, 1, 0, PM_CREATE, "Music/stall.bin", bigf, sizeof bigf, 0, 65536, &ready, &err, &res);
        CHECK(t == -2, "session B: PUT open, 65536 bytes sent");
        uint8_t nd[4096] = { 0 }; memcpy(nd, "SQLite format 3", 16); nd[16] = 0x10;
        CHECK(put(&a, 1, 2, PM_REPLACE, "staged.db", nd, sizeof nd, 0, 0, &ready, &err, &res) == T_PUT_DONE, "session A: staged.db uploaded");
        snprintf(p, sizeof p, "%s/slowcheck", W); wfile(p, "", 0);
        uint8_t q[33] = { 1 };
        struct timeval tv15 = { 15, 0 }; setsockopt(a.fd, SOL_SOCKET, SO_RCVTIMEO, &tv15, sizeof tv15);
        long long t0 = now_ms();
        int rt = rpc(&a, T_DB_STAGE, 3, q, 33, &err, buf, &on);
        unlink(p);
        CHECK(rt == T_DB_STATUS_R && now_ms() - t0 >= 4500, "session A: DB_STAGE answered after the 5 s dbcheck (%lld ms)", now_ms() - t0);
        usleep(300000);
        CHECK(rpc(&a, T_DF, 5, 0, 0, &err, 0, 0) == T_DF_R, "session A not timed out by the time it spent waiting");
        static uint8_t d[8 + 65536]; int okd = 1;
        for (uint64_t off = 65536; off < sizeof bigf; ) {
            size_t n = sizeof bigf - off > 65536 ? 65536 : sizeof bigf - off;
            be64(d, off); memcpy(d + 8, bigf + off, n); if (cli_send(&b, T_DATA, 0, 1, d, 8 + n)) okd = 0; off += n;
        }
        CHECK(okd && rpc(&b, T_PUT_END, 1, 0, 0, &err, 0, 0) == T_PUT_DONE, "session B: its PUT survived the stall and completes");
        (void)tt; (void)f; (void)s; (void)l; (void)pp;
        close(a.fd); close(b.fd);
    }
    ctl_write("QUIT\n"); CHECK(!wait_exit(pid, 3000), "stall run: exits");
    close(ctl_fd);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    paths_init(); snprintf(G.name, sizeof G.name, "HiBy R3 II");
    if (argc >= 3 && !strcmp(argv[1], "vectors")) vectors(argv[2]);
    else if (argc >= 4 && !strcmp(argv[1], "daemon")) daemon_tests(argv[2], argv[3]);
    else { fprintf(stderr, "usage: btp_test vectors FILE | daemon HIBYSYNC_X86 WORKDIR\n"); return 2; }
    printf("  %d checks passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
