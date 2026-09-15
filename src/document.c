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
    t->images = g_array_new(FALSE, FALSE, sizeof(OnInlineImage));
    return t;
}

/* inline_image_clear() — release what an inline image holds.               */
static void
inline_image_clear(OnInlineImage *img)
{
    if (img->pixels_free != NULL)
        img->pixels_free(img->pixels);
    g_bytes_unref(img->png);
}

void
on_text_free(OnText *t)
{
    if (t == NULL)
        return;
    for (guint i = 0; i < t->images->len; i++)
        inline_image_clear(&g_array_index(t->images, OnInlineImage, i));
    g_array_free(t->images, TRUE);
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
    for (guint i = 0; i < t->images->len; i++) {
        OnInlineImage img = g_array_index(t->images, OnInlineImage, i);
        g_bytes_ref(img.png);
        img.pixels      = NULL;      /* a view's cache is not copied        */
        img.pixels_free = NULL;
        g_array_append_val(c->images, img);
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
    for (guint k = 0; k < t->images->len; k++) {
        OnInlineImage *img = &g_array_index(t->images, OnInlineImage, k);
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
    for (guint k = 0; k < t->images->len; ) {
        OnInlineImage *img = &g_array_index(t->images, OnInlineImage, k);
        if (img->offset >= off && img->offset < off + n) {
            inline_image_clear(img);
            g_array_remove_index(t->images, k);
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
    OnInlineImage img = { off, g_bytes_ref(png), display_width, NULL, NULL };
    guint k = 0;                     /* keep the array sorted by offset     */
    while (k < t->images->len &&
           g_array_index(t->images, OnInlineImage, k).offset < off)
        k++;
    g_array_insert_val(t->images, k, img);
}

/* ===========================================================================
 * BLOCKS
 * ======================================================================== */

gboolean
on_block_kind_is_text(OnBlockKind kind)
{
    return kind != ON_BLOCK_IMAGE && kind != ON_BLOCK_TABLE;
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

    if (!allow_newline && memchr(t->text->str, '\n', t->text->len) != NULL)
        FAIL("text contains a newline");
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
    for (guint k = 0; k < t->images->len; k++) {
        const OnInlineImage *img = &g_array_index(t->images, OnInlineImage, k);
        if (k > 0 && img->offset <= last)
            FAIL("inline images out of order");
        if (img->offset + ON_OBJ_CHAR_LEN > t->text->len ||
            memcmp(t->text->str + img->offset, ON_OBJ_CHAR,
                   ON_OBJ_CHAR_LEN) != 0)
            FAIL("inline image %u has no U+FFFC at its offset", k);
        if (img->png == NULL)
            FAIL("inline image %u has no bytes", k);
        last = img->offset;
    }
    /* Every U+FFFC must have an image.                                     */
    guint n_obj = 0;                 /* U+FFFC characters in the text       */
    for (const gchar *p = t->text->str;
         (p = g_strstr_len(p, (gssize)(t->text->len - (p - t->text->str)),
                           ON_OBJ_CHAR)) != NULL;
         p += ON_OBJ_CHAR_LEN)
        n_obj++;
    if (n_obj != t->images->len)
        FAIL("%u U+FFFC characters but %u inline images", n_obj,
             t->images->len);
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
                if (cell == NULL || cell->images->len != 0) {
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

    L->number_run = (b->kind == ON_BLOCK_NUMBER) ? L->number_run + 1 : 0;
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
        g_string_append(t->text, cell);
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

OnDocument *
on_document_from_text(const gchar *text)
{
    OnDocument *d = doc_alloc();
    const gchar *p   = (text != NULL) ? text : "";
    const gchar *end = p + strlen(p);
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
    Loader L = { d, rep, NULL, 0, FALSE, FALSE, FALSE, FALSE, 0 };

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
                    break;
                }
                case ON_REC_IMAGE: image_record(&L, &rec); break;
                case ON_REC_TABLE: table_record(&L, &rec); break;
                case ON_REC_CHECK: check_record(&L, &rec); break;
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
           rep->run_split == 0 && rep->cr_newlines == 0;
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

/* sv_runs() — a text's runs under `para`, its inline images as records.     */
static void
sv_runs(Saver *S, const OnText *t, guint32 para)
{
    gsize at = 0;                    /* start of the current run            */
    guint k  = 0;                    /* next inline image                   */
    for (guint i = 0; i < t->runs->len; i++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, i);
        gsize cur = at, end = at + r->len;
        while (k < t->images->len &&
               g_array_index(t->images, OnInlineImage, k).offset < end) {
            const OnInlineImage *img =
                &g_array_index(t->images, OnInlineImage, k);
            sv_text(S, para | r->flags, t->text->str + cur,
                    img->offset - cur);
            sv_image(S, img->png, img->display_width);
            cur = img->offset + ON_OBJ_CHAR_LEN;
            k++;
        }
        sv_text(S, para | r->flags, t->text->str + cur, end - cur);
        at = end;
    }
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
        number_run = (b->kind == ON_BLOCK_NUMBER) ? number_run + 1 : 0;

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
        if (i + 1 < d->blocks->len)
            sv_text(&S, para | b->eol_flags, "\n", 1);
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
    gboolean    on;                  /* SET_FLAGS / SET_CHECKED / HEADER     */
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
    for (guint k = 0; k < t->images->len; k++) {
        OnInlineImage img = g_array_index(t->images, OnInlineImage, k);
        if (img.offset >= off && img.offset < off + n) {
            img.offset -= off;
            g_bytes_ref(img.png);
            img.pixels      = NULL;
            img.pixels_free = NULL;
            g_array_append_val(f->images, img);
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
    for (guint k = 0; k < f->images->len; k++) {
        const OnInlineImage *img = &g_array_index(f->images, OnInlineImage, k);
        on_text_add_image(t, off + img->offset, img->png, img->display_width);
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
    return b->text != NULL && b->kind != ON_BLOCK_CODE &&
           b->text->text->len > 0 && b->text->text->str[0] == '!';
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
        /* Two #tags meeting at the seam become one.                        */
        if (t->text->len > 0 && c->text->text->len > 0 &&
            (on_text_flags_at(t, t->text->len - 1) &
             on_text_flags_at(c->text, 0) & ON_FMT_TAG))
            d->tags_modified = TRUE;
        text_paste(t, t->text->len, c->text);
        b->eol_flags = c->eol_flags;
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
        g_ptr_array_add(d->undo_stack, d->open_group);
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
        (pos.cell < 0 && memchr(s, '\n', n) != NULL) ||
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
             * range reaches across it.                                     */
            if ((i == a.block && a.offset == 1) ||
                (i == b.block && b.offset == 0))
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
    } else if (last->kind == ON_BLOCK_TABLE && b.cell >= 0) {
        drop_last = TRUE;            /* a range ending inside a table takes
                                        the table                            */
    }
    for (guint i = b.block - 1; i > a.block; i--)
        on_document_remove_block(d, i);
    if (drop_last)
        on_document_remove_block(d, a.block + 1);

    gboolean first_text = on_block_kind_is_text(first->kind);
    gboolean drop_first = !first_text && (a.offset == 0 || a.cell >= 0);
    if (first_text) {
        gsize len = first->text->text->len;
        if (a.cell >= 0) {           /* cannot happen: text blocks have no
                                        cells */
        } else if (a.offset < len) {
            on_document_delete_text(d, a, len - a.offset);
        }
    }
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
    if (n > 1 && !on_block_kind_is_text(fl->kind)) {
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
        g_ptr_array_add(d->undo_stack, d->open_group);
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
        g_ptr_array_add(d->undo_stack, d->open_group);
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

/* append_text_line() — a text's characters minus every U+FFFC.              */
static void
append_text_line(GString *out, const OnText *t)
{
    gsize cur = 0;
    for (guint k = 0; k < t->images->len; k++) {
        const OnInlineImage *img = &g_array_index(t->images, OnInlineImage, k);
        g_string_append_len(out, t->text->str + cur,
                            (gssize)(img->offset - cur));
        cur = img->offset + ON_OBJ_CHAR_LEN;
    }
    g_string_append_len(out, t->text->str + cur,
                        (gssize)(t->text->len - cur));
}

gchar *
on_document_plain_text(const OnDocument *d)
{
    GString *out = g_string_new(NULL);
    gint number_run = 0;
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        number_run = (b->kind == ON_BLOCK_NUMBER) ? number_run + 1 : 0;
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

gint
on_document_image_count(const OnDocument *d)
{
    gint n = 0;
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->kind == ON_BLOCK_IMAGE)
            n++;
        else if (b->text != NULL)
            n += (gint)b->text->images->len;
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
            if ((guint)ord < b->text->images->len) {
                const OnInlineImage *img =
                    &g_array_index(b->text->images, OnInlineImage, ord);
                if (display_width != NULL)
                    *display_width = img->display_width;
                return img->png;
            }
            ord -= (gint)b->text->images->len;
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

GList *
on_document_collect_tags(const OnDocument *d)
{
    GList *names = NULL;             /* collected, reversed                 */
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (b->text == NULL)
            continue;
        gsize at = 0;                /* start of run k                      */
        for (guint k = 0; k < b->text->runs->len; k++) {
            const OnRun *r = &g_array_index(b->text->runs, OnRun, k);
            if (r->flags & ON_FMT_TAG) {
                gchar *name = g_strndup(b->text->text->str + at, r->len);
                g_strstrip(name);
                const gchar *bare = (*name == '#') ? name + 1 : name;
                if (*bare != '\0' &&
                    g_list_find_custom(names, bare,
                                       (GCompareFunc)g_strcmp0) == NULL)
                    names = g_list_prepend(names, g_strdup(bare));
                g_free(name);
            }
            at += r->len;
        }
    }
    return g_list_reverse(names);
}

/* ---------------------------------------------------------------------------
 * action_rest_real() — does an action line's rest-of-line text hold a
 * REAL item?  Bare "!" lines and lines that are nothing but a "due <date>"
 * suffix do not count — the extractor's rule, so ord numbering stays
 * aligned with the action_items table.
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

GArray *
on_document_action_blocks(const OnDocument *d)
{
    GArray *blocks = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < d->blocks->len; i++) {
        const OnBlock *b = g_ptr_array_index(d->blocks, i);
        if (on_block_is_action(b) && action_rest_real(b->text->text->str + 1))
            g_array_append_val(blocks, i);
    }
    return blocks;
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

    on_document_begin_group(d);
    /* Everything after the text goes: old suffix, its spacing.             */
    OnPos pos = { i, -1, end };
    if (t->text->len > end)
        on_document_delete_text(d, pos, t->text->len - end);
    if (due != 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        gchar *suffix = g_date_time_format(dt, " due %Y-%m-%d");
        g_date_time_unref(dt);
        on_document_insert_text(d, pos, suffix, strlen(suffix),
                                struck ? ON_FMT_STRIKE : 0);
        g_free(suffix);
    }
    on_document_end_group(d);
    return TRUE;
}

gboolean
on_document_action_text(OnDocument *d, gint ord, const gchar *text)
{
    guint i = action_block(d, ord);
    if (i == G_MAXUINT)
        return FALSE;
    OnText *t = on_document_block(d, i)->text;
    gsize start, end;                /* the item text                       */
    gboolean struck = action_text_span(t, &start, &end);

    on_document_begin_group(d);
    OnPos pos = { i, -1, start };
    if (end > start)
        on_document_delete_text(d, pos, end - start);
    on_document_insert_text(d, pos, text, strlen(text),
                            struck ? ON_FMT_STRIKE : 0);
    on_document_end_group(d);
    return TRUE;
}
