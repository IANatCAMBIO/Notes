/* ===========================================================================
 * main_service_test.c — main-thread posts, timers and workers
 *
 * Under the GTK build these hand-backs rode a GLib main loop; here nothing
 * runs one, so each case below would hang for ever on g_idle_add.  The test
 * pumps the Cocoa-side run loop (check_pump), as the app does.
 * =========================================================================== */

#include "check.h"

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

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    check_data_dir();
    test_posts_and_workers();
    test_timers();
    return check_result();
}
