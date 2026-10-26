package fixture.compose

/*
 * POSITIVE CONTROL for detector 2 of tools/compose_scroll_guard.py.
 *
 * The FIXED 2026-10-26 shape: the same split composable, but the header
 * lambda carries PLAIN content (no scrollable of its own), so invoking
 * it inside the split's scrollable column is exactly what the app now
 * does. The guard must flag NOTHING here - if it does, it rejects the
 * fixed layout and every future "header = { plain content }" caller.
 *
 * This file is a fixture: it is never compiled into the app.
 */
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.weight
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

@Composable
fun Split(
    modifier: Modifier = Modifier,
    header: @Composable () -> Unit,
    body: @Composable () -> Unit
) {
    Column(modifier.fillMaxSize()) {
        Column(Modifier.weight(1f)) {
            Column(
                Modifier
                    .heightIn(max = 100.dp)
                    .verticalScroll(rememberScrollState())
            ) {
                header()
            }
            Column(Modifier.weight(1f)) {
                body()
            }
        }
    }
}

@Composable
fun Screen() {
    Split(
        // plain content - the invariant the fixed app holds
        header = {
            Row {
                Text("model row")
            }
        },
        // the lazy list stays in the non-scrollable body column
        body = {
            LazyColumn {
            }
        }
    )
}
