package org.hibyos.install.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** LZO1X against tools/lzo1x.py: same compressed bytes for the same input, and round trips. */
class Lzo1xTest {
    @Test
    fun sameCompressedBytesAsPython() {
        val vectors = TestData.lines("lzo_vectors.txt")
        assertTrue(vectors.size >= 90)
        var bytes = 0L
        for (line in vectors) {
            val (kind, seed, size, clen, sha) = line.split(' ')
            val data = TestData.gen(kind, seed.toInt(), size.toInt())
            val comp = Lzo1x.compress(data)
            assertEquals("compressed size of $kind/$size", clen.toInt(), comp.size)
            assertEquals("compressed bytes of $kind/$size", sha, sha256Hex(comp))
            assertArrayEquals("round trip of $kind/$size", data, Lzo1x.decompress(comp, data.size))
            bytes += data.size
        }
        println("LZO: ${vectors.size} vectors, $bytes bytes, compressed output identical to tools/lzo1x.py")
    }

    @Test
    fun compressesASliceOfAnArray() {
        val data = TestData.gen("copies", 77, 50000)
        val whole = Lzo1x.compress(data.copyOfRange(1000, 31000))
        assertArrayEquals(whole, Lzo1x.compress(data, 1000, 30000))
    }

    @Test
    fun rejectsDamagedInput() {
        val data = TestData.gen("copies", 5, 20000)
        val comp = Lzo1x.compress(data)
        // cut short
        for (cut in listOf(0, 1, 2, comp.size / 2, comp.size - 1)) {
            try {
                Lzo1x.decompress(comp.copyOf(cut), data.size)
                fail("a stream cut at $cut was accepted")
            } catch (e: LzoException) {
                // expected
            }
        }
        // output bigger than allowed
        try {
            Lzo1x.decompress(comp, data.size - 1)
            fail("an output overrun was accepted")
        } catch (e: LzoException) {
            // expected
        }
        // random garbage never crashes with anything but LzoException
        val r = TestUtil.Rng(99)
        var rejected = 0
        repeat(3000) {
            val junk = ByteArray(1 + r.below(200)) { r.next().toByte() }
            try {
                Lzo1x.decompress(junk, 4096)
            } catch (e: LzoException) {
                rejected++
            }
        }
        // flipped bits in a valid stream
        repeat(2000) {
            val bad = comp.clone()
            bad[r.below(bad.size)] = r.next().toByte()
            try {
                Lzo1x.decompress(bad, data.size)
            } catch (e: LzoException) {
                rejected++
            }
        }
        assertTrue(rejected > 1000)
    }
}
