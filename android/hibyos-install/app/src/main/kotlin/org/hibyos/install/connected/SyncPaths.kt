package org.hibyos.install.connected

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.text.Normalizer

/**
 * The path rules of BTP/1 (section 9.2), checked on the phone before anything is sent, and the
 * way a name from the phone is turned into one the player's card accepts.
 */
object SyncPaths {
    private const val FORBIDDEN = "\\:*?\"<>|"

    /** The reason name of the first rule [bytes] breaks (as in tests/btp/vectors.json), or null. */
    fun check(bytes: ByteArray, allowRoot: Boolean = false): String? {
        if (bytes.isEmpty()) return if (allowRoot) null else "empty"
        if (bytes.size > 1024) return "too_long"
        val text = try {
            Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        } catch (e: CharacterCodingException) {
            return "bad_utf8"
        }
        if (text.startsWith("/")) return "absolute"
        val parts = text.split('/')
        if (parts.size > 32) return "too_deep"
        for (p in parts) {
            if (p.isEmpty()) return "empty_component"
            if (p == "." || p == "..") return "dot_component"
            if (p.startsWith(".")) return "hidden"
            for (ch in p) {
                if (ch.code < 0x20 || ch.code == 0x7f) return "control_char"
                if (FORBIDDEN.indexOf(ch) >= 0) return "forbidden_char"
            }
            if (p.endsWith(" ") || p.endsWith(".")) return "trailing_dot_space"
            if (p.toByteArray(Charsets.UTF_8).size > 255) return "component_too_long"
        }
        return null
    }

    fun check(path: String, allowRoot: Boolean = false): String? = check(path.toByteArray(Charsets.UTF_8), allowRoot)

    fun valid(path: String, allowRoot: Boolean = false): Boolean = check(path, allowRoot) == null

    fun nfc(s: String): String = Normalizer.normalize(s, Normalizer.Form.NFC)

    /**
     * A file or folder name from the phone made acceptable to the card: NFC, characters FAT does
     * not allow become "_", no trailing dots or spaces, at most 255 bytes (the extension is kept).
     * Null for names that cannot be sent: empty, "." or "..", and hidden names (".nomedia").
     */
    fun cleanName(name: String): String? {
        var n = nfc(name).trim()
        if (n.isEmpty() || n == "." || n == ".." || n.startsWith(".")) return null
        val sb = StringBuilder(n.length)
        for (ch in n) sb.append(if (ch.code < 0x20 || ch.code == 0x7f || FORBIDDEN.indexOf(ch) >= 0 || ch == '/') '_' else ch)
        n = sb.toString().trimEnd(' ', '.')
        if (n.isEmpty()) return null
        if (n.toByteArray(Charsets.UTF_8).size > 255) {
            val dot = n.lastIndexOf('.')
            val ext = if (dot > 0 && n.length - dot <= 16) n.substring(dot) else ""
            var stem = n.substring(0, n.length - ext.length)
            // drop whole code points: cutting a surrogate pair (an emoji) would leave an invalid name
            while (stem.isNotEmpty() && (stem + ext).toByteArray(Charsets.UTF_8).size > 255) {
                stem = stem.substring(0, stem.offsetByCodePoints(stem.length, -1))
            }
            n = stem.trimEnd(' ', '.') + ext
        }
        return n
    }

    fun join(folder: String, name: String): String = if (folder.isEmpty()) name else "$folder/$name"

    fun parent(path: String): String = path.substringBeforeLast('/', "")

    fun name(path: String): String = path.substringAfterLast('/')

    /** Breadcrumbs of a path: "" (the card) and then each folder. */
    fun crumbs(path: String): List<String> {
        if (path.isEmpty()) return listOf("")
        val out = arrayListOf("")
        var acc = ""
        for (p in path.split('/')) {
            acc = join(acc, p)
            out += acc
        }
        return out
    }
}
