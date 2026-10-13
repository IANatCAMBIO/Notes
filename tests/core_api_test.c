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
 * strike, renames from the row keeping '!', done state and due), folders
 * (nesting, paths, rename, emoji, AI mode, move, sort, trash, restore,
 * delete), tags, pins, the Trash, search, the whole-database counts,
 * Markdown and HTML export, embedded images and backups.  Output
 * formatting was the tool's own and went with it.
 *
 * One database for the whole program, in this test's NOTES_DATA_DIR.
 * =========================================================================== */

#include "check.h"

#include "action_edit.h"
#include "action_sync.h"
#include "app_lifecycle.h"
#include "export.h"
#include "image_io.h"
#include "search_query.h"
#include "serialize.h"


/*
 * folder_named — a direct child folder of `parent` by name.
 *
 * Inputs:
 *   db     — the database
 *   parent — the parent folder (0 = the top level)
 *   name   — the child's name
 *
 * Output: its id, or 0 when there is none.
 */
static gint64
folder_named(OnDatabase *db, gint64 parent, const char *name)
{
    gint64 id   = 0;
    GList *kids = on_db_folder_list(db, parent);
    for (GList *l = kids; l != NULL && id == 0; l = l->next) {
        OnFolder *f = l->data;
        if (strcmp(f->name, name) == 0) id = f->id;
    }
    on_db_folder_list_free(kids);
    return id;
}

/*
 * folder_names — a parent's direct children's names, joined by ','.
 *
 * Inputs:
 *   db     — the database
 *   parent — the parent folder (0 = the top level)
 *
 * Output: a new string; g_free it.
 */
static gchar *
folder_names(OnDatabase *db, gint64 parent)
{
    GString *out  = g_string_new(NULL);
    GList   *kids = on_db_folder_list(db, parent);
    for (GList *l = kids; l != NULL; l = l->next) {
        if (out->len > 0) g_string_append_c(out, ',');
        g_string_append(out, ((OnFolder *)l->data)->name);
    }
    on_db_folder_list_free(kids);
    return g_string_free(out, FALSE);
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
 * Output: the status (0 new, 1 in progress, 2 done), or -1 when no task
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
 * add_tag_line — append a line of #tag tokens, space-separated, each
 * carrying ON_FMT_TAG (what the editor's tag capture writes).
 *
 * Inputs:
 *   doc   — the document
 *   names — NULL-terminated tag names, without '#'
 */
static void
add_tag_line(OnDocument *doc, const char *const *names)
{
    OnBlock *b = on_block_new_text(ON_BLOCK_PARA);
    for (const char *const *n = names; *n != NULL; n++) {
        if (n != names) on_text_append(b->text, " ", 1, 0);
        gchar *token = g_strdup_printf("#%s", *n);
        on_text_append(b->text, token, strlen(token), ON_FMT_TAG);
        g_free(token);
    }
    on_document_insert_block(doc, on_document_n_blocks(doc), b);
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
    GList      *tags   = on_db_tag_list(db);
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
 * test_notes — a note from plain text, replacing its content, its
 * modified time, moving and pinning.
 *
 * Inputs:
 *   db — the database
 */
static void
test_notes(OnDatabase *db)
{
    static const char text[] =
        "Stride rollout\n\nSensor rollout.\nTabs\there and \"quotes\" and a \\ backslash.";
    gint64 folder = on_db_folder_create(db, 0, "Notes test", ON_AI_MODE_NORMAL, NULL);
    gint64 note   = on_note_new_from_text(db, folder, text);
    check(note != 0, "notes: a note is made from plain text");
    check(note_title_is(db, note, "Stride rollout"), "notes: its first line is its title");
    gchar *body = on_note_text_cached(db, note);
    check(body != NULL && strcmp(body, text) == 0,
          "notes: its plain text reads back byte for byte (tabs, quotes, backslash)");
    g_free(body);
    OnNoteMeta *meta = on_db_note_get(db, note);
    check(meta != NULL && meta->folder_id == folder, "notes: it is in the folder asked for");
    on_db_note_meta_free(meta);

    gint64 blank = on_note_new_from_text(db, 0, "");
    check(note_title_is(db, blank, ON_DEFAULT_NOTE_TITLE),
          "notes: an empty note takes the default title");

    /* Replacing the content (`note set`): the old tags and action items
     * go with the old text.                                               */
    OnDocument *doc = on_document_from_text("Old title\n! Old item");
    add_tag_line(doc, (const char *[]){ "oldtag", NULL });
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
    gint64 older = on_note_new_from_text(db, 0, "Older\nx");
    gint64 newer = on_note_new_from_text(db, 0, "Newer\nx");
    check(on_db_note_set_updated_at(db, older, 1790000000) &&
          on_db_note_set_updated_at(db, newer, 1790003600), "set-modified: times are written");
    meta = on_db_note_get(db, older);
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

    /* Move, several at once, and back to the top level.                   */
    gint64 two[] = { older, newer };
    check(on_db_notes_move(db, two, 2, folder), "move: two notes move in one call");
    check(list_has_note(on_db_note_list(db, folder), older) &&
          list_has_note(on_db_note_list(db, folder), newer) &&
          !list_has_note(on_db_note_list(db, 0), older),
          "move: they list in the folder and not at the top level");
    check(on_db_notes_move(db, &older, 1, 0) && list_has_note(on_db_note_list(db, 0), older),
          "move: '/' is the top level");

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
    gint64 salt   = on_db_folder_create(db, 0, "Salt", ON_AI_MODE_NORMAL, NULL);
    gint64 deps   = on_db_folder_create(db, salt, "Deployments", ON_AI_MODE_NORMAL, NULL);
    gint64 stride = on_db_folder_create(db, deps, "Stride", ON_AI_MODE_NORMAL, NULL);
    gint64 note   = on_note_new_from_text(db, stride,
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
          strcmp(row->context, "Stride") == 0,
          "uid: an item is found by uid, its context the note's own folder");
    on_db_action_item_free(row);

    /* What is NOT an item.                                                 */
    gint64 odd = on_note_new_from_text(db, 0,
        "Unicode caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x93\x93\n\n"
        "send due diligence report due 12/31/26\n! due 2026-01-01\n!\n! Real one");
    items = items_of(db, odd);
    check(g_list_length(items) == 1 && strcmp(((OnActionItem *)items->data)->text, "Real one") == 0,
          "lines: a line without '!', a bare '!' and a due date alone are not items");
    on_db_action_list_free(items);
    row = on_db_action_get(db, nth_uid(db, odd, 0));
    check(row != NULL && strcmp(row->context, "") == 0, "uid: a root note's context is \"\"");
    on_db_action_item_free(row);

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
    gint64 kinds = on_db_note_create(db, 0);
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
    gint64 first  = on_note_new_from_text(db, 0, "Listing one\n! one-a\n! one-b");
    gint64 second = on_note_new_from_text(db, 0, "Listing two\n! two-a");
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
    gint64      note = on_note_new_from_text(db, 0, "Edits\n! Call the bank due 2026-11-02\n! Second");
    gint64      uid  = nth_uid(db, note, 0);
    gint open_before = 0, open_after = 0;
    on_db_action_counts(db, NULL, &open_before);

    check(on_action_edit_set_done(app, uid, TRUE), "done: written");
    gint64 completed = 0;
    gboolean struck  = FALSE;
    gchar *line = stored_line(db, note, 1, &struck);
    check(task_status(db, uid, &completed) == ON_TASK_STATUS_DONE && completed != 0,
          "done: the task's status is DONE, completed_at stamped");
    check(struck && strcmp(line, "! Call the bank due 2026-11-02") == 0,
          "done: the line is struck, its text unchanged");
    g_free(line);
    on_db_action_counts(db, NULL, &open_after);
    check(open_after == open_before - 1, "done: one fewer open item");

    check(on_action_edit_set_done(app, uid, FALSE), "undone: written");
    line = stored_line(db, note, 1, &struck);
    check(!struck && task_status(db, uid, NULL) == 1,
          "undone: the line is un-struck, the task back IN PROGRESS");
    g_free(line);

    /* Due: set (ISO is what is written), then clear.                       */
    check(on_action_edit_set_due(app, uid, check_midnight(2027, 1, 15)), "due: set");
    line = stored_line(db, note, 1, NULL);
    check(strcmp(line, "! Call the bank due 2027-01-15") == 0, "due: the suffix is rewritten ISO");
    g_free(line);
    check(on_action_edit_set_due(app, uid, 0), "due: cleared");
    line = stored_line(db, note, 1, NULL);
    check(strcmp(line, "! Call the bank") == 0, "due: the suffix is gone");
    g_free(line);

    /* A done item's due date stays done.                                   */
    on_action_edit_set_done(app, uid, TRUE);
    on_action_edit_set_due(app, uid, check_midnight(2027, 2, 2));
    line = stored_line(db, note, 1, &struck);
    check(struck && strcmp(line, "! Call the bank due 2027-02-02") == 0,
          "due: a date added to a done item is struck with it");
    g_free(line);

    /* Rename from the row: '!', spacing, due suffix, strike and uid kept.  */
    OnActionItem *row = on_db_action_get(db, uid);
    check(on_db_action_set(db, uid, ON_ACTION_FIELD_TEXT, "Renamed by uid", row->done, row->due), "text: the row is renamed");
    on_db_action_item_free(row);
    check(on_action_sync_note(app, note), "text: the pull rewrites the line and saves it");
    line = stored_line(db, note, 1, &struck);
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

/* ── Folders ────────────────────────────────────────────────────────────── */

/*
 * test_folders — nesting and paths, the Info fields, move, sort.
 *
 * Inputs:
 *   db — the database
 */
static void
test_folders(OnDatabase *db)
{
    gint64 top    = on_db_folder_create(db, 0, "New", ON_AI_MODE_NORMAL, NULL);
    gint64 nested = on_db_folder_create(db, top, "Nested", ON_AI_MODE_NORMAL, NULL);
    gint64 deep   = on_db_folder_create(db, nested, "Deep", ON_AI_MODE_NORMAL, NULL);
    gchar *path   = on_db_folder_path(db, deep);
    check(strcmp(path, "New/Nested/Deep") == 0, "folders: a nested folder's path");
    g_free(path);
    GHashTable *paths = on_db_folder_path_map(db);
    check(strcmp(g_hash_table_lookup(paths, &deep), "New/Nested/Deep") == 0 &&
          g_hash_table_lookup(paths, &(gint64){ 0 }) == NULL,
          "folders: the path map agrees; the top level has no entry");
    g_hash_table_destroy(paths);

    /* Rename, emoji, AI mode: one update (the Info panel's).               */
    check(on_db_folder_update(db, nested, "Renamed", ON_AI_MODE_CUSTOM, "\xf0\x9f\x93\x81"),
          "info: name, AI mode and emoji in one update");
    gchar *emoji = on_db_folder_get_emoji(db, nested);
    path = on_db_folder_path(db, deep);
    check(strcmp(emoji, "\xf0\x9f\x93\x81") == 0 &&
          on_db_folder_get_ai_mode(db, nested) == ON_AI_MODE_CUSTOM &&
          strcmp(path, "New/Renamed/Deep") == 0,
          "info: they read back; the subtree's paths follow the rename");
    g_free(path);
    g_free(emoji);
    check(on_db_folder_update(db, nested, "Renamed", ON_AI_MODE_PROJECT, ""), "info: emoji cleared");
    emoji = on_db_folder_get_emoji(db, nested);
    check(*emoji == '\0' && on_db_folder_get_ai_mode(db, nested) == ON_AI_MODE_PROJECT,
          "info: an empty emoji reads back empty");
    g_free(emoji);
    gint64 with = on_db_folder_create(db, 0, "Born with", ON_AI_MODE_PROJECT, "\xf0\x9f\xa7\x82");
    emoji = on_db_folder_get_emoji(db, with);
    check(strcmp(emoji, "\xf0\x9f\xa7\x82") == 0 && on_db_folder_get_ai_mode(db, with) == ON_AI_MODE_PROJECT,
          "folders: emoji and AI mode can be given at creation");
    g_free(emoji);

    /* Move: anywhere but into itself.                                      */
    check(on_db_folder_move(db, deep, 0) && folder_named(db, 0, "Deep") == deep,
          "move: a folder moves to the top level");
    check(!on_db_folder_move(db, top, nested), "move: not into its own descendant");
    check(!on_db_folder_move(db, top, top), "move: not into itself");
    check(on_db_folder_move(db, deep, nested) && folder_named(db, nested, "Deep") == deep,
          "move: and back under another folder, with its subtree");

    /* Sort one level, case-insensitively.                                  */
    gint64 parent = on_db_folder_create(db, 0, "Sorting", ON_AI_MODE_NORMAL, NULL);
    on_db_folder_create(db, parent, "b", ON_AI_MODE_NORMAL, NULL);
    gint64 big_c = on_db_folder_create(db, parent, "C", ON_AI_MODE_NORMAL, NULL);
    on_db_folder_create(db, parent, "a", ON_AI_MODE_NORMAL, NULL);
    on_db_folder_create(db, big_c, "z", ON_AI_MODE_NORMAL, NULL);
    on_db_folder_create(db, big_c, "y", ON_AI_MODE_NORMAL, NULL);
    gchar *names = folder_names(db, parent);
    check(strcmp(names, "b,C,a") == 0, "sort: children list in creation order first");
    g_free(names);
    check(on_db_folder_sort_children(db, parent) == 3, "sort: three children sorted");
    names = folder_names(db, parent);
    check(strcmp(names, "a,b,C") == 0, "sort: alphabetically, ignoring case");
    g_free(names);
    names = folder_names(db, big_c);
    check(strcmp(names, "z,y") == 0, "sort: one level only");
    g_free(names);
    GList *tops = on_db_folder_list(db, 0);
    gint   n_tops = (gint)g_list_length(tops);
    on_db_folder_list_free(tops);
    check(on_db_folder_sort_children(db, 0) == n_tops, "sort: the top level sorts too");

    /* Note counts per folder.                                              */
    on_note_new_from_text(db, deep, "In deep\nx");
    on_note_new_from_text(db, deep, "Also in deep\nx");
    GHashTable *counts = on_db_note_count_map(db);
    check(GPOINTER_TO_INT(g_hash_table_lookup(counts, &deep)) == 2 &&
          g_hash_table_lookup(counts, &parent) == NULL,
          "counts: per folder, a folder with no notes absent");
    g_hash_table_destroy(counts);
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
    gint64 one = on_note_new_from_text(db, 0, "Tagged one\nbody");
    gint64 two = on_note_new_from_text(db, 0, "Tagged two\nbody");
    OnDocument *doc = on_note_document_load(db, one);
    add_tag_line(doc, (const char *[]){ "salt", "urgent", NULL });
    GList *names = on_document_collect_tags(doc);
    check(g_list_length(names) == 2 && strcmp(names->data, "salt") == 0 &&
          strcmp(names->next->data, "urgent") == 0, "tags: collected in document order, no '#'");
    g_list_free_full(names, g_free);
    check(save_with_tags(db, one, doc), "tags: saved with the note");
    on_document_free(doc);
    gchar *body = on_note_text_cached(db, one);
    check(strcmp(body, "Tagged one\nbody\n#salt #urgent") == 0, "tags: the tokens are in the text");
    g_free(body);

    doc = on_note_document_load(db, two);
    add_tag_line(doc, (const char *[]){ "salt", NULL });
    save_with_tags(db, two, doc);
    on_document_free(doc);
    check(tag_count(db, "salt") == 2 && tag_count(db, "urgent") == 1, "tags: counted per tag");
    GList *labelled = on_db_notes_by_tag(db, 0);
    check(labelled == NULL, "tags: no notes for a tag that does not exist");
    GList *tags = on_db_tag_list(db);
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
    OnText *t   = on_document_block(doc, 2)->text;
    const char *at = strstr(t->text->str, " #urgent");
    on_document_delete_text(doc, (OnPos){ 2, -1, (gsize)(at - t->text->str) }, strlen(" #urgent"));
    check(save_with_tags(db, one, doc), "untag: saved");
    on_document_free(doc);
    check(tag_count(db, "urgent") == -1 && tag_count(db, "salt") == 2,
          "untag: the tag no note carries is pruned, the other kept");
    gint64 both[] = { one, two };
    check(on_db_notes_delete(db, both, 2) && tag_count(db, "salt") == -1,
          "tags: deleting the last notes prunes their tags");
}

/* ── Trash ──────────────────────────────────────────────────────────────── */

/*
 * test_trash — notes and folders into the Trash, out again, and emptied.
 *
 * Inputs:
 *   db — the database
 */
static void
test_trash(OnDatabase *db)
{
    gint64 old   = on_db_folder_create(db, 0, "Old", ON_AI_MODE_NORMAL, NULL);
    gint64 y2024 = on_db_folder_create(db, old, "2024", ON_AI_MODE_NORMAL, NULL);
    gint64 plan  = on_note_new_from_text(db, y2024, "Retired plan\n! retired item");
    gint64 a     = on_note_new_from_text(db, 0, "Throwaway A\nx");
    gint64 b     = on_note_new_from_text(db, 0, "Throwaway B\nx");
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

    check(on_db_folder_trash(db, old), "trash: a folder with a subtree");
    GList *folders = on_db_folder_list_trashed(db);
    check(g_list_length(folders) >= 1 && ((OnFolder *)folders->data)->id == old,
          "trash: the folder lists in the Trash (its subtree with it)");
    on_db_folder_list_free(folders);
    check(on_db_note_is_trashed(db, plan) && folder_named(db, 0, "Old") == 0 &&
          !action_list_has_note(db, plan),
          "trash: a note in the folder's subtree is trashed with it, its items hidden");

    check(on_db_folder_restore(db, old) && folder_named(db, 0, "Old") == old &&
          !on_db_note_is_trashed(db, plan), "restore: the folder and what it holds come back");
    check(on_db_folder_trash(db, old) && on_db_note_restore(db, plan), "restore: a note out of a trashed folder");
    OnNoteMeta *meta = on_db_note_get(db, plan);
    check(meta != NULL && meta->folder_id == 0 && !on_db_note_is_trashed(db, plan),
          "restore: lands at the top level when its folder is still in the Trash");
    on_db_note_meta_free(meta);
    check(on_db_folder_restore(db, y2024) && folder_named(db, 0, "2024") == y2024,
          "restore: a folder whose parent is in the Trash goes to the top level");
    check(on_db_note_restore(db, a) && !on_db_note_is_trashed(db, a), "restore: a note");

    /* Permanent.                                                           */
    gint64 doomed  = on_db_folder_create(db, 0, "Doomed", ON_AI_MODE_NORMAL, NULL);
    gint64 inside  = on_note_new_from_text(db, doomed, "Inside\nx");
    check(on_db_folder_delete(db, doomed) && on_db_note_get(db, inside) == NULL,
          "delete: a folder deleted outright takes its notes");
    check(on_db_notes_delete(db, &b, 1) && on_db_note_get(db, b) == NULL,
          "delete: a note deleted outright is gone");

    /* Empty: everything flagged, and everything in a trashed subtree.      */
    gint64 c = on_note_new_from_text(db, 0, "Throwaway C\nx");
    on_db_notes_trash(db, &c, 1);
    gint64 holder = on_db_folder_create(db, 0, "Holder", ON_AI_MODE_NORMAL, NULL);
    gint64 held   = on_note_new_from_text(db, holder, "Held\nx");
    on_db_folder_trash(db, holder);
    GArray *going = on_db_trash_note_ids(db);
    gboolean has_held = FALSE;
    for (guint i = 0; i < going->len; i++) has_held |= g_array_index(going, gint64, i) == held;
    g_array_free(going, TRUE);
    check(has_held, "empty: the notes that would go include those inside trashed folders");
    check(on_db_trash_empty(db) && on_db_trash_count(db) == 0, "empty: the Trash is empty");
    check(on_db_note_get(db, c) == NULL && on_db_note_get(db, held) == NULL &&
          folder_named(db, 0, "Holder") == 0 && folder_named(db, 0, "Old") == 0,
          "empty: flagged notes, trashed folders and their notes are gone");
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
    gint64 note = on_note_new_from_text(db, 0, "Stride rollout\n\nSensor rollout.\nUnicode caf\xc3\xa9");
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
    gint notes = 0, folders = 0, tags = 0, visible, pinned, actions = 0, open = 0;
    on_db_totals(db, &notes, &folders, &tags);
    visible = on_db_note_count_visible(db);
    pinned  = on_db_note_count_pinned(db);
    on_db_action_counts(db, &actions, &open);

    gint64 folder = on_db_folder_create(db, 0, "Stats", ON_AI_MODE_NORMAL, NULL);
    gint64 note   = on_note_new_from_text(db, folder, "Stats note\n! open one\n! done one");
    OnDocument *doc = on_note_document_load(db, note);
    add_tag_line(doc, (const char *[]){ "statstag", NULL });
    save_with_tags(db, note, doc);
    on_document_free(doc);
    on_db_note_set_pinned(db, note, TRUE);
    OnApp app = { .db = db };
    on_action_edit_set_done(&app, nth_uid(db, note, 1), TRUE);

    gint n2 = 0, f2 = 0, t2 = 0, a2 = 0, o2 = 0;
    on_db_totals(db, &n2, &f2, &t2);
    on_db_action_counts(db, &a2, &o2);
    check(n2 == notes + 1 && f2 == folders + 1 && t2 == tags + 1,
          "stats: notes, folders and tags totals");
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
    gint64 folder = on_db_folder_create(db, 0, "Pictures", ON_AI_MODE_NORMAL, NULL);
    gint64 note   = on_note_new_from_text(db, folder, "Trip photos\nA line with a picture");
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
 * test_export — Markdown and HTML, one file per visible note in a mirror
 * of the folder tree; images as side-car files or inline data.
 *
 * Inputs:
 *   app      — the context, database open
 *   data_dir — the test data directory
 *   pictures — a note with two images, in folder "Pictures"
 */
static void
test_export(OnApp *app, const char *data_dir, gint64 pictures)
{
    gint64 meetings = on_db_folder_create(app->db, folder_named(app->db, 0, "Salt"), "Meetings",
                                          ON_AI_MODE_NORMAL, NULL);
    on_note_new_from_text(app->db, meetings, "Weekly sync\n\n! Circulate the minutes\nDecisions: ship it.");
    gint64 gone = on_note_new_from_text(app->db, 0, "Trashed export\nx");
    on_db_notes_trash(app->db, &gone, 1);
    gint visible = on_db_note_count_visible(app->db);

    gchar *md_dir = g_build_filename(data_dir, "out_md", NULL);
    gchar *err    = NULL;
    check(on_export_all(app, md_dir, ON_EXPORT_MARKDOWN, &err) == visible && err == NULL,
          "export-md: one file per visible note");
    gchar *path = g_build_filename(md_dir, "Salt", "Meetings", "Weekly sync.md", NULL);
    gchar *text = read_file(path, NULL);
    check(text != NULL && strstr(text, "Circulate the minutes") != NULL &&
          strstr(text, "Decisions: ship it.") != NULL,
          "export-md: the note is in its folder's mirror, its text inside");
    g_free(text);
    g_free(path);
    path = g_build_filename(md_dir, "Trashed export.md", NULL);
    check(!g_file_test(path, G_FILE_TEST_EXISTS), "export-md: a trashed note is not exported");
    g_free(path);

    path = g_build_filename(md_dir, "Pictures", "Trip photos.md", NULL);
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
    path = g_build_filename(md_dir, "Pictures", "Trip photos-img2.png", NULL);
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
    path = g_build_filename(html_dir, "Pictures", "Trip photos.html", NULL);
    text = read_file(path, NULL);
    check(text != NULL && strstr(text, "<title>Trip photos</title>") != NULL &&
          strstr(text, "src=\"data:image/png;base64,") != NULL,
          "export-html: a full page, images inline as data");
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
    on_db_totals(db, &notes, NULL, NULL);
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
    test_folders(app.db);
    test_tags(app.db);
    test_trash(app.db);
    test_search(app.db);
    test_stats(app.db);
    gint64 pictures = test_images(app.db);
    test_export(&app, data_dir, pictures);
    test_backup(app.db, data_dir);
    on_app_shutdown(&app);
    return check_result();
}
