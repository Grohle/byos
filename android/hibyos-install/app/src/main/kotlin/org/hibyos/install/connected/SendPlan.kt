package org.hibyos.install.connected

import java.io.InputStream

/** A file chosen on the phone. [parts] is its path inside what was picked: a folder keeps its tree. */
class LocalItem(
    val parts: List<String>,
    val size: Long,
    val mtime: Long,
    val open: () -> InputStream,
)

/**
 * WAITING to CANCELLED as their names say; UPDATE_FILE: a ByOS update file (hibyos-update.json,
 * hibyos-payload-*.tar), which goes through Install Update and not to the card.
 */
enum class JobStatus { WAITING, HASHING, SENDING, SENT, SKIPPED, EXISTS, TOO_BIG, BAD_NAME, FAILED, CANCELLED, UPDATE_FILE }

/** Where the automatic destination puts a file, by its kind; [folder] is the default card folder. */
enum class Route(val folder: String) {
    MUSIC("Music"), PICTURES("Pictures"), BOOKS("Books"), PLAYLISTS("Playlists"), APPS("Apps"),

    /** r3ii_2025.upt: the top of the card, where the player looks for a firmware update. */
    FIRMWARE(""),
    OTHER(""),

    /** ByOS update files: not sent to the card (Install Update takes them). */
    BYOS_UPDATE(""),
}

/** One file of a transfer, as the screens show it. */
data class SendJob(
    val id: Int,
    /** The name on the phone (path inside the picked folder). */
    val local: String,
    /** Where it goes on the card. */
    val remote: String,
    val size: Long,
    val status: JobStatus = JobStatus.WAITING,
    val done: Long = 0,
    val error: Int = 0,
    /** With the automatic destination: the group the file was put in. */
    val route: Route? = null,
) {
    val finished: Boolean get() = status == JobStatus.SENT || status == JobStatus.SKIPPED
    val retryable: Boolean get() = status == JobStatus.WAITING || status == JobStatus.FAILED || status == JobStatus.CANCELLED ||
        status == JobStatus.HASHING || status == JobStatus.SENDING
}

/** The jobs of a transfer and the folders to create first. */
class SendPlan(val jobs: List<SendJob>, val folders: List<String>, val sources: Map<Int, LocalItem>) {
    companion object {
        /**
         * Picks a group for each item: songs to Music, keeping the picked folders; lyrics, cue
         * sheets and pictures that sit in a folder with songs stay beside them (cover.jpg with
         * its album); other pictures to Pictures, books to Books, playlists to Playlists, ByOS
         * apps to Apps, the firmware to the top of the card; anything else keeps its folders at
         * the top of the card. ByOS update files are not sent (Install Update takes them).
         */
        fun routeOf(item: LocalItem, musicDirs: Set<String>): Route {
            val name = item.parts.last()
            val dir = item.parts.dropLast(1).joinToString("/").lowercase()
            // files picked one by one all share the "" folder: there only artwork follows the songs
            val besideMusic = dir in musicDirs && (dir.isNotEmpty() || name.lowercase() in FileKinds.coverNames)
            return when (FileKinds.of(name)) {
                FileKind.AUDIO -> Route.MUSIC
                FileKind.LYRICS -> Route.MUSIC
                FileKind.IMAGE -> if (besideMusic) Route.MUSIC else Route.PICTURES
                FileKind.PLAYLIST -> if (FileKinds.ext(name) == "cue" || besideMusic) Route.MUSIC else Route.PLAYLISTS
                FileKind.BOOK -> if (besideMusic && FileKinds.ext(name) == "txt") Route.MUSIC else Route.BOOKS
                FileKind.APP -> Route.APPS
                FileKind.FIRMWARE -> Route.FIRMWARE
                FileKind.BYOS_UPDATE -> Route.BYOS_UPDATE
                FileKind.OTHER -> if (besideMusic) Route.MUSIC else Route.OTHER
            }
        }

        /**
         * The automatic destination: each item goes to the folder of its group ([folders]
         * replaces the defaults of [Route.folder]). Files that the player wants in one place
         * (the firmware, apps, playlists) lose the picked folders; the others keep them.
         */
        fun auto(items: List<LocalItem>, folders: Map<Route, String> = emptyMap(), maxFile: Long = Long.MAX_VALUE): SendPlan {
            val musicDirs = items.filter { FileKinds.isAudio(it.parts.last()) }.map { it.parts.dropLast(1).joinToString("/").lowercase() }.toSet()
            val routes = HashMap<LocalItem, Route>()
            for (it in items) routes[it] = routeOf(it, musicDirs)
            return build(items, maxFile = maxFile, place = { item ->
                val r = routes[item]!!
                val base = folders[r] ?: r.folder
                val flat = r == Route.FIRMWARE || r == Route.APPS || r == Route.PLAYLISTS
                Triple(base, if (flat) listOf(item.parts.last()) else item.parts, r)
            })
        }

        /**
         * Places [items] under [dest] (a card path, "" = the top of the card). Names are cleaned
         * for the card (SyncPaths.cleanName); two files that would end with the same name get
         * " (2)", " (3)"...; hidden files and names that cannot be made valid are listed as
         * BAD_NAME and not sent; files over [maxFile] are TOO_BIG.
         */
        fun build(items: List<LocalItem>, dest: String, maxFile: Long = Long.MAX_VALUE, firstId: Int = 1): SendPlan =
            build(items, maxFile, firstId) { Triple(dest, it.parts, null) }

        private fun build(items: List<LocalItem>, maxFile: Long = Long.MAX_VALUE, firstId: Int = 1,
                          place: (LocalItem) -> Triple<String, List<String>, Route?>): SendPlan {
            val jobs = ArrayList<SendJob>()
            val sources = HashMap<Int, LocalItem>()
            val taken = HashSet<String>()
            val folders = LinkedHashSet<String>()
            var id = firstId
            for (it in items) {
                val local = it.parts.joinToString("/")
                val (dest, parts, route) = place(it)
                val jobId = id++
                if (route == Route.BYOS_UPDATE) {
                    jobs += SendJob(jobId, local, "", it.size, JobStatus.UPDATE_FILE, route = route)
                    continue
                }
                val clean = parts.map { p -> SyncPaths.cleanName(p) }
                if (clean.any { c -> c == null }) {
                    jobs += SendJob(jobId, local, "", it.size, JobStatus.BAD_NAME, route = route)
                    continue
                }
                // the automatic groups make their own top folders; a chosen destination is made by the transfer
                if (route != null && dest.isNotEmpty()) folders += dest
                var folder = dest
                for (c in clean.dropLast(1)) {
                    folder = SyncPaths.join(folder, c!!)
                    if (!taken.contains(folder.lowercase())) folders += folder
                }
                var remote = SyncPaths.join(folder, unique(clean.last()!!, folder, taken))
                if (!SyncPaths.valid(remote)) {
                    jobs += SendJob(jobId, local, remote, it.size, JobStatus.BAD_NAME, route = route)
                    continue
                }
                taken += remote.lowercase()
                if (it.size > maxFile) {
                    jobs += SendJob(jobId, local, remote, it.size, JobStatus.TOO_BIG, route = route)
                    continue
                }
                remote = SyncPaths.nfc(remote)
                jobs += SendJob(jobId, local, remote, it.size, route = route)
                sources[jobId] = it
            }
            // a parent before its children
            return SendPlan(jobs, folders.filter { SyncPaths.valid(it) }.sortedBy { it.count { c -> c == '/' } }, sources)
        }

        /** FAT compares names without case: "a.mp3" and "A.MP3" are the same file. */
        private fun unique(name: String, folder: String, taken: Set<String>): String {
            if (!taken.contains(SyncPaths.join(folder, name).lowercase())) return name
            val dot = name.lastIndexOf('.')
            val stem = if (dot > 0) name.substring(0, dot) else name
            val ext = if (dot > 0) name.substring(dot) else ""
            var n = 2
            while (true) {
                val c = "$stem ($n)$ext"
                if (!taken.contains(SyncPaths.join(folder, c).lowercase())) return c
                n++
            }
        }
    }
}
