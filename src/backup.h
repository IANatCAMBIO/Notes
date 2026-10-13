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
 *   backup_dir           destination folder; unset = the module's default:
 *                        `backups` beside the OPEN database file (the
 *                        per-user file's is camb.io/backups).
 *   backup_interval_min  minutes between backups, default
 *                        SHARED_BACKUP_INTERVAL_MIN_DEFAULT; 0 = only when
 *                        "Back Up Now" is pressed.
 *   backup_keep          files retained, SHARED_BACKUP_KEEP_MIN..MAX.
 *   backup_last_at       when the last backup was made (the module's).
 *   backup_last_error    why the last one failed (the module's; Settings
 *                        shows it).
 *
 * Settings reads the policy through shared_backup_policy_get (defaults and
 * ranges are the module's), writes it with shared_settings_set, and
 * chooses the folder with shared_db_set_backup_dir.  Backup files are
 * named SHARED_DB_BACKUP_PREFIX "YYYYMMDD-HHMMSSZ.db", the time in UTC.
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
 * SHARED_BACKUP_POLL_MIN minutes a worker runs shared_db_backup_if_due, which
 * backs up only when the shared policy says one is due and claims it, so
 * a second app polling at once does not make another.  A poll that finds
 * nothing due, or no usable folder, says nothing; one that backs up or
 * fails posts a status message.  Each tick reads app->db when it fires,
 * so the timer follows a database opened later; re-arming after an open
 * only restarts the interval.  on_app_shutdown removes it.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_backup_auto_start(OnApp *app);

#endif /* BLUE_BACKUP_H */
