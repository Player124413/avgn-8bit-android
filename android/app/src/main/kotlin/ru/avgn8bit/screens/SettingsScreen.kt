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
import ru.avgn8bit.R
import ru.avgn8bit.ui.AvgnPanel
import ru.avgn8bit.ui.str

@Composable
fun SettingsScreen(
    settings: AppSettings,
    onBack: () -> Unit,
    onControls: () -> Unit,
    onAbout: () -> Unit
) {
    val context = LocalContext.current
    // Read outside the button's lambda: those run later, outside composition.
    val logSubject = str(R.string.log_subject)
    val shareChooser = str(R.string.common_share_log)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(str(R.string.settings_title), color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = onBack) { Text(str(R.string.common_back)) }
        }
        Spacer(Modifier.height(12.dp))

        Section(str(R.string.settings_section_graphics)) {
            Text(str(R.string.settings_filter), color = Color(0xFFB0B0B8), fontSize = 12.sp)
            Row {
                FilterChip(
                    selected = settings.filter == VideoFilter.SHARP,
                    onClick = { settings.filter = VideoFilter.SHARP },
                    label = { Text(str(R.string.filter_sharp)) },
                    modifier = Modifier.padding(end = 6.dp)
                )
                FilterChip(
                    selected = settings.filter == VideoFilter.SMOOTH,
                    onClick = { settings.filter = VideoFilter.SMOOTH },
                    label = { Text(str(R.string.filter_smooth)) },
                    modifier = Modifier.padding(end = 6.dp)
                )
                FilterChip(
                    selected = settings.filter == VideoFilter.CRT,
                    onClick = { settings.filter = VideoFilter.CRT },
                    label = { Text(str(R.string.filter_crt)) }
                )
            }
            Spacer(Modifier.height(4.dp))
            SwitchRow(str(R.string.settings_integer_scaling), settings.integerScaling) { settings.integerScaling = it }
            SwitchRow(str(R.string.settings_widescreen), settings.widescreen) { settings.widescreen = it }
            SwitchRow(str(R.string.settings_fps), settings.showFps) { settings.showFps = it }
            SwitchRow(str(R.string.settings_fast_graphics), settings.fastGraphics) { settings.fastGraphics = it }
            Text(
                str(R.string.settings_fast_graphics_hint),
                color = Color(0xFF888888),
                fontSize = 11.sp
            )
        }

        Section(str(R.string.settings_section_controls)) {
            SliderRow(str(R.string.settings_controls_opacity), settings.controlsOpacity, 0.25f..1f) { settings.controlsOpacity = it }
            SliderRow(str(R.string.settings_controls_scale), settings.controlsScale, 0.6f..1.6f) { settings.controlsScale = it }
            SwitchRow(str(R.string.settings_controls_visible), settings.controlsVisible) { settings.controlsVisible = it }
            SwitchRow(str(R.string.settings_haptics), settings.haptics) {
                settings.haptics = it
                Haptics.configure(it, settings.hapticsStrength)
            }
            if (settings.haptics) {
                Text(str(R.string.settings_haptics_strength), color = Color(0xFFB0B0B8), fontSize = 12.sp)
                Slider(
                    value = settings.hapticsStrength.toFloat(),
                    onValueChange = {
                        settings.hapticsStrength = it.toInt()
                        Haptics.configure(true, it.toInt())
                    },
                    valueRange = 10f..255f
                )
            }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(onClick = onControls, modifier = Modifier.fillMaxWidth()) {
                Text(str(R.string.settings_edit_layout))
            }
        }

        Section(str(R.string.settings_section_screen)) {
            Text(str(R.string.settings_orientation), color = Color(0xFFB0B0B8), fontSize = 12.sp)
            Row(Modifier.fillMaxWidth()) {
                OrientationMode.entries.forEach { mode ->
                    FilterChip(
                        selected = settings.orientation == mode,
                        onClick = { settings.orientation = mode },
                        label = { Text(str(mode.labelRes), fontSize = 11.sp) },
                        modifier = Modifier.padding(end = 6.dp)
                    )
                }
            }
            Spacer(Modifier.height(4.dp))
            SwitchRow(str(R.string.settings_keep_screen_on), settings.keepScreenOn) { settings.keepScreenOn = it }
            SwitchRow(str(R.string.settings_hide_system_bars), settings.hideSystemBars) { settings.hideSystemBars = it }
        }

        Section(str(R.string.settings_section_files)) {
            SwitchRow(str(R.string.settings_include_videos), settings.includeVideosOnImport) { settings.includeVideosOnImport = it }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(
                onClick = {
                    val file = GameStorage.logFile(context)
                    val intent = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_SUBJECT, logSubject)
                        putExtra(Intent.EXTRA_TEXT, AppLog.read())
                    }
                    try {
                        context.startActivity(Intent.createChooser(intent, shareChooser))
                    } catch (_: Throwable) {
                    }
                },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(str(R.string.settings_share_log_with_path, AppLog.path ?: str(R.string.common_no_file)))
            }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(onClick = onAbout, modifier = Modifier.fillMaxWidth()) {
                Text(str(R.string.settings_about))
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
