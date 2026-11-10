package org.hibyos.install.core

import java.io.EOFException
import java.util.Locale

/**
 * Helpers for the contents of a HiBy .upt firmware container: the image list
 * (ota_v0/ota_update.in) and the images cut in chunks whose names chain their MD5 sums.
 * Same rules as tools/build.py (parse_update_in, extract_image, chunk_image).
 */
class UptException(message: String) : Exception(message)

/** Several sources seen as one (the chunks of an image, in order). */
class ConcatSource(private val parts: List<ByteSource>) : ByteSource {
    private val starts = LongArray(parts.size + 1)

    init {
        for (i in parts.indices) starts[i + 1] = starts[i] + parts[i].size
    }

    override val size: Long get() = starts[parts.size]

    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int) {
        if (pos < 0 || len < 0 || pos + len > size) throw EOFException("read past the end of the image")
        if (len == 0) return
        // last part whose start is <= pos
        var lo = 0
        var hi = parts.size - 1
        while (lo < hi) {
            val mid = (lo + hi + 1) ushr 1
            if (starts[mid] <= pos) lo = mid else hi = mid - 1
        }
        var i = lo
        var p = pos
        var o = off
        var left = len
        while (left > 0) {
            val inPart = p - starts[i]
            val n = minOf(left.toLong(), parts[i].size - inPart).toInt()
            if (n > 0) {
                parts[i].read(inPart, dst, o, n)
                p += n
                o += n
                left -= n
            }
            i++
        }
    }
}

/** One image described in ota_update.in (img_type, img_name, img_size, img_md5...). */
class UptImage(val fields: Map<String, String>) {
    val type: String get() = fields["img_type"] ?: ""
    val name: String get() = fields["img_name"] ?: throw UptException("the image list has an image without a name")
    val md5: String get() = fields["img_md5"] ?: throw UptException("the image list has an image without md5")
    val size: Long
        get() = fields["img_size"]?.trim()?.toLongOrNull() ?: throw UptException("the image list has an image without size")
}

/** Chunks and manifest of an image, ready to be stored in a container. */
class ChunkedImage(val wholeMd5: String, val files: List<IsoInput>)

object Upt {
    const val UPDATE_IN = "ota_v0/ota_update.in"

    fun parseUpdateIn(text: String): List<UptImage> {
        val imgs = ArrayList<LinkedHashMap<String, String>>()
        var cur: LinkedHashMap<String, String>? = null
        for (raw in text.split("\r\n", "\n", "\r")) {
            val line = raw.trim()
            val eq = line.indexOf('=')
            if (eq < 0) continue
            val k = line.substring(0, eq)
            val v = line.substring(eq + 1)
            if (k == "img_type") {
                cur = LinkedHashMap()
                cur["img_type"] = v
                imgs.add(cur)
            } else if (cur != null) {
                cur[k] = v
            }
        }
        return imgs.map { UptImage(it) }
    }

    private fun entry(files: Map<String, IsoEntry>, path: String): IsoEntry =
        files[path] ?: throw UptException("the firmware container has no $path")

    /**
     * Checks the MD5 chain of every chunk of the image [name] (reading them once) and returns
     * the image as one source over the chunks, without copying anything.
     * [onBytes] is called with the number of bytes just checked.
     */
    fun openImage(src: ByteSource, files: Map<String, IsoEntry>, name: String, md5sum: String,
                  onBytes: ((Int) -> Unit)? = null): ConcatSource {
        val me = entry(files, "ota_v0/ota_md5_$name.$md5sum")
        if (me.size > 4 shl 20) throw UptException("the chunk list of $name is too big")
        val manifest = String(src.bytes(me.offset, me.size.toInt()), Charsets.UTF_8)
        val hashes = manifest.split(Regex("\\s+")).filter { it.isNotEmpty() }
        val parts = ArrayList<ByteSource>(hashes.size)
        val buf = ByteArray(1 shl 16)
        val whole = md5()
        var prev = md5sum
        for ((i, h) in hashes.withIndex()) {
            val e = entry(files, "ota_v0/$name.${String.format(Locale.ROOT, "%04d", i)}.$prev")
            val md = md5()
            var pos = 0L
            while (pos < e.size) {
                val n = minOf(e.size - pos, buf.size.toLong()).toInt()
                src.read(e.offset + pos, buf, 0, n)
                md.update(buf, 0, n)
                whole.update(buf, 0, n)
                pos += n
                onBytes?.invoke(n)
            }
            if (Hex.of(md.digest()) != h) throw UptException("chunk $i of $name is damaged (md5 mismatch)")
            parts.add(SliceSource(src, e.offset, e.size))
            prev = h
        }
        val image = ConcatSource(parts)
        if (Hex.of(whole.digest()) != md5sum) throw UptException("the image $name is damaged (md5 mismatch)")
        return image
    }

    /** Cuts [data] like the official firmware: chunk names carry the MD5 of the previous chunk. */
    fun chunkImage(name: String, data: ByteSource, chunk: Int, onBytes: ((Int) -> Unit)? = null): ChunkedImage {
        val size = data.size
        val n = ((size + chunk - 1) / chunk).toInt()
        val hashes = ArrayList<String>(n)
        val whole = md5()
        val buf = ByteArray(1 shl 16)
        for (i in 0 until n) {
            val start = i.toLong() * chunk
            val len = minOf(chunk.toLong(), size - start)
            val md = md5()
            var pos = 0L
            while (pos < len) {
                val k = minOf(len - pos, buf.size.toLong()).toInt()
                data.read(start + pos, buf, 0, k)
                md.update(buf, 0, k)
                whole.update(buf, 0, k)
                pos += k
                onBytes?.invoke(k)
            }
            hashes.add(Hex.of(md.digest()))
        }
        val wholeMd5 = Hex.of(whole.digest())
        val out = ArrayList<IsoInput>(n + 1)
        var prev = wholeMd5
        for (i in 0 until n) {
            val start = i.toLong() * chunk
            out.add(IsoInput("ota_v0/$name.${String.format(Locale.ROOT, "%04d", i)}.$prev",
                SliceSource(data, start, minOf(chunk.toLong(), size - start))))
            prev = hashes[i]
        }
        val manifest = (hashes.joinToString("\n") + "\n").toByteArray(Charsets.US_ASCII)
        out.add(IsoInput("ota_v0/ota_md5_$name.$wholeMd5", ArraySource(manifest)))
        return ChunkedImage(wholeMd5, out)
    }
}
