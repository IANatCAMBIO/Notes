/* ===========================================================================
 * export.c — export all notes to HTML or Markdown (implementation)
 *
 * Strategy: every note's BNBF blob is loaded into an OnDocument (no GTK
 * involved, no image decoded) and walked block by block.  Each block's
 * kind (heading / code / list / plain / image / table) picks the element,
 * and the runs inside a text block give the span-level markup.
 * Consecutive list blocks merge into one list, consecutive code blocks
 * into one <pre> / fenced block.
 * =========================================================================== */

#include "export.h"
#include "document.h"
#include "serialize.h"               /* on_note_document_load               */

#include <string.h>

/* ---------------------------------------------------------------------------
 * OnExportCtx — per-note rendering context.
 *
 * Fields:
 *   format     — output format being produced.
 *   out        — the file body being built.
 *   note_dir   — directory the note file will live in (for image files).
 *   base_name  — note filename without extension (image name prefix).
 *   img_count  — how many images this note has emitted so far, used to
 *                number the side-car PNG files in Markdown mode.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnExportFormat format;
    GString       *out;
    const gchar   *note_dir;
    const gchar   *base_name;
    gint           img_count;
} OnExportCtx;

/* Inline-style bits considered when grouping runs inside a line.            */
#define EXPORT_INLINE_MASK (ON_FMT_BOLD | ON_FMT_ITALIC | \
                            ON_FMT_UNDERLINE | ON_FMT_STRIKE | ON_FMT_TAG)

/* ---------------------------------------------------------------------------
 * emit_table() — render a table block.
 * HTML: a plain <table>.  Markdown: a pipe table whose first row is the
 * header (as pipe tables require one).
 * ------------------------------------------------------------------------- */
static void
emit_table(OnExportCtx *ctx, const OnBlock *table)
{
    if (ctx->format == ON_EXPORT_HTML) {
        g_string_append(ctx->out, "<table>\n");
        for (gint r = 0; r < table->rows; r++) {
            /* Header rows use <th>.                                        */
            const gchar *cell_tag =
                (table->header && r == 0) ? "th" : "td";
            g_string_append(ctx->out, "<tr>");
            for (gint c = 0; c < table->cols; c++) {
                gchar *esc = g_markup_escape_text(
                    on_block_cell(table, r, c)->text->str, -1);
                /* Multiline cells: newlines become <br>.                   */
                gchar **lines = g_strsplit(esc, "\n", -1);
                gchar *joined = g_strjoinv("<br>", lines);
                g_strfreev(lines);
                g_string_append_printf(ctx->out, "<%s>%s</%s>",
                                       cell_tag, joined, cell_tag);
                g_free(joined);
                g_free(esc);
            }
            g_string_append(ctx->out, "</tr>\n");
        }
        g_string_append(ctx->out, "</table>\n");
    } else {
        g_string_append_c(ctx->out, '\n');
        for (gint r = 0; r < table->rows; r++) {
            for (gint c = 0; c < table->cols; c++) {
                /* Pipe tables cannot hold raw newlines: use <br>.          */
                gchar **lines = g_strsplit(
                    on_block_cell(table, r, c)->text->str, "\n", -1);
                gchar *joined = g_strjoinv("<br>", lines);
                g_strfreev(lines);
                g_string_append_printf(ctx->out, "| %s ", joined);
                g_free(joined);
            }
            g_string_append(ctx->out, "|\n");
            if (r == 0) {            /* pipe tables require this row        */
                for (gint c = 0; c < table->cols; c++)
                    g_string_append(ctx->out, "| --- ");
                g_string_append(ctx->out, "|\n");
            }
        }
        g_string_append_c(ctx->out, '\n');
    }
}

/* ---------------------------------------------------------------------------
 * emit_image() — render one image.
 * HTML: inline base64 data URI.  Markdown: write "<base>-imgN.png" beside
 * the note file and reference it relatively.
 *
 * Both write the image's STORED bytes: an export copies what the database
 * holds instead of recompressing every screenshot in the library, and what
 * lands on disk is byte-identical to what was stored.
 *   ctx — rendering context.
 *   png — the encoded image, as the block carries it.
 * ------------------------------------------------------------------------- */
static void
emit_image(OnExportCtx *ctx, GBytes *png_bytes)
{
    ctx->img_count++;

    /* The placeholder form needs no bytes at all.                          */
    if (ctx->format == ON_EXPORT_MARKDOWN && ctx->note_dir == NULL) {
        /* String render (on_export_note_markdown): there is no directory
         * for side-car files — leave a numbered placeholder instead.        */
        g_string_append_printf(ctx->out, "![image %d]()", ctx->img_count);
        return;
    }

    gsize n_png = 0;                 /* encoded byte count                  */
    const guint8 *png = g_bytes_get_data(png_bytes, &n_png);

    if (ctx->format == ON_EXPORT_HTML) {
        gchar *b64 = g_base64_encode(png, n_png);
        g_string_append_printf(ctx->out,
            "<img src=\"data:image/png;base64,%s\" alt=\"image\">", b64);
        g_free(b64);
    } else {
        gchar *img_name = g_strdup_printf("%s-img%d.png",
                                          ctx->base_name, ctx->img_count);
        gchar *img_path = g_build_filename(ctx->note_dir, img_name, NULL);
        GError *err = NULL;          /* write failure                       */
        if (g_file_set_contents(img_path, (const gchar *)png, (gssize)n_png,
                                &err))
            g_string_append_printf(ctx->out, "![image](%s)", img_name);
        else {
            g_warning("export: image write failed: %s", err->message);
            g_clear_error(&err);
        }
        g_free(img_path);
        g_free(img_name);
    }
}

/* ---------------------------------------------------------------------------
 * emit_text_run() — render one run of identically-styled text.
 *   ctx   — rendering context.
 *   text  — the run's bytes (no newlines), NOT NUL-terminated.
 *   n     — their count.
 *   flags — its EXPORT_INLINE_MASK bits.
 *   raw   — TRUE inside code blocks: no styling, escape-only (HTML).
 * ------------------------------------------------------------------------- */
static void
emit_text_run(OnExportCtx *ctx, const gchar *text, gsize n, guint32 flags,
              gboolean raw)
{
    if (n == 0)
        return;

    if (ctx->format == ON_EXPORT_HTML) {
        gchar *esc = g_markup_escape_text(text, (gssize)n);
        if (raw) {
            g_string_append(ctx->out, esc);
        } else {
            if (flags & ON_FMT_TAG)
                g_string_append(ctx->out, "<span class=\"tag\">");
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "<strong>");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "<em>");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "<u>");
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "<s>");
            g_string_append(ctx->out, esc);
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "</s>");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "</u>");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "</em>");
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "</strong>");
            if (flags & ON_FMT_TAG)
                g_string_append(ctx->out, "</span>");
        }
        g_free(esc);
    } else {
        if (raw) {
            g_string_append_len(ctx->out, text, (gssize)n);
        } else {
            /* Markdown markers.  Underline has no MD syntax; inline HTML
             * is valid Markdown, so <u> is used.  #tags stay literal.      */
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "**");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "*");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "<u>");
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "~~");
            g_string_append_len(ctx->out, text, (gssize)n);
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "~~");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "</u>");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "*");
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "**");
        }
    }
}

/* ---------------------------------------------------------------------------
 * render_inline() — a text block's runs in order, its inline images where
 * they sit (the U+FFFC placeholders themselves are not emitted).
 *   ctx  — rendering context.
 *   t    — the block's text.
 *   raw  — TRUE inside code blocks (no inline styling).
 * ------------------------------------------------------------------------- */
static void
render_inline(OnExportCtx *ctx, const OnText *t, gboolean raw)
{
    gsize at = 0;                    /* start of the current run            */
    guint k  = 0;                    /* next inline image                   */
    for (guint i = 0; i < t->runs->len; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        guint32 flags = raw ? 0 : (r->flags & EXPORT_INLINE_MASK);
        gsize cur = at, end = at + r->len;
        while (k < t->images->len &&
               g_array_index(t->images, OnInlineImage, k).offset < end) {
            const OnInlineImage *img =
                &g_array_index(t->images, OnInlineImage, k);
            emit_text_run(ctx, t->text->str + cur, img->offset - cur, flags,
                          raw);
            emit_image(ctx, img->png);
            cur = img->offset + ON_OBJ_CHAR_LEN;
            k++;
        }
        emit_text_run(ctx, t->text->str + cur, end - cur, flags, raw);
        at = end;
    }
}

/* ---------------------------------------------------------------------------
 * BLOCK-STATE HELPERS — while rendering, `open_block` tracks which
 * multi-line construct is currently open (a list or a code block) so
 * consecutive blocks of the same kind merge into one.
 * ------------------------------------------------------------------------- */

/* close_block() — emit the closer for whatever block is open.               */
static void
close_block(OnExportCtx *ctx, OnBlockKind open)
{
    if (ctx->format == ON_EXPORT_HTML) {
        if (open == ON_BLOCK_CODE)
            g_string_append(ctx->out, "</code></pre>\n");
        else if (open == ON_BLOCK_BULLET || open == ON_BLOCK_CHECK)
            g_string_append(ctx->out, "</ul>\n");
        else if (open == ON_BLOCK_NUMBER)
            g_string_append(ctx->out, "</ol>\n");
    } else {
        if (open == ON_BLOCK_CODE)
            g_string_append(ctx->out, "```\n");
        /* Markdown lists need no closer.                                   */
    }
}

/* open_block() — emit the opener for a new multi-line block.                */
static void
open_block(OnExportCtx *ctx, OnBlockKind kind)
{
    if (ctx->format == ON_EXPORT_HTML) {
        if (kind == ON_BLOCK_CODE)
            g_string_append(ctx->out, "<pre><code>");
        else if (kind == ON_BLOCK_BULLET)
            g_string_append(ctx->out, "<ul>\n");
        else if (kind == ON_BLOCK_CHECK)
            g_string_append(ctx->out, "<ul class=\"tasks\">\n");
        else if (kind == ON_BLOCK_NUMBER)
            g_string_append(ctx->out, "<ol>\n");
    } else {
        if (kind == ON_BLOCK_CODE)
            g_string_append(ctx->out, "```\n");
    }
}

/* ---------------------------------------------------------------------------
 * render_note_body() — walk every block of the document and build the
 * full body in ctx->out.
 * ------------------------------------------------------------------------- */
static void
render_note_body(OnExportCtx *ctx, const OnDocument *doc)
{
    OnBlockKind open = ON_BLOCK_PARA;    /* open multi-line block, or PARA */
    gint number_run = 0;             /* consecutive NUMBER blocks           */

    for (guint i = 0; i < on_document_n_blocks(doc); i++) {
        const OnBlock *b = on_document_block(doc, i);
        number_run = (b->kind == ON_BLOCK_NUMBER) ? number_run + 1 : 0;

        /* Blocks (lists, code) persist across lines; close the open one
         * when the kind changes.                                           */
        gboolean is_block = (b->kind == ON_BLOCK_CODE ||
                             b->kind == ON_BLOCK_BULLET ||
                             b->kind == ON_BLOCK_NUMBER ||
                             b->kind == ON_BLOCK_CHECK);
        if (open != ON_BLOCK_PARA && (!is_block || b->kind != open)) {
            close_block(ctx, open);
            open = ON_BLOCK_PARA;
        }
        if (is_block && open == ON_BLOCK_PARA) {
            open_block(ctx, b->kind);
            open = b->kind;
        }

        if (ctx->format == ON_EXPORT_HTML) {
            switch (b->kind) {
            case ON_BLOCK_H1:
                g_string_append(ctx->out, "<h1>");
                render_inline(ctx, b->text, FALSE);
                g_string_append(ctx->out, "</h1>\n");
                break;
            case ON_BLOCK_H2:
                g_string_append(ctx->out, "<h2>");
                render_inline(ctx, b->text, FALSE);
                g_string_append(ctx->out, "</h2>\n");
                break;
            case ON_BLOCK_CODE:
                render_inline(ctx, b->text, TRUE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_BULLET:
            case ON_BLOCK_NUMBER:
                g_string_append(ctx->out, "<li>");
                render_inline(ctx, b->text, FALSE);
                g_string_append(ctx->out, "</li>\n");
                break;
            case ON_BLOCK_CHECK:
                g_string_append_printf(ctx->out,
                    "<li><input type=\"checkbox\"%s disabled> ",
                    b->checked ? " checked" : "");
                render_inline(ctx, b->text, FALSE);
                g_string_append(ctx->out, "</li>\n");
                break;
            case ON_BLOCK_IMAGE:
                g_string_append(ctx->out, "<p>");
                emit_image(ctx, b->png);
                g_string_append(ctx->out, "</p>\n");
                break;
            case ON_BLOCK_TABLE:
                g_string_append(ctx->out, "<p>");
                emit_table(ctx, b);
                g_string_append(ctx->out, "</p>\n");
                break;
            default:
                if (b->text->text->len > 0) {
                    g_string_append(ctx->out, "<p>");
                    render_inline(ctx, b->text, FALSE);
                    g_string_append(ctx->out, "</p>\n");
                }
                break;
            }
        } else {
            switch (b->kind) {
            case ON_BLOCK_H1:
                g_string_append(ctx->out, "# ");
                render_inline(ctx, b->text, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_H2:
                g_string_append(ctx->out, "## ");
                render_inline(ctx, b->text, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_CODE:
                render_inline(ctx, b->text, TRUE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_BULLET:
                g_string_append(ctx->out, "- ");
                render_inline(ctx, b->text, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_CHECK:
                g_string_append(ctx->out, b->checked ? "- [x] " : "- [ ] ");
                render_inline(ctx, b->text, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_NUMBER:
                /* "N. " is valid Markdown as it stands.                    */
                g_string_append_printf(ctx->out, "%d. ", number_run);
                render_inline(ctx, b->text, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_BLOCK_IMAGE:
                emit_image(ctx, b->png);
                g_string_append(ctx->out, "\n\n");
                break;
            case ON_BLOCK_TABLE:
                emit_table(ctx, b);
                g_string_append(ctx->out, "\n\n");
                break;
            default:
                render_inline(ctx, b->text, FALSE);
                g_string_append_c(ctx->out, '\n');
                /* Blank separator line keeps paragraphs distinct.          */
                g_string_append_c(ctx->out, '\n');
                break;
            }
        }
    }
    if (open != ON_BLOCK_PARA)
        close_block(ctx, open);
}

/* ---------------------------------------------------------------------------
 * sanitize_filename() — turn a note title into a safe file basename:
 * path separators and control chars become '-', long names truncate.
 * Returns a newly allocated string.
 * ------------------------------------------------------------------------- */
static gchar *
sanitize_filename(const gchar *title)
{
    gchar *name = g_strdup(*title != '\0' ? title : "Untitled");
    for (gchar *p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || (guchar)*p < 0x20)
            *p = '-';
    }
    g_strstrip(name);
    if (*name == '\0') {
        g_free(name);
        return g_strdup("Untitled");
    }
    if (strlen(name) > 100)
        name[100] = '\0';
    return name;
}

/* ---------------------------------------------------------------------------
 * unique_path() — build "<dir>/<base><ext>", appending " (2)", " (3)"…
 * to the base if the path was already emitted THIS RUN (`used`, may be
 * NULL for single-note exports).  Uniquifying against the set rather
 * than the disk makes exports re-runnable mirrors: a second export into
 * the same directory overwrites its previous output instead of growing
 * an endless "Note (2)", "Note (3)"… series.  Returns the full path
 * and, via out_base, the final base name (both newly allocated).
 * ------------------------------------------------------------------------- */
static gchar *
unique_path(const gchar *dir, const gchar *base, const gchar *ext,
            GHashTable *used, gchar **out_base)
{
    gchar *final_base = g_strdup(base);  /* base name actually used         */
    gchar *path = g_strdup_printf("%s/%s%s", dir, final_base, ext);
    for (gint n = 2;
         used != NULL && g_hash_table_contains(used, path); n++) {
        g_free(final_base);
        g_free(path);
        final_base = g_strdup_printf("%s (%d)", base, n);
        path = g_strdup_printf("%s/%s%s", dir, final_base, ext);
    }
    if (used != NULL)
        g_hash_table_add(used, g_strdup(path));
    *out_base = final_base;
    return path;
}

/* ---------------------------------------------------------------------------
 * export_one() — render one note into `note_dir` in the given format.
 *   app      — application context (provides the database).
 *   m        — metadata of the note to export.
 *   note_dir — directory the file is written into (must exist).
 *   format   — output format.
 *   used     — full paths already emitted this run (see unique_path);
 *              NULL for single-note exports.
 * Returns TRUE if the file was written.
 * ------------------------------------------------------------------------- */
static gboolean
export_one(OnApp *app, OnNoteMeta *m, const gchar *note_dir,
           OnExportFormat format, GHashTable *used)
{
    OnDocument *doc = on_note_document_load(app->db, m->id);

    /* Compute the output path.                                             */
    const gchar *ext = (format == ON_EXPORT_HTML) ? ".html" : ".md";
    gchar *base = sanitize_filename(m->title);
    gchar *final_base = NULL;        /* base after uniquification           */
    gchar *path = unique_path(note_dir, base, ext, used, &final_base);

    /* Render.                                                              */
    OnExportCtx ctx = {
        .format    = format,
        .out       = g_string_new(NULL),
        .note_dir  = note_dir,
        .base_name = final_base,
        .img_count = 0,
    };

    if (format == ON_EXPORT_HTML) {
        gchar *esc_title = g_markup_escape_text(m->title, -1);
        g_string_append_printf(ctx.out,
            "<!DOCTYPE html>\n<html>\n<head>\n"
            "<meta charset=\"utf-8\">\n<title>%s</title>\n"
            "<style>\n"
            "body { font-family: sans-serif; max-width: 46em;"
            " margin: 2em auto; padding: 0 1em; line-height: 1.5; }\n"
            "pre { background: #f0f0f0; padding: 1em;"
            " border-radius: 6px; overflow-x: auto; }\n"
            "img { max-width: 100%%; }\n"
            ".tag { color: #c35a00; font-weight: 600; }\n"
            "ul.tasks { list-style: none; padding-left: 1.2em; }\n"
            "table { border-collapse: collapse; }\n"
            "td { border: 1px solid #bbb; padding: 4px 8px; }\n"
            "</style>\n</head>\n<body>\n", esc_title);
        g_free(esc_title);
    }

    render_note_body(&ctx, doc);

    if (format == ON_EXPORT_HTML)
        g_string_append(ctx.out, "</body>\n</html>\n");

    /* Write the file.                                                      */
    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, ctx.out->str,
                                      (gssize)ctx.out->len, &err);
    if (!ok) {
        g_warning("export: cannot write %s: %s", path, err->message);
        g_clear_error(&err);
    }

    g_string_free(ctx.out, TRUE);
    on_document_free(doc);
    g_free(path);
    g_free(final_base);
    g_free(base);
    return ok;
}

gint
on_export_all(OnApp *app, const gchar *dest_dir, OnExportFormat format,
              gchar **out_err)
{
    if (out_err != NULL)
        *out_err = NULL;

    if (g_mkdir_with_parents(dest_dir, 0755) != 0) {
        if (out_err != NULL)
            *out_err = g_strdup_printf("cannot create directory %s",
                                       dest_dir);
        return -1;
    }

    GList *notes = on_db_note_list_all(app->db, FALSE);
    gint exported = 0;               /* notes successfully written          */

    /* Paths emitted this run (for within-run uniquification) and a
     * folder_id → ready-made note_dir cache: notes cluster into few
     * folders, so the path query + mkdir run once per folder, not per
     * note.                                                                */
    GHashTable *used = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GHashTable *dirs = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, g_free);
    /* Every folder's path in ONE query, rather than walking the parent
     * chain per folder with a statement per level.                         */
    GHashTable *paths = on_db_folder_path_map(app->db);

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* the note being exported             */

        /* Mirror the folder hierarchy under the destination.               */
        gchar *note_dir = g_hash_table_lookup(dirs, &m->folder_id);
        if (note_dir == NULL) {
            const gchar *rel_dir = (m->folder_id != 0)
                ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
            note_dir = (rel_dir != NULL && *rel_dir != '\0')
                       ? g_build_filename(dest_dir, rel_dir, NULL)
                       : g_strdup(dest_dir);
            g_mkdir_with_parents(note_dir, 0755);
            gint64 *key = g_new(gint64, 1);
            *key = m->folder_id;
            g_hash_table_insert(dirs, key, note_dir);
        }

        if (export_one(app, m, note_dir, format, used))
            exported++;
    }

    g_hash_table_destroy(paths);
    g_hash_table_destroy(dirs);
    g_hash_table_destroy(used);
    on_db_note_list_free(notes);
    return exported;
}

gboolean
on_export_note(OnApp *app, gint64 note_id, const gchar *dest_dir,
               OnExportFormat format)
{
    if (g_mkdir_with_parents(dest_dir, 0755) != 0)
        return FALSE;

    OnNoteMeta *m = on_db_note_get(app->db, note_id);
    if (m == NULL)
        return FALSE;

    gboolean ok = export_one(app, m, dest_dir, format, NULL);
    on_db_note_meta_free(m);
    return ok;
}

gchar *
on_export_note_markdown(OnApp *app, gint64 note_id)
{
    /* A missing/empty note renders as an empty string.                     */
    OnDocument *doc = on_note_document_load(app->db, note_id);

    OnExportCtx ctx = {
        .format    = ON_EXPORT_MARKDOWN,
        .out       = g_string_new(NULL),
        .note_dir  = NULL,           /* string render: no image files       */
        .base_name = NULL,
        .img_count = 0,
    };
    render_note_body(&ctx, doc);
    on_document_free(doc);
    return g_string_free(ctx.out, FALSE);
}
