/* ===========================================================================
 * main_service_test.c — main-thread posts, timers and workers, and the two
 * core jobs that depend on them (the database health check and a backup
 * pass)
 *
 * Under the GTK build these hand-backs rode a GLib main loop; here nothing
 * runs one, so each case below would hang for ever on g_idle_add.  The test
 * pumps the Cocoa-side run loop (check_pump), as the app does.
 * =========================================================================== */

#include "check.h"

#include "app_core.h"
#include "backup.h"
#include "db.h"
#include "main_service.h"

#include <pthread.h>

static int  order[3];                /* the sequence posts ran in            */
static int  order_n;
static bool post_done;               /* the last post has run                */

/*
 * record_post — an on_main_post callback: note which post this is.
 *
 * Inputs:
 *   data — the post's number, cast to a pointer
 */
static void
record_post(void *data)
{
    order[order_n++] = (int)(intptr_t)data;
    post_done = order_n == 3;
}

static bool worker_off_main;         /* the worker ran off the main thread   */
static bool worker_back_on_main;     /* its hand-back ran on it              */
static bool worker_done;

/*
 * worker_reply — the worker's hand-back, on the main thread.
 *
 * Inputs:
 *   data — unused
 */
static void
worker_reply(void *data)
{
    (void)data;
    worker_back_on_main = pthread_main_np() != 0;
    worker_done = true;
}

/*
 * worker_body — the on_worker_run job.
 *
 * Inputs:
 *   data — unused
 */
static void
worker_body(void *data)
{
    worker_off_main = pthread_main_np() == 0;
    on_main_post(worker_reply, data);
}

/* One timer under test: how often it fired and whether its payload was
 * released.                                                                 */
typedef struct {
    int       fires;
    int       destroyed;
    on_timer_id_t id;                    /* for a callback that removes itself   */
    bool      remove_self;
} timer_probe_t;

/*
 * probe_fire — a timer callback: count, and remove the timer from inside
 * its own callback when asked to.
 *
 * Inputs:
 *   data — the timer_probe_t
 */
static void
probe_fire(void *data)
{
    timer_probe_t *p = data;
    p->fires++;
    if (p->remove_self) {
        on_timer_remove(p->id);
    }
}

/*
 * probe_destroy — a timer's destroy callback: count.
 *
 * Inputs:
 *   data — the timer_probe_t
 */
static void
probe_destroy(void *data)
{
    ((timer_probe_t *)data)->destroyed++;
}

/*
 * test_posts_and_workers — ordering, deferral, and which thread runs what.
 */
static void
test_posts_and_workers(void)
{
    for (intptr_t i = 1; i <= 3; i++) {
        on_main_post(record_post, (void *)i);
    }
    check(order_n == 0, "a post does not run inside on_main_post");
    check(check_pump(&post_done, 2.0), "posts run once the run loop turns");
    check(order[0] == 1 && order[1] == 2 && order[2] == 3, "posts run in the order posted");

    on_worker_run(worker_body, NULL);
    check(check_pump(&worker_done, 2.0), "a worker's hand-back arrives");
    check(worker_off_main, "the worker ran off the main thread");
    check(worker_back_on_main, "its hand-back ran on the main thread");
}

/*
 * test_timers — one-shot, repeating, removal before firing and from inside
 * the callback; the payload is released exactly once in every case.
 */
static void
test_timers(void)
{
    timer_probe_t once = { 0 };
    on_timer_id_t id = on_timer_add(0.05, false, probe_fire, &once, probe_destroy);
    check(id != 0, "a timer has a non-zero id");
    check_pump(NULL, 0.4);
    check(once.fires == 1, "a one-shot timer fires exactly once");
    check(once.destroyed == 1, "and releases its payload after firing");
    on_timer_remove(id);
    check(once.destroyed == 1, "removing a timer that has gone is a no-op");

    timer_probe_t repeat = { 0 };
    id = on_timer_add(0.05, true, probe_fire, &repeat, probe_destroy);
    check_pump(NULL, 0.4);
    check(repeat.fires >= 3, "a repeating timer keeps firing");
    on_timer_remove(id);
    int at_removal = repeat.fires;
    check(repeat.destroyed == 1, "removal releases the payload");
    check_pump(NULL, 0.2);
    check(repeat.fires == at_removal, "a removed timer never fires again");

    timer_probe_t never = { 0 };
    id = on_timer_add(0.05, false, probe_fire, &never, probe_destroy);
    on_timer_remove(id);
    check_pump(NULL, 0.2);
    check(never.fires == 0 && never.destroyed == 1,
          "a timer removed before it is due never fires and is released once");

    timer_probe_t self = { .remove_self = true };
    self.id = on_timer_add(0.05, true, probe_fire, &self, probe_destroy);
    check_pump(NULL, 0.3);
    check(self.fires == 1 && self.destroyed == 1,
          "a timer may remove itself from its own callback");

    on_timer_remove(0);
    check(true, "removing timer 0 is a no-op");
}

static bool health_done;             /* the verdict landed                   */
static bool health_on_main;

/*
 * health_landed — on_db_health_check_async's completion.
 *
 * Inputs:
 *   db   — the connection (unused)
 *   data — unused
 */
static void
health_landed(OnDatabase *db, gpointer data)
{
    (void)db;
    (void)data;
    health_on_main = pthread_main_np() != 0;
    health_done = true;
}

static char last_status[256];        /* the last on_app_status message       */

/*
 * status_hook — OnApp.notify_status for this test.
 *
 * Inputs:
 *   app     — the context (unused)
 *   message — the status line
 */
static void
status_hook(OnApp *app, const gchar *message)
{
    (void)app;
    snprintf(last_status, sizeof last_status, "%s", message);
}

/*
 * test_core_jobs — the two core jobs that hand back through this service:
 * the async integrity check and a rotating-backup pass.
 *
 * Inputs:
 *   data_dir — this test's NOTES_DATA_DIR
 */
static void
test_core_jobs(const char *data_dir)
{
    on_app_config_init();
    OnApp app = { 0 };
    app.notify_status = status_hook;
    app.db = on_db_open(NULL, NULL);
    check(app.db != NULL && g_str_has_prefix(app.db->path, data_dir),
          "the default database opens inside the test data directory");
    on_db_note_create(app.db, 0);

    on_db_health_check_async(app.db, health_landed, NULL);
    const OnDbHealth *h = on_db_health(app.db);
    check(h != NULL && h->pending, "the health check reports pending while it runs");
    check(check_pump(&health_done, 10.0), "the health verdict lands (it never did on g_idle_add)");
    h = on_db_health(app.db);
    check(health_on_main && h != NULL && !h->pending && h->ok,
          "the verdict is recorded on the main thread, and is clean");

    /* A pass abandoned by closing the connection must not call back.       */
    health_done = false;
    OnDatabase *second = on_db_open(NULL, NULL);
    on_db_health_check_async(second, health_landed, NULL);
    on_db_close(second);
    check_pump(NULL, 0.5);
    check(!health_done, "a pass abandoned by on_db_close never reports");

    /* Back Up Now: the shared module's backup on a worker, its own
     * connection, the hand-back and a status line.  The policy is in
     * shared_settings.                                                     */
    gchar *backups = g_build_filename(data_dir, "backup-out", NULL);
    g_mkdir_with_parents(backups, 0700);
    gchar *why = NULL;
    check(on_backup_set_dir(&app, backups, &why) && why == NULL,
          "a writable folder becomes the backup destination");
    gchar *dir = on_backup_dir(&app, NULL);
    check(g_strcmp0(dir, backups) == 0, "and is where backups go");
    g_free(dir);
    check(!on_backup_set_dir(&app, "relative/folder", &why) && why != NULL,
          "a relative folder is refused, with a reason");
    g_free(why);
    on_backup_set_setting(&app, SHARED_SETTING_BACKUP_KEEP, 100000);
    check(on_backup_setting(&app, SHARED_SETTING_BACKUP_KEEP) == SHARED_BACKUP_KEEP_MAX,
          "backup_keep reads back held to the module's range");
    on_backup_set_setting(&app, SHARED_SETTING_BACKUP_KEEP, 2);

    gchar *wal = g_strconcat(app.db->path, "-wal", NULL);
    for (int pass = 0; pass < 3; pass++) {
        on_backup_start(&app);
        check(app.backup_running, "a backup pass is in flight");
        bool finished = false;
        for (int i = 0; i < 500 && !finished; i++) {
            check_pump(NULL, 0.02);
            finished = !app.backup_running;
        }
        check(finished && strncmp(last_status, "Backed up to " SHARED_DB_BACKUP_PREFIX,
                                  strlen("Backed up to " SHARED_DB_BACKUP_PREFIX)) == 0,
              "the pass finishes, releases its guard and reports through on_app_status");
    }
    GDir *d = g_dir_open(backups, 0, NULL);
    int   n = 0;
    for (const gchar *name; d != NULL && (name = g_dir_read_name(d)) != NULL; ) {
        n += g_str_has_prefix(name, SHARED_DB_BACKUP_PREFIX) && g_str_has_suffix(name, ".db");
    }
    if (d != NULL) g_dir_close(d);
    check(n == 2, "three passes with backup_keep 2 leave two backups");
    check(g_file_test(app.db->path, G_FILE_TEST_EXISTS) && g_file_test(wal, G_FILE_TEST_EXISTS),
          "the live database and its -wal are untouched");
    g_free(wal);
    check(on_backup_setting(&app, SHARED_SETTING_BACKUP_LAST_AT) > 0,
          "the module recorded the time of the last backup");

    /* The periodic poll arms through the same service.                     */
    on_backup_auto_start(&app);
    check(app.backup_timer != 0, "the backup poll timer is armed");
    on_timer_remove(app.backup_timer);
    app.backup_timer = 0;

    g_free(backups);
    on_db_close(app.db);
}

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    const char *data_dir = check_data_dir();
    test_posts_and_workers();
    test_timers();
    test_core_jobs(data_dir);
    return check_result();
}
