package fixture.compose

/*
 * NEGATIVE CONTROL for detector 2 of tools/compose_scroll_guard.py
 * (rules.md R25), standing in for the git-history proof when the merge
 * commit is not in the clone (shallow checkouts).
 *
 * This is the 2026-10-26 Hydra-Stone crash shape, recreated standalone:
 * a composable lambda parameter invoked inside a scrollable container
 * whose definition carries scrollable content. The dangerous pair
 * crosses a composable boundary, so detector 1 (lexical nesting) cannot
 * see it - only detector 2 can. The guard MUST flag the header lambda
 * here; the body lambda (lazy list, invoked in a NON-scrollable column)
 * must NOT be flagged.
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
            // the capping, scrollable header column - the ONLY scroll
            // this hierarchy is allowed to have
            Column(
                Modifier
                    .heightIn(max = 100.dp)
                    .verticalScroll(rememberScrollState())
            ) {
                header()
            }
            // the body column is NOT scrollable: a LazyColumn inside it
            // is perfectly legal
            Column(Modifier.weight(1f)) {
                body()
            }
        }
    }
}

@Composable
fun Screen() {
    Split(
        // CRASH: a scrollable inside a scrollable, across the lambda
        // boundary - exactly what d0c7679's ChatTab did to the chat
        // header, which killed the app on every launch.
        header = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text("header content")
            }
        },
        // LEGAL: the lazy list is invoked in the non-scrollable body
        // column and must not be reported.
        body = {
            LazyColumn {
            }
        }
    )
}
