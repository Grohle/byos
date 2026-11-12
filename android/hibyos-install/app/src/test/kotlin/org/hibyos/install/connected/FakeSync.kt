package org.hibyos.install.connected

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest

/**
 * An in-memory player for the tests: a card with folders and files, the PUT rules of section 11
 * (modes, hidden part files, resume at a 64 KiB boundary) and a pairing with a fixed code.
 */
class FakeCard {
    val files = LinkedHashMap<String, ByteArray>()
    val folders = LinkedHashSet<String>()

    /** Part files: key = path + sha + size, as the part file name of section 11.2. */
    val parts = HashMap<String, ByteArray>()
    var free: Long = 1L shl 30
    var fat = true

    /** Break the connection once after this many bytes of a PUT (-1: never). */
    var dropAfter = -1L
    var puts = 0
    val resumedAt = ArrayList<Long>()
    var pings = 0

    fun exists(p: String) = files.containsKey(p.lowercase().let { k -> files.keys.firstOrNull { it.lowercase() == k } ?: p }) ||
        folders.any { it.equals(p, ignoreCase = true) }

    fun fileKey(p: String): String? = files.keys.firstOrNull { it.equals(p, ignoreCase = true) }

    var gets = 0

    // ---- LIB_RESCAN (section 13.3) ----
    /** Each request: null for a full rescan, else the paths. */
    val rescans = ArrayList<List<String>?>()
    var fullRescan = true
    var pathRescan = true

    /** What the library holds after a full rescan (the test's readLibrary gives it back). */
    var onRescan: (() -> Unit)? = null

    // ---- NS_UPDATE and the install (sections 14.1 to 14.3) ----
    val update = LinkedHashMap<String, ByteArray>()
    var byosVersion = "0.9.0"
    var updateKeys: List<String> = emptyList()
    var staged: String? = null
    var stages = 0
    var installs = 0
    var installUnsupported = false

    /** What UPDATE_STATUS answers after UPDATE_INSTALL, one per request (the last repeats); null drops the connection. */
    val statusScript = ArrayList<UpdateProgress?>()

    /**
     * A PUT that resumes from a part fails once with this device error, and the part is dropped
     * (a part that the daemon's stop left inconsistent). 0: never.
     */
    var failResumeWith = 0

    /** The next PUT fails once with this device error (no connection drop before it). 0: never. */
    var failNextPutWith = 0

    /**
     * The next LIST loses its connection and the session connects again by itself (as the real
     * session does): the listener hears attempt 1, this runs, then the session's cancel is asked.
     */
    var duringReconnect: (() -> Unit)? = null
}

fun sha256(b: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(b)

class FakeSession(private val card: FakeCard, override val info: SyncDeviceInfo) : SyncSession {
    var closed = false

    /**
     * A PUT was left without PUT_END or CANCEL (the phone's file failed to read). Like hibysync,
     * which runs requests one at a time, this session then answers nothing more: the next request
     * waits behind the abandoned upload until the device's 30 s limit ends the session.
     */
    var stalled = false

    private fun live() {
        if (closed) throw SyncException(SyncException.CONNECTION_LOST, "closed")
        if (stalled) {
            closed = true
            throw SyncException(SyncException.E_TIMEOUT, "the device timed out the abandoned upload")
        }
    }

    private fun checkPath(p: String, root: Boolean = false) {
        if (SyncPaths.check(p, root) != null) throw SyncException(SyncException.E_BAD_PATH, p)
    }

    private fun parentOk(p: String) {
        val parent = SyncPaths.parent(p)
        if (parent.isNotEmpty() && card.folders.none { it.equals(parent, ignoreCase = true) }) throw SyncException(SyncException.E_NOT_FOUND, parent)
    }

    override fun df(): SyncSpace {
        live()
        return SyncSpace(0, if (card.fat) 1 else 2, 32L shl 30, card.free, 1L shl 30, 100L shl 20)
    }

    private var reconnectListener: (Int, Int) -> Unit = { _, _ -> }
    private var abort: () -> Boolean = { false }

    override fun onReconnect(listener: (attempt: Int, of: Int) -> Unit) {
        reconnectListener = listener
    }

    override fun cancelWhen(cancelled: () -> Boolean) {
        abort = cancelled
    }

    override fun list(path: String): List<SyncEntry> {
        live()
        card.duringReconnect?.let { hook ->
            card.duringReconnect = null
            reconnectListener(1, 4)
            hook()
            val stop = abort()
            reconnectListener(0, 0)
            if (stop) throw SyncException(SyncException.E_CANCELLED, "cancelled")
        }
        checkPath(path, true)
        if (path.isNotEmpty() && card.folders.none { it == path }) {
            if (card.fileKey(path) != null) throw SyncException(SyncException.E_NOT_DIR) else throw SyncException(SyncException.E_NOT_FOUND)
        }
        val out = ArrayList<SyncEntry>()
        for (d in card.folders) if (SyncPaths.parent(d) == path) out += SyncEntry(SyncPaths.name(d), 1, 0, 0)
        for ((k, v) in card.files) if (SyncPaths.parent(k) == path) out += SyncEntry(SyncPaths.name(k), 0, v.size.toLong(), 0)
        return out.shuffled()
    }

    override fun stat(path: String): SyncEntry? {
        live()
        card.fileKey(path)?.let { return SyncEntry(SyncPaths.name(it), 0, card.files[it]!!.size.toLong(), 0) }
        card.folders.firstOrNull { it.equals(path, ignoreCase = true) }?.let { return SyncEntry(SyncPaths.name(it), 1, 0, 0) }
        return null
    }

    override fun mkdir(path: String, parents: Boolean) {
        live()
        checkPath(path)
        if (card.fileKey(path) != null) throw SyncException(SyncException.E_EXISTS)
        if (card.folders.any { it.equals(path, ignoreCase = true) }) {
            if (parents) return else throw SyncException(SyncException.E_EXISTS)
        }
        if (parents) {
            var acc = ""
            for (c in path.split('/')) {
                acc = SyncPaths.join(acc, c)
                if (card.folders.none { it.equals(acc, ignoreCase = true) }) card.folders += acc
            }
        } else {
            parentOk(path)
            card.folders += path
        }
    }

    override fun move(src: String, dst: String, replace: Boolean) {
        live()
        checkPath(src)
        checkPath(dst)
        if (stat(dst) != null && !(replace || src.equals(dst, ignoreCase = true))) throw SyncException(SyncException.E_EXISTS)
        parentOk(dst)
        val f = card.fileKey(src)
        if (f != null) {
            card.files[dst] = card.files.remove(f)!!
            return
        }
        if (card.folders.none { it == src }) throw SyncException(SyncException.E_NOT_FOUND)
        if (dst.startsWith("$src/")) throw SyncException(SyncException.E_BAD_PATH)
        val fs = card.folders.toList()
        card.folders.clear()
        for (d in fs) card.folders += if (d == src) dst else if (d.startsWith("$src/")) dst + d.substring(src.length) else d
        for (k in card.files.keys.toList()) if (k.startsWith("$src/")) card.files[dst + k.substring(src.length)] = card.files.remove(k)!!
    }

    override fun delete(path: String, recursive: Boolean) {
        live()
        checkPath(path)
        val f = card.fileKey(path)
        if (f != null) {
            card.files.remove(f)
            return
        }
        if (card.folders.none { it == path }) throw SyncException(SyncException.E_NOT_FOUND)
        val inside = card.folders.any { it.startsWith("$path/") } || card.files.keys.any { it.startsWith("$path/") }
        if (inside && !recursive) throw SyncException(SyncException.E_NOT_EMPTY)
        card.folders.removeAll { it == path || it.startsWith("$path/") }
        card.files.keys.removeAll { it.startsWith("$path/") }
    }

    override fun put(ns: Int, path: String, size: Long, sha256: ByteArray, mtime: Long, mode: PutMode,
                     open: (offset: Long) -> InputStream, cancelled: () -> Boolean, progress: (done: Long) -> Unit): PutResult {
        live()
        checkPath(path)
        card.puts++
        if (ns == Ns.UPDATE) {
            if (path != "hibyos-update.json" && !(path.startsWith("hibyos-payload-") && path.endsWith(".tar"))) throw SyncException(SyncException.E_BAD_PATH)
            if (mode != PutMode.REPLACE) throw SyncException(SyncException.E_NOT_ALLOWED)
            val data = open(0).use { it.readBytes() }
            if (data.size.toLong() != size || !sha256(data).contentEquals(sha256)) throw SyncException(SyncException.E_HASH_MISMATCH)
            progress(size)
            card.update[path] = data
            return PutResult(false, size)
        }
        if (card.fat && size > 0xFFFFFFFFL) throw SyncException(SyncException.E_FILE_TOO_BIG)
        parentOk(path)
        val existing = card.fileKey(path)
        if (existing != null) {
            val same = sha256(card.files[existing]!!).contentEquals(sha256)
            when (mode) {
                PutMode.CREATE -> throw SyncException(SyncException.E_EXISTS)
                PutMode.SKIP_IDENTICAL -> if (same && card.files[existing]!!.size.toLong() == size) return PutResult(true, size)
                else throw SyncException(SyncException.E_EXISTS)
                PutMode.REPLACE -> {}
            }
        }
        val key = path + "|" + sha256.joinToString("") { "%02x".format(it) } + "|" + size
        if (card.failNextPutWith != 0) {
            val code = card.failNextPutWith
            card.failNextPutWith = 0
            throw SyncException(code, "card error")
        }
        if (card.failResumeWith != 0 && (card.parts[key]?.size ?: 0) >= 65536) {
            val code = card.failResumeWith
            card.failResumeWith = 0
            card.parts.remove(key)
            throw SyncException(code, "the kept part does not match")
        }
        val part = card.parts[key] ?: ByteArray(0)
        val offset = minOf(part.size / 65536 * 65536L, size)
        if (size - offset + (1 shl 20) > card.free) throw SyncException(SyncException.E_NO_SPACE)
        if (offset > 0) card.resumedAt += offset
        val buf = ByteArrayOutputStream()
        buf.write(part, 0, offset.toInt())
        try {
            put(offset, key, size, buf, open, cancelled, progress)
        } catch (e: SyncException) {
            throw e
        } catch (e: Exception) {
            // the client gave up without CANCEL: the stream stays open on the device
            stalled = true
            throw e
        }
        val data = buf.toByteArray()
        card.parts.remove(key)
        if (data.size.toLong() != size || !sha256(data).contentEquals(sha256)) throw SyncException(SyncException.E_HASH_MISMATCH)
        if (existing != null) card.files.remove(existing)
        card.files[path] = data
        card.free -= size
        return PutResult(false, size)
    }

    private fun put(offset: Long, key: String, size: Long, buf: ByteArrayOutputStream, open: (offset: Long) -> InputStream,
                    cancelled: () -> Boolean, progress: (done: Long) -> Unit) {
        open(offset).use { i ->
            val b = ByteArray(65536)
            while (true) {
                if (cancelled()) {
                    card.parts[key] = buf.toByteArray()
                    throw SyncException(SyncException.E_CANCELLED)
                }
                val r = i.read(b)
                if (r < 0) break
                buf.write(b, 0, r)
                progress(buf.size().toLong())
                if (card.dropAfter >= 0 && buf.size() >= card.dropAfter) {
                    card.dropAfter = -1
                    card.parts[key] = buf.toByteArray()
                    closed = true
                    throw SyncException(SyncException.CONNECTION_LOST, "dropped")
                }
            }
        }
    }

    override fun get(path: String, offset: Long, out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): GetResult {
        live()
        checkPath(path)
        card.gets++
        val k = card.fileKey(path) ?: throw SyncException(if (card.folders.any { it == path }) SyncException.E_IS_DIR else SyncException.E_NOT_FOUND)
        val data = card.files[k]!!
        if (offset > data.size) throw SyncException(SyncException.E_PROTO)
        out.write(data, offset.toInt(), data.size - offset.toInt())
        progress(data.size.toLong(), data.size.toLong())
        return GetResult(data.size.toLong(), sha256(data))
    }

    var snapshot: ByteArray = ByteArray(0)

    override fun dbSnapshot(out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): ByteArray {
        live()
        out.write(snapshot)
        progress(snapshot.size.toLong(), snapshot.size.toLong())
        return sha256(snapshot)
    }

    override fun libRescan(paths: List<String>?) {
        live()
        if (paths != null && (paths.isEmpty() || paths.size > 256)) throw SyncException(SyncException.E_PROTO)
        if (if (paths == null) !card.fullRescan else !card.pathRescan) throw SyncException(SyncException.E_UNSUPPORTED)
        card.rescans += paths
        if (paths == null) card.onRescan?.invoke()
    }

    override fun updateStage(allowDowngrade: Boolean): StagedUpdate {
        live()
        card.stages++
        val json = card.update["hibyos-update.json"] ?: throw SyncException(SyncException.E_NOT_STAGED)
        val pkg = try {
            org.hibyos.install.core.btp.UpdatePackage.parse(json, card.updateKeys)
        } catch (e: org.hibyos.install.core.btp.UpdateException) {
            throw SyncException(if (e.kind == org.hibyos.install.core.btp.UpdateException.Kind.SIGNATURE) SyncException.E_SIGNATURE else SyncException.E_UPDATE_INVALID)
        }
        val tar = card.update[pkg.payloadName] ?: throw SyncException(SyncException.E_NOT_STAGED)
        if (sha256(tar).joinToString("") { "%02x".format(it) } != pkg.payloadSha256) throw SyncException(SyncException.E_UPDATE_INVALID)
        fun parts(v: String) = v.split('.').map { it.toIntOrNull() ?: 0 }
        val cmp = parts(pkg.version).zip(parts(card.byosVersion)).map { it.first.compareTo(it.second) }.firstOrNull { it != 0 } ?: 0
        if (cmp <= 0 && !allowDowngrade) throw SyncException(SyncException.E_DOWNGRADE)
        card.update.remove("hibyos-update.json")
        card.staged = pkg.version
        val restart = when (pkg.restart) { "none" -> 0; "shell" -> 1; else -> 2 }
        return StagedUpdate(pkg.version, restart, pkg.needsFirmware, tar.size.toLong())
    }

    override fun updateInstall(allowDowngrade: Boolean) {
        live()
        if (card.staged == null) throw SyncException(SyncException.E_NOT_STAGED)
        if (card.installUnsupported) throw SyncException(SyncException.E_UNSUPPORTED)
        card.installs++
    }

    override fun updateStatus(): UpdateProgress {
        live()
        val next: UpdateProgress? = when {
            card.statusScript.size > 1 -> card.statusScript.removeAt(0)
            card.statusScript.size == 1 -> card.statusScript[0]
            else -> UpdateProgress(UpdateProgress.IDLE, 0, 0, "")
        }
        if (next == null) {
            closed = true
            throw SyncException(SyncException.CONNECTION_LOST, "the shell restarted")
        }
        return next
    }

    override fun ping() {
        live()
        card.pings++
    }

    override fun close() {
        closed = true
    }
}

class FakeConnector(val card: FakeCard = FakeCard(), val code: String = "815365") : SyncConnector {
    val pairedIds = LinkedHashMap<String, PairedDevice>()
    var sessions = ArrayList<FakeSession>()
    var pairingOpen = true
    var allow = true
    var pairings = 0
    var device = SyncDevice("192.168.1.20", 7340, "0123456789abcdef", "R3II_2025", pairingOpen = true, sdMounted = true, paired = false)

    private fun info() = SyncDeviceInfo(device.deviceId, device.name, "0.9.0", "ByOS 0.9.0", local = false, sdMounted = true,
        shellAttached = true, caps = 0x1ff)

    override fun discover(timeoutMs: Int, cancelled: () -> Boolean, onFound: (SyncDevice) -> Unit): List<SyncDevice> {
        if (down) return emptyList()
        val d = device.copy(pairingOpen = pairingOpen, paired = pairedIds.containsKey(device.deviceId))
        onFound(d)
        return listOf(d)
    }

    override fun paired(): List<PairedDevice> = pairedIds.values.toList()

    override fun forget(deviceId: String) {
        pairedIds.remove(deviceId)
    }

    /** Another device answers at the player's address (its HELLO carries this id). */
    var answeringId: String? = null

    /** The player refuses AUTH (it forgot this phone); the phone keeps its keys. */
    var forgotten = false
    var connects = 0

    /** The player is out of reach (Wi-Fi off): connections and discovery find nothing. */
    var down = false

    override fun connect(host: String, port: Int, expectedId: String?, cancelled: () -> Boolean): SyncSession {
        if (down || host != device.host) throw SyncException(SyncException.UNREACHABLE, "$host: no answer")
        connects++
        val id = answeringId ?: device.deviceId
        if (!expectedId.isNullOrEmpty() && expectedId != id) throw SyncException(SyncException.IDENTITY_CHANGED)
        if (!pairedIds.containsKey(id)) throw SyncException(SyncException.NOT_PAIRED)
        if (forgotten) throw SyncException(SyncException.FORGOTTEN)
        return FakeSession(card, info()).also { sessions += it }
    }

    override fun startPairing(host: String, port: Int, clientName: String): Pairing {
        if (host != device.host) throw SyncException(SyncException.UNREACHABLE)
        if (!pairingOpen) throw SyncException(SyncException.E_PAIR_CLOSED)
        pairings++
        return object : Pairing {
            override fun confirm(code: String, cancelled: () -> Boolean): SyncSession {
                if (code != this@FakeConnector.code) throw SyncException(SyncException.WRONG_CODE)
                if (!allow) throw SyncException(SyncException.E_PAIR_REJECTED)
                pairedIds[device.deviceId] = PairedDevice(device.deviceId, device.name, host)
                forgotten = false
                return FakeSession(card, info()).also { sessions += it }
            }

            override fun close() {}
        }
    }
}
