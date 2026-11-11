/* ota_net.inc.c - ByOS online updates (OTA) from GitHub releases. See docs/OTA.md.
 *
 * Core (no UI, also built on the PC by tests/ota_test.c):
 *   - SHA-256 and SHA-512;
 *   - Ed25519 signature verification (derived from TweetNaCl, public domain:
 *     https://tweetnacl.cr.yp.to - Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers),
 *     plus the RFC 8032 check S < L (no malleable signatures);
 *   - a tiny bounded JSON reader for hibyos-update.json;
 *   - version comparison (1.2.10 > 1.2.9, 1.0.0-beta < 1.0.0);
 *   - a strict ustar extractor (regular files and folders under "payload/" only: no links,
 *     no "..", no absolute paths, size limits);
 *   - MANIFEST.md5 diff -> restart needed (none / interface / reboot).
 * Worker (#ifndef OTA_CORE_ONLY): one background thread runs a whole operation (check,
 * download + install, restore previous version); the UI only reads ota_st. Network = the
 * hibynet helper (HIBYNET_RESUME=1: an interrupted download continues with Range).
 *
 * Trust: hibyos-update.json is signed with the release key (Ed25519, public key below).
 * The JSON carries the size and sha256 of the payload tar, so the tar is covered too.
 * The signed message is the JSON file itself with the 128 hex digits of its "signature"
 * value replaced by '0' (tools/ota_sign.py does the same), so the file stays readable JSON. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>

/* Release signing public keys (Ed25519, 64 hex digits). The private key NEVER goes in the
 * repository: `python tools/ota_sign.py genkey --key PATH --install` creates it outside the
 * repo and rewrites the OTA_PUBKEY line. All zeros = online updates disabled in this build.
 * OTA_PUBKEY2 is for key rotation (a release signed with either key is accepted). */
#ifndef OTA_PUBKEY_HEX
#define OTA_PUBKEY_HEX "6d87851ece77b81547cfbde491d9c46494dc99e70922c48115bfe5d9e854e06e" /* OTA_PUBKEY */
#endif
#ifndef OTA_PUBKEY2_HEX
#define OTA_PUBKEY2_HEX "" /* OTA_PUBKEY2 */
#endif

#define OTA_URL_LATEST "https://github.com/Grohle/hibyos/releases/latest/download/hibyos-update.json"
#define OTA_JSON_MAX (64 * 1024)
#define OTA_PAYLOAD_MAX (64LL * 1024 * 1024)
#define OTA_TAR_FILES_MAX 8192
#define OTA_MIN_BATT 30

/* ================================================================== SHA-256 */
struct ota_sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; };
static const uint32_t OTA_K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define OTA_R32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void ota_sha256_blk(struct ota_sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = OTA_R32(w[i - 15], 7) ^ OTA_R32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = OTA_R32(w[i - 2], 17) ^ OTA_R32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (OTA_R32(e, 6) ^ OTA_R32(e, 11) ^ OTA_R32(e, 25)) + ((e & f) ^ (~e & g)) + OTA_K256[i] + w[i];
        uint32_t t2 = (OTA_R32(a, 2) ^ OTA_R32(a, 13) ^ OTA_R32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void ota_sha256_init(struct ota_sha256 *s)
{
    static const uint32_t h0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0); s->len = 0; s->n = 0;
}
static void ota_sha256_update(struct ota_sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        if (!s->n && n >= 64) { ota_sha256_blk(s, p); p += 64; n -= 64; continue; }
        size_t k = 64 - (size_t)s->n; if (k > n) k = n;
        memcpy(s->buf + s->n, p, k); s->n += (int)k; p += k; n -= k;
        if (s->n == 64) { ota_sha256_blk(s, s->buf); s->n = 0; }
    }
}
static void ota_sha256_final(struct ota_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8; uint8_t pad = 0x80, z = 0, L[8];
    ota_sha256_update(s, &pad, 1); while (s->n != 56) ota_sha256_update(s, &z, 1);
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    ota_sha256_update(s, L, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
static void ota_hex(const uint8_t *b, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = hx[b[i] >> 4]; out[2 * i + 1] = hx[b[i] & 15]; }
    out[2 * n] = 0;
}
static int ota_unhex(const char *s, uint8_t *out, int n)   /* exactly 2n hex digits */
{
    for (int i = 0; i < 2 * n; i++) if (!isxdigit((unsigned char)s[i])) return -1;
    for (int i = 0; i < n; i++) { unsigned v; char t[3] = { s[2 * i], s[2 * i + 1], 0 }; v = (unsigned)strtoul(t, 0, 16); out[i] = (uint8_t)v; }
    return 0;
}

/* ================================================================== SHA-512 */
struct ota_sha512 { uint64_t h[8]; uint64_t len; uint8_t buf[128]; int n; };
static const uint64_t OTA_K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };
#define OTA_R64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static void ota_sha512_blk(struct ota_sha512 *s, const uint8_t *p)
{
    uint64_t w[80], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) { uint64_t v = 0; for (int k = 0; k < 8; k++) v = v << 8 | p[8 * i + k]; w[i] = v; }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = OTA_R64(w[i - 15], 1) ^ OTA_R64(w[i - 15], 8) ^ (w[i - 15] >> 7), s1 = OTA_R64(w[i - 2], 19) ^ OTA_R64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + (OTA_R64(e, 14) ^ OTA_R64(e, 18) ^ OTA_R64(e, 41)) + ((e & f) ^ (~e & g)) + OTA_K512[i] + w[i];
        uint64_t t2 = (OTA_R64(a, 28) ^ OTA_R64(a, 34) ^ OTA_R64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void ota_sha512_init(struct ota_sha512 *s)
{
    static const uint64_t h0[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    memcpy(s->h, h0, sizeof h0); s->len = 0; s->n = 0;
}
static void ota_sha512_update(struct ota_sha512 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        if (!s->n && n >= 128) { ota_sha512_blk(s, p); p += 128; n -= 128; continue; }
        size_t k = 128 - (size_t)s->n; if (k > n) k = n;
        memcpy(s->buf + s->n, p, k); s->n += (int)k; p += k; n -= k;
        if (s->n == 128) { ota_sha512_blk(s, s->buf); s->n = 0; }
    }
}
static void ota_sha512_final(struct ota_sha512 *s, uint8_t out[64])
{
    uint64_t bits = s->len * 8; uint8_t pad = 0x80, z = 0, L[16] = { 0 };
    ota_sha512_update(s, &pad, 1); while (s->n != 112) ota_sha512_update(s, &z, 1);
    for (int i = 0; i < 8; i++) L[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    ota_sha512_update(s, L, 16);
    for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) out[8 * i + k] = (uint8_t)(s->h[i] >> (56 - 8 * k));
}

/* ================================================================== Ed25519 (verify only)
 * Field arithmetic and group law from TweetNaCl (public domain); left shifts of negative
 * values replaced by multiplications (defined behaviour). Not constant time, which is fine:
 * verification only handles public data. */
typedef int64_t ota_gf[16];
static const ota_gf OTA_GF0 = { 0 }, OTA_GF1 = { 1 };
static const ota_gf OTA_D = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 },
    OTA_D2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 },
    OTA_X = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 },
    OTA_Y = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 },
    OTA_I = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };
static const int64_t OTA_L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

static void gf_set(ota_gf r, const ota_gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
static void gf_car(ota_gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += 65536;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void gf_sel(ota_gf p, ota_gf q, int b)
{
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void gf_pack(uint8_t *o, const ota_gf n)
{
    ota_gf m, t;
    gf_set(t, n); gf_car(t); gf_car(t); gf_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        gf_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static int gf_neq(const ota_gf a, const ota_gf b) { uint8_t c[32], d[32]; gf_pack(c, a); gf_pack(d, b); return memcmp(c, d, 32) != 0; }
static int gf_par(const ota_gf a) { uint8_t d[32]; gf_pack(d, a); return d[0] & 1; }
static void gf_unpack(ota_gf o, const uint8_t *n) { for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8); o[15] &= 0x7fff; }
static void gf_A(ota_gf o, const ota_gf a, const ota_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void gf_Z(ota_gf o, const ota_gf a, const ota_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void gf_M(ota_gf o, const ota_gf a, const ota_gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    gf_car(o); gf_car(o);
}
static void gf_S(ota_gf o, const ota_gf a) { gf_M(o, a, a); }
static void gf_inv(ota_gf o, const ota_gf i)
{
    ota_gf c; gf_set(c, i);
    for (int a = 253; a >= 0; a--) { gf_S(c, c); if (a != 2 && a != 4) gf_M(c, c, i); }
    gf_set(o, c);
}
static void gf_pow2523(ota_gf o, const ota_gf i)
{
    ota_gf c; gf_set(c, i);
    for (int a = 250; a >= 0; a--) { gf_S(c, c); if (a != 1) gf_M(c, c, i); }
    gf_set(o, c);
}
static void ge_add(ota_gf p[4], ota_gf q[4])
{
    ota_gf a, b, c, d, t, e, f, g, h;
    gf_Z(a, p[1], p[0]); gf_Z(t, q[1], q[0]); gf_M(a, a, t);
    gf_A(b, p[0], p[1]); gf_A(t, q[0], q[1]); gf_M(b, b, t);
    gf_M(c, p[3], q[3]); gf_M(c, c, OTA_D2);
    gf_M(d, p[2], q[2]); gf_A(d, d, d);
    gf_Z(e, b, a); gf_Z(f, d, c); gf_A(g, d, c); gf_A(h, b, a);
    gf_M(p[0], e, f); gf_M(p[1], h, g); gf_M(p[2], g, f); gf_M(p[3], e, h);
}
static void ge_cswap(ota_gf p[4], ota_gf q[4], int b) { for (int i = 0; i < 4; i++) gf_sel(p[i], q[i], b); }
static void ge_pack(uint8_t *r, ota_gf p[4])
{
    ota_gf tx, ty, zi;
    gf_inv(zi, p[2]); gf_M(tx, p[0], zi); gf_M(ty, p[1], zi);
    gf_pack(r, ty); r[31] ^= (uint8_t)(gf_par(tx) << 7);
}
static void ge_scalarmult(ota_gf p[4], ota_gf q[4], const uint8_t *s)
{
    gf_set(p[0], OTA_GF0); gf_set(p[1], OTA_GF1); gf_set(p[2], OTA_GF1); gf_set(p[3], OTA_GF0);
    for (int i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        ge_cswap(p, q, b); ge_add(q, p); ge_add(p, p); ge_cswap(p, q, b);
    }
}
static void ge_scalarbase(ota_gf p[4], const uint8_t *s)
{
    ota_gf q[4];
    gf_set(q[0], OTA_X); gf_set(q[1], OTA_Y); gf_set(q[2], OTA_GF1); gf_M(q[3], OTA_X, OTA_Y);
    ge_scalarmult(p, q, s);
}
static void sc_modL(uint8_t *r, int64_t x[64])
{
    int64_t carry; int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * OTA_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) { x[j] += carry - (x[31] >> 4) * OTA_L[j]; carry = x[j] >> 8; x[j] &= 255; }
    for (j = 0; j < 32; j++) x[j] -= carry * OTA_L[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = (uint8_t)(x[i] & 255); }
}
static void sc_reduce(uint8_t *r)   /* r: 64 bytes -> r[0..31] = r mod L */
{
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    sc_modL(r, x);
}
static int ge_unpackneg(ota_gf r[4], const uint8_t p[32])
{
    ota_gf t, chk, num, den, den2, den4, den6;
    gf_set(r[2], OTA_GF1);
    gf_unpack(r[1], p);
    gf_S(num, r[1]); gf_M(den, num, OTA_D); gf_Z(num, num, r[2]); gf_A(den, r[2], den);
    gf_S(den2, den); gf_S(den4, den2); gf_M(den6, den4, den2); gf_M(t, den6, num); gf_M(t, t, den);
    gf_pow2523(t, t); gf_M(t, t, num); gf_M(t, t, den); gf_M(t, t, den); gf_M(r[0], t, den);
    gf_S(chk, r[0]); gf_M(chk, chk, den);
    if (gf_neq(chk, num)) gf_M(r[0], r[0], OTA_I);
    gf_S(chk, r[0]); gf_M(chk, chk, den);
    if (gf_neq(chk, num)) return -1;
    if (gf_par(r[0]) == (p[31] >> 7)) gf_Z(r[0], OTA_GF0, r[0]);
    gf_M(r[3], r[0], r[1]);
    return 0;
}
/* S < L (little endian), RFC 8032 5.1.7 */
static int sc_is_canonical(const uint8_t *s)
{
    for (int i = 31; i >= 0; i--) {
        if (s[i] < OTA_L[i]) return 1;
        if (s[i] > OTA_L[i]) return 0;
    }
    return 0;   /* == L */
}
/* 0 = valid signature of msg[0..n) by pk */
static int ota_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32])
{
    ota_gf p[4], q[4]; uint8_t h[64], t[32];
    if (!sc_is_canonical(sig + 32)) return -1;
    if (ge_unpackneg(q, pk)) return -1;
    struct ota_sha512 s; ota_sha512_init(&s);
    ota_sha512_update(&s, sig, 32); ota_sha512_update(&s, pk, 32); ota_sha512_update(&s, msg, n);
    ota_sha512_final(&s, h);
    sc_reduce(h);
    ge_scalarmult(p, q, h);
    ge_scalarbase(q, sig + 32);
    ge_add(p, q);
    ge_pack(t, p);
    int d = 0; for (int i = 0; i < 32; i++) d |= t[i] ^ sig[i];
    return d ? -1 : 0;
}

/* compiled-in keys (zero key = not configured) */
static int ota_keys(uint8_t keys[2][32])
{
    const char *hx[2] = { OTA_PUBKEY_HEX, OTA_PUBKEY2_HEX }; int n = 0;
    for (int i = 0; i < 2; i++) {
        if (strlen(hx[i]) != 64 || ota_unhex(hx[i], keys[n], 32)) continue;
        int z = 1; for (int k = 0; k < 32; k++) if (keys[n][k]) z = 0;
        if (!z) n++;
    }
    return n;
}

/* ================================================================== JSON (bounded reader) */
static const char *oj_ws(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }
static const char *oj_skip(const char *p, int depth)   /* after the value, or 0 if malformed */
{
    p = oj_ws(p); if (!p || depth > 16) return 0;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) {
            if ((unsigned char)*p < 0x20) return 0;
            if (*p == '\\') { p++; if (!*p) return 0; }
        }
        return *p == '"' ? p + 1 : 0;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']'; int obj = *p == '{';
        p = oj_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (obj) { if (*p != '"') return 0; p = oj_skip(p, depth + 1); if (!p) return 0; p = oj_ws(p); if (*p != ':') return 0; p++; }
            p = oj_skip(p, depth + 1); if (!p) return 0;
            p = oj_ws(p);
            if (*p == ',') { p = oj_ws(p + 1); continue; }
            return *p == close ? p + 1 : 0;
        }
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    const char *s = p;
    while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9')) p++;
    return p > s ? p : 0;
}
static int oj_valid(const char *doc)   /* one object and nothing but blanks after it */
{
    const char *p = oj_ws(doc); if (!p || *p != '{') return 0;
    p = oj_skip(p, 0); if (!p) return 0;
    return *oj_ws(p) == 0;
}
static const char *oj_get(const char *obj, const char *key)   /* value of obj.key (raw key compare) */
{
    obj = oj_ws(obj); if (!obj || *obj != '{') return 0;
    const char *p = oj_ws(obj + 1); size_t kl = strlen(key);
    while (p && *p == '"') {
        const char *ks = p + 1, *ke = oj_skip(p, 1); if (!ke) return 0;
        p = oj_ws(ke); if (*p != ':') return 0;
        const char *v = oj_ws(p + 1);
        if ((size_t)(ke - 1 - ks) == kl && !memcmp(ks, key, kl)) return v;
        p = oj_skip(v, 1); if (!p) return 0;
        p = oj_ws(p); if (*p != ',') return 0;
        p = oj_ws(p + 1);
    }
    return 0;
}
static void oj_utf8(char **o, char *end, unsigned cp)
{
    char b[4]; int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (end - *o > n) { memcpy(*o, b, n); *o += n; }
}
static unsigned oj_hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) { char c = p[i]; v <<= 4; if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return 0xFFFFFFFF; }
    return v;
}
/* string value -> UTF-8. Returns length, -1 if not a string, -2 if it does not fit in cap */
static int oj_str(const char *v, char *out, int cap)
{
    v = oj_ws(v); if (!v || *v != '"' || cap < 1) return -1;
    char *o = out, *end = out + cap; int trunc = 0;
    for (const char *p = v + 1; *p && *p != '"'; p++) {
        if (*p != '\\') { if (end - o > 1) *o++ = *p; else trunc = 1; continue; }
        p++;
        char *o0 = o;
        switch (*p) {
        case 'n': oj_utf8(&o, end, '\n'); break;
        case 't': oj_utf8(&o, end, '\t'); break;
        case 'r': oj_utf8(&o, end, '\r'); break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = oj_hex4(p + 1); if (cp == 0xFFFFFFFF) return -1;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                unsigned lo = oj_hex4(p + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            if (cp) oj_utf8(&o, end, cp);
            break;
        }
        case 0: *o = 0; return -1;
        default: oj_utf8(&o, end, (unsigned char)*p); break;
        }
        if (o == o0 && *p != 'b' && *p != 'f' && *p != 'u') trunc = 1;
    }
    *o = 0;
    return trunc ? -2 : (int)(o - out);
}
static int oj_ll(const char *v, long long *out)
{
    v = oj_ws(v); if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) return -1;
    char *e; errno = 0; long long x = strtoll(v, &e, 10);
    if (errno || e == v || *e == '.' || *e == 'e' || *e == 'E') return -1;
    *out = x; return 0;
}
static int oj_bool(const char *v, int *out)
{
    v = oj_ws(v); if (!v) return -1;
    if (!strncmp(v, "true", 4)) { *out = 1; return 0; }
    if (!strncmp(v, "false", 5)) { *out = 0; return 0; }
    return -1;
}

/* ================================================================== versions */
static int ota_ver_ok(const char *v)   /* 1.2.3 or 1.2.3-beta.1 */
{
    int n = 0, digits = 0;
    if (!v || !*v || strlen(v) > 23) return 0;
    for (; *v && *v != '-'; v++) {
        if (*v >= '0' && *v <= '9') { digits++; continue; }
        if (*v != '.' || !digits) return 0;
        digits = 0; n++;
    }
    if (!digits || n < 1) return 0;
    if (*v == '-') { if (!v[1]) return 0; for (v++; *v; v++) if (!isalnum((unsigned char)*v) && *v != '.') return 0; }
    return 1;
}
static int ota_vercmp(const char *a, const char *b)   /* <0, 0, >0 */
{
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        int ea = !*a || *a == '-', eb = !*b || *b == '-';
        if (ea && eb) break;
        long x = 0, y = 0;
        if (!ea) { x = strtol(a, (char **)&a, 10); if (*a == '.') a++; }
        if (!eb) { y = strtol(b, (char **)&b, 10); if (*b == '.') b++; }
        if (x != y) return x < y ? -1 : 1;
    }
    int pa = *a == '-', pb = *b == '-';
    if (pa != pb) return pa ? -1 : 1;           /* a pre-release is older than the release */
    if (!pa) return 0;
    int c = strcmp(a, b); return c < 0 ? -1 : c > 0;
}

/* ================================================================== hibyos-update.json */
enum { OTA_RS_NONE, OTA_RS_SHELL, OTA_RS_REBOOT };
struct ota_info {
    int format, needs_firmware, restart;
    char version[24], min_version[24], min_firmware[24], date[24];
    char url[512], name[96], sha256[65], fw_url[256];
    long long size;
};
static int ota_url_ok(const char *u)
{
    return (!strncmp(u, "https://", 8) || !strncmp(u, "http://", 7)) && strlen(u) > 12 && !strpbrk(u, " \t\r\n\"'\\<>");
}
static int ota_name_ok(const char *n)
{
    size_t l = strlen(n); if (l < 5 || l > 90 || n[0] == '.') return 0;
    for (const char *p = n; *p; p++) if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return 0;
    return !strcmp(n + l - 4, ".tar");
}
/* 0 = ok; -1 = not a valid ByOS update description (assumes the signature was checked) */
static int ota_parse(const char *js, struct ota_info *o)
{
    memset(o, 0, sizeof *o);
    if (!oj_valid(js)) return -1;
    char tmp[32]; long long n; const char *v, *pl;
    if (oj_str(oj_get(js, "product"), tmp, sizeof tmp) < 0 || strcmp(tmp, "hibyos")) return -1;
    if (oj_ll(oj_get(js, "format"), &n) || n != 1) return -1;
    o->format = 1;
    if (oj_str(oj_get(js, "version"), o->version, sizeof o->version) < 0 || !ota_ver_ok(o->version)) return -1;
    if ((v = oj_get(js, "min_version")) && (oj_str(v, o->min_version, sizeof o->min_version) < 0 || !ota_ver_ok(o->min_version))) return -1;
    if ((v = oj_get(js, "min_firmware")) && (oj_str(v, o->min_firmware, sizeof o->min_firmware) < 0 || !ota_ver_ok(o->min_firmware))) return -1;
    if ((v = oj_get(js, "date"))) oj_str(v, o->date, sizeof o->date);
    if ((v = oj_get(js, "needs_firmware")) && oj_bool(v, &o->needs_firmware)) return -1;
    if ((v = oj_get(js, "firmware_url")) && (oj_str(v, o->fw_url, sizeof o->fw_url) < 0 || !ota_url_ok(o->fw_url))) o->fw_url[0] = 0;
    o->restart = OTA_RS_REBOOT;
    if ((v = oj_get(js, "restart"))) {
        if (oj_str(v, tmp, sizeof tmp) < 0) return -1;
        o->restart = !strcmp(tmp, "none") ? OTA_RS_NONE : !strcmp(tmp, "shell") ? OTA_RS_SHELL : OTA_RS_REBOOT;
    }
    if (!(pl = oj_get(js, "payload")) || *pl != '{') return -1;
    if (oj_str(oj_get(pl, "url"), o->url, sizeof o->url) < 0 || !ota_url_ok(o->url)) return -1;
    if (oj_str(oj_get(pl, "name"), o->name, sizeof o->name) < 0 || !ota_name_ok(o->name)) return -1;
    if (oj_str(oj_get(pl, "sha256"), o->sha256, sizeof o->sha256) != 64) return -1;
    for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)o->sha256[i])) return -1; else o->sha256[i] = (char)tolower((unsigned char)o->sha256[i]);
    if (oj_ll(oj_get(pl, "size"), &o->size) || o->size <= 0 || o->size > OTA_PAYLOAD_MAX) return -1;
    if ((v = oj_get(pl, "compression")) && (oj_str(v, tmp, sizeof tmp) < 0 || strcmp(tmp, "none"))) return -1;   /* only plain tar */
    return 0;
}
/* changelog in language lang ("es", "zh-Hans"...): then "en", then "es", then the first one.
 * A string, or an array of strings (shown as "• a  • b"). */
static void ota_changelog(const char *js, const char *lang, char *out, int cap)
{
    out[0] = 0;
    const char *c = js ? oj_get(js, "changelog") : 0, *v = 0;
    if (!c) return;
    if (*c == '"' || *c == '[') v = c;
    else if (*c == '{') {
        const char *pref[3] = { lang, "en", "es" };
        for (int i = 0; i < 3 && !v; i++) if (pref[i]) v = oj_get(c, pref[i]);
        if (!v) { const char *p = oj_ws(c + 1); if (*p == '"') { p = oj_skip(p, 1); if (p) { p = oj_ws(p); if (*p == ':') v = oj_ws(p + 1); } } }
    }
    if (!v) return;
    if (*v == '"') { oj_str(v, out, cap); return; }
    if (*v != '[') return;
    int len = 0;
    for (const char *p = oj_ws(v + 1); p && *p == '"'; ) {
        char item[512]; if (oj_str(p, item, sizeof item) < 0 && !item[0]) break;
        int k = snprintf(out + len, cap - len, "%s• %s", len ? "  " : "", item);
        if (k < 0 || k >= cap - len) { out[len] = 0; break; }
        len += k;
        p = oj_skip(p, 1); if (!p) break;
        p = oj_ws(p); if (*p != ',') break;
        p = oj_ws(p + 1);
    }
}
/* signature: exactly one "signature" key with "ed25519:<128 hex>"; the signed message is the
 * buffer with those 128 digits set to '0'. 0 = signed by one of the keys. */
enum { OTA_SIG_OK = 0, OTA_SIG_NOKEY = 1, OTA_SIG_MISSING = 2, OTA_SIG_BAD = 3 };
static int ota_sig_check_keys(const char *buf, size_t n, uint8_t keys[][32], int nkeys)
{
    if (nkeys <= 0) return OTA_SIG_NOKEY;
    if (memchr(buf, 0, n)) return OTA_SIG_MISSING;
    const char *tok = "\"signature\"", *at = 0;
    for (const char *p = buf; (p = strstr(p, tok)); p += 11) { if (at) return OTA_SIG_MISSING; at = p; }
    if (!at) return OTA_SIG_MISSING;
    const char *p = oj_ws(at + 11); if (*p != ':') return OTA_SIG_MISSING;
    p = oj_ws(p + 1); if (strncmp(p, "\"ed25519:", 9)) return OTA_SIG_MISSING;
    const char *hx = p + 9;
    uint8_t sig[64];
    if ((size_t)(hx - buf) + 129 > n || hx[128] != '"' || ota_unhex(hx, sig, 64)) return OTA_SIG_MISSING;
    char *m = malloc(n); if (!m) return OTA_SIG_BAD;
    memcpy(m, buf, n); memset(m + (hx - buf), '0', 128);
    int ok = 0;
    for (int k = 0; k < nkeys && !ok; k++) ok = !ota_ed25519_verify(sig, (const uint8_t *)m, n, keys[k]);
    free(m);
    return ok ? OTA_SIG_OK : OTA_SIG_BAD;
}
static int ota_sig_check(const char *buf, size_t n)
{
    uint8_t keys[2][32]; int nk = ota_keys(keys);
    return ota_sig_check_keys(buf, n, keys, nk);
}

/* ================================================================== files */
static int ota_mkdirs(const char *path)   /* mkdir -p */
{
    char b[600]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(b, 0755) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(b, 0755) && errno != EEXIST ? -1 : 0;
}
static int ota_rm_rf(const char *p)
{
    struct stat s; if (lstat(p, &s)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(s.st_mode)) return unlink(p);
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; int r = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[900]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        r |= ota_rm_rf(c);
    }
    closedir(d);
    return r | rmdir(p);
}
static int ota_exists(const char *p) { struct stat s; return !stat(p, &s); }
static long long ota_fsize(const char *p) { struct stat s; return stat(p, &s) ? -1 : (long long)s.st_size; }
static long long ota_free(const char *p) { struct statvfs v; return statvfs(p, &v) ? -1LL : (long long)v.f_bavail * (long long)v.f_frsize; }
static char *ota_read_file(const char *p, size_t max, size_t *len)
{
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    struct stat s; if (fstat(fd, &s) || s.st_size <= 0 || (size_t)s.st_size > max) { close(fd); return 0; }
    char *b = malloc((size_t)s.st_size + 1); size_t got = 0;
    while (b && got < (size_t)s.st_size) { ssize_t r = read(fd, b + got, (size_t)s.st_size - got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (!b || got != (size_t)s.st_size) { free(b); return 0; }
    b[got] = 0; if (len) *len = got;
    return b;
}
static void ota_first_line(const char *p, char *out, int cap)   /* VERSION, payload_version */
{
    out[0] = 0;
    FILE *f = fopen(p, "r"); if (!f) return;
    if (fgets(out, cap, f)) { int k = 0; for (int i = 0; out[i]; i++) if (!isspace((unsigned char)out[i])) out[k++] = out[i]; out[k] = 0; }
    fclose(f);
}
/* sha256 of a file; progress in *frac (0..1) if given; cancel checked between blocks */
static int ota_file_sha256(const char *p, char hex[65], long long *size, volatile double *frac, volatile int *cancel)
{
    int fd = open(p, O_RDONLY); if (fd < 0) return -1;
    struct stat sb; long long tot = fstat(fd, &sb) ? 0 : (long long)sb.st_size, done = 0;
    struct ota_sha256 s; ota_sha256_init(&s);
    static uint8_t buf[32768]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        ota_sha256_update(&s, buf, (size_t)n); done += n;
        if (frac && tot > 0) *frac = (double)done / tot;
        if (cancel && *cancel) { close(fd); return -2; }
    }
    close(fd);
    if (n < 0) return -1;
    uint8_t d[32]; ota_sha256_final(&s, d); ota_hex(d, 32, hex);
    if (size) *size = done;
    return 0;
}

/* ================================================================== ustar extraction */
static int ota_tar_path_ok(const char *n, const char *prefix)
{
    size_t pl = strlen(prefix);
    if (strncmp(n, prefix, pl) || n[0] == '/') return 0;
    for (const char *p = n; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._-+/@", *p)) return 0;
    const char *c = n;
    for (;;) {   /* each component: not empty (except a final "/"), not "." or ".." */
        const char *e = strchr(c, '/'); size_t l = e ? (size_t)(e - c) : strlen(c);
        if ((l == 0 && e) || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (!e) break;
        c = e + 1;
        if (!*c) break;
    }
    return 1;
}
static long long ota_oct(const char *p, int n)
{
    long long v = 0; int any = 0;
    for (int i = 0; i < n && p[i]; i++) {
        if (p[i] == ' ') { if (any) break; continue; }
        if (p[i] < '0' || p[i] > '7') return -1;
        v = v * 8 + (p[i] - '0'); any = 1;
        if (v > (1LL << 40)) return -1;
    }
    return any ? v : -1;
}
/* extracts the entries of tar under "prefix" (e.g. "payload/") into dest (must be empty).
 * 0 = ok, -1 = invalid archive, -2 = cancelled, -3 = I/O error. */
static int ota_tar_extract(const char *tar, const char *dest, const char *prefix, volatile double *frac, volatile int *cancel)
{
    int fd = open(tar, O_RDONLY); if (fd < 0) return -3;
    long long tot = ota_fsize(tar), done = 0;
    uint8_t h[512]; static uint8_t buf[32768];
    int files = 0, rc = -1, zeros = 0;
    for (;;) {
        ssize_t r = read(fd, h, 512);
        if (r == 0 && zeros) { rc = 0; break; }        /* one end block is enough */
        if (r != 512) break;
        done += 512;
        int z = 1; for (int i = 0; i < 512; i++) if (h[i]) { z = 0; break; }
        if (z) { if (++zeros == 2) { rc = 0; break; } continue; }
        if (zeros) break;                                /* data after an end block */
        long long ck = ota_oct((char *)h + 148, 8);
        long long sum = 0; for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (ck != sum) break;
        if (memcmp(h + 257, "ustar", 5)) break;
        char name[260], nm[101], px[156];
        memcpy(nm, h, 100); nm[100] = 0; memcpy(px, h + 345, 155); px[155] = 0;
        if (px[0]) snprintf(name, sizeof name, "%s/%s", px, nm); else snprintf(name, sizeof name, "%s", nm);
        char type = (char)h[156];
        long long size = ota_oct((char *)h + 124, 12);
        if (size < 0 || size > OTA_PAYLOAD_MAX || ++files > OTA_TAR_FILES_MAX) break;
        if (!ota_tar_path_ok(name, prefix)) break;
        char out[700]; snprintf(out, sizeof out, "%s/%s", dest, name);
        if (type == '5') {
            if (size) break;
            size_t l = strlen(out); if (l && out[l - 1] == '/') out[l - 1] = 0;
            if (ota_mkdirs(out)) { rc = -3; break; }
            continue;
        }
        if (type != '0' && type != 0) break;             /* links, devices, pax, GNU long names: refused */
        if (name[strlen(name) - 1] == '/') break;
        char *sl = strrchr(out, '/'); *sl = 0;
        if (ota_mkdirs(out)) { rc = -3; break; }
        *sl = '/';
        int of = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (of < 0) { rc = errno == EEXIST ? -1 : -3; break; }
        long long left = size; int bad = 0;
        while (left > 0) {
            size_t want = left > (long long)sizeof buf ? sizeof buf : (size_t)left;
            ssize_t g = read(fd, buf, want);
            if (g <= 0) { bad = -1; break; }
            if (write(of, buf, (size_t)g) != g) { bad = -3; break; }
            left -= g; done += g;
            if (frac && tot > 0) *frac = (double)done / tot;
            if (cancel && *cancel) { bad = -2; break; }
        }
        if (close(of) && !bad) bad = -3;
        if (bad) { rc = bad; break; }
        long long pad = (512 - size % 512) % 512;
        if (pad && lseek(fd, pad, SEEK_CUR) < 0) break;
        done += pad;
    }
    close(fd);
    if (!rc && frac) *frac = 1;
    return rc;
}

/* ================================================================== restart needed
 * MANIFEST.md5 of the installed payload vs the new one: interface binaries and shell assets
 * only need hibyshell to restart; the display library, theme, home mirror and boot scripts
 * need a reboot (the player loads them at start). */
static int ota_path_restart(const char *p)
{
    if (!strcmp(p, "VERSION") || !strcmp(p, "MANIFEST.md5") || !strncmp(p, "Pictures/", 9)) return OTA_RS_NONE;
    if (!strcmp(p, "sys/hibyos_install.sh") || !strcmp(p, "sys/MANIFEST.sha256")) return OTA_RS_NONE;
    if (!strcmp(p, "bin/hibymod_disp.so")) return OTA_RS_REBOOT;
    if (!strncmp(p, "bin/", 4) || !strncmp(p, "shell/", 6)) return OTA_RS_SHELL;
    return OTA_RS_REBOOT;
}
struct ota_mline { char md5[33]; char path[200]; };
static int ota_manifest_load(const char *p, struct ota_mline **out)
{
    size_t len; char *b = ota_read_file(p, 1 << 20, &len); *out = 0;
    if (!b) return -1;
    int cap = 256, n = 0; struct ota_mline *m = malloc(cap * sizeof *m);
    for (char *l = b; m && l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char md[40], path[220];
        if (sscanf(l, "%39s %219s", md, path) == 2 && strlen(md) == 32 && strlen(path) < 200) {
            if (n == cap) { struct ota_mline *nm = realloc(m, (cap *= 2) * sizeof *m); if (!nm) { free(m); m = 0; break; } m = nm; }
            memcpy(m[n].md5, md, 33); memcpy(m[n].path, path, strlen(path) + 1); n++;
        }
        l = e ? e + 1 : 0;
    }
    free(b);
    if (!m) return -1;
    *out = m; return n;
}
static int ota_restart_kind(const char *old_manifest, const char *new_manifest)
{
    struct ota_mline *a, *b; int na = ota_manifest_load(old_manifest, &a), nb = ota_manifest_load(new_manifest, &b), k = OTA_RS_NONE;
    if (na < 0 || nb < 0) { free(a); free(b); return OTA_RS_REBOOT; }
    for (int i = 0; i < nb; i++) {
        int same = 0;
        for (int j = 0; j < na; j++) if (!strcmp(a[j].path, b[i].path)) { same = !strcmp(a[j].md5, b[i].md5); break; }
        if (!same) { int r = ota_path_restart(b[i].path); if (r > k) k = r; }
    }
    for (int j = 0; j < na; j++) {
        int found = 0; for (int i = 0; i < nb; i++) if (!strcmp(a[j].path, b[i].path)) { found = 1; break; }
        if (!found) { int r = ota_path_restart(a[j].path); if (r > k) k = r; }
    }
    free(a); free(b);
    return k;
}

#ifndef OTA_CORE_ONLY
/* ================================================================== worker */
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
extern char **environ;

enum { OS_IDLE, OS_CHECKING, OS_UPTODATE, OS_AVAILABLE, OS_NEWER, OS_DOWNLOADING, OS_PAUSED,
       OS_VERIFYING, OS_EXTRACTING, OS_INSTALLING, OS_RESTORING, OS_DONE, OS_ERROR };
enum { OE_NONE, OE_NOKEY, OE_NOSD, OE_NET, OE_SIG, OE_FORMAT, OE_SHA, OE_SPACE_SD, OE_SPACE_DATA, OE_BATT,
       OE_TAR, OE_INSTALL, OE_IO, OE_MINVER, OE_FIRMWARE, OE_DOWNGRADE, OE_NOPREV, OE_NONET };
enum { OTA_OP_LOAD, OTA_OP_CHECK, OTA_OP_INSTALL, OTA_OP_ROLLBACK };

struct ota_state {
    volatile int state, err, busy, cancel, restart, allow_down, quiet;
    volatile long long got, total, need;
    volatile double frac;
    pthread_mutex_t mu;
    struct ota_info info; int have_info;     /* verified (under mu) */
    char *json;                              /* verified JSON text (under mu), for the changelog */
    char installed[24], firmware[24], prev[24];
    long long last_check; int auto_on;
};
static struct ota_state ota_st = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* paths (the PC test changes them with OTA_* variables; on the device they are fixed) */
struct ota_paths { char sd[200], hm[200], net[260], sh[64], installer[260], url[512], cli[200], mounts[200], batt[200], usb[200]; };
static struct ota_paths OP;
static void ota_paths_init(void)
{
    if (OP.sd[0]) return;
    snprintf(OP.sd, sizeof OP.sd, "/usr/data/mnt/sd_0");
    snprintf(OP.hm, sizeof OP.hm, "/usr/data/hibymod");
    snprintf(OP.net, sizeof OP.net, "/usr/data/hibymod/shell/hibynet");
    snprintf(OP.sh, sizeof OP.sh, "/bin/sh");
    snprintf(OP.installer, sizeof OP.installer, "/usr/bin/hibyos_install.sh");
    snprintf(OP.url, sizeof OP.url, OTA_URL_LATEST);
    snprintf(OP.cli, sizeof OP.cli, "/usr/bin/hibymod");
    snprintf(OP.mounts, sizeof OP.mounts, "/proc/mounts");
    snprintf(OP.batt, sizeof OP.batt, "/sys/class/power_supply/battery/capacity");
    snprintf(OP.usb, sizeof OP.usb, "/sys/class/power_supply/usb/online");
#ifdef OTA_TEST
    const char *e;
#define OTA_ENV(k, f) if ((e = getenv(k)) && *e) snprintf(OP.f, sizeof OP.f, "%s", e)
    OTA_ENV("OTA_SD", sd); OTA_ENV("OTA_HM", hm); OTA_ENV("OTA_NET", net); OTA_ENV("OTA_SH", sh);
    OTA_ENV("OTA_INSTALLER", installer); OTA_ENV("OTA_URL", url); OTA_ENV("OTA_CLI", cli);
    OTA_ENV("OTA_MOUNTS", mounts); OTA_ENV("OTA_BATT", batt); OTA_ENV("OTA_USB", usb);
#undef OTA_ENV
#endif
}
static void ota_path(char *out, int cap, const char *rel)   /* SD/ByOS/<rel> */
{
    snprintf(out, cap, "%s/ByOS/%s", OP.sd, rel);
}
static int ota_sd_ok(void)
{
    FILE *f = fopen(OP.mounts, "r"); if (!f) return 0;
    char l[512], want[220]; int ok = 0;
    snprintf(want, sizeof want, " %s ", OP.sd);
    while (fgets(l, sizeof l, f)) if (strstr(l, want) || strstr(l, " /data/mnt/sd_0 ")) ok = 1;
    fclose(f); return ok;
}
static long ota_sysfs_long(const char *p, long def)
{
    char b[32] = ""; int fd = open(p, O_RDONLY); if (fd < 0) return def;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return def;
    b[n] = 0; return strtol(b, 0, 10);
}
static int ota_power_ok(void) { return ota_sysfs_long(OP.usb, 0) == 1 || ota_sysfs_long(OP.batt, 100) >= OTA_MIN_BATT; }
static int ota_charging(void) { return ota_sysfs_long(OP.usb, 0) == 1; }
static int ota_wifi_up(void)   /* a default route that is not loopback */
{
    FILE *f = fopen("/proc/net/route", "r"); if (!f) return 0;
    char l[256], ifc[32]; unsigned long dst; int ok = 0;
    while (fgets(l, sizeof l, f)) if (sscanf(l, "%31s %lx", ifc, &dst) == 2 && dst == 0 && strcmp(ifc, "lo")) ok = 1;
    fclose(f); return ok;
}
/* firmware (rootfs) version: the `hibymod status` line of /usr/bin/hibymod ("ByOS 0.6.1") */
static void ota_firmware_version(char *out, int cap)
{
    out[0] = 0;
    size_t n; char *b = ota_read_file(OP.cli, 256 * 1024, &n); if (!b) return;
    /* "ByOS x.y" (a firmware from before the rename says "HiByOS x.y") */
    char *p = strstr(b, "echo \"ByOS ");
    if (p) p += 11; else if ((p = strstr(b, "echo \"HiByOS "))) p += 13;
    if (p) { int k = 0; while (k < cap - 1 && (isalnum((unsigned char)p[k]) || p[k] == '.' || p[k] == '-')) { out[k] = p[k]; k++; } out[k] = 0; }
    free(b);
}
static void ota_versions_refresh(void)
{
    char b[256];
    snprintf(b, sizeof b, "%s/payload_version", OP.hm); ota_first_line(b, ota_st.installed, sizeof ota_st.installed);
    ota_firmware_version(ota_st.firmware, sizeof ota_st.firmware);
    ota_path(b, sizeof b, "ota/previous/payload/VERSION"); ota_first_line(b, ota_st.prev, sizeof ota_st.prev);
    if (!ota_ver_ok(ota_st.prev)) ota_st.prev[0] = 0;
}
/* ota.conf: "auto 0|1", "last_check <epoch>" */
static void ota_conf_load(void)
{
    char p[256]; snprintf(p, sizeof p, "%s/ota.conf", OP.hm);
    FILE *f = fopen(p, "r"); char l[128];
    while (f && fgets(l, sizeof l, f)) {
        long long v;
        if (sscanf(l, "auto %lld", &v) == 1) ota_st.auto_on = v != 0;
        else if (sscanf(l, "last_check %lld", &v) == 1) ota_st.last_check = v;
    }
    if (f) fclose(f);
}
static void ota_conf_save(void)
{
    char p[256], t[270]; snprintf(p, sizeof p, "%s/ota.conf", OP.hm); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) return;
    fprintf(f, "auto %d\nlast_check %lld\n", ota_st.auto_on, ota_st.last_check);
    if (fclose(f) || rename(t, p)) unlink(t);
}

/* child process with stdout -> /dev/null and stderr -> pipe (*efd) or /dev/null. env_extra: "K=V" or 0 */
static pid_t ota_spawn(char *const argv[], int *efd, const char *env_extra, const char *env_extra2)
{
    int fd[2] = { -1, -1 }; if (efd && pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (efd) posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    /* nothing else of the shell reaches the child: a long download must not keep the shim's
     * input socket open if the shell dies meanwhile (the shim releases the touch on close) */
    DIR *fdd = opendir("/proc/self/fd"); struct dirent *fe;
    while (fdd && (fe = readdir(fdd))) {
        int f = atoi(fe->d_name);
        if (f > 2 && f != dirfd(fdd)) posix_spawn_file_actions_addclose(&fa, f);
    }
    if (fdd) closedir(fdd);
    int ne = 0; while (environ && environ[ne]) ne++;
    char **env = calloc((size_t)ne + 3, sizeof *env); pid_t pid = -1; int r = -1;
    if (env) {
        int k = 0;
        for (int i = 0; i < ne; i++) {
            if (env_extra && !strncmp(environ[i], env_extra, strcspn(env_extra, "=") + 1)) continue;
            if (env_extra2 && !strncmp(environ[i], env_extra2, strcspn(env_extra2, "=") + 1)) continue;
            env[k++] = environ[i];
        }
        if (env_extra) env[k++] = (char *)env_extra;
        if (env_extra2) env[k++] = (char *)env_extra2;
        r = posix_spawn(&pid, argv[0], &fa, 0, argv, env);
        free(env);
    }
    posix_spawn_file_actions_destroy(&fa);
    if (efd) { close(fd[1]); if (r) { close(fd[0]); return -1; } fcntl(fd[0], F_SETFL, O_NONBLOCK); *efd = fd[0]; }
    return r ? -1 : pid;
}
static int ota_wait(pid_t pid, int cancellable)   /* exit code, -1 = signal/error, -2 = cancelled */
{
    int s, killed = 0;
    for (;;) {
        pid_t r = waitpid(pid, &s, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (cancellable && ota_st.cancel && !killed) { kill(pid, SIGTERM); killed = 1; }
        usleep(50000);
    }
    if (killed) return -2;
    return WIFEXITED(s) ? WEXITSTATUS(s) : -1;
}
/* hibynet get URL OUT [resume]: progress lines on stderr ("progress GOT TOTAL") */
static int ota_net_get(const char *url, const char *out, int resume, int track)
{
    char *argv[] = { OP.net, "get", (char *)url, (char *)out, 0 };
    int efd = -1;
    pid_t pid = ota_spawn(argv, track ? &efd : 0, resume ? "HIBYNET_RESUME=1" : 0, "HIBYNET_TIMEOUT=25");
    if (pid <= 0) return -1;
    char line[128]; int ln = 0, s, killed = 0, rc;
    for (;;) {
        if (efd >= 0) {
            struct pollfd pf = { efd, POLLIN, 0 }; poll(&pf, 1, 100);
            char b[256]; ssize_t n;
            while ((n = read(efd, b, sizeof b)) > 0)
                for (ssize_t k = 0; k < n; k++) {
                    if (b[k] != '\n') { if (ln < (int)sizeof line - 1) line[ln++] = b[k]; continue; }
                    line[ln] = 0; ln = 0;
                    long long g, t; if (sscanf(line, "progress %lld %lld", &g, &t) == 2) { ota_st.got = g; if (t > 0) ota_st.total = t; }
                }
        } else usleep(100000);
        pid_t r = waitpid(pid, &s, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) { s = -1; break; }
        if (ota_st.cancel && !killed) { kill(pid, SIGTERM); killed = 1; }
    }
    if (efd >= 0) close(efd);
    if (killed) return -2;
    rc = s >= 0 && WIFEXITED(s) ? WEXITSTATUS(s) : -1;
    return rc;
}
static void ota_set(int state, int err) { __sync_synchronize(); ota_st.err = err; ota_st.state = state; __sync_synchronize(); }

/* reads, verifies and parses a saved hibyos-update.json; 0 = ok (published in ota_st) */
static int ota_adopt_json(const char *path, int *err)
{
    size_t n; char *b = ota_read_file(path, OTA_JSON_MAX, &n);
    if (!b) { *err = OE_FORMAT; return -1; }
    int sg = ota_sig_check(b, n);
    if (sg) { *err = sg == OTA_SIG_NOKEY ? OE_NOKEY : OE_SIG; free(b); return -1; }
    struct ota_info in;
    if (ota_parse(b, &in)) { *err = OE_FORMAT; free(b); return -1; }
    pthread_mutex_lock(&ota_st.mu);
    free(ota_st.json); ota_st.json = b; ota_st.info = in; ota_st.have_info = 1;
    pthread_mutex_unlock(&ota_st.mu);
    return 0;
}
/* the state that the verified info leads to (available / up to date / installed is newer) */
static int ota_info_state(void)
{
    int c = ota_vercmp(ota_st.info.version, ota_st.installed);
    if (!ota_st.installed[0]) c = 1;
    return c > 0 ? OS_AVAILABLE : c == 0 ? OS_UPTODATE : OS_NEWER;
}
static void ota_tar_paths(char *tar, int cap)
{
    char rel[160]; snprintf(rel, sizeof rel, "ota/%s", ota_st.info.name); ota_path(tar, cap, rel);
}
static void ota_purge_downloads(const char *keep)   /* other versions' .tar / .tar.part */
{
    char d[260]; ota_path(d, sizeof d, "ota"); DIR *dd = opendir(d); struct dirent *e;
    while (dd && (e = readdir(dd))) {
        size_t l = strlen(e->d_name);
        int is = (l > 4 && !strcmp(e->d_name + l - 4, ".tar")) || (l > 9 && !strcmp(e->d_name + l - 9, ".tar.part"));
        if (!is || !strncmp(e->d_name, keep, strlen(keep))) continue;
        char p[600]; snprintf(p, sizeof p, "%s/%s", d, e->d_name); unlink(p);
    }
    if (dd) closedir(dd);
}

static void op_load(void)   /* page opened: last verified result, without the network */
{
    char p[260]; int err = 0;
    ota_path(p, sizeof p, "ota/hibyos-update.json");
    if (!ota_sd_ok() || !ota_exists(p) || ota_adopt_json(p, &err)) { ota_set(OS_IDLE, OE_NONE); return; }
    int s = ota_info_state();
    if (s == OS_AVAILABLE) {
        char tar[300], part[310]; ota_tar_paths(tar, sizeof tar); snprintf(part, sizeof part, "%s.part", tar);
        long long ps = ota_fsize(part);
        if (ps > 0) { ota_st.got = ps; ota_st.total = ota_st.info.size; s = OS_PAUSED; }
    }
    ota_set(s, OE_NONE);
}
static void op_check(void)
{
    char d[260], out[300], part[310], fin[300]; int err = OE_NONE;
    uint8_t k[2][32];
    if (!ota_keys(k)) { ota_set(OS_ERROR, OE_NOKEY); return; }
    if (!ota_sd_ok()) { ota_set(OS_ERROR, OE_NOSD); return; }
    ota_path(d, sizeof d, "ota"); if (ota_mkdirs(d)) { ota_set(OS_ERROR, OE_IO); return; }
    snprintf(out, sizeof out, "%s/hibyos-update.json.new", d); snprintf(part, sizeof part, "%s.part", out);
    snprintf(fin, sizeof fin, "%s/hibyos-update.json", d);
    unlink(out); unlink(part);
    int rc = ota_net_get(OP.url, out, 0, 0);
    if (rc == -2) { unlink(out); op_load(); return; }
    if (rc != 0) { unlink(out); ota_set(OS_ERROR, ota_wifi_up() ? OE_NET : OE_NONET); return; }
    if (ota_adopt_json(out, &err)) { unlink(out); lg("ota: update.json refused (%d)", err); ota_set(OS_ERROR, err); return; }
    rename(out, fin);
    ota_st.last_check = (long long)time(0); ota_conf_save();
    ota_versions_refresh();
    int s = ota_info_state();
    lg("ota: latest %s, installed %s", ota_st.info.version, ota_st.installed);
    if (s == OS_AVAILABLE) {
        char tar[300], pt[310]; ota_tar_paths(tar, sizeof tar); snprintf(pt, sizeof pt, "%s.part", tar);
        long long ps = ota_fsize(pt); if (ps > 0) { ota_st.got = ps; ota_st.total = ota_st.info.size; s = OS_PAUSED; }
    }
    ota_set(s, OE_NONE);
}
/* SD/ByOS/payload <-> ota/previous/payload and the staged one */
static int ota_swap_in(const char *stage_pl, const char *pl, const char *prevdir)
{
    char prev_pl[320]; snprintf(prev_pl, sizeof prev_pl, "%s/payload", prevdir);
    if (ota_mkdirs(prevdir) || ota_rm_rf(prev_pl)) return -1;
    if (ota_exists(pl) && rename(pl, prev_pl)) return -1;
    if (rename(stage_pl, pl)) { if (ota_exists(prev_pl)) rename(prev_pl, pl); return -1; }
    sync();
    return 0;
}
static int ota_swap_back(const char *pl, const char *prevdir, const char *stage_dir)
{
    char prev_pl[320], st_pl[320]; snprintf(prev_pl, sizeof prev_pl, "%s/payload", prevdir); snprintf(st_pl, sizeof st_pl, "%s/payload", stage_dir);
    if (!ota_exists(prev_pl)) return -1;
    if (ota_rm_rf(stage_dir) || ota_mkdirs(stage_dir)) return -1;
    if (ota_exists(pl) && rename(pl, st_pl)) return -1;
    if (rename(prev_pl, pl)) { rename(st_pl, pl); return -1; }
    rename(st_pl, prev_pl);   /* the one that was installed becomes the "previous" one */
    ota_rm_rf(stage_dir);
    sync();
    return 0;
}
/* hibyos_install.sh install; if the installed override fails, once more with the built-in one */
static int ota_run_installer(void)
{
    char *argv[] = { OP.sh, OP.installer, "install", 0 };
    pid_t pid = ota_spawn(argv, 0, 0, 0); int rc = pid > 0 ? ota_wait(pid, 0) : -1;
    if (rc != 0) {
        lg("ota: installer rc=%d, again with the built-in one", rc);
        pid = ota_spawn(argv, 0, "HIBYOS_NO_OVERRIDE=1", 0); rc = pid > 0 ? ota_wait(pid, 0) : -1;
    }
    return rc;
}
static int ota_install_log_says_space(void)
{
    char p[260]; ota_path(p, sizeof p, "install.log");
    FILE *f = fopen(p, "r"); if (!f) return 0;
    long sz = fseek(f, 0, SEEK_END) ? 0 : ftell(f); fseek(f, sz > 600 ? sz - 600 : 0, SEEK_SET);
    char b[640]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
    char *last = strstr(b, "instalando"), *q; while (last && (q = strstr(last + 1, "instalando"))) last = q;
    return last && strstr(last, "sin espacio") != 0;
}
static void op_install(void)
{
    char d[260], tar[300], part[310], stage[300], stage_pl[320], pl[300], prevdir[300], b[400];
    if (!ota_sd_ok()) { ota_set(OS_ERROR, OE_NOSD); return; }
    if (!ota_st.have_info) { ota_set(OS_ERROR, OE_FORMAT); return; }
    struct ota_info in; pthread_mutex_lock(&ota_st.mu); in = ota_st.info; pthread_mutex_unlock(&ota_st.mu);
    ota_versions_refresh();
    if (!ota_sd_ok()) { ota_set(OS_ERROR, OE_NOSD); return; }
    if (in.needs_firmware || (in.min_firmware[0] && ota_st.firmware[0] && ota_vercmp(ota_st.firmware, in.min_firmware) < 0)) { ota_set(OS_ERROR, OE_FIRMWARE); return; }
    if (in.min_version[0] && ota_st.installed[0] && ota_vercmp(ota_st.installed, in.min_version) < 0) { ota_set(OS_ERROR, OE_MINVER); return; }
    if (ota_st.installed[0] && ota_vercmp(in.version, ota_st.installed) <= 0 && !ota_st.allow_down) { ota_set(OS_ERROR, OE_DOWNGRADE); return; }
    if (!ota_power_ok()) { ota_set(OS_ERROR, OE_BATT); return; }
    ota_path(d, sizeof d, "ota"); ota_path(pl, sizeof pl, "payload");
    snprintf(stage, sizeof stage, "%s/stage", d); snprintf(stage_pl, sizeof stage_pl, "%s/payload", stage);
    snprintf(prevdir, sizeof prevdir, "%s/previous", d);
    if (ota_mkdirs(d)) { ota_set(OS_ERROR, OE_IO); return; }
    ota_tar_paths(tar, sizeof tar); snprintf(part, sizeof part, "%s.part", tar);
    ota_purge_downloads(in.name);
    ota_rm_rf(stage);

    /* 1. download (resumable) unless the whole tar is already there */
    if (ota_fsize(tar) != in.size) {
        long long have = ota_fsize(part); if (have < 0) have = 0;
        if (have > in.size) { unlink(part); have = 0; }
        long long need = 2 * in.size - have + (2 << 20);
        ota_st.need = need;
        if (ota_free(d) < need) { ota_set(OS_ERROR, OE_SPACE_SD); return; }
        ota_st.got = have; ota_st.total = in.size;
        ota_set(OS_DOWNLOADING, OE_NONE);
        unlink(tar);
        int rc = ota_net_get(in.url, tar, 1, 1);
        if (rc == -2) { ota_set(OS_PAUSED, OE_NONE); return; }
        if (rc != 0) { lg("ota: download rc=%d", rc); ota_st.got = ota_fsize(part) > 0 ? ota_fsize(part) : 0; ota_set(OS_PAUSED, ota_wifi_up() ? OE_NET : OE_NONET); return; }
    } else if (ota_free(d) < in.size + (2 << 20)) { ota_st.need = in.size + (2 << 20); ota_set(OS_ERROR, OE_SPACE_SD); return; }
    /* 2. size + sha256 (signed in the JSON) */
    ota_st.frac = 0; ota_set(OS_VERIFYING, OE_NONE);
    char hex[65]; long long sz = 0;
    int hr = ota_file_sha256(tar, hex, &sz, &ota_st.frac, &ota_st.cancel);
    if (hr == -2) { ota_set(OS_PAUSED, OE_NONE); return; }
    if (hr || sz != in.size || strcmp(hex, in.sha256)) { lg("ota: sha256 %s (want %s) %lld bytes", hex, in.sha256, sz); unlink(tar); ota_set(OS_ERROR, OE_SHA); return; }
    /* 3. extract to ota/stage/payload */
    if (!ota_sd_ok()) { ota_set(OS_ERROR, OE_NOSD); return; }
    if (!ota_power_ok()) { ota_set(OS_ERROR, OE_BATT); return; }
    ota_st.frac = 0; ota_set(OS_EXTRACTING, OE_NONE);
    if (ota_mkdirs(stage)) { ota_set(OS_ERROR, OE_IO); return; }
    int xr = ota_tar_extract(tar, stage, "payload/", &ota_st.frac, 0);
    char v[32]; snprintf(b, sizeof b, "%s/VERSION", stage_pl); ota_first_line(b, v, sizeof v);
    snprintf(b, sizeof b, "%s/MANIFEST.md5", stage_pl);
    if (xr || strcmp(v, in.version) || !ota_exists(b)) {
        lg("ota: extraction rc=%d version '%s'", xr, v); ota_rm_rf(stage);
        if (xr != -3) unlink(tar);
        ota_set(OS_ERROR, xr == -3 ? OE_IO : OE_TAR); return;
    }
    sync();
    /* what has to restart: computed from the manifests (the JSON hint can only ask for more) */
    char oldm[320]; snprintf(oldm, sizeof oldm, "%s/MANIFEST.md5", pl);
    int rk = ota_restart_kind(oldm, b); if (in.restart > rk) rk = in.restart;
    /* 4. the new payload becomes SD/ByOS/payload (the old one -> ota/previous) and the
     *    usual installer (MANIFEST check, .new + rename, user state untouched) installs it */
    ota_set(OS_INSTALLING, OE_NONE);
    char keep[320]; snprintf(keep, sizeof keep, "%s.old", prevdir);   /* the "previous" one, until it works */
    ota_rm_rf(keep);
    if (ota_exists(prevdir) && rename(prevdir, keep)) { ota_rm_rf(stage); ota_set(OS_ERROR, OE_IO); return; }
    if (ota_swap_in(stage_pl, pl, prevdir)) { ota_rm_rf(stage); ota_rm_rf(prevdir); rename(keep, prevdir); ota_set(OS_ERROR, OE_IO); return; }
    int ir = ota_run_installer();
    if (ir != 0) {
        int space = ota_install_log_says_space();
        lg("ota: install failed (rc=%d), restoring the previous payload", ir);
        if (!ota_swap_back(pl, prevdir, stage)) {
            ota_run_installer();
            ota_rm_rf(prevdir); rename(keep, prevdir);                /* the failed one is not kept */
        }   /* else: SD untouched beyond this point; the next boot reinstalls SD/ByOS/payload */
        sync();
        ota_versions_refresh();
        ota_set(OS_ERROR, space ? OE_SPACE_DATA : OE_INSTALL); return;
    }
    ota_rm_rf(keep);
    unlink(tar); ota_rm_rf(stage);
    ota_versions_refresh();
    ota_st.restart = rk;
    lg("ota: ByOS %s installed (restart %d)", ota_st.installed, rk);
    ota_set(OS_DONE, OE_NONE);
}
static void op_rollback(void)
{
    char d[260], pl[300], prevdir[300], stage[300], a[400], b2[400];
    ota_versions_refresh();
    if (!ota_sd_ok()) { ota_set(OS_ERROR, OE_NOSD); return; }
    if (!ota_st.prev[0]) { ota_set(OS_ERROR, OE_NOPREV); return; }
    if (!ota_power_ok()) { ota_set(OS_ERROR, OE_BATT); return; }
    ota_path(d, sizeof d, "ota"); ota_path(pl, sizeof pl, "payload");
    snprintf(prevdir, sizeof prevdir, "%s/previous", d); snprintf(stage, sizeof stage, "%s/stage", d);
    snprintf(a, sizeof a, "%s/MANIFEST.md5", pl); snprintf(b2, sizeof b2, "%s/payload/MANIFEST.md5", prevdir);
    int rk = ota_restart_kind(a, b2);
    ota_set(OS_RESTORING, OE_NONE);
    if (ota_swap_back(pl, prevdir, stage)) { ota_set(OS_ERROR, OE_IO); return; }
    if (ota_run_installer() != 0) {
        lg("ota: restore failed, putting the newer payload back");
        if (!ota_swap_back(pl, prevdir, stage)) ota_run_installer();
        ota_versions_refresh(); ota_set(OS_ERROR, OE_INSTALL); return;
    }
    ota_versions_refresh();
    ota_st.restart = rk;
    ota_set(OS_DONE, OE_NONE);
}
static volatile int ota_op;
static void *ota_thread(void *arg)
{
    (void)arg;
    switch (ota_op) {
    case OTA_OP_LOAD: op_load(); break;
    case OTA_OP_CHECK: op_check(); break;
    case OTA_OP_INSTALL: op_install(); break;
    case OTA_OP_ROLLBACK: op_rollback(); break;
    }
    ota_st.cancel = 0;
    __sync_synchronize();
    ota_st.busy = 0;
    return 0;
}
/* starts an operation in the background; 0 = started, -1 = another one is running */
static int ota_start(int op)
{
    ota_paths_init();
    if (!__sync_bool_compare_and_swap(&ota_st.busy, 0, 1)) return -1;
    ota_op = op; ota_st.cancel = 0;
    if (op == OTA_OP_CHECK) ota_set(OS_CHECKING, OE_NONE);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_t th; int r = pthread_create(&th, &at, ota_thread, 0);
    pthread_attr_destroy(&at);
    if (r) { ota_st.busy = 0; ota_set(OS_ERROR, OE_IO); return -1; }
    return 0;
}
static void ota_cancel(void) { ota_st.cancel = 1; }
static int ota_inited;
static void ota_init(void)   /* versions, settings and the last verified result (background) */
{
    ota_paths_init();
    ota_versions_refresh();
    if (!ota_inited) { ota_inited = 1; ota_conf_load(); }
    if (!ota_st.busy && (ota_st.state == OS_IDLE || ota_st.state == OS_UPTODATE)) ota_start(OTA_OP_LOAD);
}
/* weekly automatic check (only with the setting on, on Wi-Fi and charging, with the SD):
 * call from the shell's main loop; cheap (one stat every 10 min) */
static void ota_auto_tick(void)
{
    static long long next;
    long long t = (long long)time(0);
    if (t < next) return;
    next = t + 600;
    ota_paths_init();
    if (!ota_inited) { ota_inited = 1; ota_conf_load(); }
    if (!ota_st.auto_on || ota_st.busy || t - ota_st.last_check < 7 * 86400) return;
    if (!ota_charging() || !ota_wifi_up() || !ota_sd_ok()) return;
    ota_versions_refresh();
    ota_st.quiet = 1;
    ota_start(OTA_OP_CHECK);
}
#endif /* OTA_CORE_ONLY */
#undef OTA_R32
#undef OTA_R64
