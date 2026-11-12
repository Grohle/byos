#!/usr/bin/env python3
"""Draw the ByOS Desktop icon (the player of desktop/ui/icon.svg) and write byos.ico next to this file.

    python desktop/windows/make_icon.py
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(t):
    # #5ac8fa -> #af52de (at 0.6) -> #ff2d55, along the diagonal (as the SVG)
    c0, c1, c2 = (0x5A, 0xC8, 0xFA), (0xAF, 0x52, 0xDE), (0xFF, 0x2D, 0x55)
    return lerp(c0, c1, t / 0.6) if t < 0.6 else lerp(c1, c2, (t - 0.6) / 0.4)


def draw(size):
    s = 4 * size                       # draw big, then scale down (anti-aliasing)
    k = s / 64.0
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([14 * k, 2 * k, 50 * k, 62 * k], radius=9 * k, fill=(0x2C, 0x2C, 0x30, 255))
    # screen with the gradient
    x0, y0, x1, y1 = int(18 * k), int(6 * k), int(46 * k), int(30 * k)
    g = Image.new("RGBA", (x1 - x0, y1 - y0))
    gp = g.load()
    w, h = g.size
    for y in range(h):
        for x in range(w):
            gp[x, y] = gradient((x / max(1, w - 1) + y / max(1, h - 1)) / 2) + (255,)
    m = Image.new("L", g.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], radius=4 * k, fill=255)
    im.paste(g, (x0, y0), m)
    r = 9 * k
    d.ellipse([32 * k - r, 46 * k - r, 32 * k + r, 46 * k + r], outline=(0x8E, 0x8E, 0x93, 255), width=max(1, int(2.5 * k)))
    return im.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    big = draw(256)
    out = os.path.join(HERE, "byos.ico")
    big.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    # the app window's icon (Edge app mode takes it from the page: favicon PNGs and the manifest)
    ui = os.path.join(os.path.dirname(HERE), "ui")
    for n in (32, 192, 512):
        (big if n <= 256 else draw(512)).resize((n, n), Image.LANCZOS).save(os.path.join(ui, "icon-%d.png" % n))
    print(out, os.path.getsize(out))
