/* ===========================================================================
 * action_sync.c — pulling outside changes to action items into the notes
 * (see action_sync.h)
 * =========================================================================== */

#include "action_sync.h"
#include "serialize.h"               /* load, reconcile (the pull), save     */

/* ---------------------------------------------------------------------------
 * LiveEdit — an open editor's document the pull rewrote, told so when the
 * transaction has ended (live_edited_end).
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp  *app;
    gint64  note_id;
} LiveEdit;

/*
 * live_edited_end — an on_db_txn_on_end hook: the pull's transaction has
 * ended; the editor whose document it rewrote redraws and queues its
 * autosave (OnApp.live_edited), committed or not — the rewrite is in the
 * document either way.  Registered before the editor's own save hook, so a
 * committed save then clears the dirty state this sets.  Frees `data`.
 *
 * Inputs:
 *   data      — the LiveEdit
 *   committed — unused
 *
 * Output: none.
 */
static void
live_edited_end(gpointer data, gboolean committed)
{
    LiveEdit *edit = data;
    (void)committed;
    edit->app->live_edited(edit->app, edit->note_id);
    g_free(edit);
}

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
    if (edited && live != NULL) {
        LiveEdit *edit = g_new0(LiveEdit, 1);
        edit->app     = app;
        edit->note_id = note_id;
        on_db_txn_on_end(db, live_edited_end, edit);
    }
    if (ok && need > 0) {
        ok = live != NULL ? app->live_save(app, note_id)
                          : on_note_document_save(db, note_id, doc, TRUE, NULL);
    }
    if (live == NULL) {
        on_note_document_release(db, doc);
    }
    return on_db_txn_end(db, own, ok);
}

/*
 * sync_refresh — another writer changed what the UI shows: every open
 * editor checks its note (OnApp.check_editors) and the library rebuilds
 * (notify_notes_changed).  Nothing in a headless context.
 *
 * Inputs:
 *   app — the application context, no transaction open
 *
 * Output: none.
 */
static void
sync_refresh(OnApp *app)
{
    if (app->check_editors != NULL) {
        app->check_editors(app);
    }
    if (app->notify_notes_changed != NULL) {
        app->notify_notes_changed(app);
    }
}

/*
 * on_action_sync_run — see action_sync.h.
 */
gint
on_action_sync_run(OnApp *app, gboolean refresh)
{
    GArray  *notes  = on_db_action_pending(app->db, 0);
    gint     done   = 0;
    gboolean failed = FALSE;         /* a note stays unsynced               */
    for (guint i = 0; i < notes->len; i++) {
        gint64 id = g_array_index(notes, gint64, i);
        if (on_action_sync_note(app, id)) {
            done++;
        } else {
            failed = TRUE;
            g_warning("sync: note %" G_GINT64_FORMAT " was not synced; the next poll retries", id);
        }
    }
    g_array_unref(notes);
    app->pull_failed = failed;
    if (done > 0 || refresh) {
        sync_refresh(app);
    }
    return done;
}

/*
 * on_action_sync_poll — see action_sync.h.
 */
gboolean
on_action_sync_poll(OnApp *app)
{
    gboolean changed;                /* another connection committed        */
    gint64   rev_tasks, rev_notes;   /* the revisions in the same snapshot  */
    if (app->db == NULL) {
        return FALSE;
    }
    if (!on_db_poll_revs(app->db, &changed, &rev_tasks, &rev_notes)) {
        app->pull_failed = TRUE;     /* the look itself failed: look again  */
        return FALSE;
    }
    if (!changed && !app->pull_failed) {
        app->rev_tasks = rev_tasks;  /* only Notes' own commits moved them  */
        app->rev_notes = rev_notes;
        return FALSE;
    }
    gboolean pull  = app->pull_failed || rev_tasks != app->rev_tasks;
    gboolean notes = rev_notes != app->rev_notes;
    app->rev_tasks = rev_tasks;
    app->rev_notes = rev_notes;
    if (pull) {
        on_action_sync_run(app, notes);
    } else if (notes) {
        sync_refresh(app);
    }
    return pull;
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
on_action_sync_start(OnApp *app, gboolean refresh)
{
    gboolean changed;                /* unused: everything is new here      */
    app->rev_tasks = app->rev_notes = 0;
    on_db_poll_revs(app->db, &changed, &app->rev_tasks, &app->rev_notes);
    on_action_sync_run(app, refresh);
    if (app->sync_timer == 0) {
        app->sync_timer = on_timer_add(SHARED_DB_POLL_SECONDS, true, sync_tick, app, NULL);
    }
}
