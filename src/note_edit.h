/* ===========================================================================
 * note_edit.h — the editing policy of a note, with no toolkit in it
 *
 * What a key, a click or a paste DOES to a note is not a property of the
 * view that received it.  In the GTK build that policy lived inside the
 * widget (note_view.c); here it belongs in this module, in C over
 * OnDocument and OnDocLayout, so the AppKit view (note_view.m) and the
 * test probe drive one implementation: typing and the armed inline flags,
 * Enter and Backspace (lists continue, an empty item ends its list,
 * headings are one line), the block and inline toggles, pasting a fragment,
 * the #tag capture, find, the typing undo group.
 *
 * STATUS: work package C owns this module and grows it by lifting that
 * policy out of porting/gtk_reference/src/note_view.c (map section 3.4
 * lists the line ranges).  What is here today is the part other modules
 * already rely on, implemented for real:
 *   - the vocabulary of a key press, shared by the view and the probe;
 *   - the rule for a blank note, which the editor window applies on close.
 * Package C may add to this header freely; nothing outside the note view,
 * the probe and the editor window includes it.
 *
 * Every text position in this module is an OnPos with a UTF-8 BYTE offset
 * (utf_index.h).
 * =========================================================================== */

#ifndef ON_NOTE_EDIT_H
#define ON_NOTE_EDIT_H

#include <stdbool.h>
#include <stdint.h>

#include "document.h"

/* A key the editor acts on, as the view reports it after the input method
 * has passed on it.  Printable keys are on_key_character plus the code
 * point.                                                                    */
typedef enum {
    on_key_character,
    on_key_left,
    on_key_right,
    on_key_up,
    on_key_down,
    on_key_home,
    on_key_end,
    on_key_page_up,
    on_key_page_down,
    on_key_return,
    on_key_backspace,
    on_key_delete,
    on_key_tab,
    on_key_escape,
} on_key_t;

/* Modifier bits of a key press or a click.                                  */
typedef enum {
    on_mod_shift   = 1 << 0,
    on_mod_command = 1 << 1,         /* the scripts' "primary" and "meta"    */
    on_mod_option  = 1 << 2,         /* "alt": word movement on macOS        */
    on_mod_control = 1 << 3,
} on_mod_t;

/* ---------------------------------------------------------------------------
 * on_note_edit_is_blank() — does a note hold nothing at all: only
 * whitespace, and no image, table or task box?  A note closed in this
 * state is deleted rather than saved, so a New Note or Quick Note window
 * closed without typing leaves nothing behind.
 *
 * Inputs:
 *   doc — the note
 *
 * Output: true when it is blank.
 * ------------------------------------------------------------------------- */
bool on_note_edit_is_blank(const OnDocument *doc);

#endif /* ON_NOTE_EDIT_H */
