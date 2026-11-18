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
 * on_utf_cursor_init — see utf_index.h.
 */
void
on_utf_cursor_init(on_utf_cursor_t *cursor, const char *text, size_t len)
{
    cursor->text = text;
    cursor->len  = len;
    cursor->byte = 0;
    cursor->unit = 0;
}

/*
 * on_utf_cursor_units — see utf_index.h.
 */
size_t
on_utf_cursor_units(on_utf_cursor_t *cursor, size_t bytes)
{
    if (bytes < cursor->byte) {
        cursor->byte = 0;            /* behind it: count from the start      */
        cursor->unit = 0;
    }
    while (cursor->byte < bytes) {
        size_t n = lead_bytes((unsigned char)cursor->text[cursor->byte]);
        cursor->unit += (n == 4) ? 2 : 1;    /* outside the BMP: a surrogate pair */
        cursor->byte += n;
    }
    return cursor->unit;
}

/*
 * on_utf_cursor_bytes — see utf_index.h.
 */
size_t
on_utf_cursor_bytes(on_utf_cursor_t *cursor, size_t units)
{
    if (units < cursor->unit) {
        cursor->byte = 0;
        cursor->unit = 0;
    }
    while (cursor->byte < cursor->len) {
        size_t n    = lead_bytes((unsigned char)cursor->text[cursor->byte]);
        size_t cost = (n == 4) ? 2 : 1;
        if (units - cursor->unit < cost) {
            break;                   /* at, or inside, this character        */
        }
        cursor->unit += cost;
        cursor->byte += n;
    }
    return cursor->byte < cursor->len ? cursor->byte : cursor->len;
}

/*
 * on_utf16_from_utf8 — see utf_index.h.
 */
size_t
on_utf16_from_utf8(const char *text, size_t bytes)
{
    on_utf_cursor_t cursor;
    on_utf_cursor_init(&cursor, text, bytes);
    return on_utf_cursor_units(&cursor, bytes);
}

/*
 * on_utf8_from_utf16 — see utf_index.h.
 */
size_t
on_utf8_from_utf16(const char *text, size_t len, size_t units)
{
    on_utf_cursor_t cursor;
    on_utf_cursor_init(&cursor, text, len);
    return on_utf_cursor_bytes(&cursor, units);
}
