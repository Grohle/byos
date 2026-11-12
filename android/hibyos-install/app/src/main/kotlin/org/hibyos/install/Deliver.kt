package org.hibyos.install

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import org.hibyos.install.core.Discovery
import org.hibyos.install.core.SendItem
import org.hibyos.install.core.btp.BtpDiscovery
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.Socket
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class StoppedException : Exception("cancelled")

fun hex(b: ByteArray): String {
    val sb = StringBuilder(b.size * 2)
    for (x in b) {
        sb.append("0123456789abcdef"[(x.toInt() shr 4) and 15]).append("0123456789abcdef"[x.toInt() and 15])
    }
    return sb.toString()
}

/** The phone's side of the local network: where to look for the player and how to reach it. */
object Net {
    private fun wifi(context: Context): Pair<ConnectivityManager, Network>? {
        val cm = context.getSystemService(ConnectivityManager::class.java) ?: return null
        @Suppress("DEPRECATION")
        val n = cm.allNetworks.firstOrNull { cm.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true }
        return if (n == null) null else cm to n
    }

    /**
     * Sockets for talking to the player. On Wi-Fi they are tied to the Wi-Fi network, so that a
     * phone that prefers mobile data still reaches the player; otherwise (a phone sharing its
     * own hotspot, for example) the system chooses the route.
     */
    fun socketFactory(context: Context): () -> Socket {
        val n = wifi(context)?.second ?: return { Socket() }
        return { n.socketFactory.createSocket() }
    }

    /**
     * UDP sockets for discovery, tied to the Wi-Fi network like [socketFactory]: without that,
     * a phone that prefers mobile data (or a Wi-Fi without internet) may send the probe out of
     * the wrong interface, and no player ever answers.
     */
    fun datagramFactory(context: Context): () -> DatagramSocket {
        val n = wifi(context)?.second ?: return { DatagramSocket() }
        return {
            val s = DatagramSocket()
            try {
                n.bindSocket(s)
            } catch (e: Exception) {
                // the network went away meanwhile: the system chooses the route
            }
            s
        }
    }

    /**
     * Where the discovery probe goes: the directed broadcast address of the Wi-Fi link (from the
     * system, which is reliable on every Android version), the ones Java sees on the interfaces,
     * and the limited broadcast.
     */
    fun broadcasts(context: Context): List<InetAddress> {
        val out = LinkedHashSet<InetAddress>()
        wifi(context)?.let { (cm, n) ->
            cm.getLinkProperties(n)?.linkAddresses?.forEach { la ->
                val a = la.address
                if (a is Inet4Address && !a.isLoopbackAddress) BtpDiscovery.subnetBroadcast(a.address, la.prefixLength)?.let { out += it }
            }
        }
        out += BtpDiscovery.broadcastAddresses()
        return out.toList()
    }

    /** The addresses of the phone's own local network to ask in "Find my player". */
    fun neighbours(context: Context): List<String> {
        val out = LinkedHashSet<String>()
        wifi(context)?.let { (cm, n) ->
            cm.getLinkProperties(n)?.linkAddresses?.forEach { la ->
                val a = la.address
                if (a is Inet4Address && !a.isLoopbackAddress) out += Discovery.candidates(a.address, la.prefixLength)
            }
        }
        if (out.isEmpty()) {
            // not a Wi-Fi client: the phone may be the hotspot the player is connected to
            try {
                for (ni in NetworkInterface.getNetworkInterfaces()) {
                    if (!ni.isUp || ni.isLoopback || ni.isPointToPoint) continue
                    val name = ni.name.lowercase()
                    if (!(name.startsWith("wlan") || name.startsWith("ap") || name.startsWith("swlan") || name.startsWith("softap"))) continue
                    for (ia in ni.interfaceAddresses) {
                        val a = ia.address
                        if (a is Inet4Address && a.isSiteLocalAddress) {
                            out += Discovery.candidates(a.address, ia.networkPrefixLength.toInt())
                        }
                    }
                }
            } catch (e: Exception) {
                // no list of interfaces: nothing to look at
            }
        }
        return out.toList()
    }
}

/** The manual way: the same files written to a folder chosen by the user, or packed for sharing. */
object Deliver {
    private const val MIME = "application/octet-stream"

    /**
     * Writes [items] under the folder [tree] (keeping their folders), replacing what is there
     * with the same names, and reads every file back to compare it. Returns the folder's name.
     */
    fun saveTree(context: Context, tree: Uri, items: List<SendItem>, cancelled: () -> Boolean,
                 progress: (Long, Long, String) -> Unit): String {
        val cr = context.contentResolver
        val root = DocumentFile.fromTreeUri(context, tree) ?: throw IOException("the folder cannot be opened")
        if (!root.isDirectory || !root.canWrite()) throw IOException("the folder cannot be written to")
        val dirs = HashMap<String, DocumentFile>()
        dirs[""] = root
        val listings = HashMap<String, HashMap<String, DocumentFile>>()

        fun children(key: String, dir: DocumentFile): HashMap<String, DocumentFile> = listings.getOrPut(key) {
            val m = HashMap<String, DocumentFile>()
            for (f in dir.listFiles()) f.name?.let { m[it] = f }
            m
        }

        fun dirFor(path: String): DocumentFile {
            dirs[path]?.let { return it }
            val parentPath = path.substringBeforeLast('/', "")
            val name = path.substringAfterLast('/')
            val parent = dirFor(parentPath)
            val kids = children(parentPath, parent)
            var d = kids[name]
            if (d != null && !d.isDirectory) {
                if (!d.delete()) throw IOException("a file called $name is in the way and cannot be removed")
                d = null
            }
            if (d == null) {
                d = parent.createDirectory(name) ?: throw IOException("the folder $path could not be created")
                if (d.name != name) {
                    d.delete()
                    throw IOException("the folder $path could not be created with that name")
                }
                kids[name] = d
            }
            dirs[path] = d
            return d
        }

        val total = items.sumOf { it.size } * 2
        var done = 0L
        val buf = ByteArray(1 shl 16)
        for (item in items) {
            if (cancelled()) throw StoppedException()
            val dirPath = item.path.substringBeforeLast('/', "")
            val name = item.path.substringAfterLast('/')
            val dir = dirFor(dirPath)
            val kids = children(dirPath, dir)
            kids.remove(name)?.let { if (!it.delete()) throw IOException("${item.path} is already there and cannot be replaced") }
            val f = dir.createFile(MIME, name) ?: throw IOException("${item.path} could not be created")
            if (f.name != name) {
                f.delete()
                throw IOException("${item.path} could not be created with that name")
            }
            kids[name] = f
            progress(done, total, item.path)
            val sent = MessageDigest.getInstance("SHA-256")
            var n = 0L
            (cr.openOutputStream(f.uri, "w") ?: throw IOException("${item.path} could not be written")).use { out ->
                item.open().use { input ->
                    while (true) {
                        if (cancelled()) throw StoppedException()
                        val r = input.read(buf)
                        if (r < 0) break
                        out.write(buf, 0, r)
                        sent.update(buf, 0, r)
                        n += r
                        done += r
                        progress(done, total, item.path)
                    }
                }
                out.flush()
            }
            if (n != item.size) throw IOException("${item.path}: $n of ${item.size} bytes could be read on this phone")
            // read it back: a card that is failing, or a full one, shows here and not in the player
            val back = MessageDigest.getInstance("SHA-256")
            var m = 0L
            (cr.openInputStream(f.uri) ?: throw IOException("${item.path} could not be read back")).use { input ->
                while (true) {
                    if (cancelled()) throw StoppedException()
                    val r = input.read(buf)
                    if (r < 0) break
                    back.update(buf, 0, r)
                    m += r
                    done += r
                    progress(minOf(done, total), total, item.path)
                }
            }
            val got = hex(back.digest())
            if (m != n || got != hex(sent.digest()) || (item.sha256 != null && got != item.sha256)) {
                throw IOException("${item.path} was not stored intact ($m of $n bytes read back)")
            }
            done = done - m + n
        }
        progress(total, total, "")
        return root.name ?: "?"
    }

    /** Packs [items] into a zip (for "Share": one file instead of a folder with many). */
    fun zip(items: List<SendItem>, dest: File, cancelled: () -> Boolean, progress: (Long, Long) -> Unit) {
        dest.parentFile?.mkdirs()
        val part = File(dest.path + ".part")
        val total = items.sumOf { it.size }.coerceAtLeast(1)
        var done = 0L
        val buf = ByteArray(1 shl 16)
        try {
            ZipOutputStream(FileOutputStream(part).buffered(1 shl 16)).use { z ->
                for (item in items) {
                    z.putNextEntry(ZipEntry(item.path))
                    item.open().use { input ->
                        while (true) {
                            if (cancelled()) throw StoppedException()
                            val r = input.read(buf)
                            if (r < 0) break
                            z.write(buf, 0, r)
                            done += r
                            progress(done, total)
                        }
                    }
                    z.closeEntry()
                }
            }
            dest.delete()
            if (!part.renameTo(dest)) throw IOException("the zip could not be written")
        } finally {
            part.delete()
        }
    }
}
