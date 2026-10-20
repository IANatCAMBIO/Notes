/* ===========================================================================
 * file_icon.h — the system's icon for a kind of file, as a bitmap
 *
 * Plain C to its callers (doc_layout.c draws attached files with it);
 * implemented in file_icon.m on NSWorkspace.
 * =========================================================================== */

#ifndef ON_FILE_ICON_H
#define ON_FILE_ICON_H

#include <CoreGraphics/CoreGraphics.h>

/*
 * on_file_icon — the icon the system shows for a file of this name's type,
 * rendered at the size it will be drawn (never a larger representation than
 * `points` x `scale` pixels, per the image rule of porting/INTERFACES.md §9).
 *
 * Inputs:
 *   name   — the file's name (UTF-8); only its extension matters, and an
 *            unknown or missing one gets the generic document icon
 *   points — the side of the square it is drawn at, in points
 *   scale  — device pixels per point (1.0, 2.0 …)
 *
 * Output: a new square image (CGImageRelease it), or NULL when the bitmap
 *         could not be made.  Main thread.
 */
CGImageRef on_file_icon(const char *name, double points, double scale);

#endif /* ON_FILE_ICON_H */
