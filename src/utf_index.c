/* ===========================================================================
 * utf_index.c — UTF-8 byte <-> UTF-16 unit offsets (see utf_index.h)
 * =========================================================================== */

#include "utf_index.h"

/*
 * lead_bytes — how long the UTF-8 sequence starting with this byte is.
 *
 * Inputs:
 *   c — a lead byte of valid UTF-8
 *
 * Output: 1..4.
 */
static size_t
lead_bytes(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if (c < 0xE0) {
        return 2;
    }
    if (c < 0xF0) {
        return 3;
    }
    return 4;
}

/*
 * on_utf16_from_utf8 — see utf_index.h.
 */
size_t
on_utf16_from_utf8(const char *text, size_t bytes)
{
    size_t units = 0;
    size_t i     = 0;
    while (i < bytes) {
        size_t n = lead_bytes((unsigned char)text[i]);
        units += (n == 4) ? 2 : 1;   /* outside the BMP: a surrogate pair    */
        i += n;
    }
    return units;
}

/*
 * on_utf8_from_utf16 — see utf_index.h.
 */
size_t
on_utf8_from_utf16(const char *text, size_t len, size_t units)
{
    size_t i = 0;
    while (i < len) {
        size_t n    = lead_bytes((unsigned char)text[i]);
        size_t cost = (n == 4) ? 2 : 1;
        if (units < cost) {
            break;                   /* at, or inside, this character        */
        }
        units -= cost;
        i += n;
    }
    return i < len ? i : len;
}
