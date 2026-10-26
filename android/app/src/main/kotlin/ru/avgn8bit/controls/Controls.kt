package ru.avgn8bit.controls

import android.content.Context
import kotlinx.serialization.Serializable
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
    val label: String
        get() = when (kind) {
            BindKind.KEY -> KeyOption.byCode(code)?.label ?: "Клавиша $code"
            BindKind.PAD -> PadButton.byIndex(code)?.label ?: "Геймпад $code"
            BindKind.AXIS -> "Ось $code${if (dir < 0) " −" else " +"}"
            else -> "?"
        }

    companion object {
        fun key(vk: Int) = Binding(BindKind.KEY, vk)
        fun pad(button: Int) = Binding(BindKind.PAD, button)
        fun axis(axis: Int, dir: Int) = Binding(BindKind.AXIS, axis, dir)
    }
}

/** The runtime's canonical gamepad slots (src/runner_gamepad.h / runner_gamepad.c). */
enum class PadButton(val index: Int, val label: String, val short: String) {
    A(0, "A (низ, Cross)", "A"),
    B(1, "B (право, Circle)", "B"),
    X(2, "X (лево, Square)", "X"),
    Y(3, "Y (верх, Triangle)", "Y"),
    L1(4, "L1 / LB", "L1"),
    R1(5, "R1 / RB", "R1"),
    L2(6, "L2 / LT", "L2"),
    R2(7, "R2 / RT", "R2"),
    SELECT(8, "Select / Back", "Sel"),
    START(9, "Start", "Start"),
    L3(10, "L3 (нажатие стика)", "L3"),
    R3(11, "R3", "R3"),
    DPAD_UP(12, "D-Pad вверх", "↑"),
    DPAD_DOWN(13, "D-Pad вниз", "↓"),
    DPAD_LEFT(14, "D-Pad влево", "←"),
    DPAD_RIGHT(15, "D-Pad вправо", "→"),
    HOME(16, "Home / Guide", "⌂");

    companion object {
        fun byIndex(index: Int) = entries.firstOrNull { it.index == index }
    }
}

/** A keyboard key a button can send, named the way GameMaker names keys. */
data class KeyOption(val code: Int, val label: String, val short: String = label) {
    companion object {
        private val letters = ('A'..'Z').map { KeyOption(it.code, it.toString()) }
        private val digits = ('0'..'9').map { KeyOption(it.code, it.toString()) }
        private val fkeys = (1..12).map { KeyOption(111 + it, "F$it") }

        val all: List<KeyOption> = listOf(
            KeyOption(37, "Стрелка влево", "←"),
            KeyOption(38, "Стрелка вверх", "↑"),
            KeyOption(39, "Стрелка вправо", "→"),
            KeyOption(40, "Стрелка вниз", "↓"),
            KeyOption(13, "Enter"),
            KeyOption(27, "Escape", "Esc"),
            KeyOption(32, "Space", "␣"),
            KeyOption(16, "Shift"),
            KeyOption(17, "Control", "Ctrl"),
            KeyOption(18, "Alt"),
            KeyOption(9, "Tab"),
            KeyOption(8, "Backspace", "Bksp"),
            KeyOption(46, "Delete", "Del"),
            KeyOption(36, "Home"),
            KeyOption(35, "End"),
            KeyOption(33, "Page Up", "PgUp"),
            KeyOption(34, "Page Down", "PgDn")
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

/** Ready-made layouts. AVGN 8-bit is a Mega Man style game: move, jump, shoot, and one more button for items. */
object Presets {
    private const val JUMP_KEY = 90      // Z
    private const val SHOOT_KEY = 88     // X
    private const val ITEM_KEY = 67      // C

    /** Three buttons: jump (A), shoot (B), item (X) — the modern control scheme of the game. */
    fun avgnThreeButtons(): ControlLayout = ControlLayout(
        name = "AVGN 8-bit",
        elements = listOf(
            ControlElement.dpad(x = 0.15f, y = 0.70f, size = 0.42f),
            ControlElement.button(0.845f, 0.735f, 0.20f, "A", listOf(Binding.pad(0), Binding.key(JUMP_KEY)), 0.50f),
            ControlElement.button(0.700f, 0.855f, 0.20f, "B", listOf(Binding.pad(1), Binding.key(SHOOT_KEY)), 0.50f),
            ControlElement.button(0.665f, 0.615f, 0.165f, "X", listOf(Binding.pad(2), Binding.key(ITEM_KEY)), 0.42f),
            ControlElement.button(0.955f, 0.095f, 0.115f, "≡", listOf(Binding.pad(9), Binding.key(13)), 0.30f),
            ControlElement.button(0.865f, 0.095f, 0.10f, "Esc", listOf(Binding.pad(8), Binding.key(27)), 0.24f)
        )
    )

    /** The NES layout the game also ships with: two buttons and a D-Pad. */
    fun nes(): ControlLayout = ControlLayout(
        name = "NES (2 кнопки)",
        elements = listOf(
            ControlElement.dpad(x = 0.15f, y = 0.70f, size = 0.42f),
            ControlElement.button(0.815f, 0.790f, 0.21f, "A", listOf(Binding.pad(0), Binding.key(JUMP_KEY)), 0.50f),
            ControlElement.button(0.660f, 0.855f, 0.21f, "B", listOf(Binding.pad(1), Binding.key(SHOOT_KEY)), 0.50f),
            ControlElement.button(0.955f, 0.095f, 0.115f, "≡", listOf(Binding.pad(9), Binding.key(13)), 0.30f)
        )
    )

    /** Analogue stick on the left (also works as a D-Pad) and the three buttons. */
    fun stickLayout(): ControlLayout = ControlLayout(
        name = "Стик + кнопки",
        elements = listOf(
            ControlElement.stick(x = 0.16f, y = 0.70f, size = 0.46f),
            ControlElement.button(0.845f, 0.735f, 0.20f, "A", listOf(Binding.pad(0), Binding.key(JUMP_KEY)), 0.50f),
            ControlElement.button(0.700f, 0.855f, 0.20f, "B", listOf(Binding.pad(1), Binding.key(SHOOT_KEY)), 0.50f),
            ControlElement.button(0.665f, 0.615f, 0.165f, "X", listOf(Binding.pad(2), Binding.key(ITEM_KEY)), 0.42f),
            ControlElement.button(0.955f, 0.095f, 0.115f, "≡", listOf(Binding.pad(9), Binding.key(13)), 0.30f)
        )
    )

    /** Both thumbs on the screen edge, for phones held in portrait. */
    fun portrait(): ControlLayout = ControlLayout(
        name = "AVGN 8-bit (портрет)",
        elements = listOf(
            ControlElement.dpad(x = 0.19f, y = 0.80f, size = 0.40f),
            ControlElement.button(0.80f, 0.745f, 0.19f, "A", listOf(Binding.pad(0), Binding.key(JUMP_KEY)), 0.50f),
            ControlElement.button(0.665f, 0.865f, 0.19f, "B", listOf(Binding.pad(1), Binding.key(SHOOT_KEY)), 0.50f),
            ControlElement.button(0.885f, 0.885f, 0.155f, "X", listOf(Binding.pad(2), Binding.key(ITEM_KEY)), 0.42f),
            ControlElement.button(0.90f, 0.545f, 0.11f, "≡", listOf(Binding.pad(9), Binding.key(13)), 0.30f)
        )
    )

    fun empty(): ControlLayout = ControlLayout(name = "Пустая", elements = emptyList())

    fun defaultLandscape(): ControlLayout = avgnThreeButtons()
    fun defaultPortrait(): ControlLayout = portrait()
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
