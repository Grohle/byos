package org.hibyos.install.core

import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream

/**
 * Building ByOS from the user's official firmware: the steps the Android app and byos-cli
 * (and through it the desktop app) share, so that both keep, reuse and deliver a build the
 * same way. Plain JVM code, no Android classes.
 *
 *  1. [BundleStore]: the ByOS files to build with (the copy shipped with the app, or a newer
 *     signed one from the project's releases).
 *  2. [FirmwareCache]: the official firmware, checked once and remembered.
 *  3. [ByosBuild.patch]: the patched firmware plus a [BuildRecord] next to it.
 *  4. [ByosBuild.items] / [ByosBuild.exportSd]: what goes to the SD card, in a safe order.
 */

// ---------------------------------------------------------------------------- 1. the ByOS files

/**
 * Where the ByOS bundle in use is kept: `builtin.zip` (unpacked from the copy shipped with the
 * program, again whenever that copy changes) and `updated.zip` (a newer signed bundle, used
 * only while it is newer than the built-in one).
 */
class BundleStore(val dir: File) {
    val builtinFile = File(dir, "builtin.zip")
    private val stampFile = File(dir, "builtin.stamp")
    val updatedFile = File(dir, "updated.zip")

    /** The result of [current]: the bundle to use and whether it is the built-in one. */
    class Current(val bundle: Bundle, val builtIn: Boolean)

    /**
     * Opens the built-in bundle. [stamp] identifies the shipped copy (for example the app's
     * install time and version); when it differs from the last one, [open] is copied again.
     */
    fun builtin(stamp: String, open: () -> InputStream): Bundle {
        dir.mkdirs()
        if (!builtinFile.isFile || !stampFile.isFile || stampFile.readText() != stamp) {
            val tmp = File(dir, "builtin.tmp")
            open().use { i -> FileOutputStream(tmp).use { o -> i.copyTo(o, 1 shl 16) } }
            builtinFile.delete()
            if (!tmp.renameTo(builtinFile)) throw IOException("the ByOS files could not be unpacked")
            stampFile.writeText(stamp)
        }
        return Bundle.open(builtinFile)
    }

    /**
     * The bundle to build with: the updated one when it opens and is newer than [base], else
     * [base]. An updated bundle that is not newer (the program itself now ships the same or a
     * newer version) or that does not open is deleted.
     */
    fun current(base: Bundle): Current {
        if (updatedFile.isFile) {
            try {
                val u = Bundle.open(updatedFile)
                if (BundleUpdater.isNewer(u.hibyosVersion, base.hibyosVersion)) return Current(u, false)
                u.close()
            } catch (e: BundleException) {
                // damaged: forget it
            }
            updatedFile.delete()
        }
        return Current(base, true)
    }

    /**
     * Asks the project's releases for a newer bundle than [current]. The index, its link prefix
     * and the keys always come from [base] (the files shipped with the program), never from a
     * download. Returns null when [current] is the newest.
     */
    fun check(base: Bundle, current: Bundle, cancelled: () -> Boolean = { false }): RemoteBundle? {
        val index = base.updateIndex ?: throw BundleException("these ByOS files name no place to look for updates")
        val prefix = base.updatePrefix ?: throw BundleException("these ByOS files name no place to look for updates")
        if (base.updateKeys.isEmpty()) throw BundleException("these ByOS files carry no release key")
        val r = BundleUpdater.check(index, base.updateKeys, prefix, cancelled)
        return if (BundleUpdater.isNewer(r.version, current.hibyosVersion)) r else null
    }

    /** Downloads [remote] (checked against the signed index) and makes it the updated bundle. */
    fun install(remote: RemoteBundle, cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null,
                beforeReplace: () -> Unit = {}): Bundle {
        dir.mkdirs()
        val staged = File(dir, "staged.zip")
        BundleUpdater.download(remote, staged, cancelled, progress)
        beforeReplace() // the caller closes a bundle that still reads the file being replaced
        updatedFile.delete()
        if (!staged.renameTo(updatedFile)) throw IOException("the ByOS files could not be saved")
        return Bundle.open(updatedFile)
    }
}

// ---------------------------------------------------------------------------- 2. the official firmware

/** An official firmware file that was checked against the bundle and kept. */
class CachedFirmware(val file: File, val size: Long, val sha256: String)

/**
 * The official firmware kept in a folder: the file under the bundle's name plus `firmware.ok`
 * (its size and SHA-256), written only after the file was checked.
 */
object FirmwareCache {
    const val MARK = "firmware.ok"

    fun file(dir: File, spec: FirmwareSpec) = File(dir, spec.fileName)

    /** Records that the firmware in [dir] was checked and is [id]. */
    fun mark(dir: File, id: FirmwareId) {
        dir.mkdirs()
        File(dir, MARK).writeText("${id.size}\n${id.sha256}\n")
    }

    fun unmark(dir: File) {
        File(dir, MARK).delete()
    }

    /** The kept firmware, or null when there is none or it is not the one [spec] wants. */
    fun ready(dir: File, spec: FirmwareSpec): CachedFirmware? {
        val f = file(dir, spec)
        val mark = File(dir, MARK)
        if (!f.isFile || !mark.isFile) return null
        val lines = mark.readLines()
        if (lines.size < 2 || lines[0].toLongOrNull() != f.length()) return null
        if (spec.size != null && spec.size != f.length()) return null
        if (spec.sha256 != null && lines[1] != spec.sha256) return null
        return CachedFirmware(f, f.length(), lines[1])
    }

    fun forget(dir: File, spec: FirmwareSpec) {
        unmark(dir)
        file(dir, spec).delete()
    }

    /**
     * Checks a firmware file the user chose and keeps a copy of it in [dir]. Returns null (and
     * keeps nothing) when it is not the firmware [spec] names; [onWrong] then gets what it is.
     */
    fun import(source: File, dir: File, spec: FirmwareSpec, cancelled: () -> Boolean = { false },
               progress: ((Double) -> Unit)? = null, onWrong: ((FirmwareId) -> Unit)? = null): CachedFirmware? {
        val limit = spec.size ?: (512L shl 20)
        if (source.length() > limit) {
            onWrong?.invoke(FirmwareId(source.length(), "", ""))
            return null
        }
        val id = Patcher.identify(source, cancelled, progress)
        if (!id.matches(spec)) {
            onWrong?.invoke(id)
            return null
        }
        dir.mkdirs()
        val dest = file(dir, spec)
        if (source.canonicalFile != dest.canonicalFile) {
            unmark(dir)
            val tmp = File(dir, dest.name + ".import")
            source.copyTo(tmp, overwrite = true)
            if (tmp.length() != id.size) { tmp.delete(); throw IOException("the firmware could not be copied") }
            dest.delete()
            if (!tmp.renameTo(dest)) { tmp.delete(); throw IOException("the firmware could not be kept") }
        }
        mark(dir, id)
        return CachedFirmware(dest, id.size, id.sha256)
    }
}

// ---------------------------------------------------------------------------- 3 and 4. the build

/**
 * A finished build, kept as `result.txt` next to the patched firmware (one value per line:
 * ByOS version, size, SHA-256, build id, files, the bundle's generated_from).
 */
class BuildRecord(
    val hibyosVersion: String, val size: Long, val sha256: String, val buildId: String, val files: Int, val generatedFrom: String,
) {
    fun write(mark: File) {
        mark.writeText("$hibyosVersion\n$size\n$sha256\n$buildId\n$files\n$generatedFrom\n")
    }
}

object ByosBuild {
    const val RECORD = "result.txt"

    /** Where the payload's version and checksum list live: written last (see [sdOrder]). */
    private val LAST = listOf("ByOS/payload/MANIFEST.md5", "ByOS/payload/VERSION")

    /**
     * The build of [bundle] kept in [outDir], or null when there is none, it is incomplete, or
     * it was made from other ByOS files.
     */
    fun restore(bundle: Bundle, outDir: File, stamp: String? = null): BuildRecord? {
        val f = File(outDir, bundle.outName)
        val mark = File(outDir, RECORD)
        if (!f.isFile || !mark.isFile || !stampOk(outDir, stamp)) return null
        val l = mark.readLines()
        if (l.size < 6 || l[0] != bundle.hibyosVersion || l[5] != bundle.generatedFrom || l[1].toLongOrNull() != f.length()) return null
        return BuildRecord(l[0], f.length(), l[2], l[3], l[4].toIntOrNull() ?: 0, l[5])
    }

    /**
     * What a build in a folder was made from: the ByOS files (version, origin and SHA-256 of the
     * bundle) and the program that made it. Written before a build starts; a build whose stamp
     * differs (newer ByOS files, a newer app) or that has no record (it stopped half way) is
     * discarded by [sweep] instead of being reused or getting in the way of the next build.
     */
    const val STAMP = "build.stamp"

    /** byos-cli's description of a finished build, next to the record. */
    const val MANIFEST = "build.json"

    /** Names a build leaves in its folder (the patched firmware and partial files are matched by their ending). */
    private val OWN = setOf(RECORD, STAMP, MANIFEST, "ByOS", ".work")
    private val OWN_SUFFIX = listOf(".upt", ".part", ".new", ".tmp")

    /** The stamp of a build of [bundle] made by [app] (for example "android 1.2.0" or "byos-cli 0.1.0"). */
    fun stamp(bundle: Bundle, app: String): String =
        "format 1\nbyos ${bundle.hibyosVersion}\nfrom ${bundle.generatedFrom}\nbundle ${fileSha256(bundle.file)}\napp $app\n"

    private fun stampOk(outDir: File, stamp: String?): Boolean {
        if (stamp == null) return true
        val f = File(outDir, STAMP)
        return f.isFile && f.readText() == stamp
    }

    private fun same(a: File, b: File?): Boolean = b != null && a.absoluteFile.normalize() == b.absoluteFile.normalize()

    /**
     * Makes the build folder ready for [bundle]: a finished build of the same ByOS files made by
     * the same program ([stamp]) is kept, and only what an interrupted run left around it goes
     * (the work folder, partial files, firmware files of other versions). Anything else (a build
     * that stopped half way, one made from other ByOS files or by another version of the app) is
     * removed. The official firmware lives in its own folder and is never touched here; [keep]
     * is a file that stays even if it is in [outDir]. Returns true when a finished build was kept.
     */
    fun sweep(bundle: Bundle, outDir: File, workDir: File, stamp: String?, keep: File? = null): Boolean {
        if (restore(bundle, outDir, stamp) != null) {
            if (!same(workDir, keep)) workDir.deleteRecursively()
            outDir.listFiles()?.forEach { f ->
                val junk = f.name == ".work" || (f.isFile && f.name != bundle.outName && OWN_SUFFIX.any { f.name.endsWith(it) })
                if (junk && !same(f, keep)) f.deleteRecursively()
            }
            return true
        }
        startOver(outDir, workDir, keep)
        return false
    }

    /**
     * "Start over": removes every step of the build in [outDir] (the record, the stamp, the
     * patched firmware and its partial files, the exported ByOS folder) and [workDir]. Only names
     * a build makes are removed, so a folder given by the user keeps its other files, and [keep]
     * stays. The official firmware's cache is another folder and is never touched.
     */
    fun startOver(outDir: File, workDir: File, keep: File? = null) {
        workDir.deleteRecursively()
        outDir.listFiles()?.forEach { f ->
            val own = f.name in OWN || (f.isFile && OWN_SUFFIX.any { f.name.endsWith(it) })
            if (own && !same(f, keep)) f.deleteRecursively()
        }
    }

    /**
     * Patches [firmware] into `outDir/<bundle.outName>` and records the result. The record is
     * removed first, so a build that stops half way is never taken for a finished one. With a
     * [stamp], what an earlier run left in [outDir] is cleared first and the stamp is written
     * before the patch starts, so a later run can tell what this build was made from.
     */
    fun patch(firmware: File, bundle: Bundle, outDir: File, workDir: File, options: PatchOptions = PatchOptions(),
              listener: PatchListener? = null, stamp: String? = null): Pair<PatchResult, BuildRecord> {
        outDir.mkdirs()
        val mark = File(outDir, RECORD)
        mark.delete()
        if (stamp != null) {
            startOver(outDir, workDir, firmware)
            File(outDir, STAMP).writeText(stamp)
        }
        val r = Patcher.patch(firmware, bundle, File(outDir, bundle.outName), workDir, options, listener)
        val rec = BuildRecord(r.hibyosVersion, r.size, r.sha256, r.buildId, r.files, bundle.generatedFrom)
        rec.write(mark)
        return r to rec
    }

    /**
     * The SD files of [bundle] in the order they are copied: the payload's checksum list and
     * version last, so that a copy that stops half way leaves the old version on the card and
     * the player does not try to install a half-copied payload when it starts.
     */
    fun sdOrder(bundle: Bundle): List<BundleFile> {
        val (last, first) = bundle.sd.partition { it.path in LAST }
        return first + last.sortedBy { LAST.indexOf(it.path) }
    }

    /**
     * Everything to put on the SD card: the ByOS folder first ([sdOrder]) and the firmware last,
     * so a transfer that stops half way never leaves an update file on the card without the
     * files it needs. [firmware] is the patched file (null: the ByOS folder only).
     */
    fun items(bundle: Bundle, firmware: File?, firmwareSha256: String?): List<SendItem> {
        val list = ArrayList<SendItem>()
        for (f in sdOrder(bundle)) list += SendItem(f.path, f.size, f.sha256) { bundle.open(f.entry) }
        if (firmware != null) list += SendItem(bundle.outName, firmware.length(), firmwareSha256) { FileInputStream(firmware) }
        return list
    }

    /**
     * Writes the SD files of [bundle] under [dir] (the card's layout: `dir/ByOS/...`), each one
     * checked against its SHA-256 while it is written. Files already there with the right size
     * and hash are kept. Returns the files in copy order.
     */
    fun exportSd(bundle: Bundle, dir: File, cancelled: () -> Boolean = { false },
                 progress: ((Long, Long) -> Unit)? = null): List<BundleFile> {
        val order = sdOrder(bundle)
        val total = order.sumOf { it.size }
        var done = 0L
        val buf = ByteArray(1 shl 16)
        val root = dir.canonicalFile
        for (f in order) {
            if (cancelled()) throw PatchException(PatchException.Reason.CANCELLED, "cancelled")
            val dest = File(root, f.path)
            if (!dest.canonicalPath.startsWith(root.path + File.separator)) throw BundleException("bad path in the ByOS bundle: ${f.path}")
            if (dest.isFile && dest.length() == f.size && fileSha256(dest) == f.sha256) {
                done += f.size
                progress?.invoke(done, total)
                continue
            }
            dest.parentFile.mkdirs()
            val part = File(dest.path + ".part")
            val md = sha256()
            var n = 0L
            bundle.open(f.entry).use { i ->
                FileOutputStream(part).use { o ->
                    while (true) {
                        if (cancelled()) { o.close(); part.delete(); throw PatchException(PatchException.Reason.CANCELLED, "cancelled") }
                        val r = i.read(buf)
                        if (r < 0) break
                        md.update(buf, 0, r)
                        o.write(buf, 0, r)
                        n += r
                        done += r
                        progress?.invoke(done, total)
                    }
                }
            }
            if (n != f.size || Hex.of(md.digest()) != f.sha256) {
                part.delete()
                throw BundleException("the ByOS bundle is damaged (${f.path})")
            }
            dest.delete()
            if (!part.renameTo(dest)) { part.delete(); throw IOException("cannot write ${dest.path}") }
        }
        progress?.invoke(total, total)
        return order
    }

    fun fileSha256(f: File): String {
        val md = sha256()
        val buf = ByteArray(1 shl 16)
        FileInputStream(f).use { s ->
            while (true) {
                val n = s.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return Hex.of(md.digest())
    }
}
