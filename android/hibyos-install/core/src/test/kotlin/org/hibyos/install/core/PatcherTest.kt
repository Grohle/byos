package org.hibyos.install.core

import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import java.io.File

/** The whole patching run, every kind of step and every way it can fail, on made-up data. */
class PatcherTest {
    private lateinit var dir: File
    private lateinit var fw: File
    private lateinit var out: File
    private lateinit var tmp: File

    @Before
    fun setUp() {
        dir = TestUtil.tempDir("patcher-" + System.nanoTime())
        fw = Synthetic.firmware(File(dir, "in/firmware.upt"))
        out = File(dir, "out/test.upt")
        tmp = File(dir, "tmp")
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    private fun spec(block: Synthetic.BundleSpec.() -> Unit = {}): Synthetic.BundleSpec {
        val id = Patcher.identify(fw)
        return Synthetic.BundleSpec(firmwareMd5 = id.md5, firmwareSha256 = id.sha256, firmwareSize = id.size).apply(block)
    }

    private fun open(s: Synthetic.BundleSpec) = Bundle.open(Synthetic.bundle(File(dir, "bundle-${System.nanoTime()}.zip"), s))

    private fun expectFailure(reason: PatchException.Reason, s: Synthetic.BundleSpec, input: File = fw,
                              options: PatchOptions = PatchOptions(buildId = "abc")): PatchException {
        try {
            open(s).use { Patcher.patch(input, it, out, tmp, options) }
        } catch (e: PatchException) {
            assertEquals(e.message, reason, e.reason)
            assertFalse("a .part file was left behind", File(out.path + ".part").exists())
            assertTrue("temporary files were left behind", tmp.list().isNullOrEmpty())
            return e
        }
        throw AssertionError("patching should have failed with $reason")
    }

    private val allSteps = """[
        {"op":"normalize"},
        {"op":"overlay"},
        {"op":"build_id","path":"etc/build"},
        {"op":"text","path":"etc/config.txt","old":"wifi","new":"WIFI!","count":COUNT},
        {"op":"text","path":"small.txt","edits":[{"old_hex":"68696279","new":"HiBy","count":HIBY},{"old":"\n","new":"\r\n","count":NL}]},
        {"op":"bin","path":"lib/driver.ko","md5":"KOMD5","edits":[{"off":1000,"old":"KOOLD","new":"deadbeef"},{"off":0,"old":"KO0","new":"00"}]},
        {"op":"remove","path":"many/f0003"},
        {"op":"remove","path":"many/not-there","optional":true},
        {"op":"remove","path":"emptydir"},
        {"op":"mode","path":"bin/tool","mode":448}
    ]"""

    private fun count(hay: ByteArray, needle: String): Int {
        val s = String(hay, Charsets.ISO_8859_1)
        var n = 0
        var i = s.indexOf(needle)
        while (i >= 0) { n++; i = s.indexOf(needle, i + needle.length) }
        return n
    }

    @Test
    fun appliesEveryKindOfStep() {
        val config = TestData.gen("text", 16, 2000)
        val small = TestData.gen("text", 11, 300)
        val ko = TestData.gen("copies", 18, 150000)
        val newTool = TestData.gen("runs", 501, 30000)
        val added = TestData.gen("copies", 502, 180000)
        val s = spec {
            steps = allSteps.replace("COUNT", count(config, "wifi").toString())
                .replace("HIBY", count(small, "hiby").toString()).replace("NL", count(small, "\n").toString())
                .replace("KOMD5", md5Hex(ko)).replace("KOOLD", Hex.of(ko.copyOfRange(1000, 1004)))
                .replace("KO0", Hex.of(ko.copyOfRange(0, 1)))
            overlay += Triple("bin/tool", newTool, 420)                 // replaces: keeps mode and owner
            overlay += Triple("usr/share/new/added.bin", added, 493)    // new folders and file
            overlay += Triple("dir/hard", "no longer a link\n".toByteArray(), 420)
            sd += "ByOS/payload/VERSION" to "9.9.9\n".toByteArray()
        }
        val seen = HashSet<Stage>()
        val r = open(s).use { b ->
            Patcher.patch(fw, b, out, tmp, PatchOptions(buildId = "build-1", epochSeconds = 1700000000, utcOffsetSeconds = 0)) { st, _ -> seen += st }
        }
        assertEquals(Stage.values().toSet(), seen)
        assertEquals(out.length(), r.size)
        assertEquals(TestUtil.md5Of(out), r.md5)
        assertEquals(3, r.overlayFiles)
        assertEquals("9.9.9", r.hibyosVersion)
        assertTrue(tmp.list().isNullOrEmpty())

        FileSource(out).use { src ->
            val files = IsoReader.list(src)
            assertArrayEquals("current_version=0\n".toByteArray(), src.bytes(files["ota_config.in"]!!.offset, 18))
            val upd = String(src.bytes(files[Upt.UPDATE_IN]!!.offset, files[Upt.UPDATE_IN]!!.size.toInt()))
            val imgs = Upt.parseUpdateIn(upd)
            val k = imgs.first { it.type == "kernel" }
            val rf = imgs.first { it.type == "rootfs" }
            assertEquals(md5Hex(Synthetic.kernel), k.md5)
            val kdata = Upt.openImage(src, files, k.name, k.md5)
            assertArrayEquals(Synthetic.kernel, kdata.bytes(0, kdata.size.toInt()))
            assertEquals(r.rootfsMd5, rf.md5)
            assertEquals(r.rootfsSize, rf.size)
            assertEquals(0L, rf.size % 4096)
            val img = SquashImage(Upt.openImage(src, files, rf.name, rf.md5))
            val root = img.readTree()
            fun node(p: String): Inode? {
                var n: Inode? = root
                for (part in p.split('/')) n = n?.children?.get(part)
                return n
            }
            assertArrayEquals("build-1\n".toByteArray(), img.readFile(node("etc/build")!!))
            assertEquals(String(config).replace("wifi", "WIFI!"), String(img.readFile(node("etc/config.txt")!!)))
            assertEquals(String(small).replace("hiby", "HiBy").replace("\n", "\r\n"), String(img.readFile(node("small.txt")!!)))
            val wantKo = ko.clone()
            byteArrayOf(0xde.toByte(), 0xad.toByte(), 0xbe.toByte(), 0xef.toByte()).copyInto(wantKo, 1000)
            wantKo[0] = 0
            assertArrayEquals(wantKo, img.readFile(node("lib/driver.ko")!!))
            assertNull(node("many/f0003"))
            assertNull(node("emptydir"))
            assertTrue(node("many/f0004") != null)
            // replaced file: new content, mode set by the later "mode" step, owner of the original
            val tool = node("bin/tool")!!
            assertArrayEquals(newTool, img.readFile(tool))
            assertEquals(448, tool.mode)
            assertEquals(1000L, tool.uid)
            assertEquals(100L, tool.gid)
            // new file in new folders: root, mode of the bundle
            val add = node("usr/share/new/added.bin")!!
            assertArrayEquals(added, img.readFile(add))
            assertEquals(493, add.mode)
            assertEquals(0L, add.uid)
            assertEquals(493, node("usr/share/new")!!.mode)
            // the overlay broke the hard link; the text patch went to the other name only
            assertArrayEquals("no longer a link\n".toByteArray(), img.readFile(node("dir/hard")!!))
            // normalize: symlinks and / belong to root
            assertEquals(0L, node("dir/link")!!.uid)
            assertEquals(511, node("dir/link")!!.mode)
            assertEquals(0L, root.uid)
            // untouched data is still there
            assertArrayEquals(TestData.gen("copies", 13, 300000), img.readFile(node("multi.bin")!!))
            assertArrayEquals(ByteArray(400000), img.readFile(node("zeros.bin")!!))
        }
    }

    @Test
    fun sameInputGivesTheSameBytes() {
        val s = spec { overlay += Triple("a/b", TestData.gen("text", 1, 999), 420) }
        val o = PatchOptions(buildId = "x", epochSeconds = 1700000000, utcOffsetSeconds = 7200)
        val a = open(s).use { Patcher.patch(fw, it, out, tmp, o) }
        val second = File(dir, "out2/test.upt")
        val b = open(s).use { Patcher.patch(fw, it, second, tmp, o) }
        assertEquals(a.sha256, b.sha256)
        assertEquals(-1L, TestUtil.firstDifference(out, second))
        // and a random build id when none is given
        val c = open(s).use { Patcher.patch(fw, it, second, tmp) }
        assertTrue(Regex("[0-9a-f]{32}").matches(c.buildId))
    }

    @Test
    fun refusesAnotherFile() {
        val other = Synthetic.firmware(File(dir, "in/other.upt"), rootfs = Synthetic.rootfs.clone().also { it[it.size - 1] = 1 })
        val e = expectFailure(PatchException.Reason.WRONG_FIRMWARE, spec(), input = other)
        assertEquals(TestUtil.md5Of(other), e.detail)
        assertFalse(out.exists())
        // not a container at all
        val junk = File(dir, "in/junk.bin").also { it.writeBytes(TestData.gen("random", 8, 50000)) }
        expectFailure(PatchException.Reason.WRONG_FIRMWARE, spec(), input = junk)
        // only the MD5 matches (a bundle with a wrong SHA-256 for it)
        expectFailure(PatchException.Reason.WRONG_FIRMWARE, spec { firmwareSha256 = "00".repeat(32) })
        expectFailure(PatchException.Reason.WRONG_FIRMWARE, spec { firmwareSize = 5 })
    }

    @Test
    fun keepsAnOlderResultWhenItFails() {
        out.parentFile.mkdirs()
        out.writeText("previous result")
        expectFailure(PatchException.Reason.DAMAGED, spec {
            steps = """[{"op":"text","path":"etc/config.txt","old":"this text is not there","new":"x"}]"""
        })
        assertEquals("previous result", out.readText())
    }

    @Test
    fun reportsUnexpectedFirmwareContents() {
        fun damaged(steps: String) = expectFailure(PatchException.Reason.DAMAGED, spec { this.steps = steps })
        damaged("""[{"op":"text","path":"etc/nothing.txt","old":"a","new":"b"}]""")
        damaged("""[{"op":"text","path":"etc","old":"a","new":"b"}]""")
        damaged("""[{"op":"text","path":"etc/config.txt","old":"wifi","new":"b","count":1}]""")
        damaged("""[{"op":"text","path":"etc/config.txt","md5":"${"0".repeat(32)}","old":"wifi","new":"b","count":1}]""")
        damaged("""[{"op":"bin","path":"lib/driver.ko","md5":"${"0".repeat(32)}","edits":[]}]""")
        damaged("""[{"op":"bin","path":"lib/driver.ko","edits":[{"off":10,"old":"ffffffffffffffffffff","new":"00000000000000000000"}]}]""")
        damaged("""[{"op":"bin","path":"lib/driver.ko","edits":[{"off":149999,"old":"0000","new":"0000"}]}]""")
        damaged("""[{"op":"remove","path":"many/not-there"}]""")
        damaged("""[{"op":"remove","path":"many"}]""")
        damaged("""[{"op":"mode","path":"nope","mode":1}]""")
        damaged("""[{"op":"build_id","path":"missing-folder/build"}]""")
        damaged("""[{"op":"build_id","path":"etc"}]""")
        expectFailure(PatchException.Reason.DAMAGED, spec {
            steps = """[{"op":"overlay"}]"""
            overlay += Triple("small.txt/inside", byteArrayOf(1), 420)
        })
        expectFailure(PatchException.Reason.DAMAGED, spec {
            steps = """[{"op":"overlay"}]"""
            overlay += Triple("dir/link", byteArrayOf(1), 420)
        })
        expectFailure(PatchException.Reason.BUNDLE, spec {
            steps = """[{"op":"bin","path":"lib/driver.ko","edits":[{"off":0,"old":"00","new":"0000"}]}]"""
        })
    }

    @Test
    fun reportsDamageInsideTheContainer() {
        // a chunk that does not match its MD5 (the bundle is made to accept the file as a whole)
        fw = Synthetic.firmware(File(dir, "in/damaged.upt")) { files ->
            val i = files.indexOfFirst { it.path.startsWith("ota_v0/rootfs.squashfs.0001.") }
            val bad = files[i].data.bytes(0, files[i].data.size.toInt())
            bad[100] = (bad[100] + 1).toByte()
            files[i] = IsoInput(files[i].path, ArraySource(bad))
        }
        assertTrue(expectFailure(PatchException.Reason.DAMAGED, spec()).message!!.contains("chunk 1"))
        // a missing chunk
        fw = Synthetic.firmware(File(dir, "in/missing.upt")) { files ->
            files.removeAt(files.indexOfFirst { it.path.startsWith("ota_v0/xImage.0002.") })
        }
        expectFailure(PatchException.Reason.DAMAGED, spec())
        // no image list
        fw = Synthetic.firmware(File(dir, "in/nolist.upt")) { files -> files.removeAll { it.path == Upt.UPDATE_IN } }
        expectFailure(PatchException.Reason.DAMAGED, spec())
        // an image list without rootfs
        fw = Synthetic.firmware(File(dir, "in/norootfs.upt"), updateIn = "img_type=kernel\nimg_name=xImage\nimg_size=200000\nimg_md5=${md5Hex(Synthetic.kernel)}\n")
        expectFailure(PatchException.Reason.DAMAGED, spec())
        // a root filesystem that is not SquashFS
        fw = Synthetic.firmware(File(dir, "in/notsquash.upt"), rootfs = TestData.gen("random", 77, 300000))
        expectFailure(PatchException.Reason.DAMAGED, spec())
        // special files are refused by "normalize" (the real firmware has none)
        val withDev = TestData.resource("synthetic.squashfs")
        fw = Synthetic.firmware(File(dir, "in/dev.upt"), rootfs = withDev)
        assertTrue(expectFailure(PatchException.Reason.DAMAGED, spec()).message!!.contains("special file"))
        // a file cut short keeps a clear message
        fw = Synthetic.firmware(File(dir, "in/whole.upt"))
        val cut = File(dir, "in/cut.upt").also { it.writeBytes(fw.readBytes().copyOf(120000)) }
        fw = cut
        expectFailure(PatchException.Reason.DAMAGED, spec())
    }

    private fun ident(b: ByteArray) = """{"size":${b.size},"sha256":"${sha256Hex(b)}"}"""

    private fun withoutCr(b: ByteArray) = b.filter { it != 0x0D.toByte() }.toByteArray()

    /** An "edit" step for small.txt (300 bytes of text, also seen through dir/hard). */
    private fun smallEdit(stock: String? = null, result: String? = null, ops: String? = null): String {
        val small = TestData.gen("text", 11, 300)
        val want = small.copyOfRange(0, 100) + "ours\nand ours".toByteArray() + small.copyOfRange(250, 300)
        return """{"op":"edit","path":"small.txt","stock":${stock ?: ident(small)},"result":${result ?: ident(want)},
            "ops":${ops ?: """[{"copy":[0,100]},{"add":["ours","and ours"]},{"copy":[250,50]}]"""}}"""
    }

    @Test
    fun rebuildsOfficialFilesFromEdits() {
        val small = TestData.gen("text", 11, 300)
        val random = TestData.gen("random", 14, 70000)
        val base = withoutCr(random)
        assertTrue("the test needs CR bytes to delete", base.size < random.size)
        val wantSmall = small.copyOfRange(0, 100) + "ours\nand ours".toByteArray() + small.copyOfRange(250, 300)
        val wantRandom = "first line\n".toByteArray() + base.copyOfRange(10, 5010) + "café".toByteArray(Charsets.UTF_8) +
            base.copyOfRange(base.size - 7, base.size)
        val s = spec {
            steps = """[{"op":"normalize"},{"op":"overlay","may_replace":[".png",".jpg"]},
                ${smallEdit()},
                {"op":"edit","path":"random.bin","stock":${ident(random)},"strip_cr":true,"result":${ident(wantRandom)},
                 "ops":[{"add":["first line",""]},{"copy":[10,5000]},{"add":["café"]},{"copy":[${base.size - 7},7]}]},
                {"op":"build_id","path":"etc/build"}]"""
            overlay += Triple("usr/share/new/added.bin", byteArrayOf(1, 2, 3), 420)     // a new file is always fine
        }
        val r = open(s).use { Patcher.patch(fw, it, out, tmp, PatchOptions(buildId = "b")) }
        assertEquals(2, r.editedFiles)
        assertEquals(1, r.overlayFiles)
        FileSource(out).use { src ->
            val files = IsoReader.list(src)
            val rf = Upt.parseUpdateIn(String(src.bytes(files[Upt.UPDATE_IN]!!.offset, files[Upt.UPDATE_IN]!!.size.toInt()))).first { it.type == "rootfs" }
            val img = SquashImage(Upt.openImage(src, files, rf.name, rf.md5))
            val root = img.readTree()
            fun node(p: String): Inode {
                var n: Inode? = root
                for (part in p.split('/')) n = n?.children?.get(part)
                return n!!
            }
            assertArrayEquals(wantSmall, img.readFile(node("small.txt")))
            assertArrayEquals(wantRandom, img.readFile(node("random.bin")))
            // like an overlay file: mode and owner of the official file, and the other name of
            // a hard link keeps the official content
            assertEquals(420, node("small.txt").mode)
            assertEquals(1000L, node("small.txt").uid)
            assertEquals(1000L, node("small.txt").gid)
            assertArrayEquals(small, img.readFile(node("dir/hard")))
        }
    }

    @Test
    fun refusesEditsThatDoNotFitTheFirmware() {
        fun steps(vararg more: String) = "[" + (listOf("""{"op":"normalize"}""") + more).joinToString(",") + "]"
        val small = TestData.gen("text", 11, 300)
        // not the official file the edit was written for
        expectFailure(PatchException.Reason.DAMAGED, spec { steps = steps(smallEdit(stock = """{"size":300,"sha256":"${"0".repeat(64)}"}""")) })
        expectFailure(PatchException.Reason.DAMAGED, spec { steps = steps(smallEdit(stock = """{"size":299,"sha256":"${sha256Hex(small)}"}""")) })
        // missing, a folder, a link
        for (path in listOf("etc/nothing", "etc", "dir/link", "nowhere/at/all")) {
            expectFailure(PatchException.Reason.DAMAGED, spec { steps = steps(smallEdit().replace("\"small.txt\"", "\"$path\"")) })
        }
        // a bundle whose edit does not give the promised file, or reads outside the official one
        expectFailure(PatchException.Reason.BUNDLE, spec { steps = steps(smallEdit(result = """{"size":163,"sha256":"${"0".repeat(64)}"}""")) })
        expectFailure(PatchException.Reason.BUNDLE, spec { steps = steps(smallEdit(ops = """[{"copy":[0,100]},{"add":["ours"]}]""")) })
        expectFailure(PatchException.Reason.BUNDLE, spec { steps = steps(smallEdit(ops = """[{"copy":[250,51]}]""")) })
        // an edited file is never also shipped whole
        val e = expectFailure(PatchException.Reason.BUNDLE, spec {
            steps = steps("""{"op":"overlay"}""", smallEdit())
            overlay += Triple("small.txt", byteArrayOf(1), 420)
        })
        assertTrue(e.message, e.message!!.contains("small.txt"))
        assertFalse(out.exists())
    }

    @Test
    fun theOverlayMayOnlyReplaceImages() {
        // the first pass adds the files, the second meets them as files of the firmware
        fun twice(name: String) = spec {
            steps = """[{"op":"overlay"},{"op":"overlay","may_replace":[".png",".jpg",".jpeg"]}]"""
            overlay += Triple(name, byteArrayOf(1, 2, 3), 420)
        }
        open(twice("pics/Logo.PNG")).use { Patcher.patch(fw, it, out, tmp, PatchOptions(buildId = "b")) }
        open(twice("pics/photo.jpeg")).use { Patcher.patch(fw, it, out, tmp, PatchOptions(buildId = "b")) }
        out.delete()
        for (name in listOf("usr/bin/player.sh", "layout/a.view", "pics/png", "pics/a.png.bak")) {
            val e = expectFailure(PatchException.Reason.BUNDLE, twice(name))
            assertTrue(e.message, e.message!!.contains("official firmware"))
        }
        // a file of the made-up firmware itself
        expectFailure(PatchException.Reason.BUNDLE, spec {
            steps = """[{"op":"overlay","may_replace":[".png"]}]"""
            overlay += Triple("small.txt", byteArrayOf(1), 420)
        })
        assertFalse(out.exists())
    }

    @Test
    fun refusesAResultThatDoesNotFit() {
        val e = expectFailure(PatchException.Reason.TOO_BIG, spec {
            rootfsLimit = Synthetic.rootfs.size.toLong()
            overlay += Triple("big.bin", TestData.gen("random", 5, 100000), 420)
        })
        assertTrue(e.message!!.contains("too big"))
        assertFalse(out.exists())
    }

    @Test
    fun reportsADamagedBundle() {
        // a file of the bundle that is not what bundle.json says
        expectFailure(PatchException.Reason.BUNDLE, spec {
            overlay += Triple("x", "right".toByteArray(), 420)
            stored = { name, b -> if (name == "overlay/x") "wrong".toByteArray() else b }
        })
        fun mustNotOpen(what: String, s: Synthetic.BundleSpec, json: String? = null) {
            try {
                Bundle.open(Synthetic.bundle(File(dir, "bad.zip"), s, json ?: Synthetic.bundleJson(s))).close()
                fail("$what was accepted")
            } catch (e: BundleException) {
                // expected
            }
        }
        mustNotOpen("a newer format", spec { format = 2 })
        try {
            open(spec { steps = """[{"op":"teleport","path":"x"}]""" }).close()
            fail("an unknown step was accepted")
        } catch (e: BundleException) {
            assertTrue(e.message!!.contains("newer version of the app"))
        }
        mustNotOpen("a path that leaves the tree", spec { overlay += Triple("../evil", byteArrayOf(1), 420) })
        mustNotOpen("an absolute path", spec { steps = """[{"op":"remove","path":"/etc/passwd"}]""" })
        mustNotOpen("a path with . in it", spec { sd += "ByOS/./x" to byteArrayOf(1) })
        mustNotOpen("a missing file", spec {
            overlay += Triple("x", byteArrayOf(1), 420)
            stored = { name, b -> if (name == "overlay/x") null else b }
        })
        mustNotOpen("a file with another size", spec {
            sd += "ByOS/a" to byteArrayOf(1, 2, 3)
            stored = { name, b -> if (name == "sd/ByOS/a") byteArrayOf(1) else b }
        })
        mustNotOpen("broken JSON", spec(), "{\"format\":1,")
        mustNotOpen("no steps", spec(), """{"format":1,"hibyos_version":"1","firmware":{"label":"a","file":"b","md5":"c"},"chunk":1,"align":1,"rootfs_limit":1,"out_name":"a"}""")
        mustNotOpen("bad hex", spec { steps = """[{"op":"bin","path":"a","edits":[{"off":0,"old":"zz","new":"00"}]}]""" })
        mustNotOpen("an edit with an unknown operation", spec { steps = "[" + smallEdit(ops = """[{"move":[0,1]}]""") + "]" })
        mustNotOpen("an edit with two operations in one", spec { steps = "[" + smallEdit(ops = """[{"copy":[0,1],"add":["x"]}]""") + "]" })
        mustNotOpen("an edit with a negative offset", spec { steps = "[" + smallEdit(ops = """[{"copy":[-1,5]}]""") + "]" })
        mustNotOpen("an edit that copies nothing", spec { steps = "[" + smallEdit(ops = """[{"copy":[0,0]}]""") + "]" })
        mustNotOpen("an edit with a number as text", spec { steps = "[" + smallEdit(ops = """[{"add":["a",5]}]""") + "]" })
        mustNotOpen("an edit with a bad checksum", spec { steps = "[" + smallEdit(result = """{"size":3,"sha256":"abc"}""") + "]" })
        mustNotOpen("an edit without the official checksum", spec { steps = "[" + smallEdit().replace("\"stock\"", "\"stick\"") + "]" })
        mustNotOpen("the same file edited twice", spec { steps = "[" + smallEdit() + "," + smallEdit() + "]" })
        mustNotOpen("a bad overlay rule", spec { steps = """[{"op":"overlay","may_replace":["png"]}]""" })
        mustNotOpen("an http link", spec { extraFirmware = ""","sources":["http://example.com/x"]""" })
        mustNotOpen("a bad output name", spec(), Synthetic.bundleJson(spec()).replace("\"out_name\":\"test.upt\"", "\"out_name\":\"../x\""))
        val notZip = File(dir, "notzip.zip").also { it.writeText("hello") }
        try {
            Bundle.open(notZip).close()
            fail("a text file was accepted as a bundle")
        } catch (e: BundleException) {
            // expected
        }
        // verifyAll finds a changed SD-card file
        val s = spec {
            sd += "ByOS/bin/tool" to "abc".toByteArray()
            stored = { name, b -> if (name == "sd/ByOS/bin/tool") "abd".toByteArray() else b }
        }
        open(s).use {
            try {
                it.verifyAll()
                fail("a changed SD-card file was accepted")
            } catch (e: BundleException) {
                assertTrue(e.message!!.contains("ByOS/bin/tool"))
            }
        }
    }

    @Test
    fun readsTheBundleFields() {
        val s = spec {
            sd += "ByOS/payload/VERSION" to "9.9.9\n".toByteArray()
            sd += "ByOS/bin/x" to ByteArray(1000)
            notices += Triple("LICENSE", "Licence", "text of the licence".toByteArray())
            extraFirmware = ""","sources":["https://example.com/a","https://example.org/b"],"page":"https://example.com/page""""
        }
        open(s).use { b ->
            assertEquals("9.9.9", b.hibyosVersion)
            assertEquals("Test player, test firmware", b.firmware.label)
            assertEquals("9.9", b.firmware.version)
            assertEquals(listOf("https://example.com/a", "https://example.org/b"), b.firmware.sources)
            assertEquals("https://example.com/page", b.firmware.page)
            assertEquals(1006L, b.sdSize)
            assertEquals("Licence", b.notices[0].title)
            assertEquals("text of the licence", b.readText(b.notices[0].entry))
            assertEquals(3, b.steps.size)
            b.verifyAll()
        }
    }

    @Test
    fun stopsWhenCancelled() {
        val s = spec { overlay += Triple("big.bin", TestData.gen("copies", 5, 400000), 420) }
        for (stopAt in Stage.values()) {
            var now: Stage? = null
            var hit = false
            try {
                open(s).use { b ->
                    Patcher.patch(fw, b, out, tmp, PatchOptions(buildId = "x", cancelled = { hit = hit || now == stopAt; hit })) { st, _ -> now = st }
                }
                fail("not cancelled at $stopAt")
            } catch (e: PatchException) {
                assertEquals("at $stopAt: ${e.message}", PatchException.Reason.CANCELLED, e.reason)
            }
            assertFalse(out.exists())
            assertFalse(File(out.path + ".part").exists())
            assertTrue(tmp.list().isNullOrEmpty())
        }
    }

    @Test
    fun refusesToOverwriteItsInput() {
        try {
            open(spec()).use { Patcher.patch(fw, it, fw, tmp) }
            fail("the input was overwritten")
        } catch (e: PatchException) {
            assertEquals(PatchException.Reason.STORAGE, e.reason)
        }
        assertTrue(fw.length() > 100000)
    }
}
