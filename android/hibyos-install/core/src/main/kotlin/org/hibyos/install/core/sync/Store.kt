package org.hibyos.install.core.sync

import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import org.hibyos.install.core.btp.BtpCrypto
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** links.json: the linked folders of one device. */
class LinksFile(val clientId: String, var clientName: String, val links: MutableList<Link>) {
    fun find(id: String): Link? = links.firstOrNull { it.id == id } ?: links.filter { it.id.startsWith(id) }.singleOrNull()

    fun toMap(): Map<String, Any?> = linkedMapOf("format" to 1, "client_id" to clientId, "client_name" to clientName,
        "links" to links.map { it.toMap() })
}

/** What one link last synced (state.json): the card it went to and the files, by rel. */
class LinkState(val cardId: String?, val files: Map<String, Tracked>)

/**
 * The sync files on this computer (section 2.1): `<config>/sync/<device_id>/links.json`, the
 * per-link `local.json` (last scan) and `state.json` (last good sync), the run journals and a
 * cache of card fingerprints.
 */
class SyncStore(val configDir: File, val deviceId: String) {
    val dir = File(configDir, "sync" + File.separator + deviceId)
    val linksFile = File(dir, "links.json")
    val runsDir = File(dir, "runs")
    val cardQfpFile = File(dir, "card-qfp.json")

    fun linkDir(id: String) = File(dir, id)
    fun localFile(id: String) = File(linkDir(id), "local.json")
    fun stateFile(id: String) = File(linkDir(id), "state.json")

    companion object {
        /** This computer's client id, made once per configuration folder. */
        fun clientId(configDir: File): String {
            val f = File(configDir, "sync" + File.separator + "client.json")
            if (f.isFile) try {
                Json.optStr(Json.obj(Json.parse(f.readText())), "client_id")?.let { return it }
            } catch (e: JsonException) {}
            val id = SyncRules.hex(BtpCrypto.random(8))
            f.parentFile.mkdirs()
            writeAtomic(f, Json.write(linkedMapOf("client_id" to id)) + "\n")
            return id
        }

        fun writeAtomic(f: File, text: String) {
            f.absoluteFile.parentFile?.mkdirs()
            val tmp = File(f.path + ".tmp")
            FileOutputStream(tmp).use { o -> o.write(text.toByteArray(Charsets.UTF_8)); o.fd.sync() }
            Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        }

        fun readJson(f: File): Map<String, Any?>? = if (!f.isFile) null else try {
            Json.obj(Json.parse(f.readText()))
        } catch (e: JsonException) { null }

        fun newId(): String = SyncRules.hex(BtpCrypto.random(4))
    }

    fun loadLinks(clientName: String): LinksFile {
        val m = readJson(linksFile)
        val cid = clientId(configDir)
        if (m == null) return LinksFile(cid, clientName, ArrayList())
        val links = (m["links"] as? List<*>)?.mapNotNull { v -> try { Link.fromMap(Json.obj(v)) } catch (e: Exception) { null } }?.toMutableList() ?: ArrayList()
        return LinksFile(cid, Json.optStr(m, "client_name") ?: clientName, links)
    }

    fun saveLinks(lf: LinksFile) = writeAtomic(linksFile, Json.write(lf.toMap()) + "\n")

    fun loadScan(id: String): LocalScan? = readJson(localFile(id))?.let { try { LocalScan.fromMap(it) } catch (e: JsonException) { null } }

    fun saveScan(s: LocalScan) = writeAtomic(localFile(s.linkId), Json.write(s.toMap()) + "\n")

    fun loadState(id: String): LinkState? {
        val m = readJson(stateFile(id)) ?: return null
        return try {
            LinkState(Json.optStr(m, "card_id"), Json.obj(m["files"]).map { (rel, v) ->
                val e = Json.obj(v)
                rel to Tracked.fromMap(Json.str(e, "dest"), e)
            }.toMap())
        } catch (e: JsonException) { null }
    }

    fun saveState(id: String, cardId: String?, files: Collection<Tracked>) {
        writeAtomic(stateFile(id), Json.write(linkedMapOf("format" to 1, "card_id" to cardId, "time" to System.currentTimeMillis() / 1000,
            "files" to files.sortedBy { it.rel }.associate { it.rel to (linkedMapOf<String, Any?>("dest" to it.dest) + it.toMap()) })) + "\n")
    }

    fun loadCardQfp(): MutableMap<String, String> {
        val m = readJson(cardQfpFile) ?: return HashMap()
        return HashMap((m["files"] as? Map<*, *>)?.entries?.associate { it.key.toString() to it.value.toString() } ?: emptyMap())
    }

    fun saveCardQfp(m: Map<String, String>) = writeAtomic(cardQfpFile, Json.write(linkedMapOf("format" to 1, "files" to m)) + "\n")

    /** The file [LocalRunLock] locks while a run of this HiBy applies. */
    val runLockFile = File(dir, "apply.lock")

    fun runFile(runId: String) = File(runsDir, "$runId.jsonl")
    fun runPlanFile(runId: String) = File(runsDir, "$runId.plan.json")
    fun runDecisionsFile(runId: String) = File(runsDir, "$runId.dec.json")

    /** Runs whose journal has no end line: their finished operations are replayed before planning. */
    fun unfinishedRuns(): List<RunJournal.Contents> =
        (runsDir.listFiles { f -> f.name.endsWith(".jsonl") } ?: emptyArray()).sortedBy { it.lastModified() }
            .map { RunJournal.read(it) }.filter { !it.ended }
}

/**
 * The journal of one apply run, on this computer: one JSON line per finished operation, each
 * written and synced before the next operation starts, so a killed run loses at most the file
 * it was copying (section 3.5).
 */
class RunJournal(val file: File) : AutoCloseable {
    private val out: FileOutputStream

    init {
        file.absoluteFile.parentFile?.mkdirs()
        out = FileOutputStream(file, true)
    }

    fun append(m: Map<String, Any?>) {
        out.write((Json.write(m) + "\n").toByteArray(Charsets.UTF_8))
        out.flush()
        out.fd.sync()
    }

    override fun close() = out.close()

    class Contents(val file: File, val runId: String, val begin: Map<String, Any?>?, val ops: List<Map<String, Any?>>, val ended: Boolean,
                   val endStatus: String?) {
        val cardId get() = begin?.let { Json.optStr(it, "card_id") }
        /** Item ids already done (they are skipped when the run is resumed). */
        val doneItems: Set<String> get() = ops.mapNotNull { Json.optStr(it, "item") }.toSet()
        val pausedStatus: String? get() = ops.lastOrNull { it["op"] == "paused" }?.let { Json.optStr(it, "status") }
    }

    companion object {
        fun read(f: File): Contents {
            val lines = try { f.readLines(Charsets.UTF_8) } catch (e: Exception) { emptyList() }
            var begin: Map<String, Any?>? = null
            val ops = ArrayList<Map<String, Any?>>()
            var ended = false
            var status: String? = null
            for (l in lines) {
                if (l.isBlank()) continue
                val m = try { Json.obj(Json.parse(l)) } catch (e: JsonException) { continue }   // a line cut by a crash
                when (m["op"]) {
                    "begin" -> begin = m
                    "end" -> { ended = true; status = Json.optStr(m, "status") }
                    else -> ops += m
                }
            }
            return Contents(f, f.name.removeSuffix(".jsonl"), begin, ops, ended, status)
        }
    }
}

/** A manifest-v2 file in memory: entries by lowercase dest, with an index by source identity. */
class Manifest(val kind: String) {
    private val byDest = LinkedHashMap<String, Tracked>()
    private val byIdent = HashMap<String, String>()
    var migratedV1: String? = null

    val files: Map<String, Tracked> get() = byDest
    val size get() = byDest.size

    private fun ident(t: Tracked) = if (t.link == null) null else t.link + "\u0000" + t.rel

    fun get(dest: String) = byDest[SyncRules.lower(dest)]

    fun has(link: String, rel: String) = byIdent.containsKey(link + "\u0000" + rel)

    /** Sets the entry at [t]'s dest; an entry of the same source at another dest goes. */
    fun put(t: Tracked) {
        val key = SyncRules.lower(t.dest)
        val id = ident(t)
        if (id != null) byIdent[id]?.let { if (it != key) remove(it) }
        remove(key)
        byDest[key] = t
        if (id != null) byIdent[id] = key
    }

    fun remove(dest: String): Tracked? {
        val key = SyncRules.lower(dest)
        val t = byDest.remove(key) ?: return null
        val id = ident(t)
        if (id != null && byIdent[id] == key) byIdent.remove(id)
        return t
    }

    /** Applies one journal operation (idempotent: replaying a journal twice gives the same manifest). */
    fun apply(op: Map<String, Any?>) {
        if (Json.optStr(op, "kind") != kind) return
        when (op["op"]) {
            "put", "record" -> {
                val dest = Json.optStr(op, "dest") ?: return
                val e = op["entry"] as? Map<*, *>
                if (e == null) { remove(dest); return }
                Json.optStr(op, "from")?.let { if (!it.equals(dest, true)) remove(it) }
                put(Tracked.fromMap(dest, Json.obj(e)))
            }
            "move" -> {
                val from = Json.optStr(op, "from") ?: return
                val to = Json.optStr(op, "to") ?: return
                val t = remove(from) ?: return
                put(t.copy(dest = to, rel = Json.optStr(op, "rel") ?: t.rel, cardMtime = Json.optLong(op, "card_mtime", t.cardMtime)))
            }
            "delete", "forget" -> Json.optStr(op, "dest")?.let { remove(it) }
        }
    }

    /** The links of this kind that it knows (for state fallbacks). */
    fun forLink(id: String) = files.values.filter { it.link == id }

    fun toMap(): Map<String, Any?> = linkedMapOf<String, Any?>("format" to 2, "kind" to kind, "updated" to System.currentTimeMillis() / 1000,
        "migrated_v1" to migratedV1, "files" to files.values.sortedBy { it.dest }.associate { it.dest to it.toMap() })

    companion object {
        fun fromMap(kind: String, m: Map<String, Any?>): Manifest {
            val r = Manifest(kind)
            r.migratedV1 = Json.optStr(m, "migrated_v1")
            for ((dest, v) in Json.obj(m["files"])) r.put(Tracked.fromMap(dest, Json.obj(v)))
            return r
        }

        /**
         * Folds the branch's v1 manifest (`ByOS/sync/manifest.json`, keyed by card path, with the
         * absolute source path) into [into]: each entry whose source lies in a music link's folder
         * gets that link and its relative path; the others keep their source path until a link
         * covers it. Entries already in v2 are left alone. Returns how many entries were added or
         * resolved.
         */
        fun migrateV1(into: Manifest, v1: Map<String, Any?>, links: List<Link>, clientId: String): Int {
            var n = 0
            fun resolve(src: String): Pair<Link, String>? {
                val s = src.replace('\\', '/')
                val l = links.filter { it.local.isNotEmpty() }.map { it to it.local.replace('\\', '/').trimEnd('/') }
                    .filter { (_, root) -> s.length > root.length && s.substring(0, root.length).equals(root, true) && s[root.length] == '/' }
                    .maxByOrNull { it.second.length } ?: return null
                return l.first to SyncRules.nfc(s.substring(l.second.length + 1))
            }
            val files = (v1["files"] as? Map<*, *>) ?: emptyMap<String, Any?>()
            val now = System.currentTimeMillis() / 1000
            for ((k, v) in files) {
                val dest = k.toString()
                val e = Json.obj(v)
                if (into.get(dest) != null) continue
                val src = Json.optStr(e, "src") ?: continue
                val r = resolve(src)
                if (r != null && into.has(r.first.id, r.second)) continue
                into.put(Tracked(dest, r?.first?.id, r?.second ?: "", clientId, Json.optLong(e, "src_size", -1),
                    Json.optLong(e, "src_mtime", -1), Json.optLong(e, "size", -1), null, 0, Json.optStr(e, "covers") ?: "keep", now,
                    srcAbs = if (r == null) src else null))
                n++
            }
            // entries of an earlier migration whose folder is now linked
            for (t in into.files.values.toList()) {
                if (t.link != null || t.srcAbs == null) continue
                val r = resolve(t.srcAbs) ?: continue
                if (into.has(r.first.id, r.second)) continue
                into.put(t.copy(link = r.first.id, rel = r.second, srcAbs = null))
                n++
            }
            return n
        }
    }
}

/**
 * lock.json on the card (section 3.5 step 1). [busyUntil]: while one long transfer runs the
 * holder cannot refresh [heartbeat], so it says beforehand how long the transfer may take.
 */
class LockInfo(val clientId: String, val name: String, val runId: String, val kinds: List<String>, val started: Long, val heartbeat: Long,
               val busyUntil: Long = 0) {
    fun toMap(): Map<String, Any?> = linkedMapOf<String, Any?>("client_id" to clientId, "name" to name, "run_id" to runId, "kinds" to kinds,
        "started" to started, "heartbeat" to heartbeat).apply { if (busyUntil > 0) put("busy_until", busyUntil) }

    /** The holder stopped refreshing it more than 10 minutes ago (and is not inside a long transfer it announced). */
    fun stale(now: Long = System.currentTimeMillis() / 1000) = now - maxOf(heartbeat, busyUntil) > STALE_S

    companion object {
        const val STALE_S = 600L
        fun fromMap(m: Map<String, Any?>) = LockInfo(Json.optStr(m, "client_id") ?: "", Json.optStr(m, "name") ?: "", Json.optStr(m, "run_id") ?: "",
            (m["kinds"] as? List<*>)?.map { it.toString() } ?: emptyList(), Json.optLong(m, "started", 0), Json.optLong(m, "heartbeat", 0),
            Json.optLong(m, "busy_until", 0))
    }
}

/**
 * The lock of the sync runs of one HiBy on this computer (an OS file lock, gone when its process
 * dies). The desktop app and byos-cli share the client id, so the card lock alone cannot tell a
 * second run of this computer from a run that was killed.
 */
class LocalRunLock private constructor(private val raf: java.io.RandomAccessFile, private val lock: java.nio.channels.FileLock) : AutoCloseable {
    override fun close() {
        try { lock.release() } catch (e: Exception) {}
        try { raf.close() } catch (e: Exception) {}
    }

    companion object {
        /** The lock, or null when another run (in this or another process) holds it. */
        fun tryAcquire(f: File): LocalRunLock? {
            f.absoluteFile.parentFile?.mkdirs()
            val raf = java.io.RandomAccessFile(f, "rw")
            val l = try { raf.channel.tryLock() } catch (e: java.nio.channels.OverlappingFileLockException) { null } catch (e: java.io.IOException) { null }
            if (l == null) { raf.close(); return null }
            return LocalRunLock(raf, l)
        }
    }
}
