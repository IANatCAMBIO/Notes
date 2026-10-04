/* ===========================================================================
 * utf_index_test.c — UTF-8 byte offsets <-> UTF-16 unit offsets
 *
 * The byte-offset rule rests on these two functions being exact for every
 * width of character: one wrong step puts a caret inside a character.
 * =========================================================================== */

#include "check.h"

#include "utf_index.h"

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    /* a (1 byte, 1 unit) é (2, 1) 日 (3, 1) 😀 (4, 2) z (1, 1) */
    const char  *text = "a\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80z";
    const size_t len  = strlen(text);
    static const struct {
        size_t bytes, units;
    } boundaries[] = { { 0, 0 }, { 1, 1 }, { 3, 2 }, { 6, 3 }, { 10, 5 }, { 11, 6 } };

    bool forward = true, backward = true;
    for (size_t i = 0; i < sizeof boundaries / sizeof boundaries[0]; i++) {
        forward  = forward && on_utf16_from_utf8(text, boundaries[i].bytes) == boundaries[i].units;
        backward = backward &&
                   on_utf8_from_utf16(text, len, boundaries[i].units) == boundaries[i].bytes;
    }
    check(forward, "bytes -> UTF-16 units at every character boundary");
    check(backward, "UTF-16 units -> bytes at every character boundary");

    check(on_utf8_from_utf16(text, len, 4) == 6,
          "an offset inside a surrogate pair moves back to the character's start");
    check(on_utf8_from_utf16(text, len, 99) == len, "an offset past the end is clamped");
    check(on_utf16_from_utf8("", 0) == 0 && on_utf8_from_utf16("", 0, 3) == 0,
          "empty text is offset 0 both ways");

    /* Round trip over a longer mixed string, at every boundary. */
    const char *mixed = "Na\xc3\xafve \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 "
                        "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xe2\x80\x94 end";
    size_t mixed_len = strlen(mixed);
    bool   round     = true;
    for (size_t b = 0; b <= mixed_len; b++) {
        if (b < mixed_len && ((unsigned char)mixed[b] & 0xC0) == 0x80) {
            continue;                /* not a character boundary              */
        }
        round = round && on_utf8_from_utf16(mixed, mixed_len, on_utf16_from_utf8(mixed, b)) == b;
    }
    check(round, "byte -> unit -> byte is the identity at every boundary");
    return check_result();
}
