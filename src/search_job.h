/* ===========================================================================
 * search_job.h — one cross-note search, run off the main thread
 *
 * The search window's worker, lifted out of the GTK window so it has no
 * toolkit in it: the window parses the query on the main thread (so a bad
 * pattern is reported at once — search_query.h), takes the scope (a tag) from
 * the library's selection, and starts a job; the job opens its OWN SQLite
 * connection on a worker (one connection per thread), matches every
 * candidate note's title and cached body text against the query, and hands
 * the hits back on the main thread.
 *
 * LIFETIME.  The job owns itself.  The handle on_search_job_start returns
 * is valid until the moment `done` is called or on_search_job_cancel is
 * called, whichever comes first; after either the caller drops its
 * pointer.  A cancelled job never calls `done`, so a window may cancel in
 * its close handler and be gone before the worker finishes.
 * =========================================================================== */

#ifndef ON_SEARCH_JOB_H
#define ON_SEARCH_JOB_H

#include <glib.h>

#include "search_query.h"

/* One matching note, formatted by the worker so the main thread only shows
 * it.  Owned by the job's hits array.                                       */
typedef struct {
    gint64  id;                      /* the note, for opening it             */
    gchar  *title;                   /* its title                            */
    gchar  *tags;                    /* its tags, "Salt, Stride"; "" for none */
    gint64  updated_at;              /* modified, UNIX time                  */
} on_search_hit_t;

typedef struct on_search_job_t on_search_job_t;

/* ---------------------------------------------------------------------------
 * on_search_done_func_t — a job finished.  Called on the main thread, exactly
 * once, unless the job was cancelled first.
 *
 * Inputs:
 *   error — why the search could not run, or NULL on success
 *   hits  — the matches, in the database's listing order; borrowed for the
 *           call — copy what is kept
 *   n     — how many
 *   data  — the caller's data
 * ------------------------------------------------------------------------- */
typedef void (*on_search_done_func_t)(const gchar *error,
                                      const on_search_hit_t *const *hits,
                                      size_t n, gpointer data);

/* ---------------------------------------------------------------------------
 * on_search_job_start() — begin a search.  Main thread.
 *
 * A search of every note covers the Trash too: deleted notes stay findable
 * until the Trash is emptied.
 *
 * Inputs:
 *   db_path  — the database file (app->db->path); the worker opens its own
 *              connection to it
 *   query    — the parsed query; OWNERSHIP IS TAKEN (it is immutable, so
 *              the worker matches with it freely)
 *   tag_id   — search the (visible) notes carrying this tag only; 0 for
 *              every note
 *   done     — completion callback; must not be NULL
 *   data     — passed to done
 *
 * Output: the job's handle (see LIFETIME above); never NULL.
 * ------------------------------------------------------------------------- */
on_search_job_t *on_search_job_start(const gchar *db_path, OnQuery *query,
                                     gint64 tag_id, on_search_done_func_t done,
                                     gpointer data);

/* ---------------------------------------------------------------------------
 * on_search_job_cancel() — abandon a job: `done` will not be called, and
 * the worker stops at its next note.  Main thread.
 *
 * Inputs:
 *   job — a live handle (not one whose `done` has been called)
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_search_job_cancel(on_search_job_t *job);

#endif /* ON_SEARCH_JOB_H */
