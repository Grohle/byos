package org.hibyos.install.connected

/** One row of the player's MEDIA_TABLE, as read from a DB snapshot (section 13.1). */
data class Track(
    val path: String,
    val title: String,
    val artist: String,
    val albumArtist: String,
    val album: String,
    val genre: String = "",
    val year: Int = 0,
)

data class Album(val title: String, val artist: String, val year: Int, val tracks: List<Track>)

data class Artist(val name: String, val albums: List<Album>) {
    val trackCount: Int get() = albums.sumOf { it.tracks.size }
}

/** The library grouped the way the player's own Music app shows it. */
class Library(val artists: List<Artist>, val albums: List<Album>, val tracks: List<Track>) {
    /** The library without the songs at these card paths (or inside these card folders). */
    fun without(paths: Set<String>, folders: Set<String> = emptySet()): Library = build(tracks.filter { t ->
        val p = cardPath(t.path)
        p == null || (p !in paths && folders.none { f -> p.startsWith("$f/") })
    })

    companion object {
        /** Top folders of the card that are never deleted as an "album folder". */
        private val protectedFolders = setOf("music", "books", "pictures", "apps", "playlists", "byos", "download", "downloads",
            "recordings", "record", "podcasts", "audiobooks")

        /**
         * The card folder all [tracks] are in, when they share one, or null. A top folder such as
         * Music is never an album's own folder.
         */
        fun folderOf(tracks: List<Track>): String? {
            if (tracks.isEmpty()) return null
            val paths = tracks.map { cardPath(it.path) ?: return null }
            val dirs = paths.map { SyncPaths.parent(it) }.distinct()
            val d = dirs.singleOrNull() ?: return null
            if (d.isEmpty() || (!d.contains('/') && d.lowercase() in protectedFolders)) return null
            return d
        }

        /** The player stores text with a trailing NUL and paths as "a:\Folder\file.flac". */
        fun clean(s: String?): String = (s ?: "").trimEnd('\u0000').trim()

        /** "a:\Music\x.flac" -> "Music/x.flac" (a path on the card), or null for another drive. */
        fun cardPath(dbPath: String): String? {
            val p = clean(dbPath)
            if (p.length < 3 || !p.substring(0, 3).equals("a:\\", ignoreCase = true)) return null
            return p.substring(3).replace('\\', '/')
        }

        private val collator: Comparator<String> = Comparator { a, b ->
            val ka = sortKey(a)
            val kb = sortKey(b)
            val c = ka.compareTo(kb, ignoreCase = true)
            if (c != 0) c else a.compareTo(b)
        }

        /** "The Beatles" sorts under B, like the player does; empty names go last. */
        private fun sortKey(s: String): String {
            if (s.isEmpty()) return "\uFFFF"
            val l = s.lowercase()
            return if (l.startsWith("the ") && s.length > 4) s.substring(4) else s
        }

        fun build(rows: List<Track>): Library {
            val tracks = rows.map {
                it.copy(path = clean(it.path), title = clean(it.title).ifEmpty { clean(it.path).substringAfterLast('\\').substringBeforeLast('.') },
                    artist = clean(it.artist), albumArtist = clean(it.albumArtist), album = clean(it.album), genre = clean(it.genre))
            }
            fun artistOf(t: Track) = t.albumArtist.ifEmpty { t.artist }
            // an album is a title under one album artist; tracks keep their file order (track numbers lead the names)
            val albumMap = LinkedHashMap<Pair<String, String>, MutableList<Track>>()
            for (t in tracks) albumMap.getOrPut(artistOf(t).lowercase() to t.album.lowercase()) { ArrayList() } += t
            val albums = albumMap.values.map { list ->
                val sorted = list.sortedWith(compareBy<Track> { it.path.lowercase() })
                Album(sorted[0].album, artistOf(sorted[0]), sorted.maxOf { it.year }, sorted)
            }.sortedWith { a, b -> collator.compare(a.title, b.title) }
            val byArtist = LinkedHashMap<String, MutableList<Album>>()
            for (a in albums) byArtist.getOrPut(a.artist.lowercase()) { ArrayList() } += a
            val artists = byArtist.values.map { list ->
                Artist(list[0].artist, list.sortedWith(compareBy<Album> { if (it.year > 0) it.year else Int.MAX_VALUE }.thenBy { it.title.lowercase() }))
            }.sortedWith { a, b -> collator.compare(a.name, b.name) }
            return Library(artists, albums, tracks.sortedWith { a, b -> collator.compare(a.title, b.title) })
        }
    }
}
