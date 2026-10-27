package ru.avgn8bit

import android.content.res.AssetManager
import android.opengl.GLES20
import android.opengl.GLES30
import android.util.Log
import android.view.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File
import java.util.concurrent.LinkedBlockingQueue
import kotlin.math.max
import kotlin.math.min

/** Video filter applied when the finished game frame is put on the screen. */
enum class VideoFilter { SHARP, SMOOTH, CRT }

/** Settings the render loop reads every frame, so changes apply without restarting the game. */
data class RuntimeSettings(
    val filter: VideoFilter = VideoFilter.SHARP,
    val integerScaling: Boolean = false,
    val widescreen: Boolean = false,
    val showFps: Boolean = false,
    val noGameShaders: Boolean = false
)

/**
 * Owns the render thread: EGL, the offscreen frame the runner draws into, the present pass and the input queue.
 *
 * The thread and the EGL context live as long as the game session; the window surface comes and goes with
 * [attachSurface] / [detachSurface] (app backgrounded, screen rotated, ...). The game itself is only torn down
 * by [quit].
 */
class GameRunner(
    private val assets: AssetManager,
    private val dataDir: File,
    private val dataWinPath: String,
    private val savesDir: File,
    private val logFile: File,
    private val settings: () -> RuntimeSettings,
    private val onExitRequested: () -> Unit
) {
    companion object {
        private const val TAG = "GameRunner"

        /** Aspect of the game's own layout: it draws its screen for a 4:3 window. */
        private const val CLASSIC_ASPECT = 4f / 3f
    }

    private sealed interface Command {
        data class Attach(val surface: Surface) : Command
        data class Resize(val width: Int, val height: Int) : Command
        data object Detach : Command
        data class Paused(val paused: Boolean) : Command
        data object Quit : Command
    }

    private sealed interface Input {
        data class Key(val vk: Int, val down: Boolean) : Input
        data class Char(val codepoint: Int) : Input
        data class PadButton(val button: Int, val down: Boolean) : Input
        data class PadAxis(val axis: Int, val value: Float) : Input
        data class PadConnected(val name: String) : Input
        data object PadDisconnected : Input
        data class PadScheme(val scheme: Int) : Input
    }

    private val commands = LinkedBlockingQueue<Command>()
    private val inputs = LinkedBlockingQueue<Input>()
    private val lock = Object()

    private var thread: Thread? = null
    private val egl = GameGL()
    private val shaders = Shaders(assets)

    // Only touched by the render thread.
    private var attachedSurface: Surface? = null
    private var paused = true
    private var quitting = false
    private var gameStarted = false
    private var prepared = false
    // Read by the render thread when the runner starts; written from the UI thread.
    @Volatile private var padScheme = 1
    // Render thread only: the game-shader switch as the runtime has it.
    private var appliedNoGameShaders = false
    private var logOpen = false
    private var fboId = 0
    private var frameTexture = 0
    private var fboWidth = 0
    private var fboHeight = 0
    private var renderX = 0
    private var renderY = 0
    private var renderW = 1
    private var renderH = 1
    // Last "surface -> frame" line written to the app log, so a resize logs once instead of every frame
    private var lastRenderTarget: String? = null
    private var lastFrameNs = 0L
    private var fpsFrames = 0
    private var fpsSinceNs = 0L

    /** Presentable frames per second, for the optional overlay. */
    var fps by mutableStateOf(0f)
        private set

    /** True once the game's first room is up (the loading screen hides on it). */
    var gameRunning by mutableStateOf(false)
        private set

    /** True once the runtime has been started at least once, so "the game closed" can be told from "it never ran". */
    @Volatile
    var hasStarted: Boolean = false
        private set

    fun start() {
        if (thread != null) return
        thread = Thread(::loop, "avgn-render").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun attachSurface(surface: Surface) = post(Command.Attach(surface))
    fun detachSurface() = post(Command.Detach)
    fun updateSurfaceSize(width: Int, height: Int) = post(Command.Resize(width, height))
    fun setPaused(paused: Boolean) = post(Command.Paused(paused))

    fun quit() {
        post(Command.Quit)
        thread?.let { t ->
            try {
                t.join(5000)
            } catch (_: InterruptedException) {
            }
        }
        thread = null
    }

    private fun post(command: Command) {
        commands.offer(command)
        synchronized(lock) { lock.notifyAll() }
    }

    // ===[ Input, callable from the UI thread ]===

    fun key(vk: Int, down: Boolean) = inputs.offer(Input.Key(vk, down))
    fun character(codepoint: Int) = inputs.offer(Input.Char(codepoint))
    fun padButton(button: Int, down: Boolean) = inputs.offer(Input.PadButton(button, down))
    fun padAxis(axis: Int, value: Float) = inputs.offer(Input.PadAxis(axis, value))
    fun padConnected(name: String = "Touch controls") = inputs.offer(Input.PadConnected(name))

    /**
     * 0 = PlayStation, 1 = Xbox: the runtime reports the touch pad under that identity, which is what the game reads
     * to decide whether a controller is connected and which button glyphs to draw.
     */
    fun setGamepadScheme(scheme: Int) {
        padScheme = if (scheme == 1) 1 else 0
        inputs.offer(Input.PadScheme(padScheme))
    }
    fun padDisconnected() = inputs.offer(Input.PadDisconnected)

    // ===[ Render thread ]===

    private fun loop() {
        Log.i(TAG, "Render thread started")
        try {
            while (true) {
                drainCommands()
                if (quitting) break

                val surface = attachedSurface
                if (surface == null || paused) {
                    if (egl.hasSurface) egl.unbindWindow()
                    synchronized(lock) {
                        if (commands.isEmpty() && !quitting) lock.wait(250)
                    }
                    continue
                }

                if (!egl.hasSurface) {
                    if (!egl.bindWindow(surface)) {
                        egl.unbindWindow()
                        synchronized(lock) { lock.wait(500) }
                        continue
                    }
                    lastFrameNs = System.nanoTime()
                }

                renderFrame()
            }
        } catch (t: Throwable) {
            Log.e(TAG, "Render thread crashed", t)
            AppLog.write("Render thread crashed: ${t.stackTraceToString()}")
        } finally {
            try {
                if (gameStarted) Native.stopRunner()
                egl.teardown()
                if (logOpen) {
                    // Closes the runtime's log file. Only ever done when it was actually opened: the native side
                    // closes unconditionally and would crash on a second call.
                    Native.setActiveLogFile(null)
                    logOpen = false
                }
            } catch (t: Throwable) {
                Log.e(TAG, "Teardown failed", t)
            }
            gameRunning = false
            Log.i(TAG, "Render thread finished")
        }
    }

    private fun drainCommands() {
        while (true) {
            val command = commands.poll() ?: return
            when (command) {
                is Command.Attach -> attachedSurface = command.surface
                is Command.Resize -> if (egl.hasSurface) egl.updateSize(command.width, command.height)
                Command.Detach -> {
                    attachedSurface = null
                }
                is Command.Paused -> {
                    paused = command.paused
                    if (paused) {
                        // Apply whatever the UI released before the menu opened, then silence the game.
                        if (gameStarted) drainInputs()
                        inputs.clear()
                        if (gameStarted) Native.suspendAudio()
                    } else if (gameStarted) {
                        Native.resumeAudio()
                    }
                }
                Command.Quit -> quitting = true
            }
        }
    }

    private fun renderFrame() {
        if (!prepared) {
            Native.setBaseDir(dataDir.absolutePath)
            Native.setActiveLogFile(logFile.absolutePath)
            logOpen = true
            prepareGl()
            prepared = true
        }

        if (!gameStarted) {
            AppLog.write("Starting the runner: $dataWinPath (saves: ${savesDir.absolutePath})")
            val started = Native.startRunner(dataWinPath, savesDir.absolutePath, Native.OS_WINDOWS, fboId)
            AppLog.write("startRunner returned $started, game size ${Native.gameSize}")
            if (!started) {
                if (logOpen) {
                    Native.setActiveLogFile(null)
                    logOpen = false
                }
                onExitRequested()
                quitting = true
                return
            }
            gameStarted = true
            hasStarted = true
            gameRunning = true
            // The touch controls are a gamepad as far as the game is concerned. The runtime attaches device 0 itself
            // before the game's first frame (see attachTouchPad in android/main.c); this only says which controller
            // it should claim to be.
            Native.setGamepadScheme(padScheme)
            lastFrameNs = System.nanoTime()
        }

        val now = System.nanoTime()
        val delta = ((now - lastFrameNs) / 1_000_000_000.0).toFloat().coerceIn(0f, 0.1f)
        lastFrameNs = now

        val current = settings()

        // The game's own shaders (the palette swaps enemies, bosses and bullets are drawn with) can be switched off in
        // the launcher: those sprites are then drawn with the plain program, which answers "is a shader of the game
        // drawing this wrong" without another build.
        if (current.noGameShaders != appliedNoGameShaders) {
            appliedNoGameShaders = current.noGameShaders
            Native.setNoGameShaders(current.noGameShaders)
            AppLog.write(if (current.noGameShaders) "Drawn without the game's shaders" else "The game's shaders are back on")
        }
        computeRenderTarget(current.integerScaling, current.widescreen)
        ensureFramebuffer()
        if (current.widescreen) {
            val aspect = egl.width.toFloat() / max(1, egl.height).toFloat()
            Native.setWidescreenHackAspectRatio(aspect)
        } else {
            Native.setWidescreenHackAspectRatio(0f)
        }

        Native.beginFrame()
        drainInputs()

        when (val status = Native.stepAndDraw(renderW, renderH, delta)) {
            Native.CONTINUE -> present(current.filter)
            Native.CONTINUE_NO_SWAP -> Unit
            else -> {
                Log.i(TAG, "Runner asked to exit ($status)")
                quitting = true
                onExitRequested()
                return
            }
        }

        countFps()
        paceFrame()
    }

    private fun drainInputs() {
        while (true) {
            val input = inputs.poll() ?: return
            when (input) {
                is Input.Key -> if (input.down) Native.onKeyDown(input.vk) else Native.onKeyUp(input.vk)
                is Input.Char -> Native.onCharacter(input.codepoint)
                is Input.PadButton -> Native.gamepadButton(0, input.button, input.down)
                is Input.PadAxis -> Native.gamepadAxis(0, input.axis, input.value)
                is Input.PadConnected -> Native.gamepadConnected(0, input.name)
                is Input.PadScheme -> Native.setGamepadScheme(input.scheme)
                Input.PadDisconnected -> Native.gamepadDisconnected(0)
            }
        }
    }

    private fun countFps() {
        fpsFrames++
        val now = System.nanoTime()
        if (fpsSinceNs == 0L) fpsSinceNs = now
        if (now - fpsSinceNs >= 500_000_000L) {
            fps = fpsFrames * 1_000_000_000f / (now - fpsSinceNs)
            fpsFrames = 0
            fpsSinceNs = now
        }
    }

    private fun paceFrame() {
        var hz = 60
        try {
            val target = Native.getTargetFrameHz()
            if (target in 20..240) hz = target
        } catch (_: Throwable) {
        }
        val frameNs = 1_000_000_000L / hz
        val elapsed = System.nanoTime() - lastFrameNs
        val remaining = frameNs - elapsed
        if (remaining > 1_000_000L) {
            synchronized(lock) { lock.wait(remaining / 1_000_000L) }
        }
    }

    // ===[ Presentation ]===

    private fun prepareGl() {
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        frameTexture = textures[0]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, frameTexture)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        val framebuffers = IntArray(1)
        GLES20.glGenFramebuffers(1, framebuffers, 0)
        fboId = framebuffers[0]
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fboId)
        GLES20.glFramebufferTexture2D(
            GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, frameTexture, 0
        )
        ensureFramebuffer(force = true)
        val status = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER)
        check(status == GLES20.GL_FRAMEBUFFER_COMPLETE) { "Framebuffer incomplete: $status" }

        shaders.init()
    }

    private fun computeRenderTarget(integerScaling: Boolean, widescreen: Boolean) {
        val surfaceW = max(1, egl.width)
        val surfaceH = max(1, egl.height)

        if (widescreen) {
            // The game lays itself out for whatever window it is given, so hand it the whole screen.
            renderW = surfaceW
            renderH = surfaceH
            renderX = 0
            renderY = 0
            logRenderTarget(surfaceW, surfaceH)
            return
        }

        // The game's own layout is 4:3. The size it reports (gen8 defaultWindow) comes from the Vita game data and is
        // neither 4:3 nor the aspect of the art, so deriving the frame from it squeezed the picture horizontally.
        var w: Int
        var h: Int
        if (surfaceW.toFloat() / surfaceH.toFloat() > CLASSIC_ASPECT) {
            h = surfaceH
            w = (h * CLASSIC_ASPECT).toInt()
        } else {
            w = surfaceW
            h = (w / CLASSIC_ASPECT).toInt()
        }
        if (integerScaling && w > 0 && h > 0) {
            val scale = min(surfaceW / w, surfaceH / h)
            if (scale >= 1) {
                w *= scale
                h *= scale
            }
        }
        renderW = w.coerceAtLeast(1)
        renderH = h.coerceAtLeast(1)
        renderX = (surfaceW - renderW) / 2
        renderY = (surfaceH - renderH) / 2
        logRenderTarget(surfaceW, surfaceH)
    }

    private fun logRenderTarget(surfaceW: Int, surfaceH: Int) {
        val line = "$surfaceW x $surfaceH -> $renderW x $renderH at $renderX,$renderY"
        if (line == lastRenderTarget) return
        lastRenderTarget = line
        AppLog.write("Render target: $line (game reports ${Native.gameSize})")
    }

    private fun ensureFramebuffer(force: Boolean = false) {
        if (!force && renderW == fboWidth && renderH == fboHeight) return
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, frameTexture)
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, renderW, renderH, 0,
            GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null
        )
        fboWidth = renderW
        fboHeight = renderH
    }

    private fun present(filter: VideoFilter) {
        // The runtime keeps its own idea of the GL state and only touches the driver when the *game* asks for a
        // change (glGpuSetBlendEnable returns at once when its flag already matches), so every piece of state this
        // pass touches has to be handed back exactly as it was found. Leaving the blend disabled, for instance, made
        // the next frame draw every sprite unblended: the transparent texels of a texture page are black, so each
        // sprite came out inside a black rectangle.
        val blendWas = GLES20.glIsEnabled(GLES20.GL_BLEND)
        val scissorWas = GLES20.glIsEnabled(GLES20.GL_SCISSOR_TEST)
        val previous = IntArray(4)
        GLES20.glGetIntegerv(GLES20.GL_VIEWPORT, previous, 0)
        val viewport = previous.copyOf()
        GLES20.glGetIntegerv(GLES20.GL_CURRENT_PROGRAM, previous, 0)
        val program = previous[0]
        GLES30.glGetIntegerv(GLES30.GL_VERTEX_ARRAY_BINDING, previous, 0)
        val vao = previous[0]
        GLES20.glGetIntegerv(GLES20.GL_ACTIVE_TEXTURE, previous, 0)
        val activeTexture = previous[0]
        GLES20.glGetIntegerv(GLES20.GL_TEXTURE_BINDING_2D, previous, 0)
        val texture = previous[0]
        GLES20.glGetIntegerv(GLES20.GL_FRAMEBUFFER_BINDING, previous, 0)
        val framebuffer = previous[0]
        val clearColor = FloatArray(4)
        GLES20.glGetFloatv(GLES20.GL_COLOR_CLEAR_VALUE, clearColor, 0)

        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
        GLES20.glViewport(0, 0, egl.width, egl.height)
        GLES20.glDisable(GLES20.GL_BLEND)
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glViewport(renderX, renderY, renderW, renderH)

        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, frameTexture)
        val param = if (filter == VideoFilter.SHARP) GLES20.GL_NEAREST else GLES20.GL_LINEAR
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, param)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, param)

        GLES30.glBindVertexArray(0)
        if (filter == VideoFilter.CRT) {
            shaders.drawCrt(frameTexture, renderW, renderH)
        } else {
            shaders.drawBlit(frameTexture)
        }
        egl.swapBuffers()

        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, framebuffer)
        GLES20.glViewport(viewport[0], viewport[1], viewport[2], viewport[3])
        GLES20.glUseProgram(program)
        GLES30.glBindVertexArray(vao)
        GLES20.glActiveTexture(activeTexture)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
        GLES20.glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3])
        if (blendWas) GLES20.glEnable(GLES20.GL_BLEND) else GLES20.glDisable(GLES20.GL_BLEND)
        if (scissorWas) GLES20.glEnable(GLES20.GL_SCISSOR_TEST) else GLES20.glDisable(GLES20.GL_SCISSOR_TEST)
    }

    /**
     * A tiny shader helper: both programs draw a fullscreen triangle generated from gl_VertexID, so no
     * vertex buffers are involved.
     */
    private class Shaders(private val assets: AssetManager) {
        private var blit = 0
        private var crt = 0
        private var blitTexture = -1
        private var crtTexture = -1
        private var crtResolution = -1
        private var crtCurvature = -1
        private var crtAberration = -1
        private var crtHalation = -1
        private var crtScanlines = -1
        private var crtMask = -1
        private var crtVignette = -1
        private val vertexSource by lazy { assets.open("shaders/blit.vsh").readBytes().decodeToString() }

        fun init() {
            blit = build(vertexSource, assets.open("shaders/blit.fsh").readBytes().decodeToString())
            crt = build(vertexSource, assets.open("shaders/crt.fsh").readBytes().decodeToString())
            blitTexture = GLES20.glGetUniformLocation(blit, "uTexture")
            crtTexture = GLES20.glGetUniformLocation(crt, "uTexture")
            crtResolution = GLES20.glGetUniformLocation(crt, "uResolution")
            crtCurvature = GLES20.glGetUniformLocation(crt, "uCurvature")
            crtAberration = GLES20.glGetUniformLocation(crt, "uAberration")
            crtHalation = GLES20.glGetUniformLocation(crt, "uHalation")
            crtScanlines = GLES20.glGetUniformLocation(crt, "uScanlines")
            crtMask = GLES20.glGetUniformLocation(crt, "uMask")
            crtVignette = GLES20.glGetUniformLocation(crt, "uVignette")
        }

        fun drawBlit(texture: Int) {
            GLES20.glUseProgram(blit)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
            GLES20.glUniform1i(blitTexture, 0)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3)
        }

        fun drawCrt(texture: Int, width: Int, height: Int) {
            GLES20.glUseProgram(crt)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
            GLES20.glUniform1i(crtTexture, 0)
            GLES20.glUniform2f(crtResolution, width.toFloat(), height.toFloat())
            GLES20.glUniform1f(crtCurvature, 0.10f)
            GLES20.glUniform1f(crtAberration, 0.6f)
            GLES20.glUniform1f(crtHalation, 0.6f)
            GLES20.glUniform1f(crtScanlines, 0.5f)
            GLES20.glUniform1f(crtMask, 0.5f)
            GLES20.glUniform1f(crtVignette, 0.35f)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3)
        }

        private fun build(vertex: String, fragment: String): Int {
            val vs = compile(GLES20.GL_VERTEX_SHADER, vertex)
            val fs = compile(GLES20.GL_FRAGMENT_SHADER, fragment)
            val program = GLES20.glCreateProgram()
            GLES20.glAttachShader(program, vs)
            GLES20.glAttachShader(program, fs)
            GLES20.glLinkProgram(program)
            val status = IntArray(1)
            GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
            check(status[0] == GLES20.GL_TRUE) { "Shader link failed: ${GLES20.glGetProgramInfoLog(program)}" }
            GLES20.glDeleteShader(vs)
            GLES20.glDeleteShader(fs)
            return program
        }

        private fun compile(type: Int, source: String): Int {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, source)
            GLES20.glCompileShader(shader)
            val status = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
            check(status[0] == GLES20.GL_TRUE) { "Shader compile failed: ${GLES20.glGetShaderInfoLog(shader)}\n$source" }
            return shader
        }
    }
}
