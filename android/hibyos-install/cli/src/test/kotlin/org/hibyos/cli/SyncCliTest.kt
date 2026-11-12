package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.library.PictureWriter
import org.hibyos.install.core.sync.Kinds
import org.hibyos.install.core.sync.LocalScanner
import org.hibyos.install.core.testing.FakeDevice
import org.hibyos.install.core.testing.SynthAudio
import org.hibyos.install.core.testing.SynthAudio.Tags
import org.junit.After
import org.junit.Assert.assertArrayEquals
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
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import javax.imageio.IIOImage
import javax.imageio.ImageIO
import javax.imageio.ImageWriteParam

/**
 * The linked-folder sync (docs/SYNC_LIBRARY_DESIGN.md M1 and M2) end to end against the fake
 * HiBy: links, scan, plan, apply, resume, status and unlock, with the fault cases of section 3.5.
 */
class SyncCliTest {
    private lateinit var dir: File
    private lateinit var dev: FakeDevice
    private var port = 0
    private lateinit var cfg: File

    class Run(val code: Int, val lines: List<Map<String, Any?>>, val raw: String) {
        val last get() = lines.last()
        fun events(name: String) = lines.filter { it["event"] == name }
        fun warns(code: String) = lines.filter { it["event"] == "warn" && it["code"] == code }
        override fun toString() = raw
    }

    /** Thrown by a test hook: stops a run in the middle, as a killed process would. */
    class SimulatedKill : Error("killed")

    @Before
    fun setUp() {
        dir = File(System.getProperty("java.io.tmpdir"), "byos-cli-synctest/${System.nanoTime()}")
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

    private fun run(vararg args: String, stdin: InputStream = ByteArrayInputStream(ByteArray(0)), hook: ((Int) -> Unit)? = null,
                    checkpointEvery: Int = 200, config: File = cfg): Run {
        val bout = ByteArrayOutputStream()
        val cli = Cli(stdin, PrintStream(bout, true, "UTF-8"), PrintStream(ByteArrayOutputStream()), emptyMap(),
            ProcessRunner { _, _ -> throw java.io.IOException("no programs in tests") }, retryDelaysMs = listOf(20, 20, 20), pollMs = 20,
            checkpointEvery = checkpointEvery, syncHook = hook)
        val code = cli.run((listOf("--config", config.path, "--device", "127.0.0.1:$port", "--name", "Test PC") + args).toTypedArray())
        val text = bout.toString("UTF-8")
        val lines = text.lines().filter { it.isNotEmpty() }.map { Json.obj(Json.parse(it)) }
        assertEquals(text, "start", lines.first()["event"])
        assertEquals(text, 1, lines.count { it["event"] == "result" || it["event"] == "error" })
        return Run(code, lines, text)
    }

    private fun ok(r: Run): Map<String, Any?> { assertEquals(r.raw, 0, r.code); return r.last }

    private fun sd(rel: String) = File(dev.sd, rel)

    private val devId get() = Config.hexOf(dev.deviceId)

    private fun flac(root: File, rel: String, artist: String, album: String, title: String, n: Int, bytes: Int = 2000): File {
        val f = SynthAudio.write(root, rel, SynthAudio.flac(Tags(title, artist, album, track = n.toString()), audioBytes = bytes))
        f.setLastModified(1_700_000_000_000L + n * 1000L)
        return File(root, rel)
    }

    /** A music folder: ABBA/Gold with [n] tracks and Queen/Hits with 2. */
    private fun library(n: Int = 4, bytes: Int = 2000): File {
        val m = File(dir, "Music PC")
        for (i in 1..n) flac(m, "ABBA/Gold/$i.flac", "ABBA", "Gold", "Song $i", i, bytes)
        for (i in 1..2) flac(m, "Queen/Hits/$i.flac", "Queen", "Hits", "Hit $i", i, bytes)
        return m
    }

    private fun addLink(kind: String, local: File, vararg opts: String): String =
        (ok(run("link-add", "--kind", kind, *opts, local.path))["link"] as Map<*, *>)["id"] as String

    private fun plan(vararg opts: String): Map<String, Any?> = ok(run("sync-plan", *opts))

    @Suppress("UNCHECKED_CAST")
    private fun planItems(p: Map<String, Any?>): List<Map<String, Any?>> =
        Json.arr(Json.obj(Json.parse(File(p["plan"] as String).readText()))["items"]).map { it as Map<String, Any?> }

    @Suppress("UNCHECKED_CAST")
    private fun summary(p: Map<String, Any?>, kind: String = "music") = (p["summary"] as Map<String, Any?>)[kind] as Map<String, Any?>

    private fun cardFiles(root: String) = sd(root).walkTopDown().filter { it.isFile }.map { it.relativeTo(dev.sd).path.replace('\\', '/') }.sorted().toList()

    // ------------------------------------------------------------------ links and a first sync

    @Test
    fun linksAndAFirstSync() {
        val m = library()
        val id = addLink("music", m)
        val ls = ok(run("link-ls"))
        assertEquals(1L, ls["count"])
        val l = (ls["links"] as List<*>)[0] as Map<*, *>
        assertEquals("Music", l["card_root"]); assertEquals("artist-album", l["layout"]); assertEquals("keep", l["player_only"])
        // a second music link of the same folder is refused; another folder goes under Music/<label>
        val dup = run("link-add", "--kind", "music", m.path)
        assertEquals(dup.raw, 1, dup.code)
        val other = File(dir, "Other").also { flac(it, "x.flac", "X", "Y", "Z", 1) }
        val id2 = addLink("music", other, "--layout", "keep", "--label", "Live")
        assertEquals("Music/Live", ((ok(run("link-ls"))["links"] as List<*>)[1] as Map<*, *>)["card_root"])
        ok(run("link-rm", id2))
        assertEquals(1L, ok(run("link-ls"))["count"])

        val scan = ok(run("sync-scan"))
        assertEquals(6L, ((scan["links"] as List<*>)[0] as Map<*, *>)["files"])
        val p = plan("--out", File(dir, "first-plan.json").path)
        assertEquals(6L, summary(p)["add"])
        val apply = ok(run("sync-apply", p["plan"] as String))
        assertEquals("done", apply["status"])
        assertEquals(6L, ((apply["kinds"] as Map<*, *>)["music"] as Map<*, *>)["written"])
        assertTrue(sd("Music/ABBA/Gold/01 Song 1.flac").isFile)
        assertTrue(sd("Music/Queen/Hits/02 Hit 2.flac").isFile)
        assertTrue(sd("ByOS/sync/manifest-v2/music.json").isFile)
        assertTrue(sd("ByOS/sync/card.json").isFile)
        assertFalse("lock released", sd("ByOS/sync/lock.json").exists())
        assertTrue(File(cfg, "sync/$devId/$id/state.json").isFile)
        assertTrue("checkpoints folded", sd("ByOS/sync/journal").list().isNullOrEmpty())
        // a second plan copies nothing
        val again = plan()
        assertEquals(0L, summary(again)["add"]); assertEquals(6L, summary(again)["skip"]); assertEquals(0L, again["items"])
        // applying the same plan twice is refused
        val twice = run("sync-apply", p["plan"] as String)
        assertEquals(twice.raw, 1, twice.code)
    }

    @Test
    fun relinkingTheFolderElsewhereCopiesNothing() {
        val m = library()
        val id = addLink("music", m)
        ok(run("sync-apply", plan()["plan"] as String))
        // the music moved from D:\ to E:\ (the copy keeps the files' times)
        val moved = File(dir, "Elsewhere/Music")
        for (f in m.walkTopDown().filter { it.isFile }) {
            val t = File(moved, f.relativeTo(m).path); t.parentFile.mkdirs()
            Files.copy(f.toPath(), t.toPath(), StandardCopyOption.COPY_ATTRIBUTES)
        }
        m.deleteRecursively()
        ok(run("link-set", id, "--local", moved.path))
        val p = plan()
        assertEquals(0L, summary(p)["add"]); assertEquals(0L, summary(p)["update"]); assertEquals(6L, summary(p)["skip"])
        assertEquals(0L, (summary(p)["player_only"] as Map<*, *>)["deleted_on_computer"])
    }

    @Test
    fun sameSizeRetagAndAlbumFix() {
        val m = library(2)
        addLink("music", m)
        ok(run("sync-apply", plan()["plan"] as String))
        // a retag that keeps the size: the same bytes count, other text
        val f = File(m, "ABBA/Gold/1.flac")
        val before = f.length()
        f.writeBytes(SynthAudio.flac(Tags("Song X", "ABBA", "Gold", track = "1"), audioBytes = 2000))
        f.setLastModified(1_800_000_000_000L)
        assertEquals(before, f.length())
        // an album name fix on the other track
        File(m, "ABBA/Gold/2.flac").writeBytes(SynthAudio.flac(Tags("Song 2", "ABBA", "Gold (Remaster)", track = "2"), audioBytes = 2000))
        val p = plan()
        val items = planItems(p)
        val upd = items.single { it["src_rel"] == "ABBA/Gold/1.flac" }
        // the title changed, so the name on the card changes too: a move that also sends the new content
        assertEquals("move", upd["action"]); assertEquals(true, upd["then_update"])
        val mv = items.single { it["src_rel"] == "ABBA/Gold/2.flac" }
        assertEquals("move", mv["action"]); assertEquals("Music/ABBA/Gold (Remaster)/02 Song 2.flac", mv["dest"])
        val r = ok(run("sync-apply", p["plan"] as String))
        assertEquals("done", r["status"])
        assertTrue(sd("Music/ABBA/Gold/01 Song X.flac").isFile)
        assertArrayEquals(f.readBytes(), sd("Music/ABBA/Gold/01 Song X.flac").readBytes())
        assertTrue(sd("Music/ABBA/Gold (Remaster)/02 Song 2.flac").isFile)
        assertFalse(sd("Music/ABBA/Gold/02 Song 2.flac").exists())
        assertEquals(0L, plan()["items"])
    }

    // ------------------------------------------------------------------ interruptions

    @Test
    fun aKillAfterHalfThenReplanMakesNoDuplicatesAndNoRecopy() {
        val m = File(dir, "M")
        for (i in 1..10) flac(m, "A/$i.flac", "Band", "Album", "T$i", i)
        addLink("music", m)
        val p = plan()
        val killed = run("sync-apply", p["plan"] as String, hook = { n -> if (n == 5) throw SimulatedKill() })
        assertEquals(killed.raw, 10, killed.code)
        assertEquals(5, musicPuts())
        assertFalse("no manifest fold after a kill", sd("ByOS/sync/manifest-v2/music.json").exists())
        // a killed process leaves its lock behind; the same computer takes it over
        sd("ByOS/sync/lock.json").writeText(sd("ByOS/sync/lock.json").takeIf { it.isFile }?.readText()
            ?: "{\"client_id\":\"${clientId()}\",\"name\":\"Test PC\",\"run_id\":\"x\",\"kinds\":[\"music\"],\"started\":0,\"heartbeat\":${System.currentTimeMillis() / 1000}}")
        val again = run("sync-plan")
        assertTrue(again.warns("journal_replayed").isNotEmpty())
        assertEquals(5L, summary(ok(again))["add"]); assertEquals(5L, summary(again.last)["skip"])
        assertTrue(planItems(again.last).none { (it["dest"] as String).contains(" (2)") })
        val r = ok(run("sync-apply", again.last["plan"] as String))
        assertEquals("done", r["status"])
        assertEquals("nothing was copied twice", 10, musicPuts())
        val files = cardFiles("Music")
        assertEquals(files.toString(), 10, files.size)
        assertTrue(files.none { it.contains("(2)") })
        assertEquals(0L, plan()["items"])
        // the killed run's journal is closed by the run that folded it
        assertEquals(0L, (ok(run("sync-status"))["runs"] as List<*>).size.toLong())
    }

    /** Uploads into Music/ the fake HiBy started (resumed ones count again). */
    private fun musicPuts() = dev.log.count { it.startsWith("PUT_READY Music/") }

    private fun clientId(): String = Json.str(Json.obj(Json.parse(File(cfg, "sync/client.json").readText())), "client_id")

    @Test
    fun cardCheckpointsCoverALostComputerJournal() {
        val m = File(dir, "M")
        for (i in 1..10) flac(m, "A/$i.flac", "Band", "Album", "T$i", i)
        addLink("music", m)
        val p = plan()
        val killed = run("sync-apply", p["plan"] as String, hook = { n -> if (n == 5) throw SimulatedKill() }, checkpointEvery = 2)
        assertEquals(10, killed.code)
        assertEquals(2, sd("ByOS/sync/journal").list()!!.size)
        // this computer lost its journal (another PC, a reinstall): the card checkpoints still count
        File(cfg, "sync/$devId/runs").deleteRecursively()
        val again = plan()
        val s = summary(again)
        assertEquals(5L, s["add"]); assertEquals(4L, s["skip"])
        // the fifth file reached the card after the last checkpoint: same size and fingerprint, so adopted
        assertEquals(1L, s["adopt"])
        assertTrue(planItems(again).none { (it["dest"] as String).contains(" (2)") })
        ok(run("sync-apply", again["plan"] as String))
        assertEquals(10, cardFiles("Music").size)
        assertEquals(0L, plan()["items"])
    }

    private fun image(w: Int, h: Int): BufferedImage {
        val img = BufferedImage(w, h, BufferedImage.TYPE_INT_RGB)
        val g = img.createGraphics(); g.color = Color(200, 40, 40); g.fillRect(0, 0, w, h)
        g.color = Color(30, 30, 200); g.fillRect(w / 4, h / 4, w / 2, h / 2); g.dispose()
        return img
    }

    private fun progressiveJpeg(img: BufferedImage): ByteArray {
        val wr = ImageIO.getImageWritersByFormatName("jpeg").next()
        val out = ByteArrayOutputStream()
        ImageIO.createImageOutputStream(out).use { ios ->
            wr.output = ios
            val p = wr.defaultWriteParam
            p.progressiveMode = ImageWriteParam.MODE_DEFAULT
            wr.write(null, IIOImage(img, null, null), p)
        }
        wr.dispose()
        return out.toByteArray()
    }

    @Test
    fun aLostLinkPausesAndResumeContinuesThePreparedPart() {
        val m = File(dir, "M")
        // big files with a 1600 px progressive cover: sent as prepared copies (500 px baseline)
        val cover = progressiveJpeg(image(1600, 1600))
        for (i in 1..3) {
            val plain = flac(m, "A/plain$i.flac", "Band", "Album", "T$i", i, bytes = 400_000)
            val withArt = File(m, "A/$i.flac")
            val ci = CoverArt.info(cover)!!
            PictureWriter.setCover(plain, withArt, cover, ci.width, ci.height)
            withArt.setLastModified(1_700_000_000_000L + i * 1000L)
            plain.delete()
        }
        addLink("music", m)
        val p = plan()
        // after the first file, the link drops in the middle of the second one and the HiBy stays away
        val r = run("sync-apply", p["plan"] as String, hook = { n -> if (n == 1) { dev.dropPutAfter = 200_000; dev.refuseConnections = true } })
        assertEquals("paused", ok(r)["status"]); assertEquals(true, r.last["resumable"])
        assertTrue(r.warns("reconnecting").isNotEmpty())
        val prepared = File(cfg, "cache/sync/prepared").listFiles()!!.filter { it.name.endsWith(".flac") }
        assertEquals("the prepared copy of the cut file is kept", 1, prepared.size)
        dev.refuseConnections = false
        val status = ok(run("sync-status"))
        assertEquals("paused", ((status["runs"] as List<*>)[0] as Map<*, *>)["status"])
        dev.log.clear()
        val res = ok(run("sync-resume"))
        assertEquals("done", res["status"]); assertEquals(true, res["resumed"])
        // the same prepared bytes: the part file was continued, not restarted
        assertTrue(dev.log.toString(), dev.log.any { it.startsWith("PUT_READY") && !it.endsWith(" 0") })
        assertEquals(3, cardFiles("Music").size)
        assertEquals(500, CoverArt.info(embedded(sd("Music/Band/Album/02 T2.flac")))!!.width)
        assertTrue(File(cfg, "cache/sync/prepared").listFiles()!!.none { it.name.endsWith(".flac") })
        assertEquals(0L, plan()["items"])
    }

    private fun embedded(f: File): ByteArray {
        val p = org.hibyos.install.core.library.TagReader.read(f).picture!!
        return java.io.RandomAccessFile(f, "r").use { r -> ByteArray(p.length.toInt()).also { r.seek(p.offset); r.readFully(it) } }
    }

    @Test
    fun aDroppedConnectionResumesWithinTheRun() {
        val m = File(dir, "M")
        flac(m, "A/1.flac", "Band", "Album", "T1", 1, bytes = 400_000)
        addLink("music", m)
        dev.dropPutAfter = 200_000
        val r = run("sync-apply", plan()["plan"] as String)
        assertEquals("done", ok(r)["status"])
        assertTrue(r.warns("reconnecting").isNotEmpty())
        assertArrayEquals(File(m, "A/1.flac").readBytes(), sd("Music/Band/Album/01 T1.flac").readBytes())
    }

    @Test
    fun usbStorageModePausesAndResumes() {
        val m = File(dir, "M")
        for (i in 1..6) flac(m, "A/$i.flac", "Band", "Album", "T$i", i)
        addLink("music", m)
        dev.sdUnavailableAfterPuts = 3
        val r = run("sync-apply", plan()["plan"] as String)
        assertEquals("paused_usb_storage", ok(r)["status"])
        assertTrue(r.warns("paused_usb_storage").isNotEmpty())
        dev.sdState = 0
        val res = ok(run("sync-resume"))
        assertEquals("done", res["status"])
        assertEquals(6, cardFiles("Music").size)
        assertEquals(0L, plan()["items"])
    }

    @Test
    fun aFullCardStopsTheCopiesAndKeepsWhatFitted() {
        val m = File(dir, "M")
        for (i in 1..5) flac(m, "A/$i.flac", "Band", "Album", "T$i", i, bytes = 3000)
        addLink("music", m)
        val p = plan()
        dev.spaceLeft = File(m, "A/1.flac").length() * 2 + 100   // ByOS/ files (lock, manifest) are not counted by the fake
        val r = run("sync-apply", p["plan"] as String)
        val res = ok(r)
        assertEquals("done_with_problems", res["status"])
        val k = (res["kinds"] as Map<*, *>)["music"] as Map<*, *>
        assertEquals(2L, k["written"]); assertEquals(3L, k["not_fitted"])
        assertTrue(r.warns("no_space").isNotEmpty())
        // what fitted is recorded: the next plan only adds the rest
        dev.spaceLeft = -1
        assertEquals(3L, summary(plan())["add"])
    }

    @Test
    fun aNearlyFullCardMovesTheOldCopyAsideBeforeReplacing() {
        val m = File(dir, "M")
        flac(m, "A/1.flac", "Band", "Album", "T1", 1, bytes = 5000)
        addLink("music", m)
        ok(run("sync-apply", plan()["plan"] as String))
        // the file changes on this computer while the card is nearly full
        File(m, "A/1.flac").writeBytes(SynthAudio.flac(Tags("T1", "Band", "Album", track = "1"), audioBytes = 9000))
        dev.sdFree = 1_000_000
        val p = plan()
        assertEquals(1L, summary(p)["update"])
        dev.log.clear()
        val r = ok(run("sync-apply", p["plan"] as String))
        assertEquals("done", r["status"])
        // MOVE to ByOS/sync/trash, PUT CREATE, then the old copy is deleted
        val i = dev.log.indexOf("MOVE")
        assertTrue(dev.log.toString(), i >= 0 && dev.log.drop(i).any { it.startsWith("PUT_READY Music/") })
        assertArrayEquals(File(m, "A/1.flac").readBytes(), sd("Music/Band/Album/01 T1.flac").readBytes())
        assertFalse(sd("ByOS/sync/trash").exists() && sd("ByOS/sync/trash").walkTopDown().any { it.isFile })
        assertEquals(0L, plan()["items"])
    }

    @Test
    fun aSecondClientSeesTheLockAndUnlockNeedsForce() {
        val m = library(1)
        addLink("music", m)
        val p = plan()
        val now = System.currentTimeMillis() / 1000
        sd("ByOS/sync").mkdirs()
        sd("ByOS/sync/lock.json").writeText("{\"client_id\":\"other\",\"name\":\"Laptop\",\"run_id\":\"r1\",\"kinds\":[\"music\"],\"started\":$now,\"heartbeat\":$now}")
        val r = run("sync-apply", p["plan"] as String)
        assertEquals(r.raw, 11, r.code)
        assertEquals("locked", r.last["code"])
        assertEquals("Laptop", (r.last["locked_by"] as Map<*, *>)["name"])
        assertTrue(cardFiles("Music").isEmpty())
        val st = ok(run("sync-status"))
        val lock = st["lock"] as Map<*, *>
        assertEquals(true, lock["held"]); assertEquals(false, lock["ours"]); assertEquals(false, lock["stale"])
        assertEquals(11, run("sync-unlock").code)
        assertEquals(true, ok(run("sync-unlock", "--force"))["unlocked"])
        // a stale lock (no heartbeat for 10 minutes) is taken over
        sd("ByOS/sync/lock.json").writeText("{\"client_id\":\"other\",\"name\":\"Laptop\",\"run_id\":\"r1\",\"kinds\":[\"music\"],\"started\":0,\"heartbeat\":${now - 700}}")
        val t = run("sync-apply", p["plan"] as String)
        assertEquals("done", ok(t)["status"])
        assertTrue(t.warns("lock_taken_over").isNotEmpty())
    }

    @Test
    fun lowBatteryIsRefused() {
        val m = library(1)
        addLink("music", m)
        val p = plan()
        dev.battery = 10; dev.charging = false
        val r = run("sync-apply", p["plan"] as String)
        assertEquals(4, r.code); assertEquals("E_BATTERY", (r.last["btp"] as Map<*, *>)["name"])
        dev.charging = true
        ok(run("sync-apply", p["plan"] as String))
    }

    // ------------------------------------------------------------------ v1 migration

    @Test
    fun theBranchManifestIsMigratedWithoutCopying() {
        val m = library()
        val catalog = ok(run("music-scan", m.path))["catalog"] as String
        val legacy = ok(run("sync-plan", "--catalog", catalog, "--dest", "Music"))
        ok(run("sync-apply", legacy["plan"] as String))
        assertTrue(sd("ByOS/sync/manifest.json").isFile)
        assertEquals(6, musicPuts())
        addLink("music", m)
        val r = run("sync-plan")
        assertTrue(r.warns("manifest_migrated").isNotEmpty())
        val s = summary(ok(r))
        assertEquals(0L, s["add"]); assertEquals(0L, s["update"]); assertEquals(6L, s["skip"]); assertEquals(0L, r.last["items"])
        ok(run("sync-apply", r.last["plan"] as String))
        assertEquals("no music file was sent again", 6, musicPuts())
        assertTrue(sd("ByOS/sync/manifest-v2/music.json").readText().contains("ABBA/Gold/1.flac"))
    }

    // ------------------------------------------------------------------ player-only policy, decisions, guard

    @Test
    fun policyDeleteTicksOnlyDeletedOnComputerAndDecisionsPull() {
        val m = library(20)   // one deletion out of 22 stays under DeleteGuard's 10 %
        val id = addLink("music", m, "--player-only", "delete")
        ok(run("sync-apply", plan()["plan"] as String))
        File(m, "Queen/Hits/2.flac").delete()
        SynthAudio.write(dev.sd, "Music/Mine/Extra/x.flac", SynthAudio.flac(Tags("Mine", "Me", "Extra")))
        SynthAudio.write(dev.sd, "Music/Free/Album/f.mp3", SynthAudio.mp3(Tags("Free", "F", "A")))
        SynthAudio.write(dev.sd, "Mezclas/mix.mp3", SynthAudio.mp3(Tags("Mix", "F", "A")))
        val p = plan()
        val po = summary(p)["player_only"] as Map<*, *>
        assertEquals(1L, po["deleted_on_computer"]); assertEquals(1L, po["added_on_player"])
        val items = planItems(p)
        assertTrue(items.none { (it["dest"] as String).startsWith("Music/Free") || (it["dest"] as String).startsWith("Mezclas") })
        val del = items.single { it["reason"] == "deleted_on_computer" }
        assertEquals("delete", del["default_choice"])
        val added = items.single { it["reason"] == "added_on_player" }
        assertEquals("keep", added["default_choice"])
        // deleting asks first
        val no = run("sync-apply", p["plan"] as String, stdin = ByteArrayInputStream("{\"answer\":false}\n".toByteArray()))
        assertEquals("cancelled", no.last["code"])
        assertTrue(sd("Music/Queen/Hits/02 Hit 2.flac").isFile)
        // DEC.json: copy the player's extra file to this computer
        val dec = File(dir, "dec.json").also { it.writeText("{\"${added["id"]}\":\"pull\"}") }
        val r = ok(run("sync-apply", p["plan"] as String, "--decisions", dec.path, "--yes"))
        val k = (r["kinds"] as Map<*, *>)["music"] as Map<*, *>
        assertEquals(1L, k["removed"]); assertEquals(1L, k["pulled"])
        assertFalse(sd("Music/Queen/Hits/02 Hit 2.flac").exists())
        assertArrayEquals(sd("Music/Mine/Extra/x.flac").readBytes(), File(m, "Mine/Extra/x.flac").readBytes())
        assertTrue(sd("Music/Free/Album/f.mp3").isFile); assertTrue(sd("Mezclas/mix.mp3").isFile)
        // the pulled file is now tracked: it is never copied again (at most moved to the Artist/Album layout)
        val next = plan()
        assertEquals(0L, summary(next)["add"]); assertEquals(0L, summary(next)["update"])
        assertTrue(planItems(next).none { it["reason"] == "added_on_player" })
        assertEquals(id, (ok(run("link-ls"))["links"] as List<*>).map { (it as Map<*, *>)["id"] }.single())
    }

    @Test
    fun excludesAndUnselectedNeedTheirOwnTick() {
        val m = library(20)   // Queen's 2 tracks of 22 stay under DeleteGuard's 10 %
        val id = addLink("music", m, "--player-only", "delete")
        ok(run("sync-apply", plan()["plan"] as String))
        ok(run("link-set", id, "--exclude", "Queen/**"))
        val p = plan()
        val ex = planItems(p).filter { it["reason"] == "excluded_on_computer" }
        assertEquals(2, ex.size); assertTrue(ex.all { it["default_choice"] == "keep" })
        ok(run("sync-apply", p["plan"] as String, "--yes"))
        assertTrue(sd("Music/Queen/Hits/01 Hit 1.flac").isFile)
        // choosing artists works the same way
        ok(run("link-set", id, "--clear-exclude", "--select", "artist:ABBA", "--remove-unselected"))
        val q = plan()
        val un = planItems(q).filter { it["reason"] == "excluded_on_computer" }
        assertEquals(2, un.size); assertTrue(un.all { it["default_choice"] == "delete" })
        val no = run("sync-apply", q["plan"] as String, stdin = ByteArrayInputStream("no\n".toByteArray()))
        assertEquals(7, no.code)
        assertTrue((no.events("prompt").single()["message"] as String).contains("no longer selected"))
        ok(run("sync-apply", q["plan"] as String, "--yes"))
        assertFalse(sd("Music/Queen").exists())
        assertTrue(sd("Music/ABBA/Gold/01 Song 1.flac").isFile)
        // leaving out most of the library at once (a retag, a narrowed selection) ticks nothing
        ok(run("link-set", id, "--select", "artist:Nobody", "--remove-unselected"))
        val r = run("sync-plan")
        assertTrue(r.warns("mass_delete_blocked").isNotEmpty())
        assertTrue(planItems(ok(r)).filter { it["reason"] == "excluded_on_computer" }.all { it["default_choice"] == "keep" })
    }

    @Test
    fun deleteGuardNeedsAllowMassDelete() {
        val m = File(dir, "M")
        for (i in 1..10) flac(m, "A/$i.flac", "Band", "Album", "T$i", i)
        addLink("music", m, "--player-only", "delete")
        ok(run("sync-apply", plan()["plan"] as String))
        File(m, "A/1.flac").delete(); File(m, "A/2.flac").delete()
        val r = run("sync-plan")
        val p = ok(r)
        assertTrue(r.warns("mass_delete_blocked").isNotEmpty())
        assertEquals(true, (summary(p)["guard"] as Map<*, *>)["tripped"])
        val items = planItems(p).filter { it["reason"] == "deleted_on_computer" }
        assertEquals(2, items.size); assertTrue(items.all { it["default_choice"] == "keep" })
        val dec = File(dir, "dec.json").also { f -> f.writeText("{" + items.joinToString(",") { "\"${it["id"]}\":\"delete\"" } + "}") }
        val y = run("sync-apply", p["plan"] as String, "--decisions", dec.path, "--yes")
        assertTrue(y.warns("mass_delete_blocked").isNotEmpty())
        assertEquals(10, cardFiles("Music").size)
        // typing the count works too
        val typed = run("sync-plan").last
        val dec2 = File(dir, "dec2.json").also { f -> f.writeText("{" + planItems(typed).filter { it["reason"] == "deleted_on_computer" }.joinToString(",") { "\"${it["id"]}\":\"delete\"" } + "}") }
        val t = run("sync-apply", typed["plan"] as String, "--decisions", dec2.path, stdin = ByteArrayInputStream("2\ny\n".toByteArray()))
        assertEquals(t.raw, 0, t.code)
        assertEquals(8, cardFiles("Music").size)
        // a missing folder: nothing is ticked, whatever the policy
        m.renameTo(File(dir, "gone"))
        val g = run("sync-plan")
        assertTrue(g.warns("source_unavailable").isNotEmpty())
        assertTrue(planItems(ok(g)).filter { it["action"] == "player_only" }.all { it["default_choice"] == "keep" })
        assertEquals(8L, (summary(g.last)["player_only"] as Map<*, *>)["deleted_on_computer"])
    }

    @Test
    fun conflictsFollowTheChoice() {
        val m = library(2)
        addLink("music", m)
        ok(run("sync-apply", plan()["plan"] as String))
        // the player's tag editor changed both files
        sd("Music/ABBA/Gold/01 Song 1.flac").appendBytes(ByteArray(10))
        sd("Music/ABBA/Gold/02 Song 2.flac").appendBytes(ByteArray(10))
        val p = plan()
        val c = planItems(p).filter { it["action"] == "conflict" }.associateBy { it["src_rel"] }
        assertEquals("edited_on_player", c["ABBA/Gold/1.flac"]!!["reason"])
        val dec = File(dir, "dec.json").also { it.writeText("{\"${c["ABBA/Gold/1.flac"]!!["id"]}\":\"computer\",\"${c["ABBA/Gold/2.flac"]!!["id"]}\":\"both\"}") }
        ok(run("sync-apply", p["plan"] as String, "--decisions", dec.path))
        assertArrayEquals(File(m, "ABBA/Gold/1.flac").readBytes(), sd("Music/ABBA/Gold/01 Song 1.flac").readBytes())
        assertArrayEquals(File(m, "ABBA/Gold/2.flac").readBytes(), sd("Music/ABBA/Gold/02 Song 2 (2).flac").readBytes())
        assertEquals(File(m, "ABBA/Gold/2.flac").length() + 10, sd("Music/ABBA/Gold/02 Song 2.flac").length())
        // the player's version stays as a file of the player's own
        val q = plan()
        assertEquals(0L, summary(q)["conflict"])
        assertEquals(1L, (summary(q)["player_only"] as Map<*, *>)["added_on_player"])
    }

    // ------------------------------------------------------------------ kinds, Sync all

    @Test
    fun syncAllRunsEveryKindInSequenceUnderOneLock() {
        val m = library(1)
        val books = File(dir, "Books PC").also { File(it, "Classics").mkdirs(); File(it, "Classics/Moby Dick.epub").writeBytes(ByteArray(500) { 7 }); File(it, "notes.txt.bak").writeText("x") }
        val video = File(dir, "Video PC").also { it.mkdirs(); File(it, "film.mkv").writeBytes(ByteArray(900) { 3 }); File(it, "film.en.srt").writeText("1") }
        val ab = File(dir, "Audiobooks PC").also { SynthAudio.write(it, "Book/01.mp3", SynthAudio.mp3(Tags("Ch 1", "Author", "Book"))) }
        addLink("music", m); addLink("ebooks", books); addLink("video", video); addLink("audiobooks", ab)
        val p = plan("--kind", "all")
        assertEquals(listOf("music", "audiobooks", "ebooks", "video"), p["kinds"])
        assertEquals(1L, summary(p, "ebooks")["add"]); assertEquals(2L, summary(p, "video")["add"])
        val r = ok(run("sync-apply", p["plan"] as String))
        assertEquals("done", r["status"])
        assertTrue(sd("Books/Classics/Moby Dick.epub").isFile)
        assertTrue(sd("Video/film.mkv").isFile); assertTrue(sd("Video/film.en.srt").isFile)
        assertTrue(sd("Audiobooks/Book/01.mp3").isFile)
        for (k in Kinds.ALL) assertTrue(k, sd("ByOS/sync/manifest-v2/$k.json").isFile)
        // one kind at a time
        assertEquals(0L, plan("--kind", "video")["items"])
        val journal = File(cfg, "sync/$devId/runs").listFiles()!!.single { it.name.endsWith(".jsonl") }.readLines()
        assertEquals(1, journal.count { it.contains("\"begin\"") })
        assertEquals(listOf("music", "audiobooks", "ebooks", "video"), journal.filter { it.contains("kind_done") }.map { Json.str(Json.obj(Json.parse(it)), "kind") })
    }

    @Test
    fun fourGigabyteVideoIsBlockedOnFat32() {
        // the scan of a 4.5 GB film (no such file is written: the plan needs only its size)
        val video = File(dir, "Video PC").also { it.mkdirs(); File(it, "short.mp4").writeBytes(ByteArray(10)) }
        val id = addLink("video", video)
        File(cfg, "sync/$devId/$id").mkdirs()
        File(cfg, "sync/$devId/$id/local.json").writeText("{\"format\":1,\"link\":\"$id\",\"root\":${Json.quote(video.path)},\"status\":\"ok\"," +
            "\"files\":{\"long.mkv\":{\"size\":4500000000,\"mtime\":1700000000},\"short.mp4\":{\"size\":10,\"mtime\":${File(video, "short.mp4").lastModified() / 1000}}}}")
        dev.sdFs = 1
        val p = plan("--kind", "video", "--no-scan")
        val i = planItems(p).single { it["src_rel"] == "long.mkv" }
        assertEquals("blocked", i["action"]); assertEquals("too_big_fat32", i["reason"])
        val r = run("sync-apply", p["plan"] as String)
        assertTrue(r.warns("blocked").isNotEmpty())
        assertEquals(1L, ((ok(r)["kinds"] as Map<*, *>)["video"] as Map<*, *>)["blocked"])
        assertTrue(sd("Video/short.mp4").isFile)
    }

    // ------------------------------------------------------------------ 50,000 tracks

    @Test
    fun aFiftyThousandTrackPlanIsFastAndSmall() {
        val n = 50_000
        val synced = n - 1000
        val m = File(dir, "Big")
        flac(m, "seed/seed.flac", "Seed", "Seed", "Seed", 1)
        val id = addLink("music", m)
        // the last scan of the folder (the files themselves are not needed to plan)
        val t0 = 1_700_000_000L
        val vol = LocalScanner.volumeId(m)
        val local = StringBuilder(n * 200)
        local.append("{\"format\":1,\"link\":\"").append(id).append("\",\"root\":").append(Json.quote(m.path)).append(",\"status\":\"ok\",\"volume_id\":")
            .append(if (vol == null) "null" else Json.quote(vol)).append(",\"time\":").append(t0).append(",\"errors\":0,\"unreadable\":[],\"files\":{")
        val manifest = StringBuilder(n * 250)
        manifest.append("{\"format\":2,\"kind\":\"music\",\"files\":{")
        val cid = clientId()
        dev.listPage = 400
        for (i in 0 until n) {
            val a = "Artist ${i / 500}"; val al = "Album ${i / 12}"; val k = i % 12 + 1
            val rel = "$a/$al/${"%02d".format(k)} Track $i.flac"
            val size = 30_000_000L + i
            if (i > 0) local.append(',')
            local.append(Json.quote(rel)).append(":{\"size\":").append(size).append(",\"mtime\":").append(t0)
                .append(",\"tags\":{\"title\":").append(Json.quote("Track $i")).append(",\"artist\":").append(Json.quote(a))
                .append(",\"album\":").append(Json.quote(al)).append(",\"track\":").append(k).append(",\"disc\":0}}")
            if (i < synced) {
                val dest = "Music/$a/$al/${"%02d".format(k)} Track $i.flac"
                if (i > 0) manifest.append(',')
                manifest.append(Json.quote(dest)).append(":{\"link\":\"").append(id).append("\",\"rel\":").append(Json.quote(rel))
                    .append(",\"origin\":\"").append(cid).append("\",\"src_size\":").append(size).append(",\"src_mtime\":").append(t0)
                    .append(",\"sent_size\":").append(size).append(",\"card_mtime\":").append(t0).append(",\"covers\":\"optimize\",\"time\":").append(t0).append('}')
                dev.addVirtual(dest, size, t0)
            }
        }
        local.append("}}")
        manifest.append("}}")
        File(cfg, "sync/$devId/$id").mkdirs()
        File(cfg, "sync/$devId/$id/local.json").writeText(local.toString())
        sd("ByOS/sync/manifest-v2").mkdirs()
        sd("ByOS/sync/manifest-v2/music.json").writeText(manifest.toString())
        val start = System.nanoTime()
        val r = run("sync-plan", "--no-scan")
        val secs = (System.nanoTime() - start) / 1e9
        val p = ok(r)
        val s = summary(p)
        assertEquals(1000L, s["add"]); assertEquals(synced.toLong(), s["skip"])
        assertEquals(0L, (s["player_only"] as Map<*, *>)["deleted_on_computer"])
        val size = File(p["plan"] as String).length()
        println("50k plan: %.1f s, plan file %d bytes".format(secs, size))
        assertTrue("sync-plan of 50k tracks took $secs s", secs < 60)
        assertTrue("plan file is $size bytes", size < 2_000_000)
        assertNotNull(p["run_id"])
    }
}
