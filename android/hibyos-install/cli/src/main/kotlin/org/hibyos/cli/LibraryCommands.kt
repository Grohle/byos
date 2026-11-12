package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpConnectionException
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.BtpPaths
import org.hibyos.install.core.btp.DataSink
import org.hibyos.install.core.library.AudioInfo
import org.hibyos.install.core.library.IndexPlaylist
import org.hibyos.install.core.library.IndexTrack
import org.hibyos.install.core.library.LibraryBuilder
import org.hibyos.install.core.library.LibraryIndex
import org.hibyos.install.core.library.M3uWriter
import org.hibyos.install.core.library.ManifestEntry
import org.hibyos.install.core.library.PictureRef
import org.hibyos.install.core.library.PictureWriter
import org.hibyos.install.core.library.RangeTagSource
import org.hibyos.install.core.library.RemoteFile
import org.hibyos.install.core.library.SourceTrack
import org.hibyos.install.core.library.SyncPlanner
import org.hibyos.install.core.library.SyncSelection
import org.hibyos.install.core.library.TagReader
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

/**
 * The library and music-sync commands of byos-cli (docs/BYOS_CLI.md, "Library and music"):
 * browsing the HiBy's library from a database snapshot or a card scan, album art for the
 * desktop app, playlists, the computer's music folders, the sync plan and its copy, and the
 * HiBy's cover cache.
 */
/** A file on the card, from LIST. */
class CardFile(val path: String, val size: Long, val mtime: Long)

class LibraryCommands(private val cli: Cli) {
    private val a get() = cli.a
    private val out get() = cli.out

    companion object {
        /** Where the HiBy's Music app keeps its cover cache (src/music.inc.c MU_CACHE). */
        const val COVER_CACHE = "ByOS/cache/music"
        const val MANIFEST = "ByOS/sync/manifest.json"
        /** Bytes read per file for a card scan's first request (tags and stream info). */
        const val SCAN_HEAD = 128 * 1024
        const val MAX_ART = 16 shl 20
        val COVER_NAMES = listOf("cover.jpg", "folder.jpg", "front.jpg", "albumart.jpg", "cover.jpeg", "folder.jpeg",
            "front.jpeg", "cover.png", "folder.png", "front.png")
        /** Formats whose embedded cover the sync can write (PictureWriter). */
        val COVER_FORMATS = setOf("flac", "mp3", "m4a", "m4b")
        private val SKIP_TOP = setOf("byos", "system volume information", "\$recycle.bin", "lost.dir")
    }

    // ------------------------------------------------------------------ helpers

    private fun cacheDir(vararg parts: String) = File(cli.config.cache, parts.joinToString(File.separator)).also { it.mkdirs() }

    private fun deviceId(c: Conn) = c.session.device.deviceId?.let(Config::hexOf) ?: "unknown"

    private fun readJsonFile(f: File): Map<String, Any?> = try { Json.obj(Json.parse(f.readText())) } catch (e: JsonException) {
        throw CliError("local_io", "${f.path} is not valid JSON: ${e.message}", path = f.path)
    }

    private fun sha1(s: String): String = Config.hexOf(MessageDigest.getInstance("SHA-1").digest(s.toByteArray(Charsets.UTF_8)))

    /** [len] bytes of a card file from [off] (fewer at its end): a GET cancelled once enough arrived. */
    internal fun fetch(c: Conn, path: String, off: Long, len: Int): ByteArray {
        val buf = ByteArrayOutputStream(minOf(len, 1 shl 20))
        var got = 0
        try {
            c.session.get(path, off, false, Btp.NS_SD, { got >= len }, null) { info ->
                if (info.offset != off) throw BtpException(Btp.E_PROTO, "GET answered at ${info.offset}, asked $off")
                DataSink { b, o, n -> val k = minOf(n, len - got); if (k > 0) { buf.write(b, o, k); got += k } }
            }
        } catch (e: BtpException) {
            if (e.code != Btp.E_CANCELLED || got < len) throw e
            cli.checkCancel()
        }
        return buf.toByteArray()
    }

    /** A card file's tags through partial downloads. */
    private fun remoteTags(c: Conn, path: String, size: Long): Pair<AudioInfo?, RangeTagSource> {
        val src = RangeTagSource(size) { off, n -> fetch(c, path, off, n) }
        src.prefetch(0, SCAN_HEAD.toLong())
        val info = try { TagReader.read(src, path.substringAfterLast('/')) } catch (e: IOException) { null }
        return info to src
    }

    private fun putBytes(c: Conn, path: String, data: ByteArray, mode: Int) {
        val sha = MessageDigest.getInstance("SHA-256").digest(data)
        c.session.put(path, data.size.toLong(), sha, mode, System.currentTimeMillis() / 1000, Btp.NS_SD, { cli.cancelled.get() }, null) { off ->
            ByteArrayInputStream(data, off.toInt(), data.size - off.toInt())
        }
    }

    /** Every file under [dir] (recursively, hidden names left out), as [CardFile]s. */
    private fun walk(c: Conn, dir: String, onDir: (String) -> Unit = {}): List<CardFile> {
        val outList = ArrayList<CardFile>()
        fun go(d: String, depth: Int) {
            cli.checkCancel()
            onDir(d)
            val entries = try { c.session.list(d) } catch (e: BtpException) { if (e.remote && e.code == Btp.E_NOT_FOUND) return else throw e }
            for (e in entries) {
                val p = BtpPaths.join(d, e.name)
                if (e.isDir) {
                    if (depth == 0 && e.name.lowercase() in SKIP_TOP) continue
                    if (depth < BtpPaths.MAX_DEPTH - 1) go(p, depth + 1)
                } else if (e.kind == 0) outList += CardFile(p, e.size, e.mtime)
            }
        }
        go(dir, if (dir.isEmpty()) 0 else dir.count { it == '/' } + 1)
        return outList
    }

    private fun isAudio(p: String) = p.substringAfterLast('.', "").lowercase() in LibraryBuilder.AUDIO_EXT && p.substringAfterLast('.', "").lowercase() != "iso"
    private fun isPlaylist(p: String) = p.substringAfterLast('.', "").lowercase() in LibraryBuilder.PLAYLIST_EXT

    private fun indexResult(idx: LibraryIndex, file: File, dev: String?): LinkedHashMap<String, Any?> = linkedMapOf(
        "index" to file.path, "device" to dev, "source" to idx.source, "revision" to idx.revision,
        "tracks" to idx.tracks.count { !it.book }, "albums" to idx.albums.size, "artists" to idx.artists.size,
        "genres" to idx.genres.size, "books" to idx.books.size, "playlists" to idx.playlists.size)

    // ------------------------------------------------------------------ lib-index

    fun index(): Map<String, Any?> {
        val given = a.opt("--db")?.let { File(it) }
        if (given != null && a.has("--pull")) throw CliError("usage", "--db and --pull exclude each other")
        if (given != null) {
            if (!given.isFile) throw CliError("local_io", "${given.path} does not exist", path = given.path)
            val outF = a.opt("--out")?.let { File(it) } ?: File(cacheDir("library", "local"), "index.json")
            val idx = LibraryIndex.fromSnapshot(given, cli.fileSha(given)) { d, t -> out.progress("index", null, d.toLong(), t.toLong(), unit = "rows") }
            outF.absoluteFile.parentFile?.mkdirs()
            cli.writeJson(outF, idx.toMap())
            return indexResult(idx, outF, null)
        }
        return cli.withConn { c ->
            val id = deviceId(c)
            val dir = cacheDir("library", id)
            val snap = File(dir, "snapshot.db")
            val part = File(dir, "snapshot.db.byos-part")
            part.delete()
            val md = MessageDigest.getInstance("SHA-256")
            val t0 = System.currentTimeMillis()
            var raf: RandomAccessFile? = null
            out.progress("pull", null, 0, 0, unit = "none")
            val res = try {
                c.session.dbSnapshot({ cli.cancelled.get() }, { d, t -> out.progress("pull", null, d, t, cli.rate(d, t0)) }) {
                    val r = RandomAccessFile(part, "rw"); raf = r
                    DataSink { b, o, n -> r.write(b, o, n); md.update(b, o, n) }
                }
            } catch (e: BtpException) {
                part.delete()
                if (e.remote && (e.code == Btp.E_IO || e.code == Btp.E_UNSUPPORTED)) {
                    throw CliError("remote", "The HiBy can't share its library database yet; scan the card instead (lib-scan)",
                        e.code, e.codeName, extra = mapOf("hint" to "scan"))
                }
                throw e
            } finally { raf?.close() }
            if (!md.digest().contentEquals(res.sha256)) { part.delete(); throw CliError("verify", "the snapshot did not arrive intact") }
            Files.move(part.toPath(), snap.toPath(), StandardCopyOption.REPLACE_EXISTING)
            val rev = cli.hex(res.sha256)
            val idx = LibraryIndex.fromSnapshot(snap, rev) { d, t -> out.progress("index", null, d.toLong(), t.toLong(), unit = "rows") }
            val outF = a.opt("--out")?.let { File(it) } ?: File(dir, "index.json")
            outF.absoluteFile.parentFile?.mkdirs()
            cli.writeJson(outF, idx.toMap())
            indexResult(idx, outF, id)
        }
    }

    // ------------------------------------------------------------------ lib-scan

    private class Cached(val size: Long, val mtime: Long, val m: Map<String, Any?>?)

    private fun trackCache(m: Map<String, Any?>): AudioInfo? {
        if (m["none"] == true) return null
        val i = AudioInfo(Json.optStr(m, "codec") ?: "")
        i.title = Json.optStr(m, "title"); i.artist = Json.optStr(m, "artist"); i.album = Json.optStr(m, "album")
        i.albumArtist = Json.optStr(m, "albumArtist"); i.genre = Json.optStr(m, "genre"); i.date = Json.optStr(m, "date")
        i.track = Json.optStr(m, "track"); i.disc = Json.optStr(m, "disc")
        i.sampleRate = Json.optLong(m, "sr", 0).toInt(); i.bitsPerSample = Json.optLong(m, "bits", 0).toInt()
        i.channels = Json.optLong(m, "ch", 0).toInt(); i.bitrate = Json.optLong(m, "bitrate", 0)
        i.durationMs = Json.optLong(m, "duration", 0); i.lossless = m["lossless"] == true; i.hasPicture = m["pic"] == true
        val po = Json.optLong(m, "pic_off", -1)
        if (po >= 0) i.picture = PictureRef(po, Json.optLong(m, "pic_len", 0), Json.optStr(m, "pic_mime") ?: "image/jpeg", 3)
        return i
    }

    private fun cacheEntry(size: Long, mtime: Long, i: AudioInfo?): Map<String, Any?> = if (i == null) linkedMapOf("size" to size, "mtime" to mtime, "none" to true)
    else linkedMapOf("size" to size, "mtime" to mtime, "codec" to i.codec, "title" to i.title, "artist" to i.artist, "album" to i.album,
        "albumArtist" to i.albumArtist, "genre" to i.genre, "date" to i.date, "track" to i.track, "disc" to i.disc, "sr" to i.sampleRate,
        "bits" to i.bitsPerSample, "ch" to i.channels, "bitrate" to i.bitrate, "duration" to i.durationMs, "lossless" to i.lossless,
        "pic" to i.hasPicture, "pic_off" to i.picture?.offset, "pic_len" to i.picture?.length, "pic_mime" to i.picture?.mime)

    private fun loadCache(f: File): MutableMap<String, Cached> {
        val m = HashMap<String, Cached>()
        if (!f.isFile || a.has("--full")) return m
        try {
            val root = Json.obj(Json.parse(f.readText()))
            for ((k, v) in Json.obj(root["files"])) {
                val e = Json.obj(v)
                m[k] = Cached(Json.optLong(e, "size", -1), Json.optLong(e, "mtime", -1), e)
            }
        } catch (e: JsonException) { out.warn("cache_reset", "the tag cache could not be read; reading every file again") }
        return m
    }

    fun scan(): Map<String, Any?> {
        val roots = a.pos.map { cli.remotePath(it, allowRoot = true) }.ifEmpty { listOf("") }
        return cli.withConn { c ->
            val id = deviceId(c)
            val dir = cacheDir("library", id)
            val cacheF = File(dir, "tags.json")
            val cache = loadCache(cacheF)
            val files = ArrayList<CardFile>()
            var folders = 0
            for (r in roots) files += walk(c, r) { folders++; out.progress("list", it, folders.toLong(), 0, unit = "folders") }
            val audio = files.filter { isAudio(it.path) }.distinctBy { it.path }
            val lists = files.filter { isPlaylist(it.path) }
            val newCache = LinkedHashMap<String, Map<String, Any?>>()
            val tracks = ArrayList<IndexTrack>()
            var cached = 0; var fetched = 0; var bytes = 0L
            for ((k, f) in audio.withIndex()) {
                cli.checkCancel()
                out.progress("tags", f.path, k.toLong(), audio.size.toLong(), unit = "files")
                val hit = cache[f.path]?.takeIf { it.size == f.size && it.mtime == f.mtime && it.m != null }
                val info: AudioInfo? = if (hit != null) {
                    cached++
                    trackCache(hit.m!!)
                } else {
                    val (i, src) = try { remoteTags(c, f.path, f.size) } catch (e: BtpException) {
                        if (!e.remote) throw e
                        out.warn("unreadable", "${f.path}: ${e.message}", mapOf("path" to f.path)); null to null
                    }
                    if (src != null) { fetched++; bytes += src.fetched }
                    if (i == null && src != null) out.warn("unreadable", "${f.path}: the tags could not be read", mapOf("path" to f.path))
                    i
                }
                newCache[f.path] = cacheEntry(f.size, f.mtime, info)
                tracks += LibraryIndex.track(k + 1L, f.path, f.size, f.mtime, info)
            }
            out.progress("tags", null, audio.size.toLong(), audio.size.toLong(), unit = "files")
            cli.writeJson(cacheF, linkedMapOf("format" to 1, "files" to newCache))
            val idx = LibraryIndex.fromTracks("scan", tracks,
                lists.map { IndexPlaylist(it.path, it.path.substringAfterLast('/').substringBeforeLast('.')) })
            val outF = a.opt("--out")?.let { File(it) } ?: File(dir, "index.json")
            outF.absoluteFile.parentFile?.mkdirs()
            cli.writeJson(outF, idx.toMap())
            indexResult(idx, outF, id).apply { put("files", audio.size); put("cached", cached); put("fetched", fetched); put("bytes", bytes) }
        }
    }

    // ------------------------------------------------------------------ lib-art

    /** The cover image of a card file: the folder's image first (as the HiBy does), else the embedded front cover. */
    private fun coverBytes(c: Conn, path: String, size: Long, folderImages: MutableMap<String, String?>): ByteArray? {
        val dir = path.substringBeforeLast('/', "")
        val folderImg = folderImages.getOrPut(dir) {
            val names = try { c.session.list(dir).filter { it.kind == 0 }.associateBy { it.name.lowercase() } } catch (e: BtpException) { emptyMap() }
            COVER_NAMES.firstNotNullOfOrNull { n -> names[n]?.takeIf { it.size in 1..MAX_ART }?.let { BtpPaths.join(dir, it.name) } }
        }
        if (folderImg != null) {
            val b = fetch(c, folderImg, 0, MAX_ART)
            if (CoverArt.info(b)?.let(CoverArt::usable) == true) return b
        }
        val (info, src) = remoteTags(c, path, size)
        val pic = info?.picture ?: return null
        if (pic.length !in 1..MAX_ART.toLong()) return null
        val b = ByteArray(pic.length.toInt())
        src.readAt(pic.offset, b)
        return b
    }

    fun art(): Map<String, Any?> {
        if (a.pos.isEmpty()) throw CliError("usage", "lib-art needs PATH...")
        val size = a.int("--size", CoverArt.THUMB_SIZE)
        if (size !in setOf(256, 500)) throw CliError("usage", "--size is 256 or 500")
        val max = a.int("--max", 64)
        if (a.pos.size > max) throw CliError("usage", "at most $max paths per run")
        val paths = a.pos.map { cli.remotePath(it) }.distinct()
        val dir = cacheDir("art")
        val items = ArrayList<Map<String, Any?>>()
        var conn: Conn? = null
        val folderImages = HashMap<String, String?>()
        try {
            for ((k, p) in paths.withIndex()) {
                cli.checkCancel()
                out.progress("art", p, k.toLong(), paths.size.toLong(), unit = "files")
                val c = conn ?: cli.connect(quiet = false).also { conn = it }
                val st = try { c.session.stat(p) } catch (e: BtpException) {
                    if (e.remote) { items += linkedMapOf("path" to p, "none" to true, "error" to e.codeName); continue } else throw e
                }
                val key = sha1("$p|${st.size}|${st.mtime}") + "-$size"
                val jpg = File(dir, "$key.jpg")
                val none = File(dir, "$key.none")
                if (jpg.isFile) { items += artItem(p, key, jpg); continue }
                if (none.isFile) { items += linkedMapOf("path" to p, "key" to key, "none" to true); continue }
                val img = try { coverBytes(c, p, st.size, folderImages) } catch (e: BtpException) { if (e.remote) null else throw e }
                val prepared = img?.let { CoverArt.prepare(it, size) }
                if (prepared == null) { none.writeText(""); items += linkedMapOf("path" to p, "key" to key, "none" to true); continue }
                val tmp = File(dir, "$key.jpg.tmp")
                tmp.writeBytes(prepared.bytes)
                Files.move(tmp.toPath(), jpg.toPath(), StandardCopyOption.REPLACE_EXISTING)
                items += artItem(p, key, jpg)
            }
        } finally {
            try { conn?.close() } catch (e: Exception) {}
        }
        out.progress("art", null, paths.size.toLong(), paths.size.toLong(), unit = "files")
        return linkedMapOf("count" to items.size, "items" to items)
    }

    private fun artItem(p: String, key: String, f: File): Map<String, Any?> {
        val colorF = File(f.path.removeSuffix(".jpg") + ".color")
        val color = colorF.takeIf { it.isFile }?.readText()?.trim() ?: CoverArt.averageColor(f.readBytes())?.also { colorF.writeText(it) }
        return linkedMapOf("path" to p, "key" to key, "file" to f.path, "color" to color)
    }

    // ------------------------------------------------------------------ lib-rescan

    fun rescan(): Map<String, Any?> {
        val full = a.has("--full")
        if (full == a.pos.isNotEmpty()) throw CliError("usage", "lib-rescan needs --full or PATH... (not both)")
        if (a.pos.size > 256) throw CliError("usage", "at most 256 paths per rescan")
        val paths = a.pos.map { cli.remotePath(it) }
        return cli.withConn { c ->
            out.progress("rescan", null, 0, 0, unit = "none")
            try {
                c.session.libRescan(if (full) null else paths)
            } catch (e: BtpException) {
                if (e.remote && e.code == Btp.E_UNSUPPORTED) throw CliError("remote",
                    if (full) "This HiBy cannot start a full library update from the computer yet; use Settings > Update library on the HiBy"
                    else "ByOS is not running on the HiBy, so it cannot refresh these songs", e.code, e.codeName, extra = mapOf("mode" to if (full) "full" else "paths"))
                throw e
            }
            linkedMapOf("mode" to if (full) "full" else "paths", "count" to paths.size)
        }
    }

    // ------------------------------------------------------------------ playlists

    /**
     * A playlist on the HiBy: a card path ("Playlists/Road Trip.m3u"), a file name in Playlists
     * ("Road Trip.m3u"), or a bare name for Playlists/<name>.m3u8.
     */
    private fun playlistPath(name: String): String = when {
        name.contains('/') -> cli.remotePath(name)
        isPlaylist(name) -> cli.remotePath(BtpPaths.join(M3uWriter.DIR, name))
        else -> M3uWriter.pathFor(name)
    }

    fun playlistLs(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "playlist-ls takes no arguments")
        var count = 0
        cli.withConn { c ->
            val entries = try { c.session.list(M3uWriter.DIR) } catch (e: BtpException) {
                if (e.remote && e.code == Btp.E_NOT_FOUND) emptyList() else throw e
            }
            for (e in entries.filter { it.kind == 0 && isPlaylist(it.name) }.sortedBy { it.name.lowercase() }) {
                cli.checkCancel()
                val p = BtpPaths.join(M3uWriter.DIR, e.name)
                val tracks = if (e.size <= (1 shl 20)) M3uWriter.read(p, String(fetch(c, p, 0, e.size.toInt()), Charsets.UTF_8)) else emptyList()
                out.event("entry", linkedMapOf("name" to e.name.substringBeforeLast('.'), "path" to p, "size" to e.size, "mtime" to e.mtime,
                    "tracks" to tracks.size, "items" to tracks))
                count++
            }
        }
        return linkedMapOf("count" to count)
    }

    fun playlistAdd(): Map<String, Any?> {
        if (a.pos.size < 2) throw CliError("usage", "playlist-add needs NAME PATH...")
        val pl = playlistPath(a.pos[0])
        val add = a.pos.drop(1).map { cli.remotePath(it) }
        return cli.withConn { c ->
            val st = c.session.statOrNull(pl)
            if (st == null && !a.has("--create")) throw CliError("usage", "there is no playlist $pl (add --create)", path = pl)
            val cur = if (st != null) M3uWriter.read(pl, String(fetch(c, pl, 0, st.size.toInt()), Charsets.UTF_8)) else emptyList()
            val have = cur.map { it.lowercase() }.toHashSet()
            val added = add.filter { have.add(it.lowercase()) }
            c.session.mkdir(pl.substringBeforeLast('/'), parents = true)
            putBytes(c, pl, M3uWriter.write(pl, cur + added).toByteArray(Charsets.UTF_8), Btp.MODE_REPLACE)
            linkedMapOf("playlist" to pl, "added" to added.size, "tracks" to cur.size + added.size, "created" to (st == null))
        }
    }

    fun playlistRm(): Map<String, Any?> {
        if (a.pos.isEmpty()) throw CliError("usage", "playlist-rm needs NAME [PATH...]")
        val pl = playlistPath(a.pos[0])
        val rm = a.pos.drop(1).map { cli.remotePath(it).lowercase() }.toSet()
        return cli.withConn { c ->
            val st = c.session.statOrNull(pl) ?: throw CliError("remote", "there is no playlist $pl", Btp.E_NOT_FOUND, Btp.errorName(Btp.E_NOT_FOUND), pl)
            if (rm.isEmpty()) {
                c.session.delete(pl, false)
                return@withConn linkedMapOf<String, Any?>("playlist" to pl, "deleted" to true, "removed" to 0)
            }
            val cur = M3uWriter.read(pl, String(fetch(c, pl, 0, st.size.toInt()), Charsets.UTF_8))
            val keep = cur.filter { it.lowercase() !in rm }
            putBytes(c, pl, M3uWriter.write(pl, keep).toByteArray(Charsets.UTF_8), Btp.MODE_REPLACE)
            linkedMapOf("playlist" to pl, "deleted" to false, "removed" to cur.size - keep.size, "tracks" to keep.size)
        }
    }

    // ------------------------------------------------------------------ music-scan (this computer)

    fun musicScan(): Map<String, Any?> {
        if (a.pos.isEmpty()) throw CliError("usage", "music-scan needs DIR...")
        val roots = a.pos.map { File(it).absoluteFile }
        for (r in roots) if (!r.isDirectory) throw CliError("local_io", "${r.path} is not a folder", path = r.path)
        val dir = cacheDir("music")
        val cacheF = File(dir, "tags.json")
        val cache = loadCache(cacheF)
        val files = ArrayList<Pair<File, File>>()
        var folders = 0
        for (r in roots) {
            r.walkTopDown().onEnter { d -> cli.checkCancel(); folders++; out.progress("scan", d.path, folders.toLong(), 0, unit = "folders"); !d.name.startsWith(".") || d == r }
                .filter { it.isFile && isAudio(it.name) && !it.name.startsWith(".") }.forEach { files += it to r }
        }
        val newCache = LinkedHashMap<String, Map<String, Any?>>()
        val tracks = ArrayList<IndexTrack>()
        var changed = 0; var bytes = 0L
        for ((k, pr) in files.withIndex()) {
            cli.checkCancel()
            val (f, _) = pr
            val path = f.path.replace('\\', '/')
            out.progress("tags", path, k.toLong(), files.size.toLong(), unit = "files")
            val size = f.length(); val mtime = f.lastModified() / 1000
            val hit = cache[path]?.takeIf { it.size == size && it.mtime == mtime && it.m != null }
            val info = if (hit != null) trackCache(hit.m!!) else {
                changed++
                try { TagReader.read(f) } catch (e: IOException) { out.warn("unreadable", "$path: ${e.message}", mapOf("path" to path)); null }
            }
            newCache[path] = cacheEntry(size, mtime, info)
            tracks += LibraryIndex.track(k + 1L, path, size, mtime, info)
            bytes += size
        }
        out.progress("tags", null, files.size.toLong(), files.size.toLong(), unit = "files")
        cli.writeJson(cacheF, linkedMapOf("format" to 1, "files" to newCache))
        val idx = LibraryIndex.fromTracks("local", tracks, emptyList())
        val outF = a.opt("--out")?.let { File(it) } ?: File(dir, "catalog.json")
        outF.absoluteFile.parentFile?.mkdirs()
        cli.writeJson(outF, LinkedHashMap(idx.toMap()).apply { put("roots", roots.map { it.path.replace('\\', '/') }) })
        return linkedMapOf("catalog" to outF.path, "roots" to roots.map { it.path }, "tracks" to idx.tracks.count { !it.book },
            "albums" to idx.albums.size, "artists" to idx.artists.size, "books" to idx.books.size, "bytes" to bytes, "changed" to changed)
    }

    // ------------------------------------------------------------------ sync-plan

    private fun sourcesFrom(catalog: Map<String, Any?>): List<SourceTrack> {
        val roots = (catalog["roots"] as? List<*>)?.map { it.toString() } ?: emptyList()
        return Json.arr(catalog["tracks"]).map { v ->
            val m = Json.obj(v)
            val path = Json.str(m, "path")
            val root = roots.filter { path.startsWith("$it/") }.maxByOrNull { it.length }
            val rel = if (root != null) path.substring(root.length + 1) else path.substringAfterLast('/')
            val t = IndexTrack(Json.optLong(m, "id", 0), path, Json.optStr(m, "title") ?: "", Json.optStr(m, "artist") ?: "",
                Json.optStr(m, "albumArtist") ?: "", Json.optStr(m, "album") ?: "", Json.optStr(m, "genre") ?: "",
                Json.optLong(m, "year", 0).toInt(), Json.optLong(m, "track", 0).toInt(), Json.optLong(m, "disc", 0).toInt(),
                Json.optLong(m, "duration", 0), Json.optLong(m, "size", 0), Json.optLong(m, "mtime", 0), Json.optLong(m, "ctime", 0),
                Json.optLong(m, "sr", 0).toInt(), Json.optLong(m, "bits", 0).toInt(), Json.optLong(m, "kbps", 0).toInt(),
                m["lossless"] == true, m["art"] == true, m["book"] == true)
            SourceTrack(File(path).path, rel, t)
        }
    }

    private fun selectionFrom(m: Map<String, Any?>): SyncSelection {
        fun names(k: String) = (m[k] as? List<*>)?.map { LibraryIndex.fold(it.toString()) }?.toSet() ?: emptySet()
        val albums = (m["albums"] as? List<*>)?.map { v ->
            val o = Json.obj(v); SyncSelection.albumKey(Json.optStr(o, "artist") ?: "", Json.optStr(o, "title") ?: "")
        }?.toSet() ?: emptySet()
        val files = (m["files"] as? List<*>)?.map { File(it.toString()).path }?.toSet() ?: emptySet()
        return SyncSelection(m["all"] != false, names("artists"), albums, names("genres"), files, m["books"] != false)
    }

    private fun readManifest(c: Conn): List<ManifestEntry> {
        val st = c.session.statOrNull(MANIFEST) ?: return emptyList()
        if (st.kind != 0 || st.size > (32 shl 20)) return emptyList()
        return try {
            val m = Json.obj(Json.parse(String(fetch(c, MANIFEST, 0, st.size.toInt()), Charsets.UTF_8)))
            Json.obj(m["files"]).map { (dest, v) ->
                val e = Json.obj(v)
                ManifestEntry(dest, Json.optStr(e, "src") ?: "", Json.optLong(e, "src_size", -1), Json.optLong(e, "src_mtime", -1),
                    Json.optLong(e, "size", -1), Json.optStr(e, "covers") ?: "keep")
            }
        } catch (e: JsonException) {
            out.warn("manifest_unreadable", "$MANIFEST could not be read; files already on the HiBy are compared by size")
            emptyList()
        }
    }

    fun syncPlan(): Map<String, Any?> {
        val catF = File(a.opt("--catalog") ?: throw CliError("usage", "sync-plan needs --catalog CATALOG.json"))
        val selF = a.opt("--select")?.let { File(it) }
        val dest = cli.remotePath(a.opt("--dest") ?: "Music")
        val layout = a.opt("--layout") ?: "artist-album"
        if (layout !in setOf("artist-album", "keep")) throw CliError("usage", "--layout is artist-album or keep")
        val covers = a.opt("--covers") ?: "optimize"
        if (covers !in setOf("keep", "optimize")) throw CliError("usage", "--covers is keep or optimize")
        if (!catF.isFile) throw CliError("local_io", "${catF.path} does not exist", path = catF.path)
        val catalog = readJsonFile(catF)
        val sources = sourcesFrom(catalog)
        // only the folders scanned this time: "remove unselected" leaves the others' songs alone
        val roots = (catalog["roots"] as? List<*>)?.map { it.toString() } ?: emptyList()
        val sel = selF?.let { if (!it.isFile) throw CliError("local_io", "${it.path} does not exist", path = it.path); selectionFrom(readJsonFile(it)) } ?: SyncSelection()
        val plan = cli.withConn { c ->
            val remote = ArrayList<RemoteFile>()
            var folders = 0
            for (d in listOf(dest, "Audiobooks").distinct()) {
                remote += walk(c, d) { folders++; out.progress("list", it, folders.toLong(), 0, unit = "folders") }.map { RemoteFile(it.path, it.size, it.mtime) }
            }
            val manifest = readManifest(c)
            val df = c.session.df()
            SyncPlanner.plan(sources, sel, dest, layout, covers, remote, manifest, a.has("--remove-unselected"), df.sdFree, roots)
        }
        val outF = a.opt("--out")?.let { File(it) } ?: File(cacheDir("sync"), "plan.json")
        outF.absoluteFile.parentFile?.mkdirs()
        cli.writeJson(outF, plan.toMap())
        return linkedMapOf("plan" to outF.path, "add" to plan.add, "update" to plan.update, "skip" to plan.skip, "remove" to plan.remove,
            "bytes_add" to plan.bytesAdd, "bytes_remove" to plan.bytesRemove, "sd_free" to plan.sdFree, "fits" to plan.fits)
    }

    // ------------------------------------------------------------------ sync-apply

    /** The best cover for a local track: the folder's image, else its embedded front cover. */
    private fun localCover(f: File, info: AudioInfo?): ByteArray? {
        val dir = f.absoluteFile.parentFile
        val names = dir?.listFiles()?.filter { it.isFile }?.associateBy { it.name.lowercase() } ?: emptyMap()
        COVER_NAMES.firstNotNullOfOrNull { names[it] }?.takeIf { it.length() in 1..MAX_ART }?.let { img ->
            val b = img.readBytes()
            if (CoverArt.info(b)?.let(CoverArt::usable) == true) return b
        }
        val p = info?.picture ?: return null
        if (p.length !in 1..MAX_ART.toLong()) return null
        return RandomAccessFile(f, "r").use { r -> ByteArray(p.length.toInt()).also { b -> r.seek(p.offset); r.readFully(b) } }
    }

    /**
     * The file to send for [src]: a copy with a [CoverArt.DEVICE_SIZE] px baseline JPEG as its
     * front cover when its art is too large, progressive, PNG, or only in the folder; [src]
     * itself when it is already right or its format cannot carry a cover.
     */
    internal fun prepareCover(src: File, work: File): File {
        val ext = src.name.substringAfterLast('.', "").lowercase()
        if (ext !in COVER_FORMATS) return src
        val info = try { TagReader.read(src) } catch (e: IOException) { return src }
        val embedded = info.picture?.let { p ->
            if (p.length !in 1..MAX_ART.toLong()) null
            else RandomAccessFile(src, "r").use { r -> ByteArray(minOf(p.length, 65536L).toInt()).also { b -> r.seek(p.offset); r.readFully(b) } }
        }?.let { CoverArt.info(it) }
        if (embedded != null && !CoverArt.needsFix(embedded)) return src
        val img = localCover(src, info) ?: return src
        val prepared = CoverArt.prepare(img, CoverArt.DEVICE_SIZE) ?: return src
        work.mkdirs()
        val dst = File(work, "cover-" + System.nanoTime() + "." + ext)
        return try {
            PictureWriter.setCover(src, dst, prepared.bytes, prepared.width, prepared.height)
            dst
        } catch (e: IOException) {
            dst.delete()
            out.warn("cover_skipped", "${src.path}: ${e.message}", mapOf("path" to src.path))
            src
        }
    }

    fun syncApply(): Map<String, Any?> {
        if (a.pos.size != 1) throw CliError("usage", "sync-apply needs PLAN.json")
        val planF = File(a.pos[0])
        if (!planF.isFile) throw CliError("local_io", "${planF.path} does not exist", path = planF.path)
        val plan = readJsonFile(planF)
        val covers = a.opt("--covers") ?: Json.optStr(plan, "covers") ?: "optimize"
        if (covers !in setOf("keep", "optimize")) throw CliError("usage", "--covers is keep or optimize")
        val then = a.opt("--then") ?: "none"
        if (then !in setOf("none", "rescan", "rebuild")) throw CliError("usage", "--then is none, rescan or rebuild")
        class Item(val action: String, val src: File?, val dest: String, val size: Long, val reason: String)
        val items = Json.arr(plan["items"]).map { v ->
            val m = Json.obj(v)
            Item(Json.str(m, "action"), Json.optStr(m, "src")?.let { File(it) }, cli.remotePath(Json.str(m, "dest")), Json.optLong(m, "size", 0),
                Json.optStr(m, "reason") ?: "")
        }
        val copies = items.filter { it.action == "add" || it.action == "update" }
        val removals = items.filter { it.action == "remove" }
        for (it in copies) if (it.src == null || !it.src.isFile) throw CliError("local_io", "${it.src?.path} is gone; scan the music folders again", path = it.src?.path)
        if (removals.isNotEmpty() && !a.has("--yes")) {
            if (!cli.confirm("Remove ${removals.size} songs from the HiBy that are no longer selected?", linkedMapOf("count" to removals.size))) {
                throw CliError("cancelled", "nothing was copied or removed")
            }
        }
        val work = File(cli.config.cache, "sync" + File.separator + "work-" + ProcessHandle.current().pid())
        val bytesTotal = copies.sumOf { it.size }
        var bytesDone = 0L
        var written = 0; var skipped = items.count { it.action == "skip" }; var failed = 0; var removed = 0; var bytes = 0L
        val results = ArrayList<Map<String, Any?>>()
        val done = LinkedHashMap<String, ManifestEntry>()
        val gone = HashSet<String>()
        var conn: Conn? = null
        var library: Map<String, Any?> = linkedMapOf("action" to then, "ok" to (then == "none"))
        val startedAt = System.currentTimeMillis()
        fun overall(k: Int) = out.progress("sync", null, bytesDone, bytesTotal, cli.rate(bytesDone, startedAt),
            extra = mapOf("files_done" to k, "files_total" to copies.size))
        try {
            conn = cli.connect()
            val manifest = readManifest(conn!!).associateBy { it.dest }.toMutableMap()
            // skipped sync copies keep their manifest entry (with the new source when it moved); a
            // "same_size" file was not copied by a sync, so it stays the user's and is not recorded
            for (it in items.filter { it.action == "skip" && it.src != null && it.reason != "same_size" }) {
                val s = it.src!!
                if (manifest[it.dest]?.src != s.path) done[it.dest] = ManifestEntry(it.dest, s.path, s.length(), s.lastModified() / 1000, it.size, covers)
            }
            val dirs = copies.map { it.dest.substringBeforeLast('/', "") }.filter { it.isNotEmpty() }.distinct()
            for (d in dirs) conn!!.session.mkdir(d, parents = true)
            try {
                for ((k, it) in copies.withIndex()) {
                    cli.checkCancel()
                    overall(k)
                    val src = it.src!!
                    out.progress("prepare", it.dest, 0, 0, unit = "none")
                    val send = if (covers == "optimize") prepareCover(src, work) else src
                    try {
                        val sha = cli.sha256File(send, "hash", it.dest)
                        var attempt = 0
                        while (true) {
                            try {
                                val c = conn ?: cli.connect(quiet = true).also { n -> conn = n }
                                val t0 = System.currentTimeMillis()
                                val base = bytesDone
                                out.progress("put", it.dest, 0, send.length())
                                val r = c.session.put(it.dest, send.length(), sha, if (it.action == "update") Btp.MODE_REPLACE else Btp.MODE_SKIP_IDENTICAL,
                                    src.lastModified() / 1000, Btp.NS_SD, { cli.cancelled.get() },
                                    { d, t -> out.progress("put", it.dest, d, t, cli.rate(d, t0)); bytesDone = base + d * it.size / maxOf(1, t) }) { off ->
                                    FileInputStream(send).also { s -> s.channel.position(off) }
                                }
                                if (r.result == 1) { skipped++; results += linkedMapOf("dest" to it.dest, "result" to "skipped") }
                                else { written++; bytes += r.size; results += linkedMapOf("dest" to it.dest, "result" to "written") }
                                done[it.dest] = ManifestEntry(it.dest, src.path, src.length(), src.lastModified() / 1000, send.length(), covers)
                                if (a.has("--lyrics")) sendLyrics(c, src, it.dest)
                                break
                            } catch (e: BtpConnectionException) {
                                try { conn?.close() } catch (x: Exception) {}
                                conn = null
                                if (attempt >= cli.retryDelaysMs.size) throw e
                                out.warn("reconnecting", "connection lost; retrying in ${cli.retryDelaysMs[attempt] / 1000.0} s", mapOf("path" to it.dest))
                                Thread.sleep(cli.retryDelaysMs[attempt])
                                attempt++
                            }
                        }
                    } catch (e: BtpException) {
                        if (!e.remote || e.code in setOf(Btp.E_SD_UNAVAILABLE, Btp.E_NO_SPACE, Btp.E_CANCELLED, Btp.E_BUSY)) throw e
                        failed++
                        val ce = cli.mapError(e)
                        out.warn("failed", "${it.dest}: ${ce.message}", mapOf("path" to it.dest, "src" to src.path))
                        results += linkedMapOf("dest" to it.dest, "src" to src.path, "result" to "failed", "error" to e.codeName)
                    } finally {
                        if (send != src) send.delete()
                    }
                    bytesDone = copies.take(k + 1).sumOf { x -> x.size }
                }
                overall(copies.size)
                if (removals.isNotEmpty()) {
                    val c = conn ?: cli.connect(quiet = true).also { n -> conn = n }
                    val deleted = ArrayList<String>()
                    for ((k, it) in removals.withIndex()) {
                        cli.checkCancel()
                        out.progress("remove", it.dest, k.toLong(), removals.size.toLong(), unit = "files")
                        try { c.session.delete(it.dest, false); deleted += it.dest; removed++; gone += it.dest } catch (e: BtpException) {
                            if (!e.remote) throw e
                            if (e.code == Btp.E_NOT_FOUND) gone += it.dest else out.warn("failed", "${it.dest}: ${e.message}", mapOf("path" to it.dest))
                        }
                    }
                    cli.pruneEmpty(c, deleted)
                }
            } finally {
                // the manifest records whatever was copied, even after a cancel or a lost
                // connection, so the next sync neither copies these files again nor duplicates them
                if (done.isNotEmpty() || gone.isNotEmpty()) try {
                    val c = conn ?: cli.connect(quiet = true).also { n -> conn = n }
                    for (g in gone) manifest.remove(g)
                    manifest.putAll(done)
                    c.session.mkdir(MANIFEST.substringBeforeLast('/'), parents = true)
                    putBytes(c, MANIFEST, (JsonOut.encode(linkedMapOf("format" to 1, "files" to manifest.values.sortedBy { it.dest }.associate { m ->
                        m.dest to linkedMapOf("src" to m.src, "src_size" to m.srcSize, "src_mtime" to m.srcMtime, "size" to m.size, "covers" to m.covers)
                    })) + "\n").toByteArray(Charsets.UTF_8), Btp.MODE_REPLACE)
                } catch (e: Exception) {
                    out.warn("manifest_not_saved", "the sync record on the HiBy could not be updated: ${e.message}")
                }
            }
            // the library on the HiBy (docs/BYOS_CLI.md, sync-apply --then)
            if (then != "none") library = libraryStep(conn ?: cli.connect(quiet = true).also { n -> conn = n }, then, done.keys.toList())
        } finally {
            try { conn?.close() } catch (e: Exception) {}
            work.deleteRecursively()
        }
        return linkedMapOf("written" to written, "skipped" to skipped, "failed" to failed, "removed" to removed, "bytes" to bytes,
            "items" to results, "library" to library)
    }

    private fun sendLyrics(c: Conn, src: File, dest: String) {
        val lrc = src.absoluteFile.parentFile?.listFiles()?.firstOrNull { it.isFile && it.name.equals(src.nameWithoutExtension + ".lrc", ignoreCase = true) } ?: return
        if (lrc.length() > (1 shl 20)) return
        val d = dest.substringBeforeLast('.') + ".lrc"
        try { putBytes(c, d, lrc.readBytes(), Btp.MODE_REPLACE) } catch (e: BtpException) {
            if (!e.remote) throw e
            out.warn("lyrics_failed", "$d: ${e.message}", mapOf("path" to d))
        }
    }

    /**
     * Tells the HiBy about the new music. "rescan": LIB_RESCAN (a full rescan; the HiBy's
     * stock player cannot be asked for one yet, so this usually answers E_UNSUPPORTED).
     * "rebuild": building the database on the computer needs DB_SNAPSHOT and DB_STAGE on the
     * HiBy (not there yet) and a copy of the card's music on the computer: not done here.
     * Either way, "library_pending" tells the user to update the library on the HiBy.
     */
    private fun libraryStep(c: Conn, then: String, paths: List<String>): Map<String, Any?> {
        out.progress("library", null, 0, 0, unit = "none")
        if (then == "rebuild") {
            out.warn("library_pending", "Music copied. To see it in the player, open Settings > Update library on the HiBy.")
            return linkedMapOf("action" to then, "ok" to false, "code" to "E_UNSUPPORTED")
        }
        return try {
            c.session.libRescan(null)
            linkedMapOf("action" to then, "ok" to true, "count" to paths.size)
        } catch (e: BtpException) {
            if (!e.remote) throw e
            out.warn("library_pending", "Music copied. To see it in the player, open Settings > Update library on the HiBy.")
            linkedMapOf("action" to then, "ok" to false, "code" to e.codeName)
        }
    }

    // ------------------------------------------------------------------ the HiBy's cover cache

    fun coverCacheReset(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "cover-cache-reset takes no arguments")
        return cli.withConn { c -> linkedMapOf("deleted" to resetCovers(c)) }
    }

    private fun resetCovers(c: Conn): Int {
        val entries = try { c.session.list(COVER_CACHE, true) } catch (e: BtpException) {
            if (e.remote && e.code == Btp.E_NOT_FOUND) return 0 else throw e
        }
        var n = 0
        for (e in entries) {
            if (e.kind != 0 || !(e.name.endsWith(".none") || e.name.endsWith("_b.raw"))) continue
            cli.checkCancel()
            try { c.session.delete(BtpPaths.join(COVER_CACHE, e.name), false); n++ } catch (x: BtpException) { if (!x.remote) throw x }
        }
        return n
    }

    // ------------------------------------------------------------------ art-fix

    fun artFix(): Map<String, Any?> {
        if (a.pos.isNotEmpty()) throw CliError("usage", "art-fix takes no arguments (use --root)")
        val root = cli.remotePath(a.opt("--root") ?: "Music")
        val dry = a.has("--dry-run")
        return cli.withConn { c ->
            var folders = 0
            val files = walk(c, root) { folders++; out.progress("scan", it, folders.toLong(), 0, unit = "folders") }
            val byDir = files.filter { isAudio(it.path) && it.path.substringAfterLast('.').lowercase() in COVER_FORMATS }.groupBy { it.path.substringBeforeLast('/', "") }
            val folderImg = files.filter { it.path.substringAfterLast('/').lowercase() in COVER_NAMES }.groupBy { it.path.substringBeforeLast('/', "") }
            val candidates = ArrayList<Pair<CardFile, String>>()
            var k = 0
            val total = byDir.values.sumOf { it.size }
            for ((dir, list) in byDir) {
                for (f in list) {
                    cli.checkCancel()
                    out.progress("scan", f.path, (k++).toLong(), total.toLong(), unit = "files")
                    val (info, src) = try { remoteTags(c, f.path, f.size) } catch (e: BtpException) { if (e.remote) null to null else throw e }
                    if (info == null || src == null) continue
                    val p = info.picture
                    val reason = if (p == null) {
                        if (folderImg[dir] != null && !info.hasPicture) "missing" else null
                    } else {
                        val head = ByteArray(minOf(p.length, 65536L).toInt()); src.readAt(p.offset, head)
                        val ci = CoverArt.info(head)
                        when {
                            ci == null -> null
                            ci.progressive -> "progressive"
                            ci.format != "jpeg" -> "png"
                            maxOf(ci.width, ci.height) > CoverArt.FIX_ABOVE -> "large"
                            else -> null
                        }
                    }
                    if (reason != null) candidates += f to reason
                }
            }
            val albums = candidates.map { it.first.path.substringBeforeLast('/', "") }.distinct()
            for (d in albums) {
                val fs = candidates.filter { it.first.path.substringBeforeLast('/', "") == d }
                out.event("entry", linkedMapOf("path" to d, "files" to fs.size, "reasons" to fs.map { it.second }.distinct()))
            }
            if (dry) return@withConn linkedMapOf<String, Any?>("albums" to albums.size, "files" to candidates.size, "fixed" to 0, "skipped" to 0, "dry_run" to true)
            val work = File(cli.config.cache, "art-fix-" + ProcessHandle.current().pid())
            var fixed = 0; var skipped = 0
            try {
                for ((i, pr) in candidates.withIndex()) {
                    cli.checkCancel()
                    val f = pr.first
                    out.progress("get", f.path, i.toLong(), candidates.size.toLong(), unit = "files")
                    // a clean folder for each file: an image left by another album must never
                    // become this file's cover
                    work.deleteRecursively()
                    work.mkdirs()
                    val local = File(work, "in-" + i + "." + f.path.substringAfterLast('.'))
                    // the folder's image travels with the track, so the local copy finds it
                    folderImg[f.path.substringBeforeLast('/', "")]?.firstOrNull()?.let { img ->
                        File(work, img.path.substringAfterLast('/')).writeBytes(fetch(c, img.path, 0, minOf(img.size, MAX_ART.toLong()).toInt()))
                    }
                    RandomAccessFile(local, "rw").use { r ->
                        r.setLength(0)
                        val t0 = System.currentTimeMillis()
                        c.session.get(f.path, 0, true, Btp.NS_SD, { cli.cancelled.get() }, { d, t -> out.progress("get", f.path, d, t, cli.rate(d, t0)) }) {
                            DataSink { b, o, n -> r.write(b, o, n) }
                        }
                    }
                    val fixedFile = prepareCover(local, work)
                    if (fixedFile == local) { skipped++; work.deleteRecursively(); continue }
                    out.progress("put", f.path, i.toLong(), candidates.size.toLong(), unit = "files")
                    val sha = cli.sha256File(fixedFile, "hash", f.path)
                    c.session.put(f.path, fixedFile.length(), sha, Btp.MODE_REPLACE, f.mtime, Btp.NS_SD, { cli.cancelled.get() }, null) { off ->
                        FileInputStream(fixedFile).also { s -> s.channel.position(off) } as InputStream
                    }
                    work.deleteRecursively()
                    fixed++
                }
            } finally { work.deleteRecursively() }
            val reset = if (fixed > 0) resetCovers(c) else 0
            linkedMapOf<String, Any?>("albums" to albums.size, "files" to candidates.size, "fixed" to fixed, "skipped" to skipped, "cache_reset" to reset)
        }
    }
}
