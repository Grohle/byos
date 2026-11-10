package org.hibyos.install.core

import java.io.InputStream

/** One file to put on the player's SD card. [path] is relative to the card ("HiByOS/bin/x"). */
class SendItem(val path: String, val size: Long, val sha256: String?, val open: () -> InputStream)

class SendResult(
    val files: Int, val bytes: Long,
    /** Files that were already on the card with the same contents. */
    val skipped: Int,
    /** Files read back from the player and compared. */
    val verified: Int,
    /** Why the rest could only be checked by size, or null when everything was read back. */
    val readBackProblem: String?,
)

fun interface SendListener {
    /** [done] of [total] work units (bytes sent plus bytes read back); [path] is the file in hand. */
    fun onProgress(done: Long, total: Long, path: String, phase: Phase)

    enum class Phase { PREPARE, SEND, CHECK }
}

/**
 * Puts a set of files on the player's SD card through its Wi-Fi transfer and makes sure they
 * arrived: folders are created first, a file that is already there with the same contents is
 * left alone (so trying again after a failure continues where it stopped), another file with
 * the same name is replaced, and after sending the size is checked in the player's listing
 * and the file is read back and compared with its SHA-256.
 */
object Sender {
    fun send(client: PlayerClient, items: List<SendItem>, verify: Boolean = true,
             cancelled: () -> Boolean = { false }, listener: SendListener? = null): SendResult {
        val home = client.home
        val total = items.sumOf { it.size } * (if (verify) 2 else 1)
        var done = 0L
        var skipped = 0
        var verified = 0
        var readBack = verify
        var readBackProblem: String? = null
        val listings = HashMap<String, MutableMap<String, RemoteEntry>>()

        fun check() {
            if (cancelled()) throw TransferException(TransferException.Kind.CANCELLED, "cancelled")
        }

        fun listing(dir: String, fresh: Boolean = false): MutableMap<String, RemoteEntry> {
            if (fresh) listings.remove(dir)
            return listings.getOrPut(dir) {
                val m = HashMap<String, RemoteEntry>()
                for (e in client.list(dir)) m[e.name] = e
                m
            }
        }

        fun ensureDir(dir: String) {
            if (dir == home || listings.containsKey(dir)) return
            val parent = dir.trimEnd('/').substringBeforeLast('/') + "/"
            val name = dir.trimEnd('/').substringAfterLast('/')
            ensureDir(parent)
            val there = listing(parent)[name]
            if (there != null && !there.isDir) {
                throw TransferException(TransferException.Kind.REFUSED,
                    "a file named \"$name\" is where the folder \"$name\" must go; remove it from the card and try again")
            }
            if (there == null) {
                client.createDir(dir)
                listing(parent)[name] = RemoteEntry(name, dir, null)
                listings[dir] = HashMap()
            }
        }

        /** True when the file on the player has exactly the wanted contents; null when it cannot be read back. */
        fun sameOnPlayer(item: SendItem, remote: String, count: Boolean): Boolean? {
            if (!readBack || item.sha256 == null) return null
            val md = sha256()
            val start = done
            return try {
                val n = client.digest(remote, md, cancelled) {
                    if (count) {
                        done += it
                        listener?.onProgress(minOf(done, total), total, item.path, SendListener.Phase.CHECK)
                    }
                }
                n == item.size && Hex.of(md.digest()) == item.sha256
            } catch (e: TransferException) {
                done = start
                if (e.kind == TransferException.Kind.REFUSED || e.kind == TransferException.Kind.NOT_A_PLAYER) {
                    // this player does not give files back: sizes only from here on
                    readBack = false
                    readBackProblem = e.message
                    null
                } else {
                    throw e
                }
            }
        }

        try {
            client.list(home).let { l -> listings[home] = HashMap<String, RemoteEntry>().also { m -> l.forEach { e -> m[e.name] = e } } }
        } catch (e: TransferException) {
            if (e.kind == TransferException.Kind.REFUSED) {
                throw TransferException(TransferException.Kind.NO_SD_CARD,
                    "the player cannot open its SD card" + (e.serverText?.let { " ($it)" } ?: ""), e.status, e.serverText)
            }
            throw e
        }

        for (item in items) {
            check()
            try {
                Bundle.safePath(item.path)
            } catch (e: BundleException) {
                throw TransferException(TransferException.Kind.LOCAL, "bad file name: ${item.path}")
            }
            val remote = home + item.path
            val dir = remote.substringBeforeLast('/') + "/"
            val name = remote.substringAfterLast('/')
            listener?.onProgress(done, total, item.path, SendListener.Phase.PREPARE)
            ensureDir(dir)
            val there = listing(dir)[name]
            if (there != null && there.isDir) {
                throw TransferException(TransferException.Kind.REFUSED,
                    "a folder named \"$name\" is where the file \"$name\" must go; remove it from the card and try again")
            }
            val before = done
            if (there != null && there.size == item.size && sameOnPlayer(item, remote, true) == true) {
                done = before + item.size * (if (verify) 2 else 1)
                skipped++
                verified++
                listener?.onProgress(done, total, item.path, SendListener.Phase.CHECK)
                continue
            }
            done = before
            if (there != null) {
                client.delete(remote)
                listing(dir).remove(name)
            }
            check()
            client.upload(dir, name, item.size, item.open, cancelled) {
                listener?.onProgress(before + it, total, item.path, SendListener.Phase.SEND)
            }
            done = before + item.size
            // the player's own listing must show the whole file
            val now = listing(dir, fresh = true)[name]
            if (now == null || now.isDir) {
                throw TransferException(TransferException.Kind.MISMATCH, "the player did not keep \"${item.path}\"")
            }
            if (now.size != item.size) {
                throw TransferException(TransferException.Kind.MISMATCH,
                    "the player stored ${now.size} of ${item.size} bytes of \"${item.path}\"")
            }
            when (sameOnPlayer(item, remote, true)) {
                true -> verified++
                false -> throw TransferException(TransferException.Kind.MISMATCH,
                    "\"${item.path}\" did not arrive intact (what the player gives back is different)")
                null -> {}
            }
            done = before + item.size * (if (verify) 2 else 1)
            listener?.onProgress(done, total, item.path, SendListener.Phase.CHECK)
        }
        return SendResult(items.size, items.sumOf { it.size }, skipped, verified, readBackProblem)
    }
}
