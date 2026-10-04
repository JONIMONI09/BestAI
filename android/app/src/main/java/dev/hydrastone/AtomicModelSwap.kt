package dev.hydrastone

import java.io.File

/**
 * Replacing one file with another without ever leaving the user with nothing.
 *
 * ## The bug this exists to prevent
 *
 * The import path used to do this:
 *
 * ```
 * if (target.exists() && !target.delete()) { abort }
 * if (!tmp.renameTo(target))            { abort }   // <- old model already gone
 * ```
 *
 * If the rename failed, the previous model had already been deleted and the
 * user was left with **no model at all**, from a failure that had nothing to do
 * with the model they had just successfully validated. The comment above it
 * claimed the operation was "atomic within the same directory", which is true
 * of `renameTo` and false of delete-then-rename.
 *
 * `rename(2)` — which is what `File.renameTo` calls — is atomic on a POSIX
 * filesystem: on success the target is replaced, on failure **nothing changed**.
 * Both files live in the same directory, so the same filesystem is guaranteed
 * and `rename` cannot fail with `EXDEV`. So the pre-delete was never necessary.
 *
 * ## What it does instead
 *
 * 1. move the previous file aside to `<name>.bak` (nothing is destroyed yet),
 * 2. move the new file into place,
 * 3. on success, leave the `.bak` for the caller to discard,
 * 4. on failure, move the `.bak` back — so a failed swap is a **no-op**, not a
 *    data-loss event.
 *
 * Step 4 is what the old code could not do: after step 1 the previous model is
 * still on disk, so [rollback] can restore it. A swap that fails for any reason
 * leaves the app exactly as it was.
 *
 * ## Why `rename` is a parameter
 *
 * [swap] takes the rename operation as an argument so a test can make step 2
 * fail deterministically. A test cannot make a real `rename` fail on demand,
 * and "it probably works" is not a test. The production call passes the real
 * `File.renameTo`; [verifyRenameReplacesOnThisPlatform] checks separately that
 * the assumption the fix rests on — that `renameTo` really does replace an
 * existing file here — actually holds on the device.
 */
object AtomicModelSwap {

    const val BACKUP_SUFFIX = ".bak"

    /** What [swap] did. */
    sealed interface Outcome {
        /**
         * The new file is in place. [backup] is the previous model, still on
         * disk; the caller decides whether to keep it (see [rollback]) or
         * discard it (see [discardBackup]). Null when there was no previous
         * model.
         */
        data class Swapped(val backup: File?) : Outcome

        /** Nothing changed. [why] names the reason for the log line. */
        data class NotSwapped(val why: String) : Outcome

        /** The move failed and the previous model was put back. */
        data class RolledBack(val why: String) : Outcome

        /**
         * The move failed **and** the previous model could not be put back.
         * It is still on disk at [backup], so nothing is lost, but the app has
         * to say so loudly instead of pretending the swap was a no-op.
         */
        data class RestoreFailed(val why: String, val backup: File) : Outcome
    }

    fun backupOf(target: File): File =
        File(target.parentFile, target.name + BACKUP_SUFFIX)

    /**
     * Install [newFile] as [target], keeping the previous file recoverable.
     *
     * @param rename the move primitive; defaults to the real one. Injecting a
     *   failing `rename` is how the failure path is tested.
     */
    fun swap(
        newFile: File,
        target: File,
        rename: (File, File) -> Boolean = { from, to -> from.renameTo(to) }
    ): Outcome {
        if (!newFile.isFile) {
            return Outcome.NotSwapped("the new file is missing")
        }

        val backup = backupOf(target)
        val hadPrevious = target.isFile

        if (hadPrevious) {
            // A .bak left behind by an import that was killed between the two
            // moves would make the rename below fail for a reason the user
            // cannot act on, so it is cleared first.
            if (backup.exists() && !backup.delete()) {
                return Outcome.NotSwapped("a stale ${backup.name} could not be removed")
            }
            if (!rename(target, backup)) {
                return Outcome.NotSwapped("the previous model could not be moved aside")
            }
        }

        if (rename(newFile, target)) {
            return Outcome.Swapped(if (hadPrevious) backup else null)
        }

        if (!hadPrevious) {
            return Outcome.NotSwapped("the new model could not be moved into place")
        }

        // A failed rename(2) leaves the target untouched, so all that is left
        // is to undo the move-aside.
        return if (rename(backup, target)) {
            Outcome.RolledBack("the new model could not be moved into place")
        } else {
            Outcome.RestoreFailed(
                "the new model could not be moved into place and the previous " +
                    "one could not be put back; it is at ${backup.absolutePath}",
                backup
            )
        }
    }

    /**
     * Undo a completed [Outcome.Swapped].
     *
     * Used when the file that was just installed turns out to be unusable:
     * the previous model goes back and the rejected one is removed.
     *
     * @return true when [target] is the previous model again.
     */
    fun rollback(
        backup: File?,
        target: File,
        rename: (File, File) -> Boolean = { from, to -> from.renameTo(to) }
    ): Boolean {
        if (backup == null || !backup.isFile) {
            // Nothing to put back: there was no previous model, so the only
            // correct outcome is to remove the file that cannot be used.
            return !target.exists() || target.delete()
        }
        if (target.exists() && !target.delete()) {
            return false
        }
        return rename(backup, target)
    }

    /**
     * Drop the backup of a swap that turned out to be good.
     *
     * Called only after the new model has been loaded successfully, so the
     * previous one is no longer needed and must not linger and be mistaken for
     * an import on the next launch.
     */
    fun discardBackup(backup: File?): Boolean =
        backup == null || !backup.exists() || backup.delete()
}