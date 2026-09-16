/* ===========================================================================
 * serialize.c — the blob side of a note (implementation)
 *
 * What reads or writes a note's BNBF blob WITHOUT building a document:
 * the record walks behind the body_text cache and the action_items
 * mirror, the image ordinals the media browser and the CLI address, the
 * one PNG decoder and the one PNG encoder, and on_note_document_load —
 * the preamble of every consumer that does want the document.
 * =========================================================================== */

#include "serialize.h"
#include "db.h"                      /* OnActionItem (extraction result)    */

#include <string.h>

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
    /* Through the model, so there is ONE definition of a line's text and
     * of an action item (on_block_is_action, on_document_block_action):
     * the ord this hands the action_items table is the ord the model's
     * rewrites address.  Parsing decodes no image — the PNG bytes are
     * carried verbatim — so it costs what the old record walk did.     */
    OnDocument *doc = on_document_from_bnbf(data, len, NULL);
    if (out_text != NULL)
        *out_text = on_document_plain_text(doc);
    if (out_actions != NULL) {
        GList  *items  = NULL;       /* collected OnActionItem*, reversed   */
        GArray *blocks = on_document_action_blocks(doc);
        for (guint i = 0; i < blocks->len; i++) {
            OnActionItem *it = g_new0(OnActionItem, 1);
            on_document_block_action(doc, g_array_index(blocks, guint, i),
                                     &it->text, &it->due, &it->done);
            it->ord = (gint)i;
            items = g_list_prepend(items, it);
        }
        g_array_unref(blocks);
        *out_actions = g_list_reverse(items);
    }
    on_document_free(doc);
}
