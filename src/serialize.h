/* ===========================================================================
 * serialize.h — the blob side of a note
 *
 * The blob-level helpers around a note: the body_text cache
 * (on_note_extract_text, derived through the model), the image ordinals
 * the media browser and the CLI address (on_note_count_images /
 * on_note_image_nth*), and the document's two ends —
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
#include "image_io.h"                /* CGImageRef, the one decoded type    */

/* ---------------------------------------------------------------------------
 * on_note_extract_text() — the searchable plain text of a BNBF blob
 * (on_document_plain_text: one line per block, list prefixes rendered,
 * table cells space-separated).  No image is decoded — the model carries
 * PNG bytes verbatim.  Used to (back)fill the notes.body_text column.
 * Returns a newly allocated string; g_free() it.
 * ------------------------------------------------------------------------- */
gchar *on_note_extract_text(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_note_text_cached() — one note's plain text, read the cheap way: the
 * notes.body_text cache when it is filled, otherwise extracted from the
 * blob with on_note_extract_text() (no images decoded) and written back so
 * the next reader hits the cache.  THE one implementation — the cross-note
 * search and the CLI both used to carry their own identical copy.
 *   db — open database (written to when the cache was empty).
 *   id — the note.
 * Returns a newly allocated string, "" for a note with no content; g_free().
 * ------------------------------------------------------------------------- */
gchar *on_note_text_cached(OnDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * on_note_document_load() — note `id`'s stored content as an OnDocument
 * (document.h): the block model, no image decoded.  An empty or
 * missing note is an empty document.  THE preamble of every offscreen
 * consumer: the exporter, the CLI's content commands, the headless action
 * rewrites, the grid thumbnail.  Returns the document; on_document_free()
 * it.
 * ------------------------------------------------------------------------- */
OnDocument *on_note_document_load(OnDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * on_note_document_save() — write `doc` back as note `id`: THE one save.
 * Every path that persists a document — the editor's autosave, the CLI's
 * content commands, a headless action rewrite — comes through here, so
 * what a save consists of is defined once:
 *   1. every real action line (on_document_action_blocks) gets its uid:
 *      a line with none is allocated one (on_db_action_uid_alloc), a uid
 *      the document carries twice (a pasted copy that smuggled its record)
 *      keeps it on the first line only — BEFORE the blob is written, since
 *      the blob is where the uid lives (the ACTION record);
 *   2. the blob, the title (first line) and the searchable body text go
 *      to on_db_note_save (which bumps updated_at);
 *   3. with `sync_actions`, the task rows are brought in line with the
 *      lines (on_db_note_save_actions: create / update / re-ord /
 *      tombstone).  The editor passes whether an action line was touched
 *      since the last save; everyone else passes TRUE.
 *   changed — optional; receives whether any task row was written (the
 *             library's full refresh hangs on it).
 * Returns TRUE on success.
 * ------------------------------------------------------------------------- */
gboolean on_note_document_save(OnDatabase *db, gint64 id, OnDocument *doc,
                               gboolean sync_actions, gboolean *changed);

/* ---------------------------------------------------------------------------
 * on_note_document_reconcile() — the task rows are the truth: for every
 * real action line of `doc` whose uid has a live task, re-render the line
 * from the row where the two differ (on_document_action_text / strike /
 * due).  Run on every load — an editor opening a note, a headless rewrite
 * about to touch one — so a task edited by the CLI, the Action Items view
 * or (after the merge) the Tasks app while the note was closed shows in
 * the note.  A blank task title is left alone: the line's text is not
 * something the document can hold as empty.
 * Returns TRUE when a line was rewritten (the caller then has an unsaved
 * document: the editor autosaves, a headless path saves).
 * ------------------------------------------------------------------------- */
gboolean on_note_document_reconcile(OnDatabase *db, gint64 id,
                                    OnDocument *doc);

/* ---------------------------------------------------------------------------
 * on_note_count_images() — how many IMAGE records a BNBF blob holds, found
 * by the same cheap record walk as on_note_extract_text(): the PNG payloads
 * are skipped, never decoded.  The media view asks this of every note in a
 * folder, so it must not pay per-image decode costs just to know the count.
 *   data — BNBF bytes (NULL is answered 0).
 *   len  — length of `data`.
 * Returns the number of embedded images, 0 for an unreadable blob.
 * ------------------------------------------------------------------------- */
gint on_note_count_images(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_note_image_nth() — decode ONE embedded image out of a BNBF blob,
 * addressed by its position among the blob's IMAGE records (the same 0-based
 * ordinal on_note_count_images() counts, and the same order the editor's
 * image anchors appear in).  Only that one PNG is decoded
 * (on_image_decode_capped); every other payload is walked past.
 *   data   — BNBF bytes (NULL yields NULL).
 *   len    — length of `data`.
 *   ord    — 0-based image ordinal within the note.
 *   max_px — cap on the longest side of the decoded image in pixels (0 =
 *            full resolution); never upscales.  A thumbnail passes its
 *            display size times the screen's scale factor.
 * Returns a new image (+1: CGImageRelease() it), or NULL when the note has
 * no such image or the payload will not decode.
 * ------------------------------------------------------------------------- */
CGImageRef on_note_image_nth(const guint8 *data, gsize len, gint ord,
                             gint max_px);

/* ---------------------------------------------------------------------------
 * on_note_image_nth_png() — the ENCODED bytes of one embedded image, taken
 * verbatim out of the blob: THE walk on_note_image_nth() decodes through, and
 * what a caller that only wants to WRITE the image out should use, since
 * decoding and re-encoding a PNG costs time and loses the original bytes.
 *   data — BNBF bytes (NULL yields NULL).
 *   len  — length of `data`.
 *   ord  — 0-based image ordinal within the note.
 * Returns a new GBytes holding a copy of the payload (g_bytes_unref() it),
 * or NULL when the note has no such image.
 * ------------------------------------------------------------------------- */
GBytes *on_note_image_nth_png(const guint8 *data, gsize len, gint ord);

#endif /* BLUE_SERIALIZE_H */
