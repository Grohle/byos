package org.hibyos.install.connected

import java.io.Closeable
import java.io.InputStream
import java.io.OutputStream

/*
 * The small surface of the BTP/1 client (docs/SYNC_PROTOCOL.md) that the "ByOS connected" screens
 * use. The real client lives in the shared core; SyncBackend binds this interface to it. Keeping
 * the screens on this interface lets them be tested with an in-memory device.
 *
 * Every call blocks and runs on the app's worker thread. Failures are SyncException.
 */

/** Namespaces of section 9.1. */
object Ns {
    const val SD = 0
    const val UPDATE = 1
    const val DB = 2
}

/** PUT_BEGIN.mode (section 11.1). */
enum class PutMode(val code: Int) { CREATE(0), REPLACE(1), SKIP_IDENTICAL(2) }

/** A player that answered the UDP probe (section 16). */
data class SyncDevice(
    val host: String,
    val port: Int,
    /** 16 hex digits: the first 8 bytes of SHA-256(Ds). */
    val deviceId: String,
    val name: String,
    val pairingOpen: Boolean,
    val sdMounted: Boolean,
    /** This phone paired with it before. */
    val paired: Boolean,
)

/** A device this phone paired with, as the client stored it. */
data class PairedDevice(val deviceId: String, val name: String, val lastHost: String?)

/** What the device said in its HELLO and STATUS (sections 7 and 15). */
data class SyncDeviceInfo(
    val deviceId: String,
    val name: String,
    val byosVersion: String?,
    val firmware: String?,
    val local: Boolean,
    val sdMounted: Boolean,
    val shellAttached: Boolean,
    val caps: Int,
) {
    fun can(cap: Int): Boolean = caps and cap != 0

    companion object {
        const val CAP_FILES = 1
        const val CAP_RESUME = 2
        const val CAP_DB_SNAPSHOT = 4
        const val CAP_DB_STAGE = 8
        const val CAP_LIB_RESCAN = 16
        const val CAP_UPDATE = 32
        const val CAP_PAIRING = 64
        const val CAP_ENCRYPTION = 128
        const val CAP_STATUS = 256
    }
}

/** DF_R (section 10). sdState: 0 mounted, 1 no card, 2 exported over USB, 3 read-only. */
data class SyncSpace(val sdState: Int, val sdFs: Int, val sdTotal: Long, val sdFree: Long, val dataTotal: Long, val dataFree: Long) {
    /** FAT32 keeps files under 4 GiB. */
    val maxFile: Long get() = if (sdFs == 1) 0xFFFFFFFFL else Long.MAX_VALUE
}

/** A LIST_R entry or a STAT_R answer. kind: 0 file, 1 folder, 2 other. */
data class SyncEntry(val name: String, val kind: Int, val size: Long, val mtime: Long) {
    val isFolder: Boolean get() = kind == 1
}

/** PUT_DONE.result: 0 written, 1 skipped (identical). */
data class PutResult(val skipped: Boolean, val size: Long, val replaced: Boolean = false)

/** The end of a GET: the file's size and the SHA-256 of all of it. */
class GetResult(val size: Long, val sha256: ByteArray)

/** UPDATE_STAGE_R (section 14.2). restart: 0 none, 1 shell, 2 reboot. */
data class StagedUpdate(val version: String, val restart: Int, val needsFirmware: Boolean, val size: Long)

/** UPDATE_STATUS_R (section 14.3). */
data class UpdateProgress(val state: Int, val permille: Int, val error: Int, val version: String) {
    companion object {
        const val IDLE = 0
        const val STAGED = 1
        const val WAITING_USER = 2
        const val INSTALLING = 3
        const val DONE = 4
        const val FAILED = 5
        const val REFUSED = 6
    }
}

/** Finds players and opens sessions. */
interface SyncConnector {
    /** UDP probe and announces (section 16). Calls [onFound] as answers arrive; returns them all. */
    fun discover(timeoutMs: Int, cancelled: () -> Boolean, onFound: (SyncDevice) -> Unit): List<SyncDevice>

    /** The devices this phone paired with. */
    fun paired(): List<PairedDevice>

    /** Forgets the keys of a paired device on this phone. */
    fun forget(deviceId: String)

    /**
     * HELLO and AUTH (section 8.3) with a paired device. [expectedId] is the device the user chose
     * (null or empty for a typed address): another DEVICE_ID in HELLO throws IDENTITY_CHANGED.
     * Throws NOT_PAIRED when the phone has no keys for the device, FORGOTTEN when the device
     * refused AUTH (the keys are kept until a new pairing or an explicit forget).
     * For a paired device the call is patient: a player that is not at [host] any more is looked
     * for by its id, and one whose sync service is restarting is waited for (about 20 s) before
     * UNREACHABLE. [host] may be empty when no address was ever stored. [cancelled] stops it.
     */
    fun connect(host: String, port: Int, expectedId: String? = null, cancelled: () -> Boolean = { false }): SyncSession

    /**
     * HELLO, PAIR_COMMIT, PAIR_DEVICE and PAIR_REVEAL (section 8.2). Returns when the player shows
     * its 6-digit code.
     */
    fun startPairing(host: String, port: Int, clientName: String): Pairing
}

/** A pairing waiting for the code the user reads on the player. */
interface Pairing : Closeable {
    /**
     * Compares [code] with the client's own SAS (constant time). A different code throws
     * WRONG_CODE and the handshake is closed (BYE). A matching code sends PAIR_CONFIRM and waits
     * until the user taps Allow on the player (at most 120 s).
     */
    fun confirm(code: String, cancelled: () -> Boolean): SyncSession
}

/**
 * An open, secured session. A session of the real client comes back by itself when the
 * connection drops (hibysync restarted, Wi-Fi blinked, the phone slept): requests that can run
 * twice are repeated on the new session, the others throw RECONNECTED.
 */
interface SyncSession : Closeable {
    val info: SyncDeviceInfo

    /** Where the player was last reached (it may change when the session comes back). */
    val host: String? get() = null

    /**
     * [listener] is told while the session connects again: the attempt (from 1) and the number
     * of attempts, then 0 once it is back or has given up.
     */
    fun onReconnect(listener: (attempt: Int, of: Int) -> Unit) {}

    /**
     * While a request connects again, [cancelled] is asked as well as the request's own cancel
     * (requests without one, such as browsing and PING, have only this one): true stops the
     * reconnection and the request ends with E_CANCELLED.
     */
    fun cancelWhen(cancelled: () -> Boolean) {}

    fun df(): SyncSpace
    fun list(path: String): List<SyncEntry>

    /** STAT; null on E_NOT_FOUND. */
    fun stat(path: String): SyncEntry?
    fun mkdir(path: String, parents: Boolean)
    fun move(src: String, dst: String, replace: Boolean)
    fun delete(path: String, recursive: Boolean)

    /**
     * PUT (section 11). [open] gives the content from a byte offset: the device chooses where to
     * resume. [progress] receives the bytes of this file that are on the device.
     */
    fun put(
        ns: Int,
        path: String,
        size: Long,
        sha256: ByteArray,
        mtime: Long,
        mode: PutMode,
        open: (offset: Long) -> InputStream,
        cancelled: () -> Boolean,
        progress: (done: Long) -> Unit,
    ): PutResult

    /**
     * GET (section 12) of a card file into [out], from [offset] (the bytes before it are already
     * on the phone). Returns the file's size and the SHA-256 of the whole file (GET_END).
     */
    fun get(path: String, offset: Long, out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): GetResult

    /** DB_SNAPSHOT (section 13.1) into [out]. Returns the revision (GET_END.sha256). */
    fun dbSnapshot(out: OutputStream, cancelled: () -> Boolean, progress: (done: Long, total: Long) -> Unit): ByteArray

    /**
     * LIB_RESCAN (section 13.3). [paths] null: a full rescan by the player (mode 0, needs the
     * ByOS screen; E_UNSUPPORTED where the shell cannot start one). Otherwise 1 to 256 card
     * paths whose rows are refreshed (tags, cover, lyrics); files not in the library are skipped.
     */
    fun libRescan(paths: List<String>?)

    fun updateStage(allowDowngrade: Boolean): StagedUpdate
    fun updateInstall(allowDowngrade: Boolean)
    fun updateStatus(): UpdateProgress

    /** PING/PONG (section 17): keeps an idle session open. */
    fun ping()
}

/** A BTP/1 error code (section 6), or one of the client's own (negative) codes. */
class SyncException(val code: Int, message: String? = null, cause: Throwable? = null) : Exception(message, cause) {
    companion object {
        // client side
        const val UNREACHABLE = -1
        const val WRONG_CODE = -2
        const val NOT_PAIRED = -3
        const val IDENTITY_CHANGED = -4
        const val CONNECTION_LOST = -5
        const val NO_CLIENT = -6
        /** AUTH was refused: the player does not know this phone (any more). */
        const val FORGOTTEN = -7
        /**
         * The connection dropped during a request that must not run twice (move, delete, an
         * update step); a new session is already open, so the user can simply try again.
         */
        const val RECONNECTED = -8

        // device side (section 6)
        const val E_PROTO = 1
        const val E_VERSION = 2
        const val E_AUTH_FAILED = 5
        const val E_PAIR_CLOSED = 6
        const val E_PAIR_REJECTED = 7
        const val E_BUSY = 8
        const val E_RATE_LIMIT = 10
        const val E_TIMEOUT = 11
        const val E_CANCELLED = 12
        const val E_UNSUPPORTED = 13
        const val E_BAD_PATH = 32
        const val E_NOT_FOUND = 33
        const val E_EXISTS = 34
        const val E_NOT_DIR = 35
        const val E_IS_DIR = 36
        const val E_NOT_EMPTY = 37
        const val E_NO_SPACE = 38
        const val E_IO = 39
        const val E_SD_UNAVAILABLE = 40
        const val E_HASH_MISMATCH = 41
        const val E_FILE_TOO_BIG = 43
        const val E_NOT_ALLOWED = 44
        const val E_SIGNATURE = 64
        const val E_UPDATE_INVALID = 65
        const val E_DOWNGRADE = 66
        const val E_BATTERY = 67
        const val E_NEEDS_FIRMWARE = 68
        const val E_NOT_STAGED = 69
    }

    /** The connection is gone: the next request needs a new session. */
    val fatal: Boolean
        get() = code == UNREACHABLE || code == CONNECTION_LOST || code == IDENTITY_CHANGED || code == E_PROTO ||
            code == E_VERSION || code == E_AUTH_FAILED || code == E_TIMEOUT
}
