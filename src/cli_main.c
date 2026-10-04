/* ===========================================================================
 * cli_main.c — the `notes` command-line tool
 *
 * The GTK build was one executable that was both the command line and the
 * GUI.  A Cocoa app must run from its bundle to have an identity (Info.plist,
 * Dock tile, menu bar), so here they are two executables in the same bundle:
 *
 *     NotesAppKit.app/Contents/MacOS/NotesAppKit   the app
 *     NotesAppKit.app/Contents/MacOS/notes         this tool
 *
 * Every command, flag, output format and exit code is cli.c's, unchanged —
 * a Claude skill and scripts drive it.  The one thing this file adds is
 * what happens when cli.c answers "start the GUI" (-1): the tool launches
 * the app executable that sits beside it, passes its own arguments along
 * (the app runs the same on_cli_run, which queues `quicknote` or
 * `note open PATH` as its pending action) and exits 0.  The app inherits
 * the environment, so NOTES_DATA_DIR reaches it and a development tool can
 * never start an app on the real data directory.
 *
 * Differences from the GTK build, all in the "start the GUI" corner:
 *   - `notes quicknote` / `notes note open` with no app running return as
 *     soon as the app is launched instead of becoming the app;
 *   - a first word that is not a command is an error (exit 1) rather than
 *     being handed to GTK's option parser.
 * =========================================================================== */

#include "cli.h"
#include "config.h"
#include "identity.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

/*
 * app_executable — the app executable that sits beside this tool in the
 * bundle's Contents/MacOS.
 *
 * Inputs:
 *   out  — receives the absolute path
 *   size — its capacity
 *
 * Output: 0 on success, -1 when this tool's own location cannot be
 *         resolved.
 */
static int
app_executable(char *out, size_t size)
{
    char     self[PATH_MAX];
    uint32_t self_size = sizeof self;
    char     real[PATH_MAX];
    if (_NSGetExecutablePath(self, &self_size) != 0 ||
        realpath(self, real) == NULL) {
        return -1;
    }
    char dir[PATH_MAX];
    if (dirname_r(real, dir) == NULL) {
        return -1;
    }
    snprintf(out, size, "%s/%s", dir, ON_BUNDLE_NAME);
    return 0;
}

/*
 * launch_app — start the app, detached, with this tool's arguments, and
 * return without waiting for it.  The app gets its own session and
 * /dev/null on its standard streams, so it neither dies with the terminal
 * nor holds a caller's pipe open.
 *
 * Inputs:
 *   argc, argv — this tool's arguments; argv[1..] are passed on
 *
 * Output: 0 when the app was started, 2 otherwise (reported on stderr).
 */
static int
launch_app(int argc, char **argv)
{
    char app[PATH_MAX];
    if (app_executable(app, sizeof app) != 0) {
        fprintf(stderr, "error: cannot locate %s beside this tool\n",
                ON_BUNDLE_NAME);
        return 2;
    }

    char **app_argv = calloc((size_t)argc + 1, sizeof *app_argv);
    app_argv[0] = app;
    for (int i = 1; i < argc; i++) {
        app_argv[i] = argv[i];
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    int err = posix_spawn(NULL, app, &actions, &attr, app_argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    free(app_argv);
    if (err != 0) {
        fprintf(stderr, "error: cannot start %s: %s\n", app, strerror(err));
        return 2;
    }
    return 0;
}

/*
 * main — run one command-line invocation.
 *
 * Inputs:
 *   argc, argv — the program arguments
 *
 * Output: the process exit status: cli.c's (0 ok, 1 usage, 2 failure) for
 *         a command; 0 after launching the app, 2 if it could not be
 *         launched; 1 for an unknown command.
 */
int
main(int argc, char **argv)
{
    on_app_config_init();

    int status = on_cli_run(argc, argv);
    if (status >= 0) {
        return status;
    }
    /* -1: no command at all, or a GUI command with no app running — both
     * mean "start the app".  Anything else was simply not a command.       */
    if (argc >= 2 && !on_ipc_has_pending()) {
        fprintf(stderr, "error: unknown command: %s (try '%s help')\n",
                argv[1], ON_CLI_NAME);
        return 1;
    }
    return launch_app(argc, argv);
}
