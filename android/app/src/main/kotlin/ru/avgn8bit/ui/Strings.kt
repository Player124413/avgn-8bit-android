package ru.avgn8bit.ui

import androidx.annotation.StringRes
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource

/**
 * Shorthand for [stringResource].
 *
 * Every user-visible word in the launcher lives in `res/values/strings.xml` (English) and `res/values-ru/strings.xml`
 * (Russian), so the app speaks the language the device is set to instead of one hard-coded language.
 */
@Composable
fun str(@StringRes id: Int, vararg args: Any): String = stringResource(id, *args)
