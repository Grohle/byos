package org.hibyos.install.connected

import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpAuthException
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpDiscovery
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.BtpLink
import org.hibyos.install.core.btp.BtpReconnectedException
import org.hibyos.install.core.btp.BtpReconnector
import org.hibyos.install.core.btp.BtpSession
import org.hibyos.install.core.btp.DataSink
import org.hibyos.install.core.btp.Found
import org.hibyos.install.core.btp.Hello
import org.hibyos.install.core.btp.Progress
import org.hibyos.install.core.btp.Reached
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.URLDecoder
import java.net.URLEncoder
import java.util.concurrent.SynchronousQueue
import java.util.concurrent.TimeUnit
import org.hibyos.install.core.btp.PairedDevice as CorePaired

/**
 * How the client reaches the local network. On Android every socket is tied to the Wi-Fi
 * network (a phone that prefers mobile data, or a Wi-Fi without internet, would otherwise send
 * the player's traffic over mobile data), and the broadcast addresses and neighbours come from
 * the Wi-Fi link itself.
 */
class SyncNetwork(
    val socket: () -> Socket = { Socket() },
    val datagram: () -> DatagramSocket = { DatagramSocket() },
    /** Where the discovery probe goes. */
    val broadcasts: () -> List<InetAddress> = { BtpDiscovery.broadcastAddresses() },
    /** The addresses the TCP sweep tries when no ANNOUNCE came back (empty: no sweep). */
    val neighbours: () -> List<String> = { emptyList() },
    /** UDP port of the probe (tests use another one). */
    val discoveryPort: Int = Btp.PORT,
    /** TCP port of the sweep. */
    val sweepPort: Int = Btp.PORT,
)

/**
 * [SyncConnector] on the BTP/1 client of the shared core (org.hibyos.install.core.btp).
 *
 * The client keeps its static key in `client.key` and the paired devices in `devices.txt`
 * (one line per device: id, public key, last address, name), both in [dir], which is the app's
 * private storage. [beforeDiscovery]/[afterDiscovery] let Android hold a multicast lock while
 * the UDP answers come in.
 *
 * Sessions with a paired player are opened by [BtpReconnector] and kept by [BtpLink]: the
 * player is looked for by its device id when it is not at its last address, a daemon that is
 * restarting is waited for, and a session whose connection drops comes back by itself.
 */
class CoreSync(
    private val dir: File,
    private val software: String,
    private val beforeDiscovery: () -> Unit = {},
    private val afterDiscovery: () -> Unit = {},
    /** Lets a loopback session marked local by the device skip AUTH (adb forward). Never set by the app. */
    private val allowLocal: Boolean = false,
    private val net: SyncNetwork = SyncNetwork(),
    /** Waits between the attempts to reach a paired player (see [BtpReconnector]). */
    private val delaysMs: LongArray = BtpReconnector.DEFAULT_DELAYS_MS,
    /** The same for a short request of an open session (browsing, PING, DF). */
    private val quickDelaysMs: LongArray = BtpReconnector.QUICK_DELAYS_MS,
) : SyncConnector {
    private class Entry(val core: CorePaired, var host: String?)

    private val lock = Any()

    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    private fun unhex(s: String) = ByteArray(s.length / 2) { i -> s.substring(i * 2, i * 2 + 2).toInt(16).toByte() }

    /** The client's static X25519 secret, made the first time it is needed. */
    private fun secret(): ByteArray = synchronized(lock) {
        val f = File(dir, "client.key")
        if (f.isFile && f.length() == 32L) return f.readBytes()
        dir.mkdirs()
        val k = BtpCrypto.random(32)
        val tmp = File(dir, "client.key.tmp")
        tmp.writeBytes(k)
        if (!tmp.renameTo(f)) throw IOException("the client key could not be saved")
        return k
    }

    private fun load(): MutableList<Entry> = synchronized(lock) {
        val f = File(dir, "devices.txt")
        if (!f.isFile) return ArrayList()
        val out = ArrayList<Entry>()
        for (line in f.readLines()) {
            val p = line.split(' ')
            if (p.size < 4 || p[0].length != 16 || p[1].length != 64) continue
            try {
                out += Entry(CorePaired(unhex(p[0]), unhex(p[1]), URLDecoder.decode(p[3], "UTF-8")), p[2].takeIf { it != "-" })
            } catch (e: Exception) {
                // a damaged line is skipped
            }
        }
        return out
    }

    private fun save(list: List<Entry>) = synchronized(lock) {
        dir.mkdirs()
        val tmp = File(dir, "devices.txt.tmp")
        tmp.writeText(list.joinToString("") {
            "${hex(it.core.deviceId)} ${hex(it.core.publicKey)} ${it.host ?: "-"} ${URLEncoder.encode(it.core.name, "UTF-8")}\n"
        })
        if (!tmp.renameTo(File(dir, "devices.txt"))) {
            File(dir, "devices.txt").delete()
            if (!tmp.renameTo(File(dir, "devices.txt"))) throw IOException("the paired devices could not be saved")
        }
    }

    private fun remember(core: CorePaired, host: String) = synchronized(lock) {
        val list = load()
        list.removeAll { it.core.deviceId.contentEquals(core.deviceId) }
        list += Entry(core, host)
        while (list.size > 16) list.removeAt(0)
        save(list)
    }

    override fun paired(): List<PairedDevice> = load().map { PairedDevice(hex(it.core.deviceId), it.core.name, it.host) }

    override fun forget(deviceId: String) = synchronized(lock) {
        save(load().filter { hex(it.core.deviceId) != deviceId })
    }

    /**
     * UDP discovery first (the probe is repeated while it waits). When no player answers (a
     * network that drops broadcasts, or a daemon that was restarting), the neighbours are tried
     * directly on the BTP port: slower, but it finds the player on any network the phone can
     * reach it on.
     */
    override fun discover(timeoutMs: Int, cancelled: () -> Boolean, onFound: (SyncDevice) -> Unit): List<SyncDevice> {
        val known = load().map { hex(it.core.deviceId) }.toSet()
        val seen = LinkedHashMap<String, SyncDevice>()
        fun add(f: Found) {
            val d = SyncDevice(f.address, f.port, f.deviceIdHex, f.announce.name, f.announce.pairingOpen,
                f.announce.sdMounted, known.contains(f.deviceIdHex))
            val first = synchronized(seen) { seen.put(d.deviceId, d) == null }
            if (first) onFound(d)
        }
        beforeDiscovery()
        try {
            var udpError: IOException? = null
            try {
                BtpDiscovery.probe(timeoutMs, net.broadcasts(), net.discoveryPort, socket = net.datagram, cancelled = cancelled, onFound = ::add)
            } catch (e: IOException) {
                udpError = e
            }
            if (synchronized(seen) { seen.isEmpty() } && !cancelled()) {
                val hosts = try { net.neighbours() } catch (e: Exception) { emptyList() }
                if (hosts.isNotEmpty()) {
                    BtpDiscovery.sweep(hosts, hello("ByOS Install"), net.sweepPort, socketFactory = net.socket, cancelled = cancelled, onFound = ::add)
                } else if (udpError != null) {
                    throw SyncException(SyncException.UNREACHABLE, udpError.message, udpError)
                }
            }
            return synchronized(seen) { seen.values.toList() }
        } finally {
            afterDiscovery()
        }
    }

    /**
     * Looks for one player by its device id: the UDP probe, then (from the second attempt, and
     * only with [sweep]) the sweep. [cancelled] ends both.
     */
    private fun locate(idHex: String, attempt: Int, cancelled: () -> Boolean, sweep: Boolean = true): Found? {
        beforeDiscovery()
        try {
            val want = { f: Found -> f.deviceIdHex.equals(idHex, ignoreCase = true) }
            val udp = try {
                BtpDiscovery.probe(1500, net.broadcasts(), net.discoveryPort, socket = net.datagram, cancelled = cancelled, stopWhen = want)
            } catch (e: IOException) {
                emptyList()
            }
            udp.firstOrNull(want)?.let { return it }
            if (!sweep || attempt < 1 || cancelled()) return null
            val hosts = try { net.neighbours() } catch (e: Exception) { emptyList() }
            return BtpDiscovery.sweep(hosts, hello("ByOS Install"), net.sweepPort, socketFactory = net.socket, cancelled = cancelled,
                stopWhen = want).firstOrNull(want)
        } finally {
            afterDiscovery()
        }
    }

    private fun hello(name: String) = Hello.client(software, name)

    private fun dial(host: String, port: Int, name: String): BtpSession = BtpSession.connect(host, port, hello(name), socketFactory = net.socket)

    private fun open(host: String, port: Int, name: String): BtpSession = wrap { dial(host, port, name) }

    private fun reconnector() = BtpReconnector(dial = { h, p -> dial(h, p, "ByOS Install") },
        locate = { id, attempt, cancelled -> locate(id, attempt, cancelled) }, delaysMs = delaysMs)

    /**
     * For the short requests of an open session: a 4 s connect, the UDP probe only (no sweep of
     * the subnet) and [quickDelaysMs], so that a player that went away is reported in seconds.
     */
    private fun quickReconnector() = BtpReconnector(
        dial = { h, p -> BtpSession.connect(h, p, hello("ByOS Install"), socketFactory = net.socket, connectTimeoutMs = QUICK_CONNECT_MS) },
        locate = { id, attempt, cancelled -> locate(id, attempt, cancelled, sweep = false) }, delaysMs = quickDelaysMs)

    /** A session that comes back by itself, stored under its new address when the player moved. */
    private fun link(core: CorePaired, secret: ByteArray, reached: Reached, r: BtpReconnector = reconnector()) =
        BtpLink(r, core, secret, reached, onMoved = { h, _ ->
            try {
                remember(core, h)
            } catch (e: IOException) {
                // only the remembered address is older
            }
        }, quick = quickReconnector())

    /**
     * Opens a session with a paired player and runs AUTH (8.3).
     *
     * HELLO is not authenticated (section 7), so nothing in it is trusted: its DEVICE_ID is only
     * used to pick the stored keys, and HF_LOCAL never lets a session skip AUTH. The phone always
     * reaches the player over the network, so every session here is authenticated and encrypted.
     * Only with [allowLocal] (adb forward tooling, never the app) and a loopback address may a
     * session that the device marks as local stay in clear.
     */
    override fun connect(host: String, port: Int, expectedId: String?, cancelled: () -> Boolean): SyncSession {
        val exp = expectedId?.lowercase()?.takeIf { it.isNotEmpty() }
        if (exp != null && !(allowLocal && isLoopback(host))) {
            // a player found by discovery that this phone has no keys for goes the single-try way
            // below: its HELLO id is checked against the chosen one before pairing is offered
            val entry = load().firstOrNull { hex(it.core.deviceId) == exp }
            if (entry != null) return reconnectPaired(entry, host.ifEmpty { entry.host ?: "" }.ifEmpty { null }, port, cancelled)
        }
        val s = open(host, port, "ByOS Install")
        try {
            val id = s.device.deviceId?.let { hex(it) }
            if (!expectedId.isNullOrEmpty() && id != expectedId.lowercase()) {
                // the HELLO DEVICE_ID differs from the player the user chose: another device answers there,
                // or the player was reset (8.3)
                throw SyncException(SyncException.IDENTITY_CHANGED, "another device answered at $host")
            }
            val entry = load().firstOrNull { hex(it.core.deviceId) == id } ?: throw SyncException(SyncException.NOT_PAIRED)
            val cleartext = allowLocal && s.local && isLoopback(host)
            if (!cleartext) {
                try {
                    wrap { s.auth(secret(), entry.core) }
                } catch (e: SyncException) {
                    // E_AUTH_FAILED is sent in clear, before any key is proven: anyone could send it.
                    // The keys stay; a new pairing replaces them, an explicit Forget removes them.
                    if (e.code == SyncException.NOT_PAIRED) throw SyncException(SyncException.FORGOTTEN, e.message, e)
                    throw e
                }
            }
            remember(entry.core, host)
            if (cleartext) return CoreSession(null, s, true)
            return CoreSession(link(entry.core, secret(), Reached(s, host, port, 1)), null, false)
        } catch (e: Exception) {
            s.close()
            throw e
        }
    }

    /**
     * A remembered player: tried at its last address, waited for while its daemon restarts,
     * looked for by device id when it moved (BtpReconnector). The errors that remain are the
     * ones the user has to act on: the player forgot this phone (FORGOTTEN), another device
     * answers in its place (IDENTITY_CHANGED), or it is not on this network at all (UNREACHABLE).
     */
    private fun reconnectPaired(entry: Entry, host: String?, port: Int, cancelled: () -> Boolean): SyncSession {
        val secret = secret()
        val r = reconnector()
        val reached = try {
            wrap { r.open(entry.core, secret, host, port, cancelled) }
        } catch (e: SyncException) {
            throw when (e.code) {
                // E_AUTH_FAILED is sent in clear, before any key is proven: anyone could send it.
                // The keys stay; a new pairing replaces them, an explicit Forget removes them.
                SyncException.NOT_PAIRED -> SyncException(SyncException.FORGOTTEN, e.message, e)
                SyncException.CONNECTION_LOST, SyncException.E_TIMEOUT, SyncException.E_PROTO ->
                    SyncException(SyncException.UNREACHABLE, e.message, e)
                else -> e
            }
        }
        try {
            remember(entry.core, reached.host)
        } catch (e: IOException) {
            // the session is fine; only the stored address is older
        }
        return CoreSession(link(entry.core, secret, reached, r), null, false)
    }

    override fun startPairing(host: String, port: Int, clientName: String): Pairing {
        val s = open(host, port, clientName)
        if (!s.pairingOpen) {
            s.close()
            throw SyncException(SyncException.E_PAIR_CLOSED, "Pair new device is not open on the player")
        }
        val codes = SynchronousQueue<String>()
        val asked = SynchronousQueue<Boolean>()
        val result = SynchronousQueue<Result<CorePaired>>()
        // the core asks for the code in the middle of the handshake: it runs on its own thread,
        // and startPairing/confirm hand the code over to it
        val t = Thread({
            val r = runCatching {
                s.pair(secret(), clientName, askCode = {
                    if (!asked.offer(true, 30, TimeUnit.SECONDS)) null
                    else codes.poll(10, TimeUnit.MINUTES)?.ifEmpty { null }
                })
            }
            if (r.isFailure) asked.offer(false)
            // a result nobody waits for (the pairing was abandoned) is dropped
            result.offer(r, 150, TimeUnit.SECONDS)
        }, "hibyos-pair")
        t.isDaemon = true
        t.start()
        // returns when the device shows its code, or with the error that came first
        val ok = asked.poll(30, TimeUnit.SECONDS)
        if (ok != true) {
            val r = result.poll(2, TimeUnit.SECONDS)
            s.close()
            val e = r?.exceptionOrNull()
            throw if (e != null) mapped(e) else SyncException(SyncException.E_TIMEOUT, "the player did not answer the pairing")
        }
        return object : Pairing {
            private var done = false

            override fun confirm(code: String, cancelled: () -> Boolean): SyncSession {
                if (done) throw SyncException(SyncException.E_CANCELLED)
                done = true
                // the pairing thread may have given up already (it waits 10 minutes for a code):
                // the hand-over is bounded so that the sync worker never blocks for good
                if (!codes.offer(code.filter { it.isDigit() }, 5, TimeUnit.SECONDS)) {
                    val r = result.poll(2, TimeUnit.SECONDS)
                    s.close()
                    throw r?.exceptionOrNull()?.let { mapped(it) }
                        ?: SyncException(SyncException.E_TIMEOUT, "the pairing ended before the code was given")
                }
                while (true) {
                    if (cancelled()) {
                        s.close()
                        throw SyncException(SyncException.E_CANCELLED)
                    }
                    val r = result.poll(250, TimeUnit.MILLISECONDS) ?: continue
                    val p = r.getOrElse {
                        s.close()
                        throw mapped(it)
                    }
                    remember(p, host)
                    return CoreSession(link(p, secret(), Reached(s, host, port, 1)), null, false)
                }
            }

            override fun close() {
                if (!done) {
                    done = true
                    codes.offer("")
                }
                s.close()
            }
        }
    }

    companion object {
        /** TCP connect and HELLO limit of the quick reconnection. */
        const val QUICK_CONNECT_MS = 4_000

        fun isLoopback(host: String): Boolean = try {
            InetAddress.getByName(host).isLoopbackAddress
        } catch (e: Exception) {
            false
        }

        fun mapped(e: Throwable): SyncException = when (e) {
            is SyncException -> e
            is BtpAuthException -> when (e.reason) {
                BtpAuthException.Reason.CODE_MISMATCH -> SyncException(SyncException.WRONG_CODE, e.message, e)
                BtpAuthException.Reason.NO_CODE -> SyncException(SyncException.E_CANCELLED, e.message, e)
                BtpAuthException.Reason.IDENTITY_CHANGED, BtpAuthException.Reason.DIFFERENT_DEVICE ->
                    SyncException(SyncException.IDENTITY_CHANGED, e.message, e)
                BtpAuthException.Reason.UNKNOWN_CLIENT -> SyncException(SyncException.NOT_PAIRED, e.message, e)
                BtpAuthException.Reason.UNSUPPORTED -> SyncException(SyncException.E_UNSUPPORTED, e.message, e)
                BtpAuthException.Reason.BAD_KEY -> SyncException(SyncException.E_AUTH_FAILED, e.message, e)
            }
            is BtpException -> SyncException(e.code, e.message, e)
            is SocketTimeoutException -> SyncException(SyncException.E_TIMEOUT, e.message, e)
            is BtpConnectionException ->
                if (e.message?.startsWith("cannot connect") == true) SyncException(SyncException.UNREACHABLE, e.message, e)
                else SyncException(SyncException.CONNECTION_LOST, e.message, e)
            is IOException -> SyncException(SyncException.CONNECTION_LOST, e.message, e)
            else -> SyncException(SyncException.E_PROTO, e.message ?: e.javaClass.simpleName, e)
        }

        /** Runs a core call and turns its errors into SyncException. Local read errors stay IOException. */
        inline fun <T> wrap(f: () -> T): T = try {
            f()
        } catch (e: SyncException) {
            throw e
        } catch (e: LocalReadException) {
            throw e.cause as IOException
        } catch (e: Exception) {
            throw mapped(e)
        }
    }
}

/** A read error of a file on the phone, kept apart from the network errors during a PUT. */
class LocalReadException(cause: IOException) : RuntimeException(cause)

/**
 * A session for the screens. Over the network it runs on a [BtpLink]: when the connection drops,
 * a new session is opened (AUTH, no code) and the requests that can run twice are repeated; the
 * others throw RECONNECTED. A cleartext loopback session ([plain], adb forward tooling only)
 * has no link.
 */
private class CoreSession(private val link: BtpLink?, private val plain: BtpSession?, cleartext: Boolean) : SyncSession {
    private val first: BtpSession = link?.session ?: plain!!

    override val info = SyncDeviceInfo(
        deviceId = first.device.deviceId?.joinToString("") { "%02x".format(it) } ?: "",
        name = first.device.name ?: "",
        byosVersion = first.device.byosVersion,
        firmware = first.device.firmware,
        local = cleartext,
        sdMounted = first.sdMounted,
        shellAttached = first.shellAttached,
        caps = first.device.caps,
    )

    override val host: String? get() = link?.host

    @Volatile private var listener: (Int, Int) -> Unit = { _, _ -> }
    @Volatile private var abort: () -> Boolean = { false }

    override fun onReconnect(listener: (attempt: Int, of: Int) -> Unit) {
        this.listener = listener
    }

    override fun cancelWhen(cancelled: () -> Boolean) {
        abort = cancelled
    }

    /**
     * Runs [f] on the session; on a lost connection, see [BtpLink.call]. [quick]: a short
     * request, reconnected with fewer, shorter attempts. The reconnection stops when [cancelled]
     * or the session's own cancel ([cancelWhen]) says so.
     */
    private fun <T> run(retry: Boolean, cancelled: (() -> Boolean)? = null, quick: Boolean = false, f: (BtpSession) -> T): T {
        val l = link ?: return CoreSync.wrap { f(plain!!) }
        val stop = { cancelled?.invoke() == true || abort() }
        var told = false
        try {
            return CoreSync.wrap {
                try {
                    l.call(retry, stop, onRetry = { n, of ->
                        told = true
                        listener(n, of)
                    }, quick = quick, f = f)
                } catch (e: BtpReconnectedException) {
                    throw SyncException(SyncException.RECONNECTED, e.message, e)
                }
            }
        } finally {
            if (told) listener(0, 0)
        }
    }

    override fun df(): SyncSpace = run(true, quick = true) { s ->
        val d = s.df()
        SyncSpace(d.sdState, d.sdFs, d.sdTotal, d.sdFree, d.dataTotal, d.dataFree)
    }

    override fun list(path: String): List<SyncEntry> = run(true, quick = true) { s -> s.list(path).map { SyncEntry(it.name, it.kind, it.size, it.mtime) } }

    override fun stat(path: String): SyncEntry? = run(true, quick = true) { s ->
        s.statOrNull(path)?.let { SyncEntry(SyncPaths.name(path), it.kind, it.size, it.mtime) }
    }

    // with parents, MKDIR of a folder that is there is fine: it can run twice
    override fun mkdir(path: String, parents: Boolean) = run(parents, quick = true) { s -> s.mkdir(path, parents) }
    override fun move(src: String, dst: String, replace: Boolean) = run(false, quick = true) { s -> s.move(src, dst, replace) }
    override fun delete(path: String, recursive: Boolean) = run(false, quick = true) { s -> s.delete(path, recursive) }

    /**
     * A PUT that lost its connection is sent again on the new session: the device resumes it from
     * the part it kept (section 11.3). When the first try had sent everything and only PUT_DONE
     * was lost, the second try finds the file identical: it was written by this transfer, so it
     * is reported as written, not as "already there".
     */
    override fun put(ns: Int, path: String, size: Long, sha256: ByteArray, mtime: Long, mode: PutMode,
                     open: (offset: Long) -> InputStream, cancelled: () -> Boolean, progress: (done: Long) -> Unit): PutResult {
        var sentAll = false
        val r = run(true, cancelled) { s ->
            s.put(path, size, sha256, mode.code, mtime, ns, cancelled, Progress { done, _ ->
                if (size > 0 && done >= size) sentAll = true
                progress(done)
            }) { off ->
                val i = try {
                    open(off)
                } catch (e: IOException) {
                    throw LocalReadException(e)
                }
                LocalStream(i)
            }
        }
        return PutResult(r.result == 1 && !sentAll, r.size)
    }

    /** A GET that lost its connection goes on from the bytes already written to [out]. */
    override fun get(path: String, offset: Long, out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): GetResult {
        var pos = offset
        val r = run(true, cancelled) { s ->
            s.get(path, pos, sha = true, cancelled = cancelled, progress = Progress { d, t -> progress(d, t) }) {
                DataSink { b, off, len ->
                    out.write(b, off, len)
                    pos += len
                }
            }
        }
        return GetResult(r.info.size, r.sha256)
    }

    // a snapshot cannot go on half way (the device makes a new one): the caller asks again
    override fun dbSnapshot(out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): ByteArray =
        run(false, cancelled) { s ->
            s.dbSnapshot(cancelled, Progress { d, t -> progress(d, t) }) { DataSink { b, off, len -> out.write(b, off, len) } }.sha256
        }

    override fun libRescan(paths: List<String>?) = run(true, quick = true) { s -> s.libRescan(paths) }

    override fun updateStage(allowDowngrade: Boolean): StagedUpdate = run(false) { s ->
        val r = s.updateStage(allowDowngrade)
        StagedUpdate(r.version, r.restart, r.needsFirmware != 0, r.size)
    }

    override fun updateInstall(allowDowngrade: Boolean) {
        run(false) { s -> s.updateInstall(noConfirm = false, allowDowngrade = allowDowngrade) }
    }

    override fun ping() = run(true, quick = true) { s -> s.ping() }

    override fun updateStatus(): UpdateProgress = run(true, quick = true) { s ->
        val r = s.updateStatus()
        UpdateProgress(r.state, r.permille, r.error, r.version)
    }

    override fun close() {
        if (link != null) link.close() else plain?.close()
    }
}

/** Marks read errors of the phone's file so that they are not taken for a broken connection. */
private class LocalStream(private val i: InputStream) : InputStream() {
    private fun <T> local(f: () -> T): T = try {
        f()
    } catch (e: IOException) {
        throw LocalReadException(e)
    }

    override fun read(): Int = local { i.read() }
    override fun read(b: ByteArray, off: Int, len: Int): Int = local { i.read(b, off, len) }
    override fun close() = i.close()
}
