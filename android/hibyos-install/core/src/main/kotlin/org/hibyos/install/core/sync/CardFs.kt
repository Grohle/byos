package org.hibyos.install.core.sync

import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.DfR
import org.hibyos.install.core.btp.ListEntry
import org.hibyos.install.core.btp.StatR
import org.hibyos.install.core.btp.StatusR
import java.io.File

/**
 * The card as the sync engine sees it. byos-cli implements it over BTP (with reconnects);
 * tests can implement it over a folder. Device errors are [BtpException]s with remote = true;
 * a link that stays down is a [org.hibyos.install.core.btp.BtpConnectionException].
 */
interface CardFs {
    /** The entries of [dir] (hidden ones left out), or null when there is no such folder. */
    fun list(dir: String, hidden: Boolean = false): List<ListEntry>?
    fun stat(path: String): StatR?
    /** Up to [len] bytes of [path] from [off]. */
    fun read(path: String, off: Long, len: Int): ByteArray
    /** Uploads [file] (resuming a part left by an earlier try). Returns 0 written, 1 skipped (identical). */
    fun put(path: String, file: File, sha: ByteArray, mode: Int, mtime: Long, progress: (Long, Long) -> Unit): Int
    fun putBytes(path: String, data: ByteArray, mode: Int)
    fun mkdirs(dir: String)
    fun move(src: String, dst: String, replace: Boolean)
    fun delete(path: String, recursive: Boolean = false)
    fun df(): DfR
    /** Battery and charging, or null when the device does not report them. */
    fun status(): StatusR?
    /** Downloads [path] into [dst] (written whole; the caller renames it). */
    fun get(path: String, dst: File, progress: (Long, Long) -> Unit)
}

/** Listener for scans, plans and applies: progress, warnings, cancellation. */
interface SyncListener {
    fun progress(op: String, path: String?, done: Long, total: Long, unit: String = "bytes", extra: Map<String, Any?> = emptyMap()) {}
    fun warn(code: String, message: String, extra: Map<String, Any?> = emptyMap()) {}
    fun cancelled(): Boolean = false
}

/** The sync files on the card (section 2.2). */
object CardStore {
    const val DIR = "ByOS/sync"
    const val CARD = "ByOS/sync/card.json"
    const val LOCK = "ByOS/sync/lock.json"
    const val V1 = "ByOS/sync/manifest.json"
    const val JOURNAL = "ByOS/sync/journal"
    const val TRASH = "ByOS/sync/trash"
    fun manifestPath(kind: String) = "ByOS/sync/manifest-v2/$kind.json"

    private const val MAX_JSON = 64L shl 20

    fun isNotFound(e: BtpException) = e.remote && e.code == Btp.E_NOT_FOUND

    fun readText(fs: CardFs, path: String): String? {
        val st = fs.stat(path) ?: return null
        if (st.kind != 0 || st.size > MAX_JSON) return null
        if (st.size == 0L) return ""
        val out = java.io.ByteArrayOutputStream(st.size.toInt())
        var off = 0L
        while (off < st.size) {
            val b = fs.read(path, off, minOf(st.size - off, 8L shl 20).toInt())
            if (b.isEmpty()) break
            out.write(b); off += b.size
        }
        return String(out.toByteArray(), Charsets.UTF_8)
    }

    fun readJson(fs: CardFs, path: String): Map<String, Any?>? {
        val t = readText(fs, path) ?: return null
        return try { Json.obj(Json.parse(t)) } catch (e: JsonException) { null }
    }

    fun writeJson(fs: CardFs, path: String, m: Map<String, Any?>, mode: Int = Btp.MODE_REPLACE) {
        fs.mkdirs(SyncRules.parent(path))
        fs.putBytes(path, (Json.write(m) + "\n").toByteArray(Charsets.UTF_8), mode)
    }

    /** The card's id (card.json), or null when the card has none yet. */
    fun cardId(fs: CardFs): String? = readJson(fs, CARD)?.let { Json.optStr(it, "card_id") }

    /** The card's id, written once when missing. */
    fun ensureCardId(fs: CardFs): String {
        cardId(fs)?.let { return it }
        val id = java.util.UUID.nameUUIDFromBytes(BtpCrypto.random(16)).toString()
        try {
            writeJson(fs, CARD, linkedMapOf("card_id" to id, "created" to System.currentTimeMillis() / 1000), Btp.MODE_CREATE)
        } catch (e: BtpException) {
            if (!(e.remote && e.code == Btp.E_EXISTS)) throw e
            return cardId(fs) ?: throw e
        }
        return id
    }

    /**
     * The manifest of [kind]; for music, the branch's v1 manifest is folded in (once per entry).
     * [migrated] gets the number of v1 entries taken over.
     */
    fun loadManifest(fs: CardFs, kind: String, links: List<Link>, clientId: String, listener: SyncListener): Manifest {
        val m = readText(fs, manifestPath(kind))?.let { t ->
            try { Manifest.fromMap(kind, Json.obj(Json.parse(t))) } catch (e: JsonException) {
                listener.warn("manifest_unreadable", "${manifestPath(kind)} could not be read; the last sync state on this computer is used")
                null
            }
        } ?: Manifest(kind)
        if (kind == Kinds.MUSIC) {
            val v1 = readText(fs, V1)
            if (v1 != null) {
                val sha = SyncRules.hex(BtpCrypto.sha256(v1.toByteArray(Charsets.UTF_8)))
                val unresolved = m.files.values.any { it.link == null && it.srcAbs != null }
                if (sha != m.migratedV1 || unresolved) {
                    val parsed = try { Json.obj(Json.parse(v1)) } catch (e: JsonException) { null }
                    if (parsed != null) {
                        val n = Manifest.migrateV1(m, parsed, links.filter { it.kind == Kinds.MUSIC }, clientId)
                        m.migratedV1 = sha
                        if (n > 0) listener.warn("manifest_migrated", "$n files copied by an earlier sync are now tracked per music folder",
                            mapOf("count" to n))
                    }
                }
            }
        }
        return m
    }

    class CardJournal(val path: String, val runId: String, val clientId: String, val seq: Long, val cardId: String?, val ops: List<Map<String, Any?>>)

    /** Checkpoints left on the card by runs that did not finish (any client). */
    fun journals(fs: CardFs): List<CardJournal> {
        val entries = try { fs.list(JOURNAL) } catch (e: BtpException) { if (isNotFound(e)) null else throw e } ?: return emptyList()
        return entries.filter { it.kind == 0 && it.name.endsWith(".json") }.sortedBy { it.name }.mapNotNull { e ->
            val p = "$JOURNAL/${e.name}"
            val m = readJson(fs, p) ?: return@mapNotNull null
            CardJournal(p, Json.optStr(m, "run_id") ?: "", Json.optStr(m, "client_id") ?: "", Json.optLong(m, "seq", 0), Json.optStr(m, "card_id"),
                (m["ops"] as? List<*>)?.mapNotNull { (it as? Map<*, *>)?.let { o -> Json.obj(o) } } ?: emptyList())
        }.sortedWith(compareBy({ it.runId }, { it.seq }))
    }

    fun readLock(fs: CardFs): LockInfo? = readJson(fs, LOCK)?.let { LockInfo.fromMap(it) }

    /** Every file under [root] (hidden names left out), recursively. */
    fun walk(fs: CardFs, root: String, listener: SyncListener): List<CardFile> {
        val out = ArrayList<CardFile>()
        var folders = 0L
        fun go(d: String, depth: Int) {
            if (listener.cancelled()) throw SyncException("cancelled", "cancelled")
            folders++
            listener.progress("list", d, folders, 0, "folders")
            val entries = fs.list(d) ?: return
            for (e in entries) {
                if (e.name.startsWith(".")) continue
                val p = if (d.isEmpty()) e.name else "$d/${e.name}"
                if (e.isDir) { if (depth < 31) go(p, depth + 1) }
                else if (e.kind == 0) out += CardFile(SyncRules.nfc(p), e.size, e.mtime)
            }
        }
        go(root, root.count { it == '/' } + 1)
        return out
    }
}
