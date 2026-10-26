package fixture.compose

/*
 * POSITIVE CONTROL for tools/compose_scroll_guard.py.
 *
 * Every pattern in this file is LEGAL Compose layout: the guard must flag
 * NONE of them. If it does, the guard is too broad and would reject
 * working layouts - tools/compose_scroll_check.sh asserts zero findings
 * here, so over-detection fails the gate, not just under-detection.
 *
 * This file is a fixture: it is never compiled into the app.
 */
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.weight
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.horizontalScroll
import androidx.compose.runtime.Composable

/** A single scrollable per axis, in plain sight: the app's real shape. */
@Composable
fun OkOneScrollPerAxis() {
    Column(Modifier.fillMaxSize()) {
        // the only vertical scroll in the hierarchy
        Column(Modifier.verticalScroll(rememberScrollState())) {
            // plain content, no scrollable of its own
        }
        // the lazy list is a SIBLING of the scroll column, not a child:
        // both are children of the plain column above, so neither measures
        // the other with infinite constraints.
        LazyColumn {
        }
    }
}

/** Siblings in branches: two scrollables, neither inside the other. */
@Composable
fun OkSiblingsInBranches(flag: Boolean) {
    if (flag) {
        Column(Modifier.verticalScroll(rememberScrollState())) {
        }
    } else {
        LazyColumn {
        }
    }
}

/**
 * Cross-axis nesting is legal: a vertical scroll may contain a HORIZONTAL
 * scrollable. (The reverse on the same axis is a crash - a horizontal
 * scroll measures its children with an infinite max WIDTH, so a LazyRow
 * inside it throws just like a LazyColumn inside a verticalScroll; see
 * scroll_nested_bad.kt.)
 */
@Composable
fun OkCrossAxis() {
    Column(Modifier.verticalScroll(rememberScrollState())) {
        Row(Modifier.horizontalScroll(rememberScrollState())) {
            // plain content: the column's infinite max height is consumed
            // by the column itself, the row's infinite max width by the
            // row itself - different axes never conflict.
            Box {
            }
        }
    }
}

/** A LazyColumn that is only weighted/bounded: the chat body's shape. */
@Composable
fun OkWeightedLazy() {
    Column(Modifier.fillMaxSize()) {
        Column(Modifier.weight(1f)) {
            LazyColumn {
            }
        }
    }
}

/** Prose that mentions the pattern must not trip the guard. */
@Composable
fun OkMentionsInCommentsAndStrings() {
    Column {
        // "Column(Modifier.verticalScroll)" and "LazyColumn(" live in this
        // comment and in the string below; both are masked, both legal.
    }
    val note = "Column(Modifier.verticalScroll) around a LazyColumn( here"
    Box(Modifier.verticalScroll(rememberScrollState())) {
        // the real, single, legal scrollable of this function
        val _ = note
    }
}
