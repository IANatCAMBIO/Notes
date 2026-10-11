/* ===========================================================================
 * paint.c — drawing primitives over Core Graphics (see paint.h)
 * =========================================================================== */

#include "paint.h"

/*
 * cg_rect — an on_rect_t as a CGRect.
 *
 * Inputs:
 *   rect — the rectangle
 *
 * Output: the same rectangle.
 */
static CGRect
cg_rect(on_rect_t rect)
{
    return CGRectMake(rect.x, rect.y, rect.w, rect.h);
}

/*
 * on_paint_fill_rect — see paint.h.
 */
void
on_paint_fill_rect(CGContextRef ctx, on_rect_t rect, on_color_t color)
{
    CGContextSetRGBFillColor(ctx, color.r, color.g, color.b, color.a);
    CGContextFillRect(ctx, cg_rect(rect));
}

/*
 * on_paint_rounded_rect — see paint.h.
 */
void
on_paint_rounded_rect(CGContextRef ctx, on_rect_t rect, double radius,
                      on_color_t fill, on_color_t stroke, double width)
{
    if (fill.a > 0.0) {
        CGPathRef path = CGPathCreateWithRoundedRect(cg_rect(rect), radius,
                                                     radius, NULL);
        CGContextSetRGBFillColor(ctx, fill.r, fill.g, fill.b, fill.a);
        CGContextAddPath(ctx, path);
        CGContextFillPath(ctx);
        CGPathRelease(path);
    }
    if (width > 0.0) {
        double    inset = width / 2.0;
        CGPathRef path  = CGPathCreateWithRoundedRect(
            CGRectInset(cg_rect(rect), inset, inset), radius - inset,
            radius - inset, NULL);
        CGContextSetRGBStrokeColor(ctx, stroke.r, stroke.g, stroke.b, stroke.a);
        CGContextSetLineWidth(ctx, width);
        CGContextAddPath(ctx, path);
        CGContextStrokePath(ctx);
        CGPathRelease(path);
    }
}

/*
 * on_paint_line — see paint.h.
 */
void
on_paint_line(CGContextRef ctx, double x1, double y1, double x2, double y2,
              on_color_t color, double width)
{
    CGContextSetRGBStrokeColor(ctx, color.r, color.g, color.b, color.a);
    CGContextSetLineWidth(ctx, width);
    CGContextMoveToPoint(ctx, x1, y1);
    CGContextAddLineToPoint(ctx, x2, y2);
    CGContextStrokePath(ctx);
}

/*
 * on_paint_image — see paint.h.
 */
void
on_paint_image(CGContextRef ctx, CGImageRef image, on_rect_t rect)
{
    if (image == NULL) {
        return;
    }
    /* CGContextDrawImage draws with the image's first row at the rect's
     * maximum y; in a flipped context that is upside down, so flip the
     * rectangle's own space back for the one call.                         */
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, rect.x, rect.y + rect.h);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, rect.w, rect.h), image);
    CGContextRestoreGState(ctx);
}
