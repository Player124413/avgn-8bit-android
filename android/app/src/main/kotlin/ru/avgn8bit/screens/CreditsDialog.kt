package ru.avgn8bit.screens

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ru.avgn8bit.R
import ru.avgn8bit.ui.AvgnPanel
import ru.avgn8bit.ui.str

/**
 * The thank-you window: the PS Vita port this one is built on, and the author's channel.
 *
 * It opens by itself the first time the launcher starts ([ru.avgn8bit.AppSettings.creditsSeen]) and can be opened
 * again from About. The two links are tappable, so the repository and the channel are one touch away.
 */
@Composable
fun CreditsDialog(onDismiss: () -> Unit) {
    val uriHandler = LocalUriHandler.current
    // A phone without a browser, or a system that refuses the intent, must not crash the launcher.
    fun open(url: String) {
        try {
            uriHandler.openUri(url)
        } catch (_: Throwable) {
            // Nothing to open it with: the address is still on screen and can be typed by hand.
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = AvgnPanel,
        title = {
            Text(str(R.string.credits_title), color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold)
        },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Body(str(R.string.credits_intro))
                Spacer(Modifier.height(8.dp))
                LinkButton("github.com/conop-8888/avgn-8bit-vita") {
                    open("https://github.com/conop-8888/avgn-8bit-vita")
                }
                Spacer(Modifier.height(16.dp))
                Body(str(R.string.credits_channel))
                Spacer(Modifier.height(8.dp))
                LinkButton("t.me/player1444ports") {
                    open("https://t.me/player1444ports")
                }
                Spacer(Modifier.height(16.dp))
                Text(str(R.string.credits_footer), color = Color(0xFF9E9EA7), fontSize = 12.sp, lineHeight = 17.sp)
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(str(R.string.common_close)) }
        }
    )
}

@Composable
private fun Body(text: String) {
    Text(text, color = Color(0xFFCCCCCC), fontSize = 13.sp, lineHeight = 19.sp)
}

/** A link drawn as a full-width button: easy to hit, and the address stays readable. */
@Composable
private fun LinkButton(label: String, onClick: () -> Unit) {
    OutlinedButton(onClick = onClick, modifier = Modifier.fillMaxWidth()) {
        Text(label, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
    }
}
