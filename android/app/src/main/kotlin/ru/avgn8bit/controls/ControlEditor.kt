package ru.avgn8bit.controls

import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * The layout editor: drag anything anywhere, then tune it in the panel at the bottom.
 *
 * Everything the user changes is written back immediately through [onLayoutChange], so the game itself always
 * sees the same layout the editor draws.
 */
@Composable
fun ControlEditorScreen(
    layout: ControlLayout,
    onLayoutChange: (ControlLayout) -> Unit,
    onDone: () -> Unit,
    modifier: Modifier = Modifier,
    title: String = "Управление"
) {
    var selectedId by remember { mutableStateOf<String?>(null) }
    var panelCollapsed by remember { mutableStateOf(false) }
    var presetMenu by remember { mutableStateOf(false) }
    var addMenu by remember { mutableStateOf(false) }
    var bindingsFor by remember { mutableStateOf<ControlElement?>(null) }

    val selected = layout.element(selectedId)

    Box(modifier.fillMaxSize().background(Color(0xFF121216))) {
        ControlOverlay(
            layout = layout,
            sink = NoopSink,
            modifier = Modifier.fillMaxSize(),
            editable = true,
            selectedId = selectedId,
            onSelect = { selectedId = it },
            onLayoutChange = onLayoutChange,
            opacityMultiplier = 1f,
            scaleMultiplier = 1f,
            hapticsEnabled = false,
            snapToGrid = true,
            showGrid = true
        )

        // Top bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(Color(0xCC1A1A20))
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(title, color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
            Spacer(Modifier.width(12.dp))
            Text("${layout.elements.size} элем.", color = Color(0xFF9E9EA7), fontSize = 12.sp)
            Spacer(Modifier.weight(1f))
            Box {
                OutlinedButton(onClick = { presetMenu = true }) {
                    Text("Пресет")
                    Icon(Icons.Filled.ArrowDropDown, contentDescription = null)
                }
                DropdownMenu(expanded = presetMenu, onDismissRequest = { presetMenu = false }) {
                    DropdownMenuItem(text = { Text("AVGN — 3 кнопки") }, onClick = {
                        onLayoutChange(Presets.avgnThreeButtons()); selectedId = null; presetMenu = false
                    })
                    DropdownMenuItem(text = { Text("NES — 2 кнопки") }, onClick = {
                        onLayoutChange(Presets.nes()); selectedId = null; presetMenu = false
                    })
                    DropdownMenuItem(text = { Text("Стик + кнопки") }, onClick = {
                        onLayoutChange(Presets.stickLayout()); selectedId = null; presetMenu = false
                    })
                    DropdownMenuItem(text = { Text("Пустая раскладка") }, onClick = {
                        onLayoutChange(Presets.empty()); selectedId = null; presetMenu = false
                    })
                }
            }
            Spacer(Modifier.width(8.dp))
            Button(onClick = onDone) { Text("Готово") }
        }

        // Bottom panel
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .background(Color(0xF21A1A20))
                .padding(12.dp)
        ) {
            if (panelCollapsed) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Панель скрыта — двигать элементы можно и так", color = Color(0xFFB0B0B8), fontSize = 12.sp)
                    Spacer(Modifier.weight(1f))
                    IconButton(onClick = { panelCollapsed = false }) {
                        Icon(Icons.Filled.KeyboardArrowUp, contentDescription = "Развернуть", tint = Color.White)
                    }
                }
            } else {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box {
                        OutlinedButton(onClick = { addMenu = true }) {
                            Icon(Icons.Filled.Add, contentDescription = null)
                            Spacer(Modifier.width(4.dp))
                            Text("Добавить")
                        }
                        DropdownMenu(expanded = addMenu, onDismissRequest = { addMenu = false }) {
                            DropdownMenuItem(text = { Text("Кнопка") }, onClick = {
                                val element = ControlElement.button(0.8f, 0.8f, 0.18f, "A", listOf(Binding.pad(0)))
                                onLayoutChange(layout.copy(elements = layout.elements + element))
                                selectedId = element.id
                                addMenu = false
                            })
                            DropdownMenuItem(text = { Text("D-Pad") }, onClick = {
                                val element = ControlElement.dpad(0.2f, 0.75f, 0.42f)
                                onLayoutChange(layout.copy(elements = layout.elements + element))
                                selectedId = element.id
                                addMenu = false
                            })
                            DropdownMenuItem(text = { Text("Стик") }, onClick = {
                                val element = ControlElement.stick(0.2f, 0.75f, 0.46f)
                                onLayoutChange(layout.copy(elements = layout.elements + element))
                                selectedId = element.id
                                addMenu = false
                            })
                        }
                    }
                    Spacer(Modifier.width(12.dp))
                    Text(
                        text = selected?.let { describeElement(it) } ?: "Выберите элемент или добавьте новый",
                        color = Color(0xFFB0B0B8),
                        fontSize = 12.sp,
                        modifier = Modifier.weight(1f)
                    )
                    IconButton(onClick = { panelCollapsed = true }) {
                        Icon(Icons.Filled.KeyboardArrowDown, contentDescription = "Свернуть", tint = Color.White)
                    }
                }

                if (selected != null) {
                    Spacer(Modifier.height(8.dp))
                    ElementProperties(
                        element = selected,
                        onChange = { updated -> onLayoutChange(layout.withElement(updated)) },
                        onEditBindings = { bindingsFor = selected },
                        onDuplicate = {
                            val copy = selected.copy(id = java.util.UUID.randomUUID().toString())
                            onLayoutChange(layout.copy(elements = layout.elements + copy))
                            selectedId = copy.id
                        },
                        onDelete = {
                            onLayoutChange(layout.withoutElement(selected.id))
                            selectedId = null
                        }
                    )
                }
            }
        }
    }

    bindingsFor?.let { element ->
        BindingDialog(
            element = element,
            onDismiss = { bindingsFor = null },
            onChange = { updated -> onLayoutChange(layout.withElement(updated)); bindingsFor = updated }
        )
    }
}

private fun describeElement(element: ControlElement): String = when (element.type) {
    "dpad" -> "D-Pad"
    "stick" -> "Стик"
    else -> "Кнопка «${element.label.ifEmpty { "?" }}»"
}

@Composable
private fun ElementProperties(
    element: ControlElement,
    onChange: (ControlElement) -> Unit,
    onEditBindings: () -> Unit,
    onDuplicate: () -> Unit,
    onDelete: () -> Unit
) {
    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (element.type == "button") {
                OutlinedTextField(
                    value = element.label,
                    onValueChange = { if (it.length <= 6) onChange(element.copy(label = it)) },
                    label = { Text("Надпись") },
                    singleLine = true,
                    modifier = Modifier.width(150.dp)
                )
                Spacer(Modifier.width(8.dp))
            }
            Column(Modifier.weight(1f)) {
                Text("Размер", color = Color(0xFFB0B0B8), fontSize = 11.sp)
                Slider(
                    value = element.size,
                    onValueChange = { onChange(element.copy(size = it)) },
                    valueRange = 0.06f..0.65f
                )
            }
            Column(Modifier.weight(1f)) {
                Text("Прозрачность", color = Color(0xFFB0B0B8), fontSize = 11.sp)
                Slider(
                    value = element.opacity,
                    onValueChange = { onChange(element.copy(opacity = it)) },
                    valueRange = 0.05f..1f
                )
            }
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(onClick = onEditBindings) { Text("Кнопки и клавиши") }
            Spacer(Modifier.width(8.dp))
            if (element.type != "button") {
                Text("Оси стика", color = Color(0xFFB0B0B8), fontSize = 12.sp)
                Spacer(Modifier.width(6.dp))
                Switch(checked = element.emitAxes, onCheckedChange = { onChange(element.copy(emitAxes = it)) })
                Spacer(Modifier.width(8.dp))
            }
            Row(Modifier.horizontalScroll(rememberScrollState())) {
                if (element.type == "button") {
                    for (style in listOf("circle" to "Круг", "rounded" to "Скругление", "square" to "Квадрат")) {
                        FilterChip(
                            selected = element.style == style.first,
                            onClick = { onChange(element.copy(style = style.first)) },
                            label = { Text(style.second) },
                            modifier = Modifier.padding(end = 6.dp)
                        )
                    }
                }
            }
            Spacer(Modifier.weight(1f))
            IconButton(onClick = onDuplicate) { Icon(Icons.Filled.ContentCopy, contentDescription = "Дублировать", tint = Color.White) }
            IconButton(onClick = onDelete) { Icon(Icons.Filled.Delete, contentDescription = "Удалить", tint = Color(0xFFEF9A9A)) }
        }
    }
}

private object NoopSink : ControlSink {
    override fun key(vk: Int, down: Boolean) = Unit
    override fun pad(button: Int, down: Boolean) = Unit
    override fun axis(axis: Int, value: Float) = Unit
}

private enum class BindingTarget(val label: String) {
    PRIMARY("Кнопка"),
    UP("Вверх"),
    DOWN("Вниз"),
    LEFT("Влево"),
    RIGHT("Вправо")
}

@Composable
private fun BindingDialog(
    element: ControlElement,
    onDismiss: () -> Unit,
    onChange: (ControlElement) -> Unit
) {
    var target by remember { mutableStateOf(if (element.type == "button") BindingTarget.PRIMARY else BindingTarget.UP) }
    var tab by remember { mutableStateOf(0) }

    val list: List<Binding> = when (target) {
        BindingTarget.PRIMARY -> element.bindings
        BindingTarget.UP -> element.up
        BindingTarget.DOWN -> element.down
        BindingTarget.LEFT -> element.left
        BindingTarget.RIGHT -> element.right
    }

    fun update(newList: List<Binding>) {
        val updated = when (target) {
            BindingTarget.PRIMARY -> element.copy(bindings = newList)
            BindingTarget.UP -> element.copy(up = newList)
            BindingTarget.DOWN -> element.copy(down = newList)
            BindingTarget.LEFT -> element.copy(left = newList)
            BindingTarget.RIGHT -> element.copy(right = newList)
        }
        onChange(updated)
    }

    fun toggle(binding: Binding) {
        val newList = if (list.contains(binding)) list - binding else list + binding
        update(newList)
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Что нажимает «${describeElement(element)}»") },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Готово") } },
        text = {
            Column {
                if (element.type != "button") {
                    Row(Modifier.horizontalScroll(rememberScrollState())) {
                        BindingTarget.entries.forEach { entry ->
                            FilterChip(
                                selected = target == entry,
                                onClick = { target = entry },
                                label = { Text(entry.label) },
                                modifier = Modifier.padding(end = 6.dp)
                            )
                        }
                    }
                    Spacer(Modifier.height(8.dp))
                }
                Row {
                    FilterChip(
                        selected = tab == 0,
                        onClick = { tab = 0 },
                        label = { Text("Геймпад") },
                        modifier = Modifier.padding(end = 6.dp)
                    )
                    FilterChip(selected = tab == 1, onClick = { tab = 1 }, label = { Text("Клавиатура") })
                }
                Spacer(Modifier.height(8.dp))
                Text(
                    "Можно отметить несколько — тогда кнопка отправит всё сразу.",
                    color = Color(0xFF9E9EA7),
                    fontSize = 11.sp
                )
                Spacer(Modifier.height(8.dp))
                if (tab == 0) {
                    LazyColumn(Modifier.height(280.dp)) {
                        items(PadButton.entries) { button ->
                            val binding = Binding.pad(button.index)
                            BindingRow(
                                label = button.label,
                                checked = list.contains(binding),
                                onClick = { toggle(binding) }
                            )
                        }
                        items(listOf(0, 1, 2, 3)) { axis ->
                            val negative = Binding.axis(axis, -1)
                            val positive = Binding.axis(axis, 1)
                            BindingRow(
                                label = "Ось $axis — в минус",
                                checked = list.contains(negative),
                                onClick = { toggle(negative) }
                            )
                            BindingRow(
                                label = "Ось $axis — в плюс",
                                checked = list.contains(positive),
                                onClick = { toggle(positive) }
                            )
                        }
                    }
                } else {
                    LazyColumn(Modifier.height(280.dp)) {
                        items(KeyOption.all) { key ->
                            val binding = Binding.key(key.code)
                            BindingRow(
                                label = "${key.label}  (${key.code})",
                                checked = list.contains(binding),
                                onClick = { toggle(binding) }
                            )
                        }
                    }
                }
            }
        }
    )
}

@Composable
private fun BindingRow(label: String, checked: Boolean, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 10.dp, horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(20.dp)
                .background(if (checked) MaterialTheme.colorScheme.primary else Color.Transparent, RoundedCornerShape(4.dp))
                .border(1.dp, Color(0xFF5A5A66), RoundedCornerShape(4.dp)),
            contentAlignment = Alignment.Center
        ) {
            if (checked) Icon(Icons.Filled.Check, contentDescription = null, tint = Color.White, modifier = Modifier.size(16.dp))
        }
        Spacer(Modifier.width(12.dp))
        Text(label, color = Color.White, fontSize = 14.sp)
    }
}
