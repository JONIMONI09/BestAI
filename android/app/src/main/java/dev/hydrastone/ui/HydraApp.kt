package dev.hydrastone.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import dev.hydrastone.ChatMessage
import dev.hydrastone.ChatState
import dev.hydrastone.Role
import dev.hydrastone.R

/**
 * The four top-level destinations.
 *
 * The icons come from `material-icons-core`, not `-extended`: the release build
 * runs with `isMinifyEnabled = false`, so every icon in `-extended` would be
 * packed into every APK. Core has no Chat/Folder glyph, so Send, Star, List and
 * Settings stand in. That is a deliberate size trade, not an oversight.
 */
enum class Tab(@StringRes val labelRes: Int, val icon: ImageVector) {
    CHAT(R.string.tab_chat, Icons.AutoMirrored.Filled.Send),
    MODELS(R.string.tab_models, Icons.Filled.Star),
    DATA(R.string.tab_data, Icons.AutoMirrored.Filled.List),
    SETTINGS(R.string.tab_settings, Icons.Filled.Settings)
}

/** One selectable engine, as the shell needs to see it. */
data class EngineInfo(
    val id: String,
    val displayName: String,
    val isExperimental: Boolean
)

/**
 * Telemetry of the most recent (or still running) generation.
 *
 * [live] is true while the run is in flight: the row then shows the rate so
 * far instead of a finished run's totals. [tokensPerSecond] is derived, not
 * stored: a rate that is stored separately from its inputs is a second thing
 * that can disagree with them.
 */
data class RunStats(
    val tokens: Int,
    val elapsedMs: Long,
    val cancelled: Boolean,
    val live: Boolean = false
) {
    val tokensPerSecond: Double
        get() = if (elapsedMs > 0) tokens * 1000.0 / elapsedMs else 0.0
}

/**
 * Everything the shell renders, as one value.
 *
 * A single immutable state object means the whole screen is a function of it, so
 * a rotation cannot lose half the state and the previews render the real thing.
 */
data class MainUiState(
    val selectedTab: Tab = Tab.CHAT,
    val modelLabel: String = "",
    val hasModel: Boolean = false,
    val engines: List<EngineInfo> = emptyList(),
    val selectedEngineId: String = "",
    val autoRun: Boolean = true,
    val chatMessages: List<ChatMessage> = emptyList(),
    val chatState: ChatState = ChatState.IDLE,
    val input: String = "",
    val lastTurnCancelled: Boolean = false,
    /** True while the benchmark thread is alive. Disables the benchmark button. */
    val isBenchmarking: Boolean = false,
    val log: String = "",
    /**
     * Telemetry of the current or most recent run. Null before the first
     * finished run (a live run starts it).
     */
    val lastRun: RunStats? = null,
    /**
     * Human-readable model facts from the engine's load result, e.g.
     * "quant" -> "Q8_0", "weights" -> "1.2 MB", "state" -> "3.4 MB".
     * The values are already formatted by the owner (MainActivity), which
     * is the only place that knows the raw numbers - the UI renders them,
     * it does not re-derive them.
     */
    val modelDetail: Map<String, String> = emptyMap()
) {
    /** Convenience for the banner and the model row. */
    val selectedEngine: EngineInfo?
        get() = engines.firstOrNull { it.id == selectedEngineId }

    val engineIsExperimental: Boolean
        get() = selectedEngine?.isExperimental == true

    /**
     * True while the engine is busy with anything.
     *
     * A benchmark and a generation run compete for the same native compute
     * threads, so neither may start while the other is in flight. Derived
     * rather than stored twice, because a second flag for "busy" is a second
     * thing that can disagree with the first.
     */
    val engineBusy: Boolean
        get() = isBenchmarking ||
            chatState == ChatState.GENERATING ||
            chatState == ChatState.CANCELLING
}

/** Callbacks the shell invokes. The shell owns no engine and no file system. */
data class MainActions(
    val onTabSelected: (Tab) -> Unit = {},
    val onInputChanged: (String) -> Unit = {},
    val onSend: () -> Unit = {},
    val onStop: () -> Unit = {},
    val onNewConversation: () -> Unit = {},
    val onImportModel: () -> Unit = {},
    val onAutoRunChanged: (Boolean) -> Unit = {},
    val onBenchmark: () -> Unit = {},
    val onEngineSelected: (String) -> Unit = {}
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HydraApp(state: MainUiState, actions: MainActions) {
    Scaffold(
        topBar = { TopAppBar(title = { Text(stringResource(R.string.app_title)) }) },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEach { tab ->
                    NavigationBarItem(
                        selected = state.selectedTab == tab,
                        onClick = { actions.onTabSelected(tab) },
                        icon = { Icon(tab.icon, contentDescription = null) },
                        // The label is not decoration: it is the accessible name
                        // for the icon, which has no content description.
                        label = { Text(stringResource(tab.labelRes)) }
                    )
                }
            }
        }
    ) { padding ->
        Box(modifier = Modifier.fillMaxSize().padding(padding)) {
            when (state.selectedTab) {
                Tab.CHAT -> ChatTab(state, actions)
                Tab.MODELS -> PlaceholderTab(
                    R.string.tab_models,
                    R.string.models_empty_title,
                    R.string.models_empty_body,
                    onImportModel = actions.onImportModel
                )
                Tab.DATA -> PlaceholderTab(
                    R.string.tab_data,
                    R.string.data_empty_title,
                    R.string.data_empty_body
                )
                Tab.SETTINGS -> SettingsTab(state, actions)
            }
        }
    }
}

/**
 * Empty state shared by the tabs that have no content in this phase.
 *
 * An empty tab that says nothing looks broken; one that says what will appear
 * and why it is empty does not.
 */
@Composable
private fun PlaceholderTab(
    @StringRes titleRes: Int,
    @StringRes emptyTitleRes: Int,
    @StringRes emptyBodyRes: Int,
    onImportModel: (() -> Unit)? = null
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            text = stringResource(titleRes),
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.SemiBold
        )
        Text(
            text = stringResource(emptyTitleRes),
            style = MaterialTheme.typography.titleMedium
        )
        Text(
            text = stringResource(emptyBodyRes),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = stringResource(R.string.coming_soon),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        if (onImportModel != null) {
            Button(onClick = onImportModel) {
                Text(stringResource(R.string.action_import_model))
            }
        }
    }
}

@Composable
private fun ChatTab(state: MainUiState, actions: MainActions) {
    Column(modifier = Modifier.fillMaxSize()) {
        if (state.engineIsExperimental) {
            ExperimentalBanner()
        }
        ModelRow(
            modelLabel = state.modelLabel,
            modelDetail = state.modelDetail,
            onImportModel = actions.onImportModel
        )
        // The active engine is always named, even when there is only one to
        // choose: a chip that only appears when a choice exists cannot tell
        // the user which engine is producing the text they are reading.
        EngineStatusRow(state)
        if (state.engines.size > 1) {
            EngineSelector(state, actions)
        }
        HorizontalDivider()
        AutoRunRow(state.autoRun, actions.onAutoRunChanged)
        HorizontalDivider()

        if (state.chatMessages.isEmpty()) {
            EmptyChat()
        } else {
            MessageList(state.chatMessages, state.lastTurnCancelled)
        }

        // Live tok/s and the finished run's totals, pinned above the
        // composer so they survive a long conversation: the number the user
        // asked for ("how fast is this?") is not something to hunt for in
        // the log.
        RunTelemetryRow(state)
        InputRow(state, actions)
    }
}

/**
 * The engine that is actually loaded, always visible.
 *
 * The experimental qualifier is part of [EngineInfo.displayName]
 * ("Hydra v1 (experimental)"), so this row and the banner never drift apart:
 * there is one place that names the engine, not two.
 */
@Composable
private fun EngineStatusRow(state: MainUiState) {
    val engine = state.selectedEngine ?: return
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = stringResource(R.string.label_engine),
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(end = 8.dp),
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        AssistChip(
            onClick = {},
            label = { Text(engine.displayName) }
        )
    }
}

/**
 * Telemetry row: while a run is live it shows the tokens-so-far and the
 * running rate; when the run finishes it shows the totals and, for a
 * cancelled run, says it was stopped by the user (cancelled is not an
 * error, and the string says so instead of letting a red error do it).
 */
@Composable
private fun RunTelemetryRow(state: MainUiState) {
    val run = state.lastRun ?: return
    // getQuantityString, not stringResource: in this Compose version the
    // stringResource(id, vararg) overload wins over the plurals overload in
    // overload resolution, which lint then rejects as ResourceType
    // ("expected resource of type string"). Resources.getQuantityString has
    // a single unambiguous signature and the @PluralsRes parameter, and it
    // is the same pattern MainActivity already uses for its plurals.
    val resources = LocalResources.current
    val text = if (run.live) {
        resources.getQuantityString(R.plurals.run_live, run.tokens, run.tokens, formatRate(run.tokensPerSecond))
    } else if (run.cancelled) {
        resources.getQuantityString(R.plurals.run_done_cancelled, run.tokens, run.tokens, formatRate(run.tokensPerSecond))
    } else {
        resources.getQuantityString(R.plurals.run_done, run.tokens, run.tokens, run.elapsedMs, formatRate(run.tokensPerSecond))
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        if (run.live) {
            CircularProgressIndicator(
                strokeWidth = 2.dp,
                modifier = Modifier
                    .padding(end = 8.dp)
                    .heightIn(min = 14.dp)
            )
        }
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/** One decimal place is the resolution a rate needs; more implies a
 * precision the measurement does not have. */
private fun formatRate(tokensPerSecond: Double): String =
    String.format(java.util.Locale.ROOT, "%.1f", tokensPerSecond)

/**
 * Stated permanently, not once. A user who switches to the experimental engine
 * has to be able to see, at the moment they read the output, that this is not a
 * language model producing it.
 */
@Composable
private fun ExperimentalBanner() {
    SurfaceTintedCard(container = MaterialTheme.colorScheme.tertiaryContainer) {
        Text(
            text = stringResource(R.string.banner_experimental_engine),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onTertiaryContainer
        )
    }
}

@Composable
private fun SurfaceTintedCard(
    container: androidx.compose.ui.graphics.Color,
    content: @Composable () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        colors = CardDefaults.cardColors(containerColor = container)
    ) {
        Box(modifier = Modifier.padding(12.dp)) { content() }
    }
}

@Composable
private fun ModelRow(
    modelLabel: String,
    modelDetail: Map<String, String>,
    onImportModel: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = "${stringResource(R.string.label_model)}: $modelLabel",
                style = MaterialTheme.typography.bodySmall
            )
            // The RAM footprint the user asked for: the engine's own
            // load-time numbers (llama.cpp reports its own state size),
            // formatted by the owner, rendered here as plain text.
            if (modelDetail.isNotEmpty()) {
                Text(
                    text = modelDetail.entries.joinToString(" · ") { (k, v) -> "$k $v" },
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        FilledTonalButton(onClick = onImportModel) {
            Text(stringResource(R.string.action_import_model))
        }
    }
}

/**
 * Engine selector.
 *
 * Only rendered when more than one engine is available. On an ABI without
 * llama.cpp packaged there is nothing to choose between, and showing a single
 * chip would imply a choice that does not exist.
 */
@Composable
private fun EngineSelector(state: MainUiState, actions: MainActions) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = stringResource(R.string.engine_selector),
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(end = 8.dp)
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            state.engines.forEach { engine ->
                FilterChip(
                    selected = engine.id == state.selectedEngineId,
                    onClick = { actions.onEngineSelected(engine.id) },
                    label = { Text(engine.displayName) }
                )
            }
        }
    }
}

@Composable
private fun AutoRunRow(checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = stringResource(R.string.action_auto_run),
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f)
        )
        Switch(checked = checked, onCheckedChange = { checked -> onCheckedChange(checked) })
    }
}

@Composable
private fun EmptyChat() {
    Column(
        modifier = Modifier.fillMaxWidth().padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = stringResource(R.string.chat_empty_title),
            style = MaterialTheme.typography.titleMedium
        )
        Text(
            text = stringResource(R.string.chat_empty_body),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

/**
 * The conversation.
 *
 * `items(messages, key = { it.id })` is what keeps a streaming message from
 * being re-created on every token: without a stable key the row loses its
 * identity and the list rebuilds instead of updating one item.
 */
@Composable
private fun ColumnScope.MessageList(messages: List<ChatMessage>, lastTurnCancelled: Boolean) {
    LazyColumn(
        modifier = Modifier
            .fillMaxWidth()
            .weight(1f)
            // A guaranteed minimum height keeps the conversation readable on a
            // short screen instead of collapsing to nothing when it is empty.
            .heightIn(min = 120.dp),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        items(items = messages, key = { it.id }) { message ->
            MessageRow(message)
        }
        if (lastTurnCancelled) {
            item {
                Text(
                    text = stringResource(R.string.chat_cancelled),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error
                )
            }
        }
    }
}

@Composable
private fun MessageRow(message: ChatMessage) {
    val isUser = message.role == Role.USER
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = if (isUser) Arrangement.End else Arrangement.Start
    ) {
        Card(
            modifier = Modifier.widthIn(max = 320.dp),
            colors = CardDefaults.cardColors(
                containerColor = if (isUser) {
                    MaterialTheme.colorScheme.primaryContainer
                } else {
                    MaterialTheme.colorScheme.surfaceVariant
                }
            )
        ) {
            Column(modifier = Modifier.padding(12.dp)) {
                if (message.text.isEmpty()) {
                    CircularProgressIndicator(strokeWidth = 2.dp, modifier = Modifier.heightIn(min = 16.dp))
                } else {
                    Text(text = message.text, style = MaterialTheme.typography.bodyMedium)
                }
                // The experimental engine emits ids, not words, so they are
                // shown rather than hidden behind a decoder that does not exist.
                if (message.tokenIds.isNotEmpty() && message.text.isEmpty()) {
                    Text(
                        text = message.tokenIds.joinToString(" "),
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

/**
 * The composer.
 *
 * The button is Send while idle and Stop while running: one control that means
 * "do" and then "stop", rather than two buttons that are always on screen and
 * are therefore both sometimes wrong.
 */
@Composable
private fun InputRow(state: MainUiState, actions: MainActions) {
    val generating = state.chatState == ChatState.GENERATING ||
        state.chatState == ChatState.CANCELLING

    Row(
        modifier = Modifier
            .fillMaxWidth()
            // imePadding lifts the row above the soft keyboard; without it the
            // keyboard covers the send button on a short screen.
            .imePadding()
            .navigationBarsPadding()
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.Bottom
    ) {
        OutlinedTextField(
            value = state.input,
            onValueChange = actions.onInputChanged,
            modifier = Modifier.weight(1f),
            placeholder = { Text(stringResource(R.string.chat_input_hint)) },
            // Capped so the composer can never grow tall enough to push the
            // conversation off the screen.
            maxLines = 4,
            enabled = !generating
        )
        Spacer(modifier = Modifier.padding(horizontal = 4.dp))
        if (generating) {
            FilledTonalButton(onClick = actions.onStop) {
                Text(stringResource(R.string.action_stop))
            }
        } else {
            Button(
                onClick = actions.onSend,
                // A send button that does nothing is worse than no send button.
                enabled = state.hasModel && state.input.isNotBlank()
            ) {
                Text(stringResource(R.string.action_send))
            }
        }
    }
}

@Composable
private fun SettingsTab(state: MainUiState, actions: MainActions) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            text = stringResource(R.string.section_diagnostics),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold
        )
        // Disabled while the engine is busy, and the label says WHY it is
        // disabled. A greyed-out button with the same text as the enabled one
        // is a control the user has to guess at.
        FilledTonalButton(
            onClick = actions.onBenchmark,
            enabled = state.hasModel && !state.engineBusy
        ) {
            Text(
                stringResource(
                    if (state.isBenchmarking) R.string.action_benchmark_busy
                    else R.string.action_benchmark
                )
            )
        }
        Text(
            text = stringResource(R.string.section_engine_info),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold
        )
        AssistChip(
            onClick = {},
            label = {
                Text(
                    state.selectedEngine?.displayName ?: stringResource(R.string.coming_soon)
                )
            }
        )
        HorizontalDivider()
        Text(
            text = stringResource(R.string.coming_soon),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        TextButton(onClick = actions.onNewConversation) {
            Text(stringResource(R.string.chat_new_conversation))
        }
        if (state.log.isNotEmpty()) {
            Text(
                text = state.log,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace
            )
        }
    }
}
