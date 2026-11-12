package org.hibyos.install.core.sync

import org.hibyos.install.core.Json
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.testing.MemCardFs
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.file.Files

/** The apply engine and the scanner against a card in memory and real folders in a temporary directory. */
class SyncApplyTest {
    @get:Rule val tmp = TemporaryFolder()

    private val t0 = 1_700_000_000L
    private val nfdName = "Beyoncé.flac"
    private val nfcName = SyncRules.nfc(nfdName)

    /** Thrown by a test hook: stops a run in the middle, as a killed process would. */
    class Kill : Error("killed")

    private inner class Env(layout: String = "keep", policy: String = "keep") {
        val cfg: File = tmp.newFolder()
        val local: File = tmp.newFolder()
        val store = SyncStore(cfg, "dev1")
        val link = Link("L1", Kinds.MUSIC, local.path, "Music", layout, playerOnly = policy)
        val lf = LinksFile(SyncStore.clientId(cfg), "PC", mutableListOf(link))
        val fs = MemCardFs()
        val warns = ArrayList<String>()
        val listener = object : SyncListener {
            override fun warn(code: String, message: String, extra: Map<String, Any?>) { warns += code }
        }

        fun file(rel: String, data: String, mtime: Long = t0): File =
            File(local, rel).also { it.parentFile.mkdirs(); it.writeText(data); it.setLastModified(mtime * 1000) }

        fun scan(): LocalScan = LocalScanner.scan(link, store.loadScan(link.id)).also { store.saveScan(it) }

        fun plan(deleteUntracked: Boolean = false): Map<String, Any?> =
            SyncPlanning.plan(fs, store, lf, mapOf(link.id to scan()), PlanRequest(listOf(Kinds.MUSIC), deleteUntracked), listener)

        fun items(plan: Map<String, Any?>) = Json.arr(plan["items"]).map { PlanItem.fromMap(Json.obj(it)) }

        fun apply(plan: Map<String, Any?>, dec: Map<String, Any?> = emptyMap(), opts: ApplyOptions = ApplyOptions("PC")): ApplyResult =
            SyncApply(fs, store, lf, opts, listener).run(plan, Decisions.resolve(items(plan), dec))

        /** A plan made by hand for this card. */
        fun planOf(vararg items: PlanItem, cardId: String? = CardStore.ensureCardId(fs)): Map<String, Any?> =
            linkedMapOf("format" to 2, "card_id" to cardId, "run_id" to SyncPlanning.newRunId(), "kinds" to listOf(Kinds.MUSIC),
                "items" to items.map { it.toMap() })
    }

    private fun move(id: String, rel: String, from: String, to: String) = PlanItem(id, Kinds.MUSIC, "L1", "move", "dest_changed", rel, to, from)

    // ------------------------------------------------------------------ lock

    @Test
    fun aSecondRunOfThisComputerIsRefusedAndAKilledOneTakenOver() {
        val env = Env()
        env.file("a.flac", "aaaa")
        val p = env.plan()
        val other = LocalRunLock.tryAcquire(env.store.runLockFile)!!
        try {
            val e = try { env.apply(p); null } catch (x: SyncException) { x }
            assertEquals("locked", e?.code)
            assertTrue(env.fs.files.keys.none { it.startsWith("Music/") })
        } finally { other.close() }
        // a lock of this computer whose run is gone (no OS lock held) is taken over at once
        CardStore.writeJson(env.fs, CardStore.LOCK, LockInfo(env.lf.clientId, "PC", "old-run", listOf("music"), t0, System.currentTimeMillis() / 1000).toMap())
        assertEquals("done", env.apply(p).status)
        assertTrue(env.warns.contains("lock_taken_over"))
        assertNull("the lock is released", env.fs.files[CardStore.LOCK])
    }

    @Test
    fun aLongTransferIsAnnouncedSoTheLockDoesNotGoStale() {
        val env = Env()
        env.file("big.flac", "x".repeat(1000))
        val p = env.plan()
        var during: LockInfo? = null
        env.fs.onPut = { path -> if (path == "Music/big.flac") during = CardStore.readLock(env.fs) }
        // at 1 byte per second, 1000 bytes may take longer than the 10 minutes after which a lock is stale
        assertEquals("done", env.apply(p, opts = ApplyOptions("PC", slowRate = 1)).status)
        val l = during!!
        val now = System.currentTimeMillis() / 1000
        assertTrue("busy_until ${l.busyUntil}", l.busyUntil >= now + 1000)
        assertFalse(l.stale(now + LockInfo.STALE_S + 100))
        assertTrue(l.stale(l.busyUntil + LockInfo.STALE_S + 1))
        assertEquals(l.busyUntil, LockInfo.fromMap(l.toMap()).busyUntil)
        // a quick transfer leaves the heartbeat alone
        assertTrue(LockInfo("c", "n", "r", emptyList(), 0, 100).stale(100 + LockInfo.STALE_S + 1))
    }

    // ------------------------------------------------------------------ card id

    @Test
    fun everyPlanNamesItsCardAndAPlanWithoutOneIsRefused() {
        val env = Env()
        env.file("a.flac", "aaaa")
        val p = env.plan()
        assertNotNull("a first plan writes card.json", p["card_id"])
        assertEquals(p["card_id"], CardStore.cardId(env.fs))
        val e = try { env.apply(env.planOf(cardId = null)); null } catch (x: SyncException) { x }
        assertEquals("card_changed", e?.code)
        // another card (another card.json) refuses the plan too
        env.fs.files.remove(CardStore.CARD)
        val e2 = try { env.apply(p); null } catch (x: SyncException) { x }
        assertEquals("card_changed", e2?.code)
        assertTrue(env.fs.files.keys.none { it.startsWith("Music/") })
    }

    // ------------------------------------------------------------------ moves

    private fun swapEnv(): Env {
        val env = Env()
        env.fs.write("Music/a.flac", "AAAA".toByteArray(), t0)
        env.fs.write("Music/b.flac", "BBBBBB".toByteArray(), t0)
        return env
    }

    private fun swapPlan(env: Env) = env.planOf(move("i1", "a", "Music/a.flac", "Music/b.flac"), move("i2", "b", "Music/b.flac", "Music/a.flac"))

    private fun assertSwapped(env: Env) {
        assertEquals("BBBBBB", String(env.fs.bytes("Music/a.flac")!!))
        assertEquals("AAAA", String(env.fs.bytes("Music/b.flac")!!))
        assertTrue(env.fs.files.keys.toString(), env.fs.files.keys.none { it.contains("byos-move") })
        assertTrue("nothing was sent again", env.fs.log.none { it.startsWith("PUT Music/") })
    }

    @Test
    fun aSwapResumesAfterAStopRightAfterTheFirstTemporaryMove() {
        val env = swapEnv()
        val p = swapPlan(env)
        try { env.apply(p, opts = ApplyOptions("PC", onRecord = { if (it == 1) throw Kill() })); throw AssertionError("not killed") } catch (k: Kill) {}
        assertNull(env.fs.files["Music/a.flac"])
        assertEquals("done", env.apply(p).status)
        assertSwapped(env)
    }

    @Test
    fun aSwapResumesWhenOnlyItsSecondHalfIsLeft() {
        val env = swapEnv()
        val p = swapPlan(env)
        // stopped after both temporary moves and the first final one
        try { env.apply(p, opts = ApplyOptions("PC", onRecord = { if (it == 3) throw Kill() })); throw AssertionError("not killed") } catch (k: Kill) {}
        assertEquals("AAAA", String(env.fs.bytes("Music/b.flac")!!))
        assertEquals("done", env.apply(p).status)
        assertSwapped(env)
    }

    @Test
    fun aDeviceErrorOnAMoveFailsThatItemOnly() {
        val env = Env()
        env.fs.write("Music/a.flac", "A".toByteArray(), t0)
        env.fs.write("Music/b.flac", "B".toByteArray(), t0)
        env.fs.onMove = { src, dst ->
            when (src) {
                // a MOVE retried after the link dropped: the first try had moved the file already
                "Music/a.flac" -> { env.fs.files[dst] = env.fs.files.remove(src)!!; throw BtpException(Btp.E_NOT_FOUND, src, remote = true) }
                "Music/b.flac" -> throw BtpException(Btp.E_IO, "write error", remote = true)
            }
        }
        val r = env.apply(env.planOf(move("i1", "a", "Music/a.flac", "Music/X/a.flac"), move("i2", "b", "Music/b.flac", "Music/X/b.flac")))
        assertEquals("done_with_problems", r.status)
        assertEquals(1, r.kinds["music"]!!.moved)
        assertEquals(listOf("i2"), r.failures.map { it["id"] })
        assertEquals("A", String(env.fs.bytes("Music/X/a.flac")!!))
        assertEquals("B", String(env.fs.bytes("Music/b.flac")!!))
    }

    // ------------------------------------------------------------------ deletes

    @Test
    fun aDeleteKeepsAFileEditedOnThePlayerWithTheSameSize() {
        val env = Env()
        env.fs.write("Music/x.flac", "XXXX".toByteArray(), t0)
        env.fs.write("Music/y.flac", "YYYY".toByteArray(), t0)
        fun del(id: String, dest: String) = PlanItem(id, Kinds.MUSIC, "L1", "player_only", PlayerOnly.DELETED, null, dest, null, 4, 0, 4, t0,
            defaultChoice = "keep", choices = listOf("delete", "keep", "pull"))
        val p = env.planOf(del("i1", "Music/x.flac"), del("i2", "Music/y.flac"))
        // the player's tag editor rewrote x.flac in place: same size, new time
        env.fs.files["Music/x.flac"]!!.mtime = t0 + 120
        val r = env.apply(p, mapOf("i1" to "delete", "i2" to "delete"))
        assertEquals(1, r.kinds["music"]!!.removed)
        assertNotNull(env.fs.files["Music/x.flac"])
        assertNull(env.fs.files["Music/y.flac"])
        assertTrue(env.warns.contains("changed_since_plan"))
    }

    // ------------------------------------------------------------------ Unicode names

    @Test
    fun anNfdNameOnTheCardIsDeletedAndAdoptedUnderItsOwnSpelling() {
        val env = Env()
        env.file("keep.flac", "kkkk")
        env.fs.write("Music/x/$nfdName", "player".toByteArray(), t0)
        val p = env.plan(deleteUntracked = true)
        val po = env.items(p).single { it.action == "player_only" }
        assertEquals("Music/x/$nfdName", po.dest)
        assertEquals("delete", po.defaultChoice)
        env.apply(p)
        assertNull("the NFD file itself was deleted", env.fs.files["Music/x/$nfdName"])

        // the same song on the computer (NFC) and on the card (NFD): adopted, not copied a second time
        val env2 = Env()
        env2.file(nfcName, "same bytes")
        env2.fs.write("Music/$nfdName", "same bytes".toByteArray(), t0)
        val q = env2.plan()
        val adopt = env2.items(q).single()
        assertEquals("adopt", adopt.action)
        assertEquals("the manifest keeps the card's own spelling", "Music/$nfdName", adopt.dest)
        env2.apply(q)
        assertTrue(env2.fs.log.none { it.startsWith("PUT Music/") })
        assertEquals("nothing left to do", 0, env2.items(env2.plan()).size)
        assertEquals(setOf("Music/$nfdName"), env2.fs.files.keys.filter { it.startsWith("Music/") }.toSet())
    }

    @Test
    fun anNfdNameOnThisComputerIsScannedAndSent() {
        val env = Env()
        val f = File(env.local, nfdName)
        f.writeText("song")
        assumeTrue("the file system keeps NFD names", env.local.list()!!.contains(nfdName))
        val s = env.scan()
        val e = s.entries[nfcName]!!
        assertEquals(nfdName, e.disk)
        assertEquals(nfdName, LocalScan.fromMap(Json.obj(Json.parse(Json.write(s.toMap())))).entries[nfcName]!!.disk)
        val p = env.plan()
        assertEquals("add", env.items(p).single().action)
        val r = env.apply(p)
        assertEquals(r.failures.toString(), 1, r.kinds["music"]!!.written)
        assertArrayEquals("song".toByteArray(), env.fs.bytes("Music/$nfcName"))
        assertEquals(0, env.items(env.plan()).size)
    }

    // ------------------------------------------------------------------ scanner

    /** A directory link at [link] to [target]: a symbolic link, or a junction on Windows without the right to make one. */
    private fun dirLink(link: File, target: File): Boolean {
        try { Files.createSymbolicLink(link.toPath(), target.toPath()); return true } catch (e: Exception) {}
        if (!System.getProperty("os.name").lowercase().contains("win")) return false
        val p = ProcessBuilder("cmd", "/c", "mklink", "/J", link.path, target.path).redirectErrorStream(true).start()
        p.inputStream.readBytes()
        return p.waitFor() == 0 && link.isDirectory
    }

    @Test
    fun linkedFoldersAreFollowedAndABrokenOneKeepsItsFiles() {
        val env = Env()
        env.file("A/1.flac", "1")
        val jazz = tmp.newFolder("Jazz")
        File(jazz, "x.flac").writeText("jazz")
        val jazzLink = File(env.local, "Jazz")
        val up = File(jazz, "Up")
        assumeTrue("folder links can be made here", dirLink(jazzLink, jazz))
        try {
            // a link back up must not loop
            dirLink(up, env.local)
            val s = env.scan()
            assertTrue(s.entries.keys.toString(), "Jazz/x.flac" in s.entries)
            assertTrue("A/1.flac" in s.entries)
            assertEquals(s.entries.keys.toString(), 2, s.entries.size)
            assertEquals(0, s.errors)
            // the drive the link points to is gone: nothing under it counts as deleted
            Files.deleteIfExists(up.toPath())
            File(jazz, "x.flac").delete()
            assertTrue(jazz.delete())
            val s2 = env.scan()
            assertTrue(s2.unreadableDirs.toString(), "Jazz" in s2.unreadableDirs)
            assertTrue(s2.underUnreadable("Jazz/x.flac"))
        } finally {
            // links only: never what they point to
            try { Files.deleteIfExists(up.toPath()) } catch (e: Exception) {}
            try { Files.deleteIfExists(jazzLink.toPath()) } catch (e: Exception) {}
        }
    }

    @Test
    fun aTagReadErrorIsRecordedAndKeepsTheLastTags() {
        val env = Env(layout = "artist-album")
        env.file("A/1.flac", "1")
        env.file("A/2.flac", "2")
        val good = LocalTags("T", "Band", null, "Album", null, 1, 0, 0, 0)
        val first = LocalScanner.scan(env.link, null, readTags = { good })
        assertFalse(first.entries.values.any { it.tagError })
        env.file("A/1.flac", "1 retagged", t0 + 10)
        val second = LocalScanner.scan(env.link, first, readTags = { f -> if (f.name == "1.flac") throw java.io.IOException("locked") else good })
        val e = second.entries["A/1.flac"]!!
        assertTrue(e.tagError); assertEquals("Band", e.tags?.artist)
        assertEquals(1, second.tagErrors)
        assertTrue(LocalScan.fromMap(Json.obj(Json.parse(Json.write(second.toMap())))).entries["A/1.flac"]!!.tagError)
        // the next scan reads it again although its size and time did not change
        val third = LocalScanner.scan(env.link, second, readTags = { good })
        assertFalse(third.entries["A/1.flac"]!!.tagError)
        // an unknown format has no tags, which is not an error
        val ogg = File(env.local, "A/3.ogg").also { it.writeText("OggS") }
        assertNull(LocalScanner.tagsOf(ogg))
        try { LocalScanner.tagsOf(File(env.local, "A/missing.flac")); throw AssertionError("no error for a missing file") } catch (x: java.io.IOException) {}
    }

    @Test
    fun volumeIdsOfAnotherKindAreNotCompared() {
        assertTrue(LocalScanner.sameScheme("dev:1", "dev:2"))
        assertFalse(LocalScanner.sameScheme("dev:1", "uuid:ab"))
        assertFalse(LocalScanner.sameScheme(null, "uuid:ab"))
        assertNull(LocalScanner.linuxUuid("C:"))
        assertEquals(SyncRules.lower("Music/$nfdName"), SyncRules.lower("MUSIC/$nfcName"))
    }
}
