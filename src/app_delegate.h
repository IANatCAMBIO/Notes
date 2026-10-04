#import <Cocoa/Cocoa.h>

/*
 * NotesAppDelegate — the app's lifecycle: the launch sequence
 * (app_lifecycle.h), the main menu, the first-run question, and the one
 * way out (applicationShouldTerminate:, which flushes every editor).
 *
 * It owns the OnApp context and the NotesLibrary, and is the target of
 * every app-scope command (commands.h): run_app_command:.
 */
@interface NotesAppDelegate : NSObject <NSApplicationDelegate>
@end
