/* ===========================================================================
 * editor_window.c — WYSIWYG note editor window (implementation)
 *
 * See editor_window.h for the feature overview.  The interesting moving
 * parts in here:
 *
 *   inline formatting  — a "current flags" model: the editor tracks which
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
 *   autosave           — every buffer change re-arms a short timer; when
 *                        it fires the buffer is serialized to BNBF and
 *                        written to SQLite, and the note's tag set is
 *                        recomputed from the text.
 * =========================================================================== */

#include "editor_window.h"
#include "image_viewer.h"
#include "serialize.h"

#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <unistd.h>

/* Milliseconds of idle time after the last edit before autosaving.         */
#define AUTOSAVE_DELAY_MS 1200

/* Default size of a new editor window (client area, excluding the
 * titlebar), used when the ini carries no editor_win_w/editor_win_h.       */
#define EDITOR_WIN_DEFAULT_W 640
#define EDITOR_WIN_DEFAULT_H 509

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
 * OnEditor — all state for one open editor window.
 *
 * Fields:
 *   app             — global application context (not owned).
 *   note_id         — id of the note being edited.
 *   window          — the top-level GtkWindow.
 *   view            — the GtkTextView doing the editing.
 *   buffer          — the view's buffer; we hold an extra ref so the final
 *                     save on window destroy can still read it.
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
 *   autosave_source — GLib timeout id of the pending autosave, 0 if none.
 *   toggle_buttons  — the four inline-style GtkToggleButtons, indexed
 *                     in the same order as INLINE_TOGGLES[], used to
 *                     mirror inline_flags into the toolbar UI.
 *   tag_start       — text mark placed just before a '#' while a tag is
 *                     being typed; NULL when no capture is active.
 *   tag_popup       — GtkPopover listing matching tags (lazily built),
 *                     parented to the view and pointing at the '#'.
 *                     Non-autohide, never focusable: the view keeps the
 *                     keyboard the whole time it is up.
 *   tag_listbox     — GtkListBox inside tag_popup holding suggestions.
 *   code_buttons    — floating "copy" links, one per code block, added
 *                     as OVERLAYS of the view in buffer coordinates
 *                     (upper-right corner of each block); clicks are
 *                     hit-tested by the view's click gesture, the hand
 *                     cursor is the label's own.
 *   code_btn_idle   — idle source id for a pending code-button rebuild.
 *   ctx_offset      — buffer offset of the image or table anchor the last
 *                     context menu was opened ON: the "img-*" and
 *                     "table-*" actions act on the thing under the last
 *                     right click, read back through anchor_at_offset()
 *                     when they run (the anchor may have moved or gone).
 *   dirty           — TRUE while the note has edits the database hasn't
 *                     seen (set whenever an autosave is queued, cleared
 *                     by editor_save); closing a window with no unsaved
 *                     edits skips the final save entirely.
 *   tags_modified   — TRUE when an edit created, renamed or deleted a
 *                     styled #tag since the last save.  Maintained LIVE
 *                     by the tag-capture/insert/delete handlers, so
 *                     editor_save knows — without any buffer scan —
 *                     whether note_tags and the library's tag sidebar
 *                     need updating.
 *   status_path     — status-bar label (bottom left): the note's folder
 *                     path, same format as the library window's.  Set at
 *                     open and refreshed when the window becomes active
 *                     again, so a move made in the library shows up on
 *                     return.
 *   status_note_id  — status-bar label (bottom right): "id:N", shown
 *                     only while the statusbar_note_id setting is on
 *                     (Settings applies it live through
 *                     on_editor_status_refresh_all).
 *   status_dirty    — status-bar label at the FAR right, past every other
 *                     status message: the save-state dot, red while
 *                     `dirty` is set and green once editor_save has run.
 *                     Always visible; driven from the two places that own
 *                     `dirty`.
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

typedef struct {
    OnApp          *app;
    gint64          note_id;
    GtkWidget      *window;
    GtkTextView    *view;
    GtkTextBuffer  *buffer;

    guint32         inline_flags;
    gboolean        typing_insert;
    gint            internal_change;
    guint           autosave_source;

    GtkWidget      *toggle_buttons[4];
    GtkWidget      *toolbar;          /* the formatting toolbar              */
    GtkWidget      *toolbar_box;      /* vbox it sits in (for live rebuild)  */
    GtkWidget      *overlay;          /* stacks img_viewer over the text     */
    OnImageViewer  *img_viewer;       /* the modal panel a click on an
                                       * embedded image opens — the same
                                       * one the media browser uses         */

    GtkTextMark    *tag_start;
    GtkWidget      *tag_popup;
    GtkWidget      *tag_listbox;
    GList          *tag_choices;      /* OnTag* snapshot for the active
                                       * capture — queried once at '#',
                                       * filtered in memory per keystroke  */

    GSList         *code_buttons;
    guint           code_btn_idle;
    guint           scroll_idle;
    gint            ctx_offset;

    GtkWidget      *search_entry;
    gchar          *pending_search;   /* initial in-note query, applied once */
    guint           initial_search_idle;
    gint            pending_image;    /* image ordinal to reveal once the
                                       * fresh window is allocated, -1 = no
                                       * image was asked for (see the media
                                       * window's double-click)             */
    guint           initial_image_idle;
    gboolean        dirty;
    gboolean        tags_modified;
    GtkWidget      *status_path;
    GtkWidget      *status_note_id;
    GtkWidget      *status_dirty;    /* far-right save-state dot            */
    GList          *last_actions;    /* the action-item set last synced to
                                        the action_items table (OnActionItem
                                        list) — editor_save rewrites the
                                        table only when the freshly
                                        extracted set differs, so ordinary
                                        saves do no action work (same idea
                                        as tags_modified)                    */
    gboolean        actions_clean;  /* TRUE when no edit has occurred since
                                        the last extract that could have
                                        created or destroyed a '!' action
                                        line; the blob walk is skipped when
                                        this is set AND last_actions==NULL   */
    GPtrArray      *action_marks;   /* ActionMark* — one GtkTextMark per
                                       action line, carrying that item's
                                       stable uid.  Marks ride text edits,
                                       so they are the only signal that
                                       identifies an item whose text was
                                       REWORDED; on_db_note_set_actions
                                       takes them as match hints            */

    GPtrArray      *undo_stack;
    GPtrArray      *redo_stack;
    UndoSnap       *undo_current;
    guint           undo_commit_source;
    gboolean        undo_restoring;
    gint            undo_sentences;
} OnEditor;

/* ---------------------------------------------------------------------------
 * NotesTextView — the editor's GtkTextView subclass, private to this file.
 *
 * The one GObject subclass the GTK4 port introduces, because two things the
 * GTK3 editor did from signals are vfuncs in GTK4: painting the code-block
 * line numbers over the text ("draw" → snapshot) and re-anchoring the
 * floating copy links after a reflow ("size-allocate" → size_allocate).
 * Its dispose also unparents the #tag popover, which is a child of the view
 * that GtkTextView does not know about — and gtk_text_view_dispose LOOPS
 * FOREVER on a child it cannot remove (measured in gtktextview.c 4.22.4:
 * `while (first_child) gtk_text_view_remove()` warns and never advances), so
 * that unparent must happen before the parent class runs.
 *
 * Fields:
 *   ed — the owning editor, or NULL once it has been freed.
 * ------------------------------------------------------------------------- */
#define NOTES_TYPE_TEXT_VIEW (notes_text_view_get_type())
G_DECLARE_FINAL_TYPE(NotesTextView, notes_text_view, NOTES, TEXT_VIEW,
                     GtkTextView)

struct _NotesTextView {
    GtkTextView parent_instance;
    OnEditor   *ed;
};

/* ---------------------------------------------------------------------------
 * INLINE_TOGGLES — table describing the four inline-style toggle buttons:
 * their flag bit, tag name, the name "win.inline" takes as its target for
 * the shortcut (see on_app_install_accels), icon file, fallback markup,
 * label and tooltip.
 * ------------------------------------------------------------------------- */
static const struct {
    OnFormatFlags flag;              /* bit this button controls            */
    const gchar  *tag_name;         /* GtkTextTag it applies               */
    const gchar  *target;           /* "win.inline" target naming it       */
    const gchar  *icon;             /* local icon file basename            */
    const gchar  *markup;           /* icon fallback markup                */
    const gchar  *label;            /* button text label                   */
    const gchar  *tooltip;          /* hover help text                     */
} INLINE_TOGGLES[4] = {
    { ON_FMT_BOLD,      ON_TAGNAME_BOLD,      "bold",      NULL,
      "<b>B</b>", "Bold",      "Bold" },
    { ON_FMT_ITALIC,    ON_TAGNAME_ITALIC,    "italic",    NULL,
      "<i>I</i>", "Italic",    "Italic" },
    { ON_FMT_UNDERLINE, ON_TAGNAME_UNDERLINE, "underline", NULL,
      "<u>U</u>", "Underline", "Underline" },
    { ON_FMT_STRIKE,    ON_TAGNAME_STRIKE,    "strike",    NULL,
      "<s>S</s>", "Strike",    "Strikethrough" },
};

/* Forward declarations for callbacks referenced before their definition.   */
static void     editor_save(OnEditor *ed);
static void     editor_queue_autosave(OnEditor *ed);
static void     editor_status_dirty_update(OnEditor *ed);
static void     tag_capture_end(OnEditor *ed, gboolean apply);
/* Which editor-only DERIVED passes editor_rederive() should run over a
 * range.  None of these are ever serialized (see editor_rederive).          */
typedef enum {
    RD_ACTION   = 1 << 0,            /* blue tint on '!' action lines       */
    RD_TITLE    = 1 << 1,            /* line 0's centering + heading size   */
    RD_EMOJI    = 1 << 2,            /* macOS emoji letter-spacing          */
    RD_CODE_TAG = 1 << 3,            /* strip #tag spans inside code blocks */
} OnRederive;

static void     action_retag_lines(OnEditor *ed, gint start_off,
                                   gint end_off);
static gboolean editor_rederive(OnEditor *ed, gint start_off, gint end_off,
                                   guint what);
static void     tag_emoji_in_range(OnEditor *ed, gint start_off,
                                   gint end_off);
static void     title_line_sync(OnEditor *ed, gint start_off,
                                   gint end_off);
static void     tag_popup_update(OnEditor *ed);
static void     renumber_list_block(OnEditor *ed, gint line);
static gboolean line_strip_list_prefix(OnEditor *ed, GtkTextIter *line_start);
static void     code_buttons_queue_rebuild(OnEditor *ed);
static void     attach_table_widget(OnEditor *ed,
                                    GtkTextChildAnchor *anchor);
static void     attach_checkbox_widget(OnEditor *ed,
                                       GtkTextChildAnchor *anchor);
static void     insert_checkbox_at(OnEditor *ed, gint at);
static void     editor_search_move(OnEditor *ed,
                                   gboolean forward);
static void     undo_notify_change(OnEditor *ed);
static gboolean strip_tags_in_code_blocks(OnEditor *ed, gint start_off,
                                          gint end_off);

/* ===========================================================================
 * small helpers
 * =========================================================================== */

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

/* ---------------------------------------------------------------------------
 * update_toggle_buttons() — mirror ed->inline_flags into the four toolbar
 * toggle buttons without re-triggering their "toggled" handlers (each
 * handler is blocked by function+data while the state is pushed).
 * ------------------------------------------------------------------------- */
static void on_inline_toggle(GtkToggleButton *btn, gpointer user_data);

static void
update_toggle_buttons(OnEditor *ed)
{
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkToggleButton *btn = GTK_TOGGLE_BUTTON(ed->toggle_buttons[i]);
        g_signal_handlers_block_by_func(btn, on_inline_toggle, ed);
        gtk_toggle_button_set_active(
            btn, (ed->inline_flags & INLINE_TOGGLES[i].flag) != 0);
        g_signal_handlers_unblock_by_func(btn, on_inline_toggle, ed);
    }
}

/* ===========================================================================
 * inline formatting
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * toggle_inline_format() — handle a bold/italic/underline/strike request.
 * With a selection: toggle the tag across the selected range.  Without:
 * flip the bit in inline_flags so upcoming typed text uses it.
 *   ed   — the editor.
 *   flag — which inline style to toggle.
 * ------------------------------------------------------------------------- */
static void
toggle_inline_format(OnEditor *ed, OnFormatFlags flag)
{
    const gchar *tag_name = on_tag_name_for_flag(flag);
    if (tag_name == NULL)
        return;

    GtkTextIter sel_start, sel_end;  /* selection bounds, if any            */
    if (gtk_text_buffer_get_selection_bounds(ed->buffer,
                                             &sel_start, &sel_end)) {
        /* Toggle over the selection: if the first selected char already
         * has the style, remove it everywhere; otherwise apply it.         */
        GtkTextTag *tag = lookup_tag(ed->buffer, tag_name);
        gboolean has = gtk_text_iter_has_tag(&sel_start, tag);
        ed->internal_change++;
        if (has)
            gtk_text_buffer_remove_tag(ed->buffer, tag,
                                       &sel_start, &sel_end);
        else
            gtk_text_buffer_apply_tag(ed->buffer, tag,
                                      &sel_start, &sel_end);
        ed->internal_change--;
        ed->inline_flags = has ? (ed->inline_flags & ~(guint32)flag)
                               : (ed->inline_flags | (guint32)flag);
        editor_queue_autosave(ed);
    } else {
        /* No selection: arm/disarm the style for upcoming typing.          */
        ed->inline_flags ^= (guint32)flag;
    }
    update_toggle_buttons(ed);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* on_inline() — "win.inline(s)": the Primary+B/I/U shortcuts, by the name
 * INLINE_TOGGLES gives the style.  The toolbar's four toggle buttons stay
 * plain toggles driven by update_toggle_buttons(): bound to an action they
 * would flip themselves before the action ran.                             */
static void
on_inline(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    OnEditor *ed = user_data;        /* owning editor                       */
    const gchar *name = g_variant_get_string(param, NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        if (g_strcmp0(INLINE_TOGGLES[i].target, name) == 0) {
            toggle_inline_format(ed, INLINE_TOGGLES[i].flag);
            return;
        }
    }
}

/* on_inline_toggle() — "toggled" handler for the four style buttons.  The
 * flag each button controls is stashed on it as object data "on-flag".     */
static void
on_inline_toggle(GtkToggleButton *btn, gpointer user_data)
{
    OnEditor *ed  = user_data;       /* owning editor                       */
    guint32  flag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn),
                                                       "on-flag"));
    toggle_inline_format(ed, (OnFormatFlags)flag);
}

/* ===========================================================================
 * paragraph formatting (headings, lists, code blocks)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * line_strip_list_prefix() — if the line starting at `line_start` begins
 * with a literal list prefix ("• " or "12. "), delete it.
 *   ed         — the editor (for internal_change bookkeeping by caller).
 *   line_start — iterator at the line start; revalidated after deletion.
 * Returns TRUE if a prefix was removed.
 * ------------------------------------------------------------------------- */
static gboolean
line_strip_list_prefix(OnEditor *ed, GtkTextIter *line_start)
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
        gtk_text_buffer_delete(ed->buffer, line_start, &del_end);
        gtk_text_buffer_get_iter_at_line(ed->buffer, line_start, line);
        return TRUE;
    }

    GtkTextIter probe_end = *line_start;   /* end of the probe window       */
    /* A prefix is at most "9999. " — 6 chars; don't run past the line.     */
    for (gint i = 0; i < 7 && !gtk_text_iter_ends_line(&probe_end); i++)
        gtk_text_iter_forward_char(&probe_end);

    gchar *head = gtk_text_buffer_get_text(ed->buffer, line_start,
                                           &probe_end, FALSE);
    glong strip_chars = on_list_prefix_chars(head);
    g_free(head);

    if (strip_chars == 0)
        return FALSE;

    GtkTextIter del_end = *line_start;     /* end of the prefix to delete   */
    gtk_text_iter_forward_chars(&del_end, (gint)strip_chars);
    gint line = gtk_text_iter_get_line(line_start);
    gtk_text_buffer_delete(ed->buffer, line_start, &del_end);
    /* Deletion invalidated the iters; recompute the line start.            */
    gtk_text_buffer_get_iter_at_line(ed->buffer, line_start, line);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * renumber_list_block() — rewrite the "N. " prefixes of the contiguous
 * numbered-list block containing `line` so they count 1, 2, 3…
 *   ed   — the editor.
 *   line — any line inside the block.
 * ------------------------------------------------------------------------- */
static void
renumber_list_block(OnEditor *ed, gint line)
{
    GtkTextTag *num_tag = lookup_tag(ed->buffer, ON_TAGNAME_LIST_NUMBER);
    if (num_tag == NULL)
        return;

    /* Walk back to the first line of the block.                            */
    gint first = line;               /* first line of the numbered block    */
    while (first > 0) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(ed->buffer, &it, first - 1);
        if (!gtk_text_iter_has_tag(&it, num_tag))
            break;
        first--;
    }

    /* Rewrite prefixes forward until the block ends.                       */
    gint total_lines =
        gtk_text_buffer_get_line_count(ed->buffer);
    gint number = 1;                 /* the value to write on this line     */
    ed->internal_change++;
    for (gint l = first; l < total_lines; l++, number++) {
        GtkTextIter ls;              /* line start                          */
        gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, l);
        if (!gtk_text_iter_has_tag(&ls, num_tag))
            break;

        line_strip_list_prefix(ed, &ls);

        gchar *prefix = g_strdup_printf("%d. ", number);
        gint offset = gtk_text_iter_get_offset(&ls);
        gtk_text_buffer_insert(ed->buffer, &ls, prefix, -1);

        /* Re-apply the list tag over the inserted prefix so the line stays
         * uniformly tagged.                                                */
        GtkTextIter ps, pe;          /* prefix span                         */
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &ps, offset);
        gtk_text_buffer_get_iter_at_offset(
            ed->buffer, &pe, offset + (gint)g_utf8_strlen(prefix, -1));
        gtk_text_buffer_apply_tag(ed->buffer, num_tag, &ps, &pe);
        g_free(prefix);
    }
    ed->internal_change--;
}

/* selection_line_range() — the inclusive line range covered by the
 * selection, or the cursor's line twice when nothing is selected.  The
 * rule every paragraph-style operation uses.                                */
static void
selection_line_range(OnEditor *ed, gint *first_line, gint *last_line)
{
    GtkTextIter start, end;          /* selection (or cursor twice)         */
    if (!gtk_text_buffer_get_selection_bounds(ed->buffer, &start, &end)) {
        gtk_text_buffer_get_iter_at_mark(
            ed->buffer, &start,
            gtk_text_buffer_get_insert(ed->buffer));
        end = start;
    }
    *first_line = gtk_text_iter_get_line(&start);
    *last_line  = gtk_text_iter_get_line(&end);
}

/* ---------------------------------------------------------------------------
 * apply_paragraph_format() — set the paragraph style of every line touched
 * by the selection (or the cursor line) to `flag`.
 *   ed   — the editor.
 *   flag — one of ON_FMT_H1/H2/CODEBLOCK/LIST_BULLET/LIST_NUMBER, or 0
 *          for plain body text.
 * ------------------------------------------------------------------------- */
static void
apply_paragraph_format(OnEditor *ed, guint32 flag)
{
    gint first_line, last_line;      /* affected line range                 */
    selection_line_range(ed, &first_line, &last_line);

    ed->internal_change++;
    for (gint l = first_line; l <= last_line; l++) {
        GtkTextIter ls, le;          /* line span incl. trailing newline    */
        line_span(ed->buffer, l, &ls, &le);

        /* Clear every existing paragraph tag and any list prefix.          */
        for (gsize i = 0; i < on_n_flag_tags; i++)
            if (on_flag_tags[i].flag & ON_FMT_PARA_MASK)
                gtk_text_buffer_remove_tag_by_name(
                    ed->buffer, on_flag_tags[i].tag_name, &ls, &le);
        if (line_strip_list_prefix(ed, &ls))
            line_span(ed->buffer, l, &ls, &le);

        if (flag == 0)
            continue;                /* body text: nothing more to do       */

        /* An empty LAST line has an empty span (no trailing newline), so
         * a heading/code tag would have nothing to hold onto and the
         * style would silently not stick.  Give the line its newline to
         * carry the tag and keep the cursor on the styled line.  (Lists
         * don't need this — their visible prefix is inserted below.)      */
        if (gtk_text_iter_equal(&ls, &le) &&
            (flag & (ON_FMT_H1 | ON_FMT_H2 | ON_FMT_CODEBLOCK))) {
            gtk_text_buffer_insert(ed->buffer, &ls, "\n", -1);
            line_span(ed->buffer, l, &ls, &le);
            gtk_text_buffer_place_cursor(ed->buffer, &ls);
        }

        /* For lists, insert the visible prefix first.                      */
        if (flag == ON_FMT_LIST_BULLET) {
            gtk_text_buffer_insert(ed->buffer, &ls, "\xe2\x80\xa2 ", -1);
            line_span(ed->buffer, l, &ls, &le);
        } else if (flag == ON_FMT_LIST_CHECK) {
            insert_checkbox_at(ed, gtk_text_iter_get_offset(&ls));
            line_span(ed->buffer, l, &ls, &le);
        } else if (flag == ON_FMT_LIST_NUMBER) {
            /* Number is fixed up by renumber_list_block afterwards.        */
            gtk_text_buffer_insert(ed->buffer, &ls, "1. ", -1);
            line_span(ed->buffer, l, &ls, &le);
        }

        /* Apply the requested tag over the whole line.                     */
        gtk_text_buffer_apply_tag_by_name(
            ed->buffer, on_tag_name_for_flag(flag), &ls, &le);
    }
    ed->internal_change--;

    if (flag == ON_FMT_LIST_NUMBER)
        renumber_list_block(ed, first_line);

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
    gtk_text_buffer_get_iter_at_line(ed->buffer, &rs, first_line);
    line_span(ed->buffer, last_line, &rls, &rle);
    if (editor_rederive(ed, gtk_text_iter_get_offset(&rs),
                        gtk_text_iter_get_offset(&rle),
                        RD_ACTION | RD_TITLE |
                        (flag == ON_FMT_CODEBLOCK ? RD_CODE_TAG : 0)))
        ed->tags_modified = TRUE;

    editor_queue_autosave(ed);
    /* Code blocks may have appeared or vanished: refresh their buttons.
     * (Pure tag changes don't emit "changed", so this must be explicit.)   */
    code_buttons_queue_rebuild(ed);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * toggle_paragraph_format() — apply `flag` to the selected lines, or
 * revert them to body text when every one already carries it.  Shared by
 * the paragraph tool buttons/menu items and the Primary+M code-block
 * shortcut (all of them "win.para").
 * ------------------------------------------------------------------------- */
static void
toggle_paragraph_format(OnEditor *ed, guint32 flag)
{
    if (flag != 0) {
        gint first_line, last_line;  /* affected line range                 */
        selection_line_range(ed, &first_line, &last_line);
        gboolean all_have = TRUE;    /* every line already this style?      */
        for (gint l = first_line; l <= last_line; l++) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(ed->buffer, &it, l);
            if (line_para_flags(ed->buffer, &it) != flag) {
                all_have = FALSE;
                break;
            }
        }
        if (all_have)
            flag = 0;                /* toggle off: back to body text       */
    }
    apply_paragraph_format(ed, flag);
}

/* The paragraph styles by the name "win.para" takes as its target: the
 * tool buttons, the compact Styles/Lists menus and the Primary+M shortcut
 * ("win.para::code") all name one of these.                                */
static const struct {
    const gchar *name;               /* the action target                   */
    guint32      flag;               /* the ON_FMT_* paragraph style        */
} PARA_STYLES[] = {
    { "h1",     ON_FMT_H1          },
    { "h2",     ON_FMT_H2          },
    { "body",   0                  },
    { "bullet", ON_FMT_LIST_BULLET },
    { "number", ON_FMT_LIST_NUMBER },
    { "check",  ON_FMT_LIST_CHECK  },
    { "code",   ON_FMT_CODEBLOCK   },
};

/* on_para() — "win.para(s)": toggle the named paragraph style.             */
static void
on_para(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    OnEditor *ed = user_data;        /* owning editor                       */
    const gchar *name = g_variant_get_string(param, NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(PARA_STYLES); i++) {
        if (g_strcmp0(PARA_STYLES[i].name, name) == 0) {
            toggle_paragraph_format(ed, PARA_STYLES[i].flag);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * handle_return_in_list() — implement list continuation on Enter.
 *
 * On a non-empty list line, Enter inserts a newline plus the next prefix
 * ("• " or "N. ") and keeps the list tag going.  On an empty list item,
 * Enter *ends* the list (removes the prefix and tag, leaving a plain
 * line).
 *   ed — the editor.
 * Returns TRUE if the key press was consumed (a list line was handled).
 * ------------------------------------------------------------------------- */
static gboolean
handle_return_in_list(OnEditor *ed)
{
    GtkTextIter cursor;              /* current insertion point             */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));

    guint32 para = line_para_flags(ed->buffer, &cursor);
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
    gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le))
        gtk_text_iter_forward_to_line_end(&le);
    gchar *text = gtk_text_buffer_get_text(ed->buffer, &ls, &le, FALSE);

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

    ed->internal_change++;
    if (item_empty) {
        /* Empty item: end the list here.                                   */
        GtkTextIter span_s, span_e;  /* full line incl. newline             */
        line_span(ed->buffer, line, &span_s, &span_e);
        gtk_text_buffer_remove_tag_by_name(ed->buffer, tag_name,
                                           &span_s, &span_e);
        gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
        line_strip_list_prefix(ed, &ls);
    } else {
        /* Continue the list: newline + next prefix, tagged.  A new task
         * item always starts unchecked.                                    */
        gint at = gtk_text_iter_get_offset(&cursor);
        if (para == ON_FMT_LIST_CHECK) {
            gtk_text_buffer_insert(ed->buffer, &cursor, "\n", -1);
            GtkTextIter nl_s, nl_e;  /* the newline character               */
            gtk_text_buffer_get_iter_at_offset(ed->buffer, &nl_s, at);
            gtk_text_buffer_get_iter_at_offset(ed->buffer, &nl_e, at + 1);
            gtk_text_buffer_apply_tag_by_name(ed->buffer, tag_name,
                                              &nl_s, &nl_e);
            insert_checkbox_at(ed, at + 1);  /* anchor + space, tagged      */
        } else {
            gchar *next_prefix;      /* text inserted after the newline     */
            if (para == ON_FMT_LIST_BULLET) {
                next_prefix = g_strdup("\n\xe2\x80\xa2 ");
            } else {
                glong n = g_ascii_strtoll(text, NULL, 10);
                next_prefix = g_strdup_printf("\n%ld. ", n + 1);
            }
            gtk_text_buffer_insert(ed->buffer, &cursor, next_prefix, -1);

            GtkTextIter ins_s, ins_e;    /* span of the inserted text       */
            gtk_text_buffer_get_iter_at_offset(ed->buffer, &ins_s, at);
            gtk_text_buffer_get_iter_at_offset(
                ed->buffer, &ins_e,
                at + (gint)g_utf8_strlen(next_prefix, -1));
            gtk_text_buffer_apply_tag_by_name(ed->buffer, tag_name,
                                              &ins_s, &ins_e);
            g_free(next_prefix);

            if (para == ON_FMT_LIST_NUMBER)
                renumber_list_block(ed, line);
        }
    }
    ed->internal_change--;
    g_free(text);

    editor_queue_autosave(ed);
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
 *   ed     — the editor.
 *   wx, wy — pointer position in the VIEW's widget coordinates (what a
 *            gesture on the view reports).
 *
 * The test is against the link's bounds computed relative to the view —
 * where GTK allocated it for the current scroll position — so it is
 * scroll-proof even though the links are POSITIONED in buffer coordinates.
 * ------------------------------------------------------------------------- */
static GtkWidget *
code_link_at_view_pos(OnEditor *ed, gdouble wx, gdouble wy)
{
    for (GSList *l = ed->code_buttons; l != NULL; l = l->next) {
        GtkWidget *link = l->data;   /* one floating copy link              */
        graphene_rect_t bounds;      /* where it sits, view coordinates     */
        if (gtk_widget_compute_bounds(link, GTK_WIDGET(ed->view), &bounds) &&
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
code_copy_link_activate(OnEditor *ed, GtkWidget *link)
{
    GtkTextMark *mark =              /* block-start mark on the link        */
        g_object_get_data(G_OBJECT(link), "on-mark");
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (mark == NULL || tag == NULL)
        return FALSE;

    GtkTextIter start;               /* block start                         */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &start, mark);
    if (!gtk_text_iter_has_tag(&start, tag))
        return FALSE;                /* block vanished since last rebuild   */

    GtkTextIter end = start;         /* block end                           */
    gtk_text_iter_forward_to_tag_toggle(&end, tag);

    gchar *code = gtk_text_buffer_get_text(ed->buffer, &start, &end, FALSE);
    g_strchomp(code);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(ed->view)),
                           code);
    g_free(code);
    on_app_status(ed->app, "Codeblock copied");
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
code_buttons_update_positions(OnEditor *ed)
{
    gint view_width = gtk_widget_get_width(GTK_WIDGET(ed->view));
    if (view_width <= 0)
        return;                      /* not allocated yet                   */

    for (GSList *l = ed->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;    /* one floating copy button            */
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark == NULL)
            continue;

        gint bw;                     /* minimum width = what an overlay is
                                        allocated (gtktextviewchild.c)      */
        gtk_widget_measure(btn, GTK_ORIENTATION_HORIZONTAL, -1, &bw, NULL,
                           NULL, NULL);

        GtkTextIter it;              /* block start position                */
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &it, mark);
        gtk_text_iter_set_line_offset(&it, 0);   /* normalize to line start */

        /* Ask the layout for the whole display line's y-range: it starts
         * exactly where the paragraph background (the shading) starts —
         * unlike the character box, which sits below the above-line
         * spacing and drifted the button downward.                         */
        gint line_y, line_h;         /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(ed->view, &it, &line_y, &line_h);

        /* Equal CODE_BTN_MARGIN insets from the shaded top/right edges.     */
        gint x = view_width - CODEBLOCK_RIGHT_MARGIN - bw - CODE_BTN_MARGIN;
        gint y = line_y + CODE_BTN_MARGIN;
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "on-x")) == x &&
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "on-y")) == y)
            continue;                /* already there: no re-allocation     */
        g_object_set_data(G_OBJECT(btn), "on-x", GINT_TO_POINTER(x));
        g_object_set_data(G_OBJECT(btn), "on-y", GINT_TO_POINTER(y));
        gtk_text_view_move_overlay(ed->view, btn, x, y);
    }
}

/* ---------------------------------------------------------------------------
 * code_buttons_rebuild() — remove and recreate the floating copy buttons
 * to match the code blocks currently present in the buffer.
 * ------------------------------------------------------------------------- */
static void
code_buttons_rebuild(OnEditor *ed)
{
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);

    /* Fast path: this runs after EVERY buffer change, but the set of
     * blocks rarely changes while typing.  When the current buttons'
     * marks already sit exactly on the block starts, just reposition —
     * no widget churn.                                                     */
    if (tag != NULL && ed->app->code_copy_buttons) {
        GArray *starts =             /* block start offsets, ascending      */
            g_array_new(FALSE, FALSE, sizeof(gint));
        GtkTextIter scan;
        gtk_text_buffer_get_start_iter(ed->buffer, &scan);
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
            (guint)g_slist_length(ed->code_buttons) == starts->len;
        if (same) {
            /* Buttons were prepended while scanning forward, so the list
             * is in REVERSE document order.                                */
            guint i = starts->len;
            for (GSList *l = ed->code_buttons; same && l != NULL;
                 l = l->next) {
                GtkTextMark *mark =
                    g_object_get_data(G_OBJECT(l->data), "on-mark");
                GtkTextIter mi;
                gtk_text_buffer_get_iter_at_mark(ed->buffer, &mi, mark);
                same = (i > 0) &&
                       gtk_text_iter_get_offset(&mi) ==
                           g_array_index(starts, gint, --i);
            }
        }
        g_array_free(starts, TRUE);
        if (same) {
            code_buttons_update_positions(ed);
            return;
        }
    }

    /* Tear down the old set (their marks die with them).                   */
    for (GSList *l = ed->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark != NULL)
            gtk_text_buffer_delete_mark(ed->buffer, mark);
        gtk_text_view_remove(ed->view, btn);
    }
    g_slist_free(ed->code_buttons);
    ed->code_buttons = NULL;

    /* The buttons can be disabled entirely in File → Settings….            */
    if (!ed->app->code_copy_buttons)
        return;

    if (tag == NULL)
        return;

    /* Walk the buffer from span to span of the code-block tag.             */
    GtkTextIter it;                  /* scan position                       */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    while (TRUE) {
        if (!gtk_text_iter_starts_tag(&it, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
                break;
            if (!gtk_text_iter_starts_tag(&it, tag))
                continue;
        }

        /* One block starts here: build its "copy" hyperlink overlay.  A
         * plain label: the click is served by the view's gesture, only
         * the hover cursor is the label's own.                             */
        GtkWidget *link = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(link),
            "<span size=\"8192\" foreground=\"#0066cc\""
            " underline=\"single\">copy</span>");
        gtk_widget_set_cursor_from_name(link, "pointer");

        GtkTextMark *mark = gtk_text_buffer_create_mark(ed->buffer, NULL,
                                                        &it, TRUE);
        g_object_set_data(G_OBJECT(link), "on-mark", mark);

        /* Added at the origin; code_buttons_update_positions moves it.    */
        gtk_text_view_add_overlay(ed->view, link, 0, 0);
        ed->code_buttons = g_slist_prepend(ed->code_buttons, link);

        /* Jump past this block and continue scanning.                      */
        if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
            break;
    }
    code_buttons_update_positions(ed);
}

/* on_code_btn_idle() — deferred rebuild so the text layout has settled.     */
static gboolean
on_code_btn_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->code_btn_idle = 0;
    code_buttons_rebuild(ed);
    return G_SOURCE_REMOVE;
}

/* code_buttons_queue_rebuild() — coalesce rebuild requests into one idle.   */
static void
code_buttons_queue_rebuild(OnEditor *ed)
{
    if (ed->code_btn_idle == 0)
        ed->code_btn_idle = g_idle_add(on_code_btn_idle, ed);
}

/* ===========================================================================
 * NotesTextView — the vfuncs (see the type's banner near OnEditor)
 * =========================================================================== */

G_DEFINE_FINAL_TYPE(NotesTextView, notes_text_view, GTK_TYPE_TEXT_VIEW)

/* ---------------------------------------------------------------------------
 * notes_text_view_snapshot() — chain up (text, overlays, anchored
 * children), then paint line numbers INSIDE each code block: the block's
 * left margin is widened (see editor_apply_line_numbers) and the numbers
 * are drawn onto that strip of the block's own shading, right-aligned just
 * before the code text.  Painted, not text — selection and copying can
 * never include them.  Each block numbers from 1.  Cairo through
 * gtk_snapshot_append_cairo, in the view's widget coordinates
 * (buffer_to_window_coords with GTK_TEXT_WINDOW_WIDGET).
 * ------------------------------------------------------------------------- */
static void
notes_text_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    GTK_WIDGET_CLASS(notes_text_view_parent_class)->snapshot(widget,
                                                             snapshot);

    OnEditor *ed = NOTES_TEXT_VIEW(widget)->ed;   /* owning editor         */
    if (ed == NULL || !ed->app->code_line_numbers)
        return;
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag == NULL)
        return;

    graphene_rect_t bounds =         /* the whole widget: cairo clip        */
        GRAPHENE_RECT_INIT(0, 0, (float)gtk_widget_get_width(widget),
                           (float)gtk_widget_get_height(widget));
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);

    /* Start at the first visible buffer line.                              */
    GdkRectangle vis;                /* visible area in buffer coords       */
    gtk_text_view_get_visible_rect(ed->view, &vis);
    GtkTextIter it;                  /* walking line iterator               */
    gtk_text_view_get_line_at_y(ed->view, &it, vis.y, NULL);
    gtk_text_iter_set_line_offset(&it, 0);

    /* If that line sits mid-block, count how far into the block it is.     */
    gint num = 0;                    /* number of the PREVIOUS code line    */
    if (gtk_text_iter_has_tag(&it, tag)) {
        gint first = gtk_text_iter_get_line(&it);
        while (first > 0) {
            GtkTextIter prev;
            gtk_text_buffer_get_iter_at_line(ed->buffer, &prev, first - 1);
            if (!gtk_text_iter_has_tag(&prev, tag))
                break;
            first--;
        }
        num = gtk_text_iter_get_line(&it) - first;
    }

    PangoLayout *layout =            /* renders the number strings          */
        gtk_widget_create_pango_layout(widget, NULL);
    PangoFontDescription *fd =
        pango_font_description_from_string("monospace 9");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    while (TRUE) {
        gint y, h;                   /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(ed->view, &it, &y, &h);
        if (y > vis.y + vis.height)
            break;

        if (gtk_text_iter_has_tag(&it, tag)) {
            num++;

            /* The line's first character marks where the code text
             * begins; the gutter band and its number sit just left of
             * it, over the block's shading.                                */
            GdkRectangle rect;       /* first char, buffer coords           */
            gtk_text_view_get_iter_location(ed->view, &it, &rect);
            gint wx, wy;             /* char position in widget coords      */
            gtk_text_view_buffer_to_window_coords(
                ed->view, GTK_TEXT_WINDOW_WIDGET, rect.x, rect.y,
                &wx, &wy);
            gint wy_line;            /* line-range top in widget coords     */
            gtk_text_view_buffer_to_window_coords(
                ed->view, GTK_TEXT_WINDOW_WIDGET, 0, y, NULL, &wy_line);

            /* Grey gutter band behind the number, spanning the whole
             * line range so adjacent lines tile seamlessly.                */
            cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
            cairo_rectangle(cr, wx - 20, wy_line, 16, h);
            cairo_fill(cr);

            gchar text[16];          /* the printed number                  */
            g_snprintf(text, sizeof text, "%d", num);
            pango_layout_set_text(layout, text, -1);
            gint tw, th;             /* rendered number size                */
            pango_layout_get_pixel_size(layout, &tw, &th);
            cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
            cairo_move_to(cr, wx - 6 - tw, wy + 1);
            pango_cairo_show_layout(cr, layout);
        } else {
            num = 0;                 /* block ended: restart numbering      */
        }
        if (!gtk_text_iter_forward_line(&it))
            break;
    }
    g_object_unref(layout);
    cairo_destroy(cr);
}

/* ---------------------------------------------------------------------------
 * notes_text_view_size_allocate() — chain up, then present the #tag popover
 * (a popover child must be presented by its parent's size_allocate, which
 * GtkTextView does only for its own) and queue a re-anchoring of the copy
 * buttons: a width change moves their x, a reflow their line_y.  Queued,
 * not done here — moving an overlay queues another allocation, and the
 * idle's fast path moves nothing that has not changed.
 * ------------------------------------------------------------------------- */
static void
notes_text_view_size_allocate(GtkWidget *widget, gint width, gint height,
                              gint baseline)
{
    GTK_WIDGET_CLASS(notes_text_view_parent_class)->size_allocate(
        widget, width, height, baseline);

    OnEditor *ed = NOTES_TEXT_VIEW(widget)->ed;   /* owning editor         */
    if (ed == NULL)
        return;
    if (ed->tag_popup != NULL)
        gtk_popover_present(GTK_POPOVER(ed->tag_popup));
    code_buttons_queue_rebuild(ed);
}

/* ---------------------------------------------------------------------------
 * notes_text_view_dispose() — take the #tag popover off the view BEFORE
 * GtkTextView's dispose walks the children it knows (see the type's
 * banner: a leftover foreign child hangs that walk).  The editor state
 * outlives the view — the window's "destroy" fires after the child tree
 * is gone in GTK4 — so the pointers are cleared here for it.
 * ------------------------------------------------------------------------- */
static void
notes_text_view_dispose(GObject *object)
{
    NotesTextView *self = NOTES_TEXT_VIEW(object);
    if (self->ed != NULL) {
        g_clear_pointer(&self->ed->tag_popup, gtk_widget_unparent);
        self->ed->tag_listbox = NULL;
    }
    G_OBJECT_CLASS(notes_text_view_parent_class)->dispose(object);
}

/* notes_text_view_class_init() — install the three overrides.               */
static void
notes_text_view_class_init(NotesTextViewClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot        = notes_text_view_snapshot;
    widget_class->size_allocate   = notes_text_view_size_allocate;
    G_OBJECT_CLASS(klass)->dispose = notes_text_view_dispose;
}

/* notes_text_view_init() — nothing beyond GtkTextView's own setup.          */
static void
notes_text_view_init(NotesTextView *self)
{
    self->ed = NULL;
}

/* notes_text_view_new() — a view bound to `ed` (the vfuncs read it).       */
static GtkWidget *
notes_text_view_new(OnEditor *ed)
{
    NotesTextView *self = g_object_new(NOTES_TYPE_TEXT_VIEW, NULL);
    self->ed = ed;
    return GTK_WIDGET(self);
}

/* editor_apply_line_numbers() — widen/narrow the code-block left margin
 * (making room for the painted numbers inside the shading) and redraw.      */
static void
editor_apply_line_numbers(OnEditor *ed)
{
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag != NULL)
        g_object_set(tag, "left-margin",
                     ed->app->code_line_numbers
                         ? CODEBLOCK_LEFT_MARGIN_NUMS
                         : CODEBLOCK_LEFT_MARGIN,
                     NULL);
    gtk_widget_queue_draw(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * editors_foreach() — run `fn` on every open editor.  The one walk of the
 * open-editors table; all the public *_all entry points go through it.
 * ------------------------------------------------------------------------- */
static void
editors_foreach(OnApp *app, void (*fn)(OnEditor *ed))
{
    GHashTableIter iter;             /* walk of the open-editors table      */
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->editors);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        OnEditor *ed =               /* editor state stashed on its window  */
            g_object_get_data(G_OBJECT(value), "on-editor");
        if (ed != NULL)
            fn(ed);
    }
}

void
on_editor_apply_line_numbers_all(OnApp *app)
{
    editors_foreach(app, editor_apply_line_numbers);
}

void
on_editor_rebuild_code_buttons_all(OnApp *app)
{
    editors_foreach(app, code_buttons_queue_rebuild);
}

/* editor_title_refresh() — re-run the title pass over line 0 alone: the
 * only line the feature ever tags, so this both applies and clears it.     */
static void
editor_title_refresh(OnEditor *ed)
{
    title_line_sync(ed, 0, 0);
}

void
on_editor_title_refresh_all(OnApp *app)
{
    editors_foreach(app, editor_title_refresh);
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
 * image_texture() — THE full-resolution pixbuf → GdkTexture edge for an
 * attached image (the anchored GtkPicture, Copy Image, the modal viewer):
 * GTK decodes the cached PNG bytes itself (D4), so the texture carries every
 * pixel and stays sharp at any display size on HiDPI.  A pixbuf that will
 * not encode (on_image_png_bytes NULL) is wrapped pixel-for-pixel instead.
 * Returns a new texture (g_object_unref it).
 * ------------------------------------------------------------------------- */
static GdkTexture *
image_texture(GdkPixbuf *orig)
{
    GBytes *png = on_image_png_bytes(orig);   /* borrowed                  */
    GdkTexture *tex = NULL;
    if (png != NULL) {
        GError *err = NULL;
        tex = gdk_texture_new_from_bytes(png, &err);
        if (tex == NULL) {
            g_warning("editor: image bytes will not decode: %s",
                      err->message);
            g_clear_error(&err);
        }
    }
    return tex != NULL ? tex : on_app_texture_for_pixbuf(orig);
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

    GdkTexture *tex = image_texture(orig);
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
anchor_clear_widgets(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    guint n;                         /* how many are attached               */
    GtkWidget **widgets = gtk_text_child_anchor_get_widgets(anchor, &n);
    for (guint i = 0; i < n; i++)
        gtk_text_view_remove(ed->view, widgets[i]);
    g_free(widgets);
}

/* ---------------------------------------------------------------------------
 * attach_image_widget() — (re)create the display widget for an
 * image-carrying anchor, removing any widget it already had.
 * ------------------------------------------------------------------------- */
static void
attach_image_widget(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    gint dw;                         /* stored display width                */
    GdkPixbuf *orig = on_anchor_get_image(anchor, &dw);
    if (orig == NULL)
        return;

    /* Drop any existing display widget.                                    */
    anchor_clear_widgets(ed, anchor);

    gtk_text_view_add_child_at_anchor(ed->view, image_widget_new(orig, dw),
                                      anchor);
}

/* ---------------------------------------------------------------------------
 * editor_attach_image_widgets() — walk the whole buffer and give every
 * image anchor its display widget (used right after loading a note).
 * ------------------------------------------------------------------------- */
static void
editor_attach_image_widgets(OnEditor *ed)
{
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor == NULL)
            continue;
        if (on_anchor_is_checkbox(anchor, NULL))
            attach_checkbox_widget(ed, anchor);
        else if (on_anchor_get_table(anchor) != NULL)
            attach_table_widget(ed, anchor);
        else
            attach_image_widget(ed, anchor);
    } while (gtk_text_iter_forward_char(&it));
}

/* ---------------------------------------------------------------------------
 * insert_image_pixbuf() — embed a full-resolution image at the cursor as
 * an anchor + HiDPI widget, shown at the default thumbnail width.
 *   ed     — the editor.
 *   pixbuf — full-resolution image (caller keeps its own reference).
 * ------------------------------------------------------------------------- */
static void
insert_image_pixbuf(OnEditor *ed, GdkPixbuf *pixbuf)
{
    GtkTextIter cursor;              /* insertion point                     */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));
    ed->internal_change++;
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(ed->buffer, &cursor);
    on_anchor_set_image(anchor, pixbuf, 0);
    attach_image_widget(ed, anchor);
    ed->internal_change--;
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * anchor_at_offset() — the child anchor (image or table) at buffer offset
 * `offset`, or NULL if that position holds none.
 * ------------------------------------------------------------------------- */
static GtkTextChildAnchor *
anchor_at_offset(OnEditor *ed, gint offset)
{
    GtkTextIter it;                  /* position of the anchor              */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, offset);
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

/* editor_image_count() — how many images the note holds.                    */
static gint
editor_image_count(OnEditor *ed)
{
    gint n = 0;                      /* images seen so far                  */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor != NULL && on_anchor_get_image(anchor, NULL) != NULL)
            n++;
    } while (gtk_text_iter_forward_char(&it));
    return n;
}

/* ---------------------------------------------------------------------------
 * editor_image_nth() — the note's `ord`-th image anchor, or NULL when there
 * is no such image.  `offset_out` gets its buffer offset when non-NULL.
 * ------------------------------------------------------------------------- */
static GtkTextChildAnchor *
editor_image_nth(OnEditor *ed, gint ord, gint *offset_out)
{
    if (ord < 0)
        return NULL;

    gint n = 0;                      /* images passed so far                */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
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

/* ---------------------------------------------------------------------------
 * editor_image_ord() — the ordinal of the image at buffer offset `offset`,
 * or -1 when that position holds no image.  The inverse of
 * editor_image_nth(), for turning a click's offset into the number the modal
 * viewer addresses pictures by.
 * ------------------------------------------------------------------------- */
static gint
editor_image_ord(OnEditor *ed, gint offset)
{
    gint n = 0;                      /* images passed so far                */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
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
 *   ed         — the editor.
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
image_at_view_pos(OnEditor *ed, gdouble wx, gdouble wy,
                  gint *offset_out, gint *dw_out)
{
    gint bx, by;                     /* position in buffer coordinates      */
    gtk_text_view_window_to_buffer_coords(ed->view, GTK_TEXT_WINDOW_WIDGET,
                                          (gint)wx, (gint)wy, &bx, &by);
    GtkTextIter it;                  /* iter under the pointer              */
    gtk_text_view_get_iter_at_position(ed->view, &it, NULL, bx, by);

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
    gtk_text_view_get_iter_location(ed->view, &it, &rect);
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
replace_image_display(OnEditor *ed, gint offset, gint display_width)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(ed, offset);
    if (anchor == NULL)
        return;
    GdkPixbuf *orig = on_anchor_get_image(anchor, NULL);
    if (orig == NULL)
        return;
    on_anchor_set_image(anchor, orig, display_width);
    attach_image_widget(ed, anchor);
    editor_queue_autosave(ed);
    code_buttons_queue_rebuild(ed);
}

/* ctx_image() — the full-resolution pixbuf of the image the last context
 * menu was opened on (ed->ctx_offset), or NULL if none is there any more.
 * Anchor-owned: do not unref.                                               */
static GdkPixbuf *
ctx_image(OnEditor *ed)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(ed, ed->ctx_offset);
    return anchor != NULL ? on_anchor_get_image(anchor, NULL) : NULL;
}

/* The image context-menu actions ("win.img-*").  All act on ctx_image().   */

/* on_img_copy() — "Copy Image": put the full-resolution image on the
 * clipboard so it can be pasted outside the note.                           */
static void
on_img_copy(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    GdkPixbuf *orig = ctx_image(ed);
    if (orig == NULL)
        return;
    GdkTexture *tex = image_texture(orig);
    gdk_clipboard_set_texture(gtk_widget_get_clipboard(GTK_WIDGET(ed->view)),
                              tex);
    g_object_unref(tex);
}

/* image_open_external() — write `orig` (a full-resolution image) to a
 * temporary PNG file and hand it to an image viewer — the program
 * configured under Settings ("image_viewer" setting), or the platform
 * opener (macOS `open`, otherwise `xdg-open`) which launches the system's
 * default viewer.  Shared by the "Open" menu item and the modal viewer's
 * action link.                                                              */
static void
image_open_external(GdkPixbuf *orig)
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
    OnEditor *ed = user_data;        /* owning editor                       */
    GdkPixbuf *orig = ctx_image(ed);
    if (orig != NULL)
        image_open_external(orig);
}

/* on_img_full() / on_img_thumb() — inline display size.                     */
static void
on_img_full(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    GdkPixbuf *orig = ctx_image(ed);
    if (orig != NULL)
        replace_image_display(ed, ed->ctx_offset,
                              gdk_pixbuf_get_width(orig));
}

static void
on_img_thumb(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    replace_image_display(ed, ed->ctx_offset,
                          0);        /* 0 = the default thumbnail width     */
}

/* ===========================================================================
 * the modal image viewer — this window's side of image_viewer.[ch]
 *
 * The identical panel the media browser opens, over the text view instead of
 * over a thumbnail grid.  Pictures are addressed by ORDINAL (see the image
 * ordinals section above), looked up in the buffer on every call: the panel
 * holds no pixbuf of its own, so an edit underneath it can only make an op
 * return NULL — which closes the panel — never dangle.
 * =========================================================================== */

/* editor_viewer_count() — how many pictures the panel may walk.             */
static gint
editor_viewer_count(gpointer host)
{
    return editor_image_count((OnEditor *)host);
}

/* editor_viewer_pixbuf() — the full-resolution image at `ord`, or NULL.
 * Anchor-owned; do not unref.                                               */
static GdkPixbuf *
editor_viewer_pixbuf(OnEditor *ed, gint ord)
{
    GtkTextChildAnchor *anchor = editor_image_nth(ed, ord, NULL);
    return (anchor != NULL) ? on_anchor_get_image(anchor, NULL) : NULL;
}

/* ---------------------------------------------------------------------------
 * editor_viewer_render() — the panel's image: the full-resolution texture
 * of the anchor's pixbuf (the panel scales DOWN to fit, so the box size is
 * not needed), or NULL when the image is gone — which closes the panel.
 * ------------------------------------------------------------------------- */
static GdkPaintable *
editor_viewer_render(gpointer host, gint idx, gint box_w, gint box_h)
{
    (void)box_w; (void)box_h;
    GdkPixbuf *orig = editor_viewer_pixbuf((OnEditor *)host, idx);
    return orig != NULL ? GDK_PAINTABLE(image_texture(orig)) : NULL;
}

/* ---------------------------------------------------------------------------
 * editor_viewer_caption() — "<note> — image N of M — W x H".
 *
 * The note is NAMED even though its own titlebar is right there: "of M"
 * counts every image in the note, including ones scrolled out of sight, so
 * the panel can legitimately walk to a picture the user cannot currently see
 * — and without the note's name that looks indistinguishable from the panel
 * having wandered into another note.  It matches the media browser's caption,
 * which names the note for the same reason.  The pixel size is the one thing
 * the editor knows that the fitted picture no longer shows.
 * ------------------------------------------------------------------------- */
static gchar *
editor_viewer_caption(gpointer host, gint idx)
{
    OnEditor  *ed   = host;          /* owning editor                       */
    GdkPixbuf *orig = editor_viewer_pixbuf(ed, idx);
    if (orig == NULL)
        return NULL;

    gchar *note = on_buffer_first_line(ed->buffer);
    gchar *cap  = g_strdup_printf(
        "%s \xe2\x80\x94 image %d of %d \xe2\x80\x94 %d \xc3\x97 %d",
        (note != NULL && *note != '\0') ? note : "Untitled",
        idx + 1, editor_image_count(ed),
        gdk_pixbuf_get_width(orig), gdk_pixbuf_get_height(orig));
    g_free(note);
    return cap;
}

/* editor_viewer_action() — "Open in image viewer": hand the picture to the
 * external viewer, exactly as the context menu's Open does.  "Show in source
 * note", the media browser's action, would be a no-op here — the source note
 * is the window the panel is sitting on.                                    */
static void
editor_viewer_action(gpointer host, gint idx)
{
    image_open_external(editor_viewer_pixbuf((OnEditor *)host, idx));
}

static const OnImageViewerOps editor_viewer_ops = {
    .count   = editor_viewer_count,
    .render  = editor_viewer_render,
    .caption = editor_viewer_caption,
    .action  = editor_viewer_action,
};

/* ---------------------------------------------------------------------------
 * editor_image_menu() — the context-menu items for an embedded image, as a
 * menu model naming the "win.img-*" actions: Copy Image, Open, and
 * whichever of Display Full Size / Display as Thumbnail the image is not
 * already showing.  The one definition of that menu; on_view_pressed hands
 * it to gtk_text_view_set_extra_menu before the view opens its popup.
 *   shown_full — whether the image is displayed at full size now.
 * Returns a new model (unref it).
 * ------------------------------------------------------------------------- */
static GMenuModel *
editor_image_menu(gboolean shown_full)
{
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Copy _Image", "win.img-copy");
    g_menu_append(menu, "_Open",       "win.img-open");
    if (shown_full)
        g_menu_append(menu, "Display as _Thumbnail", "win.img-thumb");
    else
        g_menu_append(menu, "Display _Full Size",    "win.img-full");
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
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkEventController *ctl = GTK_EVENT_CONTROLLER(gesture);
    GdkEvent *event = gtk_event_controller_get_current_event(ctl);

    if (n_press == 1 && event != NULL &&
        gdk_event_triggers_context_menu(event)) {
        gint offset;                 /* the image's buffer offset           */
        gint dw;                     /* stored display width                */
        GdkPixbuf *orig =            /* image the press landed on           */
            image_at_view_pos(ed, x, y, &offset, &dw);
        GMenuModel *items = NULL;    /* the extra menu, or none             */
        if (orig != NULL) {
            ed->ctx_offset = offset;
            items = editor_image_menu(image_shown_full(orig, dw));
        }
        gtk_text_view_set_extra_menu(ed->view, items);
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
        code_link_at_view_pos(ed, x, y);
    if (link != NULL) {
        code_copy_link_activate(ed, link);
        gtk_gesture_set_state(GTK_GESTURE(gesture),
                              GTK_EVENT_SEQUENCE_CLAIMED);
        return;
    }

    /* A click on an embedded image shows it big in the shared modal viewer.
     * It used to toggle that image's INLINE size between thumbnail and full
     * instead; those two states are still on its context menu, where they no
     * longer compete with the obvious reading of a click on a picture.      */
    gint offset;                     /* the image's buffer offset           */
    if (image_at_view_pos(ed, x, y, &offset, NULL) != NULL) {
        on_image_viewer_open(ed->img_viewer, editor_image_ord(ed, offset));
        gtk_gesture_set_state(GTK_GESTURE(gesture),
                              GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

/* ===========================================================================
 * asynchronous completions
 *
 * The clipboard read and the file chooser both finish on a later main-loop
 * iteration, by which time the editor may have been closed.  Neither holds
 * a pointer to it: they carry the note id and look the editor up again in
 * the open-editors table when they complete — an editor that is gone
 * simply drops the image.
 * =========================================================================== */

/* editor_lookup() — the open editor for `note_id`, or NULL.  NULL-safe on
 * a headless OnApp (the CLI has no editors table at all).                   */
static OnEditor *
editor_lookup(OnApp *app, gint64 note_id)
{
    GtkWidget *win = app->editors != NULL
        ? g_hash_table_lookup(app->editors, &note_id) : NULL;
    return win != NULL ? g_object_get_data(G_OBJECT(win), "on-editor")
                       : NULL;
}

/* EditorRef — what a completion carries instead of an OnEditor pointer.    */
typedef struct {
    OnApp  *app;
    gint64  note_id;
} EditorRef;

/* editor_ref_new() — a heap reference to `ed` for a completion callback.   */
static EditorRef *
editor_ref_new(OnEditor *ed)
{
    EditorRef *ref = g_new(EditorRef, 1);
    ref->app     = ed->app;
    ref->note_id = ed->note_id;
    return ref;
}

/* editor_ref_take() — resolve and free a reference: the editor, or NULL
 * when it has been closed since.                                            */
static OnEditor *
editor_ref_take(EditorRef *ref)
{
    OnEditor *ed = editor_lookup(ref->app, ref->note_id);
    g_free(ref);
    return ed;
}

/* ---------------------------------------------------------------------------
 * on_paste_texture_read() — the clipboard image has arrived as a texture:
 * turn it into the pixbuf serialize.c's anchor API wants, via the PNG
 * bytes — that PNG is exactly what the note will store, and the pixbuf is
 * what the editor holds.  GTK's own PNG/TIFF/JPEG loaders did the decode,
 * so a macOS screenshot (TIFF on the pasteboard) needs no pixbuf loader.
 * ------------------------------------------------------------------------- */
static void
on_paste_texture_read(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    OnEditor *ed = editor_ref_take(user_data);
    GError *err = NULL;
    GdkTexture *tex = gdk_clipboard_read_texture_finish(
        GDK_CLIPBOARD(source), result, &err);
    if (tex == NULL) {
        g_warning("editor: cannot read clipboard image: %s", err->message);
        g_clear_error(&err);
        return;
    }
    if (ed != NULL) {
        GBytes *png = gdk_texture_save_to_png_bytes(tex);
        GInputStream *in = g_memory_input_stream_new_from_bytes(png);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(in, NULL, &err);
        if (pixbuf != NULL) {
            insert_image_pixbuf(ed, pixbuf);
            g_object_unref(pixbuf);
        } else {
            g_warning("editor: cannot decode pasted image: %s",
                      err->message);
            g_clear_error(&err);
        }
        g_object_unref(in);
        g_bytes_unref(png);
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
    OnEditor     *ed = user_data;    /* owning editor                       */
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
                                     editor_ref_new(ed));
    /* Swallow the signal so the default handler doesn't also paste.        */
    g_signal_stop_emission_by_name(view, "paste-clipboard");
}

/* on_insert_image_picked() — the Insert Image chooser closed: embed the
 * chosen file at the cursor (nothing on cancel or a closed editor).         */
static void
on_insert_image_picked(gchar *path, gpointer user_data)
{
    OnEditor *ed = editor_ref_take(user_data);
    if (path == NULL)
        return;
    if (ed != NULL) {
        GError *err = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
        if (pixbuf != NULL) {
            insert_image_pixbuf(ed, pixbuf);
            g_object_unref(pixbuf);
        } else {
            g_warning("editor: cannot load image %s: %s",
                      path, err->message);
            g_clear_error(&err);
        }
    }
    g_free(path);
}

/* ---------------------------------------------------------------------------
 * on_insert_image() — "win.insert-image" (Insert menu): pick an image file
 * and embed it at the cursor.  Asynchronous: the rest is
 * on_insert_image_picked.
 * ------------------------------------------------------------------------- */
static void
on_insert_image(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    on_app_pick_path(GTK_WINDOW(ed->window), "Insert Image", ON_PICK_OPEN,
                     "_Insert", "Images", NULL, NULL, on_insert_image_picked,
                     editor_ref_new(ed));
}

/* ===========================================================================
 * action items
 *
 * A line whose first character is '!' (outside code blocks) is an action
 * item; the rest of the line is its text (see on_note_extract_actions in
 * serialize.c — the one definition both this styling pass and the
 * library's Action Items view derive from).  The editor tints such lines
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
line_range_for_offsets(OnEditor *ed, gint start_off, gint end_off,
                       gint *first, gint *last)
{
    GtkTextIter it;                  /* offset → line resolution            */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, start_off);
    *first = gtk_text_iter_get_line(&it);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, end_off);
    *last = gtk_text_iter_get_line(&it);
}

/* ---------------------------------------------------------------------------
 * action_retag_line() — re-derive the blue on-action styling for ONE line.
 * The single definition of that rule; the range walkers just loop it.
 * ------------------------------------------------------------------------- */
static void
action_retag_line(OnEditor *ed, gint line)
{
    GtkTextIter ls, rs, le;          /* line span (+ rest start)            */
    gboolean is = action_line_rest(ed->buffer, line, &ls, &rs, &le);
    if (!is) {
        gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
        le = ls;
        if (!gtk_text_iter_ends_line(&le))
            gtk_text_iter_forward_to_line_end(&le);
    }
    if (is)
        gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-action", &ls, &le);
    else
        gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-action",
                                           &ls, &le);
}

/* ---------------------------------------------------------------------------
 * action_retag_lines() — re-derive the blue on-action styling for every
 * line between the two buffer OFFSETS (their lines, inclusive).  Cheap
 * enough to run per edit: the affected range is normally one line.
 * ------------------------------------------------------------------------- */
static void
action_retag_lines(OnEditor *ed, gint start_off, gint end_off)
{
    gint first, last;                /* affected line range                 */
    line_range_for_offsets(ed, start_off, end_off, &first, &last);
    for (gint line = first; line <= last; line++)
        action_retag_line(ed, line);
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
 * the pass — see on_editor_title_refresh_all.
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
title_view_sync(OnEditor *ed)
{
    gboolean on    = ed->app->first_line_title;   /* feature gate          */
    gboolean empty = gtk_text_buffer_get_char_count(ed->buffer) == 0;

    /* A completely empty buffer has no character at all, so no tag can
     * apply and the caret on line 0 can only be styled by the VIEW's own
     * defaults — justification here, font size via the style class below.
     * Both are exact rather than a compromise: an empty buffer IS line 0
     * and nothing else, so there is no body text to catch the fallout.
     * Any content flips them back, and the tags take over.                 */
    GtkJustification want = (on && empty) ? GTK_JUSTIFY_CENTER
                                          : GTK_JUSTIFY_LEFT;
    if (gtk_text_view_get_justification(ed->view) != want)
        gtk_text_view_set_justification(ed->view, want);

    GtkWidget *view = GTK_WIDGET(ed->view);
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
title_sync_line(OnEditor *ed, gint line)
{
    gboolean on = ed->app->first_line_title;      /* feature gate          */
    GtkTextIter ls, le;              /* line span (see banner)              */
    gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
    le = ls;
    if (gtk_text_iter_ends_line(&le))
        gtk_text_iter_forward_char(&le);          /* empty line: its '\n'   */
    else
        gtk_text_iter_forward_to_line_end(&le);

    gboolean title = on && line == 0;
    if (title)
        gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-title-center",
                                          &ls, &le);
    else
        gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-title-center",
                                           &ls, &le);
    /* Size only where no paragraph style of its own would multiply it.      */
    if (title && on_flags_at_iter(ed->buffer, &ls, ON_FMT_PARA_MASK) == 0)
        gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-title-size",
                                          &ls, &le);
    else
        gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-title-size",
                                           &ls, &le);
}

static void
title_line_sync(OnEditor *ed, gint start_off, gint end_off)
{
    title_view_sync(ed);
    gint first, last;                /* affected line range                 */
    line_range_for_offsets(ed, start_off, end_off, &first, &last);
    for (gint line = first; line <= last; line++)
        title_sync_line(ed, line);
}

/* ---------------------------------------------------------------------------
 * editor_rederive() — run the editor-only DERIVED passes over one range.
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
editor_rederive(OnEditor *ed, gint start_off, gint end_off, guint what)
{
    if (what & RD_EMOJI)
        tag_emoji_in_range(ed, start_off, end_off);
    if (what & RD_TITLE)
        title_view_sync(ed);

    if (what & (RD_ACTION | RD_TITLE)) {
        gint first, last;            /* affected line range, resolved once  */
        line_range_for_offsets(ed, start_off, end_off, &first, &last);
        for (gint line = first; line <= last; line++) {
            if (what & RD_ACTION)
                action_retag_line(ed, line);
            if (what & RD_TITLE)
                title_sync_line(ed, line);
        }
    }

    return (what & RD_CODE_TAG)
           ? strip_tags_in_code_blocks(ed, start_off, end_off) : FALSE;
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
 * numbering the extractor and action_strike_ord use).
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
action_marks_clear(OnEditor *ed)
{
    if (ed->action_marks == NULL)
        return;
    for (guint i = 0; i < ed->action_marks->len; i++) {
        ActionMark *am = g_ptr_array_index(ed->action_marks, i);
        if (!gtk_text_mark_get_deleted(am->mark))
            gtk_text_buffer_delete_mark(ed->buffer, am->mark);
    }
    g_ptr_array_set_size(ed->action_marks, 0);
}

/* ---------------------------------------------------------------------------
 * action_marks_sync() — re-place the identity marks so the n-th real
 * action line carries the n-th item's uid.  Called after any save that
 * rewrote the table (the uids are then known) and once at load.
 *   items — OnActionItem list in ord order, uids filled in.
 * ------------------------------------------------------------------------- */
static void
action_marks_sync(OnEditor *ed, GList *items)
{
    if (ed->action_marks == NULL)
        return;
    action_marks_clear(ed);

    GArray *lines = action_real_lines(ed->buffer);
    guint   i = 0;                   /* index into the line array           */
    for (GList *l = items; l != NULL && i < lines->len; l = l->next, i++) {
        OnActionItem *it = l->data;
        if (it->uid == 0)            /* nothing to remember                 */
            continue;
        GtkTextIter ls;              /* the line's start                    */
        gtk_text_buffer_get_iter_at_line(ed->buffer, &ls,
                                         g_array_index(lines, gint, i));
        ActionMark *am = g_new0(ActionMark, 1);
        am->mark = gtk_text_buffer_create_mark(ed->buffer, NULL, &ls, TRUE);
        am->uid  = it->uid;
        g_ptr_array_add(ed->action_marks, am);
    }
    g_array_unref(lines);
}

/* ---------------------------------------------------------------------------
 * action_marks_hint() — fill each item's uid with the uid marked on its
 * line, as a match hint for on_db_note_set_actions.  An item whose line
 * carries no surviving mark keeps uid 0 ("no hint"), leaving the text and
 * ord passes to identify it.
 *   items — freshly extracted OnActionItem list, in ord order.
 * ------------------------------------------------------------------------- */
static void
action_marks_hint(OnEditor *ed, GList *items)
{
    if (ed->action_marks == NULL || ed->action_marks->len == 0)
        return;

    /* line number → uid, for the marks that are still alive.               */
    GHashTable *by_line = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < ed->action_marks->len; i++) {
        ActionMark *am = g_ptr_array_index(ed->action_marks, i);
        if (gtk_text_mark_get_deleted(am->mark))
            continue;
        GtkTextIter at;              /* the mark's current position         */
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &at, am->mark);
        gint line = gtk_text_iter_get_line(&at);
        /* First mark on a line wins: a second one can only be a leftover
         * from a line that was merged into this one.                       */
        if (!g_hash_table_contains(by_line, GINT_TO_POINTER(line)))
            g_hash_table_insert(by_line, GINT_TO_POINTER(line),
                                &am->uid);
    }

    GArray *lines = action_real_lines(ed->buffer);
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
action_marks_prune(OnEditor *ed, const GtkTextIter *start,
                   const GtkTextIter *end)
{
    if (ed->action_marks == NULL || ed->action_marks->len == 0)
        return;
    for (guint i = ed->action_marks->len; i > 0; i--) {
        ActionMark *am = g_ptr_array_index(ed->action_marks, i - 1);
        if (gtk_text_mark_get_deleted(am->mark)) {
            g_ptr_array_remove_index(ed->action_marks, i - 1);
            continue;
        }
        GtkTextIter at;              /* the mark's current position         */
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &at, am->mark);
        if (gtk_text_iter_compare(&at, start) >= 0 &&
            gtk_text_iter_compare(&at, end) < 0) {
            gtk_text_buffer_delete_mark(ed->buffer, am->mark);
            g_ptr_array_remove_index(ed->action_marks, i - 1);
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
 * action_strike_ord() — strike (or un-strike) the text of the `ord`-th
 * REAL action line of `buffer` (see action_rest_real).  Works on any
 * buffer that has been through on_buffer_ensure_tags — live editors and
 * offscreen loads alike.  Returns TRUE when the line was found.
 * ------------------------------------------------------------------------- */
static gboolean
action_strike_ord(GtkTextBuffer *buffer, gint ord, gboolean done)
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
 * action_due_ord() — rewrite the "due <date>" suffix of the `ord`-th
 * REAL action line: any existing suffix is removed, then " due
 * YYYY-MM-DD" is appended for a non-zero `due` (ISO — the canonical
 * written form; the parser also reads M/D/YY).  Appended text inherits
 * the strike state of the item text so a done item stays done.
 * Returns TRUE when the line was found.
 * ------------------------------------------------------------------------- */
static gboolean
action_due_ord(GtkTextBuffer *buffer, gint ord, gint64 due)
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
 * action_text_ord() — replace the TEXT of the `ord`-th REAL action line,
 * leaving the '!' prefix, the line's own whitespace around the text and
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
static gboolean
action_text_ord(GtkTextBuffer *buffer, gint ord, const gchar *text)
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
     * same probe action_due_ord makes for the suffix it appends.          */
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

/* ---------------------------------------------------------------------------
 * action_lists_equal() — same items, same order, same done flags, same
 * due dates?  Every OnActionItem field the table mirrors must be
 * compared here, or edits to that field never reach the library.
 * ------------------------------------------------------------------------- */
static gboolean
action_lists_equal(GList *a, GList *b)
{
    for (; a != NULL && b != NULL; a = a->next, b = b->next) {
        OnActionItem *x = a->data, *y = b->data;
        if (x->done != y->done || x->due != y->due ||
            g_strcmp0(x->text, y->text) != 0)
            return FALSE;
    }
    return a == NULL && b == NULL;
}

/* ---------------------------------------------------------------------------
 * action_apply_to_note() — run one buffer edit (strike or due rewrite)
 * against note `note_id`: on the live buffer + autosave when an editor
 * is open (the save's extract-and-compare refreshes action_items), else
 * on an offscreen load with an immediate save + action_items resync
 * (images keep their cached PNG bytes, so nothing is re-encoded).
 *   edit — the line edit; receives (buffer, ord, arg).
 *   arg  — passed to `edit` untouched.  It is a pointer rather than a
 *          value because the edits need a gboolean, a gint64 and a
 *          string between them; each adapter below casts it back.  It is
 *          only ever borrowed for the duration of the call.
 * Returns TRUE when the item was found and updated.
 * ------------------------------------------------------------------------- */
typedef gboolean (*ActionEdit)(GtkTextBuffer *buffer, gint ord,
                               gconstpointer arg);

static gboolean
action_apply_to_note(OnApp *app, gint64 note_id, ActionEdit edit,
                     gint ord, gconstpointer arg, gboolean *synced)
{
    if (synced != NULL)
        *synced = FALSE;

    OnEditor *ed = editor_lookup(app, note_id);   /* NULL headless / closed */
    if (ed != NULL) {
        /* Live buffer: the autosave writes content AND the mirror later.    */
        if (!edit(ed->buffer, ord, arg))
            return FALSE;
        editor_queue_autosave(ed);
        return TRUE;
    }

    /* Full resolution: this buffer IS saved again below, and a scaled
     * pixbuf would no longer match its cached PNG bytes.  A note with no
     * content yields an empty buffer, where `edit` finds no action line and
     * fails on its own — no special case needed.                            */
    GtkTextBuffer *buffer = on_note_buffer_load(app->db, note_id, 0);

    gboolean ok = edit(buffer, ord, arg);
    if (ok) {
        gsize   out_len;             /* re-serialized blob size             */
        guint8 *out = on_note_serialize(buffer, &out_len);
        gchar  *title = on_buffer_first_line(buffer);
        gchar  *body = NULL;         /* searchable plain text               */
        GList  *actions = NULL;      /* the rewritten '!' lines             */
        on_note_extract(out, out_len, &body, &actions);
        ok = on_db_note_save(app->db, note_id, title, out, out_len, body) &&
             on_db_note_set_actions(app->db, note_id, actions);
        if (ok && synced != NULL)
            *synced = TRUE;          /* the mirror is already up to date    */
        on_db_action_list_free(actions);
        g_free(body);
        g_free(title);
        g_free(out);
    }
    g_object_unref(buffer);
    return ok;
}

/* action_strike_edit() — ActionEdit adapter for action_strike_ord.          */
static gboolean
action_strike_edit(GtkTextBuffer *buffer, gint ord, gconstpointer arg)
{
    return action_strike_ord(buffer, ord, *(const gboolean *)arg);
}

/* action_due_edit() — ActionEdit adapter for action_due_ord.                */
static gboolean
action_due_edit(GtkTextBuffer *buffer, gint ord, gconstpointer arg)
{
    return action_due_ord(buffer, ord, *(const gint64 *)arg);
}

/* action_text_edit() — ActionEdit adapter for action_text_ord.              */
static gboolean
action_text_edit(GtkTextBuffer *buffer, gint ord, gconstpointer arg)
{
    return action_text_ord(buffer, ord, arg);
}

gboolean
on_editor_action_set_done(OnApp *app, gint64 note_id, gint ord,
                          gboolean done, gboolean *synced)
{
    return action_apply_to_note(app, note_id, action_strike_edit,
                                ord, &done, synced);
}

gboolean
on_editor_action_set_due(OnApp *app, gint64 note_id, gint ord, gint64 due)
{
    return action_apply_to_note(app, note_id, action_due_edit, ord, &due,
                                NULL);
}

gboolean
on_editor_action_set_text(OnApp *app, gint64 note_id, gint ord,
                          const gchar *text)
{
    return action_apply_to_note(app, note_id, action_text_edit, ord, text,
                                NULL);
}

/* ===========================================================================
 * task-list checkboxes
 *
 * Each task line starts with a child anchor holding the checked state;
 * the editor attaches a native GtkCheckButton there.  Toggling writes
 * straight through to the anchor data and autosaves.
 * =========================================================================== */

/* on_task_checkbox_toggled() — sync the widget state into the anchor.       */
static void
on_task_checkbox_toggled(GtkCheckButton *btn, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(btn), "on-anchor");
    if (anchor != NULL)
        on_anchor_set_checkbox(anchor, gtk_check_button_get_active(btn));
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * attach_checkbox_widget() — give a checkbox anchor its GtkCheckButton
 * (replacing any widget it already had).
 * ------------------------------------------------------------------------- */
static void
attach_checkbox_widget(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    gboolean checked;                /* the anchor's stored state           */
    if (!on_anchor_is_checkbox(anchor, &checked))
        return;

    anchor_clear_widgets(ed, anchor);

    GtkWidget *btn = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(btn), checked);
    /* Anchored children sit with their BOTTOM on the text baseline, so
     * theme padding above/below the indicator lifts the box's center
     * above the text's optical center.  Strip it (pinned for all states,
     * like the code copy button — quirk of themed sizes) so the widget
     * is just the bare indicator.                                          */
    on_app_widget_add_css(btn,
        "checkbutton, checkbutton:hover, checkbutton:active "
        "{ padding: 0; min-height: 0; } check { margin: 0; }");
    /* Keyboard focus stays in the text; the box is mouse-only, and shows
     * the hyperlink-style hand while hovered.                              */
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_set_cursor_from_name(btn, "pointer");
    g_object_set_data(G_OBJECT(btn), "on-anchor", anchor);
    g_signal_connect(btn, "toggled",
                     G_CALLBACK(on_task_checkbox_toggled), ed);
    gtk_text_view_add_child_at_anchor(ed->view, btn, anchor);

    /* Lower the anchor character with the editor-only negative-rise tag
     * (created at buffer setup) so the box centers on the line's text.
     * Tag applies are invisible to the change/autosave machinery.          */
    GtkTextIter it, next;            /* the anchor's one-char span          */
    gtk_text_buffer_get_iter_at_child_anchor(ed->buffer, &it, anchor);
    next = it;
    gtk_text_iter_forward_char(&next);
    gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-check-drop",
                                      &it, &next);
}

/* ---------------------------------------------------------------------------
 * insert_checkbox_at() — put a fresh unchecked checkbox anchor (plus its
 * separating space) at buffer offset `at`, tagged as part of the line.
 * ------------------------------------------------------------------------- */
static void
insert_checkbox_at(OnEditor *ed, gint at)
{
    GtkTextIter it;                  /* insertion position                  */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, at);
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(ed->buffer, &it);
    on_anchor_set_checkbox(anchor, FALSE);
    gtk_text_buffer_insert(ed->buffer, &it, " ", -1);

    GtkTextIter ts, te;              /* the anchor + space span             */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ts, at);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &te, at + 2);
    gtk_text_buffer_apply_tag_by_name(ed->buffer, ON_TAGNAME_LIST_CHECK,
                                      &ts, &te);
    attach_checkbox_widget(ed, anchor);
}

/* ===========================================================================
 * embedded tables
 *
 * A table is a child anchor carrying an OnTable (see serialize.h); the
 * editor attaches a GtkGrid of GtkEntry cells at it.  Right-clicking any
 * cell offers structural changes (add/remove rows and columns, delete),
 * which rebuild the widget from the updated data.
 * =========================================================================== */


/* on_table_cell_changed() — a cell buffer edited: write through to the
 * data (cells are small GtkTextViews so content can be multiline; a bare
 * text view requests its content size, so cells auto-grow).                 */
static void
on_table_cell_changed(GtkTextBuffer *cell_buf, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
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
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * table_op() — one structural operation on the table the last context
 * menu was opened on (ed->ctx_offset), named by the action that asked:
 * "row-add", "row-del", "col-add", "col-del", "header" or "delete".
 * ------------------------------------------------------------------------- */
static void
table_op(OnEditor *ed, const gchar *op)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(ed, ed->ctx_offset);
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
        gtk_text_buffer_get_iter_at_child_anchor(ed->buffer, &s, anchor);
        e = s;
        gtk_text_iter_forward_char(&e);
        ed->internal_change++;
        gtk_text_buffer_delete(ed->buffer, &s, &e);
        ed->internal_change--;
        editor_queue_autosave(ed);
        return;
    }

    attach_table_widget(ed, anchor); /* rebuild at the new dimensions       */
    editor_queue_autosave(ed);
}

/* The "table-<op>" actions carry the op in their name.                     */
#define TABLE_ACTION_PREFIX "table-"

/* on_table_command() — "activate" of a plain "win.table-*" action.         */
static void
on_table_command(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)param;
    table_op(user_data,
             g_action_get_name(G_ACTION(action)) + strlen(TABLE_ACTION_PREFIX));
}

/* on_table_header_change_state() — "win.table-header", the stateful one
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
 * on_table_cell_pressed() — a context-menu press in a cell (its click
 * gesture, CAPTURE phase so it precedes the cell view's own gesture, which
 * would open the standard text popup): the structural menu (add/remove
 * last row/column, header row, delete the table).  The cell's table
 * becomes the context the "win.table-*" actions act on.
 *
 * The popover is parented to the EDITOR's view, not the cell, with the
 * press translated into its coordinates: every table op rebuilds the grid
 * — the cell would die while the menu was still its child, and
 * on_app_menu_popup unparents only from an idle after "closed" (the
 * action runs before that idle).  The editor's view outlives any table op.
 * ------------------------------------------------------------------------- */
static void
on_table_cell_pressed(GtkGestureClick *gesture, gint n_press, gdouble x,
                      gdouble y, gpointer user_data)
{
    (void)n_press;
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkWidget *cell =                /* the cell view the press landed in   */
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(gesture));
    if (event == NULL || !gdk_event_triggers_context_menu(event))
        return;                      /* the cell's own gesture: caret       */

    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(cell), "on-anchor");
    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;
    if (table == NULL)
        return;

    GtkTextIter it;                  /* where the anchor sits               */
    gtk_text_buffer_get_iter_at_child_anchor(ed->buffer, &it, anchor);
    ed->ctx_offset = gtk_text_iter_get_offset(&it);

    /* The check item mirrors the table's current header state.            */
    GAction *header = g_action_map_lookup_action(G_ACTION_MAP(ed->window),
                                                 "table-header");
    g_simple_action_set_state(G_SIMPLE_ACTION(header),
                              g_variant_new_boolean(table->header));

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();
    g_menu_append(section, "Add _Row",      "win.table-row-add");
    g_menu_append(section, "Add _Column",   "win.table-col-add");
    g_menu_append(section, "Remove Row",    "win.table-row-del");
    g_menu_append(section, "Remove Column", "win.table-col-del");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    g_menu_append(section, "_Header Row",   "win.table-header");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
    g_menu_append(section, "_Delete Table", "win.table-delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);

    graphene_point_t at;             /* the press, in the editor view       */
    if (!gtk_widget_compute_point(cell, GTK_WIDGET(ed->view),
                                  &GRAPHENE_POINT_INIT((float)x, (float)y),
                                  &at))
        at = GRAPHENE_POINT_INIT(0, 0);
    on_app_menu_popup(GTK_WIDGET(ed->view), G_MENU_MODEL(menu),
                      at.x, at.y);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ---------------------------------------------------------------------------
 * attach_table_widget() — (re)build the GtkGrid of cell views representing
 * an anchor's table, replacing any widget it already had.
 * ------------------------------------------------------------------------- */
static void
attach_table_widget(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    OnTable *table = on_anchor_get_table(anchor);
    if (table == NULL)
        return;

    /* Drop the previous widget (after a structural change).                */
    anchor_clear_widgets(ed, anchor);

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
            gtk_widget_set_size_request(cell, 64, -1);

            /* Header row: bold on a light grey fill.                       */
            if (table->header && r == 0)
                on_app_widget_add_css(cell,
                    "textview, textview text {"
                    "  background-color: #ececec;"
                    "  font-weight: bold;"
                    "}");

            g_object_set_data(G_OBJECT(cell_buf), "on-anchor", anchor);
            g_object_set_data(G_OBJECT(cell_buf), "on-row",
                              GINT_TO_POINTER(r));
            g_object_set_data(G_OBJECT(cell_buf), "on-col",
                              GINT_TO_POINTER(c));
            g_signal_connect(cell_buf, "changed",
                             G_CALLBACK(on_table_cell_changed), ed);
            g_object_set_data(G_OBJECT(cell), "on-anchor", anchor);
            GtkGesture *press = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
            gtk_event_controller_set_propagation_phase(
                GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
            g_signal_connect(press, "pressed",
                             G_CALLBACK(on_table_cell_pressed), ed);
            gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(press));

            GtkWidget *frame = gtk_frame_new(NULL);
            gtk_frame_set_child(GTK_FRAME(frame), cell);
            gtk_grid_attach(GTK_GRID(grid), frame, c, r, 1, 1);
        }
    }
    gtk_text_view_add_child_at_anchor(ed->view, grid, anchor);
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
 * tag_emoji_in_range() — apply the padding tag around every emoji between
 * the two buffer offsets.  The "on-emoji" tag adds letter spacing, which
 * Pango splits half-per-side at run edges — so the emoji itself gets
 * half a gap each side, and tagging the FOLLOWING character as well
 * doubles the trailing gap (Apple Color Emoji bleeds mostly rightward).
 *
 * macOS-only: on Linux, color emoji fonts (e.g. Noto) fit their advance
 * and need no artificial padding, so this is compiled to a no-op there.
 * Editor-only styling; never serialized.
 * ------------------------------------------------------------------------- */
static void
tag_emoji_in_range(OnEditor *ed, gint start_off, gint end_off)
{
#ifdef __APPLE__
    /* Start one character early so text typed directly after an existing
     * emoji still receives the follower's share of the padding.            */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it,
                                       MAX(0, start_off - 1));
    while (gtk_text_iter_get_offset(&it) < end_off) {
        if (is_emoji_char(gtk_text_iter_get_char(&it))) {
            GtkTextIter next = it;   /* the following character             */
            gtk_text_iter_forward_char(&next);
            gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-emoji",
                                              &it, &next);
            if (!gtk_text_iter_is_end(&next) &&
                !is_emoji_char(gtk_text_iter_get_char(&next))) {
                GtkTextIter after = next;
                gtk_text_iter_forward_char(&after);
                gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-emoji",
                                                  &next, &after);
            }
        }
        if (!gtk_text_iter_forward_char(&it))
            break;
    }
#else
    (void)ed; (void)start_off; (void)end_off;
#endif
}

/* ---------------------------------------------------------------------------
 * on_insert_emoji() — "win.insert-emoji" (Insert menu, Primary+E): open
 * GTK's built-in emoji chooser at the cursor via the text view's
 * "insert-emoji" action (the same one bound to Ctrl+. natively).  The
 * picked emoji is inserted as plain UTF-8 text, so styling, storage and
 * export handle it like any typed character.
 * ------------------------------------------------------------------------- */
static void
on_insert_emoji(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    g_signal_emit_by_name(ed->view, "insert-emoji");
}

/* ---------------------------------------------------------------------------
 * on_insert_table() — "win.insert-table": embed a fresh 3×3 table at the
 * cursor.  Rows/columns are added or removed afterwards from any cell's
 * right-click menu.
 * ------------------------------------------------------------------------- */
static void
on_insert_table(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */

    GtkTextIter cursor;              /* insertion point                     */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));
    ed->internal_change++;
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(ed->buffer, &cursor);
    on_anchor_set_table(anchor, on_table_new(3, 3));
    attach_table_widget(ed, anchor);
    ed->internal_change--;
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * on_insert_date() — "win.insert-date" (Insert menu, Primary+D): insert
 * today's date in ISO format (YYYY-MM-DD) at the cursor, replacing the
 * selection like typed text would.  It goes through the normal insert-text
 * path, so styling, autosave and undo of the surrounding text behave
 * exactly as for a paste.
 * ------------------------------------------------------------------------- */
static void
on_insert_date(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    GDateTime *now = g_date_time_new_now_local();
    gchar *date = g_date_time_format(now, "%Y-%m-%d");
    gtk_text_buffer_delete_selection(ed->buffer, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(ed->buffer, date, -1);
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
 *   ed        — the editor.
 *   start_off — first buffer offset of the range to scan.
 *   end_off   — offset just past the range.
 * Returns TRUE when at least one span was stripped — callers then mark
 * the tag set modified so the next save rewrites note_tags.
 * ------------------------------------------------------------------------- */
static gboolean
strip_tags_in_code_blocks(OnEditor *ed, gint start_off, gint end_off)
{
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_TAG);
    if (tag == NULL)
        return FALSE;

    gboolean stripped = FALSE;       /* removed anything yet?               */
    GtkTextIter it, limit;           /* walk position / range end           */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, start_off);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &limit, end_off);

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
        if (line_para_flags(ed->buffer, &it) & ON_FMT_CODEBLOCK) {
            ed->internal_change++;
            gtk_text_buffer_remove_tag(ed->buffer, tag, &it, &span_end);
            ed->internal_change--;
            stripped = TRUE;
        }
        it = span_end;
    }
    return stripped;
}

/* ---------------------------------------------------------------------------
 * tag_capture_span() — compute the span of the tag currently being typed:
 * from the '#' at ed->tag_start forward across word characters.
 *   ed    — the editor (capture must be active).
 *   start — receives the position of the '#'.
 *   end   — receives the position just past the last word character.
 * Returns TRUE if the span still starts with '#' (capture is valid).
 * ------------------------------------------------------------------------- */
static gboolean
tag_capture_span(OnEditor *ed, GtkTextIter *start, GtkTextIter *end)
{
    gtk_text_buffer_get_iter_at_mark(ed->buffer, start, ed->tag_start);
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
tag_popup_hide(OnEditor *ed)
{
    if (ed->tag_popup != NULL)
        gtk_popover_popdown(GTK_POPOVER(ed->tag_popup));
}

/* ---------------------------------------------------------------------------
 * tag_capture_end() — finish the active tag capture.
 *   ed    — the editor.
 *   apply — TRUE to style the "#word" span as a tag (if the word is
 *           non-empty); FALSE to abandon it as plain text (Escape).
 * ------------------------------------------------------------------------- */
static void
tag_capture_end(OnEditor *ed, gboolean apply)
{
    if (ed->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* the "#word" span                    */
    if (tag_capture_span(ed, &start, &end) && apply) {
        /* Only style it if there is at least one char after the '#'.       */
        if (gtk_text_iter_get_offset(&end) -
            gtk_text_iter_get_offset(&start) >= 2) {
            ed->internal_change++;
            gtk_text_buffer_apply_tag_by_name(ed->buffer, ON_TAGNAME_TAG,
                                              &start, &end);
            ed->internal_change--;
            ed->tags_modified = TRUE;    /* a tag was created               */
            editor_queue_autosave(ed);
        }
    }
    gtk_text_buffer_delete_mark(ed->buffer, ed->tag_start);
    ed->tag_start = NULL;
    on_db_tag_list_free(ed->tag_choices);
    ed->tag_choices = NULL;
    tag_popup_hide(ed);
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
    OnEditor *ed = user_data;        /* owning editor                       */
    const gchar *name =              /* chosen tag name (no '#')            */
        g_object_get_data(G_OBJECT(row), "on-tag-name");
    if (name == NULL || ed->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* current "#partial" span             */
    if (!tag_capture_span(ed, &start, &end)) {
        tag_capture_end(ed, FALSE);
        return;
    }

    ed->internal_change++;
    /* Replace "#partial" with "#name ".                                    */
    gint at = gtk_text_iter_get_offset(&start);
    gtk_text_buffer_delete(ed->buffer, &start, &end);
    gchar *full = g_strdup_printf("#%s", name);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &start, at);
    gtk_text_buffer_insert(ed->buffer, &start, full, -1);

    GtkTextIter ts, te;              /* span of the completed tag           */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ts, at);
    gtk_text_buffer_get_iter_at_offset(
        ed->buffer, &te, at + (gint)g_utf8_strlen(full, -1));
    gtk_text_buffer_apply_tag_by_name(ed->buffer, ON_TAGNAME_TAG, &ts, &te);

    /* Trailing space ends the tag (untagged).                              */
    gtk_text_buffer_insert(ed->buffer, &te, " ", -1);
    g_free(full);
    ed->internal_change--;

    /* Capture is over; the tag styling was applied above.                  */
    gtk_text_buffer_delete_mark(ed->buffer, ed->tag_start);
    ed->tag_start = NULL;
    on_db_tag_list_free(ed->tag_choices);
    ed->tag_choices = NULL;
    tag_popup_hide(ed);
    ed->tags_modified = TRUE;        /* a tag was created                   */
    editor_queue_autosave(ed);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * tag_popup_ensure() — lazily build the popover + listbox.  A GtkPopover
 * parented to the view (notes_text_view_size_allocate presents it, its
 * dispose unparents it), pointing at the '#' from below with its left edge
 * on it.  NOT autohide and NOT focusable: the view keeps the keyboard and
 * the pointer, typing goes on filtering the rows, and a click on a row
 * cannot move the focus.
 * ------------------------------------------------------------------------- */
static void
tag_popup_ensure(OnEditor *ed)
{
    if (ed->tag_popup != NULL)
        return;

    ed->tag_popup = gtk_popover_new();
    gtk_popover_set_autohide(GTK_POPOVER(ed->tag_popup), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(ed->tag_popup), FALSE);
    gtk_popover_set_position(GTK_POPOVER(ed->tag_popup), GTK_POS_BOTTOM);
    gtk_widget_set_halign(ed->tag_popup, GTK_ALIGN_START);
    gtk_widget_set_can_focus(ed->tag_popup, FALSE);
    gtk_widget_set_parent(ed->tag_popup, GTK_WIDGET(ed->view));

    ed->tag_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ed->tag_listbox),
                                    GTK_SELECTION_SINGLE);
    gtk_popover_set_child(GTK_POPOVER(ed->tag_popup), ed->tag_listbox);
    g_signal_connect(ed->tag_listbox, "row-activated",
                     G_CALLBACK(on_tag_row_activated), ed);
}

/* ---------------------------------------------------------------------------
 * tag_popup_update() — refresh the popover contents to the tags matching
 * the currently typed prefix, point it at the '#', and show or hide it
 * depending on whether anything matches.
 * ------------------------------------------------------------------------- */
static void
tag_popup_update(OnEditor *ed)
{
    if (ed->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* current "#partial" span             */
    if (!tag_capture_span(ed, &start, &end)) {
        tag_capture_end(ed, FALSE);
        return;
    }

    /* The prefix typed so far, without the '#'.                            */
    gchar *typed = gtk_text_buffer_get_text(ed->buffer, &start, &end, FALSE);
    const gchar *prefix = typed + 1;

    tag_popup_ensure(ed);

    /* Clear old suggestion rows.                                           */
    gtk_list_box_remove_all(GTK_LIST_BOX(ed->tag_listbox));

    /* Fill with case-insensitive prefix matches from the capture-start
     * snapshot (see ed->tag_choices).                                      */
    gint shown = 0;                  /* number of rows added                */
    gchar *prefix_ci = g_utf8_casefold(prefix, -1);
    for (GList *l = ed->tag_choices;
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
        gtk_list_box_insert(GTK_LIST_BOX(ed->tag_listbox), row, -1);
        shown++;
    }
    g_free(prefix_ci);
    g_free(typed);

    if (shown == 0) {
        tag_popup_hide(ed);
        return;
    }

    /* Select the first row so Enter picks it immediately.                  */
    GtkListBoxRow *first_row =
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(ed->tag_listbox), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(ed->tag_listbox), first_row);

    /* Point the popover at the '#' character: its box in buffer
     * coordinates, translated into the view's widget coordinates, which is
     * the space set_pointing_to takes for a popover parented to the view.  */
    GdkRectangle rect;               /* the '#' in buffer coords            */
    gtk_text_view_get_iter_location(ed->view, &start, &rect);
    GdkRectangle at = rect;          /* the same box, widget coords         */
    gtk_text_view_buffer_to_window_coords(ed->view, GTK_TEXT_WINDOW_WIDGET,
                                          rect.x, rect.y, &at.x, &at.y);
    gtk_popover_set_pointing_to(GTK_POPOVER(ed->tag_popup), &at);
    gtk_popover_popup(GTK_POPOVER(ed->tag_popup));
}

/* ---------------------------------------------------------------------------
 * tag_popup_move_selection() — move the popup's selected row up or down
 * (keyboard navigation while typing a tag).
 *   ed    — the editor (popup must exist).
 *   delta — +1 for down, -1 for up.
 * ------------------------------------------------------------------------- */
static void
tag_popup_move_selection(OnEditor *ed, gint delta)
{
    GtkListBox *box = GTK_LIST_BOX(ed->tag_listbox);
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
 * table the editor keeps as anchor data.  It is switched off on the note
 * buffer (editor_build_view) and the editor keeps its own history of
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
 * editor_queue_autosave(), which (re)arms a UNDO_GROUP_MS debounce
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
undo_snapshot_capture(OnEditor *ed)
{
    UndoSnap *snap = g_new0(UndoSnap, 1);
    snap->segs = g_ptr_array_new();

    GtkTextIter ins;                 /* current cursor                      */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &ins,
                                     gtk_text_buffer_get_insert(ed->buffer));
    snap->cursor = gtk_text_iter_get_offset(&ins);

    /* THE buffer traversal — the same one the serializer uses, so a
     * snapshot can never disagree with what a save would write.            */
    on_buffer_walk(ed->buffer, undo_capture_seg, snap);
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
undo_commit_now(OnEditor *ed)
{
    ed->undo_sentences = 0;          /* the cap counts per group            */
    if (ed->undo_commit_source != 0) {
        g_source_remove(ed->undo_commit_source);
        ed->undo_commit_source = 0;
    }

    UndoSnap *now = undo_snapshot_capture(ed);
    if (ed->undo_current != NULL && undo_snap_equal(now, ed->undo_current)) {
        undo_snap_free(ed->undo_current);
        ed->undo_current = now;      /* content unchanged: refresh cursor   */
        return;
    }

    if (ed->undo_current != NULL) {
        if (ed->undo_stack->len >= UNDO_MAX_GROUPS) {
            undo_snap_free(g_ptr_array_index(ed->undo_stack, 0));
            g_ptr_array_remove_index(ed->undo_stack, 0);
        }
        g_ptr_array_add(ed->undo_stack, ed->undo_current);
    }
    ed->undo_current = now;

    for (guint i = 0; i < ed->redo_stack->len; i++)
        undo_snap_free(g_ptr_array_index(ed->redo_stack, i));
    g_ptr_array_set_size(ed->redo_stack, 0);
}

/* on_undo_commit_timeout() — the group timer fired: commit.                 */
static gboolean
on_undo_commit_timeout(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->undo_commit_source = 0;
    undo_commit_now(ed);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * undo_notify_change() — called (via editor_queue_autosave) on every
 * buffer mutation: (re)arm the group-commit debounce.  Each change
 * resets the timer, so the group closes UNDO_GROUP_MS after the user
 * STOPS editing — one undo step per typing burst.
 * ------------------------------------------------------------------------- */
static void
undo_notify_change(OnEditor *ed)
{
    if (ed->undo_restoring || ed->undo_stack == NULL)
        return;
    if (ed->undo_commit_source != 0)
        g_source_remove(ed->undo_commit_source);
    ed->undo_commit_source = g_timeout_add(UNDO_GROUP_MS,
                                           on_undo_commit_timeout, ed);
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
undo_insert_seg_slice(OnEditor *ed, const UndoSeg *seg, gint s, gint n,
                      gint at)
{
    GtkTextIter it;                  /* insertion point                     */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, at);

    if (seg->kind == UNDO_SEG_TEXT) {
        const gchar *ps = g_utf8_offset_to_pointer(seg->text, s);
        const gchar *pe = g_utf8_offset_to_pointer(ps, n);
        gtk_text_buffer_insert(ed->buffer, &it, ps, (gint)(pe - ps));
    } else {
        GtkTextChildAnchor *anchor =
            gtk_text_buffer_create_child_anchor(ed->buffer, &it);
        switch (seg->kind) {
        case UNDO_SEG_IMAGE:
            on_anchor_set_image(anchor, seg->pixbuf, seg->display_width);
            attach_image_widget(ed, anchor);
            break;
        case UNDO_SEG_CHECK:
            on_anchor_set_checkbox(anchor, seg->checked);
            attach_checkbox_widget(ed, anchor);
            break;
        case UNDO_SEG_TABLE:
            on_anchor_set_table(anchor, undo_table_copy(seg->table));
            attach_table_widget(ed, anchor);
            break;
        default:
            break;
        }
    }

    GtkTextIter ss, se;              /* the span just inserted              */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ss, at);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &se, at + n);
    for (gsize t = 0; t < on_n_flag_tags; t++) {
        if (seg->flags & on_flag_tags[t].flag)
            gtk_text_buffer_apply_tag_by_name(
                ed->buffer, on_flag_tags[t].tag_name, &ss, &se);
        else
            gtk_text_buffer_remove_tag_by_name(
                ed->buffer, on_flag_tags[t].tag_name, &ss, &se);
    }
    gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-search-hit",
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
undo_restore(OnEditor *ed, const UndoSnap *from, const UndoSnap *to)
{
    if (ed->tag_start != NULL)       /* abandon a half-typed #tag           */
        tag_capture_end(ed, FALSE);

    ed->undo_restoring = TRUE;
    ed->internal_change++;

    gint buf_len = gtk_text_buffer_get_char_count(ed->buffer);
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
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ds, prefix);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &de, buf_len - suffix);
    if (!gtk_text_iter_equal(&ds, &de))
        gtk_text_buffer_delete(ed->buffer, &ds, &de);

    gint at       = prefix;          /* insertion offset                    */
    gint want_end = len_to - suffix; /* target chars [prefix, want_end)     */
    gint pos      = 0;               /* char offset within `to`             */
    for (guint i = 0; i < to->segs->len && pos < want_end; i++) {
        const UndoSeg *seg = g_ptr_array_index(to->segs, i);
        gint slen = undo_seg_len(seg);
        gint s    = MAX(prefix - pos, 0);    /* slice within this segment   */
        gint e    = MIN(want_end - pos, slen);
        if (s < e)
            at += undo_insert_seg_slice(ed, seg, s, e - s, at);
        pos += slen;
    }

    /* Editor-only emoji padding is never part of snapshots — refresh it
     * over the replaced span plus one char each side (the pad tag also
     * covers the char after an emoji).                                     */
    gint emo_s = MAX(prefix - 1, 0);
    gint emo_e = MIN(at + 1, gtk_text_buffer_get_char_count(ed->buffer));
    if (emo_s < emo_e) {
        GtkTextIter es, ee;          /* emoji-refresh span                  */
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &es, emo_s);
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &ee, emo_e);
        gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-emoji",
                                           &es, &ee);
        /* Neither the emoji padding nor the action tint is snapshotted.    */
        editor_rederive(ed, emo_s, emo_e, RD_EMOJI | RD_ACTION);
    }
    /* Nor is the title centering: the restored text comes back untagged,
     * so re-derive it over the head of the buffer.                         */
    title_line_sync(ed, 0, 0);

    GtkTextIter cur;                 /* restored cursor position            */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &cur, to->cursor);
    gtk_text_buffer_place_cursor(ed->buffer, &cur);

    ed->internal_change--;

    /* Re-adopt the inline style at the restored cursor (the guarded
     * cursor-position notify never saw the move).                          */
    ed->inline_flags = gtk_text_iter_backward_char(&cur)
                       ? on_flags_at_iter(ed->buffer, &cur, ON_FMT_INLINE_MASK) : 0;
    update_toggle_buttons(ed);

    editor_queue_autosave(ed);       /* undo_restoring suppresses grouping  */
    code_buttons_queue_rebuild(ed);
    ed->undo_restoring = FALSE;

    gtk_text_view_scroll_to_mark(ed->view,
                                 gtk_text_buffer_get_insert(ed->buffer),
                                 0.1, FALSE, 0.0, 0.0);
}

/* ---------------------------------------------------------------------------
 * editor_undo() / editor_redo() — "win.undo" / "win.redo".  Undo first
 * flushes the in-progress group so the very latest edits are what gets
 * undone.  tags_modified goes up whenever either side of the swap
 * contains #tag spans — the restore may change the note's tag set.
 * ------------------------------------------------------------------------- */
static void
editor_undo(OnEditor *ed)
{
    if (ed->undo_commit_source != 0)
        undo_commit_now(ed);
    if (ed->undo_stack->len == 0)
        return;

    UndoSnap *snap =                 /* state we are going back to          */
        g_ptr_array_index(ed->undo_stack, ed->undo_stack->len - 1);
    g_ptr_array_remove_index(ed->undo_stack, ed->undo_stack->len - 1);

    UndoSnap *from = ed->undo_current;   /* the buffer's present state      */
    if (snap->has_tags || from->has_tags)
        ed->tags_modified = TRUE;
    g_ptr_array_add(ed->redo_stack, from);
    ed->undo_current = snap;
    undo_restore(ed, from, snap);
}

static void
editor_redo(OnEditor *ed)
{
    if (ed->undo_commit_source != 0)
        undo_commit_now(ed);         /* may clear the redo stack            */
    if (ed->redo_stack->len == 0)
        return;

    UndoSnap *snap =                 /* state we are going forward to       */
        g_ptr_array_index(ed->redo_stack, ed->redo_stack->len - 1);
    g_ptr_array_remove_index(ed->redo_stack, ed->redo_stack->len - 1);

    UndoSnap *from = ed->undo_current;   /* the buffer's present state      */
    if (snap->has_tags || from->has_tags)
        ed->tags_modified = TRUE;
    g_ptr_array_add(ed->undo_stack, from);
    ed->undo_current = snap;
    undo_restore(ed, from, snap);
}

/* undo_free_history() — release both stacks and the current snapshot.       */
static void
undo_free_history(OnEditor *ed)
{
    if (ed->undo_stack != NULL) {
        for (guint i = 0; i < ed->undo_stack->len; i++)
            undo_snap_free(g_ptr_array_index(ed->undo_stack, i));
        g_ptr_array_free(ed->undo_stack, TRUE);
        ed->undo_stack = NULL;
    }
    if (ed->redo_stack != NULL) {
        for (guint i = 0; i < ed->redo_stack->len; i++)
            undo_snap_free(g_ptr_array_index(ed->redo_stack, i));
        g_ptr_array_free(ed->redo_stack, TRUE);
        ed->redo_stack = NULL;
    }
    g_clear_pointer(&ed->undo_current, undo_snap_free);
}

/* ===========================================================================
 * buffer signal handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_buffer_insert_text_before() — runs BEFORE an insertion is carried
 * out.  Marks short (typed) insertions so on_cursor_moved() can tell the
 * resulting cursor jump apart from real navigation: GtkTextBuffer emits
 * notify::cursor-position INSIDE its insert-text class handler — before
 * on_buffer_insert_text_after() has applied ed->inline_flags to the new
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
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0)
        return;
    if (g_utf8_strlen(text, len) <= 2)
        ed->typing_insert = TRUE;
}

/* ---------------------------------------------------------------------------
 * on_buffer_insert_text_after() — runs after every insertion.  Three jobs:
 *   1. enforce ed->inline_flags on short (typed) insertions,
 *   2. start a tag capture when '#' is typed at a word boundary,
 *   3. advance/close an active tag capture as the user keeps typing.
 * ------------------------------------------------------------------------- */
static void
on_buffer_insert_text_after(GtkTextBuffer *buffer, GtkTextIter *location,
                            gchar *text, gint len, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->typing_insert = FALSE;       /* set by the before-handler; the
                                      * insertion is fully processed once
                                      * this handler runs                   */
    if (ed->internal_change > 0)
        return;

    glong n_chars = g_utf8_strlen(text, len);
    gint  end_off = gtk_text_iter_get_offset(location);

    /* Pad any emoji in the insertion so they don't overlap neighbours,
     * and re-derive the action-line tint for every line the insertion
     * touched (a paste can create or split '!' lines anywhere).            */
    editor_rederive(ed, end_off - (gint)n_chars, end_off,
                    RD_EMOJI | RD_ACTION | RD_TITLE);
    /* A '!' or newline can create an action line; a paste can too.          */
    if (n_chars > 1 || memchr(text, '!', (size_t)len) ||
        memchr(text, '\n', (size_t)len))
        ed->actions_clean = FALSE;
    gtk_text_buffer_get_iter_at_offset(ed->buffer, location, end_off);

    /* Typing INSIDE an existing styled #tag renames it — flag the tag set
     * as changed so the next save updates note_tags and the sidebar.       */
    if (!ed->tags_modified) {
        GtkTextIter ins_s;           /* first inserted character            */
        gtk_text_buffer_get_iter_at_offset(buffer, &ins_s,
                                           end_off - (gint)n_chars);
        GtkTextTag *ttag = lookup_tag(buffer, ON_TAGNAME_TAG);
        if (ttag != NULL && gtk_text_iter_has_tag(&ins_s, ttag))
            ed->tags_modified = TRUE;
    }

    /* --- job 1: make typed text obey the current inline style ---------- */
    if (n_chars <= 2) {
        GtkTextIter span_s;          /* start of the inserted span          */
        gtk_text_buffer_get_iter_at_offset(buffer, &span_s,
                                           end_off - (gint)n_chars);
        ed->internal_change++;
        for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
            if (ed->inline_flags & INLINE_TOGGLES[i].flag)
                gtk_text_buffer_apply_tag_by_name(
                    buffer, INLINE_TOGGLES[i].tag_name, &span_s, location);
            else
                gtk_text_buffer_remove_tag_by_name(
                    buffer, INLINE_TOGGLES[i].tag_name, &span_s, location);
        }
        ed->internal_change--;
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
            ed->internal_change++;
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
            ed->internal_change--;
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
                ed->internal_change++;
                gtk_text_buffer_apply_tag_by_name(
                    buffer, on_tag_name_for_flag(para), &ps_ls, &apply_end);
                ed->internal_change--;
                gtk_text_buffer_get_iter_at_offset(buffer, location, end_off);
            }
        }
    }

    /* --- job 1c: pasted text can carry #tag styling onto a code-block
     * line (same-buffer paste keeps tags), and typing inside a span kept
     * from before the no-tags-in-code-blocks rule extends it — text in a
     * code block is never a tag, so strip the styling.                      */
    if (editor_rederive(ed, end_off - (gint)n_chars, end_off, RD_CODE_TAG))
        ed->tags_modified = TRUE;

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
                ++ed->undo_sentences >= UNDO_MAX_SENTENCES) {
                cap_hit = TRUE;
                break;
            }
        }
        if (cap_hit)
            undo_commit_now(ed);     /* also resets the sentence counter    */
    }

    /* --- jobs 2 & 3: tag capture ---------------------------------------- */
    if (ed->tag_start == NULL) {
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
                ed->tag_start = gtk_text_buffer_create_mark(
                    buffer, NULL, &hash, TRUE /* left gravity */);
                /* One query per capture; every keystroke inside the
                 * capture filters this snapshot in memory instead of
                 * re-querying SQLite (a round trip on network DBs).       */
                ed->tag_choices = on_db_tag_list(ed->app->db);
                tag_popup_update(ed);
            }
        }
        return;
    }

    /* Capture active: whitespace ends the tag, word chars refresh the
     * popup, anything else (punctuation) also ends it.                     */
    if (n_chars == 1) {
        gunichar c = g_utf8_get_char(text);
        if (g_unichar_isspace(c)) {
            tag_capture_end(ed, TRUE);
        } else if (g_unichar_isalnum(c) || c == '_' || c == '-') {
            tag_popup_update(ed);
        } else {
            tag_capture_end(ed, TRUE);
        }
    } else {
        /* Multi-char insertion (paste) during capture: just end it.        */
        tag_capture_end(ed, TRUE);
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
    OnEditor *ed = user_data;        /* owning editor                       */

    /* Marks are pruned even for internal changes: an undo replaces the
     * whole buffer, and marks left over from that would all collapse to
     * offset 0 and then mis-hint the first item.                           */
    action_marks_prune(ed, start, end);

    if (ed->internal_change > 0 || ed->tags_modified)
        return;

    GtkTextTag *tag = lookup_tag(buffer, ON_TAGNAME_TAG);
    if (tag == NULL)
        return;

    if (gtk_text_iter_has_tag(start, tag) ||
        gtk_text_iter_has_tag(end, tag)) {
        ed->tags_modified = TRUE;    /* range starts or ends inside a tag   */
        return;
    }
    /* Any tag toggle strictly inside the range?                            */
    GtkTextIter it = *start;
    if (gtk_text_iter_forward_to_tag_toggle(&it, tag) &&
        gtk_text_iter_compare(&it, end) < 0)
        ed->tags_modified = TRUE;
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
    (void)buffer; (void)end;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0)
        return;

    /* start == end after the deletion: retag the collapse-point line.      */
    gint off = gtk_text_iter_get_offset(start);
    /* A merge can pull a line up to 0, so the title look is re-derived too. */
    editor_rederive(ed, off, off, RD_ACTION | RD_TITLE);
    ed->actions_clean = FALSE;    /* deletes can create/destroy '!' lines   */

    if (ed->tag_start == NULL)
        return;
    GtkTextIter s, e;                /* revalidated capture span            */
    if (!tag_capture_span(ed, &s, &e))
        tag_capture_end(ed, FALSE);  /* '#' itself was deleted              */
    else
        tag_popup_update(ed);
}

/* on_scroll_idle() — deferred caret-follow scroll (see on_buffer_changed). */
static gboolean
on_scroll_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->scroll_idle = 0;
    gtk_text_view_scroll_to_mark(
        ed->view, gtk_text_buffer_get_insert(ed->buffer),
        0.08, FALSE, 0.0, 0.0);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * on_buffer_changed() — any edit re-arms the autosave timer.
 * ------------------------------------------------------------------------- */
static void
on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    (void)buffer;
    OnEditor *ed = user_data;        /* owning editor                       */
    editor_queue_autosave(ed);
    /* Text edits can grow, shrink, split or remove code blocks.            */
    code_buttons_queue_rebuild(ed);

    /* Keep the caret comfortably above the window's bottom edge while
     * typing: within_margin makes the view scroll a little AHEAD of the
     * cursor instead of letting it ride the very last pixel row.  The
     * scroll is deferred to an idle — at "changed" time the text layout
     * has not revalidated yet, so an immediate scroll computes against
     * stale extents and does nothing at the end of the document.           */
    if (ed->internal_change == 0 && ed->scroll_idle == 0)
        ed->scroll_idle = g_idle_add(on_scroll_idle, ed);
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
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0)
        return;

    GtkTextIter cursor;              /* new cursor position                 */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));

    /* Close the capture if the cursor moved outside the typed tag.         */
    if (ed->tag_start != NULL) {
        GtkTextIter s, e;            /* capture span                        */
        if (!tag_capture_span(ed, &s, &e) ||
            gtk_text_iter_compare(&cursor, &s) < 0 ||
            gtk_text_iter_compare(&cursor, &e) > 0)
            tag_capture_end(ed, TRUE);
    }

    /* Adopt the style of the character to the left of the cursor — but
     * NOT when the move comes from typing: this notify fires inside the
     * insert-text class handler, before the after-handler has applied
     * ed->inline_flags to the new character, so probing it here would
     * clear a style armed with no selection (Ctrl/Cmd+B, then type).      */
    if (ed->typing_insert)
        return;
    GtkTextIter probe = cursor;      /* the char whose style we adopt       */
    if (gtk_text_iter_backward_char(&probe))
        ed->inline_flags =
            on_flags_at_iter(ed->buffer, &probe, ON_FMT_INLINE_MASK);
    else
        ed->inline_flags = 0;
    update_toggle_buttons(ed);
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
    OnEditor *ed = user_data;        /* owning editor                       */

    /* While the tag popup is visible it owns the navigation keys.          */
    if (ed->tag_start != NULL && ed->tag_popup != NULL &&
        gtk_widget_get_visible(ed->tag_popup)) {
        switch (keyval) {
        case GDK_KEY_Down:
            tag_popup_move_selection(ed, +1);
            return TRUE;
        case GDK_KEY_Up:
            tag_popup_move_selection(ed, -1);
            return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab: {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(
                GTK_LIST_BOX(ed->tag_listbox));
            if (row != NULL) {
                on_tag_row_activated(GTK_LIST_BOX(ed->tag_listbox),
                                     row, ed);
                return TRUE;
            }
            break;
        }
        case GDK_KEY_Escape:
            tag_capture_end(ed, FALSE);
            return TRUE;
        default:
            break;
        }
    } else if (ed->tag_start != NULL && keyval == GDK_KEY_Escape) {
        tag_capture_end(ed, FALSE);
        return TRUE;
    }

    /* Enter inside a list item: continue or end the list.                  */
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (ed->tag_start != NULL)
            tag_capture_end(ed, TRUE);
        if (handle_return_in_list(ed)) {
            undo_commit_now(ed);     /* linebreak: close the undo group
                                        (list Enter inserts internally, so
                                        the insert-text cap never sees it)  */
            return TRUE;
        }
    }

    return FALSE;
}

/* ---------------------------------------------------------------------------
 * on_new_note() — "win.new-note" (Primary+N): a new note in THIS note's
 * folder, opened in its own editor.  The library (if open) refreshes via
 * the full notify — the folder's count just grew.
 * ------------------------------------------------------------------------- */
static void
on_new_note(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    OnNoteMeta *meta = on_db_note_get(ed->app->db, ed->note_id);
    if (meta == NULL)
        return;
    gint64 id = on_db_note_create(ed->app->db, meta->folder_id);
    on_db_note_meta_free(meta);
    if (id != 0) {
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
        on_editor_window_open(ed->app, id);
    }
}

/* on_find() — "win.find" (Primary+F): jump into the in-note search box.   */
static void
on_find(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(ed->search_entry);
}

/* on_undo() / on_redo() — "win.undo" (Primary+Z) and "win.redo"
 * (Primary+Shift+Z, Primary+Y).                                             */
static void
on_undo(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    editor_undo(user_data);
}

static void
on_redo(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    editor_redo(user_data);
}

/* ===========================================================================
 * in-note search
 *
 * The toolbar's right-edge entry highlights every case-insensitive match
 * with the "on-search-hit" tag as you type; Enter (or the entry icon)
 * jumps to the next match, wrapping at the end.  Primary+F focuses the
 * entry; Escape returns focus to the text.
 * =========================================================================== */

/* editor_search_clear() — drop all match highlighting.                      */
static void
editor_search_clear(OnEditor *ed)
{
    GtkTextIter s, e;                /* whole-buffer bounds                 */
    gtk_text_buffer_get_bounds(ed->buffer, &s, &e);
    gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-search-hit",
                                       &s, &e);
}

/* ---------------------------------------------------------------------------
 * on_search_changed() — re-highlight every match of the current query.
 * ------------------------------------------------------------------------- */
static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    editor_search_clear(ed);

    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (query == NULL || *query == '\0')
        return;

    GtkTextIter it;                  /* scan position                       */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    GtkTextIter match_s, match_e;    /* bounds of one match                 */
    while (gtk_text_iter_forward_search(&it, query,
                                        GTK_TEXT_SEARCH_CASE_INSENSITIVE |
                                        GTK_TEXT_SEARCH_TEXT_ONLY,
                                        &match_s, &match_e, NULL)) {
        gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-search-hit",
                                          &match_s, &match_e);
        it = match_e;
    }
}

/* Flags shared by every in-note text search.                                */
#define EDITOR_SEARCH_FLAGS (GTK_TEXT_SEARCH_CASE_INSENSITIVE | \
                             GTK_TEXT_SEARCH_TEXT_ONLY)

/* ---------------------------------------------------------------------------
 * editor_search_move() — select and scroll to the next match after the
 * cursor (forward) or the previous one before it (backward), wrapping
 * around the buffer when none remains in that direction.
 * ------------------------------------------------------------------------- */
static void
editor_search_move(OnEditor *ed, gboolean forward)
{
    const gchar *query =
        gtk_editable_get_text(GTK_EDITABLE(ed->search_entry));
    if (query == NULL || *query == '\0')
        return;

    /* Start just past the current selection in the travel direction.       */
    GtkTextIter from;                /* search start                        */
    gtk_text_buffer_get_iter_at_mark(
        ed->buffer, &from,
        forward ? gtk_text_buffer_get_selection_bound(ed->buffer)
                : gtk_text_buffer_get_insert(ed->buffer));

    GtkTextIter match_s, match_e;    /* bounds of the found match           */
    gboolean found = forward
        ? gtk_text_iter_forward_search(&from, query, EDITOR_SEARCH_FLAGS,
                                       &match_s, &match_e, NULL)
        : gtk_text_iter_backward_search(&from, query, EDITOR_SEARCH_FLAGS,
                                        &match_s, &match_e, NULL);
    if (!found) {                    /* wrap around                         */
        if (forward)
            gtk_text_buffer_get_start_iter(ed->buffer, &from);
        else
            gtk_text_buffer_get_end_iter(ed->buffer, &from);
        found = forward
            ? gtk_text_iter_forward_search(&from, query,
                                           EDITOR_SEARCH_FLAGS,
                                           &match_s, &match_e, NULL)
            : gtk_text_iter_backward_search(&from, query,
                                            EDITOR_SEARCH_FLAGS,
                                            &match_s, &match_e, NULL);
    }
    if (!found)
        return;

    gtk_text_buffer_select_range(ed->buffer, &match_s, &match_e);
    gtk_text_view_scroll_to_iter(ed->view, &match_s, 0.1,
                                 FALSE, 0.0, 0.0);
}

/* on_search_next() / on_search_prev() — Enter in the entry and the two
 * arrow buttons (first argument ignored, so one handler serves both the
 * entry's "activate" and the buttons' "clicked").                           */
static void
on_search_next(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    editor_search_move((OnEditor *)user_data, TRUE);
}

static void
on_search_prev(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    editor_search_move((OnEditor *)user_data, FALSE);
}

/* on_search_stop() — Escape in the entry: back to the text view.            */
static void
on_search_stop(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * editor_apply_search_term() — put `term` in the in-note search box,
 * highlight every match and jump to the first.  Used when a note is opened
 * from the library search window so the searched-for text is highlighted at
 * once.  on_search_changed is called explicitly rather than waiting for the
 * GtkSearchEntry's debounced "search-changed".  NULL/empty is a no-op.
 * ------------------------------------------------------------------------- */
static void
editor_apply_search_term(OnEditor *ed, const gchar *term)
{
    if (term == NULL || *term == '\0')
        return;
    gtk_editable_set_text(GTK_EDITABLE(ed->search_entry), term);
    on_search_changed(GTK_SEARCH_ENTRY(ed->search_entry), ed);
    editor_search_move(ed, TRUE);
}

/* on_initial_search_idle() — apply ed->pending_search once the freshly
 * opened window is realized and allocated (so editor_search_move's
 * scroll-to-match lands, mirroring on_scroll_idle's deferral).               */
static gboolean
on_initial_search_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->initial_search_idle = 0;
    editor_apply_search_term(ed, ed->pending_search);
    g_clear_pointer(&ed->pending_search, g_free);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * editor_reveal_image() — put the caret at the note's `ord`-th embedded
 * image and scroll it into view, a third of the way down the window.  The
 * ordinal counts image anchors in buffer order, which is exactly the order
 * on_note_count_images()/on_note_image_nth() count IMAGE records in — that
 * is what lets the media window address a thumbnail's source by number.
 *   ed  — the editor to scroll.
 *   ord — 0-based image ordinal; < 0 is a no-op.
 * Returns TRUE when the image was found and revealed.
 * ------------------------------------------------------------------------- */
static gboolean
editor_reveal_image(OnEditor *ed, gint ord)
{
    gint offset;                     /* the image's buffer offset           */
    if (editor_image_nth(ed, ord, &offset) == NULL)
        return FALSE;

    GtkTextIter it;                  /* the image's position                */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, offset);
    /* Caret only — no selection: the user never asked for one, and the next
     * keystroke would replace the image.                                   */
    gtk_text_buffer_place_cursor(ed->buffer, &it);
    gtk_text_view_scroll_to_iter(ed->view, &it, 0.0, TRUE, 0.0, 0.3);
    return TRUE;
}

/* on_initial_image_idle() — reveal ed->pending_image once the freshly opened
 * window is realized and allocated, so the scroll lands (same deferral as
 * on_initial_search_idle).                                                   */
static gboolean
on_initial_image_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->initial_image_idle = 0;
    editor_reveal_image(ed, ed->pending_image);
    ed->pending_image = -1;
    return G_SOURCE_REMOVE;
}

/* ===========================================================================
 * saving
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_save() — serialize the buffer and persist it: content blob,
 * derived title, and — only when the live tags_modified flag says a #tag
 * was created, renamed or deleted — the note's tag set.  Then pokes the
 * library: the light notes-pane refresh normally (editing a note can
 * never change folder counts), the full one (sidebar included) only for
 * tag changes.
 * ------------------------------------------------------------------------- */
static void
editor_save(OnEditor *ed)
{
    gsize   blob_len = 0;            /* BNBF blob size                      */
    guint8 *blob = on_note_serialize(ed->buffer, &blob_len);
    gchar  *title = on_buffer_first_line(ed->buffer);

    /* ONE walk of the fresh blob for both derived values.  The action list
     * is only wanted when the set might have changed (see below); asking for
     * it here costs nothing extra, since the walk happens either way.       */
    gboolean want_actions = ed->last_actions != NULL || !ed->actions_clean;
    gchar  *body = NULL;             /* searchable plain text               */
    GList  *fresh_actions = NULL;    /* '!' lines, when wanted              */
    on_note_extract(blob, blob_len, &body,
                    want_actions ? &fresh_actions : NULL);

    on_db_note_save(ed->app->db, ed->note_id, title, blob, blob_len, body);
    g_free(body);

    /* Rewrite note_tags only when the tag set actually changed — flagged
     * live while editing, so the common save does no tag work at all.      */
    gboolean tags_changed = ed->tags_modified;
    if (tags_changed) {
        GList *tags = on_buffer_collect_tags(ed->buffer);
        on_db_note_set_tags(ed->app->db, ed->note_id, tags);
        g_list_free_full(tags, g_free);
        ed->tags_modified = FALSE;
    }

    /* Mirror the '!' action lines into action_items only when they changed
     * since the last sync (cheap blob walk, no images decoded).  Skip the
     * walk entirely when actions_clean is set AND last_actions is NULL —
     * no action line existed before and no insert/delete that could have
     * created one has occurred since the last extract.                      */
    gboolean actions_changed = FALSE;
    if (want_actions) {
        GList *actions = fresh_actions;   /* from the walk above            */
        actions_changed = !action_lists_equal(actions, ed->last_actions);
        if (actions_changed) {
            /* Hint from the marks (identifies reworded items), let the
             * rebuild assign the uids, then re-place the marks on the
             * lines as they now stand.                                     */
            action_marks_hint(ed, actions);
            on_db_note_set_actions(ed->app->db, ed->note_id, actions);
            action_marks_sync(ed, actions);
            on_db_action_list_free(ed->last_actions);
            ed->last_actions = actions;
        } else {
            on_db_action_list_free(actions);
        }
        ed->actions_clean = TRUE;
    }
    ed->dirty = FALSE;
    editor_status_dirty_update(ed);   /* dot turns green                     */

    /* Window title mirrors the note title.                                 */
    if (ed->window != NULL) {
        gchar *wtitle = g_strdup_printf("Notes - %s", title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    g_free(title);
    g_free(blob);

    /* Tag-set and action-set changes touch the sidebar (tag rows, the
     * Action Items section/count): full refresh.  Plain saves take the
     * light notes-pane-only path.                                          */
    if (tags_changed || actions_changed) {
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
    } else {
        if (ed->app->notify_note_saved != NULL)
            ed->app->notify_note_saved(ed->app, ed->note_id);
    }
}

/* on_autosave_timeout() — the debounce timer fired: save now.               */
static gboolean
on_autosave_timeout(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->autosave_source = 0;
    editor_save(ed);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * editor_queue_autosave() — (re)arm the autosave debounce timer.
 * ------------------------------------------------------------------------- */
static void
editor_queue_autosave(OnEditor *ed)
{
    if (!ed->dirty) {                /* transition only — this runs per
                                        keystroke, the label update doesn't  */
        ed->dirty = TRUE;            /* there is now something to save      */
        editor_status_dirty_update(ed);              /* dot turns red        */
    }
    undo_notify_change(ed);          /* every mutation passes through here,
                                        so this is also where undo groups
                                        start (no-op while restoring)       */
    if (ed->autosave_source != 0)
        g_source_remove(ed->autosave_source);
    ed->autosave_source = g_timeout_add(AUTOSAVE_DELAY_MS,
                                        on_autosave_timeout, ed);
}

/* ---------------------------------------------------------------------------
 * buffer_is_blank() — TRUE when the buffer holds no content: nothing but
 * whitespace and no embedded objects.  Anchors (images, tables, task
 * checkboxes) appear as U+FFFC in the slice, which is not whitespace,
 * so a note holding only an image or an unticked checkbox is NOT blank.
 * ------------------------------------------------------------------------- */
static gboolean
buffer_is_blank(GtkTextBuffer *buffer)
{
    if (gtk_text_buffer_get_char_count(buffer) == 0)
        return TRUE;

    GtkTextIter start, end;          /* whole-buffer bounds                 */
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *slice = gtk_text_buffer_get_slice(buffer, &start, &end, TRUE);
    gboolean blank = TRUE;           /* nothing but whitespace so far?      */
    for (const gchar *p = slice; *p != '\0'; p = g_utf8_next_char(p)) {
        if (!g_unichar_isspace(g_utf8_get_char(p))) {
            blank = FALSE;
            break;
        }
    }
    g_free(slice);
    return blank;
}

/* ---------------------------------------------------------------------------
 * on_editor_destroy() — the window is going away: flush a final save (or
 * delete the note outright when it was left with no content), drop the
 * editor from the open-editors table, and free everything.
 *
 * In GTK4 a window's "destroy" is emitted from GtkWidget's dispose, AFTER
 * GtkWindow's dispose has unparented the child — so by now the toolbar,
 * the overlay and the status labels are gone, and nothing here may touch
 * them: the label pointers are cleared first so the save's status update
 * sees nothing to paint.  The buffer and the view survive on the editor's
 * own references; the view is released LAST, so its dispose (which
 * unparents the #tag popover through `ed`) runs while `ed` is whole.
 * ------------------------------------------------------------------------- */
static void
on_editor_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnEditor *ed = user_data;        /* owning editor                       */

    ed->status_path    = NULL;       /* the labels died with the tree       */
    ed->status_note_id = NULL;
    ed->status_dirty   = NULL;

    if (ed->autosave_source != 0) {
        g_source_remove(ed->autosave_source);
        ed->autosave_source = 0;
    }
    if (ed->code_btn_idle != 0) {
        g_source_remove(ed->code_btn_idle);
        ed->code_btn_idle = 0;
    }
    if (ed->scroll_idle != 0) {
        g_source_remove(ed->scroll_idle);
        ed->scroll_idle = 0;
    }
    if (ed->initial_search_idle != 0) {
        g_source_remove(ed->initial_search_idle);
        ed->initial_search_idle = 0;
    }
    if (ed->initial_image_idle != 0) {
        g_source_remove(ed->initial_image_idle);
        ed->initial_image_idle = 0;
    }
    if (ed->undo_commit_source != 0) {
        g_source_remove(ed->undo_commit_source);
        ed->undo_commit_source = 0;
    }
    undo_free_history(ed);
    on_image_viewer_free(ed->img_viewer);
    ed->img_viewer = NULL;
    g_clear_pointer(&ed->pending_search, g_free);
    g_slist_free(ed->code_buttons);  /* widgets die with the window         */
    ed->code_buttons = NULL;

    ed->window = NULL;               /* don't touch the dying window        */
    if (buffer_is_blank(ed->buffer)) {
        /* A note closed with no content is discarded — permanently, an
         * empty note in the Trash would be clutter — so a Ctrl+N or
         * quicknote window closed without typing leaves nothing behind.
         * Deletion can change folder counts: full library notify.         */
        on_db_notes_delete(ed->app->db, &ed->note_id, 1);
        on_app_status(ed->app, "Deleted empty note");
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
    } else if (ed->dirty) {
        editor_save(ed);             /* flush edits the autosave missed
                                        (buffer kept alive by our ref);
                                        a clean note closes instantly —
                                        no serialize, no PNG encoding      */
    }

    on_db_tag_list_free(ed->tag_choices);   /* window closed mid-capture   */
    ed->tag_choices = NULL;

    g_hash_table_remove(ed->app->editors, &ed->note_id);
    on_db_action_list_free(ed->last_actions);
    action_marks_clear(ed);          /* marks die with the buffer anyway    */
    g_ptr_array_unref(ed->action_marks);
    ed->action_marks = NULL;
    g_object_unref(ed->buffer);
    g_object_unref(ed->view);        /* its dispose still reads ed          */
    ed->view = NULL;
    g_free(ed);
}

/* ===========================================================================
 * status bar
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_status_path_update() — put the note's folder path in the status
 * bar's left label, same format as the library window's: "/" for the top
 * level, "/Folder/Sub" otherwise.
 * ------------------------------------------------------------------------- */
static void
editor_status_path_update(OnEditor *ed)
{
    if (ed->status_path == NULL)
        return;

    OnNoteMeta *meta = on_db_note_get(ed->app->db, ed->note_id);
    if (meta == NULL)
        return;

    gchar *path = on_db_folder_path(ed->app->db, meta->folder_id);
    gchar *text = g_strdup_printf("/%s", path);
    gchar *full = on_app_location_text(ed->app, text);
    gtk_label_set_text(GTK_LABEL(ed->status_path), full);
    g_free(full);
    g_free(text);
    g_free(path);
    on_db_note_meta_free(meta);
}

/* ---------------------------------------------------------------------------
 * editor_status_note_id_update() — show the note's database id in the
 * status bar's right label ("id:N"), or hide the label entirely while
 * the statusbar_note_id setting is off (this function fully owns its
 * visibility; the label is built hidden).
 * ------------------------------------------------------------------------- */
static void
editor_status_note_id_update(OnEditor *ed)
{
    if (ed->status_note_id == NULL)
        return;
    if (ed->app->statusbar_note_id) {
        gchar *text = g_strdup_printf("id:%" G_GINT64_FORMAT, ed->note_id);
        gtk_label_set_text(GTK_LABEL(ed->status_note_id), text);
        g_free(text);
    }
    gtk_widget_set_visible(ed->status_note_id, ed->app->statusbar_note_id);
}

/* ---------------------------------------------------------------------------
 * editor_status_dirty_update() — paint the far-right save-state dot: red
 * while the buffer holds edits the database hasn't seen, green once the
 * autosave (or the close-time flush) has written them.  Called from the two
 * places that own ed->dirty, and at window build for the initial green.
 * ------------------------------------------------------------------------- */
static void
editor_status_dirty_update(OnEditor *ed)
{
    if (ed->status_dirty == NULL)
        return;
    gtk_label_set_text(GTK_LABEL(ed->status_dirty),
                       ed->dirty ? "\xF0\x9F\x94\xB4"   /* 🔴 unsaved edits  */
                                 : "\xF0\x9F\x9F\xA2"); /* 🟢 saved         */
    gtk_widget_set_tooltip_text(ed->status_dirty,
                                ed->dirty ? "Unsaved changes" : "Saved");
}

/* editor_status_update() — re-render all status-bar labels of one editor.   */
static void
editor_status_update(OnEditor *ed)
{
    editor_status_path_update(ed);
    editor_status_note_id_update(ed);
    editor_status_dirty_update(ed);
}

/* ---------------------------------------------------------------------------
 * on_editor_status_refresh_all() — re-render the status bar of every open
 * editor (Settings toggled the DB-path prefix or the note id live).
 * ------------------------------------------------------------------------- */
void
on_editor_status_refresh_all(OnApp *app)
{
    editors_foreach(app, editor_status_update);
}

/* on_editor_active_changed() — "notify::is-active": the window became the
 * active one again — re-read the note's location, so a move or folder
 * rename made in the library while this editor sat in the background
 * shows up on return.                                                       */
static void
on_editor_active_changed(GObject *window, GParamSpec *pspec,
                         gpointer user_data)
{
    (void)pspec;
    if (gtk_window_is_active(GTK_WINDOW(window)))
        editor_status_path_update(user_data);
}

/* ===========================================================================
 * toolbar + window construction
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * add_para_button() — helper: append a paragraph-style tool button.
 *   ed       — the editor.
 *   toolbar  — the toolbar box to append to.
 *   icon     — local icon file basename, or NULL.
 *   fallback — markup shown as the icon when the file is missing.
 *   label    — button text label.
 *   tooltip  — hover help.
 *   style    — the "win.para" target the button applies ("h1", "body", …).
 * ------------------------------------------------------------------------- */
static void
add_para_button(OnEditor *ed, GtkWidget *toolbar, const gchar *icon,
                const gchar *fallback, const gchar *label,
                const gchar *tooltip, const gchar *style)
{
    GtkWidget *item = on_app_tool_item_new(ed->app, FALSE, icon,
                                           fallback, label, tooltip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(item), "win.para");
    gtk_actionable_set_action_target(GTK_ACTIONABLE(item), "s", style);
    gtk_box_append(GTK_BOX(toolbar), item);
}

/* ---------------------------------------------------------------------------
 * menu_button_new() — helper: a glyph-faced, frameless GtkMenuButton over
 * a menu model.  `markup` is Pango markup rendered as the button face so
 * the compact menu buttons match the letter-glyph tool buttons.  The
 * button never takes the focus: the editing actions are enabled only
 * while the text view has it, and a click here must not disable the very
 * items it is opening.
 *   markup  — the button face.
 *   tooltip — hover help.
 *   model   — the items; OWNERSHIP IS TAKEN.
 * ------------------------------------------------------------------------- */
static GtkWidget *
menu_button_new(const gchar *markup, const gchar *tooltip,
                GMenuModel *model)
{
    GtkWidget *btn  = gtk_menu_button_new();
    GtkWidget *face = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(face), markup);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), face);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(btn), FALSE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn), model);
    g_object_unref(model);
    gtk_widget_set_tooltip_text(btn, tooltip);
    return btn;
}

/* ---------------------------------------------------------------------------
 * para_menu() — a compact-toolbar menu of paragraph styles: one item per
 * (label, "win.para" target) pair.
 *   labels / styles / n — parallel arrays.
 * Returns a new model (menu_button_new takes it).
 * ------------------------------------------------------------------------- */
static GMenuModel *
para_menu(const gchar *const *labels, const gchar *const *styles, gsize n)
{
    GMenu *menu = g_menu_new();
    for (gsize i = 0; i < n; i++) {
        GMenuItem *item = g_menu_item_new(labels[i], NULL);
        g_menu_item_set_action_and_target(item, "win.para", "s", styles[i]);
        g_menu_append_item(menu, item);
        g_object_unref(item);
    }
    return G_MENU_MODEL(menu);
}

/* search_button_new() — a frameless icon button beside the in-note search
 * entry, wired to one of the two match-stepping handlers.                  */
static GtkWidget *
search_button_new(const gchar *icon_name, const gchar *tooltip,
                  GCallback clicked, OnEditor *ed)
{
    GtkWidget *btn = gtk_button_new_from_icon_name(icon_name);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect(btn, "clicked", clicked, ed);
    return btn;
}

/* ---------------------------------------------------------------------------
 * build_toolbar() — construct the formatting toolbar: a horizontal GtkBox
 * with the "toolbar" style class holding the inline-style toggles, the
 * paragraph-style buttons, code-block and image insertion.  In compact
 * mode (File → Settings…) the three paragraph-style buttons collapse into
 * an "Aa" Styles menu button and the three list buttons into a "≡" Lists
 * one.  A box demands its full natural width, which becomes the window's
 * minimum — items can never be silently clipped by narrowing the window.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_toolbar(OnEditor *ed)
{
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(toolbar, "toolbar");

    /* Inline style toggles (B, I, U, S).                                   */
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkWidget *item = on_app_tool_item_new(
            ed->app, TRUE, INLINE_TOGGLES[i].icon, INLINE_TOGGLES[i].markup,
            INLINE_TOGGLES[i].label, INLINE_TOGGLES[i].tooltip);
        g_object_set_data(G_OBJECT(item), "on-flag",
                          GUINT_TO_POINTER(INLINE_TOGGLES[i].flag));
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_inline_toggle), ed);
        gtk_box_append(GTK_BOX(toolbar), item);
        ed->toggle_buttons[i] = item;
    }

    add_para_button(ed, toolbar, "code-block", "{\xc2\xa0}", "Code",
                    "Code block", "code");

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* Paragraph styles.  These have no standard icons, so their "icons"
     * are text glyphs (still swappable by dropping a matching PNG — e.g.
     * heading-1.png — into the icons/ folder).                             */
    if (ed->app->compact_editor_toolbar) {
        static const gchar *const LABELS[] =
            { "Heading _1", "Heading _2", "_Body" };
        static const gchar *const STYLES[] = { "h1", "h2", "body" };
        gtk_box_append(GTK_BOX(toolbar),
                       menu_button_new("<b>A</b>a",
                           "Styles \xe2\x80\x94 paragraph style: "
                           "heading or body text",
                           para_menu(LABELS, STYLES, 3)));
    } else {
        add_para_button(ed, toolbar, "heading-1", "<b>H1</b>", "Heading 1",
                        "Heading 1", "h1");
        add_para_button(ed, toolbar, "heading-2", "<b>H2</b>", "Heading 2",
                        "Heading 2", "h2");
        add_para_button(ed, toolbar, "body-text", "\xc2\xb6", "Body",
                        "Plain body text", "body");
    }

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    if (ed->app->compact_editor_toolbar) {
        static const gchar *const LABELS[] =
            { "_Bulleted List", "_Numbered List", "_Task List" };
        static const gchar *const STYLES[] = { "bullet", "number", "check" };
        gtk_box_append(GTK_BOX(toolbar),
                       menu_button_new("\xe2\x89\xa1",
                           "Lists \xe2\x80\x94 bullets, numbers, or "
                           "task checkboxes",
                           para_menu(LABELS, STYLES, 3)));
    } else {
        add_para_button(ed, toolbar, "list-bullet", "\xe2\x80\xa2",
                        "Bullets", "Bulleted list", "bullet");
        add_para_button(ed, toolbar, "list-number", "1.", "Numbered",
                        "Numbered list", "number");
        /* Fallback glyph is a plain text square (U+25A1 □), not the ⬜
         * color emoji: it renders in the text font like the •/1. glyphs
         * and avoids the emoji's oversized advance in the toolbar.        */
        add_para_button(ed, toolbar, "list-check", "\xe2\x96\xa1", "Tasks",
                        "Task list with checkboxes (click a box to toggle)",
                        "check");
    }

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* One "Insert ▾" dropdown replaces the Image/Table/Emoji buttons.  The
     * shortcuts (Primary+E, Primary+D) show on the items by themselves.  */
    GMenu *insert_menu = g_menu_new();
    g_menu_append(insert_menu, "_Image\xe2\x80\xa6", "win.insert-image");
    g_menu_append(insert_menu, "_Table",             "win.insert-table");
    g_menu_append(insert_menu, "_Emoji\xe2\x80\xa6", "win.insert-emoji");
    g_menu_append(insert_menu, "_Date",              "win.insert-date");

    gtk_box_append(GTK_BOX(toolbar),
                   menu_button_new("+",
                       "Insert an image, a table, an emoji, or "
                       "today's date at the cursor",
                       G_MENU_MODEL(insert_menu)));

    /* In-note search, pinned to the toolbar's right edge (Primary+F) by an
     * expanding blank spacer.                                              */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);

    ed->search_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(ed->search_entry),
                                          "Find in note");
    gtk_editable_set_width_chars(GTK_EDITABLE(ed->search_entry), 18);
    g_signal_connect(ed->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), ed);
    g_signal_connect(ed->search_entry, "activate",
                     G_CALLBACK(on_search_next), ed);
    g_signal_connect(ed->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), ed);

    /* Entry + previous/next match buttons as one group.                    */
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(search_box), ed->search_entry);
    gtk_box_append(GTK_BOX(search_box),
                   search_button_new("go-up-symbolic", "Previous match",
                                     G_CALLBACK(on_search_prev), ed));
    gtk_box_append(GTK_BOX(search_box),
                   search_button_new("go-down-symbolic",
                                     "Next match (Enter)",
                                     G_CALLBACK(on_search_next), ed));
    gtk_box_append(GTK_BOX(toolbar), search_box);

    return toolbar;
}

/* editor_rebuild_toolbar() — swap one editor's formatting toolbar for a
 * freshly built one, refreshing the ed-> widget pointers (toggle_buttons,
 * search_entry).                                                           */
static void
editor_rebuild_toolbar(OnEditor *ed)
{
    if (ed->toolbar_box == NULL)
        return;
    gchar *query =                   /* in-note search survives the rebuild */
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ed->search_entry)));
    gtk_box_remove(GTK_BOX(ed->toolbar_box), ed->toolbar);
    ed->toolbar = build_toolbar(ed);
    gtk_box_prepend(GTK_BOX(ed->toolbar_box), ed->toolbar);
    update_toggle_buttons(ed);       /* fresh toggles: mirror inline_flags  */
    if (*query != '\0')              /* re-runs the search + highlights     */
        gtk_editable_set_text(GTK_EDITABLE(ed->search_entry), query);
    g_free(query);
}
void
on_editor_rebuild_toolbars_all(OnApp *app)
{
    editors_foreach(app, editor_rebuild_toolbar);
}

/* ---------------------------------------------------------------------------
 * editor_build_view() — create ed->view and ed->buffer, add all editor-only
 * GtkTextTags, and configure the view's margins and wrapping.  Called before
 * loading content so the tags exist when the deserializer applies them.
 * ------------------------------------------------------------------------- */
static void
editor_build_view(OnEditor *ed)
{
    /* The editor holds its own reference to the view, so the view's dispose
     * — where the vfuncs' `ed` back-pointer is last read — runs from
     * on_editor_destroy while `ed` still exists, never from the window's
     * child teardown at a moment of GTK's choosing.                        */
    ed->view   = GTK_TEXT_VIEW(g_object_ref_sink(notes_text_view_new(ed)));
    ed->buffer = gtk_text_view_get_buffer(ed->view);
    g_object_ref(ed->buffer);        /* keep alive for the final save       */
    /* ONE undo: the editor's snapshot history (see the undo section), not
     * the buffer's text-only one — which would otherwise also record every
     * edit and answer the view's own Undo menu item and Ctrl+Z binding.   */
    gtk_text_buffer_set_enable_undo(ed->buffer, FALSE);
    on_buffer_ensure_tags(ed->buffer);

    /* Editor-only highlight for in-note search matches (never appears in
     * the serializer's flag table, so it is ignored on save).              */
    gtk_text_buffer_create_tag(ed->buffer, "on-search-hit",
                               "background", "#ffec8b", NULL);
    /* Editor-only padding around color-emoji glyphs (see is_emoji_char).   */
    gtk_text_buffer_create_tag(ed->buffer, "on-emoji",
                               "letter-spacing", 5 * PANGO_SCALE, NULL);
    /* Editor-only: drop each task checkbox a few px so its box centers on
     * the text beside it — anchored children sit with their bottom on the
     * baseline, which parks the box's center above the text's (see
     * attach_checkbox_widget).  Never serialized.                          */
    gtk_text_buffer_create_tag(ed->buffer, "on-check-drop",
                               "rise", -3 * PANGO_SCALE, NULL);
    /* Editor-only tint for '!' action lines — blue, mirroring the tags'
     * orange.  Priority 0 so a #tag span inside an action line keeps its
     * own color.  Never serialized.                                        */
    GtkTextTag *action_tag = gtk_text_buffer_create_tag(
        ed->buffer, "on-action",
        "foreground", "#1a5fb4",
        "weight",     PANGO_WEIGHT_SEMIBOLD,
        NULL);
    gtk_text_tag_set_priority(action_tag, 0);
    /* Editor-only centering for the note's title line — always line 0.
     * Nothing else sets justification, so priority doesn't matter here.
     * Never serialized: only the editor centers it (title_line_sync).   */
    gtk_text_buffer_create_tag(ed->buffer, "on-title-center",
                               "justification", GTK_JUSTIFY_CENTER,
                               NULL);
    /* ...and its heading look, same values as ON_TAGNAME_H1 but derived, so
     * a line becomes (or stops being) the title purely by moving to or off
     * line 0.  Skipped where a real paragraph style would multiply the
     * scale — see title_line_sync.                                         */
    gtk_text_buffer_create_tag(ed->buffer, "on-title-size",
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.6,
                               NULL);
    /* Caret sizing while the buffer is EMPTY, where no tag can apply: 160%
     * matches the scale above, so the caret is already the height of the
     * title the first keystroke produces.  title_line_sync owns the class. */
    on_app_widget_add_css(GTK_WIDGET(ed->view),
                          "textview.on-title-empty { font-size: 160%; }");

    gtk_text_view_set_editable(ed->view, TRUE);
    gtk_text_view_set_wrap_mode(ed->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(ed->view, 16);
    gtk_text_view_set_right_margin(ed->view, 16);
    gtk_text_view_set_top_margin(ed->view, 12);
    /* Roughly one text line of bottom margin: typing on the LAST line
     * (where there is no further content to scroll ahead to) still
     * leaves a line-sized gap below the caret.                             */
    gtk_text_view_set_bottom_margin(ed->view, 20);
    gtk_text_view_set_pixels_above_lines(ed->view, 2);
    if (ed->app->code_line_numbers)
        editor_apply_line_numbers(ed);
}

/* ---------------------------------------------------------------------------
 * editor_load_content() — load the stored BNBF blob into ed->buffer,
 * re-apply derived tags (emoji padding, action-line tint), and initialise
 * ed->last_actions and the undo baseline.  Signals must NOT be connected
 * yet — loading would otherwise trigger autosave.
 * ------------------------------------------------------------------------- */
static void
editor_load_content(OnEditor *ed)
{
    gsize   blob_len = 0;
    guint8 *blob = on_db_note_load(ed->app->db, ed->note_id, &blob_len);
    if (blob != NULL) {
        ed->internal_change++;
        on_note_deserialize(ed->buffer, blob, blob_len);
        editor_attach_image_widgets(ed);
        /* Re-apply derived tags — both are editor-only, never stored.      */
        tag_emoji_in_range(ed, 0,
                           gtk_text_buffer_get_char_count(ed->buffer));
        action_retag_lines(ed, 0,
                           gtk_text_buffer_get_char_count(ed->buffer));
        ed->internal_change--;
        /* Snapshot the action set; editor_save only rewrites when it drifts. */
        ed->last_actions = on_note_extract_actions(blob, blob_len);
        ed->actions_clean = TRUE;
        g_free(blob);

        /* Seed the identity marks from the stored rows, which are in the
         * same ord order as the lines just loaded — that is what lets a
         * reword in this session keep its uid.                             */
        GList *rows = on_db_action_list_for_note(ed->app->db, ed->note_id);
        action_marks_sync(ed, rows);
        on_db_action_list_free(rows);
    }
    /* Present line 0 as the title.  OUTSIDE the branch above: a note with
     * no blob is exactly the case whose caret needs the view-level
     * fallbacks, set before the window is ever shown.  Only the head of the
     * buffer needs the pass — freshly deserialized text can't be carrying
     * the editor-only tags.                                                */
    title_line_sync(ed, 0, 0);

    ed->undo_stack   = g_ptr_array_new();
    ed->redo_stack   = g_ptr_array_new();
    ed->undo_current = undo_snapshot_capture(ed);
}

/* ---------------------------------------------------------------------------
 * editor_build_layout() — assemble the toolbar, text-view scroll, and status
 * bar into a vertical box and set it as ed->window's child.
 * ------------------------------------------------------------------------- */
static void
editor_build_layout(OnEditor *ed)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ed->toolbar_box = vbox;
    ed->toolbar     = build_toolbar(ed);
    gtk_box_append(GTK_BOX(vbox), ed->toolbar);
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll),
                                              FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(ed->view));

    /* The modal image viewer stacks over the text, so the scroll goes in an
     * overlay.  Only the text area is covered: the toolbar and status bar
     * stay live, which is fine — the panel is about looking at a picture,
     * not about locking the window.                                        */
    ed->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(ed->overlay), scroll);
    gtk_widget_set_vexpand(ed->overlay, TRUE);
    gtk_box_append(GTK_BOX(vbox), ed->overlay);

    /* --- status bar: note location (left), note id, save-state dot ------ */
    ed->status_path = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(ed->status_path),
                            PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(ed->status_path, TRUE);
    on_app_widget_add_css(ed->status_path, "label { font-size: 85%; }");

    /* Note id — built hidden: its updater owns visibility
     * (statusbar_note_id setting, default off).                            */
    ed->status_note_id = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_note_id), 1.0);
    on_app_widget_add_css(ed->status_note_id, "label { font-size: 85%; }");
    gtk_widget_set_visible(ed->status_note_id, FALSE);

    /* Save-state dot — the very last thing on the bar, always shown.        */
    ed->status_dirty = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_dirty), 1.0);
    on_app_widget_add_css(ed->status_dirty, "label { font-size: 70%; }");

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 3);
    gtk_widget_set_margin_bottom(status_bar, 3);
    gtk_box_append(GTK_BOX(status_bar), ed->status_path);
    gtk_box_append(GTK_BOX(status_bar), ed->status_note_id);
    gtk_box_append(GTK_BOX(status_bar), ed->status_dirty);

    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(vbox), status_bar);
    editor_status_update(ed);

    gtk_window_set_child(GTK_WINDOW(ed->window), vbox);

    /* Built last, once the overlay can reach the toplevel: the panel installs
     * its styling for that window's display.                               */
    ed->img_viewer = on_image_viewer_new(
        ed->overlay, &editor_viewer_ops, ed, "Open in image viewer",
        "Open this image at full size in an external viewer");
}

/* ---------------------------------------------------------------------------
 * on_editor_window_key_pressed() — offer every key to the modal image
 * viewer first: Escape closes it, the arrows walk the note's images.  It
 * takes none while it is closed, so editing keeps all of its own keys.
 *
 * A key controller on the WINDOW in the CAPTURE phase: it runs before the
 * key reaches the focus widget, which is what lets the panel claim keys off
 * the text view.  The view KEEPS the focus while the panel is up
 * (deliberately — see image_viewer.c), so every key the panel does not want
 * is swallowed here as well: without that, typing at a picture would edit
 * the note blind behind it.
 * ------------------------------------------------------------------------- */
static gboolean
on_editor_window_key_pressed(GtkEventControllerKey *controller, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer user_data)
{
    (void)controller; (void)keycode;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (on_image_viewer_key_press(ed->img_viewer, keyval, state))
        return TRUE;
    return on_image_viewer_is_open(ed->img_viewer);   /* modal: eat the rest */
}

/* ===========================================================================
 * actions
 *
 * Every command the editor offers is a "win." GAction on its window; the
 * toolbar, the compact Styles/Lists/Insert menus, the image and table
 * context menus and every keyboard shortcut (on_app_install_accels) only
 * NAME them.  Nothing here is "app.": an editor's commands act on this
 * note, so they are only ever invoked from inside this window.
 *
 * The EDITING actions are enabled only while the note's text view has the
 * focus.  A window accelerator fires whatever widget has the focus, and the
 * in-note search entry and every table cell (its own GtkTextView) have
 * keys of their own; a disabled action is skipped by the accelerator
 * lookup, so Primary+B there propagates to the entry or cell as a key,
 * exactly as it did when the shortcuts were handled on the view itself.
 * =========================================================================== */

/* Action name → handler, parameter type, gated on the view's focus.        */
typedef struct {
    const gchar *name;
    void       (*activate)(GSimpleAction *, GVariant *, gpointer);
    const gchar *param_type;         /* GVariant type string, or NULL       */
    gboolean     editing;            /* enabled only while the view has
                                        the focus                           */
} EditorAction;

static const EditorAction EDITOR_ACTIONS[] = {
    { "new-note",      on_new_note,      NULL, FALSE },
    { "find",          on_find,          NULL, FALSE },
    { "undo",          on_undo,          NULL, TRUE  },
    { "redo",          on_redo,          NULL, TRUE  },
    { "inline",        on_inline,        "s",  TRUE  },
    { "para",          on_para,          "s",  TRUE  },
    { "insert-image",  on_insert_image,  NULL, TRUE  },
    { "insert-table",  on_insert_table,  NULL, TRUE  },
    { "insert-emoji",  on_insert_emoji,  NULL, TRUE  },
    { "insert-date",   on_insert_date,   NULL, TRUE  },
    /* context menus: they act on ed->ctx_offset, and a table's menu runs
     * with the focus in a CELL, so neither set is gated                    */
    { "img-copy",      on_img_copy,      NULL, FALSE },
    { "img-open",      on_img_open,      NULL, FALSE },
    { "img-full",      on_img_full,      NULL, FALSE },
    { "img-thumb",     on_img_thumb,     NULL, FALSE },
    { "table-row-add", on_table_command, NULL, FALSE },
    { "table-row-del", on_table_command, NULL, FALSE },
    { "table-col-add", on_table_command, NULL, FALSE },
    { "table-col-del", on_table_command, NULL, FALSE },
    { "table-delete",  on_table_command, NULL, FALSE },
};
/* ---------------------------------------------------------------------------
 * editor_actions_set_editing() — enable or disable the gated actions.
 * Tolerates a window that is gone or whose action map is already empty
 * (the focus leaves the view once more while the window is torn down).
 *   ed      — the editor.
 *   enabled — TRUE while the text view has the focus.
 * ------------------------------------------------------------------------- */
static void
editor_actions_set_editing(OnEditor *ed, gboolean enabled)
{
    if (ed->window == NULL)
        return;
    GActionMap *map = G_ACTION_MAP(ed->window);
    for (gsize i = 0; i < G_N_ELEMENTS(EDITOR_ACTIONS); i++) {
        if (!EDITOR_ACTIONS[i].editing)
            continue;
        GAction *action =            /* NULL once the map is torn down      */
            g_action_map_lookup_action(map, EDITOR_ACTIONS[i].name);
        if (action != NULL)
            g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
}

/* on_view_is_focus_changed() — the gate: "notify::is-focus" of the view's
 * focus controller.  is-focus, not enter/leave — those also fire for the
 * view's DESCENDANTS, and a table cell (its own GtkTextView) is exactly
 * where the gate must be closed (D11).                                      */
static void
on_view_is_focus_changed(GObject *controller, GParamSpec *pspec,
                         gpointer user_data)
{
    (void)pspec;
    editor_actions_set_editing(
        user_data,
        gtk_event_controller_focus_is_focus(
            GTK_EVENT_CONTROLLER_FOCUS(controller)));
}
/* ---------------------------------------------------------------------------
 * editor_install_actions() — add every action to the window: the table
 * above, plus the stateful "table-header" behind the table menu's check
 * item.  Must run before anything that names them is built (the toolbar),
 * so those items come up sensitive.
 * ------------------------------------------------------------------------- */
static void
editor_install_actions(OnEditor *ed)
{
    GActionMap *map = G_ACTION_MAP(ed->window);
    for (gsize i = 0; i < G_N_ELEMENTS(EDITOR_ACTIONS); i++) {
        const EditorAction *a = &EDITOR_ACTIONS[i];
        GSimpleAction *action = g_simple_action_new(
            a->name,
            a->param_type != NULL ? G_VARIANT_TYPE(a->param_type) : NULL);
        g_signal_connect(action, "activate", G_CALLBACK(a->activate), ed);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
    }

    GSimpleAction *header = g_simple_action_new_stateful(
        "table-header", NULL, g_variant_new_boolean(FALSE));
    g_signal_connect(header, "change-state",
                     G_CALLBACK(on_table_header_change_state), ed);
    g_action_map_add_action(map, G_ACTION(header));
    g_object_unref(header);

    /* Nothing has the focus until the window shows; the view takes it then
     * (on_editor_window_open grabs it after presenting) and the focus
     * controller opens the gate.  Until then the editing actions are off. */
    editor_actions_set_editing(ed, FALSE);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "notify::is-focus",
                     G_CALLBACK(on_view_is_focus_changed), ed);
    gtk_widget_add_controller(GTK_WIDGET(ed->view), focus);
}

/* ---------------------------------------------------------------------------
 * editor_connect_signals() — connect all buffer signals and install the
 * view's and the window's event controllers.  Called after content is
 * loaded so that loading never triggers autosave handlers.
 *
 * The view's key controller and click gesture run in the CAPTURE phase:
 * GtkTextView's own controllers are bubble-phase, and both of ours must
 * see the event first (Enter-in-list before the view inserts a newline, a
 * right press before the view builds its popup).  The window's key
 * controller is capture-phase too, so the modal image viewer is offered
 * every key before the focus widget.
 * ------------------------------------------------------------------------- */
static void
editor_connect_signals(OnEditor *ed)
{
    g_signal_connect(ed->buffer, "insert-text",
                     G_CALLBACK(on_buffer_insert_text_before), ed);
    g_signal_connect_after(ed->buffer, "insert-text",
                           G_CALLBACK(on_buffer_insert_text_after), ed);
    g_signal_connect(ed->buffer, "delete-range",
                     G_CALLBACK(on_buffer_delete_range_before), ed);
    g_signal_connect_after(ed->buffer, "delete-range",
                           G_CALLBACK(on_buffer_delete_range_after), ed);
    g_signal_connect(ed->buffer, "changed",
                     G_CALLBACK(on_buffer_changed), ed);
    g_signal_connect(ed->buffer, "notify::cursor-position",
                     G_CALLBACK(on_cursor_moved), ed);
    g_signal_connect(ed->view, "paste-clipboard",
                     G_CALLBACK(on_paste_clipboard), ed);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(on_view_key_pressed), ed);
    gtk_widget_add_controller(GTK_WIDGET(ed->view), keys);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_view_pressed), ed);
    gtk_widget_add_controller(GTK_WIDGET(ed->view),
                              GTK_EVENT_CONTROLLER(click));

    GtkEventController *win_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(win_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(win_keys, "key-pressed",
                     G_CALLBACK(on_editor_window_key_pressed), ed);
    gtk_widget_add_controller(ed->window, win_keys);

    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);
    g_signal_connect(ed->window, "notify::is-active",
                     G_CALLBACK(on_editor_active_changed), ed);

    /* Give existing code blocks their floating copy buttons.               */
    code_buttons_queue_rebuild(ed);
}

/* ---------------------------------------------------------------------------
 * editor_window_open_full() — shared implementation behind the three public
 * open functions.  Both extras are optional and mutually independent:
 *   search_term — pre-populates the in-note search box and jumps to the
 *                 first match, so a note opened from the library search
 *                 window lands with its hit highlighted (NULL for none).
 *   image_ord   — scrolls to that image of the note, so a note opened from
 *                 the media window lands on the thumbnail that was
 *                 double-clicked (-1 for none).
 * ------------------------------------------------------------------------- */
static GtkWidget *
editor_window_open_full(OnApp *app, gint64 note_id, const gchar *search_term,
                        gint image_ord)
{
    /* Already open?  Just raise the existing window (and re-run the search
     * or the scroll if one was requested — the note may already be up from a
     * prior open).  Its buffer is long since allocated, so the reveal needs
     * no idle deferral here.                                               */
    OnEditor *existing = editor_lookup(app, note_id);
    if (existing != NULL) {
        gtk_window_present(GTK_WINDOW(existing->window));
        if (search_term != NULL && *search_term != '\0')
            editor_apply_search_term(existing, search_term);
        if (image_ord >= 0)
            editor_reveal_image(existing, image_ord);
        return existing->window;
    }

    OnNoteMeta *meta = on_db_note_get(app->db, note_id);
    if (meta == NULL) {
        g_warning("editor: note %" G_GINT64_FORMAT " does not exist",
                  note_id);
        return NULL;
    }

    OnEditor *ed = g_new0(OnEditor, 1);
    ed->app     = app;
    ed->note_id = note_id;
    ed->pending_image = -1;          /* no image reveal unless asked for    */
    /* Identity marks for the '!' lines; seeded by editor_load_content.     */
    ed->action_marks = g_ptr_array_new_with_free_func(g_free);

    /* --- window: a plain GtkWindow, standard titlebar (no HeaderBar) ---- */
    /* A GtkApplicationWindow, for the "win." action group the toolbar,
     * menus and shortcuts name; it adds itself to the application.  The
     * application menubar is NOT rendered in-window here — on a desktop
     * with no shell menubar that would put File/View atop every editor. */
    ed->window = gtk_application_window_new(app->gtk_app);
    gtk_application_window_set_show_menubar(
        GTK_APPLICATION_WINDOW(ed->window), FALSE);

    /* Open at the configured default size (editor_win_w/editor_win_h in
     * the ini, fixed — unlike the search window's, these are not written
     * back on resize).                                                     */
    gint win_w = EDITOR_WIN_DEFAULT_W;
    gint win_h = EDITOR_WIN_DEFAULT_H;
    on_app_config_get_size("editor_win_w", "editor_win_h", &win_w, &win_h);
    gtk_window_set_default_size(GTK_WINDOW(ed->window), win_w, win_h);
    {
        gchar *wtitle = g_strdup_printf("Notes - %s", meta->title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    editor_build_view(ed);
    editor_install_actions(ed);      /* before the toolbar names them       */
    editor_load_content(ed);
    editor_build_layout(ed);
    editor_connect_signals(ed);

    /* Register in the open-editors table (key freed by the table), and
     * stash the editor state on its window for cross-module access.        */
    gint64 *key = g_new(gint64, 1);
    *key = note_id;
    g_hash_table_insert(app->editors, key, ed->window);
    g_object_set_data(G_OBJECT(ed->window), "on-editor", ed);

    /* Opened from search: apply the query once the window is realized and
     * allocated so the scroll-to-match lands (see on_scroll_idle).          */
    if (search_term != NULL && *search_term != '\0') {
        ed->pending_search = g_strdup(search_term);
        ed->initial_search_idle = g_idle_add(on_initial_search_idle, ed);
    }

    /* Opened from the media window: same deferral, so the scroll to the
     * image lands on an allocated view.                                    */
    if (image_ord >= 0) {
        ed->pending_image = image_ord;
        ed->initial_image_idle = g_idle_add(on_initial_image_idle, ed);
    }

    on_db_note_meta_free(meta);
    gtk_window_present(GTK_WINDOW(ed->window));
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    return ed->window;
}

GtkWidget *
on_editor_window_open(OnApp *app, gint64 note_id)
{
    return editor_window_open_full(app, note_id, NULL, -1);
}

GtkWidget *
on_editor_window_open_search(OnApp *app, gint64 note_id,
                             const gchar *search_term)
{
    return editor_window_open_full(app, note_id, search_term, -1);
}

GtkWidget *
on_editor_window_open_image(OnApp *app, gint64 note_id, gint image_ord)
{
    return editor_window_open_full(app, note_id, NULL, image_ord);
}
