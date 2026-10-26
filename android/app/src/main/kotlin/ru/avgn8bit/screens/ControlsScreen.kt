package ru.avgn8bit.screens

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import ru.avgn8bit.AppSettings
import ru.avgn8bit.R
import ru.avgn8bit.ui.str
import ru.avgn8bit.controls.ControlEditorScreen
import ru.avgn8bit.controls.ControlsStore

/**
 * The layout editor outside the game, so the controls can be set up before the first launch (and fixed without
 * opening the menu).
 */
@Composable
fun ControlsScreen(settings: AppSettings, onBack: () -> Unit) {
    val context = LocalContext.current
    val store = remember { ControlsStore(context) }
    val configuration = LocalConfiguration.current
    val isPortrait = configuration.screenHeightDp > configuration.screenWidthDp
    var layout by remember(isPortrait) { mutableStateOf(store.load(isPortrait)) }

    LaunchedEffect(layout) { store.save(isPortrait, layout) }

    ControlEditorScreen(
        layout = layout,
        onLayoutChange = { layout = it },
        onDone = onBack,
        modifier = Modifier.fillMaxSize(),
        title = if (isPortrait) str(R.string.controls_title_portrait) else str(R.string.controls_title_landscape),
        portrait = isPortrait
    )
}
