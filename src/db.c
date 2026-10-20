/* ===========================================================================
 * db.c — SQLite persistence layer for Notes (implementation)
 *
 * See db.h for the public API and schema overview.  All functions log
 * failures through g_warning() and return a "failed" value rather than
 * aborting, so the UI can degrade gracefully.
 * =========================================================================== */

#include "db.h"
#include "main_service.h"            /* the health pass's worker + hand-back */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>

/* The async health pass in flight on a connection (health section).    */
typedef struct HealthJob HealthJob;
static void on_db_health_abandon(OnDatabase *db);

/* The action_items view (shared_db.c creates it): every LIVE action item
 * as a task row — note_id, ord, text (the task's title), done (status
 * DONE), due, uid, updated_at (the note's) and visible (the note is
 * outside the Trash).  Every listing reads it.                            */
#define ACTION_VIEW_NAME "action_items"

/* Column list shared by the action-item queries, matching
 * run_action_query()'s expectations.                                        */
#define ACTION_COLS "note_id, ord, text, done, due, uid"

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

/*
 * open_db — the body of on_db_open and on_db_open_existing.
 *
 * Inputs:
 *   path   — the database file, or NULL for shared_db_path().
 *   create — TRUE: a missing file is created (shared_db_open); FALSE: it is
 *            refused (shared_db_open_existing).
 *   err    — optional; on failure receives the reason (g_free it).
 *
 * Output: a new OnDatabase*, or NULL on failure (also logged).
 */
static OnDatabase *
open_db(const gchar *path, gboolean create, gchar **err)
{
    char *module_err = NULL;         /* the shared module's message         */
    char *resolved   = path != NULL ? strdup(path) : shared_db_path(&module_err);
    shared_db *shared = resolved == NULL ? NULL
                      : create ? shared_db_open(resolved, &module_err)
                               : shared_db_open_existing(resolved, &module_err);
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
 * on_db_open — see db.h.
 */
OnDatabase *
on_db_open(const gchar *path, gchar **err)
{
    return open_db(path, TRUE, err);
}

/*
 * on_db_open_existing — see db.h.
 */
OnDatabase *
on_db_open_existing(const gchar *path, gchar **err)
{
    return open_db(path, FALSE, err);
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

/*
 * on_db_health_check_async — see db.h.
 */
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

/*
 * on_db_health — see db.h.
 */
const OnDbHealth *
on_db_health(OnDatabase *db)
{
    return (db->health.when != 0 || db->health.pending) ? &db->health : NULL;
}

/* Bytes per read when hashing.  The file is read in fixed-size chunks
 * rather than slurped: this runs against the live database, whose size is
 * the user's business and not ours to hold in memory twice.              */
#define SHA_CHUNK 65536

/*
 * on_db_file_sha256 — see db.h.
 */
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
 * notes
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * bulk_ids() — run `sql` (one parameter: the row id) once per id inside
 * one write transaction (txn_begin: the caller's, or its own), rolled back
 * whole on the first failure: the body of the delete and the trash of
 * several notes.  The transaction matters for more than atomicity: in
 * autocommit mode SQLite fsyncs per statement, which froze the GUI on a
 * big multi-note operation.
 *   sql     — the statement to step per id.
 *   ids / n — the rows to act on.
 * Returns TRUE when everything committed.  n == 0 is a successful no-op that
 * opens no transaction at all.
 * ------------------------------------------------------------------------- */
static gboolean
bulk_ids(OnDatabase *db, const char *sql, const gint64 *ids, gsize n)
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
        sqlite3_bind_int64(stmt, 1, ids[i]);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    if (!ok)
        g_warning("db: bulk statement failed: %s",
                  sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return txn_end(db, own, ok);
}

/*
 * on_db_note_create — see db.h.
 */
gint64
on_db_note_create(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO notes (created_at, updated_at) "
        "VALUES (strftime('%s','now'), strftime('%s','now'))");
    if (stmt == NULL)
        return 0;

    gint64 new_id = 0;                   /* id of the inserted note         */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: note_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

/*
 * on_db_notes_delete — see db.h.
 */
gboolean
on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction for the lot — autocommit would fsync per note.     */
    return bulk_ids(db, "DELETE FROM notes WHERE id=?", ids, n);
}

/* The size of the pieces a file moves in between the file system and a blob. */
#define FILE_CHUNK (1024 * 1024)

/*
 * on_db_note_file_add — see db.h.
 */
gint64
on_db_note_file_add(OnDatabase *db, gint64 note_id, const gchar *path,
                    const gchar *name)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > ON_FILE_MAX_BYTES) {
        close(fd);
        return 0;
    }
    gboolean own;                        /* this call's transaction?        */
    if (!txn_begin(db, &own)) {
        close(fd);
        return 0;
    }
    gint64 id = 0;                       /* the new row                     */
    gboolean ok = FALSE;
    sqlite3_stmt *stmt = cached_prepare(db,
        "INSERT INTO note_file (note_id, name, size, data) "
        "VALUES (?, ?, ?, zeroblob(?))");
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, note_id);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, st.st_size);
        sqlite3_bind_int64(stmt, 4, st.st_size);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (ok)
            id = sqlite3_last_insert_rowid(db->handle);
        else
            g_warning("db: note_file add: %s", sqlite3_errmsg(db->handle));
        sqlite3_reset(stmt);
    }
    sqlite3_blob *blob = NULL;
    if (ok && st.st_size > 0)
        ok = sqlite3_blob_open(db->handle, "main", "note_file", "data", id, 1,
                               &blob) == SQLITE_OK;
    guint8 *buf = ok && blob != NULL ? g_malloc(FILE_CHUNK) : NULL;
    for (off_t at = 0; ok && blob != NULL && at < st.st_size; ) {
        ssize_t got = read(fd, buf, FILE_CHUNK);
        /* A file that shrank under us would leave zeros: refuse it.        */
        ok = got > 0 && sqlite3_blob_write(blob, buf, (int)got, (int)at) == SQLITE_OK;
        at += got > 0 ? got : 0;
    }
    g_free(buf);
    if (blob != NULL)
        sqlite3_blob_close(blob);
    close(fd);
    return txn_end(db, own, ok) && ok ? id : 0;
}

/*
 * on_db_note_file_copy — see db.h.
 */
gint64
on_db_note_file_copy(OnDatabase *db, gint64 id, gint64 note_id)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "INSERT INTO note_file (note_id, name, size, data) "
        "SELECT ?, name, size, data FROM note_file WHERE id = ?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_int64(stmt, 1, note_id);
    sqlite3_bind_int64(stmt, 2, id);
    gint64 copy = 0;                     /* the new row, or 0               */
    if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db->handle) == 1)
        copy = sqlite3_last_insert_rowid(db->handle);
    sqlite3_reset(stmt);
    return copy;
}

/*
 * on_db_note_file_export — see db.h.
 */
gboolean
on_db_note_file_export(OnDatabase *db, gint64 id, const gchar *path)
{
    sqlite3_blob *blob = NULL;
    if (sqlite3_blob_open(db->handle, "main", "note_file", "data", id, 0,
                          &blob) != SQLITE_OK)
        return FALSE;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        sqlite3_blob_close(blob);
        return FALSE;
    }
    int      size = sqlite3_blob_bytes(blob);
    guint8  *buf  = g_malloc(FILE_CHUNK);
    gboolean ok   = TRUE;
    for (int at = 0; ok && at < size; ) {
        int n = MIN(FILE_CHUNK, size - at);
        ok = sqlite3_blob_read(blob, buf, n, at) == SQLITE_OK &&
             write(fd, buf, (size_t)n) == n;
        at += n;
    }
    g_free(buf);
    sqlite3_blob_close(blob);
    ok = close(fd) == 0 && ok;
    if (!ok)
        unlink(path);
    return ok;
}

/*
 * on_db_note_files_keep — see db.h.
 */
gboolean
on_db_note_files_keep(OnDatabase *db, gint64 note_id, const gint64 *keep,
                      gsize n)
{
    GString *sql = g_string_new("DELETE FROM note_file WHERE note_id = ?");
    if (n > 0) {
        g_string_append(sql, " AND id NOT IN (");
        for (gsize i = 0; i < n; i++)
            g_string_append_printf(sql, i > 0 ? ",%" G_GINT64_FORMAT : "%" G_GINT64_FORMAT,
                                   keep[i]);
        g_string_append_c(sql, ')');
    }
    sqlite3_stmt *stmt = n == 0 ? cached_prepare(db, sql->str) : prepare(db, sql->str);
    gboolean ok = FALSE;
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, note_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok)
            g_warning("db: note_file keep: %s", sqlite3_errmsg(db->handle));
        if (n == 0)
            sqlite3_reset(stmt);
        else
            sqlite3_finalize(stmt);
    }
    g_string_free(sql, TRUE);
    return ok;
}

/*
 * on_db_note_save — see db.h.
 */
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

/*
 * on_db_note_body_text — see db.h.
 */
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

/*
 * on_db_note_set_body_text — see db.h.
 */
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

/*
 * on_db_note_set_updated_at — see db.h.
 */
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

/*
 * on_db_note_load — see db.h.
 */
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
#define NOTE_META_COLS "id, title, updated_at, pinned, created_at"

/* ---------------------------------------------------------------------------
 * meta_from_row() — build one OnNoteMeta from the current result row of a
 * statement selecting NOTE_META_COLS in that order.
 * ------------------------------------------------------------------------- */
static OnNoteMeta *
meta_from_row(sqlite3_stmt *stmt)
{
    OnNoteMeta *m = g_new0(OnNoteMeta, 1);
    m->id         = sqlite3_column_int64(stmt, 0);
    m->title      = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    m->updated_at = sqlite3_column_int64(stmt, 2);
    m->pinned     = sqlite3_column_int(stmt, 3) != 0;
    m->created_at = sqlite3_column_int64(stmt, 4);
    return m;
}

/*
 * on_db_note_get — see db.h.
 */
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

/*
 * on_db_note_list_all — see db.h.
 */
GList *
on_db_note_list_all(OnDatabase *db, gboolean include_trash)
{
    return meta_query(db, include_trash
        ? "SELECT " NOTE_META_COLS " "
          "FROM notes ORDER BY updated_at DESC, id"
        : "SELECT " NOTE_META_COLS " "
          "FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL " "
          "ORDER BY updated_at DESC, id");
}

/*
 * on_db_note_list_recent — see db.h.
 */
GList *
on_db_note_list_recent(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
}

/*
 * on_db_note_set_pinned — see db.h.
 */
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

/*
 * on_db_note_list_pinned — see db.h.
 */
GList *
on_db_note_list_pinned(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE pinned=1 AND " SHARED_DB_NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
}

/*
 * on_db_note_count_pinned — see db.h.
 */
gint
on_db_note_count_pinned(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE pinned=1 AND " SHARED_DB_NOTE_VISIBLE_SQL,
        -1);
}

/*
 * on_db_note_meta_free — see db.h.
 */
void
on_db_note_meta_free(OnNoteMeta *meta)
{
    if (meta == NULL)
        return;
    g_free(meta->title);
    g_free(meta);
}

/*
 * on_db_note_list_free — see db.h.
 */
void
on_db_note_list_free(GList *notes)
{
    g_list_free_full(notes, (GDestroyNotify)on_db_note_meta_free);
}

/*
 * on_db_note_list_ids — see db.h.
 */
gint64 *
on_db_note_list_ids(GList *notes, gsize *n)
{
    *n = g_list_length(notes);
    gint64 *ids = g_new(gint64, *n + 1);
    gsize   i   = 0;
    for (GList *l = notes; l != NULL; l = l->next)
        ids[i++] = ((const OnNoteMeta *)l->data)->id;
    return ids;
}

/* =========================================================================
 * tags — the one set both apps share (shared_db.h): a note's are the #tags
 * of its text (in_text); in_field rows are the move from folders' until
 * Notes writes them into the text; the schema's triggers drop a tag
 * nothing carries any more
 * ========================================================================= */

/* The columns run_tag_query reads, over the tags table aliased g.          */
#define TAG_COLS "g.id, g.name, g.emoji, g.favorite_notes"

/* ---------------------------------------------------------------------------
 * run_tag_query() — step a prepared statement whose columns are TAG_COLS,
 * then (when `sources`) in_field and in_text, and collect the rows as
 * OnTag structs.  Finalizes `stmt`.
 * Returns a GList of OnTag*; free with on_db_tag_list_free().
 * ------------------------------------------------------------------------- */
static GList *
run_tag_query(sqlite3_stmt *stmt, gboolean sources)
{
    GList *out = NULL;                   /* accumulated OnTag* rows         */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnTag *t = g_new0(OnTag, 1);
        t->id       = sqlite3_column_int64(stmt, 0);
        t->name     = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        t->emoji    = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        t->favorite = sqlite3_column_int(stmt, 3) != 0;
        if (sources) {
            t->in_field = sqlite3_column_int(stmt, 4) != 0;
            t->in_text  = sqlite3_column_int(stmt, 5) != 0;
        }
        if (t->name == NULL)
            t->name = g_strdup("");
        if (t->emoji == NULL)
            t->emoji = g_strdup("");
        out = g_list_prepend(out, t);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

/*
 * on_db_tag_list — see db.h.
 */
GList *
on_db_tag_list(OnDatabase *db, OnTagScope scope)
{
    sqlite3_stmt *stmt = prepare(db, scope == ON_TAGS_ALL
        ? "SELECT " TAG_COLS " FROM tags g ORDER BY g.name COLLATE NOCASE, g.id"
        : "SELECT " TAG_COLS " FROM tags g WHERE EXISTS (SELECT 1 FROM note_tags nt"
          " JOIN notes n ON n.id = nt.note_id WHERE nt.tag_id = g.id AND n."
          SHARED_DB_NOTE_VISIBLE_SQL ") ORDER BY g.name COLLATE NOCASE, g.id");
    return stmt != NULL ? run_tag_query(stmt, FALSE) : NULL;
}

/*
 * on_db_tag_free — see db.h.
 */
void
on_db_tag_free(OnTag *tag)
{
    if (tag == NULL)
        return;
    g_free(tag->name);
    g_free(tag->emoji);
    g_free(tag);
}

/*
 * on_db_tag_list_free — see db.h.
 */
void
on_db_tag_list_free(GList *tags)
{
    g_list_free_full(tags, (GDestroyNotify)on_db_tag_free);
}

/* first_tag() — the first tag of a run_tag_query list (the rest freed), or
 * NULL for an empty one.                                                   */
static OnTag *
first_tag(GList *tags)
{
    OnTag *tag = NULL;                   /* the result, taken out of the list */
    if (tags != NULL) {
        tag = tags->data;
        tags->data = NULL;
    }
    on_db_tag_list_free(tags);
    return tag;
}

/*
 * on_db_tag_get — see db.h.
 */
OnTag *
on_db_tag_get(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db, "SELECT " TAG_COLS " FROM tags g WHERE g.id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    return first_tag(run_tag_query(stmt, FALSE));
}

/*
 * on_db_tag_find — see db.h.
 */
OnTag *
on_db_tag_find(OnDatabase *db, const gchar *name)
{
    gchar        *clean = g_strstrip(g_strdup(name != NULL ? name : ""));
    sqlite3_stmt *stmt  = prepare(db, "SELECT " TAG_COLS " FROM tags g WHERE g.name=?");
    if (stmt == NULL) {
        g_free(clean);
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, clean, -1, SQLITE_TRANSIENT);
    g_free(clean);
    return first_tag(run_tag_query(stmt, FALSE));
}

/*
 * on_db_tag_set_emoji — see db.h.
 */
gboolean
on_db_tag_set_emoji(OnDatabase *db, gint64 id, const gchar *emoji)
{
    gchar        *clean = g_strstrip(g_strdup(emoji != NULL ? emoji : ""));
    sqlite3_stmt *stmt  = prepare(db, "UPDATE tags SET emoji=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, clean, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
    }
    g_free(clean);
    return stmt_done(db, stmt);
}

/*
 * on_db_tag_set_favorite — see db.h.
 */
gboolean
on_db_tag_set_favorite(OnDatabase *db, gint64 id, gboolean favorite)
{
    sqlite3_stmt *stmt = prepare(db, "UPDATE tags SET favorite_notes=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int(stmt, 1, favorite ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

/*
 * on_db_note_tags — see db.h.
 */
GList *
on_db_note_tags(OnDatabase *db, gint64 note_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " TAG_COLS ", nt.in_field, nt.in_text FROM note_tags nt"
        " JOIN tags g ON g.id = nt.tag_id WHERE nt.note_id=?"
        " ORDER BY g.name COLLATE NOCASE, g.id");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, note_id);
    return run_tag_query(stmt, TRUE);
}

/* ---------------------------------------------------------------------------
 * tag_link() — the tag `name` (made when no tag of that name exists, in any
 * case) linked to note `note_id` from its text.  Inside the caller's
 * transaction.
 * Returns the tag id, or 0 on failure.
 * ------------------------------------------------------------------------- */
static gint64
tag_link(OnDatabase *db, gint64 note_id, const gchar *name)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "INSERT INTO tags (name) VALUES (?) ON CONFLICT (name) DO NOTHING");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (!cached_stmt_done(db, stmt))
        return 0;
    stmt = cached_prepare(db,
        "INSERT INTO note_tags (note_id, tag_id, in_text)"
        " SELECT ?1, id, 1 FROM tags WHERE name = ?2"
        " ON CONFLICT (note_id, tag_id) DO UPDATE SET in_text = 1");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_int64(stmt, 1, note_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    if (!cached_stmt_done(db, stmt))
        return 0;
    stmt = cached_prepare(db, "SELECT id FROM tags WHERE name = ?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    gint64 id = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return id;
}

/*
 * on_db_note_set_text_tags — see db.h.  The new set is linked first, then
 * every text link of the note that is not in it lets go: a row the field
 * also has keeps it (in_text cleared), the rest go (and their tag with
 * them, when nothing else carries it: the schema's trigger).
 */
gboolean
on_db_note_set_text_tags(OnDatabase *db, gint64 note_id, GList *tag_names)
{
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return FALSE;

    GString *keep = g_string_new("");    /* the kept ids, "1,5,9"           */
    gboolean ok   = TRUE;                /* overall success flag            */
    for (GList *l = tag_names; ok && l != NULL; l = l->next) {
        gint64 id = tag_link(db, note_id, l->data);
        ok = id != 0;
        g_string_append_printf(keep, "%s%" G_GINT64_FORMAT, keep->len > 0 ? "," : "", id);
    }
    if (ok) {
        gchar *sql = g_strdup_printf(
            "UPDATE note_tags SET in_text = 0 WHERE note_id = %" G_GINT64_FORMAT
            " AND in_text = 1 AND in_field = 1 AND tag_id NOT IN (%s);"
            "DELETE FROM note_tags WHERE note_id = %" G_GINT64_FORMAT
            " AND in_text = 1 AND in_field = 0 AND tag_id NOT IN (%s)",
            note_id, keep->str, note_id, keep->str);
        ok = exec_simple(db, sql);
        g_free(sql);
    }
    g_string_free(keep, TRUE);
    return txn_end(db, own, ok);
}

/*
 * on_db_field_tag_notes — see db.h.
 */
GArray *
on_db_field_tag_notes(OnDatabase *db)
{
    GArray       *out  = g_array_new(FALSE, FALSE, sizeof(gint64));
    sqlite3_stmt *stmt = prepare(db,
        "SELECT DISTINCT note_id FROM note_tags WHERE in_field = 1 ORDER BY note_id");
    if (stmt == NULL)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 id = sqlite3_column_int64(stmt, 0);
        g_array_append_val(out, id);
    }
    sqlite3_finalize(stmt);
    return out;
}

/*
 * on_db_note_ids_with_tag — see db.h.
 */
GArray *
on_db_note_ids_with_tag(OnDatabase *db, gint64 tag_id)
{
    GArray       *out  = g_array_new(FALSE, FALSE, sizeof(gint64));
    sqlite3_stmt *stmt = prepare(db,
        "SELECT DISTINCT note_id FROM note_tags WHERE tag_id = ? ORDER BY note_id");
    if (stmt == NULL)
        return out;
    sqlite3_bind_int64(stmt, 1, tag_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 id = sqlite3_column_int64(stmt, 0);
        g_array_append_val(out, id);
    }
    sqlite3_finalize(stmt);
    return out;
}

/*
 * on_db_note_field_tags_clear — see db.h.
 */
gboolean
on_db_note_field_tags_clear(OnDatabase *db, gint64 note_id)
{
    gchar *sql = g_strdup_printf(
        "DELETE FROM note_tags WHERE note_id = %" G_GINT64_FORMAT
        " AND in_field = 1 AND in_text = 0;"
        "UPDATE note_tags SET in_field = 0 WHERE note_id = %" G_GINT64_FORMAT
        " AND in_field = 1",
        note_id, note_id);
    gboolean ok = exec_simple(db, sql);
    g_free(sql);
    return ok;
}

/*
 * on_db_notes_by_tag — see db.h.
 */
GList *
on_db_notes_by_tag(OnDatabase *db, gint64 tag_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " FROM notes "
        "WHERE id IN (SELECT note_id FROM note_tags WHERE tag_id=?) "
        "  AND " SHARED_DB_NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
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

/*
 * on_db_action_uid_alloc — see db.h.
 */
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
 * the note_task_tombstone trigger applies when a link row is deleted
 * (SHARED_DB_SQL_TOMBSTONE_WHERE)
 * (a subtask deleted before keeps its own updated_at, so task_revive can
 * tell the two apart).
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
task_tombstone(OnDatabase *db, gint64 task_id)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE tasks SET deleted = 1, updated_at = ?1"
        " WHERE " SHARED_DB_SQL_TOMBSTONE_WHERE("?2"));
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
 * task_create() — a new task for a '!' line: appended as a top-level task
 * (after every other: Tasks' one order), with the line's title, due date
 * and done state (status DONE and completed_at now, else New) and every
 * other column at the schema's default; then its note_task link, the
 * seen_* baseline set to the line's values, which tags it "Action Items"
 * (the schema's note_task_action_tag trigger).
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
task_create(OnDatabase *db, const OnActionItem *it, gint64 note_id, gint ord)
{
    gint64 now = task_now();
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO tasks (parent_id, title, due, status,"
        "                   completed_at, position, updated_at, created_at)"
        " VALUES (NULL, ?1, ?2, ?3, ?4,"
        "   (SELECT COALESCE(MAX(position), 0) + 1 FROM tasks"
        "     WHERE parent_id IS NULL), ?5, ?5)");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_text(stmt, 1, it->text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, it->due);
    sqlite3_bind_int(stmt, 3, it->done ? SHARED_DB_TASK_STATUS_DONE : SHARED_DB_TASK_STATUS_NEW);
    sqlite3_bind_int64(stmt, 4, it->done ? now : 0);
    sqlite3_bind_int64(stmt, 5, now);
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
 * column_bytes() — every byte of a TEXT column, NUL-terminated but kept
 * whole when it holds a NUL (sqlite3_column_bytes, not strlen).
 *   stmt — a statement on a result row.
 *   col  — the column.
 *   len  — out: the byte count.
 * Returns a new string ("" for NULL); g_free() it.
 * ------------------------------------------------------------------------- */
static gchar *
column_bytes(sqlite3_stmt *stmt, int col, gsize *len)
{
    const guchar *text = sqlite3_column_text(stmt, col);
    *len = (gsize)sqlite3_column_bytes(stmt, col);
    gchar *out = g_malloc(*len + 1);
    if (*len > 0)
        memcpy(out, text, *len);
    out[*len] = '\0';
    return out;
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
    row->seen_title   = column_bytes(stmt, 4, &row->seen_title_len);
    row->seen_done    = sqlite3_column_int(stmt, 5) != 0;
    row->seen_due     = sqlite3_column_int64(stmt, 6);
    row->seen_deleted = sqlite3_column_int(stmt, 7) != 0;
    row->title        = column_bytes(stmt, 8, &row->title_len);
    row->title_ok     = g_utf8_validate_len(row->title, row->title_len, NULL);
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

/*
 * on_db_action_link_clear — see db.h.
 */
void
on_db_action_link_clear(OnActionLink *row)
{
    g_free(row->seen_title);
    g_free(row->title);
    row->seen_title     = NULL;
    row->title          = NULL;
    row->seen_title_len = 0;
    row->title_len      = 0;
}

/*
 * on_db_action_links — see db.h.
 */
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

/*
 * on_db_action_link_update — see db.h.
 */
gboolean
on_db_action_link_update(OnDatabase *db, const OnActionLink *row)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE note_task SET note_id=?, ord=?, seen_title=?, seen_done=?,"
        " seen_due=?, seen_deleted=? WHERE uid=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, row->note_id);
    sqlite3_bind_int(stmt, 2, row->ord);
    sqlite3_bind_text(stmt, 3, row->seen_title != NULL ? row->seen_title : "",
                      row->seen_title != NULL ? (int)row->seen_title_len : 0,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, row->seen_done ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, row->seen_due);
    sqlite3_bind_int(stmt, 6, row->seen_deleted ? 1 : 0);
    sqlite3_bind_int64(stmt, 7, row->uid);
    return cached_stmt_done(db, stmt);
}

/*
 * on_db_action_link_title_moved — see db.h.
 */
gboolean
on_db_action_link_title_moved(const OnActionLink *row)
{
    return row->title_len != row->seen_title_len ||
           memcmp(row->title, row->seen_title, row->title_len) != 0;
}

/*
 * on_db_action_link_agree_title — see db.h.
 */
void
on_db_action_link_agree_title(OnActionLink *row, const gchar *text, gsize len)
{
    g_free(row->seen_title);
    row->seen_title = g_malloc(len + 1);
    if (len > 0)
        memcpy(row->seen_title, text, len);
    row->seen_title[len] = '\0';
    row->seen_title_len  = len;
}

/*
 * on_db_action_link_delete — see db.h.
 */
gboolean
on_db_action_link_delete(OnDatabase *db, gint64 uid)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM note_task WHERE uid=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, uid);
    return stmt_done(db, stmt);
}

/*
 * on_db_action_diff — see db.h.
 */
guint
on_db_action_diff(const OnActionItem *it, const OnActionLink *row)
{
    guint mask = 0;                  /* the fields that differ              */
    gsize n    = it->text != NULL ? strlen(it->text) : 0;
    if (n != row->seen_title_len || memcmp(it->text, row->seen_title, n) != 0)
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
        on_db_action_link_agree_title(row, it->text, strlen(it->text));
        row->seen_done  = it->done;
        row->seen_due   = it->due;
        link = TRUE;
    }
    row->note_id = note_id;
    row->ord     = ord;
    if (ok && link)
        ok = on_db_action_link_update(db, row);
    return ok;
}

/* One task of a note whose line is gone (on_db_note_save_actions).       */
typedef struct {
    gint64 uid;                      /* its note_task row                   */
    gint64 task_id;                  /* the task to tombstone               */
} GoneLine;

/*
 * on_db_note_save_actions — see db.h.
 */
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

/*
 * on_db_action_set — see db.h.
 */
gboolean
on_db_action_set(OnDatabase *db, gint64 uid, guint mask, const gchar *title,
                 gboolean done, gint64 due)
{
    gint64 task_id = query_int64(db,
        "SELECT nt.task_id FROM note_task nt JOIN tasks t ON t.id = nt.task_id"
        " WHERE nt.uid = ? AND t.deleted = 0", uid);
    return task_id != 0 && task_push(db, task_id, mask, title, done, due);
}

/*
 * on_db_action_pending — see db.h.
 */
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

/* run_action_query() — collect every row of `stmt` (columns:
 * ACTION_COLS) into a list of OnActionItem* and finalize it — the
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
        out = g_list_prepend(out, it);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

/*
 * on_db_action_list — see db.h.
 */
GList *
on_db_action_list(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_COLS " FROM " ACTION_VIEW_NAME " "
        "WHERE visible ORDER BY updated_at DESC, note_id, ord");
    return (stmt != NULL) ? run_action_query(stmt) : NULL;
}

/*
 * on_db_action_list_for_note — see db.h.
 */
GList *
on_db_action_list_for_note(OnDatabase *db, gint64 note_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_COLS " FROM " ACTION_VIEW_NAME " "
        "WHERE note_id=? ORDER BY ord");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, note_id);
    return run_action_query(stmt);
}

/*
 * on_db_action_get — see db.h.
 */
OnActionItem *
on_db_action_get(OnDatabase *db, gint64 uid)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_COLS " FROM " ACTION_VIEW_NAME
        " WHERE uid=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, uid);
    GList *one = run_action_query(stmt);   /* at most one row             */
    OnActionItem *it = one != NULL ? one->data : NULL;
    g_list_free(one);
    return it;
}

/*
 * on_db_action_counts — see db.h.
 */
void
on_db_action_counts(OnDatabase *db, gint *total, gint *open)
{
    if (total != NULL) *total = 0;
    if (open  != NULL) *open  = 0;
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*), COALESCE(SUM(done=0),0) "
        "FROM " ACTION_VIEW_NAME " WHERE visible");
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (total != NULL) *total = sqlite3_column_int(stmt, 0);
        if (open  != NULL) *open  = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
}

/*
 * on_db_action_item_free — see db.h.
 */
void
on_db_action_item_free(OnActionItem *it)
{
    g_free(it->text);
    g_free(it);
}

/*
 * on_db_action_list_free — see db.h.
 */
void
on_db_action_list_free(GList *items)
{
    g_list_free_full(items, (GDestroyNotify)on_db_action_item_free);
}

/* =========================================================================
 * trash
 * ========================================================================= */

/*
 * on_db_notes_trash — see db.h.
 */
gboolean
on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction for the lot, like on_db_notes_delete.                 */
    return bulk_ids(db, "UPDATE notes SET trashed=1 WHERE id=?", ids, n);
}

/*
 * on_db_note_restore — see db.h.
 */
gboolean
on_db_note_restore(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db, "UPDATE notes SET trashed=0 WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    return stmt_done(db, stmt);
}

/*
 * on_db_note_list_trashed — see db.h.
 */
GList *
on_db_note_list_trashed(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE trashed=1 ORDER BY updated_at DESC");
}

/*
 * on_db_trash_count — see db.h.
 */
gint
on_db_trash_count(OnDatabase *db)
{
    return (gint)query_int64(db, "SELECT COUNT(*) FROM notes WHERE trashed=1", -1);
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

/*
 * on_db_trash_note_ids — see db.h.
 */
GArray *
on_db_trash_note_ids(OnDatabase *db)
{
    return run_id_query(prepare(db, "SELECT id FROM notes WHERE trashed=1"));
}

/*
 * on_db_note_ids_with_files — see db.h.
 */
GArray *
on_db_note_ids_with_files(OnDatabase *db)
{
    return run_id_query(prepare(db, "SELECT DISTINCT note_id FROM note_file"));
}

/*
 * on_db_trash_empty — see db.h.
 */
gboolean
on_db_trash_empty(OnDatabase *db)
{
    gboolean own;                        /* this call's transaction?       */
    if (!txn_begin(db, &own))
        return FALSE;
    return txn_end(db, own, exec_simple(db, "DELETE FROM notes WHERE trashed=1"));
}

/* =========================================================================
 * counts
 * ========================================================================= */

/*
 * on_db_note_is_trashed — see db.h.
 */
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

/*
 * on_db_note_count_visible — see db.h.
 */
gint
on_db_note_count_visible(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL, -1);
}

/*
 * on_db_totals — see db.h.
 */
void
on_db_totals(OnDatabase *db, gint *notes, gint *tags)
{
    if (notes != NULL) *notes = 0;
    if (tags  != NULL) *tags  = 0;

    /* Two scalar subqueries in one row: one statement for the dialog.      */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT (SELECT COUNT(*) FROM notes), "
        "       (SELECT COUNT(DISTINCT tag_id) FROM note_tags)");
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (notes != NULL) *notes = sqlite3_column_int(stmt, 0);
        if (tags  != NULL) *tags  = sqlite3_column_int(stmt, 1);
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

/*
 * on_db_tag_count_map — see db.h.
 */
GHashTable *
on_db_tag_count_map(OnDatabase *db)
{
    return count_map_from_query(db,
        "SELECT tag_id, COUNT(*) FROM note_tags "
        "WHERE note_id IN (SELECT id FROM notes WHERE "
        SHARED_DB_NOTE_VISIBLE_SQL ") "
        "GROUP BY tag_id");
}

/* ---------------------------------------------------------------------------
 * ids_json() — note ids as a JSON array, "[3,17,42]", for a query's
 * `id IN (SELECT value FROM json_each(?))`: one bound value however many
 * ids, where a list of '?' placeholders would hit SQLite's variable cap.
 *   ids / n — the ids, n > 0.
 * Returns the owned text; g_free() it after the statement has run.
 * ------------------------------------------------------------------------- */
static gchar *
ids_json(const gint64 *ids, gsize n)
{
    GString *json = g_string_new("[");
    for (gsize i = 0; i < n; i++)
        g_string_append_printf(json, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", ids[i]);
    g_string_append_c(json, ']');
    return g_string_free(json, FALSE);
}

/* ---------------------------------------------------------------------------
 * text_map_from_stmt() — run a two-column (id, text) query into a
 * gint64* → owned text hash table, finalizing the statement.
 *   stmt — the prepared, bound statement, or NULL (a failed prepare).
 * Returns the map; empty when stmt is NULL.
 * ------------------------------------------------------------------------- */
static GHashTable *
text_map_from_stmt(sqlite3_stmt *stmt)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);
    if (stmt == NULL)
        return map;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 *key = g_new(gint64, 1);
        *key = sqlite3_column_int64(stmt, 0);
        g_hash_table_insert(map, key,
            g_strdup((const gchar *)sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return map;
}

/* The two map queries, with and without the id filter (`where` is the
 * filter clause or "").  The filter's parameter is ?2 in the text query,
 * after max_chars, and ?1 in the tag names query. */
#define NOTE_TEXT_SQL(where) \
    "SELECT id, CASE WHEN ?1 > 0 THEN SUBSTR(body_text, 1, ?1) ELSE body_text END " \
    "FROM notes WHERE body_text IS NOT NULL" where
#define NOTE_TAG_NAMES_SQL(where) \
    "SELECT note_id, group_concat(name, ', ') FROM (" \
    "  SELECT nt.note_id, g.name FROM note_tags nt JOIN tags g ON g.id = nt.tag_id" where \
    "  ORDER BY nt.note_id, g.name COLLATE NOCASE, g.id)" \
    " GROUP BY note_id"

/*
 * on_db_note_text_map — see db.h.
 */
GHashTable *
on_db_note_text_map(OnDatabase *db, gint max_chars, const gint64 *ids, gsize n)
{
    /* One statement, two uses: full text for searching, a leading slice for
     * the list-view previews.  The truncated form used to be a second,
     * near-identical function.                                              */
    sqlite3_stmt *stmt = prepare(db, ids != NULL
        ? NOTE_TEXT_SQL(" AND id IN (SELECT value FROM json_each(?2))")
        : NOTE_TEXT_SQL(""));
    gchar *json = NULL;                  /* the id filter, while it runs   */
    if (stmt != NULL) {
        sqlite3_bind_int(stmt, 1, max_chars);
        if (ids != NULL) {
            json = ids_json(ids, n);
            sqlite3_bind_text(stmt, 2, json, -1, SQLITE_STATIC);
        }
    }
    GHashTable *map = text_map_from_stmt(stmt);
    g_free(json);
    return map;
}

/*
 * on_db_note_tag_names_map — see db.h.
 */
GHashTable *
on_db_note_tag_names_map(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* group_concat over a sorted subquery: SQLite keeps the inner order.   */
    sqlite3_stmt *stmt = prepare(db, ids != NULL
        ? NOTE_TAG_NAMES_SQL(" WHERE nt.note_id IN (SELECT value FROM json_each(?1))")
        : NOTE_TAG_NAMES_SQL(""));
    gchar *json = NULL;                  /* the id filter, while it runs   */
    if (stmt != NULL && ids != NULL) {
        json = ids_json(ids, n);
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
    }
    GHashTable *map = text_map_from_stmt(stmt);
    g_free(json);
    return map;
}
