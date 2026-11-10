package org.hibyos.install.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream

/**
 * The firmware container writer against tools/build.py write_upt() (pycdlib): the same
 * generated files must give the same bytes. The reference numbers are in iso_vector.txt.
 */
class IsoTest {
    private val vec = TestData.lines("iso_vector.txt").associate { it.substringBefore(' ') to it.substringAfter(' ') }

    private fun files(): List<IsoInput> {
        val out = ArrayList<IsoInput>()
        out += IsoInput("ota_config.in", ArraySource(TestData.gen("text", 7001, 190)))
        out += IsoInput("ota_v0/ota_v0.ok", ArraySource("ok\n".toByteArray()))
        out += IsoInput("ota_v0/ota_update.in", ArraySource(TestData.gen("text", 7002, 420)))
        out += Upt.chunkImage("xImage", ArraySource(TestData.gen("copies", 7003, 700000)), 524288).files
        out += Upt.chunkImage("rootfs.squashfs", ArraySource(TestData.gen("copies", 7004, 1300001)), 524288).files
        return out
    }

    private fun build(files: List<IsoInput>): ByteArray {
        val bos = ByteArrayOutputStream()
        val n = UptIsoWriter.write(bos, files, vec["epoch"]!!.toLong(), vec["utc_offset"]!!.toInt())
        assertEquals(bos.size().toLong(), n)
        return bos.toByteArray()
    }

    @Test
    fun sameBytesAsPycdlib() {
        val files = files()
        assertEquals(vec["files"]!!.toInt(), files.size)
        assertEquals(vec["names_sha256"], sha256Hex(files.map { it.path }.sorted().joinToString("\n").toByteArray()))
        val iso = build(files)
        assertEquals(vec["size"]!!.toInt(), iso.size)
        assertEquals(vec["sha256"], sha256Hex(iso))
        // the order in which the files are given does not matter
        assertArrayEquals(iso, build(files.reversed()))
        println("ISO: ${iso.size} bytes identical to tools/build.py write_upt() (sha256 ${vec["sha256"]})")
    }

    @Test
    fun readsBackWhatItWrote() {
        val files = files()
        val iso = build(files)
        val src = ArraySource(iso)
        val list = IsoReader.list(src)
        assertEquals(files.map { it.path }.toSet(), list.keys)
        for (f in files) {
            val e = list[f.path]!!
            assertEquals(f.data.size, e.size)
            assertArrayEquals(f.path, f.data.bytes(0, f.data.size.toInt()), src.bytes(e.offset, e.size.toInt()))
        }
        // and the chunks join back into the images, with every MD5 of the chain checked
        val whole = TestData.gen("copies", 7004, 1300001)
        var seen = 0L
        val img = Upt.openImage(src, list, "rootfs.squashfs", md5Hex(whole)) { seen += it }
        assertEquals(whole.size.toLong(), img.size)
        assertEquals(whole.size.toLong(), seen)
        assertArrayEquals(whole, img.bytes(0, whole.size))
        assertArrayEquals(whole.copyOfRange(524000, 1100000), img.bytes(524000, 576000))
    }

    @Test
    fun detectsADamagedChunk() {
        val files = files()
        val iso = build(files)
        val list = IsoReader.list(ArraySource(iso))
        val whole = TestData.gen("copies", 7004, 1300001)
        val victim = list.values.first { it.path.startsWith("ota_v0/rootfs.squashfs.0001.") }
        iso[(victim.offset + 12345).toInt()] = (iso[(victim.offset + 12345).toInt()] + 1).toByte()
        try {
            Upt.openImage(ArraySource(iso), list, "rootfs.squashfs", md5Hex(whole))
            fail("a damaged chunk was accepted")
        } catch (e: UptException) {
            assertTrue(e.message!!.contains("chunk 1"))
        }
        // a missing chunk
        val fewer = LinkedHashMap(list)
        fewer.remove(victim.path)
        try {
            Upt.openImage(ArraySource(build(files)), fewer, "rootfs.squashfs", md5Hex(whole))
            fail("a missing chunk was accepted")
        } catch (e: UptException) {
            // expected
        }
    }

    @Test
    fun shortNamesFollowWriteUpt() {
        val names = UptIsoWriter.shortNames(listOf("ota_config.in", "ota_v0/ota_update.in", "ota_v0/ota_v0.ok",
            "ota_v0/rootfs.squashfs.0000.aaaa", "ota_v0/rootfs.squashfs.0001.bbbb", "ota_v0/rootfs.squashfs.0002.cccc",
            "ota_v0/xImage.0000.f00f", "ota_v0/ota_md5_xImage.1234"))
        assertEquals("OTA_CONF.IN;1", names["ota_config.in"])
        assertEquals("ROOTFS.AAA;1", names["ota_v0/rootfs.squashfs.0000.aaaa"])
        assertEquals("ROOTFS.BBB;1", names["ota_v0/rootfs.squashfs.0001.bbbb"])
        assertEquals("XIMAGE.F00;1", names["ota_v0/xImage.0000.f00f"])
        assertEquals("OTA_MD5_.123;1", names["ota_v0/ota_md5_xImage.1234"])
        // collisions get a counter, as in write_upt()
        val clash = UptIsoWriter.shortNames(listOf("ota_v0/abcdefghij.0000.aaa1", "ota_v0/abcdefghij.0001.aaa2",
            "ota_v0/abcdefghij.0002.aaa3"))
        assertEquals(listOf("ABCDEFGH.AAA;1", "ABCDEFG1.AAA;1", "ABCDEFG2.AAA;1"), clash.values.toList())
    }

    @Test
    fun gmtOffsetFollowsPycdlib() {
        assertEquals(4, UptIsoWriter.gmtOffset(1767225600, 3600))
        assertEquals(8, UptIsoWriter.gmtOffset(1751328000, 7200))
        assertEquals(0, UptIsoWriter.gmtOffset(1767225600, 0))
        assertEquals(-20, UptIsoWriter.gmtOffset(1751328000 + 12 * 3600, -5 * 3600))
        assertEquals(22, UptIsoWriter.gmtOffset(1751328000 + 12 * 3600, 5 * 3600 + 1800))
    }

    @Test
    fun rejectsThingsThatAreNotContainers() {
        for (junk in listOf(ByteArray(0), ByteArray(40000), TestData.gen("random", 3, 100000))) {
            try {
                IsoReader.list(ArraySource(junk))
                fail("junk was accepted as a container")
            } catch (e: IsoException) {
                // expected
            }
        }
        // damage in the directory area must never give anything but IsoException
        val iso = build(files())
        val r = TestUtil.Rng(31337)
        repeat(500) {
            val bad = iso.copyOf(70000)
            repeat(1 + r.below(6)) { bad[16 * 2048 + r.below(bad.size - 16 * 2048)] = r.next().toByte() }
            try {
                IsoReader.list(ArraySource(bad))
            } catch (e: IsoException) {
                // fine
            }
        }
    }
}
