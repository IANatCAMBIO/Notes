#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * search_window — the search window: a query (search_query.h), a scope, and
 * the results of a search run on a worker (search_job.h).
 *
 * The module registers its own commands with the library, so the menus and
 * the toolbar reach it without any other module naming it; until it does,
 * those items are disabled.
 *
 * STATUS: search_window.m is a STUB for work package H that registers
 * nothing.  The specification is porting/gtk_reference/src/search_window.c
 * (its worker is already lifted into search_job.c).  This header is FROZEN.
 */

/*
 * on_search_install — register the "search", "find" (in the library, Find
 * opens the search window) and "search-here" commands.  Called once at
 * startup by the app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_search_install(NotesLibrary *lib);

/*
 * on_search_window_open — show a new search window.
 *
 * Inputs:
 *   lib                — the library (its selection is the "Selected
 *                        Folder/Tag" scope, read each time Search is
 *                        pressed: on_library_search_scope)
 *   scope_to_selection — YES preselects "Selected Folder/Tag" (Search
 *                        Here…); NO preselects All Notes
 *   query              — text to search for at once (the library toolbar's
 *                        search field); nil or @"" just opens the window
 */
void on_search_window_open(NotesLibrary *lib, BOOL scope_to_selection, NSString *query);
