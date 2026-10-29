/* ===========================================================================
 * action_sync.h — pulling outside changes to action items into the notes,
 * and tags written into them from outside the editor
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
 * seconds and when the app becomes active), and right after each of
 * Notes' OWN commits (a commit hook on the connection posts one look to
 * the main run loop, however many commits precede it).  Each look is
 * shared_db_poll — the module's one snapshot of whether ANOTHER connection
 * committed and of both change revisions:
 *   - no outside commit: every rev movement since the last look was
 *     Notes' own; the revs are adopted into app->rev_* and nothing runs
 *     (unless the last pull failed and this is the timer's tick: see
 *     below) — so Notes' own saves never refresh the library;
 *   - 'tasks' moved (or the last pull failed): the pull runs;
 *   - only 'notes' moved: no pull — the editors check their notes and the
 *     library refreshes (another writer changed notes or tags);
 *   - neither moved (e.g. only shared_settings changed): nothing.
 * Every outside commit, whatever moved, is then reported through
 * OnApp.notify_outside_commit (the day planner re-reads its day: Tasks'
 * calendar sync writes tables no revision covers).
 * A pull that left a note unsynced sets app->pull_failed, and the next
 * tick pulls again whether or not anything else committed.  A note whose
 * open editor waits for the user to answer its "changed outside" panel
 * (OnApp.live_held) is not a failure: the pull leaves it alone, and the
 * answer runs the pull (on_action_sync_run).
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
 * Output: TRUE on success (also when there was nothing to do, or the note
 *         is held: OnApp.live_held); FALSE when the note could not be
 *         read or saved (everything rolled back).
 * ------------------------------------------------------------------------- */
gboolean on_action_sync_note(OnApp *app, gint64 note_id);

/* ---------------------------------------------------------------------------
 * on_action_sync_run() — the whole pull: every note with work and not
 * held (OnApp.live_held), each in its own transaction; app->pull_failed
 * records whether one failed.  Then,
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
 * on_note_append_tags() — give a note tags from outside its text (a drop on
 * a sidebar tag): each name
 * becomes a #tag on the note's tag line (on_document_tag_word,
 * on_document_append_tag; one it has already is skipped), and the note is
 * saved with its text tags, in one write transaction (or the caller's).
 * An open editor's live document takes them as the pull's edits do (see
 * WHAT above), so they are one undo step there.
 *
 * Inputs:
 *   app     — the application context, its database open
 *   note_id — the note
 *   names   — tag names in any spelling ("Old Projects" → #OldProjects)
 *   n       — how many
 *
 * Output: TRUE on success (also when the note had every tag already);
 *         FALSE when the note could not be read or saved, or its editor
 *         waits for the "changed outside" panel (nothing written).
 * ------------------------------------------------------------------------- */
gboolean on_note_append_tags(OnApp *app, gint64 note_id, const gchar *const *names,
                             gsize n);

/* ---------------------------------------------------------------------------
 * on_note_remove_tags() — take tags out of a note's text
 * (on_document_remove_tag), the way on_note_append_tags puts them in: the
 * open editor's document when it has one, one undo step there, else the
 * stored note.  A tag the note does not carry is skipped; a tag no note and
 * no task carries any more is deleted by the schema's triggers.
 *
 * Inputs:
 *   app     — the application context, its database open
 *   note_id — the note
 *   names   — tag names in any spelling
 *   n       — how many
 *
 * Output: TRUE on success (also when the note carried none of them);
 *         FALSE when the note could not be read or saved, or its editor
 *         waits for the "changed outside" panel (nothing written).
 * ------------------------------------------------------------------------- */
gboolean on_note_remove_tags(OnApp *app, gint64 note_id, const gchar *const *names,
                             gsize n);

/* ---------------------------------------------------------------------------
 * on_action_sync_start() — a database was just opened (launch, or File >
 * Open Database): take its revisions as the baseline, hook its commits
 * (the look after each of Notes' own), run the pull once now, and arm
 * the poll timer (app->sync_timer), once.  on_app_shutdown
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
