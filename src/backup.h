/* ===========================================================================
 * backup.h — optional rotating database backups for Notes
 *
 * OFF by default.  Enabled in Settings → Database, where the user may also
 * pick a destination directory — and when they have not, backups go to a
 * `backups/` folder inside the DEFAULT DATABASE DIRECTORY under the home
 * directory (~/.local/share/notes/backups).  Switching the feature on
 * therefore always does something; there is no enabled-but-inert state to
 * fall into.
 *
 * The reason to choose a folder anyway: the point of the feature is a copy
 * somewhere INDEPENDENT of wherever the live file lives, so one mishap
 * cannot take both.  That matters here because Notes' database is routinely
 * pointed at a shared or synced folder (File → Open Database…), and a sync
 * daemon can replace a file underneath an open SQLite connection.  The
 * default is independent of a database kept elsewhere — but it is a
 * SUBFOLDER of the database's own directory when the database sits at its
 * default location, and Settings says so rather than implying otherwise.
 *
 * Each pass writes "notes-YYYYMMDD-HHMMSS.db" into that directory and then
 * prunes the oldest, keeping at most `backup_keep` files — bounded by
 * design, so an hourly timer cannot fill a disk.  A pass whose source has
 * not changed since the last backup does nothing at all, so an idle app
 * writes nothing.
 *
 * Safety rules this module will not bend:
 *   - the copy goes through on_db_backup_to(), SQLite's online backup API,
 *     never a byte copy: it snapshots a live database consistently and so
 *     cannot capture a torn page;
 *   - every new backup is VERIFIED (on_db_verify_file) before it counts,
 *     and a copy that fails verification is removed rather than left to be
 *     mistaken for a good one;
 *   - PRUNING HAPPENS ONLY AFTER a new backup has verified, so a failing
 *     backup can never erode the history that is already there.
 *
 * Threading: a worker thread with its OWN SQLite connection (a connection
 * must not cross threads — the same rule the search worker follows), so a
 * slow or unreachable destination — a network mount, a sleeping external
 * disk — never blocks the UI.  Completion is marshalled back with
 * g_idle_add.
 *
 * Config keys (all in the [notes] group):
 *   backup_enabled       1|0, default 0 — the master switch.
 *   backup_dir           destination directory; absent = a backups/ folder
 *                        inside the default database directory.
 *   backup_interval_min  minutes between passes, default 60; 0 = only when
 *                        "Back Up Now" is pressed.
 *   backup_keep          how many files to retain, default 10.
 *   backup_source_stamp  the source's identity at the last SUCCESSFUL
 *                        backup, so an unchanged database is not copied
 *                        again.  Written here rather than into the database
 *                        because Notes keeps no key-value table — its
 *                        settings table was dropped in 2026-07 and every
 *                        preference lives in the ini.  The stamp names the
 *                        source PATH as well as its size and mtime, so
 *                        opening a different database simply does not match
 *                        and gets backed up, which is the right answer.
 * =========================================================================== */

#ifndef BLUE_BACKUP_H
#define BLUE_BACKUP_H

#include "app.h"

/* Default retention and cadence, shared with the Settings spin buttons so
 * the UI and the timer cannot disagree about what "unset" means.            */
#define ON_BACKUP_KEEP_DEFAULT     10
#define ON_BACKUP_INTERVAL_DEFAULT 60

/* ---------------------------------------------------------------------------
 * on_backup_dir() — where backups actually go: `backup_dir` when it names
 * something, otherwise a `backups/` folder inside the default database
 * directory, CREATED if missing so the fallback is always usable.  That
 * default is TIDY, not independent: it shares the fate of the folder
 * holding the database, and Settings says so.
 *
 * Returns a new string (g_free).  Never NULL.  This is the single answer to
 * "where?" — Settings displays it and the worker writes to it, so the label
 * can never disagree with the behaviour.
 * ------------------------------------------------------------------------- */
gchar *on_backup_dir(void);

/* ---------------------------------------------------------------------------
 * on_backup_ready() — is the feature usable right now?
 *
 * Returns TRUE when backups are enabled and on_backup_dir() is a writable
 * directory.  On FALSE, *reason (optional; g_free) says which of those
 * failed, in words fit for a status bar.  Callers use it to explain
 * themselves rather than failing silently.
 * ------------------------------------------------------------------------- */
gboolean on_backup_ready(gchar **reason);

/* ---------------------------------------------------------------------------
 * on_backup_start() — run one backup pass on a worker thread.
 *
 * Early-outs, each with a status message: disabled or misconfigured (see
 * on_backup_ready), and "already running".  A pass whose source is unchanged
 * since the last backup writes nothing and says nothing.  Main thread only.
 *
 * Inputs:
 *   app     — application context, for the status bar and the in-flight flag
 *   db_path — the source database file
 *
 * Output:
 *   none; the outcome is reported in the library's status bar.
 * ------------------------------------------------------------------------- */
void on_backup_start(OnApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * on_backup_auto_start() — (re)install the periodic timer from
 * `backup_interval_min`, or remove it when backups are off.  Safe to call
 * again whenever the settings change; MUST be called again whenever the app
 * opens a different database, since the timer carries the path.
 *
 * It does NOT run an immediate pass: an app that has just started has
 * nothing new to preserve, and a backup on every launch would churn the
 * rotation for anyone who opens the app often.
 * ------------------------------------------------------------------------- */
void on_backup_auto_start(OnApp *app, const gchar *db_path);

#endif /* BLUE_BACKUP_H */
