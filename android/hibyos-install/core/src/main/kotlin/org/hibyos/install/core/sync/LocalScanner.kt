package org.hibyos.install.core.sync

import org.hibyos.install.core.library.TagException
import org.hibyos.install.core.library.TagReader
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.attribute.BasicFileAttributes

/**
 * Scans one link's folder on this computer (sync-scan): every file of the link's kind, with
 * size, modification time and (for audio) tags. Tags, hashes and fingerprints of files whose
 * size and time did not change come from the previous scan. A folder that cannot be read is
 * recorded as unreadable, never as empty, so nothing under it is ever "deleted on the computer".
 * Symbolic links and junctions are followed (a link back to a folder above is not); a broken
 * link is recorded as unreadable.
 */
object LocalScanner {
    /**
     * The volume of [dir]: the Windows volume serial, the Linux file system UUID, or the Unix
     * device number (which can change when a USB drive is plugged in again); null when unknown.
     */
    fun volumeId(dir: File): String? {
        return try {
            val p = dir.toPath()
            if (!Files.exists(p)) return null
            val store = Files.getFileStore(p)
            try {
                (store.getAttribute("volume:vsn") as? Number)?.let { return "vsn:" + Integer.toHexString(it.toInt()) }
            } catch (e: Exception) {}
            try { linuxUuid(store.name())?.let { return "uuid:$it" } } catch (e: Exception) {}
            try {
                (Files.getAttribute(p, "unix:dev") as? Number)?.let { return "dev:" + it.toLong() }
            } catch (e: Exception) {}
            null
        } catch (e: Exception) { null }
    }

    /** The UUID of the Linux block device [dev] ("/dev/sdb1"), from the links in [byUuid]. */
    fun linuxUuid(dev: String, byUuid: File = File("/dev/disk/by-uuid")): String? {
        if (!dev.startsWith("/dev/")) return null
        val real = File(dev).canonicalPath
        return byUuid.listFiles()?.firstOrNull { try { it.canonicalPath == real } catch (e: IOException) { false } }?.name
    }

    /** Whether two volume ids are of the same kind ("vsn:", "uuid:", "dev:"), so that comparing them means something. */
    fun sameScheme(a: String?, b: String?) = a != null && b != null && a.substringBefore(':') == b.substringBefore(':')

    /**
     * The tags of [f]; null for a format the reader does not know (such files have no tags to
     * read). Throws when the file could not be read (locked, cut short, cloud placeholder).
     */
    fun tagsOf(f: File): LocalTags? {
        val i = try { TagReader.read(f) } catch (e: TagException) {
            if (e.message == "unsupported file format") return null
            throw e
        }
        return LocalTags(i.title, i.artist, i.albumArtist, i.album, i.genre, TagReader.leadingInt(i.track), TagReader.leadingInt(i.disc),
            TagReader.leadingInt(i.date), i.durationMs)
    }

    /**
     * [readTags] returns null for a file without tags and throws (IOException or
     * RuntimeException) when the tags could not be read: the entry is then marked [LocalEntry.tagError].
     */
    fun scan(
        link: Link, prev: LocalScan?, full: Boolean = false,
        cancelled: () -> Boolean = { false },
        progress: (op: String, path: String?, done: Long, total: Long) -> Unit = { _, _, _, _ -> },
        readTags: (File) -> LocalTags? = ::tagsOf,
    ): LocalScan {
        val root = File(link.local)
        val vol = volumeId(root)
        fun result(status: String) = LocalScan(link.id, link.local, status, vol, emptyMap(), listOf(""), 0)
        if (!root.exists()) return result("missing")
        if (!root.isDirectory) return result("not_dir")
        val top = root.listFiles() ?: return result("unreadable")
        if (top.isEmpty()) return result("empty")
        // (rel in NFC, rel as spelled on disk, file)
        val files = ArrayList<Triple<String, String, File>>()
        val unreadable = ArrayList<String>()
        var errors = 0
        var folders = 0L
        val rootReal = try { root.toPath().toRealPath() } catch (e: IOException) { root.toPath().toAbsolutePath().normalize() }
        fun walk(dir: File, rel: String, disk: String, list: Array<File>?, real: Path) {
            if (cancelled()) throw SyncException("cancelled", "cancelled")
            folders++
            progress("scan", dir.path, folders, 0)
            if (list == null) { unreadable += rel; return }
            for (f in list) {
                val n = f.name
                if (n.startsWith(".")) continue
                val d = if (disk.isEmpty()) n else "$disk/$n"
                val r = SyncRules.nfc(d)
                val p = f.toPath()
                val own = try { Files.readAttributes(p, BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS) } catch (e: IOException) { errors++; continue }
                // a symbolic link, or a junction (a directory reparse point)
                val linked = own.isSymbolicLink || (own.isDirectory && own.isOther)
                val a = if (!linked) own else try { Files.readAttributes(p, BasicFileAttributes::class.java) } catch (e: IOException) {
                    unreadable += r; continue   // a broken link: whatever it pointed to stays on the HiBy
                }
                if (a.isDirectory) {
                    if (r.count { it == '/' } >= 64) continue
                    val sub = if (!linked) real.resolve(n) else try { p.toRealPath() } catch (e: IOException) { unreadable += r; continue }
                    // a link to a folder above: its files are listed there already
                    if (linked && (real.startsWith(sub) || rootReal.startsWith(sub))) continue
                    walk(f, r, d, f.listFiles(), sub)
                } else if (a.isRegularFile) {
                    if (Kinds.relevant(link.kind, n)) files += Triple(r, d, f)
                }
            }
        }
        walk(root, "", "", top, rootReal)
        val entries = LinkedHashMap<String, LocalEntry>(files.size * 2)
        val readTagsFor = link.kind == Kinds.MUSIC || link.kind == Kinds.AUDIOBOOKS
        for ((k, tr) in files.withIndex()) {
            if (cancelled()) throw SyncException("cancelled", "cancelled")
            val (rel, disk, f) = tr
            val size = f.length()
            val mtime = f.lastModified() / 1000
            if (mtime == 0L && !f.exists()) { errors++; continue }
            val last = prev?.entries?.get(rel)
            val old = last?.takeIf { !full && it.size == size && it.mtime == mtime }
            val media = Kinds.isMedia(link.kind, f.name)
            var tagError = false
            val tags = when {
                !media || !readTagsFor -> null
                old != null && old.tags != null && !old.tagError -> old.tags
                else -> {
                    if (k % 50 == 0) progress("tags", f.path, k.toLong(), files.size.toLong())
                    try { readTags(f) } catch (e: IOException) { tagError = true; null } catch (e: RuntimeException) { tagError = true; null }
                }
            } ?: if (tagError) last?.tags else null   // the last tags read keep the file where it was
            entries[rel] = LocalEntry(rel, size, mtime, tags, old?.sha, old?.qfp, if (disk != rel) disk else null, tagError)
        }
        progress("tags", null, files.size.toLong(), files.size.toLong())
        return LocalScan(link.id, link.local, "ok", vol, entries, unreadable, errors)
    }
}
