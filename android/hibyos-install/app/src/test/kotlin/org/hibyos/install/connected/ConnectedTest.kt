package org.hibyos.install.connected

import org.hibyos.install.R
import org.hibyos.install.core.Json
import org.hibyos.install.core.SendItem
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File
import java.nio.file.Files

/** The connected mode without Android: path rules, plans, the library and the whole model on a fake player. */
class ConnectedTest {

    // ---- path rules against the shared vectors ---------------------------------------------

    private fun vectors(): File? {
        var d: File? = File("").absoluteFile
        while (d != null) {
            val f = File(d, "tests/btp/vectors.json")
            if (f.isFile) return f
            d = d.parentFile
        }
        return null
    }

    private fun unhex(s: String) = ByteArray(s.length / 2) { i -> s.substring(i * 2, i * 2 + 2).toInt(16).toByte() }

    @Test
    fun pathRulesMatchTheVectors() {
        val f = vectors()
        assumeTrue("tests/btp/vectors.json not found", f != null)
        val paths = Json.obj(Json.obj(Json.parse(f!!.readText()))["paths"])
        var n = 0
        for (v in Json.arr(paths["valid"])) {
            val m = Json.obj(v)
            assertNull(Json.str(m, "path_hex"), SyncPaths.check(unhex(Json.str(m, "path_hex"))))
            n++
        }
        for (v in Json.arr(paths["invalid"])) {
            val m = Json.obj(v)
            assertEquals(Json.str(m, "path_hex"), Json.str(m, "reason"), SyncPaths.check(unhex(Json.str(m, "path_hex"))))
            n++
        }
        assertNull(SyncPaths.check(ByteArray(0), allowRoot = true))
        assertTrue(n >= 20)
    }

    @Test
    fun namesAreCleanedForTheCard() {
        assertEquals("a_b_.mp3", SyncPaths.cleanName("a:b?.mp3"))
        assertEquals("Song", SyncPaths.cleanName("Song. . "))
        assertNull(SyncPaths.cleanName(".nomedia"))
        assertNull(SyncPaths.cleanName(".."))
        assertNull(SyncPaths.cleanName("   "))
        // decomposed é becomes the composed form the player's database uses
        assertEquals("Canci\u00f3n.flac", SyncPaths.cleanName("Cancio\u0301n.flac"))
        val long = "x".repeat(300) + ".flac"
        val c = SyncPaths.cleanName(long)!!
        assertTrue(c.endsWith(".flac"))
        assertEquals(255, c.toByteArray().size)
        assertNull(SyncPaths.check(c))
        // emoji are cut as whole code points, never half a surrogate pair
        val emoji = SyncPaths.cleanName("\uD83C\uDFB5".repeat(100) + ".flac")!!
        assertTrue(emoji.endsWith(".flac"))
        assertTrue(emoji.toByteArray().size <= 255)
        assertEquals(emoji, String(emoji.toByteArray(), Charsets.UTF_8))
        assertFalse(emoji.contains('?'))
        assertEquals(0, (emoji.length - 5) % 2)
        assertNull(SyncPaths.check(emoji))
        assertEquals(listOf("", "Music", "Music/ABBA"), SyncPaths.crumbs("Music/ABBA"))
    }

    // ---- plans ------------------------------------------------------------------------------

    private fun item(vararg parts: String, data: ByteArray = parts.last().toByteArray()) =
        LocalItem(parts.toList(), data.size.toLong(), 0) { ByteArrayInputStream(data) }

    @Test
    fun planPlacesFilesAndFolders() {
        val p = SendPlan.build(listOf(
            item("Album", "CD1", "01 a.flac"),
            item("Album", "cover.jpg"),
            item("Album", ".DS_Store"),
            item("Album", "01 A.FLAC"),
            item("Album", "CD1", "01 A.flac"),
        ), "Music")
        assertEquals(listOf("Music/Album", "Music/Album/CD1"), p.folders)
        assertEquals(listOf("Music/Album/CD1/01 a.flac", "Music/Album/cover.jpg", "", "Music/Album/01 A.FLAC", "Music/Album/CD1/01 A (2).flac"),
            p.jobs.map { it.remote })
        assertEquals(JobStatus.BAD_NAME, p.jobs[2].status)
        assertEquals(4, p.sources.size)
    }

    @Test
    fun planMarksFilesTooBigForFat() {
        val big = LocalItem(listOf("film.mkv"), 5L shl 30, 0) { ByteArrayInputStream(ByteArray(0)) }
        val p = SendPlan.build(listOf(big, item("a.mp3")), "", maxFile = 0xFFFFFFFFL)
        assertEquals(JobStatus.TOO_BIG, p.jobs[0].status)
        assertEquals("a.mp3", p.jobs[1].remote)
        assertTrue(p.folders.isEmpty())
    }

    @Test
    fun automaticPlanRoutesByKind() {
        val p = SendPlan.auto(listOf(
            item("Abba Gold", "01 Dancing Queen.flac"),
            item("Abba Gold", "01 Dancing Queen.lrc"),
            item("Abba Gold", "cover.jpg"),
            item("Abba Gold", "album.cue"),
            item("Holiday", "beach.jpg"),
            item("song.mp3"),
            item("folder.jpg"),
            item("novel.epub"),
            item("notes.txt"),
            item("Mix", "road trip.m3u8"),
            item("Downloads", "r3ii_2025.upt"),
            item("Weather.hpk"),
            item("hibyos-update.json"),
            item("hibyos-payload-0.9.1.tar"),
            item("Other", "data.bin"),
        ))
        val byLocal = p.jobs.associate { it.local to it }
        fun at(l: String) = byLocal[l]!!.remote
        assertEquals("Music/Abba Gold/01 Dancing Queen.flac", at("Abba Gold/01 Dancing Queen.flac"))
        assertEquals("Music/Abba Gold/01 Dancing Queen.lrc", at("Abba Gold/01 Dancing Queen.lrc"))
        assertEquals("Music/Abba Gold/cover.jpg", at("Abba Gold/cover.jpg"))
        assertEquals("Music/Abba Gold/album.cue", at("Abba Gold/album.cue"))
        assertEquals("Pictures/Holiday/beach.jpg", at("Holiday/beach.jpg"))
        assertEquals("Music/song.mp3", at("song.mp3"))
        // picked next to loose songs: the cover stays with them
        assertEquals("Music/folder.jpg", at("folder.jpg"))
        assertEquals("Books/novel.epub", at("novel.epub"))
        assertEquals("Books/notes.txt", at("notes.txt"))
        assertEquals("Playlists/road trip.m3u8", at("Mix/road trip.m3u8"))
        assertEquals("r3ii_2025.upt", at("Downloads/r3ii_2025.upt"))
        assertEquals("Apps/Weather.hpk", at("Weather.hpk"))
        assertEquals("Other/data.bin", at("Other/data.bin"))
        assertEquals(JobStatus.UPDATE_FILE, byLocal["hibyos-update.json"]!!.status)
        assertEquals(JobStatus.UPDATE_FILE, byLocal["hibyos-payload-0.9.1.tar"]!!.status)
        assertFalse(p.jobs.first { it.local == "hibyos-update.json" }.retryable)
        assertTrue(p.folders.containsAll(listOf("Music", "Music/Abba Gold", "Pictures", "Pictures/Holiday", "Books", "Playlists", "Apps", "Other")))
        assertTrue(p.folders.indexOf("Music") < p.folders.indexOf("Music/Abba Gold"))
        // a group sent elsewhere
        val q = SendPlan.auto(listOf(item("Holiday", "beach.jpg"), item("x.flac")), mapOf(Route.PICTURES to "Music/Art", Route.MUSIC to ""))
        assertEquals(listOf("Music/Art/Holiday/beach.jpg", "x.flac"), q.jobs.map { it.remote })
    }

    @Test
    fun automaticDestinationInTheModel() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        assertEquals(Destination.AUTO, m.state.dest)
        m.addPicked { listOf(item("Album", "a.flac"), item("photo.png"), item("hibyos-update.json", data = "{}".toByteArray())) }
        assertEquals(listOf(Route.MUSIC, Route.PICTURES, Route.BYOS_UPDATE), m.state.transfer.jobs.map { it.route })
        m.setRoute(Route.PICTURES, "Music")
        assertEquals("Music/photo.png", m.state.transfer.jobs[1].remote)
        // the update file goes to Install Update instead (its signature is checked there)
        m.useUpdateFiles()
        assertEquals(2, m.state.transfer.jobs.size)
        assertEquals(R.string.sync_err_signature, m.state.otaPick.error?.title)
        m.startSend()
        assertTrue(m.state.transfer.finished)
        assertEquals(0, m.state.transfer.problems)
        assertTrue(card.files.containsKey("Music/Album/a.flac"))
        assertTrue(card.files.containsKey("Music/photo.png"))
        // back to the default folder
        m.clearSend()
        m.setRoute(Route.PICTURES, "Pictures")
        assertTrue(m.state.routes.isEmpty())
    }

    // ---- library ----------------------------------------------------------------------------

    @Test
    fun libraryGroupsLikeThePlayer() {
        val nul = "\u0000"
        val lib = Library.build(listOf(
            Track("a:\\The Beatles\\Abbey Road\\02 Something.flac$nul", "Something$nul", "The Beatles$nul", "", "Abbey Road$nul", year = 1969),
            Track("a:\\The Beatles\\Abbey Road\\01 Come Together.flac$nul", "Come Together", "The Beatles", "", "Abbey Road", year = 1969),
            Track("a:\\ABBA\\Gold\\01 Dancing Queen.flac", "Dancing Queen", "ABBA", "ABBA", "Gold", year = 1992),
            Track("a:\\VA\\Hits\\01 x.mp3", "X", "Someone", "Various Artists", "Hits"),
            Track("a:\\VA\\Hits\\02 y.mp3", "", "Other", "Various Artists", "Hits"),
        ))
        assertEquals(listOf("ABBA", "The Beatles", "Various Artists"), lib.artists.map { it.name })
        val road = lib.artists[1].albums.single()
        assertEquals(listOf("Come Together", "Something"), road.tracks.map { it.title })
        assertEquals(2, lib.artists[2].trackCount)
        assertEquals("02 y", lib.albums.first { it.title == "Hits" }.tracks[1].title)
        assertEquals("The Beatles/Abbey Road/01 Come Together.flac", Library.cardPath(road.tracks[0].path))
        assertNull(Library.cardPath("b:\\x.mp3"))
    }

    // ---- the model on a fake player ---------------------------------------------------------

    private class Env(val c: FakeConnector, val byos: List<SendItem>? = null) : SyncEnv {
        val tmp: File = Files.createTempDirectory("sync").toFile()
        var picks = 0
        var library: List<Track> = emptyList()
        override fun connector(): SyncConnector = c
        override val clientName = "Test phone"
        override val cacheDir: File = tmp
        override fun post(f: () -> Unit) = f()
        override fun execute(f: () -> Unit) = f()
        override fun readLibrary(file: File): List<Track> = library
        override fun pickFiles() {
            picks++
        }
        override fun pickFolder() {
            picks++
        }
        override fun byosFiles(): List<SendItem>? = byos

        var keys: List<String> = emptyList()
        override val releaseKeys: List<String> get() = keys
        var slept = 0L
        var onSleep: (() -> Unit)? = null
        val notes = ArrayList<WorkNote?>()
        override fun work(note: WorkNote?) {
            notes += note
        }
        override fun sleep(ms: Long) {
            slept += ms
            clock += ms
            onSleep?.invoke()
        }

        /** The keep-alive timer, fired by hand; and a clock the tests move. */
        var timer: (() -> Unit)? = null
        var clock = 1_000_000L
        override fun every(periodMs: Long, f: () -> Unit): java.io.Closeable {
            timer = f
            return java.io.Closeable { timer = null }
        }
        override fun now(): Long = clock
    }

    private fun paired(env: Env): ConnectedModel {
        val m = ConnectedModel(env)
        m.findDevices()
        assertEquals(1, m.state.found.size)
        m.connectTo(m.state.found[0])
        val l = m.state.link as Link.Pairing
        assertFalse(l.wrong)
        m.setCode("815 365")
        assertEquals("815365", (m.state.link as Link.Pairing).code)
        m.confirmCode()
        assertTrue(m.state.link is Link.Connected)
        return m
    }

    @Test
    fun pairingWithAWrongCodeStartsAgain() {
        val env = Env(FakeConnector())
        val m = ConnectedModel(env)
        assertTrue(m.state.available)
        m.connectAddress("192.168.1.20")
        m.setCode("000000")
        m.confirmCode()
        val l = m.state.link as Link.Pairing
        assertTrue(l.wrong)
        assertEquals("", l.code)
        assertEquals(2, env.c.pairings)
        m.setCode("815365")
        m.confirmCode()
        val c = m.state.link as Link.Connected
        assertEquals("R3II_2025", c.device.name)
        assertEquals(1, m.state.paired.size)
        // a second connection uses AUTH, without a code
        m.disconnect()
        m.connectTo(m.state.found.firstOrNull() ?: env.c.device.copy(paired = true))
        assertTrue(m.state.link is Link.Connected)
        assertEquals(2, env.c.pairings)
    }

    @Test
    fun pairingErrorsAreShown() {
        val c = FakeConnector()
        c.pairingOpen = false
        val m = ConnectedModel(Env(c))
        m.connectAddress("192.168.1.20")
        assertEquals(R.string.sync_err_pair_closed, m.state.error?.title)
        m.connectAddress("10.0.0.9:7340")
        assertEquals(R.string.sync_err_unreachable, m.state.error?.title)
        m.connectAddress("not an address!")
        assertEquals(R.string.sync_err_bad_address, m.state.error?.title)
        c.pairingOpen = true
        c.allow = false
        m.connectAddress("192.168.1.20")
        m.setCode("815365")
        m.confirmCode()
        assertTrue(m.state.link is Link.Idle)
        assertEquals(R.string.sync_err_pair_rejected, m.state.error?.title)
    }

    @Test
    fun sendResumesAfterALostConnection() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        val big = ByteArray(300_000) { (it * 7).toByte() }
        m.setDestination(Destination.BOOKS)
        m.addPicked { listOf(item("Book", "part1.m4b", data = big), item("Book", "notes.txt"), item("Book", ".hidden")) }
        assertEquals(3, m.state.transfer.jobs.size)
        card.dropAfter = 200_000
        // the player stays out of reach: the app tries again for about a minute, then says so
        env.c.down = true
        m.startSend()
        assertEquals(ConnectedModel.RECONNECT_DELAYS.sum(), env.slept)
        assertEquals(0, m.state.transfer.reconnecting)
        env.c.down = false
        val t = m.state.transfer
        assertFalse(t.running)
        assertEquals(R.string.sync_err_lost, t.error?.title)
        assertTrue(m.state.link is Link.Idle)
        // the file whose connection dropped is not shown as "not sent": it waits for the next try
        assertEquals(JobStatus.WAITING, t.jobs[0].status)
        assertEquals(0, t.problems)
        assertEquals(2, t.left)
        // connect again and go on: the device resumes from the 64 KiB boundary of what it kept
        m.connectTo(env.c.device.copy(paired = true))
        m.startSend()
        assertTrue(m.state.transfer.finished)
        assertEquals(listOf(262_144L), card.resumedAt)
        assertTrue(card.files["Books/Book/part1.m4b"]!!.contentEquals(big))
        assertEquals("notes.txt", String(card.files["Books/Book/notes.txt"]!!))
        assertEquals(JobStatus.BAD_NAME, m.state.transfer.jobs[2].status)
        assertEquals(2, m.state.transfer.sent)
    }

    @Test
    fun transferReconnectsByItselfAndResumes() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        val big = ByteArray(300_000) { (it * 3).toByte() }
        m.setDestination(Destination.MUSIC)
        m.addPicked { listOf(item("Live", "set.flac", data = big), item("Live", "b.mp3")) }
        card.dropAfter = 200_000
        // the player got a new address while the Wi-Fi was down: it is found again by its device id
        env.onSleep = { env.c.device = env.c.device.copy(host = "192.168.1.21") }
        m.startSend()
        val t = m.state.transfer
        assertTrue(t.finished)
        assertNull(t.error)
        assertEquals(2, t.sent)
        assertEquals(listOf(262_144L), card.resumedAt)
        assertTrue(card.files["Music/Live/set.flac"]!!.contentEquals(big))
        assertEquals(2_000L, env.slept)
        val l = m.state.link as Link.Connected
        assertEquals("192.168.1.21", l.device.host)
        // the notification saw the reconnection, and nothing runs any more
        assertTrue(env.notes.any { it?.reconnecting == 1 })
        assertNull(env.notes.last())
    }

    @Test
    fun workNotesFollowTheTransfer() {
        val s = SyncState(transfer = Transfer(jobs = listOf(SendJob(1, "a", "Music/a", 10)), running = true, done = 5, total = 20, current = "Music/a"))
        val n = ConnectedModel.workOf(s)!!
        assertEquals(WorkNote.Kind.SEND, n.kind)
        assertEquals(250, n.permille)
        assertNull(ConnectedModel.workOf(SyncState()))
        assertEquals(WorkNote.Kind.INSTALL, ConnectedModel.workOf(SyncState(ota = OtaPhase.Installing("0.9.1", 500)))!!.kind)
        assertEquals(-1, ConnectedModel.workOf(SyncState(ota = OtaPhase.WaitingUser("0.9.1")))!!.permille)
    }

    /** Review: a phone file that fails mid-PUT must not leave the next request behind the abandoned upload. */
    @Test
    fun localReadErrorDuringPutOpensANewSession() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        val sessions = env.c.sessions.size
        val data = ByteArray(200_000) { it.toByte() }
        var opens = 0
        val bad = LocalItem(listOf("bad.flac"), data.size.toLong(), 0) {
            opens++
            if (opens == 1) ByteArrayInputStream(data)
            else object : java.io.InputStream() {
                var n = 0
                override fun read(): Int = throw java.io.IOException("provider error")
                override fun read(b: ByteArray, off: Int, len: Int): Int {
                    if (n >= 100_000) throw java.io.IOException("provider error")
                    val k = minOf(len, 100_000 - n)
                    System.arraycopy(data, n, b, off, k)
                    n += k
                    return k
                }
            }
        }
        m.addPicked { listOf(bad, item("good.mp3")) }
        m.startSend()
        val j = m.state.transfer.jobs
        assertEquals(listOf(JobStatus.FAILED, JobStatus.SENT), j.map { it.status })
        assertEquals("good.mp3", String(card.files["Music/good.mp3"]!!))
        assertTrue(env.c.sessions[sessions - 1].stalled)
        assertEquals(sessions + 1, env.c.sessions.size)
        assertTrue(m.state.link is Link.Connected)
        assertNull(m.state.transfer.error)
    }

    /** Review: a file changed after it was hashed is hashed again on the next try. */
    @Test
    fun hashMismatchIsHashedAgainOnResume() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        var opens = 0
        val changing = LocalItem(listOf("x.txt"), 3, 0) {
            opens++
            ByteArrayInputStream((if (opens == 1) "old" else "new").toByteArray())
        }
        m.setDestination(Destination.MUSIC)
        m.addPicked { listOf(changing) }
        m.startSend()
        assertEquals(JobStatus.FAILED, m.state.transfer.jobs[0].status)
        assertEquals(SyncException.E_HASH_MISMATCH, m.state.transfer.jobs[0].error)
        m.startSend()
        assertEquals(JobStatus.SENT, m.state.transfer.jobs[0].status)
        assertEquals("new", String(card.files["Music/x.txt"]!!))
    }

    /** Review: the idle session gets a PING every 30 s, also while a big file is hashed. */
    @Test
    fun idleSessionsAreKeptAlive() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        val tick = env.timer!!
        tick()
        assertEquals(0, card.pings) // just connected
        env.clock += 30_000
        tick()
        assertEquals(1, card.pings)
        tick()
        assertEquals(1, card.pings) // nothing due yet
        // hashing a file whose reads take 20 s each sends PINGs between the blocks
        val data = ByteArray(6 * 65536)
        val slow = LocalItem(listOf("slow.bin"), data.size.toLong(), 0) {
            object : java.io.InputStream() {
                val i = ByteArrayInputStream(data)
                override fun read(): Int = i.read()
                override fun read(b: ByteArray, off: Int, len: Int): Int {
                    env.clock += 20_000
                    return i.read(b, off, len)
                }
            }
        }
        val before = card.pings
        m.addPicked { listOf(slow) }
        m.startSend()
        assertEquals(JobStatus.SENT, m.state.transfer.jobs[0].status)
        assertTrue("pings while hashing: ${card.pings - before}", card.pings - before >= 3)
        // a ping that finds the connection gone ends the session
        env.c.sessions.last().closed = true
        env.clock += 60_000
        tick()
        assertTrue(m.state.link is Link.Idle)
        assertEquals(R.string.sync_err_lost, m.state.error?.title)
        m.close()
        assertNull(env.timer)
    }

    /** Review: the device the user chose is the one that must answer (8.3). */
    @Test
    fun anotherDeviceAtTheAddressIsNotPaired() {
        val env = Env(FakeConnector())
        val m = paired(env)
        m.disconnect()
        env.c.answeringId = "ffffffffffffffff"
        m.connectTo(env.c.device.copy(paired = true))
        assertTrue(m.state.link is Link.Idle)
        assertEquals(R.string.sync_err_identity, m.state.error?.title)
        assertEquals(1, env.c.pairings)
    }

    /** Review: an AUTH refusal keeps the keys and offers pairing again. */
    @Test
    fun forgottenPhoneKeepsItsKeys() {
        val env = Env(FakeConnector())
        val m = paired(env)
        m.disconnect()
        env.c.forgotten = true
        env.c.pairingOpen = false
        m.connectTo(env.c.device.copy(paired = true))
        assertEquals(R.string.sync_err_forgotten, m.state.error?.title)
        assertEquals(1, env.c.paired().size)
        env.c.pairingOpen = true
        m.connectTo(env.c.device.copy(paired = true))
        assertTrue(m.state.link is Link.Pairing)
        m.setCode("815365")
        m.confirmCode()
        assertTrue(m.state.link is Link.Connected)
    }

    @Test
    fun typedPortIsKept() {
        val env = Env(FakeConnector())
        val m = ConnectedModel(env)
        m.findDevices()
        m.connectAddress("192.168.1.20:7341")
        assertEquals(7341, (m.state.link as Link.Pairing).device.port)
        m.cancelPairing()
        m.connectAddress("192.168.1.20")
        assertEquals("0123456789abcdef", (m.state.link as Link.Pairing).device.deviceId)
    }

    /** Review: close() comes from the main thread; the socket work must happen on the worker. */
    @Test
    fun closeRunsOnTheWorker() {
        val queued = ArrayList<() -> Unit>()
        var direct = true
        val env = object : SyncEnv by Env(FakeConnector()) {
            override fun execute(f: () -> Unit) = if (direct) f() else { queued += f; Unit }
        }
        val c = (env.connector() as FakeConnector)
        val m = ConnectedModel(env)
        m.findDevices()
        m.connectTo(m.state.found[0])
        m.setCode("815365")
        m.confirmCode()
        assertTrue(m.state.link is Link.Connected)
        direct = false
        m.close()
        assertFalse(c.sessions.last().closed)
        queued.forEach { it() }
        assertTrue(c.sessions.last().closed)
    }

    @Test
    fun existingFilesAreSkippedOrReported() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Music"
        card.files["Music/a.mp3"] = "a.mp3".toByteArray()
        card.files["Music/b.mp3"] = "old".toByteArray()
        val m = paired(env)
        m.addPicked { listOf(item("a.mp3"), item("b.mp3"), item("c.mp3")) }
        m.startSend()
        val j = m.state.transfer.jobs
        assertEquals(listOf(JobStatus.SKIPPED, JobStatus.EXISTS, JobStatus.SENT), j.map { it.status })
        assertEquals("old", String(card.files["Music/b.mp3"]!!))
        // with "replace", a new choice replaces it
        m.clearSend()
        m.setReplace(true)
        m.addPicked { listOf(item("b.mp3")) }
        m.startSend()
        assertEquals(JobStatus.SENT, m.state.transfer.jobs[0].status)
        assertEquals("b.mp3", String(card.files["Music/b.mp3"]!!))
    }

    @Test
    fun fileBrowserOperations() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += listOf("Music", "Music/Old", "Books")
        card.files["Music/x.flac"] = ByteArray(10)
        card.files["Music/Old/y.flac"] = ByteArray(5)
        val m = paired(env)
        m.openFolder("Music")
        assertEquals(listOf("Old", "x.flac"), m.state.browser.entries!!.map { it.name })
        m.prompt(Prompt.NewFolder("New: one"))
        m.applyPrompt()
        assertTrue(card.folders.contains("Music/New_ one"))
        assertEquals(listOf("New_ one", "Old", "x.flac"), m.state.browser.entries!!.map { it.name })
        m.prompt(Prompt.Rename(SyncEntry("x.flac", 0, 10, 0), "z.flac"))
        m.applyPrompt()
        assertTrue(card.files.containsKey("Music/z.flac"))
        // move a folder into another one, then try to move it into itself
        m.startMove(SyncEntry("Old", 1, 0, 0))
        m.openFolder("Books")
        m.moveHere()
        assertTrue(card.files.containsKey("Books/Old/y.flac"))
        assertNull(m.state.browser.moving)
        m.startMove(SyncEntry("Old", 1, 0, 0))
        m.openFolder("Books/Old")
        m.moveHere()
        assertEquals(R.string.sync_err_move_inside, m.state.browser.error?.title)
        m.cancelMove()
        assertTrue(m.up())
        assertEquals("Books", m.state.browser.path)
        m.prompt(Prompt.Delete(SyncEntry("Old", 1, 0, 0)))
        m.applyPrompt()
        assertFalse(card.files.containsKey("Books/Old/y.flac"))
        // "Send here" takes the folder open in the browser
        m.sendHere()
        assertEquals("Books", m.state.target)
        m.setDestination(Destination.PICTURES)
        assertEquals("Pictures", m.state.target)
    }

    @Test
    fun libraryFromTheSnapshot() {
        val env = Env(FakeConnector())
        val m = paired(env)
        (env.c.sessions.last()).snapshot = "SQLite format 3\u0000".toByteArray()
        env.library = listOf(Track("a:\\A\\B\\1.flac", "One", "A", "", "B"))
        m.loadLibrary()
        val r = m.state.library as LibraryPhase.Ready
        assertEquals("A", r.library.artists.single().name)
        assertTrue(File(env.tmp, "sync/library.db").isFile)
        m.libraryNav(LibraryNav(artist = "A"))
        m.libraryNav(m.state.libraryNav.copy(album = "A" to "B"))
        assertTrue(m.libraryBack())
        assertTrue(m.libraryBack())
        assertFalse(m.libraryBack())
    }

    @Test
    fun byosUpdateUsesThePatchedFiles() {
        val fw = "firmware".toByteArray()
        val shell = "shell".toByteArray()
        val items = listOf(
            SendItem("ByOS/payload/bin/hibyshell", shell.size.toLong(), sha256(shell).joinToString("") { "%02x".format(it) }) { ByteArrayInputStream(shell) },
            SendItem("r3ii_2025.upt", fw.size.toLong(), null) { ByteArrayInputStream(fw) },
        )
        val env = Env(FakeConnector(), items)
        val card = env.c.card
        card.folders += listOf("ByOS", "ByOS/payload", "ByOS/payload/bin")
        card.files["ByOS/payload/bin/hibyshell"] = shell
        card.files["r3ii_2025.upt"] = "older".toByteArray()
        val m = paired(env)
        m.sendByosUpdate()
        assertEquals(UpdatePhase.Sent(2, 1), m.state.update)
        assertEquals("firmware", String(card.files["r3ii_2025.upt"]!!))
    }

    @Test
    fun byosUpdateGoesOnAfterALostConnection() {
        // the player's sync service restarts in the middle of the firmware file (as the shell
        // does under memory pressure): the app connects again and resumes that file
        val fw = ByteArray(300_000) { (it * 13).toByte() }
        val small = "shell".toByteArray()
        val items = listOf(
            SendItem("ByOS/payload/bin/hibyshell", small.size.toLong(), null) { ByteArrayInputStream(small) },
            SendItem("r3ii_2025.upt", fw.size.toLong(), null) { ByteArrayInputStream(fw) },
        )
        val env = Env(FakeConnector(), items)
        val card = env.c.card
        val m = paired(env)
        card.dropAfter = 200_000
        m.sendByosUpdate()
        assertEquals(UpdatePhase.Sent(2, 0), m.state.update)
        assertTrue(card.files["r3ii_2025.upt"]!!.contentEquals(fw))
        assertEquals("shell", String(card.files["ByOS/payload/bin/hibyshell"]!!))
        // resumed from what the device kept, not sent again from the start
        assertEquals(1, card.resumedAt.size)
        assertTrue(m.state.link is Link.Connected)
    }

    @Test
    fun aRememberedPlayerThatIsNotFoundIsStillPaired() {
        val env = Env(FakeConnector())
        val m = paired(env)
        m.disconnect()
        env.c.down = true
        val p = m.state.paired.single()
        m.connectTo(SyncDevice(p.lastHost ?: "", 7340, p.deviceId, p.name, false, true, true))
        assertTrue(m.state.link is Link.Idle)
        // not "pair again", not "search for a new device": the pairing is kept
        assertEquals(R.string.sync_err_paired_gone, m.state.error?.title)
        assertEquals(1, m.state.paired.size)
        env.c.down = false
        m.connectTo(SyncDevice(p.lastHost ?: "", 7340, p.deviceId, p.name, false, true, true))
        assertTrue(m.state.link is Link.Connected)
    }

    // ---- library rescan (section 13.3) ------------------------------------------------------

    @Test
    fun sentMusicIsCountedAndTheLibraryUpdated() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Music"
        card.files["Music/old.flac"] = "old".toByteArray()
        val m = paired(env)
        m.setReplace(true)
        m.addPicked { listOf(item("new.flac"), item("old.flac"), item("cover.jpg")) }
        m.startSend()
        // one new song; the replaced one gets its tags refreshed at once (mode 1)
        assertEquals(1, m.state.transfer.newMusic)
        assertEquals(listOf<List<String>?>(listOf("Music/old.flac")), card.rescans)
        (env.c.sessions.last()).snapshot = "SQLite format 3\u0000".toByteArray()
        card.onRescan = { env.library = listOf(Track("a:\\Music\\new.flac", "New", "A", "", "B")) }
        m.rescanLibrary()
        assertEquals(Rescan.Done, m.state.rescan)
        assertEquals(0, m.state.transfer.newMusic)
        assertEquals("New", (m.state.library as LibraryPhase.Ready).library.tracks.single().title)
        assertNull(card.rescans.last())
    }

    @Test
    fun rescanThatThePlayerCannotDo() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.fullRescan = false
        card.pathRescan = false
        card.folders += "Music"
        card.files["Music/a.mp3"] = "x".toByteArray()
        val m = paired(env)
        m.setReplace(true)
        m.addPicked { listOf(item("a.mp3"), item("b.mp3")) }
        m.startSend()
        // an older ByOS without mode 1 is not an error for the transfer
        assertNull(m.state.transfer.error)
        assertEquals(listOf(JobStatus.SENT, JobStatus.SENT), m.state.transfer.jobs.map { it.status })
        m.rescanLibrary()
        assertEquals(Rescan.Unsupported, m.state.rescan)
        assertFalse(m.state.busy)
        assertTrue(m.state.link is Link.Connected)
    }

    // ---- managing the library ---------------------------------------------------------------

    private fun libraryOnCard(env: Env): ConnectedModel {
        val card = env.c.card
        card.folders += listOf("Music", "Music/ABBA", "Music/ABBA/Gold", "Music/Mix")
        for (p in listOf("Music/ABBA/Gold/01 Dancing Queen.flac", "Music/ABBA/Gold/02 Knowing Me.flac", "Music/ABBA/Gold/cover.jpg",
            "Music/Mix/a.mp3", "Music/Mix/b.mp3", "Music/loose.mp3")) card.files[p] = p.toByteArray()
        env.library = listOf(
            Track("a:\\Music\\ABBA\\Gold\\01 Dancing Queen.flac", "Dancing Queen", "ABBA", "", "Gold"),
            Track("a:\\Music\\ABBA\\Gold\\02 Knowing Me.flac", "Knowing Me", "ABBA", "", "Gold"),
            Track("a:\\Music\\Mix\\a.mp3", "A", "Various", "", "Hits"),
            Track("a:\\Music\\Mix\\b.mp3", "B", "Other", "", "Other album"),
            Track("a:\\Music\\loose.mp3", "Loose", "Solo", "", "Single"),
        )
        val m = paired(env)
        env.c.sessions.last().snapshot = "SQLite format 3\u0000".toByteArray()
        m.loadLibrary()
        return m
    }

    private fun lib(m: ConnectedModel) = (m.state.library as LibraryPhase.Ready).library

    @Test
    fun albumWithItsOwnFolderIsDeletedWithIt() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.fullRescan = false
        val m = libraryOnCard(env)
        val gold = lib(m).albums.first { it.title == "Gold" }
        assertEquals("Music/ABBA/Gold", Library.folderOf(gold.tracks))
        m.libraryNav(LibraryNav(artist = "ABBA", album = "ABBA" to "Gold"))
        m.libraryDelete(LibTarget.AlbumOf(gold))
        m.confirmLibraryDelete()
        assertNull(m.state.libDelete)
        assertFalse(card.folders.contains("Music/ABBA/Gold"))
        assertFalse(card.files.containsKey("Music/ABBA/Gold/cover.jpg"))
        assertTrue(card.folders.contains("Music/ABBA"))
        // the player cannot rescan: the songs are taken out of the list here, and the open album closes
        assertEquals(Rescan.Unsupported, m.state.rescan)
        assertTrue(lib(m).albums.none { it.title == "Gold" })
        assertNull(m.state.libraryNav.album)
    }

    @Test
    fun albumSharingAFolderLosesOnlyItsSongs() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = libraryOnCard(env)
        val hits = lib(m).albums.first { it.title == "Hits" }
        card.onRescan = { env.library = env.library.filter { !it.path.endsWith("a.mp3") } }
        m.libraryDelete(LibTarget.AlbumOf(hits))
        m.confirmLibraryDelete()
        assertFalse(card.files.containsKey("Music/Mix/a.mp3"))
        assertTrue(card.files.containsKey("Music/Mix/b.mp3"))
        assertTrue(card.folders.contains("Music/Mix"))
        // the player rescanned: the library comes from the new snapshot
        assertEquals(Rescan.Done, m.state.rescan)
        assertEquals(4, lib(m).tracks.size)
        // a loose song in a top folder never takes that folder with it
        val loose = lib(m).tracks.first { it.title == "Loose" }
        assertNull(Library.folderOf(listOf(loose)))
        m.libraryDelete(LibTarget.Song(loose))
        m.confirmLibraryDelete()
        assertFalse(card.files.containsKey("Music/loose.mp3"))
        assertTrue(card.folders.contains("Music"))
    }

    @Test
    fun searchFindsWordsWithoutAccents() {
        val lib = Library.build(listOf(
            Track("a:\\B\\Lemonade\\01.flac", "Pray You Catch Me", "Beyoncé", "", "Lemonade"),
            Track("a:\\R\\Bodas\\01.flac", "Canción", "Rosalía", "", "Los Ángeles"),
            Track("a:\\A\\Gold\\01.flac", "Dancing Queen", "ABBA", "", "Gold"),
        ))
        assertEquals(listOf("Beyoncé"), lib.search("beyonce").artists.map { it.name })
        assertEquals(listOf("Los Ángeles"), lib.search("angeles rosalia").albums.map { it.title })
        assertEquals(listOf("Canción"), lib.search("CANCION").tracks.map { it.title })
        assertEquals(listOf("Dancing Queen"), lib.search("queen abba").tracks.map { it.title })
        assertTrue(lib.search("zzz").empty)
        assertTrue(lib.search("  ").empty)
    }

    @Test
    fun artworkComesFromTheAlbumFolderOnce() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = libraryOnCard(env)
        card.files["Music/ABBA/Gold/Folder.JPG"] = "jpeg".toByteArray()
        val gold = lib(m).albums.first { it.title == "Gold" }
        val folder = Library.artFolder(gold)!!
        m.loadCover(folder)
        // cover.jpg wins over folder.jpg
        val path = m.state.covers[folder]!!
        assertEquals("Music/ABBA/Gold/cover.jpg", String(File(path).readBytes()))
        val gets = card.gets
        m.loadCover(folder)
        assertEquals(gets, card.gets)
        // a folder without pictures has no artwork; a second connection reads the cache
        m.loadCover("Music/Mix")
        assertTrue(m.state.covers.containsKey("Music/Mix"))
        assertNull(m.state.covers["Music/Mix"])
        m.disconnect()
        m.connectTo(env.c.device.copy(paired = true))
        m.loadCover(folder)
        assertEquals(gets, card.gets)
        assertEquals(path, m.state.covers[folder])
        // albums split in CD folders look in their parent
        val cds = Album("Box", "X", 0, listOf(Track("a:\\Music\\Box\\CD1\\1.flac", "a", "X", "", "Box"), Track("a:\\Music\\Box\\CD2\\1.flac", "b", "X", "", "Box")))
        assertEquals("Music/Box", Library.artFolder(cds))
    }

    @Test
    fun savingACardFileToThePhone() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Recordings"
        card.files["Recordings/talk.wav"] = ByteArray(70_000) { it.toByte() }
        var asked: String? = null
        val m = ConnectedModel(object : SyncEnv by env {
            override fun createDocument(name: String) {
                asked = name
            }
        })
        m.findDevices()
        m.connectTo(m.state.found[0])
        m.setCode("815365")
        m.confirmCode()
        m.openFolder("Recordings")
        m.saveToPhone(m.state.browser.entries!!.single())
        assertEquals("talk.wav", asked)
        val out = java.io.ByteArrayOutputStream()
        m.downloadTo { out }
        val d = m.state.browser.download!!
        assertTrue(d.finished)
        assertEquals(70_000L, d.total)
        assertTrue(out.toByteArray().contentEquals(card.files["Recordings/talk.wav"]))
        assertFalse(m.state.busy)
        m.clearDownload()
        assertNull(m.state.browser.download)
        // a file gone in the meantime
        m.saveToPhone(SyncEntry("gone.mp3", 0, 3, 0))
        m.downloadTo { java.io.ByteArrayOutputStream() }
        assertEquals(R.string.sync_err_not_found, m.state.browser.download?.error?.title)
    }

    @Test
    fun m3uPathsRoundTrip() {
        assertEquals("../Music/ABBA/Gold/01.flac", M3u.relative("Playlists", "Music/ABBA/Gold/01.flac"))
        assertEquals("01.flac", M3u.relative("Music/ABBA/Gold", "Music/ABBA/Gold/01.flac"))
        assertEquals("../B/x.mp3", M3u.relative("Music/A", "Music/B/x.mp3"))
        val text = M3u.write("Playlists", listOf("Music/ABBA/Gold/01 Dancing Queen.flac" to "ABBA - Dancing Queen", "Music/x.mp3" to ""))
        assertTrue(text.startsWith("#EXTM3U\n#EXTINF:-1,ABBA - Dancing Queen\n../Music/ABBA/Gold/01 Dancing Queen.flac\n"))
        assertEquals(listOf("Music/ABBA/Gold/01 Dancing Queen.flac", "Music/x.mp3"), M3u.read("Playlists", text))
        // what other tools write: Windows paths, the player's own absolute paths, a BOM; URLs and paths off the card are skipped
        val other = "﻿#EXTM3U\r\n..\\Music\\a.flac\r\n/usr/data/mnt/sd_0/Music/b.flac\r\na:\\Music\\c.flac\r\nhttp://radio/x\r\n/etc/passwd\r\n../../out.mp3\r\n"
        assertEquals(listOf("Music/a.flac", "Music/b.flac", "Music/c.flac"), M3u.read("Playlists", other))
    }

    @Test
    fun playlistsAreMadeAndChanged() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = libraryOnCard(env)
        val gold = lib(m).albums.first { it.title == "Gold" }
        // no Playlists folder yet: an empty list, not an error
        m.loadPlaylists()
        assertEquals(emptyList<String>(), m.state.playlists.files)
        m.addToPlaylist(LibTarget.AlbumOf(gold))
        m.newPlaylistName("Road trip")
        m.createPlaylist()
        val file = "Playlists/Road trip.m3u8"
        assertEquals(listOf(file), m.state.playlists.files)
        assertEquals("Road trip", m.state.playlists.added)
        assertNull(m.state.playlists.adding)
        val text = String(card.files[file]!!)
        assertTrue(text, text.contains("#EXTINF:-1,ABBA - Dancing Queen\n../Music/ABBA/Gold/01 Dancing Queen.flac"))
        // add a song to it, open it, remove the first song
        m.addToPlaylist(LibTarget.Song(lib(m).tracks.first { it.title == "Loose" }))
        m.addToPlaylistFile(file)
        m.openPlaylist(file)
        assertEquals(listOf("Music/ABBA/Gold/01 Dancing Queen.flac", "Music/ABBA/Gold/02 Knowing Me.flac", "Music/loose.mp3"), m.state.playlists.open!!.songs)
        m.removeFromPlaylist(0)
        assertEquals(2, M3u.read("Playlists", String(card.files[file]!!)).size)
        // a second playlist with the same name is refused, the first one stays
        m.newPlaylistName("road trip")
        m.createPlaylist()
        assertEquals(R.string.sync_err_exists, m.state.playlists.error?.title)
        assertTrue(m.libraryBack())
        assertNull(m.state.libraryNav.playlist)
        m.deletePlaylist(file)
        assertFalse(card.files.containsKey(file))
        assertEquals(emptyList<String>(), m.state.playlists.files)
    }

    @Test
    fun movingFromTheLibraryOpensFiles() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Music/Old"
        val m = libraryOnCard(env)
        val gold = lib(m).albums.first { it.title == "Gold" }
        m.libraryMove(LibTarget.AlbumOf(gold))
        assertEquals("Music/ABBA/Gold", m.state.browser.moving)
        assertEquals("Music/ABBA", m.state.browser.path)
        m.openFolder("Music/Old")
        m.moveHere()
        assertTrue(card.files.containsKey("Music/Old/Gold/cover.jpg"))
        assertTrue(m.state.libraryChanged)
        // a song's folder in Files
        m.revealInFiles(LibTarget.Song(lib(m).tracks.first { it.title == "B" }))
        assertEquals("Music/Mix", m.state.browser.path)
        assertEquals(listOf("b.mp3"), m.state.browser.entries!!.map { it.name }.filter { it == "b.mp3" })
        // an artist has no folder to move
        assertNull(LibTarget.ArtistOf(lib(m).artists.first()).cardPath)
    }

    // ---- review fixes -------------------------------------------------------------------------

    /** An env whose worker jobs wait in a queue until [run] (the tests see the states in between). */
    private class Queued(val base: Env) : SyncEnv by base {
        var hold = false
        val jobs = ArrayDeque<() -> Unit>()
        override fun execute(f: () -> Unit) {
            if (hold) jobs += f else f()
        }
        fun run() {
            hold = false
            while (jobs.isNotEmpty()) jobs.removeFirst()()
        }
    }

    private fun pairedWith(env: SyncEnv, c: FakeConnector): ConnectedModel {
        val m = ConnectedModel(env)
        m.connectTo(c.device)
        m.setCode(c.code)
        m.confirmCode()
        assertTrue(m.state.link is Link.Connected)
        return m
    }

    @Test
    fun albumFolderWithOtherFilesKeepsThem() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.fullRescan = false
        val m = libraryOnCard(env)
        for (p in listOf("Music/ABBA/Gold/booklet.pdf", "Music/ABBA/Gold/live.mp4", "Music/ABBA/Gold/01 Dancing Queen.lrc")) card.files[p] = p.toByteArray()
        m.libraryDelete(LibTarget.AlbumOf(lib(m).albums.first { it.title == "Gold" }))
        m.confirmLibraryDelete()
        // only the songs go: the booklet, the video and the folder stay
        assertTrue(card.folders.contains("Music/ABBA/Gold"))
        assertFalse(card.files.containsKey("Music/ABBA/Gold/01 Dancing Queen.flac"))
        assertFalse(card.files.containsKey("Music/ABBA/Gold/02 Knowing Me.flac"))
        assertTrue(card.files.containsKey("Music/ABBA/Gold/booklet.pdf"))
        assertTrue(card.files.containsKey("Music/ABBA/Gold/live.mp4"))
    }

    @Test
    fun albumFolderWithLyricsAndArtworkGoesWithIt() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.fullRescan = false
        val m = libraryOnCard(env)
        for (p in listOf("Music/ABBA/Gold/01 Dancing Queen.lrc", "Music/ABBA/Gold/album.cue", "Music/ABBA/Gold/.DS_Store")) card.files[p] = p.toByteArray()
        m.libraryDelete(LibTarget.AlbumOf(lib(m).albums.first { it.title == "Gold" }))
        m.confirmLibraryDelete()
        assertFalse(card.folders.contains("Music/ABBA/Gold"))
        assertTrue(card.files.keys.none { it.startsWith("Music/ABBA/Gold/") })
    }

    @Test
    fun movingAnAlbumOutOfASharedFolderMovesOnlyItsSongs() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Music/Beatles"
        card.files["Music/Mix/a.lrc"] = "lyrics".toByteArray()
        val m = libraryOnCard(env)
        val hits = lib(m).albums.first { it.title == "Hits" }
        // its folder is shared with another album's song
        assertEquals("Music/Mix", LibTarget.AlbumOf(hits).cardPath)
        m.libraryMove(LibTarget.AlbumOf(hits))
        val b = m.state.browser
        assertEquals("Music", b.path)
        assertEquals(listOf("Music/Mix/a.mp3", "Music/Mix/a.lrc"), b.movingFiles)
        assertEquals("Hits", b.movingLabel)
        assertFalse(m.state.busy)
        m.openFolder("Music/Beatles")
        m.moveHere()
        assertTrue(card.files.containsKey("Music/Beatles/a.mp3"))
        assertTrue(card.files.containsKey("Music/Beatles/a.lrc"))
        assertTrue(card.files.containsKey("Music/Mix/b.mp3"))
        assertTrue(card.folders.contains("Music/Mix"))
        assertFalse(card.folders.contains("Music/Beatles/Mix"))
        assertNull(m.state.browser.moving)
        assertTrue(m.state.browser.movingFiles.isEmpty())
    }

    @Test
    fun playlistEditsKeepWhatTheAppDoesNotRead() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Playlists"
        val cp1252 = charset("windows-1252")
        val file = "Playlists/Old.m3u"
        val text = "#EXTM3U\r\n#EXTINF:-1,Beyonc\u00e9 - Halo\r\n..\\M\u00fasica\\Beyonc\u00e9\\01.mp3\r\n#EXTINF:-1,Radio\r\nhttp://radio.example/stream\r\n" +
            "C:\\Users\\Me\\Music\\x.mp3\r\n#EXTGENRE:Pop\r\n../Music/loose.mp3\r\n"
        card.files[file] = text.toByteArray(cp1252)
        val m = libraryOnCard(env)
        m.openPlaylist(file)
        assertEquals(listOf("M\u00fasica/Beyonc\u00e9/01.mp3", "Music/loose.mp3"), m.state.playlists.open!!.songs)
        m.addToPlaylist(LibTarget.Song(lib(m).tracks.first { it.title == "B" }))
        m.addToPlaylistFile(file)
        var now = String(card.files[file]!!, cp1252)
        assertEquals(text + "#EXTINF:-1,Other - B\r\n../Music/Mix/b.mp3\r\n", now)
        assertEquals(3, m.state.playlists.open!!.songs.size)
        // removing the first song takes its two lines only; the URL, the PC path and the tag stay
        m.removeFromPlaylist(0)
        now = String(card.files[file]!!, cp1252)
        assertFalse(now.contains("Beyonc"))
        assertTrue(now.contains("#EXTINF:-1,Radio\r\nhttp://radio.example/stream\r\n"))
        assertTrue(now.contains("C:\\Users\\Me\\Music\\x.mp3\r\n#EXTGENRE:Pop\r\n"))
        assertEquals(listOf("Music/loose.mp3", "Music/Mix/b.mp3"), m.state.playlists.open!!.songs)
        // a UTF-8 file stays UTF-8, with its BOM
        val utf = byteArrayOf(0xEF.toByte(), 0xBB.toByte(), 0xBF.toByte()) + "#EXTM3U\n../Canci\u00f3n.flac\n".toByteArray()
        val f = M3u.parse("Playlists", utf).adding(listOf("Music/\u00c1.mp3" to ""))
        assertEquals("Canci\u00f3n.flac", f.songs[0])
        assertTrue(f.bytes().contentEquals(utf + "../Music/\u00c1.mp3\n".toByteArray()))
        // a legacy file that cannot hold a new name is written as UTF-8
        val legacy = M3u.parse("Playlists", "..\\Caf\u00e9.mp3\n".toByteArray(cp1252)).adding(listOf("Music/\u65e5\u672c.mp3" to ""))
        assertEquals(listOf("Caf\u00e9.mp3", "Music/\u65e5\u672c.mp3"), M3u.parse("Playlists", legacy.bytes()).songs)
    }

    @Test
    fun aStoppedTransferFollowsANewDestination() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        m.setDestination(Destination.MUSIC)
        m.addPicked { listOf(item("Live", "a.flac"), item("Live", "b.flac", data = ByteArray(300_000) { it.toByte() }), item("Live", "c.flac")) }
        card.dropAfter = 200_000
        env.c.down = true
        m.startSend()
        env.c.down = false
        assertEquals(JobStatus.SENT, m.state.transfer.jobs[0].status)
        // c.flac still waits: the screen offers the destination and the pickers again
        assertFalse(m.state.transfer.finished)
        m.connectTo(env.c.device.copy(paired = true))
        // a new destination and one more file: the sent file stays, the rest follows the choice
        m.setDestination(Destination.BOOKS)
        m.addPicked { listOf(item("notes.txt")) }
        val jobs = m.state.transfer.jobs
        assertEquals(listOf("Music/Live/a.flac", "Books/Live/b.flac", "Books/Live/c.flac", "Books/notes.txt"), jobs.map { it.remote })
        assertEquals(JobStatus.SENT, jobs[0].status)
        assertEquals(4, jobs.map { it.id }.toSet().size)
        m.startSend()
        assertTrue(m.state.transfer.finished)
        assertEquals(0, m.state.transfer.problems)
        assertTrue(card.files.containsKey("Books/Live/b.flac"))
        assertTrue(card.files.containsKey("Books/Live/c.flac"))
        assertTrue(card.files.containsKey("Books/notes.txt"))
        assertFalse(card.files.containsKey("Music/Live/b.flac"))
        assertFalse(card.files.containsKey("Music/Live/c.flac"))
    }

    @Test
    fun aRefusedReconnectionEndsTheSession() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = paired(env)
        m.setDestination(Destination.MUSIC)
        m.addPicked { listOf(item("Live", "b.flac", data = ByteArray(300_000) { it.toByte() })) }
        card.dropAfter = 200_000
        // the player forgot this phone while it slept
        env.c.forgotten = true
        m.startSend()
        assertEquals(R.string.sync_err_forgotten, m.state.transfer.error?.title)
        assertTrue(m.state.link is Link.Idle)
        assertFalse(m.state.busy)
    }

    @Test
    fun anotherPlayerStartsWithoutTheOldLibrary() {
        val env = Env(FakeConnector())
        val m = libraryOnCard(env)
        val gold = lib(m).albums.first { it.title == "Gold" }
        m.addToPlaylist(null)
        m.loadPlaylists()
        assertNotNull(m.state.playlists.files)
        // the same player again keeps what was read
        m.connectTo(env.c.device.copy(paired = true))
        assertTrue(m.state.library is LibraryPhase.Ready)
        // another player: nothing of the first one's card is kept
        env.c.device = env.c.device.copy(deviceId = "fedcba9876543210", name = "Other")
        m.connectTo(env.c.device)
        m.setCode(env.c.code)
        m.confirmCode()
        assertEquals("Other", m.state.connected!!.device.name)
        assertEquals(LibraryPhase.Idle, m.state.library)
        assertEquals(Playlists(), m.state.playlists)
        assertTrue(m.state.covers.isEmpty())
        m.libraryDelete(LibTarget.AlbumOf(gold))
        m.confirmLibraryDelete()
        assertTrue(env.c.card.files.containsKey("Music/ABBA/Gold/01 Dancing Queen.flac"))
    }

    @Test
    fun filesSharedDuringATransferWaitForItsEnd() {
        val base = Env(FakeConnector())
        val env = Queued(base)
        val m = pairedWith(env, base.c)
        m.setDestination(Destination.MUSIC)
        m.addPicked { listOf(item("a.flac")) }
        env.hold = true
        m.startSend()
        assertTrue(m.state.transfer.running)
        m.addPicked { listOf(item("photo.jpg")) }
        assertTrue(m.state.sharedWaiting)
        env.run()
        assertTrue(base.c.card.files.containsKey("Music/a.flac"))
        assertEquals(listOf("Music/photo.jpg"), m.state.transfer.jobs.map { it.remote })
        assertFalse(m.state.sharedWaiting)
    }

    @Test
    fun playlistsAreNotLeftLoadingWhileTheLibraryIsBusy() {
        val base = Env(FakeConnector())
        val env = Queued(base)
        val m = pairedWith(env, base.c)
        base.c.sessions.last().snapshot = "SQLite format 3\u0000".toByteArray()
        m.loadLibrary()
        env.hold = true
        m.rescanLibrary()
        assertTrue(m.state.busy)
        m.openPlaylist("Playlists/x.m3u8")
        assertNull(m.state.libraryNav.playlist)
        m.loadPlaylists()
        assertNull(m.state.playlists.files)
        env.run()
        assertFalse(m.state.busy)
        m.loadPlaylists()
        assertEquals(emptyList<String>(), m.state.playlists.files)
    }

    @Test
    fun aCopyThatDoesNotFinishLeavesNoFileOnThePhone() {
        val env = Env(FakeConnector())
        val card = env.c.card
        card.folders += "Recordings"
        card.files["Recordings/talk.wav"] = ByteArray(1000) { it.toByte() }
        val m = paired(env)
        var discarded = 0
        // the file went away on the card
        m.openFolder("Recordings")
        m.saveToPhone(m.state.browser.entries!!.single())
        card.files.clear()
        m.downloadTo(discard = { discarded++ }) { java.io.ByteArrayOutputStream() }
        assertEquals(1, discarded)
        // nothing was asked for (or the session is gone): the empty document goes too
        m.downloadTo(discard = { discarded++ }) { java.io.ByteArrayOutputStream() }
        assertEquals(2, discarded)
        // a whole copy stays
        card.files["Recordings/talk.wav"] = ByteArray(1000) { it.toByte() }
        m.clearDownload()
        m.saveToPhone(SyncEntry("talk.wav", 0, 1000, 0))
        m.downloadTo(discard = { discarded++ }) { java.io.ByteArrayOutputStream() }
        assertEquals(2, discarded)
        assertTrue(m.state.browser.download!!.finished)
    }

    @Test
    fun replacedArtworkIsReadAgain() {
        val env = Env(FakeConnector())
        val card = env.c.card
        val m = libraryOnCard(env)
        val folder = Library.artFolder(lib(m).albums.first { it.title == "Gold" })!!
        m.loadCover(folder)
        val first = m.state.covers[folder]!!
        card.files["Music/ABBA/Gold/cover.jpg"] = "a new, larger picture".toByteArray()
        m.disconnect()
        m.connectTo(env.c.device.copy(paired = true))
        m.loadCover(folder)
        val second = m.state.covers[folder]!!
        assertEquals("a new, larger picture", String(File(second).readBytes()))
        assertFalse(File(first).exists())
    }

    // ---- signed update (sections 14.1 to 14.3) ----------------------------------------------

    private class Release(val json: ByteArray, val tar: ByteArray, val key: String, val version: String)

    /** A release signed with a fresh key, as the release tooling makes it (docs/OTA.md). */
    private fun release(version: String, restart: String = "shell", tarSize: Int = 150_000, needsFirmware: Boolean = false): Release {
        val tar = ByteArray(tarSize) { (it * 13 + version.length).toByte() }
        val kp = java.security.KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
        val key = kp.public.encoded.takeLast(32).joinToString("") { "%02x".format(it) }
        val zeros = "0".repeat(128)
        val hex = sha256(tar).joinToString("") { "%02x".format(it) }
        val text = "{\"format\": 1, \"product\": \"hibyos\", \"version\": \"$version\", \"restart\": \"$restart\", " +
            "\"needs_firmware\": $needsFirmware, \"payload\": {\"name\": \"hibyos-payload-$version.tar\", " +
            "\"url\": \"https://example.com/hibyos-payload-$version.tar\", \"size\": ${tar.size}, \"sha256\": \"$hex\", " +
            "\"compression\": \"none\"}, \"signature\": \"ed25519:$zeros\"}"
        val sig = java.security.Signature.getInstance("Ed25519").apply { initSign(kp.private); update(text.toByteArray()) }.sign()
        return Release(text.replace(zeros, sig.joinToString("") { "%02x".format(it) }).toByteArray(), tar, key, version)
    }

    private fun updateEnv(r: Release): Env {
        val env = Env(FakeConnector())
        env.keys = listOf(r.key)
        env.c.card.updateKeys = listOf(r.key)
        return env
    }

    @Test
    fun signedUpdateIsSentStagedAndInstalled() {
        val r = release("0.9.1")
        val env = updateEnv(r)
        val card = env.c.card
        val m = paired(env)
        // the description alone first, then the tar: both are kept
        m.addUpdateFiles { listOf(item("hibyos-update.json", data = r.json)) }
        assertEquals("0.9.1", m.state.otaPick.version)
        assertFalse(m.state.otaPick.ready)
        m.addUpdateFiles { listOf(item("hibyos-payload-0.9.1 (1).tar", data = r.tar)) }
        assertTrue(m.state.otaPick.ready)
        assertEquals("shell", m.state.otaPick.restart)
        card.statusScript += listOf(
            UpdateProgress(UpdateProgress.WAITING_USER, 0, 0, "0.9.1"),
            UpdateProgress(UpdateProgress.INSTALLING, 420, 0, "0.9.1"),
            UpdateProgress(UpdateProgress.INSTALLING, 990, 0, "0.9.1"),
            null, // the shell restarts with the new ByOS: the connection drops
        )
        m.installUpdate(false)
        assertEquals(OtaPhase.Done("0.9.1", 1), m.state.ota)
        // the tar is stored under the name the signed description gives, whatever its name on the phone
        assertTrue(card.update.containsKey("hibyos-payload-0.9.1.tar"))
        assertEquals("0.9.1", card.staged)
        assertEquals(1, card.installs)
        assertTrue(env.slept >= 3000)
        // the session ended with the restart: the user connects again later
        assertTrue(m.state.link is Link.Idle)
        assertNull(m.state.error)
        assertFalse(m.state.busy)
    }

    @Test
    fun updateFilesAreCheckedOnThePhone() {
        val r = release("0.9.1")
        val env = updateEnv(r)
        val m = paired(env)
        // signed by another key: refused at once, nothing is sent
        env.keys = listOf(release("0.9.2").key)
        m.addUpdateFiles { listOf(item("hibyos-update.json", data = r.json)) }
        assertEquals(R.string.sync_err_signature, m.state.otaPick.error?.title)
        assertNull(m.state.otaPick.version)
        // a file that is not part of an update
        env.keys = listOf(r.key)
        m.addUpdateFiles { listOf(item("song.flac")) }
        assertEquals(R.string.sync_err_update_kind, m.state.otaPick.error?.title)
        // a tar of another size is shown as not matching; one of the right size but other content fails before sending
        m.addUpdateFiles { listOf(item("hibyos-update.json", data = r.json), item("other.tar", data = ByteArray(10))) }
        assertNull(m.state.otaPick.error)
        assertTrue(m.state.otaPick.tarMismatch)
        assertFalse(m.state.otaPick.ready)
        m.addUpdateFiles { listOf(item("other.tar", data = ByteArray(r.tar.size))) }
        assertTrue(m.state.otaPick.ready)
        m.installUpdate(false)
        val f = m.state.ota as OtaPhase.Failed
        assertEquals(R.string.sync_err_update_payload, f.error.title)
        assertTrue(env.c.card.update.isEmpty())
        m.clearUpdateFiles()
        assertEquals(OtaPick(), m.state.otaPick)
        assertEquals(OtaPhase.Idle, m.state.ota)
    }

    @Test
    fun olderUpdateNeedsConsentAndIsNotSentTwice() {
        val r = release("0.8.0", restart = "reboot")
        val env = updateEnv(r)
        val card = env.c.card
        val m = paired(env)
        m.addUpdateFiles { listOf(item("hibyos-update.json", data = r.json), item("hibyos-payload-0.8.0.tar", data = r.tar)) }
        m.installUpdate(false)
        val f = m.state.ota as OtaPhase.Failed
        assertTrue(f.downgrade)
        assertEquals(R.string.sync_err_downgrade, f.error.title)
        val puts = card.puts
        card.statusScript += listOf(UpdateProgress(UpdateProgress.WAITING_USER, 0, 0, "0.8.0"), UpdateProgress(UpdateProgress.REFUSED, 0, 0, "0.8.0"))
        m.installUpdate(true)
        assertEquals(puts, card.puts)
        assertEquals(OtaPhase.Refused("0.8.0"), m.state.ota)
        assertTrue(m.state.link is Link.Connected)
        // installing again after a refusal sends the description again (the player moved it when it checked it)
        card.statusScript.clear()
        card.statusScript += UpdateProgress(UpdateProgress.DONE, 1000, 0, "0.8.0")
        m.installUpdate(true)
        assertTrue(card.puts > puts)
        assertEquals(OtaPhase.Done("0.8.0", 2), m.state.ota)
        assertTrue(m.state.link is Link.Idle)
    }

    @Test
    fun updateErrorsFromThePlayer() {
        val r = release("0.9.1", restart = "none")
        val env = updateEnv(r)
        val card = env.c.card
        val m = paired(env)
        m.addUpdateFiles { listOf(item("hibyos-update.json", data = r.json), item("p.tar", data = r.tar)) }
        card.statusScript += listOf(UpdateProgress(UpdateProgress.INSTALLING, 100, 0, "0.9.1"),
            UpdateProgress(UpdateProgress.FAILED, 0, SyncException.E_BATTERY, "0.9.1"))
        m.installUpdate(false)
        val f = m.state.ota as OtaPhase.Failed
        assertEquals(R.string.sync_err_battery, f.error.title)
        assertFalse(f.downgrade)
        // ByOS Sync running without the ByOS screen: staged, to install from Software Update
        card.installUnsupported = true
        m.installUpdate(false)
        assertEquals(OtaPhase.StagedOnly("0.9.1"), m.state.ota)
        // "stop waiting" ends the watching only; the session stays
        card.installUnsupported = false
        card.statusScript.clear()
        card.statusScript += UpdateProgress(UpdateProgress.WAITING_USER, 0, 0, "0.9.1")
        env.onSleep = { m.cancelWork() }
        m.installUpdate(false)
        assertEquals(OtaPhase.Idle, m.state.ota)
        assertTrue(m.state.link is Link.Connected)
        // without UPDATE_STATUS answers for too long, the app stops watching and says so
        env.onSleep = null
        m.installUpdate(false)
        assertEquals(R.string.sync_err_update_slow, (m.state.ota as OtaPhase.Failed).error.title)
    }

    @Test
    fun withoutAClientNothingStarts() {
        val env = object : SyncEnv {
            override fun connector(): SyncConnector? = null
            override val clientName = "x"
            override val cacheDir = File(".")
            override fun post(f: () -> Unit) = f()
            override fun execute(f: () -> Unit) = f()
            override fun readLibrary(file: File) = emptyList<Track>()
            override fun pickFiles() {}
            override fun pickFolder() {}
            override fun byosFiles(): List<SendItem>? = null
        }
        val m = ConnectedModel(env)
        assertFalse(m.state.available)
        m.findDevices()
        assertTrue(m.state.link is Link.Idle)
        assertNotNull(m.state)
    }
}
