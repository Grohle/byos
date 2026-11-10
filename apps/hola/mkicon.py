"""Genera icon.png (128x128, cuadrado a sangre y opaco: la mascara la pone el instalador).

Mismo lenguaje que los iconos del sistema (theme/launcher_icons.js): rejilla de 100 unidades,
luz desde arriba (fondo mas claro arriba, sombra suave hacia abajo), sin brillo propio
(el instalador ya anade un filo de luz). Bocadillo blanco con tres puntos de "escribiendo".
Se dibuja a 8x y se reduce con LANCZOS para bordes suaves."""
import os
from PIL import Image, ImageDraw, ImageFilter

S, K = 128, 8
N = S * K
U = N / 100                                   # 1 unidad de la rejilla de 100


def u(*v):
    return tuple(int(round(x * U)) for x in v)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# fondo: indigo (arriba-izquierda) -> violeta -> coral (abajo-derecha), con curva suave
grad = Image.new("RGB", (256, 256))
gp = grad.load()
c0, c1, c2 = (88, 86, 240), (150, 82, 236), (255, 106, 136)
for y in range(256):
    for x in range(256):
        t = (y * .8 + x * .3) / (255 * 1.1)
        t = t * t * (3 - 2 * t)
        gp[x, y] = lerp(c0, c1, t / .5) if t < .5 else lerp(c1, c2, (t - .5) / .5)
img = grad.resize((N, N), Image.BICUBIC).convert("RGBA")


def bubble(draw, dy=0, fill=(255, 255, 255, 255)):
    draw.rounded_rectangle(u(17, 24 + dy, 83, 70 + dy), int(17 * U), fill=fill)
    # cola: curva hacia abajo a la izquierda
    tail = [(26, 62 + dy), (24, 78 + dy), (23.4, 80.6 + dy), (26, 80.4 + dy), (40, 68 + dy), (44, 64 + dy)]
    draw.polygon([u(x, y) for x, y in tail], fill=fill)


# sombra suave del bocadillo (hacia abajo)
sh = Image.new("RGBA", (N, N), (0, 0, 0, 0))
bubble(ImageDraw.Draw(sh), dy=2.4, fill=(40, 0, 80, 95))
img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(2.4 * U)))
# bocadillo con un degradado blanco -> lavanda muy suave
bm = Image.new("L", (N, N), 0)
bubble(ImageDraw.Draw(bm), fill=255)
bm = bm.filter(ImageFilter.GaussianBlur(.35 * U))                 # une la cola con el cuerpo sin escalon
fillg = Image.new("RGBA", (N, N))
fd = ImageDraw.Draw(fillg)
for y in range(N):
    fd.line((0, y, N, y), fill=lerp((255, 255, 255), (238, 234, 252), y / N) + (255,))
img.paste(fillg, (0, 0), bm)
# tres puntos con los colores del fondo
d = ImageDraw.Draw(img)
for i, c in enumerate([(98, 92, 240), (162, 84, 232), (255, 106, 136)]):
    cx = 35 + i * 15
    d.ellipse(u(cx - 5.4, 47 - 5.4, cx + 5.4, 47 + 5.4), fill=c + (255,))
img.convert("RGB").resize((S, S), Image.LANCZOS).save(os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.png"))
print("icon.png ok")
