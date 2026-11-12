#!/usr/bin/env python3
"""Byte-level test vectors for BTP/1, the ByOS sync protocol (docs/SYNC_PROTOCOL.md).

    python tools/btp_test_vectors.py            writes tests/btp/vectors.json
    python tools/btp_test_vectors.py --check    only runs the self-tests, writes nothing

Standard library only. Everything is deterministic: keys and nonces come from SHA-256 of
fixed labels, so running it twice gives the same file byte for byte. The C daemon (hibysync),
the Kotlin client core and any other implementation must reproduce these bytes exactly.

The crypto primitives are re-implemented here from their public specifications:
  X25519            RFC 7748
  HSalsa20/XSalsa20 "Extending the Salsa20 nonce" (Bernstein), as used by NaCl/TweetNaCl
  Poly1305          RFC 8439 section 2.5
  secretbox         NaCl crypto_secretbox_xsalsa20poly1305 (output = tag || ciphertext)
  HKDF-SHA256       RFC 5869
Before anything is written they are checked against the published vectors (RFC 7748
section 5.2 and 6.1, RFC 8439 2.5.2, RFC 5869 case 1, the NaCl "box" example).
"""
import hashlib
import hmac
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(REPO, "tests", "btp", "vectors.json")

# ====================================================================== primitives

P25519 = 2 ** 255 - 19


def x25519(k: bytes, u: bytes) -> bytes:
    """RFC 7748 X25519(k, u). Both 32 bytes, little-endian."""
    assert len(k) == 32 and len(u) == 32
    kb = bytearray(k)
    kb[0] &= 248
    kb[31] &= 127
    kb[31] |= 64
    scalar = int.from_bytes(kb, "little")
    x1 = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    a24 = 121665
    for t in range(254, -1, -1):
        kt = (scalar >> t) & 1
        swap ^= kt
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = kt
        a = (x2 + z2) % P25519
        aa = a * a % P25519
        b = (x2 - z2) % P25519
        bb = b * b % P25519
        e = (aa - bb) % P25519
        c = (x3 + z3) % P25519
        d = (x3 - z3) % P25519
        da = d * a % P25519
        cb = c * b % P25519
        x3 = (da + cb) ** 2 % P25519
        z3 = x1 * (da - cb) ** 2 % P25519
        x2 = aa * bb % P25519
        z2 = e * (aa + a24 * e) % P25519
    if swap:
        x2, x3 = x3, x2
        z2, z3 = z3, z2
    return (x2 * pow(z2, P25519 - 2, P25519) % P25519).to_bytes(32, "little")


def x25519_base(k: bytes) -> bytes:
    return x25519(k, (9).to_bytes(32, "little"))


def _rotl(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def _salsa_rounds(x):
    x = list(x)
    for _ in range(10):
        # column round
        x[4] ^= _rotl((x[0] + x[12]) & 0xFFFFFFFF, 7); x[8] ^= _rotl((x[4] + x[0]) & 0xFFFFFFFF, 9)
        x[12] ^= _rotl((x[8] + x[4]) & 0xFFFFFFFF, 13); x[0] ^= _rotl((x[12] + x[8]) & 0xFFFFFFFF, 18)
        x[9] ^= _rotl((x[5] + x[1]) & 0xFFFFFFFF, 7); x[13] ^= _rotl((x[9] + x[5]) & 0xFFFFFFFF, 9)
        x[1] ^= _rotl((x[13] + x[9]) & 0xFFFFFFFF, 13); x[5] ^= _rotl((x[1] + x[13]) & 0xFFFFFFFF, 18)
        x[14] ^= _rotl((x[10] + x[6]) & 0xFFFFFFFF, 7); x[2] ^= _rotl((x[14] + x[10]) & 0xFFFFFFFF, 9)
        x[6] ^= _rotl((x[2] + x[14]) & 0xFFFFFFFF, 13); x[10] ^= _rotl((x[6] + x[2]) & 0xFFFFFFFF, 18)
        x[3] ^= _rotl((x[15] + x[11]) & 0xFFFFFFFF, 7); x[7] ^= _rotl((x[3] + x[15]) & 0xFFFFFFFF, 9)
        x[11] ^= _rotl((x[7] + x[3]) & 0xFFFFFFFF, 13); x[15] ^= _rotl((x[11] + x[7]) & 0xFFFFFFFF, 18)
        # row round
        x[1] ^= _rotl((x[0] + x[3]) & 0xFFFFFFFF, 7); x[2] ^= _rotl((x[1] + x[0]) & 0xFFFFFFFF, 9)
        x[3] ^= _rotl((x[2] + x[1]) & 0xFFFFFFFF, 13); x[0] ^= _rotl((x[3] + x[2]) & 0xFFFFFFFF, 18)
        x[6] ^= _rotl((x[5] + x[4]) & 0xFFFFFFFF, 7); x[7] ^= _rotl((x[6] + x[5]) & 0xFFFFFFFF, 9)
        x[4] ^= _rotl((x[7] + x[6]) & 0xFFFFFFFF, 13); x[5] ^= _rotl((x[4] + x[7]) & 0xFFFFFFFF, 18)
        x[11] ^= _rotl((x[10] + x[9]) & 0xFFFFFFFF, 7); x[8] ^= _rotl((x[11] + x[10]) & 0xFFFFFFFF, 9)
        x[9] ^= _rotl((x[8] + x[11]) & 0xFFFFFFFF, 13); x[10] ^= _rotl((x[9] + x[8]) & 0xFFFFFFFF, 18)
        x[12] ^= _rotl((x[15] + x[14]) & 0xFFFFFFFF, 7); x[13] ^= _rotl((x[12] + x[15]) & 0xFFFFFFFF, 9)
        x[14] ^= _rotl((x[13] + x[12]) & 0xFFFFFFFF, 13); x[15] ^= _rotl((x[14] + x[13]) & 0xFFFFFFFF, 18)
    return x


_SIGMA = struct.unpack("<4I", b"expand 32-byte k")


def _salsa_input(key: bytes, sixteen: bytes):
    k = struct.unpack("<8I", key)
    n = struct.unpack("<4I", sixteen)
    return [_SIGMA[0], k[0], k[1], k[2], k[3], _SIGMA[1], n[0], n[1],
            n[2], n[3], _SIGMA[2], k[4], k[5], k[6], k[7], _SIGMA[3]]


def hsalsa20(key: bytes, n16: bytes) -> bytes:
    z = _salsa_rounds(_salsa_input(key, n16))
    return struct.pack("<8I", z[0], z[5], z[10], z[15], z[6], z[7], z[8], z[9])


def salsa20_block(key: bytes, n8: bytes, counter: int) -> bytes:
    inp = _salsa_input(key, n8 + struct.pack("<Q", counter))
    z = _salsa_rounds(inp)
    return struct.pack("<16I", *[(a + b) & 0xFFFFFFFF for a, b in zip(z, inp)])


def xsalsa20_stream(key: bytes, n24: bytes, length: int) -> bytes:
    sub = hsalsa20(key, n24[:16])
    out = bytearray()
    ctr = 0
    while len(out) < length:
        out += salsa20_block(sub, n24[16:24], ctr)
        ctr += 1
    return bytes(out[:length])


def poly1305(msg: bytes, key: bytes) -> bytes:
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:32], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        n = int.from_bytes(msg[i:i + 16] + b"\x01", "little")
        acc = (acc + n) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def secretbox(msg: bytes, n24: bytes, key: bytes) -> bytes:
    """NaCl crypto_secretbox_xsalsa20poly1305; returns tag(16) || ciphertext."""
    stream = xsalsa20_stream(key, n24, 32 + len(msg))
    ct = bytes(a ^ b for a, b in zip(msg, stream[32:]))
    return poly1305(ct, stream[:32]) + ct


def secretbox_open(box: bytes, n24: bytes, key: bytes):
    if len(box) < 16:
        return None
    tag, ct = box[:16], box[16:]
    stream = xsalsa20_stream(key, n24, 32 + len(ct))
    if not hmac.compare_digest(poly1305(ct, stream[:32]), tag):
        return None
    return bytes(a ^ b for a, b in zip(ct, stream[32:]))


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def sha256(*parts: bytes) -> bytes:
    h = hashlib.sha256()
    for p in parts:
        h.update(p)
    return h.digest()


H = bytes.fromhex


def self_test():
    # RFC 7748 5.2
    assert x25519(H("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                  H("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")) == \
        H("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
    # RFC 7748 6.1
    a_sk = H("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    b_sk = H("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
    a_pk, b_pk = x25519_base(a_sk), x25519_base(b_sk)
    assert a_pk == H("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
    assert b_pk == H("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
    shared = x25519(a_sk, b_pk)
    assert shared == x25519(b_sk, a_pk) == H("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
    # NaCl: crypto_box_beforenm = HSalsa20(shared, 0^16)
    firstkey = hsalsa20(shared, bytes(16))
    assert firstkey == H("1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389")
    # NaCl box example (tests/box.c / secretbox.c): first 16 bytes of the box are the tag
    nonce = H("69696ee955b62b73cd62bda875fc73d68219e0036b7a0b37")
    m = H("be075fc53c81f2d5cf141316ebeb0c7b5228c52a4c62cbd44b66849b64244ffc"
          "e5ecbaaf33bd751a1ac728d45e6c61296cdc3c01233561f41db66cce314adb31"
          "0e3be8250c46f06dceea3a7fa1348057e2f6556ad6b1318a024a838f21af1fde"
          "048977eb48f59ffd4924ca1c60902e52f0a089bc76897040e082f93776384864"
          "5e0705")
    box = secretbox(m, nonce, firstkey)
    assert box[:16] == H("f3ffc7703f9400e52a7dfb4b3d3305d9"), box[:16].hex()
    assert box[16:48] == H("8e993b9f48681273c29650ba32fc76ce48332ea7164d96a4476fb8c531a1186a"), box[16:48].hex()
    assert secretbox_open(box, nonce, firstkey) == m
    bad = bytearray(box); bad[20] ^= 1
    assert secretbox_open(bytes(bad), nonce, firstkey) is None
    # RFC 8439 2.5.2
    assert poly1305(b"Cryptographic Forum Research Group",
                    H("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")) == \
        H("a8061dc1305136c6c22b8baf0c0127a9")
    # RFC 5869 test case 1
    prk = hkdf_extract(H("000102030405060708090a0b0c"), b"\x0b" * 22)
    assert prk == H("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5")
    assert hkdf_expand(prk, H("f0f1f2f3f4f5f6f7f8f9"), 42) == \
        H("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865")
    return {"x25519_shared": shared, "firstkey": firstkey}


# ====================================================================== BTP/1 encoding

MAGIC = b"BT"
VERSION = 1
HEADER_LEN = 12
MAX_PAYLOAD = 65544          # 8-byte DATA offset + 64 KiB of data
MAX_DATA = 65536
TAG_LEN = 16
MAX_RECORD = HEADER_LEN + MAX_PAYLOAD + TAG_LEN

F_END = 0x01

T = {
    "HELLO": 0x01, "ERROR": 0x02, "OK": 0x03, "PING": 0x04, "PONG": 0x05, "BYE": 0x06, "CANCEL": 0x07,
    "PAIR_COMMIT": 0x10, "PAIR_DEVICE": 0x11, "PAIR_REVEAL": 0x12, "PAIR_CONFIRM": 0x13, "PAIR_RESULT": 0x14,
    "AUTH_INIT": 0x18, "AUTH_REPLY": 0x19, "AUTH_FINISH": 0x1A, "AUTH_OK": 0x1B,
    "STAT": 0x20, "STAT_R": 0x21, "LIST": 0x22, "LIST_R": 0x23, "DF": 0x24, "DF_R": 0x25,
    "MKDIR": 0x26, "MOVE": 0x27, "DELETE": 0x28,
    "PUT_BEGIN": 0x30, "PUT_READY": 0x31, "DATA": 0x32, "PUT_END": 0x33, "PUT_DONE": 0x34,
    "GET": 0x38, "GET_R": 0x39, "GET_END": 0x3A,
    "DB_SNAPSHOT": 0x40, "DB_STAGE": 0x41, "DB_STATUS": 0x42, "DB_STATUS_R": 0x43, "DB_UNSTAGE": 0x44,
    "LIB_RESCAN": 0x45,
    "UPDATE_STAGE": 0x50, "UPDATE_STAGE_R": 0x51, "UPDATE_INSTALL": 0x52, "UPDATE_STATUS": 0x53,
    "UPDATE_STATUS_R": 0x54,
    "STATUS": 0x60, "STATUS_R": 0x61,
}

E = {
    "E_PROTO": 1, "E_VERSION": 2, "E_UNKNOWN_TYPE": 3, "E_AUTH_REQUIRED": 4, "E_AUTH_FAILED": 5,
    "E_PAIR_CLOSED": 6, "E_PAIR_REJECTED": 7, "E_BUSY": 8, "E_TOO_LARGE": 9, "E_RATE_LIMIT": 10,
    "E_TIMEOUT": 11, "E_CANCELLED": 12, "E_UNSUPPORTED": 13, "E_INTERNAL": 14,
    "E_BAD_PATH": 32, "E_NOT_FOUND": 33, "E_EXISTS": 34, "E_NOT_DIR": 35, "E_IS_DIR": 36,
    "E_NOT_EMPTY": 37, "E_NO_SPACE": 38, "E_IO": 39, "E_SD_UNAVAILABLE": 40, "E_HASH_MISMATCH": 41,
    "E_OFFSET": 42, "E_FILE_TOO_BIG": 43, "E_NOT_ALLOWED": 44,
    "E_SIGNATURE": 64, "E_UPDATE_INVALID": 65, "E_DOWNGRADE": 66, "E_BATTERY": 67, "E_NEEDS_FIRMWARE": 68,
    "E_NOT_STAGED": 69, "E_DB_INVALID": 80, "E_DB_STALE": 81,
}


def u8(v): return struct.pack(">B", v)
def u16(v): return struct.pack(">H", v)
def u32(v): return struct.pack(">I", v)
def u64(v): return struct.pack(">Q", v)
def i64(v): return struct.pack(">q", v)


def str16(s):
    b = s.encode("utf-8") if isinstance(s, str) else s
    assert len(b) <= 0xFFFF
    return u16(len(b)) + b


def str8(s):
    b = s.encode("utf-8")
    assert len(b) <= 255
    return u8(len(b)) + b


def tlv(tag, value: bytes):
    return u8(tag) + u16(len(value)) + value


def frame(mtype, stream, payload=b"", flags=0):
    assert len(payload) <= MAX_PAYLOAD
    return MAGIC + u8(VERSION) + u8(T[mtype] if isinstance(mtype, str) else mtype) + u8(flags) + u8(0) + \
        u16(stream) + u32(len(payload)) + payload


def parse_header(b: bytes):
    """Returns (type, flags, stream, length) or raises ValueError(error code name)."""
    if len(b) < HEADER_LEN:
        raise ValueError("E_PROTO")
    if b[0:2] != MAGIC:
        raise ValueError("E_PROTO")
    if b[2] != VERSION:
        raise ValueError("E_VERSION")
    if b[4] & ~F_END & 0xFF:
        raise ValueError("E_PROTO")
    if b[5] != 0:
        raise ValueError("E_PROTO")
    length = struct.unpack(">I", b[8:12])[0]
    if length > MAX_PAYLOAD:
        raise ValueError("E_TOO_LARGE")
    if len(b) != HEADER_LEN + length:
        raise ValueError("E_PROTO")
    return b[3], b[4], struct.unpack(">H", b[6:8])[0], length


C2D, D2C = 0x01, 0x02


def nonce_for(direction, counter):
    return u8(direction) + bytes(15) + u64(counter)


def seal(key, direction, counter, inner: bytes):
    box = secretbox(inner, nonce_for(direction, counter), key)
    return u32(len(box)) + box


def open_record(key, direction, counter, rec: bytes):
    n = struct.unpack(">I", rec[:4])[0]
    if n < TAG_LEN + HEADER_LEN or n > MAX_RECORD or len(rec) != 4 + n:
        return None
    return secretbox_open(rec[4:], nonce_for(direction, counter), key)


# ----------------------------------------------------------------- path rules

FORBIDDEN = set('\\:*?"<>|')
MAX_PATH = 1024
MAX_COMPONENT = 255
MAX_DEPTH = 32


def check_path(p: bytes, allow_root=False):
    """None if valid, else the reason (same names as SYNC_PROTOCOL.md section 9)."""
    if p == b"":
        return None if allow_root else "empty"
    if len(p) > MAX_PATH:
        return "too_long"
    try:
        s = p.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return "bad_utf8"
    if s.startswith("/"):
        return "absolute"
    comps = s.split("/")
    if len(comps) > MAX_DEPTH:
        return "too_deep"
    for c in comps:
        if c == "":
            return "empty_component"
        if c in (".", ".."):
            return "dot_component"
        if c.startswith("."):
            return "hidden"
        for ch in c:
            if ord(ch) < 0x20 or ord(ch) == 0x7F:
                return "control_char"
            if ch in FORBIDDEN:
                return "forbidden_char"
        if c.endswith(" ") or c.endswith("."):
            return "trailing_dot_space"
        if len(c.encode("utf-8")) > MAX_COMPONENT:
            return "component_too_long"
    return None


# ----------------------------------------------------------------- deterministic material

def det(label, n=32):
    out = b""
    i = 0
    while len(out) < n:
        out += sha256(b"BTP/1 test vector|" + label.encode() + b"|" + bytes([i]))
        i += 1
    return out[:n]


def sas_from(prk):
    v = struct.unpack(">I", hkdf_expand(prk, b"BTP/1 sas", 4))[0]
    return "%06d" % (v % 1000000)


def hx(b): return b.hex()


def fr(name, mtype, stream, payload, flags=0, fields=None):
    b = frame(mtype, stream, payload, flags)
    return {"name": name, "type": mtype, "type_code": T[mtype], "stream": stream, "flags": flags,
            "payload": hx(payload), "frame": hx(b), "fields": fields or {}}


CAPS = {"FILES": 1 << 0, "RESUME": 1 << 1, "DB_SNAPSHOT": 1 << 2, "DB_STAGE": 1 << 3, "LIB_RESCAN": 1 << 4,
        "UPDATE": 1 << 5, "PAIRING": 1 << 6, "ENCRYPTION": 1 << 7, "STATUS": 1 << 8}
HF = {"LOCAL": 1 << 0, "PAIRING_OPEN": 1 << 1, "SD_MOUNTED": 1 << 2, "SHELL_ATTACHED": 1 << 3}
TLV = {"SOFTWARE": 0x01, "NAME": 0x02, "DEVICE_ID": 0x03, "BYOS_VERSION": 0x04, "FIRMWARE": 0x05, "MODEL": 0x06}


def hello(role, flags, caps, tlvs):
    body = u8(1) + u8(1) + u8(role) + u8(flags) + u32(MAX_PAYLOAD) + u32(caps)
    for tag, val in tlvs:
        body += tlv(TLV[tag], val if isinstance(val, bytes) else val.encode("utf-8"))
    return body


ALL_CAPS = sum(CAPS.values())


def build():
    prims = self_test()
    v = {
        "format": 1,
        "protocol": "BTP/1",
        "spec": "docs/SYNC_PROTOCOL.md",
        "generator": "tools/btp_test_vectors.py",
        "notes": "All byte strings are lowercase hex. Integers in frames are big-endian. "
                 "X25519 keys/scalars are little-endian as in RFC 7748.",
        "constants": {"header_len": HEADER_LEN, "max_payload": MAX_PAYLOAD, "max_data": MAX_DATA,
                      "tag_len": TAG_LEN, "max_record": MAX_RECORD, "tcp_port": 7340, "udp_probe_port": 7340,
                      "udp_announce_port": 7341, "types": T, "errors": E, "caps": CAPS, "hello_flags": HF,
                      "hello_tlv": TLV},
    }

    # ------------------------------------------------------------ primitives
    sb_key, sb_nonce = det("secretbox key"), det("secretbox nonce", 24)
    sb_msgs = [b"", b"BTP/1", bytes(range(64)), bytes(range(256)) * 2]
    v["primitives"] = {
        "x25519": [
            {"scalar": "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
             "u": "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
             "out": "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", "source": "RFC 7748 5.2"},
            {"scalar": "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
             "u": "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
             "out": hx(prims["x25519_shared"]), "source": "RFC 7748 6.1"},
        ],
        "hsalsa20": [{"key": hx(prims["x25519_shared"]), "in": "00" * 16, "out": hx(prims["firstkey"]),
                      "source": "NaCl crypto_box_beforenm example"}],
        "poly1305": [{"key": "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b",
                      "msg": hx(b"Cryptographic Forum Research Group"), "tag": "a8061dc1305136c6c22b8baf0c0127a9",
                      "source": "RFC 8439 2.5.2"}],
        "secretbox": [{"key": hx(sb_key), "nonce": hx(sb_nonce), "msg": hx(m), "box": hx(secretbox(m, sb_nonce, sb_key))}
                      for m in sb_msgs],
        "hkdf_sha256": [
            {"ikm": "0b" * 22, "salt": "000102030405060708090a0b0c", "info": "f0f1f2f3f4f5f6f7f8f9", "len": 42,
             "okm": "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
             "source": "RFC 5869 A.1"}],
    }

    # ------------------------------------------------------------ keys
    cs = det("client static secret")
    ds = det("device static secret")
    ce_pair, de_pair = det("client ephemeral pair"), det("device ephemeral pair")
    ce_auth, de_auth = det("client ephemeral auth"), det("device ephemeral auth")
    Cs, Ds = x25519_base(cs), x25519_base(ds)
    device_id = sha256(Ds)[:8]
    key_id = sha256(Cs)[:16]

    # ------------------------------------------------------------ hellos
    hello_c = hello(0, 0, ALL_CAPS, [("SOFTWARE", "byos-cli 0.1.0"), ("NAME", "Desktop")])
    hello_d_usb = hello(1, HF["LOCAL"] | HF["SD_MOUNTED"] | HF["SHELL_ATTACHED"], ALL_CAPS,
                        [("SOFTWARE", "hibysync 0.1.0"), ("NAME", "HiBy R3 II"), ("DEVICE_ID", device_id),
                         ("BYOS_VERSION", "0.8.0"), ("FIRMWARE", "0.8.0"), ("MODEL", "R3II-2025")])
    hello_d_wifi = hello(1, HF["PAIRING_OPEN"] | HF["SD_MOUNTED"] | HF["SHELL_ATTACHED"], ALL_CAPS,
                         [("SOFTWARE", "hibysync 0.1.0"), ("NAME", "HiBy R3 II"), ("DEVICE_ID", device_id),
                          ("BYOS_VERSION", "0.8.0"), ("FIRMWARE", "0.8.0"), ("MODEL", "R3II-2025")])

    sha_hello = sha256(b"hello world\n")
    big = bytes((i * 7 + 3) & 0xFF for i in range(70000))
    sha_big = sha256(big)
    frames = [
        fr("hello client", "HELLO", 0, hello_c, fields={"proto_min": 1, "proto_max": 1, "role": 0, "flags": 0,
           "max_payload": MAX_PAYLOAD, "caps": ALL_CAPS, "software": "byos-cli 0.1.0", "name": "Desktop"}),
        fr("hello device (usb, local link)", "HELLO", 0, hello_d_usb, fields={"role": 1,
           "flags": HF["LOCAL"] | HF["SD_MOUNTED"] | HF["SHELL_ATTACHED"], "device_id": hx(device_id)}),
        fr("ping", "PING", 0, bytes.fromhex("0102030405060708")),
        fr("pong", "PONG", 0, bytes.fromhex("0102030405060708")),
        fr("bye", "BYE", 0, b""),
        fr("ok", "OK", 5, b"", F_END),
        fr("error not found", "ERROR", 5, u16(E["E_NOT_FOUND"]) + str16("no such file"), F_END,
           {"code": E["E_NOT_FOUND"], "message": "no such file"}),
        fr("cancel stream 9", "CANCEL", 9, b""),
        fr("stat", "STAT", 1, u8(0) + str16("Music/ABBA/01 Waterloo.flac"),
           fields={"ns": 0, "path": "Music/ABBA/01 Waterloo.flac"}),
        fr("stat reply", "STAT_R", 1, u8(0) + u64(31457280) + i64(1767225600), F_END,
           {"kind": 0, "size": 31457280, "mtime": 1767225600}),
        fr("list", "LIST", 2, u8(0) + u8(0) + str16("Music"), fields={"ns": 0, "flags": 0, "path": "Music"}),
        fr("list reply page", "LIST_R", 2,
           u16(2) + u8(1) + u64(0) + i64(1767225600) + str16("ABBA") +
           u8(0) + u64(1234) + i64(1767225601) + str16("Ñandú.mp3"), F_END,
           {"count": 2, "entries": [{"kind": 1, "size": 0, "mtime": 1767225600, "name": "ABBA"},
                                    {"kind": 0, "size": 1234, "mtime": 1767225601, "name": "Ñandú.mp3"}]}),
        fr("df", "DF", 3, b""),
        fr("df reply", "DF_R", 3, u8(0) + u8(2) + u64(127865454592) + u64(64000000000) + u64(150994944) + u64(25165824),
           F_END, {"sd_state": 0, "sd_fs": 2, "sd_total": 127865454592, "sd_free": 64000000000,
                   "data_total": 150994944, "data_free": 25165824}),
        fr("mkdir -p", "MKDIR", 4, u8(0) + u8(1) + str16("Music/New/Album"), fields={"ns": 0, "flags": 1}),
        fr("move", "MOVE", 6, u8(0) + u8(0) + str16("Music/a.flac") + str16("Music/b.flac"),
           fields={"ns": 0, "flags": 0, "src": "Music/a.flac", "dst": "Music/b.flac"}),
        fr("delete recursive", "DELETE", 7, u8(0) + u8(1) + str16("Music/Old"), fields={"ns": 0, "flags": 1}),
        fr("put begin", "PUT_BEGIN", 11,
           u8(0) + u8(0) + u64(12) + i64(1767225600) + sha_hello + str16("Docs/hello.txt"),
           fields={"ns": 0, "mode": 0, "size": 12, "mtime": 1767225600, "sha256": hx(sha_hello), "path": "Docs/hello.txt"}),
        fr("put ready (fresh)", "PUT_READY", 11, u64(0), fields={"offset": 0}),
        fr("data", "DATA", 11, u64(0) + b"hello world\n", fields={"offset": 0, "len": 12}),
        fr("put end", "PUT_END", 11, b""),
        fr("put done", "PUT_DONE", 11, u8(0) + u64(12), F_END, {"result": 0, "size": 12}),
        fr("put done skipped", "PUT_DONE", 13, u8(1) + u64(12), F_END, {"result": 1, "size": 12}),
        fr("get", "GET", 15, u8(0) + u8(1) + u64(0) + str16("Docs/hello.txt"), fields={"ns": 0, "flags": 1, "offset": 0}),
        fr("get reply", "GET_R", 15, u64(12) + i64(1767225600) + u64(0), fields={"size": 12, "mtime": 1767225600, "offset": 0}),
        fr("get end", "GET_END", 15, sha_hello, F_END, {"sha256": hx(sha_hello)}),
        fr("db snapshot", "DB_SNAPSHOT", 17, b""),
        fr("db stage", "DB_STAGE", 19, u8(0) + det("base db", 32), fields={"flags": 0, "base_sha256": hx(det("base db", 32))}),
        fr("db status reply", "DB_STATUS_R", 21,
           u8(1) + det("staged db", 32) + det("base db", 32) + u64(2174976) + u8(0) + u64(25165824), F_END,
           {"staged": 1, "staged_sha256": hx(det("staged db", 32)), "live_sha256": hx(det("base db", 32)),
            "live_size": 2174976, "last_apply": 0, "data_free": 25165824}),
        fr("lib rescan paths", "LIB_RESCAN", 23, u8(1) + u16(2) + str16("Music/a.flac") + str16("Music/b.flac"),
           fields={"mode": 1, "paths": ["Music/a.flac", "Music/b.flac"]}),
        fr("update stage", "UPDATE_STAGE", 25, u8(0), fields={"flags": 0}),
        fr("update stage reply", "UPDATE_STAGE_R", 25, str16("0.8.1") + u8(1) + u8(0) + u64(4718592), F_END,
           {"version": "0.8.1", "restart": 1, "needs_firmware": 0, "size": 4718592}),
        fr("update install", "UPDATE_INSTALL", 27, u8(0), fields={"flags": 0}),
        fr("update status reply", "UPDATE_STATUS_R", 29, u8(3) + u16(420) + u16(0) + str16("0.8.1"), F_END,
           {"state": 3, "permille": 420, "error": 0, "version": "0.8.1"}),
        fr("status reply", "STATUS_R", 31, u8(87) + u8(1) + u8(0) +
           tlv(TLV["BYOS_VERSION"], b"0.8.0") + tlv(TLV["FIRMWARE"], b"0.8.0"), F_END,
           {"battery": 87, "charging": 1, "sd_state": 0, "byos_version": "0.8.0", "firmware": "0.8.0"}),
    ]
    # a full-size DATA frame (64 KiB) and the resume of the 70000-byte file at the durable offset 65536
    full = frame("DATA", 41, u64(0) + big[:MAX_DATA])
    frames.append({"name": "data full size (64 KiB)", "type": "DATA", "type_code": T["DATA"], "stream": 41,
                   "flags": 0, "frame_len": len(full), "frame_sha256": hx(sha256(full)),
                   "frame_head": hx(full[:32]), "fields": {"offset": 0, "len": MAX_DATA,
                   "content": "byte i = (i*7+3) & 0xff for i in 0..69999", "file_sha256": hx(sha_big)}})
    frames.append(fr("put ready (resume at durable offset)", "PUT_READY", 41, u64(65536), fields={"offset": 65536}))
    frames.append(fr("data after resume", "DATA", 41, u64(65536) + big[65536:], fields={"offset": 65536, "len": 70000 - 65536}))
    v["frames"] = frames

    v["invalid_frames"] = [
        {"name": "bad magic", "frame": hx(b"BX" + frame("OK", 1)[2:]), "error": "E_PROTO"},
        {"name": "version 2", "frame": hx(b"BT\x02" + frame("OK", 1)[3:]), "error": "E_VERSION"},
        {"name": "reserved flag bit", "frame": hx(frame("OK", 1, b"", 0x02)), "error": "E_PROTO"},
        {"name": "reserved byte not zero", "frame": hx(frame("OK", 1)[:5] + b"\x01" + frame("OK", 1)[6:]), "error": "E_PROTO"},
        {"name": "length over max_payload", "header": hx(MAGIC + u8(1) + u8(T["DATA"]) + u8(0) + u8(0) + u16(1) + u32(MAX_PAYLOAD + 1)),
         "error": "E_TOO_LARGE"},
        {"name": "unknown type 0x7f", "frame": hx(frame(0x7F, 1)), "error": "E_UNKNOWN_TYPE"},
    ]
    for item in v["invalid_frames"]:
        b = bytes.fromhex(item.get("frame") or item["header"])
        try:
            t, _, _, _ = parse_header(b) if "frame" in item else parse_header(b + bytes(MAX_PAYLOAD + 1))
            got = "E_UNKNOWN_TYPE" if t not in T.values() else "ok"
        except ValueError as e:
            got = str(e)
        assert got == item["error"], (item["name"], got)

    # ------------------------------------------------------------ paths
    good = ["Music", "Music/ABBA/01 Waterloo.flac", "Ñandú/Canción (live).mp3", "a/b/c/d", "日本語/曲.flac",
            "x" * 255, "Books/Audio Book - Part 1.m4b"]
    bad = [("/Music", "absolute"), ("Music//a", "empty_component"), ("Music/", "empty_component"),
           ("Music/../etc", "dot_component"), ("./a", "dot_component"), (".temp/x", "hidden"), ("Music/.hidden", "hidden"),
           ("a\x01b", "control_char"), ("a\x7fb", "control_char"), ("a\\b", "forbidden_char"), ("C:x", "forbidden_char"),
           ("what?", "forbidden_char"), ("a|b", "forbidden_char"), ("trailing. ", "trailing_dot_space"),
           ("dir./x", "trailing_dot_space"), ("x" * 256, "component_too_long"), ("/".join(["d"] * 33), "too_deep"),
           ("a/" * 512 + "b", "too_long"), ("", "empty")]
    paths = {"valid": [], "invalid": []}
    for p in good:
        assert check_path(p.encode()) is None, p
        paths["valid"].append({"path_hex": hx(p.encode()), "path": p})
    for p, why in bad:
        got = check_path(p.encode())
        assert got == why, (p, got, why)
        paths["invalid"].append({"path_hex": hx(p.encode()), "reason": why})
    bad_utf8 = b"Music/\xc3\x28.flac"
    assert check_path(bad_utf8) == "bad_utf8"
    paths["invalid"].append({"path_hex": hx(bad_utf8), "reason": "bad_utf8"})
    paths["root"] = {"path_hex": "", "note": "the empty path names the namespace root; only LIST, STAT and DF accept it"}
    v["paths"] = paths

    # ------------------------------------------------------------ PUT part-file names (section 11.2)
    def part_name(path, digest, size):
        pid = sha256(path.encode("utf-8") + b"\x00" + digest + u64(size))[:8].hex()
        parent = path.rsplit("/", 1)[0] + "/" if "/" in path else ""
        return parent + ".btp-" + pid + ".part"
    v["part_files"] = [
        {"path": "Docs/hello.txt", "size": 12, "sha256": hx(sha_hello), "part": part_name("Docs/hello.txt", sha_hello, 12)},
        {"path": "big.bin", "size": 70000, "sha256": hx(sha_big), "part": part_name("big.bin", sha_big, 70000)},
        {"path": "Music/Ñandú.flac", "size": 70000, "sha256": hx(sha_big), "part": part_name("Music/Ñandú.flac", sha_big, 70000)},
    ]
    v["resume_offsets"] = [{"part_size": s, "size": n, "offset": min((s // 65536) * 65536, n)}
                           for s, n in [(0, 70000), (65535, 70000), (65536, 70000), (69999, 70000),
                                        (131072, 100000), (200000, 140000)]]

    # ------------------------------------------------------------ discovery
    probe_nonce = det("probe nonce", 8)
    probe = b"BTPD" + u8(1) + u8(1) + u16(0) + probe_nonce
    ann = b"BTPD" + u8(1) + u8(2) + u16(0x0003) + probe_nonce + u16(7340) + device_id + str8("HiBy R3 II")
    unsol = b"BTPD" + u8(1) + u8(2) + u16(0x0002) + bytes(8) + u16(7340) + device_id + str8("HiBy R3 II")
    v["discovery"] = [
        {"name": "probe", "packet": hx(probe), "fields": {"version": 1, "type": 1, "flags": 0, "nonce": hx(probe_nonce)}},
        {"name": "announce (answer)", "packet": hx(ann), "fields": {"version": 1, "type": 2, "flags": 3,
         "nonce": hx(probe_nonce), "tcp_port": 7340, "device_id": hx(device_id), "name": "HiBy R3 II"}},
        {"name": "announce (unsolicited, sd mounted, pairing closed)", "packet": hx(unsol),
         "fields": {"flags": 2, "nonce": "00" * 8}},
    ]

    # ------------------------------------------------------------ pairing over Wi-Fi
    Ce, De = x25519_base(ce_pair), x25519_base(de_pair)
    nc, nd = det("pair client nonce", 16), det("pair device nonce", 16)
    commit = sha256(b"BTP/1 commit", Ce, nc)
    th = sha256(b"BTP/1 pair", sha256(hello_c), sha256(hello_d_wifi), commit, De, nd, Ce, nc)
    ss = x25519(ce_pair, De)
    assert ss == x25519(de_pair, Ce)
    prk = hkdf_extract(th, ss)
    k_c2d, k_d2c = hkdf_expand(prk, b"BTP/1 c2d", 32), hkdf_expand(prk, b"BTP/1 d2c", 32)
    sas = sas_from(prk)

    clear = [
        ("c2d", fr("hello client", "HELLO", 0, hello_c)),
        ("d2c", fr("hello device (wifi, pairing open)", "HELLO", 0, hello_d_wifi)),
        ("c2d", fr("pair commit", "PAIR_COMMIT", 0, commit)),
        ("d2c", fr("pair device", "PAIR_DEVICE", 0, De + nd)),
        ("c2d", fr("pair reveal", "PAIR_REVEAL", 0, Ce + nc)),
    ]
    enc_plain = [
        ("c2d", 0, frame("PAIR_CONFIRM", 0, Cs + str16("Desktop")), "pair confirm"),
        ("d2c", 0, frame("PAIR_RESULT", 0, u8(0) + Ds + device_id + str16("HiBy R3 II")), "pair result accepted"),
        ("c2d", 1, frame("DF", 1, b""), "df"),
        ("d2c", 1, frame("DF_R", 1, u8(0) + u8(2) + u64(127865454592) + u64(64000000000) + u64(150994944) + u64(25165824), F_END), "df reply"),
    ]
    enc = []
    for d, ctr, inner, name in enc_plain:
        key, dirb = (k_c2d, C2D) if d == "c2d" else (k_d2c, D2C)
        rec = seal(key, dirb, ctr, inner)
        assert open_record(key, dirb, ctr, rec) == inner
        assert open_record(key, dirb, ctr + 1, rec) is None
        other = k_d2c if key == k_c2d else k_c2d
        assert open_record(other, dirb, ctr, rec) is None
        enc.append({"dir": d, "counter": ctr, "name": name, "nonce": hx(nonce_for(dirb, ctr)),
                    "inner_frame": hx(inner), "record": hx(rec)})
    tampered = bytearray(bytes.fromhex(enc[2]["record"]))
    tampered[-1] ^= 0x80
    v["pairing"] = {
        "client_static_secret": hx(cs), "client_static_public": hx(Cs),
        "device_static_secret": hx(ds), "device_static_public": hx(Ds),
        "device_id": hx(device_id), "client_key_id": hx(key_id),
        "client_ephemeral_secret": hx(ce_pair), "client_ephemeral_public": hx(Ce), "client_nonce": hx(nc),
        "device_ephemeral_secret": hx(de_pair), "device_ephemeral_public": hx(De), "device_nonce": hx(nd),
        "commit": hx(commit), "transcript_hash": hx(th), "shared_secret": hx(ss), "prk": hx(prk),
        "sas": sas, "k_c2d": hx(k_c2d), "k_d2c": hx(k_d2c),
        "clear_frames": [{"dir": d, **f} for d, f in clear],
        "encrypted": enc,
        "tampered_record": {"record": hx(bytes(tampered)), "dir": "c2d", "counter": 1, "expect": "reject (E_AUTH_FAILED, close)"},
    }

    # ------------------------------------------------------------ reconnect (AUTH) over Wi-Fi
    Ce2, De2 = x25519_base(ce_auth), x25519_base(de_auth)
    hello_d_wifi2 = hello(1, HF["SD_MOUNTED"] | HF["SHELL_ATTACHED"], ALL_CAPS,
                          [("SOFTWARE", "hibysync 0.1.0"), ("NAME", "HiBy R3 II"), ("DEVICE_ID", device_id),
                           ("BYOS_VERSION", "0.8.0"), ("FIRMWARE", "0.8.0"), ("MODEL", "R3II-2025")])
    ee = x25519(ce_auth, De2)
    es = x25519(cs, De2)
    se = x25519(ce_auth, Ds)
    assert ee == x25519(de_auth, Ce2) and es == x25519(de_auth, Cs) and se == x25519(ds, Ce2)
    th2 = sha256(b"BTP/1 auth", sha256(hello_c), sha256(hello_d_wifi2), key_id, Ce2, De2, Cs, Ds)
    prk2 = hkdf_extract(th2, ee + es + se)
    a_c2d, a_d2c = hkdf_expand(prk2, b"BTP/1 c2d", 32), hkdf_expand(prk2, b"BTP/1 d2c", 32)
    clear2 = [
        ("c2d", fr("hello client", "HELLO", 0, hello_c)),
        ("d2c", fr("hello device (wifi, paired peers)", "HELLO", 0, hello_d_wifi2)),
        ("c2d", fr("auth init", "AUTH_INIT", 0, key_id + Ce2)),
        ("d2c", fr("auth reply", "AUTH_REPLY", 0, De2)),
    ]
    plain2 = [
        ("c2d", 0, frame("AUTH_FINISH", 0, b""), "auth finish"),
        ("d2c", 0, frame("AUTH_OK", 0, b""), "auth ok"),
        ("c2d", 1, frame("PUT_BEGIN", 1, u8(0) + u8(0) + u64(12) + i64(1767225600) + sha_hello + str16("Docs/hello.txt")), "put begin"),
        ("d2c", 1, frame("PUT_READY", 1, u64(0)), "put ready"),
        ("c2d", 2, frame("DATA", 1, u64(0) + b"hello world\n"), "data"),
        ("c2d", 3, frame("PUT_END", 1, b""), "put end"),
        ("d2c", 2, frame("PUT_DONE", 1, u8(0) + u64(12), F_END), "put done"),
    ]
    enc2 = []
    for d, ctr, inner, name in plain2:
        key, dirb = (a_c2d, C2D) if d == "c2d" else (a_d2c, D2C)
        rec = seal(key, dirb, ctr, inner)
        assert open_record(key, dirb, ctr, rec) == inner
        enc2.append({"dir": d, "counter": ctr, "name": name, "nonce": hx(nonce_for(dirb, ctr)),
                     "inner_frame": hx(inner), "record": hx(rec)})
    v["auth"] = {
        "client_static_secret": hx(cs), "device_static_secret": hx(ds),
        "client_static_public": hx(Cs), "device_static_public": hx(Ds), "client_key_id": hx(key_id),
        "client_ephemeral_secret": hx(ce_auth), "client_ephemeral_public": hx(Ce2),
        "device_ephemeral_secret": hx(de_auth), "device_ephemeral_public": hx(De2),
        "dh_ee": hx(ee), "dh_es": hx(es), "dh_se": hx(se), "transcript_hash": hx(th2), "prk": hx(prk2),
        "k_c2d": hx(a_c2d), "k_d2c": hx(a_d2c),
        "clear_frames": [{"dir": d, **f} for d, f in clear2],
        "encrypted": enc2,
    }

    # ------------------------------------------------------------ SAS edge cases
    v["sas_examples"] = []
    for i in range(4):
        p = hkdf_extract(det("sas salt %d" % i), det("sas ikm %d" % i))
        raw = hkdf_expand(p, b"BTP/1 sas", 4)
        v["sas_examples"].append({"prk": hx(p), "sas_bytes": hx(raw), "sas": sas_from(p)})
    return v


def main():
    v = build()
    if "--check" in sys.argv:
        print("BTP/1 self-tests passed")
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    data = json.dumps(v, indent=1, ensure_ascii=False) + "\n"
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(data)
    print("wrote %s (%d bytes, sha256 %s); pairing SAS %s" % (os.path.relpath(OUT, REPO), len(data.encode()),
          sha256(data.encode()).hex()[:16], v["pairing"]["sas"]))


if __name__ == "__main__":
    main()
