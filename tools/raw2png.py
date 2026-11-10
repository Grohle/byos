"""raw2png.py ENTRADA.raw SALIDA.png [ANCHO ALTO]: RGB565 little endian -> PNG."""
import struct
import sys

from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) >= 5 else (320, 480)
raw = open(src, "rb").read()[: w * h * 2]
px = struct.unpack("<%dH" % (w * h), raw)
img = Image.new("RGB", (w, h))
img.putdata([((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
img.save(dst)
