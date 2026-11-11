package org.hibyos.install.core

import java.io.Closeable
import java.io.File
import java.io.InputStream
import java.util.zip.ZipFile

/**
 * The ByOS bundle: a ZIP with bundle.json (what firmware is supported and the ordered list
 * of changes to make to it), the ByOS files that go into the root filesystem (overlay/...),
 * the files that go to the SD card (sd/...) and licence texts (notices/...).
 *
 * It is generated from the Python definitions by tools/export_bundle.py, so this app has no
 * list of files or patches of its own. It contains no HiBy firmware: only ByOS files and
 * the description of the edits.
 */
class BundleException(message: String) : Exception(message)

class FirmwareSpec(
    val label: String, val fileName: String, val md5: String, val sha256: String?, val size: Long?,
    /** Version of the official firmware, as HiBy names it ("1.3"). */
    val version: String = "",
    /** HiBy's own direct download links, tried in order. */
    val sources: List<String> = emptyList(),
    /** HiBy's page that publishes the firmware, for people to download it themselves. */
    val page: String? = null,
    /** What the stock "About" screen shows on its "BuildTime" line for this firmware ("Mar 2 2026"). */
    val build: String? = null,
)

class BundleFile(val path: String, val entry: String, val size: Long, val sha256: String, val mode: Int)

class Notice(val title: String, val entry: String)

class TextEdit(val old: ByteArray, val new: ByteArray, val count: Int)

class BinEdit(val offset: Int, val old: ByteArray, val new: ByteArray)

/** One piece of an edited official file: bytes kept from the official file, or text of ours. */
sealed class EditOp {
    class Copy(val offset: Int, val length: Int) : EditOp()
    class Add(val data: ByteArray) : EditOp()
}

sealed class Step {
    /** Owners/modes as tools/build.py leaves them (symlinks and / owned by root). */
    object Normalize : Step()

    /**
     * Adds or replaces the files of the bundle's overlay list. The overlay holds only files
     * written for ByOS: it may take the place of an official file only when the name ends in
     * one of [mayReplace] (our own artwork under HiBy's file name) and never of a file that an
     * [Edit] step builds. A null [mayReplace] means the bundle states no such rule.
     */
    class Overlay(val mayReplace: List<String>?) : Step()

    /**
     * Rebuilds the ByOS version of one official file from the copy inside the user's own
     * firmware (tools/vendor_edits.json): the bundle carries the text written for ByOS and
     * the places of the official file to keep, never the official text.
     */
    class Edit(
        val path: String, val stockSize: Long, val stockSha256: String, val stripCr: Boolean,
        val resultSize: Long, val resultSha256: String, val ops: List<EditOp>,
    ) : Step()

    /** Writes the build identifier to [path]. */
    class BuildId(val path: String) : Step()

    /** Replaces text in an existing file; each original text must appear exactly [TextEdit.count] times. */
    class Text(val path: String, val md5: String?, val sha256: String?, val edits: List<TextEdit>) : Step()

    /** Replaces bytes at fixed offsets of an existing file with a known MD5. */
    class Bin(val path: String, val md5: String?, val sha256: String?, val edits: List<BinEdit>) : Step()

    /** Removes a file or symlink (optionally only if it exists). */
    class Remove(val path: String, val optional: Boolean) : Step()

    /** Changes the permission bits of an existing entry. */
    class Mode(val path: String, val mode: Int) : Step()
}

class Bundle private constructor(private val zip: ZipFile, val file: File, json: Map<String, Any?>) : Closeable {
    val format: Int = Json.long(json, "format").toInt()
    val hibyosVersion: String = Json.str(json, "hibyos_version")
    val generatedFrom: String = Json.optStr(json, "generated_from") ?: ""
    val firmware: FirmwareSpec
    val chunk: Int = Json.long(json, "chunk").toInt()
    val align: Int = Json.long(json, "align").toInt()
    val rootfsLimit: Long = Json.long(json, "rootfs_limit")
    val outName: String = Json.str(json, "out_name")
    val steps: List<Step>
    val overlay: List<BundleFile>
    val sd: List<BundleFile>
    val notices: List<Notice>

    /** Where newer bundles are announced (a signed index), or null when this bundle names none. */
    val updateIndex: String?

    /** Where newer bundles may be downloaded from (links of the index must start with this). */
    val updatePrefix: String?

    /** Ed25519 public keys (hex) that may sign the index. */
    val updateKeys: List<String>

    /** Port and SD-card folder of the player's own Wi-Fi transfer. */
    val transferPort: Int
    val transferHome: String

    init {
        if (format != FORMAT) {
            throw BundleException("this ByOS bundle (format $format) needs a newer version of the app")
        }
        if (chunk <= 0 || align <= 0 || rootfsLimit <= 0) throw BundleException("bad sizes in the ByOS bundle")
        if (!Regex("[A-Za-z0-9._-]{1,64}").matches(outName)) throw BundleException("bad output name in the ByOS bundle")
        val fw = Json.obj(json["firmware"])
        firmware = FirmwareSpec(Json.str(fw, "label"), Json.str(fw, "file"), Json.str(fw, "md5").lowercase(),
            Json.optStr(fw, "sha256")?.lowercase(), if (fw["size"] == null) null else Json.long(fw, "size"),
            Json.optStr(fw, "version") ?: "",
            Json.arr(fw["sources"] ?: emptyList<Any?>()).map { httpsUrl(it as? String) },
            Json.optStr(fw, "page")?.let { httpsUrl(it) },
            Json.optStr(fw, "build")?.takeIf { Regex("[A-Za-z0-9 .:,/-]{1,32}").matches(it) })
        overlay = files(json["overlay"], "overlay/")
        sd = files(json["sd"], "sd/")
        notices = Json.arr(json["notices"] ?: emptyList<Any?>()).map {
            val m = Json.obj(it)
            val p = safePath(Json.str(m, "path"))
            val e = "notices/$p"
            if (zip.getEntry(e) == null) throw BundleException("the ByOS bundle is incomplete ($e)")
            Notice(Json.optStr(m, "title") ?: p, e)
        }
        steps = Json.arr(json["steps"]).map { step(Json.obj(it)) }
        val edited = steps.filterIsInstance<Step.Edit>().map { it.path }
        if (edited.toSet().size != edited.size) throw BundleException("repeated edit in the ByOS bundle")
        val up = json["updates"]?.let { Json.obj(it) }
        updateIndex = up?.let { Json.optStr(it, "index") }?.let { httpsUrl(it) }
        updatePrefix = up?.let { Json.optStr(it, "prefix") }?.let { httpsUrl(it) }
        updateKeys = Json.arr(up?.get("keys") ?: emptyList<Any?>()).map {
            val k = (it as? String)?.lowercase() ?: ""
            if (!Regex("[0-9a-f]{64}").matches(k)) throw BundleException("bad key in the ByOS bundle")
            k
        }
        val tr = json["transfer"]?.let { Json.obj(it) }
        transferPort = tr?.let { Json.optLong(it, "port", 4399).toInt() } ?: 4399
        transferHome = tr?.let { Json.optStr(it, "home") } ?: "/data/mnt/sd_0/"
        if (transferPort !in 1..65535 || !Regex("(/[A-Za-z0-9._-]+)+/").matches(transferHome)) {
            throw BundleException("bad transfer settings in the ByOS bundle")
        }
    }

    private fun files(v: Any?, prefix: String): List<BundleFile> = Json.arr(v ?: emptyList<Any?>()).map {
        val m = Json.obj(it)
        val p = safePath(Json.str(m, "path"))
        val e = prefix + p
        val ze = zip.getEntry(e) ?: throw BundleException("the ByOS bundle is incomplete ($e)")
        val size = Json.long(m, "size")
        if (ze.size != size) throw BundleException("the ByOS bundle is damaged ($e)")
        BundleFile(p, e, size, Json.str(m, "sha256").lowercase(), Json.optLong(m, "mode", 420).toInt())
    }

    private fun bytesOf(m: Map<String, Any?>, key: String): ByteArray {
        Json.optStr(m, key + "_hex")?.let { return Hex.parse(it) }
        return Json.str(m, key).toByteArray(Charsets.UTF_8)
    }

    private fun step(m: Map<String, Any?>): Step = when (val op = Json.str(m, "op")) {
        "normalize" -> Step.Normalize
        "overlay" -> Step.Overlay(m["may_replace"]?.let { v ->
            Json.arr(v).map {
                val ext = (it as? String)?.lowercase() ?: ""
                if (!Regex("\\.[a-z0-9]{1,8}").matches(ext)) throw BundleException("bad overlay rule in the ByOS bundle")
                ext
            }
        })
        "edit" -> {
            val path = safePath(Json.str(m, "path"))
            fun ident(key: String): Pair<Long, String> {
                val o = Json.obj(m[key])
                val size = Json.long(o, "size")
                val sha = Json.str(o, "sha256").lowercase()
                if (size < 0 || size > MAX_FILE || !Regex("[0-9a-f]{64}").matches(sha)) {
                    throw BundleException("bad edit in the ByOS bundle ($path)")
                }
                return size to sha
            }
            val stock = ident("stock")
            val result = ident("result")
            val strip = when (val v = m["strip_cr"]) {
                null -> false
                is Boolean -> v
                else -> throw BundleException("bad edit in the ByOS bundle ($path)")
            }
            val ops = Json.arr(m["ops"]).map {
                val o = Json.obj(it)
                if (o.size != 1) throw BundleException("bad edit in the ByOS bundle ($path)")
                val (kind, arg) = o.entries.first()
                when (kind) {
                    "copy" -> {
                        val a = Json.arr(arg)
                        val off = a.getOrNull(0) as? Long
                        val len = a.getOrNull(1) as? Long
                        if (a.size != 2 || off == null || len == null || off < 0 || len <= 0 || off + len > MAX_FILE) {
                            throw BundleException("bad edit in the ByOS bundle ($path)")
                        }
                        EditOp.Copy(off.toInt(), len.toInt())
                    }
                    "add" -> EditOp.Add(Json.arr(arg).joinToString("\n") {
                        it as? String ?: throw BundleException("bad edit in the ByOS bundle ($path)")
                    }.toByteArray(Charsets.UTF_8))
                    else -> throw BundleException("this ByOS bundle needs a newer version of the app (unknown edit \"$kind\")")
                }
            }
            Step.Edit(path, stock.first, stock.second, strip, result.first, result.second, ops)
        }
        "build_id" -> Step.BuildId(safePath(Json.str(m, "path")))
        "text" -> {
            val edits = if (m["edits"] != null) Json.arr(m["edits"]).map { Json.obj(it) } else listOf(m)
            Step.Text(safePath(Json.str(m, "path")), Json.optStr(m, "md5")?.lowercase(),
                Json.optStr(m, "sha256")?.lowercase(),
                edits.map { TextEdit(bytesOf(it, "old"), bytesOf(it, "new"), Json.optLong(it, "count", 1).toInt()) })
        }
        "bin" -> Step.Bin(safePath(Json.str(m, "path")), Json.optStr(m, "md5")?.lowercase(),
            Json.optStr(m, "sha256")?.lowercase(),
            Json.arr(m["edits"]).map {
                val e = Json.obj(it)
                BinEdit(Json.long(e, "off").toInt(), Hex.parse(Json.str(e, "old")), Hex.parse(Json.str(e, "new")))
            })
        "remove" -> Step.Remove(safePath(Json.str(m, "path")), m["optional"] == true)
        "mode" -> Step.Mode(safePath(Json.str(m, "path")), Json.long(m, "mode").toInt())
        else -> throw BundleException("this ByOS bundle needs a newer version of the app (unknown step \"$op\")")
    }

    fun open(entry: String): InputStream =
        zip.getInputStream(zip.getEntry(entry) ?: throw BundleException("the ByOS bundle is incomplete ($entry)"))

    /** Reads one file of the bundle and checks its size and SHA-256. */
    fun read(f: BundleFile): ByteArray {
        if (f.size > MAX_FILE) throw BundleException("file too big in the ByOS bundle (${f.path})")
        val b = ByteArray(f.size.toInt())
        open(f.entry).use {
            if (it.readFullyInto(b, 0, b.size) != b.size || it.read() >= 0) {
                throw BundleException("the ByOS bundle is damaged (${f.path})")
            }
        }
        if (sha256Hex(b) != f.sha256) throw BundleException("the ByOS bundle is damaged (${f.path})")
        return b
    }

    fun readText(entry: String): String = open(entry).use { String(it.readBytes(), Charsets.UTF_8) }

    /** Total size of the files that go to the SD card. */
    val sdSize: Long get() = sd.sumOf { it.size }

    /** Checks every file of the bundle against the sizes and SHA-256 of bundle.json. */
    fun verifyAll() {
        val buf = ByteArray(1 shl 16)
        for (f in overlay + sd) {
            val md = sha256()
            var total = 0L
            open(f.entry).use {
                while (true) {
                    val n = it.read(buf)
                    if (n < 0) break
                    md.update(buf, 0, n)
                    total += n
                }
            }
            if (total != f.size || Hex.of(md.digest()) != f.sha256) {
                throw BundleException("the ByOS bundle is damaged (${f.path})")
            }
        }
    }

    override fun close() = zip.close()

    companion object {
        const val FORMAT = 1
        private const val MAX_FILE = 64L shl 20

        fun open(file: File): Bundle {
            val zip = try {
                ZipFile(file)
            } catch (e: java.io.IOException) {
                throw BundleException("the ByOS bundle cannot be opened: ${e.message}")
            }
            try {
                val e = zip.getEntry("bundle.json") ?: throw BundleException("this is not a ByOS bundle")
                if (e.size > 8 shl 20) throw BundleException("this is not a ByOS bundle")
                val text = zip.getInputStream(e).use { String(it.readBytes(), Charsets.UTF_8) }
                val json = try {
                    Json.obj(Json.parse(text))
                } catch (x: JsonException) {
                    throw BundleException("the ByOS bundle is damaged (${x.message})")
                }
                return try {
                    Bundle(zip, file, json)
                } catch (x: JsonException) {
                    throw BundleException("the ByOS bundle is damaged (${x.message})")
                } catch (x: IllegalArgumentException) {
                    throw BundleException("the ByOS bundle is damaged (${x.message})")
                }
            } catch (t: Throwable) {
                zip.close()
                throw t
            }
        }

        internal fun httpsUrl(u: String?): String {
            if (u == null || !u.startsWith("https://") || u.any { it <= ' ' }) {
                throw BundleException("bad link in the ByOS bundle")
            }
            return u
        }

        /** A relative path with "/" separators and no empty, "." or ".." components. */
        internal fun safePath(p: String): String {
            if (p.isEmpty() || p.startsWith("/") || p.contains('\\') || p.contains('\u0000')) {
                throw BundleException("bad path in the ByOS bundle: $p")
            }
            for (part in p.split('/')) {
                if (part.isEmpty() || part == "." || part == "..") throw BundleException("bad path in the ByOS bundle: $p")
            }
            return p
        }
    }
}
