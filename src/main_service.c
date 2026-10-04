/* ===========================================================================
 * main_service.c — main-thread posts, timers and workers on GCD
 * (see main_service.h)
 * =========================================================================== */

#include "main_service.h"

#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * main_timer_t — one live timer.  The list is main-thread state: timers are
 * added, fired and removed on the main thread only, so it needs no lock.
 *
 * The dispatch source does NOT carry a pointer to this record: its context
 * is the timer's id, and the handler looks the record up.  A handler that
 * was already queued when the timer was removed then finds nothing and
 * returns, instead of touching freed memory.
 * ------------------------------------------------------------------------- */
typedef struct main_timer_t {
    OnTimerId         id;
    dispatch_source_t source;        /* owned: cancelled + released on remove */
    bool              repeating;
    OnMainFunc        fn;
    void             *data;
    OnMainFunc        destroy;       /* releases data, or NULL               */
    struct main_timer_t  *next;
} main_timer_t;

static main_timer_t  *timers     = NULL; /* every live timer                     */
static OnTimerId  last_timer = 0;    /* the id handed out last               */

void
on_main_post(OnMainFunc fn, void *data)
{
    dispatch_async_f(dispatch_get_main_queue(), data, fn);
}

/*
 * timer_find — the live timer with this id.
 *
 * Inputs:
 *   id — a timer id
 *
 * Output: the address of the list link that points at the record (so the
 *         caller can unlink it), or NULL when no such timer is live.
 */
static main_timer_t **
timer_find(OnTimerId id)
{
    for (main_timer_t **link = &timers; *link != NULL; link = &(*link)->next) {
        if ((*link)->id == id) {
            return link;
        }
    }
    return NULL;
}

/*
 * timer_fire — the dispatch source's event handler, on the main queue.
 * A one-shot is removed after its callback; the callback may already have
 * removed the timer itself, which the second lookup notices.
 *
 * Inputs:
 *   context — the timer's id, as set with dispatch_set_context
 *
 * Output: none.
 */
static void
timer_fire(void *context)
{
    OnTimerId  id   = (OnTimerId)(uintptr_t)context;
    main_timer_t **link = timer_find(id);
    if (link == NULL) {
        return;
    }
    bool repeating = (*link)->repeating;
    (*link)->fn((*link)->data);
    if (!repeating) {
        on_timer_remove(id);
    }
}

OnTimerId
on_timer_add(double seconds, bool repeating, OnMainFunc fn, void *data,
             OnMainFunc destroy)
{
    main_timer_t *t = calloc(1, sizeof *t);
    if (++last_timer == 0) {         /* wrapped: 0 means "no timer"          */
        last_timer = 1;
    }
    t->id        = last_timer;
    t->repeating = repeating;
    t->fn        = fn;
    t->data      = data;
    t->destroy   = destroy;
    t->source    = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                          dispatch_get_main_queue());
    t->next      = timers;
    timers       = t;

    uint64_t nanos = (uint64_t)(seconds * (double)NSEC_PER_SEC);
    dispatch_set_context(t->source, (void *)(uintptr_t)t->id);
    dispatch_source_set_event_handler_f(t->source, timer_fire);
    /* A tenth of the period as leeway: nothing here needs a precise tick,
     * and leeway lets the system coalesce wake-ups.                         */
    dispatch_source_set_timer(t->source,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)nanos),
                              repeating ? nanos : DISPATCH_TIME_FOREVER,
                              nanos / 10);
    dispatch_resume(t->source);
    return t->id;
}

void
on_timer_remove(OnTimerId id)
{
    main_timer_t **link = timer_find(id);
    if (link == NULL) {
        return;
    }
    main_timer_t *t = *link;
    *link = t->next;
    dispatch_source_cancel(t->source);
    dispatch_release(t->source);
    if (t->destroy != NULL) {
        t->destroy(t->data);
    }
    free(t);
}

/*
 * worker_queue_create — dispatch_once initialiser for the worker queue:
 * concurrent (a slow backup must not hold up a search), utility priority
 * (never competes with the UI), one autorelease pool per job.
 *
 * Inputs:
 *   context — receives the queue (a dispatch_queue_t *)
 *
 * Output: none.
 */
static void
worker_queue_create(void *context)
{
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        dispatch_queue_attr_make_with_autorelease_frequency(
            DISPATCH_QUEUE_CONCURRENT,
            DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM),
        QOS_CLASS_UTILITY, 0);
    *(dispatch_queue_t *)context =
        dispatch_queue_create("notes.core.worker", attr);
}

void
on_worker_run(OnMainFunc fn, void *data)
{
    static dispatch_queue_t queue;   /* created once, lives for the process  */
    static dispatch_once_t  once;
    dispatch_once_f(&once, &queue, worker_queue_create);
    dispatch_async_f(queue, data, fn);
}
