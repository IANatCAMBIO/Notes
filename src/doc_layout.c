/* ===========================================================================
 * doc_layout.c — OnDocLayout: the geometry of an OnDocument on screen.
 * See doc_layout.h for the contract; the GTK build's src/doc_layout.c
 * is the behavioural specification.  Pango/GSK calls are replaced here with
 * text_layout.h (CoreText) and paint.h (CoreGraphics).
 * =========================================================================== */

#include "doc_layout.h"
#include "text_layout.h"
#include "paint.h"
#include "image_io.h"
#include "utf_index.h"
#include "look.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Page geometry (points) ─────────────────────────────────────────────── */

#define LEFT_MARGIN   ((double)on_look_margin_left)
#define RIGHT_MARGIN  ((double)on_look_margin_right)
#define TOP_MARGIN    ((double)on_look_margin_top)
#define BOTTOM_MARGIN ((double)on_look_margin_bottom)
#define BLOCK_GAP     ((double)on_look_block_gap)

#define LIST_INDENT   ((double)on_look_list_indent)
#define CODE_INSET    ((double)on_look_code_inset)
#define CODE_INSET_NUM ((double)on_look_code_inset_numbered)
#define CODE_PAD      ((double)on_look_code_pad)
#define TABLE_PAD     ((double)on_look_table_pad)
#define TABLE_COL_MAX ((double)on_look_table_col_max)
#define TABLE_COL_MIN ((double)on_look_table_col_min)
#define CHECK_SIZE    ((double)on_look_checkbox_size)

/* ── BL — one block's layout ────────────────────────────────────────────── */

typedef struct {
    bool                valid;
    on_text_layout_t   *layout;     /* text blocks                           */
    double              y;          /* top, content coordinates              */
    double              h;          /* height INCLUDING BLOCK_GAP            */
    double              x;          /* left of text / object                 */
    double              w;          /* layout width / object width           */
    double              img_h;      /* IMAGE block height                    */
    /* TABLE */
    double             *col_w;      /* [cols]                                */
    double             *row_h;      /* [rows]                                */
    on_text_layout_t  **cells;      /* [n_cells]                             */
    size_t              n_cells;    /* rows*cols when cells was made: the
                                       document may have reshaped since     */
} BL;

struct OnDocLayout {
    OnDocument       *doc;
    double            width;
    double            font_size;
    OnDocLayoutStyle  style;
    BL               *bl;           /* [n_blocks]                            */
    size_t            n_bl;
    bool              geometry_valid;
    double            total_h;
    /* preedit */
    bool              pre_on;
    OnPos             pre_pos;
    char             *pre_text;
    size_t            pre_cursor;
    /* find hits */
    OnDocHit         *hits;
    size_t            n_hits;
    /* the code runs' "copy" word, made when first needed for the style */
    on_text_layout_t *copy_word;
};

/* ── BL helpers ─────────────────────────────────────────────────────────── */

/*
 * bl_clear — release what a BL holds (not the struct itself).  It reads
 * nothing from the document: by the time a block is cleared the document
 * may already have reshaped, moved or removed it.
 *
 * Inputs:
 *   b — the block layout
 */
static void
bl_clear(BL *b)
{
    on_text_layout_free(b->layout);
    b->layout = NULL;
    if (b->cells) {
        for (size_t k = 0; k < b->n_cells; k++)
            on_text_layout_free(b->cells[k]);
        free(b->cells);
        b->cells   = NULL;
        b->n_cells = 0;
    }
    free(b->col_w);
    b->col_w = NULL;
    free(b->row_h);
    b->row_h = NULL;
    b->valid = false;
}

/*
 * bl_clear_i — clear the BL at index i.
 *
 * Inputs:
 *   L — the layout
 *   i — block index
 */
static void
bl_clear_i(OnDocLayout *L, size_t i)
{
    if (i < L->n_bl) bl_clear(&L->bl[i]);
}

/* ── Code-run helpers ───────────────────────────────────────────────────── */

/*
 * code_run_first — for a CODE block, the index of its run's first block.
 */
static size_t
code_run_first(const OnDocLayout *L, size_t i)
{
    while (i > 0 &&
           on_document_block(L->doc, (guint)(i - 1))->kind == ON_BLOCK_CODE)
        i--;
    return i;
}

/*
 * code_run_last — for a CODE block, the index of its run's last block.
 */
static size_t
code_run_last(const OnDocLayout *L, size_t i)
{
    size_t n = (size_t)on_document_n_blocks(L->doc);
    while (i + 1 < n &&
           on_document_block(L->doc, (guint)(i + 1))->kind == ON_BLOCK_CODE)
        i++;
    return i;
}

/*
 * code_run_starts — is block i the first block of its code run?
 */
static bool
code_run_starts(const OnDocLayout *L, size_t i)
{
    return on_document_block(L->doc, (guint)i)->kind == ON_BLOCK_CODE &&
           (i == 0 ||
            on_document_block(L->doc, (guint)(i - 1))->kind != ON_BLOCK_CODE);
}

/* ── content_width ──────────────────────────────────────────────────────── */

/*
 * content_width — the usable width between margins.
 */
static double
content_width(const OnDocLayout *L)
{
    double w = L->width - LEFT_MARGIN - RIGHT_MARGIN;
    return w < 40.0 ? 40.0 : w;
}

/* ── image_fit ──────────────────────────────────────────────────────────── */

/*
 * image_fit — the on-screen size of an image: the stored width, or the
 * default thumbnail (aspect-kept, never upscaled).
 *
 * Inputs:
 *   L             — the layout
 *   src_w, src_h  — the image's pixel size
 *   display_width — stored width override (0 = use thumbnail defaults)
 *   out_w, out_h  — receive the display size
 *
 * Output: none.
 */
static void
image_fit(const OnDocLayout *L, int src_w, int src_h, guint32 display_width,
          double *out_w, double *out_h)
{
    double cw  = content_width(L);
    double want;
    if (display_width > 0) {
        want = (double)display_width < (double)src_w ? (double)display_width
                                                      : (double)src_w;
    } else {
        want = (double)src_w < on_look_image_thumb_w ? (double)src_w : on_look_image_thumb_w;
        if (src_h > 0 && (double)src_h * want > on_look_image_thumb_h * (double)src_w)
            want = (double)src_w * on_look_image_thumb_h / (double)src_h;
    }
    want = want > cw ? cw : (want < 1.0 ? 1.0 : want);
    *out_w = want;
    *out_h = src_h > 0 ? (want * (double)src_h / (double)src_w) : want;
    if (*out_h < 1.0) *out_h = 1.0;
}

/* ── preedit helpers ────────────────────────────────────────────────────── */

/*
 * idx_of — adjust a block text offset for an active preedit splice.
 * Offsets at or after the preedit insertion point shift by pre_len.
 */
static size_t
idx_of(const OnDocLayout *L, size_t block, int cell, size_t off)
{
    if (L->pre_on && L->pre_pos.block == block &&
        L->pre_pos.cell == cell && off >= L->pre_pos.offset)
        return off + strlen(L->pre_text);
    return off;
}

/*
 * off_of — the reverse: map a spliced index back to the original offset.
 */
static size_t
off_of(const OnDocLayout *L, size_t block, int cell, size_t idx)
{
    if (L->pre_on && L->pre_pos.block == block &&
        L->pre_pos.cell == cell && idx > L->pre_pos.offset) {
        size_t n = strlen(L->pre_text);
        return (idx >= L->pre_pos.offset + n) ? idx - n : L->pre_pos.offset;
    }
    return idx;
}

/* ── Building on_text_content_t ─────────────────────────────────────────── */

/* A text's content for on_text_layout_new, and the buffers it points into. */
typedef struct {
    on_text_content_t c;
    char             *text_buf;     /* the preedit-spliced text, or NULL     */
    on_text_span_t   *hits;         /* this text's find highlights           */
    on_text_box_t    *boxes;        /* its inline images' boxes              */
} content_t;

/*
 * make_content — describe a text or a table cell to the text layout: its
 * text (with the input method's preedit spliced in), its runs, its find
 * highlights and a box for each inline image, sized as GTK sizes them
 * (image_fit; 16 pt for an image that cannot be read).
 *
 * Inputs:
 *   L           — the layout
 *   t           — the OnText to describe
 *   block, cell — where it is (for the preedit and the hits)
 *   out         — receives the content; content_free it
 */
static void
make_content(const OnDocLayout *L, const OnText *t, size_t block, int cell, content_t *out)
{
    *out = (content_t){ 0 };
    on_text_content_t *c = &out->c;

    /* Preedit splice. */
    bool pre = L->pre_on && (size_t)L->pre_pos.block == block &&
               L->pre_pos.cell == cell;
    size_t pre_len = pre ? strlen(L->pre_text) : 0;
    if (pre) {
        size_t base_len = t->text->len;
        size_t ins = L->pre_pos.offset < base_len ? L->pre_pos.offset : base_len;
        char *buf = malloc(base_len + pre_len + 1);
        memcpy(buf, t->text->str, ins);
        memcpy(buf + ins, L->pre_text, pre_len);
        memcpy(buf + ins + pre_len, t->text->str + ins, base_len - ins);
        buf[base_len + pre_len] = '\0';
        out->text_buf  = buf;
        c->text        = buf;
        c->len         = base_len + pre_len;
        c->marked.start = ins;
        c->marked.len   = pre_len;
    } else {
        c->text = t->text->str;
        c->len  = t->text->len;
    }

    /* Runs. */
    c->runs   = (const OnRun *)t->runs->data;
    c->n_runs = (size_t)t->runs->len;

    /* Inline images: a box each, at the object character's offset. */
    guint n_img = t->images ? t->images->len : 0;
    if (n_img > 0) {
        out->boxes = calloc(n_img, sizeof *out->boxes);
        for (guint k = 0; k < n_img; k++) {
            const OnInlineImage *img = &g_array_index(t->images, OnInlineImage, k);
            int    iw = 0, ih = 0;
            double w = 16.0, h = 16.0;
            if (on_image_probe_size(g_bytes_get_data(img->png, NULL),
                                    (size_t)g_bytes_get_size(img->png), &iw, &ih))
                image_fit(L, iw, ih, img->display_width, &w, &h);
            out->boxes[k] = (on_text_box_t){ idx_of(L, block, cell, img->offset), w, h };
        }
        c->boxes   = out->boxes;
        c->n_boxes = n_img;
    }

    /* Find hits on this block/cell. */
    size_t n_h = 0;
    for (size_t k = 0; k < L->n_hits; k++)
        if ((size_t)L->hits[k].block == block && L->hits[k].cell == cell) n_h++;
    if (n_h > 0) {
        out->hits = calloc(n_h, sizeof *out->hits);
        size_t j = 0;
        for (size_t k = 0; k < L->n_hits; k++) {
            const OnDocHit *h = &L->hits[k];
            if ((size_t)h->block == block && h->cell == cell)
                out->hits[j++] = (on_text_span_t){ idx_of(L, block, cell, h->start), h->len };
        }
        c->hits   = out->hits;
        c->n_hits = n_h;
    }
}

/*
 * content_free — release what make_content allocated.
 *
 * Inputs:
 *   content — the content
 */
static void
content_free(content_t *content)
{
    free(content->text_buf);
    free(content->hits);
    free(content->boxes);
}

/* ── text_style_for ─────────────────────────────────────────────────────── */

/*
 * text_style_for — fill a style for a text block.
 *
 * Inputs:
 *   L     — the layout
 *   blk   — the block
 *   block — block index
 *   cell  — cell index (-1 for block text)
 *   width — wrap width
 *   header_cell — true for a table header cell
 *   no_wrap     — true for table column-width measurement
 *
 * Output: the filled style.
 */
static on_text_style_t
text_style_for(const OnDocLayout *L, const OnBlock *blk, size_t block,
               int cell, double width, bool header_cell, bool no_wrap)
{
    on_text_style_t st = { 0 };
    st.font_size = L->font_size;
    st.dark      = L->style.dark;
    bool is_h1   = blk->kind == ON_BLOCK_H1;
    bool is_h2   = blk->kind == ON_BLOCK_H2;
    bool is_code = blk->kind == ON_BLOCK_CODE;
    bool title   = L->style.title_line && block == 0 &&
                   cell == -1 && blk->kind == ON_BLOCK_PARA;
    st.scale    = (title || is_h1) ? on_look_h1_scale
                : is_h2            ? on_look_h2_scale : 1.0;
    st.mono     = is_code;
    st.bold     = title || is_h1 || is_h2 || header_cell;
    st.centered = title;
    st.action   = (cell == -1) && on_block_is_action((OnBlock *)blk);
    st.plain    = is_code;
    st.width    = no_wrap ? 0.0 : width;
    return st;
}

/* ── layout_block ───────────────────────────────────────────────────────── */

/*
 * layout_block — lay block i out into its BL.
 *
 * Inputs:
 *   L — the layout
 *   i — block index
 *
 * Output: none.
 */
static void
layout_block(OnDocLayout *L, size_t i)
{
    bl_clear_i(L, i);
    BL *b         = &L->bl[i];
    const OnBlock *blk = on_document_block(L->doc, (guint)i);
    double cw     = content_width(L);

    if (blk->kind == ON_BLOCK_IMAGE) {
        int iw = 24, ih = 24;
        on_image_probe_size(g_bytes_get_data(blk->png, NULL),
                            (size_t)g_bytes_get_size(blk->png), &iw, &ih);
        double fw, fh;
        image_fit(L, iw, ih, blk->display_width, &fw, &fh);
        b->x     = LEFT_MARGIN;
        b->w     = fw;
        b->img_h = fh;
        b->h     = fh + BLOCK_GAP;
        b->valid = true;
        return;
    }

    if (blk->kind == ON_BLOCK_TABLE) {
        b->col_w = calloc((size_t)blk->cols, sizeof *b->col_w);
        b->row_h = calloc((size_t)blk->rows, sizeof *b->row_h);
        b->n_cells = (size_t)(blk->rows * blk->cols);
        b->cells   = calloc(b->n_cells, sizeof *b->cells);
        /* Measure each column (no wrap). */
        for (int c = 0; c < blk->cols; c++) {
            double widest = TABLE_COL_MIN;
            for (int r = 0; r < blk->rows; r++) {
                OnText *cell_t = on_block_cell(blk, r, c);
                bool hdr = blk->header && r == 0;
                content_t ct;
                make_content(L, cell_t, i, r * blk->cols + c, &ct);
                on_text_style_t st = text_style_for(L, blk, i,
                    r * blk->cols + c, 0.0, hdr, true);
                on_text_layout_t *probe = on_text_layout_new(&ct.c, &st);
                double pw, ph;
                on_text_layout_size(probe, &pw, &ph);
                on_text_layout_free(probe);
                content_free(&ct);
                double want = pw + 2.0 * TABLE_PAD + 2.0;
                if (want > widest) widest = want;
            }
            b->col_w[c] = widest < TABLE_COL_MAX ? widest : TABLE_COL_MAX;
        }
        /* Shrink if overflows. */
        double sum = 0.0;
        for (int c = 0; c < blk->cols; c++) sum += b->col_w[c];
        if (sum > cw) {
            for (int c = 0; c < blk->cols; c++) {
                double w = b->col_w[c] * cw / sum;
                b->col_w[c] = w < TABLE_COL_MIN ? TABLE_COL_MIN : w;
            }
        }
        /* Lay out cells at their column widths. */
        for (int r = 0; r < blk->rows; r++) {
            double tallest = 0.0;
            for (int c = 0; c < blk->cols; c++) {
                int ci = r * blk->cols + c;
                OnText *cell_t = on_block_cell(blk, r, c);
                bool hdr = blk->header && r == 0;
                double cw2 = b->col_w[c] - 2.0 * TABLE_PAD;
                if (cw2 < 8.0) cw2 = 8.0;
                content_t ct;
                make_content(L, cell_t, i, ci, &ct);
                on_text_style_t st = text_style_for(L, blk, i, ci, cw2,
                                                     hdr, false);
                b->cells[ci] = on_text_layout_new(&ct.c, &st);
                content_free(&ct);
                double pw, ph;
                on_text_layout_size(b->cells[ci], &pw, &ph);
                if (ph > tallest) tallest = ph;
            }
            b->row_h[r] = tallest + 2.0 * TABLE_PAD;
        }
        double th = 1.0;
        for (int r = 0; r < blk->rows; r++) th += b->row_h[r];
        b->x = LEFT_MARGIN;
        b->w = 1.0;
        for (int c = 0; c < blk->cols; c++) b->w += b->col_w[c];
        b->h = th + BLOCK_GAP;
        b->valid = true;
        return;
    }

    /* Text block. */
    double indent = 0.0;
    if (blk->kind == ON_BLOCK_BULLET || blk->kind == ON_BLOCK_NUMBER ||
        blk->kind == ON_BLOCK_CHECK)
        indent = LIST_INDENT;
    else if (blk->kind == ON_BLOCK_CODE)
        indent = L->style.code_numbers ? CODE_INSET_NUM : CODE_INSET;
    b->x = LEFT_MARGIN + indent;
    double tw = cw - indent - (blk->kind == ON_BLOCK_CODE ? CODE_INSET : 0.0);
    b->w = tw < 8.0 ? 8.0 : tw;

    content_t ct;
    make_content(L, blk->text, i, -1, &ct);
    on_text_style_t st = text_style_for(L, blk, i, -1, b->w, false, false);
    b->layout = on_text_layout_new(&ct.c, &st);
    content_free(&ct);

    double pw, ph;
    on_text_layout_size(b->layout, &pw, &ph);
    b->h = ph + BLOCK_GAP;
    if (blk->kind == ON_BLOCK_CODE) {
        if (code_run_starts(L, i))
            b->h += CODE_PAD;
        if (code_run_last(L, i) == i)
            b->h += CODE_PAD;
    }
    b->valid = true;
}

/* ── validate ───────────────────────────────────────────────────────────── */

/*
 * validate — ensure every BL is laid out and y offsets are summed.
 *
 * Inputs:
 *   L — the layout
 *
 * Output: none.
 */
static void
validate(OnDocLayout *L)
{
    if (L->geometry_valid) return;
    size_t n = (size_t)on_document_n_blocks(L->doc);
    if (L->n_bl != n) {
        for (size_t k = n; k < L->n_bl; k++) bl_clear_i(L, k);
        for (size_t k = L->n_bl; k < n; k++) L->bl[k] = (BL){ 0 };
        BL *new_bl = realloc(L->bl, (n + 1) * sizeof *L->bl);
        if (new_bl || n == 0) { L->bl = new_bl; }
        L->n_bl = n;
    }
    double y = TOP_MARGIN;
    for (size_t i = 0; i < n; i++) {
        if (!L->bl[i].valid) layout_block(L, i);
        L->bl[i].y = y;
        y += L->bl[i].h;
    }
    L->total_h = y - BLOCK_GAP + BOTTOM_MARGIN;
    L->geometry_valid = true;
}

/* ── invalidate helpers ─────────────────────────────────────────────────── */

static void
invalidate(OnDocLayout *L, size_t i)
{
    if (i < L->n_bl) { bl_clear_i(L, i); L->geometry_valid = false; }
}

static void
invalidate_around(OnDocLayout *L, size_t i)
{
    if (i > 0) invalidate(L, i - 1);
    invalidate(L, i);
    invalidate(L, i + 1);
}

static void
invalidate_all(OnDocLayout *L)
{
    for (size_t k = 0; k < L->n_bl; k++) bl_clear_i(L, k);
    L->geometry_valid = false;
}

/* ── text_top ───────────────────────────────────────────────────────────── */

/*
 * text_top — y where a text block's layout starts (code pads move it down).
 */
static double
text_top(const OnDocLayout *L, size_t i)
{
    return L->bl[i].y + (code_run_starts(L, i) ? CODE_PAD : 0.0);
}

/* The word on a code run's first line that copies the run. */
#define COPY_WORD "copy"

/*
 * copy_word_rect — where a code run's "copy" word sits, when the style
 * shows it: right-aligned on the run's first line.
 *
 * Inputs:
 *   L     — the layout (validated)
 *   first — the run's first block
 *   out   — receives the word's rectangle (with 2 pt of slack each side)
 *
 * Output: false when code copy words are off.
 */
static bool
copy_word_rect(OnDocLayout *L, size_t first, on_rect_t *out)
{
    if (!L->style.code_copy) return false;
    if (L->copy_word == NULL) {
        on_text_content_t c = { .text = COPY_WORD, .len = strlen(COPY_WORD) };
        on_text_style_t   st = { .font_size = L->font_size, .scale = 0.85,
                                 .action = true, .dark = L->style.dark };
        L->copy_word = on_text_layout_new(&c, &st);
    }
    double w, h;
    on_text_layout_size(L->copy_word, &w, &h);
    *out = (on_rect_t){ L->width - RIGHT_MARGIN - 6.0 - w, L->bl[first].y + 2.0, w + 4.0, h };
    return true;
}

/*
 * checkbox_rect — the drawn box of a task (CHECK) block, centred on its
 * first line.  Drawing and hit testing both use it.
 *
 * Inputs:
 *   L   — the layout (validated)
 *   i   — the block
 *   out — receives the box
 */
static void
checkbox_rect(OnDocLayout *L, size_t i, on_rect_t *out)
{
    on_rect_t line;
    on_text_layout_line(L->bl[i].layout, 0, NULL, &line);
    double line_h = line.h < CHECK_SIZE ? CHECK_SIZE : line.h;
    *out = (on_rect_t){ LEFT_MARGIN + LIST_INDENT - CHECK_SIZE - 8.0,
                        text_top(L, i) + (line_h - CHECK_SIZE) / 2.0, CHECK_SIZE, CHECK_SIZE };
}

/*
 * rect_has — is a point inside a rectangle?
 *
 * Inputs:
 *   r    — the rectangle
 *   x, y — the point
 *
 * Output: true when it is.
 */
static bool
rect_has(on_rect_t r, double x, double y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ── cell_origin ────────────────────────────────────────────────────────── */

/*
 * cell_origin — the top-left of cell (r, c) of table block i, and its size.
 *
 * Inputs:
 *   L              — the layout
 *   i              — block index
 *   r, c           — row and column
 *   x, y, w, h     — receive the origin and size
 *
 * Output: none.
 */
static void
cell_origin(const OnDocLayout *L, size_t i, int r, int c,
            double *x, double *y, double *w, double *h)
{
    const BL *b = &L->bl[i];
    double cx = b->x + 1.0, cy = b->y + 1.0;
    for (int k = 0; k < c; k++) cx += b->col_w[k];
    for (int k = 0; k < r; k++) cy += b->row_h[k];
    *x = cx;
    *y = cy;
    *w = b->col_w[c];
    *h = b->row_h[r];
}

/* ── layout_for ─────────────────────────────────────────────────────────── */

/*
 * layout_for — the text layout and origin for a position's text.
 *
 * Inputs:
 *   L        — the layout
 *   pos      — the position
 *   ox, oy   — receive the layout's origin (content coords)
 *
 * Output: the layout (borrowed), or NULL for non-text blocks.
 */
static on_text_layout_t *
layout_for(const OnDocLayout *L, OnPos pos, double *ox, double *oy)
{
    const BL *b         = &L->bl[pos.block];
    const OnBlock *blk  = on_document_block(L->doc, pos.block);
    if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0 && b->cells &&
        (size_t)pos.cell < (size_t)(blk->rows * blk->cols)) {
        double cx, cy, cw, ch;
        cell_origin(L, pos.block, pos.cell / blk->cols, pos.cell % blk->cols,
                    &cx, &cy, &cw, &ch);
        *ox = cx + TABLE_PAD;
        *oy = cy + TABLE_PAD;
        return b->cells[pos.cell];
    }
    if (!b->layout) return NULL;
    *ox = b->x;
    *oy = text_top(L, pos.block);
    return b->layout;
}

/* ── image_ord_of ───────────────────────────────────────────────────────── */

/*
 * image_ord_of — the document ordinal of block i's inline image k, or
 * the IMAGE block at i.
 */
static int
image_ord_of(const OnDocLayout *L, size_t i, int k)
{
    int ord = 0;
    for (size_t j = 0; j < i; j++) {
        const OnBlock *blk = on_document_block(L->doc, (guint)j);
        if (blk->kind == ON_BLOCK_IMAGE)
            ord++;
        else if (blk->text)
            ord += (int)blk->text->images->len;
    }
    return ord + (k >= 0 ? k : 0);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_doc_layout_new — see doc_layout.h.
 */
OnDocLayout *
on_doc_layout_new(OnDocument *doc, double font_size)
{
    OnDocLayout *L   = calloc(1, sizeof *L);
    L->doc           = doc;
    L->font_size     = font_size;
    L->width         = 400.0;
    L->style.title_line = true;
    size_t n         = (size_t)on_document_n_blocks(doc);
    L->bl            = calloc(n + 1, sizeof *L->bl);
    L->n_bl          = n;
    return L;
}

/*
 * on_doc_layout_free — see doc_layout.h.
 */
void
on_doc_layout_free(OnDocLayout *L)
{
    if (!L) return;
    for (size_t i = 0; i < L->n_bl; i++) bl_clear_i(L, i);
    free(L->bl);
    free(L->pre_text);
    free(L->hits);
    on_text_layout_free(L->copy_word);
    free(L);
}

/*
 * on_doc_layout_set_width — see doc_layout.h.
 */
void
on_doc_layout_set_width(OnDocLayout *L, double width)
{
    if (width == L->width) return;
    L->width = width;
    invalidate_all(L);
}

/*
 * on_doc_layout_set_style — see doc_layout.h.
 */
void
on_doc_layout_set_style(OnDocLayout *L, const OnDocLayoutStyle *style)
{
    L->style = *style;
    on_text_layout_free(L->copy_word);     /* its colour follows the appearance */
    L->copy_word = NULL;
    invalidate_all(L);
}

/*
 * on_doc_layout_block_changed — see doc_layout.h.
 */
void
on_doc_layout_block_changed(OnDocLayout *L, guint i)
{
    invalidate_around(L, (size_t)i);
}

/*
 * on_doc_layout_blocks_inserted — see doc_layout.h.
 */
void
on_doc_layout_blocks_inserted(OnDocLayout *L, guint i, guint n)
{
    size_t new_n = L->n_bl + n;
    BL *nb = realloc(L->bl, (new_n + 1) * sizeof *L->bl);
    if (nb) L->bl = nb;
    memmove(&L->bl[i + n], &L->bl[i], (L->n_bl - i) * sizeof *L->bl);
    for (guint k = 0; k < n; k++) L->bl[i + k] = (BL){ 0 };
    L->n_bl = new_n;
    invalidate_around(L, (size_t)i);
    invalidate(L, (size_t)(i + n));
    L->geometry_valid = false;
}

/*
 * on_doc_layout_blocks_removed — see doc_layout.h.
 */
void
on_doc_layout_blocks_removed(OnDocLayout *L, guint i, guint n)
{
    for (guint k = 0; k < n && (size_t)(i + k) < L->n_bl; k++)
        bl_clear_i(L, (size_t)(i + k));
    size_t actual = n < (guint)(L->n_bl - i) ? n : (guint)(L->n_bl - i);
    memmove(&L->bl[i], &L->bl[i + actual],
            (L->n_bl - i - actual) * sizeof *L->bl);
    L->n_bl -= actual;
    invalidate_around(L, (size_t)i);
    L->geometry_valid = false;
}

/*
 * on_doc_layout_set_preedit — see doc_layout.h.
 */
void
on_doc_layout_set_preedit(OnDocLayout *L, OnPos pos, const char *text,
                          size_t cursor)
{
    if (L->pre_on) invalidate(L, L->pre_pos.block);
    free(L->pre_text);
    L->pre_on     = text && *text;
    L->pre_pos    = pos;
    L->pre_text   = L->pre_on ? strdup(text) : NULL;
    L->pre_cursor = cursor;
    if (L->pre_on) invalidate(L, pos.block);
}

/*
 * on_doc_layout_set_hits — see doc_layout.h.
 */
void
on_doc_layout_set_hits(OnDocLayout *L, const OnDocHit *hits, size_t n)
{
    /* Only blocks whose hits changed re-lay out. */
    size_t i = 0, j = 0;
    while ((L->hits && i < L->n_hits) || (hits && j < n)) {
        size_t bi = (L->hits && i < L->n_hits) ? (size_t)L->hits[i].block
                                                 : SIZE_MAX;
        size_t bj = (hits && j < n)             ? (size_t)hits[j].block
                                                 : SIZE_MAX;
        size_t b = bi < bj ? bi : bj;
        /* Check if hits for block b changed. */
        bool changed = (bi != bj);
        if (!changed) {
            /* Compare all hits for block b. */
            size_t ii = i, jj = j;
            while (ii < L->n_hits && (size_t)L->hits[ii].block == b) ii++;
            while (jj < n && (size_t)hits[jj].block == b) jj++;
            size_t ni = ii - i, nj = jj - j;
            if (ni != nj) {
                changed = true;
            } else {
                for (size_t k = 0; k < ni && !changed; k++) {
                    const OnDocHit *a = &L->hits[i + k];
                    const OnDocHit *c = &hits[j + k];
                    if (a->cell != c->cell || a->start != c->start ||
                        a->len != c->len)
                        changed = true;
                }
            }
            i = ii; j = jj;
        } else {
            if (bi <= bj) { while (i < L->n_hits && (size_t)L->hits[i].block == b) i++; }
            if (bj <= bi) { while (j < n && (size_t)hits[j].block == b) j++; }
        }
        if (changed && b < L->n_bl) invalidate(L, b);
    }
    free(L->hits);
    if (n > 0) {
        L->hits = malloc(n * sizeof *L->hits);
        memcpy(L->hits, hits, n * sizeof *L->hits);
        L->n_hits = n;
    } else {
        L->hits   = NULL;
        L->n_hits = 0;
    }
}

/* ── Geometry ───────────────────────────────────────────────────────────── */

/*
 * on_doc_layout_height — see doc_layout.h.
 */
double
on_doc_layout_height(OnDocLayout *L)
{
    validate(L);
    return L->total_h;
}

/*
 * on_doc_layout_caret_rect — see doc_layout.h.
 */
void
on_doc_layout_caret_rect(OnDocLayout *L, OnPos pos, on_rect_t *out)
{
    validate(L);
    if (pos.block >= (guint)L->n_bl) {
        *out = (on_rect_t){ LEFT_MARGIN, TOP_MARGIN, 1.0, 16.0 };
        return;
    }
    const BL *b     = &L->bl[pos.block];
    const OnBlock *blk = on_document_block(L->doc, pos.block);
    double ox = 0.0, oy = 0.0;
    on_text_layout_t *layout = layout_for(L, pos, &ox, &oy);
    if (!layout) {
        double h = (blk->kind == ON_BLOCK_IMAGE) ? b->img_h : b->h - BLOCK_GAP;
        *out = (on_rect_t){ b->x + (pos.offset > 0 ? b->w : -1.0), b->y, 1.0, h };
        return;
    }
    size_t idx = idx_of(L, pos.block, pos.cell, pos.offset);
    if (L->pre_on && (size_t)L->pre_pos.block == pos.block &&
        L->pre_pos.cell == pos.cell && pos.offset == L->pre_pos.offset)
        idx = pos.offset + L->pre_cursor;
    on_rect_t cr;
    on_text_layout_cursor_rect(layout, idx, &cr);
    out->x = ox + cr.x;
    out->y = oy + cr.y;
    out->w = 1.0;
    out->h = cr.h;
}

/*
 * on_doc_layout_code_run — see doc_layout.h.
 */
void
on_doc_layout_code_run(OnDocLayout *L, guint block, guint *first,
                       guint *count)
{
    validate(L);
    if (on_document_block(L->doc, block)->kind != ON_BLOCK_CODE) {
        *first = block;
        *count = 1;
        return;
    }
    size_t f = code_run_first(L, (size_t)block);
    size_t la = code_run_last(L, (size_t)block);
    *first = (guint)f;
    *count = (guint)(la - f + 1);
}

/*
 * on_doc_layout_image — see doc_layout.h.
 */
CGImageRef
on_doc_layout_image(OnDocLayout *L, guint block, gint inline_ord)
{
    OnBlock *blk = on_document_block(L->doc, block);
    if (!blk) return NULL;
    GBytes *png = NULL;
    gpointer *cache = &blk->pixels;
    GDestroyNotify *cfree = &blk->pixels_free;
    if (blk->kind == ON_BLOCK_IMAGE) {
        png = blk->png;
    } else {
        if (!blk->text || inline_ord < 0 ||
            (size_t)inline_ord >= blk->text->images->len)
            return NULL;
        OnInlineImage *img =
            &g_array_index(blk->text->images, OnInlineImage, (guint)inline_ord);
        png   = img->png;
        cache = &img->pixels;
        cfree = &img->pixels_free;
    }
    if (*cache) return (CGImageRef)*cache;
    gsize n;
    const guint8 *d = g_bytes_get_data(png, &n);
    CGImageRef img_ref = on_image_decode_capped(d, n, 0);
    if (!img_ref) return NULL;
    *cache = img_ref;
    *cfree = (GDestroyNotify)CGImageRelease;
    return img_ref;
}

/* ── Hit testing ────────────────────────────────────────────────────────── */

/*
 * block_at_y — the block whose span covers y (clamped).
 */
static size_t
block_at_y(const OnDocLayout *L, double y)
{
    size_t n = L->n_bl;
    if (!n) return 0;
    size_t lo = 0, hi = n - 1;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (y < L->bl[mid].y + L->bl[mid].h) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

/*
 * on_doc_layout_hit — see doc_layout.h.
 */
void
on_doc_layout_hit(OnDocLayout *L, double x, double y, OnDocHitResult *out)
{
    validate(L);
    memset(out, 0, sizeof *out);
    out->kind = ON_HIT_TEXT;
    out->pos.cell = -1;
    out->image_ord = -1;
    if (!L->n_bl) return;
    size_t i = block_at_y(L, y);
    const BL *b = &L->bl[i];
    const OnBlock *blk = on_document_block(L->doc, (guint)i);
    out->block = (guint)i;
    out->pos.block = (guint)i;

    if (blk->kind == ON_BLOCK_IMAGE) {
        bool inside = x >= b->x && x < b->x + b->w &&
                      y >= b->y && y < b->y + b->img_h;
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
        double cx, cy, cw, ch;
        for (int r = 0; r < blk->rows; r++) {
            for (int c = 0; c < blk->cols; c++) {
                cell_origin(L, i, r, c, &cx, &cy, &cw, &ch);
                if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
                    out->kind     = ON_HIT_TEXT;
                    out->pos.cell = r * blk->cols + c;
                    on_text_layout_t *cl = b->cells[out->pos.cell];
                    if (cl) {
                        size_t idx = on_text_layout_index_at(
                            cl, x - cx - TABLE_PAD, y - cy - TABLE_PAD);
                        out->pos.offset = off_of(L, i, out->pos.cell, idx);
                    }
                    return;
                }
            }
        }
        return;
    }
    /* Text block: the run's "copy" word, a task's box, else the text. */
    on_rect_t r;
    if (code_run_starts(L, i) && copy_word_rect(L, i, &r) && rect_has(r, x, y)) {
        out->kind = ON_HIT_COPY;
        out->pos.offset = 0;
        return;
    }
    if (blk->kind == ON_BLOCK_CHECK) {
        checkbox_rect(L, i, &r);
        if (rect_has(r, x, y)) {
            out->kind = ON_HIT_CHECKBOX;
            out->pos.offset = 0;
            return;
        }
    }
    double ty = text_top(L, i);
    if (b->layout) {
        size_t idx = on_text_layout_index_at(b->layout, x - b->x, y - ty);
        out->pos.offset = off_of(L, i, -1, idx);
    }
    /* Check for inline image hit. */
    for (guint k = 0; k < blk->text->images->len; k++) {
        const OnInlineImage *img =
            &g_array_index(blk->text->images, OnInlineImage, k);
        on_rect_t br;
        on_text_layout_box_rect(b->layout, (size_t)k, &br);
        if (x >= b->x + br.x && x < b->x + br.x + br.w &&
            y >= ty + br.y    && y < ty + br.y + br.h) {
            out->kind         = ON_HIT_IMAGE;
            out->inline_image = true;
            out->image_ord    = image_ord_of(L, i, (int)k);
            out->pos.offset   = img->offset;
            return;
        }
    }
}

/* ── Navigation ─────────────────────────────────────────────────────────── */

static size_t
text_len_at(OnDocLayout *L, OnPos pos)
{
    OnText *t = on_document_text_at(L->doc, pos);
    return t ? (size_t)t->text->len : 0;
}

static OnPos
block_start_pos(OnDocLayout *L, size_t i)
{
    OnPos p = { (guint)i, -1, 0 };
    const OnBlock *blk = on_document_block(L->doc, (guint)i);
    if (blk->kind == ON_BLOCK_TABLE) p.cell = 0;
    return p;
}

static OnPos
block_end_pos(OnDocLayout *L, size_t i)
{
    OnPos p = { (guint)i, -1, 0 };
    const OnBlock *blk = on_document_block(L->doc, (guint)i);
    if (blk->kind == ON_BLOCK_TABLE) {
        p.cell   = blk->rows * blk->cols - 1;
        p.offset = (gsize)text_len_at(L, p);
    } else if (blk->kind == ON_BLOCK_IMAGE) {
        p.offset = 1;
    } else {
        p.offset = (gsize)blk->text->text->len;
    }
    return p;
}

/*
 * line_of_byte — the line index within a layout that contains `byte`, plus
 * the layout-relative x of `byte`.
 *
 * Inputs:
 *   layout  — the text layout
 *   byte    — byte offset
 *   line    — receives the line index
 *   x       — receives the x position
 *
 * Output: none.
 */
static void
line_of_byte(on_text_layout_t *layout, size_t byte, size_t *line, double *x)
{
    on_rect_t cr;
    on_text_layout_cursor_rect(layout, byte, &cr);
    *x = cr.x;
    /* Find line by y. */
    size_t n = on_text_layout_line_count(layout);
    *line = 0;
    for (size_t li = 0; li < n; li++) {
        on_rect_t lr;
        on_text_layout_line(layout, li, NULL, &lr);
        if (cr.y >= lr.y) *line = li;
    }
}

/*
 * on_doc_layout_move — see doc_layout.h.
 */
OnPos
on_doc_layout_move(OnDocLayout *L, OnPos pos, OnDocMove how, double *goal_x)
{
    validate(L);
    size_t n = L->n_bl;
    if ((size_t)pos.block >= n) return pos;
    const OnBlock *blk = on_document_block(L->doc, pos.block);
    double ox = 0.0, oy = 0.0;
    on_text_layout_t *layout = layout_for(L, pos, &ox, &oy);
    OnPos out = pos;

    switch (how) {
    case ON_MOVE_CHAR_LEFT:
    case ON_MOVE_WORD_LEFT:
        *goal_x = -1.0;
        if (layout) {
            size_t idx = idx_of(L, pos.block, pos.cell, pos.offset);
            size_t nidx = (how == ON_MOVE_WORD_LEFT)
                ? on_text_layout_prev_word(layout, idx)
                : on_text_layout_prev_cursor(layout, idx);
            if (nidx != idx) {
                out.offset = (gsize)off_of(L, pos.block, pos.cell, nidx);
                return out;
            }
        } else if (pos.offset > 0) {
            out.offset = 0; return out;
        }
        if (blk->kind == ON_BLOCK_TABLE && pos.cell > 0) {
            out.cell = pos.cell - 1;
            out.offset = (gsize)text_len_at(L, out);
            return out;
        }
        if (!pos.block) return pos;
        return block_end_pos(L, (size_t)pos.block - 1);

    case ON_MOVE_CHAR_RIGHT:
    case ON_MOVE_WORD_RIGHT:
        *goal_x = -1.0;
        if (layout) {
            size_t idx = idx_of(L, pos.block, pos.cell, pos.offset);
            size_t nidx = (how == ON_MOVE_WORD_RIGHT)
                ? on_text_layout_next_word(layout, idx)
                : on_text_layout_next_cursor(layout, idx);
            if (nidx != idx) {
                out.offset = (gsize)off_of(L, pos.block, pos.cell, nidx);
                return out;
            }
        } else if (pos.offset == 0) {
            out.offset = 1; return out;
        }
        if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0 &&
            pos.cell + 1 < blk->rows * blk->cols) {
            out.cell = pos.cell + 1;
            out.offset = 0;
            return out;
        }
        if ((size_t)pos.block + 1 >= n) return pos;
        return block_start_pos(L, (size_t)pos.block + 1);

    case ON_MOVE_LINE_START:
    case ON_MOVE_LINE_END: {
        *goal_x = -1.0;
        if (!layout) { out.offset = (how == ON_MOVE_LINE_START) ? 0 : 1; return out; }
        size_t li; double lx;
        size_t idx = idx_of(L, pos.block, pos.cell, pos.offset);
        line_of_byte(layout, idx, &li, &lx);
        on_text_span_t span;
        on_text_layout_line(layout, li, &span, NULL);
        size_t raw = (how == ON_MOVE_LINE_START) ? span.start : span.start + span.len;
        out.offset = (gsize)off_of(L, pos.block, pos.cell, raw);
        if (how == ON_MOVE_LINE_END && li + 1 < on_text_layout_line_count(layout)) {
            /* A wrapped line ends with the whitespace it broke at, and that
             * index IS the next line's start: the line's end is before it. */
            OnText *t     = on_document_text_at(L->doc, pos);
            gsize   start = (gsize)off_of(L, pos.block, pos.cell, span.start);
            while (t && out.offset > start) {
                const gchar *q = g_utf8_prev_char(t->text->str + out.offset);
                if (!g_unichar_isspace(g_utf8_get_char(q))) break;
                out.offset = (gsize)(q - t->text->str);
            }
        }
        return out;
    }

    case ON_MOVE_LINE_UP:
    case ON_MOVE_LINE_DOWN: {
        int dir = (how == ON_MOVE_LINE_DOWN) ? 1 : -1;
        if (layout) {
            size_t li; double lx;
            size_t idx = idx_of(L, pos.block, pos.cell, pos.offset);
            line_of_byte(layout, idx, &li, &lx);
            if (*goal_x < 0.0) *goal_x = ox + lx;
            size_t n_lines = on_text_layout_line_count(layout);
            size_t target = (dir > 0) ? li + 1 : (li > 0 ? li - 1 : SIZE_MAX);
            if (target < n_lines) {
                on_rect_t tr;
                on_text_layout_line(layout, target, NULL, &tr);
                double ty = tr.y + tr.h / 2.0;
                size_t nidx = on_text_layout_index_at(layout, *goal_x - ox, ty);
                out.offset = (gsize)off_of(L, pos.block, pos.cell, nidx);
                return out;
            }
            /* Table: try next/prev row of same column. */
            if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0) {
                int r = pos.cell / blk->cols + dir, c = pos.cell % blk->cols;
                if (r >= 0 && r < blk->rows) {
                    out.cell = r * blk->cols + c;
                    double cx, cy, cw, ch;
                    cell_origin(L, pos.block, r, c, &cx, &cy, &cw, &ch);
                    on_text_layout_t *cl = L->bl[pos.block].cells[out.cell];
                    size_t tli = dir > 0 ? 0 : on_text_layout_line_count(cl) - 1;
                    on_rect_t tr2;
                    on_text_layout_line(cl, tli, NULL, &tr2);
                    double ty = tr2.y + tr2.h / 2.0;
                    out.offset = (gsize)off_of(L, pos.block, out.cell,
                        on_text_layout_index_at(cl, *goal_x - (cx + TABLE_PAD), ty));
                    return out;
                }
            }
        } else {
            if (*goal_x < 0.0) {
                const BL *b = &L->bl[pos.block];
                *goal_x = b->x + (pos.offset > 0 ? b->w : 0.0);
            }
        }
        /* Into the neighbouring block. */
        if ((dir < 0 && !pos.block) || (dir > 0 && (size_t)pos.block + 1 >= n))
            return pos;
        size_t j = (size_t)pos.block + dir;
        out.block = (guint)j;
        out.cell  = -1;
        const OnBlock *nb = on_document_block(L->doc, (guint)j);
        if (nb->kind == ON_BLOCK_IMAGE) {
            const BL *b = &L->bl[j];
            out.offset = (*goal_x > b->x + b->w / 2.0) ? 1 : 0;
            return out;
        }
        if (nb->kind == ON_BLOCK_TABLE) {
            int r = dir > 0 ? 0 : nb->rows - 1, c = 0;
            double cx, cy, cw, ch;
            for (c = 0; c < nb->cols; c++) {
                cell_origin(L, j, r, c, &cx, &cy, &cw, &ch);
                if (*goal_x < cx + cw) break;
            }
            c = c < nb->cols ? c : nb->cols - 1;
            out.cell = r * nb->cols + c;
            on_text_layout_t *cl = L->bl[j].cells[out.cell];
            cell_origin(L, j, r, c, &cx, &cy, &cw, &ch);
            size_t tli = dir > 0 ? 0 : on_text_layout_line_count(cl) - 1;
            on_rect_t tr;
            on_text_layout_line(cl, tli, NULL, &tr);
            out.offset = (gsize)off_of(L, j, out.cell,
                on_text_layout_index_at(cl, *goal_x - (cx + TABLE_PAD),
                                         tr.y + tr.h / 2.0));
            return out;
        }
        on_text_layout_t *nl = layout_for(L, out, &ox, &oy);
        if (!nl) return block_start_pos(L, j);
        size_t tli = dir > 0 ? 0 : on_text_layout_line_count(nl) - 1;
        on_rect_t tr;
        on_text_layout_line(nl, tli, NULL, &tr);
        out.offset = (gsize)off_of(L, j, -1,
            on_text_layout_index_at(nl, *goal_x - ox, tr.y + tr.h / 2.0));
        return out;
    }

    case ON_MOVE_BLOCK_START:
        *goal_x = -1.0;
        if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0) {
            out.offset = 0; return out;
        }
        return block_start_pos(L, pos.block);
    case ON_MOVE_BLOCK_END:
        *goal_x = -1.0;
        if (blk->kind == ON_BLOCK_TABLE && pos.cell >= 0) {
            out.offset = (gsize)text_len_at(L, pos); return out;
        }
        return block_end_pos(L, pos.block);
    case ON_MOVE_DOC_START:
        *goal_x = -1.0;
        return block_start_pos(L, 0);
    case ON_MOVE_DOC_END:
        *goal_x = -1.0;
        return block_end_pos(L, n - 1);
    }
    return pos;
}

/*
 * on_doc_layout_word_at — see doc_layout.h.
 */
void
on_doc_layout_word_at(OnDocLayout *L, OnPos pos, OnPos *a, OnPos *b)
{
    validate(L);
    *a = *b = pos;
    double ox, oy;
    on_text_layout_t *layout = layout_for(L, pos, &ox, &oy);
    if (!layout) { a->offset = 0; b->offset = 1; return; }
    size_t idx = idx_of(L, pos.block, pos.cell, pos.offset);
    on_text_span_t span;
    on_text_layout_word_at(layout, idx, &span);
    a->offset = (gsize)off_of(L, pos.block, pos.cell, span.start);
    b->offset = (gsize)off_of(L, pos.block, pos.cell, span.start + span.len);
}

/*
 * on_doc_layout_line_at — see doc_layout.h.
 */
void
on_doc_layout_line_at(OnDocLayout *L, OnPos pos, OnPos *a, OnPos *b)
{
    double gx = -1.0;
    *a = on_doc_layout_move(L, pos, ON_MOVE_LINE_START, &gx);
    gx = -1.0;
    *b = on_doc_layout_move(L, pos, ON_MOVE_LINE_END, &gx);
}

/* ── Painting ───────────────────────────────────────────────────────────── */

/*
 * draw_selection_in_layout — fill selection rectangles for the [s, e) byte
 * range of a text layout.
 *
 * Inputs:
 *   ctx    — the graphics context
 *   layout — the text layout
 *   ox, oy — the layout's origin in content coordinates
 *   s, e   — byte range (spliced indices)
 *   c      — the selection colour
 *
 * Output: none.
 */
static void
draw_selection_in_layout(CGContextRef ctx, on_text_layout_t *layout,
                         double ox, double oy, size_t s, size_t e,
                         on_color_t c)
{
    on_text_span_t span  = { s, e > s ? e - s : 0 };
    size_t         n     = on_text_layout_range_rects(layout, span, NULL, 0);
    on_rect_t     *rects = calloc(n + 1, sizeof *rects);
    n = on_text_layout_range_rects(layout, span, rects, n);
    for (size_t k = 0; k < n; k++) {
        on_rect_t r = {
            ox + rects[k].x, oy + rects[k].y,
            rects[k].w > 0.0 ? rects[k].w : 4.0, rects[k].h
        };
        on_paint_fill_rect(ctx, r, c);
    }
    free(rects);
}

/*
 * sel_span — the selected byte span of block i / cell, given ordered
 * selection a..b.  Returns false when nothing is selected.
 */
static bool
sel_span(const OnDocLayout *L __attribute__((unused)), const OnDocPaint *p,
         size_t i, int cell, size_t len, size_t *s, size_t *e, bool *whole)
{
    *whole = false;
    if (!p->has_sel || (size_t)p->sel_a.block > i || (size_t)p->sel_b.block < i)
        return false;
    bool first = (size_t)p->sel_a.block == i, last = (size_t)p->sel_b.block == i;
    if (first && last && p->sel_a.cell >= 0 && p->sel_a.cell == p->sel_b.cell) {
        if (cell != p->sel_a.cell) return false;
        *s = (size_t)p->sel_a.offset;
        *e = (size_t)p->sel_b.offset;
        return *s < *e;
    }
    if (cell >= 0) { *whole = true; *s = 0; *e = len; return true; }
    *s = first ? (size_t)p->sel_a.offset : 0;
    *e = last  ? (size_t)p->sel_b.offset : len;
    if (!first && !last) *whole = true;
    return true;
}

/*
 * on_doc_layout_draw — see doc_layout.h.
 */
void
on_doc_layout_draw(OnDocLayout *L, CGContextRef ctx, const OnDocPaint *p)
{
    validate(L);
    size_t n = L->n_bl;
    if (!n) return;
    on_color_t selc = on_look_pick(p->focused ? on_look_selection
                                              : on_look_selection_dim,
                                   L->style.dark);
    on_color_t text_c = on_look_pick(on_look_text, L->style.dark);
    int number_run = 0;
    int code_run   = 0;

    size_t first = block_at_y(L, p->clip.y);
    /* Number/code run counts above clip. */
    for (size_t i = first; i > 0; i--) {
        if (on_document_block(L->doc, (guint)(i - 1))->kind != ON_BLOCK_NUMBER) break;
        number_run++;
    }
    code_run = (int)(first - code_run_first(L, first));

    for (size_t i = first; i < n; i++) {
        const BL *b = &L->bl[i];
        if (b->y > p->clip.y + p->clip.h) break;
        const OnBlock *blk = on_document_block(L->doc, (guint)i);
        number_run = (blk->kind == ON_BLOCK_NUMBER) ? number_run + 1 : 0;
        code_run   = (blk->kind == ON_BLOCK_CODE)   ? code_run + 1   : 0;
        bool whole;
        size_t s, e;

        if (blk->kind == ON_BLOCK_IMAGE) {
            on_rect_t r = { b->x, b->y, b->w, b->img_h };
            CGImageRef img = on_doc_layout_image(L, (guint)i, -1);
            if (img)
                on_paint_image(ctx, img, r);
            else
                on_paint_fill_rect(ctx, r,
                    on_look_pick(on_look_code_fill, L->style.dark));
            if (sel_span(L, p, i, -1, 1, &s, &e, &whole) &&
                (whole || (s == 0 && e == 1)))
                on_paint_fill_rect(ctx, r, selc);
            continue;
        }

        if (blk->kind == ON_BLOCK_TABLE) {
            double cx, cy, cw, ch;
            on_color_t bd = on_look_pick(on_look_table_border, L->style.dark);
            on_color_t hdr = on_look_pick(on_look_table_header, L->style.dark);
            for (int r = 0; r < blk->rows; r++) {
                for (int c = 0; c < blk->cols; c++) {
                    cell_origin(L, i, r, c, &cx, &cy, &cw, &ch);
                    if (blk->header && r == 0)
                        on_paint_fill_rect(ctx, (on_rect_t){cx, cy, cw, ch}, hdr);
                    int ci = r * blk->cols + c;
                    on_text_layout_t *cl = b->cells[ci];
                    if (!cl) continue;
                    /* Selection */
                    OnText *ct = on_block_cell(blk, r, c);
                    if (sel_span(L, p, i, ci, ct ? (size_t)ct->text->len : 0,
                                 &s, &e, &whole)) {
                        if (whole)
                            on_paint_fill_rect(ctx, (on_rect_t){cx, cy, cw, ch}, selc);
                        else
                            draw_selection_in_layout(ctx, cl,
                                cx + TABLE_PAD, cy + TABLE_PAD,
                                idx_of(L, i, ci, s), idx_of(L, i, ci, e), selc);
                    }
                    on_text_layout_draw(cl, ctx, cx + TABLE_PAD, cy + TABLE_PAD);
                    /* Cell borders: top and left. */
                    on_paint_fill_rect(ctx, (on_rect_t){cx - 1, cy - 1, cw + 1, 1}, bd);
                    on_paint_fill_rect(ctx, (on_rect_t){cx - 1, cy - 1, 1, ch + 1}, bd);
                }
            }
            /* Bottom and right border. */
            on_paint_fill_rect(ctx, (on_rect_t){b->x, b->y + b->h - BLOCK_GAP - 1, b->w, 1}, bd);
            on_paint_fill_rect(ctx, (on_rect_t){b->x + b->w - 1, b->y, 1, b->h - BLOCK_GAP}, bd);
            continue;
        }

        /* Text block. */
        double ty = text_top(L, i);
        if (blk->kind == ON_BLOCK_CODE) {
            double bg_h = b->h - (code_run_last(L, i) == i ? BLOCK_GAP : 0.0);
            on_paint_fill_rect(ctx,
                (on_rect_t){ LEFT_MARGIN, b->y, content_width(L), bg_h },
                on_look_pick(on_look_code_fill, L->style.dark));
            if (L->style.code_numbers) {
                char num[16];
                snprintf(num, sizeof num, "%d", code_run);
                on_text_content_t nc = { .text = num, .len = strlen(num) };
                on_text_style_t ns = {
                    .font_size = L->font_size * 0.8,
                    .scale = 1.0, .mono = true, .muted = true,
                    .dark = L->style.dark, .width = 0.0
                };
                on_text_layout_t *nl = on_text_layout_new(&nc, &ns);
                double nw, nh;
                on_text_layout_size(nl, &nw, &nh);
                on_text_layout_draw(nl, ctx, b->x - 8.0 - nw, ty + 1.0);
                on_text_layout_free(nl);
            }
        }
        if (!b->layout) continue;
        /* Selection. */
        if (sel_span(L, p, i, -1, (size_t)blk->text->text->len, &s, &e, &whole)) {
            draw_selection_in_layout(ctx, b->layout, b->x, ty,
                idx_of(L, i, -1, s), idx_of(L, i, -1, e), selc);
        }
        /* List prefix. */
        if (blk->kind == ON_BLOCK_BULLET) {
            const char *bullet = "\xe2\x80\xa2";
            on_text_content_t bc = { .text = bullet, .len = 3 };
            on_text_style_t bs = { .font_size = L->font_size, .scale = 1.0,
                                   .dark = L->style.dark };
            on_text_layout_t *bl2 = on_text_layout_new(&bc, &bs);
            double bw, bh;
            on_text_layout_size(bl2, &bw, &bh);
            on_text_layout_draw(bl2, ctx, b->x - 12.0 - bw, ty);
            on_text_layout_free(bl2);
        } else if (blk->kind == ON_BLOCK_NUMBER) {
            char num[16];
            snprintf(num, sizeof num, "%d.", number_run);
            on_text_content_t nc = { .text = num, .len = strlen(num) };
            on_text_style_t ns = { .font_size = L->font_size, .scale = 1.0,
                                   .dark = L->style.dark };
            on_text_layout_t *nl = on_text_layout_new(&nc, &ns);
            double nw, nh;
            on_text_layout_size(nl, &nw, &nh);
            on_text_layout_draw(nl, ctx, b->x - 8.0 - nw, ty);
            on_text_layout_free(nl);
        } else if (blk->kind == ON_BLOCK_CHECK) {
            /* Checkbox: centred on the first line. */
            on_rect_t box;
            checkbox_rect(L, i, &box);
            double bx = box.x, by_c = box.y;
            on_color_t check_c = on_look_pick(on_look_checkbox, L->style.dark);
            on_color_t white   = { 1, 1, 1, 1 };
            on_color_t bd2     = on_look_pick(on_look_table_border, L->style.dark);
            if (blk->checked) {
                on_paint_rounded_rect(ctx, box, 3.0, check_c,
                                      (on_color_t){0,0,0,0}, 0.0);
                /* Tick: two line segments. */
                on_paint_line(ctx, bx + 3, by_c + 7, bx + 6, by_c + 10, white, 1.5);
                on_paint_line(ctx, bx + 5, by_c + 10, bx + 11, by_c + 4, white, 1.5);
            } else {
                on_paint_rounded_rect(ctx, box, 3.0,
                                      on_look_pick(on_look_page, L->style.dark),
                                      bd2, 1.0);
            }
        }
        on_text_layout_draw(b->layout, ctx, b->x, ty);
        /* Inline images over their boxes. */
        for (guint k = 0; k < blk->text->images->len; k++) {
            on_rect_t br;
            on_text_layout_box_rect(b->layout, (size_t)k, &br);
            on_rect_t ir = { b->x + br.x, ty + br.y, br.w, br.h };
            CGImageRef cg = on_doc_layout_image(L, (guint)i, (gint)k);
            if (cg) on_paint_image(ctx, cg, ir);
        }
        /* The "copy" word on the first block of a code run. */
        on_rect_t cw;
        if (code_run_starts(L, i) && copy_word_rect(L, i, &cw))
            on_text_layout_draw(L->copy_word, ctx, cw.x + 2.0, cw.y);
    }
    /* Caret. */
    if (p->caret_visible) {
        on_rect_t cr;
        on_doc_layout_caret_rect(L, p->caret, &cr);
        on_paint_fill_rect(ctx, cr, text_c);
    }
}
