/* ===========================================================================
 * document.h — OnDocument, the block model of a note
 *
 * GLib only — no GTK.  A note is an array of BLOCKS: paragraphs, headings,
 * list items, code lines, images and tables.  Each text block owns its
 * text and the RUNS of inline flags over it; the block boundary IS the
 * newline, so no text block contains one.  This is the shape BNBF
 * (bnbf.h) already has — a flat record stream whose lines carry a
 * paragraph style — made explicit, and it is what BLOCK_MODEL.md
 * specifies; read that first.
 *
 * Loading and saving are on_document_from_bnbf() / on_document_to_bnbf().
 * A blob the editor wrote round-trips BYTE-IDENTICAL; the loader
 * normalizes the few shapes the format allows but the model does not (a
 * table in the middle of a line, a check line without its box) and counts
 * each in an OnDocLoadReport so the step-1 scan can say what it changed.
 *
 * Every mutation of a document goes through the OPERATIONS (on_document_*
 * below): they keep the invariants, keep the undo log and notify the
 * observer.  Direct field writes are for constructors and the loader only.
 * =========================================================================== */

#ifndef ON_DOCUMENT_H
#define ON_DOCUMENT_H

#include <glib.h>

#include "bnbf.h"                    /* OnFormatFlags, OnTable              */

/* ---------------------------------------------------------------------------
 * BLOCKS
 * ------------------------------------------------------------------------- */
typedef enum {
    ON_BLOCK_PARA,                   /* body text                            */
    ON_BLOCK_H1,                     /* heading 1                            */
    ON_BLOCK_H2,                     /* heading 2                            */
    ON_BLOCK_BULLET,                 /* bulleted list item                   */
    ON_BLOCK_NUMBER,                 /* numbered list item                   */
    ON_BLOCK_CHECK,                  /* task item: `checked` is the state    */
    ON_BLOCK_CODE,                   /* ONE code line; consecutive CODE
                                        blocks are one visual code block     */
    ON_BLOCK_IMAGE,                  /* an image on a line of its own        */
    ON_BLOCK_TABLE,                  /* rows × cols of text cells            */
    ON_N_BLOCK_KINDS
} OnBlockKind;

/* One run of identically-styled text.  Runs TILE a text exactly.          */
typedef struct {
    gsize   len;                     /* bytes, never 0                       */
    guint32 flags;                   /* ON_FMT_RUN_MASK bits only            */
} OnRun;

/* An image INSIDE a line of text (see BLOCK_MODEL.md, "Inline images"):
 * the text carries U+FFFC at `offset`, this is what it stands for.        */
typedef struct {
    gsize    offset;                 /* byte offset of the U+FFFC            */
    GBytes  *png;                    /* the stored bytes, verbatim           */
    guint32  display_width;          /* 0 = default thumbnail sizing         */
} OnInlineImage;

/* The UTF-8 encoding of U+FFFC, the object replacement character.        */
#define ON_OBJ_CHAR     "\xef\xbf\xbc"
#define ON_OBJ_CHAR_LEN 3

/* Text of a text block, or of one table cell.                             */
typedef struct {
    GString *text;                   /* UTF-8; NO newline (cells excepted),
                                        NO list prefix, NO checkbox          */
    GArray  *runs;                   /* OnRun, adjacent equal flags merged   */
    GArray  *images;                 /* OnInlineImage, ascending offset;
                                        always empty in a cell               */
} OnText;

typedef struct {
    OnBlockKind kind;
    OnText     *text;                /* every kind but IMAGE and TABLE       */
    gboolean    checked;             /* CHECK                                */
    guint32     eol_flags;           /* ON_FMT_RUN_MASK bits the line's
                                        NEWLINE carries in the blob.  Pure
                                        fidelity: a newline's inline style
                                        shows nothing, but the editor tags
                                        it like any typed character, so it
                                        is remembered and written back      */
    /* IMAGE */
    GBytes     *png;                 /* the stored bytes, VERBATIM — never
                                        re-encoded                           */
    guint32     display_width;       /* 0 = default thumbnail sizing         */
    /* TABLE */
    gint        rows, cols;
    gboolean    header;              /* first row is a header row            */
    GPtrArray  *cells;               /* OnText*, rows*cols, row-major        */
    /* identity — NOT serialized, survives every edit of this block         */
    gint64      action_uid;          /* action_items.uid, 0 = none yet       */
} OnBlock;

/* ---------------------------------------------------------------------------
 * OnText
 * ------------------------------------------------------------------------- */

/* on_text_new() — empty text, no runs.                                    */
OnText *on_text_new(void);

/* on_text_free() — release a text, its runs and inline images.            */
void on_text_free(OnText *t);

/* on_text_copy() — a deep copy (images share their GBytes).               */
OnText *on_text_copy(const OnText *t);

/* ---------------------------------------------------------------------------
 * on_text_insert() — insert `n` bytes of `s` at byte offset `off` under
 * `flags`, splitting the run there; inline images after `off` shift.
 * `off` must be a character boundary <= the text length.  Newlines are
 * refused at the DOCUMENT level (on_document_insert_text) for block text;
 * a table cell may hold them.
 * ------------------------------------------------------------------------- */
void on_text_insert(OnText *t, gsize off, const gchar *s, gsize n,
                    guint32 flags);

/* on_text_append() — on_text_insert at the end.                            */
void on_text_append(OnText *t, const gchar *s, gsize n, guint32 flags);

/* ---------------------------------------------------------------------------
 * on_text_delete() — remove the `n` bytes at `off`: runs shrink, empty
 * ones go, equal neighbours merge, inline images inside the span are
 * dropped and those after it shift.  Both ends must be character
 * boundaries.
 * ------------------------------------------------------------------------- */
void on_text_delete(OnText *t, gsize off, gsize n);

/* ---------------------------------------------------------------------------
 * on_text_set_flags() — set (on) or clear (off) the bits of `mask` over the
 * `n` bytes at `off`; other bits are untouched.  Runs split and re-merge.
 * ------------------------------------------------------------------------- */
void on_text_set_flags(OnText *t, gsize off, gsize n, guint32 mask,
                       gboolean on);

/* on_text_flags_at() — the run flags of the character at byte `off`, or of
 * the last character when `off` is the text length (0 for empty text).   */
guint32 on_text_flags_at(const OnText *t, gsize off);

/* on_text_add_image() — record an inline image at `off`, where the text
 * must already hold U+FFFC (the loader inserts that first).  Takes its
 * own reference on `png`.                                                 */
void on_text_add_image(OnText *t, gsize off, GBytes *png,
                       guint32 display_width);

/* ---------------------------------------------------------------------------
 * BLOCK constructors / destructor
 * ------------------------------------------------------------------------- */

/* on_block_new_text() — an empty block of a text kind.                    */
OnBlock *on_block_new_text(OnBlockKind kind);

/* on_block_new_image() — an image block; takes its own reference on png. */
OnBlock *on_block_new_image(GBytes *png, guint32 display_width);

/* on_block_new_table() — a rows×cols table of empty cells (min 1×1).      */
OnBlock *on_block_new_table(gint rows, gint cols);

/* on_block_free() — release a block and everything it owns.               */
void on_block_free(OnBlock *b);

/* on_block_copy() — a deep copy, uid included.                            */
OnBlock *on_block_copy(const OnBlock *b);

/* on_block_kind_is_text() — PARA/H1/H2/BULLET/NUMBER/CHECK/CODE.          */
gboolean on_block_kind_is_text(OnBlockKind kind);

/* on_block_cell() — a table's cell (row r, column c); NULL out of range.  */
OnText *on_block_cell(const OnBlock *b, gint r, gint c);

/* on_block_kind_para_flag() — the ON_FMT_PARA_MASK bit a kind is stored
 * under (0 for PARA, IMAGE and TABLE).                                    */
guint32 on_block_kind_para_flag(OnBlockKind kind);

/* ---------------------------------------------------------------------------
 * DOCUMENT
 * ------------------------------------------------------------------------- */
typedef struct OnDocument OnDocument;

/* on_document_new() — an empty note: ONE empty PARA block.                */
OnDocument *on_document_new(void);

/* on_document_free() — release the document and every block.             */
void on_document_free(OnDocument *d);

/* on_document_n_blocks() / on_document_block() — the block array.        */
guint    on_document_n_blocks(const OnDocument *d);
OnBlock *on_document_block(const OnDocument *d, guint i);

/* on_document_check() — verify every invariant BLOCK_MODEL.md lists.
 * Returns TRUE when they hold; otherwise FALSE with the first failure
 * described in `why` (if given).  Cheap enough for tests to call after
 * every operation; the loader and saver call it in debug builds.         */
gboolean on_document_check(const OnDocument *d, GString *why);

/* ---------------------------------------------------------------------------
 * LOADING AND SAVING
 *
 * What the loader normalizes, counted per document:
 *   mixed_para     — a line whose runs disagree on the paragraph bit; the
 *                    first character's wins, as in the editor
 *   unknown_flags  — a run carried bits outside PARA|RUN; dropped
 *   bullet_no_prefix — a bulleted line without its "• " (it gains one)
 *   number_renumbered — a numbered line whose "N. " was not its position
 *   styled_prefix  — a list prefix carrying inline flags (they are dropped;
 *                    the saver writes a prefix with none)
 *   check_no_space — a CHECK record not followed by a space (gains one)
 *   check_no_tag   — a CHECK line whose text is not under LIST_CHECK
 *   check_no_box   — a LIST_CHECK line with no CHECK record (gains one)
 *   split_check    — a CHECK record mid-line: the line splits there
 *   split_table    — a TABLE record on a line with anything else: splits
 *   image_inline   — an IMAGE record sharing a line with text (kept as an
 *                    inline image — informational, round-trips exactly)
 *   cr_newlines    — a line ended in "\r\n", a lone "\r" or U+2029 (a
 *                    Windows paste): the editor and Pango break there, so
 *                    the model does too, and the saver writes "\n"
 *   run_split      — two consecutive TEXT records carried the same flags
 *                    (builds before 2026-08 wrote the title line as its
 *                    own record); the saver writes the maximal run
 *   old_version    — the blob's version is below ON_BNBF_VERSION (the
 *                    bytes differ in the header alone)
 * `error` is the reader's message when the blob was malformed; the
 * document holds what parsed before that point.
 * ------------------------------------------------------------------------- */
typedef struct {
    guint mixed_para, unknown_flags;
    guint bullet_no_prefix, number_renumbered, styled_prefix;
    guint check_no_space, check_no_tag, check_no_box, split_check;
    guint split_table, image_inline;
    guint run_split, cr_newlines, old_version;
    const gchar *error;
} OnDocLoadReport;

/* on_document_from_bnbf() — a document from a blob.  NULL/empty data is an
 * empty note.  `rep` is optional and is zeroed first.  Never returns NULL. */
OnDocument *on_document_from_bnbf(const guint8 *data, gsize len,
                                  OnDocLoadReport *rep);

/* on_document_to_bnbf() — the document as a new BNBF blob (g_free it);
 * `out_len` receives its size.  Adjacent same-flag runs — across block
 * boundaries too — merge into one TEXT record, so the output is the
 * maximal-run form the GtkTextBuffer serializer produces.                 */
guint8 *on_document_to_bnbf(const OnDocument *d, gsize *out_len);

/* on_document_load_report_is_clean() — no normalization counted, no
 * error, current version (image_inline does not count).                  */
gboolean on_document_load_report_is_clean(const OnDocLoadReport *rep);

/* ---------------------------------------------------------------------------
 * POSITIONS
 * ------------------------------------------------------------------------- */
typedef struct {
    guint block;                     /* index into the blocks               */
    gint  cell;                      /* row*cols+col inside a TABLE, -1
                                        elsewhere                            */
    gsize offset;                    /* BYTE offset into that text          */
} OnPos;

/* on_document_text_at() — the OnText a position addresses: the block's
 * text, or the cell's; NULL for an image block or an invalid position.   */
OnText *on_document_text_at(const OnDocument *d, OnPos pos);

/* ---------------------------------------------------------------------------
 * OPERATIONS — every mutation.  Each keeps the invariants, records its
 * inverse in the undo log and notifies the observer.  All return FALSE
 * (and do nothing) on an invalid argument; the tests assert on TRUE.
 *
 * Text ops take an OnPos and work on block text or a cell alike.  Block
 * ops take a block index.  A group (on_document_begin_group /
 * end_group, nestable) makes several ops one undo step; an op outside
 * any group is a step of its own.
 * ------------------------------------------------------------------------- */

/* on_document_insert_text() — `n` bytes of `s` at `pos` under `flags`
 * (ON_FMT_RUN_MASK bits).  Refuses a newline in block text; a cell may
 * hold one.                                                               */
gboolean on_document_insert_text(OnDocument *d, OnPos pos, const gchar *s,
                                 gsize n, guint32 flags);

/* on_document_delete_text() — the `n` bytes at `pos` (inline images in
 * the span go with it; undo brings them back).                            */
gboolean on_document_delete_text(OnDocument *d, OnPos pos, gsize n);

/* on_document_set_flags() — set or clear `mask` over the `n` bytes at
 * `pos`.                                                                  */
gboolean on_document_set_flags(OnDocument *d, OnPos pos, gsize n,
                               guint32 mask, gboolean on);

/* on_document_split_block() — cut text block `pos.block` at `pos.offset`:
 * the tail becomes a new block of the SAME kind right after it (list
 * continuation and "Enter on an empty item ends the list" are the
 * caller's policy, applied with set_kind afterwards).  The first half's
 * newline takes `eol_flags` (the inline flags the caller has armed); the
 * second keeps the original one.  `pos.cell` must be -1.                  */
gboolean on_document_split_block(OnDocument *d, OnPos pos,
                                 guint32 eol_flags);

/* on_document_join_blocks() — append text block i+1 to text block i and
 * remove it; block i keeps its kind and takes i+1's newline flags.       */
gboolean on_document_join_blocks(OnDocument *d, guint i);

/* on_document_set_kind() — change a text block's kind (text kinds only). */
gboolean on_document_set_kind(OnDocument *d, guint i, OnBlockKind kind);

/* on_document_set_checked() — a CHECK block's state.                     */
gboolean on_document_set_checked(OnDocument *d, guint i, gboolean checked);

/* on_document_set_eol_flags() — the inline flags on block i's newline.   */
gboolean on_document_set_eol_flags(OnDocument *d, guint i, guint32 flags);

/* on_document_insert_block() — put `block` at index i (<= n_blocks);
 * ownership passes to the document.                                       */
gboolean on_document_insert_block(OnDocument *d, guint i, OnBlock *block);

/* on_document_remove_block() — remove block i.  The last remaining block
 * cannot be removed (a document is never empty): FALSE.                  */
gboolean on_document_remove_block(OnDocument *d, guint i);

/* on_document_table_set_header() — a TABLE block's header-row flag.      */
gboolean on_document_table_set_header(OnDocument *d, guint i,
                                      gboolean header);

/* on_document_table_insert_row() / remove_row() / insert_col() /
 * remove_col() — reshape TABLE block i at row/column `at` (insert: 0..n
 * inclusive; remove: 0..n-1, never the last one).                         */
gboolean on_document_table_insert_row(OnDocument *d, guint i, gint at);
gboolean on_document_table_remove_row(OnDocument *d, guint i, gint at);
gboolean on_document_table_insert_col(OnDocument *d, guint i, gint at);
gboolean on_document_table_remove_col(OnDocument *d, guint i, gint at);

/* ---------------------------------------------------------------------------
 * UNDO — a log of inverse operations, grouped.
 * ------------------------------------------------------------------------- */

/* on_document_begin_group() / end_group() — bracket several ops into one
 * undo step; nests.  An unbalanced begin leaves the group open, which is
 * what a view does while typing: it ends the group on the pause.         */
void on_document_begin_group(OnDocument *d);
void on_document_end_group(OnDocument *d);

/* on_document_undo() / redo() — step the log; FALSE when there is
 * nothing to step to.  Undo closes an open group first.                  */
gboolean on_document_undo(OnDocument *d);
gboolean on_document_redo(OnDocument *d);
gboolean on_document_can_undo(const OnDocument *d);
gboolean on_document_can_redo(const OnDocument *d);

/* on_document_undo_depth() — how many undo steps the log holds (an open
 * group counts as one).  A host that remembers the depth at its last save
 * knows whether the document has changed since without comparing bytes. */
guint on_document_undo_depth(const OnDocument *d);

/* on_document_clear_undo() — forget the whole history (after a load).    */
void on_document_clear_undo(OnDocument *d);

/* ---------------------------------------------------------------------------
 * OBSERVER — what changed, for the layout.  Every callback is optional.
 * `block_changed(i)` — block i's content, kind, state or eol changed;
 * `blocks_inserted(i, n)` — n new blocks now sit at i (blocks after them
 * shifted); `blocks_removed(i, n)` — the n blocks at i are gone.
 * ------------------------------------------------------------------------- */
typedef struct {
    void (*block_changed)(OnDocument *d, guint i, gpointer data);
    void (*blocks_inserted)(OnDocument *d, guint i, guint n, gpointer data);
    void (*blocks_removed)(OnDocument *d, guint i, guint n, gpointer data);
} OnDocumentObserver;

void on_document_set_observer(OnDocument *d, const OnDocumentObserver *obs,
                              gpointer data);

/* ---------------------------------------------------------------------------
 * CHANGE FLAGS — what the host needs to know at save time, kept live by
 * the operations (never by scanning).  Each read clears its flag.
 *   tags    — an op created, changed or removed a run carrying ON_FMT_TAG
 *   actions — an op touched a block that was or became an action line,
 *             or changed the block structure
 * ------------------------------------------------------------------------- */
gboolean on_document_take_tags_modified(OnDocument *d);
gboolean on_document_take_actions_modified(OnDocument *d);

/* ---------------------------------------------------------------------------
 * DERIVED READS — computed from the blocks, nothing stored.
 * ------------------------------------------------------------------------- */

/* on_block_is_action() — a text block (not CODE) whose text starts with
 * '!' — the format contract on_note_extract_actions defines.             */
gboolean on_block_is_action(const OnBlock *b);

/* Titles derived from a note's first line are cut to this many characters
 * (the list views' sanity bound).                                         */
#define ON_TITLE_MAX_CHARS 80

/* ---------------------------------------------------------------------------
 * on_document_title() — the note's title: the first line that holds
 * anything (text, or an image/table), rendered as plain text and
 * whitespace-trimmed; `fallback` when that leaves nothing (no such line,
 * an object line, a blank line); cut to `max_chars` characters.  The
 * rule on_buffer_first_line applies to the GtkTextBuffer, so the two
 * agree on every note.  Returns a new string.
 * ------------------------------------------------------------------------- */
gchar *on_document_title(const OnDocument *d, const gchar *fallback,
                         glong max_chars);

/* ---------------------------------------------------------------------------
 * on_document_from_text() — a document of PARA blocks, one per line of
 * `text` (broken like the loader breaks lines: "\n", "\r\n", "\r",
 * U+2029).  What `note new` and `note set` build.  Never NULL.
 * ------------------------------------------------------------------------- */
OnDocument *on_document_from_text(const gchar *text);

/* ---------------------------------------------------------------------------
 * on_document_collect_tags() — the distinct #tag names in the note (runs
 * carrying ON_FMT_TAG, whitespace-trimmed, without the leading '#'), in
 * document order.  Returns a GList of new strings; g_list_free_full(list,
 * g_free).
 * ------------------------------------------------------------------------- */
GList *on_document_collect_tags(const OnDocument *d);

/* ---------------------------------------------------------------------------
 * ACTION ITEMS BY ORDINAL — the note's REAL action lines numbered in
 * document order: a non-CODE text block whose text starts with '!' and
 * whose rest is more than whitespace and a "due <date>" (bare "!" lines
 * and lines that are only a due date do not count — the extractor's
 * numbering, so ord n here is ord n in the action_items table).  Each
 * rewrite is one undo group and returns FALSE when there is no such item.
 * ------------------------------------------------------------------------- */

/* on_document_action_blocks() — block indices of the real action lines,
 * ord order.  Returns a new GArray of guint; g_array_unref() it.         */
GArray *on_document_action_blocks(const OnDocument *d);

/* on_document_action_strike() — strike (done) or un-strike everything
 * after the '!'.                                                          */
gboolean on_document_action_strike(OnDocument *d, gint ord, gboolean done);

/* on_document_action_due() — rewrite the "due <date>" suffix: any
 * existing one is removed, then " due YYYY-MM-DD" appended for a non-zero
 * `due` (local-midnight UNIX time); the appended text takes the item
 * text's strike state so a done item stays done.                         */
gboolean on_document_action_due(OnDocument *d, gint ord, gint64 due);

/* on_document_action_text() — replace the item TEXT, keeping the '!'
 * prefix, the line's own spacing and any trailing "due <date>"; the new
 * text is given the old text's strike state explicitly.  `text` must be
 * non-blank and newline-free (callers validate — see cli.c).             */
gboolean on_document_action_text(OnDocument *d, gint ord,
                                 const gchar *text);

/* on_document_plain_text() — the note as plain text: one line per block
 * (list prefixes rendered, U+FFFC dropped), table cells space-separated
 * on the table's line.  Returns a new string.                             */
gchar *on_document_plain_text(const OnDocument *d);

/* on_document_image_count() — every image, block and inline, in document
 * order: the ordinal the media browser addresses images by.              */
gint on_document_image_count(const OnDocument *d);

/* on_document_image_nth() — the `ord`-th image's bytes (borrowed) and
 * display width, or NULL when there is no such image.                    */
GBytes *on_document_image_nth(const OnDocument *d, gint ord,
                              guint32 *display_width);

#endif /* ON_DOCUMENT_H */
