#!/usr/bin/env python3
"""Regenerates the reference data of the :core unit tests from the Python tools of HiByOS.

  python android/hibyos-install/tools/gen_test_vectors.py lzo iso      (needs pycdlib)
  python3 android/hibyos-install/tools/gen_test_vectors.py squashfs    (Linux or WSL, needs
                                                                        mksquashfs + unsquashfs)

Everything is made from generated data (a small xorshift32 generator shared with the Kotlin
tests): no HiBy file is involved. Output goes to core/src/test/resources/:

  lzo_vectors.txt      size and SHA-256 of tools/lzo1x.py compress() for many inputs
  iso_vector.txt       size and SHA-256 of a container written by tools/build.py write_upt()
  synthetic.squashfs   a small image made by mksquashfs (the reference implementation)
  synthetic.txt        what tools/squashfs.py reads from it and the SHA-256 of what it writes
                       after a fixed list of changes (also checked here with unsquashfs)
"""
import calendar
import hashlib
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(APP, "..", ".."))
RES = os.path.join(APP, "core", "src", "test", "resources")
sys.path.insert(0, os.path.join(REPO, "tools"))

WORDS = ["hiby", "os", "player", "music", "the", "and", "settings", "wifi", "volume", "track",
         "album", "artist", "\n"]
KINDS = ["random", "zeros", "text", "runs", "copies"]
SIZES = [0, 1, 3, 4, 12, 13, 14, 17, 18, 19, 238, 239, 256, 1000, 4096, 8192, 65536, 131072, 200000]


class Rng:
    def __init__(self, seed):
        self.s = seed or 0x1234567

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def below(self, n):
        return self.next() % n


def gen(kind, seed, size):
    """Test data. The Kotlin twin is TestData.gen()."""
    r = Rng(seed)
    out = bytearray()
    if kind == "zeros":
        return bytes(size)
    if kind == "random":
        while len(out) < size:
            out.append(r.next() & 0xFF)
    elif kind == "text":
        while len(out) < size:
            out += WORDS[r.below(len(WORDS))].encode() + b" "
    elif kind == "runs":
        while len(out) < size:
            b = r.below(256)
            out += bytes([b]) * (1 + r.below(300))
    elif kind == "copies":
        while len(out) < 64:
            out.append(r.next() & 0xFF)
        while len(out) < size:
            if r.below(4) == 0:
                for _ in range(1 + r.below(8)):
                    out.append(r.next() & 0xFF)
            else:
                dist = 1 + r.below(min(len(out), 60000))
                for _ in range(3 + r.below(400)):
                    out.append(out[len(out) - dist])
    else:
        raise ValueError(kind)
    return bytes(out[:size])


def sha(b):
    return hashlib.sha256(b).hexdigest()


def write(name, text):
    os.makedirs(RES, exist_ok=True)
    with open(os.path.join(RES, name), "w", newline="\n", encoding="utf-8") as f:
        f.write(text)
    print("wrote", os.path.join(RES, name))


# ------------------------------------------------------------------------------ lzo
def do_lzo():
    import lzo1x
    lines = ["# kind seed size compressed_size sha256(compressed)   -- tools/lzo1x.py compress()"]
    seed = 1
    for kind in KINDS:
        for size in SIZES:
            data = gen(kind, seed, size)
            comp = lzo1x.compress(data)
            assert lzo1x.decompress(comp, len(data)) == data
            lines.append(f"{kind} {seed} {size} {len(comp)} {sha(comp)}")
            seed += 1
    write("lzo_vectors.txt", "\n".join(lines) + "\n")


# ------------------------------------------------------------------------------ iso
ISO_EPOCH = 1767225600


def iso_files(build):
    files = {"ota_config.in": gen("text", 7001, 190),
             "ota_v0/ota_v0.ok": b"ok\n",
             "ota_v0/ota_update.in": gen("text", 7002, 420)}
    for name, seed, size in (("xImage", 7003, 700000), ("rootfs.squashfs", 7004, 1300001)):
        chunks, _md5 = build.chunk_image(name, gen("copies", seed, size))
        files.update(chunks)
    return files


def do_iso():
    import build
    files = iso_files(build)
    out = os.path.join(APP, "build", "iso_vector.upt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    real = time.time
    try:
        time.time = lambda: float(ISO_EPOCH)
        build.write_upt(out, files)
    finally:
        time.time = real
    with open(out, "rb") as f:
        blob = f.read()
    offset = calendar.timegm(time.localtime(ISO_EPOCH)) - ISO_EPOCH
    names = sorted(files)
    write("iso_vector.txt",
          "# container written by tools/build.py write_upt() (pycdlib) from generated files\n"
          f"epoch {ISO_EPOCH}\nutc_offset {offset}\nsize {len(blob)}\nsha256 {sha(blob)}\n"
          f"files {len(names)}\nnames_sha256 {sha(chr(10).join(names).encode())}\n")
    os.remove(out)


# ------------------------------------------------------------------------- squashfs
SQ_FILES = [                       # path, kind, seed, size, mode
    ("empty", "zeros", 0, 0, 0o644),
    ("small.txt", "text", 11, 300, 0o644),
    ("block.bin", "text", 12, 131072, 0o600),
    ("multi.bin", "copies", 13, 300000, 0o644),
    ("random.bin", "random", 14, 70000, 0o644),
    ("zeros.bin", "zeros", 0, 400000, 0o644),
    ("dir/sub/deep.txt", "text", 15, 5000, 0o644),
    ("etc/config.txt", "text", 16, 2000, 0o644),
    ("bin/tool", "runs", 17, 9000, 0o755),
    ("lib/driver.ko", "copies", 18, 150000, 0o644),
]
SQ_MANY = 300
SQ_ODD_NAME = "ñandú ü.txt"


def sq_changes(squashfs, img, root):
    """The changes the Kotlin test (SquashFsTest.changes) makes too."""
    def node(path):
        n = root
        for part in path.split("/"):
            n = n.children[part.encode()]
        return n
    node("small.txt").set_data(gen("text", 101, 5000))                 # also seen through dir/hard
    f = squashfs.Inode("file", 0o755, 0, 0, img.mtime)
    f.set_data(gen("copies", 102, 200000))
    node("dir").children[b"new.bin"] = f
    d = squashfs.Inode("dir", 0o755, 0, 0, img.mtime)
    a = squashfs.Inode("file", 0o644, 1000, 100, img.mtime)
    a.set_data(gen("text", 103, 17))
    d.children[b"a"] = a
    ln = squashfs.Inode("symlink", 0o777, 0, 0, img.mtime)
    ln.target = b"a"
    d.children[b"ln"] = ln
    root.children[b"newdir"] = d
    del node("many").children[b"f0007"]
    node("bin/tool").mode = 0o700
    node("zeros.bin").set_data(b"")
    ko = bytearray(img.read_file(node("lib/driver.ko")))
    ko[1000:1004] = b"\xde\xad\xbe\xef"
    node("lib/driver.ko").set_data(bytes(ko))


def sq_listing(squashfs, img, root):
    lines = []
    for path, n in squashfs.walk(root):
        if n.kind == "file":
            extra = f"{n.size} {sha(img.read_file(n) if n.data is None else n.data)}"
        elif n.kind == "symlink":
            extra = n.target.hex()
        elif n.kind in ("blkdev", "chrdev"):
            extra = str(n.rdev)
        else:
            extra = "-"
        lines.append(f"{path.hex()} {n.kind} {n.mode:o} {n.uid} {n.gid} {extra}")
    return lines


def do_squashfs():
    import squashfs
    for tool in ("mksquashfs", "unsquashfs"):
        if shutil.which(tool) is None:
            sys.exit(f"{tool} not found: run this part on Linux or in WSL")
    work = os.path.join(os.path.expanduser("~"), ".cache", "hibyos-install-test")
    shutil.rmtree(work, ignore_errors=True)
    tree = os.path.join(work, "tree")
    expected = {}
    for path, kind, seed, size, mode in SQ_FILES:
        p = os.path.join(tree, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        data = gen(kind, seed, size)
        with open(p, "wb") as f:
            f.write(data)
        os.chmod(p, mode)
        expected[path] = data
    os.makedirs(os.path.join(tree, "many"))
    for i in range(SQ_MANY):
        data = gen("text", 1000 + i, 20 + i)
        with open(os.path.join(tree, "many", f"f{i:04d}"), "wb") as f:
            f.write(data)
        expected[f"many/f{i:04d}"] = data
    with open(os.path.join(tree, "many", SQ_ODD_NAME).encode(), "wb") as f:
        f.write(b"odd name\n")
    expected["many/" + SQ_ODD_NAME] = b"odd name\n"
    os.symlink("../small.txt", os.path.join(tree, "dir", "link"))
    os.link(os.path.join(tree, "small.txt"), os.path.join(tree, "dir", "hard"))
    expected["dir/hard"] = expected["small.txt"]
    os.makedirs(os.path.join(tree, "dev"))
    os.makedirs(os.path.join(tree, "emptydir"))
    os.mkfifo(os.path.join(tree, "dev", "fifo"))
    image = os.path.join(RES, "synthetic.squashfs")
    os.makedirs(RES, exist_ok=True)
    subprocess.run(["mksquashfs", tree, image, "-comp", "lzo", "-b", "131072", "-noappend", "-no-xattrs",
                    "-no-progress", "-quiet", "-mkfs-time", "1700000000", "-all-time", "1700000000",
                    "-p", "dev/null c 666 0 0 1 3", "-p", "dev/sda b 660 0 6 8 0",
                    "-p", "bin/tool m 4755 1000 100"], check=True)
    with open(image, "rb") as f:
        raw = f.read()

    img = squashfs.Image(raw)
    root = img.read_tree()
    for path, data in expected.items():                       # the Python reader against the truth
        n = root
        for part in path.split("/"):
            n = n.children[part.encode()]
        assert img.read_file(n) == data, path
    before = sq_listing(squashfs, img, root)

    same, _st = squashfs.write_image(root, img, mtime=1234567890)     # rewrite without changes
    sq_changes(squashfs, img, root)
    after = sq_listing(squashfs, img, root)
    new, st = squashfs.write_image(root, img)

    # the reference tool must read the image written by tools/squashfs.py and give the same files
    out_img = os.path.join(work, "new.squashfs")
    with open(out_img, "wb") as f:
        f.write(new + b"\0" * (-len(new) % 4096))
    dest = os.path.join(work, "extract")
    # (device nodes cannot be created without being root: that is not an error of the image)
    subprocess.run(["unsquashfs", "-no-progress", "-quiet", "-no-xattrs", "-no-exit-code", "-d", dest, out_img],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    lls = subprocess.run(["unsquashfs", "-lls", out_img], check=True, capture_output=True).stdout
    listed = [l for l in lls.splitlines() if b" squashfs-root" in l]
    assert len(listed) == len(list(squashfs.walk(root))), (len(listed), len(list(squashfs.walk(root))))
    assert any(l.startswith(b"crw-rw-rw-") and l.endswith(b"/dev/null") for l in listed), "dev/null"
    checked = 0
    for path, n in squashfs.walk(root):
        if path == b"/":
            continue
        p = os.path.join(dest.encode(), path[1:])
        if n.kind == "file":
            with open(p, "rb") as f:
                assert f.read() == (img.read_file(n) if n.data is None else n.data), path
            assert (os.lstat(p).st_mode & 0o7777) == n.mode, path
            checked += 1
        elif n.kind == "symlink":
            assert os.readlink(p) == n.target, path
        elif n.kind == "dir":
            assert os.path.isdir(p), path
    print(f"unsquashfs agrees: {checked} files of the image written by tools/squashfs.py")

    write("synthetic.txt",
          "# synthetic.squashfs was made by mksquashfs 4.x (-comp lzo) from generated data\n"
          f"image {len(raw)} {sha(raw)}\n"
          f"rewrite 1234567890 {len(same)} {sha(same)}\n"
          f"changed {len(new)} {sha(new)}\n"
          f"stats {st['kept_data']} {st['new_data_in']} {st['new_data_out']} {st['metadata']}\n"
          + "".join(f"before {l}\n" for l in before)
          + "".join(f"after {l}\n" for l in after))
    shutil.rmtree(work, ignore_errors=True)


def main():
    parts = sys.argv[1:] or ["lzo", "iso", "squashfs"]
    for p in parts:
        {"lzo": do_lzo, "iso": do_iso, "squashfs": do_squashfs}[p]()


if __name__ == "__main__":
    main()
