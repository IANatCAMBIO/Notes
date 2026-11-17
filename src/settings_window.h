#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * settings_window — the Settings window: every preference in notes.ini
 * (table-driven checkboxes that write through at once), the Apple
 * Calendar section shared with Tasks and the Apple Notes copy.
 *
 * The module registers its own commands with the library, so the menus and
 * the toolbar reach it without any other module naming it; until it does,
 * those items are disabled.
 *
 * The specification is the GTK build's src/settings_window.c.
 */

/*
 * on_settings_install — register the "preferences" command, which shows the
 * Settings window.  Called once at startup by the app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_settings_install(NotesLibrary *lib);
