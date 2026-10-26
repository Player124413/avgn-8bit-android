package ru.avgn8bit

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File

enum class OrientationMode(val id: String, val label: String) {
    LANDSCAPE("landscape", "Ландшафт"),
    SENSOR_LANDSCAPE("sensor_landscape", "Ландшафт (по датчику)"),
    PORTRAIT("portrait", "Портрет"),
    AUTO("auto", "Как повернут телефон");

    companion object {
        fun from(id: String?): OrientationMode = entries.firstOrNull { it.id == id } ?: SENSOR_LANDSCAPE
    }
}

/**
 * Everything the user can change, backed by SharedPreferences. Compose reads the fields directly, so a switch
 * takes effect on the next frame without any plumbing.
 */
class AppSettings(context: Context) {
    private val prefs: SharedPreferences = context.getSharedPreferences("avgn8bit", Context.MODE_PRIVATE)

    var filter by mutableStateOf(VideoFilter.entries.firstOrNull { it.name == prefs.getString("filter", null) } ?: VideoFilter.SHARP)
        private set
    var integerScaling by mutableStateOf(prefs.getBoolean("integerScaling", false))
        private set
    var widescreen by mutableStateOf(prefs.getBoolean("widescreen", false))
        private set
    var showFps by mutableStateOf(prefs.getBoolean("showFps", false))
        private set

    var controlsVisible by mutableStateOf(prefs.getBoolean("controlsVisible", true))
        private set
    var controlsOpacity by mutableStateOf(prefs.getFloat("controlsOpacity", 1f))
        private set
    var controlsScale by mutableStateOf(prefs.getFloat("controlsScale", 1f))
        private set
    var haptics by mutableStateOf(prefs.getBoolean("haptics", true))
        private set
    var hapticsStrength by mutableStateOf(prefs.getInt("hapticsStrength", 55))
        private set

    var orientation by mutableStateOf(OrientationMode.from(prefs.getString("orientation", null)))
        private set
    var keepScreenOn by mutableStateOf(prefs.getBoolean("keepScreenOn", true))
        private set
    var fastGraphics by mutableStateOf(prefs.getBoolean("fastGraphics", true))
        private set
    var hideSystemBars by mutableStateOf(prefs.getBoolean("hideSystemBars", true))
        private set
    var importedFolderName by mutableStateOf(prefs.getString("importedFolder", null))
        private set
    var includeVideosOnImport by mutableStateOf(prefs.getBoolean("includeVideos", true))
        private set

    fun setFilter(value: VideoFilter) { filter = value; prefs.edit().putString("filter", value.name).apply() }
    fun setIntegerScaling(value: Boolean) { integerScaling = value; prefs.edit().putBoolean("integerScaling", value).apply() }
    fun setWidescreen(value: Boolean) { widescreen = value; prefs.edit().putBoolean("widescreen", value).apply() }
    fun setShowFps(value: Boolean) { showFps = value; prefs.edit().putBoolean("showFps", value).apply() }

    fun setControlsVisible(value: Boolean) { controlsVisible = value; prefs.edit().putBoolean("controlsVisible", value).apply() }
    fun setControlsOpacity(value: Float) { controlsOpacity = value; prefs.edit().putFloat("controlsOpacity", value).apply() }
    fun setControlsScale(value: Float) { controlsScale = value; prefs.edit().putFloat("controlsScale", value).apply() }
    fun setHaptics(value: Boolean) { haptics = value; prefs.edit().putBoolean("haptics", value).apply() }
    fun setHapticsStrength(value: Int) { hapticsStrength = value; prefs.edit().putInt("hapticsStrength", value).apply() }

    fun setOrientation(value: OrientationMode) { orientation = value; prefs.edit().putString("orientation", value.id).apply() }
    fun setKeepScreenOn(value: Boolean) { keepScreenOn = value; prefs.edit().putBoolean("keepScreenOn", value).apply() }
    fun setFastGraphics(value: Boolean) { fastGraphics = value; prefs.edit().putBoolean("fastGraphics", value).apply() }
    fun setHideSystemBars(value: Boolean) { hideSystemBars = value; prefs.edit().putBoolean("hideSystemBars", value).apply() }
    fun setImportedFolderName(value: String?) { importedFolderName = value; prefs.edit().putString("importedFolder", value).apply() }
    fun setIncludeVideosOnImport(value: Boolean) { includeVideosOnImport = value; prefs.edit().putBoolean("includeVideos", value).apply() }

    fun runtimeSettings() = RuntimeSettings(
        filter = filter,
        integerScaling = integerScaling,
        widescreen = widescreen,
        showFps = showFps
    )

    /**
     * The runtime carries the optimizations the PS Vita port added to the renderer (texture cache, opaque batches,
     * CRT baking, palette shaders). They are on by default; each one can be turned off by creating a marker file of
     * the matching name next to data.win, which is what the "accurate" mode does when a device renders something
     * wrong.
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

    private companion object {
        /** Marker files the renderer checks (src/gl/gl_renderer.c, BS_VITA_PATH). */
        val ACCURATE_MARKERS = listOf(
            "nocrtbake", "nocrtbakesf", "nocrtfast", "nocrtgrid", "nocrthalfglow", "nocrtmerge",
            "nocrtfinalglow", "nodeferclear", "nodirectfinal", "nofastdefault", "nofastpal",
            "noopaque", "noopaquelinear", "nopalcolor", "nopalswapfast", "notexcache", "notexprefetch"
        )
    }
}
