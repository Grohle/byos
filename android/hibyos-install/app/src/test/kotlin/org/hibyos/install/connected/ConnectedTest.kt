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
        m.startSend()
        val t = m.state.transfer
        assertFalse(t.running)
        assertEquals(R.string.sync_err_lost, t.error?.title)
        assertTrue(m.state.link is Link.Idle)
        assertEquals(JobStatus.FAILED, t.jobs[0].status)
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
