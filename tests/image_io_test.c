/* ===========================================================================
 * image_io_test.c — the neutral image API: decode (capped), probe, encode
 * =========================================================================== */

#include "check.h"

#include "document.h"
#include "image_io.h"
#include "serialize.h"

#include <unistd.h>

/* The picture every case starts from.                                       */
#define SRC_W 64
#define SRC_H 48

/*
 * make_image — a SRC_W x SRC_H opaque image drawn into a bitmap context.
 *
 * Output: a new image (+1).
 */
static CGImageRef
make_image(void)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, SRC_W, SRC_H, 8, 0, space,
                                             kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(ctx, 0.2, 0.5, 0.9, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, SRC_W, SRC_H));
    CGContextSetRGBFillColor(ctx, 1.0, 0.8, 0.1, 1.0);
    CGContextFillRect(ctx, CGRectMake(8, 8, 20, 12));
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(space);
    return image;
}

/*
 * size_is — does a decoded image have these pixel dimensions?
 *
 * Inputs:
 *   image — the image, or NULL
 *   w, h  — the expected size
 *
 * Output: true when it does.
 */
static bool
size_is(CGImageRef image, size_t w, size_t h)
{
    return image != NULL && CGImageGetWidth(image) == w &&
           CGImageGetHeight(image) == h;
}

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    const char *data_dir = check_data_dir();

    CGImageRef source = make_image();
    size_t   n_png = 0;
    uint8_t *png   = on_image_png_encode(source, &n_png);
    CGImageRelease(source);
    check(png != NULL && n_png > 8 && memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0,
          "an image encodes to PNG bytes");

    int w = 0, h = 0;
    check(on_image_probe_size(png, n_png, &w, &h) && w == SRC_W && h == SRC_H,
          "the header probe reports the pixel size");
    check(on_image_probe_size(png, n_png, NULL, NULL), "the probe's outputs are optional");

    CGImageRef full = on_image_decode_capped(png, n_png, 0);
    check(size_is(full, SRC_W, SRC_H), "max_px 0 decodes at full resolution");
    CGImageRelease(full);

    CGImageRef small = on_image_decode_capped(png, n_png, 16);
    check(size_is(small, 16, 12), "a cap shrinks the longest side, keeping the aspect");
    CGImageRelease(small);

    CGImageRef capped_up = on_image_decode_capped(png, n_png, 500);
    check(size_is(capped_up, SRC_W, SRC_H), "a cap larger than the image never upscales");
    CGImageRelease(capped_up);

    /* The decoder copies its input: the image must survive the buffer.     */
    uint8_t *copy = malloc(n_png);
    memcpy(copy, png, n_png);
    CGImageRef outlives = on_image_decode_capped(copy, n_png, 0);
    memset(copy, 0, n_png);
    free(copy);
    size_t   n_again = 0;
    uint8_t *again   = on_image_png_encode(outlives, &n_again);
    check(again != NULL && on_image_probe_size(again, n_again, &w, &h) && w == SRC_W,
          "a decoded image does not depend on the caller's buffer");
    free(again);
    CGImageRelease(outlives);

    const uint8_t junk[] = "this is not an image at all";
    check(on_image_decode_capped(junk, sizeof junk, 0) == NULL, "junk does not decode");
    check(!on_image_probe_size(junk, sizeof junk, &w, &h), "junk does not probe");
    check(on_image_decode_capped(NULL, 0, 0) == NULL && !on_image_probe_size(NULL, 0, &w, &h),
          "no bytes is a failure, not a crash");

    /* A file of any readable format becomes PNG bytes.                     */
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/picture.png", data_dir);
    FILE *f = fopen(path, "wb");
    fwrite(png, 1, n_png, f);
    fclose(f);
    size_t   n_file = 0;
    uint8_t *from_file = on_image_file_to_png(path, &n_file);
    check(from_file != NULL && on_image_probe_size(from_file, n_file, &w, &h) &&
          w == SRC_W && h == SRC_H, "an image file loads as PNG bytes");
    free(from_file);

    snprintf(path, sizeof path, "%s/not-an-image.txt", data_dir);
    f = fopen(path, "wb");
    fputs("plain text\n", f);
    fclose(f);
    check(on_image_file_to_png(path, &n_file) == NULL && n_file == 0,
          "a file that is not an image is refused");
    snprintf(path, sizeof path, "%s/missing.png", data_dir);
    check(on_image_file_to_png(path, &n_file) == NULL, "a missing file is refused");

    /* Through a note: the stored bytes come back verbatim, and an ordinal
     * past the end is none.                                                */
    GBytes     *bytes = g_bytes_new(png, n_png);
    OnDocument *doc   = on_document_new();
    on_document_insert_block(doc, 1, on_block_new_image(bytes, 0));
    gsize   blob_len = 0;
    guint8 *blob     = on_document_to_bnbf(doc, &blob_len);
    check(on_note_count_images(blob, blob_len) == 1, "the blob holds one image");
    GBytes *stored = on_note_image_nth_png(blob, blob_len, 0);
    check(stored != NULL && g_bytes_equal(stored, bytes), "stored PNG bytes are verbatim");
    check(on_note_image_nth_png(blob, blob_len, 1) == NULL, "an ordinal past the end is NULL");
    g_bytes_unref(stored);
    g_free(blob);
    on_document_free(doc);
    g_bytes_unref(bytes);

    free(png);
    return check_result();
}
