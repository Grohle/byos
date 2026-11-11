#!/usr/bin/env python3
"""
hibymod build: genera un .upt para HiBy R3 II 2025 a partir del firmware oficial.

Ejecutar en Linux/WSL (necesita unsquashfs/mksquashfs con lzo y pycdlib):
  python3 build.py --stock r3ii_2025.upt --overlay ../overlay --out r3ii_2025.upt
  python3 build.py --stock r3ii_2025.upt --roundtrip        # valida el empaquetador

Solo se modifica el rootfs. El kernel (xImage) se copia tal cual.
"""
import argparse, hashlib, io, os, re, shutil, stat, subprocess, sys, tempfile

import pycdlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # python -I: no script dir in sys.path
import vendor_edits                # edits to HiBy's own files, stored without HiBy's text

CHUNK = 524288          # tamano de trozo del firmware oficial
ALIGN = 4096            # img_size del oficial va alineado a 4 KiB
# particion rootfs del R3 II 2025 (/proc/mtd): 0x2D00000 bytes. Se deja un 5 %
# de margen para bloques defectuosos de la NAND.
ROOTFS_LIMIT = int(0x2D00000 * 0.95)
BUILD_ID_PATH = "etc/hibymod_build"

# Parches de texto sobre ficheros existentes: (ruta, texto_original, texto_nuevo).
# Si el texto original no aparece exactamente, la construccion se aborta.
TEXT_PATCHES = [
    # /usr/data (UBIFS en NAND) montado sin "-o sync": la base de datos SQLite
    # del reproductor escribia cada pagina de forma sincrona -> escaneos lentisimos.
    ("usr/bin/mount_ubifs.sh",
     "mount -o sync -t ubifs /dev/${ubi_name}_0 $mount_path",
     "mount -t ubifs /dev/${ubi_name}_0 $mount_path"),
    # Menu oficial de color de letras (codigo ya presente en hiby_player)
    ("usr/resource/config.json",
     '{"lyric_color_enable":0}',
     '{"lyric_color_enable":1}'),
    # Menu oficial "Modo USB" (permite activar ADB para diagnostico)
    ("usr/resource/set_functions.json",
     '{"usb_mode":0}',
     '{"usb_mode":1}'),
    # Funciones de serie ocultas (riesgo bajo, ver docs/research_hidden_firmware.md 3.1):
    # aparecen en Ajustes del sistema de HiBy
    ("usr/resource/set_functions.json", '{"dac_charge_disable":0}', '{"dac_charge_disable":1}'),
    ("usr/resource/set_functions.json", '{"car_mode":0}', '{"car_mode":1}'),
    ("usr/resource/set_functions.json", '{"car_mode_auto_play":0}', '{"car_mode_auto_play":1}'),
    ("usr/resource/set_functions.json", '{"standby":0}', '{"standby":1}'),
    ("usr/resource/set_functions.json", '{"double_touch_wakeup":0}', '{"double_touch_wakeup":1}'),
]

# Parches binarios: (ruta, md5 original, [(offset, original, nuevo)])
BIN_PATCHES = [
    # ES9219: los 8 filtros digitales (el driver forzaba todo valor != 6 a 0) y max 6 -> 7
    ("module_driver/codec_es9219.ko", "0d698e6f04fe301a063aec53b726f7cd",
     [(0x16e4, "0b200200", "00000000"),
      (0x260c, "06000000", "07000000"),
      (0x2610, "06000000", "07000000")]),
]


# The overlay holds only files written for ByOS. It may replace an official file only when
# that file is an image (our own artwork under HiBy's file name). Any other official file is
# changed through TEXT_PATCHES, BIN_PATCHES or tools/vendor_edits.json, which carry the change
# and not HiBy's text; putting an edited copy of it in overlay/ aborts the build.
OVERLAY_MAY_REPLACE = (".png", ".jpg", ".jpeg")


def overlay_replace_ok(rel, edited_paths):
    """None if the overlay file `rel` may replace an existing official file, else the reason."""
    rel = rel.replace(os.sep, "/")
    if rel in edited_paths:
        return f"overlay/{rel}: this file is built by tools/vendor_edits.json; remove it from overlay/"
    if not rel.lower().endswith(OVERLAY_MAY_REPLACE):
        return (f"overlay/{rel}: would replace a file of the official firmware. Do not store edited "
                "copies of HiBy's files; describe the change in tools/vendor_edits.json "
                "(python tools/vendor_edits.py --help)")
    return None


def overlay_file(rel, data):
    """Contenido final de un fichero de la capa y su modo si es nuevo (compartido con
    tools/hibyos_patcher.py)."""
    if data.startswith(b"#!") or rel.endswith((".sh", ".json", ".ini", ".view")):
        data = data.replace(b"\r\n", b"\n")
    mode = 0o755 if data.startswith((b"#!", b"\x7fELF")) else 0o644
    return data, mode


def run(cmd, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, check=True, **kw)


def md5(b):
    return hashlib.md5(b).hexdigest()


# ---------------------------------------------------------------- lectura .upt
def read_upt(path):
    iso = pycdlib.PyCdlib()
    iso.open(path)
    files = {}
    for d, _dirs, names in iso.walk(rr_path="/"):
        for n in names:
            p = (d.rstrip("/") + "/" + n)
            buf = io.BytesIO()
            iso.get_file_from_iso_fp(buf, rr_path=p)
            files[p.lstrip("/")] = buf.getvalue()
    iso.close()
    return files


def parse_update_in(text):
    imgs, cur = [], None
    for line in text.splitlines():
        line = line.strip()
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k == "img_type":
            cur = {"img_type": v}
            imgs.append(cur)
        elif cur is not None:
            cur[k] = v
    return imgs


def extract_image(files, name, md5sum):
    manifest = files[f"ota_v0/ota_md5_{name}.{md5sum}"].decode()
    hashes = [h for h in manifest.split() if h]
    data, prev = b"", md5sum
    for i, h in enumerate(hashes):
        chunk = files[f"ota_v0/{name}.{i:04d}.{prev}"]
        assert md5(chunk) == h, f"md5 of chunk {i} of {name}"
        data += chunk
        prev = h
    return data


# ---------------------------------------------------------- escritura .upt
def chunk_image(name, data):
    """Devuelve {ruta: bytes} con los trozos y el manifiesto, como el oficial."""
    out, hashes = {}, []
    whole = md5(data)
    prev = whole
    for i in range(0, len(data), CHUNK):
        c = data[i:i + CHUNK]
        out[f"ota_v0/{name}.{i // CHUNK:04d}.{prev}"] = c
        prev = md5(c)
        hashes.append(prev)
    out[f"ota_v0/ota_md5_{name}.{whole}"] = ("\n".join(hashes) + "\n").encode()
    return out, whole


def write_upt(path, files):
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, joliet=3, rock_ridge="1.09", vol_ident="CDROM",
            sys_ident="LINUX")
    iso.add_directory("/OTA_V0", rr_name="ota_v0", joliet_path="/ota_v0")
    used = set()
    for p in sorted(files):
        d, n = (p.rsplit("/", 1) if "/" in p else ("", p))
        base, _, ext = n.partition(".")
        stem = re.sub(r"[^A-Z0-9_]", "_", base.upper())[:8] or "F"
        tail = re.sub(r"[^A-Z0-9_]", "_", n.rsplit(".", 1)[-1].upper())[:3] if "." in n else ""
        k = 0
        while True:
            short = f"{stem}.{tail}" if tail else stem
            iso_dir = ("/" + d.upper()) if d else ""
            iso_p = f"{iso_dir}/{short};1"
            if iso_p not in used:
                break
            k += 1
            stem = (stem[:8 - len(str(k))] + str(k))
        used.add(iso_p)
        data = files[p]
        iso.add_fp(io.BytesIO(data), len(data), iso_p, rr_name=n,
                   joliet_path="/" + p)
    iso.write(path)
    iso.close()


# ------------------------------------------------------------- rootfs
def listing(sqfs):
    r = subprocess.run(["unsquashfs", "-lln", sqfs], check=True,
                       capture_output=True, text=True).stdout
    ent = {}
    for line in r.splitlines():
        m = re.match(r"^(\S+)\s+(\d+/\d+)\s+(\d+)\s+\S+\s+\S+\s+squashfs-root(.*)$", line)
        if m:
            path = m.group(4).split(" -> ")[0] or "/"
            ent[path] = (m.group(1), m.group(2), int(m.group(3)))
    return ent


def build_rootfs(stock_sqfs, overlay, work):
    root = os.path.join(work, "rootfs")
    # en user namespace no se puede hacer chown a uid 33/1000 -> rc=2 no fatal;
    # los propietarios se restauran con el fichero pseudo y se verifican despues
    r = subprocess.run(["unsquashfs", "-q", "-n", "-ig", "-d", root, stock_sqfs],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode not in (0, 2) or not os.path.isfile(os.path.join(root, "usr/bin/hiby_player")):
        raise SystemExit(f"unsquashfs failed (rc={r.returncode})")

    # a partir de aqui, permisos normales para lo que crea hibymod (755/644)
    os.umask(0o022)

    edits = vendor_edits.load()
    edited = {e["path"] for e in edits}

    # capa de ficheros nuevos/reemplazados
    if overlay:
        for d, _dirs, names in os.walk(overlay):
            for n in names:
                src = os.path.join(d, n)
                rel = os.path.relpath(src, overlay)
                dst = os.path.join(root, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                data, mode = overlay_file(rel, open(src, "rb").read())
                why = overlay_replace_ok(rel, edited) if (os.path.lexists(dst) or
                                                          rel.replace(os.sep, "/") in edited) else None
                if why:
                    raise SystemExit(why)
                if os.path.lexists(dst):
                    mode = stat.S_IMODE(os.lstat(dst).st_mode)  # conservar modo original
                    os.remove(dst)
                open(dst, "wb").write(data)
                os.chmod(dst, mode)
                print("  overlay:", rel)

    # edits to HiBy's own files: the ByOS version is rebuilt from the official copy and
    # replaces it exactly like an overlay file would (new file, original mode and owner)
    for e in edits:
        p = os.path.join(root, e["path"])
        if os.path.islink(p) or not os.path.isfile(p):
            raise SystemExit(f"vendor edit {e['path']}: not a regular file in the official firmware")
        try:
            data = vendor_edits.apply(e, open(p, "rb").read())
        except vendor_edits.EditError as err:
            raise SystemExit(f"vendor edit: {err}")
        mode = stat.S_IMODE(os.lstat(p).st_mode)
        os.remove(p)
        open(p, "wb").write(data)
        os.chmod(p, mode)
        print("  vendor edit:", e["path"])

    # identificador unico de esta construccion (invalida la "ruta rapida" de /usr/data)
    import uuid
    bid = uuid.uuid4().hex
    with open(os.path.join(root, BUILD_ID_PATH), "w") as f:
        f.write(bid + chr(10))
    print("  build id:", bid)

    # parches de texto
    for rel, old, new in TEXT_PATCHES:
        p = os.path.join(root, rel)
        t = open(p, "rb").read()
        o, nw = old.encode(), new.encode()
        cnt = t.count(o)
        # tolera espacios en JSON: compara sin espacios si no aparece literal
        if cnt != 1:
            raise SystemExit(f"patch {rel}: original text found {cnt} times (expected 1)")
        open(p, "wb").write(t.replace(o, nw))
        print("  patch:", rel)

    for rel, want_md5, edits in BIN_PATCHES:
        p = os.path.join(root, rel)
        b = bytearray(open(p, "rb").read())
        if md5(bytes(b)) != want_md5:
            raise SystemExit(f"binary patch {rel}: unexpected md5 {md5(bytes(b))}")
        for off, old, new in edits:
            o, nw = bytes.fromhex(old), bytes.fromhex(new)
            if bytes(b[off:off + len(o)]) != o:
                raise SystemExit(f"binary patch {rel}@{off:#x}: unexpected bytes")
            b[off:off + len(o)] = nw
        mode = stat.S_IMODE(os.lstat(p).st_mode)
        open(p, "wb").write(bytes(b))
        os.chmod(p, mode)
        print("  patch bin:", rel, md5(bytes(b)))

    # atributos especiales del original (propietarios no-root)
    orig = listing(stock_sqfs)
    pseudo = os.path.join(work, "pseudo.txt")
    with open(pseudo, "w") as f:
        for path, (mode, owner, _sz) in orig.items():
            if mode.startswith("l") or path == "/":
                continue
            uid, gid = owner.split("/")
            if uid == "0" and gid == "0" and not re.search(r"[sStT]", mode):
                continue
            if not os.path.lexists(root + path):
                continue
            perm = 0
            bits = mode[1:]
            for i, ch in enumerate(bits):
                if ch in "rwxst":
                    perm |= 1 << (8 - i)
            if bits[2] in "sS": perm |= 0o4000
            if bits[5] in "sS": perm |= 0o2000
            if bits[8] in "tT": perm |= 0o1000
            if bits[2] == "S": perm &= ~0o100
            if bits[5] == "S": perm &= ~0o010
            if bits[8] == "T": perm &= ~0o001
            f.write(f"{path} m {perm:o} {uid} {gid}\n")
    print(open(pseudo).read())

    out = os.path.join(work, "rootfs.squashfs")
    if os.geteuid() != 0:
        raise SystemExit("run as root or inside 'unshare -r' to keep file owners")
    run(["mksquashfs", root, out, "-comp", "lzo", "-b", "131072", "-noappend",
         "-no-xattrs", "-no-progress", "-quiet", "-pf", pseudo])
    return out, orig


def compare_listings(orig, new):
    print("\n=== rootfs differences (original -> new)")
    diffs = 0
    for p in sorted(set(orig) | set(new)):
        a, b = orig.get(p), new.get(p)
        if a is None:
            print("  + ", p, b); diffs += 1
        elif b is None:
            print("  - ", p, a); diffs += 1
        elif a[0] != b[0] or a[1] != b[1] or (a[2] != b[2] and not a[0].startswith("d")):
            print("  ~ ", p, a, "->", b); diffs += 1
    print(f"=== {diffs} differences\n")


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True)
    ap.add_argument("--overlay")
    ap.add_argument("--out")
    ap.add_argument("--roundtrip", action="store_true",
                    help="reempaqueta las imagenes originales sin cambios y compara")
    a = ap.parse_args()
    os.umask(0)  # conservar permisos exactos al extraer

    files = read_upt(a.stock)
    upd = files["ota_v0/ota_update.in"].decode()
    imgs = parse_update_in(upd)
    kern = next(i for i in imgs if i["img_type"] == "kernel")
    rfs = next(i for i in imgs if i["img_type"] == "rootfs")
    kdata = extract_image(files, kern["img_name"], kern["img_md5"])
    rdata = extract_image(files, rfs["img_name"], rfs["img_md5"])
    assert md5(kdata) == kern["img_md5"] and len(kdata) == int(kern["img_size"])
    assert md5(rdata) == rfs["img_md5"] and len(rdata) == int(rfs["img_size"])
    print("stock OK:", kern["img_name"], len(kdata), "|", rfs["img_name"], len(rdata))

    work = tempfile.mkdtemp(prefix="hibymod_")
    stock_sqfs = os.path.join(work, "stock.squashfs")
    open(stock_sqfs, "wb").write(rdata)

    if a.roundtrip:
        new_rdata = rdata
    else:
        sq, orig = build_rootfs(stock_sqfs, a.overlay, work)
        new_rdata = open(sq, "rb").read()
        compare_listings(orig, listing(sq))
        pad = (-len(new_rdata)) % ALIGN
        new_rdata += b"\0" * pad
        limit = ROOTFS_LIMIT
        print(f"rootfs: {len(new_rdata)} of {limit} bytes allowed ({len(new_rdata) * 100 // limit} %)")
        if len(new_rdata) > limit:
            raise SystemExit("rootfs too big for the partition; aborted")

    out_files = {"ota_config.in": files["ota_config.in"],
                 "ota_v0/ota_v0.ok": files["ota_v0/ota_v0.ok"]}
    kc, kmd5 = chunk_image(kern["img_name"], kdata)
    rc, rmd5 = chunk_image(rfs["img_name"], new_rdata)
    out_files.update(kc); out_files.update(rc)
    # la sustitucion de md5/size solo debe afectar al bloque rootfs
    head, sep, tail = upd.partition("img_type=rootfs")
    tail = tail.replace(f"img_md5={rfs['img_md5']}", f"img_md5={rmd5}") \
               .replace(f"img_size={rfs['img_size']}", f"img_size={len(new_rdata)}")
    new_upd = head + sep + tail
    out_files["ota_v0/ota_update.in"] = new_upd.encode()

    if a.roundtrip:
        same = set(out_files) == set(files) and all(out_files[k] == files[k] for k in files)
        print("ROUNDTRIP: files identical to the original =", same)
        if not same:
            for k in sorted(set(out_files) | set(files)):
                if out_files.get(k) != files.get(k):
                    print("  differs:", k)
            sys.exit(1)
        out = a.out or os.path.join(work, "roundtrip.upt")
    else:
        out = a.out

    write_upt(out, out_files)

    # verificacion final: releer el .upt generado como lo haria el recovery
    chk = read_upt(out)
    imgs2 = parse_update_in(chk["ota_v0/ota_update.in"].decode())
    for i in imgs2:
        d = extract_image(chk, i["img_name"], i["img_md5"])
        assert md5(d) == i["img_md5"] and len(d) == int(i["img_size"]), i
        print("verified:", i["img_name"], i["img_size"], i["img_md5"])
    if not a.roundtrip:
        open(os.path.join(work, "check.squashfs"), "wb").write(
            extract_image(chk, rfs["img_name"], rmd5))
        run(["unsquashfs", "-s", os.path.join(work, "check.squashfs")],
            stdout=subprocess.DEVNULL)
    print("\nOK ->", out, os.path.getsize(out), "bytes")
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
