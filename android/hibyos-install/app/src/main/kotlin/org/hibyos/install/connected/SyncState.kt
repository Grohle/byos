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
    /** Each file to the folder of its kind (SendPlan.auto). */
    AUTO(""),
    MUSIC("Music"), BOOKS("Books"), PICTURES("Pictures"), APPS("Apps"),

    /** The top of the card, where the player looks for firmware updates (r3ii_2025.upt). */
    UPDATE(""),
}

data class Transfer(
    val jobs: List<SendJob> = emptyList(),
    /** Songs this transfer added to the card: the player's library shows them after a rescan. */
    val newMusic: Int = 0,
    val running: Boolean = false,
    /** Bytes hashed plus bytes sent, over twice the total (each file is read twice). */
    val done: Long = 0,
    val total: Long = 0,
    val current: String = "",
    val hashing: Boolean = false,
    val error: ErrorInfo? = null,
    /** The connection dropped: this is the attempt to connect again (0: connected). */
    val reconnecting: Int = 0,
) {
    val sent: Int get() = jobs.count { it.status == JobStatus.SENT }
    val skipped: Int get() = jobs.count { it.status == JobStatus.SKIPPED }
    /** Hidden files (".nomedia") are left out on purpose, and ByOS update files go to Install Update: they are not a problem. */
    val problems: Int get() = jobs.count {
        !it.finished && it.status != JobStatus.WAITING && it.status != JobStatus.UPDATE_FILE &&
            !(it.status == JobStatus.BAD_NAME && it.local.substringAfterLast('/').startsWith("."))
    }
    val left: Int get() = jobs.count { it.retryable }
    val finished: Boolean get() = !running && jobs.isNotEmpty() && jobs.none { it.status == JobStatus.WAITING }
}

/** Work that keeps going while the app is in the background (the transfer service shows it). */
data class WorkNote(val kind: Kind, val done: Long, val total: Long, val detail: String, val reconnecting: Int = 0) {
    enum class Kind { SEND, UPDATE, INSTALL }

    /** 0 to 1000, or -1 when unknown. */
    val permille: Int get() = if (total > 0) (done * 1000 / total).toInt().coerceIn(0, 1000) else -1
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
    /**
     * An album that shares its folder with other files: only its songs move ([moving] is then
     * their folder, which stays). [movingLabel] names what moves.
     */
    val movingFiles: List<String> = emptyList(),
    val movingLabel: String? = null,
    val prompt: Prompt? = null,
    val working: Boolean = false,
    /** A card file being copied to the phone ("Save to phone"). */
    val download: Download? = null,
)

/** GET of one card file into a document the user created on the phone. */
data class Download(val name: String, val done: Long = 0, val total: Long = 0, val finished: Boolean = false, val error: ErrorInfo? = null) {
    val running: Boolean get() = !finished && error == null
}

/** LIB_RESCAN mode 0: the player looks for new, changed and removed songs, then the library is read again. */
sealed interface Rescan {
    data object Idle : Rescan
    data object Running : Rescan
    data object Done : Rescan

    /** This player cannot rescan on request: its own Music settings can. */
    data object Unsupported : Rescan
    data class Failed(val error: ErrorInfo) : Rescan
}

sealed interface LibraryPhase {
    data object Idle : LibraryPhase
    data class Loading(val done: Long, val total: Long) : LibraryPhase
    data class Ready(val library: Library) : LibraryPhase
    data class Failed(val error: ErrorInfo) : LibraryPhase
}

/** A song, an album or an artist the user acts on in the library. */
sealed interface LibTarget {
    data class Song(val track: Track) : LibTarget
    data class AlbumOf(val album: Album) : LibTarget
    data class ArtistOf(val artist: Artist) : LibTarget

    val tracks: List<Track>
        get() = when (this) {
            is Song -> listOf(track)
            is AlbumOf -> album.tracks
            is ArtistOf -> artist.albums.flatMap { it.tracks }
        }

    /** What Move and Show in Files act on: the song's file, or the album's own folder (null: none). */
    val cardPath: String?
        get() = when (this) {
            is Song -> Library.cardPath(track.path)
            is AlbumOf -> Library.folderOf(album.tracks)
            is ArtistOf -> null
        }
}

/**
 * Which list of the library is open: tab 0 artists, 1 albums, 2 songs, 3 playlists; then an
 * artist, then an album, or a playlist (its card path).
 */
data class LibraryNav(val tab: Int = 0, val artist: String? = null, val album: Pair<String, String>? = null, val playlist: String? = null)

/** The playlists of the card (the .m3u and .m3u8 files in Playlists) and what is being done with them. */
data class Playlists(
    /** The playlist files; null until read. */
    val files: List<String>? = null,
    val open: Playlist? = null,
    /** Songs waiting for the user to choose the playlist they go to. */
    val adding: LibTarget? = null,
    /** The name typed for a new playlist (null: no question open). */
    val newName: String? = null,
    val working: Boolean = false,
    val error: ErrorInfo? = null,
    /** The playlist songs were just added to, shown as a short confirmation. */
    val added: String? = null,
)

sealed interface UpdatePhase {
    data object Idle : UpdatePhase
    /** [reconnecting]: the connection dropped and this is the attempt to connect again (0: connected). */
    data class Sending(val done: Long, val total: Long, val path: String, val reconnecting: Int = 0) : UpdatePhase
    data class Sent(val files: Int, val skipped: Int) : UpdatePhase
    data class Failed(val error: ErrorInfo) : UpdatePhase
}

/**
 * A signed ByOS update chosen on the phone (docs/OTA.md): `hibyos-update.json` and the payload
 * tar it names. The signature is checked on the phone as soon as the JSON is picked, and again
 * by the player.
 */
data class OtaPick(
    /** Set when a valid, signed description was picked. */
    val version: String? = null,
    val payloadName: String? = null,
    val payloadSize: Long = 0,
    val needsFirmware: Boolean = false,
    /** "none", "shell" or "reboot", as the description announces. */
    val restart: String? = null,
    /** The tar picked on the phone (its own name) and its size. */
    val tarName: String? = null,
    val tarSize: Long = 0,
    val error: ErrorInfo? = null,
) {
    /** A tar was picked but it is not the payload the description names (size differs). */
    val tarMismatch: Boolean get() = version != null && tarName != null && tarSize != payloadSize
    val ready: Boolean get() = version != null && tarName != null && !tarMismatch
}

/** The install of a signed update over BTP/1 (sections 14.1 to 14.3). */
sealed interface OtaPhase {
    data object Idle : OtaPhase
    data class Sending(val done: Long, val total: Long, val path: String, val reconnecting: Int = 0) : OtaPhase

    /** UPDATE_STAGE: the player checks the signature and hashes the payload. */
    data object Checking : OtaPhase

    /** The player shows its confirmation alert. */
    data class WaitingUser(val version: String) : OtaPhase
    data class Installing(val version: String, val permille: Int) : OtaPhase

    /** restart: 0 none, 1 the ByOS interface restarts, 2 the player asks to restart. */
    data class Done(val version: String, val restart: Int) : OtaPhase

    /** The user tapped Cancel on the player. */
    data class Refused(val version: String) : OtaPhase

    /** Staged, but this ByOS Sync cannot start the install: Software Update on the player can. */
    data class StagedOnly(val version: String) : OtaPhase

    /** [downgrade]: the player refused an older or equal version; installing it anyway is offered. */
    data class Failed(val error: ErrorInfo, val downgrade: Boolean = false) : OtaPhase

    val running: Boolean get() = this is Sending || this is Checking || this is WaitingUser || this is Installing
}

data class SyncState(
    /** This build has a BTP/1 client. */
    val available: Boolean = false,
    val found: List<SyncDevice> = emptyList(),
    val paired: List<PairedDevice> = emptyList(),
    val link: Link = Link.Idle,
    val error: ErrorInfo? = null,
    val dest: Destination = Destination.AUTO,
    /** With the automatic destination: groups sent to another folder than their default one. */
    val routes: Map<Route, String> = emptyMap(),
    /** A folder chosen in the browser ("Send here"), instead of [dest]. */
    val destPath: String? = null,
    val replace: Boolean = false,
    val transfer: Transfer = Transfer(),
    /** Files shared from another app while a transfer runs: they join the list when it ends. */
    val sharedWaiting: Boolean = false,
    val browser: Browser = Browser(),
    val library: LibraryPhase = LibraryPhase.Idle,
    val libraryNav: LibraryNav = LibraryNav(),
    val update: UpdatePhase = UpdatePhase.Idle,
    val rescan: Rescan = Rescan.Idle,
    /** Waiting for the user to confirm the deletion of a song, an album or an artist. */
    val libDelete: LibTarget? = null,
    val libWorking: Boolean = false,
    val libError: ErrorInfo? = null,
    /** Files were moved, renamed or deleted on the card: the library is out of date until a rescan. */
    val libraryChanged: Boolean = false,
    val playlists: Playlists = Playlists(),
    /** The search over artists, albums and songs ("" shows the lists). */
    val libraryQuery: String = "",
    /** Album artwork read from the card: album folder -> image file in the app's cache (null: none found). */
    val covers: Map<String, String?> = emptyMap(),
    val otaPick: OtaPick = OtaPick(),
    val ota: OtaPhase = OtaPhase.Idle,
    /**
     * The open session lost its connection outside a transfer or an update (browsing, the
     * keep-alive) and connects again: the attempt (0: connected). The connection and file
     * screens show it with a Cancel.
     */
    val reconnecting: Int = 0,
    /** How many attempts the reconnection in progress makes (shown as "n of [reconnectOf]"). */
    val reconnectOf: Int = 0,
) {
    val connected: Link.Connected? get() = link as? Link.Connected
    val target: String get() = destPath ?: dest.path

    /** Work is running: leaving the screen is held back. */
    val busy: Boolean
        get() = link is Link.Searching || link is Link.Connecting || (link is Link.Pairing && link.waiting) ||
            transfer.running || library is LibraryPhase.Loading || update is UpdatePhase.Sending || browser.working ||
            ota.running || rescan == Rescan.Running || libWorking || browser.download?.running == true || playlists.working
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

    /** With the automatic destination: the card folder for one group ("" is the top of the card). */
    fun setRoute(route: Route, folder: String)

    /** Hands the ByOS update files of the choice to Install Update. */
    fun useUpdateFiles()
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

    /** Asks the player to update its library, then reads it again. */
    fun rescanLibrary()

    /** Asks to confirm the deletion of [target] from the card (null closes the question). */
    fun libraryDelete(target: LibTarget?)
    fun confirmLibraryDelete()

    /** Opens Files at the song or the album folder, ready to move it. */
    fun libraryMove(target: LibTarget)

    /** Opens Files at the folder of the song or the album. */
    fun revealInFiles(target: LibTarget)

    fun searchLibrary(text: String)

    fun loadPlaylists()
    fun openPlaylist(file: String)

    /** Asks which playlist [target]'s songs go to (null closes the question). */
    fun addToPlaylist(target: LibTarget?)
    fun addToPlaylistFile(file: String)

    /** The name of a new playlist being typed (null closes the question). */
    fun newPlaylistName(text: String?)
    fun createPlaylist()
    fun removeFromPlaylist(index: Int)
    fun deletePlaylist(file: String)

    /** Fetches the artwork of the album in this card folder (once; the result lands in [SyncState.covers]). */
    fun loadCover(folder: String)

    /** Opens the system's "save as" for a card file of the browser, then copies it there. */
    fun saveToPhone(entry: SyncEntry)
    fun clearDownload()

    fun sendByosUpdate()

    /** Signed update: choose hibyos-update.json and its tar (one or both at a time). */
    fun pickUpdateFiles()
    fun clearUpdateFiles()

    /** Sends the picked update, stages it and starts the install; [allowDowngrade] after E_DOWNGRADE. */
    fun installUpdate(allowDowngrade: Boolean)
    fun cancelWork()
}
