package org.hibyos.install.core

import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.security.MessageDigest
import java.util.IdentityHashMap
import java.util.UUID

/**
 * Turns the user's own copy of the official firmware into the ByOS firmware.
 *
 * This is the JVM counterpart of tools/hibyos_patcher.py and follows it step by step; the
 * output is the same file, byte for byte, when the build identifier and the date written in
 * the container are the same. The list of changes comes from the [Bundle], not from this code.
 *
 * Nothing big is held in memory: the firmware is read from the file, the new root filesystem
 * goes to a temporary file and the container is written from both.
 */
class PatchException(val reason: Reason, message: String, val detail: String? = null) : Exception(message) {
    enum class Reason {
        /** The file is not the supported official firmware. [detail] has its MD5. */
        WRONG_FIRMWARE,

        /** The file looked right but something inside is damaged or unexpected. */
        DAMAGED,

        /** The ByOS bundle is damaged or needs a newer app. */
        BUNDLE,

        /** The new root filesystem does not fit in the player's partition. */
        TOO_BIG,

        /** A check of the result failed; nothing usable was produced. */
        VERIFY,

        /** Reading or writing failed (storage full, file removed...). */
        STORAGE,
        CANCELLED,
    }
}

enum class Stage(val weight: Int) {
    CHECK(4), READ(10), APPLY(4), BUILD(26), VERIFY_ROOTFS(26), WRITE(14), VERIFY_UPT(16);

    /** Overall progress 0..1 for a [fraction] 0..1 of this stage. */
    fun overall(fraction: Double): Double {
        val all = entries.sumOf { it.weight }.toDouble()
        val before = entries.filter { it.ordinal < ordinal }.sumOf { it.weight }
        return (before + weight * fraction.coerceIn(0.0, 1.0)) / all
    }
}

fun interface PatchListener {
    /** [fraction] of the current stage, 0..1. Throw nothing here; use [PatchOptions.cancelled] to stop. */
    fun onProgress(stage: Stage, fraction: Double)
}

class PatchOptions(
    /** Build identifier written to the firmware (32 hex digits). A random one by default. */
    val buildId: String? = null,
    /** Date stored in the container. */
    val epochSeconds: Long = System.currentTimeMillis() / 1000,
    /** Local time minus UTC at that date, in seconds. */
    val utcOffsetSeconds: Int = java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 1000,
    val cancelled: () -> Boolean = { false },
)

class PatchResult(
    val output: File, val size: Long, val md5: String, val sha256: String, val buildId: String,
    val hibyosVersion: String, val entries: Int, val files: Int, val overlayFiles: Int,
    val rootfsSize: Long, val rootfsLimit: Long, val rootfsMd5: String, val stats: SquashStats,
    /** Official files rebuilt with the ByOS changes (the "edit" steps of the bundle). */
    val editedFiles: Int = 0,
)

class FirmwareId(val size: Long, val md5: String, val sha256: String) {
    /** True if this is exactly the firmware the bundle patches. */
    fun matches(spec: FirmwareSpec): Boolean =
        md5 == spec.md5 && (spec.sha256 == null || sha256 == spec.sha256) && (spec.size == null || size == spec.size)
}

object Patcher {
    /** MD5 and SHA-256 of a file. */
    fun identify(file: File, cancelled: () -> Boolean = { false }, progress: ((Double) -> Unit)? = null): FirmwareId {
        val m = md5()
        val s = sha256()
        val buf = ByteArray(1 shl 16)
        val total = file.length().coerceAtLeast(1)
        var done = 0L
        FileInputStream(file).use {
            while (true) {
                if (cancelled()) throw PatchException(PatchException.Reason.CANCELLED, "cancelled")
                val n = it.read(buf)
                if (n < 0) break
                m.update(buf, 0, n)
                s.update(buf, 0, n)
                done += n
                progress?.invoke(done.toDouble() / total)
            }
        }
        return FirmwareId(done, Hex.of(m.digest()), Hex.of(s.digest()))
    }

    /**
     * Patches [input] and writes the result to [output] (through "[output].part"). [workDir]
     * receives one temporary file of about the size of the root filesystem.
     */
    fun patch(input: File, bundle: Bundle, output: File, workDir: File, options: PatchOptions = PatchOptions(),
              listener: PatchListener? = null): PatchResult {
        val tmpRootfs = File(workDir, "rootfs.new")
        val part = File(output.path + ".part")
        var ok = false
        try {
            workDir.mkdirs()
            output.absoluteFile.parentFile?.mkdirs()
            val r = Run(input, bundle, output, tmpRootfs, part, options, listener).run()
            ok = true
            return r
        } catch (e: PatchException) {
            throw e
        } catch (e: BundleException) {
            throw PatchException(PatchException.Reason.BUNDLE, e.message ?: "bad ByOS bundle")
        } catch (e: IsoException) {
            throw PatchException(PatchException.Reason.DAMAGED, e.message ?: "damaged firmware container")
        } catch (e: UptException) {
            throw PatchException(PatchException.Reason.DAMAGED, e.message ?: "damaged firmware container")
        } catch (e: SquashfsException) {
            throw PatchException(PatchException.Reason.DAMAGED, "the root filesystem of the firmware cannot be read: ${e.message}")
        } catch (e: LzoException) {
            throw PatchException(PatchException.Reason.DAMAGED, "the root filesystem of the firmware cannot be read: ${e.message}")
        } catch (e: EOFException) {
            throw PatchException(PatchException.Reason.DAMAGED, "the firmware file is cut short")
        } catch (e: IOException) {
            throw PatchException(PatchException.Reason.STORAGE, e.message ?: e.javaClass.simpleName)
        } finally {
            tmpRootfs.delete()
            if (!ok) part.delete()
        }
    }

    private class Run(
        val input: File, val bundle: Bundle, val output: File, val tmpRootfs: File, val part: File,
        val opt: PatchOptions, val listener: PatchListener?,
    ) {
        fun fail(reason: PatchException.Reason, msg: String, detail: String? = null): Nothing =
            throw PatchException(reason, msg, detail)

        fun damaged(msg: String): Nothing = fail(PatchException.Reason.DAMAGED, msg)

        fun tick(stage: Stage, fraction: Double) {
            if (opt.cancelled()) fail(PatchException.Reason.CANCELLED, "cancelled")
            listener?.onProgress(stage, fraction.coerceIn(0.0, 1.0))
        }

        fun lookup(root: Inode, rel: String): Inode? {
            var node = root
            for (part in rel.split('/')) {
                val ch = node.children ?: damaged("/$rel: a parent is not a directory")
                node = ch[nameKey(part)] ?: return null
            }
            return node
        }

        fun content(img: SquashImage, node: Inode): ByteArray = node.data ?: img.readFile(node)

        fun existingFile(root: Inode, rel: String, what: String): Inode {
            val node = lookup(root, rel)
            if (node == null || node.kind != Kind.FILE) damaged("$what $rel: file not found")
            return node
        }

        fun checkHashes(b: ByteArray, md5: String?, sha: String?, what: String) {
            if (md5 != null && md5Hex(b) != md5) damaged("$what: unexpected md5 ${md5Hex(b)}")
            if (sha != null && sha256Hex(b) != sha) damaged("$what: unexpected content")
        }

        fun run(): PatchResult {
            if (input.absoluteFile.normalize() == output.absoluteFile.normalize()) {
                fail(PatchException.Reason.STORAGE, "the output would overwrite the input file")
            }
            // ---------------------------------------------------------------- check
            tick(Stage.CHECK, 0.0)
            val id = identify(input, opt.cancelled) { tick(Stage.CHECK, it) }
            if (!id.matches(bundle.firmware)) {
                fail(PatchException.Reason.WRONG_FIRMWARE,
                    "this is not the supported official firmware (${bundle.firmware.label})", id.md5)
            }
            FileSource(input).use { src -> return patchSource(src) }
        }

        fun patchSource(src: ByteSource): PatchResult {
            // ---------------------------------------------------------------- read
            tick(Stage.READ, 0.0)
            val files = IsoReader.list(src)
            fun small(path: String): ByteArray {
                val e = files[path] ?: damaged("the firmware container has no $path")
                if (e.size > 1 shl 20) damaged("$path is too big")
                return src.bytes(e.offset, e.size.toInt())
            }
            val upd = String(small(Upt.UPDATE_IN), Charsets.UTF_8)
            val imgs = Upt.parseUpdateIn(upd)
            val kern = imgs.firstOrNull { it.type == "kernel" } ?: damaged("the firmware has no kernel image")
            val rfs = imgs.firstOrNull { it.type == "rootfs" } ?: damaged("the firmware has no rootfs image")
            val toRead = (kern.size + rfs.size).coerceAtLeast(1)
            var read = 0L
            val onRead: (Int) -> Unit = { read += it; tick(Stage.READ, read.toDouble() / toRead) }
            val kdata = Upt.openImage(src, files, kern.name, kern.md5, onRead)
            val rdata = Upt.openImage(src, files, rfs.name, rfs.md5, onRead)
            if (kdata.size != kern.size) damaged("kernel image check failed")
            if (rdata.size != rfs.size) damaged("rootfs image check failed")
            val img = SquashImage(rdata)
            val root = img.readTree()
            tick(Stage.READ, 1.0)

            // ---------------------------------------------------------------- apply
            tick(Stage.APPLY, 0.0)
            val bid = opt.buildId ?: UUID.randomUUID().toString().replace("-", "")
            if (!Regex("[0-9A-Za-z._-]{1,64}").matches(bid)) fail(PatchException.Reason.BUNDLE, "bad build identifier")
            var overlayCount = 0
            var editCount = 0
            val edited = bundle.steps.filterIsInstance<Step.Edit>().map { it.path }.toSet()
            for ((i, step) in bundle.steps.withIndex()) {
                when (step) {
                    is Step.Normalize -> normalize(root)
                    is Step.Overlay -> overlayCount += applyOverlay(root, img.mtime, step.mayReplace, edited)
                    is Step.Edit -> {
                        applyEdit(img, root, step)
                        editCount++
                    }
                    is Step.BuildId -> {
                        var node = lookup(root, step.path)
                        if (node == null) {
                            node = Inode(Kind.FILE, 420, 0, 0, img.mtime)
                            val dir = step.path.substringBeforeLast('/', "")
                            val parent = if (dir.isEmpty()) root else lookup(root, dir)
                            val ch = parent?.children ?: damaged("/${step.path}: the folder does not exist")
                            ch[nameKey(step.path.substringAfterLast('/'))] = node
                        } else if (node.kind != Kind.FILE) {
                            damaged("/${step.path} is not a regular file")
                        }
                        node.setData((bid + "\n").toByteArray(Charsets.US_ASCII))
                    }
                    is Step.Text -> {
                        val node = existingFile(root, step.path, "patch")
                        var t = content(img, node)
                        checkHashes(t, step.md5, step.sha256, "patch ${step.path}")
                        for (e in step.edits) {
                            val cnt = count(t, e.old)
                            if (cnt != e.count) {
                                damaged("patch ${step.path}: original text found $cnt times (expected ${e.count})")
                            }
                            t = replace(t, e.old, e.new)
                        }
                        node.setData(t)     // in place: hard links share the change
                    }
                    is Step.Bin -> {
                        val node = existingFile(root, step.path, "binary patch")
                        val b = content(img, node).copyOf()
                        checkHashes(b, step.md5, step.sha256, "binary patch ${step.path}")
                        for (e in step.edits) {
                            if (e.old.size != e.new.size) fail(PatchException.Reason.BUNDLE, "binary patch ${step.path}: bad edit")
                            if (e.offset < 0 || e.offset + e.old.size > b.size ||
                                !b.copyOfRange(e.offset, e.offset + e.old.size).contentEquals(e.old)) {
                                damaged("binary patch ${step.path}@0x${e.offset.toString(16)}: unexpected bytes")
                            }
                            System.arraycopy(e.new, 0, b, e.offset, e.new.size)
                        }
                        node.setData(b)
                    }
                    is Step.Remove -> {
                        val dir = step.path.substringBeforeLast('/', "")
                        val parent = if (dir.isEmpty()) root else lookup(root, dir)
                        val key = nameKey(step.path.substringAfterLast('/'))
                        val node = parent?.children?.get(key)
                        if (node == null) {
                            if (!step.optional) damaged("remove ${step.path}: not found")
                        } else {
                            if (node.kind == Kind.DIR && node.children!!.isNotEmpty()) {
                                damaged("remove ${step.path}: the folder is not empty")
                            }
                            parent.children!!.remove(key)
                        }
                    }
                    is Step.Mode -> {
                        val node = lookup(root, step.path) ?: damaged("mode ${step.path}: not found")
                        node.mode = step.mode and 0xFFF
                    }
                }
                tick(Stage.APPLY, (i + 1.0) / bundle.steps.size)
            }

            // ---------------------------------------------------------------- build
            tick(Stage.BUILD, 0.0)
            val stats: SquashStats
            var rootfsSize: Long
            FileSink(tmpRootfs).use { sink ->
                stats = SquashWriter.write(root, img, sink) { done, total ->
                    tick(Stage.BUILD, if (total > 0) done.toDouble() / total else 1.0)
                }
                val pad = ((bundle.align - sink.position % bundle.align) % bundle.align).toInt()
                if (pad > 0) sink.write(ByteArray(pad))
                rootfsSize = sink.position
            }
            if (rootfsSize > bundle.rootfsLimit) {
                fail(PatchException.Reason.TOO_BIG, "the new root filesystem is too big for the partition " +
                    "($rootfsSize of ${bundle.rootfsLimit} bytes allowed)")
            }
            tick(Stage.BUILD, 1.0)

            FileSource(tmpRootfs).use { newSrc ->
                // ------------------------------------------------------------ verify rootfs
                val (entries, nfiles) = verifyRootfs(newSrc, img, root)

                // ------------------------------------------------------------ write
                tick(Stage.WRITE, 0.0)
                val out = ArrayList<IsoInput>()
                out.add(IsoInput("ota_config.in", ArraySource(small("ota_config.in"))))
                out.add(IsoInput("ota_v0/ota_v0.ok", ArraySource(small("ota_v0/ota_v0.ok"))))
                val toHash = (kdata.size + newSrc.size).coerceAtLeast(1)
                var hashed = 0L
                val onHash: (Int) -> Unit = { hashed += it; tick(Stage.WRITE, 0.3 * hashed / toHash) }
                val kc = Upt.chunkImage(kern.name, kdata, bundle.chunk, onHash)
                val rc = Upt.chunkImage(rfs.name, newSrc, bundle.chunk, onHash)
                out.addAll(kc.files)
                out.addAll(rc.files)
                val cut = upd.indexOf("img_type=rootfs")
                val newUpd = if (cut < 0) upd else upd.substring(0, cut) + "img_type=rootfs" +
                    upd.substring(cut + "img_type=rootfs".length)
                        .replace("img_md5=${rfs.md5}", "img_md5=${rc.wholeMd5}")
                        .replace("img_size=${rfs.fields["img_size"]}", "img_size=$rootfsSize")
                out.add(IsoInput(Upt.UPDATE_IN, ArraySource(newUpd.toByteArray(Charsets.UTF_8))))
                if (out.map { it.path }.toSet().size != out.size) damaged("duplicate names in the firmware container")

                part.delete()
                val m = md5()
                val s = sha256()
                val size: Long
                FileOutputStream(part).use { fos ->
                    val os = DigestStream(BufferedOutputStream(fos, 1 shl 16), m, s)
                    size = UptIsoWriter.write(os, out, opt.epochSeconds, opt.utcOffsetSeconds) { done, total ->
                        tick(Stage.WRITE, 0.3 + 0.7 * done / total)
                    }
                    os.flush()
                    fos.fd.sync()
                }
                tick(Stage.WRITE, 1.0)

                // ------------------------------------------------------------ verify container
                verifyUpt(part, kern.name, kern.md5, rfs.name, newSrc, size)
                if (output.exists() && !output.delete()) {
                    fail(PatchException.Reason.STORAGE, "cannot replace ${output.name}")
                }
                if (!part.renameTo(output)) fail(PatchException.Reason.STORAGE, "cannot create ${output.name}")
                return PatchResult(output, size, Hex.of(m.digest()), Hex.of(s.digest()), bid, bundle.hibyosVersion,
                    entries, nfiles, overlayCount, rootfsSize, bundle.rootfsLimit, rc.wholeMd5, stats, editCount)
            }
        }

        /** See normalize_like_build_py() in tools/hibyos_patcher.py. */
        fun normalize(root: Inode) {
            for ((path, n) in SquashImage.walk(root)) {
                if (n.kind == Kind.SYMLINK) {
                    n.uid = 0
                    n.gid = 0
                    n.mode = 511
                } else if (n.kind != Kind.DIR && n.kind != Kind.FILE) {
                    damaged("unsupported special file in the firmware: ${displayName(path)}")
                }
            }
            root.uid = 0
            root.gid = 0
        }

        /**
         * See apply_vendor_edits() in tools/hibyos_patcher.py and tools/vendor_edits.py: the
         * official file must be the expected one, the result must be the expected one, and it
         * replaces the official file like an overlay file (new inode, original mode and owner).
         */
        fun applyEdit(img: SquashImage, root: Inode, e: Step.Edit) {
            val dir = e.path.substringBeforeLast('/', "")
            val parent = if (dir.isEmpty()) root else lookup(root, dir)
            val key = nameKey(e.path.substringAfterLast('/'))
            val old = parent?.children?.get(key)
            if (old == null || old.kind != Kind.FILE) {
                damaged("edit ${e.path}: not a regular file in the official firmware")
            }
            val stock = content(img, old)
            if (stock.size.toLong() != e.stockSize || sha256Hex(stock) != e.stockSha256) {
                damaged("edit ${e.path}: this is not the expected official file (size ${stock.size})")
            }
            val base = if (e.stripCr) {
                val o = ByteArrayOutputStream(stock.size)
                for (b in stock) if (b != 0x0D.toByte()) o.write(b.toInt())
                o.toByteArray()
            } else stock
            val out = ByteArrayOutputStream(e.resultSize.toInt().coerceAtLeast(16))
            for (op in e.ops) {
                when (op) {
                    is EditOp.Copy -> {
                        if (op.offset < 0 || op.length <= 0 || op.offset.toLong() + op.length > base.size) {
                            fail(PatchException.Reason.BUNDLE,
                                "edit ${e.path}: copy [${op.offset}, ${op.length}] is outside the official file")
                        }
                        out.write(base, op.offset, op.length)
                    }
                    is EditOp.Add -> out.write(op.data, 0, op.data.size)
                }
            }
            val res = out.toByteArray()
            if (res.size.toLong() != e.resultSize || sha256Hex(res) != e.resultSha256) {
                fail(PatchException.Reason.BUNDLE, "edit ${e.path}: the edited file is not the expected one")
            }
            val new = Inode(Kind.FILE, old.mode, old.uid, old.gid, img.mtime)
            new.setData(res)
            parent.children!![key] = new                 // breaks a hard link, like rm + create
        }

        /** See overlay_replace_ok() in tools/build.py. */
        fun overlayMayReplace(path: String, mayReplace: List<String>?, edited: Set<String>) {
            if (path in edited) {
                fail(PatchException.Reason.BUNDLE, "overlay $path: this file is built by an edit; it must not be in the overlay")
            }
            if (mayReplace != null && mayReplace.none { path.lowercase().endsWith(it) }) {
                fail(PatchException.Reason.BUNDLE, "overlay $path: would replace a file of the official firmware")
            }
        }

        fun applyOverlay(root: Inode, mtime: Long, mayReplace: List<String>?, edited: Set<String>): Int {
            var count = 0
            for (f in bundle.overlay) {
                val data = bundle.read(f)
                val parts = f.path.split('/')
                var node = root
                for (p in parts.dropLast(1)) {
                    val ch = node.children!!
                    var child = ch[nameKey(p)]
                    if (child == null) {                 // new directory: root:root 0755
                        child = Inode(Kind.DIR, 493, 0, 0, mtime)
                        ch[nameKey(p)] = child
                    } else if (child.kind != Kind.DIR) {
                        damaged("overlay ${f.path}: /$p is not a directory in the firmware")
                    }
                    node = child
                }
                var mode = f.mode
                var uid = 0L
                var gid = 0L
                val key = nameKey(parts.last())
                val old = node.children!![key]
                if (old != null || f.path in edited) overlayMayReplace(f.path, mayReplace, edited)
                if (old != null) {
                    if (old.kind != Kind.FILE) damaged("overlay ${f.path}: replaces a ${old.kind.label}, not supported")
                    mode = old.mode                      // original mode and owner are kept
                    uid = old.uid
                    gid = old.gid
                }
                val new = Inode(Kind.FILE, mode, uid, gid, mtime)
                new.setData(data)
                node.children!![key] = new               // breaks a hard link, like rm + create
                count++
            }
            return count
        }

        /** Re-reads the new image and compares every entry with the intended tree. */
        fun verifyRootfs(newSrc: ByteSource, img: SquashImage, root: Inode): Pair<Int, Int> {
            fun bad(msg: String): Nothing = fail(PatchException.Reason.VERIFY, "verify: $msg")
            tick(Stage.VERIFY_ROOTFS, 0.0)
            val nimg: SquashImage
            val nroot: Inode
            try {
                nimg = SquashImage(newSrc)
                nroot = nimg.readTree()
            } catch (e: SquashfsException) {
                bad("the new root filesystem cannot be read back (${e.message})")
            }
            if (!(96 <= nimg.inodeTable && nimg.inodeTable < nimg.dirTable && nimg.dirTable < nimg.fragTable &&
                    nimg.fragTable < nimg.exportTable && nimg.exportTable < nimg.idTable && nimg.idTable < nimg.bytesUsed)) {
                bad("unexpected table layout")
            }
            val want = SquashImage.walk(root)
            val got = SquashImage.walk(nroot)
            if (want.size != got.size) bad("the file list differs")
            for (i in want.indices) if (want[i].first != got[i].first) bad("the file list differs")
            val pair = IdentityHashMap<Inode, Inode>()
            var files = 0
            val total = want.sumOf { if (it.second.kind == Kind.FILE) it.second.size else 0L }.coerceAtLeast(1)
            var done = 0L
            val mdA = sha256()
            val mdB = sha256()
            for (i in want.indices) {
                val path = want[i].first
                val a = want[i].second
                val b = got[i].second
                if (a.kind != b.kind || a.mode != b.mode || a.uid != b.uid || a.gid != b.gid) {
                    bad("attributes differ on ${displayName(path)}")
                }
                val prev = pair.putIfAbsent(a, b)
                if (prev != null && prev !== b) bad("hard links differ on ${displayName(path)}")
                if (a.kind == Kind.SYMLINK && !(a.target ?: ByteArray(0)).contentEquals(b.target ?: ByteArray(0))) {
                    bad("link target differs on ${displayName(path)}")
                }
                if (a.kind == Kind.FILE) {
                    if (a.size != b.size) bad("size differs on ${displayName(path)}")
                    val da = a.data
                    val ha = if (da != null) { mdA.reset(); mdA.digest(da) } else img.digestFile(a, mdA)
                    val hb = try {
                        nimg.digestFile(b, mdB)
                    } catch (e: SquashfsException) {
                        bad("${displayName(path)} cannot be read back (${e.message})")
                    } catch (e: LzoException) {
                        bad("${displayName(path)} cannot be read back (${e.message})")
                    }
                    if (!MessageDigest.isEqual(ha, hb)) bad("content differs on ${displayName(path)}")
                    files++
                    done += a.size
                    tick(Stage.VERIFY_ROOTFS, done.toDouble() / total)
                }
            }
            val distinct = IdentityHashMap<Inode, Boolean>()
            for (v in pair.values) distinct[v] = true
            if (distinct.size != pair.size) bad("hard links differ")
            tick(Stage.VERIFY_ROOTFS, 1.0)
            return want.size to files
        }

        /** Re-reads the written container: MD5 chains, sizes, same kernel, the rootfs just built. */
        fun verifyUpt(file: File, kernName: String, kernMd5: String, rfsName: String, rootfs: ByteSource, size: Long) {
            fun bad(msg: String): Nothing = fail(PatchException.Reason.VERIFY, "verify: $msg")
            tick(Stage.VERIFY_UPT, 0.0)
            if (file.length() != size) bad("the written file has the wrong size")
            FileSource(file).use { src ->
                try {
                    val files = IsoReader.list(src)
                    val ue = files[Upt.UPDATE_IN] ?: bad("the written file has no image list")
                    val imgs = Upt.parseUpdateIn(String(src.bytes(ue.offset, ue.size.toInt()), Charsets.UTF_8))
                    val total = (imgs.sumOf { it.size } + rootfs.size).coerceAtLeast(1)
                    var done = 0L
                    val on: (Int) -> Unit = { done += it; tick(Stage.VERIFY_UPT, done.toDouble() / total) }
                    val seen = HashMap<String, Pair<UptImage, ConcatSource>>()
                    for (i in imgs) {
                        val d = Upt.openImage(src, files, i.name, i.md5, on)     // checks the md5 chain
                        if (d.size != i.size) bad("${i.name} md5/size mismatch")
                        seen[i.type] = i to d
                    }
                    val k = seen["kernel"] ?: bad("the kernel image is missing")
                    if (k.first.name != kernName || k.first.md5 != kernMd5) bad("the kernel image changed")
                    val r = seen["rootfs"] ?: bad("the rootfs image is missing")
                    if (r.first.name != rfsName || r.second.size != rootfs.size) {
                        bad("the rootfs image in the .upt is not the one built")
                    }
                    val a = ByteArray(1 shl 16)
                    val b = ByteArray(1 shl 16)
                    var pos = 0L
                    while (pos < rootfs.size) {
                        val n = minOf(rootfs.size - pos, a.size.toLong()).toInt()
                        r.second.read(pos, a, 0, n)
                        rootfs.read(pos, b, 0, n)
                        for (j in 0 until n) if (a[j] != b[j]) bad("the rootfs image in the .upt is not the one built")
                        pos += n
                        on(n)
                    }
                } catch (e: IsoException) {
                    bad("the written file cannot be read back (${e.message})")
                } catch (e: UptException) {
                    bad(e.message ?: "the written file is damaged")
                }
            }
            tick(Stage.VERIFY_UPT, 1.0)
        }

        fun count(t: ByteArray, pat: ByteArray): Int {
            if (pat.isEmpty()) return t.size + 1
            var n = 0
            var i = indexOf(t, pat, 0)
            while (i >= 0) {
                n++
                i = indexOf(t, pat, i + pat.size)
            }
            return n
        }

        fun indexOf(t: ByteArray, pat: ByteArray, from: Int): Int {
            val last = t.size - pat.size
            var i = from
            outer@ while (i <= last) {
                for (j in pat.indices) if (t[i + j] != pat[j]) { i++; continue@outer }
                return i
            }
            return -1
        }

        fun replace(t: ByteArray, old: ByteArray, new: ByteArray): ByteArray {
            if (old.isEmpty()) fail(PatchException.Reason.BUNDLE, "empty text in a patch")
            val out = ByteBuf(t.size + 64)
            var p = 0
            while (true) {
                val i = indexOf(t, old, p)
                if (i < 0) break
                out.bytes(t, p, i - p).bytes(new)
                p = i + old.size
            }
            out.bytes(t, p, t.size - p)
            return out.toByteArray()
        }
    }

    /** Output stream that also feeds digests. */
    private class DigestStream(private val out: OutputStream, private vararg val mds: MessageDigest) : OutputStream() {
        override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

        override fun write(b: ByteArray, off: Int, len: Int) {
            out.write(b, off, len)
            for (m in mds) m.update(b, off, len)
        }

        override fun flush() = out.flush()
        override fun close() = out.close()
    }
}
