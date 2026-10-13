/* ===========================================================================
 * main_service.h — the main thread, timers and worker threads of the core
 *
 * The GTK build leaned on a running GLib main loop for three things:
 * handing a result from a worker thread back to the main thread
 * (g_idle_add), timers (g_timeout_add) and starting worker threads
 * (g_thread_new).  Under NSApplication nothing pumps a GLib main loop, so
 * a g_idle_add would simply never fire — no crash, just a health check
 * that says "Checking…" for ever and a backup that never finishes.  This
 * module is the replacement, on Grand Central Dispatch, and it is the ONLY
 * scheduling the core uses.
 *
 * The main queue is drained by the Cocoa run loop in the app, and by
 * CFRunLoopRun / dispatch_main in a test.  A process that never runs
 * either (the development seeder) must not rely on on_main_post or a
 * timer: nothing it calls does.
 *
 * Threading rule that goes with it: one SQLite connection per thread.  A
 * worker opens its own connection (on_db_open_existing: never creates the
 * file) and closes it before it posts its result back; the OnDatabase in
 * OnApp belongs to the main thread alone.
 * =========================================================================== */

#ifndef ON_MAIN_SERVICE_H
#define ON_MAIN_SERVICE_H

#include <stdbool.h>

/* A unit of work: `data` is whatever the caller handed over with it.       */
typedef void (*on_main_func_t)(void *data);

/* A timer's handle; 0 is "no timer".                                       */
typedef unsigned int on_timer_id_t;

/* ---------------------------------------------------------------------------
 * on_main_post() — run fn(data) on the main thread, later: the call returns
 * at once and fn runs on a following pass of the main run loop, in the
 * order posted.  Safe from any thread; this is how a worker hands its
 * result back.
 *
 * Inputs:
 *   fn   — the work; must not be NULL
 *   data — passed to fn, which owns it from then on
 *
 * Output: none.  There is no way to cancel a post: a job that may be
 *         abandoned carries its own cancelled flag and frees itself in fn.
 * ------------------------------------------------------------------------- */
void on_main_post(on_main_func_t fn, void *data);

/* ---------------------------------------------------------------------------
 * on_timer_add() — call fn(data) on the main thread after `seconds`, once
 * or repeatedly.  Main thread only.
 *
 * A one-shot timer removes itself after it has fired; a repeating one runs
 * until on_timer_remove().  Either way `destroy` is called exactly once
 * with `data`, when the timer goes away, so the timer can own its payload.
 * fn may remove its own timer.
 *
 * Inputs:
 *   seconds   — the delay, and the period when repeating; must be > 0
 *   repeating — false for a one-shot
 *   fn        — the work; must not be NULL
 *   data      — passed to fn and to destroy
 *   destroy   — releases data, or NULL when there is nothing to release
 *
 * Output: the timer's id (never 0).
 * ------------------------------------------------------------------------- */
on_timer_id_t on_timer_add(double seconds, bool repeating, on_main_func_t fn,
                       void *data, on_main_func_t destroy);

/* ---------------------------------------------------------------------------
 * on_timer_remove() — stop a timer and release its payload.  Main thread
 * only.  After it returns fn is never called again.
 *
 * Inputs:
 *   id — a timer from on_timer_add; 0 or an id that has already gone
 *        (a one-shot that fired) is ignored
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_timer_remove(on_timer_id_t id);

/* ---------------------------------------------------------------------------
 * on_worker_run() — run fn(data) on a background thread.  Safe from any
 * thread.  Each job gets its own autorelease pool, so a worker that calls
 * into ImageIO or Foundation does not accumulate temporaries.
 *
 * The worker must not touch the main thread's OnDatabase, the config, or
 * any UI object; it finishes by calling on_main_post with its result.
 *
 * Inputs:
 *   fn   — the work; must not be NULL
 *   data — passed to fn, which owns it from then on
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_worker_run(on_main_func_t fn, void *data);

#endif /* ON_MAIN_SERVICE_H */
