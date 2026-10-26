package ru.avgn8bit

import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The log the game itself writes (native side) plus everything the app wants to remember about a session.
 * Kept in the app's files dir, shown in the in-game Logs dialog and shareable from there.
 */
object AppLog {
    private const val TAG = "AVGN8bit"
    private const val MAX_BYTES = 2L * 1024 * 1024

    @Volatile
    private var file: File? = null
    private val stamp = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    fun init(target: File) {
        synchronized(this) {
            file = target
            target.parentFile?.mkdirs()
            if (target.exists() && target.length() > 0) target.appendText("\n=== new session ===\n")
        }
    }

    val path: String?
        get() = file?.absolutePath

    fun write(message: String) {
        Log.i(TAG, message)
        synchronized(this) {
            val f = file ?: return
            try {
                if (f.length() > MAX_BYTES) {
                    val rotated = File(f.parentFile, "${f.name}.1")
                    rotated.delete()
                    f.renameTo(rotated)
                }
                f.appendText("[${stamp.format(Date())}] $message\n")
            } catch (_: Throwable) {
                // Logging must never take the game down.
            }
        }
    }

    /** The whole log, or an empty string when there is nothing to show yet (the UI names that case). */
    fun read(): String = try {
        file?.takeIf { it.exists() }?.readText().orEmpty()
    } catch (_: Throwable) {
        ""
    }

    fun clear() {
        synchronized(this) {
            try {
                file?.writeText("")
            } catch (_: Throwable) {
            }
        }
    }
}
