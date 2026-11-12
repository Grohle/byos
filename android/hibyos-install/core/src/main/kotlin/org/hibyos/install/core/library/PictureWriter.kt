package org.hibyos.install.core.library

import java.io.BufferedOutputStream
import java.io.File
import java.io.OutputStream
import java.io.RandomAccessFile

/**
 * Replaces the embedded front cover of a FLAC, MP3 or M4A file, writing a new file next to
 * nothing: [setCover] reads [src] and streams the result to [dst]; the audio bytes are copied
 * unchanged. A port of ByOS's own `ht_set_cover` (src/hibytag.c), the code the HiBy runs for
 * the same job:
 *
 * - FLAC: old front covers (PICTURE type 3) and padding are dropped, a new PICTURE block goes
 *   after the other blocks, then the Vorbis comment and [PAD] bytes of padding.
 * - MP3: old front covers (APIC type 3) are dropped and a new APIC is added to the ID3v2 tag
 *   (version kept; a new ID3v2.3 tag when there was none). ID3v2.2 and ID3v2.3 with
 *   unsynchronisation are not supported.
 * - M4A: moov/udta/meta/ilst/covr is replaced or created; when moov grows and comes before
 *   mdat, the chunk offsets in stco/co64 are moved. Fragmented MP4 is not supported.
 *
 * Ogg and Opus are not supported, as on the device.
 */
object PictureWriter {
    const val PAD = 8192
    private const val BIG = 1 shl 16

    class Unsupported(message: String) : TagException(message)

    /** Writes [src] with [jpeg] ([width] x [height]) as its front cover to [dst]. Returns the new size. */
    fun setCover(src: File, dst: File, jpeg: ByteArray, width: Int, height: Int): Long {
        require(src.absoluteFile != dst.absoluteFile) { "dst must be another file" }
        RandomAccessFile(src, "r").use { f ->
            val size = f.length()
            val head = ByteArray(minOf(12L, size).toInt()).also { f.seek(0); f.readFully(it) }
            fun at(o: Int, s: String) = head.size >= o + s.length && String(head, o, s.length, Charsets.ISO_8859_1) == s
            BufferedOutputStream(dst.outputStream(), BIG).use { out ->
                val w = Sink(f, out)
                when {
                    at(0, "fLaC") -> flac(w, 0, jpeg, width, height)
                    at(4, "ftyp") -> m4a(w, jpeg)
                    at(0, "ID3") -> {
                        val end = id3End(f)
                        val m = ByteArray(4)
                        if (end + 4 <= size) { f.seek(end); f.readFully(m) }
                        if (String(m, Charsets.ISO_8859_1) == "fLaC") flac(w, end, jpeg, width, height) else mp3(w, jpeg)
                    }
                    src.name.lowercase().endsWith(".mp3") -> mp3(w, jpeg)
                    else -> throw Unsupported("embedded covers are written to FLAC, MP3 and M4A files only")
                }
            }
        }
        return dst.length()
    }

    /** Copies ranges of the source and writes new bytes. */
    private class Sink(val f: RandomAccessFile, val out: OutputStream) {
        private val buf = ByteArray(BIG)
        var written = 0L
        fun copy(off: Long, len: Long) {
            f.seek(off)
            var left = len
            while (left > 0) {
                val n = minOf(left, buf.size.toLong()).toInt()
                f.readFully(buf, 0, n)
                out.write(buf, 0, n)
                left -= n; written += n
            }
        }
        fun put(b: ByteArray) { out.write(b); written += b.size }
        fun zero(n: Int) { put(ByteArray(n)) }
        fun read(off: Long, n: Int): ByteArray { val b = ByteArray(n); f.seek(off); f.readFully(b); return b }
    }

    private fun be32(v: Long) = byteArrayOf((v ushr 24).toByte(), (v ushr 16).toByte(), (v ushr 8).toByte(), v.toByte())
    private fun be(b: ByteArray, p: Int, n: Int): Long { var v = 0L; for (i in 0 until n) v = (v shl 8) or (b[p + i].toLong() and 0xff); return v }
    private fun ss32(v: Long) = byteArrayOf(((v ushr 21) and 0x7f).toByte(), ((v ushr 14) and 0x7f).toByte(), ((v ushr 7) and 0x7f).toByte(), (v and 0x7f).toByte())
    private fun ss(b: ByteArray, p: Int) = ((b[p].toLong() and 0x7f) shl 21) or ((b[p + 1].toLong() and 0x7f) shl 14) or
        ((b[p + 2].toLong() and 0x7f) shl 7) or (b[p + 3].toLong() and 0x7f)

    // ------------------------------------------------------------------ FLAC

    private class Blk(val type: Int, val off: Long, val len: Long)

    private fun flac(w: Sink, base: Long, jpeg: ByteArray, width: Int, height: Int) {
        val size = w.f.length()
        val blocks = ArrayList<Blk>()
        var pos = base + 4
        while (true) {
            if (pos + 4 > size) throw TagException("FLAC metadata runs past the end of the file")
            val h = w.read(pos, 4)
            val last = h[0].toInt() and 0x80 != 0
            val type = h[0].toInt() and 0x7f
            val len = be(h, 1, 3)
            if (pos + 4 + len > size) throw TagException("FLAC metadata runs past the end of the file")
            blocks += Blk(type, pos, len)
            pos += 4 + len
            if (last) break
            if (blocks.size > 256) throw Unsupported("too many FLAC metadata blocks")
        }
        val audio = pos
        if (blocks.isEmpty() || blocks[0].type != 0) throw TagException("FLAC without STREAMINFO")
        val pic = 42L + jpeg.size
        if (pic >= (1 shl 24)) throw Unsupported("the cover is too large for a FLAC block")
        fun hdr(type: Int, last: Boolean, len: Long) = byteArrayOf(((if (last) 0x80 else 0) or type).toByte(), (len ushr 16).toByte(), (len ushr 8).toByte(), len.toByte())
        val vc = blocks.indexOfFirst { it.type == 4 }
        w.copy(0, base + 4)
        for ((j, b) in blocks.withIndex()) {
            if (j == vc || b.type == 1) continue
            if (b.type == 6 && b.len >= 4 && be(w.read(b.off + 4, 4), 0, 4) == 3L) continue
            w.put(hdr(b.type, false, b.len)); w.copy(b.off + 4, b.len)
        }
        w.put(hdr(6, false, pic))
        w.put(be32(3)); w.put(be32(10)); w.put("image/jpeg".toByteArray(Charsets.ISO_8859_1))
        w.put(be32(0)); w.put(be32(width.toLong())); w.put(be32(height.toLong())); w.put(be32(24)); w.put(be32(0)); w.put(be32(jpeg.size.toLong()))
        w.put(jpeg)
        if (vc >= 0) { w.put(hdr(4, false, blocks[vc].len)); w.copy(blocks[vc].off + 4, blocks[vc].len) }
        w.put(hdr(1, true, PAD.toLong())); w.zero(PAD)
        w.copy(audio, size - audio)
    }

    // ------------------------------------------------------------------ MP3 (ID3v2)

    private fun id3End(f: RandomAccessFile): Long {
        val h = ByteArray(10); f.seek(0); f.readFully(h)
        return 10 + ss(h, 6) + if (h[3].toInt() == 4 && h[5].toInt() and 0x10 != 0) 10 else 0
    }

    private class Frame(val off: Long, val size: Long)

    private fun mp3(w: Sink, jpeg: ByteArray) {
        val fsize = w.f.length()
        var ver = 0; var flags = 0; var tagEnd = 0L
        val frames = ArrayList<Frame>()
        val drop = HashSet<Int>()
        if (fsize >= 10) {
            val h = w.read(0, 10)
            if (String(h, 0, 3, Charsets.ISO_8859_1) == "ID3" && h[3] in 2..4 && (h[6].toInt() or h[7].toInt() or h[8].toInt() or h[9].toInt()) and 0x80 == 0) {
                ver = h[3].toInt(); flags = h[5].toInt()
                val tsize = ss(h, 6)
                tagEnd = 10 + tsize + if (ver == 4 && flags and 0x10 != 0) 10 else 0
                if (tagEnd > fsize) throw TagException("the ID3 tag runs past the end of the file")
                if (ver == 2 || (ver == 3 && flags and 0x80 != 0)) throw Unsupported("ID3v2.2 tags and unsynchronised ID3v2.3 tags are not supported")
                var off = 10L
                if (flags and 0x40 != 0) {
                    val x = w.read(10, 4)
                    off += if (ver == 3) 4 + be(x, 0, 4) else ss(x, 0)
                }
                val lim = 10 + tsize
                while (off + 10 <= lim) {
                    val b = w.read(off, 10)
                    if (b[0].toInt() == 0 || !(0 until 4).all { val c = b[it].toInt().toChar(); c in 'A'..'Z' || c in '0'..'9' }) break
                    val sz = if (ver == 3 || ((b[4].toInt() or b[5].toInt() or b[6].toInt() or b[7].toInt()) and 0x80) != 0) be(b, 4, 4) else ss(b, 4)
                    if (off + 10 + sz > lim) break
                    if (String(b, 0, 4, Charsets.ISO_8859_1) == "APIC" && sz > 4) {
                        val fl = b[9].toInt() and 0xff
                        var skip = 0; var ok = true
                        if (ver == 3) { if (fl and 0xc0 != 0) ok = false; if (fl and 0x20 != 0) skip = 1 }
                        else { if (fl and 0x0e != 0) ok = false; if (fl and 0x40 != 0) skip++; if (fl and 0x01 != 0) skip += 4 }
                        val n = minOf(sz, 80L).toInt()
                        val d = w.read(off + 10, n)
                        if (ok && skip + 2 < n) {
                            val z = (skip + 1 until n).firstOrNull { d[it].toInt() == 0 }
                            if (z != null && z + 1 < n && d[z + 1].toInt() == 3) drop += frames.size
                        }
                    }
                    frames += Frame(off, 10 + sz)
                    off += 10 + sz
                }
            }
        }
        val nver = if (ver != 0) ver else 3
        var body = 10L + 14 + jpeg.size + PAD
        for ((i, fr) in frames.withIndex()) if (i !in drop) body += fr.size
        if (body >= (1L shl 28)) throw Unsupported("the ID3 tag would be too large")
        w.put(byteArrayOf('I'.code.toByte(), 'D'.code.toByte(), '3'.code.toByte(), nver.toByte(), 0, (if (ver != 0) flags and 0x20 else 0).toByte()))
        w.put(ss32(body))
        for ((i, fr) in frames.withIndex()) if (i !in drop) w.copy(fr.off, fr.size)
        val dl = 14L + jpeg.size
        w.put("APIC".toByteArray(Charsets.ISO_8859_1)); w.put(if (nver == 4) ss32(dl) else be32(dl)); w.put(byteArrayOf(0, 0))
        w.put(byteArrayOf(0) + "image/jpeg".toByteArray(Charsets.ISO_8859_1) + byteArrayOf(0, 3, 0))
        w.put(jpeg)
        w.zero(PAD)
        w.copy(tagEnd, fsize - tagEnd) // audio and any ID3v1 tag as they are
    }

    // ------------------------------------------------------------------ M4A / MP4

    private class Box(val type: String, val off: Long, val size: Long)

    private fun boxAt(w: Sink, off: Long, end: Long, strict: Boolean): Box? {
        if (off + 8 > end) return null
        val h = w.read(off, 8)
        var s = be(h, 0, 4)
        val t = String(h, 4, 4, Charsets.ISO_8859_1)
        if (s == 1L) {
            if (strict || off + 16 > end) return null
            s = be(w.read(off + 8, 8), 0, 8)
            if (s < 16) return null
        } else if (s == 0L) { if (strict) return null; s = end - off }
        else if (s < 8) return null
        if (off + s > end) return null
        return Box(t, off, s)
    }

    private fun find(w: Sink, off0: Long, end: Long, type: String): Box? {
        var off = off0
        while (off < end) {
            val b = boxAt(w, off, end, true) ?: throw Unsupported("unusual MP4 box layout")
            if (b.type == type) return b
            off += b.size
        }
        return null
    }

    private fun boxHdr(w: Sink, size: Long, type: String) { w.put(be32(size)); w.put(type.toByteArray(Charsets.ISO_8859_1)) }

    private fun m4a(w: Sink, jpeg: ByteArray) {
        val fsize = w.f.length()
        var moov: Box? = null; var nmoov = 0; var frag = false
        var off = 0L
        while (off < fsize) {
            val b = boxAt(w, off, fsize, false) ?: throw TagException("bad MP4 box at $off")
            if (b.type == "moov") {
                if (be(w.read(off, 4), 0, 4) != b.size) throw Unsupported("64-bit moov box")
                moov = b; nmoov++
            }
            if (b.type == "moof" || b.type == "sidx") frag = true
            off += b.size
        }
        if (nmoov != 1 || moov == null) throw TagException("MP4 without exactly one moov box")
        val mo = moov.off; val me = mo + moov.size
        val udta = find(w, mo + 8, me, "udta")
        val meta = udta?.let { find(w, it.off + 8, it.off + it.size, "meta") }
        if (meta != null && meta.size < 12) throw TagException("bad meta box")
        val ilst = meta?.let { find(w, it.off + 12, it.off + it.size, "ilst") }
        val covr = ilst?.let { find(w, it.off + 8, it.off + it.size, "covr") }
        val covrN = 8 + 16L + jpeg.size
        val ilstN = if (ilst != null) ilst.size - (covr?.size ?: 0) + covrN else 8 + covrN
        val metaN = if (meta != null) meta.size - (ilst?.size ?: 0) + ilstN else 12 + 33 + ilstN
        val udtaN = if (udta != null) udta.size - (meta?.size ?: 0) + metaN else 8 + metaN
        val moovN = moov.size - (udta?.size ?: 0) + udtaN
        if (moovN > 0xFFFFFFFFL) throw Unsupported("moov would be too large")
        val delta = moovN - moov.size
        if (delta != 0L && me < fsize && frag) throw Unsupported("fragmented MP4")

        fun covrOut() {
            boxHdr(w, covrN, "covr"); boxHdr(w, 16L + jpeg.size, "data"); w.put(be32(13)); w.put(be32(0)); w.put(jpeg)
        }
        fun kids(from: Long, end: Long, skip: Box?, fn: (() -> Unit)?) {
            var o = from
            while (o < end) {
                val b = boxAt(w, o, end, true) ?: throw TagException("bad MP4 box at $o")
                if (skip != null && o == skip.off) fn?.invoke() else w.copy(o, b.size)
                o += b.size
            }
        }
        fun ilstOut() {
            boxHdr(w, ilstN, "ilst")
            if (ilst != null) kids(ilst.off + 8, ilst.off + ilst.size, covr, null)
            covrOut()
        }
        fun metaOut() {
            boxHdr(w, metaN, "meta")
            if (meta != null) {
                w.copy(meta.off + 8, 4)
                kids(meta.off + 12, meta.off + meta.size, ilst, ::ilstOut)
                if (ilst == null) ilstOut()
            } else {
                // version/flags of meta, then hdlr "mdir" / "appl"
                w.put(byteArrayOf(0, 0, 0, 0, 0, 0, 0, 33) + "hdlr".toByteArray() + ByteArray(8) + "mdirappl".toByteArray() + ByteArray(9))
                ilstOut()
            }
        }
        fun udtaOut() {
            boxHdr(w, udtaN, "udta")
            if (udta != null) {
                kids(udta.off + 8, udta.off + udta.size, meta, ::metaOut)
                if (meta == null) metaOut()
            } else metaOut()
        }
        fun chunks(o: Long, sz: Long, wide: Boolean) {
            if (sz < 16) throw TagException("bad chunk offset box")
            val h = w.read(o, 16)
            val cnt = be(h, 12, 4)
            val es = if (wide) 8 else 4
            if (16 + cnt * es != sz) throw Unsupported("unusual chunk offset box")
            w.put(h)
            var p = o + 16; var left = cnt * es
            while (left > 0) {
                val k = minOf(left, 16384L).toInt()
                val b = w.read(p, k)
                var i = 0
                while (i < k) {
                    var v = be(b, i, es)
                    if (v >= me) {
                        v += delta
                        if (!wide && v > 0xFFFFFFFFL) throw Unsupported("chunk offsets would overflow")
                    }
                    for (j in 0 until es) b[i + j] = (v ushr (8 * (es - 1 - j))).toByte()
                    i += es
                }
                w.put(b)
                p += k; left -= k
            }
        }
        fun tree(from: Long, end: Long) {
            var o = from
            while (o < end) {
                val b = boxAt(w, o, end, false) ?: throw TagException("bad MP4 box at $o")
                when (b.type) {
                    "trak", "mdia", "minf", "stbl" -> {
                        if (be(w.read(o, 4), 0, 4) != b.size) throw Unsupported("64-bit box")
                        w.copy(o, 8); tree(o + 8, o + b.size)
                    }
                    "stco", "co64" -> chunks(o, b.size, b.type == "co64")
                    else -> w.copy(o, b.size)
                }
                o += b.size
            }
        }

        w.copy(0, mo)
        boxHdr(w, moovN, "moov")
        var o = mo + 8
        while (o < me) {
            val b = boxAt(w, o, me, true) ?: throw TagException("bad MP4 box at $o")
            when {
                udta != null && o == udta.off -> udtaOut()
                b.type == "trak" -> tree(o, o + b.size)
                else -> w.copy(o, b.size)
            }
            o += b.size
        }
        if (udta == null) udtaOut()
        w.copy(me, fsize - me)
    }
}
