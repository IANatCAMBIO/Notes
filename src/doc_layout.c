/* ===========================================================================
 * doc_layout.c — STUB (work package B replaces this file)
 *
 * Satisfies doc_layout.h with a layout of no extent that paints nothing, so
 * the app links and runs while the real one is ported.  The specification
 * is porting/gtk_reference/src/doc_layout.c: carry its geometry (block
 * positions, code runs, image fit, table fitting, hit testing, cross-block
 * cursor movement) and replace its Pango and GSK calls with text_layout.h
 * and paint.h.
 * =========================================================================== */

#include "doc_layout.h"

#include <stdlib.h>

/* The stub remembers only which document it is for.                        */
struct OnDocLayout {
    OnDocument *doc;
};

/*
 * on_doc_layout_new — see doc_layout.h (stub).
 */
OnDocLayout *
on_doc_layout_new(OnDocument *doc, double font_size)
{
    (void)font_size;
    OnDocLayout *layout = calloc(1, sizeof *layout);
    layout->doc = doc;
    return layout;
}

/*
 * on_doc_layout_free — see doc_layout.h (stub).
 */
void
on_doc_layout_free(OnDocLayout *layout)
{
    free(layout);
}

/*
 * on_doc_layout_set_width — see doc_layout.h (stub).
 */
void
on_doc_layout_set_width(OnDocLayout *layout, double width)
{
    (void)layout;
    (void)width;
}

/*
 * on_doc_layout_set_style — see doc_layout.h (stub).
 */
void
on_doc_layout_set_style(OnDocLayout *layout, const OnDocLayoutStyle *style)
{
    (void)layout;
    (void)style;
}

/*
 * on_doc_layout_set_document — see doc_layout.h (stub).
 */
void
on_doc_layout_set_document(OnDocLayout *layout, OnDocument *doc)
{
    layout->doc = doc;
}

/*
 * on_doc_layout_block_changed — see doc_layout.h (stub).
 */
void
on_doc_layout_block_changed(OnDocLayout *layout, guint i)
{
    (void)layout;
    (void)i;
}

/*
 * on_doc_layout_blocks_inserted — see doc_layout.h (stub).
 */
void
on_doc_layout_blocks_inserted(OnDocLayout *layout, guint i, guint n)
{
    (void)layout;
    (void)i;
    (void)n;
}

/*
 * on_doc_layout_blocks_removed — see doc_layout.h (stub).
 */
void
on_doc_layout_blocks_removed(OnDocLayout *layout, guint i, guint n)
{
    (void)layout;
    (void)i;
    (void)n;
}

/*
 * on_doc_layout_set_preedit — see doc_layout.h (stub).
 */
void
on_doc_layout_set_preedit(OnDocLayout *layout, OnPos pos, const char *text,
                          size_t cursor)
{
    (void)layout;
    (void)pos;
    (void)text;
    (void)cursor;
}

/*
 * on_doc_layout_set_hits — see doc_layout.h (stub).
 */
void
on_doc_layout_set_hits(OnDocLayout *layout, const OnDocHit *hits, size_t n)
{
    (void)layout;
    (void)hits;
    (void)n;
}

/*
 * on_doc_layout_height — see doc_layout.h (stub).
 */
double
on_doc_layout_height(OnDocLayout *layout)
{
    (void)layout;
    return 0.0;
}

/*
 * on_doc_layout_caret_rect — see doc_layout.h (stub).
 */
void
on_doc_layout_caret_rect(OnDocLayout *layout, OnPos pos, on_rect_t *out)
{
    (void)layout;
    (void)pos;
    *out = (on_rect_t){ 0 };
}

/*
 * on_doc_layout_hit — see doc_layout.h (stub).
 */
void
on_doc_layout_hit(OnDocLayout *layout, double x, double y,
                  OnDocHitResult *out)
{
    (void)layout;
    (void)x;
    (void)y;
    *out = (OnDocHitResult){ .kind = ON_HIT_TEXT, .pos = { 0, -1, 0 } };
}

/*
 * on_doc_layout_move — see doc_layout.h (stub).
 */
OnPos
on_doc_layout_move(OnDocLayout *layout, OnPos pos, OnDocMove how,
                   double *goal_x)
{
    (void)layout;
    (void)how;
    (void)goal_x;
    return pos;
}

/*
 * on_doc_layout_word_at — see doc_layout.h (stub).
 */
void
on_doc_layout_word_at(OnDocLayout *layout, OnPos pos, OnPos *a, OnPos *b)
{
    (void)layout;
    *a = pos;
    *b = pos;
}

/*
 * on_doc_layout_line_at — see doc_layout.h (stub).
 */
void
on_doc_layout_line_at(OnDocLayout *layout, OnPos pos, OnPos *a, OnPos *b)
{
    (void)layout;
    *a = pos;
    *b = pos;
}

/*
 * on_doc_layout_image — see doc_layout.h (stub).
 */
CGImageRef
on_doc_layout_image(OnDocLayout *layout, guint block, gint inline_ord)
{
    (void)layout;
    (void)block;
    (void)inline_ord;
    return NULL;
}

/*
 * on_doc_layout_code_run — see doc_layout.h (stub).
 */
void
on_doc_layout_code_run(OnDocLayout *layout, guint block, guint *first,
                       guint *count)
{
    (void)layout;
    *first = block;
    *count = 1;
}

/*
 * on_doc_layout_draw — see doc_layout.h (stub).
 */
void
on_doc_layout_draw(OnDocLayout *layout, CGContextRef ctx,
                   const OnDocPaint *paint)
{
    (void)layout;
    (void)ctx;
    (void)paint;
}
