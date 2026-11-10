"""recsheet.py GRABACION.bin SALIDA.png [columnas]
Hoja de contactos de una grabacion de `hibyctl rec`: cada fotograma con su tiempo y el
intervalo con el anterior (en rojo si supera 30 ms = tiron visible)."""
import struct
import sys

from PIL import Image, ImageDraw

src, dst = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 8
raw = open(src, "rb").read()
FR = 8 + 160 * 240 * 2
frames = []
for i in range(len(raw) // FR):
    t, yo = struct.unpack_from("<II", raw, i * FR)
    px = struct.unpack_from("<%dH" % (160 * 240), raw, i * FR + 8)
    im = Image.new("RGB", (160, 240))
    im.putdata([((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
    frames.append((t, yo, im))
rows = (len(frames) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 164, rows * 258), (40, 40, 40))
d = ImageDraw.Draw(sheet)
prev = None
gaps = []
for i, (t, yo, im) in enumerate(frames):
    x, y = (i % cols) * 164, (i // cols) * 258
    sheet.paste(im, (x + 2, y + 16))
    gap = t - prev if prev is not None else 0
    if prev is not None:
        gaps.append(gap)
    d.text((x + 3, y + 2), "%d %dms +%d" % (i, t, gap), fill=(255, 80, 80) if gap > 30 else (220, 220, 220))
    prev = t
sheet.save(dst)
if gaps:
    print("fotogramas %d, intervalo medio %.1f ms, peor %d ms, >30ms: %d" % (len(frames), sum(gaps) / len(gaps), max(gaps), sum(g > 30 for g in gaps)))
