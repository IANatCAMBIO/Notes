/* ===========================================================================
 * serialize.h — the blob side of a note
 *
 * The blob-level helpers around a note: the body_text cache
 * (on_note_extract_text, derived through the model), the image ordinals
 * the media browser addresses (on_note_count_images /
 * on_note_image_nth_png), and the document's two ends —
 * on_note_document_load, the preamble of every consumer that wants the
 * document, and on_note_document_save, THE one way a document is written
 * back (blob, title, body text, and the task rows its '!' lines are).
 * The format itself is bnbf.h; decoding and encoding pixels is image_io.h.
 * =========================================================================== */

#ifndef BLUE_SERIALIZE_H
#define BLUE_SERIALIZE_H

#include "bnbf.h"                    /* the format: flags, reader, writer  */
#include "document.h"                /* OnDocument                          */
#include "db.h"                      /* OnDatabase, OnActionItem            */

/* ---------------------------------------------------------------------------
 * on_note_extract_text() — the searchable plain text of a BNBF blob
 * (on_document_plain_text: one line per block, list prefixes rendered,
 * table cells space-separated).  No image is decoded — the model carries
 * PNG bytes verbatim.  Used to (back)fill the notes.body_text column.
 * Returns a newly allocated string (g_free() it), or NULL when the blob is
 * malformed or from a newer format.
 * ------------------------------------------------------------------------- */
gchar *on_note_extract_text(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_note_text_cached() — one note's plain text, read the cheap way: the
 * notes.body_text cache when it is filled, otherwise extracted from the
 * blob with on_note_extract_text() (no images decoded) and written back so
 * the next reader hits the cache.  THE one implementation.
 *   db — open database (written to when the cache was empty).
 *   id — the note.
 * Returns a newly allocated string, "" for a note with no content, or NULL
 * when the note does not exist or cannot be read; g_free().
 * ------------------------------------------------------------------------- */
gchar *on_note_text_cached(OnDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * on_note_document_load() — note `id`'s stored content as an OnDocument
 * (document.h): the block model, no image decoded, its tag line at
 * block 0 (an empty one for a note with no content yet,
 * on_document_add_tag_line).  A note with no content is an empty
 * document.  THE preamble of every consumer: the
 * editor, the exporter, the headless action rewrites, the grid
 * thumbnail.  Returns the document (on_document_free()
 * it), or NULL when the note does not exist, the read failed, or its blob
 * is malformed or from a newer format — never an empty document in its
 * place, which a save would write over the note.
 * ------------------------------------------------------------------------- */
OnDocument *on_note_document_load(OnDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * on_note_document_save() — write `doc` back as note `id`: THE one save.
 * Every path that persists a document — the editor's autosave, a pull, a
 * new note from text — comes through here, so what a save consists of is
 * defined once, and it is ONE write transaction (the caller's, when one is
 * open on the connection — a pull — else its own):
 *   1. every real action line (on_document_action_blocks) gets its uid:
 *      a line with none is allocated one (on_db_action_uid_alloc), a uid
 *      the document carries twice (a pasted copy that smuggled its record)
 *      keeps it on the first line only — BEFORE the blob is written, since
 *      the blob is where the uid lives (the ACTION record);
 *   2. the blob, the title (first line) and the searchable body text go
 *      to on_db_note_save (which bumps updated_at);
 *   3. with `sync_actions`, THE PUSH (on_db_note_save_actions): new lines
 *      become tasks, a field of a line that differs from its seen_*
 *      baseline is written to the task (that field only), removed lines
 *      are tombstoned.  The editor passes whether an action line was
 *      touched since the last save; everyone else passes TRUE.
 * The uids step 1 allocated exist only if the transaction COMMITS: when it
 * does not — this save failed, or the caller's enclosing transaction rolls
 * back or fails to commit — every line that received one is set back to
 * uid 0 when the transaction ends (on_db_txn_on_end), so the next save
 * allocates again and no rolled-back uid is ever carried into a second
 * note.  `doc` must therefore outlive the transaction: inside a caller's
 * transaction, give it up with on_note_document_release, not
 * on_document_free.
 *   changed — optional; receives whether any task row was written (the
 *             library's full refresh hangs on it).
 * The note's attached files are settled in the same transaction: every
 * note_file row of the note that `doc` no longer names — in its blocks, or
 * in the undo/redo history that could bring a block back — is deleted
 * (on_db_note_files_keep).  A host that has just emptied the history
 * (on_document_clear_undo) therefore frees the files of deleted blocks
 * with its next save.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
gboolean on_note_document_save(OnDatabase *db, gint64 id, OnDocument *doc,
                               gboolean sync_actions, gboolean *changed);

/* ---------------------------------------------------------------------------
 * on_note_files_sweep() — delete the note_file rows that a note's STORED
 * content does not name: the safety net behind the saves, for the files an
 * editor discarded or a crash left (run at launch, and when an editor
 * closes without saving).  A note whose content cannot be read is skipped,
 * never swept.
 *   note_id — the note to sweep, or 0 for every note that owns a file.
 * Returns TRUE when every note it looked at was settled.
 * ------------------------------------------------------------------------- */
gboolean on_note_files_sweep(OnDatabase *db, gint64 note_id);

/* ---------------------------------------------------------------------------
 * on_note_document_save_tags() — rewrite note `id`'s TEXT tags from the
 * document's #tag runs (on_document_collect_tags →
 * on_db_note_set_text_tags, which makes missing tags; the old Tags field
 * is not touched).  The second half of a
 * save whose document's tags changed: the editor calls it when its tags
 * were touched, everything else after every save.
 *   db  — open database.
 *   id  — the note.
 *   doc — its document.
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
gboolean on_note_document_save_tags(OnDatabase *db, gint64 id,
                                    const OnDocument *doc);

/* ---------------------------------------------------------------------------
 * on_note_new_from_text() — a new note holding plain text: its tag line,
 * then one PARA block per line (on_document_from_text), the first its
 * title, every '!' line an action item, `tag` a #tag on the tag line —
 * created and written
 * through THE save, in ONE write transaction (the caller's, when one is
 * open): a failed save leaves no note behind.  What New Note, the
 * development seeder and the tests build their notes with (it was the
 * command-line tool's `note new`).
 *   db   — open database.
 *   tag  — a tag (any name) put on the tag line as a #tag
 *          (on_document_tag_word, on_document_append_tag), or NULL.
 *   text — the content, UTF-8 (invalid bytes become U+FFFD).
 * Returns the new note's id, or 0 on failure.
 * ------------------------------------------------------------------------- */
gint64 on_note_new_from_text(OnDatabase *db, const gchar *tag,
                             const gchar *text);

/* ---------------------------------------------------------------------------
 * on_note_new_from_document() — on_note_new_from_text for a document the
 * caller built (its first block the title, no tag line yet): the tag line
 * and `tag` are added, and the note is created and saved the same way, in
 * ONE write transaction (the caller's, when one is open).
 *   db  — open database.
 *   tag — a tag put on the tag line, or NULL.
 *   doc — the content; TAKEN (released with the transaction).
 * Returns the new note's id, or 0 on failure.
 * ------------------------------------------------------------------------- */
gint64 on_note_new_from_document(OnDatabase *db, const gchar *tag, OnDocument *doc);

/* ---------------------------------------------------------------------------
 * on_note_for_event() — THE note taken for an occurrence of a calendar
 * event (the day planner's double-click): the one made for it before,
 * while it is out of the Trash; else a new one — the event's title as its
 * title, then, when it has attendees, an "Attendees:" line, one bulleted
 * line each and an empty line, then an empty line, where the editor puts
 * the caret (NotesEditorRevealEnd) — bound to the occurrence
 * (on_db_event_note_bind).  A title starting with '!' is not made an
 * action item: the '!' prefix is dropped.  One write transaction.
 *   db          — open database.
 *   event_id    — the event's EventKit identifier.
 *   occurrence  — the occurrence's start, unix seconds (a recurring
 *                 event's occurrences share the identifier).
 *   title       — the title the planner shows (UTF-8).
 *   attendees   — the attendees' names (UTF-8), as the calendar lists
 *                 them; a line break in a name or the title becomes a
 *                 space.
 *   n_attendees — how many.
 *   created     — out: true when a new note was made, false when the
 *                 occurrence's note was found; may be NULL.
 * Returns the note's id, or 0 on failure (nothing is written then).
 * ------------------------------------------------------------------------- */
int64_t on_note_for_event(OnDatabase *db, const char *event_id, int64_t occurrence,
                          const char *title, const char *const *attendees,
                          size_t n_attendees, bool *created);

/* ---------------------------------------------------------------------------
 * on_note_document_release() — free a document that a save inside the
 * open write transaction may still refer to (its fresh uids are reset if
 * the transaction does not commit): freed when that transaction ends, or
 * at once when none is open.
 *   db  — open database.
 *   doc — the document (owned by the caller until now), or NULL.
 * ------------------------------------------------------------------------- */
void on_note_document_release(OnDatabase *db, OnDocument *doc);

/* ---------------------------------------------------------------------------
 * on_note_document_reconcile() — THE PULL into one document: bring note
 * `id`'s '!' lines in line with what other writers (Tasks, an agent) did to
 * their tasks, judged against the note_task seen_* baseline.  Must run
 * inside a write transaction (on_db_txn_begin) that then saves the
 * document when this asks for it, so the line, the baseline and the task
 * always move together.  For each note_task row of the note:
 *   - task deleted with seen_deleted = 0 (deleted elsewhere): its line, if
 *     the document has it, is UNMARKED — the leading '!' and the
 *     whitespace after it are deleted, repeatedly while the line still
 *     starts with '!'; the rest (text, a due suffix, its formatting) stays
 *     as a plain line and the block's uid is cleared — and the note_task
 *     row is deleted (its tombstone trigger finds the task deleted already);
 *   - task deleted with seen_deleted = 1, or live with seen_deleted = 1:
 *     nothing (Notes' own tombstone; a task restored elsewhere whose line
 *     is gone);
 *   - otherwise, in the order text, strike, due: where the task's value
 *     differs from seen_* (title vs seen_title byte for byte; status DONE vs
 *     seen_done; due vs seen_due), the line is rewritten to the task's
 *     value if it differs (on_document_action_text / _strike / _due: the
 *     '!', spacing, due suffix and strike of the line are kept; a due date
 *     is written " due YYYY-MM-DD"), and seen_* := the task's value (every
 *     byte of the title).  A title the line cannot take — empty; holding a
 *     line break (on_document_text_has_line_break: "\n", "\r", U+2029),
 *     U+FFFC, a NUL or invalid UTF-8 over its whole stored length; or text
 *     that would read back as something else, e.g. ending in "due <date>"
 *     — leaves the line as it is: seen_title is still set to the task's
 *     title, so the save's push (which the pull then asks for) writes the
 *     line's text back to the task.
 * Each rewrite is an undo group of the document.
 *
 * Inputs:
 *   db      — open database, inside a write transaction
 *   id      — the note
 *   doc     — its document (stored, or an open editor's)
 *   changed — optional; receives whether the document was rewritten
 *
 * Output: 1 when the note must now be saved (a line rewritten or a title
 *         refused), 0 when nothing more is needed, -1 on a database error
 *         (the caller rolls back).
 * ------------------------------------------------------------------------- */
gint on_note_document_reconcile(OnDatabase *db, gint64 id, OnDocument *doc,
                                gboolean *changed);

/* ---------------------------------------------------------------------------
 * on_note_count_images() — how many IMAGE records a BNBF blob holds, found
 * by the same cheap record walk as on_note_extract_text(): the PNG payloads
 * are skipped, never decoded.  The media view asks this of every note it
 * lists, so it must not pay per-image decode costs just to know the count.
 *   data — BNBF bytes (NULL is answered 0).
 *   len  — length of `data`.
 * Returns the number of embedded images, 0 for an unreadable blob.
 * ------------------------------------------------------------------------- */
gint on_note_count_images(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_note_image_nth_png() — the ENCODED bytes of one embedded image, taken
 * verbatim out of the blob, addressed by its position among the blob's
 * IMAGE records (the same 0-based ordinal on_note_count_images() counts, and
 * the order the editor's image anchors appear in); every other payload is
 * walked past.  A thumbnail decodes it capped (on_ui_thumbnail); a caller
 * that only wants to WRITE the image out uses the bytes as they are.
 *   data — BNBF bytes (NULL yields NULL).
 *   len  — length of `data`.
 *   ord  — 0-based image ordinal within the note.
 * Returns a new GBytes holding a copy of the payload (g_bytes_unref() it),
 * or NULL when the note has no such image.
 * ------------------------------------------------------------------------- */
GBytes *on_note_image_nth_png(const guint8 *data, gsize len, gint ord);

#endif /* BLUE_SERIALIZE_H */
