/* ===========================================================================
 * app_lifecycle.c — database open policy, service start and shutdown
 * (see app_lifecycle.h)
 * =========================================================================== */

#include "app_lifecycle.h"
#include "backup.h"

gchar *
on_app_db_path(OnApp *app)
{
    return app->db_dir != NULL
           ? g_build_filename(app->db_dir, ON_DB_FILENAME, NULL)
           : on_db_default_path();
}

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

OnIpcServerResult
on_app_start_services(OnApp *app)
{
    /* The socket first: if another instance already serves this data
     * directory, this one must do nothing further.                        */
    OnIpcServerResult served = on_ipc_server_start(app);
    if (served == ON_IPC_SERVER_BUSY) {
        return served;
    }
    on_app_status(app, "DB at %s loaded", app->db->path);
    on_app_db_health_start(app);
    on_backup_auto_start(app, app->db->path);
    on_ipc_run_pending(app);
    return served;
}

gboolean
on_app_switch_db(OnApp *app, const gchar *path, gboolean set_default)
{
    if (app->flush_editors != NULL) {
        app->flush_editors(app);
    }
    gchar *old_path = g_strdup(app->db->path);
    on_db_close(app->db);
    app->db = on_db_open(path);
    if (app->db == NULL) {
        app->db = on_db_open(old_path);      /* back to where we were        */
        g_free(old_path);
        return FALSE;
    }
    g_free(old_path);

    if (set_default) {
        gchar *new_dir = g_path_get_dirname(path);
        on_app_config_set("db_dir", new_dir);
        g_free(app->db_dir);
        app->db_dir       = new_dir;
        app->db_transient = FALSE;
    } else {
        app->db_transient = TRUE;    /* session only: persist nothing        */
    }

    /* The backup timer carries the database path, so it is re-armed onto
     * the file that is now open.                                           */
    on_backup_auto_start(app, app->db->path);
    if (app->notify_notes_changed != NULL) {
        app->notify_notes_changed(app);
    }
    on_app_status(app, "DB at %s loaded", app->db->path);
    on_app_db_health_start(app);     /* every opened database is checked     */
    return TRUE;
}

void
on_app_shutdown(OnApp *app)
{
    on_ipc_server_stop();
    if (app->flush_editors != NULL) {
        app->flush_editors(app);
    }
    on_timer_remove(app->backup_timer);
    app->backup_timer = 0;
    on_db_close(app->db);            /* NULL-safe                            */
    app->db = NULL;
}
