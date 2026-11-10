#!/usr/bin/env python3
"""Release signing key and signatures for HiByOS online updates (docs/OTA.md).

  python tools/ota_sign.py genkey --key PATH [--install]   new key OUTSIDE the repository;
                                                           --install writes its public key in
                                                           src/ota_net.inc.c (OTA_PUBKEY)
  python tools/ota_sign.py pubkey --key PATH               print the public key (64 hex)
  python tools/ota_sign.py install --key PATH [--slot 2]   write the public key in the source
  python tools/ota_sign.py sign --key PATH FILE.json       fill in the "signature" of the file
  python tools/ota_sign.py verify FILE.json [--pub HEX]    check it (default: compiled-in keys)

The key file holds the 32-byte Ed25519 seed as text ("hibyos-ota-ed25519-seed:<64 hex>").
It must never be committed: genkey refuses paths inside the repository. Keep a backup offline
(a lost key means devices can only be updated again by reflashing a build with a new key).

Signature format: the JSON has exactly one key "signature" whose value is "ed25519:<128 hex>";
the signed message is the whole file with those 128 digits replaced by '0'. The device does
exactly the same (src/ota_net.inc.c, ota_sig_check).

Backends: `cryptography`, else `pynacl`, else the pure-Python tools/ota_ed25519.py.
"""
import argparse
import os
import re
import secrets
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SRC = os.path.join(ROOT, "src", "ota_net.inc.c")
SIG_RE = re.compile(rb'"signature"\s*:\s*"ed25519:([0-9A-Fa-f]{128})"')
PREFIX = "hibyos-ota-ed25519-seed:"
PUB_RE = {1: re.compile(r'(#define OTA_PUBKEY_HEX ")([0-9A-Fa-f]{64})(" /\* OTA_PUBKEY \*/)'),
          2: re.compile(r'(#define OTA_PUBKEY2_HEX ")([0-9A-Fa-f]{64}|)(" /\* OTA_PUBKEY2 \*/)')}


# ------------------------------------------------------------------ backends
def _backend(name=None):
    order = [name] if name else ["cryptography", "nacl", "pure"]
    for b in order:
        try:
            if b == "cryptography":
                from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
                from cryptography.hazmat.primitives import serialization as ser

                def pub(seed):
                    return Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)

                def sign(seed, msg):
                    return Ed25519PrivateKey.from_private_bytes(seed).sign(msg)

                def verify(pk, msg, sig):
                    try:
                        Ed25519PublicKey.from_public_bytes(pk).verify(sig, msg)
                        return True
                    except Exception:
                        return False
                return b, pub, sign, verify
            if b == "nacl":
                import nacl.signing
                import nacl.exceptions

                def pub(seed):
                    return bytes(nacl.signing.SigningKey(seed).verify_key)

                def sign(seed, msg):
                    return nacl.signing.SigningKey(seed).sign(msg).signature

                def verify(pk, msg, sig):
                    try:
                        nacl.signing.VerifyKey(pk).verify(msg, sig)
                        return True
                    except nacl.exceptions.BadSignatureError:
                        return False
                return b, pub, sign, verify
            if b == "pure":
                sys.path.insert(0, HERE)
                import ota_ed25519 as E
                return b, E.public_key, E.sign, E.verify
        except ImportError:
            continue
    raise SystemExit(f"ERROR: no Ed25519 backend {name or ''}")


BACKEND = os.environ.get("HIBYOS_OTA_BACKEND") or None


def backend():
    return _backend(BACKEND)


# ------------------------------------------------------------------ keys
def inside_repo(path):
    a = os.path.normcase(os.path.realpath(os.path.abspath(path)))
    r = os.path.normcase(os.path.realpath(ROOT))
    try:
        return os.path.commonpath([a, r]) == r
    except ValueError:
        return False


def load_seed(path):
    with open(path, encoding="ascii") as f:
        t = f.read().strip()
    if not t.startswith(PREFIX) or len(t) != len(PREFIX) + 64:
        raise SystemExit(f"ERROR: {path} is not a HiByOS OTA key file")
    return bytes.fromhex(t[len(PREFIX):])


def public_hex(seed):
    return backend()[1](seed).hex()


def compiled_pubkeys(src=SRC):
    with open(src, encoding="utf-8") as f:
        t = f.read()
    keys = []
    for slot in (1, 2):
        m = PUB_RE[slot].search(t)
        if m and m.group(2) and set(m.group(2)) != {"0"}:
            keys.append(m.group(2).lower())
    return keys


def install_pub(pubhex, slot=1, src=SRC):
    with open(src, encoding="utf-8", newline="") as f:
        t = f.read()
    t2, n = PUB_RE[slot].subn(lambda m: m.group(1) + pubhex + m.group(3), t)
    if n != 1:
        raise SystemExit(f"ERROR: OTA_PUBKEY{'' if slot == 1 else '2'} line not found in {src}")
    with open(src, "w", encoding="utf-8", newline="") as f:
        f.write(t2)


# ------------------------------------------------------------------ signatures
def _locate(data):
    if data.count(b'"signature"') != 1:
        raise ValueError('the JSON must contain exactly one "signature" key')
    m = SIG_RE.search(data)
    if not m:
        raise ValueError('"signature" must be "ed25519:" + 128 hex digits')
    return m.start(1), m.end(1)


def signed_message(data):
    a, b = _locate(data)
    return data[:a] + b"0" * 128 + data[b:]


def sign_bytes(data, seed):
    a, b = _locate(data)
    msg = data[:a] + b"0" * 128 + data[b:]
    sig = backend()[2](seed, msg)
    return data[:a] + sig.hex().encode() + data[b:]


def verify_bytes(data, pubkeys):
    try:
        a, b = _locate(data)
    except ValueError:
        return False
    sig = bytes.fromhex(data[a:b].decode())
    msg = data[:a] + b"0" * 128 + data[b:]
    ver = backend()[3]
    return any(ver(bytes.fromhex(k), msg, sig) for k in pubkeys)


# ------------------------------------------------------------------ cli
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("genkey"); g.add_argument("--key", required=True); g.add_argument("--install", action="store_true")
    g.add_argument("--force", action="store_true", help="overwrite an existing key file (DANGER)")
    g.add_argument("--allow-in-repo", action="store_true", help=argparse.SUPPRESS)   # tests only
    pk = sub.add_parser("pubkey"); pk.add_argument("--key", required=True)
    ins = sub.add_parser("install"); ins.add_argument("--key", required=True); ins.add_argument("--slot", type=int, choices=(1, 2), default=1)
    s = sub.add_parser("sign"); s.add_argument("--key", required=True); s.add_argument("file")
    v = sub.add_parser("verify"); v.add_argument("file"); v.add_argument("--pub", action="append")
    a = ap.parse_args()

    if a.cmd == "genkey":
        if inside_repo(a.key) and not a.allow_in_repo:
            raise SystemExit(f"ERROR: {a.key} is inside the repository; keep the private key elsewhere "
                             "(e.g. %USERPROFILE%\\.hibyos\\ota_ed25519.key)")
        if os.path.exists(a.key) and not a.force:
            raise SystemExit(f"ERROR: {a.key} already exists (a new key would lock out devices with the old one)")
        os.makedirs(os.path.dirname(os.path.abspath(a.key)), exist_ok=True)
        seed = secrets.token_bytes(32)
        fd = os.open(a.key, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as f:
            f.write(PREFIX + seed.hex() + "\n")
        try:
            os.chmod(a.key, 0o600)
        except OSError:
            pass
        pub = public_hex(seed)
        print(f"key: {os.path.abspath(a.key)}  (back it up offline; never commit it)")
        print(f"public key: {pub}")
        if a.install:
            install_pub(pub)
            print(f"written to {SRC} (OTA_PUBKEY); rebuild hibyshell")
        else:
            print(f"next: python tools/ota_sign.py install --key {a.key}")
    elif a.cmd == "pubkey":
        print(public_hex(load_seed(a.key)))
    elif a.cmd == "install":
        pub = public_hex(load_seed(a.key))
        install_pub(pub, a.slot)
        print(f"OTA_PUBKEY{'' if a.slot == 1 else '2'} = {pub} in {SRC}; rebuild hibyshell")
    elif a.cmd == "sign":
        with open(a.file, "rb") as f:
            data = f.read()
        out = sign_bytes(data, load_seed(a.key))
        with open(a.file, "wb") as f:
            f.write(out)
        print(f"signed {a.file} with {public_hex(load_seed(a.key))}")
    elif a.cmd == "verify":
        keys = [k.lower() for k in a.pub] if a.pub else compiled_pubkeys()
        if not keys:
            raise SystemExit("ERROR: no public key (src/ota_net.inc.c has none; use --pub)")
        with open(a.file, "rb") as f:
            ok = verify_bytes(f.read(), keys)
        print("signature OK" if ok else "signature INVALID")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
