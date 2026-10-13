/* ===========================================================================
 * dev_seed.c — the development database, written through the core API
 *
 * `make seed` (tools/seed-dev.sh) builds dev/data/shared_data.db (the file
 * $CAMBIO_SHARED_DB names) with this tool:
 * nested folders (some with an emoji, one in project AI mode), six tags,
 * pinned notes, two trashed notes and a trashed folder, a note with every
 * block kind (note 1: headings, lists, task boxes, code, a table, block
 * and inline images, inline styles, a #tag, action lines), a picture
 * note, a 200-line note, forty dated filler notes, and 13 '!' action items
 * — open, two done, overdue, due today, due later, undated — in several
 * contexts.  Due dates are relative to today, so the Action Items view
 * always shows each case.  Everything goes through the same calls the app
 * makes: on_note_new_from_text / on_note_document_save, the tag links,
 * on_action_edit_set_done, the folder, pin and trash calls.
 *
 *   dev_seed check                 exit 0 when no app holds the data
 *                                  directory (so it may be wiped)
 *   dev_seed seed IMAGE IMAGE2     fill the (new) shared database;
 *                                  IMAGE and IMAGE2 are any two
 *                                  image files, for the pictures
 *
 * It refuses to run unless NOTES_DATA_DIR and CAMBIO_SHARED_DB are set:
 * this tool only ever writes development data.  It holds the app lock while it runs, so an
 * app cannot start on the database in the middle of it.
 * =========================================================================== */

#include "action_edit.h"
#include "app_lifecycle.h"
#include "identity.h"
#include "image_io.h"
#include "serialize.h"
#include "shared_db.h"                  /* SHARED_DB_ENV_VAR */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * add_text — append a text block of one kind, in one style.
 *
 * Inputs:
 *   doc   — the document
 *   kind  — a text kind
 *   text  — the block's text
 *   flags — ON_FMT_RUN_MASK bits for the whole text
 *
 * Output: the new block (owned by the document).
 */
static OnBlock *
add_text(OnDocument *doc, OnBlockKind kind, const char *text, guint32 flags)
{
    OnBlock *b = on_block_new_text(kind);
    on_text_append(b->text, text, strlen(text), flags);
    on_document_insert_block(doc, on_document_n_blocks(doc), b);
    return b;
}

/*
 * add_styled — append a body paragraph whose pieces carry different inline
 * styles and a #tag.
 *
 * Inputs:
 *   doc — the document
 *
 * Output: none.
 */
static void
add_styled(OnDocument *doc)
{
    static const struct {
        const char *text;
        guint32     flags;
    } pieces[] = {
        { "Inline styles: ",       0 },
        { "bold",                  ON_FMT_BOLD },
        { ", ",                    0 },
        { "italic",                ON_FMT_ITALIC },
        { ", ",                    0 },
        { "underlined",            ON_FMT_UNDERLINE },
        { ", ",                    0 },
        { "struck",                ON_FMT_STRIKE },
        { ", ",                    0 },
        { "bold italic",           ON_FMT_BOLD | ON_FMT_ITALIC },
        { " and a tag: ",          0 },
        { "#demo",                 ON_FMT_TAG },
        { ".",                     0 },
    };
    OnBlock *b = on_block_new_text(ON_BLOCK_PARA);
    for (size_t i = 0; i < sizeof pieces / sizeof pieces[0]; i++) {
        on_text_append(b->text, pieces[i].text, strlen(pieces[i].text),
                       pieces[i].flags);
    }
    on_document_insert_block(doc, on_document_n_blocks(doc), b);
}

/*
 * add_table — append a 3x3 table with a header row; one cell holds two
 * lines.
 *
 * Inputs:
 *   doc — the document
 *
 * Output: none.
 */
static void
add_table(OnDocument *doc)
{
    static const char *const cells[3][3] = {
        { "Collector",   "Status",       "Owner"  },
        { "Kong",        "Deployed",     "Ian"    },
        { "Apigee",      "In review\nsecond line", "Priya" },
    };
    OnBlock *t = on_block_new_table(3, 3);
    t->header = TRUE;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            on_text_append(on_block_cell(t, r, c), cells[r][c],
                           strlen(cells[r][c]), 0);
        }
    }
    on_document_insert_block(doc, on_document_n_blocks(doc), t);
}

/*
 * append_tag — append a #tag token to a document's last line, one space
 * after the text, carrying ON_FMT_TAG (what the editor's tag capture
 * writes).
 *
 * Inputs:
 *   doc  — the document (its last block is a text block)
 *   name — the tag, without '#'
 */
static void
append_tag(OnDocument *doc, const char *name)
{
    guint  last  = on_document_n_blocks(doc) - 1;
    OnPos  pos   = { last, -1, on_document_block(doc, last)->text->text->len };
    on_document_insert_text(doc, pos, " ", 1, 0);
    pos.offset++;
    gchar *token = g_strdup_printf("#%s", name);
    on_document_insert_text(doc, pos, token, strlen(token), ON_FMT_TAG);
    g_free(token);
}

/*
 * seed_rich — append every block kind to a note and save it.
 *
 * Inputs:
 *   db      — the database
 *   note_id — an existing note
 *   image   — PNG bytes for the picture blocks
 *
 * Output: TRUE on success.
 */
static gboolean
seed_rich(OnDatabase *db, gint64 note_id, GBytes *image)
{
    OnDocument *doc = on_note_document_load(db, note_id);
    if (doc == NULL) {
        return FALSE;
    }
    add_text(doc, ON_BLOCK_H1, "Heading one", 0);
    add_text(doc, ON_BLOCK_H2, "Heading two", 0);
    add_styled(doc);
    add_text(doc, ON_BLOCK_PARA,
             "A long paragraph, to see wrapping: the quick brown fox jumps "
             "over the lazy dog, then does it again and again until the line "
             "is far wider than any window the editor will be given, so that "
             "it must break at a word boundary at least twice.", 0);
    add_text(doc, ON_BLOCK_PARA,
             "Unicode: caf\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e "
             "\xe2\x80\x94 \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
             "\xf0\x9f\x91\xa7 \xe2\x80\x94 \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7", 0);
    add_text(doc, ON_BLOCK_BULLET, "A bulleted item", 0);
    add_text(doc, ON_BLOCK_BULLET, "A second bullet", 0);
    add_text(doc, ON_BLOCK_NUMBER, "First numbered step", 0);
    add_text(doc, ON_BLOCK_NUMBER, "Second numbered step", 0);
    add_text(doc, ON_BLOCK_NUMBER, "Third numbered step", 0);
    add_text(doc, ON_BLOCK_CHECK, "An open task box", 0);
    add_text(doc, ON_BLOCK_CHECK, "A ticked task box", 0)->checked = TRUE;
    add_text(doc, ON_BLOCK_CODE, "int", 0);
    add_text(doc, ON_BLOCK_CODE, "main(void)", 0);
    add_text(doc, ON_BLOCK_CODE, "{", 0);
    add_text(doc, ON_BLOCK_CODE, "    return 0;   /* a code block is consecutive CODE lines */", 0);
    add_text(doc, ON_BLOCK_CODE, "}", 0);
    add_table(doc);
    add_text(doc, ON_BLOCK_PARA, "A picture on a line of its own:", 0);
    on_document_insert_block(doc, on_document_n_blocks(doc),
                             on_block_new_image(image, 0));
    OnBlock *with_inline = add_text(
        doc, ON_BLOCK_PARA, "An inline picture " ON_OBJ_CHAR " inside a line.", 0);
    on_text_add_image(with_inline->text, strlen("An inline picture "), image, 0);
    add_text(doc, ON_BLOCK_PARA, "! Review every block kind due 2026-10-20", 0);
    add_text(doc, ON_BLOCK_H2, "! A heading can be an action item too", 0);
    add_text(doc, ON_BLOCK_BULLET, "! but a bullet starting with ! is not one", 0);
    add_text(doc, ON_BLOCK_PARA, "", 0);

    gboolean ok = on_note_document_save(db, note_id, doc, TRUE, NULL) &&
                  on_note_document_save_tags(db, note_id, doc);
    on_document_free(doc);
    return ok;
}

/*
 * day — a date relative to today, as the "YYYY-MM-DD" a due suffix takes.
 *
 * Inputs:
 *   offset — days from today (negative = in the past)
 *   out    — receives the date; at least 11 bytes
 */
static void
day(int offset, char out[static 11])
{
    time_t    now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_mday += offset;
    tm.tm_hour  = 12;                /* clear of any DST edge */
    tm.tm_isdst = -1;
    time_t when = mktime(&tm);
    localtime_r(&when, &tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
}

/*
 * note — a new note from text, optionally with #tags and images appended,
 * saved through THE save with its tag links.
 *
 * Inputs:
 *   db     — the database
 *   folder — its folder (0 = the top level)
 *   text   — its content, one line per block
 *   tags   — NULL-terminated tag names to append to the last line, or NULL
 *   images — NULL-terminated PNGs to append as image blocks, or NULL
 *
 * Output: the note's id; 0 on failure (reported on stderr).
 */
static gint64
note(OnDatabase *db, gint64 folder, const char *text,
     const char *const *tags, GBytes *const *images)
{
    gint64 id = on_note_new_from_text(db, folder, text);
    if (id == 0 || (tags == NULL && images == NULL)) {
        if (id == 0) fprintf(stderr, "dev_seed: could not create a note\n");
        return id;
    }
    OnDocument *doc = on_note_document_load(db, id);
    if (doc == NULL) {
        return 0;
    }
    for (const char *const *t = tags; t != NULL && *t != NULL; t++) {
        append_tag(doc, *t);
    }
    for (GBytes *const *i = images; i != NULL && *i != NULL; i++) {
        on_document_insert_block(doc, on_document_n_blocks(doc),
                                 on_block_new_image(*i, 0));
    }
    gboolean ok = on_note_document_save(db, id, doc, TRUE, NULL) &&
                  on_note_document_save_tags(db, id, doc);
    on_document_free(doc);
    if (!ok) {
        fprintf(stderr, "dev_seed: could not save note %" G_GINT64_FORMAT "\n", id);
    }
    return ok ? id : 0;
}

/*
 * done — mark a note's action item done, the way the Action Items view
 * does (the task row, then the struck line).
 *
 * Inputs:
 *   app  — the headless context
 *   id   — the note
 *   ord  — the item's position among the note's action lines
 *
 * Output: TRUE when the item exists and was marked.
 */
static gboolean
done(OnApp *app, gint64 id, guint ord)
{
    GList        *items = on_db_action_list_for_note(app->db, id);
    OnActionItem *it    = g_list_nth_data(items, ord);
    gboolean      ok    = it != NULL && on_action_edit_set_done(app, it->uid, TRUE);
    on_db_action_list_free(items);
    return ok;
}

/*
 * file_png — an image file as the PNG bytes a note stores.
 *
 * Inputs:
 *   path — any image file ImageIO reads
 *
 * Output: the bytes (g_bytes_unref), or NULL (reported on stderr).
 */
static GBytes *
file_png(const char *path)
{
    size_t   n   = 0;
    uint8_t *png = on_image_file_to_png(path, &n);
    if (png == NULL) {
        fprintf(stderr, "dev_seed: cannot load image %s\n", path);
        return NULL;
    }
    return g_bytes_new_with_free_func(png, n, free, png);
}

/*
 * seed — write the whole development database (see the file comment).
 *
 * Inputs:
 *   app   — the headless context, its (empty) database open
 *   image — PNG bytes for the rich note and the first trip photo
 *   photo — PNG bytes for the second trip photo
 *
 * Output: TRUE when every step succeeded.
 */
static gboolean
seed(OnApp *app, GBytes *image, GBytes *photo)
{
    OnDatabase *db = app->db;
    char d_3[11], d0[11], d1[11], d2[11], d5[11], d9[11], d_10[11], d14[11];
    day(-3, d_3); day(0, d0); day(1, d1); day(2, d2);
    day(5, d5); day(9, d9); day(-10, d_10); day(14, d14);

    gint64 salt     = on_db_folder_create(db, 0, "Salt", ON_AI_MODE_NORMAL, "\xf0\x9f\xa7\x82");
    gint64 deps     = on_db_folder_create(db, salt, "Deployments", ON_AI_MODE_PROJECT, NULL);
    gint64 stride_f = on_db_folder_create(db, deps, "Stride", ON_AI_MODE_NORMAL, NULL);
    gint64 apollo_f = on_db_folder_create(db, deps, "Apollo", ON_AI_MODE_NORMAL, NULL);
    gint64 meet_f   = on_db_folder_create(db, salt, "Meetings", ON_AI_MODE_NORMAL, NULL);
    gint64 home     = on_db_folder_create(db, 0, "Home", ON_AI_MODE_NORMAL, "\xf0\x9f\x8f\xa1");
    gint64 kitchen  = on_db_folder_create(db, home, "Kitchen", ON_AI_MODE_NORMAL, NULL);
    gint64 garden_f = on_db_folder_create(db, home, "Garden", ON_AI_MODE_NORMAL, NULL);
    gint64 reading  = on_db_folder_create(db, 0, "Reading", ON_AI_MODE_NORMAL, "\xf0\x9f\x93\x9a");
    gint64 old      = on_db_folder_create(db, 0, "Old Projects", ON_AI_MODE_NORMAL, NULL);
    gint64 y2024    = on_db_folder_create(db, old, "2024", ON_AI_MODE_NORMAL, NULL);
    gboolean ok = salt && deps && stride_f && apollo_f && meet_f && home && kitchen &&
                  garden_f && reading && old && y2024;

    gint64 rich = note(db, reading, "Every block kind\n\nThis note shows each thing the editor draws.",
                       NULL, NULL);
    ok = ok && rich != 0 && seed_rich(db, rich, image);

    gchar *text = g_strdup_printf(
        "Stride rollout plan\n\nSensor rollout for the Stride tenant.\n"
        "! Rerun the traffic reports due %s\n! Confirm the collector version due %s\n"
        "! Send the summary to Priya due %s\n! Archive the old dashboards\n"
        "Notes from the kickoff call.", d_3, d0, d5);
    gint64 stride = note(db, stride_f, text, (const char *[]){ "salt", "urgent", NULL }, NULL);
    g_free(text);
    ok = ok && stride != 0 && done(app, stride, 3);

    text = g_strdup_printf("Apollo PoV checklist\n\n! Book the validation call due %s\n"
                           "! Collect sample traffic due %s\nKong plugin deployed in staging.",
                           d2, d9);
    ok = ok && note(db, apollo_f, text, (const char *[]){ "salt", NULL }, NULL) != 0;
    g_free(text);

    text = g_strdup_printf("Weekly sync\n\nAttendees: Ian, Priya, Sam.\n"
                           "! Circulate the minutes due %s\n"
                           "Decisions: ship the hybrid upgrade next sprint.", d1);
    gint64 meeting = note(db, meet_f, text, (const char *[]){ "salt", "meetings", NULL }, NULL);
    g_free(text);
    ok = ok && meeting != 0 && done(app, meeting, 0);

    text = g_strdup_printf("Groceries\n\n! Milk\n! Bread due %s\nOlive oil, tomatoes, basil.", d0);
    gint64 groceries = note(db, kitchen, text, (const char *[]){ "home", NULL }, NULL);
    g_free(text);

    text = g_strdup_printf("Garden jobs\n\n! Prune the apple tree due %s\n"
                           "Order compost before the frost.", d_10);
    ok = ok && groceries != 0 &&
         note(db, garden_f, text, (const char *[]){ "home", NULL }, NULL) != 0;
    g_free(text);

    text = g_strdup_printf("Loose note\n\nNot in any folder.\n! A root action item due %s", d14);
    ok = ok && note(db, 0, text, NULL, NULL) != 0;
    g_free(text);

    ok = ok && note(db, home, "Trip photos\n\nTwo pictures appended from files.", NULL,
                    (GBytes *const[]){ image, photo, NULL }) != 0;

    GString *lines = g_string_new("A long note\n");
    for (int i = 1; i <= 200; i++) {
        g_string_append_printf(lines, "\nLine %d of a note long enough to scroll.", i);
    }
    ok = ok && note(db, reading, lines->str, NULL, NULL) != 0;
    g_string_free(lines, TRUE);

    ok = ok && on_db_note_set_pinned(db, stride, TRUE) &&
         on_db_note_set_pinned(db, groceries, TRUE);

    /* Forty filler notes a day apart, for list scrolling and sorting.      */
    gint64 now = (gint64)time(NULL);
    for (int i = 1; i <= 40 && ok; i++) {
        text = g_strdup_printf("Reading log %d\n\nChapter %d notes.", i, i);
        gint64 id = note(db, reading, text,
                         i % 4 == 0 ? (const char *[]){ "reading", NULL } : NULL, NULL);
        g_free(text);
        ok = id != 0 && on_db_note_set_updated_at(db, id, now - i * 86400);
    }

    /* The Trash: two notes, and a folder with a note inside it.            */
    gint64 gone[2] = {
        note(db, 0, "Draft to throw away\n\nNothing important.", NULL, NULL),
        note(db, kitchen, "Old shopping list\n\n! Something that no longer matters", NULL, NULL),
    };
    ok = ok && gone[0] != 0 && gone[1] != 0 && on_db_notes_trash(db, gone, 2) &&
         note(db, y2024, "Retired plan\n\nFrom 2024.", NULL, NULL) != 0 &&
         on_db_folder_trash(db, old);
    return ok;
}

/*
 * print_counts — what was written, for the person who ran `make seed`.
 *
 * Inputs:
 *   db — the database
 */
static void
print_counts(OnDatabase *db)
{
    gint notes = 0, folders = 0, tags = 0, actions = 0, open = 0;
    on_db_totals(db, &notes, &folders, &tags);
    on_db_action_counts(db, &actions, &open);
    printf("notes\t%d (%d visible)\nfolders\t%d\ntags\t%d\npinned\t%d\ntrash\t%d\n"
           "actions\t%d\t%d open\ndatabase\t%s\n",
           notes, on_db_note_count_visible(db), folders, tags, on_db_note_count_pinned(db),
           on_db_trash_count(db), actions, open, db->path);
}

/*
 * main — run one of the two commands (see the file comment).
 *
 * Inputs:
 *   argc, argv — the command line
 *
 * Output: 0 on success, 1 for bad usage, 2 on failure.
 */
int
main(int argc, char **argv)
{
    if (getenv(ON_DATA_DIR_ENV) == NULL || getenv(SHARED_DB_ENV_VAR) == NULL) {
        fprintf(stderr, "dev_seed: %s and %s must both be set; this tool only "
                        "writes development data\n", ON_DATA_DIR_ENV,
                SHARED_DB_ENV_VAR);
        return 2;
    }
    on_app_config_init();

    /* Either command needs the data directory to itself: with an app on
     * it, this tool would be a second writer.  The lock is held until
     * on_app_shutdown (or the end of the process).                         */
    if (on_app_lock_take() != on_app_lock_held) {
        fprintf(stderr, "dev_seed: an app is running on this data directory; "
                        "quit it first\n");
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "check") == 0) {
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "seed") != 0) {
        fprintf(stderr, "usage: dev_seed check | dev_seed seed IMAGE IMAGE2\n");
        return 1;
    }

    GBytes *image = file_png(argv[2]);
    GBytes *photo = file_png(argv[3]);
    OnApp   app   = { 0 };
    gboolean ok   = image != NULL && photo != NULL && on_app_open_db(&app, NULL) &&
                    seed(&app, image, photo);
    if (ok) {
        print_counts(app.db);
    } else {
        fprintf(stderr, "dev_seed: seeding failed\n");
    }
    on_app_shutdown(&app);
    if (image != NULL) g_bytes_unref(image);
    if (photo != NULL) g_bytes_unref(photo);
    return ok ? 0 : 2;
}
