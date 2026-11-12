package org.hibyos.install.connected

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.hibyos.install.ErrorInfo
import org.hibyos.install.R
import org.hibyos.install.core.SendItem
import java.io.Closeable
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean

/** What the connected mode needs from Android (and what the tests replace). */
interface SyncEnv {
    fun connector(): SyncConnector?
    val clientName: String

    /** Where the library snapshot is kept. */
    val cacheDir: File

    /** Runs on the UI thread. */
    fun post(f: () -> Unit)

    /** Runs on the worker thread, one job at a time. */
    fun execute(f: () -> Unit)

    fun readLibrary(file: File): List<Track>
    fun pickFiles()
    fun pickFolder()

    /** The ByOS files built by the patch flow (the ByOS folder first, the firmware last), or null. */
    fun byosFiles(): List<SendItem>?

    /** Calls [f] every [periodMs] on a timer thread until the result is closed. Tests leave it off. */
    fun every(periodMs: Long, f: () -> Unit): Closeable = Closeable {}

    /** Monotonic milliseconds (tests move it by hand). */
    fun now(): Long = System.nanoTime() / 1_000_000
}

/**
 * The "ByOS connected" mode: a session with the player over BTP/1 (pairing, sending files, the
 * file browser, the library and ByOS updates). One job runs at a time on the worker; the
 * screens only read [state].
 */
class ConnectedModel(private val env: SyncEnv) : SyncActions {
    var state by mutableStateOf(SyncState())
        private set

    private val connector: SyncConnector? = env.connector()
    @Volatile
    private var session: SyncSession? = null
    private var pairing: Pairing? = null
    private var lastDevice: SyncDevice? = null

    /** The player of the open session, as it identified itself (used to reopen the session). */
    private var current: SyncDevice? = null

    @Volatile
    private var stop = false
    private val cancelled = { stop }

    /** Picked on the phone, waiting to be sent. */
    private var picked: List<LocalItem> = emptyList()
    private var sources: Map<Int, LocalItem> = emptyMap()
    private var folders: List<String> = emptyList()
    private val hashes = HashMap<Int, ByteArray>()

    /** When the open session last sent a PING (section 17: every 30 s while idle). */
    @Volatile
    private var lastPing = 0L
    private val pingQueued = AtomicBoolean(false)
    private val keepAlive: Closeable

    init {
        state = state.copy(available = connector != null, paired = safePaired())
        // the timer only queues a PING on the worker: it never runs while a request is in flight
        keepAlive = env.every(PING_MS) { queuePing() }
    }

    /** Queues one keep-alive on the worker (at most one waits at a time). */
    fun queuePing() {
        if (session != null && pingQueued.compareAndSet(false, true)) env.execute {
            pingQueued.set(false)
            try {
                keepSessionAlive()
            } catch (e: Exception) {
                // failed() already showed it
            }
        }
    }

    /** On the worker, between requests or between hash blocks: a PING when the session was quiet. */
    private fun keepSessionAlive() {
        val s = session ?: return
        val now = env.now()
        if (now - lastPing < PING_MS - 5_000) return
        lastPing = now
        try {
            s.ping()
        } catch (e: Exception) {
            if (session === s) failed(e)
            throw e
        }
    }

    private fun safePaired(): List<PairedDevice> = try {
        connector?.paired() ?: emptyList()
    } catch (e: Exception) {
        emptyList()
    }

    private fun ui(f: (SyncState) -> SyncState) = env.post { state = f(state) }

    private fun begin(): Boolean {
        if (state.busy) return false
        stop = false
        return true
    }

    private class Pace {
        private var last = 0L
        fun due(force: Boolean = false): Boolean {
            val now = System.nanoTime()
            if (!force && now - last < 80_000_000L) return false
            last = now
            return true
        }
    }

    // ---- errors -------------------------------------------------------------------------

    fun errorOf(e: Throwable): ErrorInfo {
        val d = e.message?.trim()?.takeIf { it.isNotEmpty() }
        if (e !is SyncException) {
            return if (e is IOException) ErrorInfo(R.string.sync_err_local, d ?: e.javaClass.simpleName, R.string.sync_hint_local)
            else ErrorInfo(R.string.sync_err_generic, d ?: e.javaClass.simpleName)
        }
        return when (e.code) {
            SyncException.UNREACHABLE -> ErrorInfo(R.string.sync_err_unreachable, d, R.string.sync_hint_unreachable)
            SyncException.CONNECTION_LOST, SyncException.E_TIMEOUT -> ErrorInfo(R.string.sync_err_lost, d, R.string.sync_hint_lost)
            SyncException.IDENTITY_CHANGED, SyncException.E_AUTH_FAILED -> ErrorInfo(R.string.sync_err_identity, d, R.string.sync_hint_identity)
            SyncException.FORGOTTEN -> ErrorInfo(R.string.sync_err_forgotten, d, R.string.sync_hint_forgotten)
            SyncException.NO_CLIENT -> ErrorInfo(R.string.sync_err_no_client, d, R.string.sync_hint_no_client)
            SyncException.E_PAIR_CLOSED -> ErrorInfo(R.string.sync_err_pair_closed, d, R.string.sync_hint_pair_closed)
            SyncException.E_PAIR_REJECTED -> ErrorInfo(R.string.sync_err_pair_rejected, d, R.string.sync_hint_pair_rejected)
            SyncException.E_BUSY -> ErrorInfo(R.string.sync_err_busy, d, R.string.sync_hint_busy)
            SyncException.E_RATE_LIMIT -> ErrorInfo(R.string.sync_err_rate, d, R.string.sync_hint_rate)
            SyncException.E_NO_SPACE -> ErrorInfo(R.string.sync_err_no_space, d, R.string.sync_hint_no_space)
            SyncException.E_SD_UNAVAILABLE -> ErrorInfo(R.string.sync_err_sd, d, R.string.sync_hint_sd)
            SyncException.E_EXISTS -> ErrorInfo(R.string.sync_err_exists, d)
            SyncException.E_BAD_PATH -> ErrorInfo(R.string.sync_err_bad_name, d)
            SyncException.E_FILE_TOO_BIG -> ErrorInfo(R.string.sync_err_too_big, d)
            SyncException.E_NOT_FOUND -> ErrorInfo(R.string.sync_err_not_found, d, R.string.sync_hint_refresh)
            SyncException.E_NOT_EMPTY -> ErrorInfo(R.string.sync_err_not_empty, d)
            SyncException.E_UNSUPPORTED, SyncException.E_VERSION, SyncException.E_NOT_ALLOWED ->
                ErrorInfo(R.string.sync_err_unsupported, d, R.string.sync_hint_unsupported)
            SyncException.E_IO, SyncException.E_HASH_MISMATCH -> ErrorInfo(R.string.sync_err_io, d, R.string.sync_hint_io)
            else -> ErrorInfo(R.string.sync_err_generic, d ?: "BTP error ${e.code}")
        }
    }

    /** After an error that ends the session: back to the start, keeping what was picked. */
    private fun dropSession() {
        try {
            session?.close()
        } catch (e: Exception) {
            // already gone
        }
        session = null
    }

    private fun lost(e: Throwable): Boolean = e is SyncException && e.fatal

    // ---- connection -----------------------------------------------------------------------

    override fun findDevices() {
        val c = connector ?: return
        if (!begin()) return
        state = state.copy(link = Link.Searching, error = null, found = emptyList())
        env.execute {
            try {
                val found = c.discover(2500, cancelled) { d ->
                    ui { it.copy(found = (it.found.filter { x -> x.deviceId != d.deviceId } + d)) }
                }
                val paired = safePaired()
                ui {
                    it.copy(link = Link.Idle, paired = paired, found = found.distinctBy { d -> d.deviceId },
                        error = if (found.isEmpty() && !stop) ErrorInfo(R.string.sync_err_none_found, null, R.string.sync_hint_none_found) else null)
                }
            } catch (e: Exception) {
                ui { it.copy(link = Link.Idle, error = errorOf(e)) }
            }
        }
    }

    override fun connectAddress(text: String) {
        val t = text.trim()
        val host = t.substringBefore(':')
        val port = if (t.contains(':')) t.substringAfter(':').toIntOrNull() else 7340
        if (host.isEmpty() || port == null || port !in 1..65535 || !host.all { it.isLetterOrDigit() || it == '.' || it == '-' }) {
            state = state.copy(error = ErrorInfo(R.string.sync_err_bad_address, t))
            return
        }
        val known = state.found.firstOrNull { it.host == host && it.port == port }
        connectTo(known ?: SyncDevice(host, port, "", host, pairingOpen = false, sdMounted = true, paired = false))
    }

    override fun connectTo(device: SyncDevice) {
        val c = connector ?: return
        if (!begin()) return
        dropSession()
        lastDevice = device
        state = state.copy(link = Link.Connecting(device), error = null)
        env.execute {
            try {
                // the device the user chose: another one answering at that address is refused
                val s = try {
                    c.connect(device.host, device.port, device.deviceId.ifEmpty { null })
                } catch (e: SyncException) {
                    when (e.code) {
                        SyncException.NOT_PAIRED -> null
                        // the player refused this phone's keys: pairing again is offered; the keys stay
                        // until a new pairing succeeds (the refusal is not authenticated)
                        SyncException.FORGOTTEN -> try {
                            beginPairing(c, device, false)
                            return@execute
                        } catch (e2: SyncException) {
                            throw if (e2.code == SyncException.E_PAIR_CLOSED) e else e2
                        }
                        else -> throw e
                    }
                }
                if (s != null) opened(device, s) else beginPairing(c, device, false)
            } catch (e: Exception) {
                ui { it.copy(link = Link.Idle, error = errorOf(e)) }
            }
        }
    }

    private fun beginPairing(c: SyncConnector, device: SyncDevice, wrong: Boolean) {
        val p = c.startPairing(device.host, device.port, env.clientName)
        pairing = p
        ui { it.copy(link = Link.Pairing(device, wrong = wrong)) }
    }

    private fun opened(device: SyncDevice, s: SyncSession) {
        session = s
        lastPing = env.now()
        val space = try {
            s.df()
        } catch (e: SyncException) {
            null
        }
        val paired = safePaired()
        val d = device.copy(name = s.info.name.ifEmpty { device.name }, deviceId = s.info.deviceId, paired = true)
        current = d
        lastDevice = d
        ui { it.copy(link = Link.Connected(d, s.info, space), paired = paired, error = null, browser = Browser()) }
    }

    override fun setCode(text: String) {
        val l = state.link as? Link.Pairing ?: return
        if (l.waiting) return
        state = state.copy(link = l.copy(code = text.filter { it.isDigit() }.take(6), wrong = false))
    }

    override fun confirmCode() {
        val c = connector ?: return
        val l = state.link as? Link.Pairing ?: return
        val p = pairing ?: return
        if (l.code.length != 6 || l.waiting) return
        stop = false
        state = state.copy(link = l.copy(waiting = true))
        env.execute {
            try {
                val s = p.confirm(l.code, cancelled)
                pairing = null
                opened(l.device, s)
            } catch (e: SyncException) {
                pairing = null
                if (e.code == SyncException.WRONG_CODE) {
                    // the handshake is over: a new one shows a new code on the player
                    try {
                        beginPairing(c, l.device, true)
                    } catch (e2: Exception) {
                        ui { it.copy(link = Link.Idle, error = errorOf(e2)) }
                    }
                } else if (e.code == SyncException.E_CANCELLED) {
                    ui { it.copy(link = Link.Idle) }
                } else {
                    ui { it.copy(link = Link.Idle, error = errorOf(e)) }
                }
            } catch (e: Exception) {
                pairing = null
                ui { it.copy(link = Link.Idle, error = errorOf(e)) }
            }
        }
    }

    override fun cancelPairing() {
        stop = true
        val p = pairing
        pairing = null
        if (state.link is Link.Pairing && !(state.link as Link.Pairing).waiting) state = state.copy(link = Link.Idle)
        env.execute {
            try {
                p?.close()
            } catch (e: Exception) {
                // nothing to keep
            }
        }
    }

    override fun forget(deviceId: String) {
        val c = connector ?: return
        if (state.busy) return
        env.execute {
            try {
                c.forget(deviceId)
            } catch (e: Exception) {
                // the list below shows what is left
            }
            val paired = safePaired()
            ui { it.copy(paired = paired, found = it.found.map { d -> if (d.deviceId == deviceId) d.copy(paired = false) else d }) }
        }
    }

    override fun disconnect() {
        if (state.busy) return
        val s = session
        session = null
        state = state.copy(link = Link.Idle, browser = Browser(), library = LibraryPhase.Idle, libraryNav = LibraryNav())
        env.execute {
            try {
                s?.close()
            } catch (e: Exception) {
                // already closed
            }
        }
    }

    override fun refreshSpace() {
        val s = session ?: return
        if (state.busy) return
        env.execute {
            try {
                val space = s.df()
                ui { st -> st.connected?.let { st.copy(link = it.copy(space = space)) } ?: st }
            } catch (e: Exception) {
                failed(e)
            }
        }
    }

    /** An error outside a transfer: show it, and end the session when it is gone. */
    private fun failed(e: Throwable) {
        val err = errorOf(e)
        if (lost(e)) {
            dropSession()
            ui { it.copy(link = Link.Idle, error = err) }
        } else {
            ui { it.copy(error = err) }
        }
    }

    /**
     * A new session with the same player (AUTH, no code). Used after a file on the phone could not be
     * read in the middle of a PUT: the device still waits for that upload and runs requests one at a
     * time, so the old session cannot carry the next request.
     */
    private fun reopen(): SyncSession {
        dropSession()
        val d = current ?: throw SyncException(SyncException.CONNECTION_LOST)
        val c = connector ?: throw SyncException(SyncException.NO_CLIENT)
        val s = c.connect(d.host, d.port, d.deviceId.ifEmpty { null })
        session = s
        lastPing = env.now()
        return s
    }

    /** [putSmart] on the open session; a read error of the phone's file replaces the session, then is rethrown. */
    private fun putLocal(ns: Int, path: String, size: Long, sha: ByteArray, mtime: Long, replace: Boolean,
                         open: (Long) -> InputStream, progress: (Long) -> Unit): PutResult {
        val s = session ?: throw SyncException(SyncException.CONNECTION_LOST)
        try {
            return putSmart(s, ns, path, size, sha, mtime, replace, open, progress)
        } catch (e: IOException) {
            reopen()
            throw e
        }
    }

    // ---- sending files ----------------------------------------------------------------------

    override fun setDestination(d: Destination) {
        if (state.transfer.running) return
        state = state.copy(dest = d, destPath = null)
        replan()
    }

    override fun setReplace(on: Boolean) {
        if (state.transfer.running) return
        state = state.copy(replace = on)
    }

    override fun pickFiles() {
        if (!state.transfer.running) env.pickFiles()
    }

    override fun pickFolder() {
        if (!state.transfer.running) env.pickFolder()
    }

    /** Adds what the user picked (read on the worker by [load], which may walk a folder). */
    fun addPicked(load: () -> List<LocalItem>) {
        if (state.transfer.running) return
        env.execute {
            try {
                val items = load()
                env.post {
                    // a finished transfer is replaced by the new choice
                    if (state.transfer.finished) clearPlan()
                    picked = picked + items
                    replan()
                }
            } catch (e: Exception) {
                ui { it.copy(transfer = it.transfer.copy(error = errorOf(e))) }
            }
        }
    }

    private fun clearPlan() {
        picked = emptyList()
        sources = emptyMap()
        folders = emptyList()
        hashes.clear()
        state = state.copy(transfer = Transfer())
    }

    private fun replan() {
        if (state.transfer.jobs.any { it.status != JobStatus.WAITING && it.status != JobStatus.BAD_NAME && it.status != JobStatus.TOO_BIG }) return
        val max = state.connected?.space?.maxFile ?: Long.MAX_VALUE
        val plan = SendPlan.build(picked, state.target, max)
        sources = plan.sources
        folders = plan.folders
        hashes.clear()
        state = state.copy(transfer = Transfer(jobs = plan.jobs))
    }

    override fun clearSend() {
        if (state.transfer.running) return
        clearPlan()
    }

    override fun startSend() {
        val s = session ?: return
        val t = state.transfer
        if (t.jobs.none { it.retryable }) return
        if (!begin()) return
        val target = state.target
        val replace = state.replace
        val todo = t.jobs.filter { it.retryable }
        val total = todo.sumOf { it.size * 2 - (if (hashes.containsKey(it.id)) it.size else 0) }
        state = state.copy(transfer = t.copy(running = true, done = 0, total = total, error = null,
            jobs = t.jobs.map { if (it.retryable) it.copy(status = JobStatus.WAITING, done = 0, error = 0) else it }))
        env.execute {
            var done = 0L
            val pace = Pace()
            fun mark(id: Int, f: (SendJob) -> SendJob) = ui { st ->
                st.copy(transfer = st.transfer.copy(jobs = st.transfer.jobs.map { if (it.id == id) f(it) else it }))
            }
            try {
                if (target.isNotEmpty()) s.mkdir(target, true)
                for (f in folders) s.mkdir(f, true)
                for (job in todo) {
                    if (stop) throw SyncException(SyncException.E_CANCELLED)
                    val src = sources[job.id] ?: continue
                    try {
                        val sha = hashes[job.id] ?: run {
                            mark(job.id) { it.copy(status = JobStatus.HASHING) }
                            val base = done
                            val h = hash(src.open) { n ->
                                if (pace.due()) ui { st -> st.copy(transfer = st.transfer.copy(done = base + n, current = job.remote, hashing = true)) }
                            }
                            if (h.second != job.size) throw IOException("${job.local}: the file changed while it was read")
                            hashes[job.id] = h.first
                            done += job.size
                            h.first
                        }
                        mark(job.id) { it.copy(status = JobStatus.SENDING) }
                        val base = done
                        val r = putLocal(Ns.SD, job.remote, job.size, sha, src.mtime, replace, { off -> openAt(src.open, off) }) { n ->
                            if (pace.due()) ui { st ->
                                st.copy(transfer = st.transfer.copy(done = base + n, current = job.remote, hashing = false,
                                    jobs = st.transfer.jobs.map { if (it.id == job.id) it.copy(done = n) else it }))
                            }
                        }
                        done = base + job.size
                        mark(job.id) { it.copy(status = if (r.skipped) JobStatus.SKIPPED else JobStatus.SENT, done = job.size) }
                    } catch (e: SyncException) {
                        when {
                            e.code == SyncException.E_CANCELLED -> throw e
                            e.fatal || e.code == SyncException.E_NO_SPACE || e.code == SyncException.E_SD_UNAVAILABLE ||
                                e.code == SyncException.CONNECTION_LOST -> {
                                mark(job.id) { it.copy(status = JobStatus.FAILED, error = e.code) }
                                throw e
                            }
                            e.code == SyncException.E_EXISTS -> mark(job.id) { it.copy(status = JobStatus.EXISTS, error = e.code) }
                            e.code == SyncException.E_FILE_TOO_BIG -> mark(job.id) { it.copy(status = JobStatus.TOO_BIG, error = e.code) }
                            e.code == SyncException.E_BAD_PATH -> mark(job.id) { it.copy(status = JobStatus.BAD_NAME, error = e.code) }
                            else -> {
                                // the file changed after it was hashed: the next try hashes it again
                                if (e.code == SyncException.E_HASH_MISMATCH) hashes.remove(job.id)
                                mark(job.id) { it.copy(status = JobStatus.FAILED, error = e.code) }
                            }
                        }
                    } catch (e: IOException) {
                        // this file could not be read on the phone (putLocal already opened a new
                        // session if a PUT was under way): the others go on
                        hashes.remove(job.id)
                        mark(job.id) { it.copy(status = JobStatus.FAILED, error = 0) }
                    }
                }
                ui { it.copy(transfer = it.transfer.copy(running = false, done = it.transfer.total, current = "")) }
                session?.let { refreshAfterWrite(it) }
            } catch (e: Exception) {
                val cancel = e is SyncException && e.code == SyncException.E_CANCELLED
                val err = if (cancel) null else errorOf(e)
                if (lost(e)) dropSession()
                ui {
                    it.copy(link = if (lost(e)) Link.Idle else it.link, transfer = it.transfer.copy(running = false, current = "", error = err,
                        jobs = it.transfer.jobs.map { j ->
                            if (j.status == JobStatus.HASHING || j.status == JobStatus.SENDING) j.copy(status = if (cancel) JobStatus.CANCELLED else JobStatus.FAILED)
                            else j
                        }))
                }
            }
        }
    }

    /** SKIP_IDENTICAL first; with [replace], a different file of the same name is then replaced. */
    private fun putSmart(s: SyncSession, ns: Int, path: String, size: Long, sha: ByteArray, mtime: Long, replace: Boolean,
                         open: (Long) -> InputStream, progress: (Long) -> Unit): PutResult = try {
        s.put(ns, path, size, sha, mtime, PutMode.SKIP_IDENTICAL, open, cancelled, progress)
    } catch (e: SyncException) {
        if (e.code != SyncException.E_EXISTS || !replace) throw e
        s.put(ns, path, size, sha, mtime, PutMode.REPLACE, open, cancelled, progress)
    }

    private fun openAt(open: () -> InputStream, offset: Long): InputStream {
        val i = open()
        var left = offset
        while (left > 0) {
            val n = i.skip(left)
            if (n <= 0) {
                if (i.read() < 0) {
                    i.close()
                    throw IOException("the file is shorter than expected")
                }
                left--
            } else {
                left -= n
            }
        }
        return i
    }

    /** SHA-256 of a phone file. Hashing a big file can take minutes: the open session gets its PINGs meanwhile. */
    private fun hash(open: () -> InputStream, progress: (Long) -> Unit): Pair<ByteArray, Long> {
        val md = MessageDigest.getInstance("SHA-256")
        val buf = ByteArray(1 shl 16)
        var n = 0L
        open().use { i ->
            while (true) {
                if (stop) throw SyncException(SyncException.E_CANCELLED)
                val r = i.read(buf)
                if (r < 0) break
                md.update(buf, 0, r)
                n += r
                progress(n)
                if (session != null && env.now() - lastPing >= PING_MS) keepSessionAlive()
            }
        }
        return md.digest() to n
    }

    private fun refreshAfterWrite(s: SyncSession) {
        try {
            val space = s.df()
            ui { st -> st.connected?.let { st.copy(link = it.copy(space = space)) } ?: st }
        } catch (e: Exception) {
            // only the free space shown
        }
    }

    // ---- file browser -------------------------------------------------------------------------

    override fun openFolder(path: String) {
        val s = session ?: return
        if (state.busy) return
        state = state.copy(browser = state.browser.copy(path = path, loading = true, error = null,
            entries = if (path == state.browser.path) state.browser.entries else null))
        env.execute {
            try {
                val list = sortEntries(s.list(path))
                ui { if (it.browser.path == path) it.copy(browser = it.browser.copy(entries = list, loading = false)) else it }
            } catch (e: Exception) {
                val err = errorOf(e)
                if (lost(e)) dropSession()
                ui {
                    it.copy(link = if (lost(e)) Link.Idle else it.link, error = if (lost(e)) err else it.error,
                        browser = it.browser.copy(loading = false, error = err))
                }
            }
        }
    }

    /** Back in the browser: one folder up. False at the top of the card. */
    fun up(): Boolean {
        val b = state.browser
        if (b.path.isEmpty() || state.busy) return false
        openFolder(SyncPaths.parent(b.path))
        return true
    }

    override fun refreshFolder() = openFolder(state.browser.path)

    override fun sendHere() {
        if (state.transfer.running) return
        state = state.copy(destPath = state.browser.path)
        replan()
    }

    override fun prompt(p: Prompt?) {
        if (state.browser.working) return
        state = state.copy(browser = state.browser.copy(prompt = p))
    }

    override fun applyPrompt() {
        val s = session ?: return
        val b = state.browser
        val p = b.prompt ?: return
        if (!begin()) return
        val dir = b.path
        val op: () -> Unit = when (p) {
            is Prompt.NewFolder -> {
                val n = SyncPaths.cleanName(p.name) ?: return badName()
                ({ s.mkdir(SyncPaths.join(dir, n), false) })
            }
            is Prompt.Rename -> {
                val n = SyncPaths.cleanName(p.name) ?: return badName()
                if (n == p.entry.name) {
                    state = state.copy(browser = b.copy(prompt = null))
                    return
                }
                ({ s.move(SyncPaths.join(dir, p.entry.name), SyncPaths.join(dir, n), false) })
            }
            is Prompt.Delete -> ({ s.delete(SyncPaths.join(dir, p.entry.name), p.entry.isFolder) })
        }
        state = state.copy(browser = b.copy(working = true, error = null))
        browserOp(s, op)
    }

    private fun badName() {
        state = state.copy(browser = state.browser.copy(error = ErrorInfo(R.string.sync_err_bad_name, null)))
    }

    private fun browserOp(s: SyncSession, op: () -> Unit) {
        env.execute {
            try {
                op()
                val path = state.browser.path
                val list = sortEntries(s.list(path))
                ui { it.copy(browser = it.browser.copy(working = false, prompt = null, moving = null, entries = list)) }
                refreshAfterWrite(s)
            } catch (e: Exception) {
                val err = errorOf(e)
                if (lost(e)) dropSession()
                ui {
                    it.copy(link = if (lost(e)) Link.Idle else it.link, error = if (lost(e)) err else it.error,
                        browser = it.browser.copy(working = false, prompt = null, error = err))
                }
            }
        }
    }

    override fun startMove(entry: SyncEntry) {
        if (state.busy) return
        state = state.copy(browser = state.browser.copy(moving = SyncPaths.join(state.browser.path, entry.name), prompt = null))
    }

    override fun moveHere() {
        val s = session ?: return
        val b = state.browser
        val src = b.moving ?: return
        val dst = SyncPaths.join(b.path, SyncPaths.name(src))
        if (dst == src) {
            state = state.copy(browser = b.copy(moving = null))
            return
        }
        if (b.path == src || b.path.startsWith("$src/")) {
            state = state.copy(browser = b.copy(error = ErrorInfo(R.string.sync_err_move_inside, src)))
            return
        }
        if (!begin()) return
        state = state.copy(browser = b.copy(working = true, error = null))
        browserOp(s) { s.move(src, dst, false) }
    }

    override fun cancelMove() {
        state = state.copy(browser = state.browser.copy(moving = null))
    }

    // ---- library --------------------------------------------------------------------------------

    override fun loadLibrary() {
        val s = session ?: return
        if (!begin()) return
        state = state.copy(library = LibraryPhase.Loading(0, 0), libraryNav = LibraryNav())
        env.execute {
            val dir = File(env.cacheDir, "sync")
            val tmp = File(dir, "library.tmp")
            val db = File(dir, "library.db")
            try {
                dir.mkdirs()
                val pace = Pace()
                FileOutputStream(tmp).use { o ->
                    s.dbSnapshot(o, cancelled) { done, total ->
                        if (pace.due(done == total)) ui { it.copy(library = LibraryPhase.Loading(done, total)) }
                    }
                }
                db.delete()
                if (!tmp.renameTo(db)) throw IOException("the library copy could not be kept")
                val lib = Library.build(env.readLibrary(db))
                ui { it.copy(library = LibraryPhase.Ready(lib)) }
            } catch (e: Exception) {
                tmp.delete()
                if (e is SyncException && e.code == SyncException.E_CANCELLED) {
                    ui { it.copy(library = LibraryPhase.Idle) }
                } else {
                    val err = errorOf(e)
                    if (lost(e)) dropSession()
                    ui { it.copy(link = if (lost(e)) Link.Idle else it.link, library = LibraryPhase.Failed(err)) }
                }
            }
        }
    }

    override fun libraryNav(nav: LibraryNav) {
        state = state.copy(libraryNav = nav)
    }

    /** Back in the library: album -> artist -> list. False at the list. */
    fun libraryBack(): Boolean {
        val n = state.libraryNav
        return when {
            n.album != null -> {
                state = state.copy(libraryNav = n.copy(album = null))
                true
            }
            n.artist != null -> {
                state = state.copy(libraryNav = n.copy(artist = null))
                true
            }
            else -> false
        }
    }

    // ---- ByOS update (the files of the patch flow) ---------------------------------------------

    override fun sendByosUpdate() {
        val s = session ?: return
        val files = env.byosFiles() ?: return
        if (!begin()) return
        val total = files.sumOf { it.size * (if (it.sha256 == null) 2 else 1) }
        state = state.copy(update = UpdatePhase.Sending(0, total, ""))
        env.execute {
            try {
                val pace = Pace()
                var done = 0L
                var skipped = 0
                val dirs = files.map { SyncPaths.parent(it.path) }.filter { it.isNotEmpty() }.distinct().sortedBy { it.count { c -> c == '/' } }
                for (d in dirs) s.mkdir(d, true)
                for (f in files) {
                    if (stop) throw SyncException(SyncException.E_CANCELLED)
                    val sha = f.sha256?.let { unhex(it) } ?: run {
                        val base = done
                        val h = hash(f.open) { n -> if (pace.due()) ui { it.copy(update = UpdatePhase.Sending(base + n, total, f.path)) } }
                        done += f.size
                        h.first
                    }
                    val base = done
                    val r = putLocal(Ns.SD, f.path, f.size, sha, 0, true, { off -> openAt(f.open, off) }) { n ->
                        if (pace.due()) ui { it.copy(update = UpdatePhase.Sending(base + n, total, f.path)) }
                    }
                    if (r.skipped) skipped++
                    done = base + f.size
                }
                ui { it.copy(update = UpdatePhase.Sent(files.size, skipped)) }
                session?.let { refreshAfterWrite(it) }
            } catch (e: Exception) {
                if (e is SyncException && e.code == SyncException.E_CANCELLED) {
                    ui { it.copy(update = UpdatePhase.Idle) }
                } else {
                    val err = errorOf(e)
                    if (lost(e)) dropSession()
                    ui { it.copy(link = if (lost(e)) Link.Idle else it.link, update = UpdatePhase.Failed(err)) }
                }
            }
        }
    }

    override fun cancelWork() {
        stop = true
    }

    /** The device to reconnect to after a lost connection. */
    fun reconnect() {
        lastDevice?.let { connectTo(it) }
    }

    /**
     * Ends everything when the app goes away. Called on the main thread: the closing (BYE on the
     * socket) runs on the worker, after the job in flight has seen [stop].
     */
    fun close() {
        stop = true
        try {
            keepAlive.close()
        } catch (e: Exception) {
            // the timer is gone anyway
        }
        val p = pairing
        pairing = null
        env.execute {
            dropSession()
            current = null
            try {
                p?.close()
            } catch (e: Exception) {
                // closing anyway
            }
        }
    }

    companion object {
        /** Section 17: clients PING every 30 s while idle (the device drops a session quiet for 120 s). */
        const val PING_MS = 30_000L

        fun sortEntries(list: List<SyncEntry>): List<SyncEntry> =
            list.sortedWith(compareBy<SyncEntry> { if (it.isFolder) 0 else 1 }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name })

        fun unhex(s: String): ByteArray = ByteArray(s.length / 2) { i -> s.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
    }
}
