package org.hibyos.cli

import java.awt.image.BufferedImage
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import javax.imageio.IIOImage
import javax.imageio.ImageIO
import javax.imageio.ImageWriteParam
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Album art prepared on the computer, so the HiBy never has to decode a large image
 * (JDK image I/O only, no dependency).
 *
 * The player's cover cache decodes the embedded picture inside a 10 MB budget and scales it
 * with libjpeg's DCT scaling (1/2, 1/4, 1/8): a 3000 px or progressive JPEG can fail there and
 * the player then falls back to its blurry 128 px copy. A [DEVICE_SIZE] px baseline JPEG
 * decodes at 1/2 scale to 250 px, sharp at the 232 px of Now Playing. The idea of resizing
 * art on the PC for the HiBy (and putting transparent images on white) comes from the
 * hiby-mods database updater (MIT, Copyright (c) 2026 noisetta and contributors, see
 * THIRD_PARTY_NOTICES.md); this is a new implementation.
 *
 * It lives in the CLI, not in the core: Android has no java.awt / javax.imageio.
 */
object CoverArt {
    /** Embedded art written for the HiBy. */
    const val DEVICE_SIZE = 500
    /** Thumbnails for the desktop app's library view. */
    const val THUMB_SIZE = 256
    const val QUALITY = 0.88f
    /** The device's own rule (src/nowplaying.inc.c np_img_usable): smaller or longer images are no cover. */
    const val MIN_SIDE = 120
    const val MAX_ASPECT = 1.6
    /** Embedded art above this is worth replacing (art-fix). */
    const val FIX_ABOVE = 1000
    /** Input images above this many pixels are refused (a broken or hostile header). */
    private const val MAX_PIXELS = 40_000_000L

    class Info(val width: Int, val height: Int, val format: String, val progressive: Boolean)

    class Result(val bytes: ByteArray, val width: Int, val height: Int)

    /** Size and kind of a JPEG (SOFn) or PNG (IHDR) from its first bytes, or null. */
    fun info(b: ByteArray): Info? {
        if (b.size >= 24 && b[0] == 0x89.toByte() && b[1] == 'P'.code.toByte() && b[2] == 'N'.code.toByte() && b[3] == 'G'.code.toByte()) {
            val w = be32(b, 16); val h = be32(b, 20)
            return if (w > 0 && h > 0) Info(w, h, "png", false) else null
        }
        if (b.size < 4 || b[0] != 0xff.toByte() || b[1] != 0xd8.toByte()) return null
        var p = 2
        while (p + 4 <= b.size) {
            if (b[p] != 0xff.toByte()) { p++; continue }
            val m = b[p + 1].toInt() and 0xff
            if (m == 0xff) { p++; continue }
            if (m == 0xd8 || m in 0xd0..0xd7 || m == 0x01) { p += 2; continue }
            val len = ((b[p + 2].toInt() and 0xff) shl 8) or (b[p + 3].toInt() and 0xff)
            val sof = m in 0xc0..0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc
            if (sof) {
                if (p + 9 > b.size) return null
                val h = ((b[p + 5].toInt() and 0xff) shl 8) or (b[p + 6].toInt() and 0xff)
                val w = ((b[p + 7].toInt() and 0xff) shl 8) or (b[p + 8].toInt() and 0xff)
                if (w <= 0 || h <= 0) return null
                return Info(w, h, "jpeg", m == 0xc2 || m == 0xc6 || m == 0xca || m == 0xce)
            }
            if (m == 0xda || len < 2) return null
            p += 2 + len
        }
        return null
    }

    /** True when the HiBy shows this image as a cover (size and shape rule of the device). */
    fun usable(i: Info): Boolean {
        val lo = min(i.width, i.height); val hi = max(i.width, i.height)
        return lo >= MIN_SIDE && hi.toDouble() / lo <= MAX_ASPECT
    }

    /** True when an embedded picture is worth replacing with a [DEVICE_SIZE] px baseline JPEG. */
    fun needsFix(i: Info): Boolean = usable(i) && (i.progressive || i.format != "jpeg" || max(i.width, i.height) > FIX_ABOVE)

    /**
     * Decodes [src] (JPEG or PNG), puts transparency on white, scales it so its longer side
     * is at most [size] px (never larger than it was) with a Lanczos-3 filter and writes a
     * baseline JPEG. Null when the image cannot be decoded or would not be shown as a cover.
     */
    fun prepare(src: ByteArray, size: Int = DEVICE_SIZE, quality: Float = QUALITY): Result? {
        val i = info(src) ?: return null
        if (!usable(i) || i.width.toLong() * i.height > MAX_PIXELS) return null
        val img = try { ImageIO.read(ByteArrayInputStream(src)) } catch (e: Exception) { null } ?: return null
        val flat = flatten(img)
        val scale = min(1.0, size.toDouble() / max(flat.width, flat.height))
        val w = max(1, (flat.width * scale).roundToInt())
        val h = max(1, (flat.height * scale).roundToInt())
        val out = if (w == flat.width && h == flat.height) flat else resize(flat, w, h)
        return Result(jpeg(out, quality), w, h)
    }

    /** An RGB copy with any transparency composed on white. */
    fun flatten(img: BufferedImage): BufferedImage {
        val w = img.width; val h = img.height
        val out = BufferedImage(w, h, BufferedImage.TYPE_INT_RGB)
        val row = IntArray(w)
        for (y in 0 until h) {
            img.getRGB(0, y, w, 1, row, 0, w)
            for (x in 0 until w) {
                val c = row[x]
                val a = (c ushr 24) and 0xff
                if (a != 0xff) {
                    fun ch(v: Int) = (v * a + 255 * (255 - a) + 127) / 255
                    row[x] = (ch((c shr 16) and 0xff) shl 16) or (ch((c shr 8) and 0xff) shl 8) or ch(c and 0xff)
                } else row[x] = c and 0xffffff
            }
            out.setRGB(0, y, w, 1, row, 0, w)
        }
        return out
    }

    /** Baseline (not progressive) JPEG at [quality]. */
    fun jpeg(img: BufferedImage, quality: Float = QUALITY): ByteArray {
        val writer = ImageIO.getImageWritersByFormatName("jpeg").next()
        val bout = ByteArrayOutputStream()
        try {
            ImageIO.createImageOutputStream(bout).use { ios ->
                writer.output = ios
                val p = writer.defaultWriteParam
                p.compressionMode = ImageWriteParam.MODE_EXPLICIT
                p.compressionQuality = quality
                p.progressiveMode = ImageWriteParam.MODE_DISABLED
                writer.write(null, IIOImage(img, null, null), p)
            }
        } finally {
            writer.dispose()
        }
        return bout.toByteArray()
    }

    // ------------------------------------------------------------------ Lanczos-3

    private fun lanczos(x: Double): Double {
        if (x == 0.0) return 1.0
        if (abs(x) >= 3.0) return 0.0
        val px = PI * x
        return 3.0 * sin(px) * sin(px / 3.0) / (px * px)
    }

    /** Per output pixel: the first source index and the normalised weights. */
    private class Taps(val start: IntArray, val weights: Array<DoubleArray>)

    private fun taps(src: Int, dst: Int): Taps {
        val scale = dst.toDouble() / src
        val support = if (scale < 1) 3.0 / scale else 3.0
        val stretch = if (scale < 1) scale else 1.0
        val start = IntArray(dst)
        val weights = Array(dst) { DoubleArray(0) }
        for (i in 0 until dst) {
            val center = (i + 0.5) / scale - 0.5
            val lo = max(0, floor(center - support).toInt() + 1)
            val hi = min(src - 1, ceil(center + support).toInt() - 1)
            val n = max(1, hi - lo + 1)
            val w = DoubleArray(n)
            var sum = 0.0
            for (k in 0 until n) { w[k] = lanczos((lo + k - center) * stretch); sum += w[k] }
            if (sum != 0.0) for (k in 0 until n) w[k] /= sum else w[0] = 1.0
            start[i] = min(lo, src - 1); weights[i] = w
        }
        return Taps(start, weights)
    }

    /** Separable Lanczos-3 resampling of an RGB image (horizontal pass, then vertical). */
    fun resize(img: BufferedImage, w: Int, h: Int): BufferedImage {
        val sw = img.width; val sh = img.height
        val src = IntArray(sw * sh).also { img.getRGB(0, 0, sw, sh, it, 0, sw) }
        val tx = taps(sw, w); val ty = taps(sh, h)
        // horizontal: sh rows x w columns, 3 channels as floats
        val mid = FloatArray(sh * w * 3)
        for (y in 0 until sh) {
            val r = y * sw
            for (x in 0 until w) {
                var a = 0.0; var g = 0.0; var b = 0.0
                val s = tx.start[x]; val ws = tx.weights[x]
                for (k in ws.indices) {
                    val c = src[r + min(sw - 1, s + k)]
                    val wk = ws[k]
                    a += wk * ((c shr 16) and 0xff); g += wk * ((c shr 8) and 0xff); b += wk * (c and 0xff)
                }
                val o = (y * w + x) * 3
                mid[o] = a.toFloat(); mid[o + 1] = g.toFloat(); mid[o + 2] = b.toFloat()
            }
        }
        val out = IntArray(w * h)
        for (y in 0 until h) {
            val s = ty.start[y]; val ws = ty.weights[y]
            for (x in 0 until w) {
                var a = 0.0; var g = 0.0; var b = 0.0
                for (k in ws.indices) {
                    val o = (min(sh - 1, s + k) * w + x) * 3
                    val wk = ws[k]
                    a += wk * mid[o]; g += wk * mid[o + 1]; b += wk * mid[o + 2]
                }
                fun c(v: Double) = v.roundToInt().coerceIn(0, 255)
                out[y * w + x] = (c(a) shl 16) or (c(g) shl 8) or c(b)
            }
        }
        return BufferedImage(w, h, BufferedImage.TYPE_INT_RGB).also { it.setRGB(0, 0, w, h, out, 0, w) }
    }

    /** The average colour of an image as "#rrggbb" (the desktop tints an open album with it). */
    fun averageColor(jpeg: ByteArray): String? {
        val img = try { ImageIO.read(ByteArrayInputStream(jpeg)) } catch (e: Exception) { null } ?: return null
        var r = 0L; var g = 0L; var b = 0L; var n = 0L
        val step = max(1, min(img.width, img.height) / 32)
        var y = 0
        while (y < img.height) {
            var x = 0
            while (x < img.width) {
                val c = img.getRGB(x, y); r += (c shr 16) and 0xff; g += (c shr 8) and 0xff; b += c and 0xff; n++
                x += step
            }
            y += step
        }
        if (n == 0L) return null
        return "#%02x%02x%02x".format(r / n, g / n, b / n)
    }

    private fun be32(b: ByteArray, p: Int) = ((b[p].toInt() and 0xff) shl 24) or ((b[p + 1].toInt() and 0xff) shl 16) or
        ((b[p + 2].toInt() and 0xff) shl 8) or (b[p + 3].toInt() and 0xff)
}
