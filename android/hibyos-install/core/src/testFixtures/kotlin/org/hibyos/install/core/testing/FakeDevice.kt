package org.hibyos.install.core.testing

import org.hibyos.install.core.Ed25519
import org.hibyos.install.core.Json
import org.hibyos.install.core.btp.Announce
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.BtpPaths
import org.hibyos.install.core.btp.DbStatusR
import org.hibyos.install.core.btp.DfR
import org.hibyos.install.core.btp.ErrorMsg
import org.hibyos.install.core.btp.Frame
import org.hibyos.install.core.btp.FrameChannel
import org.hibyos.install.core.btp.Hello
import org.hibyos.install.core.btp.ListEntry
import org.hibyos.install.core.btp.ListR
import org.hibyos.install.core.btp.PayloadReader
import org.hibyos.install.core.btp.PayloadWriter
import org.hibyos.install.core.btp.Payloads
import org.hibyos.install.core.btp.PutBegin
import org.hibyos.install.core.btp.StatR
import org.hibyos.install.core.btp.StatusR
import org.hibyos.install.core.btp.UpdateStageR
import org.hibyos.install.core.btp.UpdateStatusR
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.util.concurrent.CopyOnWriteArrayList

/**
 * The device side of BTP/1 in Kotlin, for tests only: a stand-in for hibysync that serves a
 * folder as the SD card. It follows docs/SYNC_PROTOCOL.md closely enough to exercise every
 * client path (pairing, AUTH, encrypted records, PUT resume, cancellation, DB staging,
 * update staging), but it is not the device implementation and makes no durability promises.
 */
class FakeDevice(val sd: File, val data: File) {
    // ---- knobs
    @Volatile var sdState = 0
    @Volatile var sdFs = 2
    @Volatile var pairingOpen = false
    @Volatile var allowPairing = true
    @Volatile var shellAttached = true
    @Volatile var byosVersion = "0.8.0"
    @Volatile var firmware = "0.8.0"
    @Volatile var battery = 87
    @Volatile var charging = true
    var name = "HiBy R3 II"
    var updateKeys: List<String> = emptyList()
    /** Close the connection once after this many PUT data bytes were received (resume tests). */
    @Volatile var dropPutAfter: Long = -1
    /** Slow down GET/DB_SNAPSHOT data (cancellation tests). */
    @Volatile var getDelayMs = 0L
    /** Set HF_LOCAL in HELLO on every listener, as an impostor on the network would. */
    @Volatile var claimLocal = false
    /** Send a PING on stream 0 right after PUT_READY. */
    @Volatile var pingDuringPut = false
    /** Wait this long before PUT_READY/PUT_DONE (a device hashing a large part). */
    @Volatile var putReadyDelayMs = 0L
    /** An extra entry name added to every LIST reply (a malicious device). */
    @Volatile var extraListName: String? = null
    /** Derive AUTH keys from a wrong key, as a reset device or an impostor would: AUTH_FINISH does not open. */
    @Volatile var failAuthFinish = false
    /** Read AUTH_FINISH, then never answer (a link that stalls). */
    @Volatile var stallAfterAuthFinish = false
    /** Free space DF reports. */
    @Volatile var sdFree = 64000000000L
    /** When >= 0: the bytes new uploads may still take; a larger PUT gets E_NO_SPACE. */
    @Volatile var spaceLeft = -1L
    /** When > 0: after this many finished SD uploads the card goes to USB storage (E_SD_UNAVAILABLE). */
    @Volatile var sdUnavailableAfterPuts = -1
    /** Close new connections right away (a HiBy that went away). */
    @Volatile var refuseConnections = false
    /** Close this many connections right after AUTH_FINISH, without AUTH_OK (a daemon stopped mid-AUTH). */
    @Volatile var closeAfterAuthFinish = 0
    /** Answer this many HELLOs with ERROR E_BUSY on stream 0 (both session slots taken). */
    @Volatile var busyHellos = 0
    /** Connections accepted so far (each listener). */
    @Volatile var accepted = 0
    /** Entries per LIST_R page (small pages exercise the client's paging). */
    @Volatile var listPage = 3
    /** Uploads finished in NS_SD. */
    @Volatile var sdPuts = 0

    // ---- files that exist only in LIST/STAT (large synthetic libraries without real files)
    private val vFiles = HashMap<String, ListEntry>()
    private val vChildren = HashMap<String, LinkedHashMap<String, ListEntry>>()

    /** Adds a file that LIST and STAT show but that has no content (GET fails). */
    @Synchronized fun addVirtual(path: String, size: Long, mtime: Long) {
        var p = path
        var e = ListEntry(0, size, mtime, path.substringAfterLast('/'))
        vFiles[path.lowercase()] = e
        while (true) {
            val dir = p.substringBeforeLast('/', "")
            val kids = vChildren.getOrPut(dir.lowercase()) { LinkedHashMap() }
            if (kids.containsKey(e.name.lowercase()) && e.kind == 1) break
            kids[e.name.lowercase()] = e
            if (dir.isEmpty()) break
            p = dir
            e = ListEntry(1, 0, mtime, dir.substringAfterLast('/'))
        }
    }

    @Synchronized private fun virtualKids(dir: String): List<ListEntry>? = vChildren[dir.lowercase()]?.values?.toList()
    @Synchronized private fun virtualStat(path: String): ListEntry? = vFiles[path.lowercase()] ?: if (vChildren.containsKey(path.lowercase())) ListEntry(1, 0, 0, path) else null

    // ---- state visible to tests
    @Volatile var lastSas: String? = null
    val log = CopyOnWriteArrayList<String>()
    val deviceSecret: ByteArray = BtpCrypto.random(32)
    val devicePublic: ByteArray = BtpCrypto.x25519Base(deviceSecret)
    val deviceId: ByteArray = BtpCrypto.sha256(devicePublic).copyOf(8)
    val peers = CopyOnWriteArrayList<ByteArray>() // client static public keys
    @Volatile var updateState = 0
    @Volatile var updateVersion = ""
    @Volatile var rescans = 0

    val liveDb = File(data, "usrlocal_media.db")
    private val dbDir = File(data, "hibymod/sync/db")
    private val otaDir get() = File(sd, "ByOS/ota")

    private val servers = ArrayList<ServerSocket>()
    private var udp: DatagramSocket? = null
    @Volatile private var stopped = false

    init { sd.mkdirs(); dbDir.mkdirs() }

    fun startLocal(port: Int = 0): Int = listen(local = true, port = port)
    fun startWifi(): Int = listen(local = false)

    /** The TCP port that ANNOUNCE gives (changed by [moveWifi]). */
    @Volatile var announcePort = 0

    /** Probes received by the discovery socket. */
    @Volatile var probes = 0

    /** Ignore this many probes (lost broadcasts, or a daemon still starting). */
    @Volatile var ignoreProbes = 0

    /** Answers discovery probes on a UDP port (returned); announces point at [tcpPort]. */
    fun startDiscovery(tcpPort: Int): Int {
        announcePort = tcpPort
        val s = DatagramSocket(0, InetAddress.getByName("127.0.0.1"))
        udp = s
        Thread {
            val buf = ByteArray(64)
            while (!stopped) {
                val p = DatagramPacket(buf, buf.size)
                try { s.receive(p) } catch (e: IOException) { break }
                val nonce = Announce.parseProbe(p.data, p.length) ?: continue
                probes++
                if (ignoreProbes > 0) { ignoreProbes--; continue }
                val flags = (if (pairingOpen) 1 else 0) or (if (sdState == 0) 2 else 0) or (if (peers.isNotEmpty()) 4 else 0)
                val a = Announce(flags, nonce, announcePort, deviceId, name).encode()
                try { s.send(DatagramPacket(a, a.size, p.address, p.port)) } catch (e: IOException) {}
            }
        }.apply { isDaemon = true; start() }
        return s.localPort
    }

    /**
     * hibysync stopped and started again: every open connection is cut (no BYE), as when the
     * shell kills the daemon. The listeners stay (a new daemon binds the same port).
     */
    fun restart() {
        for (s in live) try { s.close() } catch (e: IOException) {}
        live.clear()
    }

    /**
     * The player got a new address: the Wi-Fi listeners close, a new one opens (its port is
     * returned and announced), and open connections are cut.
     */
    fun moveWifi(): Int {
        for (ss in servers.filter { !localServers.contains(it) }) { try { ss.close() } catch (e: IOException) {}; servers.remove(ss) }
        restart()
        val p = listen(local = false)
        announcePort = p
        return p
    }

    private val live = java.util.concurrent.CopyOnWriteArrayList<Socket>()
    private val localServers = ArrayList<ServerSocket>()

    private fun listen(local: Boolean, port: Int = 0): Int {
        val ss = ServerSocket()
        ss.reuseAddress = true
        ss.bind(java.net.InetSocketAddress(InetAddress.getByName("127.0.0.1"), port), 10)
        servers += ss
        if (local) localServers += ss
        Thread {
            while (!stopped) {
                val s = try { ss.accept() } catch (e: IOException) { break }
                accepted++
                if (refuseConnections) { try { s.close() } catch (e: IOException) {}; continue }
                live += s
                Thread { try { Session(s, local || claimLocal).run() } finally { live.remove(s) } }.apply { isDaemon = true; start() }
            }
        }.apply { isDaemon = true; start() }
        return ss.localPort
    }

    fun stop() {
        stopped = true
        servers.forEach { try { it.close() } catch (e: IOException) {} }
        udp?.close()
    }

    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }

    private class Fail(val code: Int, msg: String) : Exception(msg)

    private inner class Session(val sock: Socket, val local: Boolean) {
        val ch = FrameChannel(sock.getInputStream(), sock.getOutputStream(), Btp.DIR_D2C)
        var secured = false
        var helloC = ByteArray(0)
        var helloD = ByteArray(0)
        var incomingThisSession = false

        fun err(stream: Int, code: Int, msg: String) = ch.send(Frame(Btp.ERROR, stream, ErrorMsg(code, msg).encode(), Btp.F_END))
        fun ok(stream: Int, type: Int, payload: ByteArray = ByteArray(0)) = ch.send(Frame(type, stream, payload, Btp.F_END))

        fun run() {
            try {
                sock.soTimeout = 30000
                val h = ch.receive()
                if (h.type != Btp.HELLO || h.stream != 0) { err(0, Btp.E_PROTO, "HELLO first"); return }
                helloC = h.payload
                Hello.decode(helloC)
                if (busyHellos > 0) { busyHellos--; err(0, Btp.E_BUSY, "busy"); return }
                val flags = (if (local) Btp.HF_LOCAL else 0) or (if (pairingOpen) Btp.HF_PAIRING_OPEN else 0) or
                    (if (sdState == 0) Btp.HF_SD_MOUNTED else 0) or (if (shellAttached) Btp.HF_SHELL_ATTACHED else 0)
                helloD = Hello(role = 1, flags = flags, caps = Btp.ALL_CAPS, tlvs = listOf(
                    Btp.TLV_SOFTWARE to "fake-hibysync 0.1.0".toByteArray(), Btp.TLV_NAME to name.toByteArray(),
                    Btp.TLV_DEVICE_ID to deviceId, Btp.TLV_BYOS_VERSION to byosVersion.toByteArray(),
                    Btp.TLV_FIRMWARE to firmware.toByteArray(), Btp.TLV_MODEL to "R3II-2025".toByteArray())).encode()
                ch.send(Frame(Btp.HELLO, 0, helloD))
                while (true) {
                    val f = ch.receive()
                    log += Btp.TYPE_NAMES[f.type] ?: "0x%02x".format(f.type)
                    if (f.stream == 0) {
                        when (f.type) {
                            Btp.PING -> { ch.send(Frame(Btp.PONG, 0, f.payload)); continue }
                            Btp.BYE -> return
                            Btp.PAIR_COMMIT -> { if (!pair(f)) return; continue }
                            Btp.AUTH_INIT -> { if (!auth(f)) return; continue }
                            else -> { err(0, Btp.E_PROTO, "unexpected on stream 0"); return }
                        }
                    }
                    if (!local && !secured) { err(f.stream, Btp.E_AUTH_REQUIRED, "pair or authenticate first"); return }
                    if (f.type == Btp.CANCEL) continue // nothing running: the request already ended
                    try {
                        if (!handle(f)) return
                    } catch (e: Fail) {
                        err(f.stream, e.code, e.message ?: "")
                    } catch (e: BtpException) {
                        if (e.code == -1) return
                        err(f.stream, e.code, e.message ?: "")
                    }
                }
            } catch (e: BtpConnectionException) {
                // client gone
            } catch (e: IOException) {
            } catch (e: BtpException) {
                try { err(0, e.code, e.message ?: "") } catch (x: Exception) {}
            } finally {
                ch.wipe()
                try { sock.close() } catch (e: IOException) {}
            }
        }

        // ------------------------------------------------------------ pairing / auth

        fun pair(commitFrame: Frame): Boolean {
            if (!pairingOpen) { err(0, Btp.E_PAIR_CLOSED, "pairing is not open"); return false }
            val commit = commitFrame.payload
            val de = BtpCrypto.random(32); val dE = BtpCrypto.x25519Base(de); val nd = BtpCrypto.random(16)
            ch.send(Frame(Btp.PAIR_DEVICE, 0, dE + nd))
            val rv = ch.receive()
            if (rv.type != Btp.PAIR_REVEAL) { err(0, Btp.E_PROTO, "PAIR_REVEAL expected"); return false }
            val cE = rv.payload.copyOfRange(0, 32); val nc = rv.payload.copyOfRange(32, 48)
            if (!BtpCrypto.equal(commit, BtpCrypto.sha256("BTP/1 commit".toByteArray(), cE, nc))) {
                err(0, Btp.E_AUTH_FAILED, "bad commitment"); return false
            }
            val th = BtpCrypto.sha256("BTP/1 pair".toByteArray(), BtpCrypto.sha256(helloC), BtpCrypto.sha256(helloD), commit, dE, nd, cE, nc)
            val prk = BtpCrypto.hkdfExtract(th, BtpCrypto.x25519(de, cE))
            lastSas = org.hibyos.install.core.btp.BtpSession.sasFrom(prk)
            ch.secure(BtpCrypto.hkdfExpand(prk, "BTP/1 d2c".toByteArray(), 32), BtpCrypto.hkdfExpand(prk, "BTP/1 c2d".toByteArray(), 32))
            val cf = ch.receive()
            if (cf.type == Btp.BYE) { log += "PAIRFAIL mismatch"; return false }
            if (cf.type != Btp.PAIR_CONFIRM) { err(0, Btp.E_PROTO, "PAIR_CONFIRM expected"); return false }
            val r = PayloadReader(cf.payload)
            val cS = r.bytes(32)
            val clientName = r.str16()
            if (!allowPairing) { err(0, Btp.E_PAIR_REJECTED, "the user denied pairing"); return false }
            peers.removeIf { it.contentEquals(cS) }
            peers += cS
            log += "PAIRED $clientName"
            ch.send(Frame(Btp.PAIR_RESULT, 0, PayloadWriter().u8(0).bytes(devicePublic).bytes(deviceId).str16(name).build()))
            secured = true
            return true
        }

        fun auth(f: Frame): Boolean {
            val keyId = f.payload.copyOfRange(0, 16)
            val cE = f.payload.copyOfRange(16, 48)
            val cS = peers.firstOrNull { BtpCrypto.sha256(it).copyOf(16).contentEquals(keyId) }
            if (cS == null) { err(0, Btp.E_AUTH_FAILED, "unknown client"); return false }
            val de = BtpCrypto.random(32); val dE = BtpCrypto.x25519Base(de)
            ch.send(Frame(Btp.AUTH_REPLY, 0, dE))
            val ee = BtpCrypto.x25519(de, cE); val es = BtpCrypto.x25519(de, cS); val se = BtpCrypto.x25519(deviceSecret, cE)
            val th = BtpCrypto.sha256("BTP/1 auth".toByteArray(), BtpCrypto.sha256(helloC), BtpCrypto.sha256(helloD), keyId, cE, dE, cS, devicePublic)
            val prk = BtpCrypto.hkdfExtract(th, ee + es + se)
            if (failAuthFinish) ch.secure(BtpCrypto.random(32), BtpCrypto.random(32))
            else ch.secure(BtpCrypto.hkdfExpand(prk, "BTP/1 d2c".toByteArray(), 32), BtpCrypto.hkdfExpand(prk, "BTP/1 c2d".toByteArray(), 32))
            val fin = ch.receive() // throws if it does not open: the session is closed
            if (fin.type != Btp.AUTH_FINISH) return false
            if (closeAfterAuthFinish > 0) { closeAfterAuthFinish--; return false }
            if (stallAfterAuthFinish) { Thread.sleep(5000); return false }
            ch.send(Frame(Btp.AUTH_OK, 0, ByteArray(0)))
            secured = true
            return true
        }

        // ------------------------------------------------------------ paths

        fun root(ns: Int): File = when (ns) {
            Btp.NS_SD -> sd
            Btp.NS_UPDATE -> otaDir.also { it.mkdirs() }
            Btp.NS_DB -> dbDir
            else -> throw Fail(Btp.E_NOT_ALLOWED, "unknown namespace")
        }

        fun sdCheck(ns: Int) {
            if ((ns == Btp.NS_SD || ns == Btp.NS_UPDATE) && sdState != 0 && sdState != 3) throw Fail(Btp.E_SD_UNAVAILABLE, "SD not available")
        }

        fun resolve(ns: Int, path: String, allowRoot: Boolean = false): File {
            sdCheck(ns)
            BtpPaths.check(path, allowRoot)?.let { throw Fail(Btp.E_BAD_PATH, it) }
            when (ns) {
                Btp.NS_UPDATE -> if (path != "hibyos-update.json" && !Regex("hibyos-payload-[0-9A-Za-z._-]+\\.tar").matches(path)) throw Fail(Btp.E_BAD_PATH, "not allowed here")
                Btp.NS_DB -> if (path != "staged.db") throw Fail(Btp.E_BAD_PATH, "not allowed here")
            }
            val stored = when {
                ns == Btp.NS_UPDATE && path == "hibyos-update.json" -> "sync-update.json"
                ns == Btp.NS_DB -> "incoming.db"
                else -> path
            }
            return if (path.isEmpty()) root(ns) else File(root(ns), stored)
        }

        fun kind(f: File) = if (f.isDirectory) 1 else if (f.isFile) 0 else 2

        fun sha(f: File, upTo: Long = f.length()): ByteArray {
            val md = MessageDigest.getInstance("SHA-256")
            f.inputStream().use { s ->
                val b = ByteArray(65536); var left = upTo
                while (left > 0) { val n = s.read(b, 0, minOf(b.size.toLong(), left).toInt()); if (n < 0) break; md.update(b, 0, n); left -= n }
            }
            return md.digest()
        }

        // ------------------------------------------------------------ requests

        fun handle(f: Frame): Boolean {
            val s = f.stream
            val r = PayloadReader(f.payload)
            when (f.type) {
                Btp.STAT -> {
                    val ns = r.u8(); val p = r.str16(); r.done()
                    if (ns != Btp.NS_SD && ns != Btp.NS_UPDATE && ns != Btp.NS_DB) throw Fail(Btp.E_NOT_ALLOWED, "ns")
                    val file = resolve(ns, p, allowRoot = true)
                    if (!file.exists()) {
                        val v = if (ns == Btp.NS_SD) virtualStat(p) else null
                        if (v == null) throw Fail(Btp.E_NOT_FOUND, "no such file")
                        ok(s, Btp.STAT_R, StatR(v.kind, v.size, v.mtime).encode())
                        return true
                    }
                    ok(s, Btp.STAT_R, StatR(kind(file), if (file.isFile) file.length() else 0, file.lastModified() / 1000).encode())
                }
                Btp.LIST -> {
                    val ns = r.u8(); val fl = r.u8(); val p = r.str16(); r.done()
                    if (ns != Btp.NS_SD) throw Fail(Btp.E_NOT_ALLOWED, "LIST only in NS_SD")
                    val d = resolve(ns, p, allowRoot = true)
                    val virtual = virtualKids(p)
                    if (!d.exists() && virtual == null) throw Fail(Btp.E_NOT_FOUND, "no such folder")
                    if (d.exists() && !d.isDirectory) throw Fail(Btp.E_NOT_DIR, "not a folder")
                    val real = (d.listFiles() ?: emptyArray()).filter { fl and 1 != 0 || !it.name.startsWith(".") }
                        .map { ListEntry(kind(it), if (it.isFile) it.length() else 0, it.lastModified() / 1000, it.name) }
                    val realNames = real.map { it.name.lowercase() }.toSet()
                    val entries = real + (virtual ?: emptyList()).filter { it.name.lowercase() !in realNames } +
                        listOfNotNull(extraListName?.let { ListEntry(0, 1, 0, it) })
                    // small pages to exercise the client's page handling
                    val pages = entries.chunked(listPage).ifEmpty { listOf(emptyList()) }
                    for ((i, pg) in pages.withIndex()) ch.send(Frame(Btp.LIST_R, s, ListR.encode(pg), if (i == pages.size - 1) Btp.F_END else 0))
                }
                Btp.DF -> {
                    r.done()
                    val mounted = sdState == 0 || sdState == 3
                    val free = if (spaceLeft >= 0) minOf(sdFree, spaceLeft) else sdFree
                    ok(s, Btp.DF_R, DfR(sdState, if (mounted) sdFs else 0, if (mounted) 127865454592 else 0, if (mounted) free else 0, 150994944, 25165824).encode())
                }
                Btp.MKDIR -> {
                    val ns = r.u8(); val fl = r.u8(); val p = r.str16(); r.done()
                    if (ns != Btp.NS_SD) throw Fail(Btp.E_NOT_ALLOWED, "MKDIR only in NS_SD")
                    val d = resolve(ns, p)
                    if (d.exists()) { if (d.isDirectory && fl and 1 != 0) { ok(s, Btp.OK); return true }; throw Fail(Btp.E_EXISTS, "exists") }
                    if (fl and 1 != 0) d.mkdirs() else { if (!d.parentFile.isDirectory) throw Fail(Btp.E_NOT_FOUND, "no parent"); d.mkdir() }
                    ok(s, Btp.OK)
                }
                Btp.MOVE -> {
                    val ns = r.u8(); val fl = r.u8(); val a = r.str16(); val b = r.str16(); r.done()
                    if (ns != Btp.NS_SD) throw Fail(Btp.E_NOT_ALLOWED, "MOVE only in NS_SD")
                    val src = resolve(ns, a); val dst = resolve(ns, b)
                    if (!src.exists()) throw Fail(Btp.E_NOT_FOUND, "no source")
                    if (dst.exists() && fl and 1 == 0) throw Fail(Btp.E_EXISTS, "target exists")
                    if (dst.canonicalPath.startsWith(src.canonicalPath + File.separator)) throw Fail(Btp.E_BAD_PATH, "into itself")
                    if (dst.exists()) dst.delete()
                    if (!src.renameTo(dst)) throw Fail(Btp.E_IO, "rename failed")
                    ok(s, Btp.OK)
                }
                Btp.DELETE -> {
                    val ns = r.u8(); val fl = r.u8(); val p = r.str16(); r.done()
                    val t = resolve(ns, p)
                    if (!t.exists()) throw Fail(Btp.E_NOT_FOUND, "no such file")
                    if (t.isDirectory && (t.list()?.isNotEmpty() == true) && fl and 1 == 0) throw Fail(Btp.E_NOT_EMPTY, "not empty")
                    if (!t.deleteRecursively()) throw Fail(Btp.E_IO, "delete failed")
                    ok(s, Btp.OK)
                }
                Btp.PUT_BEGIN -> return put(s, PutBegin.decode(f.payload))
                Btp.GET -> {
                    val ns = r.u8(); val fl = r.u8(); val off = r.u64(); val p = r.str16(); r.done()
                    if (ns != Btp.NS_SD) throw Fail(Btp.E_NOT_ALLOWED, "GET only in NS_SD")
                    val file = resolve(ns, p)
                    if (!file.exists()) throw Fail(Btp.E_NOT_FOUND, "no such file")
                    if (file.isDirectory) throw Fail(Btp.E_IS_DIR, "is a folder")
                    sendFile(s, file, off, fl and 1 != 0)
                }
                Btp.DB_SNAPSHOT -> {
                    if (!liveDb.exists()) throw Fail(Btp.E_IO, "no library database")
                    val snap = File(dbDir, "snap.db")
                    liveDb.copyTo(snap, overwrite = true)
                    try { sendFile(s, snap, 0, true) } finally { snap.delete() }
                }
                Btp.DB_STAGE -> {
                    val fl = r.u8(); val base = r.bytes(32); r.done()
                    val inc = File(dbDir, "incoming.db")
                    if (!inc.exists() || !incomingThisSession) throw Fail(Btp.E_NOT_STAGED, "upload staged.db first")
                    val head = inc.inputStream().use { it.readNBytes(16) }
                    if (!String(head, Charsets.ISO_8859_1).startsWith("SQLite format 3")) throw Fail(Btp.E_DB_INVALID, "not SQLite")
                    if (fl and 1 == 0 && liveDb.exists() && !sha(liveDb).contentEquals(base)) throw Fail(Btp.E_DB_STALE, "library changed")
                    val staged = File(dbDir, "staged.db")
                    staged.delete(); inc.renameTo(staged)
                    File(dbDir, "staged.json").writeText("{\"sha256\":\"${hex(sha(staged))}\",\"size\":${staged.length()}}")
                    ok(s, Btp.DB_STATUS_R, dbStatus())
                }
                Btp.DB_STATUS -> ok(s, Btp.DB_STATUS_R, dbStatus())
                Btp.DB_UNSTAGE -> {
                    listOf("staged.db", "staged.json", "incoming.db").forEach { File(dbDir, it).delete() }
                    ok(s, Btp.DB_STATUS_R, dbStatus())
                }
                Btp.LIB_RESCAN -> {
                    val mode = r.u8(); val n = r.u16(); repeat(n) { r.str16() }; r.done()
                    if (mode == 0 && !shellAttached) throw Fail(Btp.E_UNSUPPORTED, "no shell")
                    rescans++
                    ok(s, Btp.OK)
                }
                Btp.UPDATE_STAGE -> { val fl = r.u8(); r.done(); ok(s, Btp.UPDATE_STAGE_R, stageUpdate(fl and 1 != 0, rename = true)) }
                Btp.UPDATE_INSTALL -> {
                    val fl = r.u8(); r.done()
                    if (!File(otaDir, "hibyos-update.json").exists()) throw Fail(Btp.E_NOT_STAGED, "nothing staged")
                    stageUpdate(fl and 2 != 0, rename = false)
                    if (!shellAttached) throw Fail(Btp.E_UNSUPPORTED, "no shell attached")
                    updateState = 3
                    ok(s, Btp.OK)
                }
                Btp.UPDATE_STATUS -> {
                    r.done()
                    val st = updateState
                    ok(s, Btp.UPDATE_STATUS_R, UpdateStatusR(st, if (st == 3) 500 else if (st == 4) 1000 else 0, 0, updateVersion).encode())
                    if (st == 3) updateState = 4
                    else if (st == 4) { byosVersion = updateVersion; updateState = 0; return false } // the shell restarts: connection drops
                }
                Btp.STATUS -> {
                    r.done()
                    ok(s, Btp.STATUS_R, StatusR(battery, if (charging) 1 else 0, sdState, listOf(Btp.TLV_BYOS_VERSION to byosVersion.toByteArray(),
                        Btp.TLV_FIRMWARE to firmware.toByteArray())).encode())
                }
                else -> err(s, Btp.E_UNKNOWN_TYPE, "unknown type")
            }
            return true
        }

        fun dbStatus(): ByteArray {
            val staged = File(dbDir, "staged.db")
            return DbStatusR(if (staged.exists()) 1 else 0, if (staged.exists()) sha(staged) else ByteArray(32),
                if (liveDb.exists()) sha(liveDb) else ByteArray(32), if (liveDb.exists()) liveDb.length() else 0, 0, 25165824).encode()
        }

        fun stageUpdate(allowDowngrade: Boolean, rename: Boolean): ByteArray {
            val js = File(otaDir, if (rename) "sync-update.json" else "hibyos-update.json")
            if (!js.exists()) throw Fail(Btp.E_NOT_STAGED, "no update JSON")
            val bytes = js.readBytes()
            if (!Ed25519.verifySignedJson(bytes, updateKeys)) throw Fail(Btp.E_SIGNATURE, "not signed by ByOS")
            val m = Json.obj(Json.parse(String(bytes, Charsets.UTF_8)))
            val ver = Json.str(m, "version")
            val pl = Json.obj(m["payload"])
            val tar = File(otaDir, Json.str(pl, "name"))
            if (!tar.exists()) throw Fail(Btp.E_NOT_STAGED, "payload not uploaded")
            if (tar.length() != Json.long(pl, "size") || hex(sha(tar)) != Json.str(pl, "sha256")) throw Fail(Btp.E_UPDATE_INVALID, "payload differs")
            fun parts(v: String) = v.split('.').map { it.toIntOrNull() ?: 0 }
            val cmp = parts(ver).zip(parts(byosVersion)).map { it.first.compareTo(it.second) }.firstOrNull { it != 0 } ?: 0
            if (cmp <= 0 && !allowDowngrade) throw Fail(Btp.E_DOWNGRADE, "not newer")
            if (rename) { val dst = File(otaDir, "hibyos-update.json"); dst.delete(); js.renameTo(dst) }
            updateVersion = ver
            updateState = 1
            val restart = when (Json.optStr(m, "restart")) { "none" -> 0; "shell" -> 1; else -> 2 }
            return UpdateStageR(ver, restart, if (m["needs_firmware"] == true) 1 else 0, tar.length()).encode()
        }

        fun sendFile(s: Int, file: File, off: Long, withSha: Boolean) {
            val size = file.length()
            if (off > size) throw Fail(Btp.E_OFFSET, "offset past the end")
            ch.send(Frame(Btp.GET_R, s, Payloads.getR(size, file.lastModified() / 1000, off)))
            val md = MessageDigest.getInstance("SHA-256")
            RandomAccessFile(file, "r").use { raf ->
                if (withSha && off > 0) { val b = ByteArray(65536); var p = 0L; while (p < off) { val n = raf.read(b, 0, minOf(b.size.toLong(), off - p).toInt()); md.update(b, 0, n); p += n } }
                raf.seek(off)
                var pos = off
                val buf = ByteArray(Btp.MAX_DATA)
                while (pos < size) {
                    if (ch.hasInput()) {
                        val c = ch.receive()
                        if (c.type == Btp.CANCEL && c.stream == s) { log += "CANCEL"; err(s, Btp.E_CANCELLED, "cancelled"); return }
                    }
                    val n = raf.read(buf, 0, minOf(buf.size.toLong(), size - pos).toInt())
                    if (withSha) md.update(buf, 0, n)
                    ch.send(Frame(Btp.DATA, s, Payloads.data(pos, buf, 0, n)), flush = getDelayMs > 0)
                    pos += n
                    if (getDelayMs > 0) Thread.sleep(getDelayMs)
                }
            }
            ok(s, Btp.GET_END, if (withSha) md.digest() else ByteArray(32))
        }

        fun put(s: Int, b: PutBegin): Boolean {
            val target = resolve(b.ns, b.path)
            if (putReadyDelayMs > 0) Thread.sleep(putReadyDelayMs)
            if (b.ns == Btp.NS_DB && b.size > 64L shl 20) throw Fail(Btp.E_FILE_TOO_BIG, "too big")
            if (sdFs == 1 && b.size > 4294967295L) throw Fail(Btp.E_FILE_TOO_BIG, "FAT32 limit")
            if (b.ns == Btp.NS_SD && spaceLeft >= 0 && !b.path.startsWith("ByOS/") && b.size > spaceLeft) throw Fail(Btp.E_NO_SPACE, "card full")
            if (!target.parentFile.isDirectory) throw Fail(Btp.E_NOT_FOUND, "parent folder missing")
            if (target.isDirectory) throw Fail(Btp.E_IS_DIR, "is a folder")
            if (target.exists()) when (b.mode) {
                Btp.MODE_CREATE -> throw Fail(Btp.E_EXISTS, "exists")
                Btp.MODE_SKIP_IDENTICAL -> {
                    if (target.length() == b.size && sha(target).contentEquals(b.sha256)) { ok(s, Btp.PUT_DONE, Payloads.putDone(1, b.size)); return true }
                    throw Fail(Btp.E_EXISTS, "exists and differs")
                }
            }
            val part = File(target.parentFile, BtpPaths.partFile(b.path, b.sha256, b.size).substringAfterLast('/'))
            val md = MessageDigest.getInstance("SHA-256")
            var offset = 0L
            if (part.exists()) {
                offset = BtpPaths.resumeOffset(part.length(), b.size)
                RandomAccessFile(part, "rw").use { it.setLength(offset) }
                val buf = ByteArray(65536)
                part.inputStream().use { st -> var left = offset; while (left > 0) { val n = st.read(buf, 0, minOf(buf.size.toLong(), left).toInt()); md.update(buf, 0, n); left -= n } }
            } else {
                part.createNewFile()
            }
            log += "PUT_READY ${b.path} $offset"
            if (pingDuringPut) {
                // in the same write as PUT_READY, so the client finds it waiting during the upload
                ch.send(Frame(Btp.PUT_READY, s, Payloads.u64(offset)), flush = false)
                ch.send(Frame(Btp.PING, 0, byteArrayOf(1, 2, 3, 4, 5, 6, 7, 8)))
                log += "PING"
            } else {
                ch.send(Frame(Btp.PUT_READY, s, Payloads.u64(offset)))
            }
            var pos = offset
            RandomAccessFile(part, "rw").use { raf ->
                raf.seek(offset)
                while (true) {
                    val f = ch.receive()
                    if (f.stream != s) {
                        if (f.type == Btp.PING) ch.send(Frame(Btp.PONG, 0, f.payload))
                        if (f.type == Btp.PONG) log += "PONG"
                        continue
                    }
                    when (f.type) {
                        Btp.CANCEL -> { log += "CANCEL"; err(s, Btp.E_CANCELLED, "cancelled"); return true }
                        Btp.DATA -> {
                            val o = PayloadReader(f.payload).u64()
                            if (o != pos) { err(s, Btp.E_OFFSET, "offset $o, expected $pos"); return true }
                            val n = f.payload.size - 8
                            if (pos + n > b.size) { raf.close(); part.delete(); err(s, Btp.E_HASH_MISMATCH, "too much data"); return true }
                            raf.write(f.payload, 8, n)
                            md.update(f.payload, 8, n)
                            pos += n
                            val drop = dropPutAfter
                            if (drop >= 0 && pos >= drop) {
                                dropPutAfter = -1
                                raf.fd.sync()
                                log += "DROPPED at $pos"
                                return false
                            }
                        }
                        Btp.PUT_END -> {
                            raf.fd.sync()
                            break
                        }
                        else -> { err(s, Btp.E_PROTO, "unexpected during PUT"); return false }
                    }
                }
            }
            if (pos != b.size || !md.digest().contentEquals(b.sha256)) { part.delete(); throw Fail(Btp.E_HASH_MISMATCH, "content differs") }
            if (target.exists() && b.mode == Btp.MODE_CREATE) { throw Fail(Btp.E_EXISTS, "exists") }
            target.delete()
            if (!part.renameTo(target)) throw Fail(Btp.E_IO, "rename failed")
            if (b.mtime != 0L) target.setLastModified(b.mtime * 1000)
            if (b.ns == Btp.NS_DB) incomingThisSession = true
            ok(s, Btp.PUT_DONE, Payloads.putDone(0, b.size))
            if (b.ns == Btp.NS_SD) {
                if (spaceLeft >= 0 && !b.path.startsWith("ByOS/")) spaceLeft -= b.size
                sdPuts++
                if (sdUnavailableAfterPuts > 0 && --sdUnavailableAfterPuts == 0) sdState = 2
            }
            return true
        }
    }
}
