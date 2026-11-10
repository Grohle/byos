package org.hibyos.install.core

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.File
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import java.util.Collections

/**
 * A stand-in for the player's Wi-Fi transfer server, written from the protocol that HiBy's
 * own web page speaks (usr/share/web/pages/index-EN.js of the official firmware) and from
 * the configuration of its web server (etc/thttpd.conf: port 4399, CGI programs, answers
 * without Content-Length, one request per connection).
 *
 * What the page fixes: the request names and fields, the JSON listing (name, path, size for
 * files only, ctime), "409 = the folder already exists" and the SD card being /data/mnt/sd_0/.
 * What it does not fix, and is therefore an assumption here: the exact error codes and texts
 * (the texts are the ones found as strings in upload.cgi), and that an upload over an existing
 * name replaces it. The switches below make the server misbehave in the ways a real one could.
 */
class MockPlayer(private val sd: File?) : Closeable {
    private val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
    val port: Int get() = server.localPort
    val log: MutableList<String> = Collections.synchronizedList(ArrayList<String>())

    @Volatile var hostname: String? = "R3II_2025"
    /** Free space left on the card. */
    @Volatile var freeBytes: Long = Long.MAX_VALUE
    /** Close the connection after this many bytes of an upload (once). */
    @Volatile var dropUploadAfter: Long = -1
    /** Stop reading an upload after this many bytes and never answer (once). */
    @Volatile var stallUploadAfter: Long = -1
    /** Take the whole upload and never answer (once). */
    @Volatile var neverConfirm = false
    /** Answer the upload with an error as soon as it starts, without reading it. */
    @Volatile var failEarly = false
    /** Extensions the server accepts (upper case), or null for everything. */
    @Volatile var onlyTypes: Set<String>? = null
    /** Store one byte less / one byte changed (once). */
    @Volatile var storeShort = false
    @Volatile var storeDamaged = false
    /** /download does not work. */
    @Volatile var noDownload = false
    /** Answer everything with a web page, like some other device on the network. */
    @Volatile var impostor = false
    /** Answer everything with bytes that are not HTTP. */
    @Volatile var garbage = false
    /** Keep the old file when a name is taken, storing the new one as "name (1)". */
    @Volatile var renameOnClash = false

    private val home = "/data/mnt/sd_0/"
    private val thread = Thread {
        while (!server.isClosed) {
            val s = try {
                server.accept()
            } catch (e: Exception) {
                break
            }
            Thread { s.use { handle(it) } }.apply { isDaemon = true }.start()
        }
    }.apply { isDaemon = true; start() }

    override fun close() {
        server.close()
        thread.interrupt()
    }

    fun uploads(): List<String> = synchronized(log) { log.filter { it.startsWith("UPLOAD ") } }

    private fun readLine(i: InputStream): String? {
        val sb = StringBuilder()
        while (true) {
            val c = i.read()
            if (c < 0) return if (sb.isEmpty()) null else sb.toString()
            if (c == '\n'.code) return sb.toString().trimEnd('\r')
            sb.append(c.toChar())
        }
    }

    private fun answer(s: Socket, status: Int, body: String, type: String = "application/json; charset=utf-8") {
        val reason = mapOf(200 to "OK", 400 to "Bad Request", 403 to "Forbidden", 404 to "Not Found", 409 to "Conflict",
            500 to "Internal Server Error")[status] ?: "Error"
        try {
            s.getOutputStream().apply {
                write("HTTP/1.0 $status $reason\r\nServer: thttpd\r\nContent-Type: $type\r\n\r\n$body".toByteArray())
                flush()
            }
        } catch (e: Exception) {
            // the client went away
        }
    }

    private fun local(path: String): File? {
        if (sd == null || !path.startsWith(home) || path.contains("/../") || path.endsWith("/..")) return null
        return File(sd, path.substring(home.length))
    }

    private fun form(body: String): Map<String, String> = body.split('&').filter { it.contains('=') }.associate {
        URLDecoder.decode(it.substringBefore('='), "UTF-8") to URLDecoder.decode(it.substringAfter('='), "UTF-8")
    }

    private fun used(): Long = sd?.walkTopDown()?.filter { it.isFile }?.sumOf { it.length() } ?: 0L

    private fun handle(s: Socket) {
        try {
            val input = BufferedInputStream(s.getInputStream())
            if (garbage) {
                s.getOutputStream().write("SSH-2.0-dropbear\r\n".toByteArray())
                return
            }
            val first = readLine(input) ?: return
            val (method, target) = first.split(' ').let { it[0] to it.getOrElse(1) { "/" } }
            val headers = HashMap<String, String>()
            while (true) {
                val l = readLine(input) ?: break
                if (l.isEmpty()) break
                headers[l.substringBefore(':').trim().lowercase()] = l.substringAfter(':').trim()
            }
            val length = headers["content-length"]?.toLongOrNull() ?: 0L
            val path = target.substringBefore('?')
            val query = form(target.substringAfter('?', ""))
            if (impostor) {
                input.skip(length)
                answer(s, 200, "<html><body>Router login</body></html>", "text/html")
                return
            }
            when {
                method == "GET" && path == "/hostname" -> {
                    log += "GET /hostname"
                    if (hostname == null) answer(s, 404, "<html>The requested URL '/hostname' was not found on this server.</html>", "text/html")
                    else answer(s, 200, hostname + "\n", "text/plain")
                }
                method == "GET" && path == "/list" -> {
                    val p = query["path"] ?: ""
                    log += "LIST $p"
                    if (sd == null) return answer(s, 404, """{"error":"Home not found!"}""")
                    val dir = local(p)
                    if (dir == null || !dir.isDirectory) return answer(s, 404, """{"error":"Path not found!"}""")
                    val items = dir.listFiles()!!.sortedBy { it.name }.joinToString(",") {
                        if (it.isDirectory) """{"name":${Json.quote(it.name)},"path":${Json.quote(p + it.name + "/")},"ctime":${it.lastModified() / 1000}}"""
                        else """{"name":${Json.quote(it.name)},"path":${Json.quote(p + it.name)},"size":${it.length()},"ctime":${it.lastModified() / 1000}}"""
                    }
                    answer(s, 200, "[$items]")
                }
                method == "GET" && path == "/download" -> {
                    val p = query["path"] ?: ""
                    log += "DOWNLOAD $p"
                    val f = local(p)
                    if (noDownload) return answer(s, 404, "<html>The requested URL '/download' was not found on this server.</html>", "text/html")
                    if (f == null || !f.isFile) return answer(s, 404, """{"error":"Path not found!"}""")
                    s.getOutputStream().apply {
                        write("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: ${f.length()}\r\n\r\n".toByteArray())
                        f.inputStream().use { it.copyTo(this) }
                        flush()
                    }
                }
                method == "POST" && path == "/upload" -> upload(s, input, headers, length)
                method == "POST" -> {
                    val body = ByteArray(length.toInt())
                    input.readFullyInto(body, 0, body.size)
                    val f = form(String(body))
                    when (path) {
                        "/create" -> {
                            val p = f["path"] ?: ""
                            log += "CREATE $p"
                            val d = local(p) ?: return answer(s, if (sd == null) 404 else 403, """{"error":"Invalid path $p"}""")
                            if (d.exists()) return answer(s, 409, """{"error":"exists"}""")
                            if (!d.parentFile.isDirectory) return answer(s, 404, """{"error":"Path not found!"}""")
                            d.mkdir()
                            answer(s, 200, "{}")
                        }
                        "/delete" -> {
                            val p = f["path"] ?: ""
                            log += "DELETE $p"
                            val d = local(p)
                            if (d == null || !d.exists()) return answer(s, 404, """{"error":"Path not found!"}""")
                            d.deleteRecursively()
                            answer(s, 200, "{}")
                        }
                        "/move" -> {
                            val a = local(f["oldPath"] ?: "")
                            val b = local(f["newPath"] ?: "")
                            log += "MOVE ${f["oldPath"]} ${f["newPath"]}"
                            if (a == null || b == null || !a.exists()) return answer(s, 404, """{"error":"Path not found!"}""")
                            a.renameTo(b)
                            answer(s, 200, "{}")
                        }
                        else -> answer(s, 404, "<html>The requested URL '$path' was not found on this server.</html>", "text/html")
                    }
                }
                else -> answer(s, 404, "<html>The requested URL '$path' was not found on this server.</html>", "text/html")
            }
        } catch (e: Exception) {
            // a broken connection ends the request, as on the real server
        }
    }

    private fun upload(s: Socket, input: InputStream, headers: Map<String, String>, length: Long) {
        if (failEarly) {
            log += "UPLOAD (refused at once)"
            answer(s, 500, """{"error":"New file fail. E: Read-only file system"}""")
            // as thttpd does: stop sending, keep reading for a moment, then close
            try {
                s.shutdownOutput()
                s.soTimeout = 500
                val sink = ByteArray(65536)
                val until = System.currentTimeMillis() + 1000
                while (System.currentTimeMillis() < until && input.read(sink) >= 0) { /* discard */ }
            } catch (e: Exception) {
                // closed by the client
            }
            return
        }
        val body = ByteArrayOutputStream()
        val buf = ByteArray(8192)
        var got = 0L
        while (got < length) {
            if (dropUploadAfter in 0..got) {
                dropUploadAfter = -1
                log += "UPLOAD (dropped after $got bytes)"
                s.setSoLinger(true, 0)   // reset, as when the player leaves the Wi-Fi
                s.close()
                return
            }
            if (stallUploadAfter in 0..got) {
                stallUploadAfter = -1
                log += "UPLOAD (stalled after $got bytes)"
                try { Thread.sleep(60000) } catch (e: InterruptedException) { /* test over */ }
                return
            }
            val n = input.read(buf, 0, minOf(buf.size.toLong(), length - got).toInt())
            if (n < 0) return
            body.write(buf, 0, n)
            got += n
        }
        if (neverConfirm) {
            neverConfirm = false
            log += "UPLOAD (never confirmed)"
            try { Thread.sleep(60000) } catch (e: InterruptedException) { /* test over */ }
            return
        }
        val boundary = "--" + (headers["content-type"] ?: "").substringAfter("boundary=")
        val raw = body.toByteArray()
        val text = String(raw, Charsets.ISO_8859_1)   // one char per byte: offsets are byte offsets
        var dir: String? = null
        var name: String? = null
        var data: ByteArray? = null
        var pos = text.indexOf(boundary)
        while (pos >= 0) {
            val headEnd = text.indexOf("\r\n\r\n", pos)
            if (headEnd < 0) break
            val next = text.indexOf("\r\n" + boundary, headEnd)
            if (next < 0) break
            val head = text.substring(pos, headEnd)
            val field = Regex("name=\"([^\"]*)\"").find(head)?.groupValues?.get(1)
            val content = raw.copyOfRange(headEnd + 4, next)
            when (field) {
                "path" -> dir = String(content, Charsets.UTF_8)
                "files[]" -> {
                    name = String(Regex("filename=\"([^\"]*)\"").find(head)!!.groupValues[1].toByteArray(Charsets.ISO_8859_1), Charsets.UTF_8)
                    data = content
                }
            }
            pos = next + 2
        }
        if (dir == null) return answer(s, 400, """{"error":"Path not found!"}""")
        if (name == null || data == null) return answer(s, 400, """{"error":"No file was uploaded"}""")
        log += "UPLOAD $dir$name ${data.size}"
        if (sd == null) return answer(s, 404, """{"error":"Home not found!"}""")
        val d = local(dir)
        if (d == null || !dir.endsWith("/")) return answer(s, 403, """{"error":"Invalid path $dir"}""")
        if (!d.isDirectory) return answer(s, 404, """{"error":"Path not found!"}""")
        onlyTypes?.let {
            if (name.substringAfterLast('.', "").uppercase() !in it) return answer(s, 403, """{"error":"Unsupported file type"}""")
        }
        if (data.size > freeBytes) return answer(s, 500, """{"error":"Write to file fail"}""")
        var out = data
        if (storeShort) { storeShort = false; out = data.copyOf(data.size - 1) }
        if (storeDamaged) { storeDamaged = false; out = data.clone().also { it[it.size / 2] = (it[it.size / 2] + 1).toByte() } }
        var target = File(d, name)
        if (target.exists() && renameOnClash) target = File(d, "$name (1)")
        target.writeBytes(out)
        freeBytes = if (freeBytes == Long.MAX_VALUE) freeBytes else freeBytes - data.size
        answer(s, 200, "{}")
    }
}
