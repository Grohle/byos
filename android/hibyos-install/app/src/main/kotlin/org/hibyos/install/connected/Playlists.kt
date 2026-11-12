package org.hibyos.install.connected

/**
 * Playlists as .m3u8 files in the Playlists folder of the card: UTF-8, #EXTM3U, one song per
 * line as a path relative to the playlist (as most players and computers read them). The
 * stock player's own playlists (MANAGER_TABLE in its database) are not touched.
 */
object M3u {
    const val FOLDER = "Playlists"

    fun isPlaylist(name: String): Boolean = FileKinds.ext(name) in setOf("m3u", "m3u8")

    /** The playlist's name without its extension. */
    fun title(file: String): String = file.substringAfterLast('/').substringBeforeLast('.')

    /** [song] (a card path) as seen from a playlist in [dir]: "../Music/ABBA/01.flac". */
    fun relative(dir: String, song: String): String {
        val from = if (dir.isEmpty()) emptyList() else dir.split('/')
        val to = song.split('/')
        var common = 0
        while (common < from.size && common < to.size - 1 && from[common].equals(to[common], ignoreCase = true)) common++
        return (List(from.size - common) { ".." } + to.drop(common)).joinToString("/")
    }

    /** The text of a playlist in [dir] with these songs (card paths) and their titles for #EXTINF. */
    fun write(dir: String, songs: List<Pair<String, String>>): String = buildString {
        append("#EXTM3U\n")
        for ((path, title) in songs) {
            if (title.isNotEmpty()) append("#EXTINF:-1,").append(title.replace('\n', ' ')).append('\n')
            append(relative(dir, path)).append('\n')
        }
    }

    /**
     * The songs of a playlist in [dir] as card paths. Relative paths, paths from the top of the
     * card, the player's own absolute paths (/usr/data/mnt/sd_0/...) and Windows-style ones
     * (a:\..., backslashes) are understood; other lines are skipped.
     */
    fun read(dir: String, text: String): List<String> {
        val out = ArrayList<String>()
        for (raw in text.removePrefix("\uFEFF").lines()) {
            var l = raw.trim()
            if (l.isEmpty() || l.startsWith("#")) continue
            l = l.replace('\\', '/')
            val abs = listOf("/usr/data/mnt/sd_0/", "/mnt/sd_0/", "/mnt/sdcard/", "a:/", "A:/").firstOrNull { l.startsWith(it) }
            val parts = ArrayList<String>()
            if (abs != null) {
                l = l.substring(abs.length)
            } else if (l.startsWith("/") || l.contains("://") || (l.length > 1 && l[1] == ':')) {
                continue
            } else if (dir.isNotEmpty()) {
                parts += dir.split('/')
            }
            var bad = false
            for (p in l.split('/')) {
                when (p) {
                    "", "." -> {}
                    ".." -> if (parts.isEmpty()) bad = true else parts.removeAt(parts.size - 1)
                    else -> parts += p
                }
            }
            if (!bad && parts.isNotEmpty()) out += parts.joinToString("/")
        }
        return out
    }
}

/** A playlist opened in the library: its file and its songs as card paths. */
data class Playlist(val file: String, val songs: List<String>)
