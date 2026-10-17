#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_favorites — the library's Favorites bar: a strip across the whole
 * window under the toolbar (the sidebar and the notes start below it) with
 * one button per tag a note outside the Trash carries (the sidebar's
 * tags) that is a favorite in Notes (tags.favorite_notes;
 * SharedFavoriteButton) in the order the user dragged them into (the
 * favorites_order setting), new favorites at the end by name: the design
 * of the Tasks app's bar.  View ▸ Hide Favorites Bar slides it away
 * (library_window.m; the favorites_bar_visible setting).  Clicking a
 * button selects its tag in the sidebar; the button of the selected tag is
 * lit.  Dragging a button along the bar reorders it.  A right-click on a
 * button offers Edit Tag… (on_library_edit_tag) and Remove from Favorites.
 * With no favorite the bar says how to make one.
 *
 * It owns neither the selection (the sidebar's) nor the tags (the
 * database's).
 */

/*
 * on_library_favorites_new — build the bar for the library window.  Called
 * once per window, by library_window.m.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the bar's view (the caller places it and sets its height:
 *         shared_look_favorites_bar_h plus a 1 pt rule shown, 0 hidden).
 */
NSView *on_library_favorites_new(NotesLibrary *lib);

/*
 * on_library_favorites_refresh — rebuild the buttons from the database and
 * light the selected tag's.  Called by the library window whenever the
 * selection is listed (on_library_window_location_changed), which every
 * refresh does.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_favorites_refresh(NotesLibrary *lib);
