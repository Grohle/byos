#!/usr/bin/env python3
"""Construye la carpeta HiByOS/ de la SD para una version publica (ver docs/INSTALADOR.md).

  python tools/make_payload.py [--version 0.6.0] [--out out/sd]

Resultado: out/sd/HiByOS/payload/
  VERSION        version del payload (por defecto la de overlay/usr/bin/hibymod)
  MANIFEST.md5   md5 de cada fichero (el instalador lo verifica antes de tocar nada)
  bin/           hibyshell hibywall hibymix hibytag hibymod_disp.so   (de bin/, compilados en WSL)
  shell/         capas .ov, icons/*.ov, wall.raw, home.sig            (de shell/, tools/shell_assets.py)
  theme/         SOLO nuestros ficheros de layout/theme3 y litegui/theme3 (rutas relativas a
                 /usr/resource). Nada de fabrica: el instalador copia theme3 de fabrica del
                 propio aparato y pone estos encima (como build_theme.py deploy()).
  mirror/        inicio "espejo": vista del inicio con fondo launcher/mirror.png e iconos
                 transparentes. Se aplica cuando hibyshell ya ha generado mirror.png.
  Pictures/      los 9 fondos (theme/wallpapers.py); se copian a /Pictures de la SD si faltan.

Se comprueba que ningun fichero de theme/ sea identico al de fabrica (no se redistribuye nada
de HiBy) y que los binarios sean MIPS.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
THEME = os.path.join(ROOT, "theme")
STOCK = os.environ.get("HIBY_STOCK", r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource")
sys.path.insert(0, THEME)
import viewlib as V  # noqa: E402

BINS = {  # payload/bin/<nombre>: fuente
    "hibyshell": "bin/hibyshell",
    "hibywall": "bin/hibywall",
    "hibymix": "bin/hibymix",
    "hibytag": "bin/hibytag",
    "hibymod_disp.so": "bin/hibymod_disp.so",
}
# fuentes de cada binario (solo para avisar si el binario es mas viejo que el codigo)
SRCS = {
    "hibyshell": ["src/hibyshell.c", "src/*.inc.c", "src/hm_shared.h", "src/digits.h", "src/hibytag.c"],
    "hibywall": ["src/hibywall.c"],
    "hibymix": ["src/hibymix.c"],
    "hibytag": ["src/hibytag.c", "src/hibytag.h"],
    "hibymod_disp.so": ["src/hibymod_disp.c", "src/hm_shared.h"],
}
THEME_SUBS = ("layout/theme3", "litegui/theme3")
# iconos del inicio de fabrica que en modo espejo pasan a ser transparentes
MIRROR_ICONS = ("book", "book_s", "dac", "dac_s", "music", "music_s", "stream_media", "stream_media_s",
                "sys_set", "sys_set_s", "wireless", "wireless_s")


def version_default():
    with open(os.path.join(ROOT, "overlay", "usr", "bin", "hibymod"), encoding="utf-8") as f:
        m = re.search(r'echo "HiByOS ([0-9][0-9A-Za-z.\-]*)"', f.read())
    return m.group(1) if m else "0.0.0"


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def check_mips(path):
    with open(path, "rb") as f:
        h = f.read(20)
    if h[:4] != b"\x7fELF" or h[5] != 1 or struct.unpack("<H", h[18:20])[0] != 8:
        sys.exit(f"ERROR: {path} no es un ELF MIPS little-endian")


def newest_src(patterns):
    import glob
    t = 0
    for p in patterns:
        for f in glob.glob(os.path.join(ROOT, p)):
            t = max(t, os.path.getmtime(f))
    return t


def stock_path(rel):
    return os.path.join(STOCK, *rel.split("/"))


def volume_dlg(dst):
    """Dialogo de volumen de HiBy oculto (hibyshell dibuja su propia pastilla de volumen)."""
    root = V.load(stock_path("layout/theme3/dialog/volume.dlg"))
    vg = V.get(root, "viewgroup")
    n = 0
    for k, el in V.children(vg):
        if k in ("imageview", "progress_bar"):
            V.set_(el, "visible", "hide")
            n += 1
    if n != 3:
        sys.exit(f"ERROR: volume.dlg de fabrica inesperado ({n} elementos)")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    V.save(dst, root)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=version_default())
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "sd"))
    a = ap.parse_args()

    top = os.path.join(a.out, "HiByOS")
    pl = os.path.join(top, "payload")
    if os.path.isdir(top):
        shutil.rmtree(top)
    os.makedirs(pl)
    warn = []

    # ---- binarios
    for name, rel in BINS.items():
        src = os.path.join(ROOT, rel)
        if not os.path.isfile(src):
            sys.exit(f"ERROR: falta {rel} (compilar en WSL con ~/hiby/tools/zbuild_*.sh)")
        check_mips(src)
        if newest_src(SRCS[name]) > os.path.getmtime(src) + 1:
            warn.append(f"{rel} es mas antiguo que su codigo fuente (recompilar?)")
        copy(src, os.path.join(pl, "bin", name))

    # ---- capas del shell (sin vistas previas, PNG de trabajo ni estado del aparato)
    sh = os.path.join(ROOT, "shell")
    for f in sorted(os.listdir(sh)):
        if f.endswith(".ov") or f in ("wall.raw", "home.sig"):
            copy(os.path.join(sh, f), os.path.join(pl, "shell", f))
    for f in sorted(os.listdir(os.path.join(sh, "icons"))):
        if f.endswith(".ov"):
            copy(os.path.join(sh, "icons", f), os.path.join(pl, "shell", "icons", f))
    for f in ("wall.raw", "home.sig", "home_btn.ov", "hibyos_logo.ov"):
        if not os.path.isfile(os.path.join(pl, "shell", f)):
            sys.exit(f"ERROR: falta shell/{f} (python tools/shell_assets.py)")

    # ---- tema: solo nuestros ficheros de theme3
    th = os.path.join(pl, "theme")
    srcs = [os.path.join(THEME, "out"), os.path.join(ROOT, "overlay", "usr", "resource")]
    for base in srcs:
        for sub in THEME_SUBS:
            d = os.path.join(base, *sub.split("/"))
            for dp, _dn, fn in os.walk(d):
                for f in fn:
                    s = os.path.join(dp, f)
                    rel = os.path.relpath(s, base).replace("\\", "/")
                    copy(s, os.path.join(th, *rel.split("/")))
    volume_dlg(os.path.join(th, "layout", "theme3", "dialog", "volume.dlg"))
    for f in ("layout/theme3/launcher/hiby_launcher_apps.view", "litegui/theme3/launcher/wallpaper.png",
              "layout/theme3/topbar/topbar.view"):
        if not os.path.isfile(os.path.join(th, *f.split("/"))):
            sys.exit(f"ERROR: falta theme/{f} (theme/build_theme.py, theme/topbar.py, theme/hibyos_brand.py)")

    # ---- inicio espejo
    mi = os.path.join(pl, "mirror")
    view = os.path.join(mi, "layout", "theme3", "launcher", "hiby_launcher_apps.view")
    os.makedirs(os.path.dirname(view))
    subprocess.run([sys.executable, os.path.join(THEME, "mirror_view.py"),
                    os.path.join(th, "layout", "theme3", "launcher", "hiby_launcher_apps.view"), view],
                   check=True, capture_output=True)
    ldir = os.path.join(mi, "litegui", "theme3", "launcher")
    os.makedirs(ldir)
    for n in MIRROR_ICONS:
        src = os.path.join(th, "litegui", "theme3", "launcher", n + ".png")
        if not os.path.isfile(src):
            sys.exit(f"ERROR: falta el icono {n}.png del tema (theme/build_theme.py)")
        Image.new("RGBA", Image.open(src).size, (0, 0, 0, 0)).save(os.path.join(ldir, n + ".png"), optimize=True)

    # ---- comprobacion de derechos: nada identico a fabrica
    if not os.path.isdir(STOCK):
        sys.exit(f"ERROR: no encuentro los recursos de fabrica en {STOCK} (HIBY_STOCK=...)")
    for base in (th, mi):
        for dp, _dn, fn in os.walk(base):
            for f in fn:
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, base).replace("\\", "/")
                sp = stock_path(rel)
                if os.path.isfile(sp) and md5(sp) == md5(p):
                    os.remove(p)
                    warn.append(f"quitado {rel}: es identico al de fabrica")

    # ---- fondos para /Pictures
    tmp = tempfile.mkdtemp()
    try:
        subprocess.run([sys.executable, os.path.join(THEME, "wallpapers.py"), os.path.join(tmp, "p")],
                       check=True, capture_output=True)
        for f in sorted(os.listdir(os.path.join(tmp, "p"))):
            copy(os.path.join(tmp, "p", f), os.path.join(pl, "Pictures", f))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- version y manifiesto (LF; rutas sin espacios)
    with open(os.path.join(pl, "VERSION"), "w", newline="\n") as f:
        f.write(a.version + "\n")
    lines = []
    for dp, _dn, fn in os.walk(pl):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, pl).replace("\\", "/")
            if rel == "MANIFEST.md5":
                continue
            if any(c in rel for c in " \t\n\\"):
                sys.exit(f"ERROR: nombre no admitido en el payload: {rel!r}")
            lines.append(f"{md5(p)}  {rel}\n")
    lines.sort(key=lambda l: l.split("  ", 1)[1])
    with open(os.path.join(pl, "MANIFEST.md5"), "w", newline="\n", encoding="utf-8") as f:
        f.writelines(lines)

    # ---- resumen
    tot = {}
    for l in lines:
        rel = l.split("  ", 1)[1].strip()
        k = rel.split("/")[0] if "/" in rel else "."
        tot.setdefault(k, [0, 0])
        tot[k][0] += 1
        tot[k][1] += os.path.getsize(os.path.join(pl, rel))
    print(f"HiByOS {a.version}: {os.path.abspath(top)}")
    for k, (n, b) in sorted(tot.items()):
        print(f"  {k:9s} {n:4d} ficheros {b / 1024:8.1f} KB")
    print(f"  total     {len(lines):4d} ficheros {sum(b for _n, b in tot.values()) / 1024:8.1f} KB")
    for w in warn:
        print("AVISO:", w)


if __name__ == "__main__":
    main()
