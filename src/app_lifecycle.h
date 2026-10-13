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
 *   4. on_app_open_db(app)             — or, when no database file exists
 *                                      at on_app_db_path(), ask the user
 *                                      first; never create one silently
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
 *     editor could not save — then removes the backup and sync timers,
 *     closes the database and lets go of the app lock.  It is idempotent.
 *   - every route out goes through it: the app delegate calls it from
 *     applicationShouldTerminate: (Quit, the Dock, logout and restart all
 *     arrive there), cancelling the quit when it fails, and turns SIGTERM
 *     into [NSApp terminate:], which arrives there too.  Nothing calls
 *     exit() with windows open.
 *   - on_app_switch_db() flushes the same way before it closes the old
 *     database, and does not switch when an editor could not save.
 *
 * What is NOT covered: SIGKILL, a crash or power loss can lose the edits
 * typed since the last autosave (at most the autosave delay), and a
 * backup pass in flight is abandoned (it was writing a partial file, which
 * the rotation never counts and the next pass deletes).
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
 * (shared_db_path: $CAMBIO_SHARED_DB, else the pointer file both apps'
 * "Set as Default" writes, else ~/Library/Application Support/camb.io/
 * shared_data.db).
 *
 * Inputs:
 *   err — optional; on failure receives the reason (g_free it)
 *
 * Output: a new string (g_free it), or NULL when the location cannot be
 *         resolved (a relative $CAMBIO_SHARED_DB, a malformed pointer file).
 * ------------------------------------------------------------------------- */
gchar *on_app_db_path(gchar **err);

/* ---------------------------------------------------------------------------
 * on_app_open_db() — open the shared database at on_app_db_path(),
 * CREATING it if no file is there (the caller asks the user first when that
 * matters).  A file that is not a shared database is refused untouched.
 * There is deliberately NO fallback to another location: silently opening a
 * different database is how notes "disappear" and how writes are stranded
 * in the wrong file.  One configured database, or a clear failure.
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
 * integrity check, the backup poll and the pull of outside changes
 * (action_sync.h: once now, then its poll timer).
 *
 * Inputs:
 *   app — the application context
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_app_start_services(OnApp *app);

/* What on_app_switch_db did. */
typedef enum {
    ON_SWITCH_DONE,                  /* the app is on the new database       */
    ON_SWITCH_UNSAVED,               /* an editor could not save: no switch  */
    ON_SWITCH_FAILED,                /* the new file would not open          */
} on_switch_result_t;

/* ---------------------------------------------------------------------------
 * on_app_switch_db() — File > Open Database File…: switch the running app
 * to another shared database file, for this session or as the new default
 * of both apps.  Every editor is flushed and closed first; if one cannot
 * save, nothing changes.  If the new file cannot be opened (it is not a
 * shared database, it is on a network or synced folder, …) the previous
 * database is re-opened and nothing else changes.
 *
 * Inputs:
 *   app         — the application context
 *   path        — the database file to open
 *   set_default — TRUE to make it the default both apps open
 *                 (shared_db_set_default_path, which refuses while
 *                 $CAMBIO_SHARED_DB fixes the file); FALSE for this
 *                 session only
 *   err         — optional; on ON_SWITCH_FAILED receives the reason, the
 *                 shared module's words (g_free it)
 *
 * Output: ON_SWITCH_DONE when the app is now on `path` (the library has
 *         been pulled into and refreshed, the backup poll re-armed, the integrity
 *         check started).  ON_SWITCH_UNSAVED when an editor could not save:
 *         it is still open and the old database still in use.
 *         ON_SWITCH_FAILED when `path` could not be opened, or could not
 *         be made the default: the previous database is open again, or —
 *         if even that failed — app->db is NULL and the caller must quit.
 * ------------------------------------------------------------------------- */
on_switch_result_t on_app_switch_db(OnApp *app, const gchar *path,
                                    gboolean set_default, gchar **err);

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
