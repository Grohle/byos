#!/usr/bin/env python3
"""Capturas para el README: ejecuta pasos (gestos por ADB) y guarda la pantalla VISIBLE.

Uso: python readme_shots.py PASO [PASO...]
  PASO = nombre:accion   accion = tap,X,Y | swipe,X1,Y1,X2,Y2,MS | hold,X,Y,MS | wait,MS | shot
Ej.:  python readme_shots.py "home1:shot" "x:swipe,260,250,60,250,200" "home2:shot"
Las capturas van a docs/media/shots/<nombre>.png (320x480).
Necesita /usr/data/hibymod/t/hibyctl (lee el bufer visible, no el 0 como /dev/fb0)."""
import os, struct, subprocess, sys, time
from PIL import Image

ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = os.environ.get("HIBY_DEV", "HiBy R3II_2025")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "media", "shots")
os.makedirs(OUT, exist_ok=True)


def sh(c):
    return subprocess.run([ADB, "-s", DEV, "shell", c], capture_output=True, text=True).stdout


def shot(name):
    sh("/usr/data/hibymod/t/hibyctl shot /usr/data/_s.raw >/dev/null 2>&1")
    tmp = os.path.join(OUT, "_s.raw")
    subprocess.run([ADB, "-s", DEV, "pull", "/usr/data/_s.raw", tmp], capture_output=True)
    sh("rm -f /usr/data/_s.raw")
    raw = open(tmp, "rb").read(); os.remove(tmp)
    px = struct.unpack("<%dH" % (320 * 480), raw[:320 * 480 * 2])
    im = Image.new("RGB", (320, 480))
    im.putdata([((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
    p = os.path.join(OUT, name + ".png"); im.save(p); print(p)


for step in sys.argv[1:]:
    name, act = step.split(":", 1)
    a = act.split(",")
    if a[0] == "shot": shot(name)
    elif a[0] == "wait": time.sleep(int(a[1]) / 1000)
    elif a[0] == "tap": sh(f"hibyctl tap {a[1]} {a[2]}"); time.sleep(1.0)
    elif a[0] == "swipe": sh("hibyctl swipe " + " ".join(a[1:])); time.sleep(1.0)
    elif a[0] == "hold": sh(f"hibyctl hold {a[1]} {a[2]} {a[3]}"); time.sleep(0.8)
