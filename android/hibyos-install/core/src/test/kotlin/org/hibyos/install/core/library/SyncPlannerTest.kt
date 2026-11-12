package org.hibyos.install.core.library

import org.hibyos.install.core.btp.BtpPaths
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SyncPlannerTest {
    private fun t(src: String, title: String, artist: String, album: String, track: Int = 0, disc: Int = 0, size: Long = 1000,
                  mtime: Long = 100, albumArtist: String = "", genre: String = "", book: Boolean = false): SourceTrack {
        val rel = src.removePrefix("C:/Music/")
        return SourceTrack(src, rel, IndexTrack(0, src, title, artist, albumArtist, album, genre, 0, track, disc, 0, size, mtime, mtime,
            44100, 16, 0, true, false, book))
    }

    private val lib = listOf(
        t("C:/Music/abba/01.flac", "Dancing Queen", "ABBA", "Arrival", 1, genre = "Pop"),
        t("C:/Music/abba/02.flac", "Money, Money, Money", "ABBA", "Arrival", 2, genre = "Pop"),
        t("C:/Music/x/what.flac", "What? Why: \"Now\"", "AC/DC", "Live.", 3),
        t("C:/Music/x/d1.flac", "Intro", "Band", "Double", 1, 1),
        t("C:/Music/x/d2.flac", "Intro", "Band", "Double", 1, 2),
        t("C:/Music/x/dup1.flac", "Same", "Band", "Same"),
        t("C:/Music/x/dup2.flac", "same", "Band", "Same"),
        t("C:/Books/dune/1.mp3", "Part 1", "Herbert", "Dune", book = true),
    )

    @Test
    fun namesFollowTheCardRules() {
        assertEquals("What_ Why_ _Now_", SyncPlanner.cleanName("What? Why: \"Now\""))
        assertEquals("Live", SyncPlanner.cleanName("Live."))
        assertEquals("hidden", SyncPlanner.cleanName(".hidden"))
        assertEquals("Unknown", SyncPlanner.cleanName(" .. "))
        assertEquals("AC_DC", SyncPlanner.cleanName("AC/DC"))
        assertEquals("Canción", SyncPlanner.cleanName("Cancio\u0301n"))
    }

    @Test
    fun artistAlbumLayout() {
        val p = SyncPlanner.plan(lib, SyncSelection(), "Music", "artist-album", "keep", emptyList(), emptyList(), false, 1L shl 40)
        val dests = p.items.associate { it.src to it.dest }
        assertEquals("Music/ABBA/Arrival/01 Dancing Queen.flac", dests["C:/Music/abba/01.flac"])
        assertEquals("Music/AC_DC/Live/03 What_ Why_ _Now_.flac", dests["C:/Music/x/what.flac"])
        // several discs: the disc number goes first
        assertEquals("Music/Band/Double/1-01 Intro.flac", dests["C:/Music/x/d1.flac"])
        assertEquals("Music/Band/Double/2-01 Intro.flac", dests["C:/Music/x/d2.flac"])
        // FAT names compare without case: the second one gets " (2)"
        assertEquals(setOf("Music/Band/Same/Same.flac", "Music/Band/Same/same (2).flac"),
            setOf(dests["C:/Music/x/dup1.flac"], dests["C:/Music/x/dup2.flac"]))
        assertEquals("Audiobooks/Dune/1.mp3", dests["C:/Books/dune/1.mp3"])
        assertTrue(p.items.all { BtpPaths.check(it.dest) == null })
        assertEquals(8, p.add); assertTrue(p.fits)
    }

    @Test
    fun keepLayoutAndSelection() {
        val sel = SyncSelection(all = false, artists = setOf(LibraryIndex.fold("abba")), books = false)
        val p = SyncPlanner.plan(lib, sel, "Music", "keep", "keep", emptyList(), emptyList(), false, 1L shl 40)
        assertEquals(listOf("Music/abba/01.flac", "Music/abba/02.flac"), p.items.map { it.dest })
        val byGenre = SyncPlanner.plan(lib, SyncSelection(all = false, genres = setOf("pop")), "Music", "keep", "keep", emptyList(), emptyList(), false, 1L shl 40)
        assertEquals(3, byGenre.items.size) // two Pop songs and the book
        val byAlbum = SyncPlanner.plan(lib, SyncSelection(all = false, albums = setOf(SyncSelection.albumKey("band", "double")), books = false),
            "Music", "keep", "keep", emptyList(), emptyList(), false, 1L shl 40)
        assertEquals(2, byAlbum.items.size)
    }

    @Test
    fun manifestSkipsUpdatesAndRemoves() {
        val one = listOf(lib[0], lib[1])
        val remote = listOf(
            RemoteFile("Music/ABBA/Arrival/01 Dancing Queen.flac", 1200, 5),   // optimized copy (bigger)
            RemoteFile("Music/ABBA/Arrival/02 Money, Money, Money.flac", 1000, 5),
            RemoteFile("Music/Old/gone.flac", 777, 5),
            RemoteFile("Music/Mine/user.flac", 555, 5),
        )
        val manifest = listOf(
            ManifestEntry("Music/ABBA/Arrival/01 Dancing Queen.flac", "C:/Music/abba/01.flac", 1000, 100, 1200, "optimize"),
            ManifestEntry("Music/ABBA/Arrival/02 Money, Money, Money.flac", "C:/Music/abba/02.flac", 1000, 99, 1000, "optimize"),
            ManifestEntry("Music/Old/gone.flac", "C:/Music/old.flac", 777, 1, 777, "optimize"),
        )
        val p = SyncPlanner.plan(one, SyncSelection(), "Music", "artist-album", "optimize", remote, manifest, true, 10_000_000_000)
        val by = p.items.associateBy { it.dest }
        assertEquals("skip", by["Music/ABBA/Arrival/01 Dancing Queen.flac"]!!.action)
        assertEquals("update", by["Music/ABBA/Arrival/02 Money, Money, Money.flac"]!!.action) // the source changed (mtime)
        assertEquals("remove", by["Music/Old/gone.flac"]!!.action)
        // a file the sync never copied is never removed
        assertNull(by["Music/Mine/user.flac"])
        assertEquals(1, p.remove)
        // covers chosen differently than last time: copied again
        val p2 = SyncPlanner.plan(one, SyncSelection(), "Music", "artist-album", "keep", remote, manifest, false, 10_000_000_000)
        assertEquals("covers", p2.items.first { it.src == "C:/Music/abba/01.flac" }.reason)
    }

    @Test
    fun userFilesAreNeverOverwritten() {
        val remote = listOf(RemoteFile("Music/ABBA/Arrival/01 Dancing Queen.flac", 4242, 5))
        val p = SyncPlanner.plan(listOf(lib[0]), SyncSelection(), "Music", "artist-album", "keep", remote, emptyList(), false, 1L shl 40)
        assertEquals("Music/ABBA/Arrival/01 Dancing Queen (2).flac", p.items[0].dest)
        assertEquals("add", p.items[0].action)
        // the same size: taken as the same song, not copied twice
        val same = SyncPlanner.plan(listOf(lib[0]), SyncSelection(), "Music", "artist-album", "keep",
            listOf(RemoteFile("Music/ABBA/Arrival/01 Dancing Queen.flac", 1000, 5)), emptyList(), false, 1L shl 40)
        assertEquals("skip", same.items[0].action)
    }

    @Test
    fun space() {
        val p = SyncPlanner.plan(lib, SyncSelection(), "Music", "artist-album", "keep", emptyList(), emptyList(), false, SyncPlanner.MARGIN + 7999)
        assertEquals(8000, p.bytesAdd)
        assertFalse(p.fits)
        assertTrue(SyncPlanner.plan(lib, SyncSelection(), "Music", "artist-album", "keep", emptyList(), emptyList(), false, SyncPlanner.MARGIN + 8000).fits)
    }

    @Test
    fun m3u() {
        val text = M3uWriter.write("Playlists/Road trip.m3u8", listOf("Music/ABBA/01.flac", "Playlists/x.mp3"))
        assertEquals("#EXTM3U\r\n../Music/ABBA/01.flac\r\nx.mp3\r\n", text)
        assertEquals(listOf("Music/ABBA/01.flac", "Playlists/x.mp3"), M3uWriter.read("Playlists/Road trip.m3u8", text))
        assertEquals(listOf("Music/a.flac", "Music/b.flac", "c.mp3"),
            M3uWriter.read("Playlists/p.m3u", "\uFEFF#EXTM3U\na:\\Music\\a.flac\n/Music/b.flac\n# comment\n../c.mp3\n"))
        assertEquals("Playlists/Road trip.m3u8", M3uWriter.pathFor("Road trip"))
        assertEquals("Playlists/a_b.m3u8", M3uWriter.pathFor("a/b"))
    }
}
