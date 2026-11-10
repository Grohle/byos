"""Checks src/qr.inc.c against the python-qrcode reference encoder, module by module.

Run through tests/qr_test.sh (it builds /tmp/qr_test and provides the Python packages).
For every payload and each of the 8 masks the two matrices must be identical; the mask our
encoder picks on its own must also decode back to the payload with OpenCV's QR detector.
"""
import os
import random
import subprocess
import sys

import qrcode
from qrcode import constants, util

BIN = sys.argv[1]


def ours(data: bytes, mask: int):
    r = subprocess.run([BIN, str(mask), data.hex()], capture_output=True, text=True)
    if r.returncode == 2:
        return None
    assert r.returncode == 0, r
    lines = r.stdout.split("\n")
    ver, msk, n = map(int, lines[0].split())
    return ver, msk, [[c == "1" for c in row] for row in lines[1:1 + n]]


def reference(data: bytes, mask: int):
    q = qrcode.QRCode(error_correction=constants.ERROR_CORRECT_M, border=0, mask_pattern=mask)
    q.add_data(util.QRData(data, mode=util.MODE_8BIT_BYTE))
    q.make(fit=True)
    return q.version, [[bool(c) for c in row] for row in q.modules]


def decode(mat):
    import cv2
    import numpy as np
    n, s, b = len(mat), 6, 4
    img = np.full(((n + 2 * b) * s, (n + 2 * b) * s), 255, np.uint8)
    for y in range(n):
        for x in range(n):
            if mat[y][x]:
                img[(y + b) * s:(y + b + 1) * s, (x + b) * s:(x + b + 1) * s] = 0
    txt, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
    return txt


random.seed(7)
payloads = [b"https://github.com/Grohle/hibyos", b"https://buymeacoffee.com/grohle", b"A", b"HELLO WORLD 123"]
# every version boundary of level M in byte mode (v1..v10), one byte under and exactly full
for cap in (14, 26, 42, 62, 84, 106, 122, 152, 180, 213):
    for n in (cap - 1, cap):
        payloads.append(bytes(random.choice(b"abcdefghijklmnopqrstuvwxyz0123456789/:.-_?=&") for _ in range(n)))
        payloads.append(bytes(random.randrange(256) for _ in range(n)))

bad = 0
for p in payloads:
    for mask in range(8):
        o = ours(p, mask)
        rv, rm = reference(p, mask)
        if o is None or o[0] != rv or o[2] != rm:
            bad += 1
            print(f"MISMATCH len={len(p)} mask={mask} ours_v={o and o[0]} ref_v={rv}")
    o = ours(p, -1)
    try:
        text = p.decode("ascii")
    except UnicodeDecodeError:
        continue
    got = decode(o[2])
    if got != text:
        bad += 1
        print(f"DECODE FAIL len={len(p)} v={o[0]} mask={o[1]}: {got!r}")

assert ours(b"x" * 214, -1) is None, "214 bytes must not fit (v10-M holds 213)"
print(f"{len(payloads)} payloads x 8 masks compared, {bad} problems")
sys.exit(1 if bad else 0)
