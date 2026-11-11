#!/usr/bin/env python3
"""Builds the ByOS bundle that the ByOS Install app carries (and can download).

  python android/hibyos-install/tools/export_bundle.py            (from the repository root)

The app has no list of files or patches of its own: everything comes from this bundle, and
this script takes everything from the same Python definitions the desktop patcher uses
(tools/build.py, tools/hibyos_patcher.py and tools/vendor_edits.json), so both stay in
lockstep.

Output (default app/src/main/assets/hibyos-bundle.zip), a ZIP with:
  bundle.json     supported firmware (hashes only), limits and the ordered list of steps;
                  the changes to HiBy's own files are stored as edits (what to keep of the
                  official file, by offset, and the text written for ByOS), never as files
  overlay/...     the ByOS files for the root filesystem, already normalised by
                  build.overlay_file() (line endings) with the mode they get when new
  sd/ByOS/...   the files for the SD card (tools/make_payload.py, or --sd FOLDER)
  notices/...     licence texts (LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/*) when present

It contains no HiBy firmware. The ZIP is reproducible: same inputs, same bytes.
It also writes build/hibyos-install-bundle.json, the small index file to publish with a
release so that the app can find and verify a newer bundle (see README, "Publishing a new
bundle").
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import types
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(APP, "..", ".."))
FORMAT = 1

# What we know about each supported official firmware, by MD5 (the MD5 itself comes from
# tools/hibyos_patcher.py). "sources" are HiBy's own download links, tried in order by the
# app; "page" is where HiBy publishes the firmware, opened when the direct download fails.
KNOWN_FIRMWARE = {
    "0d16d09c1458dd2f2cdaf38d0047917a": {
        "label": "HiBy R3 II (2025), official firmware v1.3 of 2 March 2026",
        "version": "1.3",
        # what the stock "About" screen shows on its "BuildTime:" line (usr/bin/hiby_player of
        # that firmware: the format "BuildTime: %s %s" and the date "Mar  2 2026")
        "build": "Mar 2 2026",
        "size": 46759936,
        "sha256": "764f0c6724675d4c0009c065576fd8a5d21564396f531a1440f261d46dd8b81a",
        "sources": [
            "https://drive.usercontent.google.com/download?id=1cBNQby-X9kFE5uTcbjNEDHSQZAgFIvyF&export=download",
        ],
        "page": "https://store.hiby.com/apps/help-center"
                "#hc-r3-ii-2025-firmware-v13-update-not-available-in-r3-ii-2025-version",
        "folder": "https://drive.google.com/drive/folders/1vkl5niBBi8cPC2rIGgQKYu_vaxgLEECy",
    },
}

# Patch definitions of tools/build.py this script knows how to export. Anything else that
# looks like a definition stops the export, so that a new kind of change cannot be silently
# left out of the app.
KNOWN_DEFS = {"TEXT_PATCHES", "BIN_PATCHES", "OVERLAY_MAY_REPLACE"}
DEF_PATTERN = re.compile(r"^[A-Z][A-Z0-9_]*(PATCHES|EDITS|REMOVE|REMOVALS|DELETE|CHMOD|MODES|REPLACE)$")
# Files whose content decides what the bundle does (hashed into "definitions_sha256").
DEFINITION_FILES = ("build.py", "hibyos_patcher.py", "vendor_edits.py", "vendor_edits.json",
                    "squashfs.py", "lzo1x.py")


def die(msg):
    sys.exit("ERROR: " + msg)


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def load_definitions(repo):
    tools = os.path.join(repo, "tools")
    sys.path.insert(0, tools)
    try:
        import pycdlib  # noqa: F401
    except ImportError:                       # only needed to read/write .upt files, not here
        sys.modules["pycdlib"] = types.ModuleType("pycdlib")
    import build
    import hibyos_patcher
    import vendor_edits
    unknown = [n for n in dir(build) if DEF_PATTERN.match(n) and n not in KNOWN_DEFS]
    unknown += [n for n in dir(hibyos_patcher) if DEF_PATTERN.match(n) and n not in KNOWN_DEFS]
    if unknown:
        die("tools/build.py or tools/hibyos_patcher.py has definitions this script does not know: "
            + ", ".join(sorted(set(unknown))) + ". Teach export_bundle.py (and the app) about them.")
    return build, hibyos_patcher, vendor_edits


def edit_steps(vendor_edits):
    """tools/vendor_edits.json as "edit" steps: only what the firmware builders use (the older
    versions kept under "previous" are for the installer on the player, not for patching)."""
    steps = []
    for e in vendor_edits.load():
        ops = []
        for op in e["ops"]:
            (kind, arg), = op.items()
            if kind == "copy":
                off, n = arg
                if not (isinstance(off, int) and isinstance(n, int) and off >= 0 and n > 0):
                    die(f"tools/vendor_edits.json: {e['path']}: bad copy {arg!r}")
                ops.append({"copy": [off, n]})
            elif kind == "add":
                if not all(isinstance(x, str) for x in arg):
                    die(f"tools/vendor_edits.json: {e['path']}: bad add")
                ops.append({"add": list(arg)})
            else:
                die(f"tools/vendor_edits.json: {e['path']}: operation {kind!r} is not known to "
                    "export_bundle.py. Teach it (and the app) about it.")
        unknown = set(e) - {"path", "stock", "strip_cr", "result", "ops", "previous"}
        if unknown:
            die(f"tools/vendor_edits.json: {e['path']}: fields this script does not know: "
                + ", ".join(sorted(unknown)))
        steps.append({"op": "edit", "path": e["path"],
                      "stock": {"size": e["stock"]["size"], "sha256": e["stock"]["sha256"]},
                      "strip_cr": bool(e.get("strip_cr", False)),
                      "result": {"size": e["result"]["size"], "sha256": e["result"]["sha256"]},
                      "ops": ops})
    return steps


def steps_from(build, vendor_edits):
    """Same order as patch() in tools/hibyos_patcher.py."""
    steps = [{"op": "normalize"},
             {"op": "overlay", "may_replace": [x.lower() for x in build.OVERLAY_MAY_REPLACE]}]
    steps += edit_steps(vendor_edits)
    steps.append({"op": "build_id", "path": build.BUILD_ID_PATH})
    for rel, old, new in build.TEXT_PATCHES:
        steps.append({"op": "text", "path": rel, "old": old, "new": new})
    for rel, md5, edits in build.BIN_PATCHES:
        steps.append({"op": "bin", "path": rel, "md5": md5,
                      "edits": [{"off": off, "old": old, "new": new} for off, old, new in edits]})
    return steps


def overlay_files(build, overlay):
    """(path, data, mode) in the order of apply_overlay() in tools/hibyos_patcher.py."""
    out = []
    for d, dirs, names in os.walk(overlay):
        dirs.sort()
        for name in sorted(names):
            src = os.path.join(d, name)
            rel = os.path.relpath(src, overlay)
            with open(src, "rb") as f:
                data, mode = build.overlay_file(rel, f.read())
            out.append((rel.replace(os.sep, "/"), data, mode))
    return out


def sd_files(repo, sd, work):
    """(path below the SD card root, data) of the ByOS folder."""
    if sd is None:
        os.makedirs(work, exist_ok=True)
        r = subprocess.run([sys.executable, os.path.join(repo, "tools", "make_payload.py"), "--out", work],
                           capture_output=True, text=True)
        sys.stdout.write("".join("   " + l + "\n" for l in r.stdout.splitlines()))
        if r.returncode != 0:
            die("tools/make_payload.py failed:\n" + r.stderr + r.stdout +
                "\nBuild the SD-card files first or pass --sd <folder that contains ByOS/>.")
        sd = work
    top = os.path.join(sd, "ByOS")
    if not os.path.isfile(os.path.join(top, "payload", "VERSION")):
        die(f"{top} is not a ByOS SD-card folder (payload/VERSION is missing)")
    out = []
    for d, dirs, names in os.walk(top):
        dirs.sort()
        for name in sorted(names):
            p = os.path.join(d, name)
            with open(p, "rb") as f:
                out.append((os.path.relpath(p, sd).replace(os.sep, "/"), f.read()))
    return out


def notice_files(repo):
    """Licence texts for the "Open-source licences" screen: (path, title, data)."""
    out = []
    for name, title in (("LICENSE", "ByOS licence"), ("THIRD_PARTY_NOTICES.md", "Third-party notices")):
        p = os.path.join(repo, name)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                out.append((name, title, f.read()))
    d = os.path.join(repo, "LICENSES")
    if os.path.isdir(d):
        for name in sorted(os.listdir(d)):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    out.append(("LICENSES/" + name, os.path.splitext(name)[0], f.read()))
    return out


def git_state(repo, paths):
    def git(*a):
        r = subprocess.run(["git", "-C", repo, *a], capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    head = git("rev-parse", "--short", "HEAD") or "unknown"
    dirty = bool(git("status", "--porcelain", "--", *paths))
    return head + ("+modified" if dirty else "")


RELEASES = "https://github.com/Grohle/hibyos/releases"
INDEX_NAME = "hibyos-install-bundle.json"


def release_keys(repo):
    """The Ed25519 public keys of the ByOS releases (src/ota_net.inc.c), without empty slots."""
    path = os.path.join(repo, "src", "ota_net.inc.c")
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    keys = re.findall(r'#define OTA_PUBKEY2?_HEX "([0-9A-Fa-f]{64})"', text)
    return [k.lower() for k in keys if set(k) != {"0"}]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default=REPO, help="ByOS repository (default: the one this script is in)")
    ap.add_argument("--overlay", help="overlay folder (default: <repo>/overlay)")
    ap.add_argument("--sd", help="folder that contains ByOS/ for the SD card (default: run tools/make_payload.py)")
    ap.add_argument("--out", default=os.path.join(APP, "app", "src", "main", "assets", "hibyos-bundle.zip"))
    ap.add_argument("--work", default=os.path.join(APP, "build", "bundle-work"))
    ap.add_argument("--index", default=os.path.join(APP, "build", "hibyos-install-bundle.json"),
                    help="where to write the index file for releases")
    ap.add_argument("--url", help="download URL to write in the index file "
                                  "(default: the GitHub release asset for this version)")
    a = ap.parse_args()
    repo = os.path.abspath(a.repo)
    overlay = os.path.abspath(a.overlay or os.path.join(repo, "overlay"))

    build, patcher, vendor_edits = load_definitions(repo)
    version = patcher.hibyos_version(overlay)
    md5 = patcher.SUPPORTED_MD5
    known = KNOWN_FIRMWARE.get(md5)
    if known is None:
        die(f"tools/hibyos_patcher.py now supports firmware {md5}, which this script does not know. "
            "Add its label, size, SHA-256 and HiBy's download link to KNOWN_FIRMWARE.")

    ov = overlay_files(build, overlay)
    steps = steps_from(build, vendor_edits)
    edited = {s["path"] for s in steps if s["op"] == "edit"}
    clash = sorted(p for p, _d, _m in ov if p in edited)
    if clash:
        die("these overlay files are also built by tools/vendor_edits.json: " + ", ".join(clash))
    sd = sd_files(repo, a.sd, a.work)
    notices = notice_files(repo)
    sd_version = next((d.decode().strip() for p, d in sd if p == "ByOS/payload/VERSION"), "")
    if sd_version != version:
        die(f"the SD-card files are version {sd_version!r} but the overlay is {version!r}")

    defs = hashlib.sha256()
    for name in DEFINITION_FILES:
        with open(os.path.join(repo, "tools", name), "rb") as f:
            defs.update(f.read().replace(b"\r\n", b"\n"))

    meta = {
        "format": FORMAT,
        "hibyos_version": version,
        "generated_from": git_state(repo, ["tools", "overlay", "theme", "shell", "src", "LICENSES"]),
        "definitions_sha256": defs.hexdigest(),
        "firmware": {"label": known["label"], "version": known["version"], "file": patcher.OUT_NAME,
                     "md5": md5, "sha256": known["sha256"], "size": known["size"],
                     "sources": known["sources"], "page": known["page"], "folder": known["folder"],
                     "build": known.get("build")},
        "chunk": build.CHUNK,
        "align": build.ALIGN,
        "rootfs_limit": build.ROOTFS_LIMIT,
        "out_name": patcher.OUT_NAME,
        "steps": steps,
        "overlay": [{"path": p, "mode": mode, "size": len(d), "sha256": sha256(d)} for p, d, mode in ov],
        "sd": [{"path": p, "size": len(d), "sha256": sha256(d)} for p, d in sd],
        "notices": [{"path": p, "title": t} for p, t, _d in notices],
        # the player's own Wi-Fi transfer (etc/thttpd.conf and usr/share/web/pages/index-EN.js of the firmware)
        "transfer": {"port": 4399, "home": "/data/mnt/sd_0/"},
        # where newer bundles are announced, and the release keys that may sign that announcement
        "updates": {"index": RELEASES + "/latest/download/" + INDEX_NAME,
                    "prefix": RELEASES + "/download/",
                    "keys": release_keys(repo)},
    }

    entries = [("bundle.json", (json.dumps(meta, indent=1, ensure_ascii=False) + "\n").encode())]
    entries += [("overlay/" + p, d) for p, d, _m in ov]
    entries += [("sd/" + p, d) for p, d in sd]
    entries += [("notices/" + p, d) for p, _t, d in notices]
    names = [n for n, _ in entries]
    if len(set(names)) != len(names):
        die("duplicate names in the bundle")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tmp = a.out + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))   # fixed date: reproducible
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
    os.replace(tmp, a.out)

    with open(a.out, "rb") as f:
        blob = f.read()
    url = a.url or f"https://github.com/Grohle/hibyos/releases/download/v{version}/hibyos-install-bundle-{version}.zip"
    # Signed like hibyos-update.json (docs/OTA.md): fill in the signature with
    #   python tools/ota_sign.py sign --key <release key> <index file>
    index = {"format": FORMAT, "product": "hibyos-install-bundle", "hibyos_version": version, "url": url,
             "size": len(blob), "sha256": sha256(blob), "signature": "ed25519:" + "0" * 128}
    os.makedirs(os.path.dirname(os.path.abspath(a.index)), exist_ok=True)
    with open(a.index, "w", newline="\n", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
        f.write("\n")
    release_copy = os.path.join(os.path.dirname(os.path.abspath(a.index)), f"hibyos-install-bundle-{version}.zip")
    with open(release_copy, "wb") as f:
        f.write(blob)

    print(f"ByOS {version} bundle: {a.out}")
    print(f"   from {meta['generated_from']}, definitions {meta['definitions_sha256'][:16]}")
    print(f"   firmware: {known['label']} (md5 {md5})")
    print(f"   steps: {len(meta['steps'])} ({len(edited)} edits of official files)   overlay: {len(ov)} files {sum(len(d) for _p, d, _m in ov)} bytes"
          f"   sd: {len(sd)} files {sum(len(d) for _p, d in sd)} bytes   notices: {len(notices)}")
    print(f"   size {len(blob)} bytes, sha256 {index['sha256']}")
    print(f"   release keys: {len(meta['updates']['keys'])}")
    print(f"   for releases: {release_copy}")
    print(f"                 {a.index}  (unsigned: sign it with tools/ota_sign.py before publishing)")


if __name__ == "__main__":
    main()
