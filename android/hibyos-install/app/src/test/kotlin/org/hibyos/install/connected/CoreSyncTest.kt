package org.hibyos.install.connected

import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpSession
import org.hibyos.install.core.btp.DfR
import org.hibyos.install.core.btp.ErrorMsg
import org.hibyos.install.core.btp.Frame
import org.hibyos.install.core.btp.FrameChannel
import org.hibyos.install.core.btp.Hello
import org.hibyos.install.core.btp.PayloadReader
import org.hibyos.install.core.btp.PayloadWriter
import org.hibyos.install.core.btp.PutBegin
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.file.Files
import java.util.concurrent.ConcurrentHashMap
import kotlin.concurrent.thread

/**
 * The app's binding to the core client ([CoreSync]) against a small BTP/1 device on a loopback
 * socket: pairing with a wrong and a right code, AUTH on reconnect, file requests, and a device
 * that forgot the phone.
 */
class CoreSyncTest {
    /** Just enough of hibysync, written from docs/SYNC_PROTOCOL.md with the core's own codecs and crypto. */
    private class TinyDevice : Closeable {
        val server = ServerSocket(0, 5, InetAddress.getLoopbackAddress())
        val ds: ByteArray = BtpCrypto.random(32)
        val dS: ByteArray = BtpCrypto.x25519Base(ds)
        val id: ByteArray = BtpCrypto.sha256(dS).copyOf(8)
        val peers = ArrayList<ByteArray>()
        val files = ConcurrentHashMap<String, ByteArray>()

        @Volatile var sas: String? = null
        @Volatile var pairingOpen = true

        /** Sets HF_LOCAL in HELLO, as an impostor on the LAN could. */
        @Volatile var claimLocal = false
        @Volatile var pings = 0

        init {
            thread(isDaemon = true) {
                while (!server.isClosed) {
                    val s = try {
                        server.accept()
                    } catch (e: Exception) {
                        break
                    }
                    thread(isDaemon = true) {
                        try {
                            s.use { serve(it) }
                        } catch (e: Exception) {
                            // the client went away
                        }
                    }
                }
            }
        }

        val port get() = server.localPort

        private fun end(ch: FrameChannel, type: Int, stream: Int, p: ByteArray) = ch.send(Frame(type, stream, p, Btp.F_END))

        private fun error(ch: FrameChannel, stream: Int, code: Int) = end(ch, Btp.ERROR, stream, ErrorMsg(code, "test").encode())

        private fun serve(sock: Socket) {
            val ch = FrameChannel(sock.getInputStream(), sock.getOutputStream(), Btp.DIR_D2C)
            val helloC = ch.receive().payload
            val flags = (if (pairingOpen) Btp.HF_PAIRING_OPEN else 0) or Btp.HF_SD_MOUNTED or (if (claimLocal) Btp.HF_LOCAL else 0)
            val helloD = Hello(role = 1, flags = flags, caps = Btp.ALL_CAPS,
                tlvs = listOf(Btp.TLV_NAME to "TinyHiBy".toByteArray(), Btp.TLV_DEVICE_ID to id, Btp.TLV_BYOS_VERSION to "0.9.0".toByteArray())).encode()
            ch.send(Frame(Btp.HELLO, 0, helloD))
            while (true) {
                val f = ch.receive()
                when (f.type) {
                    Btp.BYE -> return
                    Btp.PING -> {
                        pings++
                        ch.send(Frame(Btp.PONG, 0, f.payload))
                    }
                    Btp.PAIR_COMMIT -> {
                        val commit = f.payload
                        val de = BtpCrypto.random(32)
                        val dE = BtpCrypto.x25519Base(de)
                        val nd = BtpCrypto.random(16)
                        ch.send(Frame(Btp.PAIR_DEVICE, 0, dE + nd))
                        val r = PayloadReader(ch.receive().payload)
                        val cE = r.bytes(32)
                        val nc = r.bytes(16)
                        if (!BtpCrypto.sha256("BTP/1 commit".toByteArray(), cE, nc).contentEquals(commit)) return error(ch, 0, Btp.E_AUTH_FAILED)
                        val th = BtpCrypto.sha256("BTP/1 pair".toByteArray(), BtpCrypto.sha256(helloC), BtpCrypto.sha256(helloD), commit, dE, nd, cE, nc)
                        val prk = BtpCrypto.hkdfExtract(th, BtpCrypto.x25519(de, cE))
                        sas = BtpSession.sasFrom(prk)
                        ch.secure(BtpCrypto.hkdfExpand(prk, "BTP/1 d2c".toByteArray(), 32), BtpCrypto.hkdfExpand(prk, "BTP/1 c2d".toByteArray(), 32))
                        val c = ch.receive()
                        if (c.type != Btp.PAIR_CONFIRM) return
                        val cr = PayloadReader(c.payload)
                        synchronized(peers) { peers += cr.bytes(32) }
                        ch.send(Frame(Btp.PAIR_RESULT, 0, PayloadWriter().u8(0).bytes(dS).bytes(id).str16("TinyHiBy").build()))
                    }
                    Btp.AUTH_INIT -> {
                        val r = PayloadReader(f.payload)
                        val keyId = r.bytes(16)
                        val cE = r.bytes(32)
                        val cS = synchronized(peers) { peers.firstOrNull { BtpCrypto.sha256(it).copyOf(16).contentEquals(keyId) } }
                            ?: return error(ch, 0, Btp.E_AUTH_FAILED)
                        val de = BtpCrypto.random(32)
                        val dE = BtpCrypto.x25519Base(de)
                        ch.send(Frame(Btp.AUTH_REPLY, 0, dE))
                        val ikm = BtpCrypto.x25519(de, cE) + BtpCrypto.x25519(de, cS) + BtpCrypto.x25519(ds, cE)
                        val th = BtpCrypto.sha256("BTP/1 auth".toByteArray(), BtpCrypto.sha256(helloC), BtpCrypto.sha256(helloD), keyId, cE, dE, cS, dS)
                        val prk = BtpCrypto.hkdfExtract(th, ikm)
                        ch.secure(BtpCrypto.hkdfExpand(prk, "BTP/1 d2c".toByteArray(), 32), BtpCrypto.hkdfExpand(prk, "BTP/1 c2d".toByteArray(), 32))
                        if (ch.receive().type != Btp.AUTH_FINISH) return
                        ch.send(Frame(Btp.AUTH_OK, 0, ByteArray(0)))
                    }
                    Btp.DF -> end(ch, Btp.DF_R, f.stream, DfR(0, 1, 64L shl 30, 20L shl 30, 1L shl 30, 300L shl 20).encode())
                    Btp.LIST -> {
                        val w = PayloadWriter().u16(files.size)
                        for ((k, v) in files) w.u8(0).u64(v.size.toLong()).i64(0).str16(k.substringAfterLast('/')) // names, not paths
                        end(ch, Btp.LIST_R, f.stream, w.build())
                    }
                    Btp.MKDIR -> end(ch, Btp.OK, f.stream, ByteArray(0))
                    Btp.PUT_BEGIN -> {
                        val b = PutBegin.decode(f.payload)
                        if (files.containsKey(b.path) && b.mode == Btp.MODE_CREATE) {
                            error(ch, f.stream, Btp.E_EXISTS)
                            continue
                        }
                        ch.send(Frame(Btp.PUT_READY, f.stream, PayloadWriter().u64(0).build()))
                        val data = ByteArrayOutputStream()
                        while (true) {
                            val d = ch.receive()
                            if (d.type == Btp.PUT_END) break
                            if (d.type != Btp.DATA) return // BYE or anything else in the middle of the upload ends this session
                            data.write(d.payload, 8, d.payload.size - 8)
                        }
                        val all = data.toByteArray()
                        if (!BtpCrypto.sha256(all).contentEquals(b.sha256)) {
                            error(ch, f.stream, Btp.E_HASH_MISMATCH)
                            continue
                        }
                        files[b.path] = all
                        end(ch, Btp.PUT_DONE, f.stream, PayloadWriter().u8(0).u64(all.size.toLong()).build())
                    }
                    else -> error(ch, f.stream, Btp.E_UNSUPPORTED)
                }
            }
        }

        override fun close() = server.close()
    }

    private val device = TinyDevice()
    private val dir = Files.createTempDirectory("keys").toFile()

    @After
    fun done() {
        device.close()
        dir.deleteRecursively()
    }

    private fun sas(): String {
        repeat(200) {
            device.sas?.let { return it }
            Thread.sleep(10)
        }
        fail("the device never computed its code")
        throw IllegalStateException()
    }

    @Test
    fun pairReconnectAndSend() {
        val c = CoreSync(dir, "test")
        try {
            c.connect("127.0.0.1", device.port)
            fail("not paired yet")
        } catch (e: SyncException) {
            assertEquals(SyncException.NOT_PAIRED, e.code)
        }
        // a wrong code ends that handshake
        val p1 = c.startPairing("127.0.0.1", device.port, "Phone")
        val wrong = sas().let { if (it == "000000") "000001" else "000000" }
        try {
            p1.confirm(wrong) { false }
            fail("wrong code accepted")
        } catch (e: SyncException) {
            assertEquals(SyncException.WRONG_CODE, e.code)
        }
        device.sas = null
        val p2 = c.startPairing("127.0.0.1", device.port, "Phone")
        val s = p2.confirm(sas()) { false }
        assertEquals("TinyHiBy", s.info.name)
        assertEquals(1, c.paired().size)
        assertEquals(device.id.joinToString("") { "%02x".format(it) }, c.paired()[0].deviceId)
        assertEquals(20L shl 30, s.df().sdFree)
        s.close()

        // a new session uses AUTH and the stored keys (also from a new CoreSync: they are on disk)
        val s2 = CoreSync(dir, "test").connect("127.0.0.1", device.port)
        assertEquals("0.9.0", s2.info.byosVersion)
        val data = ByteArray(200_000) { it.toByte() }
        val sha = BtpCrypto.sha256(data)
        var last = 0L
        val r = s2.put(Ns.SD, "Music/x.flac", data.size.toLong(), sha, 0, PutMode.SKIP_IDENTICAL,
            { off -> ByteArrayInputStream(data, off.toInt(), data.size - off.toInt()) }, { false }) { last = it }
        assertFalse(r.skipped)
        assertEquals(data.size.toLong(), last)
        assertTrue(device.files["Music/x.flac"]!!.contentEquals(data))
        assertEquals(listOf("x.flac"), s2.list("Music").map { it.name })
        s2.ping()
        assertEquals(1, device.pings)
        assertFalse(s2.info.local)
        try {
            s2.put(Ns.SD, "Music/x.flac", 1, BtpCrypto.sha256(byteArrayOf(1)), 0, PutMode.CREATE, { ByteArrayInputStream(byteArrayOf(1)) }, { false }) {}
            fail("CREATE over an existing file")
        } catch (e: SyncException) {
            assertEquals(SyncException.E_EXISTS, e.code)
            assertFalse(e.fatal)
        }
        s2.close()

        // the player forgot this phone: AUTH fails with FORGOTTEN. That refusal is unauthenticated,
        // so the keys stay until a new pairing replaces them or the user forgets the player
        synchronized(device.peers) { device.peers.clear() }
        try {
            c.connect("127.0.0.1", device.port)
            fail("forgotten client accepted")
        } catch (e: SyncException) {
            assertEquals(SyncException.FORGOTTEN, e.code)
        }
        assertEquals(1, c.paired().size)
        c.forget(c.paired()[0].deviceId)
        assertTrue(c.paired().isEmpty())
    }

    private fun pairedCore(): CoreSync {
        val c = CoreSync(dir, "test")
        val p = c.startPairing("127.0.0.1", device.port, "Phone")
        p.confirm(sas()) { false }.close()
        return c
    }

    /** Review: HF_LOCAL in an unauthenticated HELLO must not let the session skip AUTH. */
    @Test
    fun reviewLocalFlagSkipsAuth() {
        val c = pairedCore()
        device.claimLocal = true
        // a real paired device that sets the flag: still AUTH, still encrypted
        val s = c.connect("127.0.0.1", device.port)
        assertFalse(s.info.local)
        assertEquals(20L shl 30, s.df().sdFree)
        s.close()
        // an impostor with the same id that does not hold the keys: AUTH runs and fails
        synchronized(device.peers) { device.peers.clear() }
        try {
            c.connect("127.0.0.1", device.port).close()
            fail("HF_LOCAL skipped AUTH")
        } catch (e: SyncException) {
            assertEquals(SyncException.FORGOTTEN, e.code)
        }
        assertEquals(1, c.paired().size)
        // only tooling that asks for it may use a clear session, and only on loopback
        val tool = CoreSync(dir, "test", allowLocal = true)
        val t = tool.connect("127.0.0.1", device.port)
        assertTrue(t.info.local)
        t.close()
        assertTrue(CoreSync.isLoopback("127.0.0.1"))
        assertFalse(CoreSync.isLoopback("192.168.1.20"))
    }

    @Test
    fun anotherDeviceAtTheAddressIsRefused() {
        val c = pairedCore()
        try {
            c.connect("127.0.0.1", device.port, "00112233aabbccdd").close()
            fail("a different device was accepted")
        } catch (e: SyncException) {
            assertEquals(SyncException.IDENTITY_CHANGED, e.code)
        }
        val id = device.id.joinToString("") { "%02x".format(it) }
        c.connect("127.0.0.1", device.port, id).close()
    }

    @Test
    fun pairingClosedAndUnreachable() {
        device.pairingOpen = false
        val c = CoreSync(dir, "test")
        try {
            c.startPairing("127.0.0.1", device.port, "Phone")
            fail()
        } catch (e: SyncException) {
            assertEquals(SyncException.E_PAIR_CLOSED, e.code)
        }
        val closed = ServerSocket(0, 1, InetAddress.getLoopbackAddress()).let { val p = it.localPort; it.close(); p }
        try {
            c.connect("127.0.0.1", closed)
            fail()
        } catch (e: SyncException) {
            assertEquals(SyncException.UNREACHABLE, e.code)
        }
    }

    @Test
    fun theModelPairsThroughTheCore() {
        val env = object : SyncEnv {
            val c = CoreSync(dir, "test")
            override fun connector(): SyncConnector = c
            override val clientName = "Phone"
            override val cacheDir = dir
            override fun post(f: () -> Unit) = f()
            override fun execute(f: () -> Unit) = f()
            override fun readLibrary(file: java.io.File) = emptyList<Track>()
            override fun pickFiles() {}
            override fun pickFolder() {}
            override fun byosFiles(): List<org.hibyos.install.core.SendItem>? = null
        }
        val m = ConnectedModel(env)
        m.connectAddress("127.0.0.1:${device.port}")
        assertTrue(m.state.link is Link.Pairing)
        m.setCode(sas())
        m.confirmCode()
        val l = m.state.link as Link.Connected
        assertEquals("TinyHiBy", l.device.name)
        assertEquals(20L shl 30, l.space?.sdFree)
        m.addPicked { listOf(LocalItem(listOf("a.txt"), 3, 0) { ByteArrayInputStream("abc".toByteArray()) }) }
        m.startSend()
        assertEquals(JobStatus.SENT, m.state.transfer.jobs.single().status)
        assertEquals("abc", String(device.files["Music/a.txt"]!!))

        // a phone file that fails to read in the middle of its PUT: a new session carries the next file
        m.clearSend()
        val big = ByteArray(150_000) { it.toByte() }
        var opens = 0
        val failing = LocalItem(listOf("bad.bin"), big.size.toLong(), 0) {
            opens++
            if (opens == 1) ByteArrayInputStream(big) // hashing reads it fine
            else object : java.io.InputStream() {
                var n = 0
                override fun read(): Int = throw java.io.IOException("provider error")
                override fun read(b: ByteArray, off: Int, len: Int): Int {
                    if (n > 70_000) throw java.io.IOException("provider error")
                    val k = minOf(len, big.size - n)
                    System.arraycopy(big, n, b, off, k)
                    n += k
                    return k
                }
            }
        }
        m.addPicked { listOf(failing, LocalItem(listOf("b.txt"), 3, 0) { ByteArrayInputStream("xyz".toByteArray()) }) }
        m.startSend()
        val jobs = m.state.transfer.jobs
        assertEquals(listOf(JobStatus.FAILED, JobStatus.SENT), jobs.map { it.status })
        assertEquals("xyz", String(device.files["Music/b.txt"]!!))
        assertTrue(m.state.link is Link.Connected)
        m.disconnect()
    }
}
