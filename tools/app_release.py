#!/usr/bin/env python3
"""Signed update description of a companion app release: byos-app-update.json (docs/APP_UPDATES.md).

  python tools/app_release.py android out/release/hibyos-install-1.2.0.apk --notes notes.json --key KEY
  python tools/app_release.py desktop out/release/ByOS.exe --version 0.9.2 --notes notes.json --key KEY
  python tools/app_release.py verify out/release/install-v1.2.0/byos-app-update.json [--file ASSET] [--pub HEX]

android  reads versionName and versionCode from android/hibyos-install/app/build.gradle.kts (or
         --version / --version-code), tag install-vX.Y.Z, asset = the APK's file name, min_os = 26.
desktop  needs --version (the version built into ByOS.exe), tag desktop-vX.Y.Z, asset ByOS.exe,
         min_os = 10.0.17134 (Windows 10 1803: tar.exe).

The file is written to out/release/<tag>/byos-app-update.json (or --out FILE). With --key it is
signed with that Ed25519 key file (tools/ota_sign.py format) and checked again; without --key it
keeps the zero signature and the command to sign it is printed. Then upload BOTH the asset and
byos-app-update.json to the GitHub release <tag> (never marked "latest": the latest release is
the ByOS firmware). The apps accept the file only if it is signed with a key built into them
(the player's OTA release key) and the downloaded asset matches the size and SHA-256 written here.

notes.json: {"en": "...", "es": "...", "fr": "..."} (English is required), or --notes-en/-es/-fr.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import ota_sign  # noqa: E402

GRADLE = os.path.join(ROOT, "android", "hibyos-install", "app", "build.gradle.kts")
APPS = {
    "android": {"id": "byos-install-android", "prefix": "install-v", "ext": ".apk", "min_os": "26"},
    "desktop": {"id": "byos-desktop-windows", "prefix": "desktop-v", "ext": ".exe", "min_os": "10.0.17134"},
}
INDEX = "byos-app-update.json"
VERSION_RE = re.compile(r"^\d{1,6}(\.\d{1,6}){1,3}$")
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
MAX_ASSET = 512 << 20


def gradle_version(path=GRADLE):
    with open(path, encoding="utf-8") as f:
        t = f.read()
    name = re.search(r'versionName\s*=\s*"([^"]+)"', t)
    code = re.search(r"versionCode\s*=\s*(\d+)", t)
    if not name or not code:
        raise SystemExit(f"ERROR: versionName/versionCode not found in {path}")
    return name.group(1), int(code.group(1))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def read_notes(a):
    notes = {}
    if a.notes:
        with open(a.notes, encoding="utf-8") as f:
            notes = json.load(f)
        if not isinstance(notes, dict):
            raise SystemExit("ERROR: the notes file must be a JSON object {\"en\": ..., \"es\": ..., \"fr\": ...}")
    for lang in ("en", "es", "fr"):
        v = getattr(a, f"notes_{lang}", None)
        if v:
            notes[lang] = v
    notes = {k: str(v).strip() for k, v in notes.items() if re.fullmatch(r"[a-z]{2}", k) and str(v).strip()}
    if "en" not in notes:
        raise SystemExit("ERROR: release notes in English are required (--notes FILE or --notes-en TEXT)")
    for lang in ("es", "fr"):
        if lang not in notes:
            print(f"warning: no release notes in '{lang}': the app shows the English ones", file=sys.stderr)
    for k, v in notes.items():
        if len(v) > 4000:
            raise SystemExit(f"ERROR: the '{k}' notes are longer than 4000 characters")
    return notes


def build_index(kind, asset_path, version, version_code=None, notes=None, min_os=None, published=None):
    """The index as text, with the zero signature (sign it with ota_sign.sign_bytes)."""
    app = APPS[kind]
    name = os.path.basename(asset_path)
    if not VERSION_RE.match(version):
        raise SystemExit(f"ERROR: '{version}' is not a plain version like 1.2.0")
    if not NAME_RE.match(name) or not name.endswith(app["ext"]):
        raise SystemExit(f"ERROR: '{name}' is not a {app['ext']} file name the apps accept")
    size = os.path.getsize(asset_path)
    if not 0 < size <= MAX_ASSET:
        raise SystemExit(f"ERROR: {name} has {size} bytes")
    if kind == "android" and name != f"hibyos-install-{version}.apk":
        print(f"warning: the APK is usually named hibyos-install-{version}.apk, not {name}", file=sys.stderr)
    m = {"format": 1, "product": "byos-app-update", "app": app["id"], "version": version}
    if kind == "android":
        if not version_code or version_code <= 0:
            raise SystemExit("ERROR: the Android index needs a versionCode")
        m["version_code"] = int(version_code)
    m.update({
        "tag": app["prefix"] + version, "asset": name, "size": size, "sha256": sha256_file(asset_path),
        "min_os": min_os or app["min_os"], "published": published or datetime.date.today().isoformat(),
        "notes": notes or {}, "signature": "ed25519:" + "0" * 128,
    })
    return json.dumps(m, indent=1, ensure_ascii=False) + "\n"


def check(data, asset_path=None, keys=None):
    """Signature (keys: the compiled-in release keys by default) and, with the asset, size and SHA-256."""
    keys = keys or ota_sign.compiled_pubkeys()
    if not ota_sign.verify_bytes(data, keys):
        return "signature INVALID (not made by " + ", ".join(k[:16] + "…" for k in keys) + ")"
    if asset_path:
        m = json.loads(data.decode("utf-8"))
        # the local copy may have another name (updates\ByOS-<version>.exe): only its contents count
        if os.path.getsize(asset_path) != m["size"] or sha256_file(asset_path) != m["sha256"]:
            return f"{asset_path} is not the file this index describes (size or SHA-256)"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for kind in APPS:
        p = sub.add_parser(kind)
        p.add_argument("asset")
        p.add_argument("--version")
        if kind == "android":
            p.add_argument("--version-code", type=int)
        p.add_argument("--min-os")
        p.add_argument("--notes")
        p.add_argument("--notes-en"); p.add_argument("--notes-es"); p.add_argument("--notes-fr")
        p.add_argument("--published", help="date (default: today)")
        p.add_argument("--key", help="Ed25519 key file (tools/ota_sign.py); without it the file is left unsigned")
        p.add_argument("--out")
    v = sub.add_parser("verify")
    v.add_argument("index")
    v.add_argument("--file", help="the asset, to check its size and SHA-256 too")
    v.add_argument("--pub", action="append", help="public key (64 hex); default: the keys compiled into the player")
    a = ap.parse_args()

    if a.cmd == "verify":
        with open(a.index, "rb") as f:
            data = f.read()
        problem = check(data, a.file, [k.lower() for k in a.pub] if a.pub else None)
        print(problem or "OK: signed" + (" and the file matches" if a.file else ""))
        sys.exit(1 if problem else 0)

    if not os.path.isfile(a.asset):
        raise SystemExit(f"ERROR: {a.asset} not found")
    if a.cmd == "android":
        gname, gcode = gradle_version()
        version = a.version or gname
        code = a.version_code or gcode
        if a.version and a.version != gname:
            print(f"warning: --version {a.version} differs from versionName {gname} in build.gradle.kts", file=sys.stderr)
    else:
        if not a.version:
            raise SystemExit("ERROR: --version is required for the desktop app (the version built into the exe)")
        version, code = a.version, None
    text = build_index(a.cmd, a.asset, version, code, read_notes(a), a.min_os, a.published)
    tag = APPS[a.cmd]["prefix"] + version
    out = a.out or os.path.join(ROOT, "out", "release", tag, INDEX)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    data = text.encode("utf-8")
    if a.key:
        seed = ota_sign.load_seed(a.key)
        data = ota_sign.sign_bytes(data, seed)
        pub = ota_sign.public_hex(seed)
        problem = check(data, a.asset, [pub])
        if problem:
            raise SystemExit("ERROR: " + problem)
        if pub not in ota_sign.compiled_pubkeys():
            print(f"warning: {pub[:16]}… is not a release key built into the player and the apps: "
                  "they will refuse this file (fine for tests)", file=sys.stderr)
    with open(out, "wb") as f:
        f.write(data)
    print(f"{out}\n  {APPS[a.cmd]['id']} {version} ({tag}), {os.path.basename(a.asset)}: "
          f"{os.path.getsize(a.asset)} bytes, sha256 {sha256_file(a.asset)}")
    if a.key:
        print("  signed; upload it with the asset to the release " + tag)
    else:
        print(f"  NOT signed yet: python tools/ota_sign.py sign --key KEY {out}")


if __name__ == "__main__":
    main()
