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


BAYER4 = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def rgb565_dither(img):
    """RGBA 8 bits -> bytes RGB565 con tramado ordenado 4x4 (evita bandas en los degradados de los iconos:
    el panel solo tiene 5/6 bits). Un color plano que ya cae en un nivel 565 exacto no se trama."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        row = BAYER4[y & 3]
        for x in range(w):
            r, g, b, a = px[x, y]
            t = (row[x & 3] + .5) / 16

            def q(v, lv):
                f = v * lv / 255
                i = int(f)
                return min(lv, i + (1 if f - i > t else 0))
            out += struct.pack("<H", (q(r, 31) << 11) | (q(g, 63) << 5) | q(b, 31))
    return bytes(out)


def icons():
    """PNG de launcher_icons.js -> shell/icons/<app>_<tam>.ov (RGB565 tramado + alfa)"""
    src = os.path.join(OUT, "icons_png")
    dst = os.path.join(OUT, "icons")
    os.makedirs(dst, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(src)) if os.path.isdir(src) else []:
        if f.endswith(".png"):
            img = Image.open(os.path.join(src, f)).convert("RGBA")
            w, h = img.size
            with open(os.path.join(dst, f[:-4] + ".ov"), "wb") as o:
                o.write(b"HMOV" + struct.pack("<HH", w, h) + rgb565_dither(img) + bytes(img.getchannel("A").tobytes()))
            n += 1
    print("iconos:", n)


def hibyos_logo():
    """Logo de ByOS para "Acerca de" (de theme/bootanim/logo.png, tools/bootanim.py)"""
    src = Image.open(os.path.join(HERE, "..", "theme", "bootanim", "logo.png")).convert("RGBA")
    w = 132
    save_ov(src.resize((w, round(src.height * w / src.width)), Image.LANCZOS), "hibyos_logo")


def default_wall():
    """Fondo por defecto del launcher = el del tema (Aurora), 320x480 RGB565 (filas 0-29 bajo la barra)."""
    sys.path.insert(0, os.path.join(HERE, "..", "theme"))
    import wallpaper
    wall = wallpaper.aurora()                       # 320x450, la zona bajo la barra de estado
    # mismo tramado que el tema (fase del patron desde y=30), para que encaje pixel a pixel
    full = Image.new("RGB", (320, 480))
    full.paste(wallpaper.dither565(wall).convert("RGB"), (0, 30))
    full.paste(full.crop((0, 30, 320, 60)).transpose(Image.FLIP_TOP_BOTTOM), (0, 0))   # reflejo: sin corte en la fila 30
    px = list(full.getdata())
    with open(os.path.join(OUT, "wall.raw"), "wb") as o:
        o.write(b"".join(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) for r, g, b in px))
    print("fondo por defecto: wall.raw")


def cc_icons():
    """Glifos del centro de control (26x26, blancos con alfa, peso tipo SF Symbols): shell/cc_<nombre>.ov
    Se dibujan en theme/launcher_icons.js (SVG) -> shell/icons_png/cc/cc_<nombre>.png.
    Incluye cc_bt (Bluetooth) y cc_buds (auriculares) ademas de los 11 que usa el centro de control."""
    src = os.path.join(OUT, "icons_png", "cc")
    if not os.path.isdir(src):
        print("sin shell/icons_png/cc: ejecuta theme/launcher_icons.js"); return
    n = 0
    for f in sorted(os.listdir(src)):
        if f.startswith("cc_") and f.endswith(".png"):
            im = Image.open(os.path.join(src, f)).convert("RGBA")
            a = im.getchannel("A")                       # glifo blanco puro: solo cuenta el alfa
            white = Image.new("RGBA", im.size, (255, 255, 255, 255)); white.putalpha(a)
            save_ov(white, f[:-4]); n += 1
    print("glifos cc:", n)


def main():
    os.makedirs(OUT, exist_ok=True)
    save_ov(home_button(), "home_btn")
    save_ov(chevron(), "chevron")
    save_ov(home_bar(), "home_bar")
    icons()
    cc_icons()
    default_wall()
    sig = os.path.join(HERE, "..", "docs", "capturas", "_firma_inicio.txt")
    with open(sig) as f, open(os.path.join(OUT, "home.sig"), "w", newline="\n") as o:
        o.write(f.read())
    print("capas en", os.path.abspath(OUT))


if __name__ == "__main__":
    sys.exit(main())
