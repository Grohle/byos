package org.hibyos.install.core.library

import org.hibyos.install.core.testing.SynthAudio
import org.hibyos.install.core.testing.SynthAudio.Tags
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

class LibraryBuilderTest {
    private val schema = LibTestUtil.stockSchema()
    private val defs = schema.filter { it.type == "table" }.associate { it.name to SchemaSql.parseTable(it.sql!!) }

    private fun z(s: String) = SqlText(s + "\u0000")
    private fun row(table: String, rowid: Long, vararg kv: Pair<String, Any?>): SqlRow {
        val m = kv.toMap()
        return SqlRow(rowid, defs[table]!!.columns.map { m[it.name] })
    }

    private fun track(table: String, rowid: Long, id: Long, path: String, name: String, ctime: Long = 1700000000) = row(table, rowid,
        "id" to id, "path" to z(path), "name" to z(name), "album" to z("LP"), "artist" to z("Band"), "genre" to z("Unknown"),
        "year" to 0L, "dis_id" to 0L, "ck_id" to 0L, "has_child_file" to 0L, "begin_time" to 0L, "end_time" to -1L, "cue_id" to -1L,
        "character" to SqlText(name.take(1)), "ctime" to ctime, "mtime" to 1690000000L)

    /** A snapshot as the device would have it: a few tracks, user data in the kept tables. */
    private fun base(f: File) {
        val data = mapOf(
            "MEDIA_TABLE" to listOf(
                track("MEDIA_TABLE", 1, 2, "a:\\Band\\LP\\01 Song.flac", "Song", 1700000000),
                track("MEDIA_TABLE", 2, 7, "a:\\Gone\\x.mp3", "x"),
                track("MEDIA_TABLE", 3, 50, "a:\\Band\\LP\\03 Three.flac", "Three", 1700000500)),
            "HISTORY_TABLE" to listOf(track("HISTORY_TABLE", 12, 50, "a:\\Band\\LP\\03 Three.flac", "Three"),
                track("HISTORY_TABLE", 13, 7, "a:\\Gone\\x.mp3", "x")),
            "COLLECT_TABLE" to listOf(track("COLLECT_TABLE", 1, 2, "a:\\Band\\LP\\01 Song.flac", "Song")),
            "list_tb_1" to listOf(track("list_tb_1", 1, 2, "a:\\Band\\LP\\01 Song.flac", "Song")),
            "MANAGER_TABLE" to listOf(row("MANAGER_TABLE", 1, "id" to 1L, "path" to z("album_artist\\Band"), "count" to 1L,
                "music_count" to 1L, "dir_count" to 0L, "m3u_count" to 0L)),
            "VERSION_TABLE" to listOf(row("VERSION_TABLE", 1, "version_id" to 20181015L)),
            "M3U_TABLE" to listOf(row("M3U_TABLE", 1, "id" to 1L, "path" to z("a:\\Mezclas\\Mix.m3u"), "name" to z("Mix.m3u"), "character" to SqlText("M"))),
            "SEARCH_TABLE" to listOf(track("SEARCH_TABLE", 1, 2, "a:\\Band\\LP\\01 Song.flac", "Song")),
        )
        SqliteWriter(f).write(schema) { data[it] ?: emptyList() }
    }

    private fun music(root: File) {
        SynthAudio.write(root, "Band/LP/01 Song.flac", SynthAudio.flac(Tags("Song", "Band", "LP", "Band", "Rock", "2001", "1")), 1710000000)
        SynthAudio.write(root, "Band/LP/02 Other.flac", SynthAudio.flac(Tags("Other", "Band", "LP", "Band", "Rock", "2001", "2",
            extra = mapOf("MQAENCODER" to "v1"))), 1710000001)
        SynthAudio.write(root, "Band/LP/03 Three.flac", SynthAudio.flac(Tags("Three", "Band", "LP", "Band", "rock", "2001", "3")), 1710000002)
        SynthAudio.write(root, "Band/LP/cover.jpg", byteArrayOf(1, 2, 3))
        SynthAudio.write(root, "Band/LP/01 Song.LRC", "[00:00]la".toByteArray())
        SynthAudio.write(root, "Band/Hi/01 Hi.flac", SynthAudio.flac(Tags("Hi", "Band", "Hi", null, null, "2020", "1"),
            sampleRate = 96000, bits = 24), 1710000003)
        // decomposed (NFD) file name; tags differ only in case from "Band"/"LP"
        SynthAudio.write(root, "Solo/Cancio\u0301n.mp3", SynthAudio.mp3(Tags(null, "band", "lp", genre = "Pop")), 1710000004)
        SynthAudio.write(root, "Mix/Live.m4a", SynthAudio.m4a(Tags("¡Live!", "Other Band", "Live", genre = "Jazz")), 1710000005)
        SynthAudio.write(root, "Mix/Alac.m4a", SynthAudio.m4a(Tags("100 Days", "Other Band", "Live"), alac = true, sampleRate = 48000), 1710000006)
        SynthAudio.write(root, "Root.mp3", SynthAudio.mp3(Tags("Root song", "Band", "Singles")), 1710000007)
        SynthAudio.write(root, "junk.ogg", ByteArray(100), 1710000008)
        SynthAudio.write(root, "Lists/new.m3u", "Band/LP/01 Song.flac\n".toByteArray())
        SynthAudio.write(root, ".hidden/secret.flac", SynthAudio.flac(Tags("secret")))
    }

    private fun text(v: Any?) = (v as SqlText).text

    @Test
    fun buildsStockLibrary() {
        val dir = LibTestUtil.tempDir("builder")
        val baseFile = File(dir, "base.db"); base(baseFile)
        val root = File(dir, "sd"); music(root)
        val out = File(dir, "new.db")
        val warns = ArrayList<String>()
        val rep = LibraryBuilder.build(baseFile, out, LibraryOptions(root, folderCovers = true, sidecarLyrics = true, now = 1800000000),
            object : LibraryListener { override fun warn(code: String, message: String, path: String?) { warns += code } })
        assertEquals(9, rep.tracks)
        assertEquals(listOf("no_tags"), warns)
        assertEquals(1, rep.noTags)
        assertEquals(1, rep.renumbered)
        assertEquals(1, rep.playlistsAdded)
        assertTrue(rep.keptTables.containsAll(listOf("HISTORY_TABLE", "COLLECT_TABLE", "list_tb_1", "MANAGER_TABLE", "VERSION_TABLE")))

        SqliteReader(out).use { r ->
            val mdef = defs["MEDIA_TABLE"]!!
            fun c(n: String) = mdef.col(n)
            val media = r.table("MEDIA_TABLE")
            // id 7 belonged to a removed file that still has a history row: it is not reused
            assertEquals(listOf(1L, 2L, 3L, 4L, 5L, 6L, 8L, 9L, 10L), media.map { it.values[c("id")] as Long }.sorted())
            val byPath = media.associateBy { text(it.values[c("path")]) }
            val song = byPath["a:\\Band\\LP\\01 Song.flac\u0000"]!!
            assertEquals(2L, song.values[c("id")])                        // id kept
            assertEquals(1700000000L, song.values[c("ctime")])            // date added kept
            assertEquals(1710000000L, song.values[c("mtime")])
            assertEquals("a:\\Band\\LP\\cover.jpg\u0000", text(song.values[c("album_pic_path")]))
            assertEquals("a:\\Band\\LP\\01 Song.LRC\u0000", text(song.values[c("lrc_path")]))
            assertEquals("2", text(song.values[c("quality")]))
            assertEquals(61868L, song.values[c("format")])
            assertEquals("S", text(song.values[c("character")]))
            assertEquals("SONG\u0000", text(song.values[c("pinyin_charater")]))
            assertEquals(1L, song.values[c("ck_id")])
            assertEquals(2001L, song.values[c("year")])
            val three = byPath["a:\\Band\\LP\\03 Three.flac\u0000"]!!
            val threeId = three.values[c("id")] as Long
            assertEquals(4L, threeId)
            assertEquals(1700000500L, three.values[c("ctime")])
            assertEquals("18", text(byPath["a:\\Band\\LP\\02 Other.flac\u0000"]!!.values[c("quality")]))
            val hi = byPath["a:\\Band\\Hi\\01 Hi.flac\u0000"]!!
            assertEquals("3", text(hi.values[c("quality")])); assertEquals(24L, hi.values[c("bit")]); assertEquals(96000L, hi.values[c("sample_rate")])
            assertEquals(1800000000L, hi.values[c("ctime")])              // new track: build time
            val nfc = byPath["a:\\Solo\\Canción.mp3\u0000"]!!                 // NFC path
            assertEquals("Canción\u0000", text(nfc.values[c("name")]))       // file name without extension
            assertEquals("1", text(nfc.values[c("quality")])); assertEquals(85L, nfc.values[c("format")]); assertEquals(128000L, nfc.values[c("bit_rate")])
            val junk = byPath["a:\\junk.ogg\u0000"]!!
            assertEquals("junk\u0000", text(junk.values[c("name")])); assertEquals("0", text(junk.values[c("quality")]))
            assertEquals(26447L, junk.values[c("format")])
            // stock order: letters, numbers, others
            val order = media.map { text(it.values[c("name")]).trimEnd('\u0000') }
            assertEquals(listOf("Canción", "Hi", "junk", "Other", "Root song", "Song", "Three", "100 Days", "¡Live!"), order)

            // catalogue tables
            val albums = r.table("ALBUM_TABLE").associateBy { text(it.values[1]) }
            assertEquals(setOf("Hi\u0000", "LP\u0000", "Live\u0000", "Singles\u0000", "Unknown\u0000"), albums.keys)
            val lp = albums["LP\u0000"]!!
            assertEquals(4L, lp.values[3])                               // LP and lp: one album of 4 tracks
            assertEquals(2L, lp.values[0])                               // id of its first track
            assertEquals(1700000000L, lp.values[4])
            assertEquals(5, r.table("ALBUM2_TABLE").size)
            assertEquals(r.table("ALBUM2_TABLE").map { it.values[0] as Long }, r.table("ALBUM2_TABLE").map { it.values[0] as Long }.sorted())
            assertEquals(setOf("Band\u0000", "Other Band\u0000", "Unknown\u0000"), r.table("ARTIST_TABLE").map { text(it.values[1]) }.toSet())
            assertEquals(setOf("Band\u0000"), r.table("ALBUM_ARTIST_TABLE").map { text(it.values[1]) }.toSet())
            assertEquals(listOf(9L, 5L, 3L, 4L, 1L), r.table("COUNT_TABLE").map { it.values[0] })
            assertEquals(setOf("FLAC", "MQA", "MP3", "M4A", "OGG"), r.table("FORMAT_TABLE").map { text(it.values[1]) }.toSet())
            assertEquals(9, r.table("CTIME_TABLE").size)
            assertEquals(0, r.table("SEARCH_TABLE").size)
            assertEquals(0, r.table("MEDIA3_TABLE").size)
            // root folder cache: folders and audio files of the SD root only
            val m2 = r.table("MEDIA2_TABLE").map { text(it.values[1]).trimEnd('\u0000') }.toSet()
            assertEquals(setOf("a:\\Band\\*", "a:\\Solo\\*", "a:\\Mix\\*", "a:\\Lists\\*", "a:\\Root.mp3", "a:\\junk.ogg"), m2)
            // playlists: device rows kept, new file added with a new id
            val m3u = r.table("M3U_TABLE")
            assertEquals(listOf(1L, 2L), m3u.map { it.values[0] })
            assertEquals("a:\\Lists\\new.m3u\u0000", text(m3u[1].values[1]))
            // kept tables: unchanged, except the history row of the renumbered track
            val hist = r.table("HISTORY_TABLE")
            assertEquals(listOf(12L, 13L), hist.map { it.rowid })
            assertEquals(threeId, hist[0].values[0])
            assertEquals(7L, hist[1].values[0])
            assertTrue(media.none { it.values[c("id")] == 7L })
        }
        val before = LibTestUtil.dump(baseFile).lines().filter { l -> listOf("COLLECT_TABLE|", "list_tb_1|", "MANAGER_TABLE|", "VERSION_TABLE|").any { l.startsWith(it) } }
        val after = LibTestUtil.dump(out).lines().filter { l -> listOf("COLLECT_TABLE|", "list_tb_1|", "MANAGER_TABLE|", "VERSION_TABLE|").any { l.startsWith(it) } }
        assertEquals(before, after)
        assertFalse(File(out.path + ".tmp").exists())

        assumeTrue("python with sqlite3 not found", LibTestUtil.python != null)
        assertEquals("ok", LibTestUtil.py(LibTestUtil.INTEGRITY, out.path).trim())
        // the stock player's typical queries work on the result
        val q = LibTestUtil.py("""
import sqlite3, sys
c = sqlite3.connect('file:' + sys.argv[1] + '?mode=ro', uri=True)
a = c.execute("select count(*) from MEDIA_TABLE where album='lp'||char(0)").fetchone()[0]
b = c.execute("select name from MEDIA_TABLE where character='S' order by rowid").fetchall()
d = c.execute("select id from MEDIA_TABLE where path=?", ('a:\\Band\\LP\\01 Song.flac\x00',)).fetchone()[0]
sys.stdout.write('%d|%s|%d' % (a, ','.join(x[0].rstrip('\x00') for x in b), d))
""", out.path)
        assertEquals("4|Song|2", q.trim())
    }

    /** A removed track's id is not reused, and a re-pointed row never duplicates a kept one. */
    @Test
    fun removedIdsAndDuplicateRows() {
        val dir = LibTestUtil.tempDir("builder-ids")
        val baseFile = File(dir, "base.db")
        val a = "a:\\A.flac"; val b = "a:\\B.flac"; val c = "a:\\C.flac"
        val data = mapOf(
            "MEDIA_TABLE" to listOf(track("MEDIA_TABLE", 1, 1, a, "A"), track("MEDIA_TABLE", 2, 2, b, "B"), track("MEDIA_TABLE", 3, 5, c, "C")),
            "HISTORY_TABLE" to listOf(track("HISTORY_TABLE", 1, 5, c, "C"), track("HISTORY_TABLE", 2, 3, c, "C"), track("HISTORY_TABLE", 3, 2, b, "B")),
            "COLLECT_TABLE" to listOf(track("COLLECT_TABLE", 1, 2, b, "B")),
            "VERSION_TABLE" to listOf(row("VERSION_TABLE", 1, "version_id" to 20181015L)),
        )
        SqliteWriter(baseFile).write(schema) { data[it] ?: emptyList() }
        val root = File(dir, "sd")
        SynthAudio.write(root, "A.flac", SynthAudio.flac(Tags("A")))
        SynthAudio.write(root, "C.flac", SynthAudio.flac(Tags("C")))
        SynthAudio.write(root, "D.flac", SynthAudio.flac(Tags("D")))
        val out = File(dir, "new.db")
        val rep = LibraryBuilder.build(baseFile, out, LibraryOptions(root, now = 1800000000))
        assertEquals(1, rep.renumbered)
        SqliteReader(out).use { r ->
            val mdef = defs["MEDIA_TABLE"]!!
            val ids = r.table("MEDIA_TABLE").associate { text(it.values[mdef.col("path")]).trimEnd('\u0000') to it.values[mdef.col("id")] as Long }
            assertEquals(1L, ids[a])            // kept
            assertEquals(3L, ids[c])            // 5 is outside 1..3: renumbered
            assertEquals(4L, ids["a:\\D.flac"]) // 2 was B's (removed, still in history and favourites): not reused
            // HISTORY: (5, C) would become (3, C), which already exists: dropped; B's rows keep id 2
            val hist = r.table("HISTORY_TABLE")
            assertEquals(listOf(2L, 3L), hist.map { it.rowid })
            assertEquals(listOf(3L, 2L), hist.map { it.values[0] })
            assertEquals(2L, r.table("COLLECT_TABLE").single().values[0])
        }
        assumeTrue("python with sqlite3 not found", LibTestUtil.python != null)
        assertEquals("ok", LibTestUtil.py(LibTestUtil.INTEGRITY, out.path).trim())
    }

    @Test
    fun refusesSomethingElse() {
        val dir = LibTestUtil.tempDir("builder-bad")
        val f = File(dir, "other.db")
        SqliteWriter(f).write(listOf(SchemaEntry("table", "t", "t", 1, "CREATE TABLE t(a INT)"))) { emptyList() }
        try {
            LibraryBuilder.build(f, File(dir, "o.db"), LibraryOptions(dir))
            throw AssertionError("must refuse")
        } catch (e: LibraryException) {
            assertTrue(e.message!!.contains("not a HiBy library"))
        }
    }

    /** Optional: rebuild against the real snapshot named by HIBYOS_LIBRARY_DB with synthetic music. */
    @Test
    fun realSnapshotKeepsUserTables() {
        val src = System.getenv("HIBYOS_LIBRARY_DB")
        assumeTrue("HIBYOS_LIBRARY_DB not set", !src.isNullOrEmpty())
        val dir = LibTestUtil.tempDir("builder-real")
        val root = File(dir, "sd"); music(root)
        val out = File(dir, "new.db")
        LibraryBuilder.build(File(src!!), out, LibraryOptions(root, now = 1800000000))
        assumeTrue(LibTestUtil.python != null)
        assertEquals("ok", LibTestUtil.py(LibTestUtil.INTEGRITY, out.path).trim())
        val keep = listOf("COLLECT_TABLE|", "list_tb_1|", "MANAGER_TABLE|", "VERSION_TABLE|", "PLAYLIST_TABLE|", "BOOK_TABLE|")
        assertEquals(LibTestUtil.dump(File(src)).lines().filter { l -> keep.any { l.startsWith(it) } },
            LibTestUtil.dump(out).lines().filter { l -> keep.any { l.startsWith(it) } })
    }
}
