/* ===========================================================================
 * doc_layout.h — OnDocLayout: the geometry of an OnDocument on screen
 *
 * One OnDocLayout per view.  For every block it keeps a PangoLayout (text
 * kinds and table cells), a height and a y offset, and it answers the
 * three questions a view has: where does this position sit (caret rect),
 * what is under this point (hit test), and where does a cursor movement
 * land (navigation).  It also paints: text through
 * gtk_snapshot_append_layout — the path every GTK text widget draws
 * through — images as textures, tables, code shading, list prefixes,
 * checkboxes, selection and caret.
 *
 * Everything the layout knows comes from the document and the style the
 * view sets; the view relays the document's observer notifications so
 * exactly the changed block re-lays out.  Coordinates are CONTENT
 * coordinates: x from the widget's left edge, y from the top of the
 * content (the view subtracts its scroll offset).
 * =========================================================================== */

#ifndef ON_DOC_LAYOUT_H
#define ON_DOC_LAYOUT_H

#include <gtk/gtk.h>

#include "document.h"

typedef struct OnDocLayout OnDocLayout;

/* What the view's settings decide about the look.                          */
typedef struct {
    gboolean title_line;             /* block 0 centred + heading-sized     */
    gboolean code_numbers;           /* line numbers in the code gutter     */
    gboolean code_copy;              /* the "copy" word on a code run       */
} OnDocLayoutStyle;

/* on_doc_layout_new() — a layout for `doc`, measuring with the widget's
 * Pango context (fonts, DPI, direction).  The layout keeps a reference on
 * neither: the view owns both and frees the layout first.                */
OnDocLayout *on_doc_layout_new(OnDocument *doc, PangoContext *ctx);

/* on_doc_layout_free() — release every cached PangoLayout.                */
void on_doc_layout_free(OnDocLayout *L);

/* on_doc_layout_set_width() — the widget's allocated width; a change
 * invalidates every block.                                                */
void on_doc_layout_set_width(OnDocLayout *L, gint width);

/* on_doc_layout_set_style() — the look; a change invalidates everything.  */
void on_doc_layout_set_style(OnDocLayout *L, const OnDocLayoutStyle *style);

/* on_doc_layout_set_document() — after a load: a new document, all stale. */
void on_doc_layout_set_document(OnDocLayout *L, OnDocument *doc);

/* The document observer, relayed by the view.                             */
void on_doc_layout_block_changed(OnDocLayout *L, guint i);
void on_doc_layout_blocks_inserted(OnDocLayout *L, guint i, guint n);
void on_doc_layout_blocks_removed(OnDocLayout *L, guint i, guint n);

/* on_doc_layout_invalidate_all() — everything re-lays out (font change). */
void on_doc_layout_invalidate_all(OnDocLayout *L);

/* ---------------------------------------------------------------------------
 * on_doc_layout_set_preedit() — an input method's uncommitted text, shown
 * spliced into the block at `pos` (its offset), styled by `attrs`; NULL
 * text clears it.  `cursor` is the caret's byte offset inside the preedit.
 * ------------------------------------------------------------------------- */
void on_doc_layout_set_preedit(OnDocLayout *L, OnPos pos, const gchar *text,
                               PangoAttrList *attrs, gint cursor);

/* A find hit: `len` bytes at `start` of a block's text (cell -1) or cell. */
typedef struct {
    guint block;
    gint  cell;
    gsize start, len;
} OnDocHit;

/* on_doc_layout_set_hits() — the find highlights (OnDocHit, sorted by
 * block); the layout takes its own copy.  NULL clears.                    */
void on_doc_layout_set_hits(OnDocLayout *L, const GArray *hits);

/* ---------------------------------------------------------------------------
 * GEOMETRY.  Every call validates what it needs first.
 * ------------------------------------------------------------------------- */

/* on_doc_layout_height() — the content height, margins included.          */
gint on_doc_layout_height(OnDocLayout *L);

/* on_doc_layout_block_rect() — a block's box (spacing excluded).           */
void on_doc_layout_block_rect(OnDocLayout *L, guint i, graphene_rect_t *out);

/* on_doc_layout_caret_rect() — the caret at `pos`: a 1 px wide box.  For
 * an object block offset 0 is its left edge, 1 its right.                 */
void on_doc_layout_caret_rect(OnDocLayout *L, OnPos pos,
                              graphene_rect_t *out);

/* What a point lands on.                                                  */
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
    gboolean  inline_image;          /* ON_HIT_IMAGE: inline, not a block   */
} OnDocHitResult;

/* on_doc_layout_hit() — what is at (x, y); past the last block, the end
 * of the document; above the first, its start.                            */
void on_doc_layout_hit(OnDocLayout *L, gdouble x, gdouble y,
                       OnDocHitResult *out);

/* Cursor movements.                                                       */
typedef enum {
    ON_MOVE_CHAR_LEFT, ON_MOVE_CHAR_RIGHT,
    ON_MOVE_WORD_LEFT, ON_MOVE_WORD_RIGHT,
    ON_MOVE_LINE_START, ON_MOVE_LINE_END,     /* the visual line           */
    ON_MOVE_LINE_UP, ON_MOVE_LINE_DOWN,
    ON_MOVE_BLOCK_START, ON_MOVE_BLOCK_END,
    ON_MOVE_DOC_START, ON_MOVE_DOC_END,
} OnDocMove;

/* on_doc_layout_move() — where `how` takes the caret from `pos`.  `goal_x`
 * (in/out) is the x a run of vertical moves aims at: -1 to take it from
 * `pos`; horizontal moves reset it to -1.  Returns pos itself at an end. */
OnPos on_doc_layout_move(OnDocLayout *L, OnPos pos, OnDocMove how,
                         gint *goal_x);

/* on_doc_layout_word_at() — the word around `pos` (a double click).       */
void on_doc_layout_word_at(OnDocLayout *L, OnPos pos, OnPos *a, OnPos *b);

/* on_doc_layout_line_at() — the visual line around `pos` (triple click). */
void on_doc_layout_line_at(OnDocLayout *L, OnPos pos, OnPos *a, OnPos *b);

/* on_doc_layout_image_texture() — the decoded image of an IMAGE block or
 * the `inline_ord`-th inline image of a text block (borrowed, cached on
 * the block), or NULL when the bytes will not decode.                     */
GdkTexture *on_doc_layout_image_texture(OnDocLayout *L, guint block,
                                        gint inline_ord);

/* ---------------------------------------------------------------------------
 * PAINTING
 * ------------------------------------------------------------------------- */
typedef struct {
    graphene_rect_t clip;            /* the visible content area            */
    gboolean has_sel;
    OnPos    sel_a, sel_b;           /* ordered                             */
    gboolean caret_visible;
    OnPos    caret;
    gboolean focused;                /* selection colour                    */
    GdkRGBA  fg;                     /* text colour                         */
} OnDocPaint;

/* on_doc_layout_snapshot() — paint the blocks intersecting `p->clip`.     */
void on_doc_layout_snapshot(OnDocLayout *L, GtkSnapshot *snap,
                            const OnDocPaint *p);

/* on_doc_layout_code_run() — for a CODE block, the index of the first
 * block of its run and the run's length (what the "copy" word copies).  */
void on_doc_layout_code_run(OnDocLayout *L, guint block, guint *first,
                            guint *count);

#endif /* ON_DOC_LAYOUT_H */
