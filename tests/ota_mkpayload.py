#!/usr/bin/env python3
"""Synthetic ByOS payloads for tests/ota_test.sh (same layout as tools/make_payload.py, tiny
fake files; the real installer accepts them).

  ota_mkpayload.py OUT VERSION [--shell N] [--theme N] [--disp N] [--override] [--bad-manifest]

  --shell N      variant of bin/hibyshell and shell/home_btn.ov (changes -> interface restart)
  --theme N      variant of the theme view (changes -> reboot)
  --disp N       variant of bin/hibymod_disp.so (changes -> reboot)
  --override     sys/hibyos_install.sh = the real installer + a marker line, and its
                 sys/MANIFEST.sha256 (like make_payload.py)
  --bad-manifest one md5 in MANIFEST.md5 is wrong (the installer must refuse the payload)
  --no-launcher-view  consistent payload that the installer gives up on half-way (after
                 copying files): tests the recovery of the previous version
"""
import argparse
import hashlib
import os
import re
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
INST = os.path.join(HERE, "..", "overlay", "usr", "bin", "hibyos_install.sh")

ap = argparse.ArgumentParser()
ap.add_argument("out"); ap.add_argument("version")
ap.add_argument("--shell", type=int, default=0); ap.add_argument("--theme", type=int, default=0)
ap.add_argument("--disp", type=int, default=0)
ap.add_argument("--override", action="store_true"); ap.add_argument("--bad-manifest", action="store_true")
ap.add_argument("--no-launcher-view", action="store_true")
ap.add_argument("--override-rev", type=int, help="HIBYOS_SYS_REV of the override (default: rootfs + 1)")
a = ap.parse_args()

if os.path.isdir(a.out):
    shutil.rmtree(a.out)
F = {
    "VERSION": f"{a.version}\n".encode(),
    "bin/hibyshell": b"\x7fELF shell " + str(a.shell).encode() * 300,
    "bin/hibywall": b"\x7fELF wall",
    "bin/hibymod_disp.so": b"\x7fELF disp " + str(a.disp).encode(),
    "shell/home_btn.ov": b"overlay " + str(a.shell).encode(),
    "shell/wall.raw": b"".join(hashlib.sha256(b"%d" % i).digest() for i in range(22000)),   # ~700 KB: progress lines
    "shell/icons/music_56.ov": b"icon",
    "theme/layout/theme3/launcher/hiby_launcher_apps.view": b'{"ours":' + str(a.theme).encode() + b"}",
    "theme/layout/theme3/topbar/topbar.view": b'{"topbar":1}',
    "theme/litegui/theme3/launcher/music.png": b"PNG ours",
    "mirror/layout/theme3/launcher/hiby_launcher_apps.view": b'{"mirror":1}',
    "mirror/litegui/theme3/launcher/music.png": b"PNG transparent",
    "Pictures/Aurora.png": b"PNG aurora",
}
if a.no_launcher_view:
    del F["theme/layout/theme3/launcher/hiby_launcher_apps.view"]
if a.override:
    with open(INST, "rb") as f:
        s = f.read().replace(b"\r\n", b"\n")
    marker = b'echo "OVERRIDE $*" >> ${HIBYOS_TMP:-/tmp/hibymod}/override.log\n'
    m = re.search(rb"^# HIBYOS_SYS_REV=([0-9]+)", s, re.M)
    rev = a.override_rev if a.override_rev is not None else int(m.group(1)) + 1   # newer than the rootfs copy
    s = s[:m.start()] + b"# HIBYOS_SYS_REV=%d" % rev + s[m.end():]
    nl = s.index(b"\n") + 1                       # after the #! line
    s = s[:nl] + marker + s[nl:]
    F["sys/hibyos_install.sh"] = s
    F["sys/MANIFEST.sha256"] = f"{hashlib.sha256(s).hexdigest()}  hibyos_install.sh\n".encode()
lines = []
for rel, data in sorted(F.items()):
    p = os.path.join(a.out, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)
    lines.append(f"{hashlib.md5(data).hexdigest()}  {rel}\n")
if a.bad_manifest:
    lines[1] = "0" * 32 + lines[1][32:]
with open(os.path.join(a.out, "MANIFEST.md5"), "w", newline="\n") as f:
    f.writelines(lines)
