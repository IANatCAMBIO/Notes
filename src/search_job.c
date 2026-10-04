/* ===========================================================================
 * search_job.c — one cross-note search on a worker (see search_job.h)
 *
 * Note bodies come from the notes.body_text cache column, so a search
 * never decodes an image.  Rows saved before the column existed have a
 * NULL cache; those fall back to extracting the text from the blob
 * (on_note_text_cached), which writes the cache back.
 * =========================================================================== */

#include "search_job.h"
#include "db.h"
#include "main_service.h"
#include "serialize.h"               /* on_note_text_cached                 */

#include <stdatomic.h>

/* Above this many candidate notes, one bulk body_text query beats reading
 * the rows one at a time; at or below it the bulk fetch would pull the whole
 * column to inspect a handful of notes (a folder- or tag-scoped search).    */
#define SEARCH_BULK_TEXT_MIN 64

/* strftime-style format of a hit's modified time ("Jun 3, 2026 14:05").    */
#define SEARCH_TIME_FORMAT "%b %e, %Y %H:%M"

/* ---------------------------------------------------------------------------
 * OnSearchJob — everything one search needs, snapshotted on the main
 * thread so the worker never touches the UI or the shared connection.
 *
 *   cancelled — set from the main thread; read by the worker between notes
 *               and by the main-thread hand-back.
 *   db_path   — database file to open privately (owned).
 *   query     — the parsed query (owned).
 *   scoped, scope, scope_id — the candidate notes.
 *   done, data — where the result goes.
 *   hits      — OnSearchHit *, filled by the worker.
 *   error     — the worker's failure message, or NULL (owned).
 * ------------------------------------------------------------------------- */
struct OnSearchJob {
    atomic_int        cancelled;
    gchar            *db_path;
    OnQuery          *query;
    gboolean          scoped;
    OnSearchScope     scope;
    gint64            scope_id;
    OnSearchDoneFunc  done;
    gpointer          data;
    GPtrArray        *hits;
    gchar            *error;
};

/*
 * search_hit_free — GDestroyNotify for the hits array.
 *
 * Inputs:
 *   data — an OnSearchHit
 *
 * Output: none.
 */
static void
search_hit_free(gpointer data)
{
    OnSearchHit *h = data;
    g_free(h->path);
    g_free(h->when);
    g_free(h);
}

/*
 * search_finish — the main-thread end of a job (on_main_post): deliver the
 * result unless the job was cancelled, then free the job.  This is the
 * only place a job is freed, so a cancelled job is still released.
 *
 * Inputs:
 *   data — the OnSearchJob
 *
 * Output: none.
 */
static void
search_finish(void *data)
{
    OnSearchJob *job = data;
    if (!atomic_load(&job->cancelled)) {
        job->done(job->error, job->hits, job->data);
    }
    g_ptr_array_free(job->hits, TRUE);
    g_free(job->db_path);
    on_query_free(job->query);
    g_free(job->error);
    g_free(job);
}

/*
 * search_worker — the search itself (on_worker_run): a private connection,
 * the candidate notes, the matching.  Finishes by posting search_finish.
 *
 * Inputs:
 *   data — the OnSearchJob
 *
 * Output: none.
 */
static void
search_worker(void *data)
{
    OnSearchJob *job = data;
    OnDatabase  *db  = on_db_open(job->db_path);
    if (db == NULL) {
        job->error = g_strdup("Could not open the database.");
        on_main_post(search_finish, job);
        return;
    }

    GList *notes;                    /* OnNoteMeta * candidates              */
    if (!job->scoped) {
        notes = on_db_note_list_all(db, TRUE);
    } else if (job->scope == ON_SCOPE_TAG) {
        notes = on_db_notes_by_tag(db, job->scope_id);
    } else {
        notes = on_db_note_list(db, job->scope_id);
    }

    /* Folder-id -> path strings, all fetched in one query.                 */
    GHashTable *paths = on_db_folder_path_map(db);

    /* Note bodies: ONE query for the whole column beats a SELECT per
     * candidate when searching everything; a scoped search with a handful
     * of candidates reads just those.                                      */
    GHashTable *bodies = g_list_length(notes) > SEARCH_BULK_TEXT_MIN
                         ? on_db_note_text_map(db, 0) : NULL;

    for (GList *l = notes; l != NULL; l = l->next) {
        if (atomic_load(&job->cancelled)) {
            break;
        }
        OnNoteMeta  *m    = l->data;
        const gchar *body = bodies != NULL
                            ? g_hash_table_lookup(bodies, &m->id) : NULL;
        gchar *extracted = NULL;     /* small scope, or an uncached row      */
        if (body == NULL) {
            extracted = on_note_text_cached(db, m->id);
            body      = extracted;
        }
        gboolean match = on_query_matches(job->query, m->title, body);
        g_free(extracted);
        if (!match) {
            continue;
        }

        const gchar *folder = g_hash_table_lookup(paths, &m->folder_id);
        GDateTime   *dt     = g_date_time_new_from_unix_local(m->updated_at);
        OnSearchHit *h      = g_new0(OnSearchHit, 1);
        h->id   = m->id;
        h->path = g_strdup_printf("%s/%s", folder != NULL ? folder : "",
                                  m->title);
        h->when = g_date_time_format(dt, SEARCH_TIME_FORMAT);
        g_date_time_unref(dt);
        g_ptr_array_add(job->hits, h);
    }
    if (bodies != NULL) {
        g_hash_table_destroy(bodies);
    }
    g_hash_table_destroy(paths);
    on_db_note_list_free(notes);
    on_db_close(db);

    on_main_post(search_finish, job);
}

OnSearchJob *
on_search_job_start(const gchar *db_path, OnQuery *query, gboolean scoped,
                    OnSearchScope scope, gint64 scope_id,
                    OnSearchDoneFunc done, gpointer data)
{
    OnSearchJob *job = g_new0(OnSearchJob, 1);
    atomic_init(&job->cancelled, 0);
    job->db_path  = g_strdup(db_path);
    job->query    = query;
    job->scoped   = scoped;
    job->scope    = scope;
    job->scope_id = scope_id;
    job->done     = done;
    job->data     = data;
    job->hits     = g_ptr_array_new_with_free_func(search_hit_free);
    on_worker_run(search_worker, job);
    return job;
}

void
on_search_job_cancel(OnSearchJob *job)
{
    atomic_store(&job->cancelled, 1);
}
