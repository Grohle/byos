#!/usr/bin/env python3
"""hibyctl - cliente del host para el arnes HiBy (habla con hibyinput dentro del guest, tcp 127.0.0.1:5555).

  hibyctl.py tap X Y            hibyctl.py swipe X1 Y1 X2 Y2 [ms]
  hibyctl.py key VOLUMEUP       hibyctl.py key 116 click 0
  hibyctl.py shot out.png       (captura del area visible de /dev/fb0, RGB565 -> PNG)
  hibyctl.py sh 'free; ps'      (ejecuta en el guest)
  hibyctl.py fbinfo
  hibyctl.py script FILE        (un comando por linea, mismas palabras que el protocolo)
Puerto: HIBY_PORT (def 5555).
"""
import os, socket, struct, sys, zlib


def connect():
    s = socket.create_connection(("127.0.0.1", int(os.environ.get("HIBY_PORT", "5555"))), timeout=600)
    return s, s.makefile("rb")


def readline(f):
    return f.readline().decode(errors="replace").rstrip("\n")


def png_rgb565(w, h, data, path, bpp=16):
    rows = []
    for y in range(h):
        row = bytearray([0])
        line = data[y * w * 2:(y + 1) * w * 2]
        for (px,) in struct.iter_unpack("<H", line):
            r = (px >> 11) & 31; g = (px >> 5) & 63; b = px & 31
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def cmd(s, f, line, out=None):
    s.sendall((line + "\n").encode())
    w = line.split()
    if w[0] == "shot":
        hdr = readline(f).split()
        if hdr[0] != "shot":
            print(" ".join(hdr)); return
        W, H, n = int(hdr[1]), int(hdr[2]), int(hdr[3])
        data = f.read(n)
        png_rgb565(W, H, data, out or "shot.png")
        print("shot -> %s (%dx%d)" % (out or "shot.png", W, H))
    elif w[0] == "sh":
        while True:
            l = readline(f)
            if l.startswith(".") and l[1:].lstrip("-").isdigit():
                return int(l[1:])
            print(l)
    else:
        print(readline(f))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    s, f = connect()
    if a[0] == "shot":
        cmd(s, f, "shot", a[1] if len(a) > 1 else "shot.png")
    elif a[0] == "sh":
        sys.exit(cmd(s, f, "sh " + " ".join(a[1:])) or 0)
    elif a[0] == "script":
        for line in open(a[1]):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("shot"):
                p = line.split()
                cmd(s, f, "shot", p[1] if len(p) > 1 else "shot.png")
            else:
                cmd(s, f, line)
    else:
        cmd(s, f, " ".join(a))
    s.sendall(b"quit\n")


if __name__ == "__main__":
    main()
