/* ===========================================================================
 * backup.c — backups of the shared database (see backup.h)
 * =========================================================================== */

#include "backup.h"
#include "db.h"
#include "main_service.h"            /* the worker, its hand-back, the timer */

#include <stdlib.h>
#include <time.h>

/*
 * module_message — move a message the shared module malloc'd into a
 * GLib string for this module's callers.
 *
 * Inputs:
 *   msg — the module's message (freed here), or NULL
 *   def — what to say when there is none
 *
 * Output: a new string; g_free it.
 */
static gchar *
module_message(char *msg, const gchar *def)
{
    gchar *out = g_strdup(msg != NULL ? msg : def);
    free(msg);
    return out;
}

/*
 * on_backup_dir — see backup.h.
 */
gchar *
on_backup_dir(OnApp *app, gchar **why)
{
    char *err = NULL;                /* the module's message                */
    char *dir = shared_db_backup_dir(app->db->shared, &err);
    if (dir == NULL) {
        gchar *m = module_message(err, "no backup folder");
        if (why != NULL) *why = m; else g_free(m);
        return NULL;
    }
    gchar *out = g_strdup(dir);
    free(dir);
    return out;
}

/* ---------------------------------------------------------------------------
 * BackupJob — one pass, owned by the worker and released on the main
 * thread in backup_finish().
 *
 * Fields:
 *   app      — application context (status bar, in-flight flag).
 *   db_path  — the database, copied so the worker never reads app->db.
 *   periodic — shared_db_backup_if_due rather than Back Up Now.
 *   message  — what to say when it is over, or NULL to say nothing.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp    *app;
    gchar    *db_path;
    gboolean  periodic;
    gchar    *message;
} BackupJob;

/*
 * backup_finish — the pass's main-thread tail (on_main_post): report,
 * release the in-flight guard, free the job.
 *
 * Inputs:
 *   data — the BackupJob
 *
 * Output: none.
 */
static void
backup_finish(void *data)
{
    BackupJob *job = data;           /* the pass that just ended            */
    job->app->backup_running = FALSE;
    if (job->message != NULL)
        on_app_status(job->app, "%s", job->message);
    g_free(job->message);
    g_free(job->db_path);
    g_free(job);
}

/*
 * backup_worker — the pass, on its own connection: the module's backup
 * (or its due-check-and-claim, for the timer), then the hand-back.
 *
 * Inputs:
 *   data — the BackupJob
 *
 * Output: none; the outcome rides the job back through backup_finish.
 */
static void
backup_worker(void *data)
{
    BackupJob  *job = data;          /* the pass in flight                  */
    gchar      *why = NULL;          /* why the connection did not open     */
    OnDatabase *db  = on_db_open_existing(job->db_path, &why);
    if (db == NULL) {
        job->message = g_strdup_printf("Backup failed: %s", why);
        g_free(why);
        on_main_post(backup_finish, job);
        return;
    }
    char *out = NULL;                /* the new backup's path               */
    char *err = NULL;                /* the module's message                */
    int   rc  = job->periodic
                ? shared_db_backup_if_due(db->shared, time(NULL), &out, &err)
                : (shared_db_backup(db->shared, &out, &err) == 0 ? 1 : -1);
    on_db_close(db);
    if (rc > 0) {
        gchar *base = g_path_get_basename(out);
        job->message = g_strdup_printf("Backed up to %s", base);
        g_free(base);
    } else if (rc < 0) {
        job->message = g_strdup_printf("Backup failed: %s",
                                       err != NULL ? err : "unknown error");
    }
    free(out);
    free(err);
    on_main_post(backup_finish, job);
}

/*
 * backup_launch — start one pass on a worker unless one is in flight or
 * there is no usable folder.
 *
 * Inputs:
 *   app      — the application context, its database open
 *   periodic — the timer's due-check (silent when refused) or Back Up Now
 *              (the refusal is posted as a status message)
 *
 * Output: none.
 */
static void
backup_launch(OnApp *app, gboolean periodic)
{
    if (app->backup_running)
        return;
    gchar *why = NULL;               /* why there is no usable folder       */
    gchar *dir = on_backup_dir(app, &why);
    if (dir == NULL) {
        if (!periodic)
            on_app_status(app, "%s", why);
        g_free(why);
        return;
    }
    g_free(dir);
    app->backup_running = TRUE;
    BackupJob *job = g_new0(BackupJob, 1);
    job->app      = app;
    job->db_path  = g_strdup(app->db->path);
    job->periodic = periodic;
    on_worker_run(backup_worker, job);
}

/*
 * on_backup_start — see backup.h.
 */
void
on_backup_start(OnApp *app)
{
    backup_launch(app, FALSE);
}

/*
 * backup_tick — one timer firing: ask the module whether a backup is due.
 *
 * Inputs:
 *   data — the OnApp
 *
 * Output: none; the timer repeats until on_backup_auto_start re-arms it or
 *         on_app_shutdown removes it.
 */
static void
backup_tick(void *data)
{
    OnApp *app = data;
    if (app->db != NULL)
        backup_launch(app, TRUE);
}

/*
 * on_backup_auto_start — see backup.h.
 */
void
on_backup_auto_start(OnApp *app)
{
    on_timer_remove(app->backup_timer);
    app->backup_timer = on_timer_add(SHARED_BACKUP_POLL_MIN * 60.0, true,
                                     backup_tick, app, NULL);
}
