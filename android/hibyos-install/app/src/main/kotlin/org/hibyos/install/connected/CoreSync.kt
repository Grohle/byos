package org.hibyos.install.connected

import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpAuthException
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpDiscovery
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.BtpSession
import org.hibyos.install.core.btp.DataSink
import org.hibyos.install.core.btp.Hello
import org.hibyos.install.core.btp.Progress
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.net.URLDecoder
import java.net.URLEncoder
import java.util.concurrent.SynchronousQueue
import java.util.concurrent.TimeUnit
import org.hibyos.install.core.btp.PairedDevice as CorePaired

/**
 * [SyncConnector] on the BTP/1 client of the shared core (org.hibyos.install.core.btp).
 *
 * The client keeps its static key in `client.key` and the paired devices in `devices.txt`
 * (one line per device: id, public key, last address, name), both in [dir], which is the app's
 * private storage. [beforeDiscovery]/[afterDiscovery] let Android hold a multicast lock while
 * the UDP answers come in.
 */
class CoreSync(
    private val dir: File,
    private val software: String,
    private val beforeDiscovery: () -> Unit = {},
    private val afterDiscovery: () -> Unit = {},
    /** Lets a loopback session marked local by the device skip AUTH (adb forward). Never set by the app. */
    private val allowLocal: Boolean = false,
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

    override fun discover(timeoutMs: Int, cancelled: () -> Boolean, onFound: (SyncDevice) -> Unit): List<SyncDevice> {
        val known = load().map { hex(it.core.deviceId) }.toSet()
        beforeDiscovery()
        try {
            return BtpDiscovery.probe(timeoutMs).map {
                SyncDevice(it.address, it.announce.tcpPort, it.deviceIdHex, it.announce.name, it.announce.pairingOpen,
                    it.announce.sdMounted, known.contains(it.deviceIdHex)).also(onFound)
            }
        } catch (e: IOException) {
            throw SyncException(SyncException.UNREACHABLE, e.message, e)
        } finally {
            afterDiscovery()
        }
    }

    private fun hello(name: String) = Hello.client(software, name)

    private fun open(host: String, port: Int, name: String): BtpSession = wrap { BtpSession.connect(host, port, hello(name)) }

    /**
     * Opens a session with a paired player and runs AUTH (8.3).
     *
     * HELLO is not authenticated (section 7), so nothing in it is trusted: its DEVICE_ID is only
     * used to pick the stored keys, and HF_LOCAL never lets a session skip AUTH. The phone always
     * reaches the player over the network, so every session here is authenticated and encrypted.
     * Only with [allowLocal] (adb forward tooling, never the app) and a loopback address may a
     * session that the device marks as local stay in clear.
     */
    override fun connect(host: String, port: Int, expectedId: String?): SyncSession {
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
            return CoreSession(s, cleartext)
        } catch (e: Exception) {
            s.close()
            throw e
        }
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
                    return CoreSession(s, false)
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

private class CoreSession(private val s: BtpSession, cleartext: Boolean) : SyncSession {
    override val info = SyncDeviceInfo(
        deviceId = s.device.deviceId?.joinToString("") { "%02x".format(it) } ?: "",
        name = s.device.name ?: "",
        byosVersion = s.device.byosVersion,
        firmware = s.device.firmware,
        local = cleartext,
        sdMounted = s.sdMounted,
        shellAttached = s.shellAttached,
        caps = s.device.caps,
    )

    override fun df(): SyncSpace = CoreSync.wrap {
        val d = s.df()
        SyncSpace(d.sdState, d.sdFs, d.sdTotal, d.sdFree, d.dataTotal, d.dataFree)
    }

    override fun list(path: String): List<SyncEntry> = CoreSync.wrap { s.list(path).map { SyncEntry(it.name, it.kind, it.size, it.mtime) } }

    override fun stat(path: String): SyncEntry? = CoreSync.wrap {
        s.statOrNull(path)?.let { SyncEntry(SyncPaths.name(path), it.kind, it.size, it.mtime) }
    }

    override fun mkdir(path: String, parents: Boolean) = CoreSync.wrap { s.mkdir(path, parents) }
    override fun move(src: String, dst: String, replace: Boolean) = CoreSync.wrap { s.move(src, dst, replace) }
    override fun delete(path: String, recursive: Boolean) = CoreSync.wrap { s.delete(path, recursive) }

    override fun put(ns: Int, path: String, size: Long, sha256: ByteArray, mtime: Long, mode: PutMode,
                     open: (offset: Long) -> InputStream, cancelled: () -> Boolean, progress: (done: Long) -> Unit): PutResult =
        CoreSync.wrap {
            val r = s.put(path, size, sha256, mode.code, mtime, ns, cancelled, Progress { done, _ -> progress(done) }) { off ->
                val i = try {
                    open(off)
                } catch (e: IOException) {
                    throw LocalReadException(e)
                }
                LocalStream(i)
            }
            PutResult(r.result == 1, r.size)
        }

    override fun get(path: String, offset: Long, out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): GetResult =
        CoreSync.wrap {
            val r = s.get(path, offset, sha = true, cancelled = cancelled, progress = Progress { d, t -> progress(d, t) }) {
                DataSink { b, off, len -> out.write(b, off, len) }
            }
            GetResult(r.info.size, r.sha256)
        }

    override fun dbSnapshot(out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): ByteArray =
        CoreSync.wrap {
            s.dbSnapshot(cancelled, Progress { d, t -> progress(d, t) }) { DataSink { b, off, len -> out.write(b, off, len) } }.sha256
        }

    override fun libRescan(paths: List<String>?) = CoreSync.wrap { s.libRescan(paths) }

    override fun updateStage(allowDowngrade: Boolean): StagedUpdate = CoreSync.wrap {
        val r = s.updateStage(allowDowngrade)
        StagedUpdate(r.version, r.restart, r.needsFirmware != 0, r.size)
    }

    override fun updateInstall(allowDowngrade: Boolean) {
        CoreSync.wrap { s.updateInstall(noConfirm = false, allowDowngrade = allowDowngrade) }
    }

    override fun ping() = CoreSync.wrap { s.ping() }

    override fun updateStatus(): UpdateProgress = CoreSync.wrap {
        val r = s.updateStatus()
        UpdateProgress(r.state, r.permille, r.error, r.version)
    }

    override fun close() = s.close()
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
