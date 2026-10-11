#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_ai — the AI summary: the pane under the notes pane and the
 * subprocess that fills it.
 *
 * The module registers its own commands with the library, so the menus and
 * the toolbar reach it without any other module naming it; until it does,
 * those items are disabled.
 *
 * The specification is the GTK build's src/library_window.c, the AI
 * section.
 */

/*
 * on_library_ai_install — register the "ai-summary" command and set
 * OnApp.notify_ai_changed (the AI toolbar item is enabled only while the
 * ai_enabled preference is on).  Called once at startup by the app
 * delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_ai_install(NotesLibrary *lib);

/*
 * on_library_ai_pane_new — the AI summary pane the library window places
 * under the notes pane.  It is hidden until a summary is asked for.
 * Called once per window, by library_window.m.
 *
 * Output: the pane's view.
 */
NSView *on_library_ai_pane_new(NotesLibrary *lib);
