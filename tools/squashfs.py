"""
Minimal pure-Python SquashFS 4.0 (LZO) reader and writer, used by the HiByOS patcher.

The writer does NOT recompress existing data: compressed data blocks and fragment blocks
of files that are kept from the source image are copied byte for byte (only relocated).
New or changed file data is LZO1X compressed (tools/lzo1x.py) or stored uncompressed when
compression does not help, exactly like mksquashfs does per block.

Output layout follows mksquashfs: superblock, data + fragment blocks, inode table,
directory table, fragment table, export (NFS lookup) table, id table. No xattrs.
"""
import bisect
import hashlib
import struct

import lzo1x

MAGIC = 0x73717368
META_SIZE = 8192
INVALID_BLK = 0xFFFFFFFFFFFFFFFF
NO_FRAG = 0xFFFFFFFF
NO_XATTR = 0xFFFFFFFF
UNCOMP_BLOCK = 1 << 24          # data/fragment size field: block stored uncompressed
COMP_LZO = 3

FLAG_UNCOMP_INODES = 0x0001
FLAG_COMP_OPT = 0x0400
FLAG_DUPLICATES = 0x0040
FLAG_EXPORTABLE = 0x0080
FLAG_NO_XATTRS = 0x0200

# inode types
T_DIR, T_REG, T_SYM, T_BLK, T_CHR, T_FIFO, T_SOCK = 1, 2, 3, 4, 5, 6, 7
T_LDIR, T_LREG, T_LSYM, T_LBLK, T_LCHR, T_LFIFO, T_LSOCK = 8, 9, 10, 11, 12, 13, 14
KIND = {1: "dir", 2: "file", 3: "symlink", 4: "blkdev", 5: "chrdev", 6: "fifo", 7: "socket"}
BASIC = {v: k for k, v in KIND.items()}

SB_FMT = "<IIIIIHHHHHHQQQQQQQQ"     # 96 bytes


class SquashfsError(Exception):
    pass


class Inode:
    """One inode. Hard links are several directory entries pointing to the same object.

    kind: dir/file/symlink/blkdev/chrdev/fifo/socket. mode: permission bits (12 bits).
    For files, exactly one of `data` (bytes, new content) or `orig` (reference to the
    blocks in the source image) is set."""
    __slots__ = ("kind", "mode", "uid", "gid", "mtime", "ino", "children", "size",
                 "data", "orig", "target", "rdev", "xattr")

    def __init__(self, kind, mode, uid=0, gid=0, mtime=0):
        self.kind, self.mode, self.uid, self.gid, self.mtime = kind, mode & 0o7777, uid, gid, mtime
        self.ino = None
        self.children = {} if kind == "dir" else None
        self.size = 0
        self.data = None
        self.orig = None        # (blocks_start, [sizes], frag_index or None, frag_offset, sparse)
        self.target = None
        self.rdev = 0
        self.xattr = NO_XATTR

    def set_data(self, data):
        self.data, self.orig, self.size = bytes(data), None, len(data)


# ------------------------------------------------------------------------------ reader
class Image:
    def __init__(self, img):
        self.img = img = bytes(img) if not isinstance(img, bytes) else img
        if len(img) < 96:
            raise SquashfsError("image too small")
        f = struct.unpack_from(SB_FMT, img, 0)
        (self.magic, self.inode_count, self.mtime, self.block_size, self.frag_count,
         self.comp, self.block_log, self.flags, self.id_count, vmaj, vmin, self.root_ref,
         self.bytes_used, self.id_table, self.xattr_table, self.inode_table,
         self.dir_table, self.frag_table, self.export_table) = f
        if self.magic != MAGIC:
            raise SquashfsError("not a squashfs image")
        if (vmaj, vmin) != (4, 0):
            raise SquashfsError(f"unsupported squashfs version {vmaj}.{vmin}")
        if self.comp != COMP_LZO:
            raise SquashfsError(f"unsupported compressor id {self.comp} (only LZO)")
        if self.flags & FLAG_COMP_OPT:
            raise SquashfsError("compressor options are not supported")
        if self.block_size != 1 << self.block_log:
            raise SquashfsError("inconsistent block size")
        if self.xattr_table != INVALID_BLK:
            raise SquashfsError("xattrs are not supported")
        if self.bytes_used > len(img):
            raise SquashfsError("truncated image")

        self.ids = list(struct.unpack(f"<{self.id_count}I",
                                      self._indexed_table(self.id_table, self.id_count, 4)))
        frag_raw = self._indexed_table(self.frag_table, self.frag_count, 16) if self.frag_count else b""
        self.frags = [struct.unpack_from("<QII", frag_raw, i * 16)[:2] for i in range(self.frag_count)]
        # directory table ends where the first of the following tables begins
        ends = [self.bytes_used]
        for start, count, esz in ((self.frag_table, self.frag_count, 16),
                                  (self.export_table, self.inode_count, 8),
                                  (self.id_table, self.id_count, 4)):
            if start != INVALID_BLK and count:
                ends.append(struct.unpack_from("<Q", img, start)[0])
        self.inodes_raw, self.inode_map = self._meta_range(self.inode_table, self.dir_table)
        self.dirs_raw, self.dir_map = self._meta_range(self.dir_table, min(ends))
        self._frag_cache = {}

    # metadata blocks ------------------------------------------------------------
    def _meta_block(self, pos):
        hdr = struct.unpack_from("<H", self.img, pos)[0]
        size = hdr & 0x7FFF
        raw = self.img[pos + 2:pos + 2 + size]
        if len(raw) != size:
            raise SquashfsError("truncated metadata block")
        data = raw if hdr & 0x8000 else lzo1x.decompress(raw, META_SIZE)
        return data, pos + 2 + size

    def _meta_range(self, start, end):
        out, mp, pos = bytearray(), {}, start
        while pos < end:
            mp[pos - start] = len(out)
            data, pos = self._meta_block(pos)
            out += data
        if pos != end:
            raise SquashfsError("metadata table overruns the next table")
        return bytes(out), mp

    def _indexed_table(self, index_start, count, esz):
        length = count * esz
        nblk = (length + META_SIZE - 1) // META_SIZE
        ptrs = struct.unpack_from(f"<{nblk}Q", self.img, index_start)
        out = bytearray()
        for p in ptrs:
            out += self._meta_block(p)[0]
        if len(out) < length:
            raise SquashfsError("short table")
        return bytes(out[:length])

    # inodes ---------------------------------------------------------------------
    def _inode_pos(self, ref):
        blk, off = ref >> 16, ref & 0xFFFF
        if blk not in self.inode_map:
            raise SquashfsError(f"bad inode reference {ref:#x}")
        return self.inode_map[blk] + off

    def read_inode(self, ref):
        b = self.inodes_raw
        p = self._inode_pos(ref)
        itype, mode, uidx, gidx, mtime, ino = struct.unpack_from("<HHHHII", b, p)
        p += 16
        kind = KIND.get(itype if itype <= 7 else itype - 7)
        if kind is None:
            raise SquashfsError(f"unknown inode type {itype}")
        node = Inode(kind, mode, self.ids[uidx], self.ids[gidx], mtime)
        node.ino = ino
        nlink = 1
        bs = self.block_size
        if itype == T_DIR:
            start, nlink, fsize, off, parent = struct.unpack_from("<IIHHI", b, p)
            node.orig = ("dir", start, off, fsize)
        elif itype == T_LDIR:
            nlink, fsize, start, parent, icount, off, xattr = struct.unpack_from("<IIIIHHI", b, p)
            node.orig = ("dir", start, off, fsize)
            if xattr != NO_XATTR:
                raise SquashfsError("xattrs are not supported")
        elif itype in (T_REG, T_LREG):
            if itype == T_REG:
                bstart, frag, foff, fsize = struct.unpack_from("<IIII", b, p)
                p += 16
                sparse = 0
            else:
                bstart, fsize, sparse, nlink, frag, foff, xattr = struct.unpack_from("<QQQIIII", b, p)
                p += 40
                if xattr != NO_XATTR:
                    raise SquashfsError("xattrs are not supported")
            nblocks = fsize // bs if frag != NO_FRAG else (fsize + bs - 1) // bs
            sizes = list(struct.unpack_from(f"<{nblocks}I", b, p))
            node.size = fsize
            node.orig = (bstart, sizes, None if frag == NO_FRAG else frag, foff, sparse)
        elif itype in (T_SYM, T_LSYM):
            nlink, tsize = struct.unpack_from("<II", b, p)
            node.target = b[p + 8:p + 8 + tsize]
            if itype == T_LSYM and struct.unpack_from("<I", b, p + 8 + tsize)[0] != NO_XATTR:
                raise SquashfsError("xattrs are not supported")
        elif itype in (T_BLK, T_CHR, T_LBLK, T_LCHR):
            nlink, node.rdev = struct.unpack_from("<II", b, p)
        else:
            nlink, = struct.unpack_from("<I", b, p)
        return node, nlink

    def read_dir_entries(self, node):
        _, start, off, fsize = node.orig
        if start not in self.dir_map:
            raise SquashfsError("bad directory reference")
        p = self.dir_map[start] + off
        end = p + fsize - 3
        b = self.dirs_raw
        out = []
        while p < end:
            count, iblk, base = struct.unpack_from("<III", b, p)
            p += 12
            for _ in range(count + 1):
                ioff, delta, etype, nsize = struct.unpack_from("<HhHH", b, p)
                p += 8
                name = b[p:p + nsize + 1]
                p += nsize + 1
                out.append((name, (iblk << 16) | ioff, base + delta, etype))
        if p != end:
            raise SquashfsError("directory listing size mismatch")
        return out

    def read_tree(self):
        """Return the root Inode with the full tree. Hard links share Inode objects."""
        by_ino = {}
        nlinks = {}

        def load(ref):
            node, nlink = self.read_inode(ref)
            if node.ino in by_ino:
                return by_ino[node.ino]
            by_ino[node.ino] = node
            nlinks[node.ino] = nlink
            if node.kind == "dir":
                for name, cref, cino, etype in self.read_dir_entries(node):
                    if name in (b".", b"..") or b"/" in name or not name:
                        raise SquashfsError(f"invalid name {name!r}")
                    child = by_ino.get(cino)
                    if child is None:
                        child = load(cref)
                    elif child.kind == "dir":
                        raise SquashfsError("directory linked twice")
                    if child.ino != cino or BASIC[child.kind] != etype:
                        raise SquashfsError("directory entry does not match its inode")
                    node.children[name] = child
                node.orig = None
            return node

        root = load(self.root_ref)
        if len(by_ino) != self.inode_count:
            raise SquashfsError(f"found {len(by_ino)} inodes, superblock says {self.inode_count}")
        # hard link counts must match the number of entries found
        refs = {}
        for _path, node in walk(root):
            refs[id(node)] = refs.get(id(node), 0) + 1
        for node in by_ino.values():
            if node.kind == "dir":
                want = 2 + sum(1 for c in node.children.values() if c.kind == "dir")
            else:
                want = refs[id(node)]
            if nlinks[node.ino] != want:
                raise SquashfsError(f"link count mismatch on inode {node.ino}")
        return root

    # file data -------------------------------------------------------------------
    def block_ranges(self, node):
        """(start, length) of the compressed data run of a file kept from this image."""
        bstart, sizes, _frag, _off, _sp = node.orig
        length = sum(s & ~UNCOMP_BLOCK for s in sizes)
        return bstart, length

    def fragment_block(self, idx):
        if idx not in self._frag_cache:
            start, size = self.frags[idx]
            raw = self.img[start:start + (size & ~UNCOMP_BLOCK)]
            self._frag_cache[idx] = raw if size & UNCOMP_BLOCK else lzo1x.decompress(raw, self.block_size)
        return self._frag_cache[idx]

    def read_file(self, node):
        if node.data is not None:
            return node.data
        bstart, sizes, frag, foff, _sp = node.orig
        bs = self.block_size
        out = bytearray()
        pos = bstart
        for s in sizes:
            want = min(bs, node.size - len(out))
            if s == 0:
                out += bytes(want)
                continue
            n = s & ~UNCOMP_BLOCK
            raw = self.img[pos:pos + n]
            pos += n
            blk = raw if s & UNCOMP_BLOCK else lzo1x.decompress(raw, bs)
            if len(blk) != want:
                raise SquashfsError("data block has the wrong size")
            out += blk
        if frag is not None:
            fb = self.fragment_block(frag)
            rest = node.size - len(out)
            if foff + rest > len(fb):
                raise SquashfsError("fragment overrun")
            out += fb[foff:foff + rest]
        if len(out) != node.size:
            raise SquashfsError("file size mismatch")
        return bytes(out)


def walk(root, prefix=b""):
    """Yield (path, node) for every entry, depth first, names sorted. Root path is b"/"."""
    yield (prefix or b"/"), root
    stack = [(prefix, root)]
    while stack:
        pfx, d = stack.pop()
        for name in sorted(d.children):
            c = d.children[name]
            p = pfx + b"/" + name
            yield p, c
            if c.kind == "dir":
                stack.append((p, c))


# ------------------------------------------------------------------------------ writer
class _Meta:
    """Metadata stream split in 8 KiB blocks, each LZO compressed (or stored)."""

    def __init__(self):
        self.out = bytearray()
        self.buf = bytearray()
        self.block_starts = []

    def pos(self):
        return len(self.out), len(self.buf)

    def write(self, b):
        self.buf += b
        while len(self.buf) >= META_SIZE:
            self._flush(bytes(self.buf[:META_SIZE]))
            del self.buf[:META_SIZE]

    def finish(self):
        if self.buf:
            self._flush(bytes(self.buf))
            self.buf = bytearray()
        return bytes(self.out)

    def _flush(self, chunk):
        self.block_starts.append(len(self.out))
        c = lzo1x.compress(chunk)
        if len(c) < len(chunk):
            self.out += struct.pack("<H", len(c)) + c
        else:
            self.out += struct.pack("<H", len(chunk) | 0x8000) + chunk


def _compress_block(data):
    c = lzo1x.compress(data)
    if len(c) < len(data):
        return c, len(c)
    return data, len(data) | UNCOMP_BLOCK


def write_image(root, src, mtime=None, log=None):
    """Write a new image for the tree `root`. `src` is the source Image whose blocks are
    referenced by unchanged files. Returns the image bytes (not padded)."""
    bs = src.block_size
    mtime = src.mtime if mtime is None else mtime
    entries = list(walk(root))

    # unique inodes in write order (post-order: children before their directory)
    order, seen = [], set()

    def visit(d):
        for name in sorted(d.children):
            c = d.children[name]
            if c.kind == "dir":
                visit(c)
            elif id(c) not in seen:
                seen.add(id(c))
                order.append(c)
        order.append(d)
    visit(root)
    number = {id(n): i + 1 for i, n in enumerate(order)}
    nrefs = {}
    for _p, n in entries:
        nrefs[id(n)] = nrefs.get(id(n), 0) + 1
    for n in order:
        if n.kind == "dir" and nrefs[id(n)] != 1:
            raise SquashfsError("a directory is linked twice")

    # ---------------------------------------------------------------- data area
    out = bytearray(96)
    # 1) original blocks still referenced, copied verbatim (relocated)
    ranges = []
    used_frags = set()
    for n in order:
        if n.kind == "file" and n.data is None:
            bstart, sizes, frag, _foff, _sp = n.orig
            start, length = src.block_ranges(n)
            if length:
                ranges.append((start, start + length))
            if frag is not None:
                used_frags.add(frag)
    for fi in used_frags:
        start, size = src.frags[fi]
        ranges.append((start, start + (size & ~UNCOMP_BLOCK)))
    ranges.sort()
    merged = []
    for a, b in ranges:
        if a < 96 or b > src.inode_table:
            raise SquashfsError("data block outside the data area")
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    reloc_starts, reloc_new = [], []
    for a, b in merged:
        reloc_starts.append(a)
        reloc_new.append(len(out))
        out += src.img[a:b]
    kept_bytes = len(out) - 96

    def reloc(old):
        i = bisect.bisect_right(reloc_starts, old) - 1
        if i < 0 or old >= merged[i][1]:
            raise SquashfsError("unrelocatable block")
        return reloc_new[i] + (old - merged[i][0])

    frag_table = []                     # (start, size_field)
    frag_map = {}
    for fi in sorted(used_frags):
        start, size = src.frags[fi]
        frag_map[fi] = len(frag_table)
        frag_table.append((reloc(start), size))

    # 2) new data (deduplicated by content)
    layout = {}                         # id(node) -> (blocks_start, sizes, frag_idx, frag_off)
    by_hash = {}
    frag_buf = bytearray()
    pending = []                        # nodes whose fragment is in frag_buf: (node_key, off)
    new_bytes = 0

    def flush_frag():
        nonlocal frag_buf
        if not frag_buf:
            return
        c, field = _compress_block(bytes(frag_buf))
        frag_table.append((len(out), field))
        out.extend(c)
        frag_buf = bytearray()

    for n in order:
        if n.kind != "file":
            continue
        if n.data is None:
            bstart, sizes, frag, foff, _sp = n.orig
            start, length = src.block_ranges(n)
            layout[id(n)] = (reloc(bstart) if length else len(out), sizes,
                             None if frag is None else frag_map[frag], foff, n.orig[4])
            continue
        key = (len(n.data), hashlib.sha256(n.data).digest())
        if key in by_hash:
            layout[id(n)] = by_hash[key]
            continue
        data = n.data
        new_bytes += len(data)
        if 0 < len(data) < bs:
            if len(frag_buf) + len(data) > bs:
                flush_frag()
            lay = (len(out), [], len(frag_table), len(frag_buf), 0)
            frag_buf += data
        else:
            start, sizes = len(out), []
            for i in range(0, len(data), bs):
                c, field = _compress_block(data[i:i + bs])
                out.extend(c)
                sizes.append(field)
            lay = (start, sizes, None, 0, 0)
        layout[id(n)] = lay
        by_hash[key] = lay
    flush_frag()
    data_end = len(out)

    # ---------------------------------------------------------------- ids
    ids = []
    for n in order:
        for v in (n.uid, n.gid):
            if v not in ids:
                ids.append(v)
    if len(ids) > 65535:
        raise SquashfsError("too many ids")
    idx = {v: i for i, v in enumerate(ids)}

    # ---------------------------------------------------------------- inode + dir tables
    itab, dtab = _Meta(), _Meta()
    refs = {}
    total = len(order)

    def hdr(itype, n):
        return struct.pack("<HHHHII", itype, n.mode & 0o7777, idx[n.uid], idx[n.gid],
                           n.mtime if n.mtime is not None else mtime, number[id(n)])

    def write_inode(n, parent_no=None, listing=None):
        blk, off = itab.pos()
        nlink = nrefs[id(n)]
        if n.kind == "dir":
            dblk, doff, dsize = listing
            nlink = 2 + sum(1 for c in n.children.values() if c.kind == "dir")
            if dsize + 3 <= 0xFFFF:
                rec = hdr(T_DIR, n) + struct.pack("<IIHHI", dblk, nlink, dsize + 3, doff, parent_no)
            else:
                rec = hdr(T_LDIR, n) + struct.pack("<IIIIHHI", nlink, dsize + 3, dblk, parent_no,
                                                   0, doff, NO_XATTR)
        elif n.kind == "file":
            start, sizes, frag, foff, sparse = layout[id(n)]
            fragf = NO_FRAG if frag is None else frag
            if frag is not None and not (0 < n.size and len(sizes) == n.size // bs):
                raise SquashfsError("inconsistent fragment layout")
            if frag is None and len(sizes) != (n.size + bs - 1) // bs:
                raise SquashfsError("inconsistent block layout")
            blist = struct.pack(f"<{len(sizes)}I", *sizes)
            if nlink == 1 and start < 1 << 32 and n.size < 1 << 32:
                rec = hdr(T_REG, n) + struct.pack("<IIII", start, fragf, foff, n.size) + blist
            else:
                rec = hdr(T_LREG, n) + struct.pack("<QQQIIII", start, n.size, sparse, nlink,
                                                   fragf, foff, NO_XATTR) + blist
        elif n.kind == "symlink":
            rec = hdr(T_SYM, n) + struct.pack("<II", nlink, len(n.target)) + n.target
        elif n.kind in ("blkdev", "chrdev"):
            rec = hdr(BASIC[n.kind], n) + struct.pack("<II", nlink, n.rdev)
        else:
            rec = hdr(BASIC[n.kind], n) + struct.pack("<I", nlink)
        itab.write(rec)
        ref = (blk << 16) | off
        refs[id(n)] = ref
        return ref

    def write_dir(d, parent_no):
        my_no = number[id(d)]
        items = []
        for name in sorted(d.children):
            c = d.children[name]
            if not name or b"/" in name or name in (b".", b"..") or len(name) > 256:
                raise SquashfsError(f"invalid name {name!r}")
            if c.kind == "dir":
                write_dir(c, my_no)
            elif id(c) not in refs:
                write_inode(c)
            items.append((name, c))
        # directory listing
        dblk, doff = dtab.pos()
        lst = bytearray()
        i = 0
        while i < len(items):
            ref0 = refs[id(items[i][1])]
            base = number[id(items[i][1])]
            group = []
            while (i < len(items) and len(group) < 256
                   and refs[id(items[i][1])] >> 16 == ref0 >> 16
                   and -32768 <= number[id(items[i][1])] - base <= 32767):
                group.append(items[i])
                i += 1
            lst += struct.pack("<III", len(group) - 1, ref0 >> 16, base)
            for name, c in group:
                r = refs[id(c)]
                lst += struct.pack("<HhHH", r & 0xFFFF, number[id(c)] - base, BASIC[c.kind],
                                   len(name) - 1) + name
        dtab.write(lst)
        return write_inode(d, parent_no, (dblk, doff, len(lst)))

    root_ref = write_dir(root, total + 1)
    if len(refs) != total:
        raise SquashfsError("not every inode was written")

    # ---------------------------------------------------------------- tables
    inode_table = len(out)
    out += itab.finish()
    dir_table = len(out)
    out += dtab.finish()

    def indexed(raw):
        m = _Meta()
        base = len(out)
        m.write(raw)
        blob = m.finish()
        out.extend(blob)
        start = len(out)
        out.extend(struct.pack(f"<{len(m.block_starts)}Q", *[base + s for s in m.block_starts]))
        return start

    frag_start = indexed(b"".join(struct.pack("<QII", s, f, 0) for s, f in frag_table))
    lookup = [0] * total
    for n in order:
        lookup[number[id(n)] - 1] = refs[id(n)]
    export_start = indexed(struct.pack(f"<{total}Q", *lookup))
    id_start = indexed(struct.pack(f"<{len(ids)}I", *ids))
    bytes_used = len(out)

    flags = FLAG_DUPLICATES | FLAG_EXPORTABLE | FLAG_NO_XATTRS
    struct.pack_into(SB_FMT, out, 0, MAGIC, total, mtime, bs, len(frag_table), COMP_LZO,
                     src.block_log, flags, len(ids), 4, 0, root_ref, bytes_used, id_start,
                     INVALID_BLK, inode_table, dir_table, frag_start, export_start)
    stats = {"kept_data": kept_bytes, "new_data_in": new_bytes,
             "new_data_out": data_end - 96 - kept_bytes, "metadata": bytes_used - data_end,
             "inodes": total, "fragments": len(frag_table), "bytes_used": bytes_used}
    return bytes(out), stats
