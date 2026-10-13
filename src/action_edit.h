/* ===========================================================================
 * action_edit.h — editing one action item from OUTSIDE its note
 *
 * An action item is a task row; the '!' line in a note renders it
 * (db.h, "An ACTION ITEM is a TASK ROW").  These two functions are the one
 * way the app changes an item other than by typing in the note: the
 * library's Action Items view (checkbox, due-date sheet).  (A task row
 * changed directly in the database — a rename, say — reaches the line
 * through on_note_document_reconcile, serialize.h.)
 *
 * Order, always: the TASK ROW first (on_db_action_set), then the line that
 * renders it —
 *   - on the note's OPEN editor when there is one: the document is reached
 *     through app->live_document, edited with the on_document_action_*
 *     rewrite, and app->live_edited tells the editor (which redraws and
 *     queues its autosave; the autosave's sync then finds the row equal);
 *   - otherwise on the stored document: loaded, reconciled against its task
 *     rows (which re-renders the line from the row just written) and saved
 *     at once.
 * Headless (a seeder or a test) app->live_document is NULL and only the
 * stored-document path exists.
 *
 * (In the GTK build this lived in editor_window.c as
 * on_editor_action_set_*.)
 *
 * Main thread only.
 * =========================================================================== */

#ifndef ON_ACTION_EDIT_H
#define ON_ACTION_EDIT_H

#include "app_core.h"

/* ---------------------------------------------------------------------------
 * on_action_edit_set_done() — set an item's done state: the task's status
 * folds the Tasks way (done -> DONE, undone -> IN_PROGRESS if it was DONE),
 * and the '!' line is struck through or un-struck.
 *
 * Inputs:
 *   app  — the application context (its db; its hooks decide live/stored)
 *   uid  — the item's stable identity
 *   done — the new state
 *
 * Output: TRUE when a live item carries `uid` and its row was written.  A
 *         line that cannot be found afterwards is logged; the row stands.
 * ------------------------------------------------------------------------- */
gboolean on_action_edit_set_done(OnApp *app, gint64 uid, gboolean done);

/* ---------------------------------------------------------------------------
 * on_action_edit_set_due() — set or clear an item's due date: the row, then
 * the line's trailing "due <date>" (written ISO, "due 2026-07-07"; appended
 * text inherits the item's strike state).
 *
 * Inputs:
 *   app — the application context
 *   uid — the item's stable identity
 *   due — local-midnight UNIX timestamp, or 0 to clear
 *
 * Output: as on_action_edit_set_done.
 * ------------------------------------------------------------------------- */
gboolean on_action_edit_set_due(OnApp *app, gint64 uid, gint64 due);

#endif /* ON_ACTION_EDIT_H */
