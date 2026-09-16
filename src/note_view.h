/* ===========================================================================
 * note_view.h — the rich-text note ENGINE: OnNoteView, a drawn widget
 *
 * One OnNoteView edits one note.  The note is an OnDocument (document.h);
 * this widget lays it out with Pango (doc_layout.h) and draws text,
 * images, tables and code blocks itself — no widget ever lives inside it
 * (BLOCK_MODEL.md says why).  The window that hosts the view
 * (editor_window.c) supplies the chrome — toolbar, actions, autosave,
 * status bar, the modal image viewer — and never touches the document
 * itself.
 *
 * What the engine does:
 *   - inline styles (bold/italic/underline/strike) on a "current flags"
 *     model: the active set is enforced on newly typed characters, like a
 *     word processor
 *   - paragraph kinds: headings, bulleted / numbered / task lists and
 *     code blocks; Enter continues a list, Enter on an empty item ends it,
 *     Backspace at the start of an item makes it body text
 *   - code runs with painted line numbers and a "copy" word
 *   - images (block and inline), task checkboxes and tables, drawn, with
 *     their context menus
 *   - #tag capture with an autocomplete popover
 *   - the DERIVED, never-serialized looks: the title line, the action-line
 *     tint, the macOS emoji padding
 *   - action-item identity (a uid per '!' block) and the '!' line rewrites
 *   - undo/redo (the document's op log, grouped by typing pause), the
 *     clipboard (plain text + a BNBF fragment, so a table or an image
 *     pastes as itself), input methods, the in-note find
 *
 * The view never references its host.  It reports through signals:
 *   "edited"               — the note changed in a way that needs saving
 *   "inline-flags-changed" — on_note_view_inline_flags() has a new answer
 *   "image-activated"      — an embedded image was clicked (its ordinal)
 * =========================================================================== */

#ifndef ON_NOTE_VIEW_H
#define ON_NOTE_VIEW_H

#include "app.h"
#include "document.h"

#define ON_TYPE_NOTE_VIEW (on_note_view_get_type())
G_DECLARE_FINAL_TYPE(OnNoteView, on_note_view, ON, NOTE_VIEW, GtkWidget)

/* The clipboard's own format for a note fragment: a BNBF blob.            */
#define ON_NOTE_MIME "application/x-notes-bnbf"

/* ---------------------------------------------------------------------------
 * on_note_view_new() — a fresh, empty note view.
 *   app — application context, read for the settings and the database
 *         (the #tag popup's choices); not owned.
 * Returns the view (floating; a container or g_object_ref_sink takes it).
 * ------------------------------------------------------------------------- */
GtkWidget *on_note_view_new(OnApp *app);

/* on_note_view_action_group() — the "view." action group behind the
 * context menus (image, table, clipboard).  Already inserted on the view;
 * a host whose context popovers are parented ELSEWHERE (on_app_menu_popup
 * parents every popover to the window's child box, D14) inserts it there
 * too.  Returns the group (borrowed; the view owns it).                   */
GActionGroup *on_note_view_action_group(OnNoteView *v);

/* on_note_view_load() — replace the note with a stored BNBF blob (NULL =
 * empty note): caret to the start, undo history cleared, nothing reported
 * as an edit.                                                             */
void on_note_view_load(OnNoteView *v, const guint8 *blob, gsize len);

/* on_note_view_serialize() — the note as a new BNBF blob (g_free it).     */
guint8 *on_note_view_serialize(OnNoteView *v, gsize *out_len);

/* on_note_view_first_line() — the note's title (on_document_title with
 * the app's default).  Returns a new string; g_free it.                  */
gchar *on_note_view_first_line(OnNoteView *v);

/* on_note_view_is_blank() — no content at all: nothing but whitespace and
 * no image, table or task box.                                            */
gboolean on_note_view_is_blank(OnNoteView *v);

/* on_note_view_collect_tags() — the note's distinct #tag names (see
 * on_document_collect_tags).                                              */
GList *on_note_view_collect_tags(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_toggle_inline() — bold/italic/underline/strike.  With a
 * selection: toggle the flag across it.  Without: flip the bit in the
 * current inline flags so upcoming typed text uses it.  Emits
 * "inline-flags-changed" and focuses the view.
 * ------------------------------------------------------------------------- */
void on_note_view_toggle_inline(OnNoteView *v, OnFormatFlags flag);

/* on_note_view_inline_flags() — the ON_FMT_INLINE_MASK bits newly typed
 * text will carry (what the toolbar's toggle buttons mirror).            */
guint32 on_note_view_inline_flags(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_toggle_paragraph() — give the selected blocks (or the
 * caret's) the kind `flag` stands for, or make them body text when every
 * one already is that kind.  Focuses the view.
 *   flag — one ON_FMT_PARA_MASK bit, or 0 for plain body text.
 * ------------------------------------------------------------------------- */
void on_note_view_toggle_paragraph(OnNoteView *v, guint32 flag);

/* on_note_view_insert_image() — an image at the caret, on a line of its
 * own, stored as PNG (encoded once, here), shown at the thumbnail size.  */
void on_note_view_insert_image(OnNoteView *v, GdkPixbuf *pixbuf);

/* on_note_view_insert_table() — a fresh 3×3 table at the caret.           */
void on_note_view_insert_table(OnNoteView *v);

/* on_note_view_insert_date() — today's date (YYYY-MM-DD) as typed text.  */
void on_note_view_insert_date(OnNoteView *v);

/* on_note_view_insert_emoji() — GTK's emoji chooser at the caret.         */
void on_note_view_insert_emoji(OnNoteView *v);

/* on_note_view_undo() / redo() — step the document's history; the caret
 * lands where the step changed things.                                    */
void on_note_view_undo(OnNoteView *v);
void on_note_view_redo(OnNoteView *v);

/* on_note_view_find() — highlight every case-insensitive match of `text`
 * (NULL or "" clears).  on_note_view_find_step() selects and scrolls to
 * the next match after the caret (or the previous one), wrapping.       */
void on_note_view_find(OnNoteView *v, const gchar *text);
void on_note_view_find_step(OnNoteView *v, const gchar *text,
                            gboolean forward);

/* ---------------------------------------------------------------------------
 * IMAGE ORDINALS — the note's images numbered in document order (block
 * images and inline ones alike): the app's ONE way of addressing a note's
 * images, the order on_note_count_images() walks IMAGE records in.
 * ------------------------------------------------------------------------- */
gint        on_note_view_image_count(OnNoteView *v);
GdkTexture *on_note_view_image_texture(OnNoteView *v, gint ord); /* borrowed */
GBytes     *on_note_view_image_png(OnNoteView *v, gint ord);     /* borrowed */

/* on_note_view_image_reveal() — caret to the `ord`-th image, scrolled
 * into view.  FALSE when there is no such image.                         */
gboolean on_note_view_image_reveal(OnNoteView *v, gint ord);

/* on_note_image_open_external() — write an image's stored bytes to a
 * temporary PNG and hand it to the configured viewer (Settings
 * "image_viewer") or the platform opener.  NULL is a no-op.             */
void on_note_image_open_external(GBytes *png);

/* on_note_view_settings_changed() — re-read the settings the engine
 * renders from and apply them live.                                       */
void on_note_view_settings_changed(OnNoteView *v);

/* on_note_view_take_tags_modified() / take_actions_modified() — what the
 * host needs at save time, kept live by the document's operations; each
 * read clears its flag.  Actions read TRUE once after every load.       */
gboolean on_note_view_take_tags_modified(OnNoteView *v);
gboolean on_note_view_take_actions_modified(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * ACTION-ITEM IDENTITY — every '!' block carries its item's stable uid
 * (OnBlock.action_uid), which rides every edit of that block.  The host
 * owns the action_items table; the view owns the blocks.
 * ------------------------------------------------------------------------- */

/* on_note_view_action_marks_hint() — fill each item's uid with the uid of
 * its block, as a match hint for on_db_note_set_actions.  An item whose
 * block carries none keeps uid 0.
 *   items — freshly extracted OnActionItem list, in ord order.           */
void on_note_view_action_marks_hint(OnNoteView *v, GList *items);

/* on_note_view_action_marks_sync() — give the n-th real action block the
 * n-th item's uid: after any save that rewrote the table and once after
 * load.                                                                   */
void on_note_view_action_marks_sync(OnNoteView *v, GList *items);

/* on_note_view_action_strike/due/text() — the live-editor side of the
 * headless on_document_action_* rewrites: the same edit, then "edited". */
gboolean on_note_view_action_strike(OnNoteView *v, gint ord, gboolean done);
gboolean on_note_view_action_due(OnNoteView *v, gint ord, gint64 due);
gboolean on_note_view_action_text(OnNoteView *v, gint ord, const gchar *text);

/* ---------------------------------------------------------------------------
 * THE KEYBOARD AS FUNCTION CALLS — what the key controller and the input
 * method do, callable: tests/ui_probe.c drives a view through these and
 * looks at the document and the pixels, since GTK4 offers no synthetic
 * input.  on_note_view_feed_key() is a key press (keyval + modifiers) as
 * the controller receives it after the input method passed on it;
 * on_note_view_feed_text() is a committed string, as the method delivers
 * typed characters.
 * ------------------------------------------------------------------------- */
gboolean on_note_view_feed_key(OnNoteView *v, guint keyval,
                               GdkModifierType state);
void     on_note_view_feed_text(OnNoteView *v, const gchar *text);

/* on_note_view_feed_click() — a press at widget coordinates (x, y):
 * `n_press` 1/2/3, `state` the modifiers, `button` GDK_BUTTON_PRIMARY or
 * SECONDARY.  Returns TRUE when the press was consumed by something that
 * is not text (a checkbox, a copy word, an image).                        */
gboolean on_note_view_feed_click(OnNoteView *v, gdouble x, gdouble y,
                                 gint n_press, GdkModifierType state,
                                 guint button);

/* on_note_view_caret() — where the caret is (for the probe's checks).    */
OnPos on_note_view_caret(OnNoteView *v);

/* on_note_view_document() — the note itself (borrowed; read-only use).   */
OnDocument *on_note_view_document(OnNoteView *v);

#endif /* ON_NOTE_VIEW_H */
