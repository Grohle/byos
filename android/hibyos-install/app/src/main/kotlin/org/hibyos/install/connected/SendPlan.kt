package org.hibyos.install.connected

import java.io.InputStream

/** A file chosen on the phone. [parts] is its path inside what was picked: a folder keeps its tree. */
class LocalItem(
    val parts: List<String>,
    val size: Long,
    val mtime: Long,
    val open: () -> InputStream,
)

enum class JobStatus { WAITING, HASHING, SENDING, SENT, SKIPPED, EXISTS, TOO_BIG, BAD_NAME, FAILED, CANCELLED }

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
) {
    val finished: Boolean get() = status == JobStatus.SENT || status == JobStatus.SKIPPED
    val retryable: Boolean get() = status == JobStatus.WAITING || status == JobStatus.FAILED || status == JobStatus.CANCELLED ||
        status == JobStatus.HASHING || status == JobStatus.SENDING
}

/** The jobs of a transfer and the folders to create first. */
class SendPlan(val jobs: List<SendJob>, val folders: List<String>, val sources: Map<Int, LocalItem>) {
    companion object {
        /**
         * Places [items] under [dest] (a card path, "" = the top of the card). Names are cleaned
         * for the card (SyncPaths.cleanName); two files that would end with the same name get
         * " (2)", " (3)"...; hidden files and names that cannot be made valid are listed as
         * BAD_NAME and not sent; files over [maxFile] are TOO_BIG.
         */
        fun build(items: List<LocalItem>, dest: String, maxFile: Long = Long.MAX_VALUE, firstId: Int = 1): SendPlan {
            val jobs = ArrayList<SendJob>()
            val sources = HashMap<Int, LocalItem>()
            val taken = HashSet<String>()
            val folders = LinkedHashSet<String>()
            var id = firstId
            for (it in items) {
                val local = it.parts.joinToString("/")
                val clean = it.parts.map { p -> SyncPaths.cleanName(p) }
                val jobId = id++
                if (clean.any { c -> c == null }) {
                    jobs += SendJob(jobId, local, "", it.size, JobStatus.BAD_NAME)
                    continue
                }
                var folder = dest
                for (c in clean.dropLast(1)) {
                    folder = SyncPaths.join(folder, c!!)
                    if (!taken.contains(folder.lowercase())) folders += folder
                }
                var remote = SyncPaths.join(folder, unique(clean.last()!!, folder, taken))
                if (!SyncPaths.valid(remote)) {
                    jobs += SendJob(jobId, local, remote, it.size, JobStatus.BAD_NAME)
                    continue
                }
                taken += remote.lowercase()
                if (it.size > maxFile) {
                    jobs += SendJob(jobId, local, remote, it.size, JobStatus.TOO_BIG)
                    continue
                }
                remote = SyncPaths.nfc(remote)
                jobs += SendJob(jobId, local, remote, it.size)
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
