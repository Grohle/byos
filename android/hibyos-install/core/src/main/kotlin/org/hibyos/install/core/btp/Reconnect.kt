package org.hibyos.install.core.btp

import java.io.Closeable
import java.io.IOException

/** An authenticated session with a paired device, and where it was reached. */
class Reached(val session: BtpSession, val host: String, val port: Int, val attempts: Int)

/**
 * Opens an authenticated session with a device this client paired with, the way "tap the
 * remembered player" should behave on a real network:
 *
 * - the shell stops and restarts hibysync (low memory, Wi-Fi option changed, shell restart):
 *   for a few seconds nothing listens, or the connection is reset. Tried again with growing
 *   waits ([delaysMs]);
 * - the player got another address from DHCP: [locate] looks for it by device id (UDP
 *   discovery, and the caller's fallback) and the session is opened at the new address;
 * - the device is busy or rate limits for a moment (`E_BUSY`, `E_RATE_LIMIT`, `E_TIMEOUT`):
 *   tried again;
 * - the device closed the connection right after AUTH_FINISH (a daemon stopping at that moment
 *   looks the same as a device that cannot open the record): tried once more, and only a second
 *   time in a row is it reported as a changed identity.
 *
 * Only what retrying cannot change ends it at once: the device does not know this client
 * ([BtpAuthException.Reason.UNKNOWN_CLIENT], the "pair again" case), AUTH_OK that does not
 * open (another device or a reset player), an unsupported device, or cancellation.
 */
class BtpReconnector(
    /** TCP connect and HELLO; throws as [BtpSession.connect]. */
    private val dial: (host: String, port: Int) -> BtpSession,
    /** Looks for the device on the network; [attempt] counts from 0. Null when it did not answer. */
    private val locate: (deviceIdHex: String, attempt: Int) -> Found? = { _, _ -> null },
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    val delaysMs: LongArray = DEFAULT_DELAYS_MS,
) {
    private sealed class Try {
        class Ok(val session: BtpSession) : Try()
        class Again(val error: Exception, val look: Boolean) : Try()
    }

    private var closedEarly = 0

    /**
     * Opens the session. [lastHost] may be null (never reached over the network yet): the device
     * is then only looked for. [onRetry] is told before each wait (attempt from 1, the error).
     */
    fun open(paired: PairedDevice, clientSecret: ByteArray, lastHost: String?, lastPort: Int = Btp.PORT,
             cancelled: () -> Boolean = { false }, onRetry: (attempt: Int, error: Exception) -> Unit = { _, _ -> }): Reached {
        val idHex = paired.deviceId.joinToString("") { "%02x".format(it) }
        var host = lastHost
        var port = lastPort
        var last: Exception? = null
        closedEarly = 0
        for (attempt in delaysMs.indices) {
            if (attempt > 0) {
                onRetry(attempt, last ?: BtpConnectionException("the device did not answer"))
                pause(delaysMs[attempt], cancelled)
            }
            if (cancelled()) throw BtpException(Btp.E_CANCELLED, "cancelled")
            var look = host == null
            val h = host
            if (h != null) {
                when (val r = once(h, port, paired, clientSecret)) {
                    is Try.Ok -> return Reached(r.session, h, port, attempt + 1)
                    is Try.Again -> {
                        last = r.error
                        look = r.look
                    }
                }
            }
            if (!look || cancelled()) continue
            val f = try {
                locate(idHex, attempt)
            } catch (e: IOException) {
                null
            }
            if (f == null || !f.deviceIdHex.equals(idHex, ignoreCase = true)) {
                if (last == null) last = BtpConnectionException("the device was not found on the network")
                continue
            }
            if (f.address == h && f.port == port) continue // the same place: wait and try again
            host = f.address
            port = f.port
            when (val r = once(f.address, f.port, paired, clientSecret)) {
                is Try.Ok -> return Reached(r.session, f.address, f.port, attempt + 1)
                is Try.Again -> last = r.error
            }
        }
        throw last ?: BtpConnectionException("the device did not answer")
    }

    private fun pause(ms: Long, cancelled: () -> Boolean) {
        var left = ms
        while (left > 0) {
            if (cancelled()) throw BtpException(Btp.E_CANCELLED, "cancelled")
            val step = minOf(left, 200L)
            sleep(step)
            left -= step
        }
    }

    /** One connection: HELLO, the device id check, AUTH. Throws what retrying cannot change. */
    private fun once(host: String, port: Int, paired: PairedDevice, secret: ByteArray): Try {
        val s = try {
            dial(host, port)
        } catch (e: BtpException) {
            if (e.remote && e.code in TRANSIENT) return Try.Again(e, look = false)
            if (e.remote && e.code == Btp.E_VERSION) throw e
            // something else answers at that address (not hibysync, or garbage): look elsewhere
            return Try.Again(e, look = true)
        } catch (e: IOException) {
            // nothing listens (daemon restarting, or the address moved), reset, timeout
            return Try.Again(e, look = true)
        }
        try {
            val id = s.device.deviceId
            if (id == null || !id.contentEquals(paired.deviceId)) {
                // another device has the address now: the paired one is somewhere else
                s.close()
                return Try.Again(BtpAuthException(BtpAuthException.Reason.DIFFERENT_DEVICE,
                    "another device answers at $host; looking for the paired one"), look = true)
            }
            s.auth(secret, paired)
            closedEarly = 0
            return Try.Ok(s)
        } catch (e: BtpAuthException) {
            s.close()
            if (e.reason == BtpAuthException.Reason.IDENTITY_CHANGED && e.closedEarly && ++closedEarly < 2) {
                return Try.Again(e, look = false)
            }
            throw e
        } catch (e: BtpException) {
            s.close()
            if (e.remote && e.code in TRANSIENT) return Try.Again(e, look = false)
            throw e
        } catch (e: IOException) {
            s.close()
            return Try.Again(e, look = false)
        } catch (e: RuntimeException) {
            s.close()
            throw e
        }
    }

    companion object {
        /** About 20 s in all: long enough for the shell to start hibysync again. */
        val DEFAULT_DELAYS_MS = longArrayOf(0, 1_000, 2_000, 3_000, 5_000, 8_000)

        /** Device errors that a later attempt can get past. */
        val TRANSIENT = setOf(Btp.E_BUSY, Btp.E_RATE_LIMIT, Btp.E_TIMEOUT)

        /** The error means the connection is gone (and a new session may carry on). */
        fun isConnectionLoss(e: Throwable): Boolean = when (e) {
            is BtpConnectionException -> true
            is java.net.SocketTimeoutException -> true
            is java.net.SocketException -> true
            // the device's idle timeout or shutdown arrives as an ERROR on stream 0, then it closes
            is BtpException -> e.remote && e.code == Btp.E_TIMEOUT
            else -> false
        }
    }
}

/** A request was not repeated after the connection dropped; a new session is open, so it can be tried again. */
class BtpReconnectedException(cause: Exception) :
    Exception("the connection dropped and was opened again; try again", cause)

/**
 * A session with a paired device that comes back by itself: when a request finds the
 * connection gone (hibysync restarted, Wi-Fi blinked, the phone slept), [call] opens a new one
 * with [reconnector] (AUTH with the stored keys, no code) and, for requests that can safely run
 * twice, runs the request again. Requests that must not run twice get [BtpReconnectedException]
 * once the new session is open.
 *
 * Not thread-safe: one request at a time, as [BtpSession].
 */
class BtpLink(
    private val reconnector: BtpReconnector,
    private val paired: PairedDevice,
    private val clientSecret: ByteArray,
    first: Reached,
    /** Told when the device was reached at another address (to store it). */
    private val onMoved: (host: String, port: Int) -> Unit = { _, _ -> },
) : Closeable {
    @Volatile var session: BtpSession = first.session
        private set
    var host: String = first.host
        private set
    var port: Int = first.port
        private set

    /** New sessions opened after the first one. */
    var reconnects = 0
        private set

    @Volatile private var closed = false

    /**
     * Runs [f] on the session. On a lost connection: a new session, then [f] again when [retry]
     * (it gets the new session), otherwise [BtpReconnectedException].
     */
    fun <T> call(retry: Boolean, cancelled: () -> Boolean = { false }, onRetry: (Int, Exception) -> Unit = { _, _ -> },
                 f: (BtpSession) -> T): T {
        val s = session
        try {
            return f(s)
        } catch (e: Exception) {
            if (closed || !BtpReconnector.isConnectionLoss(e)) throw e
            reopen(cancelled, onRetry, e)
            if (!retry) throw BtpReconnectedException(e)
        }
        return f(session)
    }

    /** Replaces the session with a new one (throws the reconnector's error, or [cause] when cancelled before any attempt). */
    fun reopen(cancelled: () -> Boolean = { false }, onRetry: (Int, Exception) -> Unit = { _, _ -> }, cause: Exception? = null) {
        try {
            session.close()
        } catch (e: Exception) {
            // already gone
        }
        if (closed) throw cause ?: BtpConnectionException("closed")
        val r = reconnector.open(paired, clientSecret, host, port, cancelled, onRetry)
        if (closed) {
            r.session.close()
            throw cause ?: BtpConnectionException("closed")
        }
        val moved = r.host != host || r.port != port
        session = r.session
        host = r.host
        port = r.port
        reconnects++
        if (moved) onMoved(r.host, r.port)
    }

    override fun close() {
        closed = true
        session.close()
    }
}
