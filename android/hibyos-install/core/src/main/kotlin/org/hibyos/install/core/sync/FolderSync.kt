package org.hibyos.install.core.sync

import org.hibyos.install.core.btp.BtpPaths
import org.hibyos.install.core.library.IndexTrack
import org.hibyos.install.core.library.SourceTrack
import org.hibyos.install.core.library.SyncPlanner
import org.hibyos.install.core.library.SyncSelection
import kotlin.math.abs

/** Plan item ids, unique within one plan. */
class IdGen { private var n = 0; fun next() = "i" + (++n) }

/** Everything [FolderSync.diff] compares for one link. */
class DiffInput(
    val link: Link,
    val scan: LocalScan,
    /** Manifest entries of this link (identity: link id + rel). */
    val tracked: List<Tracked>,
    /** Lowercase dest → entry, for files of this kind written by another link or another client. */
    val others: Map<String, Tracked>,
    /** Every file under the link's card root, from LIST (unfiltered). */
    val card: List<CardFile>,
    val clientId: String,
    /** vfat | exfat | unknown */
    val fs: String = "exfat",
    /** Card roots of other links inside this one: left to those links. */
    val nestedRoots: List<String> = emptyList(),
    /** --delete-untracked: files the player has that no sync of this computer wrote are pre-ticked too. */
    val deleteUntracked: Boolean = false,
    /** Overrides the link's player_only policy (DEC.json "policy"). */
    val policy: String? = null,
    /** Quick fingerprints, only asked for when sizes already match. */
    val localQfp: (String) -> String? = { null },
    val cardQfp: (CardFile) -> String? = { null },
    /** Full SHA-256 of a local file (only for a changed time with the same size). */
    val localSha: (String) -> String? = { null },
)

/**
 * The diff of section 3.3: what one link's folder and the card's copy of it need. Pure: all
 * reads of files go through the callbacks of [DiffInput], so it runs the same against the fake
 * device, the real one or test data.
 */
object FolderSync {
    private val COVER_FORMATS = setOf("flac", "mp3", "m4a")

    private fun indexTrack(rel: String, e: LocalEntry): IndexTrack {
        val t = e.tags
        return IndexTrack(0, rel, t?.title ?: SyncRules.name(rel).substringBeforeLast('.'), t?.artist ?: "", t?.albumArtist ?: "",
            t?.album ?: "", t?.genre ?: "", t?.year ?: 0, t?.track ?: 0, t?.disc ?: 0, t?.durationMs ?: 0, e.size, e.mtime, e.mtime,
            0, 0, 0, false, false, false)
    }

    private fun keepDest(root: String, rel: String) = BtpPaths.join(root, *rel.split('/').map { SyncPlanner.cleanName(it) }.toTypedArray())

    fun diff(inp: DiffInput, ids: IdGen = IdGen()): LinkPlan {
        val link = inp.link
        val kind = link.kind
        val root = link.cardRoot
        val scan = inp.scan
        val scanOk = scan.status == "ok"
        val lower = SyncRules::lower
        fun inNested(p: String) = inp.nestedRoots.any { SyncRules.under(p, it) || lower(p) == lower(it) }
        fun relToRoot(p: String) = if (root.isEmpty()) p else p.substring(root.length + 1)

        // ---- the card under this link's root, without what a sync never touches
        val bookFolders = inp.card.filter { SyncRules.name(it.path).equals("book.json", true) }.map { lower(SyncRules.parent(it.path)) }.toSet()
        val cardFiles = inp.card.filter {
            SyncRules.under(it.path, root) && !SyncRules.alwaysExcludedOnCard(it.path) && !inNested(it.path) &&
                true
        }
        val cardBy = HashMap<String, CardFile>(cardFiles.size * 2)
        for (c in cardFiles) cardBy[lower(c.path)] = c
        val cardByDir = cardFiles.groupBy { lower(SyncRules.parent(it.path)) }

        // ---- what this link wrote before, by source identity
        val trackedByRel = HashMap<String, Tracked>()
        for (t in inp.tracked) {
            if (t.link != link.id) continue
            val prev = trackedByRel[t.rel]
            if (prev == null || (cardBy[lower(prev.dest)] == null && cardBy[lower(t.dest)] != null)) trackedByRel[t.rel] = t
        }

        // ---- 1. the local files: wanted, or present but left out (selection, exclusions)
        val sel = link.selection.toSyncSelection()
        val wanted = java.util.TreeMap<String, LocalEntry>()
        val notWanted = HashSet<String>()
        val mediaRels = ArrayList<String>()
        val sideRels = ArrayList<String>()
        val localDirsWithMedia = HashSet<String>()
        val cueDirs = HashSet<String>()
        // left out by the selection while their tags could not be read: never pre-ticked for deletion
        val tagUnknown = HashSet<String>()
        for (e in scan.entries.values) {
            val nm = SyncRules.name(e.rel)
            if (SyncRules.hidden(e.rel) || !Kinds.relevant(kind, nm)) continue
            val media = Kinds.isMedia(kind, nm)
            if (media) localDirsWithMedia += SyncRules.parent(e.rel)
            if (Kinds.ext(nm) == "cue") cueDirs += SyncRules.parent(e.rel)
            if (SyncRules.excluded(link.exclude, e.rel)) { notWanted += e.rel; continue }
            if (!media && !link.includeSidecars) { notWanted += e.rel; continue }
            if (media && kind == Kinds.MUSIC && !link.selection.all && !sel.wants(SourceTrack(e.rel, e.rel, indexTrack(e.rel, e)))) {
                notWanted += e.rel
                if (e.tagError) tagUnknown += e.rel
                continue
            }
            wanted[e.rel] = e
            if (media) mediaRels += e.rel else sideRels += e.rel
        }
        mediaRels.sort(); sideRels.sort()

        // ---- 2. renamed on this computer: a missing tracked file and a new one with the same size and time
        val rekeyed = HashMap<String, Tracked>()
        if (scanOk) {
            val missing = trackedByRel.values.filter { it.rel !in scan.entries && !scan.underUnreadable(it.rel) }
            if (missing.isNotEmpty()) {
                val byKey = missing.groupBy { it.srcSize to it.srcMtime }
                val usedOld = HashSet<String>()
                for (rel in mediaRels + sideRels) {
                    if (rel in trackedByRel) continue
                    val e = wanted[rel]!!
                    val cands = byKey[e.size to e.mtime]?.filter { it.rel !in usedOld } ?: continue
                    if (cands.isEmpty()) continue
                    val pick = if (cands.size == 1) cands[0] else {
                        val s = inp.localSha(rel)
                        cands.firstOrNull { s != null && it.srcSha == s } ?: cands.firstOrNull { SyncRules.name(it.rel).equals(SyncRules.name(rel), true) }
                    } ?: continue
                    rekeyed[rel] = pick
                    usedOld += pick.rel
                }
                for ((_, old) in rekeyed) trackedByRel.remove(old.rel)
                for ((rel, old) in rekeyed) trackedByRel[rel] = old
            }
        }

        // ---- 3. where each wanted file goes on the card
        val music = kind == Kinds.MUSIC && link.layout == "artist-album"
        val itracks = HashMap<String, IndexTrack>()
        val discs = HashMap<String, MutableSet<Int>>()
        if (music) for (rel in mediaRels) {
            val it = indexTrack(rel, wanted[rel]!!); itracks[rel] = it
            discs.getOrPut(SyncSelection.albumKey(it.groupArtist, it.album)) { HashSet() } += it.disc
        }
        fun natural(rel: String): String {
            val it = itracks[rel]
            // tags that could not be read say nothing about where the file belongs: it stays where it is
            if (wanted[rel]?.tagError == true) trackedByRel[rel]?.let { t -> return t.dest }
            if (!music || it == null || SyncRules.parent(rel) in cueDirs) return keepDest(root, rel)
            val multi = (discs[SyncSelection.albumKey(it.groupArtist, it.album)]?.size ?: 0) > 1
            return SyncPlanner.destFor(SourceTrack(rel, rel, it), root, "artist-album", multi)
        }
        val destOf = HashMap<String, String>()
        val claimed = HashMap<String, String>()
        // files this link wrote that stay on the card (left out, kept player-only) keep their names
        for ((rel, t) in trackedByRel) if (rel !in wanted && cardBy.containsKey(lower(t.dest))) claimed[lower(t.dest)] = "#" + rel
        val nat = HashMap<String, String>()
        for (rel in mediaRels) {
            val n = natural(rel); nat[rel] = n
            val t = trackedByRel[rel] ?: continue
            if ((lower(t.dest) == lower(n) || lower(SyncRules.unnumbered(t.dest)) == lower(n)) && lower(t.dest) !in claimed) {
                // the spelling the card has (it may differ from the manifest's in case or Unicode form)
                destOf[rel] = cardBy[lower(t.dest)]?.path ?: t.dest; claimed[lower(t.dest)] = rel
            }
        }
        fun claim(rel: String, want: String): String {
            var d = want; var k = 2
            while (lower(d) in claimed) d = SyncRules.numbered(want, k++)
            claimed[lower(d)] = rel
            return d
        }
        for (rel in mediaRels) if (rel !in destOf) destOf[rel] = claim(rel, nat[rel]!!)
        // sidecars follow their track (same base name) or their folder
        val mediaByDir = mediaRels.groupBy { SyncRules.parent(it) }
        val localMediaByDir = scan.entries.keys.filter { Kinds.isMedia(kind, SyncRules.name(it)) && !SyncRules.hidden(it) }.groupBy { SyncRules.parent(it) }
        for (rel in sideRels) {
            val dir = SyncRules.parent(rel); val nm = SyncRules.name(rel)
            var d: String? = null
            if (Kinds.isTrackSidecar(kind, nm)) {
                val local = (localMediaByDir[dir] ?: emptyList())
                    .filter { nm.startsWith(SyncRules.name(it).substringBeforeLast('.') + ".") }.maxByOrNull { it.length }
                if (local != null) {
                    val td = destOf[local]
                    if (td == null) { notWanted += rel; wanted.remove(rel); continue }
                    val base = SyncRules.name(local).substringBeforeLast('.')
                    d = td.substringBeforeLast('.') + "." + SyncPlanner.cleanName(nm.substring(base.length + 1), "")
                }
            }
            if (d == null) {
                val ms = mediaByDir[dir]
                if (ms != null && ms.isNotEmpty()) {
                    val folder = ms.map { SyncRules.parent(destOf[it]!!) }.groupingBy { it }.eachCount().maxByOrNull { it.value }!!.key
                    d = BtpPaths.join(folder, SyncPlanner.cleanName(nm))
                } else if (dir in localDirsWithMedia) { notWanted += rel; wanted.remove(rel); continue }
                else d = keepDest(root, rel)
            }
            val t = trackedByRel[rel]
            destOf[rel] = if (t != null && lower(t.dest) == lower(d) && lower(t.dest) !in claimed) { claimed[lower(t.dest)] = rel; cardBy[lower(t.dest)]?.path ?: t.dest } else claim(rel, d)
        }

        // ---- 4. compare each wanted file with what the card has
        val items = ArrayList<PlanItem>()
        var skip = 0
        var fpBytes = 0L
        val consumed = HashSet<String>()
        for (t in trackedByRel.values) consumed += lower(t.dest)
        for (d in destOf.values) consumed += lower(d)
        val trackedDests = trackedByRel.values.mapTo(HashSet()) { lower(it.dest) }
        val itemOf = HashMap<String, PlanItem>()
        fun add(i: PlanItem) { items += i; if (i.srcRel != null) itemOf[i.srcRel] = i }
        for (rel in mediaRels + sideRels) {
            val e = wanted[rel] ?: continue
            val dest = destOf[rel] ?: continue
            val nm = SyncRules.name(rel)
            val title = itracks[rel]?.title ?: e.tags?.title ?: nm.substringBeforeLast('.')
            val isMedia = Kinds.isMedia(kind, nm)
            val covers = if (kind == Kinds.MUSIC && isMedia && Kinds.ext(nm) in COVER_FORMATS) link.covers else "keep"
            val t = trackedByRel[rel]
            if (t != null) {
                var on = cardBy[lower(t.dest)]
                var cardPath = on?.path ?: t.dest
                var renamedOnCard = false
                var state = when {
                    on == null -> "missing"
                    on.size != t.sentSize -> "changed"
                    t.cardMtime > 0 && abs(on.mtime - t.cardMtime) > 2 -> "changed"
                    else -> "same"
                }
                if (state == "missing") {
                    val dir = lower(SyncRules.parent(t.dest))
                    val cands = (cardByDir[dir] ?: emptyList()).filter {
                        lower(it.path) !in consumed && it.size == t.sentSize &&
                            (t.cardMtime <= 0 || abs(it.mtime - t.cardMtime) <= 2)
                    }
                    val found = if (cands.size == 1 && (t.sentQfp == null || inp.cardQfp(cands[0]) == t.sentQfp)) cands[0] else null
                    if (found != null) {
                        if (t.sentQfp != null) fpBytes += 2L * minOf(found.size, SyncRules.QFP_PART.toLong())
                        on = found; cardPath = found.path; renamedOnCard = true; state = "same"; consumed += lower(found.path)
                    }
                }
                var localChanged = e.size != t.srcSize || e.mtime != t.srcMtime
                var touch = false
                if (e.size == t.srcSize && e.mtime != t.srcMtime && t.srcSha != null) {
                    fpBytes += e.size
                    if (inp.localSha(rel) == t.srcSha) { localChanged = false; touch = true }
                }
                val coversChanged = covers != "keep" && t.covers != covers
                val rekey = rekeyed.containsKey(rel) || renamedOnCard
                val base = PlanItem("", kind, link.id, "", "", rel, dest, null, e.size, e.mtime, on?.size ?: -1, on?.mtime ?: 0, title, t.origin, covers = covers)
                when (state) {
                    "missing" -> add(copyOf(base, ids.next(), "add", "missing_on_card", cardSize = -1))
                    "changed" -> add(copyOf(base, ids.next(), "conflict", if (localChanged) "changed_both" else "edited_on_player",
                        dest = cardPath, default = "skip", choices = listOf("computer", "player", "both", "skip")))
                    else -> when {
                        lower(cardPath) != lower(dest) -> add(copyOf(base, ids.next(), "move", if (rekey) "renamed" else "dest_changed",
                            oldDest = cardPath, thenUpdate = localChanged || coversChanged))
                        localChanged || coversChanged -> add(copyOf(base, ids.next(), "update", if (localChanged) "changed" else "covers"))
                        touch || rekey -> add(copyOf(base, ids.next(), "record", if (touch) "mtime_only" else "renamed",
                            record = t.copy(dest = dest, rel = rel, srcSize = e.size, srcMtime = e.mtime, cardMtime = on!!.mtime).toMap() + mapOf("from" to t.dest)))
                        else -> skip++
                    }
                }
            } else {
                val on = cardBy[lower(dest)]
                val other = inp.others[lower(dest)]
                val base = PlanItem("", kind, link.id, "", "", rel, dest, null, e.size, e.mtime, on?.size ?: -1, on?.mtime ?: 0, title,
                    other?.origin, covers = covers)
                // the file already there keeps its own spelling (an NFD name is another name on the card)
                when {
                    on == null -> add(copyOf(base, ids.next(), "add", "new"))
                    on.size == e.size -> {
                        fpBytes += 2L * minOf(on.size, SyncRules.QFP_PART.toLong())
                        val c = inp.cardQfp(on)
                        if (c != null && c == inp.localQfp(rel)) add(copyOf(base, ids.next(), "adopt", if (other != null) "same_content_other" else "same_content",
                            dest = on.path, record = Tracked(on.path, link.id, rel, inp.clientId, e.size, e.mtime, on.size, null, on.mtime, covers, 0, sentQfp = c).toMap()))
                        else add(copyOf(base, ids.next(), "conflict", "different_content", dest = on.path, default = "skip", choices = listOf("computer", "player", "both", "skip")))
                    }
                    else -> add(copyOf(base, ids.next(), "conflict", "different_content", dest = on.path, default = "skip", choices = listOf("computer", "player", "both", "skip")))
                }
            }
        }

        // ---- blocked: FAT32 4 GiB, path rules, other links' folders, full FAT folders
        val vfat = inp.fs == "vfat"
        for ((k, i) in items.withIndex()) {
            val sends = i.action == "add" || i.action == "update" || (i.action == "move" && i.thenUpdate)
            // a move puts a file at a new place too: the same path rules apply to it
            val places = sends || i.action == "move"
            val why = when {
                !places -> null
                sends && vfat && i.size > SyncRules.FAT32_MAX -> "too_big_fat32"
                BtpPaths.check(i.dest) != null -> "bad_path_" + BtpPaths.check(i.dest)
                inNested(i.dest) -> "inside_other_link"
                else -> null
            }
            if (why != null) items[k] = copyOf(i, i.id, "blocked", why)
        }
        if (vfat) {
            val slots = HashMap<String, Int>()
            val seenDirs = HashSet<String>()
            fun addEntry(dir: String, name: String) { slots[lower(dir)] = (slots[lower(dir)] ?: 2) + SyncRules.fatSlots(name) }
            for (c in inp.card) {
                addEntry(SyncRules.parent(c.path), SyncRules.name(c.path))
                var d = SyncRules.parent(c.path)
                while (d.isNotEmpty() && seenDirs.add(lower(d))) { addEntry(SyncRules.parent(d), SyncRules.name(d)); d = SyncRules.parent(d) }
            }
            val adds = items.filter { it.action == "add" }.groupBy { lower(SyncRules.parent(it.dest)) }
            for ((dir, list) in adds) {
                val need = (slots[dir] ?: 2) + list.sumOf { SyncRules.fatSlots(SyncRules.name(it.dest)) }
                if (need > SyncRules.FAT_FOLDER_SLOTS) {
                    val block = list.mapTo(HashSet()) { it.id }
                    for ((k, i) in items.withIndex()) if (i.id in block) items[k] = copyOf(i, i.id, "blocked", "folder_full_fat")
                }
            }
        }

        // ---- 5. files this link wrote that have no wanted source any more
        var unreadableKept = 0
        val playerOnly = ArrayList<PlanItem>()
        for ((rel, t) in trackedByRel.entries.sortedBy { it.value.dest }.map { it.key to it.value }) {
            if (rel in wanted) continue
            if (!SyncRules.under(t.dest, root) || inNested(t.dest) || SyncRules.alwaysExcludedOnCard(t.dest)) continue
            val on = cardBy[lower(t.dest)]
            if (on == null) {
                items += PlanItem(ids.next(), kind, link.id, "record", "gone_from_card", rel, t.dest, record = mapOf("forget" to true))
                continue
            }
            val present = rel in scan.entries
            val reason = when {
                present -> PlayerOnly.EXCLUDED
                scanOk && scan.underUnreadable(rel) -> null
                t.origin != null && t.origin != inp.clientId -> PlayerOnly.OTHER_DEVICE
                else -> PlayerOnly.DELETED
            }
            if (reason == null) { unreadableKept++; continue }
            val changed = on.size != t.sentSize || (t.cardMtime > 0 && abs(on.mtime - t.cardMtime) > 2)
            val extra = HashMap<String, Any?>()
            if (changed) extra["card_changed"] = true
            if (reason == PlayerOnly.EXCLUDED && rel in tagUnknown) extra["tags_unreadable"] = true
            playerOnly += PlanItem(ids.next(), kind, link.id, "player_only", reason, rel, on.path, null, on.size, 0, on.size, on.mtime,
                SyncRules.name(t.dest).substringBeforeLast('.'), t.origin, extra = extra)
        }

        // ---- 6. files on the card under the root that no sync of this link wrote
        val untracked = cardFiles.filter { lower(it.path) !in consumed && lower(it.path) !in trackedDests && Kinds.relevant(kind, SyncRules.name(it.path)) &&
            !SyncRules.excluded(link.exclude, relToRoot(it.path)) }
        val untrackedMedia = untracked.filter { Kinds.isMedia(kind, SyncRules.name(it.path)) }.sortedBy { it.path }
        val poByPath = HashMap<String, PlanItem>()
        for (i in playerOnly) poByPath[lower(i.dest)] = i
        fun downloaded(p: String): Boolean = when (kind) {
            Kinds.AUDIOBOOKS -> {
                val top = p.substring(root.length + 1).substringBefore('/', "")
                top.isNotEmpty() && lower(BtpPaths.join(root, top)) in bookFolders
            }
            Kinds.EBOOKS -> SyncRules.parent(p).equals(root, true) && Kinds.ext(p) == "epub" && SyncRules.name(p).contains(" - ")
            else -> false
        }
        for (c in untrackedMedia) {
            val other = inp.others[lower(c.path)]
            val reason = when {
                other != null && (other.origin != inp.clientId || other.link == null) -> PlayerOnly.OTHER_DEVICE
                other != null -> continue   // another link of this computer: left to it
                downloaded(c.path) -> PlayerOnly.DOWNLOADED
                else -> PlayerOnly.ADDED
            }
            val i = PlanItem(ids.next(), kind, link.id, "player_only", reason, null, c.path, null, c.size, 0, c.size, c.mtime,
                SyncRules.name(c.path).substringBeforeLast('.'), other?.origin)
            playerOnly += i
            poByPath[lower(c.path)] = i
        }
        // sidecars on the card: they go with their track or folder, and stay while it stays
        val mediaOnCardByDir = cardFiles.filter { Kinds.isMedia(kind, SyncRules.name(it.path)) }.groupBy { lower(SyncRules.parent(it.path)) }
        for (c in untracked.filter { !Kinds.isMedia(kind, SyncRules.name(it.path)) }.sortedBy { it.path }) {
            val dir = lower(SyncRules.parent(c.path)); val nm = SyncRules.name(c.path)
            val inDir = mediaOnCardByDir[dir] ?: emptyList()
            var parentItem: PlanItem? = null
            if (Kinds.isTrackSidecar(kind, nm)) {
                val track = inDir.filter { nm.startsWith(SyncRules.name(it.path).substringBeforeLast('.') + ".") }.maxByOrNull { it.path.length }
                if (track != null) {
                    parentItem = poByPath[lower(track.path)] ?: continue   // its track stays: so does the sidecar
                }
            }
            if (parentItem == null && inDir.isNotEmpty()) {
                // a folder sidecar stays while any media file of the folder stays
                if (inDir.any { poByPath[lower(it.path)] == null }) continue
                parentItem = poByPath[lower(inDir.first().path)]
            }
            val other = inp.others[lower(c.path)]
            val reason = parentItem?.reason ?: when {
                other != null && (other.origin != inp.clientId || other.link == null) -> PlayerOnly.OTHER_DEVICE
                other != null -> continue
                downloaded(c.path) -> PlayerOnly.DOWNLOADED
                else -> PlayerOnly.ADDED
            }
            playerOnly += PlanItem(ids.next(), kind, link.id, "player_only", reason, null, c.path, null, c.size, 0, c.size, c.mtime,
                nm, other?.origin, parent = parentItem?.id)
        }

        // ---- 7. DeleteGuard
        // files that would be pre-ticked for deletion: deleted on this computer, and (with
        // remove_unselected) left out by the selection, which a retag or a tag read error can do in bulk
        val nDeleted = playerOnly.count {
            it.parent == null && (it.reason == PlayerOnly.DELETED || (it.reason == PlayerOnly.EXCLUDED && link.removeUnselected && it.extra["tags_unreadable"] != true))
        }
        val guardReason = when {
            !scanOk -> "source_unavailable"
            // ids of another kind (a Linux device number before the file system UUID) are not compared
            LocalScanner.sameScheme(link.volumeId, scan.volumeId) && link.volumeId != scan.volumeId -> "volume_changed"
            scan.errors > 0 -> "scan_errors"
            nDeleted > SyncRules.MASS_FILES || nDeleted > trackedByRel.size * SyncRules.MASS_SHARE -> "mass_delete"
            else -> null
        }
        val guard = Guard(guardReason != null, guardReason)

        // ---- 8. the pre-ticked choice of each player-only file
        val policy = inp.policy ?: link.playerOnly
        val byId = HashMap<String, PlanItem>()
        for (i in playerOnly) {
            val parent = i.parent?.let { byId[it] }
            val del = when {
                parent != null -> parent.defaultChoice == "delete"
                guard.tripped -> false
                i.extra["card_changed"] == true -> false
                i.extra["tags_unreadable"] == true -> false
                i.reason == PlayerOnly.DELETED -> policy == "delete"
                i.reason == PlayerOnly.EXCLUDED -> link.removeUnselected
                else -> inp.deleteUntracked
            }
            val extra = HashMap(i.extra)
            if (policy == "ask" && parent == null) extra["needs_decision"] = true
            if (guard.tripped) extra["guard"] = true
            val done = copyOf(i, i.id, "player_only", i.reason, default = if (del) "delete" else "keep",
                choices = if (i.parent != null) emptyList() else listOf("delete", "keep", "pull"), extra = extra)
            byId[i.id] = done
            items += done
        }

        val counts = linkedMapOf<String, Int>()
        for (a in listOf("add", "update", "move", "adopt", "record", "conflict", "blocked")) counts[a] = items.count { it.action == a }
        for (r in PlayerOnly.ALL) counts[r] = items.count { it.action == "player_only" && it.reason == r && it.parent == null }
        counts["excluded"] = notWanted.size
        counts["unreadable_kept"] = unreadableKept
        return LinkPlan(link, items, skip, guard, trackedByRel.size, counts, fpBytes)
    }

    private fun copyOf(i: PlanItem, id: String, action: String, reason: String, dest: String = i.dest, oldDest: String? = i.oldDest,
                       cardSize: Long = i.cardSize, default: String? = i.defaultChoice, choices: List<String> = i.choices,
                       thenUpdate: Boolean = i.thenUpdate, record: Map<String, Any?>? = i.record, extra: Map<String, Any?> = i.extra) =
        PlanItem(id, i.kind, i.link, action, reason, i.srcRel, dest, oldDest, i.size, i.mtime, cardSize, i.cardMtime, i.title, i.origin,
            default, choices, i.parent, thenUpdate, i.covers, record, extra)
}
