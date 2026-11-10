#!/usr/bin/env python3
"""
HiByOS patcher: turns YOUR copy of the official HiBy R3 II (2025) firmware v1.3
(r3ii_2025.upt) into the HiByOS firmware, on your own computer.

HiByOS does not redistribute HiBy's firmware. This program only contains the HiByOS
files (the "overlay") and the list of changes (tools/build.py and tools/vendor_edits.json,
which store our changes and none of HiBy's text); it modifies your copy locally.

Pure Python, no external tools, no root/admin rights:
  Windows:      HiByOS-Patcher.exe path\\to\\r3ii_2025.upt   (or drag the .upt onto the .exe)
  macOS/Linux:  python3 hibyos_patcher.py /path/to/r3ii_2025.upt

The result is written to a "HiByOS-<version>" folder next to the input file.
It produces the same root filesystem content as tools/build.py (the Linux build), see
docs/PATCHER.md. Unlike build.py it does not recompress HiBy's data: the compressed blocks
of untouched files are copied byte for byte.
"""
import argparse
import hashlib
import os
import re
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = getattr(sys, "_MEIPASS", HERE)          # PyInstaller one-file bundle
# our modules first (an unrelated installed package called "build" must not win);
# the bundled copy of pycdlib in vendor/ is only a fallback
for _p in (HERE, BASE):
    if _p in sys.path:
        sys.path.remove(_p)
    sys.path.insert(0, _p)
for _p in (os.path.join(BASE, "vendor"), os.path.join(HERE, "vendor")):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.append(_p)

try:
    import build                       # tools/build.py: .upt format + patch definitions
    import squashfs
    import vendor_edits                # edits to HiBy's own files, stored without HiBy's text
except ImportError as e:               # pragma: no cover
    print("ERROR: a required module is missing:", e)
    print("On macOS/Linux install it with:  python3 -m pip install pycdlib")
    sys.exit(2)
if not hasattr(build, "TEXT_PATCHES") or not hasattr(build, "overlay_file"):
    print("ERROR: the wrong 'build' module was loaded:", getattr(build, "__file__", "?"))
    sys.exit(2)

SUPPORTED_MD5 = "0d16d09c1458dd2f2cdaf38d0047917a"   # official R3 II (2025) v1.3, 2 March 2026
OUT_NAME = "r3ii_2025.upt"


class PatchError(Exception):
    pass


def step(msg):
    print("\n== " + msg, flush=True)


def info(msg):
    print("   " + msg, flush=True)


def find_overlay(cli):
    cands = [cli] if cli else [os.path.join(BASE, "overlay"), os.path.join(HERE, "overlay"),
                                os.path.join(HERE, "..", "overlay")]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "usr", "bin", "hibymod")):
            return os.path.abspath(c)
    raise PatchError("the HiByOS files (overlay folder) were not found next to the patcher")


def hibyos_version(overlay):
    t = open(os.path.join(overlay, "usr", "bin", "hibymod"), "rb").read().decode("utf-8", "replace")
    m = re.search(r"HiByOS (\d+(?:\.\d+)+)", t)
    if not m:
        raise PatchError("cannot read the HiByOS version from the overlay")
    return m.group(1)


def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------ tree editing
def lookup(root, rel):
    node = root
    for part in rel.encode().split(b"/"):
        if node.kind != "dir":
            raise PatchError(f"/{rel}: a parent is not a directory")
        if part not in node.children:
            return None
        node = node.children[part]
    return node


def content(img, node):
    return node.data if node.data is not None else img.read_file(node)


def normalize_like_build_py(root):
    """build.py extracts the image inside a user namespace, where unsquashfs cannot restore
    owners, and then restores, through a mksquashfs pseudo file, the owner and mode of every
    original entry that is not a symlink and is not root:root or has setuid/setgid/sticky
    bits. Net effect: symlinks and the root directory end up root:root (mode 0777 for
    symlinks), everything else keeps its original owner and mode. Device nodes, FIFOs and
    sockets cannot be extracted there at all, so they are not supported here either."""
    for path, n in squashfs.walk(root):
        if n.kind == "symlink":
            n.uid = n.gid = 0
            n.mode = 0o777
        elif n.kind not in ("dir", "file"):
            raise PatchError(f"unsupported special file in the firmware: {path.decode()}")
    root.uid = root.gid = 0


def apply_overlay(root, overlay, mtime, edited=()):
    count = 0
    for d, dirs, names in os.walk(overlay):
        dirs.sort()
        for name in sorted(names):
            src = os.path.join(d, name)
            rel = os.path.relpath(src, overlay)
            data, mode = build.overlay_file(rel, open(src, "rb").read())
            parts = rel.replace(os.sep, "/").encode().split(b"/")
            node = root
            for part in parts[:-1]:
                child = node.children.get(part)
                if child is None:                  # new directory: root:root 0755
                    child = squashfs.Inode("dir", 0o755, 0, 0, mtime)
                    node.children[part] = child
                elif child.kind != "dir":
                    raise PatchError(f"overlay {rel}: /{part.decode()} is not a directory in the firmware")
                node = child
            uid = gid = 0
            old = node.children.get(parts[-1])
            if old is not None or rel.replace(os.sep, "/") in edited:
                why = build.overlay_replace_ok(rel, edited)
                if why:
                    raise PatchError(why)
            if old is not None:
                if old.kind != "file":
                    raise PatchError(f"overlay {rel}: replaces a {old.kind}, not supported")
                mode, uid, gid = old.mode, old.uid, old.gid   # original mode and owner are kept
            new = squashfs.Inode("file", mode, uid, gid, mtime)
            new.set_data(data)
            node.children[parts[-1]] = new             # breaks a hard link, like rm + create
            count += 1
    return count


def apply_vendor_edits(img, root, edits, mtime, log):
    """tools/vendor_edits.json: rebuild the HiByOS version of a few official files from the copy
    in this firmware. The new file replaces the official one exactly like an overlay file
    (new inode, original mode and owner)."""
    for e in edits:
        rel = e["path"]
        parent = lookup(root, os.path.dirname(rel)) if "/" in rel else root
        name = os.path.basename(rel).encode()
        old = parent.children.get(name) if parent is not None and parent.kind == "dir" else None
        if old is None or old.kind != "file":
            raise PatchError(f"vendor edit {rel}: not a regular file in the official firmware")
        try:
            data = vendor_edits.apply(e, content(img, old))
        except vendor_edits.EditError as err:
            raise PatchError(f"vendor edit: {err}")
        new = squashfs.Inode("file", old.mode, old.uid, old.gid, mtime)
        new.set_data(data)
        parent.children[name] = new
        log(f"edit: {rel}")
    return len(edits)


def write_in_place(img, root, rel, data):
    """Like open(path, 'wb').write(data) on an existing file: hard links share the change."""
    node = lookup(root, rel)
    if node is None or node.kind != "file":
        raise PatchError(f"/{rel} is missing or is not a regular file")
    node.set_data(data)
    return node


def apply_patches(img, root, log):
    for rel, old, new in build.TEXT_PATCHES:
        node = lookup(root, rel)
        if node is None or node.kind != "file":
            raise PatchError(f"patch {rel}: file not found")
        t = content(img, node)
        o, nw = old.encode(), new.encode()
        cnt = t.count(o)
        if cnt != 1:
            raise PatchError(f"patch {rel}: original text found {cnt} times (expected 1)")
        write_in_place(img, root, rel, t.replace(o, nw))
        log(f"patch: {rel}")
    for rel, want_md5, edits in build.BIN_PATCHES:
        node = lookup(root, rel)
        if node is None or node.kind != "file":
            raise PatchError(f"binary patch {rel}: file not found")
        b = bytearray(content(img, node))
        if build.md5(bytes(b)) != want_md5:
            raise PatchError(f"binary patch {rel}: unexpected md5 {build.md5(bytes(b))}")
        for off, old, new in edits:
            o, nw = bytes.fromhex(old), bytes.fromhex(new)
            if bytes(b[off:off + len(o)]) != o:
                raise PatchError(f"binary patch {rel}@{off:#x}: unexpected bytes")
            b[off:off + len(o)] = nw
        write_in_place(img, root, rel, bytes(b))
        log(f"patch bin: {rel} {build.md5(bytes(b))}")


# ------------------------------------------------------------------ verification
def verify_rootfs(new_bytes, img, root):
    """Re-read the new image with an independent pass and compare every entry (type, mode,
    owner, link target, hard links, full content) with the intended tree."""
    nimg = squashfs.Image(new_bytes)
    s = nimg
    if not (96 <= s.inode_table < s.dir_table < s.frag_table < s.export_table < s.id_table < s.bytes_used):
        raise PatchError("verify: unexpected table layout")
    nroot = nimg.read_tree()
    want = list(squashfs.walk(root))
    got = list(squashfs.walk(nroot))
    if [p for p, _ in want] != [p for p, _ in got]:
        raise PatchError("verify: the file list differs")
    pair = {}
    files = 0
    for (path, a), (_p, b) in zip(want, got):
        if (a.kind, a.mode, a.uid, a.gid) != (b.kind, b.mode, b.uid, b.gid):
            raise PatchError(f"verify: attributes differ on {path.decode()}")
        if pair.setdefault(id(a), id(b)) != id(b):
            raise PatchError(f"verify: hard links differ on {path.decode()}")
        if a.kind == "symlink" and a.target != b.target:
            raise PatchError(f"verify: link target differs on {path.decode()}")
        if a.kind == "file":
            if a.size != b.size:
                raise PatchError(f"verify: size differs on {path.decode()}")
            if hashlib.sha256(content(img, a)).digest() != hashlib.sha256(nimg.read_file(b)).digest():
                raise PatchError(f"verify: content differs on {path.decode()}")
            files += 1
    if len(set(pair.values())) != len(pair):
        raise PatchError("verify: hard links differ")
    return len(want), files


def verify_upt(path, kern, kdata_md5, rfs_name, rdata):
    chk = build.read_upt(path)
    imgs = build.parse_update_in(chk["ota_v0/ota_update.in"].decode())
    seen = {}
    for i in imgs:
        d = build.extract_image(chk, i["img_name"], i["img_md5"])   # checks the md5 chain
        if build.md5(d) != i["img_md5"] or len(d) != int(i["img_size"]):
            raise PatchError(f"verify: {i['img_name']} md5/size mismatch")
        seen[i["img_type"]] = (i["img_name"], d)
        info(f"verified: {i['img_name']} {i['img_size']} bytes md5 {i['img_md5']}")
    if seen["kernel"][0] != kern or build.md5(seen["kernel"][1]) != kdata_md5:
        raise PatchError("verify: the kernel image changed")
    if seen["rootfs"][0] != rfs_name or seen["rootfs"][1] != rdata:
        raise PatchError("verify: the rootfs image in the .upt is not the one built")


# ------------------------------------------------------------------ main
def patch(upt_path, out_dir=None, overlay=None, build_id=None):
    t0 = time.time()
    overlay = find_overlay(overlay)
    version = hibyos_version(overlay)
    print(f"HiByOS patcher {version}")

    step("Checking the official firmware")
    upt_path = os.path.abspath(upt_path)
    if not os.path.isfile(upt_path):
        raise PatchError(f"file not found: {upt_path}")
    got = file_md5(upt_path)
    if got != SUPPORTED_MD5:
        raise PatchError("this is not the supported official firmware.\n"
                         f"   your file: {got}\n"
                         f"   expected:  {SUPPORTED_MD5}  (HiBy R3 II 2025, firmware v1.3 of 2 March 2026)\n"
                         "   HiByOS only patches that exact file. Nothing was changed.")
    info("OK: HiBy R3 II (2025) firmware v1.3")

    step("Reading the firmware")
    files = build.read_upt(upt_path)
    upd = files["ota_v0/ota_update.in"].decode()
    imgs = build.parse_update_in(upd)
    kern = next(i for i in imgs if i["img_type"] == "kernel")
    rfs = next(i for i in imgs if i["img_type"] == "rootfs")
    kdata = build.extract_image(files, kern["img_name"], kern["img_md5"])
    rdata = build.extract_image(files, rfs["img_name"], rfs["img_md5"])
    if build.md5(kdata) != kern["img_md5"] or len(kdata) != int(kern["img_size"]):
        raise PatchError("kernel image check failed")
    if build.md5(rdata) != rfs["img_md5"] or len(rdata) != int(rfs["img_size"]):
        raise PatchError("rootfs image check failed")
    info(f"{kern['img_name']} {len(kdata)} bytes, {rfs['img_name']} {len(rdata)} bytes")
    img = squashfs.Image(rdata)
    root = img.read_tree()
    info(f"root filesystem: {img.inode_count} inodes, {len(list(squashfs.walk(root)))} entries")

    step("Applying the HiByOS changes")
    normalize_like_build_py(root)
    try:
        edits = vendor_edits.load()
    except (OSError, ValueError, KeyError, vendor_edits.EditError) as e:
        raise PatchError(f"cannot read the list of edits (vendor_edits.json): {e}")
    n = apply_overlay(root, overlay, img.mtime, {e["path"] for e in edits})
    info(f"{n} HiByOS files added or replaced")
    n = apply_vendor_edits(img, root, edits, img.mtime, lambda m: None)
    info(f"{n} official files edited in place (start-up scripts and screen layouts)")
    bid = build_id or uuid.uuid4().hex
    node = lookup(root, build.BUILD_ID_PATH)
    if node is None:
        node = squashfs.Inode("file", 0o644, 0, 0, img.mtime)
        lookup(root, os.path.dirname(build.BUILD_ID_PATH)).children[
            os.path.basename(build.BUILD_ID_PATH).encode()] = node
    node.set_data((bid + "\n").encode())
    info(f"build id: {bid}")
    apply_patches(img, root, info)

    step("Building the new root filesystem")
    new_rdata, st = squashfs.write_image(root, img)
    new_rdata += b"\0" * ((-len(new_rdata)) % build.ALIGN)
    limit = build.ROOTFS_LIMIT
    info(f"HiBy data kept as is: {st['kept_data']} bytes; new data: {st['new_data_in']} -> "
         f"{st['new_data_out']} bytes; metadata: {st['metadata']} bytes")
    info(f"rootfs: {len(new_rdata)} of {limit} bytes allowed ({len(new_rdata) * 100 / limit:.1f} %)")
    if len(new_rdata) > limit:
        raise PatchError("the new root filesystem is too big for the partition; aborted")

    step("Verifying the new root filesystem (reading back every file)")
    entries, nfiles = verify_rootfs(new_rdata, img, root)
    info(f"OK: {entries} entries, {nfiles} files identical to the intended content")

    step("Writing the HiByOS firmware")
    out_dir = os.path.abspath(out_dir or os.path.join(os.path.dirname(upt_path), f"HiByOS-{version}"))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, OUT_NAME)
    if os.path.normcase(os.path.abspath(out)) == os.path.normcase(upt_path):
        raise PatchError("the output would overwrite the input file; choose another folder")
    out_files = {"ota_config.in": files["ota_config.in"],
                 "ota_v0/ota_v0.ok": files["ota_v0/ota_v0.ok"]}
    kc, _kmd5 = build.chunk_image(kern["img_name"], kdata)
    rc, rmd5 = build.chunk_image(rfs["img_name"], new_rdata)
    out_files.update(kc)
    out_files.update(rc)
    head, sep, tail = upd.partition("img_type=rootfs")
    tail = tail.replace(f"img_md5={rfs['img_md5']}", f"img_md5={rmd5}") \
               .replace(f"img_size={rfs['img_size']}", f"img_size={len(new_rdata)}")
    out_files["ota_v0/ota_update.in"] = (head + sep + tail).encode()
    tmp = out + ".part"
    if os.path.exists(tmp):
        os.remove(tmp)
    sde = os.environ.get("SOURCE_DATE_EPOCH")
    real_time = time.time
    try:
        try:
            if sde:                     # reproducible ISO dates (used for testing)
                time.time = lambda: float(sde)
            build.write_upt(tmp, out_files)
        finally:
            time.time = real_time
        step("Verifying the written file")
        verify_upt(tmp, kern["img_name"], kern["img_md5"], rfs["img_name"], new_rdata)
        os.replace(tmp, out)
    except BaseException:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise
    print(f"\nDONE in {time.time() - t0:.0f} s.\n"
          f"HiByOS firmware: {out}\n"
          f"   size {os.path.getsize(out)} bytes, md5 {file_md5(out)}\n\n"
          "Next: copy this r3ii_2025.upt and the HiByOS/ folder from the release to the root\n"
          "of the SD card, then on the player: Settings > Firmware update > Via SD card.")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Turn the official HiBy R3 II (2025) firmware v1.3 "
                                             "into the HiByOS firmware.")
    ap.add_argument("upt", nargs="?", help="the official r3ii_2025.upt downloaded from HiBy")
    ap.add_argument("--out-dir", help="output folder (default: HiByOS-<version> next to the input)")
    ap.add_argument("--no-pause", action="store_true", help="do not wait for Enter at the end (Windows)")
    ap.add_argument("--overlay", help=argparse.SUPPRESS)
    ap.add_argument("--build-id", help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    pause = os.name == "nt" and not a.no_pause
    rc = 0
    try:
        path = a.upt
        if not path:
            print("HiByOS patcher: turns the official HiBy R3 II (2025) firmware v1.3 into HiByOS.")
            print("Drag the official r3ii_2025.upt onto this window (or type its path) and press Enter.")
            path = input("> ").strip()
            if path.startswith("& "):
                path = path[2:].strip()
            path = path.strip('"').strip("'")
            if not path:
                raise PatchError("no file given")
        patch(path, a.out_dir, a.overlay, a.build_id)
    except PatchError as e:
        print("\nERROR:", e)
        rc = 1
    except (KeyboardInterrupt, EOFError):
        print("\nCancelled.")
        rc = 1
    except OSError as e:                        # unreadable input, read-only folder, disk full...
        print(f"\nERROR: {e.strerror or e}: {e.filename or ''}")
        print("Check that the file can be read and that its folder is writable (or use --out-dir).")
        rc = 1
    except Exception as e:                      # unexpected: show it, never leave a half file
        import traceback
        traceback.print_exc()
        print("\nERROR: unexpected failure:", e)
        print("Nothing was installed. Please report this error.")
        rc = 1
    if pause:
        try:
            input("\nPress Enter to close this window...")
        except (EOFError, KeyboardInterrupt):
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
