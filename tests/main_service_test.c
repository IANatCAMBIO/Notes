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
    app.db = on_db_open(NULL);
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
    OnDatabase *second = on_db_open(NULL);
    on_db_health_check_async(second, health_landed, NULL);
    on_db_close(second);
    check_pump(NULL, 0.5);
    check(!health_done, "a pass abandoned by on_db_close never reports");

    /* A backup pass: worker, verify, hand-back, status line.               */
    gchar *backups = g_build_filename(data_dir, "backup-out", NULL);
    g_mkdir_with_parents(backups, 0700);
    on_app_config_set("backup_enabled", "1");
    on_app_config_set("backup_dir", backups);
    on_backup_start(&app, app.db->path);
    check(app.backup_running, "a backup pass is in flight");
    bool finished = false;
    for (int i = 0; i < 500 && !finished; i++) {
        check_pump(NULL, 0.02);
        finished = !app.backup_running;
    }
    check(finished, "the backup pass finishes and releases its guard");
    check(strncmp(last_status, "Backed up to notes-", 19) == 0,
          "it reports through on_app_status");
    GDir *d = g_dir_open(backups, 0, NULL);
    const gchar *name = d != NULL ? g_dir_read_name(d) : NULL;
    check(name != NULL && g_str_has_prefix(name, "notes-") && g_str_has_suffix(name, ".db"),
          "the backup file exists in the chosen folder");
    if (d != NULL) {
        g_dir_close(d);
    }
    gchar *stamp = on_app_config_get("backup_source_stamp");
    check(stamp != NULL, "the source stamp was persisted on the main thread");
    g_free(stamp);

    /* The periodic timer arms and disarms through the same service.        */
    on_app_config_set("backup_interval_min", "60");
    on_backup_auto_start(&app, app.db->path);
    check(app.backup_timer != 0, "the backup timer is armed while backups are on");
    on_app_config_set("backup_enabled", "0");
    on_backup_auto_start(&app, app.db->path);
    check(app.backup_timer == 0, "and removed when they are switched off");

    g_free(backups);
    on_db_close(app.db);
}

/* What the paced-backup worker reports back. */
typedef struct {
    OnDatabase *db;                  /* its own connection                   */
    gchar      *dest;                /* the backup file                      */
    gboolean    ok;                  /* on_db_backup_to's verdict            */
    gint        done;                /* set (atomically) when it returns     */
} paced_backup_t;

/*
 * paced_backup_worker — run one on_db_backup_to on a worker thread, as the
 * backup service does.
 *
 * Inputs:
 *   data — the paced_backup_t
 *
 * Output: NULL.
 */
static gpointer
paced_backup_worker(gpointer data)
{
    paced_backup_t *job = data;
    job->ok = on_db_backup_to(job->db, job->dest);
    g_atomic_int_set(&job->done, 1);
    return NULL;
}

/*
 * test_paced_backup — a backup of a database many steps long completes
 * while another connection keeps writing (each write sends the copy back
 * to the start), no write waits long for it, and the copy is whole.
 *
 * Inputs:
 *   data_dir — the test data directory
 */
static void
test_paced_backup(const char *data_dir)
{
    OnDatabase *writer = on_db_open(NULL);
    gint64      note   = on_db_note_create(writer, 0);
    gsize       big    = 8 * 1024 * 1024;     /* ~2000 pages: four steps    */
    guint8     *bytes  = g_malloc(big);
    for (gsize i = 0; i < big; i++) bytes[i] = (guint8)(i * 2654435761u >> 24);
    check(on_db_note_save(writer, note, "Big", bytes, big, NULL), "a large note is stored");

    paced_backup_t job = { on_db_open(NULL), g_build_filename(data_dir, "paced.db", NULL), FALSE, 0 };
    GThread *thread = g_thread_new("paced-backup", paced_backup_worker, &job);
    gint64   slowest = 0, writes = 0;
    while (!g_atomic_int_get(&job.done)) {
        gint64 t0 = g_get_monotonic_time();
        on_db_note_set_updated_at(writer, note, writes++);
        slowest = MAX(slowest, g_get_monotonic_time() - t0);
    }
    g_thread_join(thread);
    check(job.ok, "the backup completes while another connection keeps writing");
    check(slowest < G_USEC_PER_SEC, "no write waits a second for it");

    OnDatabase *copy = on_db_open(job.dest);
    guint8     *blob = NULL;
    gsize       len  = 0;
    check(copy != NULL && on_db_note_load(copy, note, &blob, &len) &&
          len == big && memcmp(blob, bytes, big) == 0,
          "the copy holds the large note whole");
    g_free(blob);
    on_db_close(copy);
    on_db_close(job.db);
    on_db_close(writer);
    g_free(job.dest);
    g_free(bytes);
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
    test_paced_backup(data_dir);
    return check_result();
}
