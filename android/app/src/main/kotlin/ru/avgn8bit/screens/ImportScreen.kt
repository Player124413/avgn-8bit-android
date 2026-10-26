package ru.avgn8bit.screens

import android.content.Intent
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

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        try {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: Throwable) {
            // Some document providers do not hand out a persistable grant; the copy still works right now.
        }
        val inspected = GameStorage.inspect(context, uri)
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
        Spacer(Modifier.height(8.dp))
        Text(
            "Выберите папку, в которой лежит игра: та, что открывается по кнопке «Управление → Просмотреть локальные файлы» в Steam. " +
                "Нужны data.win, шрифты .ttf и папки Lang, Attract, Cutscenes.",
            color = Color(0xFFB0B0B8),
            fontSize = 13.sp
        )

        Spacer(Modifier.height(16.dp))
        Button(
            onClick = { picker.launch(null) },
            modifier = Modifier.fillMaxWidth().height(52.dp),
            shape = RoundedCornerShape(14.dp)
        ) {
            Text("Выбрать папку игры", fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
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
                    Text(p.gameFolderName ?: "Игра", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold)
                    Text(
                        "Всего: ${mb(p.totalBytes)}" + if (p.videoBytes > 0) " (видео: ${mb(p.videoBytes)})" else "",
                        color = Color(0xFF9E9EA7),
                        fontSize = 12.sp
                    )
                    Spacer(Modifier.height(6.dp))
                    val byFolder = p.sizesByFolder.filterKeys { it != "data.win" && !it.endsWith(".ttf") }
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
                                    GameStorage.import(context, p.entries, settings.includeVideosOnImport) { name, fraction ->
                                        progress = fraction * 0.8f
                                        progressText = name
                                    }
                                    // The Vita build of the game needs the patched data.win; the patch itself is a
                                    // small diff shipped inside the app, so this runs offline and adds no download.
                                    progressText = "Собираю data_vita.win…"
                                    // Reading the 568 KB diff and writing ~170 MB: never on the UI thread.
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
                    if (busy) {
                        Spacer(Modifier.height(10.dp))
                        LinearProgressIndicator(
                            progress = { progress },
                            modifier = Modifier.fillMaxWidth(),
                            color = AvgnRed
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(progressText, color = Color(0xFF9E9EA7), fontSize = 11.sp)
                    }
                }
            }
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
            "Порт не содержит файлов игры. Он читает данные из вашей копии, поэтому без импорта игра не запустится.",
            color = Color(0xFF6E6E78),
            fontSize = 11.sp
        )
        Spacer(Modifier.height(12.dp))
        OutlinedButton(onClick = { picker.launch(null) }, modifier = Modifier.fillMaxWidth()) {
            Text("Выбрать другую папку")
        }
    }
}

private fun mb(bytes: Long): String = when {
    bytes <= 0 -> "0 МБ"
    bytes >= 1073741824L -> "%.1f ГБ".format(bytes / 1073741824.0)
    else -> "%d МБ".format(bytes / 1048576)
}
