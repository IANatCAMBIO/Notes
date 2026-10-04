/* ===========================================================================
 * text_layout.c — STUB (work package B replaces this file)
 *
 * Satisfies text_layout.h with a layout that has no extent, so the app
 * links and runs while the CoreText backend is written.  Nothing calls it
 * until doc_layout.c is real.  The behaviour to implement is the contract
 * in text_layout.h; the Pango code it replaces is
 * porting/gtk_reference/src/doc_layout.c (text_layout_new and every
 * pango_layout_* call), mapped to CoreText in
 * porting/GTK_PORTING_MAP.md section 3.4.
 * =========================================================================== */

#include "text_layout.h"

#include <stdlib.h>

/* The stub keeps only the text's length, to clamp cursor steps.            */
struct on_text_layout {
    size_t len;
};

/*
 * on_text_layout_new — see text_layout.h (stub).
 */
on_text_layout_t *
on_text_layout_new(const on_text_content_t *content,
                   const on_text_style_t *style)
{
    (void)style;
    on_text_layout_t *layout = calloc(1, sizeof *layout);
    layout->len = content->len;
    return layout;
}

/*
 * on_text_layout_free — see text_layout.h (stub).
 */
void
on_text_layout_free(on_text_layout_t *layout)
{
    free(layout);
}

/*
 * on_text_layout_size — see text_layout.h (stub).
 */
void
on_text_layout_size(const on_text_layout_t *layout, double *w, double *h)
{
    (void)layout;
    *w = 0.0;
    *h = 0.0;
}

/*
 * on_text_layout_line_count — see text_layout.h (stub).
 */
size_t
on_text_layout_line_count(const on_text_layout_t *layout)
{
    (void)layout;
    return 1;
}

/*
 * on_text_layout_line — see text_layout.h (stub).
 */
void
on_text_layout_line(const on_text_layout_t *layout, size_t line,
                    on_text_span_t *span, on_rect_t *rect)
{
    (void)line;
    if (span != NULL) {
        *span = (on_text_span_t){ 0, layout->len };
    }
    if (rect != NULL) {
        *rect = (on_rect_t){ 0 };
    }
}

/*
 * on_text_layout_cursor_rect — see text_layout.h (stub).
 */
void
on_text_layout_cursor_rect(const on_text_layout_t *layout, size_t byte,
                           on_rect_t *out)
{
    (void)layout;
    (void)byte;
    *out = (on_rect_t){ 0 };
}

/*
 * on_text_layout_index_at — see text_layout.h (stub).
 */
size_t
on_text_layout_index_at(const on_text_layout_t *layout, double x, double y)
{
    (void)layout;
    (void)x;
    (void)y;
    return 0;
}

/*
 * on_text_layout_next_cursor — see text_layout.h (stub).
 */
size_t
on_text_layout_next_cursor(const on_text_layout_t *layout, size_t byte)
{
    (void)layout;
    return byte;
}

/*
 * on_text_layout_prev_cursor — see text_layout.h (stub).
 */
size_t
on_text_layout_prev_cursor(const on_text_layout_t *layout, size_t byte)
{
    (void)layout;
    return byte;
}

/*
 * on_text_layout_next_word — see text_layout.h (stub).
 */
size_t
on_text_layout_next_word(const on_text_layout_t *layout, size_t byte)
{
    (void)layout;
    return byte;
}

/*
 * on_text_layout_prev_word — see text_layout.h (stub).
 */
size_t
on_text_layout_prev_word(const on_text_layout_t *layout, size_t byte)
{
    (void)layout;
    return byte;
}

/*
 * on_text_layout_word_at — see text_layout.h (stub).
 */
void
on_text_layout_word_at(const on_text_layout_t *layout, size_t byte,
                       on_text_span_t *span)
{
    (void)layout;
    *span = (on_text_span_t){ byte, 0 };
}

/*
 * on_text_layout_range_rects — see text_layout.h (stub).
 */
size_t
on_text_layout_range_rects(const on_text_layout_t *layout,
                           on_text_span_t span, on_rect_t *rects, size_t max)
{
    (void)layout;
    (void)span;
    (void)rects;
    (void)max;
    return 0;
}

/*
 * on_text_layout_box_rect — see text_layout.h (stub).
 */
void
on_text_layout_box_rect(const on_text_layout_t *layout, size_t box,
                        on_rect_t *out)
{
    (void)layout;
    (void)box;
    *out = (on_rect_t){ 0 };
}

/*
 * on_text_layout_draw — see text_layout.h (stub).
 */
void
on_text_layout_draw(const on_text_layout_t *layout, CGContextRef ctx,
                    double x, double y)
{
    (void)layout;
    (void)ctx;
    (void)x;
    (void)y;
}
