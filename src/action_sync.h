/* ===========================================================================
 * action_sync.h — pulling outside changes to action items into the notes
 *
 * Notes shares its database with Tasks (and with agents using the shared
 * skill).  A '!' line's task can be renamed, completed, re-dated or
 * deleted by any of them; this module brings those changes into the notes
 * that carry the lines, eagerly, so note text, titles and search stay right
 * without the note being opened.  (The other direction — a line edited in
 * Notes — is the push of every save: serialize.h, on_db_note_save_actions.)
 *
 * WHEN: at launch and when a database is opened (on_action_sync_start),
 * then on every poll (on_action_sync_poll: every SHARED_DB_POLL_SECONDS
 * seconds and when the app becomes active).  One read snapshot
 * (on_db_poll_revs) says whether ANOTHER connection committed
 * (shared_db_changed) and gives the change revisions (shared_db_rev):
 *   - no outside commit: every rev movement since the last look was
 *     Notes' own; the revs are re-read into app->rev_* and nothing runs
 *     (unless the last pull failed: see below);
 *   - 'tasks' moved (or the last pull failed): the pull runs;
 *   - only 'notes' moved: no pull — the editors check their notes and the
 *     library refreshes (another writer changed notes, folders or tags);
 *   - neither moved (e.g. only shared_settings changed): nothing.
 * A pull that left a note unsynced sets app->pull_failed, and the next
 * tick pulls again whether or not anything else committed.
 *
 * WHAT, per note with work (on_db_action_pending), in ONE write
 * transaction: the pull (on_note_document_reconcile: rewrite or unmark the
 * lines, move seen_*) and, when that asks for it, the save with its push.
 *   - note open in an editor: the editor's live document is reconciled
 *     (each rewrite an undo group; caret, selection and scroll stay) and
 *     the editor writes at once (OnApp.live_save) inside the transaction;
 *   - note closed: the stored document is loaded, reconciled and saved.
 * Nothing touches the UI inside the transaction: the editor's redraw
 * (OnApp.live_edited) and its "saved" state run when the transaction has
 * ended (on_db_txn_on_end) — and an editor whose transaction did not
 * commit stays dirty.  Then, only when a note was synced or the 'notes'
 * rev moved, every open editor checks its note for a rewrite by another
 * writer (OnApp.check_editors) and the library is fully refreshed
 * (notify_notes_changed).
 *
 * Main thread only: the app connection, the editors and the library are
 * main-thread state.  Headless (no hooks) every note is the closed case.
 * =========================================================================== */

#ifndef ON_ACTION_SYNC_H
#define ON_ACTION_SYNC_H

#include "app_core.h"

/* ---------------------------------------------------------------------------
 * on_action_sync_note() — the pull for one note, in one write transaction
 * (or the caller's, when one is open: an edit from the Action Items view
 * writes its task field first, in the same transaction).  Writes nothing
 * when the note has no work.
 *
 * Inputs:
 *   app     — the application context, its database open
 *   note_id — the note
 *
 * Output: TRUE on success (also when there was nothing to do); FALSE when
 *         the note could not be read or saved (everything rolled back).
 * ------------------------------------------------------------------------- */
gboolean on_action_sync_note(OnApp *app, gint64 note_id);

/* ---------------------------------------------------------------------------
 * on_action_sync_run() — the whole pull: every note with work, each in its
 * own transaction; app->pull_failed records whether one failed.  Then,
 * when a note was synced or `refresh` asks for it, the editors' check and
 * the library's refresh (both through hooks; nothing headless) — after
 * every transaction has ended.
 *
 * Inputs:
 *   app     — the application context, its database open
 *   refresh — refresh even when nothing was synced (the 'notes' rev
 *             moved, or another database was opened)
 *
 * Output: how many notes were synced (failures are logged and skipped).
 * ------------------------------------------------------------------------- */
gint on_action_sync_run(OnApp *app, gboolean refresh);

/* ---------------------------------------------------------------------------
 * on_action_sync_poll() — one look at the shared file (see WHEN above):
 * the pull when the 'tasks' rev moved or the last pull failed, the
 * editors' check and the library's refresh when only the 'notes' rev
 * moved, nothing otherwise.  What the timer and the app's activation call.
 *
 * Inputs:
 *   app — the application context (nothing happens with no database)
 *
 * Output: TRUE when the pull ran.
 * ------------------------------------------------------------------------- */
gboolean on_action_sync_poll(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_action_sync_start() — a database was just opened (launch, or File >
 * Open Database): take its revisions as the baseline, run the pull once
 * now, and arm the poll timer (app->sync_timer), once.  on_app_shutdown
 * removes it.
 *
 * Inputs:
 *   app     — the application context, its database open
 *   refresh — refresh the editors and the library even when nothing was
 *             synced (another database replaced the one they show)
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_action_sync_start(OnApp *app, gboolean refresh);

#endif /* ON_ACTION_SYNC_H */
