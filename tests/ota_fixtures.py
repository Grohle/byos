#!/usr/bin/env python3
"""Fixtures for tests/ota_test.c (run by tests/ota_test.sh):  ota_fixtures.py OUTDIR

  test1.key test2.key, pub1.hex pub2.hex   deterministic TEST keys (never used for releases)
  vectors.txt     sha256/sha512 of random messages (padding edges) and Ed25519 cases:
                  RFC 8032 vectors, signatures made with the Python backends (cryptography,
                  pynacl if present, pure Python) and tampered ones; "ed PK MSG SIG EXPECT"
  json/*.json     hibyos-update.json variants + json/expect.txt "file sig parse [lang changelog]"
  tar/*.tar       ustar archives (good and hostile) + tar/expect.txt "file rc"
"""
import hashlib
import json
import os
import random
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import ota_sign  # noqa: E402
import ota_ed25519 as PURE  # noqa: E402

OUT = sys.argv[1]
os.makedirs(os.path.join(OUT, "json"), exist_ok=True)
os.makedirs(os.path.join(OUT, "tar"), exist_ok=True)
L = 2 ** 252 + 27742317777372353535851937790883648493

seed1 = hashlib.sha256(b"hibyos-ota-TEST-key-1 (public, tests only)").digest()
seed2 = hashlib.sha256(b"hibyos-ota-TEST-key-2 (public, tests only)").digest()
for n, s in (("1", seed1), ("2", seed2)):
    with open(os.path.join(OUT, f"test{n}.key"), "w") as f:
        f.write(ota_sign.PREFIX + s.hex() + "\n")
    with open(os.path.join(OUT, f"pub{n}.hex"), "w") as f:
        f.write(ota_sign.public_hex(s))
pub1, pub2 = ota_sign.public_hex(seed1), ota_sign.public_hex(seed2)

backends = []
for b in ("cryptography", "nacl", "pure"):
    try:
        backends.append(ota_sign._backend(b))
    except SystemExit:
        pass
print("backends:", ", ".join(b[0] for b in backends))

# ------------------------------------------------------------------ vectors
rng = random.Random(1)
lines = []
for n in (0, 1, 3, 55, 56, 57, 63, 64, 65, 111, 112, 113, 119, 120, 127, 128, 129, 200, 1000, 4097):
    m = bytes(rng.randrange(256) for _ in range(n))
    lines.append(f"sha256 {m.hex() or '-'} {hashlib.sha256(m).hexdigest()}")
    lines.append(f"sha512 {m.hex() or '-'} {hashlib.sha512(m).hexdigest()}")

RFC = [  # RFC 8032 7.1, tests 1-3 (secret, public, message, signature)
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]
for sk, pk, msg, sig in RFC:
    s = bytes.fromhex(sk)
    for name, pubf, signf, verf in backends:
        assert pubf(s).hex() == pk, f"{name}: RFC public key"
        assert signf(s, bytes.fromhex(msg)).hex() == sig, f"{name}: RFC signature"
        assert verf(bytes.fromhex(pk), bytes.fromhex(msg), bytes.fromhex(sig)), f"{name}: RFC verify"
    lines.append(f"ed {pk} {msg or '-'} {sig} 1")

for i in range(48):
    s = bytes(rng.randrange(256) for _ in range(32))
    m = bytes(rng.randrange(256) for _ in range(rng.choice((0, 1, 31, 32, 64, 100, 300))))
    name, pubf, signf, verf = backends[i % len(backends)]
    pk = pubf(s)
    sig = signf(s, m)
    for bn, bp, bs, bv in backends:           # every backend agrees (Ed25519 is deterministic)
        assert bp(s) == pk and bs(s, m) == sig and bv(pk, m, sig), f"backend {bn} differs from {name}"
    lines.append(f"ed {pk.hex()} {m.hex() or '-'} {sig.hex()} 1")
    j = rng.randrange(64)
    bad = bytearray(sig); bad[j] ^= 1 << rng.randrange(8)
    lines.append(f"ed {pk.hex()} {m.hex() or '-'} {bytes(bad).hex()} 0")
    if m:
        mm = bytearray(m); mm[rng.randrange(len(m))] ^= 0x40
        lines.append(f"ed {pk.hex()} {bytes(mm).hex()} {sig.hex()} 0")
    other = pubf(hashlib.sha256(s).digest())
    lines.append(f"ed {other.hex()} {m.hex() or '-'} {sig.hex()} 0")
    S = int.from_bytes(sig[32:], "little") + L          # same point, S not reduced: must be refused
    if S < 2 ** 256:
        lines.append(f"ed {pk.hex()} {m.hex() or '-'} {(sig[:32] + S.to_bytes(32, 'little')).hex()} 0")
    pk_bad = bytearray(pk); pk_bad[31] ^= 0x80
    lines.append(f"ed {bytes(pk_bad).hex()} {m.hex() or '-'} {sig.hex()} 0")
with open(os.path.join(OUT, "vectors.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")

# ------------------------------------------------------------------ JSON
def doc(**kw):
    d = {
        "format": 1, "product": "hibyos", "version": "0.9.1", "date": "2026-11-10",
        "min_version": "0.6.0", "min_firmware": "0.6.0", "needs_firmware": False,
        "firmware_url": "https://github.com/Grohle/hibyos/releases/tag/v0.9.1", "restart": "shell",
        "payload": {"name": "hibyos-payload-0.9.1.tar", "url": "https://github.com/Grohle/hibyos/releases/download/v0.9.1/hibyos-payload-0.9.1.tar",
                    "size": 123456, "sha256": "ab" * 32, "compression": "none"},
        "changelog": {"en": "Fixed things.", "es": "Arreglos.", "fr": "Corrections."},
        "signature": "ed25519:" + "0" * 128,
    }
    for k, v in kw.items():
        if k.startswith("pl_"):
            d["payload"][k[3:]] = v
        elif v is None:
            d.pop(k, None)
        else:
            d[k] = v
    return (json.dumps(d, ensure_ascii=False, indent=1) + "\n").encode()


exp = []


def put(name, data, sig, parse, *cl):
    with open(os.path.join(OUT, "json", name), "wb") as f:
        f.write(data)
    exp.append(" ".join([name, str(sig), str(parse)] + list(cl)))


S = lambda d, s=seed1: ota_sign.sign_bytes(d, s)  # noqa: E731
good = S(doc())
put("good.json", good, 0, 0, "es", "Arreglos.")
put("tampered.json", good.replace(b"Fixed things", b"Fixed thinks"), 3, 0)
put("tampered_size.json", good.replace(b"123456", b"123457"), 3, 0)
put("unsigned.json", doc(), 3, 0)
put("nosig.json", doc(signature=None), 2, 0)
put("twosig.json", doc(extra={"signature": "x"}), 2, 0)
put("badsigfmt.json", doc(signature="rsa:" + "0" * 128), 2, 0)
put("otherkey.json", S(doc(), seed2), 3, 0)
put("rotated.json", S(doc(), seed2), "R", 0)            # R: valid with [pub1, pub2]
for name, kw in (("format2", dict(format=2)), ("product", dict(product="other")), ("version", dict(version="1.x")),
                 ("version_long", dict(version="1." + "1" * 30)), ("sha_short", dict(pl_sha256="ab" * 31)),
                 ("sha_hex", dict(pl_sha256="zz" * 32)), ("size0", dict(pl_size=0)), ("size_big", dict(pl_size=200 * 1024 * 1024)),
                 ("size_str", dict(pl_size="12")), ("url_ftp", dict(pl_url="ftp://x/y.tar")), ("url_space", dict(pl_url="https://x/a b.tar")),
                 ("name_dots", dict(pl_name="../x.tar")), ("name_ext", dict(pl_name="x.zip")), ("gzip", dict(pl_compression="gzip")),
                 ("needs_str", dict(needs_firmware="yes")), ("no_payload", dict(payload=None)), ("minver_bad", dict(min_version="x"))):
    put(name + ".json", S(doc(**kw)), 0, -1)
put("trailing.json", S(doc()[:-1] + b"{}\n"), 0, -1)
nest = "x"
for _ in range(20):
    nest = [nest]
deep = doc(changelog={"en": "x", "es": "y", "fr": "z", "deep": nest})
put("deep.json", S(deep), 0, -1)
put("http.json", S(doc(pl_url="http://127.0.0.1:8080/dl/hibyos-payload-0.9.1.tar")), 0, 0)
put("upper_sha.json", S(doc(pl_sha256="AB" * 32)), 0, 0)
put("unicode.json", S(doc(changelog={"en": "x", "es": "Canción \U0001F3B5", "fr": "y"})), 0, 0, "es", "Canción \U0001F3B5")
put("list.json", S(doc(changelog={"en": ["First", "Second"], "es": "s", "fr": "f"})), 0, 0, "en", "• First  • Second")
put("fallback.json", S(doc()), 0, 0, "zh-Hans", "Fixed things.")
put("only_es.json", S(doc(changelog={"es": "solo"})), 0, 0, "ja", "solo")
put("restart_none.json", S(doc(restart="none")), 0, 0)
with open(os.path.join(OUT, "json", "expect.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(exp) + "\n")

# ------------------------------------------------------------------ tar
def hdr(name, size=0, typ=b"0", mode=0o644, prefix=b"", link=b"", magic=b"ustar\x0000", fix=True):
    h = bytearray(512)
    h[0:len(name)] = name
    h[100:108] = b"%07o\0" % mode
    h[108:116] = b"0000000\0"; h[116:124] = b"0000000\0"
    h[124:136] = b"%011o\0" % size
    h[136:148] = b"%011o\0" % 1700000000
    h[148:156] = b" " * 8
    h[156:157] = typ
    h[157:157 + len(link)] = link
    h[257:265] = magic
    h[265:269] = b"root"; h[297:301] = b"root"
    h[345:345 + len(prefix)] = prefix
    if fix:
        h[148:156] = b"%06o\0 " % sum(h)
    return bytes(h)


def entry(name, data=b"", **kw):
    pad = (512 - len(data) % 512) % 512
    return hdr(name, len(data), **kw) + data + b"\0" * pad


END = b"\0" * 1024
texp = []


def tput(name, data, rc):
    with open(os.path.join(OUT, "tar", name), "wb") as f:
        f.write(data)
    texp.append(f"{name} {rc}")


goodt = (entry(b"payload/", typ=b"5", mode=0o755) + entry(b"payload/VERSION", b"0.9.1\n") +
         entry(b"payload/bin/hibyshell", b"\x7fELF" + bytes(range(256)) * 9, mode=0o755) +
         entry(b"payload/a/b/c.txt", b"hello") + entry(b"payload/empty", b"") +
         entry(b"c.txt", b"x", prefix=b"payload/deep/er"))
tput("good.tar", goodt + END, 0)
tput("good_oneend.tar", goodt + b"\0" * 512, 0)
tput("traversal.tar", entry(b"payload/../evil", b"x") + END, -1)
tput("traversal2.tar", entry(b"payload/a/../../evil", b"x") + END, -1)
tput("absolute.tar", entry(b"/payload/x", b"x") + END, -1)
tput("dot.tar", entry(b"payload/./x", b"x") + END, -1)
tput("dslash.tar", entry(b"payload//x", b"x") + END, -1)
tput("outside.tar", entry(b"other/x", b"x") + END, -1)
tput("symlink.tar", entry(b"payload/l", typ=b"2", link=b"/etc/passwd") + END, -1)
tput("hardlink.tar", entry(b"payload/h", typ=b"1", link=b"payload/VERSION") + END, -1)
tput("device.tar", entry(b"payload/dev", typ=b"3") + END, -1)
tput("gnulong.tar", entry(b"././@LongLink", b"payload/" + b"x" * 120 + b"\0", typ=b"L") + END, -1)
tput("dup.tar", entry(b"payload/x", b"1") + entry(b"payload/x", b"2") + END, -1)
tput("badsum.tar", hdr(b"payload/x", 1, fix=False) + b"x" + b"\0" * 511 + END, -1)
tput("nomagic.tar", entry(b"payload/x", b"x", magic=b"\0" * 8) + END, -1)
tput("truncated.tar", (entry(b"payload/VERSION", b"0.9.1\n") + entry(b"payload/big", b"z" * 5000))[:512 * 3 + 100], -1)
tput("noend.tar", entry(b"payload/x", b"x"), -1)
tput("bigsize.tar", hdr(b"payload/x", 70 * 1024 * 1024) + END, -1)
tput("weird_chars.tar", entry(b"payload/a b", b"x") + END, -1)
tput("dir_with_size.tar", hdr(b"payload/d/", 5, typ=b"5") + END, -1)
tput("garbage_after_end.tar", goodt + END + entry(b"payload/late", b"x"), 0)   # stops at the first end
# pax header (python's PAX format for a long name): refused
import io  # noqa: E402
bio = io.BytesIO()
with tarfile.open(fileobj=bio, mode="w", format=tarfile.PAX_FORMAT) as t:
    ti = tarfile.TarInfo("payload/" + "n" * 150 + ".txt"); ti.size = 1
    t.addfile(ti, io.BytesIO(b"x"))
tput("pax.tar", bio.getvalue(), -1)
# what tools/ota_release.py writes (python USTAR, directories with "/")
bio = io.BytesIO()
with tarfile.open(fileobj=bio, mode="w", format=tarfile.USTAR_FORMAT) as t:
    for n, d in (("payload", None), ("payload/bin", None), ("payload/bin/x", b"abc"), ("payload/VERSION", b"1.0.0\n")):
        ti = tarfile.TarInfo(n)
        if d is None:
            ti.type = tarfile.DIRTYPE; t.addfile(ti)
        else:
            ti.size = len(d); t.addfile(ti, io.BytesIO(d))
tput("python_ustar.tar", bio.getvalue(), 0)
with open(os.path.join(OUT, "tar", "expect.txt"), "w") as f:
    f.write("\n".join(texp) + "\n")
print(f"fixtures in {OUT}: {len(lines)} vectors, {len(exp)} json, {len(texp)} tar")
