package org.hibyos.install.core.library

import org.hibyos.install.core.btp.BtpPaths
import java.text.Normalizer

/** A file the sync can copy: a track of the computer's music folders. [src] is its absolute local path. */
class SourceTrack(val src: String, val rel: String, val track: IndexTrack)

/** What the user chose to keep on the HiBy (iTunes' Music tab). Names compare case- and accent-insensitively. */
class SyncSelection(
    val all: Boolean = true,
    val artists: Set<String> = emptySet(),
    /** Album keys: [albumKey] (artist, title). */
    val albums: Set<String> = emptySet(),
    val genres: Set<String> = emptySet(),
    /** Local files picked one by one (absolute paths, also used for playlists' members). */
    val files: Set<String> = emptySet(),
    val books: Boolean = true,
) {
    fun wants(t: SourceTrack): Boolean {
        if (t.track.book) return books
        if (all) return true
        val k = t.track
        return LibraryIndex.fold(k.groupArtist) in artists || albumKey(k.groupArtist, k.album) in albums ||
            (k.genre.isNotEmpty() && LibraryIndex.fold(k.genre) in genres) || t.src in files
    }

    companion object {
        fun albumKey(artist: String, title: String) = LibraryIndex.fold(artist) + "\u0000" + LibraryIndex.fold(title)
    }
}

/** One device file synced before (ByOS/sync/manifest.json). */
class ManifestEntry(val dest: String, val src: String, val srcSize: Long, val srcMtime: Long, val size: Long, val covers: String)

/** A file on the HiBy, from `ls --recursive`. */
class RemoteFile(val path: String, val size: Long, val mtime: Long)

class PlanItem(val action: String, val src: String?, val dest: String, val size: Long, val mtime: Long, val reason: String,
               val title: String? = null, val oldSize: Long = 0)

class SyncPlan(val dest: String, val layout: String, val covers: String, val items: List<PlanItem>, val sdFree: Long) {
    val add get() = items.count { it.action == "add" }
    val update get() = items.count { it.action == "update" }
    val skip get() = items.count { it.action == "skip" }
    val remove get() = items.count { it.action == "remove" }
    /** Bytes the card needs: new files, plus growth of updated ones, minus removed ones. */
    val bytesAdd: Long get() = items.filter { it.action == "add" }.sumOf { it.size } +
        items.filter { it.action == "update" }.sumOf { it.size - it.oldSize }
    val bytesRemove: Long get() = items.filter { it.action == "remove" }.sumOf { it.size }
    /** It fits with [SyncPlanner.MARGIN] to spare. */
    val fits: Boolean get() = bytesAdd - bytesRemove + SyncPlanner.MARGIN <= sdFree

    fun toMap(): Map<String, Any?> = linkedMapOf(
        "format" to 1, "dest" to dest, "layout" to layout, "covers" to covers,
        "add" to add, "update" to update, "skip" to skip, "remove" to remove,
        "bytes_add" to bytesAdd, "bytes_remove" to bytesRemove, "sd_free" to sdFree, "fits" to fits,
        "items" to items.map { linkedMapOf("action" to it.action, "src" to it.src, "dest" to it.dest, "size" to it.size,
            "mtime" to it.mtime, "reason" to it.reason, "title" to it.title, "old_size" to it.oldSize) },
    )
}

/**
 * Works out what a music sync copies, replaces and removes (docs/BYOS_CLI.md, sync-plan).
 *
 * - Layout "artist-album": `<dest>/<Album artist or Artist>/<Album>/<NN> <Title>.<ext>`
 *   (`<D>-<NN>` on albums with several discs); "keep": the path under the source folder.
 *   Audiobooks go to `Audiobooks/<Book>/<file name>`.
 * - Names follow SYNC_PROTOCOL 9.2: NFC, no `\ : * ? " < > |` or control characters, no
 *   leading dot, no trailing dot or space, at most 255 bytes; two tracks that would get the
 *   same name (FAT compares without case) get " (2)", " (3)"...
 * - A file is skipped when the manifest says it came from the same source file (size and
 *   modification time) with the same cover choice and the HiBy still has it at that size;
 *   without a manifest entry, when the HiBy has a file of the same size there (the copy then
 *   checks the SHA-256 itself).
 * - "Remove unselected" only ever removes files the manifest lists (copied by a sync before),
 *   never other files.
 */
object SyncPlanner {
    const val MARGIN = 64L shl 20
    private const val FORBIDDEN = "\\/:*?\"<>|"

    /** One clean path component, or [fallback] when nothing is left. */
    fun cleanName(s: String, fallback: String = "Unknown"): String {
        var n = Normalizer.normalize(s, Normalizer.Form.NFC)
        n = buildString { for (c in n) append(if (c.code < 0x20 || c.code == 0x7f || c in FORBIDDEN) '_' else c) }
        n = n.trim().trimStart('.').trimEnd('.', ' ').trim()
        n = BtpPaths.clipUtf8(n, 200).trimEnd('.', ' ')
        return n.ifEmpty { fallback }
    }

    private fun pad(n: Int) = if (n < 10) "0$n" else n.toString()

    /** Where [t] goes on the HiBy (before collisions are resolved). */
    fun destFor(t: SourceTrack, dest: String, layout: String, multiDisc: Boolean): String {
        val ext = t.src.substringAfterLast('.', "").lowercase()
        val k = t.track
        if (k.book) {
            val book = cleanName(k.album.ifEmpty { t.rel.substringBeforeLast('/', "").substringAfterLast('/') }, "Audiobook")
            return BtpPaths.join("Audiobooks", book, cleanName(t.rel.substringAfterLast('/'), "Part.$ext"))
        }
        if (layout == "keep") return BtpPaths.join(dest, *t.rel.split('/').map { cleanName(it) }.toTypedArray())
        val artist = cleanName(k.groupArtist, "Unknown Artist")
        val album = cleanName(k.album, "Unknown Album")
        val num = when {
            k.track <= 0 -> ""
            multiDisc && k.disc > 0 -> "${k.disc}-${pad(k.track)} "
            else -> "${pad(k.track)} "
        }
        val title = cleanName(num + k.title, "Track")
        return BtpPaths.join(dest, artist, album, "$title.$ext")
    }

    fun plan(
        sources: List<SourceTrack>, sel: SyncSelection, dest: String, layout: String, covers: String,
        remote: List<RemoteFile>, manifest: List<ManifestEntry>, removeUnselected: Boolean, sdFree: Long,
    ): SyncPlan {
        require(layout == "artist-album" || layout == "keep") { "layout is artist-album or keep" }
        val chosen = sources.filter { sel.wants(it) }
        // albums with more than one disc number get the disc in front of the track number
        val discs = HashMap<String, MutableSet<Int>>()
        for (t in chosen) discs.getOrPut(SyncSelection.albumKey(t.track.groupArtist, t.track.album)) { HashSet() } += t.track.disc
        val remoteBy = remote.associateBy { it.path.lowercase() }
        val manBySrc = manifest.groupBy { it.src }
        val manByDest = manifest.associateBy { it.dest.lowercase() }
        val used = HashSet<String>()
        val items = ArrayList<PlanItem>()
        for (t in chosen.sortedBy { it.src }) {
            // a file synced before keeps its place on the HiBy
            val prev = manBySrc[t.src]?.firstOrNull { it.dest.lowercase() !in used }
            var d = prev?.dest ?: destFor(t, dest, layout, (discs[SyncSelection.albumKey(t.track.groupArtist, t.track.album)]?.size ?: 0) > 1)
            if (prev == null) {
                var n = 2
                val base = d.substringBeforeLast('.'); val ext = d.substringAfterLast('.', "")
                // taken by another track of this sync, by a file a sync copied from another
                // source, or by a different file the user put there (never overwritten)
                fun taken(p: String): Boolean {
                    val k = p.lowercase()
                    if (k in used) return true
                    val on = remoteBy[k] ?: return false
                    val m = manByDest[k]
                    return if (m != null) m.src != t.src else on.size != t.track.size
                }
                while (taken(d)) {
                    d = "$base (${n++})" + if (ext.isNotEmpty()) ".$ext" else ""
                }
            }
            used += d.lowercase()
            val on = remoteBy[d.lowercase()]
            val m = manByDest[d.lowercase()]
            val title = t.track.title
            val item = when {
                on == null -> PlanItem("add", t.src, d, t.track.size, t.track.mtime, if (prev != null) "missing" else "new", title)
                m != null && m.src == t.src && m.srcSize == t.track.size && m.srcMtime == t.track.mtime && m.covers == covers && on.size == m.size ->
                    PlanItem("skip", t.src, d, on.size, t.track.mtime, "unchanged", title)
                m == null && on.size == t.track.size ->
                    PlanItem("skip", t.src, d, on.size, t.track.mtime, "same_size", title)
                else -> PlanItem("update", t.src, d, t.track.size, t.track.mtime, if (m?.covers != null && m.covers != covers) "covers" else "changed", title, on.size)
            }
            items += item
        }
        if (removeUnselected) {
            for (m in manifest) {
                if (m.dest.lowercase() in used) continue
                val on = remoteBy[m.dest.lowercase()] ?: continue
                items += PlanItem("remove", null, m.dest, on.size, on.mtime, "unselected", m.dest.substringAfterLast('/').substringBeforeLast('.'))
            }
        }
        return SyncPlan(dest, layout, covers, items, sdFree)
    }
}

/**
 * Playlists as the HiBy reads them: `Playlists/<name>.m3u8`, UTF-8 without BOM, `\r\n` line
 * ends, an `#EXTM3U` header and paths relative to the playlist's folder. Reading also accepts
 * `a:\` paths, absolute card paths and backslashes.
 */
object M3uWriter {
    const val DIR = "Playlists"

    fun pathFor(name: String) = BtpPaths.join(DIR, SyncPlanner.cleanName(name, "Playlist") + ".m3u8")

    /** The text of a playlist at [playlistPath] listing [entries] (card paths). */
    fun write(playlistPath: String, entries: List<String>): String {
        val dir = playlistPath.substringBeforeLast('/', "").split('/').filter { it.isNotEmpty() }
        val sb = StringBuilder("#EXTM3U\r\n")
        for (e in entries) sb.append(relative(dir, e)).append("\r\n")
        return sb.toString()
    }

    private fun relative(dir: List<String>, target: String): String {
        val t = target.split('/').filter { it.isNotEmpty() }
        var common = 0
        while (common < dir.size && common < t.size - 1 && dir[common].equals(t[common], ignoreCase = true)) common++
        return (List(dir.size - common) { ".." } + t.drop(common)).joinToString("/")
    }

    /** The card paths a playlist at [playlistPath] lists. */
    fun read(playlistPath: String, text: String): List<String> {
        val dir = playlistPath.substringBeforeLast('/', "").split('/').filter { it.isNotEmpty() }
        return text.removePrefix("\uFEFF").lines().map { it.trim() }.filter { it.isNotEmpty() && !it.startsWith("#") }.mapNotNull { l ->
            val p = l.replace('\\', '/')
            val parts: List<String> = when {
                p.length >= 3 && p[1] == ':' && p[2] == '/' -> p.substring(3).split('/')
                p.startsWith("/") -> p.split('/')
                else -> dir + p.split('/')
            }
            val out = ArrayList<String>()
            for (c in parts) when (c) { "", "." -> {}; ".." -> if (out.isNotEmpty()) out.removeAt(out.size - 1); else -> out += c }
            if (out.isEmpty()) null else Normalizer.normalize(out.joinToString("/"), Normalizer.Form.NFC)
        }
    }
}
