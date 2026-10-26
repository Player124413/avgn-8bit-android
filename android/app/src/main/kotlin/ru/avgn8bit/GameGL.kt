package ru.avgn8bit

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.util.Log
import android.view.Surface

/**
 * Self-managed EGL. The context outlives the window surface on purpose: the game's textures, shaders and
 * surfaces must survive the app being backgrounded (the window goes away, the game does not).
 *
 * The GL context is per-thread, so every call must happen on the render thread.
 *
 * Reworked from the Butterscotch Android frontend (AGPL-3.0); see android/NOTICE.md.
 */
class GameGL {
    companion object {
        private const val TAG = "GameGL"
    }

    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var config: EGLConfig? = null
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var surface: EGLSurface = EGL14.EGL_NO_SURFACE

    var width: Int = 1
        private set
    var height: Int = 1
        private set

    val hasSurface: Boolean
        get() = surface !== EGL14.EGL_NO_SURFACE

    private fun initOnce(): Boolean {
        if (context !== EGL14.EGL_NO_CONTEXT) return true

        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display === EGL14.EGL_NO_DISPLAY) {
            Log.e(TAG, "eglGetDisplay failed")
            return false
        }
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
            Log.e(TAG, "eglInitialize failed: 0x${EGL14.eglGetError().toString(16)}")
            return false
        }

        val cfgAttribs = intArrayOf(
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_RENDERABLE_TYPE, 0x0040, // EGL_OPENGL_ES3_BIT
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_DEPTH_SIZE, 0,
            EGL14.EGL_STENCIL_SIZE, 0,
            EGL14.EGL_NONE
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfigs = IntArray(1)
        if (!EGL14.eglChooseConfig(display, cfgAttribs, 0, configs, 0, 1, numConfigs, 0) || numConfigs[0] < 1) {
            Log.e(TAG, "eglChooseConfig failed: 0x${EGL14.eglGetError().toString(16)}")
            return false
        }
        config = configs[0]

        val ctxAttribs = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE)
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT, ctxAttribs, 0)
        if (context === EGL14.EGL_NO_CONTEXT) {
            Log.e(TAG, "eglCreateContext failed: 0x${EGL14.eglGetError().toString(16)}")
            return false
        }
        return true
    }

    fun bindWindow(androidSurface: Surface): Boolean {
        if (!initOnce()) return false
        if (hasSurface) return true

        surface = EGL14.eglCreateWindowSurface(display, config, androidSurface, intArrayOf(EGL14.EGL_NONE), 0)
        if (surface === EGL14.EGL_NO_SURFACE) {
            Log.e(TAG, "eglCreateWindowSurface failed: 0x${EGL14.eglGetError().toString(16)}")
            return false
        }
        if (!EGL14.eglMakeCurrent(display, surface, surface, context)) {
            Log.e(TAG, "eglMakeCurrent failed: 0x${EGL14.eglGetError().toString(16)}")
            return false
        }

        val w = IntArray(1)
        val h = IntArray(1)
        EGL14.eglQuerySurface(display, surface, EGL14.EGL_WIDTH, w, 0)
        EGL14.eglQuerySurface(display, surface, EGL14.EGL_HEIGHT, h, 0)
        width = w[0].coerceAtLeast(1)
        height = h[0].coerceAtLeast(1)
        Log.i(TAG, "EGL surface bound: ${width}x$height")
        return true
    }

    fun unbindWindow() {
        if (!hasSurface) return
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, context)
        EGL14.eglDestroySurface(display, surface)
        surface = EGL14.EGL_NO_SURFACE
    }

    fun updateSize(newWidth: Int, newHeight: Int) {
        if (newWidth > 0) width = newWidth
        if (newHeight > 0) height = newHeight
    }

    fun swapBuffers() {
        if (!hasSurface) return
        if (!EGL14.eglSwapBuffers(display, surface)) {
            val err = EGL14.eglGetError()
            if (err != EGL14.EGL_BAD_SURFACE && err != EGL14.EGL_BAD_NATIVE_WINDOW) {
                Log.e(TAG, "eglSwapBuffers failed: 0x${err.toString(16)}")
            }
        }
    }

    fun teardown() {
        if (hasSurface) {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, context)
            EGL14.eglDestroySurface(display, surface)
            surface = EGL14.EGL_NO_SURFACE
        }
        if (context !== EGL14.EGL_NO_CONTEXT) {
            EGL14.eglDestroyContext(display, context)
            context = EGL14.EGL_NO_CONTEXT
        }
        if (display !== EGL14.EGL_NO_DISPLAY) {
            EGL14.eglTerminate(display)
            display = EGL14.EGL_NO_DISPLAY
        }
    }
}
