package org.hibyos.install.core.library

import org.hibyos.install.core.testing.SynthAudio
import org.hibyos.install.core.testing.SynthAudio.Tags
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File

class TagReaderTest {
    private val dir = LibTestUtil.tempDir("tags")
    private fun file(name: String, b: ByteArray): File = SynthAudio.write(dir, name, b)

    @Test
    fun flacVorbisCommentsAndStreamInfo() {
        val f = file("a.flac", SynthAudio.flac(Tags("Canción", "Ñandú", "Álbum", "Various", "Pop", "2021-03-04", "7/12", "2/2",
            picture = true), sampleRate = 96000, bits = 24, seconds = 100, audioBytes = 120000))
        val i = TagReader.read(f)
        assertEquals("flac", i.codec)
        assertEquals("Canción", i.title); assertEquals("Ñandú", i.artist); assertEquals("Álbum", i.album)
        assertEquals("Various", i.albumArtist); assertEquals("Pop", i.genre); assertEquals("2021-03-04", i.date)
        assertEquals(7, TagReader.leadingInt(i.track)); assertEquals(2, TagReader.leadingInt(i.disc))
        assertEquals(96000, i.sampleRate); assertEquals(24, i.bitsPerSample); assertEquals(2, i.channels)
        assertEquals(100_000L, i.durationMs)
        assertTrue(i.lossless); assertTrue(i.hasPicture); assertFalse(i.mqa)
        assertEquals(120000L * 8 / 100, i.bitrate)
        assertEquals("3", LibraryBuilder.quality("flac", i))
    }

    @Test
    fun flacMqaDetection() {
        val i = TagReader.read(file("m.flac", SynthAudio.flac(Tags("x", extra = mapOf("MQAENCODER" to "MQAEncode v1.1")))))
        assertTrue(i.mqa)
        assertEquals("18", LibraryBuilder.quality("flac", i))
        val j = TagReader.read(file("m2.flac", SynthAudio.flac(Tags("y", extra = mapOf("ENCODER" to "MQA Encoder")))))
        assertTrue(j.mqa)
        assertEquals("2", LibraryBuilder.quality("flac", TagReader.read(file("p.flac", SynthAudio.flac(Tags("z"))))))
    }

    @Test
    fun mp3Id3v23Utf16() {
        val i = TagReader.read(file("b.mp3", SynthAudio.mp3(Tags("Título", "Artista", "Disco", "AA", "(17)", "1998", "3/10", "1/1",
            extra = mapOf("MQAENCODER" to "no"), picture = true), version = 3, frames = 100)))
        assertEquals("mp3", i.codec)
        assertEquals("Título", i.title); assertEquals("Artista", i.artist); assertEquals("Disco", i.album)
        assertEquals("AA", i.albumArtist); assertEquals("Rock", i.genre); assertEquals("1998", i.date)
        assertEquals(3, TagReader.leadingInt(i.track))
        assertEquals(44100, i.sampleRate); assertEquals(2, i.channels); assertEquals(128000L, i.bitrate)
        assertTrue(i.hasPicture)
        // 100 frames x 417 bytes at 128 kbit/s
        assertEquals(100L * 417 * 8 * 1000 / 128000, i.durationMs)
        assertEquals("1", LibraryBuilder.quality("mp3", TagReader.read(file("q.mp3", SynthAudio.mp3(Tags("q"))))))
    }

    @Test
    fun mp3Id3v24Utf8AndXing() {
        val i = TagReader.read(file("c.mp3", SynthAudio.mp3(Tags("日本語", "Björk", genre = "Electronic"), version = 4, frames = 50, xing = true)))
        assertEquals("日本語", i.title); assertEquals("Björk", i.artist); assertEquals("Electronic", i.genre)
        // the Xing header says 150 frames of 1152 samples
        assertEquals(150L * 1152 * 1000 / 44100, i.durationMs)
    }

    @Test
    fun mp3Id3v1Fallback() {
        val b = SynthAudio.mp3(Tags(album = "Only v2 album"), frames = 10, id3v1 = true)
        val i = TagReader.read(file("d.mp3", b))
        assertEquals("V1 Title", i.title); assertEquals("V1 Artist", i.artist); assertEquals("Only v2 album", i.album)
        assertEquals("1999", i.date); assertEquals(7, TagReader.leadingInt(i.track)); assertEquals("Rock", i.genre)
    }

    @Test
    fun m4aAacAndAlac() {
        val a = TagReader.read(file("e.m4a", SynthAudio.m4a(Tags("Song", "Band", "LP", "Band", "Jazz", "2010", "5", "1", picture = true), seconds = 30)))
        assertEquals("aac", a.codec)
        assertEquals("Song", a.title); assertEquals("Band", a.artist); assertEquals("LP", a.album); assertEquals("Band", a.albumArtist)
        assertEquals("Jazz", a.genre); assertEquals("2010", a.date); assertEquals("5", a.track); assertEquals("1", a.disc)
        assertEquals(44100, a.sampleRate); assertEquals(2, a.channels); assertEquals(30_000L, a.durationMs)
        assertEquals(256000L, a.bitrate); assertTrue(a.hasPicture); assertFalse(a.lossless)
        assertEquals("1", LibraryBuilder.quality("m4a", a))
        val l = TagReader.read(file("f.m4a", SynthAudio.m4a(Tags("Lossless"), alac = true, sampleRate = 88200,
            extraFreeform = mapOf("MQAENCODER" to "x"))))
        assertEquals("alac", l.codec); assertTrue(l.lossless); assertEquals(24, l.bitsPerSample); assertEquals(88200, l.sampleRate)
        assertTrue(l.mqa)
    }

    @Test
    fun brokenFilesFailCleanly() {
        for ((name, b) in listOf("x.flac" to "fLaC".toByteArray() + ByteArray(3), "y.mp3" to ByteArray(2000), "z.m4a" to
            byteArrayOf(0, 0, 0, 8) + "ftyp".toByteArray() + byteArrayOf(0, 0, 0, 100) + "moov".toByteArray(), "w.ogg" to ByteArray(10))) {
            try {
                TagReader.read(file(name, b))
                fail("$name should not parse")
            } catch (e: java.io.IOException) {
                // TagException or EOF: the builder lists such files by their name
            }
        }
        assertNull(TagReader.read(file("g.mp3", SynthAudio.mp3(Tags()))).title)
    }

    @Test
    fun characterAndOrder() {
        assertEquals("Á", LibraryBuilder.character("ábreme"))
        assertEquals("A", LibraryBuilder.character("abc"))
        assertEquals("¡", LibraryBuilder.character("¡Ay"))
        assertEquals("Д", LibraryBuilder.character("дом"))
        val names = listOf("¿Qué?", "10k", "Abc", "1nce", "abd", "(es)", "Zeta", "2 Dias", "100 AMAPOLAS", "Ábreme", "19 Dias")
        assertEquals(listOf("Abc", "abd", "Zeta", "1nce", "2 Dias", "10k", "19 Dias", "100 AMAPOLAS", "(es)", "¿Qué?", "Ábreme"),
            names.sortedWith(LibraryBuilder.stockOrder))
    }
}
