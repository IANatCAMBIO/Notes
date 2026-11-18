/* ===========================================================================
 * utf_index.h — UTF-8 byte offsets <-> UTF-16 code-unit offsets
 *
 * THE BYTE-OFFSET RULE.  The document addresses text by UTF-8 BYTE offset:
 * OnPos.offset, OnRun.len, OnDocHit.start/len, every index that crosses
 * doc_layout.h, text_layout.h and note_edit.h.  CoreText, NSString,
 * NSTextInputClient ranges and NSAccessibility count UTF-16 code units.
 * One wrong conversion cuts a character in half and corrupts the note.
 *
 * So the conversion happens in exactly two places — inside the text-layout
 * backend (text_layout.c) and in the note view's linear-index mapping for
 * the input method and accessibility — and both use these two functions.
 * Nothing else in the app converts, and no interface carries a UTF-16
 * index.
 * =========================================================================== */

#ifndef ON_UTF_INDEX_H
#define ON_UTF_INDEX_H

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * on_utf16_from_utf8() — how many UTF-16 code units the first `bytes` bytes
 * of a UTF-8 string encode.
 *
 * Inputs:
 *   text  — valid UTF-8
 *   bytes — a byte offset on a character boundary
 *
 * Output: the UTF-16 offset of that position.
 * ------------------------------------------------------------------------- */
size_t on_utf16_from_utf8(const char *text, size_t bytes);

/* ---------------------------------------------------------------------------
 * on_utf8_from_utf16() — the byte offset of a UTF-16 offset.  An offset that
 * falls between the two halves of a surrogate pair is moved back to the
 * start of that character, so the result is always a character boundary.
 *
 * Inputs:
 *   text  — valid UTF-8
 *   len   — its length in bytes
 *   units — a UTF-16 offset; past the end is clamped to `len`
 *
 * Output: the byte offset.
 * ------------------------------------------------------------------------- */
size_t on_utf8_from_utf16(const char *text, size_t len, size_t units);

/* ---------------------------------------------------------------------------
 * on_utf_cursor_t — the same conversions for a run of offsets that mostly
 * go FORWARD through one text (a layout's runs, lines, hits, boxes): each
 * picks up where the last one stopped instead of counting from byte 0, so
 * a block's conversions cost one pass over it, not one per offset.  An
 * offset behind the cursor starts again from the beginning.  The two
 * functions above are this cursor used once.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *text;                /* valid UTF-8                          */
    size_t      len;                 /* its length in bytes                  */
    size_t      byte, unit;          /* where it stands: a character boundary
                                        and its UTF-16 offset               */
} on_utf_cursor_t;

/* ---------------------------------------------------------------------------
 * on_utf_cursor_init() — a cursor at the start of a text.
 *
 * Inputs:
 *   cursor — the cursor to set
 *   text   — valid UTF-8, kept by the caller while the cursor is used
 *   len    — its length in bytes
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_utf_cursor_init(on_utf_cursor_t *cursor, const char *text, size_t len);

/* ---------------------------------------------------------------------------
 * on_utf_cursor_units() — on_utf16_from_utf8 through a cursor.
 *
 * Inputs:
 *   cursor — the cursor; moved to `bytes`
 *   bytes  — a byte offset on a character boundary, at most the length
 *
 * Output: the UTF-16 offset of that position.
 * ------------------------------------------------------------------------- */
size_t on_utf_cursor_units(on_utf_cursor_t *cursor, size_t bytes);

/* ---------------------------------------------------------------------------
 * on_utf_cursor_bytes() — on_utf8_from_utf16 through a cursor (the same
 * rounding back out of a surrogate pair, the same clamping).
 *
 * Inputs:
 *   cursor — the cursor; moved to the result
 *   units  — a UTF-16 offset
 *
 * Output: the byte offset.
 * ------------------------------------------------------------------------- */
size_t on_utf_cursor_bytes(on_utf_cursor_t *cursor, size_t units);

#endif /* ON_UTF_INDEX_H */
