package androidx.compose.material3
import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.Dp

annotation class ExperimentalMaterial3Api

class ColorScheme(
    val primary: Color = Color.Black,
    val onPrimary: Color = Color.Black,
    val primaryContainer: Color = Color.Black,
    val onPrimaryContainer: Color = Color.Black,
    val secondary: Color = Color.Black,
    val onSecondary: Color = Color.Black,
    val tertiary: Color = Color.Black,
    val onTertiary: Color = Color.Black,
    val tertiaryContainer: Color = Color.Black,
    val onTertiaryContainer: Color = Color.Black,
    val error: Color = Color.Black,
    val onError: Color = Color.Black,
    val surface: Color = Color.Black,
    val onSurface: Color = Color.Black,
    val surfaceVariant: Color = Color.Black,
    val onSurfaceVariant: Color = Color.Black,
    val background: Color = Color.Black,
    val onBackground: Color = Color.Black
)

class TextTheme(
    val headlineSmall: TextStyle = TextStyle(),
    val titleMedium: TextStyle = TextStyle(),
    val bodyMedium: TextStyle = TextStyle(),
    val bodySmall: TextStyle = TextStyle(),
    val labelLarge: TextStyle = TextStyle(),
    val labelMedium: TextStyle = TextStyle(),
    val labelSmall: TextStyle = TextStyle()
)

object MaterialTheme {
    val colorScheme: ColorScheme get() = ColorScheme()
    val typography: TextTheme get() = TextTheme()
}
@Composable
fun MaterialTheme(colorScheme: ColorScheme = MaterialTheme.colorScheme, content: @Composable () -> Unit) {}

@Composable fun lightColorScheme(
    primary: Color = Color.Black, onPrimary: Color = Color.Black,
    primaryContainer: Color = Color.Black, onPrimaryContainer: Color = Color.Black,
    secondary: Color = Color.Black, onSecondary: Color = Color.Black,
    tertiary: Color = Color.Black, onTertiary: Color = Color.Black,
    error: Color = Color.Black, onError: Color = Color.Black,
    surface: Color = Color.Black, onSurface: Color = Color.Black
): ColorScheme = ColorScheme()
@Composable fun darkColorScheme(
    primary: Color = Color.Black, onPrimary: Color = Color.Black,
    primaryContainer: Color = Color.Black, onPrimaryContainer: Color = Color.Black,
    secondary: Color = Color.Black, onSecondary: Color = Color.Black,
    tertiary: Color = Color.Black, onTertiary: Color = Color.Black,
    error: Color = Color.Black, onError: Color = Color.Black,
    surface: Color = Color.Black, onSurface: Color = Color.Black
): ColorScheme = ColorScheme()
@Composable fun dynamicLightColorScheme(context: Context): ColorScheme = ColorScheme()
@Composable fun dynamicDarkColorScheme(context: Context): ColorScheme = ColorScheme()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TopAppBar(title: @Composable () -> Unit, modifier: Modifier = Modifier) {}

@Composable
fun Scaffold(
    modifier: Modifier = Modifier,
    topBar: @Composable () -> Unit = {},
    bottomBar: @Composable () -> Unit = {},
    content: @Composable (androidx.compose.foundation.layout.PaddingValues) -> Unit
) {}

class NavigationBarScope
@Composable
fun NavigationBar(content: @Composable NavigationBarScope.() -> Unit) {}
@Composable
fun NavigationBarItem(
    selected: Boolean,
    onClick: () -> Unit,
    icon: @Composable () -> Unit,
    label: @Composable () -> Unit = {},
    modifier: Modifier = Modifier
) {}

@Composable fun Text(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = Color.Black,
    fontSize: androidx.compose.ui.unit.TextUnit = androidx.compose.ui.unit.TextUnit.Unspecified,
    fontWeight: androidx.compose.ui.text.font.FontWeight? = null,
    fontFamily: androidx.compose.ui.text.font.FontFamily? = null,
    fontStyle: androidx.compose.ui.text.font.FontStyle? = null,
    style: TextStyle = TextStyle(),
    maxLines: Int = Int.MAX_VALUE
) {}

@Composable fun Button(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable () -> Unit
) {}
@Composable fun FilledTonalButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable () -> Unit
) {}
@Composable fun TextButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    content: @Composable () -> Unit
) {}

class CardColors
object CardDefaults {
    fun cardColors(containerColor: Color = Color.Black, contentColor: Color = Color.Black): CardColors = CardColors()
}
@Composable fun Card(
    modifier: Modifier = Modifier,
    colors: CardColors = CardDefaults.cardColors(),
    content: @Composable () -> Unit
) {}

@Composable fun AssistChip(
    onClick: () -> Unit,
    label: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {}
@Composable fun FilterChip(
    selected: Boolean,
    onClick: () -> Unit,
    label: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {}

@Composable fun HorizontalDivider(modifier: Modifier = Modifier, thickness: Dp = Dp(1f), color: Color = Color.Black) {}
@Composable fun Icon(
    imageVector: ImageVector,
    contentDescription: String?,
    modifier: Modifier = Modifier,
    tint: Color = Color.Black
) {}
@Composable fun CircularProgressIndicator(
    modifier: Modifier = Modifier,
    strokeWidth: Dp = Dp(4f),
    color: Color = Color.Black
) {}

@Composable
fun Switch(
    checked: Boolean,
    onCheckedChange: ((Boolean) -> Unit)?,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {}

@Composable
fun Slider(
    value: Float,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    valueRange: ClosedFloatingPointRange<Float> = 0f..1f,
    steps: Int = 0,
    onValueChangeFinished: (() -> Unit)? = null,
    label: @Composable (() -> Unit)? = null
) {}

@Composable
fun OutlinedTextField(
    value: String,
    onValueChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    placeholder: @Composable (() -> Unit)? = null,
    maxLines: Int = 1
) {}
