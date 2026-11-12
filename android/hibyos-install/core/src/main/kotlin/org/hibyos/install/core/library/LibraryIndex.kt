package org.hibyos.install.core.library

import java.io.File
import java.text.Normalizer

/** One track of a [LibraryIndex]. [path] is the path on the card ("Music/ABBA/01.flac"). */
class IndexTrack(
    val id: Long, val path: String, val title: String, val artist: String, val albumArtist: String, val album: String,
    val genre: String, val year: Int, val track: Int, val disc: Int, val durationMs: Long, val size: Long,
    val mtime: Long, val ctime: Long, val sampleRate: Int, val bits: Int, val kbps: Int, val lossless: Boolean,
    val hasArt: Boolean, val book: Boolean,
) {
    val format: String get() = path.substringAfterLast('.', "").uppercase()
    /** The name the library groups by: the album artist, else the artist. */
    val groupArtist: String get() = albumArtist.ifEmpty { artist }
}

class IndexPlaylist(val path: String, val name: String)

class IndexAlbum(val title: String, val artist: String, val year: Int, val genre: String, val tracks: List<Int>, val art: String?)

class IndexArtist(val name: String, val albums: List<Int>, val tracks: Int)

class IndexBook(val name: String, val path: String, val tracks: List<Int>, val durationMs: Long, val size: Long)

/**
 * The HiBy's music library as the desktop app shows it: tracks grouped into albums, artists,
 * genres, audiobooks and playlists. Made from a snapshot of the player's own database
 * ([fromSnapshot]) or from tags read file by file ([fromTracks]). The grouping rules are the
 * ones of the Android app's Library screen: text without the player's trailing NUL, `a:\`
 * paths turned into card paths, "The " ignored when sorting, empty names last.
 */
class LibraryIndex(
    val source: String,
    val revision: String?,
    val time: Long,
    tracksIn: List<IndexTrack>,
    playlistsIn: List<IndexPlaylist>,
) {
    val tracks: List<IndexTrack> = tracksIn.sortedWith { a, b -> collator.compare(a.title, b.title).let { if (it != 0) it else a.path.compareTo(b.path) } }
    val playlists: List<IndexPlaylist> = playlistsIn.sortedWith { a, b -> collator.compare(a.name, b.name) }
    val albums: List<IndexAlbum>
    val artists: List<IndexArtist>
    val genres: List<Pair<String, Int>>
    val books: List<IndexBook>

    init {
        val music = tracks.indices.filter { !tracks[it].book }
        val byAlbum = LinkedHashMap<String, MutableList<Int>>()
        for (i in music) {
            val t = tracks[i]
            byAlbum.getOrPut(fold(t.groupArtist) + "\u0000" + fold(t.album)) { ArrayList() } += i
        }
        albums = byAlbum.values.map { list ->
            val sorted = list.sortedWith(compareBy<Int>({ tracks[it].disc }, { if (tracks[it].track > 0) tracks[it].track else Int.MAX_VALUE }, { tracks[it].path.lowercase() }))
            val first = tracks[sorted[0]]
            val genre = sorted.map { tracks[it].genre }.filter { it.isNotEmpty() }.groupingBy { it }.eachCount().maxByOrNull { it.value }?.key ?: ""
            IndexAlbum(first.album, first.groupArtist, sorted.maxOf { tracks[it].year }, genre, sorted,
                sorted.firstOrNull { tracks[it].hasArt }?.let { tracks[it].path } ?: first.path)
        }.sortedWith { a, b -> collator.compare(a.title, b.title).let { if (it != 0) it else collator.compare(a.artist, b.artist) } }
        val byArtist = LinkedHashMap<String, MutableList<Int>>()
        for ((i, a) in albums.withIndex()) byArtist.getOrPut(fold(a.artist)) { ArrayList() } += i
        artists = byArtist.values.map { list ->
            val sorted = list.sortedWith(compareBy<Int>({ if (albums[it].year > 0) albums[it].year else Int.MAX_VALUE }, { albums[it].title.lowercase() }))
            IndexArtist(albums[sorted[0]].artist, sorted, sorted.sumOf { albums[it].tracks.size })
        }.sortedWith { a, b -> collator.compare(a.name, b.name) }
        val g = LinkedHashMap<String, Pair<String, Int>>()
        for (i in music) {
            val n = tracks[i].genre
            if (n.isEmpty()) continue
            val k = fold(n)
            g[k] = (g[k]?.first ?: n) to ((g[k]?.second ?: 0) + 1)
        }
        genres = g.values.sortedWith { a, b -> collator.compare(a.first, b.first) }
        // an audiobook is a folder of book tracks, or one file
        val byBook = LinkedHashMap<String, MutableList<Int>>()
        for (i in tracks.indices) if (tracks[i].book) {
            val t = tracks[i]
            val key = if (t.path.lowercase().endsWith(".m4b")) t.path else t.path.substringBeforeLast('/', t.path)
            byBook.getOrPut(key) { ArrayList() } += i
        }
        books = byBook.map { (key, list) ->
            val sorted = list.sortedWith(compareBy<Int>({ tracks[it].disc }, { tracks[it].track }, { tracks[it].path.lowercase() }))
            val first = tracks[sorted[0]]
            val name = if (sorted.size == 1 && key == first.path) first.album.ifEmpty { first.title } else first.album.ifEmpty { key.substringAfterLast('/') }
            IndexBook(name, key, sorted, sorted.sumOf { tracks[it].durationMs }, sorted.sumOf { tracks[it].size })
        }.sortedWith { a, b -> collator.compare(a.name, b.name) }
    }

    /** The JSON form written to index.json (docs/BYOS_CLI.md, lib-index). */
    fun toMap(): Map<String, Any?> = linkedMapOf(
        "format" to 1, "source" to source, "revision" to revision, "time" to time,
        "counts" to linkedMapOf("tracks" to tracks.count { !it.book }, "albums" to albums.size, "artists" to artists.size,
            "genres" to genres.size, "books" to books.size, "playlists" to playlists.size),
        "tracks" to tracks.map { t ->
            linkedMapOf("id" to t.id, "path" to t.path, "title" to t.title, "artist" to t.artist, "albumArtist" to t.albumArtist,
                "album" to t.album, "genre" to t.genre, "year" to t.year, "track" to t.track, "disc" to t.disc,
                "duration" to t.durationMs, "format" to t.format, "sr" to t.sampleRate, "bits" to t.bits, "kbps" to t.kbps,
                "lossless" to t.lossless, "size" to t.size, "mtime" to t.mtime, "ctime" to t.ctime, "art" to t.hasArt, "book" to t.book)
        },
        "albums" to albums.map { a ->
            linkedMapOf("title" to a.title, "artist" to a.artist, "year" to a.year, "genre" to a.genre, "tracks" to a.tracks, "art" to a.art)
        },
        "artists" to artists.map { linkedMapOf("name" to it.name, "albums" to it.albums, "tracks" to it.tracks) },
        "genres" to genres.map { linkedMapOf("name" to it.first, "tracks" to it.second) },
        "books" to books.map { linkedMapOf("name" to it.name, "path" to it.path, "tracks" to it.tracks, "duration" to it.durationMs, "size" to it.size) },
        "playlists" to playlists.map { linkedMapOf("path" to it.path, "name" to it.name) },
    )

    companion object {
        /** Top folders whose files count as audiobooks (as the Android app and the player's Books app use them). */
        val BOOK_DIRS = setOf("audiobooks", "audiolibros", "livres audio", "books", "hörbücher", "audiolibri")

        /** The player stores text with a trailing NUL. */
        fun clean(s: Any?): String = when (s) {
            is SqlText -> s.text
            is String -> s
            null -> ""
            else -> s.toString()
        }.trimEnd('\u0000').trim()

        /** "a:\Music\x.flac" -> "Music/x.flac" (a path on the card), or null for another drive. */
        fun cardPath(dbPath: String): String? {
            val p = clean(dbPath)
            if (p.length < 3 || !p.substring(0, 3).equals("a:\\", ignoreCase = true)) return null
            return Normalizer.normalize(p.substring(3).replace('\\', '/'), Normalizer.Form.NFC)
        }

        /** Case- and accent-insensitive key (grouping, search). */
        fun fold(s: String): String = Normalizer.normalize(s.lowercase(), Normalizer.Form.NFD).replace(Regex("\\p{M}+"), "")

        /** "The Beatles" sorts under B, like the player does; empty names go last. */
        private fun sortKey(s: String): String {
            if (s.isEmpty()) return "\uFFFF"
            val l = fold(s)
            return if (l.startsWith("the ") && s.length > 4) l.substring(4) else l
        }

        val collator: Comparator<String> = Comparator { a, b ->
            val c = sortKey(a).compareTo(sortKey(b))
            if (c != 0) c else a.compareTo(b)
        }

        /** True for a card path under one of [BOOK_DIRS], or an .m4b file. */
        fun isBook(path: String): Boolean =
            path.lowercase().endsWith(".m4b") || path.substringBefore('/', "").lowercase() in BOOK_DIRS

        private fun int(v: Any?): Int = when (v) { is Long -> v.coerceIn(Int.MIN_VALUE.toLong(), Int.MAX_VALUE.toLong()).toInt(); is Double -> v.toInt(); is SqlText -> TagReader.leadingInt(v.text); else -> 0 }
        private fun long(v: Any?): Long = when (v) { is Long -> v; is Double -> v.toLong(); else -> 0L }

        /**
         * Reads MEDIA_TABLE, M3U_TABLE and BOOK_TABLE of a database snapshot. Rows of other
         * drives, folders and CUE sub-tracks' parents are left out.
         */
        fun fromSnapshot(db: File, revision: String?, time: Long = System.currentTimeMillis() / 1000,
                         progress: (Int, Int) -> Unit = { _, _ -> }): LibraryIndex {
            SqliteReader(db).use { r ->
                val schema = r.schema()
                fun def(n: String) = schema.firstOrNull { it.type == "table" && it.name.equals(n, true) }?.sql?.let { SchemaSql.parseTable(it) }
                val media = def("MEDIA_TABLE") ?: throw LibraryException("the snapshot has no MEDIA_TABLE")
                fun col(n: String) = media.col(n)
                val c = listOf("id", "path", "name", "album", "artist", "genre", "year", "dis_id", "ck_id", "has_child_file", "size",
                    "sample_rate", "bit_rate", "bit", "format", "quality", "album_pic_path", "ctime", "mtime", "album_artist", "begin_time", "end_time")
                    .associateWith { col(it) }
                fun v(row: SqlRow, n: String): Any? = c[n]!!.let { if (it < 0) null else row.values.getOrNull(it) }
                val bookPaths = def("BOOK_TABLE")?.let { d -> r.table("BOOK_TABLE").mapNotNull { row -> d.col("path").takeIf { it >= 0 }?.let { cardPath(clean(row.values.getOrNull(it))) } } } ?: emptyList()
                val rows = r.table("MEDIA_TABLE")
                val tracks = ArrayList<IndexTrack>(rows.size)
                for ((k, row) in rows.withIndex()) {
                    if (k % 500 == 0) progress(k, rows.size)
                    if (long(v(row, "has_child_file")) != 0L) continue
                    val path = cardPath(clean(v(row, "path"))) ?: continue
                    val ext = path.substringAfterLast('.', "").lowercase()
                    val quality = clean(v(row, "quality"))
                    val sr = int(v(row, "sample_rate"))
                    val bits = int(v(row, "bit"))
                    val lossy = ext in setOf("mp3", "mp2", "aac", "m4a", "m4b", "ogg", "oga", "wma", "opus") && quality == "1"
                    val br = long(v(row, "bit_rate"))
                    val kbps = (if (lossy) br / 1000 else br / 1024).toInt()
                    val begin = long(v(row, "begin_time")); val end = long(v(row, "end_time"))
                    val dur = if (end > begin && begin >= 0) end - begin else 0L
                    val title = clean(v(row, "name")).ifEmpty { path.substringAfterLast('/').substringBeforeLast('.') }
                    val art = clean(v(row, "album_pic_path")).isNotEmpty()
                    val book = isBook(path) || bookPaths.any { path == it || path.startsWith("$it/") }
                    tracks += IndexTrack(long(v(row, "id")), path, title, clean(v(row, "artist")).let { if (it == "Unknown") "" else it },
                        clean(v(row, "album_artist")), clean(v(row, "album")).let { if (it == "Unknown") "" else it },
                        clean(v(row, "genre")).let { if (it == "Unknown") "" else it }, int(v(row, "year")), int(v(row, "ck_id")),
                        int(v(row, "dis_id")), dur, long(v(row, "size")), long(v(row, "mtime")), long(v(row, "ctime")), sr, bits, kbps,
                        !lossy, art, book)
                }
                val playlists = def("M3U_TABLE")?.let { d ->
                    val pc = d.col("path"); val nc = d.col("name")
                    r.table("M3U_TABLE").mapNotNull { row ->
                        val p = cardPath(clean(row.values.getOrNull(pc))) ?: return@mapNotNull null
                        IndexPlaylist(p, clean(row.values.getOrNull(nc)).ifEmpty { p.substringAfterLast('/') }.substringBeforeLast('.'))
                    }
                } ?: emptyList()
                progress(rows.size, rows.size)
                return LibraryIndex("snapshot", revision, time, tracks, playlists)
            }
        }

        /** A track from tags read by [TagReader] (a card scan or a folder on this computer). */
        fun track(id: Long, path: String, size: Long, mtime: Long, info: AudioInfo?, ctime: Long = mtime): IndexTrack {
            val i = info
            val ext = path.substringAfterLast('.', "").lowercase()
            val lossless = i?.lossless ?: (ext in setOf("flac", "wav", "aif", "aiff", "dsf", "dff", "ape"))
            return IndexTrack(id, path, i?.title ?: path.substringAfterLast('/').substringBeforeLast('.'), i?.artist ?: "",
                i?.albumArtist ?: "", i?.album ?: "", i?.genre ?: "", TagReader.leadingInt(i?.date), TagReader.leadingInt(i?.track),
                TagReader.leadingInt(i?.disc), i?.durationMs ?: 0, size, mtime, ctime, i?.sampleRate ?: 0,
                if (lossless) i?.bitsPerSample ?: 0 else 0, ((i?.bitrate ?: 0) / 1000).toInt(), lossless,
                i?.hasPicture == true, isBook(path))
        }

        fun fromTracks(source: String, tracks: List<IndexTrack>, playlists: List<IndexPlaylist>, revision: String? = null,
                       time: Long = System.currentTimeMillis() / 1000) = LibraryIndex(source, revision, time, tracks, playlists)
    }
}
