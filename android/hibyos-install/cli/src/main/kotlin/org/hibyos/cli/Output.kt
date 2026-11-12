package org.hibyos.cli

import java.io.PrintStream

/** A failure that ends the run with an `error` line and an exit code (docs/BYOS_CLI.md 2.1). */
class CliError(
    val code: String, message: String,
    val btpCode: Int? = null, val btpName: String? = null, val path: String? = null,
    val extra: Map<String, Any?> = emptyMap(),
) : Exception(message) {
    val exit: Int get() = EXIT[code] ?: 10

    companion object {
        val EXIT = mapOf("usage" to 1, "no_device" to 2, "auth" to 3, "remote" to 4, "local_io" to 5, "verify" to 6,
            "cancelled" to 7, "rejected" to 8, "timeout" to 9, "internal" to 10)
    }
}

/** Minimal JSON writer for the output lines. */
object JsonOut {
    fun encode(v: Any?): String = StringBuilder().also { write(it, v) }.toString()

    private fun write(sb: StringBuilder, v: Any?) {
        when (v) {
            null -> sb.append("null")
            is String -> quote(sb, v)
            is Boolean -> sb.append(v)
            is Int, is Long, is Short, is Byte -> sb.append(v)
            is Double -> if (v.isFinite()) sb.append(if (v == Math.floor(v) && Math.abs(v) < 1e15) v.toLong().toString() else v.toString()) else sb.append("null")
            is Float -> write(sb, v.toDouble())
            is Map<*, *> -> {
                sb.append('{')
                var first = true
                for ((k, x) in v) {
                    if (!first) sb.append(',')
                    first = false
                    quote(sb, k.toString()); sb.append(':'); write(sb, x)
                }
                sb.append('}')
            }
            is Iterable<*> -> {
                sb.append('[')
                var first = true
                for (x in v) { if (!first) sb.append(','); first = false; write(sb, x) }
                sb.append(']')
            }
            is Array<*> -> write(sb, v.asList())
            else -> quote(sb, v.toString())
        }
    }

    private fun quote(sb: StringBuilder, s: String) {
        sb.append('"')
        for (c in s) {
            when {
                c == '"' -> sb.append("\\\"")
                c == '\\' -> sb.append("\\\\")
                c == '\n' -> sb.append("\\n")
                c == '\r' -> sb.append("\\r")
                c == '\t' -> sb.append("\\t")
                c.code < 0x20 || c == ' ' || c == ' ' -> sb.append(String.format("\\u%04x", c.code))
                else -> sb.append(c)
            }
        }
        sb.append('"')
    }
}

/**
 * The output channel: JSON lines on stdout (default) or plain text with --human.
 * Diagnostics go to stderr only with --verbose.
 */
class Out(private val stdout: PrintStream, private val stderr: PrintStream, var human: Boolean = false, var verbose: Boolean = false) {
    private val t0 = System.currentTimeMillis()
    private var lastProgress = 0L
    private var lastProgressKey: String? = null
    @Volatile var finished = false
        private set

    /** When the last line was printed, and the last progress line (repeated by the heartbeat). */
    private var lastLineAt = System.currentTimeMillis()
    private var lastProgressFields: Map<String, Any?>? = null

    /** True while a prompt waits for an answer: the only silent state (no heartbeat). */
    @Volatile var prompting = false

    companion object {
        /** Repeat the progress line after this much silence (the caller's watchdog allows 25 s; the spec asks for 5 s). */
        const val HEARTBEAT_MS = 4000L
    }

    /**
     * Starts the heartbeat (BYOS_CLI.md section 2): a daemon thread that repeats the last
     * progress line (same op and done) whenever nothing was printed for [intervalMs], so a
     * long device-side wait is never silent. It stops after the result or error line, and
     * stays quiet while a prompt waits.
     */
    fun startHeartbeat(intervalMs: Long) {
        if (intervalMs <= 0) return
        val tick = (intervalMs / 4).coerceIn(10, 250)
        Thread {
            while (true) {
                try { Thread.sleep(tick) } catch (e: InterruptedException) { return@Thread }
                synchronized(this) {
                    if (finished) return@Thread
                    val last = lastProgressFields
                    if (!prompting && last != null && System.currentTimeMillis() - lastLineAt >= intervalMs) event("progress", last)
                }
            }
        }.apply { isDaemon = true; name = "byos-cli-heartbeat"; start() }
    }

    @Synchronized
    fun event(name: String, fields: Map<String, Any?>) {
        if (finished) return
        lastLineAt = System.currentTimeMillis()
        when (name) {
            "progress" -> lastProgressFields = LinkedHashMap(fields)
            "prompt" -> lastProgressFields = null // after an answer, an old progress line would be stale
        }
        if (human) {
            stdout.println(humanLine(name, fields))
        } else {
            val m = LinkedHashMap<String, Any?>()
            m["v"] = 1; m["event"] = name; m["t"] = System.currentTimeMillis() - t0
            m.putAll(fields)
            stdout.print(JsonOut.encode(m))
            stdout.print('\n')
        }
        stdout.flush()
        if (name == "result" || name == "error") finished = true
    }

    /** At most 5 progress lines per second per operation, but always the last one. */
    @Synchronized
    fun progress(op: String, path: String?, done: Long, total: Long, rate: Long = 0, unit: String = "bytes") {
        val now = System.currentTimeMillis()
        val key = "$op|$path"
        val final = done >= total
        if (!final && key == lastProgressKey && now - lastProgress < 200) return
        lastProgress = now
        lastProgressKey = key
        event("progress", linkedMapOf("op" to op, "path" to path, "done" to done, "total" to total, "rate" to rate, "unit" to unit))
    }

    fun warn(code: String, message: String, extra: Map<String, Any?> = emptyMap()) =
        event("warn", linkedMapOf<String, Any?>("code" to code, "message" to message).apply { putAll(extra) })

    fun debug(msg: String) {
        if (verbose) { stderr.println("byos-cli: $msg"); stderr.flush() }
    }

    private fun humanLine(name: String, f: Map<String, Any?>): String = when (name) {
        "start" -> "byos-cli ${f["version"]}: ${f["command"]}"
        "device" -> "Device ${f["name"]} (${f["id"]}) over ${f["transport"]}" + (f["address"]?.let { " at $it" } ?: "") +
            (if (f["reachable"] == false) " - not reachable" else "")
        "entry" -> "%-5s %14s  %s".format(f["kind"], if (f["kind"] == "dir") "" else f["size"].toString(), f["path"])
        "progress" -> "${f["op"]}: ${f["path"] ?: ""} ${f["done"]}/${f["total"]} ${f["unit"]}"
        "prompt" -> "${f["message"]}:"
        "warn" -> "Warning: ${f["message"]}"
        "result" -> "Done. " + f.filterKeys { it != "ok" && it != "items" }.entries.joinToString(", ") { "${it.key}=${JsonOut.encode(it.value)}" }
        "error" -> "Error (${f["code"]}): ${f["message"]}"
        else -> "$name ${JsonOut.encode(f)}"
    }
}
