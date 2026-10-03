package dev.hydrastone

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Who produced a [ChatMessage].
 */
enum class Role { USER, ASSISTANT }

/**
 * One chat message. [id] is stable for the life of the message: the chat list is
 * keyed on it, so a streaming assistant message is created once and then
 * appended to, never re-keyed on every token.
 */
data class ChatMessage(
    val id: String,
    val role: Role,
    val text: String,
    /** Raw token ids, when the engine reports them. Used by the debug row. */
    val tokenIds: List<Int> = emptyList()
)

/** Outcome of [EngineInterface.load]. */
data class EngineLoadResult(
    val ok: Boolean,
    /** User-facing reason. Empty when [ok]. Never an internal detail. */
    val error: String? = null,
    /** Model metadata for the Models tab. Empty until a model is loaded. */
    val detail: Map<String, String> = emptyMap()
)

/** Outcome of a finished [EngineInterface.generate] call. */
data class GenerationSummary(
    val generatedTokens: Int,
    val cancelled: Boolean,
    val error: String? = null,
    val elapsedMs: Long = 0
)

/**
 * Streaming callbacks for one generation.
 *
 * Every callback is invoked on the engine's own background thread, never on the
 * main thread. The chat state holder is responsible for hopping to a dispatcher;
 * an engine must not do UI-thread work.
 */
interface GenerationCallbacks {
    /** Once, before the first token. Drives the typing indicator. */
    fun onFirstToken() {}

    /** Per token, with the raw id. [text] may be empty for engines that do not detokenize. */
    fun onToken(tokenId: Int, text: String) {}

    /** Always exactly once, after the last [onToken]. */
    fun onComplete(summary: GenerationSummary) {}
}

/**
 * The one abstraction the chat UI is allowed to talk to.
 *
 * The UI never names a concrete engine. It asks for an [EngineInterface] and gets
 * either the llama.cpp engine or the Hydra v1 engine, which is what lets one chat
 * screen drive both without a single `if (engine is ...)` on the rendering path.
 *
 * Two members exist because the two engines genuinely differ, and hiding that
 * difference would be the dishonest option:
 *
 *  - [isExperimental] is true for the Hydra v1 engine. It is a ternary recurrence
 *    over a 2-bit-packed weight file with no tokenizer and no language model
 *    behind it, so it can only echo a token sequence. The UI must say so on
 *    screen rather than let a user mistake its output for a language model.
 *  - [capabilities] tells the UI which controls are meaningful at all, instead of
 *    the UI hard-coding knowledge of one engine.
 */
interface EngineInterface {

    /** Stable identifier, e.g. "llama" or "hydra". Used for the persisted selection. */
    val id: String

    /** Shown in the engine selector chip. */
    val displayName: String

    /** True when this engine must not be presented as a language model. */
    val isExperimental: Boolean

    /** What the current model actually supports, for enabling/disabling controls. */
    val capabilities: EngineCapabilities

    /**
     * Loads a model from an absolute path.
     *
     * Returns rather than throws: an unsupported architecture, a corrupt header
     * and a full disk are all ordinary outcomes with a user-facing message.
     */
    fun load(modelPath: String): EngineLoadResult

    /**
     * Generates from [prompt] for at most [maxTokens] tokens.
     *
     * Returns immediately; progress arrives through [callbacks] on a background
     * thread and [GenerationCallbacks.onComplete] always fires exactly once.
     * A call while another is running is a programming error and is rejected.
     */
    fun generate(prompt: String, maxTokens: Int, callbacks: GenerationCallbacks)

    /** Asks the running generation to stop. Safe to call when idle. */
    fun cancel()

    /** Releases the model. Safe to call when nothing is loaded. */
    fun unload()
}

/**
 * What a loaded model can actually do.
 *
 * [contextWindow] is the size the context was created with, not the model's
 * training length: the chat screen truncates against this number.
 */
data class EngineCapabilities(
    val contextWindow: Int = 0,
    val supportsStreamingText: Boolean = false,
    /** False for the Hydra engine, which has no tokenizer at all. */
    val understandsTextPrompt: Boolean = false
)

/**
 * The chat lifecycle, shared by every engine.
 *
 * Idle -> Generating -> Cancelling -> Idle is the whole state machine. Cancelling
 * is a distinct state rather than a boolean because "the user pressed stop" and
 * "the engine stopped" are genuinely different instants, and the input row has to
 * keep the Stop button visible in between instead of flickering back to Send.
 */
enum class ChatState { IDLE, GENERATING, CANCELLING }

/**
 * Everything the chat screen renders, as one observable object.
 *
 * Holding it in a [StateFlow] rather than in composable-local state is what makes
 * the screen survive rotation mid-generation: a token arriving while the activity
 * is being recreated updates this object, and the recreated screen reads the
 * accumulated text instead of starting blank.
 */
class ChatStateHolder {

    private val _state = MutableStateFlow(ChatState.IDLE)
    val state: StateFlow<ChatState> = _state.asStateFlow()

    private val _messages = MutableStateFlow<List<ChatMessage>>(emptyList())
    val messages: StateFlow<List<ChatMessage>> = _messages.asStateFlow()

    private val _liveTokenIds = MutableStateFlow<List<Int>>(emptyList())
    val liveTokenIds: StateFlow<List<Int>> = _liveTokenIds.asStateFlow()

    /** Set when a turn ended because the user pressed Stop. */
    private val _lastTurnCancelled = MutableStateFlow(false)
    val lastTurnCancelled: StateFlow<Boolean> = _lastTurnCancelled.asStateFlow()

    fun onUserMessage(text: String) {
        _messages.value = _messages.value + ChatMessage(nextId(), Role.USER, text)
        _lastTurnCancelled.value = false
    }

    fun onGenerationStarted() {
        _state.value = ChatState.GENERATING
        _liveTokenIds.value = emptyList()
        val id = nextId()
        _messages.value = _messages.value + ChatMessage(id, Role.ASSISTANT, "")
        _activeId = id
    }

    fun onToken(tokenId: Int, text: String) {
        val id = _activeId ?: return
        _liveTokenIds.value = _liveTokenIds.value + tokenId
        _messages.value = _messages.value.map {
            if (it.id == id) it.copy(text = it.text + text, tokenIds = _liveTokenIds.value) else it
        }
    }

    fun onFirstToken() {
        if (_state.value == ChatState.GENERATING) _state.value = ChatState.GENERATING
    }

    fun onCancelRequested() {
        if (_state.value == ChatState.GENERATING) _state.value = ChatState.CANCELLING
    }

    fun onComplete(cancelled: Boolean) {
        _state.value = ChatState.IDLE
        _lastTurnCancelled.value = cancelled
        _activeId = null
    }

    /** Drops the conversation. Backs the "New conversation" action. */
    fun clear() {
        _messages.value = emptyList()
        _liveTokenIds.value = emptyList()
        _lastTurnCancelled.value = false
        _activeId = null
    }

    private var _activeId: String? = null
    private var counter = 0L

    /**
     * Monotonic ids. A random id would make the LazyColumn keys change on every
     * process start, which turns every rotation into a full list rebuild.
     */
    private fun nextId(): String = "m${counter++}"
}
