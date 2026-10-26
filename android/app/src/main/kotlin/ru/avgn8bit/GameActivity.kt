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
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
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
import androidx.compose.ui.graphics.Color
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
import ru.avgn8bit.controls.ControlEditorScreen
import ru.avgn8bit.controls.ControlLayout
import ru.avgn8bit.controls.ControlOverlay
import ru.avgn8bit.controls.ControlSink
import ru.avgn8bit.controls.ControlsStore
import ru.avgn8bit.controls.releaseAllControls
import ru.avgn8bit.ui.AvgnTheme

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
    private var prepareText by mutableStateOf("Загрузка…")
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
            prepareText = if (bundled) "Распаковываю встроенную игру…" else "Готовлю файлы игры…"
            Thread(::prepareAndStart, "avgn-prepare").start()
        }

        setContent {
            AvgnTheme {
                val configuration = LocalConfiguration.current
                val isPortrait = configuration.screenHeightDp > configuration.screenWidthDp
                var layout by remember(isPortrait) { mutableStateOf(controls.load(isPortrait)) }
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
                                    progress != null -> "$prepareText ${(progress * 100).toInt()}%"
                                    prepareText != "Загрузка…" -> prepareText
                                    else -> "Загрузка игры…"
                                },
                                color = Color(0xFFCCCCCC)
                            )
                            Spacer(Modifier.height(8.dp))
                            Text(
                                "Первый запуск может занять до минуты",
                                color = Color(0xFF888888),
                                fontSize = 12.sp
                            )
                        }
                    }

                    if (menuOpen) {
                        InGameMenu(
                            settings = settings,
                            onResume = { menuOpen = false },
                            onEditControls = { menuOpen = false; editing = true },
                            onQuickSettings = { menuOpen = false; quickSettings = true },
                            onToggleControls = { settings.setControlsVisible(!settings.controlsVisible) },
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
                            title = if (isPortrait) "Управление (портрет)" else "Управление (ландшафт)"
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
                            title = { Text("Игра не запустилась") },
                            text = { Text(message) },
                            confirmButton = {
                                TextButton(onClick = {
                                    fatalError = null
                                    finish()
                                }) { Text("Выход") }
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
                runOnUiThread { fatalError = "Файлы игры не найдены. Импортируйте их на главном экране." }
                return
            }
            val written = BundledGame.install(
                this,
                onProgress = { _, fraction ->
                    runOnUiThread {
                        prepareText = "Распаковываю встроенную игру…"
                        prepareProgress = fraction * 0.5f
                    }
                },
                shouldStop = { isFinishing || isDestroyed }
            )
            unpacked = true
            AppLog.write("Unpacked the bundled game: ${written / 1048576} MB")
            base = GameStorage.rawDataWin(this) ?: GameStorage.patchedDataWin(this)
            if (base == null) {
                runOnUiThread { fatalError = "Не удалось распаковать встроенные файлы игры. Подробности в логе." }
                return
            }
        }

        val offset = if (unpacked) 0.5f else 0f
        runOnUiThread { prepareText = "Собираю data_vita.win…" }
        val result = GameStorage.ensurePatched(this) { fraction ->
            runOnUiThread { prepareProgress = offset + fraction * (1f - offset) }
        }
        val patched = result.output
        val source = base
        runOnUiThread {
            startGame(if (result.ok && patched != null) patched else source)
            prepareProgress = null
            if (!result.ok) {
                AppLog.write("Starting on the unpatched data.win: ${result.message}")
                prepareWarning = result.message
            }
        }
    }

    private fun startGame(dataWin: File) {
        AppLog.write("Starting the game: ${dataWin.absolutePath} (${dataWin.length()} bytes)")
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
                        fatalError = "Игра не запустилась. Подробности в логе (меню «Логи»).\n\n" +
                            "Обычно это значит, что файлы игры не подходят этой версии порта: нужны data.win, " +
                            "шрифты .ttf и папки Lang, Attract, Cutscenes из Steam-версии игры."
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

    override fun key(vk: Int, down: Boolean) = runner?.key(vk, down) ?: Unit
    override fun pad(button: Int, down: Boolean) = runner?.padButton(button, down) ?: Unit
    override fun axis(axis: Int, value: Float) = runner?.padAxis(axis, value) ?: Unit

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

@Composable
private fun InGameMenu(
    settings: AppSettings,
    onResume: () -> Unit,
    onEditControls: () -> Unit,
    onQuickSettings: () -> Unit,
    onToggleControls: () -> Unit,
    onLogs: () -> Unit,
    onQuit: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onResume,
        title = { Text("Пауза") },
        text = {
            Column {
                MenuRow("Продолжить игру", onResume)
                MenuRow("Настроить управление", onEditControls)
                MenuRow(if (settings.controlsVisible) "Скрыть кнопки" else "Показать кнопки", onToggleControls)
                MenuRow("Графика и звук", onQuickSettings)
                MenuRow("Логи", onLogs)
                MenuRow("Выйти из игры", onQuit, danger = true)
            }
        },
        confirmButton = { TextButton(onClick = onResume) { Text("Продолжить") } }
    )
}

@Composable
private fun MenuRow(text: String, onClick: () -> Unit, danger: Boolean = false) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(text, color = if (danger) Color(0xFFEF9A9A) else Color.White, fontSize = 16.sp)
    }
}

@Composable
fun QuickSettingsDialog(settings: AppSettings, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Графика") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
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
                Spacer(Modifier.height(8.dp))
                SettingSwitch("Целочисленное масштабирование", settings.integerScaling) { settings.setIntegerScaling(it) }
                SettingSwitch("Широкий экран (больше видно)", settings.widescreen) { settings.setWidescreen(it) }
                SettingSwitch("Счётчик FPS", settings.showFps) { settings.setShowFps(it) }
                SettingSwitch("Вибрация кнопок", settings.haptics) {
                    settings.setHaptics(it)
                    Haptics.configure(it, settings.hapticsStrength)
                }
                Text(
                    "CRT рисует поверх игры дополнительный эффект телевизора. Игра уже рисует свой — включайте, если нравится.",
                    color = Color(0xFF888888),
                    fontSize = 11.sp
                )
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Закрыть") } }
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
    val appLog = remember { AppLog.read() }
    val nativeLog = remember {
        val file = GameStorage.nativeLogFile(context)
        if (!file.isFile) "Рантайм ещё ничего не записал." else {
            // A session can write megabytes of engine warnings; the end is what matters.
            val text = file.readText()
            if (text.length > 120_000) "…(начало обрезано)…\n" + text.takeLast(120_000) else text
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Лог") },
        text = {
            Column {
                Row {
                    FilterChip(
                        selected = tab == 0,
                        onClick = { tab = 0 },
                        label = { Text("Приложение") },
                        modifier = Modifier.padding(end = 6.dp)
                    )
                    FilterChip(
                        selected = tab == 1,
                        onClick = { tab = 1 },
                        label = { Text("Игра") }
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
                    context.startActivity(Intent.createChooser(intent, "Поделиться логом"))
                } catch (_: Throwable) {
                }
            }) { Text("Поделиться") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Закрыть") } }
    )
}
