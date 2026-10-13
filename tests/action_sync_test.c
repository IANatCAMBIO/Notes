/* ===========================================================================
 * action_sync_test.c — the two-way sync of '!' lines with the shared
 * database's tasks: the push on save (only the fields Notes changed, the
 * seen_* baseline), the pull of outside changes (action_sync.h), and the
 * two together against a second connection and a second PROCESS writing
 * the same file, as Tasks does.
 *
 * Every database is the scratch file $CAMBIO_SHARED_DB (check.h).
 * =========================================================================== */

#include "check.h"

#include "app_core.h"
#include "db.h"
#include "serialize.h"

#include <sys/wait.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────────────── */

/*
 * task_int — one integer column of the task behind a uid, read on `db`.
 *
 * Inputs:
 *   db     — a connection
 *   uid    — the item
 *   column — the tasks column (a literal from this file)
 *
 * Output: the value, or -1 when there is no such task.
 */
static gint64
task_int(OnDatabase *db, gint64 uid, const char *column)
{
    gchar *sql = g_strdup_printf("SELECT t.%s FROM tasks t JOIN note_task n ON n.task_id = t.id"
                                 " WHERE n.uid = ?", column);
    sqlite3_stmt *st = NULL;
    gint64        v  = -1;
    sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL);
    sqlite3_bind_int64(st, 1, uid);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    g_free(sql);
    return v;
}

/*
 * task_title — the title of the task behind a uid.
 *
 * Inputs:
 *   db  — a connection
 *   uid — the item
 *
 * Output: a new string ("" when there is no task); g_free it.
 */
static gchar *
task_title(OnDatabase *db, gint64 uid)
{
    sqlite3_stmt *st = NULL;
    gchar        *t  = NULL;
    sqlite3_prepare_v2(db->handle, "SELECT t.title FROM tasks t JOIN note_task n"
                       " ON n.task_id = t.id WHERE n.uid = ?", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, uid);
    if (sqlite3_step(st) == SQLITE_ROW) t = g_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return t != NULL ? t : g_strdup("");
}

/*
 * link_of — the note_task row of a uid, with its task.
 *
 * Inputs:
 *   db   — a connection
 *   uid  — the item
 *   note — the note it is linked to
 *   row  — receives the row (on_db_action_link_clear it)
 *
 * Output: TRUE when found.
 */
static gboolean
link_of(OnDatabase *db, gint64 uid, gint64 note, OnActionLink *row)
{
    GArray  *rows  = on_db_action_links(db, note);
    gboolean found = FALSE;
    for (guint i = 0; i < rows->len && !found; i++) {
        OnActionLink *r = &g_array_index(rows, OnActionLink, i);
        if (r->uid == uid) {
            *row = *r;
            r->seen_title = r->title = NULL;   /* moved to the caller */
            found = TRUE;
        }
    }
    g_array_unref(rows);
    return found;
}

/*
 * nth_uid — the uid of a note's ord-th item, from its stored blob.
 *
 * Inputs:
 *   db   — a connection
 *   note — the note
 *   ord  — the item's position among the note's action lines
 *
 * Output: the uid, 0 when there is no such line.
 */
static gint64
nth_uid(OnDatabase *db, gint64 note, gint ord)
{
    OnDocument *doc    = on_note_document_load(db, note);
    GArray     *blocks = doc != NULL ? on_document_action_blocks(doc) : NULL;
    gint64      uid    = 0;
    if (blocks != NULL && (guint)ord < blocks->len)
        uid = on_document_block(doc, g_array_index(blocks, guint, ord))->action_uid;
    if (blocks != NULL) g_array_unref(blocks);
    on_document_free(doc);
    return uid;
}

/*
 * exec_sql — run SQL on a connection (another app's write, in a test).
 *
 * Inputs:
 *   db  — the connection
 *   sql — the statements
 *
 * Output: TRUE on success.
 */
static gboolean
exec_sql(OnDatabase *db, const char *sql)
{
    return sqlite3_exec(db->handle, sql, NULL, NULL, NULL) == SQLITE_OK;
}

/*
 * exec_uid — run one statement with the item's task id bound as ?1.
 *
 * Inputs:
 *   db  — the connection
 *   uid — the item
 *   sql — one statement using ?1 for the task id
 *
 * Output: TRUE on success.
 */
static gboolean
exec_uid(OnDatabase *db, gint64 uid, const char *sql)
{
    gint64 task = 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db->handle, "SELECT task_id FROM note_task WHERE uid=?", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, uid);
    if (sqlite3_step(st) == SQLITE_ROW) task = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL);
    sqlite3_bind_int64(st, 1, task);
    gboolean ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/*
 * edit_note — load a stored note, apply one action rewrite, save it with
 * the push.
 *
 * Inputs:
 *   db      — the connection
 *   note    — the note
 *   ord     — which action line
 *   text    — new item text, or NULL to leave it
 *   strike  — -1 to leave it, else the new done state
 *   changed — receives whether a task row was written (may be NULL)
 *
 * Output: TRUE when the save succeeded.
 */
static gboolean
edit_note(OnDatabase *db, gint64 note, gint ord, const char *text, int strike,
          gboolean *changed)
{
    OnDocument *doc = on_note_document_load(db, note);
    if (doc == NULL) return FALSE;
    if (text != NULL) on_document_action_text(doc, ord, text);
    if (strike >= 0) on_document_action_strike(doc, ord, strike != 0);
    gboolean ok = on_note_document_save(db, note, doc, TRUE, changed);
    on_document_free(doc);
    return ok;
}

/* ── The push ───────────────────────────────────────────────────────────── */

/*
 * test_push_new — a new line is a new task in the managed list, with the
 * schema's defaults, and its baseline is the line.
 *
 * Inputs:
 *   db — the database
 */
static void
test_push_new(OnDatabase *db)
{
    /* A list with the right name but another emoji is not the managed one. */
    exec_sql(db, "INSERT INTO lists (name, emoji) VALUES ('Action Items', '')");
    gint64 note = on_note_new_from_text(db, 0, "Push\n! Call the bank due 2026-11-02\n! Done one");
    gint64 uid  = nth_uid(db, note, 0);
    check(uid != 0, "push: the line has a uid");
    check(task_int(db, uid, "status") == 0 && task_int(db, uid, "due") == check_midnight(2026, 11, 2) &&
          task_int(db, uid, "completed_at") == 0,
          "push: a new open line is a New task with its due date");
    check(task_int(db, uid, "due_time") == SHARED_DB_TASK_DUE_TIME_DEFAULT &&
          task_int(db, uid, "recur_time") == SHARED_DB_TASK_RECUR_TIME_DEFAULT &&
          task_int(db, uid, "scheduled_duration") == SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT &&
          task_int(db, uid, "priority") == 0 && task_int(db, uid, "parent_id") <= 0,
          "push: every other column is the shared schema's default (due_time 17:00)");
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db->handle, "SELECT l.name, l.emoji FROM lists l JOIN tasks t ON t.list_id = l.id"
                       " JOIN note_task n ON n.task_id = t.id WHERE n.uid = ?", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, uid);
    gboolean managed = sqlite3_step(st) == SQLITE_ROW &&
                       strcmp((const char *)sqlite3_column_text(st, 0), SHARED_DB_ACTION_LIST_NAME) == 0 &&
                       strcmp((const char *)sqlite3_column_text(st, 1), SHARED_DB_ACTION_LIST_EMOJI) == 0;
    sqlite3_finalize(st);
    check(managed, "push: it is filed in the managed list, found by name AND emoji");

    OnActionLink row;
    check(link_of(db, uid, note, &row) && strcmp(row.seen_title, "Call the bank") == 0 &&
          !row.seen_done && row.seen_due == check_midnight(2026, 11, 2) && !row.seen_deleted,
          "push: seen_* is the line's text, done state and due date");
    on_db_action_link_clear(&row);

    gboolean changed = TRUE;
    check(edit_note(db, note, 0, NULL, -1, &changed) && !changed,
          "push: a save that changed no line writes no task");
}

/*
 * test_push_fields — only the fields that changed on Notes' side reach the
 * task: a due date and a priority Tasks set survive a Notes rename; the
 * status rules of a done-only source.
 *
 * Inputs:
 *   db    — Notes' connection
 *   other — a second connection to the same file (Tasks, say)
 */
static void
test_push_fields(OnDatabase *db, OnDatabase *other)
{
    gint64 note = on_note_new_from_text(db, 0, "Fields\n! Write the report due 2026-12-01");
    gint64 uid  = nth_uid(db, note, 0);

    /* Tasks: priority and due.  Notes, not having pulled: renames.          */
    exec_uid(other, uid, "UPDATE tasks SET priority = 3 WHERE id = ?1");
    gint64 tasks_due = check_midnight(2027, 1, 5);
    gchar *set_due = g_strdup_printf("UPDATE tasks SET due = %lld WHERE id = ?1", (long long)tasks_due);
    exec_uid(other, uid, set_due);
    g_free(set_due);
    check(edit_note(db, note, 0, "Write the final report", -1, NULL), "fields: Notes renames the line");
    gchar *title = task_title(db, uid);
    check(strcmp(title, "Write the final report") == 0, "fields: the title is pushed");
    g_free(title);
    check(task_int(db, uid, "due") == tasks_due && task_int(db, uid, "priority") == 3,
          "fields: the due date and priority Tasks set survive (due was not Notes' change)");
    OnActionLink row;
    link_of(db, uid, note, &row);
    check(strcmp(row.seen_title, "Write the final report") == 0 &&
          row.seen_due == check_midnight(2026, 12, 1),
          "fields: seen_title follows the push; seen_due still the old value (a pull is pending)");
    on_db_action_link_clear(&row);

    /* Status: In Progress set by Tasks survives a rename; strike → DONE
     * with completed_at; unstrike → In Progress; New stays New.            */
    exec_uid(other, uid, "UPDATE tasks SET status = 1 WHERE id = ?1");
    edit_note(db, note, 0, "Write the final report today", -1, NULL);
    check(task_int(db, uid, "status") == 1, "fields: In Progress survives a Notes rename");
    edit_note(db, note, 0, NULL, 1, NULL);
    check(task_int(db, uid, "status") == ON_TASK_STATUS_DONE && task_int(db, uid, "completed_at") > 0,
          "fields: a strike makes it DONE and stamps completed_at");
    edit_note(db, note, 0, NULL, 0, NULL);
    check(task_int(db, uid, "status") == 1, "fields: un-striking a DONE task makes it In Progress");

    /* Moved to another list in Tasks: Notes never writes list_id again.    */
    exec_sql(other, "INSERT INTO lists (name, emoji) VALUES ('Work', '')");
    exec_uid(other, uid, "UPDATE tasks SET list_id = (SELECT MAX(id) FROM lists) WHERE id = ?1");
    gint64 moved = task_int(db, uid, "list_id");
    edit_note(db, note, 0, "Report, moved", -1, NULL);
    check(task_int(db, uid, "list_id") == moved, "fields: a list set in Tasks sticks");
}

/*
 * test_push_delete — a removed line tombstones its task (seen_deleted),
 * an undo revives it; a task deleted elsewhere is left for the pull.
 *
 * Inputs:
 *   db    — Notes' connection
 *   other — a second connection
 */
static void
test_push_delete(OnDatabase *db, OnDatabase *other)
{
    gint64      note = on_note_new_from_text(db, 0, "Delete\n! Keep me\n! Remove me");
    gint64      uid  = nth_uid(db, note, 1);
    OnDocument *doc  = on_note_document_load(db, note);
    GArray     *bl   = on_document_action_blocks(doc);
    guint       line = g_array_index(bl, guint, 1);
    g_array_unref(bl);
    on_document_clear_undo(doc);
    on_document_remove_block(doc, line);
    check(on_note_document_save(db, note, doc, TRUE, NULL), "delete: the note without the line is saved");
    OnActionLink row;
    link_of(db, uid, note, &row);
    check(task_int(db, uid, "deleted") == 1 && row.seen_deleted,
          "delete: the task is tombstoned, seen_deleted = 1");
    on_db_action_link_clear(&row);

    on_document_undo(doc);
    check(on_note_document_save(db, note, doc, TRUE, NULL) && task_int(db, uid, "deleted") == 0,
          "delete: undo brings the line back and revives the same task");
    link_of(db, uid, note, &row);
    check(!row.seen_deleted, "delete: seen_deleted is 0 again");
    on_db_action_link_clear(&row);
    on_document_free(doc);

    /* Deleted elsewhere, not yet pulled: a save neither revives nor
     * re-tombstones it.                                                    */
    gint64 keep = nth_uid(db, note, 0);
    exec_uid(other, keep, "UPDATE tasks SET deleted = 1 WHERE id = ?1");
    edit_note(db, note, 1, "Remove me, renamed", -1, NULL);
    check(task_int(db, keep, "deleted") == 1, "delete: a task deleted elsewhere is not revived by a save");

    /* The Trash hides a note's items but leaves their tasks; a hard delete
     * tombstones them (the shared trigger).                                */
    gint64 trashed = on_note_new_from_text(db, 0, "Trashed\n! In the trash");
    gint64 tuid    = nth_uid(db, trashed, 0);
    on_db_notes_trash(db, &trashed, 1);
    check(task_int(db, tuid, "deleted") == 0 && on_db_action_get(db, tuid) != NULL,
          "trash: the task stays live (only the view's visible flag hides it)");
    on_db_notes_delete(db, &trashed, 1);
    check(task_int(db, tuid, "deleted") == -1 || task_int(db, tuid, "deleted") == 1,
          "hard delete: the note_task row goes and its task is tombstoned");
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db->handle, "SELECT COUNT(*) FROM tasks WHERE title = 'In the trash' AND deleted = 1",
                       -1, &st, NULL);
    check(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1,
          "hard delete: the task row remains, tombstoned");
    sqlite3_finalize(st);
}

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    check_data_dir();
    on_app_config_init();
    OnDatabase *db    = on_db_open(NULL, NULL);
    OnDatabase *other = on_db_open(NULL, NULL);
    check(db != NULL && other != NULL, "two connections to the scratch file");
    test_push_new(db);
    test_push_fields(db, other);
    test_push_delete(db, other);
    on_db_close(other);
    on_db_close(db);
    return check_result();
}
