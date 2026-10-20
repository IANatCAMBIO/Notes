/* ===========================================================================
 * action_sync.c — pulling outside changes to action items into the notes,
 * and tags written into them from outside the editor (see action_sync.h)
 * =========================================================================== */

#include "action_sync.h"
#include "serialize.h"               /* load, reconcile (the pull), save     */

#include <stdlib.h>

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
 * sync_held — is the note held back from the pull: its open editor waits
 * for the user to answer the "changed outside" panel (OnApp.live_held)?
 * Writing it now would only be refused and rolled back; the answer runs
 * the pull (on_action_sync_run).
 *
 * Inputs:
 *   app     — the application context
 *   note_id — the note
 *
 * Output: TRUE when the pull must leave the note alone for now.
 */
static gboolean
sync_held(OnApp *app, gint64 note_id)
{
    return app->live_held != NULL && app->live_held(app, note_id);
}

/*
 * on_action_sync_note — see action_sync.h.
 */
gboolean
on_action_sync_note(OnApp *app, gint64 note_id)
{
    OnDatabase *db      = app->db;
    if (sync_held(app, note_id)) {
        return TRUE;
    }
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
 * edit_note_tags — change a note's #tags with a document edit, applied to
 * the open editor's document when the note is being edited, else to the
 * stored one, in one transaction.
 *
 * Inputs:
 *   app     — the application context, its database open
 *   note_id — the note
 *   names   — tag names in any spelling (on_document_tag_word)
 *   n       — how many
 *   apply   — the edit for one tag word; TRUE when it changed the document
 *
 * Output: TRUE on success (also when nothing needed changing); FALSE when
 *         the note could not be read or saved, or its editor waits for the
 *         "changed outside" panel (nothing written).
 */
static gboolean
edit_note_tags(OnApp *app, gint64 note_id, const gchar *const *names, gsize n,
               gboolean (*apply)(OnDocument *d, const gchar *word))
{
    OnDatabase *db = app->db;
    if (sync_held(app, note_id)) {
        return FALSE;
    }
    gboolean own;                    /* this call's transaction?            */
    if (!on_db_txn_begin(db, &own)) {
        return FALSE;
    }
    OnDocument *live = app->live_document != NULL ? app->live_document(app, note_id) : NULL;
    OnDocument *doc  = live != NULL ? live : on_note_document_load(db, note_id);
    gboolean    ok   = doc != NULL;
    gboolean    edited = FALSE;      /* the document was changed            */
    for (gsize i = 0; ok && i < n; i++) {
        gchar *word = on_document_tag_word(names[i]);
        if (word != NULL && apply(doc, word)) {
            edited = TRUE;
        }
        g_free(word);
    }
    if (edited && live != NULL) {
        LiveEdit *edit = g_new0(LiveEdit, 1);
        edit->app     = app;
        edit->note_id = note_id;
        on_db_txn_on_end(db, live_edited_end, edit);
    }
    if (ok && edited) {
        ok = live != NULL ? app->live_save(app, note_id)
                          : on_note_document_save(db, note_id, doc, FALSE, NULL) &&
                            on_note_document_save_tags(db, note_id, doc);
    }
    if (live == NULL && doc != NULL) {
        on_note_document_release(db, doc);
    }
    return on_db_txn_end(db, own, ok);
}

/*
 * on_note_append_tags — see action_sync.h.
 */
gboolean
on_note_append_tags(OnApp *app, gint64 note_id, const gchar *const *names, gsize n)
{
    return edit_note_tags(app, note_id, names, n, on_document_append_tag);
}

/*
 * on_note_remove_tags — see action_sync.h.
 */
gboolean
on_note_remove_tags(OnApp *app, gint64 note_id, const gchar *const *names, gsize n)
{
    return edit_note_tags(app, note_id, names, n, on_document_remove_tag);
}

/*
 * field_tags_to_text — one note's old Tags field written into its text
 * (on_note_append_tags) and emptied, keeping its updated_at, in one
 * transaction.
 *
 * Inputs:
 *   app     — the application context, no transaction open
 *   note_id — a note with field tags (on_db_field_tag_notes)
 *
 * Output: TRUE on success; FALSE when nothing was written (logged).
 */
static gboolean
field_tags_to_text(OnApp *app, gint64 note_id)
{
    OnDatabase *db = app->db;
    gboolean    own;                 /* this call's transaction?            */
    if (!on_db_txn_begin(db, &own)) {
        return FALSE;
    }
    OnNoteStamp stamp;               /* updated_at before the rewrite       */
    GList      *tags  = on_db_note_tags(db, note_id);
    GPtrArray  *names = g_ptr_array_new();
    for (GList *l = tags; l != NULL; l = l->next) {
        const OnTag *t = l->data;
        if (t->in_field) {
            g_ptr_array_add(names, t->name);
        }
    }
    gboolean ok = on_db_note_stamp(db, note_id, &stamp) &&
                  on_note_append_tags(app, note_id, (const gchar *const *)names->pdata,
                                      names->len) &&
                  on_db_note_field_tags_clear(db, note_id) &&
                  on_db_note_set_updated_at(db, note_id, stamp.updated_at);
    g_ptr_array_free(names, TRUE);
    on_db_tag_list_free(tags);
    if (!ok) {
        g_warning("tags: note %" G_GINT64_FORMAT "'s Tags field could not go into its text",
                  note_id);
    }
    return on_db_txn_end(db, own, ok);
}

/*
 * on_note_field_tags_to_text — see action_sync.h.
 */
gint
on_note_field_tags_to_text(OnApp *app)
{
    GArray *notes = on_db_field_tag_notes(app->db);
    gint    done  = 0;
    for (guint i = 0; i < notes->len; i++) {
        if (field_tags_to_text(app, g_array_index(notes, gint64, i))) {
            done++;
        }
    }
    g_array_unref(notes);
    return done;
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
        if (sync_held(app, id)) {
            continue;
        }
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
 * sync_poll — one look at the shared file (shared_db_poll: whether another
 * connection committed, and both revisions, in one snapshot), then what
 * it calls for: no outside commit — every rev movement was Notes' own, so
 * the revs are adopted and nothing runs (unless `retry` and the last pull
 * failed); else the pull when 'tasks' moved, the refresh when 'notes'
 * moved.
 *
 * Inputs:
 *   app   — the application context (nothing happens with no database)
 *   retry — pull again when the last pull failed, outside commit or not
 *           (the timer's tick; not the look after Notes' own commit)
 *
 * Output: TRUE when the pull ran.
 */
static gboolean
sync_poll(OnApp *app, gboolean retry)
{
    int      changed;                /* another connection committed        */
    int64_t  rev_tasks, rev_notes;   /* the revisions in the same snapshot  */
    char    *err = NULL;             /* the module's message                */
    if (app->db == NULL) {
        return FALSE;
    }
    if (shared_db_poll(app->db->shared, &changed, &rev_tasks, &rev_notes, &err) != 0) {
        g_warning("sync: cannot look for changes: %s", err != NULL ? err : "?");
        free(err);
        app->pull_failed = TRUE;     /* the look itself failed: look again  */
        return FALSE;
    }
    gboolean failed = retry && app->pull_failed;
    if (!changed && !failed) {
        app->rev_tasks = rev_tasks;  /* only Notes' own commits moved them  */
        app->rev_notes = rev_notes;
        return FALSE;
    }
    gboolean pull  = failed || rev_tasks != app->rev_tasks;
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
 * on_action_sync_poll — see action_sync.h.
 */
gboolean
on_action_sync_poll(OnApp *app)
{
    return sync_poll(app, TRUE);
}

/*
 * sync_after_commit — the look posted by sync_commit_hook: right after
 * Notes' own commit, adopt the revisions it moved (no outside commit since
 * the last look) or handle what another writer moved meanwhile.
 *
 * Inputs:
 *   data — the OnApp
 *
 * Output: none.
 */
static void
sync_after_commit(void *data)
{
    OnApp *app = data;
    app->commit_look_posted = FALSE;
    sync_poll(app, FALSE);
}

/*
 * sync_commit_hook — sqlite3_commit_hook on the app's connection: a write
 * transaction of Notes' own (explicit or autocommit) is committing.  It
 * must not touch the connection, so it only posts the look that follows
 * (sync_after_commit), once however many commits come before it runs.
 *
 * Inputs:
 *   data — the OnApp
 *
 * Output: 0 — the commit goes ahead.
 */
static int
sync_commit_hook(void *data)
{
    OnApp *app = data;
    if (!app->commit_look_posted) {
        app->commit_look_posted = TRUE;
        on_main_post(sync_after_commit, app);
    }
    return 0;
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
    int      changed;                /* unused: everything is new here      */
    int64_t  rev_tasks = 0, rev_notes = 0;
    on_note_field_tags_to_text(app);
    shared_db_poll(app->db->shared, &changed, &rev_tasks, &rev_notes, NULL);
    app->rev_tasks = rev_tasks;
    app->rev_notes = rev_notes;
    sqlite3_commit_hook(app->db->handle, sync_commit_hook, app);
    on_action_sync_run(app, refresh);
    if (app->sync_timer == 0) {
        app->sync_timer = on_timer_add(SHARED_DB_POLL_SECONDS, true, sync_tick, app, NULL);
    }
}
