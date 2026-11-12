package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.BtpPaths
import org.hibyos.install.core.btp.DataSink
import org.hibyos.install.core.btp.DfR
import org.hibyos.install.core.btp.ListEntry
import org.hibyos.install.core.btp.StatR
import org.hibyos.install.core.btp.StatusR
import org.hibyos.install.core.sync.ApplyOptions
import org.hibyos.install.core.sync.CardFs
import org.hibyos.install.core.sync.CardStore
import org.hibyos.install.core.sync.Decisions
import org.hibyos.install.core.sync.Kinds
import org.hibyos.install.core.sync.Link
import org.hibyos.install.core.sync.LinkSelection
import org.hibyos.install.core.sync.LinksFile
import org.hibyos.install.core.sync.LocalScan
import org.hibyos.install.core.sync.LocalScanner
import org.hibyos.install.core.sync.PlanItem
import org.hibyos.install.core.sync.PlanRequest
import org.hibyos.install.core.sync.PlayerOnly
import org.hibyos.install.core.sync.RunJournal
import org.hibyos.install.core.sync.SyncApply
import org.hibyos.install.core.sync.SyncException
import org.hibyos.install.core.sync.SyncListener
import org.hibyos.install.core.sync.SyncPlanning
import org.hibyos.install.core.sync.SyncRules
import org.hibyos.install.core.sync.SyncStore
import java.io.ByteArrayInputStream
import java.io.Closeable
import java.io.File
import java.io.FileInputStream
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

/**
 * The card over BTP for the sync engine. A dropped link is reconnected up to three times
 * (2, 4 and 8 s apart, as uploads do); after that the BtpConnectionException reaches the
 * engine, which pauses the run.
 */
class BtpCardFs(private val cli: Cli, private var conn: Conn?) : CardFs, Closeable {
    private val lib = LibraryCommands(cli)

    private fun <T> op(path: String?, f: (Conn) -> T): T {
        var attempt = 0
        while (true) {
            try {
                val c = conn ?: try { cli.connect(quiet = true) } catch (e: CliError) {
                    if (e.code == "no_device" || e.code == "timeout") throw BtpConnectionException(e.message ?: "no device") else throw e
                } catch (e: java.io.IOException) {
                    throw BtpConnectionException(e.message ?: "no device")
                } catch (e: BtpException) {
                    if (e.remote) throw e else throw BtpConnectionException(e.message ?: "no device")
                }.also { conn = it }
                return f(c)
            } catch (e: BtpConnectionException) {
                try { conn?.close() } catch (x: Exception) {}
                conn = null
                if (attempt >= cli.retryDelaysMs.size) throw e
                cli.out.warn("reconnecting", "connection lost; retrying in ${cli.retryDelaysMs[attempt] / 1000.0} s", mapOf("path" to path))
                Thread.sleep(cli.retryDelaysMs[attempt])
                attempt++
            }
        }
    }

    override fun list(dir: String, hidden: Boolean): List<ListEntry>? = op(dir) { c ->
        try { c.session.list(dir, hidden) } catch (e: BtpException) { if (e.remote && e.code == Btp.E_NOT_FOUND) null else throw e }
    }

    override fun stat(path: String): StatR? = op(path) { it.session.statOrNull(path) }

    override fun read(path: String, off: Long, len: Int): ByteArray = op(path) { lib.fetch(it, path, off, len) }

    override fun put(path: String, file: File, sha: ByteArray, mode: Int, mtime: Long, progress: (Long, Long) -> Unit): Int = op(path) { c ->
        c.session.put(path, file.length(), sha, mode, mtime, Btp.NS_SD, { cli.cancelled.get() }, { d, t -> progress(d, t) }) { off ->
            FileInputStream(file).also { s -> s.channel.position(off) }
        }.result
    }

    override fun putBytes(path: String, data: ByteArray, mode: Int) = op(path) { c ->
        val sha = MessageDigest.getInstance("SHA-256").digest(data)
        c.session.put(path, data.size.toLong(), sha, mode, System.currentTimeMillis() / 1000, Btp.NS_SD, { false }, null) { off ->
            ByteArrayInputStream(data, off.toInt(), data.size - off.toInt())
        }
        Unit
    }

    override fun mkdirs(dir: String) { if (dir.isNotEmpty()) op(dir) { it.session.mkdir(dir, true) } }

    override fun move(src: String, dst: String, replace: Boolean) = op(src) { it.session.move(src, dst, replace) }

    override fun delete(path: String, recursive: Boolean) = op(path) { it.session.delete(path, recursive) }

    override fun df(): DfR = op(null) { it.session.df() }

    override fun status(): StatusR? = op(null) { if (it.session.hasCap(Btp.CAP_STATUS)) it.session.status() else null }

    override fun get(path: String, dst: File, progress: (Long, Long) -> Unit) = op(path) { c ->
        val md = MessageDigest.getInstance("SHA-256")
        RandomAccessFile(dst, "rw").use { r ->
            r.setLength(0)
            val res = c.session.get(path, 0, true, Btp.NS_SD, { cli.cancelled.get() }, { d, t -> progress(d, t) }) {
                DataSink { b, o, n -> r.write(b, o, n); md.update(b, o, n) }
            }
            if (!md.digest().contentEquals(res.sha256)) throw BtpException(Btp.E_HASH_MISMATCH, "$path did not arrive intact", remote = true)
        }
    }

    override fun close() { try { conn?.close() } catch (e: Exception) {}; conn = null }
}

/**
 * Folder sync commands (docs/BYOS_CLI.md "Folder sync", docs/SYNC_LIBRARY_DESIGN.md
 * sections 3 and 9): links, scan, plan, apply, resume, status and unlock.
 */
class SyncCommands(private val cli: Cli) {
    private val a get() = cli.a
    private val out get() = cli.out
    private val lib by lazy { LibraryCommands(cli) }

    private val listener = object : SyncListener {
        override fun progress(op: String, path: String?, done: Long, total: Long, unit: String, extra: Map<String, Any?>) =
            out.progress(op, path, done, total, (extra["rate"] as? Long) ?: 0, unit, extra.filterKeys { it != "rate" })
        override fun warn(code: String, message: String, extra: Map<String, Any?>) = out.warn(code, message, extra)
        override fun cancelled() = cli.cancelled.get()
    }

    private fun deviceId(c: Conn) = c.session.device.deviceId?.let(Config::hexOf) ?: "unknown"

    /** The device the links belong to: --device-id, else the connected HiBy's id. */
    private fun deviceIdOffline(): String = a.opt("--device-id")?.lowercase()?.also {
        if (it.length < 4 || !it.all { c -> c.isDigit() || c in 'a'..'f' }) throw CliError("usage", "--device-id needs a device id (hex)")
    } ?: cli.withConn { deviceId(it) }

    private fun store(dev: String) = SyncStore(cli.config.dir, dev)

    private fun clientName(): String = a.opt("--name") ?: try { java.net.InetAddress.getLocalHost().hostName } catch (e: Exception) { "byos-cli" }

    internal fun syncError(e: SyncException): CliError = when (e.code) {
        "locked" -> CliError("locked", e.message ?: "locked", extra = e.extra)
        "cancelled" -> CliError("cancelled", "cancelled")
        "battery" -> CliError("remote", e.message ?: "battery low", Btp.E_BATTERY, Btp.errorName(Btp.E_BATTERY), extra = e.extra)
        "card_changed" -> CliError("verify", e.message ?: "card changed", extra = mapOf("reason" to "card_changed"))
        else -> CliError("usage", e.message ?: e.code, extra = e.extra)
    }

    // ------------------------------------------------------------------ links

    private fun linkFields(l: Link): Map<String, Any?> = l.toMap()

    private fun parseSelection(base: LinkSelection?): LinkSelection? {
        val sels = a.optAll("--select")
        val file = a.opt("--select-file")
        if (sels.isEmpty() && file == null && !a.has("--select-all")) return base
        if (a.has("--select-all")) return LinkSelection()
        var artists = ArrayList<String>(); var albums = ArrayList<Pair<String, String>>(); var genres = ArrayList<String>(); var files = ArrayList<String>()
        var all = sels.isEmpty() && file == null
        if (file != null) {
            val f = File(file)
            if (!f.isFile) throw CliError("local_io", "$file does not exist", path = file)
            val m = try { Json.obj(Json.parse(f.readText())) } catch (e: JsonException) { throw CliError("local_io", "$file is not valid JSON") }
            val s = LinkSelection.fromMap(m)
            all = s.all; artists += s.artists; albums += s.albums; genres += s.genres; files += s.files
        }
        for (s in sels) {
            val k = s.substringBefore(':'); val v = s.substringAfter(':', "")
            if (v.isEmpty() && s != "all") throw CliError("usage", "--select takes all, artist:NAME, album:ARTIST::TITLE, genre:NAME or file:PATH")
            all = false
            when (k) {
                "all" -> all = true
                "artist", "artists" -> artists += v
                "album", "albums" -> { if (!v.contains("::")) throw CliError("usage", "--select album:ARTIST::TITLE"); albums += v.substringBefore("::") to v.substringAfter("::") }
                "genre", "genres" -> genres += v
                "file", "files" -> files += SyncRules.nfc(v.replace('\\', '/').trim('/'))
                else -> throw CliError("usage", "--select takes all, artist:NAME, album:ARTIST::TITLE, genre:NAME or file:PATH")
            }
        }
        if (sels.contains("all")) all = true
        return LinkSelection(all, artists, albums, genres, files)
    }

    private fun checkRoot(root: String, lf: LinksFile, self: String?): String {
        val r = cli.remotePath(root)
        if (SyncRules.alwaysExcludedOnCard(r) || r.equals("Music/Free", true)) throw CliError("usage", "$r is reserved on the HiBy (ByOS, Playlists, Music/Free)")
        val clash = lf.links.firstOrNull { it.id != self && it.cardRoot.equals(r, true) }
        if (clash != null) throw CliError("usage", "link ${clash.id} already syncs into $r on the HiBy; give another --card-root")
        return r
    }

    private fun applyOptions(l: Link, lf: LinksFile) {
        a.opt("--layout")?.let {
            if (it !in Link.LAYOUTS) throw CliError("usage", "--layout is artist-album or keep")
            if (it == "artist-album" && l.kind != Kinds.MUSIC) throw CliError("usage", "the artist-album layout is for music links")
            l.layout = it
        }
        a.opt("--player-only")?.let { if (it !in Link.POLICIES) throw CliError("usage", "--player-only is keep, delete or ask"); l.playerOnly = it }
        a.opt("--covers")?.let { if (it !in setOf("optimize", "keep")) throw CliError("usage", "--covers is optimize or keep"); l.covers = it }
        a.opt("--label")?.let { l.label = it }
        if (a.has("--remove-unselected")) l.removeUnselected = true
        if (a.has("--keep-unselected")) l.removeUnselected = false
        if (a.has("--no-sidecars")) l.includeSidecars = false
        if (a.has("--sidecars")) l.includeSidecars = true
        if (a.has("--disable")) l.enabled = false
        if (a.has("--enable")) l.enabled = true
        if (a.has("--clear-exclude")) l.exclude = emptyList()
        val ex = a.optAll("--exclude")
        if (ex.isNotEmpty()) l.exclude = (l.exclude + ex).distinct()
        parseSelection(l.selection)?.let {
            if (!it.all && l.kind != Kinds.MUSIC) throw CliError("usage", "choosing artists, albums or genres is for music links")
            l.selection = it
        }
        a.opt("--card-root")?.let { l.cardRoot = checkRoot(it, lf, l.id) }
    }

    fun linkLs(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "link-ls takes no arguments")
        val dev = deviceIdOffline()
        val lf = store(dev).loadLinks(clientName())
        for (l in lf.links) out.event("entry", linkFields(l))
        return linkedMapOf("device_id" to dev, "client_id" to lf.clientId, "count" to lf.links.size, "links" to lf.links.map { linkFields(it) })
    }

    fun linkAdd(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "link-add needs --kind KIND LOCAL_FOLDER")
        val kind = try { Kinds.check(a.opt("--kind") ?: throw CliError("usage", "link-add needs --kind music|audiobooks|ebooks|video")) } catch (e: SyncException) { throw syncError(e) }
        val dir = File(a.pos[0]).absoluteFile
        if (!dir.isDirectory) throw CliError("local_io", "${dir.path} is not a folder", path = dir.path)
        val dev = deviceIdOffline()
        val st = store(dev)
        val lf = st.loadLinks(clientName())
        if (lf.links.any { File(it.local).absoluteFile == dir && it.kind == kind }) throw CliError("usage", "${dir.path} is already linked for $kind")
        val label = a.opt("--label") ?: dir.name.ifEmpty { kind }
        var root = Kinds.defaultRoot(kind)
        if (a.opt("--card-root") == null && lf.links.any { it.cardRoot.equals(root, true) }) root = BtpPaths.join(root, SyncRules.windowsSafe(org.hibyos.install.core.library.SyncPlanner.cleanName(label, kind)))
        val l = Link(SyncStore.newId() + SyncStore.newId(), kind, dir.path, checkRoot(a.opt("--card-root") ?: root, lf, null),
            if (kind == Kinds.MUSIC) "artist-album" else "keep", label = label, volumeId = LocalScanner.volumeId(dir))
        applyOptions(l, lf)
        lf.links += l
        st.saveLinks(lf)
        return linkedMapOf("device_id" to dev, "link" to linkFields(l))
    }

    fun linkSet(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "link-set needs LINK_ID")
        val dev = deviceIdOffline()
        val st = store(dev)
        val lf = st.loadLinks(clientName())
        val l = lf.find(a.pos[0]) ?: throw CliError("usage", "no link ${a.pos[0]} (link-ls lists them)")
        a.opt("--local")?.let {
            val dir = File(it).absoluteFile
            if (!dir.isDirectory) throw CliError("local_io", "${dir.path} is not a folder", path = dir.path)
            // the same files in another place (another drive): the identity (link, path inside) stays
            l.local = dir.path
            l.volumeId = LocalScanner.volumeId(dir)
        }
        applyOptions(l, lf)
        st.saveLinks(lf)
        return linkedMapOf("device_id" to dev, "link" to linkFields(l))
    }

    fun linkRm(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "link-rm needs LINK_ID")
        val dev = deviceIdOffline()
        val st = store(dev)
        val lf = st.loadLinks(clientName())
        val l = lf.find(a.pos[0]) ?: throw CliError("usage", "no link ${a.pos[0]} (link-ls lists them)")
        lf.links.remove(l)
        st.saveLinks(lf)
        st.linkDir(l.id).deleteRecursively()
        return linkedMapOf("device_id" to dev, "removed" to l.id, "count" to lf.links.size)
    }

    // ------------------------------------------------------------------ scan

    private fun kinds(): List<String> = try { Kinds.parse(a.opt("--kind")) } catch (e: SyncException) { throw syncError(e) }

    private fun scanLinks(st: SyncStore, links: List<Link>, full: Boolean): Map<String, LocalScan> {
        val res = LinkedHashMap<String, LocalScan>()
        for (l in links) {
            val s = try {
                LocalScanner.scan(l, st.loadScan(l.id), full, { cli.cancelled.get() },
                    { op, p, d, t -> out.progress(op, p, d, t, unit = if (op == "scan") "folders" else "files") })
            } catch (e: SyncException) { throw syncError(e) }
            st.saveScan(s)
            if (s.status != "ok") out.warn("source_unavailable", "${l.local} is not available (${s.status}). Nothing will be deleted.",
                mapOf("link" to l.id, "status" to s.status))
            if (s.unreadableDirs.isNotEmpty()) out.warn("unreadable", "${s.unreadableDirs.size} folders under ${l.local} could not be read; their files are kept on the HiBy",
                mapOf("link" to l.id, "folders" to s.unreadableDirs.take(20)))
            res[l.id] = s
        }
        return res
    }

    fun syncScan(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "sync-scan takes no arguments")
        val ks = kinds()
        val dev = deviceIdOffline()
        val st = store(dev)
        val lf = st.loadLinks(clientName())
        val links = lf.links.filter { it.kind in ks && it.enabled }
        if (links.isEmpty()) throw CliError("usage", "no enabled link for ${ks.joinToString()}; add one with link-add")
        val scans = scanLinks(st, links, a.has("--full"))
        return linkedMapOf("device_id" to dev, "links" to links.map { l ->
            val s = scans[l.id]!!
            linkedMapOf("id" to l.id, "kind" to l.kind, "status" to s.status, "files" to s.entries.size, "unreadable" to s.unreadableDirs.size,
                "errors" to s.errors, "bytes" to s.entries.values.sumOf { it.size })
        })
    }

    // ------------------------------------------------------------------ plan

    fun syncPlan(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "sync-plan takes no arguments")
        val ks = kinds()
        return cli.withConn { c ->
            val dev = deviceId(c)
            val st = store(dev)
            val lf = st.loadLinks(clientName())
            val links = lf.links.filter { it.kind in ks && it.enabled }
            if (links.isEmpty()) throw CliError("usage", "no enabled link for ${ks.joinToString()}; add one with link-add")
            val scans = if (a.has("--no-scan")) links.associate { l ->
                l.id to (st.loadScan(l.id) ?: throw CliError("usage", "link ${l.id} has not been scanned yet (sync-scan)"))
            } else scanLinks(st, links, false)
            val fs = BtpCardFs(cli, c)
            val plan = try {
                SyncPlanning.plan(fs, st, lf, scans, PlanRequest(ks, a.has("--delete-untracked"), deviceId = dev), listener)
            } catch (e: SyncException) { throw syncError(e) }
            val outF = a.opt("--out")?.let { File(it) } ?: File(cli.config.cache, "sync" + File.separator + "plan-v2.json")
            outF.absoluteFile.parentFile?.mkdirs()
            cli.writeJson(outF, plan)
            for (l in Json.arr(plan["links"]).map { Json.obj(it) }) {
                val g = Json.obj(l["guard"])
                if (g["tripped"] == true) out.warn(if (g["reason"] == "mass_delete") "mass_delete_blocked" else "source_unavailable",
                    if (g["reason"] == "mass_delete") "Many files would be removed from the HiBy; none is ticked. Check that the folder is complete."
                    else "Your ${l["kind"]} folder looks unavailable (${g["reason"]}). Nothing will be deleted.", mapOf("link" to l["id"], "reason" to g["reason"]))
            }
            linkedMapOf("plan" to outF.path, "run_id" to plan["run_id"], "kinds" to plan["kinds"], "fits" to plan["fits"],
                "sd_free" to plan["sd_free"], "items" to Json.arr(plan["items"]).size, "summary" to plan["summary"],
                "size" to outF.length())
        }
    }

    // ------------------------------------------------------------------ apply

    private fun prepareCovers(src: File, item: PlanItem, outFile: File): Boolean {
        val work = File(outFile.parentFile, "work-" + ProcessHandle.current().pid() + "-" + System.nanoTime())
        try {
            val f = lib.prepareCover(src, work)
            if (f == src) return false
            Files.move(f.toPath(), outFile.toPath(), StandardCopyOption.REPLACE_EXISTING)
            return true
        } finally { work.deleteRecursively() }
    }

    private fun runApply(plan: Map<String, Any?>, choices: Map<String, String>, dev: String, c: Conn): Map<String, Any?> {
        val st = store(dev)
        val lf = st.loadLinks(clientName())
        val fs = BtpCardFs(cli, c)
        val then = a.opt("--then") ?: "none"
        if (then !in setOf("none", "rescan", "rebuild")) throw CliError("usage", "--then is none, rescan or rebuild")
        val prepDir = File(cli.config.cache, "sync" + File.separator + "prepared")
        // prepared copies left by runs older than two weeks
        prepDir.listFiles()?.filter { System.currentTimeMillis() - it.lastModified() > 14L * 86400000 }?.forEach { it.deleteRecursively() }
        val engine = SyncApply(fs, st, lf, ApplyOptions(clientName(), a.has("--take-over"), cli.checkpointEvery, prepare = ::prepareCovers,
            preparedDir = prepDir, onRecord = cli.syncHook), listener)
        val res = try { engine.run(plan, choices) } catch (e: SyncException) { throw syncError(e) } finally { fs.close() }
        if (res.status == "cancelled") throw CliError("cancelled", "cancelled; run sync-resume to go on", extra = mapOf("run_id" to res.runId, "resumable" to true))
        val outMap = LinkedHashMap(res.toMap())
        var library: Map<String, Any?> = linkedMapOf("action" to then, "ok" to (then == "none"))
        if (then != "none" && (res.status == "done" || res.status == "done_with_problems")) {
            library = cli.withConn(quiet = true) { lib.libraryStep(it, then, res.written) }
        }
        outMap["library"] = library
        return outMap
    }

    fun syncApply(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "sync-apply needs PLAN.json")
        val planF = File(a.pos[0])
        if (!planF.isFile) throw CliError("local_io", "${planF.path} does not exist", path = planF.path)
        val plan = try { Json.obj(Json.parse(planF.readText())) } catch (e: JsonException) { throw CliError("local_io", "${planF.path} is not valid JSON") }
        val items = Json.arr(plan["items"]).map { PlanItem.fromMap(Json.obj(it)) }
        val dec = a.opt("--decisions")?.let { p ->
            val f = File(p)
            if (!f.isFile) throw CliError("local_io", "$p does not exist", path = p)
            try { Json.obj(Json.parse(f.readText())) } catch (e: JsonException) { throw CliError("local_io", "$p is not valid JSON") }
        } ?: emptyMap()
        val choices = try { HashMap(Decisions.resolve(items, dec)) } catch (e: SyncException) { throw syncError(e) }
        val byId = items.associateBy { it.id }
        // deletions: guarded ones need --allow-mass-delete (or the typed count), all need a yes
        val guarded = items.filter { it.parent == null && choices[it.id] == "delete" && it.extra["guard"] == true }
        if (guarded.isNotEmpty()) {
            val ok = when {
                a.has("--allow-mass-delete") && a.has("--yes") -> true
                a.has("--yes") -> false
                else -> {
                    val ans = cli.ask("confirm_mass_delete", "Type ${guarded.size} to delete ${guarded.size} files from the HiBy although the folder looks incomplete",
                        linkedMapOf("count" to guarded.size, "bytes" to guarded.sumOf { it.size }))
                    ans?.toString()?.trim() == guarded.size.toString()
                }
            }
            if (!ok) {
                out.warn("mass_delete_blocked", "${guarded.size} deletions were not confirmed; those files stay on the HiBy", mapOf("count" to guarded.size))
                for (g in guarded) choices[g.id] = "keep"
                for (i in items) if (i.parent != null) choices[i.id] = choices[i.parent] ?: "keep"
            }
        }
        val deletes = items.filter { it.parent == null && choices[it.id] == "delete" }
        if (deletes.isNotEmpty() && !a.has("--yes")) {
            val by = deletes.groupingBy { it.reason }.eachCount()
            val unselected = by[PlayerOnly.EXCLUDED] ?: 0
            val msg = "Delete ${deletes.size} files from the HiBy" + (if (unselected > 0) " ($unselected no longer selected)" else "") + "?"
            if (!cli.confirm(msg, linkedMapOf("count" to deletes.size, "bytes" to deletes.sumOf { it.size }, "reasons" to by))) {
                throw CliError("cancelled", "nothing was copied or deleted")
            }
        }
        if (byId.isEmpty() && Json.arr(plan["items"]).isEmpty()) out.debug("empty plan")
        return cli.withConn { c ->
            val dev = deviceId(c)
            val planDev = Json.optStr(plan, "device_id")
            if (!planDev.isNullOrEmpty() && planDev != dev) throw CliError("usage", "this plan was made for another HiBy ($planDev)")
            runApply(plan, choices, dev, c)
        }
    }

    fun syncResume(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "sync-resume takes no arguments")
        return cli.withConn { c ->
            val dev = deviceId(c)
            val st = store(dev)
            val run = st.unfinishedRuns().lastOrNull { st.runPlanFile(it.runId).isFile }
                ?: return@withConn linkedMapOf<String, Any?>("resumed" to false, "status" to "nothing_to_resume")
            val plan = SyncStore.readJson(st.runPlanFile(run.runId)) ?: throw CliError("local_io", "the plan of run ${run.runId} cannot be read")
            val choices = SyncStore.readJson(st.runDecisionsFile(run.runId))?.mapValues { it.value.toString() } ?: emptyMap()
            val r = runApply(plan, choices, dev, c)
            LinkedHashMap(r).apply { put("resumed", true) }
        }
    }

    // ------------------------------------------------------------------ status / unlock

    fun syncStatus(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "sync-status takes no arguments")
        var conn: Conn? = null
        val dev = a.opt("--device-id")?.lowercase() ?: try {
            cli.connect(quiet = true).also { conn = it }.let { deviceId(it) }
        } catch (e: CliError) { if (e.code == "no_device" || e.code == "timeout") throw CliError(e.code, "${e.message}; or give --device-id") else throw e }
        try {
            val st = store(dev)
            val lf = st.loadLinks(clientName())
            val runs = st.unfinishedRuns().map { r ->
                linkedMapOf("run_id" to r.runId, "kinds" to (r.begin?.get("kinds")), "started" to r.begin?.get("time"), "done" to r.doneItems.size,
                    "status" to (r.pausedStatus ?: "interrupted"), "resumable" to st.runPlanFile(r.runId).isFile)
            }
            val links = lf.links.map { l ->
                val s = st.loadState(l.id)
                linkedMapOf("id" to l.id, "kind" to l.kind, "label" to l.label, "local" to l.local, "card_root" to l.cardRoot, "enabled" to l.enabled,
                    "tracked" to (s?.files?.size ?: 0), "last_sync" to st.stateFile(l.id).takeIf { it.isFile }?.lastModified()?.div(1000),
                    "scanned" to st.localFile(l.id).takeIf { it.isFile }?.lastModified()?.div(1000))
            }
            val res = linkedMapOf<String, Any?>("device_id" to dev, "client_id" to lf.clientId, "links" to links, "runs" to runs)
            if (conn == null && a.opt("--device-id") == null) conn = cli.connect(quiet = true)
            val c = conn
            if (c != null) {
                val fs = BtpCardFs(cli, c)
                val lock = CardStore.readLock(fs)
                res["card_id"] = CardStore.cardId(fs)
                res["lock"] = lock?.let { linkedMapOf<String, Any?>("held" to true, "ours" to (it.clientId == lf.clientId), "stale" to it.stale()).apply { putAll(it.toMap()) } }
                    ?: linkedMapOf("held" to false)
                res["card_journals"] = CardStore.journals(fs).size
            }
            return res
        } finally { try { conn?.close() } catch (e: Exception) {} }
    }

    fun syncUnlock(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "sync-unlock takes no arguments")
        return cli.withConn { c ->
            val lf = store(deviceId(c)).loadLinks(clientName())
            val fs = BtpCardFs(cli, c)
            val lock = CardStore.readLock(fs) ?: return@withConn linkedMapOf<String, Any?>("held" to false, "unlocked" to false)
            val ours = lock.clientId == lf.clientId
            if (!ours && !lock.stale() && !a.has("--force")) {
                throw CliError("locked", "The HiBy is being synced from ${lock.name}; add --force to remove its lock", extra = mapOf("locked_by" to lock.toMap()))
            }
            fs.delete(CardStore.LOCK)
            linkedMapOf<String, Any?>("held" to true, "unlocked" to true, "ours" to ours, "stale" to lock.stale(), "locked_by" to lock.toMap())
        }
    }

}
