/* ===========================================================================
 * serialize.h — the blob side of a note
 *
 * What reads or writes a note's BNBF blob WITHOUT building an OnDocument:
 * the record walks behind the body_text cache and the action_items mirror
 * (on_note_extract*), the image ordinals the media browser and the CLI
 * address (on_note_count_images / on_note_image_nth*), THE one PNG decoder
 * (on_png_decode_capped) and encoder (on_image_png_bytes), and
 * on_note_document_load — the preamble of every consumer that does want
 * the document (document.h).  The format itself is bnbf.h.
 * =========================================================================== */

#ifndef BLUE_SERIALIZE_H
#define BLUE_SERIALIZE_H

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "bnbf.h"                    /* the format: flags, reader, writer  */
#include "document.h"                /* OnDocument                          */
#include "db.h"                      /* OnDatabase, OnActionItem            */

/* ---------------------------------------------------------------------------
 * on_note_extract_text() — pull the searchable plain text out of a BNBF
 * blob WITHOUT building a document or decoding any images: TEXT
 * runs are concatenated, table cells are appended (space-separated), and
 * image/checkbox payloads are skipped.  Orders of magnitude cheaper than
 * a full deserialize; used to (back)fill the notes.body_text column.
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
 * (document.h): the block model, no GTK, no image decoded.  An empty or
 * missing note is an empty document.  THE preamble of every offscreen
 * consumer: the exporter, the CLI's content commands, the headless action
 * rewrites, the grid thumbnail.  Returns the document; on_document_free()
 * it.
 * ------------------------------------------------------------------------- */
OnDocument *on_note_document_load(OnDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * on_note_extract_actions() — pull the ACTION ITEMS out of a BNBF blob
 * without building a document (same cheap record walk as
 * on_note_extract_text).  An action item is a line whose first character
 * is '!' outside a code block (an embedded image/table/checkbox occupies
 * the first slot like any character, so such lines never qualify): its
 * text is the rest of the line, trimmed, and it is "done" when every
 * non-space character of that rest carries ON_FMT_STRIKE.  Lines with
 * nothing after the '!' are ignored.
 * Returns a GList of OnActionItem (db.h; ord = list position, note_id
 * left 0); free with on_db_action_list_free().
 * ------------------------------------------------------------------------- */
GList *on_note_extract_actions(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_note_extract() — the ONE record walk behind on_note_extract_text() and
 * on_note_extract_actions(), able to produce both in a single pass.  Every
 * save needs both (body_text cache + action mirror), and walking the blob
 * twice for them was pure duplication of a walk over the whole note.
 *   out_text    — receives the plain text (g_free), or NULL to skip it.
 *   out_actions — receives the action items (on_db_action_list_free), or
 *                 NULL to skip them.
 * An unreadable blob yields "" and no items, never a failure.
 * ------------------------------------------------------------------------- */
void on_note_extract(const guint8 *data, gsize len, gchar **out_text,
                     GList **out_actions);

/* ---------------------------------------------------------------------------
 * on_png_decode_capped() — decode one encoded image into a pixbuf,
 * shrinking it DURING decode to at most `max_px` on its longest side (0 =
 * full resolution; never upscales).  THE one decode path to a pixbuf: the
 * media view's thumbnails, on_note_image_nth() and the grid thumbnail all
 * come through here, so the size cap and the failure reporting exist once.
 * (The editor draws GdkTextures decoded straight from the bytes.)
 *   png    — encoded bytes (PNG as written by the serializer).
 *   n_png  — their length.
 * Returns a new pixbuf reference (g_object_unref() it), or NULL when the
 * payload will not decode.
 * ------------------------------------------------------------------------- */
GdkPixbuf *on_png_decode_capped(const guint8 *png, gsize n_png, gint max_px);

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
 * image anchors appear in).  Only that one PNG is decoded; every other
 * payload is walked past.
 *   data   — BNBF bytes (NULL yields NULL).
 *   len    — length of `data`.
 *   ord    — 0-based image ordinal within the note.
 *   max_px — cap on the longest side of the decoded pixbuf (0 = full
 *            resolution); never upscales.
 * Returns a new pixbuf reference (g_object_unref() it), or NULL when the
 * note has no such image or the payload will not decode.
 * ------------------------------------------------------------------------- */
GdkPixbuf *on_note_image_nth(const guint8 *data, gsize len, gint ord,
                             gint max_px);

/* ---------------------------------------------------------------------------
 * on_image_png_bytes() — THE PNG encoding of one attached image.
 *
 * A pixbuf that came out of a note carries its ORIGINAL bytes cached on it
 * as "on-png" (attached by the full-resolution load), and a pixbuf that did
 * not is encoded once and cached the same way.  So an image is compressed
 * at most once per session however many times it is written out — which is
 * the whole reason saving an image-heavy note is not a main-loop stall, and
 * why exporting one does not recompress what the database already holds.
 *   pixbuf — the image, as carried by an anchor (on_anchor_get_image).
 * Returns BORROWED bytes owned by the pixbuf — do not unref — or NULL when
 * the image could not be encoded.
 * ------------------------------------------------------------------------- */
GBytes *on_image_png_bytes(GdkPixbuf *pixbuf);

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

/* ---------------------------------------------------------------------------
 * on_png_probe_size() — an encoded image's pixel dimensions from its HEADER,
 * without decoding the pixels: the loader is fed only the first bytes and
 * then closed, so a 12 MP screenshot costs nothing to measure.
 *   png   — encoded bytes.
 *   n_png — their length.
 *   w/h   — receive the dimensions (either may be NULL).
 * Returns TRUE when the header parsed.
 * ------------------------------------------------------------------------- */
gboolean on_png_probe_size(const guint8 *png, gsize n_png, gint *w, gint *h);

#endif /* BLUE_SERIALIZE_H */
