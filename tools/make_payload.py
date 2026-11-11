#!/usr/bin/env python3
"""Builds the ByOS/ folder for the SD card of a public release (see docs/INSTALLER.md).

  python tools/make_payload.py [--version 0.6.0] [--out out/sd]

Result: out/sd/ByOS/payload/
  VERSION        payload version (default: the one in overlay/usr/bin/hibymod)
  MANIFEST.md5   md5 of every file (the installer verifies it before touching anything)
  bin/           hibyshell hibywall hibymix hibytag hibymod_disp.so ...  (from bin/, built in WSL)
  shell/         .ov layers, icons/*.ov, wall.raw, home.sig             (from shell/, tools/shell_assets.py)
  theme/         OUR files of layout/theme3 and litegui/theme3 (paths relative to /usr/resource):
                 our own artwork. Nothing from HiBy: the installer copies the official theme3
                 from the player itself and puts these on top.
  mirror/        "mirror" home screen: transparent icons (ours). Applied once hibyshell has
                 generated launcher/mirror.png.
  derive/        how to build, ON THE PLAYER, the files that are HiBy's own layouts with our
                 changes (home screen, status bar, About, volume dialog): INDEX + a list of
                 operations per file. Only our changes are stored, never HiBy's text; the
                 installer applies them to the copy that is already in the player's firmware.
  Pictures/      the 9 wallpapers (theme/wallpapers.py); copied to /Pictures of the SD if missing.
  sys/           newer versions of root filesystem scripts (hibyos_install.sh...) + MANIFEST.sha256:
                 the script in the firmware only uses them if the sha256 matches (docs/OTA.md).
  licenses/      THIRD_PARTY_NOTICES.md and the licence texts of the third-party components
                 inside bin/ (tools/notices.py).

Nothing from the official firmware is redistributed. This script needs YOUR unpacked copy of
the official root filesystem to compute the lists in derive/ (HIBY_STOCK_ROOT, or HIBY_STOCK
for its usr/resource folder); it checks that copy against known hashes, refuses to ship any
text file that exists in the official firmware, and removes any image identical to HiBy's.

Format of derive/ (version 1), applied by overlay/usr/bin/hibyos_install.sh:

  derive/INDEX   text, one line per way of building a file ("#" lines are comments):
                   <req|opt> <dest> <sha256 of the result> <base> <sha256 of the base> <strip> <id>
                 dest   path inside the payload the result is installed as (theme/..., mirror/...,
                        sys/...), exactly as if the payload contained that file;
                 base   file of the player's root filesystem (usr/resource/... or usr/bin/...);
                 strip  1 = delete every CR (0x0D) byte of the base before applying;
                 id     "-" = the base already is the result (copy it); otherwise the name of
                        derive/<id>.ops and derive/<id>.add.
                 Several lines may have the same dest: the first one whose base has the stated
                 sha256 is used (the firmware may already carry an older ByOS version of that
                 file). "req": if none matches, the installation is refused before anything is
                 changed. "opt": the file is skipped (the copy in the firmware stays) and the
                 installer logs it.
  derive/<id>.ops  one operation per line:  c <offset> <length>   bytes of the base
                                            a <offset> <length>   bytes of derive/<id>.add
  derive/<id>.add  the bytes we add, concatenated (our text only).
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
THEME = os.path.join(ROOT, "theme")
# your unpacked copy of the official root filesystem (never part of this repository)
STOCK = os.environ.get("HIBY_STOCK", r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource")
STOCK_ROOT = os.environ.get("HIBY_STOCK_ROOT") or os.path.normpath(os.path.join(STOCK, "..", ".."))
STOCK = os.path.join(STOCK_ROOT, "usr", "resource")
sys.path.insert(0, THEME)
sys.path.insert(0, HERE)
import viewlib as V  # noqa: E402
import vendor_edits as VE  # noqa: E402
import notices  # noqa: E402

BINS = {  # payload/bin/<name>: source
    "hibyshell": "bin/hibyshell",
    "hibywall": "bin/hibywall",
    "hibymix": "bin/hibymix",
    "hibytag": "bin/hibytag",
    "hibymod_disp.so": "bin/hibymod_disp.so",
    "hibynet": "bin/hibynet",          # lyrics and covers (LRCLIB, iTunes, Cover Art Archive)
    "cacert.pem": "bin/cacert.pem",    # Mozilla TLS roots for hibynet
    "hibybudsd": "out/agent_buds/hibybudsd",   # headphones daemon (tools/wsl/zbuild_buds.sh); the Headphones app needs it
}
# sources of each binary (only to warn when the binary is older than its code)
SRCS = {
    "hibyshell": ["src/hibyshell.c", "src/*.inc.c", "src/hm_shared.h", "src/digits.h", "src/hibytag.c"],
    "hibywall": ["src/hibywall.c"],
    "hibymix": ["src/hibymix.c"],
    "hibytag": ["src/hibytag.c", "src/hibytag.h"],
    "hibymod_disp.so": ["src/hibymod_disp.c", "src/hm_shared.h"],
    "hibynet": ["src/hibynet.c"],
    "hibybudsd": ["src/hibybudsd.c"],
}
THEME_SUBS = ("layout/theme3", "litegui/theme3")
# Replacements of root filesystem scripts (docs/OTA.md): name in sys/ -> marker. A script is
# only shipped when its version in the firmware already knows how to delegate (the marker is
# the safety condition). Source: overlay/usr/bin/<name> when the script is entirely ours;
# otherwise the entry "usr/bin/<name>" of tools/vendor_edits.json (HiBy's script with our
# changes), which goes to derive/ and is rebuilt on the player.
SYS = {
    "hibyos_install.sh": None,
    "hiby_player.sh": "HIBYOS_SYS_COUNTED",
}
# official home screen icons that become transparent in mirror mode
MIRROR_ICONS = ("book", "book_s", "dac", "dac_s", "music", "music_s", "stream_media", "stream_media_s",
                "sys_set", "sys_set_s", "wireless", "wireless_s")
# Official files (other than the ones in tools/vendor_edits.json) that this script reads to
# compute derive/: path in the root filesystem -> (size, sha256) in firmware v1.3.
OFFICIAL = {
    "usr/resource/layout/theme3/launcher/hiby_launcher_apps.view":
        (5187, "457b558904a96e782ac7fd4e0136481580dccf4d94181722b42914af908863e1"),
    "usr/resource/layout/theme3/dialog/volume.dlg":
        (873, "48f8d119a02b653792c279df8edad4b5b1e7a0266f13692dc5a60bd1b9c0b227"),
}
# derived files without which ByOS must not be installed (the rest are optional)
REQUIRED = ("layout/theme3/launcher/hiby_launcher_apps.view",)
IMAGES = (".png", ".jpg", ".jpeg")
# an alternative base (an older ByOS version of a file) is not used if the bytes we would
# have to store contain a run of HiBy's text this long
MAX_SHARED = VE.MIN_MATCH_SCRIPT


def version_default():
    with open(os.path.join(ROOT, "overlay", "usr", "bin", "hibymod"), encoding="utf-8") as f:
        m = re.search(r'echo "ByOS ([0-9][0-9A-Za-z.\-]*)"', f.read())
    return m.group(1) if m else "0.0.0"


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def check_mips(path):
    with open(path, "rb") as f:
        h = f.read(20)
    if h[:4] != b"\x7fELF" or h[5] != 1 or struct.unpack("<H", h[18:20])[0] != 8:
        sys.exit(f"ERROR: {path} is not a little-endian MIPS ELF")


def newest_src(patterns):
    import glob
    t = 0
    for p in patterns:
        for f in glob.glob(os.path.join(ROOT, p)):
            t = max(t, os.path.getmtime(f))
    return t


def stock_path(rel):
    return os.path.join(STOCK, *rel.split("/"))


def in_official(root_rel):
    """True if the official firmware has a regular file at this root filesystem path."""
    p = os.path.join(STOCK_ROOT, *root_rel.split("/"))
    return os.path.isfile(p) and not os.path.islink(p)


def official(root_rel, entries):
    """Bytes of an official file we derive from, checked against the known size and sha256."""
    data = read(os.path.join(STOCK_ROOT, *root_rel.split("/")))
    if root_rel in entries:
        want = (entries[root_rel]["stock"]["size"], entries[root_rel]["stock"]["sha256"])
    elif root_rel in OFFICIAL:
        want = OFFICIAL[root_rel]
    else:
        sys.exit(f"ERROR: {root_rel} is a file of the official firmware that this script does not know.\n"
                 "       A payload file may not be an edited copy of it unless its size and sha256 are\n"
                 "       listed in OFFICIAL (tools/make_payload.py) or in tools/vendor_edits.json.")
    if (len(data), VE.sha256(data)) != want:
        sys.exit(f"ERROR: {os.path.join(STOCK_ROOT, root_rel)} is not the file of the official firmware v1.3\n"
                 "       (set HIBY_STOCK_ROOT to a clean unpacked copy of its root filesystem).")
    return data


def volume_dlg(dst):
    """HiBy's volume dialog, hidden (hibyshell draws its own volume pill)."""
    root = V.load(stock_path("layout/theme3/dialog/volume.dlg"))
    vg = V.get(root, "viewgroup")
    n = 0
    for k, el in V.children(vg):
        if k in ("imageview", "progress_bar"):
            V.set_(el, "visible", "hide")
            n += 1
    if n != 3:
        sys.exit(f"ERROR: unexpected official volume.dlg ({n} elements)")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    V.save(dst, root)


def same_picture(a, b):
    """True if two image files show exactly the same pixels (whatever the encoding)."""
    try:
        ia, ib = Image.open(a).convert("RGBA"), Image.open(b).convert("RGBA")
    except Exception:
        return False
    # raw comparison: ImageChops.difference().getbbox() only looks at the alpha channel of an
    # RGBA image (Pillow >= 10) and would call two different opaque pictures "the same"
    return ia.size == ib.size and ia.tobytes() == ib.tobytes()


class Derive:
    """Collects the files to be rebuilt on the player and writes payload/derive/."""

    def __init__(self, entries, warn):
        self.entries, self.warn = entries, warn
        self.items = []            # (dest, root_rel, result)

    def add(self, dest, root_rel, result):
        self.items.append((dest, root_rel, result))

    def bases(self, root_rel):
        """What a player may have at that path, best first: the ByOS versions written by the
        firmware builders (current, then older ones) and the official file."""
        stock = official(root_rel, self.entries)
        out = []
        e = self.entries.get(root_rel)
        if e:
            for v in [e] + e.get("previous", []):
                out.append(VE.apply(e, stock, v))
        out.append(stock)
        return stock, out

    def write(self, pl, expect):
        if not self.items:
            return 0
        d = os.path.join(pl, "derive")
        os.makedirs(d)
        lines = ["# ByOS: files rebuilt on the player from the copy in its own firmware.\n",
                 "# <req|opt> <dest> <sha256 result> <base> <sha256 base> <strip CR> <id>   (tools/make_payload.py)\n"]
        blobs = {}
        for dest, root_rel, result in sorted(self.items):
            kind = "req" if dest.endswith(REQUIRED) else "opt"
            stock, bases = self.bases(root_rel)
            pristine = stock.replace(b"\r", b"")
            rsum, n = VE.sha256(result), 0
            for dev in bases:
                dsum = VE.sha256(dev)
                if dsum == rsum:
                    lines.append(f"{kind} {dest} {rsum} {root_rel} {dsum} 0 -\n")
                    n += 1
                    continue
                strip = b"\r" in dev and b"\r" not in result
                base = dev.replace(b"\r", b"") if strip else dev
                if b"\0" in base or b"\1" in base or b"\0" in result or b"\1" in result:
                    sys.exit(f"ERROR: {dest}: binary data cannot be derived on the player")
                ops = VE.make_ops(base, result, VE.MIN_MATCH)
                lits = VE.literals(ops)
                shared = VE.longest_shared(lits, pristine)
                if shared >= MAX_SHARED:
                    self.warn.append(f"{dest}: not derivable from the version {dsum[:12]} of {root_rel} without "
                                     f"storing {shared} bytes of HiBy's text; that alternative is left out")
                    continue
                add, text = b"", []
                for op in ops:
                    (k, arg), = op.items()
                    if k == "copy":
                        text.append(f"c {arg[0]} {arg[1]}\n")
                    else:
                        lit = "\n".join(arg).encode("utf-8")
                        text.append(f"a {len(add)} {len(lit)}\n")
                        add += lit
                ident = hashlib.sha256(f"{dsum} {rsum} {int(strip)}".encode()).hexdigest()[:12]
                blobs[ident] = ("".join(text).encode(), add)
                lines.append(f"{kind} {dest} {rsum} {root_rel} {dsum} {int(strip)} {ident}\n")
                n += 1
            if not n:
                sys.exit(f"ERROR: {dest}: no way to derive it on the player")
            if expect:
                write(os.path.join(expect, *dest.split("/")), result)
        for ident, (ops, add) in sorted(blobs.items()):
            write(os.path.join(d, ident + ".ops"), ops)
            write(os.path.join(d, ident + ".add"), add)
        write(os.path.join(d, "INDEX"), "".join(lines).encode())
        return len(self.items)


def main():
    ap = argparse.ArgumentParser(description="Build the ByOS/ folder for the SD card.")
    ap.add_argument("--version", default=version_default())
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "sd"))
    ap.add_argument("--expect", help="for tests: also write here the full files that the player will "
                                     "build from derive/ (they contain HiBy's text: never publish them)")
    ap.add_argument("--edits", help=argparse.SUPPRESS)       # another vendor_edits.json (tests)
    a = ap.parse_args()

    if not os.path.isdir(STOCK):
        sys.exit(f"ERROR: the official root filesystem was not found in {STOCK_ROOT}\n"
                 "       (set HIBY_STOCK_ROOT to your unpacked copy of the official firmware's rootfs)")
    try:
        entries = {e["path"]: e for e in VE.load(a.edits)}
    except (OSError, ValueError, KeyError, VE.EditError) as e:
        sys.exit(f"ERROR: tools/vendor_edits.json: {e}")

    top = os.path.join(a.out, "ByOS")
    pl = os.path.join(top, "payload")
    if os.path.isdir(top):
        shutil.rmtree(top)
    os.makedirs(pl)
    if a.expect and os.path.isdir(a.expect):
        shutil.rmtree(a.expect)
    warn = []
    derive = Derive(entries, warn)

    # ---- binaries
    for name, rel in BINS.items():
        src = os.path.join(ROOT, rel)
        if not os.path.isfile(src):
            sys.exit(f"ERROR: {rel} is missing (build it in WSL with tools/wsl/zbuild_*.sh)")
        if name.endswith(".pem"):
            copy(src, os.path.join(pl, "bin", name)); continue
        check_mips(src)
        if newest_src(SRCS[name]) > os.path.getmtime(src) + 1:
            warn.append(f"{rel} is older than its source code (rebuild?)")
        copy(src, os.path.join(pl, "bin", name))

    # ---- shell layers (no previews, working PNGs or device state)
    sh = os.path.join(ROOT, "shell")
    for f in sorted(os.listdir(sh)):
        if f.endswith(".ov") or f in ("wall.raw", "home.sig"):
            copy(os.path.join(sh, f), os.path.join(pl, "shell", f))
    for f in sorted(os.listdir(os.path.join(sh, "icons"))):
        if f.endswith(".ov"):
            copy(os.path.join(sh, "icons", f), os.path.join(pl, "shell", "icons", f))
    for f in ("wall.raw", "home.sig", "home_btn.ov", "hibyos_logo.ov"):
        if not os.path.isfile(os.path.join(pl, "shell", f)):
            sys.exit(f"ERROR: shell/{f} is missing (python tools/shell_assets.py)")

    stage = tempfile.mkdtemp(prefix="hibyos_payload_")
    try:
        # ---- theme and mirror home screen, complete, in a private folder: what the player
        #      must end up with. HiBy's layouts with our changes are among them.
        th = os.path.join(stage, "theme")
        for base in (os.path.join(THEME, "out"), os.path.join(ROOT, "overlay", "usr", "resource")):
            for sub in THEME_SUBS:
                d = os.path.join(base, *sub.split("/"))
                for dp, _dn, fn in os.walk(d):
                    for f in fn:
                        s = os.path.join(dp, f)
                        rel = os.path.relpath(s, base).replace("\\", "/")
                        copy(s, os.path.join(th, *rel.split("/")))
        for p, e in sorted(entries.items()):        # the layouts that the firmware builders edit
            if p.startswith("usr/resource/") and p[len("usr/resource/"):].startswith(tuple(s + "/" for s in THEME_SUBS)):
                write(os.path.join(th, *p[len("usr/resource/"):].split("/")), VE.apply(e, official(p, entries)))
        official("usr/resource/layout/theme3/dialog/volume.dlg", entries)
        volume_dlg(os.path.join(th, "layout", "theme3", "dialog", "volume.dlg"))
        for f in ("layout/theme3/launcher/hiby_launcher_apps.view", "litegui/theme3/launcher/wallpaper.png",
                  "layout/theme3/topbar/topbar.view"):
            if not os.path.isfile(os.path.join(th, *f.split("/"))):
                sys.exit(f"ERROR: theme/{f} is missing (theme/build_theme.py, theme/topbar.py, theme/hibyos_brand.py)")

        mi = os.path.join(stage, "mirror")
        view = os.path.join(mi, "layout", "theme3", "launcher", "hiby_launcher_apps.view")
        os.makedirs(os.path.dirname(view))
        subprocess.run([sys.executable, os.path.join(THEME, "mirror_view.py"),
                        os.path.join(th, "layout", "theme3", "launcher", "hiby_launcher_apps.view"), view],
                       check=True, capture_output=True)
        ldir = os.path.join(mi, "litegui", "theme3", "launcher")
        os.makedirs(ldir)
        for n in MIRROR_ICONS:
            src = os.path.join(th, "litegui", "theme3", "launcher", n + ".png")
            if not os.path.isfile(src):
                sys.exit(f"ERROR: the theme icon {n}.png is missing (theme/build_theme.py)")
            Image.new("RGBA", Image.open(src).size, (0, 0, 0, 0)).save(os.path.join(ldir, n + ".png"), optimize=True)

        # ---- what goes into the payload: our own files as they are; HiBy's files with our
        #      changes only as a list of operations (derive/). Nothing identical to HiBy's.
        for top_dir, base in (("theme", th), ("mirror", mi)):
            for dp, dn, fn in os.walk(base):
                dn.sort()
                for f in sorted(fn):
                    p = os.path.join(dp, f)
                    rel = os.path.relpath(p, base).replace("\\", "/")
                    root_rel = "usr/resource/" + rel
                    has = in_official(root_rel)
                    if has and md5(stock_path(rel)) == md5(p):
                        warn.append(f"left out {top_dir}/{rel}: identical to the official file")
                    elif rel.lower().endswith(IMAGES):
                        if has and same_picture(p, stock_path(rel)):
                            warn.append(f"left out {top_dir}/{rel}: same picture as the official file")
                        else:
                            copy(p, os.path.join(pl, top_dir, *rel.split("/")))
                    elif has or root_rel in entries:
                        derive.add(f"{top_dir}/{rel}", root_rel, read(p))
                    else:
                        copy(p, os.path.join(pl, top_dir, *rel.split("/")))
    finally:
        shutil.rmtree(stage, ignore_errors=True)

    # ---- wallpapers for /Pictures
    tmp = tempfile.mkdtemp()
    try:
        subprocess.run([sys.executable, os.path.join(THEME, "wallpapers.py"), os.path.join(tmp, "p")],
                       check=True, capture_output=True)
        for f in sorted(os.listdir(os.path.join(tmp, "p"))):
            copy(os.path.join(tmp, "p", f), os.path.join(pl, "Pictures", f))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- replacements of root filesystem scripts (LF, like build.py) and their MANIFEST.sha256
    sysd = os.path.join(pl, "sys")
    sums = []
    for name, marker in SYS.items():
        src = os.path.join(ROOT, "overlay", "usr", "bin", name)
        root_rel = "usr/bin/" + name
        ours = os.path.isfile(src)
        if ours:
            data = read(src).replace(b"\r\n", b"\n")
        elif root_rel in entries:
            data = VE.apply(entries[root_rel], official(root_rel, entries))
        else:
            sys.exit(f"ERROR: {name}: neither overlay/usr/bin/{name} nor an entry in tools/vendor_edits.json")
        if marker and marker.encode() not in data:
            continue                       # its version in the firmware does not delegate yet: not shipped
        if ours:
            write(os.path.join(sysd, name), data)
        else:
            derive.add("sys/" + name, root_rel, data)
        sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
    if sums:
        os.makedirs(sysd, exist_ok=True)
        with open(os.path.join(sysd, "MANIFEST.sha256"), "w", newline="\n", encoding="utf-8") as f:
            f.writelines(sums)

    nder = derive.write(pl, a.expect)

    # ---- third-party notices (licence texts of what bin/ contains)
    notices.collect("sd-payload", os.path.join(pl, "licenses"))

    # ---- last check: no text file of the payload may exist in the official firmware
    for top_dir in ("theme", "mirror"):
        for dp, _dn, fn in os.walk(os.path.join(pl, top_dir)):
            for f in fn:
                rel = os.path.relpath(os.path.join(dp, f), os.path.join(pl, top_dir)).replace("\\", "/")
                if not rel.lower().endswith(IMAGES) and in_official("usr/resource/" + rel):
                    sys.exit(f"ERROR: internal: {top_dir}/{rel} would ship an edited official file")

    # ---- version and manifest (LF; paths without spaces)
    with open(os.path.join(pl, "VERSION"), "w", newline="\n") as f:
        f.write(a.version + "\n")
    lines = []
    for dp, _dn, fn in os.walk(pl):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, pl).replace("\\", "/")
            if rel == "MANIFEST.md5":
                continue
            if any(c in rel for c in " \t\n\\"):
                sys.exit(f"ERROR: name not allowed in the payload: {rel!r}")
            lines.append(f"{md5(p)}  {rel}\n")
    lines.sort(key=lambda l: l.split("  ", 1)[1])
    with open(os.path.join(pl, "MANIFEST.md5"), "w", newline="\n", encoding="utf-8") as f:
        f.writelines(lines)

    # ---- summary
    tot = {}
    for l in lines:
        rel = l.split("  ", 1)[1].strip()
        k = rel.split("/")[0] if "/" in rel else "."
        tot.setdefault(k, [0, 0])
        tot[k][0] += 1
        tot[k][1] += os.path.getsize(os.path.join(pl, rel))
    print(f"ByOS {a.version}: {os.path.abspath(top)}")
    for k, (n, b) in sorted(tot.items()):
        print(f"  {k:9s} {n:4d} files {b / 1024:8.1f} KB")
    print(f"  total     {len(lines):4d} files {sum(b for _n, b in tot.values()) / 1024:8.1f} KB")
    print(f"  {nder} files are rebuilt on the player from its own firmware (derive/)")
    for w in warn:
        print("WARNING:", w)


if __name__ == "__main__":
    main()
