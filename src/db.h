/* ===========================================================================
 * db.h — SQLite persistence layer for Notes
 *
 * All notes, folders and tags live in the ONE SQLite file Notes shares with
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
 *   folders   (id, parent_id, name, sort_order, trashed, ai_mode, emoji) -- nested via parent_id
 *   notes     (id, folder_id, title, content BLOB,
 *              sort_order, created_at, updated_at,
 *              pinned, body_text, trashed)
 *   tags      (id, name UNIQUE)
 *   note_tags    (note_id, tag_id)                       -- many-to-many
 *   list_groups, lists, tasks                            -- the Tasks app's tables
 *   note_task (uid, task_id, note_id, ord,               -- a '!' line's task, keyed by the
 *              seen_title, seen_done, seen_due,             uid the note blob carries, and
 *              seen_deleted)                                what the line last agreed with
 *   action_uid_seq (id, next)                            -- uid high-water mark
 *
 * An ACTION ITEM is a TASK ROW: a '!' line in a note is a rendering of its
 * task's title, done state (status 2) and due date, linked through
 * note_task by the uid the line carries in the blob (BNBF ACTION record).
 * Both apps edit those three fields; the seen_* columns are the baseline
 * that tells whose side changed (serialize.h: the push on save, the pull
 * of outside changes).  Notes writes only title/status/due/completed_at/
 * updated_at/deleted of a task — and list_id, parent_id, position when it
 * creates one — never the Tasks-only columns.  The action_items_ctx view
 * is what every listing reads.
 *
 * A NULL parent_id means "top-level folder"; a NULL folder_id means the
 * note lives at the top level ("Notes" root).
 *
 * Trash is a soft-delete FLAG, not a folder: deleting sets trashed=1 and
 * leaves folder_id/parent_id untouched (that is the "restore to" location).
 * A trashed folder keeps its whole subtree attached — only the top folder
 * is flagged; its descendants (and their notes) are implicitly trashed via
 * the trash_folder_ids view (recursive closure of flagged folders).  All
 * normal listings/counts filter trash out; on_db_note_list_all() can
 * include it so search still finds deleted notes.
 * =========================================================================== */

#ifndef BLUE_DB_H
#define BLUE_DB_H

#include <glib.h>
#include <sqlite3.h>

#include "shared_db.h"

/* ---------------------------------------------------------------------------
 * DB constants
 * ------------------------------------------------------------------------- */
/* Default title used for a newly created note and as the fallback when the
 * first-line extraction returns empty text.                                  */
#define ON_DEFAULT_NOTE_TITLE          "New Note"

/* Maximum depth walked when building a folder path string.  A corrupt cycle
 * in the parent_id chain (hand-edited DB) would otherwise spin forever.     */
#define ON_DB_FOLDER_PATH_DEPTH_LIMIT  128

/* Maximum depth checked when validating a folder move for cycles.           */
#define ON_DB_FOLDER_CYCLE_DEPTH_LIMIT 1000

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
 *            own with on_db_open(db->path, …).
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
} OnDatabase;

/* ---------------------------------------------------------------------------
 * OnFolder — metadata for one folder row.
 *
 * Fields:
 *   id    — primary key of the folder.
 *   name  — display name (owned string).
 *   emoji — optional display prefix shown in the sidebar (owned string;
 *            "" when not set).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gchar  *name;
    gchar  *emoji;
} OnFolder;

/* ---------------------------------------------------------------------------
 * OnNoteMeta — lightweight note metadata used by list views.
 *
 * Fields:
 *   id         — primary key of the note.
 *   folder_id  — id of the containing folder, or 0 for top level.
 *   title      — display title, derived from the first line (owned string).
 *   updated_at — UNIX timestamp of the last save.
 *   created_at — UNIX timestamp of the note's creation.
 *   pinned     — whether the note appears in the Pinned Notes section.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   id;
    gint64   folder_id;
    gchar   *title;
    gint64   updated_at;
    gint64   created_at;
    gboolean pinned;
} OnNoteMeta;

/* ---------------------------------------------------------------------------
 * OnTag — one tag row.
 *
 * Fields:
 *   id   — primary key of the tag.
 *   name — tag text without the leading '#' (owned string).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gchar  *name;
} OnTag;

/* ---------------------------------------------------------------------------
 * OnActionItem — one action item as the listings hand it out (a row of
 * the action_items_ctx view: a task with its note's placement) and as a
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
 *   context — the CONTAINER the owning note lives in: the name of its
 *             own folder, whatever sits above that — a note in
 *             Salt/Deployments/Stride has the context "Stride" ("" for a
 *             note at the root).  Derived by the view from the folder
 *             tree — never stored: a stored copy would go stale the
 *             moment a note moved or a folder was renamed.  NULL on an
 *             item built from a line, which knows no folder.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   note_id;
    gint     ord;
    gchar   *text;
    gboolean done;
    gint64   due;
    gint64   uid;
    gchar   *context;
} OnActionItem;

/* --------------------------- lifecycle ---------------------------------- */

/* ---------------------------------------------------------------------------
 * on_db_open() — open a read-write connection to the shared database
 * through shared_db_open: a missing or empty file is created with the
 * shared schema; a file that is not a shared database (an old notes.db or
 * tasks.db, anything unstamped) is REFUSED untouched, as is one on a
 * network or cloud-synced folder.
 *   path — the database file, or NULL for shared_db_path() ($CAMBIO_SHARED_DB,
 *          else the pointer file, else the per-user default).
 *   err  — optional; on failure receives the reason in words fit for the
 *          user (g_free it).
 * Returns a new OnDatabase*, or NULL on failure (also logged).              */
OnDatabase *on_db_open(const gchar *path, gchar **err);

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

/* ---------------------------- folders ----------------------------------- */

/* Create a folder.
 *   parent_id — id of the parent folder, or 0 for top level.
 *   name      — display name for the new folder.
 * Returns the new folder's id, or 0 on failure.                             */
/* Create a folder under `parent_id` (0 = top level), appended after its
 * existing siblings.  ai_mode (ON_AI_MODE_*) and emoji ("" or NULL for none)
 * are written by the same INSERT.
 * Returns the new folder's id, or 0 on failure.                              */
gint64 on_db_folder_create(OnDatabase *db, gint64 parent_id,
                           const gchar *name, gint ai_mode,
                           const gchar *emoji);

/* Set a folder's name, AI mode and emoji in ONE statement (the Info dialog
 * can change all three).  Returns TRUE on success.                          */
gboolean on_db_folder_update(OnDatabase *db, gint64 id, const gchar *name,
                             gint ai_mode, const gchar *emoji);

/* ai_mode values stored in folders.ai_mode.                                 */
#define ON_AI_MODE_NORMAL  0
#define ON_AI_MODE_PROJECT 1
#define ON_AI_MODE_CUSTOM  2

gint     on_db_folder_get_ai_mode(OnDatabase *db, gint64 id);

/* Get a folder's display emoji. Returns a new string (g_free() it; never
 * NULL — "" when not set).                                                   */
gchar   *on_db_folder_get_emoji(OnDatabase *db, gint64 id);

/* Move folder `id` (with its whole subtree, implicitly) under
 * `parent_id` (0 = top level), appending it after that parent's existing
 * children.  Also clears the trashed flag, so dragging a folder out of
 * the Trash restores it to the drop location.  Refuses cycles: moving a
 * folder into itself or any of its descendants returns FALSE.               */
gboolean on_db_folder_move(OnDatabase *db, gint64 id, gint64 parent_id);

/* Persist an explicit ordering of folders within one parent.
 *   folder_ids — ids in the desired order (sort_order = array index).
 *   n          — number of ids.
 * All updates run in one transaction. Returns TRUE on success.               */
gboolean on_db_folder_reorder(OnDatabase *db, const gint64 *folder_ids,
                              gsize n);

/* Order one folder's DIRECT children alphabetically (case-insensitive) and
 * persist that as their sort_order — THE "Sort Subfolders Alphabetically"
 * operation of the folder context menu.  One level
 * only: each child's own children keep the order they had.
 *   parent_id — whose children to sort (0 = the top level).
 * Returns how many children were sorted, or -1 on failure.                  */
gint on_db_folder_sort_children(OnDatabase *db, gint64 parent_id);

/* PERMANENTLY delete folder `id`; all descendant folders and contained
 * notes are removed by ON DELETE CASCADE and orphaned tags are pruned.
 * The GUI trashes folders instead (on_db_folder_trash); this is the
 * delete-from-the-Trash path. Returns TRUE on success.                      */
gboolean on_db_folder_delete(OnDatabase *db, gint64 id);

/* List the *direct* children of `parent_id` (0 = top level), ordered by
 * sort_order then name.
 * Returns a GList of OnFolder*; free with on_db_folder_list_free().         */
GList *on_db_folder_list(OnDatabase *db, gint64 parent_id);

/* Free a list returned by on_db_folder_list().                              */
void on_db_folder_list_free(GList *folders);

/* ---------------------------------------------------------------------------
 * on_db_folder_child_map() — every non-trashed folder in ONE query, grouped
 * by parent.  Building a folder tree by recursing with on_db_folder_list()
 * costs one query per folder (115 of them on a 114-folder database, on every
 * sidebar rebuild); walking this map costs one, the same trick
 * on_db_folder_path_map() uses.  Children come out in exactly the order
 * on_db_folder_list() returns them.
 * Returns a map of parent id → GList of OnFolder* (read it with
 * on_db_folder_children()); free the whole thing with
 * on_db_folder_child_map_free().  The lists and folders belong to the map —
 * never pass one to on_db_folder_list_free().
 * ------------------------------------------------------------------------- */
GHashTable *on_db_folder_child_map(OnDatabase *db);

/* Direct children of `parent_id` (0 = top level) within a child map, or
 * NULL when that folder has none.  Borrowed: owned by the map.              */
GList *on_db_folder_children(GHashTable *map, gint64 parent_id);

/* Free a map from on_db_folder_child_map() and every folder in it.  NULL-safe. */
void on_db_folder_child_map_free(GHashTable *map);

/* ----------------------------- notes ------------------------------------ */

/* Create an empty note in folder `folder_id` (0 = top level).
 * Returns the new note's id, or 0 on failure.                               */
gint64 on_db_note_create(OnDatabase *db, gint64 folder_id);

/* Permanently delete `n` notes in ONE transaction, pruning orphaned tags
 * once at the end. Returns TRUE on success.                                 */
gboolean on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n);

/* Move `n` notes into folder `folder_id` (0 = top level) in ONE
 * transaction (per-note moves would fsync per call — a big
 * multi-selection drop froze the GUI), appended in array order.  Clears
 * each note's trashed flag (drag out of Trash = restore).  Returns TRUE
 * if every move succeeded (all-or-nothing: failure rolls the batch back).   */
gboolean on_db_notes_move(OnDatabase *db, const gint64 *note_ids, gsize n,
                          gint64 folder_id);

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

/* List notes directly inside folder `folder_id` (0 = top level), ordered
 * by sort_order then updated_at descending.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list(OnDatabase *db, gint64 folder_id);

/* List ALL notes in the database, ordered by folder then sort_order.
 *   include_trash — TRUE to include trashed notes and notes inside
 *                   trashed folders (search wants them); FALSE for the
 *                   normal visible set (export).
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

/* Free a list returned by on_db_note_list()/on_db_note_list_all().          */
void on_db_note_list_free(GList *notes);

/* ------------------------------ tags ------------------------------------ */

/* List every known tag, ordered by name.
 * Returns a GList of OnTag*; free with on_db_tag_list_free().               */
GList *on_db_tag_list(OnDatabase *db);

/* Free a list returned by on_db_tag_list().                                 */
void on_db_tag_list_free(GList *tags);

/* Replace note `note_id`'s tag set with `tag_names` (a GList of "const
 * gchar*" tag names, without '#'). Creates missing tags and prunes tags
 * that no longer label any note. Returns TRUE on success.                   */
gboolean on_db_note_set_tags(OnDatabase *db, gint64 note_id, GList *tag_names);

/* List all notes labeled with tag `tag_id`, newest first.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_notes_by_tag(OnDatabase *db, gint64 tag_id);

/* --------------------------- action items -------------------------------- */

/* tasks.status of a done task (== struck in the note); 0 is New, 1 In
 * Progress — the Tasks app's encoding, which shared_db.h does not name.
 * New items go into the managed list SHARED_DB_ACTION_LIST_NAME /
 * SHARED_DB_ACTION_LIST_EMOJI (shared_db.h).                              */
#define ON_TASK_STATUS_DONE        2

/* Allocate a fresh uid from the action_uid_seq high-water mark: only ever
 * incremented, so a uid retired with a deleted item is never handed out
 * again.  Runs in its own transaction unless the caller already has one
 * open; a uid burned by a save that then fails is harmless.  Returns 0 on
 * failure.                                                                */
gint64 on_db_action_uid_alloc(OnDatabase *db);

/* Sync note `note_id`'s task rows to `items` — the note's real action
 * lines in document order, each with the uid its line carries (never 0:
 * the caller allocates first, since the blob must hold the uid before it
 * is written).  One transaction: an item with no task row gets one
 * (created in the managed list); an existing task is updated only where
 * title / done / due differ (done folds into status the way the Tasks
 * app's done-only sources do: done → 2, undone → 1 if it was 2, else
 * unchanged; completed_at stamped on entering 2; a tombstoned row comes
 * back to life — the undo of a deleted line); its note_task row is
 * re-pointed and re-numbered when the line moved; and every task of this
 * note whose uid is no longer among the items is TOMBSTONED (deleted=1).
 * `changed` (optional) receives whether any task row was written.
 * Returns TRUE on success.                                                */
gboolean on_db_note_save_actions(OnDatabase *db, gint64 note_id,
                                 GList *items, gboolean *changed);

/* One live item by uid, off the view (context included); NULL when no
 * live task carries that uid.  Free with on_db_action_item_free().        */
OnActionItem *on_db_action_get(OnDatabase *db, gint64 uid);

/* Write one live item's title / done / due onto its task (the row-first
 * half of an external edit; the line is re-rendered from it afterwards).
 * FALSE when no live task carries that uid.                               */
gboolean on_db_action_set(OnDatabase *db, gint64 uid, const gchar *title,
                          gboolean done, gint64 due);

/* Free one OnActionItem.                                                  */
void on_db_action_item_free(OnActionItem *it);

/* Every action item of every visible (non-trashed) note, newest note
 * first, note order preserved within a note, each with its `context` (the
 * name of the note's folder).  Returns a GList of OnActionItem*; free with
 * on_db_action_list_free().                                                 */
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

/* Move `n` notes to the Trash in one transaction (trashed=1; folder_id is
 * kept as the restore location). Returns TRUE on success.                   */
gboolean on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n);

/* Restore note `id` from the Trash.  Its folder_id is kept unless that
 * folder is itself (implicitly) trashed, in which case the note lands at
 * the top level. Also un-nests a note out of a trashed folder (moves it
 * to the top level) even when the note itself carries no trashed flag.
 * Returns TRUE on success.                                                  */
gboolean on_db_note_restore(OnDatabase *db, gint64 id);

/* Move folder `id` (with its whole subtree, implicitly) to the Trash.       */
gboolean on_db_folder_trash(OnDatabase *db, gint64 id);

/* Restore folder `id` to its original parent, or to the top level when
 * that parent is itself still in the Trash. Returns TRUE on success.        */
gboolean on_db_folder_restore(OnDatabase *db, gint64 id);

/* List the directly-trashed folders (the Trash section's children),
 * ordered by name. Returns a GList of OnFolder*; free with
 * on_db_folder_list_free().                                                 */
GList *on_db_folder_list_trashed(OnDatabase *db);

/* List the directly-trashed notes (what selecting Trash shows), newest
 * first. Returns a GList of OnNoteMeta*; free with on_db_note_list_free(). */
GList *on_db_note_list_trashed(OnDatabase *db);

/* TRUE when note `id` is in the Trash — either flagged itself or sitting
 * inside a trashed folder's subtree (the same rule every visible listing
 * filters on).  A missing note reads as trashed: nothing visible to keep. */
gboolean on_db_note_is_trashed(OnDatabase *db, gint64 id);

/* Number of items directly in the Trash: trashed notes + trashed folders. */
gint on_db_trash_count(OnDatabase *db);

/* Ids of EVERY note that would go with an empty-trash: directly-trashed
 * notes plus all notes inside trashed folder subtrees.  Used to close
 * their editors first. Returns a GArray of gint64; g_array_free(_, TRUE).   */
GArray *on_db_trash_note_ids(OnDatabase *db);

/* Ids of every note in folder `folder_id`'s subtree (the folder itself
 * included). Returns a GArray of gint64; g_array_free(_, TRUE).             */
GArray *on_db_folder_note_ids(OnDatabase *db, gint64 folder_id);

/* Permanently delete everything in the Trash (one transaction, orphaned
 * tags pruned). Returns TRUE on success.                                    */
gboolean on_db_trash_empty(OnDatabase *db);

/* ----------------------------- counts ----------------------------------- */

/* Number of visible (non-trashed) notes in the whole database — the
 * count shown on the "All Notes" sidebar row.                               */
gint on_db_note_count_visible(OnDatabase *db);

/* Total row counts across the whole database (any out-param may be NULL). */
void on_db_totals(OnDatabase *db, gint *notes, gint *folders, gint *tags);

/* Per-folder note counts in ONE query (folder id 0 = top level).
 * Returns a GHashTable of gint64* → GINT_TO_POINTER(count); destroy with
 * g_hash_table_destroy(). Missing keys mean zero.                           */
GHashTable *on_db_note_count_map(OnDatabase *db);

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

/* Per-tag note counts in one query; same shape as above.                    */
GHashTable *on_db_tag_count_map(OnDatabase *db);

/* ---------------------------- utilities --------------------------------- */

/* Build the "/Folder/Sub" style path of folder `folder_id` (empty string
 * for 0). Used by the exporter to mirror the hierarchy on disk.
 * Returns a newly allocated string; g_free() it.                            */
gchar *on_db_folder_path(OnDatabase *db, gint64 folder_id);

/* All folder paths in ONE query: folder id (gint64*) → "Folder/Sub"
 * string (same format as on_db_folder_path, no leading slash).  Trashed
 * folders are included so Trash listings resolve too; the top level (0)
 * has no entry — callers treat a miss as "".  Per-note path lookups in
 * the library go through this map, never per-row on_db_folder_path —
 * per-query latency hurts on shared/network DBs.
 * Free with g_hash_table_destroy().                                         */
GHashTable *on_db_folder_path_map(OnDatabase *db);

#endif /* BLUE_DB_H */
