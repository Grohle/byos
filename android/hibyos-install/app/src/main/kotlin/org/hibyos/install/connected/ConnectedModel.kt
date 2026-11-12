package org.hibyos.install.connected

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.hibyos.install.ErrorInfo
import org.hibyos.install.R
import org.hibyos.install.core.SendItem
import org.hibyos.install.core.btp.UpdateException
import org.hibyos.install.core.btp.UpdatePackage
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

    /** Opens the system picker for a signed update (hibyos-update.json and its tar). */
    fun pickUpdateFiles() {}

    /** The keys a signed update must be signed with (docs/OTA.md). */
    val releaseKeys: List<String> get() = UpdatePackage.RELEASE_KEYS

    /** Opens the system's "save as" for a file named [name]; the answer comes back through ConnectedModel.downloadTo. */
    fun createDocument(name: String) {}

    /** Waits on the worker, between two UPDATE_STATUS requests (tests do not wait). */
    fun sleep(ms: Long) = Thread.sleep(ms)

    /** On the UI thread after each change: what runs now (null: nothing), for a foreground notification. */
    fun work(note: WorkNote?) {}
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

    private fun ui(f: (SyncState) -> SyncState) = env.post {
        state = f(state)
        env.work(workOf(state))
    }

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
            SyncException.RECONNECTED -> ErrorInfo(R.string.sync_err_reconnected, d, R.string.sync_hint_reconnected)
            SyncException.NO_CLIENT -> ErrorInfo(R.string.sync_err_no_client, d, R.string.sync_hint_no_client)
            SyncException.E_PAIR_CLOSED -> ErrorInfo(R.string.sync_err_pair_closed, d, R.string.sync_hint_pair_closed)
            SyncException.E_PAIR_REJECTED -> ErrorInfo(R.string.sync_err_pair_rejected, d, R.string.sync_hint_pair_rejected)
            SyncException.E_BUSY -> ErrorInfo(R.string.sync_err_busy, d, R.string.sync_hint_busy)
            SyncException.E_RATE_LIMIT -> ErrorInfo(R.string.sync_err_rate, d, R.string.sync_hint_rate)
            SyncException.E_NO_SPACE -> ErrorInfo(R.string.sync_err_no_space, d, R.string.sync_hint_no_space)
            SyncException.E_SD_UNAVAILABLE -> ErrorInfo(R.string.sync_err_sd, d, R.string.sync_hint_sd)
            SyncException.E_EXISTS -> ErrorInfo(R.string.sync_err_exists, d)
            SyncException.E_BAD_PATH -> ErrorInfo(R.string.sync_err_bad_name, d)
            SyncException.E_FILE_TOO_BIG -> ErrorInfo(R.string.sync_err_too_big, d, R.string.sync_hint_too_big)
            SyncException.E_NOT_DIR, SyncException.E_IS_DIR -> ErrorInfo(R.string.sync_err_kind, d, R.string.sync_hint_refresh)
            SyncException.E_NOT_FOUND -> ErrorInfo(R.string.sync_err_not_found, d, R.string.sync_hint_refresh)
            SyncException.E_NOT_EMPTY -> ErrorInfo(R.string.sync_err_not_empty, d)
            SyncException.E_UNSUPPORTED, SyncException.E_VERSION, SyncException.E_NOT_ALLOWED ->
                ErrorInfo(R.string.sync_err_unsupported, d, R.string.sync_hint_unsupported)
            SyncException.E_IO, SyncException.E_HASH_MISMATCH -> ErrorInfo(R.string.sync_err_io, d, R.string.sync_hint_io)
            SyncException.E_SIGNATURE -> ErrorInfo(R.string.sync_err_signature, d, R.string.sync_hint_signature)
            SyncException.E_UPDATE_INVALID -> ErrorInfo(R.string.sync_err_update_invalid, d, R.string.sync_hint_update_invalid)
            SyncException.E_DOWNGRADE -> ErrorInfo(R.string.sync_err_downgrade, d, R.string.sync_hint_downgrade)
            SyncException.E_BATTERY -> ErrorInfo(R.string.sync_err_battery, d, R.string.sync_hint_battery)
            SyncException.E_NEEDS_FIRMWARE -> ErrorInfo(R.string.sync_err_needs_firmware, d, R.string.sync_hint_needs_firmware)
            SyncException.E_NOT_STAGED -> ErrorInfo(R.string.sync_err_not_staged, d, R.string.sync_hint_not_staged)
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

    /**
     * The error ended the session: a fatal one, or any error after the session was already
     * dropped (a reconnection the player refused with NOT_PAIRED or FORGOTTEN), so the screens
     * never show Connected without a session.
     */
    private fun lost(e: Throwable): Boolean = (e is SyncException && e.fatal) || session == null

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
                // the device the user chose: another one answering at that address is refused. A
                // paired player is waited for and looked for by its id (it may have restarted its
                // sync service or moved to another address), so this can take a few seconds
                val s = try {
                    c.connect(device.host, device.port, device.deviceId.ifEmpty { null }, cancelled)
                } catch (e: SyncException) {
                    when (e.code) {
                        SyncException.E_CANCELLED -> {
                            ui { it.copy(link = Link.Idle) }
                            return@execute
                        }
                        // still paired: say that the player was not found, not that it must be paired again
                        SyncException.UNREACHABLE, SyncException.CONNECTION_LOST, SyncException.E_TIMEOUT ->
                            if (device.paired && device.deviceId.isNotEmpty()) {
                                ui { it.copy(link = Link.Idle, error = ErrorInfo(R.string.sync_err_paired_gone, device.name,
                                    R.string.sync_hint_paired_gone)) }
                                return@execute
                            } else throw e
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

    /**
     * The session comes back by itself when its connection drops (see SyncSession): while it does,
     * the running transfer or update says "connecting again" instead of looking stuck.
     */
    private fun watch(s: SyncSession) {
        s.onReconnect { n ->
            ui {
                it.copy(
                    transfer = if (it.transfer.running) it.transfer.copy(reconnecting = n) else it.transfer,
                    update = (it.update as? UpdatePhase.Sending)?.copy(reconnecting = n) ?: it.update,
                    ota = (it.ota as? OtaPhase.Sending)?.copy(reconnecting = n) ?: it.ota,
                )
            }
            // the player may have a new address now
            if (n == 0) s.host?.let { h -> moved(h) }
        }
    }

    /** The open session reached the player at [host]: kept for the next connection. */
    private fun moved(host: String) {
        val d = current ?: return
        if (d.host == host) return
        val m = d.copy(host = host)
        current = m
        lastDevice = m
        ui { st -> st.connected?.let { st.copy(link = it.copy(device = m)) } ?: st }
    }

    private fun opened(device: SyncDevice, s: SyncSession) {
        session = s
        watch(s)
        lastPing = env.now()
        val space = try {
            s.df()
        } catch (e: SyncException) {
            null
        }
        val paired = safePaired()
        val d = device.copy(host = s.host ?: device.host, name = s.info.name.ifEmpty { device.name }, deviceId = s.info.deviceId, paired = true)
        val samePlayer = d.deviceId.isNotEmpty() && d.deviceId == libraryOf
        libraryOf = d.deviceId
        current = d
        lastDevice = d
        coverAsked.clear()
        ui {
            val base = it.copy(link = Link.Connected(d, s.info, space), paired = paired, error = null, browser = Browser())
            // what was read from another player (its library, playlists, artwork) must never act on this one's card
            if (samePlayer) base else base.copy(library = LibraryPhase.Idle, libraryNav = LibraryNav(), playlists = Playlists(), covers = emptyMap(),
                rescan = Rescan.Idle, libDelete = null, libError = null, libraryChanged = false, libraryQuery = "")
        }
    }

    /** The player whose library, playlists and artwork the state holds (its device id). */
    private var libraryOf: String? = null

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
        state = state.copy(link = Link.Idle, browser = Browser(), library = LibraryPhase.Idle, libraryNav = LibraryNav(), playlists = Playlists(),
            covers = emptyMap(), libDelete = null, libError = null, libraryChanged = false, rescan = Rescan.Idle)
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
        val s = c.connect(d.host, d.port, d.deviceId.ifEmpty { null }, cancelled)
        session = s
        watch(s)
        s.host?.let { moved(it) }
        lastPing = env.now()
        return s
    }

    /**
     * A transfer lost its connection: waits a little longer each time (RECONNECT_DELAYS), then
     * opens a new session with the same player, without a code (AUTH). When the player is no
     * longer at its address (a new DHCP lease), it is looked for again by its device id. Returns
     * the attempts used; throws [lostWith] when every attempt failed.
     */
    private fun reconnectDuringTransfer(from: Int, lostWith: SyncException,
                                        show: (Int) -> Unit = { n -> ui { it.copy(transfer = it.transfer.copy(reconnecting = n)) } }): Int {
        dropSession()
        var attempt = from
        try {
            while (attempt < RECONNECT_DELAYS.size) {
                val n = attempt + 1
                show(n)
                env.sleep(RECONNECT_DELAYS[attempt])
                attempt++
                if (stop) throw SyncException(SyncException.E_CANCELLED)
                try {
                    reconnectCurrent()
                    return attempt
                } catch (e: SyncException) {
                    // the player refused this phone: trying again changes nothing
                    if (e.code == SyncException.NOT_PAIRED || e.code == SyncException.FORGOTTEN || e.code == SyncException.IDENTITY_CHANGED ||
                        e.code == SyncException.E_CANCELLED) throw e
                }
            }
            throw lostWith
        } finally {
            show(0)
        }
    }

    private fun reconnectCurrent() {
        val d = current ?: throw SyncException(SyncException.CONNECTION_LOST)
        val c = connector ?: throw SyncException(SyncException.NO_CLIENT)
        val s = try {
            // for a paired player the connector itself waits for it and looks for it by its id
            c.connect(d.host, d.port, d.deviceId.ifEmpty { null }, cancelled)
        } catch (e: SyncException) {
            if (e.code != SyncException.UNREACHABLE || d.deviceId.isEmpty()) throw e
            val found = c.discover(2000, cancelled) {}.firstOrNull { it.deviceId == d.deviceId } ?: throw e
            val m = d.copy(host = found.host, port = found.port)
            current = m
            lastDevice = m
            c.connect(m.host, m.port, d.deviceId, cancelled).also {
                ui { st -> st.connected?.let { st.copy(link = it.copy(device = m)) } ?: st }
            }
        }
        session = s
        watch(s)
        s.host?.let { moved(it) }
        lastPing = env.now()
    }

    /** [putSmart] on the open session; a read error of the phone's file replaces the session, then is rethrown. */
    private fun putLocal(ns: Int, path: String, size: Long, sha: ByteArray, mtime: Long, replace: Boolean,
                         open: (Long) -> InputStream, exact: PutMode? = null, progress: (Long) -> Unit): PutResult {
        val s = session ?: throw SyncException(SyncException.CONNECTION_LOST)
        try {
            return if (exact != null) s.put(ns, path, size, sha, mtime, exact, open, cancelled, progress)
            else putSmart(s, ns, path, size, sha, mtime, replace, open, progress)
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

    override fun setRoute(route: Route, folder: String) {
        if (state.transfer.running) return
        state = state.copy(routes = if (folder == route.folder) state.routes - route else state.routes + (route to folder))
        replan()
    }

    override fun useUpdateFiles() {
        if (state.transfer.running) return
        val upd = picked.filter { FileKinds.of(it.parts.last()) == FileKind.BYOS_UPDATE }
        if (upd.isEmpty()) return
        picked = picked - upd.toSet()
        replan()
        addUpdateFiles { upd }
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

    /**
     * Adds what the user picked (read on the worker by [load], which may walk a folder). Files
     * shared from another app while a transfer runs are not dropped: the worker reads them once
     * the transfer is over, and the send screen says they are waiting meanwhile.
     */
    fun addPicked(load: () -> List<LocalItem>) {
        if (state.transfer.running) state = state.copy(sharedWaiting = true)
        env.execute {
            try {
                val items = load()
                env.post {
                    if (state.transfer.running) {
                        // a transfer started meanwhile: they wait behind it on the worker
                        addPicked { items }
                        return@post
                    }
                    // a finished transfer is replaced by the new choice
                    if (state.transfer.finished) clearPlan()
                    picked = picked + items
                    replan()
                    state = state.copy(sharedWaiting = false)
                }
            } catch (e: Exception) {
                ui { it.copy(sharedWaiting = false, transfer = it.transfer.copy(error = errorOf(e))) }
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

    /**
     * Plans the picked files for the chosen destination. After a transfer that stopped half way
     * (cancelled, connection lost), the files already on the card stay as they are and only the
     * others (and anything picked since) follow the new choice.
     */
    private fun replan() {
        val old = state.transfer
        val kept = old.jobs.filter { it.finished }
        val keptItems = java.util.Collections.newSetFromMap(java.util.IdentityHashMap<LocalItem, Boolean>())
        for (j in kept) sources[j.id]?.let { keptItems += it }
        val rest = picked.filter { it !in keptItems }
        val max = state.connected?.space?.maxFile ?: Long.MAX_VALUE
        val firstId = (kept.maxOfOrNull { it.id } ?: 0) + 1
        val plan = if (state.destPath == null && state.dest == Destination.AUTO) SendPlan.auto(rest, state.routes, max, firstId)
        else SendPlan.build(rest, state.target, max, firstId)
        sources = kept.mapNotNull { j -> sources[j.id]?.let { j.id to it } }.toMap() + plan.sources
        folders = plan.folders
        hashes.clear()
        state = state.copy(transfer = Transfer(jobs = kept + plan.jobs, newMusic = old.newMusic))
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
            var newMusic = 0
            val changedMusic = ArrayList<String>()
            fun mark(id: Int, f: (SendJob) -> SendJob) = ui { st ->
                st.copy(transfer = st.transfer.copy(jobs = st.transfer.jobs.map { if (it.id == id) f(it) else it }))
            }
            val settled = HashSet<Int>()
            var attempt = 0
            try {
                while (true) {
                    try {
                        val cur = session ?: throw SyncException(SyncException.CONNECTION_LOST)
                        if (target.isNotEmpty()) cur.mkdir(target, true)
                        for (f in folders) cur.mkdir(f, true)
                        for (job in todo) {
                            if (job.id in settled) continue
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
                                if (!r.skipped && FileKinds.isAudio(job.remote)) {
                                    if (r.replaced) changedMusic += job.remote else newMusic++
                                }
                            } catch (e: SyncException) {
                                when {
                                    e.code == SyncException.E_CANCELLED -> throw e
                                    // the connection dropped: the file is not marked "not sent" while the
                                    // session comes back and the PUT resumes (it would read as a failure
                                    // that then turns into "sent"); the end marks it if it never comes back
                                    e.fatal || e.code == SyncException.CONNECTION_LOST || e.code == SyncException.RECONNECTED -> {
                                        mark(job.id) { it.copy(status = JobStatus.WAITING, done = 0) }
                                        throw e
                                    }
                                    e.code == SyncException.E_NO_SPACE || e.code == SyncException.E_SD_UNAVAILABLE -> {
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
                            settled += job.id
                        }
                        break
                    } catch (e: SyncException) {
                        // the session already came back by itself: go on where it stopped
                        if (e.code == SyncException.RECONNECTED && !stop && session != null && attempt < RECONNECT_DELAYS.size) {
                            attempt++
                            continue
                        }
                        // the connection dropped (Wi-Fi, the player asleep): connect again and go on where it stopped
                        if (!lost(e) || stop || current == null || attempt >= RECONNECT_DELAYS.size) throw e
                        attempt = reconnectDuringTransfer(attempt, e)
                    }
                }
                session?.let { refreshTags(it, changedMusic) }
                val added = newMusic
                ui { it.copy(transfer = it.transfer.copy(running = false, done = it.transfer.total, current = "", newMusic = it.transfer.newMusic + added),
                    rescan = if (added > 0) Rescan.Idle else it.rescan) }
                session?.let { refreshAfterWrite(it) }
            } catch (e: Exception) {
                val cancel = e is SyncException && e.code == SyncException.E_CANCELLED
                val err = if (cancel) null else errorOf(e)
                // also a reconnection the player refused (NOT_PAIRED, FORGOTTEN): the session is gone
                val gone = lost(e)
                if (gone) dropSession()
                ui {
                    it.copy(link = if (gone) Link.Idle else it.link, transfer = it.transfer.copy(running = false, current = "", error = err,
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
        s.put(ns, path, size, sha, mtime, PutMode.REPLACE, open, cancelled, progress).copy(replaced = true)
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

    /**
     * Songs replaced on the card keep their library rows: LIB_RESCAN mode 1 refreshes their tags,
     * cover and lyrics at once (section 13.3). Best effort: an older ByOS without it is fine.
     */
    private fun refreshTags(s: SyncSession, paths: List<String>) {
        for (chunk in paths.chunked(256)) {
            try {
                s.libRescan(chunk)
            } catch (e: SyncException) {
                if (e.fatal) throw e
                return
            }
        }
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
                ui {
                    it.copy(browser = it.browser.copy(working = false, prompt = null, moving = null, movingFiles = emptyList(), movingLabel = null, entries = list),
                        libraryChanged = it.libraryChanged || it.library is LibraryPhase.Ready)
                }
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
        state = state.copy(browser = state.browser.copy(moving = SyncPaths.join(state.browser.path, entry.name), movingFiles = emptyList(), movingLabel = null, prompt = null))
    }

    override fun moveHere() {
        val s = session ?: return
        val b = state.browser
        val src = b.moving ?: return
        if (b.movingFiles.isNotEmpty()) {
            // an album's songs out of a shared folder: each file goes here, the folder stays
            val files = b.movingFiles
            if (files.all { SyncPaths.parent(it) == b.path }) {
                cancelMove()
                return
            }
            if (!begin()) return
            state = state.copy(browser = b.copy(working = true, error = null))
            browserOp(s) {
                for (f in files) {
                    if (stop) throw SyncException(SyncException.E_CANCELLED)
                    if (SyncPaths.parent(f) == b.path) continue
                    try {
                        s.move(f, SyncPaths.join(b.path, SyncPaths.name(f)), false)
                    } catch (e: SyncException) {
                        // moved by an earlier try that stopped half way
                        if (e.code != SyncException.E_NOT_FOUND) throw e
                    }
                }
            }
            return
        }
        val dst = SyncPaths.join(b.path, SyncPaths.name(src))
        if (dst == src) {
            cancelMove()
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
        state = state.copy(browser = state.browser.copy(moving = null, movingFiles = emptyList(), movingLabel = null))
    }

    // ---- library --------------------------------------------------------------------------------

    /** DB_SNAPSHOT into the app's cache, read with Android's SQLite. */
    private fun readSnapshot(s: SyncSession, progress: ((Long, Long) -> Unit)?): Library {
        val dir = File(env.cacheDir, "sync")
        val tmp = File(dir, "library.tmp")
        val db = File(dir, "library.db")
        try {
            dir.mkdirs()
            val pace = Pace()
            FileOutputStream(tmp).use { o ->
                s.dbSnapshot(o, cancelled) { done, total ->
                    if (progress != null && pace.due(done == total)) progress(done, total)
                }
            }
            db.delete()
            if (!tmp.renameTo(db)) throw IOException("the library copy could not be kept")
            return Library.build(env.readLibrary(db))
        } finally {
            tmp.delete()
        }
    }

    override fun loadLibrary() {
        val s = session ?: return
        if (!begin()) return
        state = state.copy(library = LibraryPhase.Loading(0, 0), libraryNav = LibraryNav())
        env.execute {
            try {
                val lib = readSnapshot(s) { done, total -> ui { it.copy(library = LibraryPhase.Loading(done, total)) } }
                ui { it.copy(library = LibraryPhase.Ready(lib)) }
            } catch (e: Exception) {
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

    override fun libraryDelete(target: LibTarget?) {
        if (state.libWorking) return
        state = state.copy(libDelete = target, libError = null)
    }

    /**
     * Opens Files ready to move [target]. An album moves with its folder only when that folder is
     * its own (the rule of Delete, [ownsFolder]); in a folder shared with other songs or files
     * (Music/Singles, a flat "Música"), only the album's songs and their lyrics move.
     */
    override fun libraryMove(target: LibTarget) {
        val p = target.cardPath ?: return
        if (state.busy) return
        if (target !is LibTarget.AlbumOf) {
            state = state.copy(browser = Browser(path = SyncPaths.parent(p), moving = p))
            openFolder(SyncPaths.parent(p))
            return
        }
        val s = session ?: return
        if (!begin()) return
        val songs = target.album.tracks.mapNotNull { Library.cardPath(it.path) }
        val parent = SyncPaths.parent(p)
        state = state.copy(browser = Browser(path = parent, loading = true, working = true))
        env.execute {
            try {
                val files = if (ownsFolder(s, p, songs)) emptyList() else {
                    // each song with the lyrics of the same name beside it
                    val stems = songs.map { SyncPaths.name(it).substringBeforeLast('.').lowercase() }.toSet()
                    val lyrics = try {
                        s.list(p).filter { !it.isFolder && FileKinds.of(it.name) == FileKind.LYRICS && it.name.substringBeforeLast('.').lowercase() in stems }
                            .map { SyncPaths.join(p, it.name) }
                    } catch (e: SyncException) {
                        if (e.fatal) throw e
                        emptyList()
                    }
                    songs + lyrics
                }
                val list = sortEntries(s.list(parent))
                ui {
                    it.copy(browser = Browser(path = parent, entries = list, moving = p, movingFiles = files,
                        movingLabel = if (files.isEmpty()) null else target.album.title.ifEmpty { SyncPaths.name(songs.first()) }))
                }
            } catch (e: Exception) {
                val err = errorOf(e)
                if (lost(e)) dropSession()
                ui {
                    it.copy(link = if (lost(e)) Link.Idle else it.link, error = if (lost(e)) err else it.error,
                        browser = Browser(path = parent, error = err))
                }
            }
        }
    }

    override fun revealInFiles(target: LibTarget) {
        val p = target.cardPath ?: target.tracks.firstNotNullOfOrNull { Library.cardPath(it.path) } ?: return
        if (state.busy) return
        val dir = if (target is LibTarget.Song) SyncPaths.parent(p) else p
        state = state.copy(browser = Browser(path = dir))
        openFolder(dir)
    }

    /**
     * Deletes the songs of the target from the card. An album that has a folder of its own (no
     * other songs, no subfolders) goes with its folder, so its cover and lyrics go too; otherwise
     * only its songs are deleted. Then the player updates its library; where it cannot, the
     * songs are only taken out of the list shown here.
     */
    override fun confirmLibraryDelete() {
        val s = session ?: return
        val t = state.libDelete ?: return
        val lib = (state.library as? LibraryPhase.Ready)?.library ?: return
        if (!begin()) return
        state = state.copy(libWorking = true, libError = null)
        env.execute {
            try {
                val files = HashSet<String>()
                val dirs = HashSet<String>()
                val albums = when (t) {
                    is LibTarget.Song -> emptyList()
                    is LibTarget.AlbumOf -> listOf(t.album)
                    is LibTarget.ArtistOf -> t.artist.albums
                }
                if (t is LibTarget.Song) {
                    val p = Library.cardPath(t.track.path) ?: throw SyncException(SyncException.E_NOT_FOUND, t.track.path)
                    deleteIfThere(s, p)
                    files += p
                }
                for (al in albums) {
                    if (stop) throw SyncException(SyncException.E_CANCELLED)
                    val paths = al.tracks.mapNotNull { Library.cardPath(it.path) }
                    val folder = Library.folderOf(al.tracks)
                    if (folder != null && ownsFolder(s, folder, paths)) {
                        s.delete(folder, true)
                        dirs += folder
                    } else {
                        for (p in paths) deleteIfThere(s, p)
                    }
                    files += paths
                }
                val (next, rescan) = try {
                    s.libRescan(null)
                    readSnapshot(s, null) to Rescan.Done
                } catch (e: SyncException) {
                    if (e.code != SyncException.E_UNSUPPORTED) throw e
                    lib.without(files, dirs) to Rescan.Unsupported
                }
                ui {
                    val n = it.libraryNav
                    val nav = when (t) {
                        is LibTarget.ArtistOf -> n.copy(artist = null, album = null)
                        is LibTarget.AlbumOf -> n.copy(album = null)
                        else -> n
                    }
                    it.copy(libWorking = false, libDelete = null, library = LibraryPhase.Ready(next), rescan = rescan, libraryNav = nav,
                        libraryChanged = false)
                }
                refreshAfterWrite(s)
            } catch (e: Exception) {
                val err = errorOf(e)
                if (lost(e)) dropSession()
                ui { it.copy(link = if (lost(e)) Link.Idle else it.link, libWorking = false, libDelete = null, libError = err) }
            }
        }
    }

    private fun deleteIfThere(s: SyncSession, path: String) {
        try {
            s.delete(path, false)
        } catch (e: SyncException) {
            if (e.code != SyncException.E_NOT_FOUND) throw e
        }
    }

    /**
     * True when [folder] is the album's own folder: no subfolder, and nothing but [songs] and the
     * files that come with an album (artwork, lyrics, cue sheets, rip logs, hidden files). Any
     * other file (a booklet PDF, a video, notes, another song) keeps the folder: only the songs go.
     */
    private fun ownsFolder(s: SyncSession, folder: String, songs: List<String>): Boolean {
        val mine = songs.map { SyncPaths.name(it).lowercase() }.toSet()
        val list = try {
            s.list(folder)
        } catch (e: SyncException) {
            if (e.fatal) throw e
            return false
        }
        return list.all { !it.isFolder && (it.name.lowercase() in mine || albumCompanion(it.name)) }
    }

    /** Files that belong to an album's folder and go with it. */
    private fun albumCompanion(name: String): Boolean {
        val n = name.lowercase()
        if (n.startsWith(".") || n == "thumbs.db" || n == "desktop.ini") return true
        return FileKinds.of(n) == FileKind.IMAGE || FileKinds.ext(n) in companionExts
    }

    private val companionExts = setOf("lrc", "cue", "log", "accurip", "m3u", "m3u8", "sfv", "md5", "ffp")

    // ---- playlists (.m3u8 files in Playlists) ---------------------------------------------------

    private fun pl(f: (Playlists) -> Playlists) = ui { it.copy(playlists = f(it.playlists)) }

    /** Runs a playlist job on the worker: errors are shown on the playlists, a lost connection ends the session. */
    private fun playlistJob(f: (SyncSession) -> Unit) {
        val s = session ?: return
        if (!begin()) return
        state = state.copy(playlists = state.playlists.copy(working = true, error = null, added = null))
        env.execute {
            try {
                f(s)
                pl { it.copy(working = false) }
            } catch (e: Exception) {
                val err = errorOf(e)
                if (lost(e)) dropSession()
                ui { it.copy(link = if (lost(e)) Link.Idle else it.link, playlists = it.playlists.copy(working = false, error = err, newName = null)) }
            }
        }
    }

    private fun listPlaylists(s: SyncSession): List<String> = try {
        s.list(M3u.FOLDER).filter { !it.isFolder && M3u.isPlaylist(it.name) }
            .sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name }).map { SyncPaths.join(M3u.FOLDER, it.name) }
    } catch (e: SyncException) {
        if (e.code != SyncException.E_NOT_FOUND) throw e
        emptyList()
    }

    /** The playlist file as it is on the card, line for line (its other lines are kept when it is changed). */
    private fun readPlaylistFile(s: SyncSession, file: String): M3uFile {
        val out = java.io.ByteArrayOutputStream()
        s.get(file, 0, out, cancelled) { _, _ -> }
        return M3u.parse(SyncPaths.parent(file), out.toByteArray())
    }

    /** Card path -> "Artist - Title" for the #EXTINF lines of songs added to a playlist. */
    private fun titled(songs: List<String>): List<Pair<String, String>> {
        val titles = ((state.library as? LibraryPhase.Ready)?.library?.tracks ?: emptyList())
            .associate { (Library.cardPath(it.path) ?: "").lowercase() to listOf(it.artist, it.title).filter { t -> t.isNotEmpty() }.joinToString(" - ") }
        return songs.map { it to (titles[it.lowercase()] ?: "") }
    }

    private fun writePlaylist(s: SyncSession, file: String, f: M3uFile, create: Boolean) {
        val data = f.bytes()
        s.mkdir(SyncPaths.parent(file), true)
        s.put(Ns.SD, file, data.size.toLong(), MessageDigest.getInstance("SHA-256").digest(data), 0,
            if (create) PutMode.CREATE else PutMode.REPLACE, { off -> openAt({ data.inputStream() }, off) }, cancelled) {}
    }

    override fun loadPlaylists() = playlistJob { s ->
        val files = listPlaylists(s)
        pl { it.copy(files = files) }
    }

    override fun openPlaylist(file: String) {
        // refused while other work runs: the list stays as it is (no endless "Loading")
        if (session == null || state.busy) return
        state = state.copy(libraryNav = state.libraryNav.copy(playlist = file), playlists = state.playlists.copy(open = null))
        playlistJob { s ->
            val p = Playlist(file, readPlaylistFile(s, file).songs)
            pl { it.copy(open = p) }
        }
    }

    override fun addToPlaylist(target: LibTarget?) {
        if (state.playlists.working) return
        state = state.copy(playlists = state.playlists.copy(adding = target, error = null, added = null))
        if (target != null && state.playlists.files == null) loadPlaylists()
    }

    private fun songsOf(t: LibTarget): List<String> = t.tracks.mapNotNull { Library.cardPath(it.path) }

    override fun addToPlaylistFile(file: String) {
        val t = state.playlists.adding ?: return
        playlistJob { s ->
            val f = readPlaylistFile(s, file).adding(titled(songsOf(t)))
            writePlaylist(s, file, f, false)
            pl { it.copy(adding = null, added = M3u.title(file), open = if (it.open?.file == file) Playlist(file, f.songs) else it.open) }
        }
    }

    override fun newPlaylistName(text: String?) {
        if (state.playlists.working) return
        state = state.copy(playlists = state.playlists.copy(newName = text?.take(80)))
    }

    override fun createPlaylist() {
        val name = SyncPaths.cleanName(state.playlists.newName ?: return)
        if (name == null) {
            state = state.copy(playlists = state.playlists.copy(error = ErrorInfo(R.string.sync_err_bad_name, null)))
            return
        }
        val file = SyncPaths.join(M3u.FOLDER, "$name.m3u8")
        val songs = state.playlists.adding?.let { songsOf(it) } ?: emptyList()
        playlistJob { s ->
            writePlaylist(s, file, M3u.empty(SyncPaths.parent(file)).adding(titled(songs)), true)
            val files = listPlaylists(s)
            pl { it.copy(files = files, newName = null, adding = null, added = if (songs.isEmpty()) null else M3u.title(file)) }
        }
    }

    /**
     * Takes one song out of the open playlist. The file is read again first and only that song's
     * lines go; when the file changed meanwhile, the same song is looked for by its path.
     */
    override fun removeFromPlaylist(index: Int) {
        val p = state.playlists.open ?: return
        if (index !in p.songs.indices) return
        val song = p.songs[index]
        playlistJob { s ->
            val f = readPlaylistFile(s, p.file)
            val at = if (f.songs.getOrNull(index) == song) index else f.songs.indexOfFirst { it.equals(song, ignoreCase = true) }
            val next = if (at >= 0) f.removing(at).also { writePlaylist(s, p.file, it, false) } else f
            pl { it.copy(open = Playlist(p.file, next.songs)) }
        }
    }

    override fun deletePlaylist(file: String) = playlistJob { s ->
        s.delete(file, false)
        val files = listPlaylists(s)
        ui { it.copy(playlists = it.playlists.copy(files = files, open = null), libraryNav = it.libraryNav.copy(playlist = null)) }
    }

    override fun searchLibrary(text: String) {
        state = state.copy(libraryQuery = text.take(100))
    }

    private val coverAsked: MutableSet<String> = java.util.Collections.synchronizedSet(HashSet())

    private fun shortHash(s: String): String =
        MessageDigest.getInstance("SHA-256").digest(s.toByteArray()).take(12).joinToString("") { "%02x".format(it) }

    private val coverDir: File get() = File(env.cacheDir, "sync/covers")

    /** The cache file of one version of a folder's artwork: the player and folder, then the picture's name, size and date. */
    private fun coverFile(folder: String, pic: SyncEntry): File {
        val prefix = shortHash((current?.deviceId ?: "") + "/" + folder.lowercase())
        return File(coverDir, prefix + "-" + shortHash(pic.name + "|" + pic.size + "|" + pic.mtime) + ".img")
    }

    /**
     * The album's artwork, from its folder on the card (cover.jpg, folder.jpg..., else any
     * picture): LIST, then GET into the app's cache. The cached copy is used again only while
     * the picture on the card keeps its name, size and date, so replaced artwork shows at the
     * next connection. Asked once per folder and per connection, when the album shows on screen.
     */
    override fun loadCover(folder: String) {
        val s = session ?: return
        if (folder.isEmpty() || !coverAsked.add(folder)) return
        env.execute {
            if (session !== s) {
                coverAsked.remove(folder)
                return@execute
            }
            try {
                val pic = Library.coverIn(s.list(folder))
                if (pic == null) {
                    ui { it.copy(covers = it.covers + (folder to null)) }
                    return@execute
                }
                val f = coverFile(folder, pic)
                if (f.isFile && f.length() > 0) {
                    ui { it.copy(covers = it.covers + (folder to f.path)) }
                    return@execute
                }
                // older versions of this folder's artwork go
                val prefix = f.name.substringBefore('-') + "-"
                coverDir.listFiles()?.forEach { if (it.name.startsWith(prefix)) it.delete() }
                f.parentFile?.mkdirs()
                val tmp = File(f.path + ".tmp")
                FileOutputStream(tmp).use { o -> s.get(SyncPaths.join(folder, pic.name), 0, o, { false }) { _, _ -> } }
                f.delete()
                if (!tmp.renameTo(f)) throw IOException("the artwork could not be kept")
                ui { it.copy(covers = it.covers + (folder to f.path)) }
            } catch (e: Exception) {
                if (lost(e)) {
                    coverAsked.remove(folder)
                    failed(e)
                } else {
                    // no artwork is shown: the album keeps its music note
                    ui { it.copy(covers = it.covers + (folder to null)) }
                }
            }
        }
    }

    private var pendingDownload: String? = null

    override fun saveToPhone(entry: SyncEntry) {
        if (entry.isFolder || session == null || state.busy) return
        pendingDownload = SyncPaths.join(state.browser.path, entry.name)
        env.createDocument(entry.name)
    }

    override fun clearDownload() {
        if (state.browser.download?.running == true) return
        state = state.copy(browser = state.browser.copy(download = null))
    }

    /**
     * Copies the card file chosen with [saveToPhone] into what the user created on the phone
     * ([open] gives its stream), checking the whole file against the SHA-256 of GET_END. When
     * the copy does not finish whole (cancelled, connection lost, SHA-256 differs) or cannot
     * start, [discard] deletes that document: no partial file is left looking like a good one.
     */
    fun downloadTo(discard: () -> Unit = {}, open: () -> java.io.OutputStream) {
        fun drop() = try {
            discard()
        } catch (e: Exception) {
            // nothing more can be done about it
        }
        val s = session
        val path = pendingDownload
        pendingDownload = null
        if (s == null || path == null || !begin()) {
            env.execute { drop() }
            return
        }
        val name = SyncPaths.name(path)
        state = state.copy(browser = state.browser.copy(download = Download(name)))
        env.execute {
            var whole = false
            try {
                val pace = Pace()
                val md = MessageDigest.getInstance("SHA-256")
                val r = open().use { o ->
                    java.security.DigestOutputStream(o, md).use { d ->
                        s.get(path, 0, d, cancelled) { done, total ->
                            if (pace.due(done == total)) ui { it.copy(browser = it.browser.copy(download = Download(name, done, total))) }
                        }
                    }
                }
                if (!md.digest().contentEquals(r.sha256)) throw SyncException(SyncException.E_HASH_MISMATCH, "$name: the copy differs from the card")
                whole = true
                ui { it.copy(browser = it.browser.copy(download = Download(name, r.size, r.size, finished = true))) }
            } catch (e: Exception) {
                if (!whole) drop()
                if (e is SyncException && e.code == SyncException.E_CANCELLED) {
                    ui { it.copy(browser = it.browser.copy(download = null)) }
                } else {
                    val err = errorOf(e)
                    if (lost(e)) dropSession()
                    ui { it.copy(link = if (lost(e)) Link.Idle else it.link, browser = it.browser.copy(download = Download(name, error = err))) }
                }
            }
        }
    }

    override fun rescanLibrary() {
        val s = session ?: return
        if (!begin()) return
        state = state.copy(rescan = Rescan.Running)
        env.execute {
            try {
                s.libRescan(null)
                // the player answers when its scan is over: the new library is read at once
                val lib = readSnapshot(s, null)
                ui { it.copy(rescan = Rescan.Done, library = LibraryPhase.Ready(lib), transfer = it.transfer.copy(newMusic = 0), libraryChanged = false) }
            } catch (e: Exception) {
                when {
                    e is SyncException && e.code == SyncException.E_UNSUPPORTED -> ui { it.copy(rescan = Rescan.Unsupported) }
                    e is SyncException && e.code == SyncException.E_CANCELLED -> ui { it.copy(rescan = Rescan.Idle) }
                    else -> {
                        val err = errorOf(e)
                        if (lost(e)) dropSession()
                        ui { it.copy(link = if (lost(e)) Link.Idle else it.link, rescan = Rescan.Failed(err)) }
                    }
                }
            }
        }
    }

    /** Back in the library: album -> artist -> list. False at the list. */
    fun libraryBack(): Boolean {
        val n = state.libraryNav
        return when {
            n.playlist != null -> {
                state = state.copy(libraryNav = n.copy(playlist = null), playlists = state.playlists.copy(open = null))
                true
            }
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

    /**
     * Sends the ByOS folder and the firmware file made by the patch flow to the card (several
     * hundred files). The player's sync service may restart during it (the shell stops it under
     * memory pressure): the session comes back by itself, and when it cannot, the transfer
     * connects again like Send Files does and goes on with the file it stopped at (the device
     * resumes that one from its part). A file that is already on the card is skipped.
     */
    override fun sendByosUpdate() {
        if (session == null) return
        val files = env.byosFiles() ?: return
        if (!begin()) return
        val total = files.sumOf { it.size * (if (it.sha256 == null) 2 else 1) }
        state = state.copy(update = UpdatePhase.Sending(0, total, ""))
        env.execute {
            var atPath = ""
            try {
                val pace = Pace()
                var done = 0L
                var skipped = 0
                var next = 0
                val hashed = HashMap<Int, ByteArray>()
                var attempt = 0
                fun sending(d: Long, path: String) = ui { st ->
                    val r = (st.update as? UpdatePhase.Sending)?.reconnecting ?: 0
                    st.copy(update = UpdatePhase.Sending(d, total, path, r))
                }
                while (true) {
                    try {
                        val s = session ?: throw SyncException(SyncException.CONNECTION_LOST)
                        if (next == 0) {
                            val dirs = files.map { SyncPaths.parent(it.path) }.filter { it.isNotEmpty() }.distinct().sortedBy { it.count { c -> c == '/' } }
                            for (d in dirs) s.mkdir(d, true)
                        }
                        while (next < files.size) {
                            val f = files[next]
                            atPath = f.path
                            if (stop) throw SyncException(SyncException.E_CANCELLED)
                            val sha = f.sha256?.let { unhex(it) } ?: hashed[next] ?: run {
                                val base = done
                                val h = hash(f.open) { n -> if (pace.due()) sending(base + n, f.path) }
                                done += f.size
                                hashed[next] = h.first
                                h.first
                            }
                            val base = done
                            val r = putLocal(Ns.SD, f.path, f.size, sha, 0, true, { off -> openAt(f.open, off) }) { n ->
                                if (pace.due()) sending(base + n, f.path)
                            }
                            if (r.skipped) skipped++
                            done = base + f.size
                            next++
                        }
                        break
                    } catch (e: SyncException) {
                        if (e.code == SyncException.RECONNECTED && !stop && session != null && attempt < RECONNECT_DELAYS.size) {
                            attempt++
                            continue
                        }
                        if (!lost(e) || stop || current == null || attempt >= RECONNECT_DELAYS.size) throw e
                        attempt = reconnectDuringUpdate(attempt, e)
                    }
                }
                ui { it.copy(update = UpdatePhase.Sent(files.size, skipped)) }
                session?.let { refreshAfterWrite(it) }
            } catch (e: Exception) {
                if (e is SyncException && e.code == SyncException.E_CANCELLED) {
                    ui { it.copy(update = UpdatePhase.Idle) }
                } else {
                    // say which file it stopped at: the next try goes on from there
                    val err = errorOf(e).let { if (atPath.isNotEmpty() && it.detail.isNullOrEmpty()) it.copy(detail = atPath) else it }
                    val gone = lost(e)
                    if (gone) dropSession()
                    ui { it.copy(link = if (gone) Link.Idle else it.link, update = UpdatePhase.Failed(err)) }
                }
            }
        }
    }

    /** [reconnectDuringTransfer] for the update screens: their progress says "connecting again". */
    private fun reconnectDuringUpdate(from: Int, lostWith: SyncException): Int {
        fun show(n: Int) = ui {
            it.copy(update = (it.update as? UpdatePhase.Sending)?.copy(reconnecting = n) ?: it.update,
                ota = (it.ota as? OtaPhase.Sending)?.copy(reconnecting = n) ?: it.ota)
        }
        return reconnectDuringTransfer(from, lostWith, ::show)
    }

    // ---- signed ByOS update (sections 14.1 to 14.3) --------------------------------------------

    private var otaPkg: UpdatePackage? = null
    private var otaTar: LocalItem? = null
    private var otaTarSha: ByteArray? = null

    /** The update files were sent to the player of the open session (a downgrade retry skips sending). */
    private var otaSentTo: SyncSession? = null

    override fun pickUpdateFiles() {
        if (!state.ota.running) env.pickUpdateFiles()
    }

    override fun clearUpdateFiles() {
        if (state.ota.running) return
        otaPkg = null
        otaTar = null
        otaTarSha = null
        otaSentTo = null
        state = state.copy(otaPick = OtaPick(), ota = OtaPhase.Idle)
    }

    private fun updateError(e: UpdateException): ErrorInfo = when (e.kind) {
        UpdateException.Kind.SIGNATURE -> ErrorInfo(R.string.sync_err_signature, e.message, R.string.sync_hint_signature)
        UpdateException.Kind.PAYLOAD -> ErrorInfo(R.string.sync_err_update_payload, e.message, R.string.sync_hint_update_payload)
        else -> ErrorInfo(R.string.sync_err_update_invalid, e.message, R.string.sync_hint_update_invalid)
    }

    /**
     * Adds what the user picked for a signed update: a `.json` is read and its signature checked
     * at once (nothing in it is used before that), a `.tar` is kept as the payload. Picking one
     * file replaces the earlier file of the same kind.
     */
    fun addUpdateFiles(load: () -> List<LocalItem>) {
        if (state.ota.running) return
        env.execute {
            try {
                var pkg: UpdatePackage? = null
                var tar: LocalItem? = null
                var err: ErrorInfo? = null
                for (it in load()) {
                    val name = it.parts.last()
                    val lower = name.lowercase()
                    when {
                        lower.endsWith(".json") -> try {
                            pkg = UpdatePackage.parse(readSmall(it, UpdatePackage.JSON_MAX), env.releaseKeys)
                        } catch (e: UpdateException) {
                            err = updateError(e)
                        }
                        lower.endsWith(".tar") -> tar = it
                        else -> err = ErrorInfo(R.string.sync_err_update_kind, name, R.string.sync_hint_update_kind)
                    }
                }
                env.post {
                    if (pkg != null) otaPkg = pkg
                    if (tar != null) {
                        otaTar = tar
                        otaTarSha = null
                    }
                    if (pkg != null || tar != null) otaSentTo = null
                    val p = otaPkg
                    val t = otaTar
                    state = state.copy(ota = if (state.ota.running) state.ota else OtaPhase.Idle, otaPick = OtaPick(
                        version = p?.version, payloadName = p?.payloadName, payloadSize = p?.payloadSize ?: 0,
                        needsFirmware = p?.needsFirmware ?: false, restart = p?.restart,
                        tarName = t?.parts?.last(), tarSize = t?.size ?: 0, error = err))
                }
            } catch (e: Exception) {
                ui { it.copy(otaPick = it.otaPick.copy(error = errorOf(e))) }
            }
        }
    }

    private fun readSmall(item: LocalItem, max: Int): ByteArray {
        if (item.size > max) throw UpdateException(UpdateException.Kind.INVALID, "the update description is too large")
        val out = java.io.ByteArrayOutputStream()
        item.open().use { i ->
            val b = ByteArray(8192)
            while (true) {
                val n = i.read(b)
                if (n < 0) break
                out.write(b, 0, n)
                if (out.size() > max) throw UpdateException(UpdateException.Kind.INVALID, "the update description is too large")
            }
        }
        return out.toByteArray()
    }

    override fun installUpdate(allowDowngrade: Boolean) {
        val s = session ?: return
        val pkg = otaPkg ?: return
        val tar = otaTar ?: return
        if (!state.otaPick.ready) return
        if (!begin()) return
        val skipSend = allowDowngrade && otaSentTo === s
        val total = if (skipSend) 0L else pkg.json.size + tar.size * (if (otaTarSha == null) 2 else 1)
        state = state.copy(ota = if (skipSend) OtaPhase.Checking else OtaPhase.Sending(0, total, UPDATE_JSON))
        env.execute {
            var staged: StagedUpdate? = null
            try {
                fun send() {
                    val pace = Pace()
                    var done = 0L
                    // the payload is hashed first: a tar that is not the signed one is refused before anything is sent
                    val sha = otaTarSha ?: run {
                        val h = hash(tar.open) { n -> if (pace.due()) ui { it.copy(ota = OtaPhase.Sending(n, total, pkg.payloadName)) } }
                        val hex = h.first.joinToString("") { "%02x".format(it) }
                        if (h.second != pkg.payloadSize || hex != pkg.payloadSha256) {
                            throw UpdateException(UpdateException.Kind.PAYLOAD, "${tar.parts.last()} is not the payload of ByOS ${pkg.version}")
                        }
                        otaTarSha = h.first
                        h.first
                    }
                    done += if (total > pkg.json.size + tar.size) tar.size else 0
                    val js = pkg.json
                    putLocal(Ns.UPDATE, UPDATE_JSON, js.size.toLong(), MessageDigest.getInstance("SHA-256").digest(js), 0, true, { off -> openAt({ js.inputStream() }, off) },
                        exact = PutMode.REPLACE) {}
                    done += js.size
                    val base = done
                    putLocal(Ns.UPDATE, pkg.payloadName, tar.size, sha, tar.mtime, true, { off -> openAt(tar.open, off) },
                        exact = PutMode.REPLACE) { n ->
                        if (pace.due()) ui { it.copy(ota = OtaPhase.Sending(base + n, total, pkg.payloadName)) }
                    }
                    otaSentTo = session
                    ui { it.copy(ota = OtaPhase.Checking) }
                }
                // the player's sync service may restart while the payload goes over: the session
                // comes back by itself, or the sending connects again and goes on (the device resumes
                // the tar from its part)
                var attempt = 0
                fun sendSurely() {
                    while (true) {
                        try {
                            send()
                            return
                        } catch (e: SyncException) {
                            if (e.code == SyncException.RECONNECTED && !stop && session != null && attempt < RECONNECT_DELAYS.size) {
                                attempt++
                                continue
                            }
                            if (!lost(e) || stop || current == null || attempt >= RECONNECT_DELAYS.size) throw e
                            attempt = reconnectDuringUpdate(attempt, e)
                        }
                    }
                }
                if (!skipSend) sendSurely()
                staged = try {
                    (session ?: throw SyncException(SyncException.CONNECTION_LOST)).updateStage(allowDowngrade)
                } catch (e: SyncException) {
                    // the description was already moved by an earlier check, or the connection dropped
                    // during the check (it may or may not have run): send everything again, then check
                    if (!(skipSend && e.code == SyncException.E_NOT_STAGED) && e.code != SyncException.RECONNECTED) throw e
                    sendSurely()
                    (session ?: throw SyncException(SyncException.CONNECTION_LOST)).updateStage(allowDowngrade)
                }
                val st = session ?: throw SyncException(SyncException.CONNECTION_LOST)
                val version = staged.version.ifEmpty { pkg.version }
                try {
                    // over Wi-Fi the player always asks the user first (NO_CONFIRM is for USB only)
                    st.updateInstall(allowDowngrade)
                } catch (e: SyncException) {
                    if (e.code != SyncException.E_UNSUPPORTED) throw e
                    ui { it.copy(ota = OtaPhase.StagedOnly(version)) }
                    return@execute
                }
                ui { it.copy(ota = OtaPhase.WaitingUser(version)) }
                watchInstall(st, version, staged.restart)
            } catch (e: Exception) {
                when {
                    e is SyncException && e.code == SyncException.E_CANCELLED -> ui { it.copy(ota = OtaPhase.Idle) }
                    e is UpdateException -> ui { it.copy(ota = OtaPhase.Failed(updateError(e))) }
                    else -> {
                        val err = errorOf(e)
                        if (lost(e)) dropSession()
                        ui {
                            it.copy(link = if (lost(e)) Link.Idle else it.link,
                                ota = OtaPhase.Failed(err, downgrade = e is SyncException && e.code == SyncException.E_DOWNGRADE))
                        }
                    }
                }
            }
        }
    }

    /**
     * Polls UPDATE_STATUS about once a second until the install ends. When ByOS restarts its
     * interface the connection drops after "installing": that is the expected end.
     */
    private fun watchInstall(s: SyncSession, version: String, restart: Int) {
        var last = -1
        val deadline = env.now() + INSTALL_WATCH_MS
        while (true) {
            if (stop) {
                // the install goes on (or still waits) on the player: only the watching stops
                ui { it.copy(ota = OtaPhase.Idle) }
                return
            }
            val u = try {
                s.updateStatus()
            } catch (e: SyncException) {
                if (lost(e) && last >= UpdateProgress.INSTALLING) {
                    finishInstall(version, maxOf(restart, 1))
                    return
                }
                throw e
            }
            val v = u.version.ifEmpty { version }
            when (u.state) {
                UpdateProgress.WAITING_USER -> ui { it.copy(ota = OtaPhase.WaitingUser(v)) }
                UpdateProgress.INSTALLING -> ui { it.copy(ota = OtaPhase.Installing(v, u.permille.coerceIn(0, 1000))) }
                UpdateProgress.DONE -> {
                    finishInstall(v, restart)
                    return
                }
                UpdateProgress.FAILED -> {
                    ui { it.copy(ota = OtaPhase.Failed(errorOf(SyncException(u.error, "update status: failed")))) }
                    return
                }
                UpdateProgress.REFUSED -> {
                    ui { it.copy(ota = OtaPhase.Refused(v)) }
                    return
                }
                else -> if (last >= UpdateProgress.INSTALLING) {
                    finishInstall(v, restart)
                    return
                }
            }
            last = u.state
            if (env.now() > deadline) {
                ui { it.copy(ota = OtaPhase.Failed(ErrorInfo(R.string.sync_err_update_slow, null, R.string.sync_hint_update_slow))) }
                return
            }
            env.sleep(1000)
        }
    }

    /** The install is over: when ByOS restarts, the session is gone and the user connects again later. */
    private fun finishInstall(version: String, restart: Int) {
        if (restart != 0) {
            dropSession()
            ui { it.copy(ota = OtaPhase.Done(version, restart), link = Link.Idle, browser = Browser(), library = LibraryPhase.Idle) }
        } else {
            ui { it.copy(ota = OtaPhase.Done(version, restart)) }
            session?.let { refreshAfterWrite(it) }
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

        /** Waits before each new connection when a transfer lost its own (about a minute in all). */
        val RECONNECT_DELAYS = longArrayOf(2_000, 4_000, 8_000, 15_000, 30_000)

        /** What runs in the background, for the notification of the transfer service. */
        fun workOf(s: SyncState): WorkNote? {
            val t = s.transfer
            val o = s.ota
            val u = s.update
            return when {
                t.running -> WorkNote(WorkNote.Kind.SEND, t.done, t.total, t.current, t.reconnecting)
                o is OtaPhase.Sending -> WorkNote(WorkNote.Kind.UPDATE, o.done, o.total, o.path)
                o is OtaPhase.Installing -> WorkNote(WorkNote.Kind.INSTALL, o.permille.toLong(), 1000, o.version)
                o.running -> WorkNote(WorkNote.Kind.INSTALL, 0, 0, "")
                u is UpdatePhase.Sending -> WorkNote(WorkNote.Kind.UPDATE, u.done, u.total, u.path)
                else -> null
            }
        }

        /** The name of the update description in NS_UPDATE (section 14.1). */
        const val UPDATE_JSON = "hibyos-update.json"

        /** How long the app watches an install (the player's own limits end it much sooner). */
        const val INSTALL_WATCH_MS = 30 * 60_000L

        fun sortEntries(list: List<SyncEntry>): List<SyncEntry> =
            list.sortedWith(compareBy<SyncEntry> { if (it.isFolder) 0 else 1 }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name })

        fun unhex(s: String): ByteArray = ByteArray(s.length / 2) { i -> s.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
    }
}
