"""Coleccion de fondos para la carpeta Pictures de la SD (estilo iOS: degradados de malla
suaves con manchas de color desenfocadas). 320x480 = resolucion exacta de la pantalla.
  python wallpapers.py SALIDA_DIR"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

W, H = 320, 480


def mesh(base, blobs, blur=70, grain=0):
    big = Image.new("RGB", (W * 2, H * 2), base)
    d = ImageDraw.Draw(big)
    for (cx, cy, r, col) in blobs:
        d.ellipse(((cx - r) * 2, (cy - r) * 2, (cx + r) * 2, (cy + r) * 2), fill=col)
    big = big.filter(ImageFilter.GaussianBlur(blur * 2))
    return big.resize((W, H), Image.LANCZOS)


def vertical(top, bottom):
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        t = y / (H - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(W):
            px[x, y] = c
    return im


WALLS = {
    "Negro": lambda: Image.new("RGB", (W, H), (0, 0, 0)),
    "Aurora": lambda: mesh((40, 20, 90), [(60, 80, 160, (230, 60, 160)), (280, 140, 150, (90, 70, 240)), (120, 380, 190, (40, 120, 255)), (300, 420, 130, (170, 60, 220))]),
    "Medianoche": lambda: mesh((8, 10, 30), [(80, 120, 170, (40, 40, 140)), (260, 360, 190, (90, 30, 120)), (200, 60, 110, (20, 70, 140))], blur=80),
    "Oceano": lambda: mesh((5, 30, 60), [(70, 90, 170, (0, 140, 200)), (270, 260, 170, (0, 90, 170)), (120, 430, 160, (0, 190, 170))]),
    "Brasa": lambda: mesh((60, 10, 10), [(90, 100, 160, (255, 120, 40)), (260, 250, 170, (220, 40, 60)), (130, 420, 170, (255, 170, 60))]),
    "Bosque": lambda: mesh((5, 25, 15), [(80, 120, 170, (20, 120, 70)), (260, 330, 180, (10, 80, 60)), (240, 60, 110, (90, 150, 80))], blur=80),
    "Grafito": lambda: mesh((22, 22, 26), [(90, 110, 170, (60, 60, 70)), (250, 380, 190, (38, 38, 46))], blur=90),
    "Plata": lambda: mesh((170, 172, 180), [(80, 100, 170, (220, 222, 230)), (260, 380, 190, (130, 134, 145)), (260, 80, 120, (200, 204, 214))], blur=90),
    "Atardecer": lambda: mesh((40, 20, 60), [(160, 60, 200, (255, 140, 90)), (60, 300, 170, (220, 70, 130)), (280, 420, 170, (110, 50, 170))]),
}

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn in WALLS.items():
        fn().save(os.path.join(out, name + ".png"))
    sheet = Image.new("RGB", (len(WALLS) * 84, 126))
    for i, name in enumerate(WALLS):
        sheet.paste(Image.open(os.path.join(out, name + ".png")).resize((80, 120)), (i * 84 + 2, 3))
    sheet.save(os.path.join(out, "..", "wallpapers_sheet.png"))
    print("fondos:", len(WALLS))
