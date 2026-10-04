#import "library_ai.h"

/*
 * library_ai.m — STUB (work package H replaces this file).
 * See library_ai.h for what it is to become.
 */

/*
 * on_library_ai_install — see library_ai.h (stub: nothing is registered, so the
 * module's menu and toolbar items stay disabled).
 */
void
on_library_ai_install(NotesLibrary *lib)
{
}

/*
 * on_library_ai_pane_new — see library_ai.h (stub: an empty, hidden pane).
 */
NSView *
on_library_ai_pane_new(NotesLibrary *lib)
{
    NSView *pane = [[NSView alloc] initWithFrame:NSZeroRect];
    pane.hidden  = YES;
    return pane;
}
