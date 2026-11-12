package org.hibyos.install.connected

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction

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
        for ((path, title) in songs) append(entry(dir, path, title).joinToString("\n")).append('\n')
    }

    private fun entry(dir: String, path: String, title: String): List<String> =
        (if (title.isNotEmpty()) listOf("#EXTINF:-1," + title.replace('\n', ' ').replace('\r', ' ')) else emptyList()) + relative(dir, path)

    /**
     * The songs of a playlist in [dir] as card paths. Relative paths, paths from the top of the
     * card, the player's own absolute paths (/usr/data/mnt/sd_0/...) and Windows-style ones
     * (a:\..., backslashes) are understood; other lines are skipped.
     */
    fun read(dir: String, text: String): List<String> = text.removePrefix("\uFEFF").lines().mapNotNull { song(dir, it) }

    /** The card path one line of a playlist in [dir] names, or null (a comment, a URL, a path off the card). */
    fun song(dir: String, raw: String): String? {
        var l = raw.trim()
        if (l.isEmpty() || l.startsWith("#")) return null
        l = l.replace('\\', '/')
        val abs = listOf("/usr/data/mnt/sd_0/", "/mnt/sd_0/", "/mnt/sdcard/", "a:/", "A:/").firstOrNull { l.startsWith(it) }
        val parts = ArrayList<String>()
        if (abs != null) {
            l = l.substring(abs.length)
        } else if (l.startsWith("/") || l.contains("://") || (l.length > 1 && l[1] == ':')) {
            return null
        } else if (dir.isNotEmpty()) {
            parts += dir.split('/')
        }
        for (p in l.split('/')) {
            when (p) {
                "", "." -> {}
                ".." -> if (parts.isEmpty()) return null else parts.removeAt(parts.size - 1)
                else -> parts += p
            }
        }
        return if (parts.isNotEmpty()) parts.joinToString("/") else null
    }

    private val legacy: Charset = try {
        Charset.forName("windows-1252")
    } catch (e: Exception) {
        Charsets.ISO_8859_1
    }

    /**
     * A playlist file as it is on the card. UTF-8 when the bytes are valid UTF-8, else the
     * Windows Latin encoding older .m3u files use (CP1252), so "Beyoncé" stays "Beyoncé".
     */
    fun parse(dir: String, bytes: ByteArray): M3uFile {
        val bom = bytes.size >= 3 && bytes[0] == 0xEF.toByte() && bytes[1] == 0xBB.toByte() && bytes[2] == 0xBF.toByte()
        val body = if (bom) bytes.copyOfRange(3, bytes.size) else bytes
        val (text, cs) = try {
            Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(body)).toString() to Charsets.UTF_8
        } catch (e: CharacterCodingException) {
            String(body, legacy) to legacy
        }
        val eol = if (text.contains("\r\n")) "\r\n" else "\n"
        val lines = text.split(eol).let { if (it.lastOrNull()?.isEmpty() == true) it.dropLast(1) else it }
        return M3uFile(dir, lines, cs, bom, eol)
    }

    /** A new, empty playlist (UTF-8, as .m3u8 files are). */
    fun empty(dir: String): M3uFile = M3uFile(dir, listOf("#EXTM3U"), Charsets.UTF_8, false, "\n")

    internal fun addEntries(f: M3uFile, songs: List<Pair<String, String>>): List<String> {
        val kept = f.lines.dropLastWhile { it.isBlank() }
        val head = if (kept.none { it.isNotBlank() }) listOf("#EXTM3U") else emptyList()
        return kept + head + songs.flatMap { (p, t) -> entry(f.dir, p, t) }
    }
}

/**
 * A playlist file read from the card, kept line for line: editing it only adds or removes the
 * lines of a song, so what the app does not read (stream URLs, songs on another drive, paths on
 * a computer, other #EXT tags) stays, in the file's own encoding and line ends.
 */
class M3uFile internal constructor(val dir: String, val lines: List<String>, val charset: Charset, val bom: Boolean, val eol: String) {
    /** The line of each song this app understands, and its card path. */
    private val songLines: List<Pair<Int, String>> = lines.mapIndexedNotNull { i, l -> M3u.song(dir, l)?.let { i to it } }

    val songs: List<String> get() = songLines.map { it.second }

    /** Without the song at [index] (and the #EXTINF just above it). */
    fun removing(index: Int): M3uFile {
        val at = songLines[index].first
        val from = if (at > 0 && lines[at - 1].trimStart().startsWith("#EXTINF", ignoreCase = true)) at - 1 else at
        return M3uFile(dir, lines.subList(0, from) + lines.subList(at + 1, lines.size), charset, bom, eol)
    }

    /** With these songs (card path, title for #EXTINF) at the end. */
    fun adding(songs: List<Pair<String, String>>): M3uFile = M3uFile(dir, M3u.addEntries(this, songs), charset, bom, eol)

    /** The file to write back: its own encoding when it can hold every line, else UTF-8. */
    fun bytes(): ByteArray {
        val text = lines.joinToString(eol) + eol
        val cs = if (charset == Charsets.UTF_8 || charset.newEncoder().canEncode(text)) charset else Charsets.UTF_8
        val body = text.toByteArray(cs)
        return if (bom && cs == Charsets.UTF_8) byteArrayOf(0xEF.toByte(), 0xBB.toByte(), 0xBF.toByte()) + body else body
    }
}

/** A playlist opened in the library: its file and its songs as card paths. */
data class Playlist(val file: String, val songs: List<String>)
