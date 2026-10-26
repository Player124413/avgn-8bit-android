package ru.avgn8bit.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

val AvgnRed = Color(0xFFE53935)
val AvgnRedDark = Color(0xFF8E1B18)
val AvgnInk = Color(0xFF0B0B0F)
val AvgnPanel = Color(0xFF17171D)

private val colors = darkColorScheme(
    primary = AvgnRed,
    onPrimary = Color.White,
    primaryContainer = AvgnRedDark,
    onPrimaryContainer = Color.White,
    secondary = Color(0xFFB0BEC5),
    background = AvgnInk,
    onBackground = Color(0xFFEDEDED),
    surface = AvgnPanel,
    onSurface = Color(0xFFEDEDED),
    surfaceVariant = Color(0xFF24242C),
    onSurfaceVariant = Color(0xFFB0B0B8),
    error = Color(0xFFEF9A9A)
)

private val typography = Typography(
    headlineMedium = TextStyle(fontSize = 26.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.5.sp),
    titleLarge = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold),
    bodyMedium = TextStyle(fontSize = 14.sp),
    labelLarge = TextStyle(fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
)

@Composable
fun AvgnTheme(content: @Composable () -> Unit) {
    // The port is dark whatever the system thinks: it is a CRT game.
    MaterialTheme(colorScheme = colors, typography = typography, content = content)
}
