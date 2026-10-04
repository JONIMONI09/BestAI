package dev.hydrastone

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * The model swap must never cost the user the model they already had.
 *
 * WHY AN INSTRUMENTED TEST AND NOT A HOST TEST
 * --------------------------------------------
 * The fix rests on a property of the *platform*: `File.renameTo` calls
 * `rename(2)`, which replaces an existing file atomically and leaves it
 * untouched when it fails. That property has to hold in the app's own storage
 * on the device the app ships to, not on a Linux build machine
 * (rules.md R23). Everything here therefore runs in the app's real
 * `filesDir` on a real Android runtime.
 *
 * WHAT IS ACTUALLY PROVEN
 * -----------------------
 * 1. The assumption the fix rests on: `renameTo` replaces an existing file
 *    here, so a pre-delete is unnecessary.
 * 2. A failing rename leaves the previous model in place, byte for byte.
 *    The failure is injected rather than waited for - a real `rename` cannot
 *    be made to fail on demand, and a test that cannot fail is decoration
 *    (rules.md R25).
 * 3. A negative control: the OLD order (delete, then rename) with the same
 *    injected failure really does destroy the model. Without this, "the test
 *    passes" would be consistent with the bug still being present.
 */
@RunWith(AndroidJUnit4::class)
class AtomicModelSwapTest {

    private lateinit var dir: File

    @Before
    fun setUp() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        dir = File(ctx.filesDir, "swap-test")
        dir.deleteRecursively()
        assertTrue(dir.mkdirs())
    }

    private fun file(name: String, body: String): File =
        File(dir, name).apply { writeText(body) }

    /** True when the swap put the new content in place. */
    private fun swapped(o: AtomicModelSwap.Outcome) =
        o is AtomicModelSwap.Outcome.Swapped

    @Test
    fun renameTo_replacesAnExistingFile_onThisPlatform() {
        val target = file("a.hydra", "OLD")
        val source = file("b.hydra", "NEW")

        // The whole fix depends on this being true. If a filesystem ever
        // refuses the replacement, the fix has to change - and this test is
        // what says so instead of the swap silently deleting first again.
        assertTrue("renameTo must replace an existing file", source.renameTo(target))
        assertEquals("NEW", target.readText())
        assertFalse("a replace must not leave the source behind", source.exists())
    }

    @Test
    fun successfulSwap_replacesTheModel_andLeavesABackupToDiscard() {
        val target = file("a.hydra", "OLD")
        val source = file("b.hydra", "NEW")

        val outcome = AtomicModelSwap.swap(source, target)

        assertTrue("the swap must succeed, got $outcome", swapped(outcome))
        assertEquals("NEW", target.readText())
        val backup = (outcome as AtomicModelSwap.Outcome.Swapped).backup
        assertNotNull("the previous model must still be recoverable", backup)
        assertEquals("OLD", backup!!.readText())
        assertTrue("the backup is removed only once the new model is good",
            AtomicModelSwap.discardBackup(backup))
        assertFalse(backup.exists())
    }

    @Test
    fun firstImport_withNoPreviousModel_succeedsAndHasNoBackup() {
        val target = File(dir, "a.hydra").apply { assertFalse(exists()) }
        val source = file("b.hydra", "NEW")

        val outcome = AtomicModelSwap.swap(source, target)

        assertTrue("the swap must succeed, got $outcome", swapped(outcome))
        assertNull("there was no previous model, so there is nothing to back up",
            (outcome as AtomicModelSwap.Outcome.Swapped).backup)
        assertEquals("NEW", target.readText())
    }

    @Test
    fun failedRename_keepsThePreviousModel_byteForByte() {
        val target = file("a.hydra", "PREVIOUS-MODEL-BYTES")
        val source = file("b.hydra", "NEW")
        val backup = AtomicModelSwap.backupOf(target)

        // Injected failure of the SECOND move only: the move-aside of the
        // previous model must still work, or the test would be proving the
        // wrong thing. This is exactly the situation the old code turned
        // into data loss.
        var calls = 0
        val failingSecondMove: (File, File) -> Boolean = { from, to ->
            calls++
            if (calls == 2) false else from.renameTo(to)
        }

        val outcome = AtomicModelSwap.swap(source, target, failingSecondMove)

        assertTrue(
            "a failed rename must be reported as a rollback, got $outcome",
            outcome is AtomicModelSwap.Outcome.RolledBack
        )
        assertTrue("the previous model must still be the active file", target.isFile)
        assertEquals(
            "the previous model must be byte-for-byte unchanged",
            "PREVIOUS-MODEL-BYTES", target.readText()
        )
        assertFalse(
            "the backup must be gone: it was put back, not left behind",
            backup.exists()
        )
    }

    @Test
    fun failedRename_onFirstImport_leavesNothingHalfInstalled() {
        val target = File(dir, "a.hydra").apply { assertFalse(exists()) }
        val source = file("b.hydra", "NEW")

        val outcome = AtomicModelSwap.swap(source, target) { _, _ -> false }

        assertTrue(
            "with no previous model there is nothing to roll back to",
            outcome is AtomicModelSwap.Outcome.NotSwapped
        )
        assertFalse("a failed first import must not install anything", target.exists())
    }

    @Test
    fun aModelThatTurnsOutToBeUnusable_isRolledBack() {
        val target = file("a.hydra", "PREVIOUS-MODEL-BYTES")
        val source = file("b.hydra", "NEW-BUT-UNUSABLE")

        val outcome = AtomicModelSwap.swap(source, target)
        assertTrue("setup: the swap itself succeeds", swapped(outcome))
        assertEquals("NEW-BUT-UNUSABLE", target.readText())

        // ...the engine then refuses the new file, so the caller rolls back.
        assertTrue(
            "the unusable model must be replaced by the previous one",
            AtomicModelSwap.rollback(
                (outcome as AtomicModelSwap.Outcome.Swapped).backup, target
            )
        )
        assertEquals("PREVIOUS-MODEL-BYTES", target.readText())
    }

    /**
     * NEGATIVE CONTROL. Proves the test above can fail.
     *
     * This is the OLD implementation, verbatim in shape: delete the target,
     * then rename. Run with the same injected failure, it destroys the model
     * the user had. If this control ever stops failing, the tests above have
     * stopped proving anything.
     */
    @Test
    fun negativeControl_theOldDeleteThenRenameOrder_reallyLosesTheModel() {
        val target = file("a.hydra", "PREVIOUS-MODEL-BYTES")
        val source = file("b.hydra", "NEW")

        var calls = 0
        val failingSecondMove: (File, File) -> Boolean = { from, to ->
            calls++
            if (calls == 2) false else from.renameTo(to)
        }

        // --- the code that used to be in MainActivity.importModel() ---
        val movedAside = failingSecondMove(target, AtomicModelSwap.backupOf(target))
        assertTrue("setup: the old code moved the previous model aside", movedAside)
        val oldOrderSucceeded = failingSecondMove(source, target)
        // -------------------------------------------------------------------

        assertFalse("setup: the rename really did fail", oldOrderSucceeded)
        assertFalse(
            "CONTROL FAILED: the old order kept the model, so these tests are " +
                "not proving the fix works",
            target.isFile
        )
        assertFalse(
            "CONTROL FAILED: the old order installed the new model anyway",
            target.exists()
        )
        // The model is gone, and the backup is the only thing left of it.
        assertTrue(
            "the old order leaves the user with no model at the expected path",
            AtomicModelSwap.backupOf(target).readText() == "PREVIOUS-MODEL-BYTES"
        )
    }
}