package org.hibyos.install.core.btp

import org.hibyos.install.core.testing.FakeDevice
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import java.io.File
import java.net.InetAddress
import java.security.MessageDigest
import kotlin.concurrent.thread

/**
 * The paired-device reconnection of the core ([BtpReconnector], [BtpLink]) and discovery
 * ([BtpDiscovery]) against the fake hibysync: the daemon restarting, the player moving to
 * another address, a device that forgot the client, transient refusals, and an upload cut in
 * the middle that must still end as "written".
 */
class ReconnectTest {
    private lateinit var dir: File
    private lateinit var dev: FakeDevice
    private var port = 0
    private val secret = BtpCrypto.random(32)
    private lateinit var paired: PairedDevice
    private val localhost: InetAddress = InetAddress.getByName("127.0.0.1")

    @Before
    fun setUp() {
        dir = File(System.getProperty("java.io.tmpdir"), "btp-reconnect-test/${System.nanoTime()}")
        dir.mkdirs()
        dev = FakeDevice(File(dir, "sd"), File(dir, "data"))
        port = dev.startWifi()
        // paired beforehand: the device knows the client's key, the client the device's
        dev.peers += BtpCrypto.x25519Base(secret)
        paired = PairedDevice(dev.deviceId, dev.devicePublic, dev.name)
    }

    @After
    fun tearDown() {
        dev.stop()
        dir.deleteRecursively()
    }

    private val hello = Hello.client("reconnect-test", "Test phone")
    private fun dial(h: String, p: Int) = BtpSession.connect(h, p, hello, 3000)

    private fun reconnector(udp: Int? = null, delays: LongArray = longArrayOf(0, 100, 200, 300, 500, 800)) = BtpReconnector(
        dial = ::dial,
        locate = { id, _, cancelled ->
            if (udp == null) null
            else BtpDiscovery.probe(600, listOf(localhost), udp, cancelled = cancelled, stopWhen = { it.deviceIdHex == id })
                .firstOrNull { it.deviceIdHex == id }
        },
        delaysMs = delays,
    )

    private fun link(r: BtpReconnector, onMoved: (String, Int) -> Unit = { _, _ -> }): BtpLink =
        BtpLink(r, paired, secret, r.open(paired, secret, "127.0.0.1", port), onMoved)

    private fun sha(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b)

    @Test
    fun opensAtTheStoredAddress() {
        val r = reconnector().open(paired, secret, "127.0.0.1", port)
        r.session.use {
            assertTrue(it.secured)
            assertEquals(port, r.port)
            assertEquals(1, r.attempts)
        }
    }

    @Test
    fun comesBackAfterTheDaemonRestarts() {
        val l = link(reconnector())
        l.use {
            assertEquals(0, it.call(retry = true) { s -> s.df().sdState })
            // the shell kills hibysync and starts it again a moment later
            dev.refuseConnections = true
            dev.restart()
            thread { Thread.sleep(400); dev.refuseConnections = false }
            val retries = ArrayList<Int>()
            val df = it.call(retry = true, onRetry = { n, _ -> retries += n }) { s -> s.df() }
            assertEquals(0, df.sdState)
            assertEquals(1, it.reconnects)
            assertTrue("it waited for the daemon", retries.isNotEmpty())
            assertTrue(it.session.secured)
        }
    }

    @Test
    fun findsThePlayerAtItsNewAddress() {
        val udp = dev.startDiscovery(port)
        var moved: Pair<String, Int>? = null
        val l = link(reconnector(udp)) { h, p -> moved = h to p }
        l.use {
            val newPort = dev.moveWifi()
            val list = it.call(retry = true) { s -> s.list("") }
            assertNotNull(list)
            assertEquals(newPort, it.port)
            assertEquals("127.0.0.1" to newPort, moved)
        }
    }

    @Test
    fun aRememberedPlayerIsFoundWithoutAnAddress() {
        val udp = dev.startDiscovery(port)
        val r = reconnector(udp).open(paired, secret, null)
        r.session.use { assertEquals(port, r.port) }
    }

    @Test
    fun aPlayerThatForgotTheClientIsReportedAtOnce() {
        dev.peers.clear()
        val started = System.nanoTime()
        try {
            reconnector(delays = longArrayOf(0, 2000, 2000, 2000)).open(paired, secret, "127.0.0.1", port)
            fail("AUTH should be refused")
        } catch (e: BtpAuthException) {
            assertEquals(BtpAuthException.Reason.UNKNOWN_CLIENT, e.reason)
        }
        assertTrue("no retries for a forgotten client", System.nanoTime() - started < 1_500_000_000L)
    }

    @Test
    fun aDaemonStoppingDuringAuthIsTriedAgain() {
        dev.closeAfterAuthFinish = 1
        val r = reconnector().open(paired, secret, "127.0.0.1", port)
        r.session.use { assertEquals(2, r.attempts) }
    }

    @Test
    fun aDeviceThatNeverFinishesAuthIsReported() {
        dev.closeAfterAuthFinish = 5
        try {
            reconnector().open(paired, secret, "127.0.0.1", port)
            fail("the identity cannot be proven")
        } catch (e: BtpAuthException) {
            assertEquals(BtpAuthException.Reason.IDENTITY_CHANGED, e.reason)
        }
    }

    @Test
    fun aRecordThatDoesNotOpenIsNeverRetried() {
        dev.failAuthFinish = true
        try {
            reconnector().open(paired, secret, "127.0.0.1", port)
            fail("another device must not be accepted")
        } catch (e: BtpAuthException) {
            assertEquals(BtpAuthException.Reason.IDENTITY_CHANGED, e.reason)
        } catch (e: BtpConnectionException) {
            // the fake closes when the record does not open on its side: that is also a refusal,
            // but it must not have been accepted
        }
        dev.failAuthFinish = false
    }

    @Test
    fun aBusyPlayerIsTriedAgain() {
        dev.busyHellos = 2
        val r = reconnector().open(paired, secret, "127.0.0.1", port)
        r.session.use { assertEquals(3, r.attempts) }
    }

    @Test
    fun aRequestThatMustNotRunTwiceSaysSoAfterReconnecting() {
        val l = link(reconnector())
        l.use {
            it.session.mkdir("A", false)
            dev.restart()
            try {
                it.call(retry = false) { s -> s.mkdir("B", false) }
                fail("the request must not be repeated silently")
            } catch (e: BtpReconnectedException) {
                // the new session is open
            }
            it.call(retry = false) { s -> s.mkdir("B", false) }
            assertTrue(File(dir, "sd/B").isDirectory)
        }
    }

    @Test
    fun anUploadCutInTheMiddleEndsWritten() {
        val data = ByteArray(700_000) { (it * 31 + 7).toByte() }
        val l = link(reconnector())
        l.use {
            dev.dropPutAfter = 300_000
            val out = it.call(retry = true) { s ->
                s.put("Music/song.flac".substringAfter('/'), data.size.toLong(), sha(data), Btp.MODE_SKIP_IDENTICAL) { off ->
                    data.inputStream(off.toInt(), data.size - off.toInt())
                }
            }
            // the second PUT resumed from the kept part and wrote the file: not "skipped", not an error
            assertEquals(0, out.result)
            assertTrue("resumed, not sent again from the start", out.resumedFrom > 0)
            assertArrayEquals(data, File(dir, "sd/song.flac").readBytes())
            assertEquals(1, it.reconnects)
            // the same file again is identical: skipped
            val again = it.call(retry = true) { s -> s.put("song.flac", data.size.toLong(), sha(data), Btp.MODE_SKIP_IDENTICAL) { data.inputStream() } }
            assertEquals(1, again.result)
        }
    }

    // ---- the shell stops hibysync gracefully (shutdown_all: ERROR E_CANCELLED on the stream, BYE)

    private fun putAll(l: BtpLink, path: String, data: ByteArray): PutOutcome = l.call(retry = true) { s ->
        s.put(path, data.size.toLong(), sha(data), Btp.MODE_SKIP_IDENTICAL) { off -> data.inputStream(off.toInt(), data.size - off.toInt()) }
    }

    @Test
    fun aGracefulStopInTheMiddleOfAnUploadIsResumed() {
        val data = ByteArray(700_000) { (it * 13 + 1).toByte() }
        link(reconnector()).use {
            dev.stopPutAfter = 300_000
            val out = putAll(it, "stopped.flac", data)
            assertEquals(1, dev.gracefulStops)
            // not a cancel by the user: the session came back and the PUT resumed from the part
            assertEquals(0, out.result)
            assertTrue("resumed, not sent again from the start", out.resumedFrom > 0)
            assertArrayEquals(data, File(dir, "sd/stopped.flac").readBytes())
            assertEquals(1, it.reconnects)
        }
    }

    @Test
    fun aGracefulStopBeforePutDoneIsResumed() {
        val data = ByteArray(700_000) { (it * 5 + 3).toByte() }
        link(reconnector()).use {
            dev.stopAtPutEnd = true
            val out = putAll(it, "waiting.flac", data)
            assertEquals(1, dev.gracefulStops)
            assertEquals(0, out.result)
            assertTrue(out.resumedFrom > 0)
            assertArrayEquals(data, File(dir, "sd/waiting.flac").readBytes())
            assertEquals(1, it.reconnects)
        }
    }

    @Test
    fun aGracefulStopInTheMiddleOfADownloadGoesOn() {
        val data = ByteArray(700_000) { (it * 11 + 2).toByte() }
        File(dir, "sd/big.bin").writeBytes(data)
        link(reconnector()).use {
            dev.stopGetAfter = 200_000
            val out = java.io.ByteArrayOutputStream()
            var pos = 0L
            val r = it.call(retry = true) { s ->
                s.get("big.bin", pos) { DataSink { b, off, len -> out.write(b, off, len); pos += len } }
            }
            assertEquals(1, dev.gracefulStops)
            assertArrayEquals(data, out.toByteArray())
            assertArrayEquals(sha(data), r.sha256)
            assertEquals(1, it.reconnects)
        }
    }

    @Test
    fun aCancelOfTheClientIsNotTakenForALostConnection() {
        assertTrue(BtpReconnector.isConnectionLoss(BtpException(Btp.E_CANCELLED, "stopping", remote = true)))
        assertFalse(BtpReconnector.isConnectionLoss(BtpException(Btp.E_CANCELLED, "cancelled")))
        val data = ByteArray(700_000) { it.toByte() }
        File(dir, "sd/slow.bin").writeBytes(data)
        dev.getDelayMs = 5
        link(reconnector()).use {
            var cancel = false
            try {
                it.call(retry = true, cancelled = { cancel }) { s ->
                    s.get("slow.bin", 0, cancelled = { cancel }, progress = Progress { d, _ -> if (d > 100_000) cancel = true }) {
                        DataSink { _, _, _ -> }
                    }
                }
                fail("cancelled by the client")
            } catch (e: BtpException) {
                assertEquals(Btp.E_CANCELLED, e.code)
                assertFalse(e.remote)
            }
            assertEquals(0, it.reconnects)
        }
    }

    @Test
    fun aPlayerThatStaysBusyAfterALossIsStillALostConnection() {
        link(reconnector(delays = longArrayOf(0, 50, 50))).use {
            dev.restart()
            dev.busyHellos = 100
            try {
                it.call(retry = true) { s -> s.df() }
                fail("never reached again")
            } catch (e: BtpConnectionException) {
                // a lost connection (the app drops the session), not a "busy" answer to the request
                val c = e.cause as BtpException
                assertEquals(Btp.E_BUSY, c.code)
                assertTrue(BtpReconnector.isConnectionLoss(e))
            }
        }
    }

    @Test
    fun aShortRequestUsesTheQuickReconnector() {
        val quick = reconnector(delays = longArrayOf(0, 50))
        val full = reconnector(delays = longArrayOf(0, 2000, 2000, 2000))
        val l = BtpLink(full, paired, secret, full.open(paired, secret, "127.0.0.1", port), quick = quick)
        l.use {
            dev.refuseConnections = true
            dev.restart()
            val tries = ArrayList<Pair<Int, Int>>()
            val started = System.nanoTime()
            try {
                it.call(retry = true, onRetry = { n, of -> tries += n to of }, quick = true) { s -> s.ping() }
                fail("the player went away")
            } catch (e: java.io.IOException) {
                assertTrue(BtpReconnector.isConnectionLoss(e))
            }
            assertTrue("reported in well under the full patience", System.nanoTime() - started < 1_500_000_000L)
            assertEquals(listOf(1 to 2, 2 to 2), tries)
        }
    }

    @Test
    fun cancellingStopsTheWait() {
        dev.refuseConnections = true
        var cancel = false
        thread { Thread.sleep(150); cancel = true }
        try {
            reconnector(delays = longArrayOf(0, 5000, 5000)).open(paired, secret, "127.0.0.1", port, { cancel })
            fail("cancelled")
        } catch (e: BtpException) {
            assertEquals(Btp.E_CANCELLED, e.code)
        }
    }

    @Test
    fun aGoneDeviceEndsWithAConnectionError() {
        dev.refuseConnections = true
        try {
            reconnector(delays = longArrayOf(0, 50, 50)).open(paired, secret, "127.0.0.1", port)
            fail("nobody answers")
        } catch (e: java.io.IOException) {
            assertTrue(BtpReconnector.isConnectionLoss(e))
        }
    }

    // ---- discovery

    @Test
    fun announcesAreCheckedBeforeUse() {
        val nonce = ByteArray(8) { 5 }
        val a = Announce(3, nonce, 7340, ByteArray(8) { 9 }, "HiBy R3 II").encode()
        val ok = BtpDiscovery.accept(InetAddress.getByName("192.168.1.23"), a, a.size, nonce)
        assertNotNull(ok)
        assertEquals("192.168.1.23", ok!!.address)
        assertEquals(7340, ok.port)
        assertEquals("0909090909090909", ok.deviceIdHex)
        assertTrue(ok.announce.pairingOpen)
        assertTrue(ok.announce.sdMounted)
        // another nonce (an old answer), a public sender, a truncated packet, a probe, a port of 0
        assertNull(BtpDiscovery.accept(InetAddress.getByName("192.168.1.23"), a, a.size, ByteArray(8)))
        assertNull(BtpDiscovery.accept(InetAddress.getByName("8.8.8.8"), a, a.size, nonce))
        assertNull(BtpDiscovery.accept(InetAddress.getByName("10.0.0.2"), a, a.size - 3, nonce))
        val p = Announce.probe(nonce)
        assertNull(BtpDiscovery.accept(InetAddress.getByName("10.0.0.2"), p, p.size, nonce))
        val zero = Announce(0, nonce, 0, ByteArray(8), "x").encode()
        assertNull(BtpDiscovery.accept(InetAddress.getByName("10.0.0.2"), zero, zero.size, nonce))
    }

    @Test
    fun subnetBroadcastAddresses() {
        assertEquals("192.168.1.255", BtpDiscovery.subnetBroadcast(byteArrayOf(192.toByte(), 168.toByte(), 1, 40), 24)!!.hostAddress)
        assertEquals("10.0.15.255", BtpDiscovery.subnetBroadcast(byteArrayOf(10, 0, 12, 1), 22)!!.hostAddress)
        assertNull(BtpDiscovery.subnetBroadcast(byteArrayOf(10, 0, 12, 1), 32))
        assertNull(BtpDiscovery.subnetBroadcast(ByteArray(16), 64))
    }

    @Test
    fun aLostProbeIsSentAgain() {
        val udp = dev.startDiscovery(port)
        dev.ignoreProbes = 2
        val seen = ArrayList<Found>()
        val found = BtpDiscovery.probe(2500, listOf(localhost), udp, onFound = { seen += it },
            stopWhen = { it.deviceIdHex == paired.deviceId.joinToString("") { b -> "%02x".format(b) } })
        assertEquals(1, found.size)
        assertEquals(port, found[0].port)
        assertEquals(1, seen.size)
        assertTrue(dev.probes >= 3)
    }

    @Test
    fun theSweepFindsThePlayerWhenBroadcastsAreDropped() {
        val seen = ArrayList<Found>()
        val found = BtpDiscovery.sweep(listOf("127.0.0.1"), hello, port, onFound = { seen += it })
        assertEquals(1, found.size)
        assertEquals(dev.name, found[0].announce.name)
        assertArrayEquals(dev.deviceId, found[0].announce.deviceId)
        assertEquals(port, found[0].port)
        assertEquals(1, seen.size)
        // nothing listening there: nothing found, quickly
        dev.stop()
        assertTrue(BtpDiscovery.sweep(listOf("127.0.0.1"), hello, port, connectTimeoutMs = 300).isEmpty())
    }
}
