package ru.avgn8bit.controls

import android.content.Context
import androidx.annotation.StringRes
import kotlinx.serialization.Serializable
import ru.avgn8bit.R
import kotlinx.serialization.json.Json
import java.util.UUID

/**
 * The touch controls are described by a small JSON document: a list of elements with a normalized position
 * (0..1 of the screen) and a size (a fraction of the screen's shorter side), plus what each element sends to
 * the game. Two are kept: one for landscape and one for portrait.
 *
 * An element can send several things at once (a gamepad button *and* a key), which is how the default layout
 * works: every button looks like a controller to the game and also presses a keyboard key, so games that only
 * listen for one of the two still work.
 */

object BindKind {
    const val KEY = "key"   // GameMaker vk_ code
    const val PAD = "pad"   // gamepad button slot, see [PadButton]
    const val AXIS = "axis" // gamepad axis, [Binding.dir] is -1 or +1
}

@Serializable
data class Binding(
    val kind: String,
    val code: Int,
    val dir: Int = 0
) {
    companion object {
        fun key(vk: Int) = Binding(BindKind.KEY, vk)
        fun pad(button: Int) = Binding(BindKind.PAD, button)
        fun axis(axis: Int, dir: Int) = Binding(BindKind.AXIS, axis, dir)
    }
}

/** The runtime's canonical gamepad slots (src/runner_gamepad.h / runner_gamepad.c). */
enum class PadButton(val index: Int, @StringRes val labelRes: Int) {
    A(0, R.string.pad_a),
    B(1, R.string.pad_b),
    X(2, R.string.pad_x),
    Y(3, R.string.pad_y),
    L1(4, R.string.pad_l1),
    R1(5, R.string.pad_r1),
    L2(6, R.string.pad_l2),
    R2(7, R.string.pad_r2),
    SELECT(8, R.string.pad_select),
    START(9, R.string.pad_start),
    L3(10, R.string.pad_l3),
    R3(11, R.string.pad_r3),
    DPAD_UP(12, R.string.pad_dpad_up),
    DPAD_DOWN(13, R.string.pad_dpad_down),
    DPAD_LEFT(14, R.string.pad_dpad_left),
    DPAD_RIGHT(15, R.string.pad_dpad_right),
    HOME(16, R.string.pad_home);

    companion object {
        fun byIndex(index: Int) = entries.firstOrNull { it.index == index }
    }
}

/**
 * A keyboard key a button can send, named the way GameMaker names keys. Keys that have a name of their own carry a
 * string resource; letters, digits and function keys are their own name, so [literal] covers them.
 */
data class KeyOption(val code: Int, val literal: String = "", @StringRes val labelRes: Int = 0) {
    companion object {
        private val letters = ('A'..'Z').map { KeyOption(it.code, it.toString()) }
        private val digits = ('0'..'9').map { KeyOption(it.code, it.toString()) }
        private val fkeys = (1..12).map { KeyOption(111 + it, "F$it") }

        val all: List<KeyOption> = listOf(
            KeyOption(37, "←", R.string.key_left),
            KeyOption(38, "↑", R.string.key_up),
            KeyOption(39, "→", R.string.key_right),
            KeyOption(40, "↓", R.string.key_down),
            KeyOption(13, "Enter", R.string.key_enter),
            KeyOption(27, "Esc", R.string.key_escape),
            KeyOption(32, "Space", R.string.key_space),
            KeyOption(16, "Shift", R.string.key_shift),
            KeyOption(17, "Ctrl", R.string.key_control),
            KeyOption(18, "Alt", R.string.key_alt),
            KeyOption(9, "Tab", R.string.key_tab),
            KeyOption(8, "Bksp", R.string.key_backspace),
            KeyOption(46, "Del", R.string.key_delete),
            KeyOption(36, "Home", R.string.key_home),
            KeyOption(35, "End", R.string.key_end),
            KeyOption(33, "PgUp", R.string.key_page_up),
            KeyOption(34, "PgDn", R.string.key_page_down)
        ) + letters + digits + fkeys

        private val byCodeMap = all.associateBy { it.code }
        fun byCode(code: Int) = byCodeMap[code]
    }
}

@Serializable
data class ControlElement(
    val id: String,
    val type: String,                 // "button" | "dpad" | "stick"
    val x: Float,                     // 0..1, centre of the element on the screen
    val y: Float,
    val size: Float,                  // diameter, as a fraction of the screen's shorter side
    val label: String = "",
    val opacity: Float = 0.45f,
    val style: String = "circle",     // "circle" | "rounded" | "square"
    val bindings: List<Binding> = emptyList(),
    val up: List<Binding> = emptyList(),
    val down: List<Binding> = emptyList(),
    val left: List<Binding> = emptyList(),
    val right: List<Binding> = emptyList(),
    val emitAxes: Boolean = true,     // dpad/stick also drive the left analogue stick
    val deadzone: Float = 0.22f,
    val haptic: Boolean = true
) {
    companion object {
        fun button(x: Float, y: Float, size: Float, label: String, bindings: List<Binding>, opacity: Float = 0.45f) =
            ControlElement(UUID.randomUUID().toString(), "button", x, y, size, label, opacity, "circle", bindings)

        fun dpad(x: Float, y: Float, size: Float, pad: Boolean = true, keys: Boolean = true, opacity: Float = 0.42f) =
            ControlElement(
                id = UUID.randomUUID().toString(),
                type = "dpad",
                x = x,
                y = y,
                size = size,
                label = "",
                opacity = opacity,
                style = "rounded",
                up = listOfNotNull(if (pad) Binding.pad(12) else null, if (keys) Binding.key(38) else null),
                down = listOfNotNull(if (pad) Binding.pad(13) else null, if (keys) Binding.key(40) else null),
                left = listOfNotNull(if (pad) Binding.pad(14) else null, if (keys) Binding.key(37) else null),
                right = listOfNotNull(if (pad) Binding.pad(15) else null, if (keys) Binding.key(39) else null)
            )

        fun stick(x: Float, y: Float, size: Float, opacity: Float = 0.35f) = ControlElement(
            id = UUID.randomUUID().toString(),
            type = "stick",
            x = x,
            y = y,
            size = size,
            label = "",
            opacity = opacity,
            style = "circle"
        )
    }
}

@Serializable
data class ControlLayout(
    val version: Int = 1,
    val name: String = "AVGN 8-bit",
    val elements: List<ControlElement> = emptyList()
) {
    fun withElement(element: ControlElement) =
        copy(elements = elements.map { if (it.id == element.id) element else it })

    fun withoutElement(id: String) = copy(elements = elements.filterNot { it.id == id })

    fun element(id: String?) = elements.firstOrNull { it.id == id }
}

/**
 * The three button schemes the launcher offers. The layout behind them is the same — a D-Pad, the three action
 * buttons the game uses (jump, shoot, item) and Start/Back — only the names on the buttons change, so the player can
 * have the buttons of the controller they are used to. Whatever the scheme, a button sends both a gamepad button and a
 * key (see the note at the top of this file), which is all the game itself ever sees.
 */
enum class ButtonScheme(@StringRes val labelRes: Int) {
    PLAYSTATION(R.string.preset_playstation),
    XBOX(R.string.preset_xbox),
    PC(R.string.preset_pc);

    /** Names of the action buttons, in the order jump, shoot, item. */
    val actionLabels: List<String>
        get() = when (this) {
            PLAYSTATION -> listOf("✕", "○", "□")
            XBOX -> listOf("A", "B", "X")
            PC -> listOf("Z", "X", "C")
        }

}

/** Ready-made layouts: one per [ButtonScheme], in the two orientations. */
object Presets {
    private const val JUMP_KEY = 90      // Z
    private const val SHOOT_KEY = 88     // X
    private const val ITEM_KEY = 67      // C
    private const val START_KEY = 13     // Enter
    private const val BACK_KEY = 27      // Escape

    /** The layout for a scheme, sized for the orientation the screen is in. */
    fun layout(scheme: ButtonScheme, portrait: Boolean): ControlLayout =
        if (portrait) portraitLayout(scheme) else landscapeLayout(scheme)

    /** D-Pad on the left, the three action buttons under the right thumb, Start and Back in the top right corner. */
    private fun landscapeLayout(scheme: ButtonScheme): ControlLayout {
        val (jump, shoot, item) = scheme.actionLabels
        return ControlLayout(
            name = scheme.name,
            elements = listOf(
                ControlElement.dpad(x = 0.15f, y = 0.70f, size = 0.42f),
                ControlElement.button(0.845f, 0.735f, 0.20f, jump, listOf(Binding.pad(0), Binding.key(JUMP_KEY)), 0.50f),
                ControlElement.button(0.700f, 0.855f, 0.20f, shoot, listOf(Binding.pad(1), Binding.key(SHOOT_KEY)), 0.50f),
                ControlElement.button(0.665f, 0.615f, 0.165f, item, listOf(Binding.pad(2), Binding.key(ITEM_KEY)), 0.42f),
                ControlElement.button(0.955f, 0.095f, 0.115f, "≡", listOf(Binding.pad(9), Binding.key(START_KEY)), 0.30f),
                ControlElement.button(0.865f, 0.095f, 0.10f, "Esc", listOf(Binding.pad(8), Binding.key(BACK_KEY)), 0.24f)
            )
        )
    }

    /** Both thumbs on the screen edge, for phones held in portrait. */
    private fun portraitLayout(scheme: ButtonScheme): ControlLayout {
        val (jump, shoot, item) = scheme.actionLabels
        return ControlLayout(
            name = scheme.name,
            elements = listOf(
                ControlElement.dpad(x = 0.19f, y = 0.80f, size = 0.40f),
                ControlElement.button(0.80f, 0.745f, 0.19f, jump, listOf(Binding.pad(0), Binding.key(JUMP_KEY)), 0.50f),
                ControlElement.button(0.665f, 0.865f, 0.19f, shoot, listOf(Binding.pad(1), Binding.key(SHOOT_KEY)), 0.50f),
                ControlElement.button(0.885f, 0.885f, 0.155f, item, listOf(Binding.pad(2), Binding.key(ITEM_KEY)), 0.42f),
                ControlElement.button(0.90f, 0.545f, 0.11f, "≡", listOf(Binding.pad(9), Binding.key(START_KEY)), 0.30f)
            )
        )
    }

    /** What a fresh install starts with. */
    val defaultScheme = ButtonScheme.PLAYSTATION

    fun defaultLandscape(): ControlLayout = layout(defaultScheme, portrait = false)
    fun defaultPortrait(): ControlLayout = layout(defaultScheme, portrait = true)
}

/** Loads and saves the layouts. One per screen orientation, so both can be tuned separately. */
class ControlsStore(context: Context) {
    private val prefs = context.getSharedPreferences("avgn8bit-controls", Context.MODE_PRIVATE)
    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
        prettyPrint = true
    }

    fun load(portrait: Boolean): ControlLayout {
        val key = if (portrait) "layout_portrait" else "layout_landscape"
        val raw = prefs.getString(key, null) ?: return if (portrait) Presets.defaultPortrait() else Presets.defaultLandscape()
        return try {
            json.decodeFromString(ControlLayout.serializer(), raw)
        } catch (t: Throwable) {
            if (portrait) Presets.defaultPortrait() else Presets.defaultLandscape()
        }
    }

    fun save(portrait: Boolean, layout: ControlLayout) {
        val key = if (portrait) "layout_portrait" else "layout_landscape"
        prefs.edit().putString(key, json.encodeToString(ControlLayout.serializer(), layout)).apply()
    }

    fun reset(portrait: Boolean): ControlLayout {
        val layout = if (portrait) Presets.defaultPortrait() else Presets.defaultLandscape()
        save(portrait, layout)
        return layout
    }

    fun export(layout: ControlLayout): String = json.encodeToString(ControlLayout.serializer(), layout)

    fun import(raw: String): ControlLayout? = try {
        json.decodeFromString(ControlLayout.serializer(), raw)
    } catch (_: Throwable) {
        null
    }
}
