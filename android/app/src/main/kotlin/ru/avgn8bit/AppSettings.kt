package ru.avgn8bit

import android.content.Context
import android.content.SharedPreferences
import androidx.annotation.StringRes
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File

/** [id] is what is written to the preferences; [labelRes] is what the settings screen shows. */
enum class OrientationMode(val id: String, @StringRes val labelRes: Int) {
    LANDSCAPE("landscape", R.string.orientation_landscape),
    SENSOR_LANDSCAPE("sensor_landscape", R.string.orientation_sensor_landscape),
    PORTRAIT("portrait", R.string.orientation_portrait),
    AUTO("auto", R.string.orientation_auto);

    companion object {
        fun from(id: String?): OrientationMode = entries.firstOrNull { it.id == id } ?: SENSOR_LANDSCAPE
    }
}

/**
 * Everything the user can change, backed by SharedPreferences.
 *
 * Every property is a Compose state, so a switch takes effect on the next frame, and its setter writes through to
 * the preferences immediately (there is no "save" button, and the game can be killed at any time by the system).
 */
class AppSettings(context: Context) {
    private val prefs: SharedPreferences = context.getSharedPreferences("avgn8bit", Context.MODE_PRIVATE)

    // ===[ Graphics ]===

    private var filterState by mutableStateOf(
        VideoFilter.entries.firstOrNull { it.name == prefs.getString("filter", null) } ?: VideoFilter.SHARP
    )
    var filter: VideoFilter
        get() = filterState
        set(value) {
            filterState = value
            putString("filter", value.name)
        }

    private var integerScalingState by mutableStateOf(prefs.getBoolean("integerScaling", false))
    var integerScaling: Boolean
        get() = integerScalingState
        set(value) {
            integerScalingState = value
            putBoolean("integerScaling", value)
        }

    private var widescreenState by mutableStateOf(prefs.getBoolean("widescreen", false))
    var widescreen: Boolean
        get() = widescreenState
        set(value) {
            widescreenState = value
            putBoolean("widescreen", value)
        }

    private var showFpsState by mutableStateOf(prefs.getBoolean("showFps", false))
    var showFps: Boolean
        get() = showFpsState
        set(value) {
            showFpsState = value
            putBoolean("showFps", value)
        }

    /**
     * The renderer keeps the Vita port's fast paths (texture cache, opaque batches, CRT baking, palette shaders).
     * They are what makes the game run on a phone; turning them off creates the marker files the runtime checks for
     * and falls back to the plain paths, which is the first thing to try when something renders wrong.
     */
    private var fastGraphicsState by mutableStateOf(prefs.getBoolean("fastGraphics", true))
    var fastGraphics: Boolean
        get() = fastGraphicsState
        set(value) {
            fastGraphicsState = value
            putBoolean("fastGraphics", value)
        }

    // ===[ Touch controls ]===

    private var controlsVisibleState by mutableStateOf(prefs.getBoolean("controlsVisible", true))
    var controlsVisible: Boolean
        get() = controlsVisibleState
        set(value) {
            controlsVisibleState = value
            putBoolean("controlsVisible", value)
        }

    private var controlsOpacityState by mutableStateOf(prefs.getFloat("controlsOpacity", 1f))
    var controlsOpacity: Float
        get() = controlsOpacityState
        set(value) {
            controlsOpacityState = value
            putFloat("controlsOpacity", value)
        }

    private var controlsScaleState by mutableStateOf(prefs.getFloat("controlsScale", 1f))
    var controlsScale: Float
        get() = controlsScaleState
        set(value) {
            controlsScaleState = value
            putFloat("controlsScale", value)
        }

    private var hapticsState by mutableStateOf(prefs.getBoolean("haptics", true))
    var haptics: Boolean
        get() = hapticsState
        set(value) {
            hapticsState = value
            putBoolean("haptics", value)
        }

    private var hapticsStrengthState by mutableStateOf(prefs.getInt("hapticsStrength", 55))
    var hapticsStrength: Int
        get() = hapticsStrengthState
        set(value) {
            hapticsStrengthState = value
            putInt("hapticsStrength", value)
        }

    // ===[ Screen ]===

    private var orientationState by mutableStateOf(OrientationMode.from(prefs.getString("orientation", null)))
    var orientation: OrientationMode
        get() = orientationState
        set(value) {
            orientationState = value
            putString("orientation", value.id)
        }

    private var keepScreenOnState by mutableStateOf(prefs.getBoolean("keepScreenOn", true))
    var keepScreenOn: Boolean
        get() = keepScreenOnState
        set(value) {
            keepScreenOnState = value
            putBoolean("keepScreenOn", value)
        }

    private var hideSystemBarsState by mutableStateOf(prefs.getBoolean("hideSystemBars", true))
    var hideSystemBars: Boolean
        get() = hideSystemBarsState
        set(value) {
            hideSystemBarsState = value
            putBoolean("hideSystemBars", value)
        }

    // ===[ Files ]===

    private var importedFolderNameState by mutableStateOf(prefs.getString("importedFolder", null))
    var importedFolderName: String?
        get() = importedFolderNameState
        set(value) {
            importedFolderNameState = value
            putString("importedFolder", value)
        }

    private var includeVideosOnImportState by mutableStateOf(prefs.getBoolean("includeVideos", true))
    var includeVideosOnImport: Boolean
        get() = includeVideosOnImportState
        set(value) {
            includeVideosOnImportState = value
            putBoolean("includeVideos", value)
        }

    fun runtimeSettings() = RuntimeSettings(
        filter = filter,
        integerScaling = integerScaling,
        widescreen = widescreen,
        showFps = showFps
    )

    /**
     * The runtime carries the optimizations the PS Vita port added to the renderer. Each one can be turned off by
     * creating a marker file of the matching name next to data.win, which is what the "accurate" mode does.
     */
    fun applyGraphicsMarkers(dataDir: File) {
        dataDir.mkdirs()
        for (marker in ACCURATE_MARKERS) {
            val file = File(dataDir, marker)
            if (fastGraphics) {
                if (file.exists()) file.delete()
            } else if (!file.exists()) {
                try {
                    file.createNewFile()
                } catch (_: Throwable) {
                    // A read-only data folder only means the markers cannot be written; the game still runs.
                }
            }
        }
    }

    private fun putString(key: String, value: String?) {
        prefs.edit().putString(key, value).apply()
    }

    private fun putBoolean(key: String, value: Boolean) {
        prefs.edit().putBoolean(key, value).apply()
    }

    private fun putFloat(key: String, value: Float) {
        prefs.edit().putFloat(key, value).apply()
    }

    private fun putInt(key: String, value: Int) {
        prefs.edit().putInt(key, value).apply()
    }

    private companion object {
        /** Marker files the renderer checks (src/gl/gl_renderer.c, BS_VITA_PATH). */
        val ACCURATE_MARKERS = listOf(
            "nocrtbake", "nocrtbakesf", "nocrtfast", "nocrtgrid", "nocrthalfglow", "nocrtmerge",
            "nocrtfinalglow", "nodeferclear", "nodirectfinal", "nofastdefault", "nofastpal",
            "noopaque", "noopaquelinear", "nopalcolor", "nopalswapfast", "notexcache", "notexprefetch"
        )
    }
}
