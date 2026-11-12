#!/usr/bin/env python3
"""tools/app_release.py: the signed byos-app-update.json of an app release.

    python tests/app_release_test.py

Uses a throw-away key in out/test_app_release/ (never the release key). When Node.js is there,
the index is also read by the desktop app's own parser (desktop/lib/app-update.mjs).
"""
import json
import os
import shutil
import subprocess
import sys
import unittest

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TOOL = os.path.join(ROOT, "tools", "app_release.py")
SIGN = os.path.join(ROOT, "tools", "ota_sign.py")
WORK = os.path.join(ROOT, "out", "test_app_release")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ota_sign  # noqa: E402


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def load(path):
    return json.loads(read(path).decode("utf-8"))


def run(*args, ok=True):
    r = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, encoding="utf-8")
    if ok and r.returncode != 0:
        raise AssertionError(r.stdout + r.stderr)
    return r


class AppReleaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        shutil.rmtree(WORK, ignore_errors=True)
        os.makedirs(WORK)
        cls.key = os.path.join(WORK, "test.key")
        subprocess.run([sys.executable, SIGN, "genkey", "--key", cls.key, "--allow-in-repo"], check=True, capture_output=True)
        cls.pub = ota_sign.public_hex(ota_sign.load_seed(cls.key))
        cls.notes = os.path.join(WORK, "notes.json")
        with open(cls.notes, "w", encoding="utf-8") as f:
            json.dump({"en": "Faster sync.", "es": "Sincronización más rápida.", "fr": "Synchro plus rapide."}, f, ensure_ascii=False)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(WORK, ignore_errors=True)

    def asset(self, name, size=5000):
        p = os.path.join(WORK, name)
        with open(p, "wb") as f:
            f.write(bytes((i * 7) & 255 for i in range(size)))
        return p

    def test_android_index_is_signed_and_complete(self):
        apk = self.asset("hibyos-install-1.2.0.apk")
        out = os.path.join(WORK, "a", "byos-app-update.json")
        run("android", apk, "--version", "1.2.0", "--version-code", "7", "--notes", self.notes, "--key", self.key, "--out", out)
        m = load(out)
        self.assertEqual(m["app"], "byos-install-android")
        self.assertEqual(m["tag"], "install-v1.2.0")
        self.assertEqual(m["version_code"], 7)
        self.assertEqual(m["asset"], "hibyos-install-1.2.0.apk")
        self.assertEqual(m["size"], 5000)
        self.assertEqual(m["min_os"], "26")
        self.assertEqual(m["notes"]["es"], "Sincronización más rápida.")
        # signed by the test key, checked with the file; refused with the player's keys
        self.assertEqual(run("verify", out, "--file", apk, "--pub", self.pub).returncode, 0)
        self.assertNotEqual(run("verify", out, ok=False).returncode, 0)
        # another file, or a changed index: refused
        other = self.asset("other.apk", 5001)
        self.assertNotEqual(run("verify", out, "--file", other, "--pub", self.pub, ok=False).returncode, 0)
        data = read(out).replace(b'"size": 5000', b'"size": 5001')
        bad = os.path.join(WORK, "bad.json")
        write(bad, data)
        self.assertNotEqual(run("verify", bad, "--pub", self.pub, ok=False).returncode, 0)

    def test_android_reads_the_version_from_gradle(self):
        name, code = __import__("app_release").gradle_version()
        apk = self.asset(f"hibyos-install-{name}.apk", 100)
        out = os.path.join(WORK, "g", "byos-app-update.json")
        run("android", apk, "--notes-en", "Fixes.", "--out", out)
        m = load(out)
        self.assertEqual((m["version"], m["version_code"]), (name, code))
        self.assertEqual(m["signature"], "ed25519:" + "0" * 128)  # no --key: left unsigned

    def test_refuses_what_the_apps_would_refuse(self):
        exe = self.asset("ByOS.exe")
        self.assertNotEqual(run("desktop", exe, "--notes", self.notes, ok=False).returncode, 0)  # no version
        self.assertNotEqual(run("desktop", exe, "--version", "v0.9.2", "--notes-en", "x", "--out", os.path.join(WORK, "x.json"), ok=False).returncode, 0)
        self.assertNotEqual(run("desktop", exe, "--version", "0.9.2", "--out", os.path.join(WORK, "y.json"), ok=False).returncode, 0)  # no notes
        wrong = self.asset("ByOS.apk")
        self.assertNotEqual(run("desktop", wrong, "--version", "0.9.2", "--notes-en", "x", "--out", os.path.join(WORK, "z.json"), ok=False).returncode, 0)

    def test_desktop_index_is_read_by_the_desktop_app(self):
        exe = self.asset("ByOS.exe", 12345)
        out = os.path.join(WORK, "d", "byos-app-update.json")
        run("desktop", exe, "--version", "0.9.2", "--notes", self.notes, "--key", self.key, "--out", out)
        m = load(out)
        self.assertEqual((m["app"], m["tag"], m["asset"], m["min_os"]), ("byos-desktop-windows", "desktop-v0.9.2", "ByOS.exe", "10.0.17134"))
        self.assertNotIn("version_code", m)
        node = shutil.which("node")
        if not node:
            self.skipTest("Node.js is not installed")
        lib = os.path.join(ROOT, "desktop", "lib", "app-update.mjs").replace("\\", "/")
        script = ("import { parseIndex } from 'file:///" + lib.lstrip("/") + "';"
                  "import { readFileSync } from 'node:fs';"
                  f"const r = parseIndex(readFileSync(process.argv[1]), ['{self.pub}'], 'desktop-v0.9.2');"
                  "console.log(JSON.stringify(r));")
        r = subprocess.run([node, "--input-type=module", "-e", script, out], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(r.returncode, 0, r.stderr)
        got = json.loads(r.stdout)
        self.assertEqual((got["version"], got["size"], got["sha256"]), ("0.9.2", 12345, m["sha256"]))


if __name__ == "__main__":
    unittest.main()
