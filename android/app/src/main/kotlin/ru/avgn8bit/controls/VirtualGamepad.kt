package ru.avgn8bit.controls

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.sp
import ru.avgn8bit.Haptics
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.roundToInt

/** Where control elements send their presses. */
interface ControlSink {
    fun key(vk: Int, down: Boolean)
    fun pad(button: Int, down: Boolean)
    fun axis(axis: Int, value: Float)
}

private val ACCENT = Color(0xFFE53935)
private val BASE = Color(0xFFFFFFFF)
private val EDIT_ACCENT = Color(0xFF4FC3F7)

/** What the overlay currently draws for one element (immutable, so Compose notices every change). */
private data class ElementState(
    val pressed: Boolean = false,
    val dirs: Int = 0,               // bit 0 up, 1 down, 2 left, 3 right
    val knob: Offset = Offset.Zero
)

/**
 * The touch controls: draws the layout and feeds [sink] from every finger.
 *
 * One pointer handler owns all fingers (instead of one per button), so multi-touch works, a thumb can slide
 * from one D-Pad direction to another, and dragging elements around in the editor is the same code path as
 * pressing them.
 */
@Composable
fun ControlOverlay(
    layout: ControlLayout,
    sink: ControlSink,
    modifier: Modifier = Modifier,
    editable: Boolean = false,
    selectedId: String? = null,
    onSelect: (String?) -> Unit = { },
    onLayoutChange: (ControlLayout) -> Unit = { },
    opacityMultiplier: Float = 1f,
    scaleMultiplier: Float = 1f,
    hapticsEnabled: Boolean = true,
    snapToGrid: Boolean = true,
    showGrid: Boolean = true
) {
    var sizePx by remember { mutableStateOf(IntSize.Zero) }
    val states = remember { mutableStateMapOf<String, ElementState>() }
    val activeBindings = remember { HashMap<String, MutableSet<Binding>>() }
    val grabbed = remember { HashMap<Long, String>() }

    val currentLayout by rememberUpdatedState(layout)
    val currentSink by rememberUpdatedState(sink)
    val currentOnSelect by rememberUpdatedState(onSelect)
    val currentOnLayoutChange by rememberUpdatedState(onLayoutChange)

    val density = LocalDensity.current

    Box(
        modifier = modifier
            .onSizeChanged { sizePx = it }
            .pointerInput(editable, sizePx) {
                val handler = GestureHandler(
                    layoutProvider = { currentLayout },
                    sink = currentSink,
                    states = states,
                    activeBindings = activeBindings,
                    grabbed = grabbed,
                    editable = editable,
                    snap = snapToGrid,
                    haptics = hapticsEnabled,
                    scale = scaleMultiplier,
                    size = { sizePx },
                    onSelect = { currentOnSelect(it) },
                    onLayoutChange = { currentOnLayoutChange(it) }
                )
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        for (change in event.changes) {
                            val id = change.id.value
                            when {
                                change.pressed && !change.previousPressed -> handler.down(id, change.position)
                                change.pressed -> handler.move(id, change.position, change.position - change.previousPosition)
                                !change.pressed && change.previousPressed -> handler.up(id)
                            }
                            if (change.pressed) change.consume()
                        }
                    }
                }
            }
    ) {
        val widthPx = sizePx.width.toFloat()
        val heightPx = sizePx.height.toFloat()
        val shorter = min(widthPx, heightPx)

        Canvas(Modifier.fillMaxSize()) {
            if (showGrid && editable) {
                var x = 0f
                while (x <= 1f) {
                    drawLine(Color(0x18FFFFFF), Offset(x * widthPx, 0f), Offset(x * widthPx, heightPx), strokeWidth = 1f)
                    x += 0.05f
                }
                var y = 0f
                while (y <= 1f) {
                    drawLine(Color(0x18FFFFFF), Offset(0f, y * heightPx), Offset(widthPx, y * heightPx), strokeWidth = 1f)
                    y += 0.05f
                }
            }

            for (element in layout.elements) {
                val center = Offset(element.x * widthPx, element.y * heightPx)
                val radius = (element.size * scaleMultiplier * shorter / 2f).coerceAtLeast(16f)
                val state = states[element.id]
                val alpha = (element.opacity * opacityMultiplier).coerceIn(0.05f, 1f)
                when (element.type) {
                    "button" -> drawButtonShape(element, center, radius, alpha, state)
                    "dpad" -> drawDpadShape(center, radius, alpha, state)
                    "stick" -> drawStickShape(center, radius, alpha, state)
                }
                if (editable) {
                    val selected = element.id == selectedId
                    val color = if (selected) EDIT_ACCENT else Color(0x66FFFFFF)
                    if (element.type == "dpad") {
                        drawRoundRect(
                            color = color,
                            topLeft = Offset(center.x - radius, center.y - radius),
                            size = Size(radius * 2, radius * 2),
                            cornerRadius = CornerRadius(radius * 0.22f),
                            style = Stroke(width = if (selected) 5f else 2f)
                        )
                    } else {
                        drawCircle(color, radius, center, style = Stroke(width = if (selected) 5f else 2f))
                    }
                    drawCircle(color, radius = 5f, center = center)
                }
            }
        }

        // Labels are drawn in a wide, free-standing box next to the element instead of inside it: the old box was a
        // square of the button's size, which cut longer names ("Esc", "Start", a triangle) in half, and the middle of
        // the button is where the finger presses and where the arrows of a D-Pad are.
        val labelSizeSp = (shorter * 0.042f).coerceAtLeast(11f)
        val labelHeightPx = labelSizeSp * density.density
        val labelWidthPx = labelHeightPx * 5f
        for (element in layout.elements) {
            if (element.label.isEmpty()) continue
            val radius = (element.size * scaleMultiplier * shorter / 2f).coerceAtLeast(16f)
            val alpha = (element.opacity * opacityMultiplier).coerceIn(0.15f, 1f)
            val centerX = element.x * widthPx
            val centerY = element.y * heightPx
            // Above the element, or below it when the element sits too close to the top edge for the text to fit.
            val above = centerY - radius - labelHeightPx >= 0f
            val top = if (above) centerY - radius - labelHeightPx else centerY + radius + labelHeightPx * 0.25f
            Box(
                modifier = Modifier
                    .offset {
                        IntOffset(
                            (centerX - labelWidthPx / 2f).roundToInt(),
                            top.roundToInt()
                        )
                    }
                    .size(with(density) { labelWidthPx.toDp() }, with(density) { labelHeightPx.toDp() })
                    .wrapContentSize(Alignment.Center, unbounded = true)
            ) {
                Text(
                    text = element.label,
                    color = BASE.copy(alpha = (alpha + 0.4f).coerceAtMost(1f)),
                    fontSize = labelSizeSp.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1
                )
            }
        }
    }
}

/** All the finger bookkeeping. Created inside the pointer handler so it always sees the live layout. */
private class GestureHandler(
    private val layoutProvider: () -> ControlLayout,
    private val sink: ControlSink,
    private val states: MutableMap<String, ElementState>,
    private val activeBindings: MutableMap<String, MutableSet<Binding>>,
    private val grabbed: MutableMap<Long, String>,
    private val editable: Boolean,
    private val snap: Boolean,
    private val haptics: Boolean,
    private val scale: Float,
    private val size: () -> IntSize,
    private val onSelect: (String?) -> Unit,
    private val onLayoutChange: (ControlLayout) -> Unit
) {
    private fun radiusOf(element: ControlElement): Float {
        val shorter = min(size().width, size().height).toFloat()
        return (element.size * scale * shorter / 2f).coerceAtLeast(16f)
    }

    private fun centerOf(element: ControlElement) =
        Offset(element.x * size().width, element.y * size().height)

    private fun elementAt(position: Offset): ControlElement? {
        val layout = layoutProvider()
        return layout.elements.lastOrNull { element ->
            val center = centerOf(element)
            val radius = radiusOf(element)
            if (element.type == "dpad") {
                abs(position.x - center.x) <= radius && abs(position.y - center.y) <= radius
            } else {
                hypot(position.x - center.x, position.y - center.y) <= radius
            }
        }
    }

    private fun setState(id: String, transform: (ElementState) -> ElementState) {
        states[id] = transform(states[id] ?: ElementState())
    }

    private fun emit(binding: Binding, down: Boolean) {
        when (binding.kind) {
            BindKind.KEY -> sink.key(binding.code, down)
            BindKind.PAD -> sink.pad(binding.code, down)
            BindKind.AXIS -> sink.axis(binding.code, if (down) binding.dir.toFloat() else 0f)
        }
    }

    fun release(id: String) {
        val active = activeBindings.remove(id) ?: emptySet()
        active.forEach { emit(it, false) }
        states[id] = ElementState()
    }

    private fun apply(id: String, wanted: Set<Binding>) {
        val current = activeBindings.getOrPut(id) { LinkedHashSet() }
        current.filterNot { wanted.contains(it) }.forEach { emit(it, false) }
        wanted.filterNot { current.contains(it) }.forEach { emit(it, true) }
        current.clear()
        current.addAll(wanted)
        setState(id) { it.copy(pressed = wanted.isNotEmpty()) }
    }

    fun down(id: Long, position: Offset) {
        val element = elementAt(position)
        if (element == null) {
            if (editable) onSelect(null)
            return
        }
        if (grabbed.containsKey(id)) return
        grabbed[id] = element.id
        if (editable) {
            onSelect(element.id)
            return
        }
        if (element.haptic && haptics) Haptics.tick()
        when (element.type) {
            "button" -> apply(element.id, element.bindings.toSet())
            "dpad" -> updateDpad(element, position, true)
            "stick" -> updateStick(element, position)
        }
    }

    fun move(id: Long, position: Offset, delta: Offset) {
        val elementId = grabbed[id] ?: return
        val element = layoutProvider().element(elementId) ?: return
        if (editable) {
            var nx = (element.x + delta.x / size().width).coerceIn(0.02f, 0.98f)
            var ny = (element.y + delta.y / size().height).coerceIn(0.02f, 0.98f)
            if (snap) {
                val step = 0.02f
                nx = (nx / step).roundToInt() * step
                ny = (ny / step).roundToInt() * step
            }
            onLayoutChange(layoutProvider().withElement(element.copy(x = nx, y = ny)))
            return
        }
        when (element.type) {
            "dpad" -> updateDpad(element, position, true)
            "stick" -> updateStick(element, position)
        }
    }

    fun up(id: Long) {
        val elementId = grabbed.remove(id) ?: return
        if (editable) return
        release(elementId)
        val element = layoutProvider().element(elementId)
        if (element != null && (element.type == "dpad" || element.type == "stick") && element.emitAxes) {
            sink.axis(0, 0f)
            sink.axis(1, 0f)
        }
    }

    private fun updateDpad(element: ControlElement, position: Offset, down: Boolean) {
        val center = centerOf(element)
        val radius = radiusOf(element)
        val dx = (position.x - center.x) / radius
        val dy = (position.y - center.y) / radius
        var dirs = 0
        if (down) {
            if (dy < -0.34f) dirs = dirs or 1
            if (dy > 0.34f) dirs = dirs or 2
            if (dx < -0.34f) dirs = dirs or 4
            if (dx > 0.34f) dirs = dirs or 8
            if (abs(dx) > 0.30f && abs(dy) > 0.30f) {
                // Diagonals: both axes at once.
                dirs = dirs or (if (dx < 0) 4 else 8)
                dirs = dirs or (if (dy < 0) 1 else 2)
            }
        }
        val previous = states[element.id]?.dirs ?: 0
        if (dirs != 0 && dirs != previous && element.haptic && haptics) Haptics.tick()

        val wanted = LinkedHashSet<Binding>()
        if (dirs and 1 != 0) wanted.addAll(element.up)
        if (dirs and 2 != 0) wanted.addAll(element.down)
        if (dirs and 4 != 0) wanted.addAll(element.left)
        if (dirs and 8 != 0) wanted.addAll(element.right)
        apply(element.id, wanted)
        setState(element.id) { it.copy(dirs = dirs) }
        if (element.emitAxes) {
            sink.axis(0, when { dirs and 4 != 0 -> -1f; dirs and 8 != 0 -> 1f; else -> 0f })
            sink.axis(1, when { dirs and 1 != 0 -> -1f; dirs and 2 != 0 -> 1f; else -> 0f })
        }
    }

    private fun updateStick(element: ControlElement, position: Offset) {
        val center = centerOf(element)
        val radius = radiusOf(element)
        var dx = (position.x - center.x) / radius
        var dy = (position.y - center.y) / radius
        val length = hypot(dx, dy)
        if (length > 1f) {
            dx /= length
            dy /= length
        }
        val magnitude = hypot(dx, dy)
        val nx = if (magnitude < element.deadzone) 0f else dx
        val ny = if (magnitude < element.deadzone) 0f else dy

        var dirs = 0
        if (ny < -0.45f) dirs = dirs or 1
        if (ny > 0.45f) dirs = dirs or 2
        if (nx < -0.45f) dirs = dirs or 4
        if (nx > 0.45f) dirs = dirs or 8
        val previous = states[element.id]?.dirs ?: 0
        if (dirs != 0 && dirs != previous && element.haptic && haptics) Haptics.tick()

        val wanted = LinkedHashSet<Binding>()
        if (dirs and 1 != 0) wanted.addAll(element.up.ifEmpty { listOf(Binding.pad(12)) })
        if (dirs and 2 != 0) wanted.addAll(element.down.ifEmpty { listOf(Binding.pad(13)) })
        if (dirs and 4 != 0) wanted.addAll(element.left.ifEmpty { listOf(Binding.pad(14)) })
        if (dirs and 8 != 0) wanted.addAll(element.right.ifEmpty { listOf(Binding.pad(15)) })
        apply(element.id, wanted)
        setState(element.id) { it.copy(dirs = dirs, knob = Offset(nx * radius * 0.55f, ny * radius * 0.55f)) }
        if (element.emitAxes) {
            sink.axis(0, nx)
            sink.axis(1, ny)
        }
    }
}

/** Releases everything a layout can hold, used when the game is paused or the controls are hidden. */
fun releaseAllControls(layout: ControlLayout, sink: ControlSink) {
    val sent = HashSet<Binding>()
    layout.elements.forEach { element ->
        (element.bindings + element.up + element.down + element.left + element.right)
            .filter { sent.add(it) }
            .forEach { binding ->
                when (binding.kind) {
                    BindKind.KEY -> sink.key(binding.code, false)
                    BindKind.PAD -> sink.pad(binding.code, false)
                    BindKind.AXIS -> sink.axis(binding.code, 0f)
                }
            }
    }
    sink.axis(0, 0f)
    sink.axis(1, 0f)
}

private fun DrawScope.drawButtonShape(
    element: ControlElement,
    center: Offset,
    radius: Float,
    alpha: Float,
    state: ElementState?
) {
    val pressed = state?.pressed == true
    val r = if (pressed) radius * 1.05f else radius
    val fill = if (pressed) ACCENT.copy(alpha = (alpha + 0.30f).coerceAtMost(0.95f)) else BASE.copy(alpha = alpha * 0.45f)
    val border = if (pressed) ACCENT.copy(alpha = 0.95f) else BASE.copy(alpha = (alpha + 0.25f).coerceAtMost(0.9f))

    when (element.style) {
        "square" -> {
            drawRect(fill, Offset(center.x - r, center.y - r), Size(r * 2, r * 2))
            drawRect(border, Offset(center.x - r, center.y - r), Size(r * 2, r * 2), style = Stroke(width = 3f))
        }
        "rounded" -> {
            drawRoundRect(fill, Offset(center.x - r, center.y - r), Size(r * 2, r * 2), CornerRadius(r * 0.35f))
            drawRoundRect(border, Offset(center.x - r, center.y - r), Size(r * 2, r * 2), CornerRadius(r * 0.35f), style = Stroke(width = 3f))
        }
        else -> {
            drawCircle(fill, radius = r, center = center)
            drawCircle(border, radius = r, center = center, style = Stroke(width = 3f))
        }
    }
}

private fun DrawScope.drawDpadShape(center: Offset, radius: Float, alpha: Float, state: ElementState?) {
    val dirs = state?.dirs ?: 0
    drawRoundRect(
        color = BASE.copy(alpha = alpha * 0.30f),
        topLeft = Offset(center.x - radius, center.y - radius),
        size = Size(radius * 2, radius * 2),
        cornerRadius = CornerRadius(radius * 0.22f)
    )
    drawRoundRect(
        color = BASE.copy(alpha = (alpha + 0.25f).coerceAtMost(0.85f)),
        topLeft = Offset(center.x - radius, center.y - radius),
        size = Size(radius * 2, radius * 2),
        cornerRadius = CornerRadius(radius * 0.22f),
        style = Stroke(width = 3f)
    )

    fun arrow(points: List<Offset>, active: Boolean) {
        val path = Path().apply {
            moveTo(points[0].x, points[0].y)
            lineTo(points[1].x, points[1].y)
            lineTo(points[2].x, points[2].y)
            close()
        }
        drawPath(path, if (active) ACCENT.copy(alpha = 0.95f) else BASE.copy(alpha = (alpha + 0.30f).coerceAtMost(0.9f)))
    }

    val t = radius * 0.80f
    val w = radius * 0.34f
    arrow(listOf(Offset(center.x, center.y - t), Offset(center.x - w, center.y - radius * 0.30f), Offset(center.x + w, center.y - radius * 0.30f)), dirs and 1 != 0)
    arrow(listOf(Offset(center.x, center.y + t), Offset(center.x - w, center.y + radius * 0.30f), Offset(center.x + w, center.y + radius * 0.30f)), dirs and 2 != 0)
    arrow(listOf(Offset(center.x - t, center.y), Offset(center.x - radius * 0.30f, center.y - w), Offset(center.x - radius * 0.30f, center.y + w)), dirs and 4 != 0)
    arrow(listOf(Offset(center.x + t, center.y), Offset(center.x + radius * 0.30f, center.y - w), Offset(center.x + radius * 0.30f, center.y + w)), dirs and 8 != 0)
}

private fun DrawScope.drawStickShape(center: Offset, radius: Float, alpha: Float, state: ElementState?) {
    val knob = state?.knob ?: Offset.Zero
    drawCircle(BASE.copy(alpha = alpha * 0.22f), radius = radius, center = center)
    drawCircle(BASE.copy(alpha = (alpha + 0.2f).coerceAtMost(0.8f)), radius = radius, center = center, style = Stroke(width = 3f))
    val active = knob.getDistance() > 1f
    drawCircle(
        color = if (active) ACCENT.copy(alpha = 0.85f) else BASE.copy(alpha = (alpha + 0.25f).coerceAtMost(0.85f)),
        radius = radius * 0.42f,
        center = center + knob
    )
}
