package ru.avgn8bit

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.unit.IntSize

/**
 * JNI bridge to the Butterscotch runtime (see runtime/src/android/main.c).
 *
 * Threading: every function except [setBaseDir], [setActiveLogFile], the DataWin helpers and the two
 * native-to-Kotlin callbacks must be called from the thread that owns the EGL context
 * ([GameRunner]'s render thread). Input is therefore queued by the UI and drained on that thread.
 */
object Native {
    const val CONTINUE = 0
    const val SHOULD_EXIT = 1
    const val CONTINUE_NO_SWAP = 2

    /** GameMaker's os_windows: the game then behaves like the PC build it was made for. */
    const val OS_WINDOWS = 0

    init {
        System.loadLibrary("avgn8bit")
        init()
    }

    external fun init()
    external fun setBaseDir(path: String)
    external fun setActiveLogFile(path: String?)

    // ===[ Data.win helpers (safe from any thread, no GL needed) ]===
    external fun dataWinParseLight(path: String): Long
    external fun dataWinFree(handle: Long)
    external fun dataWinName(handle: Long): String?
    external fun dataWinDisplayName(handle: Long): String?

    // ===[ Render thread only ]===
    external fun startRunner(dataWinPath: String, savesPath: String, osType: Int, hostFramebuffer: Int): Boolean
    external fun beginFrame()
    external fun onKeyDown(keyCode: Int)
    external fun onKeyUp(keyCode: Int)
    external fun onCharacter(codePoint: Int)
    external fun gamepadConnected(device: Int, name: String?)
    external fun gamepadDisconnected(device: Int)
    external fun gamepadButton(device: Int, button: Int, isDown: Boolean)
    external fun gamepadAxis(device: Int, axis: Int, value: Float)

    /** Which controller the touch pad imitates: 0 = PlayStation, 1 = Xbox. The game draws matching glyphs. */
    external fun setGamepadScheme(scheme: Int)

    /** Draw with the plain program instead of the game's own shaders (the launcher's diagnostic switch). */
    external fun setNoGameShaders(on: Boolean)
    external fun stepAndDraw(winW: Int, winH: Int, deltaSeconds: Float): Int
    external fun getTargetFrameHz(): Int
    external fun stopRunner()
    external fun suspendAudio()
    external fun resumeAudio()
    external fun setWidescreenHackAspectRatio(aspectRatio: Float)
    external fun setNormalizedCursorPosition(x: Float, y: Float)
    external fun setMouseButtonState(button: Int, isDown: Boolean)
    external fun setFreeCamera(panX: Float, panY: Float, zoom: Float)

    /** Window title the game asked for (also the game's name, usually). */
    var title: String? by mutableStateOf(null)
        private set

    /** The game's own "application surface" size, used to letterbox it on the phone's screen. */
    var gameSize: IntSize? by mutableStateOf(null)
        private set

    @JvmStatic
    fun onTitleChanged(title: String) {
        this.title = title
    }

    @JvmStatic
    fun onGameSizeChanged(width: Int, height: Int) {
        gameSize = IntSize(width, height)
    }
}
