/* ===========================================================================
 * image_io.c — decode, probe and encode through ImageIO (see image_io.h)
 *
 * Every Core Foundation object created here is released here, on every
 * path, except the CGImageRef a function hands to its caller.
 * =========================================================================== */

#include "image_io.h"

#include <ImageIO/ImageIO.h>
#include <stdlib.h>
#include <string.h>

/* The uniform type identifier of PNG.  Spelled out because the UTType
 * constants are Objective-C and this file is C.                             */
#define PNG_TYPE CFSTR("public.png")

/*
 * source_first_image — the first image of a source, optionally shrunk
 * during decode.
 *
 * Inputs:
 *   source — the image source (borrowed)
 *   max_px — cap on the longest side in pixels; 0 = full resolution
 *
 * Output: a new image (+1), or NULL when the source holds no decodable
 *         image.
 */
static CGImageRef
source_first_image(CGImageSourceRef source, int max_px)
{
    if (max_px <= 0) {
        return CGImageSourceCreateImageAtIndex(source, 0, NULL);
    }
    /* "FromImageAlways" makes ImageIO scale the real image rather than
     * return an embedded preview of unknown size; it never scales up.      */
    CFNumberRef cap = CFNumberCreate(NULL, kCFNumberIntType, &max_px);
    const void *keys[]   = { kCGImageSourceCreateThumbnailFromImageAlways,
                             kCGImageSourceThumbnailMaxPixelSize };
    const void *values[] = { kCFBooleanTrue, cap };
    CFDictionaryRef options = CFDictionaryCreate(
        NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, options);
    CFRelease(options);
    CFRelease(cap);
    return image;
}

/*
 * on_image_decode_capped — see image_io.h.
 */
CGImageRef
on_image_decode_capped(const uint8_t *bytes, size_t n, int max_px)
{
    if (bytes == NULL || n == 0) {
        return NULL;
    }
    /* A copy, not a no-copy wrapper: a full-resolution CGImage decodes
     * lazily and keeps its source data, which would otherwise be the
     * caller's buffer after the caller has freed it.                       */
    CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)n);
    if (data == NULL) {
        return NULL;
    }
    CGImageSourceRef source = CGImageSourceCreateWithData(data, NULL);
    CFRelease(data);
    if (source == NULL) {
        return NULL;
    }
    CGImageRef image = source_first_image(source, max_px);
    CFRelease(source);
    return image;
}

/*
 * on_image_probe_size — see image_io.h.
 */
bool
on_image_probe_size(const uint8_t *bytes, size_t n, int *w, int *h)
{
    if (bytes == NULL || n == 0) {
        return false;
    }
    /* No copy: nothing created here outlives the call.                     */
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, bytes, (CFIndex)n,
                                                 kCFAllocatorNull);
    if (data == NULL) {
        return false;
    }
    CGImageSourceRef source = CGImageSourceCreateWithData(data, NULL);
    CFRelease(data);
    if (source == NULL) {
        return false;
    }
    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(source, 0, NULL);
    CFRelease(source);
    if (props == NULL) {
        return false;
    }
    int pw = 0, ph = 0;              /* the declared pixel dimensions        */
    CFNumberRef nw = CFDictionaryGetValue(props, kCGImagePropertyPixelWidth);
    CFNumberRef nh = CFDictionaryGetValue(props, kCGImagePropertyPixelHeight);
    bool ok = nw != NULL && nh != NULL &&
              CFNumberGetValue(nw, kCFNumberIntType, &pw) &&
              CFNumberGetValue(nh, kCFNumberIntType, &ph) &&
              pw > 0 && ph > 0;
    CFRelease(props);
    if (!ok) {
        return false;
    }
    if (w != NULL) {
        *w = pw;
    }
    if (h != NULL) {
        *h = ph;
    }
    return true;
}

/*
 * on_image_png_encode — see image_io.h.
 */
uint8_t *
on_image_png_encode(CGImageRef image, size_t *n_out)
{
    *n_out = 0;
    if (image == NULL) {
        return NULL;
    }
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    if (data == NULL) {
        return NULL;
    }
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithData(data, PNG_TYPE, 1, NULL);
    uint8_t *out = NULL;             /* the caller's copy                    */
    if (dest != NULL) {
        CGImageDestinationAddImage(dest, image, NULL);
        if (CGImageDestinationFinalize(dest)) {
            size_t n = (size_t)CFDataGetLength(data);
            out = malloc(n);
            memcpy(out, CFDataGetBytePtr(data), n);
            *n_out = n;
        }
        CFRelease(dest);
    }
    CFRelease(data);
    return out;
}

/*
 * on_image_file_to_png — see image_io.h.
 */
uint8_t *
on_image_file_to_png(const char *path, size_t *n_out)
{
    *n_out = 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (url == NULL) {               /* an empty path: CFRelease(NULL) traps */
        return NULL;
    }
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (source == NULL) {
        return NULL;
    }
    CGImageRef image = source_first_image(source, 0);
    CFRelease(source);
    if (image == NULL) {
        return NULL;
    }
    uint8_t *png = on_image_png_encode(image, n_out);
    CGImageRelease(image);
    return png;
}
