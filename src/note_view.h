/* ===========================================================================
 * note_view.h — the rich-text note ENGINE: OnNoteView, a GtkTextView subclass
 *
 * One OnNoteView edits one note's buffer.  Everything that edits or renders
 * that buffer lives behind this API — the window that hosts the view
 * (editor_window.c) supplies the chrome (toolbar, actions, autosave, status
 * bar, the modal image viewer) and never touches the buffer's tags itself.
 *
 * What the engine does:
 *   - inline styles (bold/italic/underline/strike) on a "current flags"
 *     model: the active set is enforced on newly typed characters, like a
 *     word processor
 *   - paragraph styles: headings, bulleted / numbered / task lists and
 *     code blocks as GtkTextTags over whole lines; list items carry a
 *     literal prefix, Enter continues a list, Enter on an empty item ends it
 *   - code blocks with painted line numbers, shading of empty lines and a
 *     floating "copy" link per block
 *   - embedded images, task checkboxes and tables as child anchors with
 *     native widgets, plus the image and table context menus
 *   - #tag capture with an autocomplete popover
 *   - the DERIVED, never-serialized looks: the title line, the action-line
 *     tint, the macOS emoji padding
 *   - action-item identity marks and the '!' line rewrites
 *   - snapshot undo/redo and the in-note find
 *
 * The view never references its host.  It reports through signals:
 *   "edited"               — the note changed in a way that needs saving
 *   "inline-flags-changed" — on_note_view_inline_flags() has a new answer
 *   "image-activated"      — an embedded image was clicked (its ordinal)
 *   "cell-created"         — a table cell (a widget with keys of its own)
 *                            was built; the host hangs its gate on it
 *
 * The three on_note_buffer_action_* functions work on a BARE GtkTextBuffer
 * with no view at all, so the headless action rewrites (a CLI or library
 * tick on a note that is not open) run the same code as the live editor.
 * =========================================================================== */

#ifndef ON_NOTE_VIEW_H
#define ON_NOTE_VIEW_H

#include "app.h"
#include "serialize.h"           /* OnFormatFlags                        */

#define ON_TYPE_NOTE_VIEW (on_note_view_get_type())
G_DECLARE_FINAL_TYPE(OnNoteView, on_note_view, ON, NOTE_VIEW, GtkTextView)

/* ---------------------------------------------------------------------------
 * on_note_view_new() — a fresh, empty note view: the standard tag set plus
 * the editor-only tags on its buffer, the view's margins and wrapping, its
 * buffer signal handlers, key and click controllers, the "view." action
 * group (image and table context menus) and the process-wide CSS.
 *   app — application context, read for the settings and the database
 *         (the #tag popup's choices); not owned.
 * Returns the view (floating; a container or g_object_ref_sink takes it).
 * ------------------------------------------------------------------------- */
GtkWidget *on_note_view_new(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_note_view_action_group() — the "view." action group behind the image
 * and table context menus.  It is already inserted on the view; a host
 * whose context popovers are parented ELSEWHERE (on_app_menu_popup parents
 * every popover to the window's child box, D14) inserts it there too, so
 * "view.table-*" resolves from that popover.
 * Returns the group (borrowed; the view owns it).
 * ------------------------------------------------------------------------- */
GActionGroup *on_note_view_action_group(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_load() — replace the buffer with a stored BNBF blob: every
 * image, checkbox and table anchor gets its widget, the derived looks are
 * re-applied, the undo history restarts from this state.  Nothing is
 * reported as an edit.  A NULL blob is an empty note (still sets the
 * title-line presentation and the undo baseline).
 *   blob / len — BNBF bytes as loaded from SQLite, or NULL / 0.
 * ------------------------------------------------------------------------- */
void on_note_view_load(OnNoteView *v, const guint8 *blob, gsize len);

/* on_note_view_serialize() — the buffer as a new BNBF blob (g_free it);
 * `out_len` receives its size.  See on_note_serialize().                    */
guint8 *on_note_view_serialize(OnNoteView *v, gsize *out_len);

/* on_note_view_first_line() — the note's title: the first non-empty line
 * (see on_buffer_first_line).  Returns a new string; g_free it.            */
gchar *on_note_view_first_line(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_toggle_inline() — bold/italic/underline/strike.  With a
 * selection: toggle the tag across it.  Without: flip the bit in the
 * current inline flags so upcoming typed text uses it.  Emits
 * "inline-flags-changed" and focuses the view.
 *   flag — one ON_FMT_INLINE_MASK bit.
 * ------------------------------------------------------------------------- */
void on_note_view_toggle_inline(OnNoteView *v, OnFormatFlags flag);

/* on_note_view_inline_flags() — the ON_FMT_INLINE_MASK bits newly typed
 * text will carry (what the toolbar's toggle buttons mirror).              */
guint32 on_note_view_inline_flags(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_toggle_paragraph() — apply `flag` to the selected lines (or
 * the cursor line), or revert them to body text when every one already
 * carries it.  Focuses the view.
 *   flag — one ON_FMT_PARA_MASK bit, or 0 for plain body text.
 * ------------------------------------------------------------------------- */
void on_note_view_toggle_paragraph(OnNoteView *v, guint32 flag);

/* ---------------------------------------------------------------------------
 * on_note_view_insert_image() — embed a full-resolution image at the cursor
 * as an anchor + HiDPI widget, shown at the default thumbnail width.
 *   pixbuf — the image; the caller keeps its own reference.
 * ------------------------------------------------------------------------- */
void on_note_view_insert_image(OnNoteView *v, GdkPixbuf *pixbuf);

/* on_note_view_insert_table() — a fresh 3×3 table at the cursor; rows and
 * columns are added or removed from any cell's right-click menu.           */
void on_note_view_insert_table(OnNoteView *v);

/* on_note_view_insert_date() — today's date (YYYY-MM-DD) at the cursor,
 * replacing the selection like typed text; goes through the normal
 * insert path, so styling, autosave and undo behave as for a paste.        */
void on_note_view_insert_date(OnNoteView *v);

/* on_note_view_insert_emoji() — GTK's emoji chooser at the cursor (the
 * text view's own "insert-emoji"); the pick is plain UTF-8 text.           */
void on_note_view_insert_emoji(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_undo() / on_note_view_redo() — the snapshot history.  Undo
 * first flushes the in-progress edit group so the very latest edits are
 * what gets undone; a no-op with nothing to step to.
 * ------------------------------------------------------------------------- */
void on_note_view_undo(OnNoteView *v);
void on_note_view_redo(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_find() — highlight every case-insensitive match of `text`
 * in the note (the "on-search-hit" tag), dropping the previous
 * highlights.  NULL or "" just clears them.
 * ------------------------------------------------------------------------- */
void on_note_view_find(OnNoteView *v, const gchar *text);

/* ---------------------------------------------------------------------------
 * on_note_view_find_step() — select and scroll to the next match of `text`
 * after the cursor (forward) or the previous one before it, wrapping
 * around the buffer when none remains in that direction.  Takes the text
 * rather than remembering on_note_view_find()'s: the host's search entry
 * reports its changes debounced, and a step must follow the entry's
 * LIVE text.  NULL or "" is a no-op.
 * ------------------------------------------------------------------------- */
void on_note_view_find_step(OnNoteView *v, const gchar *text,
                            gboolean forward);

/* ---------------------------------------------------------------------------
 * IMAGE ORDINALS — the note's images numbered in buffer order.  That order
 * is the app's ONE way of addressing a note's images: it is the order
 * on_note_count_images() walks the blob's IMAGE records in, so the media
 * browser, the editor's modal viewer and on_editor_window_open_image() all
 * agree on which picture is "image 3".
 * ------------------------------------------------------------------------- */

/* on_note_view_image_count() — how many images the note holds.             */
gint on_note_view_image_count(OnNoteView *v);

/* on_note_view_image_nth() — the `ord`-th image's full-resolution pixbuf
 * (anchor-owned: do not unref), or NULL when there is no such image.      */
GdkPixbuf *on_note_view_image_nth(OnNoteView *v, gint ord);

/* ---------------------------------------------------------------------------
 * on_note_view_image_reveal() — put the caret at the `ord`-th image and
 * scroll it into view, a third of the way down.  Caret only, no
 * selection: the next keystroke must not replace the image.
 * Returns TRUE when the image was found; a negative `ord` is a no-op.
 * ------------------------------------------------------------------------- */
gboolean on_note_view_image_reveal(OnNoteView *v, gint ord);

/* ---------------------------------------------------------------------------
 * on_note_image_open_external() — write a full-resolution image to a
 * temporary PNG and hand it to an image viewer: the program configured
 * under Settings ("image_viewer"), else the platform opener (macOS `open`,
 * otherwise `xdg-open`).  Shared by the image menu's Open and the modal
 * viewer's action link.  NULL is a no-op.
 * ------------------------------------------------------------------------- */
void on_note_image_open_external(GdkPixbuf *orig);

/* ---------------------------------------------------------------------------
 * on_note_view_settings_changed() — re-read the settings the engine
 * renders from (code_copy_buttons, code_line_numbers, first_line_title)
 * and apply them live: the copy links are rebuilt, the code-block margin
 * makes or drops room for the painted numbers, the title-line look is
 * re-derived.
 * ------------------------------------------------------------------------- */
void on_note_view_settings_changed(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_take_tags_modified() — whether an edit created, renamed or
 * deleted a styled #tag since the last call, and clear the flag.  Kept
 * LIVE by the capture / insert / delete / undo paths, so a save learns
 * without any buffer scan whether note_tags needs rewriting.
 * ------------------------------------------------------------------------- */
gboolean on_note_view_take_tags_modified(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * on_note_view_take_actions_modified() — whether an edit since the last
 * call (or the last load) COULD have created or destroyed a '!' action
 * line — a '!' or newline typed, a paste, any deletion — and clear the
 * flag.  The host skips the action extract on a save when this is FALSE
 * and it holds no items from before.
 * ------------------------------------------------------------------------- */
gboolean on_note_view_take_actions_modified(OnNoteView *v);

/* ---------------------------------------------------------------------------
 * ACTION-ITEM IDENTITY — one GtkTextMark per action line, carrying that
 * item's stable uid (see the marks section in note_view.c).  The host owns
 * the action_items table; the view owns the marks.
 * ------------------------------------------------------------------------- */

/* on_note_view_action_marks_hint() — fill each item's uid with the uid
 * marked on its line, as a match hint for on_db_note_set_actions.  An
 * item whose line carries no surviving mark keeps uid 0 ("no hint").
 *   items — freshly extracted OnActionItem list, in ord order.            */
void on_note_view_action_marks_hint(OnNoteView *v, GList *items);

/* on_note_view_action_marks_sync() — re-place the marks so the n-th real
 * action line carries the n-th item's uid: after any save that rewrote
 * the table (the uids are then known) and once after load.
 *   items — OnActionItem list in ord order, uids filled in.               */
void on_note_view_action_marks_sync(OnNoteView *v, GList *items);

/* ---------------------------------------------------------------------------
 * ACTION LINE REWRITES on a bare buffer — any buffer that has been through
 * on_buffer_ensure_tags: a live view's or an offscreen on_note_buffer_load.
 * `ord` numbers the REAL action lines (bare "!" lines and lines that are
 * only a "due <date>" do not count — the extractor's numbering).  Each
 * returns TRUE when the line was found.
 * ------------------------------------------------------------------------- */

/* on_note_buffer_action_strike() — strike (done) or un-strike the text of
 * the `ord`-th action line.                                                */
gboolean on_note_buffer_action_strike(GtkTextBuffer *buffer, gint ord,
                                      gboolean done);

/* on_note_buffer_action_due() — rewrite the line's "due <date>" suffix:
 * any existing one is removed, then " due YYYY-MM-DD" appended for a
 * non-zero `due` (local-midnight UNIX time); the appended text inherits
 * the item's strike state so a done item stays done.                      */
gboolean on_note_buffer_action_due(GtkTextBuffer *buffer, gint ord,
                                   gint64 due);

/* on_note_buffer_action_text() — replace the line's item TEXT, keeping the
 * '!' prefix, the line's own spacing and any trailing "due <date>"; the
 * new run is given the old text's strike state explicitly.  `text` must
 * be non-blank and newline-free (callers validate — see cli.c).           */
gboolean on_note_buffer_action_text(GtkTextBuffer *buffer, gint ord,
                                    const gchar *text);

#endif /* ON_NOTE_VIEW_H */
