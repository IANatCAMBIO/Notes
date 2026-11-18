#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_window — the library window's frame: the toolbar, the
 * Favorites bar across the window, the split between the sidebar, the
 * notes pane and the day planner under it, and the status bar.  The panes themselves come from their own modules
 * (library_favorites.h, library_sidebar.h, library_notes.h,
 * library_planner.h).
 *
 * There is at most one library window.  Closing it does not quit the app
 * while editors are open; it is built again when it is next asked for.
 *
 * OWNERSHIP.  This module holds the window's controller in a file-static
 * from show to close.  In windowWillClose: the controller removes its
 * status timer and the Apple Notes progress observer and lets go of
 * itself; the panes' views go with the window.
 *
 */

/*
 * on_library_window_install — set the OnApp hooks the library answers
 * (notify_notes_changed, notify_note_saved, notify_status,
 * notify_outside_commit; the first, second and fourth are passed on to the
 * Apple Notes copy, apple_notes.h) and register the window's own commands
 * (toggle-sidebar, toggle-favorites, toggle-day-planner).  Called once at
 * startup by the app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_window_install(NotesLibrary *lib);

/*
 * on_library_window_show — bring the library window to the front, building
 * it first if it is closed.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_window_show(NotesLibrary *lib);

/*
 * on_library_window_location_changed — the selection changed (or what it
 * names did): the status bar's left side shows the new location and the
 * Favorites bar lights the selected tag (its buttons are rebuilt with the
 * sidebar, on_library_refresh_all).  Called by the notes pane whenever it
 * lists a selection.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_window_location_changed(NotesLibrary *lib);

/*
 * on_library_window_planner_changed — the calendar settings changed
 * (Settings' Calendar section): the day planner reads its day again,
 * while it shows; the switch going off closes it and takes its toolbar
 * button away, going on brings the button back.
 *
 * Inputs:
 *   lib — the library (unused: there is one window)
 */
void on_library_window_planner_changed(NotesLibrary *lib);

/*
 * on_library_window_refreshing — tell the user the library is being
 * refreshed: a spinner and "Refreshing Library…" at the left of the
 * status bar, painted at once (the refresh that follows holds the main
 * thread, so nothing would be drawn until it ended), kept at least
 * on_look_refresh_hold seconds after the last call, so a quick refresh
 * reads rather than flickers.  Then the bar shows what it did before (the
 * Apple Notes sync's count, or nothing).  Called by on_library_refresh_all
 * before it rebuilds the panes.
 *
 * Inputs:
 *   lib — the library (unused: there is one window)
 */
void on_library_window_refreshing(NotesLibrary *lib);
