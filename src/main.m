#import <Cocoa/Cocoa.h>
#import "app_delegate.h"

#include "config.h"

/*
 * main — the app's entry point.  The order is the one in app_lifecycle.h:
 * the configuration first, then the application (whose delegate takes the
 * data directory's lock before anything else).
 *
 * Inputs:
 *   argc — argument count (unused: the app takes no arguments)
 *   argv — argument vector (unused)
 *
 * Output: does not return (the app ends in applicationShouldTerminate:).
 */
int
main(int argc, char *argv[])
{
    on_app_config_init();

    @autoreleasepool {
        NSApplication *app         = [NSApplication sharedApplication];
        NotesAppDelegate *delegate = [[NotesAppDelegate alloc] init];
        app.delegate               = delegate;
        [app run];
    }
    return 0;
}
