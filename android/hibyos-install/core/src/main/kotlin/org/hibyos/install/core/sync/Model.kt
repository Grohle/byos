package org.hibyos.install.core.sync

import org.hibyos.install.core.Json
import org.hibyos.install.core.library.LibraryBuilder
import org.hibyos.install.core.library.LibraryIndex
import org.hibyos.install.core.library.SyncSelection

/**
 * Folder sync (docs/SYNC_LIBRARY_DESIGN.md section 3): the data the engine keeps on the computer
 * (links.json, local.json, state.json, the run journals) and on the card (manifest-v2, card
 * journals, lock.json, card.json), and the plan it shows before anything changes.
 */
class SyncException(val code: String, message: String, val extra: Map<String, Any?> = emptyMap()) : Exception(message)

/** The four kinds of linked folders, in the order "Sync all" runs them. */
object Kinds {
    const val MUSIC = "music"
    const val AUDIOBOOKS = "audiobooks"
    const val EBOOKS = "ebooks"
    const val VIDEO = "video"
    val ALL = listOf(MUSIC, AUDIOBOOKS, EBOOKS, VIDEO)

    fun check(k: String): String = if (k in ALL) k else throw SyncException("usage", "unknown kind \"$k\" (music, audiobooks, ebooks or video)")

    /** "all" or one kind, as --kind takes it. */
    fun parse(k: String?): List<String> = when (k) { null, "all" -> ALL; else -> listOf(check(k)) }

    fun defaultRoot(kind: String) = when (kind) {
        MUSIC -> "Music"; AUDIOBOOKS -> "Audiobooks"; EBOOKS -> "Books"; VIDEO -> "Video"
        else -> throw SyncException("usage", "unknown kind $kind")
    }

    private val AUDIO = LibraryBuilder.AUDIO_EXT.toSet()
    private val EBOOK = setOf("epub", "pdf", "txt", "fb2", "mobi", "azw", "azw3", "cbz", "cbr", "djvu")
    private val VIDEO_EXT = setOf("mp4", "m4v", "mkv", "avi", "mov", "webm", "wmv", "flv", "3gp", "ts", "mpg", "mpeg")
    val COVER_NAMES = setOf("cover.jpg", "folder.jpg", "front.jpg", "cover.jpeg", "folder.jpeg", "front.jpeg",
        "cover.png", "folder.png", "front.png", "albumart.jpg")

    fun ext(name: String) = name.substringAfterLast('.', "").lowercase()

    /** A file the kind syncs as an item of its own (a song, a book, a film). */
    fun isMedia(kind: String, name: String): Boolean {
        val e = ext(name)
        return when (kind) {
            MUSIC -> e in AUDIO && e != "m4b"   // .m4b files are audiobooks: an audiobooks link carries them
            AUDIOBOOKS -> e in AUDIO
            EBOOKS -> e in EBOOK
            VIDEO -> e in VIDEO_EXT
            else -> false
        }
    }

    /** Sidecar that belongs to one media file with the same base name (lyrics, subtitles). */
    fun isTrackSidecar(kind: String, name: String): Boolean {
        val e = ext(name)
        return when (kind) {
            MUSIC, AUDIOBOOKS -> e == "lrc"
            VIDEO -> e in setOf("srt", "ass", "ssa", "vtt", "sub")
            else -> false
        }
    }

    /** Sidecar that belongs to a folder (cover image, CUE sheet, album playlist). */
    fun isFolderSidecar(kind: String, name: String): Boolean {
        val n = name.lowercase()
        return when (kind) {
            MUSIC -> n in COVER_NAMES || ext(n) in setOf("cue", "m3u", "m3u8")
            AUDIOBOOKS -> n in COVER_NAMES || ext(n) == "cue"
            EBOOKS, VIDEO -> n in COVER_NAMES
            else -> false
        }
    }

    fun isSidecar(kind: String, name: String) = isTrackSidecar(kind, name) || isFolderSidecar(kind, name)

    /** Whether a file belongs to [kind] at all (anything else is never planned, never deleted). */
    fun relevant(kind: String, name: String) = isMedia(kind, name) || isSidecar(kind, name)
}

/** What a link copies (the branch's SyncSelection, kept per link). Names compare without case or accents. */
class LinkSelection(
    val all: Boolean = true,
    val artists: List<String> = emptyList(),
    /** Pairs (album artist or artist, album title). */
    val albums: List<Pair<String, String>> = emptyList(),
    val genres: List<String> = emptyList(),
    /** Files chosen one by one, as paths relative to the link's folder. */
    val files: List<String> = emptyList(),
) {
    fun toSyncSelection() = SyncSelection(all, artists.map { LibraryIndex.fold(it) }.toSet(),
        albums.map { SyncSelection.albumKey(it.first, it.second) }.toSet(), genres.map { LibraryIndex.fold(it) }.toSet(), files.toSet(), true)

    fun toMap(): Map<String, Any?> = linkedMapOf("mode" to if (all) "all" else "chosen", "artists" to artists,
        "albums" to albums.map { linkedMapOf("artist" to it.first, "title" to it.second) }, "genres" to genres, "files" to files)

    companion object {
        fun fromMap(m: Map<String, Any?>?): LinkSelection {
            if (m == null) return LinkSelection()
            fun names(k: String) = (m[k] as? List<*>)?.map { it.toString() } ?: emptyList()
            val albums = (m["albums"] as? List<*>)?.map { v -> val o = Json.obj(v); (Json.optStr(o, "artist") ?: "") to (Json.optStr(o, "title") ?: "") } ?: emptyList()
            val all = when (m["mode"]) { "chosen" -> false; "all" -> true; else -> m["all"] != false }
            return LinkSelection(all, names("artists"), albums, names("genres"), names("files"))
        }
    }
}

/** One linked folder (links.json). */
class Link(
    val id: String,
    val kind: String,
    /** The folder on this computer (absolute path). */
    var local: String,
    var cardRoot: String,
    var layout: String,
    var selection: LinkSelection = LinkSelection(),
    var removeUnselected: Boolean = false,
    /** keep | delete | ask: what happens to files that are only on the player. */
    var playerOnly: String = "keep",
    var includeSidecars: Boolean = true,
    var exclude: List<String> = emptyList(),
    var enabled: Boolean = true,
    var label: String = "",
    /** optimize | keep (music): send covers as 500 px baseline JPEGs. */
    var covers: String = "optimize",
    /** The local folder's volume (Windows volume serial, Unix device), for DeleteGuard. */
    var volumeId: String? = null,
    var entriesLast: Long = 0,
) {
    fun toMap(): Map<String, Any?> = linkedMapOf("id" to id, "kind" to kind, "local" to linkedMapOf("type" to "path", "path" to local),
        "root_marker" to linkedMapOf("volume_id" to volumeId, "entries_last" to entriesLast),
        "card_root" to cardRoot, "layout" to layout, "selection" to selection.toMap(), "remove_unselected" to removeUnselected,
        "player_only" to playerOnly, "include_sidecars" to includeSidecars, "exclude" to exclude, "enabled" to enabled,
        "label" to label, "covers" to covers)

    companion object {
        val LAYOUTS = setOf("artist-album", "keep")
        val POLICIES = setOf("keep", "delete", "ask")

        fun fromMap(m: Map<String, Any?>): Link {
            val loc = m["local"]
            val path = when (loc) { is String -> loc; is Map<*, *> -> Json.optStr(Json.obj(loc), "path") ?: ""; else -> "" }
            val marker = (m["root_marker"] as? Map<*, *>)?.let { Json.obj(it) }
            return Link(Json.str(m, "id"), Kinds.check(Json.str(m, "kind")), path, Json.optStr(m, "card_root") ?: Kinds.defaultRoot(Json.str(m, "kind")),
                Json.optStr(m, "layout") ?: "keep", LinkSelection.fromMap((m["selection"] as? Map<*, *>)?.let { Json.obj(it) }),
                m["remove_unselected"] == true, Json.optStr(m, "player_only") ?: "keep", m["include_sidecars"] != false,
                (m["exclude"] as? List<*>)?.map { it.toString() } ?: emptyList(), m["enabled"] != false, Json.optStr(m, "label") ?: "",
                Json.optStr(m, "covers") ?: "optimize", marker?.let { Json.optStr(it, "volume_id") }, marker?.let { Json.optLong(it, "entries_last", 0) } ?: 0)
        }
    }
}

/** Tags of a local media file, as far as the card layout needs them. */
class LocalTags(
    val title: String?, val artist: String?, val albumArtist: String?, val album: String?, val genre: String?,
    val track: Int, val disc: Int, val year: Int, val durationMs: Long,
) {
    fun toMap(): Map<String, Any?> = linkedMapOf("title" to title, "artist" to artist, "albumArtist" to albumArtist, "album" to album,
        "genre" to genre, "track" to track, "disc" to disc, "year" to year, "duration" to durationMs)

    companion object {
        fun fromMap(m: Map<String, Any?>) = LocalTags(Json.optStr(m, "title"), Json.optStr(m, "artist"), Json.optStr(m, "albumArtist"),
            Json.optStr(m, "album"), Json.optStr(m, "genre"), Json.optLong(m, "track", 0).toInt(), Json.optLong(m, "disc", 0).toInt(),
            Json.optLong(m, "year", 0).toInt(), Json.optLong(m, "duration", 0))
    }
}

/** One file of a link's folder (local.json). [rel]: NFC, '/' separated, relative to the link's folder. */
class LocalEntry(val rel: String, val size: Long, val mtime: Long, val tags: LocalTags? = null, var sha: String? = null, var qfp: String? = null)

/** The result of scanning one link's folder. */
class LocalScan(
    val linkId: String,
    val root: String,
    /** ok | missing | not_dir | unreadable | empty */
    val status: String,
    val volumeId: String?,
    val entries: Map<String, LocalEntry>,
    /** Folders (relative) that could not be read: nothing under them is ever "deleted on the computer". */
    val unreadableDirs: List<String> = emptyList(),
    val errors: Int = 0,
    val time: Long = System.currentTimeMillis() / 1000,
) {
    fun underUnreadable(rel: String) = unreadableDirs.any { it.isEmpty() || rel.startsWith("$it/") }

    fun toMap(): Map<String, Any?> = linkedMapOf("format" to 1, "link" to linkId, "root" to root, "status" to status, "volume_id" to volumeId,
        "time" to time, "errors" to errors, "unreadable" to unreadableDirs,
        "files" to entries.values.sortedBy { it.rel }.associate { e ->
            e.rel to linkedMapOf<String, Any?>("size" to e.size, "mtime" to e.mtime).apply {
                if (e.tags != null) put("tags", e.tags.toMap())
                if (e.sha != null) put("sha", e.sha)
                if (e.qfp != null) put("qfp", e.qfp)
            }
        })

    companion object {
        fun fromMap(m: Map<String, Any?>): LocalScan {
            val files = LinkedHashMap<String, LocalEntry>()
            for ((rel, v) in Json.obj(m["files"])) {
                val e = Json.obj(v)
                files[rel] = LocalEntry(rel, Json.optLong(e, "size", 0), Json.optLong(e, "mtime", 0),
                    (e["tags"] as? Map<*, *>)?.let { LocalTags.fromMap(Json.obj(it)) }, Json.optStr(e, "sha"), Json.optStr(e, "qfp"))
            }
            return LocalScan(Json.optStr(m, "link") ?: "", Json.optStr(m, "root") ?: "", Json.optStr(m, "status") ?: "ok",
                Json.optStr(m, "volume_id"), files, (m["unreadable"] as? List<*>)?.map { it.toString() } ?: emptyList(),
                Json.optLong(m, "errors", 0).toInt(), Json.optLong(m, "time", 0))
        }
    }
}

/**
 * One file a sync put on the card (manifest-v2 entry, also kept in state.json). The identity of
 * its source is ([link], [rel]); [origin] is the client that wrote it.
 */
class Tracked(
    val dest: String,
    val link: String?,
    val rel: String,
    val origin: String?,
    val srcSize: Long,
    val srcMtime: Long,
    val sentSize: Long,
    val sentSha: String?,
    val cardMtime: Long,
    val covers: String,
    val time: Long,
    val srcSha: String? = null,
    val sentQfp: String? = null,
    /** v1 entries whose folder matched no link yet: the absolute source path. */
    val srcAbs: String? = null,
) {
    fun copy(dest: String = this.dest, link: String? = this.link, rel: String = this.rel, origin: String? = this.origin,
             srcSize: Long = this.srcSize, srcMtime: Long = this.srcMtime, sentSize: Long = this.sentSize, sentSha: String? = this.sentSha,
             cardMtime: Long = this.cardMtime, covers: String = this.covers, time: Long = this.time, srcSha: String? = this.srcSha,
             sentQfp: String? = this.sentQfp, srcAbs: String? = this.srcAbs) =
        Tracked(dest, link, rel, origin, srcSize, srcMtime, sentSize, sentSha, cardMtime, covers, time, srcSha, sentQfp, srcAbs)

    fun toMap(): Map<String, Any?> = linkedMapOf<String, Any?>("link" to link, "rel" to rel, "origin" to origin, "src_size" to srcSize,
        "src_mtime" to srcMtime, "sent_size" to sentSize, "sent_sha" to sentSha, "card_mtime" to cardMtime, "covers" to covers, "time" to time).apply {
        if (srcSha != null) put("src_sha", srcSha)
        if (sentQfp != null) put("sent_qfp", sentQfp)
        if (srcAbs != null) put("src_abs", srcAbs)
    }

    companion object {
        fun fromMap(dest: String, m: Map<String, Any?>) = Tracked(dest, Json.optStr(m, "link"), Json.optStr(m, "rel") ?: "", Json.optStr(m, "origin"),
            Json.optLong(m, "src_size", -1), Json.optLong(m, "src_mtime", -1), Json.optLong(m, "sent_size", -1), Json.optStr(m, "sent_sha"),
            Json.optLong(m, "card_mtime", 0), Json.optStr(m, "covers") ?: "keep", Json.optLong(m, "time", 0), Json.optStr(m, "src_sha"),
            Json.optStr(m, "sent_qfp"), Json.optStr(m, "src_abs"))
    }
}

/** A file on the card, from LIST. */
class CardFile(val path: String, val size: Long, val mtime: Long)

/** One line of the plan the user sees (only the items that do something: skips are counted). */
class PlanItem(
    val id: String,
    val kind: String,
    val link: String,
    /** add | update | move | adopt | record | conflict | player_only | blocked */
    val action: String,
    val reason: String,
    val srcRel: String?,
    val dest: String,
    val oldDest: String? = null,
    /** Local size for copies; card size for player-only items. */
    val size: Long = 0,
    /** Local modification time (seconds) the plan saw. */
    val mtime: Long = 0,
    /** Size of the file now on the card at [dest] (or [oldDest] for moves), -1 when none. */
    val cardSize: Long = -1,
    val cardMtime: Long = 0,
    val title: String? = null,
    val origin: String? = null,
    val defaultChoice: String? = null,
    val choices: List<String> = emptyList(),
    /** Sidecars follow the choice of their track or folder item. */
    val parent: String? = null,
    /** A move that also sends the new content. */
    val thenUpdate: Boolean = false,
    val covers: String? = null,
    /** For record-only items: what to record (adopt, rename, mtime-only change, forget). */
    val record: Map<String, Any?>? = null,
    val extra: Map<String, Any?> = emptyMap(),
) {
    fun toMap(): Map<String, Any?> = linkedMapOf<String, Any?>("id" to id, "kind" to kind, "link" to link, "action" to action, "reason" to reason,
        "src_rel" to srcRel, "dest" to dest, "old_dest" to oldDest, "size" to size, "mtime" to mtime, "card_size" to cardSize,
        "card_mtime" to cardMtime, "title" to title, "origin" to origin, "default_choice" to defaultChoice, "choices" to choices).apply {
        if (parent != null) put("parent", parent)
        if (thenUpdate) put("then_update", true)
        if (covers != null) put("covers", covers)
        if (record != null) put("record", record)
        putAll(extra)
    }

    companion object {
        fun fromMap(m: Map<String, Any?>) = PlanItem(Json.str(m, "id"), Json.str(m, "kind"), Json.str(m, "link"), Json.str(m, "action"),
            Json.optStr(m, "reason") ?: "", Json.optStr(m, "src_rel"), Json.str(m, "dest"), Json.optStr(m, "old_dest"), Json.optLong(m, "size", 0),
            Json.optLong(m, "mtime", 0), Json.optLong(m, "card_size", -1), Json.optLong(m, "card_mtime", 0), Json.optStr(m, "title"),
            Json.optStr(m, "origin"), Json.optStr(m, "default_choice"), (m["choices"] as? List<*>)?.map { it.toString() } ?: emptyList(),
            Json.optStr(m, "parent"), m["then_update"] == true, Json.optStr(m, "covers"), (m["record"] as? Map<*, *>)?.let { Json.obj(it) },
            m.filterKeys { it in EXTRA_KEYS })

        private val EXTRA_KEYS = setOf("card_changed", "needs_decision", "guard")
    }
}

object PlayerOnly {
    const val DELETED = "deleted_on_computer"
    const val EXCLUDED = "excluded_on_computer"
    const val OTHER_DEVICE = "from_other_device"
    const val ADDED = "added_on_player"
    const val DOWNLOADED = "downloaded_on_player"
    val ALL = listOf(DELETED, EXCLUDED, OTHER_DEVICE, ADDED, DOWNLOADED)
}

/** DeleteGuard's verdict for one link (section 3.3.3). */
class Guard(val tripped: Boolean, val reason: String?) {
    fun toMap(): Map<String, Any?> = linkedMapOf("tripped" to tripped, "reason" to reason)
}

/** The per-link outcome of the diff. */
class LinkPlan(val link: Link, val items: List<PlanItem>, val skip: Int, val guard: Guard, val tracked: Int,
               val counts: Map<String, Int>, val bytesFingerprint: Long)
