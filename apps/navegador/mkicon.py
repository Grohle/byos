"""mkicon.py - icono del Navegador (128x128 a sangre, opaco; el instalador aplica la mascara).

Mismo lenguaje que los iconos del sistema (theme/launcher_icons.js): rejilla de 100 unidades,
luz desde arriba, sombra suave hacia abajo. Globo terraqueo con esfera luminosa, meridianos y
paralelos (no es una brujula: evitamos parecernos a Safari)."""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

S, K = 128, 8
N = S * K
U = N / 100


def u(*v):
    return tuple(int(round(x * U)) for x in v)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# fondo: cielo (arriba) -> azul profundo (abajo)
img = Image.new("RGBA", (N, N))
d = ImageDraw.Draw(img)
for y in range(N):
    d.line((0, y, N, y), fill=lerp((64, 200, 255), (10, 92, 226), (y / N) ** 1.1) + (255,))

cx, cy, R = 50, 50, 33
# sombra del globo
sh = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ImageDraw.Draw(sh).ellipse(u(cx - R, cy - R + 2.6, cx + R, cy + R + 2.6), fill=(0, 30, 100, 110))
img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(2.6 * U)))

# esfera: degradado radial blanco -> celeste, con el foco arriba a la izquierda
sp = Image.new("RGBA", (N, N), (0, 0, 0, 0))
spx = sp.load()
x0, y0, x1, y1 = u(cx - R, cy - R, cx + R, cy + R)
fx, fy = (cx - 11) * U, (cy - 13) * U
for y in range(y0, y1):
    for x in range(x0, x1):
        t = min(1, math.hypot(x - fx, y - fy) / (R * 1.75 * U))
        spx[x, y] = lerp((255, 255, 255), (172, 214, 255), t ** 1.3) + (255,)
mask = Image.new("L", (N, N), 0)
ImageDraw.Draw(mask).ellipse(u(cx - R, cy - R, cx + R, cy + R), fill=255)
img.paste(sp, (0, 0), mask)

# rejilla del globo (azul, dentro de la esfera)
grid = Image.new("RGBA", (N, N), (0, 0, 0, 0))
g = ImageDraw.Draw(grid)
col, w = (20, 120, 240, 235), int(2.3 * U)
for k in (.42, .82):                                          # meridianos (elipses)
    g.ellipse(u(cx - R * k, cy - R, cx + R * k, cy + R), outline=col, width=w)
g.line(u(cx, cy - R, cx, cy + R), fill=col, width=w)
for fr in (-.52, 0, .52):                                     # paralelos
    yy = cy + R * fr
    hw = math.sqrt(R * R - (R * fr) ** 2)
    g.line(u(cx - hw, yy, cx + hw, yy), fill=col, width=w)
img.paste(grid, (0, 0), Image.composite(grid.getchannel("A"), Image.new("L", (N, N), 0), mask))
d = ImageDraw.Draw(img)
d.ellipse(u(cx - R, cy - R, cx + R, cy + R), outline=(20, 120, 240, 255), width=int(2.6 * U))

here = os.path.dirname(os.path.abspath(__file__))
img.convert("RGB").resize((S, S), Image.LANCZOS).save(os.path.join(here, "icon.png"), optimize=True)
print("icon.png ok")
