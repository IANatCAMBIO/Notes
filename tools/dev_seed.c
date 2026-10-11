/* ===========================================================================
 * dev_seed.c — the part of the development data the CLI cannot write
 *
 * `notes note new` builds body paragraphs only, so a note that shows every
 * block kind — headings, lists, task boxes, a code block, a table, block
 * and inline images, inline styles, a #tag, action lines — is written here,
 * straight through the document model and THE save (on_note_document_save).
 * tools/seed-dev.sh drives it, with the CLI doing everything else.
 *
 *   dev_seed check                 exit 0 when no app serves the data
 *                                  directory (so it may be wiped)
 *   dev_seed rich NOTE_ID IMAGE    append every block kind to NOTE_ID,
 *                                  using IMAGE (any image file) for the
 *                                  pictures
 *
 * It refuses to run unless NOTES_DATA_DIR is set: this tool only ever
 * writes development data.
 * =========================================================================== */

#include "app_lifecycle.h"
#include "identity.h"
#include "image_io.h"
#include "ipc.h"
#include "serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * seed_rich — append every block kind to a note and save it.
 *
 * Inputs:
 *   db         — the database
 *   note_id    — an existing note
 *   image_path — an image file for the picture blocks
 *
 * Output: 0 on success, 2 on failure (reported on stderr).
 */
static int
seed_rich(OnDatabase *db, gint64 note_id, const char *image_path)
{
    OnNoteMeta *meta = on_db_note_get(db, note_id);
    if (meta == NULL) {
        fprintf(stderr, "dev_seed: no such note: %" G_GINT64_FORMAT "\n", note_id);
        return 2;
    }
    on_db_note_meta_free(meta);

    size_t   n_png = 0;
    uint8_t *png   = on_image_file_to_png(image_path, &n_png);
    if (png == NULL) {
        fprintf(stderr, "dev_seed: cannot load image %s\n", image_path);
        return 2;
    }
    GBytes *image = g_bytes_new_with_free_func(png, n_png, free, png);

    OnDocument *doc = on_note_document_load(db, note_id);
    if (doc == NULL) {
        fprintf(stderr, "dev_seed: cannot read note %" G_GINT64_FORMAT "\n",
                note_id);
        g_bytes_unref(image);
        return 2;
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

    gboolean ok = on_note_document_save(db, note_id, doc, TRUE, NULL);
    GList *tags = on_document_collect_tags(doc);
    ok = ok && on_db_note_set_tags(db, note_id, tags);
    g_list_free_full(tags, g_free);
    on_document_free(doc);
    g_bytes_unref(image);
    if (!ok) {
        fprintf(stderr, "dev_seed: could not save note %" G_GINT64_FORMAT "\n", note_id);
        return 2;
    }
    printf("wrote every block kind to note %" G_GINT64_FORMAT "\n", note_id);
    return 0;
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
    if (getenv(ON_DATA_DIR_ENV) == NULL) {
        fprintf(stderr, "dev_seed: %s is not set; this tool only writes "
                        "development data\n", ON_DATA_DIR_ENV);
        return 2;
    }
    on_app_config_init();

    /* Either command needs the data directory to itself: with an app
     * serving it, this tool would be a second writer.                      */
    if (on_ipc_try_remote("ping", NULL) != ON_IPC_NO_SERVER) {
        fprintf(stderr, "dev_seed: an app is running on this data directory; "
                        "quit it first\n");
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "check") == 0) {
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "rich") != 0) {
        fprintf(stderr, "usage: dev_seed check | dev_seed rich NOTE_ID IMAGE\n");
        return 1;
    }

    OnApp app = { 0 };
    app.db_dir = on_app_config_load_db_dir();
    if (!on_app_open_db(&app)) {
        return 2;
    }
    int rc = seed_rich(app.db, g_ascii_strtoll(argv[2], NULL, 10), argv[3]);
    on_db_close(app.db);
    g_free(app.db_dir);
    return rc;
}
