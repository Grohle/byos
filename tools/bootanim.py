#!/usr/bin/env python3
"""Genera la animacion de arranque de HiByOS (formato HBA1) a partir del video.

Uso: python bootanim.py source.mp4 outdir
 - Escala el video 1280x720 para que el logo mida ~260 px y lo centra en 320x480.
 - Fondo negro puro (los pixeles casi negros del video se fuerzan a 0).
 - Intro (0..LOOP-1) una vez y bucle LOOP..END con fundido cruzado cosido.
 - Solo guarda la banda vertical con contenido; cada fotograma RGB565 LE con zlib.
Salida: hibyos.hba, last.png (fotograma de reposo = inicio del bucle, para el 0.png
del reproductor) y logo.png (logo recortado con alfa para "Acerca de").
"""
import glob, os, struct, subprocess, sys, tempfile, zlib
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
LOOP, END, XF = 68, 176, 12        # bucle 68..175, ultimos 12 fundidos con 56..67
Y0, Y1 = 186, 296
os.makedirs(out, exist_ok=True)
tmp = tempfile.mkdtemp()
subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-vf",
                "scale=538:302:flags=lanczos,crop=320:302:109:0,pad=320:480:0:89:black",
                os.path.join(tmp, "%03d.png")], check=True)
fs = sorted(glob.glob(os.path.join(tmp, "*.png")))
F = [np.asarray(Image.open(f).convert("RGB")).astype(np.float32) for f in fs]
frames = []
L = END - LOOP
for i in range(END):
    a = F[i]
    if i >= END - XF:                       # cose el final del bucle con lo previo a LOOP
        w = (i - (END - XF) + 1) / (XF + 1)
        a = a * (1 - w) + F[i - L] * w
    frames.append(a)

def to565(a):
    a = a.copy()
    a[a.max(axis=2) <= 8] = 0               # fondo negro puro
    a = np.clip(a + 0.5, 0, 255).astype(np.int32)
    return ((a[..., 0] >> 3) << 11 | (a[..., 1] >> 2) << 5 | (a[..., 2] >> 3)).astype("<u2")

blobs = [zlib.compress(to565(a)[Y0:Y1].tobytes(), 9) for a in frames]
hdr = b"HBA1" + struct.pack("<7H", 320, Y1 - Y0, Y0, 24, len(blobs), LOOP, 0)
offs, o = [], len(hdr) + 4 * (len(blobs) + 1)
for b in blobs:
    offs.append(o); o += len(b)
offs.append(o)
with open(os.path.join(out, "hibyos.hba"), "wb") as f:
    f.write(hdr + struct.pack(f"<{len(offs)}I", *offs) + b"".join(blobs))
print("hibyos.hba", o, "bytes,", len(blobs), "frames")

rest = frames[LOOP].copy(); rest[rest.max(axis=2) <= 8] = 0
Image.fromarray(np.clip(rest, 0, 255).astype(np.uint8)).save(os.path.join(out, "last.png"))
# logo con alfa (luminancia como alfa, color desmultiplicado) desde el fotograma mas brillante
k = int(np.argmax([f[Y0:Y1].mean() for f in F[LOOP:END]])) + LOOP
a = F[k][Y0:Y1]
al = np.clip(a.max(axis=2), 0, 255); al[al <= 14] = 0
rgb = np.where(al[..., None] > 0, a * 255 / np.maximum(al[..., None], 1), 0)
img = Image.fromarray(np.dstack([np.clip(rgb, 0, 255), al]).astype(np.uint8), "RGBA")
img.crop(img.getbbox()).save(os.path.join(out, "logo.png"))
print("logo.png", img.getbbox())
