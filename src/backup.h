/* ===========================================================================
 * backup.h — backups of the shared database, through the shared module
 *
 * The copy (VACUUM INTO a new timestamped file), its verification, the
 * retention and the schedule all belong to the shared module (shared_db.h:
 * shared_db_backup, shared_db_backup_if_due), and the policy lives IN the
 * database, in shared_settings, so one policy serves Notes and Tasks:
 * whichever app is running makes the backup when it is due, and it is
 * made once.  The keys are SHARED_SETTING_BACKUP_*:
 *
 *   backup_enabled       1|0, default SHARED_BACKUP_ENABLED_DEFAULT.
 *   backup_dir           destination folder; unset = the module's default
 *                        (~/Library/Application Support/camb.io/backups).
 *   backup_interval_min  minutes between backups, default
 *                        SHARED_BACKUP_INTERVAL_MIN_DEFAULT; 0 = only when
 *                        "Back Up Now" is pressed.
 *   backup_keep          files retained, SHARED_BACKUP_KEEP_MIN..MAX.
 *   backup_last_at       when the last backup was made (the module's).
 *   backup_last_error    why the last one failed (the module's).
 *
 * What this module adds is Notes' part: the worker (with its OWN
 * connection — a connection never crosses threads — so a slow destination
 * never blocks the UI) and the periodic poll timer.  (A sandboxed run —
 * $CAMBIO_SHARED_DB set — with no backup_dir backs up into `backups/`
 * beside that file: the module's rule, so dev and test backups never reach
 * the per-user folder.)
 *
 * The module never deletes a database's -wal / -shm files; nor does this.
 *
 * Main thread only, apart from the pass's own worker.  A process with no
 * main run loop (the development seeder) never finishes a pass or ticks.
 * =========================================================================== */

#ifndef BLUE_BACKUP_H
#define BLUE_BACKUP_H

#include "app_core.h"

/* How often, in minutes, the periodic timer asks the module whether a
 * backup is due.  The module answers from the policy above, so a change in
 * Settings (or made by Tasks) takes effect at the next poll.               */
#define ON_BACKUP_POLL_MIN 1

/* ---------------------------------------------------------------------------
 * on_backup_setting() — one integer of the backup policy, read from
 * shared_settings on the app's connection, with the module's default when
 * it is unset (and backup_keep held to SHARED_BACKUP_KEEP_MIN..MAX).
 *
 * Inputs:
 *   app — the application context, its database open
 *   key — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN, _KEEP or _LAST_AT
 *
 * Output: the value; the default (0 for LAST_AT) when unset or unreadable.
 * ------------------------------------------------------------------------- */
gint64 on_backup_setting(OnApp *app, const gchar *key);

/* ---------------------------------------------------------------------------
 * on_backup_set_setting() — write one integer of the backup policy to
 * shared_settings (a failure is logged).  Both apps read it from there.
 *
 * Inputs:
 *   app   — the application context, its database open
 *   key   — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN or _KEEP
 *   value — the new value
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_backup_set_setting(OnApp *app, const gchar *key, gint64 value);

/* ---------------------------------------------------------------------------
 * on_backup_dir() — where backups go: shared_db_backup_dir (the backup_dir
 * setting, else the module's default; created when missing).  The single
 * answer to "where?": Settings shows it and every pass asks it
 * first, so the label cannot disagree with the behaviour.
 *
 * Inputs:
 *   app — the application context, its database open
 *   why — optional; receives the reason when the answer is NULL (g_free)
 *
 * Output: the folder (g_free it), or NULL.
 * ------------------------------------------------------------------------- */
gchar *on_backup_dir(OnApp *app, gchar **why);

/* ---------------------------------------------------------------------------
 * on_backup_set_dir() — make `dir` the backup destination, if it can be:
 * an absolute path to a writable folder.  A refused folder is not stored.
 *
 * Inputs:
 *   app — the application context, its database open
 *   dir — the folder chosen
 *   why — optional; receives the reason on refusal (g_free it)
 *
 * Output: TRUE when it is now the destination.
 * ------------------------------------------------------------------------- */
gboolean on_backup_set_dir(OnApp *app, const gchar *dir, gchar **why);

/* ---------------------------------------------------------------------------
 * on_backup_start() — "Back Up Now": one backup on a worker, regardless of
 * the schedule (shared_db_backup).  Refused with a status message when
 * on_backup_dir refuses; ignored while a pass is in flight.  The outcome
 * is posted as a status message.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_backup_start(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_backup_auto_start() — (re)arm the periodic timer: every
 * ON_BACKUP_POLL_MIN minutes a worker runs shared_db_backup_if_due, which
 * backs up only when the shared policy says one is due and claims it, so
 * a second app polling at once does not make another.  A poll that finds
 * nothing due, or no usable folder, says nothing; one that backs up or
 * fails posts a status message.  Call again after opening another
 * database: the timer carries its path.  on_app_shutdown removes it.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_backup_auto_start(OnApp *app);

#endif /* BLUE_BACKUP_H */
