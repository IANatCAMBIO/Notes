/* ===========================================================================
 * action_edit.c — editing one action item from outside its note
 * (see action_edit.h)
 * =========================================================================== */

#include "action_edit.h"
#include "action_sync.h"             /* the pull renders the line           */

/*
 * action_apply — one edit of the item `uid`: the task field, then the
 * pull of its note, in one transaction.
 *
 * Inputs:
 *   app  — the application context
 *   uid  — the item
 *   mask — the field (ON_ACTION_FIELD_DONE or ON_ACTION_FIELD_DUE)
 *   done — the new done state (ON_ACTION_FIELD_DONE; ignored otherwise)
 *   due  — the new due date (ON_ACTION_FIELD_DUE; ignored otherwise)
 *
 * Output: TRUE when a live item carries `uid` and everything was written.
 */
static gboolean
action_apply(OnApp *app, gint64 uid, guint mask, gboolean done, gint64 due)
{
    OnActionItem *it = on_db_action_get(app->db, uid);
    if (it == NULL) {
        return FALSE;
    }
    gint64   note_id = it->note_id;
    gboolean own;                    /* this call's transaction?            */
    gboolean ok = on_db_txn_begin(app->db, &own);
    if (ok) {
        ok = on_db_action_set(app->db, uid, mask, it->text, done, due) &&
             on_action_sync_note(app, note_id);
        ok = on_db_txn_end(app->db, own, ok);
    }
    on_db_action_item_free(it);
    return ok;
}

/*
 * on_action_edit_set_done — see action_edit.h.
 */
gboolean
on_action_edit_set_done(OnApp *app, gint64 uid, gboolean done)
{
    return action_apply(app, uid, ON_ACTION_FIELD_DONE, done, 0);
}

/*
 * on_action_edit_set_due — see action_edit.h.
 */
gboolean
on_action_edit_set_due(OnApp *app, gint64 uid, gint64 due)
{
    return action_apply(app, uid, ON_ACTION_FIELD_DUE, FALSE, due);
}
