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
import ru.avgn8bit.R
import ru.avgn8bit.ui.str

@Composable
fun AboutScreen(onBack: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(str(R.string.about_title), color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = onBack) { Text(str(R.string.common_back)) }
        }
        Spacer(Modifier.height(12.dp))

        Section(str(R.string.about_section_port)) {
            Body(str(R.string.about_body_port))
            Spacer(Modifier.height(8.dp))
            Body(str(R.string.about_version, BuildConfig.VERSION_NAME))
        }

        Section(str(R.string.about_section_controls)) {
            Body(str(R.string.about_body_controls))
        }

        Section(str(R.string.about_section_thanks)) {
            Body(str(R.string.about_body_thanks))
        }

        Section(str(R.string.about_section_license)) {
            Body(str(R.string.about_body_license))
        }

        Spacer(Modifier.height(24.dp))
        Body(str(R.string.about_footer))
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun Body(text: String) {
    Text(text, color = Color(0xFFCCCCCC), fontSize = 13.sp, lineHeight = 19.sp)
}
