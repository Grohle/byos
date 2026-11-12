package org.hibyos.install

import android.app.Application
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import org.hibyos.install.core.AppId
import org.hibyos.install.core.AppRelease
import org.hibyos.install.core.AppUpdater
import org.hibyos.install.core.Bundle
import org.hibyos.install.core.BundleStore
import org.hibyos.install.core.ByosBuild
import org.hibyos.install.core.FirmwareCache
import org.hibyos.install.core.BundleException
import org.hibyos.install.core.BundleUpdater
import org.hibyos.install.core.Discovery
import org.hibyos.install.core.DownloadException
import org.hibyos.install.core.FirmwareFetcher
import org.hibyos.install.core.FirmwareId
import org.hibyos.install.core.PatchException
import org.hibyos.install.core.PatchListener
import org.hibyos.install.core.PatchOptions
import org.hibyos.install.core.Patcher
import org.hibyos.install.core.PlayerClient
import org.hibyos.install.core.RemoteBundle
import org.hibyos.install.core.SendItem
import org.hibyos.install.core.SendListener
import org.hibyos.install.core.Sender
import org.hibyos.install.core.TransferException
import org.hibyos.install.connected.ConnectedModel
import org.hibyos.install.connected.LibraryDb
import org.hibyos.install.connected.Picked
import org.hibyos.install.connected.SyncBackend
import org.hibyos.install.connected.SyncConnector
import org.hibyos.install.connected.SyncEnv
import org.hibyos.install.connected.Track
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.security.MessageDigest
import java.util.concurrent.Executors

/** What only the activity can do: system pickers, the browser, the share sheet. */
interface Host {
    fun pickFirmware()
    fun pickFolder()

    /** ByOS connected: files and a folder to send to the player. */
    fun pickSyncFiles()
    fun pickSyncFolder()

    /** ByOS connected: hibyos-update.json and its payload tar. */
    fun pickSyncUpdate()

    /** Android 13+: asks once whether the transfer notification may be shown. */
    fun askNotifications()

    /** ByOS connected: "save as" for a file copied from the player. */
    fun createSyncDocument(name: String)
    fun open(url: String): Boolean
    fun share(files: List<File>)

    /** True when Android lets this app hand an APK to the system installer. */
    fun canInstallPackages(): Boolean

    /** Opens Android's "install unknown apps" setting for this app. */
    fun askInstallPermission()

    /** Hands a checked APK to the system installer; false when it could not be opened. */
    fun installPackage(file: File): Boolean
}

/**
 * The state of the installation and the work behind it. One job runs at a time on a worker
 * thread; the screens only read [state]. What was downloaded and built is kept in the app's
 * own storage, so closing the app does not lose it.
 */
class InstallViewModel(app: Application) : AndroidViewModel(app), Actions {
    var state by mutableStateOf(UiState())
        private set

    var host: Host? = null

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "hibyos-work") }

    @Volatile
    private var stop = false
    private val cancelled = { stop }

    private val files: File = app.filesDir
    private val firmwareDir = File(files, "firmware")
    private val outDir = File(files, "out")
    private val bundleDir = File(files, "bundle")
    private val store = BundleStore(bundleDir)
    private val workDir = File(app.cacheDir, "work")
    private val shareDir = File(app.cacheDir, "share")

    private var bundle: Bundle? = null
    private var builtIn: Bundle? = null
    private var remote: RemoteBundle? = null

    // ---- this app's own updates: their own worker, so a check never waits for a build
    private val appWorker = Executors.newSingleThreadExecutor { r -> Thread(r, "hibyos-app-update") }
    @Volatile
    private var appStop = false
    private val appUpdateDir = File(files, "app-update")
    private var appRelease: AppRelease? = null
    private var appApk: File? = null
    private val prefs = app.getSharedPreferences("app_update", android.content.Context.MODE_PRIVATE)

    /** What a build in [outDir] was made from: the ByOS files and this version of the app. */
    private fun stamp(b: Bundle) = ByosBuild.stamp(b, "android ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})")

    /** ByOS connected: its own worker, so that a transfer never waits for a patch and the other way round. */
    private val syncWorker = Executors.newSingleThreadExecutor { r -> Thread(r, "hibyos-sync") }
    val connected = ConnectedModel(object : SyncEnv {
        override fun connector(): SyncConnector? =
            SyncBackend.connector(app, File(app.filesDir, "sync"), BuildConfig.VERSION_NAME)
        override val clientName: String = (android.os.Build.MODEL ?: "Android").take(64)
        override val cacheDir: File = app.cacheDir
        override fun post(f: () -> Unit) {
            main.post(f)
        }
        override fun execute(f: () -> Unit) {
            try {
                syncWorker.execute(f)
            } catch (e: java.util.concurrent.RejectedExecutionException) {
                // the view model is gone
            }
        }
        override fun every(periodMs: Long, f: () -> Unit): java.io.Closeable {
            val t = java.util.concurrent.Executors.newSingleThreadScheduledExecutor { r ->
                Thread(r, "hibyos-sync-ping").apply { isDaemon = true }
            }
            t.scheduleWithFixedDelay({ try { f() } catch (e: Exception) {} }, periodMs, periodMs, java.util.concurrent.TimeUnit.MILLISECONDS)
            return java.io.Closeable { t.shutdownNow() }
        }
        override fun readLibrary(file: File): List<Track> = LibraryDb.read(file)
        override fun pickFiles() {
            host?.pickSyncFiles()
        }
        override fun pickFolder() {
            host?.pickSyncFolder()
        }
        override fun pickUpdateFiles() {
            host?.pickSyncUpdate()
        }
        override fun createDocument(name: String) {
            host?.createSyncDocument(name)
        }
        override fun work(note: org.hibyos.install.connected.WorkNote?) {
            if (note != null && !askedNotifications) {
                askedNotifications = true
                host?.askNotifications()
            }
            org.hibyos.install.connected.TransferService.update(app, note, android.os.SystemClock.elapsedRealtime())
        }
        override fun byosFiles(): List<SendItem>? {
            val b = bundle ?: return null
            if (state.patch !is PatchState.Done) return null
            return items(b, true)
        }
    })

    fun syncFilesPicked(uris: List<Uri>) {
        val app = getApplication<Application>()
        connected.addPicked { Picked.files(app, uris) }
    }

    private var askedNotifications = false

    /**
     * Files shared from another app: they are added to Send Files, which opens (over the
     * connection screen when no player is connected yet). ByOS update files among them are
     * handed to Install Update by the automatic destination. During a transfer they wait for
     * its end (the send screen, which stays open while it runs, says so).
     */
    fun syncShared(uris: List<Uri>) {
        val app = getApplication<Application>()
        connected.addPicked { Picked.files(app, uris) }
        if (state.busy || connected.state.busy) return
        state = state.copy(stack = listOf(Screen.HOME, Screen.SYNC, Screen.SYNC_SEND))
    }

    /** The document created for "Save to phone": the card file is copied into it. */
    fun syncSaveTo(uri: Uri) {
        val app = getApplication<Application>()
        connected.downloadTo(discard = { android.provider.DocumentsContract.deleteDocument(app.contentResolver, uri) }) {
            app.contentResolver.openOutputStream(uri, "w") ?: throw java.io.IOException("the file cannot be written")
        }
    }

    fun syncUpdatePicked(uris: List<Uri>) {
        val app = getApplication<Application>()
        connected.addUpdateFiles { Picked.files(app, uris) }
    }

    fun syncFolderPicked(tree: Uri) {
        val app = getApplication<Application>()
        connected.addPicked { Picked.tree(app, tree) }
    }

    init {
        val version = try {
            app.packageManager.getPackageInfo(app.packageName, 0).versionName ?: ""
        } catch (e: Exception) {
            ""
        }
        val auto = prefs.getBoolean("auto", true)
        state = state.copy(appVersion = version, appUpdateAuto = auto)
        worker.execute { start() }
        quietAppCheck()
    }

    // ---- small tools ------------------------------------------------------------------------

    private fun ui(f: (UiState) -> UiState) {
        main.post { state = f(state) }
    }

    /** Lets a progress report through about 12 times a second, and always the last one. */
    private class Pace {
        private var last = 0L
        fun due(force: Boolean = false): Boolean {
            val now = System.nanoTime()
            if (!force && now - last < 80_000_000L) return false
            last = now
            return true
        }
    }

    private fun begin(): Boolean {
        if (state.busy) return false
        stop = false
        return true
    }

    private fun nice(m: String?): String? = m?.trim()?.takeIf { it.isNotEmpty() }?.replaceFirstChar { it.uppercase() }

    private fun error(e: Throwable, update: Boolean = false): ErrorInfo = when (e) {
        is DownloadException -> when (e.kind) {
            DownloadException.Kind.OFFLINE ->
                if (update) ErrorInfo(R.string.err_update_offline, nice(e.message), R.string.hint_update_offline)
                else ErrorInfo(R.string.err_offline, nice(e.message), R.string.hint_offline)
            DownloadException.Kind.HTTP, DownloadException.Kind.NOT_A_FILE ->
                if (update) ErrorInfo(R.string.err_update_offline, nice(e.message), R.string.hint_update_offline)
                else ErrorInfo(R.string.err_http, nice(e.message), R.string.hint_http)
            DownloadException.Kind.MISMATCH ->
                if (update) ErrorInfo(R.string.err_untrusted, nice(e.message), R.string.hint_untrusted)
                else ErrorInfo(R.string.err_dl_mismatch, nice(e.message), R.string.hint_http)
            DownloadException.Kind.UNTRUSTED -> ErrorInfo(R.string.err_untrusted, nice(e.message), R.string.hint_untrusted)
            DownloadException.Kind.STORAGE -> ErrorInfo(R.string.err_storage, nice(e.message), R.string.hint_storage)
            DownloadException.Kind.CANCELLED -> ErrorInfo(R.string.err_generic, nice(e.message))
        }
        is PatchException -> when (e.reason) {
            PatchException.Reason.DAMAGED, PatchException.Reason.WRONG_FIRMWARE ->
                ErrorInfo(R.string.err_patch_damaged, nice(e.message), R.string.hint_patch_damaged)
            PatchException.Reason.BUNDLE -> ErrorInfo(R.string.err_patch_bundle, nice(e.message), R.string.hint_patch_bundle)
            PatchException.Reason.TOO_BIG -> ErrorInfo(R.string.err_patch_toobig, nice(e.message), R.string.hint_patch_bundle)
            PatchException.Reason.VERIFY -> ErrorInfo(R.string.err_patch_verify, nice(e.message), R.string.hint_patch_verify)
            PatchException.Reason.STORAGE -> ErrorInfo(R.string.err_storage, nice(e.message), R.string.hint_storage)
            PatchException.Reason.CANCELLED -> ErrorInfo(R.string.err_generic, nice(e.message))
        }
        is TransferException -> when (e.kind) {
            TransferException.Kind.UNREACHABLE -> ErrorInfo(R.string.err_unreachable, nice(e.message), R.string.hint_unreachable)
            TransferException.Kind.NOT_A_PLAYER -> ErrorInfo(R.string.err_not_player, nice(e.message), R.string.hint_not_player)
            TransferException.Kind.NO_SD_CARD -> ErrorInfo(R.string.err_no_sd, nice(e.message), R.string.hint_no_sd)
            TransferException.Kind.REFUSED -> ErrorInfo(R.string.err_refused, nice(e.message), R.string.hint_refused)
            TransferException.Kind.INTERRUPTED -> ErrorInfo(R.string.err_interrupted, nice(e.message), R.string.hint_interrupted)
            TransferException.Kind.MISMATCH -> ErrorInfo(R.string.err_mismatch, nice(e.message), R.string.hint_mismatch)
            TransferException.Kind.LOCAL -> ErrorInfo(R.string.err_local, nice(e.message), R.string.hint_local)
            TransferException.Kind.CANCELLED -> ErrorInfo(R.string.err_generic, nice(e.message))
        }
        is BundleException -> ErrorInfo(R.string.err_patch_bundle, nice(e.message), R.string.hint_patch_bundle)
        else -> ErrorInfo(R.string.err_generic, nice(e.message) ?: e.javaClass.simpleName)
    }

    private fun info(b: Bundle, builtIn: Boolean) = BundleInfo(
        hibyosVersion = b.hibyosVersion,
        generatedFrom = b.generatedFrom,
        builtIn = builtIn,
        firmwareLabel = b.firmware.label,
        firmwareVersion = b.firmware.version.ifEmpty { "1.3" },
        firmwareFile = b.firmware.fileName,
        firmwareSize = b.firmware.size ?: 0,
        firmwarePage = b.firmware.page,
        firmwareBuild = b.firmware.build,
        canDownload = b.firmware.sources.isNotEmpty(),
        outName = b.outName,
        overlayFiles = b.overlay.size,
        sdFiles = b.sd.size,
        sdSize = b.sdSize,
        port = b.transferPort,
        notices = b.notices.map { NoticeItem(it.title, it.entry) },
        canUpdate = this.builtIn?.let { it.updateIndex != null && it.updatePrefix != null && it.updateKeys.isNotEmpty() } ?: false,
    )

    /** The space "Free space" gives back: the builds and partial files (the checked firmware is kept). */
    private fun used(): Long {
        var n = 0L
        for (d in listOf(outDir, workDir, shareDir, appUpdateDir)) d.walkTopDown().forEach { if (it.isFile) n += it.length() }
        partialFirmware().forEach { n += it.length() }
        return n
    }

    /** A firmware download or copy that did not finish (never the checked file). */
    private fun partialFirmware(): List<File> =
        firmwareDir.listFiles()?.filter { it.isFile && (it.name.endsWith(".part") || it.name.endsWith(".tmp")) } ?: emptyList()

    // ---- start: the ByOS files, and what a previous run left -------------------------------

    private fun start() {
        try {
            val app = getApplication<Application>()
            val want = try {
                val pi = app.packageManager.getPackageInfo(app.packageName, 0)
                "${pi.lastUpdateTime}:${pi.versionName}"
            } catch (e: Exception) {
                "?"
            }
            // the core's store, shared with byos-cli: the shipped copy, or a newer signed one
            val base = store.builtin(want) { app.assets.open("hibyos-bundle.zip") }
            builtIn = base
            val current = store.current(base)
            val use = current.bundle
            bundle = use
            val bi = info(use, current.builtIn)
            val fw = restoreFirmware(use)
            // a build made from other ByOS files or by another version of the app, or one that
            // stopped half way, is removed now instead of getting in the way of the next one
            val discarded = sweepBuild(use)
            val patch = restorePatch(use)
            val usedNow = used()
            ui { it.copy(bundle = bi, firmware = fw, patch = patch, storageUsed = usedNow, startError = null, buildDiscarded = discarded) }
        } catch (e: Exception) {
            val err = error(e)
            ui { it.copy(startError = err) }
        }
    }

    private fun restoreFirmware(b: Bundle): FirmwareState {
        val f = FirmwareCache.ready(firmwareDir, b.firmware) ?: return FirmwareState.Idle
        return FirmwareState.Ready(f.size, f.sha256)
    }

    /**
     * Clears what an earlier build left that does not fit [b] and this app (core ByosBuild.sweep).
     * True when something that looked like a build was removed. The firmware is never touched.
     */
    private fun sweepBuild(b: Bundle): Boolean {
        val had = outDir.listFiles()?.any { it.name == ByosBuild.RECORD || it.name == ByosBuild.STAMP || it.name.endsWith(".part") } == true
        workDir.deleteRecursively()
        val kept = ByosBuild.sweep(b, outDir, workDir, stamp(b))
        if (!kept) shareDir.deleteRecursively()
        return had && !kept
    }

    private fun restorePatch(b: Bundle): PatchState {
        val r = ByosBuild.restore(b, outDir, stamp(b)) ?: return PatchState.Idle
        return PatchState.Done(r.size, r.sha256, r.buildId, r.files, r.hibyosVersion)
    }

    // ---- navigation -------------------------------------------------------------------------

    override fun go(screen: Screen) {
        val s = state
        if ((s.busy || connected.state.busy) && screen != s.screen) return
        val at = s.stack.indexOf(screen)
        val stack = if (at >= 0) s.stack.subList(0, at + 1).toList() else s.stack + screen
        state = s.copy(stack = stack, delivered = s.delivered || screen == Screen.INSTALL)
    }

    /** False when there is nowhere to go back to (the activity then closes). */
    fun canGoBack(): Boolean = state.stack.size > 1 || state.busy || connected.state.busy

    override fun back() {
        val s = state
        if (s.busy || connected.state.busy || s.stack.size <= 1) return
        // inside the connected screens "back" first walks up their own levels
        if (s.screen == Screen.SYNC_FILES && connected.state.browser.moving == null && connected.up()) return
        if (s.screen == Screen.SYNC_LIBRARY && connected.libraryBack()) return
        state = s.copy(stack = s.stack.dropLast(1))
    }

    override fun cancel() {
        stop = true
    }

    override fun openUrl(url: String) {
        host?.open(url)
    }

    // ---- step 1: the official firmware ------------------------------------------------------

    private fun markFirmware(id: FirmwareId) {
        FirmwareCache.mark(firmwareDir, id)
    }

    override fun downloadFirmware() {
        val b = bundle ?: return
        if (!begin()) return
        // the checked firmware is kept for good: never downloaded twice
        FirmwareCache.ready(firmwareDir, b.firmware)?.let {
            state = state.copy(firmware = FirmwareState.Ready(it.size, it.sha256))
            return
        }
        val dest = File(firmwareDir, b.firmware.fileName)
        val size = b.firmware.size ?: 0
        val have = File(dest.path + ".part").length()
        if (files.usableSpace < size - have + (8L shl 20)) {
            state = state.copy(firmware = FirmwareState.Failed(
                ErrorInfo(R.string.err_space, null, R.string.hint_space, mb(size)), true))
            return
        }
        state = state.copy(firmware = FirmwareState.Downloading(have, size))
        worker.execute {
            try {
                firmwareDir.mkdirs()
                FirmwareCache.unmark(firmwareDir)
                val pace = Pace()
                val id = FirmwareFetcher.fetch(b.firmware, dest, cancelled,
                    progress = { done, total ->
                        if (pace.due(done == total)) ui { it.copy(firmware = FirmwareState.Downloading(done, if (total > 0) total else size)) }
                    },
                    onCheck = { f -> if (pace.due(f >= 1.0)) ui { it.copy(firmware = FirmwareState.Checking(f.toFloat())) } })
                markFirmware(id)
                val usedNow = used()
                ui { it.copy(firmware = FirmwareState.Ready(id.size, id.sha256), storageUsed = usedNow) }
            } catch (e: DownloadException) {
                if (e.kind == DownloadException.Kind.CANCELLED) ui { it.copy(firmware = FirmwareState.Idle) }
                else ui { it.copy(firmware = FirmwareState.Failed(error(e), true)) }
            } catch (e: Exception) {
                ui { it.copy(firmware = FirmwareState.Failed(error(e), true)) }
            }
        }
    }

    override fun pickFirmware() {
        if (state.busy) return
        host?.pickFirmware()
    }

    /** Takes the file the user chose, checking it while it is copied into the app. */
    fun importFirmware(uri: Uri) {
        val b = bundle ?: return
        if (!begin()) return
        state = state.copy(firmware = FirmwareState.Checking(0f))
        worker.execute {
            val app = getApplication<Application>()
            val tmp = File(firmwareDir, "picked.tmp")
            var name: String? = null
            try {
                firmwareDir.mkdirs()
                var total = 0L
                try {
                    app.contentResolver.query(uri, null, null, null, null)?.use { c ->
                        if (c.moveToFirst()) {
                            val ni = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                            val si = c.getColumnIndex(OpenableColumns.SIZE)
                            if (ni >= 0) name = c.getString(ni)
                            if (si >= 0 && !c.isNull(si)) total = c.getLong(si)
                        }
                    }
                } catch (e: Exception) {
                    // the name is only for the message
                }
                val md5 = MessageDigest.getInstance("MD5")
                val sha = MessageDigest.getInstance("SHA-256")
                val expected = b.firmware.size ?: 0
                val limit = if (expected > 0) expected else 512L shl 20
                var n = 0L
                val buf = ByteArray(1 shl 16)
                val pace = Pace()
                var tooBig = false
                val input = app.contentResolver.openInputStream(uri) ?: throw IOException("the file cannot be opened")
                input.use { i ->
                    FileOutputStream(tmp).use { o ->
                        while (true) {
                            if (stop) throw StoppedException()
                            val r = i.read(buf)
                            if (r < 0) break
                            md5.update(buf, 0, r)
                            sha.update(buf, 0, r)
                            n += r
                            // a bigger file cannot be the firmware: it is still measured, but no longer kept
                            if (n > limit) tooBig = true else o.write(buf, 0, r)
                            if (pace.due()) {
                                val f = (n.toDouble() / maxOf(total, expected, 1L)).coerceIn(0.0, 1.0).toFloat()
                                ui { it.copy(firmware = FirmwareState.Checking(f)) }
                            }
                            if (n > (2L shl 30)) break
                        }
                    }
                }
                val id = FirmwareId(n, hex(md5.digest()), hex(sha.digest()))
                if (tooBig || !id.matches(b.firmware)) {
                    tmp.delete()
                    val shown = name
                    ui { it.copy(firmware = FirmwareState.Wrong(shown, n, id.md5)) }
                } else {
                    val dest = File(firmwareDir, b.firmware.fileName)
                    dest.delete()
                    if (!tmp.renameTo(dest)) throw IOException("the file could not be kept")
                    markFirmware(id)
                    val usedNow = used()
                    ui { it.copy(firmware = FirmwareState.Ready(id.size, id.sha256), storageUsed = usedNow) }
                }
            } catch (e: StoppedException) {
                tmp.delete()
                val keep = restoreFirmware(b)
                ui { it.copy(firmware = keep) }
            } catch (e: Exception) {
                tmp.delete()
                val msg = nice(e.message) ?: e.javaClass.simpleName
                val full = files.usableSpace < (8L shl 20)
                ui {
                    it.copy(firmware = FirmwareState.Failed(
                        if (full) ErrorInfo(R.string.err_storage, msg, R.string.hint_storage) else ErrorInfo(R.string.err_read, msg), false))
                }
            }
        }
    }

    override fun forgetFirmware() {
        if (bundle == null || state.busy) return
        // the checked file stays until another checked one replaces it (a firmware is deleted
        // only when its checksum no longer matches); only the screen goes back to the choices
        state = state.copy(firmware = FirmwareState.Idle)
    }

    // ---- step 2: patching -------------------------------------------------------------------

    override fun patch() {
        val b = bundle ?: return
        if (state.firmware !is FirmwareState.Ready) return
        if (!begin()) return
        val need = (b.firmware.size ?: (48L shl 20)) * 2 + (24L shl 20)
        // everything an earlier build left is replaced: it counts as free space
        val reusable = (outDir.listFiles()?.filter { it.isFile }?.sumOf { it.length() } ?: 0L) +
            (workDir.listFiles()?.sumOf { it.length() } ?: 0L)
        if (files.usableSpace + reusable < need) {
            state = state.copy(patch = PatchState.Failed(ErrorInfo(R.string.err_space, null, R.string.hint_space, mb(need))))
            return
        }
        state = state.copy(patch = PatchState.Running(org.hibyos.install.core.Stage.CHECK, 0f), delivered = false,
            buildDiscarded = false, startedOver = false,
            wifi = state.wifi.copy(phase = if (state.wifi.player != null) WifiPhase.Connected(state.wifi.player!!) else WifiPhase.Idle),
            manual = ManualState.Idle)
        worker.execute {
            val input = File(firmwareDir, b.firmware.fileName)
            try {
                shareDir.deleteRecursively()
                val pace = Pace()
                // the core's build step (also byos-cli's "build"): patch, then record the result
                // with the stamp: what an earlier run left is cleared first, and a later start can
                // tell what this build was made from
                val (r, _) = ByosBuild.patch(input, b, outDir, workDir, PatchOptions(cancelled = cancelled),
                    PatchListener { stage, f ->
                        if (pace.due(f >= 1.0)) ui { it.copy(patch = PatchState.Running(stage, stage.overall(f).toFloat())) }
                    }, stamp(b))
                val usedNow = used()
                ui { it.copy(patch = PatchState.Done(r.size, r.sha256, r.buildId, r.files, r.hibyosVersion), storageUsed = usedNow) }
            } catch (e: PatchException) {
                when (e.reason) {
                    PatchException.Reason.CANCELLED -> ui { it.copy(patch = PatchState.Idle) }
                    PatchException.Reason.WRONG_FIRMWARE -> {
                        // the kept file is no longer the one that was checked: ask for it again
                        FirmwareCache.forget(firmwareDir, b.firmware)
                        ui { it.copy(patch = PatchState.Failed(error(e)), firmware = FirmwareState.Idle) }
                    }
                    else -> ui { it.copy(patch = PatchState.Failed(error(e))) }
                }
            } catch (e: Exception) {
                ui { it.copy(patch = PatchState.Failed(error(e))) }
            } finally {
                workDir.deleteRecursively()
            }
        }
    }

    // ---- step 3: sending --------------------------------------------------------------------

    // the order (ByOS folder first, payload version last, firmware at the very end) is the
    // core's, shared with byos-cli's build-send
    private fun items(b: Bundle, firmware: Boolean): List<SendItem> =
        ByosBuild.items(b, if (firmware) File(outDir, b.outName) else null, (state.patch as? PatchState.Done)?.sha256)

    override fun setAddress(text: String) {
        val s = state
        if (s.busy) return
        val phase = if (s.wifi.phase is WifiPhase.Failed || s.wifi.phase is WifiPhase.Connected) WifiPhase.Idle else s.wifi.phase
        state = s.copy(wifi = s.wifi.copy(address = text.take(64), phase = phase, player = null))
    }

    override fun findPlayers() {
        val b = bundle ?: return
        if (!begin()) return
        val app = getApplication<Application>()
        val hosts = Net.neighbours(app)
        if (hosts.isEmpty()) {
            state = state.copy(wifi = state.wifi.copy(phase = WifiPhase.Failed(
                ErrorInfo(R.string.err_no_wifi, null, R.string.hint_no_wifi), false)))
            return
        }
        state = state.copy(wifi = state.wifi.copy(found = emptyList(), player = null, phase = WifiPhase.Searching(0, hosts.size)))
        worker.execute {
            try {
                val pace = Pace()
                val found = Discovery.scan(hosts, b.transferPort, b.transferHome, cancelled = cancelled,
                    socketFactory = Net.socketFactory(app),
                    onProgress = { done, total ->
                        if (pace.due(done == total)) ui {
                            if (it.wifi.phase is WifiPhase.Searching) it.copy(wifi = it.wifi.copy(phase = WifiPhase.Searching(done, total))) else it
                        }
                    },
                    onFound = { p ->
                        val fp = FoundPlayer(p.host, p.port, p.name, p.sdCard)
                        ui { it.copy(wifi = it.wifi.copy(found = (it.wifi.found + fp).distinctBy { x -> x.host })) }
                    }).map { FoundPlayer(it.host, it.port, it.name, it.sdCard) }
                ui {
                    when {
                        stop -> it.copy(wifi = it.wifi.copy(found = found, phase = WifiPhase.Idle))
                        found.isEmpty() -> it.copy(wifi = it.wifi.copy(found = found, phase = WifiPhase.Failed(
                            ErrorInfo(R.string.err_none_found, null, R.string.hint_none_found), false)))
                        found.size == 1 && found[0].sdCard -> it.copy(wifi = it.wifi.copy(found = found, player = found[0],
                            address = found[0].host, phase = WifiPhase.Connected(found[0])))
                        else -> it.copy(wifi = it.wifi.copy(found = found, phase = WifiPhase.Idle))
                    }
                }
            } catch (e: Exception) {
                ui { it.copy(wifi = it.wifi.copy(phase = WifiPhase.Failed(error(e), false))) }
            }
        }
    }

    override fun connect(player: FoundPlayer?) {
        val b = bundle ?: return
        if (!begin()) return
        val target = if (player != null) player.host to player.port else PlayerClient.parseAddress(state.wifi.address)?.let {
            // what the player shows already carries its port; a bare address gets the one of the firmware
            if (state.wifi.address.contains(':')) it else it.first to b.transferPort
        }
        if (target == null) {
            state = state.copy(wifi = state.wifi.copy(phase = WifiPhase.Failed(
                ErrorInfo(R.string.err_bad_address, null, R.string.hint_bad_address), false)))
            return
        }
        val address = if (player != null) player.host else state.wifi.address
        state = state.copy(wifi = state.wifi.copy(address = address, player = null, phase = WifiPhase.Connecting))
        val app = getApplication<Application>()
        worker.execute {
            try {
                val p = PlayerClient(target.first, target.second, b.transferHome, socketFactory = Net.socketFactory(app)).identify()
                val fp = FoundPlayer(p.host, p.port, p.name, p.sdCard)
                if (!p.sdCard) {
                    ui { it.copy(wifi = it.wifi.copy(phase = WifiPhase.Failed(
                        ErrorInfo(R.string.err_no_sd, nice(p.sdText), R.string.hint_no_sd), false))) }
                } else {
                    ui { it.copy(wifi = it.wifi.copy(player = fp, phase = WifiPhase.Connected(fp))) }
                }
            } catch (e: Exception) {
                ui { it.copy(wifi = it.wifi.copy(phase = WifiPhase.Failed(error(e), false))) }
            }
        }
    }

    override fun changePlayer() {
        val s = state
        if (s.busy) return
        state = s.copy(wifi = s.wifi.copy(player = null, phase = WifiPhase.Idle))
    }

    override fun send() {
        val b = bundle ?: return
        val player = state.wifi.player ?: return
        if (state.patch !is PatchState.Done) return
        if (!begin()) return
        val list = items(b, true)
        val total = list.sumOf { it.size } * 2
        state = state.copy(wifi = state.wifi.copy(phase = WifiPhase.Sending(0, total, "", SendListener.Phase.PREPARE)))
        val app = getApplication<Application>()
        worker.execute {
            try {
                val client = PlayerClient(player.host, player.port, b.transferHome, socketFactory = Net.socketFactory(app))
                val pace = Pace()
                var lastPath = ""
                val r = Sender.send(client, list, true, cancelled, SendListener { done, all, path, phase ->
                    if (pace.due(path != lastPath || done == all)) {
                        lastPath = path
                        ui {
                            if (it.wifi.phase is WifiPhase.Sending) it.copy(wifi = it.wifi.copy(phase = WifiPhase.Sending(done, all, path, phase))) else it
                        }
                    }
                })
                ui {
                    it.copy(delivered = true, wifi = it.wifi.copy(
                        phase = WifiPhase.Done(r.files, r.skipped, r.verified, r.readBackProblem != null)))
                }
            } catch (e: TransferException) {
                if (e.kind == TransferException.Kind.CANCELLED) ui { it.copy(wifi = it.wifi.copy(phase = WifiPhase.Connected(player))) }
                else ui { it.copy(wifi = it.wifi.copy(phase = WifiPhase.Failed(error(e), true))) }
            } catch (e: Exception) {
                ui { it.copy(wifi = it.wifi.copy(phase = WifiPhase.Failed(error(e), true))) }
            }
        }
    }

    override fun saveToFolder() {
        if (state.busy || state.patch !is PatchState.Done) return
        host?.pickFolder()
    }

    /** Writes the firmware and the ByOS folder under the folder the user chose. */
    fun saveTo(tree: Uri) {
        val b = bundle ?: return
        if (state.patch !is PatchState.Done) return
        if (!begin()) return
        val list = items(b, true)
        state = state.copy(manual = ManualState.Saving(0, list.sumOf { it.size } * 2, ""))
        val app = getApplication<Application>()
        worker.execute {
            try {
                val pace = Pace()
                val name = Deliver.saveTree(app, tree, list, cancelled) { done, total, path ->
                    if (pace.due(done == total)) ui { if (it.manual is ManualState.Saving) it.copy(manual = ManualState.Saving(done, total, path)) else it }
                }
                ui { it.copy(manual = ManualState.Saved(name, list.size), delivered = true) }
            } catch (e: StoppedException) {
                ui { it.copy(manual = ManualState.Idle) }
            } catch (e: Exception) {
                val msg = nice(e.message) ?: e.javaClass.simpleName
                ui { it.copy(manual = ManualState.Failed(ErrorInfo(R.string.err_save, msg, R.string.hint_save))) }
            }
        }
    }

    override fun share() {
        val b = bundle ?: return
        if (state.patch !is PatchState.Done) return
        if (!begin()) return
        state = state.copy(manual = ManualState.Preparing(0f))
        worker.execute {
            try {
                val zip = File(shareDir, "ByOS.zip")
                val stamp = File(shareDir, "ByOS.version")
                val want = "${b.hibyosVersion}:${b.generatedFrom}"
                if (!zip.isFile || !stamp.isFile || stamp.readText() != want) {
                    val pace = Pace()
                    stamp.delete()
                    Deliver.zip(items(b, false), zip, cancelled) { done, total ->
                        if (pace.due(done == total)) ui { if (it.manual is ManualState.Preparing) it.copy(manual = ManualState.Preparing(done.toFloat() / total)) else it }
                    }
                    stamp.writeText(want)
                }
                val out = File(outDir, b.outName)
                val usedNow = used()
                main.post {
                    state = state.copy(manual = ManualState.Idle, storageUsed = usedNow)
                    host?.share(listOf(out, zip))
                }
            } catch (e: StoppedException) {
                ui { it.copy(manual = ManualState.Idle) }
            } catch (e: Exception) {
                val msg = nice(e.message) ?: e.javaClass.simpleName
                ui { it.copy(manual = ManualState.Failed(ErrorInfo(R.string.err_share, msg, R.string.hint_storage))) }
            }
        }
    }

    // ---- about: newer ByOS files, licences, space -----------------------------------------

    override fun checkUpdate() {
        val base = builtIn ?: return
        val current = bundle ?: return
        val index = base.updateIndex ?: return
        val prefix = base.updatePrefix ?: return
        if (!begin()) return
        state = state.copy(update = UpdateState.Checking)
        worker.execute {
            try {
                // the keys always come from the files built into the app, never from a download
                val r = BundleUpdater.check(index, base.updateKeys, prefix, cancelled)
                if (BundleUpdater.isNewer(r.version, current.hibyosVersion)) {
                    remote = r
                    ui { it.copy(update = UpdateState.Available(r.version, r.size)) }
                } else {
                    ui { it.copy(update = UpdateState.UpToDate(current.hibyosVersion)) }
                }
            } catch (e: DownloadException) {
                if (e.kind == DownloadException.Kind.CANCELLED) ui { it.copy(update = UpdateState.Idle) }
                else ui { it.copy(update = UpdateState.Failed(error(e, true))) }
            } catch (e: Exception) {
                ui { it.copy(update = UpdateState.Failed(error(e, true))) }
            }
        }
    }

    override fun downloadUpdate() {
        val r = remote ?: return
        if (!begin()) return
        state = state.copy(update = UpdateState.Downloading(r.version, 0, r.size))
        worker.execute {
            try {
                val pace = Pace()
                val b = store.install(r, cancelled, { done, total ->
                    if (pace.due(done == total)) ui { it.copy(update = UpdateState.Downloading(r.version, done, if (total > 0) total else r.size)) }
                }) {
                    val old = bundle
                    if (old != null && old !== builtIn) old.close()
                }
                bundle = b
                remote = null
                val bi = info(b, false)
                val fw = restoreFirmware(b)
                // the build of the older files goes now; the checked firmware stays
                sweepBuild(b)
                val patch = restorePatch(b)
                ui {
                    it.copy(bundle = bi, firmware = if (it.firmware is FirmwareState.Ready) fw else it.firmware, patch = patch,
                        delivered = false, manual = ManualState.Idle, update = UpdateState.Installed(b.hibyosVersion),
                        wifi = it.wifi.copy(phase = if (it.wifi.player != null) WifiPhase.Connected(it.wifi.player) else WifiPhase.Idle))
                }
            } catch (e: DownloadException) {
                if (e.kind == DownloadException.Kind.CANCELLED) ui { it.copy(update = UpdateState.Available(r.version, r.size)) }
                else ui { it.copy(update = UpdateState.Failed(error(e, true))) }
            } catch (e: Exception) {
                ui { it.copy(update = UpdateState.Failed(error(e, true))) }
            }
        }
    }

    override fun openNotice(item: NoticeItem?) {
        val app = getApplication<Application>()
        if (state.busy) return
        val b = bundle
        worker.execute {
            try {
                val title: String
                val text: String
                if (item == null) {
                    title = app.getString(R.string.licenses_android)
                    text = app.getString(R.string.licenses_android_intro) + "\n\n" +
                        app.assets.open("licenses/Apache-2.0.txt").use { String(it.readBytes(), Charsets.UTF_8) }
                } else {
                    title = item.title
                    text = b?.readText(item.entry) ?: ""
                }
                val parts = noticeParagraphs(text)
                ui { it.copy(licenseTitle = title, licenseText = parts, stack = it.stack.filter { s -> s != Screen.LICENSE_TEXT } + Screen.LICENSE_TEXT) }
            } catch (e: Exception) {
                // a notice that cannot be read is simply not opened
            }
        }
    }

    /** Frees the space of the builds and partial files. The checked official firmware is kept. */
    override fun cleanUp() {
        if (!begin()) return
        state = state.copy(patch = PatchState.Idle, manual = ManualState.Idle, delivered = false,
            wifi = state.wifi.copy(phase = if (state.wifi.player != null) WifiPhase.Connected(state.wifi.player!!) else WifiPhase.Idle))
        worker.execute {
            for (d in listOf(outDir, workDir, shareDir)) d.deleteRecursively()
            partialFirmware().forEach { it.delete() }
            if (state.appUpdate !is AppUpdateState.Downloading) appUpdateDir.deleteRecursively()
            val usedNow = used()
            ui { it.copy(storageUsed = usedNow) }
        }
    }

    /**
     * "Start Over" on the build screen: removes the build steps (the patched firmware, its
     * record and stamp, partial files, the work folder, the zip for sharing). The official
     * firmware stays, so the next build starts at once.
     */
    override fun startOver() {
        val b = bundle ?: return
        if (!begin()) return
        state = state.copy(patch = PatchState.Idle, manual = ManualState.Idle, delivered = false, buildDiscarded = false,
            wifi = state.wifi.copy(phase = if (state.wifi.player != null) WifiPhase.Connected(state.wifi.player!!) else WifiPhase.Idle))
        worker.execute {
            ByosBuild.startOver(outDir, workDir)
            shareDir.deleteRecursively()
            val fw = restoreFirmware(b)
            val usedNow = used()
            ui { it.copy(firmware = if (it.firmware is FirmwareState.Downloading || it.firmware is FirmwareState.Checking) it.firmware else fw,
                startedOver = true, storageUsed = usedNow) }
        }
    }

    // ---- this app's own updates (core AppUpdater, docs/APP_UPDATES.md) ----------------------

    private fun appError(e: Throwable): ErrorInfo = when {
        e is DownloadException && e.kind == DownloadException.Kind.HTTP && e.status == 429 ->
            ErrorInfo(R.string.err_appupd_limit, nice(e.message), R.string.hint_appupd_limit)
        e is DownloadException && (e.kind == DownloadException.Kind.UNTRUSTED || e.kind == DownloadException.Kind.MISMATCH) ->
            ErrorInfo(R.string.err_appupd_untrusted, nice(e.message), R.string.hint_appupd_untrusted)
        e is DownloadException && e.kind == DownloadException.Kind.STORAGE ->
            ErrorInfo(R.string.err_storage, nice(e.message), R.string.hint_storage)
        e is DownloadException -> ErrorInfo(R.string.err_appupd_offline, nice(e.message), R.string.hint_appupd_offline)
        else -> ErrorInfo(R.string.err_appupd_untrusted, nice(e.message) ?: e.javaClass.simpleName, R.string.hint_appupd_untrusted)
    }

    private fun lang(): String = java.util.Locale.getDefault().language

    /** At most once a day, when the app starts and the user did not turn it off. Silent unless an update is found. */
    private fun quietAppCheck() {
        if (!prefs.getBoolean("auto", true)) return
        val last = prefs.getLong("last", 0)
        val now = System.currentTimeMillis()
        if (now - last in 0 until 24L * 3600 * 1000) return
        appCheck(quiet = true)
    }

    override fun checkAppUpdate() = appCheck(quiet = false)

    private fun appCheck(quiet: Boolean) {
        val s = state.appUpdate
        if (s is AppUpdateState.Checking || s is AppUpdateState.Downloading) return
        appStop = false
        if (!quiet) state = state.copy(appUpdate = AppUpdateState.Checking)
        try {
            appWorker.execute {
                try {
                    val r = AppUpdater.check(AppId.ANDROID, BuildConfig.VERSION_NAME, BuildConfig.VERSION_CODE.toLong(),
                        cancelled = { appStop })
                    prefs.edit().putLong("last", System.currentTimeMillis()).apply()
                    val minSdk = r?.minOs?.substringBefore('.')?.toIntOrNull()
                    when {
                        r == null -> {
                            appUpdateDir.deleteRecursively() // an APK of this or an older version is no use any more
                            if (!quiet) ui { it.copy(appUpdate = AppUpdateState.UpToDate(BuildConfig.VERSION_NAME)) }
                        }
                        minSdk != null && minSdk > android.os.Build.VERSION.SDK_INT ->
                            if (!quiet) ui { it.copy(appUpdate = AppUpdateState.Failed(ErrorInfo(R.string.err_appupd_os, "Android API $minSdk", R.string.hint_appupd_os))) }
                        else -> {
                            appRelease = r
                            ui { it.copy(appUpdate = AppUpdateState.Available(r.version, r.size, r.notesFor(lang()))) }
                        }
                    }
                } catch (e: Exception) {
                    if (!quiet) {
                        val err = if (e is DownloadException && e.kind == DownloadException.Kind.CANCELLED) null else appError(e)
                        ui { it.copy(appUpdate = if (err == null) AppUpdateState.Idle else AppUpdateState.Failed(err)) }
                    }
                }
            }
        } catch (e: java.util.concurrent.RejectedExecutionException) {
            // the view model is gone
        }
    }

    override fun downloadAppUpdate() {
        val r = appRelease ?: return
        if (state.appUpdate is AppUpdateState.Downloading) return
        appStop = false
        val notes = r.notesFor(lang())
        state = state.copy(appUpdate = AppUpdateState.Downloading(r.version, 0, r.size, notes))
        val app = getApplication<Application>()
        appWorker.execute {
            try {
                appUpdateDir.mkdirs()
                // only this version's file stays in the folder
                appUpdateDir.listFiles()?.forEach { if (!it.name.startsWith(r.asset)) it.delete() }
                val pace = Pace()
                val apk = AppUpdater.download(r, File(appUpdateDir, r.asset), { appStop }) { done, total ->
                    if (pace.due(done == total)) ui { it.copy(appUpdate = AppUpdateState.Downloading(r.version, done, if (total > 0) total else r.size, notes)) }
                }
                // the signed SHA-256 matched; the APK must also be this app at the signed versionCode
                @Suppress("DEPRECATION")
                val info = app.packageManager.getPackageArchiveInfo(apk.path, 0)
                val code = info?.let { if (android.os.Build.VERSION.SDK_INT >= 28) it.longVersionCode else it.versionCode.toLong() }
                if (info == null || info.packageName != app.packageName || code != r.versionCode) {
                    apk.delete()
                    throw DownloadException(DownloadException.Kind.MISMATCH, "the downloaded file is not ByOS Install ${r.version}")
                }
                appApk = apk
                main.post { handOff(r.version, notes) }
            } catch (e: Exception) {
                val cancelled = e is DownloadException && e.kind == DownloadException.Kind.CANCELLED
                ui { it.copy(appUpdate = if (cancelled) AppUpdateState.Available(r.version, r.size, notes) else AppUpdateState.Failed(appError(e))) }
            }
        }
    }

    /** On the main thread: the checked APK to the system installer, or first to the permission screen. */
    private fun handOff(version: String, notes: String) {
        val apk = appApk ?: return
        val h = host
        if (h == null) {
            state = state.copy(appUpdate = AppUpdateState.Ready(version, notes))
            return
        }
        if (!h.canInstallPackages()) {
            state = state.copy(appUpdate = AppUpdateState.NeedsPermission(version, notes))
            return
        }
        state = state.copy(appUpdate = if (h.installPackage(apk)) AppUpdateState.Ready(version, notes)
            else AppUpdateState.Failed(ErrorInfo(R.string.err_appupd_open, null, R.string.hint_appupd_open)))
    }

    override fun installAppUpdate() {
        when (val s = state.appUpdate) {
            is AppUpdateState.NeedsPermission -> if (host?.canInstallPackages() == true) handOff(s.version, s.notes) else host?.askInstallPermission()
            is AppUpdateState.Ready -> handOff(s.version, s.notes)
            else -> {}
        }
    }

    /** The activity is back in front (for example from the "install unknown apps" setting). */
    fun resumed() {
        val s = state.appUpdate
        if (s is AppUpdateState.NeedsPermission && host?.canInstallPackages() == true) handOff(s.version, s.notes)
    }

    override fun setAppUpdateAuto(on: Boolean) {
        prefs.edit().putBoolean("auto", on).apply()
        state = state.copy(appUpdateAuto = on)
    }

    override fun onCleared() {
        stop = true
        appStop = true
        appWorker.shutdown()
        connected.close()
        syncWorker.shutdown()
        worker.shutdown()
        host = null
    }
}

fun mb(bytes: Long): String {
    val tenths = (bytes * 10 + (1L shl 19)) / (1L shl 20)
    return "${tenths / 10}.${tenths % 10} MB"
}
