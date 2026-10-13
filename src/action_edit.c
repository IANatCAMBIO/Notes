/* ===========================================================================
 * action_edit.c — editing one action item from outside its note
 * (see action_edit.h)
 * =========================================================================== */

#include "action_edit.h"
#include "serialize.h"               /* document load / reconcile / save    */

/* Which field of the item an edit replaces.                                 */
typedef enum { ACTION_STRIKE, ACTION_DUE } action_edit_t;

/*
 * action_rewrite — re-render one field of the item's '!' line in an open
 * editor's document (a stored document is rewritten by the reconcile).
 *
 * Inputs:
 *   doc   — the document holding the line
 *   ord   — the line's position among the document's action lines
 *   which — the field
 *   done, due — the item's values after the edit
 *
 * Output: TRUE when the line was found and rewritten.
 */
static gboolean
action_rewrite(OnDocument *doc, gint ord, action_edit_t which,
               gboolean done, gint64 due)
{
    return which == ACTION_STRIKE ? on_document_action_strike(doc, ord, done)
                                  : on_document_action_due(doc, ord, due);
}

/*
 * action_apply — one edit (strike or due) of the item `uid`: the row
 * first, then the line, live or stored (see the header).  The line is
 * addressed by where the uid is NOW (on_document_action_ord) — unsaved
 * edits in an open editor included.
 *
 * Inputs:
 *   app   — the application context
 *   uid   — the item
 *   which — the edit
 *   done  — the new done state (ACTION_STRIKE; ignored otherwise)
 *   due   — the new due date (ACTION_DUE; ignored otherwise)
 *
 * Output: TRUE when a live item carries `uid` and its row was written.
 */
static gboolean
action_apply(OnApp *app, gint64 uid, action_edit_t which, gboolean done,
             gint64 due)
{
    OnActionItem *it = on_db_action_get(app->db, uid);
    if (it == NULL) {
        return FALSE;
    }
    /* The row, with one field replaced.                                    */
    if (which == ACTION_STRIKE) {
        due = it->due;
    } else {
        done = it->done;
    }
    gboolean ok      = on_db_action_set(app->db, uid,
                                        which == ACTION_STRIKE ? ON_ACTION_FIELD_DONE
                                                               : ON_ACTION_FIELD_DUE,
                                        it->text, done, due);
    gint64   note_id = it->note_id;
    on_db_action_item_free(it);
    if (!ok) {
        return FALSE;
    }

    /* Open in an editor: edit the live document and let the editor save.
     * NULL hook = headless; NULL document = the note is not open.          */
    OnDocument *live = app->live_document != NULL
                       ? app->live_document(app, note_id) : NULL;
    if (live != NULL) {
        gint ord = on_document_action_ord(live, uid);
        if (action_rewrite(live, ord, which, done, due)) {
            app->live_edited(app, note_id);
        } else {
            g_warning("action %" G_GINT64_FORMAT ": no line in the open "
                      "editor of note %" G_GINT64_FORMAT, uid, note_id);
        }
        return TRUE;
    }

    /* Closed: the row already holds this edit, so reconciling the stored
     * document against its rows IS the rewrite — along with any other
     * drift between the two — and the save writes it back.                */
    OnDocument *doc = on_note_document_load(app->db, note_id);
    if (doc == NULL) {
        g_warning("action %" G_GINT64_FORMAT ": note %" G_GINT64_FORMAT
                  " cannot be read", uid, note_id);
        return FALSE;
    }
    on_note_document_reconcile(app->db, note_id, doc);
    if (on_document_action_ord(doc, uid) < 0) {
        g_warning("action %" G_GINT64_FORMAT ": no line in note %"
                  G_GINT64_FORMAT, uid, note_id);
    } else {
        ok = on_note_document_save(app->db, note_id, doc, TRUE, NULL);
    }
    on_document_free(doc);
    return ok;
}

/*
 * on_action_edit_set_done — see action_edit.h.
 */
gboolean
on_action_edit_set_done(OnApp *app, gint64 uid, gboolean done)
{
    return action_apply(app, uid, ACTION_STRIKE, done, 0);
}

/*
 * on_action_edit_set_due — see action_edit.h.
 */
gboolean
on_action_edit_set_due(OnApp *app, gint64 uid, gint64 due)
{
    return action_apply(app, uid, ACTION_DUE, FALSE, due);
}
