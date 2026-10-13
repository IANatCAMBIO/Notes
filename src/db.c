/* ===========================================================================
 * db.c — SQLite persistence layer for Notes (implementation)
 *
 * See db.h for the public API and schema overview.  All functions log
 * failures through g_warning() and return a "failed" value rather than
 * aborting, so the UI can degrade gracefully.
 * =========================================================================== */

#include "db.h"
#include "main_service.h"            /* the health pass's worker + hand-back */

#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>

/* The async health pass in flight on a connection (health section).    */
typedef struct HealthJob HealthJob;
static void on_db_health_abandon(OnDatabase *db);

/* The action_items_ctx view (shared_db.c creates it): every LIVE action
 * item as a task row with its note's placement — note_id, ord, text (the
 * task's title), done (status DONE), due, uid, folder_id, updated_at (the
 * note's), visible (the note is outside the Trash) and context (the name
 * of the note's folder, "" at the root).  Every listing reads it.          */
#define ACTION_CTX_VIEW_NAME "action_items_ctx"

/* Column list shared by the action-item queries, matching
 * run_action_query()'s expectations.                                        */
#define ACTION_COLS "note_id, ord, text, done, due, uid"

/* Remove tags that no longer label any note; run (via exec_simple) after
 * anything that can delete note_tags rows, so the library's tag list
 * stays tidy.                                                                */
#define PRUNE_ORPHAN_TAGS_SQL \
    "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)"

/* ---------------------------------------------------------------------------
 * exec_simple() — run a parameterless SQL string, logging any error.
 *   db  — open database.
 *   sql — SQL text to execute.
 * Returns TRUE if the statement(s) executed without error.
 * ------------------------------------------------------------------------- */
static gboolean
exec_simple(OnDatabase *db, const char *sql)
{
    char *errmsg = NULL;                 /* sqlite-allocated error string   */
    if (sqlite3_exec(db->handle, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        g_warning("db: exec failed: %s (sql: %.80s)", errmsg, sql);
        sqlite3_free(errmsg);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * txn_begin() — start a write transaction (shared_db_begin: BEGIN
 * IMMEDIATE, so a busy file is waited for up front and a write never fails
 * half-way with SQLITE_BUSY) — or JOIN the caller's, when one is already
 * open on this connection: several writes (a pull, its save, a note's
 * tags) are then one transaction.
 *   db  — open database.
 *   own — out: TRUE when this call opened the transaction (txn_end then
 *         commits it), FALSE when it joined the caller's.
 * Returns TRUE when a transaction is open.
 * ------------------------------------------------------------------------- */
static gboolean
txn_begin(OnDatabase *db, gboolean *own)
{
    *own = FALSE;
    if (sqlite3_get_autocommit(db->handle) == 0)
        return TRUE;
    char *err = NULL;                /* the module's message               */
    if (shared_db_begin(db->shared, &err) != 0) {
        g_warning("db: cannot begin a transaction: %s", err != NULL ? err : "?");
        free(err);
        return FALSE;
    }
    *own = TRUE;
    return TRUE;
}

/* One hook registered with on_db_txn_on_end.                               */
typedef struct {
    OnDbTxnHook hook;
    gpointer    data;
} TxnHook;

/* ---------------------------------------------------------------------------
 * txn_run_hooks() — the transaction has ended (committed or not): run and
 * forget every hook registered with on_db_txn_on_end, in order.  The list
 * is taken first, so a hook may open a transaction of its own.
 *   db        — open database, no transaction open.
 *   committed — whether the transaction's work was committed.
 * ------------------------------------------------------------------------- */
static void
txn_run_hooks(OnDatabase *db, gboolean committed)
{
    GArray *hooks = db->txn_hooks;   /* the ended transaction's hooks       */
    if (hooks == NULL || hooks->len == 0)
        return;
    db->txn_hooks = g_array_new(FALSE, FALSE, sizeof(TxnHook));
    for (guint i = 0; i < hooks->len; i++) {
        const TxnHook *h = &g_array_index(hooks, TxnHook, i);
        h->hook(h->data, committed);
    }
    g_array_unref(hooks);
}

/* ---------------------------------------------------------------------------
 * txn_end() — finish what txn_begin started: COMMIT when all of it
 * succeeded, ROLLBACK otherwise (a failed COMMIT rolls back too), then the
 * on_db_txn_on_end hooks.  A joined transaction is left to its owner (who
 * sees `ok` and rolls back on FALSE).
 *   db  — open database, inside the transaction.
 *   own — txn_begin's `own`.
 *   ok  — whether every step in it succeeded.
 * Returns TRUE only when every step succeeded (and, when owned, committed).
 * ------------------------------------------------------------------------- */
static gboolean
txn_end(OnDatabase *db, gboolean own, gboolean ok)
{
    if (!own)
        return ok;
    gboolean committed = FALSE;      /* the work is in the file             */
    char    *err       = NULL;       /* the module's message                */
    if (!ok) {
        shared_db_rollback(db->shared);
    } else if (shared_db_commit(db->shared, &err) != 0) {
        g_warning("db: commit failed: %s", err != NULL ? err : "?");
        free(err);
    } else {
        committed = TRUE;
    }
    txn_run_hooks(db, committed);
    return committed;
}

/* ---------------------------------------------------------------------------
 * prepare() — wrap sqlite3_prepare_v2 with error logging.
 *   db  — open database.
 *   sql — single SQL statement with '?' placeholders.
 * Returns a prepared statement to finalize with sqlite3_finalize(), or
 * NULL on error.
 * ------------------------------------------------------------------------- */
static sqlite3_stmt *
prepare(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;           /* the compiled statement          */
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        g_warning("db: prepare failed: %s (sql: %.80s)",
                  sqlite3_errmsg(db->handle), sql);
        return NULL;
    }
    return stmt;
}

/* ---------------------------------------------------------------------------
 * bind_id_or_null() — bind a folder/parent id, mapping 0 to SQL NULL.
 * The schema uses NULL (not 0) to mean "top level", so every id bind for
 * a nullable column goes through this helper.
 *   stmt — statement to bind into.
 *   idx  — 1-based parameter index.
 *   id   — the id value; 0 means NULL.
 * ------------------------------------------------------------------------- */
static void
bind_id_or_null(sqlite3_stmt *stmt, int idx, gint64 id)
{
    if (id > 0)
        sqlite3_bind_int64(stmt, idx, id);
    else
        sqlite3_bind_null(stmt, idx);
}

/* ---------------------------------------------------------------------------
 * stmt_done() — run a fully-bound single DML statement to completion.
 *   db   — open database (for the error message).
 *   stmt — prepared and bound statement, or NULL (a failed prepare);
 *          consumed (finalized) in every case.
 * Steps once and logs a g_warning() on anything but SQLITE_DONE.
 * Returns TRUE when the statement completed.
 * ------------------------------------------------------------------------- */
static gboolean
stmt_done(OnDatabase *db, sqlite3_stmt *stmt);      /* see stmt_finish below */

/* stmt_cache_finalize() — GDestroyNotify wrapper for sqlite3_finalize,
 * used as the value-destroy function of stmt_cache.                          */
static void
stmt_cache_finalize(gpointer stmt)
{
    sqlite3_finalize((sqlite3_stmt *)stmt);
}

/* ---------------------------------------------------------------------------
 * cached_prepare() — look up a compiled statement in the per-connection cache;
 * prepare and cache it on the first call.  Returns the statement reset and
 * with all bindings cleared, ready for fresh binding.  The caller must NOT
 * finalize it — the cache owns the lifetime.
 *   db  — open database.
 *   sql — string literal (the pointer is used as the cache key).
 * Returns the statement, or NULL on a prepare failure.
 * ------------------------------------------------------------------------- */
static sqlite3_stmt *
cached_prepare(OnDatabase *db, const char *sql)
{
    if (db->stmt_cache == NULL)
        return prepare(db, sql);
    sqlite3_stmt *stmt = g_hash_table_lookup(db->stmt_cache, sql);
    if (stmt != NULL) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return stmt;
    }
    stmt = prepare(db, sql);
    if (stmt != NULL)
        g_hash_table_insert(db->stmt_cache, (gpointer)sql, stmt);
    return stmt;
}

/* ---------------------------------------------------------------------------
 * stmt_finish() — run a fully-bound single DML statement to completion and
 * dispose of it according to who owns it.  The shared body of stmt_done()
 * and cached_stmt_done(), which differ only in that.
 *   cached — TRUE for a statement the cache owns (reset + clear bindings,
 *            never finalize); FALSE for a one-off (finalize).
 * ------------------------------------------------------------------------- */
static gboolean
stmt_finish(OnDatabase *db, sqlite3_stmt *stmt, gboolean cached)
{
    if (stmt == NULL)
        return FALSE;
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok)
        g_warning("db: statement failed: %s", sqlite3_errmsg(db->handle));
    if (cached) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    } else {
        sqlite3_finalize(stmt);
    }
    return ok;
}

static gboolean
stmt_done(OnDatabase *db, sqlite3_stmt *stmt)
{
    return stmt_finish(db, stmt, FALSE);
}

/* ---------------------------------------------------------------------------
 * cached_stmt_done() — execute a cached DML statement then reset it for
 * reuse.  Unlike stmt_done(), the statement is NOT finalized.
 * ------------------------------------------------------------------------- */
static gboolean
cached_stmt_done(OnDatabase *db, sqlite3_stmt *stmt)
{
    return stmt_finish(db, stmt, TRUE);
}

/* ---------------------------------------------------------------------------
 * query_int64() — first column of the first row of a scalar SELECT.
 *   db      — open database.
 *   sql     — single SELECT with at most one integer '?' placeholder.
 *   bind_id — value for that placeholder, or a negative value for none.
 * Returns the value, or 0 when the query fails or returns no row.
 * ------------------------------------------------------------------------- */
static gint64
query_int64(OnDatabase *db, const char *sql, gint64 bind_id)
{
    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt == NULL)
        return 0;
    if (bind_id >= 0)
        sqlite3_bind_int64(stmt, 1, bind_id);
    gint64 value = 0;                    /* result, 0 when no row           */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

/* =========================================================================
 * lifecycle
 * ========================================================================= */

OnDatabase *
on_db_open(const gchar *path, gchar **err)
{
    char *module_err = NULL;         /* the shared module's message         */
    char *resolved   = path != NULL ? strdup(path) : shared_db_path(&module_err);
    shared_db *shared = resolved != NULL ? shared_db_open(resolved, &module_err)
                                         : NULL;
    if (shared == NULL) {
        g_warning("db: cannot open %s: %s", resolved != NULL ? resolved : "the database",
                  module_err != NULL ? module_err : "?");
        if (err != NULL)
            *err = g_strdup(module_err != NULL ? module_err : "unknown error");
        free(module_err);
        free(resolved);
        return NULL;
    }
    free(resolved);

    OnDatabase *db = g_new0(OnDatabase, 1);
    db->shared = shared;
    db->handle = shared_db_handle(shared);
    db->path   = g_strdup(shared_db_file(shared));
    /* Statements are keyed by SQL literal pointer; finalized on db_close.   */
    db->stmt_cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                           stmt_cache_finalize);
    db->txn_hooks  = g_array_new(FALSE, FALSE, sizeof(TxnHook));
    return db;
}

/*
 * on_db_close — see db.h.  A transaction still open is rolled back and its
 * on_db_txn_on_end hooks run (not committed) before the connection goes.
 */
void
on_db_close(OnDatabase *db)
{
    if (db == NULL)
        return;
    if (sqlite3_get_autocommit(db->handle) == 0)
        shared_db_rollback(db->shared);
    txn_run_hooks(db, FALSE);
    g_array_unref(db->txn_hooks);
    /* Finalize all cached statements BEFORE closing the connection.          */
    if (db->stmt_cache != NULL) {
        g_hash_table_destroy(db->stmt_cache);
        db->stmt_cache = NULL;
    }
    shared_db_close(db->shared);      /* NULL-safe; closes the handle        */
    /* An async health pass still running must not report to a connection
     * that is gone: the job outlives us and frees itself.                */
    if (db->health_job != NULL)
        on_db_health_abandon(db);
    g_free(db->health.detail);
    g_free(db->path);
    g_free(db);
}

/* =========================================================================
 * health: is this database structurally sound, and what file is it?
 * ========================================================================= */

/* health_record() — write one pass's verdict onto the connection.          */
static void
health_record(OnDatabase *db, gboolean ok, gboolean ran, gchar *detail)
{
    g_free(db->health.detail);
    db->health.ok      = ok;
    db->health.ran     = ran;
    db->health.detail  = detail;     /* handed over                         */
    db->health.when    = g_get_real_time() / G_USEC_PER_SEC;
    db->health.pending = FALSE;
}

/* ---------------------------------------------------------------------------
 * HealthJob — one async pass: the file path (copied, since the worker may
 * outlive a closed connection), the verdict it brings back, and the
 * callback.  The worker owns nothing else; `cancelled` is set by
 * on_db_close and read on the main thread only.
 * ------------------------------------------------------------------------- */
struct HealthJob {
    OnDatabase     *db;
    gchar          *path;
    OnDbHealthFunc  done;
    gpointer        data;
    gboolean        ok, ran;
    gchar          *detail;
    gboolean        cancelled;
};

/* on_db_health_abandon() — the connection is closing: the running pass
 * must not report to it.  The job outlives the connection and frees
 * itself in its own main-thread hand-back.                               */
static void
on_db_health_abandon(OnDatabase *db)
{
    ((HealthJob *)db->health_job)->cancelled = TRUE;
    db->health_job = NULL;
}

/* health_job_done() — on the main thread (on_main_post): record the
 * verdict (unless the connection was closed meanwhile) and tell the
 * caller; free the job.                                                   */
static void
health_job_done(void *user_data)
{
    HealthJob *job = user_data;
    if (!job->cancelled) {
        health_record(job->db, job->ok, job->ran, job->detail);
        job->detail = NULL;          /* handed over                         */
        job->db->health_job = NULL;
        if (job->done != NULL)
            job->done(job->db, job->data);
    }
    g_free(job->detail);
    g_free(job->path);
    g_free(job);
}

/* health_worker() — the worker (on_worker_run): the pass over a fresh
 * READ-ONLY connection of its own (shared_db_check_file: integrity_check,
 * foreign_key_check, the stamps, WAL, the cross-table invariants), then
 * the hand-back to the main thread.                                       */
static void
health_worker(void *user_data)
{
    HealthJob *job    = user_data;
    int        ran    = 0;           /* the checks executed                 */
    char      *detail = NULL;        /* the findings, one per line          */
    char      *err    = NULL;        /* why the checks did not run          */
    int        rc     = shared_db_check_file(job->path, 1, &ran, &detail, &err);
    job->ok  = rc == 0;
    job->ran = ran != 0;
    if (rc == 1)
        job->detail = g_strdup(detail);
    else if (rc < 0)
        job->detail = g_strdup_printf("cannot check %s: %s", job->path,
                                      err != NULL ? err : "unknown error");
    free(detail);
    free(err);
    on_main_post(health_job_done, job);
}

void
on_db_health_check_async(OnDatabase *db, OnDbHealthFunc done, gpointer data)
{
    if (db->health_job != NULL)
        return;                      /* one pass at a time                  */
    HealthJob *job = g_new0(HealthJob, 1);
    job->db   = db;
    job->path = g_strdup(db->path);
    job->done = done;
    job->data = data;
    db->health_job     = job;
    db->health.pending = TRUE;
    on_worker_run(health_worker, job);
}

const OnDbHealth *
on_db_health(OnDatabase *db)
{
    return (db->health.when != 0 || db->health.pending) ? &db->health : NULL;
}

/* Bytes per read when hashing.  The file is read in fixed-size chunks
 * rather than slurped: this runs against the live database, whose size is
 * the user's business and not ours to hold in memory twice.              */
#define SHA_CHUNK 65536

gchar *
on_db_file_sha256(const gchar *path)
{
    FILE *f = g_fopen(path, "rb");
    if (f == NULL)
        return NULL;

    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    guchar    *buf = g_malloc(SHA_CHUNK);
    gsize      n;                    /* bytes in the current chunk          */
    while ((n = fread(buf, 1, SHA_CHUNK, f)) > 0)
        g_checksum_update(sum, buf, (gssize)n);
    gboolean bad = ferror(f) != 0;   /* a short read that was an error      */
    g_free(buf);
    fclose(f);

    gchar *hex = bad ? NULL : g_strdup(g_checksum_get_string(sum));
    g_checksum_free(sum);
    return hex;
}

/* =========================================================================
 * folders
 * ========================================================================= */

gint64
on_db_folder_create(OnDatabase *db, gint64 parent_id, const gchar *name,
                    gint ai_mode, const gchar *emoji)
{
    /* ai_mode and emoji ride the INSERT: setting them afterwards meant
     * three separate statements (and fsyncs) to create one folder.          */
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO folders (parent_id, name, ai_mode, emoji, sort_order) "
        "VALUES (?, ?, ?, ?, "
        "  COALESCE((SELECT MAX(sort_order)+1 FROM folders "
        "            WHERE parent_id IS ?), 0))");
    if (stmt == NULL)
        return 0;

    bind_id_or_null(stmt, 1, parent_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, ai_mode);
    sqlite3_bind_text(stmt, 4, emoji != NULL ? emoji : "", -1,
                      SQLITE_TRANSIENT);
    bind_id_or_null(stmt, 5, parent_id);

    gint64 new_id = 0;                   /* id of the inserted row          */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: folder_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

gint
on_db_folder_get_ai_mode(OnDatabase *db, gint64 id)
{
    return (gint)query_int64(db,
        "SELECT ai_mode FROM folders WHERE id=?", id);
}

gchar *
on_db_folder_get_emoji(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COALESCE(emoji,'') FROM folders WHERE id=?");
    if (stmt == NULL)
        return g_strdup("");
    sqlite3_bind_int64(stmt, 1, id);
    gchar *emoji = NULL;              /* result, empty when no row / null    */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        emoji = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return emoji != NULL ? emoji : g_strdup("");
}

gboolean
on_db_folder_update(OnDatabase *db, gint64 id, const gchar *name,
                    gint ai_mode, const gchar *emoji)
{
    /* The Info dialog can change all three at once; one UPDATE instead of a
     * rename plus two setters.                                              */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET name=?, ai_mode=?, emoji=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, ai_mode);
        sqlite3_bind_text(stmt, 3, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, id);
    }
    return stmt_done(db, stmt);
}

/* folder_parent_of() — COALESCE(parent_id,0) of folder `id`, or 0 when
 * the row doesn't exist (the walk in on_db_folder_move just stops).         */
static gint64
folder_parent_of(OnDatabase *db, gint64 id)
{
    return query_int64(db,
        "SELECT COALESCE(parent_id,0) FROM folders WHERE id=?", id);
}

gboolean
on_db_folder_move(OnDatabase *db, gint64 id, gint64 parent_id)
{
    if (id == 0 || parent_id == id)
        return FALSE;

    /* Cycle guard: walk up from the target parent — if the chain passes
     * through the folder being moved, the move would detach a subtree
     * into itself.  The depth cap only matters if the table already
     * holds a corrupt cycle; it turns a hang into a refusal.                */
    gint64 p = parent_id;                /* ancestor cursor                 */
    for (gint depth = 0; p != 0; depth++) {
        if (p == id || depth > ON_DB_FOLDER_CYCLE_DEPTH_LIMIT)
            return FALSE;
        p = folder_parent_of(db, p);
    }

    /* Re-parent, appending after the new parent's existing children
     * (the moved folder is excluded from the MAX in case it is already
     * there).  trashed=0 makes drag-out-of-Trash a restore-to-location,
     * mirroring on_db_notes_move.                                           */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET parent_id=?, trashed=0, sort_order="
        "  COALESCE((SELECT MAX(sort_order)+1 FROM folders "
        "            WHERE parent_id IS ? AND id<>?), 0) "
        "WHERE id=?");
    if (stmt != NULL) {
        bind_id_or_null(stmt, 1, parent_id);
        bind_id_or_null(stmt, 2, parent_id);
        sqlite3_bind_int64(stmt, 3, id);
        sqlite3_bind_int64(stmt, 4, id);
    }
    return stmt_done(db, stmt);
}

/* ---------------------------------------------------------------------------
 * BULK ID OPERATIONS
 *
 * Four callers (delete, trash, move, reorder) all need the same shape: one
 * transaction around a statement stepped once per id, rolled back whole on
 * the first failure.  Only the binding differs, so that is the callback.
 *
 * The transaction matters for more than atomicity: in autocommit mode SQLite
 * fsyncs per statement, which froze the GUI on a big multi-note drop.
 * ------------------------------------------------------------------------- */

/* BulkBind — bind one row's parameters.  `i` is the row's index in `ids`
 * (reorder uses it as the sort_order), `id` the row id, `data` the caller's
 * context (the destination folder for a move, NULL otherwise).              */
typedef void (*BulkBind)(sqlite3_stmt *stmt, gsize i, gint64 id,
                         gpointer data);

/* ---------------------------------------------------------------------------
 * bulk_ids() — run `sql` once per id inside one write transaction
 * (txn_begin: the caller's, or its own).
 *   sql       — the statement to step per id.
 *   ids / n   — the rows to act on.
 *   bind      — binds one row's parameters.
 *   data      — passed through to `bind`.
 *   after_sql — a parameterless statement to run inside the SAME transaction
 *               once every row succeeded (the orphan-tag prune), or NULL.
 * Returns TRUE when everything committed.  n == 0 is a successful no-op that
 * opens no transaction at all.
 * ------------------------------------------------------------------------- */
static gboolean
bulk_ids(OnDatabase *db, const char *sql,
         const gint64 *ids, gsize n, BulkBind bind, gpointer data,
         const char *after_sql)
{
    if (n == 0)
        return TRUE;
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return FALSE;

    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt == NULL)
        return txn_end(db, own, FALSE);

    gboolean ok = TRUE;                  /* set FALSE on first failure      */
    for (gsize i = 0; i < n && ok; i++) {
        bind(stmt, i, ids[i], data);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    if (!ok)
        g_warning("db: bulk statement failed: %s",
                  sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);

    if (ok && after_sql != NULL)
        ok = exec_simple(db, after_sql);
    return txn_end(db, own, ok);
}

/* bind_id_only() — BulkBind for statements whose only parameter is the id.  */
static void
bind_id_only(sqlite3_stmt *stmt, gsize i, gint64 id, gpointer data)
{
    (void)i; (void)data;
    sqlite3_bind_int64(stmt, 1, id);
}

/* bind_order_id() — BulkBind for the reorder UPDATEs: sort_order = the row's
 * index in the caller's array, then the id.                                 */
static void
bind_order_id(sqlite3_stmt *stmt, gsize i, gint64 id, gpointer data)
{
    (void)data;
    sqlite3_bind_int(stmt, 1, (int)i);
    sqlite3_bind_int64(stmt, 2, id);
}

/* ---------------------------------------------------------------------------
 * reorder_rows() — persist an explicit row ordering in one transaction.
 *   sql — an UPDATE with two placeholders: sort_order, then row id.
 *   ids — row ids in the desired order (sort_order = array index).
 *   n   — number of ids.
 * Body of on_db_folder_reorder (once shared with a note reorder the GUI
 * no longer offers: the notes list is always sorted).  Returns TRUE
 * when every update succeeded (failure rolls the batch back).
 * ------------------------------------------------------------------------- */
static gboolean
reorder_rows(OnDatabase *db, const char *sql, const gint64 *ids, gsize n)
{
    return bulk_ids(db, sql, ids, n, bind_order_id, NULL, NULL);
}

gboolean
on_db_folder_reorder(OnDatabase *db, const gint64 *folder_ids, gsize n)
{
    return reorder_rows(db, "UPDATE folders SET sort_order=? WHERE id=?",
                        folder_ids, n);
}

/* folder_name_cmp() — GCompareFunc ordering two OnFolder* by casefolded
 * name, the comparison on_db_folder_sort_children() persists.              */
static gint
folder_name_cmp(gconstpointer a, gconstpointer b)
{
    const OnFolder *fa = a, *fb = b;
    gchar *ca = g_utf8_casefold(fa->name != NULL ? fa->name : "", -1);
    gchar *cb = g_utf8_casefold(fb->name != NULL ? fb->name : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca);
    g_free(cb);
    return result;
}

gint
on_db_folder_sort_children(OnDatabase *db, gint64 parent_id)
{
    GList *kids = on_db_folder_list(db, parent_id);
    kids = g_list_sort(kids, folder_name_cmp);

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (GList *l = kids; l != NULL; l = l->next)
        g_array_append_val(ids, ((OnFolder *)l->data)->id);
    on_db_folder_list_free(kids);

    /* Nothing to persist for an empty or single-child folder.              */
    gint n = (gint)ids->len;
    gboolean ok = (ids->len < 2) ||
                  on_db_folder_reorder(db, (const gint64 *)ids->data,
                                       ids->len);
    g_array_free(ids, TRUE);
    return ok ? n : -1;
}

gboolean
on_db_folder_delete(OnDatabase *db, gint64 id)
{
    return bulk_ids(db, "DELETE FROM folders WHERE id=?", &id, 1,
                    bind_id_only, NULL, PRUNE_ORPHAN_TAGS_SQL);
}

/* Column list shared by every folder query, matching run_folder_query()'s
 * expectations.  OnFolder has no parent/sort_order fields, so neither is
 * selected — ORDER BY sort_order works without it being in the result.      */
#define FOLDER_COLS "id, name, COALESCE(emoji,'')"

/* run_folder_query() — collect every row of `stmt` (columns: FOLDER_COLS)
 * into a list of OnFolder* and finalize it.                                 */
static GList *
run_folder_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnFolder* rows      */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnFolder *f  = g_new0(OnFolder, 1);
        f->id    = sqlite3_column_int64(stmt, 0);
        f->name  = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        f->emoji = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        if (f->emoji == NULL)
            f->emoji = g_strdup("");
        out = g_list_prepend(out, f);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_folder_list(OnDatabase *db, gint64 parent_id)
{
    /* trashed=0: a directly-trashed folder disappears from the normal
     * tree (it lives under the Trash section); its untouched descendants
     * never get listed because nothing recurses into it.                    */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " FOLDER_COLS " FROM folders "
        "WHERE parent_id IS ? AND trashed=0 "
        "ORDER BY sort_order, name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    bind_id_or_null(stmt, 1, parent_id);
    return run_folder_query(stmt);
}

/* free_folder() — GDestroyNotify for one OnFolder.                          */
static void
free_folder(gpointer data)
{
    OnFolder *f = data;
    g_free(f->name);
    g_free(f->emoji);
    g_free(f);
}

void
on_db_folder_list_free(GList *folders)
{
    g_list_free_full(folders, free_folder);
}

/* free_folder_list() — GDestroyNotify for on_db_folder_child_map values.    */
static void
free_folder_list(gpointer data)
{
    g_list_free_full(data, free_folder);
}

GHashTable *
on_db_folder_child_map(OnDatabase *db)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, free_folder_list);

    /* Same filter and ordering as on_db_folder_list(), so a tree built by
     * walking this map comes out row-for-row identical to the recursive
     * one-query-per-folder walk it replaces.                                */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " FOLDER_COLS ", COALESCE(parent_id,0) FROM folders "
        "WHERE trashed=0 "
        "ORDER BY sort_order, name COLLATE NOCASE");
    if (stmt == NULL)
        return map;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnFolder *f  = g_new0(OnFolder, 1);
        f->id    = sqlite3_column_int64(stmt, 0);
        f->name  = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        f->emoji = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        gint64 parent = sqlite3_column_int64(stmt, 3);   /* 0 = top level   */

        /* Rows arrive in display order, so appending keeps each sibling
         * list ordered (walking to the tail is free at sibling counts).
         * g_list_append returns the unchanged head of a non-empty list, so
         * only the FIRST child of a parent has to be stored in the map —
         * re-inserting later would make the table free the list we are
         * still building.                                                   */
        GList *kids = g_hash_table_lookup(map, &parent);
        GList *head = g_list_append(kids, f);
        if (kids == NULL) {
            gint64 *key = g_new(gint64, 1);
            *key = parent;
            g_hash_table_insert(map, key, head);
        }
    }
    sqlite3_finalize(stmt);
    return map;
}

GList *
on_db_folder_children(GHashTable *map, gint64 parent_id)
{
    return g_hash_table_lookup(map, &parent_id);
}

void
on_db_folder_child_map_free(GHashTable *map)
{
    if (map != NULL)
        g_hash_table_destroy(map);
}

/* =========================================================================
 * notes
 * ========================================================================= */

gint64
on_db_note_create(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO notes (folder_id, title, sort_order, created_at, "
        "                   updated_at) "
        "VALUES (?, '" ON_DEFAULT_NOTE_TITLE "', "
        "  COALESCE((SELECT MAX(sort_order)+1 FROM notes "
        "            WHERE folder_id IS ?), 0), "
        "  strftime('%s','now'), strftime('%s','now'))");
    if (stmt == NULL)
        return 0;
    bind_id_or_null(stmt, 1, folder_id);
    bind_id_or_null(stmt, 2, folder_id);

    gint64 new_id = 0;                   /* id of the inserted note         */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: note_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

gboolean
on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction for the lot — autocommit would fsync per note — and
     * the orphan-tag prune runs once at the end instead of per note.        */
    return bulk_ids(db, "DELETE FROM notes WHERE id=?",
                    ids, n, bind_id_only, NULL, PRUNE_ORPHAN_TAGS_SQL);
}

/* bind_move() — BulkBind for on_db_notes_move: the destination folder twice
 * (column and MAX subselect), then the note id.  `data` points at the
 * destination folder id.                                                    */
static void
bind_move(sqlite3_stmt *stmt, gsize i, gint64 id, gpointer data)
{
    (void)i;
    gint64 folder_id = *(const gint64 *)data;
    bind_id_or_null(stmt, 1, folder_id);
    bind_id_or_null(stmt, 2, folder_id);
    sqlite3_bind_int64(stmt, 3, id);
}

gboolean
on_db_notes_move(OnDatabase *db, const gint64 *note_ids, gsize n,
                 gint64 folder_id)
{
    /* trashed=0: dragging a note out of the Trash into a folder is a
     * restore — a moved note is always meant to be visible where it
     * lands.  The MAX subselect re-evaluates per row inside the
     * transaction, so the notes land appended in array order.               */
    return bulk_ids(db,
                    "UPDATE notes SET folder_id=?, trashed=0, sort_order="
                    "  COALESCE((SELECT MAX(sort_order)+1 FROM notes "
                    "            WHERE folder_id IS ?), 0) "
                    "WHERE id=?",
                    note_ids, n, bind_move, &folder_id, NULL);
}

gboolean
on_db_note_save(OnDatabase *db, gint64 id, const gchar *title,
                const guint8 *content, gsize len, const gchar *body_text)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE notes SET title=?, content=?, body_text=?, "
        "updated_at=strftime('%s','now') WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
        if (content != NULL && len > 0)
            sqlite3_bind_blob(stmt, 2, content, (int)len, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 2);
        if (body_text != NULL)
            sqlite3_bind_text(stmt, 3, body_text, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 3);
        sqlite3_bind_int64(stmt, 4, id);
    }
    /* No row changed means the note is gone (deleted behind an open
     * editor): that is a failed save, never a silent success.             */
    return cached_stmt_done(db, stmt) && sqlite3_changes(db->handle) == 1;
}

gchar *
on_db_note_body_text(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT body_text FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    gchar *text = NULL;              /* cached text, NULL if unfilled       */
    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        text = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return text;
}

gboolean
on_db_note_set_body_text(OnDatabase *db, gint64 id, const gchar *body_text)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET body_text=? WHERE id=? AND body_text IS NULL");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, body_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

gboolean
on_db_note_set_updated_at(OnDatabase *db, gint64 id, gint64 ts)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET updated_at=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, ts);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

gboolean
on_db_note_load(OnDatabase *db, gint64 id, guint8 **out, gsize *out_len)
{
    *out     = NULL;
    *out_len = 0;
    sqlite3_stmt *stmt = prepare(db, "SELECT content FROM notes WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);

    int rc = sqlite3_step(stmt);         /* ROW, DONE (no such note), error */
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int         n    = sqlite3_column_bytes(stmt, 0);
        if (blob != NULL && n > 0) {
            *out     = g_memdup2(blob, (gsize)n);
            *out_len = (gsize)n;
        }
    } else if (rc != SQLITE_DONE) {
        g_warning("db: cannot read note %" G_GINT64_FORMAT ": %s",
                  id, sqlite3_errmsg(db->handle));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
}

/* Column list shared by every note-metadata query, matching
 * meta_from_row()'s expectations.                                           */
#define NOTE_META_COLS \
    "id, COALESCE(folder_id,0), title, sort_order, updated_at, pinned, " \
    "created_at"

/* ---------------------------------------------------------------------------
 * meta_from_row() — build one OnNoteMeta from the current result row of a
 * statement selecting NOTE_META_COLS in that order.
 * ------------------------------------------------------------------------- */
static OnNoteMeta *
meta_from_row(sqlite3_stmt *stmt)
{
    OnNoteMeta *m = g_new0(OnNoteMeta, 1);
    m->id         = sqlite3_column_int64(stmt, 0);
    m->folder_id  = sqlite3_column_int64(stmt, 1);
    m->title      = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
    m->updated_at = sqlite3_column_int64(stmt, 4);
    m->pinned     = sqlite3_column_int(stmt, 5) != 0;
    m->created_at = sqlite3_column_int64(stmt, 6);
    return m;
}

OnNoteMeta *
on_db_note_get(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);

    OnNoteMeta *meta = NULL;             /* result, NULL if no such row     */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        meta = meta_from_row(stmt);
    sqlite3_finalize(stmt);
    return meta;
}

/* ---------------------------------------------------------------------------
 * collect_meta() — drain `stmt` (columns = NOTE_META_COLS) into a list of
 * OnNoteMeta*, disposing of the statement by ownership: a cached one is
 * reset for reuse, a one-off is finalized.  The shared body of the four
 * meta-query entry points below.
 * ------------------------------------------------------------------------- */
static GList *
collect_meta(sqlite3_stmt *stmt, gboolean cached)
{
    GList *out = NULL;                   /* accumulated OnNoteMeta* rows    */
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out = g_list_prepend(out, meta_from_row(stmt));
    if (cached) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    } else {
        sqlite3_finalize(stmt);
    }
    return g_list_reverse(out);
}

/* run_meta_query() — collect a one-off stmt's rows and finalize it.         */
static GList *
run_meta_query(sqlite3_stmt *stmt)
{
    return collect_meta(stmt, FALSE);
}

/* run_meta_query_cached() — collect a cached stmt's rows and reset it.      */
static GList *
run_meta_query_cached(sqlite3_stmt *stmt)
{
    return collect_meta(stmt, TRUE);
}

/* meta_query() — prepare a parameterless note-metadata SELECT (columns =
 * NOTE_META_COLS) and collect its rows.  Returns a GList of OnNoteMeta*
 * (free with on_db_note_list_free), or NULL on a prepare failure.            */
static GList *
meta_query(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = prepare(db, sql);
    return (stmt != NULL) ? run_meta_query(stmt) : NULL;
}

/* meta_query_cached() — look up or compile a parameterless note-metadata
 * SELECT and collect its rows via run_meta_query_cached().                   */
static GList *
meta_query_cached(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = cached_prepare(db, sql);
    return (stmt != NULL) ? run_meta_query_cached(stmt) : NULL;
}

GList *
on_db_note_list(OnDatabase *db, gint64 folder_id)
{
    /* trashed=0 keeps directly-trashed notes out; when the folder itself
     * sits in the Trash (browsing it from the Trash section) its regular
     * notes carry no flag and still list here.                              */
    sqlite3_stmt *stmt = cached_prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE folder_id IS ? AND trashed=0 "
        "ORDER BY sort_order, updated_at DESC");
    if (stmt == NULL)
        return NULL;
    bind_id_or_null(stmt, 1, folder_id);
    return run_meta_query_cached(stmt);
}

GList *
on_db_note_list_all(OnDatabase *db, gboolean include_trash)
{
    return meta_query(db, include_trash
        ? "SELECT " NOTE_META_COLS " "
          "FROM notes ORDER BY folder_id, sort_order"
        : "SELECT " NOTE_META_COLS " "
          "FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL " "
          "ORDER BY folder_id, sort_order");
}

GList *
on_db_note_list_recent(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
}

gboolean
on_db_note_set_pinned(OnDatabase *db, gint64 id, gboolean pinned)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET pinned=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

GList *
on_db_note_list_pinned(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE pinned=1 AND " SHARED_DB_NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
}

gint
on_db_note_count_pinned(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE pinned=1 AND " SHARED_DB_NOTE_VISIBLE_SQL,
        -1);
}

void
on_db_note_meta_free(OnNoteMeta *meta)
{
    if (meta == NULL)
        return;
    g_free(meta->title);
    g_free(meta);
}

void
on_db_note_list_free(GList *notes)
{
    g_list_free_full(notes, (GDestroyNotify)on_db_note_meta_free);
}

/* =========================================================================
 * tags
 * ========================================================================= */

/* tag_find() — look up tag `name` WITHOUT creating it.  Returns its id, or
 * 0.                                                                       */
static gint64
tag_find(OnDatabase *db, const gchar *name)
{
    sqlite3_stmt *stmt = prepare(db, "SELECT id FROM tags WHERE name=?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    gint64 tag_id = 0;               /* found id, or 0                      */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        tag_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return tag_id;
}

/* on_db_tag_get_or_create() — look up tag `name`, creating it if
 * missing.  Returns the tag id, or 0 on failure.                            */
static gint64
on_db_tag_get_or_create(OnDatabase *db, const gchar *name)
{
    /* Try the fast path first: the tag already exists.                     */
    gint64 tag_id = tag_find(db, name);   /* resulting tag id         */
    if (tag_id != 0)
        return tag_id;

    /* Not found: insert it.                                                */
    sqlite3_stmt *stmt = prepare(db, "INSERT INTO tags (name) VALUES (?)");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_DONE)
        tag_id = sqlite3_last_insert_rowid(db->handle);
    sqlite3_finalize(stmt);
    return tag_id;
}

/* run_tag_query() — step a prepared statement whose result columns are
 * (id, name) and collect the rows as OnTag structs.  Finalizes `stmt`.
 * Returns a GList of OnTag*; free with on_db_tag_list_free().               */
static GList *
run_tag_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnTag* rows         */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnTag *t = g_new0(OnTag, 1);
        t->id   = sqlite3_column_int64(stmt, 0);
        t->name = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        out = g_list_prepend(out, t);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_tag_list(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, name FROM tags ORDER BY name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    return run_tag_query(stmt);
}

/* free_tag() — GDestroyNotify for one OnTag.                                */
static void
free_tag(gpointer data)
{
    OnTag *t = data;
    g_free(t->name);
    g_free(t);
}

void
on_db_tag_list_free(GList *tags)
{
    g_list_free_full(tags, free_tag);
}

gboolean
on_db_note_set_tags(OnDatabase *db, gint64 note_id, GList *tag_names)
{
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return FALSE;

    /* Drop the note's old tag links, then re-add the current set.          */
    sqlite3_stmt *stmt = prepare(db,
        "DELETE FROM note_tags WHERE note_id=?");
    gboolean ok = stmt != NULL;          /* overall success flag            */
    if (ok) {
        sqlite3_bind_int64(stmt, 1, note_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    for (GList *l = tag_names; ok && l != NULL; l = l->next) {
        const gchar *name = l->data;     /* one tag name, no leading '#'    */
        gint64 tag_id = on_db_tag_get_or_create(db, name);
        if (tag_id == 0) {
            ok = FALSE;
            break;
        }
        stmt = prepare(db,
            "INSERT OR IGNORE INTO note_tags (note_id, tag_id) VALUES (?,?)");
        if (stmt == NULL) {
            ok = FALSE;
            break;
        }
        sqlite3_bind_int64(stmt, 1, note_id);
        sqlite3_bind_int64(stmt, 2, tag_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    /* Remove tags that no longer label any note so the tag list in the
     * library window stays tidy.                                           */
    if (ok)
        ok = exec_simple(db, PRUNE_ORPHAN_TAGS_SQL);

    return txn_end(db, own, ok);
}

GList *
on_db_notes_by_tag(OnDatabase *db, gint64 tag_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT n.id, COALESCE(n.folder_id,0), n.title, n.sort_order, "
        "       n.updated_at, n.pinned, n.created_at "
        "FROM notes n JOIN note_tags nt ON nt.note_id = n.id "
        "WHERE nt.tag_id=? AND n.trashed=0 AND (n.folder_id IS NULL OR "
        "      n.folder_id NOT IN (SELECT id FROM trash_folder_ids)) "
        "ORDER BY n.updated_at DESC");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, tag_id);
    return run_meta_query(stmt);
}

/* =========================================================================
 * action items — a queryable mirror of the '!' lines in note content,
 * rebuilt whenever a save changes them (see on_note_extract_actions)
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * action_uid_alloc() — take the next stable uid from the action_uid_seq
 * high-water mark and advance it.  MUST run inside the caller's
 * transaction so a rolled-back rebuild does not burn ids (and, more
 * importantly, so two allocations can never collide).  The counter is
 * only ever incremented, so a uid retired with a deleted item is never
 * handed out to a different item.
 * Returns the new uid, or 0 on failure.
 * ------------------------------------------------------------------------- */
static gint64
action_uid_alloc(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT next FROM action_uid_seq WHERE id=1");
    if (stmt == NULL)
        return 0;
    gint64 uid = 0;                      /* the id to hand out              */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        uid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (uid <= 0)
        return 0;

    stmt = prepare(db, "UPDATE action_uid_seq SET next=? WHERE id=1");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_int64(stmt, 1, uid + 1);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? uid : 0;
}

gint64
on_db_action_uid_alloc(OnDatabase *db)
{
    /* Outside a transaction the read-then-write of the high-water mark is
     * wrapped in one (IMMEDIATE: the write lock from the start), so two
     * allocations cannot read the same `next`.                            */
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return 0;
    gint64 uid = action_uid_alloc(db);
    return txn_end(db, own, uid != 0) ? uid : 0;
}

/* ---------------------------------------------------------------------------
 * task_now() — the Tasks app's timestamp: UNIX seconds.
 * ------------------------------------------------------------------------- */
static gint64
task_now(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---------------------------------------------------------------------------
 * task_list_managed() — the id of the managed list every Notes-born task
 * is filed in: the live list named SHARED_DB_ACTION_LIST_NAME with the
 * emoji SHARED_DB_ACTION_LIST_EMOJI, the lowest id when there are several;
 * created (appended after every other list) when there is none.  Never
 * seeded blind: a database that holds no item holds no list either.
 * Returns the list id, or 0 on failure.
 * ------------------------------------------------------------------------- */
static gint64
task_list_managed(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id FROM lists WHERE name=? AND emoji=? AND deleted=0"
        " ORDER BY id LIMIT 1");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, SHARED_DB_ACTION_LIST_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, SHARED_DB_ACTION_LIST_EMOJI, -1, SQLITE_STATIC);
    gint64 id = sqlite3_step(stmt) == SQLITE_ROW
                ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    if (id != 0)
        return id;

    stmt = prepare(db,
        "INSERT INTO lists (name, emoji, position, updated_at) VALUES (?, ?,"
        "  (SELECT COALESCE(MAX(position), 0) + 1 FROM lists), ?)");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, SHARED_DB_ACTION_LIST_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, SHARED_DB_ACTION_LIST_EMOJI, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, task_now());
    if (!stmt_done(db, stmt))
        return 0;
    return sqlite3_last_insert_rowid(db->handle);
}

/* ---------------------------------------------------------------------------
 * task_push() — write the fields in `mask` (ON_ACTION_FIELD_*) of a '!'
 * line onto task `task_id`, and only those: THE one statement Notes
 * changes a task with, so a field Tasks changed meanwhile is never written
 * back.  Done folds into status the way a done-only source must: done →
 * SHARED_DB_TASK_STATUS_DONE, stamping completed_at on entry; not done →
 * SHARED_DB_SQL_UNTICK_STATUS (IN_PROGRESS only if it was DONE, else
 * unchanged: New and In Progress are never pushed), a second statement in
 * the same transaction.  updated_at is stamped.  Every other column is the
 * Tasks app's alone.  Every CASE reads the row as it was, so completed_at
 * sees the old status.
 * Returns TRUE on success (a mask of 0 writes nothing).
 * ------------------------------------------------------------------------- */
static gboolean
task_push(OnDatabase *db, gint64 task_id, guint mask, const gchar *title,
          gboolean done, gint64 due)
{
    if (mask == 0)
        return TRUE;
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE tasks SET"
        "  title = CASE WHEN ?1 THEN ?2 ELSE title END,"
        "  due   = CASE WHEN ?3 THEN ?4 ELSE due END,"
        "  completed_at = CASE WHEN ?5 AND ?6 = 1 AND status <> "
                               SHARED_DB_STR(SHARED_DB_TASK_STATUS_DONE) " THEN ?7"
        "                      ELSE completed_at END,"
        "  status = CASE WHEN ?5 AND ?6 = 1 THEN "
                         SHARED_DB_STR(SHARED_DB_TASK_STATUS_DONE)
        "                ELSE status END,"
        "  updated_at = ?7"
        " WHERE id = ?8");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int(stmt, 1, (mask & ON_ACTION_FIELD_TEXT) != 0);
    sqlite3_bind_text(stmt, 2, title != NULL ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (mask & ON_ACTION_FIELD_DUE) != 0);
    sqlite3_bind_int64(stmt, 4, due);
    sqlite3_bind_int(stmt, 5, (mask & ON_ACTION_FIELD_DONE) != 0);
    sqlite3_bind_int(stmt, 6, done ? 1 : 0);
    sqlite3_bind_int64(stmt, 7, task_now());
    sqlite3_bind_int64(stmt, 8, task_id);
    if (!cached_stmt_done(db, stmt))
        return FALSE;
    if ((mask & ON_ACTION_FIELD_DONE) == 0 || done)
        return TRUE;
    stmt = cached_prepare(db,
        "UPDATE tasks SET " SHARED_DB_SQL_UNTICK_STATUS " WHERE id = ?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, task_id);
    return cached_stmt_done(db, stmt);
}

/* ---------------------------------------------------------------------------
 * task_tombstone() — Notes removed the line of task `task_id`: tombstone
 * it AND its subtasks, in one statement with one updated_at — the rule
 * the note_task_tombstone trigger applies when a link row is deleted:
 *   UPDATE tasks SET deleted = 1, updated_at = now
 *    WHERE (id = X OR parent_id = X) AND deleted = 0
 * (a subtask deleted before keeps its own updated_at, so task_revive can
 * tell the two apart).
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
task_tombstone(OnDatabase *db, gint64 task_id)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE tasks SET deleted = 1, updated_at = ?1"
        " WHERE (id = ?2 OR parent_id = ?2) AND deleted = 0");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, task_now());
    sqlite3_bind_int64(stmt, 2, task_id);
    return cached_stmt_done(db, stmt);
}

/* ---------------------------------------------------------------------------
 * task_revive() — an undo brought back the line of task `task_id`, which
 * Notes had tombstoned (task_tombstone): revive it and the subtasks that
 * were tombstoned in that same operation — those still deleted whose
 * updated_at is the parent's tombstone updated_at — in one statement:
 *   UPDATE tasks SET deleted = 0, updated_at = now
 *    WHERE id = X OR (parent_id = X AND deleted = 1 AND updated_at = T)
 * with T the parent's updated_at read first.  A subtask deleted on its
 * own (another updated_at) stays deleted.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
task_revive(OnDatabase *db, gint64 task_id)
{
    gint64 tombstoned_at = query_int64(db,
        "SELECT updated_at FROM tasks WHERE id = ?", task_id);
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE tasks SET deleted = 0, updated_at = ?1"
        " WHERE id = ?2 OR (parent_id = ?2 AND deleted = 1 AND updated_at = ?3)");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, task_now());
    sqlite3_bind_int64(stmt, 2, task_id);
    sqlite3_bind_int64(stmt, 3, tombstoned_at);
    return cached_stmt_done(db, stmt);
}

/* ---------------------------------------------------------------------------
 * task_create() — a new task for a '!' line: appended to the managed list
 * as a top-level task, with the line's title, due date and done state
 * (status DONE and completed_at now, else New) and every other column at
 * the schema's default; then its note_task link, the seen_* baseline set
 * to the line's values.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
task_create(OnDatabase *db, const OnActionItem *it, gint64 note_id, gint ord)
{
    gint64 list_id = task_list_managed(db);
    if (list_id == 0)
        return FALSE;
    gint64 now = task_now();
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO tasks (list_id, parent_id, title, due, status,"
        "                   completed_at, position, updated_at)"
        " VALUES (?1, NULL, ?2, ?3, ?4, ?5,"
        "   (SELECT COALESCE(MAX(position), 0) + 1 FROM tasks"
        "     WHERE list_id = ?1 AND parent_id IS NULL), ?6)");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, list_id);
    sqlite3_bind_text(stmt, 2, it->text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, it->due);
    sqlite3_bind_int(stmt, 4, it->done ? SHARED_DB_TASK_STATUS_DONE : SHARED_DB_TASK_STATUS_NEW);
    sqlite3_bind_int64(stmt, 5, it->done ? now : 0);
    sqlite3_bind_int64(stmt, 6, now);
    if (!stmt_done(db, stmt))
        return FALSE;
    gint64 task_id = sqlite3_last_insert_rowid(db->handle);

    stmt = prepare(db,
        "INSERT INTO note_task (uid, task_id, note_id, ord, seen_title,"
        "                       seen_done, seen_due, seen_deleted)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, 0)");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, it->uid);
    sqlite3_bind_int64(stmt, 2, task_id);
    sqlite3_bind_int64(stmt, 3, note_id);
    sqlite3_bind_int(stmt, 4, ord);
    sqlite3_bind_text(stmt, 5, it->text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, it->done ? 1 : 0);
    sqlite3_bind_int64(stmt, 7, it->due);
    return stmt_done(db, stmt);
}

/* ---------------------------------------------------------------------------
 * link_from_row() — fill an OnActionLink from the current result row of a
 * statement selecting LINK_COLS (below), in that order; its strings are
 * new (on_db_action_link_clear).  Returns TRUE.
 * ------------------------------------------------------------------------- */
static gboolean
link_from_row(sqlite3_stmt *stmt, OnActionLink *row)
{
    row->uid          = sqlite3_column_int64(stmt, 0);
    row->task_id      = sqlite3_column_int64(stmt, 1);
    row->note_id      = sqlite3_column_int64(stmt, 2);
    row->ord          = sqlite3_column_int(stmt, 3);
    row->seen_title   = g_strdup((const gchar *)sqlite3_column_text(stmt, 4));
    row->seen_done    = sqlite3_column_int(stmt, 5) != 0;
    row->seen_due     = sqlite3_column_int64(stmt, 6);
    row->seen_deleted = sqlite3_column_int(stmt, 7) != 0;
    row->title        = g_strdup((const gchar *)sqlite3_column_text(stmt, 8));
    row->done         = sqlite3_column_int(stmt, 9) == SHARED_DB_TASK_STATUS_DONE;
    row->due          = sqlite3_column_int64(stmt, 10);
    row->deleted      = sqlite3_column_int(stmt, 11) != 0;
    return TRUE;
}

/* The columns link_from_row reads, in its order.                            */
#define LINK_COLS \
    "nt.uid, nt.task_id, nt.note_id, nt.ord, nt.seen_title, nt.seen_done," \
    " nt.seen_due, nt.seen_deleted, t.title, t.status, t.due, t.deleted"

/* link_get() — the note_task row of `uid` and its task, whatever note it is
 * linked to.  Returns FALSE when there is none; the row's strings are
 * owned (on_db_action_link_clear).                                          */
static gboolean
link_get(OnDatabase *db, gint64 uid, OnActionLink *row)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "SELECT " LINK_COLS " FROM note_task nt JOIN tasks t ON t.id = nt.task_id"
        " WHERE nt.uid = ?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, uid);
    gboolean found = sqlite3_step(stmt) == SQLITE_ROW &&
                     link_from_row(stmt, row);
    sqlite3_reset(stmt);
    return found;
}

void
on_db_action_link_clear(OnActionLink *row)
{
    g_free(row->seen_title);
    g_free(row->title);
    row->seen_title = NULL;
    row->title      = NULL;
}

GArray *
on_db_action_links(OnDatabase *db, gint64 note_id)
{
    GArray *rows = g_array_new(FALSE, TRUE, sizeof(OnActionLink));
    g_array_set_clear_func(rows, (GDestroyNotify)on_db_action_link_clear);
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " LINK_COLS " FROM note_task nt JOIN tasks t ON t.id = nt.task_id"
        " WHERE nt.note_id = ? ORDER BY nt.ord");
    if (stmt == NULL)
        return rows;
    sqlite3_bind_int64(stmt, 1, note_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnActionLink row;
        link_from_row(stmt, &row);
        g_array_append_val(rows, row);
    }
    sqlite3_finalize(stmt);
    return rows;
}

/* ---------------------------------------------------------------------------
 * link_update() — write the note_task row of `uid`: placement and the
 * whole seen_* baseline (the caller passes the values it agreed on).
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
link_update(OnDatabase *db, const OnActionLink *row)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE note_task SET note_id=?, ord=?, seen_title=?, seen_done=?,"
        " seen_due=?, seen_deleted=? WHERE uid=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, row->note_id);
    sqlite3_bind_int(stmt, 2, row->ord);
    sqlite3_bind_text(stmt, 3, row->seen_title != NULL ? row->seen_title : "",
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, row->seen_done ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, row->seen_due);
    sqlite3_bind_int(stmt, 6, row->seen_deleted ? 1 : 0);
    sqlite3_bind_int64(stmt, 7, row->uid);
    return cached_stmt_done(db, stmt);
}

gboolean
on_db_action_link_set_seen(OnDatabase *db, const OnActionLink *row)
{
    return link_update(db, row);
}

gboolean
on_db_action_link_delete(OnDatabase *db, gint64 uid)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM note_task WHERE uid=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, uid);
    return stmt_done(db, stmt);
}

guint
on_db_action_diff(const OnActionItem *it, const OnActionLink *row)
{
    guint mask = 0;                  /* the fields that differ              */
    if (g_strcmp0(it->text, row->seen_title) != 0)
        mask |= ON_ACTION_FIELD_TEXT;
    if ((it->done != 0) != (row->seen_done != 0))
        mask |= ON_ACTION_FIELD_DONE;
    if (it->due != row->seen_due)
        mask |= ON_ACTION_FIELD_DUE;
    return mask;
}

/* ---------------------------------------------------------------------------
 * push_item() — the push for one line that has a note_task row (see
 * on_db_note_save_actions in db.h for the rules).
 *   row     — the row as read (its seen_* are updated here, in memory too).
 *   it      — the line.
 *   note_id, ord — where the line is now.
 *   changed — set TRUE when a task row is written.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
push_item(OnDatabase *db, OnActionLink *row, const OnActionItem *it,
          gint64 note_id, gint ord, gboolean *changed)
{
    if (row->deleted && !row->seen_deleted)
        return TRUE;                 /* deleted elsewhere: the pull's       */
    gboolean ok   = TRUE;
    gboolean link = row->note_id != note_id || row->ord != ord;
    if (row->deleted) {              /* Notes tombstoned it; the line is back */
        ok = task_revive(db, row->task_id);
        *changed = TRUE;
    }
    if (row->seen_deleted) {
        row->seen_deleted = FALSE;
        link = TRUE;
    }
    guint mask = on_db_action_diff(it, row);
    if (ok && mask != 0) {
        ok = task_push(db, row->task_id, mask, it->text, it->done, it->due);
        *changed = TRUE;
        g_free(row->seen_title);
        row->seen_title = g_strdup(it->text);
        row->seen_done  = it->done;
        row->seen_due   = it->due;
        link = TRUE;
    }
    row->note_id = note_id;
    row->ord     = ord;
    if (ok && link)
        ok = link_update(db, row);
    return ok;
}

/* One task of a note whose line is gone (on_db_note_save_actions).       */
typedef struct {
    gint64 uid;                      /* its note_task row                   */
    gint64 task_id;                  /* the task to tombstone               */
} GoneLine;

gboolean
on_db_note_save_actions(OnDatabase *db, gint64 note_id, GList *items,
                        gboolean *changed)
{
    gboolean dummy;                  /* when the caller does not ask        */
    if (changed == NULL)
        changed = &dummy;
    *changed = FALSE;
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return FALSE;
    gboolean ok = TRUE;              /* the outcome                         */
    GHashTable *kept = g_hash_table_new(g_int64_hash, g_int64_equal);
    gint ord = 0;                    /* position among the items            */
    for (GList *l = items; l != NULL && ok; l = l->next, ord++) {
        OnActionItem *it = l->data;  /* one real action line                */
        g_hash_table_add(kept, &it->uid);
        OnActionLink row = { 0 };
        if (!link_get(db, it->uid, &row)) {
            ok = task_create(db, it, note_id, ord);
            *changed = TRUE;
            continue;
        }
        ok = push_item(db, &row, it, note_id, ord, changed);
        on_db_action_link_clear(&row);
    }

    /* Every live task of this note whose line is gone, that Notes knew as
     * live (seen_deleted = 0): tombstoned, remembered as Notes' doing.     */
    if (ok) {
        GArray *gone = g_array_new(FALSE, FALSE, sizeof(GoneLine));
        sqlite3_stmt *stmt = prepare(db,
            "SELECT nt.uid, nt.task_id FROM note_task nt"
            " JOIN tasks t ON t.id = nt.task_id"
            " WHERE nt.note_id = ? AND t.deleted = 0 AND nt.seen_deleted = 0");
        if (stmt == NULL) {
            ok = FALSE;
        } else {
            sqlite3_bind_int64(stmt, 1, note_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                GoneLine line = { sqlite3_column_int64(stmt, 0),
                                  sqlite3_column_int64(stmt, 1) };
                if (!g_hash_table_contains(kept, &line.uid))
                    g_array_append_val(gone, line);
            }
            sqlite3_finalize(stmt);
        }
        for (guint i = 0; i < gone->len && ok; i++) {
            const GoneLine *line = &g_array_index(gone, GoneLine, i);
            ok = task_tombstone(db, line->task_id);
            if (ok) {
                stmt = prepare(db,
                    "UPDATE note_task SET seen_deleted=1 WHERE uid=?");
                if (stmt != NULL)
                    sqlite3_bind_int64(stmt, 1, line->uid);
                ok = stmt_done(db, stmt);
            }
            *changed = TRUE;
        }
        g_array_unref(gone);
    }
    g_hash_table_destroy(kept);

    return txn_end(db, own, ok);
}

gboolean
on_db_action_set(OnDatabase *db, gint64 uid, guint mask, const gchar *title,
                 gboolean done, gint64 due)
{
    gint64 task_id = query_int64(db,
        "SELECT nt.task_id FROM note_task nt JOIN tasks t ON t.id = nt.task_id"
        " WHERE nt.uid = ? AND t.deleted = 0", uid);
    return task_id != 0 && task_push(db, task_id, mask, title, done, due);
}

GArray *
on_db_action_pending(OnDatabase *db, gint64 note_id)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    sqlite3_stmt *stmt = prepare(db,
        "SELECT DISTINCT nt.note_id FROM note_task nt"
        " JOIN tasks t ON t.id = nt.task_id"
        " WHERE (?1 = 0 OR nt.note_id = ?1) AND " SHARED_DB_SQL_ACTION_PENDING
        " ORDER BY nt.note_id");
    if (stmt == NULL)
        return ids;
    sqlite3_bind_int64(stmt, 1, note_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 id = sqlite3_column_int64(stmt, 0);
        g_array_append_val(ids, id);
    }
    sqlite3_finalize(stmt);
    return ids;
}

/* FNV-1a 64-bit parameters (the note stamp's hash).                       */
#define FNV64_OFFSET 0xcbf29ce484222325ULL
#define FNV64_PRIME  0x100000001b3ULL

/*
 * on_db_note_stamp — see db.h.  The hash is FNV-1a 64 over the blob bytes
 * (none for an empty note).
 */
gboolean
on_db_note_stamp(OnDatabase *db, gint64 id, OnNoteStamp *stamp)
{
    memset(stamp, 0, sizeof *stamp);
    sqlite3_stmt *stmt = cached_prepare(db,
        "SELECT updated_at, content FROM notes WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found) {
        const guchar *blob = sqlite3_column_blob(stmt, 1);
        gint64        n    = sqlite3_column_bytes(stmt, 1);
        guint64       hash = FNV64_OFFSET;
        for (gint64 i = 0; i < n; i++) {
            hash ^= blob[i];
            hash *= FNV64_PRIME;
        }
        stamp->updated_at = sqlite3_column_int64(stmt, 0);
        stamp->size       = n;
        stamp->hash       = hash;
    }
    sqlite3_reset(stmt);
    return found;
}

/*
 * on_db_note_stamp_same — see db.h.
 */
gboolean
on_db_note_stamp_same(const OnNoteStamp *a, const OnNoteStamp *b)
{
    return a->updated_at == b->updated_at && a->size == b->size &&
           a->hash == b->hash;
}

/*
 * on_db_txn_begin — see db.h.
 */
gboolean
on_db_txn_begin(OnDatabase *db, gboolean *own)
{
    return txn_begin(db, own);
}

/*
 * on_db_txn_end — see db.h.
 */
gboolean
on_db_txn_end(OnDatabase *db, gboolean own, gboolean ok)
{
    return txn_end(db, own, ok);
}

/*
 * on_db_txn_on_end — see db.h.
 */
gboolean
on_db_txn_on_end(OnDatabase *db, OnDbTxnHook hook, gpointer data)
{
    if (sqlite3_get_autocommit(db->handle) != 0)
        return FALSE;
    TxnHook h = { hook, data };
    g_array_append_val(db->txn_hooks, h);
    return TRUE;
}

/*
 * on_db_poll_revs — see db.h.  A deferred BEGIN: the first statement opens
 * the read snapshot, and PRAGMA data_version and both revs are read in it.
 */
gboolean
on_db_poll_revs(OnDatabase *db, gboolean *changed, gint64 *rev_tasks,
                gint64 *rev_notes)
{
    if (sqlite3_get_autocommit(db->handle) == 0 || !exec_simple(db, "BEGIN"))
        return FALSE;
    int     moved = shared_db_changed(db->shared);
    int64_t t = 0, n = 0;            /* the two revisions                  */
    gboolean ok = shared_db_rev(db->shared, SHARED_DB_REV_TASKS, &t, NULL) == 0 &&
                  shared_db_rev(db->shared, SHARED_DB_REV_NOTES, &n, NULL) == 0;
    exec_simple(db, "COMMIT");
    if (!ok)
        return FALSE;
    *changed   = moved != 0;
    *rev_tasks = t;
    *rev_notes = n;
    return TRUE;
}

/* The columns both listings read off the action_items_ctx view, in
 * run_action_query()'s order: ACTION_COLS then the context.                 */
#define ACTION_CTX_COLS ACTION_COLS ", context"

/* run_action_query() — collect every row of `stmt` (columns:
 * ACTION_CTX_COLS) into a list of OnActionItem* and finalize it — the
 * action-item counterpart of run_meta_query()/run_tag_query().             */
static GList *
run_action_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnActionItem rows   */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnActionItem *it = g_new0(OnActionItem, 1);
        it->note_id = sqlite3_column_int64(stmt, 0);
        it->ord     = sqlite3_column_int(stmt, 1);
        it->text    = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        it->done    = sqlite3_column_int(stmt, 3) != 0;
        it->due     = sqlite3_column_int64(stmt, 4);
        it->uid     = sqlite3_column_int64(stmt, 5);
        it->context = g_strdup((const gchar *)sqlite3_column_text(stmt, 6));
        out = g_list_prepend(out, it);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_action_list(OnDatabase *db)
{
    /* Off the action_items_ctx view, so the GUI's Context and a sister
     * app's reading of it are one definition.                              */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_CTX_COLS " FROM " ACTION_CTX_VIEW_NAME " "
        "WHERE visible ORDER BY updated_at DESC, ord");
    return (stmt != NULL) ? run_action_query(stmt) : NULL;
}

GList *
on_db_action_list_for_note(OnDatabase *db, gint64 note_id)
{
    /* Off the view as well, so an item read by note carries the same
     * context an item read across notes does (action show vs action list). */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_CTX_COLS " FROM " ACTION_CTX_VIEW_NAME " "
        "WHERE note_id=? ORDER BY ord");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, note_id);
    return run_action_query(stmt);
}

OnActionItem *
on_db_action_get(OnDatabase *db, gint64 uid)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_CTX_COLS " FROM " ACTION_CTX_VIEW_NAME
        " WHERE uid=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, uid);
    GList *one = run_action_query(stmt);   /* at most one row             */
    OnActionItem *it = one != NULL ? one->data : NULL;
    g_list_free(one);
    return it;
}

void
on_db_action_counts(OnDatabase *db, gint *total, gint *open)
{
    if (total != NULL) *total = 0;
    if (open  != NULL) *open  = 0;
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*), COALESCE(SUM(done=0),0) "
        "FROM " ACTION_CTX_VIEW_NAME " WHERE visible");
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (total != NULL) *total = sqlite3_column_int(stmt, 0);
        if (open  != NULL) *open  = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
}

void
on_db_action_item_free(OnActionItem *it)
{
    g_free(it->text);
    g_free(it->context);
    g_free(it);
}

void
on_db_action_list_free(GList *items)
{
    g_list_free_full(items, (GDestroyNotify)on_db_action_item_free);
}

/* =========================================================================
 * trash
 * ========================================================================= */

gboolean
on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction for the lot, like on_db_notes_delete.                 */
    return bulk_ids(db,
                    "UPDATE notes SET trashed=1 WHERE id=?",
                    ids, n, bind_id_only, NULL, NULL);
}

gboolean
on_db_note_restore(OnDatabase *db, gint64 id)
{
    /* The stored folder_id IS the "where it was deleted from"; it only
     * moves to the top level when that folder is itself in the Trash.       */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET trashed=0, folder_id="
        "  CASE WHEN folder_id IN (SELECT id FROM trash_folder_ids) "
        "       THEN NULL ELSE folder_id END "
        "WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    return stmt_done(db, stmt);
}

gboolean
on_db_folder_trash(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET trashed=1 WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    return stmt_done(db, stmt);
}

gboolean
on_db_folder_restore(OnDatabase *db, gint64 id)
{
    /* Re-parent to the top level when the original parent is still in
     * the Trash (the CASE subquery sees the pre-update flags; the folder
     * can never be its own parent, so clearing its flag in the same
     * statement is safe).                                                   */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET trashed=0, parent_id="
        "  CASE WHEN parent_id IN (SELECT id FROM trash_folder_ids "
        "                          WHERE id<>?) "
        "       THEN NULL ELSE parent_id END "
        "WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

GList *
on_db_folder_list_trashed(OnDatabase *db)
{
    /* FOLDER_COLS, like every other folder query: run_folder_query() reads
     * the columns positionally, so a hand-written list here silently loaded
     * parent_id as the name (and the name as the emoji).                   */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " FOLDER_COLS " FROM folders "
        "WHERE trashed=1 ORDER BY name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    return run_folder_query(stmt);
}

GList *
on_db_note_list_trashed(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE trashed=1 ORDER BY updated_at DESC");
}

gint
on_db_trash_count(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT (SELECT COUNT(*) FROM notes   WHERE trashed=1) + "
        "       (SELECT COUNT(*) FROM folders WHERE trashed=1)", -1);
}

/* run_id_query() — collect a one-column id result set into a GArray of
 * gint64 and finalize the statement.                                        */
static GArray *
run_id_query(sqlite3_stmt *stmt)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (stmt != NULL) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 id = sqlite3_column_int64(stmt, 0);
            g_array_append_val(ids, id);
        }
        sqlite3_finalize(stmt);
    }
    return ids;
}

GArray *
on_db_trash_note_ids(OnDatabase *db)
{
    return run_id_query(prepare(db,
        "SELECT id FROM notes WHERE trashed=1 OR "
        "folder_id IN (SELECT id FROM trash_folder_ids)"));
}

GArray *
on_db_folder_note_ids(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "WITH RECURSIVE sub(id) AS ("
        "  SELECT ? UNION "
        "  SELECT f.id FROM folders f JOIN sub ON f.parent_id = sub.id"
        ") SELECT id FROM notes WHERE folder_id IN (SELECT id FROM sub)");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, folder_id);
    return run_id_query(stmt);
}

gboolean
on_db_trash_empty(OnDatabase *db)
{
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return FALSE;

    /* The explicit notes delete covers directly-trashed notes; deleting
     * the flagged folders cascades through their subtrees (descendant
     * folders and every note inside).                                       */
    gboolean ok =
        exec_simple(db,
            "DELETE FROM notes WHERE trashed=1 OR "
            "folder_id IN (SELECT id FROM trash_folder_ids)") &&
        exec_simple(db, "DELETE FROM folders WHERE trashed=1") &&
        exec_simple(db, PRUNE_ORPHAN_TAGS_SQL);
    return txn_end(db, own, ok);
}

/* =========================================================================
 * counts
 * ========================================================================= */

gboolean
on_db_note_is_trashed(OnDatabase *db, gint64 id)
{
    /* Ask for the note by id AND the visibility rule: a row comes back only
     * when it exists and is visible, so "no row" means trashed — or gone,
     * which the callers have already ruled out.                            */
    return query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE id=? AND " SHARED_DB_NOTE_VISIBLE_SQL,
        id) == 0;
}

gint
on_db_note_count_visible(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL, -1);
}

void
on_db_totals(OnDatabase *db, gint *notes, gint *folders, gint *tags)
{
    if (notes   != NULL) *notes   = 0;
    if (folders != NULL) *folders = 0;
    if (tags    != NULL) *tags    = 0;

    /* Three scalar subqueries in one row: the three totals used to be three
     * separate prepare/step/finalize cycles (each with its SQL built by
     * g_strdup_printf) for one About dialog.                                */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT (SELECT COUNT(*) FROM notes), "
        "       (SELECT COUNT(*) FROM folders), "
        "       (SELECT COUNT(*) FROM tags)");
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (notes   != NULL) *notes   = sqlite3_column_int(stmt, 0);
        if (folders != NULL) *folders = sqlite3_column_int(stmt, 1);
        if (tags    != NULL) *tags    = sqlite3_column_int(stmt, 2);
    }
    sqlite3_finalize(stmt);
}

/* ---------------------------------------------------------------------------
 * count_map_from_query() — run a two-column (id, count) query into a
 * gint64* → GINT_TO_POINTER(count) hash table.
 * ------------------------------------------------------------------------- */
static GHashTable *
count_map_from_query(OnDatabase *db, const gchar *sql)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt != NULL) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 *key = g_new(gint64, 1);
            *key = sqlite3_column_int64(stmt, 0);
            g_hash_table_insert(map, key,
                                GINT_TO_POINTER(
                                    sqlite3_column_int(stmt, 1)));
        }
        sqlite3_finalize(stmt);
    }
    return map;
}

GHashTable *
on_db_note_count_map(OnDatabase *db)
{
    /* trashed=0 only (no subtree filter): folders inside the Trash keep
     * their counts — the map also labels the Trash section's folder rows.   */
    return count_map_from_query(db,
        "SELECT COALESCE(folder_id,0), COUNT(*) FROM notes WHERE trashed=0 "
        "GROUP BY COALESCE(folder_id,0)");
}

GHashTable *
on_db_tag_count_map(OnDatabase *db)
{
    return count_map_from_query(db,
        "SELECT nt.tag_id, COUNT(*) FROM note_tags nt "
        "JOIN notes n ON n.id = nt.note_id "
        "WHERE n.trashed=0 AND (n.folder_id IS NULL OR "
        "      n.folder_id NOT IN (SELECT id FROM trash_folder_ids)) "
        "GROUP BY nt.tag_id");
}

GHashTable *
on_db_note_text_map(OnDatabase *db, gint max_chars)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);
    /* One statement, two uses: full text for searching, a leading slice for
     * the list-view previews.  The truncated form used to be a second,
     * near-identical function.                                              */
    sqlite3_stmt *stmt = (max_chars > 0)
        ? prepare(db, "SELECT id, SUBSTR(body_text, 1, ?) FROM notes "
                      "WHERE body_text IS NOT NULL")
        : prepare(db, "SELECT id, body_text FROM notes "
                      "WHERE body_text IS NOT NULL");
    if (stmt == NULL)
        return map;
    if (max_chars > 0)
        sqlite3_bind_int(stmt, 1, max_chars);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 *key = g_new(gint64, 1);
        *key = sqlite3_column_int64(stmt, 0);
        g_hash_table_insert(map, key,
            g_strdup((const gchar *)sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return map;
}

/* =========================================================================
 * utilities
 * ========================================================================= */

/* FolderRow — one folders row held in memory while building the path
 * map: parent id + name.                                                    */
typedef struct {
    gint64  parent;
    gchar  *name;
} FolderRow;

/* folder_row_free() — GDestroyNotify for FolderRow values.                  */
static void
folder_row_free(gpointer data)
{
    FolderRow *r = data;
    g_free(r->name);
    g_free(r);
}

GHashTable *
on_db_folder_path_map(OnDatabase *db)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);

    /* Load every folder row once (trashed included), then build each
     * path by walking parents in memory — no per-folder queries.            */
    GHashTable *rows = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, folder_row_free);
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, COALESCE(parent_id,0), name FROM folders");
    if (stmt == NULL) {
        g_hash_table_destroy(rows);
        return map;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 *key = g_new(gint64, 1);
        *key = sqlite3_column_int64(stmt, 0);
        FolderRow *r = g_new0(FolderRow, 1);
        r->parent = sqlite3_column_int64(stmt, 1);
        r->name   = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        g_hash_table_insert(rows, key, r);
    }
    sqlite3_finalize(stmt);

    GHashTableIter it;               /* over the loaded rows                */
    gpointer k, v;
    g_hash_table_iter_init(&it, rows);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        FolderRow *r = v;            /* the folder itself                   */
        GString *path = g_string_new(r->name);
        gint64 cur   = r->parent;    /* ancestor cursor                     */
        gint   depth = 0;            /* cap guards a corrupt cycle          */
        while (cur != 0 && depth++ < ON_DB_FOLDER_PATH_DEPTH_LIMIT) {
            FolderRow *p = g_hash_table_lookup(rows, &cur);
            if (p == NULL)
                break;
            g_string_prepend(path, "/");
            g_string_prepend(path, p->name);
            cur = p->parent;
        }
        gint64 *key = g_new(gint64, 1);
        *key = *(gint64 *)k;
        g_hash_table_insert(map, key, g_string_free(path, FALSE));
    }
    g_hash_table_destroy(rows);
    return map;
}

gchar *
on_db_folder_path(OnDatabase *db, gint64 folder_id)
{
    /* Walk from the folder up to the root, prepending each name.  The
     * depth cap keeps a corrupt parent_id cycle (hand-edited db) from
     * spinning forever — no real tree is anywhere near that deep.          */
    GString *path = g_string_new("");    /* built back-to-front             */
    gint64   cur  = folder_id;           /* current folder in the walk      */
    gint     depth = 0;                  /* levels walked so far            */

    while (cur > 0 && depth++ < ON_DB_FOLDER_PATH_DEPTH_LIMIT) {
        /* Cached statement: this loop runs once per tree LEVEL, and the
         * function itself runs on every library navigation and every editor
         * focus-in, so a fresh prepare/finalize per level added up.        */
        sqlite3_stmt *stmt = cached_prepare(db,
            "SELECT name, COALESCE(parent_id,0) FROM folders WHERE id=?");
        if (stmt == NULL)
            break;
        sqlite3_bind_int64(stmt, 1, cur);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *name = (const gchar *)sqlite3_column_text(stmt, 0);
            /* Prepend "name/" to whatever we have so far (both prepends
             * copy, so the reset below is safe).                           */
            g_string_prepend(path, "/");
            g_string_prepend(path, name);
            cur = sqlite3_column_int64(stmt, 1);
        } else {
            cur = 0;
        }
        sqlite3_reset(stmt);             /* cache owns it: reset, never
                                            finalize                        */
    }

    /* Trim the trailing slash left by the loop, if any.                    */
    if (path->len > 0 && path->str[path->len - 1] == '/')
        g_string_truncate(path, path->len - 1);
    return g_string_free(path, FALSE);
}

