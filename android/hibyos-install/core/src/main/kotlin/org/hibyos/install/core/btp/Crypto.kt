package org.hibyos.install.core.btp

import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * The primitives BTP/1 needs (docs/SYNC_PROTOCOL.md section 8): X25519 (RFC 7748),
 * XSalsa20-Poly1305 secretbox in the "tag first" form, HKDF-SHA256 (RFC 5869) and SHA-256.
 *
 * X25519 follows the public-domain TweetNaCl algorithm (16 limbs of 16 bits). Salsa20,
 * HSalsa20 and Poly1305 (26-bit limbs, the "donna" layout) are written from their public
 * specifications. Nothing here comes from a library: the core module has no dependencies, and
 * the Android versions this code runs on do not all have X25519.
 */
object BtpCrypto {
    private val rng = SecureRandom()

    fun random(n: Int): ByteArray = ByteArray(n).also { rng.nextBytes(it) }

    fun sha256(vararg parts: ByteArray): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        for (p in parts) md.update(p)
        return md.digest()
    }

    fun hmacSha256(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        // an empty HMAC key is valid (RFC 2104) but SecretKeySpec refuses it
        mac.init(SecretKeySpec(if (key.isEmpty()) ByteArray(64) else key, "HmacSHA256"))
        for (p in parts) mac.update(p)
        return mac.doFinal()
    }

    fun hkdfExtract(salt: ByteArray, ikm: ByteArray): ByteArray = hmacSha256(salt, ikm)

    fun hkdfExpand(prk: ByteArray, info: ByteArray, length: Int): ByteArray {
        require(length in 0..255 * 32)
        val out = ByteArray(length)
        var t = ByteArray(0)
        var pos = 0
        var i = 1
        while (pos < length) {
            t = hmacSha256(prk, t, info, byteArrayOf(i.toByte()))
            val n = minOf(t.size, length - pos)
            System.arraycopy(t, 0, out, pos, n)
            pos += n
            i++
        }
        return out
    }

    /** Constant-time comparison. */
    fun equal(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    // ------------------------------------------------------------------ X25519

    private fun gf() = LongArray(16)
    private val GF_121665 = LongArray(16).also { it[0] = 0xDB41; it[1] = 1 }

    private fun car25519(o: LongArray) {
        for (i in 0 until 16) {
            o[i] += 1L shl 16
            val c = o[i] shr 16
            if (i < 15) o[i + 1] += c - 1 else o[0] += 38 * (c - 1)
            o[i] -= c shl 16
        }
    }

    private fun sel25519(p: LongArray, q: LongArray, b: Int) {
        val c = (b - 1).toLong().inv()
        for (i in 0 until 16) {
            val t = c and (p[i] xor q[i])
            p[i] = p[i] xor t
            q[i] = q[i] xor t
        }
    }

    private fun pack25519(o: ByteArray, n: LongArray) {
        val m = gf()
        val t = n.copyOf()
        car25519(t); car25519(t); car25519(t)
        for (j in 0 until 2) {
            m[0] = t[0] - 0xffed
            for (i in 1 until 15) {
                m[i] = t[i] - 0xffff - ((m[i - 1] shr 16) and 1)
                m[i - 1] = m[i - 1] and 0xffff
            }
            m[15] = t[15] - 0x7fff - ((m[14] shr 16) and 1)
            val b = ((m[15] shr 16) and 1).toInt()
            m[14] = m[14] and 0xffff
            sel25519(t, m, 1 - b)
        }
        for (i in 0 until 16) {
            o[2 * i] = (t[i] and 0xff).toByte()
            o[2 * i + 1] = (t[i] shr 8).toByte()
        }
    }

    private fun unpack25519(o: LongArray, n: ByteArray) {
        for (i in 0 until 16) o[i] = (n[2 * i].toLong() and 0xff) + ((n[2 * i + 1].toLong() and 0xff) shl 8)
        o[15] = o[15] and 0x7fff
    }

    private fun fAdd(o: LongArray, a: LongArray, b: LongArray) { for (i in 0 until 16) o[i] = a[i] + b[i] }
    private fun fSub(o: LongArray, a: LongArray, b: LongArray) { for (i in 0 until 16) o[i] = a[i] - b[i] }

    private fun fMul(o: LongArray, a: LongArray, b: LongArray) {
        val t = LongArray(31)
        for (i in 0 until 16) for (j in 0 until 16) t[i + j] += a[i] * b[j]
        for (i in 0 until 15) t[i] += 38 * t[i + 16]
        for (i in 0 until 16) o[i] = t[i]
        car25519(o)
        car25519(o)
    }

    private fun fSq(o: LongArray, a: LongArray) = fMul(o, a, a)

    private fun inv25519(o: LongArray, i: LongArray) {
        val c = i.copyOf()
        for (a in 253 downTo 0) {
            fSq(c, c)
            if (a != 2 && a != 4) fMul(c, c, i)
        }
        System.arraycopy(c, 0, o, 0, 16)
    }

    /** X25519(scalar, u) as in RFC 7748 (the scalar is clamped here). */
    fun x25519(scalar: ByteArray, u: ByteArray): ByteArray {
        require(scalar.size == 32 && u.size == 32)
        val z = scalar.copyOf()
        z[31] = ((z[31].toInt() and 127) or 64).toByte()
        z[0] = (z[0].toInt() and 248).toByte()
        val x = gf()
        unpack25519(x, u)
        val a = gf(); val b = x.copyOf(); val c = gf(); val d = gf(); val e = gf(); val f = gf()
        a[0] = 1; d[0] = 1
        for (i in 254 downTo 0) {
            val r = ((z[i shr 3].toInt() and 0xff) shr (i and 7)) and 1
            sel25519(a, b, r); sel25519(c, d, r)
            fAdd(e, a, c); fSub(a, a, c); fAdd(c, b, d); fSub(b, b, d)
            fSq(d, e); fSq(f, a); fMul(a, c, a); fMul(c, b, e)
            fAdd(e, a, c); fSub(a, a, c); fSq(b, a); fSub(c, d, f)
            fMul(a, c, GF_121665); fAdd(a, a, d); fMul(c, c, a); fMul(a, d, f)
            fMul(d, b, x); fSq(b, e)
            sel25519(a, b, r); sel25519(c, d, r)
        }
        inv25519(c, c)
        fMul(a, a, c)
        val out = ByteArray(32)
        pack25519(out, a)
        z.fill(0)
        return out
    }

    private val BASE = ByteArray(32).also { it[0] = 9 }

    fun x25519Base(scalar: ByteArray): ByteArray = x25519(scalar, BASE)

    fun isAllZero(b: ByteArray): Boolean {
        var acc = 0
        for (x in b) acc = acc or x.toInt()
        return acc == 0
    }

    // ------------------------------------------------------------------ Salsa20 family

    private val SIGMA = "expand 32-byte k".toByteArray(Charsets.US_ASCII)

    private fun ld32(b: ByteArray, o: Int): Int =
        (b[o].toInt() and 0xff) or ((b[o + 1].toInt() and 0xff) shl 8) or
            ((b[o + 2].toInt() and 0xff) shl 16) or ((b[o + 3].toInt() and 0xff) shl 24)

    private fun st32(b: ByteArray, o: Int, v: Int) {
        b[o] = v.toByte(); b[o + 1] = (v shr 8).toByte(); b[o + 2] = (v shr 16).toByte(); b[o + 3] = (v shr 24).toByte()
    }

    /** Fills the 16-word Salsa20 input: key (32), 16 bytes in the middle (nonce and counter). */
    private fun input(key: ByteArray, sixteen: ByteArray, sOff: Int): IntArray {
        val x = IntArray(16)
        x[0] = ld32(SIGMA, 0); x[5] = ld32(SIGMA, 4); x[10] = ld32(SIGMA, 8); x[15] = ld32(SIGMA, 12)
        for (i in 0 until 4) {
            x[1 + i] = ld32(key, 4 * i)
            x[11 + i] = ld32(key, 16 + 4 * i)
            x[6 + i] = ld32(sixteen, sOff + 4 * i)
        }
        return x
    }

    private fun rounds(x: IntArray) {
        var x0 = x[0]; var x1 = x[1]; var x2 = x[2]; var x3 = x[3]; var x4 = x[4]; var x5 = x[5]; var x6 = x[6]; var x7 = x[7]
        var x8 = x[8]; var x9 = x[9]; var x10 = x[10]; var x11 = x[11]; var x12 = x[12]; var x13 = x[13]; var x14 = x[14]; var x15 = x[15]
        for (i in 0 until 10) {
            x4 = x4 xor Integer.rotateLeft(x0 + x12, 7); x8 = x8 xor Integer.rotateLeft(x4 + x0, 9)
            x12 = x12 xor Integer.rotateLeft(x8 + x4, 13); x0 = x0 xor Integer.rotateLeft(x12 + x8, 18)
            x9 = x9 xor Integer.rotateLeft(x5 + x1, 7); x13 = x13 xor Integer.rotateLeft(x9 + x5, 9)
            x1 = x1 xor Integer.rotateLeft(x13 + x9, 13); x5 = x5 xor Integer.rotateLeft(x1 + x13, 18)
            x14 = x14 xor Integer.rotateLeft(x10 + x6, 7); x2 = x2 xor Integer.rotateLeft(x14 + x10, 9)
            x6 = x6 xor Integer.rotateLeft(x2 + x14, 13); x10 = x10 xor Integer.rotateLeft(x6 + x2, 18)
            x3 = x3 xor Integer.rotateLeft(x15 + x11, 7); x7 = x7 xor Integer.rotateLeft(x3 + x15, 9)
            x11 = x11 xor Integer.rotateLeft(x7 + x3, 13); x15 = x15 xor Integer.rotateLeft(x11 + x7, 18)

            x1 = x1 xor Integer.rotateLeft(x0 + x3, 7); x2 = x2 xor Integer.rotateLeft(x1 + x0, 9)
            x3 = x3 xor Integer.rotateLeft(x2 + x1, 13); x0 = x0 xor Integer.rotateLeft(x3 + x2, 18)
            x6 = x6 xor Integer.rotateLeft(x5 + x4, 7); x7 = x7 xor Integer.rotateLeft(x6 + x5, 9)
            x4 = x4 xor Integer.rotateLeft(x7 + x6, 13); x5 = x5 xor Integer.rotateLeft(x4 + x7, 18)
            x11 = x11 xor Integer.rotateLeft(x10 + x9, 7); x8 = x8 xor Integer.rotateLeft(x11 + x10, 9)
            x9 = x9 xor Integer.rotateLeft(x8 + x11, 13); x10 = x10 xor Integer.rotateLeft(x9 + x8, 18)
            x12 = x12 xor Integer.rotateLeft(x15 + x14, 7); x13 = x13 xor Integer.rotateLeft(x12 + x15, 9)
            x14 = x14 xor Integer.rotateLeft(x13 + x12, 13); x15 = x15 xor Integer.rotateLeft(x14 + x13, 18)
        }
        x[0] = x0; x[1] = x1; x[2] = x2; x[3] = x3; x[4] = x4; x[5] = x5; x[6] = x6; x[7] = x7
        x[8] = x8; x[9] = x9; x[10] = x10; x[11] = x11; x[12] = x12; x[13] = x13; x[14] = x14; x[15] = x15
    }

    /** HSalsa20(key, 16-byte input): the 32-byte XSalsa20 subkey. */
    fun hsalsa20(key: ByteArray, in16: ByteArray, off: Int = 0): ByteArray {
        val x = input(key, in16, off)
        rounds(x)
        val out = ByteArray(32)
        val idx = intArrayOf(0, 5, 10, 15, 6, 7, 8, 9)
        for (i in 0 until 8) st32(out, 4 * i, x[idx[i]])
        return out
    }

    /** XSalsa20 key stream, XORed into [data] in place, starting at key-stream byte [skip]. */
    private fun xsalsa20Xor(key: ByteArray, nonce: ByteArray, data: ByteArray, off: Int, len: Int, skip: Int) {
        val sub = hsalsa20(key, nonce, 0)
        val sixteen = ByteArray(16)
        System.arraycopy(nonce, 16, sixteen, 0, 8)
        val block = ByteArray(64)
        var counter = (skip / 64).toLong()
        var pos = skip % 64
        var i = 0
        while (i < len) {
            for (k in 0 until 8) sixteen[8 + k] = (counter ushr (8 * k)).toByte()
            val x = input(sub, sixteen, 0)
            val y = x.copyOf()
            rounds(x)
            for (k in 0 until 16) st32(block, 4 * k, x[k] + y[k])
            while (pos < 64 && i < len) {
                data[off + i] = (data[off + i].toInt() xor block[pos].toInt()).toByte()
                i++; pos++
            }
            pos = 0
            counter++
        }
        sub.fill(0)
    }

    /** The first 32 bytes of the XSalsa20 key stream (the Poly1305 key of a secretbox). */
    private fun polyKey(key: ByteArray, nonce: ByteArray): ByteArray {
        val k = ByteArray(32)
        xsalsa20Xor(key, nonce, k, 0, 32, 0)
        return k
    }

    // ------------------------------------------------------------------ Poly1305

    fun poly1305(key: ByteArray, msg: ByteArray, off: Int = 0, len: Int = msg.size - off): ByteArray {
        val m26 = 0x3ffffffL
        val r0 = ld32(key, 0).toLong() and 0xffffffffL and 0x3ffffff
        val r1 = ((ld32(key, 3).toLong() and 0xffffffffL) ushr 2) and 0x3ffff03
        val r2 = ((ld32(key, 6).toLong() and 0xffffffffL) ushr 4) and 0x3ffc0ff
        val r3 = ((ld32(key, 9).toLong() and 0xffffffffL) ushr 6) and 0x3f03fff
        val r4 = ((ld32(key, 12).toLong() and 0xffffffffL) ushr 8) and 0x00fffff
        val s1 = r1 * 5; val s2 = r2 * 5; val s3 = r3 * 5; val s4 = r4 * 5
        var h0 = 0L; var h1 = 0L; var h2 = 0L; var h3 = 0L; var h4 = 0L
        val blk = ByteArray(16)
        var p = 0
        while (p < len) {
            val n = minOf(16, len - p)
            val hibit: Long
            val b: ByteArray
            val bo: Int
            if (n == 16) {
                b = msg; bo = off + p; hibit = 1L shl 24
            } else {
                blk.fill(0)
                System.arraycopy(msg, off + p, blk, 0, n)
                blk[n] = 1
                b = blk; bo = 0; hibit = 0
            }
            h0 += (ld32(b, bo).toLong() and 0xffffffffL) and m26
            h1 += ((ld32(b, bo + 3).toLong() and 0xffffffffL) ushr 2) and m26
            h2 += ((ld32(b, bo + 6).toLong() and 0xffffffffL) ushr 4) and m26
            h3 += ((ld32(b, bo + 9).toLong() and 0xffffffffL) ushr 6) and m26
            h4 += ((ld32(b, bo + 12).toLong() and 0xffffffffL) ushr 8) or hibit
            val d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1
            var d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2
            var d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3
            var d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4
            var d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0
            var c = d0 ushr 26; h0 = d0 and m26
            d1 += c; c = d1 ushr 26; h1 = d1 and m26
            d2 += c; c = d2 ushr 26; h2 = d2 and m26
            d3 += c; c = d3 ushr 26; h3 = d3 and m26
            d4 += c; c = d4 ushr 26; h4 = d4 and m26
            h0 += c * 5; c = h0 ushr 26; h0 = h0 and m26
            h1 += c
            p += n
        }
        var c = h1 ushr 26; h1 = h1 and m26
        h2 += c; c = h2 ushr 26; h2 = h2 and m26
        h3 += c; c = h3 ushr 26; h3 = h3 and m26
        h4 += c; c = h4 ushr 26; h4 = h4 and m26
        h0 += c * 5; c = h0 ushr 26; h0 = h0 and m26
        h1 += c
        var g0 = h0 + 5; c = g0 ushr 26; g0 = g0 and m26
        var g1 = h1 + c; c = g1 ushr 26; g1 = g1 and m26
        var g2 = h2 + c; c = g2 ushr 26; g2 = g2 and m26
        var g3 = h3 + c; c = g3 ushr 26; g3 = g3 and m26
        var g4 = h4 + c - (1L shl 26)
        var mask = (g4 ushr 63) - 1   // all ones when g >= p (use g), zero otherwise
        g0 = g0 and mask; g1 = g1 and mask; g2 = g2 and mask; g3 = g3 and mask; g4 = g4 and mask
        mask = mask.inv()
        h0 = (h0 and mask) or g0; h1 = (h1 and mask) or g1; h2 = (h2 and mask) or g2
        h3 = (h3 and mask) or g3; h4 = (h4 and mask) or g4
        val f0 = (h0 or (h1 shl 26)) and 0xffffffffL
        val f1 = ((h1 ushr 6) or (h2 shl 20)) and 0xffffffffL
        val f2 = ((h2 ushr 12) or (h3 shl 14)) and 0xffffffffL
        val f3 = ((h3 ushr 18) or (h4 shl 8)) and 0xffffffffL
        val out = ByteArray(16)
        var f = f0 + (ld32(key, 16).toLong() and 0xffffffffL); st32(out, 0, f.toInt())
        f = f1 + (ld32(key, 20).toLong() and 0xffffffffL) + (f ushr 32); st32(out, 4, f.toInt())
        f = f2 + (ld32(key, 24).toLong() and 0xffffffffL) + (f ushr 32); st32(out, 8, f.toInt())
        f = f3 + (ld32(key, 28).toLong() and 0xffffffffL) + (f ushr 32); st32(out, 12, f.toInt())
        return out
    }

    // ------------------------------------------------------------------ secretbox

    /** crypto_secretbox_easy: returns tag (16) || ciphertext. */
    fun secretbox(msg: ByteArray, nonce: ByteArray, key: ByteArray): ByteArray {
        require(nonce.size == 24 && key.size == 32)
        val out = ByteArray(16 + msg.size)
        System.arraycopy(msg, 0, out, 16, msg.size)
        xsalsa20Xor(key, nonce, out, 16, msg.size, 32)
        val pk = polyKey(key, nonce)
        val tag = poly1305(pk, out, 16, msg.size)
        pk.fill(0)
        System.arraycopy(tag, 0, out, 0, 16)
        return out
    }

    /** Opens a tag-first box; null when the tag does not match. */
    fun secretboxOpen(box: ByteArray, nonce: ByteArray, key: ByteArray, off: Int = 0, len: Int = box.size - off): ByteArray? {
        require(nonce.size == 24 && key.size == 32)
        if (len < 16) return null
        val pk = polyKey(key, nonce)
        val tag = poly1305(pk, box, off + 16, len - 16)
        pk.fill(0)
        if (!MessageDigest.isEqual(tag, box.copyOfRange(off, off + 16))) return null
        val out = box.copyOfRange(off + 16, off + len)
        xsalsa20Xor(key, nonce, out, 0, out.size, 32)
        return out
    }
}
