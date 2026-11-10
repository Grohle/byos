#!/usr/bin/env python3
"""Barra de estado de HiByOS: glifos nuevos para litegui/themeN/topbar y topbar.view.

La barra de serie es solo imagenes (digitos, iconos de 20 px de alto) colocadas por
layout/themeN/topbar/topbar.view, asi que redibujandolas se cambia en TODAS las
pantallas (las de HiBy y, por transparencia, el launcher y las apps de hibyshell).

Reglas de diseno (GUIA_UI_AGENTES.md): misma fuente que el shell (default.otf),
cifras tabulares, todo centrado en la misma linea (y=15 de la barra), iconos de 12 px
de alto, sin cajas de codec, hora a la izquierda como en iOS.

Tambien genera src/digits.h (plantillas del OCR de volumen) a partir de las cifras
nuevas. Uso: python topbar.py [--deploy]
"""
import os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont
import viewlib as V

HERE = os.path.dirname(os.path.abspath(__file__))
OVL = os.path.join(HERE, "..", "overlay", "usr", "resource")
STOCK = r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource"
FONT = os.path.join(STOCK, "fonts", "default.otf")
ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = os.environ.get("HIBY_DEV", "HiBy R3II_2025")
S = 4                                   # superm uestreo
WHITE, RED, GREEN, GREY = (255, 255, 255), (255, 69, 58), (48, 209, 88), (142, 142, 147)
CY = 10.0                               # centro vertical dentro de la celda de 20 px
files = {}                              # nombre -> Image


def canvas(w):
    im = Image.new("RGBA", (w * S, 20 * S), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def done(name, im):
    files[name] = im.resize((im.width // S, im.height // S), Image.LANCZOS)


def font(px):
    return ImageFont.truetype(FONT, int(px * S))


# ---- cifras tabulares (celda 10x20), tambien en rojo ("r": bateria baja) ----------
F = font(15)
def glyph(ch, w, col, f=F, dy=0.0, bold=2):
    im, d = canvas(w)
    d.text((w * S / 2, (CY + dy) * S), ch, font=f, fill=col + (255,), anchor="mm", stroke_width=bold, stroke_fill=col + (255,))
    return im

for n in "0123456789":
    done(n, glyph(n, 10, WHITE)); done(n + "r", glyph(n, 10, RED))
done("colon", glyph(":", 6, WHITE, dy=-0.8))
done("dot", glyph(".", 10, WHITE)); done("dotr", glyph(".", 10, RED))
done("-", glyph("-", 10, WHITE)); done("slash", glyph("/", 10, WHITE))
done("percent", glyph("%", 12, WHITE, f=font(11), dy=0.6))
done("L", glyph("L", 10, WHITE, f=font(12))); done("R", glyph("R", 10, WHITE, f=font(12)))
done("am", glyph("AM", 18, WHITE, f=font(10))); done("pm", glyph("PM", 18, WHITE, f=font(10)))


# ---- iconos (12 px de alto, trazo 1.6 px) -------------------------------------
LW = 1.6 * S
def P(x, y): return (x * S, y * S)

def speaker():
    im, d = canvas(20)
    d.polygon([P(3, 8), P(6, 8), P(10, 4.5), P(10, 15.5), P(6, 12), P(3, 12)], fill=WHITE)
    d.arc([P(9, 6.5), P(14, 13.5)], -55, 55, fill=WHITE, width=int(LW))
    d.arc([P(9.5, 4), P(17.5, 16)], -55, 55, fill=WHITE, width=int(LW))
    return im

def headphones():
    im, d = canvas(20)
    d.arc([P(3.5, 4), P(16.5, 18)], 180, 360, fill=WHITE, width=int(LW))
    d.line([P(4.3, 11), P(4.3, 13)], fill=WHITE, width=int(LW)); d.line([P(15.7, 11), P(15.7, 13)], fill=WHITE, width=int(LW))
    d.rounded_rectangle([P(3, 10.5), P(6.5, 16)], 1.3 * S, fill=WHITE)
    d.rounded_rectangle([P(13.5, 10.5), P(17, 16)], 1.3 * S, fill=WHITE)
    return im

def label(t, w=20, sz=8.5):
    im, d = canvas(w)
    d.rounded_rectangle([P(1, 4.5), P(w - 1, 15.5)], 2.5 * S, outline=WHITE, width=int(1.2 * S))
    d.text((w * S / 2, CY * S), t, font=font(sz), fill=WHITE, anchor="mm")
    return im

def usb():
    im, d = canvas(20)
    d.line([P(10, 4), P(10, 16)], fill=WHITE, width=int(LW))
    d.polygon([P(10, 3), P(12, 6), P(8, 6)], fill=WHITE)
    d.line([P(10, 12), P(6, 9.5), P(6, 7.5)], fill=WHITE, width=int(LW))
    d.line([P(10, 13.5), P(14, 11), P(14, 9)], fill=WHITE, width=int(LW))
    d.ellipse([P(8.3, 14.3), P(11.7, 17.4)], fill=WHITE)
    return im

def play():
    im, d = canvas(20)
    d.polygon([P(7, 5), P(15, 10), P(7, 15)], fill=WHITE)
    return im

def pause():
    im, d = canvas(20)
    d.rounded_rectangle([P(6, 5), P(8.8, 15)], 0.8 * S, fill=WHITE); d.rounded_rectangle([P(11.2, 5), P(14, 15)], 0.8 * S, fill=WHITE)
    return im

def wifi(level):
    im, d = canvas(20)
    for i, r in enumerate((2.6, 6.2, 9.8)):
        on = i < level
        col = WHITE + (255,) if on else (255, 255, 255, 90)
        if i == 0:
            d.pieslice([P(10 - r, 15 - r), P(10 + r, 15 + r)], 225, 315, fill=col)
        else:
            d.arc([P(10 - r, 15.5 - r), P(10 + r, 15.5 + r)], 225, 315, fill=col, width=int(2.0 * S))
    return im

def bluetooth():
    im, d = canvas(14)
    d.line([P(4, 7.5), P(10, 12.5), P(7, 15), P(7, 5), P(10, 7.5), P(4, 12.5)], fill=WHITE, width=int(1.4 * S), joint="curve")
    return im

def link():
    im, d = canvas(18)
    d.rounded_rectangle([P(2.5, 7.5), P(10.5, 12.5)], 2.5 * S, outline=WHITE, width=int(1.5 * S))
    d.rounded_rectangle([P(7.5, 7.5), P(15.5, 12.5)], 2.5 * S, outline=WHITE, width=int(1.5 * S))
    return im

def lock():
    im, d = canvas(18)
    d.arc([P(6, 4), P(12, 11)], 180, 360, fill=WHITE, width=int(1.5 * S))
    d.line([P(6, 7.5), P(6, 9)], fill=WHITE, width=int(1.5 * S)); d.line([P(12, 7.5), P(12, 9)], fill=WHITE, width=int(1.5 * S))
    d.rounded_rectangle([P(4.5, 9), P(13.5, 16)], 1.5 * S, fill=WHITE)
    return im

blank = lambda w: canvas(w)[0]

# pila vertical 14x20 (misma geometria que la de serie: el relleno se recorta por nivel)
def battery_bg(col):
    im, d = canvas(14)
    d.rounded_rectangle([P(5, 2.4), P(9, 4.2)], 0.6 * S, fill=col)                 # borne
    d.rounded_rectangle([P(1.6, 3.6), P(12.4, 17.6)], 2.8 * S, outline=col, width=int(1.3 * S))
    return im

def battery_fill():
    im, d = canvas(14)
    d.rounded_rectangle([P(3.4, 5.4), P(10.6, 15.8)], 1.4 * S, fill=WHITE)
    return im

done("speaker", speaker()); done("po", headphones())
done("lo", label("LO")); done("balance", label("4.4")); done("spdif", label("OPT"))
done("usb", usb()); done("a2dp", label("BT")); done("adb", label("ADB", 22, 7.5).crop((0, 0, 20 * S, 20 * S)))
done("play", play()); done("pause", pause())
for i in range(4): done(f"wifi_connect_{i}", wifi(i if i else 1))
done("wifi_unconnect", blank(20))                       # como iOS: sin Wi-Fi no hay icono
done("bluetooth", bluetooth()); done("bluetooth_unconnect", blank(14))
done("hl_connect", link()); done("hl_disconnect", blank(18))
done("chage_lock", lock())
done("battery_bg", battery_bg(WHITE + (235,))); done("battery_charge_bg", battery_bg(GREEN + (255,)))
done("battery_low_bg", battery_bg(RED + (255,))); done("battery", battery_fill())

# ---- disposicion (topbar.view): hora a la izquierda, estado a la derecha -----------
POS = {
    "topbar_nv_clock": 14, "topbar_iv_play_status": 64,
    "topbar_iv_speaker": 124, "topbar_nv_speaker": 144, "topbar_iv_output": 172,
    "topbar_iv_hl": 194, "topbar_iv_bt": 212, "topbar_iv_net": 228,
    "topbar_nv_battery": 252, "topbar_iv_battery": 298,
}

out = []
for th in ("theme2", "theme3"):
    for n, im in files.items():
        rel = f"litegui/{th}/topbar/{n}.png"
        p = os.path.join(OVL, rel); os.makedirs(os.path.dirname(p), exist_ok=True); im.save(p); out.append((rel, p))
    root = V.load(os.path.join(STOCK, "layout", th, "topbar", "topbar.view"))
    for _k, el in V.walk(root):
        n = V.get(el, "name")
        if n in POS: V.set_(el, "x", POS[n]); V.set_(el, "y", 5)
    rel = f"layout/{th}/topbar/topbar.view"; p = os.path.join(OVL, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True); V.save(p, root); out.append((rel, p))

# vista previa ampliada (sobre negro)
prev = Image.new("RGB", (320, 30), (0, 0, 0))
seq = [("1", 14), ("2", 24), ("colon", 34), ("4", 40), ("5", 50), ("pause", 64), ("speaker", 124), ("2", 144), ("5", 154),
       ("po", 172), ("wifi_connect_3", 228), ("1", 252), ("0", 262), ("0", 272), ("percent", 282), ("battery_bg", 298), ("battery", 298)]
for n, x in seq: prev.paste(files[n], (x, 5), files[n])
prev.resize((960, 90), Image.NEAREST).save(os.path.join(HERE, "out", "topbar_preview.png"))

# ---- plantillas del OCR de volumen (src/digits.h) -----------------------------
rows_h = []
with open(os.path.join(HERE, "..", "src", "digits.h"), "w", newline="\n") as f:
    f.write("/* digits.h: cifras de la barra de estado de HiByOS (generado por theme/topbar.py) */\n")
    f.write("struct dtpl { int w, h; const char *rows[16]; };\nstatic const struct dtpl DIGITS[10] = {\n")
    for n in "0123456789":
        a = files[n].getchannel("A").point(lambda v: 255 if v > 140 else 0)
        bb = a.getbbox(); g = a.crop(bb)
        rows = ['"' + "".join("#" if g.getpixel((x, y)) else "." for x in range(g.width)) + '"' for y in range(g.height)]
        rows_h.append(g.height)
        f.write(f"    {{ {g.width}, {g.height}, {{ {', '.join(rows)} }} }},   /* {n} */\n")
    f.write("};\n")
print("cifras: alto", min(rows_h), "-", max(rows_h), "px;", len(out), "ficheros")

if "--deploy" in sys.argv:
    for rel, src in out:
        dst = "/usr/data/hibymod/theme/" + rel
        subprocess.run([ADB, "-s", DEV, "shell", f"mkdir -p '{os.path.dirname(dst)}'"], check=True)
        subprocess.run([ADB, "-s", DEV, "push", src, dst], check=True, capture_output=True)
    print("desplegado en", len(out), "rutas")
