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
 * The specification is the GTK build's src/settings_window.c.
 */

/*
 * on_settings_install — register the "preferences" command, which shows the
 * Settings window.  While that window is open the module sets
 * OnApp.notify_db_state so the health plate and the backup line repaint
 * when a check starts or lands and when a backup pass ends, and clears it
 * when the window closes.  Called once at startup
 * by the app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_settings_install(NotesLibrary *lib);
