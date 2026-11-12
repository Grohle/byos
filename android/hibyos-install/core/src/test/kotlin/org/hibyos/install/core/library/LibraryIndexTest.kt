package org.hibyos.install.core.library

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class LibraryIndexTest {
    private val schema = LibTestUtil.stockSchema()
    private val defs = schema.filter { it.type == "table" }.associate { it.name to SchemaSql.parseTable(it.sql!!) }

    private fun z(s: String) = SqlText(s + "\u0000")
    private fun row(table: String, rowid: Long, vararg kv: Pair<String, Any?>): SqlRow {
        val m = kv.toMap()
        return SqlRow(rowid, defs[table]!!.columns.map { m[it.name] })
    }

    private var nextRow = 1L
    private fun media(id: Long, path: String, name: String, album: String, artist: String, albumArtist: String = "",
                      track: Long = 0, disc: Long = 0, year: Long = 0, genre: String = "Unknown", folder: Boolean = false, pic: String = "") =
        row("MEDIA_TABLE", nextRow++, "id" to id, "path" to z(path), "name" to z(name), "album" to z(album), "artist" to z(artist),
            "genre" to z(genre), "year" to year, "dis_id" to disc, "ck_id" to track, "has_child_file" to if (folder) 3L else 0L,
            "begin_time" to 0L, "end_time" to -1L, "cue_id" to -1L, "character" to SqlText(name.take(1)), "size" to 1000L + id,
            "sample_rate" to 44100L, "bit_rate" to 1411L * 1024, "bit" to 16L, "channel" to 2L, "format" to 61868L,
            "quality" to SqlText("2"), "album_pic_path" to z(pic), "lrc_path" to z(""), "ctime" to 1700000000L + id,
            "mtime" to 1690000000L, "album_artist" to z(albumArtist))

    private fun snapshot(f: File) {
        val data = mapOf(
            "MEDIA_TABLE" to listOf(
                media(1, "a:\\Music\\The Beatles\\Abbey Road\\02 Something.flac", "Something", "Abbey Road", "The Beatles", track = 2, year = 1969),
                media(2, "a:\\Music\\The Beatles\\Abbey Road\\01 Come Together.flac", "Come Together", "Abbey Road", "The Beatles", track = 1, year = 1969, pic = "a:\\Music\\The Beatles\\Abbey Road\\cover.jpg"),
                media(3, "a:\\Music\\ABBA\\Arrival\\01 Dancing Queen.flac", "Dancing Queen", "Arrival", "ABBA", genre = "Pop", track = 1),
                media(4, "a:\\Music\\Various\\Hits\\01 x.mp3", "Song X", "Hits", "Artist A", albumArtist = "Various Artists", track = 1, genre = "pop"),
                media(5, "a:\\Music\\Various\\Hits\\02 y.mp3", "Song Y", "Hits", "Artist B", albumArtist = "Various Artists", track = 2),
                media(6, "a:\\Audiobooks\\Dune\\Part 1.mp3", "Part 1", "Dune", "Herbert"),
                media(7, "a:\\Audiobooks\\Dune\\Part 2.mp3", "Part 2", "Dune", "Herbert"),
                media(8, "a:\\Music", "Music", "Unknown", "Unknown", folder = true),
                media(9, "b:\\Other\\z.flac", "Elsewhere", "", ""),
                media(10, "a:\\Music\\Ñandú\\Canción.flac", "", "", ""),
            ),
            "M3U_TABLE" to listOf(row("M3U_TABLE", 1, "id" to 1L, "path" to z("a:\\Playlists\\Road trip.m3u8"), "name" to z("Road trip.m3u8"), "character" to SqlText("R"))),
            "VERSION_TABLE" to listOf(row("VERSION_TABLE", 1, "version_id" to 20181015L)),
        )
        SqliteWriter(f).write(schema) { data[it] ?: emptyList() }
    }

    @Test
    fun snapshotIsGroupedLikeThePlayer() {
        val d = LibTestUtil.tempDir("index")
        val db = File(d, "snap.db")
        snapshot(db)
        val idx = LibraryIndex.fromSnapshot(db, "rev1", 42)
        // folders and other drives are left out
        assertEquals(8, idx.tracks.size)
        assertEquals(6, idx.tracks.count { !it.book })
        assertTrue(idx.tracks.none { it.path.contains("\\") || it.title.contains("\u0000") })
        // a missing title falls back to the file name, NFC
        assertTrue(idx.tracks.any { it.title == "Canción" && it.path == "Music/Ñandú/Canción.flac" })
        // albums: "The Beatles" sorts as Beatles; tracks by number; album artist groups compilations
        assertEquals(listOf("Abbey Road", "Arrival", "Hits", ""), idx.albums.map { it.title })
        val abbey = idx.albums[0]
        assertEquals(listOf("Come Together", "Something"), abbey.tracks.map { idx.tracks[it].title })
        assertEquals("Music/The Beatles/Abbey Road/01 Come Together.flac", abbey.art)
        assertEquals(1969, abbey.year)
        val hits = idx.albums.first { it.title == "Hits" }
        assertEquals("Various Artists", hits.artist)
        assertEquals(2, hits.tracks.size)
        // artists: ABBA, The Beatles (as B), Various Artists, then the unnamed one last
        assertEquals(listOf("ABBA", "The Beatles", "Various Artists", ""), idx.artists.map { it.name })
        // genres merge case; "Unknown" is no genre
        assertEquals(listOf("Pop" to 2), idx.genres)
        // the book folder is one audiobook with two parts
        assertEquals(1, idx.books.size)
        assertEquals("Dune", idx.books[0].name)
        assertEquals("Audiobooks/Dune", idx.books[0].path)
        assertEquals(2, idx.books[0].tracks.size)
        assertEquals(listOf("Road trip"), idx.playlists.map { it.name })
        assertEquals("Playlists/Road trip.m3u8", idx.playlists[0].path)
        val m = idx.toMap()
        assertEquals("snapshot", m["source"]); assertEquals("rev1", m["revision"])
        @Suppress("UNCHECKED_CAST")
        val counts = m["counts"] as Map<String, Any?>
        assertEquals(6, counts["tracks"]); assertEquals(1, counts["books"]); assertEquals(1, counts["playlists"])
    }

    @Test
    fun pathsAndFolding() {
        assertEquals("Music/a b/c.flac", LibraryIndex.cardPath("a:\\Music\\a b\\c.flac\u0000"))
        assertEquals("x.mp3", LibraryIndex.cardPath("A:\\x.mp3"))
        assertNull(LibraryIndex.cardPath("b:\\x.mp3"))
        assertEquals(LibraryIndex.fold("Beyoncé"), LibraryIndex.fold("BEYONCE"))
        assertTrue(LibraryIndex.isBook("Audiobooks/x/1.mp3"))
        assertTrue(LibraryIndex.isBook("Music/x.m4b"))
        assertFalse(LibraryIndex.isBook("Music/Audiobooks/x.mp3"))
    }

    @Test
    fun tracksFromTags() {
        val i = AudioInfo("flac").apply { title = "T"; artist = "A"; album = "B"; track = "3/10"; disc = "2"; date = "2001-05-01"; sampleRate = 96000; bitsPerSample = 24; lossless = true; hasPicture = true }
        val t = LibraryIndex.track(1, "Music/x.flac", 10, 20, i)
        assertEquals(3, t.track); assertEquals(2, t.disc); assertEquals(2001, t.year); assertEquals(24, t.bits); assertTrue(t.hasArt)
        val none = LibraryIndex.track(2, "Music/Folder/Untitled.mp3", 10, 20, null)
        assertEquals("Untitled", none.title)
        assertFalse(none.lossless)
    }
}
