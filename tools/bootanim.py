#!/usr/bin/env python3
"""Builds the ByOS boot animation (HBA1 format) from the frames rendered by
theme/bootanim/render_intro.js (theme/bootanim/intro.html).

Usage: python bootanim.py FRAMES_DIR OUT_DIR [LOOP_FRAME]
 - FRAMES_DIR holds 0000.png, 0001.png... (320x480 or 2x) and logo.png (still logo).
 - Intro (0..LOOP-1) plays once, then LOOP..END loops (intro.html makes that part periodic).
 - Pure black background; only the vertical band with content is stored, each frame
   RGB565 LE compressed with zlib.
Output: hibyos.hba, last.png (rest frame = start of the loop, used as the player's 0.png)
and logo.png (still logo cropped with alpha, for "About").
"""
import glob, os, struct, sys, zlib
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
LOOP = int(sys.argv[3]) if len(sys.argv) > 3 else 90          # 3.75 s at 24 fps
FPS = 24
os.makedirs(out, exist_ok=True)

def load(p):
    im = Image.open(p).convert("RGB")
    if im.size != (320, 480):
        im = im.resize((320, 480), Image.LANCZOS)
    return np.asarray(im).astype(np.float32)

fs = sorted(f for f in glob.glob(os.path.join(src, "[0-9]*.png")))
frames = [load(f) for f in fs]
END = len(frames)

def clean(a):
    a = a.copy()
    a[a.max(axis=2) <= 6] = 0                 # pure black background
    return a

frames = [clean(a) for a in frames]
rows = np.zeros(480, bool)
for a in frames:
    rows |= a.max(axis=(1, 2)) > 0
ys = np.nonzero(rows)[0]
Y0, Y1 = int(ys[0]) & ~1, min(480, (int(ys[-1]) + 2) & ~1)

def to565(a):
    a = np.clip(a + 0.5, 0, 255).astype(np.int32)
    return ((a[..., 0] >> 3) << 11 | (a[..., 1] >> 2) << 5 | (a[..., 2] >> 3)).astype("<u2")

blobs = [zlib.compress(to565(a)[Y0:Y1].tobytes(), 9) for a in frames]
hdr = b"HBA1" + struct.pack("<7H", 320, Y1 - Y0, Y0, FPS, len(blobs), LOOP, 0)
offs, o = [], len(hdr) + 4 * (len(blobs) + 1)
for b in blobs:
    offs.append(o); o += len(b)
offs.append(o)
with open(os.path.join(out, "hibyos.hba"), "wb") as f:
    f.write(hdr + struct.pack(f"<{len(offs)}I", *offs) + b"".join(blobs))
print("hibyos.hba", o, "bytes,", len(blobs), "frames, band", Y0, Y1, "loop from", LOOP)

Image.fromarray(np.clip(frames[LOOP], 0, 255).astype(np.uint8)).save(os.path.join(out, "last.png"))

# still logo with alpha (luminance as alpha, colour un-premultiplied)
a = np.asarray(Image.open(os.path.join(src, "logo.png")).convert("RGB")).astype(np.float32)
al = np.clip(a.max(axis=2), 0, 255); al[al <= 10] = 0
rgb = np.where(al[..., None] > 0, a * 255 / np.maximum(al[..., None], 1), 0)
img = Image.fromarray(np.dstack([np.clip(rgb, 0, 255), al]).astype(np.uint8), "RGBA")
img = img.crop(img.getbbox())
if img.width > 400:                           # rendered at 2x: keep it at 1x like before
    img = img.resize((img.width // 2, img.height // 2), Image.LANCZOS)
img.save(os.path.join(out, "logo.png"))
print("logo.png", img.size)
