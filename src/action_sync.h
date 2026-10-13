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
 * WHEN: at launch (on_action_sync_start), then whenever the shared module
 * reports that ANOTHER connection committed to the file (shared_db_changed
 * on the app's connection, polled every SHARED_DB_POLL_SECONDS seconds and
 * when the app becomes active).  Notes' own commits on the app connection
 * never count, so a pull's own saves do not trigger another pull; a pull
 * that finds nothing to change writes nothing.
 *
 * WHAT, per note with work (on_db_action_pending), in ONE write
 * transaction: the pull (on_note_document_reconcile: rewrite or unmark the
 * lines, move seen_*) and, when that asks for it, the save with its push.
 *   - note open in an editor: the editor's live document is reconciled
 *     (each rewrite an undo group; caret, selection and scroll stay) and
 *     the editor saves at once (OnApp.live_save) inside the transaction;
 *   - note closed: the stored document is loaded, reconciled and saved.
 * Then every open editor checks its note for a rewrite by another writer
 * (OnApp.check_editors), and the library is fully refreshed
 * (notify_notes_changed: the Action Items pane, the notes list's titles
 * and dates, the previews and search text that the saves rewrote).
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
 * on_action_sync_run() — the whole pull: every note with work, then the
 * editors' check and the library's refresh (both through hooks; nothing
 * in a headless context).
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: how many notes were synced (failures are logged and skipped).
 * ------------------------------------------------------------------------- */
gint on_action_sync_run(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_action_sync_poll() — run the pull if another connection committed
 * since the last look (shared_db_changed).  What the timer and the app's
 * activation call.
 *
 * Inputs:
 *   app — the application context (nothing happens with no database)
 *
 * Output: TRUE when there was a change and the pull ran.
 * ------------------------------------------------------------------------- */
gboolean on_action_sync_poll(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_action_sync_start() — run the pull once now (launch, or a database
 * just opened) and arm the poll timer (app->sync_timer), once.
 * on_app_shutdown removes it.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_action_sync_start(OnApp *app);

#endif /* ON_ACTION_SYNC_H */
