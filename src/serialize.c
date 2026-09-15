/* ===========================================================================
 * serialize.c — BNBF ⇄ GtkTextBuffer (implementation)
 *
 * The record framing lives in bnbf.c; this file walks buffers and drives
 * that reader and writer:
 *
 *   serialize:   walk the buffer character by character, grouping runs of
 *                identical formatting into TEXT records and emitting an
 *                IMAGE record (PNG bytes) wherever a GdkPixbuf is embedded.
 *
 *   deserialize: read records back, inserting text with the matching
 *                GtkTextTags applied, and decoding PNG bytes back into
 *                embedded pixbufs.
 * =========================================================================== */

#include "serialize.h"
#include "db.h"                      /* OnActionItem (extraction result)    */

#include <string.h>

/* ---------------------------------------------------------------------------
 * on_flag_tags — THE flag ⇄ tag-name table (declared in serialize.h).
 * Serializer, editor, undo and export all iterate this single copy so the
 * mapping can never fall out of sync.
 * ------------------------------------------------------------------------- */
const OnFlagTag on_flag_tags[] = {
    { ON_FMT_BOLD,        ON_TAGNAME_BOLD        },
    { ON_FMT_ITALIC,      ON_TAGNAME_ITALIC      },
    { ON_FMT_UNDERLINE,   ON_TAGNAME_UNDERLINE   },
    { ON_FMT_STRIKE,      ON_TAGNAME_STRIKE      },
    { ON_FMT_H1,          ON_TAGNAME_H1          },
    { ON_FMT_H2,          ON_TAGNAME_H2          },
    { ON_FMT_CODEBLOCK,   ON_TAGNAME_CODEBLOCK   },
    { ON_FMT_LIST_BULLET, ON_TAGNAME_LIST_BULLET },
    { ON_FMT_LIST_NUMBER, ON_TAGNAME_LIST_NUMBER },
    { ON_FMT_LIST_CHECK,  ON_TAGNAME_LIST_CHECK  },
    { ON_FMT_TAG,         ON_TAGNAME_TAG         },
};

void
on_buffer_ensure_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);

    /* If one of our tags exists they all do — creation is atomic below.    */
    if (gtk_text_tag_table_lookup(table, ON_TAGNAME_BOLD) != NULL)
        return;

    /* Inline character styles.                                             */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_BOLD,
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_ITALIC,
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_UNDERLINE,
                               "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_STRIKE,
                               "strikethrough", TRUE, NULL);

    /* Headings: larger, bold text applied to whole lines.                  */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_H1,
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.6,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_H2,
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.3,
                               NULL);

    /* Code block: monospace on a subtle grey background, slightly inset.   */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_CODEBLOCK,
                               "family",             "monospace",
                               "paragraph-background", "#f0f0f0",
                               "left-margin",        24,
                               "right-margin",       24,
                               NULL);

    /* List items: indented paragraphs.  The visible "• " / "1. " prefix is
     * inserted as literal text by the editor; the tag provides indent so
     * wrapped lines align under the text, Apple Notes style.               */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_BULLET,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_NUMBER,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_CHECK,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);

    /* Inline #tag token: tinted so tags stand out from prose.              */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_TAG,
                               "foreground", "#c35a00",
                               "weight",     PANGO_WEIGHT_SEMIBOLD,
                               NULL);
}

const gsize on_n_flag_tags = G_N_ELEMENTS(on_flag_tags);

guint32
on_flags_at_iter(GtkTextBuffer *buffer, const GtkTextIter *iter,
                 guint32 mask)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    guint32 flags = 0;               /* accumulated format bits             */

    for (gsize i = 0; i < on_n_flag_tags; i++) {
        if ((on_flag_tags[i].flag & mask) == 0)
            continue;
        GtkTextTag *tag =
            gtk_text_tag_table_lookup(table, on_flag_tags[i].tag_name);
        if (tag != NULL && gtk_text_iter_has_tag(iter, tag))
            flags |= on_flag_tags[i].flag;
    }
    return flags;
}

void
on_flag_run_init(OnFlagRun *run, GtkTextBuffer *buffer, guint32 mask)
{
    run->buffer      = buffer;
    run->mask        = mask;
    run->flags       = 0;
    run->next_toggle = -1;           /* nothing probed yet                  */
}

guint32
on_flag_run_at(OnFlagRun *run, const GtkTextIter *iter)
{
    if (gtk_text_iter_get_offset(iter) < run->next_toggle)
        return run->flags;           /* still inside the probed run         */

    run->flags = on_flags_at_iter(run->buffer, iter, run->mask);

    /* Where can the flag set change next?  A NULL tag means "any tag", so
     * this also stops at editor-only tags (emoji padding, search hits, the
     * action tint).  Those toggle more often than the serialized ones, which
     * only costs a few extra probes — never a missed one.                   */
    GtkTextIter next = *iter;        /* scan cursor for the next toggle     */
    run->next_toggle = gtk_text_iter_forward_to_tag_toggle(&next, NULL)
                       ? gtk_text_iter_get_offset(&next) : G_MAXINT;
    return run->flags;
}

const gchar *
on_tag_name_for_flag(guint32 flag)
{
    for (gsize i = 0; i < on_n_flag_tags; i++)
        if (on_flag_tags[i].flag == (OnFormatFlags)flag)
            return on_flag_tags[i].tag_name;
    return NULL;
}

/* ===========================================================================
 * BUFFER WALK
 *
 * ONE traversal of a GtkTextBuffer, shared by the serializer and the
 * editor's undo snapshot.  Both need exactly the same decomposition —
 * anchors interrupt text runs, runs split where the flag set changes,
 * payloadless anchors and stray U+FFFC characters are dropped — and each
 * used to carry its own copy, with comments warning they must stay in step.
 * Now they differ only in what they do with a segment.
 * ------------------------------------------------------------------------- */

/* UTF-8 encoding of U+FFFC, the object-replacement character a child anchor
 * or embedded paintable occupies in a text slice.                           */
#define OBJ_REPLACEMENT "\xef\xbf\xbc"

/* seg_flush() — hand the pending text run to the callback and reset it.     */
static void
seg_flush(OnBufferSeg *seg, GString *run, guint32 flags,
          OnBufferSegFn cb, gpointer data)
{
    if (run->len == 0)
        return;
    memset(seg, 0, sizeof *seg);
    seg->kind   = ON_SEG_TEXT;
    seg->flags  = flags;
    seg->text   = run->str;
    seg->n_text = run->len;
    cb(seg, data);
    g_string_truncate(run, 0);
}

void
on_buffer_walk(GtkTextBuffer *buffer, OnBufferSegFn cb, gpointer data)
{
    GtkTextIter iter;                /* walk position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);

    GString  *run       = g_string_new(NULL);  /* text of the pending run   */
    guint32   run_flags = 0;                   /* formatting of the run     */
    OnFlagRun frun;                            /* per-run flag probing      */
    OnBufferSeg seg;                           /* reused, copied by callers */
    on_flag_run_init(&frun, buffer, ~0u);

    while (!gtk_text_iter_is_end(&iter)) {
        /* Images and tables live on child anchors.  A paintable inserted
         * straight into the buffer (nothing in the app does that) has no
         * anchor and no pixbuf, so it is dropped like any stray U+FFFC
         * below.                                                           */
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);

        gboolean checked;            /* the checkbox's state                */
        if (anchor != NULL && on_anchor_is_checkbox(anchor, &checked)) {
            seg_flush(&seg, run, run_flags, cb, data);
            memset(&seg, 0, sizeof seg);
            seg.kind    = ON_SEG_CHECK;
            seg.checked = checked;
            seg.flags   = on_flag_run_at(&frun, &iter);
            cb(&seg, data);
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        OnTable *table = (anchor != NULL)
                         ? on_anchor_get_table(anchor) : NULL;
        if (table != NULL) {
            seg_flush(&seg, run, run_flags, cb, data);
            memset(&seg, 0, sizeof seg);
            seg.kind  = ON_SEG_TABLE;
            seg.table = table;       /* borrowed: the anchor owns it        */
            seg.flags = on_flag_run_at(&frun, &iter);
            cb(&seg, data);
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        gint display_width = 0;      /* the user's chosen display width     */
        GdkPixbuf *original = (anchor != NULL)   /* full-resolution image   */
                              ? on_anchor_get_image(anchor, &display_width)
                              : NULL;
        if (original != NULL) {
            seg_flush(&seg, run, run_flags, cb, data);
            memset(&seg, 0, sizeof seg);
            seg.kind          = ON_SEG_IMAGE;
            seg.pixbuf        = original;   /* borrowed                     */
            seg.display_width = display_width;
            seg.flags         = on_flag_run_at(&frun, &iter);
            cb(&seg, data);
            gtk_text_iter_forward_char(&iter);
            continue;
        }
        if (anchor != NULL) {        /* payloadless anchor: skip its 0xFFFC
                                        WITHOUT breaking the run           */
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        /* A plain text position.  The formatting holds until the next tag
         * toggle, so take the whole stretch at once instead of one
         * character at a time — measured 6.9 ms -> 0.1 ms across a 20 000
         * character note.  A stretch containing an anchor (U+FFFC in the
         * slice) falls back to the careful path so the anchor branches
         * above still see it.                                              */
        guint32 flags = on_flag_run_at(&frun, &iter);
        if (flags != run_flags) {
            seg_flush(&seg, run, run_flags, cb, data);
            run_flags = flags;
        }

        GtkTextIter stop = iter;     /* end of this same-formatting stretch */
        if (!gtk_text_iter_forward_to_tag_toggle(&stop, NULL))
            gtk_text_buffer_get_end_iter(buffer, &stop);
        gchar *slice = gtk_text_buffer_get_slice(buffer, &iter, &stop, TRUE);
        const gchar *obj = strstr(slice, OBJ_REPLACEMENT);
        if (obj == NULL) {
            g_string_append(run, slice);
            iter = stop;
        } else if (obj == slice) {
            /* An object sits right here.  Anchors were handled above, so
             * this is a stray replacement character from a paste: drop it. */
            gtk_text_iter_forward_char(&iter);
        } else {
            /* Take the text up to the object and STOP THERE, so the next
             * turn of the loop meets the object in the branches above.
             * Advancing a single character here instead would re-slice the
             * rest of the stretch per character — quadratic on a long run
             * that happens to contain an image.                            */
            g_string_append_len(run, slice, (gssize)(obj - slice));
            gtk_text_iter_forward_chars(
                &iter, (gint)g_utf8_strlen(slice, obj - slice));
        }
        g_free(slice);
    }
    seg_flush(&seg, run, run_flags, cb, data);
    g_string_free(run, TRUE);
}

GBytes *
on_image_png_bytes(GdkPixbuf *pixbuf)
{
    /* The pixbuf never changes once attached, so one encoding serves every
     * writer: the cache is attached by the full-resolution load (the note's
     * ORIGINAL bytes, so nothing is ever recompressed) or filled here on the
     * first write of a freshly pasted image.                               */
    GBytes *cached = g_object_get_data(G_OBJECT(pixbuf), "on-png");
    if (cached != NULL)
        return cached;

    gchar  *png   = NULL;            /* freshly encoded bytes               */
    gsize   n_png = 0;               /* their length                        */
    GError *err   = NULL;            /* encode failure                      */
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &png, &n_png, "png", &err, NULL)) {
        g_warning("image: PNG encode failed: %s",
                  err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        return NULL;
    }

    GBytes *bytes = g_bytes_new_take(png, n_png);
    g_object_set_data_full(G_OBJECT(pixbuf), "on-png", bytes,
                           (GDestroyNotify)g_bytes_unref);
    return bytes;                    /* owned by the pixbuf                 */
}

/* ---------------------------------------------------------------------------
 * serialize_seg() — OnBufferSegFn writing each segment out as a BNBF record.
 * ------------------------------------------------------------------------- */
static void
serialize_seg(const OnBufferSeg *seg, gpointer data)
{
    OnBnbfWriter *w = data;          /* the growing BNBF blob               */

    switch (seg->kind) {
    case ON_SEG_TEXT:
        on_bnbf_write_text(w, seg->flags, seg->text, seg->n_text);
        break;

    case ON_SEG_CHECK:
        on_bnbf_write_check(w, seg->checked);
        break;

    case ON_SEG_TABLE:
        on_bnbf_write_table_begin(w, seg->table->header, seg->table->rows,
                                  seg->table->cols);
        for (gint i = 0; i < seg->table->rows * seg->table->cols; i++) {
            const gchar *cell = g_ptr_array_index(seg->table->cells, i);
            on_bnbf_write_table_cell(w, cell, strlen(cell));
        }
        break;

    case ON_SEG_IMAGE: {
        GBytes *png_bytes = on_image_png_bytes(seg->pixbuf);
        if (png_bytes != NULL) {
            gsize n_png = 0;         /* PNG byte count                      */
            gconstpointer png = g_bytes_get_data(png_bytes, &n_png);
            on_bnbf_write_image(w, (guint32)seg->display_width, png, n_png);
        }
        break;
    }
    }
}

guint8 *
on_note_serialize(GtkTextBuffer *buffer, gsize *out_len)
{
    OnBnbfWriter w;                  /* the growing BNBF blob               */
    on_bnbf_writer_init(&w);
    on_buffer_walk(buffer, serialize_seg, &w);
    return on_bnbf_writer_finish(&w, out_len);
}

/* ---------------------------------------------------------------------------
 * insert_with_flags() — insert `text` at the buffer end with every tag
 * named by `flags` applied.
 *   buffer — destination buffer.
 *   text   — UTF-8 text to insert.
 *   n      — byte length of `text`.
 *   flags  — ON_FMT_* bits to apply.
 * ------------------------------------------------------------------------- */
static void
insert_with_flags(GtkTextBuffer *buffer, const gchar *text, gssize n,
                  guint32 flags)
{
    GtkTextIter end;                 /* insertion point (buffer end)        */
    gtk_text_buffer_get_end_iter(buffer, &end);

    /* Remember where the inserted span starts so tags can be applied.      */
    gint start_offset = gtk_text_iter_get_offset(&end);
    gtk_text_buffer_insert(buffer, &end, text, n);

    if (flags == 0)
        return;

    GtkTextIter start;               /* start of the span just inserted     */
    gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
    gtk_text_buffer_get_end_iter(buffer, &end);

    for (gsize i = 0; i < on_n_flag_tags; i++) {
        if (flags & on_flag_tags[i].flag)
            gtk_text_buffer_apply_tag_by_name(
                buffer, on_flag_tags[i].tag_name, &start, &end);
    }
}

/* ---------------------------------------------------------------------------
 * on_size_prepared() — GdkPixbufLoader callback capping decode size:
 * shrink to at most `max_px` (passed via user_data) on the longest side,
 * preserving aspect ratio.  Never upscales.
 * ------------------------------------------------------------------------- */
static void
on_size_prepared(GdkPixbufLoader *loader, gint width, gint height,
                 gpointer user_data)
{
    gint max_px = GPOINTER_TO_INT(user_data);
    gint longest = MAX(width, height);
    if (longest > max_px) {
        gdouble scale = (gdouble)max_px / longest;
        gdk_pixbuf_loader_set_size(loader,
                                   MAX(1, (gint)(width * scale)),
                                   MAX(1, (gint)(height * scale)));
    }
}

GdkPixbuf *
on_png_decode_capped(const guint8 *png, gsize n_png, gint max_px)
{
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    if (max_px > 0)
        g_signal_connect(loader, "size-prepared",
                         G_CALLBACK(on_size_prepared),
                         GINT_TO_POINTER(max_px));

    GdkPixbuf *out = NULL;           /* the decoded image, owned            */
    GError    *err = NULL;           /* decode failure, if any              */
    if (gdk_pixbuf_loader_write(loader, png, n_png, &err) &&
        gdk_pixbuf_loader_close(loader, &err)) {
        GdkPixbuf *pixbuf =          /* borrowed from the loader            */
            gdk_pixbuf_loader_get_pixbuf(loader);
        if (pixbuf != NULL)
            out = g_object_ref(pixbuf);
    } else {
        g_warning("image: bad image data: %s",
                  err != NULL ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_object_unref(loader);
    return out;
}

gboolean
on_note_deserialize(GtkTextBuffer *buffer, const guint8 *data, gsize len)
{
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, "", -1);

    OnBnbfReader r;                  /* the one record walker               */
    if (!on_bnbf_open(&r, data, len)) {
        g_warning("deserialize: %s", r.error);
        return FALSE;
    }

    OnBnbfRecord rec;                /* the record being built from         */
    while (on_bnbf_next(&r, &rec)) {
        switch (rec.type) {
        case ON_REC_TEXT:
            insert_with_flags(buffer, rec.text, (gssize)rec.n_text,
                              rec.flags);
            break;

        case ON_REC_IMAGE: {
            /* Decode the PNG bytes and embed an image-carrying anchor.
             * Widgets (for on-screen display) are attached separately by
             * the editor; offscreen consumers just read the anchor data.   */
            GdkPixbuf *pixbuf =      /* owned; the anchor takes its own ref */
                on_png_decode_capped(rec.png, rec.n_png, 0);
            if (pixbuf != NULL) {
                GtkTextIter end;
                gtk_text_buffer_get_end_iter(buffer, &end);
                GtkTextChildAnchor *anchor =
                    gtk_text_buffer_create_child_anchor(buffer, &end);
                on_anchor_set_image(anchor, pixbuf,
                                    (gint)rec.display_width);
                /* Keep the source PNG bytes on the pixbuf so saves emit
                 * them verbatim instead of re-encoding (see the "on-png"
                 * cache in on_note_serialize).                             */
                g_object_set_data_full(G_OBJECT(pixbuf), "on-png",
                    g_bytes_new(rec.png, rec.n_png),
                    (GDestroyNotify)g_bytes_unref);
                g_object_unref(pixbuf);
            }
            break;
        }

        case ON_REC_TABLE: {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextChildAnchor *anchor =
                gtk_text_buffer_create_child_anchor(buffer, &end);
            on_anchor_set_table(anchor, rec.table);   /* takes ownership    */
            break;
        }

        case ON_REC_CHECK: {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextChildAnchor *anchor =
                gtk_text_buffer_create_child_anchor(buffer, &end);
            on_anchor_set_checkbox(anchor, rec.checked);
            break;
        }

        default:
            break;                   /* on_bnbf_next yields only the four   */
        }
    }

    if (r.error != NULL) {
        g_warning("deserialize: %s", r.error);
        return FALSE;
    }
    if (!r.saw_end) {
        /* Ran off the end without seeing ON_REC_END — tolerate but report.    */
        g_warning("deserialize: missing end marker");
        return FALSE;
    }
    return TRUE;
}

void
on_anchor_set_checkbox(GtkTextChildAnchor *anchor, gboolean checked)
{
    /* Encoded as 1 (unchecked) / 2 (checked) so NULL means "no checkbox". */
    g_object_set_data(G_OBJECT(anchor), "on-checkbox",
                      GINT_TO_POINTER(checked ? 2 : 1));
}

gboolean
on_anchor_is_checkbox(GtkTextChildAnchor *anchor, gboolean *out_checked)
{
    gint v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor),
                                               "on-checkbox"));
    if (out_checked != NULL)
        *out_checked = (v == 2);
    return v != 0;
}

void
on_anchor_set_table(GtkTextChildAnchor *anchor, OnTable *table)
{
    g_object_set_data_full(G_OBJECT(anchor), "on-table", table,
                           (GDestroyNotify)on_table_free);
}

OnTable *
on_anchor_get_table(GtkTextChildAnchor *anchor)
{
    return g_object_get_data(G_OBJECT(anchor), "on-table");
}

void
on_anchor_set_image(GtkTextChildAnchor *anchor, GdkPixbuf *original,
                    gint display_width)
{
    g_object_set_data_full(G_OBJECT(anchor), "on-original",
                           g_object_ref(original), g_object_unref);
    g_object_set_data(G_OBJECT(anchor), "on-display-width",
                      GINT_TO_POINTER(display_width));
}

GdkPixbuf *
on_anchor_get_image(GtkTextChildAnchor *anchor, gint *display_width)
{
    if (display_width != NULL)
        *display_width = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(anchor), "on-display-width"));
    return g_object_get_data(G_OBJECT(anchor), "on-original");
}

gint
on_note_count_images(const guint8 *data, gsize len)
{
    OnBnbfReader r;                  /* the shared record walker            */
    if (data == NULL || !on_bnbf_open(&r, data, len))
        return 0;

    gint n = 0;                      /* images seen so far                  */
    OnBnbfRecord rec;                /* the record being walked past        */
    while (on_bnbf_next(&r, &rec)) {
        if (rec.type == ON_REC_IMAGE)
            n++;
        else if (rec.type == ON_REC_TABLE)
            on_table_free(rec.table);    /* the reader hands ownership over */
    }
    return n;
}

GBytes *
on_note_image_nth_png(const guint8 *data, gsize len, gint ord)
{
    OnBnbfReader r;                  /* the shared record walker            */
    if (data == NULL || ord < 0 || !on_bnbf_open(&r, data, len))
        return NULL;

    gint n = 0;                      /* images seen so far                  */
    OnBnbfRecord rec;                /* the record being walked past        */
    GBytes *out = NULL;              /* the one payload we copy out         */
    while (on_bnbf_next(&r, &rec)) {
        if (rec.type == ON_REC_TABLE) {
            on_table_free(rec.table);
        } else if (rec.type == ON_REC_IMAGE && n++ == ord) {
            out = g_bytes_new(rec.png, rec.n_png);
            break;
        }
    }
    return out;
}

GdkPixbuf *
on_note_image_nth(const guint8 *data, gsize len, gint ord, gint max_px)
{
    /* The copy the GBytes makes is one image's payload, alive only until
     * the decode finishes — the media browser holds one at a time.        */
    GBytes *png = on_note_image_nth_png(data, len, ord);
    if (png == NULL)
        return NULL;

    gsize n_png;                     /* payload size                        */
    const guint8 *bytes = g_bytes_get_data(png, &n_png);
    GdkPixbuf *out = on_png_decode_capped(bytes, n_png, max_px);
    g_bytes_unref(png);
    return out;
}

/* ---------------------------------------------------------------------------
 * probe_size_prepared() — "size-prepared" handler for on_png_probe_size():
 * records the image's declared dimensions and asks for nothing else.
 * ------------------------------------------------------------------------- */
static void
probe_size_prepared(GdkPixbufLoader *loader, gint width, gint height,
                    gpointer user_data)
{
    (void)loader;
    gint *wh = user_data;            /* [0]=width, [1]=height               */
    wh[0] = width;
    wh[1] = height;
}

gboolean
on_png_probe_size(const guint8 *png, gsize n_png, gint *w, gint *h)
{
    if (png == NULL || n_png == 0)
        return FALSE;

    gint wh[2] = { 0, 0 };           /* dimensions the header declares      */
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    g_signal_connect(loader, "size-prepared",
                     G_CALLBACK(probe_size_prepared), wh);

    /* A PNG declares its size in the IHDR chunk, within the first few dozen
     * bytes, and the loader emits size-prepared as soon as it has read it —
     * so feeding it the head of the file is enough.  Closing a loader that
     * was given a truncated image fails; that error is expected and
     * discarded, since the dimensions are already in hand.                 */
    GError *err = NULL;              /* the expected truncation failure     */
    gdk_pixbuf_loader_write(loader, png, MIN(n_png, 1024), &err);
    g_clear_error(&err);
    gdk_pixbuf_loader_close(loader, &err);
    g_clear_error(&err);
    g_object_unref(loader);

    if (wh[0] <= 0 || wh[1] <= 0)
        return FALSE;
    if (w != NULL)
        *w = wh[0];
    if (h != NULL)
        *h = wh[1];
    return TRUE;
}

gchar *
on_note_extract_text(const guint8 *data, gsize len)
{
    gchar *text = NULL;              /* the concatenated plain text         */
    on_note_extract(data, len, &text, NULL);
    return text;
}

gchar *
on_note_text_cached(OnDatabase *db, gint64 id)
{
    gchar *cached = on_db_note_body_text(db, id);
    if (cached != NULL)
        return cached;

    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob == NULL)
        return g_strdup("");

    gchar *text = on_note_extract_text(blob, blob_len);
    g_free(blob);
    on_db_note_set_body_text(db, id, text);
    return text;
}

OnDocument *
on_note_document_load(OnDatabase *db, gint64 id)
{
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    OnDocument *doc = on_document_from_bnbf(blob, blob_len, NULL);
    g_free(blob);
    return doc;
}

/* ---------------------------------------------------------------------------
 * action_finish_line() — helper for on_note_extract_actions(): if the
 * line just ended was an action line with real text, append it to *items
 * (text trimmed, any trailing "due <date>" split off into `due`,
 * ord = list position); either way reset the line state.
 * ------------------------------------------------------------------------- */
typedef struct {
    gboolean at_start;               /* cursor sits at a line start         */
    gboolean is_action;              /* current line began with '!'         */
    gboolean struck;                 /* every rest non-space char struck?   */
    gboolean have_rest;              /* any non-space char after the '!'?   */
    GString *text;                   /* rest-of-line accumulator            */
} ActionScan;

static void
action_finish_line(ActionScan *s, GList **items, gint *ord)
{
    if (s->is_action && s->have_rest) {
        gchar *text = g_strdup(s->text->str);
        gsize  due_start;            /* where the text part ends            */
        gint64 due = 0;              /* parsed due date, 0 = none           */
        if (on_action_split_due(text, &due_start, &due))
            text[due_start] = '\0';
        g_strstrip(text);
        if (*text != '\0') {         /* a bare "! due 7/7/26" is no item    */
            OnActionItem *it = g_new0(OnActionItem, 1);
            it->text = text;
            it->done = s->struck;
            it->due  = due;
            it->ord  = (*ord)++;
            *items = g_list_prepend(*items, it);
        } else {
            g_free(text);
        }
    }
    g_string_truncate(s->text, 0);
    s->at_start  = TRUE;
    s->is_action = FALSE;
}

GList *
on_note_extract_actions(const guint8 *data, gsize len)
{
    GList *actions = NULL;           /* collected OnActionItem*             */
    on_note_extract(data, len, NULL, &actions);
    return actions;
}

void
on_note_extract(const guint8 *data, gsize len, gchar **out_text,
                GList **out_actions)
{
    GString *text = (out_text != NULL) ? g_string_new(NULL) : NULL;
    GList   *items = NULL;           /* collected OnActionItem*, reversed   */
    gint     ord   = 0;              /* next item's position index          */
    ActionScan s = { TRUE, FALSE, TRUE, FALSE, g_string_new(NULL) };
    gboolean want_actions = out_actions != NULL;

    OnBnbfReader r;                  /* the same walker the loader uses     */
    OnBnbfRecord rec;
    if (on_bnbf_open(&r, data, len)) {
        while (on_bnbf_next(&r, &rec)) {
            if (rec.type == ON_REC_TEXT) {
                if (text != NULL)
                    g_string_append_len(text, rec.text, rec.n_text);
                for (guint32 i = 0; want_actions && i < rec.n_text; i++) {
                    gchar c = rec.text[i];
                                     /* one BYTE — '\n'/'!' are ASCII, and
                                        UTF-8 tail bytes are all >= 0x80    */
                    if (c == '\n') {
                        action_finish_line(&s, &items, &ord);
                    } else if (s.at_start) {
                        s.at_start  = FALSE;
                        s.is_action = c == '!' &&
                                      (rec.flags & ON_FMT_CODEBLOCK) == 0;
                        if (s.is_action) {  /* the '!' is not item text     */
                            s.struck    = TRUE;
                            s.have_rest = FALSE;
                        }
                    } else if (s.is_action) {
                        g_string_append_c(s.text, c);
                        if (!g_ascii_isspace((guchar)c)) {
                            s.have_rest = TRUE;
                            if ((rec.flags & ON_FMT_STRIKE) == 0)
                                s.struck = FALSE;
                        }
                    }
                }
            } else {
                /* Images, tables and checkboxes all occupy the line's first
                 * slot like any character, so such a line is never an
                 * action line.  Table cells additionally join the text,
                 * space-separated.                                         */
                if (rec.type == ON_REC_TABLE) {
                    if (text != NULL)
                        for (gint cell = 0;
                             cell < rec.table->rows * rec.table->cols;
                             cell++) {
                            g_string_append(text,
                                g_ptr_array_index(rec.table->cells, cell));
                            g_string_append_c(text, ' ');
                        }
                    on_table_free(rec.table);
                }
                s.at_start = FALSE;
            }
        }
    }
    if (want_actions)
        action_finish_line(&s, &items, &ord);   /* line without trailing \n */

    g_string_free(s.text, TRUE);
    if (out_text != NULL)
        *out_text = g_string_free(text, FALSE);
    if (out_actions != NULL)
        *out_actions = g_list_reverse(items);
}

gchar *
on_buffer_first_line(GtkTextBuffer *buffer)
{
    GtkTextIter start, line_end;     /* span of the first line              */
    gtk_text_buffer_get_start_iter(buffer, &start);

    /* Skip leading blank lines so a note starting with newlines still
     * gets a meaningful title.                                             */
    while (gtk_text_iter_ends_line(&start) &&
           !gtk_text_iter_is_end(&start))
        gtk_text_iter_forward_line(&start);

    line_end = start;
    if (!gtk_text_iter_ends_line(&line_end))
        gtk_text_iter_forward_to_line_end(&line_end);

    gchar *text = gtk_text_buffer_get_text(buffer, &start, &line_end, FALSE);
    g_strstrip(text);

    if (*text == '\0') {
        g_free(text);
        return g_strdup(ON_DEFAULT_NOTE_TITLE);
    }
    /* Keep titles a sane length for the list views.                        */
    if (g_utf8_strlen(text, -1) > ON_TITLE_MAX_CHARS) {
        gchar *cut = g_utf8_substring(text, 0, ON_TITLE_MAX_CHARS);
        g_free(text);
        return cut;
    }
    return text;
}

GList *
on_buffer_collect_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, ON_TAGNAME_TAG);
    if (tag == NULL)
        return NULL;

    GList *names = NULL;             /* collected unique tag names          */
    GtkTextIter iter;                /* scan position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);

    /* Jump from tag-span to tag-span using forward_to_tag_toggle.          */
    while (TRUE) {
        if (!gtk_text_iter_starts_tag(&iter, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&iter, tag))
                break;
            if (!gtk_text_iter_starts_tag(&iter, tag))
                continue;
        }
        GtkTextIter span_end = iter; /* end of this tag span                */
        gtk_text_iter_forward_to_tag_toggle(&span_end, tag);

        gchar *text = gtk_text_buffer_get_text(buffer, &iter,
                                               &span_end, FALSE);
        /* Spans include the leading '#'; strip it and surrounding space.   */
        g_strstrip(text);
        const gchar *name = (*text == '#') ? text + 1 : text;
        if (*name != '\0' &&
            g_list_find_custom(names, name,
                               (GCompareFunc)g_strcmp0) == NULL)
            names = g_list_prepend(names, g_strdup(name));
        g_free(text);

        iter = span_end;
    }
    return g_list_reverse(names);
}
