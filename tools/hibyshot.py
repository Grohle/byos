#!/usr/bin/env python3
"""Captura la pantalla del HiBy R3 II por ADB (framebuffer RGB565 320x480, doble bufer).
Uso: python hibyshot.py salida.png [--device IP:5555]"""
import subprocess, sys, os, struct
from PIL import Image

ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe") if os.name == "nt" else "adb"
dev = "192.168.1.147:5555"
args = sys.argv[1:]
if "--device" in args:
    i = args.index("--device"); dev = args[i + 1]; del args[i:i + 2]
out = args[0] if args else "screen.png"

def sh(cmd):
    return subprocess.run([ADB, "-s", dev, "shell", cmd], capture_output=True, text=True).stdout.strip()

# offset visible actual (pan del doble bufer)
pan = sh("cat /sys/class/graphics/fb0/pan").replace(",", " ").split()
yoff = int(pan[1]) if len(pan) > 1 and pan[1].isdigit() else 0
W, H = 320, 480
# el driver solo expone un fotograma por /dev/fb0: se lee entero sin desplazamiento
# con la pantalla en reposo el driver devuelve 0 bytes: se reintenta unas veces
DUMP = "for i in 1 2 3 4 5 6 7 8; do dd if=/dev/fb0 of=/tmp/fb.raw 2>/dev/null; [ -s /tmp/fb.raw ] && break; sleep 0.25; done; wc -c < /tmp/fb.raw"
if sh(DUMP).strip() in ("", "0"):
    sys.exit("pantalla apagada: enciendela (o: hibyinput key 116) y repite")
tmp = out + ".raw"
for attempt in range(3):
    r = subprocess.run([ADB, "-s", dev, "pull", "/tmp/fb.raw", tmp], capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(tmp) and os.path.getsize(tmp) >= W * H * 2:
        break
    sh(DUMP)
else:
    sys.exit("no se pudo leer el framebuffer: " + r.stderr.strip())
sh("rm -f /tmp/fb.raw")
raw = open(tmp, "rb").read(); os.remove(tmp)
frames = len(raw) // (W * H * 2)
start = (yoff // H if frames > 1 else 0) * W * H * 2
px = struct.unpack("<%dH" % (W * H), raw[start: start + W * H * 2])
img = Image.new("RGB", (W, H))
img.putdata([(((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31) for p in px])
img.save(out)
print(f"{out}  (pan y={yoff})")
