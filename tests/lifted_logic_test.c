/* ===========================================================================
 * lifted_logic_test.c — the logic lifted out of GTK files into the core:
 * action_edit (from the editor window), search_job (from the search
 * window) and the lifecycle (from main.c), with the one-app-per-data-
 * directory lock between real processes
 * =========================================================================== */

#include "check.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "action_edit.h"
#include "app_lifecycle.h"
#include "export.h"
#include "search_job.h"
#include "serialize.h"

/* ── A stand-in for an open editor ──────────────────────────────────────── */

static OnDocument *live_doc;         /* the "open editor's" document         */
static gint64      live_note;        /* the note it is editing               */
static int         live_edits;       /* live_edited calls                    */
static int         flushes;          /* flush_editors calls                  */
static gboolean    flush_fails;      /* the "editor" cannot save             */

/*
 * fake_live_document — OnApp.live_document: only live_note is "open".
 *
 * Inputs:
 *   app     — the context (unused)
 *   note_id — the note asked about
 *
 * Output: the open document, or NULL.
 */
static OnDocument *
fake_live_document(OnApp *app, gint64 note_id)
{
    (void)app;
    return note_id == live_note ? live_doc : NULL;
}

/*
 * fake_live_edited — OnApp.live_edited: count.
 *
 * Inputs:
 *   app     — the context (unused)
 *   note_id — the note (unused)
 */
static void
fake_live_edited(OnApp *app, gint64 note_id)
{
    (void)app;
    (void)note_id;
    live_edits++;
}

/*
 * fake_flush — OnApp.flush_editors: count the calls made while the
 * database is still open (editors need it to save).
 *
 * Inputs:
 *   app — the context
 *
 * Output: FALSE while flush_fails is set (an editor that cannot save).
 */
static gboolean
fake_flush(OnApp *app)
{
    if (app->db != NULL) {
        flushes++;
    }
    return !flush_fails;
}

/*
 * line_of — the text of a stored note's block.
 *
 * Inputs:
 *   db    — the database
 *   note  — the note
 *   block — the block index
 *
 * Output: a new string; g_free it.
 */
static gchar *
line_of(OnDatabase *db, gint64 note, guint block)
{
    OnDocument *doc  = on_note_document_load(db, note);
    gchar      *text = g_strdup(on_document_block(doc, block)->text->text->str);
    on_document_free(doc);
    return text;
}

/*
 * blob_is — whether a note's stored blob is exactly these bytes.
 *
 * Inputs:
 *   db    — the database
 *   note  — the note
 *   bytes — the expected blob
 *   n     — its size
 *
 * Output: TRUE when the stored blob equals bytes.
 */
static gboolean
blob_is(OnDatabase *db, gint64 note, const guint8 *bytes, gsize n)
{
    guint8 *blob = NULL;
    gsize   len  = 0;
    gboolean same = on_db_note_load(db, note, &blob, &len) &&
                    len == n && memcmp(blob, bytes, n) == 0;
    g_free(blob);
    return same;
}

/*
 * test_unreadable_note — a note whose stored blob cannot be read (a newer
 * format, a damaged header) is never treated as an empty note: no consumer
 * hands out an empty document for it, and no command saves an edit over
 * it.  The stored bytes survive every attempt untouched.
 *
 * Inputs:
 *   app — the context, database open, no hooks
 */
static void
test_unreadable_note(OnApp *app)
{
    /* "BNBF", version 99: what a future build would write.                 */
    static const guint8 future[] = { 'B', 'N', 'B', 'F', 99, 0, 0, 0, 1, 0, 0, 0, 'x' };
    gint64 note = on_db_note_create(app->db, 0);
    check(on_db_note_save(app->db, note, "From the future", future, sizeof future, NULL),
          "unreadable: a newer-format blob is stored");

    check(on_note_document_load(app->db, note) == NULL,
          "unreadable: the document load fails rather than returning an empty note");
    check(on_note_document_load(app->db, 999999) == NULL,
          "unreadable: so does the load of a note that does not exist");

    check(on_note_text_cached(app->db, note) == NULL,
          "unreadable: its plain text is NULL, never an empty body");

    /* A note with an action item whose blob then became unreadable: an
     * edit of the item must not write a document over it.                  */
    gint64 with_item = on_note_new_from_text(app->db, 0, "Had an item\n! Item");
    GList *items = on_db_action_list_for_note(app->db, with_item);
    gint64 uid   = items != NULL ? ((OnActionItem *)items->data)->uid : 0;
    on_db_action_list_free(items);
    on_db_note_save(app->db, with_item, "From the future", future, sizeof future, NULL);
    check(uid != 0 && !on_action_edit_set_done(app, uid, TRUE),
          "unreadable: an action edit of its item fails");
    check(blob_is(app->db, note, future, sizeof future) &&
          blob_is(app->db, with_item, future, sizeof future),
          "unreadable: the stored bytes are untouched");

    /* A save to a note that is gone fails rather than "succeeding".        */
    gint64 gone = on_db_note_create(app->db, 0);
    on_db_notes_delete(app->db, &gone, 1);
    check(!on_db_note_save(app->db, gone, "Gone", future, sizeof future, NULL),
          "a save to a deleted note reports failure");
}

/*
 * test_export_names — an export never writes two notes to one file, nor
 * outside the folder it was given: titles differing only in case get
 * their own files (the volume is case-insensitive), and a folder named
 * ".." stays inside.
 *
 * Inputs:
 *   app      — the context, database open, no hooks
 *   data_dir — the test data directory
 */
static void
test_export_names(OnApp *app, const char *data_dir)
{
    gint64 up   = on_db_folder_create(app->db, 0, "..", ON_AI_MODE_NORMAL, NULL);
    on_note_new_from_text(app->db, up, "Plan\nfirst");
    on_note_new_from_text(app->db, up, "plan\nsecond");
    gchar *dest = g_build_filename(data_dir, "export", "inside", NULL);
    on_export_all(app, dest, ON_EXPORT_MARKDOWN, NULL);
    gchar *dir = g_build_filename(dest, "--", NULL);
    gint   files = 0;
    gboolean first = FALSE, second = FALSE;
    GDir  *d = g_dir_open(dir, 0, NULL);
    for (const gchar *name; d != NULL && (name = g_dir_read_name(d)) != NULL; ) {
        gchar *path = g_build_filename(dir, name, NULL);
        gchar *text = NULL;
        if (g_file_get_contents(path, &text, NULL, NULL)) {
            files++;
            first  = first  || strstr(text, "first") != NULL;
            second = second || strstr(text, "second") != NULL;
        }
        g_free(text);
        g_free(path);
    }
    if (d != NULL) g_dir_close(d);
    check(files == 2 && first && second,
          "export: titles differing only in case get a file each");
    gchar *outside = g_build_filename(data_dir, "export", "Plan.md", NULL);
    check(!g_file_test(outside, G_FILE_TEST_EXISTS), "export: a folder named '..' stays inside");
    g_free(outside);
    g_free(dir);
    g_free(dest);
}

/*
 * test_action_edit — headless (no hooks) and live (hooks installed).
 *
 * Inputs:
 *   app — the context, database open, no hooks
 */
static void
test_action_edit(OnApp *app)
{
    gint64 note = on_note_new_from_text(app->db, 0, "Plan\n! Call the bank due 2026-11-02\n! Second");
    GList *items = on_db_action_list_for_note(app->db, note);
    check(g_list_length(items) == 2, "two action lines became two task rows");
    gint64 uid = ((OnActionItem *)items->data)->uid;
    on_db_action_list_free(items);

    /* Headless: every hook NULL — the stored document is rewritten.        */
    check(on_action_edit_set_done(app, uid, TRUE), "headless: done is written");
    OnActionItem *row = on_db_action_get(app->db, uid);
    check(row != NULL && row->done, "headless: the task row is done");
    on_db_action_item_free(row);
    OnDocument *stored = on_note_document_load(app->db, note);
    gboolean done = FALSE;
    on_document_block_action(stored, 1, NULL, NULL, &done);
    check(done, "headless: the stored line is struck");
    on_document_free(stored);

    check(on_action_edit_set_due(app, uid, 0), "headless: due is cleared");
    gchar *line = line_of(app->db, note, 1);
    check(strcmp(line, "! Call the bank") == 0, "headless: the due suffix is gone");
    g_free(line);

    check(!on_action_edit_set_done(app, 987654, TRUE), "an unknown uid is refused");

    /* Live: the note is "open" — its document is edited in place, the
     * editor is told, and the STORED blob is left for the editor's save.   */
    live_note = note;
    live_doc  = on_note_document_load(app->db, note);
    app->live_document = fake_live_document;
    app->live_edited   = fake_live_edited;
    gchar     *before = line_of(app->db, note, 1);
    gint64     due    = check_midnight(2027, 3, 3);
    check(on_action_edit_set_due(app, uid, due), "live: a due date is written");
    check(live_edits == 1, "live: the editor is told exactly once");
    check(strcmp(on_document_block(live_doc, 1)->text->text->str,
                 "! Call the bank due 2027-03-03") == 0,
          "live: the open document shows the new due date");
    gchar *after = line_of(app->db, note, 1);
    check(strcmp(before, after) == 0, "live: the stored blob waits for the editor's own save");
    row = on_db_action_get(app->db, uid);
    check(row != NULL && row->due == due, "live: the task row was written first");
    on_db_action_item_free(row);
    g_free(before);
    g_free(after);

    /* A different, closed note still takes the stored path.                */
    gint64 other = on_note_new_from_text(app->db, 0, "Other\n! Elsewhere");
    items = on_db_action_list_for_note(app->db, other);
    gint64 other_uid = ((OnActionItem *)items->data)->uid;
    on_db_action_list_free(items);
    check(on_action_edit_set_done(app, other_uid, TRUE) && live_edits == 1,
          "live hooks installed, note closed: the stored path runs");

    on_document_free(live_doc);
    live_doc = NULL;
    app->live_document = NULL;
    app->live_edited   = NULL;
}

/* ── Search ─────────────────────────────────────────────────────────────── */

static bool search_done;             /* the job's done callback ran          */
static int  search_hits;             /* how many hits it reported            */
static char first_path[256];         /* the first hit's path                 */

/*
 * search_landed — on_search_done_func_t.
 *
 * Inputs:
 *   error — failure message, or NULL
 *   hits  — the matches
 *   n     — how many
 *   data  — unused
 */
static void
search_landed(const gchar *error, const on_search_hit_t *const *hits, size_t n,
              gpointer data)
{
    (void)data;
    search_hits = error != NULL ? -1 : (int)n;
    if (search_hits > 0) {
        snprintf(first_path, sizeof first_path, "%s", hits[0]->path);
    }
    search_done = true;
}

/*
 * test_search_job — a search on a worker, scoped and unscoped, and a
 * cancelled one.
 *
 * Inputs:
 *   app — the context, database open
 */
static void
test_search_job(OnApp *app)
{
    gint64 folder = on_db_folder_create(app->db, 0, "Recipes", 0, "");
    on_note_new_from_text(app->db, folder, "Soup\nleeks and potatoes");
    on_note_new_from_text(app->db, 0, "Shopping\npotatoes, onions");

    OnQuery *q = on_query_new("potatoes", FALSE, FALSE, NULL);
    on_search_job_start(app->db->path, q, FALSE, ON_SCOPE_FOLDER, 0, search_landed, NULL);
    check(check_pump(&search_done, 10.0), "an unscoped search reports on the main thread");
    check(search_hits == 2, "it finds both notes");

    search_done = false;
    q = on_query_new("potatoes", FALSE, FALSE, NULL);
    on_search_job_start(app->db->path, q, TRUE, ON_SCOPE_FOLDER, folder, search_landed, NULL);
    check(check_pump(&search_done, 10.0) && search_hits == 1, "a folder-scoped search finds one");
    check(strcmp(first_path, "Recipes/Soup") == 0, "a hit carries its display path");

    search_done = false;
    q = on_query_new("potatoes", FALSE, FALSE, NULL);
    on_search_job_t *job = on_search_job_start(app->db->path, q, FALSE, ON_SCOPE_FOLDER, 0,
                                           search_landed, NULL);
    on_search_job_cancel(job);
    check_pump(NULL, 0.5);
    check(!search_done, "a cancelled search never reports");
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

extern char **environ;

/*
 * spawn_self — run this test program again with one argument, its stdout
 * on a pipe.
 *
 * Inputs:
 *   self — this executable
 *   arg  — "--hold-lock" or "--probe-lock"
 *   out  — receives the read end of the child's stdout (close it)
 *
 * Output: the child's pid, or -1 when it could not be started.
 */
static pid_t
spawn_self(const char *self, const char *arg, int *out)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    pid_t pid  = -1;
    char *argv[] = { (char *)self, (char *)arg, NULL };
    if (posix_spawn(&pid, self, &actions, NULL, argv, environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    *out = fds[0];
    return pid;
}

/*
 * lock_child — the child side: "--hold-lock" takes the app lock, says so
 * on stdout and waits to be killed (a crash, as far as the lock knows);
 * "--probe-lock" exits 0 when the lock could be taken.
 *
 * Inputs:
 *   arg — the argument it was started with
 *
 * Output: the exit status (does not return for --hold-lock once held).
 */
static int
lock_child(const char *arg)
{
    on_app_lock_result_t got = on_app_lock_take();
    if (strcmp(arg, "--probe-lock") == 0) return got == on_app_lock_held ? 0 : 1;
    if (got != on_app_lock_held) return 1;
    fputs("held\n", stdout);
    fflush(stdout);
    for (;;) pause();
}

/*
 * test_app_lock — one app per data directory: a second process finds the
 * lock busy while the first lives, and free once it has died.
 *
 * Inputs:
 *   self — this executable
 */
static void
test_app_lock(const char *self)
{
    int   out    = -1;
    pid_t holder = spawn_self(self, "--hold-lock", &out);
    char  said[8] = "";
    check(holder > 0 && read(out, said, sizeof said - 1) > 0 && strncmp(said, "held", 4) == 0,
          "lock: another process takes the data directory's lock");
    close(out);
    check(on_app_lock_take() == on_app_lock_busy, "lock: this one then finds it busy");
    kill(holder, SIGKILL);
    waitpid(holder, NULL, 0);
    check(on_app_lock_take() == on_app_lock_held, "lock: free once the holder has died");
    check(on_app_lock_take() == on_app_lock_held, "lock: taking it again is harmless");
    int   status = -1;
    pid_t probe  = spawn_self(self, "--probe-lock", &out);
    close(out);
    waitpid(probe, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 1, "lock: and others find it busy");
}


/*
 * test_lifecycle — open policy and the shutdown order.
 *
 * Inputs:
 *   app      — a zeroed context
 *   data_dir — this test's NOTES_DATA_DIR
 */
static void
test_lifecycle(OnApp *app, const char *data_dir)
{
    gchar *path = on_app_db_path(NULL);
    check(path != NULL && strcmp(path, getenv(SHARED_DB_ENV_VAR)) == 0,
          "the database is the one $CAMBIO_SHARED_DB names");
    g_free(path);

    /* An old notes.db (no stamps, tables) is refused untouched, with the
     * module's reason — never opened, never "upgraded".                    */
    gchar   *old = g_build_filename(data_dir, "old-notes.db", NULL);
    sqlite3 *sq  = NULL;
    sqlite3_open(old, &sq);
    sqlite3_exec(sq, "CREATE TABLE notes (id INTEGER PRIMARY KEY)", NULL, NULL, NULL);
    sqlite3_close(sq);
    gchar  *before = NULL, *after = NULL, *why = NULL;
    gsize   n_before = 0, n_after = 0;
    g_file_get_contents(old, &before, &n_before, NULL);
    OnDatabase *refused = on_db_open(old, &why);
    g_file_get_contents(old, &after, &n_after, NULL);
    check(refused == NULL && why != NULL && n_before == n_after &&
          memcmp(before, after, n_before) == 0,
          "a file that is not a shared database is refused, byte for byte untouched");
    g_free(before);
    g_free(after);
    g_free(why);
    g_free(old);

    check(on_app_open_db(app, NULL) && app->db != NULL, "the default database opens");
    sqlite3_stmt *mode = NULL;
    sqlite3_prepare_v2(app->db->handle, "PRAGMA journal_mode", -1, &mode, NULL);
    gboolean wal = mode != NULL && sqlite3_step(mode) == SQLITE_ROW &&
                   strcmp((const char *)sqlite3_column_text(mode, 0), "wal") == 0;
    sqlite3_finalize(mode);
    check(wal && strcmp(app->db->path, getenv(SHARED_DB_ENV_VAR)) == 0,
          "it is the shared file, in WAL mode through the shared module");

    /* Set as Default while $CAMBIO_SHARED_DB fixes the file: the module
     * refuses, the reason comes back, and the app is on its old file.      */
    gchar *other = g_build_filename(data_dir, "other.db", NULL);
    gchar *was   = g_strdup(app->db->path);
    why = NULL;
    check(on_app_switch_db(app, other, TRUE, &why) == ON_SWITCH_FAILED && why != NULL &&
          app->db != NULL && strcmp(app->db->path, was) == 0,
          "Set as Default under $CAMBIO_SHARED_DB is refused with a reason; the old file stays open");
    g_free(why);
    check(on_app_switch_db(app, other, FALSE, NULL) == ON_SWITCH_DONE &&
          strcmp(app->db->path, other) == 0, "Session Only opens (creates) another shared file");
    check(on_app_switch_db(app, was, FALSE, NULL) == ON_SWITCH_DONE, "and back");
    g_free(other);
    g_free(was);
}

/*
 * main — run the test, or be one of its lock children.
 *
 * Inputs:
 *   argc, argv — "--hold-lock" / "--probe-lock" select a child (lock_child)
 *
 * Output: 0 when every check held.
 */
int
main(int argc, char **argv)
{
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof data_dir, "%s", check_data_dir());
    on_app_config_init();
    if (argc == 2) {
        return lock_child(argv[1]);
    }

    static OnApp app;
    test_app_lock(argv[0]);
    test_lifecycle(&app, data_dir);
    test_action_edit(&app);
    test_unreadable_note(&app);
    test_export_names(&app, data_dir);
    test_search_job(&app);

    /* Shutdown: editors flush while the database is still open, then it
     * closes; a second call is harmless.                                   */
    app.flush_editors = fake_flush;
    flush_fails = TRUE;
    check(!on_app_shutdown(&app) && app.db != NULL,
          "shutdown stops, the database still open, when an editor cannot save");
    flush_fails = FALSE;
    flushes = 0;
    check(on_app_shutdown(&app), "shutdown completes once the editors have saved");
    check(flushes == 1 && app.db == NULL, "shutdown flushes editors, then closes the database");
    on_app_shutdown(&app);
    check(app.db == NULL, "a second shutdown is harmless");

    int   out    = -1, status = -1;
    pid_t probe  = spawn_self(argv[0], "--probe-lock", &out);
    close(out);
    waitpid(probe, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "shutdown releases the app lock");
    return check_result();
}
