#!/usr/bin/env python3
"""
hibymod_covers.py - Revisa y arregla caratulas para el HiBy R3 II 2025.

Por que: el R3 II tiene muy poca RAM y pantalla de 320 px. Una caratula de
3000x3000 ocupa ~36 MB al decodificarse (mas que la RAM libre), y las JPEG
"progresivas", PNG o CMYK suelen fallar en el decodificador del aparato
(caratula en blanco) o disparar picos de memoria durante el escaneo.

Que hace (por carpeta con musica):
  - Busca caratula de carpeta (cover/folder/front .jpg/.jpeg/.png/.bmp).
  - Revisa la caratula incrustada de cada pista.
  - Informa de problemas: demasiado grande, progresiva, PNG/BMP, CMYK, ausente.
  - Con --apply:
      * crea/reescribe cover.jpg: JPEG baseline RGB, 320x320 (configurable),
        a partir de la caratula de carpeta o, si no hay, de la incrustada.
      * con --fix-embedded tambien reduce la caratula incrustada de cada pista
        (solo toca la etiqueta de imagen; el audio no se recodifica).
  - Las imagenes originales de carpeta se mueven a .hibymod_backup/ (no se borran).

Uso:
  pip install mutagen Pillow
  python hibymod_covers.py E:\\Music                 # solo informe (no cambia nada)
  python hibymod_covers.py E:\\Music --apply         # crea cover.jpg optimizados
  python hibymod_covers.py E:\\Music --apply --fix-embedded
"""
import argparse, csv, io, os, shutil, sys

try:
    from PIL import Image
    from mutagen import File as MFile
    from mutagen.flac import FLAC, Picture
    from mutagen.id3 import ID3, APIC, ID3NoHeaderError
    from mutagen.mp4 import MP4, MP4Cover
except ImportError:
    sys.exit("Faltan dependencias: pip install mutagen Pillow")

AUDIO = {".flac", ".mp3", ".m4a", ".aac", ".alac", ".ogg", ".oga", ".opus", ".wav",
         ".aif", ".aiff", ".dsf", ".dff", ".ape", ".wma", ".wv", ".mp4", ".m4b"}
COVER_STEMS = ("cover", "folder", "front")
COVER_EXT = (".jpg", ".jpeg", ".png", ".bmp")
MAX_BYTES = 300 * 1024


def img_info(data):
    """(ancho, alto, formato, progresiva, modo) o None si no se puede leer."""
    try:
        im = Image.open(io.BytesIO(data))
        prog = bool(im.info.get("progressive") or im.info.get("progression"))
        return im.width, im.height, im.format, prog, im.mode
    except Exception:
        return None


def problems(data, size):
    inf = img_info(data)
    if inf is None:
        return ["ilegible"]
    w, h, fmt, prog, mode = inf
    p = []
    if max(w, h) > size * 2:
        p.append(f"grande {w}x{h}")
    if fmt != "JPEG":
        p.append(fmt or "formato?")
    if prog:
        p.append("progresiva")
    if mode not in ("RGB", "L"):
        p.append(f"modo {mode}")
    if len(data) > MAX_BYTES:
        p.append(f"{len(data) // 1024} KB")
    return p


def normalize(data, size, quality):
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGB", im.size, (0, 0, 0))
        bg.paste(im, mask=im.split()[-1])
        im = bg
    elif im.mode != "RGB":
        im = im.convert("RGB")
    # recorte cuadrado centrado solo si es casi cuadrada; si no, encajar
    w, h = im.size
    if abs(w - h) <= max(w, h) * 0.03:
        s = min(w, h)
        im = im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
        im = im.resize((size, size), Image.LANCZOS)
    else:
        im.thumbnail((size, size), Image.LANCZOS)
    out = io.BytesIO()
    im.save(out, "JPEG", quality=quality, optimize=True, progressive=False,
            subsampling="4:2:0")
    return out.getvalue()


def embedded_picture(path):
    """Devuelve los bytes de la caratula incrustada (frontal preferente) o None."""
    try:
        f = MFile(path)
    except Exception:
        return None
    if f is None:
        return None
    try:
        if isinstance(f, FLAC):
            pics = sorted(f.pictures, key=lambda p: p.type != 3)
            return pics[0].data if pics else None
        if isinstance(f, MP4):
            c = (f.tags or {}).get("covr")
            return bytes(c[0]) if c else None
        tags = getattr(f, "tags", None)
        if tags is None:
            return None
        if hasattr(tags, "getall"):  # ID3 (mp3, wav, aiff, dsf)
            ap = sorted(tags.getall("APIC"), key=lambda a: a.type != 3)
            return ap[0].data if ap else None
        for k in ("metadata_block_picture",):  # ogg/opus
            if k in tags:
                import base64
                return Picture(base64.b64decode(tags[k][0])).data
        for k in tags.keys():  # APE / WavPack
            if "cover art" in str(k).lower():
                v = tags[k].value
                return v.split(b"\0", 1)[1] if b"\0" in v else v
    except Exception:
        return None
    return None


def replace_embedded(path, jpg):
    """Sustituye la caratula incrustada por 'jpg'. Devuelve True si cambio algo."""
    f = MFile(path)
    if isinstance(f, FLAC):
        if not f.pictures:
            return False
        pic = Picture(); pic.type = 3; pic.mime = "image/jpeg"; pic.data = jpg
        im = Image.open(io.BytesIO(jpg)); pic.width, pic.height, pic.depth = im.width, im.height, 24
        f.clear_pictures(); f.add_picture(pic); f.save(); return True
    if isinstance(f, MP4):
        if not (f.tags and f.tags.get("covr")):
            return False
        f.tags["covr"] = [MP4Cover(jpg, imageformat=MP4Cover.FORMAT_JPEG)]; f.save(); return True
    tags = getattr(f, "tags", None)
    if tags is not None and hasattr(tags, "getall") and tags.getall("APIC"):
        tags.delall("APIC")
        tags.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="Cover", data=jpg))
        f.save(); return True
    if tags is not None and "metadata_block_picture" in tags:
        import base64
        pic = Picture(); pic.type = 3; pic.mime = "image/jpeg"; pic.data = jpg
        tags["metadata_block_picture"] = [base64.b64encode(pic.write()).decode()]
        f.save(); return True
    return False


def find_folder_cover(d, names):
    best = None
    for n in names:
        stem, ext = os.path.splitext(n.lower())
        if ext in COVER_EXT and stem in COVER_STEMS:
            # preferencia: cover > folder > front ; jpg > resto
            rank = (COVER_STEMS.index(stem), 0 if ext in (".jpg", ".jpeg") else 1)
            if best is None or rank < best[0]:
                best = (rank, n)
    return best[1] if best else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("music_dir")
    ap.add_argument("--apply", action="store_true", help="aplicar cambios (por defecto solo informa)")
    ap.add_argument("--fix-embedded", action="store_true", help="reducir tambien la caratula incrustada")
    ap.add_argument("--size", type=int, default=320, help="lado en px (pantalla R3 II = 320)")
    ap.add_argument("--quality", type=int, default=88)
    ap.add_argument("--report", default="hibymod_covers_report.csv")
    a = ap.parse_args()

    rows, stats = [], dict(dirs=0, ok=0, fixed=0, missing=0, emb_fixed=0, errors=0)
    for d, dirs, files in os.walk(a.music_dir):
        dirs[:] = [x for x in dirs if not x.startswith(".")]
        tracks = sorted(f for f in files if os.path.splitext(f)[1].lower() in AUDIO)
        if not tracks:
            continue
        stats["dirs"] += 1
        fc = find_folder_cover(d, files)
        fc_data = open(os.path.join(d, fc), "rb").read() if fc else None
        fc_prob = problems(fc_data, a.size) if fc_data else ["sin caratula de carpeta"]

        emb_bad, emb_src = 0, None
        for t in tracks:
            e = embedded_picture(os.path.join(d, t))
            if e:
                emb_src = emb_src or e
                if problems(e, a.size):
                    emb_bad += 1

        src = fc_data if fc_data and img_info(fc_data) else emb_src
        status = "ok" if (fc == "cover.jpg" and not fc_prob) else "revisar"
        if src is None:
            status = "SIN CARATULA"
            stats["missing"] += 1
        rows.append([d, fc or "", "; ".join(fc_prob), len(tracks), emb_bad, status])

        if not a.apply or src is None:
            if status == "ok":
                stats["ok"] += 1
            continue
        try:
            if status != "ok":
                jpg = normalize(src, a.size, a.quality)
                bk = os.path.join(d, ".hibymod_backup")
                for n in files:
                    stem, ext = os.path.splitext(n.lower())
                    if ext in COVER_EXT and stem in COVER_STEMS:
                        os.makedirs(bk, exist_ok=True)
                        shutil.move(os.path.join(d, n), os.path.join(bk, n))
                open(os.path.join(d, "cover.jpg"), "wb").write(jpg)
                stats["fixed"] += 1
            else:
                stats["ok"] += 1
                jpg = open(os.path.join(d, "cover.jpg"), "rb").read()
            if a.fix_embedded and emb_bad:
                for t in tracks:
                    p = os.path.join(d, t)
                    e = embedded_picture(p)
                    if e and problems(e, a.size):
                        if replace_embedded(p, jpg):
                            stats["emb_fixed"] += 1
        except Exception as ex:
            stats["errors"] += 1
            print("ERROR", d, ex)

    with open(a.report, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f, delimiter=";")
        w.writerow(["carpeta", "caratula_carpeta", "problemas_carpeta", "pistas",
                    "pistas_con_caratula_incrustada_problematica", "estado"])
        w.writerows(rows)

    print(f"Carpetas con musica: {stats['dirs']}")
    print(f"  ya correctas:       {stats['ok']}")
    print(f"  sin ninguna caratula: {stats['missing']}")
    if a.apply:
        print(f"  cover.jpg creados/optimizados: {stats['fixed']}")
        print(f"  pistas con caratula incrustada reducida: {stats['emb_fixed']}")
        print(f"  errores: {stats['errors']}")
    else:
        rev = sum(1 for r in rows if r[5] == "revisar")
        print(f"  a optimizar:        {rev}   (ejecuta con --apply)")
    print(f"Informe: {os.path.abspath(a.report)}")


if __name__ == "__main__":
    main()
