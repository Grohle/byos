package org.hibyos.install.core

import java.io.File
import java.io.FileOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * A made-up "firmware" and made-up bundles for the tests that must run everywhere: a
 * container with a fake kernel and the generated root filesystem of synthetic.squashfs.
 * Nothing here comes from HiBy.
 */
internal object Synthetic {
    const val CHUNK = 65536
    val kernel: ByteArray by lazy { TestData.gen("copies", 9001, 200000) }

    /** synthetic.squashfs without the special files (the real firmware has none either). */
    val rootfs: ByteArray by lazy {
        val img = SquashImage(ArraySource(TestData.resource("synthetic.squashfs")))
        val root = img.readTree()
        root.children!!.remove("dev")
        val sink = ArraySink()
        SquashWriter.write(root, img, sink)
        val raw = sink.toByteArray()
        raw.copyOf((raw.size + 4095) / 4096 * 4096)
    }

    fun updateIn(kernel: ByteArray, rootfs: ByteArray) =
        "ota_version=0\n\nimg_type=kernel\nimg_name=xImage\nimg_size=${kernel.size}\nimg_md5=${md5Hex(kernel)}\n\n" +
            "img_type=rootfs\nimg_name=rootfs.squashfs\nimg_size=${rootfs.size}\nimg_md5=${md5Hex(rootfs)}\n"

    /** Writes the made-up firmware; [damage] may change the files before they are stored. */
    fun firmware(file: File, rootfs: ByteArray = this.rootfs, updateIn: String? = null,
                 damage: ((MutableList<IsoInput>) -> Unit)? = null): File {
        val files = ArrayList<IsoInput>()
        files += IsoInput("ota_config.in", ArraySource("current_version=0\n".toByteArray()))
        files += IsoInput("ota_v0/ota_v0.ok", ArraySource("ok\n".toByteArray()))
        files += IsoInput(Upt.UPDATE_IN, ArraySource((updateIn ?: updateIn(kernel, rootfs)).toByteArray()))
        files += Upt.chunkImage("xImage", ArraySource(kernel), CHUNK).files
        files += Upt.chunkImage("rootfs.squashfs", ArraySource(rootfs), CHUNK).files
        damage?.invoke(files)
        file.parentFile.mkdirs()
        FileOutputStream(file).use { UptIsoWriter.write(it, files, 1700000000, 0) }
        return file
    }

    class BundleSpec(
        var format: Int = 1,
        var firmwareMd5: String = "",
        var firmwareSha256: String? = null,
        var firmwareSize: Long? = null,
        var steps: String = """[{"op":"normalize"},{"op":"overlay"},{"op":"build_id","path":"etc/build"}]""",
        val overlay: MutableList<Triple<String, ByteArray, Int>> = ArrayList(),
        val sd: MutableList<Pair<String, ByteArray>> = ArrayList(),
        val notices: MutableList<Triple<String, String, ByteArray>> = ArrayList(),
        var rootfsLimit: Long = 4L shl 20,
        var extraFirmware: String = "",
        /** Lets a test store something else than what bundle.json promises. */
        var stored: (String, ByteArray) -> ByteArray? = { _, b -> b },
    )

    fun bundleJson(s: BundleSpec): String {
        val fw = StringBuilder("""{"label":"Test player, test firmware","file":"test.upt","version":"9.9","md5":"${s.firmwareMd5}"""")
        s.firmwareSha256?.let { fw.append(""","sha256":"$it"""") }
        s.firmwareSize?.let { fw.append(""","size":$it""") }
        fw.append(s.extraFirmware).append("}")
        val ov = s.overlay.joinToString(",") { (p, d, m) ->
            """{"path":${Json.quote(p)},"mode":$m,"size":${d.size},"sha256":"${sha256Hex(d)}"}"""
        }
        val sd = s.sd.joinToString(",") { (p, d) -> """{"path":${Json.quote(p)},"size":${d.size},"sha256":"${sha256Hex(d)}"}""" }
        val no = s.notices.joinToString(",") { (p, t, _) -> """{"path":${Json.quote(p)},"title":${Json.quote(t)}}""" }
        return """{"format":${s.format},"hibyos_version":"9.9.9","generated_from":"test","firmware":$fw,""" +
            """"chunk":$CHUNK,"align":4096,"rootfs_limit":${s.rootfsLimit},"out_name":"test.upt",""" +
            """"steps":${s.steps},"overlay":[$ov],"sd":[$sd],"notices":[$no]}"""
    }

    fun bundle(file: File, s: BundleSpec, json: String = bundleJson(s)): File {
        file.parentFile.mkdirs()
        ZipOutputStream(FileOutputStream(file)).use { z ->
            fun put(name: String, data: ByteArray) {
                val d = s.stored(name, data) ?: return
                z.putNextEntry(ZipEntry(name))
                z.write(d)
                z.closeEntry()
            }
            put("bundle.json", json.toByteArray())
            for ((p, d, _) in s.overlay) put("overlay/$p", d)
            for ((p, d) in s.sd) put("sd/$p", d)
            for ((p, _, d) in s.notices) put("notices/$p", d)
        }
        return file
    }
}
