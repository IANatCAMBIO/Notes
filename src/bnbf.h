/* ===========================================================================
 * bnbf.h — the BNBF note format: flags, records, reader and writer
 *
 * GLib only — no GTK.  This is the half of the old serialize.[ch] that
 * describes BYTES, split out so the document model (document.[ch]) and its
 * headless tests can read and write notes without a toolkit.  serialize.c
 * (the GtkTextBuffer side, until the block model replaces it) drives the
 * same reader and writer.
 *
 * BNBF layout (all integers little-endian):
 *
 *   [4 bytes]  magic "BNBF"
 *   [u32]      format version (currently 5; 1–4 are still readable)
 *   ...records...
 *   [u8 0x00]  end marker
 *
 * Record types:
 *   [u8 0x01]  TEXT  : [u32 flags] [u32 byte_len] [byte_len UTF-8 bytes]
 *   [u8 0x02]  IMAGE : [u32 display_width]            (version >= 2 only)
 *                      [u32 png_len] [png_len bytes of PNG data]
 *   [u8 0x03]  TABLE : [u32 tflags]                   (version >= 4 only;
 *                                                      bit 0 = header row)
 *                      [u32 rows] [u32 cols]          (version >= 3)
 *                      rows*cols x ([u32 len] [len UTF-8 bytes]) row-major
 *   [u8 0x04]  CHECK : [u8 state]                     (version >= 5)
 *                      a task-list checkbox (0 = unchecked, 1 = checked)
 *
 * IMAGE records always hold the image at its ORIGINAL resolution; the
 * display_width field records how wide the user chose to show it in the
 * editor (0 = default thumbnail sizing).  Version 1 blobs lack the
 * display_width field.
 *
 * A TEXT record holds one "run": a maximal span of characters that all
 * share the same formatting flags.  Formatting is a bitmask (ON_FMT_*) so
 * the format is self-contained and easy to parse from any language.
 *
 * LINES.  Nothing in the format marks a paragraph: a line is the text
 * between two '\n' characters (which live inside TEXT records, and may sit
 * anywhere in one).  A paragraph style is a flag bit carried by every
 * character of its line INCLUDING the terminating newline.  Bulleted and
 * numbered lines start with a literal "\xe2\x80\xa2 " / "12. " prefix in
 * the text; a task line starts with a CHECK record followed by " ".
 * Images, tables and checkboxes occupy a character position on their line.
 * =========================================================================== */

#ifndef ON_BNBF_H
#define ON_BNBF_H

#include <glib.h>

/* ---------------------------------------------------------------------------
 * Formatting flag bits used in TEXT records.
 * ------------------------------------------------------------------------- */
typedef enum {
    ON_FMT_BOLD        = 1 << 0,   /* bold text                             */
    ON_FMT_ITALIC      = 1 << 1,   /* italic text                           */
    ON_FMT_UNDERLINE   = 1 << 2,   /* underlined text                       */
    ON_FMT_STRIKE      = 1 << 3,   /* strikethrough text                    */
    ON_FMT_H1          = 1 << 4,   /* heading level 1 (paragraph)           */
    ON_FMT_H2          = 1 << 5,   /* heading level 2 (paragraph)           */
    ON_FMT_CODEBLOCK   = 1 << 6,   /* monospace code block (paragraph)      */
    ON_FMT_LIST_BULLET = 1 << 7,   /* bulleted list item (paragraph)        */
    ON_FMT_LIST_NUMBER = 1 << 8,   /* numbered list item (paragraph)        */
    ON_FMT_TAG         = 1 << 9,   /* inline #tag token                     */
    ON_FMT_LIST_CHECK  = 1 << 10,  /* task-list item with checkbox (para)   */
} OnFormatFlags;

/* Format-bit groups: the four inline (character) styles, the six mutually
 * exclusive paragraph styles (whole lines only — at most one PARA bit is
 * ever set on a character), and the RUN mask — every bit that can vary
 * WITHIN a line, i.e. the inline styles plus the #tag marker.             */
#define ON_FMT_INLINE_MASK (ON_FMT_BOLD | ON_FMT_ITALIC | \
                            ON_FMT_UNDERLINE | ON_FMT_STRIKE)
#define ON_FMT_PARA_MASK   (ON_FMT_H1 | ON_FMT_H2 | ON_FMT_CODEBLOCK | \
                            ON_FMT_LIST_BULLET | ON_FMT_LIST_NUMBER | \
                            ON_FMT_LIST_CHECK)
#define ON_FMT_RUN_MASK    (ON_FMT_INLINE_MASK | ON_FMT_TAG)

/* The bullet prefix a bulleted line carries in its text: U+2022 + space.  */
#define ON_BULLET_PREFIX "\xe2\x80\xa2 "

/* on_list_prefix_chars() — length in CHARACTERS of the literal list
 * prefix at the start of `head` ("\xe2\x80\xa2 " bullet or "12. "), or 0
 * if none.  `head` is a short UTF-8 probe of the line start (callers pass
 * ~7 chars).  The one parser the editor (prefix stripping), the exporters
 * and the document loader use.                                            */
glong on_list_prefix_chars(const gchar *head);

/* ---------------------------------------------------------------------------
 * on_action_split_due() — locate a trailing "due <date>" in an action
 * item's rest-of-line text.  The date is ISO "YYYY-MM-DD" (what the app
 * writes) or "M/D/YY" / "M/D/YYYY"; the last word-boundary "due" whose
 * remainder parses wins.  The one parser both the extractor and the
 * editor's due-date rewriting use (like on_list_prefix_chars).
 *   rest      — the text after the line's '!' (NUL-terminated).
 *   due_start — receives the BYTE offset in `rest` of the "due" word
 *               (the item text ends before it, whitespace-trimmed).
 *   due       — receives local midnight of the date as a UNIX timestamp.
 * Returns TRUE when a due date was found and parsed.
 * ------------------------------------------------------------------------- */
gboolean on_action_split_due(const gchar *rest, gsize *due_start,
                             gint64 *due);

/* ---------------------------------------------------------------------------
 * OnTable — the cells of a TABLE record, as the reader hands them out and
 * as the GtkTextBuffer side keeps them on a table anchor.
 *
 * Fields:
 *   rows/cols — current dimensions.
 *   header    — whether the first row is styled/exported as a header.
 *   cells     — rows*cols owned strings, row-major (never NULL entries;
 *               cell text may contain newlines).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint       rows;
    gint       cols;
    gboolean   header;
    GPtrArray *cells;
} OnTable;

/* on_table_new() — a rows×cols table of empty cells (at least 1×1).       */
OnTable *on_table_new(gint rows, gint cols);

/* on_table_free() — release a table and its cell strings (NULL is a no-op). */
void on_table_free(OnTable *table);

/* on_table_get()/on_table_set() — cell access (row r, column c); out of
 * range reads "" and writes nothing.                                      */
const gchar *on_table_get(OnTable *table, gint r, gint c);
void on_table_set(OnTable *table, gint r, gint c, const gchar *text);

/* ---------------------------------------------------------------------------
 * READER — one cursor over a blob's records, so the header validation, the
 * per-record-type framing and every truncation check exist ONCE.  Every
 * consumer drives it: the document loader and the image-ordinal walks
 * (serialize.c).
 *
 * The reader never warns; it records why it stopped in `error` and lets
 * the caller decide (the deserializer reports, the extractor stops quietly).
 * ------------------------------------------------------------------------- */

/* Current format version the writer emits.  Version 2 added the
 * display_width field to IMAGE records; version 3 added TABLE records;
 * version 4 added the tflags field to TABLE records; version 5 added CHECK
 * records.  All older versions are still readable.                        */
#define ON_BNBF_VERSION 5u

/* Record type bytes.                                                      */
#define ON_REC_END   0x00            /* end of document                     */
#define ON_REC_TEXT  0x01            /* formatted text run                  */
#define ON_REC_IMAGE 0x02            /* inline PNG image                    */
#define ON_REC_TABLE 0x03            /* embedded table of text cells        */
#define ON_REC_CHECK 0x04            /* task-list checkbox                  */

typedef struct {
    const guint8 *data;              /* the blob                            */
    gsize         len;               /* its size                            */
    gsize         pos;               /* read cursor                         */
    guint32       version;           /* format version from the header      */
    gboolean      saw_end;           /* an ON_REC_END was reached           */
    const gchar  *error;             /* why the walk stopped, or NULL       */
} OnBnbfReader;

/* One record, as handed to the caller.  Only the fields belonging to
 * `type` are meaningful; `text` and `png` point INTO the blob.            */
typedef struct {
    guint8        type;              /* ON_REC_TEXT / IMAGE / TABLE / CHECK */
    guint32       flags;             /* TEXT: ON_FMT_* bits of the run      */
    const gchar  *text;              /* TEXT: run bytes (NOT terminated)    */
    guint32       n_text;
    const guint8 *png;               /* IMAGE: encoded bytes                */
    guint32       n_png;
    guint32       display_width;     /* IMAGE: stored display width (v2+)   */
    gboolean      checked;           /* CHECK: the box's state              */
    OnTable      *table;             /* TABLE: parsed; the CALLER owns it   */
} OnBnbfRecord;

/* on_bnbf_open() — validate the header and position at the first record.
 * Returns FALSE (with reader->error set) on a bad magic or version.       */
gboolean on_bnbf_open(OnBnbfReader *r, const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_bnbf_next() — read the next record, fully consuming it.
 * Returns FALSE at ON_REC_END (reader->saw_end set), when the blob runs
 * out, or on a malformed record (reader->error set).  A TABLE record
 * arrives with rec->table allocated; the caller frees it with
 * on_table_free().
 * ------------------------------------------------------------------------- */
gboolean on_bnbf_next(OnBnbfReader *r, OnBnbfRecord *rec);

/* ---------------------------------------------------------------------------
 * WRITER — the record framing, once.  Start with on_bnbf_writer_init (the
 * magic and current version), append records, finish to take the bytes.
 * Text runs are written exactly as given — the caller merges adjacent
 * same-flag runs if it wants maximal ones (the document serializer does).
 * ------------------------------------------------------------------------- */
typedef struct {
    GByteArray *out;                 /* the growing blob                    */
} OnBnbfWriter;

/* on_bnbf_writer_init() — a fresh blob with header written.               */
void on_bnbf_writer_init(OnBnbfWriter *w);

/* on_bnbf_write_text() — a TEXT record of `n` bytes under `flags`.  An
 * empty run (n == 0) writes nothing.                                      */
void on_bnbf_write_text(OnBnbfWriter *w, guint32 flags, const gchar *text,
                        gsize n);

/* on_bnbf_write_image() — an IMAGE record: the encoded bytes VERBATIM.    */
void on_bnbf_write_image(OnBnbfWriter *w, guint32 display_width,
                         const guint8 *png, gsize n_png);

/* on_bnbf_write_check() — a CHECK record.                                 */
void on_bnbf_write_check(OnBnbfWriter *w, gboolean checked);

/* on_bnbf_write_table_begin() / on_bnbf_write_table_cell() — a TABLE
 * record: the header, then EXACTLY rows*cols cells in row-major order
 * (the writer counts nothing; that is the caller's contract).             */
void on_bnbf_write_table_begin(OnBnbfWriter *w, gboolean header, gint rows,
                               gint cols);
void on_bnbf_write_table_cell(OnBnbfWriter *w, const gchar *text, gsize n);

/* on_bnbf_writer_finish() — append the end marker and hand over the bytes
 * (g_free() them); `out_len` receives their count.  The writer is spent. */
guint8 *on_bnbf_writer_finish(OnBnbfWriter *w, gsize *out_len);

#endif /* ON_BNBF_H */
