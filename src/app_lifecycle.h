/* ===========================================================================
 * app_lifecycle.h — opening the database, starting the services, shutting
 * down: the toolkit-free half of the app's main sequence
 *
 * The order the app delegate follows (the GTK build's main.c, kept):
 *
 *   1. on_app_config_init()            the ini, before anything reads it
 *   2. on_app_lock_take()              this copy is THE one on the data
 *                                      directory, or it quits untouched
 *   3. on_app_prefs_load(app)
 *   4. on_app_open_db(app)             the existing database, or a notice
 *                                      and a quit: Notes never creates one
 *   5. install the UI's hooks in app (each module's install function)
 *   6. build the library window
 *   7. on_app_start_services(app)
 *   ...
 *   8. on_app_shutdown(app)            from applicationShouldTerminate:
 *
 * ONE APP PER DATA DIRECTORY
 *
 * The app holds an exclusive flock on <data dir>/notes.lock
 * (ON_LOCK_FILENAME) from launch to on_app_shutdown; the kernel drops it
 * if the process dies.  A second copy started on the same data directory
 * finds it held before it has opened the database or shown a window, and
 * quits (raising the first).  Without it, two copies would each hold
 * open editors over the same notes.  (Other WRITERS of the shared database
 * — Tasks, an agent — are expected: their changes reach Notes through
 * the pull, action_sync.h.)  The development seeder takes the same lock,
 * so `make seed` cannot wipe a database an app is running on.
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
 *     returns.  No timers, no deferred work: when it returns TRUE, the
 *     database holds everything.  An editor whose save fails stays open
 *     and the hook returns FALSE.
 *   - on_app_shutdown() is the ONE way the app ends: it calls
 *     app->flush_editors — and stops there, changing nothing else, when an
 *     editor could not save — then removes the sync timer, closes the
 *     database and lets go of the app lock.  It is idempotent.
 *   - every route out goes through it: the app delegate calls it from
 *     applicationShouldTerminate: (Quit, the Dock, logout and restart all
 *     arrive there), cancelling the quit when it fails, and turns SIGTERM
 *     into [NSApp terminate:], which arrives there too.  Nothing calls
 *     exit() with windows open.
 *
 * What is NOT covered: SIGKILL, a crash or power loss can lose the edits
 * typed since the last autosave (at most the autosave delay).
 *
 * Main thread only.
 * =========================================================================== */

#ifndef ON_APP_LIFECYCLE_H
#define ON_APP_LIFECYCLE_H

#include "app_core.h"

/* Outcome of taking the app lock.                                           */
typedef enum {
    on_app_lock_held,                /* this process holds it                */
    on_app_lock_busy,                /* another live process holds it        */
    on_app_lock_failed,              /* it could not be tried (stderr)       */
} on_app_lock_result_t;

/* ---------------------------------------------------------------------------
 * on_app_lock_take() — make this process THE one using the data directory
 * (see ONE APP PER DATA DIRECTORY above).  A copy that is just quitting
 * still holds the lock for a moment, so a busy lock is retried briefly
 * before the answer is "busy".  Held until on_app_shutdown or the end of
 * the process.
 *
 * Output: on_app_lock_held (also when already held), on_app_lock_busy
 *         when another live process holds it, on_app_lock_failed when it
 *         could not be tried (reported on stderr).
 * ------------------------------------------------------------------------- */
on_app_lock_result_t on_app_lock_take(void);

/* ---------------------------------------------------------------------------
 * on_app_db_path() — where the shared database is expected
 * (shared_db_path: $CAMBIO_SHARED_DB, else the pointer file
 * ~/Library/Application Support/camb.io/location, else
 * ~/Library/Application Support/camb.io/shared_data.db).
 *
 * Inputs:
 *   err — optional; on failure receives the reason (g_free it)
 *
 * Output: a new string (g_free it), or NULL when the location cannot be
 *         resolved (a relative $CAMBIO_SHARED_DB, a malformed pointer file).
 * ------------------------------------------------------------------------- */
gchar *on_app_db_path(gchar **err);

/* ---------------------------------------------------------------------------
 * on_app_open_db() — open the shared database at on_app_db_path().  It is
 * deliberately NEVER created here: camb.io Database Manager (camb.io_db_mgr.app)
 * creates the file and looks after it, and a missing one is refused with
 * a reason (on_db_open_existing), as is a file that is not a shared
 * database.  There is no fallback to another location either: silently
 * opening a different database is how notes "disappear" and how writes
 * are stranded in the wrong file.  One configured database, or a clear
 * failure.
 *
 * Inputs:
 *   app — the application context; app->db is set on success
 *   err — optional; on failure receives the reason (g_free it)
 *
 * Output: TRUE when the database is open.  On FALSE app->db is NULL; the
 *         caller reports the reason and quits.
 * ------------------------------------------------------------------------- */
gboolean on_app_open_db(OnApp *app, gchar **err);

/* ---------------------------------------------------------------------------
 * on_app_start_services() — everything that starts once the database is
 * open, the hooks are installed and the library window exists: the
 * sweep of attached files no note names (a crash leaves them) and the
 * pull of outside changes (action_sync.h: once now, then its poll timer).
 *
 * Inputs:
 *   app — the application context
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_app_start_services(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_shutdown() — end the app's work in the one safe order (see the
 * contract above).  Safe to call more than once, and with no database open.
 *
 * Inputs:
 *   app — the application context
 *
 * Output: TRUE when the work has ended: no editor is open, no timer is
 *         armed, app->db is NULL and the app lock is released.  FALSE when an
 *         editor could not save its edits: it is still open, and nothing
 *         else was stopped — the caller must not end the process.
 * ------------------------------------------------------------------------- */
gboolean on_app_shutdown(OnApp *app);

#endif /* ON_APP_LIFECYCLE_H */
