package fixture.compose

/*
 * NEGATIVE CONTROL for tools/compose_scroll_guard.py (rules.md R25).
 *
 * Every scrollable pattern in this file is deliberately ILLEGAL: each one
 * produces the "Vertically/Horizontally scrollable component was measured
 * with an infinity maximum ... constraints" crash at measure time. The
 * guard must flag every single one of them; tools/compose_scroll_check.sh
 * asserts the finding count, so a guard that silently stops detecting
 * nested scrollables fails the gate here, not on a phone.
 *
 * This file is a fixture: it is never compiled into the app.
 */
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.horizontalScroll
import androidx.compose.runtime.Composable

/** The exact 2026-10-26 crash shape: scroll column inside scroll column. */
@Composable
fun BadNestedScrollColumns() {
    Column {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                // two levels of verticalScroll: the inner one gets an
                // infinite max height and throws.
            }
        }
    }
}

/** The classic case: a LazyColumn inside a vertically scrolling column. */
@Composable
fun BadLazyInsideScroll() {
    Column(Modifier.verticalScroll(rememberScrollState())) {
        LazyColumn {
            // the LazyColumn is measured with an infinite max height.
        }
    }
}

/** Horizontal axis: LazyRow inside a horizontalScroll row. */
@Composable
fun BadLazyRowInsideHScroll() {
    Row(Modifier.horizontalScroll(rememberScrollState())) {
        LazyRow {
            // same crash, on the horizontal axis.
        }
    }
}

/** The wrapper pattern: a scrollable lambda passed into a scrollable box. */
@Composable
fun BadWrapperScroll(header: @Composable () -> Unit) {
    Column(Modifier.verticalScroll(rememberScrollState())) {
        header()
        Box(Modifier.verticalScroll(rememberScrollState())) {
            // the box's scrollable sits inside the column's scrollable.
        }
    }
}
