/* ===========================================================================
 * app_lifecycle.c — database open policy, service start and shutdown
 * (see app_lifecycle.h)
 * =========================================================================== */

#include "app_lifecycle.h"
#include "backup.h"
#include "identity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* How often, and how far apart, a busy app lock is tried again before
 * on_app_lock_take answers "busy": long enough for a copy that is quitting
 * to let go of it.                                                         */
#define LOCK_TRIES    10
#define LOCK_RETRY_US (100 * 1000)

/* The app lock's descriptor while this process holds it, or -1.           */
static int app_lock_fd = -1;

/*
 * lock_try — try to take the app lock once, without waiting.
 *
 * Output: the locked descriptor (closing it releases the lock), or -1 with
 *         errno set: EWOULDBLOCK when another process holds it, anything
 *         else when the lock could not be tried at all.
 */
static int
lock_try(void)
{
    char *path = on_paths_data_file(ON_LOCK_FILENAME);
    int   fd   = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(path);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

/*
 * on_app_lock_take — see app_lifecycle.h.
 */
on_app_lock_result_t
on_app_lock_take(void)
{
    if (app_lock_fd >= 0) {
        return on_app_lock_held;
    }
    int lock = -1;
    for (int i = 0; i < LOCK_TRIES && lock < 0; i++) {
        lock = lock_try();
        if (lock < 0 && errno != EWOULDBLOCK) {
            fprintf(stderr, "notes: cannot take the app lock: %s\n",
                    strerror(errno));
            return on_app_lock_failed;
        }
        if (lock < 0) {
            usleep(LOCK_RETRY_US);
        }
    }
    if (lock < 0) {
        return on_app_lock_busy;
    }
    app_lock_fd = lock;
    return on_app_lock_held;
}

/*
 * on_app_db_path — see app_lifecycle.h.
 */
gchar *
on_app_db_path(OnApp *app)
{
    return app->db_dir != NULL
           ? g_build_filename(app->db_dir, ON_DB_FILENAME, NULL)
           : on_db_default_path();
}

/*
 * on_app_open_db — see app_lifecycle.h.
 */
gboolean
on_app_open_db(OnApp *app)
{
    gchar *db_path = on_app_db_path(app);
    app->db = on_db_open(db_path);
    if (app->db == NULL) {
        fprintf(stderr, "notes: could not open the notes database at %s\n"
                        "(if another instance is still shutting down, try "
                        "again in a few seconds)\n", db_path);
    }
    g_free(db_path);
    return app->db != NULL;
}

/*
 * on_app_start_services — see app_lifecycle.h.
 */
void
on_app_start_services(OnApp *app)
{
    on_app_status(app, "DB at %s loaded", app->db->path);
    on_app_db_health_start(app);
    on_backup_auto_start(app, app->db->path);
}

/*
 * on_app_switch_db — see app_lifecycle.h.
 */
on_switch_result_t
on_app_switch_db(OnApp *app, const gchar *path, gboolean set_default)
{
    if (app->flush_editors != NULL && !app->flush_editors(app)) {
        return ON_SWITCH_UNSAVED;
    }
    gchar *old_path = g_strdup(app->db->path);
    on_db_close(app->db);
    app->db = on_db_open(path);
    if (app->db == NULL) {
        app->db = on_db_open(old_path);      /* back to where we were        */
        g_free(old_path);
        return ON_SWITCH_FAILED;
    }
    g_free(old_path);

    if (set_default) {               /* (Session Only persists nothing)      */
        gchar *new_dir = g_path_get_dirname(path);
        on_app_config_set("db_dir", new_dir);
        g_free(app->db_dir);
        app->db_dir = new_dir;
    }

    /* The backup timer carries the database path, so it is re-armed onto
     * the file that is now open.                                           */
    on_backup_auto_start(app, app->db->path);
    if (app->notify_notes_changed != NULL) {
        app->notify_notes_changed(app);
    }
    on_app_status(app, "DB at %s loaded", app->db->path);
    on_app_db_health_start(app);     /* every opened database is checked     */
    return ON_SWITCH_DONE;
}

/*
 * on_app_shutdown — see app_lifecycle.h.
 */
gboolean
on_app_shutdown(OnApp *app)
{
    /* Editors first: one that cannot save stops the shutdown with nothing
     * else torn down.                                                      */
    if (app->flush_editors != NULL && !app->flush_editors(app)) {
        return FALSE;
    }
    on_timer_remove(app->backup_timer);
    app->backup_timer = 0;
    on_db_close(app->db);            /* NULL-safe                            */
    app->db = NULL;
    if (app_lock_fd >= 0) {          /* last: the database is closed         */
        close(app_lock_fd);
        app_lock_fd = -1;
    }
    return TRUE;
}
