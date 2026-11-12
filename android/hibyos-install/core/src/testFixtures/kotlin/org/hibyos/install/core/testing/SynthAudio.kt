package org.hibyos.install.core.testing

import java.io.ByteArrayOutputStream
import java.io.File

/**
 * Small synthetic audio files for tests, built byte by byte from the format specifications.
 * They are not playable music: the audio part is filler of the right shape (MPEG frame
 * headers, a FLAC body, an mdat box).
 */
object SynthAudio {
    class Tags(
        val title: String? = null, val artist: String? = null, val album: String? = null, val albumArtist: String? = null,
        val genre: String? = null, val date: String? = null, val track: String? = null, val disc: String? = null,
        val extra: Map<String, String> = emptyMap(), val picture: Boolean = false,
    )

    private fun ByteArrayOutputStream.be(v: Long, n: Int) { for (i in n - 1 downTo 0) write(((v ushr (8 * i)) and 0xff).toInt()) }
    private fun ByteArrayOutputStream.le(v: Long, n: Int) { for (i in 0 until n) write(((v ushr (8 * i)) and 0xff).toInt()) }
    private fun ByteArrayOutputStream.bytes(b: ByteArray) = write(b, 0, b.size)
    private fun ByteArrayOutputStream.ascii(s: String) = bytes(s.toByteArray(Charsets.ISO_8859_1))

    // ------------------------------------------------------------------ FLAC

    fun flac(t: Tags, sampleRate: Int = 44100, bits: Int = 16, channels: Int = 2, seconds: Int = 10, audioBytes: Int = 20000): ByteArray {
        val o = ByteArrayOutputStream()
        o.ascii("fLaC")
        // STREAMINFO
        val si = ByteArrayOutputStream()
        si.be(4096, 2); si.be(4096, 2); si.be(0, 3); si.be(0, 3)
        val total = sampleRate.toLong() * seconds
        // 20 bits rate, 3 bits channels-1, 5 bits bps-1, 36 bits samples
        val packed = (sampleRate.toLong() shl 44) or ((channels - 1).toLong() shl 41) or ((bits - 1).toLong() shl 36) or total
        si.be(packed, 8)
        si.bytes(ByteArray(16)) // MD5
        val blocks = ArrayList<Pair<Int, ByteArray>>()
        blocks += 0 to si.toByteArray()
        val vc = ByteArrayOutputStream()
        val vendor = "synthetic".toByteArray()
        vc.le(vendor.size.toLong(), 4); vc.bytes(vendor)
        val comments = ArrayList<String>()
        t.title?.let { comments += "TITLE=$it" }
        t.artist?.let { comments += "ARTIST=$it" }
        t.album?.let { comments += "ALBUM=$it" }
        t.albumArtist?.let { comments += "ALBUMARTIST=$it" }
        t.genre?.let { comments += "GENRE=$it" }
        t.date?.let { comments += "DATE=$it" }
        t.track?.let { comments += "TRACKNUMBER=$it" }
        t.disc?.let { comments += "DISCNUMBER=$it" }
        for ((k, v) in t.extra) comments += "$k=$v"
        vc.le(comments.size.toLong(), 4)
        for (c in comments) { val b = c.toByteArray(); vc.le(b.size.toLong(), 4); vc.bytes(b) }
        blocks += 4 to vc.toByteArray()
        if (t.picture) {
            val pic = ByteArrayOutputStream()
            pic.be(3, 4); val mime = "image/jpeg".toByteArray(); pic.be(mime.size.toLong(), 4); pic.bytes(mime)
            pic.be(0, 4); pic.be(1, 4); pic.be(1, 4); pic.be(24, 4); pic.be(0, 4)
            val img = byteArrayOf(0xff.toByte(), 0xd8.toByte(), 0xff.toByte(), 0xd9.toByte())
            pic.be(img.size.toLong(), 4); pic.bytes(img)
            blocks += 6 to pic.toByteArray()
        }
        blocks += 1 to ByteArray(100) // padding
        for ((i, b) in blocks.withIndex()) {
            o.write((if (i == blocks.size - 1) 0x80 else 0) or b.first)
            o.be(b.second.size.toLong(), 3)
            o.bytes(b.second)
        }
        o.bytes(ByteArray(audioBytes) { (0xf8 + it % 7).toByte() })
        return o.toByteArray()
    }

    // ------------------------------------------------------------------ MP3

    private fun syncsafe(o: ByteArrayOutputStream, v: Int) {
        o.write((v shr 21) and 0x7f); o.write((v shr 14) and 0x7f); o.write((v shr 7) and 0x7f); o.write(v and 0x7f)
    }

    /** ID3v2.3 (UTF-16 text) or ID3v2.4 (UTF-8 text) and [frames] MPEG-1 layer III frames at 128 kbit/s, 44.1 kHz. */
    fun mp3(t: Tags, version: Int = 3, frames: Int = 40, xing: Boolean = false, id3v1: Boolean = false): ByteArray {
        val body = ByteArrayOutputStream()
        fun text(id: String, v: String?) {
            if (v == null) return
            val data = ByteArrayOutputStream()
            if (version == 3) { data.write(1); data.bytes(v.toByteArray(Charsets.UTF_16)) } // BOM + UTF-16
            else { data.write(3); data.bytes(v.toByteArray(Charsets.UTF_8)) }
            frame(body, id, data.toByteArray(), version)
        }
        text("TIT2", t.title); text("TPE1", t.artist); text("TALB", t.album); text("TPE2", t.albumArtist)
        text("TCON", t.genre); text(if (version == 3) "TYER" else "TDRC", t.date); text("TRCK", t.track); text("TPOS", t.disc)
        for ((k, v) in t.extra) {
            val d = ByteArrayOutputStream(); d.write(3); d.bytes(k.toByteArray()); d.write(0); d.bytes(v.toByteArray())
            frame(body, "TXXX", d.toByteArray(), version)
        }
        if (t.picture) {
            val d = ByteArrayOutputStream(); d.write(0); d.ascii("image/jpeg"); d.write(0); d.write(3); d.write(0)
            d.bytes(byteArrayOf(0xff.toByte(), 0xd8.toByte(), 0xff.toByte(), 0xd9.toByte()))
            frame(body, "APIC", d.toByteArray(), version)
        }
        body.bytes(ByteArray(64)) // padding
        val o = ByteArrayOutputStream()
        o.ascii("ID3"); o.write(version); o.write(0); o.write(0); syncsafe(o, body.size())
        o.bytes(body.toByteArray())
        // MPEG-1 layer III, 128 kbit/s, 44100 Hz, no padding, joint stereo: frame = 417 bytes
        val hdr = byteArrayOf(0xff.toByte(), 0xfb.toByte(), 0x90.toByte(), 0x44)
        for (i in 0 until frames) {
            val fr = ByteArray(417)
            System.arraycopy(hdr, 0, fr, 0, 4)
            if (i == 0 && xing) {
                // side info is 32 bytes for MPEG-1 stereo; then "Xing", flags (frames), frame count
                val x = 4 + 32
                "Xing".toByteArray().copyInto(fr, x)
                fr[x + 7] = 1
                val n = frames * 3 // pretend the stream is three times longer (VBR)
                fr[x + 8] = (n ushr 24).toByte(); fr[x + 9] = (n ushr 16).toByte(); fr[x + 10] = (n ushr 8).toByte(); fr[x + 11] = n.toByte()
            }
            o.bytes(fr)
        }
        if (id3v1) {
            val v1 = ByteArray(128)
            "TAG".toByteArray().copyInto(v1, 0)
            "V1 Title".toByteArray().copyInto(v1, 3)
            "V1 Artist".toByteArray().copyInto(v1, 33)
            "1999".toByteArray().copyInto(v1, 93)
            v1[126] = 7; v1[127] = 17
            o.bytes(v1)
        }
        return o.toByteArray()
    }

    private fun frame(o: ByteArrayOutputStream, id: String, data: ByteArray, version: Int) {
        o.ascii(id)
        if (version == 4) syncsafe(o, data.size) else o.be(data.size.toLong(), 4)
        o.be(0, 2)
        o.bytes(data)
    }

    // ------------------------------------------------------------------ MP4 / M4A

    private fun box(type: String, vararg parts: ByteArray): ByteArray {
        val body = ByteArrayOutputStream(); parts.forEach { body.bytes(it) }
        val o = ByteArrayOutputStream(); o.be(8L + body.size(), 4); o.ascii(type); o.bytes(body.toByteArray())
        return o.toByteArray()
    }

    private fun be(v: Long, n: Int) = ByteArrayOutputStream().apply { be(v, n) }.toByteArray()

    private fun dataBox(kind: Int, payload: ByteArray) = box("data", be(kind.toLong(), 4), be(0, 4), payload)

    fun m4a(t: Tags, alac: Boolean = false, sampleRate: Int = 44100, seconds: Int = 30, extraFreeform: Map<String, String> = emptyMap()): ByteArray {
        val mdhd = box("mdhd", be(0, 4), be(0, 4), be(0, 4), be(sampleRate.toLong(), 4), be(sampleRate.toLong() * seconds, 4), be(0, 4))
        val hdlr = box("hdlr", be(0, 4), be(0, 4), "soun".toByteArray(), ByteArray(12), byteArrayOf(0))
        val sampleEntryHead = ByteArrayOutputStream().apply {
            bytes(ByteArray(6)); be(1, 2); bytes(ByteArray(8)); be(2, 2); be(if (alac) 24 else 16, 2); be(0, 4); be(sampleRate.toLong(), 2); be(0, 2)
        }.toByteArray()
        val entry = if (alac) {
            val cfg = ByteArrayOutputStream().apply {
                be(0, 4); be(4096, 4); write(0); write(24); write(40); write(10); write(14); write(2); be(255, 2); be(0, 4); be(0, 4); be(sampleRate.toLong(), 4)
            }.toByteArray()
            box("alac", sampleEntryHead, box("alac", cfg))
        } else {
            val dcd = ByteArrayOutputStream().apply { write(0x40); write(0x15); be(0, 3); be(256000, 4); be(256000, 4) }.toByteArray()
            val esd = ByteArrayOutputStream().apply {
                be(0, 4)
                write(3); write(3 + 2 + dcd.size); be(1, 2); write(0)
                write(4); write(dcd.size); bytes(dcd)
            }.toByteArray()
            box("mp4a", sampleEntryHead, box("esds", esd))
        }
        val stsd = box("stsd", be(0, 4), be(1, 4), entry)
        val trak = box("trak", box("mdia", mdhd, hdlr, box("minf", box("stbl", stsd))))
        val items = ArrayList<ByteArray>()
        fun txt(type: String, v: String?) { if (v != null) items += box(type, dataBox(1, v.toByteArray())) }
        txt("©nam", t.title); txt("©ART", t.artist); txt("©alb", t.album); txt("aART", t.albumArtist)
        txt("©gen", t.genre); txt("©day", t.date)
        t.track?.let { items += box("trkn", dataBox(0, be(0, 2) + be(it.substringBefore('/').toLong(), 2) + be(0, 4))) }
        t.disc?.let { items += box("disk", dataBox(0, be(0, 2) + be(it.substringBefore('/').toLong(), 2) + be(0, 2))) }
        if (t.picture) items += box("covr", dataBox(13, byteArrayOf(0xff.toByte(), 0xd8.toByte())))
        for ((k, v) in extraFreeform + t.extra) {
            items += box("----", box("mean", be(0, 4), "com.apple.iTunes".toByteArray()), box("name", be(0, 4), k.toByteArray()), dataBox(1, v.toByteArray()))
        }
        val ilst = box("ilst", *items.toTypedArray())
        val meta = box("meta", be(0, 4), box("hdlr", be(0, 4), be(0, 4), "mdir".toByteArray(), ByteArray(12), byteArrayOf(0)), ilst)
        val moov = box("moov", box("mvhd", ByteArray(100)), trak, box("udta", meta))
        val ftyp = box("ftyp", "M4A ".toByteArray(), be(0, 4), "M4A mp42isom".toByteArray())
        val mdat = box("mdat", ByteArray(5000) { it.toByte() })
        // moov after mdat, as many encoders write it
        return ftyp + mdat + moov
    }

    fun write(dir: File, rel: String, bytes: ByteArray, mtimeSec: Long? = null): File {
        val f = File(dir, rel)
        f.parentFile.mkdirs()
        f.writeBytes(bytes)
        if (mtimeSec != null) f.setLastModified(mtimeSec * 1000)
        return f
    }
}
