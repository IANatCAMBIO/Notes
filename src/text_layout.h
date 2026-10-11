/* ===========================================================================
 * text_layout.h — one laid-out piece of text: the CoreText seam
 *
 * An on_text_layout_t is the text of ONE block (or one table cell), shaped,
 * wrapped to a width and ready to be measured, queried and drawn.  It is
 * what a PangoLayout was in the GTK build, and it is the ONLY place the
 * editor touches CoreText: doc_layout.c holds the geometry of a document
 * (which block is where, lists, tables, code shading, images) and asks
 * these functions everything about text.
 *
 * EVERY INDEX CROSSING THIS INTERFACE IS A UTF-8 BYTE OFFSET into the text
 * the layout was built from, on a character boundary.  The backend converts
 * to and from CoreText's UTF-16 inside (utf_index.h); a caller never sees a
 * UTF-16 index.  This is the byte-offset rule of the whole editor.
 *
 * Coordinates are points, relative to the layout's own top-left corner, y
 * growing downwards.
 *
 * text_layout.c implements it on CoreText (CTTypesetter line breaking with
 * word wrap and a character fallback, CTLine metrics, CFStringTokenizer
 * word boundaries, composed-character cursor positions, a CTRunDelegate per
 * inline box, hand-drawn strikethrough and highlight rectangles).
 * =========================================================================== */

#ifndef ON_TEXT_LAYOUT_H
#define ON_TEXT_LAYOUT_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>

#include "document.h"                /* OnRun, ON_FMT_* flags               */
#include "paint.h"                   /* on_rect_t                           */

typedef struct on_text_layout on_text_layout_t;

/* A byte range of the text.                                                 */
typedef struct {
    size_t start, len;
} on_text_span_t;

/* An inline object: the U+FFFC at `offset` is drawn as an empty box of this
 * size (doc_layout paints the image into on_text_layout_box_rect).          */
typedef struct {
    size_t offset;
    double width, height;
} on_text_box_t;

/* What to lay out.  Everything is borrowed for the call.                    */
typedef struct {
    const char           *text;      /* UTF-8, an input method's uncommitted
                                        text already spliced in             */
    size_t                len;       /* bytes                                */
    const OnRun          *runs;      /* inline styles, tiling `len` exactly  */
    size_t                n_runs;
    const on_text_box_t  *boxes;     /* inline objects, ascending offset     */
    size_t                n_boxes;
    const on_text_span_t *hits;      /* find highlights, ascending           */
    size_t                n_hits;
    on_text_span_t        marked;    /* the uncommitted text, underlined;
                                        len 0 = none                         */
} on_text_content_t;

/* How to lay it out.                                                        */
typedef struct {
    double font_size;                /* body size in points (look.h)         */
    double scale;                    /* 1.0, or a heading scale              */
    double width;                    /* wrap width; <= 0 = never wrap        */
    bool   mono;                     /* code: the monospaced system font     */
    bool   bold;                     /* headings, header cells               */
    bool   centered;                 /* the derived title line               */
    bool   action;                   /* a '!' line: action colour, semibold  */
    bool   plain;                    /* code: run flags and tags not shown   */
    bool   muted;                    /* code line numbers: their grey        */
    bool   dark;                     /* dark appearance (look.h colours)     */
} on_text_style_t;

/* ---------------------------------------------------------------------------
 * on_text_layout_new() — lay text out.
 *
 * Inputs:
 *   content — the text and what decorates it (copied; nothing is kept)
 *   style   — font, width and look
 *
 * Output: a new layout; on_text_layout_free it.  Never NULL: empty text is
 *         one empty line with the font's height.
 * ------------------------------------------------------------------------- */
on_text_layout_t *on_text_layout_new(const on_text_content_t *content,
                                     const on_text_style_t *style);

/* ---------------------------------------------------------------------------
 * on_text_layout_free() — release a layout and every CoreText object it
 * holds.
 *
 * Inputs:
 *   layout — the layout; NULL is ignored
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_free(on_text_layout_t *layout);

/* ---------------------------------------------------------------------------
 * on_text_layout_size() — the layout's size: its widest line and the sum of
 * its line heights, both rounded UP to whole points so that hit testing and
 * painting agree.
 *
 * Inputs:
 *   layout — the layout
 *   w, h   — receive the size
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_size(const on_text_layout_t *layout, double *w, double *h);

/* ---------------------------------------------------------------------------
 * on_text_layout_line_count() — how many visual lines the text wrapped to.
 *
 * Inputs:
 *   layout — the layout
 *
 * Output: at least 1.
 * ------------------------------------------------------------------------- */
size_t on_text_layout_line_count(const on_text_layout_t *layout);

/* ---------------------------------------------------------------------------
 * on_text_layout_line() — one visual line.
 *
 * Inputs:
 *   layout — the layout
 *   line   — its index, < on_text_layout_line_count
 *   span   — receives the bytes on the line (a wrapped line includes the
 *            space it broke at); may be NULL
 *   rect   — receives the line's box: x and w of its ink, y and h of the
 *            line; may be NULL
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_line(const on_text_layout_t *layout, size_t line,
                         on_text_span_t *span, on_rect_t *rect);

/* ---------------------------------------------------------------------------
 * on_text_layout_cursor_rect() — where a caret at a byte offset is drawn.
 * At a wrap point the offset belongs to the START of the next line.
 *
 * Inputs:
 *   layout — the layout
 *   byte   — the offset, 0..len
 *   out    — receives x, y and height (w is 1)
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_cursor_rect(const on_text_layout_t *layout, size_t byte,
                                on_rect_t *out);

/* ---------------------------------------------------------------------------
 * on_text_layout_index_at() — the cursor position nearest a point: the line
 * is chosen by y (clamped to the first and last), the position by x, taking
 * the far side of a character when the point is past its middle.
 *
 * Inputs:
 *   layout — the layout
 *   x, y   — the point
 *
 * Output: a byte offset on a cursor position.
 * ------------------------------------------------------------------------- */
size_t on_text_layout_index_at(const on_text_layout_t *layout, double x,
                               double y);

/* ---------------------------------------------------------------------------
 * on_text_layout_next_cursor() / on_text_layout_prev_cursor() — one step by
 * composed character (an emoji sequence, a letter with combining marks, are
 * one step).
 *
 * Inputs:
 *   layout — the layout
 *   byte   — where from
 *
 * Output: the next or previous cursor position; `byte` itself at the end
 *         (or start) of the text.
 * ------------------------------------------------------------------------- */
size_t on_text_layout_next_cursor(const on_text_layout_t *layout, size_t byte);
size_t on_text_layout_prev_cursor(const on_text_layout_t *layout, size_t byte);

/* ---------------------------------------------------------------------------
 * on_text_layout_next_word() / on_text_layout_prev_word() — one step by
 * word: forward to the end of the next word, back to the start of the
 * previous one.
 *
 * Inputs:
 *   layout — the layout
 *   byte   — where from
 *
 * Output: the next word's end (forward) or the previous word's start
 *         (back) — spaces and punctuation are skipped, as Pango's word
 *         boundaries do; the text's end or start when there is no word
 *         that way.
 * ------------------------------------------------------------------------- */
size_t on_text_layout_next_word(const on_text_layout_t *layout, size_t byte);
size_t on_text_layout_prev_word(const on_text_layout_t *layout, size_t byte);

/* ---------------------------------------------------------------------------
 * on_text_layout_word_at() — the word around an offset (a double click).
 *
 * Inputs:
 *   layout — the layout
 *   byte   — an offset inside or beside the word
 *   span   — receives the word; len 0 at `byte` when there is none
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_word_at(const on_text_layout_t *layout, size_t byte,
                            on_text_span_t *span);

/* ---------------------------------------------------------------------------
 * on_text_layout_range_rects() — the rectangles covering a byte range, one
 * per visual line it touches (a selection, a find hit).
 *
 * Inputs:
 *   layout — the layout
 *   span   — the range
 *   rects  — receives up to `max` rectangles
 *   max    — capacity of `rects`
 *
 * Output: how many rectangles the range needs — which may exceed `max`; the
 *         caller then calls again with room for that many.
 * ------------------------------------------------------------------------- */
size_t on_text_layout_range_rects(const on_text_layout_t *layout,
                                  on_text_span_t span, on_rect_t *rects,
                                  size_t max);

/* ---------------------------------------------------------------------------
 * on_text_layout_box_rect() — where an inline object's box landed.
 *
 * Inputs:
 *   layout — the layout
 *   box    — its index in the content's `boxes`
 *   out    — receives the box
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_box_rect(const on_text_layout_t *layout, size_t box,
                             on_rect_t *out);

/* ---------------------------------------------------------------------------
 * on_text_layout_draw() — draw the layout: highlights, text, underlines and
 * strikethroughs.  Inline boxes are left empty for the caller.
 *
 * Inputs:
 *   layout — the layout
 *   ctx    — a FLIPPED context (see paint.h)
 *   x, y   — where the layout's top-left corner goes
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_text_layout_draw(const on_text_layout_t *layout, CGContextRef ctx,
                         double x, double y);

#endif /* ON_TEXT_LAYOUT_H */
