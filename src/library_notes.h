#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_notes — the library's notes pane: the notes of the current
 * selection as a list or a grid (one selection shared by the two), and the
 * Action Items list that replaces them while that sidebar row is selected.
 */

/*
 * on_library_notes_install — register the pane's commands with the library
 * (view-list, view-grid, toggle-view, toggle-done, note-pin).
 * Called once at startup by the app delegate.
 */
void on_library_notes_install(NotesLibrary *lib);

/*
 * on_library_notes_new — build the notes pane for the library window.
 * Called once per window, by library_window.m.
 *
 * Output: the pane's view; it switches between list, grid and Action Items
 *         itself, per on_library_view.
 */
NSView *on_library_notes_new(NotesLibrary *lib);

/*
 * on_library_notes_refresh — list the notes of the current selection
 * afresh (or the action items).  Selected notes stay selected where they
 * are still listed; the scroll position is kept when the selection is the
 * one already shown.
 */
void on_library_notes_refresh(NotesLibrary *lib);

/*
 * on_library_notes_note_saved — ONE note's content was saved by its editor:
 * update that row (title, modified time, preview, thumbnail) and nothing
 * else.  The OnApp.notify_note_saved hook lands here.
 *
 * Inputs:
 *   lib     — the library
 *   note_id — the note
 */
void on_library_notes_note_saved(NotesLibrary *lib, gint64 note_id);

/*
 * on_library_notes_selected_ids — the notes selected in the pane.
 *
 * Output: their ids, in display order; empty when none (and in the Action
 *         Items view).
 */
NSArray<NSNumber *> *on_library_notes_selected_ids(NotesLibrary *lib);
