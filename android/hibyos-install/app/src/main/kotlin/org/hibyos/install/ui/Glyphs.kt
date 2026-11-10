package org.hibyos.install.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/** A small line drawing in a 24 x 24 box, in the given colour. */
typealias GlyphDraw = DrawScope.(Color) -> Unit

@Composable
fun Glyph(glyph: GlyphDraw, color: Color, size: Dp = 20.dp, modifier: Modifier = Modifier) {
    Canvas(modifier.size(size)) { glyph(color) }
}

private fun DrawScope.line(c: Color, w: Float = 2f, build: Path.() -> Unit) {
    val k = size.minDimension / 24f
    val p = Path().apply(build)
    scale(k, k, pivot = Offset.Zero) {
        drawPath(p, c, style = Stroke(width = w, cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

private fun DrawScope.solid(c: Color, build: Path.() -> Unit) {
    val k = size.minDimension / 24f
    val p = Path().apply(build)
    scale(k, k, pivot = Offset.Zero) { drawPath(p, c) }
}

private fun Path.dot(x: Float, y: Float, r: Float) = addOval(Rect(Offset(x, y), r))

private fun Path.box(l: Float, t: Float, r: Float, b: Float, radius: Float) =
    addRoundRect(RoundRect(l, t, r, b, CornerRadius(radius, radius)))

object Glyphs {
    val Chevron: GlyphDraw = { c -> line(c, 2.6f) { moveTo(9f, 5f); lineTo(16f, 12f); lineTo(9f, 19f) } }

    val Back: GlyphDraw = { c -> line(c, 2.6f) { moveTo(15f, 4f); lineTo(7f, 12f); lineTo(15f, 20f) } }

    val Check: GlyphDraw = { c -> line(c, 2.6f) { moveTo(5f, 12.5f); lineTo(10f, 17.5f); lineTo(19f, 7f) } }

    val Cross: GlyphDraw = { c -> line(c, 2.6f) { moveTo(6.5f, 6.5f); lineTo(17.5f, 17.5f); moveTo(17.5f, 6.5f); lineTo(6.5f, 17.5f) } }

    val Download: GlyphDraw = { c ->
        line(c) {
            moveTo(12f, 3.5f); lineTo(12f, 15f)
            moveTo(7.5f, 10.5f); lineTo(12f, 15f); lineTo(16.5f, 10.5f)
            moveTo(4.5f, 16.5f); lineTo(4.5f, 20f); lineTo(19.5f, 20f); lineTo(19.5f, 16.5f)
        }
    }

    val Share: GlyphDraw = { c ->
        line(c) {
            moveTo(8.5f, 10f); lineTo(6f, 10f); lineTo(6f, 20.5f); lineTo(18f, 20.5f); lineTo(18f, 10f); lineTo(15.5f, 10f)
            moveTo(12f, 3f); lineTo(12f, 14.5f)
            moveTo(8.5f, 6.5f); lineTo(12f, 3f); lineTo(15.5f, 6.5f)
        }
    }

    val Wifi: GlyphDraw = { c ->
        line(c, 2.2f) {
            arcTo(Rect(Offset(12f, 18.5f), 13f), 228f, 84f, true)
            arcTo(Rect(Offset(12f, 18.5f), 8.6f), 228f, 84f, true)
            arcTo(Rect(Offset(12f, 18.5f), 4.3f), 228f, 84f, true)
        }
        solid(c) { dot(12f, 18.5f, 1.5f) }
    }

    val Layers: GlyphDraw = { c ->
        line(c) {
            moveTo(12f, 3.5f); lineTo(21f, 8.5f); lineTo(12f, 13.5f); lineTo(3f, 8.5f); close()
            moveTo(3f, 12.5f); lineTo(12f, 17.5f); lineTo(21f, 12.5f)
            moveTo(3f, 16.5f); lineTo(12f, 21.5f); lineTo(21f, 16.5f)
        }
    }

    val Folder: GlyphDraw = { c ->
        line(c) {
            moveTo(3f, 6f); lineTo(9.5f, 6f); lineTo(11.5f, 8.5f); lineTo(21f, 8.5f); lineTo(21f, 19f); lineTo(3f, 19f); close()
        }
    }

    val Player: GlyphDraw = { c ->
        line(c) {
            box(6.5f, 2.5f, 17.5f, 21.5f, 2.6f)
            box(9.3f, 5.5f, 14.7f, 11f, 0.8f)
            dot(12f, 16.2f, 2.1f)
        }
    }

    val Card: GlyphDraw = { c ->
        line(c) {
            moveTo(9.5f, 3f); lineTo(18f, 3f); lineTo(18f, 21f); lineTo(6f, 21f); lineTo(6f, 6.5f); close()
        }
        line(c, 1.7f) {
            moveTo(10.2f, 6.5f); lineTo(10.2f, 9.5f)
            moveTo(12.7f, 6.5f); lineTo(12.7f, 9.5f)
            moveTo(15.2f, 6.5f); lineTo(15.2f, 9.5f)
        }
    }

    val Battery: GlyphDraw = { c ->
        line(c) {
            box(2.5f, 7.5f, 19.5f, 16.5f, 2.6f)
            moveTo(22f, 10.7f); lineTo(22f, 13.3f)
        }
        solid(c) { box(5f, 10f, 14f, 14f, 1f) }
    }

    val Info: GlyphDraw = { c ->
        line(c) {
            dot(12f, 12f, 9f)
            moveTo(12f, 11f); lineTo(12f, 16.5f)
        }
        solid(c) { dot(12f, 7.7f, 1.25f) }
    }

    val Alert: GlyphDraw = { c ->
        line(c) {
            moveTo(12f, 3.5f); lineTo(21.5f, 20f); lineTo(2.5f, 20f); close()
            moveTo(12f, 9.5f); lineTo(12f, 14f)
        }
        solid(c) { dot(12f, 17f, 1.2f) }
    }

    val Search: GlyphDraw = { c ->
        line(c, 2.2f) {
            dot(10.5f, 10.5f, 6.5f)
            moveTo(15.4f, 15.4f); lineTo(20.5f, 20.5f)
        }
    }

    val Doc: GlyphDraw = { c ->
        line(c) {
            moveTo(6f, 3f); lineTo(13.5f, 3f); lineTo(18f, 7.5f); lineTo(18f, 21f); lineTo(6f, 21f); close()
            moveTo(13.5f, 3f); lineTo(13.5f, 7.5f); lineTo(18f, 7.5f)
        }
    }

    val Refresh: GlyphDraw = { c ->
        line(c) {
            arcTo(Rect(Offset(12f, 12f), 8f), -35f, 290f, true)
            moveTo(19f, 3.5f); lineTo(18.7f, 7.6f); lineTo(14.6f, 7.2f)
        }
    }

    val External: GlyphDraw = { c ->
        line(c) {
            moveTo(7.5f, 16.5f); lineTo(17f, 7f)
            moveTo(9.5f, 7f); lineTo(17f, 7f); lineTo(17f, 14.5f)
        }
    }

    val Trash: GlyphDraw = { c ->
        line(c) {
            moveTo(4.5f, 7f); lineTo(19.5f, 7f)
            moveTo(9f, 7f); lineTo(9f, 4f); lineTo(15f, 4f); lineTo(15f, 7f)
            moveTo(6.5f, 7f); lineTo(7.7f, 20.5f); lineTo(16.3f, 20.5f); lineTo(17.5f, 7f)
        }
    }

    val Book: GlyphDraw = { c ->
        line(c) {
            moveTo(12f, 6.5f); lineTo(12f, 20f)
            moveTo(12f, 6.5f); lineTo(9f, 4.8f); lineTo(3.5f, 4.8f); lineTo(3.5f, 18f); lineTo(9f, 18f); lineTo(12f, 20f)
            moveTo(12f, 6.5f); lineTo(15f, 4.8f); lineTo(20.5f, 4.8f); lineTo(20.5f, 18f); lineTo(15f, 18f); lineTo(12f, 20f)
        }
    }

    val Phone: GlyphDraw = { c ->
        line(c) {
            box(6.5f, 2.5f, 17.5f, 21.5f, 2.6f)
            moveTo(10.5f, 18.5f); lineTo(13.5f, 18.5f)
        }
    }
}
