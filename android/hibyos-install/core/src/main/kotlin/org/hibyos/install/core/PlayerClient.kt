package org.hibyos.install.core

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.URLEncoder
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * Client of the player's own "Import Music via Wi-Fi" server: the small web server that the
 * official firmware starts on port 4399 while that screen is open. It speaks what HiBy's own
 * web page speaks (usr/share/web/pages/index-EN.js in the firmware):
 *
 *   GET  /hostname                    model name, plain text
 *   GET  /list?path=DIR               JSON array of {name, path, size (files only), ctime}
 *   POST /create   path=DIR           new folder (409 when it is already there)
 *   POST /upload   multipart: path=DIR, files[]=the file
 *   POST /delete   path=PATH
 *   POST /move     oldPath=, newPath=
 *   GET  /download?path=FILE
 *
 * Paths are absolute on the player; the SD card is [DEFAULT_HOME]. The page allows .upt
 * files and folders, which is all an installation needs.
 *
 * Plain sockets on purpose: the server is HTTP/1.0 style (one request per connection), and
 * an upload must be streamed, stopped at once when cancelled, and its error answer read even
 * when the player closes the connection half way.
 */
class TransferException(
    val kind: Kind, message: String, val status: Int = 0, val serverText: String? = null, cause: Throwable? = null,
) : Exception(message, cause) {
    enum class Kind {
        /** Nothing answers at that address and port. */
        UNREACHABLE,

        /** Something answers, but it is not the player's Wi-Fi transfer. */
        NOT_A_PLAYER,

        /** The player answers but has no usable SD card. */
        NO_SD_CARD,

        /** The player answered a request with an error. */
        REFUSED,

        /** The connection broke or stalled. */
        INTERRUPTED,

        /** What is on the player is not what was sent. */
        MISMATCH,
        CANCELLED,

        /** A file on this phone could not be read. */
        LOCAL,
    }
}

class RemoteEntry(val name: String, val path: String, val size: Long?) {
    val isDir: Boolean get() = size == null
}

class PlayerInfo(val host: String, val port: Int, val name: String?, val sdCard: Boolean, val sdText: String?)

class HttpReply(val status: Int, val reason: String, val headers: Map<String, String>, val body: ByteArray) {
    val text: String get() = String(body, Charsets.UTF_8)

    /** The start of the answer, as one printable line (for error messages). */
    fun brief(): String = text.replace(Regex("<[^>]*>"), " ").replace(Regex("\\s+"), " ").trim().take(160)
}

class PlayerClient(
    val host: String,
    val port: Int = DEFAULT_PORT,
    val home: String = DEFAULT_HOME,
    private val connectTimeoutMs: Int = 5000,
    private val readTimeoutMs: Int = 20000,
    /** How long an upload may make no progress at all before it is given up. */
    private val stallTimeoutMs: Int = 45000,
    /** How long the player may take to answer after the last byte of an upload. */
    private val finishTimeoutMs: Int = 180000,
    private val socketFactory: () -> Socket = { Socket() },
) {
    companion object {
        const val DEFAULT_PORT = 4399
        const val DEFAULT_HOME = "/data/mnt/sd_0/"
        private const val MAX_TEXT_BODY = 8 shl 20

        internal fun enc(s: String): String = URLEncoder.encode(s, "UTF-8")

        /** Accepts "192.168.1.20", "192.168.1.20:4399" and what the player shows, "http://192.168.1.20:4399". */
        fun parseAddress(text: String): Pair<String, Int>? {
            var t = text.trim()
            if (t.startsWith("http://", true)) t = t.substring(7)
            t = t.substringBefore('/').trim()
            if (t.isEmpty() || t.length > 253) return null
            var port = DEFAULT_PORT
            val colon = t.lastIndexOf(':')
            var hostPart = t
            if (colon >= 0) {
                port = t.substring(colon + 1).toIntOrNull() ?: return null
                hostPart = t.substring(0, colon)
            }
            if (port !in 1..65535) return null
            if (!Regex("[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?").matches(hostPart)) return null
            if (Regex("[0-9.]+").matches(hostPart)) {
                val parts = hostPart.split('.')
                if (parts.size != 4 || parts.any { it.isEmpty() || it.length > 3 || it.toInt() > 255 }) return null
            }
            return hostPart to port
        }
    }

    // ------------------------------------------------------------------ plain HTTP

    private fun connect(): Socket {
        val s = try {
            socketFactory()
        } catch (e: IOException) {
            throw TransferException(TransferException.Kind.UNREACHABLE, "no network: ${e.message}", cause = e)
        }
        try {
            s.connect(InetSocketAddress(host, port), connectTimeoutMs)
            s.soTimeout = readTimeoutMs
            s.tcpNoDelay = true
            return s
        } catch (e: Exception) {
            try { s.close() } catch (x: IOException) { /* nothing to do */ }
            if (e is IOException || e is IllegalArgumentException || e is SecurityException) {
                throw TransferException(TransferException.Kind.UNREACHABLE, "nothing answers at $host:$port", cause = e)
            }
            throw e
        }
    }

    private fun head(method: String, target: String, extra: String = ""): ByteArray =
        ("$method $target HTTP/1.1\r\nHost: $host:$port\r\nUser-Agent: ByOS-Install\r\nAccept: */*\r\n" +
            "Connection: close\r\n$extra\r\n").toByteArray(Charsets.ISO_8859_1)

    private fun line(input: InputStream, limit: Int = 8192): String? {
        val sb = StringBuilder()
        while (true) {
            val c = input.read()
            if (c < 0) return if (sb.isEmpty()) null else sb.toString()
            if (c == '\n'.code) return sb.toString().trimEnd('\r')
            sb.append(c.toChar())
            if (sb.length > limit) throw TransferException(TransferException.Kind.NOT_A_PLAYER, "$host:$port does not answer like the player")
        }
    }

    /** Reads the answer; the body goes to [sink] (with [onBytes]) or, without one, into the reply. */
    private fun readReply(input: InputStream, sink: OutputStream? = null, onBytes: ((Int) -> Unit)? = null,
                          cancelled: () -> Boolean = { false }): HttpReply {
        val notPlayer = { TransferException(TransferException.Kind.NOT_A_PLAYER, "$host:$port does not answer like the player") }
        val first = line(input) ?: throw TransferException(TransferException.Kind.INTERRUPTED, "the player closed the connection without answering")
        val m = Regex("HTTP/1\\.[01] (\\d{3})(?: (.*))?").matchEntire(first) ?: throw notPlayer()
        val status = m.groupValues[1].toInt()
        val headers = HashMap<String, String>()
        var count = 0
        while (true) {
            val l = line(input) ?: break
            if (l.isEmpty()) break
            if (++count > 100) throw notPlayer()
            val colon = l.indexOf(':')
            if (colon > 0) headers[l.substring(0, colon).trim().lowercase()] = l.substring(colon + 1).trim()
        }
        val toSink = sink != null && status in 200..299
        val mem = ByteArrayOutputStream()
        val buf = ByteArray(1 shl 16)
        var total = 0L
        fun take(n: Int) {
            if (toSink) {
                sink!!.write(buf, 0, n)
                onBytes?.invoke(n)
            } else {
                if (total + n > MAX_TEXT_BODY) throw notPlayer()
                mem.write(buf, 0, n)
            }
            total += n
        }
        fun copy(limit: Long): Long {
            var left = limit
            while (left > 0) {
                if (cancelled()) throw TransferException(TransferException.Kind.CANCELLED, "cancelled")
                val n = input.read(buf, 0, minOf(left, buf.size.toLong()).toInt())
                if (n < 0) break
                take(n)
                left -= n
            }
            return limit - left
        }
        val length = headers["content-length"]?.toLongOrNull()
        if (headers["transfer-encoding"]?.contains("chunked", true) == true) {
            while (true) {
                val sizeLine = line(input) ?: throw TransferException(TransferException.Kind.INTERRUPTED, "the connection to the player broke")
                val n = sizeLine.substringBefore(';').trim().toLongOrNull(16) ?: throw notPlayer()
                if (n == 0L) break
                if (copy(n) != n) throw TransferException(TransferException.Kind.INTERRUPTED, "the connection to the player broke")
                line(input)
            }
        } else if (length != null) {
            if (length < 0) throw notPlayer()
            if (copy(length) != length) throw TransferException(TransferException.Kind.INTERRUPTED, "the connection to the player broke")
        } else {
            copy(Long.MAX_VALUE)
        }
        return HttpReply(status, m.groupValues[2], headers, mem.toByteArray())
    }

    private fun <T> io(what: String, block: () -> T): T = try {
        block()
    } catch (e: TransferException) {
        throw e
    } catch (e: SocketTimeoutException) {
        throw TransferException(TransferException.Kind.INTERRUPTED, "the player stopped answering ($what)", cause = e)
    } catch (e: IOException) {
        throw TransferException(TransferException.Kind.INTERRUPTED, "the connection to the player broke ($what)", cause = e)
    }

    fun get(target: String): HttpReply = io("GET $target") {
        connect().use { s ->
            s.getOutputStream().apply { write(head("GET", target)); flush() }
            readReply(BufferedInputStream(s.getInputStream()))
        }
    }

    private fun post(target: String, fields: List<Pair<String, String>>): HttpReply = io("POST $target") {
        val body = fields.joinToString("&") { enc(it.first) + "=" + enc(it.second) }.toByteArray(Charsets.UTF_8)
        connect().use { s ->
            s.getOutputStream().apply {
                write(head("POST", target, "Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n" +
                    "X-Requested-With: XMLHttpRequest\r\nContent-Length: ${body.size}\r\n"))
                write(body)
                flush()
            }
            readReply(BufferedInputStream(s.getInputStream()))
        }
    }

    private fun refused(what: String, r: HttpReply): TransferException {
        val text = r.brief()
        val kind = when {
            text.contains("Home not found", true) -> TransferException.Kind.NO_SD_CARD
            r.status == 404 && !r.headers.getOrDefault("content-type", "").contains("json") &&
                text.contains("not found", true) && text.contains("URL", true) -> TransferException.Kind.NOT_A_PLAYER
            else -> TransferException.Kind.REFUSED
        }
        return TransferException(kind, "$what: the player answered ${r.status}" + (if (text.isEmpty()) "" else " ($text)"),
            r.status, text.ifEmpty { null })
    }

    // ------------------------------------------------------------------ the player's requests

    /** The model name the player gives ("R3II_2025"), or null when it gives none. */
    fun hostname(): String? {
        val r = get("/hostname")
        if (r.status != 200) return null
        val t = r.text.trim()
        return if (t.isNotEmpty() && t.length <= 64 && t.all { it in ' '..'~' } && !t.contains('<')) t else null
    }

    fun list(dir: String): List<RemoteEntry> {
        val r = get("/list?path=" + enc(dir))
        if (r.status !in 200..299) throw refused("reading $dir", r)
        val arr = try {
            Json.arr(Json.parse(r.text))
        } catch (e: JsonException) {
            throw TransferException(TransferException.Kind.NOT_A_PLAYER, "$host:$port does not answer like the player")
        }
        return arr.mapNotNull {
            val m = it as? Map<*, *> ?: return@mapNotNull null
            val name = (m["name"] as? String)?.trimEnd('/') ?: return@mapNotNull null
            if (name.isEmpty()) return@mapNotNull null
            val path = m["path"] as? String ?: (dir + name)
            val size = when (val s = m["size"]) {
                is Long -> s
                is Double -> s.toLong()
                else -> null
            }
            // as in the player's page: an entry without a size is a folder
            RemoteEntry(name, path, if (path.endsWith("/")) null else size)
        }
    }

    /** Creates a folder ([dir] ends in "/"). True if it was created, false if it was already there. */
    fun createDir(dir: String): Boolean {
        val r = post("/create", listOf("path" to dir))
        if (r.status == 409) return false
        if (r.status !in 200..299) throw refused("creating the folder $dir", r)
        return true
    }

    fun delete(path: String) {
        val r = post("/delete", listOf("path" to path))
        if (r.status !in 200..299) throw refused("deleting $path", r)
    }

    fun move(oldPath: String, newPath: String) {
        val r = post("/move", listOf("oldPath" to oldPath, "newPath" to newPath))
        if (r.status !in 200..299) throw refused("renaming $oldPath", r)
    }

    /** Asks the player who it is and whether its SD card is there. */
    fun identify(): PlayerInfo {
        val name = try {
            hostname()
        } catch (e: TransferException) {
            if (e.kind == TransferException.Kind.UNREACHABLE) throw e
            null
        }
        return try {
            list(home)
            PlayerInfo(host, port, name, true, null)
        } catch (e: TransferException) {
            when {
                e.kind == TransferException.Kind.NO_SD_CARD -> PlayerInfo(host, port, name, false, e.serverText)
                // the name file is there and the list request is known but fails: a player without a card
                e.kind == TransferException.Kind.REFUSED && name != null -> PlayerInfo(host, port, name, false, e.serverText)
                e.kind == TransferException.Kind.REFUSED || e.kind == TransferException.Kind.INTERRUPTED ->
                    throw TransferException(TransferException.Kind.NOT_A_PLAYER, "$host:$port does not answer like the player", e.status, e.serverText)
                else -> throw e
            }
        }
    }

    /**
     * Sends one file into [dir] (which ends in "/" and must exist), as the player's web page
     * does. [onBytes] gets the number of file bytes sent so far.
     */
    fun upload(dir: String, name: String, size: Long, open: () -> InputStream,
               cancelled: () -> Boolean = { false }, onBytes: ((Long) -> Unit)? = null) {
        if (name.isEmpty() || name.any { it == '/' || it == '\\' || it < ' ' }) {
            throw TransferException(TransferException.Kind.LOCAL, "bad file name: $name")
        }
        val boundary = "----ByOSInstall" + java.lang.Long.toHexString(System.nanoTime()) + java.lang.Long.toHexString(size)
        val pre = ("--$boundary\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n$dir\r\n" +
            "--$boundary\r\nContent-Disposition: form-data; name=\"files[]\"; filename=\"${name.replace("\"", "%22")}\"\r\n" +
            "Content-Type: application/octet-stream\r\n\r\n").toByteArray(Charsets.UTF_8)
        val post = "\r\n--$boundary--\r\n".toByteArray(Charsets.UTF_8)
        val total = pre.size + size + post.size

        val s = connect()
        val lastMove = AtomicLong(System.nanoTime())
        val finished = AtomicBoolean(false)
        val stalled = AtomicBoolean(false)
        val stopped = AtomicBoolean(false)
        val sending = AtomicBoolean(true)
        val watchdog = Thread {
            while (!finished.get()) {
                try { Thread.sleep(200) } catch (e: InterruptedException) { return@Thread }
                val idle = (System.nanoTime() - lastMove.get()) / 1_000_000
                val limit = if (sending.get()) stallTimeoutMs else finishTimeoutMs
                if (cancelled()) stopped.set(true) else if (idle > limit) stalled.set(true) else continue
                try { s.close() } catch (e: IOException) { /* nothing to do */ }
                return@Thread
            }
        }
        watchdog.isDaemon = true
        watchdog.start()
        try {
            val out = s.getOutputStream()
            val input = BufferedInputStream(s.getInputStream())
            var sent = 0L
            var localError: IOException? = null
            var early = false
            try {
                out.write(head("POST", "/upload", "Content-Type: multipart/form-data; boundary=$boundary\r\n" +
                    "X-Requested-With: XMLHttpRequest\r\nContent-Length: $total\r\n"))
                out.write(pre)
                val buf = ByteArray(1 shl 15)
                val src = try {
                    open()
                } catch (e: IOException) {
                    localError = e
                    throw e
                }
                src.use {
                    while (sent < size) {
                        if (cancelled()) throw TransferException(TransferException.Kind.CANCELLED, "cancelled")
                        // An answer before the end of the file can only be a refusal: stop and read it
                        // while it can still be read.
                        if (input.available() > 0) {
                            early = true
                            break
                        }
                        val n = try {
                            it.read(buf, 0, minOf(buf.size.toLong(), size - sent).toInt())
                        } catch (e: IOException) {
                            localError = e
                            throw e
                        }
                        if (n < 0) {
                            localError = IOException("the file is shorter than expected")
                            throw localError!!
                        }
                        out.write(buf, 0, n)
                        sent += n
                        lastMove.set(System.nanoTime())
                        onBytes?.invoke(sent)
                    }
                }
                if (!early) {
                    out.write(post)
                    out.flush()
                }
                sending.set(false)
                lastMove.set(System.nanoTime())
            } catch (e: IOException) {
                if (stopped.get()) throw TransferException(TransferException.Kind.CANCELLED, "cancelled")
                localError?.let { throw TransferException(TransferException.Kind.LOCAL, "the file could not be read on this phone: ${it.message}", cause = it) }
                if (stalled.get()) throw TransferException(TransferException.Kind.INTERRUPTED, "the player stopped receiving after $sent of $size bytes", cause = e)
                // The player closed the connection: it may have said why.
                val why = try {
                    s.soTimeout = 3000
                    readReply(input)
                } catch (x: Exception) {
                    null
                }
                if (why != null && why.status >= 400) throw refused("sending $name", why)
                throw TransferException(TransferException.Kind.INTERRUPTED, "the connection broke after $sent of $size bytes", cause = e)
            }
            val r = try {
                s.soTimeout = finishTimeoutMs
                readReply(input)
            } catch (e: IOException) {
                if (stopped.get()) throw TransferException(TransferException.Kind.CANCELLED, "cancelled")
                throw TransferException(TransferException.Kind.INTERRUPTED, "the player did not confirm $name", cause = e)
            }
            if (r.status !in 200..299) throw refused("sending $name", r)
            if (early) {
                throw TransferException(TransferException.Kind.INTERRUPTED,
                    "the player answered before it had the whole file ($sent of $size bytes)", r.status, r.brief().ifEmpty { null })
            }
        } finally {
            finished.set(true)
            watchdog.interrupt()
            try { s.close() } catch (e: IOException) { /* nothing to do */ }
        }
    }

    /** Reads a file back from the player through [md]; returns its length. */
    fun digest(path: String, md: MessageDigest, cancelled: () -> Boolean = { false }, onBytes: ((Int) -> Unit)? = null): Long =
        io("reading back $path") {
            connect().use { s ->
                s.getOutputStream().apply { write(head("GET", "/download?path=" + enc(path))); flush() }
                var total = 0L
                val sink = object : OutputStream() {
                    override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)
                    override fun write(b: ByteArray, off: Int, len: Int) {
                        md.update(b, off, len)
                        total += len
                    }
                }
                val r = readReply(BufferedInputStream(s.getInputStream()), sink, onBytes, cancelled)
                if (r.status !in 200..299) throw refused("reading back $path", r)
                total
            }
        }
}
