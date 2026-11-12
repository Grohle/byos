package org.hibyos.install.core.sync

import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.text.Normalizer

/** Path rules shared by the scan, the diff and the apply (section 3.3). */
object SyncRules {
    /** Above this, a FAT32 card cannot hold the file. */
    const val FAT32_MAX = 4294967295L
    /** Long-name directory slots a FAT folder can hold before the sync stops adding to it. */
    const val FAT_FOLDER_SLOTS = 60000
    /** DeleteGuard: more than this share of a link's tracked files, or more than [MASS_FILES]. */
    const val MASS_SHARE = 0.10
    const val MASS_FILES = 200
    /** Bytes read at each end of a file for the quick fingerprint. */
    const val QFP_PART = 16 * 1024

    fun nfc(s: String): String = Normalizer.normalize(s, Normalizer.Form.NFC)

    fun lower(p: String) = p.lowercase()

    fun under(path: String, root: String): Boolean = root.isEmpty() || lower(path).startsWith(lower(root) + "/")

    fun parent(p: String) = p.substringBeforeLast('/', "")

    fun name(p: String) = p.substringAfterLast('/')

    /** A path component that is hidden (".name"): never synced, never deleted. */
    fun hidden(path: String) = path.split('/').any { it.startsWith(".") }

    /**
     * Card paths a sync never lists or touches, whatever the link says: the HiBy's own
     * downloads (Music/Free), ByOS's folder, the playlists folder (playlist sync owns it),
     * hidden files and system folders.
     */
    fun alwaysExcludedOnCard(path: String): Boolean {
        val l = lower(path)
        if (hidden(path)) return true
        if (l == "byos" || l.startsWith("byos/") || l == "playlists" || l.startsWith("playlists/")) return true
        if (l == "music/free" || l.startsWith("music/free/")) return true
        val top = l.substringBefore('/')
        return top in setOf("system volume information", "\$recycle.bin", "lost.dir")
    }

    private val globCache = HashMap<String, Regex>()

    /**
     * A glob as in links.json "exclude": `**` matches across folders, `*` within one name, `?`
     * one character. A pattern without '/' matches a name at any depth. Case-insensitive.
     */
    fun glob(pattern: String): Regex = synchronized(globCache) {
        globCache.getOrPut(pattern) {
            val p = pattern.replace('\\', '/').trim('/')
            val sb = StringBuilder()
            if (!p.contains('/')) sb.append("(?:.*/)?")
            var i = 0
            while (i < p.length) {
                val c = p[i]
                when {
                    c == '*' && i + 1 < p.length && p[i + 1] == '*' -> {
                        i++
                        if (i + 1 < p.length && p[i + 1] == '/') { i++; sb.append("(?:.*/)?") } else sb.append(".*")
                    }
                    c == '*' -> sb.append("[^/]*")
                    c == '?' -> sb.append("[^/]")
                    else -> sb.append(Regex.escape(c.toString()))
                }
                i++
            }
            // a folder pattern also covers what is inside it
            sb.append("(?:/.*)?")
            Regex(sb.toString(), RegexOption.IGNORE_CASE)
        }
    }

    fun excluded(patterns: List<String>, rel: String): Boolean = patterns.any { glob(it).matches(rel) }

    /** Long-name slots one FAT directory entry needs: 1 + ceil(UTF-16 units / 13). */
    fun fatSlots(name: String): Int = 1 + (name.length + 12) / 13

    /** Names Windows refuses (pulls to this computer get a "_" added). */
    fun windowsSafe(name: String): String {
        val base = name.substringBefore('.').uppercase()
        val reserved = base in setOf("CON", "PRN", "AUX", "NUL") || Regex("(COM|LPT)[1-9]").matches(base)
        return if (reserved) name.substringBefore('.') + "_" + name.substring(name.substringBefore('.').length) else name
    }

    fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }

    /** Quick fingerprint: SHA-256 of (size ‖ first 16 KiB ‖ last 16 KiB). */
    fun qfp(size: Long, head: ByteArray, tail: ByteArray): String {
        val md = MessageDigest.getInstance("SHA-256")
        val s = ByteArray(8) { (size ushr (56 - 8 * it)).toByte() }
        md.update(s); md.update(head); md.update(tail)
        return hex(md.digest())
    }

    /** The ranges [qfp] reads: (offset, length) of the head and of the tail. */
    fun qfpRanges(size: Long): Pair<Pair<Long, Int>, Pair<Long, Int>> {
        val n = minOf(size, QFP_PART.toLong()).toInt()
        return (0L to n) to (maxOf(0L, size - QFP_PART) to n)
    }

    fun qfpFile(f: File): String = RandomAccessFile(f, "r").use { r ->
        val size = r.length()
        val (h, t) = qfpRanges(size)
        val head = ByteArray(h.second); r.seek(h.first); r.readFully(head)
        val tail = ByteArray(t.second); r.seek(t.first); r.readFully(tail)
        qfp(size, head, tail)
    }

    fun shaFile(f: File, cancelled: () -> Boolean = { false }, progress: (Long, Long) -> Unit = { _, _ -> }): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        val total = f.length()
        var done = 0L
        f.inputStream().use { s ->
            val b = ByteArray(1 shl 16)
            while (true) {
                if (cancelled()) throw SyncException("cancelled", "cancelled")
                val n = s.read(b)
                if (n < 0) break
                md.update(b, 0, n)
                done += n
                progress(done, total)
            }
        }
        return md.digest()
    }

    /** "Name (2).flac" → "Name.flac": the name before collision numbering. */
    fun unnumbered(p: String): String {
        val dir = parent(p); val n = name(p)
        val ext = if (n.contains('.')) "." + n.substringAfterLast('.') else ""
        val base = n.removeSuffix(ext).replace(Regex(" \\(\\d+\\)$"), "")
        return (if (dir.isEmpty()) "" else "$dir/") + base + ext
    }

    /** [p] with " (n)" before its extension. */
    fun numbered(p: String, n: Int): String {
        val dir = parent(p); val nm = name(p)
        val ext = if (nm.contains('.')) "." + nm.substringAfterLast('.') else ""
        return (if (dir.isEmpty()) "" else "$dir/") + nm.removeSuffix(ext) + " ($n)" + ext
    }
}
