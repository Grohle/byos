#!/usr/bin/env python3
"""Control remoto del HiBy R3 II por ADB Wi-Fi: toques, capturas y hojas de contacto.

Uso desde la linea de comandos (las acciones se ejecutan en orden):
  python hiby.py tap:80,110 wait:1.2 shot:musica swipe:160,400,160,120 shot:musica2 sheet:tour1
Acciones: tap:X,Y  hold:X,Y,MS  swipe:X1,Y1,X2,Y2[,MS]  key:CODE[,eventN]  back  wait:S  shot:NOMBRE  sheet:NOMBRE
"""
import os, struct, subprocess, sys, time
from PIL import Image, ImageDraw

ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = os.environ.get("HIBY_DEV", "192.168.1.147:5555")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "capturas")
os.makedirs(OUT, exist_ok=True)
CTL = "/tmp/hibyctl"
LOCAL_CTL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bin", "hibyctl")
session = []


def sh(cmd):
    return subprocess.run([ADB, "-s", DEV, "shell", cmd], capture_output=True, text=True).stdout


def ensure_ctl():
    if "ok" not in sh(f"[ -x {CTL} ] && echo ok"):
        subprocess.run([ADB, "-s", DEV, "push", LOCAL_CTL, CTL], capture_output=True)
        sh(f"chmod +x {CTL}")


def shot(name):
    sh(f"{CTL} shot /tmp/s.raw 2>/dev/null")
    tmp = os.path.join(OUT, name + ".raw")
    subprocess.run([ADB, "-s", DEV, "pull", "/tmp/s.raw", tmp], capture_output=True)
    raw = open(tmp, "rb").read(); os.remove(tmp)
    px = struct.unpack("<%dH" % (320 * 480), raw[:320 * 480 * 2])
    img = Image.new("RGB", (320, 480))
    img.putdata([(((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
    path = os.path.join(OUT, name + ".png")
    img.save(path)
    session.append((name, path))
    print("captura:", name)


REF_HOME = os.path.join(OUT, "_ref_inicio.png")


def grab():
    """Captura a memoria (sin guardar) y devuelve la imagen."""
    sh(f"{CTL} shot /tmp/s.raw 2>/dev/null")
    tmp = os.path.join(OUT, "_tmp.raw")
    subprocess.run([ADB, "-s", DEV, "pull", "/tmp/s.raw", tmp], capture_output=True)
    raw = open(tmp, "rb").read(); os.remove(tmp)
    px = struct.unpack("<%dH" % (320 * 480), raw[:320 * 480 * 2])
    img = Image.new("RGB", (320, 480))
    img.putdata([(((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
    return img


HOME_SIG = os.path.join(OUT, "_firma_inicio.txt")   # "x y r g b tolerancia" por linea


def is_home(img):
    # firma por puntos si existe (robusta frente a cambios del resto de la pantalla)
    if os.path.exists(HOME_SIG):
        im = img.convert("RGB")
        for line in open(HOME_SIG):
            p = line.split()
            if len(p) < 6:
                continue
            x, y, r, g, b, tol = map(int, p[:6])
            pr, pg, pb = im.getpixel((x, y))
            if abs(pr - r) > tol or abs(pg - g) > tol or abs(pb - b) > tol:
                return False
        return True
    if not os.path.exists(REF_HOME):
        return False
    ref = Image.open(REF_HOME).convert("L").crop((0, 30, 320, 480)).resize((80, 112))
    cur = img.convert("L").crop((0, 30, 320, 480)).resize((80, 112))
    diff = sum(abs(a - b) for a, b in zip(ref.getdata(), cur.getdata())) / (80 * 112)
    return diff < 6


def go_home():
    for _ in range(8):
        if is_home(grab()):
            return True
        sh(f"{CTL} tap 22 48"); time.sleep(1.0)
    print("AVISO: no he podido volver al inicio")
    return False


def sheet(name, cols=4):
    if not session:
        return
    rows = (len(session) + cols - 1) // cols
    W, H = 330, 505
    o = Image.new("RGB", (cols * W, rows * H), (28, 28, 30))
    d = ImageDraw.Draw(o)
    for i, (n, p) in enumerate(session):
        x, y = (i % cols) * W, (i // cols) * H
        o.paste(Image.open(p), (x + 5, y + 22))
        d.text((x + 8, y + 5), n, fill=(230, 230, 230))
    path = os.path.join(OUT, name + ".png")
    o.save(path)
    print("hoja:", path)
    session.clear()


def main(actions):
    ensure_ctl()
    for a in actions:
        k, _, v = a.partition(":")
        p = v.split(",") if v else []
        if k == "tap":    sh(f"{CTL} tap {p[0]} {p[1]}"); time.sleep(1.2)
        elif k == "hold": sh(f"{CTL} hold {p[0]} {p[1]} {p[2]}"); time.sleep(1.2)
        elif k == "swipe": sh(f"{CTL} swipe {' '.join(p)}"); time.sleep(1.2)
        elif k == "key":  sh(f"{CTL} key {' '.join(p)}"); time.sleep(1.2)
        elif k == "back": sh(f"{CTL} tap 22 48"); time.sleep(1.2)
        elif k == "home": go_home()
        elif k == "refhome": grab().save(REF_HOME); print("referencia de inicio guardada")
        elif k == "wait": time.sleep(float(v))
        elif k == "shot": shot(v)
        elif k == "sheet": sheet(v)
        else: print("accion desconocida:", a)


if __name__ == "__main__":
    main(sys.argv[1:])
