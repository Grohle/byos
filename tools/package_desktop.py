#!/usr/bin/env python3
"""Package ByOS Desktop for Windows as a portable zip.

    python tools/package_desktop.py --version 0.1.0

Needs android/hibyos-install/cli/build/libs/byos-cli.jar (gradlew :cli:fatJar). Writes
out/release/ByOS-Desktop-<version>-windows.zip with one folder, "ByOS Desktop":

    ByOS Desktop.cmd    the launcher (checks Node.js 20+, Java 17+ and adb, then opens the app)
    byos-cli.jar        the command-line engine the app drives
    app/                the web UI and its local runner (desktop/: server.mjs, lib/, ui/, mock/)
    README.txt          how to start it
    LICENSE, licenses/  ByOS licence, THIRD_PARTY_NOTICES.md and the licences of byos-cli

Node.js, Java and adb are not included: the launcher tells the user where to get them.
"""
import argparse, hashlib, os, sys, zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import notices  # noqa: E402

ROOT = "ByOS Desktop"
APP_PARTS = ["server.mjs", "package.json", "README.md", "lib", "ui", "mock"]

README = """ByOS Desktop {version} for Windows
================================

Manage a HiBy R3 II (2025) running ByOS from your computer: files on the SD card,
the music library, playlists, and building and installing ByOS from HiBy's official
firmware.

What you need
-------------
1. Node.js 20 or newer (LTS):        https://nodejs.org
2. Java 17 or newer (Temurin 21):    https://adoptium.net
3. For USB connections, adb from Google's "SDK Platform-Tools for Windows":
   https://developer.android.com/tools/releases/platform-tools
   Unzip it so that the "platform-tools" folder sits next to "ByOS Desktop.cmd"
   (or have adb on your PATH).

Start
-----
1. Unzip this file anywhere (for example in Documents).
2. On the HiBy, open Settings > ByOS Sync and connect the USB cable.
3. Double-click "ByOS Desktop.cmd". The app opens in its own window.

To try it without a HiBy, open a command prompt in this folder and run:
    node app\\server.mjs --mock --open

More: app\\README.md and https://github.com/Grohle/hibyos
"""


def crlf(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def add_tree(z, src, arc):
    if os.path.isfile(src):
        z.write(src, arc)
        return
    for dp, dn, fn in os.walk(src):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, src).replace(os.sep, "/")
            z.write(p, arc + "/" + rel)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--jar", default=os.path.join(REPO, "android", "hibyos-install", "cli", "build", "libs", "byos-cli.jar"))
    ap.add_argument("--out", default=os.path.join(REPO, "out", "release"))
    a = ap.parse_args()
    if not os.path.isfile(a.jar):
        sys.exit("byos-cli.jar not found: %s (run gradlew :cli:fatJar in android/hibyos-install)" % a.jar)
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, "ByOS-Desktop-%s-windows.zip" % a.version)
    lic_tmp = os.path.join(REPO, "out", "desktop_licenses")
    if os.path.isdir(lic_tmp):
        for n in os.listdir(lic_tmp):
            os.remove(os.path.join(lic_tmp, n))
    lic = notices.collect("byos-cli", lic_tmp)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        with open(os.path.join(REPO, "desktop", "windows", "ByOS Desktop.cmd"), encoding="utf-8") as f:
            z.writestr(ROOT + "/ByOS Desktop.cmd", crlf(f.read()))
        z.writestr(ROOT + "/README.txt", crlf(README.format(version=a.version)))
        z.write(a.jar, ROOT + "/byos-cli.jar")
        for part in APP_PARTS:
            add_tree(z, os.path.join(REPO, "desktop", part), ROOT + "/app/" + part)
        z.write(os.path.join(REPO, "LICENSE"), ROOT + "/LICENSE")
        for p in lic:
            z.write(p, ROOT + "/licenses/" + os.path.basename(p))
    h = hashlib.sha256(open(out, "rb").read()).hexdigest()
    print("ByOS Desktop %s -> %s (%d bytes, sha256 %s)" % (a.version, out, os.path.getsize(out), h))


if __name__ == "__main__":
    main()
