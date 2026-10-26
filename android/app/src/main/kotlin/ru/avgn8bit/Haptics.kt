package ru.avgn8bit

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/** Short ticks for on-screen buttons, with the strength the user picked in the settings. */
object Haptics {
    private var vibrator: Vibrator? = null
    private var enabled = true
    private var strength = 55

    fun init(context: Context) {
        vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val manager = context.getSystemService(VibratorManager::class.java)
            manager?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
    }

    fun configure(enabled: Boolean, strength: Int) {
        this.enabled = enabled
        this.strength = strength.coerceIn(1, 255)
    }

    fun tick(durationMs: Long = 12) {
        if (!enabled) return
        val v = vibrator ?: return
        if (!v.hasVibrator()) return
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                v.vibrate(VibrationEffect.createOneShot(durationMs, strength))
            } else {
                @Suppress("DEPRECATION")
                v.vibrate(durationMs)
            }
        } catch (_: Throwable) {
        }
    }
}
