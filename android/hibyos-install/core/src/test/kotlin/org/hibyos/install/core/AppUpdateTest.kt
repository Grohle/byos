package org.hibyos.install.core

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import java.io.File
import java.net.InetSocketAddress
import java.util.Collections

/** The apps' own updates: release list, signed index, verified download (against a local server). */
class AppUpdateTest {
    private lateinit var server: HttpServer
    private lateinit var dir: File
    private val requests: MutableList<String> = Collections.synchronizedList(ArrayList<String>())
    private val files = HashMap<String, ByteArray>()

    @Volatile private var apiStatus = 200
    @Volatile private var apiHeaders = mapOf<String, String>()

    private val base: String get() = "http://127.0.0.1:${server.address.port}"
    private val api: String get() = "$base/api/releases"
    private val downloads: String get() = "$base/download/"

    private val seed = ByteArray(32) { (it * 7 + 3).toByte() }
    private val keys: List<String> get() = listOf(Hex.of(TestSigner.publicKey(seed)))

    @Before
    fun start() {
        dir = TestUtil.tempDir("app-" + System.nanoTime())
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
        requests += path
        try {
            if (path == "/api/moved") {
                x.responseHeaders.add("Location", "$base/api/releases")
                x.sendResponseHeaders(301, -1)
                return
            }
            if (path == "/api/releases" && apiStatus != 200) {
                apiHeaders.forEach { (k, v) -> x.responseHeaders.add(k, v) }
                x.sendResponseHeaders(apiStatus, -1)
                return
            }
            val body = files[path]
            if (body == null) {
                x.sendResponseHeaders(404, -1)
                return
            }
            x.responseHeaders.add("Content-Type", "application/octet-stream")
            x.sendResponseHeaders(200, body.size.toLong())
            x.responseBody.use { it.write(body) }
        } finally {
            x.close()
        }
    }

    private fun release(tag: String, assets: List<String> = listOf(AppUpdater.INDEX_NAME), draft: Boolean = false, pre: Boolean = false) =
        "{\"tag_name\":\"$tag\",\"draft\":$draft,\"prerelease\":$pre,\"body\":\"notes\",\"assets\":[" +
            assets.joinToString(",") { "{\"name\":\"$it\",\"browser_download_url\":\"https://evil.invalid/$it\"}" } + "]}"

    private fun list(vararg r: String) = ("[" + r.joinToString(",") + "]").toByteArray()

    private fun index(
        version: String, size: Long, sha: String, app: String = "byos-install-android", tag: String = "install-v$version",
        asset: String = "hibyos-install-$version.apk", code: String = "\"version_code\": 5,", format: Int = 1,
        product: String = "byos-app-update", seed: ByteArray = this.seed,
    ): ByteArray = TestSigner.signJson(seed, "{\n \"format\": $format,\n \"product\": \"$product\",\n \"app\": \"$app\",\n" +
        " \"version\": \"$version\",\n $code\n \"tag\": \"$tag\",\n \"asset\": \"$asset\",\n \"size\": $size,\n" +
        " \"sha256\": \"$sha\",\n \"min_os\": \"26\",\n \"published\": \"2026-11-12\",\n" +
        " \"notes\": {\"en\": \"Faster sync.\", \"es\": \"Sincronización más rápida.\", \"fr\": \"Synchro plus rapide.\"},\n" +
        " \"signature\": \"ed25519:${"0".repeat(128)}\"\n}\n")

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

    @Test
    fun picksTheNewestReleaseOfTheApp() {
        val l = list(
            release("v0.9.1", listOf("hibyos-update.json", "hibyos-install-bundle.json")), // the firmware: "latest"
            release("install-v1.1.0"),
            release("install-v1.10.0", draft = true),
            release("install-v1.9.0", pre = true),
            release("install-v1.2.0"),
            release("install-v1.3.0", listOf("hibyos-install-1.3.0.apk")), // no index: skipped
            release("install-v1.2.0-rc1"),
            release("desktop-v2.0.0"),
            "\"not a release\"",
        )
        val c = AppUpdater.newestRelease(l, AppId.ANDROID)!!
        assertEquals("install-v1.2.0", c.tag)
        assertEquals("1.2.0", c.version)
        assertEquals("desktop-v2.0.0", AppUpdater.newestRelease(l, AppId.DESKTOP_WINDOWS)!!.tag)
        assertNull(AppUpdater.newestRelease(list(release("v0.9.1")), AppId.ANDROID))
        assertNull(AppUpdater.newestRelease(list(), AppId.ANDROID))
        expect(DownloadException.Kind.NOT_A_FILE) { AppUpdater.newestRelease("<html>".toByteArray(), AppId.ANDROID) }
        assertEquals("1.10.0", AppUpdater.tagVersion(AppId.ANDROID, "install-v1.10.0"))
        assertNull(AppUpdater.tagVersion(AppId.ANDROID, "install-v1"))
        assertNull(AppUpdater.tagVersion(AppId.ANDROID, "install-v1.2.0-beta"))
        assertNull(AppUpdater.tagVersion(AppId.ANDROID, "desktop-v1.2.0"))
        assertTrue(AppUpdater.isNewer("1.10.0", "1.9.9"))
        assertFalse(AppUpdater.isNewer("1.1.0", "1.1.0"))
        assertFalse(AppUpdater.isNewer("1.0.9", "1.1.0"))
    }

    @Test
    fun readsOnlyASignedIndexOfThisAppAndRelease() {
        val sha = "ab".repeat(32)
        val good = index("1.2.0", 4000, sha)
        val r = AppUpdater.parseIndex(good, keys, AppId.ANDROID, "install-v1.2.0", "https://x.invalid/dl/")
        assertEquals("1.2.0", r.version)
        assertEquals(5L, r.versionCode)
        assertEquals("hibyos-install-1.2.0.apk", r.asset)
        assertEquals(4000L, r.size)
        assertEquals(sha, r.sha256)
        assertEquals("26", r.minOs)
        assertEquals("https://x.invalid/dl/install-v1.2.0/hibyos-install-1.2.0.apk", r.url)
        assertEquals("Synchro plus rapide.", r.notesFor("fr-FR"))
        assertEquals("Faster sync.", r.notesFor("de"))

        fun untrusted(data: ByteArray, app: AppId = AppId.ANDROID, tag: String = "install-v1.2.0", k: List<String> = keys) =
            expect(DownloadException.Kind.UNTRUSTED) { AppUpdater.parseIndex(data, k, app, tag) }
        // not signed by the release key, changed after signing, no key
        assertTrue(untrusted(index("1.2.0", 4000, sha, seed = ByteArray(32) { 1 })).message!!.contains("not signed"))
        untrusted(String(good).replace("4000", "4001").toByteArray())
        untrusted(String(good).replace("Faster", "Slower").toByteArray())
        untrusted(good, k = emptyList())
        untrusted(good, k = listOf("0".repeat(64)))
        untrusted("<html>Not Found</html>".toByteArray())
        // signed, but for another app, another release, or with bad fields
        untrusted(good, app = AppId.DESKTOP_WINDOWS, tag = "install-v1.2.0")
        untrusted(index("1.2.0", 4000, sha, app = "byos-desktop-windows"))
        untrusted(good, tag = "install-v1.3.0") // an old index replayed under a newer tag
        untrusted(index("1.2.0", 4000, sha, tag = "install-v1.2.1"))
        untrusted(index("1.2.0", 4000, sha, product = "hibyos-install-bundle"))
        assertTrue(untrusted(index("1.2.0", 4000, sha, format = 2)).message!!.contains("newer version"))
        untrusted(index("1.2.0", 4000, sha, asset = "../evil.apk"))
        untrusted(index("1.2.0", 4000, sha, asset = "hibyos-install-1.2.0.exe"))
        untrusted(index("1.2.0", 4000, sha, asset = "a/b.apk"))
        untrusted(index("1.2.0", 0, sha))
        untrusted(index("1.2.0", AppUpdater.MAX_ASSET + 1, sha))
        untrusted(index("1.2.0", 4000, "xyz"))
        untrusted(index("1.2.0", 4000, sha, code = "")) // Android needs its versionCode
        untrusted(index("1.2.0", 4000, sha, code = "\"version_code\": 0,"))
        // the desktop app has no versionCode
        val d = index("0.9.2", 4000, sha, app = "byos-desktop-windows", tag = "desktop-v0.9.2", asset = "ByOS.exe", code = "")
        assertNull(AppUpdater.parseIndex(d, keys, AppId.DESKTOP_WINDOWS, "desktop-v0.9.2").versionCode)
    }

    @Test
    fun updatesFromTheReleasesAndChecksTheFile() {
        val apk = ByteArray(70_000) { (it * 31 + 7).toByte() }
        val sha = sha256Hex(apk)
        files["/api/releases"] = list(release("v0.9.1"), release("install-v1.2.0"), release("install-v1.1.0"))
        files["/download/install-v1.2.0/byos-app-update.json"] = index("1.2.0", apk.size.toLong(), sha)
        files["/download/install-v1.2.0/hibyos-install-1.2.0.apk"] = apk

        val r = AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, api, downloads)!!
        assertEquals("1.2.0", r.version)
        assertEquals("$downloads" + "install-v1.2.0/hibyos-install-1.2.0.apk", r.url)
        // up to date: the index is not even downloaded
        requests.clear()
        assertNull(AppUpdater.check(AppId.ANDROID, "1.2.0", 5, keys, api, downloads))
        assertEquals(listOf("/api/releases"), requests.toList())
        // newer name but not a higher versionCode: Android would refuse it
        assertNull(AppUpdater.check(AppId.ANDROID, "1.1.0", 5, keys, api, downloads))
        // GitHub's redirect for a renamed repository
        assertEquals("1.2.0", AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, "$base/api/moved", downloads)!!.version)

        val dest = File(dir, "update.apk")
        var last = 0L
        AppUpdater.download(r, dest) { done, _ -> last = done }
        assertArrayEquals(apk, dest.readBytes())
        assertEquals(apk.size.toLong(), last)
        assertFalse(File(dir, "update.apk.new").exists())
        // already there and right: not downloaded again
        requests.clear()
        AppUpdater.download(r, dest)
        assertTrue(requests.isEmpty())

        // a file that is not the signed one is not kept
        dest.delete()
        files["/download/install-v1.2.0/hibyos-install-1.2.0.apk"] = apk.clone().also { it[100] = (it[100] + 1).toByte() }
        expect(DownloadException.Kind.MISMATCH) { AppUpdater.download(r, dest) }
        assertFalse(dest.exists())
        assertFalse(File(dir, "update.apk.new").exists())
        files["/download/install-v1.2.0/hibyos-install-1.2.0.apk"] = apk.copyOf(apk.size - 1)
        expect(DownloadException.Kind.NOT_A_FILE) { AppUpdater.download(r, dest) }
        assertFalse(dest.exists())

        // an index that was changed is refused before the file is downloaded
        files["/download/install-v1.2.0/byos-app-update.json"] =
            String(index("1.2.0", apk.size.toLong(), sha)).replace("\"size\": ${apk.size}", "\"size\": ${apk.size + 1}").toByteArray()
        requests.clear()
        expect(DownloadException.Kind.UNTRUSTED) { AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, api, downloads) }
        assertFalse(requests.any { it.endsWith(".apk") })
        // a release without its index file on the server
        files.remove("/download/install-v1.2.0/byos-app-update.json")
        expect(DownloadException.Kind.HTTP) { AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, api, downloads) }
    }

    @Test
    fun saysWhenGitHubLimitsRequests() {
        apiStatus = 403
        apiHeaders = mapOf("X-RateLimit-Remaining" to "0", "X-RateLimit-Reset" to (System.currentTimeMillis() / 1000 + 600).toString())
        val e = expect(DownloadException.Kind.HTTP) { AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, api, downloads) }
        assertEquals(429, e.status)
        assertTrue(e.message, e.message!!.contains("10 min"))
        apiStatus = 429
        apiHeaders = mapOf("Retry-After" to "60")
        assertEquals(429, expect(DownloadException.Kind.HTTP) { AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, api, downloads) }.status)
        apiStatus = 500
        apiHeaders = emptyMap()
        assertEquals(500, expect(DownloadException.Kind.HTTP) { AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, api, downloads) }.status)
        // no server at all
        expect(DownloadException.Kind.OFFLINE) { AppUpdater.check(AppId.ANDROID, "1.1.0", 2, keys, "http://127.0.0.1:1/x", downloads) }
    }

    @Test
    fun theBuiltInKeysAreThePlayersReleaseKeys() {
        assertEquals(org.hibyos.install.core.btp.UpdatePackage.RELEASE_KEYS, AppUpdater.KEYS)
        assertTrue(AppUpdater.KEYS.isNotEmpty() && AppUpdater.KEYS.all { Regex("[0-9a-f]{64}").matches(it) })
        assertTrue(AppUpdater.RELEASES_API.startsWith("https://api.github.com/repos/"))
        assertTrue(AppUpdater.DOWNLOADS.startsWith("https://github.com/"))
    }
}
