/* ===========================================================================
 * paint.h — the few drawing primitives the editor's painter needs
 *
 * doc_layout.c paints a note with these and with on_text_layout_draw; they
 * replace the GtkSnapshot calls of the GTK build.  Every function draws
 * into a Core Graphics context whose coordinate system is FLIPPED — origin
 * top-left, y growing downwards — which is what a flipped NSView hands its
 * drawRect:, and what the layout's content coordinates are.
 *
 * Plain C: Core Graphics is a C API.
 * =========================================================================== */

#ifndef ON_PAINT_H
#define ON_PAINT_H

#include <CoreGraphics/CoreGraphics.h>

#include "look.h"

/* A rectangle in content coordinates (points, y down).                      */
typedef struct {
    double x, y, w, h;
} on_rect_t;

/* ---------------------------------------------------------------------------
 * on_paint_fill_rect() — fill a rectangle.
 *
 * Inputs:
 *   ctx   — the context
 *   rect  — where
 *   color — with what
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_paint_fill_rect(CGContextRef ctx, on_rect_t rect, on_color_t color);

/* ---------------------------------------------------------------------------
 * on_paint_stroke_rect() — outline a rectangle with a line drawn INSIDE its
 * edge, so a 1 pt border of a w x h box covers exactly that box.
 *
 * Inputs:
 *   ctx   — the context
 *   rect  — the box
 *   color — the line's colour
 *   width — the line's width
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_paint_stroke_rect(CGContextRef ctx, on_rect_t rect, on_color_t color,
                          double width);

/* ---------------------------------------------------------------------------
 * on_paint_rounded_rect() — a rounded rectangle, filled and outlined (the
 * task checkbox).
 *
 * Inputs:
 *   ctx    — the context
 *   rect   — the box
 *   radius — corner radius
 *   fill   — fill colour; alpha 0 leaves it unfilled
 *   stroke — outline colour, drawn inside the edge
 *   width  — outline width; 0 for none
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_paint_rounded_rect(CGContextRef ctx, on_rect_t rect, double radius,
                           on_color_t fill, on_color_t stroke, double width);

/* ---------------------------------------------------------------------------
 * on_paint_line() — a straight line (a strikethrough, a checkbox tick).
 *
 * Inputs:
 *   ctx            — the context
 *   x1, y1, x2, y2 — its ends
 *   color          — its colour
 *   width          — its width
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_paint_line(CGContextRef ctx, double x1, double y1, double x2,
                   double y2, on_color_t color, double width);

/* ---------------------------------------------------------------------------
 * on_paint_image() — draw an image scaled into a rectangle, the right way
 * up in the flipped context.
 *
 * Inputs:
 *   ctx   — the context
 *   image — the image (borrowed); NULL draws nothing
 *   rect  — where
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_paint_image(CGContextRef ctx, CGImageRef image, on_rect_t rect);

#endif /* ON_PAINT_H */
