"""hpk.py - empaquetador de apps de hibymod (.hpk = tar ustar sin comprimir). Ver docs/APP_FORMAT.md.

  python tools/hpk.py build DIR -o app.hpk     valida DIR y crea un paquete reproducible
  python tools/hpk.py inspect app.hpk          muestra el manifest y el contenido
  python tools/hpk.py verify app.hpk           aplica las mismas comprobaciones que el instalador

El tar se normaliza: formato ustar, uid/gid 0 sin nombres, mtime fijo, orden alfabetico con
manifest.ini e icon.png primero (el shell los lee sin recorrer todo el archivo), modos 0644/0755.
Sin dependencias obligatorias; usa Pillow (icono) y pyelftools (ELF) si estan instalados."""
import argparse
import io
import os
import re
import struct
import sys
import tarfile

MTIME = 946684800                 # 2000-01-01: tar reproducible
HIBYMOD_VERSION = "0.1.0"         # igual que HPK_HIBYMOD_VERSION en src/hpk.inc.c
MAX_FILE = 256 << 20
MAX_ENTRIES = 4096
MAX_MANIFEST = 16384
MAX_ICON = 1 << 20
REQUIRED = ["id", "version", "min_hibymod", "name", "exec", "arch", "input"]
RECOMMENDED = ["name.en", "name.fr", "description", "description.en", "description.fr", "stop_player", "ram_kb"]
SEMVER = re.compile(r"^(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})\.(0|[1-9]\d{0,5})([-+][0-9A-Za-z.+-]+)?$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,47}$")


class HpkError(Exception):
    pass


def parse_manifest(text):
    if text.startswith("﻿"):
        text = text[1:]
    kv = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in "#;[" or "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] == '"':
            v = v[1:-1]
        kv[k.strip()] = v
    return kv


def vtuple(v):
    m = SEMVER.match(v)
    return tuple(int(x) for x in m.groups()[:3]) if m else None


def norm_path(name):
    """Igual que hpk_norm(): relativa, sin '..', sin control ni barra invertida."""
    if name.startswith("/"):
        raise HpkError("ruta absoluta: %r" % name)
    parts = []
    for p in name.split("/"):
        if p in ("", "."):
            continue
        if p == "..":
            raise HpkError("ruta con '..': %r" % name)
        if any(ord(c) < 0x20 or c in "\x7f\\" for c in p):
            raise HpkError("caracter no permitido en %r" % name)
        parts.append(p)
    return "/".join(parts)


def check_manifest(kv, warn, strict=False):
    for k in REQUIRED:
        if not kv.get(k):
            raise HpkError("manifest.ini: falta '%s'" % k)
    if "name.es" in kv and "name" not in kv:
        kv["name"] = kv["name.es"]
    if not ID_RE.match(kv["id"]) or ".." in kv["id"]:
        raise HpkError("id invalido %r (a-z 0-9 . _ -, max 48, sin '..')" % kv["id"])
    if not vtuple(kv["version"]):
        raise HpkError("version no es semver: %r" % kv["version"])
    if not vtuple(kv["min_hibymod"]):
        raise HpkError("min_hibymod no es semver: %r" % kv["min_hibymod"])
    if vtuple(kv["min_hibymod"]) > vtuple(HIBYMOD_VERSION):
        if strict:
            raise HpkError("necesita hibymod %s (este instalador es %s)" % (kv["min_hibymod"], HIBYMOD_VERSION))
        warn("min_hibymod %s es mayor que la version conocida %s" % (kv["min_hibymod"], HIBYMOD_VERSION))
    if kv["arch"] != "mipsel-hf":
        raise HpkError("arch debe ser mipsel-hf")
    if kv["input"] not in ("touch", "keys", "both"):
        raise HpkError("input debe ser touch, keys o both")
    if kv.get("stop_player", "0") not in ("0", "1"):
        raise HpkError("stop_player debe ser 0 o 1")
    if kv.get("ram_kb") and not (kv["ram_kb"].isdigit() and int(kv["ram_kb"]) <= 1 << 20):
        raise HpkError("ram_kb debe ser un entero")
    for k in RECOMMENDED:
        if k not in kv:
            warn("manifest.ini: conviene definir '%s'" % k)
    for k in ("name", "name.en", "name.fr"):
        if len(kv.get(k, "").encode()) > 47:
            warn("%s se truncara a 47 bytes" % k)
    for k in ("description", "description.en", "description.fr"):
        if len(kv.get(k, "").encode()) > 159:
            warn("%s se truncara a 159 bytes" % k)
    kv["exec"] = norm_path(kv["exec"])
    if not kv["exec"]:
        raise HpkError("exec vacio")
    return kv


def check_elf(data, name):
    """ELF32 LE, MIPS, ISA <= mips32r2, sin PT_INTERP, sin soft-float. Devuelve una descripcion."""
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise HpkError("%s: no es ELF32 little-endian" % name)
    etype, mach = struct.unpack_from("<HH", data, 16)
    flags, = struct.unpack_from("<I", data, 36)
    phoff, = struct.unpack_from("<I", data, 28)
    phes, phn = struct.unpack_from("<HH", data, 42)
    if mach != 8 or etype not in (2, 3):
        raise HpkError("%s: no es un ejecutable MIPS" % name)
    if flags & 0xF0000000 not in (0, 0x10000000, 0x50000000, 0x70000000):
        raise HpkError("%s: ISA no compatible (hace falta mips32r2 o inferior)" % name)
    if phes != 32 or not 0 < phn <= 64 or phoff + phn * 32 > len(data):
        raise HpkError("%s: cabeceras de programa invalidas" % name)
    fp = "?"
    for i in range(phn):
        pt, po, _, _, pf = struct.unpack_from("<5I", data, phoff + i * 32)
        if pt == 3:
            raise HpkError("%s: es dinamico (PT_INTERP); compila con -static" % name)
        if pt == 0x70000003 and pf >= 24 and po + 24 <= len(data):
            fp = {1: "hard (double)", 2: "hard (single)", 3: "soft", 5: "fpxx", 6: "fp64", 7: "fp64a"}.get(data[po + 7], str(data[po + 7]))
            if data[po + 7] == 3:
                raise HpkError("%s: coma flotante soft; usa mipsel-linux-musleabihf" % name)
    try:                                               # segunda opinion, si esta pyelftools
        from elftools.elf.elffile import ELFFile
        e = ELFFile(io.BytesIO(data))
        if any(s.header.p_type == "PT_INTERP" for s in e.iter_segments()):
            raise HpkError("%s: es dinamico" % name)
    except ImportError:
        pass
    return "ELF32 MIPS LE estatico, fp %s, %d KB" % (fp, len(data) // 1024)


def check_icon(data, warn):
    if len(data) > MAX_ICON:
        raise HpkError("icon.png pasa de 1 MB")
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise HpkError("icon.png no es PNG")
    w, h = struct.unpack(">II", data[16:24])
    if not (16 <= w <= 512 and 16 <= h <= 512):
        raise HpkError("icon.png debe medir entre 16 y 512 px (tiene %dx%d)" % (w, h))
    if w != h:
        warn("icon.png no es cuadrado: se recortara al centro")
    if w != 128:
        warn("icon.png: se recomienda 128x128 (tiene %dx%d)" % (w, h))
    try:
        from PIL import Image
        im = Image.open(io.BytesIO(data)).convert("RGBA")
        if im.getextrema()[3][0] < 255:
            warn("icon.png tiene transparencia: debe ser a sangre (la mascara la pone el instalador)")
    except ImportError:
        pass


# ------------------------------------------------------------------ build
def build(src, out):
    warns = []
    warn = warns.append
    mpath = os.path.join(src, "manifest.ini")
    if not os.path.isfile(mpath):
        raise HpkError("falta %s" % mpath)
    raw = open(mpath, "rb").read()
    if len(raw) > MAX_MANIFEST:
        raise HpkError("manifest.ini pasa de 16 KB")
    kv = check_manifest(parse_manifest(raw.decode("utf-8")), warn)
    files, dirs = [], []
    for root, dn, fn in os.walk(src):
        dn.sort()
        for d in dn:
            p = os.path.join(root, d)
            if os.path.islink(p):
                raise HpkError("enlace simbolico no permitido: %s" % p)
            dirs.append(os.path.relpath(p, src).replace(os.sep, "/"))
        for f in fn:
            p = os.path.join(root, f)
            if os.path.islink(p) or not os.path.isfile(p):
                raise HpkError("solo se admiten ficheros normales: %s" % p)
            rel = os.path.relpath(p, src).replace(os.sep, "/")
            if os.path.abspath(p) == os.path.abspath(out):
                continue
            files.append(rel)
    if kv["exec"] not in files:
        raise HpkError("exec=%s no esta en %s" % (kv["exec"], src))
    print(check_elf(open(os.path.join(src, kv["exec"]), "rb").read(), kv["exec"]))
    if "icon.png" in files:
        check_icon(open(os.path.join(src, "icon.png"), "rb").read(), warn)
    else:
        warn("no hay icon.png: el instalador generara uno de color liso")
    first = [f for f in ("manifest.ini", "icon.png") if f in files]
    order = [(f, False) for f in first] + sorted([(d, True) for d in dirs] + [(f, False) for f in files if f not in first])
    if len(order) > MAX_ENTRIES:
        raise HpkError("demasiados ficheros (max %d)" % MAX_ENTRIES)
    tmp = out + ".part"
    with tarfile.open(tmp, "w", format=tarfile.USTAR_FORMAT) as t:
        for name, isdir in order:
            ti = tarfile.TarInfo(name)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            ti.mtime = MTIME
            if isdir:
                ti.type, ti.mode = tarfile.DIRTYPE, 0o755
                t.addfile(ti)
                continue
            p = os.path.join(src, name)
            size = os.path.getsize(p)
            if size > MAX_FILE:
                raise HpkError("%s pasa de 256 MB" % name)
            ti.size = size
            ti.mode = 0o755 if (name == kv["exec"] or os.access(p, os.X_OK) and open(p, "rb").read(4) in (b"\x7fELF", b"#!/b")) else 0o644
            with open(p, "rb") as f:
                t.addfile(ti, f)
    os.replace(tmp, out)
    for w in warns:
        print("aviso:", w)
    print("%s: %s %s, %d ficheros, %d KB" % (out, kv["id"], kv["version"], len(files), os.path.getsize(out) // 1024))


# ------------------------------------------------------------------ lectura y verificacion
def read_members(path):
    """Recorre el tar con las mismas reglas que hpk.inc.c (no confia en tarfile)."""
    data = open(path, "rb").read()
    off, out = 0, []
    while True:
        if off == len(data):
            break
        if off + 512 > len(data):
            raise HpkError("archivo truncado")
        b = data[off:off + 512]
        if not any(b):
            break
        if b[257:262] != b"ustar":
            raise HpkError("no es ustar")
        try:
            ck = int(b[148:156].split(b"\0")[0].strip() or b"x", 8)
        except ValueError:
            raise HpkError("suma de comprobacion ilegible")
        s = sum(b[:148]) + 256 + sum(b[156:])
        ss = sum((x - 256 if x > 127 else x) for x in b[:148] + b[156:]) + 256
        if ck not in (s, ss):
            raise HpkError("suma de comprobacion erronea en la cabecera %d" % (off // 512))
        name = b[:100].split(b"\0")[0].decode("utf-8", "replace")
        pre = b[345:500].split(b"\0")[0].decode("utf-8", "replace")
        if pre:
            name = pre + "/" + name
        typ = chr(b[156]) if b[156] else "0"
        if b[124] & 0x80:
            raise HpkError("tamano en base-256 no admitido")
        size = int(b[124:136].split(b"\0")[0].strip() or b"0", 8)
        mode = int(b[100:108].split(b"\0")[0].strip() or b"644", 8)
        if typ not in "05":
            raise HpkError("%s: tipo '%s' no permitido (solo ficheros y directorios)" % (name, typ))
        if size > MAX_FILE or off + 512 + size > len(data):
            raise HpkError("%s: tamano invalido" % name)
        n = norm_path(name)
        if n:
            out.append((n, typ, size, mode, data[off + 512:off + 512 + size]))
        elif typ != "5":
            raise HpkError("entrada sin nombre")
        off += 512 + (size + 511) // 512 * 512
        if len(out) > MAX_ENTRIES:
            raise HpkError("demasiadas entradas")
    if not out:
        raise HpkError("archivo vacio")
    return out


def verify(path, quiet=False):
    warns = []
    m = read_members(path)
    names = [x[0] for x in m]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise HpkError("entradas repetidas: %s" % ", ".join(sorted(dup)))
    by = {x[0]: x for x in m}
    for n, t, _, _, _ in m:                        # un fichero no puede ser a la vez directorio
        p = n.rsplit("/", 1)[0] if "/" in n else ""
        while p:
            if p in by and by[p][1] != "5":
                raise HpkError("%s esta dentro de %s, que es un fichero" % (n, p))
            p = p.rsplit("/", 1)[0] if "/" in p else ""
    if "manifest.ini" not in by:
        raise HpkError("falta manifest.ini")
    if by["manifest.ini"][2] > MAX_MANIFEST:
        raise HpkError("manifest.ini demasiado grande")
    kv = check_manifest(parse_manifest(by["manifest.ini"][4].decode("utf-8")), warns.append, strict=True)
    if kv["exec"] not in by or by[kv["exec"]][1] != "0":
        raise HpkError("exec=%s no esta en el paquete" % kv["exec"])
    elf = check_elf(by[kv["exec"]][4], kv["exec"])
    if "icon.png" in by:
        check_icon(by["icon.png"][4], warns.append)
    if not quiet:
        for w in warns:
            print("aviso:", w)
        print("OK %s %s (%s)" % (kv["id"], kv["version"], elf))
    return kv, m


def inspect(path):
    kv, m = verify(path, quiet=True)
    w = max(len(k) for k in kv)
    for k, v in kv.items():
        print("%-*s = %s" % (w, k, v))
    print()
    total = 0
    for n, t, size, mode, _ in m:
        total += size
        print("%s %04o %9d  %s" % ("d" if t == "5" else "-", mode, size, n + ("/" if t == "5" else "")))
    print("%d entradas, %d KB descomprimido, %d KB el paquete" % (len(m), total // 1024, os.path.getsize(path) // 1024))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    b = sp.add_parser("build")
    b.add_argument("dir")
    b.add_argument("-o", "--out", required=True)
    sp.add_parser("inspect").add_argument("hpk")
    sp.add_parser("verify").add_argument("hpk")
    a = ap.parse_args()
    try:
        if a.cmd == "build":
            build(a.dir, a.out)
        elif a.cmd == "inspect":
            inspect(a.hpk)
        else:
            verify(a.hpk)
    except HpkError as e:
        print("error:", e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
