package org.hibyos.cli

import java.io.File
import java.util.concurrent.TimeUnit

/** Runs a program and returns (exit code, combined output). Replaced in tests. */
fun interface ProcessRunner {
    fun run(cmd: List<String>, timeoutMs: Long): Pair<Int, String>

    companion object {
        val SYSTEM = ProcessRunner { cmd, timeoutMs ->
            val p = ProcessBuilder(cmd).redirectErrorStream(true).start()
            val out = StringBuilder()
            val reader = Thread { out.append(p.inputStream.bufferedReader().readText()) }.apply { isDaemon = true; start() }
            if (!p.waitFor(timeoutMs, TimeUnit.MILLISECONDS)) {
                p.destroyForcibly()
                throw CliError("timeout", "${cmd.first()} did not finish in time")
            }
            reader.join(1000)
            p.exitValue() to out.toString()
        }
    }
}

class AdbDevice(val serial: String, val state: String, val model: String?)

/**
 * The few adb commands byos-cli needs. adb itself is never bundled or downloaded: it comes
 * from --adb, the ADB environment variable or PATH (docs/BYOS_CLI.md section 1).
 */
class Adb(private val path: String, private val runner: ProcessRunner) {
    private fun adb(serial: String?, vararg args: String, timeoutMs: Long = 15000): Pair<Int, String> {
        val cmd = ArrayList<String>()
        cmd += path
        if (serial != null) { cmd += "-s"; cmd += serial }
        cmd += args
        return try {
            runner.run(cmd, timeoutMs)
        } catch (e: java.io.IOException) {
            throw CliError("no_device", "adb could not be started ($path): ${e.message}")
        }
    }

    fun devices(): List<AdbDevice> {
        val (code, out) = adb(null, "devices", "-l")
        if (code != 0) throw CliError("no_device", "adb devices failed: ${out.trim()}")
        return parseDevices(out)
    }

    /** `adb forward tcp:<local> tcp:7340`; returns the local port (adb picks one for 0). */
    fun forward(serial: String?, local: Int, remote: Int): Int {
        val (code, out) = adb(serial, "forward", "tcp:$local", "tcp:$remote")
        if (code != 0) throw CliError("no_device", "adb forward failed: ${out.trim()}")
        if (local != 0) return local
        return out.trim().lines().lastOrNull()?.trim()?.toIntOrNull() ?: throw CliError("no_device", "adb did not say which port it chose")
    }

    fun removeForward(serial: String?, local: Int) {
        try { adb(serial, "forward", "--remove", "tcp:$local", timeoutMs = 5000) } catch (e: CliError) {}
    }

    fun shell(serial: String?, command: String): Pair<Int, String> = adb(serial, "shell", command, timeoutMs = 15000)

    companion object {
        /** States `adb devices -l` can print after the serial. */
        private val STATE = Regex("^(.*?)\\s+(device|offline|unauthorized|recovery|sideload|bootloader|authorizing|connecting|host|rescue|no permissions)(\\s.*)?$")

        /** Parses `adb devices -l`. Serials may contain spaces (the HiBy R3 II 2025 reports
         *  "HiBy R3II_2025"), so the line is split at the state word, not at whitespace. */
        fun parseDevices(out: String): List<AdbDevice> =
            out.lines().drop(1).map { it.trim() }.filter { it.isNotEmpty() && !it.startsWith("*") }.mapNotNull { line ->
                val m = STATE.find(line) ?: return@mapNotNull null
                val model = m.groupValues[3].trim().split(Regex("\\s+")).firstOrNull { it.startsWith("model:") }?.removePrefix("model:")
                AdbDevice(m.groupValues[1].trim(), m.groupValues[2], model)
            }

        /** --adb, then ADB, then PATH; null when adb is not installed. */
        fun locate(option: String?, env: Map<String, String>): String? {
            option?.let { return it }
            env["ADB"]?.takeIf { it.isNotEmpty() }?.let { return it }
            val names = if (System.getProperty("os.name").lowercase().contains("win")) listOf("adb.exe", "adb.cmd", "adb.bat") else listOf("adb")
            for (dir in (env["PATH"] ?: env["Path"] ?: "").split(File.pathSeparatorChar)) {
                if (dir.isEmpty()) continue
                for (n in names) {
                    val f = File(dir, n)
                    if (f.isFile && f.canExecute()) return f.path
                }
            }
            return null
        }
    }
}
