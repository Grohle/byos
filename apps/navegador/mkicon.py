"""mkicon.py - icono del Navegador (128x128 a sangre, opaco; el instalador aplica la mascara).
Degradado azul cielo, globo blanco con meridianos y aguja de brujula (roja/blanca)."""
import math
from PIL import Image, ImageDraw, ImageFilter
S = 512
img = Image.new("RGB", (S, S))
top, bot = (90, 200, 250), (0, 110, 240)
px = img.load()
for y in range(S):
    t = y / (S - 1)
    c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
    for x in range(S):
        px[x, y] = c
d = ImageDraw.Draw(img, "RGBA")
cx = cy = S / 2; R = S * 0.36; w = int(S * 0.028)
# sombra suave del globo
sh = Image.new("L", (S, S), 0); ImageDraw.Draw(sh).ellipse((cx - R, cy - R + 10, cx + R, cy + R + 10), fill=70)
sh = sh.filter(ImageFilter.GaussianBlur(14)); img.paste((0, 40, 110), (0, 0), sh)
d = ImageDraw.Draw(img, "RGBA")
d.ellipse((cx - R, cy - R, cx + R, cy + R), fill=(255, 255, 255, 40), outline=(255, 255, 255, 255), width=w)
# meridianos y paralelos
for k in (0.42, 0.78):
    d.ellipse((cx - R * k, cy - R, cx + R * k, cy + R), outline=(255, 255, 255, 150), width=w // 2)
d.line((cx, cy - R, cx, cy + R), fill=(255, 255, 255, 150), width=w // 2)
for f in (-0.5, 0, 0.5):
    yy = cy + R * f; hw = math.sqrt(max(R * R - (R * f) ** 2, 0))
    d.line((cx - hw, yy, cx + hw, yy), fill=(255, 255, 255, 150), width=w // 2)
# aguja: rombo rojo hacia arriba-derecha y blanco hacia abajo-izquierda
a = math.radians(45); L = R * 0.92; Wd = R * 0.17
ux, uy = math.cos(a), -math.sin(a); vx, vy = -uy, ux
tip1 = (cx + ux * L, cy + uy * L); tip2 = (cx - ux * L, cy - uy * L)
s1 = (cx + vx * Wd, cy + vy * Wd); s2 = (cx - vx * Wd, cy - vy * Wd)
d.polygon([tip1, s1, s2], fill=(255, 59, 48, 255))
d.polygon([tip2, s1, s2], fill=(255, 255, 255, 255))
d.ellipse((cx - Wd * 0.35, cy - Wd * 0.35, cx + Wd * 0.35, cy + Wd * 0.35), fill=(60, 60, 67, 255))
img.resize((128, 128), Image.LANCZOS).save("icon.png", optimize=True)
