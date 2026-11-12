package org.hibyos.install.core

import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.FileOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** The build steps shared by the Android app and byos-cli (Build.kt), on made-up data. */
class BuildTest {
    private lateinit var dir: File
    private lateinit var fw: File

    @Before
    fun setUp() {
        dir = TestUtil.tempDir("build-" + System.nanoTime())
        fw = Synthetic.firmware(File(dir, "in/firmware.upt"))
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    private val payload = linkedMapOf(
        "ByOS/payload/VERSION" to "9.9.9\n".toByteArray(),
        "ByOS/payload/MANIFEST.md5" to "abc  apps/x.bin\n".toByteArray(),
        "ByOS/payload/apps/x.bin" to TestData.gen("random", 5, 90_000),
        "ByOS/README.txt" to "hello\n".toByteArray(),
    )

    private fun bundle(): Bundle {
        val id = Patcher.identify(fw)
        val s = Synthetic.BundleSpec(firmwareMd5 = id.md5, firmwareSha256 = id.sha256, firmwareSize = id.size)
        for ((p, b) in payload) s.sd += p to b
        return Bundle.open(Synthetic.bundle(File(dir, "bundle-${System.nanoTime()}.zip"), s))
    }

    @Test
    fun patchRecordsAndRestores() {
        bundle().use { b ->
            val out = File(dir, "out")
            assertNull(ByosBuild.restore(b, out))
            val (r, rec) = ByosBuild.patch(fw, b, out, File(dir, "work"), PatchOptions(buildId = "0123456789abcdef0123456789abcdef"))
            assertEquals(r.sha256, rec.sha256)
            val again = ByosBuild.restore(b, out)
            assertNotNull(again)
            assertEquals(r.size, again!!.size)
            assertEquals("0123456789abcdef0123456789abcdef", again.buildId)
            assertEquals("9.9.9", again.hibyosVersion)
            // the record format is the Android app's result.txt, unchanged
            assertEquals(listOf("9.9.9", "${r.size}", r.sha256, r.buildId, "${r.files}", "test"), File(out, ByosBuild.RECORD).readLines())
            // a patched file that no longer matches its record is not reused
            File(out, b.outName).appendBytes(byteArrayOf(1))
            assertNull(ByosBuild.restore(b, out))
        }
    }

    @Test
    fun anInterruptedBuildIsDiscardedAndTheFirmwareKept() {
        bundle().use { b ->
            val out = File(dir, "out")
            val work = File(dir, "work")
            val stamp = ByosBuild.stamp(b, "test 1.0")
            val fwDir = File(dir, "fw")
            val cached = FirmwareCache.import(fw, fwDir, b.firmware)!!
            // what a run killed half way leaves: the stamp, a partial firmware, the work folder, a partial export
            out.mkdirs()
            File(out, ByosBuild.STAMP).writeText(stamp)
            File(out, b.outName + ".part").writeBytes(ByteArray(1000))
            File(work, "rootfs.new").apply { parentFile.mkdirs(); writeBytes(ByteArray(1000)) }
            File(out, "ByOS/payload/VERSION.part").apply { parentFile.mkdirs(); writeText("9.9") }
            File(out, "notes.txt").writeText("the user's own file")
            assertNull(ByosBuild.restore(b, out, stamp))
            assertFalse(ByosBuild.sweep(b, out, work, stamp))
            assertEquals(listOf("notes.txt"), out.list()!!.toList())
            assertFalse(work.exists())
            // the next build runs, and the checked firmware is still there
            val (r, _) = ByosBuild.patch(cached.file, b, out, work, PatchOptions(buildId = "0123456789abcdef0123456789abcdef"), stamp = stamp)
            assertEquals(r.sha256, ByosBuild.restore(b, out, stamp)!!.sha256)
            assertEquals(stamp, File(out, ByosBuild.STAMP).readText())
            assertNotNull(FirmwareCache.ready(fwDir, b.firmware))
            // a finished build of the same files is kept; only the leftovers around it go
            File(out, "old-version.upt").writeBytes(ByteArray(10))
            File(out, "x.part").writeBytes(ByteArray(10))
            File(out, ".work").mkdirs()
            assertTrue(ByosBuild.sweep(b, out, work, stamp))
            assertTrue(File(out, b.outName).isFile)
            assertFalse(File(out, "old-version.upt").exists())
            assertFalse(File(out, "x.part").exists())
            assertFalse(File(out, ".work").exists())
            // another version of the app, or other ByOS files: the build is not reused but removed
            assertNull(ByosBuild.restore(b, out, ByosBuild.stamp(b, "test 1.1")))
            assertFalse(ByosBuild.sweep(b, out, work, ByosBuild.stamp(b, "test 1.1")))
            assertFalse(File(out, b.outName).exists())
            assertFalse(File(out, ByosBuild.RECORD).exists())
            assertTrue(File(out, "notes.txt").isFile)
            assertNotNull(FirmwareCache.ready(fwDir, b.firmware))
            // the stamp names the ByOS files by their checksum
            assertTrue(stamp.contains("bundle " + ByosBuild.fileSha256(b.file)))
        }
    }

    @Test
    fun startOverRemovesTheBuildStepsOnly() {
        bundle().use { b ->
            val out = File(dir, "out")
            val stamp = ByosBuild.stamp(b, "test")
            ByosBuild.patch(fw, b, out, File(dir, "work"), PatchOptions(buildId = "0123456789abcdef0123456789abcdef"), stamp = stamp)
            ByosBuild.exportSd(b, out)
            File(out, "build.json").writeText("{}")
            val input = File(out, "given.upt").apply { writeBytes(fw.readBytes()) }
            File(out, "mine.txt").writeText("kept")
            ByosBuild.startOver(out, File(dir, "work"), keep = input)
            assertEquals(setOf("given.upt", "mine.txt"), out.list()!!.toSet())
            assertNull(ByosBuild.restore(b, out))
            // patching from a firmware that sits in the build folder does not delete it
            ByosBuild.patch(input, b, out, File(dir, "work"), PatchOptions(buildId = "0123456789abcdef0123456789abcdef"), stamp = stamp)
            assertTrue(input.isFile)
        }
    }

    @Test
    fun sdOrderPutsTheVersionLastAndFirmwareAtTheEnd() {
        bundle().use { b ->
            val order = ByosBuild.sdOrder(b).map { it.path }
            assertEquals(listOf("ByOS/payload/apps/x.bin", "ByOS/README.txt", "ByOS/payload/MANIFEST.md5", "ByOS/payload/VERSION"), order)
            val upt = File(dir, "x.upt").apply { writeBytes(ByteArray(10)) }
            val items = ByosBuild.items(b, upt, "aa")
            assertEquals(order + "test.upt", items.map { it.path })
            assertEquals(order, ByosBuild.items(b, null, null).map { it.path })
        }
    }

    @Test
    fun exportWritesTheCardLayoutAndKeepsGoodFiles() {
        bundle().use { b ->
            val out = File(dir, "export")
            var last = 0L
            ByosBuild.exportSd(b, out) { d, _ -> last = d }
            assertEquals(b.sdSize, last)
            for ((p, data) in payload) assertArrayEquals(p, data, File(out, p).readBytes())
            assertFalse(File(out, "ByOS/payload/VERSION.part").exists())
            // a damaged copy is written again, a good one is kept
            val x = File(out, "ByOS/payload/apps/x.bin")
            x.writeBytes(ByteArray(5))
            val readme = File(out, "ByOS/README.txt")
            readme.setLastModified(1_000_000_000_000)
            ByosBuild.exportSd(b, out)
            assertArrayEquals(payload["ByOS/payload/apps/x.bin"], x.readBytes())
            assertEquals(1_000_000_000_000, readme.lastModified())
        }
    }

    @Test
    fun firmwareCacheImportsOnlyTheRightFile() {
        bundle().use { b ->
            val cache = File(dir, "fw")
            assertNull(FirmwareCache.ready(cache, b.firmware))
            val junk = File(dir, "junk.upt").apply { writeBytes(TestData.gen("random", 2, 1000)) }
            var wrong: FirmwareId? = null
            assertNull(FirmwareCache.import(junk, cache, b.firmware) { wrong = it })
            assertNotNull(wrong)
            assertNull(FirmwareCache.ready(cache, b.firmware))
            val c = FirmwareCache.import(fw, cache, b.firmware)!!
            assertEquals(fw.length(), c.size)
            assertEquals(c.sha256, FirmwareCache.ready(cache, b.firmware)!!.sha256)
            assertArrayEquals(fw.readBytes(), FirmwareCache.file(cache, b.firmware).readBytes())
            FirmwareCache.forget(cache, b.firmware)
            assertNull(FirmwareCache.ready(cache, b.firmware))
            assertFalse(FirmwareCache.file(cache, b.firmware).exists())
        }
    }

    private fun bundleZip(file: File, version: String): File {
        val json = Synthetic.bundleJson(Synthetic.BundleSpec(firmwareMd5 = "00")).replace("\"9.9.9\"", "\"$version\"")
        ZipOutputStream(FileOutputStream(file)).use { z ->
            z.putNextEntry(ZipEntry("bundle.json")); z.write(json.toByteArray()); z.closeEntry()
        }
        return file
    }

    @Test
    fun bundleStoreUsesTheNewestFiles() {
        val store = BundleStore(File(dir, "store"))
        val shipped = bundleZip(File(dir, "shipped.zip"), "1.0.0")
        val base = store.builtin("a") { shipped.inputStream() }
        assertEquals("1.0.0", base.hibyosVersion)
        assertTrue(store.current(base).builtIn)
        // a newer updated bundle wins; an older one is forgotten
        bundleZip(store.updatedFile, "1.1.0")
        val cur = store.current(base)
        assertFalse(cur.builtIn)
        assertEquals("1.1.0", cur.bundle.hibyosVersion)
        cur.bundle.close()
        bundleZip(store.updatedFile, "0.9.0")
        assertTrue(store.current(base).builtIn)
        assertFalse(store.updatedFile.exists())
        base.close()
        // a new shipped copy (new stamp) replaces the unpacked one
        val newer = bundleZip(File(dir, "shipped2.zip"), "2.0.0")
        store.builtin("b") { newer.inputStream() }.use { assertEquals("2.0.0", it.hibyosVersion) }
        store.builtin("b") { throw AssertionError("not copied again for the same stamp") }.close()
    }
}
