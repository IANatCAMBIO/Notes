#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * settings_window — the Settings window: every preference in notes.ini
 * (table-driven checkboxes that write through at once), the database health
 * plate, the rotating-backup controls and the AI settings.
 *
 * The module registers its own commands with the library, so the menus and
 * the toolbar reach it without any other module naming it; until it does,
 * those items are disabled.
 *
 * STATUS: settings_window.m is a STUB for work package H that registers
 * nothing.  The specification is
 * porting/gtk_reference/src/settings_window.c.  This header is FROZEN.
 */

/*
 * on_settings_install — register the "preferences" command, which shows the
 * Settings window.  While that window is open the module sets
 * OnApp.notify_db_health so the health plate repaints when a check starts
 * or lands, and clears it when the window closes.  Called once at startup
 * by the app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_settings_install(NotesLibrary *lib);
