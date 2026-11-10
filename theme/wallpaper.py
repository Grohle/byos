#!/usr/bin/env python3
"""Fondo de pantalla del inicio + dock de cristal prerenderizado.

  python wallpaper.py OUT_DIR [imagen]      (sin imagen: fondo "Aurora" por defecto)

Genera en OUT_DIR:
  wallpaper.png   320x450 (zona del inicio bajo la barra de estado), opaco
  dock.png        cristal: el trozo de fondo bajo el dock, desenfocado y aclarado, con
                  esquinas redondeadas y brillo de borde. Es una imagen fija, asi que el
                  "liquid glass" no cuesta nada al dibujar.
Ambas tramadas (ordered dithering) a RGB565 para que los degradados no hagan bandas.
"""
import sys, os
from PIL import Image, ImageDraw, ImageFilter, ImageOps, ImageEnhance

W, H = 320, 450                      # area del inicio (la barra de estado ocupa 30 px)
DOCK = (10, 350, 300, 92)            # x, y, w, h relativos al area del inicio
RADIUS = 28

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def dither565(img):
    """Tramado ordenado 4x4 a 5-6-5 bits; devuelve RGB(A) de 8 bits ya cuantizado."""
    img = img.convert("RGBA")
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            t = (BAYER[y & 3][x & 3] + 0.5) / 16.0
            def q(v, bits):
                levels = (1 << bits) - 1
                f = v / 255.0 * levels
                i = int(f) + (1 if (f - int(f)) > t else 0)
                return round(min(levels, i) * 255 / levels)
            px[x, y] = (q(r, 5), q(g, 6), q(b, 5), a)
    return img


def aurora():
    base = Image.new("RGB", (W, H), (5, 6, 26))
    layer = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    for (cx, cy, rad, col) in [(0.15, 0.22, 230, (59, 91, 255)), (0.92, 0.55, 200, (0, 212, 176)),
                               (0.25, 0.86, 230, (164, 59, 255)), (0.85, 0.08, 160, (255, 79, 163))]:
        x, y = cx * W, cy * H
        d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=col)
    layer = layer.filter(ImageFilter.GaussianBlur(70))
    return Image.blend(base, layer, 0.78)


def from_image(path):
    im = Image.open(path).convert("RGB")
    return ImageOps.fit(im, (W, H), Image.LANCZOS)


def glass(wall):
    x, y, w, h = DOCK
    region = wall.crop((x - 20, y - 20, x + w + 20, y + h + 20)).filter(ImageFilter.GaussianBlur(14))
    region = region.crop((20, 20, 20 + w, 20 + h))
    region = ImageEnhance.Brightness(region).enhance(1.12)
    white = Image.new("RGB", (w, h), (255, 255, 255))
    g = Image.blend(region, white, 0.16).convert("RGBA")
    # brillo superior y borde fino (como Liquid Glass)
    hl = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dh = ImageDraw.Draw(hl)
    for i in range(h // 2):
        dh.line((0, i, w, i), fill=(255, 255, 255, int(46 * (1 - i / (h / 2)))))
    g = Image.alpha_composite(g, hl)
    rim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle((0, 0, w - 1, h - 1), RADIUS, outline=(255, 255, 255, 70), width=1)
    g = Image.alpha_composite(g, rim)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), RADIUS, fill=255)
    g.putalpha(mask)
    return g


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    wall = from_image(sys.argv[2]) if len(sys.argv) > 2 else aurora()
    dither565(wall).convert("RGB").save(os.path.join(out, "wallpaper.png"), optimize=True)
    dither565(glass(wall)).save(os.path.join(out, "dock.png"), optimize=True)
    print("fondo y dock generados en", out)


if __name__ == "__main__":
    main()
