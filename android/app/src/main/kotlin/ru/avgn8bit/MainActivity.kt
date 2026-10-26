package ru.avgn8bit

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import ru.avgn8bit.screens.AboutScreen
import ru.avgn8bit.screens.ControlsScreen
import ru.avgn8bit.screens.ImportScreen
import ru.avgn8bit.screens.SettingsScreen
import ru.avgn8bit.ui.AvgnPanel
import ru.avgn8bit.ui.AvgnRed
import ru.avgn8bit.ui.AvgnTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val settings = (application as AvgnApp).settings

        setContent {
            AvgnTheme {
                val context = LocalContext.current
                var gameReady by remember { mutableStateOf(GameStorage.isReady(context)) }
                var screen by remember { mutableStateOf(if (gameReady) Screen.HOME else Screen.IMPORT) }

                BackHandler(enabled = screen != Screen.HOME) { screen = Screen.HOME }

                Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
                    when (screen) {
                        Screen.HOME -> HomeScreen(
                            settings = settings,
                            gameReady = gameReady,
                            onPlay = { startActivity(Intent(this@MainActivity, GameActivity::class.java)) },
                            onImport = { screen = Screen.IMPORT },
                            onSettings = { screen = Screen.SETTINGS },
                            onControls = { screen = Screen.CONTROLS },
                            onAbout = { screen = Screen.ABOUT }
                        )

                        Screen.IMPORT -> ImportScreen(
                            settings = settings,
                            onDone = {
                                gameReady = GameStorage.isReady(context)
                                screen = Screen.HOME
                            },
                            onBack = {
                                gameReady = GameStorage.isReady(context)
                                screen = Screen.HOME
                            }
                        )

                        Screen.SETTINGS -> SettingsScreen(
                            settings = settings,
                            onBack = { screen = Screen.HOME },
                            onControls = { screen = Screen.CONTROLS },
                            onAbout = { screen = Screen.ABOUT }
                        )

                        Screen.CONTROLS -> ControlsScreen(
                            settings = settings,
                            onBack = { screen = Screen.HOME }
                        )

                        Screen.ABOUT -> AboutScreen(onBack = { screen = Screen.HOME })
                    }
                }
            }
        }
    }
}

private enum class Screen { HOME, IMPORT, SETTINGS, CONTROLS, ABOUT }

@Composable
private fun HomeScreen(
    settings: AppSettings,
    gameReady: Boolean,
    onPlay: () -> Unit,
    onImport: () -> Unit,
    onSettings: () -> Unit,
    onControls: () -> Unit,
    onAbout: () -> Unit
) {
    val context = LocalContext.current
    val imported = gameReady
    val gameName by produceState<String?>(null, imported) {
        value = if (!imported) null else withContext(Dispatchers.IO) { GameStorage.describe(context) }
    }
    val sizeText by produceState("", imported) {
        value = if (!imported) "" else withContext(Dispatchers.IO) {
            val bytes = GameStorage.installedSize(context)
            when {
                bytes <= 0 -> ""
                bytes >= 1073741824L -> "%.1f ГБ".format(bytes / 1073741824.0)
                else -> "%d МБ".format(bytes / 1048576)
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Spacer(Modifier.height(28.dp))
        Text(
            text = "AVGN",
            color = AvgnRed,
            fontSize = 44.sp,
            fontWeight = FontWeight.Black,
            fontFamily = FontFamily.Monospace
        )
        Text(
            text = "8-BIT  ·  ANDROID PORT",
            color = Color(0xFFB0B0B8),
            fontSize = 14.sp,
            fontFamily = FontFamily.Monospace,
            letterSpacing = 2.sp
        )
        Spacer(Modifier.height(24.dp))

        Card(
            colors = CardDefaults.cardColors(containerColor = AvgnPanel),
            shape = RoundedCornerShape(18.dp),
            modifier = Modifier.fillMaxWidth()
        ) {
            Column(Modifier.padding(18.dp)) {
                if (imported) {
                    Text("Файлы игры на месте", color = Color(0xFF9CCC65), fontSize = 12.sp)
                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = gameName?.takeIf { it.isNotBlank() } ?: "AVGN 8-bit",
                        color = Color.White,
                        fontSize = 22.sp,
                        fontWeight = FontWeight.Bold
                    )
                    if (sizeText.isNotEmpty()) {
                        Text(sizeText, color = Color(0xFF9E9EA7), fontSize = 12.sp)
                    }
                    settings.importedFolderName?.let { folder ->
                        Text(
                            "Папка: $folder",
                            color = Color(0xFF6E6E78),
                            fontSize = 11.sp
                        )
                    }
                } else {
                    Text("Файлы игры ещё не импортированы", color = Color(0xFFFFB74D), fontSize = 12.sp)
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "Нужна ваша копия AVGN 8-bit из Steam: data.win, шрифты и папки Lang, Attract, Cutscenes.",
                        color = Color(0xFFCCCCCC),
                        fontSize = 14.sp
                    )
                }
                Spacer(Modifier.height(16.dp))
                if (imported) {
                    Button(
                        onClick = onPlay,
                        modifier = Modifier.fillMaxWidth().height(56.dp),
                        shape = RoundedCornerShape(14.dp)
                    ) {
                        Text("ИГРАТЬ", fontSize = 18.sp, fontWeight = FontWeight.Bold)
                    }
                    Spacer(Modifier.height(10.dp))
                }
                OutlinedButton(onClick = onImport, modifier = Modifier.fillMaxWidth()) {
                    Text(if (imported) "Обновить файлы игры" else "Импортировать файлы игры")
                }
            }
        }

        Spacer(Modifier.height(16.dp))

        MenuButton("Сенсорное управление", "Раскладка, кнопки, размер и прозрачность", onControls)
        MenuButton("Настройки", "Графика, ориентация, вибрация", onSettings)
        MenuButton("О порте", "Что это, лицензии, исходники", onAbout)

        Spacer(Modifier.height(24.dp))
        Text(
            text = "Порт работает на Butterscotch (AGPL-3.0). Файлы самой игры не входят в APK: используются те, что вы импортировали из своей копии.",
            color = Color(0xFF6E6E78),
            fontSize = 11.sp
        )
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
fun MenuButton(title: String, subtitle: String, onClick: () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = AvgnPanel),
        shape = RoundedCornerShape(14.dp),
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
            .clickable(onClick = onClick)
    ) {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(title, color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                Text(subtitle, color = Color(0xFF9E9EA7), fontSize = 12.sp)
            }
            OutlinedButton(onClick = onClick) { Text("Открыть") }
        }
    }
}

@Composable
fun LoadingRow(text: String = "Загрузка…") {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
        CircularProgressIndicator(modifier = Modifier.width(20.dp).height(20.dp), color = AvgnRed)
        Spacer(Modifier.width(12.dp))
        Text(text, color = Color(0xFFB0B0B8))
    }
}
