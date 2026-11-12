package org.hibyos.install.connected

/** What a file is, from its name: where it belongs on the card and what the player does with it. */
enum class FileKind { AUDIO, IMAGE, LYRICS, BOOK, PLAYLIST, FIRMWARE, BYOS_UPDATE, APP, OTHER }

object FileKinds {
    /** The formats the R3 II plays (the stock player's own list, without containers it skips). */
    private val audio = setOf("mp3", "flac", "m4a", "aac", "alac", "wav", "aif", "aiff", "aifc", "ape", "ogg", "oga", "opus",
        "wma", "dsf", "dff", "wv", "mpc", "tak", "m4b", "mka")
    private val image = setOf("jpg", "jpeg", "png", "bmp", "gif", "webp")
    private val book = setOf("epub", "txt", "pdf", "fb2", "mobi")
    private val playlist = setOf("m3u", "m3u8", "pls", "cue")

    fun ext(name: String): String = name.substringAfterLast('.', "").lowercase()

    fun of(name: String): FileKind {
        val n = name.substringAfterLast('/').lowercase()
        val e = ext(n)
        return when {
            n == "r3ii_2025.upt" || e == "upt" -> FileKind.FIRMWARE
            n == "hibyos-update.json" || (n.startsWith("hibyos-payload-") && e == "tar") -> FileKind.BYOS_UPDATE
            e in audio -> FileKind.AUDIO
            e in image -> FileKind.IMAGE
            e == "lrc" -> FileKind.LYRICS
            e in book -> FileKind.BOOK
            e in playlist -> FileKind.PLAYLIST
            e == "hpk" -> FileKind.APP
            else -> FileKind.OTHER
        }
    }

    fun isAudio(name: String): Boolean = of(name) == FileKind.AUDIO

    /** The names the stock player and most taggers use for an album's artwork, best first. */
    val coverNames = listOf("cover.jpg", "folder.jpg", "front.jpg", "cover.png", "folder.png", "front.png", "albumart.jpg", "album.jpg")
}
