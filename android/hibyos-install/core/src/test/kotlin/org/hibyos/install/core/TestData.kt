package org.hibyos.install.core

import java.io.ByteArrayOutputStream

/** Generated test data: the Kotlin twin of gen() in tools/gen_test_vectors.py. */
internal object TestData {
    private val WORDS = listOf("hiby", "os", "player", "music", "the", "and", "settings", "wifi", "volume", "track",
        "album", "artist", "\n")

    fun gen(kind: String, seed: Int, size: Int): ByteArray {
        val r = TestUtil.Rng(seed)
        if (kind == "zeros") return ByteArray(size)
        val out = ByteArrayOutputStream(size + 512)
        when (kind) {
            "random" -> while (out.size() < size) out.write(r.next() and 0xFF)
            "text" -> while (out.size() < size) {
                out.write(WORDS[r.below(WORDS.size)].toByteArray(Charsets.US_ASCII))
                out.write(' '.code)
            }
            "runs" -> while (out.size() < size) {
                val b = r.below(256)
                repeat(1 + r.below(300)) { out.write(b) }
            }
            "copies" -> {
                // needs random access to what was written: use a plain array
                var buf = ByteArray(size + 1024)
                var n = 0
                fun put(b: Int) {
                    if (n == buf.size) buf = buf.copyOf(n * 2)
                    buf[n++] = b.toByte()
                }
                while (n < 64) put(r.next() and 0xFF)
                while (n < size) {
                    if (r.below(4) == 0) {
                        repeat(1 + r.below(8)) { put(r.next() and 0xFF) }
                    } else {
                        val dist = 1 + r.below(minOf(n, 60000))
                        repeat(3 + r.below(400)) { put(buf[n - dist].toInt() and 0xFF) }
                    }
                }
                return buf.copyOf(size)
            }
            else -> throw IllegalArgumentException(kind)
        }
        return out.toByteArray().copyOf(size)
    }

    fun resource(name: String): ByteArray =
        (TestData::class.java.getResourceAsStream("/$name") ?: throw AssertionError("missing test resource $name"))
            .use { it.readBytes() }

    /** Lines of a text resource without comments. */
    fun lines(name: String): List<String> =
        String(resource(name), Charsets.UTF_8).split('\n').filter { it.isNotBlank() && !it.startsWith("#") }
}
