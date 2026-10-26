package ru.avgn8bit.screens

import android.content.Intent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ru.avgn8bit.AppLog
import ru.avgn8bit.AppSettings
import ru.avgn8bit.GameStorage
import ru.avgn8bit.Haptics
import ru.avgn8bit.OrientationMode
import ru.avgn8bit.VideoFilter
import ru.avgn8bit.ui.AvgnPanel

@Composable
fun SettingsScreen(
    settings: AppSettings,
    onBack: () -> Unit,
    onControls: () -> Unit,
    onAbout: () -> Unit
) {
    val context = LocalContext.current

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Настройки", color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = onBack) { Text("Назад") }
        }
        Spacer(Modifier.height(12.dp))

        Section("Графика") {
            Text("Фильтр", color = Color(0xFFB0B0B8), fontSize = 12.sp)
            Row {
                FilterChip(
                    selected = settings.filter == VideoFilter.SHARP,
                    onClick = { settings.setFilter(VideoFilter.SHARP) },
                    label = { Text("Пиксели") },
                    modifier = Modifier.padding(end = 6.dp)
                )
                FilterChip(
                    selected = settings.filter == VideoFilter.SMOOTH,
                    onClick = { settings.setFilter(VideoFilter.SMOOTH) },
                    label = { Text("Сглаживание") },
                    modifier = Modifier.padding(end = 6.dp)
                )
                FilterChip(
                    selected = settings.filter == VideoFilter.CRT,
                    onClick = { settings.setFilter(VideoFilter.CRT) },
                    label = { Text("CRT") }
                )
            }
            Spacer(Modifier.height(4.dp))
            SwitchRow("Целочисленное масштабирование", settings.integerScaling) { settings.setIntegerScaling(it) }
            SwitchRow("Широкий экран (больше видно по краям)", settings.widescreen) { settings.setWidescreen(it) }
            SwitchRow("Счётчик FPS", settings.showFps) { settings.setShowFps(it) }
            SwitchRow("Оптимизации Vita (быстрее)", settings.fastGraphics) { settings.setFastGraphics(it) }
            Text(
                "Рендер порта унаследован от PS Vita-версии: кэш текстур, непрозрачные батчи, запекание. " +
                    "Обычно это самый быстрый режим; если картинка выглядит неправильно (чёрные или пропавшие " +
                    "спрайты), выключите — тогда все оптимизации отключаются через файлы-маркеры рядом с data.win.",
                color = Color(0xFF888888),
                fontSize = 11.sp
            )
        }

        Section("Управление") {
            SliderRow("Прозрачность кнопок", settings.controlsOpacity, 0.25f..1f) { settings.setControlsOpacity(it) }
            SliderRow("Размер кнопок", settings.controlsScale, 0.6f..1.6f) { settings.setControlsScale(it) }
            SwitchRow("Показывать кнопки", settings.controlsVisible) { settings.setControlsVisible(it) }
            SwitchRow("Вибрация при нажатии", settings.haptics) {
                settings.setHaptics(it)
                Haptics.configure(it, settings.hapticsStrength)
            }
            if (settings.haptics) {
                Text("Сила вибрации", color = Color(0xFFB0B0B8), fontSize = 12.sp)
                Slider(
                    value = settings.hapticsStrength.toFloat(),
                    onValueChange = {
                        settings.setHapticsStrength(it.toInt())
                        Haptics.configure(true, it.toInt())
                    },
                    valueRange = 10f..255f
                )
            }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(onClick = onControls, modifier = Modifier.fillMaxWidth()) {
                Text("Настроить раскладку кнопок")
            }
        }

        Section("Экран") {
            Text("Ориентация", color = Color(0xFFB0B0B8), fontSize = 12.sp)
            Row(Modifier.fillMaxWidth()) {
                OrientationMode.entries.forEach { mode ->
                    FilterChip(
                        selected = settings.orientation == mode,
                        onClick = { settings.setOrientation(mode) },
                        label = { Text(mode.label, fontSize = 11.sp) },
                        modifier = Modifier.padding(end = 6.dp)
                    )
                }
            }
            Spacer(Modifier.height(4.dp))
            SwitchRow("Не гасить экран", settings.keepScreenOn) { settings.setKeepScreenOn(it) }
            SwitchRow("Скрывать системные панели", settings.hideSystemBars) { settings.setHideSystemBars(it) }
        }

        Section("Файлы") {
            SwitchRow("Копировать видео при импорте", settings.includeVideosOnImport) { settings.setIncludeVideosOnImport(it) }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(
                onClick = {
                    val file = GameStorage.logFile(context)
                    val intent = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_SUBJECT, "AVGN 8-bit лог")
                        putExtra(Intent.EXTRA_TEXT, AppLog.read())
                    }
                    try {
                        context.startActivity(Intent.createChooser(intent, "Поделиться логом"))
                    } catch (_: Throwable) {
                    }
                },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Поделиться логом (${AppLog.path ?: "нет файла"})")
            }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(onClick = onAbout, modifier = Modifier.fillMaxWidth()) {
                Text("О порте и лицензии")
            }
        }
        Spacer(Modifier.height(30.dp))
    }
}

@Composable
fun Section(title: String, content: @Composable () -> Unit) {
    Spacer(Modifier.height(12.dp))
    Text(title.uppercase(), color = Color(0xFF9E9EA7), fontSize = 11.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
    Spacer(Modifier.height(6.dp))
    Card(
        colors = CardDefaults.cardColors(containerColor = AvgnPanel),
        shape = RoundedCornerShape(16.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(14.dp)) { content() }
    }
}

@Composable
fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Color.White, fontSize = 14.sp, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
fun SliderRow(label: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Row {
            Text(label, color = Color.White, fontSize = 14.sp, modifier = Modifier.weight(1f))
            Text("%.0f%%".format(if (range.endInclusive <= 2f) value * 100 else value), color = Color(0xFF9E9EA7), fontSize = 12.sp)
        }
        Slider(value = value, onValueChange = onChange, valueRange = range)
    }
}
