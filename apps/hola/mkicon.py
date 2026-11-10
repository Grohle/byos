"""Genera icon.png (128x128, cuadrado a sangre: la mascara squircle la pone el instalador)."""
import os
from PIL import Image, ImageDraw, ImageFilter

S, K = 128, 4
N = S * K
img = Image.new("RGB", (N, N))
px = img.load()
top, mid, bot = (40, 46, 120), (120, 64, 226), (255, 118, 132)
for y in range(N):
    for x in range(N):
        t = (y * .85 + x * .35) / (N * 1.2)
        a, b, u = (top, mid, t / .55) if t < .55 else (mid, bot, (t - .55) / .45)
        u = u * u * (3 - 2 * u)
        px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * u) for i in range(3))
img = img.convert("RGBA")
# bocadillo blanco con sombra suave
sh = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(sh)
d.rounded_rectangle((24 * K, 32 * K, 104 * K, 88 * K), 20 * K, fill=(0, 0, 0, 90))
d.polygon([(40 * K, 84 * K), (34 * K, 104 * K), (58 * K, 86 * K)], fill=(0, 0, 0, 90))
sh = sh.filter(ImageFilter.GaussianBlur(4 * K)).transform(sh.size, Image.AFFINE, (1, 0, 0, 0, 1, -3 * K))
img.alpha_composite(sh)
d = ImageDraw.Draw(img)
d.rounded_rectangle((24 * K, 28 * K, 104 * K, 84 * K), 20 * K, fill=(255, 255, 255, 255))
d.polygon([(40 * K, 80 * K), (34 * K, 100 * K), (58 * K, 82 * K)], fill=(255, 255, 255, 255))
for i, c in enumerate([(120, 64, 226), (190, 90, 200), (255, 118, 132)]):
    cx = (46 + i * 18) * K
    d.ellipse((cx - 6 * K, 50 * K, cx + 6 * K, 62 * K), fill=c + (255,))
img.resize((S, S), Image.LANCZOS).save(os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.png"))
print("icon.png ok")
