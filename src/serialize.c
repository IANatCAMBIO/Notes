/* ===========================================================================
 * serialize.c — the blob side of a note (implementation)
 *
 * What reads or writes a note's BNBF blob WITHOUT building a document:
 * the record walks behind the body_text cache and the image ordinals the
 * media browser and the CLI address, and on_note_document_load /
 * on_note_document_save — the document's two ends.
 * =========================================================================== */

#include "serialize.h"
#include "db.h"                      /* OnActionItem (extraction result)    */

#include <string.h>

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

CGImageRef
on_note_image_nth(const guint8 *data, gsize len, gint ord, gint max_px)
{
    /* The copy the GBytes makes is one image's payload, alive only until
     * the decode finishes — the media browser holds one at a time.        */
    GBytes *png = on_note_image_nth_png(data, len, ord);
    if (png == NULL)
        return NULL;

    gsize n_png;                     /* payload size                        */
    const guint8 *bytes = g_bytes_get_data(png, &n_png);
    CGImageRef out = on_image_decode_capped(bytes, n_png, max_px);
    g_bytes_unref(png);
    return out;
}

gchar *
on_note_extract_text(const guint8 *data, gsize len)
{
    /* Through the model, so there is ONE definition of a line's text.
     * Parsing decodes no image — the PNG bytes are carried verbatim.     */
    OnDocLoadReport rep;             /* what the reader met                 */
    OnDocument *doc = on_document_from_bnbf(data, len, &rep);
    gchar *text = rep.error == NULL ? on_document_plain_text(doc) : NULL;
    on_document_free(doc);
    return text;
}

gchar *
on_note_text_cached(OnDatabase *db, gint64 id)
{
    gchar *cached = on_db_note_body_text(db, id);
    if (cached != NULL)
        return cached;

    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = NULL;             /* the stored blob                     */
    if (!on_db_note_load(db, id, &blob, &blob_len))
        return NULL;
    if (blob == NULL)
        return g_strdup("");

    gchar *text = on_note_extract_text(blob, blob_len);
    g_free(blob);
    if (text != NULL)
        on_db_note_set_body_text(db, id, text);
    return text;
}

OnDocument *
on_note_document_load(OnDatabase *db, gint64 id)
{
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = NULL;             /* the stored blob                     */
    if (!on_db_note_load(db, id, &blob, &blob_len))
        return NULL;

    OnDocLoadReport rep;             /* what the reader met                 */
    OnDocument *doc = on_document_from_bnbf(blob, blob_len, &rep);
    g_free(blob);
    if (rep.error != NULL) {
        /* A partial document saved back would truncate the note.          */
        g_warning("note %" G_GINT64_FORMAT " cannot be read: %s", id, rep.error);
        on_document_free(doc);
        return NULL;
    }
    return doc;
}

gboolean
on_note_document_save(OnDatabase *db, gint64 id, OnDocument *doc,
                      gboolean sync_actions, gboolean *changed)
{
    if (changed != NULL)
        *changed = FALSE;

    /* 1. Identities, before the bytes: dedupe, then allocate.             */
    GArray *blocks = on_document_action_blocks(doc);
    GHashTable *seen = g_hash_table_new(g_int64_hash, g_int64_equal);
    GList *items = NULL;             /* the lines as OnActionItem, reversed */
    gboolean ok = TRUE;              /* the outcome                         */
    for (guint i = 0; i < blocks->len && ok; i++) {
        OnBlock *b = on_document_block(doc, g_array_index(blocks, guint, i));
        if (b->action_uid != 0 && g_hash_table_contains(seen, &b->action_uid))
            b->action_uid = 0;       /* a pasted copy: a new item           */
        if (b->action_uid == 0) {
            b->action_uid = on_db_action_uid_alloc(db);
            ok = b->action_uid != 0;
        }
        g_hash_table_add(seen, &b->action_uid);
        OnActionItem *it = g_new0(OnActionItem, 1);
        on_document_block_action(doc, g_array_index(blocks, guint, i),
                                 &it->text, &it->due, &it->done);
        it->uid     = b->action_uid;
        it->ord     = (gint)i;
        it->note_id = id;
        items = g_list_prepend(items, it);
    }
    items = g_list_reverse(items);
    g_hash_table_destroy(seen);
    g_array_unref(blocks);

    /* 2. The note itself.                                                 */
    if (ok) {
        gsize blob_len;              /* serialized content size             */
        guint8 *blob  = on_document_to_bnbf(doc, &blob_len);
        gchar  *title = on_document_title(doc, ON_DEFAULT_NOTE_TITLE,
                                          ON_TITLE_MAX_CHARS);
        gchar  *body  = on_document_plain_text(doc);
        ok = on_db_note_save(db, id, title, blob, blob_len, body);
        g_free(body);
        g_free(title);
        g_free(blob);
    }

    /* 3. The tasks the lines are.                                         */
    if (ok && sync_actions)
        ok = on_db_note_save_actions(db, id, items, changed);
    on_db_action_list_free(items);
    return ok;
}

gboolean
on_note_document_reconcile(OnDatabase *db, gint64 id, OnDocument *doc)
{
    GList *rows = on_db_action_list_for_note(db, id);
    if (rows == NULL)
        return FALSE;
    GHashTable *by_uid = g_hash_table_new(g_int64_hash, g_int64_equal);
    for (GList *l = rows; l != NULL; l = l->next) {
        OnActionItem *it = l->data;  /* one task row                        */
        g_hash_table_insert(by_uid, &it->uid, it);
    }

    gboolean healed = FALSE;         /* any line rewritten?                 */
    GArray *blocks = on_document_action_blocks(doc);
    for (guint i = 0; i < blocks->len; i++) {
        const OnBlock *b =
            on_document_block(doc, g_array_index(blocks, guint, i));
        OnActionItem *row = b->action_uid != 0
                            ? g_hash_table_lookup(by_uid, &b->action_uid)
                            : NULL;
        if (row == NULL)
            continue;
        gchar   *text;               /* what the line says now              */
        gint64   due;
        gboolean done;
        on_document_block_action(doc, g_array_index(blocks, guint, i),
                                 &text, &due, &done);
        /* Each rewrite re-resolves the line by ord: a text rewrite does
         * not move it, so ord i still names it.                          */
        if (g_strcmp0(text, row->text) != 0 && row->text != NULL &&
            *row->text != '\0')
            healed |= on_document_action_text(doc, (gint)i, row->text);
        if (done != row->done)
            healed |= on_document_action_strike(doc, (gint)i, row->done);
        if (due != row->due)
            healed |= on_document_action_due(doc, (gint)i, row->due);
        g_free(text);
    }
    g_array_unref(blocks);
    g_hash_table_destroy(by_uid);
    on_db_action_list_free(rows);
    return healed;
}
