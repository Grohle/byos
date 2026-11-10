package org.hibyos.install.core

import java.io.File
import java.io.RandomAccessFile

internal object TestUtil {
    /** Runs a program, returns its output; fails with the output if it does not exit with 0. */
    fun run(cmd: List<String>, env: Map<String, String> = emptyMap(), dir: File? = null): String {
        val pb = ProcessBuilder(cmd).redirectErrorStream(true)
        if (dir != null) pb.directory(dir)
        pb.environment().putAll(env)
        val p = pb.start()
        val out = p.inputStream.bufferedReader().readText()
        val code = p.waitFor()
        if (code != 0) throw AssertionError("${cmd.joinToString(" ")} exited with $code:\n$out")
        return out
    }

    /** Offset of the first different byte of two files, or -1 if they are identical. */
    fun firstDifference(a: File, b: File): Long {
        RandomAccessFile(a, "r").use { ra ->
            RandomAccessFile(b, "r").use { rb ->
                val ba = ByteArray(1 shl 16)
                val bb = ByteArray(1 shl 16)
                var pos = 0L
                val n = minOf(ra.length(), rb.length())
                while (pos < n) {
                    val k = minOf(n - pos, ba.size.toLong()).toInt()
                    ra.readFully(ba, 0, k)
                    rb.readFully(bb, 0, k)
                    for (i in 0 until k) if (ba[i] != bb[i]) return pos + i
                    pos += k
                }
                return if (ra.length() == rb.length()) -1 else n
            }
        }
    }

    fun tempDir(name: String): File {
        val base = System.getenv("HIBYOS_WORK")?.let { File(it) }
            ?: File(System.getProperty("java.io.tmpdir"), "hibyos-install-test")
        val d = File(base, name)
        d.deleteRecursively()
        d.mkdirs()
        return d
    }

    fun md5Of(f: File): String {
        val md = md5()
        f.inputStream().use { s ->
            val buf = ByteArray(1 shl 16)
            while (true) {
                val n = s.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return Hex.of(md.digest())
    }

    /** Same generator as tools/gen_test_vectors.py (xorshift32). */
    class Rng(seed: Int) {
        private var s = if (seed == 0) 0x1234567 else seed
        fun next(): Int {
            s = s xor (s shl 13)
            s = s xor (s ushr 17)
            s = s xor (s shl 5)
            return s
        }

        fun below(n: Int): Int = ((next().toLong() and 0xffffffffL) % n).toInt()
    }
}
