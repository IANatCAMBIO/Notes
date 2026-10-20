/* ===========================================================================
 * file_icon.m — the system's icon for a kind of file (see file_icon.h)
 * =========================================================================== */

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "file_icon.h"

/*
 * on_file_icon — see file_icon.h.
 */
CGImageRef
on_file_icon(const char *name, double points, double scale)
{
    NSString *ext  = [@(name) pathExtension];
    UTType   *type = ext.length > 0 ? [UTType typeWithFilenameExtension:ext] : nil;
    NSImage  *icon = [NSWorkspace.sharedWorkspace iconForContentType:type ?: UTTypeData];

    size_t         px = (size_t)ceil(points * scale);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef   ctx = CGBitmapContextCreate(NULL, px, px, 8, 0, cs,
                                               (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (ctx == NULL) return NULL;

    NSGraphicsContext *previous = NSGraphicsContext.currentContext;
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:ctx
                                                                               flipped:NO];
    [icon drawInRect:NSMakeRect(0, 0, px, px) fromRect:NSZeroRect
           operation:NSCompositingOperationSourceOver fraction:1.0];
    NSGraphicsContext.currentContext = previous;

    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return image;
}
