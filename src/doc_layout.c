/* ===========================================================================
 * doc_layout.c — OnDocLayout (implementation).  See doc_layout.h.
 *
 * One BL (block layout) per document block, invalidated per block by the
 * observer relays and re-laid out on the next geometry question.  Block y
 * offsets are a prefix sum recomputed whenever any block was stale.
 * =========================================================================== */

#include "doc_layout.h"
#include "app.h"                     /* the emoji padding rule              */

#include <string.h>

/* The page: the view's margins, and the gap under every block.             */
#define LEFT_MARGIN    16
#define RIGHT_MARGIN   16
#define TOP_MARGIN     12
#define BOTTOM_MARGIN  20
#define BLOCK_GAP       2

/* Kinds' geometry.                                                          */
#define LIST_INDENT    32            /* list text starts here; the prefix
                                        sits in the gutter before it        */
#define CODE_INSET     24            /* code text inset inside its shading  */
#define CODE_INSET_NUM 36            /* … with room for line numbers        */
#define CODE_PAD        3            /* shading above the first / below the
                                        last line of a run                  */
#define TABLE_PAD       6            /* cell padding                        */
#define TABLE_COL_MAX 320            /* a column never asks for more        */
#define TABLE_COL_MIN  40
#define CHECK_SIZE     14            /* the drawn task box                  */
#define H1_SCALE      1.6
#define H2_SCALE      1.3
#define COPY_WORD    "copy"
/* The default thumbnail box a freshly inserted image fits (aspect kept,
 * never upscaled); "Display Full Size" on its menu enlarges it.            */
#define IMAGE_THUMB_W 200
#define IMAGE_THUMB_H 125

/* Colours (the app is a light theme; nothing here follows the widget's
 * backdrop state — that is the point, see CLAUDE.md quirk #23).            */
static const GdkRGBA C_TAG      = { 0.764, 0.353, 0.000, 1 };  /* #c35a00 */
static const GdkRGBA C_ACTION   = { 0.102, 0.373, 0.706, 1 };  /* #1a5fb4 */
static const GdkRGBA C_CODE_BG  = { 0.941, 0.941, 0.941, 1 };  /* #f0f0f0 */
static const GdkRGBA C_CODE_NUM = { 0.533, 0.533, 0.533, 1 };
static const GdkRGBA C_TABLE_BD = { 0.733, 0.733, 0.733, 1 };  /* #bbb    */
static const GdkRGBA C_HEADER   = { 0.925, 0.925, 0.925, 1 };  /* #ececec */
static const GdkRGBA C_HIT      = { 1.000, 0.925, 0.545, 1 };  /* #ffec8b */
static const GdkRGBA C_SEL      = { 0.36, 0.56, 0.90, 0.35 };
static const GdkRGBA C_SEL_DIM  = { 0.60, 0.60, 0.60, 0.30 };
static const GdkRGBA C_CHECK    = { 0.20, 0.47, 0.86, 1 };
static const GdkRGBA C_WHITE    = { 1, 1, 1, 1 };

/* ---------------------------------------------------------------------------
 * BL — one block's layout.
 * ------------------------------------------------------------------------- */
typedef struct {
    gboolean     valid;
    PangoLayout *layout;             /* text kinds                          */
    gint         y;                  /* top, content coordinates            */
    gint         h;                  /* height INCLUDING the gap below      */
    gint         x;                  /* left of the text / object           */
    gint         w;                  /* layout width / object width         */
    gint         img_h;              /* IMAGE block                         */
    GArray      *col_w;              /* TABLE: gint per column              */
    GArray      *row_h;              /* TABLE: gint per row                 */
    GPtrArray   *cells;              /* TABLE: PangoLayout* per cell        */
    gint         code_first;         /* CODE: first block of the run        */
    gint         code_len;           /* CODE: blocks in the run             */
} BL;

struct OnDocLayout {
    OnDocument       *doc;
    PangoContext     *ctx;
    gint              emoji_pad;     /* on_emoji_pad() for ctx's font, Pango
                                      * units; 0 = the font fits, no pad   */
    gint              emoji_caret_pad; /* px the caret moves right after a
                                      * line's LAST emoji: Pango drops the
                                      * trailing half of the spacing there
                                      * and the glyph overdraws by exactly
                                      * that half less ON_EMOJI_GAP        */
    gint              width;
    OnDocLayoutStyle  style;
    GArray           *bl;            /* BL, one per block                   */
    gboolean          geometry_valid;/* every BL valid and y's summed       */
    gint              total_h;
    /* preedit */
    gboolean          pre_on;
    OnPos             pre_pos;
    gchar            *pre_text;
    PangoAttrList    *pre_attrs;
    gint              pre_cursor;
    /* find hits */
    GArray           *hits;          /* OnDocHit                            */
    /* cached glyph layouts */
    PangoLayout      *copy_word;
    PangoLayout      *bullet;
    /* cached font descriptions: [mono][bold][scale index]                 */
    PangoFontDescription *fonts[2][2][3];
};

static void fonts_clear(OnDocLayout *L);

/* bl_clear() — drop what a BL holds (not the struct).                       */
static void
bl_clear(BL *b)
{
    g_clear_object(&b->layout);
    if (b->col_w != NULL) { g_array_unref(b->col_w); b->col_w = NULL; }
    if (b->row_h != NULL) { g_array_unref(b->row_h); b->row_h = NULL; }
    if (b->cells != NULL) { g_ptr_array_unref(b->cells); b->cells = NULL; }
    b->valid = FALSE;
}

OnDocLayout *
on_doc_layout_new(OnDocument *doc, PangoContext *ctx)
{
    OnDocLayout *L = g_new0(OnDocLayout, 1);
    L->doc = doc;
    L->ctx = ctx;
    L->emoji_pad = on_emoji_pad(ctx);
    L->emoji_caret_pad = L->emoji_pad / (2 * PANGO_SCALE) - ON_EMOJI_GAP;
    L->width = 400;
    L->style.title_line = TRUE;
    L->bl = g_array_new(FALSE, TRUE, sizeof(BL));
    g_array_set_size(L->bl, on_document_n_blocks(doc));
    return L;
}

void
on_doc_layout_free(OnDocLayout *L)
{
    if (L == NULL)
        return;
    for (guint i = 0; i < L->bl->len; i++)
        bl_clear(&g_array_index(L->bl, BL, i));
    g_array_unref(L->bl);
    if (L->hits != NULL)
        g_array_unref(L->hits);
    g_free(L->pre_text);
    if (L->pre_attrs != NULL)
        pango_attr_list_unref(L->pre_attrs);
    g_clear_object(&L->copy_word);
    g_clear_object(&L->bullet);
    fonts_clear(L);
    g_free(L);
}

/* invalidate() — one block stale.                                           */
static void
invalidate(OnDocLayout *L, guint i)
{
    if (i < L->bl->len) {
        bl_clear(&g_array_index(L->bl, BL, i));
        L->geometry_valid = FALSE;
    }
}

void
on_doc_layout_invalidate_all(OnDocLayout *L)
{
    for (guint i = 0; i < L->bl->len; i++)
        bl_clear(&g_array_index(L->bl, BL, i));
    g_clear_object(&L->copy_word);
    g_clear_object(&L->bullet);
    fonts_clear(L);
    L->geometry_valid = FALSE;
}

void
on_doc_layout_set_width(OnDocLayout *L, gint width)
{
    if (width == L->width)
        return;
    L->width = width;
    on_doc_layout_invalidate_all(L);
}

void
on_doc_layout_set_style(OnDocLayout *L, const OnDocLayoutStyle *style)
{
    L->style = *style;
    on_doc_layout_invalidate_all(L);
}

void
on_doc_layout_set_document(OnDocLayout *L, OnDocument *doc)
{
    for (guint i = 0; i < L->bl->len; i++)
        bl_clear(&g_array_index(L->bl, BL, i));
    L->doc = doc;
    g_array_set_size(L->bl, 0);
    g_array_set_size(L->bl, on_document_n_blocks(doc));
    L->pre_on = FALSE;
    if (L->hits != NULL) {
        g_array_unref(L->hits);
        L->hits = NULL;
    }
    L->geometry_valid = FALSE;
}

/* A CODE block's run neighbours change when a CODE block appears or goes:
 * invalidate the blocks on either side too, so their run info is fresh.  */
static void
invalidate_around(OnDocLayout *L, guint i)
{
    if (i > 0)
        invalidate(L, i - 1);
    invalidate(L, i);
    invalidate(L, i + 1);
}

void
on_doc_layout_block_changed(OnDocLayout *L, guint i)
{
    invalidate_around(L, i);
}

void
on_doc_layout_blocks_inserted(OnDocLayout *L, guint i, guint n)
{
    BL empty;
    memset(&empty, 0, sizeof empty);
    for (guint k = 0; k < n; k++)
        g_array_insert_val(L->bl, i, empty);
    invalidate_around(L, i);
    invalidate(L, i + n);
    L->geometry_valid = FALSE;
}

void
on_doc_layout_blocks_removed(OnDocLayout *L, guint i, guint n)
{
    for (guint k = 0; k < n && i < L->bl->len; k++)
        bl_clear(&g_array_index(L->bl, BL, i));
    g_array_remove_range(L->bl, i, MIN(n, L->bl->len - i));
    invalidate_around(L, i);
    if (i > 0)
        invalidate(L, i - 1);
    L->geometry_valid = FALSE;
}

void
on_doc_layout_set_preedit(OnDocLayout *L, OnPos pos, const gchar *text,
                          PangoAttrList *attrs, gint cursor)
{
    if (L->pre_on)
        invalidate(L, L->pre_pos.block);
    g_free(L->pre_text);
    if (L->pre_attrs != NULL)
        pango_attr_list_unref(L->pre_attrs);
    L->pre_on     = text != NULL && *text != '\0';
    L->pre_pos    = pos;
    L->pre_text   = L->pre_on ? g_strdup(text) : NULL;
    L->pre_attrs  = (L->pre_on && attrs != NULL) ? pango_attr_list_ref(attrs)
                                                  : NULL;
    L->pre_cursor = cursor;
    if (L->pre_on)
        invalidate(L, pos.block);
}

void
on_doc_layout_set_hits(OnDocLayout *L, const GArray *hits)
{
    /* Blocks that had or get a hit re-lay out.                             */
    if (L->hits != NULL) {
        for (guint k = 0; k < L->hits->len; k++)
            invalidate(L, g_array_index(L->hits, OnDocHit, k).block);
        g_array_unref(L->hits);
        L->hits = NULL;
    }
    if (hits != NULL && hits->len > 0) {
        L->hits = g_array_sized_new(FALSE, FALSE, sizeof(OnDocHit), hits->len);
        g_array_append_vals(L->hits, hits->data, hits->len);
        for (guint k = 0; k < L->hits->len; k++)
            invalidate(L, g_array_index(L->hits, OnDocHit, k).block);
    }
}

/* ===========================================================================
 * LAYING OUT
 * ======================================================================== */

/* content_width() — the width between the margins.                          */
static gint
content_width(OnDocLayout *L)
{
    return MAX(40, L->width - LEFT_MARGIN - RIGHT_MARGIN);
}

/* font_for() — the base font, scaled by `scale`, monospace when asked.
 * A NEW description (free it); the cached ones below serve the blocks.  */
static PangoFontDescription *
font_for(OnDocLayout *L, gdouble scale, gboolean mono)
{
    PangoFontDescription *fd =
        pango_font_description_copy(pango_context_get_font_description(L->ctx));
    if (mono)
        pango_font_description_set_family(fd, "monospace");
    if (scale != 1.0) {
        gint size = pango_font_description_get_size(fd);
        if (size == 0)
            size = 10 * PANGO_SCALE;
        if (pango_font_description_get_size_is_absolute(fd))
            pango_font_description_set_absolute_size(fd, size * scale);
        else
            pango_font_description_set_size(fd, (gint)(size * scale));
    }
    return fd;
}

/* block_font() — the six fonts a block can use, built once: plain and
 * monospace, regular and bold, at body / H2 / H1 size.  Every block's
 * layout would otherwise pay a font-map lookup for a fresh description
 * (measured: 615 blocks 221 ms → 45 ms).                                   */
static const PangoFontDescription *
block_font(OnDocLayout *L, gboolean mono, gboolean bold, gdouble scale)
{
    gint si = scale == H1_SCALE ? 2 : scale == H2_SCALE ? 1 : 0;
    PangoFontDescription **slot = &L->fonts[mono][bold][si];
    if (*slot == NULL) {
        *slot = font_for(L, scale, mono);
        if (bold)
            pango_font_description_set_weight(*slot, PANGO_WEIGHT_BOLD);
    }
    return *slot;
}

/* fonts_clear() — drop the cached descriptions (a font change).            */
static void
fonts_clear(OnDocLayout *L)
{
    for (gint m = 0; m < 2; m++)
        for (gint b = 0; b < 2; b++)
            for (gint s = 0; s < 3; s++)
                g_clear_pointer(&L->fonts[m][b][s], pango_font_description_free);
}

/* emoji_sequence_end() — for `p` at an emoji, the byte just past its whole
 * sequence: joiners (variation selectors, ZWJ, keycap, tags —
 * on_is_emoji_joiner) and any emoji they join, skin tones included (those
 * are emoji characters themselves).  Adjacent independent emoji fold into
 * one span too, which changes nothing: Pango spaces every cluster inside
 * a run exactly as it would two runs.                                      */
static const gchar *
emoji_sequence_end(const gchar *p)
{
    const gchar *e = g_utf8_next_char(p);
    while (*e != '\0') {
        gunichar c = g_utf8_get_char(e);
        if (!on_is_emoji_char(c) && !on_is_emoji_joiner(c))
            break;
        e = g_utf8_next_char(e);
    }
    return e;
}

/* ends_with_emoji() — does the text before byte `off` end in an emoji
 * sequence?                                                                 */
static gboolean
ends_with_emoji(const gchar *text, gsize off)
{
    /* Walk back over the sequence's tail, then check the head.             */
    const gchar *p = text + off;
    while (p > text) {
        const gchar *q = g_utf8_prev_char(p);
        gunichar c = g_utf8_get_char(q);
        if (on_is_emoji_char(c))
            return TRUE;
        if (!on_is_emoji_joiner(c))
            return FALSE;
        p = q;
    }
    return FALSE;
}

/* image_texture() — decode (once) the image behind `png` into `*cache`.     */
static GdkTexture *
image_texture(GBytes *png, gpointer *cache, GDestroyNotify *cache_free)
{
    if (*cache != NULL)
        return *cache;
    GError *err = NULL;
    GdkTexture *tex = gdk_texture_new_from_bytes(png, &err);
    if (tex == NULL) {
        g_warning("image: bad image data: %s", err->message);
        g_clear_error(&err);
        return NULL;
    }
    *cache      = tex;
    *cache_free = g_object_unref;
    return tex;
}

/* image_size() — an image's pixel size from its PNG header (IHDR sits at
 * a fixed offset), so laying out a note decodes nothing; a payload that is
 * not a PNG is decoded to be measured.                                     */
static gboolean
image_size(GBytes *png, gpointer *cache, GDestroyNotify *cache_free,
           gint *w, gint *h)
{
    gsize n;
    const guint8 *d = g_bytes_get_data(png, &n);
    static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (n >= 24 && memcmp(d, sig, 8) == 0 && memcmp(d + 12, "IHDR", 4) == 0) {
        *w = (gint)(((guint32)d[16] << 24) | ((guint32)d[17] << 16) |
                    ((guint32)d[18] << 8) | d[19]);
        *h = (gint)(((guint32)d[20] << 24) | ((guint32)d[21] << 16) |
                    ((guint32)d[22] << 8) | d[23]);
        return *w > 0 && *h > 0;
    }
    GdkTexture *tex = image_texture(png, cache, cache_free);
    if (tex == NULL)
        return FALSE;
    *w = gdk_texture_get_width(tex);
    *h = gdk_texture_get_height(tex);
    return TRUE;
}

/* image_fit() — the on-screen size of an image: the stored width, or the
 * default thumbnail box (ON_IMAGE_THUMB_W × H, aspect kept), never wider
 * than the source or the content.                                          */
static void
image_fit(OnDocLayout *L, gint w, gint h, guint32 display_width,
          gint *out_w, gint *out_h)
{
    gint want;
    if (display_width > 0) {
        want = MIN((gint)display_width, w);
    } else {
        want = MIN(w, IMAGE_THUMB_W);
        if (h > 0 && h * want > IMAGE_THUMB_H * w)
            want = w * IMAGE_THUMB_H / h;
    }
    want = MAX(1, MIN(want, content_width(L)));
    *out_w = want;
    *out_h = MAX(1, (gint)((gdouble)h * want / w));
}

/* Layout index ⇄ document offset around an active preedit in block i.      */
static gsize
idx_of(OnDocLayout *L, guint i, gint cell, gsize off)
{
    if (L->pre_on && L->pre_pos.block == i && L->pre_pos.cell == cell &&
        off >= L->pre_pos.offset)
        return off + strlen(L->pre_text);
    return off;
}

static gsize
off_of(OnDocLayout *L, guint i, gint cell, gsize idx)
{
    if (L->pre_on && L->pre_pos.block == i && L->pre_pos.cell == cell &&
        idx > L->pre_pos.offset) {
        gsize n = strlen(L->pre_text);
        return (idx >= L->pre_pos.offset + n) ? idx - n : L->pre_pos.offset;
    }
    return idx;
}

/* ---------------------------------------------------------------------------
 * attr_span() — add one attribute over [start, end) of the layout text.
 * ------------------------------------------------------------------------- */
static void
attr_span(PangoAttrList *attrs, PangoAttribute *a, gsize start, gsize end)
{
    a->start_index = (guint)start;
    a->end_index   = (guint)end;
    pango_attr_list_insert(attrs, a);
}

/* ---------------------------------------------------------------------------
 * text_layout_new() — a PangoLayout for an OnText: the runs as attributes,
 * inline images as shape attributes, the derived looks, the find hits and
 * the preedit.
 *   kind   — the owning block's kind (or PARA for a cell).
 *   block  — the block index; cell -1 for block text.
 *   action — the block is an action line (the blue tint).
 *   title  — the block is the derived title (centred, heading-sized).
 * ------------------------------------------------------------------------- */
static PangoLayout *
text_layout_new(OnDocLayout *L, const OnText *t, OnBlockKind kind,
                guint block, gint cell, gboolean action, gboolean title,
                gboolean header_cell, gint width)
{
    PangoLayout *layout = pango_layout_new(L->ctx);
    gdouble scale = title || kind == ON_BLOCK_H1 ? H1_SCALE
                  : kind == ON_BLOCK_H2 ? H2_SCALE : 1.0;
    pango_layout_set_font_description(
        layout, block_font(L, kind == ON_BLOCK_CODE,
                           title || kind == ON_BLOCK_H1 ||
                           kind == ON_BLOCK_H2 || header_cell, scale));
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    if (title)
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    /* The text, with the preedit spliced in.                               */
    gboolean pre = L->pre_on && L->pre_pos.block == block &&
                   L->pre_pos.cell == cell;
    gsize pre_off = pre ? L->pre_pos.offset : 0;
    gsize pre_len = pre ? strlen(L->pre_text) : 0;
    if (pre) {
        GString *s = g_string_new_len(t->text->str, (gssize)pre_off);
        g_string_append(s, L->pre_text);
        g_string_append_len(s, t->text->str + pre_off,
                            (gssize)(t->text->len - pre_off));
        pango_layout_set_text(layout, s->str, (gint)s->len);
        g_string_free(s, TRUE);
    } else {
        pango_layout_set_text(layout, t->text->str, (gint)t->text->len);
    }
#define IDX(o) ((gsize)(o) + ((pre && (gsize)(o) >= pre_off) ? pre_len : 0))

    PangoAttrList *attrs = pango_attr_list_new();
    gsize len = t->text->len;

    /* The action tint first, so #tags laid over it keep their own colour. */
    if (action && len > 0) {
        attr_span(attrs, pango_attr_foreground_new(
            (guint16)(C_ACTION.red * 65535), (guint16)(C_ACTION.green * 65535),
            (guint16)(C_ACTION.blue * 65535)), 0, IDX(len));
        attr_span(attrs, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD),
                  0, IDX(len));
    }

    gsize at = 0;
    for (guint k = 0; k < t->runs->len; k++) {
        const OnRun *r = &g_array_index(t->runs, OnRun, k);
        gsize s = IDX(at), e = IDX(at + r->len);
        if (kind != ON_BLOCK_CODE) {
            if (r->flags & ON_FMT_BOLD)
                attr_span(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD), s, e);
            if (r->flags & ON_FMT_ITALIC)
                attr_span(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC), s, e);
            if (r->flags & ON_FMT_UNDERLINE)
                attr_span(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
                          s, e);
            if (r->flags & ON_FMT_STRIKE)
                attr_span(attrs, pango_attr_strikethrough_new(TRUE), s, e);
            if (r->flags & ON_FMT_TAG) {
                attr_span(attrs, pango_attr_foreground_new(
                    (guint16)(C_TAG.red * 65535), (guint16)(C_TAG.green * 65535),
                    (guint16)(C_TAG.blue * 65535)), s, e);
                attr_span(attrs, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD),
                          s, e);
            }
        }
        at += r->len;
    }

    /* Inline images take their box through a shape attribute.             */
    for (guint k = 0; k < t->images->len; k++) {
        OnInlineImage *img = &g_array_index(t->images, OnInlineImage, k);
        gint w = 16, h = 16, iw, ih;
        if (image_size(img->png, &img->pixels, &img->pixels_free, &iw, &ih))
            image_fit(L, iw, ih, img->display_width, &w, &h);
        PangoRectangle rect = { 0, -h * PANGO_SCALE, w * PANGO_SCALE,
                                h * PANGO_SCALE };
        attr_span(attrs, pango_attr_shape_new(&rect, &rect),
                  IDX(img->offset), IDX(img->offset) + ON_OBJ_CHAR_LEN);
    }

    /* Apple Color Emoji overdraws its advance: pad each emoji (D24) by the
     * MEASURED amount, on_emoji_pad() — 0 where the font fits, so this is
     * a no-op on Linux without any platform test.  The span covers the
     * WHOLE emoji sequence — variation selectors, skin tones, ZWJ-joined
     * parts — so an attribute boundary never cuts one (Pango would shape
     * the tail on its own, as a hex box).  Pango splits the spacing half
     * before, half after the glyph and DROPS the trailing half at a line
     * end; the caret after a line's last emoji gets emoji_caret_pad for
     * that.                                                              */
    for (const gchar *p = t->text->str;
         L->emoji_pad != 0 && *p != '\0'; p = g_utf8_next_char(p)) {
        if (!on_is_emoji_char(g_utf8_get_char(p)))
            continue;
        const gchar *e = emoji_sequence_end(p);
        gsize o = (gsize)(p - t->text->str);
        attr_span(attrs, pango_attr_letter_spacing_new(L->emoji_pad),
                  IDX(o), IDX((gsize)(e - t->text->str)));
        p = g_utf8_prev_char(e);
    }

    /* Find hits.                                                           */
    if (L->hits != NULL) {
        for (guint k = 0; k < L->hits->len; k++) {
            const OnDocHit *h = &g_array_index(L->hits, OnDocHit, k);
            if (h->block != block || h->cell != cell)
                continue;
            attr_span(attrs, pango_attr_background_new(
                (guint16)(C_HIT.red * 65535), (guint16)(C_HIT.green * 65535),
                (guint16)(C_HIT.blue * 65535)),
                IDX(h->start), IDX(h->start + h->len));
        }
    }

    /* The preedit's own attributes, shifted to where it sits.              */
    if (pre && L->pre_attrs != NULL) {
        PangoAttrIterator *it = pango_attr_list_get_iterator(L->pre_attrs);
        do {
            gint s, e;
            pango_attr_iterator_range(it, &s, &e);
            if (e == G_MAXINT)
                e = (gint)pre_len;
            GSList *list = pango_attr_iterator_get_attrs(it);
            for (GSList *l = list; l != NULL; l = l->next) {
                PangoAttribute *a = pango_attribute_copy(l->data);
                attr_span(attrs, a, pre_off + (gsize)s,
                          pre_off + (gsize)MIN(e, (gint)pre_len));
            }
            g_slist_free_full(list, (GDestroyNotify)pango_attribute_destroy);
        } while (pango_attr_iterator_next(it));
        pango_attr_iterator_destroy(it);
    } else if (pre) {
        attr_span(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
                  pre_off, pre_off + pre_len);
    }
#undef IDX

    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
    return layout;
}

/* ---------------------------------------------------------------------------
 * layout_block() — lay block i out into its BL.
 * ------------------------------------------------------------------------- */
static void
layout_block(OnDocLayout *L, guint i)
{
    BL *b = &g_array_index(L->bl, BL, i);
    bl_clear(b);
    const OnBlock *blk = on_document_block(L->doc, i);
    gint cw = content_width(L);
    b->code_first = -1;
    b->code_len   = 0;

    if (blk->kind == ON_BLOCK_IMAGE) {
        gint w = 24, h = 24, iw, ih;
        if (image_size(blk->png, &((OnBlock *)blk)->pixels,
                       &((OnBlock *)blk)->pixels_free, &iw, &ih))
            image_fit(L, iw, ih, blk->display_width, &w, &h);
        b->x = LEFT_MARGIN;
        b->w = w;
        b->img_h = h;
        b->h = h + BLOCK_GAP;
    } else if (blk->kind == ON_BLOCK_TABLE) {
        /* Column widths: each column's widest unwrapped line, padded and
         * capped; shrunk proportionally when the sum overflows.            */
        b->col_w = g_array_sized_new(FALSE, TRUE, sizeof(gint), blk->cols);
        b->row_h = g_array_sized_new(FALSE, TRUE, sizeof(gint), blk->rows);
        g_array_set_size(b->col_w, blk->cols);
        g_array_set_size(b->row_h, blk->rows);
        b->cells = g_ptr_array_new_with_free_func(g_object_unref);
        for (gint c = 0; c < blk->cols; c++) {
            gint widest = TABLE_COL_MIN;
            for (gint r = 0; r < blk->rows; r++) {
                OnText *cell = on_block_cell(blk, r, c);
                PangoLayout *probe = text_layout_new(
                    L, cell, ON_BLOCK_PARA, i, r * blk->cols + c, FALSE,
                    FALSE, blk->header && r == 0, -1);
                pango_layout_set_width(probe, -1);
                gint pw, ph;
                pango_layout_get_pixel_size(probe, &pw, &ph);
                widest = MAX(widest, pw + 2 * TABLE_PAD + 2);
                g_object_unref(probe);
            }
            g_array_index(b->col_w, gint, c) = MIN(widest, TABLE_COL_MAX);
        }
        gint sum = 0;
        for (gint c = 0; c < blk->cols; c++)
            sum += g_array_index(b->col_w, gint, c);
        if (sum > cw) {
            for (gint c = 0; c < blk->cols; c++) {
                gint *w = &g_array_index(b->col_w, gint, c);
                *w = MAX(TABLE_COL_MIN, *w * cw / sum);
            }
        }
        for (gint r = 0; r < blk->rows; r++) {
            gint tallest = 0;
            for (gint c = 0; c < blk->cols; c++) {
                gint colw = g_array_index(b->col_w, gint, c);
                PangoLayout *cell = text_layout_new(
                    L, on_block_cell(blk, r, c), ON_BLOCK_PARA, i,
                    r * blk->cols + c, FALSE, FALSE, blk->header && r == 0,
                    MAX(8, colw - 2 * TABLE_PAD));
                g_ptr_array_add(b->cells, cell);
                gint pw, ph;
                pango_layout_get_pixel_size(cell, &pw, &ph);
                tallest = MAX(tallest, ph);
            }
            g_array_index(b->row_h, gint, r) = tallest + 2 * TABLE_PAD;
        }
        gint th = 1;
        for (gint r = 0; r < blk->rows; r++)
            th += g_array_index(b->row_h, gint, r);
        b->x = LEFT_MARGIN;
        b->w = 1;
        for (gint c = 0; c < blk->cols; c++)
            b->w += g_array_index(b->col_w, gint, c);
        b->h = th + BLOCK_GAP;
    } else {
        gint indent = 0;
        if (blk->kind == ON_BLOCK_BULLET || blk->kind == ON_BLOCK_NUMBER ||
            blk->kind == ON_BLOCK_CHECK)
            indent = LIST_INDENT;
        else if (blk->kind == ON_BLOCK_CODE)
            indent = L->style.code_numbers ? CODE_INSET_NUM : CODE_INSET;
        gboolean title = L->style.title_line && i == 0 &&
                         blk->kind == ON_BLOCK_PARA;
        b->x = LEFT_MARGIN + indent;
        b->w = MAX(8, cw - indent - (blk->kind == ON_BLOCK_CODE ? CODE_INSET
                                                                 : 0));
        b->layout = text_layout_new(L, blk->text, blk->kind, i, -1,
                                    on_block_is_action(blk), title, FALSE,
                                    b->w);
        gint pw, ph;
        pango_layout_get_pixel_size(b->layout, &pw, &ph);
        b->h = ph + BLOCK_GAP;
        if (blk->kind == ON_BLOCK_CODE) {
            /* The run this block belongs to, for numbering and the word.   */
            guint first = i;
            while (first > 0 &&
                   on_document_block(L->doc, first - 1)->kind == ON_BLOCK_CODE)
                first--;
            guint last = i;
            while (last + 1 < on_document_n_blocks(L->doc) &&
                   on_document_block(L->doc, last + 1)->kind == ON_BLOCK_CODE)
                last++;
            b->code_first = (gint)first;
            b->code_len   = (gint)(last - first + 1);
            if (i == first)
                b->h += CODE_PAD;
            if (i == last)
                b->h += CODE_PAD;
        }
    }
    b->valid = TRUE;
}

/* validate() — every block laid out, y offsets summed.                     */
static void
validate(OnDocLayout *L)
{
    if (L->geometry_valid)
        return;
    guint n = on_document_n_blocks(L->doc);
    if (L->bl->len != n)             /* a relay was missed: resync          */
        g_array_set_size(L->bl, n);
    gint y = TOP_MARGIN;
    for (guint i = 0; i < n; i++) {
        BL *b = &g_array_index(L->bl, BL, i);
        if (!b->valid)
            layout_block(L, i);
        b->y = y;
        y += b->h;
    }
    L->total_h = y - BLOCK_GAP + BOTTOM_MARGIN;
    L->geometry_valid = TRUE;
}

/* block_at_y() — the block whose vertical span holds y (clamped).           */
static guint
block_at_y(OnDocLayout *L, gdouble y)
{
    guint n = L->bl->len;
    if (n == 0)
        return 0;
    guint lo = 0, hi = n - 1;
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        const BL *b = &g_array_index(L->bl, BL, mid);
        if (y < b->y + b->h)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* text_top() — where a text block's layout starts vertically (a code run's
 * first line sits under its padding).                                       */
static gint
text_top(OnDocLayout *L, guint i)
{
    const BL *b = &g_array_index(L->bl, BL, i);
    return b->y + ((b->code_first == (gint)i) ? CODE_PAD : 0);
}

gint
on_doc_layout_height(OnDocLayout *L)
{
    validate(L);
    return L->total_h;
}

void
on_doc_layout_block_rect(OnDocLayout *L, guint i, graphene_rect_t *out)
{
    validate(L);
    if (i >= L->bl->len) {
        *out = GRAPHENE_RECT_INIT(0, 0, 0, 0);
        return;
    }
    const BL *b = &g_array_index(L->bl, BL, i);
    *out = GRAPHENE_RECT_INIT(LEFT_MARGIN, b->y, content_width(L),
                              b->h - BLOCK_GAP);
}

void
on_doc_layout_code_run(OnDocLayout *L, guint block, guint *first,
                       guint *count)
{
    validate(L);
    const BL *b = &g_array_index(L->bl, BL, block);
    *first = (b->code_first >= 0) ? (guint)b->code_first : block;
    *count = (b->code_len > 0) ? (guint)b->code_len : 1;
}

GdkTexture *
on_doc_layout_image_texture(OnDocLayout *L, guint block, gint inline_ord)
{
    OnBlock *blk = on_document_block(L->doc, block);
    if (blk == NULL)
        return NULL;
    if (blk->kind == ON_BLOCK_IMAGE)
        return image_texture(blk->png, &blk->pixels, &blk->pixels_free);
    if (blk->text == NULL || inline_ord < 0 ||
        (guint)inline_ord >= blk->text->images->len)
        return NULL;
    OnInlineImage *img = &g_array_index(blk->text->images, OnInlineImage,
                                        inline_ord);
    return image_texture(img->png, &img->pixels, &img->pixels_free);
}

/* ---------------------------------------------------------------------------
 * cell_origin() — where cell (r, c) of table block i starts, and its size.
 * ------------------------------------------------------------------------- */
static void
cell_origin(OnDocLayout *L, guint i, gint r, gint c, gint *x, gint *y,
            gint *w, gint *h)
{
    const BL *b = &g_array_index(L->bl, BL, i);
    gint cx = b->x + 1, cy = b->y + 1;
    for (gint k = 0; k < c; k++)
        cx += g_array_index(b->col_w, gint, k);
    for (gint k = 0; k < r; k++)
        cy += g_array_index(b->row_h, gint, k);
    *x = cx;
    *y = cy;
    *w = g_array_index(b->col_w, gint, c);
    *h = g_array_index(b->row_h, gint, r);
}

/* layout_for() — the PangoLayout and origin of a text position's text.     */
static PangoLayout *
layout_for(OnDocLayout *L, OnPos pos, gint *ox, gint *oy)
{
    const BL *b = &g_array_index(L->bl, BL, pos.block);
    const OnBlock *blk = on_document_block(L->doc, pos.block);
    if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0 && b->cells != NULL &&
        (guint)pos.cell < b->cells->len) {
        gint x, y, w, h;
        cell_origin(L, pos.block, pos.cell / blk->cols, pos.cell % blk->cols,
                    &x, &y, &w, &h);
        *ox = x + TABLE_PAD;
        *oy = y + TABLE_PAD;
        return g_ptr_array_index(b->cells, pos.cell);
    }
    if (b->layout == NULL)
        return NULL;
    *ox = b->x;
    *oy = text_top(L, pos.block);
    return b->layout;
}

void
on_doc_layout_caret_rect(OnDocLayout *L, OnPos pos, graphene_rect_t *out)
{
    validate(L);
    if (pos.block >= L->bl->len) {
        *out = GRAPHENE_RECT_INIT(LEFT_MARGIN, TOP_MARGIN, 1, 16);
        return;
    }
    const BL *b = &g_array_index(L->bl, BL, pos.block);
    const OnBlock *blk = on_document_block(L->doc, pos.block);
    gint ox, oy;
    PangoLayout *layout = layout_for(L, pos, &ox, &oy);
    if (layout == NULL) {
        /* An object: its left or right edge.                               */
        gint h = (blk->kind == ON_BLOCK_IMAGE) ? b->img_h : b->h - BLOCK_GAP;
        *out = GRAPHENE_RECT_INIT(b->x + (pos.offset > 0 ? b->w : -1),
                                  b->y, 1, h);
        return;
    }
    gsize idx = idx_of(L, pos.block, pos.cell, pos.offset);
    if (L->pre_on && L->pre_pos.block == pos.block &&
        L->pre_pos.cell == pos.cell && pos.offset == L->pre_pos.offset)
        idx = pos.offset + (gsize)L->pre_cursor;
    PangoRectangle strong;
    pango_layout_get_cursor_pos(layout, (gint)idx, &strong, NULL);
    gdouble x = ox + strong.x / (gdouble)PANGO_SCALE;
    /* After a line's last emoji: clear of the glyph's overdraw.            */
    if (idx > 0 && L->emoji_caret_pad > 0) {
        gint line, lx;
        pango_layout_index_to_line_x(layout, (gint)idx, FALSE, &line, &lx);
        PangoLayoutLine *ll = pango_layout_get_line_readonly(layout, line);
        const gchar *text = pango_layout_get_text(layout);
        if ((gsize)(ll->start_index + ll->length) == idx &&
            ends_with_emoji(text, idx))
            x += L->emoji_caret_pad;
    }
    *out = GRAPHENE_RECT_INIT(x, oy + strong.y / (gdouble)PANGO_SCALE, 1,
                              strong.height / (gdouble)PANGO_SCALE);
}

/* ---------------------------------------------------------------------------
 * xy_to_offset() — the document offset for a point inside a layout.
 * ------------------------------------------------------------------------- */
static gsize
xy_to_offset(OnDocLayout *L, PangoLayout *layout, guint block, gint cell,
             gint x, gint y)
{
    gint index, trailing;
    pango_layout_xy_to_index(layout, x * PANGO_SCALE, y * PANGO_SCALE,
                             &index, &trailing);
    const gchar *text = pango_layout_get_text(layout);
    const gchar *p = text + index;
    while (trailing-- > 0 && *p != '\0')
        p = g_utf8_next_char(p);
    return off_of(L, block, cell, (gsize)(p - text));
}

/* copy_word_rect() — where a code run's "copy" word sits, if it shows.     */
static gboolean
copy_word_rect(OnDocLayout *L, guint first, graphene_rect_t *out)
{
    if (!L->style.code_copy)
        return FALSE;
    if (L->copy_word == NULL) {
        L->copy_word = pango_layout_new(L->ctx);
        PangoFontDescription *fd = font_for(L, 0.85, FALSE);
        pango_layout_set_font_description(L->copy_word, fd);
        pango_font_description_free(fd);
        pango_layout_set_text(L->copy_word, COPY_WORD, -1);
    }
    gint w, h;
    pango_layout_get_pixel_size(L->copy_word, &w, &h);
    const BL *b = &g_array_index(L->bl, BL, first);
    *out = GRAPHENE_RECT_INIT(L->width - RIGHT_MARGIN - 6 - w, b->y + 2,
                              w + 4, h);
    return TRUE;
}

/* checkbox_rect() — the drawn box of a CHECK block.                         */
static void
checkbox_rect(OnDocLayout *L, guint i, graphene_rect_t *out)
{
    const BL *b = &g_array_index(L->bl, BL, i);
    PangoLayoutLine *line = pango_layout_get_line_readonly(b->layout, 0);
    PangoRectangle lr;
    pango_layout_line_get_pixel_extents(line, NULL, &lr);
    gint line_h = MAX(lr.height, CHECK_SIZE);
    *out = GRAPHENE_RECT_INIT(LEFT_MARGIN + LIST_INDENT - CHECK_SIZE - 8,
                              b->y + (line_h - CHECK_SIZE) / 2.0, CHECK_SIZE,
                              CHECK_SIZE);
}

/* image_ord_of() — the document ordinal of block i's inline image k, or
 * of the image block i.                                                     */
static gint
image_ord_of(OnDocLayout *L, guint i, gint k)
{
    gint ord = 0;
    for (guint j = 0; j < i; j++) {
        const OnBlock *blk = on_document_block(L->doc, j);
        if (blk->kind == ON_BLOCK_IMAGE)
            ord++;
        else if (blk->text != NULL)
            ord += (gint)blk->text->images->len;
    }
    return ord + MAX(k, 0);
}

void
on_doc_layout_hit(OnDocLayout *L, gdouble x, gdouble y, OnDocHitResult *out)
{
    validate(L);
    memset(out, 0, sizeof *out);
    out->kind = ON_HIT_TEXT;
    out->pos.cell = -1;
    out->image_ord = -1;
    guint n = L->bl->len;
    if (n == 0)
        return;
    guint i = block_at_y(L, y);
    const BL *b = &g_array_index(L->bl, BL, i);
    const OnBlock *blk = on_document_block(L->doc, i);
    out->block = i;
    out->pos.block = i;

    if (blk->kind == ON_BLOCK_IMAGE) {
        gboolean inside = x >= b->x && x < b->x + b->w && y >= b->y &&
                          y < b->y + b->img_h;
        out->pos.offset = (x < b->x + b->w / 2.0) ? 0 : 1;
        if (inside) {
            out->kind = ON_HIT_IMAGE;
            out->image_ord = image_ord_of(L, i, 0);
        }
        return;
    }
    if (blk->kind == ON_BLOCK_TABLE) {
        out->kind = ON_HIT_OBJECT;
        out->pos.offset = (x < b->x + b->w / 2.0) ? 0 : 1;
        gint cx, cy, cw, ch;
        for (gint r = 0; r < blk->rows; r++) {
            for (gint c = 0; c < blk->cols; c++) {
                cell_origin(L, i, r, c, &cx, &cy, &cw, &ch);
                if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
                    out->kind = ON_HIT_TEXT;
                    out->pos.cell = r * blk->cols + c;
                    PangoLayout *cell = g_ptr_array_index(b->cells,
                                                          out->pos.cell);
                    out->pos.offset = xy_to_offset(
                        L, cell, i, out->pos.cell, (gint)x - cx - TABLE_PAD,
                        (gint)y - cy - TABLE_PAD);
                    return;
                }
            }
        }
        return;
    }

    /* Text block.                                                          */
    graphene_rect_t r;
    if (b->code_first == (gint)i && copy_word_rect(L, i, &r) &&
        graphene_rect_contains_point(&r, &GRAPHENE_POINT_INIT(x, y))) {
        out->kind = ON_HIT_COPY;
        out->pos.offset = 0;
        return;
    }
    if (blk->kind == ON_BLOCK_CHECK) {
        checkbox_rect(L, i, &r);
        if (graphene_rect_contains_point(&r, &GRAPHENE_POINT_INIT(x, y))) {
            out->kind = ON_HIT_CHECKBOX;
            out->pos.offset = 0;
            return;
        }
    }
    gint ty = text_top(L, i);
    out->pos.offset = xy_to_offset(L, b->layout, i, -1, (gint)x - b->x,
                                   (gint)y - ty);
    /* On an inline image's box?                                            */
    for (guint k = 0; k < blk->text->images->len; k++) {
        const OnInlineImage *img =
            &g_array_index(blk->text->images, OnInlineImage, k);
        PangoRectangle pr;
        pango_layout_index_to_pos(b->layout,
                                  (gint)idx_of(L, i, -1, img->offset), &pr);
        graphene_rect_t ir = GRAPHENE_RECT_INIT(
            b->x + pr.x / (gdouble)PANGO_SCALE, ty + pr.y / (gdouble)PANGO_SCALE,
            pr.width / (gdouble)PANGO_SCALE, pr.height / (gdouble)PANGO_SCALE);
        if (graphene_rect_contains_point(&ir, &GRAPHENE_POINT_INIT(x, y))) {
            out->kind = ON_HIT_IMAGE;
            out->inline_image = TRUE;
            out->image_ord = image_ord_of(L, i, (gint)k);
            out->pos.offset = img->offset;
            return;
        }
    }
}

/* ===========================================================================
 * NAVIGATION
 * ======================================================================== */

/* text_end() — the end position of a text (block or cell).                  */
static gsize
text_len_at(OnDocLayout *L, OnPos pos)
{
    OnText *t = on_document_text_at(L->doc, pos);
    return (t != NULL) ? t->text->len : 0;
}

/* char_step() — the next (dir > 0) or previous cursor position in a text,
 * by Pango's log attrs (grapheme clusters).  Returns FALSE at the edge.    */
static gboolean
char_step(PangoLayout *layout, OnDocLayout *L, OnPos *pos, gint dir,
          gboolean word)
{
    gint n_attrs;
    const PangoLogAttr *la = pango_layout_get_log_attrs_readonly(layout,
                                                                 &n_attrs);
    const gchar *text = pango_layout_get_text(layout);
    gsize idx = idx_of(L, pos->block, pos->cell, pos->offset);
    /* Character index of idx.                                              */
    gint ci = 0;
    for (const gchar *p = text; (gsize)(p - text) < idx && *p != '\0';
         p = g_utf8_next_char(p))
        ci++;
    gint ni = ci;
    while (TRUE) {
        ni += dir;
        if (ni < 0 || ni >= n_attrs)
            return FALSE;
        if (word ? (dir > 0 ? la[ni].is_word_end : la[ni].is_word_start)
                 : la[ni].is_cursor_position)
            break;
    }
    const gchar *p = text;
    for (gint k = 0; k < ni; k++)
        p = g_utf8_next_char(p);
    pos->offset = off_of(L, pos->block, pos->cell, (gsize)(p - text));
    return TRUE;
}

/* block_end_pos() — the last position of block i.                           */
static OnPos
block_end_pos(OnDocLayout *L, guint i)
{
    OnPos p = { i, -1, 0 };
    const OnBlock *blk = on_document_block(L->doc, i);
    if (blk->kind == ON_BLOCK_TABLE) {
        p.cell = blk->rows * blk->cols - 1;
        p.offset = text_len_at(L, p);
    } else if (blk->kind == ON_BLOCK_IMAGE) {
        p.offset = 1;
    } else {
        p.offset = blk->text->text->len;
    }
    return p;
}

/* block_start_pos() — the first position of block i.                        */
static OnPos
block_start_pos(OnDocLayout *L, guint i)
{
    OnPos p = { i, -1, 0 };
    const OnBlock *blk = on_document_block(L->doc, i);
    if (blk->kind == ON_BLOCK_TABLE)
        p.cell = 0;
    return p;
}

/* line_of() — the visual line holding pos, as its index in the layout and
 * the caret's x in it.                                                      */
static void
line_of(OnDocLayout *L, PangoLayout *layout, OnPos pos, gint *line, gint *x)
{
    gsize idx = idx_of(L, pos.block, pos.cell, pos.offset);
    pango_layout_index_to_line_x(layout, (gint)idx, FALSE, line, x);
}

/* pos_in_line() — the position at x (Pango units) on a layout's line.      */
static gsize
pos_in_line(OnDocLayout *L, PangoLayout *layout, guint block, gint cell,
            gint line, gint x)
{
    PangoLayoutLine *ll = pango_layout_get_line_readonly(layout, line);
    gint index, trailing;
    pango_layout_line_x_to_index(ll, x, &index, &trailing);
    const gchar *text = pango_layout_get_text(layout);
    const gchar *p = text + index;
    while (trailing-- > 0 && *p != '\0')
        p = g_utf8_next_char(p);
    return off_of(L, block, cell, (gsize)(p - text));
}

OnPos
on_doc_layout_move(OnDocLayout *L, OnPos pos, OnDocMove how, gint *goal_x)
{
    validate(L);
    guint n = L->bl->len;
    if (pos.block >= n)
        return pos;
    const OnBlock *blk = on_document_block(L->doc, pos.block);
    gint ox = 0, oy = 0;
    PangoLayout *layout = layout_for(L, pos, &ox, &oy);
    OnPos out = pos;

    switch (how) {
    case ON_MOVE_CHAR_LEFT:
    case ON_MOVE_WORD_LEFT:
        *goal_x = -1;
        if (layout != NULL &&
            char_step(layout, L, &out, -1, how == ON_MOVE_WORD_LEFT))
            return out;
        if (layout == NULL && pos.offset > 0) {   /* object: before it     */
            out.offset = 0;
            return out;
        }
        if (blk->kind == ON_BLOCK_TABLE && pos.cell > 0) {
            out.cell = pos.cell - 1;
            out.offset = text_len_at(L, out);
            return out;
        }
        if (pos.block == 0)
            return pos;
        return block_end_pos(L, pos.block - 1);

    case ON_MOVE_CHAR_RIGHT:
    case ON_MOVE_WORD_RIGHT:
        *goal_x = -1;
        if (layout != NULL &&
            char_step(layout, L, &out, +1, how == ON_MOVE_WORD_RIGHT))
            return out;
        if (layout == NULL && pos.offset == 0) {  /* object: after it      */
            out.offset = 1;
            return out;
        }
        if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0 &&
            pos.cell + 1 < blk->rows * blk->cols) {
            out.cell = pos.cell + 1;
            out.offset = 0;
            return out;
        }
        if (pos.block + 1 >= n)
            return pos;
        return block_start_pos(L, pos.block + 1);

    case ON_MOVE_LINE_START:
    case ON_MOVE_LINE_END: {
        *goal_x = -1;
        if (layout == NULL) {
            out.offset = (how == ON_MOVE_LINE_START) ? 0 : 1;
            return out;
        }
        gint line, x;
        line_of(L, layout, pos, &line, &x);
        PangoLayoutLine *ll = pango_layout_get_line_readonly(layout, line);
        gsize s = (gsize)ll->start_index;
        gsize e = s + (gsize)ll->length;
        out.offset = off_of(L, pos.block, pos.cell,
                            how == ON_MOVE_LINE_START ? s : e);
        return out;
    }

    case ON_MOVE_LINE_UP:
    case ON_MOVE_LINE_DOWN: {
        gint dir = (how == ON_MOVE_LINE_DOWN) ? 1 : -1;
        if (layout != NULL) {
            gint line, x;
            line_of(L, layout, pos, &line, &x);
            if (*goal_x < 0)
                *goal_x = ox + x / PANGO_SCALE;
            gint target = line + dir;
            if (target >= 0 && target < pango_layout_get_line_count(layout)) {
                out.offset = pos_in_line(L, layout, pos.block, pos.cell,
                                         target, (*goal_x - ox) * PANGO_SCALE);
                return out;
            }
            /* Out of this layout: the next cell row, or the next block.   */
            if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0) {
                gint r = pos.cell / blk->cols + dir, c = pos.cell % blk->cols;
                if (r >= 0 && r < blk->rows) {
                    out.cell = r * blk->cols + c;
                    PangoLayout *cl; gint cx, cy;
                    cl = layout_for(L, out, &cx, &cy);
                    gint lines = pango_layout_get_line_count(cl);
                    out.offset = pos_in_line(L, cl, out.block, out.cell,
                                             dir > 0 ? 0 : lines - 1,
                                             (*goal_x - cx) * PANGO_SCALE);
                    return out;
                }
            }
        } else if (*goal_x < 0) {
            const BL *b = &g_array_index(L->bl, BL, pos.block);
            *goal_x = b->x + (pos.offset > 0 ? b->w : 0);
        }
        /* Into the neighbouring block.                                     */
        if ((dir < 0 && pos.block == 0) || (dir > 0 && pos.block + 1 >= n))
            return pos;
        guint j = pos.block + dir;
        const OnBlock *nb = on_document_block(L->doc, j);
        out.block = j;
        out.cell  = -1;
        if (nb->kind == ON_BLOCK_IMAGE) {
            const BL *b = &g_array_index(L->bl, BL, j);
            out.offset = (*goal_x > b->x + b->w / 2) ? 1 : 0;
            return out;
        }
        if (nb->kind == ON_BLOCK_TABLE) {
            /* The nearest column, top or bottom row.                       */
            gint r = dir > 0 ? 0 : nb->rows - 1, c = 0;
            gint cx, cy, cw, ch;
            for (c = 0; c < nb->cols; c++) {
                cell_origin(L, j, r, c, &cx, &cy, &cw, &ch);
                if (*goal_x < cx + cw)
                    break;
            }
            c = MIN(c, nb->cols - 1);
            out.cell = r * nb->cols + c;
            PangoLayout *cl;
            cl = layout_for(L, out, &cx, &cy);
            gint lines = pango_layout_get_line_count(cl);
            out.offset = pos_in_line(L, cl, j, out.cell,
                                     dir > 0 ? 0 : lines - 1,
                                     (*goal_x - cx) * PANGO_SCALE);
            return out;
        }
        PangoLayout *nl;
        gint nx, ny;
        nl = layout_for(L, out, &nx, &ny);
        gint lines = pango_layout_get_line_count(nl);
        out.offset = pos_in_line(L, nl, j, -1, dir > 0 ? 0 : lines - 1,
                                 (*goal_x - nx) * PANGO_SCALE);
        return out;
    }

    case ON_MOVE_BLOCK_START:
        *goal_x = -1;
        if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0) {
            out.offset = 0;
            return out;
        }
        return block_start_pos(L, pos.block);
    case ON_MOVE_BLOCK_END:
        *goal_x = -1;
        if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0) {
            out.offset = text_len_at(L, pos);
            return out;
        }
        return block_end_pos(L, pos.block);
    case ON_MOVE_DOC_START:
        *goal_x = -1;
        return block_start_pos(L, 0);
    case ON_MOVE_DOC_END:
        *goal_x = -1;
        return block_end_pos(L, n - 1);
    }
    return pos;
}

void
on_doc_layout_word_at(OnDocLayout *L, OnPos pos, OnPos *a, OnPos *b)
{
    validate(L);
    *a = *b = pos;
    gint ox, oy;
    PangoLayout *layout = layout_for(L, pos, &ox, &oy);
    if (layout == NULL) {
        a->offset = 0;
        b->offset = 1;
        return;
    }
    gint n_attrs;
    const PangoLogAttr *la = pango_layout_get_log_attrs_readonly(layout,
                                                                 &n_attrs);
    const gchar *text = pango_layout_get_text(layout);
    gsize idx = idx_of(L, pos.block, pos.cell, pos.offset);
    gint ci = 0;
    for (const gchar *p = text; (gsize)(p - text) < idx && *p != '\0';
         p = g_utf8_next_char(p))
        ci++;
    gint s = ci, e = ci;
    while (s > 0 && !la[s].is_word_start)
        s--;
    while (e < n_attrs - 1 && !la[e].is_word_end)
        e++;
    const gchar *p = text;
    for (gint k = 0; k < s; k++)
        p = g_utf8_next_char(p);
    a->offset = off_of(L, pos.block, pos.cell, (gsize)(p - text));
    for (gint k = s; k < e; k++)
        p = g_utf8_next_char(p);
    b->offset = off_of(L, pos.block, pos.cell, (gsize)(p - text));
}

void
on_doc_layout_line_at(OnDocLayout *L, OnPos pos, OnPos *a, OnPos *b)
{
    gint gx = -1;
    *a = on_doc_layout_move(L, pos, ON_MOVE_LINE_START, &gx);
    *b = on_doc_layout_move(L, pos, ON_MOVE_LINE_END, &gx);
}

/* ===========================================================================
 * PAINTING
 * ======================================================================== */

/* fill() — a solid rectangle.                                               */
static void
fill(GtkSnapshot *snap, const GdkRGBA *c, gdouble x, gdouble y, gdouble w,
     gdouble h)
{
    gtk_snapshot_append_color(snap, c, &GRAPHENE_RECT_INIT(x, y, w, h));
}

/* draw_layout() — a PangoLayout at (x, y) in `fg`.                          */
static void
draw_layout(GtkSnapshot *snap, PangoLayout *layout, gdouble x, gdouble y,
            const GdkRGBA *fg)
{
    gtk_snapshot_save(snap);
    gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(x, y));
    gtk_snapshot_append_layout(snap, layout, fg);
    gtk_snapshot_restore(snap);
}

/* draw_selection_in_layout() — the selection's [s, e) byte span of a
 * layout as filled rectangles, line by line.                                */
static void
draw_selection_in_layout(GtkSnapshot *snap, PangoLayout *layout, gint ox,
                         gint oy, gsize s, gsize e, const GdkRGBA *c)
{
    PangoLayoutIter *it = pango_layout_get_iter(layout);
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
        gsize ls = (gsize)line->start_index, le = ls + (gsize)line->length;
        PangoRectangle ext;
        pango_layout_iter_get_line_extents(it, NULL, &ext);
        if (s <= ls && e >= le && le == ls) {
            /* An empty line inside the selection: a token.                */
            fill(snap, c, ox + ext.x / (gdouble)PANGO_SCALE,
                 oy + ext.y / (gdouble)PANGO_SCALE, 4,
                 ext.height / (gdouble)PANGO_SCALE);
            continue;
        }
        gsize a = MAX(s, ls), b = MIN(e, le);
        if (a >= b)
            continue;
        gint *ranges, n;
        pango_layout_line_get_x_ranges(line, (gint)a, (gint)b, &ranges, &n);
        for (gint k = 0; k < n; k++)
            fill(snap, c, ox + ranges[2 * k] / (gdouble)PANGO_SCALE,
                 oy + ext.y / (gdouble)PANGO_SCALE,
                 (ranges[2 * k + 1] - ranges[2 * k]) / (gdouble)PANGO_SCALE,
                 ext.height / (gdouble)PANGO_SCALE);
        g_free(ranges);
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
}

/* sel_span() — the selected byte span of block i's text (cell -1) or of a
 * cell, given the ordered selection; FALSE when none of it is selected.
 * `whole` says the block is inside the selection as a unit.                */
static gboolean
sel_span(OnDocLayout *L, const OnDocPaint *p, guint i, gint cell, gsize len,
         gsize *s, gsize *e, gboolean *whole)
{
    *whole = FALSE;
    if (!p->has_sel || i < p->sel_a.block || i > p->sel_b.block)
        return FALSE;
    gboolean first = i == p->sel_a.block, last = i == p->sel_b.block;
    if (first && last && p->sel_a.cell >= 0 && p->sel_a.cell == p->sel_b.cell) {
        if (cell != p->sel_a.cell)
            return FALSE;
        *s = p->sel_a.offset;
        *e = p->sel_b.offset;
        return *s < *e;
    }
    if (cell >= 0) {                 /* a cell of a table taken whole       */
        *whole = TRUE;
        *s = 0;
        *e = len;
        return TRUE;
    }
    *s = first ? p->sel_a.offset : 0;
    *e = last  ? p->sel_b.offset : len;
    if (!first && !last)
        *whole = TRUE;
    (void)L;
    return TRUE;
}

/* prefix_layout() — the bullet or number glyph for a list block.            */
static PangoLayout *
prefix_layout(OnDocLayout *L, const gchar *text)
{
    PangoLayout *l = pango_layout_new(L->ctx);
    pango_layout_set_font_description(l,
                                      pango_context_get_font_description(L->ctx));
    pango_layout_set_text(l, text, -1);
    return l;
}

void
on_doc_layout_snapshot(OnDocLayout *L, GtkSnapshot *snap, const OnDocPaint *p)
{
    validate(L);
    guint n = L->bl->len;
    if (n == 0)
        return;
    const GdkRGBA *selc = p->focused ? &C_SEL : &C_SEL_DIM;
    gint number_run = 0;             /* consecutive NUMBER blocks           */
    guint first = block_at_y(L, p->clip.origin.y);
    /* Numbering needs the run's start, which may be above the clip.       */
    for (guint i = first; i > 0; i--) {
        if (on_document_block(L->doc, i - 1)->kind != ON_BLOCK_NUMBER)
            break;
        number_run++;
    }

    for (guint i = first; i < n; i++) {
        const BL *b = &g_array_index(L->bl, BL, i);
        if (b->y > p->clip.origin.y + p->clip.size.height)
            break;
        const OnBlock *blk = on_document_block(L->doc, i);
        number_run = (blk->kind == ON_BLOCK_NUMBER) ? number_run + 1 : 0;
        gboolean whole;
        gsize s, e;

        if (blk->kind == ON_BLOCK_IMAGE) {
            GdkTexture *tex = image_texture(blk->png, &((OnBlock *)blk)->pixels,
                                            &((OnBlock *)blk)->pixels_free);
            graphene_rect_t r = GRAPHENE_RECT_INIT(b->x, b->y, b->w, b->img_h);
            if (tex != NULL)
                gtk_snapshot_append_scaled_texture(
                    snap, tex, GSK_SCALING_FILTER_TRILINEAR, &r);
            else
                fill(snap, &C_CODE_BG, b->x, b->y, b->w, b->img_h);
            if (sel_span(L, p, i, -1, 1, &s, &e, &whole) && (whole || (s == 0 && e == 1)))
                gtk_snapshot_append_color(snap, selc, &r);
            continue;
        }

        if (blk->kind == ON_BLOCK_TABLE) {
            gint cx, cy, cw, ch;
            for (gint r = 0; r < blk->rows; r++) {
                for (gint c = 0; c < blk->cols; c++) {
                    cell_origin(L, i, r, c, &cx, &cy, &cw, &ch);
                    if (blk->header && r == 0)
                        fill(snap, &C_HEADER, cx, cy, cw, ch);
                    gint cell = r * blk->cols + c;
                    PangoLayout *cl = g_ptr_array_index(b->cells, cell);
                    gsize clen = strlen(pango_layout_get_text(cl));
                    if (sel_span(L, p, i, cell, clen, &s, &e, &whole)) {
                        if (whole)
                            fill(snap, selc, cx, cy, cw, ch);
                        else
                            draw_selection_in_layout(
                                snap, cl, cx + TABLE_PAD, cy + TABLE_PAD,
                                idx_of(L, i, cell, s), idx_of(L, i, cell, e),
                                selc);
                    }
                    draw_layout(snap, cl, cx + TABLE_PAD, cy + TABLE_PAD,
                                &p->fg);
                    /* Borders: every cell draws its top and left, the
                     * table its bottom and right.                          */
                    fill(snap, &C_TABLE_BD, cx - 1, cy - 1, cw + 1, 1);
                    fill(snap, &C_TABLE_BD, cx - 1, cy - 1, 1, ch + 1);
                }
            }
            fill(snap, &C_TABLE_BD, b->x, b->y + b->h - BLOCK_GAP - 1, b->w, 1);
            fill(snap, &C_TABLE_BD, b->x + b->w - 1, b->y, 1,
                 b->h - BLOCK_GAP);
            if (sel_span(L, p, i, -1, 1, &s, &e, &whole) &&
                (whole || (s == 0 && e == 1)) &&
                !(p->sel_a.block == i && p->sel_a.cell >= 0))
                fill(snap, selc, b->x, b->y, b->w, b->h - BLOCK_GAP);
            continue;
        }

        /* Text kinds.                                                      */
        gint ty = text_top(L, i);
        if (blk->kind == ON_BLOCK_CODE) {
            fill(snap, &C_CODE_BG, LEFT_MARGIN, b->y, content_width(L),
                 b->h - ((b->code_first + b->code_len - 1 == (gint)i)
                         ? BLOCK_GAP : 0));
            if (L->style.code_numbers) {
                gchar num[16];
                g_snprintf(num, sizeof num, "%d",
                           (gint)i - b->code_first + 1);
                PangoLayout *nl = pango_layout_new(L->ctx);
                PangoFontDescription *fd = font_for(L, 0.8, TRUE);
                pango_layout_set_font_description(nl, fd);
                pango_font_description_free(fd);
                pango_layout_set_text(nl, num, -1);
                gint nw, nh;
                pango_layout_get_pixel_size(nl, &nw, &nh);
                draw_layout(snap, nl, b->x - 8 - nw, ty + 1, &C_CODE_NUM);
                g_object_unref(nl);
            }
        }
        if (sel_span(L, p, i, -1, blk->text->text->len, &s, &e, &whole))
            draw_selection_in_layout(snap, b->layout, b->x, ty,
                                     idx_of(L, i, -1, s), idx_of(L, i, -1, e),
                                     selc);
        if (blk->kind == ON_BLOCK_BULLET) {
            if (L->bullet == NULL)
                L->bullet = prefix_layout(L, "\xe2\x80\xa2");
            gint pw, ph;
            pango_layout_get_pixel_size(L->bullet, &pw, &ph);
            draw_layout(snap, L->bullet, b->x - 12 - pw, ty, &p->fg);
        } else if (blk->kind == ON_BLOCK_NUMBER) {
            gchar num[16];
            g_snprintf(num, sizeof num, "%d.", number_run);
            PangoLayout *nl = prefix_layout(L, num);
            gint pw, ph;
            pango_layout_get_pixel_size(nl, &pw, &ph);
            draw_layout(snap, nl, b->x - 8 - pw, ty, &p->fg);
            g_object_unref(nl);
        } else if (blk->kind == ON_BLOCK_CHECK) {
            graphene_rect_t r;
            checkbox_rect(L, i, &r);
            GskRoundedRect rr;
            gsk_rounded_rect_init_from_rect(&rr, &r, 3);
            if (blk->checked) {
                gtk_snapshot_push_rounded_clip(snap, &rr);
                gtk_snapshot_append_color(snap, &C_CHECK, &r);
                gtk_snapshot_pop(snap);
                /* The tick: two strokes.                                   */
                gdouble x0 = r.origin.x, y0 = r.origin.y;
                fill(snap, &C_WHITE, x0 + 3, y0 + 7, 3, 2);
                fill(snap, &C_WHITE, x0 + 5, y0 + 8, 2, 3);
                fill(snap, &C_WHITE, x0 + 6, y0 + 7, 2, 2);
                fill(snap, &C_WHITE, x0 + 7, y0 + 5, 2, 2);
                fill(snap, &C_WHITE, x0 + 8, y0 + 4, 3, 2);
            } else {
                GdkRGBA border[4] = { C_TABLE_BD, C_TABLE_BD, C_TABLE_BD,
                                      C_TABLE_BD };
                float widths[4] = { 1, 1, 1, 1 };
                gtk_snapshot_push_rounded_clip(snap, &rr);
                gtk_snapshot_append_color(snap, &C_WHITE, &r);
                gtk_snapshot_pop(snap);
                gtk_snapshot_append_border(snap, &rr, widths, border);
            }
        }
        draw_layout(snap, b->layout, b->x, ty, &p->fg);

        /* Inline images over their boxes.                                 */
        for (guint k = 0; k < blk->text->images->len; k++) {
            OnInlineImage *img =
                &g_array_index(blk->text->images, OnInlineImage, k);
            GdkTexture *tex = image_texture(img->png, &img->pixels,
                                            &img->pixels_free);
            PangoRectangle pr;
            pango_layout_index_to_pos(b->layout,
                                      (gint)idx_of(L, i, -1, img->offset), &pr);
            graphene_rect_t r = GRAPHENE_RECT_INIT(
                b->x + pr.x / (gdouble)PANGO_SCALE,
                ty + pr.y / (gdouble)PANGO_SCALE,
                pr.width / (gdouble)PANGO_SCALE,
                pr.height / (gdouble)PANGO_SCALE);
            if (tex != NULL)
                gtk_snapshot_append_scaled_texture(
                    snap, tex, GSK_SCALING_FILTER_TRILINEAR, &r);
        }

        /* The "copy" word on the first block of a code run.               */
        graphene_rect_t cr;
        if (b->code_first == (gint)i && copy_word_rect(L, i, &cr))
            draw_layout(snap, L->copy_word, cr.origin.x + 2, cr.origin.y,
                        &C_ACTION);
    }

    if (p->caret_visible) {
        graphene_rect_t r;
        on_doc_layout_caret_rect(L, p->caret, &r);
        gtk_snapshot_append_color(snap, &p->fg, &r);
    }
}
