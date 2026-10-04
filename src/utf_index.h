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

#endif /* ON_UTF_INDEX_H */
