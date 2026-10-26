package ru.avgn8bit.screens

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ru.avgn8bit.AppLog
import ru.avgn8bit.AppSettings
import ru.avgn8bit.BundledGame
import ru.avgn8bit.GameStorage
import ru.avgn8bit.ui.AvgnPanel
import ru.avgn8bit.ui.AvgnRed

@Composable
fun ImportScreen(settings: AppSettings, onDone: () -> Unit, onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var preview by remember { mutableStateOf<GameStorage.ImportPreview?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var progress by remember { mutableStateOf(0f) }
    var progressText by remember { mutableStateOf("") }
    var done by remember { mutableStateOf(GameStorage.isReady(context)) }
    var patchMessage by remember { mutableStateOf<String?>(null) }
    var patchOk by remember { mutableStateOf(false) }

    val bundledAvailable = remember { BundledGame.isAvailable(context) }
    var bundledInstalled by remember { mutableStateOf(BundledGame.isInstalled(context)) }
    val bundledBytes = remember(bundledAvailable) { if (bundledAvailable) BundledGame.bundledBytes(context) else 0L }

    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        keepPermission(context, uri)
        val inspected = GameStorage.inspectFolder(context, uri)
        preview = inspected
        error = inspected.error
    }

    val zipPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        keepPermission(context, uri)
        preview = null
        val inspected = GameStorage.inspectZip(context, uri)
        preview = inspected
        error = inspected.error
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Файлы игры", color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = onBack) { Text("Назад") }
        }
        Spacer(Modifier.height(10.dp))
        Text(
            "Можно выбрать папку с игрой (та, что открывается по кнопке «Управление → Просмотреть локальные " +
                "файлы» в Steam) или zip-архив с ней. Нужны data.win, шрифты .ttf и папки Lang, Attract, Cutscenes.",
            color = Color(0xFFB0B0B8),
            fontSize = 13.sp
        )

        if (bundledAvailable && !bundledInstalled) {
            Spacer(Modifier.height(16.dp))
            Card(
                colors = CardDefaults.cardColors(containerColor = Color(0xFF1F2A1B)),
                shape = RoundedCornerShape(16.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("Игра уже внутри APK", color = Color(0xFFA5D6A7), fontSize = 15.sp, fontWeight = FontWeight.Bold)
                    Text(
                        "Эта сборка включает файлы игры (${mb(bundledBytes)}). Их нужно один раз распаковать в " +
                            "хранилище приложения — дальше всё как обычно.",
                        color = Color(0xFFB9C7B4),
                        fontSize = 12.sp
                    )
                    Spacer(Modifier.height(10.dp))
                    Button(
                        onClick = {
                            busy = true
                            error = null
                            scope.launch {
                                try {
                                    val written = withContext(Dispatchers.IO) {
                                        BundledGame.install(context) { name, fraction ->
                                            progress = fraction
                                            progressText = name
                                        }
                                    }
                                    progressText = "Собираю data_vita.win…"
                                    val patch = withContext(Dispatchers.IO) {
                                        GameStorage.ensurePatched(context) { fraction ->
                                            progress = fraction
                                        }
                                    }
                                    patchOk = patch.ok
                                    patchMessage = if (patch.ok) {
                                        "data_vita.win собран (${mb(patch.output?.length() ?: 0L)})"
                                    } else {
                                        patch.message
                                    }
                                    bundledInstalled = true
                                    done = true
                                    busy = false
                                    progress = 1f
                                    AppLog.write("Bundled game installed: $written bytes; patch: ${patch.message}")
                                    if (patch.ok) onDone()
                                } catch (t: Throwable) {
                                    busy = false
                                    error = "Не удалось распаковать: ${t.message}"
                                    AppLog.write("Bundled install failed: ${t.stackTraceToString()}")
                                }
                            }
                        },
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(if (busy) "Распаковываю…" else "Распаковать и играть")
                    }
                }
            }
        }

        if (!bundledAvailable || bundledInstalled) {
            Spacer(Modifier.height(16.dp))
            Button(
                onClick = { folderPicker.launch(null) },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp)
            ) {
                Text("Выбрать папку игры", fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { zipPicker.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream", "*/*")) },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp)
            ) {
                Text("Выбрать ZIP-архив с игрой", fontSize = 16.sp)
            }
            Text(
                "Архив можно положить в «Загрузки» и выбрать здесь — распаковка идёт сразу в хранилище " +
                    "приложения, без лишней копии.",
                color = Color(0xFF6E6E78),
                fontSize = 11.sp
            )
        }

        error?.let {
            Spacer(Modifier.height(12.dp))
            Card(colors = CardDefaults.cardColors(containerColor = Color(0xFF3A1E1E))) {
                Text(it, color = Color(0xFFFFCDD2), modifier = Modifier.padding(14.dp), fontSize = 13.sp)
            }
        }

        preview?.let { p ->
            Spacer(Modifier.height(16.dp))
            Card(
                colors = CardDefaults.cardColors(containerColor = AvgnPanel),
                shape = RoundedCornerShape(16.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text(
                        p.gameFolderName ?: "Игра",
                        color = Color.White,
                        fontSize = 18.sp,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        buildString {
                            append("Файлов: ${p.fileCount} · ")
                            if (p.sizesIncomplete) append("около ") else append("")
                            append(mb(p.totalBytes))
                            if (p.videoBytes > 0) append(" (видео: ${mb(p.videoBytes)})")
                        },
                        color = Color(0xFF9E9EA7),
                        fontSize = 12.sp
                    )
                    Spacer(Modifier.height(6.dp))
                    val byFolder = p.sizesByFolder.filterKeys { it != GameStorage.DATA_WIN && !it.endsWith(".ttf") }
                    byFolder.forEach { (folder, size) ->
                        Text("$folder — ${mb(size)}", color = Color(0xFFB0B0B8), fontSize = 12.sp)
                    }
                    Spacer(Modifier.height(12.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Копировать катсцены", color = Color.White, fontSize = 14.sp)
                            Text(
                                "Видео занимают больше всего места. Без них игра просто пропускает ролики.",
                                color = Color(0xFF9E9EA7),
                                fontSize = 11.sp
                            )
                        }
                        Switch(
                            checked = settings.includeVideosOnImport,
                            onCheckedChange = { settings.setIncludeVideosOnImport(it) }
                        )
                    }
                    Spacer(Modifier.height(12.dp))
                    val bytesToCopy = p.totalBytes - if (settings.includeVideosOnImport) 0 else p.videoBytes
                    Button(
                        onClick = {
                            busy = true
                            error = null
                            scope.launch {
                                try {
                                    GameStorage.importFiles(context, p, settings.includeVideosOnImport) { name, fraction ->
                                        progress = fraction * 0.8f
                                        progressText = name
                                    }
                                    // The Vita build of the game needs the patched data.win; the patch itself is a
                                    // small diff shipped inside the app, so this runs offline and adds no download.
                                    progressText = "Собираю data_vita.win…"
                                    val patch = withContext(Dispatchers.IO) {
                                        GameStorage.ensurePatched(context) { fraction ->
                                            progress = 0.8f + fraction * 0.2f
                                        }
                                    }
                                    patchOk = patch.ok
                                    patchMessage = if (patch.ok) {
                                        "data_vita.win собран (${mb(patch.output?.length() ?: 0L)})"
                                    } else {
                                        patch.message
                                    }
                                    settings.setImportedFolderName(p.gameFolderName)
                                    done = true
                                    busy = false
                                    progress = 1f
                                } catch (t: Throwable) {
                                    busy = false
                                    error = "Не удалось скопировать: ${t.message}"
                                    AppLog.write("Import failed: ${t.stackTraceToString()}")
                                }
                            }
                        },
                        enabled = !busy && p.hasDataWin,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(if (busy) "Копирую…" else "Импортировать (${mb(bytesToCopy)})")
                    }
                }
            }
        }

        if (busy) {
            Spacer(Modifier.height(12.dp))
            LinearProgressIndicator(
                progress = { progress },
                modifier = Modifier.fillMaxWidth(),
                color = AvgnRed
            )
            Spacer(Modifier.height(4.dp))
            Text(progressText, color = Color(0xFF9E9EA7), fontSize = 11.sp)
        }

        if (done && !busy) {
            Spacer(Modifier.height(16.dp))
            Text(
                "Файлы игры готовы. Запустить игру можно с главного экрана.",
                color = Color(0xFF9CCC65),
                fontSize = 13.sp
            )
            patchMessage?.let {
                Spacer(Modifier.height(6.dp))
                Text(it, color = if (patchOk) Color(0xFF9CCC65) else Color(0xFFFFB74D), fontSize = 12.sp)
            }
            Spacer(Modifier.height(10.dp))
            Button(onClick = onDone, modifier = Modifier.fillMaxWidth()) {
                Text("Готово")
            }
            Spacer(Modifier.height(6.dp))
            OutlinedButton(
                onClick = {
                    busy = true
                    scope.launch {
                        val result = withContext(Dispatchers.IO) {
                            GameStorage.ensurePatched(context) { fraction -> progress = fraction }
                        }
                        patchOk = result.ok
                        patchMessage = if (result.ok) {
                            "data_vita.win собран (${mb(result.output?.length() ?: 0L)})"
                        } else {
                            result.message
                        }
                        busy = false
                    }
                },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Пересобрать data_vita.win")
            }
        }

        Spacer(Modifier.height(24.dp))
        Text(
            "Порт не содержит файлов игры (если они не были вложены в APK при сборке). Он читает данные из вашей " +
                "копии, поэтому без импорта игра не запустится.",
            color = Color(0xFF6E6E78),
            fontSize = 11.sp
        )
    }
}

private fun keepPermission(context: android.content.Context, uri: Uri) {
    try {
        context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
    } catch (_: Throwable) {
        // Some providers do not hand out a persistable grant; the import still works right now.
    }
}

private fun mb(bytes: Long): String = when {
    bytes <= 0 -> "0 МБ"
    bytes >= 1073741824L -> "%.1f ГБ".format(bytes / 1073741824.0)
    else -> "%d МБ".format(bytes / 1048576)
}
