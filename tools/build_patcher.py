#!/usr/bin/env python3
"""
Packages the HiByOS patcher (tools/hibyos_patcher.py).

  python tools/build_patcher.py            (Windows: builds the .exe too)

Produces in out/patcher/:
  HiByOS-Patcher.exe                        (Windows only, PyInstaller one-file console app)
  HiByOS-Patcher-<version>-windows.zip      (the .exe + README.txt)
  HiByOS-Patcher-<version>-python.zip       (pure Python version for macOS/Linux/Windows:
                                             patcher modules + overlay + bundled pycdlib)

If PyInstaller or pycdlib are missing, a private virtual environment is created in
out/patcher/venv (out/ is not tracked by git) and the script re-runs itself there.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, "tools")
OUT = os.path.join(REPO, "out", "patcher")
MODULES = ["hibyos_patcher.py", "build.py", "squashfs.py", "lzo1x.py"]
ZIP_DATE = (2026, 1, 1, 0, 0, 0)

README_WIN = """HiByOS patcher {v} for Windows
================================

HiByOS does not include HiBy's firmware. This program turns YOUR copy of the official
HiBy R3 II (2025) firmware v1.3 into the HiByOS firmware, on your computer.

1. Download the official firmware r3ii_2025.upt (v1.3) from HiBy's website.
2. Drag r3ii_2025.upt onto HiByOS-Patcher.exe
   (or double-click HiByOS-Patcher.exe and drag the file into the window, then Enter;
    or from a terminal: HiByOS-Patcher.exe C:\\path\\to\\r3ii_2025.upt).
3. The HiByOS firmware is written to a "HiByOS-{v}" folder next to your official file:
      HiByOS-{v}\\r3ii_2025.upt
   Your original file is never modified.
4. Copy that r3ii_2025.upt and the HiByOS/ folder from the release to the root of the
   SD card, then on the player: Settings > Firmware update > Via SD card.

The patcher only accepts the exact official file (md5 0d16d09c1458dd2f2cdaf38d0047917a).
It checks every step and writes nothing if anything is unexpected.
Windows SmartScreen may warn because the program is not signed: "More info" > "Run anyway".
"""

README_PY = """HiByOS patcher {v} (Python version: macOS, Linux, Windows)
==========================================================

HiByOS does not include HiBy's firmware. This program turns YOUR copy of the official
HiBy R3 II (2025) firmware v1.3 into the HiByOS firmware, on your computer.
It is pure Python: no other tools, no root/administrator rights.

Requirements: Python 3.8 or newer (macOS: python.org installer or Homebrew; Linux: the
system python3). The pycdlib library is included in vendor/ (LGPL-2.1, see
vendor/pycdlib-LICENSE.txt), nothing to install.

1. Download the official firmware r3ii_2025.upt (v1.3) from HiBy's website.
2. Open a terminal in this folder and run:
      python3 hibyos_patcher.py /path/to/r3ii_2025.upt
   (Windows: py hibyos_patcher.py C:\\path\\to\\r3ii_2025.upt)
3. The HiByOS firmware is written to a "HiByOS-{v}" folder next to your official file:
      HiByOS-{v}/r3ii_2025.upt
   Your original file is never modified.
4. Copy that r3ii_2025.upt and the HiByOS/ folder from the release to the root of the
   SD card, then on the player: Settings > Firmware update > Via SD card.

The patcher only accepts the exact official file (md5 0d16d09c1458dd2f2cdaf38d0047917a).
It checks every step and writes nothing if anything is unexpected.
"""


def version():
    t = open(os.path.join(REPO, "overlay", "usr", "bin", "hibymod"), encoding="utf-8").read()
    return re.search(r"HiByOS (\d+(?:\.\d+)+)", t).group(1)


def ensure_tools():
    try:
        import PyInstaller  # noqa: F401
        import pycdlib  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("HIBYOS_PATCHER_VENV"):
        sys.exit("PyInstaller/pycdlib still missing inside the build venv")
    venv = os.path.join(OUT, "venv")
    py = os.path.join(venv, "Scripts", "python.exe") if os.name == "nt" else os.path.join(venv, "bin", "python")
    if not os.path.exists(py):
        print("== Creating", venv)
        subprocess.check_call([sys.executable, "-m", "venv", venv])
    subprocess.check_call([py, "-m", "pip", "-q", "install", "pycdlib", "pyinstaller"])
    env = dict(os.environ, HIBYOS_PATCHER_VENV="1")
    sys.exit(subprocess.call([py, os.path.abspath(__file__)] + sys.argv[1:], env=env))


def add_tree(z, src, arc):
    for d, dirs, names in os.walk(src):
        dirs.sort()
        for n in sorted(names):
            p = os.path.join(d, n)
            rel = os.path.relpath(p, src).replace(os.sep, "/")
            add_file(z, p, f"{arc}/{rel}")


def add_file(z, path, arc, data=None):
    zi = zipfile.ZipInfo(arc, ZIP_DATE)
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = (0o755 if arc.endswith(".py") or arc.endswith(".exe") else 0o644) << 16
    z.writestr(zi, data if data is not None else open(path, "rb").read())


def pycdlib_files():
    import pycdlib
    pkg = os.path.dirname(pycdlib.__file__)
    lic = None
    for dist in glob.glob(os.path.join(os.path.dirname(pkg), "pycdlib-*.dist-info")):
        for cand in glob.glob(os.path.join(dist, "**", "*"), recursive=True):
            if re.search(r"(LICEN[CS]E|COPYING)", os.path.basename(cand), re.I) and os.path.isfile(cand):
                lic = cand
    return pkg, lic


def main():
    ensure_tools()
    v = version()
    os.makedirs(OUT, exist_ok=True)
    print("HiByOS patcher", v)

    # ---- pure Python zip
    pyzip = os.path.join(OUT, f"HiByOS-Patcher-{v}-python.zip")
    top = f"HiByOS-Patcher-{v}"
    pkg, lic = pycdlib_files()
    with zipfile.ZipFile(pyzip, "w") as z:
        add_file(z, None, f"{top}/README.txt", README_PY.format(v=v).encode())
        for m in MODULES:
            add_file(z, os.path.join(TOOLS, m), f"{top}/{m}")
        add_tree(z, os.path.join(REPO, "overlay"), f"{top}/overlay")
        for d, dirs, names in os.walk(pkg):
            dirs[:] = sorted(x for x in dirs if x != "__pycache__")
            for n in sorted(names):
                if n.endswith(".py") or n == "py.typed":
                    p = os.path.join(d, n)
                    rel = os.path.relpath(p, pkg).replace(os.sep, "/")
                    add_file(z, p, f"{top}/vendor/pycdlib/{rel}")
        if lic:
            add_file(z, lic, f"{top}/vendor/pycdlib-LICENSE.txt")
        else:
            print("WARNING: pycdlib license file not found")
    print("->", pyzip, os.path.getsize(pyzip), "bytes")

    # ---- Windows exe
    if os.name != "nt":
        print("(not on Windows: the .exe is not built)")
        return
    work = os.path.join(OUT, "build")
    shutil.rmtree(work, ignore_errors=True)
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--console",
           "--name", "HiByOS-Patcher", "--distpath", OUT, "--workpath", work, "--specpath", work,
           "--paths", TOOLS, "--hidden-import", "build", "--hidden-import", "squashfs",
           "--hidden-import", "lzo1x", "--exclude-module", "tkinter",
           "--add-data", os.path.join(REPO, "overlay") + os.pathsep + "overlay",
           os.path.join(TOOLS, "hibyos_patcher.py")]
    print("+", " ".join(cmd))
    subprocess.check_call(cmd)
    exe = os.path.join(OUT, "HiByOS-Patcher.exe")
    winzip = os.path.join(OUT, f"HiByOS-Patcher-{v}-windows.zip")
    with zipfile.ZipFile(winzip, "w") as z:
        add_file(z, exe, f"HiByOS-Patcher-{v}/HiByOS-Patcher.exe")
        add_file(z, None, f"HiByOS-Patcher-{v}/README.txt",
                 README_WIN.format(v=v).replace("\n", "\r\n").encode())
    print("->", exe, os.path.getsize(exe), "bytes")
    print("->", winzip, os.path.getsize(winzip), "bytes")


if __name__ == "__main__":
    main()
