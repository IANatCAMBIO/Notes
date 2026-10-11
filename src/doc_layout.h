/* ===========================================================================
 * doc_layout.h — OnDocLayout: the geometry of an OnDocument on screen
 *
 * One OnDocLayout per note view.  For every block it keeps a text layout
 * (text kinds and table cells — text_layout.h), a height and a y offset,
 * and it answers the three questions a view has: where does this position
 * sit (caret rect), what is under this point (hit test), and where does a
 * cursor movement land (navigation).  It also paints: text, images, tables,
 * code shading, list prefixes, checkboxes, selection and caret.
 *
 * Everything the layout knows comes from the document and the style the
 * view sets; the view relays the document's observer notifications so
 * exactly the changed block re-lays out.
 *
 * COORDINATES are content coordinates in points: x from the view's left
 * edge, y DOWN from the top of the content.  The view is flipped and as
 * tall as on_doc_layout_height, inside an NSScrollView.
 *
 * POSITIONS are OnPos (document.h): block, cell, and a UTF-8 BYTE offset.
 * No UTF-16 index crosses this interface (utf_index.h).
 *
 * This is the GTK build's doc_layout.h with its five toolkit types removed
 * (PangoContext, PangoAttrList, graphene_rect_t, GdkTexture, GtkSnapshot /
 * GdkRGBA); the functions and their meaning are otherwise unchanged, so
 * the GTK build's src/doc_layout.c is the specification.
 * =========================================================================== */

#ifndef ON_DOC_LAYOUT_H
#define ON_DOC_LAYOUT_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#include "document.h"
#include "paint.h"                   /* on_rect_t                           */

typedef struct OnDocLayout OnDocLayout;

/* What the view's settings decide about the look.                          */
typedef struct {
    bool title_line;                 /* block 0 centred + heading-sized     */
    bool code_numbers;               /* line numbers in the code gutter     */
    bool code_copy;                  /* the "copy" word on a code run       */
    bool dark;                       /* dark appearance (look.h colours)    */
} OnDocLayoutStyle;

/* ---------------------------------------------------------------------------
 * on_doc_layout_new() — a layout for a document.
 *
 * Inputs:
 *   doc       — the document; not owned: the view owns both and frees the
 *               layout first
 *   font_size — body text size in points (look.h: on_look_font_size)
 *
 * Output: a new layout; on_doc_layout_free it.
 * ------------------------------------------------------------------------- */
OnDocLayout *on_doc_layout_new(OnDocument *doc, double font_size);

/* ---------------------------------------------------------------------------
 * on_doc_layout_free() — release the layout, every cached text layout and
 * nothing else: decoded images live on their blocks (OnBlock.pixels) and go
 * with the document.
 *
 * Inputs:
 *   layout — the layout; NULL is ignored
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_free(OnDocLayout *layout);

/* ---------------------------------------------------------------------------
 * on_doc_layout_set_width() — the view's width; a change invalidates every
 * block.
 *
 * Inputs:
 *   layout — the layout
 *   width  — points
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_set_width(OnDocLayout *layout, double width);

/* ---------------------------------------------------------------------------
 * on_doc_layout_set_style() — the look; a change invalidates everything.
 *
 * Inputs:
 *   layout — the layout
 *   style  — the look (copied)
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_set_style(OnDocLayout *layout,
                             const OnDocLayoutStyle *style);

/* ---------------------------------------------------------------------------
 * The document observer (OnDocumentObserver), relayed by the view so that
 * exactly the changed blocks re-lay out.
 *
 * Inputs:
 *   layout — the layout
 *   i      — the first block concerned
 *   n      — how many blocks were inserted or removed
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_block_changed(OnDocLayout *layout, guint i);
void on_doc_layout_blocks_inserted(OnDocLayout *layout, guint i, guint n);
void on_doc_layout_blocks_removed(OnDocLayout *layout, guint i, guint n);

/* ---------------------------------------------------------------------------
 * on_doc_layout_set_preedit() — an input method's uncommitted (marked)
 * text, shown spliced into the block at `pos` and underlined.
 *
 * Inputs:
 *   layout — the layout
 *   pos    — where it is spliced in
 *   text   — the uncommitted text; NULL clears it
 *   cursor — the caret's BYTE offset inside `text`
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_set_preedit(OnDocLayout *layout, OnPos pos,
                               const char *text, size_t cursor);

/* A find hit: `len` bytes at `start` of a block's text (cell -1) or cell.  */
typedef struct {
    guint  block;
    gint   cell;
    gsize  start, len;
} OnDocHit;

/* ---------------------------------------------------------------------------
 * on_doc_layout_set_hits() — the find highlights.  Only the blocks whose
 * hits changed re-lay out.
 *
 * Inputs:
 *   layout — the layout
 *   hits   — the hits, sorted by block; copied
 *   n      — how many; 0 clears
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_set_hits(OnDocLayout *layout, const OnDocHit *hits,
                            size_t n);

/* ── Geometry.  Every call lays out what it needs first. ────────────────── */

/* ---------------------------------------------------------------------------
 * on_doc_layout_height() — the content height, margins included: the height
 * the view gives itself.
 *
 * Inputs:
 *   layout — the layout
 *
 * Output: points.
 * ------------------------------------------------------------------------- */
double on_doc_layout_height(OnDocLayout *layout);

/* ---------------------------------------------------------------------------
 * on_doc_layout_caret_rect() — the caret at a position: a 1 pt wide box.
 * For an image or table block, offset 0 is its left edge and 1 its right.
 *
 * Inputs:
 *   layout — the layout
 *   pos    — the position
 *   out    — receives the box
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_caret_rect(OnDocLayout *layout, OnPos pos, on_rect_t *out);

/* What a point lands on.                                                    */
typedef enum {
    ON_HIT_TEXT,                     /* a text position (pos)               */
    ON_HIT_IMAGE,                    /* an image: block, or inline at pos   */
    ON_HIT_CHECKBOX,                 /* a task block's box                  */
    ON_HIT_COPY,                     /* a code run's "copy" word            */
    ON_HIT_OBJECT,                   /* a table's border / outside a cell   */
} OnHitKind;

typedef struct {
    OnHitKind kind;
    OnPos     pos;                   /* the nearest text position, always   */
    guint     block;                 /* the block hit                       */
    gint      image_ord;             /* ON_HIT_IMAGE: the image's ordinal   */
    bool      inline_image;          /* ON_HIT_IMAGE: inline, not a block   */
} OnDocHitResult;

/* ---------------------------------------------------------------------------
 * on_doc_layout_hit() — what is at a point.  Past the last block is the end
 * of the document; above the first, its start.
 *
 * Inputs:
 *   layout — the layout
 *   x, y   — the point, content coordinates
 *   out    — receives the result
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_hit(OnDocLayout *layout, double x, double y,
                       OnDocHitResult *out);

/* Cursor movements.                                                         */
typedef enum {
    ON_MOVE_CHAR_LEFT, ON_MOVE_CHAR_RIGHT,
    ON_MOVE_WORD_LEFT, ON_MOVE_WORD_RIGHT,
    ON_MOVE_LINE_START, ON_MOVE_LINE_END,     /* the visual line           */
    ON_MOVE_LINE_UP, ON_MOVE_LINE_DOWN,
    ON_MOVE_BLOCK_START, ON_MOVE_BLOCK_END,
    ON_MOVE_DOC_START, ON_MOVE_DOC_END,
} OnDocMove;

/* ---------------------------------------------------------------------------
 * on_doc_layout_move() — where a movement takes the caret.
 *
 * Inputs:
 *   layout — the layout
 *   pos    — where from
 *   how    — the movement
 *   goal_x — in/out: the x a run of vertical moves aims at; pass -1 to take
 *            it from `pos`.  Horizontal moves reset it to -1.
 *
 * Output: the new position; `pos` itself at an end of the document.
 * ------------------------------------------------------------------------- */
OnPos on_doc_layout_move(OnDocLayout *layout, OnPos pos, OnDocMove how,
                         double *goal_x);

/* ---------------------------------------------------------------------------
 * on_doc_layout_word_at() / on_doc_layout_line_at() — the word, or the
 * visual line, around a position (a double and a triple click).
 *
 * Inputs:
 *   layout — the layout
 *   pos    — the position
 *   a, b   — receive the range's ends, a <= b
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_word_at(OnDocLayout *layout, OnPos pos, OnPos *a, OnPos *b);
void on_doc_layout_line_at(OnDocLayout *layout, OnPos pos, OnPos *a, OnPos *b);

/* ---------------------------------------------------------------------------
 * on_doc_layout_image() — the decoded image of an IMAGE block, or of an
 * inline image of a text block.  Decoded on first use (image_io.h) and
 * cached on the block (OnBlock.pixels, released with it by CGImageRelease).
 *
 * Inputs:
 *   layout     — the layout
 *   block      — the block
 *   inline_ord — -1 for an IMAGE block; else the index of the inline image
 *                within the block's text
 *
 * Output: the image, BORROWED (do not release; valid while the block
 *         lives), or NULL when the bytes will not decode.
 * ------------------------------------------------------------------------- */
CGImageRef on_doc_layout_image(OnDocLayout *layout, guint block,
                               gint inline_ord);

/* ---------------------------------------------------------------------------
 * on_doc_layout_code_run() — for a CODE block, the run of consecutive CODE
 * blocks it belongs to (what the "copy" word copies).
 *
 * Inputs:
 *   layout — the layout
 *   block  — a CODE block
 *   first  — receives the run's first block
 *   count  — receives its length
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_code_run(OnDocLayout *layout, guint block, guint *first,
                            guint *count);

/* ── Painting ───────────────────────────────────────────────────────────── */

typedef struct {
    on_rect_t clip;                  /* the visible content area            */
    bool      has_sel;
    OnPos     sel_a, sel_b;          /* ordered                             */
    bool      caret_visible;
    OnPos     caret;
    bool      focused;               /* selection colour                    */
} OnDocPaint;

/* ---------------------------------------------------------------------------
 * on_doc_layout_draw() — paint the blocks intersecting the clip, then the
 * selection and the caret.  The page background is the view's to fill.
 *
 * Inputs:
 *   layout — the layout
 *   ctx    — a FLIPPED context whose origin is the content's origin (what
 *            a flipped NSView's drawRect: has)
 *   paint  — what to show
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_doc_layout_draw(OnDocLayout *layout, CGContextRef ctx,
                        const OnDocPaint *paint);

#endif /* ON_DOC_LAYOUT_H */
