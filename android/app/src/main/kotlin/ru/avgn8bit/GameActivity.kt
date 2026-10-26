package ru.avgn8bit

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.io.File
import ru.avgn8bit.controls.ButtonScheme
import ru.avgn8bit.controls.ControlEditorScreen
import ru.avgn8bit.controls.ControlLayout
import ru.avgn8bit.controls.ControlOverlay
import ru.avgn8bit.controls.ControlSink
import ru.avgn8bit.controls.ControlsStore
import ru.avgn8bit.controls.Presets
import ru.avgn8bit.controls.releaseAllControls
import ru.avgn8bit.ui.AvgnTheme
import ru.avgn8bit.ui.str

/**
 * The game itself: a SurfaceView the runtime draws into, the touch controls on top of it, and a menu that
 * pauses everything.
 */
class GameActivity : ComponentActivity(), ControlSink {
    private lateinit var settings: AppSettings
    private lateinit var controls: ControlsStore
    // Observable so the Compose side re-runs its pause effect once the runner exists (it is created after the
    // first-launch data patch) and so the FPS counter picks it up.
    private var runner by mutableStateOf<GameRunner?>(null)
    private var fatalError by mutableStateOf<String?>(null)
    private var activityPaused by mutableStateOf(false)
    // null until the preparation thread reports a step; the loading screen shows its own default text for that
    private var prepareText by mutableStateOf<String?>(null)
    private var prepareProgress by mutableStateOf<Float?>(null)
    private var prepareWarning by mutableStateOf<String?>(null)
    private var pendingSurface: android.view.Surface? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = (application as AvgnApp).settings
        controls = ControlsStore(this)
        Haptics.init(this)
        Haptics.configure(settings.haptics, settings.hapticsStrength)

        requestedOrientation = when (settings.orientation) {
            OrientationMode.LANDSCAPE -> ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            OrientationMode.SENSOR_LANDSCAPE -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            OrientationMode.PORTRAIT -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            OrientationMode.AUTO -> ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
        }
        if (settings.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemUi()

        val imported = GameStorage.rawDataWin(this) ?: GameStorage.patchedDataWin(this)
        val bundled = imported == null && BundledGame.isAvailable(this)
        if (imported == null && !bundled) {
            AppLog.write("GameActivity started without an imported game")
            finish()
            return
        }

        settings.applyGraphicsMarkers(GameStorage.dataDir(this))

        val dir = GameStorage.dataDir(this)
        if (imported != null && GameDataPatcher.isPatched(dir)) {
            startGame(File(dir, GameDataPatcher.OUTPUT_NAME))
        } else {
            prepareText = getString(if (bundled) R.string.game_unpacking_bundled else R.string.game_preparing)
            Thread(::prepareAndStart, "avgn-prepare").start()
        }

        setContent {
            AvgnTheme {
                val configuration = LocalConfiguration.current
                val isPortrait = configuration.screenHeightDp > configuration.screenWidthDp
                var layout by remember(isPortrait) { mutableStateOf(controls.load(isPortrait)) }
                var scheme by remember { mutableStateOf(Presets.defaultScheme) }
                var menuOpen by remember { mutableStateOf(false) }
                var editing by remember { mutableStateOf(false) }
                var quickSettings by remember { mutableStateOf(false) }
                var showLog by remember { mutableStateOf(false) }

                LaunchedEffect(layout) { controls.save(isPortrait, layout) }

                // The game is paused while any of our overlays is up, or while the app is in the background.
                val paused = menuOpen || editing || quickSettings || activityPaused
                LaunchedEffect(paused, runner) {
                    if (paused) releaseAllControls(layout, this@GameActivity)
                    runner?.setPaused(paused)
                }

                Box(Modifier.fillMaxSize().background(Color.Black)) {
                    AndroidView(
                        modifier = Modifier.fillMaxSize(),
                        factory = { context ->
                            SurfaceView(context).apply {
                                holder.addCallback(object : SurfaceHolder.Callback {
                                    override fun surfaceCreated(holder: SurfaceHolder) {
                                        pendingSurface = holder.surface
                                        runner?.attachSurface(holder.surface)
                                    }

                                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                                        runner?.updateSurfaceSize(width, height)
                                    }

                                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                                        pendingSurface = null
                                        runner?.detachSurface()
                                    }
                                })
                            }
                        }
                    )

                    if (settings.controlsVisible && !paused) {
                        ControlOverlay(
                            layout = layout,
                            sink = this@GameActivity,
                            modifier = Modifier.fillMaxSize(),
                            opacityMultiplier = settings.controlsOpacity,
                            scaleMultiplier = settings.controlsScale,
                            hapticsEnabled = settings.haptics
                        )
                    }

                    // FPS
                    if (settings.showFps) {
                        val fps = runner?.fps ?: 0f
                        Text(
                            text = "${fps.toInt()} FPS",
                            color = Color(0xAAFFFFFF),
                            fontSize = 12.sp,
                            modifier = Modifier.align(Alignment.TopEnd).padding(top = 40.dp, end = 16.dp)
                        )
                    }

                    // Pause button, always reachable
                    Box(
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(start = 10.dp, top = 34.dp)
                            .size(46.dp)
                            .background(Color(0x55000000), CircleShape)
                            .border(1.dp, Color(0x66FFFFFF), CircleShape)
                            .clickable { menuOpen = true },
                        contentAlignment = Alignment.Center
                    ) {
                        Text("II", color = Color(0xCCFFFFFF), fontSize = 15.sp, fontWeight = FontWeight.Bold)
                    }

                    if (runner?.gameRunning != true && fatalError == null) {
                        Column(
                            modifier = Modifier.align(Alignment.Center),
                            horizontalAlignment = Alignment.CenterHorizontally
                        ) {
                            val progress = prepareProgress
                            if (progress != null) {
                                CircularProgressIndicator(
                                    progress = { progress },
                                    color = Color(0xFFE53935),
                                    trackColor = Color(0x33FFFFFF)
                                )
                            } else {
                                CircularProgressIndicator(color = Color(0xFFE53935))
                            }
                            Spacer(Modifier.height(16.dp))
                            Text(
                                text = when {
                                    runner?.gameRunning == true -> ""
                                    else -> {
                                        val step = prepareText
                                        val label = step ?: str(if (progress != null) R.string.common_loading else R.string.game_loading)
                                        if (progress != null) "$label ${(progress * 100).toInt()}%" else label
                                    }
                                },
                                color = Color(0xFFCCCCCC)
                            )
                            Spacer(Modifier.height(8.dp))
                            Text(
                                str(R.string.game_first_launch),
                                color = Color(0xFF888888),
                                fontSize = 12.sp
                            )
                        }
                    }

                    if (menuOpen) {
                        PauseScreen(
                            settings = settings,
                            scheme = scheme,
                            onScheme = { chosen ->
                                scheme = chosen
                                layout = Presets.layout(chosen, isPortrait)
                            },
                            onResume = { menuOpen = false },
                            onEditControls = { menuOpen = false; editing = true },
                            onQuickSettings = { menuOpen = false; quickSettings = true },
                            onToggleControls = { settings.controlsVisible = !settings.controlsVisible },
                            onLogs = { showLog = true },
                            onQuit = {
                                menuOpen = false
                                runner?.quit()
                                finish()
                            }
                        )
                    }

                    if (quickSettings) {
                        QuickSettingsDialog(
                            settings = settings,
                            onDismiss = { quickSettings = false }
                        )
                    }

                    if (editing) {
                        ControlEditorScreen(
                            layout = layout,
                            onLayoutChange = { layout = it },
                            onDone = { editing = false },
                            modifier = Modifier.fillMaxSize(),
                            title = if (isPortrait) str(R.string.controls_title_portrait) else str(R.string.controls_title_landscape),
                            portrait = isPortrait
                        )
                    }

                    if (showLog) {
                        LogDialog(onDismiss = { showLog = false })
                    }

                    prepareWarning?.let { warning ->
                        LaunchedEffect(warning) {
                            kotlinx.coroutines.delay(9000)
                            prepareWarning = null
                        }
                        Text(
                            text = warning,
                            color = Color(0xFFFFE0B2),
                            fontSize = 11.sp,
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .padding(12.dp)
                                .background(Color(0xCC4A2A12), RoundedCornerShape(10.dp))
                                .padding(horizontal = 12.dp, vertical = 8.dp)
                        )
                    }

                    fatalError?.let { message ->
                        AlertDialog(
                            onDismissRequest = { },
                            title = { Text(str(R.string.game_failed_title)) },
                            text = { Text(message) },
                            confirmButton = {
                                TextButton(onClick = {
                                    fatalError = null
                                    finish()
                                }) { Text(str(R.string.game_exit)) }
                            }
                        )
                    }
                }
            }
        }
    }

    /**
     * Runs off the UI thread before the runtime starts: unpacks the game if it was baked into the APK, then builds
     * the patched data.win. Both steps report progress on the loading screen, and a failure never blocks the game —
     * the retail data.win is used instead.
     */
    private fun prepareAndStart() {
        var base = GameStorage.rawDataWin(this) ?: GameStorage.patchedDataWin(this)
        var unpacked = false

        if (base == null) {
            if (!BundledGame.isAvailable(this)) {
                runOnUiThread { fatalError = getString(R.string.game_files_missing) }
                return
            }
            val written = BundledGame.install(
                this,
                onProgress = { _, fraction ->
                    runOnUiThread {
                        prepareText = getString(R.string.game_unpacking_bundled)
                        prepareProgress = fraction * 0.5f
                    }
                },
                shouldStop = { isFinishing || isDestroyed }
            )
            unpacked = true
            AppLog.write("Unpacked the bundled game: ${written / 1048576} MB")
            base = GameStorage.rawDataWin(this) ?: GameStorage.patchedDataWin(this)
            if (base == null) {
                runOnUiThread { fatalError = getString(R.string.game_unpack_failed) }
                return
            }
        }

        val offset = if (unpacked) 0.5f else 0f
        runOnUiThread { prepareText = getString(R.string.import_patching) }
        val result = GameStorage.ensurePatched(this) { fraction ->
            runOnUiThread { prepareProgress = offset + fraction * (1f - offset) }
        }
        val patched = result.output
        val source = base
        runOnUiThread {
            // startGame may set a warning of its own (a missing Lang folder), so its message wins over this one.
            if (!result.ok) {
                AppLog.write("Starting on the unpatched data.win: ${result.message}")
                prepareWarning = result.message
            }
            startGame(if (result.ok && patched != null) patched else source)
            prepareProgress = null
        }
    }

    private fun startGame(dataWin: File) {
        AppLog.write("Starting the game: ${dataWin.absolutePath} (${dataWin.length()} bytes)")
        // The game keeps its menu labels and dialogs in Lang/ next to data.win; without them there is nothing to write
        // into a menu, so a screen like the options list comes out empty. Say so in the log (and on screen) instead of
        // leaving it a mystery.
        val langFiles = File(GameStorage.dataDir(this), "Lang").walkTopDown().count { it.isFile }
        AppLog.write("Lang/: $langFiles files")
        if (langFiles == 0) {
            AppLog.write("Lang/ is missing or empty: the game's own text (menus, option names) will be blank")
            prepareWarning = getString(R.string.game_lang_missing)
        }
        lateinit var created: GameRunner
        created = GameRunner(
            assets = assets,
            dataDir = GameStorage.dataDir(this),
            dataWinPath = dataWin.absolutePath,
            savesDir = GameStorage.savesDir(this),
            logFile = GameStorage.nativeLogFile(this),
            settings = { settings.runtimeSettings() },
            onExitRequested = {
                runOnUiThread {
                    if (created.hasStarted) {
                        AppLog.write("The game closed itself")
                        finish()
                    } else {
                        AppLog.write("The runner could not start")
                        fatalError = getString(R.string.game_runner_failed)
                    }
                }
            }
        )
        runner = created
        created.start()
        pendingSurface?.let { created.attachSurface(it) }
    }

    override fun onResume() {
        super.onResume()
        hideSystemUi()
        Haptics.configure(settings.haptics, settings.hapticsStrength)
        activityPaused = false
    }

    override fun onPause() {
        super.onPause()
        activityPaused = true
        runner?.setPaused(true)
    }

    override fun onDestroy() {
        super.onDestroy()
        runner?.quit()
        runner = null
    }

    private fun hideSystemUi() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
    }

    // ===[ ControlSink ]===

    override fun key(vk: Int, down: Boolean) {
        logInput("key $vk ${if (down) "down" else "up"}")
        runner?.key(vk, down)
    }

    override fun pad(button: Int, down: Boolean) {
        logInput("pad $button ${if (down) "down" else "up"}")
        runner?.padButton(button, down)
    }

    // The first touches of a session go to the log, so a "the buttons do nothing" report can be answered with facts:
    // either the app sent the press (and the problem is on the game's side) or it never arrived.
    private var inputLogLeft = 40

    private fun logInput(what: String) {
        if (inputLogLeft <= 0) return
        inputLogLeft--
        AppLog.write("input: $what" + if (inputLogLeft == 0) " (further input not logged)" else "")
    }

    override fun axis(axis: Int, value: Float) {
        runner?.padAxis(axis, value)
    }

    // ===[ Physical keyboards and controllers ]===

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (dispatchPhysical(event, true)) return true
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (dispatchPhysical(event, false)) return true
        return super.onKeyUp(keyCode, event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD
        ) {
            if (event.action == MotionEvent.ACTION_MOVE) {
                runner?.padAxis(0, event.getAxisValue(MotionEvent.AXIS_X).coerceIn(-1f, 1f))
                runner?.padAxis(1, event.getAxisValue(MotionEvent.AXIS_Y).coerceIn(-1f, 1f))
                runner?.padAxis(2, event.getAxisValue(MotionEvent.AXIS_Z).coerceIn(-1f, 1f))
                runner?.padAxis(3, event.getAxisValue(MotionEvent.AXIS_RZ).coerceIn(-1f, 1f))
                return true
            }
        }
        return super.onGenericMotionEvent(event)
    }

    /** Sends a physical key or controller button to the game; false when it is not one we map. */
    private fun dispatchPhysical(event: KeyEvent, down: Boolean): Boolean {
        val isGamepad = event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        if (isGamepad) {
            val button = PhysicalInput.gamepadButton(event.keyCode)
            if (button != null) {
                runner?.padButton(button, down)
                return true
            }
        }
        val vk = PhysicalInput.gmlKey(event.keyCode)
        if (vk != null) {
            runner?.key(vk, down)
            if (down) {
                val char = PhysicalInput.character(event)
                if (char != 0) runner?.character(char)
            }
            return true
        }
        return false
    }
}

/** Android key codes → the codes GameMaker's keyboard_* functions use. */
object PhysicalInput {
    fun gmlKey(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> 38
        KeyEvent.KEYCODE_DPAD_DOWN -> 40
        KeyEvent.KEYCODE_DPAD_LEFT -> 37
        KeyEvent.KEYCODE_DPAD_RIGHT -> 39
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER, KeyEvent.KEYCODE_DPAD_CENTER -> 13
        KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_BACK -> 27
        KeyEvent.KEYCODE_SPACE -> 32
        KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT -> 16
        KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT -> 17
        KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_ALT_RIGHT -> 18
        KeyEvent.KEYCODE_TAB -> 9
        KeyEvent.KEYCODE_DEL -> 46
        KeyEvent.KEYCODE_FORWARD_DEL -> 46
        KeyEvent.KEYCODE_MOVE_HOME -> 36
        KeyEvent.KEYCODE_MOVE_END -> 35
        KeyEvent.KEYCODE_PAGE_UP -> 33
        KeyEvent.KEYCODE_PAGE_DOWN -> 34
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 'A'.code + (keyCode - KeyEvent.KEYCODE_A)
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> '0'.code + (keyCode - KeyEvent.KEYCODE_0)
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> 112 + (keyCode - KeyEvent.KEYCODE_F1)
        else -> null
    }

    fun gamepadButton(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> 0
        KeyEvent.KEYCODE_BUTTON_B -> 1
        KeyEvent.KEYCODE_BUTTON_X -> 2
        KeyEvent.KEYCODE_BUTTON_Y -> 3
        KeyEvent.KEYCODE_BUTTON_L1 -> 4
        KeyEvent.KEYCODE_BUTTON_R1 -> 5
        KeyEvent.KEYCODE_BUTTON_L2 -> 6
        KeyEvent.KEYCODE_BUTTON_R2 -> 7
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> 8
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> 9
        KeyEvent.KEYCODE_BUTTON_THUMBL -> 10
        KeyEvent.KEYCODE_BUTTON_THUMBR -> 11
        KeyEvent.KEYCODE_DPAD_UP -> 12
        KeyEvent.KEYCODE_DPAD_DOWN -> 13
        KeyEvent.KEYCODE_DPAD_LEFT -> 14
        KeyEvent.KEYCODE_DPAD_RIGHT -> 15
        KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_HOME -> 16
        else -> null
    }

    fun character(event: KeyEvent): Int {
        val unicode = event.unicodeChar
        if (unicode == 0) return 0
        return try {
            val chars = Character.toChars(unicode)
            if (chars.size == 1) chars[0].code else 0
        } catch (_: Throwable) {
            0
        }
    }
}

/**
 * The pause menu. It is drawn here instead of being an [AlertDialog] because a dialog gets whatever height the
 * platform leaves it, and on a phone held sideways that is almost none: the rows were clipped away and all the
 * player could see was the single "Resume" button, with no way out of the game.
 */
@Composable
private fun PauseScreen(
    settings: AppSettings,
    scheme: ButtonScheme,
    onScheme: (ButtonScheme) -> Unit,
    onResume: () -> Unit,
    onEditControls: () -> Unit,
    onQuickSettings: () -> Unit,
    onToggleControls: () -> Unit,
    onLogs: () -> Unit,
    onQuit: () -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xB3000000))
            .pointerInput(Unit) { detectTapGestures { onResume() } },
        contentAlignment = Alignment.Center
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth(0.86f)
                .widthIn(max = 420.dp)
                .clip(RoundedCornerShape(22.dp))
                .background(Color(0xF216161C))
                .border(1.dp, Color(0x33FFFFFF), RoundedCornerShape(22.dp))
                // A tap on the panel itself must not close the menu through the scrim behind it.
                .pointerInput(Unit) { detectTapGestures { } }
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 14.dp, vertical = 12.dp)
        ) {
            Text(
                str(R.string.pause_title),
                color = Color.White,
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(start = 4.dp, bottom = 8.dp)
            )
            PauseRow(str(R.string.pause_resume), emphasised = true, onClick = onResume)
            PauseRow(str(R.string.pause_edit_controls), onClick = onEditControls)
            PauseRow(
                if (settings.controlsVisible) str(R.string.pause_hide_controls) else str(R.string.pause_show_controls),
                onClick = onToggleControls
            )
            PauseRow(str(R.string.pause_quick_settings), onClick = onQuickSettings)
            PauseRow(str(R.string.pause_logs), onClick = onLogs)

            Text(
                str(R.string.pause_scheme),
                color = Color(0xFF9E9EA7),
                fontSize = 11.sp,
                modifier = Modifier.padding(start = 4.dp, top = 10.dp, bottom = 4.dp)
            )
            Row(Modifier.fillMaxWidth().padding(start = 2.dp)) {
                for (option in ButtonScheme.entries) {
                    FilterChip(
                        selected = option == scheme,
                        onClick = { onScheme(option) },
                        label = { Text(option.brand) },
                        modifier = Modifier.padding(end = 6.dp)
                    )
                }
            }

            Spacer(Modifier.height(10.dp))
            PauseRow(str(R.string.pause_quit), danger = true, onClick = onQuit)
        }
    }
}

@Composable
private fun PauseRow(
    text: String,
    onClick: () -> Unit,
    danger: Boolean = false,
    emphasised: Boolean = false
) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(
                when {
                    danger -> Color(0x33EF5350)
                    emphasised -> Color(0x2EFFFFFF)
                    else -> Color(0x14FFFFFF)
                }
            )
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 12.dp)
    ) {
        Text(
            text = text,
            color = when {
                danger -> Color(0xFFFF8A80)
                emphasised -> Color.White
                else -> Color(0xFFEDEDF2)
            },
            fontSize = 15.sp,
            fontWeight = if (emphasised || danger) FontWeight.SemiBold else FontWeight.Normal
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun QuickSettingsDialog(settings: AppSettings, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(str(R.string.settings_section_graphics)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text(str(R.string.settings_filter), color = Color(0xFFB0B0B8), fontSize = 12.sp)
                FlowRow {
                    FilterChip(
                        selected = settings.filter == VideoFilter.SHARP,
                        onClick = { settings.filter = VideoFilter.SHARP },
                        label = { Text(str(R.string.filter_sharp)) },
                        modifier = Modifier.padding(end = 6.dp, bottom = 4.dp)
                    )
                    FilterChip(
                        selected = settings.filter == VideoFilter.SMOOTH,
                        onClick = { settings.filter = VideoFilter.SMOOTH },
                        label = { Text(str(R.string.filter_smooth)) },
                        modifier = Modifier.padding(end = 6.dp, bottom = 4.dp)
                    )
                    FilterChip(
                        selected = settings.filter == VideoFilter.CRT,
                        onClick = { settings.filter = VideoFilter.CRT },
                        label = { Text(str(R.string.filter_crt)) },
                        modifier = Modifier.padding(bottom = 4.dp)
                    )
                }
                Spacer(Modifier.height(8.dp))
                SettingSwitch(str(R.string.settings_integer_scaling), settings.integerScaling) { settings.integerScaling = it }
                SettingSwitch(str(R.string.settings_widescreen_short), settings.widescreen) { settings.widescreen = it }
                SettingSwitch(str(R.string.settings_fps), settings.showFps) { settings.showFps = it }
                SettingSwitch(str(R.string.settings_haptics_short), settings.haptics) {
                    settings.haptics = it
                    Haptics.configure(it, settings.hapticsStrength)
                }
                Text(
                    str(R.string.quick_crt_hint),
                    color = Color(0xFF888888),
                    fontSize = 11.sp
                )
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(str(R.string.common_close)) } }
    )
}

@Composable
fun SettingSwitch(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, color = Color.White, fontSize = 14.sp, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@Composable
private fun LogDialog(onDismiss: () -> Unit) {
    val context = LocalContext.current
    var tab by remember { mutableStateOf(0) }
    // Read outside remember(): its calculation must not call into composition.
    val emptyLog = str(R.string.log_empty)
    val nothingYet = str(R.string.log_nothing_yet)
    val cutOff = str(R.string.log_truncated)
    val shareTitle = str(R.string.common_share_log)
    val appLog = remember(emptyLog) { AppLog.read().ifEmpty { emptyLog } }
    val nativeLog = remember(nothingYet, cutOff) {
        val file = GameStorage.nativeLogFile(context)
        if (!file.isFile) nothingYet else {
            // A session can write megabytes of engine warnings; the end is what matters.
            val text = file.readText()
            if (text.length > 120_000) cutOff + text.takeLast(120_000) else text
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(str(R.string.log_title)) },
        text = {
            Column {
                Row {
                    FilterChip(
                        selected = tab == 0,
                        onClick = { tab = 0 },
                        label = { Text(str(R.string.log_tab_app)) },
                        modifier = Modifier.padding(end = 6.dp)
                    )
                    FilterChip(
                        selected = tab == 1,
                        onClick = { tab = 1 },
                        label = { Text(str(R.string.log_tab_game)) }
                    )
                }
                Spacer(Modifier.height(8.dp))
                Column(Modifier.height(340.dp).verticalScroll(rememberScrollState())) {
                    Text(
                        text = if (tab == 0) appLog else nativeLog,
                        fontSize = 10.sp,
                        color = Color(0xFFCCCCCC)
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                try {
                    val intent = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_TEXT, appLog + "\n\n===== native =====\n" + nativeLog)
                    }
                    context.startActivity(Intent.createChooser(intent, shareTitle))
                } catch (_: Throwable) {
                }
            }) { Text(str(R.string.common_share)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(str(R.string.common_close)) } }
    )
}
