#!/usr/bin/env python3
"""Package ByOS Desktop for Windows as one program: ByOS.exe.

    python tools/package_desktop.py --version 0.9.0

The exe is the launcher (desktop/windows/launcher.c, built by tools/wsl/zbuild_desktop_exe.sh into
out/desktop_exe/launcher.exe) followed by a zip and a 16-byte trailer ("BYOSPAK1" + the zip's
offset, little endian). At its first start the launcher unpacks the zip into
%LOCALAPPDATA%\\ByOS Desktop\\<version> (see launcher.c). The zip holds:

    app/              the web UI and its local runner (desktop/: server.mjs, lib/, ui/, mock/)
    byos-cli.jar      the engine the app drives (gradlew :cli:fatJar)
    runtime/node/     node.exe of the official Node.js Windows x64 zip (--node-zip)
    runtime/java/     a Java runtime made with jlink from a JDK 21 (--jre), see below
    platform-tools/   adb.exe and its two DLLs from Google's SDK Platform-Tools (--platform-tools)
    licenses/         THIRD_PARTY_NOTICES.md, byos-cli's licences, Node.js' LICENSE and the
                      platform-tools NOTICE (the Java runtime keeps its own in runtime/java/legal)
    LICENSE           ByOS' licence

Inputs, as used for 0.9.0:
  --node-zip  https://nodejs.org/dist/v24.22.1/node-v24.22.1-win-x64.zip (checked against SHASUMS256.txt)
  --jre       jlink --add-modules java.base,java.desktop,jdk.crypto.ec,jdk.unsupported,jdk.zipfs,jdk.charsets
                    --strip-debug --no-header-files --no-man-pages --compress zip-6 --output JRE
              (Eclipse Temurin 21; byos-cli needs java.base and java.desktop, jdeps --print-module-deps)
  --platform-tools  an unpacked SDK Platform-Tools for Windows (37.0.1)
"""
import argparse, hashlib, io, os, struct, sys, zipfile

REPO = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))
import notices  # noqa: E402

APP_PARTS = ["server.mjs", "package.json", "README.md", "lib", "ui", "mock"]
ADB_FILES = ["adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll"]


def add_tree(z, src, arc):
    if os.path.isfile(src):
        z.write(src, arc)
        return
    for dp, dn, fn in os.walk(src):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n)
            z.write(p, arc + "/" + os.path.relpath(p, src).replace(os.sep, "/"))


def main():
    dl = os.path.join(REPO, "out", "desktop_dl")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--launcher", default=os.path.join(REPO, "out", "desktop_exe", "launcher.exe"))
    ap.add_argument("--jar", default=os.path.join(REPO, "android", "hibyos-install", "cli", "build", "libs", "byos-cli.jar"))
    ap.add_argument("--node-zip", default=os.path.join(dl, "node-v24.22.1-win-x64.zip"))
    ap.add_argument("--jre", default=os.path.join(dl, "jre"))
    ap.add_argument("--platform-tools", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "platform-tools"))
    ap.add_argument("--out", default=os.path.join(REPO, "out", "release"))
    a = ap.parse_args()
    for p, what in [(a.launcher, "launcher (tools/wsl/zbuild_desktop_exe.sh)"), (a.jar, "byos-cli.jar (gradlew :cli:fatJar)"),
                    (a.node_zip, "Node.js zip"), (os.path.join(a.jre, "bin", "java.exe"), "Java runtime (jlink)")] + \
                   [(os.path.join(a.platform_tools, f), "platform-tools") for f in ADB_FILES + ["NOTICE.txt"]]:
        if not os.path.isfile(p):
            sys.exit("missing %s: %s" % (what, p))

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for part in APP_PARTS:
            add_tree(z, os.path.join(REPO, "desktop", part), "app/" + part)
        z.write(a.jar, "byos-cli.jar")
        with zipfile.ZipFile(a.node_zip) as nz:
            top = nz.namelist()[0].split("/")[0]
            z.writestr("runtime/node/node.exe", nz.read(top + "/node.exe"))
            z.writestr("licenses/Node.js-LICENSE.txt", nz.read(top + "/LICENSE"))
        add_tree(z, a.jre, "runtime/java")
        for f in ADB_FILES:
            z.write(os.path.join(a.platform_tools, f), "platform-tools/" + f)
        z.write(os.path.join(a.platform_tools, "NOTICE.txt"), "licenses/Android-platform-tools-NOTICE.txt")
        tmp = os.path.join(REPO, "out", "desktop_licenses")
        if os.path.isdir(tmp):
            for n in os.listdir(tmp):
                os.remove(os.path.join(tmp, n))
        for p in notices.collect("byos-cli", tmp):
            z.write(p, "licenses/" + os.path.basename(p))
        z.write(os.path.join(REPO, "LICENSE"), "LICENSE")
    payload = buf.getvalue()

    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, "ByOS.exe")   # an app name, not a file name with a version (the version is in its properties)
    launcher = open(a.launcher, "rb").read()
    with open(out, "wb") as f:
        f.write(launcher)
        f.write(payload)
        f.write(b"BYOSPAK1" + struct.pack("<Q", len(launcher)))
    h = hashlib.sha256(open(out, "rb").read()).hexdigest()
    print("ByOS Desktop %s -> %s (%d bytes, sha256 %s)" % (a.version, out, os.path.getsize(out), h))


if __name__ == "__main__":
    main()
