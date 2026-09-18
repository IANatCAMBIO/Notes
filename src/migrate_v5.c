/* ===========================================================================
 * migrate_v5.c — the one-time migration to user_version 5 (see the header).
 * TEMPORARY: removed once the live database has crossed.
 * =========================================================================== */

#include "migrate_v5.h"

#include "document.h"

#include <glib/gstdio.h>             /* g_unlink                            */
#include <string.h>

/* The version this migration produces.  2, 3 and 4 are consumed by earlier
 * one-time passes (CLAUDE.md); a database at any of them, or at 0, comes
 * through here.                                                             */
#define MIGRATE_V5_VERSION 5

/* Suffix of the pre-migration copy, beside the database file.              */
#define MIGRATE_V5_BACKUP_SUFFIX ".pre-v5.bak"

/* ---------------------------------------------------------------------------
 * OldRow — one action_items row of a note, as the old mirror held it.
 *
 * Fields:
 *   ord     — its position in the old table (can be stale, see the header).
 *   text    — its text (owned).
 *   uid     — its stable id, 0 for a row from before the uid column.
 *   claimed — matched to a line already?
 * ------------------------------------------------------------------------- */
typedef struct {
    gint     ord;
    gchar   *text;
    gint64   uid;
    gboolean claimed;
} OldRow;

/* old_row_free() — GDestroyNotify for an OldRow.                            */
static void
old_row_free(gpointer data)
{
    OldRow *r = data;
    g_free(r->text);
    g_free(r);
}

/* old_rows_free() — GDestroyNotify for a note's GPtrArray of OldRow.       */
static void
old_rows_free(gpointer data)
{
    g_ptr_array_unref(data);
}

/* ---------------------------------------------------------------------------
 * Counters — what the pass did, for the summary line.
 * ------------------------------------------------------------------------- */
typedef struct {
    guint notes, rewritten, identical, normalized;
    guint tasks, carried, fresh, stale_rows;
} Counts;

/* ---------------------------------------------------------------------------
 * exec_sql() — run one statement on the raw handle; warns on failure.
 *   db  — open database.
 *   sql — the statement.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
exec_sql(OnDatabase *db, const gchar *sql)
{
    gchar *err = NULL;               /* sqlite-allocated message            */
    if (sqlite3_exec(db->handle, sql, NULL, NULL, &err) != SQLITE_OK) {
        g_warning("migrate v5: %s (sql: %.60s)", err, sql);
        sqlite3_free(err);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * backup() — VACUUM INTO the pre-migration copy, unless one that VERIFIES
 * is already there.  Runs OUTSIDE the transaction (VACUUM refuses to run
 * inside one).  A copy that fails its check — the truncated file a
 * VACUUM stopped by a full disk leaves behind (measured) — is deleted,
 * journal and all, and written again; a stale one is never trusted.
 *   db — open database.
 * Returns TRUE when a verified copy is in place.
 * ------------------------------------------------------------------------- */
static gboolean
backup(OnDatabase *db)
{
    gchar *path    = g_strconcat(db->path, MIGRATE_V5_BACKUP_SUFFIX, NULL);
    gchar *journal = g_strconcat(path, "-journal", NULL);
    gboolean ok = FALSE;             /* a verified copy is in place         */
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        ok = on_db_verify_file(path, NULL);
        if (ok)
            g_message("migrate v5: backup %s already exists, kept", path);
        else
            g_warning("migrate v5: backup %s is damaged, redoing it", path);
    }
    if (!ok) {
        g_unlink(path);
        g_unlink(journal);
        gchar *sql = sqlite3_mprintf("VACUUM INTO %Q", path);
        ok = exec_sql(db, sql);
        sqlite3_free(sql);
        if (ok)
            ok = on_db_verify_file(path, NULL);
        if (ok) {
            g_message("migrate v5: backed up to %s", path);
        } else {
            g_unlink(path);          /* never leave a bad copy to be found  */
            g_unlink(journal);
        }
    }
    g_free(journal);
    g_free(path);
    return ok;
}

/* ---------------------------------------------------------------------------
 * old_rows_read() — the action_items table, per note.  A database whose
 * table lacks the uid column (before user_version 3) or has no table at
 * all yields an empty map: nothing there is worth carrying.
 *   db — open database.
 * Returns note_id (gint64*) → GPtrArray of OldRow; destroy it.
 * ------------------------------------------------------------------------- */
static GHashTable *
old_rows_read(OnDatabase *db)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, old_rows_free);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "SELECT note_id, ord, text, uid FROM action_items",
                           -1, &stmt, NULL) != SQLITE_OK)
        return map;                  /* no table, or no uid column          */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 note_id = sqlite3_column_int64(stmt, 0);
        GPtrArray *rows = g_hash_table_lookup(map, &note_id);
        if (rows == NULL) {
            gint64 *key = g_new(gint64, 1);
            *key = note_id;
            rows = g_ptr_array_new_with_free_func(old_row_free);
            g_hash_table_insert(map, key, rows);
        }
        OldRow *r = g_new0(OldRow, 1);
        r->ord  = sqlite3_column_int(stmt, 1);
        r->text = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        r->uid  = sqlite3_column_int64(stmt, 3);
        g_ptr_array_add(rows, r);
    }
    sqlite3_finalize(stmt);
    return map;
}

/* ---------------------------------------------------------------------------
 * row_claim() — the old row for a line: identical text first, then the
 * same ord; NULL when neither matches an unclaimed row with a uid.
 *   rows — the note's old rows (NULL for none).
 *   text — the line's item text.
 *   ord  — the line's position among the note's action lines.
 * ------------------------------------------------------------------------- */
static OldRow *
row_claim(GPtrArray *rows, const gchar *text, gint ord)
{
    if (rows == NULL)
        return NULL;
    for (guint pass = 0; pass < 2; pass++) {
        for (guint i = 0; i < rows->len; i++) {
            OldRow *r = g_ptr_array_index(rows, i);
            if (r->claimed || r->uid == 0)
                continue;
            if (pass == 0 ? g_strcmp0(r->text, text) == 0 : r->ord == ord) {
                r->claimed = TRUE;
                return r;
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * content_write() — the content-only UPDATE: the blob, and nothing else
 * about the note (updated_at in particular), changes.
 *   db   — open database.
 *   id   — the note.
 *   blob — the new bytes.
 *   len  — their count.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
content_write(OnDatabase *db, gint64 id, const guint8 *blob, gsize len)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "UPDATE notes SET content=? WHERE id=?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_blob(stmt, 1, blob, (int)len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* ---------------------------------------------------------------------------
 * migrate_note() — one note: its action lines become tasks, its blob
 * carries their uids.  A note with no real action line is not touched.
 *   db    — open database, inside the transaction.
 *   id    — the note.
 *   rows  — its old action_items rows, or NULL.
 *   c     — counters.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
migrate_note(OnDatabase *db, gint64 id, GPtrArray *rows, Counts *c)
{
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob == NULL)
        return TRUE;
    OnDocument *doc = on_document_from_bnbf(blob, blob_len, NULL);
    GArray *blocks = on_document_action_blocks(doc);
    gboolean ok = TRUE;              /* the outcome                         */

    if (blocks->len > 0) {
        /* The identity guard, before any uid is set: does the model give
         * these bytes back as they are (from byte 8, past the header)?    */
        gsize   n_plain;
        guint8 *plain = on_document_to_bnbf(doc, &n_plain);
        gboolean identical = n_plain == blob_len && blob_len >= 8 &&
                             memcmp(plain + 8, blob + 8, blob_len - 8) == 0;
        g_free(plain);
        if (identical) {
            c->identical++;
        } else {
            c->normalized++;
            g_message("migrate v5: note %" G_GINT64_FORMAT " normalized", id);
        }

        for (guint i = 0; i < blocks->len && ok; i++) {
            OnBlock *b = on_document_block(doc, g_array_index(blocks, guint, i));
            gchar   *text;           /* the line's item                     */
            gint64   due;
            gboolean done;
            on_document_block_action(doc, g_array_index(blocks, guint, i),
                                     &text, &due, &done);
            OldRow *r = row_claim(rows, text, (gint)i);
            gint64 uid = r != NULL ? r->uid : on_db_action_uid_alloc(db);
            if (r != NULL)
                c->carried++;
            else
                c->fresh++;
            ok = uid != 0 &&
                 on_db_action_create(db, id, uid, (gint)i, text, done, due);
            b->action_uid = uid;
            c->tasks++;
            g_free(text);
        }
        if (ok) {
            gsize   n_out;
            guint8 *out = on_document_to_bnbf(doc, &n_out);
            ok = content_write(db, id, out, n_out);
            g_free(out);
            c->rewritten++;
        }
    }
    if (rows != NULL)
        for (guint i = 0; i < rows->len; i++)
            c->stale_rows += !((OldRow *)g_ptr_array_index(rows, i))->claimed;

    g_array_unref(blocks);
    on_document_free(doc);
    g_free(blob);
    return ok;
}

gboolean
on_migrate_v5_run(OnDatabase *db)
{
    if (db == NULL)
        return FALSE;
    if (on_db_user_version(db) >= MIGRATE_V5_VERSION)
        return TRUE;

    /* A database with no notes has nothing to back up: a fresh file.      */
    GList *notes = on_db_note_list_all(db, TRUE);
    if (notes != NULL && !backup(db)) {
        on_db_note_list_free(notes);
        return FALSE;
    }

    if (!exec_sql(db, "BEGIN IMMEDIATE")) {
        on_db_note_list_free(notes);
        return FALSE;
    }
    Counts c = { 0 };
    GHashTable *old = old_rows_read(db);
    gboolean ok = TRUE;              /* the outcome                         */
    for (GList *l = notes; l != NULL && ok; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */
        c.notes++;
        ok = migrate_note(db, m->id, g_hash_table_lookup(old, &m->id), &c);
    }
    g_hash_table_destroy(old);
    on_db_note_list_free(notes);

    ok = ok && exec_sql(db, "DROP TABLE IF EXISTS action_items") &&
         on_db_set_user_version(db, MIGRATE_V5_VERSION) &&
         exec_sql(db, "COMMIT");
    if (!ok) {
        /* A COMMIT that failed (a full disk) has rolled back on its own;
         * only an open transaction needs the explicit one.                */
        if (sqlite3_get_autocommit(db->handle) == 0)
            exec_sql(db, "ROLLBACK");
        g_warning("migrate v5: FAILED — rolled back, database unchanged");
        return FALSE;
    }
    g_message("migrate v5: %u notes, %u rewritten (%u identical, %u "
              "normalized); %u tasks (%u uids carried, %u fresh); %u stale "
              "rows dropped", c.notes, c.rewritten, c.identical,
              c.normalized, c.tasks, c.carried, c.fresh, c.stale_rows);
    return TRUE;
}
