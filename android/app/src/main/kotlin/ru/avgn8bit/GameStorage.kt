package ru.avgn8bit

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * The game's files on the phone: what gets imported from the user's copy of AVGN 8-bit, and where the runtime
 * reads them from.
 *
 * Layout (all inside the app's private storage, so no storage permission is needed):
 *   files/avgn/data.win        the game
 *   files/avgn/*.ttf           the fonts the game loads with font_add()
 *   files/avgn/Lang, Attract  texts and the attract-mode screens
 *   files/avgn/Cutscenes       videos (optional, they are the big part of the install)
 *   files/saves                saves, options, log.txt
 */
object GameStorage {
    private const val DIR_DATA = "avgn"
    private const val DIR_SAVES = "saves"

    private val ROOT_DIRS = listOf("Lang", "Attract", "Cutscenes")

    fun dataDir(context: Context): File = File(context.filesDir, DIR_DATA).apply { mkdirs() }
    fun savesDir(context: Context): File = File(context.filesDir, DIR_SAVES).apply { mkdirs() }
    /** What the app itself logs. */
    fun logFile(context: Context): File = File(File(context.filesDir, "logs"), "app.log")

    /** What the runtime logs (the game's own print() output and engine warnings). */
    fun nativeLogFile(context: Context): File = File(File(context.filesDir, "logs"), "native.log")

    /** The retail data.win the user copied in; this is also what the patch is applied to. */
    fun rawDataWin(context: Context): File? =
        File(dataDir(context), "data.win").takeIf { it.isFile && it.length() > 0 }

    /**
     * What the runtime actually loads: the patched data.win when it is there, the retail one otherwise. The Vita
     * build of the game expects the patched file, so [ensurePatched] is what normally makes this the former.
     */
    fun dataWin(context: Context): File? {
        val dir = dataDir(context)
        if (GameDataPatcher.isPatched(dir)) return File(dir, GameDataPatcher.OUTPUT_NAME)
        return File(dir, GameDataPatcher.OUTPUT_NAME).takeIf { it.isFile && it.length() > 0 }
            ?: rawDataWin(context)
    }

    fun isReady(context: Context): Boolean = rawDataWin(context) != null || dataWin(context) != null

    /**
     * Builds `<data dir>/data_vita.win` from the imported data.win and the patch embedded in the app. Safe to call
     * repeatedly; an already patched, verified file is reused.
     *
     * When the patch cannot be applied (a different game version) the game is still left runnable: the runtime then
     * loads the retail data.win.
     */
    fun ensurePatched(context: Context, onProgress: (Float) -> Unit = {}): GameDataPatcher.Result {
        val dir = dataDir(context)
        val patched = File(dir, GameDataPatcher.OUTPUT_NAME)
        if (GameDataPatcher.isPatched(dir)) return GameDataPatcher.Result(true, "Уже собран", patched)
        val raw = rawDataWin(context) ?: return GameDataPatcher.Result(false, "Нет файла data.win")
        return GameDataPatcher.apply(context, raw, dir, onProgress)
    }

    fun installedSize(context: Context): Long = dataDir(context).walkTopDown().filter { it.isFile }.sumOf { it.length() }

    fun clearImportedFiles(context: Context) {
        val dir = dataDir(context)
        dir.listFiles()?.forEach { it.deleteRecursively() }
    }

    /** A file the import would take, resolved from the picked folder. */
    data class Entry(val file: DocumentFile, val relative: String, val size: Long)

    /**
     * Looks at the picked folder (or a single folder below it, so picking the Steam folder or the game folder
     * both work) and returns its data.win if it looks like AVGN 8-bit.
     */
    fun inspect(context: Context, uri: Uri): ImportPreview {
        val root = DocumentFile.fromTreeUri(context, uri)
            ?: return ImportPreview(null, emptyList(), 0, "Папка недоступна")
        val gameRoot = findGameRoot(root)
            ?: return ImportPreview(null, emptyList(), 0, "В этой папке нет data.win — выберите папку с файлами игры")
        val entries = collect(gameRoot, includeVideos = true)
        val sizes = entries.groupBy { it.relative.substringBefore('/') }.mapValues { (_, list) -> list.sumOf { it.size } }
        return ImportPreview(gameRoot.name ?: "AVGN 8-bit", entries, entries.sumOf { it.size }, null, sizes)
    }

    private fun findGameRoot(root: DocumentFile): DocumentFile? {
        if (root.findFile("data.win") != null || root.findFile("data_vita.win") != null) return root
        // The Vita folder layout (AVGN8bit/) or the game folder chosen one level too high.
        root.listFiles().filter { it.isDirectory }.forEach { child ->
            if (child.findFile("data.win") != null || child.findFile("data_vita.win") != null) return child
        }
        return null
    }

    private fun collect(root: DocumentFile, includeVideos: Boolean): List<Entry> {
        val out = ArrayList<Entry>()
        root.listFiles().forEach { child ->
            val name = child.name ?: return@forEach
            val lower = name.lowercase()
            if (child.isFile) {
                when {
                    name == "data.win" || name == "data_vita.win" -> out += Entry(child, "data.win", child.length())
                    lower.endsWith(".ttf") || lower.endsWith(".otf") -> out += Entry(child, name, child.length())
                }
            } else if (child.isDirectory && ROOT_DIRS.any { it.equals(name, ignoreCase = true) }) {
                val target = ROOT_DIRS.first { it.equals(name, ignoreCase = true) }
                collectDirectory(child, target, includeVideos, out)
            }
        }
        return out
    }

    private fun collectDirectory(dir: DocumentFile, relative: String, includeVideos: Boolean, out: MutableList<Entry>) {
        dir.listFiles().forEach { child ->
            val name = child.name ?: return@forEach
            val path = "$relative/$name"
            if (child.isDirectory) {
                collectDirectory(child, path, includeVideos, out)
            } else {
                if (!includeVideos && name.lowercase().endsWith(".mp4")) return@forEach
                out += Entry(child, path, child.length())
            }
        }
    }

    data class ImportPreview(
        val gameFolderName: String?,
        val entries: List<Entry>,
        val totalBytes: Long,
        val error: String?,
        val sizesByFolder: Map<String, Long> = emptyMap()
    ) {
        val hasDataWin: Boolean get() = entries.any { it.relative == "data.win" }
        val videoBytes: Long get() = sizesByFolder["Cutscenes"] ?: 0L
    }

    /**
     * Copies the game into the app. [includeVideos] false skips the cutscenes, which is what most of the size is:
     * the game then skips them instead of playing them.
     */
    suspend fun import(context: Context, entries: List<Entry>, includeVideos: Boolean, onProgress: (String, Float) -> Unit) =
        withContext(Dispatchers.IO) {
            val target = dataDir(context)
            clearImportedFiles(context)
            target.mkdirs()

            val wanted = entries.filter { includeVideos || !it.relative.lowercase().endsWith(".mp4") }
            val total = wanted.sumOf { it.size }.coerceAtLeast(1L)
            var copied = 0L

            for (entry in wanted) {
                val out = File(target, entry.relative)
                out.parentFile?.mkdirs()
                onProgress(entry.relative, copied.toFloat() / total)
                context.contentResolver.openInputStream(entry.file.uri)?.use { input ->
                    out.outputStream().use { output -> input.copyTo(output, 1 shl 16) }
                }
                copied += entry.size
            }
            onProgress("", 1f)
            AppLog.write("Imported ${wanted.size} files (${copied / 1048576} MB) into ${target.absolutePath}")
        }

    /** Reads the game's own name out of data.win; null when the file is not a WAD we can parse. */
    fun describe(context: Context): String? {
        val file = dataWin(context) ?: return null
        return try {
            val handle = Native.dataWinParseLight(file.absolutePath)
            if (handle == 0L) return null
            try {
                Native.dataWinDisplayName(handle) ?: Native.dataWinName(handle)
            } finally {
                Native.dataWinFree(handle)
            }
        } catch (t: Throwable) {
            AppLog.write("Could not read the game name: ${t.message}")
            null
        }
    }
}
