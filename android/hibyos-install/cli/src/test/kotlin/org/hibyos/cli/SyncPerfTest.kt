package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.sync.ApplyOptions
import org.hibyos.install.core.sync.CardStore
import org.hibyos.install.core.sync.Decisions
import org.hibyos.install.core.sync.Kinds
import org.hibyos.install.core.sync.Link
import org.hibyos.install.core.sync.LinksFile
import org.hibyos.install.core.sync.LocalEntry
import org.hibyos.install.core.sync.LocalScan
import org.hibyos.install.core.sync.LocalScanner
import org.hibyos.install.core.sync.LocalTags
import org.hibyos.install.core.sync.Manifest
import org.hibyos.install.core.sync.PlanItem
import org.hibyos.install.core.sync.PlanRequest
import org.hibyos.install.core.sync.SyncApply
import org.hibyos.install.core.sync.SyncListener
import org.hibyos.install.core.sync.SyncPlanning
import org.hibyos.install.core.sync.SyncStore
import org.hibyos.install.core.sync.Tracked
import org.hibyos.install.core.testing.MemCardFs
import org.hibyos.install.core.testing.SynthAudio
import org.hibyos.install.core.testing.SynthAudio.Tags
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * The owner's library has more than 30,000 tracks (docs/SYNC_LIBRARY_DESIGN.md, "Owner
 * decisions"): the folder scan (real tags, then the incremental scan) and an apply with its
 * journal, card checkpoints and manifest fold, each at 50,000 tracks.
 */
class SyncPerfTest {
    @get:Rule val tmp = TemporaryFolder()

    private val n = 50_000
    private val t0 = 1_700_000_000L
    private fun secs(start: Long) = (System.nanoTime() - start) / 1e9

    private fun rel(i: Int) = "Artist ${i / 500}/Album ${i / 12}/${"%02d".format(i % 12 + 1)} Track $i.flac"

    @Test
    fun aFiftyThousandTrackScanReadsTagsOnceThenReusesThem() {
        val root = tmp.newFolder("Big")
        var start = System.nanoTime()
        for (i in 0 until n) {
            val a = "Artist ${i / 500}"
            SynthAudio.write(root, rel(i), SynthAudio.flac(Tags("Track $i", a, "Album ${i / 12}", track = (i % 12 + 1).toString()), audioBytes = 16), t0)
        }
        println("50k scan: files written in %.1f s".format(secs(start)))
        val store = SyncStore(tmp.newFolder("cfg"), "dev1")
        val link = Link("L1", Kinds.MUSIC, root.path, "Music", "artist-album")
        var reads = 0
        val readTags = { f: File -> reads++; LocalScanner.tagsOf(f) }
        start = System.nanoTime()
        val first = LocalScanner.scan(link, null, readTags = readTags)
        val tFirst = secs(start)
        assertEquals("ok", first.status)
        assertEquals(n, first.entries.size)
        assertEquals(n, reads)
        assertEquals(0, first.tagErrors)
        assertEquals("Artist 99", first.entries[rel(49_999)]!!.tags?.artist)
        start = System.nanoTime()
        store.saveScan(first)
        val loaded = store.loadScan(link.id)!!
        val tSave = secs(start)
        assertEquals(n, loaded.entries.size)
        reads = 0
        start = System.nanoTime()
        val second = LocalScanner.scan(link, loaded, readTags = readTags)
        val tSecond = secs(start)
        assertEquals("unchanged files are not read again", 0, reads)
        assertEquals(n, second.entries.size)
        println("50k scan: first %.1f s, local.json save+load %.1f s (%d bytes), incremental %.1f s"
            .format(tFirst, tSave, store.localFile(link.id).length(), tSecond))
        assertTrue("first scan of 50k tracks took $tFirst s", tFirst < 240)
        assertTrue("local.json of 50k tracks took $tSave s", tSave < 30)
        assertTrue("incremental scan of 50k tracks took $tSecond s", tSecond < 120)
    }

    @Test
    fun aFiftyThousandTrackApplyJournalsCheckpointsAndFolds() {
        // a layout change: all 50k tracks move on the card (no bytes to send, so the engine's own cost shows)
        val cfg = tmp.newFolder("cfg")
        val local = tmp.newFolder("Big")
        val store = SyncStore(cfg, "dev1")
        val link = Link("L1", Kinds.MUSIC, local.path, "Music", "keep")
        val lf = LinksFile(SyncStore.clientId(cfg), "PC", mutableListOf(link))
        val fs = MemCardFs()
        val man = Manifest(Kinds.MUSIC)
        val entries = LinkedHashMap<String, LocalEntry>()
        for (i in 0 until n) {
            val r = rel(i); val size = 30_000_000L + i
            entries[r] = LocalEntry(r, size, t0, LocalTags("Track $i", "Artist ${i / 500}", null, "Album ${i / 12}", null, i % 12 + 1, 0, 0, 0))
            val old = "Music/Old/$r"
            man.put(Tracked(old, link.id, r, lf.clientId, size, t0, size, null, t0, "keep", t0))
            fs.addVirtual(old, size, t0)
        }
        CardStore.writeJson(fs, CardStore.manifestPath(Kinds.MUSIC), man.toMap())
        val manBytes = fs.files[CardStore.manifestPath(Kinds.MUSIC)]!!.size
        val scan = LocalScan(link.id, local.path, "ok", null, entries)
        val listener = object : SyncListener {}
        var start = System.nanoTime()
        val plan = SyncPlanning.plan(fs, store, lf, mapOf(link.id to scan), PlanRequest(listOf(Kinds.MUSIC)), listener)
        val tPlan = secs(start)
        val items = Json.arr(plan["items"]).map { PlanItem.fromMap(Json.obj(it)) }
        assertEquals(n, items.count { it.action == "move" })
        start = System.nanoTime()
        val r = SyncApply(fs, store, lf, ApplyOptions("PC"), listener).run(plan, Decisions.resolve(items, emptyMap()))
        val tApply = secs(start)
        assertEquals(r.failures.toString(), "done", r.status)
        assertEquals(n, r.kinds["music"]!!.moved)
        assertEquals(n, fs.files.keys.count { it.startsWith("Music/Artist ") })
        assertTrue("the old folders are pruned", fs.dirs.none { it.startsWith("Music/Old") })
        start = System.nanoTime()
        val back = CardStore.loadManifest(fs, Kinds.MUSIC, lf.links, lf.clientId, listener)
        val tRead = secs(start)
        assertEquals(n, back.size)
        assertEquals("Music/${rel(123)}", back.forLink(link.id).single { it.rel == rel(123) }.dest)
        assertTrue("the folded run leaves no card checkpoint", CardStore.journals(fs).isEmpty())
        val journal = store.runFile(r.runId)
        println("50k apply: plan %.1f s, apply %.1f s (journal %d bytes), manifest %d bytes read back in %.1f s"
            .format(tPlan, tApply, journal.length(), manBytes, tRead))
        assertTrue("plan of 50k moves took $tPlan s", tPlan < 60)
        assertTrue("apply of 50k moves took $tApply s", tApply < 300)
        assertTrue("manifest read took $tRead s", tRead < 30)
    }
}
