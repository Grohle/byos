package org.hibyos.install.core.library

import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.text.Normalizer

/**
 * Builds the stock player's library database (`usrlocal_media.db`, R3 II 2025 schema,
 * VERSION_TABLE 20181015) on a computer, from a snapshot of the live database and the
 * music folder that mirrors the SD card.
 *
 * The rules come from docs/research_companion_apps.md (critic Decisions 6 and 7) and from
 * a real device database. The idea of rebuilding the database on a PC, the `a:\` path
 * form, the format codes and the list of rebuilt tables were learned from the hiby-mods
 * database updater (MIT, Copyright (c) 2026 noisetta and contributors; see
 * THIRD_PARTY_NOTICES.md). This is a new implementation, with these differences:
 *
 * - The snapshot's schema is kept exactly; tables that hold the user's own data (history,
 *   favourites, playlists, the queue with MANAGER_TABLE, books, version and any table this
 *   code does not know) are copied unchanged.
 * - Track ids and "date added" (ctime) are kept for files that were already in the snapshot,
 *   so history, favourites and the queue still point at the same tracks. Ids stay 1..N, except
 *   that an id of a removed file is never reused (new files then get ids above N).
 * - MEDIA2_TABLE is rebuilt only as the cache of the SD root folder, SEARCH_TABLE and
 *   MEDIA3_TABLE are left empty, as the stock player leaves them.
 * - MQA (quality "18" and the MQA format row), ctime/mtime in the catalogue tables and
 *   unique M3U ids are filled as the device does; sorting follows the stock order seen on
 *   the device (letters, then numbers in numeric order, then everything else).
 */
class LibraryOptions(
    /** The folder whose contents are on the SD card (the card itself, or a mirror). */
    val musicRoot: File,
    /** The folder that maps to `a:\` (the SD root); [musicRoot] or one of its parents. */
    val sdRoot: File = musicRoot,
    /** Record `cover.jpg`/`folder.jpg`... found next to the tracks. */
    val folderCovers: Boolean = false,
    /** Record `<track>.lrc` next to the tracks. */
    val sidecarLyrics: Boolean = false,
    /** Unix seconds used as "date added" for new tracks and for the root-folder cache. */
    val now: Long = System.currentTimeMillis() / 1000,
)

class LibraryReport(
    val tracks: Int, val albums: Int, val artists: Int, val genres: Int, val albumArtists: Int, val playlistsAdded: Int,
    val skipped: Int, val noTags: Int, val renumbered: Int, val keptTables: List<String>, val rebuiltTables: List<String>,
    val sha256: String, val size: Long,
)

interface LibraryListener {
    fun progress(op: String, done: Int, total: Int, path: String?) {}
    fun warn(code: String, message: String, path: String?) {}
}

class LibraryException(message: String) : Exception(message)

object LibraryBuilder {
    const val SCHEMA_VERSION = 20181015L

    /** Extension -> stock format code (MEDIA_TABLE.format). */
    val FORMAT_CODES = mapOf(
        "flac" to 61868L, "dsf" to 54736L, "dff" to 54736L, "mp3" to 85L, "mp2" to 80L, "wav" to 1L, "aif" to 1L,
        "aiff" to 1L, "aac" to 255L, "m4a" to 255L, "m4b" to 255L, "wma" to 353L, "ogg" to 26447L, "oga" to 26447L,
        "opus" to 28503L, "ape" to 21574L, "iso" to 0L,
    )
    val AUDIO_EXT = FORMAT_CODES.keys
    val PLAYLIST_EXT = setOf("m3u", "m3u8")
    private val LOSSY = setOf("mp3", "mp2", "aac", "m4a", "m4b", "ogg", "oga", "wma", "opus")
    private val DSD = setOf("dsf", "dff", "iso")
    private val SYSTEM_DIRS = setOf("system volume information", "\$recycle.bin", "lost.dir")
    private val COVER_NAMES = listOf("cover.jpg", "folder.jpg", "front.jpg", "albumart.jpg", "cover.jpeg", "folder.jpeg",
        "front.jpeg", "cover.png", "folder.png", "front.png")

    val REBUILT = listOf("MEDIA_TABLE", "MEDIA2_TABLE", "MEDIA3_TABLE", "SEARCH_TABLE", "ARTIST_TABLE", "ARTIST2_TABLE",
        "ALBUM_TABLE", "ALBUM2_TABLE", "GENRE_TABLE", "GENRE2_TABLE", "ALBUM_ARTIST_TABLE", "ALBUM_ARTIST2_TABLE",
        "COUNT_TABLE", "CTIME_TABLE", "MTIME_TABLE", "FORMAT_TABLE", "FORMAT2_TABLE", "M3U_TABLE")

    // ------------------------------------------------------------------ text rules

    fun nfc(s: String): String = Normalizer.normalize(s, Normalizer.Form.NFC)
    private fun z(s: String) = SqlText(s + "\u0000")
    fun asciiUpper(s: String) = buildString(s.length) { for (c in s) append(if (c in 'a'..'z') c - 32 else c) }
    private fun asciiLower(s: String) = buildString(s.length) { for (c in s) append(if (c in 'A'..'Z') c + 32 else c) }
    private fun key(s: String) = asciiLower(s) // NOCASE equality

    /** Upper case of one code point as the player does it (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic). */
    fun upper(c: Int): Int = when {
        c in 'a'.code..'z'.code -> c - 32
        c in 0xE0..0xFE && c != 0xF7 -> c - 32
        c == 0xFF -> 0x178
        c in 0x3B1..0x3C9 && c != 0x3C2 -> c - 32
        c in 0x430..0x44F -> c - 32
        c in 0x450..0x45F -> c - 80
        c in 0x100..0x17F && (c and 1) == 1 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F ->
            if (c in 0x139..0x148 || c in 0x179..0x17E) c else c - 1
        c in 0x139..0x148 && (c and 1) == 0 -> c - 1
        c in 0x179..0x17E && (c and 1) == 0 -> c - 1
        else -> c
    }

    /** The index letter (column `character`, no NUL): the first character in upper case. */
    fun character(name: String): String = if (name.isEmpty()) "" else String(Character.toChars(upper(name.codePointAt(0))))

    private fun utf8(s: String) = s.toByteArray(Charsets.UTF_8)

    private fun cmpBytes(a: ByteArray, b: ByteArray): Int {
        for (i in 0 until minOf(a.size, b.size)) {
            val d = (a[i].toInt() and 0xff) - (b[i].toInt() and 0xff)
            if (d != 0) return d
        }
        return a.size - b.size
    }

    private fun tier(s: String): Int = when {
        s.isEmpty() -> 2
        s[0] in 'A'..'Z' || s[0] in 'a'..'z' -> 0
        s[0] in '0'..'9' -> 1
        else -> 2
    }

    /** Stock list order: ASCII letters (case-insensitive), then numbers by value, then the rest by bytes. */
    val stockOrder: Comparator<String> = Comparator { a, b ->
        val ta = tier(a); val tb = tier(b)
        if (ta != tb) return@Comparator ta - tb
        if (ta == 1) {
            val na = a.takeWhile { it.isDigit() }.trimStart('0').ifEmpty { "0" }
            val nb = b.takeWhile { it.isDigit() }.trimStart('0').ifEmpty { "0" }
            val d = if (na.length != nb.length) na.length - nb.length else na.compareTo(nb)
            if (d != 0) return@Comparator d
        }
        cmpBytes(utf8(asciiUpper(a)), utf8(asciiUpper(b)))
    }

    /** `a:\Folder\File.flac` for [file] under [sdRoot]. */
    fun hibyPath(file: File, sdRoot: File): String {
        val rel = sdRoot.absoluteFile.toPath().normalize().relativize(file.absoluteFile.toPath().normalize()).toString()
        return "a:\\" + nfc(rel.replace('/', '\\'))
    }

    fun quality(ext: String, info: AudioInfo?): String {
        val sr = info?.sampleRate ?: 0
        return when {
            sr == 0 -> "0"
            info?.mqa == true -> "18"
            ext in DSD || sr > 48000 -> "3"
            ext in LOSSY && info?.lossless != true -> "1"
            else -> "2"
        }
    }

    // ------------------------------------------------------------------ build

    private class Track(val file: File, val ext: String, val path: String, val info: AudioInfo?) {
        var id = 0L
        var ctime = 0L
        var mtime = 0L
        var size = 0L
        lateinit var name: String
        lateinit var album: String
        lateinit var artist: String
        lateinit var genre: String
        lateinit var albumArtist: String
        val mqa get() = info?.mqa == true
        val formatName get() = if (mqa) "MQA" else ext.uppercase()
    }

    private fun walk(dir: File, out: MutableList<File>, playlists: MutableList<File>) {
        val names = dir.list() ?: return
        for (n in names.sortedWith(String.CASE_INSENSITIVE_ORDER)) {
            if (n.startsWith(".") || n.lowercase() in SYSTEM_DIRS) continue
            val f = File(dir, n)
            if (f.isDirectory) walk(f, out, playlists)
            else {
                val ext = n.substringAfterLast('.', "").lowercase()
                if (ext in AUDIO_EXT) out += f
                else if (ext in PLAYLIST_EXT) playlists += f
            }
        }
    }

    private fun rowFor(def: TableDef, values: Map<String, Any?>): List<Any?> =
        def.columns.map { c -> values[c.name] ?: values[c.name.lowercase()] }

    fun build(base: File, out: File, opts: LibraryOptions, listener: LibraryListener = object : LibraryListener {}): LibraryReport {
        if (!opts.musicRoot.isDirectory) throw LibraryException("the music folder does not exist: ${opts.musicRoot}")
        val sdRootPath = opts.sdRoot.absoluteFile.toPath().normalize()
        if (!opts.musicRoot.absoluteFile.toPath().normalize().startsWith(sdRootPath)) {
            throw LibraryException("the music folder must be inside the SD root folder")
        }
        SqliteReader(base).use { r ->
            val schema = r.schema()
            val tables = schema.filter { it.type == "table" && it.rootPage > 0 }
            val defs = HashMap<String, TableDef>()
            for (t in tables) defs[t.name.uppercase()] = SchemaSql.parseTable(t.sql ?: throw LibraryException("table ${t.name} has no SQL"))
            for (need in listOf("MEDIA_TABLE", "VERSION_TABLE")) {
                if (need !in defs) throw LibraryException("the base is not a HiBy library database (no $need)")
            }
            val version = r.table("VERSION_TABLE").firstOrNull()?.values?.firstOrNull()
            if (version != SCHEMA_VERSION) {
                listener.warn("schema_version", "unexpected library version $version (expected $SCHEMA_VERSION); the result may not work", null)
            }
            val mediaDef = defs["MEDIA_TABLE"]!!
            val pathCol = mediaDef.col("path")
            val idCol = mediaDef.col("id")
            val ctimeCol = mediaDef.col("ctime")
            // what the snapshot knows about each path: id and "date added"
            val old = HashMap<String, Pair<Long, Long>>()
            for (row in r.table("MEDIA_TABLE")) {
                val p = (row.values.getOrNull(pathCol) as? SqlText)?.text?.trimEnd('\u0000') ?: continue
                val id = row.values.getOrNull(idCol) as? Long ?: continue
                val ct = row.values.getOrNull(ctimeCol) as? Long ?: 0L
                old.putIfAbsent(key(p), id to ct)
            }

            // ---- scan
            val files = ArrayList<File>()
            val playlists = ArrayList<File>()
            walk(opts.musicRoot, files, playlists)
            val tracks = ArrayList<Track>()
            var skipped = 0
            var noTags = 0
            for ((i, f) in files.withIndex()) {
                listener.progress("scan", i, files.size, f.path)
                val ext = f.name.substringAfterLast('.', "").lowercase()
                if (!f.canRead()) {
                    skipped++
                    listener.warn("unreadable", "cannot read ${f.path}", f.path)
                    continue
                }
                val info = try {
                    TagReader.read(f)
                } catch (e: TagException) {
                    noTags++
                    listener.warn("no_tags", "no tags read from ${f.name} (${e.message}); listed by its file name", f.path)
                    null
                } catch (e: IOException) {
                    skipped++
                    listener.warn("unreadable", "cannot read ${f.path}: ${e.message}", f.path)
                    continue
                } catch (e: RuntimeException) {
                    noTags++
                    listener.warn("no_tags", "tags of ${f.name} could not be parsed; listed by its file name", f.path)
                    null
                }
                val t = Track(f, ext, hibyPath(f, opts.sdRoot), info)
                t.size = f.length()
                t.mtime = f.lastModified() / 1000
                val stem = nfc(f.name.substringBeforeLast('.'))
                t.name = info?.title?.let(::nfc) ?: stem
                t.artist = info?.artist?.let(::nfc) ?: "Unknown"
                t.album = info?.album?.let(::nfc) ?: "Unknown"
                t.genre = info?.genre?.let(::nfc) ?: "Unknown"
                t.albumArtist = info?.albumArtist?.let(::nfc) ?: ""
                tracks += t
            }
            listener.progress("scan", files.size, files.size, null)
            // the same path twice (case-only differences) would break the database's unique key
            val seen = HashSet<String>()
            tracks.removeAll { t -> !seen.add(key(t.path)).also { if (!it) listener.warn("duplicate_path", "ignored ${t.path}: same name as another file except for case", t.file.path) } }

            // ---- ids: keep the snapshot's id when it is in 1..N and not taken; fill the rest in path order.
            // An id that belonged to a file no longer on the card is never given to another file:
            // kept history, favourite and playlist rows of the removed file still carry it. When
            // 1..N has no other free id, new files get ids above N.
            val n = tracks.size
            val byPath = tracks.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.path })
            val used = HashSet<Long>()
            var renumbered = 0
            for (t in byPath) {
                val o = old[key(t.path)]
                t.ctime = o?.second?.takeIf { it > 0 } ?: opts.now
                if (o != null && o.first in 1..n.toLong() && used.add(o.first)) t.id = o.first
            }
            val present = tracks.mapTo(HashSet()) { key(it.path) }
            val reserved = old.filterKeys { it !in present }.values.mapTo(HashSet()) { it.first }
            var nextFree = 1L
            for (t in byPath) {
                if (t.id != 0L) continue
                while (nextFree in used || nextFree in reserved) nextFree++
                t.id = nextFree
                used += nextFree
                if (old.containsKey(key(t.path))) renumbered++
            }
            val newIdByPath = tracks.associate { key(it.path) to it.id }
            val byId = tracks.sortedBy { it.id }

            // ---- rows of the rebuilt tables
            val rows = HashMap<String, List<SqlRow>>()
            fun put(table: String, list: List<Map<String, Any?>>) {
                val def = defs[table] ?: return
                rows[table] = list.mapIndexed { i, m -> SqlRow((i + 1).toLong(), rowFor(def, m)) }
            }
            val cover = HashMap<File, String>()
            fun coverFor(t: Track): String {
                if (!opts.folderCovers) return ""
                val d = t.file.parentFile
                return cover.getOrPut(d) {
                    val names = (d.list() ?: emptyArray()).associateBy { it.lowercase() }
                    COVER_NAMES.firstNotNullOfOrNull { names[it] }?.let { hibyPath(File(d, it), opts.sdRoot) } ?: ""
                }
            }
            fun lrcFor(t: Track): String {
                if (!opts.sidecarLyrics) return ""
                val stem = t.file.name.substringBeforeLast('.')
                val m = (t.file.parentFile.list() ?: emptyArray()).firstOrNull { it.equals("$stem.lrc", ignoreCase = true) }
                return m?.let { hibyPath(File(t.file.parentFile, it), opts.sdRoot) } ?: ""
            }
            fun media(t: Track): Map<String, Any?> {
                val i = t.info
                val lossy = t.ext in LOSSY && i?.lossless != true
                val br = i?.bitrate ?: 0L
                return mapOf(
                    "id" to t.id, "path" to z(t.path), "name" to z(t.name), "album" to z(t.album), "artist" to z(t.artist),
                    "genre" to z(t.genre), "year" to TagReader.leadingInt(i?.date).toLong(),
                    "dis_id" to TagReader.leadingInt(i?.disc).toLong(), "ck_id" to TagReader.leadingInt(i?.track).toLong(),
                    "has_child_file" to 0L, "begin_time" to 0L, "end_time" to -1L, "cue_id" to -1L,
                    "character" to SqlText(character(t.name)), "size" to t.size,
                    "sample_rate" to (i?.sampleRate ?: 0).toLong(),
                    // lossless rates are stored in units of 1/1024 kbit, lossy ones in bit/s (as on the device)
                    "bit_rate" to if (lossy) br else (br / 1000) * 1024,
                    "bit" to (if (lossy) 16 else i?.bitsPerSample ?: 0).toLong(), "channel" to (i?.channels ?: 0).toLong(),
                    "format" to (FORMAT_CODES[t.ext] ?: 0L), "quality" to SqlText(quality(t.ext, i)),
                    "album_pic_path" to z(coverFor(t)), "lrc_path" to z(lrcFor(t)), "track_gain" to null, "track_peak" to null,
                    "ctime" to t.ctime, "mtime" to t.mtime, "pinyin_charater" to z(asciiUpper(t.name)),
                    "album_artist" to z(t.albumArtist),
                )
            }
            listener.progress("write", 0, 1, null)
            val byName = tracks.sortedWith(Comparator<Track> { a, b -> stockOrder.compare(a.name, b.name) }.thenBy { it.id })
            put("MEDIA_TABLE", byName.map(::media))
            put("MEDIA3_TABLE", emptyList())
            put("SEARCH_TABLE", emptyList())

            // root folder cache
            val rootRows = ArrayList<Map<String, Any?>>()
            for (nm in opts.sdRoot.list() ?: emptyArray()) {
                if (nm.startsWith(".") || nm.lowercase() in SYSTEM_DIRS) continue
                val f = File(opts.sdRoot, nm)
                val dir = f.isDirectory
                if (!dir && nm.substringAfterLast('.', "").lowercase() !in AUDIO_EXT) continue
                val shown = nfc(nm)
                rootRows += mapOf(
                    "id" to rootRows.size.toLong(), "path" to z("a:\\" + shown + if (dir) "\\*" else ""), "name" to z(shown),
                    "album" to z("Unknown"), "artist" to z("Unknown"), "genre" to z("Unknown"), "year" to 0L, "dis_id" to 0L,
                    "ck_id" to 0L, "has_child_file" to if (dir) 3L else 0L, "begin_time" to 0L,
                    "end_time" to if (dir) -2L else -1L, "cue_id" to if (dir) -2L else -1L, "character" to SqlText(character(shown)),
                    "size" to 0L, "sample_rate" to 0L, "bit_rate" to 0L, "bit" to 0L, "channel" to 0L, "format" to 0L,
                    "quality" to SqlText("0"), "album_pic_path" to z(""), "lrc_path" to z(""), "track_gain" to null,
                    "track_peak" to null, "ctime" to opts.now, "mtime" to opts.now, "pinyin_charater" to z(asciiUpper(shown)),
                    "album_artist" to z(""),
                )
            }
            put("MEDIA2_TABLE", rootRows)

            // catalogue tables: one row per value (case-insensitive), fields from its first track
            class Agg(val value: String, val first: Track) { var tracks = 0; val albums = HashSet<String>() }
            fun aggregate(sel: (Track) -> String): List<Agg> {
                val m = LinkedHashMap<String, Agg>()
                for (t in byId) {
                    val v = sel(t)
                    if (v.isEmpty()) continue
                    val a = m.getOrPut(key(v)) { Agg(v, t) }
                    a.tracks++
                    a.albums += key(t.album)
                }
                return m.values.toList()
            }
            fun catalogue(table: String, col: String, aggs: List<Agg>, cnTracks: Boolean, mqa: Boolean) {
                val list = aggs.map { a ->
                    val m = linkedMapOf<String, Any?>(
                        "id" to a.first.id, col to z(a.value), "character" to SqlText(character(a.value)),
                        "cn" to (if (cnTracks) a.tracks else a.albums.size).toLong(), "ctime" to a.first.ctime, "mtime" to a.first.mtime,
                        "pinyin_charater" to z(asciiUpper(a.value)))
                    if (mqa) m["mqa"] = if (a.first.mqa) 1L else 0L
                    a to m
                }
                put(table, list.sortedWith(Comparator<Pair<Agg, Map<String, Any?>>> { x, y -> stockOrder.compare(x.first.value, y.first.value) }.thenBy { it.first.first.id }).map { it.second })
                put(table.replace("_TABLE", "2_TABLE"), list.sortedBy { it.first.first.id }.map { it.second })
            }
            val artists = aggregate { it.artist }
            val albums = aggregate { it.album }
            val genres = aggregate { it.genre }
            val albumArtists = aggregate { it.albumArtist }
            catalogue("ARTIST_TABLE", "artist", artists, cnTracks = false, mqa = false)
            catalogue("ALBUM_TABLE", "album", albums, cnTracks = true, mqa = true)
            catalogue("GENRE_TABLE", "genre", genres, cnTracks = true, mqa = false)
            catalogue("ALBUM_ARTIST_TABLE", "album_artist", albumArtists, cnTracks = false, mqa = true)
            put("COUNT_TABLE", listOf(n, albums.size, artists.size, genres.size, albumArtists.size).map { mapOf("cn" to it.toLong()) })
            put("CTIME_TABLE", tracks.sortedWith(compareBy<Track> { it.ctime }.thenBy { it.id }).map { mapOf("media_id" to it.id) })
            put("MTIME_TABLE", tracks.sortedWith(compareBy<Track> { it.mtime }.thenBy { it.id }).map { mapOf("media_id" to it.id) })
            val formats = LinkedHashMap<String, Track>()
            for (t in byId) formats.putIfAbsent(t.formatName, t)
            val fmtRows = formats.map { (nm, t) -> t.id to mapOf("id" to t.id, "format" to SqlText(nm), "character" to SqlText(character(nm)), "cn" to 1L) }
            put("FORMAT_TABLE", fmtRows.sortedWith(Comparator { a, b -> stockOrder.compare((a.second["format"] as SqlText).text, (b.second["format"] as SqlText).text) }).map { it.second })
            put("FORMAT2_TABLE", fmtRows.sortedBy { it.first }.map { it.second })

            // M3U_TABLE: the snapshot's rows (mixes made on the device) plus new playlist files
            var playlistsAdded = 0
            defs["M3U_TABLE"]?.let { def ->
                val kept = r.table("M3U_TABLE")
                val pc = def.col("path"); val ic = def.col("id")
                val known = kept.mapNotNull { (it.values.getOrNull(pc) as? SqlText)?.text?.trimEnd('\u0000')?.let(::key) }.toHashSet()
                var nextId = (kept.mapNotNull { it.values.getOrNull(ic) as? Long }.maxOrNull() ?: 0L) + 1
                var nextRow = (kept.maxOfOrNull { it.rowid } ?: 0L) + 1
                val add = ArrayList<SqlRow>()
                for (p in playlists) {
                    val hp = hibyPath(p, opts.sdRoot)
                    if (!known.add(key(hp))) continue
                    val nm = nfc(p.name)
                    add += SqlRow(nextRow++, rowFor(def, mapOf("id" to nextId++, "path" to z(hp), "name" to z(nm), "character" to SqlText(character(nm)))))
                    playlistsAdded++
                }
                rows["M3U_TABLE"] = kept + add
            }

            // ---- kept tables, with track ids updated where a track got a new id
            val kept = ArrayList<String>()
            for (t in tables) {
                val nm = t.name.uppercase()
                if (nm in rows) continue
                kept += t.name
                val def = defs[nm]!!
                val pc = def.col("path"); val ic = def.col("id")
                val src = r.rows(t.rootPage)
                if (renumbered == 0 || pc < 0 || ic < 0 || def.col("name") < 0) { rows[nm] = src; continue }
                val cc = def.col("cue_id")
                fun pathOf(row: SqlRow) = (row.values.getOrNull(pc) as? SqlText)?.text?.trimEnd('\u0000')
                fun newId(row: SqlRow): Long? = pathOf(row)?.let { newIdByPath[key(it)] }
                fun pairKey(id: Any?, p: String?, cue: Any?) = "$id|${p?.let(::key)}|$cue"
                // (id, path, cue_id) of every row that keeps its id: a re-pointed row must not collide
                // with one of them, or the table's PRIMARY KEY would be violated
                val pairs = HashSet<String>()
                for (row in src) {
                    val nid = newId(row)
                    val oid = row.values.getOrNull(ic)
                    if (nid == null || nid == oid) pairs.add(pairKey(oid, pathOf(row), row.values.getOrNull(cc)))
                }
                rows[nm] = src.mapNotNull { row ->
                    val nid = newId(row)
                    val oid = row.values.getOrNull(ic)
                    if (nid == null || nid == oid) return@mapNotNull row
                    // the same entry already exists with the new id: drop the duplicate
                    if (!pairs.add(pairKey(nid, pathOf(row), row.values.getOrNull(cc)))) return@mapNotNull null
                    SqlRow(row.rowid, row.values.toMutableList().also { it[ic] = nid })
                }
            }

            val tmp = File(out.path + ".tmp")
            SqliteWriter(tmp, r.pageSize, r.userVersion, r.applicationId).write(schema) { name ->
                rows[name.uppercase()] ?: r.rows(schema.first { it.name == name }.rootPage)
            }
            out.delete()
            if (!tmp.renameTo(out)) throw IOException("cannot write ${out.path}")
            listener.progress("write", 1, 1, null)
            val md = MessageDigest.getInstance("SHA-256")
            out.inputStream().use { s -> val b = ByteArray(1 shl 16); while (true) { val k = s.read(b); if (k < 0) break; md.update(b, 0, k) } }
            return LibraryReport(n, albums.size, artists.size, genres.size, albumArtists.size, playlistsAdded, skipped, noTags,
                renumbered, kept, REBUILT.filter { it in defs }, md.digest().joinToString("") { "%02x".format(it) }, out.length())
        }
    }
}
