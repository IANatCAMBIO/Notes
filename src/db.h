/* ===========================================================================
 * db.h — SQLite persistence layer for Notes
 *
 * All notes and tags live in the ONE SQLite file Notes shares with
 * the Tasks app (../shared_data, shared_db.h): that module resolves where
 * the file is (shared_db_path), opens every connection (WAL, busy timeout,
 * foreign keys, local disk only), and holds the schema — every table,
 * index, view and trigger below is created there, not here.
 *
 * Note *content* is stored as an opaque binary BLOB in the custom "BNBF"
 * format produced by serialize.c; this module never interprets it.
 *
 * Schema (created by shared_db.c; what this module reads and writes)
 * ------
 *   notes     (id, title, content BLOB, created_at, updated_at,
 *              pinned, body_text, trashed)
 *   tags      (id, name UNIQUE NOCASE, emoji,           -- SHARED with Tasks
 *              favorite_tasks, favorite_notes)
 *   note_tags (note_id, tag_id, in_field, in_text)       -- a note's tags, by source
 *   tasks, task_tags                                     -- the Tasks app's tables
 *   note_task (uid, task_id, note_id, ord,               -- a '!' line's task, keyed by the
 *              seen_title, seen_done, seen_due,             uid the note blob carries, and
 *              seen_deleted)                                what the line last agreed with
 *   action_uid_seq (id, next)                            -- uid high-water mark
 *
 * An ACTION ITEM is a TASK ROW: a '!' line in a note is a rendering of its
 * task's title, done state (status DONE) and due date, linked through
 * note_task by the uid the line carries in the blob (BNBF ACTION record).
 * Both apps edit those three fields; the seen_* columns are the baseline
 * that tells whose side changed (serialize.h: the push on save, the pull
 * of outside changes).  Notes writes only title/status/due/completed_at/
 * updated_at/deleted of a task — and parent_id, position when it creates
 * one (the schema tags it "Action Items") — never the Tasks-only columns.
 * The action_items view is what every listing reads.
 *
 * TAGS, not folders, organise the notes: all notes live in one store.  A
 * note's tags come from two sources, kept apart (note_tags.in_field /
 * in_text): its Tags field (on_db_note_field_tag_add / _remove) and the
 * #word tags of its text (on_db_note_set_text_tags, from every save).  A
 * tag the text has cannot be taken off in the field.  The tags are the two
 * apps': a name compares case-insensitively and is never renamed, the
 * emoji is shared, the favorite flag is each app's (favorite_notes here).
 * The schema's triggers delete a tag once no note and no live task carries
 * it, so nothing here prunes.
 *
 * Trash is a soft-delete FLAG: deleting sets trashed=1 and the note keeps
 * its tags; Restore clears the flag.  All normal listings/counts filter
 * trash out; on_db_note_list_all() can include it so search still finds
 * deleted notes.
 * =========================================================================== */

#ifndef BLUE_DB_H
#define BLUE_DB_H

#include <glib.h>
#include <sqlite3.h>

#include "shared_db.h"

/* ---------------------------------------------------------------------------
 * OnDbHealth — the outcome of one health pass over a database.
 *
 * `ok` is the only thing a caller normally acts on; `ran` is what tells
 * "the checks found problems" apart from "the checks could not be run",
 * which are a very different sentence to put in front of a user — and the
 * dividing line is whether the checks came back with ANYTHING, not
 * whether sqlite returned an error, because a corrupt file does both.
 * `detail` is NULL when there is nothing to say; `when` is unix time, and
 * 0 means no pass has been made.
 * ------------------------------------------------------------------------- */
typedef struct {
    gboolean  ok;                    /* both checks ran AND both passed     */
    gboolean  ran;                   /* both checks actually executed       */
    gint64    when;                  /* unix time of the pass, 0 = never    */
    gchar    *detail;                /* what went wrong, or NULL (owned)    */
    gboolean  pending;               /* an async pass is running now        */
} OnDbHealth;

/* ---------------------------------------------------------------------------
 * OnDatabase — one open connection to the shared database.
 *
 * Fields:
 *   shared — the shared module's connection (owned; shared_db.h), which
 *            owns the SQLite handle.  One per thread: a worker opens its
 *            own with on_db_open_existing(db->path, …).
 *   handle — shared_db_handle(shared), for this module's queries (borrowed
 *            from `shared`).
 *   path   — absolute path of the database file (owned string).
 * ------------------------------------------------------------------------- */
typedef struct {
    shared_db  *shared;
    sqlite3    *handle;
    gchar      *path;
    GHashTable *stmt_cache;   /* sql literal → sqlite3_stmt*, reused via
                                  reset+clear_bindings between calls        */
    /* The last health pass over this file, held IN MEMORY for the life
     * of the connection and deliberately never written down — see
     * on_db_health_check_async().                                          */
    OnDbHealth  health;
    gpointer    health_job;   /* the async pass in flight, or NULL (db.c) */
    GArray     *txn_hooks;    /* what runs when the open write transaction
                                  ends (on_db_txn_on_end), in order         */
} OnDatabase;

/* ---------------------------------------------------------------------------
 * OnNoteMeta — lightweight note metadata used by list views.
 *
 * Fields:
 *   id         — primary key of the note.
 *   title      — display title, derived from the first line (owned string).
 *   updated_at — UNIX timestamp of the last save.
 *   created_at — UNIX timestamp of the note's creation.
 *   pinned     — whether the note appears in the Pinned Notes section.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   id;
    gchar   *title;
    gint64   updated_at;
    gint64   created_at;
    gboolean pinned;
} OnNoteMeta;

/* ---------------------------------------------------------------------------
 * OnTag — one tag (shared with Tasks).
 *
 * Fields:
 *   id       — primary key of the tag.
 *   name     — the name, without a leading '#' (owned string); never
 *              renamed, compared case-insensitively.
 *   emoji    — display prefix, "" for none (owned string; shared).
 *   favorite — favorite_notes: a button in Notes' Favorites bar.
 *   in_field — read by on_db_note_tags only: the note's Tags field has it.
 *   in_text  — read by on_db_note_tags only: a #word in its text has it.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   id;
    gchar   *name;
    gchar   *emoji;
    gboolean favorite;
    gboolean in_field;
    gboolean in_text;
} OnTag;

/* Which tags on_db_tag_list lists.                                          */
typedef enum {
    ON_TAGS_CARRIED,                 /* those a note outside the Trash
                                      * carries: what Notes shows           */
    ON_TAGS_ALL                      /* every tag, Tasks' too: what Notes
                                      * offers wherever a tag is chosen     */
} OnTagScope;

/* ---------------------------------------------------------------------------
 * OnActionItem — one action item as the listings hand it out (a row of
 * the action_items view) and as a
 * save hands the note's '!' lines in (on_note_document_save builds one
 * per line for on_db_note_save_actions).
 *
 * Fields:
 *   note_id — owning note.
 *   ord     — the item's position among the note's action lines.
 *   text    — the task's title = the '!' line's remainder, trimmed,
 *             without any trailing "due <date>" (owned).
 *   done    — the task's status is DONE = the line is struck through.
 *   due     — the task's due date as a UNIX timestamp (local midnight) =
 *             the line's trailing "due <date>"; 0 = none.
 *   uid     — the item's STABLE identity: the line carries it in the note
 *             blob (BNBF ACTION record), note_task maps it to the task.
 *             Allocated once from action_uid_seq when a line is first
 *             saved, never reused once retired.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   note_id;
    gint     ord;
    gchar   *text;
    gboolean done;
    gint64   due;
    gint64   uid;
} OnActionItem;

/* --------------------------- lifecycle ---------------------------------- */

/* ---------------------------------------------------------------------------
 * on_db_open() — open a read-write connection to the shared database
 * through shared_db_open: a MISSING file is created atomically with the
 * shared schema; an existing file that is empty (0 bytes) or not a shared
 * database (an old notes.db or tasks.db, anything unstamped) is REFUSED
 * untouched, as is one on a network or cloud-synced folder — so nothing
 * may create an empty file at the path first.  Only the main connection,
 * at launch or "Create a New Database", uses it; every other connection
 * uses on_db_open_existing.
 *   path — the database file, or NULL for shared_db_path() ($CAMBIO_SHARED_DB,
 *          else the pointer file, else the per-user default).
 *   err  — optional; on failure receives the reason in words fit for the
 *          user (g_free it).
 * Returns a new OnDatabase*, or NULL on failure (also logged).              */
OnDatabase *on_db_open(const gchar *path, gchar **err);

/* ---------------------------------------------------------------------------
 * on_db_open_existing() — on_db_open that never creates the file
 * (shared_db_open_existing): for every connection but the main one at
 * launch — the workers (backup, search, grid cards, media scan), the
 * first-run "Open a Database File…" trial and File > Open Database File….
 * A file the user moved aside (a restore, a rollback) is refused as
 * missing rather than silently replaced by an empty database.
 * Inputs and output as on_db_open.                                         */
OnDatabase *on_db_open_existing(const gchar *path, gchar **err);

/* Close the database and free the handle.  Safe to call with NULL.         */
void on_db_close(OnDatabase *db);

/* ---------------------------------------------------------------------------
 * on_db_health_check_async() — shared_db_check (integrity, foreign keys,
 * stamps, WAL, the cross-table invariants) against the OPEN database, on
 * a worker thread with its own READ-ONLY connection to the file (a
 * connection must not cross threads), so nothing waits for it:
 * PRAGMA integrity_check walks every page and every index entry, ~2 s per
 * 600 MB on a warm local disk and longer from a cold iCloud Drive file.
 * on_db_health() reports `pending` until the verdict lands; it is then
 * remembered on the connection, so a later reader (the Settings window) can
 * say WHEN the answer was true, and `done` is called on the main thread.
 *
 * It writes NOTHING to the database, and that is the point rather than an
 * optimisation: a verdict stamped into the file would make a health check
 * MUTATE THE FILE IT WAS MEASURING, so the SHA-256 shown beside it would
 * move on every press of Update while the user's notes sat untouched.  The
 * cost, accepted: the verdict does not survive a restart.  Living on the
 * connection is also what makes File → Open Database safe — the old
 * connection's verdict cannot outlive the file it describes.  Closing the database while the pass runs abandons it —
 * `done` is never called for a connection that is gone.  One pass per
 * connection at a time: a second call while one is pending is ignored.
 *   db   — the open database.
 *   done — called on the main thread with the verdict recorded; may be
 *          NULL.
 *   data — its user data.
 * ------------------------------------------------------------------------- */
typedef void (*OnDbHealthFunc)(OnDatabase *db, gpointer data);
void on_db_health_check_async(OnDatabase *db, OnDbHealthFunc done,
                              gpointer data);

/* on_db_health() — the stored result, or NULL when no pass has been made or
 * started on this connection.  Borrowed, owned by the connection: do not
 * free it, and do not keep it across another pass.  While
 * `pending` is set the other fields are the PREVIOUS pass's (or zero).    */
const OnDbHealth *on_db_health(OnDatabase *db);

/* ---------------------------------------------------------------------------
 * on_db_file_sha256() — hex SHA-256 of the file at `path`, or NULL when it
 * cannot be read (g_free the result).
 *
 * Answers about the file AS IT STANDS, and is never stored: a live database
 * changes with the next thing the user types, and writing the digest into
 * the database would change it again.  A caller showing one is showing a
 * fingerprint of this moment, which is exactly what makes it comparable
 * with `shasum -a 256` or with a backup.
 * ------------------------------------------------------------------------- */
gchar *on_db_file_sha256(const gchar *path);

/* ----------------------------- notes ------------------------------------ */

/* Create an empty note.  Returns the new note's id, or 0 on failure.       */
gint64 on_db_note_create(OnDatabase *db);

/* Permanently delete `n` notes in ONE transaction (the schema drops the
 * tags nothing carries any more). Returns TRUE on success.                  */
gboolean on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n);

/* Persist a note's title, serialized content, and searchable plain text.
 *   id        — note to save.
 *   title     — display title (first line of the note).
 *   content   — BNBF blob bytes (may be NULL when len is 0).
 *   len       — length of `content` in bytes.
 *   body_text — plain text of the note for fast searching (may be NULL).
 * Also bumps updated_at.  Returns TRUE on success; FALSE on an error or
 * when the note no longer exists.                                           */
gboolean on_db_note_save(OnDatabase *db, gint64 id, const gchar *title,
                         const guint8 *content, gsize len,
                         const gchar *body_text);

/* ---------------------------------------------------------------------------
 * OnNoteStamp — what an open editor compares to tell that a note was
 * rewritten by another writer since it loaded or saved it: the row's
 * updated_at, the length of its blob and a 64-bit FNV-1a hash of the blob
 * bytes (so a rewrite in the same second to the same size is still seen).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  updated_at;
    gint64  size;
    guint64 hash;
} OnNoteStamp;

/* on_db_note_stamp() — note `id`'s stamp as stored now (read inside the
 * caller's transaction, if one is open).  Returns FALSE when the note does
 * not exist or cannot be read (*stamp is then zeroed).                    */
gboolean on_db_note_stamp(OnDatabase *db, gint64 id, OnNoteStamp *stamp);

/* on_db_note_stamp_same() — whether two stamps describe the same stored
 * note (every field equal).                                               */
gboolean on_db_note_stamp_same(const OnNoteStamp *a, const OnNoteStamp *b);

/* Read a note's cached searchable text. Returns a new string (g_free()
 * it), or NULL when the cache is unfilled (older rows) — callers then
 * extract from the blob and may write it back via
 * on_db_note_set_body_text().                                               */
gchar *on_db_note_body_text(OnDatabase *db, gint64 id);

/* Fill the searchable-text cache for one note, if it is still empty: text
 * extracted from an older read (the search worker) never overwrites what a
 * save has written meanwhile.                                               */
gboolean on_db_note_set_body_text(OnDatabase *db, gint64 id,
                                  const gchar *body_text);

/* Overwrite a note's updated_at with an explicit UNIX timestamp.  Used by
 * importers to preserve the original modification date (ordinary saves
 * stamp the current time). Returns TRUE on success.                         */
gboolean on_db_note_set_updated_at(OnDatabase *db, gint64 id, gint64 ts);

/* Load a note's serialized content.
 *   id      — note to load.
 *   out     — receives a newly allocated buffer (g_free() it), or NULL when
 *             the note has no content yet.
 *   out_len — receives the blob length in bytes.
 * Returns TRUE when the note was read; FALSE when it does not exist or the
 * read failed (then *out is NULL).  A failed read is never an empty note.  */
gboolean on_db_note_load(OnDatabase *db, gint64 id, guint8 **out,
                         gsize *out_len);

/* Fetch metadata for a single note. Returns an OnNoteMeta* to free with
 * on_db_note_meta_free(), or NULL if not found.                             */
OnNoteMeta *on_db_note_get(OnDatabase *db, gint64 id);

/* List ALL notes in the database, newest first.
 *   include_trash — TRUE to include trashed notes (search wants them);
 *                   FALSE for the normal visible set (export).
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list_all(OnDatabase *db, gboolean include_trash);

/* List every visible (non-trashed) note, newest first — the library's
 * "All Notes" view. Returns a GList of OnNoteMeta*; free with
 * on_db_note_list_free().                                                   */
GList *on_db_note_list_recent(OnDatabase *db);

/* Set or clear a note's pinned flag. Returns TRUE on success.               */
gboolean on_db_note_set_pinned(OnDatabase *db, gint64 id, gboolean pinned);

/* List every pinned note, newest first.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list_pinned(OnDatabase *db);

/* Number of pinned notes.                                                   */
gint on_db_note_count_pinned(OnDatabase *db);

/* Free one OnNoteMeta and its strings.                                      */
void on_db_note_meta_free(OnNoteMeta *meta);

/* Free a list of OnNoteMeta* (on_db_note_list_all and the other listings). */
void on_db_note_list_free(GList *notes);

/* ------------------------------ tags ------------------------------------ */

/* The tags of `scope`, ordered by name (case-insensitive).
 * Returns a GList of OnTag*; free with on_db_tag_list_free().               */
GList *on_db_tag_list(OnDatabase *db, OnTagScope scope);

/* Free a list of OnTag* (on_db_tag_list, on_db_note_tags).                   */
void on_db_tag_list_free(GList *tags);

/* Free one OnTag (NULL-safe).                                               */
void on_db_tag_free(OnTag *tag);

/* One tag by id, or NULL when it has gone.  Free with on_db_tag_free.       */
OnTag *on_db_tag_get(OnDatabase *db, gint64 id);

/* The tag of a name (trimmed, compared case-insensitively), or NULL when
 * there is none.  Free with on_db_tag_free.                                 */
OnTag *on_db_tag_find(OnDatabase *db, const gchar *name);

/* Set a tag's emoji ("" or NULL for none; Tasks shows it too) or Notes'
 * favorite flag.  A tag's name never changes.  Each returns TRUE when
 * written.                                                                  */
gboolean on_db_tag_set_emoji(OnDatabase *db, gint64 id, const gchar *emoji);
gboolean on_db_tag_set_favorite(OnDatabase *db, gint64 id, gboolean favorite);

/* A note's tags, by name, each with its sources (OnTag.in_field /
 * in_text).  Returns a GList of OnTag*; free with on_db_tag_list_free().    */
GList *on_db_note_tags(OnDatabase *db, gint64 note_id);

/* The note's TEXT tags become exactly `tag_names` (a GList of "const
 * gchar*" names, without '#'; an existing tag of a name in any case is
 * used, a missing one made): a tag the text no longer has keeps its row
 * while the Tags field has it and goes otherwise.  The Tags field is not
 * touched.  Runs in the caller's transaction when one is open.  Returns
 * TRUE on success.                                                          */
gboolean on_db_note_set_text_tags(OnDatabase *db, gint64 note_id,
                                  GList *tag_names);

/* Put the tag `name` (trimmed; the existing tag of that name in any case,
 * else a new one) in the note's Tags field.  The note's content and
 * updated_at are not touched.  Returns the tag's id, or 0 when the name
 * is blank, the note is missing or the write failed.                       */
gint64 on_db_note_field_tag_add(OnDatabase *db, gint64 note_id,
                                const gchar *name);

/* Take tag `tag_id` out of the note's Tags field.  Refused while the
 * note's text has it (it comes off in the text).  Returns 1 when written
 * (also when the field did not have it), 0 when refused, -1 when the
 * write failed (logged).                                                    */
gint on_db_note_field_tag_remove(OnDatabase *db, gint64 note_id, gint64 tag_id);

/* List all notes labeled with tag `tag_id`, newest first.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_notes_by_tag(OnDatabase *db, gint64 tag_id);

/* --------------------------- action items -------------------------------- */

/* A struck line is a task with status SHARED_DB_TASK_STATUS_DONE; every
 * other status is "not done" (shared_db.h names them).  New items carry
 * the tag SHARED_DB_ACTION_LIST_NAME / SHARED_DB_ACTION_LIST_EMOJI (the
 * schema gives it).                                                       */

/* Allocate a fresh uid from the action_uid_seq high-water mark: only ever
 * incremented, so a uid retired with a deleted item is never handed out
 * again.  Runs in its own transaction unless the caller already has one
 * open.  Inside a transaction that then rolls back, the counter rolls back
 * too and the same uid will be handed out again: whoever stored the uid
 * outside the database must drop it unless the transaction commits
 * (on_note_document_save does, through on_db_txn_on_end).  Returns 0 on
 * failure.                                                                */
gint64 on_db_action_uid_alloc(OnDatabase *db);

/* The three fields a '!' line and its task share, as bits of a mask.     */
#define ON_ACTION_FIELD_TEXT 1u      /* line text  <-> tasks.title          */
#define ON_ACTION_FIELD_DONE 2u      /* struck     <-> tasks.status DONE    */
#define ON_ACTION_FIELD_DUE  4u      /* due suffix <-> tasks.due            */

/* ---------------------------------------------------------------------------
 * OnActionLink — one note_task row and the state of its task: the line's
 * identity and placement, the seen_* baseline (what the line and the task
 * last agreed on), and the task's own title / done / due / deleted.  Its
 * two strings are owned: on_db_action_link_clear.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   uid;
    gint64   task_id;
    gint64   note_id;
    gint     ord;
    gchar   *seen_title;             /* every byte of it (NUL-terminated,  */
    gsize    seen_title_len;         /* but may hold a NUL: use the length) */
    gboolean seen_done;
    gint64   seen_due;
    gboolean seen_deleted;           /* 1 = Notes itself tombstoned it      */
    gchar   *title;                  /* the task's, every byte, as above    */
    gsize    title_len;
    gboolean title_ok;               /* valid UTF-8 with no NUL over its
                                        whole length: a line can show it   */
    gboolean done;                   /* status == DONE                      */
    gint64   due;
    gboolean deleted;
} OnActionLink;

/* ---------------------------------------------------------------------------
 * on_db_note_save_actions() — THE PUSH: bring note `note_id`'s tasks in
 * line with `items`, its real action lines in document order (each with
 * the uid its line carries — never 0: the caller allocates first, since the
 * blob must hold the uid before it is written).  Joins the caller's
 * transaction (on_db_txn_begin) or runs in its own.  Per line:
 *   - no note_task row for the uid: a new task in the managed list (title,
 *     due, done → status DONE + completed_at now, else NEW; every other
 *     column the schema's default) and its row, seen_* = the line;
 *   - task deleted with seen_deleted = 0 (deleted elsewhere, not yet
 *     pulled — maybe its line was pasted here from another note): left
 *     alone, for the pull;
 *   - task deleted with seen_deleted = 1 (Notes removed the line, and an
 *     undo brought it back): revived (deleted = 0), seen_deleted = 0,
 *     with the subtasks tombstoned in that same operation (parent_id = the
 *     task, still deleted, updated_at = the parent's tombstone updated_at);
 *   - then each field whose LINE value differs from its seen_* value —
 *     only those — is written to the task (ON_ACTION_FIELD_*; done folds
 *     into status: done → DONE stamping completed_at on entry, not done →
 *     SHARED_DB_SQL_UNTICK_STATUS), updated_at stamped, and seen_* := the line's
 *     values; the row is re-pointed / re-numbered when the line moved.
 *     A field equal to its seen_* is never written: a change Tasks made
 *     to it stays (the pull brings it into the line).
 *   - every task of this note whose line is gone, still live and known
 *     live to Notes (seen_deleted = 0), is TOMBSTONED (deleted = 1) with
 *     its live subtasks, one statement, one updated_at
 *     (SHARED_DB_SQL_TOMBSTONE_WHERE, the note_task_tombstone trigger's
 *     rule), and its row gets seen_deleted = 1 (so an undo can revive it).
 * `changed` (optional) receives whether any task row was written.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
gboolean on_db_note_save_actions(OnDatabase *db, gint64 note_id,
                                 GList *items, gboolean *changed);

/* on_db_action_diff() — which fields (ON_ACTION_FIELD_*) of the line `it`
 * differ from the row's seen_* baseline: what a push writes.  Text is
 * compared byte for byte over seen_title's whole length (the line's text
 * is already trimmed), done as a boolean, due as the integer.             */
guint on_db_action_diff(const OnActionItem *it, const OnActionLink *row);

/* on_db_action_link_title_moved() — whether the task's title differs from
 * seen_title, byte for byte over both whole lengths (a NUL inside one
 * counts).                                                                */
gboolean on_db_action_link_title_moved(const OnActionLink *row);

/* on_db_action_link_agree_title() — seen_title := `len` bytes of `text`
 * (in memory; on_db_action_link_update writes it).                        */
void on_db_action_link_agree_title(OnActionLink *row, const gchar *text,
                                   gsize len);

/* on_db_action_links() — every note_task row of note `note_id` with its
 * task, ord order.  Returns a GArray of OnActionLink that clears its
 * elements; g_array_unref() it.                                           */
GArray *on_db_action_links(OnDatabase *db, gint64 note_id);

/* on_db_action_link_update() — write a row's placement and its whole
 * seen_* baseline as given (the push and the pull: "seen := what the line
 * and the task now agree on"); seen_title is written with all of its
 * bytes.  TRUE on success.                                                */
gboolean on_db_action_link_update(OnDatabase *db, const OnActionLink *row);

/* on_db_action_link_delete() — delete one note_task row (the pull, when
 * the task was deleted elsewhere and its line has been unmarked; the
 * tombstone trigger does nothing to an already deleted task).             */
gboolean on_db_action_link_delete(OnDatabase *db, gint64 uid);

/* on_db_action_link_clear() — free a row's strings (NULL-safe fields).    */
void on_db_action_link_clear(OnActionLink *row);

/* on_db_action_pending() — the notes the pull has work in: those with a
 * note_task row matching SHARED_DB_SQL_ACTION_PENDING (seen_deleted = 0,
 * and the task differs from seen_* — title, status DONE vs seen_done, due
 * — or is deleted).  `note_id` 0 asks about every note, else about that
 * one.  Returns a GArray of gint64 note ids,
 * ascending; g_array_unref() it.                                          */
GArray *on_db_action_pending(OnDatabase *db, gint64 note_id);

/* ---------------------------------------------------------------------------
 * on_db_txn_begin() — one write transaction around several calls (BEGIN
 * IMMEDIATE through shared_db_begin), or JOIN the one already open on this
 * connection.  Every on_db_* writer joins an open transaction the same
 * way.
 *   db  — open database.
 *   own — out: TRUE when this call opened the transaction (on_db_txn_end
 *         then commits it), FALSE when it joined the caller's.
 * Returns TRUE when a transaction is open.
 * ------------------------------------------------------------------------- */
gboolean on_db_txn_begin(OnDatabase *db, gboolean *own);

/* ---------------------------------------------------------------------------
 * on_db_txn_end() — finish what on_db_txn_begin started: when `own`, COMMIT
 * if `ok`, else ROLLBACK, and then run every hook registered with
 * on_db_txn_on_end, in order, with whether the work was committed (a
 * failed COMMIT rolls back and counts as not committed).  A joined
 * transaction is left to its owner, who sees `ok`.
 *   db  — open database, inside the transaction.
 *   own — on_db_txn_begin's `own`.
 *   ok  — whether every step in it succeeded.
 * Returns TRUE only when every step succeeded and, when owned, committed.
 * ------------------------------------------------------------------------- */
gboolean on_db_txn_end(OnDatabase *db, gboolean own, gboolean ok);

/* What on_db_txn_on_end runs: `data` as registered, and whether the
 * transaction committed.  The hook owns `data` (frees it, if it must).    */
typedef void (*OnDbTxnHook)(gpointer data, gboolean committed);

/* ---------------------------------------------------------------------------
 * on_db_txn_on_end() — run `hook` when the write transaction open on this
 * connection ends (the outermost on_db_txn_end, or on_db_close, which
 * counts as a rollback): what a joined writer does about state OUTSIDE the
 * database that is right only if its writes are committed — a document's
 * freshly allocated uids reset on a rollback, an editor marked saved only
 * after the commit, a refresh made after the lock is released.  Hooks run
 * in registration order, after the COMMIT or ROLLBACK, with no
 * transaction open.
 *   db   — open database, inside a transaction.
 *   hook — the hook.
 *   data — its argument.
 * Returns TRUE when registered; FALSE (hook not called) when no
 * transaction is open.
 * ------------------------------------------------------------------------- */
gboolean on_db_txn_on_end(OnDatabase *db, OnDbTxnHook hook, gpointer data);

/* One live item by uid, off the view; NULL when no
 * live task carries that uid.  Free with on_db_action_item_free().        */
OnActionItem *on_db_action_get(OnDatabase *db, gint64 uid);

/* Write the fields in `mask` (ON_ACTION_FIELD_*) onto the live task with
 * this uid, and only those, with the push's status rule (the task half of
 * an edit from the Action Items view; the pull then renders the line).
 * seen_* is NOT changed.  FALSE when no live task carries that uid.       */
gboolean on_db_action_set(OnDatabase *db, gint64 uid, guint mask,
                          const gchar *title, gboolean done, gint64 due);

/* Free one OnActionItem.                                                  */
void on_db_action_item_free(OnActionItem *it);

/* Every action item of every visible (non-trashed) note, newest note
 * first, note order preserved within a note.  Returns a GList of
 * OnActionItem*; free with on_db_action_list_free().                        */
GList *on_db_action_list(OnDatabase *db);

/* All action items for one specific note, ordered by ord.  Returns a GList
 * of OnActionItem*; free with on_db_action_list_free().                     */
GList *on_db_action_list_for_note(OnDatabase *db, gint64 note_id);

/* Item counts across all visible notes (either out-param may be NULL):
 * `total` = every item, `open` = the not-yet-done ones.                     */
void on_db_action_counts(OnDatabase *db, gint *total, gint *open);

/* Free a list of OnActionItem (from on_db_action_list or
 * on_note_extract_actions).                                                 */
void on_db_action_list_free(GList *items);

/* ----------------------------- trash ------------------------------------ */

/* Move `n` notes to the Trash in one transaction (trashed=1; their tags
 * stay). Returns TRUE on success.                                           */
gboolean on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n);

/* Restore note `id` from the Trash, with its tags.  Returns TRUE on
 * success.                                                                  */
gboolean on_db_note_restore(OnDatabase *db, gint64 id);

/* List the trashed notes (what selecting Trash shows), newest first.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list_trashed(OnDatabase *db);

/* TRUE when note `id` is in the Trash (the rule every visible listing
 * filters on).  A missing note reads as trashed: nothing visible to keep. */
gboolean on_db_note_is_trashed(OnDatabase *db, gint64 id);

/* Number of notes in the Trash.                                            */
gint on_db_trash_count(OnDatabase *db);

/* Ids of every note in the Trash: what an empty-trash deletes (used to
 * close their editors first). Returns a GArray of gint64;
 * g_array_free(_, TRUE).                                                    */
GArray *on_db_trash_note_ids(OnDatabase *db);

/* Permanently delete every note in the Trash (one transaction; the schema
 * drops the tags nothing carries any more). Returns TRUE on success.       */
gboolean on_db_trash_empty(OnDatabase *db);

/* ----------------------------- counts ----------------------------------- */

/* Number of visible (non-trashed) notes in the whole database — the
 * count shown on the "All Notes" sidebar row.                               */
gint on_db_note_count_visible(OnDatabase *db);

/* Totals for the About dialog (either out-param may be NULL): every note,
 * and the tags notes carry.                                                 */
void on_db_totals(OnDatabase *db, gint *notes, gint *tags);

/* ---------------------------------------------------------------------------
 * on_db_note_text_map() — every filled body_text row in ONE query: note id
 * (gint64*) → owned text.  The readers that need many notes' text at once
 * use this instead of one SELECT per note — the whole column is ~1.5 MB even
 * for image-heavy databases, and per-query latency is what hurts on
 * shared/network DBs.
 *   max_chars — keep only this many leading characters of each value (the
 *               list-view preview shows one line), or 0 for the full text.
 * Rows with a NULL cache (pre-column saves) are ABSENT; callers fall back to
 * on_note_text_cached() for those.
 * Destroy with g_hash_table_destroy().                                      */
GHashTable *on_db_note_text_map(OnDatabase *db, gint max_chars);

/* Per-tag counts of visible notes in ONE query: tag id (gint64*) →
 * GINT_TO_POINTER(count); missing keys mean zero.  Destroy with
 * g_hash_table_destroy().                                                   */
GHashTable *on_db_tag_count_map(OnDatabase *db);

/* Every note's tags as one display string, in ONE query: note id
 * (gint64*) → "Salt, Stride" (by name, case-insensitive; owned).  A note
 * with no tag has no entry — callers treat a miss as "".  The notes list's
 * Tags column, the search results and the exporter read this, never one
 * query per note.  Destroy with g_hash_table_destroy().                     */
GHashTable *on_db_note_tag_names_map(OnDatabase *db);

#endif /* BLUE_DB_H */
