package org.hibyos.install.core

import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * The companion apps update themselves from the project's GitHub releases (docs/APP_UPDATES.md).
 *
 * The newest release of the repository is the ByOS firmware release, so an app finds its own
 * newest release by listing the releases and taking the highest version among the tags with its
 * prefix. Each app release carries `byos-app-update.json`, a small JSON signed with the project's
 * Ed25519 release key (the same scheme as the player's updater, docs/OTA.md). Nothing in a
 * release is used before that signature is checked with the keys built into the app, and the
 * signed index fixes the app, the version, the file name, its size and its SHA-256: the file
 * itself is only kept when it is exactly that file.
 */
enum class AppId(val id: String, val tagPrefix: String, val extension: String) {
    /** ByOS Install for Android: `install-vX.Y.Z`, `hibyos-install-X.Y.Z.apk`. */
    ANDROID("byos-install-android", "install-v", ".apk"),

    /** ByOS Desktop for Windows: `desktop-vX.Y.Z`, `ByOS.exe` (the version is in the signed index). */
    DESKTOP_WINDOWS("byos-desktop-windows", "desktop-v", ".exe"),
}

/** A newer app version, as its signed index describes it. */
class AppRelease(
    val app: AppId,
    val version: String,
    /** Android's versionCode (null for the desktop app). */
    val versionCode: Long?,
    val tag: String,
    val asset: String,
    val size: Long,
    val sha256: String,
    /** Android: the lowest API level; Windows: the lowest version ("10.0.17134"). Null: any. */
    val minOs: String?,
    val published: String?,
    /** Release notes by language ("en", "es", "fr"). */
    val notes: Map<String, String>,
    /** Where the file is downloaded from: always built here from the tag and the file name. */
    val url: String,
) {
    /** The notes in [lang], else in English, else empty. */
    fun notesFor(lang: String): String = notes[lang.lowercase().substringBefore('-')] ?: notes["en"] ?: ""
}

object AppUpdater {
    const val INDEX_NAME = "byos-app-update.json"
    const val FORMAT = 1L
    const val PRODUCT = "byos-app-update"
    const val MAX_INDEX = 64 shl 10
    const val MAX_LIST = 8 shl 20
    const val MAX_ASSET = 512L shl 20

    /** The releases of the project, newest first (GitHub follows the repository's renames). */
    const val RELEASES_API = "https://api.github.com/repos/Grohle/hibyos/releases?per_page=100"

    /** Release files: DOWNLOADS + tag + "/" + file name. */
    const val DOWNLOADS = "https://github.com/Grohle/hibyos/releases/download/"

    /** The release keys built into the app (the player's own keys, src/ota_net.inc.c). */
    val KEYS: List<String> get() = org.hibyos.install.core.btp.UpdatePackage.RELEASE_KEYS

    private val VERSION = Regex("[0-9]{1,6}(\\.[0-9]{1,6}){1,3}")
    private val NAME = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,99}")

    /** True if [a] is a newer version than [b]. */
    fun isNewer(a: String, b: String): Boolean = BundleUpdater.isNewer(a, b)

    /** The version in [tag] when it is a release tag of [app] ("install-v1.2.0" -> "1.2.0"), else null. */
    fun tagVersion(app: AppId, tag: String): String? {
        if (!tag.startsWith(app.tagPrefix)) return null
        val v = tag.substring(app.tagPrefix.length)
        return if (VERSION.matches(v)) v else null
    }

    /** The newest published release of [app] (tag, version) that carries an index. */
    class Candidate(val tag: String, val version: String)

    /**
     * Picks the newest release of [app] from GitHub's release list: published (not a draft, not
     * a pre-release), a tag with the app's prefix and a plain version, and an index among its
     * files. Null when there is none.
     */
    fun newestRelease(list: ByteArray, app: AppId): Candidate? {
        val releases = try {
            Json.arr(Json.parse(String(list, Charsets.UTF_8)))
        } catch (e: JsonException) {
            throw DownloadException(DownloadException.Kind.NOT_A_FILE, "GitHub's list of releases cannot be read (${e.message})")
        } catch (e: ClassCastException) {
            throw DownloadException(DownloadException.Kind.NOT_A_FILE, "GitHub's list of releases cannot be read")
        }
        var best: Candidate? = null
        for (r in releases) {
            val m = r as? Map<*, *> ?: continue
            if (m["draft"] == true || m["prerelease"] == true) continue
            val tag = m["tag_name"] as? String ?: continue
            val version = tagVersion(app, tag) ?: continue
            val assets = m["assets"] as? List<*> ?: continue
            if (assets.none { (it as? Map<*, *>)?.get("name") == INDEX_NAME }) continue
            if (best == null || isNewer(version, best.version)) best = Candidate(tag, version)
        }
        return best
    }

    /**
     * Checks the signature of an index with [keys] first, then reads it. It must be the index
     * of [app] for the release [tag]. [downloads] is the base of the file's link.
     */
    fun parseIndex(data: ByteArray, keys: List<String>, app: AppId, tag: String, downloads: String = DOWNLOADS): AppRelease {
        val untrusted = { why: String -> DownloadException(DownloadException.Kind.UNTRUSTED, why) }
        if (data.size > MAX_INDEX) throw untrusted("the update description is too large")
        if (!Ed25519.verifySignedJson(data, keys)) throw untrusted("the update description is not signed by ByOS")
        try {
            val m = Json.obj(Json.parse(String(data, Charsets.UTF_8)))
            if (Json.optStr(m, "product") != PRODUCT) throw untrusted("this is not an app update description")
            if (Json.long(m, "format") != FORMAT) throw untrusted("this update needs a newer version of the app to be read")
            if (Json.str(m, "app") != app.id) throw untrusted("the update description is for another app")
            val version = Json.str(m, "version")
            if (!VERSION.matches(version)) throw untrusted("the update description has no valid version")
            val t = Json.str(m, "tag")
            if (t != tag || tagVersion(app, t) != version) throw untrusted("the update description belongs to another release")
            val asset = Json.str(m, "asset")
            if (!NAME.matches(asset) || !asset.endsWith(app.extension) || asset.contains("..")) throw untrusted("unexpected file name in the update description")
            val size = Json.long(m, "size")
            if (size <= 0 || size > MAX_ASSET) throw untrusted("unexpected size in the update description")
            val sha = Json.str(m, "sha256").lowercase()
            if (!Regex("[0-9a-f]{64}").matches(sha)) throw untrusted("unexpected SHA-256 in the update description")
            val code = if (app == AppId.ANDROID) Json.long(m, "version_code").also {
                if (it <= 0 || it > 2_100_000_000L) throw untrusted("unexpected version code in the update description")
            } else null
            val minOs = Json.optStr(m, "min_os")?.also {
                if (!Regex("[0-9]{1,6}(\\.[0-9]{1,6}){0,3}").matches(it)) throw untrusted("unexpected minimum system in the update description")
            }
            val notes = LinkedHashMap<String, String>()
            (m["notes"] as? Map<*, *>)?.forEach { (k, v) ->
                if (k is String && v is String && Regex("[a-z]{2}").matches(k)) notes[k] = v.take(4000)
            }
            return AppRelease(app, version, code, t, asset, size, sha, minOs, Json.optStr(m, "published")?.take(32), notes,
                downloads + t + "/" + asset)
        } catch (e: JsonException) {
            throw untrusted("the update description is not valid (${e.message})")
        }
    }

    /**
     * Asks the project's releases for a newer version of [app] than [current] (and, on Android,
     * a higher versionCode than [currentCode]). Returns null when [current] is the newest.
     * GitHub's request limit gives an HTTP error with status 429 and the time to wait.
     */
    fun check(
        app: AppId,
        current: String,
        currentCode: Long? = null,
        keys: List<String> = KEYS,
        api: String = RELEASES_API,
        downloads: String = DOWNLOADS,
        cancelled: () -> Boolean = { false },
    ): AppRelease? {
        val newest = newestRelease(apiGet(api, cancelled), app) ?: return null
        if (!isNewer(newest.version, current)) return null
        val index = Downloader.text(downloads + newest.tag + "/" + INDEX_NAME, MAX_INDEX, cancelled)
        val r = parseIndex(index, keys, app, newest.tag, downloads)
        if (!isNewer(r.version, current)) return null
        if (currentCode != null && (r.versionCode ?: 0) <= currentCode) return null
        return r
    }

    /**
     * Downloads [r] to [dest] (through "[dest].part", resumed when possible). The file is
     * kept only when its size and SHA-256 are the signed ones; otherwise nothing is left.
     */
    fun download(r: AppRelease, dest: File, cancelled: () -> Boolean = { false }, progress: ((Long, Long) -> Unit)? = null): File {
        if (dest.isFile && dest.length() == r.size && sha256(dest) == r.sha256) return dest
        val tmp = File(dest.path + ".new")
        Downloader.fetch(r.url, tmp, r.size, MAX_ASSET, cancelled, progress)
        try {
            if (tmp.length() != r.size || sha256(tmp) != r.sha256) {
                throw DownloadException(DownloadException.Kind.MISMATCH, "the downloaded file is not the signed ${r.asset}")
            }
            dest.delete()
            if (!tmp.renameTo(dest)) throw DownloadException(DownloadException.Kind.STORAGE, "the update could not be saved")
            return dest
        } finally {
            tmp.delete()
        }
    }

    fun sha256(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(f).use { s ->
            val b = ByteArray(1 shl 16)
            while (true) {
                val n = s.read(b)
                if (n < 0) break
                md.update(b, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    /** GET on GitHub's REST API, with its rate limit turned into a clear error. */
    internal fun apiGet(url: String, cancelled: () -> Boolean): ByteArray {
        val c = try {
            (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 15000
                readTimeout = 30000
                instanceFollowRedirects = true
                setRequestProperty("User-Agent", "ByOS-App-Update")
                setRequestProperty("Accept", "application/vnd.github+json")
                setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            }
        } catch (e: IOException) {
            throw DownloadException(DownloadException.Kind.OFFLINE, "no connection: ${e.message}", cause = e)
        }
        try {
            val status = try {
                c.responseCode
            } catch (e: IOException) {
                throw DownloadException(DownloadException.Kind.OFFLINE, "no connection: ${e.message}", cause = e)
            }
            if (status == 429 || (status == 403 && c.getHeaderField("X-RateLimit-Remaining") == "0")) {
                val reset = c.getHeaderField("X-RateLimit-Reset")?.toLongOrNull()
                val retry = c.getHeaderField("Retry-After")?.toLongOrNull()
                val wait = retry ?: reset?.let { it - System.currentTimeMillis() / 1000 } ?: 3600
                val minutes = maxOf(1, (wait + 59) / 60)
                throw DownloadException(DownloadException.Kind.HTTP,
                    "GitHub limits how often it can be asked; try again in $minutes min", 429)
            }
            if (status != 200) throw DownloadException(DownloadException.Kind.HTTP, "GitHub answered $status", status)
            val out = java.io.ByteArrayOutputStream()
            val buf = ByteArray(1 shl 16)
            c.inputStream.use {
                while (true) {
                    if (cancelled()) throw DownloadException(DownloadException.Kind.CANCELLED, "cancelled")
                    val n = it.read(buf)
                    if (n < 0) break
                    if (out.size() + n > MAX_LIST) throw DownloadException(DownloadException.Kind.NOT_A_FILE, "GitHub's answer is too long")
                    out.write(buf, 0, n)
                }
            }
            return out.toByteArray()
        } catch (e: IOException) {
            throw DownloadException(DownloadException.Kind.OFFLINE, "the connection failed: ${e.message}", cause = e)
        } finally {
            c.disconnect()
        }
    }
}
