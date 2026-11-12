package org.hibyos.install.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.hibyos.install.ErrorInfo
import org.hibyos.install.R

// ---- text -----------------------------------------------------------------------------------

@Composable
fun Txt(text: String, style: TextStyle, modifier: Modifier = Modifier, maxLines: Int = Int.MAX_VALUE,
        overflow: TextOverflow = TextOverflow.Clip) {
    BasicText(text = text, modifier = modifier, style = style, maxLines = maxLines, overflow = overflow)
}

@Composable
fun Txt(text: AnnotatedString, style: TextStyle, modifier: Modifier = Modifier, maxLines: Int = Int.MAX_VALUE,
        overflow: TextOverflow = TextOverflow.Clip) {
    BasicText(text = text, modifier = modifier, style = style, maxLines = maxLines, overflow = overflow)
}

/** Turns the ** marks of the string resources into bold, brighter text. */
fun rich(s: String): AnnotatedString = buildAnnotatedString {
    var i = 0
    var bold = false
    while (true) {
        val j = s.indexOf("**", i)
        val piece = if (j < 0) s.substring(i) else s.substring(i, j)
        if (bold) withStyle(SpanStyle(fontWeight = FontWeight.SemiBold, color = C.label)) { append(piece) } else append(piece)
        if (j < 0) break
        bold = !bold
        i = j + 2
    }
}

/** "ByOS Install" with the two colours of the ByOS logo. */
fun wordmark(): AnnotatedString = buildAnnotatedString {
    append("By")
    withStyle(SpanStyle(color = C.cyan)) { append("OS") }
    append(" Install")
}

// ---- touch ----------------------------------------------------------------------------------

@Composable
private fun pressable(onClick: (() -> Unit)?, enabled: Boolean = true): Pair<Modifier, Boolean> {
    if (onClick == null) return Modifier to false
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    return Modifier.clickable(interactionSource = source, indication = null, enabled = enabled, onClick = onClick) to (pressed && enabled)
}

// ---- page -----------------------------------------------------------------------------------

private val contentWidth = 560.dp

@Composable
fun NavAction(text: String, onClick: () -> Unit) {
    val (click, pressed) = pressable(onClick)
    Box(Modifier.heightIn(min = 44.dp).then(click).alpha(if (pressed) 0.4f else 1f).padding(horizontal = 16.dp),
        contentAlignment = Alignment.Center) {
        Txt(text, T.nav, maxLines = 1)
    }
}

@Composable
private fun NavBar(title: AnnotatedString, back: String?, onBack: (() -> Unit)?, trailing: (@Composable () -> Unit)?, collapsed: Boolean) {
    val titleAlpha by animateFloatAsState(if (collapsed) 1f else 0f, tween(160), label = "navTitle")
    Box(Modifier.fillMaxWidth().height(44.dp)) {
        Txt(title, T.navTitle, Modifier.align(Alignment.Center).padding(horizontal = 96.dp).alpha(titleAlpha), maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        if (onBack != null) {
            val (click, pressed) = pressable(onBack)
            Row(Modifier.align(Alignment.CenterStart).height(44.dp).then(click).alpha(if (pressed) 0.4f else 1f)
                .padding(start = 6.dp, end = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Glyph(Glyphs.Back, C.blue, 24.dp)
                Txt(back ?: stringResource(R.string.back), T.nav, Modifier.padding(start = 1.dp).widthIn(max = 120.dp), maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
            }
        }
        if (trailing != null) Box(Modifier.align(Alignment.CenterEnd)) { trailing() }
        Box(Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(0.5.dp).alpha(titleAlpha).background(C.sep))
    }
}

/**
 * A screen: the bar with "back", the large title, the scrolling content and, pinned at the
 * bottom, the buttons of the screen.
 */
@Composable
fun Page(
    title: AnnotatedString,
    back: String? = null,
    onBack: (() -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    bottom: (@Composable ColumnScope.() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val scroll = rememberScrollState()
    val limit = with(LocalDensity.current) { 40.dp.toPx() }
    val collapsed by remember(limit) { derivedStateOf { scroll.value > limit } }
    Column(Modifier.fillMaxSize().background(C.bg).windowInsetsPadding(WindowInsets.safeDrawing)) {
        NavBar(title, back, onBack, trailing, collapsed)
        Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.TopCenter) {
            Column(Modifier.widthIn(max = contentWidth).fillMaxWidth().verticalScroll(scroll)) {
                Txt(title, T.largeTitle, Modifier.padding(start = 20.dp, end = 20.dp, top = 2.dp, bottom = 12.dp))
                content()
                Spacer(Modifier.height(8.dp))
            }
        }
        if (bottom != null) {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.TopCenter) {
                Column(Modifier.widthIn(max = contentWidth).fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)) { bottom() }
            }
        }
    }
}

/** The bar alone, for the screens that bring their own scrolling list. */
@Composable
fun PageFrame(title: AnnotatedString, back: String?, onBack: (() -> Unit)?, collapsed: Boolean, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize().background(C.bg).windowInsetsPadding(WindowInsets.safeDrawing)) {
        NavBar(title, back, onBack, null, collapsed)
        Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.TopCenter) {
            Column(Modifier.widthIn(max = contentWidth).fillMaxWidth()) { content() }
        }
    }
}

// ---- grouped lists --------------------------------------------------------------------------

class Rows internal constructor() {
    internal val list = ArrayList<Pair<Dp, @Composable () -> Unit>>()

    /** Adds a row; [inset] is where the line above it starts. */
    fun row(inset: Dp = 16.dp, content: @Composable () -> Unit) {
        list += inset to content
    }
}

val iconInset = 58.dp

@Composable
fun Section(header: String? = null, footer: String? = null, build: Rows.() -> Unit) {
    val rows = Rows().apply(build)
    if (rows.list.isEmpty()) return
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        if (header != null) Txt(header.uppercase(), T.header, Modifier.padding(start = 16.dp, end = 16.dp, bottom = 7.dp))
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(C.card)) {
            rows.list.forEachIndexed { i, r ->
                if (i > 0) Box(Modifier.padding(start = r.first).fillMaxWidth().height(0.5.dp).background(C.sep))
                r.second()
            }
        }
        if (footer != null) Txt(rich(footer), T.footer, Modifier.padding(start = 16.dp, end = 16.dp, top = 7.dp))
    }
    Spacer(Modifier.height(26.dp))
}

@Composable
fun IconTile(glyph: GlyphDraw, color: Color) {
    Box(Modifier.size(30.dp).clip(RoundedCornerShape(7.dp)).background(color), contentAlignment = Alignment.Center) {
        Glyph(glyph, Color.White, 20.dp)
    }
}

/** A row of a list: icon, title and smaller text, a value or a status at the right, a chevron. */
@Composable
fun Item(
    title: String,
    subtitle: String? = null,
    value: String? = null,
    icon: (@Composable () -> Unit)? = null,
    chevron: Boolean = false,
    titleColor: Color = C.label,
    valueColor: Color = C.secondary,
    trailing: (@Composable () -> Unit)? = null,
    enabled: Boolean = true,
    onClick: (() -> Unit)? = null,
) {
    val (click, pressed) = pressable(onClick, enabled)
    Row(Modifier.fillMaxWidth().background(if (pressed) C.pressed else Color.Transparent).then(click).heightIn(min = 44.dp)
        .alpha(if (enabled) 1f else 0.4f).padding(horizontal = 16.dp, vertical = 11.dp), verticalAlignment = Alignment.CenterVertically) {
        if (icon != null) {
            icon()
            Spacer(Modifier.width(12.dp))
        }
        Column(Modifier.weight(1f)) {
            Txt(title, T.body.copy(color = titleColor))
            if (subtitle != null) Txt(rich(subtitle), T.sub, Modifier.padding(top = 2.dp))
        }
        if (value != null) {
            Txt(value, T.value.copy(color = valueColor), Modifier.padding(start = 12.dp).widthIn(max = 210.dp), maxLines = 2,
                overflow = TextOverflow.Ellipsis)
        }
        if (trailing != null) {
            Spacer(Modifier.width(12.dp))
            trailing()
        }
        if (chevron) Glyph(Glyphs.Chevron, C.tertiary, 14.dp, Modifier.padding(start = 8.dp))
    }
}

/** A row with a label and a long value under it (a checksum). */
@Composable
fun CodeItem(title: String, code: String) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp)) {
        Txt(title, T.body)
        Txt(code, T.mono, Modifier.padding(top = 4.dp))
    }
}

/** A paragraph inside a card. */
@Composable
fun TextRow(text: String, style: TextStyle = T.callout.copy(color = Color(0xFFD1D1D6))) {
    Txt(rich(text), style, Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 13.dp))
}

/** One numbered instruction. */
@Composable
fun StepText(n: Int, text: String) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)) {
        Box(Modifier.size(24.dp).clip(CircleShape).background(C.fill), contentAlignment = Alignment.Center) {
            Txt(n.toString(), T.badge.copy(fontSize = T.footer.fontSize))
        }
        Txt(rich(text), T.callout.copy(color = Color(0xFFD1D1D6)), Modifier.padding(start = 12.dp, top = 1.dp).weight(1f))
    }
}

val stepInset = 52.dp

/** Something to check before going on. */
@Composable
fun CheckText(text: String) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)) {
        Box(Modifier.size(24.dp).clip(CircleShape).background(C.green.copy(alpha = 0.18f)), contentAlignment = Alignment.Center) {
            Glyph(Glyphs.Check, C.green, 15.dp)
        }
        Txt(rich(text), T.callout.copy(color = Color(0xFFD1D1D6)), Modifier.padding(start = 12.dp, top = 1.dp).weight(1f))
    }
}

/** The round mark of a step: done, the one to do now, or still to come. */
@Composable
fun StepBadge(n: Int, state: Int) {
    val color = when (state) {
        2 -> C.green
        1 -> C.blue
        else -> C.fill
    }
    Box(Modifier.size(30.dp).clip(CircleShape).background(color), contentAlignment = Alignment.Center) {
        if (state == 2) Glyph(Glyphs.Check, Color.White, 18.dp)
        else Txt(n.toString(), T.badge.copy(color = if (state == 1) Color.White else C.secondary, fontSize = T.callout.fontSize))
    }
}

// ---- buttons --------------------------------------------------------------------------------

@Composable
fun PrimaryButton(text: String, enabled: Boolean = true, color: Color = C.blue, onClick: () -> Unit) {
    val (click, pressed) = pressable(onClick, enabled)
    Box(Modifier.fillMaxWidth().heightIn(min = 50.dp).clip(RoundedCornerShape(14.dp)).background(if (enabled) color else C.card).then(click)
        .alpha(if (pressed) 0.6f else 1f).padding(horizontal = 16.dp, vertical = 12.dp), contentAlignment = Alignment.Center) {
        Txt(text, T.button.copy(color = if (enabled) Color.White else C.tertiary), maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

/** The quiet button: "Cancel" while work is running. */
@Composable
fun GreyButton(text: String, color: Color = C.label, onClick: () -> Unit) {
    val (click, pressed) = pressable(onClick)
    Box(Modifier.fillMaxWidth().heightIn(min = 50.dp).clip(RoundedCornerShape(14.dp)).background(C.card).then(click)
        .alpha(if (pressed) 0.6f else 1f).padding(horizontal = 16.dp, vertical = 12.dp), contentAlignment = Alignment.Center) {
        Txt(text, T.button.copy(color = color), maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
fun LinkButton(text: String, onClick: () -> Unit) {
    val (click, pressed) = pressable(onClick)
    Box(Modifier.fillMaxWidth().heightIn(min = 44.dp).then(click).alpha(if (pressed) 0.4f else 1f).padding(horizontal = 16.dp, vertical = 10.dp),
        contentAlignment = Alignment.Center) {
        Txt(text, T.nav.copy(textAlign = TextAlign.Center), maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

// ---- progress -------------------------------------------------------------------------------

@Composable
fun ProgressBar(fraction: Float, modifier: Modifier = Modifier, color: Color = C.blue) {
    val f by animateFloatAsState(fraction.coerceIn(0f, 1f), tween(220, easing = LinearEasing), label = "progress")
    Canvas(modifier.fillMaxWidth().height(6.dp)) {
        val r = size.height / 2
        drawLine(C.fill, Offset(r, r), Offset(size.width - r, r), size.height, StrokeCap.Round)
        if (f > 0f) drawLine(color, Offset(r, r), Offset(r + (size.width - 2 * r) * f, r), size.height, StrokeCap.Round)
    }
}

@Composable
fun Spinner(size: Dp = 22.dp, color: Color = C.secondary) {
    val t = rememberInfiniteTransition(label = "spinner")
    val angle by t.animateFloat(0f, 360f, infiniteRepeatable(tween(900, easing = LinearEasing), RepeatMode.Restart), label = "angle")
    Canvas(Modifier.size(size)) {
        val w = this.size.minDimension * 0.13f
        val inset = w / 2
        drawArc(color.copy(alpha = 0.25f), 0f, 360f, false, Offset(inset, inset),
            Size(this.size.width - w, this.size.height - w), style = Stroke(w))
        drawArc(color, angle - 90f, 100f, false, Offset(inset, inset),
            Size(this.size.width - w, this.size.height - w), style = Stroke(w, cap = StrokeCap.Round))
    }
}

/** The big number of a running job, with its bar and what it is doing right now. */
@Composable
fun ProgressCard(title: String, fraction: Float, detail: String? = null, detail2: String? = null, note: String? = null) {
    val f = fraction.coerceIn(0f, 1f)
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(C.card).padding(start = 20.dp, end = 20.dp, top = 22.dp, bottom = 22.dp),
            horizontalAlignment = Alignment.CenterHorizontally) {
            Txt(title, T.headline.copy(textAlign = TextAlign.Center))
            Txt(stringResource(R.string.percent, (f * 100).toInt()), T.percent, Modifier.padding(top = 6.dp, bottom = 12.dp), maxLines = 1)
            ProgressBar(f)
            if (detail != null) {
                Txt(detail, T.sub.copy(textAlign = TextAlign.Center, color = Color(0xFFD1D1D6)), Modifier.padding(top = 14.dp), maxLines = 1)
            }
            if (detail2 != null) {
                Txt(detail2, T.caption.copy(textAlign = TextAlign.Center, color = C.secondary), Modifier.padding(top = 3.dp), maxLines = 1,
                    overflow = TextOverflow.MiddleEllipsis)
            }
        }
        if (note != null) Txt(rich(note), T.footer, Modifier.padding(start = 16.dp, end = 16.dp, top = 7.dp))
    }
    Spacer(Modifier.height(26.dp))
}

// ---- results --------------------------------------------------------------------------------

/** The result of a step: a round mark, a headline and a sentence. */
@Composable
fun Hero(glyph: GlyphDraw, color: Color, title: String, text: String? = null) {
    Column(Modifier.fillMaxWidth().padding(start = 28.dp, end = 28.dp, top = 10.dp, bottom = 26.dp),
        horizontalAlignment = Alignment.CenterHorizontally) {
        Box(Modifier.size(68.dp).clip(CircleShape).background(color), contentAlignment = Alignment.Center) {
            Glyph(glyph, Color.White, 38.dp)
        }
        Txt(title, T.title2, Modifier.padding(top = 16.dp))
        if (text != null) Txt(rich(text), T.lead.copy(textAlign = TextAlign.Center), Modifier.padding(top = 6.dp))
    }
}

@Composable
fun hint(e: ErrorInfo): String? = when {
    e.hint == 0 -> null
    e.hintArg != null -> stringResource(e.hint, e.hintArg)
    else -> stringResource(e.hint)
}

/** An error: what happened, what to do, and the technical detail in small print. */
@Composable
fun ErrorSection(e: ErrorInfo) {
    val advice = hint(e)
    Section {
        row {
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp)) {
                IconTile(Glyphs.Alert, C.red)
                Column(Modifier.padding(start = 12.dp).weight(1f)) {
                    Txt(stringResource(e.title), T.headline, Modifier.padding(top = 3.dp))
                    if (advice != null) Txt(advice, T.callout.copy(color = Color(0xFFD1D1D6), fontSize = T.sub.fontSize, lineHeight = T.sub.lineHeight), Modifier.padding(top = 5.dp))
                    if (e.detail != null) Txt(e.detail, T.caption, Modifier.padding(top = 7.dp), maxLines = 6, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}

// ---- input ----------------------------------------------------------------------------------

@Composable
fun Field(value: String, hint: String, enabled: Boolean = true, onGo: () -> Unit, onChange: (String) -> Unit) {
    BasicTextField(
        value = value,
        onValueChange = { onChange(it.replace("\n", "")) },
        modifier = Modifier.fillMaxWidth().heightIn(min = 44.dp).padding(horizontal = 16.dp, vertical = 11.dp),
        enabled = enabled,
        singleLine = true,
        textStyle = T.body,
        cursorBrush = SolidColor(C.blue),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go, autoCorrectEnabled = false),
        keyboardActions = KeyboardActions(onGo = { onGo() }),
        decorationBox = { inner ->
            Box(contentAlignment = Alignment.CenterStart) {
                if (value.isEmpty()) Txt(hint, T.body.copy(color = C.tertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                inner()
            }
        },
    )
}
