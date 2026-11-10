"""Genera font8x16.h (fuente de mapa de bits 8x16) desde una fuente de consola PSF de Linux.
Uso (WSL): python3 mkfont.py /usr/share/consolefonts/Lat15-TerminusBold16.psf.gz font8x16.h
Incluye ASCII 32-126 y unos pocos caracteres latinos (es/fr) con su punto de codigo Unicode."""
import gzip
import struct
import sys

EXTRA = "áéíóúñÑçèàêâîôûëïüœÉÈÀÇ¡¿«»·…"


def load_psf(path):
    data = (gzip.open if path.endswith(".gz") else open)(path, "rb").read()
    if data[:2] == b"\x36\x04":                       # PSF1
        mode, h = data[2], data[3]
        n = 512 if mode & 1 else 256
        glyphs = [data[4 + i * h:4 + (i + 1) * h] for i in range(n)]
        uni = {}
        if mode & 2:                                  # tabla Unicode: uint16 por glifo, fin 0xFFFF
            p = 4 + n * h
            for i in range(n):
                while True:
                    (u,) = struct.unpack_from("<H", data, p)
                    p += 2
                    if u == 0xFFFF:
                        break
                    if u != 0xFFFE:
                        uni.setdefault(u, i)
        return 8, h, glyphs, uni
    magic, ver, hs, flags, n, cb, h, w = struct.unpack_from("<8I", data)
    if magic != 0x864AB572:
        raise SystemExit("no es PSF")
    glyphs = [data[hs + i * cb:hs + (i + 1) * cb] for i in range(n)]
    uni = {}
    if flags & 1:
        p = hs + n * cb
        for i in range(n):
            while data[p] != 0xFF:
                if data[p] == 0xFE:
                    p += 1
                    continue
                b = data[p]
                ln = 1 if b < 0x80 else 2 if b < 0xE0 else 3 if b < 0xF0 else 4
                uni.setdefault(ord(data[p:p + ln].decode("utf-8")), i)
                p += ln
            p += 1
    return w, h, glyphs, uni


def main():
    w, h, glyphs, uni = load_psf(sys.argv[1])
    if w != 8 or h != 16:
        raise SystemExit("hace falta una fuente 8x16")
    cps = list(range(32, 127)) + [ord(c) for c in EXTRA]
    out = ["/* font8x16.h - generado por mkfont.py desde " + sys.argv[1].split("/")[-1] + " (no editar) */",
           "static const unsigned short font_cp[] = {"]
    cps = [c for c in cps if c in uni or c < 128]
    out.append("    " + ", ".join(str(c) for c in cps))
    out.append("};")
    out.append("static const unsigned char font_bits[][16] = {")
    for c in cps:
        g = glyphs[uni.get(c, c)]
        out.append("    {" + ",".join("0x%02x" % b for b in g[:16]) + "},  /* U+%04X */" % c)
    out.append("};")
    out.append("#define FONT_N %d" % len(cps))
    open(sys.argv[2], "w", newline="\n").write("\n".join(out) + "\n")
    print(len(cps), "glifos")


main()
