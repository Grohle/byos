package org.hibyos.install.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.util.TimeZone

/**
 * The real thing: patches the official firmware with this module and with the Python patcher
 * of the repository, run at test time, and compares the two results byte for byte.
 *
 * The firmware belongs to HiBy and is never part of this repository, so the test only runs
 * when it is told where the pieces are:
 *   HIBYOS_FIRMWARE       the official r3ii_2025.upt
 *   HIBYOS_REPO           the HiByOS repository (tools/, overlay/)
 *   HIBYOS_PYTHON         a Python with pycdlib (default "python")
 *   HIBYOS_EXPORT_PYTHON  a Python with Pillow, for the SD-card files (default "python")
 *   HIBYOS_SD             optional: a folder that contains HiByOS/ (skips make_payload.py)
 *   HIBYOS_WORK           optional: where to put the results
 */
class RealFirmwareTest {
    private val buildId = "0123456789abcdef0123456789abcdef"
    private val epoch = 1767225600L

    @Test
    fun sameResultAsThePythonPatcher() {
        val fw = System.getenv("HIBYOS_FIRMWARE")
        val repoPath = System.getenv("HIBYOS_REPO")
        assumeTrue("HIBYOS_FIRMWARE and HIBYOS_REPO are not set", fw != null && repoPath != null)
        val firmware = File(fw!!)
        val repo = File(repoPath!!)
        val python = System.getenv("HIBYOS_PYTHON") ?: "python"
        val exportPython = System.getenv("HIBYOS_EXPORT_PYTHON") ?: "python"
        val work = TestUtil.tempDir("real")

        // 1. the bundle, generated now from the Python definitions
        val bundleFile = File(work, "bundle.zip")
        val export = ArrayList(listOf(exportPython, File(repo, "android/hibyos-install/tools/export_bundle.py").path,
            "--repo", repo.path, "--out", bundleFile.path, "--work", File(work, "sd").path,
            "--index", File(work, "index.json").path))
        System.getenv("HIBYOS_SD")?.let { export += listOf("--sd", it) }
        println(TestUtil.run(export, mapOf("PYTHONIOENCODING" to "utf-8")))

        // 2. the Python patcher, with a fixed build id and date
        val pyOut = File(work, "py")
        val t0 = System.currentTimeMillis()
        val log = TestUtil.run(listOf(python, File(repo, "tools/hibyos_patcher.py").path, firmware.path,
            "--out-dir", pyOut.path, "--no-pause", "--build-id", buildId),
            mapOf("SOURCE_DATE_EPOCH" to epoch.toString(), "PYTHONIOENCODING" to "utf-8"))
        val pyMs = System.currentTimeMillis() - t0
        println(log)
        val pyFile = File(pyOut, "r3ii_2025.upt")
        assertTrue(pyFile.isFile)

        // 3. this module
        val ktFile = File(work, "kt/r3ii_2025.upt")
        ktFile.parentFile.mkdirs()
        val stages = LinkedHashMap<Stage, Double>()
        var last = -1.0
        var monotonic = true
        val t1 = System.currentTimeMillis()
        val result = Bundle.open(bundleFile).use { bundle ->
            bundle.verifyAll()
            val id = Patcher.identify(firmware)
            assertTrue("the test firmware is not the one the bundle supports", id.matches(bundle.firmware))
            assertEquals(bundle.firmware.sha256, id.sha256)
            assertEquals(bundle.firmware.size, id.size)
            Patcher.patch(firmware, bundle, ktFile, File(work, "tmp"),
                PatchOptions(buildId = buildId, epochSeconds = epoch,
                    utcOffsetSeconds = TimeZone.getDefault().getOffset(epoch * 1000) / 1000),
                PatchListener { stage, f ->
                    stages[stage] = f
                    val o = stage.overall(f)
                    if (o < last - 1e-9) monotonic = false
                    last = o
                })
        }
        val ktMs = System.currentTimeMillis() - t1

        // 4. compare
        val diff = TestUtil.firstDifference(pyFile, ktFile)
        val rt = Runtime.getRuntime()
        println("EQUIVALENCE python=${pyFile.length()} bytes md5 ${TestUtil.md5Of(pyFile)}  " +
            "kotlin=${ktFile.length()} bytes md5 ${result.md5}")
        println("EQUIVALENCE first difference at: $diff (-1 = identical)   python $pyMs ms, kotlin $ktMs ms, " +
            "max heap ${rt.maxMemory() shr 20} MB, heap in use at the end ${(rt.totalMemory() - rt.freeMemory()) shr 20} MB")
        println("EQUIVALENCE rootfs ${result.rootfsSize} of ${result.rootfsLimit} bytes, md5 ${result.rootfsMd5}; " +
            "${result.entries} entries, ${result.files} files verified, ${result.overlayFiles} overlay files; " +
            "sha256 ${result.sha256}")
        assertEquals("the two results differ at byte $diff", -1L, diff)
        assertEquals(pyFile.length(), result.size)
        assertEquals(TestUtil.md5Of(pyFile), result.md5)
        assertEquals(buildId, result.buildId)
        assertTrue("progress went backwards", monotonic)
        assertEquals(Stage.values().toSet(), stages.keys)
        for ((s, f) in stages) assertEquals("stage $s did not finish", 1.0, f, 1e-9)
        assertTrue(File(work, "tmp").list().isNullOrEmpty())
        assertTrue(!File(ktFile.path + ".part").exists())
    }
}
