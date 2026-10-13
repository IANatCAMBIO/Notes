/* ===========================================================================
 * action_edit.h — editing one action item from OUTSIDE its note
 *
 * An action item is a task row; the '!' line in a note renders it
 * (db.h, "An ACTION ITEM is a TASK ROW").  These two functions are the one
 * way the app changes an item other than by typing in the note: the
 * library's Action Items view (checkbox, due-date sheet).
 *
 * An edit is exactly what an edit in the Tasks app is — the one field is
 * written to the task (on_db_action_set: that field only, the push's status
 * rule) — followed at once, in the SAME transaction, by the pull for its
 * note (action_sync.h, on_action_sync_note), which renders the field into
 * the line: on the note's open editor when there is one (saved at once),
 * else on the stored document.  Headless (a seeder or a test) only the
 * stored path exists.
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
 * Output: TRUE when a live item carries `uid` and the task and its line
 *         were written; FALSE otherwise (nothing written).
 * ------------------------------------------------------------------------- */
gboolean on_action_edit_set_done(OnApp *app, gint64 uid, gboolean done);

/* ---------------------------------------------------------------------------
 * on_action_edit_set_due() — set or clear an item's due date: the task,
 * then the line's trailing "due <date>" (written ISO, "due 2026-07-07";
 * appended text inherits the item's strike state).
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
