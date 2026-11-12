package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.library.SchemaEntry
import org.hibyos.install.core.library.SqlRow
import org.hibyos.install.core.library.SqlText
import org.hibyos.install.core.library.SqliteWriter
import org.hibyos.install.core.testing.FakeDevice
import org.hibyos.install.core.testing.SynthAudio
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.io.PrintStream
import java.net.ServerSocket
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.Signature

class CliTest {
    private lateinit var dir: File
    private lateinit var dev: FakeDevice
    private var usbPort = 0
    private var wifiPort = 0
    private lateinit var cfg: File

    class Run(val code: Int, val lines: List<Map<String, Any?>>, val raw: String) {
        val last get() = lines.last()
        fun events(name: String) = lines.filter { it["event"] == name }
        override fun toString() = raw
    }

    @Before
    fun setUp() {
        dir = File(System.getProperty("java.io.tmpdir"), "byos-cli-test/${System.nanoTime()}")
        dir.mkdirs()
        dev = FakeDevice(File(dir, "sd"), File(dir, "data"))
        usbPort = dev.startLocal()
        wifiPort = dev.startWifi()
        cfg = File(dir, "config")
    }

    @After
    fun tearDown() {
        dev.stop()
        dir.deleteRecursively()
    }

    private fun run(vararg args: String, stdin: InputStream = ByteArrayInputStream(ByteArray(0)), keys: List<String>? = null,
                    env: Map<String, String> = emptyMap(), runner: ProcessRunner = ProcessRunner { _, _ -> throw java.io.IOException("no programs in tests") },
                    device: String? = "127.0.0.1:$usbPort", heartbeatMs: Long = Out.HEARTBEAT_MS): Run {
        val bout = ByteArrayOutputStream()
        val ps = PrintStream(bout, true, "UTF-8")
        val cli = Cli(stdin, ps, PrintStream(ByteArrayOutputStream()), env, runner,
            keys ?: org.hibyos.install.core.btp.UpdatePackage.RELEASE_KEYS, listOf(20, 20, 20), 20, 3000, heartbeatMs)
        val all = ArrayList<String>()
        all += listOf("--config", cfg.path)
        if (device != null) all += listOf("--device", device)
        all += args
        val code = cli.run(all.toTypedArray())
        val text = bout.toString("UTF-8")
        val lines = text.lines().filter { it.isNotEmpty() }.map { Json.obj(Json.parse(it)) }
        // the contract: first line start, exactly one final result/error, every line versioned
        assertEquals(text, "start", lines.first()["event"])
        assertEquals(text, 1, lines.count { it["event"] == "result" || it["event"] == "error" })
        assertTrue(text, lines.last()["event"] == "result" || lines.last()["event"] == "error")
        assertTrue(lines.all { it["v"] == 1L && it["t"] is Long })
        assertEquals(text, if (code == 0) "result" else "error", lines.last()["event"])
        return Run(code, lines, text)
    }

    private fun sha(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }
    private fun data(n: Int, seed: Int = 1) = ByteArray(n) { ((it * 31 + seed * 7) xor (it ushr 9)).toByte() }

    // ------------------------------------------------------------------ basics

    @Test
    fun dfAndJsonLines() {
        val r = run("df")
        assertEquals(0, r.code)
        val d = r.events("device").single()
        assertEquals("usb", d["transport"]); assertEquals("HiBy R3 II", d["name"]); assertEquals("mounted", d["sd_state"])
        assertEquals(true, r.last["ok"]); assertEquals("mounted", r.last["sd_state"]); assertEquals("exfat", r.last["sd_fs"])
        assertEquals(127865454592L, r.last["sd_total"])
    }

    @Test
    fun usageErrors() {
        val r = run("frobnicate")
        assertEquals(1, r.code); assertEquals("usage", r.last["code"])
        assertEquals(1, run("ls", "--bogus").code)
        assertEquals(1, run("put", "only-one").code)
        assertEquals(1, run("ls", "a/../b").code)
    }

    @Test
    fun noDevice() {
        val closed = ServerSocket(0).use { it.localPort }
        val r = run("df", device = "127.0.0.1:$closed")
        assertEquals(2, r.code); assertEquals("no_device", r.last["code"])
        val r2 = run("df", device = null) // no adb, nothing paired
        assertEquals(2, r2.code)
    }

    @Test
    fun humanOutput() {
        val bout = ByteArrayOutputStream()
        val code = Cli(ByteArrayInputStream(ByteArray(0)), PrintStream(bout, true, "UTF-8"), PrintStream(ByteArrayOutputStream()), emptyMap())
            .run(arrayOf("--config", cfg.path, "--device", "127.0.0.1:$usbPort", "--human", "df"))
        assertEquals(0, code)
        val t = bout.toString("UTF-8")
        assertFalse(t.trimStart().startsWith("{"))
        assertTrue(t.contains("Done."))
    }

    @Test
    fun fileOperations() {
        assertEquals(0, run("mkdir", "-p", "Music/New/Album").code)
        File(dev.sd, "Music/New/Album/a.flac").writeBytes(data(10))
        File(dev.sd, "Music/New/.hidden").writeBytes(data(1))
        val ls = run("ls", "Music", "--recursive")
        assertEquals(0, ls.code)
        assertEquals(listOf("Music/New", "Music/New/Album", "Music/New/Album/a.flac"), ls.events("entry").map { it["path"] })
        assertEquals("file", ls.events("entry")[2]["kind"]); assertEquals(10L, ls.events("entry")[2]["size"])
        assertEquals(4, run("ls", "Music/New", "--all").events("entry").size + 2)
        assertEquals(0, run("mv", "Music/New/Album/a.flac", "Music/b.flac").code)
        assertTrue(File(dev.sd, "Music/b.flac").isFile)
        val mvExists = run("mv", "Music/b.flac", "Music/b.flac")
        assertEquals(4, mvExists.code); assertEquals("E_EXISTS", (mvExists.last["btp"] as Map<*, *>)["name"])
        val notEmpty = run("rm", "Music/New")
        assertEquals("E_NOT_EMPTY", (notEmpty.last["btp"] as Map<*, *>)["name"])
        // -r asks first: "no" keeps the folder
        val no = run("rm", "-r", "Music/New", stdin = ByteArrayInputStream("no\n".toByteArray()))
        assertEquals(7, no.code)
        assertEquals("confirm", no.events("prompt").single()["kind"])
        assertTrue(File(dev.sd, "Music/New").isDirectory)
        val yes = run("rm", "-r", "Music/New", stdin = ByteArrayInputStream("{\"answer\": true}\n".toByteArray()))
        assertEquals(0, yes.code)
        assertFalse(File(dev.sd, "Music/New").exists())
        assertEquals(listOf("Music/b.flac"), run("rm", "Music/b.flac", "--yes").last["deleted"])
        val nf = run("rm", "nope")
        assertEquals(4, nf.code); assertEquals(33L, (nf.last["btp"] as Map<*, *>)["code"])
    }

    // ------------------------------------------------------------------ put / get

    @Test
    fun putModes() {
        val f = File(dir, "01 Waterloo.flac"); f.writeBytes(data(300_000))
        val r = run("put", f.path, "Music/ABBA/", "--mkdir")
        assertEquals(r.raw, 0, r.code)
        assertEquals(1L, r.last["written"]); assertEquals(300_000L, r.last["bytes"])
        assertArrayEquals(data(300_000), File(dev.sd, "Music/ABBA/01 Waterloo.flac").readBytes())
        assertTrue(r.events("progress").any { it["op"] == "hash" } && r.events("progress").any { it["op"] == "put" })
        assertEquals(f.lastModified() / 1000, File(dev.sd, "Music/ABBA/01 Waterloo.flac").lastModified() / 1000)
        // CREATE refuses to replace
        val again = run("put", f.path, "Music/ABBA/")
        assertEquals(4, again.code); assertEquals("E_EXISTS", (again.last["btp"] as Map<*, *>)["name"])
        assertEquals("exists", again.events("warn").single()["code"])
        assertEquals("failed", ((again.last["items"] as List<*>)[0] as Map<*, *>)["result"])
        // identical: skipped
        val skip = run("put", f.path, "Music/ABBA/", "--skip-identical")
        assertEquals(0, skip.code); assertEquals(1L, skip.last["skipped"])
        // changed content: skip-identical refuses, overwrite replaces
        f.writeBytes(data(1000, 2))
        assertEquals(4, run("put", f.path, "Music/ABBA/", "--skip-identical").code)
        val over = run("put", f.path, "Music/ABBA/", "--overwrite")
        assertEquals(0, over.code)
        assertArrayEquals(data(1000, 2), File(dev.sd, "Music/ABBA/01 Waterloo.flac").readBytes())
        // exact remote path for one file
        assertEquals(0, run("put", f.path, "Renamed.flac").code)
        assertTrue(File(dev.sd, "Renamed.flac").isFile)
    }

    @Test
    fun putRecursiveAndInvalidNames() {
        val src = File(dir, "Album"); File(src, "CD1").mkdirs()
        File(src, "CD1/01.flac").writeBytes(data(5000)); File(src, "cover.jpg").writeBytes(data(100))
        // decomposed name: sent as NFC
        File(src, "Cancio\u0301n.mp3").writeBytes(data(10))
        val r = run("put", src.path, "Music", "--recursive")
        assertEquals(r.raw, 0, r.code)
        assertEquals(3L, r.last["written"])
        assertTrue(File(dev.sd, "Music/Album/CD1/01.flac").isFile)
        assertTrue(File(dev.sd, "Music/Album").list()!!.any { it == "Canci\u00f3n.mp3" || it == "Cancio\u0301n.mp3" })
        File(src, ".secret").writeBytes(data(1))
        val bad = run("put", src.path, "Other", "--recursive")
        assertEquals(1, bad.code)
        assertEquals("hidden", ((bad.last["invalid"] as List<*>)[0] as Map<*, *>)["reason"])
        assertFalse(File(dev.sd, "Other").exists()) // nothing sent
    }

    @Test
    fun putResumesAfterDroppedConnection() {
        val content = data(1_000_000, 3)
        val f = File(dir, "big.flac"); f.writeBytes(content)
        dev.dropPutAfter = 200_000
        val r = run("put", f.path, "big.flac")
        assertEquals(r.raw, 0, r.code)
        val resumed = r.events("warn").single { it["code"] == "resumed" }
        assertEquals(262144L, resumed["offset"])
        assertTrue(r.events("warn").any { it["code"] == "reconnecting" })
        assertArrayEquals(content, File(dev.sd, "big.flac").readBytes())
        assertTrue(dev.log.any { it.startsWith("DROPPED") })
        assertTrue(dev.sd.list()!!.none { it.endsWith(".part") })
    }

    @Test
    fun sdUnavailable() {
        dev.sdState = 2
        val f = File(dir, "x.mp3"); f.writeBytes(data(10))
        val r = run("put", f.path, "x.mp3")
        assertEquals(4, r.code); assertEquals("E_SD_UNAVAILABLE", (r.last["btp"] as Map<*, *>)["name"])
        val df = run("df")
        assertEquals(0, df.code); assertEquals("usb_storage", df.last["sd_state"]); assertEquals(0L, df.last["sd_total"])
        assertEquals("unavailable", df.events("device").single()["sd_state"])
    }

    @Test
    fun getVerifiesAndResumes() {
        val content = data(400_000, 5)
        File(dev.sd, "Music").mkdirs()
        val remote = File(dev.sd, "Music/song.flac"); remote.writeBytes(content); remote.setLastModified(1767225600000)
        val dst = File(dir, "out/song.flac")
        val r = run("get", "Music/song.flac", dst.path)
        assertEquals(r.raw, 0, r.code)
        assertArrayEquals(content, dst.readBytes())
        assertEquals(sha(content), ((r.last["items"] as List<*>)[0] as Map<*, *>)["sha256"])
        assertEquals(1767225600000, dst.lastModified())
        // existing file is kept without --overwrite
        assertEquals(5, run("get", "Music/song.flac", dst.path).code)
        // a leftover part with the same remote size and mtime resumes
        dst.delete()
        val part = File(dst.path + ".byos-part"); part.writeBytes(content.copyOf(100_000))
        File(part.path + ".json").writeText("{\"path\":\"Music/song.flac\",\"size\":400000,\"mtime\":1767225600}")
        val r2 = run("get", "Music/song.flac", dst.path)
        assertEquals(0, r2.code)
        assertEquals(100000L, r2.events("warn").single { it["code"] == "resumed" }["offset"])
        assertArrayEquals(content, dst.readBytes())
        assertFalse(part.exists())
        // recursive into a folder
        val r3 = run("get", "Music", File(dir, "tree").path + File.separator, "--recursive")
        assertEquals(0, r3.code)
        assertArrayEquals(content, File(dir, "tree/Music/song.flac").readBytes())
    }

    @Test
    fun cancelDownload() {
        File(dev.sd, "huge.bin").writeBytes(data(4_000_000))
        dev.getDelayMs = 30
        val pin = PipedInputStream(); val pout = PipedOutputStream(pin)
        Thread { Thread.sleep(400); pout.write("{\"cancel\": true}\n".toByteArray()); pout.flush() }.start()
        val r = run("get", "huge.bin", File(dir, "huge.bin").path, stdin = pin)
        assertEquals(r.raw, 7, r.code)
        assertEquals("cancelled", r.last["code"])
        assertTrue(dev.log.contains("CANCEL"))
    }

    // ------------------------------------------------------------------ pairing and Wi-Fi

    private fun pair(code: (String) -> String): Run {
        val pin = PipedInputStream(); val pout = PipedOutputStream(pin)
        Thread {
            val deadline = System.currentTimeMillis() + 5000
            while (dev.lastSas == null && System.currentTimeMillis() < deadline) Thread.sleep(10)
            pout.write((code(dev.lastSas ?: "") + "\n").toByteArray()); pout.flush()
        }.start()
        return run("--name", "Test PC", "connect", "--pair", stdin = pin, device = "127.0.0.1:$wifiPort")
    }

    @Test
    fun pairingThenEncryptedSessions() {
        // not paired: refused before anything else
        val before = run("df", device = "127.0.0.1:$wifiPort")
        assertEquals(3, before.code); assertEquals("auth", before.last["code"])
        // pairing closed on the HiBy
        val closed = pair { it }
        assertEquals(4, closed.code); assertEquals("E_PAIR_CLOSED", (closed.last["btp"] as Map<*, *>)["name"])
        dev.lastSas = null
        dev.pairingOpen = true
        val p = pair { it }
        assertEquals(p.raw, 0, p.code)
        val prompt = p.events("prompt").single()
        assertEquals("pair_code", prompt["kind"]); assertEquals(6L, prompt["digits"]); assertEquals("HiBy R3 II", prompt["device"])
        assertEquals("pair_wait", p.events("progress").single()["op"])
        assertEquals(true, p.last["paired"]); assertEquals(true, p.last["encrypted"]); assertEquals("wifi", p.last["transport"])
        assertTrue(dev.log.contains("PAIRED Test PC"))
        val devices = Json.arr(Json.parse(File(cfg, "devices.json").readText()))
        assertEquals(1, devices.size)
        assertEquals(dev.deviceId.joinToString("") { "%02x".format(it) }, Json.obj(devices[0])["id"])
        assertTrue(File(cfg, "client.key").length() == 32L)
        // reconnect: AUTH, encrypted file operations
        val f = File(dir, "w.flac"); f.writeBytes(data(150_000, 9))
        val put = run("put", f.path, "w.flac", device = "127.0.0.1:$wifiPort")
        assertEquals(put.raw, 0, put.code)
        assertArrayEquals(data(150_000, 9), File(dev.sd, "w.flac").readBytes())
        val st = run("status", device = "127.0.0.1:$wifiPort")
        assertEquals(0, st.code); assertEquals(true, st.last["encrypted"]); assertEquals("wifi", st.last["transport"])
        assertEquals(87L, st.last["battery"])
        // discovery finds it; --device wifi uses the announce
        val udp = dev.startDiscovery(wifiPort)
        val list = run("devices", "--wifi-only", "--wait", "500", "--discovery", "127.0.0.1:$udp", device = null)
        assertEquals(0, list.code)
        val d = list.events("device").single()
        assertEquals(true, d["paired"]); assertEquals(true, d["reachable"]); assertEquals(1L, list.last["count"])
        val w = run("df", "--discovery", "127.0.0.1:$udp", device = "wifi")
        assertEquals(w.raw, 0, w.code)
        assertEquals("wifi", w.events("device").single()["transport"])
        // the default device is the most recently used paired one
        assertEquals(0, run("df", "--discovery", "127.0.0.1:$udp", device = null).code)
        // forget: the next connection needs pairing again
        assertEquals(0, run("connect", "--forget", device = null).code)
        assertEquals(3, run("df", device = "127.0.0.1:$wifiPort").code)
    }

    @Test
    fun pairingWrongCodeAndDeny() {
        dev.pairingOpen = true
        val wrong = pair { if (it == "000000") "111111" else "000000" }
        assertEquals(3, wrong.code); assertEquals("code does not match", wrong.last["message"])
        Thread.sleep(100)
        assertTrue(dev.log.contains("PAIRFAIL mismatch"))
        assertFalse(File(cfg, "devices.json").exists())
        dev.lastSas = null
        dev.allowPairing = false
        val denied = pair { it }
        assertEquals(8, denied.code); assertEquals("rejected", denied.last["code"])
    }

    // ------------------------------------------------------------------ trust comes from the transport, not from the device

    private fun pairOk() {
        dev.pairingOpen = true
        val p = pair { it }
        assertEquals(p.raw, 0, p.code)
        dev.pairingOpen = false
    }

    private fun deadUdpPort() = java.net.DatagramSocket(0).use { it.localPort }

    @Test
    fun impostorClaimingUsbOverWifiIsRefused() {
        pairOk()
        val udp = dev.startDiscovery(wifiPort)
        dev.claimLocal = true // a device on the network that says "local": no AUTH and cleartext, if believed
        val f = File(dir, "secret.flac"); f.writeBytes(data(5000))
        dev.log.clear()
        val r = run("put", f.path, "secret.flac", "--discovery", "127.0.0.1:$udp", device = "wifi")
        assertEquals(r.raw, 4, r.code)
        assertEquals("E_PROTO", (r.last["btp"] as Map<*, *>)["name"])
        assertTrue(r.raw, (r.last["message"] as String).contains("claims a USB session"))
        assertFalse(dev.log.contains("PUT_BEGIN"))
        assertFalse(File(dev.sd, "secret.flac").exists())
        // the default device (the paired one, found by discovery) and pairing over Wi-Fi: refused too
        assertEquals(4, run("df", "--discovery", "127.0.0.1:$udp", device = null).code)
        assertEquals(4, run("connect", "--pair", "--discovery", "127.0.0.1:$udp", device = "wifi").code)
        // an explicit loopback address is the adb-forward path: it may run without AUTH
        val usb = run("df", device = "127.0.0.1:$wifiPort")
        assertEquals(usb.raw, 0, usb.code)
        assertEquals("usb", usb.events("device").single()["transport"])
    }

    @Test
    fun anotherDeviceAtTheStoredAddressIsRefused() {
        pairOk()
        val other = FakeDevice(File(dir, "sd2"), File(dir, "data2"))
        try {
            val otherPort = other.startWifi()
            val devices = File(cfg, "devices.json")
            devices.writeText(devices.readText().replace("127.0.0.1:$wifiPort", "127.0.0.1:$otherPort"))
            val r = run("df", "--discovery", "127.0.0.1:${deadUdpPort()}", device = "wifi")
            assertEquals(r.raw, 3, r.code)
            assertTrue(r.raw, (r.last["message"] as String).contains("different device"))
            assertFalse(other.log.contains("AUTH_INIT"))
        } finally {
            other.stop()
        }
    }

    @Test
    fun identityChangedOnlyWhenAuthFinishDoesNotOpen() {
        pairOk()
        dev.failAuthFinish = true
        val bad = run("df", device = "127.0.0.1:$wifiPort")
        assertEquals(bad.raw, 3, bad.code)
        assertTrue(bad.raw, (bad.last["message"] as String).contains("could not prove its identity"))
        // a link that stalls after AUTH_FINISH is a timeout, not an impostor
        dev.failAuthFinish = false
        dev.stallAfterAuthFinish = true
        val slow = run("--timeout", "800", "df", device = "127.0.0.1:$wifiPort")
        assertEquals(slow.raw, 9, slow.code)
        assertEquals("timeout", slow.last["code"])
    }

    @Test
    fun firmwareAtTheRootOnlyOverUsb() {
        val fw = File(dir, "r3ii_2025.UPT"); fw.writeBytes(data(3000))
        // known to be Wi-Fi before connecting: refused before hashing or connecting
        val w = run("put", fw.path, "/", "--discovery", "127.0.0.1:${deadUdpPort()}", device = "wifi")
        assertEquals(w.raw, 1, w.code); assertEquals("usage", w.last["code"])
        assertTrue(w.events("progress").isEmpty())
        // a network session found out only after connecting: refused before any PUT
        pairOk()
        dev.log.clear()
        val w2 = run("put", fw.path, "/", device = "127.0.0.1:$wifiPort")
        assertEquals(w2.raw, 1, w2.code)
        assertFalse(dev.log.contains("PUT_BEGIN"))
        // in a folder over Wi-Fi: allowed; at the root over USB: allowed
        assertEquals(0, run("put", fw.path, "Backup/", "--mkdir", device = "127.0.0.1:$wifiPort").code)
        val u = run("put", fw.path, "/")
        assertEquals(u.raw, 0, u.code)
        assertTrue(File(dev.sd, "r3ii_2025.UPT").isFile)
    }

    @Test
    fun getRefusesNamesThatLeaveTheFolder() {
        File(dev.sd, "Music").mkdirs(); File(dev.sd, "Music/a.flac").writeBytes(data(10))
        for (evil in listOf("..", "..\\..\\evil.bat", "x/../../evil.bat", "C:evil.bat")) {
            dev.extraListName = evil
            val r = run("get", "Music", File(dir, "tree").path + File.separator, "--recursive")
            assertEquals(r.raw, 4, r.code)
            assertEquals("E_PROTO", (r.last["btp"] as Map<*, *>)["name"])
        }
        assertFalse(File(dir, "evil.bat").exists()); assertFalse(File(dir.parentFile, "evil.bat").exists())
        // hidden names are fine in a listing
        dev.extraListName = ".hidden"
        assertEquals(0, run("ls", "Music", "--all").code)
        for (ok in listOf("a.flac", ".x", "Canción")) assertNull(ok, org.hibyos.install.core.btp.BtpPaths.checkName(ok))
        for (bad in listOf("", ".", "..", "...", "a/b", "a\\b", "c:", "a\u0001")) assertNotNull(bad, org.hibyos.install.core.btp.BtpPaths.checkName(bad))
    }

    @Test
    fun pingDuringUploadDoesNotStall() {
        dev.pingDuringPut = true
        val f = File(dir, "p.flac"); f.writeBytes(data(1_000_000, 7))
        val r = run("--timeout", "3000", "put", f.path, "p.flac")
        assertEquals(r.raw, 0, r.code)
        assertTrue(r.events("warn").none { it["code"] == "reconnecting" })
        assertTrue(dev.log.contains("PING")); assertTrue(dev.log.contains("PONG"))
        assertArrayEquals(data(1_000_000, 7), File(dev.sd, "p.flac").readBytes())
    }

    @Test
    fun heartbeatRepeatsProgressDuringLongWaits() {
        dev.putReadyDelayMs = 900
        val f = File(dir, "h.flac"); f.writeBytes(data(1000))
        val r = run("put", f.path, "h.flac", heartbeatMs = 100)
        assertEquals(r.raw, 0, r.code)
        val waits = r.events("progress").filter { it["op"] == "put" && it["done"] == 0L }
        assertTrue(r.raw, waits.size >= 4)
        assertEquals("result", r.lines.last()["event"])
    }

    // ------------------------------------------------------------------ USB through adb

    @Test
    fun usbThroughAdb() {
        val calls = ArrayList<List<String>>()
        val runner = ProcessRunner { cmd, _ ->
            calls += cmd
            when (cmd.drop(1).filter { it != "-s" && it != "SER123" }.firstOrNull()) {
                "devices" -> 0 to "List of devices attached\nSER123\tdevice product:r3 model:R3II transport_id:1\n\n"
                "forward" -> 0 to ""
                "shell" -> 0 to ""
                else -> 1 to "?"
            }
        }
        val env = mapOf("ADB" to "fake-adb")
        val r = run("--local-port", usbPort.toString(), "df", env = env, runner = runner, device = "usb")
        assertEquals(r.raw, 0, r.code)
        val d = r.events("device").single()
        assertEquals("usb", d["transport"]); assertEquals("SER123", d["serial"])
        assertTrue(calls.any { it.containsAll(listOf("forward", "tcp:$usbPort", "tcp:7340")) })
        assertTrue(calls.any { it.contains("--remove") })
        // default device: the one USB device
        assertEquals(0, run("--local-port", usbPort.toString(), "df", env = env, runner = runner, device = null).code)
        // nothing listening: daemon not running
        val closed = ServerSocket(0).use { it.localPort }
        val nr = run("--local-port", closed.toString(), "df", env = env, runner = runner, device = "usb")
        assertEquals(2, nr.code)
        assertEquals("daemon_not_running", nr.events("warn").single()["code"])
        // adb missing
        val na = run("df", device = "usb")
        assertEquals(2, na.code)
        assertTrue((na.last["message"] as String).contains("adb"))
    }

    // ------------------------------------------------------------------ library database

    private fun schema(): List<SchemaEntry> {
        val text = javaClass.getResource("/library/r3ii_2025_schema.sql")?.readText()
            ?: File(System.getenv("HIBYOS_REPO_HINT") ?: "../../..", "android/hibyos-install/core/src/test/resources/library/r3ii_2025_schema.sql").readText()
        val lines = text.lines().filter { it.isNotEmpty() }
        return lines.chunked(2).map { (h, s) -> val p = h.removePrefix("-- ").split(' '); SchemaEntry(p[0], p[1], p[2], 1, s.removeSuffix(";").ifEmpty { null }) }
    }

    @Test
    fun libraryRoundTrip() {
        // the device's live library
        dev.liveDb.parentFile.mkdirs()
        SqliteWriter(dev.liveDb).write(schema()) { if (it == "VERSION_TABLE") listOf(SqlRow(1, listOf(20181015L))) else if (it == "M3U_TABLE")
            listOf(SqlRow(1, listOf(1L, SqlText("a:\\Mezclas\\m.m3u\u0000"), SqlText("m.m3u\u0000"), SqlText("M")))) else emptyList() }
        val snap = File(dir, "snap.db")
        val pull = run("db-pull", snap.path)
        assertEquals(pull.raw, 0, pull.code)
        assertEquals(sha(dev.liveDb.readBytes()), pull.last["sha256"])
        assertTrue(File(snap.path + ".json").isFile)
        // build from synthetic music
        val music = File(dir, "card")
        SynthAudio.write(music, "Band/LP/01.flac", SynthAudio.flac(SynthAudio.Tags("One", "Band", "LP")))
        SynthAudio.write(music, "Band/LP/02.mp3", SynthAudio.mp3(SynthAudio.Tags("Two", "Band", "LP")))
        SynthAudio.write(music, "x.ogg", ByteArray(10))
        val newDb = File(dir, "new.db")
        val b = run("db-build", "--base", snap.path, "--music", music.path, "--out", newDb.path, "--report", File(dir, "rep.json").path, device = null)
        assertEquals(b.raw, 0, b.code)
        assertEquals(3L, b.last["tracks"]); assertEquals(2L, b.last["albums"]); assertEquals(pull.last["sha256"], b.last["base_sha256"])
        assertEquals("no_tags", b.events("warn").single()["code"])
        assertTrue(b.events("progress").any { it["op"] == "scan" })
        assertTrue(File(dir, "rep.json").readText().contains("kept_tables"))
        // push: stages it
        val push = run("db-push", newDb.path)
        assertEquals(push.raw, 0, push.code)
        assertEquals(true, push.last["staged"]); assertEquals(true, push.last["restart_needed"])
        assertArrayEquals(newDb.readBytes(), File(dev.data, "hibymod/sync/db/staged.db").readBytes())
        // the library changed on the HiBy: stale
        dev.liveDb.appendBytes(ByteArray(4096))
        val stale = run("db-push", newDb.path)
        assertEquals(4, stale.code)
        assertEquals("E_DB_STALE", (stale.last["btp"] as Map<*, *>)["name"])
        assertTrue((stale.last["message"] as String).contains("pull and build again"))
        assertEquals(0, run("db-push", newDb.path, "--force").code)
        // no base revision at all
        File(newDb.path + ".json").delete()
        assertEquals(1, run("db-push", newDb.path).code)
        val st = run("status")
        assertEquals(true, (st.last["db"] as Map<*, *>)["staged"])
    }

    // ------------------------------------------------------------------ updates

    private fun signedRelease(version: String, tar: ByteArray, keyHex: (String) -> Unit, restart: String = "shell"): ByteArray {
        val kp = KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
        val pub = kp.public.encoded.takeLast(32).toByteArray()
        keyHex(pub.joinToString("") { "%02x".format(it) })
        val zeros = "0".repeat(128)
        val text = """{"format": 1, "product": "hibyos", "version": "$version", "min_version": "0.6.0", "restart": "$restart",
 "payload": {"name": "hibyos-payload-$version.tar", "url": "https://github.com/Grohle/hibyos/releases/download/v$version/hibyos-payload-$version.tar",
 "size": ${tar.size}, "sha256": "${sha(tar)}", "compression": "none"}, "signature": "ed25519:$zeros"}"""
        val sig = Signature.getInstance("Ed25519").apply { initSign(kp.private); update(text.toByteArray()) }.sign()
        return text.replace(zeros, sig.joinToString("") { "%02x".format(it) }).toByteArray()
    }

    @Test
    fun updateSendAndInstall() {
        val tar = data(200_000, 4)
        var key = ""
        val json = signedRelease("0.8.1", tar, { key = it })
        dev.updateKeys = listOf(key)
        val jf = File(dir, "hibyos-update.json"); jf.writeBytes(json)
        val tf = File(dir, "hibyos-payload-0.8.1.tar"); tf.writeBytes(tar)
        // a wrong key: refused locally, nothing sent
        val bad = run("update-send", jf.path, tf.path)
        assertEquals(6, bad.code); assertEquals("verify", bad.last["code"])
        assertFalse(dev.log.contains("PUT_BEGIN"))
        // a damaged payload: refused locally
        val tf2 = File(dir, "damaged.tar"); tf2.writeBytes(data(200_000, 5))
        assertEquals(6, run("update-send", jf.path, tf2.path, keys = listOf(key)).code)
        // staged and installed with the shell restart
        val r = run("update-send", jf.path, tf.path, "--install", "--no-confirm", keys = listOf(key))
        assertEquals(r.raw, 0, r.code)
        assertEquals("0.8.1", r.last["version"]); assertEquals("shell", r.last["restart"]); assertEquals("installed", r.last["state"])
        assertEquals(false, r.last["needs_firmware"])
        assertTrue(r.events("progress").any { it["op"] == "install" && it["unit"] == "permille" })
        assertEquals("0.8.1", dev.byosVersion)
        assertTrue(File(dev.sd, "ByOS/ota/hibyos-update.json").isFile)
        assertArrayEquals(tar, File(dev.sd, "ByOS/ota/hibyos-payload-0.8.1.tar").readBytes())
        // same version again: downgrade refused by the device
        val again = run("update-send", jf.path, tf.path, keys = listOf(key))
        assertEquals(4, again.code); assertEquals("E_DOWNGRADE", (again.last["btp"] as Map<*, *>)["name"])
        // nothing staged anymore after an install? the staged JSON is still there: install is possible with --allow-downgrade
        assertEquals(0, run("update-install", "--no-wait", "--allow-downgrade", keys = listOf(key)).code)
    }

    @Test
    fun updateInstallWithoutStaging() {
        val r = run("update-install")
        assertEquals(4, r.code)
        assertEquals("E_NOT_STAGED", (r.last["btp"] as Map<*, *>)["name"])
    }

    @Test
    fun patchRefusesWrongFirmware() {
        val bundle = File(System.getenv("HIBYOS_REPO_HINT") ?: "../../..", "android/hibyos-install/app/src/main/assets/hibyos-bundle.zip")
        assumeTrue("the ByOS bundle is not built", bundle.isFile)
        val fw = File(dir, "junk.upt"); fw.writeBytes(data(100_000))
        val r = run("patch", "--firmware", fw.path, "--bundle", bundle.path, "--out", File(dir, "o.upt").path, device = null)
        assertEquals(r.raw, 6, r.code)
        assertEquals("verify", r.last["code"])
        assertFalse(File(dir, "o.upt").exists())
    }

    @Test
    fun firmwareFetchNeedsBundle() {
        val r = run("firmware-fetch", device = null)
        assertEquals(1, r.code)
        assertNull(r.last["btp"])
        assertNotNull(r.last["message"])
    }
}
