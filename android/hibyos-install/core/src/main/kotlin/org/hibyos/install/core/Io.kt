package org.hibyos.install.core

import java.io.Closeable
import java.io.EOFException
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest

/** Random access, read-only bytes (a file on disk or an array in tests). */
interface ByteSource : Closeable {
    val size: Long

    /** Reads exactly [len] bytes at [pos] into [dst] at [off]; fails if they are not there. */
    fun read(pos: Long, dst: ByteArray, off: Int, len: Int)

    fun bytes(pos: Long, len: Int): ByteArray {
        val b = ByteArray(len)
        read(pos, b, 0, len)
        return b
    }

    override fun close() {}
}

class ArraySource(private val data: ByteArray) : ByteSource {
    override val size: Long get() = data.size.toLong()
    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int) {
        if (pos < 0 || len < 0 || pos + len > data.size) throw EOFException("read past the end of the data")
        System.arraycopy(data, pos.toInt(), dst, off, len)
    }
}

class FileSource(file: File) : ByteSource {
    private val raf = RandomAccessFile(file, "r")
    override val size: Long = raf.length()
    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int) {
        if (pos < 0 || len < 0 || pos + len > size) throw EOFException("read past the end of the file")
        raf.seek(pos)
        raf.readFully(dst, off, len)
    }

    override fun close() = raf.close()
}

/** A window of another source. */
class SliceSource(private val base: ByteSource, private val start: Long, override val size: Long) : ByteSource {
    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int) {
        if (pos < 0 || len < 0 || pos + len > size) throw EOFException("read past the end of the slice")
        base.read(start + pos, dst, off, len)
    }
}

internal fun ByteSource.copyTo(out: OutputStream, pos: Long, len: Long, buf: ByteArray, md: MessageDigest? = null) {
    var p = pos
    var left = len
    while (left > 0) {
        val n = minOf(left, buf.size.toLong()).toInt()
        read(p, buf, 0, n)
        out.write(buf, 0, n)
        md?.update(buf, 0, n)
        p += n
        left -= n
    }
}

internal fun InputStream.readFullyInto(buf: ByteArray, off: Int, len: Int): Int {
    var done = 0
    while (done < len) {
        val n = read(buf, off + done, len - done)
        if (n < 0) break
        done += n
    }
    return done
}

internal object Hex {
    private const val DIGITS = "0123456789abcdef"
    fun of(b: ByteArray): String {
        val sb = StringBuilder(b.size * 2)
        for (x in b) {
            sb.append(DIGITS[(x.toInt() shr 4) and 15]).append(DIGITS[x.toInt() and 15])
        }
        return sb.toString()
    }

    fun parse(s: String): ByteArray {
        require(s.length % 2 == 0) { "odd hex string" }
        return ByteArray(s.length / 2) {
            val hi = Character.digit(s[it * 2], 16)
            val lo = Character.digit(s[it * 2 + 1], 16)
            require(hi >= 0 && lo >= 0) { "bad hex string" }
            ((hi shl 4) or lo).toByte()
        }
    }
}

internal fun md5() = MessageDigest.getInstance("MD5")
internal fun sha256() = MessageDigest.getInstance("SHA-256")
internal fun md5Hex(b: ByteArray, off: Int = 0, len: Int = b.size - off): String =
    Hex.of(md5().apply { update(b, off, len) }.digest())

internal fun sha256Hex(b: ByteArray): String = Hex.of(sha256().digest(b))

/** Little-endian accessors on byte arrays. */
internal fun ByteArray.u8(p: Int) = this[p].toInt() and 0xFF
internal fun ByteArray.u16(p: Int) = u8(p) or (u8(p + 1) shl 8)
internal fun ByteArray.s16(p: Int) = u16(p).toShort().toInt()
internal fun ByteArray.u32(p: Int): Long = (u16(p).toLong() or (u16(p + 2).toLong() shl 16)) and 0xFFFFFFFFL
internal fun ByteArray.i32(p: Int): Int = u16(p) or (u16(p + 2) shl 16)
internal fun ByteArray.u64(p: Int): Long = u32(p) or (u32(p + 4) shl 32)

/** Growable little-endian byte buffer. */
internal class ByteBuf(capacity: Int = 256) {
    var buf = ByteArray(capacity)
        private set
    var size = 0
        private set

    private fun need(n: Int) {
        if (size + n > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, size + n))
    }

    fun u8(v: Int): ByteBuf { need(1); buf[size++] = v.toByte(); return this }
    fun u16(v: Int): ByteBuf { need(2); buf[size++] = v.toByte(); buf[size++] = (v shr 8).toByte(); return this }
    fun u32(v: Long): ByteBuf {
        need(4)
        for (i in 0 until 4) buf[size++] = (v shr (8 * i)).toByte()
        return this
    }

    fun u64(v: Long): ByteBuf {
        need(8)
        for (i in 0 until 8) buf[size++] = (v ushr (8 * i)).toByte()
        return this
    }

    fun bytes(b: ByteArray, off: Int = 0, len: Int = b.size - off): ByteBuf {
        need(len)
        System.arraycopy(b, off, buf, size, len)
        size += len
        return this
    }

    fun be16(v: Int): ByteBuf { need(2); buf[size++] = (v shr 8).toByte(); buf[size++] = v.toByte(); return this }
    fun be32(v: Long): ByteBuf {
        need(4)
        for (i in 3 downTo 0) buf[size++] = (v shr (8 * i)).toByte()
        return this
    }

    /** ISO 9660 "both byte orders" fields. */
    fun both16(v: Int): ByteBuf = u16(v).be16(v)
    fun both32(v: Long): ByteBuf = u32(v).be32(v)

    fun fill(value: Int, n: Int): ByteBuf {
        need(n)
        java.util.Arrays.fill(buf, size, size + n, value.toByte())
        size += n
        return this
    }

    fun dropFirst(n: Int) {
        System.arraycopy(buf, n, buf, 0, size - n)
        size -= n
    }

    fun clear() { size = 0 }
    fun toByteArray(): ByteArray = buf.copyOf(size)
}
