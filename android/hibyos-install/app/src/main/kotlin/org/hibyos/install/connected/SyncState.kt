package org.hibyos.install.connected

import org.hibyos.install.ErrorInfo

/** Where the connection to the player stands. */
sealed interface Link {
    data object Idle : Link
    data object Searching : Link
    data class Connecting(val device: SyncDevice) : Link

    /** The player shows a 6-digit code; the user types it here. [waiting]: code sent, waiting for Allow. */
    data class Pairing(val device: SyncDevice, val code: String = "", val waiting: Boolean = false, val wrong: Boolean = false) : Link
    data class Connected(val device: SyncDevice, val info: SyncDeviceInfo, val space: SyncSpace?) : Link
}

/** The card destinations offered for sending (section "send" of the connected mode). */
enum class Destination(val path: String) {
    MUSIC("Music"), BOOKS("Books"), PICTURES("Pictures"), APPS("Apps"),

    /** The top of the card, where the player looks for firmware updates (r3ii_2025.upt). */
    UPDATE(""),
}

data class Transfer(
    val jobs: List<SendJob> = emptyList(),
    val running: Boolean = false,
    /** Bytes hashed plus bytes sent, over twice the total (each file is read twice). */
    val done: Long = 0,
    val total: Long = 0,
    val current: String = "",
    val hashing: Boolean = false,
    val error: ErrorInfo? = null,
) {
    val sent: Int get() = jobs.count { it.status == JobStatus.SENT }
    val skipped: Int get() = jobs.count { it.status == JobStatus.SKIPPED }
    /** Hidden files (".nomedia") are left out on purpose: they are not a problem. */
    val problems: Int get() = jobs.count {
        !it.finished && it.status != JobStatus.WAITING && !(it.status == JobStatus.BAD_NAME && it.local.substringAfterLast('/').startsWith("."))
    }
    val left: Int get() = jobs.count { it.retryable }
    val finished: Boolean get() = !running && jobs.isNotEmpty() && jobs.none { it.status == JobStatus.WAITING }
}

/** What a file-browser action is waiting for. */
sealed interface Prompt {
    data class NewFolder(val name: String = "") : Prompt
    data class Rename(val entry: SyncEntry, val name: String) : Prompt
    data class Delete(val entry: SyncEntry) : Prompt
}

data class Browser(
    val path: String = "",
    val entries: List<SyncEntry>? = null,
    val loading: Boolean = false,
    val error: ErrorInfo? = null,
    /** An entry being moved: the user walks to the target folder and taps "Move here". */
    val moving: String? = null,
    val prompt: Prompt? = null,
    val working: Boolean = false,
)

sealed interface LibraryPhase {
    data object Idle : LibraryPhase
    data class Loading(val done: Long, val total: Long) : LibraryPhase
    data class Ready(val library: Library) : LibraryPhase
    data class Failed(val error: ErrorInfo) : LibraryPhase
}

/** Which list of the library is open: tab 0 artists, 1 albums, 2 songs; then an artist, then an album. */
data class LibraryNav(val tab: Int = 0, val artist: String? = null, val album: Pair<String, String>? = null)

sealed interface UpdatePhase {
    data object Idle : UpdatePhase
    data class Sending(val done: Long, val total: Long, val path: String) : UpdatePhase
    data class Sent(val files: Int, val skipped: Int) : UpdatePhase
    data class Failed(val error: ErrorInfo) : UpdatePhase
}

data class SyncState(
    /** This build has a BTP/1 client. */
    val available: Boolean = false,
    val found: List<SyncDevice> = emptyList(),
    val paired: List<PairedDevice> = emptyList(),
    val link: Link = Link.Idle,
    val error: ErrorInfo? = null,
    val dest: Destination = Destination.MUSIC,
    /** A folder chosen in the browser ("Send here"), instead of [dest]. */
    val destPath: String? = null,
    val replace: Boolean = false,
    val transfer: Transfer = Transfer(),
    val browser: Browser = Browser(),
    val library: LibraryPhase = LibraryPhase.Idle,
    val libraryNav: LibraryNav = LibraryNav(),
    val update: UpdatePhase = UpdatePhase.Idle,
) {
    val connected: Link.Connected? get() = link as? Link.Connected
    val target: String get() = destPath ?: dest.path

    /** Work is running: leaving the screen is held back. */
    val busy: Boolean
        get() = link is Link.Searching || link is Link.Connecting || (link is Link.Pairing && link.waiting) ||
            transfer.running || library is LibraryPhase.Loading || update is UpdatePhase.Sending || browser.working
}

/** Everything the connected screens can ask for. */
interface SyncActions {
    fun findDevices()
    fun connectTo(device: SyncDevice)
    fun connectAddress(text: String)
    fun setCode(text: String)
    fun confirmCode()
    fun cancelPairing()
    fun forget(deviceId: String)
    fun disconnect()
    fun refreshSpace()

    fun setDestination(d: Destination)
    fun setReplace(on: Boolean)
    fun pickFiles()
    fun pickFolder()
    fun startSend()
    fun clearSend()

    fun openFolder(path: String)
    fun refreshFolder()
    fun sendHere()
    fun prompt(p: Prompt?)
    fun applyPrompt()
    fun startMove(entry: SyncEntry)
    fun moveHere()
    fun cancelMove()

    fun loadLibrary()
    fun libraryNav(nav: LibraryNav)

    fun sendByosUpdate()
    fun cancelWork()
}
