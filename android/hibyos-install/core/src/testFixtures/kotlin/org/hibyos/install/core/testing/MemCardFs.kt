package org.hibyos.install.core.testing

import org.hibyos.install.core.btp.Btp
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.BtpException
import org.hibyos.install.core.btp.DfR
import org.hibyos.install.core.btp.ListEntry
import org.hibyos.install.core.btp.StatR
import org.hibyos.install.core.btp.StatusR
import org.hibyos.install.core.sync.CardFs
import java.io.File
import java.util.TreeMap
import java.util.TreeSet

/**
 * A card in memory for the sync engine's tests: paths are exact strings (no case folding, no
 * Unicode normalisation, as vfat and exFAT under Linux). Files added with [addVirtual] have a
 * size but no bytes (reads give zeros), so 50,000-track cards stay small.
 */
class MemCardFs(var free: Long = 1L shl 40, var fsType: Int = 2) : CardFs {
    class F(var data: ByteArray?, var size: Long, var mtime: Long)

    val files = TreeMap<String, F>()
    val dirs = TreeSet<String>()
    /** Every operation, as "OP path" (PUT, PUTB, MOVE, DELETE, GET). */
    val log = ArrayList<String>()
    /** Called before each MOVE; may throw (or move and then throw, as a retried MOVE would). */
    var onMove: ((String, String) -> Unit)? = null
    /** Called before each PUT and PUTB of [path]; may throw. */
    var onPut: ((String) -> Unit)? = null

    private fun err(code: Int, m: String): Nothing = throw BtpException(code, m, remote = true)
    private fun parent(p: String) = p.substringBeforeLast('/', "")

    private fun children(dir: String): Pair<Collection<String>, Collection<String>> {
        val pre = if (dir.isEmpty()) "" else "$dir/"
        val fs = files.subMap(pre, pre + "￿").keys.filter { it.length > pre.length && it.indexOf('/', pre.length) < 0 }
        val ds = dirs.subSet(pre, pre + "￿").filter { it.length > pre.length && it.indexOf('/', pre.length) < 0 }
        return fs to ds
    }

    fun addVirtual(path: String, size: Long, mtime: Long) { mkdirs(parent(path)); files[path] = F(null, size, mtime) }

    fun write(path: String, data: ByteArray, mtime: Long = System.currentTimeMillis() / 1000) { mkdirs(parent(path)); files[path] = F(data, data.size.toLong(), mtime) }

    fun bytes(path: String): ByteArray? = files[path]?.let { it.data ?: ByteArray(it.size.toInt()) }

    override fun list(dir: String, hidden: Boolean): List<ListEntry>? {
        if (dir.isNotEmpty() && dir !in dirs) return null
        val (fs, ds) = children(dir)
        val pre = if (dir.isEmpty()) 0 else dir.length + 1
        return ds.map { ListEntry(1, 0, 0, it.substring(pre)) } + fs.map { val f = files[it]!!; ListEntry(0, f.size, f.mtime, it.substring(pre)) }
            .filter { hidden || !it.name.startsWith(".") }
    }

    override fun stat(path: String): StatR? = files[path]?.let { StatR(0, it.size, it.mtime) } ?: if (path in dirs || path.isEmpty()) StatR(1, 0, 0) else null

    override fun read(path: String, off: Long, len: Int): ByteArray {
        val f = files[path] ?: err(Btp.E_NOT_FOUND, path)
        val n = maxOf(0, minOf(len.toLong(), f.size - off)).toInt()
        val d = f.data ?: return ByteArray(n)
        return d.copyOfRange(off.toInt(), off.toInt() + n)
    }

    private fun store(path: String, data: ByteArray, sha: ByteArray, mode: Int, mtime: Long): Int {
        val cur = files[path]
        if (cur != null) {
            if (mode == Btp.MODE_CREATE) err(Btp.E_EXISTS, path)
            if (mode == Btp.MODE_SKIP_IDENTICAL) {
                if (cur.size == data.size.toLong() && cur.data != null && BtpCrypto.sha256(cur.data!!).contentEquals(sha)) return 1
                err(Btp.E_EXISTS, path)
            }
        }
        if (parent(path).isNotEmpty() && parent(path) !in dirs) err(Btp.E_NOT_FOUND, parent(path))
        if (data.size - (cur?.size ?: 0) > free) err(Btp.E_NO_SPACE, path)
        free -= data.size - (cur?.size ?: 0)
        files[path] = F(data, data.size.toLong(), mtime)
        return 0
    }

    override fun put(path: String, file: File, sha: ByteArray, mode: Int, mtime: Long, progress: (Long, Long) -> Unit): Int {
        onPut?.invoke(path)
        log += "PUT $path"
        val d = file.readBytes()
        progress(d.size.toLong(), d.size.toLong())
        return store(path, d, sha, mode, mtime)
    }

    override fun putBytes(path: String, data: ByteArray, mode: Int) {
        onPut?.invoke(path)
        log += "PUTB $path"
        store(path, data, BtpCrypto.sha256(data), mode, System.currentTimeMillis() / 1000)
    }

    override fun mkdirs(dir: String) {
        var d = dir
        while (d.isNotEmpty() && dirs.add(d)) d = parent(d)
    }

    override fun move(src: String, dst: String, replace: Boolean) {
        onMove?.invoke(src, dst)
        log += "MOVE $src -> $dst"
        if (files[dst] != null && !replace) err(Btp.E_EXISTS, dst)
        if (parent(dst).isNotEmpty() && parent(dst) !in dirs) err(Btp.E_NOT_FOUND, parent(dst))
        val f = files.remove(src)
        if (f != null) { files[dst] = f; return }
        if (src !in dirs) err(Btp.E_NOT_FOUND, src)
        val pre = "$src/"
        for (k in files.subMap(pre, pre + "￿").keys.toList()) files[dst + "/" + k.substring(pre.length)] = files.remove(k)!!
        for (k in dirs.subSet(pre, pre + "￿").toList()) { dirs.remove(k); dirs += dst + "/" + k.substring(pre.length) }
        dirs.remove(src); dirs += dst
    }

    override fun delete(path: String, recursive: Boolean) {
        log += "DELETE $path"
        val f = files.remove(path)
        if (f != null) { free += f.size; return }
        if (path !in dirs) err(Btp.E_NOT_FOUND, path)
        val pre = "$path/"
        val inside = files.subMap(pre, pre + "￿").keys.toList()
        val sub = dirs.subSet(pre, pre + "￿").toList()
        if ((inside.isNotEmpty() || sub.isNotEmpty()) && !recursive) err(Btp.E_NOT_EMPTY, path)
        for (k in inside) free += files.remove(k)!!.size
        dirs.removeAll(sub.toSet()); dirs.remove(path)
    }

    override fun df() = DfR(1, fsType, 1L shl 41, free, 1L shl 30, 1L shl 29)

    override fun status(): StatusR? = null

    override fun get(path: String, dst: File, progress: (Long, Long) -> Unit) {
        log += "GET $path"
        val d = bytes(path) ?: err(Btp.E_NOT_FOUND, path)
        dst.writeBytes(d)
        progress(d.size.toLong(), d.size.toLong())
    }
}
