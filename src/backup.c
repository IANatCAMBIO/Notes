/* ===========================================================================
 * backup.c — optional rotating database backups (see backup.h)
 * =========================================================================== */

#include "backup.h"
#include "db.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

/* Backup filenames: sortable, so lexical order IS chronological order and
 * the prune can pick victims without parsing dates or trusting mtimes on a
 * destination that may be a network mount.                                  */
#define BACKUP_PREFIX "notes-"
#define BACKUP_SUFFIX ".db"

/* The ini key holding the source's identity at the last SUCCESSFUL backup
 * (see the header for why it lives in the ini and not the database).        */
#define BACKUP_STAMP_KEY "backup_source_stamp"

/* The default destination: a folder of its own inside the default database
 * directory, so the live notes.db does not sit among the copies.            */
#define BACKUP_SUBDIR "backups"

/*
 * source_stamp — a cheap identity for "the state of the last backup": the
 * DESTINATION and the SOURCE PATH, plus the source file's size and
 * modification time.  Not a hash: this runs on every timer tick, and the
 * question is only "would another backup right now be a duplicate", which
 * mtime answers for a file SQLite has written.
 *
 * The destination is part of it deliberately.  Without it, choosing a new
 * backup folder would produce NO backup there until the database happened
 * to change — the new folder would sit empty and the feature would look
 * broken.  Including it means a changed destination counts as "not backed
 * up yet", which is what the user just asked for by changing it.  The
 * source path is in it for the same reason one level up: the stamp lives in
 * the ini, which outlives any one database, so File → Open Database must
 * not look like "already backed up".
 *
 * Inputs:
 *   path     — the source database file
 *   dest_dir — where its backups go
 *
 * Output:
 *   a new string (g_free), or NULL when the file cannot be stat'd.
 */
static gchar *
source_stamp(const gchar *path, const gchar *dest_dir)
{
    GStatBuf sb;                     /* the source's size and mtime         */
    if (g_stat(path, &sb) != 0)
        return NULL;
    return g_strdup_printf("%s|%s|%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                           dest_dir != NULL ? dest_dir : "",
                           path != NULL ? path : "",
                           (gint64)sb.st_size, (gint64)sb.st_mtime);
}

/*
 * backup_keep — the retention bound, read from config with the shared
 * default and clamped to something sane: a hand-edited ini must not be able
 * to ask for zero retention, which would delete a backup the moment it was
 * made.
 *
 * Output:
 *   how many backup files to retain, 1..500.
 */
static gint
backup_keep(void)
{
    gchar *v = on_app_config_get("backup_keep");
    gint   n = v != NULL ? atoi(v) : ON_BACKUP_KEEP_DEFAULT;
    g_free(v);
    return CLAMP(n, 1, 500);
}

/*
 * backup_interval — minutes between passes, from config, with a negative in
 * a hand-edited ini read as "manual only" rather than as a wild timer.
 *
 * Output:
 *   minutes, 0 = only when "Back Up Now" is pressed.
 */
static gint
backup_interval(void)
{
    gchar *v = on_app_config_get("backup_interval_min");
    gint   n = v != NULL ? atoi(v) : ON_BACKUP_INTERVAL_DEFAULT;
    g_free(v);
    return MAX(n, 0);
}

gchar *
on_backup_dir(void)
{
    gchar *dir = on_app_config_get("backup_dir");
    if (dir != NULL && *dir != '\0')
        return dir;
    g_free(dir);
    /* Built off the default database PATH so the two can never drift apart,
     * and created here so the fallback is usable with no setup at all.  It
     * is a SUBDIRECTORY rather than the database's own folder because
     * backups and the live file mixed together is a folder nobody can read
     * at a glance: the pruning already has to match `notes-*.db` so as not
     * to touch `notes.db`, and a listing that needs a filename rule to be
     * understood is one a human will eventually get wrong.  It buys
     * TIDINESS and nothing else — a subfolder shares the fate of its parent
     * exactly, so this is no more independent than the database's own
     * folder, and Settings still says so.                                   */
    gchar *def  = on_db_default_path();
    gchar *base = g_path_get_dirname(def);
    gchar *sub  = g_build_filename(base, BACKUP_SUBDIR, NULL);
    g_free(def);
    g_free(base);
    g_mkdir_with_parents(sub, 0755);
    return sub;
}

gboolean
on_backup_ready(gchar **reason)
{
    if (reason != NULL)
        *reason = NULL;
    if (!on_app_config_get_bool("backup_enabled", FALSE)) {
        if (reason != NULL)
            *reason = g_strdup("Backups are switched off "
                               "(Settings \xe2\x86\x92 Database)");
        return FALSE;
    }
    /* There is no "no folder chosen" case: on_backup_dir always answers,
     * and creates the fallback.  What is left is a CHOSEN folder that has
     * since gone away or turned read-only — a removed external disk, most
     * likely — which is worth saying plainly rather than silently
     * redirecting somewhere the user is not looking.                       */
    gchar    *dir = on_backup_dir();
    gboolean  ok  = FALSE;           /* is the destination usable?          */
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        if (reason != NULL)
            *reason = g_strdup_printf("Backup folder does not exist: %s",
                                      dir);
    } else if (g_access(dir, W_OK) != 0) {
        if (reason != NULL)
            *reason = g_strdup_printf("Backup folder is not writable: %s",
                                      dir);
    } else {
        ok = TRUE;
    }
    g_free(dir);
    return ok;
}

/*
 * backup_name_cmp — g_ptr_array_sort comparator over the paths themselves.
 *
 * NOT g_strcmp0 directly: g_ptr_array_sort hands its comparator POINTERS TO
 * THE ELEMENTS (gchar **), so passing g_strcmp0 compares the pointer values
 * as if they were the strings — an ordering that looks plausible and is
 * arbitrary.  That is load-bearing here, because this order is what decides
 * which backup the prune DELETES: measured on a four-file rotation with
 * keep=3, it removed the second-oldest and kept the oldest.
 *
 * Inputs:
 *   a, b — gchar ** into the array.
 *
 * Output:
 *   the usual <0 / 0 / >0.
 */
static gint
backup_name_cmp(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

/*
 * backup_list — every backup file in `dir`, sorted oldest first.
 *
 * Matched by our own PREFIX and SUFFIX only, so nothing else the user keeps
 * in that folder is ever a prune candidate — the destination may well be a
 * folder they also use for other things.
 *
 * Inputs:
 *   dir — the destination directory
 *
 * Output:
 *   a GPtrArray of full paths, oldest first; free with
 *   g_ptr_array_free(a, TRUE).  Never NULL; empty when the directory cannot
 *   be read.
 */
static GPtrArray *
backup_list(const gchar *dir)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GDir      *d   = g_dir_open(dir, 0, NULL);
    if (d == NULL)
        return out;
    const gchar *name;               /* one directory entry                 */
    while ((name = g_dir_read_name(d)) != NULL) {
        if (g_str_has_prefix(name, BACKUP_PREFIX) &&
            g_str_has_suffix(name, BACKUP_SUFFIX))
            g_ptr_array_add(out, g_build_filename(dir, name, NULL));
    }
    g_dir_close(d);
    /* Lexical == chronological, by construction of the filename.           */
    g_ptr_array_sort(out, backup_name_cmp);
    return out;
}

/* ---------------------------------------------------------------------------
 * BackupJob — one pass, owned by the worker thread and released on the main
 * thread in backup_finish().
 *
 * Fields:
 *   app     — application context (status bar, in-flight flag).
 *   db_path — the source, copied so the worker never reads app->db.
 *   dir     — the resolved destination.
 *   keep    — retention bound for the prune.
 *   stamp   — the source identity to persist on success, or NULL; the
 *             worker fills it and the MAIN thread writes it, because the
 *             ini is main-thread state.
 *   message — what to say when it is over, or NULL to say nothing.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp    *app;
    gchar    *db_path;
    gchar    *dir;
    gint      keep;
    gchar    *stamp;
    gchar    *message;
} BackupJob;

/*
 * backup_job_free — release one job and everything it owns.
 *
 * Inputs:
 *   job — the job; must not be NULL.
 *
 * Output:
 *   none.
 */
static void
backup_job_free(BackupJob *job)
{
    g_free(job->db_path);
    g_free(job->dir);
    g_free(job->stamp);
    g_free(job->message);
    g_free(job);
}

/*
 * backup_finish — main-thread tail: persist the stamp, report, and release
 * the in-flight guard.
 *
 * Inputs:
 *   data — the BackupJob.
 *
 * Output:
 *   G_SOURCE_REMOVE always (a one-shot idle).
 */
static gboolean
backup_finish(gpointer data)
{
    BackupJob *job = data;           /* the pass that just ended            */
    job->app->backup_running = FALSE;
    if (job->stamp != NULL)
        on_app_config_set(BACKUP_STAMP_KEY, job->stamp);
    if (job->message != NULL)
        on_app_status(job->app, "%s", job->message);
    backup_job_free(job);
    return G_SOURCE_REMOVE;
}

/*
 * backup_thread — the pass itself, on its own SQLite connection.
 *
 * Order is deliberate and is the whole safety argument:
 *   1. copy the source into a fresh, uniquely named file;
 *   2. VERIFY it, and delete it again if it does not pass;
 *   3. hand the stamp back for the main thread to record;
 *   4. ONLY NOW prune the oldest beyond `keep`.
 *
 * Step 4 last is what keeps a run of failing backups from eating the good
 * history that is already on disk.  (The "has anything changed?" test is
 * made on the MAIN thread, in on_backup_start, because the ini it reads is
 * main-thread state.)
 *
 * Inputs:
 *   data — the BackupJob.
 *
 * Output:
 *   NULL always; the outcome rides the job back through backup_finish.
 */
static gpointer
backup_thread(gpointer data)
{
    BackupJob  *job = data;          /* the pass in flight                  */
    OnDatabase *db  = on_db_open(job->db_path);
    if (db == NULL) {
        job->message = g_strdup("Backup failed: cannot open the database");
        g_clear_pointer(&job->stamp, g_free);
        g_idle_add(backup_finish, job);
        return NULL;
    }

    /* A unique, sortable name.  The seconds-resolution stamp could collide
     * if two passes landed in the same second (a timer tick racing a manual
     * press), so a suffix is appended until the name is free — silently
     * overwriting an existing backup is exactly what this must never do.   */
    GDateTime *now  = g_date_time_new_now_local();
    gchar     *when = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    gchar *dest = NULL;              /* the file this pass writes           */
    for (gint n = 0; n < 100; n++) {
        g_free(dest);
        dest = n == 0
            ? g_strdup_printf("%s/%s%s%s", job->dir, BACKUP_PREFIX, when,
                              BACKUP_SUFFIX)
            : g_strdup_printf("%s/%s%s-%d%s", job->dir, BACKUP_PREFIX, when,
                              n, BACKUP_SUFFIX);
        if (!g_file_test(dest, G_FILE_TEST_EXISTS))
            break;
    }
    g_free(when);

    if (!on_db_backup_to(db, dest)) {
        job->message = g_strdup_printf("Backup to %s failed", dest);
        g_clear_pointer(&job->stamp, g_free);
        g_unlink(dest);              /* a half-written file is not history  */
        g_free(dest);
        on_db_close(db);
        g_idle_add(backup_finish, job);
        return NULL;
    }
    on_db_close(db);

    /* Verify before it counts as a backup at all.  An unverifiable copy is
     * worse than no copy: it would sit in the rotation looking like history
     * and displace a good one.                                             */
    gchar *detail = NULL;            /* sqlite's words, when it fails       */
    if (!on_db_verify_file(dest, &detail)) {
        g_unlink(dest);
        job->message = g_strdup_printf(
            "Backup failed verification and was discarded: %s",
            detail != NULL ? detail : "?");
        g_free(detail);
        g_clear_pointer(&job->stamp, g_free);
        g_free(dest);
        g_idle_add(backup_finish, job);
        return NULL;
    }
    g_free(detail);

    /* Prune — only now, and only our own files.                            */
    GPtrArray *have   = backup_list(job->dir);
    gint       pruned = 0;           /* how many were removed               */
    for (guint i = 0; have->len - i > (guint)job->keep; i++) {
        if (g_unlink(g_ptr_array_index(have, i)) == 0)
            pruned++;
        else
            g_warning("backup: could not remove %s",
                      (const gchar *)g_ptr_array_index(have, i));
    }
    guint kept = have->len;          /* before the prune                    */
    g_ptr_array_free(have, TRUE);

    gchar *base = g_path_get_basename(dest);
    job->message = pruned > 0
        ? g_strdup_printf("Backed up to %s (%u kept, %d pruned)", base,
                          kept - (guint)pruned, pruned)
        : g_strdup_printf("Backed up to %s (%u kept)", base, kept);
    g_free(base);
    g_free(dest);
    g_idle_add(backup_finish, job);
    return NULL;
}

void
on_backup_start(OnApp *app, const gchar *db_path)
{
    gchar *why = NULL;               /* why the feature is unusable         */
    if (!on_backup_ready(&why)) {
        on_app_status(app, "%s", why != NULL ? why : "Backups unavailable");
        g_free(why);
        return;
    }
    g_free(why);
    if (app->backup_running)
        return;                      /* a pass is already in flight         */

    gchar *dir   = on_backup_dir();  /* resolved, never NULL                */
    gchar *stamp = source_stamp(db_path, dir);
    gchar *last  = on_app_config_get(BACKUP_STAMP_KEY);
    gboolean same = stamp != NULL && last != NULL &&
                    g_strcmp0(stamp, last) == 0;
    g_free(last);
    if (same) {
        /* Nothing has changed: silent, because nothing happened.           */
        g_free(stamp);
        g_free(dir);
        return;
    }

    app->backup_running = TRUE;
    BackupJob *job = g_new0(BackupJob, 1);
    job->app     = app;
    job->db_path = g_strdup(db_path);
    job->dir     = dir;              /* handed over                         */
    job->keep    = backup_keep();
    job->stamp   = stamp;            /* handed over; NULL = do not record   */
    GThread *th = g_thread_new("notes-backup", backup_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * The periodic timer.
 *
 * The db path rides the timer in a copy of its own, because the app can be
 * pointed at a different database while the timer is armed (File → Open
 * Database…) — and re-arming is what carries the new path.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp *app;
    gchar *db_path;
} BackupTick;

/*
 * backup_tick_free — release a timer's payload when the source is removed.
 *
 * Inputs:
 *   data — the BackupTick.
 *
 * Output:
 *   none.
 */
static void
backup_tick_free(gpointer data)
{
    BackupTick *t = data;            /* the timer's payload                 */
    g_free(t->db_path);
    g_free(t);
}

/*
 * backup_tick — one timer firing: run a pass.
 *
 * Inputs:
 *   data — the BackupTick.
 *
 * Output:
 *   G_SOURCE_CONTINUE always; the timer is removed by re-arming, never by
 *   returning here.
 */
static gboolean
backup_tick(gpointer data)
{
    BackupTick *t = data;            /* the timer's payload                 */
    on_backup_start(t->app, t->db_path);
    return G_SOURCE_CONTINUE;
}

void
on_backup_auto_start(OnApp *app, const gchar *db_path)
{
    if (app->backup_timer != 0) {
        g_source_remove(app->backup_timer);
        app->backup_timer = 0;
    }
    gint minutes = backup_interval();
    if (!on_app_config_get_bool("backup_enabled", FALSE) || minutes <= 0)
        return;                      /* off, or manual-only                 */

    BackupTick *t = g_new0(BackupTick, 1);
    t->app     = app;
    t->db_path = g_strdup(db_path);
    app->backup_timer = g_timeout_add_seconds_full(
        G_PRIORITY_DEFAULT_IDLE, (guint)minutes * 60, backup_tick, t,
        backup_tick_free);
}
