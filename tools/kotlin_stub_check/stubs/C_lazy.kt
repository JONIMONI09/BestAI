package androidx.compose.foundation.lazy
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier

class LazyListItemInfo {
    val index: Int get() = 0
    val key: Any? get() = null
}
class LazyListLayoutInfo {
    val visibleItemsInfo: List<LazyListItemInfo> get() = emptyList()
    val totalItemsCount: Int get() = 0
    val firstVisibleItemIndex: Int get() = 0
}
class LazyListState(initialFirstVisibleItemIndex: Int = 0, initialFirstVisibleItemScrollOffset: Int = 0) {
    val layoutInfo: LazyListLayoutInfo get() = LazyListLayoutInfo()
    suspend fun animateScrollToItem(index: Int, scrollOffset: Int = 0) {}
    suspend fun scrollToItem(index: Int, scrollOffset: Int = 0) {}
}
@Composable
fun rememberLazyListState(initialFirstVisibleItemIndex: Int = 0): LazyListState = LazyListState()

interface LazyItemScope
class LazyListScope {
    fun item(content: @Composable () -> Unit) {}
    fun item(key: Any?, content: @Composable () -> Unit) {}
}
fun <T> LazyListScope.items(
    items: List<T>,
    key: ((item: T) -> Any)? = null,
    contentType: ((item: T) -> Any?)? = null,
    content: @Composable LazyItemScope.(item: T) -> Unit
) {}
@Composable
fun LazyColumn(
    state: LazyListState = rememberLazyListState(),
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(),
    reverseLayout: Boolean = false,
    verticalArrangement: Arrangement = Arrangement.Start,
    horizontalAlignment: androidx.compose.ui.Alignment = androidx.compose.ui.Alignment.CenterHorizontally,
    content: LazyListScope.() -> Unit
) {}
