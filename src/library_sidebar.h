#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_sidebar — the library's sidebar: the views (All Notes, Pinned
 * Notes, Action Items), then the tags the notes carry, and the Trash —
 * the design of the Tasks app's sidebar.  A right-click on a tag offers
 * Edit Tag… (its emoji and Notes' favorite flag; a tag's name never
 * changes); a right click leaves the selection alone, the menu's items
 * carry the row (on_sb_ref_t).
 *
 * It owns the selection: when the user selects a row it writes
 * lib.sel_kind / sel_id / sel_name and calls on_library_notes_refresh and
 * the window's status update.  It is also the library's only DROP target
 * (notes dragged onto a tag get it as a #tag on their bottom lines; onto
 * the Trash, they go there).
 */

/*
 * on_library_sidebar_new — build the sidebar for the library window.
 * Called once per window, by library_window.m.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the sidebar's view (the caller puts it in the split view).
 */
NSView *on_library_sidebar_new(NotesLibrary *lib);

/*
 * on_library_sidebar_refresh — rebuild the rows from the database: tags,
 * counts, which fixed rows exist.  The selection survives; a selection that
 * no longer exists falls back to All Notes (updating lib's selection and
 * refreshing the notes pane for it).  When
 * the selection survives the notes pane is not refreshed: the caller does
 * that.
 *
 * Output: YES when the notes pane was refreshed (the selection moved).
 */
BOOL on_library_sidebar_refresh(NotesLibrary *lib);

/*
 * on_library_sidebar_refresh_counts — re-read and redraw the counts only
 * (an action item was ticked): the rows, the selection and the scroll
 * position stay.  A no-op while counts are hidden.
 */
void on_library_sidebar_refresh_counts(NotesLibrary *lib);

/*
 * on_library_sidebar_note_added — a NEW untagged note was created (New
 * Note's, or a calendar event's, library_planner.h):
 * count it in All Notes, redrawing that row alone (no query, no rebuild).
 * A no-op while counts are hidden.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_sidebar_note_added(NotesLibrary *lib);

/*
 * on_library_sidebar_select — select a row from code (a Favorites bar
 * button, leaving an emptied Trash) and update lib's selection to match.
 *
 * Inputs:
 *   lib   — the library
 *   kind  — the row's kind
 *   ident — its tag id (0 for the fixed rows)
 */
void on_library_sidebar_select(NotesLibrary *lib, on_sb_kind_t kind, gint64 ident);

/*
 * on_library_sidebar_fit — size the sidebar to the rows on show, honouring
 * the sidebar_fit_content preference (a no-op while it is off).  Settings
 * calls this when the preference is switched on.
 */
void on_library_sidebar_fit(NotesLibrary *lib);
