/* ===========================================================================
 * lifted_logic_test.c — the logic lifted out of GTK files into the core:
 * action_edit (from the editor window), search_job (from the search
 * window) and the lifecycle (from main.c)
 * =========================================================================== */

#include "check.h"

#include "action_edit.h"
#include "app_lifecycle.h"
#include "cli.h"
#include "search_job.h"
#include "serialize.h"

/* ── A stand-in for an open editor ──────────────────────────────────────── */

static OnDocument *live_doc;         /* the "open editor's" document         */
static gint64      live_note;        /* the note it is editing               */
static int         live_edits;       /* live_edited calls                    */
static int         flushes;          /* flush_editors calls                  */

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
 */
static void
fake_flush(OnApp *app)
{
    if (app->db != NULL) {
        flushes++;
    }
}

/*
 * new_note — a note with the given text, saved through THE save.
 *
 * Inputs:
 *   db     — the database
 *   folder — its folder (0 = root)
 *   text   — the content, one block per line
 *
 * Output: the note's id.
 */
static gint64
new_note(OnDatabase *db, gint64 folder, const char *text)
{
    gint64      id  = on_db_note_create(db, folder);
    OnDocument *doc = on_document_from_text(text);
    on_note_document_save(db, id, doc, TRUE, NULL);
    on_document_free(doc);
    return id;
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
 * cli — run one CLI command in-process, as the app runs a remote one.
 *
 * Inputs:
 *   app  — the context
 *   argc — how many words
 *   argv — the words, argv[0] the program
 *
 * Output: the command's exit code.
 */
static int
cli(OnApp *app, int argc, char **argv)
{
    fflush(stdout);
    return on_cli_dispatch(app, argc, argv);
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

    gchar id[32];
    g_snprintf(id, sizeof id, "%" G_GINT64_FORMAT, note);
    char *append[] = { "notes", "note", "append", id, "added", NULL };
    check(cli(app, 5, append) == 2, "unreadable: note append fails");
    char *tag[] = { "notes", "note", "tag", id, "later", NULL };
    check(cli(app, 5, tag) == 2, "unreadable: note tag fails");
    char *cat[] = { "notes", "note", "cat", id, NULL };
    check(cli(app, 4, cat) == 2, "unreadable: note cat fails instead of printing an empty body");
    char *cat_md[] = { "notes", "note", "cat", id, "--md", NULL };
    check(cli(app, 5, cat_md) == 2, "unreadable: and so does note cat --md");
    check(blob_is(app->db, note, future, sizeof future),
          "unreadable: the stored bytes are untouched");

    /* A save to a note that is gone fails rather than "succeeding".        */
    gint64 gone = on_db_note_create(app->db, 0);
    on_db_notes_delete(app->db, &gone, 1);
    check(!on_db_note_save(app->db, gone, "Gone", future, sizeof future, NULL),
          "a save to a deleted note reports failure");
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
    gint64 note = new_note(app->db, 0, "Plan\n! Call the bank due 2026-11-02\n! Second");
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

    check(on_action_edit_set_text(app, uid, "Call the building society"),
          "headless: rename is written");
    gchar *line = line_of(app->db, note, 1);
    check(strcmp(line, "! Call the building society due 2026-11-02") == 0,
          "headless: the line keeps its '!' and its due date");
    g_free(line);

    check(on_action_edit_set_due(app, uid, 0), "headless: due is cleared");
    line = line_of(app->db, note, 1);
    check(strcmp(line, "! Call the building society") == 0, "headless: the due suffix is gone");
    g_free(line);

    check(!on_action_edit_set_done(app, 987654, TRUE), "an unknown uid is refused");

    /* Live: the note is "open" — its document is edited in place, the
     * editor is told, and the STORED blob is left for the editor's save.   */
    live_note = note;
    live_doc  = on_note_document_load(app->db, note);
    app->live_document = fake_live_document;
    app->live_edited   = fake_live_edited;
    gchar *before = line_of(app->db, note, 1);
    check(on_action_edit_set_text(app, uid, "Live rename"), "live: rename is written");
    check(live_edits == 1, "live: the editor is told exactly once");
    check(strcmp(on_document_block(live_doc, 1)->text->text->str, "! Live rename") == 0,
          "live: the open document shows the new text");
    gchar *after = line_of(app->db, note, 1);
    check(strcmp(before, after) == 0, "live: the stored blob waits for the editor's own save");
    row = on_db_action_get(app->db, uid);
    check(row != NULL && strcmp(row->text, "Live rename") == 0, "live: the task row was written first");
    on_db_action_item_free(row);
    g_free(before);
    g_free(after);

    /* A different, closed note still takes the stored path.                */
    gint64 other = new_note(app->db, 0, "Other\n! Elsewhere");
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
    new_note(app->db, folder, "Soup\nleeks and potatoes");
    new_note(app->db, 0, "Shopping\npotatoes, onions");

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
    gchar *path = on_app_db_path(app);
    char expect[PATH_MAX];
    snprintf(expect, sizeof expect, "%s/notes.db", data_dir);
    check(strcmp(path, expect) == 0, "with no db_dir the database is in the data directory");
    g_free(path);

    app->db_dir = g_build_filename(data_dir, "elsewhere", "missing", NULL);
    check(!on_app_open_db(app) && app->db == NULL,
          "an unopenable configured database is a failure, never a fallback");
    g_clear_pointer(&app->db_dir, g_free);

    check(on_app_open_db(app) && app->db != NULL, "the default database opens");
}

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof data_dir, "%s", check_data_dir());
    on_app_config_init();

    static OnApp app;
    test_lifecycle(&app, data_dir);
    test_action_edit(&app);
    test_unreadable_note(&app);
    test_search_job(&app);

    /* Shutdown: editors flush while the database is still open, then it
     * closes; a second call is harmless.                                   */
    app.flush_editors = fake_flush;
    on_app_shutdown(&app);
    check(flushes == 1 && app.db == NULL, "shutdown flushes editors, then closes the database");
    on_app_shutdown(&app);
    check(app.db == NULL, "a second shutdown is harmless");
    return check_result();
}
