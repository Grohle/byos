package org.hibyos.install.core

import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest

/**
 * Minimal SquashFS 4.0 (LZO) reader and writer: a port of tools/squashfs.py of the HiByOS
 * repository that streams the image from/to storage instead of holding it in memory.
 *
 * The writer does not recompress existing data: the compressed data and fragment blocks of
 * files kept from the source image are copied byte for byte (only relocated). New or changed
 * file data is LZO1X compressed, or stored when compression does not help.
 *
 * Output layout (as mksquashfs): superblock, data + fragment blocks, inode table, directory
 * table, fragment table, export table, id table. No xattrs.
 *
 * File and directory names are kept as ISO-8859-1 strings holding the raw name bytes, so the
 * natural String order equals the byte order Python uses for `bytes`.
 */
class SquashfsException(message: String) : Exception(message)

enum class Kind(val basic: Int, val label: String) {
    DIR(1, "dir"), FILE(2, "file"), SYMLINK(3, "symlink"), BLKDEV(4, "blkdev"),
    CHRDEV(5, "chrdev"), FIFO(6, "fifo"), SOCKET(7, "socket");

    companion object {
        fun ofBasic(t: Int): Kind? = entries.firstOrNull { it.basic == t }
    }
}

/** Where the data of a file kept from the source image lives. */
class FileRef(val start: Long, val sizes: IntArray, val frag: Int, val fragOffset: Int, val sparse: Long)

/**
 * One inode. Hard links are several directory entries pointing to the same object. For files
 * exactly one of [data] (new content) or [orig] (blocks of the source image) is set.
 */
class Inode(val kind: Kind, mode: Int, var uid: Long = 0, var gid: Long = 0, var mtime: Long? = 0) {
    var mode: Int = mode and 0xFFF
    var ino: Long = 0
    val children: HashMap<String, Inode>? = if (kind == Kind.DIR) HashMap() else null
    var size: Long = 0
    var data: ByteArray? = null
        private set
    var orig: FileRef? = null
    var target: ByteArray? = null
    var rdev: Long = 0

    // reader/writer scratch state
    internal var dirStart = 0L
    internal var dirOffset = 0
    internal var dirSize = 0L
    internal var number = 0
    internal var nrefs = 0
    internal var ref = -1L
    internal var layout: FileRef? = null
    internal var nlinkRead = 0L
    internal var seenRefs = 0

    fun setData(d: ByteArray) {
        data = d
        orig = null
        size = d.size.toLong()
    }
}

internal const val SQ_MAGIC = 0x73717368L
internal const val META_SIZE = 8192
internal const val INVALID_BLK = -1L
internal const val NO_FRAG = 0xFFFFFFFFL
internal const val NO_XATTR = 0xFFFFFFFFL
internal const val UNCOMP_BLOCK = 1 shl 24
internal const val COMP_LZO = 3
internal const val FLAG_COMP_OPT = 0x0400
internal const val FLAG_DUPLICATES = 0x0040
internal const val FLAG_EXPORTABLE = 0x0080
internal const val FLAG_NO_XATTRS = 0x0200
private const val T_DIR = 1
private const val T_REG = 2
private const val T_SYM = 3
private const val T_BLK = 4
private const val T_CHR = 5
private const val T_LDIR = 8
private const val T_LREG = 9
private const val T_LSYM = 10
private const val T_LBLK = 11
private const val T_LCHR = 12

fun latin1(b: ByteArray, off: Int = 0, len: Int = b.size - off) = String(b, off, len, Charsets.ISO_8859_1)

/** Name key (ISO-8859-1 string of the UTF-8 bytes) of a normal text path component. */
fun nameKey(s: String) = String(s.toByteArray(Charsets.UTF_8), Charsets.ISO_8859_1)

/** Human readable form of a name key or path made of name keys. */
fun displayName(key: String) = String(key.toByteArray(Charsets.ISO_8859_1), Charsets.UTF_8)

// -------------------------------------------------------------------------------- reader
class SquashImage(private val img: ByteSource) {
    val inodeCount: Long
    val mtime: Long
    val blockSize: Int
    val fragCount: Int
    val blockLog: Int
    val flags: Int
    val idCount: Int
    val rootRef: Long
    val bytesUsed: Long
    val idTable: Long
    val xattrTable: Long
    val inodeTable: Long
    val dirTable: Long
    val fragTable: Long
    val exportTable: Long
    val ids: LongArray
    internal val fragStart: LongArray
    internal val fragSize: IntArray

    private val inodesRaw: ByteArray
    private val inodeMap: HashMap<Long, Int>
    private val dirsRaw: ByteArray
    private val dirMap: HashMap<Long, Int>
    private val fragCache = object : LinkedHashMap<Int, ByteArray>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Int, ByteArray>?) = size > 12
    }
    private val rawBuf: ByteArray
    private val blockBuf: ByteArray

    init {
        if (img.size < 96) throw SquashfsException("image too small")
        val sb = img.bytes(0, 96)
        val magic = sb.u32(0)
        inodeCount = sb.u32(4)
        mtime = sb.u32(8)
        val bs = sb.u32(12)
        fragCount = sb.u32(16).toInt()
        val comp = sb.u16(20)
        blockLog = sb.u16(22)
        flags = sb.u16(24)
        idCount = sb.u16(26)
        val vmaj = sb.u16(28)
        val vmin = sb.u16(30)
        rootRef = sb.u64(32)
        bytesUsed = sb.u64(40)
        idTable = sb.u64(48)
        xattrTable = sb.u64(56)
        inodeTable = sb.u64(64)
        dirTable = sb.u64(72)
        fragTable = sb.u64(80)
        exportTable = sb.u64(88)
        if (magic != SQ_MAGIC) throw SquashfsException("not a squashfs image")
        if (vmaj != 4 || vmin != 0) throw SquashfsException("unsupported squashfs version $vmaj.$vmin")
        if (comp != COMP_LZO) throw SquashfsException("unsupported compressor id $comp (only LZO)")
        if (flags and FLAG_COMP_OPT != 0) throw SquashfsException("compressor options are not supported")
        if (blockLog !in 12..20 || bs != (1L shl blockLog)) throw SquashfsException("inconsistent block size")
        blockSize = bs.toInt()
        if (xattrTable != INVALID_BLK) throw SquashfsException("xattrs are not supported")
        if (bytesUsed < 0 || bytesUsed > img.size) throw SquashfsException("truncated image")
        if (fragCount < 0 || inodeCount > 10_000_000L) throw SquashfsException("implausible table sizes")
        rawBuf = ByteArray(blockSize)
        blockBuf = ByteArray(blockSize)

        val idRaw = indexedTable(idTable, idCount.toLong(), 4)
        ids = LongArray(idCount) { idRaw.u32(it * 4) }
        val fragRaw = if (fragCount != 0) indexedTable(fragTable, fragCount.toLong(), 16) else ByteArray(0)
        fragStart = LongArray(fragCount) { fragRaw.u64(it * 16) }
        fragSize = IntArray(fragCount) { fragRaw.i32(it * 16 + 8) }
        // the directory table ends where the first of the following tables begins
        var end = bytesUsed
        for ((start, count) in listOf(fragTable to fragCount.toLong(), exportTable to inodeCount, idTable to idCount.toLong())) {
            if (start != INVALID_BLK && count != 0L) end = minOf(end, img.bytes(start, 8).u64(0))
        }
        val (ir, im) = metaRange(inodeTable, dirTable)
        inodesRaw = ir
        inodeMap = im
        val (dr, dm) = metaRange(dirTable, end)
        dirsRaw = dr
        dirMap = dm
    }

    // metadata blocks --------------------------------------------------------------
    /** Appends the decoded metadata block at [pos] to [out] and returns the position after it. */
    private fun metaBlock(pos: Long, out: ByteBuf): Long {
        if (pos < 0 || pos + 2 > img.size) throw SquashfsException("truncated metadata block")
        val hdr = img.bytes(pos, 2).u16(0)
        val size = hdr and 0x7FFF
        if (pos + 2 + size > img.size) throw SquashfsException("truncated metadata block")
        val raw = img.bytes(pos + 2, size)
        if (hdr and 0x8000 != 0) {
            out.bytes(raw)
        } else {
            val tmp = ByteArray(META_SIZE)
            val n = try {
                Lzo1x.decompress(raw, 0, size, tmp, META_SIZE)
            } catch (e: LzoException) {
                throw SquashfsException("corrupt metadata block: ${e.message}")
            }
            out.bytes(tmp, 0, n)
        }
        return pos + 2 + size
    }

    private fun metaRange(start: Long, end: Long): Pair<ByteArray, HashMap<Long, Int>> {
        val out = ByteBuf(1 shl 16)
        val mp = HashMap<Long, Int>()
        var pos = start
        while (pos < end) {
            mp[pos - start] = out.size
            pos = metaBlock(pos, out)
        }
        if (pos != end) throw SquashfsException("metadata table overruns the next table")
        return out.toByteArray() to mp
    }

    private fun indexedTable(indexStart: Long, count: Long, esz: Int): ByteArray {
        val length = count * esz
        val nblk = ((length + META_SIZE - 1) / META_SIZE).toInt()
        if (indexStart < 0 || indexStart + nblk * 8L > img.size) throw SquashfsException("short table")
        val ptrs = img.bytes(indexStart, nblk * 8)
        val out = ByteBuf(length.toInt() + META_SIZE)
        for (i in 0 until nblk) metaBlock(ptrs.u64(i * 8), out)
        if (out.size < length) throw SquashfsException("short table")
        return out.buf.copyOf(length.toInt())
    }

    // inodes -----------------------------------------------------------------------
    private fun inodePos(ref: Long): Int {
        val blk = ref ushr 16
        val off = (ref and 0xFFFF).toInt()
        val base = inodeMap[blk] ?: throw SquashfsException("bad inode reference 0x${ref.toString(16)}")
        return base + off
    }

    private fun id(idx: Int): Long {
        if (idx >= ids.size) throw SquashfsException("bad id index")
        return ids[idx]
    }

    internal fun readInode(ref: Long): Inode {
        val b = inodesRaw
        var p = inodePos(ref)
        try {
            val itype = b.u16(p)
            val mode = b.u16(p + 2)
            val uidx = b.u16(p + 4)
            val gidx = b.u16(p + 6)
            val mt = b.u32(p + 8)
            val ino = b.u32(p + 12)
            p += 16
            val kind = Kind.ofBasic(if (itype <= 7) itype else itype - 7)
                ?: throw SquashfsException("unknown inode type $itype")
            val node = Inode(kind, mode, id(uidx), id(gidx), mt)
            node.ino = ino
            var nlink = 1L
            val bs = blockSize.toLong()
            when (itype) {
                T_DIR -> {
                    node.dirStart = b.u32(p)
                    nlink = b.u32(p + 4)
                    node.dirSize = b.u16(p + 8).toLong()
                    node.dirOffset = b.u16(p + 10)
                }
                T_LDIR -> {
                    nlink = b.u32(p)
                    node.dirSize = b.u32(p + 4)
                    node.dirStart = b.u32(p + 8)
                    node.dirOffset = b.u16(p + 18)
                    if (b.u32(p + 20) != NO_XATTR) throw SquashfsException("xattrs are not supported")
                }
                T_REG, T_LREG -> {
                    val bstart: Long
                    val frag: Long
                    val foff: Long
                    val fsize: Long
                    var sparse = 0L
                    if (itype == T_REG) {
                        bstart = b.u32(p)
                        frag = b.u32(p + 4)
                        foff = b.u32(p + 8)
                        fsize = b.u32(p + 12)
                        p += 16
                    } else {
                        bstart = b.u64(p)
                        fsize = b.u64(p + 8)
                        sparse = b.u64(p + 16)
                        nlink = b.u32(p + 24)
                        frag = b.u32(p + 28)
                        foff = b.u32(p + 32)
                        if (b.u32(p + 36) != NO_XATTR) throw SquashfsException("xattrs are not supported")
                        p += 40
                    }
                    if (fsize < 0 || fsize > (1L shl 40)) throw SquashfsException("implausible file size")
                    val nblocks = (if (frag != NO_FRAG) fsize / bs else (fsize + bs - 1) / bs).toInt()
                    if (p + nblocks * 4L > b.size) throw SquashfsException("inode table overrun")
                    val sizes = IntArray(nblocks) { b.i32(p + it * 4) }
                    node.size = fsize
                    node.orig = FileRef(bstart, sizes, if (frag == NO_FRAG) -1 else frag.toInt(), foff.toInt(), sparse)
                }
                T_SYM, T_LSYM -> {
                    nlink = b.u32(p)
                    val tsize = b.u32(p + 4).toInt()
                    if (tsize < 0 || p + 8 + tsize > b.size) throw SquashfsException("inode table overrun")
                    node.target = b.copyOfRange(p + 8, p + 8 + tsize)
                    if (itype == T_LSYM && b.u32(p + 8 + tsize) != NO_XATTR) throw SquashfsException("xattrs are not supported")
                }
                T_BLK, T_CHR, T_LBLK, T_LCHR -> {
                    nlink = b.u32(p)
                    node.rdev = b.u32(p + 4)
                }
                else -> nlink = b.u32(p)
            }
            node.nlinkRead = nlink
            return node
        } catch (e: IndexOutOfBoundsException) {
            throw SquashfsException("inode table overrun")
        }
    }

    private class DirEntry(val name: String, val ref: Long, val ino: Long, val type: Int)

    private fun readDirEntries(node: Inode): List<DirEntry> {
        val base = dirMap[node.dirStart] ?: throw SquashfsException("bad directory reference")
        var p = base + node.dirOffset
        val end = p + node.dirSize.toInt() - 3
        val b = dirsRaw
        val out = ArrayList<DirEntry>()
        try {
            while (p < end) {
                val count = b.u32(p)
                val iblk = b.u32(p + 4)
                val ibase = b.u32(p + 8)
                p += 12
                if (count > 255) throw SquashfsException("bad directory header")
                for (i in 0..count.toInt()) {
                    val ioff = b.u16(p)
                    val delta = b.s16(p + 2)
                    val etype = b.u16(p + 4)
                    val nsize = b.u16(p + 6)
                    p += 8
                    if (p + nsize + 1 > b.size) throw SquashfsException("directory table overrun")
                    out.add(DirEntry(latin1(b, p, nsize + 1), (iblk shl 16) or ioff.toLong(), (ibase + delta) and 0xFFFFFFFFL, etype))
                    p += nsize + 1
                }
            }
        } catch (e: IndexOutOfBoundsException) {
            throw SquashfsException("directory table overrun")
        }
        if (p != end) throw SquashfsException("directory listing size mismatch")
        return out
    }

    /** Returns the root inode with the full tree. Hard links share Inode objects. */
    fun readTree(): Inode {
        val byIno = HashMap<Long, Inode>()

        fun load(ref: Long, depth: Int): Inode {
            if (depth > 200) throw SquashfsException("directory tree too deep")
            val node = readInode(ref)
            byIno[node.ino]?.let { return it }
            byIno[node.ino] = node
            if (node.kind == Kind.DIR) {
                for (e in readDirEntries(node)) {
                    if (e.name == "." || e.name == ".." || e.name.contains('/') || e.name.isEmpty()) {
                        throw SquashfsException("invalid name ${e.name}")
                    }
                    var child = byIno[e.ino]
                    if (child == null) {
                        child = load(e.ref, depth + 1)
                    } else if (child.kind == Kind.DIR) {
                        throw SquashfsException("directory linked twice")
                    }
                    if (child.ino != e.ino || child.kind.basic != e.type) {
                        throw SquashfsException("directory entry does not match its inode")
                    }
                    node.children!![e.name] = child
                }
            }
            return node
        }

        val root = load(rootRef, 0)
        if (byIno.size.toLong() != inodeCount) {
            throw SquashfsException("found ${byIno.size} inodes, superblock says $inodeCount")
        }
        // hard link counts must match the number of entries found
        for (n in byIno.values) n.seenRefs = 0
        for ((_, node) in walk(root)) node.seenRefs++
        for (node in byIno.values) {
            val want = if (node.kind == Kind.DIR) {
                2L + node.children!!.values.count { it.kind == Kind.DIR }
            } else {
                node.seenRefs.toLong()
            }
            if (node.nlinkRead != want) throw SquashfsException("link count mismatch on inode ${node.ino}")
        }
        return root
    }

    // file data --------------------------------------------------------------------
    /** Length of the compressed data run of a file kept from this image. */
    internal fun blockRunLength(ref: FileRef): Long {
        var length = 0L
        for (s in ref.sizes) length += (s and UNCOMP_BLOCK.inv()).toLong() and 0xFFFFFFFFL
        return length
    }

    internal fun copyRaw(pos: Long, len: Long, sink: Sink, buf: ByteArray) {
        var p = pos
        var left = len
        while (left > 0) {
            val n = minOf(left, buf.size.toLong()).toInt()
            img.read(p, buf, 0, n)
            sink.write(buf, 0, n)
            p += n
            left -= n
        }
    }

    private fun fragmentBlock(idx: Int): ByteArray {
        fragCache[idx]?.let { return it }
        if (idx < 0 || idx >= fragCount) throw SquashfsException("bad fragment index")
        val start = fragStart[idx]
        val field = fragSize[idx]
        val n = field and UNCOMP_BLOCK.inv()
        if (n < 0 || n > blockSize || start < 0 || start + n > img.size) throw SquashfsException("bad fragment block")
        val raw = img.bytes(start, n)
        val blk = if (field and UNCOMP_BLOCK != 0) raw else try {
            Lzo1x.decompress(raw, blockSize)
        } catch (e: LzoException) {
            throw SquashfsException("corrupt fragment block: ${e.message}")
        }
        fragCache[idx] = blk
        return blk
    }

    /** Feeds the content of [node] to [sink] block by block. */
    fun streamFile(node: Inode, sink: (ByteArray, Int, Int) -> Unit) {
        node.data?.let { sink(it, 0, it.size); return }
        val ref = node.orig ?: throw SquashfsException("not a file of this image")
        val bs = blockSize
        var done = 0L
        var pos = ref.start
        for (s in ref.sizes) {
            val want = minOf(bs.toLong(), node.size - done).toInt()
            if (s == 0) {
                java.util.Arrays.fill(blockBuf, 0, want, 0)
                sink(blockBuf, 0, want)
                done += want
                continue
            }
            val n = s and UNCOMP_BLOCK.inv()
            if (n < 0 || n > bs || pos < 0 || pos + n > img.size) throw SquashfsException("bad data block")
            img.read(pos, rawBuf, 0, n)
            pos += n
            if (s and UNCOMP_BLOCK != 0) {
                if (n != want) throw SquashfsException("data block has the wrong size")
                sink(rawBuf, 0, n)
            } else {
                val got = try {
                    Lzo1x.decompress(rawBuf, 0, n, blockBuf, bs)
                } catch (e: LzoException) {
                    throw SquashfsException("corrupt data block: ${e.message}")
                }
                if (got != want) throw SquashfsException("data block has the wrong size")
                sink(blockBuf, 0, got)
            }
            done += want
        }
        if (ref.frag >= 0) {
            val fb = fragmentBlock(ref.frag)
            val rest = node.size - done
            if (rest < 0 || ref.fragOffset + rest > fb.size) throw SquashfsException("fragment overrun")
            sink(fb, ref.fragOffset, rest.toInt())
            done += rest
        }
        if (done != node.size) throw SquashfsException("file size mismatch")
    }

    fun readFile(node: Inode): ByteArray {
        node.data?.let { return it }
        if (node.size > Int.MAX_VALUE / 2) throw SquashfsException("file too big to load")
        val out = ByteArray(node.size.toInt())
        var p = 0
        streamFile(node) { b, off, len ->
            if (p + len > out.size) throw SquashfsException("file size mismatch")
            System.arraycopy(b, off, out, p, len)
            p += len
        }
        return out
    }

    fun digestFile(node: Inode, md: MessageDigest): ByteArray {
        md.reset()
        streamFile(node) { b, off, len -> md.update(b, off, len) }
        return md.digest()
    }

    companion object {
        /** Every entry as (path, node), names sorted, in the same order as squashfs.walk(). */
        fun walk(root: Inode): List<Pair<String, Inode>> {
            val out = ArrayList<Pair<String, Inode>>()
            out.add("/" to root)
            val stack = ArrayDeque<Pair<String, Inode>>()
            stack.addLast("" to root)
            while (stack.isNotEmpty()) {
                val (pfx, d) = stack.removeLast()
                val ch = d.children!!
                for (name in ch.keys.sorted()) {
                    val c = ch[name]!!
                    val p = "$pfx/$name"
                    out.add(p to c)
                    if (c.kind == Kind.DIR) stack.addLast(p to c)
                }
            }
            return out
        }
    }
}

// -------------------------------------------------------------------------------- output
/** Sequential output with a known position, plus one rewrite of the start at the end. */
interface Sink : Closeable {
    val position: Long
    fun write(b: ByteArray, off: Int = 0, len: Int = b.size - off)
    fun writeAt(pos: Long, b: ByteArray)
}

class FileSink(file: File) : Sink {
    private val raf = RandomAccessFile(file, "rw").also { it.setLength(0) }
    private val buf = ByteArray(1 shl 16)
    private var fill = 0
    private var flushed = 0L
    override val position: Long get() = flushed + fill

    override fun write(b: ByteArray, off: Int, len: Int) {
        var o = off
        var left = len
        while (left > 0) {
            if (fill == buf.size) flush()
            val n = minOf(left, buf.size - fill)
            System.arraycopy(b, o, buf, fill, n)
            fill += n
            o += n
            left -= n
        }
    }

    private fun flush() {
        raf.write(buf, 0, fill)
        flushed += fill
        fill = 0
    }

    override fun writeAt(pos: Long, b: ByteArray) {
        flush()
        raf.seek(pos)
        raf.write(b)
        raf.seek(flushed)
    }

    override fun close() {
        flush()
        raf.fd.sync()
        raf.close()
    }
}

class ArraySink : Sink {
    private val out = ByteBuf(1 shl 16)
    override val position: Long get() = out.size.toLong()
    override fun write(b: ByteArray, off: Int, len: Int) { out.bytes(b, off, len) }
    override fun writeAt(pos: Long, b: ByteArray) { System.arraycopy(b, 0, out.buf, pos.toInt(), b.size) }
    override fun close() {}
    fun toByteArray() = out.toByteArray()
}

// -------------------------------------------------------------------------------- writer
class SquashStats(
    val keptData: Long, val newDataIn: Long, val newDataOut: Long, val metadata: Long,
    val inodes: Int, val fragments: Int, val bytesUsed: Long,
)

/** Metadata stream split in 8 KiB blocks, each LZO compressed (or stored). */
private class Meta {
    val out = ByteBuf(1 shl 14)
    val buf = ByteBuf(META_SIZE * 2)
    val blockStarts = ArrayList<Int>()

    fun write(b: ByteArray, off: Int = 0, len: Int = b.size - off) {
        buf.bytes(b, off, len)
        while (buf.size >= META_SIZE) {
            flush(META_SIZE)
            buf.dropFirst(META_SIZE)
        }
    }

    fun finish(): ByteBuf {
        if (buf.size > 0) {
            flush(buf.size)
            buf.clear()
        }
        return out
    }

    private fun flush(n: Int) {
        blockStarts.add(out.size)
        val c = Lzo1x.compress(buf.buf, 0, n)
        if (c.size < n) {
            out.u16(c.size).bytes(c)
        } else {
            out.u16(n or 0x8000).bytes(buf.buf, 0, n)
        }
    }
}

object SquashWriter {
    /**
     * Writes a new image for the tree [root] to [out] (which must be empty). [src] is the
     * source image whose blocks are referenced by unchanged files. The image is not padded.
     * [progress] receives (bytes of file data handled, total).
     */
    fun write(root: Inode, src: SquashImage, out: Sink, mtimeOverride: Long? = null,
              progress: ((Long, Long) -> Unit)? = null): SquashStats {
        val bs = src.blockSize
        val mtime = mtimeOverride ?: src.mtime
        val entries = SquashImage.walk(root)

        // unique inodes in write order (post-order: children before their directory)
        for ((_, n) in entries) { n.number = 0; n.nrefs = 0; n.ref = -1; n.layout = null }
        val order = ArrayList<Inode>()
        fun visit(d: Inode, depth: Int) {
            if (depth > 200) throw SquashfsException("directory tree too deep")
            val ch = d.children!!
            for (name in ch.keys.sorted()) {
                val c = ch[name]!!
                if (c.kind == Kind.DIR) {
                    visit(c, depth + 1)
                } else if (c.number == 0) {
                    c.number = -1
                    order.add(c)
                }
            }
            order.add(d)
        }
        visit(root, 0)
        for ((i, n) in order.withIndex()) n.number = i + 1
        for ((_, n) in entries) n.nrefs++
        for (n in order) {
            if (n.kind == Kind.DIR && n.nrefs != 1) throw SquashfsException("a directory is linked twice")
        }

        // ------------------------------------------------------------ data area
        if (out.position != 0L) throw SquashfsException("the output is not empty")
        out.write(ByteArray(96))
        val copyBuf = ByteArray(1 shl 16)
        // 1) original blocks still referenced, copied verbatim (relocated)
        val ranges = ArrayList<LongArray>()
        val usedFrags = java.util.TreeSet<Int>()
        var newTotal = 0L
        for (n in order) {
            if (n.kind != Kind.FILE) continue
            if (n.data != null) { newTotal += n.data!!.size; continue }
            val o = n.orig ?: throw SquashfsException("file without content")
            val length = src.blockRunLength(o)
            if (length != 0L) ranges.add(longArrayOf(o.start, o.start + length))
            if (o.frag >= 0) usedFrags.add(o.frag)
        }
        for (fi in usedFrags) {
            if (fi >= src.fragCount) throw SquashfsException("bad fragment index")
            val start = src.fragStart[fi]
            ranges.add(longArrayOf(start, start + (src.fragSize[fi] and UNCOMP_BLOCK.inv())))
        }
        ranges.sortWith { a, b -> if (a[0] != b[0]) a[0].compareTo(b[0]) else a[1].compareTo(b[1]) }
        val merged = ArrayList<LongArray>()
        for (r in ranges) {
            if (r[0] < 96 || r[1] > src.inodeTable) throw SquashfsException("data block outside the data area")
            if (merged.isNotEmpty() && r[0] <= merged.last()[1]) {
                merged.last()[1] = maxOf(merged.last()[1], r[1])
            } else {
                merged.add(longArrayOf(r[0], r[1]))
            }
        }
        var keptTotal = 0L
        for (m in merged) keptTotal += m[1] - m[0]
        val grand = keptTotal + newTotal
        var done = 0L
        val relocNew = LongArray(merged.size)
        for ((i, m) in merged.withIndex()) {
            relocNew[i] = out.position
            var p = m[0]
            while (p < m[1]) {
                val n = minOf(m[1] - p, 1L shl 20)
                src.copyRaw(p, n, out, copyBuf)
                p += n
                done += n
                progress?.invoke(done, grand)
            }
        }
        val keptBytes = out.position - 96

        fun reloc(old: Long): Long {
            // index of the last merged range starting at or before `old`
            var lo = 0
            var hi = merged.size
            while (lo < hi) {
                val mid = (lo + hi) ushr 1
                if (merged[mid][0] <= old) lo = mid + 1 else hi = mid
            }
            val i = lo - 1
            if (i < 0 || old >= merged[i][1]) throw SquashfsException("unrelocatable block")
            return relocNew[i] + (old - merged[i][0])
        }

        val fragTabStart = ArrayList<Long>()
        val fragTabSize = ArrayList<Int>()
        val fragMap = HashMap<Int, Int>()
        for (fi in usedFrags) {
            fragMap[fi] = fragTabStart.size
            fragTabStart.add(reloc(src.fragStart[fi]))
            fragTabSize.add(src.fragSize[fi])
        }

        // 2) new data (deduplicated by content)
        val byHash = HashMap<String, FileRef>()
        val fragBuf = ByteBuf(bs)
        var newBytes = 0L
        val sha = sha256()

        fun writeBlock(data: ByteArray, off: Int, len: Int): Int {
            val c = Lzo1x.compress(data, off, len)
            return if (c.size < len) {
                out.write(c)
                c.size
            } else {
                out.write(data, off, len)
                len or UNCOMP_BLOCK
            }
        }

        fun flushFrag() {
            if (fragBuf.size == 0) return
            val pos = out.position
            val field = writeBlock(fragBuf.buf, 0, fragBuf.size)
            fragTabStart.add(pos)
            fragTabSize.add(field)
            fragBuf.clear()
        }

        for (n in order) {
            if (n.kind != Kind.FILE) continue
            val data = n.data
            if (data == null) {
                val o = n.orig!!
                val length = src.blockRunLength(o)
                n.layout = FileRef(if (length != 0L) reloc(o.start) else out.position, o.sizes,
                    if (o.frag < 0) -1 else fragMap[o.frag]!!, o.fragOffset, o.sparse)
                continue
            }
            val key = "${data.size}:" + Hex.of(sha.digest(data))
            val known = byHash[key]
            if (known != null) {
                n.layout = known
                continue
            }
            newBytes += data.size
            val lay: FileRef
            if (data.isNotEmpty() && data.size < bs) {
                if (fragBuf.size + data.size > bs) flushFrag()
                lay = FileRef(out.position, IntArray(0), fragTabStart.size, fragBuf.size, 0)
                fragBuf.bytes(data)
            } else {
                val start = out.position
                val sizes = IntArray((data.size + bs - 1) / bs)
                var i = 0
                var k = 0
                while (i < data.size) {
                    sizes[k++] = writeBlock(data, i, minOf(bs, data.size - i))
                    i += bs
                }
                lay = FileRef(start, sizes, -1, 0, 0)
            }
            n.layout = lay
            byHash[key] = lay
            done += data.size
            progress?.invoke(done, grand)
        }
        flushFrag()
        val dataEnd = out.position

        // ------------------------------------------------------------ ids
        val ids = ArrayList<Long>()
        val idx = HashMap<Long, Int>()
        for (n in order) {
            for (v in longArrayOf(n.uid, n.gid)) {
                if (v !in idx) {
                    idx[v] = ids.size
                    ids.add(v)
                }
            }
        }
        if (ids.size > 65535) throw SquashfsException("too many ids")

        // ------------------------------------------------------------ inode + dir tables
        val itab = Meta()
        val dtab = Meta()
        val total = order.size
        var written = 0

        fun hdr(itype: Int, n: Inode) = ByteBuf(64).u16(itype).u16(n.mode and 0xFFF).u16(idx[n.uid]!!)
            .u16(idx[n.gid]!!).u32(n.mtime ?: mtime).u32(n.number.toLong())

        fun writeInode(n: Inode, parentNo: Int = 0, dblk: Int = 0, doff: Int = 0, dsize: Int = 0): Long {
            val blk = itab.out.size
            val off = itab.buf.size
            var nlink = n.nrefs.toLong()
            val rec: ByteBuf
            when (n.kind) {
                Kind.DIR -> {
                    nlink = 2L + n.children!!.values.count { it.kind == Kind.DIR }
                    rec = if (dsize + 3 <= 0xFFFF) {
                        hdr(T_DIR, n).u32(dblk.toLong()).u32(nlink).u16(dsize + 3).u16(doff).u32(parentNo.toLong())
                    } else {
                        hdr(T_LDIR, n).u32(nlink).u32(dsize + 3L).u32(dblk.toLong()).u32(parentNo.toLong())
                            .u16(0).u16(doff).u32(NO_XATTR)
                    }
                }
                Kind.FILE -> {
                    val l = n.layout!!
                    val fragf = if (l.frag < 0) NO_FRAG else l.frag.toLong()
                    if (l.frag >= 0 && !(0 < n.size && l.sizes.size.toLong() == n.size / bs)) {
                        throw SquashfsException("inconsistent fragment layout")
                    }
                    if (l.frag < 0 && l.sizes.size.toLong() != (n.size + bs - 1) / bs) {
                        throw SquashfsException("inconsistent block layout")
                    }
                    rec = if (nlink == 1L && l.start < (1L shl 32) && n.size < (1L shl 32)) {
                        hdr(T_REG, n).u32(l.start).u32(fragf).u32(l.fragOffset.toLong()).u32(n.size)
                    } else {
                        hdr(T_LREG, n).u64(l.start).u64(n.size).u64(l.sparse).u32(nlink).u32(fragf)
                            .u32(l.fragOffset.toLong()).u32(NO_XATTR)
                    }
                    for (s in l.sizes) rec.u32(s.toLong())
                }
                Kind.SYMLINK -> {
                    val t = n.target ?: throw SquashfsException("symlink without target")
                    rec = hdr(T_SYM, n).u32(nlink).u32(t.size.toLong()).bytes(t)
                }
                Kind.BLKDEV, Kind.CHRDEV -> rec = hdr(n.kind.basic, n).u32(nlink).u32(n.rdev)
                else -> rec = hdr(n.kind.basic, n).u32(nlink)
            }
            itab.write(rec.buf, 0, rec.size)
            val ref = (blk.toLong() shl 16) or off.toLong()
            n.ref = ref
            written++
            return ref
        }

        fun writeDir(d: Inode, parentNo: Int): Long {
            val myNo = d.number
            val ch = d.children!!
            val names = ch.keys.sorted()
            val items = ArrayList<Pair<ByteArray, Inode>>(names.size)
            for (name in names) {
                val c = ch[name]!!
                val raw = name.toByteArray(Charsets.ISO_8859_1)
                if (name.isEmpty() || name.contains('/') || name == "." || name == ".." || raw.size > 256 ||
                    name.any { it.code > 255 }) {
                    throw SquashfsException("invalid name $name")
                }
                if (c.kind == Kind.DIR) {
                    writeDir(c, myNo)
                } else if (c.ref < 0) {
                    writeInode(c)
                }
                items.add(raw to c)
            }
            // directory listing
            val dblk = dtab.out.size
            val doff = dtab.buf.size
            val lst = ByteBuf(1024)
            var i = 0
            while (i < items.size) {
                val ref0 = items[i].second.ref
                val base = items[i].second.number
                val first = i
                while (i < items.size && i - first < 256 &&
                    (items[i].second.ref ushr 16) == (ref0 ushr 16) &&
                    (items[i].second.number - base) in -32768..32767) {
                    i++
                }
                lst.u32((i - first - 1).toLong()).u32(ref0 ushr 16).u32(base.toLong())
                for (k in first until i) {
                    val (raw, c) = items[k]
                    lst.u16((c.ref and 0xFFFF).toInt()).u16((c.number - base) and 0xFFFF).u16(c.kind.basic)
                        .u16(raw.size - 1).bytes(raw)
                }
            }
            dtab.write(lst.buf, 0, lst.size)
            return writeInode(d, parentNo, dblk, doff, lst.size)
        }

        val rootRef = writeDir(root, total + 1)
        if (written != total) throw SquashfsException("not every inode was written")

        // ------------------------------------------------------------ tables
        val inodeTable = out.position
        itab.finish().let { out.write(it.buf, 0, it.size) }
        val dirTable = out.position
        dtab.finish().let { out.write(it.buf, 0, it.size) }

        fun indexed(raw: ByteBuf): Long {
            val m = Meta()
            val base = out.position
            m.write(raw.buf, 0, raw.size)
            m.finish().let { out.write(it.buf, 0, it.size) }
            val start = out.position
            val ptrs = ByteBuf(8 * m.blockStarts.size + 8)
            for (s in m.blockStarts) ptrs.u64(base + s)
            out.write(ptrs.buf, 0, ptrs.size)
            return start
        }

        val fragRaw = ByteBuf(16 * fragTabStart.size + 16)
        for (i in fragTabStart.indices) fragRaw.u64(fragTabStart[i]).u32(fragTabSize[i].toLong() and 0xFFFFFFFFL).u32(0)
        val fragStart = indexed(fragRaw)
        val lookup = ByteBuf(8 * total + 8)
        for (n in order) lookup.u64(n.ref)   // order[i] has number i + 1
        val exportStart = indexed(lookup)
        val idRaw = ByteBuf(4 * ids.size + 4)
        for (v in ids) idRaw.u32(v)
        val idStart = indexed(idRaw)
        val bytesUsed = out.position

        val flags = FLAG_DUPLICATES or FLAG_EXPORTABLE or FLAG_NO_XATTRS
        val sb = ByteBuf(96).u32(SQ_MAGIC).u32(total.toLong()).u32(mtime).u32(bs.toLong())
            .u32(fragTabStart.size.toLong()).u16(COMP_LZO).u16(src.blockLog).u16(flags).u16(ids.size)
            .u16(4).u16(0).u64(rootRef).u64(bytesUsed).u64(idStart).u64(INVALID_BLK).u64(inodeTable)
            .u64(dirTable).u64(fragStart).u64(exportStart)
        out.writeAt(0, sb.toByteArray())
        return SquashStats(keptBytes, newBytes, dataEnd - 96 - keptBytes, bytesUsed - dataEnd, total,
            fragTabStart.size, bytesUsed)
    }
}
