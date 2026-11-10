package org.hibyos.install.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.sp

/** The colours of HiByOS on the player: black, dark grey cards and the system accents. */
object C {
    val bg = Color(0xFF000000)
    val card = Color(0xFF1C1C1E)
    val fill = Color(0xFF2C2C2E)
    val pressed = Color(0xFF2C2C2E)
    val sep = Color(0xFF38383A)
    val label = Color(0xFFFFFFFF)
    val secondary = Color(0xFF98989F)
    val tertiary = Color(0xFF636366)
    val blue = Color(0xFF0A84FF)
    val green = Color(0xFF30D158)
    val red = Color(0xFFFF453A)
    val orange = Color(0xFFFF9F0A)
    val purple = Color(0xFF5E5CE6)
    val cyan = Color(0xFF64D2FF)
    val pink = Color(0xFFFF375F)
    val grey = Color(0xFF8E8E93)
}

object T {
    val largeTitle = TextStyle(color = C.label, fontSize = 34.sp, lineHeight = 41.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.2.sp)
    val navTitle = TextStyle(color = C.label, fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold, textAlign = TextAlign.Center)
    val nav = TextStyle(color = C.blue, fontSize = 17.sp, lineHeight = 22.sp)
    val title2 = TextStyle(color = C.label, fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center)
    val headline = TextStyle(color = C.label, fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold)
    val body = TextStyle(color = C.label, fontSize = 17.sp, lineHeight = 22.sp)
    val callout = TextStyle(color = C.label, fontSize = 16.sp, lineHeight = 22.sp)
    val lead = TextStyle(color = C.secondary, fontSize = 16.sp, lineHeight = 22.sp)
    val value = TextStyle(color = C.secondary, fontSize = 17.sp, lineHeight = 22.sp, textAlign = TextAlign.End)
    val sub = TextStyle(color = C.secondary, fontSize = 14.sp, lineHeight = 19.sp)
    val header = TextStyle(color = C.secondary, fontSize = 13.sp, lineHeight = 18.sp, letterSpacing = 0.2.sp)
    val footer = TextStyle(color = C.secondary, fontSize = 13.sp, lineHeight = 18.sp)
    val caption = TextStyle(color = C.tertiary, fontSize = 12.sp, lineHeight = 16.sp)
    val mono = TextStyle(color = C.secondary, fontSize = 12.sp, lineHeight = 17.sp, fontFamily = FontFamily.Monospace)
    val button = TextStyle(color = C.label, fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold, textAlign = TextAlign.Center)
    val percent = TextStyle(color = C.label, fontSize = 56.sp, lineHeight = 64.sp, fontWeight = FontWeight.Bold,
        fontFeatureSettings = "tnum", textAlign = TextAlign.Center)
    val badge = TextStyle(color = C.label, fontSize = 14.sp, lineHeight = 18.sp, fontWeight = FontWeight.SemiBold,
        fontFeatureSettings = "tnum", textAlign = TextAlign.Center)
    val license = TextStyle(color = Color(0xFFD1D1D6), fontSize = 13.sp, lineHeight = 19.sp)
}
