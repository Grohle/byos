#!/usr/bin/env python3
"""mk_testrepo.py OUTDIR HOLA_HPK [N]

Builds a local store repository for the VM harness (docs/VM_SHELL.md): OUTDIR/index.json,
OUTDIR/icons/*.png and OUTDIR/hola.hpk. It is served over plain HTTP from the host
(python3 -m http.server) and the guest reaches it at http://10.0.2.2:PORT/index.json.

- org.hibymod.hola: the real demo package (installable; sha256/size of HOLA_HPK).
- N filler apps (org.vmtest.appNN, default 24) with generated icons, so the lists are long
  enough to scroll. Every 6th is featured (Today), every 7th is "available": false (IN TESTING).
  Their hpk points at a missing file: GET fails cleanly with "Download failed".
Pure standard library (no PIL)."""
import hashlib
import json
import os
import shutil
import struct
import sys
import zlib


def png(path, size, rgb1, rgb2, mark):
    """square PNG with a diagonal gradient and a light disc (so the tint/mask are visible)"""
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            t = (x + y) / (2 * size - 2)
            r, g, b = (int(rgb1[i] + (rgb2[i] - rgb1[i]) * t) for i in range(3))
            dx, dy = x - size * 0.5, y - size * 0.42
            if dx * dx + dy * dy < (size * (0.16 + 0.02 * (mark % 4))) ** 2:
                r, g, b = 250, 250, 250
            row += bytes((r, g, b, 255))
        rows.append(bytes(row))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def main():
    out, hola = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    os.makedirs(os.path.join(out, "icons"), exist_ok=True)
    shutil.copyfile(hola, os.path.join(out, "hola.hpk"))
    blob = open(hola, "rb").read()
    apps = [{
        "id": "org.hibymod.hola", "version": "1.0.0", "min_hibymod": "0.1.0",
        "name": {"es": "Hola", "en": "Hello", "fr": "Bonjour"},
        "desc": {"es": "Demo: saludo, degradado y un círculo que sigue al dedo",
                 "en": "Demo: greeting, gradient and a circle that follows your finger",
                 "fr": "Démo : salutation, dégradé et un cercle qui suit le doigt"},
        "author": "hibymod", "category": "demo", "ram_kb": 900, "stop_player": False, "featured": True,
        "size": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
        "icon": "icons/org.hibymod.hola.png", "hpk": "hola.hpk"}]
    png(os.path.join(out, "icons", "org.hibymod.hola.png"), 128, (255, 149, 0), (255, 59, 48), 1)
    palette = [((10, 132, 255), (94, 92, 230)), ((48, 209, 88), (0, 122, 80)), ((255, 55, 95), (175, 82, 222)),
               ((255, 214, 10), (255, 149, 0)), ((100, 210, 255), (0, 90, 200)), ((142, 142, 147), (44, 44, 48))]
    cats = ["music", "tools", "games", "radio", "books", "utilities"]
    for i in range(1, n + 1):
        aid = "org.vmtest.app%02d" % i
        c1, c2 = palette[i % len(palette)]
        png(os.path.join(out, "icons", aid + ".png"), 128, c1, c2, i)
        a = {"id": aid, "version": "1.%d.0" % i, "min_hibymod": "0.1.0",
             "name": {"es": "Prueba %d" % i, "en": "Test app %d" % i, "fr": "App test %d" % i},
             "desc": {"es": "Aplicación de relleno número %d para probar el desplazamiento de la tienda" % i,
                      "en": "Filler application number %d to exercise scrolling in the store" % i,
                      "fr": "Application de remplissage n° %d pour tester le défilement" % i},
             "author": "VM harness", "category": cats[i % len(cats)], "ram_kb": 300 + 50 * i,
             "featured": i % 6 == 0, "size": 40960 + i * 1024, "sha256": "0" * 64,
             "icon": "icons/%s.png" % aid, "hpk": "missing/%s.hpk" % aid}
        if i % 7 == 0:
            a["available"] = False
            del a["hpk"], a["sha256"]
        apps.append(a)
    idx = {"format": 1, "name": {"es": "Pruebas VM", "en": "VM tests", "fr": "Tests VM"},
           "homepage": "http://10.0.2.2/", "apps": apps}
    json.dump(idx, open(os.path.join(out, "index.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("test repo: %d apps in %s" % (len(apps), out))


if __name__ == "__main__":
    main()
