/* ===========================================================================
 * note_edit.c — the editing policy of a note (see note_edit.h)
 * =========================================================================== */

#include "note_edit.h"

/*
 * on_note_edit_is_blank — see note_edit.h.
 */
bool
on_note_edit_is_blank(const OnDocument *doc)
{
    for (guint i = 0; i < on_document_n_blocks(doc); i++) {
        const OnBlock *b = on_document_block(doc, i);
        if (b->kind == ON_BLOCK_TAGS) {
            continue;                /* tags alone are not content          */
        }
        /* An image or table block, a task box, an inline image: content.   */
        if (b->text == NULL || b->kind == ON_BLOCK_CHECK ||
            b->text->objects->len > 0) {
            return false;
        }
        for (const gchar *p = b->text->text->str; *p != '\0';
             p = g_utf8_next_char(p)) {
            if (!g_unichar_isspace(g_utf8_get_char(p))) {
                return false;
            }
        }
    }
    return true;
}
