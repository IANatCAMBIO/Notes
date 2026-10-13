/* ===========================================================================
 * action_sync.c — pulling outside changes to action items into the notes
 * (see action_sync.h)
 * =========================================================================== */

#include "action_sync.h"
#include "serialize.h"               /* load, reconcile (the pull), save     */

/*
 * on_action_sync_note — see action_sync.h.
 */
gboolean
on_action_sync_note(OnApp *app, gint64 note_id)
{
    OnDatabase *db      = app->db;
    GArray     *pending = on_db_action_pending(db, note_id);
    gboolean    work    = pending->len > 0;
    g_array_unref(pending);
    if (!work) {
        return TRUE;
    }

    gboolean own;                    /* this call's transaction?            */
    if (!on_db_txn_begin(db, &own)) {
        return FALSE;
    }
    OnDocument *live = app->live_document != NULL ? app->live_document(app, note_id) : NULL;
    OnDocument *doc  = live != NULL ? live : on_note_document_load(db, note_id);
    gboolean    ok   = doc != NULL;
    if (!ok) {
        g_warning("sync: note %" G_GINT64_FORMAT " cannot be read; its items wait", note_id);
    }
    gboolean edited = FALSE;         /* the document was rewritten          */
    gint     need   = ok ? on_note_document_reconcile(db, note_id, doc, &edited) : -1;
    ok = need >= 0;
    if (ok && edited && live != NULL) {
        app->live_edited(app, note_id);
    }
    if (ok && need > 0) {
        ok = live != NULL ? app->live_save(app, note_id)
                          : on_note_document_save(db, note_id, doc, TRUE, NULL);
    }
    if (live == NULL && doc != NULL) {
        on_document_free(doc);
    }
    return on_db_txn_end(db, own, ok);
}

/*
 * on_action_sync_run — see action_sync.h.
 */
gint
on_action_sync_run(OnApp *app)
{
    GArray *notes = on_db_action_pending(app->db, 0);
    gint    done  = 0;
    for (guint i = 0; i < notes->len; i++) {
        gint64 id = g_array_index(notes, gint64, i);
        if (on_action_sync_note(app, id)) {
            done++;
        } else {
            g_warning("sync: note %" G_GINT64_FORMAT " was not synced", id);
        }
    }
    g_array_unref(notes);
    if (app->check_editors != NULL) {
        app->check_editors(app);
    }
    if (app->notify_notes_changed != NULL) {
        app->notify_notes_changed(app);
    }
    return done;
}

/*
 * on_action_sync_poll — see action_sync.h.
 */
gboolean
on_action_sync_poll(OnApp *app)
{
    if (app->db == NULL || shared_db_changed(app->db->shared) != 1) {
        return FALSE;
    }
    on_action_sync_run(app);
    return TRUE;
}

/*
 * sync_tick — the poll timer firing.
 *
 * Inputs:
 *   data — the OnApp
 *
 * Output: none.
 */
static void
sync_tick(void *data)
{
    on_action_sync_poll(data);
}

/*
 * on_action_sync_start — see action_sync.h.
 */
void
on_action_sync_start(OnApp *app)
{
    on_action_sync_run(app);
    if (app->sync_timer == 0) {
        app->sync_timer = on_timer_add(ON_ACTION_SYNC_POLL_S, true, sync_tick, app, NULL);
    }
}
