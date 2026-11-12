package org.hibyos.install.core.library

import org.hibyos.install.core.testing.SynthAudio
import org.hibyos.install.core.testing.SynthAudio.Tags
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile

/** Embedded picture locations (TagReader), partial reads (RangeTagSource) and cover rewriting (PictureWriter). */
class PictureWriterTest {
    private val jpeg = ByteArray(3000) { (it * 7).toByte() }.also { it[0] = 0xff.toByte(); it[1] = 0xd8.toByte() }
    private val dir = LibTestUtil.tempDir("picture")

    private fun pictureBytes(f: File): ByteArray? {
        val i = TagReader.read(f)
        val p = i.picture ?: return null
        return RandomAccessFile(f, "r").use { r -> ByteArray(p.length.toInt()).also { r.seek(p.offset); r.readFully(it) } }
    }

    private val tiny = byteArrayOf(0xff.toByte(), 0xd8.toByte(), 0xff.toByte(), 0xd9.toByte())

    @Test
    fun pictureLocations() {
        val flac = SynthAudio.write(dir, "a.flac", SynthAudio.flac(Tags("T", picture = true)))
        assertArrayEquals(tiny, pictureBytes(flac))
        for (v in listOf(3, 4)) {
            val mp3 = SynthAudio.write(dir, "a$v.mp3", SynthAudio.mp3(Tags("T", "A", picture = true), version = v))
            assertArrayEquals("ID3v2.$v", tiny, pictureBytes(mp3))
            assertEquals("T", TagReader.read(mp3).title)
        }
        val m4a = SynthAudio.write(dir, "a.m4a", SynthAudio.m4a(Tags("T", picture = true)))
        assertArrayEquals(byteArrayOf(0xff.toByte(), 0xd8.toByte()), pictureBytes(m4a))
    }

    @Test
    fun rangeSourceReadsTheSameAsTheFile() {
        val files = listOf(
            SynthAudio.write(dir, "r.flac", SynthAudio.flac(Tags("Flac", "A", "B", genre = "Rock", picture = true))),
            SynthAudio.write(dir, "r.mp3", SynthAudio.mp3(Tags("Mp3", "A", "B", picture = true), xing = true, id3v1 = true)),
            // moov after mdat: the reader asks for a block near the end of the file
            SynthAudio.write(dir, "r.m4a", SynthAudio.m4a(Tags("M4a", "A", "B", track = "4", picture = true))),
        )
        for (f in files) {
            val bytes = f.readBytes()
            val asked = ArrayList<Pair<Long, Int>>()
            val src = RangeTagSource(bytes.size.toLong(), block = 1024) { off, n -> asked += off to n; bytes.copyOfRange(off.toInt(), off.toInt() + n) }
            val r = TagReader.read(src, f.name)
            val l = TagReader.read(f)
            assertEquals(f.name, l.title, r.title)
            assertEquals(f.name, l.artist, r.artist)
            assertEquals(f.name, l.durationMs, r.durationMs)
            assertEquals(f.name, l.picture?.offset, r.picture?.offset)
            assertTrue(f.name, asked.all { it.first % 1024 == 0L })
            // every block is fetched once
            assertEquals(f.name, asked.size, asked.map { it.first }.distinct().size)
        }
    }

    private fun mp3Audio(b: ByteArray): ByteArray {
        val size = ((b[6].toInt() and 0x7f) shl 21) or ((b[7].toInt() and 0x7f) shl 14) or ((b[8].toInt() and 0x7f) shl 7) or (b[9].toInt() and 0x7f)
        return b.copyOfRange(10 + size, b.size)
    }

    private fun flacAudio(f: File): ByteArray {
        val b = f.readBytes()
        var p = 4
        while (true) { val last = b[p].toInt() and 0x80 != 0; val len = ((b[p + 1].toInt() and 0xff) shl 16) or ((b[p + 2].toInt() and 0xff) shl 8) or (b[p + 3].toInt() and 0xff); p += 4 + len; if (last) break }
        return b.copyOfRange(p, b.size)
    }

    @Test
    fun flacCoverIsReplacedAndAudioKept() {
        val src = SynthAudio.write(dir, "in.flac", SynthAudio.flac(Tags("Song", "Band", "LP", track = "1", picture = true)))
        val out = File(dir, "out.flac")
        PictureWriter.setCover(src, out, jpeg, 500, 500)
        assertArrayEquals(jpeg, pictureBytes(out))
        val i = TagReader.read(out)
        assertEquals("Song", i.title); assertEquals("1", i.track); assertEquals(3, i.picture!!.type)
        assertArrayEquals(flacAudio(src), flacAudio(out))
        // one front cover only, the old one is gone
        val text = String(out.readBytes(), Charsets.ISO_8859_1)
        assertEquals(1, Regex("image/jpeg").findAll(text).count())
    }

    @Test
    fun mp3CoverIsReplacedOrAdded() {
        for (v in listOf(3, 4)) {
            val src = SynthAudio.write(dir, "in$v.mp3", SynthAudio.mp3(Tags("Song", "Band", picture = true), version = v, id3v1 = true))
            val out = File(dir, "out$v.mp3")
            PictureWriter.setCover(src, out, jpeg, 500, 500)
            assertArrayEquals(jpeg, pictureBytes(out))
            assertEquals("Song", TagReader.read(out).title)
            assertArrayEquals(mp3Audio(src.readBytes()), mp3Audio(out.readBytes()))
            assertEquals(v, out.readBytes()[3].toInt())
        }
        // no ID3 tag at all: a new ID3v2.3 tag
        val raw = mp3Audio(SynthAudio.mp3(Tags("x")))
        val bare = SynthAudio.write(dir, "bare.mp3", raw)
        val out = File(dir, "bare-out.mp3")
        PictureWriter.setCover(bare, out, jpeg, 500, 500)
        assertArrayEquals(jpeg, pictureBytes(out))
        assertArrayEquals(raw, mp3Audio(out.readBytes()))
    }

    @Test
    fun m4aCoverWithMoovAtTheEnd() {
        val src = SynthAudio.write(dir, "in.m4a", SynthAudio.m4a(Tags("Song", "Band", picture = true)))
        val out = File(dir, "out.m4a")
        PictureWriter.setCover(src, out, jpeg, 500, 500)
        assertArrayEquals(jpeg, pictureBytes(out))
        assertEquals("Song", TagReader.read(out).title)
        val a = src.readBytes(); val b = out.readBytes()
        // ftyp + mdat come first and are unchanged
        val mdatEnd = 8 + 20 + 8 + 5000
        assertArrayEquals(a.copyOfRange(0, mdatEnd), b.copyOfRange(0, mdatEnd))
    }

    private fun box(type: String, vararg parts: ByteArray): ByteArray {
        val body = ByteArrayOutputStream(); parts.forEach { body.write(it) }
        val n = 8 + body.size()
        return byteArrayOf((n ushr 24).toByte(), (n ushr 16).toByte(), (n ushr 8).toByte(), n.toByte()) + type.toByteArray(Charsets.ISO_8859_1) + body.toByteArray()
    }
    private fun be32(v: Int) = byteArrayOf((v ushr 24).toByte(), (v ushr 16).toByte(), (v ushr 8).toByte(), v.toByte())

    @Test
    fun m4aChunkOffsetsMoveWhenMoovComesFirst() {
        val payload = ByteArray(4000) { (it % 251).toByte() }
        fun file(chunkOff: Int): ByteArray {
            val stco = box("stco", be32(0), be32(1), be32(chunkOff))
            val trak = box("trak", box("mdia", box("minf", box("stbl", stco))))
            val moov = box("moov", box("mvhd", ByteArray(100)), trak)
            val ftyp = box("ftyp", "M4A ".toByteArray(), be32(0))
            return ftyp + moov + box("mdat", payload)
        }
        val probe = file(0)
        val dataAt = probe.size - payload.size
        val src = SynthAudio.write(dir, "first.m4a", file(dataAt))
        val out = File(dir, "first-out.m4a")
        PictureWriter.setCover(src, out, jpeg, 500, 500)
        val b = out.readBytes()
        val newDataAt = b.size - payload.size
        assertArrayEquals(payload, b.copyOfRange(newDataAt, b.size))
        val s = String(b, Charsets.ISO_8859_1).indexOf("stco")
        val off = ((b[s + 12].toInt() and 0xff) shl 24) or ((b[s + 13].toInt() and 0xff) shl 16) or ((b[s + 14].toInt() and 0xff) shl 8) or (b[s + 15].toInt() and 0xff)
        assertEquals(newDataAt, off)
        assertNotNull(pictureBytes(out))
        assertArrayEquals(jpeg, pictureBytes(out))
    }

    @Test(expected = PictureWriter.Unsupported::class)
    fun oggIsRefused() {
        val f = SynthAudio.write(dir, "x.ogg", "OggS".toByteArray() + ByteArray(100))
        PictureWriter.setCover(f, File(dir, "x-out.ogg"), jpeg, 500, 500)
    }
}
