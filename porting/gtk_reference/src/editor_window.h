/* ===========================================================================
 * editor_window.h — WYSIWYG note editor window
 *
 * Each note opens in its own top-level GtkWindow (standard titlebar, no
 * GtkHeaderBar) containing a formatting toolbar and a GtkTextView.
 *
 * Features:
 *   - inline styles: bold, italic, underline, strikethrough
 *   - paragraph styles: heading 1/2, bulleted list, numbered list,
 *     monospace code blocks (with a one-click "copy code block" button)
 *   - inline images pasted from the clipboard or inserted from a file;
 *     clicking one shows it big in the shared modal image viewer
 *     (image_viewer.[ch]), the same panel the media browser opens, with the
 *     arrow keys and its Previous | Next links walking the note's images.
 *     Its INLINE size (thumbnail or full) is the context menu's business
 *     now, not the click's
 *   - inline #tags with an autocomplete popup (space ends the tag)
 *   - debounced autosave to SQLite via the BNBF serializer
 * =========================================================================== */

#ifndef BLUE_EDITOR_WINDOW_H
#define BLUE_EDITOR_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * on_editor_window_open() — open (or focus) the editor for a note.
 *
 * If the note already has an open editor window, that window is presented;
 * otherwise a new window is created, the note content is loaded from the
 * database, and the window is shown.
 *
 *   app     — global application context.
 *   note_id — id of the note to edit.
 * Returns the editor's GtkWindow, or NULL if the note does not exist.
 * ------------------------------------------------------------------------- */
GtkWidget *on_editor_window_open(OnApp *app, gint64 note_id);

/* ---------------------------------------------------------------------------
 * on_editor_window_open_search() — like on_editor_window_open(), but also
 * pre-populates the editor's in-note search box with `search_term` and jumps
 * to the first match.  Used when a note is opened from the library's search
 * window so the searched-for text is highlighted straight away.  A NULL or
 * empty term behaves exactly like on_editor_window_open().
 *
 *   app         — global application context.
 *   note_id     — id of the note to edit.
 *   search_term — text to seed the in-note search with (may be NULL).
 * Returns the editor's GtkWindow, or NULL if the note does not exist.
 * ------------------------------------------------------------------------- */
GtkWidget *on_editor_window_open_search(OnApp *app, gint64 note_id,
                                        const gchar *search_term);

/* ---------------------------------------------------------------------------
 * on_editor_window_open_image() — like on_editor_window_open(), but also
 * scrolls the note to one of its embedded images and puts the caret there.
 * Used by the media window, where double-clicking a thumbnail opens the note
 * it came from at that picture.
 *
 *   app       — global application context.
 *   note_id   — id of the note to edit.
 *   image_ord — 0-based position of the image among the note's embedded
 *               images (the same ordinal on_note_count_images() counts);
 *               a negative value behaves exactly like
 *               on_editor_window_open().
 * Returns the editor's GtkWindow, or NULL if the note does not exist.
 * ------------------------------------------------------------------------- */
GtkWidget *on_editor_window_open_image(OnApp *app, gint64 note_id,
                                       gint image_ord);

/* ---------------------------------------------------------------------------
 * on_editor_window_open_action() — like on_editor_window_open(), but also
 * scrolls the note to one of its action items and puts the caret on it.
 * Used by the library's Action Items view, where double-clicking an item
 * opens the note it lives in at that line.
 *
 *   app        — global application context.
 *   note_id    — id of the note to edit.
 *   action_ord — the item's action_items ordinal (0-based); a negative
 *                value behaves exactly like on_editor_window_open().
 * Returns the editor's GtkWindow, or NULL if the note does not exist.
 * ------------------------------------------------------------------------- */
GtkWidget *on_editor_window_open_action(OnApp *app, gint64 note_id,
                                        gint action_ord);

/* ---------------------------------------------------------------------------
 * on_editor_rebuild_code_buttons_all() — re-evaluate the code-block copy
 * buttons in every open editor window.  Called by the settings window
 * when the "show copy button" preference changes.
 * ------------------------------------------------------------------------- */
void on_editor_rebuild_code_buttons_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_apply_line_numbers_all() — show/hide the code-block line
 * number gutter in every open editor per app->code_line_numbers.  Called
 * by the settings window when the preference changes.
 * ------------------------------------------------------------------------- */
void on_editor_apply_line_numbers_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_rebuild_toolbars_all() — rebuild the formatting toolbar of
 * every open editor per app->compact_editor_toolbar.  Called by the
 * settings window when the compact-toolbar preference changes.
 * ------------------------------------------------------------------------- */
void on_editor_rebuild_toolbars_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_title_refresh_all() — re-derive the title-line presentation
 * (centering, and the caret sizing on an unwritten title) in every open
 * editor per app->first_line_title.  Called by the settings window when
 * the preference changes (applies live).
 * ------------------------------------------------------------------------- */
void on_editor_title_refresh_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_status_refresh_all() — re-render the status-bar location of
 * every open editor window.  Called by the settings window when the
 * DB-path-prefix preference changes (applies live).
 * ------------------------------------------------------------------------- */
void on_editor_status_refresh_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_action_set_done() — set an action item's done state: its TASK
 * ROW first (status folds the Tasks way: done → DONE, undone → IN_PROGRESS
 * if it was DONE), then the '!' line is re-rendered — struck through or
 * un-struck — on the live view when the note is open (its autosave
 * follows), else on the stored document with an immediate save.  Called
 * by the library's Action Items checkbox column and the CLI.
 *   app  — global application context.
 *   uid  — the item's stable identity.
 *   done — the new state.
 * Returns TRUE when a live item carries `uid` and its row was written.
 * ------------------------------------------------------------------------- */
gboolean on_editor_action_set_done(OnApp *app, gint64 uid, gboolean done);

/* ---------------------------------------------------------------------------
 * on_editor_action_set_due() — set an action item's due date: the task
 * row first, then the '!' line's trailing "due <date>" is replaced (or
 * appended; `due` = 0 removes it).  The written form is ISO
 * ("due 2026-07-07"); appended text inherits the item's strike state.
 * Same contract as on_editor_action_set_done.
 *   due — local-midnight UNIX timestamp, or 0 to clear.
 * Returns TRUE when a live item carries `uid` and its row was written.
 * ------------------------------------------------------------------------- */
gboolean on_editor_action_set_due(OnApp *app, gint64 uid, gint64 due);

/* ---------------------------------------------------------------------------
 * on_editor_action_set_text() — rename an action item: the task row's
 * title first, then the '!' line keeps its prefix, its spacing and any
 * trailing "due <date>"; only the text between them is replaced, and it
 * carries the item's strike state over so a done item stays done.  The
 * uid is the line's, in the blob, and a rename does not touch it.  Same
 * contract as on_editor_action_set_done.
 *   text — the new item text.  Must be non-blank and contain no newline:
 *          a blank would stop the line being an action item and a newline
 *          would split it into two, either of which retires the item.
 *          Callers validate (see cmd_action_text in cli.c).
 * Returns TRUE when a live item carries `uid` and its row was written.
 * ------------------------------------------------------------------------- */
gboolean on_editor_action_set_text(OnApp *app, gint64 uid, const gchar *text);

#endif /* BLUE_EDITOR_WINDOW_H */
