package org.hibyos.cli

import org.hibyos.install.core.Bundle
import org.hibyos.install.core.BundleException
import org.hibyos.install.core.BundleStore
import org.hibyos.install.core.ByosBuild
import org.hibyos.install.core.FirmwareCache
import org.hibyos.install.core.DownloadException
import org.hibyos.install.core.Downloader
import org.hibyos.install.core.FirmwareFetcher
import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import org.hibyos.install.core.PatchException
import org.hibyos.install.core.PatchListener
import org.hibyos.install.core.PatchOptions
import org.hibyos.install.core.Patcher
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpAuthException
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpDiscovery
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.BtpPaths
import org.hibyos.install.core.btp.BtpSession
import org.hibyos.install.core.btp.DataSink
import org.hibyos.install.core.btp.DbStatusR
import org.hibyos.install.core.btp.DfR
import org.hibyos.install.core.btp.Found
import org.hibyos.install.core.btp.Hello
import org.hibyos.install.core.btp.UpdateException
import org.hibyos.install.core.btp.UpdatePackage
import org.hibyos.install.core.btp.UpdateStageR
import org.hibyos.install.core.btp.UpdateStatusR
import org.hibyos.install.core.library.LibraryBuilder
import org.hibyos.install.core.library.LibraryException
import org.hibyos.install.core.library.LibraryListener
import org.hibyos.install.core.library.LibraryOptions
import org.hibyos.install.core.library.SqliteException
import java.io.Closeable
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.io.PrintStream
import java.io.RandomAccessFile
import java.net.InetAddress
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

const val VERSION = "0.1.0"

/** Parsed command line: global options are accepted before or after the command. */
class Args(val command: String, val pos: List<String>, val flags: Set<String>, val opts: Map<String, String>,
           val multi: Map<String, List<String>> = emptyMap()) {
    fun has(f: String) = f in flags
    fun opt(o: String) = opts[o]
    /** Every value of an option that may be repeated (--exclude, --select). */
    fun optAll(o: String): List<String> = multi[o] ?: emptyList()
    fun int(o: String, def: Int): Int = opts[o]?.let { it.toIntOrNull() ?: throw CliError("usage", "$o needs a number") } ?: def
}

/**
 * One BTP session plus how it was reached. [local] is true only for a session the client itself
 * opened to the loopback address (adb forward to the USB device) and that the device also
 * accepted as local: the only kind that runs without AUTH and may use NO_CONFIRM.
 */
class Conn(val session: BtpSession, val transport: String, val address: String, val serial: String?,
           var record: DeviceRecord?, val local: Boolean, private val cleanup: () -> Unit) : Closeable {
    override fun close() {
        try { session.close() } finally { cleanup() }
    }
}

/**
 * byos-cli (docs/BYOS_CLI.md). One run is one command and at most one device connection
 * (plus reconnects after a dropped upload or an update restart).
 */
class Cli(
    private val stdin: InputStream = System.`in`,
    stdout: PrintStream = System.out,
    stderr: PrintStream = System.err,
    private val env: Map<String, String> = System.getenv(),
    private val runner: ProcessRunner = ProcessRunner.SYSTEM,
    private val updateKeys: List<String> = UpdatePackage.RELEASE_KEYS,
    /** Delays before the 3 upload reconnects (2, 4, 8 s by default). */
    internal val retryDelaysMs: List<Long> = listOf(2000, 4000, 8000),
    private val pollMs: Long = 1000,
    private val reconnectWindowMs: Long = 60000,
    /** Silence after which the last progress line is repeated (BYOS_CLI.md section 2, heartbeat). */
    private val heartbeatMs: Long = Out.HEARTBEAT_MS,
    /** Files between two card checkpoints of a sync run (SYNC_LIBRARY_DESIGN.md 3.5). */
    internal val checkpointEvery: Int = 200,
    /** Tests only: called after each journalled sync operation (a test stops the run here as a kill would). */
    internal val syncHook: ((Int) -> Unit)? = null,
) {
    val out = Out(stdout, stderr)
    val cancelled = AtomicBoolean(false)
    private val answers = LinkedBlockingQueue<String>()
    private val eof = "\u0000EOF"
    internal lateinit var a: Args
    internal lateinit var config: Config
    private var timeoutMs = 15000
    private val lib by lazy { LibraryCommands(this) }
    private val sync by lazy { SyncCommands(this) }

    /** Starts hibysync over adb without --start (build-send waiting for the HiBy after its restart). */
    private var forceStart = false

    companion object {
        /** The ByOS files shipped in the jar (cli/build.gradle.kts copies the Android app's asset). */
        const val BUILTIN_BUNDLE = "/org/hibyos/cli/hibyos-bundle.zip"
        const val BUILD_MANIFEST = "build.json"
        /** Where build-send moves other firmware files found at the card root. */
        const val PARKED = "ByOS/firmware/parked"
        /** The OTA installer's own battery rule (docs/OTA.md): at least 30 %, or charging. */
        const val MIN_BATTERY = 30

        private val GLOBAL_VALUES = setOf("--device", "--adb", "--local-port", "--config", "--name", "--timeout", "--discovery")
        private val GLOBAL_FLAGS = setOf("--human", "--verbose", "--start")
        private val LINK_FLAGS = setOf("--remove-unselected", "--no-sidecars", "--disable", "--select-all")
        private val LINK_VALUES = setOf("--card-root", "--layout", "--player-only", "--select", "--select-file", "--exclude", "--label",
            "--covers", "--device-id")
        /** Options that may be given more than once. */
        private val MULTI = setOf("--select", "--exclude")
        private val COMMANDS: Map<String, Pair<Set<String>, Set<String>>> = mapOf(
            "devices" to (setOf("--usb-only", "--wifi-only") to setOf("--wait")),
            "connect" to (setOf("--pair", "--forget") to setOf("--code")),
            "ls" to (setOf("--all", "--recursive") to setOf("--max-depth")),
            "put" to (setOf("--overwrite", "--skip-identical", "--recursive", "--mkdir", "--preserve-mtime", "--no-mtime") to emptySet()),
            "get" to (setOf("--recursive", "--overwrite", "--no-verify") to emptySet()),
            "mkdir" to (setOf("-p") to emptySet()),
            "mv" to (setOf("--replace") to emptySet()),
            "rm" to (setOf("-r", "--yes", "--prune-empty") to emptySet()),
            "df" to (emptySet<String>() to emptySet()),
            "db-pull" to (emptySet<String>() to emptySet()),
            "db-build" to (emptySet<String>() to setOf("--base", "--music", "--out", "--sd-root", "--covers", "--lyrics", "--sort", "--report")),
            "db-push" to (setOf("--force") to setOf("--base-sha256")),
            "build-info" to (emptySet<String>() to setOf("--bundle", "--dir")),
            "bundle-update" to (setOf("--check") to emptySet()),
            "firmware-fetch" to (emptySet<String>() to setOf("--bundle", "--out", "--from")),
            "build" to (setOf("--force") to setOf("--bundle", "--firmware", "--out")),
            "build-send" to (setOf("--install", "--yes", "--no-wait") to setOf("--dir", "--wait-max")),
            "patch" to (emptySet<String>() to setOf("--firmware", "--bundle", "--out", "--work")),
            "update-send" to (setOf("--latest", "--allow-downgrade", "--install", "--no-confirm", "--no-wait", "--wait") to emptySet()),
            "update-install" to (setOf("--no-confirm", "--allow-downgrade", "--wait", "--no-wait") to emptySet()),
            "status" to (emptySet<String>() to emptySet()),
            // library and music sync (LibraryCommands.kt)
            "lib-index" to (setOf("--pull") to setOf("--db", "--out")),
            "lib-scan" to (setOf("--full") to setOf("--out")),
            "lib-art" to (emptySet<String>() to setOf("--size", "--max")),
            "lib-rescan" to (setOf("--full") to emptySet()),
            "playlist-ls" to (emptySet<String>() to emptySet()),
            "playlist-add" to (setOf("--create") to emptySet()),
            "playlist-rm" to (emptySet<String>() to emptySet()),
            "music-scan" to (setOf("--full") to setOf("--out")),
            // sync-plan --catalog is the branch's single-folder music sync; without it, the linked-folder sync (SyncCommands.kt)
            "sync-plan" to (setOf("--remove-unselected", "--delete-untracked", "--no-scan") to setOf("--catalog", "--select", "--dest", "--layout", "--covers", "--out", "--kind")),
            "sync-apply" to (setOf("--lyrics", "--yes", "--allow-mass-delete", "--take-over") to setOf("--covers", "--then", "--decisions")),
            "sync-scan" to (setOf("--full") to setOf("--kind", "--device-id")),
            "sync-resume" to (setOf("--take-over") to setOf("--then")),
            "sync-status" to (emptySet<String>() to setOf("--device-id")),
            "sync-unlock" to (setOf("--force") to emptySet()),
            "link-ls" to (emptySet<String>() to setOf("--device-id")),
            "link-add" to (LINK_FLAGS to (LINK_VALUES + "--kind")),
            "link-set" to ((LINK_FLAGS + setOf("--enable", "--keep-unselected", "--sidecars", "--clear-exclude", "--select-all")) to (LINK_VALUES + "--local")),
            "link-rm" to (emptySet<String>() to setOf("--device-id")),
            "cover-cache-reset" to (emptySet<String>() to emptySet()),
            "art-fix" to (setOf("--dry-run") to setOf("--root")),
        )

        val USAGE = """
            Usage: byos-cli <command> [options] [arguments]
            Commands: ${COMMANDS.keys.joinToString(", ")}
            Global options: --device SPEC, --adb PATH, --local-port N, --config DIR, --name NAME, --timeout MS, --human, --verbose, --start
            See docs/BYOS_CLI.md.
        """.trimIndent()

        fun parse(argv: List<String>): Args {
            var command: String? = null
            val pos = ArrayList<String>()
            val flags = HashSet<String>()
            val opts = HashMap<String, String>()
            val multi = HashMap<String, MutableList<String>>()
            var i = 0
            var onlyPos = false
            fun allowedFlags() = GLOBAL_FLAGS + (command?.let { COMMANDS[it]?.first } ?: emptySet())
            fun allowedValues() = GLOBAL_VALUES + (command?.let { COMMANDS[it]?.second } ?: emptySet())
            while (i < argv.size) {
                var t = argv[i++]
                if (!onlyPos && t == "--") { onlyPos = true; continue }
                if (!onlyPos && t.startsWith("-") && t.length > 1) {
                    var v: String? = null
                    if (t.startsWith("--") && t.contains('=')) { v = t.substringAfter('='); t = t.substringBefore('=') }
                    when (t) {
                        in allowedFlags() -> { if (v != null) throw CliError("usage", "$t takes no value"); flags += t }
                        in allowedValues() -> {
                            opts[t] = v ?: (if (i < argv.size) argv[i++] else throw CliError("usage", "$t needs a value"))
                            if (t in MULTI) multi.getOrPut(t) { ArrayList() } += opts[t]!!
                        }
                        else -> throw CliError("usage", "unknown option $t" + (command?.let { " for $it" } ?: ""))
                    }
                    continue
                }
                if (command == null) {
                    if (t !in COMMANDS) throw CliError("usage", "unknown command \"$t\"")
                    command = t
                } else pos += t
            }
            return Args(command ?: throw CliError("usage", "no command given"), pos, flags, opts, multi)
        }
    }

    // ------------------------------------------------------------------ run

    fun run(argv: Array<String>): Int {
        val cmdName = argv.firstOrNull { it in COMMANDS } ?: argv.firstOrNull() ?: ""
        out.human = "--human" in argv
        out.verbose = "--verbose" in argv
        out.event("start", linkedMapOf("command" to cmdName, "version" to VERSION, "protocol" to "BTP/1"))
        out.startHeartbeat(heartbeatMs)
        return try {
            a = parse(argv.toList())
            config = Config(a.opt("--config")?.let { File(it) } ?: Config.defaultDir(env))
            timeoutMs = a.int("--timeout", 15000)
            startInput()
            val result = when (a.command) {
                "devices" -> cmdDevices()
                "connect" -> cmdConnect()
                "ls" -> cmdLs()
                "put" -> cmdPut()
                "get" -> cmdGet()
                "mkdir" -> cmdMkdir()
                "mv" -> cmdMv()
                "rm" -> cmdRm()
                "df" -> withConn { c -> dfFields(c.session.df()) }
                "db-pull" -> cmdDbPull()
                "db-build" -> cmdDbBuild()
                "db-push" -> cmdDbPush()
                "build-info" -> cmdBuildInfo()
                "bundle-update" -> cmdBundleUpdate()
                "firmware-fetch" -> cmdFirmwareFetch()
                "build" -> cmdBuild()
                "build-send" -> cmdBuildSend()
                "patch" -> cmdPatch()
                "update-send" -> cmdUpdateSend()
                "update-install" -> withConn { c -> installAndWait(c, null) }
                "status" -> cmdStatus()
                "lib-index" -> lib.index()
                "lib-scan" -> lib.scan()
                "lib-art" -> lib.art()
                "lib-rescan" -> lib.rescan()
                "playlist-ls" -> lib.playlistLs()
                "playlist-add" -> lib.playlistAdd()
                "playlist-rm" -> lib.playlistRm()
                "music-scan" -> lib.musicScan()
                "sync-plan" -> if (a.opt("--catalog") != null) lib.syncPlan() else {
                    for (o in listOf("--select", "--dest", "--layout", "--covers")) if (a.opt(o) != null) throw CliError("usage", "$o goes with --catalog; links keep their own options (link-set)")
                    if (a.has("--remove-unselected")) throw CliError("usage", "--remove-unselected goes with --catalog; use link-set --remove-unselected")
                    sync.syncPlan()
                }
                "sync-apply" -> if (isPlanV2()) sync.syncApply() else lib.syncApply()
                "sync-scan" -> sync.syncScan()
                "sync-resume" -> sync.syncResume()
                "sync-status" -> sync.syncStatus()
                "sync-unlock" -> sync.syncUnlock()
                "link-ls" -> sync.linkLs()
                "link-add" -> sync.linkAdd()
                "link-set" -> sync.linkSet()
                "link-rm" -> sync.linkRm()
                "cover-cache-reset" -> lib.coverCacheReset()
                "art-fix" -> lib.artFix()
                else -> throw CliError("usage", "unknown command")
            }
            out.event("result", linkedMapOf<String, Any?>("ok" to true).apply { putAll(result) })
            0
        } catch (t: Throwable) {
            val e = mapError(t)
            val m = linkedMapOf<String, Any?>("ok" to false, "code" to e.code, "message" to e.message)
            if (e.btpCode != null) m["btp"] = linkedMapOf("code" to e.btpCode, "name" to e.btpName)
            if (e.path != null) m["path"] = e.path
            m.putAll(e.extra)
            if (e.code == "usage" && out.human) out.debug(USAGE)
            out.event("error", m)
            if (e.code == "internal") out.debug(t.stackTraceToString())
            e.exit
        }
    }

    /** A sync plan of the linked-folder sync (format 2) rather than the branch's catalogue plan. */
    private fun isPlanV2(): Boolean {
        val f = a.pos.firstOrNull()?.let { File(it) } ?: return false
        if (!f.isFile) return false
        return try { Json.obj(Json.parse(f.readText()))["format"] == 2L } catch (e: Exception) { false }
    }

    fun mapError(t: Throwable): CliError = when (t) {
        is CliError -> t
        is org.hibyos.install.core.sync.SyncException -> sync.syncError(t)
        is BtpAuthException -> when (t.reason) {
            BtpAuthException.Reason.CODE_MISMATCH -> CliError("auth", "code does not match")
            else -> CliError("auth", t.message ?: "authentication failed")
        }
        is BtpException -> when {
            t.code == Btp.E_CANCELLED -> CliError("cancelled", "cancelled")
            t.remote && t.code == Btp.E_PAIR_REJECTED -> CliError("rejected", "pairing was refused on the HiBy", t.code, t.codeName)
            t.remote && t.code == Btp.E_PAIR_CLOSED -> CliError("remote", "Open Pair new device on the HiBy", t.code, t.codeName)
            t.remote && t.code == Btp.E_AUTH_FAILED -> CliError("auth", "the HiBy refused this computer; pair again", t.code, t.codeName)
            t.remote && t.code == Btp.E_SD_UNAVAILABLE -> CliError("remote", "The HiBy's card is not available (no card, or connected as a USB drive)", t.code, t.codeName)
            t.remote -> CliError("remote", t.message?.takeIf { it.isNotEmpty() } ?: t.codeName, t.code, t.codeName)
            else -> CliError("remote", "protocol error: ${t.message}", t.code, t.codeName)
        }
        is BtpConnectionException -> if (t.message?.startsWith("timeout") == true) CliError("timeout", "the HiBy stopped answering")
            else CliError("no_device", t.message ?: "connection lost")
        is UpdateException -> CliError("verify", t.message ?: "invalid update")
        is LibraryException -> CliError("usage", t.message ?: "cannot build the library")
        is SqliteException -> CliError("verify", "the database cannot be read: ${t.message}")
        is IOException -> CliError("local_io", t.message ?: t.javaClass.simpleName)
        else -> CliError("internal", "${t.javaClass.simpleName}: ${t.message}")
    }

    // ------------------------------------------------------------------ stdin: answers and cancel

    private fun startInput() {
        Thread {
            try {
                val r = stdin.bufferedReader(Charsets.UTF_8)
                while (true) {
                    val line = r.readLine() ?: break
                    val t = line.trim()
                    if (t.startsWith("{")) {
                        val m = try { Json.obj(Json.parse(t)) } catch (e: JsonException) { null }
                        if (m?.get("cancel") == true) { cancelled.set(true); continue }
                    }
                    answers.put(line)
                }
            } catch (e: IOException) {
            } finally {
                answers.put(eof)
            }
        }.apply { isDaemon = true; name = "byos-cli-stdin"; start() }
    }

    internal fun checkCancel() { if (cancelled.get()) throw CliError("cancelled", "cancelled") }

    /** Prints a prompt and reads one answer: a JSON {"answer": ...} or plain text; empty or EOF is null. */
    internal fun ask(kind: String, message: String, extra: Map<String, Any?> = emptyMap()): Any? {
        out.event("prompt", linkedMapOf<String, Any?>("kind" to kind, "message" to message).apply { putAll(extra) })
        out.prompting = true // the only silent state: no heartbeat while the user answers
        try {
            while (true) {
                checkCancel()
                val line = answers.poll(200, TimeUnit.MILLISECONDS) ?: continue
                if (line == eof) { answers.put(eof); return null }
                val t = line.trim()
                if (t.isEmpty()) return null
                if (t.startsWith("{")) {
                    val m = try { Json.obj(Json.parse(t)) } catch (e: JsonException) { throw CliError("usage", "bad answer: $t") }
                    return m["answer"]
                }
                return t
            }
        } finally {
            out.prompting = false
        }
    }

    internal fun confirm(message: String, extra: Map<String, Any?>): Boolean = when (val v = ask("confirm", message, extra)) {
        is Boolean -> v
        is String -> v.lowercase() in setOf("y", "yes", "true", "s", "si", "sí", "o", "oui")
        else -> false
    }

    // ------------------------------------------------------------------ connection (section 4)

    private fun clientName(): String = BtpPaths.clipUtf8(a.opt("--name") ?: try {
        InetAddress.getLocalHost().hostName
    } catch (e: Exception) { "byos-cli" }, Btp.MAX_NAME)

    private fun hello() = Hello.client("byos-cli $VERSION", clientName())

    private fun adbPath(): String? = Adb.locate(a.opt("--adb"), env)

    private fun deviceFields(c: Conn): Map<String, Any?> {
        val d = c.session.device
        return linkedMapOf("id" to d.deviceId?.let(Config::hexOf), "name" to d.name, "transport" to c.transport,
            "address" to c.address, "serial" to c.serial, "paired" to (c.record != null), "pairing_open" to c.session.pairingOpen,
            "sd_state" to if (c.session.sdMounted) "mounted" else "unavailable", "byos_version" to d.byosVersion, "firmware" to d.firmware)
    }

    internal fun <T> withConn(allowUnauth: Boolean = false, quiet: Boolean = false, f: (Conn) -> T): T {
        val c = connect(allowUnauth, quiet)
        try {
            return f(c)
        } finally {
            c.close()
        }
    }

    /** Connects as --device says (or the default) and authenticates remote sessions. */
    fun connect(allowUnauth: Boolean = false, quiet: Boolean = false): Conn {
        checkCancel()
        val spec = a.opt("--device")
        val c = when {
            spec == null -> {
                val adb = adbPath()
                val usb = if (adb != null) try { Adb(adb, runner).devices().filter { it.state == "device" } } catch (e: CliError) { emptyList() } else emptyList()
                when {
                    usb.size == 1 -> usbConnect(usb[0].serial, allowUnauth)
                    else -> {
                        val rec = config.devices().maxByOrNull { it.lastSeen }
                            ?: throw CliError("no_device", if (usb.size > 1) "several HiBy players are connected over USB; use --device usb:SERIAL"
                                else "no HiBy found: connect it over USB (adb), or pair it over Wi-Fi with: byos-cli --device wifi connect --pair")
                        wifiConnect(rec, null, allowUnauth)
                    }
                }
            }
            spec == "usb" || spec.startsWith("usb:") -> usbConnect(spec.removePrefix("usb").removePrefix(":").ifEmpty { null }, allowUnauth)
            spec == "wifi" || spec.startsWith("wifi:") -> {
                val id = spec.removePrefix("wifi").removePrefix(":").ifEmpty { null }
                if (id != null && (id.length < 4 || !id.all { it.isDigit() || it.lowercaseChar() in 'a'..'f' })) throw CliError("usage", "wifi:DEVICE_ID needs at least 4 hex digits")
                wifiConnect(if (id != null) config.find(id) ?: throw CliError("auth", "no paired device $id; pair it with connect --pair") else null, id, allowUnauth)
            }
            else -> {
                val (host, port) = hostPort(spec)
                tcpConnect(host, port, if (isLoopback(host)) "local" else "wifi", null, allowUnauth, null, null)
            }
        }
        if (!quiet) out.event("device", deviceFields(c))
        return c
    }

    private fun hostPort(spec: String): Pair<String, Int> {
        fun port(p: String) = p.toIntOrNull()?.takeIf { it in 1..65535 } ?: throw CliError("usage", "bad port in $spec")
        if (spec.startsWith("[")) {
            val rest = spec.substringAfter("]", "")
            return spec.substringBefore("]").removePrefix("[") to (if (rest.startsWith(":")) port(rest.substring(1)) else Btp.PORT)
        }
        if (spec.count { it == ':' } == 1) return spec.substringBefore(':') to port(spec.substringAfter(':'))
        return spec to Btp.PORT
    }

    private fun isLoopback(host: String) = host == "127.0.0.1" || host == "localhost" || host == "::1"

    /**
     * True when --device names a network connection, so the session can never be local
     * (known before connecting; for the default device it is only known after).
     */
    private fun surelyRemote(): Boolean {
        val spec = a.opt("--device") ?: return false
        if (spec == "usb" || spec.startsWith("usb:")) return false
        if (spec == "wifi" || spec.startsWith("wifi:")) return true
        return !isLoopback(hostPort(spec).first)
    }

    /**
     * Opens a session. [transport] is how the client itself reached the device: "usb" (adb
     * forward) and "local" (an explicit loopback address) may run without AUTH when the device
     * agrees; "wifi" (discovery, a stored or an explicit network address) always authenticates
     * with the stored keys, and a device that claims HF_LOCAL there is refused. [expected] is the
     * paired device a Wi-Fi target was chosen for: another DEVICE_ID is refused before AUTH.
     */
    private fun tcpConnect(host: String, port: Int, transport: String, serial: String?, allowUnauth: Boolean,
                           cleanup: (() -> Unit)?, expected: DeviceRecord?): Conn {
        out.debug("connecting to $host:$port")
        val s = BtpSession.connect(host, port, hello(), timeoutMs)
        val conn: Conn
        try {
            val loopback = (transport == "usb" || transport == "local") && isLoopback(host)
            if (s.deviceSaysLocal && !loopback) {
                throw BtpException(Btp.E_PROTO, "the device at $host:$port claims a USB session over the network; refused")
            }
            val local = loopback && s.deviceSaysLocal
            val idHex = s.device.deviceId?.let(Config::hexOf)
            if (!local && !allowUnauth && expected != null && idHex != expected.id) {
                throw CliError("auth", "different device or the HiBy was reset; pair again")
            }
            val all = config.devices()
            val rec = idHex?.let { id -> all.firstOrNull { it.id == id } }
            conn = Conn(s, if (local) "usb" else "wifi", "$host:$port", serial, rec, local, cleanup ?: {})
            if (!local) {
                if (rec == null) {
                    if (!allowUnauth) throw CliError("auth", "this computer is not paired with ${s.device.name ?: "the HiBy"}; run: byos-cli --device $host connect --pair")
                } else {
                    s.auth(config.clientSecret(), rec.toPaired())
                }
            }
            if (rec != null) {
                rec.lastSeen = System.currentTimeMillis() / 1000
                if (!local) rec.lastAddress = "$host:$port"
                s.device.name?.let { rec.name = it }
                config.saveDevices(all)
            }
        } catch (t: Throwable) {
            try { s.close() } finally { cleanup?.invoke() }
            throw t
        }
        return conn
    }

    private fun usbConnect(serial: String?, allowUnauth: Boolean): Conn {
        val path = adbPath() ?: throw CliError("no_device", "adb was not found: install Android platform-tools or pass --adb PATH")
        val adb = Adb(path, runner)
        val devs = adb.devices()
        val chosen = if (serial != null) devs.firstOrNull { it.serial == serial } ?: throw CliError("no_device", "no USB device $serial")
        else {
            val ok = devs.filter { it.state == "device" }
            when {
                ok.size == 1 -> ok[0]
                ok.size > 1 -> throw CliError("usage", "several devices are connected over USB; use --device usb:SERIAL")
                devs.any { it.state == "unauthorized" } -> throw CliError("no_device", "the HiBy did not authorise this computer for USB debugging")
                else -> throw CliError("no_device", "no HiBy connected over USB")
            }
        }
        if (chosen.state != "device") throw CliError("no_device", "the USB device ${chosen.serial} is ${chosen.state}")
        val want = a.int("--local-port", Btp.PORT)
        val local = try { adb.forward(chosen.serial, want, Btp.PORT) } catch (e: CliError) {
            if (a.opt("--local-port") == null) adb.forward(chosen.serial, 0, Btp.PORT) else throw e
        }
        val cleanup = { adb.removeForward(chosen.serial, local) }
        fun attempt(): Conn = tcpConnect("127.0.0.1", local, "usb", chosen.serial, allowUnauth, cleanup, null)
        return try {
            attempt()
        } catch (e: BtpConnectionException) {
            if (!forceStart) out.warn("daemon_not_running", "ByOS Sync is not running on the HiBy")
            if (!a.has("--start") && !forceStart) {
                cleanup()
                throw CliError("no_device", "open Settings > ByOS Sync on the HiBy (or run again with --start)")
            }
            adb.shell(chosen.serial, "d=/usr/data/hibymod; for b in \$d/shell/hibysync \$d/bin/hibysync; do [ -x \$b ] && exec \$b serve --idle-exit 300 --detach; done; echo no-hibysync")
            val deadline = System.currentTimeMillis() + 5000
            while (true) {
                try { return attempt() } catch (x: BtpConnectionException) {
                    if (System.currentTimeMillis() > deadline) { cleanup(); throw CliError("no_device", "hibysync did not start on the HiBy") }
                    Thread.sleep(500)
                }
            }
            @Suppress("UNREACHABLE_CODE") throw e
        }
    }

    private fun discoveryTargets(): Pair<List<InetAddress>, Int> {
        val d = a.opt("--discovery") ?: return BtpDiscovery.broadcastAddresses() to Btp.PORT
        return listOf(InetAddress.getByName(d.substringBeforeLast(':'))) to (d.substringAfterLast(':').toIntOrNull() ?: Btp.PORT)
    }

    private fun discover(waitMs: Int): List<Found> {
        val (targets, port) = discoveryTargets()
        return try { BtpDiscovery.probe(waitMs, targets, port) } catch (e: IOException) { out.debug("discovery failed: ${e.message}"); emptyList() }
    }

    private fun wifiConnect(rec: DeviceRecord?, idPrefix: String?, allowUnauth: Boolean): Conn {
        val found = discover(1200)
        val paired = config.devices()
        fun stored(r: DeviceRecord) = r.lastAddress?.let { hostPort(it) }
        // the paired record the target is chosen for (null only for an unpaired device while pairing)
        var expected: DeviceRecord? = rec
        val target: Pair<String, Int>? = when {
            rec != null -> found.firstOrNull { it.deviceIdHex == rec.id }?.let { it.address to it.announce.tcpPort } ?: stored(rec)
            else -> {
                val cands = if (allowUnauth) found else found.filter { f -> paired.any { it.id == f.deviceIdHex } }
                val filtered = if (idPrefix != null) cands.filter { it.deviceIdHex.startsWith(idPrefix.lowercase()) } else cands
                when (filtered.size) {
                    1 -> {
                        expected = paired.firstOrNull { it.id == filtered[0].deviceIdHex }
                        filtered[0].address to filtered[0].announce.tcpPort
                    }
                    0 -> paired.singleOrNull()?.let { only -> expected = only; stored(only) }
                    else -> throw CliError("usage", "several HiBy players answered; use --device wifi:DEVICE_ID")
                }
            }
        }
        target ?: throw CliError("no_device", "no HiBy found on the Wi-Fi network: turn on Wi-Fi sync in Settings > ByOS Sync")
        return tcpConnect(target.first, target.second, "wifi", null, allowUnauth, null, expected)
    }

    // ------------------------------------------------------------------ helpers

    internal fun remotePath(p: String, allowRoot: Boolean = false): String {
        val n = BtpPaths.nfc(p.replace('\\', '/').trim('/'))
        BtpPaths.check(n, allowRoot)?.let { throw CliError("usage", "\"$p\" is not a valid path on the HiBy ($it)", path = p) }
        return n
    }

    internal fun sha256File(f: File, op: String = "hash", path: String? = null): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        val total = f.length()
        var done = 0L
        val t0 = System.currentTimeMillis()
        FileInputStream(f).use { s ->
            val b = ByteArray(1 shl 16)
            while (true) {
                checkCancel()
                val n = s.read(b)
                if (n < 0) break
                md.update(b, 0, n)
                done += n
                out.progress(op, path ?: f.path, done, total, rate(done, t0))
            }
        }
        if (total == 0L) out.progress(op, path ?: f.path, 0, 0)
        return md.digest()
    }

    internal fun rate(done: Long, t0: Long): Long { val dt = System.currentTimeMillis() - t0; return if (dt <= 0) 0 else done * 1000 / dt }

    internal fun hex(b: ByteArray) = Config.hexOf(b)

    internal fun dfFields(d: DfR): Map<String, Any?> = linkedMapOf(
        "sd_state" to (DfR.SD_STATES.getOrNull(d.sdState) ?: "unknown"), "sd_fs" to (DfR.SD_FS.getOrNull(d.sdFs) ?: "unknown"),
        "sd_total" to d.sdTotal, "sd_free" to d.sdFree, "data_total" to d.dataTotal, "data_free" to d.dataFree)

    private fun dbFields(d: DbStatusR): Map<String, Any?> = linkedMapOf("staged" to (d.staged == 1),
        "staged_sha256" to if (d.staged == 1) hex(d.stagedSha256) else null, "live_sha256" to hex(d.liveSha256),
        "live_size" to d.liveSize, "last_apply" to (DbStatusR.LAST_APPLY.getOrNull(d.lastApply) ?: "unknown"), "data_free" to d.dataFree)

    internal fun writeJson(f: File, m: Map<String, Any?>) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(JsonOut.encode(m) + "\n")
        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING)
    }

    // ------------------------------------------------------------------ devices / connect

    private fun cmdDevices(): Map<String, Any?> {
        if (a.has("--usb-only") && a.has("--wifi-only")) throw CliError("usage", "--usb-only and --wifi-only exclude each other")
        var count = 0
        val seen = HashSet<String>()
        val paired = config.devices()
        if (!a.has("--wifi-only")) {
            val path = adbPath()
            if (path == null) out.warn("adb_missing", "adb was not found; USB devices are not listed")
            else {
                val adb = Adb(path, runner)
                for (d in adb.devices()) {
                    val m = linkedMapOf<String, Any?>("transport" to "usb", "serial" to d.serial, "name" to (d.model ?: d.serial), "reachable" to false)
                    if (d.state == "device") {
                        try {
                            val port = adb.forward(d.serial, 0, Btp.PORT)
                            try {
                                BtpSession.connect("127.0.0.1", port, hello(), 3000).use { s ->
                                    val id = s.device.deviceId?.let(Config::hexOf)
                                    m["id"] = id; m["name"] = s.device.name; m["reachable"] = true; m["paired"] = paired.any { it.id == id }
                                    m["pairing_open"] = s.pairingOpen; m["sd_state"] = if (s.sdMounted) "mounted" else "unavailable"
                                    m["byos_version"] = s.device.byosVersion; m["firmware"] = s.device.firmware
                                    id?.let { seen += it }
                                }
                            } finally { adb.removeForward(d.serial, port) }
                        } catch (e: Exception) {
                            m["state"] = "sync_not_running"
                        }
                    } else m["state"] = d.state
                    out.event("device", m); count++
                }
            }
        }
        if (!a.has("--usb-only")) {
            for (f in discover(a.int("--wait", 1500))) {
                if (f.deviceIdHex in seen) continue
                seen += f.deviceIdHex
                out.event("device", linkedMapOf("id" to f.deviceIdHex, "name" to f.announce.name, "transport" to "wifi",
                    "address" to "${f.address}:${f.announce.tcpPort}", "reachable" to true, "paired" to paired.any { it.id == f.deviceIdHex },
                    "pairing_open" to f.announce.pairingOpen, "sd_state" to if (f.announce.sdMounted) "mounted" else "unavailable"))
                count++
            }
            for (p in paired) {
                if (p.id in seen) continue
                out.event("device", linkedMapOf("id" to p.id, "name" to p.name, "transport" to "wifi", "address" to p.lastAddress,
                    "reachable" to false, "paired" to true, "pairing_open" to false))
                count++
            }
        }
        return linkedMapOf("count" to count)
    }

    private fun cmdConnect(): Map<String, Any?> {
        if (a.has("--forget")) {
            val spec = a.opt("--device")
            val all = config.devices()
            val rec = when {
                spec?.startsWith("wifi:") == true -> config.find(spec.removePrefix("wifi:"))
                all.size == 1 -> all[0]
                else -> null
            } ?: throw CliError("usage", "which device? use --device wifi:DEVICE_ID")
            config.saveDevices(all.filter { it.id != rec.id })
            return linkedMapOf("id" to rec.id, "name" to rec.name, "paired" to false, "transport" to "wifi", "encrypted" to false)
        }
        val pair = a.has("--pair")
        val c = connect(allowUnauth = pair)
        c.use {
            if (pair) {
                val dev = c.session.pair(config.clientSecret(), clientName(), { name ->
                    val v = a.opt("--code") ?: ask("pair_code", "Type the 6-digit code shown on the HiBy", linkedMapOf("device" to name, "digits" to 6))
                    v?.toString()
                }, {
                    out.progress("pair_wait", null, 0, 0, unit = "none")
                })
                val all = config.devices().filter { it.id != hex(dev.deviceId) }.toMutableList()
                val now = System.currentTimeMillis() / 1000
                val rec = DeviceRecord(hex(dev.deviceId), dev.name, hex(dev.publicKey), now, if (c.transport == "wifi") c.address else null, now)
                all += rec
                config.saveDevices(all)
                c.record = rec
                return linkedMapOf("id" to rec.id, "name" to rec.name, "paired" to true, "transport" to c.transport, "encrypted" to c.session.encrypted)
            }
            return linkedMapOf("id" to c.session.device.deviceId?.let(Config::hexOf), "name" to c.session.device.name,
                "paired" to (c.record != null), "transport" to c.transport, "encrypted" to c.session.encrypted)
        }
    }

    // ------------------------------------------------------------------ files

    private fun entryFields(dir: String, e: org.hibyos.install.core.btp.ListEntry) = linkedMapOf<String, Any?>(
        "name" to e.name, "path" to BtpPaths.join(dir, e.name), "kind" to when (e.kind) { 0 -> "file"; 1 -> "dir"; else -> "other" },
        "size" to e.size, "mtime" to e.mtime)

    private fun cmdLs(): Map<String, Any?> {
        if (a.pos.size > 1) throw CliError("usage", "ls takes one path")
        val path = remotePath(a.pos.firstOrNull() ?: "", allowRoot = true)
        val maxDepth = a.int("--max-depth", 32)
        var count = 0
        withConn { c ->
            fun walk(dir: String, depth: Int) {
                checkCancel()
                val entries = c.session.list(dir, a.has("--all")).sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
                for (e in entries) {
                    out.event("entry", entryFields(dir, e)); count++
                    if (e.isDir && a.has("--recursive") && depth < maxDepth && !e.name.startsWith(".")) walk(BtpPaths.join(dir, e.name), depth + 1)
                }
            }
            walk(path, 1)
        }
        return linkedMapOf("count" to count, "path" to path)
    }

    private class PutItem(val local: File, val remote: String)

    private fun cmdPut(): Map<String, Any?> {
        if (a.pos.size < 2) throw CliError("usage", "put needs LOCAL... REMOTE")
        val mode = when {
            a.has("--overwrite") && a.has("--skip-identical") -> throw CliError("usage", "--overwrite and --skip-identical exclude each other")
            a.has("--overwrite") -> Btp.MODE_REPLACE
            a.has("--skip-identical") -> Btp.MODE_SKIP_IDENTICAL
            else -> Btp.MODE_CREATE
        }
        val locals = a.pos.dropLast(1).map { File(it) }
        val rawRemote = a.pos.last()
        val remoteBase = remotePath(rawRemote, allowRoot = true)
        val items = ArrayList<PutItem>()
        val invalid = ArrayList<Map<String, Any?>>()
        fun add(f: File, remote: String) {
            val r = BtpPaths.nfc(remote)
            val why = BtpPaths.check(r)
            if (why != null) invalid += linkedMapOf("local" to f.path, "path" to r, "reason" to why) else items += PutItem(f, r)
        }
        for (l in locals) {
            if (!l.exists()) throw CliError("local_io", "${l.path} does not exist", path = l.path)
            if (l.isDirectory) {
                if (!a.has("--recursive")) throw CliError("usage", "${l.path} is a folder: add --recursive")
                val base = l.absoluteFile.toPath().normalize()
                val name = base.fileName?.toString() ?: ""
                Files.walk(base).use { st ->
                    st.filter { Files.isRegularFile(it) }.sorted().forEach { p ->
                        val rel = base.relativize(p).toString().replace(File.separatorChar, '/')
                        add(p.toFile(), BtpPaths.join(remoteBase, name, rel))
                    }
                }
            } else {
                val exact = locals.size == 1 && !rawRemote.endsWith("/") && !rawRemote.endsWith("\\") && remoteBase.isNotEmpty()
                add(l, if (exact) remoteBase else BtpPaths.join(remoteBase, l.name))
            }
        }
        if (invalid.isNotEmpty()) throw CliError("usage", "some names cannot be stored on the HiBy's card", extra = mapOf("invalid" to invalid))
        // firmware files at the SD root: USB only (the recovery loader flashes any .upt it finds there)
        val upt = items.filter { isUptAtRoot(it.remote) }
        fun refuseUpt(): Nothing = throw CliError("usage", "firmware files (.upt) can be copied to the HiBy's card root over USB only",
            path = upt[0].remote, extra = mapOf("files" to upt.map { it.remote }))
        if (upt.isNotEmpty() && surelyRemote()) refuseUpt()
        // hash first
        val hashes = items.map { sha256File(it.local, "hash", it.remote) }
        val results = ArrayList<Map<String, Any?>>()
        var written = 0; var skipped = 0; var failed = 0; var bytes = 0L
        var firstFail: CliError? = null
        var conn: Conn? = null
        try {
            conn = connect()
            if (upt.isNotEmpty() && !conn.local) refuseUpt()
            val dirs = LinkedHashSet<String>()
            if (a.has("--mkdir") && !(items.size == 1 && items[0].remote == remoteBase)) { if (remoteBase.isNotEmpty()) dirs += remoteBase }
            if (a.has("--mkdir") && items.size == 1 && items[0].remote == remoteBase && remoteBase.contains('/')) dirs += remoteBase.substringBeforeLast('/')
            if (a.has("--recursive")) items.forEach { if (it.remote.contains('/')) dirs += it.remote.substringBeforeLast('/') }
            for (d in dirs) conn.session.mkdir(d, parents = true)
            for ((k, it) in items.withIndex()) {
                checkCancel()
                val sha = hashes[k]
                val mtime = if (a.has("--no-mtime")) 0L else it.local.lastModified() / 1000
                var attempt = 0
                while (true) {
                    try {
                        val c = conn ?: connect(quiet = true).also { conn = it }
                        if (!c.local && isUptAtRoot(it.remote)) refuseUpt()
                        val t0 = System.currentTimeMillis()
                        // shown (and repeated by the heartbeat) while the HiBy checks a part to resume
                        out.progress("put", it.remote, 0, it.local.length())
                        val res = c.session.put(it.remote, it.local.length(), sha, mode, mtime, Btp.NS_SD, { cancelled.get() },
                            { done, total -> out.progress("put", it.remote, done, total, rate(done, t0)) }) { off ->
                            FileInputStream(it.local).also { s -> s.channel.position(off) }
                        }
                        if (res.resumedFrom > 0) out.warn("resumed", "resumed ${it.remote} at byte ${res.resumedFrom}", mapOf("offset" to res.resumedFrom, "path" to it.remote))
                        if (res.result == 1) { skipped++; results += linkedMapOf("path" to it.remote, "result" to "skipped", "sha256" to hex(sha)) }
                        else { written++; bytes += res.size; results += linkedMapOf("path" to it.remote, "result" to "written", "sha256" to hex(sha)) }
                        break
                    } catch (e: BtpConnectionException) {
                        try { conn?.close() } catch (x: Exception) {}
                        conn = null
                        if (attempt >= retryDelaysMs.size) throw e
                        out.warn("reconnecting", "connection lost; retrying in ${retryDelaysMs[attempt] / 1000.0} s", mapOf("path" to it.remote))
                        Thread.sleep(retryDelaysMs[attempt])
                        attempt++
                    } catch (e: BtpException) {
                        if (!e.remote || e.code in setOf(Btp.E_SD_UNAVAILABLE, Btp.E_NO_SPACE, Btp.E_CANCELLED, Btp.E_BUSY)) throw e
                        failed++
                        val ce = mapError(e)
                        if (firstFail == null) firstFail = CliError(ce.code, ce.message ?: "", ce.btpCode, ce.btpName, it.remote)
                        if (e.code == Btp.E_EXISTS) out.warn("exists", "${it.remote} already exists on the HiBy; not replaced (use --overwrite)", mapOf("path" to it.remote))
                        else out.warn("failed", "${it.remote}: ${ce.message}", mapOf("path" to it.remote))
                        results += linkedMapOf("path" to it.remote, "result" to "failed", "sha256" to hex(sha), "error" to e.codeName)
                        break
                    }
                }
            }
        } finally {
            try { conn?.close() } catch (x: Exception) {}
        }
        val summary = linkedMapOf<String, Any?>("files" to items.size, "written" to written, "skipped" to skipped, "failed" to failed,
            "bytes" to bytes, "items" to results)
        if (failed > 0) {
            val f = firstFail!!
            throw CliError(f.code, "$failed of ${items.size} files failed (first: ${f.path}: ${f.message})", f.btpCode, f.btpName, f.path, summary)
        }
        return summary
    }

    private fun isUptAtRoot(remote: String) = !remote.contains('/') && remote.lowercase().endsWith(".upt")

    private fun cmdGet(): Map<String, Any?> {
        if (a.pos.size < 2) throw CliError("usage", "get needs REMOTE... LOCAL")
        val localArg = a.pos.last()
        val local = File(localArg)
        val remotes = a.pos.dropLast(1).map { remotePath(it) }
        val items = ArrayList<Map<String, Any?>>()
        var bytes = 0L
        withConn { c ->
            val jobs = ArrayList<Pair<String, File>>()
            for (r in remotes) {
                val st = c.session.stat(r)
                if (st.kind == 1) {
                    if (!a.has("--recursive")) throw CliError("usage", "$r is a folder: add --recursive", path = r)
                    val name = r.substringAfterLast('/')
                    val top = File(local, name)
                    val topCanon = top.canonicalPath
                    // names come from the device (checked as single components by ListR.decode);
                    // each local file must also stay inside the target folder
                    fun inside(into: File, entry: String): File {
                        val f = File(into, entry)
                        if (!f.canonicalPath.startsWith(topCanon + File.separator)) {
                            throw CliError("remote", "protocol error: the HiBy sent a name that leaves the target folder",
                                Btp.E_PROTO, Btp.errorName(Btp.E_PROTO), BtpPaths.join(r, entry))
                        }
                        return f
                    }
                    fun walk(dir: String, into: File, depth: Int) {
                        checkCancel()
                        if (depth > BtpPaths.MAX_DEPTH) {
                            throw CliError("remote", "protocol error: $dir is nested too deeply", Btp.E_PROTO, Btp.errorName(Btp.E_PROTO), dir)
                        }
                        for (e in c.session.list(dir)) {
                            if (e.isDir) walk(BtpPaths.join(dir, e.name), inside(into, e.name), depth + 1)
                            else if (e.kind == 0) jobs += BtpPaths.join(dir, e.name) to inside(into, e.name)
                        }
                    }
                    walk(r, top, r.count { it == '/' } + 1)
                } else {
                    val single = remotes.size == 1 && !local.isDirectory && !localArg.endsWith("/") && !localArg.endsWith(File.separator)
                    jobs += r to (if (single) local else File(local, r.substringAfterLast('/')))
                }
            }
            for ((r, dst) in jobs) {
                checkCancel()
                if (dst.exists() && !a.has("--overwrite")) throw CliError("local_io", "${dst.path} exists (use --overwrite)", path = dst.path)
                val sha = download(c, r, dst) { onStart, sink -> c.session.get(r, onStart.first, !a.has("--no-verify"), Btp.NS_SD, { cancelled.get() }, onStart.second, sink) }
                bytes += dst.length()
                items += linkedMapOf("path" to r, "local" to dst.path, "sha256" to sha)
            }
        }
        return linkedMapOf("files" to items.size, "bytes" to bytes, "items" to items)
    }

    /**
     * Downloads to `<dst>.byos-part` (resuming a part with the same remote size and mtime),
     * verifies the whole-file SHA-256 when the device sent one, then renames.
     */
    private fun download(c: Conn, remote: String, dst: File,
                         fetch: (Pair<Long, org.hibyos.install.core.btp.Progress>, (org.hibyos.install.core.btp.GetInfo) -> DataSink) -> org.hibyos.install.core.btp.GetOutcome): String? {
        dst.absoluteFile.parentFile?.mkdirs()
        val part = File(dst.path + ".byos-part")
        val meta = File(part.path + ".json")
        var offset = 0L
        val st = try { c.session.stat(remote) } catch (e: BtpException) { null }
        if (part.isFile && meta.isFile && st != null) {
            val m = try { Json.obj(Json.parse(meta.readText())) } catch (e: Exception) { null }
            if (m != null && Json.optLong(m, "size", -1) == st.size && Json.optLong(m, "mtime", -1) == st.mtime && part.length() < st.size) offset = part.length()
        }
        if (offset == 0L) part.delete()
        if (st != null) writeJson(meta, linkedMapOf("path" to remote, "size" to st.size, "mtime" to st.mtime))
        val md = MessageDigest.getInstance("SHA-256")
        if (offset > 0) {
            FileInputStream(part).use { s -> val b = ByteArray(1 shl 16); var left = offset; while (left > 0) { val n = s.read(b, 0, minOf(b.size.toLong(), left).toInt()); md.update(b, 0, n); left -= n } }
            out.warn("resumed", "resumed $remote at byte $offset", mapOf("offset" to offset, "path" to remote))
        }
        val t0 = System.currentTimeMillis()
        var raf: RandomAccessFile? = null
        var info: org.hibyos.install.core.btp.GetInfo? = null
        val res = try {
            fetch(offset to org.hibyos.install.core.btp.Progress { d, t -> out.progress("get", remote, d, t, rate(d - offset, t0)) }) { gi ->
                info = gi
                val f = RandomAccessFile(part, "rw")
                f.setLength(gi.offset)
                f.seek(gi.offset)
                raf = f
                DataSink { b, o, n -> f.write(b, o, n); md.update(b, o, n) }
            }
        } finally {
            raf?.close()
        }
        val got = md.digest()
        val zero = res.sha256.all { it.toInt() == 0 }
        if (!zero && !got.contentEquals(res.sha256)) {
            part.delete(); meta.delete()
            throw CliError("verify", "$remote did not arrive intact (SHA-256 differs)", path = remote)
        }
        Files.move(part.toPath(), dst.toPath(), StandardCopyOption.REPLACE_EXISTING)
        meta.delete()
        info?.let { if (it.mtime > 0) dst.setLastModified(it.mtime * 1000) }
        return if (zero) null else hex(got)
    }

    private fun cmdMkdir(): Map<String, Any?> {
        if (a.pos.isEmpty()) throw CliError("usage", "mkdir needs PATH...")
        val paths = a.pos.map { remotePath(it) }
        val created = ArrayList<String>()
        withConn { c -> for (p in paths) { checkCancel(); c.session.mkdir(p, a.has("-p")); created += p } }
        return linkedMapOf("created" to created)
    }

    private fun cmdMv(): Map<String, Any?> {
        if (a.pos.size != 2) throw CliError("usage", "mv needs SRC DST")
        val src = remotePath(a.pos[0]); val dst = remotePath(a.pos[1])
        withConn { c -> c.session.move(src, dst, a.has("--replace")) }
        return linkedMapOf("src" to src, "dst" to dst)
    }

    private fun cmdRm(): Map<String, Any?> {
        if (a.pos.isEmpty()) throw CliError("usage", "rm needs PATH...")
        val paths = a.pos.map { remotePath(it) }
        val deleted = ArrayList<String>()
        var quiet = false
        if (a.has("-r") && !a.has("--yes")) {
            // look first, then ask with no session open (the HiBy closes a session after 120 s
            // without requests), then reconnect to delete
            val dirs = withConn { c -> paths.filter { p -> checkCancel(); c.session.statOrNull(p)?.kind == 1 } }
            quiet = true
            for (p in dirs) {
                if (!confirm("Delete folder $p and everything in it?", linkedMapOf("path" to p))) {
                    throw CliError("cancelled", "not deleted: $p", path = p)
                }
            }
        }
        val pruned = ArrayList<String>()
        withConn(quiet = quiet) { c ->
            for (p in paths) {
                checkCancel()
                c.session.delete(p, a.has("-r"))
                deleted += p
            }
            if (a.has("--prune-empty")) pruned += pruneEmpty(c, deleted)
        }
        return linkedMapOf("deleted" to deleted, "pruned" to pruned)
    }

    /**
     * Removes the folders that [removed] left empty, from the deepest up, never a top-level
     * folder of the card (Music, Audiobooks...). A folder with any entry, hidden ones included,
     * stays.
     */
    internal fun pruneEmpty(c: Conn, removed: Collection<String>): List<String> {
        val out = ArrayList<String>()
        val dirs = removed.mapNotNull { p -> p.substringBeforeLast('/', "").ifEmpty { null } }.toSortedSet(compareByDescending<String> { it.count { c -> c == '/' } }.thenBy { it })
        val seen = HashSet<String>()
        while (dirs.isNotEmpty()) {
            val d = dirs.first()
            dirs.remove(d)
            if (!d.contains('/') || !seen.add(d)) continue
            val st = c.session.statOrNull(d) ?: continue
            if (st.kind != 1 || c.session.list(d, true).isNotEmpty()) continue
            c.session.delete(d, false)
            out += d
            dirs += d.substringBeforeLast('/')
        }
        return out
    }

    // ------------------------------------------------------------------ library database

    private fun cmdDbPull(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "db-pull needs OUT.db")
        val dst = File(a.pos[0])
        var size = 0L
        var sha: String? = null
        var devId: String? = null
        withConn { c ->
            devId = c.session.device.deviceId?.let(Config::hexOf)
            dst.absoluteFile.parentFile?.mkdirs()
            val part = File(dst.path + ".byos-part")
            part.delete()
            val md = MessageDigest.getInstance("SHA-256")
            val t0 = System.currentTimeMillis()
            var f: RandomAccessFile? = null
            out.progress("db_pull", null, 0, 0, unit = "none") // repeated while the HiBy makes the snapshot
            val res = try {
                c.session.dbSnapshot({ cancelled.get() }, { d, t -> out.progress("db_pull", null, d, t, rate(d, t0)) }) {
                    val raf = RandomAccessFile(part, "rw"); f = raf
                    DataSink { b, o, n -> raf.write(b, o, n); md.update(b, o, n) }
                }
            } finally { f?.close() }
            if (!md.digest().contentEquals(res.sha256)) { part.delete(); throw CliError("verify", "the snapshot did not arrive intact") }
            Files.move(part.toPath(), dst.toPath(), StandardCopyOption.REPLACE_EXISTING)
            size = dst.length(); sha = hex(res.sha256)
        }
        writeJson(File(dst.path + ".json"), linkedMapOf("device" to devId, "sha256" to sha, "size" to size, "time" to System.currentTimeMillis() / 1000))
        return linkedMapOf("path" to dst.path, "size" to size, "sha256" to sha)
    }

    internal fun fileSha(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(f).use { s -> val b = ByteArray(1 shl 16); while (true) { val n = s.read(b); if (n < 0) break; md.update(b, 0, n) } }
        return hex(md.digest())
    }

    private fun cmdDbBuild(): Map<String, Any?> {
        val base = File(a.opt("--base") ?: throw CliError("usage", "db-build needs --base SNAPSHOT.db"))
        val music = File(a.opt("--music") ?: throw CliError("usage", "db-build needs --music ROOT"))
        val outF = File(a.opt("--out") ?: throw CliError("usage", "db-build needs --out NEW.db"))
        if (!base.isFile) throw CliError("local_io", "${base.path} does not exist", path = base.path)
        val covers = when (a.opt("--covers") ?: "none") { "none" -> false; "folder" -> true; else -> throw CliError("usage", "--covers is none or folder") }
        val lyrics = when (a.opt("--lyrics") ?: "none") { "none" -> false; "sidecar" -> true; else -> throw CliError("usage", "--lyrics is none or sidecar") }
        if ((a.opt("--sort") ?: "stock") != "stock") throw CliError("usage", "--sort supports only stock")
        val warnings = ArrayList<Map<String, Any?>>()
        val rep = LibraryBuilder.build(base, outF, LibraryOptions(music, a.opt("--sd-root")?.let { File(it) } ?: music, covers, lyrics),
            object : LibraryListener {
                override fun progress(op: String, done: Int, total: Int, path: String?) {
                    if (cancelled.get()) throw CliError("cancelled", "cancelled")
                    out.progress(op, path, done.toLong(), total.toLong(), unit = "files")
                }
                override fun warn(code: String, message: String, path: String?) {
                    out.warn(code, message, mapOf("path" to path)); warnings += linkedMapOf("code" to code, "message" to message, "path" to path)
                }
            })
        val baseMeta = File(base.path + ".json")
        val baseSha = (if (baseMeta.isFile) try { Json.optStr(Json.obj(Json.parse(baseMeta.readText())), "sha256") } catch (e: JsonException) { null } else null)
            ?: fileSha(base)
        writeJson(File(outF.path + ".json"), linkedMapOf("base_sha256" to baseSha, "sha256" to rep.sha256, "size" to rep.size,
            "time" to System.currentTimeMillis() / 1000))
        val result = linkedMapOf<String, Any?>("out" to outF.path, "tracks" to rep.tracks, "albums" to rep.albums, "artists" to rep.artists,
            "genres" to rep.genres, "album_artists" to rep.albumArtists, "playlists_added" to rep.playlistsAdded, "skipped" to rep.skipped,
            "no_tags" to rep.noTags, "renumbered" to rep.renumbered, "sha256" to rep.sha256, "base_sha256" to baseSha)
        a.opt("--report")?.let { writeJson(File(it), LinkedHashMap(result).apply { put("kept_tables", rep.keptTables); put("rebuilt_tables", rep.rebuiltTables); put("warnings", warnings) }) }
        return result
    }

    private fun cmdDbPush(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "db-push needs NEW.db")
        val f = File(a.pos[0])
        if (!f.isFile) throw CliError("local_io", "${f.path} does not exist", path = f.path)
        val head = f.inputStream().use { it.readNBytes(16) }
        if (!String(head, Charsets.ISO_8859_1).startsWith("SQLite format 3")) throw CliError("verify", "${f.path} is not a SQLite database")
        val force = a.has("--force")
        val baseHex = a.opt("--base-sha256") ?: File(f.path + ".json").takeIf { it.isFile }?.let {
            try { Json.optStr(Json.obj(Json.parse(it.readText())), "base_sha256") } catch (e: JsonException) { null }
        }
        if (baseHex == null && !force) throw CliError("usage", "no base revision: run db-build (it writes NEW.db.json), or give --base-sha256 or --force")
        if (baseHex != null && !Regex("[0-9a-fA-F]{64}").matches(baseHex)) throw CliError("usage", "--base-sha256 needs 64 hex digits")
        val sha = sha256File(f, "hash", "staged.db")
        return withConn { c ->
            val t0 = System.currentTimeMillis()
            c.session.put("staged.db", f.length(), sha, Btp.MODE_REPLACE, 0, Btp.NS_DB, { cancelled.get() },
                { d, t -> out.progress("put", "staged.db", d, t, rate(d, t0)) }) { off -> FileInputStream(f).also { it.channel.position(off) } }
            out.progress("db_stage", "staged.db", 0, 0, unit = "none")
            val st = try {
                c.session.dbStage(baseHex?.let { DeviceRecord.hex(it.lowercase()) }, force)
            } catch (e: BtpException) {
                if (e.remote && e.code == Btp.E_DB_STALE) throw CliError("remote", "The library changed on the HiBy since db-pull; pull and build again", e.code, e.codeName)
                if (e.remote && e.code == Btp.E_NO_SPACE) throw CliError("remote", "Not enough space on the HiBy for the new library; turn on 'Library on SD card'", e.code, e.codeName)
                throw e
            }
            linkedMapOf("staged" to (st.staged == 1), "sha256" to hex(sha), "live_sha256" to hex(st.liveSha256),
                "restart_needed" to true, "message" to "Restart the HiBy to use the new library")
        }
    }

    // ------------------------------------------------------------------ firmware and patch

    // ------------------------------------------------------------------ ByOS build (docs/BYOS_CLI.md, "Build ByOS")

    /** Where the bundle in use came from: "file" (--bundle), "builtin" (shipped in this jar) or "updated". */
    private var bundleSource = "file"

    private fun bundleStore() = BundleStore(File(config.cache, "bundle"))
    private fun firmwareDir() = File(config.cache, "firmware")
    private fun buildDir(opt: String) = a.opt(opt)?.let { File(it) } ?: File(config.cache, "build")

    /** The ByOS files shipped inside byos-cli.jar (the same file the Android app ships), unpacked to the cache. */
    private fun builtinBundle(store: BundleStore): Bundle? {
        val bytes = Cli::class.java.getResourceAsStream(BUILTIN_BUNDLE)?.use { it.readBytes() } ?: return null
        val stamp = "${bytes.size}:${hex(MessageDigest.getInstance("SHA-256").digest(bytes))}"
        return try { store.builtin(stamp) { bytes.inputStream() } } catch (e: BundleException) {
            throw CliError("verify", e.message ?: "bad bundle")
        }
    }

    /** --bundle FILE, else the newest of the built-in and the updated ByOS files (the Android app's rule). */
    private fun openBundle(): Bundle = try {
        val given = a.opt("--bundle")
        if (given != null) {
            if (!File(given).isFile) throw CliError("local_io", "$given does not exist", path = given)
            bundleSource = "file"
            Bundle.open(File(given))
        } else {
            val store = bundleStore()
            val base = builtinBundle(store) ?: throw CliError("usage", "this byos-cli has no built-in ByOS files; give them with --bundle BUNDLE.zip")
            val cur = store.current(base)
            bundleSource = if (cur.builtIn) "builtin" else "updated"
            cur.bundle
        }
    } catch (e: BundleException) { throw CliError("verify", e.message ?: "bad bundle") }

    private fun downloadError(e: DownloadException, page: String?): CliError {
        val hint = page?.let { " Download it yourself from $it" } ?: ""
        return when (e.kind) {
            DownloadException.Kind.CANCELLED -> CliError("cancelled", "cancelled")
            DownloadException.Kind.MISMATCH, DownloadException.Kind.UNTRUSTED -> CliError("verify", (e.message ?: "wrong file") + "." + hint, extra = mapOf("page" to page))
            DownloadException.Kind.STORAGE -> CliError("local_io", e.message ?: "cannot save")
            else -> CliError("no_device", (e.message ?: "download failed") + "." + hint, extra = mapOf("page" to page))
        }
    }

    private fun patchError(e: PatchException): CliError = when (e.reason) {
        PatchException.Reason.CANCELLED -> CliError("cancelled", "cancelled")
        PatchException.Reason.STORAGE -> CliError("local_io", e.message ?: "storage error")
        PatchException.Reason.WRONG_FIRMWARE -> CliError("verify", (e.message ?: "wrong firmware") + (e.detail?.let { " (MD5 $it)" } ?: ""))
        else -> CliError("verify", e.message ?: "patch failed")
    }

    private fun firmwareFields(spec: org.hibyos.install.core.FirmwareSpec): Map<String, Any?> = linkedMapOf(
        "label" to spec.label, "version" to spec.version, "file" to spec.fileName, "size" to spec.size, "md5" to spec.md5,
        "sha256" to spec.sha256, "build" to spec.build, "page" to spec.page, "can_download" to spec.sources.isNotEmpty())

    private fun cmdBuildInfo(): Map<String, Any?> {
        val b = openBundle()
        val fw = FirmwareCache.ready(firmwareDir(), b.firmware)
        val dir = buildDir("--dir")
        val built = readBuild(dir)?.takeIf { it.hibyosVersion == b.hibyosVersion && ByosBuild.restore(b, dir) != null }
        return linkedMapOf("hibyos_version" to b.hibyosVersion, "generated_from" to b.generatedFrom, "bundle" to bundleSource,
            "bundle_path" to b.file.path, "firmware" to firmwareFields(b.firmware), "firmware_ready" to (fw != null),
            "firmware_path" to fw?.file?.path, "build" to built?.let {
                linkedMapOf("dir" to dir.path, "upt" to File(dir, it.upt).path, "size" to it.size, "sha256" to it.sha256,
                    "hibyos_version" to it.hibyosVersion, "build_id" to it.buildId, "files" to it.files.size, "sd_size" to it.files.sumOf { f -> f.second })
            })
    }

    private fun cmdBundleUpdate(): Map<String, Any?> {
        val store = bundleStore()
        val base = builtinBundle(store) ?: throw CliError("usage", "this byos-cli has no built-in ByOS files to update")
        val cur = store.current(base).bundle
        val remote = try { store.check(base, cur, { cancelled.get() }) } catch (e: DownloadException) {
            throw downloadError(e, null)
        } catch (e: BundleException) { throw CliError("usage", e.message ?: "no update index") }
        if (remote == null) return linkedMapOf("current" to cur.hibyosVersion, "latest" to cur.hibyosVersion, "available" to false, "updated" to false)
        if (a.has("--check")) return linkedMapOf("current" to cur.hibyosVersion, "latest" to remote.version, "available" to true,
            "updated" to false, "size" to remote.size)
        val t0 = System.currentTimeMillis()
        val b = try {
            store.install(remote, { cancelled.get() }, { d, t -> out.progress("download", "hibyos-bundle", d, t, rate(d, t0)) })
        } catch (e: DownloadException) { throw downloadError(e, null) }
        return linkedMapOf("current" to b.hibyosVersion, "latest" to remote.version, "available" to true, "updated" to true, "size" to remote.size)
    }

    private fun cmdFirmwareFetch(): Map<String, Any?> {
        val b = openBundle()
        val spec = b.firmware
        val dir = firmwareDir()
        val outF = a.opt("--out")?.let { File(it) }
        fun done(f: File, size: Long, md5: String, sha: String, cached: Boolean, source: String): Map<String, Any?> {
            if (outF != null && outF.absoluteFile != f.absoluteFile) {
                outF.absoluteFile.parentFile?.mkdirs()
                f.copyTo(outF, overwrite = true)
            }
            return linkedMapOf("path" to (outF ?: f).path, "version" to spec.version, "size" to size, "md5" to md5, "sha256" to sha,
                "cached" to cached, "source" to source)
        }
        a.opt("--from")?.let { from ->
            // a file the user picked: checked, then kept in the cache like a download
            val src = File(from)
            if (!src.isFile) throw CliError("local_io", "$from does not exist", path = from)
            var wrong: org.hibyos.install.core.FirmwareId? = null
            val c = try {
                FirmwareCache.import(src, dir, spec, { cancelled.get() }, { f -> out.progress("check", src.path, (f * 1000).toLong(), 1000, unit = "permille") }) { wrong = it }
            } catch (e: PatchException) { throw patchError(e) }
            if (c == null) {
                val w = wrong
                throw CliError("verify", "this file is not ${spec.label}" + (w?.md5?.takeIf { it.isNotEmpty() }?.let { " (MD5 $it)" } ?: ""),
                    path = from, extra = linkedMapOf("page" to spec.page, "size" to w?.size, "md5" to w?.md5?.ifEmpty { null }))
            }
            return done(c.file, c.size, spec.md5, c.sha256, false, "file")
        }
        FirmwareCache.ready(dir, spec)?.let { return done(it.file, it.size, spec.md5, it.sha256, true, "cache") }
        val dest = FirmwareCache.file(dir, spec)
        dir.mkdirs()
        FirmwareCache.unmark(dir)
        val t0 = System.currentTimeMillis()
        val id = try {
            FirmwareFetcher.fetch(spec, dest, { cancelled.get() }, { d, t -> out.progress("download", dest.path, d, t, rate(d, t0)) },
                { f -> out.progress("check", dest.path, (f * 1000).toLong(), 1000, unit = "permille") })
        } catch (e: DownloadException) { throw downloadError(e, spec.page) }
        FirmwareCache.mark(dir, id)
        return done(dest, id.size, id.md5, id.sha256, false, "download")
    }

    /** build.json: what `build` made, in the order `build-send` copies it. */
    private class BuildManifest(val hibyosVersion: String, val upt: String, val size: Long, val md5: String, val sha256: String,
                                val buildId: String, val files: List<Triple<String, Long, String>>)

    private fun readBuild(dir: File): BuildManifest? {
        val f = File(dir, BUILD_MANIFEST)
        if (!f.isFile) return null
        return try {
            val m = Json.obj(Json.parse(f.readText()))
            if (Json.long(m, "format") != 1L) return null
            BuildManifest(Json.str(m, "hibyos_version"), Json.str(m, "upt"), Json.long(m, "size"), Json.str(m, "md5"), Json.str(m, "sha256"),
                Json.str(m, "build_id"), Json.arr(m["files"]).map {
                    val e = Json.obj(it)
                    Triple(remotePath(Json.str(e, "path")), Json.long(e, "size"), Json.str(e, "sha256"))
                })
        } catch (e: JsonException) { null }
    }

    private fun buildResult(dir: File, m: BuildManifest, cached: Boolean): Map<String, Any?> = linkedMapOf(
        "out" to dir.path, "upt" to File(dir, m.upt).path, "size" to m.size, "md5" to m.md5, "sha256" to m.sha256,
        "hibyos_version" to m.hibyosVersion, "build_id" to m.buildId, "files" to m.files.size, "sd_size" to m.files.sumOf { it.second },
        "cached" to cached)

    private fun cmdBuild(): Map<String, Any?> {
        val b = openBundle()
        val dir = buildDir("--out")
        val fw = a.opt("--firmware")?.let { File(it) } ?: FirmwareCache.ready(firmwareDir(), b.firmware)?.file
            ?: throw CliError("usage", "there is no official firmware yet: run firmware-fetch first, or give --firmware FILE.upt")
        if (!fw.isFile) throw CliError("local_io", "${fw.path} does not exist", path = fw.path)
        // a finished build of the same ByOS files is reused (the patch takes a minute)
        if (!a.has("--force") && a.opt("--firmware") == null) {
            val m = readBuild(dir)
            val rec = ByosBuild.restore(b, dir)
            if (m != null && rec != null && m.hibyosVersion == b.hibyosVersion && m.sha256 == rec.sha256 &&
                m.files.all { (p, size, _) -> File(dir, p).length() == size }) return buildResult(dir, m, true)
        }
        File(dir, BUILD_MANIFEST).delete()
        val work = File(dir, ".work")
        val upt = File(dir, b.outName)
        val (r, _) = try {
            ByosBuild.patch(fw, b, dir, work, PatchOptions(cancelled = { cancelled.get() }), PatchListener { stage, fraction ->
                out.progress("patch", upt.path, (stage.overall(fraction) * 1000).toLong(), 1000, unit = "permille",
                    extra = mapOf("stage" to stage.name.lowercase()))
            })
        } catch (e: PatchException) { throw patchError(e) } finally { work.deleteRecursively() }
        val files = try {
            ByosBuild.exportSd(b, dir, { cancelled.get() }) { d, t -> out.progress("export", File(dir, "ByOS").path, d, t) }
        } catch (e: PatchException) { throw patchError(e) } catch (e: BundleException) { throw CliError("verify", e.message ?: "bad bundle") }
        val m = BuildManifest(r.hibyosVersion, b.outName, r.size, r.md5, r.sha256, r.buildId, files.map { Triple(it.path, it.size, it.sha256) })
        writeJson(File(dir, BUILD_MANIFEST), linkedMapOf("format" to 1, "hibyos_version" to m.hibyosVersion, "generated_from" to b.generatedFrom,
            "firmware_version" to b.firmware.version, "upt" to m.upt, "size" to m.size, "md5" to m.md5, "sha256" to m.sha256,
            "build_id" to m.buildId, "files" to m.files.map { (p, s, h) -> linkedMapOf("path" to p, "size" to s, "sha256" to h) }))
        return buildResult(dir, m, false)
    }

    /**
     * Copies a build to the HiBy over USB (the ByOS folder, then the firmware at the card root)
     * and, with --install, starts the HiBy's own reinstall through HiBy's recovery (the command
     * the HiBy's Software Update page runs) and waits until the HiBy is back with the new ByOS.
     */
    private fun cmdBuildSend(): Map<String, Any?> {
        val dir = buildDir("--dir")
        val m = readBuild(dir) ?: throw CliError("usage", "there is no build in ${dir.path}: run build first")
        if (surelyRemote()) throw CliError("usage", "installing a ByOS build needs the USB cable (firmware files never travel over Wi-Fi)")
        // every file is checked against the build before anything is sent
        val items = m.files.map { (p, size, sha) -> Triple(p, File(dir, p), size to sha) } +
            Triple(m.upt, File(dir, m.upt), m.size to m.sha256)
        for ((p, f, want) in items) {
            if (!f.isFile || f.length() != want.first || hex(sha256File(f, "hash", p)) != want.second) {
                throw CliError("verify", "the build in ${dir.path} changed after it was made ($p); build it again", path = p)
            }
        }
        val res = linkedMapOf<String, Any?>("version" to m.hibyosVersion, "upt" to m.upt)
        val parked = ArrayList<String>()
        var written = 0; var skipped = 0; var bytes = 0L
        var serial: String? = null
        var before: String? = null
        withConn { c ->
            if (!c.local) throw CliError("usage", "installing a ByOS build needs the USB cable (firmware files never travel over Wi-Fi)")
            if (!c.session.sdMounted) throw CliError("remote", "The HiBy's card is not available (no card, or connected as a USB drive)",
                Btp.E_SD_UNAVAILABLE, Btp.errorName(Btp.E_SD_UNAVAILABLE))
            serial = c.serial
            before = c.session.device.byosVersion
            // the recovery flashes the first .upt it finds at the root (update.upt first): only ours may be there
            for (e in c.session.list("")) {
                if (e.kind != 0 || !e.name.lowercase().endsWith(".upt") || e.name == m.upt) continue
                c.session.mkdir(PARKED, parents = true)
                c.session.move(e.name, BtpPaths.join(PARKED, e.name), replace = true)
                parked += e.name
                out.warn("parked", "moved ${e.name} from the card root to $PARKED so that the recovery installs ByOS", mapOf("path" to e.name))
            }
            for (d in m.files.map { it.first.substringBeforeLast('/', "") }.filter { it.isNotEmpty() }.distinct()) c.session.mkdir(d, parents = true)
            for ((p, f, want) in items) {
                checkCancel()
                val t0 = System.currentTimeMillis()
                out.progress("put", p, 0, f.length())
                val r = c.session.put(p, f.length(), DeviceRecord.hex(want.second), if (p == m.upt) Btp.MODE_REPLACE else Btp.MODE_SKIP_IDENTICAL,
                    f.lastModified() / 1000, Btp.NS_SD, { cancelled.get() }, { d, t -> out.progress("put", p, d, t, rate(d, t0)) }) { off ->
                    FileInputStream(f).also { s -> s.channel.position(off) }
                }
                if (r.result == 1) skipped++ else { written++; bytes += r.size }
            }
            if (a.has("--install")) {
                val ok = a.has("--yes") || confirm("Install ByOS ${m.hibyosVersion} on the HiBy now? It restarts into HiBy's recovery, installs the firmware and starts again. Keep the cable connected.",
                    linkedMapOf("version" to m.hibyosVersion))
                if (!ok) throw CliError("cancelled", "not installed; the files stay on the card")
                val st = c.session.status()
                if (st.battery != 255 && st.battery < MIN_BATTERY && st.charging != 1) {
                    throw CliError("remote", "the HiBy's battery is at ${st.battery} %: charge it to $MIN_BATTERY % or keep it charging",
                        Btp.E_BATTERY, Btp.errorName(Btp.E_BATTERY), extra = mapOf("battery" to st.battery))
                }
            }
        }
        res.putAll(linkedMapOf("files" to items.size, "written" to written, "skipped" to skipped, "bytes" to bytes, "parked" to parked))
        if (!a.has("--install")) return res.apply { put("state", "sent") }

        // the HiBy's own reinstall: checks its recovery and the .upt, switches to the recovery and restarts
        val adb = Adb(adbPath() ?: throw CliError("no_device", "adb was not found: install Android platform-tools or pass --adb PATH"), runner)
        out.progress("reboot", m.hibyosVersion, 0, 0, unit = "none")
        val (_, text) = adb.shell(serial, "/usr/bin/hibymod reinstall --yes")
        val refusal = text.lines().firstOrNull { it.startsWith("abortado") || it.startsWith("uso:") || it.contains("not found") }
        if (refusal != null) throw CliError("remote", "the HiBy did not start the reinstall: ${refusal.trim()}", extra = mapOf("output" to text.take(2000)))
        if (a.has("--no-wait")) return res.apply { put("state", "rebooting") }
        // wait for the HiBy to come back with the new ByOS: the recovery, then (when the payload
        // changed) one more restart while ByOS installs itself
        val waitMs = a.int("--wait-max", 900) * 1000L
        val deadline = System.currentTimeMillis() + waitMs
        var gone = false
        forceStart = true
        while (System.currentTimeMillis() < deadline) {
            checkCancel()
            out.progress(if (gone) "reconnect" else "reboot", m.hibyosVersion, 0, 0, unit = "none")
            try {
                connect(quiet = true).use { c ->
                    val v = c.session.device.byosVersion
                    // the same version as before only counts once the HiBy was seen restarting
                    if (v == m.hibyosVersion && (gone || before != m.hibyosVersion)) {
                        return res.apply { put("state", "installed"); put("installed", v) }
                    }
                }
            } catch (e: Exception) {
                if (e is CliError && e.code == "cancelled") throw e
                gone = true // restarting: no adb device, no forward, or hibysync not up yet
            }
            Thread.sleep(pollMs * 3)
        }
        throw CliError("timeout", "the HiBy did not come back with ByOS ${m.hibyosVersion} within ${waitMs / 1000} s; check its screen",
            extra = mapOf("state" to "rebooting"))
    }

    private fun cmdPatch(): Map<String, Any?> {
        val fw = File(a.opt("--firmware") ?: throw CliError("usage", "patch needs --firmware FILE.upt"))
        val outF = File(a.opt("--out") ?: throw CliError("usage", "patch needs --out BYOS.upt"))
        if (!fw.isFile) throw CliError("local_io", "${fw.path} does not exist", path = fw.path)
        val work = a.opt("--work")?.let { File(it) } ?: Files.createTempDirectory("byos-patch").toFile()
        openBundle().use { b ->
            val r = try {
                Patcher.patch(fw, b, outF, work, PatchOptions(cancelled = { cancelled.get() }), PatchListener { stage, fraction ->
                    out.progress(stage.name.lowercase(), outF.path, (fraction * 1000).toLong(), 1000, unit = "permille")
                })
            } catch (e: PatchException) {
                throw when (e.reason) {
                    PatchException.Reason.CANCELLED -> CliError("cancelled", "cancelled")
                    PatchException.Reason.STORAGE -> CliError("local_io", e.message ?: "storage error")
                    PatchException.Reason.WRONG_FIRMWARE -> CliError("verify", (e.message ?: "wrong firmware") + (e.detail?.let { " (MD5 $it)" } ?: ""))
                    else -> CliError("verify", e.message ?: "patch failed")
                }
            } finally {
                if (a.opt("--work") == null) work.deleteRecursively()
            }
            return linkedMapOf("out" to r.output.path, "size" to r.size, "md5" to r.md5, "sha256" to r.sha256, "hibyos_version" to r.hibyosVersion)
        }
    }

    // ------------------------------------------------------------------ updates

    private fun cmdUpdateSend(): Map<String, Any?> {
        val jsonF: File
        val tarF: File
        if (a.has("--latest")) {
            if (a.pos.isNotEmpty()) throw CliError("usage", "--latest takes no files")
            config.cache.mkdirs()
            val js = try { Downloader.text(UpdatePackage.LATEST_URL, UpdatePackage.JSON_MAX, { cancelled.get() }) } catch (e: DownloadException) {
                throw CliError("no_device", "cannot download the latest release: ${e.message}")
            }
            val pkg = UpdatePackage.parse(js, updateKeys)
            jsonF = File(config.cache, "hibyos-update.json").also { it.writeBytes(js) }
            tarF = File(config.cache, pkg.payloadName)
            if (!(tarF.isFile && tarF.length() == pkg.payloadSize)) {
                val t0 = System.currentTimeMillis()
                try {
                    Downloader.fetch(pkg.payloadUrl, tarF, pkg.payloadSize, UpdatePackage.PAYLOAD_MAX, { cancelled.get() }, { d, t -> out.progress("download", tarF.path, d, t, rate(d, t0)) })
                } catch (e: DownloadException) { throw CliError("no_device", "cannot download the payload: ${e.message}") }
            }
        } else {
            if (a.pos.size != 2) throw CliError("usage", "update-send needs JSON TAR (or --latest)")
            jsonF = File(a.pos[0]); tarF = File(a.pos[1])
            if (!jsonF.isFile) throw CliError("local_io", "${jsonF.path} does not exist")
        }
        // local check first: nothing is sent unless signature, fields and payload are right
        val pkg = UpdatePackage.parse(jsonF.readBytes(), updateKeys)
        pkg.checkPayload(tarF)
        val tarSha = sha256File(tarF, "hash", pkg.payloadName)
        return withConn { c ->
            for ((name, f, sha) in listOf(Triple("hibyos-update.json", jsonF, BtpCrypto.sha256(pkg.json)), Triple(pkg.payloadName, tarF, tarSha))) {
                var attempt = 0
                while (true) {
                    try {
                        val t0 = System.currentTimeMillis()
                        c.session.put(name, f.length(), sha, Btp.MODE_REPLACE, 0, Btp.NS_UPDATE, { cancelled.get() },
                            { d, t -> out.progress("put", name, d, t, rate(d, t0)) }) { off -> FileInputStream(f).also { it.channel.position(off) } }
                        break
                    } catch (e: BtpConnectionException) {
                        throw e // one connection per run for staging; the next run resumes the part
                    }
                }
            }
            out.progress("verify", pkg.payloadName, 0, 0, unit = "none") // the HiBy checks the signature and hashes the payload
            val st: UpdateStageR = c.session.updateStage(a.has("--allow-downgrade"))
            val res = linkedMapOf<String, Any?>("version" to st.version, "restart" to (UpdateStageR.RESTART.getOrNull(st.restart) ?: "reboot"),
                "needs_firmware" to (st.needsFirmware != 0), "size" to st.size)
            if (a.has("--install")) res.putAll(installAndWait(c, st))
            res
        }
    }

    private fun installAndWait(conn: Conn, staged: UpdateStageR?): Map<String, Any?> {
        if (a.has("--wait") && a.has("--no-wait")) throw CliError("usage", "--wait and --no-wait exclude each other")
        val noConfirm = a.has("--no-confirm")
        // NO_CONFIRM only on a session the client itself opened over USB, never on the device's word
        if (noConfirm && !conn.local) out.warn("confirm_on_device", "--no-confirm works over USB only; confirm the update on the HiBy")
        out.progress("install", staged?.version, 0, 1000, unit = "permille")
        try {
            conn.session.updateInstall(noConfirm && conn.local, a.has("--allow-downgrade"))
        } catch (e: BtpException) {
            if (e.remote && e.code == Btp.E_NOT_STAGED) throw CliError("remote", "no update is staged on the HiBy: run update-send first", e.code, e.codeName)
            if (e.remote && e.code == Btp.E_UNSUPPORTED) throw CliError("remote", "ByOS Sync is running without the ByOS interface; install the staged update from Settings > Software Update", e.code, e.codeName)
            throw e
        }
        val restartName = staged?.let { UpdateStageR.RESTART.getOrNull(it.restart) }
        if (a.has("--no-wait")) return linkedMapOf("version" to staged?.version, "state" to "started", "restart" to restartName)
        var version = staged?.version ?: ""
        var lastState = -1
        var warned = false
        while (true) {
            checkCancel()
            val st: UpdateStatusR = try {
                conn.session.updateStatus()
            } catch (e: BtpConnectionException) {
                if (lastState >= 3) break // the shell restarts: expected
                throw e
            }
            if (st.version.isNotEmpty()) version = st.version
            lastState = st.state
            when (st.state) {
                2 -> { if (!warned) { out.warn("confirm_on_device", "confirm the update on the HiBy"); warned = true }; out.progress("install", version, 0, 1000, unit = "permille") }
                3 -> out.progress("install", version, st.permille.toLong(), 1000, unit = "permille")
                4 -> out.progress("install", version, 1000, 1000, unit = "permille")
                5 -> throw CliError("remote", "the update failed on the HiBy", st.error, Btp.errorName(st.error))
                6 -> throw CliError("rejected", "the update was not confirmed on the HiBy")
                0 -> if (lastState >= 3) break
            }
            Thread.sleep(pollMs)
        }
        // reconnect and check the new version
        conn.close()
        val deadline = System.currentTimeMillis() + reconnectWindowMs
        while (System.currentTimeMillis() < deadline) {
            checkCancel()
            out.progress("reconnect", version, 0, 0, unit = "none")
            try {
                connect(quiet = true).use { c ->
                    if (c.session.device.byosVersion == version) {
                        return linkedMapOf("version" to version, "state" to "installed", "restart" to (restartName ?: "shell"))
                    }
                }
            } catch (e: Exception) {
                if (e is CliError && e.code == "cancelled") throw e
            }
            Thread.sleep(minOf(2000L, pollMs * 2))
        }
        return linkedMapOf("version" to version, "state" to "restart_pending", "restart" to (restartName ?: "reboot"))
    }

    private fun cmdStatus(): Map<String, Any?> = withConn { c ->
        val s = c.session
        val st = s.status()
        val df = s.df()
        val db = try { dbFields(s.dbStatus()) } catch (e: BtpException) { if (e.remote) mapOf("error" to e.codeName) else throw e }
        val up = try { s.updateStatus().let { u -> linkedMapOf("state" to (UpdateStatusR.STATES.getOrNull(u.state) ?: "unknown"), "version" to u.version, "permille" to u.permille) } }
            catch (e: BtpException) { if (e.remote) mapOf("error" to e.codeName) else throw e }
        linkedMapOf("device" to deviceFields(c), "battery" to (if (st.battery == 255) null else st.battery), "charging" to (st.charging == 1),
            "sd" to dfFields(df), "db" to db, "update" to up, "transport" to c.transport, "encrypted" to s.encrypted)
    }
}
