/* ===========================================================================
 * note_view.c — the rich-text note engine (implementation)
 *
 * See note_view.h for the feature overview and the API.  The interesting
 * moving parts in here:
 *
 *   inline formatting  — a "current flags" model: the view tracks which
 *                        inline styles (bold/italic/…) are active and
 *                        enforces them on newly typed characters, exactly
 *                        like a word processor.
 *
 *   paragraph styles   — headings, lists and code blocks are GtkTextTags
 *                        applied to whole lines (including the newline).
 *                        List items additionally carry a literal "• " or
 *                        "1. " prefix; Enter continues the list and Enter
 *                        on an empty item ends it.
 *
 *   #tag capture       — typing '#' begins a capture: a popup offers
 *                        matching known tags; space (or Enter/click-away)
 *                        ends the tag; Escape cancels it.
 *
 *   derived looks      — the title line, the action-line tint and the
 *                        emoji padding are editor-only tags re-derived
 *                        from the text on every edit, never serialized.
 *
 *   history            — whole-buffer snapshots grouped by typing pause,
 *                        because GtkTextBuffer's own undo is text-only.
 *
 * Every mutation the engine makes funnels into note_view_edited(), which
 * (re)arms the undo group and emits "edited" — the host's cue to autosave.
 * The engine never references its host: what the host must know arrives
 * as a signal, what it must do goes through the API in note_view.h.
 * =========================================================================== */

#include "note_view.h"
#include "serialize.h"

#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <unistd.h>

/* Maximum number of suggestions shown in the tag popup.                    */
#define TAG_POPUP_MAX 8

/* Width reserved for the floating code-block copy button, and its inset
 * from the block's shaded top and right edges.                             */
#define CODE_BTN_MARGIN 4

/* Horizontal margin the code-block tag applies (see on_buffer_ensure_tags
 * in serialize.c) — the shaded background ends this far from the window
 * edge, and the copy button must sit inside it.                            */
#define CODEBLOCK_RIGHT_MARGIN 24

/* Left margin of code blocks: the default from on_buffer_ensure_tags,
 * and the widened variant that leaves room for painted line numbers
 * inside the block's shading.                                              */
#define CODEBLOCK_LEFT_MARGIN        24
#define CODEBLOCK_LEFT_MARGIN_NUMS   36

/* ---------------------------------------------------------------------------
 * OnNoteView — the engine's GtkTextView subclass and all of its state.
 *
 * Three things GtkTextView does through vfuncs in GTK4 are why this is a
 * subclass and not a set of signal handlers: painting inside the code
 * blocks (snapshot_layer: the line numbers over the text, and the shading
 * of EMPTY code lines under it, which GTK4's paragraph-background skips),
 * re-anchoring the floating copy links after a reflow (size_allocate), and
 * taking the #tag popover off the view before GtkTextView's dispose walks
 * the children it knows — gtk_text_view_dispose LOOPS FOREVER on a child
 * it cannot remove (measured in gtktextview.c 4.22.4: `while (first_child)
 * gtk_text_view_remove()` warns and never advances).
 *
 * Fields:
 *   app             — global application context (not owned): the settings
 *                     the engine renders from, the database for the #tag
 *                     popup's choices, the status line.
 *   buffer          — the view's own buffer (borrowed from the parent
 *                     class; cached because every function reads it).
 *   actions         — the "view." action group behind the image and table
 *                     context menus (owned).
 *   inline_flags    — ON_FMT_* bits applied to newly typed characters.
 *   typing_insert   — TRUE from the insert-text before-handler to the
 *                     after-handler for short (typed) insertions, so
 *                     on_cursor_moved() knows the cursor jump it sees
 *                     mid-insert comes from typing and must not adopt
 *                     the (still untagged) new character's style —
 *                     that would wipe a style armed with no selection.
 *   internal_change — nesting counter; >0 while *we* mutate the buffer
 *                     programmatically, so signal handlers know to ignore
 *                     the resulting insert/delete events.
 *   tag_start       — text mark placed just before a '#' while a tag is
 *                     being typed; NULL when no capture is active.
 *   tag_popup       — GtkPopover listing matching tags (lazily built),
 *                     parented to the view and pointing at the '#'.
 *                     Non-autohide, never focusable: the view keeps the
 *                     keyboard the whole time it is up.
 *   tag_listbox     — GtkListBox inside tag_popup holding suggestions.
 *   tag_choices     — OnTag* snapshot for the active capture — queried
 *                     once at '#', filtered in memory per keystroke.
 *   code_buttons    — floating "copy" links, one per code block, added
 *                     as OVERLAYS of the view in buffer coordinates
 *                     (upper-right corner of each block); clicks are
 *                     hit-tested by the view's click gesture, the hand
 *                     cursor is the label's own.
 *   code_button_pool — links no block uses right now, hidden.  An overlay
 *                     can NEVER be taken off a GtkTextView in GTK 4.22:
 *                     gtk_text_view_remove() walks only the anchored
 *                     children and warns "is not a child" for an overlay
 *                     (gtktextview.c; the removal that would work is in
 *                     the private GtkTextViewChild).  So a link whose
 *                     block went away is hidden and kept here, and the
 *                     next block that needs one takes it back.
 *   code_btn_idle   — idle source id for a pending code-button rebuild.
 *   scroll_on_allocate — an anchor (image, table) was just inserted:
 *                     the caret-follow scroll must run from the NEXT
 *                     size_allocate, after the layout has measured the new
 *                     child.  An idle is a race against GTK's own
 *                     validation idle (measured: the same insert scrolled
 *                     on one run and not on the next).
 *   scroll_idle     — idle source id of the deferred caret-follow scroll
 *                     (see on_buffer_changed).
 *   join_para       — the paragraph style (ON_FMT_PARA_MASK bits, 0 =
 *                     body) of the FIRST line of a deletion that joins
 *                     lines, captured by the delete-range before-handler
 *                     and re-asserted over the merged line by the
 *                     after-handler; -1 while no join is in flight.  A
 *                     join keeps the SECOND line's newline, and with it
 *                     that line's paragraph tag — so backspacing at the
 *                     start of an (emptied) code line would otherwise turn
 *                     the body line above into the code block.
 *   ctx_offset      — buffer offset of the image or table anchor the last
 *                     context menu was opened ON: the "img-*" and
 *                     "table-*" actions act on the thing under the last
 *                     right click, read back through anchor_at_offset()
 *                     when they run (the anchor may have moved or gone).
 *   tags_modified   — TRUE when an edit created, renamed or deleted a
 *                     styled #tag since the host last took the flag.
 *                     Maintained LIVE by the tag-capture/insert/delete
 *                     handlers, so a save knows — without any buffer
 *                     scan — whether note_tags and the library's tag
 *                     sidebar need updating.
 *   actions_modified — TRUE when an edit since the host last took the
 *                     flag (or the last load) could have created or
 *                     destroyed a '!' action line; the host's save skips
 *                     the blob walk when this is clear AND it holds no
 *                     items from before.
 *   action_marks    — ActionMark* — one GtkTextMark per action line,
 *                     carrying that item's stable uid.  Marks ride text
 *                     edits, so they are the only signal that identifies
 *                     an item whose text was REWORDED;
 *                     on_db_note_set_actions takes them as match hints.
 *   undo_stack      — past buffer snapshots, oldest first (see the
 *                     undo/redo section for the whole design).
 *   redo_stack      — snapshots undone and re-doable, oldest first.
 *   undo_current    — snapshot matching the last committed buffer state
 *                     (what Ctrl+Z returns FROM).
 *   undo_commit_source — pending group-commit timer id, 0 if none.
 *   undo_restoring  — TRUE while undo/redo rebuilds the buffer, so the
 *                     resulting mutations don't record history.
 *   undo_sentences  — sentence enders ('.'/'?') typed into the pending
 *                     undo group; the UNDO_MAX_SENTENCES-th commits the
 *                     group early (reset on every commit).
 * ------------------------------------------------------------------------- */
typedef struct UndoSnap UndoSnap;    /* defined in the undo/redo section    */

struct _OnNoteView {
    GtkTextView         parent_instance;

    OnApp              *app;
    GtkTextBuffer      *buffer;
    GSimpleActionGroup *actions;

    guint32             inline_flags;
    gboolean            typing_insert;
    gint                internal_change;

    GtkTextMark        *tag_start;
    GtkWidget          *tag_popup;
    GtkWidget          *tag_listbox;
    GList              *tag_choices;

    GSList             *code_buttons;
    GSList             *code_button_pool;
    guint               code_btn_idle;
    guint               scroll_idle;
    gboolean            scroll_on_allocate;
    gint                join_para;
    gint                ctx_offset;

    gboolean            tags_modified;
    gboolean            actions_modified;
    GPtrArray          *action_marks;

    GPtrArray          *undo_stack;
    GPtrArray          *redo_stack;
    UndoSnap           *undo_current;
    guint               undo_commit_source;
    gboolean            undo_restoring;
    gint                undo_sentences;
};

G_DEFINE_FINAL_TYPE(OnNoteView, on_note_view, GTK_TYPE_TEXT_VIEW)

/* The view's signals (see note_view.h for what each means to a host).      */
enum {
    SIG_EDITED,
    SIG_INLINE_FLAGS_CHANGED,
    SIG_IMAGE_ACTIVATED,
    SIG_CELL_CREATED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Which editor-only DERIVED passes note_view_rederive() should run over a
 * range.  None of these are ever serialized (see note_view_rederive).       */
typedef enum {
    RD_ACTION   = 1 << 0,            /* blue tint on '!' action lines       */
    RD_TITLE    = 1 << 1,            /* line 0's centering + heading size   */
    RD_EMOJI    = 1 << 2,            /* macOS emoji letter-spacing          */
    RD_CODE_TAG = 1 << 3,            /* strip #tag spans inside code blocks */
} OnRederive;

/* Forward declarations for callbacks referenced before their definition.   */
static void     tag_capture_end(OnNoteView *v, gboolean apply);
static void     note_view_scroll_to_caret(OnNoteView *v);
static void     note_view_scroll_after_layout(OnNoteView *v);
static void     action_retag_lines(OnNoteView *v, gint start_off,
                                   gint end_off);
static gboolean note_view_rederive(OnNoteView *v, gint start_off,
                                   gint end_off, guint what);
static void     tag_emoji_in_range(OnNoteView *v, gint start_off,
                                   gint end_off);
static void     title_line_sync(OnNoteView *v, gint start_off,
                                gint end_off);
static void     tag_popup_update(OnNoteView *v);
static void     renumber_list_block(OnNoteView *v, gint line);
static gboolean line_strip_list_prefix(OnNoteView *v,
                                       GtkTextIter *line_start);
static void     code_buttons_queue_rebuild(OnNoteView *v);
static void     attach_table_widget(OnNoteView *v,
                                    GtkTextChildAnchor *anchor);
static void     attach_checkbox_widget(OnNoteView *v,
                                       GtkTextChildAnchor *anchor);
static void     insert_checkbox_at(OnNoteView *v, gint at);
static void     undo_notify_change(OnNoteView *v);
static void     undo_commit_now(OnNoteView *v);
static gboolean strip_tags_in_code_blocks(OnNoteView *v, gint start_off,
                                          gint end_off);

/* ===========================================================================
 * small helpers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * note_view_edited() — the ONE funnel for every mutation the engine makes:
 * (re)arms the undo group (a no-op while restoring) and tells the host —
 * "edited" is its cue to queue an autosave.
 * ------------------------------------------------------------------------- */
static void
note_view_edited(OnNoteView *v)
{
    undo_notify_change(v);           /* every mutation passes through here,
                                        so this is also where undo groups
                                        start (no-op while restoring)       */
    g_signal_emit(v, signals[SIG_EDITED], 0);
}

/* note_view_flags_changed() — v->inline_flags changed: tell the host, whose
 * toolbar toggles mirror it.                                                */
static void
note_view_flags_changed(OnNoteView *v)
{
    g_signal_emit(v, signals[SIG_INLINE_FLAGS_CHANGED], 0);
}

/* lookup_tag() — fetch a named GtkTextTag from the buffer's tag table.      */
static GtkTextTag *
lookup_tag(GtkTextBuffer *buffer, const gchar *name)
{
    return gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(buffer), name);
}

/* ---------------------------------------------------------------------------
 * line_para_flags() — which ON_FMT_PARA_MASK style the line containing
 * `iter` carries (0 if plain body text).  Paragraph tags are applied to
 * whole lines, so testing the first character is sufficient; for an
 * empty line the newline position itself is tested.
 * ------------------------------------------------------------------------- */
static guint32
line_para_flags(GtkTextBuffer *buffer, const GtkTextIter *iter)
{
    GtkTextIter ls = *iter;          /* start of the line                   */
    gtk_text_iter_set_line_offset(&ls, 0);
    return on_flags_at_iter(buffer, &ls, ON_FMT_PARA_MASK);
}

/* ---------------------------------------------------------------------------
 * line_span() — compute [start, end) covering one whole line *including*
 * its trailing newline (so paragraph tags cover the newline and typing at
 * end-of-line inherits them).
 *   buffer — the buffer.
 *   line   — line number.
 *   start  — receives the line start.
 *   end    — receives the start of the next line (or buffer end).
 * ------------------------------------------------------------------------- */
static void
line_span(GtkTextBuffer *buffer, gint line, GtkTextIter *start,
          GtkTextIter *end)
{
    gtk_text_buffer_get_iter_at_line(buffer, start, line);
    *end = *start;
    if (!gtk_text_iter_ends_line(end))
        gtk_text_iter_forward_to_line_end(end);
    /* Step over the newline so it is part of the span.                     */
    if (!gtk_text_iter_is_end(end))
        gtk_text_iter_forward_char(end);
}

/* ===========================================================================
 * inline formatting
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_note_view_toggle_inline() — handle a bold/italic/underline/strike
 * request.  With a selection: toggle the tag across the selected range.
 * Without: flip the bit in inline_flags so upcoming typed text uses it.
 *   v    — the view.
 *   flag — which inline style to toggle.
 * ------------------------------------------------------------------------- */
void
on_note_view_toggle_inline(OnNoteView *v, OnFormatFlags flag)
{
    const gchar *tag_name = on_tag_name_for_flag(flag);
    if (tag_name == NULL)
        return;

    GtkTextIter sel_start, sel_end;  /* selection bounds, if any            */
    if (gtk_text_buffer_get_selection_bounds(v->buffer,
                                             &sel_start, &sel_end)) {
        /* Toggle over the selection: if the first selected char already
         * has the style, remove it everywhere; otherwise apply it.         */
        GtkTextTag *tag = lookup_tag(v->buffer, tag_name);
        gboolean has = gtk_text_iter_has_tag(&sel_start, tag);
        v->internal_change++;
        if (has)
            gtk_text_buffer_remove_tag(v->buffer, tag,
                                       &sel_start, &sel_end);
        else
            gtk_text_buffer_apply_tag(v->buffer, tag,
                                      &sel_start, &sel_end);
        v->internal_change--;
        v->inline_flags = has ? (v->inline_flags & ~(guint32)flag)
                               : (v->inline_flags | (guint32)flag);
        note_view_edited(v);
    } else {
        /* No selection: arm/disarm the style for upcoming typing.          */
        v->inline_flags ^= (guint32)flag;
    }
    note_view_flags_changed(v);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

guint32
on_note_view_inline_flags(OnNoteView *v)
{
    return v->inline_flags;
}

/* ===========================================================================
 * paragraph formatting (headings, lists, code blocks)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * line_strip_list_prefix() — if the line starting at `line_start` begins
 * with a literal list prefix ("• " or "12. "), delete it.
 *   v         — the view (for internal_change bookkeeping by caller).
 *   line_start — iterator at the line start; revalidated after deletion.
 * Returns TRUE if a prefix was removed.
 * ------------------------------------------------------------------------- */
static gboolean
line_strip_list_prefix(OnNoteView *v, GtkTextIter *line_start)
{
    /* Task lines: the prefix is a checkbox anchor (+ separating space).    */
    GtkTextChildAnchor *anchor =
        gtk_text_iter_get_child_anchor(line_start);
    if (anchor != NULL && on_anchor_is_checkbox(anchor, NULL)) {
        GtkTextIter nx = *line_start;      /* char after the anchor         */
        gtk_text_iter_forward_char(&nx);
        gint n = (gtk_text_iter_get_char(&nx) == ' ') ? 2 : 1;
        GtkTextIter del_end = *line_start;
        gtk_text_iter_forward_chars(&del_end, n);
        gint line = gtk_text_iter_get_line(line_start);
        gtk_text_buffer_delete(v->buffer, line_start, &del_end);
        gtk_text_buffer_get_iter_at_line(v->buffer, line_start, line);
        return TRUE;
    }

    GtkTextIter probe_end = *line_start;   /* end of the probe window       */
    /* A prefix is at most "9999. " — 6 chars; don't run past the line.     */
    for (gint i = 0; i < 7 && !gtk_text_iter_ends_line(&probe_end); i++)
        gtk_text_iter_forward_char(&probe_end);

    gchar *head = gtk_text_buffer_get_text(v->buffer, line_start,
                                           &probe_end, FALSE);
    glong strip_chars = on_list_prefix_chars(head);
    g_free(head);

    if (strip_chars == 0)
        return FALSE;

    GtkTextIter del_end = *line_start;     /* end of the prefix to delete   */
    gtk_text_iter_forward_chars(&del_end, (gint)strip_chars);
    gint line = gtk_text_iter_get_line(line_start);
    gtk_text_buffer_delete(v->buffer, line_start, &del_end);
    /* Deletion invalidated the iters; recompute the line start.            */
    gtk_text_buffer_get_iter_at_line(v->buffer, line_start, line);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * renumber_list_block() — rewrite the "N. " prefixes of the contiguous
 * numbered-list block containing `line` so they count 1, 2, 3…
 *   v   — the view.
 *   line — any line inside the block.
 * ------------------------------------------------------------------------- */
static void
renumber_list_block(OnNoteView *v, gint line)
{
    GtkTextTag *num_tag = lookup_tag(v->buffer, ON_TAGNAME_LIST_NUMBER);
    if (num_tag == NULL)
        return;

    /* Walk back to the first line of the block.                            */
    gint first = line;               /* first line of the numbered block    */
    while (first > 0) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(v->buffer, &it, first - 1);
        if (!gtk_text_iter_has_tag(&it, num_tag))
            break;
        first--;
    }

    /* Rewrite prefixes forward until the block ends.                       */
    gint total_lines =
        gtk_text_buffer_get_line_count(v->buffer);
    gint number = 1;                 /* the value to write on this line     */
    v->internal_change++;
    for (gint l = first; l < total_lines; l++, number++) {
        GtkTextIter ls;              /* line start                          */
        gtk_text_buffer_get_iter_at_line(v->buffer, &ls, l);
        if (!gtk_text_iter_has_tag(&ls, num_tag))
            break;

        line_strip_list_prefix(v, &ls);

        gchar *prefix = g_strdup_printf("%d. ", number);
        gint offset = gtk_text_iter_get_offset(&ls);
        gtk_text_buffer_insert(v->buffer, &ls, prefix, -1);

        /* Re-apply the list tag over the inserted prefix so the line stays
         * uniformly tagged.                                                */
        GtkTextIter ps, pe;          /* prefix span                         */
        gtk_text_buffer_get_iter_at_offset(v->buffer, &ps, offset);
        gtk_text_buffer_get_iter_at_offset(
            v->buffer, &pe, offset + (gint)g_utf8_strlen(prefix, -1));
        gtk_text_buffer_apply_tag(v->buffer, num_tag, &ps, &pe);
        g_free(prefix);
    }
    v->internal_change--;
}

/* selection_line_range() — the inclusive line range covered by the
 * selection, or the cursor's line twice when nothing is selected.  The
 * rule every paragraph-style operation uses.                                */
static void
selection_line_range(OnNoteView *v, gint *first_line, gint *last_line)
{
    GtkTextIter start, end;          /* selection (or cursor twice)         */
    if (!gtk_text_buffer_get_selection_bounds(v->buffer, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(
            v->buffer, &start,
            gtk_text_buffer_get_insert(v->buffer));
        end = start;
    }
    *first_line = gtk_text_iter_get_line(&start);
    *last_line  = gtk_text_iter_get_line(&end);
}

/* ---------------------------------------------------------------------------
 * apply_paragraph_format() — set the paragraph style of every line touched
 * by the selection (or the cursor line) to `flag`.
 *   v   — the view.
 *   flag — one of ON_FMT_H1/H2/CODEBLOCK/LIST_BULLET/LIST_NUMBER, or 0
 *          for plain body text.
 * ------------------------------------------------------------------------- */
static void
apply_paragraph_format(OnNoteView *v, guint32 flag)
{
    gint first_line, last_line;      /* affected line range                 */
    selection_line_range(v, &first_line, &last_line);

    v->internal_change++;
    for (gint l = first_line; l <= last_line; l++) {
        GtkTextIter ls, le;          /* line span incl. trailing newline    */
        line_span(v->buffer, l, &ls, &le);

        /* Clear every existing paragraph tag and any list prefix.          */
        for (gsize i = 0; i < on_n_flag_tags; i++)
            if (on_flag_tags[i].flag & ON_FMT_PARA_MASK)
                gtk_text_buffer_remove_tag_by_name(
                    v->buffer, on_flag_tags[i].tag_name, &ls, &le);
        if (line_strip_list_prefix(v, &ls))
            line_span(v->buffer, l, &ls, &le);

        if (flag == 0)
            continue;                /* body text: nothing more to do       */

        /* An empty LAST line has an empty span (no trailing newline), so
         * a heading/code tag would have nothing to hold onto and the
         * style would silently not stick.  Give the line its newline to
         * carry the tag and keep the cursor on the styled line.  (Lists
         * don't need this — their visible prefix is inserted below.)      */
        if (gtk_text_iter_equal(&ls, &le) &&
            (flag & (ON_FMT_H1 | ON_FMT_H2 | ON_FMT_CODEBLOCK))) {
            gtk_text_buffer_insert(v->buffer, &ls, "\n", -1);
            line_span(v->buffer, l, &ls, &le);
            gtk_text_buffer_place_cursor(v->buffer, &ls);
        }

        /* For lists, insert the visible prefix first.                      */
        if (flag == ON_FMT_LIST_BULLET) {
            gtk_text_buffer_insert(v->buffer, &ls, "\xe2\x80\xa2 ", -1);
            line_span(v->buffer, l, &ls, &le);
        } else if (flag == ON_FMT_LIST_CHECK) {
            insert_checkbox_at(v, gtk_text_iter_get_offset(&ls));
            line_span(v->buffer, l, &ls, &le);
        } else if (flag == ON_FMT_LIST_NUMBER) {
            /* Number is fixed up by renumber_list_block afterwards.        */
            gtk_text_buffer_insert(v->buffer, &ls, "1. ", -1);
            line_span(v->buffer, l, &ls, &le);
        }

        /* Apply the requested tag over the whole line.                     */
        gtk_text_buffer_apply_tag_by_name(
            v->buffer, on_tag_name_for_flag(flag), &ls, &le);
    }
    v->internal_change--;

    if (flag == ON_FMT_LIST_NUMBER)
        renumber_list_block(v, first_line);

    /* Becoming (or ceasing to be) a code block flips whether a '!' line
     * counts as an action item — re-derive the tint for the range.         */
    /* Styling a line can insert a newline or a list prefix, either of which
     * shifts lines under the passes.  Becoming (or ceasing to be) a code
     * block also flips whether a '!' line counts as an action item, and
     * whether line 0 still gets the derived title size: a style picked by
     * hand suppresses it, plain body text (¶) brings it back.  And text
     * inside a code block is never a #tag, so tag spans the range carried
     * into the block are stripped (the queued save drops their links).     */
    GtkTextIter rs, rls, rle;        /* range bounds                        */
    gtk_text_buffer_get_iter_at_line(v->buffer, &rs, first_line);
    line_span(v->buffer, last_line, &rls, &rle);
    if (note_view_rederive(v, gtk_text_iter_get_offset(&rs),
                        gtk_text_iter_get_offset(&rle),
                        RD_ACTION | RD_TITLE |
                        (flag == ON_FMT_CODEBLOCK ? RD_CODE_TAG : 0)))
        v->tags_modified = TRUE;

    note_view_edited(v);
    /* Code blocks may have appeared or vanished: refresh their buttons.
     * (Pure tag changes don't emit "changed", so this must be explicit.)   */
    code_buttons_queue_rebuild(v);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

/* ---------------------------------------------------------------------------
 * on_note_view_toggle_paragraph() — apply `flag` to the selected lines, or
 * revert them to body text when every one already carries it.  Shared by
 * the paragraph tool buttons/menu items and the Primary+M code-block
 * shortcut (all of them the host's "win.para").
 * ------------------------------------------------------------------------- */
void
on_note_view_toggle_paragraph(OnNoteView *v, guint32 flag)
{
    if (flag != 0) {
        gint first_line, last_line;  /* affected line range                 */
        selection_line_range(v, &first_line, &last_line);
        gboolean all_have = TRUE;    /* every line already this style?      */
        for (gint l = first_line; l <= last_line; l++) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(v->buffer, &it, l);
            if (line_para_flags(v->buffer, &it) != flag) {
                all_have = FALSE;
                break;
            }
        }
        if (all_have)
            flag = 0;                /* toggle off: back to body text       */
    }
    apply_paragraph_format(v, flag);
}

/* ---------------------------------------------------------------------------
 * handle_return_in_list() — implement list continuation on Enter.
 *
 * On a non-empty list line, Enter inserts a newline plus the next prefix
 * ("• " or "N. ") and keeps the list tag going.  On an empty list item,
 * Enter *ends* the list (removes the prefix and tag, leaving a plain
 * line).
 *   v — the view.
 * Returns TRUE if the key press was consumed (a list line was handled).
 * ------------------------------------------------------------------------- */
static gboolean
handle_return_in_list(OnNoteView *v)
{
    GtkTextIter cursor;              /* current insertion point             */
    gtk_text_buffer_get_iter_at_mark(v->buffer, &cursor,
                                     gtk_text_buffer_get_insert(v->buffer));

    guint32 para = line_para_flags(v->buffer, &cursor);
    if (para != ON_FMT_LIST_BULLET && para != ON_FMT_LIST_NUMBER &&
        para != ON_FMT_LIST_CHECK)
        return FALSE;

    const gchar *tag_name =          /* the list tag for this line type     */
        (para == ON_FMT_LIST_BULLET) ? ON_TAGNAME_LIST_BULLET
      : (para == ON_FMT_LIST_CHECK)  ? ON_TAGNAME_LIST_CHECK
                                     : ON_TAGNAME_LIST_NUMBER;
    gint line = gtk_text_iter_get_line(&cursor);

    /* Measure the line's content beyond its prefix.                        */
    GtkTextIter ls, le;              /* text-only line span                 */
    gtk_text_buffer_get_iter_at_line(v->buffer, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le))
        gtk_text_iter_forward_to_line_end(&le);
    gchar *text = gtk_text_buffer_get_text(v->buffer, &ls, &le, FALSE);

    /* Compute the character length of the prefix on this line.             */
    glong prefix_chars = 0;          /* prefix length in characters         */
    if (para == ON_FMT_LIST_CHECK) {
        /* The prefix is a checkbox anchor (+ separating space).            */
        GtkTextChildAnchor *a = gtk_text_iter_get_child_anchor(&ls);
        if (a != NULL && on_anchor_is_checkbox(a, NULL)) {
            GtkTextIter nx = ls;
            gtk_text_iter_forward_char(&nx);
            prefix_chars = (gtk_text_iter_get_char(&nx) == ' ') ? 2 : 1;
        }
    } else {
        /* "• " or "12. " — the shared parser, so this agrees with
         * line_strip_list_prefix and the exporter about what a prefix is.   */
        prefix_chars = on_list_prefix_chars(text);
    }
    /* Length in characters INCLUDING anchors (get_text drops them).        */
    gboolean item_empty =
        (glong)(gtk_text_iter_get_offset(&le) -
                gtk_text_iter_get_offset(&ls)) <= prefix_chars;

    v->internal_change++;
    if (item_empty) {
        /* Empty item: end the list here.                                   */
        GtkTextIter span_s, span_e;  /* full line incl. newline             */
        line_span(v->buffer, line, &span_s, &span_e);
        gtk_text_buffer_remove_tag_by_name(v->buffer, tag_name,
                                           &span_s, &span_e);
        gtk_text_buffer_get_iter_at_line(v->buffer, &ls, line);
        line_strip_list_prefix(v, &ls);
    } else {
        /* Continue the list: newline + next prefix, tagged.  A new task
         * item always starts unchecked.                                    */
        gint at = gtk_text_iter_get_offset(&cursor);
        if (para == ON_FMT_LIST_CHECK) {
            gtk_text_buffer_insert(v->buffer, &cursor, "\n", -1);
            GtkTextIter nl_s, nl_e;  /* the newline character               */
            gtk_text_buffer_get_iter_at_offset(v->buffer, &nl_s, at);
            gtk_text_buffer_get_iter_at_offset(v->buffer, &nl_e, at + 1);
            gtk_text_buffer_apply_tag_by_name(v->buffer, tag_name,
                                              &nl_s, &nl_e);
            insert_checkbox_at(v, at + 1);  /* anchor + space, tagged      */
        } else {
            gchar *next_prefix;      /* text inserted after the newline     */
            if (para == ON_FMT_LIST_BULLET) {
                next_prefix = g_strdup("\n\xe2\x80\xa2 ");
            } else {
                glong n = g_ascii_strtoll(text, NULL, 10);
                next_prefix = g_strdup_printf("\n%ld. ", n + 1);
            }
            gtk_text_buffer_insert(v->buffer, &cursor, next_prefix, -1);

            GtkTextIter ins_s, ins_e;    /* span of the inserted text       */
            gtk_text_buffer_get_iter_at_offset(v->buffer, &ins_s, at);
            gtk_text_buffer_get_iter_at_offset(
                v->buffer, &ins_e,
                at + (gint)g_utf8_strlen(next_prefix, -1));
            gtk_text_buffer_apply_tag_by_name(v->buffer, tag_name,
                                              &ins_s, &ins_e);
            g_free(next_prefix);

            if (para == ON_FMT_LIST_NUMBER)
                renumber_list_block(v, line);
        }
    }
    v->internal_change--;
    g_free(text);

    note_view_edited(v);
    return TRUE;
}

/* ===========================================================================
 * code blocks — floating per-block "copy" links
 *
 * Every contiguous code-block span gets a small "copy" link pinned to its
 * upper-right corner.  The links are plain GtkLabels added as OVERLAYS of
 * the text view, positioned in buffer coordinates: GTK allocates an overlay
 * at buffer_y + top_margin − scroll on every allocation (D6), so they ride
 * scrolling on their own and the top margin is never our business.  They
 * are repositioned whenever the buffer changes or the view reflows.  Each
 * carries a left-gravity GtkTextMark at its block's start as object data
 * "on-mark".
 *
 * A label has no click handling of its own, so the click comes from the
 * view's click gesture via code_link_at_view_pos(); the hand cursor is the
 * label's own (gtk_widget_set_cursor_from_name), since every GTK4 widget
 * owns its cursor.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * code_link_at_view_pos() — the "copy" link under a pointer position, or
 * NULL when that position is not on one.
 *   v     — the view.
 *   wx, wy — pointer position in the VIEW's widget coordinates (what a
 *            gesture on the view reports).
 *
 * The test is against the link's bounds computed relative to the view —
 * where GTK allocated it for the current scroll position — so it is
 * scroll-proof even though the links are POSITIONED in buffer coordinates.
 * ------------------------------------------------------------------------- */
static GtkWidget *
code_link_at_view_pos(OnNoteView *v, gdouble wx, gdouble wy)
{
    for (GSList *l = v->code_buttons; l != NULL; l = l->next) {
        GtkWidget *link = l->data;   /* one floating copy link              */
        graphene_rect_t bounds;      /* where it sits, view coordinates     */
        if (gtk_widget_compute_bounds(link, GTK_WIDGET(v), &bounds) &&
            graphene_rect_contains_point(&bounds,
                                         &GRAPHENE_POINT_INIT((float)wx,
                                                              (float)wy)))
            return link;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * code_copy_link_activate() — copy the whole code block whose start mark is
 * attached to `link` and post a status confirmation.
 * Returns TRUE when something was copied.
 * ------------------------------------------------------------------------- */
static gboolean
code_copy_link_activate(OnNoteView *v, GtkWidget *link)
{
    GtkTextMark *mark =              /* block-start mark on the link        */
        g_object_get_data(G_OBJECT(link), "on-mark");
    GtkTextTag *tag = lookup_tag(v->buffer, ON_TAGNAME_CODEBLOCK);
    if (mark == NULL || tag == NULL)
        return FALSE;

    GtkTextIter start;               /* block start                         */
    gtk_text_buffer_get_iter_at_mark(v->buffer, &start, mark);
    if (!gtk_text_iter_has_tag(&start, tag))
        return FALSE;                /* block vanished since last rebuild   */

    GtkTextIter end = start;         /* block end                           */
    gtk_text_iter_forward_to_tag_toggle(&end, tag);

    gchar *code = gtk_text_buffer_get_text(v->buffer, &start, &end, FALSE);
    g_strchomp(code);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(v)),
                           code);
    g_free(code);
    on_app_status(v->app, "Codeblock copied");
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * code_buttons_update_positions() — anchor every copy button to the
 * upper-right corner of its block, in BUFFER coordinates.
 *
 * Runs only when content or geometry changes (rebuild, size_allocate),
 * never on scroll: the overlays ride the scroll by themselves.  Each link
 * remembers the position it was last moved to ("on-x"/"on-y" object data,
 * 0/0 from the add) and is moved only when that changes —
 * gtk_text_view_move_overlay queues an allocation UNCONDITIONALLY
 * (gtktextviewchild.c), and since size_allocate is what queues this pass,
 * an unconditional move would re-allocate forever.
 * ------------------------------------------------------------------------- */
static void
code_buttons_update_positions(OnNoteView *v)
{
    gint view_width = gtk_widget_get_width(GTK_WIDGET(v));
    if (view_width <= 0)
        return;                      /* not allocated yet                   */

    for (GSList *l = v->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;    /* one floating copy button            */
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark == NULL)
            continue;

        gint bw;                     /* minimum width = what an overlay is
                                        allocated (gtktextviewchild.c)      */
        gtk_widget_measure(btn, GTK_ORIENTATION_HORIZONTAL, -1, &bw, NULL,
                           NULL, NULL);

        GtkTextIter it;              /* block start position                */
        gtk_text_buffer_get_iter_at_mark(v->buffer, &it, mark);
        gtk_text_iter_set_line_offset(&it, 0);   /* normalize to line start */

        /* Ask the layout for the whole display line's y-range: it starts
         * exactly where the paragraph background (the shading) starts —
         * unlike the character box, which sits below the above-line
         * spacing and drifted the button downward.                         */
        gint line_y, line_h;         /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(v), &it, &line_y, &line_h);

        /* Equal CODE_BTN_MARGIN insets from the shaded top/right edges.     */
        gint x = view_width - CODEBLOCK_RIGHT_MARGIN - bw - CODE_BTN_MARGIN;
        gint y = line_y + CODE_BTN_MARGIN;
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "on-x")) == x &&
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "on-y")) == y)
            continue;                /* already there: no re-allocation     */
        g_object_set_data(G_OBJECT(btn), "on-x", GINT_TO_POINTER(x));
        g_object_set_data(G_OBJECT(btn), "on-y", GINT_TO_POINTER(y));
        gtk_text_view_move_overlay(GTK_TEXT_VIEW(v), btn, x, y);
    }
}

/* ---------------------------------------------------------------------------
 * code_buttons_rebuild() — remove and recreate the floating copy buttons
 * to match the code blocks currently present in the buffer.
 * ------------------------------------------------------------------------- */
static void
code_buttons_rebuild(OnNoteView *v)
{
    GtkTextTag *tag = lookup_tag(v->buffer, ON_TAGNAME_CODEBLOCK);

    /* Fast path: this runs after EVERY buffer change, but the set of
     * blocks rarely changes while typing.  When the current buttons'
     * marks already sit exactly on the block starts, just reposition —
     * no widget churn.                                                     */
    if (tag != NULL && v->app->code_copy_buttons) {
        GArray *starts =             /* block start offsets, ascending      */
            g_array_new(FALSE, FALSE, sizeof(gint));
        GtkTextIter scan;
        gtk_text_buffer_get_start_iter(v->buffer, &scan);
        while (TRUE) {
            if (!gtk_text_iter_starts_tag(&scan, tag)) {
                if (!gtk_text_iter_forward_to_tag_toggle(&scan, tag))
                    break;
                if (!gtk_text_iter_starts_tag(&scan, tag))
                    continue;
            }
            gint off = gtk_text_iter_get_offset(&scan);
            g_array_append_val(starts, off);
            if (!gtk_text_iter_forward_to_tag_toggle(&scan, tag))
                break;
        }

        gboolean same =              /* do buttons match block starts?      */
            (guint)g_slist_length(v->code_buttons) == starts->len;
        if (same) {
            /* Buttons were prepended while scanning forward, so the list
             * is in REVERSE document order.                                */
            guint i = starts->len;
            for (GSList *l = v->code_buttons; same && l != NULL;
                 l = l->next) {
                GtkTextMark *mark =
                    g_object_get_data(G_OBJECT(l->data), "on-mark");
                GtkTextIter mi;
                gtk_text_buffer_get_iter_at_mark(v->buffer, &mi, mark);
                same = (i > 0) &&
                       gtk_text_iter_get_offset(&mi) ==
                           g_array_index(starts, gint, --i);
            }
        }
        g_array_free(starts, TRUE);
        if (same) {
            code_buttons_update_positions(v);
            return;
        }
    }

    /* Retire the old set into the pool (their marks die here; the widgets
     * cannot — see code_button_pool in the OnNoteView banner).              */
    for (GSList *l = v->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark != NULL)
            gtk_text_buffer_delete_mark(v->buffer, mark);
        g_object_set_data(G_OBJECT(btn), "on-mark", NULL);
        g_object_set_data(G_OBJECT(btn), "on-x", GINT_TO_POINTER(-1));
        gtk_widget_set_visible(btn, FALSE);
        v->code_button_pool = g_slist_prepend(v->code_button_pool, btn);
    }
    g_slist_free(v->code_buttons);
    v->code_buttons = NULL;

    /* The buttons can be disabled entirely in File → Settings….            */
    if (!v->app->code_copy_buttons)
        return;

    if (tag == NULL)
        return;

    /* Walk the buffer from span to span of the code-block tag.             */
    GtkTextIter it;                  /* scan position                       */
    gtk_text_buffer_get_start_iter(v->buffer, &it);
    while (TRUE) {
        if (!gtk_text_iter_starts_tag(&it, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
                break;
            if (!gtk_text_iter_starts_tag(&it, tag))
                continue;
        }

        /* One block starts here: its "copy" hyperlink overlay — a pooled
         * one shown again, else a new one.  A plain label: the click is
         * served by the view's gesture, only the hover cursor is the
         * label's own.                                                    */
        GtkWidget *link;
        if (v->code_button_pool != NULL) {
            link = v->code_button_pool->data;
            v->code_button_pool = g_slist_delete_link(
                v->code_button_pool, v->code_button_pool);
            gtk_widget_set_visible(link, TRUE);
        } else {
            link = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(link),
                "<span size=\"8192\" foreground=\"#0066cc\""
                " underline=\"single\">copy</span>");
            gtk_widget_set_cursor_from_name(link, "pointer");
            /* Added at the origin; code_buttons_update_positions moves it */
            gtk_text_view_add_overlay(GTK_TEXT_VIEW(v), link, 0, 0);
        }

        GtkTextMark *mark = gtk_text_buffer_create_mark(v->buffer, NULL,
                                                        &it, TRUE);
        g_object_set_data(G_OBJECT(link), "on-mark", mark);
        v->code_buttons = g_slist_prepend(v->code_buttons, link);

        /* Jump past this block and continue scanning.                      */
        if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
            break;
    }
    code_buttons_update_positions(v);
}

/* on_code_btn_idle() — deferred rebuild so the text layout has settled.     */
static gboolean
on_code_btn_idle(gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    v->code_btn_idle = 0;
    code_buttons_rebuild(v);
    return G_SOURCE_REMOVE;
}

/* code_buttons_queue_rebuild() — coalesce rebuild requests into one idle.   */
static void
code_buttons_queue_rebuild(OnNoteView *v)
{
    if (v->code_btn_idle == 0)
        v->code_btn_idle = g_idle_add(on_code_btn_idle, v);
}

/* ===========================================================================
 * OnNoteView — the vfuncs (see the type's banner at the top of the file)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * code_block_walk_start() — the first buffer line at or above the top of
 * the visible area, and — when it sits mid-block — how many code lines the
 * block already has above it, so numbering continues rather than restarts.
 *   v  — the view.
 *   tag — the code-block tag.
 *   it  — out: the line to start walking from (line offset 0).
 *   vis — out: the visible area in buffer coordinates.
 * Returns the number of the code line BEFORE `it` (0 at a block start).
 * ------------------------------------------------------------------------- */
static gint
code_block_walk_start(OnNoteView *v, GtkTextTag *tag, GtkTextIter *it,
                      GdkRectangle *vis)
{
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(v), vis);
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(v), it, vis->y, NULL);
    gtk_text_iter_set_line_offset(it, 0);
    if (!gtk_text_iter_has_tag(it, tag))
        return 0;
    gint first = gtk_text_iter_get_line(it);
    while (first > 0) {
        GtkTextIter prev;
        gtk_text_buffer_get_iter_at_line(v->buffer, &prev, first - 1);
        if (!gtk_text_iter_has_tag(&prev, tag))
            break;
        first--;
    }
    return gtk_text_iter_get_line(it) - first;
}

/* ---------------------------------------------------------------------------
 * on_note_view_snapshot_layer() — GtkTextView's hook for drawing under
 * and over the text, in BUFFER coordinates (GTK translates the snapshot by
 * the scroll offset before calling; the text itself is drawn under the
 * same translation).  Two jobs, both walking the visible code-block lines:
 *
 *   BELOW_TEXT — shade the EMPTY code lines.  The code tag's
 *   paragraph-background does the rest, but GTK4 does not paint a
 *   paragraph background on a line holding only its newline (measured on
 *   4.22 with a pixel probe; GTK3 did), so a blank line inside a block
 *   came up white.  The tag's own colour is used, so the two cannot differ.
 *
 *   ABOVE_TEXT — the line numbers INSIDE each code block: the block's
 *   left margin is widened (see note_view_apply_line_numbers) and the numbers
 *   are drawn onto that strip of the block's shading, right-aligned just
 *   before the code text.  Painted, not text — selection and copying can
 *   never include them.  Each block numbers from 1.
 * ------------------------------------------------------------------------- */
static void
on_note_view_snapshot_layer(GtkTextView *view, GtkTextViewLayer layer,
                            GtkSnapshot *snapshot)
{
    OnNoteView *v = ON_NOTE_VIEW(view);
    GtkTextTag *tag = lookup_tag(v->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag == NULL)
        return;
    if (layer == GTK_TEXT_VIEW_LAYER_ABOVE_TEXT && !v->app->code_line_numbers)
        return;

    GdkRectangle vis;                /* visible area in buffer coords       */
    GtkTextIter it;                  /* walking line iterator               */
    gint num = code_block_walk_start(v, tag, &it, &vis);

    GdkRGBA *shade = NULL;           /* the tag's paragraph background      */
    gint right_edge = 0;             /* where a text line's shading ends    */
    cairo_t *cr = NULL;              /* the number painter (ABOVE only)     */
    PangoLayout *layout = NULL;      /* renders the number strings          */
    if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT) {
        g_object_get(tag, "paragraph-background-rgba", &shade, NULL);
        if (shade == NULL)
            return;
        /* GTK shades a text line from its left margin to the view's width
         * less the RIGHT margin in force (the tag's when it sets one), so
         * the empty-line fill stops at the same edge.                     */
        gboolean tag_right;          /* does the tag set a right margin?    */
        gint right_margin;
        g_object_get(tag, "right-margin-set", &tag_right,
                     "right-margin", &right_margin, NULL);
        if (!tag_right)
            right_margin = gtk_text_view_get_right_margin(view);
        right_edge = vis.x + vis.width - right_margin;
    } else {
        cr = gtk_snapshot_append_cairo(snapshot,
            &GRAPHENE_RECT_INIT((float)vis.x, (float)vis.y,
                                (float)vis.width, (float)vis.height));
        layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), NULL);
        PangoFontDescription *fd =
            pango_font_description_from_string("monospace 9");
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }

    while (TRUE) {
        gint y, h;                   /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(v), &it, &y, &h);
        if (y > vis.y + vis.height)
            break;
        if (!gtk_text_iter_has_tag(&it, tag)) {
            num = 0;                 /* block ended: restart numbering      */
        } else {
            num++;
            GdkRectangle rect;       /* first char, buffer coords           */
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(v), &it, &rect);
            if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT) {
                if (gtk_text_iter_ends_line(&it) && right_edge > rect.x)
                    gtk_snapshot_append_color(snapshot, shade,
                        &GRAPHENE_RECT_INIT((float)rect.x, (float)y,
                            (float)(right_edge - rect.x), (float)h));
            } else {
                /* Grey gutter band behind the number, spanning the whole
                 * line range so adjacent lines tile seamlessly, just left
                 * of where the code text begins.                           */
                cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
                cairo_rectangle(cr, rect.x - 20, y, 16, h);
                cairo_fill(cr);
                gchar text[16];      /* the printed number                  */
                g_snprintf(text, sizeof text, "%d", num);
                pango_layout_set_text(layout, text, -1);
                gint tw, th;         /* rendered number size                */
                pango_layout_get_pixel_size(layout, &tw, &th);
                cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
                cairo_move_to(cr, rect.x - 6 - tw, rect.y + 1);
                pango_cairo_show_layout(cr, layout);
            }
        }
        if (!gtk_text_iter_forward_line(&it))
            break;
    }
    if (shade != NULL)
        gdk_rgba_free(shade);
    if (layout != NULL)
        g_object_unref(layout);
    if (cr != NULL)
        cairo_destroy(cr);
}

/* ---------------------------------------------------------------------------
 * on_note_view_size_allocate() — chain up, then present the #tag popover
 * (a popover child must be presented by its parent's size_allocate, which
 * GtkTextView does only for its own) and queue a re-anchoring of the copy
 * buttons: a width change moves their x, a reflow their line_y.  Queued,
 * not done here — moving an overlay queues another allocation, and the
 * idle's fast path moves nothing that has not changed.
 * ------------------------------------------------------------------------- */
static void
on_note_view_size_allocate(GtkWidget *widget, gint width, gint height,
                           gint baseline)
{
    GTK_WIDGET_CLASS(on_note_view_parent_class)->size_allocate(
        widget, width, height, baseline);

    OnNoteView *v = ON_NOTE_VIEW(widget);
    if (v->tag_popup != NULL)
        gtk_popover_present(GTK_POPOVER(v->tag_popup));
    if (v->scroll_on_allocate) {
        /* The layout has just been validated with the new anchored child
         * measured, so this scroll sees the caret line's true height.     */
        v->scroll_on_allocate = FALSE;
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(v),
                                     gtk_text_buffer_get_insert(v->buffer),
                                     0.08, FALSE, 0.0, 0.0);
    }
    code_buttons_queue_rebuild(v);
}

/* ---------------------------------------------------------------------------
 * note_view_apply_line_numbers() — widen/narrow the code-block left margin
 * (making room for the painted numbers inside the shading) and redraw.
 * The margin is written only when it differs: setting a tag property
 * invalidates the whole layout, and this runs on EVERY settings change.
 * ------------------------------------------------------------------------- */
static void
note_view_apply_line_numbers(OnNoteView *v)
{
    GtkTextTag *tag = lookup_tag(v->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag != NULL) {
        gint want = v->app->code_line_numbers ? CODEBLOCK_LEFT_MARGIN_NUMS
                                              : CODEBLOCK_LEFT_MARGIN;
        gint have;                   /* the margin the tag applies now      */
        g_object_get(tag, "left-margin", &have, NULL);
        if (have != want)
            g_object_set(tag, "left-margin", want, NULL);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

void
on_note_view_settings_changed(OnNoteView *v)
{
    code_buttons_queue_rebuild(v);   /* code_copy_buttons                   */
    note_view_apply_line_numbers(v); /* code_line_numbers                   */
    /* first_line_title: re-run the title pass over line 0 alone — the only
     * line the feature ever tags, so this both applies and clears it.      */
    title_line_sync(v, 0, 0);
}

/* ===========================================================================
 * images
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * image_effective_width() — the logical on-screen width an image should
 * be shown at: the stored choice, or the default thumbnail cap, never
 * wider than the original.
 * ------------------------------------------------------------------------- */
static gint
image_effective_width(GdkPixbuf *orig, gint display_width)
{
    gint w = gdk_pixbuf_get_width(orig);
    gint h = gdk_pixbuf_get_height(orig);
    if (display_width > 0)
        return MIN(display_width, w);

    /* Thumbnail mode: fit inside ON_IMAGE_THUMB_W × ON_IMAGE_THUMB_H
     * (aspect preserved), never upscaling.                                 */
    gint tw = MIN(w, ON_IMAGE_THUMB_W);
    if (h > 0 && h * tw > ON_IMAGE_THUMB_H * w)  /* still too tall at tw   */
        tw = w * ON_IMAGE_THUMB_H / h;
    return MAX(tw, 1);
}

/* ---------------------------------------------------------------------------
 * image_widget_new() — build the GtkPicture showing `orig` at
 * `display_width` logical pixels.  The picture holds the full-resolution
 * texture and is SIZED by a size request: an anchored child is allocated
 * its MINIMUM size (gtktextlayout.c add_child_attrs, 4.22.4), so with
 * can-shrink on (minimum 0) the request IS the box, and the texture is
 * scaled down into it — every source pixel still there for HiDPI.
 * (can-shrink OFF would make the minimum the texture's full size and the
 * request could only enlarge it.)  A click on it opens the modal viewer,
 * hence its hand cursor.
 * ------------------------------------------------------------------------- */
static GtkWidget *
image_widget_new(GdkPixbuf *orig, gint display_width)
{
    gint w    = gdk_pixbuf_get_width(orig);
    gint h    = gdk_pixbuf_get_height(orig);
    gint want = image_effective_width(orig, display_width);
    gint want_h = MAX(1, (gint)((gdouble)h * want / w));

    GdkTexture *tex = on_app_texture_for_pixbuf(orig);
    GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
    g_object_unref(tex);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(picture, want, want_h);
    gtk_widget_set_cursor_from_name(picture, "pointer");
    return picture;
}

/* anchor_clear_widgets() — remove every widget attached at `anchor` from
 * the view (before attaching a replacement).                                */
static void
anchor_clear_widgets(OnNoteView *v, GtkTextChildAnchor *anchor)
{
    guint n;                         /* how many are attached               */
    GtkWidget **widgets = gtk_text_child_anchor_get_widgets(anchor, &n);
    for (guint i = 0; i < n; i++)
        gtk_text_view_remove(GTK_TEXT_VIEW(v), widgets[i]);
    g_free(widgets);
}

/* ---------------------------------------------------------------------------
 * attach_image_widget() — (re)create the display widget for an
 * image-carrying anchor, removing any widget it already had.
 * ------------------------------------------------------------------------- */
static void
attach_image_widget(OnNoteView *v, GtkTextChildAnchor *anchor)
{
    gint dw;                         /* stored display width                */
    GdkPixbuf *orig = on_anchor_get_image(anchor, &dw);
    if (orig == NULL)
        return;

    /* Drop any existing display widget.                                    */
    anchor_clear_widgets(v, anchor);

    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(v),
                                      image_widget_new(orig, dw), anchor);
}

/* ---------------------------------------------------------------------------
 * note_view_attach_widgets() — walk the whole buffer and give every image,
 * checkbox and table anchor its widget (used right after loading a note).
 * ------------------------------------------------------------------------- */
static void
note_view_attach_widgets(OnNoteView *v)
{
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(v->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor == NULL)
            continue;
        if (on_anchor_is_checkbox(anchor, NULL))
            attach_checkbox_widget(v, anchor);
        else if (on_anchor_get_table(anchor) != NULL)
            attach_table_widget(v, anchor);
        else
            attach_image_widget(v, anchor);
    } while (gtk_text_iter_forward_char(&it));
}

/* ---------------------------------------------------------------------------
 * on_note_view_insert_image() — embed a full-resolution image at the cursor
 * as an anchor + HiDPI widget, shown at the default thumbnail width.
 *   v      — the view.
 *   pixbuf — full-resolution image (caller keeps its own reference).
 * ------------------------------------------------------------------------- */
void
on_note_view_insert_image(OnNoteView *v, GdkPixbuf *pixbuf)
{
    GtkTextIter cursor;              /* insertion point                     */
    gtk_text_buffer_get_iter_at_mark(v->buffer, &cursor,
                                     gtk_text_buffer_get_insert(v->buffer));
    v->internal_change++;
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(v->buffer, &cursor);
    on_anchor_set_image(anchor, pixbuf, 0);
    attach_image_widget(v, anchor);
    v->internal_change--;
    note_view_edited(v);
    note_view_scroll_after_layout(v);
}

/* ---------------------------------------------------------------------------
 * anchor_at_offset() — the child anchor (image or table) at buffer offset
 * `offset`, or NULL if that position holds none.
 * ------------------------------------------------------------------------- */
static GtkTextChildAnchor *
anchor_at_offset(OnNoteView *v, gint offset)
{
    GtkTextIter it;                  /* position of the anchor              */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, offset);
    return gtk_text_iter_get_child_anchor(&it);
}

/* ===========================================================================
 * image ORDINALS — the note's images numbered in buffer order
 *
 * That order is the app's ONE way of addressing a note's images: it is the
 * order on_note_count_images() walks the blob's IMAGE records in, so the
 * media browser, the editor's modal viewer and
 * on_editor_window_open_image() all agree on which picture is "image 3".
 * Keep these three walks the only definition of it.
 * =========================================================================== */

gint
on_note_view_image_count(OnNoteView *v)
{
    gint n = 0;                      /* images seen so far                  */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(v->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor != NULL && on_anchor_get_image(anchor, NULL) != NULL)
            n++;
    } while (gtk_text_iter_forward_char(&it));
    return n;
}

/* ---------------------------------------------------------------------------
 * image_nth_anchor() — the note's `ord`-th image anchor, or NULL when there
 * is no such image.  `offset_out` gets its buffer offset when non-NULL.
 * ------------------------------------------------------------------------- */
static GtkTextChildAnchor *
image_nth_anchor(OnNoteView *v, gint ord, gint *offset_out)
{
    if (ord < 0)
        return NULL;

    gint n = 0;                      /* images passed so far                */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(v->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor == NULL || on_anchor_get_image(anchor, NULL) == NULL)
            continue;
        if (n++ != ord)
            continue;
        if (offset_out != NULL)
            *offset_out = gtk_text_iter_get_offset(&it);
        return anchor;
    } while (gtk_text_iter_forward_char(&it));
    return NULL;
}

GdkPixbuf *
on_note_view_image_nth(OnNoteView *v, gint ord)
{
    GtkTextChildAnchor *anchor = image_nth_anchor(v, ord, NULL);
    return (anchor != NULL) ? on_anchor_get_image(anchor, NULL) : NULL;
}

/* ---------------------------------------------------------------------------
 * image_ord_at() — the ordinal of the image at buffer offset `offset`,
 * or -1 when that position holds no image.  The inverse of
 * image_nth_anchor(), for turning a click's offset into the number the modal
 * viewer addresses pictures by.
 * ------------------------------------------------------------------------- */
static gint
image_ord_at(OnNoteView *v, gint offset)
{
    gint n = 0;                      /* images passed so far                */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(v->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor == NULL || on_anchor_get_image(anchor, NULL) == NULL)
            continue;
        if (gtk_text_iter_get_offset(&it) == offset)
            return n;
        n++;
    } while (gtk_text_iter_forward_char(&it));
    return -1;
}

/* ---------------------------------------------------------------------------
 * image_at_view_pos() — the embedded image under a pointer position, or
 * NULL when that position is not inside one.
 *   v         — the view.
 *   wx, wy     — pointer position in the view's widget coordinates.
 *   offset_out — filled with the image anchor's buffer offset, optional.
 *   dw_out     — filled with the stored display width, optional.
 * Returns the anchor-owned full-resolution pixbuf (do not unref), or NULL.
 *
 * get_iter_at_position, NOT get_iter_at_location: the latter returns
 * CURSOR positions, so a position on the right half of an image resolves
 * to the gap after it — and with two adjacent images that gap IS the next
 * image's anchor, so the wrong image was picked.  Positions past the end
 * of a line resolve to the line-end iter, hence the one-character step
 * back; the closing rectangle test is what stops that step from claiming
 * the empty space beside an image.
 * ------------------------------------------------------------------------- */
static GdkPixbuf *
image_at_view_pos(OnNoteView *v, gdouble wx, gdouble wy,
                  gint *offset_out, gint *dw_out)
{
    gint bx, by;                     /* position in buffer coordinates      */
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(v),
                                          GTK_TEXT_WINDOW_WIDGET,
                                          (gint)wx, (gint)wy, &bx, &by);
    GtkTextIter it;                  /* iter under the pointer              */
    gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(v), &it, NULL, bx, by);

    if (gtk_text_iter_get_child_anchor(&it) == NULL &&
        !gtk_text_iter_backward_char(&it))
        return NULL;
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
    if (anchor == NULL)
        return NULL;

    gint dw;                         /* stored display width                */
    GdkPixbuf *orig = on_anchor_get_image(anchor, &dw);
    if (orig == NULL)
        return NULL;

    GdkRectangle rect;               /* the anchor character's own box      */
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(v), &it, &rect);
    if (bx < rect.x || bx >= rect.x + rect.width ||
        by < rect.y || by >= rect.y + rect.height)
        return NULL;

    if (offset_out != NULL)
        *offset_out = gtk_text_iter_get_offset(&it);
    if (dw_out != NULL)
        *dw_out = dw;
    return orig;
}

/* image_shown_full() — TRUE when an image with stored display width `dw`
 * is currently displayed at its full size rather than as a thumbnail.     */
static gboolean
image_shown_full(GdkPixbuf *orig, gint dw)
{
    return image_effective_width(orig, dw) >= gdk_pixbuf_get_width(orig);
}

/* ---------------------------------------------------------------------------
 * replace_image_display() — change the stored display width of the image
 * at `offset` and rebuild its widget.  No text changes, so this is purely
 * a presentation update plus an autosave (the width is persisted).
 * ------------------------------------------------------------------------- */
static void
replace_image_display(OnNoteView *v, gint offset, gint display_width)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(v, offset);
    if (anchor == NULL)
        return;
    GdkPixbuf *orig = on_anchor_get_image(anchor, NULL);
    if (orig == NULL)
        return;
    on_anchor_set_image(anchor, orig, display_width);
    attach_image_widget(v, anchor);
    note_view_edited(v);
    code_buttons_queue_rebuild(v);
}

/* ctx_image() — the full-resolution pixbuf of the image the last context
 * menu was opened on (v->ctx_offset), or NULL if none is there any more.
 * Anchor-owned: do not unref.                                               */
static GdkPixbuf *
ctx_image(OnNoteView *v)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(v, v->ctx_offset);
    return anchor != NULL ? on_anchor_get_image(anchor, NULL) : NULL;
}

/* The image context-menu actions ("view.img-*").  All act on ctx_image().   */

/* on_img_copy() — "Copy Image": put the full-resolution image on the
 * clipboard so it can be pasted outside the note.                           */
static void
on_img_copy(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnNoteView *v = user_data;       /* the view                            */
    GdkPixbuf *orig = ctx_image(v);
    if (orig == NULL)
        return;
    GdkTexture *tex = on_app_texture_for_pixbuf(orig);
    gdk_clipboard_set_texture(gtk_widget_get_clipboard(GTK_WIDGET(v)),
                              tex);
    g_object_unref(tex);
}

/* ---------------------------------------------------------------------------
 * on_note_image_open_external() — write `orig` (a full-resolution image)
 * to a temporary PNG file and hand it to an image viewer — the program
 * configured under Settings ("image_viewer" setting), or the platform
 * opener (macOS `open`, otherwise `xdg-open`) which launches the system's
 * default viewer.  Shared by the "Open" menu item and the host's modal
 * viewer's action link.
 * ------------------------------------------------------------------------- */
void
on_note_image_open_external(GdkPixbuf *orig)
{
    if (orig == NULL)
        return;

    GError *err = NULL;
    gchar  *path = NULL;             /* temporary PNG path                  */
    gint fd = g_file_open_tmp("blue-note-XXXXXX.png", &path, &err);
    if (fd < 0) {
        g_warning("cannot create temp image file: %s", err->message);
        g_clear_error(&err);
        return;
    }
    close(fd);
    if (!gdk_pixbuf_save(orig, path, "png", &err, NULL)) {
        g_warning("cannot write temp image file: %s", err->message);
        g_clear_error(&err);
        g_free(path);
        return;
    }

    gchar *viewer = on_app_config_get("image_viewer");
#ifdef __APPLE__
    const gchar *opener = "open";
#else
    const gchar *opener = "xdg-open";
#endif
    gchar *argv[] = {
        (viewer != NULL && *viewer != '\0') ? viewer : (gchar *)opener,
        path, NULL
    };
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &err)) {
        g_warning("cannot launch image viewer: %s", err->message);
        g_clear_error(&err);
    }
    g_free(viewer);
    g_free(path);
}

/* on_img_open() — "Open" context-menu item.                                */
static void
on_img_open(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnNoteView *v = user_data;       /* the view                            */
    GdkPixbuf *orig = ctx_image(v);
    if (orig != NULL)
        on_note_image_open_external(orig);
}

/* on_img_full() / on_img_thumb() — inline display size.                     */
static void
on_img_full(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnNoteView *v = user_data;       /* the view                            */
    GdkPixbuf *orig = ctx_image(v);
    if (orig != NULL)
        replace_image_display(v, v->ctx_offset,
                              gdk_pixbuf_get_width(orig));
}

static void
on_img_thumb(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnNoteView *v = user_data;       /* the view                            */
    replace_image_display(v, v->ctx_offset,
                          0);        /* 0 = the default thumbnail width     */
}

/* ---------------------------------------------------------------------------
 * image_menu() — the context-menu items for an embedded image, as a menu
 * model naming the "view.img-*" actions: Copy Image, Open, and
 * whichever of Display Full Size / Display as Thumbnail the image is not
 * already showing.  The one definition of that menu; on_view_pressed hands
 * it to gtk_text_view_set_extra_menu before the view opens its popup.
 *   shown_full — whether the image is displayed at full size now.
 * Returns a new model (unref it).
 * ------------------------------------------------------------------------- */
static GMenuModel *
image_menu(gboolean shown_full)
{
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Copy _Image", "view.img-copy");
    g_menu_append(menu, "_Open",       "view.img-open");
    if (shown_full)
        g_menu_append(menu, "Display as _Thumbnail", "view.img-thumb");
    else
        g_menu_append(menu, "Display _Full Size",    "view.img-full");
    return G_MENU_MODEL(menu);
}

/* ---------------------------------------------------------------------------
 * on_view_pressed() — the view's click gesture, in the CAPTURE phase so it
 * runs before GtkTextView's own bubble-phase gesture (which places the
 * caret, starts a drag, or opens the context menu).
 *
 * A context-menu press (right button, or Ctrl+click on macOS) on an
 * embedded image hands the view the image items as its extra menu — set
 * HERE, before the view builds its popup, and gtk_text_view_set_extra_menu
 * rebuilds the popup whenever the model changes — and remembers that image
 * as the context the "img-*" actions act on.  Off an image the extra menu
 * is cleared so a stale one cannot show.
 *
 * A plain single left click runs a code block's floating "copy" link, or
 * shows an embedded image in the modal viewer; either CLAIMS the sequence,
 * which is what keeps the view's gesture from moving the caret or starting
 * a drag underneath (the GTK3 handler returned TRUE for the same reason).
 * Modifiers keep their selection meanings, and the second and third
 * presses of a double click must not toggle again.
 * ------------------------------------------------------------------------- */
static void
on_view_pressed(GtkGestureClick *gesture, gint n_press, gdouble x,
                gdouble y, gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    GtkEventController *ctl = GTK_EVENT_CONTROLLER(gesture);
    GdkEvent *event = gtk_event_controller_get_current_event(ctl);

    if (n_press == 1 && event != NULL &&
        gdk_event_triggers_context_menu(event)) {
        gint offset;                 /* the image's buffer offset           */
        gint dw;                     /* stored display width                */
        GdkPixbuf *orig =            /* image the press landed on           */
            image_at_view_pos(v, x, y, &offset, &dw);
        GMenuModel *items = NULL;    /* the extra menu, or none             */
        if (orig != NULL) {
            v->ctx_offset = offset;
            items = image_menu(image_shown_full(orig, dw));
        }
        gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(v), items);
        g_clear_object(&items);
        return;                      /* the view opens its menu             */
    }

    if (n_press != 1 ||
        gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))
            != GDK_BUTTON_PRIMARY ||
        (gtk_event_controller_get_current_event_state(ctl) &
         (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) != 0)
        return;                      /* default handling                    */

    /* The copy links sit ON TOP of their block's shading, so they are
     * tested before anything that reads the text under the pointer.       */
    GtkWidget *link =                /* copy link under the pointer         */
        code_link_at_view_pos(v, x, y);
    if (link != NULL) {
        code_copy_link_activate(v, link);
        gtk_gesture_set_state(GTK_GESTURE(gesture),
                              GTK_EVENT_SEQUENCE_CLAIMED);
        return;
    }

    /* A click on an embedded image is reported to the host, which shows it
     * big in the shared modal viewer.  It used to toggle that image's INLINE
     * size between thumbnail and full instead; those two states are still on
     * its context menu, where they no longer compete with the obvious reading
     * of a click on a picture.                                              */
    gint offset;                     /* the image's buffer offset           */
    if (image_at_view_pos(v, x, y, &offset, NULL) != NULL) {
        g_signal_emit(v, signals[SIG_IMAGE_ACTIVATED], 0,
                      image_ord_at(v, offset));
        gtk_gesture_set_state(GTK_GESTURE(gesture),
                              GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

/* ===========================================================================
 * pasting images
 *
 * The clipboard read finishes on a later main-loop iteration, by which time
 * the view may have been destroyed with its window.  The completion holds a
 * WEAK reference: a view that is gone simply drops the image.  (A strong
 * reference would keep a disposed view alive with its host's signal
 * handlers still attached to it.)
 * =========================================================================== */

/* paste_ref_new() / paste_ref_take() — the weak reference a completion
 * carries; take resolves it (a new strong ref, or NULL) and frees it.       */
static GWeakRef *
paste_ref_new(OnNoteView *v)
{
    GWeakRef *ref = g_new(GWeakRef, 1);
    g_weak_ref_init(ref, v);
    return ref;
}

static OnNoteView *
paste_ref_take(GWeakRef *ref)
{
    OnNoteView *v = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    return v;
}

/* ---------------------------------------------------------------------------
 * on_paste_texture_read() — the clipboard image has arrived as a texture:
 * turn it into the pixbuf serialize.c's anchor API wants, via the PNG
 * bytes — that PNG is exactly what the note will store, and the pixbuf is
 * what the view holds.  GTK's own PNG/TIFF/JPEG loaders did the decode,
 * so a macOS screenshot (TIFF on the pasteboard) needs no pixbuf loader.
 * ------------------------------------------------------------------------- */
static void
on_paste_texture_read(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    OnNoteView *v = paste_ref_take(user_data);   /* our ref, or NULL      */
    GError *err = NULL;
    GdkTexture *tex = gdk_clipboard_read_texture_finish(
        GDK_CLIPBOARD(source), result, &err);
    if (tex == NULL) {
        g_warning("editor: cannot read clipboard image: %s", err->message);
        g_clear_error(&err);
        g_clear_object(&v);
        return;
    }
    if (v != NULL) {
        GBytes *png = gdk_texture_save_to_png_bytes(tex);
        GInputStream *in = g_memory_input_stream_new_from_bytes(png);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(in, NULL, &err);
        if (pixbuf != NULL) {
            on_note_view_insert_image(v, pixbuf);
            g_object_unref(pixbuf);
        } else {
            g_warning("editor: cannot decode pasted image: %s",
                      err->message);
            g_clear_error(&err);
        }
        g_object_unref(in);
        g_bytes_unref(png);
        g_object_unref(v);
    }
    g_object_unref(tex);
}

/* ---------------------------------------------------------------------------
 * on_paste_clipboard() — intercept Ctrl+V: if the clipboard holds an image
 * (e.g. a screenshot), embed it and swallow the default paste; otherwise
 * fall through so GTK pastes text / rich text normally.
 *
 * "Holds an image" is asked of the clipboard's advertised formats joined
 * with GDK's deserializers — the same test gdk_clipboard_read_texture_async
 * makes — so every image type GDK can turn into a texture counts (PNG, and
 * the TIFF macOS screenshots arrive as), and the decision is synchronous
 * even though the read is not: the signal is stopped here, the bytes come
 * later.  The GTK3 atom probing (Apple-private pasteboard types that
 * asserted inside GDK) has no GTK4 counterpart — GdkContentFormats already
 * carry only what GDK could name.
 * ------------------------------------------------------------------------- */
static void
on_paste_clipboard(GtkTextView *view, gpointer user_data)
{
    OnNoteView   *v  = user_data;    /* the view                            */
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(view));

    GdkContentFormats *formats =     /* advertised + what deserializes      */
        gdk_content_formats_union_deserialize_gtypes(
            gdk_content_formats_ref(gdk_clipboard_get_formats(cb)));
    gboolean image = gdk_content_formats_contain_gtype(formats,
                                                       GDK_TYPE_TEXTURE);
    gdk_content_formats_unref(formats);
    if (!image)
        return;                      /* text: GTK's default handler pastes */

    gdk_clipboard_read_texture_async(cb, NULL, on_paste_texture_read,
                                     paste_ref_new(v));
    /* Swallow the signal so the default handler doesn't also paste.        */
    g_signal_stop_emission_by_name(view, "paste-clipboard");
}

/* ===========================================================================
 * action items
 *
 * A line whose first character is '!' (outside code blocks) is an action
 * item; the rest of the line is its text (see on_note_extract_actions in
 * serialize.c — the one definition both this styling pass and the
 * library's Action Items view derive from).  The view tints such lines
 * blue with the editor-only "on-action" tag — never serialized, always
 * re-derived from the text, exactly like the emoji padding.  A DONE item
 * is one whose text is struck through (ON_TAGNAME_STRIKE), which DOES
 * serialize — that is how the checked state persists in the note.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * action_line_rest() — is buffer line `line` an action line?  On TRUE,
 * *ls and *le hold the whole line span (newline excluded) and *rs the
 * first character after the '!'.
 * ------------------------------------------------------------------------- */
static gboolean
action_line_rest(GtkTextBuffer *buffer, gint line, GtkTextIter *ls,
                 GtkTextIter *rs, GtkTextIter *le)
{
    gtk_text_buffer_get_iter_at_line(buffer, ls, line);
    if (gtk_text_iter_get_char(ls) != '!')
        return FALSE;                /* anchors read as U+FFFC: never '!'   */
    if (on_flags_at_iter(buffer, ls, ON_FMT_PARA_MASK) & ON_FMT_CODEBLOCK)
        return FALSE;                /* '!' inside code is just code        */
    *le = *ls;
    if (!gtk_text_iter_ends_line(le))
        gtk_text_iter_forward_to_line_end(le);
    *rs = *ls;
    gtk_text_iter_forward_char(rs);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * line_range_for_offsets() — the inclusive line range two buffer offsets
 * cover.  Every derived-tag pass works in lines, so the conversion happens
 * ONCE per pass rather than once per pass component.
 * ------------------------------------------------------------------------- */
static void
line_range_for_offsets(OnNoteView *v, gint start_off, gint end_off,
                       gint *first, gint *last)
{
    GtkTextIter it;                  /* offset → line resolution            */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, start_off);
    *first = gtk_text_iter_get_line(&it);
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, end_off);
    *last = gtk_text_iter_get_line(&it);
}

/* ---------------------------------------------------------------------------
 * action_retag_line() — re-derive the blue on-action styling for ONE line.
 * The single definition of that rule; the range walkers just loop it.
 * ------------------------------------------------------------------------- */
static void
action_retag_line(OnNoteView *v, gint line)
{
    GtkTextIter ls, rs, le;          /* line span (+ rest start)            */
    gboolean is = action_line_rest(v->buffer, line, &ls, &rs, &le);
    if (!is) {
        gtk_text_buffer_get_iter_at_line(v->buffer, &ls, line);
        le = ls;
        if (!gtk_text_iter_ends_line(&le))
            gtk_text_iter_forward_to_line_end(&le);
    }
    if (is)
        gtk_text_buffer_apply_tag_by_name(v->buffer, "on-action", &ls, &le);
    else
        gtk_text_buffer_remove_tag_by_name(v->buffer, "on-action",
                                           &ls, &le);
}

/* ---------------------------------------------------------------------------
 * action_retag_lines() — re-derive the blue on-action styling for every
 * line between the two buffer OFFSETS (their lines, inclusive).  Cheap
 * enough to run per edit: the affected range is normally one line.
 * ------------------------------------------------------------------------- */
static void
action_retag_lines(OnNoteView *v, gint start_off, gint end_off)
{
    gint first, last;                /* affected line range                 */
    line_range_for_offsets(v, start_off, end_off, &first, &last);
    for (gint line = first; line <= last; line++)
        action_retag_line(v, line);
}

/* ---------------------------------------------------------------------------
 * title_line_sync() — keep line 0 presented as the note's title, whatever
 * it happens to contain.  A retag pass over every line between the two
 * buffer OFFSETS (their lines, inclusive): LINE 0 is the title, always —
 * blank or not, freshly typed or promoted by deleting the line above it —
 * and gets the editor-only "on-title-center" and "on-title-size" tags;
 * every other line in range loses them (a paste or an Enter can carry them
 * in).  Both are DERIVED, like the emoji padding and the action tint: never
 * serialized, so the title look is presentation only.  Nothing is written
 * into the note, which is what lets the setting turn it off again.
 *
 * The whole thing is gated on the first_line_title setting; with it off
 * every line is treated as body text (line 0 still SUPPLIES the note title,
 * that is just on_buffer_first_line reading the buffer).  Since the setting
 * applies live, the off path must actively CLEAR the tags rather than skip
 * the pass — see on_note_view_settings_changed.
 *
 * The SIZE tag is skipped when line 0 already carries a paragraph style of
 * its own: GTK MULTIPLIES the scale of every tag on a character
 * (_gtk_text_attributes_fill_from_tags), so stacking 1.6 onto a real H1
 * would render the line at 2.56x.  An H1 line 0 is already title-sized; an
 * H2, code-block or list line 0 is a style the user picked by hand.
 *
 * GTK reads paragraph properties from the line's FIRST character, which
 * decides how far each span reaches: a line with text is covered up to (not
 * including) its newline — covering the newline would buy nothing and would
 * let the next line inherit the title look from text typed right after it —
 * while an EMPTY line is covered through its newline, the only character it
 * has that can carry a tag (or hold a leaked one, which is why the clearing
 * branch follows the same rule).
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * title_view_sync() — the VIEW-level half of the title look, which applies
 * to the whole widget rather than a line: justification and the font-size
 * class that style the caret while the buffer is empty (see the banner).
 * ------------------------------------------------------------------------- */
static void
title_view_sync(OnNoteView *v)
{
    gboolean on    = v->app->first_line_title;   /* feature gate          */
    gboolean empty = gtk_text_buffer_get_char_count(v->buffer) == 0;

    /* A completely empty buffer has no character at all, so no tag can
     * apply and the caret on line 0 can only be styled by the VIEW's own
     * defaults — justification here, font size via the style class below.
     * Both are exact rather than a compromise: an empty buffer IS line 0
     * and nothing else, so there is no body text to catch the fallout.
     * Any content flips them back, and the tags take over.                 */
    GtkJustification want = (on && empty) ? GTK_JUSTIFY_CENTER
                                          : GTK_JUSTIFY_LEFT;
    if (gtk_text_view_get_justification(GTK_TEXT_VIEW(v)) != want)
        gtk_text_view_set_justification(GTK_TEXT_VIEW(v), want);

    GtkWidget *view = GTK_WIDGET(v);
    if ((on && empty) != gtk_widget_has_css_class(view, "on-title-empty")) {
        if (on && empty)
            gtk_widget_add_css_class(view, "on-title-empty");
        else
            gtk_widget_remove_css_class(view, "on-title-empty");
    }
}

/* ---------------------------------------------------------------------------
 * title_sync_line() — the per-line half: give line 0 the derived title tags
 * and take them off every other line in range.  Single definition of that
 * rule (see the title_line_sync banner for why the spans differ between an
 * empty line and one with text).
 * ------------------------------------------------------------------------- */
static void
title_sync_line(OnNoteView *v, gint line)
{
    gboolean on = v->app->first_line_title;      /* feature gate          */
    GtkTextIter ls, le;              /* line span (see banner)              */
    gtk_text_buffer_get_iter_at_line(v->buffer, &ls, line);
    le = ls;
    if (gtk_text_iter_ends_line(&le))
        gtk_text_iter_forward_char(&le);          /* empty line: its '\n'   */
    else
        gtk_text_iter_forward_to_line_end(&le);

    gboolean title = on && line == 0;
    if (title)
        gtk_text_buffer_apply_tag_by_name(v->buffer, "on-title-center",
                                          &ls, &le);
    else
        gtk_text_buffer_remove_tag_by_name(v->buffer, "on-title-center",
                                           &ls, &le);
    /* Size only where no paragraph style of its own would multiply it.      */
    if (title && on_flags_at_iter(v->buffer, &ls, ON_FMT_PARA_MASK) == 0)
        gtk_text_buffer_apply_tag_by_name(v->buffer, "on-title-size",
                                          &ls, &le);
    else
        gtk_text_buffer_remove_tag_by_name(v->buffer, "on-title-size",
                                           &ls, &le);
}

static void
title_line_sync(OnNoteView *v, gint start_off, gint end_off)
{
    title_view_sync(v);
    gint first, last;                /* affected line range                 */
    line_range_for_offsets(v, start_off, end_off, &first, &last);
    for (gint line = first; line <= last; line++)
        title_sync_line(v, line);
}

/* ---------------------------------------------------------------------------
 * note_view_rederive() — run the editor-only DERIVED passes over one range.
 *
 * None of these are stored in the note: the action tint, the title look and
 * the emoji padding are re-derived from the text every time it changes, and
 * #tag spans have no business inside a code block.  Every edit needs some
 * combination of them, and they were spelled out separately at four call
 * sites — easy to update three and forget the fourth.  This is the one
 * place that knows the set, and it resolves the line range once instead of
 * once per pass.
 *   what — which passes to run (OnRederive bits).
 * Returns TRUE when RD_CODE_TAG actually stripped a #tag span, which means
 * the note's tag set changed and the next save must rewrite note_tags.
 * ------------------------------------------------------------------------- */
static gboolean
note_view_rederive(OnNoteView *v, gint start_off, gint end_off, guint what)
{
    if (what & RD_EMOJI)
        tag_emoji_in_range(v, start_off, end_off);
    if (what & RD_TITLE)
        title_view_sync(v);

    if (what & (RD_ACTION | RD_TITLE)) {
        gint first, last;            /* affected line range, resolved once  */
        line_range_for_offsets(v, start_off, end_off, &first, &last);
        for (gint line = first; line <= last; line++) {
            if (what & RD_ACTION)
                action_retag_line(v, line);
            if (what & RD_TITLE)
                title_sync_line(v, line);
        }
    }

    return (what & RD_CODE_TAG)
           ? strip_tags_in_code_blocks(v, start_off, end_off) : FALSE;
}

/* ---------------------------------------------------------------------------
 * action_rest_real() — does an action line's rest-of-line text hold a
 * REAL item?  Bare "!" lines and lines that are nothing but a
 * "due <date>" suffix don't count — matching the extractor, so ord
 * numbering stays aligned between the table and the buffer walks.
 * ------------------------------------------------------------------------- */
static gboolean
action_rest_real(const gchar *rest)
{
    gchar *t = g_strdup(rest);
    gsize  due_start;                /* where the text part ends            */
    gint64 due;
    if (on_action_split_due(t, &due_start, &due))
        t[due_start] = '\0';
    gboolean real = *g_strstrip(t) != '\0';
    g_free(t);
    return real;
}

/* ===========================================================================
 * action-item identity marks
 *
 * An action item's stable uid lives in the action_items table, not in the
 * note text — so when a save rebuilds that table, something has to say
 * which new '!' line is which old item.  Identical text answers that for
 * an item that merely moved, but NOT for one whose text was reworded,
 * which is exactly the case an external mirror (the Lists app) depends
 * on.  A GtkTextMark rides arbitrary edits to the text around it, so one
 * mark per action line, tagged with that item's uid, survives any amount
 * of rewording and any number of lines inserted above.
 *
 * Marks are pruned when their text is DELETED (see action_marks_prune):
 * GtkTextBuffer would otherwise collapse the mark onto the following
 * line, where it would confidently misidentify a different item — worse
 * than having no hint at all.  A cut-and-paste reorder therefore loses
 * its mark, and the text pass in on_db_note_set_actions picks it up
 * instead; the two signals cover each other's blind spot.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ActionMark — one action line's identity.
 *
 * Fields:
 *   mark — buffer mark at the line's start (left gravity, so text typed
 *          at the start of the line stays to its right).
 *   uid  — the stable uid of the item on that line.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkTextMark *mark;
    gint64       uid;
} ActionMark;

/* ---------------------------------------------------------------------------
 * action_real_lines() — the buffer line numbers of every REAL action line,
 * in order, so the n-th entry corresponds to the item at ord n (the same
 * numbering the extractor and on_note_buffer_action_strike use).
 * Returns a new GArray of gint; g_array_unref() it.
 * ------------------------------------------------------------------------- */
static GArray *
action_real_lines(GtkTextBuffer *buffer)
{
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(gint));
    gint n_lines = gtk_text_buffer_get_line_count(buffer);
    for (gint line = 0; line < n_lines; line++) {
        GtkTextIter ls, rs, le;      /* line span (+ rest start)            */
        if (!action_line_rest(buffer, line, &ls, &rs, &le))
            continue;
        gchar *t = gtk_text_buffer_get_text(buffer, &rs, &le, FALSE);
        gboolean real = action_rest_real(t);
        g_free(t);
        if (real)
            g_array_append_val(lines, line);
    }
    return lines;
}

/* action_marks_clear() — drop every identity mark (buffer side included). */
static void
action_marks_clear(OnNoteView *v)
{
    if (v->action_marks == NULL)
        return;
    for (guint i = 0; i < v->action_marks->len; i++) {
        ActionMark *am = g_ptr_array_index(v->action_marks, i);
        if (!gtk_text_mark_get_deleted(am->mark))
            gtk_text_buffer_delete_mark(v->buffer, am->mark);
    }
    g_ptr_array_set_size(v->action_marks, 0);
}

/* ---------------------------------------------------------------------------
 * on_note_view_action_marks_sync() — re-place the identity marks so the
 * n-th real action line carries the n-th item's uid.  Called after any
 * save that rewrote the table (the uids are then known) and once at load.
 *   items — OnActionItem list in ord order, uids filled in.
 * ------------------------------------------------------------------------- */
void
on_note_view_action_marks_sync(OnNoteView *v, GList *items)
{
    if (v->action_marks == NULL)
        return;
    action_marks_clear(v);

    GArray *lines = action_real_lines(v->buffer);
    guint   i = 0;                   /* index into the line array           */
    for (GList *l = items; l != NULL && i < lines->len; l = l->next, i++) {
        OnActionItem *it = l->data;
        if (it->uid == 0)            /* nothing to remember                 */
            continue;
        GtkTextIter ls;              /* the line's start                    */
        gtk_text_buffer_get_iter_at_line(v->buffer, &ls,
                                         g_array_index(lines, gint, i));
        ActionMark *am = g_new0(ActionMark, 1);
        am->mark = gtk_text_buffer_create_mark(v->buffer, NULL, &ls, TRUE);
        am->uid  = it->uid;
        g_ptr_array_add(v->action_marks, am);
    }
    g_array_unref(lines);
}

/* ---------------------------------------------------------------------------
 * on_note_view_action_marks_hint() — fill each item's uid with the uid
 * marked on its line, as a match hint for on_db_note_set_actions.  An
 * item whose line carries no surviving mark keeps uid 0 ("no hint"),
 * leaving the text and ord passes to identify it.
 *   items — freshly extracted OnActionItem list, in ord order.
 * ------------------------------------------------------------------------- */
void
on_note_view_action_marks_hint(OnNoteView *v, GList *items)
{
    if (v->action_marks == NULL || v->action_marks->len == 0)
        return;

    /* line number → uid, for the marks that are still alive.               */
    GHashTable *by_line = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < v->action_marks->len; i++) {
        ActionMark *am = g_ptr_array_index(v->action_marks, i);
        if (gtk_text_mark_get_deleted(am->mark))
            continue;
        GtkTextIter at;              /* the mark's current position         */
        gtk_text_buffer_get_iter_at_mark(v->buffer, &at, am->mark);
        gint line = gtk_text_iter_get_line(&at);
        /* First mark on a line wins: a second one can only be a leftover
         * from a line that was merged into this one.                       */
        if (!g_hash_table_contains(by_line, GINT_TO_POINTER(line)))
            g_hash_table_insert(by_line, GINT_TO_POINTER(line),
                                &am->uid);
    }

    GArray *lines = action_real_lines(v->buffer);
    guint   i = 0;                   /* index into the line array           */
    for (GList *l = items; l != NULL && i < lines->len; l = l->next, i++) {
        gint    line = g_array_index(lines, gint, i);
        gint64 *uid  = g_hash_table_lookup(by_line, GINT_TO_POINTER(line));
        if (uid != NULL)
            ((OnActionItem *)l->data)->uid = *uid;
    }
    g_array_unref(lines);
    g_hash_table_destroy(by_line);
}

/* ---------------------------------------------------------------------------
 * action_marks_prune() — destroy the identity marks that sit inside a
 * range about to be deleted.  MUST run BEFORE the deletion: afterwards
 * GtkTextBuffer has already collapsed those marks onto the join point,
 * where they are indistinguishable from the surviving neighbour's mark.
 * ------------------------------------------------------------------------- */
static void
action_marks_prune(OnNoteView *v, const GtkTextIter *start,
                   const GtkTextIter *end)
{
    if (v->action_marks == NULL || v->action_marks->len == 0)
        return;
    for (guint i = v->action_marks->len; i > 0; i--) {
        ActionMark *am = g_ptr_array_index(v->action_marks, i - 1);
        if (gtk_text_mark_get_deleted(am->mark)) {
            g_ptr_array_remove_index(v->action_marks, i - 1);
            continue;
        }
        GtkTextIter at;              /* the mark's current position         */
        gtk_text_buffer_get_iter_at_mark(v->buffer, &at, am->mark);
        if (gtk_text_iter_compare(&at, start) >= 0 &&
            gtk_text_iter_compare(&at, end) < 0) {
            gtk_text_buffer_delete_mark(v->buffer, am->mark);
            g_ptr_array_remove_index(v->action_marks, i - 1);
        }
    }
}

/* ---------------------------------------------------------------------------
 * action_nth_real_line() — locate the `ord`-th REAL action line, using the
 * SAME numbering as the extractor and the identity marks (action_real_lines
 * is the one definition of that order, so the three cannot drift apart).
 *   ord — position among the real action lines.
 *   ls  — receives the line start; rs the first char after the '!'; le the
 *         line end (newline excluded).
 * Returns TRUE when that many action lines exist.
 * ------------------------------------------------------------------------- */
static gboolean
action_nth_real_line(GtkTextBuffer *buffer, gint ord, GtkTextIter *ls,
                     GtkTextIter *rs, GtkTextIter *le)
{
    GArray *lines = action_real_lines(buffer);
    gboolean found = ord >= 0 && ord < (gint)lines->len &&
                     action_line_rest(buffer,
                                      g_array_index(lines, gint, ord),
                                      ls, rs, le);
    g_array_unref(lines);
    return found;
}

/* ---------------------------------------------------------------------------
 * on_note_buffer_action_strike() — strike (or un-strike) the text of the
 * `ord`-th REAL action line of `buffer` (see action_rest_real).  Works on
 * any buffer that has been through on_buffer_ensure_tags — live views and
 * offscreen loads alike.  Returns TRUE when the line was found.
 * ------------------------------------------------------------------------- */
gboolean
on_note_buffer_action_strike(GtkTextBuffer *buffer, gint ord, gboolean done)
{
    GtkTextIter ls, rs, le;          /* line span (+ rest start)            */
    if (!action_nth_real_line(buffer, ord, &ls, &rs, &le))
        return FALSE;
    if (done)
        gtk_text_buffer_apply_tag_by_name(buffer, ON_TAGNAME_STRIKE,
                                          &rs, &le);
    else
        gtk_text_buffer_remove_tag_by_name(buffer, ON_TAGNAME_STRIKE,
                                           &rs, &le);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * on_note_buffer_action_due() — rewrite the "due <date>" suffix of the
 * `ord`-th REAL action line: any existing suffix is removed, then " due
 * YYYY-MM-DD" is appended for a non-zero `due` (ISO — the canonical
 * written form; the parser also reads M/D/YY).  Appended text inherits
 * the strike state of the item text so a done item stays done.
 * Returns TRUE when the line was found.
 * ------------------------------------------------------------------------- */
gboolean
on_note_buffer_action_due(GtkTextBuffer *buffer, gint ord, gint64 due)
{
    GtkTextIter ls, rs, le;          /* line span (+ rest start)            */
    if (!action_nth_real_line(buffer, ord, &ls, &rs, &le))
        return FALSE;
    gchar *rest = gtk_text_buffer_get_text(buffer, &rs, &le, FALSE);

    /* Byte length of the item text: everything up to an existing
     * "due <date>" (or the whole rest), trailing whitespace dropped.       */
    gsize  text_bytes;
    gsize  due_start;
    gint64 old_due;
    text_bytes = on_action_split_due(rest, &due_start, &old_due)
                 ? due_start : strlen(rest);
    while (text_bytes > 0 &&
           g_ascii_isspace((guchar)rest[text_bytes - 1]))
        text_bytes--;
    glong text_chars = g_utf8_strlen(rest, (gssize)text_bytes);

    /* Does the item text end struck through?  The new suffix must
     * match, or setting a due date would "reopen" a done item.             */
    GtkTextIter del_s = rs;          /* everything after the text goes      */
    gtk_text_iter_forward_chars(&del_s, (gint)text_chars);
    gboolean struck = FALSE;
    if (text_chars > 0) {
        GtkTextIter probe = del_s;
        gtk_text_iter_backward_char(&probe);
        struck = (on_flags_at_iter(buffer, &probe, ON_FMT_INLINE_MASK) &
                  ON_FMT_STRIKE) != 0;
    }

    gtk_text_buffer_delete(buffer, &del_s, &le);
    if (due != 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        gchar *suffix = g_date_time_format(dt, " due %Y-%m-%d");
        g_date_time_unref(dt);
        if (struck)
            gtk_text_buffer_insert_with_tags_by_name(
                buffer, &del_s, suffix, -1, ON_TAGNAME_STRIKE, NULL);
        else
            gtk_text_buffer_insert(buffer, &del_s, suffix, -1);
        g_free(suffix);
    }
    g_free(rest);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * on_note_buffer_action_text() — replace the TEXT of the `ord`-th REAL
 * action line, leaving the '!' prefix, the line's own whitespace around
 * the text and
 * any trailing "due <date>" exactly as they were.  The span replaced is
 * the one on_action_split_due marks off — the SAME derivation the
 * extractor uses to produce OnActionItem.text — so the rewritten line
 * re-extracts to `text` and no due date is needed for the boundary
 * (without one the item text simply runs to the end of the line).
 * The replacement is given the old text's strike state explicitly, so
 * renaming a done item neither reopens it nor silently completes it.
 *   text — the new item text; the caller guarantees it is non-blank and
 *          newline-free (a blank would stop the line being an item at
 *          all, a newline would split it into two).
 * Returns TRUE when the line was found.
 * ------------------------------------------------------------------------- */
gboolean
on_note_buffer_action_text(GtkTextBuffer *buffer, gint ord,
                           const gchar *text)
{
    GtkTextIter ls, rs, le;          /* line span (+ rest start)            */
    if (!action_nth_real_line(buffer, ord, &ls, &rs, &le))
        return FALSE;
    gchar *rest = gtk_text_buffer_get_text(buffer, &rs, &le, FALSE);

    /* Byte span of the item text inside `rest`: up to an existing
     * "due <date>" (or the whole rest), then trailing whitespace dropped
     * and leading whitespace skipped, so the line keeps its own spacing.  */
    gsize  text_bytes;               /* end of the item text                */
    gsize  due_start;
    gint64 due;
    text_bytes = on_action_split_due(rest, &due_start, &due)
                 ? due_start : strlen(rest);
    while (text_bytes > 0 &&
           g_ascii_isspace((guchar)rest[text_bytes - 1]))
        text_bytes--;
    gsize lead = 0;                  /* whitespace right after the '!'      */
    while (lead < text_bytes && g_ascii_isspace((guchar)rest[lead]))
        lead++;

    GtkTextIter start = rs;          /* first char of the item text         */
    gtk_text_iter_forward_chars(&start,
                                (gint)g_utf8_strlen(rest, (gssize)lead));
    GtkTextIter end = rs;            /* just past the item text             */
    gtk_text_iter_forward_chars(&end,
                                (gint)g_utf8_strlen(rest,
                                                    (gssize)text_bytes));

    /* Struck at its last character = the done state to carry over, the
     * same probe on_note_buffer_action_due makes for the suffix it
     * appends.                                                          */
    gboolean struck = FALSE;
    if (!gtk_text_iter_equal(&start, &end)) {
        GtkTextIter probe = end;
        gtk_text_iter_backward_char(&probe);
        struck = (on_flags_at_iter(buffer, &probe, ON_FMT_INLINE_MASK) &
                  ON_FMT_STRIKE) != 0;
    }
    g_free(rest);

    gtk_text_buffer_delete(buffer, &start, &end);
    gint off = gtk_text_iter_get_offset(&start);   /* insertion point       */
    gtk_text_buffer_insert(buffer, &start, text, -1);

    /* `start` now sits at the END of the inserted run.  The strike is set
     * both ways on purpose: a plain insert inherits the tags of whatever
     * character precedes it, which on a done item is struck text.         */
    GtkTextIter from;                /* start of the inserted run           */
    gtk_text_buffer_get_iter_at_offset(buffer, &from, off);
    if (struck)
        gtk_text_buffer_apply_tag_by_name(buffer, ON_TAGNAME_STRIKE,
                                          &from, &start);
    else
        gtk_text_buffer_remove_tag_by_name(buffer, ON_TAGNAME_STRIKE,
                                           &from, &start);
    return TRUE;
}

/* ===========================================================================
 * task-list checkboxes
 *
 * Each task line starts with a child anchor holding the checked state;
 * the view attaches a native GtkCheckButton there.  Toggling writes
 * straight through to the anchor data and autosaves.
 * =========================================================================== */

/* on_task_checkbox_toggled() — sync the widget state into the anchor.       */
static void
on_task_checkbox_toggled(GtkCheckButton *btn, gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(btn), "on-anchor");
    if (anchor != NULL)
        on_anchor_set_checkbox(anchor, gtk_check_button_get_active(btn));
    note_view_edited(v);
}

/* ---------------------------------------------------------------------------
 * attach_checkbox_widget() — give a checkbox anchor its GtkCheckButton
 * (replacing any widget it already had).
 * ------------------------------------------------------------------------- */
static void
attach_checkbox_widget(OnNoteView *v, GtkTextChildAnchor *anchor)
{
    gboolean checked;                /* the anchor's stored state           */
    if (!on_anchor_is_checkbox(anchor, &checked))
        return;

    anchor_clear_widgets(v, anchor);

    GtkWidget *btn = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(btn), checked);
    /* Anchored children sit with their BOTTOM on the text baseline, so
     * theme padding above/below the indicator lifts the box's center
     * above the text's optical center.  Strip it (note_view_install_css) so
     * the widget is just the bare indicator.                               */
    gtk_widget_add_css_class(btn, "notes-task-check");
    /* Keyboard focus stays in the text; the box is mouse-only, and shows
     * the hyperlink-style hand while hovered.                              */
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_set_cursor_from_name(btn, "pointer");
    g_object_set_data(G_OBJECT(btn), "on-anchor", anchor);
    g_signal_connect(btn, "toggled",
                     G_CALLBACK(on_task_checkbox_toggled), v);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(v), btn, anchor);

    /* Lower the anchor character with the editor-only negative-rise tag
     * (created at buffer setup) so the box centers on the line's text.
     * Tag applies are invisible to the change/autosave machinery.          */
    GtkTextIter it, next;            /* the anchor's one-char span          */
    gtk_text_buffer_get_iter_at_child_anchor(v->buffer, &it, anchor);
    next = it;
    gtk_text_iter_forward_char(&next);
    gtk_text_buffer_apply_tag_by_name(v->buffer, "on-check-drop",
                                      &it, &next);
}

/* ---------------------------------------------------------------------------
 * insert_checkbox_at() — put a fresh unchecked checkbox anchor (plus its
 * separating space) at buffer offset `at`, tagged as part of the line.
 * ------------------------------------------------------------------------- */
static void
insert_checkbox_at(OnNoteView *v, gint at)
{
    GtkTextIter it;                  /* insertion position                  */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, at);
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(v->buffer, &it);
    on_anchor_set_checkbox(anchor, FALSE);
    gtk_text_buffer_insert(v->buffer, &it, " ", -1);

    GtkTextIter ts, te;              /* the anchor + space span             */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &ts, at);
    gtk_text_buffer_get_iter_at_offset(v->buffer, &te, at + 2);
    gtk_text_buffer_apply_tag_by_name(v->buffer, ON_TAGNAME_LIST_CHECK,
                                      &ts, &te);
    attach_checkbox_widget(v, anchor);
}

/* ===========================================================================
 * embedded tables
 *
 * A table is a child anchor carrying an OnTable (see serialize.h); the
 * view attaches a GtkGrid of cell views at it.  Right-clicking any
 * cell offers structural changes (add/remove rows and columns, delete),
 * which rebuild the widget from the updated data.
 * =========================================================================== */

/* A table column is never wider than this (logical px, margins included);
 * a cell whose longest line needs more wraps and grows DOWN instead.       */
#define TABLE_CELL_MAX_WIDTH 320
/* And never narrower than this, so an empty column is still clickable.     */
#define TABLE_CELL_MIN_WIDTH 64

/* ---------------------------------------------------------------------------
 * table_fit_columns() — size every cell of a table grid: each COLUMN to
 * the widest line among its cells (header included), capped at
 * TABLE_CELL_MAX_WIDTH, and each CELL to the height of its text wrapped at
 * that width.  Both are size requests, measured with a PangoLayout in the
 * cell's own font, so the grid has its true size from its first measure.
 *
 * Why the table measures itself instead of letting the cells request:
 * GTK4's GtkTextView no longer requests its content WIDTH at all (its
 * horizontal measure is margins plus anchored children — gtktextview.c
 * 4.22 gtk_text_view_measure; GTK3 reported the layout width), so a long
 * line sat at the minimum and hid the overflow.  And its HEIGHT is the
 * layout height, which a freshly attached cell has not computed yet: the
 * outer view's layout measured the grid before the cells validated, got a
 * stub height, and cached it — a table inserted on an empty last line was
 * drawn past the bottom of the view with no scroll range (D25; measured
 * to be a race, right in about two runs of three).  With explicit
 * requests nothing depends on validation order.
 *   grid — the table's GtkGrid (frame per cell, cell = frame child).
 *   rows / cols — the table's dimensions.
 * ------------------------------------------------------------------------- */
static void
table_fit_columns(GtkWidget *grid, gint rows, gint cols)
{
    for (gint c = 0; c < cols; c++) {
        gint widest = 0;             /* widest line in this column, px      */
        for (gint r = 0; r < rows; r++) {
            GtkWidget *frame = gtk_grid_get_child_at(GTK_GRID(grid), c, r);
            if (frame == NULL)
                continue;
            GtkWidget *cell = gtk_frame_get_child(GTK_FRAME(frame));
            GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cell));
            GtkTextIter s, e;
            gtk_text_buffer_get_bounds(buf, &s, &e);
            gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
            PangoLayout *lay = gtk_widget_create_pango_layout(cell, text);
            gint w;                  /* the longest line's pixel width      */
            pango_layout_get_pixel_size(lay, &w, NULL);
            g_object_unref(lay);
            g_free(text);
            w += gtk_text_view_get_left_margin(GTK_TEXT_VIEW(cell)) +
                 gtk_text_view_get_right_margin(GTK_TEXT_VIEW(cell)) + 2;
            widest = MAX(widest, w);
        }
        gint width = CLAMP(widest, TABLE_CELL_MIN_WIDTH, TABLE_CELL_MAX_WIDTH);

        for (gint r = 0; r < rows; r++) {
            GtkWidget *frame = gtk_grid_get_child_at(GTK_GRID(grid), c, r);
            if (frame == NULL)
                continue;
            GtkWidget *cell = gtk_frame_get_child(GTK_FRAME(frame));
            GtkTextView *tv = GTK_TEXT_VIEW(cell);
            GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
            GtkTextIter s, e;
            gtk_text_buffer_get_bounds(buf, &s, &e);
            gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
            /* The text's height once wrapped at the column's inner width,
             * exactly as the cell will lay it out; an empty cell is one
             * line tall.                                                   */
            PangoLayout *lay = gtk_widget_create_pango_layout(
                cell, *text != '\0' ? text : " ");
            pango_layout_set_wrap(lay, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_width(lay,
                (width - gtk_text_view_get_left_margin(tv)
                       - gtk_text_view_get_right_margin(tv)) * PANGO_SCALE);
            gint h;                  /* wrapped text height, px             */
            pango_layout_get_pixel_size(lay, NULL, &h);
            g_object_unref(lay);
            g_free(text);
            h += gtk_text_view_get_top_margin(tv) +
                 gtk_text_view_get_bottom_margin(tv);
            gtk_widget_set_size_request(cell, width, h);
        }
    }
}

/* on_table_cell_changed() — a cell buffer edited: write through to the
 * data, then refit the columns (the cell's own line may now be the widest
 * in its column, or no longer be).                                          */
static void
on_table_cell_changed(GtkTextBuffer *cell_buf, gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(cell_buf), "on-anchor");
    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;
    if (table == NULL)
        return;
    gint r = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell_buf),
                                               "on-row"));
    gint c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell_buf),
                                               "on-col"));
    GtkTextIter s, e;                /* the cell's full contents            */
    gtk_text_buffer_get_bounds(cell_buf, &s, &e);
    gchar *text = gtk_text_buffer_get_text(cell_buf, &s, &e, FALSE);
    on_table_set(table, r, c, text);
    g_free(text);
    GtkWidget *cell =                /* the edited cell's own view          */
        g_object_get_data(G_OBJECT(cell_buf), "on-cell");
    GtkWidget *grid = gtk_widget_get_ancestor(cell, GTK_TYPE_GRID);
    if (grid != NULL)
        table_fit_columns(grid, table->rows, table->cols);
    note_view_edited(v);
}

/* ---------------------------------------------------------------------------
 * table_op() — one structural operation on the table the last context
 * menu was opened on (v->ctx_offset), named by the action that asked:
 * "row-add", "row-del", "col-add", "col-del", "header" or "delete".
 * ------------------------------------------------------------------------- */
static void
table_op(OnNoteView *v, const gchar *op)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(v, v->ctx_offset);
    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;
    if (table == NULL)
        return;

    if (g_strcmp0(op, "row-add") == 0)
        on_table_resize(table, table->rows + 1, table->cols);
    else if (g_strcmp0(op, "row-del") == 0)
        on_table_resize(table, table->rows - 1, table->cols);
    else if (g_strcmp0(op, "col-add") == 0)
        on_table_resize(table, table->rows, table->cols + 1);
    else if (g_strcmp0(op, "col-del") == 0)
        on_table_resize(table, table->rows, table->cols - 1);
    else if (g_strcmp0(op, "header") == 0)
        table->header = !table->header;
    else if (g_strcmp0(op, "delete") == 0) {
        /* Remove the anchor character; its widget dies with it.            */
        GtkTextIter s, e;            /* the anchor's single character       */
        gtk_text_buffer_get_iter_at_child_anchor(v->buffer, &s, anchor);
        e = s;
        gtk_text_iter_forward_char(&e);
        v->internal_change++;
        gtk_text_buffer_delete(v->buffer, &s, &e);
        v->internal_change--;
        note_view_edited(v);
        return;
    }

    attach_table_widget(v, anchor); /* rebuild at the new dimensions       */
    note_view_edited(v);
}

/* The "table-<op>" actions carry the op in their name.                     */
#define TABLE_ACTION_PREFIX "table-"

/* on_table_command() — "activate" of a plain "view.table-*" action.         */
static void
on_table_command(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)param;
    table_op(user_data,
             g_action_get_name(G_ACTION(action)) + strlen(TABLE_ACTION_PREFIX));
}

/* on_table_header_change_state() — "view.table-header", the stateful one
 * behind the Header Row check item: its state is set from the table when
 * the menu opens, and a flip toggles the table's header.                    */
static void
on_table_header_change_state(GSimpleAction *action, GVariant *value,
                             gpointer user_data)
{
    g_simple_action_set_state(action, value);
    table_op(user_data, "header");
}

/* ---------------------------------------------------------------------------
 * on_table_cell_button() — a bubble-phase legacy controller on each cell
 * that reports every button press and release HANDLED once the cell's own
 * controllers have run (all of a widget's controllers run before GTK reads
 * the verdict), so the event never reaches the OUTER view.  Without it,
 * GtkTextView's own press handler on the outer view — which calls
 * gtk_widget_grab_focus unconditionally and does not claim a plain press —
 * ran right after the cell's and took the focus straight back: clicking
 * into a cell only "took" when the two happened to leave the focus on the
 * cell, every third or fourth click.  (Claiming the sequence instead would
 * deny the cell's own gesture, which is what places the caret.)
 *   controller — the legacy controller.
 *   event      — the event.
 *   user_data  — unused.
 * Returns TRUE for button events (stop), FALSE for everything else.
 * ------------------------------------------------------------------------- */
static gboolean
on_table_cell_button(GtkEventControllerLegacy *controller, GdkEvent *event,
                     gpointer user_data)
{
    (void)controller; (void)user_data;
    GdkEventType type = gdk_event_get_event_type(event);
    return type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE;
}

/* ---------------------------------------------------------------------------
 * on_table_cell_pressed() — a context-menu press in a cell (its click
 * gesture, CAPTURE phase so it precedes the cell view's own gesture, which
 * would open the standard text popup): the structural menu (add/remove
 * last row/column, header row, delete the table).  The cell's table
 * becomes the context the "view.table-*" actions act on.
 *
 * on_app_menu_popup parents the popover to the WINDOW's child box, never
 * to the cell — which matters here: every table op rebuilds the grid, so
 * the cell dies while the menu's teardown idle is still pending.  It also
 * means the "view." group must be reachable from that box: the host
 * inserts on_note_view_action_group() there (see note_view.h).
 * ------------------------------------------------------------------------- */
static void
on_table_cell_pressed(GtkGestureClick *gesture, gint n_press, gdouble x,
                      gdouble y, gpointer user_data)
{
    (void)n_press;
    OnNoteView *v = user_data;       /* the view                            */
    GtkWidget *cell =                /* the cell view the press landed in   */
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(gesture));
    if (event == NULL || !gdk_event_triggers_context_menu(event)) {
        /* Anchored children inside a GtkTextView don't reliably receive
         * the focus from the default click handling (GTK4 as GTK3): force
         * it so the caret lands in the cell, then let the cell's own
         * gesture place it.                                                */
        gtk_widget_grab_focus(cell);
        return;
    }

    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(cell), "on-anchor");
    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;
    if (table == NULL)
        return;

    GtkTextIter it;                  /* where the anchor sits               */
    gtk_text_buffer_get_iter_at_child_anchor(v->buffer, &it, anchor);
    v->ctx_offset = gtk_text_iter_get_offset(&it);

    /* The check item mirrors the table's current header state.            */
    GAction *header = g_action_map_lookup_action(G_ACTION_MAP(v->actions),
                                                 "table-header");
    g_simple_action_set_state(G_SIMPLE_ACTION(header),
                              g_variant_new_boolean(table->header));

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();
    g_menu_append(section, "Add _Row",      "view.table-row-add");
    g_menu_append(section, "Add _Column",   "view.table-col-add");
    g_menu_append(section, "Remove Row",    "view.table-row-del");
    g_menu_append(section, "Remove Column", "view.table-col-del");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    g_menu_append(section, "_Header Row",   "view.table-header");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    g_menu_append(section, "_Delete Table", "view.table-delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);

    on_app_menu_popup(cell, G_MENU_MODEL(menu), x, y);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ---------------------------------------------------------------------------
 * attach_table_widget() — (re)build the GtkGrid of cell views representing
 * an anchor's table, replacing any widget it already had.
 * ------------------------------------------------------------------------- */
static void
attach_table_widget(OnNoteView *v, GtkTextChildAnchor *anchor)
{
    OnTable *table = on_anchor_get_table(anchor);
    if (table == NULL)
        return;

    /* Drop the previous widget (after a structural change).                */
    anchor_clear_widgets(v, anchor);

    GtkWidget *grid = gtk_grid_new();
    for (gint r = 0; r < table->rows; r++) {
        for (gint c = 0; c < table->cols; c++) {
            /* Each cell is a bare GtkTextView (multiline, and it requests
             * its content size so the cell auto-grows) inside a frame
             * that draws the cell border.                                  */
            GtkWidget *cell = gtk_text_view_new();
            GtkTextBuffer *cell_buf =
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(cell));
            gtk_text_buffer_set_text(cell_buf,
                                     on_table_get(table, r, c), -1);
            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(cell), 6);
            gtk_text_view_set_right_margin(GTK_TEXT_VIEW(cell), 6);
            gtk_text_view_set_top_margin(GTK_TEXT_VIEW(cell), 3);
            gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(cell), 3);
            /* Wrap only past the column cap: table_fit_columns gives each
             * column its widest line up to TABLE_CELL_MAX_WIDTH.          */
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(cell),
                                        GTK_WRAP_WORD_CHAR);
            g_object_set_data(G_OBJECT(cell_buf), "on-cell", cell);

            /* Header row: bold on a light grey fill (note_view_install_css). */
            if (table->header && r == 0)
                gtk_widget_add_css_class(cell, "notes-table-header");

            g_object_set_data(G_OBJECT(cell_buf), "on-anchor", anchor);
            g_object_set_data(G_OBJECT(cell_buf), "on-row",
                              GINT_TO_POINTER(r));
            g_object_set_data(G_OBJECT(cell_buf), "on-col",
                              GINT_TO_POINTER(c));
            g_signal_connect(cell_buf, "changed",
                             G_CALLBACK(on_table_cell_changed), v);
            g_object_set_data(G_OBJECT(cell), "on-anchor", anchor);
            /* A cell has keys of its own: the host hangs its editing gate
             * on it (D19).                                                 */
            g_signal_emit(v, signals[SIG_CELL_CREATED], 0, cell);
            GtkGesture *press = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
            gtk_event_controller_set_propagation_phase(
                GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
            g_signal_connect(press, "pressed",
                             G_CALLBACK(on_table_cell_pressed), v);
            gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(press));
            /* And the presses STOP at the cell: see on_table_cell_button. */
            GtkEventController *swallow = gtk_event_controller_legacy_new();
            g_signal_connect(swallow, "event",
                             G_CALLBACK(on_table_cell_button), NULL);
            gtk_widget_add_controller(cell, swallow);

            GtkWidget *frame = gtk_frame_new(NULL);
            /* Square cells: the theme rounds every frame 8 px in GTK4
             * (note_view_install_css takes it back off).                    */
            gtk_widget_add_css_class(frame, "notes-table-cell");
            gtk_frame_set_child(GTK_FRAME(frame), cell);
            gtk_grid_attach(GTK_GRID(grid), frame, c, r, 1, 1);
        }
    }
    table_fit_columns(grid, table->rows, table->cols);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(v), grid, anchor);
}

/* ---------------------------------------------------------------------------
 * is_emoji_char() — rough emoji detection: the blocks that render via the
 * color emoji font and overlap neighbouring text on macOS (Apple Color
 * Emoji draws wider than the advance Pango reserves for it).
 * ------------------------------------------------------------------------- */
static gboolean
is_emoji_char(gunichar c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) ||   /* emoji + symbols planes    */
           (c >= 0x2600  && c <= 0x27BF)  ||   /* misc symbols, dingbats    */
           (c >= 0x1F1E6 && c <= 0x1F1FF) ||   /* regional indicators       */
           c == 0x2B50 || c == 0x2B55;         /* star, circle              */
}

/* ---------------------------------------------------------------------------
 * tag_emoji_in_range() — apply the padding tag to every emoji between the
 * two buffer offsets — the EMOJI ONLY.  The "on-emoji" tag adds letter
 * spacing; Apple Color Emoji draws ~2 px past its advance, and the spacing
 * pushes the next character clear of it.  Measured on GTK 4.22's Pango
 * ("a😀bcd", spacing 5): tagging the emoji alone widens its advance by the
 * full 5 and leaves the follower at its natural width; tagging the
 * follower too (the GTK3 recipe, written when Pango dropped the half-gap
 * at a run edge) widens the FOLLOWER by 5 as well, a visible hole before
 * the second letter typed after an emoji.
 *
 * macOS-only: on Linux, color emoji fonts (e.g. Noto) fit their advance
 * and need no artificial padding, so this is compiled to a no-op there.
 * Editor-only styling; never serialized.
 * ------------------------------------------------------------------------- */
static void
tag_emoji_in_range(OnNoteView *v, gint start_off, gint end_off)
{
#ifdef __APPLE__
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, start_off);
    while (gtk_text_iter_get_offset(&it) < end_off) {
        if (is_emoji_char(gtk_text_iter_get_char(&it))) {
            GtkTextIter next = it;   /* the following character             */
            gtk_text_iter_forward_char(&next);
            gtk_text_buffer_apply_tag_by_name(v->buffer, "on-emoji",
                                              &it, &next);
        }
        if (!gtk_text_iter_forward_char(&it))
            break;
    }
#else
    (void)v; (void)start_off; (void)end_off;
#endif
}

/* ---------------------------------------------------------------------------
 * on_note_view_insert_emoji() — the host's "win.insert-emoji" (Insert menu,
 * Primary+E): open GTK's built-in emoji chooser at the cursor via the text
 * view's "insert-emoji" action (the same one bound to Ctrl+. natively).
 * The picked emoji is inserted as plain UTF-8 text, so styling, storage
 * and export handle it like any typed character.
 * ------------------------------------------------------------------------- */
void
on_note_view_insert_emoji(OnNoteView *v)
{
    gtk_widget_grab_focus(GTK_WIDGET(v));
    g_signal_emit_by_name(GTK_TEXT_VIEW(v), "insert-emoji");
}

/* ---------------------------------------------------------------------------
 * on_note_view_insert_table() — the host's "win.insert-table": embed a
 * fresh 3×3 table at the cursor.  Rows/columns are added or removed
 * afterwards from any cell's right-click menu.
 * ------------------------------------------------------------------------- */
void
on_note_view_insert_table(OnNoteView *v)
{
    GtkTextIter cursor;              /* insertion point                     */
    gtk_text_buffer_get_iter_at_mark(v->buffer, &cursor,
                                     gtk_text_buffer_get_insert(v->buffer));
    v->internal_change++;
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(v->buffer, &cursor);
    on_anchor_set_table(anchor, on_table_new(3, 3));
    attach_table_widget(v, anchor);
    v->internal_change--;
    note_view_edited(v);
    note_view_scroll_after_layout(v);
}

/* ---------------------------------------------------------------------------
 * on_note_view_insert_date() — the host's "win.insert-date" (Insert menu,
 * Primary+D): insert today's date in ISO format (YYYY-MM-DD) at the
 * cursor, replacing the selection like typed text would.  It goes through
 * the normal insert-text path, so styling, autosave and undo of the
 * surrounding text behave exactly as for a paste.
 * ------------------------------------------------------------------------- */
void
on_note_view_insert_date(OnNoteView *v)
{
    gtk_widget_grab_focus(GTK_WIDGET(v));
    GDateTime *now = g_date_time_new_now_local();
    gchar *date = g_date_time_format(now, "%Y-%m-%d");
    gtk_text_buffer_delete_selection(v->buffer, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(v->buffer, date, -1);
    g_free(date);
    g_date_time_unref(now);
}

/* ===========================================================================
 * #tag capture and autocomplete popup
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * strip_tags_in_code_blocks() — text inside a code block is never a #tag,
 * but tag spans still land there: by formatting tagged text as a code
 * block, or by pasting styled text into one.  Remove the ON_TAGNAME_TAG
 * styling from every span on a code-block line within [start_off,
 * end_off).
 *   v        — the view.
 *   start_off — first buffer offset of the range to scan.
 *   end_off   — offset just past the range.
 * Returns TRUE when at least one span was stripped — callers then mark
 * the tag set modified so the next save rewrites note_tags.
 * ------------------------------------------------------------------------- */
static gboolean
strip_tags_in_code_blocks(OnNoteView *v, gint start_off, gint end_off)
{
    GtkTextTag *tag = lookup_tag(v->buffer, ON_TAGNAME_TAG);
    if (tag == NULL)
        return FALSE;

    gboolean stripped = FALSE;       /* removed anything yet?               */
    GtkTextIter it, limit;           /* walk position / range end           */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, start_off);
    gtk_text_buffer_get_iter_at_offset(v->buffer, &limit, end_off);

    /* A range boundary can fall mid-span (typing inside a styled tag):
     * pull the walk back to that span's start so it is seen whole.         */
    if (gtk_text_iter_has_tag(&it, tag) && !gtk_text_iter_starts_tag(&it, tag))
        gtk_text_iter_backward_to_tag_toggle(&it, tag);

    /* Jump span to span, as in on_buffer_collect_tags().                   */
    while (gtk_text_iter_compare(&it, &limit) < 0) {
        if (!gtk_text_iter_starts_tag(&it, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
                break;
            continue;
        }
        GtkTextIter span_end = it;   /* end of this tag span                */
        gtk_text_iter_forward_to_tag_toggle(&span_end, tag);
        if (line_para_flags(v->buffer, &it) & ON_FMT_CODEBLOCK) {
            v->internal_change++;
            gtk_text_buffer_remove_tag(v->buffer, tag, &it, &span_end);
            v->internal_change--;
            stripped = TRUE;
        }
        it = span_end;
    }
    return stripped;
}

/* ---------------------------------------------------------------------------
 * tag_capture_span() — compute the span of the tag currently being typed:
 * from the '#' at v->tag_start forward across word characters.
 *   v    — the view (capture must be active).
 *   start — receives the position of the '#'.
 *   end   — receives the position just past the last word character.
 * Returns TRUE if the span still starts with '#' (capture is valid).
 * ------------------------------------------------------------------------- */
static gboolean
tag_capture_span(OnNoteView *v, GtkTextIter *start, GtkTextIter *end)
{
    gtk_text_buffer_get_iter_at_mark(v->buffer, start, v->tag_start);
    if (gtk_text_iter_get_char(start) != '#')
        return FALSE;

    *end = *start;
    gtk_text_iter_forward_char(end);      /* step past the '#'              */
    while (!gtk_text_iter_is_end(end)) {
        gunichar c = gtk_text_iter_get_char(end);
        if (!(g_unichar_isalnum(c) || c == '_' || c == '-'))
            break;
        gtk_text_iter_forward_char(end);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * tag_popup_hide() — pop the suggestion popover down if it is showing.
 * ------------------------------------------------------------------------- */
static void
tag_popup_hide(OnNoteView *v)
{
    if (v->tag_popup != NULL)
        gtk_popover_popdown(GTK_POPOVER(v->tag_popup));
}

/* ---------------------------------------------------------------------------
 * tag_capture_end() — finish the active tag capture.
 *   v    — the view.
 *   apply — TRUE to style the "#word" span as a tag (if the word is
 *           non-empty); FALSE to abandon it as plain text (Escape).
 * ------------------------------------------------------------------------- */
static void
tag_capture_end(OnNoteView *v, gboolean apply)
{
    if (v->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* the "#word" span                    */
    if (tag_capture_span(v, &start, &end) && apply) {
        /* Only style it if there is at least one char after the '#'.       */
        if (gtk_text_iter_get_offset(&end) -
            gtk_text_iter_get_offset(&start) >= 2) {
            v->internal_change++;
            gtk_text_buffer_apply_tag_by_name(v->buffer, ON_TAGNAME_TAG,
                                              &start, &end);
            v->internal_change--;
            v->tags_modified = TRUE;    /* a tag was created               */
            note_view_edited(v);
        }
    }
    gtk_text_buffer_delete_mark(v->buffer, v->tag_start);
    v->tag_start = NULL;
    on_db_tag_list_free(v->tag_choices);
    v->tag_choices = NULL;
    tag_popup_hide(v);
}

/* ---------------------------------------------------------------------------
 * on_tag_row_activated() — a suggestion row was clicked/activated: replace
 * the partially typed word with the chosen tag name, style it, and end the
 * capture with a trailing space (which is what "ends" a tag).
 * ------------------------------------------------------------------------- */
static void
on_tag_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    OnNoteView *v = user_data;       /* the view                            */
    const gchar *name =              /* chosen tag name (no '#')            */
        g_object_get_data(G_OBJECT(row), "on-tag-name");
    if (name == NULL || v->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* current "#partial" span             */
    if (!tag_capture_span(v, &start, &end)) {
        tag_capture_end(v, FALSE);
        return;
    }

    v->internal_change++;
    /* Replace "#partial" with "#name ".                                    */
    gint at = gtk_text_iter_get_offset(&start);
    gtk_text_buffer_delete(v->buffer, &start, &end);
    gchar *full = g_strdup_printf("#%s", name);
    gtk_text_buffer_get_iter_at_offset(v->buffer, &start, at);
    gtk_text_buffer_insert(v->buffer, &start, full, -1);

    GtkTextIter ts, te;              /* span of the completed tag           */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &ts, at);
    gtk_text_buffer_get_iter_at_offset(
        v->buffer, &te, at + (gint)g_utf8_strlen(full, -1));
    gtk_text_buffer_apply_tag_by_name(v->buffer, ON_TAGNAME_TAG, &ts, &te);

    /* Trailing space ends the tag (untagged).                              */
    gtk_text_buffer_insert(v->buffer, &te, " ", -1);
    g_free(full);
    v->internal_change--;

    /* Capture is over; the tag styling was applied above.                  */
    gtk_text_buffer_delete_mark(v->buffer, v->tag_start);
    v->tag_start = NULL;
    on_db_tag_list_free(v->tag_choices);
    v->tag_choices = NULL;
    tag_popup_hide(v);
    v->tags_modified = TRUE;        /* a tag was created                   */
    note_view_edited(v);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

/* ---------------------------------------------------------------------------
 * tag_popup_ensure() — lazily build the popover + listbox.  A GtkPopover
 * parented to the view (on_note_view_size_allocate presents it, its
 * dispose unparents it), pointing at the '#' from below with its left edge
 * on it.  NOT autohide and NOT focusable: the view keeps the keyboard and
 * the pointer, typing goes on filtering the rows, and a click on a row
 * cannot move the focus.
 * ------------------------------------------------------------------------- */
static void
tag_popup_ensure(OnNoteView *v)
{
    if (v->tag_popup != NULL)
        return;

    v->tag_popup = gtk_popover_new();
    gtk_popover_set_autohide(GTK_POPOVER(v->tag_popup), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(v->tag_popup), FALSE);
    gtk_popover_set_position(GTK_POPOVER(v->tag_popup), GTK_POS_BOTTOM);
    gtk_widget_set_halign(v->tag_popup, GTK_ALIGN_START);
    gtk_widget_set_can_focus(v->tag_popup, FALSE);
    gtk_widget_set_parent(v->tag_popup, GTK_WIDGET(v));

    v->tag_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(v->tag_listbox),
                                    GTK_SELECTION_SINGLE);
    gtk_popover_set_child(GTK_POPOVER(v->tag_popup), v->tag_listbox);
    g_signal_connect(v->tag_listbox, "row-activated",
                     G_CALLBACK(on_tag_row_activated), v);
}

/* ---------------------------------------------------------------------------
 * tag_popup_update() — refresh the popover contents to the tags matching
 * the currently typed prefix, point it at the '#', and show or hide it
 * depending on whether anything matches.
 * ------------------------------------------------------------------------- */
static void
tag_popup_update(OnNoteView *v)
{
    if (v->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* current "#partial" span             */
    if (!tag_capture_span(v, &start, &end)) {
        tag_capture_end(v, FALSE);
        return;
    }

    /* The prefix typed so far, without the '#'.                            */
    gchar *typed = gtk_text_buffer_get_text(v->buffer, &start, &end, FALSE);
    const gchar *prefix = typed + 1;

    tag_popup_ensure(v);

    /* Clear old suggestion rows.                                           */
    gtk_list_box_remove_all(GTK_LIST_BOX(v->tag_listbox));

    /* Fill with case-insensitive prefix matches from the capture-start
     * snapshot (see v->tag_choices).                                      */
    gint shown = 0;                  /* number of rows added                */
    gchar *prefix_ci = g_utf8_casefold(prefix, -1);
    for (GList *l = v->tag_choices;
         l != NULL && shown < TAG_POPUP_MAX; l = l->next) {
        OnTag *t = l->data;
        gchar *name_ci = g_utf8_casefold(t->name, -1);
        gboolean match = g_str_has_prefix(name_ci, prefix_ci);
        g_free(name_ci);
        if (!match)
            continue;

        GtkWidget *label = gtk_label_new(NULL);
        gchar *text = g_strdup_printf("#%s", t->name);
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 3);
        gtk_widget_set_margin_bottom(label, 3);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "on-tag-name",
                               g_strdup(t->name), g_free);
        gtk_list_box_insert(GTK_LIST_BOX(v->tag_listbox), row, -1);
        shown++;
    }
    g_free(prefix_ci);
    g_free(typed);

    if (shown == 0) {
        tag_popup_hide(v);
        return;
    }

    /* Select the first row so Enter picks it immediately.                  */
    GtkListBoxRow *first_row =
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(v->tag_listbox), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(v->tag_listbox), first_row);

    /* Point the popover at the '#' character: its box in buffer
     * coordinates, translated into the view's widget coordinates, which is
     * the space set_pointing_to takes for a popover parented to the view.  */
    GdkRectangle rect;               /* the '#' in buffer coords            */
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(v), &start, &rect);
    GdkRectangle at = rect;          /* the same box, widget coords         */
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(v),
                                          GTK_TEXT_WINDOW_WIDGET,
                                          rect.x, rect.y, &at.x, &at.y);
    gtk_popover_set_pointing_to(GTK_POPOVER(v->tag_popup), &at);
    gtk_popover_popup(GTK_POPOVER(v->tag_popup));
}

/* ---------------------------------------------------------------------------
 * tag_popup_move_selection() — move the popup's selected row up or down
 * (keyboard navigation while typing a tag).
 *   v    — the view (popup must exist).
 *   delta — +1 for down, -1 for up.
 * ------------------------------------------------------------------------- */
static void
tag_popup_move_selection(OnNoteView *v, gint delta)
{
    GtkListBox *box = GTK_LIST_BOX(v->tag_listbox);
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(box);
    gint index = (sel != NULL)
                 ? gtk_list_box_row_get_index(sel) + delta
                 : 0;
    if (index < 0)
        index = 0;
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(box, index);
    if (row != NULL)
        gtk_list_box_select_row(box, row);
}

/* ===========================================================================
 * undo / redo
 *
 * GtkTextBuffer's own undo (GTK4) is text-only: it would re-insert a
 * deleted anchor as an EMPTY anchor, without the image, checkbox state or
 * table the view keeps as anchor data.  It is switched off on the note
 * buffer (on_note_view_init) and the view keeps its own history of
 * whole-buffer SNAPSHOTS instead: a segment list
 * mirroring the serializer's walk — text runs with ON_FMT_* flags, plus
 * one segment per child anchor (image / checkbox / table).  Images are
 * held by pixbuf REFERENCE, shared across snapshots and with the live
 * buffer, so no PNG bytes are copied and nothing is re-decoded; tables
 * are deep-copied because the anchor's OnTable is mutated in place by
 * its cell entries.  Editor-only tags (emoji padding, search hits) stay
 * out of snapshots — restore re-runs the emoji pass.
 *
 * Grouping: every buffer mutation already funnels through
 * note_view_edited(), which (re)arms a UNDO_GROUP_MS debounce
 * timer — like the autosave debounce, each change RESETS it, so a group
 * commits only after the user pauses for UNDO_GROUP_MS: one undo step
 * per typing burst.  When it fires, the previously committed snapshot
 * is pushed on the undo stack and a fresh capture becomes "current".
 * Primary+Z flushes any pending group, then swaps the current snapshot
 * for the popped one (old current goes to the redo stack); Primary+Y
 * or Primary+Shift+Z mirrors it.  A commit that captures a state
 * identical to the current snapshot (e.g. an autosave queued by a
 * no-op) pushes nothing.  Two caps keep a non-stop burst from becoming
 * one giant step: a typed linebreak commits the group immediately, and
 * so does the UNDO_MAX_SENTENCES-th sentence ender ('.' or '?').
 *
 * Restore is MINIMAL-DIFF: the common prefix and suffix (in characters)
 * between the buffer's present state and the target snapshot stay
 * untouched — only the differing middle is deleted and re-inserted.
 * Undoing a word therefore doesn't churn every image widget in the note
 * or yank the scroll position the way a full clear-and-rebuild did.
 * =========================================================================== */

#define UNDO_GROUP_MS      1000      /* pause that closes an undo group     */
#define UNDO_MAX_GROUPS    100       /* history depth (oldest dropped)      */
#define UNDO_MAX_SENTENCES 5         /* sentence enders per group, tops     */

typedef enum {
    UNDO_SEG_TEXT,                   /* styled text run                     */
    UNDO_SEG_IMAGE,                  /* image anchor                        */
    UNDO_SEG_CHECK,                  /* task-checkbox anchor                */
    UNDO_SEG_TABLE,                  /* table anchor                        */
} UndoSegKind;

/* One buffer segment.  `flags` covers the whole run — for anchors, the
 * anchor's own 0xFFFC character (checkbox anchors carry the
 * on-list-check paragraph tag, and every anchor char sits inside its
 * line's paragraph span).                                                   */
typedef struct {
    UndoSegKind kind;
    guint32     flags;               /* ON_FMT_* bits on the segment        */
    gchar      *text;                /* TEXT: owned UTF-8 run               */
    GdkPixbuf  *pixbuf;              /* IMAGE: owned reference              */
    gint        display_width;       /* IMAGE: chosen on-screen width       */
    gboolean    checked;             /* CHECK: checkbox state               */
    OnTable    *table;               /* TABLE: owned deep copy              */
} UndoSeg;

/* One history entry: the whole buffer plus the cursor position.            */
struct UndoSnap {
    GPtrArray *segs;                 /* UndoSeg*, in buffer order           */
    gint       cursor;               /* insert-mark offset at capture time  */
    gboolean   has_tags;             /* any run carries ON_FMT_TAG (used to
                                      * set tags_modified on restore)       */
};

/* undo_seg_free() — release one segment and whatever it owns.               */
static void
undo_seg_free(UndoSeg *seg)
{
    g_free(seg->text);
    if (seg->pixbuf != NULL)
        g_object_unref(seg->pixbuf);
    if (seg->table != NULL)
        on_table_free(seg->table);
    g_free(seg);
}

/* undo_snap_free() — release one snapshot and all its segments.             */
static void
undo_snap_free(UndoSnap *snap)
{
    for (guint i = 0; i < snap->segs->len; i++)
        undo_seg_free(g_ptr_array_index(snap->segs, i));
    g_ptr_array_free(snap->segs, TRUE);
    g_free(snap);
}

/* undo_table_copy() — deep copy of a table (cells + header flag).           */
static OnTable *
undo_table_copy(OnTable *src)
{
    OnTable *copy = on_table_new(src->rows, src->cols);
    copy->header = src->header;
    for (gint r = 0; r < src->rows; r++)
        for (gint c = 0; c < src->cols; c++)
            on_table_set(copy, r, c, on_table_get(src, r, c));
    return copy;
}

/* ---------------------------------------------------------------------------
 * undo_snapshot_capture() — walk the buffer into a new snapshot.  Same
 * traversal as on_note_serialize(): anchors interrupt text runs; runs
 * split where the flag set changes; imageless anchors and stray 0xFFFC
 * chars are dropped (as on save).
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * undo_capture_seg() — OnBufferSegFn turning one walked segment into an
 * UndoSeg.  Images are held by REFERENCE (shared with the live buffer and
 * every other snapshot, so no PNG bytes are copied); tables are deep-copied
 * because the anchor's OnTable is mutated in place by its cell views.
 * ------------------------------------------------------------------------- */
static void
undo_capture_seg(const OnBufferSeg *in, gpointer data)
{
    UndoSnap *snap = data;
    UndoSeg  *seg  = g_new0(UndoSeg, 1);
    seg->flags = in->flags;

    switch (in->kind) {
    case ON_SEG_TEXT:
        seg->kind = UNDO_SEG_TEXT;
        seg->text = g_strndup(in->text, in->n_text);
        if (in->flags & ON_FMT_TAG)
            snap->has_tags = TRUE;
        break;
    case ON_SEG_CHECK:
        seg->kind    = UNDO_SEG_CHECK;
        seg->checked = in->checked;
        break;
    case ON_SEG_TABLE:
        seg->kind  = UNDO_SEG_TABLE;
        seg->table = undo_table_copy(in->table);
        break;
    case ON_SEG_IMAGE:
        seg->kind          = UNDO_SEG_IMAGE;
        seg->pixbuf        = g_object_ref(in->pixbuf);
        seg->display_width = in->display_width;
        break;
    }
    g_ptr_array_add(snap->segs, seg);
}

static UndoSnap *
undo_snapshot_capture(OnNoteView *v)
{
    UndoSnap *snap = g_new0(UndoSnap, 1);
    snap->segs = g_ptr_array_new();

    GtkTextIter ins;                 /* current cursor                      */
    gtk_text_buffer_get_iter_at_mark(v->buffer, &ins,
                                     gtk_text_buffer_get_insert(v->buffer));
    snap->cursor = gtk_text_iter_get_offset(&ins);

    /* THE buffer traversal — the same one the serializer uses, so a
     * snapshot can never disagree with what a save would write.            */
    on_buffer_walk(v->buffer, undo_capture_seg, snap);
    return snap;
}

/* undo_seg_equal() — content equality of two segments.                      */
static gboolean
undo_seg_equal(const UndoSeg *x, const UndoSeg *y)
{
    if (x->kind != y->kind || x->flags != y->flags)
        return FALSE;
    switch (x->kind) {
    case UNDO_SEG_TEXT:
        return g_strcmp0(x->text, y->text) == 0;
    case UNDO_SEG_IMAGE:
        return x->pixbuf == y->pixbuf &&
               x->display_width == y->display_width;
    case UNDO_SEG_CHECK:
        return x->checked == y->checked;
    case UNDO_SEG_TABLE:
        if (x->table->rows != y->table->rows ||
            x->table->cols != y->table->cols ||
            x->table->header != y->table->header)
            return FALSE;
        for (gint c = 0; c < x->table->rows * x->table->cols; c++)
            if (g_strcmp0(g_ptr_array_index(x->table->cells, c),
                          g_ptr_array_index(y->table->cells, c)) != 0)
                return FALSE;
        return TRUE;
    }
    return FALSE;
}

/* undo_snap_equal() — content equality (cursor position ignored).           */
static gboolean
undo_snap_equal(const UndoSnap *a, const UndoSnap *b)
{
    if (a->segs->len != b->segs->len)
        return FALSE;
    for (guint i = 0; i < a->segs->len; i++)
        if (!undo_seg_equal(g_ptr_array_index(a->segs, i),
                            g_ptr_array_index(b->segs, i)))
            return FALSE;
    return TRUE;
}

/* undo_seg_len() / undo_snap_len() — length in buffer characters (every
 * anchor occupies one 0xFFFC character).                                    */
static gint
undo_seg_len(const UndoSeg *seg)
{
    return seg->kind == UNDO_SEG_TEXT
           ? (gint)g_utf8_strlen(seg->text, -1) : 1;
}

static gint
undo_snap_len(const UndoSnap *snap)
{
    gint len = 0;                    /* accumulated char count              */
    for (guint i = 0; i < snap->segs->len; i++)
        len += undo_seg_len(g_ptr_array_index(snap->segs, i));
    return len;
}

/* ---------------------------------------------------------------------------
 * undo_common_prefix() / undo_common_suffix() — how many leading/trailing
 * characters two snapshots share.  Whole equal segments are consumed in
 * lockstep; at the first differing pair, two text runs with the SAME
 * flags still contribute their common prefix/suffix characters.  The
 * caller must clamp so the two regions never overlap.
 * ------------------------------------------------------------------------- */
static gint
undo_common_prefix(const UndoSnap *a, const UndoSnap *b)
{
    gint  chars = 0;                 /* shared leading characters           */
    guint i     = 0;                 /* segment index in both snapshots     */
    while (i < a->segs->len && i < b->segs->len) {
        const UndoSeg *x = g_ptr_array_index(a->segs, i);
        const UndoSeg *y = g_ptr_array_index(b->segs, i);
        if (undo_seg_equal(x, y)) {
            chars += undo_seg_len(x);
            i++;
            continue;
        }
        if (x->kind == UNDO_SEG_TEXT && y->kind == UNDO_SEG_TEXT &&
            x->flags == y->flags) {
            const gchar *p = x->text, *q = y->text;
            while (*p != '\0' && *q != '\0' &&
                   g_utf8_get_char(p) == g_utf8_get_char(q)) {
                chars++;
                p = g_utf8_next_char(p);
                q = g_utf8_next_char(q);
            }
        }
        break;
    }
    return chars;
}

static gint
undo_common_suffix(const UndoSnap *a, const UndoSnap *b)
{
    gint chars = 0;                  /* shared trailing characters          */
    gint i = (gint)a->segs->len - 1; /* walk both tails leftwards           */
    gint j = (gint)b->segs->len - 1;
    while (i >= 0 && j >= 0) {
        const UndoSeg *x = g_ptr_array_index(a->segs, i);
        const UndoSeg *y = g_ptr_array_index(b->segs, j);
        if (undo_seg_equal(x, y)) {
            chars += undo_seg_len(x);
            i--;
            j--;
            continue;
        }
        if (x->kind == UNDO_SEG_TEXT && y->kind == UNDO_SEG_TEXT &&
            x->flags == y->flags) {
            const gchar *p = x->text + strlen(x->text);
            const gchar *q = y->text + strlen(y->text);
            while (p > x->text && q > y->text) {
                const gchar *pp = g_utf8_prev_char(p);
                const gchar *qq = g_utf8_prev_char(q);
                if (g_utf8_get_char(pp) != g_utf8_get_char(qq))
                    break;
                chars++;
                p = pp;
                q = qq;
            }
        }
        break;
    }
    return chars;
}

/* ---------------------------------------------------------------------------
 * undo_commit_now() — close the in-progress edit group: push the old
 * current snapshot on the undo stack (unless nothing actually changed)
 * and capture the buffer as the new current state.  Any real commit
 * invalidates the redo branch.
 * ------------------------------------------------------------------------- */
static void
undo_commit_now(OnNoteView *v)
{
    v->undo_sentences = 0;          /* the cap counts per group            */
    if (v->undo_commit_source != 0) {
        g_source_remove(v->undo_commit_source);
        v->undo_commit_source = 0;
    }

    UndoSnap *now = undo_snapshot_capture(v);
    if (v->undo_current != NULL && undo_snap_equal(now, v->undo_current)) {
        undo_snap_free(v->undo_current);
        v->undo_current = now;      /* content unchanged: refresh cursor   */
        return;
    }

    if (v->undo_current != NULL) {
        if (v->undo_stack->len >= UNDO_MAX_GROUPS) {
            undo_snap_free(g_ptr_array_index(v->undo_stack, 0));
            g_ptr_array_remove_index(v->undo_stack, 0);
        }
        g_ptr_array_add(v->undo_stack, v->undo_current);
    }
    v->undo_current = now;

    for (guint i = 0; i < v->redo_stack->len; i++)
        undo_snap_free(g_ptr_array_index(v->redo_stack, i));
    g_ptr_array_set_size(v->redo_stack, 0);
}

/* on_undo_commit_timeout() — the group timer fired: commit.                 */
static gboolean
on_undo_commit_timeout(gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    v->undo_commit_source = 0;
    undo_commit_now(v);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * undo_notify_change() — called (via note_view_edited) on every
 * buffer mutation: (re)arm the group-commit debounce.  Each change
 * resets the timer, so the group closes UNDO_GROUP_MS after the user
 * STOPS editing — one undo step per typing burst.
 * ------------------------------------------------------------------------- */
static void
undo_notify_change(OnNoteView *v)
{
    if (v->undo_restoring || v->undo_stack == NULL)
        return;
    if (v->undo_commit_source != 0)
        g_source_remove(v->undo_commit_source);
    v->undo_commit_source = g_timeout_add(UNDO_GROUP_MS,
                                           on_undo_commit_timeout, v);
}

/* ---------------------------------------------------------------------------
 * undo_insert_seg_slice() — insert characters [s, s+n) of one segment at
 * buffer offset `at`: text slices by UTF-8 offset, anchors whole (their
 * slice is always the single 0xFFFC char).  The fresh span first has
 * every Notes tag stripped — GtkTextBuffer makes insertions inherit
 * tags applied on BOTH sides of the insertion point — then the segment's
 * own flags applied.  Returns n.
 * ------------------------------------------------------------------------- */
static gint
undo_insert_seg_slice(OnNoteView *v, const UndoSeg *seg, gint s, gint n,
                      gint at)
{
    GtkTextIter it;                  /* insertion point                     */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, at);

    if (seg->kind == UNDO_SEG_TEXT) {
        const gchar *ps = g_utf8_offset_to_pointer(seg->text, s);
        const gchar *pe = g_utf8_offset_to_pointer(ps, n);
        gtk_text_buffer_insert(v->buffer, &it, ps, (gint)(pe - ps));
    } else {
        GtkTextChildAnchor *anchor =
            gtk_text_buffer_create_child_anchor(v->buffer, &it);
        switch (seg->kind) {
        case UNDO_SEG_IMAGE:
            on_anchor_set_image(anchor, seg->pixbuf, seg->display_width);
            attach_image_widget(v, anchor);
            break;
        case UNDO_SEG_CHECK:
            on_anchor_set_checkbox(anchor, seg->checked);
            attach_checkbox_widget(v, anchor);
            break;
        case UNDO_SEG_TABLE:
            on_anchor_set_table(anchor, undo_table_copy(seg->table));
            attach_table_widget(v, anchor);
            break;
        default:
            break;
        }
    }

    GtkTextIter ss, se;              /* the span just inserted              */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &ss, at);
    gtk_text_buffer_get_iter_at_offset(v->buffer, &se, at + n);
    for (gsize t = 0; t < on_n_flag_tags; t++) {
        if (seg->flags & on_flag_tags[t].flag)
            gtk_text_buffer_apply_tag_by_name(
                v->buffer, on_flag_tags[t].tag_name, &ss, &se);
        else
            gtk_text_buffer_remove_tag_by_name(
                v->buffer, on_flag_tags[t].tag_name, &ss, &se);
    }
    gtk_text_buffer_remove_tag_by_name(v->buffer, "on-search-hit",
                                       &ss, &se);
    return n;
}

/* ---------------------------------------------------------------------------
 * undo_restore() — take the buffer from state `from` (its present
 * content) to state `to`, touching as little as possible: the common
 * prefix and suffix stay in place, only the differing middle is deleted
 * and re-inserted (recreating any anchors + widgets inside it).  Then
 * re-run the emoji padding over the seam, put the cursor back, and
 * re-adopt the inline style there.  Runs with history recording
 * suppressed; the change still autosaves.
 * ------------------------------------------------------------------------- */
static void
undo_restore(OnNoteView *v, const UndoSnap *from, const UndoSnap *to)
{
    if (v->tag_start != NULL)       /* abandon a half-typed #tag           */
        tag_capture_end(v, FALSE);

    v->undo_restoring = TRUE;
    v->internal_change++;

    gint buf_len = gtk_text_buffer_get_char_count(v->buffer);
    gint len_to  = undo_snap_len(to);

    gint prefix = undo_common_prefix(from, to);
    gint suffix = undo_common_suffix(from, to);
    /* The two regions must never overlap ("aaaa" → "aaa" counts 3+3).      */
    gint max_common = MIN(undo_snap_len(from), len_to) - prefix;
    suffix = CLAMP(suffix, 0, max_common);
    /* Belt and braces vs. the live buffer (from should always match it).   */
    prefix = MIN(prefix, buf_len);
    suffix = MIN(suffix, buf_len - prefix);

    /* Replace buffer [prefix, buf_len - suffix)
     * with `to`'s   [prefix, len_to - suffix).                             */
    GtkTextIter ds, de;              /* doomed middle range                 */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &ds, prefix);
    gtk_text_buffer_get_iter_at_offset(v->buffer, &de, buf_len - suffix);
    if (!gtk_text_iter_equal(&ds, &de))
        gtk_text_buffer_delete(v->buffer, &ds, &de);

    gint at       = prefix;          /* insertion offset                    */
    gint want_end = len_to - suffix; /* target chars [prefix, want_end)     */
    gint pos      = 0;               /* char offset within `to`             */
    for (guint i = 0; i < to->segs->len && pos < want_end; i++) {
        const UndoSeg *seg = g_ptr_array_index(to->segs, i);
        gint slen = undo_seg_len(seg);
        gint s    = MAX(prefix - pos, 0);    /* slice within this segment   */
        gint e    = MIN(want_end - pos, slen);
        if (s < e)
            at += undo_insert_seg_slice(v, seg, s, e - s, at);
        pos += slen;
    }

    /* Editor-only emoji padding is never part of snapshots — refresh it
     * over the replaced span plus one char each side (the pad tag also
     * covers the char after an emoji).                                     */
    gint emo_s = MAX(prefix - 1, 0);
    gint emo_e = MIN(at + 1, gtk_text_buffer_get_char_count(v->buffer));
    if (emo_s < emo_e) {
        GtkTextIter es, ee;          /* emoji-refresh span                  */
        gtk_text_buffer_get_iter_at_offset(v->buffer, &es, emo_s);
        gtk_text_buffer_get_iter_at_offset(v->buffer, &ee, emo_e);
        gtk_text_buffer_remove_tag_by_name(v->buffer, "on-emoji",
                                           &es, &ee);
        /* Neither the emoji padding nor the action tint is snapshotted.    */
        note_view_rederive(v, emo_s, emo_e, RD_EMOJI | RD_ACTION);
    }
    /* Nor is the title centering: the restored text comes back untagged,
     * so re-derive it over the head of the buffer.                         */
    title_line_sync(v, 0, 0);

    GtkTextIter cur;                 /* restored cursor position            */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &cur, to->cursor);
    gtk_text_buffer_place_cursor(v->buffer, &cur);

    v->internal_change--;

    /* Re-adopt the inline style at the restored cursor (the guarded
     * cursor-position notify never saw the move).                          */
    v->inline_flags = gtk_text_iter_backward_char(&cur)
                      ? on_flags_at_iter(v->buffer, &cur, ON_FMT_INLINE_MASK)
                      : 0;
    note_view_flags_changed(v);

    note_view_edited(v);       /* undo_restoring suppresses grouping  */
    code_buttons_queue_rebuild(v);
    v->undo_restoring = FALSE;

    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(v),
                                 gtk_text_buffer_get_insert(v->buffer),
                                 0.1, FALSE, 0.0, 0.0);
}

/* ---------------------------------------------------------------------------
 * on_note_view_undo() / on_note_view_redo() — the host's "win.undo" /
 * "win.redo".  Undo first flushes the in-progress group so the very latest
 * edits are what gets undone.  tags_modified goes up whenever either side
 * of the swap contains #tag spans — the restore may change the note's tag
 * set.
 * ------------------------------------------------------------------------- */
void
on_note_view_undo(OnNoteView *v)
{
    if (v->undo_commit_source != 0)
        undo_commit_now(v);
    if (v->undo_stack->len == 0)
        return;

    UndoSnap *snap =                 /* state we are going back to          */
        g_ptr_array_index(v->undo_stack, v->undo_stack->len - 1);
    g_ptr_array_remove_index(v->undo_stack, v->undo_stack->len - 1);

    UndoSnap *from = v->undo_current;   /* the buffer's present state      */
    if (snap->has_tags || from->has_tags)
        v->tags_modified = TRUE;
    g_ptr_array_add(v->redo_stack, from);
    v->undo_current = snap;
    undo_restore(v, from, snap);
}

void
on_note_view_redo(OnNoteView *v)
{
    if (v->undo_commit_source != 0)
        undo_commit_now(v);         /* may clear the redo stack            */
    if (v->redo_stack->len == 0)
        return;

    UndoSnap *snap =                 /* state we are going forward to       */
        g_ptr_array_index(v->redo_stack, v->redo_stack->len - 1);
    g_ptr_array_remove_index(v->redo_stack, v->redo_stack->len - 1);

    UndoSnap *from = v->undo_current;   /* the buffer's present state      */
    if (snap->has_tags || from->has_tags)
        v->tags_modified = TRUE;
    g_ptr_array_add(v->undo_stack, from);
    v->undo_current = snap;
    undo_restore(v, from, snap);
}

/* undo_free_history() — release both stacks and the current snapshot.       */
static void
undo_free_history(OnNoteView *v)
{
    if (v->undo_stack != NULL) {
        for (guint i = 0; i < v->undo_stack->len; i++)
            undo_snap_free(g_ptr_array_index(v->undo_stack, i));
        g_ptr_array_free(v->undo_stack, TRUE);
        v->undo_stack = NULL;
    }
    if (v->redo_stack != NULL) {
        for (guint i = 0; i < v->redo_stack->len; i++)
            undo_snap_free(g_ptr_array_index(v->redo_stack, i));
        g_ptr_array_free(v->redo_stack, TRUE);
        v->redo_stack = NULL;
    }
    g_clear_pointer(&v->undo_current, undo_snap_free);
}

/* ===========================================================================
 * buffer signal handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_buffer_insert_text_before() — runs BEFORE an insertion is carried
 * out.  Marks short (typed) insertions so on_cursor_moved() can tell the
 * resulting cursor jump apart from real navigation: GtkTextBuffer emits
 * notify::cursor-position INSIDE its insert-text class handler — before
 * on_buffer_insert_text_after() has applied v->inline_flags to the new
 * text — so adopting the new character's (still untagged) style there
 * would clear a style armed via Ctrl/Cmd+B with no selection.  The same
 * ≤2-char threshold as the after-handler's enforcement keeps pastes on
 * the old adopt-from-buffer path.
 * ------------------------------------------------------------------------- */
static void
on_buffer_insert_text_before(GtkTextBuffer *buffer, GtkTextIter *location,
                             gchar *text, gint len, gpointer user_data)
{
    (void)buffer; (void)location;
    OnNoteView *v = user_data;       /* the view                            */
    if (v->internal_change > 0)
        return;
    if (g_utf8_strlen(text, len) <= 2)
        v->typing_insert = TRUE;
}

/* ---------------------------------------------------------------------------
 * on_buffer_insert_text_after() — runs after every insertion.  Three jobs:
 *   1. enforce v->inline_flags on short (typed) insertions,
 *   2. start a tag capture when '#' is typed at a word boundary,
 *   3. advance/close an active tag capture as the user keeps typing.
 * ------------------------------------------------------------------------- */
static void
on_buffer_insert_text_after(GtkTextBuffer *buffer, GtkTextIter *location,
                            gchar *text, gint len, gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    v->typing_insert = FALSE;       /* set by the before-handler; the
                                      * insertion is fully processed once
                                      * this handler runs                   */
    if (v->internal_change > 0)
        return;

    glong n_chars = g_utf8_strlen(text, len);
    gint  end_off = gtk_text_iter_get_offset(location);

    /* Pad any emoji in the insertion so they don't overlap neighbours,
     * and re-derive the action-line tint for every line the insertion
     * touched (a paste can create or split '!' lines anywhere).            */
    note_view_rederive(v, end_off - (gint)n_chars, end_off,
                    RD_EMOJI | RD_ACTION | RD_TITLE);
    /* A '!' or newline can create an action line; a paste can too.          */
    if (n_chars > 1 || memchr(text, '!', (size_t)len) ||
        memchr(text, '\n', (size_t)len))
        v->actions_modified = TRUE;
    gtk_text_buffer_get_iter_at_offset(v->buffer, location, end_off);

    /* Typing INSIDE an existing styled #tag renames it — flag the tag set
     * as changed so the next save updates note_tags and the sidebar.       */
    if (!v->tags_modified) {
        GtkTextIter ins_s;           /* first inserted character            */
        gtk_text_buffer_get_iter_at_offset(buffer, &ins_s,
                                           end_off - (gint)n_chars);
        GtkTextTag *ttag = lookup_tag(buffer, ON_TAGNAME_TAG);
        if (ttag != NULL && gtk_text_iter_has_tag(&ins_s, ttag))
            v->tags_modified = TRUE;
    }

    /* --- job 1: make typed text obey the current inline style ---------- */
    if (n_chars <= 2) {
        GtkTextIter span_s;          /* start of the inserted span          */
        gtk_text_buffer_get_iter_at_offset(buffer, &span_s,
                                           end_off - (gint)n_chars);
        v->internal_change++;
        for (gsize i = 0; i < on_n_flag_tags; i++) {
            if (!(on_flag_tags[i].flag & ON_FMT_INLINE_MASK))
                continue;
            if (v->inline_flags & on_flag_tags[i].flag)
                gtk_text_buffer_apply_tag_by_name(
                    buffer, on_flag_tags[i].tag_name, &span_s, location);
            else
                gtk_text_buffer_remove_tag_by_name(
                    buffer, on_flag_tags[i].tag_name, &span_s, location);
        }
        v->internal_change--;
        /* location may have been invalidated by tag ops; refresh it.       */
        gtk_text_buffer_get_iter_at_offset(buffer, location, end_off);
    }

    /* --- job 1b: paragraph-style line continuity ------------------------ */
    /* Inserted text only inherits tags that span BOTH sides of the insert
     * point, so typing at the start of a styled line — in particular on
     * an EMPTY styled line, whose only character is its tagged trailing
     * newline — lands untagged and the line falls out of its style.
     * Re-assert the line's style over the whole span (probing the
     * trailing newline when the line start is the fresh untagged text).
     * Exception: Enter at the end of a heading leaves an empty line whose
     * newline inherited the heading tag — strip it instead, so the next
     * line starts as body text (headings are single-line; code blocks by
     * contrast grow line by line).
     * For pastes (n_chars > 2) starting at line offset 0: same gravity
     * problem — the start mark slides right, leaving pasted text before
     * the tag.  Re-assert from the paste-start line.  Code blocks cover
     * every pasted line; headings cover only the starting line.         */
    if (n_chars <= 2) {
        GtkTextIter ls, le;          /* cursor line incl. trailing newline  */
        line_span(buffer, gtk_text_iter_get_line(location), &ls, &le);
        guint32 para = line_para_flags(buffer, &ls);
        if (para == 0 && !gtk_text_iter_equal(&ls, &le)) {
            GtkTextIter nl = le;     /* last char of the span (the newline) */
            gtk_text_iter_backward_char(&nl);
            para = on_flags_at_iter(buffer, &nl, ON_FMT_PARA_MASK);
        }
        if (para != 0) {
            v->internal_change++;
            if ((para & (ON_FMT_H1 | ON_FMT_H2)) &&
                memchr(text, '\n', len) != NULL &&
                gtk_text_iter_ends_line(&ls)) {
                gtk_text_buffer_remove_tag_by_name(buffer, ON_TAGNAME_H1,
                                                   &ls, &le);
                gtk_text_buffer_remove_tag_by_name(buffer, ON_TAGNAME_H2,
                                                   &ls, &le);
            } else {
                gtk_text_buffer_apply_tag_by_name(
                    buffer, on_tag_name_for_flag(para), &ls, &le);
            }
            v->internal_change--;
            gtk_text_buffer_get_iter_at_offset(buffer, location, end_off);
        }
    } else {
        GtkTextIter ps;              /* first character of the paste        */
        gtk_text_buffer_get_iter_at_offset(buffer, &ps,
                                           end_off - (gint)n_chars);
        if (gtk_text_iter_get_line_offset(&ps) == 0) {
            GtkTextIter ps_ls, ps_le; /* span of the paste-start line      */
            line_span(buffer, gtk_text_iter_get_line(&ps), &ps_ls, &ps_le);
            guint32 para = line_para_flags(buffer, &ps_ls);
            if (para == 0) {
                /* The paragraph tag's start mark slid right past the
                 * paste, so the line start is untagged.  The original
                 * '\n' that carried the tag was at P and is now at
                 * P + n_chars = end_off — probe that position.        */
                GtkTextIter orig_nl;
                gtk_text_buffer_get_iter_at_offset(buffer, &orig_nl,
                                                   end_off);
                para = on_flags_at_iter(buffer, &orig_nl, ON_FMT_PARA_MASK);
            }
            if (para != 0) {
                GtkTextIter apply_end;
                if (para == ON_FMT_CODEBLOCK) {
                    /* multi-line: cover all pasted lines                */
                    GtkTextIter dummy;
                    line_span(buffer, gtk_text_iter_get_line(location),
                              &dummy, &apply_end);
                } else {
                    apply_end = ps_le; /* heading is single-line         */
                }
                v->internal_change++;
                gtk_text_buffer_apply_tag_by_name(
                    buffer, on_tag_name_for_flag(para), &ps_ls, &apply_end);
                v->internal_change--;
                gtk_text_buffer_get_iter_at_offset(buffer, location, end_off);
            }
        }
    }

    /* --- job 1c: pasted text can carry #tag styling onto a code-block
     * line (same-buffer paste keeps tags), and typing inside a span kept
     * from before the no-tags-in-code-blocks rule extends it — text in a
     * code block is never a tag, so strip the styling.                      */
    if (note_view_rederive(v, end_off - (gint)n_chars, end_off, RD_CODE_TAG))
        v->tags_modified = TRUE;

    /* --- undo group caps: a linebreak closes the pending group at once,
     * and so does the UNDO_MAX_SENTENCES-th sentence ender, so one long
     * unbroken burst can't become a single giant undo step ---------------- */
    {
        gboolean cap_hit = FALSE;    /* commit the group after this char?   */
        for (const gchar *p = text; p < text + len;
             p = g_utf8_next_char(p)) {
            gunichar c = g_utf8_get_char(p);
            if (c == '\n') {
                cap_hit = TRUE;
                break;
            }
            if ((c == '.' || c == '?') &&
                ++v->undo_sentences >= UNDO_MAX_SENTENCES) {
                cap_hit = TRUE;
                break;
            }
        }
        if (cap_hit)
            undo_commit_now(v);     /* also resets the sentence counter    */
    }

    /* --- jobs 2 & 3: tag capture ---------------------------------------- */
    if (v->tag_start == NULL) {
        /* Start capture on a freshly typed '#' at a word boundary — but
         * never inside a code block, where '#' is comment/preprocessor
         * syntax, not a tag.                                               */
        if (n_chars == 1 && text[0] == '#') {
            GtkTextIter hash;        /* position of the '#'                 */
            gtk_text_buffer_get_iter_at_offset(buffer, &hash, end_off - 1);
            if (line_para_flags(buffer, &hash) & ON_FMT_CODEBLOCK)
                return;
            GtkTextIter before = hash;
            gboolean at_boundary = !gtk_text_iter_backward_char(&before) ||
                g_unichar_isspace(gtk_text_iter_get_char(&before));
            if (at_boundary) {
                v->tag_start = gtk_text_buffer_create_mark(
                    buffer, NULL, &hash, TRUE /* left gravity */);
                /* One query per capture; every keystroke inside the
                 * capture filters this snapshot in memory instead of
                 * re-querying SQLite (a round trip on network DBs).       */
                v->tag_choices = on_db_tag_list(v->app->db);
                tag_popup_update(v);
            }
        }
        return;
    }

    /* Capture active: whitespace ends the tag, word chars refresh the
     * popup, anything else (punctuation) also ends it.                     */
    if (n_chars == 1) {
        gunichar c = g_utf8_get_char(text);
        if (g_unichar_isspace(c)) {
            tag_capture_end(v, TRUE);
        } else if (g_unichar_isalnum(c) || c == '_' || c == '-') {
            tag_popup_update(v);
        } else {
            tag_capture_end(v, TRUE);
        }
    } else {
        /* Multi-char insertion (paste) during capture: just end it.        */
        tag_capture_end(v, TRUE);
    }
}

/* ---------------------------------------------------------------------------
 * on_buffer_delete_range_before() — runs BEFORE a deletion is carried out
 * (afterwards the removed text is unknowable): drop the action-item
 * identity marks the deletion takes with it, and if the doomed range
 * touches a styled #tag, a tag is being deleted or renamed, so flag the
 * tag set as changed for the next save.
 * ------------------------------------------------------------------------- */
static void
on_buffer_delete_range_before(GtkTextBuffer *buffer, GtkTextIter *start,
                              GtkTextIter *end, gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */

    /* Marks are pruned even for internal changes: an undo replaces the
     * whole buffer, and marks left over from that would all collapse to
     * offset 0 and then mis-hint the first item.                           */
    action_marks_prune(v, start, end);

    if (v->internal_change > 0)
        return;

    /* A deletion spanning lines JOINS them: remember the first line's
     * paragraph style so the after-handler can keep it in charge of the
     * merged line (see join_para in the OnNoteView banner).                 */
    v->join_para = -1;
    if (gtk_text_iter_get_line(start) != gtk_text_iter_get_line(end)) {
        GtkTextIter first = *start;  /* the line the cursor lands in        */
        gtk_text_iter_set_line_offset(&first, 0);
        v->join_para = (gint)line_para_flags(buffer, &first);
    }

    if (v->tags_modified)
        return;

    GtkTextTag *tag = lookup_tag(buffer, ON_TAGNAME_TAG);
    if (tag == NULL)
        return;

    if (gtk_text_iter_has_tag(start, tag) ||
        gtk_text_iter_has_tag(end, tag)) {
        v->tags_modified = TRUE;    /* range starts or ends inside a tag   */
        return;
    }
    /* Any tag toggle strictly inside the range?                            */
    GtkTextIter it = *start;
    if (gtk_text_iter_forward_to_tag_toggle(&it, tag) &&
        gtk_text_iter_compare(&it, end) < 0)
        v->tags_modified = TRUE;
}

/* ---------------------------------------------------------------------------
 * on_buffer_delete_range_after() — after a deletion, re-derive the action
 * tint of the merged line (removing a '!' or a newline can create or
 * destroy an action line), then cancel any active tag capture whose '#'
 * was removed, or refresh the popup otherwise.
 * ------------------------------------------------------------------------- */
static void
on_buffer_delete_range_after(GtkTextBuffer *buffer, GtkTextIter *start,
                             GtkTextIter *end, gpointer user_data)
{
    (void)end;
    OnNoteView *v = user_data;       /* the view                            */
    if (v->internal_change > 0)
        return;

    /* start == end after the deletion: retag the collapse-point line.      */
    gint off = gtk_text_iter_get_offset(start);

    /* Lines were joined: the merged line's newline came from the SECOND
     * line, so its paragraph tag must be replaced by the first line's.    */
    if (v->join_para >= 0) {
        guint32 flags = (guint32)v->join_para;
        v->join_para = -1;
        GtkTextIter ls, le;          /* the merged line incl. its newline   */
        line_span(buffer, gtk_text_iter_get_line(start), &ls, &le);
        v->internal_change++;
        for (gsize i = 0; i < on_n_flag_tags; i++)
            if (on_flag_tags[i].flag & ON_FMT_PARA_MASK)
                gtk_text_buffer_remove_tag_by_name(
                    buffer, on_flag_tags[i].tag_name, &ls, &le);
        if (flags != 0)
            gtk_text_buffer_apply_tag_by_name(
                buffer, on_tag_name_for_flag(flags), &ls, &le);
        v->internal_change--;
        gtk_text_buffer_get_iter_at_offset(buffer, start, off);
    }
    /* A merge can pull a line up to 0, so the title look is re-derived too. */
    note_view_rederive(v, off, off, RD_ACTION | RD_TITLE);
    v->actions_modified = TRUE;    /* deletes can create/destroy '!' lines   */

    if (v->tag_start == NULL)
        return;
    GtkTextIter s, e;                /* revalidated capture span            */
    if (!tag_capture_span(v, &s, &e))
        tag_capture_end(v, FALSE);  /* '#' itself was deleted              */
    else
        tag_popup_update(v);
}

/* on_scroll_idle() — deferred caret-follow scroll (see on_buffer_changed). */
static gboolean
on_scroll_idle(gpointer user_data)
{
    OnNoteView *v = user_data;       /* the view                            */
    v->scroll_idle = 0;
    gtk_text_view_scroll_to_mark(
        GTK_TEXT_VIEW(v), gtk_text_buffer_get_insert(v->buffer),
        0.08, FALSE, 0.0, 0.0);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * note_view_scroll_to_caret() — keep the caret comfortably above the
 * window's bottom edge: within_margin makes the view scroll a little AHEAD
 * of the cursor instead of letting it ride the very last pixel row.
 * Deferred to an idle — at "changed" time the text layout has not
 * revalidated yet, so an immediate scroll computes against stale extents
 * and does nothing at the end of the document.  Called by the "changed"
 * handler for typing, and DIRECTLY by the anchor inserts (image, table):
 * those run under internal_change, which keeps the handler's scroll off,
 * and a table inserted on the last line otherwise sat below the viewport.
 * ------------------------------------------------------------------------- */
static void
note_view_scroll_to_caret(OnNoteView *v)
{
    if (v->scroll_idle == 0)
        v->scroll_idle = g_idle_add(on_scroll_idle, v);
}

/* note_view_scroll_after_layout() — the anchor inserts' variant: scroll
 * from the next size_allocate, once the new child has been measured (see
 * scroll_on_allocate in the private struct).                               */
static void
note_view_scroll_after_layout(OnNoteView *v)
{
    v->scroll_on_allocate = TRUE;
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

/* ---------------------------------------------------------------------------
 * on_buffer_changed() — any edit is reported to the host (its autosave).
 * ------------------------------------------------------------------------- */
static void
on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    (void)buffer;
    OnNoteView *v = user_data;       /* the view                            */
    note_view_edited(v);
    /* Text edits can grow, shrink, split or remove code blocks.            */
    code_buttons_queue_rebuild(v);

    if (v->internal_change == 0)
        note_view_scroll_to_caret(v);
}

/* ---------------------------------------------------------------------------
 * on_cursor_moved() — "notify::cursor-position" handler.  Refreshes the
 * inline-style state from the character left of the new cursor position
 * (so the toolbar reflects where you are), and closes a tag capture when
 * the cursor leaves the tag being typed.
 * ------------------------------------------------------------------------- */
static void
on_cursor_moved(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object; (void)pspec;
    OnNoteView *v = user_data;       /* the view                            */
    if (v->internal_change > 0)
        return;

    GtkTextIter cursor;              /* new cursor position                 */
    gtk_text_buffer_get_iter_at_mark(v->buffer, &cursor,
                                     gtk_text_buffer_get_insert(v->buffer));

    /* Close the capture if the cursor moved outside the typed tag.         */
    if (v->tag_start != NULL) {
        GtkTextIter s, e;            /* capture span                        */
        if (!tag_capture_span(v, &s, &e) ||
            gtk_text_iter_compare(&cursor, &s) < 0 ||
            gtk_text_iter_compare(&cursor, &e) > 0)
            tag_capture_end(v, TRUE);
    }

    /* Adopt the style of the character to the left of the cursor — but
     * NOT when the move comes from typing: this notify fires inside the
     * insert-text class handler, before the after-handler has applied
     * v->inline_flags to the new character, so probing it here would
     * clear a style armed with no selection (Ctrl/Cmd+B, then type).      */
    if (v->typing_insert)
        return;
    GtkTextIter probe = cursor;      /* the char whose style we adopt       */
    if (gtk_text_iter_backward_char(&probe))
        v->inline_flags =
            on_flags_at_iter(v->buffer, &probe, ON_FMT_INLINE_MASK);
    else
        v->inline_flags = 0;
    note_view_flags_changed(v);
}

/* ---------------------------------------------------------------------------
 * on_view_key_pressed() — the view's key controller, CAPTURE phase so it
 * runs before GtkTextView's own bubble-phase controller (and its input
 * method): tag-popup navigation, Escape and Enter-in-list.  The Primary+key
 * shortcuts are NOT here: they are application accelerators on the "win."
 * actions (on_app_install_accels), which the window fires before the view
 * ever sees the key.
 * Returns TRUE when the key was fully handled here.
 * ------------------------------------------------------------------------- */
static gboolean
on_view_key_pressed(GtkEventControllerKey *controller, guint keyval,
                    guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller; (void)keycode; (void)state;
    OnNoteView *v = user_data;       /* the view                            */

    /* While the tag popup is visible it owns the navigation keys.          */
    if (v->tag_start != NULL && v->tag_popup != NULL &&
        gtk_widget_get_visible(v->tag_popup)) {
        switch (keyval) {
        case GDK_KEY_Down:
            tag_popup_move_selection(v, +1);
            return TRUE;
        case GDK_KEY_Up:
            tag_popup_move_selection(v, -1);
            return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab: {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(
                GTK_LIST_BOX(v->tag_listbox));
            if (row != NULL) {
                on_tag_row_activated(GTK_LIST_BOX(v->tag_listbox),
                                     row, v);
                return TRUE;
            }
            break;
        }
        case GDK_KEY_Escape:
            tag_capture_end(v, FALSE);
            return TRUE;
        default:
            break;
        }
    } else if (v->tag_start != NULL && keyval == GDK_KEY_Escape) {
        tag_capture_end(v, FALSE);
        return TRUE;
    }

    /* Enter inside a list item: continue or end the list.                  */
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (v->tag_start != NULL)
            tag_capture_end(v, TRUE);
        if (handle_return_in_list(v)) {
            undo_commit_now(v);     /* linebreak: close the undo group
                                        (list Enter inserts internally, so
                                        the insert-text cap never sees it)  */
            return TRUE;
        }
    }

    return FALSE;
}

/* ===========================================================================
 * in-note find
 *
 * The host's search entry highlights every case-insensitive match with the
 * "on-search-hit" tag as you type; Enter (or the entry's arrow buttons)
 * jumps to the next match, wrapping at the end.  The matching is GTK's
 * (gtk_text_iter_forward_search); the highlight tag is ours.
 * =========================================================================== */

/* Flags shared by every in-note text search.                                */
#define FIND_FLAGS (GTK_TEXT_SEARCH_CASE_INSENSITIVE | \
                    GTK_TEXT_SEARCH_TEXT_ONLY)

/* ---------------------------------------------------------------------------
 * on_note_view_find() — drop all match highlighting, then re-highlight
 * every match of `text` (none for NULL or "").
 * ------------------------------------------------------------------------- */
void
on_note_view_find(OnNoteView *v, const gchar *text)
{
    GtkTextIter s, e;                /* whole-buffer bounds                 */
    gtk_text_buffer_get_bounds(v->buffer, &s, &e);
    gtk_text_buffer_remove_tag_by_name(v->buffer, "on-search-hit",
                                       &s, &e);

    if (text == NULL || *text == '\0')
        return;

    GtkTextIter it;                  /* scan position                       */
    gtk_text_buffer_get_start_iter(v->buffer, &it);
    GtkTextIter match_s, match_e;    /* bounds of one match                 */
    while (gtk_text_iter_forward_search(&it, text, FIND_FLAGS,
                                        &match_s, &match_e, NULL)) {
        gtk_text_buffer_apply_tag_by_name(v->buffer, "on-search-hit",
                                          &match_s, &match_e);
        it = match_e;
    }
}

/* ---------------------------------------------------------------------------
 * on_note_view_find_step() — select and scroll to the next match of `text`
 * after the cursor (forward) or the previous one before it (backward),
 * wrapping around the buffer when none remains in that direction.
 * ------------------------------------------------------------------------- */
void
on_note_view_find_step(OnNoteView *v, const gchar *text, gboolean forward)
{
    if (text == NULL || *text == '\0')
        return;

    /* Start just past the current selection in the travel direction.       */
    GtkTextIter from;                /* search start                        */
    gtk_text_buffer_get_iter_at_mark(
        v->buffer, &from,
        forward ? gtk_text_buffer_get_selection_bound(v->buffer)
                : gtk_text_buffer_get_insert(v->buffer));

    GtkTextIter match_s, match_e;    /* bounds of the found match           */
    gboolean found = forward
        ? gtk_text_iter_forward_search(&from, text, FIND_FLAGS,
                                       &match_s, &match_e, NULL)
        : gtk_text_iter_backward_search(&from, text, FIND_FLAGS,
                                        &match_s, &match_e, NULL);
    if (!found) {                    /* wrap around                         */
        if (forward)
            gtk_text_buffer_get_start_iter(v->buffer, &from);
        else
            gtk_text_buffer_get_end_iter(v->buffer, &from);
        found = forward
            ? gtk_text_iter_forward_search(&from, text,
                                           FIND_FLAGS,
                                           &match_s, &match_e, NULL)
            : gtk_text_iter_backward_search(&from, text,
                                            FIND_FLAGS,
                                            &match_s, &match_e, NULL);
    }
    if (!found)
        return;

    gtk_text_buffer_select_range(v->buffer, &match_s, &match_e);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(v), &match_s, 0.1,
                                 FALSE, 0.0, 0.0);
}

/* ---------------------------------------------------------------------------
 * on_note_view_image_reveal() — put the caret at the note's `ord`-th
 * embedded image and scroll it into view, a third of the way down the
 * window.  The ordinal counts image anchors in buffer order, which is
 * exactly the order on_note_count_images()/on_note_image_nth() count IMAGE
 * records in — that is what lets the media window address a thumbnail's
 * source by number.
 *   v   — the view to scroll.
 *   ord — 0-based image ordinal; < 0 is a no-op.
 * Returns TRUE when the image was found and revealed.
 * ------------------------------------------------------------------------- */
gboolean
on_note_view_image_reveal(OnNoteView *v, gint ord)
{
    gint offset;                     /* the image's buffer offset           */
    if (image_nth_anchor(v, ord, &offset) == NULL)
        return FALSE;

    GtkTextIter it;                  /* the image's position                */
    gtk_text_buffer_get_iter_at_offset(v->buffer, &it, offset);
    /* Caret only — no selection: the user never asked for one, and the next
     * keystroke would replace the image.                                   */
    gtk_text_buffer_place_cursor(v->buffer, &it);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(v), &it, 0.0, TRUE, 0.0, 0.3);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * note_view_install_css() — the engine's DISPLAY-level stylesheet, installed
 * once per process (application priority, so every rule outranks the
 * theme's whatever the widget's state).  One rule per "notes-" class the
 * view puts on its widgets; a class-scoped selector reaches the sub-nodes
 * (`> check`, `> text`) the old per-widget providers reached implicitly.
 *
 * 1. Task checkboxes: the bare indicator, no theme padding around it
 *    (attach_checkbox_widget).
 * 2. Table header cells: bold on a light grey fill.  GTK4 paints the text
 *    on the view's `text` child (the theme makes it transparent), so both
 *    nodes get the colour.
 * 3. Table cell frames: square — the theme rounds every frame 8 px.
 * 4. The empty-buffer title caret (title_view_sync): 160% matches the
 *    on-title-size tag's 1.6 scale.  The class sits on the `textview` node
 *    itself, since the view takes its default font from its own style.
 * ------------------------------------------------------------------------- */
static void
note_view_install_css(void)
{
    static gboolean installed = FALSE;
    if (installed)
        return;
    installed = TRUE;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "checkbutton.notes-task-check { padding: 0; min-height: 0; }"
        "checkbutton.notes-task-check > check { margin: 0; }"
        "textview.notes-table-header, textview.notes-table-header > text {"
        "  background-color: #ececec;"
        "  font-weight: bold;"
        "}"
        "frame.notes-table-cell { border-radius: 0; }"
        "textview.on-title-empty { font-size: 160%; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ===========================================================================
 * the "view." actions
 *
 * The image and table context menus name actions on the VIEW's own group,
 * inserted on the widget as "view": the view owns its menus, so a host
 * defines none of these.  Nothing here is gated on the focus — every one
 * acts on v->ctx_offset, and a table's menu runs with the focus in a CELL.
 * =========================================================================== */

/* Action name → handler for the plain (stateless) "view." actions.         */
static const struct {
    const gchar *name;
    void       (*activate)(GSimpleAction *, GVariant *, gpointer);
} VIEW_ACTIONS[] = {
    { "img-copy",      on_img_copy      },
    { "img-open",      on_img_open      },
    { "img-full",      on_img_full      },
    { "img-thumb",     on_img_thumb     },
    { "table-row-add", on_table_command },
    { "table-row-del", on_table_command },
    { "table-col-add", on_table_command },
    { "table-col-del", on_table_command },
    { "table-delete",  on_table_command },
};

/* ---------------------------------------------------------------------------
 * note_view_install_actions() — build v->actions: the table above, plus the
 * stateful "table-header" behind the table menu's check item, and insert
 * the group on the view as "view".
 * ------------------------------------------------------------------------- */
static void
note_view_install_actions(OnNoteView *v)
{
    v->actions = g_simple_action_group_new();
    GActionMap *map = G_ACTION_MAP(v->actions);
    for (gsize i = 0; i < G_N_ELEMENTS(VIEW_ACTIONS); i++) {
        GSimpleAction *action = g_simple_action_new(VIEW_ACTIONS[i].name,
                                                    NULL);
        g_signal_connect(action, "activate",
                         G_CALLBACK(VIEW_ACTIONS[i].activate), v);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
    }

    GSimpleAction *header = g_simple_action_new_stateful(
        "table-header", NULL, g_variant_new_boolean(FALSE));
    g_signal_connect(header, "change-state",
                     G_CALLBACK(on_table_header_change_state), v);
    g_action_map_add_action(map, G_ACTION(header));
    g_object_unref(header);

    gtk_widget_insert_action_group(GTK_WIDGET(v), "view",
                                   G_ACTION_GROUP(v->actions));
}

GActionGroup *
on_note_view_action_group(OnNoteView *v)
{
    return G_ACTION_GROUP(v->actions);
}

/* ===========================================================================
 * construction, loading and teardown
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_note_view_init() — GObject instance init: the buffer's tag set (the
 * standard one plus the editor-only tags), the view's margins and
 * wrapping, the buffer signal handlers, the view's controllers and the
 * "view." actions.  Everything that does not need `app`, which
 * on_note_view_new() sets right after.
 *
 * The view's key controller and click gesture run in the CAPTURE phase:
 * GtkTextView's own controllers are bubble-phase, and both of ours must
 * see the event first (Enter-in-list before the view inserts a newline, a
 * right press before the view builds its popup).
 * ------------------------------------------------------------------------- */
static void
on_note_view_init(OnNoteView *v)
{
    v->join_para        = -1;
    v->actions_modified = TRUE;      /* nothing extracted yet               */
    /* Identity marks for the '!' lines; seeded by the host after load.     */
    v->action_marks = g_ptr_array_new_with_free_func(g_free);

    v->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(v));
    /* ONE undo: the view's snapshot history (see the undo section), not
     * the buffer's text-only one — which would otherwise also record every
     * edit and answer the view's own Undo menu item and Ctrl+Z binding.   */
    gtk_text_buffer_set_enable_undo(v->buffer, FALSE);
    on_buffer_ensure_tags(v->buffer);

    /* Editor-only highlight for in-note search matches (never appears in
     * the serializer's flag table, so it is ignored on save).              */
    gtk_text_buffer_create_tag(v->buffer, "on-search-hit",
                               "background", "#ffec8b", NULL);
    /* Editor-only padding around color-emoji glyphs (see is_emoji_char).   */
    gtk_text_buffer_create_tag(v->buffer, "on-emoji",
                               "letter-spacing", 5 * PANGO_SCALE, NULL);
    /* Editor-only: drop each task checkbox a few px so its box centers on
     * the text beside it — anchored children sit with their bottom on the
     * baseline, which parks the box's center above the text's (see
     * attach_checkbox_widget).  Never serialized.                          */
    gtk_text_buffer_create_tag(v->buffer, "on-check-drop",
                               "rise", -3 * PANGO_SCALE, NULL);
    /* Editor-only tint for '!' action lines — blue, mirroring the tags'
     * orange.  Priority 0 so a #tag span inside an action line keeps its
     * own color.  Never serialized.                                        */
    GtkTextTag *action_tag = gtk_text_buffer_create_tag(
        v->buffer, "on-action",
        "foreground", "#1a5fb4",
        "weight",     PANGO_WEIGHT_SEMIBOLD,
        NULL);
    gtk_text_tag_set_priority(action_tag, 0);
    /* Editor-only centering for the note's title line — always line 0.
     * Nothing else sets justification, so priority doesn't matter here.
     * Never serialized: only the view centers it (title_line_sync).     */
    gtk_text_buffer_create_tag(v->buffer, "on-title-center",
                               "justification", GTK_JUSTIFY_CENTER,
                               NULL);
    /* ...and its heading look, same values as ON_TAGNAME_H1 but derived, so
     * a line becomes (or stops being) the title purely by moving to or off
     * line 0.  Skipped where a real paragraph style would multiply the
     * scale — see title_line_sync.                                         */
    gtk_text_buffer_create_tag(v->buffer, "on-title-size",
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.6,
                               NULL);
    /* Caret sizing while the buffer is EMPTY, where no tag can apply: the
     * "on-title-empty" class (rule in note_view_install_css, 160% to match
     * the scale above) makes the caret already the height of the title
     * the first keystroke produces.  title_view_sync owns the class.       */

    gtk_text_view_set_editable(GTK_TEXT_VIEW(v), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v), 16);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(v), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(v), 12);
    /* Roughly one text line of bottom margin: typing on the LAST line
     * (where there is no further content to scroll ahead to) still
     * leaves a line-sized gap below the caret.                             */
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(v), 20);
    gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(v), 2);

    g_signal_connect(v->buffer, "insert-text",
                     G_CALLBACK(on_buffer_insert_text_before), v);
    g_signal_connect_after(v->buffer, "insert-text",
                           G_CALLBACK(on_buffer_insert_text_after), v);
    g_signal_connect(v->buffer, "delete-range",
                     G_CALLBACK(on_buffer_delete_range_before), v);
    g_signal_connect_after(v->buffer, "delete-range",
                           G_CALLBACK(on_buffer_delete_range_after), v);
    g_signal_connect(v->buffer, "changed",
                     G_CALLBACK(on_buffer_changed), v);
    g_signal_connect(v->buffer, "notify::cursor-position",
                     G_CALLBACK(on_cursor_moved), v);
    g_signal_connect(v, "paste-clipboard",
                     G_CALLBACK(on_paste_clipboard), v);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(on_view_key_pressed), v);
    gtk_widget_add_controller(GTK_WIDGET(v), keys);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_view_pressed), v);
    gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(click));

    note_view_install_actions(v);
}

GtkWidget *
on_note_view_new(OnApp *app)
{
    note_view_install_css();
    OnNoteView *v = g_object_new(ON_TYPE_NOTE_VIEW, NULL);
    v->app = app;
    if (app->code_line_numbers)
        note_view_apply_line_numbers(v);
    return GTK_WIDGET(v);
}

/* ---------------------------------------------------------------------------
 * on_note_view_load() — see note_view.h.  The "changed" handler is blocked
 * for the deserialize: it is the one buffer handler with no
 * internal_change guard (an internal mutation must still queue a save),
 * and a load must not read as an edit.  Every other handler steps aside
 * on internal_change.
 * ------------------------------------------------------------------------- */
void
on_note_view_load(OnNoteView *v, const guint8 *blob, gsize len)
{
    undo_free_history(v);            /* a re-load restarts the history      */
    if (blob != NULL) {
        v->internal_change++;
        g_signal_handlers_block_by_func(v->buffer, on_buffer_changed, v);
        on_note_deserialize(v->buffer, blob, len);
        g_signal_handlers_unblock_by_func(v->buffer, on_buffer_changed, v);
        note_view_attach_widgets(v);
        /* Re-apply derived tags — both are editor-only, never stored.      */
        tag_emoji_in_range(v, 0,
                           gtk_text_buffer_get_char_count(v->buffer));
        action_retag_lines(v, 0,
                           gtk_text_buffer_get_char_count(v->buffer));
        v->internal_change--;
        v->actions_modified = FALSE; /* the host snapshots the set now      */
    }
    /* Present line 0 as the title.  OUTSIDE the branch above: a note with
     * no blob is exactly the case whose caret needs the view-level
     * fallbacks, set before the window is ever shown.  Only the head of the
     * buffer needs the pass — freshly deserialized text can't be carrying
     * the editor-only tags.                                                */
    title_line_sync(v, 0, 0);

    v->undo_stack   = g_ptr_array_new();
    v->redo_stack   = g_ptr_array_new();
    v->undo_current = undo_snapshot_capture(v);

    /* Give existing code blocks their floating copy buttons.               */
    code_buttons_queue_rebuild(v);
}

guint8 *
on_note_view_serialize(OnNoteView *v, gsize *out_len)
{
    return on_note_serialize(v->buffer, out_len);
}

gchar *
on_note_view_first_line(OnNoteView *v)
{
    return on_buffer_first_line(v->buffer);
}

gboolean
on_note_view_take_tags_modified(OnNoteView *v)
{
    gboolean was = v->tags_modified;
    v->tags_modified = FALSE;
    return was;
}

gboolean
on_note_view_take_actions_modified(OnNoteView *v)
{
    gboolean was = v->actions_modified;
    v->actions_modified = FALSE;
    return was;
}

/* ---------------------------------------------------------------------------
 * on_note_view_dispose() — release everything the view owns, and take the
 * #tag popover off the view BEFORE GtkTextView's dispose walks the
 * children it knows (see the type's banner: a leftover foreign child hangs
 * that walk).  The buffer is still whole here — the marks come off it —
 * and is GtkTextView's to drop once we chain up.  Runs from the host's
 * final unref, after its close-time save, so nothing here is reachable
 * from a live window; every step tolerates a second call.
 * ------------------------------------------------------------------------- */
static void
on_note_view_dispose(GObject *object)
{
    OnNoteView *v = ON_NOTE_VIEW(object);

    if (v->code_btn_idle != 0) {
        g_source_remove(v->code_btn_idle);
        v->code_btn_idle = 0;
    }
    if (v->scroll_idle != 0) {
        g_source_remove(v->scroll_idle);
        v->scroll_idle = 0;
    }
    if (v->undo_commit_source != 0) {
        g_source_remove(v->undo_commit_source);
        v->undo_commit_source = 0;
    }
    undo_free_history(v);
    g_clear_pointer(&v->tag_popup, gtk_widget_unparent);
    v->tag_listbox = NULL;
    on_db_tag_list_free(v->tag_choices);   /* closed mid-capture           */
    v->tag_choices = NULL;
    g_slist_free(v->code_buttons);   /* the widgets die with the view       */
    v->code_buttons = NULL;
    g_slist_free(v->code_button_pool);
    v->code_button_pool = NULL;
    if (v->action_marks != NULL) {
        action_marks_clear(v);       /* marks die with the buffer anyway    */
        g_ptr_array_unref(v->action_marks);
        v->action_marks = NULL;
    }
    g_clear_object(&v->actions);

    G_OBJECT_CLASS(on_note_view_parent_class)->dispose(object);
}

/* on_note_view_class_init() — the three overrides and the four signals.    */
static void
on_note_view_class_init(OnNoteViewClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->size_allocate   = on_note_view_size_allocate;
    GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer =
        on_note_view_snapshot_layer;
    G_OBJECT_CLASS(klass)->dispose = on_note_view_dispose;

    signals[SIG_EDITED] = g_signal_new(
        "edited", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIG_INLINE_FLAGS_CHANGED] = g_signal_new(
        "inline-flags-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIG_IMAGE_ACTIVATED] = g_signal_new(
        "image-activated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
    signals[SIG_CELL_CREATED] = g_signal_new(
        "cell-created", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 1, GTK_TYPE_WIDGET);
}
