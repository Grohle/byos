package org.hibyos.install.core.btp

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * Frames over a byte stream (section 3), in clear or as encrypted records once [secure] has
 * been called. Used by the client and by the test device, so [sendDir] says which nonce
 * direction this side writes with ([Btp.DIR_C2D] for a client).
 *
 * [send] is thread-safe (a CANCEL may be sent from another thread while a transfer runs);
 * [receive] must be called from one thread at a time.
 */
class FrameChannel(input: InputStream, output: OutputStream, private val sendDir: Int,
                   private val maxPayload: Int = Btp.MAX_PAYLOAD) {
    private val inp = if (input is BufferedInputStream) input else BufferedInputStream(input, 1 shl 17)
    private val out = BufferedOutputStream(output, 1 shl 17)
    private val recvDir = if (sendDir == Btp.DIR_C2D) Btp.DIR_D2C else Btp.DIR_C2D
    private var sendKey: ByteArray? = null
    private var recvKey: ByteArray? = null
    private var sendCounter = 0L
    private var recvCounter = 0L
    private val hdr = ByteArray(Btp.HEADER_LEN)
    private val lock = Any()

    val encrypted: Boolean get() = sendKey != null

    /** Switches both directions to encrypted records; counters start at 0. */
    fun secure(sendKey: ByteArray, recvKey: ByteArray) {
        synchronized(lock) {
            this.sendKey = sendKey.copyOf()
            this.recvKey = recvKey.copyOf()
            sendCounter = 0
            recvCounter = 0
        }
    }

    fun wipe() {
        synchronized(lock) {
            sendKey?.fill(0)
            recvKey?.fill(0)
        }
    }

    /** True when bytes are waiting to be read (used to notice an early ERROR during an upload). */
    fun hasInput(): Boolean = try {
        inp.available() > 0
    } catch (e: IOException) {
        true
    }

    fun send(f: Frame, flush: Boolean = true) {
        synchronized(lock) {
            try {
                val k = sendKey
                if (k == null) {
                    out.write(f.encode())
                } else {
                    if (sendCounter >= (1L shl 32) - 1) throw BtpConnectionException("record counter exhausted; reconnect")
                    val box = BtpCrypto.secretbox(f.encode(), nonce(sendDir, sendCounter++), k)
                    val n = box.size
                    out.write(byteArrayOf((n ushr 24).toByte(), (n ushr 16).toByte(), (n ushr 8).toByte(), n.toByte()))
                    out.write(box)
                }
                if (flush) out.flush()
            } catch (e: BtpConnectionException) {
                throw e
            } catch (e: IOException) {
                throw BtpConnectionException("connection lost: ${e.message}", e)
            }
        }
    }

    fun flush() {
        synchronized(lock) {
            try {
                out.flush()
            } catch (e: IOException) {
                throw BtpConnectionException("connection lost: ${e.message}", e)
            }
        }
    }

    private fun readFully(b: ByteArray, off: Int, len: Int) {
        var done = 0
        while (done < len) {
            val n = try {
                inp.read(b, off + done, len - done)
            } catch (e: java.net.SocketTimeoutException) {
                throw BtpConnectionException("timeout: the other side stopped answering", e)
            } catch (e: IOException) {
                throw BtpConnectionException("connection lost: ${e.message}", e)
            }
            if (n < 0) throw BtpConnectionException("connection closed", EOFException())
            done += n
        }
    }

    /**
     * Reads the next frame. A malformed header raises [BtpException] (clear sessions); any
     * problem with an encrypted record raises [BtpConnectionException]: the session is over.
     */
    fun receive(): Frame {
        val k = recvKey
        if (k == null) {
            readFully(hdr, 0, Btp.HEADER_LEN)
            val h = Frame.parseHeader(hdr, 0, maxPayload)
            val p = ByteArray(h.length)
            readFully(p, 0, h.length)
            return Frame(h.type, h.stream, p, h.flags)
        }
        val lb = ByteArray(4)
        readFully(lb, 0, 4)
        val n = ((lb[0].toLong() and 0xff) shl 24) or ((lb[1].toLong() and 0xff) shl 16) or
            ((lb[2].toLong() and 0xff) shl 8) or (lb[3].toLong() and 0xff)
        if (n < Btp.TAG_LEN + Btp.HEADER_LEN || n > Btp.TAG_LEN + Btp.HEADER_LEN + maxPayload) {
            throw BtpConnectionException("bad record length $n")
        }
        val box = ByteArray(n.toInt())
        readFully(box, 0, box.size)
        val inner = BtpCrypto.secretboxOpen(box, nonce(recvDir, recvCounter), k)
            ?: throw BtpRecordAuthException("a record failed authentication")
        recvCounter++
        return try {
            Frame.decode(inner)
        } catch (e: BtpException) {
            throw BtpConnectionException("malformed encrypted frame: ${e.message}")
        }
    }

    companion object {
        fun nonce(dir: Int, counter: Long): ByteArray {
            val n = ByteArray(24)
            n[0] = dir.toByte()
            for (i in 0 until 8) n[16 + i] = (counter ushr (8 * (7 - i))).toByte()
            return n
        }
    }
}
