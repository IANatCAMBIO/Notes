/* ===========================================================================
 * core_api_test.c — the behaviours the command-line tool's golden suite
 * pinned, checked through the core API itself
 *
 * The `notes` tool was a thin layer over db.h, serialize.h, document.h,
 * action_edit.h, search_query.h and export.h, and its golden suite
 * (tests/cli_golden.sh, removed with the tool) was in effect the
 * end-to-end test of that core.  What it pinned that lives in the core is
 * checked here directly: notes from text, the '!' action-line rules and
 * the task rows they are (uid addressing, due dates ISO and M/D/YY,
 * strike, renames from the row keeping '!', done state and due), tags
 * (the Tags field and the #tags of the text, emoji, favorites, the two
 * scopes), pins, the Trash, search, the whole-database counts,
 * Markdown and HTML export, embedded images and backups.  Output
 * formatting was the tool's own and went with it.
 *
 * One database for the whole program, in this test's NOTES_DATA_DIR.
 * =========================================================================== */

#include "check.h"

#include <unistd.h>

#include "action_edit.h"
#include "action_sync.h"
#include "app_lifecycle.h"
#include "export.h"
#include "image_io.h"
#include "search_query.h"
#include "serialize.h"


/*
 * note_tags_are — a note's tags with their sources, "name:FT" by name (F
 * for the Tags field, T for the text, '-' for neither), joined by ','.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *
 * Output: a new string; g_free it.
 */
static gchar *
note_tags_are(OnDatabase *db, gint64 note)
{
    GString *out  = g_string_new(NULL);
    GList   *tags = on_db_note_tags(db, note);
    for (GList *l = tags; l != NULL; l = l->next) {
        const OnTag *t = l->data;
        g_string_append_printf(out, "%s%s:%c%c", out->len > 0 ? "," : "", t->name,
                               t->in_field ? 'F' : '-', t->in_text ? 'T' : '-');
    }
    on_db_tag_list_free(tags);
    return g_string_free(out, FALSE);
}

/*
 * tag_listed — is a tag of this name among on_db_tag_list(scope)?
 *
 * Inputs:
 *   db    — the database
 *   scope — ON_TAGS_CARRIED or ON_TAGS_ALL
 *   name  — the tag's name (exact spelling)
 *
 * Output: TRUE when listed.
 */
static gboolean
tag_listed(OnDatabase *db, OnTagScope scope, const char *name)
{
    gboolean found = FALSE;
    GList   *tags  = on_db_tag_list(db, scope);
    for (GList *l = tags; l != NULL; l = l->next) {
        if (strcmp(((OnTag *)l->data)->name, name) == 0) found = TRUE;
    }
    on_db_tag_list_free(tags);
    return found;
}

/*
 * list_has_note — whether a note list holds a note.
 *
 * Inputs:
 *   notes — a GList of OnNoteMeta* (freed here)
 *   id    — the note
 *
 * Output: TRUE when it does.
 */
static gboolean
list_has_note(GList *notes, gint64 id)
{
    gboolean found = FALSE;
    for (GList *l = notes; l != NULL; l = l->next) {
        if (((OnNoteMeta *)l->data)->id == id) found = TRUE;
    }
    on_db_note_list_free(notes);
    return found;
}

/*
 * note_title_is — whether a note exists with exactly this title.
 *
 * Inputs:
 *   db    — the database
 *   id    — the note
 *   title — the expected title
 *
 * Output: TRUE when it matches.
 */
static gboolean
note_title_is(OnDatabase *db, gint64 id, const char *title)
{
    OnNoteMeta *m  = on_db_note_get(db, id);
    gboolean    ok = m != NULL && strcmp(m->title, title) == 0;
    on_db_note_meta_free(m);
    return ok;
}

/*
 * items_of — a note's action items.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *
 * Output: the GList of OnActionItem* (on_db_action_list_free it).
 */
static GList *
items_of(OnDatabase *db, gint64 note)
{
    return on_db_action_list_for_note(db, note);
}

/*
 * nth_uid — the uid of a note's item at position `ord`.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *   ord  — the position among its action lines
 *
 * Output: the uid, or 0 when there is no such item.
 */
static gint64
nth_uid(OnDatabase *db, gint64 note, guint ord)
{
    GList        *items = items_of(db, note);
    OnActionItem *it    = g_list_nth_data(items, ord);
    gint64        uid   = it != NULL ? it->uid : 0;
    on_db_action_list_free(items);
    return uid;
}

/*
 * action_list_has_note — whether the cross-note action listing holds any
 * item of a note.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *
 * Output: TRUE when it does.
 */
static gboolean
action_list_has_note(OnDatabase *db, gint64 note)
{
    gboolean found = FALSE;
    GList   *all   = on_db_action_list(db);
    for (GList *l = all; l != NULL; l = l->next) {
        if (((OnActionItem *)l->data)->note_id == note) found = TRUE;
    }
    on_db_action_list_free(all);
    return found;
}

/*
 * task_status — the Tasks-side status of an item's task row.
 *
 * Inputs:
 *   db        — the database
 *   uid       — the item
 *   completed — receives completed_at (may be NULL)
 *
 * Output: the status (SHARED_DB_TASK_STATUS_*), or -1 when no task
 *         row is linked to the uid.
 */
static int
task_status(OnDatabase *db, gint64 uid, gint64 *completed)
{
    sqlite3_stmt *st = NULL;
    int           status = -1;
    sqlite3_prepare_v2(db->handle,
                       "SELECT t.status, t.completed_at FROM tasks t "
                       "JOIN note_task n ON n.task_id = t.id WHERE n.uid = ?",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, uid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        status = sqlite3_column_int(st, 0);
        if (completed != NULL) *completed = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return status;
}

/*
 * stored_line — the text of a stored note's block, and whether that block
 * is a struck (done) action line.
 *
 * Inputs:
 *   db    — the database
 *   note  — the note
 *   block — the block index
 *   done  — receives the line's done state (may be NULL)
 *
 * Output: a new string; g_free it.  "" when the note cannot be read.
 */
static gchar *
stored_line(OnDatabase *db, gint64 note, guint block, gboolean *done)
{
    OnDocument *doc = on_note_document_load(db, note);
    if (doc == NULL) return g_strdup("");
    gchar *text = g_strdup(on_document_block(doc, block)->text->text->str);
    if (done != NULL) {
        *done = FALSE;
        on_document_block_action(doc, block, NULL, NULL, done);
    }
    on_document_free(doc);
    return text;
}

/*
 * add_tags — put #tag tokens on a note's tag line, space-separated, each
 * carrying ON_FMT_TAG (on_document_append_tag: where the editor's tag
 * capture leaves them).
 *
 * Inputs:
 *   doc   — the document, loaded (it has its tag line)
 *   names — NULL-terminated tag words, without '#'
 */
static void
add_tags(OnDocument *doc, const char *const *names)
{
    for (const char *const *n = names; *n != NULL; n++) {
        on_document_append_tag(doc, *n);
    }
}

/*
 * save_with_tags — THE save plus the tag links the editor writes after it.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *   doc  — its document
 *
 * Output: TRUE when both were written.
 */
static gboolean
save_with_tags(OnDatabase *db, gint64 note, OnDocument *doc)
{
    return on_note_document_save(db, note, doc, TRUE, NULL) &&
           on_note_document_save_tags(db, note, doc);
}

/*
 * tag_count — how many notes a tag labels, from the per-tag count map.
 *
 * Inputs:
 *   db   — the database
 *   name — the tag
 *
 * Output: the count; -1 when no such tag exists.
 */
static int
tag_count(OnDatabase *db, const char *name)
{
    int         n      = -1;
    GHashTable *counts = on_db_tag_count_map(db);
    GList      *tags   = on_db_tag_list(db, ON_TAGS_ALL);
    for (GList *l = tags; l != NULL; l = l->next) {
        OnTag *t = l->data;
        if (strcmp(t->name, name) == 0) {
            n = GPOINTER_TO_INT(g_hash_table_lookup(counts, &t->id));
        }
    }
    on_db_tag_list_free(tags);
    g_hash_table_destroy(counts);
    return n;
}

/*
 * file_png — a fixture image file as the PNG bytes a note stores.
 *
 * Inputs:
 *   name — the file under tests/fixtures (tests run from the repository
 *          root)
 *
 * Output: the bytes (g_bytes_unref), or NULL when it cannot be read.
 */
static GBytes *
file_png(const char *name)
{
    gchar   *path = g_build_filename("tests", "fixtures", name, NULL);
    size_t   n    = 0;
    uint8_t *png  = on_image_file_to_png(path, &n);
    g_free(path);
    return png != NULL ? g_bytes_new_with_free_func(png, n, free, png) : NULL;
}

/* ── Notes ──────────────────────────────────────────────────────────────── */

/*
 * test_notes — a note from plain text (with a tag in its Tags field),
 * replacing its content, its modified time and pinning.
 *
 * Inputs:
 *   db — the database
 */
static void
test_notes(OnDatabase *db)
{
    static const char text[] =
        "Stride rollout\n\nSensor rollout.\nTabs\there and \"quotes\" and a \\ backslash.";
    gint64 note   = on_note_new_from_text(db, "Notes test", text);
    check(note != 0, "notes: a note is made from plain text");
    check(note_title_is(db, note, "Stride rollout"), "notes: its first line is its title");
    gchar *body = on_note_text_cached(db, note);
    gchar *want = g_strconcat("#NotesTest\n", text, NULL);
    check(body != NULL && strcmp(body, want) == 0,
          "notes: its plain text reads back byte for byte (tabs, quotes, backslash), "
          "the tag asked for on its tag line");
    g_free(want);
    g_free(body);
    gchar *tags = note_tags_are(db, note);
    check(strcmp(tags, "NotesTest:-T") == 0, "notes: it carries the tag asked for, as a #word");
    g_free(tags);

    gint64 blank = on_note_new_from_text(db, NULL, "");
    check(note_title_is(db, blank, SHARED_DB_NOTE_DEFAULT_TITLE),
          "notes: an empty note takes the default title");

    /* Replacing the content (`note set`): the old tags and action items
     * go with the old text.                                               */
    OnDocument *doc = on_document_from_text("Old title\n! Old item");
    on_document_add_tag_line(doc);
    add_tags(doc, (const char *[]){ "oldtag", NULL });
    check(save_with_tags(db, note, doc), "set: the old content with a tag and an item is saved");
    on_document_free(doc);
    check(tag_count(db, "oldtag") == 1 && nth_uid(db, note, 0) != 0,
          "set: the tag labels the note and the item is a task");
    doc = on_document_from_text("Replaced title\n\nReplaced body\n! replaced action due 2026-11-11");
    check(save_with_tags(db, note, doc), "set: the new content is saved over it");
    on_document_free(doc);
    check(note_title_is(db, note, "Replaced title"), "set: the title follows the new first line");
    check(tag_count(db, "oldtag") == -1, "set: a tag no note carries any more is pruned");
    GList *items = items_of(db, note);
    OnActionItem *it = items != NULL ? items->data : NULL;
    check(g_list_length(items) == 1 && strcmp(it->text, "replaced action") == 0 &&
          it->due == check_midnight(2026, 11, 11),
          "set: the old item is retired and the new line is the note's one item");
    on_db_action_list_free(items);

    /* Modified time (importers): listings order by it.                    */
    gint64 older = on_note_new_from_text(db, NULL, "Older\nx");
    gint64 newer = on_note_new_from_text(db, NULL, "Newer\nx");
    check(on_db_note_set_updated_at(db, older, 1790000000) &&
          on_db_note_set_updated_at(db, newer, 1790003600), "set-modified: times are written");
    OnNoteMeta *meta = on_db_note_get(db, older);
    check(meta != NULL && meta->updated_at == 1790000000, "set-modified: the time reads back");
    on_db_note_meta_free(meta);
    GList *recent = on_db_note_list_recent(db);
    gint pos_older = -1, pos_newer = -1, i = 0;
    for (GList *l = recent; l != NULL; l = l->next, i++) {
        gint64 id = ((OnNoteMeta *)l->data)->id;
        if (id == older) pos_older = i;
        if (id == newer) pos_newer = i;
    }
    on_db_note_list_free(recent);
    check(pos_newer >= 0 && pos_newer < pos_older, "set-modified: the newer note lists first");

    /* Pin.                                                                */
    gint pinned = on_db_note_count_pinned(db);
    check(on_db_note_set_pinned(db, older, TRUE) && on_db_note_set_pinned(db, newer, TRUE),
          "pin: two notes are pinned");
    check(on_db_note_count_pinned(db) == pinned + 2 &&
          list_has_note(on_db_note_list_pinned(db), newer), "pin: they count and list as pinned");
    check(on_db_note_set_pinned(db, older, FALSE) && on_db_note_count_pinned(db) == pinned + 1 &&
          !list_has_note(on_db_note_list_pinned(db), older), "unpin: the flag clears");
}

/* ── Action items ───────────────────────────────────────────────────────── */

/*
 * test_action_lines — which lines are action items, what their fields
 * are, and the dates a due suffix may be written in.
 *
 * Inputs:
 *   db — the database
 */
static void
test_action_lines(OnDatabase *db)
{
    gint64 note   = on_note_new_from_text(db, "Stride",
        "Stride rollout\n\nSensor rollout.\n! Rerun the reports due 2026-09-30\n"
        "! Confirm the version due 10/3/26\n!   Archive dashboards  \nnot ! an item");
    GList *items = items_of(db, note);
    check(g_list_length(items) == 3, "lines: three '!' lines are three items");
    if (g_list_length(items) == 3) {
        OnActionItem *a = g_list_nth_data(items, 0);
        OnActionItem *b = g_list_nth_data(items, 1);
        OnActionItem *c = g_list_nth_data(items, 2);
        check(strcmp(a->text, "Rerun the reports") == 0 && a->due == check_midnight(2026, 9, 30),
              "lines: an ISO due date is split off the text");
        check(strcmp(b->text, "Confirm the version") == 0 && b->due == check_midnight(2026, 10, 3),
              "lines: so is an M/D/YY one");
        check(strcmp(c->text, "Archive dashboards") == 0 && c->due == 0,
              "lines: the text is trimmed; no suffix, no due date");
        check(a->ord == 0 && b->ord == 1 && c->ord == 2 && !a->done && !b->done,
              "lines: items are numbered in line order and start open");
        check(a->uid != 0 && a->uid != b->uid && b->uid != c->uid, "lines: each has its own uid");
    }
    on_db_action_list_free(items);

    OnActionItem *row = on_db_action_get(db, nth_uid(db, note, 1));
    check(row != NULL && row->note_id == note && row->ord == 1 &&
          strcmp(row->text, "Confirm the version") == 0,
          "uid: an item is found by uid");
    on_db_action_item_free(row);

    /* What is NOT an item.                                                 */
    gint64 odd = on_note_new_from_text(db, 0,
        "Unicode caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x93\x93\n\n"
        "send due diligence report due 12/31/26\n! due 2026-01-01\n!\n! Real one");
    items = items_of(db, odd);
    check(g_list_length(items) == 1 && strcmp(((OnActionItem *)items->data)->text, "Real one") == 0,
          "lines: a line without '!', a bare '!' and a due date alone are not items");
    on_db_action_list_free(items);

    /* Block kinds: a heading can be an item; a list or code line cannot.  */
    OnDocument *doc = on_document_new();
    static const struct { OnBlockKind kind; const char *text; } lines[] = {
        { ON_BLOCK_PARA,   "Kinds" },
        { ON_BLOCK_H2,     "! A heading item" },
        { ON_BLOCK_BULLET, "! a bullet is not one" },
        { ON_BLOCK_CHECK,  "! nor a task box" },
        { ON_BLOCK_CODE,   "! nor code" },
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        OnBlock *b = on_block_new_text(lines[i].kind);
        on_text_append(b->text, lines[i].text, strlen(lines[i].text), 0);
        on_document_insert_block(doc, on_document_n_blocks(doc), b);
    }
    on_document_remove_block(doc, 0);   /* the empty block a new document holds */
    gint64 kinds = on_db_note_create(db);
    check(on_note_document_save(db, kinds, doc, TRUE, NULL), "kinds: the note is saved");
    on_document_free(doc);
    items = items_of(db, kinds);
    check(g_list_length(items) == 1 &&
          strcmp(((OnActionItem *)items->data)->text, "A heading item") == 0,
          "lines: only the heading counts among heading, bullet, task box and code");
    on_db_action_list_free(items);

    /* The due-date parser itself.                                         */
    gsize  at  = 0;
    gint64 due = 0;
    check(on_action_split_due("pay due 2027-01-15", &at, &due) &&
          at == 4 && due == check_midnight(2027, 1, 15), "due: ISO YYYY-MM-DD");
    check(on_action_split_due("pay due 1/2/2027", &at, &due) &&
          due == check_midnight(2027, 1, 2), "due: M/D/YYYY");
    check(on_action_split_due("due 12/31/26", &at, &due) && at == 0 &&
          due == check_midnight(2026, 12, 31), "due: M/D/YY");
    check(!on_action_split_due("due 13/45/99", &at, &due), "due: an impossible date is refused");
    check(!on_action_split_due("send due diligence", &at, &due), "due: 'due' with no date is text");
    check(on_action_split_due("due by due 2026-03-04", &at, &due) && at == 7,
          "due: the last 'due' that parses wins");
}

/*
 * test_action_list — the cross-note listing: newest note first, line order
 * within a note, trashed notes left out; uids survive renumbering.
 *
 * Inputs:
 *   db — the database
 */
static void
test_action_list(OnDatabase *db)
{
    gint64 first  = on_note_new_from_text(db, NULL, "Listing one\n! one-a\n! one-b");
    gint64 second = on_note_new_from_text(db, NULL, "Listing two\n! two-a");
    on_db_note_set_updated_at(db, first, 1890000000);
    on_db_note_set_updated_at(db, second, 1890003600);

    GList *all = on_db_action_list(db);
    const char *order[3] = { NULL, NULL, NULL };
    gint seen = 0;
    for (GList *l = all; l != NULL; l = l->next) {
        OnActionItem *it = l->data;
        if ((it->note_id == first || it->note_id == second) && seen < 3) order[seen++] = it->text;
    }
    check(seen == 3 && strcmp(order[0], "two-a") == 0 && strcmp(order[1], "one-a") == 0 &&
          strcmp(order[2], "one-b") == 0,
          "list: newest note first, line order within a note");
    on_db_action_list_free(all);

    /* A new line above an item renumbers it, never re-identifies it.      */
    gint64 uid_b = nth_uid(db, first, 1);
    OnDocument *doc = on_note_document_load(db, first);
    OnBlock    *b   = on_block_new_text(ON_BLOCK_PARA);
    on_text_append(b->text, "! inserted", strlen("! inserted"), 0);
    on_document_insert_block(doc, 1, b);
    check(on_note_document_save(db, first, doc, TRUE, NULL), "renumber: a line is inserted above");
    on_document_free(doc);
    OnActionItem *row = on_db_action_get(db, uid_b);
    check(row != NULL && row->ord == 2 && strcmp(row->text, "one-b") == 0,
          "renumber: the item keeps its uid at its new position");
    on_db_action_item_free(row);

    /* Trash hides a note's items; restore brings them back.               */
    gint total = 0, open = 0, total_after = 0;
    on_db_action_counts(db, &total, &open);
    check(on_db_notes_trash(db, &second, 1), "list: a note is trashed");
    on_db_action_counts(db, &total_after, NULL);
    check(total_after == total - 1 && !action_list_has_note(db, second),
          "list: a trashed note's items leave the listing and the counts");
    OnActionItem *kept = on_db_action_get(db, nth_uid(db, second, 0));
    check(kept != NULL && strcmp(kept->text, "two-a") == 0,
          "list: trashing tombstones nothing: the item still resolves by uid");
    on_db_action_item_free(kept);
    check(on_db_note_restore(db, second), "list: and restored");
    on_db_action_counts(db, &total_after, NULL);
    check(total_after == total, "list: its items come back");
}

/*
 * test_action_edit — done / undone / due through action_edit.h (the Action
 * Items view's path), and a rename of the task row pulled into the line
 * (the path an edit made in the Tasks app takes).
 *
 * Inputs:
 *   app — the context, database open, no hooks (headless)
 */
static void
test_action_edit(OnApp *app)
{
    OnDatabase *db   = app->db;
    gint64      note = on_note_new_from_text(db, NULL, "Edits\n! Call the bank due 2026-11-02\n! Second");
    gint64      uid  = nth_uid(db, note, 0);
    gint open_before = 0, open_after = 0;
    on_db_action_counts(db, NULL, &open_before);

    check(on_action_edit_set_done(app, uid, TRUE), "done: written");
    gint64 completed = 0;
    gboolean struck  = FALSE;
    gchar *line = stored_line(db, note, text_line(1), &struck);
    check(task_status(db, uid, &completed) == SHARED_DB_TASK_STATUS_DONE && completed != 0,
          "done: the task's status is DONE, completed_at stamped");
    check(struck && strcmp(line, "! Call the bank due 2026-11-02") == 0,
          "done: the line is struck, its text unchanged");
    g_free(line);
    on_db_action_counts(db, NULL, &open_after);
    check(open_after == open_before - 1, "done: one fewer open item");

    check(on_action_edit_set_done(app, uid, FALSE), "undone: written");
    line = stored_line(db, note, text_line(1), &struck);
    check(!struck && task_status(db, uid, NULL) == SHARED_DB_TASK_STATUS_IN_PROGRESS,
          "undone: the line is un-struck, the task back IN PROGRESS");
    g_free(line);

    /* Due: set (ISO is what is written), then clear.                       */
    check(on_action_edit_set_due(app, uid, check_midnight(2027, 1, 15)), "due: set");
    line = stored_line(db, note, text_line(1), NULL);
    check(strcmp(line, "! Call the bank due 2027-01-15") == 0, "due: the suffix is rewritten ISO");
    g_free(line);
    check(on_action_edit_set_due(app, uid, 0), "due: cleared");
    line = stored_line(db, note, text_line(1), NULL);
    check(strcmp(line, "! Call the bank") == 0, "due: the suffix is gone");
    g_free(line);

    /* A done item's due date stays done.                                   */
    on_action_edit_set_done(app, uid, TRUE);
    on_action_edit_set_due(app, uid, check_midnight(2027, 2, 2));
    line = stored_line(db, note, text_line(1), &struck);
    check(struck && strcmp(line, "! Call the bank due 2027-02-02") == 0,
          "due: a date added to a done item is struck with it");
    g_free(line);

    /* Rename from the row: '!', spacing, due suffix, strike and uid kept.  */
    OnActionItem *row = on_db_action_get(db, uid);
    check(on_db_action_set(db, uid, ON_ACTION_FIELD_TEXT, "Renamed by uid", row->done, row->due), "text: the row is renamed");
    on_db_action_item_free(row);
    check(on_action_sync_note(app, note), "text: the pull rewrites the line and saves it");
    line = stored_line(db, note, text_line(1), &struck);
    check(strcmp(line, "! Renamed by uid due 2027-02-02") == 0 && struck,
          "text: the line keeps its '!', its due date and its done state");
    g_free(line);
    row = on_db_action_get(db, uid);
    check(row != NULL && row->ord == 0 && strcmp(row->text, "Renamed by uid") == 0,
          "text: the uid still addresses the item");
    on_db_action_item_free(row);

    check(!on_action_edit_set_done(app, 987654, TRUE) && !on_action_edit_set_due(app, 987654, 0),
          "an unknown uid is refused");
}

/* ── Tags given from outside the text ──────────────────────────────────── */

/*
 * body_is — does a note's cached plain text read exactly so?
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *   want — the text expected
 *
 * Output: TRUE when it does.
 */
static gboolean
body_is(OnDatabase *db, gint64 note, const char *want)
{
    gchar   *body = on_note_text_cached(db, note);
    gboolean same = body != NULL && strcmp(body, want) == 0;
    if (!same) fprintf(stderr, "  body: \"%s\", want \"%s\"\n", body ? body : "(null)", want);
    g_free(body);
    return same;
}

/*
 * word_is — does on_document_tag_word make this of a name?
 *
 * Inputs:
 *   name — the tag name
 *   want — the word expected, NULL for none
 *
 * Output: TRUE when it does.
 */
static gboolean
word_is(const char *name, const char *want)
{
    gchar   *word = on_document_tag_word(name);
    gboolean same = g_strcmp0(word, want) == 0;
    g_free(word);
    return same;
}

/*
 * test_given_tags — tags given from outside the text go on the note's
 * tag line as #words (on_note_append_tags): CamelCase for a name of
 * several words, once per tag in any case;
 * emoji and Notes' favorite flag; the two scopes; the per-note names.
 *
 * Inputs:
 *   app — the context, database open
 */
static void
test_given_tags(OnApp *app)
{
    OnDatabase *db = app->db;
    check(word_is("Old Projects", "OldProjects") && word_is("Captain's Log", "CaptainsLog") &&
          word_is("salt", "salt") && word_is("  big client ", "BigClient") &&
          word_is("year-end review", "Year-endReview") && word_is("!!!", NULL) &&
          word_is("", NULL) && word_is(NULL, NULL),
          "word: joined CamelCase, punctuation dropped, one word as it is");

    gint64 a = on_note_new_from_text(db, NULL, "Given one\nbody");
    gint64 b = on_note_new_from_text(db, NULL, "Given two");
    const char *client[] = { "Big Client" };
    const char *more[]   = { "bigclient", "Salt" };
    check(on_note_append_tags(app, a, client, 1) &&
          body_is(db, a, "#BigClient\nGiven one\nbody"),
          "append: on the tag line, above the title");
    check(on_note_append_tags(app, a, more, 2) &&
          body_is(db, a, "#BigClient #Salt\nGiven one\nbody"),
          "append: a tag it has is skipped, the next joins the tag line");
    check(on_note_append_tags(app, b, client, 1) && body_is(db, b, "#BigClient\nGiven two") &&
          note_title_is(db, b, "Given two"),
          "append: the title stays the title");
    gint64 fresh = on_note_new_from_text(db, "Big Client", "\n");
    check(body_is(db, fresh, "#BigClient\n\n"),
          "append: a new note is its tag line, an empty title and an empty body line");
    on_db_notes_delete(db, &fresh, 1);       /* the counts below are a's and b's */
    gchar *tags = note_tags_are(db, a);
    check(strcmp(tags, "BigClient:-T,Salt:-T") == 0, "append: text tags");
    g_free(tags);

    /* Emoji and the favorite flag; the scopes.                             */
    OnTag *big = on_db_tag_find(db, "bigclient");
    check(big != NULL && on_db_tag_set_emoji(db, big->id, " \xf0\x9f\x8f\xa2 ") &&
          on_db_tag_set_favorite(db, big->id, TRUE), "tags: an emoji and a favorite set");
    OnTag *again = big != NULL ? on_db_tag_get(db, big->id) : NULL;
    check(again != NULL && strcmp(again->emoji, "\xf0\x9f\x8f\xa2") == 0 && again->favorite,
          "tags: they read back (the emoji trimmed)");
    on_db_tag_free(again);
    check(on_db_notes_trash(db, &a, 1) && on_db_notes_trash(db, &b, 1) &&
          tag_listed(db, ON_TAGS_ALL, "BigClient") &&
          !tag_listed(db, ON_TAGS_CARRIED, "BigClient"),
          "scopes: a tag only trashed notes carry lives, but is not shown");
    check(on_db_note_restore(db, a) && tag_listed(db, ON_TAGS_CARRIED, "BigClient"),
          "scopes: a restored note brings it back, with its tags");
    GHashTable *names = on_db_note_tag_names_map(db, NULL, 0);
    check(strcmp(g_hash_table_lookup(names, &a), "BigClient, Salt") == 0 &&
          strcmp(g_hash_table_lookup(names, &b), "BigClient") == 0 &&
          g_hash_table_lookup(names, &(gint64){ 999999 }) == NULL,
          "names: each note's tags as one string; none for an unknown note");
    g_hash_table_destroy(names);
    names = on_db_note_tag_names_map(db, &a, 1);
    check(strcmp(g_hash_table_lookup(names, &a), "BigClient, Salt") == 0 &&
          g_hash_table_lookup(names, &b) == NULL,
          "names: scoped to given notes, the others are absent");
    g_hash_table_destroy(names);
    gint64 gone[] = { a, b };
    check(on_db_notes_delete(db, gone, 2) && on_db_tag_find(db, "BigClient") == NULL,
          "delete: deleting the last notes takes their tags");
    on_db_tag_free(big);
}

/* ── Tags ───────────────────────────────────────────────────────────────── */

/*
 * test_tags — #tag runs in the text are the note's tags; links are made
 * and pruned by the save.
 *
 * Inputs:
 *   db — the database
 */
static void
test_tags(OnDatabase *db)
{
    gint64 one = on_note_new_from_text(db, NULL, "Tagged one\nbody");
    gint64 two = on_note_new_from_text(db, NULL, "Tagged two\nbody");
    OnDocument *doc = on_note_document_load(db, one);
    add_tags(doc, (const char *[]){ "salt", "urgent", NULL });
    GList *names = on_document_collect_tags(doc);
    check(g_list_length(names) == 2 && strcmp(names->data, "salt") == 0 &&
          strcmp(names->next->data, "urgent") == 0, "tags: collected in document order, no '#'");
    g_list_free_full(names, g_free);
    check(save_with_tags(db, one, doc), "tags: saved with the note");
    on_document_free(doc);
    gchar *body = on_note_text_cached(db, one);
    check(strcmp(body, "#salt #urgent\nTagged one\nbody") == 0,
          "tags: the tokens are in the text, on its first line");
    g_free(body);
    GHashTable *texts = on_db_note_text_map(db, 0, NULL, 0);
    check(strcmp(g_hash_table_lookup(texts, &one), "#salt #urgent\nTagged one\nbody") == 0 &&
          strcmp(g_hash_table_lookup(texts, &two), "\nTagged two\nbody") == 0,
          "text map: every note's full text");
    g_hash_table_destroy(texts);
    texts = on_db_note_text_map(db, 5, &one, 1);
    check(strcmp(g_hash_table_lookup(texts, &one), "#salt") == 0 &&
          g_hash_table_lookup(texts, &two) == NULL,
          "text map: a leading slice, scoped to the given notes");
    g_hash_table_destroy(texts);

    doc = on_note_document_load(db, two);
    add_tags(doc, (const char *[]){ "salt", NULL });
    save_with_tags(db, two, doc);
    on_document_free(doc);
    check(tag_count(db, "salt") == 2 && tag_count(db, "urgent") == 1, "tags: counted per tag");
    GList *labelled = on_db_notes_by_tag(db, 0);
    check(labelled == NULL, "tags: no notes for a tag that does not exist");
    GList *tags = on_db_tag_list(db, ON_TAGS_CARRIED);
    gint64 salt_id = 0;
    for (GList *l = tags; l != NULL; l = l->next) {
        if (strcmp(((OnTag *)l->data)->name, "salt") == 0) salt_id = ((OnTag *)l->data)->id;
    }
    on_db_tag_list_free(tags);
    labelled = on_db_notes_by_tag(db, salt_id);
    check(g_list_length(labelled) == 2, "tags: both notes list under #salt");
    on_db_note_list_free(labelled);

    /* Untag: the token leaves the text, the link and an orphan tag go.     */
    doc = on_note_document_load(db, one);
    OnText *t   = on_document_block(doc, 0)->text;
    const char *at = strstr(t->text->str, " #urgent");
    on_document_delete_text(doc, (OnPos){ 0, -1, (gsize)(at - t->text->str) }, strlen(" #urgent"));
    check(save_with_tags(db, one, doc), "untag: saved");
    on_document_free(doc);
    check(tag_count(db, "urgent") == -1 && tag_count(db, "salt") == 2,
          "untag: the tag no note carries is pruned, the other kept");
    /* Remove a tag: any case, with its gap; a body line left empty goes,
     * the tag line stays.                                                  */
    doc = on_note_document_load(db, two);
    check(!on_document_remove_tag(doc, "nosuchtag"), "remove tag: a tag the note lacks is skipped");
    check(on_document_remove_tag(doc, "SALT"), "remove tag: found in any case");
    check(save_with_tags(db, two, doc), "remove tag: saved");
    on_document_free(doc);
    gchar *cleared = on_note_text_cached(db, two);
    check(strcmp(cleared, "\nTagged two\nbody") == 0, "remove tag: the tag line is left empty");
    g_free(cleared);
    check(tag_count(db, "salt") == 1, "remove tag: the other note keeps the tag");
    doc = on_note_document_load(db, one);
    OnBlock *mid = on_block_new_text(ON_BLOCK_PARA);
    on_text_append(mid->text, "see ", 4, 0);
    on_text_append(mid->text, "#salt", 5, ON_FMT_TAG);
    on_text_append(mid->text, " here", 5, 0);
    on_document_insert_block(doc, text_line(1), mid);
    check(on_document_remove_tag(doc, "salt"), "remove tag: removed inside a line");
    check(strcmp(on_document_block(doc, text_line(1))->text->text->str, "see here") == 0,
          "remove tag: the gap goes with it");
    check(on_document_n_blocks(doc) == 4 && on_document_block(doc, 0)->text->text->len == 0,
          "remove tag: the tag line that held it too stays, empty");
    check(save_with_tags(db, one, doc), "remove tag: saved");
    on_document_free(doc);
    check(tag_count(db, "salt") == -1, "remove tag: the last carrier gone, the tag is pruned");
    /* A trashed note keeps its tag: it is found, and clearing it frees the tag. */
    doc = on_note_document_load(db, two);
    add_tags(doc, (const char *[]){ "salt", NULL });
    check(save_with_tags(db, two, doc), "trashed tag: saved");
    on_document_free(doc);
    check(on_db_notes_trash(db, &two, 1), "trashed tag: trashed");
    gint64 salt_tag = 0;
    tags = on_db_tag_list(db, ON_TAGS_ALL);
    for (GList *l = tags; l != NULL; l = l->next)
        if (strcmp(((OnTag *)l->data)->name, "salt") == 0) salt_tag = ((OnTag *)l->data)->id;
    on_db_tag_list_free(tags);
    GArray *carriers = on_db_note_ids_with_tag(db, salt_tag);
    labelled = on_db_notes_by_tag(db, salt_tag);
    check(salt_tag != 0 && carriers->len == 1 && g_array_index(carriers, gint64, 0) == two &&
          labelled == NULL, "trashed tag: found among the carriers, not among the visible notes");
    g_array_unref(carriers);
    doc = on_note_document_load(db, two);
    check(on_document_remove_tag(doc, "salt") && save_with_tags(db, two, doc),
          "trashed tag: removed from the trashed note");
    on_document_free(doc);
    check(tag_count(db, "salt") == -1, "trashed tag: the last carrier gone, the tag is pruned");
    gint64 both[] = { one, two };
    check(on_db_notes_delete(db, both, 2) && tag_count(db, "salt") == -1,
          "tags: deleting the last notes prunes their tags");
}

/* ── Trash ──────────────────────────────────────────────────────────────── */

/*
 * test_trash — notes into the Trash with their tags, out again, and
 * emptied.
 *
 * Inputs:
 *   db — the database
 */
static void
test_trash(OnDatabase *db)
{
    gint64 plan = on_note_new_from_text(db, "Old Projects", "Retired plan\n! retired item");
    gint64 a    = on_note_new_from_text(db, NULL, "Throwaway A\nx");
    gint64 b    = on_note_new_from_text(db, NULL, "Throwaway B\nx");
    gint visible = on_db_note_count_visible(db);
    gint trash   = on_db_trash_count(db);

    gint64 ab[] = { a, b };
    check(on_db_notes_trash(db, ab, 2), "trash: two notes in one call");
    check(on_db_note_is_trashed(db, a) && list_has_note(on_db_note_list_trashed(db), b) &&
          !list_has_note(on_db_note_list_all(db, FALSE), a) &&
          list_has_note(on_db_note_list_all(db, TRUE), a),
          "trash: they leave the visible notes, list in the Trash, and search still has them");
    check(on_db_note_count_visible(db) == visible - 2 && on_db_trash_count(db) == trash + 2,
          "trash: the counts follow");

    check(on_db_notes_trash(db, &plan, 1) && !action_list_has_note(db, plan),
          "trash: a trashed note's items are hidden");
    gchar *tags = note_tags_are(db, plan);
    check(strcmp(tags, "OldProjects:-T") == 0, "trash: it keeps its tags");
    g_free(tags);
    check(on_db_note_restore(db, plan) && !on_db_note_is_trashed(db, plan) &&
          action_list_has_note(db, plan), "restore: it comes back, its items with it");
    check(on_db_note_restore(db, a) && !on_db_note_is_trashed(db, a), "restore: a note");

    /* Permanent.                                                           */
    check(on_db_notes_delete(db, &b, 1) && on_db_note_get(db, b) == NULL,
          "delete: a note deleted outright is gone");

    /* Empty: everything flagged.                                           */
    gint64 c    = on_note_new_from_text(db, "Doomed", "Throwaway C\nx");
    gint64 held = on_note_new_from_text(db, NULL, "Held\nx");
    gint64 cd[] = { c, held };
    on_db_notes_trash(db, cd, 2);
    GArray *going = on_db_trash_note_ids(db);
    gboolean has_held = FALSE;
    for (guint i = 0; i < going->len; i++) has_held |= g_array_index(going, gint64, i) == held;
    g_array_free(going, TRUE);
    check(has_held, "empty: the notes that would go are listed first");
    check(on_db_trash_empty(db) && on_db_trash_count(db) == 0, "empty: the Trash is empty");
    check(on_db_note_get(db, c) == NULL && on_db_note_get(db, held) == NULL &&
          on_db_tag_find(db, "Doomed") == NULL,
          "empty: the trashed notes are gone, with the tags only they carried");
}

/* ── Search ─────────────────────────────────────────────────────────────── */

/*
 * matches — one query against one title and body.
 *
 * Inputs:
 *   query     — the query text
 *   use_regex — regex mode
 *   title     — the note's title
 *   body      — its body
 *
 * Output: TRUE when the query parses and matches.
 */
static gboolean
matches(const char *query, gboolean use_regex, const char *title, const char *body)
{
    OnQuery *q  = on_query_new(query, FALSE, use_regex, NULL);
    gboolean ok = q != NULL && on_query_matches(q, title, body);
    on_query_free(q);
    return ok;
}

/*
 * test_search — the query language over a note's title and plain text.
 *
 * Inputs:
 *   db — the database
 */
static void
test_search(OnDatabase *db)
{
    gint64 note = on_note_new_from_text(db, NULL, "Stride rollout\n\nSensor rollout.\nUnicode caf\xc3\xa9");
    gchar *body = on_note_text_cached(db, note);
    const char *title = "Stride rollout";
    check(matches("rollout", FALSE, title, body), "search: a word");
    check(matches("ROLLOUT stride", FALSE, title, body), "search: words are ANDed, case-insensitive");
    check(!matches("rollout nothing", FALSE, title, body), "search: every word must match");
    check(matches("\"sensor rollout\"", FALSE, title, body), "search: a quoted phrase");
    check(!matches("\"rollout sensor\"", FALSE, title, body), "search: a phrase keeps its order");
    check(!matches("rollout -sensor", FALSE, title, body), "search: -word excludes");
    check(matches("-nothing", FALSE, title, body), "search: an exclusion alone matches what lacks it");
    check(matches("CAF\xc3\x89", FALSE, title, body), "search: case folding beyond ASCII");
    check(matches("r[eo]+ll", TRUE, title, body), "search: a regex");
    GError  *err = NULL;
    OnQuery *bad = on_query_new("(", FALSE, TRUE, &err);
    check(bad == NULL && err != NULL, "search: a bad regex is an error, not a search");
    g_clear_error(&err);
    OnQuery *empty = on_query_new("", FALSE, FALSE, NULL);
    check(empty != NULL && on_query_is_empty(empty), "search: an empty query says so");
    on_query_free(empty);
    g_free(body);
}

/* ── Counts, images, export, backup ─────────────────────────────────────── */

/*
 * test_stats — the whole-database counts move with what is added.
 *
 * Inputs:
 *   db — the database
 */
static void
test_stats(OnDatabase *db)
{
    gint notes = 0, tags = 0, visible, pinned, actions = 0, open = 0;
    on_db_totals(db, &notes, &tags);
    visible = on_db_note_count_visible(db);
    pinned  = on_db_note_count_pinned(db);
    on_db_action_counts(db, &actions, &open);

    gint64 note   = on_note_new_from_text(db, "Stats", "Stats note\n! open one\n! done one");
    OnDocument *doc = on_note_document_load(db, note);
    add_tags(doc, (const char *[]){ "statstag", NULL });
    save_with_tags(db, note, doc);
    on_document_free(doc);
    on_db_note_set_pinned(db, note, TRUE);
    OnApp app = { .db = db };
    on_action_edit_set_done(&app, nth_uid(db, note, 1), TRUE);

    gint n2 = 0, t2 = 0, a2 = 0, o2 = 0;
    on_db_totals(db, &n2, &t2);
    on_db_action_counts(db, &a2, &o2);
    check(n2 == notes + 1 && t2 == tags + 2, "stats: notes and tags (field and text) totals");
    check(on_db_note_count_visible(db) == visible + 1 && on_db_note_count_pinned(db) == pinned + 1,
          "stats: visible and pinned");
    check(a2 == actions + 2 && o2 == open + 1, "stats: action items, total and open");
}

/*
 * test_images — images stored verbatim, counted and fetched by ordinal
 * without decoding.
 *
 * Inputs:
 *   db — the database
 *
 * Output: the note holding the images (for the export test).
 */
static gint64
test_images(OnDatabase *db)
{
    GBytes *folder_png = file_png("folder.png");
    GBytes *file_bytes = file_png("file.png");
    check(folder_png != NULL && file_bytes != NULL, "images: the fixtures load as PNG");
    gint64 note   = on_note_new_from_text(db, "Pictures", "Trip photos\nA line with a picture");
    OnDocument *doc = on_note_document_load(db, note);
    OnBlock    *line = on_document_block(doc, 1);
    on_text_append(line->text, " " ON_OBJ_CHAR, 1 + ON_OBJ_CHAR_LEN, 0);
    on_text_add_image(line->text, line->text->text->len - ON_OBJ_CHAR_LEN, folder_png, 0);
    on_document_insert_block(doc, on_document_n_blocks(doc), on_block_new_image(file_bytes, 0));
    check(on_note_document_save(db, note, doc, TRUE, NULL), "images: an inline and a block image saved");
    on_document_free(doc);

    guint8 *blob = NULL;
    gsize   len  = 0;
    on_db_note_load(db, note, &blob, &len);
    check(on_note_count_images(blob, len) == 2, "images: counted in the blob");
    GBytes *first  = on_note_image_nth_png(blob, len, 0);
    GBytes *second = on_note_image_nth_png(blob, len, 1);
    check(first != NULL && g_bytes_equal(first, folder_png) &&
          second != NULL && g_bytes_equal(second, file_bytes),
          "images: fetched by ordinal in document order, byte for byte as stored");
    int w = 0, h = 0;
    gsize n = 0;
    const uint8_t *bytes = g_bytes_get_data(second, &n);
    check(on_image_probe_size(bytes, n, &w, &h) && w > 0 && h > 0, "images: sized from the header");
    check(on_note_image_nth_png(blob, len, 2) == NULL, "images: no third image");
    g_bytes_unref(first);
    g_bytes_unref(second);
    g_free(blob);
    g_bytes_unref(folder_png);
    g_bytes_unref(file_bytes);
    return note;
}

/*
 * read_file — a whole file.
 *
 * Inputs:
 *   path — the file
 *   len  — receives its length (may be NULL)
 *
 * Output: the bytes (g_free), or NULL when it cannot be read.
 */
static gchar *
read_file(const char *path, gsize *len)
{
    gchar *text = NULL;
    g_file_get_contents(path, &text, len, NULL);
    return text;
}

/*
 * test_export — Markdown and HTML, one file per visible note in one
 * folder, its tags first; images as side-car files or inline data.
 *
 * Inputs:
 *   app      — the context, database open
 *   data_dir — the test data directory
 *   pictures — a note with two images, tagged "Pictures"
 */
static void
test_export(OnApp *app, const char *data_dir, gint64 pictures)
{
    gint64 sync = on_note_new_from_text(app->db, "Meetings",
                                        "Weekly sync\n\n! Circulate the minutes\nDecisions: ship it.");
    on_note_append_tags(app, sync, (const char *[]){ "Salt" }, 1);
    gint64 gone = on_note_new_from_text(app->db, NULL, "Trashed export\nx");
    on_db_notes_trash(app->db, &gone, 1);
    gint visible = on_db_note_count_visible(app->db);

    gchar *md_dir = g_build_filename(data_dir, "out_md", NULL);
    gchar *err    = NULL;
    check(on_export_all(app, md_dir, ON_EXPORT_MARKDOWN, &err) == visible && err == NULL,
          "export-md: one file per visible note");
    gchar *path = g_build_filename(md_dir, "Weekly sync.md", NULL);
    gchar *text = read_file(path, NULL);
    check(text != NULL && g_str_has_prefix(text, "tags: Meetings, Salt\n\n") &&
          strstr(text, "Circulate the minutes") != NULL &&
          strstr(text, "Decisions: ship it.") != NULL,
          "export-md: one folder, the tags line first, the text after it");
    g_free(text);
    g_free(path);
    path = g_build_filename(md_dir, "Trashed export.md", NULL);
    check(!g_file_test(path, G_FILE_TEST_EXISTS), "export-md: a trashed note is not exported");
    g_free(path);

    path = g_build_filename(md_dir, "Trip photos.md", NULL);
    text = read_file(path, NULL);
    check(text != NULL && strstr(text, "![image](Trip photos-img1.png)") != NULL &&
          strstr(text, "![image](Trip photos-img2.png)") != NULL,
          "export-md: images are referenced as numbered side-car files");
    g_free(text);
    g_free(path);
    guint8 *blob = NULL;
    gsize   len  = 0, n_file = 0;
    on_db_note_load(app->db, pictures, &blob, &len);
    GBytes *stored = on_note_image_nth_png(blob, len, 1);
    path = g_build_filename(md_dir, "Trip photos-img2.png", NULL);
    gchar *file = read_file(path, &n_file);
    gsize  n_stored = 0;
    const void *stored_bytes = g_bytes_get_data(stored, &n_stored);
    check(file != NULL && n_file == n_stored && memcmp(file, stored_bytes, n_file) == 0,
          "export-md: a side-car image is the stored bytes");
    g_free(file);
    g_free(path);
    g_bytes_unref(stored);
    g_free(blob);

    gchar *html_dir = g_build_filename(data_dir, "out_html", NULL);
    check(on_export_all(app, html_dir, ON_EXPORT_HTML, NULL) == visible,
          "export-html: one file per visible note");
    path = g_build_filename(html_dir, "Trip photos.html", NULL);
    text = read_file(path, NULL);
    check(text != NULL && strstr(text, "<title>Trip photos</title>") != NULL &&
          strstr(text, "<p class=\"tags\">Tags: <span class=\"tag\">Pictures</span></p>") != NULL &&
          strstr(text, "src=\"data:image/png;base64,") != NULL,
          "export-html: a full page, its tags at the top, images inline as data");
    g_free(text);
    g_free(path);
    g_free(html_dir);
    g_free(md_dir);
}

/*
 * test_backup — a snapshot of the open database is a database with the
 * same notes.
 *
 * Inputs:
 *   db       — the database
 *   data_dir — the test data directory
 */
static void
test_backup(OnDatabase *db, const char *data_dir)
{
    gchar *dir = g_build_filename(data_dir, "snapshots", NULL);
    g_mkdir_with_parents(dir, 0700);
    shared_settings_set(db->shared, SHARED_SETTING_BACKUP_DIR, dir, NULL);
    gint  notes = 0;
    gint  copied = -1;
    char *dest = NULL;
    on_db_totals(db, &notes, NULL);
    check(shared_db_backup(db->shared, &dest, NULL) == 0 && dest != NULL &&
          g_str_has_prefix(dest, dir), "backup: written into the chosen folder");
    shared_db *copy = dest != NULL ? shared_db_open_readonly(dest, NULL) : NULL;
    sqlite3_stmt *stmt = NULL;
    if (copy != NULL &&
        sqlite3_prepare_v2(shared_db_handle(copy), "SELECT COUNT(*) FROM notes", -1,
                           &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        copied = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    check(copy != NULL && copied == notes && notes > 0, "backup: it opens with the same notes");
    shared_db_close(copy);
    free(dest);
    g_free(dir);
}

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
/*
 * count_files — how many note_file rows a note owns.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *
 * Output: the count.
 */
static int
count_files(OnDatabase *db, gint64 note)
{
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db->handle, "SELECT COUNT(*) FROM note_file WHERE note_id = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, note);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

/*
 * files_equal — do two files hold the same bytes?
 *
 * Inputs:
 *   a, b — the paths
 *
 * Output: TRUE when both read and are equal.
 */
static gboolean
files_equal(const char *a, const char *b)
{
    gchar *da = NULL, *db_ = NULL;
    gsize  na = 0, nb = 0;
    gboolean same = g_file_get_contents(a, &da, &na, NULL) &&
                    g_file_get_contents(b, &db_, &nb, NULL) &&
                    na == nb && memcmp(da, db_, na) == 0;
    g_free(da);
    g_free(db_);
    return same;
}

/*
 * save_with_file — a note whose document is its text and a line holding one
 * attached file.
 *
 * Inputs:
 *   db   — the database
 *   note — the note
 *   id   — the note_file row
 *   name — the file's name
 */
static void
save_with_file(OnDatabase *db, gint64 note, gint64 id, const char *name)
{
    OnDocument *doc  = on_note_document_load(db, note);
    OnBlock    *line = on_block_new_text(ON_BLOCK_PARA);
    on_text_append(line->text, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN, 0);
    on_text_add_file(line->text, 0, id, name);
    on_document_insert_block(doc, on_document_n_blocks(doc), line);
    on_note_document_save(db, note, doc, TRUE, NULL);
    on_document_free(doc);
}

/*
 * test_files — attached files live in note_file rows and never outlive
 * what names them: streamed in and out intact (a file larger than one
 * piece), copied inside the database, kept by a save while the note or its
 * undo history names them and deleted when neither does, deleted with the
 * note (Delete and Empty Trash), and swept when nothing names them.
 *
 * Inputs:
 *   db       — the database
 *   data_dir — the test's data directory (scratch files go here)
 */
static void
test_files(OnDatabase *db, const char *data_dir)
{
    gchar *src = g_build_filename(data_dir, "attach-src.bin", NULL);
    gchar *out = g_build_filename(data_dir, "attach-out.bin", NULL);
    gsize  n = 2500000;              /* more than two 1 MB pieces           */
    guint8 *bytes = g_malloc(n);
    for (gsize i = 0; i < n; i++) bytes[i] = (guint8)(i * 31 + (i >> 8));
    g_file_set_contents(src, (gchar *)bytes, (gssize)n, NULL);
    g_free(bytes);

    gint64 a = on_note_new_from_text(db, NULL, "Files A\nbody");
    gint64 b = on_note_new_from_text(db, NULL, "Files B\nbody");
    gint64 f = on_db_note_file_add(db, a, src, "big.bin");
    check(f > 0 && count_files(db, a) == 1, "a file is stored in a row of its note");
    unlink(out);
    check(on_db_note_file_export(db, f, out) && files_equal(src, out),
          "a stored file comes back byte for byte (streamed in pieces)");
    check(!on_db_note_file_export(db, f, out), "export never overwrites a file");
    unlink(out);
    check(on_db_note_file_add(db, a, data_dir, "dir") == 0 &&
          on_db_note_file_add(db, a, "/nonexistent/attach", "x") == 0 &&
          count_files(db, a) == 1, "a folder or a missing file stores nothing");
    check(!on_db_note_file_export(db, 999999, out) && access(out, F_OK) != 0,
          "exporting a row that does not exist fails and leaves nothing");

    gint64 copy = on_db_note_file_copy(db, f, b);
    check(copy > 0 && copy != f && count_files(db, b) == 1 &&
          on_db_note_file_export(db, copy, out) && files_equal(src, out),
          "a copy inside the database is a row of the other note with the same bytes");
    unlink(out);
    check(on_db_note_file_copy(db, 999999, b) == 0, "copying a missing row gives 0");

    /* A save keeps what the note names; the history counts as naming.      */
    save_with_file(db, a, f, "big.bin");
    check(count_files(db, a) == 1, "a save keeps the file its note names");
    OnDocument *doc = on_note_document_load(db, a);
    guint last = on_document_n_blocks(doc) - 1;
    OnPos from = { last, -1, 0 }, to = { last, -1, ON_OBJ_CHAR_LEN };
    check(on_document_block(doc, last)->text->objects->len == 1 &&
          on_document_delete_range(doc, from, to, NULL), "the file's tile is deleted");
    on_note_document_save(db, a, doc, TRUE, NULL);
    check(count_files(db, a) == 1,
          "the save keeps the file while an undo could bring its tile back");
    check(on_document_undo(doc) && on_document_block(doc, last)->text->objects->len == 1,
          "undo brings the tile back");
    on_document_delete_range(doc, from, to, NULL);
    on_document_clear_undo(doc);
    on_note_document_save(db, a, doc, TRUE, NULL);
    check(count_files(db, a) == 0, "with the history gone, the next save deletes the file");
    on_document_free(doc);

    /* Delete and Empty Trash take a note's files with it.                  */
    check(count_files(db, b) == 1, "B still owns its copy");
    check(on_db_notes_delete(db, &b, 1) && count_files(db, b) == 0,
          "deleting a note deletes its files");
    gint64 t = on_note_new_from_text(db, NULL, "Files trashed\nbody");
    gint64 tf = on_db_note_file_add(db, t, src, "t.bin");
    save_with_file(db, t, tf, "t.bin");
    check(on_db_notes_trash(db, &t, 1) && count_files(db, t) == 1,
          "a trashed note keeps its files");
    check(on_db_trash_empty(db) && count_files(db, t) == 0,
          "emptying the Trash deletes them");

    /* The sweep deletes rows no stored note names, and never touches a
     * note it cannot read.                                                 */
    gint64 s_note = on_note_new_from_text(db, NULL, "Files swept\nbody");
    gint64 named  = on_db_note_file_add(db, s_note, src, "named.bin");
    save_with_file(db, s_note, named, "named.bin");
    gint64 stray  = on_db_note_file_add(db, s_note, src, "stray.bin");
    check(stray > 0 && count_files(db, s_note) == 2, "a stray row (a crash's leftover)");
    check(on_note_files_sweep(db, 0) && count_files(db, s_note) == 1,
          "the sweep deletes the row no note names and keeps the one it does");
    const guint8 junk[] = { 1, 2, 3 };
    check(on_db_note_save(db, s_note, "x", junk, sizeof junk, "x") &&
          on_note_files_sweep(db, s_note) && count_files(db, s_note) == 1,
          "a note whose content cannot be read keeps its files");

    on_db_notes_delete(db, &a, 1);
    on_db_notes_delete(db, &s_note, 1);
    unlink(src);
    g_free(src);
    g_free(out);
}

/*
 * block_is — is block i of a document of this kind, with this text?
 *
 * Inputs:
 *   doc  — the document
 *   i    — the block
 *   kind — the kind it should be
 *   text — the text it should hold
 *
 * Output: TRUE when both match.
 */
static gboolean
block_is(OnDocument *doc, guint i, OnBlockKind kind, const char *text)
{
    if (i >= on_document_n_blocks(doc)) return FALSE;
    OnBlock *b = on_document_block(doc, i);
    return b->kind == kind && b->text != NULL && strcmp(b->text->text->str, text) == 0;
}

/*
 * test_calendar — the day planner's reads (on_db_calendar_day) and the
 * note taken for an event (on_note_for_event): made once, with the
 * event's title, an Attendees: line and its attendees as bullets, and two
 * empty lines; found again; made afresh
 * when the first one is in the Trash; a '!' title is no action item.
 *
 * Inputs:
 *   db — the test database
 *
 * Output: none (failures are counted).
 */
static void
test_calendar(OnDatabase *db)
{
    const int64_t lo = 1791518400, hi = lo + 86400;   /* any 24 hours */
    char sql[1024];
    snprintf(sql, sizeof sql,
             "INSERT INTO gcal_calendar (id, summary, color) VALUES ('cal', 'Work', '#336699');"
             "INSERT INTO gcal_event (calendar_id, event_id, title, start, end, all_day, attendees)"
             " VALUES ('cal', 'ev1', 'Design review', %lld, %lld, 0,"
             "         'Ann Lee' || char(10) || 'bo@x.com'),"
             "        ('cal', 'ev2', 'Offsite', %lld, %lld, 1, ''),"
             "        ('cal', 'ev3', '! Urgent sync', %lld, %lld, 0, ''),"
             "        ('cal', 'ev4', 'Tomorrow', %lld, %lld, 0, '');"
             "INSERT INTO tasks (title, scheduled_start, scheduled_duration, status)"
             " VALUES ('Write the plan', %lld, 90, 0), ('Done one', %lld, 0, %d);",
             (long long)(lo + 36000), (long long)(lo + 39600), (long long)lo, (long long)hi,
             (long long)(lo + 50400), (long long)(lo + 52200), (long long)(hi + 3600),
             (long long)(hi + 7200), (long long)(lo + 32400), (long long)(lo + 43200),
             SHARED_DB_TASK_STATUS_DONE);
    check(sqlite3_exec(db->handle, sql, NULL, NULL, NULL) == SQLITE_OK, "calendar: seeded");

    on_cal_day_t day;
    check(on_db_calendar_day(db, lo, hi, &day), "calendar: the day is read");
    check(day.n_tasks == 2 && strcmp(day.tasks[0].title, "Write the plan") == 0 &&
          day.tasks[0].duration == 90 && !day.tasks[0].done,
          "calendar: the day's scheduled tasks, by start");
    check(day.n_tasks == 2 && day.tasks[1].done &&
          day.tasks[1].duration == SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT,
          "calendar: a done task, and the default length when none is stored");
    check(day.n_events == 3 && day.events[0].all_day &&
          strcmp(day.events[1].event_id, "ev1") == 0 &&
          strcmp(day.events[1].color, "#336699") == 0,
          "calendar: the day's events, all-day first, with their calendar's colour");
    on_db_calendar_day_free(&day);
    check(day.n_tasks == 0 && day.tasks == NULL, "calendar: freeing zeroes the day");

    bool    created = false;
    gint64  note    = on_note_for_event(db, "cal", "ev1", "Design review", &created);
    check(note != 0 && created, "event note: made on the first double-click");
    OnNoteMeta *m = on_db_note_get(db, note);
    check(m != NULL && strcmp(m->title, "Design review") == 0, "event note: titled by the event");
    on_db_note_meta_free(m);
    OnDocument *doc = on_note_document_load(db, note);
    check(doc != NULL && on_document_n_blocks(doc) == 7 &&
          block_is(doc, 1, ON_BLOCK_PARA, "Design review") &&
          block_is(doc, 2, ON_BLOCK_PARA, "Attendees:") &&
          block_is(doc, 3, ON_BLOCK_BULLET, "Ann Lee") &&
          block_is(doc, 4, ON_BLOCK_BULLET, "bo@x.com") &&
          block_is(doc, 5, ON_BLOCK_PARA, "") &&
          block_is(doc, 6, ON_BLOCK_PARA, ""),
          "event note: an Attendees: line, one bullet each, then two empty lines");
    on_document_free(doc);

    check(on_note_for_event(db, "cal", "ev1", "Design review", &created) == note && !created,
          "event note: found again, not made twice");
    check(on_db_notes_trash(db, &note, 1), "event note: trashed");
    int64_t again = on_note_for_event(db, "cal", "ev1", "Design review", &created);
    check(again != 0 && again != note && created, "event note: made afresh over a trashed one");

    int64_t urgent = on_note_for_event(db, "cal", "ev3", "! Urgent sync", &created);
    doc = urgent != 0 ? on_note_document_load(db, urgent) : NULL;
    check(doc != NULL && on_document_n_blocks(doc) == 3 &&
          block_is(doc, 1, ON_BLOCK_PARA, "Urgent sync") &&
          block_is(doc, 2, ON_BLOCK_PARA, ""),
          "event note: a '!' title is no action item; no attendees, an empty body line");
    on_document_free(doc);

    int64_t gone = on_note_for_event(db, "cal", "not-cached", "Left the cache", &created);
    check(gone != 0 && created, "event note: an event no longer cached still gets its note");

    gint64  ids[] = { note, again, urgent, gone };
    on_db_notes_delete(db, ids, 4);
    check(on_db_event_note(db, "cal", "ev1") == 0, "event note: deleting the note unbinds it");
}

int
main(void)
{
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof data_dir, "%s", check_data_dir());
    on_app_config_init();

    static OnApp app;
    check(on_app_open_db(&app, NULL), "the test database opens");
    test_notes(app.db);
    test_action_lines(app.db);
    test_action_list(app.db);
    test_action_edit(&app);
    test_given_tags(&app);
    test_tags(app.db);
    test_trash(app.db);
    test_search(app.db);
    test_stats(app.db);
    gint64 pictures = test_images(app.db);
    test_export(&app, data_dir, pictures);
    test_files(app.db, data_dir);
    test_backup(app.db, data_dir);
    test_calendar(app.db);
    on_app_shutdown(&app);
    return check_result();
}
