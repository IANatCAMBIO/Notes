#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_sidebar — the library's sidebar: the folder tree, the tags, and
 * the fixed rows (Pinned Notes, All Notes, Action Items, Trash).
 *
 * It owns the selection: when the user selects a row it writes
 * lib.sel_kind / sel_id / sel_name and calls on_library_notes_refresh and
 * the window's status update.  It is also the library's only DROP target
 * (notes and folders dragged onto folders, the root and the Trash).
 *
 * The specification is the GTK build's src/library_window.c
 * (refresh_sidebar, the sidebar menus, the drop validation, the
 * fit-to-content sizing).
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
 * on_library_sidebar_refresh — rebuild the rows from the database: folders,
 * tags, counts, which fixed rows exist.  Expansion state and the selection
 * survive; a selection that no longer exists falls back to the first row
 * (updating lib's selection and refreshing the notes pane for it).  When
 * the selection survives the notes pane is not refreshed: the caller does
 * that.
 */
void on_library_sidebar_refresh(NotesLibrary *lib);

/*
 * on_library_sidebar_select — select a row from code (after creating a
 * folder, restoring one, leaving a deleted one), expanding its ancestors,
 * and update lib's selection to match.
 *
 * Inputs:
 *   lib   — the library
 *   kind  — the row's kind
 *   ident — its folder or tag id (0 for the fixed rows)
 */
void on_library_sidebar_select(NotesLibrary *lib, on_sb_kind_t kind, gint64 ident);

/*
 * on_library_sidebar_fit — size the sidebar to the rows on show, honouring
 * the sidebar_fit_content preference (a no-op while it is off).  Settings
 * calls this when the preference is switched on.
 */
void on_library_sidebar_fit(NotesLibrary *lib);
