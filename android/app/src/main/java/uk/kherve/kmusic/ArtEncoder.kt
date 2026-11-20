package uk.kherve.kmusic

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect

/** Turns album art into the watch's pixel format, and picks a background colour from it. */
object ArtEncoder {

    /** Square centre-crop scaled to [size] x [size]. */
    fun square(src: Bitmap, size: Int): Bitmap {
        val s = minOf(src.width, src.height)
        val from = Rect((src.width - s) / 2, (src.height - s) / 2, (src.width + s) / 2, (src.height + s) / 2)
        val out = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        Canvas(out).drawBitmap(src, from, Rect(0, 0, size, size), Paint(Paint.FILTER_BITMAP_FLAG))
        return out
    }

    /**
     * Pebble 8-bit colour (0b11rrggbb) for the screen background: the cover's most
     * common vivid hue, darkened so white text stays readable.
     */
    fun accent(bmp: Bitmap): Int {
        val small = Bitmap.createScaledBitmap(bmp, 24, 24, true)
        val buckets = HashMap<Int, FloatArray>()           // hue bucket -> [weight, r, g, b]
        val hsv = FloatArray(3)
        for (y in 0 until 24) for (x in 0 until 24) {
            val c = small.getPixel(x, y)
            Color.colorToHSV(c, hsv)
            val w = hsv[1] * (0.3f + hsv[2])                  // favour saturated, not-too-dark pixels
            val key = (hsv[0] / 30).toInt()
            val b = buckets.getOrPut(key) { FloatArray(4) }
            b[0] += w; b[1] += Color.red(c) * w; b[2] += Color.green(c) * w; b[3] += Color.blue(c) * w
        }
        val best = buckets.values.maxByOrNull { it[0] }
        var r = 60f; var g = 60f; var b = 80f
        if (best != null && best[0] > 1f) { r = best[1] / best[0]; g = best[2] / best[0]; b = best[3] / best[0] }
        Color.RGBToHSV(r.toInt(), g.toInt(), b.toInt(), hsv)
        hsv[1] = minOf(1f, hsv[1] * 1.2f)
        hsv[2] = minOf(hsv[2], 0.55f).coerceAtLeast(0.25f)  // dark enough for white text
        val c = Color.HSVToColor(hsv)
        fun q(v: Int) = ((v + 42) / 85).coerceIn(0, 3)
        return 0xC0 or (q(Color.red(c)) shl 4) or (q(Color.green(c)) shl 2) or q(Color.blue(c))
    }

    /**
     * Floyd–Steinberg dither into GBitmap rows: 8-bit Pebble colours, or 1-bit (LSB first).
     * [rowBytes] is the watch's bytes-per-row so the layout matches exactly.
     */
    fun encode(bmp: Bitmap, color: Boolean, rowBytes: Int): ByteArray {
        val w = bmp.width; val h = bmp.height
        val px = IntArray(w * h); bmp.getPixels(px, 0, w, 0, 0, w, h)
        val ch = if (color) 3 else 1
        val f = FloatArray(w * h * ch)
        for (i in 0 until w * h) {
            val c = px[i]
            if (color) { f[i * 3] = Color.red(c).toFloat(); f[i * 3 + 1] = Color.green(c).toFloat(); f[i * 3 + 2] = Color.blue(c).toFloat() }
            else f[i] = 0.3f * Color.red(c) + 0.59f * Color.green(c) + 0.11f * Color.blue(c)
        }
        val out = ByteArray(rowBytes * h)
        fun add(x: Int, y: Int, k: Int, e: Float) { if (x in 0 until w && y < h) f[(y * w + x) * ch + k] += e }
        for (y in 0 until h) for (x in 0 until w) {
            val lv = IntArray(ch)
            for (k in 0 until ch) {
                val o = f[(y * w + x) * ch + k]
                val q = if (color) Math.round(o / 85f).coerceIn(0, 3) else if (o < 128) 0 else 1
                val e = o - if (color) q * 85f else q * 255f
                lv[k] = q
                add(x + 1, y, k, e * 7 / 16); add(x - 1, y + 1, k, e * 3 / 16); add(x, y + 1, k, e * 5 / 16); add(x + 1, y + 1, k, e / 16)
            }
            if (color) out[y * rowBytes + x] = (0xC0 or (lv[0] shl 4) or (lv[1] shl 2) or lv[2]).toByte()
            else if (lv[0] == 1) { val i = y * rowBytes + (x shr 3); out[i] = (out[i].toInt() or (1 shl (x and 7))).toByte() }
        }
        return out
    }
}
