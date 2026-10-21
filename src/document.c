/* ===========================================================================
 * document.c — OnDocument, the block model of a note (implementation)
 *
 * See document.h for the API and BLOCK_MODEL.md for the design.  Three
 * parts: the OnText primitives (runs that tile a byte string), the block
 * and document containers, and the BNBF loader/saver — the loader is a
 * small line-oriented state machine over the record reader, the saver a
 * run merger over the record writer.
 * =========================================================================== */

#include "document.h"

#include <string.h>

/* ===========================================================================
 * OnText
 * ======================================================================== */

OnText *
on_text_new(void)
{
    OnText *t = g_new0(OnText, 1);
    t->text   = g_string_new(NULL);
    t->runs   = g_array_new(FALSE, FALSE, sizeof(OnRun));
    t->objects = g_array_new(FALSE, FALSE, sizeof(OnInlineObject));
    return t;
}

/* inline_object_clear() — release what an inline object holds.             */
static void
inline_object_clear(OnInlineObject *obj)
{
    if (obj->pixels_free != NULL)
        obj->pixels_free(obj->pixels);
    if (obj->png != NULL)
        g_bytes_unref(obj->png);
    g_free(obj->file_name);
}

/* inline_object_dup() — a copy of an object, at `offset`: its bytes shared,
 * its name copied, a view's pixel cache not copied.                         */
static OnInlineObject
inline_object_dup(const OnInlineObject *src, gsize offset)
{
    OnInlineObject c = *src;
    c.offset      = offset;
    c.png         = src->png != NULL ? g_bytes_ref(src->png) : NULL;
    c.file_name   = g_strdup(src->file_name);
    c.pixels      = NULL;
    c.pixels_free = NULL;
    return c;
}

/* text_insert_object() — put an object (taken, not copied) into the text's
 * array, keeping it sorted by offset.                                       */
static void
text_insert_object(OnText *t, OnInlineObject obj)
{
    guint k = 0;
    while (k < t->objects->len &&
           g_array_index(t->objects, OnInlineObject, k).offset < obj.offset)
        k++;
    g_array_insert_val(t->objects, k, obj);
}

void
on_text_free(OnText *t)
{
    if (t == NULL)
        return;
    for (guint i = 0; i < t->objects->len; i++)
        inline_object_clear(&g_array_index(t->objects, OnInlineObject, i));
    g_array_free(t->objects, TRUE);
    g_array_free(t->runs, TRUE);
    g_string_free(t->text, TRUE);
    g_free(t);
}

OnText *
on_text_copy(const OnText *t)
{
    OnText *c = on_text_new();
    g_string_append_len(c->text, t->text->str, (gssize)t->text->len);
    g_array_append_vals(c->runs, t->runs->data, t->runs->len);
    for (guint i = 0; i < t->objects->len; i++) {
        const OnInlineObject *src = &g_array_index(t->objects, OnInlineObject, i);
        OnInlineObject dup = inline_object_dup(src, src->offset);
        g_array_append_val(c->objects, dup);
    }
    return c;
}

/* ---------------------------------------------------------------------------
 * runs_normalize() — drop empty runs and merge adjacent equal-flag ones, so
 * the run array is always the unique minimal tiling of the text.
 * ------------------------------------------------------------------------- */
static void
runs_normalize(OnText *t)
{
    guint out = 0;                   /* write index                         */
    for (guint i = 0; i < t->runs->len; i++) {
        OnRun r = g_array_index(t->runs, OnRun, i);
        if (r.len == 0)
            continue;
        if (out > 0 &&
            g_array_index(t->runs, OnRun, out - 1).flags == r.flags) {
            g_array_index(t->runs, OnRun, out - 1).len += r.len;
            continue;
        }
        g_array_index(t->runs, OnRun, out++) = r;
    }
    g_array_set_size(t->runs, out);
}

/* ---------------------------------------------------------------------------
 * runs_split_at() — make `pos` a run boundary (a no-op when it already is,
 * or when pos is 0 / the text length).  Returns the index of the run that
 * STARTS at pos, or runs->len when pos is the end.
 * ------------------------------------------------------------------------- */
static guint
runs_split_at(OnText *t, gsize pos)
{
    gsize at = 0;                    /* start of run i                      */
    for (guint i = 0; i < t->runs->len; i++) {
        OnRun *r = &g_array_index(t->runs, OnRun, i);
        if (at == pos)
            return i;
        if (pos < at + r->len) {
            OnRun tail = { at + r->len - pos, r->flags };
            r->len = pos - at;
            g_array_insert_val(t->runs, i + 1, tail);
            return i + 1;
        }
        at += r->len;
    }
    return t->runs->len;
}

void
on_text_insert(OnText *t, gsize off, const gchar *s, gsize n,
               guint32 flags)
{
    g_return_if_fail(off <= t->text->len);
    if (n == 0)
        return;

    guint i = runs_split_at(t, off);
    OnRun r = { n, flags & ON_FMT_RUN_MASK };
    g_array_insert_val(t->runs, i, r);
    runs_normalize(t);

    g_string_insert_len(t->text, (gssize)off, s, (gssize)n);
    for (guint k = 0; k < t->objects->len; k++) {
        OnInlineObject *img = &g_array_index(t->objects, OnInlineObject, k);
        if (img->offset >= off)
            img->offset += n;
    }
}

void
on_text_append(OnText *t, const gchar *s, gsize n, guint32 flags)
{
    on_text_insert(t, t->text->len, s, n, flags);
}

void
on_text_delete(OnText *t, gsize off, gsize n)
{
    g_return_if_fail(off + n <= t->text->len);
    if (n == 0)
        return;

    guint first = runs_split_at(t, off);
    guint last  = runs_split_at(t, off + n);     /* first run AFTER span   */
    g_array_remove_range(t->runs, first, last - first);
    runs_normalize(t);

    g_string_erase(t->text, (gssize)off, (gssize)n);
    for (guint k = 0; k < t->objects->len; ) {
        OnInlineObject *img = &g_array_index(t->objects, OnInlineObject, k);
        if (img->offset >= off && img->offset < off + n) {
            inline_object_clear(img);
            g_array_remove_index(t->objects, k);
            continue;
        }
        if (img->offset >= off + n)
            img->offset -= n;
        k++;
    }
}

void
on_text_set_flags(OnText *t, gsize off, gsize n, guint32 mask,
                  gboolean on)
{
    g_return_if_fail(off + n <= t->text->len);
    if (n == 0)
        return;
    mask &= ON_FMT_RUN_MASK;
    guint first = runs_split_at(t, off);
    guint last  = runs_split_at(t, off + n);
    for (guint i = first; i < last; i++) {
        OnRun *r = &g_array_index(t->runs, OnRun, i);
        r->flags = on ? (r->flags | mask) : (r->flags & ~mask);
    }
    runs_normalize(t);
}

guint32
on_text_flags_at(const OnText *t, gsize off)
{
    gsize at = 0;                    /* start of run i                      */
    guint32 last = 0;                /* flags of the run just passed        */
    for (guint i = 0; i < t->runs->len; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        if (off < at + r->len)
            return r->flags;
        at  += r->len;
        last = r->flags;
    }
    return last;
}

void
on_text_add_image(OnText *t, gsize off, GBytes *png, guint32 display_width)
{
    g_return_if_fail(off + ON_OBJ_CHAR_LEN <= t->text->len);
    g_return_if_fail(memcmp(t->text->str + off, ON_OBJ_CHAR,
                            ON_OBJ_CHAR_LEN) == 0);
    OnInlineObject img = { .kind = ON_OBJECT_IMAGE, .offset = off,
                           .png = g_bytes_ref(png), .display_width = display_width };
    text_insert_object(t, img);
}

void
on_text_add_file(OnText *t, gsize off, gint64 id, const gchar *name)
{
    g_return_if_fail(off + ON_OBJ_CHAR_LEN <= t->text->len);
    g_return_if_fail(memcmp(t->text->str + off, ON_OBJ_CHAR,
                            ON_OBJ_CHAR_LEN) == 0);
    OnInlineObject file = { .kind = ON_OBJECT_FILE, .offset = off,
                            .file_id = id, .file_name = g_strdup(name) };
    text_insert_object(t, file);
}

/* ===========================================================================
 * BLOCKS
 * ======================================================================== */

void
on_text_walk(const OnText *t, const OnTextWalk *walk, gpointer data)
{
    gsize at = 0;                    /* start of the current run            */
    guint k  = 0;                    /* next inline image                   */
    for (guint i = 0; i < t->runs->len; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        gsize cur = at, end = at + r->len;
        while (k < t->objects->len &&
               g_array_index(t->objects, OnInlineObject, k).offset < end) {
            const OnInlineObject *img =
                &g_array_index(t->objects, OnInlineObject, k);
            walk->text(t->text->str + cur, img->offset - cur, r->flags, data);
            if (walk->object != NULL)
                walk->object(img, data);
            cur = img->offset + ON_OBJ_CHAR_LEN;
            k++;
        }
        walk->text(t->text->str + cur, end - cur, r->flags, data);
        at = end;
    }
}

gboolean
on_block_kind_is_text(OnBlockKind kind)
{
    return kind != ON_BLOCK_IMAGE && kind != ON_BLOCK_TABLE;
}

gboolean
on_block_kind_is_list(OnBlockKind kind)
{
    return kind == ON_BLOCK_BULLET || kind == ON_BLOCK_NUMBER ||
           kind == ON_BLOCK_CHECK;
}

gint
on_block_number(gint previous, OnBlockKind kind)
{
    return kind == ON_BLOCK_NUMBER ? previous + 1 : 0;
}

guint32
on_block_kind_para_flag(OnBlockKind kind)
{
    switch (kind) {
    case ON_BLOCK_H1:     return ON_FMT_H1;
    case ON_BLOCK_H2:     return ON_FMT_H2;
    case ON_BLOCK_BULLET: return ON_FMT_LIST_BULLET;
    case ON_BLOCK_NUMBER: return ON_FMT_LIST_NUMBER;
    case ON_BLOCK_CHECK:  return ON_FMT_LIST_CHECK;
    case ON_BLOCK_CODE:   return ON_FMT_CODEBLOCK;
    default:              return 0;
    }
}

OnBlock *
on_block_new_text(OnBlockKind kind)
{
    g_return_val_if_fail(on_block_kind_is_text(kind), NULL);
    OnBlock *b = g_new0(OnBlock, 1);
    b->kind = kind;
    b->text = on_text_new();
    return b;
}

OnBlock *
on_block_new_image(GBytes *png, guint32 display_width)
{
    OnBlock *b = g_new0(OnBlock, 1);
    b->kind          = ON_BLOCK_IMAGE;
    b->png           = g_bytes_ref(png);
    b->display_width = display_width;
    return b;
}

OnBlock *
on_block_new_table(gint rows, gint cols)
{
    OnBlock *b = g_new0(OnBlock, 1);
    b->kind  = ON_BLOCK_TABLE;
    b->rows  = MAX(1, rows);
    b->cols  = MAX(1, cols);
    b->cells = g_ptr_array_new_with_free_func((GDestroyNotify)on_text_free);
    for (gint i = 0; i < b->rows * b->cols; i++)
        g_ptr_array_add(b->cells, on_text_new());
    return b;
}

void
on_block_free(OnBlock *b)
{
    if (b == NULL)
        return;
    on_text_free(b->text);
    if (b->pixels_free != NULL)
        b->pixels_free(b->pixels);
    if (b->png != NULL)
        g_bytes_unref(b->png);
    if (b->cells != NULL)
        g_ptr_array_free(b->cells, TRUE);
    g_free(b);
}

/* block_forget_pixels() — drop a block's view caches (its decoded image and
 * its inline images'): a block leaving the document for the undo log keeps
 * its bytes, not its pixels, which a redo decodes again.                    */
static void
block_forget_pixels(OnBlock *b)
{
    if (b->pixels_free != NULL)
        b->pixels_free(b->pixels);
    b->pixels      = NULL;
    b->pixels_free = NULL;
    for (guint k = 0; b->text != NULL && k < b->text->objects->len; k++) {
        OnInlineObject *img = &g_array_index(b->text->objects, OnInlineObject, k);
        if (img->pixels_free != NULL)
            img->pixels_free(img->pixels);
        img->pixels      = NULL;
        img->pixels_free = NULL;
    }
}

OnBlock *
on_block_copy(const OnBlock *b)
{
    OnBlock *c = g_new0(OnBlock, 1);
    *c = *b;
    c->text  = (b->text != NULL) ? on_text_copy(b->text) : NULL;
    c->png   = (b->png != NULL) ? g_bytes_ref(b->png) : NULL;
    c->pixels      = NULL;           /* a view's cache is not copied        */
    c->pixels_free = NULL;
    c->cells = NULL;
    if (b->cells != NULL) {
        c->cells = g_ptr_array_new_with_free_func(
            (GDestroyNotify)on_text_free);
        for (guint i = 0; i < b->cells->len; i++)
            g_ptr_array_add(c->cells,
                            on_text_copy(g_ptr_array_index(b->cells, i)));
    }
    return c;
}

OnText *
on_block_cell(const OnBlock *b, gint r, gint c)
{
    if (b->kind != ON_BLOCK_TABLE ||
        r < 0 || r >= b->rows || c < 0 || c >= b->cols)
        return NULL;
    return g_ptr_array_index(b->cells, r * b->cols + c);
}

/* ===========================================================================
 * DOCUMENT
 * ======================================================================== */

typedef struct Op Op;                /* one undoable step, see UNDO         */

struct OnDocument {
    GPtrArray *blocks;               /* OnBlock*, at least one              */
    /* undo */
    GPtrArray *undo_stack;           /* GPtrArray* of Op*, oldest first     */
    GPtrArray *redo_stack;           /* same                                */
    GPtrArray *open_group;           /* group being filled, or NULL         */
    gint       group_depth;          /* begin_group nesting                 */
    /* observer */
    OnDocumentObserver observer;
    gpointer   observer_data;
    /* change flags */
    gboolean   tags_modified;
    gboolean   actions_modified;
    OnPos      last_change;          /* see on_document_last_change         */
};

static void group_free(GPtrArray *group);

/* doc_alloc() — the empty shell every constructor fills.                    */
static OnDocument *
doc_alloc(void)
{
    OnDocument *d = g_new0(OnDocument, 1);
    d->blocks = g_ptr_array_new_with_free_func((GDestroyNotify)on_block_free);
    d->undo_stack = g_ptr_array_new_with_free_func((GDestroyNotify)group_free);
    d->redo_stack = g_ptr_array_new_with_free_func((GDestroyNotify)group_free);
    return d;
}

OnDocument *
on_document_new(void)
{
    OnDocument *d = doc_alloc();
    g_ptr_array_add(d->blocks, on_block_new_text(ON_BLOCK_PARA));
    return d;
}

OnDocument *
on_document_new_from_block(OnBlock *block)
{
    OnDocument *d = doc_alloc();
    g_ptr_array_add(d->blocks, block);
    return d;
}

void
on_document_free(OnDocument *d)
{
    if (d == NULL)
        return;
    if (d->open_group != NULL)
        group_free(d->open_group);
    g_ptr_array_free(d->undo_stack, TRUE);
    g_ptr_array_free(d->redo_stack, TRUE);
    g_ptr_array_free(d->blocks, TRUE);
    g_free(d);
}

guint
on_document_n_blocks(const OnDocument *d)
{
    return d->blocks->len;
}

OnBlock *
on_document_block(const OnDocument *d, guint i)
{
    return (i < d->blocks->len) ? g_ptr_array_index(d->blocks, i) : NULL;
}

/* ---------------------------------------------------------------------------
 * text_check() — the OnText invariants; `where` names the owner in `why`.
 * ------------------------------------------------------------------------- */
static gboolean
text_check(const OnText *t, gboolean allow_newline, const gchar *where,
           GString *why)
{
#define FAIL(...) G_STMT_START {                                        \
        if (why != NULL) {                                              \
            g_string_printf(why, "%s: ", where);                        \
            g_string_append_printf(why, __VA_ARGS__);                   \
        }                                                               \
        return FALSE;                                                   \
    } G_STMT_END

    if (!allow_newline && on_document_text_has_line_break(t->text->str, t->text->len))
        FAIL("text contains a line break");
    if (!g_utf8_validate_len(t->text->str, (gssize)t->text->len, NULL))
        FAIL("text is not valid UTF-8");

    gsize sum = 0;                   /* bytes covered by the runs so far    */
    guint32 prev = 0;                /* flags of the previous run           */
    for (guint i = 0; i < t->runs->len; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        if (r->len == 0)
            FAIL("run %u is empty", i);
        if (i > 0 && r->flags == prev)
            FAIL("runs %u and %u carry equal flags", i - 1, i);
        if (r->flags & ~ON_FMT_RUN_MASK)
            FAIL("run %u carries non-run bits 0x%x", i, r->flags);
        sum += r->len;
        if (sum > t->text->len)
            FAIL("runs overrun the text");
        /* A run boundary on a UTF-8 continuation byte splits a character. */
        if (sum < t->text->len && ((guchar)t->text->str[sum] & 0xc0) == 0x80)
            FAIL("run %u ends inside a character", i);
        prev = r->flags;
    }
    if (sum != t->text->len)
        FAIL("runs cover %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " bytes",
             sum, t->text->len);

    gsize last = 0;                  /* previous image offset               */
    for (guint k = 0; k < t->objects->len; k++) {
        const OnInlineObject *img = &g_array_index(t->objects, OnInlineObject, k);
        if (k > 0 && img->offset <= last)
            FAIL("inline objects out of order");
        if (img->offset + ON_OBJ_CHAR_LEN > t->text->len ||
            memcmp(t->text->str + img->offset, ON_OBJ_CHAR,
                   ON_OBJ_CHAR_LEN) != 0)
            FAIL("inline object %u has no U+FFFC at its offset", k);
        if (img->kind == ON_OBJECT_IMAGE && img->png == NULL)
            FAIL("inline image %u has no bytes", k);
        if (img->kind == ON_OBJECT_FILE &&
            (img->file_id <= 0 || img->file_name == NULL ||
             img->file_name[0] == '\0'))
            FAIL("inline file %u has no id or name", k);
        last = img->offset;
    }
    /* Every U+FFFC must have an object.                                    */
    guint n_obj = 0;                 /* U+FFFC characters in the text       */
    for (const gchar *p = t->text->str;
         (p = g_strstr_len(p, (gssize)(t->text->len - (p - t->text->str)),
                           ON_OBJ_CHAR)) != NULL;
         p += ON_OBJ_CHAR_LEN)
        n_obj++;
    if (n_obj != t->objects->len)
        FAIL("%u U+FFFC characters but %u inline objects", n_obj,
             t->objects->len);
    return TRUE;
#undef FAIL
}

gboolean
on_document_check(const OnDocument *d, GString *why)
{
    if (d->blocks->len == 0) {
        if (why != NULL)
            g_string_assign(why, "document has no blocks");
        return FALSE;
    }
    GHashTable *uids = g_hash_table_new(g_int64_hash, g_int64_equal);
    gboolean ok = TRUE;
    gchar where[64];                 /* "block N" / "block N cell M"        */
    for (guint i = 0; ok && i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        g_snprintf(where, sizeof where, "block %u", i);
        if (b->kind < 0 || b->kind >= ON_N_BLOCK_KINDS) {
            if (why != NULL)
                g_string_printf(why, "%s: bad kind %d", where, b->kind);
            ok = FALSE;
        } else if (on_block_kind_is_text(b->kind)) {
            ok = b->text != NULL && text_check(b->text, FALSE, where, why);
            if (b->text == NULL && why != NULL)
                g_string_printf(why, "%s: text block without text", where);
        } else if (b->kind == ON_BLOCK_IMAGE) {
            if (b->png == NULL) {
                if (why != NULL)
                    g_string_printf(why, "%s: image without bytes", where);
                ok = FALSE;
            }
        } else {                     /* TABLE                                */
            if (b->rows < 1 || b->cols < 1 || b->cells == NULL ||
                b->cells->len != (guint)(b->rows * b->cols)) {
                if (why != NULL)
                    g_string_printf(why, "%s: table shape", where);
                ok = FALSE;
            }
            for (guint c = 0; ok && c < b->cells->len; c++) {
                const OnText *cell = g_ptr_array_index(b->cells, c);
                g_snprintf(where, sizeof where, "block %u cell %u", i, c);
                if (cell == NULL || cell->objects->len != 0) {
                    if (why != NULL)
                        g_string_printf(why, "%s: %s", where,
                                        cell == NULL ? "missing"
                                                     : "holds an image");
                    ok = FALSE;
                } else {
                    ok = text_check(cell, TRUE, where, why);
                }
            }
        }
        if (ok && b->eol_flags & ~ON_FMT_RUN_MASK) {
            if (why != NULL)
                g_string_printf(why, "%s: eol_flags carry non-run bits",
                                where);
            ok = FALSE;
        }
        if (ok && b->action_uid != 0) {
            if (g_hash_table_contains(uids, &b->action_uid)) {
                if (why != NULL)
                    g_string_printf(why, "%s: duplicate action uid", where);
                ok = FALSE;
            } else {
                g_hash_table_add(uids, (gpointer)&b->action_uid);
            }
        }
    }
    g_hash_table_unref(uids);
    return ok;
}

/* ===========================================================================
 * LOADER — a line-oriented state machine over the record reader.
 *
 * `cur` is the block being built for the current line, NULL between lines.
 * TEXT records are cut at every '\n'; the piece before each newline goes
 * to text_piece(), the newline itself to line_end().  Objects (CHECK,
 * IMAGE, TABLE) arrive whole.  finish_line() turns the accumulated line
 * into its block: it decides the kind from the paragraph bit and strips
 * the list prefix / checkbox space the format carries in the text.
 * ======================================================================== */

static gboolean block_is_real_action(const OnBlock *b);   /* with the
                                        action helpers, after the model     */

typedef struct {
    OnDocument      *doc;
    OnDocLoadReport *rep;            /* always non-NULL inside the loader   */
    OnBlock         *cur;            /* the line's block so far, or NULL    */
    guint32          line_para;      /* paragraph bit of the latest piece   */
    gboolean         para_seen;      /* any piece yet (text or newline)     */
    gboolean         mixed;          /* pieces disagreed on the para bit    */
    gboolean         from_check;     /* cur was started by a CHECK record   */
    gboolean         strip_space;    /* the CHECK's " " is still expected   */
    gint             number_run;     /* consecutive NUMBER blocks so far    */
    gint64           pending_uid;    /* an ACTION record's uid, for the
                                        block the current line becomes     */
    GHashTable      *uids;           /* uids seen so far (dedupe), lazily   */
} Loader;

/* para_bit() — the (single) paragraph bit of `flags`; several set count as
 * mixed and the lowest wins.                                                */
static guint32
para_bit(Loader *L, guint32 flags)
{
    guint32 para = flags & ON_FMT_PARA_MASK;
    if (para & (para - 1)) {         /* more than one bit                   */
        L->mixed = TRUE;
        para &= 0u - para;           /* lowest set bit                      */
    }
    return para;
}

/* note_para() — record a piece's paragraph bit for the line.  The FIRST
 * piece decides (the editor reads a line's style off its first character,
 * `line_para_flags`); a later piece that disagrees only marks the line
 * mixed.  For an empty line the newline is the first piece.                 */
static void
note_para(Loader *L, guint32 para)
{
    if (!L->para_seen)
        L->line_para = para;
    else if (para != L->line_para)
        L->mixed = TRUE;
    L->para_seen = TRUE;
}

/* strip_prefix() — remove the `n_bytes` list prefix at the start of a text,
 * counting a styled one.                                                    */
static void
strip_prefix(Loader *L, OnText *t, gsize n_bytes)
{
    gsize at = 0;                    /* run start                           */
    for (guint i = 0; i < t->runs->len && at < n_bytes; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        if (r->flags != 0) {
            L->rep->styled_prefix++;
            break;
        }
        at += r->len;
    }
    on_text_delete(t, 0, n_bytes);
}

/* ---------------------------------------------------------------------------
 * finish_line() — close the current line's block and append it.
 * ------------------------------------------------------------------------- */
static void
finish_line(Loader *L)
{
    OnBlock *b = L->cur;
    if (b == NULL)
        b = on_block_new_text(ON_BLOCK_PARA);    /* an empty line           */

    if (L->strip_space) {            /* CHECK with nothing after it         */
        L->rep->check_no_space++;
        L->strip_space = FALSE;
    }
    if (L->mixed)
        L->rep->mixed_para++;

    if (on_block_kind_is_text(b->kind)) {
        guint32 para = L->line_para;
        if (L->from_check) {
            if (para != ON_FMT_LIST_CHECK)
                L->rep->check_no_tag++;
            b->kind = ON_BLOCK_CHECK;
        } else if (para == ON_FMT_LIST_CHECK) {
            L->rep->check_no_box++;
            b->kind = ON_BLOCK_CHECK;
        } else if (para == ON_FMT_H1) {
            b->kind = ON_BLOCK_H1;
        } else if (para == ON_FMT_H2) {
            b->kind = ON_BLOCK_H2;
        } else if (para == ON_FMT_CODEBLOCK) {
            b->kind = ON_BLOCK_CODE;
        } else if (para == ON_FMT_LIST_BULLET) {
            b->kind = ON_BLOCK_BULLET;
            if (g_str_has_prefix(b->text->text->str, ON_BULLET_PREFIX))
                strip_prefix(L, b->text, strlen(ON_BULLET_PREFIX));
            else
                L->rep->bullet_no_prefix++;
        } else if (para == ON_FMT_LIST_NUMBER) {
            b->kind = ON_BLOCK_NUMBER;
            const gchar *str = b->text->text->str;
            glong chars = g_ascii_isdigit(str[0])
                          ? on_list_prefix_chars(str) : 0;
            if (chars > 0) {
                if (g_ascii_strtoll(str, NULL, 10) != L->number_run + 1)
                    L->rep->number_renumbered++;    /* not its position    */
                strip_prefix(L, b->text, (gsize)chars);   /* ASCII: 1 byte
                                                             per character */
            } else {
                L->rep->number_renumbered++;        /* no "N. " at all     */
            }
        }
    } else if (L->line_para != 0) {
        /* An image/table line whose newline carried a paragraph style: the
         * model has nowhere to keep it.                                    */
        L->rep->mixed_para++;
    }

    b->action_uid = L->pending_uid;
    if (L->pending_uid != 0 && !block_is_real_action(b))
        L->rep->action_stray++;      /* kept on the block, not written back */
    L->pending_uid = 0;

    L->number_run = on_block_number(L->number_run, b->kind);
    g_ptr_array_add(L->doc->blocks, b);
    L->cur        = NULL;
    L->line_para  = 0;
    L->para_seen  = FALSE;
    L->mixed      = FALSE;
    L->from_check = FALSE;
}

/* ---------------------------------------------------------------------------
 * ensure_text() — make `cur` a text block for a piece of text or an inline
 * image: start one, promote an image block (its image becomes inline), or
 * split away a table.
 * ------------------------------------------------------------------------- */
static OnBlock *
ensure_text(Loader *L)
{
    if (L->cur == NULL) {
        L->cur = on_block_new_text(ON_BLOCK_PARA);
    } else if (L->cur->kind == ON_BLOCK_IMAGE) {
        OnBlock *img = L->cur;
        L->cur = on_block_new_text(ON_BLOCK_PARA);
        on_text_append(L->cur->text, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN, 0);
        on_text_add_image(L->cur->text, 0, img->png, img->display_width);
        on_block_free(img);
        L->rep->image_inline++;
    } else if (L->cur->kind == ON_BLOCK_TABLE) {
        L->rep->split_table++;
        finish_line(L);
        L->cur = on_block_new_text(ON_BLOCK_PARA);
    }
    return L->cur;
}

/* text_piece() — one newline-free stretch of a TEXT record.                 */
static void
text_piece(Loader *L, const gchar *s, gsize n, guint32 flags)
{
    if (n == 0)
        return;
    if (flags & ~(ON_FMT_PARA_MASK | ON_FMT_RUN_MASK))
        L->rep->unknown_flags++;
    note_para(L, para_bit(L, flags));

    OnBlock *b = ensure_text(L);
    if (L->strip_space) {
        L->strip_space = FALSE;
        if (s[0] == ' ') {
            s++;
            n--;
        } else {
            L->rep->check_no_space++;
        }
    }
    on_text_append(b->text, s, n, flags & ON_FMT_RUN_MASK);
}

/* line_end() — the '\n' of a TEXT record under `flags`.                     */
static void
line_end(Loader *L, guint32 flags)
{
    if (flags & ~(ON_FMT_PARA_MASK | ON_FMT_RUN_MASK))
        L->rep->unknown_flags++;
    note_para(L, para_bit(L, flags));
    if (L->cur == NULL)
        L->cur = on_block_new_text(ON_BLOCK_PARA);
    L->cur->eol_flags = flags & ON_FMT_RUN_MASK;
    finish_line(L);
}

/* image_record() — an IMAGE record: a block of its own on an empty line,
 * inline otherwise.                                                         */
static void
image_record(Loader *L, const OnBnbfRecord *rec)
{
    GBytes *png = g_bytes_new(rec->png, rec->n_png);
    if (L->strip_space) {
        L->strip_space = FALSE;
        L->rep->check_no_space++;
    }
    if (L->cur == NULL && !L->from_check) {
        L->cur = on_block_new_image(png, rec->display_width);
    } else {
        OnBlock *b = ensure_text(L);
        gsize off = b->text->text->len;
        on_text_append(b->text, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN,
                       on_text_flags_at(b->text, off));
        on_text_add_image(b->text, off, png, rec->display_width);
        L->rep->image_inline++;
    }
    g_bytes_unref(png);
}

/* file_record() — a FILE record: an object in the line it sits on, like an
 * inline image (several in a row sit side by side).  A name that is not
 * valid UTF-8 is repaired; an empty one becomes "file".                     */
static void
file_record(Loader *L, const OnBnbfRecord *rec)
{
    if (L->strip_space) {
        L->strip_space = FALSE;
        L->rep->check_no_space++;
    }
    OnBlock *b = ensure_text(L);
    gsize off = b->text->text->len;
    on_text_append(b->text, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN,
                   on_text_flags_at(b->text, off));
    gchar *name = g_utf8_make_valid(rec->name, (gssize)rec->n_name);
    on_text_add_file(b->text, off, rec->file_id, name[0] != '\0' ? name : "file");
    g_free(name);
}

/* table_record() — a TABLE record: always a block of its own.               */
static void
table_record(Loader *L, OnBnbfRecord *rec)
{
    if (L->strip_space) {
        L->strip_space = FALSE;
        L->rep->check_no_space++;
    }
    if (L->cur != NULL || L->from_check) {
        L->rep->split_table++;
        finish_line(L);
    }
    OnBlock *b = on_block_new_table(rec->table->rows, rec->table->cols);
    b->header = rec->table->header;
    for (gint i = 0; i < b->rows * b->cols; i++) {
        const gchar *cell = g_ptr_array_index(rec->table->cells, i);
        OnText *t = g_ptr_array_index(b->cells, i);
        /* Cells may hold newlines; on_text_insert refuses them, so fill
         * the text and its single run directly.                            */
        if (g_utf8_validate(cell, -1, NULL)) {
            g_string_append(t->text, cell);
        } else {
            gchar *valid = g_utf8_make_valid(cell, -1);
            g_string_append(t->text, valid);
            g_free(valid);
            L->rep->bad_utf8++;
        }
        if (t->text->len > 0) {
            OnRun r = { t->text->len, 0 };
            g_array_append_val(t->runs, r);
        }
    }
    on_table_free(rec->table);
    L->cur = b;
}

/* check_record() — a CHECK record: starts a task line (splitting the line
 * when something already sits on it).                                       */
static void
check_record(Loader *L, const OnBnbfRecord *rec)
{
    if (L->cur != NULL || L->from_check) {
        L->rep->split_check++;
        if (L->strip_space)
            L->rep->check_no_space++;
        L->strip_space = FALSE;
        finish_line(L);
    }
    L->cur = on_block_new_text(ON_BLOCK_CHECK);
    L->cur->checked = rec->checked;
    L->from_check   = TRUE;
    L->strip_space  = TRUE;
}

/* action_record() — an ACTION record: the uid of the line it precedes.  A
 * uid seen before in this blob is dropped (a duplicate cannot be two
 * items); a record that sits mid-line, or follows another, is counted as
 * stray but still names the line it is on — the saver puts it back at the
 * line start.                                                               */
static void
action_record(Loader *L, const OnBnbfRecord *rec)
{
    if (L->uids == NULL)
        L->uids = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                        g_free, NULL);
    gint64 *key = g_new(gint64, 1);  /* owned by the table either way       */
    *key = rec->uid;
    if (!g_hash_table_add(L->uids, key)) {
        L->rep->action_dup++;        /* the table freed the duplicate key   */
        return;
    }
    if (L->cur != NULL || L->from_check || L->pending_uid != 0)
        L->rep->action_stray++;
    L->pending_uid = rec->uid;
}

/* ---------------------------------------------------------------------------
 * line_break() — the next line break in [p, end): GtkTextBuffer — and
 * Pango — break a paragraph at "\r\n", a lone "\r" and U+2029 as well as
 * at "\n", so a Windows paste showed as separate lines in the editor and
 * the model gives each its own block.  Returns where the break starts
 * (`end` when there is none) and its length in *brk.
 * ------------------------------------------------------------------------- */
static const gchar *
line_break(const gchar *p, const gchar *end, gsize *brk)
{
    for (; p < end; p++) {
        if (*p == '\n') {
            *brk = 1;
            return p;
        }
        if (*p == '\r') {
            *brk = (p + 1 < end && p[1] == '\n') ? 2 : 1;
            return p;
        }
        if ((guchar)p[0] == 0xe2 && p + 2 < end &&
            (guchar)p[1] == 0x80 && (guchar)p[2] == 0xa9) {
            *brk = 3;                /* U+2029 PARAGRAPH SEPARATOR          */
            return p;
        }
    }
    *brk = 0;
    return end;
}

/*
 * on_document_text_has_line_break — see document.h.
 */
gboolean
on_document_text_has_line_break(const gchar *s, gsize n)
{
    gsize brk;                       /* the break's length (unused)         */
    return line_break(s, s + n, &brk) != s + n;
}

OnDocument *
on_document_from_text(const gchar *text)
{
    /* Text from outside (a paste, a command line) may be anything: the
     * model's is valid UTF-8, and holds U+FFFC only where an image is.     */
    gchar   *valid = g_utf8_make_valid(text != NULL ? text : "", -1);
    GString *clean = g_string_sized_new(strlen(valid));
    for (const gchar *s = valid, *obj; *s != '\0'; s = obj + strlen(ON_OBJ_CHAR)) {
        obj = strstr(s, ON_OBJ_CHAR);
        if (obj == NULL) {
            g_string_append(clean, s);
            break;
        }
        g_string_append_len(clean, s, obj - s);
    }
    g_free(valid);

    OnDocument *d = doc_alloc();
    const gchar *p   = clean->str;
    const gchar *end = p + clean->len;
    while (TRUE) {
        gsize brk;
        const gchar *q = line_break(p, end, &brk);
        OnBlock *b = on_block_new_text(ON_BLOCK_PARA);
        on_text_append(b->text, p, (gsize)(q - p), 0);
        g_ptr_array_add(d->blocks, b);
        if (q == end)
            break;
        p = q + brk;
    }
    g_string_free(clean, TRUE);
    return d;
}

OnDocument *
on_document_from_bnbf(const guint8 *data, gsize len, OnDocLoadReport *rep)
{
    OnDocLoadReport local;           /* used when the caller passes none    */
    if (rep == NULL)
        rep = &local;
    memset(rep, 0, sizeof *rep);

    OnDocument *d = doc_alloc();
    Loader L = { d, rep, NULL, 0, FALSE, FALSE, FALSE, FALSE, 0, 0, NULL };

    if (data != NULL && len > 0) {
        OnBnbfReader r;
        OnBnbfRecord rec;
        if (on_bnbf_open(&r, data, len)) {
            if (r.version < ON_BNBF_VERSION)
                rep->old_version++;
            gboolean prev_text = FALSE;   /* last record was TEXT …       */
            guint32  prev_flags = 0;      /* … under these flags          */
            while (on_bnbf_next(&r, &rec)) {
                if (rec.type == ON_REC_TEXT && prev_text &&
                    rec.flags == prev_flags)
                    rep->run_split++;
                prev_text  = rec.type == ON_REC_TEXT;
                prev_flags = rec.flags;
                switch (rec.type) {
                case ON_REC_TEXT: {
                    const gchar *p   = rec.text;
                    const gchar *end = rec.text + rec.n_text;
                    gchar *valid = NULL;     /* a repaired copy, if needed */
                    if (!g_utf8_validate_len(rec.text, rec.n_text, NULL)) {
                        rep->bad_utf8++;
                        valid = g_utf8_make_valid(rec.text, (gssize)rec.n_text);
                        p     = valid;
                        end   = valid + strlen(valid);
                    }
                    if (rec.n_text == 0 && L.cur == NULL) {
                        /* A style-only record: the saver's way of keeping
                         * an EMPTY trailing heading/code line's kind.  */
                        note_para(&L, para_bit(&L, rec.flags));
                        L.cur = on_block_new_text(ON_BLOCK_PARA);
                    }
                    while (p < end) {
                        gsize brk;               /* bytes of the break     */
                        const gchar *q = line_break(p, end, &brk);
                        text_piece(&L, p, (gsize)(q - p), rec.flags);
                        if (q == end)
                            break;
                        if (*q != '\n')
                            rep->cr_newlines++;
                        line_end(&L, rec.flags);
                        p = q + brk;
                    }
                    g_free(valid);
                    break;
                }
                case ON_REC_IMAGE: image_record(&L, &rec); break;
                case ON_REC_TABLE: table_record(&L, &rec); break;
                case ON_REC_FILE:  file_record(&L, &rec); break;
                case ON_REC_CHECK: check_record(&L, &rec); break;
                case ON_REC_ACTION: action_record(&L, &rec); break;
                default: break;
                }
            }
            if (r.error != NULL)
                rep->error = r.error;
            else if (!r.saw_end)
                rep->error = "missing end marker";
        } else {
            rep->error = r.error;
        }
    }
    /* The last line has no newline; a blob that ends with one (or is empty)
     * ends in an empty block — both are what the saver reverses.           */
    finish_line(&L);
    if (L.uids != NULL)
        g_hash_table_destroy(L.uids);
    return d;
}

gboolean
on_document_load_report_is_clean(const OnDocLoadReport *rep)
{
    return rep->error == NULL && rep->old_version == 0 &&
           rep->mixed_para == 0 && rep->unknown_flags == 0 &&
           rep->bullet_no_prefix == 0 && rep->number_renumbered == 0 &&
           rep->styled_prefix == 0 && rep->check_no_space == 0 &&
           rep->check_no_tag == 0 && rep->check_no_box == 0 &&
           rep->split_check == 0 && rep->split_table == 0 &&
           rep->run_split == 0 && rep->cr_newlines == 0 &&
           rep->action_dup == 0 && rep->action_stray == 0 &&
           rep->bad_utf8 == 0;
}

/* ===========================================================================
 * SAVER — the blocks back to records, through a run merger so adjacent
 * same-flag text (across block boundaries too) becomes one TEXT record.
 * ======================================================================== */

typedef struct {
    OnBnbfWriter w;
    GString     *pend;               /* text not yet written                */
    guint32      pend_flags;         /* its flags                           */
} Saver;

/* sv_flush() — write the pending run, if any.                               */
static void
sv_flush(Saver *S)
{
    if (S->pend->len == 0)
        return;
    on_bnbf_write_text(&S->w, S->pend_flags, S->pend->str, S->pend->len);
    g_string_truncate(S->pend, 0);
}

/* sv_text() — queue `n` bytes under `flags`, merging with the pending run
 * when the flags match.                                                     */
static void
sv_text(Saver *S, guint32 flags, const gchar *s, gsize n)
{
    if (n == 0)
        return;
    if (S->pend->len > 0 && S->pend_flags != flags)
        sv_flush(S);
    S->pend_flags = flags;
    g_string_append_len(S->pend, s, (gssize)n);
}

/* sv_image() — an IMAGE record (breaks the pending run).                    */
static void
sv_image(Saver *S, GBytes *png, guint32 display_width)
{
    sv_flush(S);
    gsize n;
    gconstpointer bytes = g_bytes_get_data(png, &n);
    on_bnbf_write_image(&S->w, display_width, bytes, n);
}

/* What sv_runs' walk callbacks write into.                                 */
typedef struct {
    Saver   *S;
    guint32  para;                   /* the line's paragraph bit            */
} SvRuns;

/* sv_runs_text() / sv_runs_object() — on_text_walk callbacks for sv_runs.   */
static void
sv_runs_text(const gchar *s, gsize n, guint32 flags, gpointer data)
{
    SvRuns *w = data;
    sv_text(w->S, w->para | flags, s, n);
}

static void
sv_runs_object(const OnInlineObject *obj, gpointer data)
{
    Saver *S = ((SvRuns *)data)->S;
    if (obj->kind == ON_OBJECT_FILE) {
        sv_flush(S);
        on_bnbf_write_file(&S->w, obj->file_id, obj->file_name,
                           strlen(obj->file_name));
    } else {
        sv_image(S, obj->png, obj->display_width);
    }
}

/* sv_runs() — a text's runs under `para`, its inline objects as records.    */
static void
sv_runs(Saver *S, const OnText *t, guint32 para)
{
    static const OnTextWalk walk = { sv_runs_text, sv_runs_object };
    SvRuns w = { S, para };
    on_text_walk(t, &walk, &w);
}

guint8 *
on_document_to_bnbf(const OnDocument *d, gsize *out_len)
{
    Saver S;
    on_bnbf_writer_init(&S.w);
    S.pend       = g_string_new(NULL);
    S.pend_flags = 0;

    gint number_run = 0;             /* consecutive NUMBER blocks           */
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        guint32 para = on_block_kind_para_flag(b->kind);
        number_run = on_block_number(number_run, b->kind);

        /* A real action line's identity precedes its text; a uid left on
         * any other block (the '!' was deleted, the line became a bullet)
         * is not written — that item is gone, and the save says so.       */
        if (b->action_uid != 0 && block_is_real_action(b)) {
            sv_flush(&S);
            on_bnbf_write_action(&S.w, b->action_uid);
        }

        switch (b->kind) {
        case ON_BLOCK_CHECK:
            sv_flush(&S);
            on_bnbf_write_check(&S.w, b->checked);
            sv_text(&S, para, " ", 1);
            sv_runs(&S, b->text, para);
            break;
        case ON_BLOCK_BULLET:
            sv_text(&S, para, ON_BULLET_PREFIX, strlen(ON_BULLET_PREFIX));
            sv_runs(&S, b->text, para);
            break;
        case ON_BLOCK_NUMBER: {
            gchar prefix[16];        /* "N. "                               */
            gint n = g_snprintf(prefix, sizeof prefix, "%d. ", number_run);
            sv_text(&S, para, prefix, (gsize)n);
            sv_runs(&S, b->text, para);
            break;
        }
        case ON_BLOCK_IMAGE:
            sv_image(&S, b->png, b->display_width);
            break;
        case ON_BLOCK_TABLE:
            sv_flush(&S);
            on_bnbf_write_table_begin(&S.w, b->header, b->rows, b->cols);
            for (guint c = 0; c < b->cells->len; c++) {
                const OnText *cell = g_ptr_array_index(b->cells, c);
                on_bnbf_write_table_cell(&S.w, cell->text->str,
                                         cell->text->len);
            }
            break;
        default:                     /* PARA, H1, H2, CODE                  */
            sv_runs(&S, b->text, para);
            break;
        }
        if (i + 1 < d->blocks->len) {
            sv_text(&S, para | b->eol_flags, "\n", 1);
        } else if (para != 0 && b->text != NULL &&
                   b->text->text->len == 0) {
            /* The last line has no newline to carry its style, and an
             * EMPTY heading or code line has no run either: an empty
             * TEXT record under the paragraph flag records the kind.
             * (Bullet and number lines write their prefix, a task line
             * its CHECK record, so only H1/H2/CODE need this.)          */
            sv_flush(&S);
            on_bnbf_write_text(&S.w, para, "", 0);
        }
    }
    sv_flush(&S);
    g_string_free(S.pend, TRUE);
    return on_bnbf_writer_finish(&S.w, out_len);
}


/* ===========================================================================
 * OPERATIONS AND UNDO
 *
 * One implementation per operation: apply_op() performs an Op and returns
 * its INVERSE.  A public operation builds an Op, applies it and keeps the
 * inverse in the undo log; undo applies logged inverses and keeps what
 * they return for redo.  So undo, redo and the operation itself are the
 * same code path, and an operation cannot have an undo that differs from
 * its effect.
 * ======================================================================== */

typedef enum {
    OP_INSERT_TEXT,                  /* frag at pos                          */
    OP_DELETE_TEXT,                  /* n bytes at pos                       */
    OP_SET_FLAGS,                    /* mask on/off over n bytes at pos      */
    OP_RESTORE_RUNS,                 /* runs over n bytes at pos := saved    */
    OP_SPLIT,                        /* block pos.block at pos.offset        */
    OP_JOIN,                         /* block pos.block += the next          */
    OP_SET_KIND,
    OP_SET_CHECKED,
    OP_SET_EOL,
    OP_INSERT_BLOCK,                 /* block at pos.block                   */
    OP_REMOVE_BLOCK,
    OP_TABLE_HEADER,
    OP_IMAGE_WIDTH,                  /* image `at` (-1 = the block) := flags */
    OP_TABLE_INSERT_ROW,             /* at `at`, cells or fresh              */
    OP_TABLE_REMOVE_ROW,
    OP_TABLE_INSERT_COL,
    OP_TABLE_REMOVE_COL,
} OpKind;

struct Op {
    OpKind      kind;
    OnPos       pos;                 /* where; block ops use pos.block       */
    gsize       n;                   /* byte count for the span ops          */
    OnText     *frag;                /* INSERT_TEXT: what to insert (owned)  */
    GArray     *runs;                /* RESTORE_RUNS: OnRun tiling n bytes   */
    guint32     flags;               /* SET_FLAGS mask; SET_EOL / SPLIT eol  */
    gboolean    on;                  /* SET_FLAGS / SET_CHECKED / HEADER;
                                        SPLIT: an exact restore (a join's
                                        undo): `uid` is the tail's, 0 or
                                        not, and the head keeps its own     */
    OnBlockKind block_kind;          /* SET_KIND; SPLIT: the second block's  */
    gboolean    checked;             /* SPLIT: the second block's            */
    gint64      uid;                 /* SPLIT: the second block's            */
    OnBlock    *block;               /* INSERT_BLOCK (owned until applied)   */
    gint        at;                  /* table row / column                   */
    GPtrArray  *cells;               /* TABLE_INSERT_*: OnText* (owned)      */
};

static Op *
op_new(OpKind kind)
{
    Op *op = g_new0(Op, 1);
    op->kind = kind;
    op->pos.cell = -1;
    return op;
}

static void
op_free(Op *op)
{
    if (op == NULL)
        return;
    on_text_free(op->frag);
    if (op->runs != NULL)
        g_array_free(op->runs, TRUE);
    on_block_free(op->block);
    if (op->cells != NULL)
        g_ptr_array_free(op->cells, TRUE);
    g_free(op);
}

static void
group_free(GPtrArray *group)
{
    g_ptr_array_free(group, TRUE);
}

/* ---------------------------------------------------------------------------
 * Fragments — a span of an OnText as an OnText of its own, so a deletion
 * can be undone with its runs and inline images intact.
 * ------------------------------------------------------------------------- */

/* text_slice() — a copy of [off, off+n).                                    */
static OnText *
text_slice(const OnText *t, gsize off, gsize n)
{
    OnText *f = on_text_new();
    g_string_append_len(f->text, t->text->str + off, (gssize)n);
    gsize at = 0;                    /* start of run i                      */
    for (guint i = 0; i < t->runs->len && at < off + n; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        gsize s = MAX(at, off), e = MIN(at + r->len, off + n);
        if (e > s) {
            OnRun piece = { e - s, r->flags };
            g_array_append_val(f->runs, piece);
        }
        at += r->len;
    }
    for (guint k = 0; k < t->objects->len; k++) {
        const OnInlineObject *obj = &g_array_index(t->objects, OnInlineObject, k);
        if (obj->offset >= off && obj->offset < off + n) {
            OnInlineObject dup = inline_object_dup(obj, obj->offset - off);
            g_array_append_val(f->objects, dup);
        }
    }
    return f;
}

/* text_paste() — insert fragment `f` at `off`.                              */
static void
text_paste(OnText *t, gsize off, const OnText *f)
{
    gsize at = 0;                    /* bytes of f inserted so far          */
    for (guint i = 0; i < f->runs->len; i++) {
        const OnRun *r = &g_array_index(f->runs, OnRun, i);
        on_text_insert(t, off + at, f->text->str + at, r->len, r->flags);
        at += r->len;
    }
    for (guint k = 0; k < f->objects->len; k++) {
        const OnInlineObject *obj = &g_array_index(f->objects, OnInlineObject, k);
        text_insert_object(t, inline_object_dup(obj, off + obj->offset));
    }
}

/* runs_slice() — the run tiling of [off, off+n), as its own array.          */
static GArray *
runs_slice(const OnText *t, gsize off, gsize n)
{
    OnText *f = text_slice(t, off, n);
    GArray *runs = f->runs;          /* keep the runs, drop the rest        */
    f->runs = g_array_new(FALSE, FALSE, sizeof(OnRun));
    on_text_free(f);
    return runs;
}

/* runs_replace() — retile [off, off+n) with `runs` (which must sum to n).   */
static void
runs_replace(OnText *t, gsize off, gsize n, const GArray *runs)
{
    guint first = runs_split_at(t, off);
    guint last  = runs_split_at(t, off + n);
    g_array_remove_range(t->runs, first, last - first);
    g_array_insert_vals(t->runs, first, runs->data, runs->len);
    runs_normalize(t);
}

/* runs_carry() — does any run in `runs` carry `bit`?                        */
static gboolean
runs_carry(const GArray *runs, guint32 bit)
{
    for (guint i = 0; i < runs->len; i++)
        if (g_array_index(runs, OnRun, i).flags & bit)
            return TRUE;
    return FALSE;
}

gboolean
on_block_is_action(const OnBlock *b)
{
    /* Only the kinds with NO line prefix: in a bullet, numbered or task
     * line the first character is the prefix, so a '!' after it is text;
     * a code line is never an item.  THE definition — the task rows are
     * synced through this very test (serialize.c), so the ord the table
     * names is the ord the model rewrites.                               */
    return b->text != NULL &&
           (b->kind == ON_BLOCK_PARA || b->kind == ON_BLOCK_H1 ||
            b->kind == ON_BLOCK_H2) &&
           b->text->text->len > 0 && b->text->text->str[0] == '!';
}

/* ---------------------------------------------------------------------------
 * action_rest_real() — does an action line's rest-of-line text hold a
 * REAL item?  Bare "!" lines and lines that are nothing but a "due <date>"
 * suffix do not count.
 * ------------------------------------------------------------------------- */
static gboolean
action_rest_real(const gchar *rest)
{
    gchar *t = g_strdup(rest);
    gsize  due_start;                /* where the text part ends            */
    gint64 due;
    if (on_action_split_due(t, &due_start, &due))
        t[due_start] = '\0';
    gboolean real = *g_strstrip(t) != '\0';
    g_free(t);
    return real;
}

/* ---------------------------------------------------------------------------
 * block_is_real_action() — an action line that IS an item: on_block_is_action
 * plus a real rest-of-line.  THE one test the action listing, the saver
 * (which writes the ACTION record only for these) and the loader's stray
 * count share, so a uid is written exactly where the listing counts an
 * item.
 * ------------------------------------------------------------------------- */
static gboolean
block_is_real_action(const OnBlock *b)
{
    return on_block_is_action(b) && action_rest_real(b->text->text->str + 1);
}

OnText *
on_document_text_at(const OnDocument *d, OnPos pos)
{
    OnBlock *b = on_document_block(d, pos.block);
    if (b == NULL)
        return NULL;
    if (pos.cell < 0)
        return b->text;
    if (b->kind != ON_BLOCK_TABLE || pos.cell >= b->rows * b->cols)
        return NULL;
    return g_ptr_array_index(b->cells, pos.cell);
}

/* ---------------------------------------------------------------------------
 * Observer notifications.
 * ------------------------------------------------------------------------- */
static void
notify_changed(OnDocument *d, guint i)
{
    if (d->observer.block_changed != NULL)
        d->observer.block_changed(d, i, d->observer_data);
}

static void
notify_inserted(OnDocument *d, guint i, guint n)
{
    if (d->observer.blocks_inserted != NULL)
        d->observer.blocks_inserted(d, i, n, d->observer_data);
}

static void
notify_removed(OnDocument *d, guint i, guint n)
{
    if (d->observer.blocks_removed != NULL)
        d->observer.blocks_removed(d, i, n, d->observer_data);
}

/* ---------------------------------------------------------------------------
 * table_cells_take_row/col() / table_cells_put_row/col() — the four table
 * reshapes on the flat row-major cell array.  `take` removes the cells and
 * returns them (owned), `put` inserts them (or fresh empty ones for NULL).
 * ------------------------------------------------------------------------- */
static GPtrArray *
table_take_row(OnBlock *b, gint at)
{
    GPtrArray *cells = g_ptr_array_new_with_free_func(
        (GDestroyNotify)on_text_free);
    for (gint c = 0; c < b->cols; c++)
        g_ptr_array_add(cells, g_ptr_array_steal_index(b->cells, at * b->cols));
    b->rows--;
    return cells;
}

static void
table_put_row(OnBlock *b, gint at, GPtrArray *cells)
{
    for (gint c = 0; c < b->cols; c++)
        g_ptr_array_insert(b->cells, at * b->cols + c,
                           cells != NULL ? g_ptr_array_steal_index(cells, 0)
                                         : on_text_new());
    b->rows++;
}

static GPtrArray *
table_take_col(OnBlock *b, gint at)
{
    GPtrArray *cells = g_ptr_array_new_with_free_func(
        (GDestroyNotify)on_text_free);
    for (gint r = 0; r < b->rows; r++)
        g_ptr_array_add(cells,
                        g_ptr_array_steal_index(b->cells,
                                                r * (b->cols - 1) + at));
    b->cols--;
    return cells;
}

static void
table_put_col(OnBlock *b, gint at, GPtrArray *cells)
{
    for (gint r = 0; r < b->rows; r++)
        g_ptr_array_insert(b->cells, r * (b->cols + 1) + at,
                           cells != NULL ? g_ptr_array_steal_index(cells, 0)
                                         : on_text_new());
    b->cols++;
}

/* ---------------------------------------------------------------------------
 * apply_op() — perform `op` on the document, update the change flags and
 * notify, and return the inverse.  The op is assumed valid (the public
 * entry points check); ownership of op->block / op->cells passes to the
 * document where they are inserted.
 * ------------------------------------------------------------------------- */
static Op *
apply_op(OnDocument *d, Op *op)
{
    Op *inv = NULL;
    guint i = op->pos.block;
    OnBlock *b = on_document_block(d, i);
    OnText  *t = (b != NULL) ? on_document_text_at(d, op->pos) : NULL;
    gboolean was_action = (b != NULL) && on_block_is_action(b);

    switch (op->kind) {
    case OP_INSERT_TEXT:
        text_paste(t, op->pos.offset, op->frag);
        if (runs_carry(op->frag->runs, ON_FMT_TAG))
            d->tags_modified = TRUE;
        inv = op_new(OP_DELETE_TEXT);
        inv->pos = op->pos;
        inv->n   = op->frag->text->len;
        notify_changed(d, i);
        break;

    case OP_DELETE_TEXT:
        inv = op_new(OP_INSERT_TEXT);
        inv->pos  = op->pos;
        inv->frag = text_slice(t, op->pos.offset, op->n);
        if (runs_carry(inv->frag->runs, ON_FMT_TAG))
            d->tags_modified = TRUE;
        on_text_delete(t, op->pos.offset, op->n);
        notify_changed(d, i);
        break;

    case OP_SET_FLAGS:
        inv = op_new(OP_RESTORE_RUNS);
        inv->pos  = op->pos;
        inv->n    = op->n;
        inv->runs = runs_slice(t, op->pos.offset, op->n);
        if ((op->flags & ON_FMT_TAG) || runs_carry(inv->runs, ON_FMT_TAG))
            d->tags_modified = TRUE;
        on_text_set_flags(t, op->pos.offset, op->n, op->flags, op->on);
        notify_changed(d, i);
        break;

    case OP_RESTORE_RUNS:
        inv = op_new(OP_RESTORE_RUNS);
        inv->pos  = op->pos;
        inv->n    = op->n;
        inv->runs = runs_slice(t, op->pos.offset, op->n);
        if (runs_carry(op->runs, ON_FMT_TAG) ||
            runs_carry(inv->runs, ON_FMT_TAG))
            d->tags_modified = TRUE;
        runs_replace(t, op->pos.offset, op->n, op->runs);
        notify_changed(d, i);
        break;

    case OP_SPLIT: {
        OnBlock *c = on_block_new_text(op->block_kind);
        c->checked    = op->checked;
        c->action_uid = op->uid;
        c->eol_flags  = b->eol_flags;
        gsize off = op->pos.offset;
        /* A cut INSIDE a #tag span makes two tags of one.                  */
        if (off > 0 && off < t->text->len &&
            (on_text_flags_at(t, off - 1) & on_text_flags_at(t, off) &
             ON_FMT_TAG))
            d->tags_modified = TRUE;
        OnText *tail = text_slice(t, off, t->text->len - off);
        on_text_delete(t, off, t->text->len - off);
        text_paste(c->text, 0, tail);
        on_text_free(tail);
        b->eol_flags = op->flags & ON_FMT_RUN_MASK;
        /* The identity follows the TEXT: a split at column 0 (Enter at the
         * start of "! item") leaves the head empty and moves the whole
         * line down, so the uid moves with it; and a redo whose tail is
         * given the uid the head still holds takes it off the head.  Two
         * blocks never share one.                                         */
        if (off == 0 && op->uid == 0 && !op->on) {
            c->action_uid = b->action_uid;
            b->action_uid = 0;
        } else if (c->action_uid != 0 && c->action_uid == b->action_uid) {
            b->action_uid = 0;
        }
        g_ptr_array_insert(d->blocks, (gint)i + 1, c);
        inv = op_new(OP_JOIN);
        inv->pos.block = i;
        d->actions_modified = TRUE;
        notify_changed(d, i);
        notify_inserted(d, i + 1, 1);
        break;
    }

    case OP_JOIN: {
        OnBlock *c = on_document_block(d, i + 1);
        inv = op_new(OP_SPLIT);
        inv->pos.block  = i;
        inv->pos.offset = t->text->len;
        inv->flags      = b->eol_flags;
        inv->block_kind = c->kind;
        inv->checked    = c->checked;
        inv->uid        = c->action_uid;
        inv->on         = TRUE;      /* undo puts both identities back      */
        /* Two #tags meeting at the seam become one.                        */
        if (t->text->len > 0 && c->text->text->len > 0 &&
            (on_text_flags_at(t, t->text->len - 1) &
             on_text_flags_at(c->text, 0) & ON_FMT_TAG))
            d->tags_modified = TRUE;
        text_paste(t, t->text->len, c->text);
        b->eol_flags = c->eol_flags;
        /* The reverse of the column-0 split: an identity-less head takes
         * the tail's, so undoing that Enter gives the line its uid back.  */
        if (b->action_uid == 0)
            b->action_uid = c->action_uid;
        g_ptr_array_remove_index(d->blocks, i + 1);
        d->actions_modified = TRUE;
        notify_changed(d, i);
        notify_removed(d, i + 1, 1);
        break;
    }

    case OP_SET_KIND:
        inv = op_new(OP_SET_KIND);
        inv->pos.block  = i;
        inv->block_kind = b->kind;
        b->kind = op->block_kind;
        notify_changed(d, i);
        break;

    case OP_SET_CHECKED:
        inv = op_new(OP_SET_CHECKED);
        inv->pos.block = i;
        inv->on = b->checked;
        b->checked = op->on;
        notify_changed(d, i);
        break;

    case OP_SET_EOL:
        inv = op_new(OP_SET_EOL);
        inv->pos.block = i;
        inv->flags = b->eol_flags;
        b->eol_flags = op->flags & ON_FMT_RUN_MASK;
        notify_changed(d, i);
        break;

    case OP_INSERT_BLOCK:
        g_ptr_array_insert(d->blocks, (gint)i, op->block);
        if (on_block_is_action(op->block))
            d->actions_modified = TRUE;
        if (op->block->text != NULL &&
            runs_carry(op->block->text->runs, ON_FMT_TAG))
            d->tags_modified = TRUE;
        op->block = NULL;            /* the document owns it now            */
        inv = op_new(OP_REMOVE_BLOCK);
        inv->pos.block = i;
        d->actions_modified = TRUE;
        notify_inserted(d, i, 1);
        break;

    case OP_REMOVE_BLOCK:
        inv = op_new(OP_INSERT_BLOCK);
        inv->pos.block = i;
        inv->block = g_ptr_array_steal_index(d->blocks, i);
        block_forget_pixels(inv->block);
        if (inv->block->text != NULL &&
            runs_carry(inv->block->text->runs, ON_FMT_TAG))
            d->tags_modified = TRUE;
        d->actions_modified = TRUE;
        notify_removed(d, i, 1);
        break;

    case OP_TABLE_HEADER:
        inv = op_new(OP_TABLE_HEADER);
        inv->pos.block = i;
        inv->on = b->header;
        b->header = op->on;
        notify_changed(d, i);
        break;

    case OP_IMAGE_WIDTH: {
        guint32 *slot = (op->at < 0)
            ? &b->display_width
            : &g_array_index(b->text->objects, OnInlineObject, op->at)
                  .display_width;
        inv = op_new(OP_IMAGE_WIDTH);
        inv->pos.block = i;
        inv->at = op->at;
        inv->flags = *slot;
        *slot = op->flags;
        notify_changed(d, i);
        break;
    }

    case OP_TABLE_INSERT_ROW:
        table_put_row(b, op->at, op->cells);
        inv = op_new(OP_TABLE_REMOVE_ROW);
        inv->pos.block = i;
        inv->at = op->at;
        notify_changed(d, i);
        break;

    case OP_TABLE_REMOVE_ROW:
        inv = op_new(OP_TABLE_INSERT_ROW);
        inv->pos.block = i;
        inv->at = op->at;
        inv->cells = table_take_row(b, op->at);
        notify_changed(d, i);
        break;

    case OP_TABLE_INSERT_COL:
        table_put_col(b, op->at, op->cells);
        inv = op_new(OP_TABLE_REMOVE_COL);
        inv->pos.block = i;
        inv->at = op->at;
        notify_changed(d, i);
        break;

    case OP_TABLE_REMOVE_COL:
        inv = op_new(OP_TABLE_INSERT_COL);
        inv->pos.block = i;
        inv->at = op->at;
        inv->cells = table_take_col(b, op->at);
        notify_changed(d, i);
        break;
    }

    /* A block that was, or now is, an action line changed.                 */
    if (b != NULL && i < d->blocks->len &&
        (was_action || on_block_is_action(g_ptr_array_index(d->blocks, i))))
        d->actions_modified = TRUE;

    /* Where the caret belongs after this: the op's own position, or for a
     * removed block the start of what now sits there.                     */
    d->last_change = op->pos;
    if (op->kind == OP_INSERT_TEXT) {
        d->last_change.offset += op->frag->text->len;
    } else if (op->kind == OP_JOIN || op->kind == OP_SPLIT) {
        d->last_change.cell = -1;
        if (op->kind == OP_SPLIT) {
            d->last_change.block = i + 1;
            d->last_change.offset = 0;
        } else {
            d->last_change.offset = inv->pos.offset;
        }
    } else if (op->kind == OP_REMOVE_BLOCK || op->kind == OP_INSERT_BLOCK) {
        d->last_change.cell = -1;
        d->last_change.offset = 0;
        if (d->last_change.block >= d->blocks->len)
            d->last_change.block = d->blocks->len - 1;
    } else if (op->kind != OP_DELETE_TEXT && op->kind != OP_SET_FLAGS &&
               op->kind != OP_RESTORE_RUNS) {
        d->last_change.cell = -1;
        d->last_change.offset = 0;
    }
    return inv;
}

OnPos
on_document_last_change(const OnDocument *d)
{
    return d->last_change;
}

/* How many undo steps a document keeps: past this the oldest goes.  The
 * log otherwise grows for as long as the note is open.                      */
#define UNDO_MAX_STEPS 1000

/* push_undo() — a finished group onto the undo log, keeping it to
 * UNDO_MAX_STEPS.                                                           */
static void
push_undo(OnDocument *d, GPtrArray *group)
{
    g_ptr_array_add(d->undo_stack, group);
    if (d->undo_stack->len > UNDO_MAX_STEPS)
        g_ptr_array_remove_index(d->undo_stack, 0);
}

/* ---------------------------------------------------------------------------
 * do_op() — apply a public operation and log its inverse.
 * ------------------------------------------------------------------------- */
static gboolean
do_op(OnDocument *d, Op *op)
{
    Op *inv = apply_op(d, op);
    op_free(op);
    if (d->open_group == NULL)
        d->open_group = g_ptr_array_new_with_free_func((GDestroyNotify)op_free);
    g_ptr_array_add(d->open_group, inv);
    if (d->group_depth == 0) {
        push_undo(d, d->open_group);
        d->open_group = NULL;
    }
    g_ptr_array_set_size(d->redo_stack, 0);
    return TRUE;
}

/* pos_ok() — a text position: an existing text (block or cell) and an
 * offset on a character boundary within it.                                 */
static OnText *
pos_ok(const OnDocument *d, OnPos pos, gsize n)
{
    OnText *t = on_document_text_at(d, pos);
    if (t == NULL || pos.offset + n > t->text->len)
        return NULL;
    if ((pos.offset < t->text->len &&
         ((guchar)t->text->str[pos.offset] & 0xc0) == 0x80) ||
        (pos.offset + n < t->text->len &&
         ((guchar)t->text->str[pos.offset + n] & 0xc0) == 0x80))
        return NULL;                 /* inside a character                  */
    return t;
}

gboolean
on_document_insert_text(OnDocument *d, OnPos pos, const gchar *s, gsize n,
                        guint32 flags)
{
    if (pos_ok(d, pos, 0) == NULL || n == 0 ||
        !g_utf8_validate_len(s, (gssize)n, NULL) ||
        (pos.cell < 0 && on_document_text_has_line_break(s, n)) ||
        g_strstr_len(s, (gssize)n, ON_OBJ_CHAR) != NULL)
        return FALSE;
    Op *op = op_new(OP_INSERT_TEXT);
    op->pos  = pos;
    op->frag = on_text_new();
    on_text_append(op->frag, s, n, flags);
    return do_op(d, op);
}

gboolean
on_document_delete_text(OnDocument *d, OnPos pos, gsize n)
{
    if (pos_ok(d, pos, n) == NULL || n == 0)
        return FALSE;
    Op *op = op_new(OP_DELETE_TEXT);
    op->pos = pos;
    op->n   = n;
    return do_op(d, op);
}

gboolean
on_document_set_flags(OnDocument *d, OnPos pos, gsize n, guint32 mask,
                      gboolean on)
{
    if (pos_ok(d, pos, n) == NULL || n == 0 ||
        (mask & ON_FMT_RUN_MASK) == 0)
        return FALSE;
    Op *op = op_new(OP_SET_FLAGS);
    op->pos   = pos;
    op->n     = n;
    op->flags = mask & ON_FMT_RUN_MASK;
    op->on    = on;
    return do_op(d, op);
}

gboolean
on_document_split_block(OnDocument *d, OnPos pos, guint32 eol_flags)
{
    OnBlock *b = on_document_block(d, pos.block);
    if (b == NULL || pos.cell >= 0 || !on_block_kind_is_text(b->kind) ||
        pos_ok(d, pos, 0) == NULL)
        return FALSE;
    Op *op = op_new(OP_SPLIT);
    op->pos        = pos;
    op->flags      = eol_flags;
    op->block_kind = b->kind;
    return do_op(d, op);
}

gboolean
on_document_join_blocks(OnDocument *d, guint i)
{
    OnBlock *b = on_document_block(d, i);
    OnBlock *c = on_document_block(d, i + 1);
    if (b == NULL || c == NULL || !on_block_kind_is_text(b->kind) ||
        !on_block_kind_is_text(c->kind))
        return FALSE;
    Op *op = op_new(OP_JOIN);
    op->pos.block = i;
    return do_op(d, op);
}

gboolean
on_document_set_kind(OnDocument *d, guint i, OnBlockKind kind)
{
    OnBlock *b = on_document_block(d, i);
    if (b == NULL || !on_block_kind_is_text(b->kind) ||
        !on_block_kind_is_text(kind))
        return FALSE;
    if (b->kind == kind)
        return TRUE;
    Op *op = op_new(OP_SET_KIND);
    op->pos.block  = i;
    op->block_kind = kind;
    return do_op(d, op);
}

gboolean
on_document_set_checked(OnDocument *d, guint i, gboolean checked)
{
    OnBlock *b = on_document_block(d, i);
    if (b == NULL || b->kind != ON_BLOCK_CHECK)
        return FALSE;
    if (b->checked == !!checked)
        return TRUE;
    Op *op = op_new(OP_SET_CHECKED);
    op->pos.block = i;
    op->on = !!checked;
    return do_op(d, op);
}

gboolean
on_document_set_eol_flags(OnDocument *d, guint i, guint32 flags)
{
    OnBlock *b = on_document_block(d, i);
    if (b == NULL)
        return FALSE;
    if (b->eol_flags == (flags & ON_FMT_RUN_MASK))
        return TRUE;
    Op *op = op_new(OP_SET_EOL);
    op->pos.block = i;
    op->flags = flags;
    return do_op(d, op);
}

gboolean
on_document_insert_block(OnDocument *d, guint i, OnBlock *block)
{
    if (block == NULL || i > d->blocks->len)
        return FALSE;
    Op *op = op_new(OP_INSERT_BLOCK);
    op->pos.block = i;
    op->block = block;
    return do_op(d, op);
}

gboolean
on_document_remove_block(OnDocument *d, guint i)
{
    if (i >= d->blocks->len || d->blocks->len == 1)
        return FALSE;
    Op *op = op_new(OP_REMOVE_BLOCK);
    op->pos.block = i;
    return do_op(d, op);
}

gboolean
on_document_set_image_width(OnDocument *d, guint i, gint inline_ord,
                            guint32 width)
{
    OnBlock *b = on_document_block(d, i);
    if (b == NULL)
        return FALSE;
    if (inline_ord < 0) {
        if (b->kind != ON_BLOCK_IMAGE)
            return FALSE;
        if (b->display_width == width)
            return TRUE;
    } else {
        if (b->text == NULL || (guint)inline_ord >= b->text->objects->len ||
            g_array_index(b->text->objects, OnInlineObject, inline_ord).kind
                != ON_OBJECT_IMAGE)
            return FALSE;
        if (g_array_index(b->text->objects, OnInlineObject, inline_ord)
                .display_width == width)
            return TRUE;
    }
    Op *op = op_new(OP_IMAGE_WIDTH);
    op->pos.block = i;
    op->at = inline_ord;
    op->flags = width;
    return do_op(d, op);
}

/* table_ok() — block i is a table.                                          */
static OnBlock *
table_ok(const OnDocument *d, guint i)
{
    OnBlock *b = on_document_block(d, i);
    return (b != NULL && b->kind == ON_BLOCK_TABLE) ? b : NULL;
}

gboolean
on_document_table_set_header(OnDocument *d, guint i, gboolean header)
{
    OnBlock *b = table_ok(d, i);
    if (b == NULL)
        return FALSE;
    if (b->header == !!header)
        return TRUE;
    Op *op = op_new(OP_TABLE_HEADER);
    op->pos.block = i;
    op->on = !!header;
    return do_op(d, op);
}

/* table_reshape() — the shared entry of the four reshapes.                  */
static gboolean
table_reshape(OnDocument *d, guint i, OpKind kind, gint at)
{
    OnBlock *b = table_ok(d, i);
    if (b == NULL)
        return FALSE;
    gint count = (kind == OP_TABLE_INSERT_ROW || kind == OP_TABLE_REMOVE_ROW)
                 ? b->rows : b->cols;
    gboolean insert = kind == OP_TABLE_INSERT_ROW ||
                      kind == OP_TABLE_INSERT_COL;
    if (at < 0 || (insert ? at > count : (at >= count || count == 1)))
        return FALSE;
    Op *op = op_new(kind);
    op->pos.block = i;
    op->at = at;
    return do_op(d, op);
}

gboolean
on_document_table_insert_row(OnDocument *d, guint i, gint at)
{
    return table_reshape(d, i, OP_TABLE_INSERT_ROW, at);
}

gboolean
on_document_table_remove_row(OnDocument *d, guint i, gint at)
{
    return table_reshape(d, i, OP_TABLE_REMOVE_ROW, at);
}

gboolean
on_document_table_insert_col(OnDocument *d, guint i, gint at)
{
    return table_reshape(d, i, OP_TABLE_INSERT_COL, at);
}

gboolean
on_document_table_remove_col(OnDocument *d, guint i, gint at)
{
    return table_reshape(d, i, OP_TABLE_REMOVE_COL, at);
}

/* ===========================================================================
 * RANGES AND FRAGMENTS
 * ======================================================================== */

gint
on_pos_cmp(OnPos a, OnPos b)
{
    if (a.block != b.block)
        return a.block < b.block ? -1 : 1;
    if (a.cell != b.cell)
        return a.cell < b.cell ? -1 : 1;
    if (a.offset != b.offset)
        return a.offset < b.offset ? -1 : 1;
    return 0;
}

/* pos_text_ok() — a valid text position (block text or cell).              */
static gboolean
pos_text_ok(const OnDocument *d, OnPos p)
{
    return pos_ok(d, p, 0) != NULL;
}

/* pos_valid() — a valid position of any kind: text, or an object block's
 * 0 / 1.                                                                    */
static gboolean
pos_valid(const OnDocument *d, OnPos p)
{
    OnBlock *b = on_document_block(d, p.block);
    if (b == NULL)
        return FALSE;
    if (b->kind == ON_BLOCK_IMAGE || (b->kind == ON_BLOCK_TABLE && p.cell < 0))
        return p.offset <= 1;
    return pos_text_ok(d, p);
}

/* pos_before_table() — does a range END here reach no further than a
 * table's leading edge?  Beside the table at offset 0, or at the very
 * start of its first cell (where Down from the line above lands): the
 * table is not part of such a range.  Anywhere else in a cell is inside. */
static gboolean
pos_before_table(OnPos p)
{
    return p.offset == 0 && (p.cell < 0 || p.cell == 0);
}

OnDocument *
on_document_copy_range(const OnDocument *d, OnPos a, OnPos b)
{
    OnDocument *out = doc_alloc();
    if (on_pos_cmp(a, b) > 0) {
        OnPos t = a; a = b; b = t;
    }
    if (!pos_valid(d, a) || !pos_valid(d, b)) {
        g_ptr_array_add(out->blocks, on_block_new_text(ON_BLOCK_PARA));
        return out;
    }

    /* Inside one table: a cell's text, or the whole table.                 */
    OnBlock *first = on_document_block(d, a.block);
    if (a.block == b.block && first->kind == ON_BLOCK_TABLE) {
        if (a.cell == b.cell && a.cell >= 0) {
            OnBlock *p = on_block_new_text(ON_BLOCK_PARA);
            OnText *cell = g_ptr_array_index(first->cells, a.cell);
            OnText *part = text_slice(cell, a.offset, b.offset - a.offset);
            text_paste(p->text, 0, part);
            on_text_free(part);
            g_ptr_array_add(out->blocks, p);
        } else {
            OnBlock *c = on_block_copy(first);
            c->action_uid = 0;
            g_ptr_array_add(out->blocks, c);
        }
        return out;
    }

    for (guint i = a.block; i <= b.block; i++) {
        OnBlock *src = on_document_block(d, i);
        OnBlock *c;
        if (on_block_kind_is_text(src->kind)) {
            gsize from = (i == a.block) ? a.offset : 0;
            gsize to   = (i == b.block) ? b.offset : src->text->text->len;
            c = on_block_new_text(src->kind);
            c->checked   = src->checked;
            c->eol_flags = src->eol_flags;
            OnText *part = text_slice(src->text, from, to - from);
            text_paste(c->text, 0, part);
            on_text_free(part);
        } else {
            /* An object block on the range's edge is taken only when the
             * range reaches across it — for a table, past its leading
             * edge (pos_before_table): the same rule
             * on_document_delete_range applies, so a Cut across a table
             * edge puts on the clipboard exactly what it removes.          */
            if ((i == a.block && a.cell < 0 && a.offset == 1) ||
                (i == b.block && pos_before_table(b)))
                continue;
            c = on_block_copy(src);
            c->action_uid = 0;
        }
        g_ptr_array_add(out->blocks, c);
    }
    if (out->blocks->len == 0)
        g_ptr_array_add(out->blocks, on_block_new_text(ON_BLOCK_PARA));
    return out;
}

/* remove_block_keeping_one() — remove block i, replacing it with an empty
 * PARA when it is the document's last block.                                */
static void
remove_block_keeping_one(OnDocument *d, guint i)
{
    if (d->blocks->len == 1)
        on_document_insert_block(d, 1, on_block_new_text(ON_BLOCK_PARA));
    on_document_remove_block(d, i);
}

gboolean
on_document_delete_range(OnDocument *d, OnPos a, OnPos b, OnPos *out)
{
    if (on_pos_cmp(a, b) > 0) {
        OnPos t = a; a = b; b = t;
    }
    if (on_pos_cmp(a, b) == 0 || !pos_valid(d, a) || !pos_valid(d, b))
        return FALSE;
    OnBlock *first = on_document_block(d, a.block);

    on_document_begin_group(d);
    OnPos after = a;                 /* where the caret lands               */

    if (a.block == b.block) {
        if (on_block_kind_is_text(first->kind) ||
            (first->kind == ON_BLOCK_TABLE && a.cell >= 0 && a.cell == b.cell)) {
            on_document_delete_text(d, a, b.offset - a.offset);
        } else {
            /* An object block, or a cell range inside a table: the block. */
            remove_block_keeping_one(d, a.block);
            after.cell   = -1;
            after.offset = 0;
            if (after.block >= d->blocks->len)
                after.block = d->blocks->len - 1;
            /* Land at the END of a text block before, if that is where the
             * removal left us.                                             */
            OnBlock *nb = on_document_block(d, after.block);
            if (after.block == a.block && a.block > 0 && nb != NULL &&
                !on_block_kind_is_text(nb->kind)) {
                after.block--;
                nb = on_document_block(d, after.block);
                if (nb->text != NULL)
                    after.offset = nb->text->text->len;
                else
                    after.offset = 1;
            }
        }
        on_document_end_group(d);
        if (out != NULL)
            *out = after;
        return TRUE;
    }

    /* Several blocks.  Trim the last, drop the middle, trim the first,
     * then join the two edges when both are text.                          */
    OnBlock *last = on_document_block(d, b.block);
    gboolean last_text = on_block_kind_is_text(last->kind);
    gboolean drop_last = !last_text && b.offset == 1;
    if (last_text && b.offset > 0) {
        OnPos p = { b.block, -1, 0 };
        on_document_delete_text(d, p, b.offset);
    } else if (last->kind == ON_BLOCK_TABLE && !pos_before_table(b)) {
        drop_last = TRUE;            /* a range ending inside a table takes
                                        the table (pos_before_table is the
                                        one reading of "inside", shared
                                        with on_document_copy_range)       */
    }
    for (guint i = b.block - 1; i > a.block; i--)
        on_document_remove_block(d, i);
    if (drop_last)
        on_document_remove_block(d, a.block + 1);

    gboolean first_text = on_block_kind_is_text(first->kind);
    gboolean drop_first = !first_text && (a.offset == 0 || a.cell >= 0);
    if (first_text && a.offset < first->text->text->len)  /* no cells */
        on_document_delete_text(d, a, first->text->text->len - a.offset);
    if (drop_first) {
        remove_block_keeping_one(d, a.block);
        after.cell   = -1;
        after.offset = 0;
        if (after.block >= d->blocks->len)
            after.block = d->blocks->len - 1;
    } else if (first_text && !drop_last &&
               a.block + 1 < d->blocks->len &&
               on_block_kind_is_text(on_document_block(d, a.block + 1)->kind)) {
        on_document_join_blocks(d, a.block);
    }
    on_document_end_group(d);
    if (out != NULL)
        *out = after;
    return TRUE;
}

gboolean
on_document_insert_fragment(OnDocument *d, OnPos pos, const OnDocument *frag,
                            OnPos *out)
{
    OnText *t = pos_ok(d, pos, 0);
    if (t == NULL || frag->blocks->len == 0)
        return FALSE;
    OnBlock *host = on_document_block(d, pos.block);
    guint n = frag->blocks->len;
    OnPos end = pos;                 /* just after the inserted content     */

    on_document_begin_group(d);
    if (pos.cell >= 0) {
        /* A cell takes the fragment's plain text, lines joined by spaces. */
        gchar *plain = on_document_plain_text(frag);
        for (gchar *p = plain; *p != '\0'; p++)
            if (*p == '\n')
                *p = ' ';
        on_document_insert_text(d, pos, plain, strlen(plain), 0);
        end.offset += strlen(plain);
        g_free(plain);
        on_document_end_group(d);
        if (out != NULL)
            *out = end;
        return TRUE;
    }

    const OnBlock *f0 = on_document_block(frag, 0);
    if (n == 1 && on_block_kind_is_text(f0->kind)) {
        Op *op = op_new(OP_INSERT_TEXT);
        op->pos  = pos;
        op->frag = on_text_copy(f0->text);
        do_op(d, op);
        end.offset += f0->text->text->len;
        on_document_end_group(d);
        if (out != NULL)
            *out = end;
        return TRUE;
    }

    /* An EMPTY host block facing an object: the fragment goes in front of
     * it, whole, and the empty block stays as the line after — no split,
     * which would leave two empty lines around the object.                */
    if (t->text->len == 0 && !on_block_kind_is_text(f0->kind)) {
        guint at = pos.block;
        for (guint k = 0; k < n; k++) {
            const OnBlock *fk = on_document_block(frag, k);
            if (k == n - 1 && on_block_kind_is_text(fk->kind)) {
                /* Its last text block becomes the empty host's content.   */
                OnPos hp = { at, -1, 0 };
                Op *op = op_new(OP_INSERT_TEXT);
                op->pos  = hp;
                op->frag = on_text_copy(fk->text);
                do_op(d, op);
                if (fk->kind != ON_BLOCK_PARA)
                    on_document_set_kind(d, at, fk->kind);
                end.block  = at;
                end.offset = fk->text->text->len;
            } else {
                OnBlock *c = on_block_copy(fk);
                c->action_uid = 0;
                on_document_insert_block(d, at, c);
                end.block  = at;
                end.cell   = -1;
                end.offset = 1;
                at++;
            }
        }
        on_document_end_group(d);
        if (out != NULL)
            *out = end;
        return TRUE;
    }

    /* Split the host at pos; the fragment's first text block joins the
     * head, its last text block the tail, everything else lands between. */
    on_document_split_block(d, pos, on_text_flags_at(t, pos.offset));
    guint at = pos.block + 1;        /* where the next block is inserted    */
    guint first = 0, last = n;       /* fragment blocks placed as blocks    */
    if (on_block_kind_is_text(f0->kind)) {
        Op *op = op_new(OP_INSERT_TEXT);
        op->pos  = pos;
        op->frag = on_text_copy(f0->text);
        do_op(d, op);
        first = 1;
    }
    const OnBlock *fl = on_document_block(frag, n - 1);
    if (n > 1 && on_block_kind_is_text(fl->kind)) {
        OnPos tail = { pos.block + 1, -1, 0 };
        Op *op = op_new(OP_INSERT_TEXT);
        op->pos  = tail;
        op->frag = on_text_copy(fl->text);
        do_op(d, op);
        /* The tail keeps the host's kind unless the host is plain and the
         * fragment says otherwise.                                         */
        if (host->kind == ON_BLOCK_PARA && fl->kind != ON_BLOCK_PARA)
            on_document_set_kind(d, pos.block + 1, fl->kind);
        end.block  = pos.block + 1;
        end.offset = fl->text->text->len;
        last = n - 1;
    }
    for (guint k = first; k < last; k++) {
        OnBlock *c = on_block_copy(on_document_block(frag, k));
        c->action_uid = 0;
        on_document_insert_block(d, at++, c);
        end.block++;
    }
    if (!on_block_kind_is_text(fl->kind)) {
        /* Ends on an object: the caret goes after it, before the tail.    */
        end.block  = at - 1;
        end.offset = 1;
        end.cell   = -1;
    }
    on_document_end_group(d);
    if (out != NULL)
        *out = end;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * Undo log.
 * ------------------------------------------------------------------------- */

void
on_document_begin_group(OnDocument *d)
{
    d->group_depth++;
}

void
on_document_end_group(OnDocument *d)
{
    if (d->group_depth == 0)
        return;
    if (--d->group_depth == 0 && d->open_group != NULL) {
        push_undo(d, d->open_group);
        d->open_group = NULL;
    }
}

/* step() — pop the last group of `from`, apply its ops newest first, and
 * push the inverses (a group again) onto `to`.                              */
static gboolean
step(OnDocument *d, GPtrArray *from, GPtrArray *to)
{
    if (from->len == 0)
        return FALSE;
    GPtrArray *group = g_ptr_array_steal_index(from, from->len - 1);
    GPtrArray *back  = g_ptr_array_new_with_free_func((GDestroyNotify)op_free);
    for (guint k = group->len; k > 0; k--)
        g_ptr_array_add(back, apply_op(d, g_ptr_array_index(group, k - 1)));
    group_free(group);
    g_ptr_array_add(to, back);
    return TRUE;
}

gboolean
on_document_undo(OnDocument *d)
{
    d->group_depth = 0;              /* an open typing group ends here      */
    on_document_end_group(d);
    if (d->open_group != NULL) {     /* depth was already 0: flush anyway  */
        push_undo(d, d->open_group);
        d->open_group = NULL;
    }
    return step(d, d->undo_stack, d->redo_stack);
}

gboolean
on_document_redo(OnDocument *d)
{
    return step(d, d->redo_stack, d->undo_stack);
}

gboolean
on_document_can_undo(const OnDocument *d)
{
    return d->undo_stack->len > 0 || d->open_group != NULL;
}

gboolean
on_document_can_redo(const OnDocument *d)
{
    return d->redo_stack->len > 0;
}

guint
on_document_undo_depth(const OnDocument *d)
{
    return d->undo_stack->len + (d->open_group != NULL ? 1 : 0);
}

void
on_document_clear_undo(OnDocument *d)
{
    if (d->open_group != NULL) {
        group_free(d->open_group);
        d->open_group = NULL;
    }
    d->group_depth = 0;
    g_ptr_array_set_size(d->undo_stack, 0);
    g_ptr_array_set_size(d->redo_stack, 0);
}

/* file_ids_of_text() — append the ids of the attached files in a text.     */
static void
file_ids_of_text(const OnText *t, GArray *ids)
{
    for (guint k = 0; t != NULL && k < t->objects->len; k++) {
        const OnInlineObject *obj = &g_array_index(t->objects, OnInlineObject, k);
        if (obj->kind == ON_OBJECT_FILE)
            g_array_append_val(ids, obj->file_id);
    }
}

/* file_ids_of_group() — append the ids of the files the ops of one undo
 * group hold: in the text they would insert back, or the block they would
 * insert back.                                                              */
static void
file_ids_of_group(GPtrArray *group, GArray *ids)
{
    for (guint i = 0; group != NULL && i < group->len; i++) {
        const Op *op = g_ptr_array_index(group, i);
        file_ids_of_text(op->frag, ids);
        if (op->block != NULL)
            file_ids_of_text(op->block->text, ids);
    }
}

GArray *
on_document_file_ids(const OnDocument *d, gboolean with_history)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (guint i = 0; i < d->blocks->len; i++)
        file_ids_of_text(((const OnBlock *)g_ptr_array_index(d->blocks, i))->text, ids);
    if (with_history) {
        for (guint i = 0; i < d->undo_stack->len; i++)
            file_ids_of_group(g_ptr_array_index(d->undo_stack, i), ids);
        for (guint i = 0; i < d->redo_stack->len; i++)
            file_ids_of_group(g_ptr_array_index(d->redo_stack, i), ids);
        file_ids_of_group(d->open_group, ids);
    }
    return ids;
}

void
on_document_remap_files(OnDocument *d, gint64 (*map)(gint64 id, gpointer data),
                        gpointer data)
{
    for (guint i = 0; i < d->blocks->len; i++) {
        OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->text == NULL)
            continue;
        for (guint k = b->text->objects->len; k > 0; k--) {
            OnInlineObject *obj = &g_array_index(b->text->objects, OnInlineObject, k - 1);
            if (obj->kind != ON_OBJECT_FILE)
                continue;
            gint64 id = map(obj->file_id, data);
            if (id > 0) {
                obj->file_id = id;
            } else {
                OnPos at = { i, -1, obj->offset };
                on_document_delete_text(d, at, ON_OBJ_CHAR_LEN);
            }
        }
    }
    on_document_clear_undo(d);
}

void
on_document_set_observer(OnDocument *d, const OnDocumentObserver *obs,
                         gpointer data)
{
    if (obs != NULL)
        d->observer = *obs;
    else
        memset(&d->observer, 0, sizeof d->observer);
    d->observer_data = data;
}

gboolean
on_document_take_tags_modified(OnDocument *d)
{
    gboolean v = d->tags_modified;
    d->tags_modified = FALSE;
    return v;
}

gboolean
on_document_take_actions_modified(OnDocument *d)
{
    gboolean v = d->actions_modified;
    d->actions_modified = FALSE;
    return v;
}

/* ===========================================================================
 * DERIVED READS
 * ======================================================================== */

/* append_text_piece() — on_text_walk callback: the characters, into the
 * GString `data`.                                                           */
static void
append_text_piece(const gchar *s, gsize n, guint32 flags, gpointer data)
{
    (void)flags;
    g_string_append_len(data, s, (gssize)n);
}

/* append_file_name() — on_text_walk callback: an attached file reads as its
 * name (so it is found by search and titles a line it stands alone on); an
 * image reads as nothing.                                                   */
static void
append_file_name(const OnInlineObject *obj, gpointer data)
{
    if (obj->kind == ON_OBJECT_FILE)
        g_string_append(data, obj->file_name);
}

/* append_text_line() — a text's characters minus every U+FFFC, an attached
 * file as its name.                                                         */
static void
append_text_line(GString *out, const OnText *t)
{
    static const OnTextWalk walk = { append_text_piece, append_file_name };
    on_text_walk(t, &walk, out);
}

gchar *
on_document_plain_text(const OnDocument *d)
{
    GString *out = g_string_new(NULL);
    gint number_run = 0;
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        number_run = on_block_number(number_run, b->kind);
        if (i > 0)
            g_string_append_c(out, '\n');
        switch (b->kind) {
        case ON_BLOCK_BULLET:
            g_string_append(out, ON_BULLET_PREFIX);
            append_text_line(out, b->text);
            break;
        case ON_BLOCK_NUMBER:
            g_string_append_printf(out, "%d. ", number_run);
            append_text_line(out, b->text);
            break;
        case ON_BLOCK_IMAGE:
            break;
        case ON_BLOCK_TABLE:
            for (guint c = 0; c < b->cells->len; c++) {
                const OnText *cell = g_ptr_array_index(b->cells, c);
                g_string_append_len(out, cell->text->str,
                                    (gssize)cell->text->len);
                g_string_append_c(out, ' ');
            }
            break;
        default:
            append_text_line(out, b->text);
            break;
        }
    }
    return g_string_free(out, FALSE);
}

/* text_image_count() — how many of a text's inline objects are images.     */
static gint
text_image_count(const OnText *t)
{
    gint n = 0;
    for (guint k = 0; k < t->objects->len; k++)
        if (g_array_index(t->objects, OnInlineObject, k).kind == ON_OBJECT_IMAGE)
            n++;
    return n;
}

gint
on_document_image_count(const OnDocument *d)
{
    gint n = 0;
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->kind == ON_BLOCK_IMAGE)
            n++;
        else if (b->text != NULL)
            n += text_image_count(b->text);
    }
    return n;
}

GBytes *
on_document_image_nth(const OnDocument *d, gint ord, guint32 *display_width)
{
    if (ord < 0)
        return NULL;
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->kind == ON_BLOCK_IMAGE) {
            if (ord-- == 0) {
                if (display_width != NULL)
                    *display_width = b->display_width;
                return b->png;
            }
        } else if (b->text != NULL) {
            for (guint k = 0; k < b->text->objects->len; k++) {
                const OnInlineObject *img =
                    &g_array_index(b->text->objects, OnInlineObject, k);
                if (img->kind != ON_OBJECT_IMAGE || ord-- != 0)
                    continue;
                if (display_width != NULL)
                    *display_width = img->display_width;
                return img->png;
            }
        }
    }
    return NULL;
}

gchar *
on_document_title(const OnDocument *d, const gchar *fallback, glong max_chars)
{
    gchar *title = NULL;             /* the rendered first line             */
    for (guint i = 0; i < d->blocks->len && title == NULL; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->kind == ON_BLOCK_IMAGE || b->kind == ON_BLOCK_TABLE) {
            title = g_strdup("");    /* a line, but no text on it           */
        } else if (b->text->text->len > 0) {
            GString *line = g_string_new(NULL);
            if (b->kind == ON_BLOCK_BULLET)
                g_string_append(line, ON_BULLET_PREFIX);
            else if (b->kind == ON_BLOCK_NUMBER)
                g_string_append(line, "1. ");
            append_text_line(line, b->text);
            title = g_string_free(line, FALSE);
        }
    }
    if (title != NULL)
        g_strstrip(title);
    if (title == NULL || *title == '\0') {
        g_free(title);
        return g_strdup(fallback);
    }
    if (g_utf8_strlen(title, -1) > max_chars) {
        gchar *cut = g_utf8_substring(title, 0, max_chars);
        g_free(title);
        return cut;
    }
    return title;
}

gboolean
on_document_tag_char(gunichar c)
{
    return g_unichar_isalnum(c) || c == '_' || c == '-';
}

/* A #tag in a block's text: the byte span of its consecutive TAG runs.     */
typedef struct {
    gsize start;
    gsize end;
} tag_span_t;

/*
 * tag_spans — the #tags of one block's text.  A tag is the SPAN of
 * consecutive TAG-carrying runs, not one run: runs also split on
 * bold/italic/…, so a style applied inside "#hello" makes three runs of
 * one tag — per run they would have become "h", "el", "lo" and the real
 * tag gone on the next save.
 *
 * Inputs:
 *   t — the block's text
 *
 * Output: a new GArray of tag_span_t in text order; g_array_unref() it.
 */
static GArray *
tag_spans(const OnText *t)
{
    GArray   *spans  = g_array_new(FALSE, FALSE, sizeof(tag_span_t));
    gsize     at     = 0;            /* start of run k                      */
    gsize     span   = 0;            /* start of the open tag span          */
    gboolean  in_tag = FALSE;
    for (guint k = 0; k <= t->runs->len; k++) {
        const OnRun *r = (k < t->runs->len)
            ? &g_array_index(t->runs, OnRun, k) : NULL;
        gboolean tagged = r != NULL && (r->flags & ON_FMT_TAG) != 0;
        if (tagged && !in_tag) {
            span   = at;
            in_tag = TRUE;
        } else if (!tagged && in_tag) {
            tag_span_t s = { span, at };
            g_array_append_val(spans, s);
            in_tag = FALSE;
        }
        if (r != NULL)
            at += r->len;
    }
    return spans;
}

/*
 * tag_span_name — a tag's name: its span's text, whitespace-trimmed,
 * without the leading '#'.
 *
 * Inputs:
 *   t    — the block's text
 *   span — the tag's span in it
 *
 * Output: a new string; NULL when nothing is left.  g_free it.
 */
static gchar *
tag_span_name(const OnText *t, tag_span_t span)
{
    gchar *name = g_strndup(t->text->str + span.start, span.end - span.start);
    g_strstrip(name);
    gchar *bare = g_strdup(*name == '#' ? name + 1 : name);
    g_free(name);
    if (*bare == '\0') {
        g_free(bare);
        return NULL;
    }
    return bare;
}

GList *
on_document_collect_tags(const OnDocument *d)
{
    GList *names = NULL;             /* collected, reversed                 */
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->text == NULL)
            continue;
        GArray *spans = tag_spans(b->text);
        for (guint k = 0; k < spans->len; k++) {
            gchar *bare = tag_span_name(b->text, g_array_index(spans, tag_span_t, k));
            if (bare != NULL &&
                g_list_find_custom(names, bare, (GCompareFunc)g_strcmp0) == NULL)
                names = g_list_prepend(names, bare);
            else
                g_free(bare);
        }
        g_array_unref(spans);
    }
    return g_list_reverse(names);
}

gchar *
on_document_tag_word(const gchar *name)
{
    gchar  **parts = g_strsplit_set(name != NULL ? name : "", " \t\n\r", -1);
    GString *word  = g_string_new(NULL);
    guint    n     = 0;              /* the words that keep a character     */
    for (gchar **p = parts; *p != NULL; p++) {
        for (const gchar *c = *p; *c != '\0'; c = g_utf8_next_char(c)) {
            if (on_document_tag_char(g_utf8_get_char(c))) {
                n++;
                break;
            }
        }
    }
    for (gchar **p = parts; *p != NULL; p++) {
        gboolean first = TRUE;       /* the word's first kept character     */
        for (const gchar *c = *p; *c != '\0'; c = g_utf8_next_char(c)) {
            gunichar u = g_utf8_get_char(c);
            if (!on_document_tag_char(u))
                continue;
            g_string_append_unichar(word, first && n > 1 ? g_unichar_totitle(u) : u);
            first = FALSE;
        }
    }
    g_strfreev(parts);
    if (word->len == 0) {
        g_string_free(word, TRUE);
        return NULL;
    }
    return g_string_free(word, FALSE);
}

/* ---------------------------------------------------------------------------
 * tags_only() — is a text a line of #tags: at least one #tag run, and
 * nothing else but whitespace?  An empty or blank text is not (it is a
 * line left for the body).
 * ------------------------------------------------------------------------- */
static gboolean
tags_only(const OnText *t)
{
    gsize    at  = 0;                /* start of run k                      */
    gboolean tag = FALSE;            /* a #tag run seen                     */
    for (guint k = 0; k < t->runs->len; k++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, k);
        if ((r->flags & ON_FMT_TAG) != 0) {
            tag = TRUE;
        } else {
            for (const gchar *c = t->text->str + at; c < t->text->str + at + r->len;
                 c = g_utf8_next_char(c))
                if (!g_unichar_isspace(g_utf8_get_char(c)))
                    return FALSE;
        }
        at += r->len;
    }
    return tag;
}

gboolean
on_document_append_tag(OnDocument *d, const gchar *word)
{
    if (word == NULL || *word == '\0')
        return FALSE;
    gchar   *folded = g_utf8_casefold(word, -1);
    GList   *have   = on_document_collect_tags(d);
    gboolean found  = FALSE;         /* the note has the tag already        */
    for (GList *l = have; !found && l != NULL; l = l->next) {
        gchar *f = g_utf8_casefold(l->data, -1);
        found = strcmp(f, folded) == 0;
        g_free(f);
    }
    g_list_free_full(have, g_free);
    g_free(folded);
    if (found)
        return FALSE;

    on_document_begin_group(d);
    guint          last = d->blocks->len - 1;
    const OnBlock *b    = g_ptr_array_index(d->blocks, last);
    if (last == 0 || b->kind != ON_BLOCK_PARA || !tags_only(b->text)) {
        on_document_insert_block(d, ++last, on_block_new_text(ON_BLOCK_PARA));
        b = g_ptr_array_index(d->blocks, last);
    }
    OnPos pos = { last, -1, b->text->text->len };
    if (pos.offset > 0 && !g_unichar_isspace(g_utf8_get_char(
            g_utf8_prev_char(b->text->text->str + pos.offset)))) {
        on_document_insert_text(d, pos, " ", 1, 0);
        pos.offset++;
    }
    gchar *token = g_strconcat("#", word, NULL);
    on_document_insert_text(d, pos, token, strlen(token), ON_FMT_TAG);
    g_free(token);
    on_document_end_group(d);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * text_is_blank() — is a text empty or only whitespace?
 * ------------------------------------------------------------------------- */
static gboolean
text_is_blank(const OnText *t)
{
    for (const gchar *c = t->text->str; *c != '\0'; c = g_utf8_next_char(c))
        if (!g_unichar_isspace(g_utf8_get_char(c)))
            return FALSE;
    return TRUE;
}

gboolean
on_document_remove_tag(OnDocument *d, const gchar *word)
{
    if (word == NULL || *word == '\0')
        return FALSE;
    gchar   *folded  = g_utf8_casefold(word, -1);
    gboolean removed = FALSE;

    on_document_begin_group(d);
    /* Last block and last tag first, so the offsets still to be used hold. */
    for (guint i = d->blocks->len; i-- > 0; ) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->text == NULL)
            continue;
        GArray  *spans = tag_spans(b->text);
        gboolean hit   = FALSE;      /* a tag left this block               */
        for (guint k = spans->len; k-- > 0; ) {
            tag_span_t span = g_array_index(spans, tag_span_t, k);
            gchar *bare = tag_span_name(b->text, span);
            gchar *f    = bare != NULL ? g_utf8_casefold(bare, -1) : NULL;
            gboolean match = f != NULL && strcmp(f, folded) == 0;
            g_free(f);
            g_free(bare);
            if (!match)
                continue;

            /* The gap goes with it: the whitespace before the tag, or at a
             * line's start the whitespace after it.                        */
            const gchar *str  = b->text->text->str;
            gsize        from = span.start;
            gsize        to   = span.end;
            while (from > 0 && g_unichar_isspace(g_utf8_get_char(g_utf8_prev_char(str + from))))
                from = (gsize)(g_utf8_prev_char(str + from) - str);
            while (from == 0 && str[to] != '\0' && g_unichar_isspace(g_utf8_get_char(str + to)))
                to = (gsize)(g_utf8_next_char(str + to) - str);
            if (on_document_delete_text(d, (OnPos){ i, -1, from }, to - from))
                hit = removed = TRUE;
        }
        g_array_unref(spans);
        /* A body line that held nothing else goes; the title line stays.   */
        if (hit && i > 0 && d->blocks->len > 1 && b->kind == ON_BLOCK_PARA &&
            text_is_blank(b->text))
            on_document_remove_block(d, i);
    }
    on_document_end_group(d);
    g_free(folded);
    return removed;
}

GArray *
on_document_action_blocks(const OnDocument *d)
{
    GArray *blocks = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (block_is_real_action(b))
            g_array_append_val(blocks, i);
    }
    return blocks;
}

gint
on_document_action_ord(const OnDocument *d, gint64 uid)
{
    GArray *blocks = on_document_action_blocks(d);
    gint ord = -1;                   /* the answer                          */
    for (guint i = 0; i < blocks->len && ord < 0; i++) {
        const OnBlock *b = on_document_block(d, g_array_index(blocks, guint, i));
        if (b->action_uid == uid)
            ord = (gint)i;
    }
    g_array_unref(blocks);
    return ord;
}

/* action_block() — the block index of the `ord`-th real action line, or
 * G_MAXUINT.                                                                */
static guint
action_block(const OnDocument *d, gint ord)
{
    GArray *blocks = on_document_action_blocks(d);
    guint i = (ord >= 0 && (guint)ord < blocks->len)
              ? g_array_index(blocks, guint, ord) : G_MAXUINT;
    g_array_unref(blocks);
    return i;
}

/* ---------------------------------------------------------------------------
 * action_text_span() — the byte span of the item TEXT inside a line: after
 * the '!' and its leading whitespace, up to an existing "due <date>" (or
 * the line end) less trailing whitespace.  The derivation the extractor
 * uses to produce OnActionItem.text, so a rewrite of exactly this span
 * re-extracts to the new text.  Returns whether that text ends struck —
 * the done state a rewrite must carry over.
 * ------------------------------------------------------------------------- */
static gboolean
action_text_span(const OnText *t, gsize *start, gsize *end)
{
    const gchar *rest = t->text->str + 1;
    gsize  text_bytes;               /* end of the item text in `rest`      */
    gsize  due_start;
    gint64 due;
    text_bytes = on_action_split_due(rest, &due_start, &due)
                 ? due_start : strlen(rest);
    while (text_bytes > 0 && g_ascii_isspace((guchar)rest[text_bytes - 1]))
        text_bytes--;
    gsize lead = 0;                  /* whitespace right after the '!'      */
    while (lead < text_bytes && g_ascii_isspace((guchar)rest[lead]))
        lead++;
    *start = 1 + lead;
    *end   = 1 + text_bytes;
    if (*end <= *start)
        return FALSE;
    const gchar *last = g_utf8_prev_char(t->text->str + *end);
    return (on_text_flags_at(t, (gsize)(last - t->text->str)) &
            ON_FMT_STRIKE) != 0;
}

gboolean
on_document_block_action(const OnDocument *d, guint block, gchar **text,
                         gint64 *due, gboolean *done)
{
    const OnBlock *b = on_document_block(d, block);
    if (b == NULL || !on_block_is_action(b) ||
        !action_rest_real(b->text->text->str + 1))
        return FALSE;
    const OnText *t = b->text;
    gsize start, end;                /* the item text's byte span           */
    action_text_span(t, &start, &end);
    if (text != NULL)
        *text = g_strndup(t->text->str + start, end - start);
    if (due != NULL) {
        gsize due_start;
        if (!on_action_split_due(t->text->str + 1, &due_start, due))
            *due = 0;
    }
    if (done != NULL) {
        /* Done = every non-space character after the '!' struck, the due
         * date included — an un-struck due suffix is an open item.      */
        gboolean struck = TRUE;
        for (const gchar *p = t->text->str + 1; *p != '\0';
             p = g_utf8_next_char(p)) {
            if (g_ascii_isspace((guchar)*p))
                continue;
            gsize o = (gsize)(p - t->text->str);
            if ((on_text_flags_at(t, o) & ON_FMT_STRIKE) == 0) {
                struck = FALSE;
                break;
            }
        }
        *done = struck;
    }
    return TRUE;
}

gboolean
on_document_action_strike(OnDocument *d, gint ord, gboolean done)
{
    guint i = action_block(d, ord);
    if (i == G_MAXUINT)
        return FALSE;
    OnText *t = on_document_block(d, i)->text;
    OnPos pos = { i, -1, 1 };
    if (t->text->len > 1) {
        on_document_begin_group(d);
        on_document_set_flags(d, pos, t->text->len - 1, ON_FMT_STRIKE, done);
        on_document_end_group(d);
    }
    return TRUE;
}

gboolean
on_document_action_due(OnDocument *d, gint ord, gint64 due)
{
    guint i = action_block(d, ord);
    if (i == G_MAXUINT)
        return FALSE;
    OnText *t = on_document_block(d, i)->text;
    gsize start, end;                /* the item text                       */
    gboolean struck = action_text_span(t, &start, &end);
    gchar *suffix = NULL;            /* the new " due …", none for 0        */
    if (due != 0) {
        /* A time GLib cannot represent (a row written by another program)
         * is refused before anything changes.                              */
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        if (dt == NULL)
            return FALSE;
        suffix = g_date_time_format(dt, " due %Y-%m-%d");
        g_date_time_unref(dt);
    }

    on_document_begin_group(d);
    /* Everything after the text goes: old suffix, its spacing.             */
    OnPos pos = { i, -1, end };
    if (t->text->len > end)
        on_document_delete_text(d, pos, t->text->len - end);
    if (suffix != NULL)
        on_document_insert_text(d, pos, suffix, strlen(suffix),
                                struck ? ON_FMT_STRIKE : 0);
    g_free(suffix);
    on_document_end_group(d);
    return TRUE;
}

gboolean
on_document_action_text(OnDocument *d, gint ord, const gchar *text)
{
    guint i = action_block(d, ord);
    gsize n = text != NULL ? strlen(text) : 0;
    if (i == G_MAXUINT || n == 0 || !g_utf8_validate_len(text, (gssize)n, NULL) ||
        on_document_text_has_line_break(text, n) || strstr(text, ON_OBJ_CHAR) != NULL)
        return FALSE;
    OnText *t = on_document_block(d, i)->text;
    gsize start, end;                /* the item text                       */
    gboolean struck = action_text_span(t, &start, &end);
    if (g_strstr_len(t->text->str + start, (gssize)(end - start), ON_OBJ_CHAR) != NULL)
        return FALSE;                /* an inline image would be deleted    */

    /* The line as it will read: it must still be an item, and its text
     * must read back as `text` less its outer spaces (a new text of
     * "due 2026-01-01" would turn into the due date of an empty item).
     * Checked before anything is deleted: a refused rename changes
     * nothing.                                                             */
    OnText *line = on_text_new();
    on_text_append(line, t->text->str, start, 0);
    on_text_append(line, text, n, 0);
    on_text_append(line, t->text->str + end, t->text->len - end, 0);
    gsize s2, e2;                    /* the new line's item text            */
    action_text_span(line, &s2, &e2);
    gsize lead = 0, tail = n;        /* `text` without its outer spaces     */
    while (lead < tail && g_ascii_isspace((guchar)text[lead])) lead++;
    while (tail > lead && g_ascii_isspace((guchar)text[tail - 1])) tail--;
    gboolean same = action_rest_real(line->text->str + 1) && tail > lead &&
                    e2 - s2 == tail - lead &&
                    memcmp(line->text->str + s2, text + lead, tail - lead) == 0;
    on_text_free(line);
    if (!same)
        return FALSE;

    on_document_begin_group(d);
    OnPos pos = { i, -1, start };
    if (end > start)
        on_document_delete_text(d, pos, end - start);
    on_document_insert_text(d, pos, text + lead, tail - lead,
                            struck ? ON_FMT_STRIKE : 0);
    on_document_end_group(d);
    return TRUE;
}
