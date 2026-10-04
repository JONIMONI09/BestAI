package dev.hydrastone.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

private val LightColors = lightColorScheme(
    primary = Color(0xFF2E6B4F),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFB4F1CE),
    onPrimaryContainer = Color(0xFF002115),
    secondary = Color(0xFF4E6355),
    tertiary = Color(0xFF3D6373),
    error = Color(0xFFBA1A1A)
)

private val DarkColors = darkColorScheme(
    primary = Color(0xFF99D5B3),
    onPrimary = Color(0xFF003825),
    primaryContainer = Color(0xFF135138),
    onPrimaryContainer = Color(0xFFB4F1CE),
    secondary = Color(0xFFB5CCBB),
    tertiary = Color(0xFFA3CDDF),
    error = Color(0xFFFFB4AB)
)

/**
 * The app theme.
 *
 * Dynamic colour is used where the platform provides it (Android 12+), because
 * a chat surface that fights the user's wallpaper is harder to read, not
 * easier. Below that the explicit schemes above apply.
 */
@Composable
fun HydraTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit
) {
    val context = LocalContext.current
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        darkTheme -> DarkColors
        else -> LightColors
    }

    MaterialTheme(
        colorScheme = colors,
        content = content
    )
}
