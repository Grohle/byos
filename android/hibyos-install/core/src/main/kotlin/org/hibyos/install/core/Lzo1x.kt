package org.hibyos.install.core

/**
 * LZO1X decompressor and LZO1X-1 style compressor.
 *
 * This is a line-by-line port of tools/lzo1x.py of the HiByOS repository. The compressor
 * must produce exactly the same bytes as the Python one (the firmware built on the phone is
 * compared byte for byte with the one built by the desktop patcher), so its parsing
 * decisions must not be "improved" here without changing the Python side as well.
 */
class LzoException(message: String) : IllegalArgumentException(message)

object Lzo1x {
    private const val M2_MAX_LEN = 8
    private const val M2_MAX_OFFSET = 0x0800
    private const val M3_MAX_LEN = 33
    private const val M3_MAX_OFFSET = 0x4000
    private const val M4_MAX_LEN = 9
    private const val M4_MAX_OFFSET = 0xBFFF

    /**
     * Decompresses one LZO1X stream of [srcLen] bytes found at [srcOff] in [src] into [dst]
     * (starting at index 0) and returns the number of bytes written. Fails on any malformed
     * input, if the output would exceed [maxOut] bytes or if the stream does not end exactly
     * with the end-of-stream marker.
     */
    fun decompress(src: ByteArray, srcOff: Int, srcLen: Int, dst: ByteArray, maxOut: Int): Int {
        val n = srcOff + srcLen
        var ip = srcOff
        var op = 0
        if (srcLen < 3) throw LzoException("input too short")
        if (maxOut > dst.size) throw LzoException("output buffer too small")

        fun lit(count: Int) {
            if (ip + count > n) throw LzoException("input overrun (literals)")
            if (op + count > maxOut) throw LzoException("output overrun (literals)")
            System.arraycopy(src, ip, dst, op, count)
            ip += count
            op += count
        }

        fun extLen(base: Int): Int {
            var zeros = 0
            while (true) {
                if (ip >= n) throw LzoException("input overrun (length)")
                val b = src[ip].toInt() and 0xFF
                ip++
                if (b != 0) return base + zeros * 255 + b
                zeros++
                if (zeros > 1 shl 20) throw LzoException("length too long")
            }
        }

        fun copyMatch(dist: Int, length: Int) {
            val start = op - dist
            if (dist <= 0 || start < 0) throw LzoException("lookbehind overrun")
            if (op + length > maxOut) throw LzoException("output overrun (match)")
            if (dist >= length) {
                System.arraycopy(dst, start, dst, op, length)
                op += length
            } else {
                var s = start
                var i = 0
                while (i < length) {
                    dst[op++] = dst[s++]
                    i++
                }
            }
        }

        var state = 0
        var first = true
        while (true) {
            if (ip >= n) throw LzoException("input overrun (no EOF marker)")
            var t = src[ip].toInt() and 0xFF
            ip++
            if (first) {
                first = false
                if (t > 17) {
                    t -= 17
                    lit(t)
                    state = if (t < 4) t else 4
                    continue
                }
            }
            val nxt: Int
            if (t < 16) {
                if (state == 0) {
                    if (t == 0) t = extLen(15)
                    lit(t + 3)
                    state = 4
                    continue
                }
                if (ip >= n) throw LzoException("input overrun")
                if (state != 4) {
                    nxt = t and 3
                    val dist = 1 + (t shr 2) + ((src[ip].toInt() and 0xFF) shl 2)
                    ip++
                    copyMatch(dist, 2)
                } else {
                    nxt = t and 3
                    val dist = 1 + 0x800 + (t shr 2) + ((src[ip].toInt() and 0xFF) shl 2)
                    ip++
                    copyMatch(dist, 3)
                }
            } else if (t >= 64) {
                if (ip >= n) throw LzoException("input overrun")
                nxt = t and 3
                val dist = 1 + ((t shr 2) and 7) + ((src[ip].toInt() and 0xFF) shl 3)
                ip++
                copyMatch(dist, (t shr 5) + 1)
            } else if (t >= 32) {
                var length = (t and 31) + 2
                if (length == 2) length = extLen(33)
                if (ip + 2 > n) throw LzoException("input overrun")
                val v = (src[ip].toInt() and 0xFF) or ((src[ip + 1].toInt() and 0xFF) shl 8)
                ip += 2
                nxt = v and 3
                copyMatch(1 + (v shr 2), length)
            } else {
                var dist = (t and 8) shl 11
                var length = (t and 7) + 2
                if (length == 2) length = extLen(9)
                if (ip + 2 > n) throw LzoException("input overrun")
                val v = (src[ip].toInt() and 0xFF) or ((src[ip + 1].toInt() and 0xFF) shl 8)
                ip += 2
                nxt = v and 3
                dist += v shr 2
                if (dist == 0) {
                    if (length != 3) throw LzoException("bad EOF marker")
                    if (ip != n) throw LzoException("trailing data after EOF marker")
                    return op
                }
                copyMatch(dist + 0x4000, length)
            }
            state = nxt
            if (nxt != 0) lit(nxt)
        }
    }

    /** Convenience wrapper returning a new array. */
    fun decompress(src: ByteArray, maxOut: Int): ByteArray {
        val dst = ByteArray(maxOut)
        val n = decompress(src, 0, src.size, dst, maxOut)
        return if (n == maxOut) dst else dst.copyOf(n)
    }

    /** Growable output buffer (ByteArrayOutputStream without the synchronisation). */
    private class Out(capacity: Int) {
        var buf = ByteArray(capacity)
        var size = 0
        fun put(b: Int) {
            if (size == buf.size) buf = buf.copyOf(buf.size * 2)
            buf[size++] = b.toByte()
        }

        fun put(src: ByteArray, off: Int, len: Int) {
            if (size + len > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, size + len))
            System.arraycopy(src, off, buf, size, len)
            size += len
        }
    }

    /**
     * Compresses [len] bytes of [src] starting at [off] into a standard LZO1X stream
     * (greedy LZO1X-1 style parser, exact 4-byte dictionary keeping the latest position).
     */
    fun compress(src: ByteArray, off: Int = 0, len: Int = src.size - off): ByteArray {
        val n = len
        val out = Out(n + n / 16 + 64 + 3)
        var ii = 0 // first pending (not yet emitted) literal
        if (n > M2_MAX_LEN + 5) {
            // exact map "4 bytes -> latest position", open addressing
            var cap = 1024
            while (cap < n * 2) cap = cap shl 1
            val mask = cap - 1
            val keys = IntArray(cap)
            val vals = IntArray(cap) // position + 1, 0 = empty slot
            var ip = 4 // >= 4 literals before the first match
            val ipEnd = n - M2_MAX_LEN - 5
            while (ip < ipEnd) {
                val p = off + ip
                val key = (src[p].toInt() and 0xFF) or ((src[p + 1].toInt() and 0xFF) shl 8) or
                    ((src[p + 2].toInt() and 0xFF) shl 16) or ((src[p + 3].toInt() and 0xFF) shl 24)
                var h = (key * -0x61c88647) ushr 7 and mask
                var cand = -1
                while (true) {
                    val v = vals[h]
                    if (v == 0) {
                        keys[h] = key
                        vals[h] = ip + 1
                        break
                    }
                    if (keys[h] == key) {
                        cand = v - 1
                        vals[h] = ip + 1
                        break
                    }
                    h = (h + 1) and mask
                }
                if (cand < 0 || ip - cand > M4_MAX_OFFSET) {
                    ip += 1 + minOf((ip - ii) shr 5, 63)
                    continue
                }
                // extend the match (it may overlap the current position; that is valid)
                var mLen = 4
                val maxl = n - ip
                while (mLen < maxl && src[off + cand + mLen] == src[p + mLen]) mLen++
                // pending literals
                val t = ip - ii
                if (t != 0) {
                    if (t <= 3) {
                        out.buf[out.size - 2] = (out.buf[out.size - 2].toInt() or t).toByte()
                    } else if (t <= 18) {
                        out.put(t - 3)
                    } else {
                        var tt = t - 18
                        out.put(0)
                        while (tt > 255) {
                            tt -= 255
                            out.put(0)
                        }
                        out.put(tt)
                    }
                    out.put(src, off + ii, t)
                }
                var mOff = ip - cand
                ip += mLen
                ii = ip
                if (mLen <= M2_MAX_LEN && mOff <= M2_MAX_OFFSET) {
                    mOff -= 1
                    out.put(((mLen - 1) shl 5) or ((mOff and 7) shl 2))
                    out.put(mOff shr 3)
                } else if (mOff <= M3_MAX_OFFSET) {
                    mOff -= 1
                    if (mLen <= M3_MAX_LEN) {
                        out.put(32 or (mLen - 2))
                    } else {
                        mLen -= M3_MAX_LEN
                        out.put(32)
                        while (mLen > 255) {
                            mLen -= 255
                            out.put(0)
                        }
                        out.put(mLen)
                    }
                    out.put((mOff shl 2) and 0xFF)
                    out.put(mOff shr 6)
                } else {
                    mOff -= 0x4000
                    if (mLen <= M4_MAX_LEN) {
                        out.put(16 or ((mOff shr 11) and 8) or (mLen - 2))
                    } else {
                        mLen -= M4_MAX_LEN
                        out.put(16 or ((mOff shr 11) and 8))
                        while (mLen > 255) {
                            mLen -= 255
                            out.put(0)
                        }
                        out.put(mLen)
                    }
                    out.put((mOff shl 2) and 0xFF)
                    out.put((mOff shr 6) and 0xFF)
                }
            }
        }
        // trailing literals
        val t = n - ii
        if (t != 0) {
            if (out.size == 0 && t <= 238) {
                out.put(17 + t)
            } else if (t <= 3) {
                out.buf[out.size - 2] = (out.buf[out.size - 2].toInt() or t).toByte()
            } else if (t <= 18) {
                out.put(t - 3)
            } else {
                var tt = t - 18
                out.put(0)
                while (tt > 255) {
                    tt -= 255
                    out.put(0)
                }
                out.put(tt)
            }
            out.put(src, off + ii, t)
        }
        out.put(0x11)
        out.put(0)
        out.put(0)
        return out.buf.copyOf(out.size)
    }
}
