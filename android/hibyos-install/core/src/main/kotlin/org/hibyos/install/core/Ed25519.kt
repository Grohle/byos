package org.hibyos.install.core

import java.math.BigInteger
import java.security.MessageDigest

/**
 * Ed25519 signature verification (RFC 8032), verification only. Written with BigInteger
 * because the platform's own Ed25519 is missing on the Android versions this app supports;
 * one verification takes a few milliseconds, and nothing secret is ever handled here.
 *
 * Same acceptance rule as the player's updater (docs/OTA.md): S must be below the group
 * order and the recomputed R must encode to exactly the bytes in the signature.
 */
object Ed25519 {
    private val P = BigInteger.valueOf(2).pow(255).subtract(BigInteger.valueOf(19))
    internal val L = BigInteger.valueOf(2).pow(252).add(BigInteger("27742317777372353535851937790883648493"))
    private val D = BigInteger.valueOf(-121665).multiply(BigInteger.valueOf(121666).modInverse(P)).mod(P)
    private val D2 = D.add(D).mod(P)
    private val SQRT_M1 = BigInteger.valueOf(2).modPow(P.subtract(BigInteger.ONE).shiftRight(2), P)

    internal class Pt(val x: BigInteger, val y: BigInteger, val z: BigInteger, val t: BigInteger)

    private val ZERO = Pt(BigInteger.ZERO, BigInteger.ONE, BigInteger.ONE, BigInteger.ZERO)
    internal val BASE: Pt by lazy {
        val y = BigInteger.valueOf(4).multiply(BigInteger.valueOf(5).modInverse(P)).mod(P)
        val x = recoverX(y, false)!!
        Pt(x, y, BigInteger.ONE, x.multiply(y).mod(P))
    }

    private fun add(a: Pt, b: Pt): Pt {
        val aa = a.y.subtract(a.x).multiply(b.y.subtract(b.x)).mod(P)
        val bb = a.y.add(a.x).multiply(b.y.add(b.x)).mod(P)
        val c = a.t.multiply(D2).mod(P).multiply(b.t).mod(P)
        val d = a.z.add(a.z).multiply(b.z).mod(P)
        val e = bb.subtract(aa)
        val f = d.subtract(c)
        val g = d.add(c)
        val h = bb.add(aa)
        return Pt(e.multiply(f).mod(P), g.multiply(h).mod(P), f.multiply(g).mod(P), e.multiply(h).mod(P))
    }

    internal fun mul(k: BigInteger, p: Pt): Pt {
        var r = ZERO
        for (i in k.bitLength() - 1 downTo 0) {
            r = add(r, r)
            if (k.testBit(i)) r = add(r, p)
        }
        return r
    }

    private fun recoverX(y: BigInteger, odd: Boolean): BigInteger? {
        if (y >= P) return null
        val y2 = y.multiply(y).mod(P)
        val x2 = y2.subtract(BigInteger.ONE).multiply(D.multiply(y2).add(BigInteger.ONE).modInverse(P)).mod(P)
        if (x2.signum() == 0) return if (odd) null else BigInteger.ZERO
        var x = x2.modPow(P.add(BigInteger.valueOf(3)).shiftRight(3), P)
        if (x.multiply(x).subtract(x2).mod(P).signum() != 0) x = x.multiply(SQRT_M1).mod(P)
        if (x.multiply(x).subtract(x2).mod(P).signum() != 0) return null
        if (x.testBit(0) != odd) x = P.subtract(x)
        return x
    }

    internal fun le(b: ByteArray, off: Int, len: Int): BigInteger {
        val be = ByteArray(len + 1)
        for (i in 0 until len) be[len - i] = b[off + i]
        return BigInteger(be)
    }

    private fun decode(b: ByteArray): Pt? {
        val raw = le(b, 0, 32)
        val y = raw.clearBit(255)
        val x = recoverX(y, raw.testBit(255)) ?: return null
        return Pt(x, y, BigInteger.ONE, x.multiply(y).mod(P))
    }

    internal fun encode(p: Pt): ByteArray {
        val zi = p.z.modInverse(P)
        val x = p.x.multiply(zi).mod(P)
        var y = p.y.multiply(zi).mod(P)
        if (x.testBit(0)) y = y.setBit(255)
        val out = ByteArray(32)
        val be = y.toByteArray()
        for (i in 0 until minOf(32, be.size)) out[i] = be[be.size - 1 - i]
        return out
    }

    fun verify(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean {
        if (publicKey.size != 32 || signature.size != 64) return false
        val a = decode(publicKey) ?: return false
        val s = le(signature, 32, 32)
        if (s >= L) return false
        val md = MessageDigest.getInstance("SHA-512")
        md.update(signature, 0, 32)
        md.update(publicKey)
        md.update(message)
        val h = le(md.digest(), 0, 64).mod(L)
        // R' = [s]B - [h]A
        val negA = Pt(P.subtract(a.x).mod(P), a.y, a.z, P.subtract(a.t).mod(P))
        val r = add(mul(s, BASE), mul(h, negA))
        return MessageDigest.isEqual(encode(r), signature.copyOfRange(0, 32))
    }

    /**
     * Checks a JSON text signed as docs/OTA.md describes: exactly one "signature" key whose
     * value is "ed25519:" and 128 hex digits; the signed message is the whole text with those
     * digits replaced by zeros. True if one of [publicKeysHex] made the signature.
     */
    fun verifySignedJson(data: ByteArray, publicKeysHex: List<String>): Boolean {
        val text = String(data, Charsets.ISO_8859_1)
        if (Regex("\"signature\"").findAll(text).count() != 1) return false
        val m = Regex("\"signature\"\\s*:\\s*\"ed25519:([0-9A-Fa-f]{128})\"").find(text) ?: return false
        val g = m.groups[1]!!
        val sig = try {
            Hex.parse(g.value.lowercase())
        } catch (e: IllegalArgumentException) {
            return false
        }
        val msg = data.clone()
        for (i in g.range) msg[i] = '0'.code.toByte()
        for (k in publicKeysHex) {
            if (!Regex("[0-9A-Fa-f]{64}").matches(k) || k.all { it == '0' }) continue
            if (verify(Hex.parse(k.lowercase()), msg, sig)) return true
        }
        return false
    }
}
