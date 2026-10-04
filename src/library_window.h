#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_window — the library window's frame (work package E): the
 * toolbar, the split between the sidebar and the notes pane, the AI pane
 * under the notes, and the status bar.  The panes themselves come from
 * their own modules (library_sidebar.h, library_notes.h, library_ai.h).
 *
 * There is at most one library window.  Closing it does not quit the app
 * while editors are open; it is built again when it is next asked for.
 *
 * OWNERSHIP.  This module holds the window's controller in a file-static
 * from show to close.  In windowWillClose: the controller removes its
 * status timer and lets go of itself; the panes' views go with the window.
 *
 * STATUS: a working skeleton — window, toolbar from the command table,
 * split view, status bar, the hooks.  Still to do for package E: the
 * toolbar's List/Grid and Completed items changing their icons (their
 * commands are the notes pane's), and persisting nothing more than the
 * GTK build did.  This header is FROZEN.
 */

/*
 * on_library_window_install — set the OnApp hooks the library answers
 * (notify_notes_changed, notify_note_saved, notify_status) and register the
 * window's own command (toggle-sidebar).  Called once at startup by the app
 * delegate.
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
