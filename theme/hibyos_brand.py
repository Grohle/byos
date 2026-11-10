#!/usr/bin/env python3
"""Marca HiByOS en los recursos de fabrica (litegui theme2/theme3 y raiz).

 - boot_animation/en/0.png: fotograma de reposo de la animacion (identico al ultimo
   que deja hibyboot, para que el relevo al reproductor no tenga salto).
 - Acerca de (hiby_about_dev.view): logo de HiByOS, sin redes sociales, email ni web
   de HiBy; un unico boton "GitHub" que abre el QR del repositorio.
 - etc/logo*.jpeg: logo fijo de respaldo (si hibyboot no puede arrancar).

Escribe en ../overlay (rootfs) y, con --deploy, en /usr/data/hibymod/theme del aparato.
Uso: python hibyos_brand.py [--deploy]
"""
import os, subprocess, sys
import qrcode
from PIL import Image, ImageDraw, ImageFont
import viewlib as V

HERE = os.path.dirname(os.path.abspath(__file__))
OVL = os.path.join(HERE, "..", "overlay")
STOCK = r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource"
BA = os.path.join(HERE, "bootanim")
REPO = "https://github.com/Grohle/hibyos"
ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = os.environ.get("HIBY_DEV", "HiBy R3II_2025")
out = []   # (ruta relativa a /usr/resource/litegui, fichero local)


def put(rel, img_or_path, fmt="PNG"):
    dst = os.path.join(OVL, "usr", "resource", "litegui", rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if isinstance(img_or_path, Image.Image):
        img_or_path.save(dst, fmt)
    else:
        with open(img_or_path, "rb") as s, open(dst, "wb") as d:
            d.write(s.read())
    out.append((rel.replace("\\", "/"), dst))


def font(sz):
    for f in ("segoeuisb.ttf", "seguisb.ttf", "arialbd.ttf"):
        try:
            return ImageFont.truetype(f, sz)
        except OSError:
            pass
    return ImageFont.load_default()


rest = Image.open(os.path.join(BA, "last.png")).convert("RGB")
logo = Image.open(os.path.join(BA, "logo.png")).convert("RGBA")
LW = 200
logo_s = logo.resize((LW, round(logo.height * LW / logo.width)), Image.LANCZOS)

# boton GitHub (pildora oscura) y QR del repositorio
btn = Image.new("RGBA", (150, 44), (0, 0, 0, 0))
d = ImageDraw.Draw(btn)
d.rounded_rectangle((0, 0, 149, 43), 22, fill=(44, 44, 46, 255))
q = qrcode.QRCode(border=0, box_size=1); q.add_data(REPO); q.make()
mini = q.make_image(fill_color="white", back_color=(44, 44, 46)).convert("RGBA").resize((22, 22), Image.NEAREST)
btn.alpha_composite(mini, (18, 11))
d.text((50, 22), "GitHub", font=font(17), fill=(255, 255, 255, 255), anchor="lm")
q = qrcode.QRCode(border=2, box_size=4, error_correction=qrcode.constants.ERROR_CORRECT_M); q.add_data(REPO); q.make()
qr = q.make_image(fill_color="black", back_color="white").convert("RGBA").resize((168, 168), Image.NEAREST)

for th in ("", "theme2/", "theme3/"):
    put(th + "boot_animation/en/0.png", rest if th == "" else rest.convert("RGBA"))
    if not th:
        continue
    put(th + "about_dev/logo.png", logo_s)
    put(th + "about_dev/wechat.png", btn)
    put(th + "about_dev/wechat_qrcode.png", qr)
for vrel in ("layout/theme2/hiby_about_dev.view", "layout/theme3/hiby_about_dev.view", "litegui/theme3/hiby_about_dev.view"):
    th = vrel.split("/")[1] + "/"
    root = V.load(os.path.join(STOCK, vrel))
    hide = {"about_dev_iv_facebook", "about_dev_iv_microblog", "about_dev_tv_email_attr",
            "about_dev_tv_official_website_attr"}
    for _k, el in V.walk(root):
        n = V.get(el, "name")
        if n in hide:   # oculto y a 1x1 en una esquina (x negativa rompe el toque)
            V.set_(el, "visible", "hide"); V.set_(el, "x", 0); V.set_(el, "y", 0)
            if V.get(el, "w") is not None: V.set_(el, "w", 1); V.set_(el, "h", 1)
        elif n == "about_dev_iv_icon":
            V.set_(el, "x", (320 - LW) // 2); V.set_(el, "y", 44)
        elif n == "about_dev_tv_model":
            V.set_(el, "y", 128)
        elif n == "about_dev_tv_capacity_val":
            V.set_(el, "y", 160)
        elif n == "about_dev_tv_serial_number_attr":
            V.set_(el, "y", 192)
        elif n == "about_dev_iv_wechat":
            V.set_(el, "x", (320 - 150) // 2); V.set_(el, "y", 240)
    dst = os.path.join(OVL, "usr", "resource", vrel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    V.save(dst, root)
    out.append(("@" + vrel, dst))

# logo fijo de arranque (respaldo de hibyboot): JPEG baseline 320x480
for n in ("logo.jpeg", "logo1.jpeg", "logo2.jpeg"):
    p = os.path.join(OVL, "etc", n); os.makedirs(os.path.dirname(p), exist_ok=True)
    rest.save(p, "JPEG", quality=93, optimize=False, progressive=False)
print("\n".join(r for r, _ in out))

if "--deploy" in sys.argv:
    for rel, src in out:
        if rel.startswith("@"):
            dst = "/usr/data/hibymod/theme/" + rel[1:]
        elif rel.startswith("theme"):
            dst = "/usr/data/hibymod/theme/litegui/" + rel
        else:
            continue
        subprocess.run([ADB, "-s", DEV, "shell", f"mkdir -p '{os.path.dirname(dst)}'"], check=True)
        subprocess.run([ADB, "-s", DEV, "push", src, dst], check=True, capture_output=True)
        print("deploy", dst)
