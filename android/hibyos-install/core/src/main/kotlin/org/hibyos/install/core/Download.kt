package org.hibyos.install.core

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL

class DownloadException(val kind: Kind, message: String, val status: Int = 0, cause: Throwable? = null) : Exception(message, cause) {
    enum class Kind {
        /** No connection, or it broke. What was received is kept, so trying again continues. */
        OFFLINE,

        /** The server answered with an error. */
        HTTP,

        /** The link gives a web page (or something of another size) instead of the file. */
        NOT_A_FILE,

        /** The file arrived but it is not the expected one. */
        MISMATCH,

        /** The index of the HiByOS files is not signed by HiByOS, or cannot be understood. */
        UNTRUSTED,
        STORAGE,
        CANCELLED,
    }
}

/** Plain HTTPS downloads to a file, with resume. */
object Downloader {
    private const val USER_AGENT = "HiByOS-Install"

    private fun open(url: String, range: Long): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 15000
        c.readTimeout = 30000
        c.instanceFollowRedirects = true
        c.setRequestProperty("User-Agent", USER_AGENT)
        c.setRequestProperty("Accept-Encoding", "identity")
        if (range > 0) c.setRequestProperty("Range", "bytes=$range-")
        return c
    }

    /** A small text resource (at most [max] bytes). */
    fun text(url: String, max: Int = 1 shl 20, cancelled: () -> Boolean = { false }): ByteArray {
        val c = try {
            open(url, 0)
        } catch (e: IOException) {
            throw DownloadException(DownloadException.Kind.OFFLINE, "no connection: ${e.message}", cause = e)
        }
        try {
            val status = c.responseCode
            if (status != 200) throw DownloadException(DownloadException.Kind.HTTP, "the server answered $status", status)
            val out = java.io.ByteArrayOutputStream()
            val buf = ByteArray(8192)
            c.inputStream.use {
                while (true) {
                    if (cancelled()) throw DownloadException(DownloadException.Kind.CANCELLED, "cancelled")
                    val n = it.read(buf)
                    if (n < 0) break
                    if (out.size() + n > max) throw DownloadException(DownloadException.Kind.NOT_A_FILE, "the answer is too long")
                    out.write(buf, 0, n)
                }
            }
            return out.toByteArray()
        } catch (e: IOException) {
            throw DownloadException(DownloadException.Kind.OFFLINE, "the connection failed: ${e.message}", cause = e)
        } finally {
            c.disconnect()
        }
    }

    /**
     * Downloads [url] to [dest] through "[dest].part". With [expectedSize], a server that
     * announces another size or a web page is refused before anything is downloaded, and a
     * partial file left by an earlier try is continued.
     */
    fun fetch(url: String, dest: File, expectedSize: Long?, maxSize: Long = 512L shl 20,
              cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null): Long {
        val part = File(dest.path + ".part")
        dest.absoluteFile.parentFile?.mkdirs()
        var have = if (part.isFile) part.length() else 0L
        if (expectedSize == null || have >= expectedSize) have = 0
        val c = try {
            open(url, have)
        } catch (e: IOException) {
            throw DownloadException(DownloadException.Kind.OFFLINE, "no connection: ${e.message}", cause = e)
        }
        try {
            val status = try {
                c.responseCode
            } catch (e: IOException) {
                throw DownloadException(DownloadException.Kind.OFFLINE, "no connection: ${e.message}", cause = e)
            }
            if (status == 416) {
                part.delete()
                throw DownloadException(DownloadException.Kind.OFFLINE, "the download has to start again")
            }
            if (status != 200 && status != 206) {
                throw DownloadException(DownloadException.Kind.HTTP, "the server answered $status", status)
            }
            val type = (c.contentType ?: "").lowercase()
            if (type.startsWith("text/html")) {
                throw DownloadException(DownloadException.Kind.NOT_A_FILE, "the link shows a web page instead of the file")
            }
            if (status == 206) {
                val from = Regex("bytes (\\d+)-").find(c.getHeaderField("Content-Range") ?: "")?.groupValues?.get(1)?.toLongOrNull()
                if (from != have) {
                    part.delete()
                    throw DownloadException(DownloadException.Kind.OFFLINE, "the download has to start again")
                }
            } else {
                have = 0
            }
            val length = c.getHeaderField("Content-Length")?.toLongOrNull()
            val total = if (length != null) have + length else expectedSize ?: -1L
            if (expectedSize != null && length != null && total != expectedSize) {
                throw DownloadException(DownloadException.Kind.NOT_A_FILE,
                    "the link offers a file of $total bytes, not the expected $expectedSize")
            }
            if (total > maxSize) throw DownloadException(DownloadException.Kind.NOT_A_FILE, "the file is too big")
            var done = have
            try {
                RandomAccessFile(part, "rw").use { raf ->
                    raf.setLength(have)
                    raf.seek(have)
                    val buf = ByteArray(1 shl 16)
                    val input = try {
                        c.inputStream
                    } catch (e: IOException) {
                        throw DownloadException(DownloadException.Kind.OFFLINE, "the connection failed: ${e.message}", cause = e)
                    }
                    input.use {
                        while (true) {
                            if (cancelled()) throw DownloadException(DownloadException.Kind.CANCELLED, "cancelled")
                            val n = try {
                                it.read(buf)
                            } catch (e: IOException) {
                                throw DownloadException(DownloadException.Kind.OFFLINE, "the connection broke after $done bytes", cause = e)
                            }
                            if (n < 0) break
                            done += n
                            if (done > maxSize || (expectedSize != null && done > expectedSize)) {
                                throw DownloadException(DownloadException.Kind.NOT_A_FILE, "the server sends more than the expected file")
                            }
                            raf.write(buf, 0, n)
                            progress?.invoke(done, total)
                        }
                    }
                }
            } catch (e: DownloadException) {
                if (e.kind == DownloadException.Kind.NOT_A_FILE) part.delete()
                throw e
            } catch (e: IOException) {
                throw DownloadException(DownloadException.Kind.STORAGE, "the file could not be saved: ${e.message}", cause = e)
            }
            if (total >= 0 && done < total) {
                throw DownloadException(DownloadException.Kind.OFFLINE, "the connection broke after $done of $total bytes")
            }
            dest.delete()
            if (!part.renameTo(dest)) throw DownloadException(DownloadException.Kind.STORAGE, "the file could not be saved")
            return done
        } finally {
            c.disconnect()
        }
    }
}

/** Gets the official firmware from HiBy's own links and checks that it is the right file. */
object FirmwareFetcher {
    /**
     * Tries the links of [spec] in order; the file is kept only if its size, MD5 and SHA-256
     * are the expected ones. [onCheck] reports the checking of the finished download (0..1).
     */
    fun fetch(spec: FirmwareSpec, dest: File, cancelled: () -> Boolean = { false },
              progress: ((Long, Long) -> Unit)? = null, onCheck: ((Double) -> Unit)? = null): FirmwareId {
        if (spec.sources.isEmpty()) throw DownloadException(DownloadException.Kind.NOT_A_FILE, "no direct link is known for this firmware")
        var last: DownloadException? = null
        for (url in spec.sources) {
            try {
                Downloader.fetch(url, dest, spec.size, cancelled = cancelled, progress = progress)
                val id = try {
                    Patcher.identify(dest, cancelled, onCheck)
                } catch (e: PatchException) {
                    throw DownloadException(DownloadException.Kind.CANCELLED, "cancelled")
                } catch (e: IOException) {
                    throw DownloadException(DownloadException.Kind.STORAGE, "the downloaded file could not be read: ${e.message}", cause = e)
                }
                if (!id.matches(spec)) {
                    dest.delete()
                    throw DownloadException(DownloadException.Kind.MISMATCH,
                        "the downloaded file is not ${spec.label} (MD5 ${id.md5})")
                }
                return id
            } catch (e: DownloadException) {
                // A broken connection is worth another try with the same link, not with the next one.
                if (e.kind == DownloadException.Kind.CANCELLED || e.kind == DownloadException.Kind.OFFLINE ||
                    e.kind == DownloadException.Kind.STORAGE) throw e
                last = e
            }
        }
        throw last!!
    }
}

/** A newer set of HiByOS files published with a release of the project. */
class RemoteBundle(val version: String, val url: String, val size: Long, val sha256: String)

/**
 * Newer HiByOS files from the project's releases, so that a new HiByOS version does not need
 * a new version of this app. The index is a small JSON signed with the project's release key
 * (the same Ed25519 scheme as the player's own updater, docs/OTA.md); it carries the size and
 * SHA-256 of the bundle, so the one signature covers the bundle too. The keys come from the
 * bundle built into the app, never from the network.
 */
object BundleUpdater {
    const val MAX_BUNDLE = 96L shl 20

    /** True if version [a] is newer than [b] ("0.10.0" is newer than "0.9.3"). */
    fun isNewer(a: String, b: String): Boolean {
        fun parts(v: String) = v.trim().removePrefix("v").split('.', '-', '+').map { it.toIntOrNull() ?: 0 }
        val x = parts(a)
        val y = parts(b)
        for (i in 0 until maxOf(x.size, y.size)) {
            val d = x.getOrElse(i) { 0 }.compareTo(y.getOrElse(i) { 0 })
            if (d != 0) return d > 0
        }
        return false
    }

    /** Checks the signature of the index and reads it. [allowedPrefix] is where bundles may come from. */
    fun parseIndex(data: ByteArray, keys: List<String>, allowedPrefix: String): RemoteBundle {
        val untrusted = { why: String -> DownloadException(DownloadException.Kind.UNTRUSTED, why) }
        if (!Ed25519.verifySignedJson(data, keys)) throw untrusted("the list of HiByOS files is not signed by HiByOS")
        try {
            val m = Json.obj(Json.parse(String(data, Charsets.UTF_8)))
            if (Json.long(m, "format") != Bundle.FORMAT.toLong()) {
                throw untrusted("the newer HiByOS files need a newer version of the app")
            }
            val url = Json.str(m, "url")
            if (!url.startsWith(allowedPrefix) || url.any { it <= ' ' }) throw untrusted("unexpected link in the list of HiByOS files")
            val sha = Json.str(m, "sha256").lowercase()
            val size = Json.long(m, "size")
            val version = Json.str(m, "hibyos_version")
            if (!Regex("[0-9a-f]{64}").matches(sha) || size <= 0 || size > MAX_BUNDLE ||
                !Regex("[0-9A-Za-z.+-]{1,32}").matches(version)) throw untrusted("the list of HiByOS files is not valid")
            return RemoteBundle(version, url, size, sha)
        } catch (e: JsonException) {
            throw untrusted("the list of HiByOS files is not valid (${e.message})")
        }
    }

    /** Asks the project's releases which bundle is the latest one. */
    fun check(indexUrl: String, keys: List<String>, allowedPrefix: String, cancelled: () -> Boolean = { false }): RemoteBundle =
        parseIndex(Downloader.text(indexUrl, 65536, cancelled), keys, allowedPrefix)

    /**
     * Downloads [remote] to [dest]. The file is kept only if its size and SHA-256 are the
     * signed ones, it opens as a bundle of this format, every file in it matches its own
     * list and it is the announced version.
     */
    fun download(remote: RemoteBundle, dest: File, cancelled: () -> Boolean = { false },
                 progress: ((Long, Long) -> Unit)? = null): File {
        val tmp = File(dest.path + ".new")
        tmp.delete()
        Downloader.fetch(remote.url, tmp, remote.size, MAX_BUNDLE, cancelled, progress)
        try {
            val id = Patcher.identify(tmp, cancelled)
            if (id.size != remote.size || id.sha256 != remote.sha256) {
                throw DownloadException(DownloadException.Kind.MISMATCH, "the downloaded HiByOS files are damaged")
            }
            try {
                Bundle.open(tmp).use {
                    it.verifyAll()
                    if (it.hibyosVersion != remote.version) {
                        throw DownloadException(DownloadException.Kind.MISMATCH, "the downloaded HiByOS files are not version ${remote.version}")
                    }
                }
            } catch (e: BundleException) {
                throw DownloadException(DownloadException.Kind.MISMATCH, e.message ?: "the downloaded HiByOS files are damaged")
            }
            dest.delete()
            if (!tmp.renameTo(dest)) throw DownloadException(DownloadException.Kind.STORAGE, "the HiByOS files could not be saved")
            return dest
        } catch (e: PatchException) {
            throw DownloadException(DownloadException.Kind.CANCELLED, "cancelled")
        } finally {
            tmp.delete()
        }
    }
}
