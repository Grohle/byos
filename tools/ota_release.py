#!/usr/bin/env python3
"""Builds a HiByOS online-update release (docs/OTA.md): the payload tar, the signed
hibyos-update.json and the release notes, and prints the `gh release create` command
(it does not run it).

  python tools/ota_release.py --key PATH --changelog notes.json [options]

  --key PATH            release signing key (tools/ota_sign.py genkey); never in the repo
  --changelog FILE      JSON {"en": "...", "es": "...", "fr": "...", ...}; each value a string or
                        a list of strings (bullets). en, es and fr are required; any other
                        language code of the shell (de, it, ja, ko, pl, ru, zh-Hans, th,
                        zh-Hant, uk) is optional (the device falls back to en)
  --version X.Y.Z       default: the one in overlay/usr/bin/hibymod
  --build               run tools/make_payload.py first (otherwise out/sd/HiByOS/payload is used)
  --payload DIR         payload folder (default out/sd/HiByOS/payload)
  --prev-payload DIR    payload of the previous release: computes "restart" (shell/reboot) like
                        the device does; without it "reboot" is announced (the device still
                        computes the real need from the manifests)
  --min-version V       oldest installed HiByOS that may take this update (default 0.6.0)
  --min-firmware V      oldest firmware (rootfs) it works on (default: the current firmware
                        version, i.e. --version, if --needs-firmware; else 0.6.0)
  --needs-firmware      this release changes the rootfs: devices are told to use the Patcher
  --repo OWNER/NAME     default Grohle/hibyos
  --url-base URL        where the tar will be (default the GitHub release download URL)
  --out DIR             default out/release/vX.Y.Z
  --date YYYY-MM-DD     default today (UTC)
  --allow-key-mismatch  sign even if the key is not the one compiled into src/ota_net.inc.c
                        (tests only: devices would refuse the release)

Output (out/release/vX.Y.Z/): hibyos-payload-X.Y.Z.tar, hibyos-update.json, notes.md
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import ota_sign  # noqa: E402

LANGS = ("es", "en", "fr", "de", "it", "ja", "ko", "pl", "ru", "zh-Hans", "th", "zh-Hant", "uk")
PATH_OK = re.compile(r"^[A-Za-z0-9._+@/-]+$")
MTIME = 1700000000            # fixed: the same payload gives the same tar (reproducible)
MAX_PAYLOAD = 64 * 1024 * 1024


def version_default():
    with open(os.path.join(ROOT, "overlay", "usr", "bin", "hibymod"), encoding="utf-8") as f:
        m = re.search(r'echo "HiByOS ([0-9][0-9A-Za-z.\-]*)"', f.read())
    return m.group(1) if m else "0.0.0"


def ver_ok(v):
    return bool(re.fullmatch(r"[0-9]+(\.[0-9]+)+(-[A-Za-z0-9.]+)?", v)) and len(v) <= 23


def md5(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def read_manifest(pl):
    m = {}
    with open(os.path.join(pl, "MANIFEST.md5"), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                s, rel = line.rstrip("\n").split("  ", 1)
                m[rel] = s
    return m


def check_payload(pl, version):
    vf = os.path.join(pl, "VERSION")
    if not os.path.isfile(vf):
        sys.exit(f"ERROR: {pl} has no VERSION (python tools/make_payload.py, or --build)")
    with open(vf, encoding="utf-8") as f:
        v = f.readline().strip()
    if v != version:
        sys.exit(f"ERROR: the payload is version {v}, not {version} (make_payload.py --version {version})")
    man = read_manifest(pl)
    files = set()
    for dp, _dn, fn in os.walk(pl):
        for n in fn:
            rel = os.path.relpath(os.path.join(dp, n), pl).replace("\\", "/")
            if rel != "MANIFEST.md5":
                files.add(rel)
    if files != set(man):
        sys.exit(f"ERROR: MANIFEST.md5 does not match the folder (extra: {sorted(files - set(man))[:5]}, "
                 f"missing: {sorted(set(man) - files)[:5]})")
    for rel, s in man.items():
        if not PATH_OK.match(rel) or ".." in rel.split("/"):
            sys.exit(f"ERROR: path not allowed in the payload: {rel!r}")
        if md5(os.path.join(pl, rel)) != s:
            sys.exit(f"ERROR: md5 of {rel} does not match MANIFEST.md5")
    return man


def path_restart(p):   # same rules as ota_path_restart() in src/ota_net.inc.c
    if p in ("VERSION", "MANIFEST.md5") or p.startswith("Pictures/"):
        return 0
    if p in ("sys/hibyos_install.sh", "sys/MANIFEST.sha256"):
        return 0
    if p == "bin/hibymod_disp.so":
        return 2
    if p.startswith("bin/") or p.startswith("shell/"):
        return 1
    return 2


def restart_kind(old, new):
    k = 0
    for p, s in new.items():
        if old.get(p) != s:
            k = max(k, path_restart(p))
    for p in old:
        if p not in new:
            k = max(k, path_restart(p))
    return k


def build_tar(pl, out):
    """ustar, sorted, root:root, fixed mtime; entries under payload/ (what the device accepts)."""
    entries = []
    for dp, dn, fn in os.walk(pl):
        dn.sort()
        rel_d = os.path.relpath(dp, pl).replace("\\", "/")
        entries.append(("payload" if rel_d == "." else "payload/" + rel_d, None))
        for n in sorted(fn):
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, pl).replace("\\", "/")
            entries.append(("payload/" + rel, p))
    entries.sort(key=lambda e: e[0])
    tmp = out + ".tmp"
    with tarfile.open(tmp, "w", format=tarfile.USTAR_FORMAT) as t:
        for name, src in entries:
            if not PATH_OK.match(name):
                sys.exit(f"ERROR: name not allowed in the tar: {name!r}")
            ti = tarfile.TarInfo(name)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            ti.mtime = MTIME
            if src is None:
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                t.addfile(ti)
                continue
            rel = name[len("payload/"):]
            ti.mode = 0o755 if rel.startswith("bin/") and not rel.endswith((".so", ".pem")) else 0o644
            ti.size = os.path.getsize(src)
            with open(src, "rb") as f:
                t.addfile(ti, f)
    os.replace(tmp, out)


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def load_changelog(path):
    with open(path, encoding="utf-8") as f:
        c = json.load(f)
    if not isinstance(c, dict):
        sys.exit("ERROR: the changelog must be a JSON object {language: text}")
    for k in ("en", "es", "fr"):
        if k not in c:
            sys.exit(f"ERROR: the changelog needs at least en, es and fr (missing {k})")
    out = {}
    for k, v in c.items():
        if k not in LANGS:
            sys.exit(f"ERROR: unknown language {k!r} in the changelog (use {', '.join(LANGS)})")
        if isinstance(v, str):
            v = v.strip()
        elif isinstance(v, list) and all(isinstance(x, str) for x in v):
            v = [x.strip() for x in v if x.strip()]
        else:
            sys.exit(f"ERROR: changelog[{k}] must be a string or a list of strings")
        if not v:
            sys.exit(f"ERROR: changelog[{k}] is empty")
        out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", required=True)
    ap.add_argument("--changelog", required=True)
    ap.add_argument("--version", default=version_default())
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--payload", default=os.path.join(ROOT, "out", "sd", "HiByOS", "payload"))
    ap.add_argument("--prev-payload")
    ap.add_argument("--min-version", default="0.6.0")
    ap.add_argument("--min-firmware")
    ap.add_argument("--needs-firmware", action="store_true")
    ap.add_argument("--repo", default="Grohle/hibyos")
    ap.add_argument("--url-base")
    ap.add_argument("--out")
    ap.add_argument("--date", default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"))
    ap.add_argument("--allow-key-mismatch", action="store_true")
    a = ap.parse_args()

    v = a.version
    for name, x in (("--version", v), ("--min-version", a.min_version), ("--min-firmware", a.min_firmware)):
        if x is not None and not ver_ok(x):
            sys.exit(f"ERROR: {name} {x!r} is not a version like 1.2.3 or 1.2.3-beta.1")
    min_fw = a.min_firmware or (v if a.needs_firmware else "0.6.0")
    if ota_sign.inside_repo(a.key):
        sys.exit("ERROR: the signing key is inside the repository: move it out and never commit it")
    seed = ota_sign.load_seed(a.key)
    pub = ota_sign.public_hex(seed)
    compiled = ota_sign.compiled_pubkeys()
    if pub not in compiled and not a.allow_key_mismatch:
        sys.exit(f"ERROR: the key {pub[:16]}... is not compiled into src/ota_net.inc.c "
                 f"({'no key there' if not compiled else ', '.join(k[:16] + '...' for k in compiled)}): "
                 "devices would refuse this release (python tools/ota_sign.py install --key ...)")
    changelog = load_changelog(a.changelog)

    if a.build:
        subprocess.run([sys.executable, os.path.join(HERE, "make_payload.py"), "--version", v], check=True)
    pl = os.path.abspath(a.payload)
    man = check_payload(pl, v)
    restart = "reboot"
    if a.prev_payload:
        k = restart_kind(read_manifest(a.prev_payload), man)
        restart = ("none", "shell", "reboot")[k]

    out = os.path.abspath(a.out or os.path.join(ROOT, "out", "release", "v" + v))
    os.makedirs(out, exist_ok=True)
    tname = f"hibyos-payload-{v}.tar"
    tpath = os.path.join(out, tname)
    build_tar(pl, tpath)
    size = os.path.getsize(tpath)
    if size > MAX_PAYLOAD:
        sys.exit(f"ERROR: the payload tar is {size} bytes (the device accepts {MAX_PAYLOAD})")
    tag = "v" + v
    base = a.url_base or f"https://github.com/{a.repo}/releases/download/{tag}/"
    if not base.endswith("/"):
        base += "/"
    doc = {
        "format": 1,
        "product": "hibyos",
        "version": v,
        "date": a.date,
        "min_version": a.min_version,
        "min_firmware": min_fw,
        "needs_firmware": bool(a.needs_firmware),
        "firmware_url": f"https://github.com/{a.repo}/releases/tag/{tag}",
        "restart": restart,
        "payload": {"name": tname, "url": base + tname, "size": size, "sha256": sha256(tpath), "compression": "none"},
        "changelog": changelog,
        "signature": "ed25519:" + "0" * 128,
    }
    data = (json.dumps(doc, ensure_ascii=False, indent=1) + "\n").encode("utf-8")
    if len(data) > 60 * 1024:
        sys.exit("ERROR: hibyos-update.json is too big (device limit 64 KB): shorten the changelog")
    data = ota_sign.sign_bytes(data, seed)
    if not ota_sign.verify_bytes(data, [pub]):
        sys.exit("ERROR: the signature does not verify (backend problem)")
    jpath = os.path.join(out, "hibyos-update.json")
    with open(jpath, "wb") as f:
        f.write(data)

    notes = os.path.join(out, "notes.md")
    with open(notes, "w", encoding="utf-8", newline="\n") as f:
        en = changelog["en"]
        f.write(f"# HiByOS {v}\n\n")
        f.write(("\n".join(f"- {x}" for x in en) if isinstance(en, list) else en) + "\n\n")
        if a.needs_firmware:
            f.write("**This release changes the firmware: run the HiByOS Patcher on your official "
                    "firmware and install the resulting r3ii_2025.upt from the SD.**\n\n")
        f.write("Installed HiByOS devices update from *Settings > General > Software Update*.\n")
        f.write(f"Payload sha256: `{doc['payload']['sha256']}` ({size} bytes). Signed with `{pub[:16]}…`.\n")

    print(f"HiByOS {v}: {out}")
    print(f"  {tname}  {size} bytes  sha256 {doc['payload']['sha256']}")
    print(f"  hibyos-update.json  restart={restart}  needs_firmware={doc['needs_firmware']}  key {pub[:16]}...")
    print()
    print("Publish (not run here). The release must be the repository's *latest* one, because devices read")
    print(f"https://github.com/{a.repo}/releases/latest/download/hibyos-update.json :")
    print()
    print(f'  gh release create {tag} "{tpath}" "{jpath}" --repo {a.repo} --title "HiByOS {v}" '
          f'--notes-file "{notes}" --latest')


if __name__ == "__main__":
    main()
