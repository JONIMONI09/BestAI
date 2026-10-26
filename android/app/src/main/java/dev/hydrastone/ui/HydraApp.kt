package dev.hydrastone.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
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
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import dev.hydrastone.ChatMessage
import dev.hydrastone.ChatState
import dev.hydrastone.GenerationSettings
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
     * "quant" -> "Q8_0", "weights" -> "1.2 MB", "kv" -> "3.4 MB" (the KV
     * cache the context allocates, not the serialized-state size).
     * The values are already formatted by the owner (MainActivity), which
     * is the only place that knows the raw numbers - the UI renders them,
     * it does not re-derive them.
     */
    val modelDetail: Map<String, String> = emptyMap(),
    /** The persisted generation settings the Settings tab edits. */
    val settings: GenerationSettings = GenerationSettings()
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
    val onEngineSelected: (String) -> Unit = {},
    val onSettingsChanged: (GenerationSettings) -> Unit = {},
    val onRemoveImportedModel: () -> Unit = {}
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
                Tab.MODELS -> ModelsTab(state, actions)
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
    // A4: the IME and navigation-bar insets belong to the WHOLE tab, not to
    // the input row. With .imePadding() on the row only, the keyboard inset
    // inflated just that one row, and on a short window (landscape, split
    // screen, API 30+ edge-to-edge) the TopAppBar plus system bars plus the
    // inflated row consumed the entire window - the weighted chat split
    // above the row collapsed to zero and the composer rendered directly
    // under the app bar with no conversation visible at all. Applied to the
    // root column, the inset shrinks the column instead: the split gives up
    // exactly the keyboard's height, the row keeps its natural size, and
    // every height the window actually has is laid out, not dropped.
    Column(
        modifier = Modifier
            .fillMaxSize()
            .imePadding()
            .navigationBarsPadding()
    ) {
        // The rows above the conversation share the space with it through
        // AdaptiveHeaderSplit: the header keeps its natural height while it
        // fits, and scrolls internally once it hits its share. That is what
        // keeps the composer visible when the keyboard shrinks the window -
        // the header gives up space first, the input row never moves off
        // screen.
        AdaptiveHeaderSplit(
            modifier = Modifier.weight(1f),
            header = {
                // No verticalScroll here: AdaptiveHeaderSplit already wraps
                // this content in ITS capping, scrollable column. A second
                // verticalScroll inside a verticalScroll is the crash, not
                // the fix - the outer scrollable measures its children with
                // an infinite max height, and Compose throws exactly on the
                // inner one ("Vertically scrollable component was measured
                // with an infinity maximum height constraints"), on every
                // device, the moment the chat tab is composed. One scroll
                // per axis: the split owns it.
                if (state.engineIsExperimental) {
                    ExperimentalBanner()
                }
                ModelRow(
                    modelLabel = state.modelLabel,
                    modelDetail = state.modelDetail,
                    onImportModel = actions.onImportModel
                )
                // The active engine is always named, even when there is
                // only one to choose: a chip that only appears when a
                // choice exists cannot tell the user which engine is
                // producing the text they are reading.
                EngineStatusRow(state)
                if (state.engines.size > 1) {
                    EngineSelector(state, actions)
                }
                HorizontalDivider()
                AutoRunRow(state.autoRun, actions.onAutoRunChanged)
                HorizontalDivider()
            },
            body = {
                if (state.chatMessages.isEmpty()) {
                    EmptyChat()
                } else {
                    MessageList(state.chatMessages, state.lastTurnCancelled)
                }
            }
        )

        // Live tok/s and the finished run's totals, pinned above the
        // composer so they survive a long conversation: the number the user
        // asked for ("how fast is this?") is not something to hunt for in
        // the log.
        RunTelemetryRow(state)
        InputRow(state, actions)
    }
}

/**
 * Splits the vertical space between a header block and its body.
 *
 * The header gets its natural height while it fits within
 * [headerFraction] of the space, and scrolls internally beyond that. The
 * body gets everything that is left (it is the only weighted child of the
 * inner column, so it takes 100% of what the header did not use - no gap
 * can form above the composer).
 *
 * The split is relative, not fixed dp, so it stays sane when the window
 * shrinks: with the keyboard open (the window resizes below API 30, and
 * the IME inset eats the bottom on 30+), the header gives up space first
 * and the input row never gets pushed off screen.
 *
 * BoxWithConstraints instead of a custom Layout: the cap is read from the
 * incoming constraints in the same measurement pass, so there is no
 * size-callback state and no collapsed first frame. BoxWithConstraints
 * sits next to Box in the same foundation.layout package, both already
 * used by this file.
 *
 * INVARIANT (regression: the 2026-10-26 "infinity maximum height" crash):
 * this column is the ONLY vertically scrollable node in the chat
 * hierarchy. Its children are measured with an infinite max height -
 * that is what verticalScroll does - and Compose throws the moment a
 * second verticalScroll, or a LazyColumn, sits inside it. [header] and
 * [body] must therefore contain no scrollable of their own; the body's
 * LazyColumn is one level deeper, inside the non-scrollable weighted
 * column, which is what keeps it legal. tools/compose_scroll_guard.py
 * fails the build if the invariant is broken again.
 */
@Composable
private fun AdaptiveHeaderSplit(
    modifier: Modifier = Modifier,
    headerFraction: Float = 0.45f,
    header: @Composable () -> Unit,
    body: @Composable () -> Unit
) {
    BoxWithConstraints(modifier = modifier) {
        // maxHeight (the scope property, not constraints.maxHeight!) is
        // the incoming maximum height already converted to Dp; the scope's
        // constraints are pixel-based (ui.unit.Constraints), so scaling
        // goes through the plain Float: value * fraction, then .dp.
        val bounded = constraints.hasBoundedHeight
        val cap: Dp = if (bounded) {
            (maxHeight.value * headerFraction).dp
        } else {
            Dp.Unspecified
        }
        // Remembered unconditionally: remember calls keep a stable order
        // whatever the modifier below turns out to be.
        val scrollState = rememberScrollState()
        Column(Modifier.fillMaxSize()) {
            // Natural height while it fits in the cap; scrolls internally
            // once it would take more of the space.
            //
            // verticalScroll is applied only on bounded height: Compose
            // throws when a vertically scrollable node is measured with an
            // infinite max height, so an unbounded caller (however it ever
            // gets composed) must receive a natural-height column instead
            // of an app-killing exception. This is the second layer of the
            // crash guard - the first is that this column is the ONLY
            // vertical scroll in the chat hierarchy (the header content
            // must never add its own; tools/compose_scroll_guard.py
            // checks that).
            Column(
                modifier = Modifier
                    .heightIn(max = cap)
                    .then(if (bounded) Modifier.verticalScroll(scrollState) else Modifier)
            ) {
                header()
            }
            // Everything the header did not take.
            Column(modifier = Modifier.weight(1f)) {
                body()
            }
        }
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
    val run = state.lastRun
    // A5: the row reserves its height even before the first run. The old
    // early return removed the row entirely until the first run finished,
    // so the composer jumped up by the row's height the moment telemetry
    // appeared - a layout shift on the most-watched row of the app.
    Box(modifier = Modifier.fillMaxWidth().height(28.dp)) {
        if (run == null) return@Box
    // getQuantityString, not stringResource: in this Compose version the
    // stringResource(id, vararg) overload wins over the plurals overload in
    // overload resolution, which lint then rejects as ResourceType
    // ("expected resource of type string"). Resources.getQuantityString
    // has a single unambiguous signature with the @PluralsRes parameter
    // (the same call style MainActivity uses for its plurals), and
    // LocalContext.current.resources is the canonical Compose way to
    // reach the app Resources (there is no LocalResources).
    val resources = LocalContext.current.resources
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
private fun MessageList(messages: List<ChatMessage>, lastTurnCancelled: Boolean) {
    // No weight and no minimum height: AdaptiveHeaderSplit measures this
    // list with exactly the height the screen has left, and a LazyColumn
    // fills a bounded constraint. A fixed minimum would overflow the
    // window exactly when the window is smallest - keyboard open on a
    // short screen - and push the composer off screen.
    //
    // A2: follow the generation only while the user is already at the
    // bottom. Without a scroll state the list stayed pinned at its initial
    // position, so a multi-hundred-token run rendered below the visible
    // area and the chat looked dead. The rule is the one chat screens use:
    // your own message always scrolls into view (you just sent it); the
    // engine's reply follows while you are at the bottom, and stops
    // following the moment you scroll up to read history.
    val listState = rememberLazyListState()
    val last = messages.lastOrNull()
    val lastIsUser = last?.role == Role.USER
    val atBottom = listState.layoutInfo.visibleItemsInfo.lastOrNull()?.let {
        it.index == listState.layoutInfo.totalItemsCount - 1
    } ?: true
    LaunchedEffect(last?.id, last?.text?.length, lastIsUser) {
        if (messages.isNotEmpty() && (lastIsUser || atBottom)) {
            listState.animateScrollToItem(messages.lastIndex)
        }
    }
    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxWidth(),
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
            // The IME inset lives on the ChatTab root column, not here: see
            // the A4 note there. On the row only, the keyboard collapsed the
            // conversation on short windows.
            .fillMaxWidth()
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
    val settings = state.settings
    val llamaActive = state.selectedEngineId == "llama"
    val hydraActive = state.selectedEngineId == "hydra"
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
            text = stringResource(R.string.settings_generation),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold
        )
        // Per-engine sampler inventory, stated instead of implied by
        // greying: the llama.cpp sampler chain takes topK/topP, the
        // Hydra C sampler (hydra_sample_token) takes temperature, minP
        // and repetitionPenalty. No slider is ever enabled for an engine
        // that would ignore it - that would be a control that changes
        // nothing, which this UI does not ship.
        Text(
            text = stringResource(R.string.settings_llama_only),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        // Thinking mode and the system prompt are prompt-level features:
        // they only take effect on engines that understand text, which is
        // exactly llama.cpp in this app - so they are shown with the same
        // gate as the sampling controls.
        if (llamaActive) {
            SwitchRow(
                label = stringResource(R.string.thinking_switch),
                summary = stringResource(R.string.thinking_summary),
                checked = settings.thinking,
                onCheckedChange = {
                    actions.onSettingsChanged(settings.copy(thinking = it))
                }
            )
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    text = stringResource(R.string.setting_system_prompt),
                    style = MaterialTheme.typography.labelMedium
                )
                OutlinedTextField(
                    value = settings.systemPrompt,
                    onValueChange = {
                        actions.onSettingsChanged(settings.copy(systemPrompt = it))
                    },
                    modifier = Modifier.fillMaxWidth(),
                    placeholder = { Text(stringResource(R.string.setting_system_prompt_hint)) },
                    maxLines = 3
                )
            }
        }

        SettingSlider(
            label = stringResource(R.string.setting_temperature),
            value = settings.temperature,
            start = 0f, end = 2f,
            step = 0.05f,
            format = { String.format(java.util.Locale.ROOT, "%.2f", it) },
            // Both engines: llama's sampler chain and the Hydra C sampler
            // (0 = the Hydra engine's deterministic baseline).
            enabled = llamaActive || hydraActive,
            onCommit = { actions.onSettingsChanged(settings.copy(temperature = it)) }
        )
        SettingSlider(
            label = stringResource(R.string.setting_top_k),
            value = settings.topK.toFloat(),
            start = 0f, end = 100f,
            step = 1f,
            format = { it.toInt().toString() },
            enabled = llamaActive,
            onCommit = { actions.onSettingsChanged(settings.copy(topK = it.toInt())) }
        )
        SettingSlider(
            label = stringResource(R.string.setting_top_p),
            value = settings.topP,
            start = 0.05f, end = 1f,
            step = 0.05f,
            format = { String.format(java.util.Locale.ROOT, "%.2f", it) },
            enabled = llamaActive,
            onCommit = { actions.onSettingsChanged(settings.copy(topP = it)) }
        )
        SettingSlider(
            label = stringResource(R.string.setting_min_p),
            value = settings.minP,
            start = 0f, end = 0.5f,
            step = 0.01f,
            format = { String.format(java.util.Locale.ROOT, "%.2f", it) },
            // Both engines: llama's min-p chain sampler and the Hydra
            // C sampler's min-p filter.
            enabled = llamaActive || hydraActive,
            onCommit = { actions.onSettingsChanged(settings.copy(minP = it)) }
        )
        SettingSlider(
            label = stringResource(R.string.setting_rep_penalty),
            value = settings.repetitionPenalty,
            start = 1f, end = 3f,
            step = 0.05f,
            format = { String.format(java.util.Locale.ROOT, "%.2f", it) },
            // Hydra only: the pinned llama.cpp (v0.5.0) has no
            // repetition-penalty chain sampler (see GenerationParams).
            enabled = hydraActive,
            onCommit = { actions.onSettingsChanged(settings.copy(repetitionPenalty = it)) }
        )
        SettingSlider(
            label = stringResource(R.string.setting_max_tokens),
            value = settings.maxTokens.toFloat(),
            start = 32f, end = 1024f,
            step = 32f,
            format = { it.toInt().toString() },
            enabled = true,
            onCommit = { actions.onSettingsChanged(settings.copy(maxTokens = it.toInt())) }
        )
        SettingSlider(
            label = stringResource(R.string.setting_context_size),
            value = settings.contextSize.toFloat(),
            start = 512f, end = 8192f,
            step = 256f,
            format = { it.toInt().toString() },
            enabled = llamaActive,
            onCommit = { actions.onSettingsChanged(settings.copy(contextSize = it.toInt())) }
        )
        TextButton(onClick = { actions.onSettingsChanged(GenerationSettings()) }) {
            Text(stringResource(R.string.settings_reset))
        }

        HorizontalDivider()
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

/**
 * One labelled control: title, optional summary line, trailing Switch.
 *
 * Used for the thinking-mode toggle; the switch commits immediately (a
 * toggle is a discrete decision, not a drag), so it needs no local state.
 */
@Composable
private fun SwitchRow(
    label: String,
    summary: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Text(text = label, style = MaterialTheme.typography.labelLarge)
            Text(
                text = summary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

/**
 * One labelled slider with the committed value shown to the left of the
 * control.
 *
 * The drag lives in LOCAL state and the commit goes out on release
 * (onValueChangeFinished): the Settings owner persists every change into
 * SharedPreferences, and persisting on every pixel of a drag would write
 * the file faster than the user's finger moves. The local state is
 * remembered keyed on the COMMITTED value, so an external change (the
 * reset button) snaps the thumb back, while a drag in flight is never
 * reset by its own commits.
 */
@Composable
private fun SettingSlider(
    label: String,
    value: Float,
    start: Float,
    end: Float,
    step: Float,
    format: (Float) -> String,
    enabled: Boolean,
    onCommit: (Float) -> Unit
) {
    // The endpoints come in as plain floats on purpose: Kotlin's
    // ClosedFloatingPointRange exposes no .first/.last accessors (those
    // belong to the integer ranges), so the step count is derived here
    // from the values the caller already has.
    var local by remember(value) { mutableFloatStateOf(value) }
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Text(text = label, style = MaterialTheme.typography.labelLarge)
            Text(
                text = format(local),
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Slider(
            value = local,
            onValueChange = { local = it },
            onValueChangeFinished = { onCommit(local) },
            valueRange = start..end,
            steps = (((end - start) / step).toInt() - 1).coerceAtLeast(0),
            enabled = enabled,
            modifier = Modifier.widthIn(min = 160.dp)
        )
    }
}

/**
 * The Models tab: the model that is actually on disk, its facts from the
 * engine's own load result, and the two model operations the app performs -
 * import a new one, remove the imported one.
 *
 * The facts come from [MainUiState.modelDetail], already formatted by
 * MainActivity; this tab renders them and derives nothing, so the numbers
 * here and in the chat header cannot disagree.
 */
@Composable
private fun ModelsTab(state: MainUiState, actions: MainActions) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            text = stringResource(R.string.model_current),
            style = MaterialTheme.typography.headlineSmall,
            fontWeight = FontWeight.SemiBold
        )
        if (state.hasModel) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier.padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    Text(
                        text = state.modelLabel,
                        style = MaterialTheme.typography.titleMedium
                    )
                    state.modelDetail.forEach { (k, v) ->
                        Row(modifier = Modifier.fillMaxWidth()) {
                            Text(
                                text = k,
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.widthIn(min = 72.dp)
                            )
                            Text(text = v, style = MaterialTheme.typography.labelMedium)
                        }
                    }
                }
            }
            FilledTonalButton(onClick = actions.onRemoveImportedModel) {
                Text(stringResource(R.string.model_remove_imported))
            }
        } else {
            // Empty state: nothing imported yet (e.g. before the starter
            // model has been pulled from the assets). A dedicated title +
            // body string (models_empty_*) so the copy has one home.
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text(
                    text = stringResource(R.string.models_empty_title),
                    style = MaterialTheme.typography.titleMedium
                )
                Text(
                    text = stringResource(R.string.models_empty_body),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        Button(onClick = actions.onImportModel) {
            Text(stringResource(R.string.action_import_model))
        }
        Text(
            text = stringResource(R.string.models_tab_body),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}
