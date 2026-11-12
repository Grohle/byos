package org.hibyos.install.core.sync

import org.hibyos.install.core.Json
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpException
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/**
 * DEC.json (section 3.4): the user's choice per item id, plus per-kind policy overrides
 * (`{"i42":"delete","i77":"computer","policy":{"music":{"player_only":"delete"}}}`).
 */
object Decisions {
    /** The final choice of every item that takes one; sidecar children follow their parent. */
    fun resolve(items: List<PlanItem>, dec: Map<String, Any?>): Map<String, String> {
        val policy = (dec["policy"] as? Map<*, *>)?.entries?.associate { (k, v) ->
            k.toString() to ((v as? Map<*, *>)?.get("player_only")?.toString())
        } ?: emptyMap()
        val out = HashMap<String, String>()
        val byId = items.associateBy { it.id }
        for ((k, v) in dec) {
            if (k == "policy") continue
            val i = byId[k] ?: throw SyncException("usage", "DEC.json names $k, which is not an item of this plan")
            val c = v?.toString() ?: continue
            if (i.parent != null) throw SyncException("usage", "$k follows its track or folder; decide on ${i.parent} instead")
            if (c !in i.choices) throw SyncException("usage", "$k: \"$c\" is not one of ${i.choices.joinToString()}")
            out[k] = c
        }
        for (i in items) {
            if (i.parent != null || i.id in out) continue
            val c = when (i.action) {
                "player_only" -> when (policy[i.kind]) {
                    "delete" -> if (i.reason == PlayerOnly.DELETED && i.extra["guard"] != true && i.extra["card_changed"] != true) "delete" else i.defaultChoice
                    "keep" -> "keep"
                    else -> i.defaultChoice
                } ?: "keep"
                "conflict" -> i.defaultChoice ?: "skip"
                else -> null
            }
            if (c != null) out[i.id] = c
        }
        for (i in items) if (i.parent != null) out[i.id] = out[i.parent] ?: "keep"
        return out
    }
}

class ApplyOptions(
    val clientName: String,
    val takeOver: Boolean = false,
    /** Card checkpoint (and lock heartbeat) every N files or [checkpointSeconds]. */
    val checkpointEvery: Int = 200,
    val checkpointSeconds: Long = 60,
    /** Writes a prepared copy of [src] (covers optimised) to [out]; false when [src] is sent as it is. */
    val prepare: ((src: File, item: PlanItem, out: File) -> Boolean)? = null,
    val preparedDir: File? = null,
    /** Called after each journalled operation with the count so far (tests stop a run here). */
    val onRecord: ((Int) -> Unit)? = null,
)

class KindResult {
    var written = 0; var skipped = 0; var moved = 0; var removed = 0; var pulled = 0; var recorded = 0
    var failed = 0; var notFitted = 0; var blocked = 0; var conflictsLeft = 0; var kept = 0; var bytes = 0L
    fun toMap(): Map<String, Any?> = linkedMapOf("written" to written, "skipped" to skipped, "moved" to moved, "removed" to removed,
        "pulled" to pulled, "recorded" to recorded, "failed" to failed, "not_fitted" to notFitted, "blocked" to blocked,
        "conflicts_left" to conflictsLeft, "kept" to kept, "bytes" to bytes)
}

class ApplyResult(val status: String, val runId: String, val kinds: Map<String, KindResult>, val failures: List<Map<String, Any?>>,
                  val written: List<String>) {
    fun toMap(): Map<String, Any?> = linkedMapOf("status" to status, "run_id" to runId, "kinds" to kinds.mapValues { it.value.toMap() },
        "failures" to failures, "resumable" to (status.startsWith("paused") || status == "cancelled"))
}

/**
 * sync-apply (section 3.5): lock, battery check, journal replay, then per kind moves, approved
 * deletes, copies (one album folder at a time), pulls and the manifest fold. Every finished
 * operation is written to the local journal first (synced), and to a card checkpoint every
 * [ApplyOptions.checkpointEvery] files, so a killed or cut run neither copies again nor makes
 * " (2)" duplicates when planned again.
 */
class SyncApply(
    private val fs: CardFs,
    private val store: SyncStore,
    private val lf: LinksFile,
    private val opts: ApplyOptions,
    private val listener: SyncListener,
) {
    private class Pause(val status: String, cause: Throwable?) : Exception(status, cause)

    private lateinit var runId: String
    private lateinit var journal: RunJournal
    private val cardOps = ArrayList<Map<String, Any?>>()
    private var seq = 0L
    private var sinceCheckpoint = 0
    private var lastCheckpoint = System.currentTimeMillis()
    private var sdFree = 0L
    private val failures = ArrayList<Map<String, Any?>>()
    private val written = ArrayList<String>()
    private lateinit var manifests: Map<String, Manifest>
    private val now get() = System.currentTimeMillis() / 1000

    private fun checkCancel() { if (listener.cancelled()) throw Pause("cancelled", null) }

    // ------------------------------------------------------------------ lock

    fun acquireLock(kinds: List<String>) {
        val mine = LockInfo(lf.clientId, opts.clientName, runId, kinds, now, now)
        try {
            CardStore.writeJson(fs, CardStore.LOCK, mine.toMap(), Btp.MODE_CREATE)
        } catch (e: BtpException) {
            if (!(e.remote && e.code == Btp.E_EXISTS)) throw e
            val cur = CardStore.readLock(fs)
            if (cur != null && cur.clientId != lf.clientId && !cur.stale() && !opts.takeOver) {
                throw SyncException("locked", "The HiBy is being synced from ${cur.name.ifEmpty { "another computer" }}", mapOf("locked_by" to cur.toMap()))
            }
            if (cur != null && cur.clientId != lf.clientId) listener.warn("lock_taken_over", "took over the sync lock of ${cur.name}", mapOf("locked_by" to cur.toMap()))
            CardStore.writeJson(fs, CardStore.LOCK, mine.toMap(), Btp.MODE_REPLACE)
        }
    }

    private fun refreshLock(kinds: List<String>) {
        try { CardStore.writeJson(fs, CardStore.LOCK, LockInfo(lf.clientId, opts.clientName, runId, kinds, now, now).toMap(), Btp.MODE_REPLACE) }
        catch (e: BtpException) { if (!e.remote) throw e }
    }

    fun releaseLock() {
        try {
            val cur = CardStore.readLock(fs)
            if (cur == null || cur.clientId == lf.clientId) fs.delete(CardStore.LOCK)
        } catch (e: Exception) {}
    }

    // ------------------------------------------------------------------ journal

    private fun record(op: Map<String, Any?>, item: PlanItem?, kinds: List<String>) {
        val full = LinkedHashMap(op)
        if (item != null) full["item"] = item.id
        full["time"] = now
        journal.append(full)
        manifests[Json.optStr(op, "kind")]?.apply(full)
        cardOps += full
        sinceCheckpoint++
        if (sinceCheckpoint >= opts.checkpointEvery || System.currentTimeMillis() - lastCheckpoint > opts.checkpointSeconds * 1000) checkpoint(kinds)
        recorded++
        opts.onRecord?.invoke(recorded)
    }

    private var recorded = 0

    private fun checkpoint(kinds: List<String>) {
        sinceCheckpoint = 0
        lastCheckpoint = System.currentTimeMillis()
        if (cardOps.isEmpty()) return
        listener.progress("checkpoint", null, 0, 0, "none")
        fs.mkdirs(CardStore.JOURNAL)
        while (true) {
            seq++
            val p = "${CardStore.JOURNAL}/$runId-${"%06d".format(seq)}.json"
            try {
                CardStore.writeJson(fs, p, linkedMapOf("format" to 1, "run_id" to runId, "client_id" to lf.clientId, "seq" to seq,
                    "card_id" to cardId, "time" to now, "ops" to ArrayList(cardOps)), Btp.MODE_CREATE)
                break
            } catch (e: BtpException) { if (!(e.remote && e.code == Btp.E_EXISTS)) throw e }
        }
        cardOps.clear()
        refreshLock(kinds)
    }

    private var cardId: String? = null

    // ------------------------------------------------------------------ run

    /**
     * Applies [plan] with the [choices] of [Decisions.resolve]. When a journal of the same run
     * exists (sync-resume), the items it lists as done are skipped.
     */
    fun run(plan: Map<String, Any?>, choices: Map<String, String>): ApplyResult {
        runId = Json.optStr(plan, "run_id") ?: SyncPlanning.newRunId()
        val existing = store.runFile(runId).takeIf { it.isFile }?.let { RunJournal.read(it) }
        if (existing != null && existing.ended) throw SyncException("usage", "this plan was already applied; make a new plan (sync-plan)")
        val items = Json.arr(plan["items"]).map { PlanItem.fromMap(Json.obj(it)) }
        val kinds = Kinds.ALL.filter { k -> (plan["kinds"] as? List<*>)?.contains(k) == true }
        val results = kinds.associateWith { KindResult() }
        acquireLock(kinds)
        var status = "done"
        try {
            val st = fs.status()
            if (st != null && st.battery in 0..100 && st.charging != 1) {
                if (st.battery < 15) throw SyncException("battery", "The HiBy's battery is at ${st.battery} %: charge it before syncing", mapOf("battery" to st.battery))
                if (st.battery < 30) listener.warn("low_battery", "The HiBy's battery is at ${st.battery} %; keep it charging", mapOf("battery" to st.battery))
            }
            sdFree = fs.df().sdFree
            cardId = CardStore.ensureCardId(fs)
            val planCard = Json.optStr(plan, "card_id")
            if (planCard != null && planCard != cardId) throw SyncException("card_changed", "The card in the HiBy is not the one this plan was made for; plan again")
            val ctx = SyncPlanning.loadCardState(fs, store, lf, kinds, listener)
            manifests = ctx.manifests
            seq = ctx.cardJournals.filter { it.runId == runId }.maxOfOrNull { it.seq } ?: 0
            val done = existing?.doneItems ?: emptySet()
            if (existing != null) listener.warn("resumed", "resuming run $runId: ${done.size} items were already done", mapOf("run_id" to runId, "done" to done.size))
            journal = RunJournal(store.runFile(runId))
            try {
                if (existing == null) {
                    store.runsDir.mkdirs()
                    SyncStore.writeAtomic(store.runPlanFile(runId), Json.write(plan) + "\n")
                    SyncStore.writeAtomic(store.runDecisionsFile(runId), Json.write(choices) + "\n")
                    journal.append(linkedMapOf("op" to "begin", "run_id" to runId, "card_id" to cardId, "device_id" to store.deviceId,
                        "client_id" to lf.clientId, "kinds" to kinds, "time" to now))
                }
                cleanTrash()
                val folded = ArrayList<String>()
                try {
                    for (kind in kinds) {
                        applyKind(kind, items.filter { it.kind == kind && it.id !in done }, choices, results[kind]!!, kinds)
                        fold(kind, kinds)
                        folded += kind
                    }
                } catch (e: Pause) {
                    status = e.status
                } catch (e: BtpConnectionException) {
                    status = "paused"
                } catch (e: BtpException) {
                    if (e.remote && e.code == Btp.E_SD_UNAVAILABLE) status = "paused_usb_storage"
                    else if (e.code == Btp.E_CANCELLED) status = "cancelled"
                    else throw e
                }
                if (status == "done") {
                    // the journals this run has folded are no longer needed
                    for (j in ctx.cardJournals + CardStore.journals(fs).filter { it.runId == runId }) {
                        if (SyncPlanning.opKinds(j.ops).all { it in folded }) try { fs.delete(j.path) } catch (e: BtpException) { if (!e.remote) throw e }
                    }
                    for (r in ctx.localRuns) if (r.runId != runId && SyncPlanning.opKinds(r.ops).all { it in folded }) {
                        RunJournal(r.file).use { it.append(linkedMapOf("op" to "end", "status" to "folded_by:$runId", "time" to now)) }
                    }
                    cleanTrash()
                    if (results.values.any { it.failed > 0 || it.notFitted > 0 }) status = "done_with_problems"
                    journal.append(linkedMapOf("op" to "end", "status" to status, "time" to now))
                    store.saveLinks(lf)
                } else {
                    try { checkpoint(kinds) } catch (e: Exception) {}
                    journal.append(linkedMapOf("op" to "paused", "status" to status, "time" to now))
                    if (status == "paused_usb_storage") listener.warn("paused_usb_storage", "The HiBy's card became unavailable (USB storage?). Run sync-resume when it is back.")
                    else if (status == "paused") listener.warn("paused", "The connection to the HiBy was lost. Run sync-resume to go on.")
                }
            } finally { journal.close() }
        } finally {
            releaseLock()
        }
        return ApplyResult(status, runId, results, failures, written)
    }

    private fun cleanTrash() {
        val runs = try { fs.list(CardStore.TRASH) } catch (e: BtpException) { if (CardStore.isNotFound(e)) null else throw e } ?: return
        for (r in runs) if (r.isDir) try { fs.delete("${CardStore.TRASH}/${r.name}", recursive = true) } catch (e: BtpException) { if (!e.remote) throw e }
    }

    private fun linkOf(id: String) = lf.links.firstOrNull { it.id == id }

    private fun fail(item: PlanItem, code: String, message: String, res: KindResult) {
        res.failed++
        failures += linkedMapOf("id" to item.id, "dest" to item.dest, "src_rel" to item.srcRel, "error" to code, "message" to message)
        listener.warn("failed", "${item.dest}: $message", mapOf("path" to item.dest, "id" to item.id, "error" to code))
    }

    private fun exists(path: String) = fs.stat(path) != null

    private fun freeName(dest: String): String {
        var k = 2
        var d = SyncRules.numbered(dest, k)
        while (exists(d)) d = SyncRules.numbered(dest, ++k)
        return d
    }

    // ------------------------------------------------------------------ one kind

    private fun applyKind(kind: String, items: List<PlanItem>, choices: Map<String, String>, res: KindResult, kinds: List<String>) {
        res.blocked = items.count { it.action == "blocked" }
        for (i in items) if (i.action == "blocked") listener.warn("blocked", "${i.dest}: ${i.reason}", mapOf("path" to i.dest, "reason" to i.reason))
        // ---- moves (swaps go through a temporary name)
        val moves = items.filter { it.action == "move" && it.oldDest != null }
        val sources = moves.map { SyncRules.lower(it.oldDest!!) }.toSet()
        val tempOf = HashMap<String, String>()
        for (m in moves) if (SyncRules.lower(m.dest) in sources) {
            checkCancel()
            val tmp = SyncRules.parent(m.oldDest!!).let { if (it.isEmpty()) "" else "$it/" } + "byos-move-${m.id}-" + SyncRules.name(m.oldDest)
            if (!exists(m.oldDest)) continue
            fs.move(m.oldDest, tmp, false)
            record(linkedMapOf("op" to "move", "kind" to kind, "from" to m.oldDest, "to" to tmp), null, kinds)
            tempOf[m.id] = tmp
        }
        val emptied = ArrayList<String>()
        val puts = ArrayList<Pair<PlanItem, String>>()   // item, mode: add | replace | both
        for ((k, m) in moves.withIndex()) {
            checkCancel()
            listener.progress("move", m.dest, k.toLong(), moves.size.toLong(), "files")
            val from = tempOf[m.id] ?: m.oldDest!!
            if (!exists(from)) {
                // the old copy is gone: send the file to its new place instead
                puts += m to "add"; continue
            }
            if (exists(m.dest)) { fail(m, "exists", "a different file is already at the new place", res); continue }
            fs.mkdirs(SyncRules.parent(m.dest))
            fs.move(from, m.dest, false)
            record(linkedMapOf("op" to "move", "kind" to kind, "from" to from, "to" to m.dest, "rel" to m.srcRel), m, kinds)
            res.moved++
            emptied += m.oldDest!!
            if (m.thenUpdate) puts += m to "replace"
        }
        // ---- approved deletes: media first, then their sidecars
        val dels = items.filter { it.action == "player_only" && choices[it.id] == "delete" }.sortedBy { if (it.parent == null) 0 else 1 }
        res.kept = items.count { it.action == "player_only" && choices[it.id] != "delete" && it.parent == null }
        for ((k, d) in dels.withIndex()) {
            checkCancel()
            listener.progress("remove", d.dest, k.toLong(), dels.size.toLong(), "files")
            val link = linkOf(d.link)
            if (link == null || !SyncRules.under(d.dest, link.cardRoot) || SyncRules.alwaysExcludedOnCard(d.dest)) continue
            if (d.parent != null && Kinds.isFolderSidecar(kind, SyncRules.name(d.dest))) {
                // a folder's cover or CUE sheet goes only once no media file is left beside it
                val left = fs.list(SyncRules.parent(d.dest))?.any { !it.isDir && Kinds.isMedia(kind, it.name) } ?: false
                if (left) continue
            }
            val st = fs.stat(d.dest)
            if (st == null) { record(linkedMapOf("op" to "forget", "kind" to kind, "dest" to d.dest), d, kinds); continue }
            if (st.size != d.cardSize) { listener.warn("changed_since_plan", "${d.dest} changed on the HiBy since the plan; kept", mapOf("path" to d.dest)); continue }
            try { fs.delete(d.dest) } catch (e: BtpException) {
                if (!e.remote || e.code == Btp.E_SD_UNAVAILABLE) throw e
                fail(d, e.codeName, e.message ?: e.codeName, res); continue
            }
            record(linkedMapOf("op" to "delete", "kind" to kind, "dest" to d.dest), d, kinds)
            res.removed++
            sdFree += st.size
            emptied += d.dest
        }
        prune(emptied, kind)
        // ---- copies
        for (i in items) {
            when {
                i.action == "add" -> puts += i to "add"
                i.action == "update" -> puts += i to "replace"
                i.action == "conflict" && choices[i.id] == "computer" -> puts += i to "replace"
                i.action == "conflict" && choices[i.id] == "both" -> puts += i to "both"
                i.action == "conflict" && choices[i.id] == "skip" -> res.conflictsLeft++
            }
        }
        putAll(kind, puts, res, kinds)
        // ---- records (adopted files, renames, kept player versions)
        for (i in items) {
            checkCancel()
            when {
                i.action == "adopt" || (i.action == "record" && i.record?.get("forget") != true) -> {
                    val r = i.record ?: continue
                    val t = Tracked.fromMap(i.dest, r).copy(origin = lf.clientId, time = now)
                    record(linkedMapOf("op" to "record", "kind" to kind, "dest" to i.dest, "from" to Json.optStr(r, "from"), "entry" to t.toMap()), i, kinds)
                    res.recorded++
                }
                i.action == "record" -> { record(linkedMapOf("op" to "forget", "kind" to kind, "dest" to i.dest), i, kinds); res.recorded++ }
                i.action == "conflict" && choices[i.id] == "player" -> {
                    val link = linkOf(i.link) ?: continue
                    val t = Tracked(i.dest, link.id, i.srcRel ?: continue, lf.clientId, i.size, i.mtime, i.cardSize, null, i.cardMtime, i.covers ?: "keep", now)
                    record(linkedMapOf("op" to "record", "kind" to kind, "dest" to i.dest, "entry" to t.toMap()), i, kinds)
                    res.recorded++
                }
            }
        }
        // ---- copies to this computer
        for (i in items.filter { it.action == "player_only" && choices[it.id] == "pull" && it.parent == null }) {
            checkCancel()
            pull(kind, i, res, kinds)
            // its sidecars come too
            for (c in items.filter { it.parent == i.id }) pull(kind, c, res, kinds)
        }
    }

    private fun prune(removed: List<String>, kind: String) {
        val roots = lf.links.filter { it.kind == kind }.map { SyncRules.lower(it.cardRoot) }.toSet()
        val dirs = removed.map { SyncRules.parent(it) }.filter { it.isNotEmpty() }.toSortedSet(compareByDescending<String> { it.count { c -> c == '/' } }.thenBy { it })
        val seen = HashSet<String>()
        while (dirs.isNotEmpty()) {
            val d = dirs.first(); dirs.remove(d)
            if (!seen.add(SyncRules.lower(d)) || SyncRules.lower(d) in roots || !d.contains('/')) continue
            val entries = try { fs.list(d, hidden = true) } catch (e: BtpException) { if (e.remote) null else throw e } ?: continue
            if (entries.isNotEmpty()) continue
            try { fs.delete(d) } catch (e: BtpException) { if (!e.remote) throw e; continue }
            dirs += SyncRules.parent(d)
        }
    }

    // ------------------------------------------------------------------ copies

    private fun putAll(kind: String, puts: List<Pair<PlanItem, String>>, res: KindResult, kinds: List<String>) {
        if (puts.isEmpty()) return
        val total = puts.sumOf { it.first.size }
        // one album folder at a time; the smallest first when space is tight
        val tight = total + SyncPlanning.MARGIN > sdFree
        val groups = puts.groupBy { SyncRules.lower(SyncRules.parent(it.first.dest)) }.values
            .sortedWith(if (tight) compareBy { g -> g.sumOf { it.first.size } } else compareBy { g -> SyncRules.lower(g.first().first.dest) })
        var doneBytes = 0L
        var doneFiles = 0
        val t0 = System.currentTimeMillis()
        val made = HashSet<String>()
        outer@ for (g in groups) for ((item, mode) in g) {
            checkCancel()
            listener.progress("sync", item.dest, doneBytes, total, "bytes", mapOf("files_done" to doneFiles, "files_total" to puts.size,
                "rate" to rate(doneBytes, t0)))
            val link = linkOf(item.link)
            val src = link?.let { File(it.local, item.srcRel ?: "") }
            if (link == null || src == null || !src.isFile || src.length() != item.size || src.lastModified() / 1000 != item.mtime) {
                listener.warn("changed_since_plan", "${item.srcRel} changed or went away since the plan; plan again", mapOf("path" to item.dest))
                res.skipped++
                continue
            }
            try {
                val dest = if (mode == "both") freeName(item.dest) else item.dest
                val parent = SyncRules.parent(dest)
                if (parent.isNotEmpty() && made.add(SyncRules.lower(parent))) fs.mkdirs(parent)
                val srcSha = SyncRules.shaFile(src, listener::cancelled) { d, t -> listener.progress("hash", dest, d, t) }
                val send = prepared(src, item, srcSha)
                val sentSha = if (send == src) srcSha else SyncRules.shaFile(send, listener::cancelled) { d, t -> listener.progress("hash", dest, d, t) }
                val size = send.length()
                val oldSize = if (mode == "replace") maxOf(0L, item.cardSize) else 0L
                var r: Int
                if (mode == "replace" && sdFree - size + oldSize < SyncPlanning.MARGIN && exists(dest)) {
                    // nearly full: move the old copy aside, send the new one, then drop the old one
                    val trash = "${CardStore.TRASH}/$runId/$dest"
                    fs.mkdirs(SyncRules.parent(trash))
                    fs.move(dest, trash, true)
                    try {
                        r = fs.put(dest, send, sentSha, Btp.MODE_CREATE, item.mtime) { d, t -> listener.progress("put", dest, d, t) }
                    } catch (e: Exception) {
                        try { fs.move(trash, dest, false) } catch (x: Exception) {}
                        throw e
                    }
                    fs.delete(trash)
                } else {
                    val m = when (mode) { "add" -> Btp.MODE_SKIP_IDENTICAL; "both" -> Btp.MODE_CREATE; else -> Btp.MODE_REPLACE }
                    r = fs.put(dest, send, sentSha, m, item.mtime) { d, t -> listener.progress("put", dest, d, t) }
                }
                val st = fs.stat(dest)
                val entry = Tracked(dest, link.id, item.srcRel!!, lf.clientId, item.size, item.mtime, size, SyncRules.hex(sentSha),
                    st?.mtime ?: 0, item.covers ?: "keep", now, SyncRules.hex(srcSha), SyncRules.qfpFile(send))
                val from = if (item.action == "move" && mode == "add") item.oldDest else null
                record(linkedMapOf("op" to "put", "kind" to kind, "dest" to dest, "from" to from, "entry" to entry.toMap()), item, kinds)
                if (send != src) send.delete()
                if (r == 1) res.skipped++ else { res.written++; res.bytes += size; written += dest }
                sdFree -= size - oldSize
            } catch (e: BtpException) {
                if (!e.remote) throw e
                when (e.code) {
                    Btp.E_NO_SPACE -> {
                        val left = puts.size - doneFiles
                        res.notFitted += left
                        listener.warn("no_space", "$left files did not fit on the HiBy's card", mapOf("count" to left))
                        break@outer
                    }
                    Btp.E_SD_UNAVAILABLE, Btp.E_CANCELLED, Btp.E_BUSY -> throw e
                    Btp.E_EXISTS -> { listener.warn("conflict", "${item.dest}: a different file is already there; kept", mapOf("path" to item.dest)); fail(item, "exists", "a different file is already there", res) }
                    Btp.E_FILE_TOO_BIG -> fail(item, "too_big", "too big for the card's file system", res)
                    else -> fail(item, e.codeName, e.message ?: e.codeName, res)
                }
            } catch (e: SyncException) {
                if (e.code == "cancelled") throw Pause("cancelled", e)
                throw e
            } catch (e: java.io.IOException) {
                if (e is BtpConnectionException) throw e
                fail(item, "local_io", e.message ?: "read error", res)
            }
            doneFiles++
            doneBytes += item.size
        }
        listener.progress("sync", null, doneBytes, total, "bytes", mapOf("files_done" to doneFiles, "files_total" to puts.size))
    }

    private fun rate(done: Long, t0: Long): Long { val dt = System.currentTimeMillis() - t0; return if (dt <= 0) 0 else done * 1000 / dt }

    /**
     * The bytes to send for [src]: a prepared copy from the content-addressed cache (the same
     * source and options always give the same bytes, so a resumed PUT continues its part file),
     * or [src] itself.
     */
    private fun prepared(src: File, item: PlanItem, srcSha: ByteArray): File {
        val p = opts.prepare ?: return src
        val dir = opts.preparedDir ?: return src
        if (item.covers == null || item.covers == "keep") return src
        dir.mkdirs()
        val key = SyncRules.hex(BtpCrypto.sha256((SyncRules.hex(srcSha) + "|covers=" + item.covers + "|v1").toByteArray()))
        val ext = Kinds.ext(src.name)
        val f = File(dir, "$key.$ext")
        val same = File(dir, "$key.same")
        if (f.isFile && f.length() > 0) return f
        if (same.isFile) return src
        val tmp = File(dir, "$key.$ext.tmp")
        tmp.delete()
        val made = try { p(src, item, tmp) } catch (e: Exception) { false }
        if (!made || !tmp.isFile) { tmp.delete(); same.writeText(""); return src }
        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING)
        return f
    }

    // ------------------------------------------------------------------ pulls

    private fun pull(kind: String, i: PlanItem, res: KindResult, kinds: List<String>) {
        val link = linkOf(i.link) ?: return
        if (!SyncRules.under(i.dest, link.cardRoot)) return
        val rel = i.dest.substring(link.cardRoot.length + 1).split('/').map { SyncRules.windowsSafe(it) }.joinToString("/")
        var target = File(link.local, rel)
        var k = 2
        while (target.exists()) target = File(link.local, SyncRules.numbered(rel, k++))
        val finalRel = target.relativeTo(File(link.local)).path.replace('\\', '/')
        target.absoluteFile.parentFile?.mkdirs()
        val part = File(target.path + ".byos-part")
        try {
            fs.get(i.dest, part) { d, t -> listener.progress("pull", i.dest, d, t) }
            Files.move(part.toPath(), target.toPath())
        } catch (e: BtpException) {
            part.delete()
            if (!e.remote || e.code == Btp.E_SD_UNAVAILABLE) throw e
            fail(i, e.codeName, e.message ?: e.codeName, res); return
        }
        val t = Tracked(i.dest, link.id, SyncRules.nfc(finalRel), lf.clientId, target.length(), target.lastModified() / 1000, i.cardSize, null,
            i.cardMtime, if (kind == Kinds.MUSIC) link.covers else "keep", now)
        record(linkedMapOf("op" to "record", "kind" to kind, "dest" to i.dest, "entry" to t.toMap()), i, kinds)
        res.pulled++
    }

    // ------------------------------------------------------------------ fold

    private fun fold(kind: String, kinds: List<String>) {
        checkpoint(kinds)
        val man = manifests[kind]!!
        listener.progress("checkpoint", CardStore.manifestPath(kind), 0, 0, "none")
        CardStore.writeJson(fs, CardStore.manifestPath(kind), man.toMap(), Btp.MODE_REPLACE)
        journal.append(linkedMapOf("op" to "kind_done", "kind" to kind, "time" to now))
        for (link in lf.links.filter { it.kind == kind }) {
            val entries = man.forLink(link.id)
            store.saveState(link.id, cardId, entries)
            link.entriesLast = entries.size.toLong()
            if (link.volumeId == null) link.volumeId = LocalScanner.volumeId(File(link.local))
        }
    }
}
