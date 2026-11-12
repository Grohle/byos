package org.hibyos.install.core.sync

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The diff of docs/SYNC_LIBRARY_DESIGN.md section 3.3 on made-up folders and cards (no I/O). */
class FolderSyncTest {
    private val client = "c1"
    private val t0 = 1_700_000_000L

    private fun link(kind: String = Kinds.MUSIC, layout: String = if (kind == Kinds.MUSIC) "artist-album" else "keep", policy: String = "keep",
                     root: String = Kinds.defaultRoot(kind), exclude: List<String> = emptyList(), removeUnselected: Boolean = false,
                     volume: String? = null, selection: LinkSelection = LinkSelection()) =
        Link("L1", kind, "/music", root, layout, selection, removeUnselected, policy, true, exclude, volumeId = volume)

    private fun tags(artist: String, album: String, title: String, track: Int = 1, disc: Int = 0, genre: String? = null) =
        LocalTags(title, artist, null, album, genre, track, disc, 0, 0)

    private fun e(rel: String, size: Long = 1000, mtime: Long = t0, tags: LocalTags? = null) = LocalEntry(rel, size, mtime, tags)

    private fun scan(vararg es: LocalEntry, status: String = "ok", unreadable: List<String> = emptyList(), volume: String? = null) =
        LocalScan("L1", "/music", status, volume, es.associateBy { it.rel }, unreadable)

    private fun tr(rel: String, dest: String, size: Long = 1000, mtime: Long = t0, origin: String = client, sha: String? = null, covers: String = "optimize") =
        Tracked(dest, "L1", rel, origin, size, mtime, size, null, mtime, covers, t0, srcSha = sha)

    private fun card(path: String, size: Long = 1000, mtime: Long = t0) = CardFile(path, size, mtime)

    private fun diff(l: Link, s: LocalScan, tracked: List<Tracked> = emptyList(), cards: List<CardFile> = emptyList(),
                     others: Map<String, Tracked> = emptyMap(), fs: String = "exfat", deleteUntracked: Boolean = false,
                     localQfp: (String) -> String? = { null }, cardQfp: (CardFile) -> String? = { null }, localSha: (String) -> String? = { null }) =
        FolderSync.diff(DiffInput(l, s, tracked, others, cards, client, fs, emptyList(), deleteUntracked, null, localQfp, cardQfp, localSha))

    private fun LinkPlan.of(action: String) = items.filter { it.action == action }

    // ------------------------------------------------------------------ M1: identity, changes, moves

    @Test
    fun unchangedFilesAreOnlyCounted() {
        val es = (1..3).map { e("A/$it.flac", tags = tags("ABBA", "Gold", "Song $it", it)) }
        val tracked = (1..3).map { tr("A/$it.flac", "Music/ABBA/Gold/0$it Song $it.flac") }
        val p = diff(link(), scan(*es.toTypedArray()), tracked, tracked.map { card(it.dest) })
        assertEquals(3, p.skip)
        assertTrue(p.items.toString(), p.items.isEmpty())
    }

    @Test
    fun relinkKeepsIdentityByRelativePath() {
        // the folder moved from D:\Music to E:\Music: only link.local changes, rel paths are the same
        val es = (1..3).map { e("A/$it.flac", tags = tags("ABBA", "Gold", "Song $it", it)) }
        val tracked = (1..3).map { tr("A/$it.flac", "Music/ABBA/Gold/0$it Song $it.flac") }
        val l = link().apply { local = "E:\\Music" }
        val p = diff(l, LocalScan("L1", "E:\\Music", "ok", null, es.associateBy { it.rel }), tracked, tracked.map { card(it.dest) })
        assertEquals(0, p.counts["add"]); assertEquals(3, p.skip)
    }

    @Test
    fun sameSizeRetagIsAnUpdateAndATouchIsNot() {
        val t = tr("A/1.flac", "Music/ABBA/Gold/01 SOS.flac", sha = "aaa")
        val retagged = e("A/1.flac", mtime = t0 + 60, tags = tags("ABBA", "Gold", "SOS"))
        val p = diff(link(), scan(retagged), listOf(t), listOf(card(t.dest)), localSha = { "bbb" })
        assertEquals("update", p.items.single().action)
        assertEquals("changed", p.items.single().reason)
        val touched = diff(link(), scan(retagged), listOf(t), listOf(card(t.dest)), localSha = { "aaa" })
        assertEquals("record", touched.items.single().action)
        assertEquals("mtime_only", touched.items.single().reason)
        // without a known hash, a new time with the same size is sent again
        val noSha = diff(link(), scan(retagged), listOf(t.copy(srcSha = null)), listOf(card(t.dest)))
        assertEquals("update", noSha.items.single().action)
    }

    @Test
    fun albumNameFixMovesTheCardFileAndItsLyrics() {
        val es = listOf(e("A/01.flac", tags = tags("ABBA", "Gold (Fixed)", "SOS")), e("A/01.lrc", 50))
        val tracked = listOf(tr("A/01.flac", "Music/ABBA/Gold/01 SOS.flac"), tr("A/01.lrc", "Music/ABBA/Gold/01 SOS.lrc", 50, covers = "keep"))
        val p = diff(link(), scan(*es.toTypedArray()), tracked, tracked.map { card(it.dest, it.sentSize) })
        val moves = p.of("move").associateBy { it.srcRel }
        assertEquals("Music/ABBA/Gold (Fixed)/01 SOS.flac", moves["A/01.flac"]!!.dest)
        assertEquals("Music/ABBA/Gold/01 SOS.flac", moves["A/01.flac"]!!.oldDest)
        assertEquals("Music/ABBA/Gold (Fixed)/01 SOS.lrc", moves["A/01.lrc"]!!.dest)
        assertFalse(moves["A/01.flac"]!!.thenUpdate)
        assertEquals(0, p.counts["add"])
    }

    @Test
    fun renamedOnThisComputerIsAMoveWithKeepLayout() {
        val l = link(layout = "keep")
        val t = tr("Live/old name.flac", "Music/Live/old name.flac")
        val p = diff(l, scan(e("Live/new name.flac")), listOf(t), listOf(card(t.dest)))
        val m = p.items.single()
        assertEquals("move", m.action); assertEquals("renamed", m.reason)
        assertEquals("Music/Live/new name.flac", m.dest); assertEquals(t.dest, m.oldDest)
        assertTrue(p.of("player_only").isEmpty())
    }

    @Test
    fun untrackedSameSizeFileIsAdoptedOnlyWhenFingerprintsMatch() {
        val en = e("A/1.flac", tags = tags("ABBA", "Gold", "SOS"))
        val dest = "Music/ABBA/Gold/01 SOS.flac"
        val same = diff(link(), scan(en), cards = listOf(card(dest)), localQfp = { "q1" }, cardQfp = { "q1" })
        assertEquals("adopt", same.items.single().action)
        assertEquals(dest, same.items.single().record!!["rel"].let { dest })
        val other = diff(link(), scan(en), cards = listOf(card(dest)), localQfp = { "q1" }, cardQfp = { "q2" })
        assertEquals("conflict", other.items.single().action)
        assertEquals("different_content", other.items.single().reason)
        val size = diff(link(), scan(en), cards = listOf(card(dest, 999)))
        assertEquals("conflict", size.items.single().action)
        assertEquals(listOf("computer", "player", "both", "skip"), size.items.single().choices)
    }

    @Test
    fun editedOnThePlayerIsAConflict() {
        val t = tr("A/1.flac", "Music/ABBA/Gold/01 SOS.flac")
        val p = diff(link(), scan(e("A/1.flac", tags = tags("ABBA", "Gold", "SOS"))), listOf(t), listOf(card(t.dest, 1200)))
        assertEquals("conflict", p.items.single().action)
        assertEquals("edited_on_player", p.items.single().reason)
        val both = diff(link(), scan(e("A/1.flac", 1100, t0 + 5, tags("ABBA", "Gold", "SOS"))), listOf(t), listOf(card(t.dest, 1200)))
        assertEquals("changed_both", both.items.single().reason)
    }

    @Test
    fun missingOnTheCardIsCopiedAgainUnderTheSameName() {
        val t = tr("A/1.flac", "Music/ABBA/Gold/01 SOS (2).flac")
        val p = diff(link(), scan(e("A/1.flac", tags = tags("ABBA", "Gold", "SOS"))), listOf(t), emptyList())
        assertEquals("add", p.items.single().action)
        assertEquals("missing_on_card", p.items.single().reason)
        assertEquals("Music/ABBA/Gold/01 SOS (2).flac", p.items.single().dest)
    }

    @Test
    fun collisionsGetNumberedOnce() {
        val es = listOf(e("x/a.flac", tags = tags("A", "B", "T")), e("y/a.flac", tags = tags("A", "B", "T")))
        val p = diff(link(), scan(*es.toTypedArray()))
        assertEquals(setOf("Music/A/B/01 T.flac", "Music/A/B/01 T (2).flac"), p.of("add").map { it.dest }.toSet())
    }

    // ------------------------------------------------------------------ M1: DeleteGuard

    private fun tracked100() = (1..100).map { tr("A/$it.flac", "Music/A/$it.flac") }

    @Test
    fun missingOrEmptyRootNeverPreticksDeletes() {
        for (status in listOf("missing", "empty", "unreadable")) {
            val t = tracked100()
            val p = diff(link(layout = "keep", policy = "delete"), scan(status = status), t, t.map { card(it.dest) })
            val po = p.of("player_only")
            assertEquals(status, 100, po.size)
            assertTrue(status, po.all { it.defaultChoice == "keep" && it.extra["guard"] == true })
            assertTrue(p.guard.tripped); assertEquals("source_unavailable", p.guard.reason)
        }
    }

    @Test
    fun changedVolumeNeverPreticksDeletes() {
        val t = listOf(tr("A/1.flac", "Music/A/1.flac"), tr("A/2.flac", "Music/A/2.flac"))
        val p = diff(link(layout = "keep", policy = "delete", volume = "vsn:1234"), scan(e("A/1.flac"), volume = "vsn:9999"), t, t.map { card(it.dest) })
        assertEquals("volume_changed", p.guard.reason)
        assertEquals("keep", p.of("player_only").single().defaultChoice)
    }

    @Test
    fun elevenPercentMissingNeverPreticksDeletes() {
        val t = tracked100()
        val present = (12..100).map { e("A/$it.flac") }
        val p = diff(link(layout = "keep", policy = "delete"), scan(*present.toTypedArray()), t, t.map { card(it.dest) })
        assertEquals(11, p.counts[PlayerOnly.DELETED])
        assertTrue(p.of("player_only").none { it.defaultChoice == "delete" })
        assertEquals("mass_delete", p.guard.reason)
        // nine missing: under the threshold, so policy "delete" ticks them
        val ok = diff(link(layout = "keep", policy = "delete"), scan(*(10..100).map { e("A/$it.flac") }.toTypedArray()), t, t.map { card(it.dest) })
        assertFalse(ok.guard.tripped)
        assertEquals(9, ok.of("player_only").count { it.defaultChoice == "delete" })
    }

    @Test
    fun moreThan200DeletesTripTheGuard() {
        val t = (1..3000).map { tr("A/$it.flac", "Music/A/$it.flac") }
        val p = diff(link(layout = "keep", policy = "delete"), scan(*(202..3000).map { e("A/$it.flac") }.toTypedArray()), t, t.map { card(it.dest) })
        assertEquals(201, p.counts[PlayerOnly.DELETED]); assertEquals("mass_delete", p.guard.reason)
    }

    @Test
    fun scanErrorsTripTheGuard() {
        val t = listOf(tr("A/1.flac", "Music/A/1.flac"), tr("A/2.flac", "Music/A/2.flac"))
        val s = LocalScan("L1", "/music", "ok", null, mapOf("A/1.flac" to e("A/1.flac")), emptyList(), errors = 1)
        val p = diff(link(layout = "keep", policy = "delete"), s, t, t.map { card(it.dest) })
        assertEquals("scan_errors", p.guard.reason)
        assertEquals("keep", p.of("player_only").single().defaultChoice)
    }

    @Test
    fun anUnreadableFolderIsNeverDeleted() {
        val t = listOf(tr("A/1.flac", "Music/A/1.flac"), tr("B/2.flac", "Music/B/2.flac"))
        val p = diff(link(layout = "keep", policy = "delete"), scan(e("A/1.flac"), unreadable = listOf("B")), t, t.map { card(it.dest) })
        assertTrue(p.items.toString(), p.items.isEmpty())
        assertEquals(1, p.counts["unreadable_kept"])
    }

    // ------------------------------------------------------------------ M2: player-only sub-reasons and policy

    @Test
    fun policyDeletePreticksOnlyDeletedOnComputer() {
        val l = link(layout = "keep", policy = "delete", exclude = listOf("Live/**"))
        val es = (1..20).map { e("A/$it.flac") } + e("Live/x.flac")
        val t = (1..21).map { tr("A/$it.flac", "Music/A/$it.flac") } + tr("Live/x.flac", "Music/Live/x.flac")
        val cards = t.map { card(it.dest) } + card("Music/Mine/added.mp3") + card("Music/Other/o.flac")
        val others = mapOf("music/other/o.flac" to Tracked("Music/Other/o.flac", "L9", "o.flac", "c2", 1000, t0, 1000, null, t0, "keep", t0))
        val p = diff(l, scan(*es.toTypedArray()), t, cards, others)
        val po = p.of("player_only").associateBy { it.dest }
        assertEquals(PlayerOnly.DELETED, po["Music/A/21.flac"]!!.reason); assertEquals("delete", po["Music/A/21.flac"]!!.defaultChoice)
        assertEquals(PlayerOnly.EXCLUDED, po["Music/Live/x.flac"]!!.reason); assertEquals("keep", po["Music/Live/x.flac"]!!.defaultChoice)
        assertEquals(PlayerOnly.ADDED, po["Music/Mine/added.mp3"]!!.reason); assertEquals("keep", po["Music/Mine/added.mp3"]!!.defaultChoice)
        assertEquals(PlayerOnly.OTHER_DEVICE, po["Music/Other/o.flac"]!!.reason); assertEquals("keep", po["Music/Other/o.flac"]!!.defaultChoice)
        assertEquals(1, p.of("player_only").count { it.defaultChoice == "delete" })
        // --delete-untracked ticks the untracked ones too, never the excluded one
        val all = diff(l, scan(*es.toTypedArray()), t, cards, others, deleteUntracked = true).of("player_only").associateBy { it.dest }
        assertEquals("delete", all["Music/Mine/added.mp3"]!!.defaultChoice)
        assertEquals("keep", all["Music/Live/x.flac"]!!.defaultChoice)
    }

    @Test
    fun anExcludeIsExcludedOnComputerAndRemoveUnselectedTicksIt() {
        val t = (1..20).map { tr("A/$it.flac", "Music/A/$it.flac") }
        val es = (1..20).map { e("A/$it.flac") }
        val l = link(layout = "keep", exclude = listOf("A/1.flac"))
        val p = diff(l, scan(*es.toTypedArray()), t, t.map { card(it.dest) })
        val i = p.of("player_only").single()
        assertEquals(PlayerOnly.EXCLUDED, i.reason); assertEquals("keep", i.defaultChoice)
        val r = diff(link(layout = "keep", exclude = listOf("A/1.flac"), removeUnselected = true), scan(*es.toTypedArray()), t, t.map { card(it.dest) })
        assertEquals("delete", r.of("player_only").single().defaultChoice)
    }

    @Test
    fun deselectedArtistIsExcludedOnComputer() {
        val es = listOf(e("a.flac", tags = tags("ABBA", "Gold", "SOS")), e("b.flac", tags = tags("Queen", "Hits", "Bites")))
        val t = listOf(tr("a.flac", "Music/ABBA/Gold/01 SOS.flac"), tr("b.flac", "Music/Queen/Hits/01 Bites.flac"))
        val p = diff(link(selection = LinkSelection(false, artists = listOf("abba"))), scan(*es.toTypedArray()), t, t.map { card(it.dest) })
        assertEquals(1, p.skip)
        assertEquals("Music/Queen/Hits/01 Bites.flac", p.of("player_only").single { it.reason == PlayerOnly.EXCLUDED }.dest)
    }

    @Test
    fun musicFreeByosPlaylistsAndOtherRootsAreNeverListed() {
        val cards = listOf(card("Music/Free/Album/x.mp3"), card("Music/free/y.mp3"), card("Video/v.mp4"), card("ByOS/sync/manifest.json"),
            card("Playlists/a.m3u8"), card("Music/.hidden/z.mp3"), card("Mezclas/m.mp3"), card("x.mp3"))
        val p = diff(link(layout = "keep", policy = "delete"), scan(e("A/1.flac")), cards = cards, deleteUntracked = true)
        assertTrue(p.items.toString(), p.of("player_only").isEmpty())
    }

    @Test
    fun downloadedOnPlayerIsItsOwnGroup() {
        val l = link(Kinds.AUDIOBOOKS)
        val cards = listOf(card("Audiobooks/Twain - Tom Sawyer/book.json", 300), card("Audiobooks/Twain - Tom Sawyer/01.mp3"),
            card("Audiobooks/Mine/01.mp3"))
        val p = diff(l, scan(e("Other/1.mp3")), cards = cards)
        val po = p.of("player_only").associateBy { it.dest }
        assertEquals(PlayerOnly.DOWNLOADED, po["Audiobooks/Twain - Tom Sawyer/01.mp3"]!!.reason)
        assertEquals(PlayerOnly.ADDED, po["Audiobooks/Mine/01.mp3"]!!.reason)
        assertNull("book.json is not a book file", po["Audiobooks/Twain - Tom Sawyer/book.json"])
        val eb = diff(link(Kinds.EBOOKS), scan(e("x.epub")), cards = listOf(card("Books/Twain - Tom Sawyer.epub"), card("Books/mine.epub")))
        assertEquals(PlayerOnly.DOWNLOADED, eb.of("player_only").single { it.dest == "Books/Twain - Tom Sawyer.epub" }.reason)
    }

    @Test
    fun sidecarsStayWithTheirTrack() {
        // the player has a track and its lyrics; the track is kept, so the lyrics are not listed
        val t = tr("A/1.flac", "Music/A/1.flac")
        val cards = listOf(card(t.dest), card("Music/A/1.lrc", 50), card("Music/A/cover.jpg", 50))
        val p = diff(link(layout = "keep"), scan(e("A/1.flac")), listOf(t), cards)
        assertTrue(p.items.toString(), p.items.isEmpty())
        // a player-only track takes its lyrics and folder cover with it
        val q = diff(link(layout = "keep"), scan(e("B/2.flac")), cards = listOf(card("Music/X/1.flac"), card("Music/X/1.lrc", 50), card("Music/X/folder.jpg", 50)))
        val parent = q.of("player_only").single { it.dest == "Music/X/1.flac" }
        val kids = q.of("player_only").filter { it.parent == parent.id }.map { it.dest }.toSet()
        assertEquals(setOf("Music/X/1.lrc", "Music/X/folder.jpg"), kids)
    }

    @Test
    fun sidecarsAreCopiedNextToTheirTrack() {
        val es = listOf(e("A/01 x.flac", tags = tags("ABBA", "Gold", "SOS")), e("A/01 x.lrc", 40), e("A/cover.jpg", 30), e("A/notes.txt", 10))
        val p = diff(link(), scan(*es.toTypedArray()))
        assertEquals(setOf("Music/ABBA/Gold/01 SOS.flac", "Music/ABBA/Gold/01 SOS.lrc", "Music/ABBA/Gold/cover.jpg"), p.of("add").map { it.dest }.toSet())
        val off = diff(link().apply { includeSidecars = false }, scan(*es.toTypedArray()))
        assertEquals(listOf("Music/ABBA/Gold/01 SOS.flac"), off.of("add").map { it.dest })
    }

    @Test
    fun aCueAlbumKeepsItsFolderLayout() {
        val es = listOf(e("Live 1999/disc.flac", tags = tags("Band", "Live", "Whole")), e("Live 1999/disc.cue", 2), e("Other/1.flac", tags = tags("Band", "Studio", "One")))
        val p = diff(link(), scan(*es.toTypedArray()))
        val dests = p.of("add").map { it.dest }.toSet()
        assertTrue(dests.toString(), "Music/Live 1999/disc.flac" in dests)
        assertTrue(dests.toString(), "Music/Live 1999/disc.cue" in dests)
        assertTrue(dests.toString(), "Music/Band/Studio/01 One.flac" in dests)
    }

    // ------------------------------------------------------------------ M2: blocked rules

    @Test
    fun aFileOver4GiBIsBlockedOnFat32Only() {
        val big = e("Films/long.mkv", 4_500_000_000L)
        val p = diff(link(Kinds.VIDEO), scan(big), fs = "vfat")
        assertEquals("blocked", p.items.single().action); assertEquals("too_big_fat32", p.items.single().reason)
        assertEquals("add", diff(link(Kinds.VIDEO), scan(big), fs = "exfat").items.single().action)
    }

    @Test
    fun aFolderOver60kFatSlotsIsBlocked() {
        val name = "x".repeat(120)   // 1 + ceil(124 / 13) = 11 slots each
        val es = (1..5600).map { e("Big/$name $it.epub", 10) }
        val p = diff(link(Kinds.EBOOKS), scan(*es.toTypedArray()), fs = "vfat")
        assertEquals(5600, p.counts["blocked"])
        assertTrue(p.items.all { it.reason == "folder_full_fat" })
        val small = diff(link(Kinds.EBOOKS), scan(*es.take(4000).toTypedArray()), fs = "vfat")
        assertEquals(0, small.counts["blocked"])
        assertEquals(5600, diff(link(Kinds.EBOOKS), scan(*es.toTypedArray()), fs = "exfat").counts["add"])
    }

    @Test
    fun aPathTooDeepIsBlocked() {
        val rel = (1..33).joinToString("/") { "d$it" } + "/f.mp4"
        val p = diff(link(Kinds.VIDEO), scan(e(rel)))
        assertEquals("blocked", p.items.single().action)
        assertTrue(p.items.single().reason.startsWith("bad_path"))
    }

    @Test
    fun aMoveIntoAnotherLinksFolderIsBlocked() {
        // album artist "Audiobooks": the new place lies inside the audiobooks link's card root
        val t = tr("a.flac", "Music/X/Y/01 T.flac")
        val p = FolderSync.diff(DiffInput(link(root = "Music"), scan(e("a.flac", tags = tags("Audiobooks", "Ch", "T"))), listOf(t), emptyMap(),
            listOf(card(t.dest)), client, nestedRoots = listOf("Music/Audiobooks")))
        assertEquals("blocked", p.items.single().action)
        assertEquals("inside_other_link", p.items.single().reason)
    }

    // ------------------------------------------------------------------ tags that could not be read, mass unselection

    @Test
    fun aTagReadErrorNeverPreticksADeleteNorMovesTheFile() {
        val sel = LinkSelection(false, artists = listOf("abba"))
        val t = listOf(tr("a.flac", "Music/ABBA/Gold/01 SOS.flac"), tr("b.flac", "Music/ABBA/Gold/02 Mamma.flac")) +
            (3..30).map { tr("x$it.flac", "Music/ABBA/Gold/$it X.flac") }
        val es = listOf(
            // no tags at all (a new or retagged file whose tags could not be read)
            LocalEntry("a.flac", 1000, t0, null, tagError = true),
            // the last tags read, but a read error now: it stays where it is
            LocalEntry("b.flac", 1000, t0 + 5, tags("ABBA", "Gold (Remaster)", "Mamma", 2), tagError = true)) +
            (3..30).map { e("x$it.flac", tags = tags("ABBA", "Gold", "X", it)) }
        val p = diff(link(selection = sel, removeUnselected = true), scan(*es.toTypedArray()), t, t.map { card(it.dest) }, localSha = { null })
        val a = p.of("player_only").single { it.srcRel == "a.flac" }
        assertEquals(PlayerOnly.EXCLUDED, a.reason)
        assertEquals("keep", a.defaultChoice)
        assertEquals(true, a.extra["tags_unreadable"])
        assertTrue(p.of("move").none { it.srcRel == "b.flac" })
        assertEquals("Music/ABBA/Gold/02 Mamma.flac", p.items.single { it.srcRel == "b.flac" }.dest)
        assertFalse(p.guard.tripped)
    }

    @Test
    fun aMassUnselectionTripsTheGuard() {
        val es = (1..30).map { e("a$it.flac", tags = tags("ABBA", "Gold", "S$it", it)) } + e("q.flac", tags = tags("Queen", "Hits", "Q"))
        val t = (1..30).map { tr("a$it.flac", "Music/ABBA/Gold/${"%02d".format(it)} S$it.flac") } + tr("q.flac", "Music/Queen/Hits/01 Q.flac")
        // a retag or a narrowed selection that leaves out 30 of 31 files: nothing is pre-ticked
        val p = diff(link(selection = LinkSelection(false, artists = listOf("queen")), removeUnselected = true), scan(*es.toTypedArray()), t, t.map { card(it.dest) })
        assertEquals("mass_delete", p.guard.reason)
        assertTrue(p.of("player_only").all { it.defaultChoice == "keep" && it.extra["guard"] == true })
        // one of 31 is under the threshold: ticked as asked
        val q = diff(link(selection = LinkSelection(false, artists = listOf("abba")), removeUnselected = true), scan(*es.toTypedArray()), t, t.map { card(it.dest) })
        assertFalse(q.guard.tripped)
        assertEquals("delete", q.of("player_only").single().defaultChoice)
    }

    @Test
    fun aVolumeIdOfAnotherKindIsNotAChange() {
        val t = listOf(tr("A/1.flac", "Music/A/1.flac"), tr("A/2.flac", "Music/A/2.flac"))
        val p = diff(link(layout = "keep", policy = "delete", volume = "dev:2049"), scan(e("A/1.flac"), volume = "uuid:1234-abcd"), t, t.map { card(it.dest) })
        assertNull(p.guard.reason.takeIf { it == "volume_changed" })
        val q = diff(link(layout = "keep", policy = "delete", volume = "uuid:aaaa"), scan(e("A/1.flac"), volume = "uuid:bbbb"), t, t.map { card(it.dest) })
        assertEquals("volume_changed", q.guard.reason)
    }

    @Test
    fun cardPathsCompareWithoutUnicodeForm() {
        // the card lists an NFD name: it is the tracked file, under its own spelling
        val nfd = "Music/Beyoncé/x.flac"
        val rel = SyncRules.nfc("Beyoncé/x.flac")
        val t = tr(rel, SyncRules.nfc(nfd))
        val p = diff(link(layout = "keep"), scan(e(rel)), listOf(t), listOf(card(nfd)))
        assertTrue(p.items.toString(), p.items.isEmpty())
        assertEquals(1, p.skip)
        // a move or an update works on the card's own spelling
        val m = diff(link(layout = "keep"), scan(e("x.flac")), listOf(t.copy(rel = "x.flac")), listOf(card(nfd))).of("move").single()
        assertEquals(nfd, m.oldDest)
        val u = diff(link(layout = "keep"), scan(e(rel, mtime = t0 + 9)), listOf(t), listOf(card(nfd))).of("update").single()
        assertEquals(nfd, u.dest)
        val d = diff(link(layout = "keep"), scan(e("other.flac", size = 5)), listOf(t), listOf(card(nfd))).of("player_only").single()
        assertEquals(nfd, d.dest)
    }

    // ------------------------------------------------------------------ performance

    @Test
    fun fiftyThousandTracksDiffQuickly() {
        val n = 50_000
        val es = ArrayList<LocalEntry>(n); val t = ArrayList<Tracked>(n); val cards = ArrayList<CardFile>(n)
        for (i in 0 until n) {
            val a = "Artist ${i / 500}"; val al = "Album ${i / 12}"; val k = i % 12 + 1
            val rel = "$a/$al/${"%02d".format(k)} Track $i.flac"
            es += e(rel, 30_000_000L + i, tags = tags(a, al, "Track $i", k))
            if (i < n - 1000) {
                val dest = "Music/$a/$al/${"%02d".format(k)} Track $i.flac"
                t += tr(rel, dest, 30_000_000L + i); cards += card(dest, 30_000_000L + i)
            }
        }
        val start = System.nanoTime()
        val p = diff(link(), scan(*es.toTypedArray()), t, cards)
        val ms = (System.nanoTime() - start) / 1_000_000
        assertEquals(1000, p.counts["add"]); assertEquals(n - 1000, p.skip)
        assertTrue("diff of 50k tracks took $ms ms", ms < 20_000)
    }

    // ------------------------------------------------------------------ rules, decisions, manifest

    @Test
    fun globs() {
        assertTrue(SyncRules.excluded(listOf("**/*.tmp"), "a/b/c.tmp"))
        assertTrue(SyncRules.excluded(listOf("*.tmp"), "a/b/c.TMP"))
        assertTrue(SyncRules.excluded(listOf("Live/**"), "Live/x/y.flac"))
        assertTrue(SyncRules.excluded(listOf("Live"), "Live/y.flac"))
        assertFalse(SyncRules.excluded(listOf("Live/**"), "Studio/Live.flac"))
        assertFalse(SyncRules.excluded(listOf("a?.flac"), "abc.flac"))
        assertEquals("CON_.txt", SyncRules.windowsSafe("CON.txt"))
        assertEquals("a (2).flac", SyncRules.numbered("a.flac", 2))
        assertEquals("M/a.flac", SyncRules.unnumbered("M/a (12).flac"))
        assertEquals(11, SyncRules.fatSlots("x".repeat(124)))
    }

    @Test
    fun decisionsFollowPolicyAndParents() {
        val items = listOf(
            PlanItem("i1", "music", "L1", "player_only", PlayerOnly.DELETED, "a", "Music/a.flac", defaultChoice = "keep", choices = listOf("delete", "keep", "pull")),
            PlanItem("i2", "music", "L1", "player_only", PlayerOnly.DELETED, null, "Music/a.lrc", parent = "i1"),
            PlanItem("i3", "music", "L1", "player_only", PlayerOnly.ADDED, null, "Music/b.flac", defaultChoice = "keep", choices = listOf("delete", "keep", "pull")),
            PlanItem("i4", "music", "L1", "conflict", "edited_on_player", "c", "Music/c.flac", defaultChoice = "skip", choices = listOf("computer", "player", "both", "skip")),
            PlanItem("i5", "music", "L1", "player_only", PlayerOnly.DELETED, "d", "Music/d.flac", defaultChoice = "keep", choices = listOf("delete", "keep", "pull"),
                extra = mapOf("guard" to true)))
        val c = Decisions.resolve(items, mapOf("policy" to mapOf("music" to mapOf("player_only" to "delete")), "i4" to "both"))
        assertEquals("delete", c["i1"]); assertEquals("delete", c["i2"]); assertEquals("keep", c["i3"]); assertEquals("both", c["i4"])
        assertEquals("keep", c["i5"])
        assertEquals("pull", Decisions.resolve(items, mapOf("i3" to "pull"))["i3"])
        try { Decisions.resolve(items, mapOf("i3" to "computer")); throw AssertionError("accepted a bad choice") } catch (e: SyncException) {}
        try { Decisions.resolve(items, mapOf("i2" to "keep")); throw AssertionError("accepted a child choice") } catch (e: SyncException) {}
    }

    @Test
    fun manifestOpsAreIdempotentAndV1Migrates() {
        val m = Manifest(Kinds.MUSIC)
        val put = mapOf("op" to "put", "kind" to "music", "dest" to "Music/A/1.flac",
            "entry" to Tracked("Music/A/1.flac", "L1", "A/1.flac", client, 10, t0, 10, "s", t0, "keep", t0).toMap())
        m.apply(put); m.apply(put)
        assertEquals(1, m.size)
        m.apply(mapOf("op" to "move", "kind" to "music", "from" to "Music/A/1.flac", "to" to "Music/B/1.flac"))
        assertEquals("Music/B/1.flac", m.forLink("L1").single().dest)
        // a new put of the same source elsewhere replaces the old entry
        m.apply(put)
        assertEquals(1, m.size); assertEquals("Music/A/1.flac", m.forLink("L1").single().dest)
        m.apply(mapOf("op" to "delete", "kind" to "music", "dest" to "music/a/1.FLAC"))
        assertEquals(0, m.size)
        // v1: keyed by card path with the absolute source
        val v1 = mapOf("format" to 1L, "files" to mapOf(
            "Music/ABBA/Gold/01 SOS.flac" to mapOf("src" to "D:\\Music\\ABBA\\01.flac", "src_size" to 10L, "src_mtime" to t0, "size" to 12L, "covers" to "optimize"),
            "Music/X/y.flac" to mapOf("src" to "C:\\Elsewhere\\y.flac", "src_size" to 5L, "src_mtime" to t0, "size" to 5L, "covers" to "keep")))
        val l = link().apply { local = "D:\\Music" }
        val v2 = Manifest(Kinds.MUSIC)
        assertEquals(2, Manifest.migrateV1(v2, v1, listOf(l), client))
        val a = v2.get("Music/ABBA/Gold/01 SOS.flac")!!
        assertEquals("L1", a.link); assertEquals("ABBA/01.flac", a.rel); assertEquals(12, a.sentSize); assertEquals("optimize", a.covers)
        assertNull(v2.get("Music/X/y.flac")!!.link)
        // the second folder gets linked later: its entry is resolved
        val l2 = Link("L2", Kinds.MUSIC, "C:\\Elsewhere", "Music", "artist-album")
        assertEquals(1, Manifest.migrateV1(v2, v1, listOf(l, l2), client))
        assertEquals("L2", v2.get("Music/X/y.flac")!!.link)
        assertEquals(0, Manifest.migrateV1(v2, v1, listOf(l, l2), client))
    }
}
