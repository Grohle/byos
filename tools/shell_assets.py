"""Genera las capas de hibyshell (formato .ov: 'HMOV', w, h (uint16), RGB565[w*h], alfa[w*h])
y la firma de la pantalla de inicio. Salida: hibymod/shell/ (se copia a /usr/data/hibymod/shell).

Dibujo a 4x y reduccion con filtro de area para bordes suaves."""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "shell")
S = 4


def save_ov(img, name):
    img = img.convert("RGBA")
    w, h = img.size
    px = list(img.getdata())
    rgb = b"".join(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) for r, g, b, a in px)
    alpha = bytes(a for r, g, b, a in px)
    with open(os.path.join(OUT, name + ".ov"), "wb") as f:
        f.write(b"HMOV" + struct.pack("<HH", w, h) + rgb + alpha)
    img.save(os.path.join(OUT, name + "_preview.png"))


def home_button():
    """Boton de cristal oscuro con una casa (estilo iOS), 44x44."""
    n = 44
    big = Image.new("RGBA", (n * S, n * S), (0, 0, 0, 0))
    # sombra
    sh = Image.new("RGBA", big.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse((4 * S, 6 * S, 40 * S, 42 * S), fill=(0, 0, 0, 110))
    sh = sh.filter(ImageFilter.GaussianBlur(3 * S))
    big.alpha_composite(sh)
    d = ImageDraw.Draw(big)
    d.ellipse((5 * S, 4 * S, 39 * S, 38 * S), fill=(58, 58, 64, 215))
    d.ellipse((5 * S, 4 * S, 39 * S, 38 * S), outline=(255, 255, 255, 60), width=S)
    # casa
    cx, cy = 22 * S, 21 * S
    roof = [(cx - 10 * S, cy - 1 * S), (cx, cy - 10 * S), (cx + 10 * S, cy - 1 * S)]
    d.line(roof, fill=(255, 255, 255, 255), width=int(2.4 * S), joint="curve")
    d.rounded_rectangle((cx - 7 * S, cy - 3 * S, cx + 7 * S, cy + 9 * S), radius=2 * S, fill=(255, 255, 255, 255))
    d.rounded_rectangle((cx - 2.5 * S, cy + 2 * S, cx + 2.5 * S, cy + 9 * S), radius=1 * S, fill=(58, 58, 64, 255))
    return big.resize((n, n), Image.LANCZOS)


def chevron():
    """Flecha hacia arriba fina con sombra, 40x14."""
    w, h = 36, 11
    big = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    pts = [(9 * S, 7.5 * S), (18 * S, 3 * S), (27 * S, 7.5 * S)]
    sh = Image.new("RGBA", big.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).line([(x, y + S) for x, y in pts], fill=(0, 0, 0, 160), width=4 * S, joint="curve")
    sh = sh.filter(ImageFilter.GaussianBlur(1.5 * S))
    big.alpha_composite(sh)
    d = ImageDraw.Draw(big)
    d.line(pts, fill=(255, 255, 255, 235), width=int(2.1 * S), joint="curve")
    for x, y in (pts[0], pts[2]):
        r = 1.05 * S
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 240))
    return big.resize((w, h), Image.LANCZOS)


def home_bar():
    """Barra de inicio de iOS: pastilla de 112x5 blanca con un contorno oscuro muy suave
    (se lee sobre pantallas claras y oscuras). Lienzo 116x9 para el contorno."""
    w, h = 116, 9
    big = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((1 * S, 1.5 * S, 115 * S, 7.5 * S), radius=3 * S, fill=(0, 0, 0, 70))
    d.rounded_rectangle((2 * S, 2 * S, 114 * S, 7 * S), radius=2.5 * S, fill=(255, 255, 255, 235))
    return big.resize((w, h), Image.LANCZOS)


def icons():
    """PNG de launcher_icons.js -> shell/icons/<app>_<tam>.ov"""
    src = os.path.join(OUT, "icons_png")
    dst = os.path.join(OUT, "icons")
    os.makedirs(dst, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(src)) if os.path.isdir(src) else []:
        if f.endswith(".png"):
            img = Image.open(os.path.join(src, f)).convert("RGBA")
            w, h = img.size
            px = list(img.getdata())
            rgb = b"".join(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) for r, g, b, a in px)
            with open(os.path.join(dst, f[:-4] + ".ov"), "wb") as o:
                o.write(b"HMOV" + struct.pack("<HH", w, h) + rgb + bytes(a for r, g, b, a in px))
            n += 1
    print("iconos:", n)


def hibyos_logo():
    """Logo de HiByOS para "Acerca de" (de theme/bootanim/logo.png, tools/bootanim.py)"""
    src = Image.open(os.path.join(HERE, "..", "theme", "bootanim", "logo.png")).convert("RGBA")
    w = 160
    save_ov(src.resize((w, round(src.height * w / src.width)), Image.LANCZOS), "hibyos_logo")


def default_wall():
    """Fondo por defecto del launcher = el del tema (Aurora), 320x480 RGB565 (filas 0-29 bajo la barra)."""
    sys.path.insert(0, os.path.join(HERE, "..", "theme"))
    import wallpaper
    wall = wallpaper.aurora()                       # 320x450, la zona bajo la barra de estado
    # mismo tramado que el tema (fase del patron desde y=30), para que encaje pixel a pixel
    full = Image.new("RGB", (320, 480))
    full.paste(wallpaper.dither565(wall).convert("RGB"), (0, 30))
    full.paste(full.crop((0, 30, 320, 60)), (0, 0))
    px = list(full.getdata())
    with open(os.path.join(OUT, "wall.raw"), "wb") as o:
        o.write(b"".join(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) for r, g, b in px))
    print("fondo por defecto: wall.raw")


def cc_icons():
    """Iconos del centro de control (26x26, blancos con alfa): shell/cc_<nombre>.ov"""
    Z = 26 * S
    def c():
        im = Image.new("RGBA", (Z, Z), (0, 0, 0, 0)); return im, ImageDraw.Draw(im)
    def P(x, y): return (x * S, y * S)
    W_ = (255, 255, 255, 255); lw = int(2.2 * S)
    out = {}
    im, d = c()                                     # enchufe: solo alimentacion USB
    d.line([P(10, 3), P(10, 8)], fill=W_, width=lw); d.line([P(16, 3), P(16, 8)], fill=W_, width=lw)
    d.rounded_rectangle([P(6, 8), P(20, 15)], 2 * S, fill=W_)
    d.pieslice([P(6, 9), P(20, 21)], 0, 180, fill=W_)
    d.line([P(13, 19), P(13, 24)], fill=W_, width=lw)
    out["plug"] = im
    im, d = c()                                     # hoja: ahorro
    lf = Image.new("RGBA", (Z, Z), (0, 0, 0, 0)); ld = ImageDraw.Draw(lf)
    ld.ellipse([P(7.5, 2), P(18.5, 24)], fill=W_)
    ld.line([P(13, 7), P(13, 26)], fill=(0, 0, 0, 0), width=int(1.5 * S))
    im = lf.rotate(-40, resample=Image.BICUBIC, center=(Z / 2, Z / 2)); d = ImageDraw.Draw(im)
    d.line([P(5, 23), P(9, 18)], fill=W_, width=int(1.8 * S))
    out["leaf"] = im
    im, d = c()                                     # indicador: normal
    d.arc([P(3, 5), P(23, 25)], 150, 390, fill=W_, width=lw)
    d.line([P(13, 15), P(18.5, 9)], fill=W_, width=lw); d.ellipse([P(11, 13), P(15, 17)], fill=W_)
    out["gauge"] = im
    im, d = c()                                     # rayo: rendimiento
    d.polygon([P(15, 2), P(6, 15), P(12, 15), P(10, 24), P(20, 10), P(14, 10), P(17, 2)], fill=W_)
    out["bolt"] = im
    im, d = c()                                     # USB: DAC
    d.line([P(13, 4), P(13, 21)], fill=W_, width=lw); d.polygon([P(13, 1.5), P(16.5, 6), P(9.5, 6)], fill=W_)
    d.line([P(13, 15), P(7, 11.5), P(7, 9)], fill=W_, width=lw); d.rectangle([P(5.5, 6.5), P(8.5, 9.5)], fill=W_)
    d.line([P(13, 17.5), P(19, 14), P(19, 11)], fill=W_, width=lw); d.ellipse([P(17, 8.5), P(21, 12.5)], fill=W_)
    d.ellipse([P(10.5, 20), P(15.5, 25)], fill=W_)
    out["usb"] = im
    im, d = c()                                     # deslizadores: panel de HiBy
    for i, (y, kx) in enumerate(((7, 16), (13, 9), (19, 14))):
        d.line([P(4, y), P(22, y)], fill=W_, width=int(1.8 * S)); d.ellipse([P(kx - 3, y - 3), P(kx + 3, y + 3)], fill=W_)
    out["sliders"] = im
    im, d = c()                                     # wifi: inalambrico
    for r, w in ((3, 0), (7.5, lw), (12, lw)):
        if not w: d.pieslice([P(13 - r, 21 - r), P(13 + r, 21 + r)], 225, 315, fill=W_)
        else: d.arc([P(13 - r, 21.5 - r), P(13 + r, 21.5 + r)], 225, 315, fill=W_, width=w)
    out["wifi"] = im
    im, d = c()                                     # carpeta: archivos
    d.rounded_rectangle([P(3, 6), P(12, 10)], 1.5 * S, fill=W_); d.rounded_rectangle([P(3, 8), P(23, 21)], 2.5 * S, fill=W_)
    out["folder"] = im
    im, d = c()                                     # engranaje: personalizar
    import math
    pts = []
    for i in range(16):
        a = i * math.pi / 8; r = 11 if i % 2 == 0 else 8.5
        pts.append(P(13 + r * math.cos(a), 13 + r * math.sin(a)))
    d.polygon(pts, fill=W_); d.ellipse([P(9.5, 9.5), P(16.5, 16.5)], fill=(0, 0, 0, 0))
    out["gear"] = im
    im, d = c()                                     # onda: filtro del DAC
    d.line([P(2, 13), P(6, 13), P(9, 5), P(13, 21), P(17, 8), P(20, 13), P(24, 13)], fill=W_, width=lw, joint="curve")
    out["wave"] = im
    im, d = c()                                     # sol: brillo
    d.ellipse([P(8.5, 8.5), P(17.5, 17.5)], fill=W_)
    for i in range(8):
        a = i * math.pi / 4
        d.line([P(13 + 7.5 * math.cos(a), 13 + 7.5 * math.sin(a)), P(13 + 11 * math.cos(a), 13 + 11 * math.sin(a))], fill=W_, width=int(2 * S))
    out["sun"] = im
    for n, im in out.items():
        save_ov(im.resize((26, 26), Image.LANCZOS), "cc_" + n)


def main():
    os.makedirs(OUT, exist_ok=True)
    save_ov(home_button(), "home_btn")
    save_ov(chevron(), "chevron")
    save_ov(home_bar(), "home_bar")
    icons()
    default_wall()
    sig = os.path.join(HERE, "..", "docs", "capturas", "_firma_inicio.txt")
    with open(sig) as f, open(os.path.join(OUT, "home.sig"), "w", newline="\n") as o:
        o.write(f.read())
    print("capas en", os.path.abspath(OUT))


if __name__ == "__main__":
    sys.exit(main())
