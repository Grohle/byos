package org.hibyos.install.core.sync

import org.hibyos.install.core.library.TagReader
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.LinkOption

/**
 * Scans one link's folder on this computer (sync-scan): every file of the link's kind, with
 * size, modification time and (for audio) tags. Tags, hashes and fingerprints of files whose
 * size and time did not change come from the previous scan. A folder that cannot be read is
 * recorded as unreadable, never as empty, so nothing under it is ever "deleted on the computer".
 */
object LocalScanner {
    /** The volume of [dir]: the Windows volume serial, or the Unix device; null when unknown. */
    fun volumeId(dir: File): String? {
        return try {
            val p = dir.toPath()
            if (!Files.exists(p)) return null
            try {
                (Files.getFileStore(p).getAttribute("volume:vsn") as? Number)?.let { return "vsn:" + Integer.toHexString(it.toInt()) }
            } catch (e: Exception) {}
            try {
                (Files.getAttribute(p, "unix:dev") as? Number)?.let { return "dev:" + it.toLong() }
            } catch (e: Exception) {}
            null
        } catch (e: Exception) { null }
    }

    fun tagsOf(f: File): LocalTags? = try {
        val i = TagReader.read(f)
        LocalTags(i.title, i.artist, i.albumArtist, i.album, i.genre, TagReader.leadingInt(i.track), TagReader.leadingInt(i.disc),
            TagReader.leadingInt(i.date), i.durationMs)
    } catch (e: IOException) { null } catch (e: RuntimeException) { null }

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
        val files = ArrayList<Pair<String, File>>()
        val unreadable = ArrayList<String>()
        var errors = 0
        var folders = 0L
        fun walk(dir: File, rel: String, list: Array<File>?) {
            if (cancelled()) throw SyncException("cancelled", "cancelled")
            folders++
            progress("scan", dir.path, folders, 0)
            if (list == null) { unreadable += rel; return }
            for (f in list) {
                val n = f.name
                if (n.startsWith(".")) continue
                val r = SyncRules.nfc(if (rel.isEmpty()) n else "$rel/$n")
                if (Files.isSymbolicLink(f.toPath())) continue
                if (f.isDirectory) {
                    if (r.count { it == '/' } < 64) walk(f, r, f.listFiles())
                } else if (Files.isRegularFile(f.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                    if (Kinds.relevant(link.kind, n)) files += r to f
                } else if (!f.exists()) errors++
            }
        }
        walk(root, "", top)
        val entries = LinkedHashMap<String, LocalEntry>(files.size * 2)
        val readTagsFor = link.kind == Kinds.MUSIC || link.kind == Kinds.AUDIOBOOKS
        for ((k, pr) in files.withIndex()) {
            if (cancelled()) throw SyncException("cancelled", "cancelled")
            val (rel, f) = pr
            val size = f.length()
            val mtime = f.lastModified() / 1000
            if (mtime == 0L && !f.exists()) { errors++; continue }
            val old = prev?.entries?.get(rel)?.takeIf { !full && it.size == size && it.mtime == mtime }
            val media = Kinds.isMedia(link.kind, f.name)
            val tags = when {
                !media || !readTagsFor -> null
                old != null && old.tags != null -> old.tags
                else -> { if (k % 50 == 0) progress("tags", f.path, k.toLong(), files.size.toLong()); readTags(f) }
            }
            entries[rel] = LocalEntry(rel, size, mtime, tags, old?.sha, old?.qfp)
        }
        progress("tags", null, files.size.toLong(), files.size.toLong())
        return LocalScan(link.id, link.local, "ok", vol, entries, unreadable, errors)
    }
}
