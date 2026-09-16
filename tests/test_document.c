/* ===========================================================================
 * test_document.c — headless tests for the block model (document.[ch])
 * and the BNBF reader/writer it sits on (bnbf.[ch]).
 *
 * Built by `make test` against GLib ONLY — the build line has no GTK on
 * it, which is what proves the model needs none.  Blobs are built with
 * the writer, loaded, checked block by block, saved again and compared
 * byte for byte against the original: the round trip is the contract.
 * =========================================================================== */

#include "document.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Blob builders — thin wrappers over the writer so a test reads as the
 * record stream it describes.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnBnbfWriter w;
} Blob;

static void
blob_begin(Blob *b)
{
    on_bnbf_writer_init(&b->w);
}

static void
blob_text(Blob *b, guint32 flags, const gchar *s)
{
    on_bnbf_write_text(&b->w, flags, s, strlen(s));
}

/* A stand-in "PNG": the loader never decodes, so any bytes do.             */
static const guint8 PNG_A[] = { 0x89, 'P', 'N', 'G', 1, 2, 3 };
static const guint8 PNG_B[] = { 0x89, 'P', 'N', 'G', 9, 8 };

static void
blob_image(Blob *b, guint32 width, const guint8 *png, gsize n)
{
    on_bnbf_write_image(&b->w, width, png, n);
}

static void
blob_check(Blob *b, gboolean checked)
{
    on_bnbf_write_check(&b->w, checked);
}

/* blob_table() — a rows×cols table from a NULL-terminated cell list.       */
static void
blob_table(Blob *b, gboolean header, gint rows, gint cols,
           const gchar **cells)
{
    on_bnbf_write_table_begin(&b->w, header, rows, cols);
    for (gint i = 0; i < rows * cols; i++)
        on_bnbf_write_table_cell(&b->w, cells[i], strlen(cells[i]));
}

static GBytes *
blob_end(Blob *b)
{
    gsize n;
    guint8 *bytes = on_bnbf_writer_finish(&b->w, &n);
    return g_bytes_new_take(bytes, n);
}

/* ---------------------------------------------------------------------------
 * Assertions
 * ------------------------------------------------------------------------- */

/* assert_bytes_equal() — byte-for-byte, reporting the first difference and
 * the test line that asked.                                                */
static void
assert_bytes_equal(GBytes *want, const guint8 *got, gsize n_got, gint line)
{
    gsize n_want;
    const guint8 *w = g_bytes_get_data(want, &n_want);
    gsize n = MIN(n_want, n_got);
    for (gsize i = 0; i < n; i++)
        if (w[i] != got[i])
            g_error("line %d: blobs differ at byte %" G_GSIZE_FORMAT
                    ": want 0x%02x got 0x%02x", line, i, w[i], got[i]);
    if (n_want != n_got)
        g_error("line %d: blobs differ in length: want %" G_GSIZE_FORMAT
                " got %" G_GSIZE_FORMAT, line, n_want, n_got);
}

/* load_clean() — load a blob expecting a clean report and a valid
 * document; returns the document.                                          */
static OnDocument *
load_clean(GBytes *blob)
{
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    OnDocument *d = on_document_from_bnbf(data, n, &rep);
    g_assert_true(on_document_load_report_is_clean(&rep));
    GString *why = g_string_new(NULL);
    if (!on_document_check(d, why))
        g_error("invariant: %s", why->str);
    g_string_free(why, TRUE);
    return d;
}

/* assert_roundtrip() — the document saves back to exactly `blob`.          */
#define assert_roundtrip(d, blob) assert_roundtrip_at((d), (blob), __LINE__)
static void
assert_roundtrip_at(const OnDocument *d, GBytes *blob, gint line)
{
    gsize n;
    guint8 *out = on_document_to_bnbf(d, &n);
    assert_bytes_equal(blob, out, n, line);
    g_free(out);
}

/* assert_valid() — the invariants hold.                                    */
static void
assert_valid(const OnDocument *d)
{
    GString *why = g_string_new(NULL);
    if (!on_document_check(d, why))
        g_error("invariant: %s", why->str);
    g_string_free(why, TRUE);
}

/* block_text() — a text block's text as a C string (borrowed).             */
static const gchar *
block_text(const OnDocument *d, guint i)
{
    return on_document_block(d, i)->text->text->str;
}

/* ===========================================================================
 * TESTS — loading and round trips
 * ======================================================================== */

static void
test_empty(void)
{
    OnDocument *d = on_document_from_bnbf(NULL, 0, NULL);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_PARA);
    g_assert_cmpstr(block_text(d, 0), ==, "");
    assert_valid(d);

    /* Saves as header + end marker alone, the same 9 bytes an empty
     * GtkTextBuffer produced.                                              */
    Blob b;
    blob_begin(&b);
    GBytes *empty = blob_end(&b);
    g_assert_cmpuint(g_bytes_get_size(empty), ==, 9);
    assert_roundtrip(d, empty);
    on_document_free(d);

    d = load_clean(empty);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    on_document_free(d);
    g_bytes_unref(empty);
}

static void
test_paragraphs(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "one\ntwo\nthree");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    g_assert_cmpstr(block_text(d, 0), ==, "one");
    g_assert_cmpstr(block_text(d, 1), ==, "two");
    g_assert_cmpstr(block_text(d, 2), ==, "three");
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_trailing_newline(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "one\n");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    g_assert_cmpstr(block_text(d, 1), ==, "");
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_inline_styles(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_BOLD, "ab");
    blob_text(&b, 0, "cd");
    blob_text(&b, ON_FMT_ITALIC | ON_FMT_TAG, "#ef");
    blob_text(&b, 0, "\n");
    blob_text(&b, ON_FMT_STRIKE, "gh");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    OnText *t = on_document_block(d, 0)->text;
    g_assert_cmpstr(t->text->str, ==, "abcd#ef");
    g_assert_cmpuint(t->runs->len, ==, 3);
    g_assert_cmpuint(on_text_flags_at(t, 0), ==, ON_FMT_BOLD);
    g_assert_cmpuint(on_text_flags_at(t, 2), ==, 0);
    g_assert_cmpuint(on_text_flags_at(t, 4), ==, ON_FMT_ITALIC | ON_FMT_TAG);
    g_assert_cmpuint(on_text_flags_at(t, 7), ==, ON_FMT_ITALIC | ON_FMT_TAG);
    g_assert_cmpuint(on_document_block(d, 0)->eol_flags, ==, 0);
    g_assert_cmpuint(on_text_flags_at(on_document_block(d, 1)->text, 0),
                     ==, ON_FMT_STRIKE);
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

/* A newline carrying inline flags (the editor tags Enter like any typed
 * character) is remembered on the block and written back.                  */
static void
test_eol_flags(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_BOLD, "ab\n");
    blob_text(&b, 0, "cd");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_block(d, 0)->eol_flags, ==, ON_FMT_BOLD);
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_headings_and_code(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_H1, "Title\n");
    blob_text(&b, 0, "body\n");
    blob_text(&b, ON_FMT_H2, "Sub\n");
    blob_text(&b, ON_FMT_CODEBLOCK, "l1\nl2\n");
    blob_text(&b, 0, "after");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 6);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_H1);
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_PARA);
    g_assert_cmpint(on_document_block(d, 2)->kind, ==, ON_BLOCK_H2);
    g_assert_cmpint(on_document_block(d, 3)->kind, ==, ON_BLOCK_CODE);
    g_assert_cmpint(on_document_block(d, 4)->kind, ==, ON_BLOCK_CODE);
    g_assert_cmpstr(block_text(d, 4), ==, "l2");
    g_assert_cmpint(on_document_block(d, 5)->kind, ==, ON_BLOCK_PARA);
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

/* An empty line INSIDE a code block: its newline is the only character
 * that carries the style.                                                  */
static void
test_empty_styled_line(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_CODEBLOCK, "a\n\nb");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_CODE);
    g_assert_cmpstr(block_text(d, 1), ==, "");
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_bullets(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_LIST_BULLET, "\xe2\x80\xa2 one\n\xe2\x80\xa2 ");
    blob_text(&b, ON_FMT_LIST_BULLET | ON_FMT_BOLD, "two");
    blob_text(&b, ON_FMT_LIST_BULLET, "\n");
    blob_text(&b, 0, "plain");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_BULLET);
    g_assert_cmpstr(block_text(d, 0), ==, "one");
    g_assert_cmpstr(block_text(d, 1), ==, "two");
    g_assert_cmpuint(on_text_flags_at(on_document_block(d, 1)->text, 0),
                     ==, ON_FMT_BOLD);
    assert_roundtrip(d, blob);

    gchar *plain = on_document_plain_text(d);
    g_assert_cmpstr(plain, ==, "\xe2\x80\xa2 one\n\xe2\x80\xa2 two\nplain");
    g_free(plain);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_numbers(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_LIST_NUMBER, "1. a\n2. b\n");
    blob_text(&b, 0, "gap\n");
    blob_text(&b, ON_FMT_LIST_NUMBER, "1. c");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 4);
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_NUMBER);
    g_assert_cmpstr(block_text(d, 1), ==, "b");
    g_assert_cmpstr(block_text(d, 3), ==, "c");
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);

    /* A wrongly numbered line is reported and comes back renumbered.      */
    blob_begin(&b);
    blob_text(&b, ON_FMT_LIST_NUMBER, "1. a\n7. b");
    blob = blob_end(&b);
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.number_renumbered, ==, 1);
    g_assert_cmpstr(block_text(d, 1), ==, "b");
    on_document_free(d);
    g_bytes_unref(blob);

    blob_begin(&b);
    blob_text(&b, ON_FMT_LIST_NUMBER, "1. a\n2. b");
    GBytes *fixed = blob_end(&b);
    blob_begin(&b);
    blob_text(&b, ON_FMT_LIST_NUMBER, "1. a\n7. b");
    blob = blob_end(&b);
    data = g_bytes_get_data(blob, &n);
    d = on_document_from_bnbf(data, n, NULL);
    assert_roundtrip(d, fixed);
    on_document_free(d);
    g_bytes_unref(blob);
    g_bytes_unref(fixed);
}

static void
test_checks(void)
{
    Blob b;
    blob_begin(&b);
    blob_check(&b, TRUE);
    blob_text(&b, ON_FMT_LIST_CHECK, " done\n");
    blob_check(&b, FALSE);
    blob_text(&b, ON_FMT_LIST_CHECK, " ");
    blob_text(&b, ON_FMT_LIST_CHECK | ON_FMT_BOLD, "todo");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_CHECK);
    g_assert_true(on_document_block(d, 0)->checked);
    g_assert_cmpstr(block_text(d, 0), ==, "done");
    g_assert_false(on_document_block(d, 1)->checked);
    g_assert_cmpstr(block_text(d, 1), ==, "todo");
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);

    /* No space after the box: reported, and the save adds one.            */
    blob_begin(&b);
    blob_check(&b, FALSE);
    blob_text(&b, ON_FMT_LIST_CHECK, "x");
    blob = blob_end(&b);
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.check_no_space, ==, 1);
    g_assert_cmpstr(block_text(d, 0), ==, "x");
    on_document_free(d);
    g_bytes_unref(blob);

    /* A box in the middle of a line splits it.                             */
    blob_begin(&b);
    blob_text(&b, 0, "ab");
    blob_check(&b, TRUE);
    blob_text(&b, ON_FMT_LIST_CHECK, " cd");
    blob = blob_end(&b);
    data = g_bytes_get_data(blob, &n);
    d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.split_check, ==, 1);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    g_assert_cmpstr(block_text(d, 0), ==, "ab");
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_CHECK);
    g_assert_cmpstr(block_text(d, 1), ==, "cd");
    assert_valid(d);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_image_block(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "a\n");
    blob_image(&b, 320, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, "\nb");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    OnBlock *img = on_document_block(d, 1);
    g_assert_cmpint(img->kind, ==, ON_BLOCK_IMAGE);
    g_assert_cmpuint(img->display_width, ==, 320);
    g_assert_cmpuint(g_bytes_get_size(img->png), ==, sizeof PNG_A);
    g_assert_cmpint(on_document_image_count(d), ==, 1);
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);

    /* Two images on consecutive lines, and one as the very last thing.     */
    blob_begin(&b);
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, "\n");
    blob_image(&b, 0, PNG_B, sizeof PNG_B);
    blob = blob_end(&b);
    d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_IMAGE);
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_image_inline(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_BOLD, "ab");
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, ON_FMT_BOLD, "cd");
    blob_image(&b, 100, PNG_B, sizeof PNG_B);
    blob_text(&b, 0, "\nnext");
    GBytes *blob = blob_end(&b);

    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    OnDocument *d = on_document_from_bnbf(data, n, &rep);
    g_assert_true(on_document_load_report_is_clean(&rep));
    g_assert_cmpuint(rep.image_inline, ==, 2);
    assert_valid(d);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    OnText *t = on_document_block(d, 0)->text;
    g_assert_cmpstr(t->text->str, ==, "ab" ON_OBJ_CHAR "cd" ON_OBJ_CHAR);
    g_assert_cmpuint(t->images->len, ==, 2);
    g_assert_cmpuint(g_array_index(t->images, OnInlineImage, 0).offset, ==, 2);
    g_assert_cmpuint(g_array_index(t->images, OnInlineImage, 1).offset, ==, 7);
    g_assert_cmpuint(t->runs->len, ==, 1);    /* U+FFFC took the bold      */
    g_assert_cmpint(on_document_image_count(d), ==, 2);
    guint32 w;
    g_assert_cmpuint(g_bytes_get_size(on_document_image_nth(d, 1, &w)), ==,
                     sizeof PNG_B);
    g_assert_cmpuint(w, ==, 100);
    g_assert_null(on_document_image_nth(d, 2, NULL));
    assert_roundtrip(d, blob);

    gchar *plain = on_document_plain_text(d);
    g_assert_cmpstr(plain, ==, "abcd\nnext");
    g_free(plain);
    on_document_free(d);
    g_bytes_unref(blob);

    /* An image FIRST on a line that then gets text: inline at offset 0.    */
    blob_begin(&b);
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, "tail");
    blob = blob_end(&b);
    data = g_bytes_get_data(blob, &n);
    d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_PARA);
    g_assert_cmpuint(on_document_block(d, 0)->text->images->len, ==, 1);
    assert_valid(d);
    assert_roundtrip(d, blob);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_table(void)
{
    const gchar *cells[] = { "h1", "h2", "a", "b\nc" };
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "x\n");
    blob_table(&b, TRUE, 2, 2, cells);
    blob_text(&b, 0, "\ny");
    GBytes *blob = blob_end(&b);

    OnDocument *d = load_clean(blob);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    OnBlock *t = on_document_block(d, 1);
    g_assert_cmpint(t->kind, ==, ON_BLOCK_TABLE);
    g_assert_true(t->header);
    g_assert_cmpint(t->rows, ==, 2);
    g_assert_cmpstr(on_block_cell(t, 1, 1)->text->str, ==, "b\nc");
    g_assert_null(on_block_cell(t, 2, 0));
    assert_roundtrip(d, blob);
    gchar *plain = on_document_plain_text(d);
    g_assert_cmpstr(plain, ==, "x\nh1 h2 a b\nc \ny");
    g_free(plain);
    on_document_free(d);
    g_bytes_unref(blob);

    /* A table sharing a line with text is split off.                       */
    blob_begin(&b);
    blob_text(&b, 0, "x");
    blob_table(&b, FALSE, 1, 1, cells);
    blob_text(&b, 0, "y");
    blob = blob_end(&b);
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.split_table, ==, 2);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_TABLE);
    g_assert_cmpstr(block_text(d, 2), ==, "y");
    assert_valid(d);
    on_document_free(d);
    g_bytes_unref(blob);
}

/* Adjacent same-flag text across block boundaries saves as ONE record —
 * the maximal-run form the GtkTextBuffer walk produces.                    */
static void
test_run_merge_across_blocks(void)
{
    OnDocument *d = on_document_new();
    on_text_append(on_document_block(d, 0)->text, "a", 1, 0);
    /* Build the second block the way the loader does.                     */
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "a\nb");
    GBytes *blob = blob_end(&b);
    on_document_free(d);
    d = load_clean(blob);
    gsize n;
    guint8 *out = on_document_to_bnbf(d, &n);
    /* header 8 + rec 1 + flags 4 + len 4 + "a\nb" 3 + end 1               */
    g_assert_cmpuint(n, ==, 8 + 1 + 4 + 4 + 3 + 1);
    g_free(out);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_mixed_para(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_H1, "ab");
    blob_text(&b, ON_FMT_H2, "cd\n");
    blob_text(&b, 0, "e");
    GBytes *blob = blob_end(&b);

    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    OnDocument *d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.mixed_para, ==, 1);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_H1);
    assert_valid(d);
    on_document_free(d);
    g_bytes_unref(blob);
}

/* Windows line ends and U+2029 break lines like "\n" does — the editor
 * showed them as separate lines — and come back as "\n".                  */
static void
test_cr_newlines(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_CODEBLOCK, "a\r\nb\rc\xe2\x80\xa9" "d");
    GBytes *blob = blob_end(&b);
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    OnDocument *d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.cr_newlines, ==, 3);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 4);
    g_assert_cmpstr(block_text(d, 1), ==, "b");
    g_assert_cmpstr(block_text(d, 3), ==, "d");
    assert_valid(d);

    blob_begin(&b);
    blob_text(&b, ON_FMT_CODEBLOCK, "a\nb\nc\nd");
    GBytes *fixed = blob_end(&b);
    assert_roundtrip(d, fixed);
    g_bytes_unref(fixed);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_unknown_flags(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 1u << 20, "ab");
    GBytes *blob = blob_end(&b);

    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocLoadReport rep;
    OnDocument *d = on_document_from_bnbf(data, n, &rep);
    g_assert_cmpuint(rep.unknown_flags, ==, 1);
    g_assert_cmpuint(on_text_flags_at(on_document_block(d, 0)->text, 0),
                     ==, 0);
    assert_valid(d);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_malformed(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "ok\nnext");
    GBytes *blob = blob_end(&b);
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);

    /* Cut inside the TEXT record: nothing parses, the document is empty
     * but valid, and the error is reported.                                */
    OnDocLoadReport rep;
    OnDocument *d = on_document_from_bnbf(data, n - 4, &rep);
    g_assert_nonnull(rep.error);
    assert_valid(d);
    on_document_free(d);

    /* Missing end marker: everything before it parses.                    */
    d = on_document_from_bnbf(data, n - 1, &rep);
    g_assert_cmpstr(rep.error, ==, "missing end marker");
    g_assert_cmpuint(on_document_n_blocks(d), ==, 2);
    on_document_free(d);

    /* Wrong magic.                                                         */
    d = on_document_from_bnbf((const guint8 *)"nope nope", 9, &rep);
    g_assert_nonnull(rep.error);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    on_document_free(d);
    g_bytes_unref(blob);
}

/* ===========================================================================
 * TESTS — OnText operations
 * ======================================================================== */

static void
test_text_ops(void)
{
    OnDocument *d = on_document_new();
    OnText *t = on_document_block(d, 0)->text;

    on_text_append(t, "hello", 5, 0);
    on_text_append(t, " world", 6, 0);
    g_assert_cmpuint(t->runs->len, ==, 1);          /* merged             */
    on_text_set_flags(t, 0, 5, ON_FMT_BOLD, TRUE);
    g_assert_cmpuint(t->runs->len, ==, 2);
    g_assert_cmpuint(on_text_flags_at(t, 4), ==, ON_FMT_BOLD);
    g_assert_cmpuint(on_text_flags_at(t, 5), ==, 0);
    assert_valid(d);

    /* Insert INSIDE the bold run with other flags: it splits.             */
    on_text_insert(t, 2, "XY", 2, ON_FMT_ITALIC);
    g_assert_cmpstr(t->text->str, ==, "heXYllo world");
    g_assert_cmpuint(t->runs->len, ==, 4);
    g_assert_cmpuint(on_text_flags_at(t, 2), ==, ON_FMT_ITALIC);
    g_assert_cmpuint(on_text_flags_at(t, 4), ==, ON_FMT_BOLD);
    assert_valid(d);

    /* Delete across the split: the bold halves re-merge.                  */
    on_text_delete(t, 2, 2);
    g_assert_cmpstr(t->text->str, ==, "hello world");
    g_assert_cmpuint(t->runs->len, ==, 2);
    assert_valid(d);

    /* Clear the bit over part of the run.                                  */
    on_text_set_flags(t, 0, 2, ON_FMT_BOLD, FALSE);
    g_assert_cmpuint(t->runs->len, ==, 3);
    g_assert_cmpuint(on_text_flags_at(t, 1), ==, 0);
    g_assert_cmpuint(on_text_flags_at(t, 2), ==, ON_FMT_BOLD);
    assert_valid(d);

    /* Paragraph bits never land in a run.                                  */
    on_text_set_flags(t, 0, 11, ON_FMT_H1 | ON_FMT_UNDERLINE, TRUE);
    g_assert_cmpuint(on_text_flags_at(t, 0) & ON_FMT_H1, ==, 0);
    g_assert_cmpuint(on_text_flags_at(t, 0) & ON_FMT_UNDERLINE, !=, 0);
    assert_valid(d);

    /* Multi-byte characters: boundaries land on characters.               */
    on_text_delete(t, 0, 11);
    on_text_append(t, "\xe2\x80\xa2x", 4, 0);
    on_text_set_flags(t, 3, 1, ON_FMT_BOLD, TRUE);
    assert_valid(d);
    g_assert_cmpuint(t->runs->len, ==, 2);

    /* Inline image: shifts with insertions, dies with its span.           */
    GBytes *png = g_bytes_new_static(PNG_A, sizeof PNG_A);
    on_text_append(t, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN, 0);
    on_text_add_image(t, 4, png, 0);
    assert_valid(d);
    on_text_insert(t, 0, "Q", 1, 0);
    g_assert_cmpuint(g_array_index(t->images, OnInlineImage, 0).offset, ==, 5);
    assert_valid(d);
    on_text_delete(t, 5, ON_OBJ_CHAR_LEN);
    g_assert_cmpuint(t->images->len, ==, 0);
    assert_valid(d);
    g_bytes_unref(png);

    on_document_free(d);
}

static void
test_check_catches(void)
{
    OnDocument *d = on_document_new();
    OnText *t = on_document_block(d, 0)->text;
    on_text_append(t, "ab", 2, 0);
    assert_valid(d);

    /* Break a run by hand and expect the checker to say so.               */
    g_array_index(t->runs, OnRun, 0).len = 1;
    GString *why = g_string_new(NULL);
    g_assert_false(on_document_check(d, why));
    g_assert_nonnull(strstr(why->str, "runs cover"));
    g_array_index(t->runs, OnRun, 0).len = 2;

    /* A stray U+FFFC with no image.                                        */
    g_string_append(t->text, ON_OBJ_CHAR);
    g_array_index(t->runs, OnRun, 0).len = 5;
    g_assert_false(on_document_check(d, why));
    g_assert_nonnull(strstr(why->str, "U+FFFC"));
    g_string_free(why, TRUE);
    on_document_free(d);
}

static void
test_block_copy(void)
{
    const gchar *cells[] = { "a", "b" };
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_BOLD, "x");
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, "\n");
    blob_table(&b, FALSE, 1, 2, cells);
    blob_text(&b, 0, "\n");
    blob_image(&b, 7, PNG_B, sizeof PNG_B);
    GBytes *blob = blob_end(&b);
    gsize n;
    const guint8 *data = g_bytes_get_data(blob, &n);
    OnDocument *d = on_document_from_bnbf(data, n, NULL);
    assert_valid(d);

    OnBlock *text = on_block_copy(on_document_block(d, 0));
    g_assert_cmpstr(text->text->text->str, ==, "x" ON_OBJ_CHAR);
    g_assert_cmpuint(text->text->images->len, ==, 1);
    g_assert_cmpuint(text->text->runs->len, ==, 1);
    on_block_free(text);

    OnBlock *table = on_block_copy(on_document_block(d, 1));
    g_assert_cmpint(table->kind, ==, ON_BLOCK_TABLE);
    g_assert_cmpstr(on_block_cell(table, 0, 1)->text->str, ==, "b");
    on_block_free(table);

    OnBlock *img = on_block_copy(on_document_block(d, 2));
    g_assert_cmpint(img->kind, ==, ON_BLOCK_IMAGE);
    g_assert_cmpuint(img->display_width, ==, 7);
    g_assert_true(g_bytes_equal(img->png, on_document_block(d, 2)->png));
    on_block_free(img);

    on_document_free(d);
    g_bytes_unref(blob);
}


/* ===========================================================================
 * TESTS — operations and undo
 * ======================================================================== */

/* snapshot() — the document's bytes, for before/after comparison.          */
static GBytes *
snapshot(const OnDocument *d)
{
    gsize n;
    guint8 *out = on_document_to_bnbf(d, &n);
    return g_bytes_new_take(out, n);
}

static OnPos
at(guint block, gsize offset)
{
    OnPos p = { block, -1, offset };
    return p;
}

static OnPos
cell_at(guint block, gint cell, gsize offset)
{
    OnPos p = { block, cell, offset };
    return p;
}

typedef struct {
    guint changed, inserted, removed;
} Counts;

static void
obs_changed(OnDocument *d, guint i, gpointer data)
{
    (void)d; (void)i;
    ((Counts *)data)->changed++;
}

static void
obs_inserted(OnDocument *d, guint i, guint n, gpointer data)
{
    (void)d; (void)i;
    ((Counts *)data)->inserted += n;
}

static void
obs_removed(OnDocument *d, guint i, guint n, gpointer data)
{
    (void)d; (void)i;
    ((Counts *)data)->removed += n;
}

static const OnDocumentObserver COUNTING = {
    obs_changed, obs_inserted, obs_removed,
};

static void
test_ops_text(void)
{
    OnDocument *d = on_document_new();
    Counts c = { 0, 0, 0 };
    on_document_set_observer(d, &COUNTING, &c);
    GBytes *empty = snapshot(d);

    g_assert_true(on_document_insert_text(d, at(0, 0), "hello", 5, 0));
    g_assert_true(on_document_insert_text(d, at(0, 5), " #tag", 5, 0));
    g_assert_true(on_document_set_flags(d, at(0, 6), 4, ON_FMT_TAG, TRUE));
    g_assert_true(on_document_set_flags(d, at(0, 0), 5, ON_FMT_BOLD, TRUE));
    g_assert_cmpstr(block_text(d, 0), ==, "hello #tag");
    g_assert_cmpuint(on_document_block(d, 0)->text->runs->len, ==, 3);
    g_assert_cmpuint(c.changed, ==, 4);
    assert_valid(d);
    g_assert_true(on_document_take_tags_modified(d));
    g_assert_false(on_document_take_tags_modified(d));
    g_assert_false(on_document_take_actions_modified(d));
    GBytes *full = snapshot(d);

    /* Refusals.                                                            */
    g_assert_false(on_document_insert_text(d, at(0, 0), "a\nb", 3, 0));
    g_assert_false(on_document_insert_text(d, at(0, 0), ON_OBJ_CHAR, 3, 0));
    g_assert_false(on_document_insert_text(d, at(0, 11), "x", 1, 0));
    g_assert_false(on_document_insert_text(d, at(1, 0), "x", 1, 0));
    g_assert_false(on_document_delete_text(d, at(0, 8), 5));
    g_assert_false(on_document_set_flags(d, at(0, 0), 1, ON_FMT_H1, TRUE));

    /* Four ops, four undo steps, back to empty; four redos, back to full. */
    for (gint i = 0; i < 4; i++)
        g_assert_true(on_document_undo(d));
    g_assert_false(on_document_undo(d));
    assert_valid(d);
    assert_roundtrip(d, empty);
    for (gint i = 0; i < 4; i++)
        g_assert_true(on_document_redo(d));
    g_assert_false(on_document_redo(d));
    assert_roundtrip(d, full);
    assert_valid(d);

    /* A delete spanning an inline image, undone, brings the image back.   */
    GBytes *png = g_bytes_new_static(PNG_A, sizeof PNG_A);
    OnText *t = on_document_block(d, 0)->text;
    on_text_insert(t, 5, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN, 0);
    on_text_add_image(t, 5, png, 44);
    on_document_clear_undo(d);
    GBytes *with_img = snapshot(d);
    g_assert_true(on_document_delete_text(d, at(0, 3), 6));  /* "lo" img " " */
    g_assert_cmpstr(block_text(d, 0), ==, "hel#tag");
    g_assert_cmpuint(t->images->len, ==, 0);
    assert_valid(d);
    g_assert_true(on_document_undo(d));
    g_assert_cmpuint(t->images->len, ==, 1);
    g_assert_cmpuint(g_array_index(t->images, OnInlineImage, 0).display_width,
                     ==, 44);
    assert_roundtrip(d, with_img);
    g_bytes_unref(png);

    /* A multi-byte boundary is refused.                                    */
    on_document_clear_undo(d);
    g_assert_true(on_document_insert_text(d, at(0, 0), "\xe2\x80\xa2", 3, 0));
    g_assert_false(on_document_delete_text(d, at(0, 1), 1));
    g_assert_false(on_document_insert_text(d, at(0, 2), "x", 1, 0));

    g_bytes_unref(with_img);
    g_bytes_unref(full);
    g_bytes_unref(empty);
    on_document_free(d);
}

static void
test_ops_blocks(void)
{
    Blob b;
    blob_begin(&b);
    blob_check(&b, TRUE);
    blob_text(&b, ON_FMT_LIST_CHECK, " one ");
    blob_text(&b, ON_FMT_LIST_CHECK | ON_FMT_BOLD, "two");
    blob_text(&b, ON_FMT_LIST_CHECK | ON_FMT_ITALIC, "\n");
    blob_text(&b, 0, "! action");
    GBytes *blob = blob_end(&b);
    OnDocument *d = load_clean(blob);
    on_document_block(d, 0)->action_uid = 7;
    on_document_block(d, 1)->action_uid = 9;
    Counts c = { 0, 0, 0 };
    on_document_set_observer(d, &COUNTING, &c);
    on_document_take_actions_modified(d);

    /* Split "one |two": both halves CHECK, the tail bold, the first block's
     * newline takes the armed flags, the second keeps the old one.         */
    g_assert_true(on_document_split_block(d, at(0, 4), ON_FMT_BOLD));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 3);
    g_assert_cmpstr(block_text(d, 0), ==, "one ");
    g_assert_cmpstr(block_text(d, 1), ==, "two");
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_CHECK);
    g_assert_false(on_document_block(d, 1)->checked);
    g_assert_cmpuint(on_document_block(d, 0)->eol_flags, ==, ON_FMT_BOLD);
    g_assert_cmpuint(on_document_block(d, 1)->eol_flags, ==, ON_FMT_ITALIC);
    g_assert_cmpuint(on_text_flags_at(on_document_block(d, 1)->text, 0),
                     ==, ON_FMT_BOLD);
    g_assert_cmpint(on_document_block(d, 0)->action_uid, ==, 7);
    g_assert_cmpint(on_document_block(d, 2)->action_uid, ==, 9);
    g_assert_cmpuint(c.inserted, ==, 1);
    g_assert_true(on_document_take_actions_modified(d));
    assert_valid(d);

    /* Undo restores the exact bytes.                                       */
    g_assert_true(on_document_undo(d));
    assert_roundtrip(d, blob);
    g_assert_cmpuint(c.removed, ==, 1);
    g_assert_true(on_document_block(d, 0)->checked);
    g_assert_cmpint(on_document_block(d, 0)->action_uid, ==, 7);

    /* Join the check line with the action line: kind of the first kept,
     * newline flags of the second taken; undo brings the second block
     * back with its kind, state and uid.                                   */
    g_assert_true(on_document_join_blocks(d, 0));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_cmpstr(block_text(d, 0), ==, "one two! action");
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_CHECK);
    g_assert_cmpuint(on_document_block(d, 0)->eol_flags, ==, 0);
    assert_valid(d);
    g_assert_true(on_document_undo(d));
    assert_roundtrip(d, blob);
    g_assert_cmpint(on_document_block(d, 1)->action_uid, ==, 9);
    g_assert_true(on_document_redo(d));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_true(on_document_undo(d));

    /* Kind, checked, eol.                                                  */
    g_assert_true(on_document_set_kind(d, 1, ON_BLOCK_H2));
    g_assert_true(on_document_set_checked(d, 0, FALSE));
    g_assert_true(on_document_set_eol_flags(d, 1, ON_FMT_STRIKE));
    g_assert_false(on_document_set_checked(d, 1, TRUE));   /* not a CHECK */
    g_assert_false(on_document_set_kind(d, 0, ON_BLOCK_IMAGE));
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_H2);
    assert_valid(d);
    on_document_undo(d);
    on_document_undo(d);
    on_document_undo(d);
    assert_roundtrip(d, blob);

    /* Insert and remove blocks; the last one cannot go.                    */
    GBytes *png = g_bytes_new_static(PNG_B, sizeof PNG_B);
    g_assert_true(on_document_insert_block(d, 1, on_block_new_image(png, 3)));
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_IMAGE);
    g_assert_cmpint(on_document_image_count(d), ==, 1);
    g_assert_true(on_document_remove_block(d, 0));
    g_assert_true(on_document_remove_block(d, 1));
    g_assert_false(on_document_remove_block(d, 0));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_IMAGE);
    assert_valid(d);
    on_document_undo(d);
    on_document_undo(d);
    on_document_undo(d);
    assert_roundtrip(d, blob);
    g_bytes_unref(png);

    g_bytes_unref(blob);
    on_document_free(d);
}

static void
test_ops_table(void)
{
    const gchar *cells[] = { "a", "b", "c", "d" };
    Blob b;
    blob_begin(&b);
    blob_table(&b, FALSE, 2, 2, cells);
    GBytes *blob = blob_end(&b);
    OnDocument *d = load_clean(blob);

    g_assert_true(on_document_insert_text(d, cell_at(0, 3, 1), "\nx", 2, 0));
    g_assert_cmpstr(on_block_cell(on_document_block(d, 0), 1, 1)->text->str,
                    ==, "d\nx");
    g_assert_true(on_document_table_insert_row(d, 0, 1));
    g_assert_true(on_document_table_insert_col(d, 0, 0));
    g_assert_cmpint(on_document_block(d, 0)->rows, ==, 3);
    g_assert_cmpint(on_document_block(d, 0)->cols, ==, 3);
    g_assert_cmpstr(on_block_cell(on_document_block(d, 0), 2, 2)->text->str,
                    ==, "d\nx");
    g_assert_cmpstr(on_block_cell(on_document_block(d, 0), 1, 1)->text->str,
                    ==, "");
    assert_valid(d);
    g_assert_true(on_document_insert_text(d, cell_at(0, 4, 0), "mid", 3, 0));
    g_assert_true(on_document_table_remove_row(d, 0, 0));
    g_assert_true(on_document_table_remove_col(d, 0, 2));
    g_assert_cmpstr(on_block_cell(on_document_block(d, 0), 0, 1)->text->str,
                    ==, "mid");
    g_assert_true(on_document_table_set_header(d, 0, TRUE));
    assert_valid(d);
    GBytes *after = snapshot(d);

    /* Never below 1×1.                                                     */
    g_assert_true(on_document_table_remove_col(d, 0, 0));
    g_assert_false(on_document_table_remove_col(d, 0, 0));
    g_assert_true(on_document_table_remove_row(d, 0, 1));
    g_assert_false(on_document_table_remove_row(d, 0, 0));
    g_assert_false(on_document_table_insert_row(d, 0, 2));
    assert_valid(d);
    GBytes *final = snapshot(d);
    on_document_undo(d);
    on_document_undo(d);
    assert_roundtrip(d, after);

    /* All the way back, then all the way forward — through the two steps
     * just undone as well, which redo still holds.                         */
    while (on_document_undo(d))
        assert_valid(d);
    assert_roundtrip(d, blob);
    while (on_document_redo(d))
        assert_valid(d);
    assert_roundtrip(d, final);

    g_bytes_unref(final);
    g_bytes_unref(after);
    g_bytes_unref(blob);
    on_document_free(d);
}

static void
test_ops_groups(void)
{
    OnDocument *d = on_document_new();
    GBytes *empty = snapshot(d);

    on_document_begin_group(d);
    on_document_insert_text(d, at(0, 0), "a", 1, 0);
    on_document_begin_group(d);          /* nested                        */
    on_document_insert_text(d, at(0, 1), "b", 1, 0);
    on_document_end_group(d);
    on_document_insert_text(d, at(0, 2), "c", 1, 0);
    on_document_end_group(d);
    g_assert_cmpstr(block_text(d, 0), ==, "abc");
    g_assert_true(on_document_can_undo(d));
    g_assert_true(on_document_undo(d));
    assert_roundtrip(d, empty);
    g_assert_false(on_document_can_undo(d));
    g_assert_true(on_document_redo(d));
    g_assert_cmpstr(block_text(d, 0), ==, "abc");

    /* An open group (typing in progress) is closed by undo itself.        */
    on_document_begin_group(d);
    on_document_insert_text(d, at(0, 3), "d", 1, 0);
    on_document_insert_text(d, at(0, 4), "e", 1, 0);
    g_assert_true(on_document_undo(d));
    g_assert_cmpstr(block_text(d, 0), ==, "abc");

    /* A new op after undo drops the redo history.                          */
    on_document_insert_text(d, at(0, 0), "z", 1, 0);
    g_assert_false(on_document_can_redo(d));

    g_bytes_unref(empty);
    on_document_free(d);
}

/* Random operations, invariants checked after each; then undo everything
 * back to the start and redo everything back to the end, byte for byte.   */
static void
test_ops_fuzz(void)
{
    const gchar *cells[] = { "p", "q" };
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_H1, "Title\n");
    blob_text(&b, 0, "some ");
    blob_text(&b, ON_FMT_BOLD, "bold");
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, " text\n");
    blob_check(&b, FALSE);
    blob_text(&b, ON_FMT_LIST_CHECK, " task\n");
    blob_table(&b, TRUE, 1, 2, cells);
    blob_text(&b, 0, "\n");
    blob_image(&b, 50, PNG_B, sizeof PNG_B);
    blob_text(&b, 0, "\n! do it");
    GBytes *blob = blob_end(&b);
    OnDocument *d = load_clean(blob);
    GRand *rng = g_rand_new_with_seed(g_test_rand_int());  /* --seed reproduces */
    const gchar *words[] = { "x", "yy", "#tag", "! ", "\xe2\x80\xa2", "zz " };
    GPtrArray *snaps = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_bytes_unref);
    g_ptr_array_add(snaps, snapshot(d));

    for (gint step = 0; step < 300; step++) {
        guint n = on_document_n_blocks(d);
        guint i = g_rand_int_range(rng, 0, (gint)n);
        OnBlock *blk = on_document_block(d, i);
        OnPos pos = at(i, 0);
        if (blk->kind == ON_BLOCK_TABLE)
            pos.cell = g_rand_int_range(rng, 0, blk->rows * blk->cols);
        OnText *t = on_document_text_at(d, pos);
        gsize len = (t != NULL) ? t->text->len : 0;
        /* A character boundary at or before a random byte.                */
        pos.offset = (len > 0) ? (gsize)g_rand_int_range(rng, 0, (gint)len + 1)
                               : 0;
        while (pos.offset > 0 && pos.offset < len &&
               ((guchar)t->text->str[pos.offset] & 0xc0) == 0x80)
            pos.offset--;
        gsize span = 0;
        if (len > pos.offset) {
            span = (gsize)g_rand_int_range(rng, 0, (gint)(len - pos.offset) + 1);
            while (pos.offset + span < len &&
                   ((guchar)t->text->str[pos.offset + span] & 0xc0) == 0x80)
                span++;
        }
        guint depth = on_document_undo_depth(d);
        gboolean did = FALSE;        /* the call was accepted (it may still
                                        have been a no-op: see below)       */
        switch (g_rand_int_range(rng, 0, 16)) {
        case 0: case 1: case 2: {
            const gchar *w = words[g_rand_int_range(rng, 0, 6)];
            guint32 f = g_rand_boolean(rng) ? ON_FMT_BOLD : 0;
            if (t != NULL)
                did = on_document_insert_text(d, pos, w, strlen(w), f);
            break;
        }
        case 3: case 4:
            if (t != NULL && span > 0)
                did = on_document_delete_text(d, pos, span);
            break;
        case 5:
            if (t != NULL && span > 0)
                did = on_document_set_flags(d, pos, span,
                                            1u << g_rand_int_range(rng, 0, 4),
                                            g_rand_boolean(rng));
            break;
        case 6:
            if (pos.cell < 0 && t != NULL)
                did = on_document_split_block(d, pos, ON_FMT_ITALIC);
            break;
        case 7:
            did = on_document_join_blocks(d, i);
            break;
        case 8:
            did = on_document_set_kind(d, i, g_rand_int_range(rng, 0, 7));
            break;
        case 9:
            did = on_document_set_checked(d, i, g_rand_boolean(rng));
            break;
        case 10:
            did = on_document_insert_block(
                d, g_rand_int_range(rng, 0, (gint)n + 1),
                g_rand_boolean(rng) ? on_block_new_text(ON_BLOCK_CODE)
                                    : on_block_new_table(2, 2));
            break;
        case 11:
            did = on_document_remove_block(d, i);
            break;
        case 12:
            did = on_document_table_insert_row(d, i, 0) ||
                  on_document_table_insert_col(d, i, 1);
            break;
        case 13:
            did = on_document_table_remove_col(d, i, 0) ||
                  on_document_table_remove_row(d, i, 0);
            break;
        case 14: {
            guint j = g_rand_int_range(rng, 0, (gint)n);
            OnPos q = at(j, 0);
            OnText *tj = on_document_text_at(d, q);
            if (tj != NULL && tj->text->len > 0) {
                q.offset = (gsize)g_rand_int_range(rng, 0,
                                                   (gint)tj->text->len + 1);
                while (q.offset > 0 && q.offset < tj->text->len &&
                       ((guchar)tj->text->str[q.offset] & 0xc0) == 0x80)
                    q.offset--;
            } else if (tj == NULL) {
                q.offset = g_rand_boolean(rng);
            }
            OnPos p2 = pos;
            if (p2.cell >= 0) {
                p2.cell = -1;
                p2.offset = g_rand_boolean(rng);
            }
            did = on_document_delete_range(d, p2, q, NULL);
            break;
        }
        case 15:
            if (t != NULL && pos.cell < 0) {
                OnDocument *frag = on_document_copy_range(
                    d, at(0, 0), at(n - 1, 0));
                did = on_document_insert_fragment(d, pos, frag, NULL);
                on_document_free(frag);
            }
            break;
        }
        assert_valid(d);
        /* One snapshot per undo STEP: a set_* that changed nothing is
         * accepted but logs nothing.                                       */
        g_assert_true(did || on_document_undo_depth(d) == depth);
        if (on_document_undo_depth(d) > depth)
            g_ptr_array_add(snaps, snapshot(d));
    }
    g_assert_cmpuint(snaps->len, >, 120);

    /* Every undo lands on the snapshot taken before that op.              */
    for (guint k = snaps->len - 1; k > 0; k--) {
        assert_roundtrip(d, g_ptr_array_index(snaps, k));
        g_assert_true(on_document_undo(d));
        assert_valid(d);
    }
    assert_roundtrip(d, blob);
    g_assert_false(on_document_undo(d));
    for (guint k = 1; k < snaps->len; k++) {
        g_assert_true(on_document_redo(d));
        assert_valid(d);
        assert_roundtrip(d, g_ptr_array_index(snaps, k));
    }
    g_assert_false(on_document_redo(d));

    g_ptr_array_free(snaps, TRUE);
    g_rand_free(rng);
    g_bytes_unref(blob);
    on_document_free(d);
}


/* ===========================================================================
 * TESTS — derived reads and the headless rewrites
 * ======================================================================== */

static void
test_title(void)
{
    OnDocument *d = on_document_from_text("\n\n  Hello world  \nbody");
    gchar *t = on_document_title(d, "New Note", 80);
    g_assert_cmpstr(t, ==, "Hello world");
    g_free(t);
    on_document_free(d);

    d = on_document_from_text("");
    t = on_document_title(d, "New Note", 80);
    g_assert_cmpstr(t, ==, "New Note");
    g_free(t);
    on_document_free(d);

    /* A whitespace-only first line is "a line" and yields the fallback,
     * as it did on the buffer; the cut is in characters.                  */
    d = on_document_from_text("   \nreal");
    t = on_document_title(d, "New Note", 80);
    g_assert_cmpstr(t, ==, "New Note");
    g_free(t);
    on_document_free(d);
    d = on_document_from_text("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");
    t = on_document_title(d, "New Note", 3);
    g_assert_cmpstr(t, ==, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");
    g_free(t);
    on_document_free(d);

    /* An image line first: a line with no text on it — the fallback.      */
    Blob b;
    blob_begin(&b);
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, "\n");
    blob_text(&b, ON_FMT_LIST_BULLET, "\xe2\x80\xa2 item");
    GBytes *blob = blob_end(&b);
    d = load_clean(blob);
    t = on_document_title(d, "New Note", 80);
    g_assert_cmpstr(t, ==, "New Note");
    g_free(t);
    on_document_free(d);
    g_bytes_unref(blob);

    /* A bullet line first: the prefix is part of the title, as the
     * buffer's text was.                                                   */
    blob_begin(&b);
    blob_text(&b, ON_FMT_LIST_BULLET, "\xe2\x80\xa2 item");
    blob = blob_end(&b);
    d = load_clean(blob);
    t = on_document_title(d, "New Note", 80);
    g_assert_cmpstr(t, ==, "\xe2\x80\xa2 item");
    g_free(t);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_from_text(void)
{
    OnDocument *d = on_document_from_text("a\r\nb\rc\n");
    g_assert_cmpuint(on_document_n_blocks(d), ==, 4);
    g_assert_cmpstr(block_text(d, 1), ==, "b");
    g_assert_cmpstr(block_text(d, 3), ==, "");
    assert_valid(d);
    on_document_free(d);
    d = on_document_from_text(NULL);
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    assert_valid(d);
    on_document_free(d);
}

static void
test_collect_tags(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "a ");
    blob_text(&b, ON_FMT_TAG, "#work");
    blob_text(&b, 0, " b ");
    blob_text(&b, ON_FMT_TAG, " #home ");
    blob_text(&b, 0, "\n");
    blob_text(&b, ON_FMT_TAG | ON_FMT_BOLD, "#work");
    blob_text(&b, 0, " ");
    blob_text(&b, ON_FMT_TAG, "#");
    /* One tag split over three runs by a style change inside it.         */
    blob_text(&b, 0, " ");
    blob_text(&b, ON_FMT_TAG, "#he");
    blob_text(&b, ON_FMT_TAG | ON_FMT_ITALIC, "ll");
    blob_text(&b, ON_FMT_TAG, "o");
    GBytes *blob = blob_end(&b);
    OnDocument *d = load_clean(blob);
    GList *tags = on_document_collect_tags(d);
    g_assert_cmpuint(g_list_length(tags), ==, 3);
    g_assert_cmpstr(tags->data, ==, "work");
    g_assert_cmpstr(tags->next->data, ==, "home");
    g_assert_cmpstr(tags->next->next->data, ==, "hello");
    g_list_free_full(tags, g_free);
    on_document_free(d);
    g_bytes_unref(blob);
}

static void
test_action_rewrites(void)
{
    Blob b;
    blob_begin(&b);
    blob_text(&b, 0, "Title\n!\n! due 2026-01-01\n!  first item  due 2026-02-03\n");
    blob_text(&b, ON_FMT_CODEBLOCK, "! not one\n");
    blob_text(&b, 0, "! ");
    blob_text(&b, ON_FMT_STRIKE, "done item");
    GBytes *blob = blob_end(&b);
    OnDocument *d = load_clean(blob);

    GArray *ords = on_document_action_blocks(d);
    g_assert_cmpuint(ords->len, ==, 2);
    g_assert_cmpuint(g_array_index(ords, guint, 0), ==, 3);
    g_assert_cmpuint(g_array_index(ords, guint, 1), ==, 5);
    g_array_unref(ords);
    g_assert_false(on_document_action_strike(d, 2, TRUE));
    g_assert_false(on_document_action_strike(d, -1, TRUE));

    /* Strike: everything after the '!'.                                    */
    g_assert_true(on_document_action_strike(d, 0, TRUE));
    OnText *t = on_document_block(d, 3)->text;
    g_assert_cmpuint(on_text_flags_at(t, 1), ==, ON_FMT_STRIKE);
    g_assert_cmpuint(on_text_flags_at(t, t->text->len - 1), ==, ON_FMT_STRIKE);
    g_assert_cmpuint(on_text_flags_at(t, 0), ==, 0);
    assert_valid(d);

    /* Due: replaces the suffix, keeps the text's spacing and strike.       */
    GDateTime *dt = g_date_time_new_local(2027, 3, 4, 0, 0, 0);
    g_assert_true(on_document_action_due(d, 0, g_date_time_to_unix(dt)));
    g_date_time_unref(dt);
    g_assert_cmpstr(block_text(d, 3), ==, "!  first item due 2027-03-04");
    g_assert_cmpuint(on_text_flags_at(t, t->text->len - 1), ==, ON_FMT_STRIKE);
    g_assert_true(on_document_action_due(d, 0, 0));
    g_assert_cmpstr(block_text(d, 3), ==, "!  first item");
    assert_valid(d);

    /* Text: only the item text changes; the '!' and spacing survive, the
     * strike state carries over.                                           */
    g_assert_true(on_document_action_strike(d, 0, FALSE));
    dt = g_date_time_new_local(2027, 3, 4, 0, 0, 0);
    on_document_action_due(d, 0, g_date_time_to_unix(dt));
    g_date_time_unref(dt);
    g_assert_true(on_document_action_text(d, 0, "renamed"));
    g_assert_cmpstr(block_text(d, 3), ==, "!  renamed due 2027-03-04");
    g_assert_cmpuint(on_text_flags_at(t, 3), ==, 0);
    g_assert_true(on_document_action_text(d, 1, "still done"));
    g_assert_cmpstr(block_text(d, 5), ==, "! still done");
    g_assert_cmpuint(on_text_flags_at(on_document_block(d, 5)->text, 2),
                     ==, ON_FMT_STRIKE);
    assert_valid(d);

    /* Every rewrite was one undo step.                                     */
    while (on_document_undo(d))
        assert_valid(d);
    assert_roundtrip(d, blob);

    on_document_free(d);
    g_bytes_unref(blob);
}


/* ===========================================================================
 * TESTS — ranges and fragments
 * ======================================================================== */

static void
test_ranges(void)
{
    const gchar *cells[] = { "c1", "c2" };
    Blob b;
    blob_begin(&b);
    blob_text(&b, ON_FMT_H1, "Title\n");
    blob_text(&b, 0, "one ");
    blob_text(&b, ON_FMT_BOLD, "two");
    blob_text(&b, 0, "\n");
    blob_image(&b, 0, PNG_A, sizeof PNG_A);
    blob_text(&b, 0, "\n");
    blob_table(&b, FALSE, 1, 2, cells);
    blob_text(&b, 0, "\nlast");
    GBytes *blob = blob_end(&b);
    OnDocument *d = load_clean(blob);

    /* Copy across blocks: partial edges, whole middle, kinds kept.        */
    OnDocument *c = on_document_copy_range(d, at(0, 2), at(1, 5));
    g_assert_cmpuint(on_document_n_blocks(c), ==, 2);
    g_assert_cmpint(on_document_block(c, 0)->kind, ==, ON_BLOCK_H1);
    g_assert_cmpstr(block_text(c, 0), ==, "tle");
    g_assert_cmpstr(block_text(c, 1), ==, "one t");
    g_assert_cmpuint(on_text_flags_at(on_document_block(c, 1)->text, 4),
                     ==, ON_FMT_BOLD);
    assert_valid(c);
    on_document_free(c);

    /* A range reaching across the image and the table takes both; one
     * that stops before the image leaves it.                               */
    c = on_document_copy_range(d, at(1, 0), at(4, 2));
    g_assert_cmpuint(on_document_n_blocks(c), ==, 4);
    g_assert_cmpint(on_document_block(c, 1)->kind, ==, ON_BLOCK_IMAGE);
    g_assert_cmpint(on_document_block(c, 2)->kind, ==, ON_BLOCK_TABLE);
    g_assert_cmpstr(block_text(c, 3), ==, "la");
    on_document_free(c);
    c = on_document_copy_range(d, at(1, 0), at(2, 0));
    g_assert_cmpuint(on_document_n_blocks(c), ==, 1);
    on_document_free(c);
    c = on_document_copy_range(d, cell_at(3, 1, 0), cell_at(3, 1, 1));
    g_assert_cmpstr(block_text(c, 0), ==, "c");
    on_document_free(c);
    /* A range ending INSIDE a cell takes the table — what delete_range
     * removes for the same range, so a Cut across the edge round-trips;
     * one ending at the very start of the first cell stops before it.    */
    c = on_document_copy_range(d, at(2, 1), cell_at(3, 0, 1));
    g_assert_cmpuint(on_document_n_blocks(c), ==, 1);
    g_assert_cmpint(on_document_block(c, 0)->kind, ==, ON_BLOCK_TABLE);
    on_document_free(c);
    c = on_document_copy_range(d, at(1, 0), cell_at(3, 0, 0));
    g_assert_cmpuint(on_document_n_blocks(c), ==, 2);
    g_assert_cmpint(on_document_block(c, 1)->kind, ==, ON_BLOCK_IMAGE);
    on_document_free(c);
    c = on_document_copy_range(d, cell_at(3, 1, 1), at(4, 1));
    g_assert_cmpuint(on_document_n_blocks(c), ==, 2);
    g_assert_cmpint(on_document_block(c, 0)->kind, ==, ON_BLOCK_TABLE);
    g_assert_cmpstr(block_text(c, 1), ==, "l");
    on_document_free(c);

    /* Delete inside one block; undo restores it.                           */
    OnPos after;
    g_assert_true(on_document_delete_range(d, at(1, 1), at(1, 5), &after));
    g_assert_cmpstr(block_text(d, 1), ==, "owo");
    g_assert_cmpuint(after.offset, ==, 1);
    assert_valid(d);
    on_document_undo(d);
    assert_roundtrip(d, blob);

    /* Delete across blocks: the edges join, the image goes.                */
    g_assert_true(on_document_delete_range(d, at(0, 3), at(4, 2), &after));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_cmpstr(block_text(d, 0), ==, "Titst");
    g_assert_cmpint(on_document_block(d, 0)->kind, ==, ON_BLOCK_H1);
    g_assert_cmpuint(after.block, ==, 0);
    g_assert_cmpuint(after.offset, ==, 3);
    assert_valid(d);
    g_assert_true(on_document_undo(d));   /* one group                     */
    assert_roundtrip(d, blob);

    /* Deleting exactly the image block.                                    */
    g_assert_true(on_document_delete_range(d, at(2, 0), at(2, 1), &after));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 4);
    g_assert_cmpint(on_document_block(d, 2)->kind, ==, ON_BLOCK_TABLE);
    on_document_undo(d);
    assert_roundtrip(d, blob);

    /* The whole document: one empty block remains.                         */
    g_assert_true(on_document_delete_range(d, at(0, 0), at(4, 4), &after));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 1);
    g_assert_cmpstr(block_text(d, 0), ==, "");
    assert_valid(d);
    on_document_undo(d);
    assert_roundtrip(d, blob);

    /* Paste a one-block fragment into text, and a multi-block one.         */
    OnDocument *frag = on_document_from_text("X");
    on_document_set_flags(frag, at(0, 0), 1, ON_FMT_ITALIC, TRUE);
    g_assert_true(on_document_insert_fragment(d, at(1, 4), frag, &after));
    g_assert_cmpstr(block_text(d, 1), ==, "one Xtwo");
    g_assert_cmpuint(on_text_flags_at(on_document_block(d, 1)->text, 4),
                     ==, ON_FMT_ITALIC);
    g_assert_cmpuint(after.offset, ==, 5);
    assert_valid(d);
    on_document_undo(d);
    assert_roundtrip(d, blob);
    on_document_free(frag);

    frag = on_document_copy_range(d, at(1, 0), at(4, 2));   /* 4 blocks     */
    g_assert_true(on_document_insert_fragment(d, at(0, 2), frag, &after));
    g_assert_cmpuint(on_document_n_blocks(d), ==, 8);
    g_assert_cmpstr(block_text(d, 0), ==, "Tione two");
    g_assert_cmpint(on_document_block(d, 1)->kind, ==, ON_BLOCK_IMAGE);
    g_assert_cmpint(on_document_block(d, 2)->kind, ==, ON_BLOCK_TABLE);
    g_assert_cmpstr(block_text(d, 3), ==, "latle");
    g_assert_cmpint(on_document_block(d, 3)->kind, ==, ON_BLOCK_H1);
    g_assert_cmpuint(after.block, ==, 3);
    g_assert_cmpuint(after.offset, ==, 2);
    assert_valid(d);
    g_assert_true(on_document_undo(d));
    assert_roundtrip(d, blob);
    on_document_free(frag);

    /* Into a cell: plain text only.                                        */
    frag = on_document_from_text("a\nb");
    g_assert_true(on_document_insert_fragment(d, cell_at(3, 0, 1), frag,
                                              &after));
    g_assert_cmpstr(on_block_cell(on_document_block(d, 3), 0, 0)->text->str,
                    ==, "ca b1");
    on_document_free(frag);
    on_document_undo(d);
    assert_roundtrip(d, blob);

    on_document_free(d);
    g_bytes_unref(blob);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/document/empty", test_empty);
    g_test_add_func("/document/paragraphs", test_paragraphs);
    g_test_add_func("/document/trailing-newline", test_trailing_newline);
    g_test_add_func("/document/inline-styles", test_inline_styles);
    g_test_add_func("/document/eol-flags", test_eol_flags);
    g_test_add_func("/document/headings-and-code", test_headings_and_code);
    g_test_add_func("/document/empty-styled-line", test_empty_styled_line);
    g_test_add_func("/document/bullets", test_bullets);
    g_test_add_func("/document/numbers", test_numbers);
    g_test_add_func("/document/checks", test_checks);
    g_test_add_func("/document/image-block", test_image_block);
    g_test_add_func("/document/image-inline", test_image_inline);
    g_test_add_func("/document/table", test_table);
    g_test_add_func("/document/run-merge", test_run_merge_across_blocks);
    g_test_add_func("/document/mixed-para", test_mixed_para);
    g_test_add_func("/document/cr-newlines", test_cr_newlines);
    g_test_add_func("/document/unknown-flags", test_unknown_flags);
    g_test_add_func("/document/malformed", test_malformed);
    g_test_add_func("/text/ops", test_text_ops);
    g_test_add_func("/document/check-catches", test_check_catches);
    g_test_add_func("/document/block-copy", test_block_copy);
    g_test_add_func("/ops/text", test_ops_text);
    g_test_add_func("/ops/blocks", test_ops_blocks);
    g_test_add_func("/ops/table", test_ops_table);
    g_test_add_func("/ops/groups", test_ops_groups);
    g_test_add_func("/ops/fuzz", test_ops_fuzz);
    g_test_add_func("/derived/title", test_title);
    g_test_add_func("/derived/from-text", test_from_text);
    g_test_add_func("/derived/collect-tags", test_collect_tags);
    g_test_add_func("/derived/action-rewrites", test_action_rewrites);
    g_test_add_func("/ops/ranges", test_ranges);
    return g_test_run();
}
