package org.hibyos.install.core.btp

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.Socket

/** Pairing or AUTH could not be completed; [reason] is stable for programs. */
class BtpAuthException(val reason: Reason, message: String) : Exception(message) {
    enum class Reason {
        /** The code the user typed is not the one shown on the HiBy. */
        CODE_MISMATCH,
        /** The user gave no code. */
        NO_CODE,
        /** AUTH_OK did not open: the HiBy does not hold the key it had when paired. */
        IDENTITY_CHANGED,
        /** HELLO announced another device id than the stored one. */
        DIFFERENT_DEVICE,
        /** The device does not know this client (forgotten or never paired). */
        UNKNOWN_CLIENT,
        /** Low-order point or other key problem. */
        BAD_KEY,
        /** The device does not offer pairing or encryption. */
        UNSUPPORTED,
    }
}

/** What a paired client keeps about a device. */
class PairedDevice(val deviceId: ByteArray, val publicKey: ByteArray, val name: String)

class PutOutcome(
    /** 0 written, 1 skipped (identical). */
    val result: Int,
    val size: Long,
    /** Offset the device resumed from (0 for a fresh upload). */
    val resumedFrom: Long,
)

class GetInfo(val size: Long, val mtime: Long, val offset: Long)

class GetOutcome(val info: GetInfo, val sha256: ByteArray)

/** Receives downloaded bytes. */
fun interface DataSink {
    fun write(b: ByteArray, off: Int, len: Int)
}

fun interface Progress {
    fun update(done: Long, total: Long)
}

/**
 * The client side of one BTP/1 session (docs/SYNC_PROTOCOL.md): HELLO, optional pairing or
 * AUTH, then requests executed one at a time. Not thread-safe, except that the cancel checks
 * may read a flag set by another thread.
 *
 * [random] supplies key material; tests pass fixed bytes to reproduce the vectors.
 */
class BtpSession private constructor(
    private val closeable: Closeable?,
    private val socket: Socket?,
    val channel: FrameChannel,
    /** HELLO payloads exactly as sent and received (they enter the transcript hashes). */
    val helloClientPayload: ByteArray,
    val helloDevicePayload: ByteArray,
    private val random: (Int) -> ByteArray,
) : Closeable {
    val device: Hello = Hello.decode(helloDevicePayload)

    /**
     * The device's HELLO claims a loopback session (USB through adb forward). This is only the
     * peer's word: anyone answering on the network can set it. Callers decide whether a
     * session may skip AUTH from the transport they chose (adb forward to 127.0.0.1), never
     * from this flag alone, and treat it as a protocol error on a network connection.
     */
    val deviceSaysLocal: Boolean get() = device.flags and Btp.HF_LOCAL != 0

    @Deprecated("the peer's claim only; decide from the transport you chose", ReplaceWith("deviceSaysLocal"))
    val local: Boolean get() = deviceSaysLocal
    val pairingOpen: Boolean get() = device.flags and Btp.HF_PAIRING_OPEN != 0
    val sdMounted: Boolean get() = device.flags and Btp.HF_SD_MOUNTED != 0
    val shellAttached: Boolean get() = device.flags and Btp.HF_SHELL_ATTACHED != 0
    val encrypted: Boolean get() = channel.encrypted
    var secured: Boolean = false
        private set

    private var nextStream = 1
    private var closed = false

    fun hasCap(cap: Int) = device.caps and cap != 0

    private fun newStream(): Int {
        val s = nextStream
        nextStream = if (s >= 65535) 1 else s + 1
        return s
    }

    // ------------------------------------------------------------------ plumbing

    /** Next frame of [stream]; answers PING, turns ERROR into [BtpException]. */
    private fun readFor(stream: Int): Frame {
        while (true) {
            val f = channel.receive()
            if (f.stream == 0) {
                when (f.type) {
                    Btp.PING -> { channel.send(Frame(Btp.PONG, 0, f.payload)); continue }
                    Btp.PONG -> continue
                    Btp.BYE -> throw BtpConnectionException("the device closed the session")
                    Btp.ERROR -> {
                        val e = ErrorMsg.decode(f.payload)
                        throw BtpException(e.code, e.message, remote = true)
                    }
                }
                if (stream != 0) throw BtpException(Btp.E_PROTO, "unexpected ${Btp.TYPE_NAMES[f.type] ?: f.type} on stream 0")
            }
            if (f.stream != stream) continue // late frames of a cancelled stream
            if (f.type == Btp.ERROR) {
                val e = ErrorMsg.decode(f.payload)
                throw BtpException(e.code, e.message, remote = true)
            }
            return f
        }
    }

    /**
     * Runs [f] with a longer read timeout: replies that follow device-side hashing (resume,
     * SKIP_IDENTICAL, the whole-file hash of a resumed GET, snapshot and update checks) can
     * take longer than the normal timeout on a large file.
     */
    private fun <T> slow(f: () -> T): T {
        val s = socket ?: return f()
        val old = s.soTimeout
        if (old == 0 || old >= SLOW_TIMEOUT_MS) return f()
        s.soTimeout = SLOW_TIMEOUT_MS
        try {
            return f()
        } finally {
            try { s.soTimeout = old } catch (e: IOException) {}
        }
    }

    private fun expect(f: Frame, type: Int, end: Boolean?): Frame {
        if (f.type != type) throw BtpException(Btp.E_PROTO, "expected ${Btp.TYPE_NAMES[type]}, got ${Btp.TYPE_NAMES[f.type] ?: f.type}")
        if (end != null && f.end != end) throw BtpException(Btp.E_PROTO, "bad END flag on ${Btp.TYPE_NAMES[type]}")
        return f
    }

    private fun call(type: Int, payload: ByteArray, reply: Int): ByteArray {
        val s = newStream()
        channel.send(Frame(type, s, payload))
        return expect(readFor(s), reply, true).payload
    }

    /** Sends CANCEL for [stream] and waits for its terminal frame. Always throws. */
    private fun cancelStream(stream: Int): Nothing {
        channel.send(Frame(Btp.CANCEL, stream, ByteArray(0)))
        while (true) {
            val f = try {
                readFor(stream)
            } catch (e: BtpException) {
                if (e.remote) throw BtpException(Btp.E_CANCELLED, "cancelled", remote = false)
                throw e
            }
            if (f.end) throw BtpException(Btp.E_CANCELLED, "cancelled")
        }
    }

    // ------------------------------------------------------------------ pairing and AUTH (section 8)

    private fun dh(a: ByteArray, b: ByteArray): ByteArray {
        val r = BtpCrypto.x25519(a, b)
        if (BtpCrypto.isAllZero(r)) throw BtpAuthException(BtpAuthException.Reason.BAD_KEY, "the device sent a weak key")
        return r
    }

    /**
     * Pairs this client with the device (8.2). [askCode] is called after the key exchange
     * with the device name; it returns the 6 digits the user read on the HiBy, or null.
     * [onWaitingForAllow] is called while the HiBy asks its user. [allowWaitMs] bounds that wait.
     */
    fun pair(clientSecret: ByteArray, clientName: String, askCode: (String) -> String?,
             onWaitingForAllow: () -> Unit = {}, allowWaitMs: Int = 130_000): PairedDevice {
        if (!hasCap(Btp.CAP_PAIRING) || !hasCap(Btp.CAP_ENCRYPTION)) {
            throw BtpAuthException(BtpAuthException.Reason.UNSUPPORTED, "the device does not offer pairing")
        }
        val cs = clientSecret
        val cS = BtpCrypto.x25519Base(cs)
        val ce = random(32)
        val nc = random(16)
        try {
            val cE = BtpCrypto.x25519Base(ce)
            val commit = BtpCrypto.sha256("BTP/1 commit".toByteArray(), cE, nc)
            channel.send(Frame(Btp.PAIR_COMMIT, 0, commit))
            val pd = PayloadReader(expect(readFor(0), Btp.PAIR_DEVICE, null).payload)
            val dE = pd.bytes(32)
            val nd = pd.bytes(16)
            pd.done()
            channel.send(Frame(Btp.PAIR_REVEAL, 0, PayloadWriter().bytes(cE).bytes(nc).build()))
            val th = BtpCrypto.sha256("BTP/1 pair".toByteArray(), BtpCrypto.sha256(helloClientPayload),
                BtpCrypto.sha256(helloDevicePayload), commit, dE, nd, cE, nc)
            val ss = dh(ce, dE)
            val prk = BtpCrypto.hkdfExtract(th, ss)
            val sas = sasFrom(prk)
            val c2d = BtpCrypto.hkdfExpand(prk, "BTP/1 c2d".toByteArray(), 32)
            val d2c = BtpCrypto.hkdfExpand(prk, "BTP/1 d2c".toByteArray(), 32)
            channel.secure(c2d, d2c)
            c2d.fill(0); d2c.fill(0); ss.fill(0); prk.fill(0)
            val typed = askCode(device.name ?: "HiBy")?.filter { it.isDigit() }
            if (typed == null || typed.isEmpty()) {
                bye()
                throw BtpAuthException(BtpAuthException.Reason.NO_CODE, "no pairing code was given")
            }
            if (!BtpCrypto.equal(typed.toByteArray(), sas.toByteArray())) {
                bye()
                throw BtpAuthException(BtpAuthException.Reason.CODE_MISMATCH, "the code does not match the one on the HiBy")
            }
            channel.send(Frame(Btp.PAIR_CONFIRM, 0, PayloadWriter().bytes(cS).str16(BtpPaths.clipUtf8(clientName, Btp.MAX_NAME)).build()))
            onWaitingForAllow()
            val old = socket?.soTimeout
            socket?.soTimeout = allowWaitMs
            val res = try {
                expect(readFor(0), Btp.PAIR_RESULT, null).payload
            } finally {
                if (old != null) socket?.soTimeout = old
            }
            val r = PayloadReader(res)
            if (r.u8() != 0) throw BtpException(Btp.E_PROTO, "unknown pairing status")
            val dS = r.bytes(32)
            val id = r.bytes(8)
            val name = r.str16()
            r.done()
            if (!BtpCrypto.equal(id, BtpCrypto.sha256(dS).copyOf(8))) throw BtpException(Btp.E_PROTO, "device id does not match its key")
            secured = true
            return PairedDevice(id, dS, name)
        } finally {
            ce.fill(0)
        }
    }

    /**
     * Reconnects a paired client (8.3). [expected] is what was stored at pairing time.
     */
    fun auth(clientSecret: ByteArray, expected: PairedDevice) {
        if (!hasCap(Btp.CAP_PAIRING) || !hasCap(Btp.CAP_ENCRYPTION)) {
            throw BtpAuthException(BtpAuthException.Reason.UNSUPPORTED, "the device does not offer encrypted sessions")
        }
        val hid = device.deviceId
        if (hid == null || !hid.contentEquals(expected.deviceId)) {
            throw BtpAuthException(BtpAuthException.Reason.DIFFERENT_DEVICE, "different device or the HiBy was reset; pair again")
        }
        val cS = BtpCrypto.x25519Base(clientSecret)
        val keyId = BtpCrypto.sha256(cS).copyOf(16)
        val ce = random(32)
        try {
            val cE = BtpCrypto.x25519Base(ce)
            channel.send(Frame(Btp.AUTH_INIT, 0, PayloadWriter().bytes(keyId).bytes(cE).build()))
            val reply = try {
                expect(readFor(0), Btp.AUTH_REPLY, null).payload
            } catch (e: BtpException) {
                if (e.remote && e.code == Btp.E_AUTH_FAILED) {
                    throw BtpAuthException(BtpAuthException.Reason.UNKNOWN_CLIENT, "the HiBy does not know this computer; pair again")
                }
                throw e
            }
            val r = PayloadReader(reply)
            val dE = r.bytes(32)
            r.done()
            val ee = dh(ce, dE)
            val es = dh(clientSecret, dE)
            val se = dh(ce, expected.publicKey)
            val th = BtpCrypto.sha256("BTP/1 auth".toByteArray(), BtpCrypto.sha256(helloClientPayload),
                BtpCrypto.sha256(helloDevicePayload), keyId, cE, dE, cS, expected.publicKey)
            val prk = BtpCrypto.hkdfExtract(th, ee + es + se)
            val c2d = BtpCrypto.hkdfExpand(prk, "BTP/1 c2d".toByteArray(), 32)
            val d2c = BtpCrypto.hkdfExpand(prk, "BTP/1 d2c".toByteArray(), 32)
            channel.secure(c2d, d2c)
            c2d.fill(0); d2c.fill(0); prk.fill(0); ee.fill(0); es.fill(0); se.fill(0)
            channel.send(Frame(Btp.AUTH_FINISH, 0, ByteArray(0)))
            try {
                expect(readFor(0), Btp.AUTH_OK, null)
            } catch (e: BtpRecordAuthException) {
                throw identityChanged()
            } catch (e: BtpConnectionException) {
                // a clean close right after AUTH_FINISH: the device could not open it. Timeouts
                // and resets stay connection errors (a Wi-Fi drop is not an impostor).
                if (e.cause is java.io.EOFException) throw identityChanged()
                throw e
            }
            secured = true
        } finally {
            ce.fill(0)
        }
    }

    private fun identityChanged() = BtpAuthException(BtpAuthException.Reason.IDENTITY_CHANGED,
        "the HiBy could not prove its identity (it was reset, or this is not the paired device)")

    // ------------------------------------------------------------------ requests

    fun ping(data: ByteArray = random(8)) {
        channel.send(Frame(Btp.PING, 0, data))
        while (true) {
            val f = channel.receive()
            if (f.stream == 0 && f.type == Btp.PONG) {
                if (!f.payload.contentEquals(data)) throw BtpException(Btp.E_PROTO, "PONG does not echo PING")
                return
            }
            if (f.stream == 0 && f.type == Btp.ERROR) {
                val e = ErrorMsg.decode(f.payload)
                throw BtpException(e.code, e.message, remote = true)
            }
        }
    }

    fun stat(path: String, ns: Int = Btp.NS_SD): StatR = StatR.decode(call(Btp.STAT, Payloads.nsPath(ns, path), Btp.STAT_R))

    /** STAT that returns null for E_NOT_FOUND. */
    fun statOrNull(path: String, ns: Int = Btp.NS_SD): StatR? = try {
        stat(path, ns)
    } catch (e: BtpException) {
        if (e.remote && e.code == Btp.E_NOT_FOUND) null else throw e
    }

    fun list(path: String, hidden: Boolean = false, ns: Int = Btp.NS_SD): List<ListEntry> {
        val s = newStream()
        channel.send(Frame(Btp.LIST, s, Payloads.nsFlagsPath(ns, if (hidden) 1 else 0, path)))
        val all = ArrayList<ListEntry>()
        while (true) {
            val f = expect(readFor(s), Btp.LIST_R, null)
            all += ListR.decode(f.payload)
            if (f.end) return all
        }
    }

    fun df(): DfR = DfR.decode(call(Btp.DF, ByteArray(0), Btp.DF_R))

    fun mkdir(path: String, parents: Boolean, ns: Int = Btp.NS_SD) {
        call(Btp.MKDIR, Payloads.nsFlagsPath(ns, if (parents) 1 else 0, path), Btp.OK)
    }

    fun move(src: String, dst: String, replace: Boolean, ns: Int = Btp.NS_SD) {
        call(Btp.MOVE, Payloads.move(ns, if (replace) 1 else 0, src, dst), Btp.OK)
    }

    fun delete(path: String, recursive: Boolean, ns: Int = Btp.NS_SD) {
        call(Btp.DELETE, Payloads.nsFlagsPath(ns, if (recursive) 1 else 0, path), Btp.OK)
    }

    /**
     * Uploads a file (section 11). [open] returns the content starting at the given offset.
     * The SHA-256 must be computed beforehand (the device checks it at PUT_END).
     */
    fun put(path: String, size: Long, sha256: ByteArray, mode: Int = Btp.MODE_CREATE, mtime: Long = 0,
            ns: Int = Btp.NS_SD, cancelled: () -> Boolean = { false }, progress: Progress? = null,
            open: (Long) -> InputStream): PutOutcome {
        require(sha256.size == 32)
        val s = newStream()
        channel.send(Frame(Btp.PUT_BEGIN, s, PutBegin(ns, mode, size, mtime, sha256, path).encode()))
        val first = slow { readFor(s) }
        if (first.type == Btp.PUT_DONE) {
            expect(first, Btp.PUT_DONE, true)
            val r = PayloadReader(first.payload)
            val out = PutOutcome(r.u8(), r.u64(), 0)
            r.done()
            return out
        }
        val ready = PayloadReader(expect(first, Btp.PUT_READY, false).payload)
        val offset = ready.u64()
        ready.done()
        if (offset < 0 || offset > size) throw BtpException(Btp.E_PROTO, "bad resume offset $offset")
        progress?.update(offset, size)
        val buf = ByteArray(Btp.MAX_DATA)
        var pos = offset
        open(offset).use { inp ->
            while (pos < size) {
                if (cancelled()) { channel.flush(); cancelStream(s) }
                while (channel.hasInput()) pollDuringUpload(s)
                val n = minOf(buf.size.toLong(), size - pos).toInt()
                var got = 0
                while (got < n) {
                    val k = inp.read(buf, got, n - got)
                    if (k < 0) throw IOException("the local file became shorter while it was sent")
                    got += k
                }
                channel.send(Frame(Btp.DATA, s, Payloads.data(pos, buf, 0, n)), flush = false)
                pos += n
                progress?.update(pos, size)
            }
        }
        if (cancelled()) { channel.flush(); cancelStream(s) }
        channel.send(Frame(Btp.PUT_END, s, ByteArray(0)))
        val done = PayloadReader(expect(slow { readFor(s) }, Btp.PUT_DONE, true).payload)
        val out = PutOutcome(done.u8(), done.u64(), offset)
        done.done()
        return out
    }

    /**
     * Reads exactly one waiting frame during an upload without blocking for stream [s]:
     * PING/PONG on stream 0 are handled and the upload goes on; an ERROR (on [s] or stream 0)
     * or BYE ends it.
     */
    private fun pollDuringUpload(s: Int) {
        channel.flush()
        val f = channel.receive()
        if (f.stream == 0) {
            when (f.type) {
                Btp.PING -> { channel.send(Frame(Btp.PONG, 0, f.payload)); return }
                Btp.PONG -> return
                Btp.BYE -> throw BtpConnectionException("the device closed the session")
                Btp.ERROR -> { val e = ErrorMsg.decode(f.payload); throw BtpException(e.code, e.message, remote = true) }
                else -> throw BtpException(Btp.E_PROTO, "unexpected ${Btp.TYPE_NAMES[f.type] ?: f.type} on stream 0")
            }
        }
        if (f.stream != s) return // a late frame of an earlier, cancelled stream
        if (f.type == Btp.ERROR) { val e = ErrorMsg.decode(f.payload); throw BtpException(e.code, e.message, remote = true) }
        throw BtpException(Btp.E_PROTO, "unexpected ${Btp.TYPE_NAMES[f.type] ?: f.type} during upload")
    }

    private fun receiveFile(s: Int, cancelled: () -> Boolean, progress: Progress?,
                            onStart: (GetInfo) -> DataSink): GetOutcome {
        val r = PayloadReader(expect(slow { readFor(s) }, Btp.GET_R, false).payload)
        val info = GetInfo(r.u64(), r.i64(), r.u64())
        r.done()
        if (info.offset > info.size) throw BtpException(Btp.E_PROTO, "bad GET_R offset")
        val sink = onStart(info)
        var pos = info.offset
        progress?.update(pos, info.size)
        while (true) {
            if (cancelled()) cancelStream(s)
            val f = if (pos >= info.size) slow { readFor(s) } else readFor(s)
            when (f.type) {
                Btp.DATA -> {
                    if (f.end || f.payload.size < 9) throw BtpException(Btp.E_PROTO, "bad DATA frame")
                    val o = PayloadReader(f.payload).u64()
                    if (o != pos) throw BtpException(Btp.E_OFFSET, "DATA at $o, expected $pos")
                    val n = f.payload.size - 8
                    if (pos + n > info.size) throw BtpException(Btp.E_PROTO, "more data than announced")
                    sink.write(f.payload, 8, n)
                    pos += n
                    progress?.update(pos, info.size)
                }
                Btp.GET_END -> {
                    expect(f, Btp.GET_END, true)
                    if (pos != info.size) throw BtpException(Btp.E_PROTO, "transfer ended at $pos of ${info.size}")
                    val p = PayloadReader(f.payload)
                    val sha = p.bytes(32)
                    p.done()
                    return GetOutcome(info, sha)
                }
                else -> throw BtpException(Btp.E_PROTO, "unexpected ${Btp.TYPE_NAMES[f.type] ?: f.type} during download")
            }
        }
    }

    /** Downloads a file (section 12). [onStart] sees the size before any data and returns the sink. */
    fun get(path: String, offset: Long = 0, sha: Boolean = true, ns: Int = Btp.NS_SD,
            cancelled: () -> Boolean = { false }, progress: Progress? = null,
            onStart: (GetInfo) -> DataSink): GetOutcome {
        val s = newStream()
        channel.send(Frame(Btp.GET, s, Payloads.get(ns, if (sha) 1 else 0, offset, path)))
        return receiveFile(s, cancelled, progress, onStart)
    }

    fun dbSnapshot(cancelled: () -> Boolean = { false }, progress: Progress? = null, onStart: (GetInfo) -> DataSink): GetOutcome {
        val s = newStream()
        channel.send(Frame(Btp.DB_SNAPSHOT, s, ByteArray(0))) // the snapshot is made before GET_R: receiveFile waits longer for it
        return receiveFile(s, cancelled, progress, onStart)
    }

    fun dbStage(baseSha256: ByteArray?, force: Boolean): DbStatusR = slow {
        DbStatusR.decode(call(Btp.DB_STAGE, Payloads.dbStage(if (force) 1 else 0, baseSha256 ?: ByteArray(32)), Btp.DB_STATUS_R))
    }

    fun dbStatus(): DbStatusR = DbStatusR.decode(call(Btp.DB_STATUS, ByteArray(0), Btp.DB_STATUS_R))
    fun dbUnstage(): DbStatusR = DbStatusR.decode(call(Btp.DB_UNSTAGE, ByteArray(0), Btp.DB_STATUS_R))

    /** The device answers when the rescan (or `hibytag dbupdate` for each path) is over: it can take a while. */
    fun libRescan(paths: List<String>?) {
        slow { call(Btp.LIB_RESCAN, Payloads.libRescan(if (paths == null) 0 else 1, paths ?: emptyList()), Btp.OK) }
    }

    fun updateStage(allowDowngrade: Boolean): UpdateStageR = slow {
        UpdateStageR.decode(call(Btp.UPDATE_STAGE, Payloads.flags(if (allowDowngrade) 1 else 0), Btp.UPDATE_STAGE_R))
    }

    fun updateInstall(noConfirm: Boolean, allowDowngrade: Boolean) = slow {
        call(Btp.UPDATE_INSTALL, Payloads.flags((if (noConfirm) 1 else 0) or (if (allowDowngrade) 2 else 0)), Btp.OK)
    }

    fun updateStatus(): UpdateStatusR = UpdateStatusR.decode(call(Btp.UPDATE_STATUS, ByteArray(0), Btp.UPDATE_STATUS_R))

    fun status(): StatusR = StatusR.decode(call(Btp.STATUS, ByteArray(0), Btp.STATUS_R))

    /** Sends BYE (best effort). */
    fun bye() {
        try {
            channel.send(Frame(Btp.BYE, 0, ByteArray(0)))
        } catch (e: IOException) {
            // already gone
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        bye()
        channel.wipe()
        try { socket?.close() } catch (e: IOException) {}
        try { closeable?.close() } catch (e: IOException) {}
    }

    companion object {
        const val SOFTWARE = "byos-cli 0.1.0"
        /** Read timeout while the device hashes or checks large files (the spec's 120 s idle limit, plus margin). */
        const val SLOW_TIMEOUT_MS = 150_000

        fun sasFrom(prk: ByteArray): String {
            val b = BtpCrypto.hkdfExpand(prk, "BTP/1 sas".toByteArray(), 4)
            val v = ((b[0].toLong() and 0xff) shl 24) or ((b[1].toLong() and 0xff) shl 16) or
                ((b[2].toLong() and 0xff) shl 8) or (b[3].toLong() and 0xff)
            return "%06d".format(v % 1_000_000)
        }

        fun keyId(clientPublic: ByteArray): ByteArray = BtpCrypto.sha256(clientPublic).copyOf(16)

        /** HELLO exchange over an already connected stream pair. */
        fun open(input: InputStream, output: java.io.OutputStream, hello: Hello, closeable: Closeable? = null,
                 socket: Socket? = null, random: (Int) -> ByteArray = BtpCrypto::random): BtpSession {
            val ch = FrameChannel(input, output, Btp.DIR_C2D)
            val hc = hello.encode()
            try {
                ch.send(Frame(Btp.HELLO, 0, hc))
                val f = ch.receive()
                if (f.stream == 0 && f.type == Btp.ERROR) {
                    val e = ErrorMsg.decode(f.payload)
                    throw BtpException(e.code, e.message, remote = true)
                }
                if (f.type != Btp.HELLO || f.stream != 0) throw BtpException(Btp.E_PROTO, "the device did not answer HELLO")
                val d = Hello.decode(f.payload)
                if (d.role != 1) throw BtpException(Btp.E_PROTO, "the other side is not a device")
                val v = minOf(hello.protoMax, d.protoMax)
                if (v < maxOf(hello.protoMin, d.protoMin)) throw BtpException(Btp.E_VERSION, "no common protocol version")
                if (d.maxPayload < Btp.MAX_PAYLOAD) throw BtpException(Btp.E_PROTO, "device max_payload too small")
                return BtpSession(closeable, socket, ch, hc, f.payload, random)
            } catch (t: Throwable) {
                try { closeable?.close() } catch (e: IOException) {}
                try { socket?.close() } catch (e: IOException) {}
                throw t
            }
        }

        /** Connects over TCP and exchanges HELLO. */
        fun connect(host: String, port: Int, hello: Hello, timeoutMs: Int = 15000,
                    random: (Int) -> ByteArray = BtpCrypto::random): BtpSession {
            val s = Socket()
            try {
                s.tcpNoDelay = true
                s.connect(InetSocketAddress(host, port), timeoutMs)
                s.soTimeout = timeoutMs
            } catch (e: IOException) {
                try { s.close() } catch (x: IOException) {}
                throw BtpConnectionException("cannot connect to $host:$port: ${e.message}", e)
            }
            return open(s.getInputStream(), s.getOutputStream(), hello, null, s, random)
        }
    }
}
