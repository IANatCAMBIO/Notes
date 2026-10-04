/* ===========================================================================
 * image_io.h — the one image type of the core, and its four operations
 *
 * An image inside a note is PNG bytes (OnBlock.png, OnInlineImage.png): the
 * model, the database, the exporter and the CLI never hold anything else,
 * and stored bytes are written back verbatim, never re-encoded.  Pixels are
 * needed only to draw, and for that there is exactly one decoded type, a
 * CGImageRef, produced and consumed here through ImageIO.  (The GTK build
 * used GdkPixbuf; nothing of it remains.)
 *
 * OWNERSHIP across this boundary follows the Core Foundation "Create" rule,
 * with no exceptions:
 *   - a function that RETURNS a CGImageRef returns it +1: the caller owns
 *     it and must CGImageRelease it (or hand it to an owner that will — the
 *     model's OnBlock.pixels slot with pixels_free = (GDestroyNotify)
 *     CGImageRelease);
 *   - a function that TAKES a CGImageRef borrows it for the call;
 *   - byte buffers returned are malloc'd and belong to the caller: free().
 * Under ARC a CGImageRef is not managed: wrap it in an NSImage (which
 * retains it) and release your own reference in the same scope.
 *
 * Every function is thread-safe and may be called from a worker.
 * =========================================================================== */

#ifndef ON_IMAGE_IO_H
#define ON_IMAGE_IO_H

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * on_image_decode_capped() — decode encoded image bytes (PNG as stored; any
 * format ImageIO reads is accepted), shrunk DURING decode so its longest
 * side is at most max_px.  THE one decode path: thumbnails ask for their
 * display size in pixels, so a 12-megapixel screenshot is never inflated to
 * be shown in a 140-point card.  Never upscales.
 *
 * Inputs:
 *   bytes  — the encoded image; copied, so it need not outlive the call
 *   n      — its length
 *   max_px — cap on the longest side in pixels; 0 = full resolution
 *
 * Output: a new image (+1; CGImageRelease it), or NULL when the bytes will
 *         not decode.
 * ------------------------------------------------------------------------- */
CGImageRef on_image_decode_capped(const uint8_t *bytes, size_t n, int max_px);

/* ---------------------------------------------------------------------------
 * on_image_probe_size() — an encoded image's pixel dimensions from its
 * header; no pixel is decoded.
 *
 * Inputs:
 *   bytes — the encoded image
 *   n     — its length
 *   w, h  — receive the dimensions; either may be NULL
 *
 * Output: true when the header parsed (then *w and *h are > 0); false
 *         otherwise, with *w and *h untouched.
 * ------------------------------------------------------------------------- */
bool on_image_probe_size(const uint8_t *bytes, size_t n, int *w, int *h);

/* ---------------------------------------------------------------------------
 * on_image_png_encode() — an image as PNG bytes.  Called ONCE per image, when
 * it enters a note (a paste, Insert Image); from then on the bytes are the
 * image.
 *
 * Inputs:
 *   image — the image (borrowed)
 *   n_out — receives the byte count
 *
 * Output: a new buffer (free() it), or NULL when encoding failed.
 * ------------------------------------------------------------------------- */
uint8_t *on_image_png_encode(CGImageRef image, size_t *n_out);

/* ---------------------------------------------------------------------------
 * on_image_file_to_png() — an image FILE of any format ImageIO reads, as
 * PNG bytes at full resolution: what Insert Image and `note add-image`
 * store.
 *
 * Inputs:
 *   path  — the file
 *   n_out — receives the byte count
 *
 * Output: a new buffer (free() it), or NULL when the file is unreadable or
 *         is not an image.
 * ------------------------------------------------------------------------- */
uint8_t *on_image_file_to_png(const char *path, size_t *n_out);

#endif /* ON_IMAGE_IO_H */
