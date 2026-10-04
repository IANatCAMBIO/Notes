/* ===========================================================================
 * app_lifecycle.h — opening the database, starting the services, shutting
 * down: the toolkit-free half of the app's main sequence
 *
 * The order the app delegate follows (the GTK build's main.c, kept):
 *
 *   1. on_app_config_init()            the ini, before anything reads it
 *   2. on_cli_run(argc, argv)          a command-line invocation ends here
 *   3. on_app_prefs_load(app), app->db_dir = on_app_config_load_db_dir()
 *   4. on_app_open_db(app)             — or, when no database file exists
 *                                      at on_app_db_path(), ask the user
 *                                      first; never create one silently
 *   5. install the UI's hooks in app (each module's install function)
 *   6. build the library window
 *   7. on_app_start_services(app)
 *   ...
 *   8. on_app_shutdown(app)            from applicationShouldTerminate:
 *
 * THE FLUSH-ON-QUIT CONTRACT
 *
 * An editor holds edits in memory for up to its autosave delay.  Nothing
 * may end the process, or close or switch the database, while an editor
 * still holds unsaved edits.  Therefore:
 *
 *   - app->flush_editors (installed by the editor module) closes EVERY
 *     editor window synchronously: each one saves its document if it is
 *     dirty, or deletes its note if it was left blank, before the hook
 *     returns.  No timers, no deferred work: when it returns, the database
 *     holds everything.
 *   - on_app_shutdown() is the ONE way the app ends: it stops the IPC
 *     server (so no command arrives half-way through), calls
 *     app->flush_editors, removes the backup timer and closes the
 *     database.  It is idempotent.
 *   - every route out goes through it: the app delegate calls it from
 *     applicationShouldTerminate: (Quit, the Dock, logout and restart all
 *     arrive there) and turns SIGTERM into [NSApp terminate:], which
 *     arrives there too.  Nothing calls exit() with windows open.
 *   - on_app_switch_db() flushes the same way before it closes the old
 *     database.
 *
 * What is NOT covered: SIGKILL, a crash or power loss can lose the edits
 * typed since the last autosave (at most the autosave delay), and a
 * backup pass in flight is abandoned (its unverified file is not counted
 * by the next pass's prune, which only ever runs after a verified copy).
 *
 * Main thread only.
 * =========================================================================== */

#ifndef ON_APP_LIFECYCLE_H
#define ON_APP_LIFECYCLE_H

#include "app_core.h"
#include "ipc.h"

/* ---------------------------------------------------------------------------
 * on_app_db_path() — where the database is expected: app->db_dir plus the
 * fixed file name, or the default path in the data directory.
 *
 * Inputs:
 *   app — the application context (reads app->db_dir)
 *
 * Output: a new string; g_free it.
 * ------------------------------------------------------------------------- */
gchar *on_app_db_path(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_open_db() — open the database at on_app_db_path(), CREATING it if
 * no file is there (the caller asks the user first when that matters).
 * There is deliberately NO fallback to another location: silently opening a
 * different database is how notes "disappear" and how writes are stranded
 * in the wrong file.  One configured database, or a clear failure.
 *
 * Inputs:
 *   app — the application context; app->db is set on success
 *
 * Output: TRUE when the database is open.  On FALSE the reason is on
 *         stderr and app->db is NULL; the caller reports it and quits.
 * ------------------------------------------------------------------------- */
gboolean on_app_open_db(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_start_services() — everything that starts once the database is
 * open, the hooks are installed and the library window exists: the IPC
 * server, the integrity check, the backup timer, and any action the
 * command line queued for this launch (quicknote, note open).
 *
 * Inputs:
 *   app — the application context
 *
 * Output: the IPC server's result.  on_ipc_server_busy means another
 *         instance owns this data directory: NOTHING else was started, and
 *         the caller must quit without writing.  on_ipc_server_failed
 *         means everything else started but the command line cannot reach
 *         this app; the caller tells the user.
 * ------------------------------------------------------------------------- */
on_ipc_server_result_t on_app_start_services(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_switch_db() — File > Open Database File…: switch the running app
 * to another database file, for this session or as the new default.  Every
 * editor is flushed and closed first.  If the new file cannot be opened the
 * previous database is re-opened and nothing else changes.
 *
 * Inputs:
 *   app         — the application context
 *   path        — the database file to open
 *   set_default — TRUE to persist its directory as `db_dir`; FALSE for
 *                 this session only (app->db_transient)
 *
 * Output: TRUE when the app is now on `path` (the library has been told to
 *         refresh, the backup timer re-armed, the integrity check started).
 *         FALSE when it could not be opened: the previous database is open
 *         again, or — if even that failed — app->db is NULL and the caller
 *         must quit.
 * ------------------------------------------------------------------------- */
gboolean on_app_switch_db(OnApp *app, const gchar *path, gboolean set_default);

/* ---------------------------------------------------------------------------
 * on_app_shutdown() — end the app's work in the one safe order (see the
 * contract above).  Safe to call more than once, and with no database open.
 *
 * Inputs:
 *   app — the application context
 *
 * Output: none.  When it returns no editor is open, no timer is armed, the
 *         socket file is gone and app->db is NULL.
 * ------------------------------------------------------------------------- */
void on_app_shutdown(OnApp *app);

#endif /* ON_APP_LIFECYCLE_H */
