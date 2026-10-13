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

#include "action_sync.h"
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

/* ── The pull ───────────────────────────────────────────────────────────── */

/*
 * first_line — the text of a stored note's block 0 (its title line), and
 * whether it is a struck action line.
 *
 * Inputs:
 *   db   — a connection
 *   note — the note
 *   done — receives the done state (may be NULL)
 *
 * Output: a new string; g_free it.
 */
static gchar *
first_line(OnDatabase *db, gint64 note, gboolean *done)
{
    OnDocument *doc = on_note_document_load(db, note);
    if (doc == NULL) return g_strdup("");
    gchar *text = g_strdup(on_document_block(doc, 0)->text->text->str);
    if (done != NULL) {
        *done = FALSE;
        on_document_block_action(doc, 0, NULL, NULL, done);
    }
    on_document_free(doc);
    return text;
}

/*
 * pending_count — how many notes the pull still has work in.
 *
 * Inputs:
 *   db — a connection
 *
 * Output: the count.
 */
static guint
pending_count(OnDatabase *db)
{
    GArray *ids = on_db_action_pending(db, 0);
    guint   n   = ids->len;
    g_array_unref(ids);
    return n;
}

/*
 * body_has — whether a note's searchable text holds a string.
 *
 * Inputs:
 *   db     — a connection
 *   note   — the note
 *   needle — the string
 *
 * Output: TRUE when it does.
 */
static gboolean
body_has(OnDatabase *db, gint64 note, const char *needle)
{
    gchar   *body = on_db_note_body_text(db, note);
    gboolean has  = body != NULL && strstr(body, needle) != NULL;
    g_free(body);
    return has;
}

/*
 * note_title_is — whether a note's stored title is `title`.
 *
 * Inputs:
 *   db    — a connection
 *   note  — the note
 *   title — the expected title
 *
 * Output: TRUE when it is.
 */
static gboolean
note_title_is(OnDatabase *db, gint64 note, const char *title)
{
    OnNoteMeta *m  = on_db_note_get(db, note);
    gboolean    is = m != NULL && strcmp(m->title, title) == 0;
    on_db_note_meta_free(m);
    return is;
}

/*
 * pane_text — the Action Items listing's text for a uid ("" when it is not
 * listed: deleted, or its note trashed).
 *
 * Inputs:
 *   db  — a connection
 *   uid — the item
 *
 * Output: a new string; g_free it.
 */
static gchar *
pane_text(OnDatabase *db, gint64 uid)
{
    gchar *text = NULL;
    GList *all  = on_db_action_list(db);
    for (GList *l = all; l != NULL; l = l->next) {
        OnActionItem *it = l->data;
        if (it->uid == uid) text = g_strdup(it->text);
    }
    on_db_action_list_free(all);
    return text != NULL ? text : g_strdup("");
}

/*
 * outside_writes — what another app does to the four items of the pull
 * test (rename / complete / re-date / delete), on its own connection.
 *
 * Inputs:
 *   other — the other connection
 *   uids  — the four items
 */
static void
outside_writes(OnDatabase *other, const gint64 uids[4])
{
    exec_uid(other, uids[0], "UPDATE tasks SET title = 'Renamed in Tasks', updated_at = 1 WHERE id = ?1");
    exec_uid(other, uids[1], "UPDATE tasks SET status = 2, completed_at = 1 WHERE id = ?1");
    gchar *redate = g_strdup_printf("UPDATE tasks SET due = %lld WHERE id = ?1",
                                    (long long)check_midnight(2027, 4, 9));
    exec_uid(other, uids[2], redate);
    g_free(redate);
    exec_uid(other, uids[3], "UPDATE tasks SET deleted = 1 WHERE id = ?1");
}

/*
 * make_pull_notes — four closed notes, each one '!' line as its FIRST line
 * (so the note's title is the line), for the four outside changes.
 *
 * Inputs:
 *   db    — Notes' connection
 *   notes — receives the four note ids
 *   uids  — receives their items' uids
 */
static void
make_pull_notes(OnDatabase *db, gint64 notes[4], gint64 uids[4])
{
    static const char *const text[4] = {
        "! Rename me due 2026-11-02\nbody one",
        "! Complete me\nbody two",
        "! Re-date me due 2026-11-03\nbody three",
        "!! Delete me due 2026-11-04\nbody four",
    };
    for (int i = 0; i < 4; i++) {
        notes[i] = on_note_new_from_text(db, 0, text[i]);
        uids[i]  = nth_uid(db, notes[i], 0);
    }
}

/*
 * check_pulled — the four outside changes are in the closed notes: blob,
 * title, body_text, the Action Items listing, the baseline.
 *
 * Inputs:
 *   db    — Notes' connection
 *   notes — the notes
 *   uids  — their items
 *   how   — "connection" or "process", for the log
 */
static void
check_pulled(OnDatabase *db, const gint64 notes[4], const gint64 uids[4], const char *how)
{
    char     what[160];
    gboolean done = FALSE;
    gchar   *line = first_line(db, notes[0], NULL);
    gchar   *pane = pane_text(db, uids[0]);
    snprintf(what, sizeof what, "pull (%s): a rename reaches the closed note's line, title, "
             "body text and the pane", how);
    check(strcmp(line, "! Renamed in Tasks due 2026-11-02") == 0 &&
          note_title_is(db, notes[0], "! Renamed in Tasks due 2026-11-02") &&
          body_has(db, notes[0], "Renamed in Tasks") && strcmp(pane, "Renamed in Tasks") == 0, what);
    g_free(line);
    g_free(pane);

    line = first_line(db, notes[1], &done);
    snprintf(what, sizeof what, "pull (%s): a completion strikes the line", how);
    check(done && strcmp(line, "! Complete me") == 0, what);
    g_free(line);

    line = first_line(db, notes[2], NULL);
    snprintf(what, sizeof what, "pull (%s): a new due date rewrites the suffix", how);
    check(strcmp(line, "! Re-date me due 2027-04-09") == 0 && body_has(db, notes[2], "due 2027-04-09"),
          what);
    g_free(line);

    line = first_line(db, notes[3], NULL);
    pane = pane_text(db, uids[3]);
    OnActionLink row;
    snprintf(what, sizeof what, "pull (%s): a deleted task UNMARKS its line (every leading '!' "
             "and its spaces go, the text and due suffix stay), its link goes", how);
    check(strcmp(line, "Delete me due 2026-11-04") == 0 && !link_of(db, uids[3], notes[3], &row) &&
          strcmp(pane, "") == 0 && note_title_is(db, notes[3], "Delete me due 2026-11-04") &&
          nth_uid(db, notes[3], 0) == 0, what);
    g_free(line);
    g_free(pane);

    for (int i = 0; i < 3; i++) {
        link_of(db, uids[i], notes[i], &row);
        gchar *title = task_title(db, uids[i]);
        gboolean agreed = strcmp(row.seen_title, title) == 0 &&
                          row.seen_done == (task_int(db, uids[i], "status") == 2) &&
                          row.seen_due == task_int(db, uids[i], "due");
        g_free(title);
        on_db_action_link_clear(&row);
        snprintf(what, sizeof what, "pull (%s): item %d's seen_* is the task's values", how, i);
        check(agreed, what);
    }
    snprintf(what, sizeof what, "pull (%s): nothing is left pending", how);
    check(pending_count(db) == 0, what);
}

/*
 * test_pull_connection — another connection renames, completes, re-dates
 * and deletes; one poll brings all four into the closed notes, and the
 * pull's own writes trigger no second pull.
 *
 * Inputs:
 *   app   — headless context on Notes' connection
 *   other — a second connection
 */
static void
test_pull_connection(OnApp *app, OnDatabase *other)
{
    gint64 notes[4], uids[4];
    make_pull_notes(app->db, notes, uids);
    on_action_sync_poll(app);        /* drop the setup's own state          */
    outside_writes(other, uids);
    check(pending_count(app->db) == 4, "pull: four notes have work");
    check(on_action_sync_poll(app), "pull: one poll sees the outside commit and pulls");
    check_pulled(app->db, notes, uids, "connection");
    OnNoteMeta *before = on_db_note_get(app->db, notes[0]);
    check(!on_action_sync_poll(app), "pull: its own saves do not trigger another pull");
    on_action_sync_run(app);
    OnNoteMeta *after = on_db_note_get(app->db, notes[0]);
    check(before->updated_at == after->updated_at, "pull: a pull with nothing to do writes nothing");
    on_db_note_meta_free(before);
    on_db_note_meta_free(after);
}

/*
 * test_pull_process — the same four changes made by ANOTHER PROCESS (a
 * fork with its own connection, as Tasks is), then one poll.
 *
 * Inputs:
 *   app — headless context on Notes' connection
 */
static void
test_pull_process(OnApp *app)
{
    gint64 notes[4], uids[4];
    make_pull_notes(app->db, notes, uids);
    on_action_sync_poll(app);
    gchar *path = g_strdup(app->db->path);
    pid_t  pid  = fork();
    if (pid == 0) {
        OnDatabase *mine = on_db_open(path, NULL);
        if (mine == NULL) _exit(3);
        outside_writes(mine, uids);
        on_db_close(mine);
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    g_free(path);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "pull (process): the other process wrote");
    check(on_action_sync_poll(app), "pull (process): one poll sees it");
    check_pulled(app->db, notes, uids, "process");
}

/*
 * test_pull_refused — a title the line cannot take (empty; ending in what
 * reads as a due date) leaves the line, moves seen_title, and the same
 * save pushes the line's text back to the task: no row stays pending.
 *
 * Inputs:
 *   app   — headless context
 *   other — a second connection
 */
static void
test_pull_refused(OnApp *app, OnDatabase *other)
{
    gint64 note = on_note_new_from_text(app->db, 0, "Refused\n! Keep my text\n! Mine too");
    gint64 a    = nth_uid(app->db, note, 0);
    gint64 b    = nth_uid(app->db, note, 1);
    on_action_sync_poll(app);
    exec_uid(other, a, "UPDATE tasks SET title = '' WHERE id = ?1");
    exec_uid(other, b, "UPDATE tasks SET title = 'Looks dated due 2026-01-01' WHERE id = ?1");
    on_action_sync_poll(app);
    gchar *ta = task_title(app->db, a), *tb = task_title(app->db, b);
    check(strcmp(ta, "Keep my text") == 0 && strcmp(tb, "Mine too") == 0 && pending_count(app->db) == 0,
          "pull: a title the line refuses is written back from the line; nothing stays pending");
    g_free(ta);
    g_free(tb);
}

/*
 * test_concurrent — no lost updates between Notes and Tasks: Tasks changes
 * priority and due while Notes edits the title, in both orders of save
 * and pull; every change survives in the task and in the line.
 *
 * Inputs:
 *   app   — headless context
 *   other — a second connection (Tasks)
 */
static void
test_concurrent(OnApp *app, OnDatabase *other)
{
    gint64 note = on_note_new_from_text(app->db, 0, "Concurrent\n! Plan the trip due 2026-11-10");
    gint64 uid  = nth_uid(app->db, note, 0);
    on_action_sync_poll(app);

    /* Tasks first, Notes saves its title before pulling.                    */
    gchar *sql = g_strdup_printf("UPDATE tasks SET priority = 2, due = %lld WHERE id = ?1",
                                 (long long)check_midnight(2026, 12, 24));
    exec_uid(other, uid, sql);
    g_free(sql);
    check(edit_note(app->db, note, 0, "Plan the whole trip", -1, NULL),
          "concurrent: Notes saves a title edit made before it pulled");
    on_action_sync_poll(app);
    gchar *title = task_title(app->db, uid);
    gchar *line  = first_line(app->db, note, NULL);
    OnDocument *doc = on_note_document_load(app->db, note);
    gchar *text = NULL;
    gint64 due  = 0;
    GArray *bl  = on_document_action_blocks(doc);
    on_document_block_action(doc, g_array_index(bl, guint, 0), &text, &due, NULL);
    g_array_unref(bl);
    on_document_free(doc);
    check(strcmp(title, "Plan the whole trip") == 0 && task_int(app->db, uid, "priority") == 2 &&
          task_int(app->db, uid, "due") == check_midnight(2026, 12, 24) &&
          strcmp(text, "Plan the whole trip") == 0 && due == check_midnight(2026, 12, 24),
          "concurrent: Notes' title, Tasks' priority and due all survive, in the task and the line");
    g_free(title);
    g_free(line);
    g_free(text);

    /* Tasks re-dates again; Notes pulls first, then edits the title.        */
    sql = g_strdup_printf("UPDATE tasks SET due = %lld WHERE id = ?1", (long long)check_midnight(2027, 1, 2));
    exec_uid(other, uid, sql);
    g_free(sql);
    on_action_sync_poll(app);
    edit_note(app->db, note, 0, "Plan the trip, finally", -1, NULL);
    title = task_title(app->db, uid);
    check(strcmp(title, "Plan the trip, finally") == 0 &&
          task_int(app->db, uid, "due") == check_midnight(2027, 1, 2) &&
          task_int(app->db, uid, "priority") == 2 && pending_count(app->db) == 0,
          "concurrent: a pulled due date and a later Notes title both survive");
    g_free(title);
}

/*
 * test_restored_elsewhere — Notes removed a line (tombstone,
 * seen_deleted = 1) and Tasks then restored the task: Notes leaves it
 * alone (no pull work, no new tombstone on the next save).
 *
 * Inputs:
 *   app   — headless context
 *   other — a second connection
 */
static void
test_restored_elsewhere(OnApp *app, OnDatabase *other)
{
    gint64 note = on_note_new_from_text(app->db, 0, "Restore\n! Keep\n! Restored in Tasks");
    gint64 uid  = nth_uid(app->db, note, 1);
    OnDocument *doc = on_note_document_load(app->db, note);
    GArray     *bl  = on_document_action_blocks(doc);
    on_document_remove_block(doc, g_array_index(bl, guint, 1));
    g_array_unref(bl);
    on_note_document_save(app->db, note, doc, TRUE, NULL);
    exec_uid(other, uid, "UPDATE tasks SET deleted = 0 WHERE id = ?1");
    check(pending_count(app->db) == 0, "restored elsewhere: no pull work");
    on_action_sync_poll(app);
    edit_note(app->db, note, 0, "Keep, edited", -1, NULL);
    on_document_free(doc);
    check(task_int(app->db, uid, "deleted") == 0, "restored elsewhere: a later save leaves it live");
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

    OnApp app = { 0 };               /* headless: every hook NULL           */
    app.db = db;
    test_pull_connection(&app, other);
    test_pull_process(&app);
    test_pull_refused(&app, other);
    test_concurrent(&app, other);
    test_restored_elsewhere(&app, other);
    on_db_close(other);
    on_db_close(db);
    return check_result();
}
