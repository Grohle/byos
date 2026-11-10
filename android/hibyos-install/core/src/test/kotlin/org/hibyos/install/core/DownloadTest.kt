package org.hibyos.install.core

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import java.io.File
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.util.Collections
import java.util.concurrent.atomic.AtomicBoolean

/** Downloads (firmware, newer HiByOS files) against a local web server. */
class DownloadTest {
    private lateinit var server: HttpServer
    private lateinit var dir: File
    private val requests: MutableList<String> = Collections.synchronizedList(ArrayList<String>())
    private val files = HashMap<String, ByteArray>()

    /** Close the connection after this many bytes of the next answer (once). */
    @Volatile private var breakAfter = -1
    @Volatile private var ignoreRange = false

    private val base: String get() = "http://127.0.0.1:${server.address.port}"

    @Before
    fun start() {
        dir = TestUtil.tempDir("dl-" + System.nanoTime())
        server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        server.createContext("/") { x -> serve(x) }
        server.start()
    }

    @After
    fun stop() {
        server.stop(0)
        dir.deleteRecursively()
    }

    private fun serve(x: HttpExchange) {
        val path = x.requestURI.path
        val range = x.requestHeaders.getFirst("Range")
        requests += path + (range?.let { " [$it]" } ?: "")
        try {
            when {
                path == "/page" -> {
                    val body = "<html><body>Google Drive can't scan this file for viruses</body></html>".toByteArray()
                    x.responseHeaders.add("Content-Type", "text/html; charset=utf-8")
                    x.sendResponseHeaders(200, body.size.toLong())
                    x.responseBody.use { it.write(body) }
                }
                path == "/redirect" -> {
                    x.responseHeaders.add("Location", "$base/fw.upt")
                    x.sendResponseHeaders(302, -1)
                }
                files.containsKey(path) -> {
                    val data = files[path]!!
                    var from = 0
                    if (range != null && !ignoreRange) {
                        from = Regex("bytes=(\\d+)-").find(range)!!.groupValues[1].toInt()
                        if (from >= data.size) {
                            x.sendResponseHeaders(416, -1)
                            return
                        }
                        x.responseHeaders.add("Content-Range", "bytes $from-${data.size - 1}/${data.size}")
                    }
                    x.responseHeaders.add("Content-Type", "application/octet-stream")
                    x.sendResponseHeaders(if (from > 0 || (range != null && !ignoreRange)) 206 else 200, (data.size - from).toLong())
                    val cut = breakAfter
                    breakAfter = -1
                    if (cut in 0 until data.size - from) {
                        x.responseBody.write(data, from, cut)
                        x.responseBody.flush()
                        x.close()   // fewer bytes than promised: the client sees a broken connection
                    } else {
                        x.responseBody.use { it.write(data, from, data.size - from) }
                    }
                }
                else -> x.sendResponseHeaders(404, -1)
            }
        } catch (e: Exception) {
            // the client went away
        } finally {
            x.close()
        }
    }

    private fun expect(kind: DownloadException.Kind, block: () -> Unit): DownloadException {
        try {
            block()
        } catch (e: DownloadException) {
            assertEquals(e.message, kind, e.kind)
            return e
        }
        fail("expected $kind")
        throw AssertionError()
    }

    private fun spec(data: ByteArray, vararg sources: String) =
        FirmwareSpec("Test player, test firmware", "fw.upt", md5Hex(data), sha256Hex(data), data.size.toLong(), "9.9", sources.toList())

    // ------------------------------------------------------------------ plain downloads

    @Test
    fun downloadsAFile() {
        val data = TestData.gen("copies", 21, 3_000_000)
        files["/fw.upt"] = data
        val dest = File(dir, "fw.upt")
        var last = 0L
        val n = Downloader.fetch("$base/fw.upt", dest, data.size.toLong()) { done, total ->
            assertEquals(data.size.toLong(), total)
            assertTrue(done >= last)
            last = done
        }
        assertEquals(data.size.toLong(), n)
        assertEquals(data.size.toLong(), last)
        assertArrayEquals(data, dest.readBytes())
        assertFalse(File(dir, "fw.upt.part").exists())
        // through a redirect, and without knowing the size
        dest.delete()
        Downloader.fetch("$base/redirect", dest, null)
        assertArrayEquals(data, dest.readBytes())
    }

    @Test
    fun continuesABrokenDownload() {
        val data = TestData.gen("random", 22, 2_000_000)
        files["/fw.upt"] = data
        val dest = File(dir, "fw.upt")
        breakAfter = 700_000
        val e = expect(DownloadException.Kind.OFFLINE) { Downloader.fetch("$base/fw.upt", dest, data.size.toLong()) }
        assertTrue(e.message!!, e.message!!.contains("broke after"))
        assertFalse(dest.exists())
        val part = File(dir, "fw.upt.part")
        val have = part.length()
        assertTrue("kept $have bytes", have in 1..700_000)
        var first = -1L
        Downloader.fetch("$base/fw.upt", dest, data.size.toLong()) { done, _ -> if (first < 0) first = done }
        assertArrayEquals(data, dest.readBytes())
        assertTrue("the second try started at $first, after the $have bytes it had", first > have)
        assertEquals(listOf("/fw.upt", "/fw.upt [bytes=$have-]"), requests.toList())
    }

    @Test
    fun startsAgainWhenTheServerCannotContinue() {
        val data = TestData.gen("random", 23, 500_000)
        files["/fw.upt"] = data
        val dest = File(dir, "fw.upt")
        File(dir, "fw.upt.part").writeBytes(ByteArray(100_000) { 5 })   // junk from another try
        ignoreRange = true
        Downloader.fetch("$base/fw.upt", dest, data.size.toLong())
        assertArrayEquals(data, dest.readBytes())
    }

    @Test
    fun refusesAWebPageAndWrongSizes() {
        val dest = File(dir, "fw.upt")
        val e = expect(DownloadException.Kind.NOT_A_FILE) { Downloader.fetch("$base/page", dest, 1000) }
        assertTrue(e.message!!, e.message!!.contains("web page"))
        files["/fw.upt"] = ByteArray(999)
        val e2 = expect(DownloadException.Kind.NOT_A_FILE) { Downloader.fetch("$base/fw.upt", dest, 1000) }
        assertTrue(e2.message!!, e2.message!!.contains("999 bytes, not the expected 1000"))
        expect(DownloadException.Kind.NOT_A_FILE) { Downloader.fetch("$base/fw.upt", dest, null, maxSize = 500) }
        assertEquals(404, expect(DownloadException.Kind.HTTP) { Downloader.fetch("$base/nothing", dest, 1000) }.status)
        assertFalse(dest.exists())
        assertFalse(File(dir, "fw.upt.part").exists())
    }

    @Test
    fun reportsNoConnectionAndCancel() {
        val port = ServerSocket(0).use { it.localPort }
        val dest = File(dir, "fw.upt")
        expect(DownloadException.Kind.OFFLINE) { Downloader.fetch("http://127.0.0.1:$port/fw.upt", dest, 1000) }
        expect(DownloadException.Kind.OFFLINE) { Downloader.text("http://127.0.0.1:$port/index.json") }
        val data = TestData.gen("random", 24, 4_000_000)
        files["/fw.upt"] = data
        val stop = AtomicBoolean(false)
        expect(DownloadException.Kind.CANCELLED) {
            Downloader.fetch("$base/fw.upt", dest, data.size.toLong(), cancelled = { stop.get() }) { done, _ -> if (done > 500_000) stop.set(true) }
        }
        assertFalse(dest.exists())
        // what was received is kept for the next try
        assertTrue(File(dir, "fw.upt.part").length() > 500_000)
    }

    // ------------------------------------------------------------------ the firmware

    @Test
    fun fetchesAndChecksTheFirmware() {
        val data = TestData.gen("copies", 25, 1_200_000)
        files["/fw.upt"] = data
        val dest = File(dir, "fw.upt")
        var checked = 0.0
        val id = FirmwareFetcher.fetch(spec(data, "$base/fw.upt"), dest, onCheck = { checked = it })
        assertEquals(md5Hex(data), id.md5)
        assertEquals(sha256Hex(data), id.sha256)
        assertEquals(1.0, checked, 0.0)
        // the first link shows a page (as Google Drive does when it wants a confirmation): the next one is used
        dest.delete()
        requests.clear()
        FirmwareFetcher.fetch(spec(data, "$base/page", "$base/nothing", "$base/fw.upt"), dest)
        assertArrayEquals(data, dest.readBytes())
        assertEquals(listOf("/page", "/nothing", "/fw.upt"), requests.toList())
        // no link works: the last reason is reported
        dest.delete()
        expect(DownloadException.Kind.HTTP) { FirmwareFetcher.fetch(spec(data, "$base/page", "$base/nothing"), dest) }
        expect(DownloadException.Kind.NOT_A_FILE) { FirmwareFetcher.fetch(spec(data), dest) }
        assertFalse(dest.exists())
    }

    @Test
    fun refusesAnotherFirmware() {
        val wanted = TestData.gen("copies", 26, 300_000)
        val other = wanted.clone().also { it[150_000] = (it[150_000] + 1).toByte() }   // same size, one byte changed
        files["/fw.upt"] = other
        val dest = File(dir, "fw.upt")
        val e = expect(DownloadException.Kind.MISMATCH) { FirmwareFetcher.fetch(spec(wanted, "$base/fw.upt"), dest) }
        assertTrue(e.message!!, e.message!!.contains("is not Test player, test firmware") && e.message!!.contains(md5Hex(other)))
        assertFalse("a wrong file is not kept", dest.exists())
    }

    // ------------------------------------------------------------------ newer HiByOS files

    private val seed = ByteArray(32) { (it * 5 + 9).toByte() }
    private val keys: List<String> get() = listOf(Hex.of(TestSigner.publicKey(seed)))

    private fun index(version: String, url: String, size: Long, sha: String, format: Int = 1, seed: ByteArray = this.seed): ByteArray =
        TestSigner.signJson(seed, "{\n \"format\": $format,\n \"product\": \"hibyos-install-bundle\",\n \"hibyos_version\": \"$version\",\n" +
            " \"url\": \"$url\",\n \"size\": $size,\n \"sha256\": \"$sha\",\n \"signature\": \"ed25519:${"0".repeat(128)}\"\n}\n")

    @Test
    fun comparesVersions() {
        assertTrue(BundleUpdater.isNewer("0.6.2", "0.6.1"))
        assertTrue(BundleUpdater.isNewer("0.10.0", "0.9.3"))
        assertTrue(BundleUpdater.isNewer("1.0", "0.99.99"))
        assertTrue(BundleUpdater.isNewer("v0.7", "0.6.1"))
        assertTrue(BundleUpdater.isNewer("0.6.1.1", "0.6.1"))
        assertFalse(BundleUpdater.isNewer("0.6.1", "0.6.1"))
        assertFalse(BundleUpdater.isNewer("0.6", "0.6.0"))
        assertFalse(BundleUpdater.isNewer("0.6.0", "0.6.1"))
    }

    @Test
    fun readsOnlyASignedIndex() {
        val prefix = "https://example.invalid/releases/download/"
        val sha = "cd".repeat(32)
        val good = index("9.9.10", prefix + "v9.9.10/b.zip", 5000, sha)
        val r = BundleUpdater.parseIndex(good, keys, prefix)
        assertEquals("9.9.10", r.version)
        assertEquals(prefix + "v9.9.10/b.zip", r.url)
        assertEquals(5000L, r.size)
        assertEquals(sha, r.sha256)
        fun untrusted(data: ByteArray, k: List<String> = keys, p: String = prefix) =
            expect(DownloadException.Kind.UNTRUSTED) { BundleUpdater.parseIndex(data, k, p) }
        // signed by somebody else, not signed, changed after signing, no keys in the app
        val other = ByteArray(32) { 3 }
        assertTrue(untrusted(index("9.9.10", prefix + "b.zip", 5000, sha, seed = other)).message!!.contains("not signed by HiByOS"))
        untrusted(String(good).replace(Regex("ed25519:[0-9a-f]{128}"), "ed25519:" + "0".repeat(128)).toByteArray())
        untrusted(String(good).replace("5000", "5001").toByteArray())
        untrusted(String(good).replace("b.zip", "c.zip").toByteArray())
        untrusted(good, emptyList())
        untrusted(good, listOf("0".repeat(64)))
        untrusted("<html>Not Found</html>".toByteArray())
        // properly signed but not acceptable: link elsewhere, format of a later app, bad fields
        assertTrue(untrusted(index("9.9.10", "https://evil.invalid/b.zip", 5000, sha)).message!!.contains("unexpected link"))
        untrusted(good, p = "https://example.invalid/other/")
        assertTrue(untrusted(index("9.9.10", prefix + "b.zip", 5000, sha, format = 2)).message!!.contains("newer version of the app"))
        untrusted(index("9.9.10", prefix + "b.zip", 0, sha))
        untrusted(index("9.9.10", prefix + "b.zip", BundleUpdater.MAX_BUNDLE + 1, sha))
        untrusted(index("9.9.10", prefix + "b.zip", 5000, "xyz"))
        untrusted(index("9 9", prefix + "b.zip", 5000, sha))
        untrusted(TestSigner.signJson(seed, "{\"format\": 1, \"signature\": \"ed25519:${"0".repeat(128)}\"}"))
        untrusted(TestSigner.signJson(seed, "[\"signature\": \"ed25519:${"0".repeat(128)}\"]"))
    }

    private fun makeBundle(name: String, change: (Synthetic.BundleSpec) -> Unit = {}): ByteArray {
        val s = Synthetic.BundleSpec(firmwareMd5 = "0".repeat(32))
        s.overlay += Triple("etc/hello", "hello\n".toByteArray(), 420)
        s.sd += "HiByOS/VERSION" to "9.9.9\n".toByteArray()
        change(s)
        return Synthetic.bundle(File(dir, name), s).readBytes()
    }

    @Test
    fun updatesTheHiByOSFilesFromASignedRelease() {
        val data = makeBundle("made.zip")
        files["/download/v9.9.9/bundle.zip"] = data
        val prefix = "$base/download/"
        files["/latest/index.json"] = index("9.9.9", prefix + "v9.9.9/bundle.zip", data.size.toLong(), sha256Hex(data))
        val remote = BundleUpdater.check("$base/latest/index.json", keys, prefix)
        assertEquals("9.9.9", remote.version)
        val dest = File(dir, "current.zip")
        dest.writeText("the bundle in use")
        var last = 0L
        BundleUpdater.download(remote, dest) { done, _ -> last = done }
        assertEquals(data.size.toLong(), last)
        assertArrayEquals(data, dest.readBytes())
        Bundle.open(dest).use { assertEquals("9.9.9", it.hibyosVersion) }
        assertFalse(File(dir, "current.zip.new").exists())
        // an index that nobody signed is refused before anything is downloaded
        requests.clear()
        files["/latest/index.json"] = String(files["/latest/index.json"]!!).replace("9.9.9\"", "9.9.8\"").toByteArray()
        expect(DownloadException.Kind.UNTRUSTED) { BundleUpdater.check("$base/latest/index.json", keys, prefix) }
        assertEquals(listOf("/latest/index.json"), requests.toList())
        expect(DownloadException.Kind.HTTP) { BundleUpdater.check("$base/latest/none.json", keys, prefix) }
    }

    @Test
    fun keepsTheOldFilesWhenTheNewOnesAreWrong() {
        val data = makeBundle("made.zip")
        val prefix = "$base/download/"
        val dest = File(dir, "current.zip")
        fun unchanged(kind: DownloadException.Kind, remote: RemoteBundle): DownloadException {
            dest.writeText("the bundle in use")
            val e = expect(kind) { BundleUpdater.download(remote, dest) }
            assertEquals("the bundle in use", dest.readText())
            assertFalse(File(dir, "current.zip.new").exists())
            return e
        }
        // other contents than the signed ones (same size)
        files["/download/a.zip"] = data.clone().also { it[it.size / 2] = (it[it.size / 2] + 1).toByte() }
        unchanged(DownloadException.Kind.MISMATCH, RemoteBundle("9.9.9", prefix + "a.zip", data.size.toLong(), sha256Hex(data)))
        // another size than the signed one
        files["/download/b.zip"] = data.copyOf(data.size - 1)
        unchanged(DownloadException.Kind.NOT_A_FILE, RemoteBundle("9.9.9", prefix + "b.zip", data.size.toLong(), sha256Hex(data)))
        // correctly signed, but not the version it says
        files["/download/c.zip"] = data
        val e = unchanged(DownloadException.Kind.MISMATCH, RemoteBundle("9.9.10", prefix + "c.zip", data.size.toLong(), sha256Hex(data)))
        assertTrue(e.message!!, e.message!!.contains("not version 9.9.10"))
        // correctly signed, but a file inside is not what its own list says
        val broken = makeBundle("broken.zip") { it.stored = { n, b -> if (n == "sd/HiByOS/VERSION") "9.9.8\n".toByteArray() else b } }
        files["/download/d.zip"] = broken
        unchanged(DownloadException.Kind.MISMATCH, RemoteBundle("9.9.9", prefix + "d.zip", broken.size.toLong(), sha256Hex(broken)))
        // correctly signed, but for a later version of the app
        val later = makeBundle("later.zip") { it.format = 2 }
        files["/download/e.zip"] = later
        val e2 = unchanged(DownloadException.Kind.MISMATCH, RemoteBundle("9.9.9", prefix + "e.zip", later.size.toLong(), sha256Hex(later)))
        assertTrue(e2.message!!, e2.message!!.contains("newer version of the app"))
        // not a zip at all
        files["/download/f.zip"] = ByteArray(4000) { 7 }
        unchanged(DownloadException.Kind.MISMATCH, RemoteBundle("9.9.9", prefix + "f.zip", 4000, sha256Hex(ByteArray(4000) { 7 })))
        // the server is gone
        unchanged(DownloadException.Kind.HTTP, RemoteBundle("9.9.9", prefix + "none.zip", 4000, "0".repeat(64)))
    }
}
