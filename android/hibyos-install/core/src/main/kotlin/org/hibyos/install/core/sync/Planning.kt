package org.hibyos.install.core.sync

import org.hibyos.install.core.Json
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.DfR
import java.io.File
import java.io.IOException

/** What the card's sync state is once the manifests are read and the unfinished journals replayed. */
class CardContext(
    val cardId: String?,
    val manifests: Map<String, Manifest>,
    val cardJournals: List<CardStore.CardJournal>,
    val localRuns: List<RunJournal.Contents>,
)

class PlanRequest(
    val kinds: List<String>,
    /** --delete-untracked: player-only files no sync of this computer wrote are pre-ticked too. */
    val deleteUntracked: Boolean = false,
    /** Per-kind override of the links' player_only policy. */
    val policies: Map<String, String> = emptyMap(),
    val deviceId: String = "",
)

/**
 * sync-plan (section 3.4): reads the card's state, compares every enabled link of the asked
 * kinds with it and returns the plan JSON (format 2). Nothing on the card changes.
 */
object SyncPlanning {
    /** Free space the plan keeps on the card. */
    const val MARGIN = 64L shl 20
    /** Rough transfer speed for the time estimate (USB, bytes per second). */
    private const val RATE = 6L shl 20

    fun opKinds(ops: List<Map<String, Any?>>) = ops.mapNotNull { Json.optStr(it, "kind") }.toSet()

    /** Reads the manifests of [kinds] and replays card checkpoints and this computer's unfinished runs into them. */
    fun loadCardState(fs: CardFs, store: SyncStore, lf: LinksFile, kinds: List<String>, listener: SyncListener): CardContext {
        val cardId = CardStore.cardId(fs)
        val manifests = LinkedHashMap<String, Manifest>()
        for (k in kinds) manifests[k] = CardStore.loadManifest(fs, k, lf.links, lf.clientId, listener)
        val journals = CardStore.journals(fs).filter { it.cardId == null || cardId == null || it.cardId == cardId }
        for (j in journals) for (op in j.ops) manifests[Json.optStr(op, "kind")]?.apply(op)
        val runs = if (cardId == null) emptyList() else store.unfinishedRuns().filter { it.cardId == cardId }
        for (r in runs) for (op in r.ops) manifests[Json.optStr(op, "kind")]?.apply(op)
        val n = journals.sumOf { it.ops.size } + runs.sumOf { it.ops.count { o -> o["kind"] != null } }
        if (n > 0) listener.warn("journal_replayed", "$n operations of an earlier sync that did not finish were taken into account",
            mapOf("count" to n, "runs" to (runs.map { it.runId } + journals.map { it.runId }).distinct()))
        return CardContext(cardId, manifests, journals, runs)
    }

    fun fsName(df: DfR) = DfR.SD_FS.getOrNull(df.sdFs) ?: "unknown"

    /** The quick fingerprint of a card file: two short reads. */
    fun cardQfp(fs: CardFs, c: CardFile): String {
        val (h, t) = SyncRules.qfpRanges(c.size)
        val head = if (h.second == 0) ByteArray(0) else fs.read(c.path, h.first, h.second)
        val tail = if (t.second == 0) ByteArray(0) else fs.read(c.path, t.first, t.second)
        return SyncRules.qfp(c.size, head, tail)
    }

    /** Card roots of other enabled links that lie inside [link]'s root. */
    fun nestedRoots(lf: LinksFile, link: Link) =
        lf.links.filter { it.id != link.id && it.enabled && SyncRules.under(it.cardRoot, link.cardRoot) }.map { it.cardRoot }

    fun plan(fs: CardFs, store: SyncStore, lf: LinksFile, scans: Map<String, LocalScan>, req: PlanRequest, listener: SyncListener): Map<String, Any?> {
        val df = fs.df()
        val fsName = fsName(df)
        val ctx = loadCardState(fs, store, lf, req.kinds, listener)
        val ids = IdGen()
        val items = ArrayList<PlanItem>()
        val summary = LinkedHashMap<String, Any?>()
        val linksOut = ArrayList<Map<String, Any?>>()
        val qfpCache = store.loadCardQfp()
        var bytesAddAll = 0L; var bytesRemoveAll = 0L
        for (kind in req.kinds) {
            val links = lf.links.filter { it.kind == kind && it.enabled }
            if (links.isEmpty()) continue
            val man = ctx.manifests[kind]!!
            val counts = LinkedHashMap<String, Int>()
            var skip = 0; var fp = 0L; var bytesAdd = 0L; var bytesRemove = 0L
            var guardTripped = false; var guardReason: String? = null
            for (link in links) {
                val scan = scans[link.id] ?: throw SyncException("usage", "the folder of link ${link.id} has not been scanned (sync-scan)")
                listener.progress("list", link.cardRoot, 0, 0, "folders")
                val card = CardStore.walk(fs, link.cardRoot, listener)
                val tracked = man.forLink(link.id).toMutableList()
                // state.json of this card: files whose manifest entry got lost stay tracked
                store.loadState(link.id)?.takeIf { it.cardId != null && it.cardId == ctx.cardId }?.let { st ->
                    val sizes = card.associate { SyncRules.lower(it.path) to it.size }
                    for (t in st.files.values) if (!man.has(link.id, t.rel) && man.get(t.dest) == null && sizes[SyncRules.lower(t.dest)] == t.sentSize) tracked += t
                }
                val others = HashMap<String, Tracked>()
                for ((k, t) in man.files) if (t.link != link.id) others[k] = t
                val root = File(link.local)
                var scanDirty = false
                val di = DiffInput(link, scan, tracked, others, card, lf.clientId, fsName, nestedRoots(lf, link), req.deleteUntracked, req.policies[kind],
                    localQfp = { rel ->
                        scan.entries[rel]?.let { e ->
                            e.qfp ?: try { SyncRules.qfpFile(File(root, rel)).also { e.qfp = it; scanDirty = true } } catch (x: IOException) { null }
                        }
                    },
                    cardQfp = { c ->
                        val key = "${c.path}|${c.size}|${c.mtime}"
                        qfpCache[key] ?: try {
                            listener.progress("fingerprint", c.path, 0, 0, "none")
                            cardQfp(fs, c).also { qfpCache[key] = it }
                        } catch (x: BtpException) { if (!x.remote) throw x; null }
                    },
                    localSha = { rel ->
                        scan.entries[rel]?.let { e ->
                            e.sha ?: try {
                                SyncRules.hex(SyncRules.shaFile(File(root, rel), listener::cancelled) { d, t -> listener.progress("hash", rel, d, t) })
                                    .also { e.sha = it; scanDirty = true }
                            } catch (x: IOException) { null }
                        }
                    })
                val lp = FolderSync.diff(di, ids)
                if (scanDirty) store.saveScan(scan)
                items += lp.items
                skip += lp.skip; fp += lp.bytesFingerprint
                for ((k, v) in lp.counts) counts[k] = (counts[k] ?: 0) + v
                if (lp.guard.tripped && !guardTripped) { guardTripped = true; guardReason = lp.guard.reason }
                for (i in lp.items) {
                    bytesAdd += when {
                        i.action == "add" -> i.size
                        i.action == "update" || (i.action == "move" && i.thenUpdate) -> maxOf(0, i.size - maxOf(0, i.cardSize))
                        else -> 0
                    }
                    if (i.action == "player_only" && i.defaultChoice == "delete") bytesRemove += i.size
                }
                linksOut += linkedMapOf("id" to link.id, "kind" to kind, "label" to link.label, "local" to link.local, "card_root" to link.cardRoot,
                    "layout" to link.layout, "scan" to linkedMapOf("status" to scan.status, "time" to scan.time, "errors" to scan.errors,
                        "unreadable" to scan.unreadableDirs.size), "guard" to lp.guard.toMap(), "tracked" to lp.tracked)
            }
            bytesAddAll += bytesAdd; bytesRemoveAll += bytesRemove
            val po = linkedMapOf<String, Any?>()
            for (r in PlayerOnly.ALL) po[r] = counts[r] ?: 0
            val kindItems = items.filter { it.kind == kind }
            summary[kind] = linkedMapOf("add" to (counts["add"] ?: 0), "update" to (counts["update"] ?: 0), "move" to (counts["move"] ?: 0),
                "adopt" to (counts["adopt"] ?: 0), "record" to (counts["record"] ?: 0), "skip" to skip, "conflict" to (counts["conflict"] ?: 0),
                "player_only" to po, "guard" to linkedMapOf("tripped" to guardTripped, "reason" to guardReason),
                "blocked" to (counts["blocked"] ?: 0), "excluded" to (counts["excluded"] ?: 0), "unreadable_kept" to (counts["unreadable_kept"] ?: 0),
                "needs_decision" to kindItems.count { it.extra["needs_decision"] == true },
                "bytes_add" to bytesAdd, "bytes_remove" to bytesRemove, "bytes_fingerprint" to fp, "sd_free" to df.sdFree,
                "fits" to (bytesAdd - bytesRemove + MARGIN <= df.sdFree),
                "eta_s" to (bytesAdd / RATE + kindItems.count { it.action != "player_only" } / 20))
        }
        store.saveCardQfp(qfpCache)
        items.sortWith(compareBy({ Kinds.ALL.indexOf(it.kind) }, { ORDER.indexOf(it.action) }, { it.dest.lowercase() }))
        return linkedMapOf("format" to 2, "device_id" to req.deviceId, "card_id" to ctx.cardId, "client_id" to lf.clientId,
            "run_id" to newRunId(), "created" to System.currentTimeMillis() / 1000, "kinds" to req.kinds.filter { summary.containsKey(it) },
            "fs" to fsName, "sd_free" to df.sdFree, "fits" to (bytesAddAll - bytesRemoveAll + MARGIN <= df.sdFree),
            "summary" to summary, "links" to linksOut, "items" to items.map { it.toMap() })
    }

    private val ORDER = listOf("conflict", "blocked", "player_only", "move", "update", "add", "adopt", "record")

    fun newRunId(): String {
        val t = java.text.SimpleDateFormat("yyyyMMdd'T'HHmmss", java.util.Locale.ROOT).apply { timeZone = java.util.TimeZone.getTimeZone("UTC") }
            .format(java.util.Date())
        return t + "-" + SyncStore.newId()
    }
}
