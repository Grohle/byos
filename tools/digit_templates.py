"""Plantillas reales de los digitos de la barra de estado de HiBy -> src/digits.h
Entrada: capturas de 30 filas (RGB565) con un volumen conocido.
  fondo negro (pantallas de HiBy): primer plano = canal maximo alto
  fondo de pantalla (launcher): primer plano = canal minimo alto (texto blanco)"""
import os
import struct
import sys

SCR = sys.argv[1]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "digits.h")

def load(path):
    r = open(path, "rb").read()
    px = struct.unpack("<%dH" % (320 * 30), r[: 320 * 30 * 2])
    return [((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px]

def fgmap(px, black_bg):
    if black_bg:
        return [[max(px[y * 320 + x]) > 140 for x in range(320)] for y in range(30)]
    return [[min(px[y * 320 + x]) > 175 for x in range(320)] for y in range(30)]

def blobs(fg):
    cols = [any(fg[y][x] for y in range(4, 25)) for x in range(24, 70)]
    out, x = [], 0
    while x < len(cols):
        if cols[x]:
            s = x
            while x < len(cols) and cols[x]: x += 1
            out.append((24 + s, 24 + x))
        else: x += 1
    return out

samples = {}
def add(path, number, black_bg):
    fg = fgmap(load(path), black_bg)
    bl = blobs(fg)
    digs = str(number)
    for (a, b), d in zip(bl, digs):
        rows = [y for y in range(4, 25) if any(fg[y][x] for x in range(a, b))]
        top, bot = rows[0], rows[-1]
        m = [[1 if fg[y][x] else 0 for x in range(a, b)] for y in range(top, bot + 1)]
        samples.setdefault(d, m)

# tops_*: fondo negro 24,25,26,25,24,23 ; dg_*: launcher 25..32
for i, n in enumerate([24, 25, 26, 25, 24, 23]):
    add(os.path.join(SCR, "top_%d.raw" % i), n, True)
for i, n in enumerate(range(25, 33)):
    add(os.path.join(SCR, "dg_%d.raw" % i), n, False)
missing = [d for d in "0123456789" if d not in samples]
if missing:
    sys.exit("faltan digitos: " + "".join(missing))
with open(OUT, "w", newline="\n") as f:
    f.write("/* digits.h: digitos reales de la barra de estado de HiBy (generado por tools/digit_templates.py) */\n")
    f.write("struct dtpl { int w, h; const char *rows[16]; };\nstatic const struct dtpl DIGITS[10] = {\n")
    for d in "0123456789":
        m = samples[d]
        rows = ", ".join('"%s"' % "".join("#" if v else "." for v in r) for r in m)
        f.write("    { %d, %d, { %s } },   /* %s */\n" % (len(m[0]), len(m), rows, d))
    f.write("};\n")
for d in "0123456789":
    print(d, len(samples[d][0]), "x", len(samples[d]))
    for r in samples[d]: print("   ", "".join("#" if v else "." for v in r))
