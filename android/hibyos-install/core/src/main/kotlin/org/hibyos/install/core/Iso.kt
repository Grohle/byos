package org.hibyos.install.core

import java.io.EOFException
import java.io.OutputStream
import java.time.LocalDateTime
import java.time.ZoneOffset
import java.util.Locale

/**
 * The .upt firmware container is an ISO 9660 image. This file has a reader (file list with
 * Rock Ridge names) and a writer that produces, byte for byte, what tools/build.py write_upt()
 * produces with pycdlib 1.22.0 (interchange level 1 + Joliet level 3 + Rock Ridge 1.09).
 */
class IsoException(message: String) : Exception(message)

/** A regular file inside an ISO image: [path] without leading slash, bytes at [offset]. */
class IsoEntry(val path: String, val offset: Long, val size: Long)

object IsoReader {
    private const val SECTOR = 2048

    /** Lists every regular file by its Rock Ridge path, like pycdlib's walk(rr_path="/"). */
    fun list(src: ByteSource): LinkedHashMap<String, IsoEntry> {
        try {
            return listImpl(src)
        } catch (e: EOFException) {
            throw IsoException("the file is cut short or is not a firmware container")
        } catch (e: IndexOutOfBoundsException) {
            throw IsoException("the file is damaged (bad directory record)")
        }
    }

    private fun listImpl(src: ByteSource): LinkedHashMap<String, IsoEntry> {
        var pvd: ByteArray? = null
        for (s in 16 until 48) {
            val b = src.bytes(s.toLong() * SECTOR, SECTOR)
            if (latin1(b, 1, 5) != "CD001") throw IsoException("this is not a firmware container (.upt)")
            val t = b.u8(0)
            if (t == 1) { pvd = b; break }
            if (t == 255) break
        }
        if (pvd == null) throw IsoException("this is not a firmware container (.upt)")
        if (pvd.u16(128) != SECTOR) throw IsoException("unsupported block size in the firmware container")
        val out = LinkedHashMap<String, IsoEntry>()
        var skip = 0
        var records = 0

        fun rrName(dir: ByteArray, from: Int, to: Int): String? {
            var area = dir
            var a = from
            var b = to
            val name = ByteBuf(64)
            var found = false
            var hops = 0
            while (true) {
                var ce: LongArray? = null
                while (a + 4 <= b) {
                    val sig = latin1(area, a, 2)
                    val l = area.u8(a + 2)
                    if (l < 4 || a + l > b) break
                    when (sig) {
                        "NM" -> if (l >= 5 && area.u8(a + 4) and 6 == 0) {
                            name.bytes(area, a + 5, l - 5)
                            found = true
                        }
                        "CE" -> if (l >= 28) ce = longArrayOf(area.u32(a + 4), area.u32(a + 12), area.u32(a + 20))
                        "ST" -> { a = b; continue }
                    }
                    a += l
                }
                if (ce == null || ++hops > 8) break
                if (ce[2] > 1 shl 16) throw IsoException("the file is damaged (bad continuation area)")
                area = src.bytes(ce[0] * SECTOR + ce[1], ce[2].toInt())
                a = 0
                b = area.size
            }
            return if (found) String(name.buf, 0, name.size, Charsets.UTF_8) else null
        }

        fun readDir(extent: Long, size: Long, prefix: String, depth: Int, isRoot: Boolean) {
            if (depth > 8) throw IsoException("the file is damaged (directories nested too deep)")
            if (size <= 0 || size > (16 shl 20)) throw IsoException("the file is damaged (bad directory size)")
            val data = src.bytes(extent * SECTOR, size.toInt())
            var p = 0
            var first = true
            while (p < data.size) {
                val len = data.u8(p)
                if (len == 0) {
                    p = (p / SECTOR + 1) * SECTOR
                    continue
                }
                if (len < 34 || p + len > data.size) throw IsoException("the file is damaged (bad directory record)")
                if (++records > 200_000) throw IsoException("the file is damaged (too many directory records)")
                val ext = data.u32(p + 2)
                val sz = data.u32(p + 10)
                val flags = data.u8(p + 25)
                val nlen = data.u8(p + 32)
                if (33 + nlen > len) throw IsoException("the file is damaged (bad directory record)")
                var su = p + 33 + nlen + (if (nlen % 2 == 0) 1 else 0)
                val end = p + len
                val dot = nlen == 1 && (data.u8(p + 33) == 0 || data.u8(p + 33) == 1)
                if (isRoot && first && su + 7 <= end && latin1(data, su, 2) == "SP" &&
                    data.u8(su + 4) == 0xBE && data.u8(su + 5) == 0xEF) {
                    skip = data.u8(su + 6)
                }
                first = false
                if (!dot) {
                    su = minOf(su + skip, end)
                    val ident = latin1(data, p + 33, nlen)
                    val name = rrName(data, su, end)
                        ?: ident.substringBefore(';').trimEnd('.').lowercase(Locale.ROOT)
                    if (name.isEmpty() || name.contains('/')) throw IsoException("the file is damaged (bad file name)")
                    if (flags and 2 != 0) {
                        readDir(ext, sz, "$prefix$name/", depth + 1, false)
                    } else {
                        if (flags and 0x80 != 0) throw IsoException("unsupported firmware container (multi-extent file)")
                        val off = ext * SECTOR
                        if (off + sz > src.size) throw IsoException("the file is cut short")
                        out[prefix + name] = IsoEntry(prefix + name, off, sz)
                    }
                }
                p += len
            }
        }

        readDir(pvd.u32(156 + 2), pvd.u32(156 + 10), "", 0, true)
        return out
    }
}

/** One file to store: [path] is "name" or "ota_v0/name". */
class IsoInput(val path: String, val data: ByteSource)

object UptIsoWriter {
    private const val SECTOR = 2048
    private const val DIR_NAME = "ota_v0"
    private const val FILE_MODE = 0x8124   // 0100444
    private const val DIR_MODE = 0x416d    // 040555
    private const val APP_ID = "PyCdlib (C) 2015-2020 Chris Lalancette"
    private const val ER_ID = "RRIP_1991A"
    private const val ER_DESC = "THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS"
    private const val ER_SRC = "PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  SEE PUBLISHER IDENTIFIER IN " +
        "PRIMARY VOLUME DESCRIPTOR FOR CONTACT INFORMATION."

    private class Node(val path: String, val name: String, val iso: String, val data: ByteSource?) {
        var extent = 0L
        val size: Long get() = data!!.size
    }

    /**
     * ISO 9660 level 1 names chosen by write_upt() for the given paths:
     * path -> identifier without the directory (for example "ROOTFS1.02A;1").
     */
    fun shortNames(paths: Collection<String>): LinkedHashMap<String, String> {
        val used = HashSet<String>()
        val out = LinkedHashMap<String, String>()
        val bad = Regex("[^A-Z0-9_]")
        for (p in paths.sorted()) {
            val d = if ('/' in p) p.substringBeforeLast('/') else ""
            val n = p.substringAfterLast('/')
            var stem = bad.replace(n.substringBefore('.').uppercase(Locale.ROOT), "_").take(8).ifEmpty { "F" }
            val tail = if ('.' in n) bad.replace(n.substringAfterLast('.').uppercase(Locale.ROOT), "_").take(3) else ""
            var k = 0
            var short: String
            while (true) {
                short = if (tail.isNotEmpty()) "$stem.$tail" else stem
                if (used.add("$d/$short")) break
                k++
                stem = stem.take(8 - k.toString().length) + k
            }
            out[p] = "$short;1"
        }
        return out
    }

    private fun dirDate(epoch: Long, offset: Int): ByteArray {
        val l = LocalDateTime.ofEpochSecond(epoch + offset, 0, ZoneOffset.UTC)
        return byteArrayOf((l.year - 1900).toByte(), l.monthValue.toByte(), l.dayOfMonth.toByte(), l.hour.toByte(),
            l.minute.toByte(), l.second.toByte(), gmtOffset(epoch, offset).toByte())
    }

    private fun volDate(epoch: Long, offset: Int): ByteArray {
        val l = LocalDateTime.ofEpochSecond(epoch + offset, 0, ZoneOffset.UTC)
        val t = String.format(Locale.ROOT, "%04d%02d%02d%02d%02d%02d%02d", l.year, l.monthValue, l.dayOfMonth, l.hour,
            l.minute, l.second, 0)
        return t.toByteArray(Charsets.US_ASCII) + gmtOffset(epoch, offset).toByte()
    }

    /** pycdlib utils.gmtoffset_from_tm(): offset from GMT in 15 minute units. */
    internal fun gmtOffset(epoch: Long, offset: Int): Int {
        val g = LocalDateTime.ofEpochSecond(epoch, 0, ZoneOffset.UTC)
        val l = LocalDateTime.ofEpochSecond(epoch + offset, 0, ZoneOffset.UTC)
        val min = l.minute - g.minute
        val hour = l.hour - g.hour
        var yday = l.dayOfYear - g.dayOfYear
        val year = l.year - g.year
        if (year != 0) yday = year
        return Math.floorDiv(min + 60 * (hour + 24 * yday), 15)
    }

    /**
     * Writes the image to [out] and returns its size. [epochSeconds] and [utcOffsetSeconds]
     * (local time minus UTC) fill every date field; pycdlib uses the current local time.
     */
    fun write(out: OutputStream, files: List<IsoInput>, epochSeconds: Long, utcOffsetSeconds: Int,
              progress: ((Long, Long) -> Unit)? = null): Long {
        val short = shortNames(files.map { it.path })
        if (short.size != files.size) throw IsoException("duplicate file names")
        val rootFiles = ArrayList<Node>()
        val subFiles = ArrayList<Node>()
        for (f in files) {
            val d = if ('/' in f.path) f.path.substringBeforeLast('/') else ""
            val n = f.path.substringAfterLast('/')
            if (n.isEmpty() || n.any { it.code < 0x20 || it.code > 0x7e }) throw IsoException("unsupported file name ${f.path}")
            if (f.data.size <= 0 || f.data.size > 0xFFFFFFFFL) throw IsoException("unsupported file size for ${f.path}")
            val node = Node(f.path, n, short[f.path]!!, f.data)
            when (d) {
                "" -> rootFiles.add(node)
                DIR_NAME -> subFiles.add(node)
                else -> throw IsoException("unsupported directory in ${f.path}")
            }
        }
        val dirNode = Node(DIR_NAME, DIR_NAME, DIR_NAME.uppercase(Locale.ROOT), null)
        val date = dirDate(epochSeconds, utcOffsetSeconds)
        val vdate = volDate(epochSeconds, utcOffsetSeconds)

        // children in directory order (bytewise on the identifier of each namespace)
        val pvdRoot = (rootFiles + dirNode).sortedBy { it.iso }
        val pvdSub = subFiles.sortedBy { it.iso }
        val jolRoot = (rootFiles + dirNode).sortedBy { it.name }
        val jolSub = subFiles.sortedBy { it.name }

        // ---- record builders (extents are patched in once known, so build lazily)
        fun record(extent: Long, size: Long, isDir: Boolean, ident: ByteArray, su: ByteArray?): ByteArray {
            val b = ByteBuf(256)
            b.u8(0).u8(0).both32(extent).both32(size).bytes(date).u8(if (isDir) 2 else 0).u8(0).u8(0).both16(1)
                .u8(ident.size).bytes(ident)
            if (ident.size % 2 == 0) b.u8(0)
            if (su != null) b.bytes(su)
            if (b.size % 2 != 0) b.u8(0)
            if (b.size > 254) throw IsoException("file name too long for the firmware container")
            b.buf[0] = b.size.toByte()
            return b.toByteArray()
        }

        fun px(mode: Int, nlink: Int) = ByteBuf(36).bytes(byteArrayOf(0x50, 0x58, 36, 1)).both32(mode.toLong())
            .both32(nlink.toLong()).both32(0).both32(0)

        fun tf() = ByteBuf(26).bytes(byteArrayOf(0x54, 0x46, 26, 1, 0x0e)).bytes(date).bytes(date).bytes(date)

        fun rrDot(mode: Int, nlink: Int, rootDot: Boolean, erExtent: Long): ByteArray {
            val b = ByteBuf(128)
            if (rootDot) b.bytes(byteArrayOf(0x53, 0x50, 7, 1, 0xBE.toByte(), 0xEF.toByte(), 0))
            b.bytes(byteArrayOf(0x52, 0x52, 5, 1, 0x81.toByte()))
            b.bytes(px(mode, nlink).toByteArray()).bytes(tf().toByteArray())
            if (rootDot) b.bytes(byteArrayOf(0x43, 0x45, 28, 1)).both32(erExtent).both32(0).both32(erLength().toLong())
            return b.toByteArray()
        }

        fun rrNamed(name: String, mode: Int, nlink: Int): ByteArray {
            val nm = name.toByteArray(Charsets.US_ASCII)
            val b = ByteBuf(128)
            b.bytes(byteArrayOf(0x52, 0x52, 5, 1, 0x89.toByte()))
            b.bytes(byteArrayOf(0x4e, 0x4d, (5 + nm.size).toByte(), 1, 0)).bytes(nm)
            b.bytes(px(mode, nlink).toByteArray()).bytes(tf().toByteArray())
            return b.toByteArray()
        }

        val dot = byteArrayOf(0)
        val dotdot = byteArrayOf(1)
        fun ascii(s: String) = s.toByteArray(Charsets.US_ASCII)
        fun ucs2(s: String) = s.toByteArray(Charsets.UTF_16BE)

        // ---- layout: record lengths do not depend on extents, so lay out with zero extents
        fun sectorsOf(records: List<ByteArray>): Int {
            var n = 1
            var off = 0
            for (r in records) {
                if (off + r.size > SECTOR) { n++; off = 0 }
                off += r.size
            }
            return n
        }

        var pvdRootExt = 0L; var pvdSubExt = 0L; var jolRootExt = 0L; var jolSubExt = 0L; var erExt = 0L
        var pvdRootLen = 0L; var pvdSubLen = 0L; var jolRootLen = 0L; var jolSubLen = 0L

        fun extentOf(n: Node, joliet: Boolean) = if (n.data == null) (if (joliet) jolSubExt else pvdSubExt) else n.extent
        fun sizeOf(n: Node, joliet: Boolean) = if (n.data == null) (if (joliet) jolSubLen else pvdSubLen) else n.size

        fun pvdRootRecords(): List<ByteArray> {
            val l = ArrayList<ByteArray>()
            l.add(record(pvdRootExt, pvdRootLen, true, dot, rrDot(DIR_MODE, 3, true, erExt)))
            l.add(record(pvdRootExt, pvdRootLen, true, dotdot, rrDot(DIR_MODE, 3, false, 0)))
            for (n in pvdRoot) {
                val isDir = n.data == null
                l.add(record(extentOf(n, false), sizeOf(n, false), isDir, ascii(n.iso),
                    rrNamed(n.name, if (isDir) DIR_MODE else FILE_MODE, if (isDir) 2 else 1)))
            }
            return l
        }

        fun pvdSubRecords(): List<ByteArray> {
            val l = ArrayList<ByteArray>()
            l.add(record(pvdSubExt, pvdSubLen, true, dot, rrDot(DIR_MODE, 2, false, 0)))
            l.add(record(pvdRootExt, pvdRootLen, true, dotdot, rrDot(DIR_MODE, 3, false, 0)))
            for (n in pvdSub) l.add(record(n.extent, n.size, false, ascii(n.iso), rrNamed(n.name, FILE_MODE, 1)))
            return l
        }

        fun jolRootRecords(): List<ByteArray> {
            val l = ArrayList<ByteArray>()
            l.add(record(jolRootExt, jolRootLen, true, dot, null))
            l.add(record(jolRootExt, jolRootLen, true, dotdot, null))
            for (n in jolRoot) l.add(record(extentOf(n, true), sizeOf(n, true), n.data == null, ucs2(n.name), null))
            return l
        }

        fun jolSubRecords(): List<ByteArray> {
            val l = ArrayList<ByteArray>()
            l.add(record(jolSubExt, jolSubLen, true, dot, null))
            l.add(record(jolRootExt, jolRootLen, true, dotdot, null))
            for (n in jolSub) l.add(record(n.extent, n.size, false, ucs2(n.name), null))
            return l
        }

        val nPvdRoot = sectorsOf(pvdRootRecords())
        val nPvdSub = sectorsOf(pvdSubRecords())
        val nJolRoot = sectorsOf(jolRootRecords())
        val nJolSub = sectorsOf(jolSubRecords())
        // 16 PVD, 17 Joliet SVD, 18 terminator, 19 version block, 20..27 four path tables (2 blocks each)
        pvdRootExt = 28
        pvdSubExt = pvdRootExt + nPvdRoot
        jolRootExt = pvdSubExt + nPvdSub
        jolSubExt = jolRootExt + nJolRoot
        erExt = jolSubExt + nJolSub
        pvdRootLen = nPvdRoot.toLong() * SECTOR
        pvdSubLen = nPvdSub.toLong() * SECTOR
        jolRootLen = nJolRoot.toLong() * SECTOR
        jolSubLen = nJolSub.toLong() * SECTOR
        var next = erExt + 1
        val dataOrder = pvdRoot.filter { it.data != null } + pvdSub
        for (n in dataOrder) {
            n.extent = next
            next += (n.size + SECTOR - 1) / SECTOR
        }
        val totalSectors = next
        if (totalSectors > 0x7FFFFFFFL / SECTOR * 4) throw IsoException("firmware container too big")

        // ---- emit
        var written = 0L
        fun put(b: ByteArray, len: Int = b.size) {
            out.write(b, 0, len)
            written += len
        }

        fun sector(b: ByteBuf) {
            if (b.size > SECTOR) throw IsoException("internal error: sector overflow")
            put(b.buf.copyOf(SECTOR))
        }

        fun dirBlock(records: List<ByteArray>, sectors: Int) {
            val blk = ByteArray(sectors * SECTOR)
            var off = 0
            for (r in records) {
                if (off % SECTOR + r.size > SECTOR) off = (off / SECTOR + 1) * SECTOR
                System.arraycopy(r, 0, blk, off, r.size)
                off += r.size
            }
            put(blk)
        }

        fun padA(s: String, n: Int) = ascii(s).copyOf(n).also { java.util.Arrays.fill(it, s.length, n, 0x20) }
        fun padU(s: String, n: Int): ByteArray {
            val b = ByteArray(n)
            val enc = ucs2(s)
            System.arraycopy(enc, 0, b, 0, minOf(enc.size, n))
            var i = enc.size
            while (i < n) { b[i] = if ((i - enc.size) % 2 == 0) 0 else 0x20; i++ }
            return b
        }

        fun volumeDescriptor(joliet: Boolean): ByteBuf {
            val pad: (String, Int) -> ByteArray = if (joliet) ::padU else ::padA
            val b = ByteBuf(SECTOR)
            b.u8(if (joliet) 2 else 1).bytes(ascii("CD001")).u8(1).u8(0)
            b.bytes(pad("LINUX", 32)).bytes(pad("CDROM", 32)).fill(0, 8).both32(totalSectors)
            if (joliet) b.bytes(byteArrayOf(0x25, 0x2f, 0x45)).fill(0, 29) else b.fill(0, 32)
            b.both16(1).both16(1).both16(SECTOR)
            val ptSize = if (joliet) 10 + 8 + 2 * DIR_NAME.length else 10 + 8 + DIR_NAME.length
            b.both32(ptSize.toLong())
            b.u32(if (joliet) 24 else 20).u32(0).be32(if (joliet) 26 else 22).be32(0)
            b.bytes(if (joliet) record(jolRootExt, jolRootLen, true, dot, null) else record(pvdRootExt, pvdRootLen, true, dot, null))
            b.bytes(pad("", 128)).bytes(pad("", 128)).bytes(pad("", 128)).bytes(pad(APP_ID, 128))
            b.bytes(pad("", 37)).bytes(pad("", 37)).bytes(pad("", 37))
            b.bytes(vdate).bytes(vdate).bytes(ascii("0000000000000000")).u8(0).bytes(vdate)
            b.u8(1).u8(0).fill(0x20, 512)
            return b
        }

        fun pathTable(joliet: Boolean, big: Boolean): ByteBuf {
            val b = ByteBuf(SECTOR * 2)
            val rootExt = if (joliet) jolRootExt else pvdRootExt
            val subExt = if (joliet) jolSubExt else pvdSubExt
            val name = if (joliet) ucs2(DIR_NAME) else ascii(DIR_NAME.uppercase(Locale.ROOT))
            b.u8(1).u8(0)
            if (big) b.be32(rootExt).be16(1) else b.u32(rootExt).u16(1)
            b.u8(0).u8(0)
            b.u8(name.size).u8(0)
            if (big) b.be32(subExt).be16(1) else b.u32(subExt).u16(1)
            b.bytes(name)
            if (name.size % 2 != 0) b.u8(0)
            return b
        }

        put(ByteArray(16 * SECTOR))
        sector(volumeDescriptor(false))
        sector(volumeDescriptor(true))
        sector(ByteBuf(8).u8(255).bytes(ascii("CD001")).u8(1))
        sector(ByteBuf(1))
        for (joliet in booleanArrayOf(false, true)) {
            for (big in booleanArrayOf(false, true)) put(pathTable(joliet, big).buf.copyOf(2 * SECTOR))
        }
        dirBlock(pvdRootRecords(), nPvdRoot)
        dirBlock(pvdSubRecords(), nPvdSub)
        dirBlock(jolRootRecords(), nJolRoot)
        dirBlock(jolSubRecords(), nJolSub)
        sector(erRecord())
        if (written != (erExt + 1) * SECTOR) throw IsoException("internal error: layout mismatch")

        val total = totalSectors * SECTOR
        val buf = ByteArray(1 shl 16)
        val zeros = ByteArray(SECTOR)
        for (n in dataOrder) {
            if (written != n.extent * SECTOR) throw IsoException("internal error: layout mismatch")
            var pos = 0L
            val size = n.size
            while (pos < size) {
                val k = minOf(size - pos, buf.size.toLong()).toInt()
                n.data!!.read(pos, buf, 0, k)
                put(buf, k)
                pos += k
            }
            val pad = ((SECTOR - size % SECTOR) % SECTOR).toInt()
            if (pad != 0) put(zeros, pad)
            progress?.invoke(written, total)
        }
        if (written != total) throw IsoException("internal error: layout mismatch")
        return written
    }

    private fun erLength() = 8 + ER_ID.length + ER_DESC.length + ER_SRC.length

    private fun erRecord(): ByteBuf = ByteBuf(256).bytes(byteArrayOf(0x45, 0x52)).u8(erLength()).u8(1).u8(ER_ID.length)
        .u8(ER_DESC.length).u8(ER_SRC.length).u8(1).bytes(ER_ID.toByteArray(Charsets.US_ASCII))
        .bytes(ER_DESC.toByteArray(Charsets.US_ASCII)).bytes(ER_SRC.toByteArray(Charsets.US_ASCII))
}
