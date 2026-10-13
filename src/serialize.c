/* ===========================================================================
 * serialize.c — the blob side of a note (implementation)
 *
 * What reads or writes a note's BNBF blob WITHOUT building a document:
 * the record walks behind the body_text cache and the image ordinals the
 * media browser addresses, and on_note_document_load /
 * on_note_document_save — the document's two ends.
 * =========================================================================== */

#include "serialize.h"
#include "db.h"                      /* OnActionItem (extraction result)    */

#include <string.h>

/*
 * on_note_count_images — see serialize.h.
 */
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

/*
 * on_note_image_nth_png — see serialize.h.
 */
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

/*
 * on_note_extract_text — see serialize.h.
 */
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

/*
 * on_note_text_cached — see serialize.h.
 */
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

/*
 * on_note_document_load — see serialize.h.
 */
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

/* ---------------------------------------------------------------------------
 * FreshUids — the uids one save handed to a document's lines
 * (on_db_action_uid_alloc), kept until its transaction ends: they exist
 * only if it commits.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnDocument *doc;                 /* the saved document (not owned)      */
    GHashTable *uids;                /* gint64 uid -> itself (owned keys)   */
} FreshUids;

/*
 * fresh_uids_end — an on_db_txn_on_end hook: the save's transaction ended.
 * Not committed: every line still carrying one of the save's fresh uids
 * goes back to uid 0, so the next save allocates again — a rolled-back
 * uid left in the document would be handed out a second time and two
 * lines would claim one task.  Committed: nothing to do.  Frees `data`.
 *
 * Inputs:
 *   data      — the FreshUids
 *   committed — whether the save's transaction committed
 *
 * Output: none.
 */
static void
fresh_uids_end(gpointer data, gboolean committed)
{
    FreshUids *fresh = data;
    if (!committed) {
        GArray *blocks = on_document_action_blocks(fresh->doc);
        for (guint i = 0; i < blocks->len; i++) {
            OnBlock *b = on_document_block(fresh->doc, g_array_index(blocks, guint, i));
            if (g_hash_table_contains(fresh->uids, &b->action_uid))
                b->action_uid = 0;
        }
        g_array_unref(blocks);
    }
    g_hash_table_destroy(fresh->uids);
    g_free(fresh);
}

/*
 * on_note_document_save — see serialize.h.
 */
gboolean
on_note_document_save(OnDatabase *db, gint64 id, OnDocument *doc,
                      gboolean sync_actions, gboolean *changed)
{
    if (changed != NULL)
        *changed = FALSE;
    gboolean own;                    /* this call's transaction?            */
    if (!on_db_txn_begin(db, &own))
        return FALSE;

    /* 1. Identities, before the bytes: dedupe, then allocate.  The fresh
     *    uids are taken back if the transaction does not commit.          */
    GArray *blocks = on_document_action_blocks(doc);
    GHashTable *seen = g_hash_table_new(g_int64_hash, g_int64_equal);
    FreshUids *fresh = g_new0(FreshUids, 1);
    fresh->doc  = doc;
    fresh->uids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    on_db_txn_on_end(db, fresh_uids_end, fresh);
    GList *items = NULL;             /* the lines as OnActionItem, reversed */
    gboolean ok = TRUE;              /* the outcome                         */
    for (guint i = 0; i < blocks->len && ok; i++) {
        OnBlock *b = on_document_block(doc, g_array_index(blocks, guint, i));
        if (b->action_uid != 0 && g_hash_table_contains(seen, &b->action_uid))
            b->action_uid = 0;       /* a pasted copy: a new item           */
        if (b->action_uid == 0) {
            b->action_uid = on_db_action_uid_alloc(db);
            ok = b->action_uid != 0;
            if (ok)
                g_hash_table_add(fresh->uids, g_memdup2(&b->action_uid, sizeof b->action_uid));
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
        gchar  *title = on_document_title(doc, SHARED_DB_NOTE_DEFAULT_TITLE,
                                          ON_TITLE_MAX_CHARS);
        gchar  *body  = on_document_plain_text(doc);
        ok = on_db_note_save(db, id, title, blob, blob_len, body);
        g_free(body);
        g_free(title);
        g_free(blob);
    }

    /* 3. The tasks the lines are: the push.                               */
    if (ok && sync_actions)
        ok = on_db_note_save_actions(db, id, items, changed);
    on_db_action_list_free(items);
    return on_db_txn_end(db, own, ok);
}

/*
 * on_note_document_save_tags — see serialize.h.
 */
gboolean
on_note_document_save_tags(OnDatabase *db, gint64 id, const OnDocument *doc)
{
    GList   *tags = on_document_collect_tags(doc);
    gboolean ok   = on_db_note_set_tags(db, id, tags);
    g_list_free_full(tags, g_free);
    return ok;
}

/*
 * on_note_new_from_text — see serialize.h.
 */
gint64
on_note_new_from_text(OnDatabase *db, gint64 folder_id, const gchar *text)
{
    gboolean own;                    /* this call's transaction?            */
    if (!on_db_txn_begin(db, &own))
        return 0;
    gint64      id  = on_db_note_create(db, folder_id);
    OnDocument *doc = on_document_from_text(text);
    gboolean    ok  = id != 0 && on_note_document_save(db, id, doc, TRUE, NULL);
    on_note_document_release(db, doc);
    return on_db_txn_end(db, own, ok) ? id : 0;
}

/*
 * document_free_end — an on_db_txn_on_end hook: free a document once the
 * transaction its save joined has ended (its FreshUids hook, registered
 * earlier, has run by then).
 *
 * Inputs:
 *   data      — the OnDocument
 *   committed — unused
 *
 * Output: none.
 */
static void
document_free_end(gpointer data, gboolean committed)
{
    (void)committed;
    on_document_free(data);
}

/*
 * on_note_document_release — see serialize.h.
 */
void
on_note_document_release(OnDatabase *db, OnDocument *doc)
{
    if (doc != NULL && !on_db_txn_on_end(db, document_free_end, doc))
        on_document_free(doc);
}

/*
 * action_unmark — turn the real action line in `block` into a plain line:
 * delete the leading '!' and the whitespace after it, again while the line
 * still starts with '!' (so "!! x" does not stay an item), as one undo
 * group; the rest — text, a "due <date>" suffix, its formatting — stays.
 * The block's uid is cleared, so the line is no longer that item (an undo
 * brings the '!' back as a NEW item, the deleted task staying deleted).
 *
 * Inputs:
 *   doc   — the document
 *   block — the block index of a real action line
 *
 * Output: none.
 */
static void
action_unmark(OnDocument *doc, guint block)
{
    OnBlock     *b    = on_document_block(doc, block);
    const gchar *text = b->text->text->str;
    gsize        n    = 0;           /* bytes of the prefix to delete       */
    while (text[n] == '!') {
        n++;
        while (text[n] != '\0' && g_ascii_isspace((guchar)text[n]))
            n++;
    }
    OnPos at = { block, -1, 0 };
    on_document_begin_group(doc);
    on_document_delete_text(doc, at, n);
    on_document_end_group(doc);
    b->action_uid = 0;
}

/*
 * on_note_document_reconcile — see serialize.h.
 */
gint
on_note_document_reconcile(OnDatabase *db, gint64 id, OnDocument *doc,
                           gboolean *changed)
{
    gboolean ok = TRUE;              /* every write succeeded               */
    gboolean edited = FALSE;         /* the document was rewritten          */
    gboolean push   = FALSE;         /* a render was refused                */
    GArray  *rows   = on_db_action_links(db, id);
    for (guint r = 0; r < rows->len && ok; r++) {
        OnActionLink *row = &g_array_index(rows, OnActionLink, r);
        gint ord = on_document_action_ord(doc, row->uid);
        if (row->deleted && !row->seen_deleted) {
            /* Deleted elsewhere: the line becomes plain text, the link goes. */
            if (ord >= 0) {
                GArray *blocks = on_document_action_blocks(doc);
                action_unmark(doc, g_array_index(blocks, guint, ord));
                g_array_unref(blocks);
                edited = TRUE;
            }
            ok = on_db_action_link_delete(db, row->uid);
            continue;
        }
        if (row->deleted || row->seen_deleted)
            continue;                /* Notes' own tombstone, or restored
                                        elsewhere with no line: nothing to
                                        pull                                */
        gchar   *text = NULL;        /* the line as it reads now            */
        gint64   due  = 0;
        gboolean done = FALSE;
        if (ord >= 0) {
            GArray *blocks = on_document_action_blocks(doc);
            on_document_block_action(doc, g_array_index(blocks, guint, ord),
                                     &text, &due, &done);
            g_array_unref(blocks);
        }
        gboolean seen = FALSE;       /* a seen_* value moved to the task's  */
        if (on_db_action_link_title_moved(row)) {
            /* A title with a NUL or invalid UTF-8 is refused like one the
             * line cannot take (a line break, empty, ending in a due).     */
            gboolean same = row->title_ok && g_strcmp0(text, row->title) == 0;
            if (ord >= 0 && !same) {
                if (row->title_ok && on_document_action_text(doc, ord, row->title))
                    edited = TRUE;
                else
                    push = TRUE;     /* the line keeps its text; the push
                                        writes it back to the task          */
            }
            on_db_action_link_agree_title(row, row->title, row->title_len);
            seen = TRUE;
        }
        if (row->done != row->seen_done) {
            if (ord >= 0 && done != row->done)
                edited |= on_document_action_strike(doc, ord, row->done);
            row->seen_done = row->done;
            seen = TRUE;
        }
        if (row->due != row->seen_due) {
            if (ord >= 0 && due != row->due) {
                if (on_document_action_due(doc, ord, row->due))
                    edited = TRUE;
                else
                    push = TRUE;     /* a time the line cannot show        */
            }
            row->seen_due = row->due;
            seen = TRUE;
        }
        g_free(text);
        if (seen)
            ok = on_db_action_link_update(db, row);
    }
    g_array_unref(rows);
    if (changed != NULL)
        *changed = edited;
    if (!ok)
        return -1;
    return edited || push ? 1 : 0;
}
