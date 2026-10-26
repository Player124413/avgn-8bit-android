package ru.avgn8bit.screens

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ru.avgn8bit.BuildConfig

@Composable
fun AboutScreen(onBack: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("О порте", color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = onBack) { Text("Назад") }
        }
        Spacer(Modifier.height(12.dp))

        Section("AVGN 8-bit — порт для Android") {
            Body(
                "Любительский порт игры «Angry Video Game Nerd 8-bit» на Android. Он не содержит файлов игры: " +
                    "вы импортируете data.win, шрифты и видео из своей копии, а приложение исполняет игру через " +
                    "свободный движок GameMaker (Butterscotch)."
            )
            Spacer(Modifier.height(8.dp))
            Body("Версия ${BuildConfig.VERSION_NAME}. Исходники: github.com/Player124413/avgn-8bit-vita")
        }

        Section("Управление") {
            Body(
                "Кнопки, D-Pad и стик рисуются на экране. Их можно перетаскивать, менять размер, прозрачность и " +
                    "назначать на любую кнопку геймпада или клавишу. Раскладки для портрета и ландшафта хранятся " +
                    "отдельно. В игре меню «Управление» открывает тот же редактор."
            )
        }

        Section("Благодарности") {
            Body(
                "Butterscotch — свободный рантайм GameMaker (github.com/ButterscotchRunner/Butterscotch), AGPL-3.0. " +
                    "Шрифты FreeType (FTL). Игра и её содержимое принадлежат их правообладателям — этот порт не " +
                    "распространяет игру и не связан с авторами."
            )
        }

        Section("Лицензия") {
            Body(
                "Код порта распространяется по AGPL-3.0, как и используемый им рантайм. Полный текст лицензии — в " +
                    "репозитории проекта (LICENSE.runtime). Загружая файлы игры, вы используете собственную копию."
            )
        }

        Spacer(Modifier.height(24.dp))
        Body("Если что-то не запускается, пришлите лог: Настройки → Поделиться логом.")
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun Body(text: String) {
    Text(text, color = Color(0xFFCCCCCC), fontSize = 13.sp, lineHeight = 19.sp)
}
