package ru.avgn8bit

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.zip.ZipInputStream

/**
 * The game's files on the phone: what gets imported from the user's copy of AVGN 8-bit, and where the runtime
 * reads them from.
 *
 * Layout (all inside the app's private storage, so no storage permission is needed):
 *   files/avgn/data.win        the game
 *   files/avgn/  (the .ttf fonts)  what the game loads with font_add()
 *   files/avgn/Lang, Attract  texts and the attract-mode screens
 *   files/avgn/Cutscenes       videos (optional, they are the big part of the install)
 *   files/saves                saves, options, log.txt
 *
 * The user can bring the files in two ways: pick the game folder (SAF tree) or pick a zip that contains it, which
 * is what people usually end up with after zipping the Steam folder or the Vita patcher's output.
 */
object GameStorage {
    private const val DIR_DATA = "avgn"
    private const val DIR_SAVES = "saves"
    const val DATA_WIN = "data.win"
    const val VITA_WIN = "data_vita.win"

    /**
     * Everything the game ships is imported, not a list of folders picked in advance: a game that keeps one of its
     * files (a language table, a controller database, an options file) outside Lang/, Attract/ and Cutscenes/ would
     * otherwise lose it silently, and on Windows and on the Vita's memory card the whole folder is simply there.
     * Only the obvious junk of a zipped Windows folder is dropped.
     */
    private val JUNK_NAMES = setOf(
        "__macosx", ".ds_store", "thumbs.db", "desktop.ini", ".git", ".gitignore", ".gitattributes"
    )

    private fun isJunk(name: String): Boolean {
        val lower = name.lowercase()
        return lower in JUNK_NAMES || lower.startsWith("._") || lower.endsWith(".lnk") || lower.endsWith(".tmp")
    }

    fun dataDir(context: Context): File = File(context.filesDir, DIR_DATA).apply { mkdirs() }
    fun savesDir(context: Context): File = File(context.filesDir, DIR_SAVES).apply { mkdirs() }

    /** The folder with the two logs, opened from the Logs menu. */
    fun logsDir(context: Context): File = File(context.filesDir, "logs").apply { mkdirs() }

    /** What the app itself logs. */
    fun logFile(context: Context): File = File(logsDir(context), "app.log")

    /** What the runtime logs (the game's own print() output and engine warnings). */
    fun nativeLogFile(context: Context): File = File(logsDir(context), "native.log")

    /** The retail data.win the user copied in; this is also what the patch is applied to. */
    fun rawDataWin(context: Context): File? =
        File(dataDir(context), DATA_WIN).takeIf { it.isFile && it.length() > 0 }

    /** A data.win that was already patched elsewhere (the Vita patcher's output), if the user brought one. */
    fun patchedDataWin(context: Context): File? =
        File(dataDir(context), VITA_WIN).takeIf { it.isFile && it.length() > 0 }

    /**
     * What the runtime actually loads: the patched data.win when it is there, the retail one otherwise. The Vita
     * build of the game expects the patched file, so [ensurePatched] is what normally makes this the former.
     */
    fun dataWin(context: Context): File? {
        val dir = dataDir(context)
        if (GameDataPatcher.isPatched(dir)) return File(dir, GameDataPatcher.OUTPUT_NAME)
        return patchedDataWin(context) ?: rawDataWin(context)
    }

    fun isReady(context: Context): Boolean = rawDataWin(context) != null || patchedDataWin(context) != null

    fun installedSize(context: Context): Long = dataDir(context).walkTopDown().filter { it.isFile }.sumOf { it.length() }

    fun clearImportedFiles(context: Context) {
        val dir = dataDir(context)
        dir.listFiles()?.forEach { it.deleteRecursively() }
    }

    /**
     * Builds `<data dir>/data_vita.win` from the imported data.win and the patch embedded in the app. Safe to call
     * repeatedly; an already patched, verified file is reused.
     *
     * When the patch cannot be applied (a different game version) the game is still left runnable: the runtime then
     * loads the retail data.win.
     */
    fun ensurePatched(context: Context, onProgress: (Float) -> Unit = {}): GameDataPatcher.Result {
        val dir = dataDir(context)
        if (GameDataPatcher.isPatched(dir)) {
            return GameDataPatcher.Result(true, context.getString(R.string.patch_already), File(dir, GameDataPatcher.OUTPUT_NAME))
        }
        // Someone brought the Vita patcher's output instead of the retail data.win: nothing to do.
        patchedDataWin(context)?.let {
            return GameDataPatcher.Result(true, context.getString(R.string.patch_used_ready), it)
        }
        val raw = rawDataWin(context) ?: return GameDataPatcher.Result(false, context.getString(R.string.patch_no_data_win))
        return GameDataPatcher.apply(context, raw, dir, onProgress)
    }

    // ===[ Import ]===

    /** Where a file will be read from: a SAF document, or an entry inside the picked zip. */
    data class FileEntry(
        val relative: String,
        val size: Long,
        val document: DocumentFile? = null,
        val zipPath: String? = null
    )

    enum class Kind { FOLDER, ZIP }

    data class ImportPreview(
        val gameFolderName: String?,
        val entries: List<FileEntry>,
        val totalBytes: Long,
        val error: String?,
        val sizesByFolder: Map<String, Long> = emptyMap(),
        val kind: Kind = Kind.FOLDER,
        val uri: Uri? = null,
        /** True when at least one entry had no size in the zip header, so the total is an estimate. */
        val sizesIncomplete: Boolean = false
    ) {
        val hasDataWin: Boolean get() = entries.any { it.relative == DATA_WIN || it.relative == VITA_WIN }
        val videoBytes: Long
            get() = sizesByFolder.entries.firstOrNull { it.key.equals("Cutscenes", ignoreCase = true) }?.value ?: 0L
        val fileCount: Int get() = entries.size
    }

    /**
     * Looks at the picked folder (or a single folder below it, so picking the Steam folder or the game folder
     * both work) and returns its data.win if it looks like AVGN 8-bit.
     */
    fun inspectFolder(context: Context, uri: Uri): ImportPreview {
        val root = DocumentFile.fromTreeUri(context, uri)
            ?: return ImportPreview(null, emptyList(), 0, context.getString(R.string.storage_folder_unavailable), kind = Kind.FOLDER)
        val gameRoot = findGameRoot(root)
            ?: return ImportPreview(
                null, emptyList(), 0,
                context.getString(R.string.storage_no_data_win_folder),
                kind = Kind.FOLDER
            )
        val entries = collect(gameRoot, includeVideos = true)
        return ImportPreview(
            gameFolderName = gameRoot.name ?: "AVGN 8-bit",
            entries = entries,
            totalBytes = entries.sumOf { it.size },
            error = null,
            sizesByFolder = sizesByFolder(entries),
            kind = Kind.FOLDER,
            uri = uri
        )
    }

    /**
     * Looks inside a zip (the app accepts one instead of a folder, which is how most people move a 1.5 GB game
     * around). Everything the game ships is listed - it is the game's own file layout that decides what it needs,
     * not a list of folders guessed here (see [JUNK_NAMES]) - and the cutscene videos can be left out.
     */
    fun inspectZip(context: Context, uri: Uri): ImportPreview {
        val names = ArrayList<String>()
        val sizes = HashMap<String, Long>()
        var incomplete = false
        try {
            openZip(context, uri).use { zip ->
                var guard = 0
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (++guard > 200_000) break
                    if (entry.isDirectory) continue
                    names += entry.name
                    if (entry.size >= 0) sizes[entry.name] = entry.size else incomplete = true
                }
            }
        } catch (t: Throwable) {
            AppLog.write("Could not read the zip: ${t.stackTraceToString()}")
            return ImportPreview(
                null, emptyList(), 0,
                context.getString(
                    R.string.storage_zip_read_failed,
                    t.message ?: context.getString(R.string.storage_zip_corrupt)
                ),
                kind = Kind.ZIP, uri = uri
            )
        }

        val root = zipGameRoot(names)
            ?: return ImportPreview(
                null, emptyList(), 0,
                context.getString(R.string.storage_no_data_win_zip),
                kind = Kind.ZIP, uri = uri
            )
        val folderName = root.trimEnd('/').substringAfterLast('/').ifEmpty { "AVGN 8-bit" }

        val entries = ArrayList<FileEntry>()
        for (name in names) {
            val relative = zipRelative(root, name) ?: continue
            entries += FileEntry(relative, sizes[name] ?: 0L, zipPath = name)
        }
        entries.sortBy { it.relative }
        return ImportPreview(
            gameFolderName = folderName,
            entries = entries,
            totalBytes = entries.sumOf { it.size },
            error = null,
            sizesByFolder = sizesByFolder(entries),
            kind = Kind.ZIP,
            uri = uri,
            sizesIncomplete = incomplete
        )
    }

    private fun sizesByFolder(entries: List<FileEntry>): Map<String, Long> =
        entries.groupBy { it.relative.substringBefore('/') }.mapValues { (_, list) -> list.sumOf { it.size } }

    private fun findGameRoot(root: DocumentFile): DocumentFile? {
        if (root.findFile(DATA_WIN) != null || root.findFile(VITA_WIN) != null) return root
        // The Vita folder layout (AVGN8bit/) or the game folder chosen one level too high.
        root.listFiles().filter { it.isDirectory }.forEach { child ->
            if (child.findFile(DATA_WIN) != null || child.findFile(VITA_WIN) != null) return child
        }
        return null
    }

    private fun collect(root: DocumentFile, includeVideos: Boolean): List<FileEntry> {
        val out = ArrayList<FileEntry>()
        root.listFiles().forEach { child ->
            val name = child.name ?: return@forEach
            val lower = name.lowercase()
            if (child.isFile) {
                when {
                    // Keep the patched name: a file from the Vita patcher must not be overwritten with data.win.
                    lower == DATA_WIN -> out += FileEntry(DATA_WIN, child.length(), document = child)
                    lower == VITA_WIN -> out += FileEntry(VITA_WIN, child.length(), document = child)
                    isJunk(name) -> Unit
                    else -> out += FileEntry(name, child.length(), document = child)
                }
            } else if (child.isDirectory) {
                if (isJunk(name)) return@forEach
                collectDirectory(child, name, includeVideos, out)
            }
        }
        return out
    }

    private fun collectDirectory(dir: DocumentFile, relative: String, includeVideos: Boolean, out: MutableList<FileEntry>) {
        dir.listFiles().forEach { child ->
            val name = child.name ?: return@forEach
            val path = "$relative/$name"
            if (child.isDirectory) {
                collectDirectory(child, path, includeVideos, out)
            } else {
                if (!includeVideos && name.lowercase().endsWith(".mp4")) return@forEach
                out += FileEntry(path, child.length(), document = child)
            }
        }
    }

    /**
     * Copies the game into the app. [includeVideos] false skips the cutscenes, which is what most of the size is:
     * the game then skips them instead of playing them.
     */
    suspend fun importFiles(
        context: Context,
        preview: ImportPreview,
        includeVideos: Boolean,
        onProgress: (String, Float) -> Unit
    ) = withContext(Dispatchers.IO) {
        when (preview.kind) {
            Kind.FOLDER -> importFromFolder(context, preview, includeVideos, onProgress)
            Kind.ZIP -> {
                val uri = preview.uri ?: throw IllegalStateException(context.getString(R.string.storage_zip_unavailable))
                importFromZip(context, uri, preview, includeVideos, onProgress)
            }
        }
    }

    private fun importFromFolder(
        context: Context,
        preview: ImportPreview,
        includeVideos: Boolean,
        onProgress: (String, Float) -> Unit
    ) {
        val target = dataDir(context)
        clearImportedFiles(context)
        target.mkdirs()

        val wanted = preview.entries.filter { includeVideos || !it.relative.lowercase().endsWith(".mp4") }
        val total = wanted.sumOf { it.size }.coerceAtLeast(1L)
        var copied = 0L

        for (entry in wanted) {
            val file = entry.document ?: continue
            val out = File(target, entry.relative)
            out.parentFile?.mkdirs()
            onProgress(entry.relative, copied.toFloat() / total)
            context.contentResolver.openInputStream(file.uri)?.use { input ->
                out.outputStream().use { output -> input.copyTo(output, 1 shl 16) }
            }
            copied += entry.size
        }
        onProgress("", 1f)
        AppLog.write("Imported ${wanted.size} files (${copied / 1048576} MB) into ${target.absolutePath}")
    }

    /**
     * One streaming pass over the zip: the picked archive is never copied to the cache first, so a 1.5 GB zip with
     * the cutscenes in it does not need 1.5 GB of free space on top.
     */
    private fun importFromZip(
        context: Context,
        uri: Uri,
        preview: ImportPreview,
        includeVideos: Boolean,
        onProgress: (String, Float) -> Unit
    ) {
        val target = dataDir(context)
        clearImportedFiles(context)
        target.mkdirs()

        val wanted = preview.entries
            .filter { includeVideos || !it.relative.lowercase().endsWith(".mp4") }
            .associateBy { it.zipPath }
        val total = wanted.values.sumOf { it.size }
        val byCount = total <= 0
        var copied = 0L
        var done = 0

        openZip(context, uri).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) continue
                val wantedEntry = wanted[entry.name] ?: continue
                val out = File(target, wantedEntry.relative)
                out.parentFile?.mkdirs()
                val fraction = if (byCount) {
                    (done.toFloat() / wanted.size.coerceAtLeast(1)).coerceIn(0f, 1f)
                } else {
                    (copied.toFloat() / total).coerceIn(0f, 1f)
                }
                onProgress(wantedEntry.relative, fraction)
                out.outputStream().use { output ->
                    val buffer = ByteArray(1 shl 16)
                    while (true) {
                        val read = zip.read(buffer)
                        if (read <= 0) break
                        output.write(buffer, 0, read)
                        copied += read
                    }
                }
                done++
            }
        }
        onProgress("", 1f)
        AppLog.write("Imported ${done} files (${copied / 1048576} MB) from a zip into ${target.absolutePath}")
    }

    private fun openZip(context: Context, uri: Uri): ZipInputStream {
        val stream = context.contentResolver.openInputStream(uri)
            ?: throw IllegalStateException(context.getString(R.string.storage_zip_open_failed))
        return ZipInputStream(stream.buffered(1 shl 16))
    }

    /**
     * The prefix (ending with '/', or empty) that holds data.win: entries can sit at the archive root or inside a
     * single folder, which is what "Compress the AVGN8bit folder" and the Steam folder zip both produce.
     */
    private fun zipGameRoot(names: List<String>): String? {
        var best: String? = null
        for (raw in names) {
            val name = raw.replace('\\', '/').trimStart('/')
            val lower = name.lowercase()
            val isData = lower.endsWith("/$DATA_WIN") || lower == DATA_WIN ||
                lower.endsWith("/$VITA_WIN") || lower == VITA_WIN
            if (!isData) continue
            val prefix = name.substringBeforeLast('/', "")
            val candidate = if (prefix.isEmpty()) "" else "$prefix/"
            if (best == null || candidate.length < best.length) best = candidate
        }
        return best
    }

    /** Maps a zip entry to the name it gets in the data folder, or null when the port does not need it. */
    private fun zipRelative(root: String, raw: String): String? {
        val name = raw.replace('\\', '/').trimStart('/')
        if (name.startsWith("__MACOSX/") || name.contains("/__MACOSX/")) return null
        val rest = if (root.isEmpty()) name else {
            if (!name.startsWith(root)) return null
            name.substring(root.length)
        }
        if (rest.isEmpty() || rest.endsWith("/")) return null
        if (rest.split('/').any { it == ".." || it.isEmpty() }) return null
        if (rest.startsWith(".")) return null
        if (isJunk(rest.substringAfterLast('/'))) return null

        if (rest.count { it == '/' } == 0) {
            val lower = rest.lowercase()
            return when {
                lower == DATA_WIN -> DATA_WIN
                lower == VITA_WIN -> VITA_WIN
                else -> rest
            }
        }
        return rest
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
