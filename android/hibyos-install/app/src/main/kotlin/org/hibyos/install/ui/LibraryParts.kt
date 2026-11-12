package org.hibyos.install.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.hibyos.install.R

/** A picture of the cache, decoded at about the size it is shown (big covers are sampled down). */
private fun thumb(path: String, px: Int): ImageBitmap? = try {
    val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeFile(path, o)
    var sample = 1
    while (o.outWidth / (sample * 2) >= px && o.outHeight / (sample * 2) >= px) sample *= 2
    BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
} catch (e: Exception) {
    null
} catch (e: OutOfMemoryError) {
    null
}

/** An album's artwork, or its music note on pink while there is none. */
@Composable
fun Artwork(path: String?, size: Dp, corner: Dp = 6.dp, description: String? = null) {
    val px = with(LocalDensity.current) { size.roundToPx() }
    val bmp = remember(path, px) { path?.let { thumb(it, px) } }
    val shape = RoundedCornerShape(corner)
    val m = Modifier.size(size).clip(shape).semantics { if (description != null) contentDescription = description }
    if (bmp != null) {
        Image(bmp, null, m, contentScale = ContentScale.Crop)
    } else {
        Box(m.background(C.pink), contentAlignment = Alignment.Center) { Glyph(Glyphs.Music, Color.White, size * 0.55f) }
    }
}

/** The search field of the library, as on iOS: a grey rounded field with a magnifier. */
@Composable
fun SearchBar(value: String, hint: String, onChange: (String) -> Unit) {
    val clear = stringResource(R.string.sync_search_clear)
    Row(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 14.dp).fillMaxWidth().heightIn(min = 40.dp).clip(RoundedCornerShape(10.dp))
        .background(C.card).padding(horizontal = 10.dp), verticalAlignment = Alignment.CenterVertically) {
        Glyph(Glyphs.Search, C.secondary, 18.dp)
        BasicTextField(
            value = value,
            onValueChange = { onChange(it.replace("\n", "")) },
            modifier = Modifier.weight(1f).padding(start = 8.dp, top = 9.dp, bottom = 9.dp).semantics { contentDescription = hint },
            singleLine = true,
            textStyle = T.body,
            cursorBrush = SolidColor(C.blue),
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = {}),
            decorationBox = { inner ->
                Box(contentAlignment = Alignment.CenterStart) {
                    if (value.isEmpty()) Txt(hint, T.body.copy(color = C.tertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    inner()
                }
            },
        )
        if (value.isNotEmpty()) {
            Box(Modifier.size(36.dp).clip(RoundedCornerShape(18.dp)).clickable { onChange("") }
                .semantics { contentDescription = clear }, contentAlignment = Alignment.Center) {
                Glyph(Glyphs.Cross, C.secondary, 12.dp)
            }
        }
    }
}

