package org.hibyos.install.core.library

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

/** What the library needs to know about one audio file. Audio files are only ever read. */
class AudioInfo(var codec: String) {
    var title: String? = null
    var artist: String? = null
    var album: String? = null
    var albumArtist: String? = null
    var genre: String? = null
    var date: String? = null
    var track: String? = null
    var disc: String? = null
    var sampleRate = 0
    var bitsPerSample = 0
    var channels = 0
    /** Average bit rate in bits per second (0 when unknown). */
    var bitrate = 0L
    var durationMs = 0L
    var lossless = false
    var mqa = false
    var hasPicture = false
    /**
     * Where the embedded front cover's image bytes are (the front cover, else the first
     * picture), when they can be read straight from the file: null for an ID3 tag with
     * unsynchronisation, a compressed frame, or no picture at all.
     */
    var picture: PictureRef? = null
    /** Other text tags, upper-case keys (Vorbis comment names, TXXX descriptions, MP4 freeform names). */
    val extra = LinkedHashMap<String, String>()
}

/** An embedded picture: [length] image bytes at [offset] in the file; [type] is the ID3/FLAC picture type (3 = front cover). */
class PictureRef(val offset: Long, val length: Long, val mime: String, val type: Int)

open class TagException(message: String) : IOException(message)

/** Random access to the bytes of an audio file, for [TagReader]. */
interface TagSource {
    fun length(): Long
    /** Fills [b] with the bytes at [pos] (the caller keeps pos + b.size within length()). */
    fun readAt(pos: Long, b: ByteArray)
}

class FileTagSource(private val f: RandomAccessFile) : TagSource {
    override fun length(): Long = f.length()
    override fun readAt(pos: Long, b: ByteArray) { f.seek(pos); f.readFully(b) }
}

/**
 * A file read in blocks through [fetch] (offset, length) -> bytes, for instance partial
 * downloads from the HiBy: only the blocks the tag reader touches are fetched, each once.
 */
class RangeTagSource(private val size: Long, private val block: Int = 64 * 1024,
                     private val fetch: (Long, Int) -> ByteArray) : TagSource {
    private val blocks = HashMap<Long, ByteArray>()
    /** Bytes fetched so far. */
    var fetched = 0L
        private set
    /** Number of fetch calls so far. */
    var requests = 0
        private set

    override fun length(): Long = size

    /** Prefetches [len] bytes from [pos] in one request (whole blocks), so later reads there are free. */
    fun prefetch(pos: Long, len: Long) {
        if (len <= 0 || pos >= size) return
        val first = pos / block
        val last = (minOf(size, pos + len) - 1) / block
        var b = first
        while (b <= last) {
            if (blocks.containsKey(b)) { b++; continue }
            var e = b
            while (e + 1 <= last && !blocks.containsKey(e + 1)) e++
            val start = b * block
            val n = (minOf(size, (e + 1) * block) - start).toInt()
            val got = get(start, n)
            for (k in b..e) {
                val o = ((k - b) * block).toInt()
                blocks[k] = got.copyOfRange(o, minOf(got.size, o + block))
            }
            b = e + 1
        }
    }

    private fun get(pos: Long, n: Int): ByteArray {
        requests++
        val got = fetch(pos, n)
        if (got.size != n) throw TagException("short read at $pos ($n bytes asked, ${got.size} received)")
        fetched += got.size
        return got
    }

    override fun readAt(pos: Long, b: ByteArray) {
        if (b.isEmpty()) return
        prefetch(pos, b.size.toLong())
        var done = 0
        while (done < b.size) {
            val p = pos + done
            val blk = blocks[p / block]!!
            val o = (p % block).toInt()
            val n = minOf(blk.size - o, b.size - done)
            System.arraycopy(blk, o, b, done, n)
            done += n
        }
    }
}

/**
 * A small tag and stream-info reader written for ByOS from the public format
 * specifications: ID3v2.2/2.3/2.4 and ID3v1 with the MPEG audio frame header (and Xing/Info
 * or VBRI for VBR length), FLAC (STREAMINFO, VORBIS_COMMENT, PICTURE), MP4/M4A (mdhd, stsd
 * mp4a/alac, esds, the iTunes ilst atoms) and the stream headers of WAV and DSF. No GPL
 * library is used.
 */
object TagReader {
    private const val MAX_TAG = 16 shl 20

    fun read(file: File): AudioInfo = RandomAccessFile(file, "r").use { read(FileTagSource(it), file.name) }

    /** Reads any [TagSource]; [name] (the file name) only helps to recognise MP3 files without an ID3 tag. */
    fun read(f: TagSource, name: String): AudioInfo {
        run {
            val head = bytesAt(f, 0, minOf(16L, f.length()).toInt())
            val ext = name.substringAfterLast('.', "").lowercase()
            return when {
                head.size >= 4 && String(head, 0, 4, Charsets.ISO_8859_1) == "fLaC" -> flac(f, 0)
                head.size >= 12 && String(head, 4, 4, Charsets.ISO_8859_1) == "ftyp" -> mp4(f)
                head.size >= 12 && String(head, 0, 4, Charsets.ISO_8859_1) == "RIFF" && String(head, 8, 4, Charsets.ISO_8859_1) == "WAVE" -> wav(f)
                head.size >= 4 && String(head, 0, 4, Charsets.ISO_8859_1) == "DSD " -> dsf(f)
                head.size >= 3 && String(head, 0, 3, Charsets.ISO_8859_1) == "ID3" -> {
                    // FLAC files sometimes carry an ID3v2 tag in front
                    val tagEnd = id3End(head)
                    if (tagEnd + 4 <= f.length()) {
                        val m = bytesAt(f, tagEnd, 4)
                        if (String(m, Charsets.ISO_8859_1) == "fLaC") return flac(f, tagEnd)
                    }
                    mp3(f)
                }
                ext == "mp3" || ext == "mp2" -> mp3(f)
                else -> throw TagException("unsupported file format")
            }
        }
    }

    // ------------------------------------------------------------------ helpers

    private fun be(b: ByteArray, p: Int, n: Int): Long { var v = 0L; for (i in 0 until n) v = (v shl 8) or (b[p + i].toLong() and 0xff); return v }
    private fun le(b: ByteArray, p: Int, n: Int): Long { var v = 0L; for (i in n - 1 downTo 0) v = (v shl 8) or (b[p + i].toLong() and 0xff); return v }
    private fun syncsafe(b: ByteArray, p: Int): Int =
        ((b[p].toInt() and 0x7f) shl 21) or ((b[p + 1].toInt() and 0x7f) shl 14) or ((b[p + 2].toInt() and 0x7f) shl 7) or (b[p + 3].toInt() and 0x7f)

    private fun bytesAt(f: TagSource, pos: Long, n: Int): ByteArray {
        if (n < 0 || pos < 0 || pos + n > f.length()) throw TagException("truncated file")
        val b = ByteArray(n); f.readAt(pos, b); return b
    }

    private fun clean(s: String?): String? = s?.replace("\u0000", "")?.trim()?.ifEmpty { null }

    /** The value a player shows for "track" style fields: the number before any "/". */
    fun leadingInt(s: String?): Int {
        if (s == null) return 0
        var i = 0
        while (i < s.length && s[i] == ' ') i++
        var n = 0
        var k = 0
        while (i < s.length && s[i].isDigit() && k < 9) { n = n * 10 + (s[i] - '0'); i++; k++ }
        return n
    }

    private fun mqaFromTags(info: AudioInfo) {
        val e = info.extra
        if (e.containsKey("MQAENCODER") || e.containsKey("MQASAMPLERATE") || e.containsKey("ORIGINALSAMPLERATE") ||
            (e["ENCODER"]?.contains("MQA", ignoreCase = true) == true) || (e["ENCODED-BY"]?.contains("MQA", ignoreCase = true) == true)) {
            info.mqa = true
        }
    }

    private fun applyVorbisLike(info: AudioInfo, key: String, value: String) {
        val k = key.uppercase()
        val v = clean(value) ?: return
        when (k) {
            "TITLE" -> if (info.title == null) info.title = v
            "ARTIST" -> if (info.artist == null) info.artist = v
            "ALBUM" -> if (info.album == null) info.album = v
            "ALBUMARTIST", "ALBUM ARTIST", "ALBUM_ARTIST" -> if (info.albumArtist == null) info.albumArtist = v
            "GENRE" -> if (info.genre == null) info.genre = v
            "DATE", "YEAR", "ORIGINALDATE" -> if (info.date == null) info.date = v
            "TRACKNUMBER", "TRACK" -> if (info.track == null) info.track = v
            "DISCNUMBER", "DISC" -> if (info.disc == null) info.disc = v
        }
        if (!info.extra.containsKey(k)) info.extra[k] = v
    }

    // ------------------------------------------------------------------ FLAC

    private fun flac(f: TagSource, start: Long): AudioInfo {
        val info = AudioInfo("flac")
        info.lossless = true
        var pos = start + 4
        var totalSamples = 0L
        while (true) {
            val h = bytesAt(f, pos, 4)
            val last = h[0].toInt() and 0x80 != 0
            val type = h[0].toInt() and 0x7f
            val len = be(h, 1, 3).toInt()
            pos += 4
            when (type) {
                0 -> {
                    val b = bytesAt(f, pos, 34)
                    info.sampleRate = (be(b, 10, 3) ushr 4).toInt()
                    info.channels = ((b[12].toInt() shr 1) and 7) + 1
                    info.bitsPerSample = (((b[12].toInt() and 1) shl 4) or ((b[13].toInt() and 0xff) ushr 4)) + 1
                    totalSamples = ((b[13].toLong() and 0x0f) shl 32) or be(b, 14, 4)
                }
                4 -> if (len <= MAX_TAG) vorbisComment(info, bytesAt(f, pos, len), 0)
                6 -> {
                    info.hasPicture = true
                    flacPicture(f, pos, len)?.let { p -> if (info.picture == null || (p.type == 3 && info.picture!!.type != 3)) info.picture = p }
                }
            }
            pos += len
            if (last || type == 127) break
            if (pos >= f.length()) throw TagException("FLAC metadata runs past the end of the file")
        }
        if (info.sampleRate > 0 && totalSamples > 0) {
            info.durationMs = totalSamples * 1000 / info.sampleRate
            val audio = f.length() - pos
            if (info.durationMs > 0) info.bitrate = audio * 8 * 1000 / info.durationMs
        }
        mqaFromTags(info)
        return info
    }

    /** A FLAC PICTURE block at [pos] ([len] bytes): type, MIME, description, size fields, then the image. */
    private fun flacPicture(f: TagSource, pos: Long, len: Int): PictureRef? {
        if (len < 32) return null
        val h = bytesAt(f, pos, 8)
        val type = be(h, 0, 4).toInt()
        val mimeLen = be(h, 4, 4)
        if (mimeLen > 256 || 8 + mimeLen + 4 > len) return null
        val mime = String(bytesAt(f, pos + 8, mimeLen.toInt()), Charsets.ISO_8859_1)
        val descLen = be(bytesAt(f, pos + 8 + mimeLen, 4), 0, 4)
        val fixed = pos + 8 + mimeLen + 4 + descLen
        if (8 + mimeLen + 4 + descLen + 20 > len) return null
        val dataLen = be(bytesAt(f, fixed + 16, 4), 0, 4)
        if (dataLen <= 0 || fixed + 20 + dataLen > pos + len) return null
        return PictureRef(fixed + 20, dataLen, mime.lowercase(), type)
    }

    private fun vorbisComment(info: AudioInfo, b: ByteArray, start: Int) {
        var p = start
        fun u32(): Int { if (p + 4 > b.size) throw TagException("bad Vorbis comment"); val v = le(b, p, 4); p += 4; return if (v > Int.MAX_VALUE) -1 else v.toInt() }
        val vendorLen = u32()
        if (vendorLen < 0 || p + vendorLen > b.size) throw TagException("bad Vorbis comment")
        p += vendorLen
        val n = u32()
        for (i in 0 until n) {
            val l = u32()
            if (l < 0 || p + l > b.size) throw TagException("bad Vorbis comment")
            val s = String(b, p, l, Charsets.UTF_8)
            p += l
            val eq = s.indexOf('=')
            if (eq > 0) applyVorbisLike(info, s.substring(0, eq), s.substring(eq + 1))
        }
    }

    // ------------------------------------------------------------------ ID3v2 and MPEG

    private fun id3End(head: ByteArray): Long {
        val size = syncsafe(head, 6)
        val footer = if (head[3].toInt() == 4 && head[5].toInt() and 0x10 != 0) 10 else 0
        return 10L + size + footer
    }

    private val GENRES = listOf("Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz",
        "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative",
        "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
        "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass",
        "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
        "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
        "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave",
        "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock")

    /** "(13)", "13", "(13)Pop" -> genre names as players show them. */
    fun genreName(s: String): String {
        val m = Regex("^\\((\\d+)\\)(.*)$").find(s)
        if (m != null) {
            val rest = m.groupValues[2].trim()
            if (rest.isNotEmpty()) return rest
            return GENRES.getOrNull(m.groupValues[1].toInt()) ?: s
        }
        if (s.all { it.isDigit() } && s.length <= 3) return GENRES.getOrNull(s.toInt()) ?: s
        return s
    }

    private fun unsync(b: ByteArray): ByteArray {
        val out = java.io.ByteArrayOutputStream(b.size)
        var i = 0
        while (i < b.size) {
            out.write(b[i].toInt())
            if (b[i] == 0xff.toByte() && i + 1 < b.size && b[i + 1].toInt() == 0) i++
            i++
        }
        return out.toByteArray()
    }

    private fun id3Text(b: ByteArray, off: Int, len: Int): List<String> {
        if (len < 1) return emptyList()
        val enc = b[off].toInt()
        val data = b.copyOfRange(off + 1, off + len)
        val s = when (enc) {
            0 -> String(data, Charsets.ISO_8859_1)
            1 -> String(data, Charsets.UTF_16)
            2 -> String(data, Charsets.UTF_16BE)
            3 -> String(data, Charsets.UTF_8)
            else -> String(data, Charsets.ISO_8859_1)
        }
        return s.split('\u0000').map { it.trim().removePrefix("﻿") }.filter { it.isNotEmpty() }
    }

    private fun id3(info: AudioInfo, f: TagSource): Long {
        val h = bytesAt(f, 0, 10)
        val ver = h[3].toInt()
        val flags = h[5].toInt()
        val size = syncsafe(h, 6)
        val end = id3End(h)
        if (ver !in 2..4 || size > MAX_TAG) return end
        val tagLen = minOf(size.toLong(), f.length() - 10).toInt()
        // Without tag-wide unsynchronisation the frames are read one by one straight from the
        // file, so a picture is located but never read (a partial download stays small).
        val whole: ByteArray? = if (flags and 0x80 != 0 && ver < 4) unsync(bytesAt(f, 10, tagLen)) else null
        val limit = whole?.size ?: tagLen
        fun at(p: Int, n: Int): ByteArray = whole?.copyOfRange(p, p + n) ?: bytesAt(f, 10L + p, n)
        var p = 0
        if (flags and 0x40 != 0 && ver >= 3 && limit >= 4) {
            val x = at(0, 4)
            p = if (ver == 3) 4 + be(x, 0, 4).toInt() else syncsafe(x, 0)
        }
        val idLen = if (ver == 2) 3 else 4
        val hdrLen = if (ver == 2) 6 else 10
        while (p >= 0 && p + hdrLen <= limit) {
            val fh = at(p, hdrLen)
            val id = String(fh, 0, idLen, Charsets.ISO_8859_1)
            if (id[0] == '\u0000') break
            val fsize = when (ver) {
                2 -> be(fh, 3, 3).toInt()
                3 -> be(fh, 4, 4).toInt()
                else -> syncsafe(fh, 4)
            }
            val fflags = if (ver == 2) 0 else be(fh, 8, 2).toInt()
            p += hdrLen
            if (fsize < 0 || p + fsize > limit) break
            val dataPos = p
            p += fsize
            // compressed or encrypted frames are skipped
            if (ver == 3 && fflags and 0xc0 != 0) continue
            if (ver == 4 && fflags and 0x000c != 0) continue
            val isPic = id == "APIC" || id == "PIC"
            if (isPic) info.hasPicture = true
            if (!isPic && fsize > (1 shl 20)) continue
            var data = at(dataPos, if (isPic) minOf(fsize, 1024) else fsize)
            var dp = 0
            if (ver == 4) {
                if (fflags and 0x0040 != 0) dp += 1      // grouping identity
                if (fflags and 0x0001 != 0) dp += 4      // data length indicator
                data = data.copyOfRange(minOf(dp, data.size), data.size)
            }
            val frameUnsync = ver == 4 && (fflags and 0x0002 != 0 || flags and 0x80 != 0)
            if (frameUnsync) data = unsync(data)
            if (isPic) {
                if (whole == null && !frameUnsync) id3Picture(data, ver, 10L + dataPos + dp, fsize - dp)?.let { pic ->
                    if (info.picture == null || (pic.type == 3 && info.picture!!.type != 3)) info.picture = pic
                }
                continue
            }
            fun first() = id3Text(data, 0, data.size).firstOrNull()
            when (id) {
                "TIT2", "TT2" -> info.title = info.title ?: first()
                "TPE1", "TP1" -> info.artist = info.artist ?: first()
                "TALB", "TAL" -> info.album = info.album ?: first()
                "TPE2", "TP2" -> info.albumArtist = info.albumArtist ?: first()
                "TCON", "TCO" -> info.genre = info.genre ?: first()?.let { genreName(it) }
                "TDRC", "TYER", "TYE", "TDOR", "TORY" -> info.date = info.date ?: first()
                "TRCK", "TRK" -> info.track = info.track ?: first()
                "TPOS", "TPA" -> info.disc = info.disc ?: first()
                "TENC", "TEN" -> first()?.let { info.extra.putIfAbsent("ENCODED-BY", it) }
                "TSSE", "TSS" -> first()?.let { info.extra.putIfAbsent("ENCODER", it) }
                "TXXX", "TXX" -> {
                    val parts = id3Text(data, 0, data.size)
                    if (parts.size >= 2) applyVorbisLike(info, parts[0], parts[1])
                }
            }
        }
        return end
    }

    /**
     * The image inside an APIC (v2.3/2.4: encoding, MIME, NUL, type, description) or PIC
     * (v2.2: encoding, 3-letter format, type, description) frame whose first bytes are [d];
     * [dataPos] is where [d] starts in the file and [len] the frame's data length.
     */
    private fun id3Picture(d: ByteArray, ver: Int, dataPos: Long, len: Int): PictureRef? {
        if (d.size < 4) return null
        val enc = d[0].toInt()
        var p: Int
        val mime: String
        if (ver == 2) {
            val fmt = String(d, 1, 3, Charsets.ISO_8859_1).lowercase()
            mime = if (fmt == "png") "image/png" else "image/jpeg"
            p = 4
        } else {
            val z = (1 until d.size).firstOrNull { d[it].toInt() == 0 } ?: return null
            val m = String(d, 1, z - 1, Charsets.ISO_8859_1).lowercase()
            mime = when (m) { "", "jpg", "jpeg" -> "image/jpeg"; "png" -> "image/png"; else -> m }
            p = z + 1
        }
        if (p >= d.size) return null
        val type = d[p].toInt() and 0xff
        p++
        // the description ends with one NUL (Latin-1, UTF-8) or two aligned NULs (UTF-16)
        if (enc == 1 || enc == 2) {
            while (p + 1 < d.size && !(d[p].toInt() == 0 && d[p + 1].toInt() == 0)) p += 2
            p += 2
        } else {
            while (p < d.size && d[p].toInt() != 0) p++
            p += 1
        }
        if (p > d.size || p >= len) return null
        return PictureRef(dataPos + p, (len - p).toLong(), mime, type)
    }

    private val BITRATES = arrayOf(
        // MPEG-1 layer I, II, III
        intArrayOf(0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448),
        intArrayOf(0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384),
        intArrayOf(0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
        // MPEG-2/2.5 layer I, II/III
        intArrayOf(0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256),
        intArrayOf(0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
    )

    private class MpegHeader(val mpeg: Int, val layer: Int, val bitrate: Int, val sampleRate: Int, val channels: Int,
                             val frameLen: Int, val samples: Int, val mono: Boolean)

    private fun mpegHeader(b: ByteArray, p: Int): MpegHeader? {
        if (p + 4 > b.size) return null
        val h = be(b, p, 4).toInt()
        if (h ushr 21 != 0x7ff) return null
        val verBits = (h ushr 19) and 3
        val layerBits = (h ushr 17) and 3
        val brIdx = (h ushr 12) and 15
        val srIdx = (h ushr 10) and 3
        if (verBits == 1 || layerBits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3) return null
        val mpeg = when (verBits) { 3 -> 1; 2 -> 2; else -> 25 }
        val layer = 4 - layerBits
        val table = if (mpeg == 1) layer - 1 else if (layer == 1) 3 else 4
        val br = BITRATES[table][brIdx] * 1000
        val base = intArrayOf(44100, 48000, 32000)[srIdx]
        val sr = when (mpeg) { 1 -> base; 2 -> base / 2; else -> base / 4 }
        val pad = (h ushr 9) and 1
        val mono = ((h ushr 6) and 3) == 3
        val samples = when { layer == 1 -> 384; layer == 2 || mpeg == 1 -> 1152; else -> 576 }
        val len = if (layer == 1) (12 * br / sr + pad) * 4 else samples / 8 * br / sr + pad
        if (len < 4) return null
        return MpegHeader(mpeg, layer, br, sr, if (mono) 1 else 2, len, samples, mono)
    }

    private fun mp3(f: TagSource): AudioInfo {
        val info = AudioInfo("mp3")
        var start = 0L
        val head = bytesAt(f, 0, minOf(10L, f.length()).toInt())
        if (head.size == 10 && String(head, 0, 3, Charsets.ISO_8859_1) == "ID3") start = id3(info, f)
        // ID3v1 at the end fills what v2 did not give
        var audioEnd = f.length()
        if (f.length() >= 128) {
            val t = bytesAt(f, f.length() - 128, 128)
            if (String(t, 0, 3, Charsets.ISO_8859_1) == "TAG") {
                audioEnd -= 128
                fun s(o: Int, n: Int) = clean(String(t, o, n, Charsets.ISO_8859_1))
                info.title = info.title ?: s(3, 30)
                info.artist = info.artist ?: s(33, 30)
                info.album = info.album ?: s(63, 30)
                info.date = info.date ?: s(93, 4)
                if (t[125].toInt() == 0 && t[126].toInt() != 0) info.track = info.track ?: (t[126].toInt() and 0xff).toString()
                if (info.genre == null) GENRES.getOrNull(t[127].toInt() and 0xff)?.let { info.genre = it }
            }
        }
        // first MPEG frame whose successor also looks right
        val scanLen = minOf(audioEnd - start, 256L * 1024).toInt().coerceAtLeast(0)
        val buf = bytesAt(f, start, scanLen)
        var p = 0
        var hdr: MpegHeader? = null
        while (p + 4 <= buf.size) {
            val h = mpegHeader(buf, p)
            if (h != null) {
                val n = mpegHeader(buf, p + h.frameLen)
                if (n != null && n.sampleRate == h.sampleRate && n.layer == h.layer || p + h.frameLen >= buf.size) { hdr = h; break }
            }
            p++
        }
        if (hdr == null) throw TagException("no MPEG audio frame found")
        info.codec = if (hdr.layer == 2) "mp2" else "mp3"
        info.sampleRate = hdr.sampleRate
        info.channels = hdr.channels
        info.bitsPerSample = 16
        val audioStart = start + p
        // Xing / Info (side info length depends on version and channels) or VBRI
        val side = if (hdr.mpeg == 1) (if (hdr.mono) 17 else 32) else (if (hdr.mono) 9 else 17)
        var frames = 0L
        val x = p + 4 + side
        if (x + 12 <= buf.size) {
            val tag = String(buf, x, 4, Charsets.ISO_8859_1)
            if ((tag == "Xing" || tag == "Info") && (buf[x + 7].toInt() and 1) != 0) frames = be(buf, x + 8, 4)
        }
        val v = p + 4 + 32
        if (frames == 0L && v + 18 <= buf.size && String(buf, v, 4, Charsets.ISO_8859_1) == "VBRI") frames = be(buf, v + 14, 4)
        val audioBytes = audioEnd - audioStart
        if (frames > 0) {
            info.durationMs = frames * hdr.samples * 1000 / hdr.sampleRate
            if (info.durationMs > 0) info.bitrate = audioBytes * 8 * 1000 / info.durationMs
        } else {
            info.bitrate = hdr.bitrate.toLong()
            if (hdr.bitrate > 0) info.durationMs = audioBytes * 8 * 1000 / hdr.bitrate
        }
        mqaFromTags(info)
        return info
    }

    // ------------------------------------------------------------------ MP4 / M4A

    private class Atom(val type: String, val start: Long, val size: Long, val header: Int) {
        val body get() = start + header
        val end get() = start + size
    }

    private fun atoms(f: TagSource, from: Long, to: Long): List<Atom> {
        val out = ArrayList<Atom>()
        var p = from
        while (p + 8 <= to) {
            val h = bytesAt(f, p, 8)
            var size = be(h, 0, 4)
            val type = String(h, 4, 4, Charsets.ISO_8859_1)
            var hl = 8
            if (size == 1L) { size = be(bytesAt(f, p + 8, 8), 0, 8); hl = 16 }
            if (size == 0L) size = to - p
            if (size < hl || p + size > to) break
            out += Atom(type, p, size, hl)
            p += size
        }
        return out
    }

    private fun child(f: TagSource, a: Atom?, vararg path: String, skip: Int = 0): Atom? {
        var cur = a ?: return null
        for ((i, t) in path.withIndex()) {
            val off = if (i == 0) skip else 0
            cur = atoms(f, cur.body + off, cur.end).firstOrNull { it.type == t } ?: return null
        }
        return cur
    }

    private fun mp4(f: TagSource): AudioInfo {
        val top = atoms(f, 0, f.length())
        val moov = top.firstOrNull { it.type == "moov" } ?: throw TagException("MP4 without moov")
        if (moov.size > 64L shl 20) throw TagException("MP4 moov too large")
        val info = AudioInfo("aac")
        // the first audio track
        for (trak in atoms(f, moov.body, moov.end).filter { it.type == "trak" }) {
            val mdia = child(f, trak, "mdia") ?: continue
            val hdlr = child(f, mdia, "hdlr")
            if (hdlr != null && String(bytesAt(f, hdlr.body + 8, 4), Charsets.ISO_8859_1) != "soun") continue
            child(f, mdia, "mdhd")?.let { m ->
                val b = bytesAt(f, m.body, minOf(m.size - m.header, 40L).toInt())
                val (ts, dur) = if (b[0].toInt() == 1) be(b, 20, 4) to be(b, 24, 8) else be(b, 12, 4) to be(b, 16, 4)
                if (ts > 0) info.durationMs = dur * 1000 / ts
            }
            val stsd = child(f, mdia, "minf", "stbl", "stsd") ?: continue
            val entries = atoms(f, stsd.body + 8, stsd.end)
            val e = entries.firstOrNull() ?: continue
            val b = bytesAt(f, e.body, minOf(e.size - e.header, 4096L).toInt())
            if (b.size >= 28) {
                info.channels = be(b, 16, 2).toInt()
                info.bitsPerSample = be(b, 18, 2).toInt()
                info.sampleRate = be(b, 24, 2).toInt()
            }
            when (e.type) {
                "alac" -> {
                    info.codec = "alac"; info.lossless = true
                    val alac = atoms(f, e.body + 28, e.end).firstOrNull { it.type == "alac" }
                    if (alac != null && alac.size >= 36) {
                        val a = bytesAt(f, alac.body, 28)
                        info.bitsPerSample = a[9].toInt() and 0xff
                        info.channels = a[13].toInt() and 0xff
                        info.sampleRate = be(a, 24, 4).toInt()
                    }
                }
                "mp4a" -> {
                    info.codec = "aac"
                    val esds = atoms(f, e.body + 28, e.end).firstOrNull { it.type == "esds" }
                    if (esds != null) esdsBitrate(bytesAt(f, esds.body, (esds.size - esds.header).toInt()))?.let { info.bitrate = it }
                }
                else -> info.codec = e.type.trim()
            }
            break
        }
        if (info.bitrate == 0L && info.durationMs > 0) {
            val mdat = top.filter { it.type == "mdat" }.sumOf { it.size - it.header }
            info.bitrate = mdat * 8 * 1000 / info.durationMs
        }
        // tags: moov/udta/meta/ilst (meta is a full box: 4 bytes before its children)
        val meta = child(f, moov, "udta", "meta")
        val ilst = meta?.let { atoms(f, it.body + 4, it.end).firstOrNull { a -> a.type == "ilst" } }
        if (ilst != null) {
            for (item in atoms(f, ilst.body, ilst.end)) {
                if (item.size > 64L shl 20) continue
                val parts = atoms(f, item.body, item.end)
                val data = parts.firstOrNull { it.type == "data" } ?: continue
                if (item.type == "covr") {
                    info.hasPicture = true
                    if (info.picture == null && data.size - data.header > 8) {
                        val kind = be(bytesAt(f, data.body, 4), 1, 3).toInt()
                        info.picture = PictureRef(data.body + 8, data.size - data.header - 8, if (kind == 14) "image/png" else "image/jpeg", 3)
                    }
                    continue
                }
                if (data.size - data.header > 1 shl 20) continue
                val raw = bytesAt(f, data.body, (data.size - data.header).toInt())
                if (raw.size < 8) continue
                val kind = be(raw, 1, 3).toInt()
                val payload = raw.copyOfRange(8, raw.size)
                val text = if (kind == 1) String(payload, Charsets.UTF_8) else null
                when (item.type) {
                    "©nam" -> info.title = info.title ?: clean(text)
                    "©ART" -> info.artist = info.artist ?: clean(text)
                    "©alb" -> info.album = info.album ?: clean(text)
                    "aART" -> info.albumArtist = info.albumArtist ?: clean(text)
                    "©gen" -> info.genre = info.genre ?: clean(text)
                    "gnre" -> if (info.genre == null && payload.size >= 2) GENRES.getOrNull(be(payload, 0, 2).toInt() - 1)?.let { info.genre = it }
                    "©day" -> info.date = info.date ?: clean(text)
                    "©too" -> clean(text)?.let { info.extra.putIfAbsent("ENCODER", it) }
                    "trkn" -> if (payload.size >= 4) be(payload, 2, 2).toInt().takeIf { it > 0 }?.let { info.track = info.track ?: it.toString() }
                    "disk" -> if (payload.size >= 4) be(payload, 2, 2).toInt().takeIf { it > 0 }?.let { info.disc = info.disc ?: it.toString() }
                    "----" -> {
                        val nameAtom = parts.firstOrNull { it.type == "name" } ?: continue
                        val nb = bytesAt(f, nameAtom.body + 4, (nameAtom.size - nameAtom.header - 4).toInt())
                        if (text != null) applyVorbisLike(info, String(nb, Charsets.UTF_8), text)
                    }
                }
            }
        }
        mqaFromTags(info)
        return info
    }

    /** avgBitrate from the DecoderConfigDescriptor in an esds box. */
    private fun esdsBitrate(b: ByteArray): Long? {
        var p = 4 // version and flags
        fun len(): Int { var n = 0; for (i in 0 until 4) { if (p >= b.size) return -1; val x = b[p++].toInt() and 0xff; n = (n shl 7) or (x and 0x7f); if (x and 0x80 == 0) break }; return n }
        while (p < b.size) {
            val tag = b[p++].toInt() and 0xff
            val l = len()
            if (l < 0) return null
            when (tag) {
                3 -> { p += 3; val fl = b.getOrNull(p - 1)?.toInt() ?: 0; if (fl and 0x80 != 0) p += 2; if (fl and 0x40 != 0) p += 1 + (b.getOrNull(p)?.toInt() ?: 0); if (fl and 0x20 != 0) p += 2 }
                4 -> { if (p + 13 > b.size) return null; val avg = be(b, p + 9, 4); return if (avg > 0) avg else be(b, p + 5, 4).takeIf { it > 0 } }
                else -> p += l
            }
        }
        return null
    }

    // ------------------------------------------------------------------ WAV and DSF (stream info only)

    private fun wav(f: TagSource): AudioInfo {
        val info = AudioInfo("wav")
        info.lossless = true
        var p = 12L
        var dataLen = 0L
        var byteRate = 0L
        while (p + 8 <= f.length()) {
            val h = bytesAt(f, p, 8)
            val id = String(h, 0, 4, Charsets.ISO_8859_1)
            val len = le(h, 4, 4)
            if (id == "fmt " && len >= 16) {
                val b = bytesAt(f, p + 8, 16)
                info.channels = le(b, 2, 2).toInt()
                info.sampleRate = le(b, 4, 4).toInt()
                byteRate = le(b, 8, 4)
                info.bitsPerSample = le(b, 14, 2).toInt()
            } else if (id == "data") {
                dataLen = minOf(len, f.length() - p - 8)
            }
            p += 8 + len + (len and 1)
        }
        if (byteRate > 0) {
            info.bitrate = byteRate * 8
            info.durationMs = dataLen * 1000 / byteRate
        }
        return info
    }

    private fun dsf(f: TagSource): AudioInfo {
        val info = AudioInfo("dsd")
        info.lossless = true
        val b = bytesAt(f, 28, 52)
        if (String(b, 0, 4, Charsets.ISO_8859_1) != "fmt ") throw TagException("bad DSF header")
        info.channels = le(b, 24, 4).toInt()
        info.sampleRate = le(b, 28, 4).toInt()
        info.bitsPerSample = le(b, 32, 4).toInt()
        val samples = le(b, 36, 8)
        if (info.sampleRate > 0) info.durationMs = samples * 1000 / info.sampleRate
        info.bitrate = info.sampleRate.toLong() * info.channels
        return info
    }
}
