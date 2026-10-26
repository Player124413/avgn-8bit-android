package ru.avgn8bit

import android.content.Context
import java.io.File

/**
 * Optional "everything in one APK" mode.
 *
 * If the app was built with a game folder baked into the assets (`assets/game/…`), the files are still not usable
 * straight from the APK: the runtime opens data.win with regular file calls, writes its texture cache next to it and
 * expects the fonts at relative paths. So on the first launch the assets are unpacked into the app's data folder
 * once, and from then on the app behaves exactly like after a manual import.
 *
 * Builds without the bundled folder simply have nothing here: [isAvailable] is false and the app asks the user to
 * import files, as usual.
 */
object BundledGame {
    private const val ASSET_ROOT = "game"
    private const val MARKER = ".bundled"

    private val PREFERRED = listOf(
        "data_vita.win", "data.win", "Silver.ttf", "LanaPixel.ttf", "Probly8-CJK.ttf",
        "Lang", "Attract", "Cutscenes"
    )

    /** True when this APK has game files inside. */
    fun isAvailable(context: Context): Boolean = try {
        !context.assets.list(ASSET_ROOT).isNullOrEmpty()
    } catch (_: Throwable) {
        false
    }

    /** True once the assets were unpacked (or the user imported the files himself). */
    fun isInstalled(context: Context): Boolean =
        File(GameStorage.dataDir(context), MARKER).isFile || GameStorage.isReady(context)

    /** Total size of the bundled files, for the "already inside the APK" card. Best effort. */
    fun bundledBytes(context: Context): Long {
        var total = 0L
        for (path in listAssetFiles(context, ASSET_ROOT)) {
            total += try {
                context.assets.openFd(path).use { it.length }
            } catch (_: Throwable) {
                0L
            }
        }
        return total
    }

    /**
     * Unpacks `assets/game` into the app's data folder. Returns the number of bytes written. [shouldStop] lets the
     * caller abort (the activity went away).
     */
    fun install(
        context: Context,
        onProgress: (String, Float) -> Unit = { _, _ -> },
        shouldStop: () -> Boolean = { false }
    ): Long {
        val target = GameStorage.dataDir(context)
        target.mkdirs()
        val files = listAssetFiles(context, ASSET_ROOT)
        val sizes = HashMap<String, Long>()
        var total = 0L
        for (path in files) {
            val size = try {
                context.assets.openFd(path).use { it.length }
            } catch (_: Throwable) {
                0L
            }
            sizes[path] = size
            total += size
        }

        var copied = 0L
        for (path in files) {
            if (shouldStop()) return copied
            val relative = path.removePrefix("$ASSET_ROOT/")
            val out = File(target, relative)
            out.parentFile?.mkdirs()
            val fraction = if (total > 0) (copied.toFloat() / total).coerceIn(0f, 1f) else 0f
            onProgress(relative, fraction)
            context.assets.open(path).use { input ->
                out.outputStream().use { output -> copied += input.copyTo(output, 1 shl 16) }
            }
        }
        File(target, MARKER).writeText("$copied")
        AppLog.write("Unpacked the bundled game: ${files.size} files, ${copied / 1048576} MB")
        onProgress("", 1f)
        return copied
    }

    private fun listAssetFiles(context: Context, dir: String): List<String> {
        val children = try {
            context.assets.list(dir) ?: return emptyList()
        } catch (_: Throwable) {
            return emptyList()
        }
        if (children.isEmpty()) return emptyList()
        val out = ArrayList<String>()
        for (child in children) {
            val path = "$dir/$child"
            val nested = try {
                context.assets.list(path)
            } catch (_: Throwable) {
                null
            }
            if (nested.isNullOrEmpty()) out += path else out += listAssetFiles(context, path)
        }
        // Unpack in a predictable order: data.win first, then fonts, then the folders.
        val preferred = out.sortedBy { path ->
            val name = path.removePrefix("$ASSET_ROOT/")
            val index = PREFERRED.indexOfFirst { name == it || name.startsWith("$it/") }
            if (index >= 0) index else PREFERRED.size
        }
        return preferred
    }
}
