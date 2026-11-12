package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.library.SchemaEntry
import org.hibyos.install.core.library.SchemaSql
import org.hibyos.install.core.library.SqlRow
import org.hibyos.install.core.library.SqlText
import org.hibyos.install.core.library.SqliteWriter
import org.hibyos.install.core.library.TagReader
import org.hibyos.install.core.testing.FakeDevice
import org.hibyos.install.core.testing.SynthAudio
import org.hibyos.install.core.testing.SynthAudio.Tags
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.awt.Color
import java.awt.image.BufferedImage
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.io.PrintStream
import java.io.RandomAccessFile
import javax.imageio.IIOImage
import javax.imageio.ImageIO
import javax.imageio.ImageWriteParam

/** The library and music-sync commands against the fake HiBy (docs/BYOS_CLI.md, "Library and music"). */
class LibraryCliTest {
    private lateinit var dir: File
    private lateinit var dev: FakeDevice
    private var port = 0
    private lateinit var cfg: File

    class Run(val code: Int, val lines: List<Map<String, Any?>>, val raw: String) {
        val last get() = lines.last()
        fun events(name: String) = lines.filter { it["event"] == name }
        override fun toString() = raw
    }

    @Before
    fun setUp() {
        dir = File(System.getProperty("java.io.tmpdir"), "byos-cli-libtest/${System.nanoTime()}")
        dir.mkdirs()
        dev = FakeDevice(File(dir, "sd"), File(dir, "data"))
        port = dev.startLocal()
        cfg = File(dir, "config")
    }

    @After
    fun tearDown() {
        dev.stop()
        dir.deleteRecursively()
    }

    private fun run(vararg args: String, stdin: InputStream = ByteArrayInputStream(ByteArray(0)), device: String? = "127.0.0.1:$port"): Run {
        val bout = ByteArrayOutputStream()
        val cli = Cli(stdin, PrintStream(bout, true, "UTF-8"), PrintStream(ByteArrayOutputStream()), emptyMap(),
            ProcessRunner { _, _ -> throw java.io.IOException("no programs in tests") }, retryDelaysMs = listOf(20, 20, 20), pollMs = 20)
        val all = arrayListOf("--config", cfg.path)
        if (device != null) all += listOf("--device", device)
        all += args
        val code = cli.run(all.toTypedArray())
        val text = bout.toString("UTF-8")
        val lines = text.lines().filter { it.isNotEmpty() }.map { Json.obj(Json.parse(it)) }
        assertEquals(text, "start", lines.first()["event"])
        assertEquals(text, 1, lines.count { it["event"] == "result" || it["event"] == "error" })
        assertEquals(text, if (code == 0) "result" else "error", lines.last()["event"])
        return Run(code, lines, text)
    }

    private fun sd(rel: String) = File(dev.sd, rel)

    private fun image(w: Int, h: Int, alpha: Boolean = false): BufferedImage {
        val img = BufferedImage(w, h, if (alpha) BufferedImage.TYPE_INT_ARGB else BufferedImage.TYPE_INT_RGB)
        val g = img.createGraphics()
        g.color = if (alpha) Color(255, 0, 0, 0) else Color(200, 40, 40); g.fillRect(0, 0, w, h)
        g.color = Color(30, 30, 200, 255); g.fillRect(w / 4, h / 4, w / 2, h / 2)
        g.dispose()
        return img
    }

    private fun jpeg(img: BufferedImage, progressive: Boolean = false): ByteArray {
        val wr = ImageIO.getImageWritersByFormatName("jpeg").next()
        val out = ByteArrayOutputStream()
        ImageIO.createImageOutputStream(out).use { ios ->
            wr.output = ios
            val p = wr.defaultWriteParam
            p.progressiveMode = if (progressive) ImageWriteParam.MODE_DEFAULT else ImageWriteParam.MODE_DISABLED
            wr.write(null, IIOImage(img, null, null), p)
        }
        wr.dispose()
        return out.toByteArray()
    }

    private fun png(img: BufferedImage) = ByteArrayOutputStream().also { ImageIO.write(img, "png", it) }.toByteArray()

    /** A FLAC whose embedded front cover is [cover]. */
    private fun flacWithCover(t: Tags, cover: ByteArray, audioBytes: Int = 20000): ByteArray {
        val plain = File(dir, "plain-${System.nanoTime()}.flac").also { it.writeBytes(SynthAudio.flac(t, audioBytes = audioBytes)) }
        val withArt = File(dir, "art-${System.nanoTime()}.flac")
        val i = CoverArt.info(cover)!!
        org.hibyos.install.core.library.PictureWriter.setCover(plain, withArt, cover, i.width, i.height)
        return withArt.readBytes().also { plain.delete(); withArt.delete() }
    }

    private fun embedded(f: File): ByteArray? {
        val p = TagReader.read(f).picture ?: return null
        return RandomAccessFile(f, "r").use { r -> ByteArray(p.length.toInt()).also { r.seek(p.offset); r.readFully(it) } }
    }

    private fun schema(): List<SchemaEntry> {
        val text = javaClass.getResource("/library/r3ii_2025_schema.sql")?.readText()
            ?: File(System.getenv("HIBYOS_REPO_HINT") ?: "../../..", "android/hibyos-install/core/src/test/resources/library/r3ii_2025_schema.sql").readText()
        val lines = text.lines().filter { it.isNotEmpty() }
        return lines.chunked(2).map { (h, s) -> val p = h.removePrefix("-- ").split(' '); SchemaEntry(p[0], p[1], p[2], 1, s.removeSuffix(";").ifEmpty { null }) }
    }

    // ------------------------------------------------------------------ CoverArt

    @Test
    fun coverArtMakesBaselineJpegs() {
        val big = jpeg(image(3000, 3000), progressive = true)
        val bi = CoverArt.info(big)!!
        assertEquals(3000, bi.width); assertTrue(bi.progressive)
        assertTrue(CoverArt.needsFix(bi))
        val r = CoverArt.prepare(big)!!
        assertEquals(500, r.width); assertEquals(500, r.height)
        val ri = CoverArt.info(r.bytes)!!
        assertFalse(ri.progressive); assertEquals("jpeg", ri.format); assertFalse(CoverArt.needsFix(ri))
        // transparent PNG: on white
        val p = CoverArt.prepare(png(image(400, 400, alpha = true)), 256)!!
        val img = ImageIO.read(ByteArrayInputStream(p.bytes))
        val corner = img.getRGB(2, 2)
        assertTrue("corner is white", ((corner shr 16) and 0xff) > 240 && ((corner shr 8) and 0xff) > 240 && (corner and 0xff) > 240)
        val center = img.getRGB(128, 128)
        assertTrue("centre stays blue", (center and 0xff) > 150 && ((center shr 16) and 0xff) < 90)
        // never enlarged; too small or too long is no cover (the HiBy's own rule)
        assertEquals(300, CoverArt.prepare(jpeg(image(300, 300)))!!.width)
        assertEquals(null, CoverArt.prepare(jpeg(image(100, 100))))
        assertEquals(null, CoverArt.prepare(jpeg(image(400, 100))))
        assertEquals(null, CoverArt.prepare(ByteArray(100)))
        assertNotNull(CoverArt.averageColor(r.bytes))
    }

    // ------------------------------------------------------------------ lib-index

    private fun liveDb() {
        val defs = schema().filter { it.type == "table" }.associate { it.name to SchemaSql.parseTable(it.sql!!) }
        fun z(s: String) = SqlText(s + "\u0000")
        fun media(id: Long, path: String, name: String, album: String, artist: String) = SqlRow(id, defs["MEDIA_TABLE"]!!.columns.map {
            mapOf("id" to id, "path" to z(path), "name" to z(name), "album" to z(album), "artist" to z(artist), "genre" to z("Rock"),
                "has_child_file" to 0L, "cue_id" to -1L, "ck_id" to id, "size" to 100L, "ctime" to 1L, "mtime" to 2L, "album_artist" to z(""))[it.name]
        })
        dev.liveDb.parentFile.mkdirs()
        SqliteWriter(dev.liveDb).write(schema()) {
            when (it) {
                "VERSION_TABLE" -> listOf(SqlRow(1, listOf(20181015L)))
                "MEDIA_TABLE" -> listOf(media(1, "a:\\Music\\B\\LP\\1.flac", "One", "LP", "B"), media(2, "a:\\Music\\B\\LP\\2.flac", "Two", "LP", "B"),
                    media(3, "a:\\Audiobooks\\Book\\1.mp3", "Ch 1", "Book", "Author"))
                else -> emptyList()
            }
        }
    }

    @Test
    fun libIndexFromTheSnapshot() {
        liveDb()
        val r = run("lib-index")
        assertEquals(r.raw, 0, r.code)
        assertEquals("snapshot", r.last["source"]); assertEquals(2L, r.last["tracks"]); assertEquals(1L, r.last["albums"]); assertEquals(1L, r.last["books"])
        val idx = Json.obj(Json.parse(File(r.last["index"] as String).readText()))
        assertEquals(3, Json.arr(idx["tracks"]).size)
        assertTrue((r.last["index"] as String).contains(r.last["device"] as String))
        assertTrue(r.events("progress").any { it["op"] == "pull" })
        // from a file, no HiBy
        val f = run("lib-index", "--db", dev.liveDb.path, "--out", File(dir, "i.json").path, device = null)
        assertEquals(f.raw, 0, f.code); assertTrue(File(dir, "i.json").isFile)
    }

    @Test
    fun libIndexSaysWhenTheSnapshotIsMissing() {
        val r = run("lib-index")
        assertEquals(4, r.code)
        assertEquals("scan", r.last["hint"])
        assertEquals("E_IO", (r.last["btp"] as Map<*, *>)["name"])
    }

    // ------------------------------------------------------------------ lib-scan

    @Test
    fun libScanReadsOnlyTheTagsAndCaches() {
        SynthAudio.write(dev.sd, "Music/Band/LP/01 One.flac", SynthAudio.flac(Tags("One", "Band", "LP", track = "1"), audioBytes = 3_000_000))
        SynthAudio.write(dev.sd, "Music/Band/LP/02 Two.mp3", SynthAudio.mp3(Tags("Two", "Band", "LP", track = "2")))
        SynthAudio.write(dev.sd, "Music/Band/Live/x.m4a", SynthAudio.m4a(Tags("Live", "Band", "Live")))
        SynthAudio.write(dev.sd, "Music/Band/LP/cover.jpg", ByteArray(10))
        SynthAudio.write(dev.sd, "Playlists/Mix.m3u8", "#EXTM3U\r\n../Music/Band/LP/01 One.flac\r\n".toByteArray())
        SynthAudio.write(dev.sd, "ByOS/apps/ignored.mp3", ByteArray(10))
        val r = run("lib-scan")
        assertEquals(r.raw, 0, r.code)
        assertEquals("scan", r.last["source"]); assertEquals(3L, r.last["tracks"]); assertEquals(2L, r.last["albums"]); assertEquals(1L, r.last["playlists"])
        assertEquals(3L, r.last["fetched"]); assertEquals(0L, r.last["cached"])
        assertTrue("only the tags were read: ${r.last["bytes"]}", (r.last["bytes"] as Long) < 600_000)
        assertTrue(r.events("progress").any { it["op"] == "tags" })
        val again = run("lib-scan", "Music")
        assertEquals(3L, again.last["cached"]); assertEquals(0L, again.last["fetched"])
        val full = run("lib-scan", "--full")
        assertEquals(3L, full.last["fetched"])
    }

    // ------------------------------------------------------------------ lib-art

    @Test
    fun libArtMakesThumbnails() {
        SynthAudio.write(dev.sd, "Music/A/1.flac", flacWithCover(Tags("One", "A", "X"), jpeg(image(800, 800), progressive = true)))
        SynthAudio.write(dev.sd, "Music/B/1.flac", SynthAudio.flac(Tags("One", "B", "Y")))
        sd("Music/B/folder.png").writeBytes(png(image(300, 300)))
        SynthAudio.write(dev.sd, "Music/C/1.flac", SynthAudio.flac(Tags("One", "C", "Z")))
        val r = run("lib-art", "Music/A/1.flac", "Music/B/1.flac", "Music/C/1.flac", "Music/D/missing.flac")
        assertEquals(r.raw, 0, r.code)
        val items = Json.arr(r.last["items"]).map { Json.obj(it) }.associateBy { it["path"] }
        for (p in listOf("Music/A/1.flac", "Music/B/1.flac")) {
            val f = File(items[p]!!["file"] as String)
            assertTrue(f.isFile)
            val i = CoverArt.info(f.readBytes())!!
            assertEquals(p, 256, maxOf(i.width, i.height))
            assertTrue((items[p]!!["key"] as String).matches(Regex("[0-9a-f]{40}-256")))
            assertTrue((items[p]!!["color"] as String).startsWith("#"))
        }
        assertEquals(true, items["Music/C/1.flac"]!!["none"])
        assertEquals(true, items["Music/D/missing.flac"]!!["none"])
        assertEquals(1, run("lib-art", "--size", "100", "x.flac").code)
    }

    // ------------------------------------------------------------------ rescan, rm, cover cache

    @Test
    fun rescanPassesUnsupportedThrough() {
        val ok = run("lib-rescan", "Music/a.flac")
        assertEquals(ok.raw, 0, ok.code); assertEquals(1, dev.rescans)
        dev.shellAttached = false
        val full = run("lib-rescan", "--full")
        assertEquals(4, full.code)
        assertEquals("E_UNSUPPORTED", (full.last["btp"] as Map<*, *>)["name"])
        assertEquals(1, run("lib-rescan").code)
    }

    @Test
    fun rmPrunesEmptyFolders() {
        SynthAudio.write(dev.sd, "Music/A/B/x.flac", ByteArray(10))
        SynthAudio.write(dev.sd, "Music/A/keep.txt", ByteArray(1))
        SynthAudio.write(dev.sd, "Music/Z/y.flac", ByteArray(10))
        val r = run("rm", "--yes", "--prune-empty", "--", "Music/A/B/x.flac", "Music/Z/y.flac")
        assertEquals(r.raw, 0, r.code)
        assertEquals(setOf("Music/A/B", "Music/Z"), Json.arr(r.last["pruned"]).toSet())
        assertTrue(sd("Music/A").isDirectory); assertTrue(sd("Music").isDirectory)
        assertFalse(sd("Music/Z").exists())
    }

    @Test
    fun coverCacheResetTouchesOnlyTheCache() {
        for (n in listOf("k1.none", "k1_b.raw", "k1_s.raw", "k2_b.raw")) SynthAudio.write(dev.sd, "ByOS/cache/music/$n", ByteArray(4))
        SynthAudio.write(dev.sd, "Music/a_b.raw", ByteArray(4))
        val r = run("cover-cache-reset")
        assertEquals(r.raw, 0, r.code)
        assertEquals(3L, r.last["deleted"])
        assertTrue(sd("ByOS/cache/music/k1_s.raw").isFile); assertTrue(sd("Music/a_b.raw").isFile)
        assertEquals(0L, run("cover-cache-reset").last["deleted"])
    }

    // ------------------------------------------------------------------ playlists

    @Test
    fun playlists() {
        assertEquals(1, run("playlist-add", "Road trip", "Music/a.flac").code) // no --create
        val add = run("playlist-add", "--create", "--", "Road trip", "Music/a.flac", "Music/b.flac")
        assertEquals(add.raw, 0, add.code)
        assertEquals("#EXTM3U\r\n../Music/a.flac\r\n../Music/b.flac\r\n", sd("Playlists/Road trip.m3u8").readText())
        assertEquals(1L, run("playlist-add", "Road trip", "Music/c.flac", "Music/A.FLAC").last["added"])
        val ls = run("playlist-ls")
        val e = ls.events("entry").single()
        assertEquals("Road trip", e["name"]); assertEquals(3L, e["tracks"])
        assertEquals(1L, run("playlist-rm", "Road trip", "Music/b.flac").last["removed"])
        assertEquals("#EXTM3U\r\n../Music/a.flac\r\n../Music/c.flac\r\n", sd("Playlists/Road trip.m3u8").readText())
        assertEquals(true, run("playlist-rm", "Road trip").last["deleted"])
        assertFalse(sd("Playlists/Road trip.m3u8").exists())
    }

    // ------------------------------------------------------------------ music-scan, sync-plan, sync-apply

    @Test
    fun syncFromThisComputer() {
        val music = File(dir, "My Music")
        SynthAudio.write(music, "ABBA/Arrival/01.flac", flacWithCover(Tags("Dancing Queen", "ABBA", "Arrival", track = "1", genre = "Pop"), jpeg(image(1600, 1600), progressive = true)))
        SynthAudio.write(music, "ABBA/Arrival/02.mp3", SynthAudio.mp3(Tags("Money", "ABBA", "Arrival", track = "2", genre = "Pop")))
        File(music, "ABBA/Arrival/cover.png").writeBytes(png(image(700, 700)))
        File(music, "ABBA/Arrival/02.lrc").writeText("[00:01]Money")
        SynthAudio.write(music, "Other/x.flac", SynthAudio.flac(Tags("X", "Other", "Y")))
        val scan = run("music-scan", music.path, device = null)
        assertEquals(scan.raw, 0, scan.code)
        assertEquals(3L, scan.last["tracks"]); assertEquals(3L, scan.last["changed"])
        val catalog = scan.last["catalog"] as String
        assertEquals(0L, run("music-scan", music.path, device = null).last["changed"])

        val plan = run("sync-plan", "--catalog", catalog, "--dest", "Music", "--covers", "optimize")
        assertEquals(plan.raw, 0, plan.code)
        assertEquals(3L, plan.last["add"]); assertEquals(true, plan.last["fits"])
        val apply = run("sync-apply", plan.last["plan"] as String, "--lyrics")
        assertEquals(apply.raw, 0, apply.code)
        assertEquals(3L, apply.last["written"]); assertEquals(0L, apply.last["failed"])
        assertTrue(apply.events("progress").any { it["op"] == "sync" && it["files_total"] == 3L })
        // the progressive 1600 px cover became a 500 px baseline JPEG; the MP3 without art got the folder's image
        val flac = sd("Music/ABBA/Arrival/01 Dancing Queen.flac")
        val art = CoverArt.info(embedded(flac)!!)!!
        assertEquals(500, art.width); assertFalse(art.progressive)
        assertEquals(500, CoverArt.info(embedded(sd("Music/ABBA/Arrival/02 Money.mp3"))!!)!!.width)
        assertEquals("[00:01]Money", sd("Music/ABBA/Arrival/02 Money.lrc").readText())
        // the originals on this computer are untouched
        assertEquals(1600, CoverArt.info(embedded(File(music, "ABBA/Arrival/01.flac"))!!)!!.width)
        assertTrue(sd(LibraryCommands.MANIFEST).isFile)
        // a second sync copies nothing
        val again = run("sync-plan", "--catalog", catalog, "--dest", "Music")
        assertEquals(0L, again.last["add"]); assertEquals(0L, again.last["update"]); assertEquals(3L, again.last["skip"])

        // only ABBA now, removing the rest: asks first
        val sel = File(dir, "sel.json").also { it.writeText("{\"all\":false,\"artists\":[\"abba\"]}") }
        val p2 = run("sync-plan", "--catalog", catalog, "--select", sel.path, "--dest", "Music", "--remove-unselected")
        assertEquals(1L, p2.last["remove"])
        val no = run("sync-apply", p2.last["plan"] as String, stdin = ByteArrayInputStream("{\"answer\":false}\n".toByteArray()))
        assertEquals("cancelled", no.last["code"]); assertTrue(sd("Music/Other/Y/X.flac").isFile)
        dev.shellAttached = false
        val yes = run("sync-apply", p2.last["plan"] as String, "--yes", "--then", "rescan")
        assertEquals(yes.raw, 0, yes.code)
        assertEquals(1L, yes.last["removed"])
        assertFalse(sd("Music/Other").exists())
        val lib = yes.last["library"] as Map<*, *>
        assertEquals(false, lib["ok"]); assertEquals("E_UNSUPPORTED", lib["code"])
        assertTrue(yes.events("warn").any { it["code"] == "library_pending" })
        assertFalse(sd(LibraryCommands.MANIFEST).readText().contains("Other"))
    }

    // ------------------------------------------------------------------ art-fix

    @Test
    fun artFixShrinksLargeCoversOnTheHiBy() {
        SynthAudio.write(dev.sd, "Music/A/1.flac", flacWithCover(Tags("One", "A", "X"), jpeg(image(1500, 1500))))
        SynthAudio.write(dev.sd, "Music/B/1.flac", flacWithCover(Tags("One", "B", "Y"), jpeg(image(400, 400))))
        SynthAudio.write(dev.sd, "ByOS/cache/music/old.none", ByteArray(1))
        val dry = run("art-fix", "--dry-run")
        assertEquals(dry.raw, 0, dry.code)
        assertEquals(1L, dry.last["files"]); assertEquals(1L, dry.last["albums"])
        assertEquals("Music/A", dry.events("entry").single()["path"])
        assertEquals(1500, CoverArt.info(embedded(sd("Music/A/1.flac"))!!)!!.width)
        val fix = run("art-fix")
        assertEquals(fix.raw, 0, fix.code)
        assertEquals(1L, fix.last["fixed"])
        assertEquals(500, CoverArt.info(embedded(sd("Music/A/1.flac"))!!)!!.width)
        assertEquals("One", TagReader.read(sd("Music/A/1.flac")).title)
        assertFalse(sd("ByOS/cache/music/old.none").exists())
    }
}
