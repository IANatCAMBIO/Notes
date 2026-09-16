/* ===========================================================================
 * bnbf-scan.c — round-trip every note blob in a database through the
 * block model and report what changed.
 *
 *   build/bnbf-scan DB.db            summary over every note
 *   build/bnbf-scan DB.db --diff ID  the two record streams of one note,
 *                                    the stored blob and its re-save, up
 *                                    to and past their first difference
 *   build/bnbf-scan DB.db --blocks ID  one note's block structure
 *
 * Run it on a COPY of a database (sqlite3 "…" ".backup copy.db"), never
 * the live file.  Opens read-only regardless.  GLib + SQLite only.
 *
 * Verdicts per note:
 *   identical   — the re-saved blob equals the stored one byte for byte
 *   normalized  — it differs, and the loader's report says why (counts
 *                 per category are totalled)
 *   unexplained — it differs and the report is clean: a model or saver
 *                 bug, listed by id with the first differing offset
 *   invalid     — the loaded document fails on_document_check: a bug
 *   error       — the reader rejected the blob
 * =========================================================================== */

#include "document.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* rec_dump() — print a blob's records, one per line, marking the record
 * that holds byte `mark` (or none when mark is (gsize)-1).                 */
static void
rec_dump(const guint8 *data, gsize len, gsize mark)
{
    OnBnbfReader r;
    OnBnbfRecord rec;
    if (!on_bnbf_open(&r, data, len)) {
        printf("  (unreadable: %s)\n", r.error);
        return;
    }
    printf("  version %u\n", r.version);
    while (TRUE) {
        gsize at = r.pos;
        gboolean more = on_bnbf_next(&r, &rec);
        const gchar *flag = (at <= mark && mark < r.pos) ? ">>" : "  ";
        if (!more) {
            if (r.error != NULL)
                printf("%s %6" G_GSIZE_FORMAT "  ERROR %s\n", flag, at,
                       r.error);
            else
                printf("%s %6" G_GSIZE_FORMAT "  END\n", flag, at);
            break;
        }
        switch (rec.type) {
        case ON_REC_TEXT: {
            gchar *esc = g_strescape(g_strndup(rec.text, rec.n_text), NULL);
            printf("%s %6" G_GSIZE_FORMAT "  TEXT  0x%03x %5u \"%.*s%s\"\n",
                   flag, at, rec.flags, rec.n_text, 60, esc,
                   strlen(esc) > 60 ? "…" : "");
            g_free(esc);
            break;
        }
        case ON_REC_IMAGE:
            printf("%s %6" G_GSIZE_FORMAT "  IMAGE w=%u %u bytes\n", flag,
                   at, rec.display_width, rec.n_png);
            break;
        case ON_REC_CHECK:
            printf("%s %6" G_GSIZE_FORMAT "  CHECK %d\n", flag, at,
                   rec.checked);
            break;
        case ON_REC_TABLE:
            printf("%s %6" G_GSIZE_FORMAT "  TABLE %dx%d%s\n", flag, at,
                   rec.table->rows, rec.table->cols,
                   rec.table->header ? " header" : "");
            on_table_free(rec.table);
            break;
        }
    }
}

/* first_diff() — offset of the first differing byte, or (gsize)-1.        */
static gsize
first_diff(const guint8 *a, gsize na, const guint8 *b, gsize nb)
{
    gsize n = MIN(na, nb);
    for (gsize i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return (na == nb) ? (gsize)-1 : n;
}

static const gchar *KIND_NAMES[ON_N_BLOCK_KINDS] = {
    "PARA", "H1", "H2", "BULLET", "NUMBER", "CHECK", "CODE", "IMAGE", "TABLE",
};

/* blocks_dump() — one line per block.                                       */
static void
blocks_dump(const OnDocument *d)
{
    for (guint i = 0; i < on_document_n_blocks(d); i++) {
        const OnBlock *b = on_document_block(d, i);
        printf("  %4u %-6s", i, KIND_NAMES[b->kind]);
        if (b->kind == ON_BLOCK_IMAGE) {
            printf(" w=%u %" G_GSIZE_FORMAT " bytes", b->display_width,
                   g_bytes_get_size(b->png));
        } else if (b->kind == ON_BLOCK_TABLE) {
            printf(" %dx%d%s", b->rows, b->cols, b->header ? " header" : "");
        } else {
            gchar *esc = g_strescape(b->text->text->str, NULL);
            printf(" eol=0x%x runs=%u img=%u \"%.60s%s\"",
                   b->eol_flags, b->text->runs->len, b->text->images->len,
                   esc, strlen(esc) > 60 ? "…" : "");
            g_free(esc);
            if (b->kind == ON_BLOCK_CHECK)
                printf(" [%c]", b->checked ? 'x' : ' ');
        }
        printf("\n");
    }
}

/* report_add() — total one note's normalization counts into `sum`.        */
static void
report_add(OnDocLoadReport *sum, const OnDocLoadReport *r)
{
#define ADD(f) sum->f += r->f
    ADD(mixed_para); ADD(unknown_flags); ADD(bullet_no_prefix);
    ADD(number_renumbered); ADD(styled_prefix); ADD(check_no_space);
    ADD(check_no_tag); ADD(check_no_box); ADD(split_check);
    ADD(split_table); ADD(image_inline); ADD(run_split); ADD(cr_newlines);
    ADD(old_version);
#undef ADD
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bnbf-scan DB.db [--diff ID | --blocks ID]\n");
        return 2;
    }
    gint64 want_id = 0;              /* --diff / --blocks target            */
    gboolean want_diff = FALSE, want_blocks = FALSE;
    if (argc == 4) {
        want_id = g_ascii_strtoll(argv[3], NULL, 10);
        want_diff   = strcmp(argv[2], "--diff") == 0;
        want_blocks = strcmp(argv[2], "--blocks") == 0;
    }

    sqlite3 *db;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL) !=
        SQLITE_OK) {
        fprintf(stderr, "cannot open %s: %s\n", argv[1], sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_stmt *st;
    const gchar *sql = want_id != 0
        ? "SELECT id, content FROM notes WHERE id = ?"
        : "SELECT id, content FROM notes ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "query: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    if (want_id != 0)
        sqlite3_bind_int64(st, 1, want_id);

    guint n_notes = 0, n_identical = 0, n_normalized = 0, n_unexplained = 0,
          n_invalid = 0, n_error = 0, n_empty = 0;
    guint64 bytes_total = 0;
    guint max_blocks = 0, max_text = 0;
    gint64 max_blocks_id = 0, max_text_id = 0;
    guint with_inline = 0, with_tables = 0, with_checks = 0, with_code = 0,
          with_images = 0, with_eol_flags = 0;
    OnDocLoadReport sum;
    memset(&sum, 0, sizeof sum);
    GString *why = g_string_new(NULL);
    gint64 t_load = 0, t_save = 0;   /* microseconds                        */

    while (sqlite3_step(st) == SQLITE_ROW) {
        gint64 id = sqlite3_column_int64(st, 0);
        const guint8 *data = sqlite3_column_blob(st, 1);
        gsize len = (gsize)sqlite3_column_bytes(st, 1);
        n_notes++;
        bytes_total += len;
        if (data == NULL || len == 0) {
            n_empty++;
            continue;
        }

        OnDocLoadReport rep;
        gint64 t0 = g_get_monotonic_time();
        OnDocument *d = on_document_from_bnbf(data, len, &rep);
        gint64 t1 = g_get_monotonic_time();
        gsize n_out;
        guint8 *out = on_document_to_bnbf(d, &n_out);
        gint64 t2 = g_get_monotonic_time();
        t_load += t1 - t0;
        t_save += t2 - t1;
        report_add(&sum, &rep);

        gboolean valid = on_document_check(d, why);
        gsize diff = first_diff(data, len, out, n_out);

        /* Per-note statistics.                                            */
        guint n_blocks = on_document_n_blocks(d);
        if (n_blocks > max_blocks) {
            max_blocks = n_blocks;
            max_blocks_id = id;
        }
        guint text_bytes = 0;
        gboolean has_inline = FALSE, has_table = FALSE, has_check = FALSE,
                 has_code = FALSE, has_eol = FALSE;
        for (guint i = 0; i < n_blocks; i++) {
            const OnBlock *b = on_document_block(d, i);
            if (b->text != NULL) {
                text_bytes += (guint)b->text->text->len;
                if (b->text->images->len > 0)
                    has_inline = TRUE;
            }
            has_table |= b->kind == ON_BLOCK_TABLE;
            has_check |= b->kind == ON_BLOCK_CHECK;
            has_code  |= b->kind == ON_BLOCK_CODE;
            has_eol   |= b->eol_flags != 0;
        }
        if (text_bytes > max_text) {
            max_text = text_bytes;
            max_text_id = id;
        }
        with_inline += has_inline;
        with_tables += has_table;
        with_checks += has_check;
        with_code   += has_code;
        with_eol_flags += has_eol;
        with_images += on_document_image_count(d) > 0;

        if (want_blocks) {
            printf("note %" G_GINT64_FORMAT ": %u blocks\n", id, n_blocks);
            blocks_dump(d);
        }
        if (want_diff) {
            printf("note %" G_GINT64_FORMAT ": stored %" G_GSIZE_FORMAT
                   " bytes, re-saved %" G_GSIZE_FORMAT ", first difference "
                   "at %" G_GSSIZE_FORMAT "\n", id, len, n_out, (gssize)diff);
            printf("stored:\n");
            rec_dump(data, len, diff);
            printf("re-saved:\n");
            rec_dump(out, n_out, diff);
        }

        if (!valid) {
            n_invalid++;
            printf("INVALID  note %" G_GINT64_FORMAT ": %s\n", id, why->str);
        } else if (rep.error != NULL) {
            n_error++;
            printf("ERROR    note %" G_GINT64_FORMAT ": %s\n", id, rep.error);
        } else if (diff == (gsize)-1) {
            n_identical++;
        } else if (!on_document_load_report_is_clean(&rep)) {
            n_normalized++;
            printf("normalized note %" G_GINT64_FORMAT ":", id);
#define SAY(f) if (rep.f) printf(" " #f "=%u", rep.f)
            SAY(mixed_para); SAY(unknown_flags); SAY(bullet_no_prefix);
            SAY(number_renumbered); SAY(styled_prefix); SAY(check_no_space);
            SAY(check_no_tag); SAY(check_no_box); SAY(split_check);
            SAY(split_table); SAY(run_split); SAY(cr_newlines);
            SAY(old_version);
#undef SAY
            printf("\n");
        } else {
            n_unexplained++;
            printf("UNEXPLAINED note %" G_GINT64_FORMAT ": first difference "
                   "at byte %" G_GSIZE_FORMAT " (stored %" G_GSIZE_FORMAT
                   ", re-saved %" G_GSIZE_FORMAT ")\n", id, diff, len,
                   n_out);
        }
        g_free(out);
        on_document_free(d);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    printf("\n%u notes, %.1f MB of blobs, %u empty\n", n_notes,
           bytes_total / 1e6, n_empty);
    printf("  identical    %u\n  normalized   %u\n  unexplained  %u\n"
           "  invalid      %u\n  error        %u\n",
           n_identical, n_normalized, n_unexplained, n_invalid, n_error);
    printf("normalizations (lines, over all notes):\n"
           "  mixed_para %u  unknown_flags %u  bullet_no_prefix %u\n"
           "  number_renumbered %u  styled_prefix %u  check_no_space %u\n"
           "  check_no_tag %u  check_no_box %u  split_check %u\n"
           "  split_table %u  run_split %u  cr_newlines %u  old_version %u"
           "  (image_inline %u, exact)\n",
           sum.mixed_para, sum.unknown_flags, sum.bullet_no_prefix,
           sum.number_renumbered, sum.styled_prefix, sum.check_no_space,
           sum.check_no_tag, sum.check_no_box, sum.split_check,
           sum.split_table, sum.run_split, sum.cr_newlines,
           sum.old_version, sum.image_inline);
    printf("shape:\n"
           "  notes with images %u, inline images %u, tables %u, checks %u,"
           " code %u, styled newlines %u\n"
           "  most blocks: %u (note %" G_GINT64_FORMAT ");"
           " most text: %u bytes (note %" G_GINT64_FORMAT ")\n",
           with_images, with_inline, with_tables, with_checks, with_code,
           with_eol_flags, max_blocks, max_blocks_id, max_text, max_text_id);
    printf("time: load %.1f ms, save %.1f ms\n", t_load / 1e3, t_save / 1e3);
    g_string_free(why, TRUE);
    return (n_unexplained + n_invalid) > 0 ? 1 : 0;
}
