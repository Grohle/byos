package org.hibyos.install.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * SquashFS reader and writer against tools/squashfs.py on an image made by mksquashfs from
 * generated data (see tools/gen_test_vectors.py, which also checks with unsquashfs that the
 * image written by the Python code extracts to the intended files).
 */
class SquashFsTest {
    private val image = TestData.resource("synthetic.squashfs")
    private val expect = TestData.lines("synthetic.txt")

    private fun field(name: String) = expect.first { it.startsWith("$name ") }.split(' ').drop(1)

    private fun listing(img: SquashImage, root: Inode): List<String> = SquashImage.walk(root).map { (path, n) ->
        val extra = when (n.kind) {
            Kind.FILE -> "${n.size} ${Hex.of(img.digestFile(n, sha256()))}"
            Kind.SYMLINK -> Hex.of(n.target!!)
            Kind.BLKDEV, Kind.CHRDEV -> n.rdev.toString()
            else -> "-"
        }
        "${Hex.of(path.toByteArray(Charsets.ISO_8859_1))} ${n.kind.label} ${Integer.toOctalString(n.mode)} ${n.uid} ${n.gid} $extra"
    }

    private fun node(root: Inode, path: String): Inode {
        var n = root
        for (part in path.split('/')) n = n.children!![nameKey(part)] ?: throw AssertionError("no $path")
        return n
    }

    /** The same changes as sq_changes() in tools/gen_test_vectors.py. */
    private fun changes(img: SquashImage, root: Inode) {
        node(root, "small.txt").setData(TestData.gen("text", 101, 5000))
        val f = Inode(Kind.FILE, 0x1ed, 0, 0, img.mtime)
        f.setData(TestData.gen("copies", 102, 200000))
        node(root, "dir").children!!["new.bin"] = f
        val d = Inode(Kind.DIR, 0x1ed, 0, 0, img.mtime)
        val a = Inode(Kind.FILE, 0x1a4, 1000, 100, img.mtime)
        a.setData(TestData.gen("text", 103, 17))
        d.children!!["a"] = a
        val ln = Inode(Kind.SYMLINK, 0x1ff, 0, 0, img.mtime)
        ln.target = "a".toByteArray()
        d.children["ln"] = ln
        root.children!!["newdir"] = d
        assertNotNull(node(root, "many").children!!.remove("f0007"))
        node(root, "bin/tool").mode = 0x1c0
        node(root, "zeros.bin").setData(ByteArray(0))
        val ko = img.readFile(node(root, "lib/driver.ko")).clone()
        byteArrayOf(0xde.toByte(), 0xad.toByte(), 0xbe.toByte(), 0xef.toByte()).copyInto(ko, 1000)
        node(root, "lib/driver.ko").setData(ko)
    }

    @Test
    fun readsWhatMksquashfsWrote() {
        assertEquals(field("image")[1], sha256Hex(image))
        val img = SquashImage(ArraySource(image))
        val root = img.readTree()
        val want = expect.filter { it.startsWith("before ") }.map { it.removePrefix("before ") }
        assertEquals(want, listing(img, root))
        // generated contents, straight from the generator
        assertArrayEquals(TestData.gen("copies", 13, 300000), img.readFile(node(root, "multi.bin")))
        assertArrayEquals(TestData.gen("random", 14, 70000), img.readFile(node(root, "random.bin")))
        assertArrayEquals(ByteArray(400000), img.readFile(node(root, "zeros.bin")))
        assertArrayEquals("odd name\n".toByteArray(), img.readFile(node(root, "many/ñandú ü.txt")))
        assertEquals("many/ñandú ü.txt", displayName("many/" + nameKey("ñandú ü.txt")))
        // hard links are one object
        assertSame(node(root, "small.txt"), node(root, "dir/hard"))
        println("SquashFS read: ${want.size} entries identical to tools/squashfs.py")
    }

    @Test
    fun rewritesWithoutChangesLikePython() {
        val img = SquashImage(ArraySource(image))
        val root = img.readTree()
        val sink = ArraySink()
        SquashWriter.write(root, img, sink, 1234567890L)
        val out = sink.toByteArray()
        val (_, size, sha) = field("rewrite")
        assertEquals(size.toInt(), out.size)
        assertEquals(sha, sha256Hex(out))
        assertEquals(1234567890L, SquashImage(ArraySource(out)).mtime)
    }

    @Test
    fun writesChangesLikePython() {
        val img = SquashImage(ArraySource(image))
        val root = img.readTree()
        changes(img, root)
        val want = expect.filter { it.startsWith("after ") }.map { it.removePrefix("after ") }
        assertEquals(want, listing(img, root))
        val sink = ArraySink()
        var lastDone = -1L
        var total = 0L
        val stats = SquashWriter.write(root, img, sink) { done, all ->
            assertTrue(done >= lastDone && done <= all)
            lastDone = done
            total = all
        }
        val out = sink.toByteArray()
        val (size, sha) = field("changed")
        assertEquals(size.toInt(), out.size)
        assertEquals(sha, sha256Hex(out))
        assertEquals(total, lastDone)
        val st = field("stats").map { it.toLong() }
        assertEquals(st, listOf(stats.keptData, stats.newDataIn, stats.newDataOut, stats.metadata))
        assertEquals(out.size.toLong(), stats.bytesUsed)

        // and it reads back as the tree that was written
        val again = SquashImage(ArraySource(out))
        val root2 = again.readTree()
        assertEquals(want, listing(again, root2))
        assertSame(node(root2, "small.txt"), node(root2, "dir/hard"))
        println("SquashFS write: ${out.size} bytes identical to tools/squashfs.py (sha256 $sha), ${want.size} entries read back")
    }

    @Test
    fun writingToAFileGivesTheSameBytes() {
        val img = SquashImage(ArraySource(image))
        val root = img.readTree()
        changes(img, root)
        val dir = TestUtil.tempDir("squashfs")
        val f = java.io.File(dir, "out.squashfs")
        FileSink(f).use { SquashWriter.write(root, img, it) }
        assertEquals(field("changed")[1], sha256Hex(f.readBytes()))
        dir.deleteRecursively()
    }

    @Test
    fun rejectsDamagedImages() {
        fun mustFail(what: String, data: ByteArray) {
            try {
                val img = SquashImage(ArraySource(data))
                val root = img.readTree()
                for ((_, n) in SquashImage.walk(root)) if (n.kind == Kind.FILE) img.digestFile(n, sha256())
                fail("$what was accepted")
            } catch (e: SquashfsException) {
                // expected
            }
        }
        mustFail("an empty image", ByteArray(0))
        mustFail("a wrong magic", image.clone().also { it[0] = 0 })
        mustFail("a cut image", image.copyOf(image.size / 2))
        mustFail("a zstd image", image.clone().also { it[20] = 6 })
        // random damage must end in a clean error or a harmless read, never in another exception
        val r = TestUtil.Rng(4242)
        var rejected = 0
        repeat(400) {
            val bad = image.clone()
            repeat(1 + r.below(4)) { bad[r.below(bad.size)] = r.next().toByte() }
            try {
                val img = SquashImage(ArraySource(bad))
                val root = img.readTree()
                for ((_, n) in SquashImage.walk(root)) if (n.kind == Kind.FILE) img.digestFile(n, sha256())
            } catch (e: SquashfsException) {
                rejected++
            }
        }
        println("SquashFS damage: $rejected of 400 damaged images rejected cleanly, the rest read without error")
    }
}
