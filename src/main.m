#import <Cocoa/Cocoa.h>
#import "app_delegate.h"

#include "cli.h"
#include "config.h"

/*
 * main — the app's entry point.  The order is the one in app_lifecycle.h:
 * the configuration first, then the command line (the `notes` tool starts
 * this executable with its own arguments when a command needs a window:
 * on_cli_run then queues `quicknote` or `note open` for after launch, or —
 * if another instance has started meanwhile — hands it to that instance and
 * this one exits), and only then the application.
 *
 * Inputs:
 *   argc — argument count
 *   argv — argument vector
 *
 * Output: the exit status of a command handled without starting the app;
 *         otherwise this does not return (the app ends in
 *         applicationShouldTerminate:).
 */
int
main(int argc, char *argv[])
{
    on_app_config_init();
    int status = on_cli_run(argc, argv);
    if (status >= 0) return status;

    @autoreleasepool {
        NSApplication *app         = [NSApplication sharedApplication];
        NotesAppDelegate *delegate = [[NotesAppDelegate alloc] init];
        app.delegate               = delegate;
        [app run];
    }
    return 0;
}
