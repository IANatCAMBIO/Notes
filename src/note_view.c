/* ===========================================================================
 * note_view.c — the rich-text note engine (implementation)
 *
 * See note_view.h for the feature overview and BLOCK_MODEL.md for the
 * design.  The view owns three things: the DOCUMENT (every edit is an
 * operation on it), the LAYOUT (doc_layout.[ch], told what changed by the
 * document's observer), and the INPUT state — caret, selection anchor,
 * the armed inline flags, the input method's preedit, the #tag capture.
 * Drawing is one snapshot vfunc over the layout; there are no child
 * widgets except two popovers (the #tag suggestions and GTK's emoji
 * chooser), which are parented to the view and never take its focus.
 *
 * Every mutation ends in after_edit(): the layout is already current (the
 * observer ran inside the operation), so it queues a resize, scrolls the
 * caret into view, keeps the input method informed and emits "edited" —
 * the host's cue to autosave.
 * =========================================================================== */

#include "note_view.h"
#include "doc_layout.h"
#include "serialize.h"               /* on_image_png_bytes, extractors      */

#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <unistd.h>

/* Undo grouping: typing that pauses this long is one step, and so is a
 * burst of this many sentence enders.                                      */
#define UNDO_GROUP_MS      1000
#define UNDO_MAX_SENTENCES 5


/* Maximum number of suggestions shown in the tag popup.                    */
#define TAG_POPUP_MAX 8

/* ---------------------------------------------------------------------------
 * OnNoteView
 *
 * Fields:
 *   app           — application context (not owned).
 *   doc           — the note (owned).
 *   layout        — its geometry (owned).
 *   hadj/vadj, hpolicy/vpolicy — the GtkScrollable contract; only vadj
 *                   is ever driven.
 *   caret, anchor — the selection is [anchor, caret) in either order;
 *                   equal means none.
 *   goal_x        — the x a run of Up/Down aims at (-1 = take the caret's).
 *   inline_flags  — ON_FMT_INLINE_MASK bits typed text will carry.
 *   typing        — an undo group is open for a burst of typing.
 *   typing_timer  — closes it after UNDO_GROUP_MS of quiet.
 *   sentences     — sentence enders typed into the open group.
 *   im            — the input method; preedit_* mirror its state.
 *   blink_timer, caret_on, blink_since — the caret's blink, paced by
 *                        the gtk-cursor-blink* settings (see blink_restart).
 *   drag_*        — a selection drag in progress (from the drag gesture).
 *   tag_capturing, tag_start, tag_popup, tag_listbox, tag_choices — the
 *                   '#' capture (see the tag section).
 *   emoji_chooser — GTK's, built on first use.
 *   ctx           — what the last context-menu press landed on: the
 *                   "view.img-*" / "view.table-*" actions act on it.
 *   hover_kind    — what the pointer is over, for the cursor.
 *   actions       — the "view." group (owned).
 *   fresh         — no save since the load: take_actions_modified reads
 *                   TRUE once.
 *   find_text     — the in-note find's current needle (owned).
 * ------------------------------------------------------------------------- */
struct _OnNoteView {
    GtkWidget           parent_instance;

    OnApp              *app;
    OnDocument         *doc;
    OnDocLayout        *layout;

    GtkAdjustment      *hadj, *vadj;
    GtkScrollablePolicy hpolicy, vpolicy;

    OnPos               caret, anchor;
    gint                goal_x;
    guint32             inline_flags;

    gboolean            typing;
    guint               typing_timer;
    gint                sentences;

    GtkIMContext       *im;
    gchar              *preedit;
    gboolean            im_focused;

    guint               blink_timer;
    gboolean            caret_on;
    gboolean            blinking;    /* focused: the caret should blink     */
    gint64              blink_since; /* monotonic µs of the last activity   */

    gboolean            dragging;
    OnPos               drag_anchor;

    gboolean            tag_capturing;
    OnPos               tag_start;
    GtkWidget          *tag_popup;
    GtkWidget          *tag_listbox;
    GList              *tag_choices;

    GtkWidget          *emoji_chooser;

    OnDocHitResult      ctx;
    OnHitKind           hover_kind;
    gboolean            hover_set;

    GSimpleActionGroup *actions;
    gboolean            fresh;
    gchar              *find_text;
    gboolean            scroll_pending;
};

static void note_view_scrollable_init(GtkScrollableInterface *iface);
static void note_view_accessible_text_init(GtkAccessibleTextInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    OnNoteView, on_note_view, GTK_TYPE_WIDGET,
    G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, note_view_scrollable_init)
    G_IMPLEMENT_INTERFACE(GTK_TYPE_ACCESSIBLE_TEXT,
                          note_view_accessible_text_init))

enum {
    PROP_0,
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY,
};

enum {
    SIG_EDITED,
    SIG_INLINE_FLAGS_CHANGED,
    SIG_IMAGE_ACTIVATED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

static void tag_capture_end(OnNoteView *v, gboolean apply);
static void tag_popup_update(OnNoteView *v);
static void typing_end(OnNoteView *v);
static void after_edit(OnNoteView *v);
static void scroll_to_caret(OnNoteView *v);
static void im_sync_location(OnNoteView *v);
static GArray *find_hits(OnNoteView *v, const gchar *needle);
static void blink_restart(OnNoteView *v);
static void im_reset(OnNoteView *v);

/* ===========================================================================
 * small helpers
 * ======================================================================== */

/* pos_eq() — two positions the same.                                        */
static gboolean
pos_eq(OnPos a, OnPos b)
{
    return on_pos_cmp(a, b) == 0;
}

/* has_selection()                                                           */
static gboolean
has_selection(OnNoteView *v)
{
    return !pos_eq(v->caret, v->anchor);
}

/* sel_bounds() — the selection in order.                                    */
static void
sel_bounds(OnNoteView *v, OnPos *a, OnPos *b)
{
    if (on_pos_cmp(v->anchor, v->caret) <= 0) {
        *a = v->anchor;
        *b = v->caret;
    } else {
        *a = v->caret;
        *b = v->anchor;
    }
}

/* clamp_pos() — a valid position nearest to `p` in the current document.   */
static OnPos
clamp_pos(OnNoteView *v, OnPos p)
{
    guint n = on_document_n_blocks(v->doc);
    if (p.block >= n) {
        p.block = n - 1;
        p.cell = -1;
        p.offset = G_MAXSIZE;
    }
    OnBlock *b = on_document_block(v->doc, p.block);
    if (b->kind == ON_BLOCK_IMAGE) {
        p.cell = -1;
        p.offset = MIN(p.offset, 1);
        return p;
    }
    if (b->kind == ON_BLOCK_TABLE) {
        if (p.cell < 0) {
            p.offset = MIN(p.offset, 1);
            return p;
        }
        if (p.cell >= b->rows * b->cols)
            p.cell = b->rows * b->cols - 1;
    } else {
        p.cell = -1;
    }
    OnText *t = on_document_text_at(v->doc, p);
    if (t == NULL) {
        p.cell = -1;
        p.offset = 0;
        return p;
    }
    if (p.offset > t->text->len)
        p.offset = t->text->len;
    while (p.offset > 0 && p.offset < t->text->len &&
           ((guchar)t->text->str[p.offset] & 0xc0) == 0x80)
        p.offset--;
    return p;
}

/* set_caret() — move the caret (and, unless extending, the anchor).         */
static void
set_caret(OnNoteView *v, OnPos p, gboolean extend)
{
    v->caret = clamp_pos(v, p);
    if (!extend)
        v->anchor = v->caret;
    blink_restart(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
    scroll_to_caret(v);
    im_sync_location(v);
    gtk_accessible_text_update_caret_position(GTK_ACCESSIBLE_TEXT(v));
    gtk_accessible_text_update_selection_bound(GTK_ACCESSIBLE_TEXT(v));
}

/* caret_text() — the OnText the caret is in, or NULL on an object.          */
static OnText *
caret_text(OnNoteView *v)
{
    return on_document_text_at(v->doc, v->caret);
}

/* caret_block() — the block the caret is in.                                */
static OnBlock *
caret_block(OnNoteView *v)
{
    return on_document_block(v->doc, v->caret.block);
}

/* caret_ensure_text() — make the caret sit in TEXT: when it is beside an
 * object (an image or a table as a whole — NOT inside a cell, which is
 * text: on_document_text_at answers for the cell), open a new paragraph
 * after (or before) the object and move the caret into it.  THE one
 * place typing and pasting share; testing the caret's BLOCK for text
 * instead sent a paste aimed at a cell into a paragraph outside the
 * table.                                                                 */
static void
caret_ensure_text(OnNoteView *v)
{
    if (caret_text(v) != NULL)
        return;
    guint at = v->caret.block + (v->caret.offset > 0 ? 1 : 0);
    on_document_insert_block(v->doc, at, on_block_new_text(ON_BLOCK_PARA));
    OnPos p = { at, -1, 0 };
    v->caret = v->anchor = p;
}

/* style_from_app() — the layout style the settings ask for.                 */
static void
style_from_app(OnNoteView *v)
{
    OnDocLayoutStyle st = {
        .title_line   = v->app->first_line_title,
        .code_numbers = v->app->code_line_numbers,
        .code_copy    = v->app->code_copy_buttons,
    };
    on_doc_layout_set_style(v->layout, &st);
}

/* ===========================================================================
 * the document observer → the layout
 * ======================================================================== */

static void
obs_block_changed(OnDocument *d, guint i, gpointer data)
{
    (void)d;
    OnNoteView *v = data;
    on_doc_layout_block_changed(v->layout, i);
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

static void
obs_blocks_inserted(OnDocument *d, guint i, guint n, gpointer data)
{
    (void)d;
    OnNoteView *v = data;
    on_doc_layout_blocks_inserted(v->layout, i, n);
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

static void
obs_blocks_removed(OnDocument *d, guint i, guint n, gpointer data)
{
    (void)d;
    OnNoteView *v = data;
    on_doc_layout_blocks_removed(v->layout, i, n);
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

static const OnDocumentObserver OBSERVER = {
    obs_block_changed, obs_blocks_inserted, obs_blocks_removed,
};

/* ===========================================================================
 * undo grouping and the edit epilogue
 * ======================================================================== */

/* on_typing_timeout() — the pause that closes a typing group.               */
static gboolean
on_typing_timeout(gpointer data)
{
    OnNoteView *v = data;
    v->typing_timer = 0;
    typing_end(v);
    return G_SOURCE_REMOVE;
}

/* typing_begin() — open (or extend) the typing undo group.                  */
static void
typing_begin(OnNoteView *v)
{
    if (!v->typing) {
        on_document_begin_group(v->doc);
        v->typing = TRUE;
        v->sentences = 0;
    }
    if (v->typing_timer != 0)
        g_source_remove(v->typing_timer);
    v->typing_timer = g_timeout_add(UNDO_GROUP_MS, on_typing_timeout, v);
}

/* typing_end() — close the typing group, if one is open.                    */
static void
typing_end(OnNoteView *v)
{
    if (v->typing_timer != 0) {
        g_source_remove(v->typing_timer);
        v->typing_timer = 0;
    }
    if (v->typing) {
        on_document_end_group(v->doc);
        v->typing = FALSE;
    }
}

/* after_edit() — the epilogue of every mutation.                            */
static void
after_edit(OnNoteView *v)
{
    v->caret = clamp_pos(v, v->caret);
    v->anchor = clamp_pos(v, v->anchor);
    blink_restart(v);
    if (v->find_text != NULL) {
        /* The highlights are byte ranges: an edit before a hit would
         * leave its yellow on the wrong characters.  The layout re-lays
         * out only the blocks whose hits actually changed.               */
        GArray *hits = find_hits(v, v->find_text);
        on_doc_layout_set_hits(v->layout, hits);
        g_array_unref(hits);
    }
    gtk_widget_queue_resize(GTK_WIDGET(v));
    v->scroll_pending = TRUE;        /* after the new size is known         */
    im_sync_location(v);
    g_signal_emit(v, signals[SIG_EDITED], 0);
}

/* delete_selection() — remove the selected range; TRUE when there was one. */
static gboolean
delete_selection(OnNoteView *v)
{
    if (!has_selection(v))
        return FALSE;
    OnPos a, b, out;
    sel_bounds(v, &a, &b);
    if (!on_document_delete_range(v->doc, a, b, &out))
        return FALSE;
    v->caret = v->anchor = clamp_pos(v, out);
    return TRUE;
}

/* ===========================================================================
 * scrolling (GtkScrollable)
 * ======================================================================== */

static void
on_vadj_changed(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void
set_adjustment(OnNoteView *v, GtkAdjustment **slot, GtkAdjustment *adj)
{
    if (adj == NULL)
        adj = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    if (*slot == adj)
        return;
    if (*slot != NULL) {
        g_signal_handlers_disconnect_by_func(*slot, on_vadj_changed, v);
        g_object_unref(*slot);
    }
    *slot = g_object_ref_sink(adj);
    g_signal_connect(adj, "value-changed", G_CALLBACK(on_vadj_changed), v);
    gtk_widget_queue_allocate(GTK_WIDGET(v));
}

static void
note_view_set_property(GObject *object, guint prop_id, const GValue *value,
                       GParamSpec *pspec)
{
    OnNoteView *v = ON_NOTE_VIEW(object);
    switch (prop_id) {
    case PROP_HADJUSTMENT:
        set_adjustment(v, &v->hadj, g_value_get_object(value));
        break;
    case PROP_VADJUSTMENT:
        set_adjustment(v, &v->vadj, g_value_get_object(value));
        break;
    case PROP_HSCROLL_POLICY:
        v->hpolicy = g_value_get_enum(value);
        break;
    case PROP_VSCROLL_POLICY:
        v->vpolicy = g_value_get_enum(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
note_view_get_property(GObject *object, guint prop_id, GValue *value,
                       GParamSpec *pspec)
{
    OnNoteView *v = ON_NOTE_VIEW(object);
    switch (prop_id) {
    case PROP_HADJUSTMENT:    g_value_set_object(value, v->hadj); break;
    case PROP_VADJUSTMENT:    g_value_set_object(value, v->vadj); break;
    case PROP_HSCROLL_POLICY: g_value_set_enum(value, v->hpolicy); break;
    case PROP_VSCROLL_POLICY: g_value_set_enum(value, v->vpolicy); break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
note_view_scrollable_init(GtkScrollableInterface *iface)
{
    (void)iface;                     /* the properties are the contract     */
}

/* scroll_y() — the current vertical scroll offset.                          */
static gdouble
scroll_y(OnNoteView *v)
{
    return (v->vadj != NULL) ? gtk_adjustment_get_value(v->vadj) : 0;
}

/* scroll_to_caret() — bring the caret into the viewport.                    */
static void
scroll_to_caret(OnNoteView *v)
{
    if (v->vadj == NULL)
        return;
    graphene_rect_t r;
    on_doc_layout_caret_rect(v->layout, v->caret, &r);
    gdouble page  = gtk_adjustment_get_page_size(v->vadj);
    gdouble value = gtk_adjustment_get_value(v->vadj);
    if (page <= 0) {
        v->scroll_pending = TRUE;    /* not allocated yet: after it is      */
        return;
    }
    gdouble top = r.origin.y - 8, bottom = r.origin.y + r.size.height + 8;
    if (top < value)
        gtk_adjustment_set_value(v->vadj, top);
    else if (bottom > value + page)
        gtk_adjustment_set_value(v->vadj, bottom - page);
}

/* ===========================================================================
 * the widget vfuncs
 * ======================================================================== */

static void
note_view_measure(GtkWidget *widget, GtkOrientation orientation, gint for_size,
                  gint *minimum, gint *natural, gint *minimum_baseline,
                  gint *natural_baseline)
{
    OnNoteView *v = ON_NOTE_VIEW(widget);
    (void)for_size;
    *minimum_baseline = *natural_baseline = -1;
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = 120;
        *natural = 400;
    } else {
        *minimum = 0;
        *natural = on_doc_layout_height(v->layout);
    }
}

static void
note_view_size_allocate(GtkWidget *widget, gint width, gint height,
                        gint baseline)
{
    OnNoteView *v = ON_NOTE_VIEW(widget);
    (void)baseline;
    on_doc_layout_set_width(v->layout, width);
    gint total = on_doc_layout_height(v->layout);
    if (v->vadj != NULL) {
        gdouble value = MIN(gtk_adjustment_get_value(v->vadj),
                            MAX(0, total - height));
        gtk_adjustment_configure(v->vadj, value, 0, MAX(total, height),
                                 20, height * 0.9, height);
    }
    if (v->hadj != NULL)
        gtk_adjustment_configure(v->hadj, 0, 0, width, 0, 0, width);
    if (v->tag_popup != NULL)
        gtk_popover_present(GTK_POPOVER(v->tag_popup));
    if (v->emoji_chooser != NULL)
        gtk_popover_present(GTK_POPOVER(v->emoji_chooser));
    if (v->scroll_pending) {
        v->scroll_pending = FALSE;
        scroll_to_caret(v);
    }
}

/* note_view_realize() / unrealize() — the input method learns its client
 * widget HERE, not at construction: the quartz method resolves the
 * widget's root surface when told (gtkimcontextquartz.c
 * quartz_set_client_surface) and a view that is not in a window yet has
 * none — every key would then be refused for the widget's whole life.
 * GtkTextView does the same.                                                */
static void
note_view_realize(GtkWidget *widget)
{
    OnNoteView *v = ON_NOTE_VIEW(widget);
    GTK_WIDGET_CLASS(on_note_view_parent_class)->realize(widget);
    gtk_im_context_set_client_widget(v->im, widget);
    if (gtk_widget_has_focus(widget) && !v->im_focused) {
        gtk_im_context_focus_in(v->im);
        v->im_focused = TRUE;
    }
}

static void
note_view_unrealize(GtkWidget *widget)
{
    OnNoteView *v = ON_NOTE_VIEW(widget);
    gtk_im_context_set_client_widget(v->im, NULL);
    GTK_WIDGET_CLASS(on_note_view_parent_class)->unrealize(widget);
}

static void
note_view_snapshot(GtkWidget *widget, GtkSnapshot *snap)
{
    OnNoteView *v = ON_NOTE_VIEW(widget);
    gint w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    static const GdkRGBA white = { 1, 1, 1, 1 };
    gtk_snapshot_append_color(snap, &white, &GRAPHENE_RECT_INIT(0, 0, w, h));

    gdouble sy = scroll_y(v);
    OnDocPaint p;
    p.clip = GRAPHENE_RECT_INIT(0, sy, w, h);
    p.has_sel = has_selection(v);
    sel_bounds(v, &p.sel_a, &p.sel_b);
    p.caret = v->caret;
    p.caret_visible = gtk_widget_has_focus(widget) && v->caret_on &&
                      !p.has_sel;
    p.focused = gtk_widget_has_focus(widget);
    /* Our own ink, whatever the window's state: the theme's backdrop
     * dimming is exactly what quirk #23 was about.                        */
    static const GdkRGBA ink = { 0.10, 0.10, 0.10, 1 };
    p.fg = ink;

    gtk_snapshot_save(snap);
    gtk_snapshot_translate(snap, &GRAPHENE_POINT_INIT(0, -sy));
    on_doc_layout_snapshot(v->layout, snap, &p);
    gtk_snapshot_restore(snap);

    /* The popovers.                                                        */
    GtkWidget *child = gtk_widget_get_first_child(widget);
    for (; child != NULL; child = gtk_widget_get_next_sibling(child))
        gtk_widget_snapshot_child(widget, child, snap);
}

/* ===========================================================================
 * caret blink and focus
 * ======================================================================== */

/* The blink follows the desktop's own settings, as GtkTextView's does:
 * "gtk-cursor-blink" (off = a steady caret), "gtk-cursor-blink-time" (one
 * full cycle, the caret on for two thirds of it) and
 * "gtk-cursor-blink-timeout" (seconds of no activity after which the caret
 * stays on and the timer STOPS — on an idle window there is nothing left
 * that redraws the widget twice a second).  Every caret move and edit is
 * activity: it restarts the cycle with the caret on, so a caret never
 * vanishes under a keystroke.                                             */
#define BLINK_ON_DIV 3               /* on for 2/3, off for 1/3 of a cycle  */

static gboolean on_blink(gpointer data);

/* blink_arm() — schedule the next toggle, on-phase or off-phase length.   */
static void
blink_arm(OnNoteView *v)
{
    gint time_ms = 1200;             /* the settings' documented defaults   */
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(v)),
                 "gtk-cursor-blink-time", &time_ms, NULL);
    gint phase = v->caret_on ? time_ms * 2 / BLINK_ON_DIV
                             : time_ms / BLINK_ON_DIV;
    v->blink_timer = g_timeout_add(MAX(phase, 1), on_blink, v);
}

static gboolean
on_blink(gpointer data)
{
    OnNoteView *v = data;
    gint timeout_s = 10;             /* gtk-cursor-blink-timeout default    */
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(v)),
                 "gtk-cursor-blink-timeout", &timeout_s, NULL);
    if (g_get_monotonic_time() - v->blink_since >
        (gint64)timeout_s * G_USEC_PER_SEC) {
        v->caret_on = TRUE;          /* idle: steady caret, no more timer   */
        v->blink_timer = 0;
        gtk_widget_queue_draw(GTK_WIDGET(v));
        return G_SOURCE_REMOVE;
    }
    v->caret_on = !v->caret_on;
    gtk_widget_queue_draw(GTK_WIDGET(v));
    blink_arm(v);
    return G_SOURCE_REMOVE;
}

/* blink_restart() — activity: the caret on, the cycle started afresh (when
 * the view is focused and the setting allows blinking at all).            */
static void
blink_restart(OnNoteView *v)
{
    if (v->blink_timer != 0) {
        g_source_remove(v->blink_timer);
        v->blink_timer = 0;
    }
    v->caret_on = TRUE;
    v->blink_since = g_get_monotonic_time();
    gboolean blink = TRUE;           /* gtk-cursor-blink                    */
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(v)),
                 "gtk-cursor-blink", &blink, NULL);
    if (v->blinking && blink)
        blink_arm(v);
}

/* blink_set() — focus in (TRUE) starts the cycle; focus out stops it.      */
static void
blink_set(OnNoteView *v, gboolean on)
{
    v->blinking = on;
    blink_restart(v);
}

static void
on_focus_enter(GtkEventControllerFocus *c, gpointer data)
{
    (void)c;
    OnNoteView *v = data;
    blink_set(v, TRUE);
    gtk_im_context_focus_in(v->im);
    v->im_focused = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

static void
on_focus_leave(GtkEventControllerFocus *c, gpointer data)
{
    (void)c;
    OnNoteView *v = data;
    blink_set(v, FALSE);
    if (v->im_focused) {
        im_reset(v);
        gtk_im_context_focus_out(v->im);
        v->im_focused = FALSE;
    }
    if (v->tag_capturing)
        tag_capture_end(v, TRUE);
    typing_end(v);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* ===========================================================================
 * editing primitives
 * ======================================================================== */

/* insert_typed() — `text` at the caret as typed text: replaces a selection,
 * carries the armed inline flags, extends the typing group.  Newlines are
 * never typed (Enter is handled apart), so the text is one line.           */
static void
insert_typed(OnNoteView *v, const gchar *text, gsize n)
{
    if (n == 0)
        return;
    typing_begin(v);
    delete_selection(v);
    caret_ensure_text(v);
    OnPos p = v->caret;
    guint32 flags = v->inline_flags;
    /* Typing inside a #tag span extends the tag.                           */
    OnText *t = caret_text(v);
    if (p.offset > 0 && p.offset < t->text->len + 1 &&
        (on_text_flags_at(t, p.offset - 1) & ON_FMT_TAG) &&
        (p.offset == t->text->len || (on_text_flags_at(t, p.offset) & ON_FMT_TAG)))
        flags |= ON_FMT_TAG;
    if (on_document_insert_text(v->doc, p, text, n, flags)) {
        v->caret.offset += n;
        v->anchor = v->caret;
    }
    /* Sentence enders cap a typing group; so does the tag capture's end. */
    if (memchr(text, '.', n) != NULL || memchr(text, '?', n) != NULL) {
        if (++v->sentences >= UNDO_MAX_SENTENCES)
            typing_end(v);
    }
    after_edit(v);
}

/* enter_pressed() — Enter: list continuation and block splitting.          */
static void
enter_pressed(OnNoteView *v)
{
    if (v->tag_capturing)
        tag_capture_end(v, TRUE);
    typing_end(v);
    on_document_begin_group(v->doc);
    delete_selection(v);
    OnBlock *b = caret_block(v);
    OnPos p = v->caret;
    if (p.cell >= 0) {
        /* Inside a table cell: a line break inside the cell.               */
        on_document_insert_text(v->doc, p, "\n", 1, 0);
        v->caret.offset++;
        v->anchor = v->caret;
    } else if (b->text == NULL) {
        /* Beside an object: a new paragraph after (or before) it.          */
        guint at = p.block + (p.offset > 0 ? 1 : 0);
        on_document_insert_block(v->doc, at, on_block_new_text(ON_BLOCK_PARA));
        OnPos np = { at, -1, 0 };
        v->caret = v->anchor = np;
    } else {
        gboolean list = b->kind == ON_BLOCK_BULLET ||
                        b->kind == ON_BLOCK_NUMBER || b->kind == ON_BLOCK_CHECK;
        if (list && b->text->text->len == 0) {
            /* Enter on an empty item ends the list.                        */
            on_document_set_kind(v->doc, p.block, ON_BLOCK_PARA);
        } else {
            OnBlockKind kind = b->kind;
            on_document_split_block(v->doc, p, v->inline_flags);
            OnPos np = { p.block + 1, -1, 0 };
            v->caret = v->anchor = np;
            /* Headings are one line: the next is body text.  Lists go on
             * (a new task unchecked, which split already gives).          */
            if (kind == ON_BLOCK_H1 || kind == ON_BLOCK_H2)
                on_document_set_kind(v->doc, np.block, ON_BLOCK_PARA);
        }
    }
    on_document_end_group(v->doc);
    after_edit(v);
}

/* backspace_pressed() / delete_pressed() — the two deletions.               */
static void
delete_key(OnNoteView *v, gboolean backward, gboolean word)
{
    if (v->tag_capturing && !backward)
        tag_capture_end(v, TRUE);
    if (has_selection(v)) {
        typing_end(v);
        delete_selection(v);
        after_edit(v);
        return;
    }
    OnBlock *b = caret_block(v);
    OnPos p = v->caret;
    typing_begin(v);

    if (b->text == NULL && p.cell < 0) {
        /* Beside an object: the object goes when the key points at it.     */
        if ((backward && p.offset == 1) || (!backward && p.offset == 0)) {
            OnPos a = { p.block, -1, 0 }, e = { p.block, -1, 1 }, out;
            on_document_delete_range(v->doc, a, e, &out);
            v->caret = v->anchor = clamp_pos(v, out);
        } else {
            gint gx = -1;
            OnPos q = on_doc_layout_move(v->layout, p,
                                         backward ? ON_MOVE_CHAR_LEFT
                                                  : ON_MOVE_CHAR_RIGHT, &gx);
            if (!pos_eq(q, p)) {
                OnPos out;
                on_document_delete_range(v->doc, backward ? q : p,
                                         backward ? p : q, &out);
                v->caret = v->anchor = clamp_pos(v, out);
            }
        }
        after_edit(v);
        return;
    }

    OnText *t = caret_text(v);
    if (backward && p.offset == 0) {
        if (p.cell >= 0) {           /* a cell's start: nothing to join     */
            typing_end(v);
            return;
        }
        gboolean list = b->kind == ON_BLOCK_BULLET ||
                        b->kind == ON_BLOCK_NUMBER || b->kind == ON_BLOCK_CHECK;
        if (list) {
            on_document_set_kind(v->doc, p.block, ON_BLOCK_PARA);
        } else if (p.block > 0) {
            OnBlock *prev = on_document_block(v->doc, p.block - 1);
            if (prev->text != NULL) {
                gsize len = prev->text->text->len;
                /* Backspacing an EMPTY paragraph into the block above only
                 * removes the paragraph — the block above keeps its kind
                 * and text either way (join keeps the first's kind).      */
                on_document_join_blocks(v->doc, p.block - 1);
                OnPos np = { p.block - 1, -1, len };
                v->caret = v->anchor = np;
            } else if (t->text->len == 0) {
                on_document_remove_block(v->doc, p.block);
                OnPos np = { p.block - 1, -1, 1 };
                v->caret = v->anchor = np;
            } else {
                OnPos np = { p.block - 1, -1, 1 };   /* stop beside it     */
                v->caret = v->anchor = np;
            }
        }
        after_edit(v);
        return;
    }
    if (!backward && p.offset >= t->text->len) {
        if (p.cell >= 0) {
            typing_end(v);
            return;
        }
        if (p.block + 1 < on_document_n_blocks(v->doc)) {
            OnBlock *next = on_document_block(v->doc, p.block + 1);
            if (next->text != NULL) {
                on_document_join_blocks(v->doc, p.block);
            } else if (t->text->len == 0) {
                on_document_remove_block(v->doc, p.block);
                OnPos np = { p.block, -1, 0 };
                v->caret = v->anchor = np;
            } else {
                OnPos np = { p.block + 1, -1, 0 };
                v->caret = v->anchor = np;
            }
        }
        after_edit(v);
        return;
    }

    /* Inside the text: one grapheme (or word) either side.                 */
    gint gx = -1;
    OnPos q = on_doc_layout_move(v->layout, p,
                                 backward ? (word ? ON_MOVE_WORD_LEFT
                                                  : ON_MOVE_CHAR_LEFT)
                                          : (word ? ON_MOVE_WORD_RIGHT
                                                  : ON_MOVE_CHAR_RIGHT), &gx);
    if (q.block != p.block || q.cell != p.cell)
        q = p;                       /* never across a boundary here        */
    OnPos a = backward ? q : p, e = backward ? p : q;
    if (e.offset > a.offset) {
        on_document_delete_text(v->doc, a, e.offset - a.offset);
        v->caret = v->anchor = a;
    }
    if (v->tag_capturing)
        tag_popup_update(v);
    after_edit(v);
}

/* ===========================================================================
 * inline and paragraph formatting
 * ======================================================================== */

/* flags_changed() — "inline-flags-changed".                                 */
static void
flags_changed(OnNoteView *v)
{
    g_signal_emit(v, signals[SIG_INLINE_FLAGS_CHANGED], 0);
}

/* adopt_flags() — take the inline flags from the character before the
 * caret (what real navigation does; typing keeps the armed set).           */
static void
adopt_flags(OnNoteView *v)
{
    OnText *t = caret_text(v);
    guint32 flags = 0;
    if (t != NULL && t->text->len > 0)
        flags = on_text_flags_at(t, v->caret.offset > 0 ? v->caret.offset - 1
                                                        : 0) &
                ON_FMT_INLINE_MASK;
    if (flags != v->inline_flags) {
        v->inline_flags = flags;
        flags_changed(v);
    }
}

void
on_note_view_toggle_inline(OnNoteView *v, OnFormatFlags flag)
{
    if (has_selection(v)) {
        typing_end(v);
        OnPos a, b;
        sel_bounds(v, &a, &b);
        /* Toggle over the selection: if the first selected character
         * already has the style, remove it everywhere, else apply it.    */
        OnText *t = on_document_text_at(v->doc, a);
        gboolean has = t != NULL && (on_text_flags_at(t, a.offset) & flag);
        on_document_begin_group(v->doc);
        for (guint i = a.block; i <= b.block; i++) {
            OnBlock *blk = on_document_block(v->doc, i);
            if (blk->text == NULL)
                continue;
            if (i == a.block && a.cell >= 0 && a.cell == b.cell) {
                on_document_set_flags(v->doc, a, b.offset - a.offset, flag,
                                      !has);
                break;
            }
            OnPos p = { i, -1, (i == a.block) ? a.offset : 0 };
            gsize end = (i == b.block) ? b.offset : blk->text->text->len;
            if (end > p.offset)
                on_document_set_flags(v->doc, p, end - p.offset, flag, !has);
        }
        on_document_end_group(v->doc);
        v->inline_flags = has ? (v->inline_flags & ~(guint32)flag)
                              : (v->inline_flags | (guint32)flag);
        after_edit(v);
    } else {
        v->inline_flags ^= (guint32)flag;
    }
    flags_changed(v);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

guint32
on_note_view_inline_flags(OnNoteView *v)
{
    return v->inline_flags;
}

/* kind_for_flag() — the block kind a paragraph flag stands for.             */
static OnBlockKind
kind_for_flag(guint32 flag)
{
    switch (flag) {
    case ON_FMT_H1:          return ON_BLOCK_H1;
    case ON_FMT_H2:          return ON_BLOCK_H2;
    case ON_FMT_CODEBLOCK:   return ON_BLOCK_CODE;
    case ON_FMT_LIST_BULLET: return ON_BLOCK_BULLET;
    case ON_FMT_LIST_NUMBER: return ON_BLOCK_NUMBER;
    case ON_FMT_LIST_CHECK:  return ON_BLOCK_CHECK;
    default:                 return ON_BLOCK_PARA;
    }
}

void
on_note_view_toggle_paragraph(OnNoteView *v, guint32 flag)
{
    OnPos a, b;
    sel_bounds(v, &a, &b);
    OnBlockKind kind = kind_for_flag(flag);
    gboolean all_have = TRUE;
    for (guint i = a.block; i <= b.block; i++) {
        OnBlock *blk = on_document_block(v->doc, i);
        if (blk->text != NULL && blk->kind != kind)
            all_have = FALSE;
    }
    if (all_have && flag != 0)
        kind = ON_BLOCK_PARA;        /* toggle off                          */
    typing_end(v);
    on_document_begin_group(v->doc);
    for (guint i = a.block; i <= b.block; i++) {
        OnBlock *blk = on_document_block(v->doc, i);
        if (blk->text == NULL)
            continue;
        on_document_set_kind(v->doc, i, kind);
        if (kind == ON_BLOCK_CODE) {
            /* Text inside a code block is never a #tag.                    */
            gsize len = blk->text->text->len;
            OnPos p = { i, -1, 0 };
            if (len > 0)
                on_document_set_flags(v->doc, p, len, ON_FMT_TAG, FALSE);
        }
    }
    on_document_end_group(v->doc);
    after_edit(v);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

/* ===========================================================================
 * inserting things
 * ======================================================================== */

/* insert_block_at_caret() — a block of its own at the caret: the caret's
 * block is split around it (a fragment insert), the caret lands after.    */
static void
insert_block_at_caret(OnNoteView *v, OnBlock *block)
{
    typing_end(v);
    on_document_begin_group(v->doc);
    delete_selection(v);
    OnPos out = v->caret;
    if (caret_text(v) == NULL || v->caret.cell >= 0) {
        guint at = v->caret.block + (v->caret.offset > 0 ? 1 : 0);
        if (v->caret.cell >= 0)
            at = v->caret.block + 1;
        on_document_insert_block(v->doc, at, block);
        out.block = at;
        out.cell = -1;
        out.offset = 1;
    } else {
        OnDocument *frag = on_document_new_from_block(block);
        on_document_insert_fragment(v->doc, v->caret, frag, &out);
        on_document_free(frag);
    }
    on_document_end_group(v->doc);
    v->caret = v->anchor = out;
    after_edit(v);
}

void
on_note_view_insert_image(OnNoteView *v, GdkPixbuf *pixbuf)
{
    GBytes *png = on_image_png_bytes(pixbuf);
    if (png == NULL)
        return;
    insert_block_at_caret(v, on_block_new_image(png, 0));
}

void
on_note_view_insert_table(OnNoteView *v)
{
    insert_block_at_caret(v, on_block_new_table(3, 3));
}

void
on_note_view_insert_date(OnNoteView *v)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *date = g_date_time_format(now, "%Y-%m-%d");
    g_date_time_unref(now);
    insert_typed(v, date, strlen(date));
    g_free(date);
}

static void
on_emoji_picked(GtkEmojiChooser *chooser, const gchar *text, gpointer data)
{
    (void)chooser;
    OnNoteView *v = data;
    insert_typed(v, text, strlen(text));
}

void
on_note_view_insert_emoji(OnNoteView *v)
{
    if (v->emoji_chooser == NULL) {
        v->emoji_chooser = gtk_emoji_chooser_new();
        gtk_widget_set_parent(v->emoji_chooser, GTK_WIDGET(v));
        g_signal_connect(v->emoji_chooser, "emoji-picked",
                         G_CALLBACK(on_emoji_picked), v);
    }
    graphene_rect_t r;
    on_doc_layout_caret_rect(v->layout, v->caret, &r);
    GdkRectangle at = { (gint)r.origin.x, (gint)(r.origin.y - scroll_y(v)),
                        1, (gint)r.size.height };
    gtk_popover_set_pointing_to(GTK_POPOVER(v->emoji_chooser), &at);
    gtk_popover_popup(GTK_POPOVER(v->emoji_chooser));
}

/* ===========================================================================
 * undo / redo
 * ======================================================================== */

static void
undo_redo(OnNoteView *v, gboolean redo)
{
    if (v->tag_capturing)
        tag_capture_end(v, FALSE);
    typing_end(v);
    gboolean did = redo ? on_document_redo(v->doc) : on_document_undo(v->doc);
    if (!did)
        return;
    v->caret = v->anchor = clamp_pos(v, on_document_last_change(v->doc));
    adopt_flags(v);
    after_edit(v);
}

void
on_note_view_undo(OnNoteView *v)
{
    undo_redo(v, FALSE);
}

void
on_note_view_redo(OnNoteView *v)
{
    undo_redo(v, TRUE);
}

/* ===========================================================================
 * the clipboard
 * ======================================================================== */

/* copy_selection() — the selection to the clipboard as plain text AND a
 * BNBF fragment.                                                            */
static void
copy_selection(OnNoteView *v)
{
    if (!has_selection(v))
        return;
    OnPos a, b;
    sel_bounds(v, &a, &b);
    OnDocument *frag = on_document_copy_range(v->doc, a, b);
    gsize n;
    guint8 *blob = on_document_to_bnbf(frag, &n);
    gchar *text = on_document_plain_text(frag);
    on_document_free(frag);

    GBytes *bytes = g_bytes_new_take(blob, n);
    GdkContentProvider *providers[2] = {
        gdk_content_provider_new_for_bytes(ON_NOTE_MIME, bytes),
        gdk_content_provider_new_typed(G_TYPE_STRING, text),
    };
    GdkContentProvider *all = gdk_content_provider_new_union(providers, 2);
    gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(v)), all);
    g_object_unref(all);
    g_bytes_unref(bytes);
    g_free(text);
}

/* paste_fragment() — insert a fragment at the caret (replacing a
 * selection); plain-text fragments into a code block become code.          */
static void
paste_fragment(OnNoteView *v, OnDocument *frag, gboolean plain)
{
    typing_end(v);
    im_reset(v);
    on_document_begin_group(v->doc);
    delete_selection(v);
    caret_ensure_text(v);
    if (plain && caret_block(v)->kind == ON_BLOCK_CODE)
        for (guint i = 0; i < on_document_n_blocks(frag); i++)
            on_document_set_kind(frag, i, ON_BLOCK_CODE);
    OnPos out;
    if (on_document_insert_fragment(v->doc, v->caret, frag, &out))
        v->caret = v->anchor = clamp_pos(v, out);
    on_document_end_group(v->doc);
    after_edit(v);
}

/* The three clipboard reads land on later main-loop turns; each holds a
 * weak reference so a view that closed meanwhile just drops the paste.   */
static GWeakRef *
weak_new(OnNoteView *v)
{
    GWeakRef *ref = g_new(GWeakRef, 1);
    g_weak_ref_init(ref, v);
    return ref;
}

static OnNoteView *
weak_take(GWeakRef *ref)
{
    OnNoteView *v = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    return v;
}

/* on_paste_spliced() — the whole BNBF fragment has arrived.  The source
 * is a memory stream in-process but a PIPE from another instance, and one
 * read on a pipe returns one chunk: a single 64 MiB read_bytes() parsed a
 * truncated blob (and allocated 64 MiB to do it).  A splice reads to EOF. */
static void
on_paste_spliced(GObject *source, GAsyncResult *res, gpointer data)
{
    OnNoteView *v = weak_take(data);
    GMemoryOutputStream *mem = G_MEMORY_OUTPUT_STREAM(source);
    gboolean ok = g_output_stream_splice_finish(G_OUTPUT_STREAM(mem), res,
                                                NULL) >= 0;
    if (ok && v != NULL) {
        GBytes *bytes = g_memory_output_stream_steal_as_bytes(mem);
        gsize n;
        const guint8 *d = g_bytes_get_data(bytes, &n);
        OnDocument *frag = on_document_from_bnbf(d, n, NULL);
        paste_fragment(v, frag, FALSE);
        on_document_free(frag);
        g_bytes_unref(bytes);
    }
    g_object_unref(mem);
    g_clear_object(&v);
}

static void
on_paste_bytes(GObject *source, GAsyncResult *res, gpointer data)
{
    GWeakRef *ref = data;            /* handed on to the splice             */
    const gchar *mime = NULL;
    GInputStream *in = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), res,
                                                 &mime, NULL);
    if (in == NULL) {
        g_clear_object(&(OnNoteView *){ weak_take(ref) });
        return;
    }
    GOutputStream *mem = g_memory_output_stream_new_resizable();
    g_output_stream_splice_async(mem, in,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 G_PRIORITY_DEFAULT, NULL, on_paste_spliced,
                                 ref);
    g_object_unref(in);              /* the splice holds its own ref        */
}

static void
on_paste_texture(GObject *source, GAsyncResult *res, gpointer data)
{
    OnNoteView *v = weak_take(data);
    GdkTexture *tex = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source),
                                                        res, NULL);
    if (tex != NULL && v != NULL) {
        GBytes *png = gdk_texture_save_to_png_bytes(tex);
        OnDocument *frag = on_document_new_from_block(on_block_new_image(png, 0));
        g_bytes_unref(png);
        paste_fragment(v, frag, FALSE);
        on_document_free(frag);
    }
    g_clear_object(&tex);
    g_clear_object(&v);
}

static void
on_paste_text(GObject *source, GAsyncResult *res, gpointer data)
{
    OnNoteView *v = weak_take(data);
    gchar *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res,
                                                 NULL);
    if (text != NULL && v != NULL) {
        OnDocument *frag = on_document_from_text(text);
        paste_fragment(v, frag, TRUE);
        on_document_free(frag);
    }
    g_free(text);
    g_clear_object(&v);
}

/* paste() — a note fragment, else an image, else text.                     */
static void
paste(OnNoteView *v)
{
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(v));
    GdkContentFormats *formats = gdk_clipboard_get_formats(cb);
    if (gdk_content_formats_contain_mime_type(formats, ON_NOTE_MIME)) {
        const gchar *mimes[] = { ON_NOTE_MIME, NULL };
        gdk_clipboard_read_async(cb, mimes, G_PRIORITY_DEFAULT, NULL,
                                 on_paste_bytes, weak_new(v));
        return;
    }
    GdkContentFormats *all = gdk_content_formats_union_deserialize_gtypes(
        gdk_content_formats_ref(formats));
    gboolean image = gdk_content_formats_contain_gtype(all, GDK_TYPE_TEXTURE);
    gdk_content_formats_unref(all);
    if (image)
        gdk_clipboard_read_texture_async(cb, NULL, on_paste_texture,
                                         weak_new(v));
    else
        gdk_clipboard_read_text_async(cb, NULL, on_paste_text, weak_new(v));
}

/* select_all()                                                              */
static void
select_all(OnNoteView *v)
{
    gint gx = -1;
    OnPos s = { 0, -1, 0 };
    v->anchor = on_doc_layout_move(v->layout, s, ON_MOVE_DOC_START, &gx);
    v->caret  = on_doc_layout_move(v->layout, s, ON_MOVE_DOC_END, &gx);
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

/* ===========================================================================
 * #tag capture and autocomplete popup
 *
 * Typing '#' at a word boundary starts a capture: the '#' position is
 * remembered, the known tags are fetched ONCE, and a non-focusable popover
 * lists the ones matching what has been typed since.  Space, Enter, a
 * click elsewhere or the focus leaving END the capture and style the
 * "#word" as a tag; Escape ends it without.  Up/Down/Tab/Enter pick from
 * the list.
 * ======================================================================== */

/* tag_span() — the capture's "#word" span; FALSE when it no longer starts
 * with '#'.                                                                 */
static gboolean
tag_span(OnNoteView *v, OnPos *start, OnPos *end)
{
    OnText *t = on_document_text_at(v->doc, v->tag_start);
    if (t == NULL || v->tag_start.offset >= t->text->len ||
        t->text->str[v->tag_start.offset] != '#')
        return FALSE;
    *start = v->tag_start;
    *end = *start;
    const gchar *p = t->text->str + start->offset + 1;
    while (*p != '\0') {
        gunichar c = g_utf8_get_char(p);
        if (!(g_unichar_isalnum(c) || c == '_' || c == '-'))
            break;
        p = g_utf8_next_char(p);
    }
    end->offset = (gsize)(p - t->text->str);
    return TRUE;
}

static void
tag_popup_hide(OnNoteView *v)
{
    if (v->tag_popup != NULL)
        gtk_popover_popdown(GTK_POPOVER(v->tag_popup));
}

static void
tag_capture_end(OnNoteView *v, gboolean apply)
{
    if (!v->tag_capturing)
        return;
    OnPos s, e;
    if (tag_span(v, &s, &e) && apply && e.offset - s.offset >= 2)
        on_document_set_flags(v->doc, s, e.offset - s.offset, ON_FMT_TAG, TRUE);
    v->tag_capturing = FALSE;
    on_db_tag_list_free(v->tag_choices);
    v->tag_choices = NULL;
    tag_popup_hide(v);
    if (apply) {
        gtk_widget_queue_draw(GTK_WIDGET(v));
        g_signal_emit(v, signals[SIG_EDITED], 0);
    }
}

/* on_tag_row_activated() — a suggestion chosen: replace the partial word
 * with the tag name, style it, and end the capture with a space.           */
static void
on_tag_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    OnNoteView *v = data;
    const gchar *name = g_object_get_data(G_OBJECT(row), "on-tag-name");
    OnPos s, e;
    if (name == NULL || !v->tag_capturing || !tag_span(v, &s, &e)) {
        tag_capture_end(v, FALSE);
        return;
    }
    typing_end(v);
    on_document_begin_group(v->doc);
    on_document_delete_text(v->doc, s, e.offset - s.offset);
    gchar *full = g_strdup_printf("#%s", name);
    on_document_insert_text(v->doc, s, full, strlen(full), ON_FMT_TAG);
    OnPos after = s;
    after.offset += strlen(full);
    on_document_insert_text(v->doc, after, " ", 1, 0);
    after.offset++;
    g_free(full);
    on_document_end_group(v->doc);
    v->caret = v->anchor = after;
    v->tag_capturing = FALSE;
    on_db_tag_list_free(v->tag_choices);
    v->tag_choices = NULL;
    tag_popup_hide(v);
    after_edit(v);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

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

static void
tag_popup_update(OnNoteView *v)
{
    if (!v->tag_capturing)
        return;
    OnPos s, e;
    if (!tag_span(v, &s, &e)) {
        tag_capture_end(v, FALSE);
        return;
    }
    OnText *t = on_document_text_at(v->doc, s);
    gchar *prefix = g_strndup(t->text->str + s.offset + 1,
                              e.offset - s.offset - 1);
    tag_popup_ensure(v);
    gtk_list_box_remove_all(GTK_LIST_BOX(v->tag_listbox));
    gint shown = 0;
    gchar *prefix_ci = g_utf8_casefold(prefix, -1);
    for (GList *l = v->tag_choices; l != NULL && shown < TAG_POPUP_MAX;
         l = l->next) {
        OnTag *tag = l->data;
        gchar *name_ci = g_utf8_casefold(tag->name, -1);
        gboolean match = g_str_has_prefix(name_ci, prefix_ci);
        g_free(name_ci);
        if (!match)
            continue;
        gchar *text = g_strdup_printf("#%s", tag->name);
        GtkWidget *label = gtk_label_new(text);
        g_free(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 3);
        gtk_widget_set_margin_bottom(label, 3);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "on-tag-name",
                               g_strdup(tag->name), g_free);
        gtk_list_box_insert(GTK_LIST_BOX(v->tag_listbox), row, -1);
        shown++;
    }
    g_free(prefix_ci);
    g_free(prefix);
    if (shown == 0) {
        tag_popup_hide(v);
        return;
    }
    gtk_list_box_select_row(
        GTK_LIST_BOX(v->tag_listbox),
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(v->tag_listbox), 0));
    graphene_rect_t r;
    on_doc_layout_caret_rect(v->layout, s, &r);
    GdkRectangle at = { (gint)r.origin.x, (gint)(r.origin.y - scroll_y(v)),
                        1, (gint)r.size.height };
    gtk_popover_set_pointing_to(GTK_POPOVER(v->tag_popup), &at);
    gtk_popover_popup(GTK_POPOVER(v->tag_popup));
}

static void
tag_popup_move_selection(OnNoteView *v, gint delta)
{
    GtkListBox *box = GTK_LIST_BOX(v->tag_listbox);
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(box);
    gint index = (sel != NULL) ? gtk_list_box_row_get_index(sel) + delta : 0;
    index = MAX(index, 0);
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(box, index);
    if (row != NULL)
        gtk_list_box_select_row(box, row);
}

/* tag_capture_consider() — the `n` bytes of `text` were just typed and
 * now end at the caret: walk them, starting a capture on a '#' at a word
 * boundary, ending one on a non-word character, refreshing the popup
 * otherwise.  Typed text normally arrives one character at a time; a
 * longer commit is walked the same way.                                    */
static void
tag_capture_consider(OnNoteView *v, const gchar *text, gsize n)
{
    OnText *t = caret_text(v);
    if (t == NULL || caret_block(v)->kind == ON_BLOCK_CODE ||
        v->caret.cell >= 0)
        return;
    gsize start = v->caret.offset - n;     /* where the commit began       */
    for (const gchar *p = text; p < text + n; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        gsize at = start + (gsize)(p - text);
        if (v->tag_capturing) {
            if (!(g_unichar_isalnum(c) || c == '_' || c == '-'))
                tag_capture_end(v, TRUE);
            continue;
        }
        if (c != '#')
            continue;
        gboolean boundary = at == 0;
        if (!boundary) {
            gunichar pc = g_utf8_get_char(g_utf8_prev_char(t->text->str + at));
            boundary = g_unichar_isspace(pc) || g_unichar_ispunct(pc);
        }
        if (!boundary)
            continue;
        v->tag_capturing = TRUE;
        v->tag_start = v->caret;
        v->tag_start.offset = at;
        on_db_tag_list_free(v->tag_choices);
        v->tag_choices = on_db_tag_list(v->app->db);
    }
    if (v->tag_capturing)
        tag_popup_update(v);
}

/* ===========================================================================
 * input: keys and the input method
 * ======================================================================== */

static void
on_im_commit(GtkIMContext *im, const gchar *text, gpointer data)
{
    (void)im;
    OnNoteView *v = data;
    if (text == NULL || *text == '\0')
        return;
    /* A committed newline (some methods send one) is Enter.               */
    if (strcmp(text, "\n") == 0 || strcmp(text, "\r") == 0) {
        enter_pressed(v);
        return;
    }
    insert_typed(v, text, strlen(text));
    tag_capture_consider(v, text, strlen(text));
}

static void
on_im_preedit_changed(GtkIMContext *im, gpointer data)
{
    OnNoteView *v = data;
    gchar *str = NULL;
    PangoAttrList *attrs = NULL;
    gint cursor = 0;
    gtk_im_context_get_preedit_string(im, &str, &attrs, &cursor);
    g_free(v->preedit);
    v->preedit = (str != NULL && *str != '\0') ? g_strdup(str) : NULL;
    /* The cursor is in characters; the layout wants bytes.                */
    gint cursor_bytes = 0;
    if (v->preedit != NULL) {
        const gchar *p = v->preedit;
        for (gint k = 0; k < cursor && *p != '\0'; k++)
            p = g_utf8_next_char(p);
        cursor_bytes = (gint)(p - v->preedit);
    }
    if (v->preedit != NULL && caret_text(v) == NULL)
        g_clear_pointer(&v->preedit, g_free);   /* nowhere to show it     */
    on_doc_layout_set_preedit(v->layout, v->caret, v->preedit, attrs,
                              cursor_bytes);
    if (attrs != NULL)
        pango_attr_list_unref(attrs);
    g_free(str);
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

static gboolean
on_im_retrieve_surrounding(GtkIMContext *im, gpointer data)
{
    OnNoteView *v = data;
    OnText *t = caret_text(v);
    if (t == NULL)
        return FALSE;
    gint anchor = (v->anchor.block == v->caret.block &&
                   v->anchor.cell == v->caret.cell)
                  ? (gint)v->anchor.offset : (gint)v->caret.offset;
    gtk_im_context_set_surrounding_with_selection(
        im, t->text->str, (gint)t->text->len, (gint)v->caret.offset, anchor);
    return TRUE;
}

static gboolean
on_im_delete_surrounding(GtkIMContext *im, gint offset, gint n_chars,
                         gpointer data)
{
    (void)im;
    OnNoteView *v = data;
    OnText *t = caret_text(v);
    if (t == NULL)
        return FALSE;
    /* offset/n_chars are in characters relative to the caret.             */
    const gchar *base = t->text->str + v->caret.offset;
    const gchar *s = base;
    for (gint k = 0; k < -offset && s > t->text->str; k++)
        s = g_utf8_prev_char(s);
    for (gint k = 0; k < offset && *s != '\0'; k++)
        s = g_utf8_next_char(s);
    const gchar *e = s;
    for (gint k = 0; k < n_chars && *e != '\0'; k++)
        e = g_utf8_next_char(e);
    OnPos p = v->caret;
    p.offset = (gsize)(s - t->text->str);
    typing_begin(v);
    if (on_document_delete_text(v->doc, p, (gsize)(e - s))) {
        v->caret = v->anchor = p;
        after_edit(v);
    }
    return TRUE;
}

/* im_reset() — abandon a preedit in progress (a dead key's accent, a
 * half-composed CJK syllable) before the caret moves for another reason:
 * a click, a navigation key, a paste, a load.  GtkTextView does the same
 * at every one of those points; without it the preedit stayed painted at
 * its old position after the caret left, and a later cut could shorten
 * the block past that position.  The reset comes back through
 * "preedit-changed" with an empty string, which clears the layout.       */
static void
im_reset(OnNoteView *v)
{
    if (v->preedit != NULL)
        gtk_im_context_reset(v->im);
}

/* im_sync_location() — tell the input method where the caret is.          */
static void
im_sync_location(OnNoteView *v)
{
    if (v->im == NULL || !gtk_widget_get_realized(GTK_WIDGET(v)))
        return;
    graphene_rect_t r;
    on_doc_layout_caret_rect(v->layout, v->caret, &r);
    GdkRectangle at = { (gint)r.origin.x, (gint)(r.origin.y - scroll_y(v)),
                        1, (gint)r.size.height };
    gtk_im_context_set_cursor_location(v->im, &at);
}

/* move_caret() — a navigation key: extend when Shift is held.              */
static void
move_caret(OnNoteView *v, OnDocMove how, gboolean extend)
{
    if (v->tag_capturing && how != ON_MOVE_LINE_UP && how != ON_MOVE_LINE_DOWN)
        tag_capture_end(v, TRUE);
    typing_end(v);
    /* Left/Right with a selection and no Shift collapse to its edge.       */
    if (!extend && has_selection(v) &&
        (how == ON_MOVE_CHAR_LEFT || how == ON_MOVE_CHAR_RIGHT)) {
        OnPos a, b;
        sel_bounds(v, &a, &b);
        v->goal_x = -1;
        set_caret(v, how == ON_MOVE_CHAR_LEFT ? a : b, FALSE);
        adopt_flags(v);
        return;
    }
    OnPos p = on_doc_layout_move(v->layout, v->caret, how, &v->goal_x);
    /* Down off the LAST line of a code block that ends the note: there is
     * nowhere to go and Enter only adds another code line, so the block
     * had no exit.  Open a body line under it and land there.            */
    if (how == ON_MOVE_LINE_DOWN && !extend && on_pos_cmp(p, v->caret) == 0 &&
        caret_block(v)->kind == ON_BLOCK_CODE &&
        v->caret.block + 1 == on_document_n_blocks(v->doc)) {
        guint at = v->caret.block + 1;
        on_document_insert_block(v->doc, at, on_block_new_text(ON_BLOCK_PARA));
        OnPos np = { at, -1, 0 };
        v->caret = v->anchor = np;
        v->goal_x = -1;
        after_edit(v);
        return;
    }
    set_caret(v, p, extend);
    adopt_flags(v);
}

/* page_move() — Page Up/Down: a page's worth of lines.                     */
static void
page_move(OnNoteView *v, gboolean down, gboolean extend)
{
    gdouble page = (v->vadj != NULL) ? gtk_adjustment_get_page_size(v->vadj)
                                     : 400;
    graphene_rect_t r;
    on_doc_layout_caret_rect(v->layout, v->caret, &r);
    gdouble target_y = r.origin.y + (down ? page : -page);
    if (v->goal_x < 0)
        v->goal_x = (gint)r.origin.x;
    OnDocHitResult hit;
    on_doc_layout_hit(v->layout, v->goal_x, MAX(0, target_y), &hit);
    gint gx = v->goal_x;
    set_caret(v, hit.pos, extend);
    v->goal_x = gx;
    if (v->vadj != NULL)
        gtk_adjustment_set_value(v->vadj, scroll_y(v) + (down ? page : -page));
}

/* tab_pressed() — Tab: the next (or previous) cell inside a table, a tab
 * character elsewhere.                                                      */
static void
tab_pressed(OnNoteView *v, gboolean backward)
{
    OnBlock *b = caret_block(v);
    if (b->kind == ON_BLOCK_TABLE && v->caret.cell >= 0) {
        gint n = b->rows * b->cols;
        gint cell = v->caret.cell + (backward ? -1 : 1);
        if (cell < 0 || cell >= n)
            return;
        OnPos p = { v->caret.block, cell, 0 };
        OnText *t = on_document_text_at(v->doc, p);
        /* The whole cell selected, as spreadsheets do.                     */
        v->anchor = p;
        p.offset = t->text->len;
        set_caret(v, p, TRUE);
        return;
    }
    if (!backward)
        insert_typed(v, "\t", 1);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer data)
{
    (void)controller; (void)keycode;
    OnNoteView *v = data;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
#ifdef __APPLE__
    gboolean primary = (state & GDK_META_MASK) != 0;
    gboolean word    = (state & GDK_ALT_MASK) != 0;
    gboolean line    = (state & GDK_META_MASK) != 0;
#else
    gboolean primary = (state & GDK_CONTROL_MASK) != 0;
    gboolean word    = (state & GDK_CONTROL_MASK) != 0;
    gboolean line    = FALSE;
#endif

    /* The tag popup owns the navigation keys while it shows.               */
    if (v->tag_capturing && v->tag_popup != NULL &&
        gtk_widget_get_visible(v->tag_popup)) {
        switch (keyval) {
        case GDK_KEY_Down:  tag_popup_move_selection(v, +1); return TRUE;
        case GDK_KEY_Up:    tag_popup_move_selection(v, -1); return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab: {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(
                GTK_LIST_BOX(v->tag_listbox));
            if (row != NULL) {
                on_tag_row_activated(GTK_LIST_BOX(v->tag_listbox), row, v);
                return TRUE;
            }
            break;
        }
        case GDK_KEY_Escape: tag_capture_end(v, FALSE); return TRUE;
        default: break;
        }
    } else if (v->tag_capturing && keyval == GDK_KEY_Escape) {
        tag_capture_end(v, FALSE);
        return TRUE;
    }

    /* A key the input method let through while a preedit is up ends the
     * composition (the IM keeps its own keys — the arrows inside a CJK
     * candidate list never reach here).                                    */
    im_reset(v);

    switch (keyval) {
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        move_caret(v, line ? ON_MOVE_LINE_START
                           : word ? ON_MOVE_WORD_LEFT : ON_MOVE_CHAR_LEFT, shift);
        return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        move_caret(v, line ? ON_MOVE_LINE_END
                           : word ? ON_MOVE_WORD_RIGHT : ON_MOVE_CHAR_RIGHT,
                   shift);
        return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
        move_caret(v, line ? ON_MOVE_DOC_START : ON_MOVE_LINE_UP, shift);
        return TRUE;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
        move_caret(v, line ? ON_MOVE_DOC_END : ON_MOVE_LINE_DOWN, shift);
        return TRUE;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        move_caret(v, primary ? ON_MOVE_DOC_START : ON_MOVE_LINE_START, shift);
        return TRUE;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
        move_caret(v, primary ? ON_MOVE_DOC_END : ON_MOVE_LINE_END, shift);
        return TRUE;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
        page_move(v, FALSE, shift);
        return TRUE;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
        page_move(v, TRUE, shift);
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        enter_pressed(v);
        return TRUE;
    case GDK_KEY_BackSpace:
        delete_key(v, TRUE, word);
        return TRUE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        delete_key(v, FALSE, word);
        return TRUE;
    case GDK_KEY_Tab:
        tab_pressed(v, FALSE);
        return TRUE;
    case GDK_KEY_ISO_Left_Tab:
        tab_pressed(v, TRUE);
        return TRUE;
    case GDK_KEY_Escape:
        if (has_selection(v)) {
            set_caret(v, v->caret, FALSE);
            return TRUE;
        }
        return FALSE;
    default:
        break;
    }
    if (primary && !shift) {
        switch (gdk_keyval_to_lower(keyval)) {
        case GDK_KEY_a: select_all(v); return TRUE;
        case GDK_KEY_c: copy_selection(v); return TRUE;
        case GDK_KEY_x:
            if (has_selection(v)) {
                copy_selection(v);
                typing_end(v);
                delete_selection(v);
                after_edit(v);
            }
            return TRUE;
        case GDK_KEY_v: paste(v); return TRUE;
        default: break;
        }
    }
    return FALSE;
}

/* ===========================================================================
 * input: the pointer
 * ======================================================================== */

/* hit_at() — the layout's answer for a widget point.                       */
static void
hit_at(OnNoteView *v, gdouble x, gdouble y, OnDocHitResult *hit)
{
    on_doc_layout_hit(v->layout, x, y + scroll_y(v), hit);
}

/* context_menu() — the menu for what the press landed on.                  */
static void
context_menu(OnNoteView *v, const OnDocHitResult *hit, gdouble x, gdouble y)
{
    v->ctx = *hit;
    GMenu *menu = g_menu_new();
    OnBlock *b = on_document_block(v->doc, hit->block);
    if (hit->kind == ON_HIT_IMAGE) {
        GMenu *sec = g_menu_new();
        g_menu_append(sec, "Copy _Image", "view.img-copy");
        g_menu_append(sec, "_Open",       "view.img-open");
        /* Shown full size = a stored width at least the source's.        */
        guint32 dw = b->display_width;
        if (hit->inline_image)
            for (guint k = 0; k < b->text->images->len; k++)
                if (g_array_index(b->text->images, OnInlineImage, k).offset ==
                    hit->pos.offset)
                    dw = g_array_index(b->text->images, OnInlineImage, k)
                             .display_width;
        GdkTexture *tex = on_note_view_image_texture(v, hit->image_ord);
        gboolean full = tex != NULL && dw > 0 &&
                        (gint)dw >= gdk_texture_get_width(tex);
        if (full)
            g_menu_append(sec, "Display as _Thumbnail", "view.img-thumb");
        else
            g_menu_append(sec, "Display _Full Size",    "view.img-full");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }
    if (b->kind == ON_BLOCK_TABLE) {
        GMenu *sec = g_menu_new();
        g_menu_append(sec, "Add _Row",      "view.table-row-add");
        g_menu_append(sec, "Add _Column",   "view.table-col-add");
        g_menu_append(sec, "Remove Row",    "view.table-row-del");
        g_menu_append(sec, "Remove Column", "view.table-col-del");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
        g_object_unref(sec);
        sec = g_menu_new();
        GAction *hdr = g_action_map_lookup_action(G_ACTION_MAP(v->actions),
                                                  "table-header");
        g_simple_action_set_state(G_SIMPLE_ACTION(hdr),
                                  g_variant_new_boolean(b->header));
        g_menu_append(sec, "_Header Row",   "view.table-header");
        g_menu_append(sec, "_Delete Table", "view.table-delete");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }
    GMenu *sec = g_menu_new();
    g_menu_append(sec, "Cu_t",   "view.cut");
    g_menu_append(sec, "_Copy",  "view.copy");
    g_menu_append(sec, "_Paste", "view.paste");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
    on_app_menu_popup(GTK_WIDGET(v), G_MENU_MODEL(menu), x, y);
}

/* press_at() — a press at (x, y): what on_pressed does, as a function.
 * Returns TRUE when something other than text took it.                     */
static gboolean
press_at(OnNoteView *v, gdouble x, gdouble y, gint n_press,
         GdkModifierType state, guint button, gboolean context)
{
    gtk_widget_grab_focus(GTK_WIDGET(v));
    im_reset(v);
    OnDocHitResult hit;
    hit_at(v, x, y, &hit);

    if (context) {
        if (!has_selection(v) || hit.kind != ON_HIT_TEXT)
            set_caret(v, hit.pos, FALSE);
        context_menu(v, &hit, x, y);
        return TRUE;
    }
    if (button != GDK_BUTTON_PRIMARY)
        return FALSE;
    if (v->tag_capturing)
        tag_capture_end(v, TRUE);
    typing_end(v);

    if (n_press == 1) {
        switch (hit.kind) {
        case ON_HIT_COPY: {
            guint first, count;
            on_doc_layout_code_run(v->layout, hit.block, &first, &count);
            GString *s = g_string_new(NULL);
            for (guint i = first; i < first + count; i++) {
                if (i > first)
                    g_string_append_c(s, '\n');
                g_string_append(s, on_document_block(v->doc, i)->text->text->str);
            }
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(v)),
                                   s->str);
            g_string_free(s, TRUE);
            on_app_status(v->app, "Code copied");
            return TRUE;
        }
        case ON_HIT_CHECKBOX: {
            OnBlock *b = on_document_block(v->doc, hit.block);
            on_document_set_checked(v->doc, hit.block, !b->checked);
            after_edit(v);
            return TRUE;
        }
        case ON_HIT_IMAGE:
            if ((state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0) {
                set_caret(v, hit.pos, FALSE);
                g_signal_emit(v, signals[SIG_IMAGE_ACTIVATED], 0,
                              hit.image_ord);
                return TRUE;
            }
            break;
        default:
            break;
        }
        v->goal_x = -1;
        set_caret(v, hit.pos, (state & GDK_SHIFT_MASK) != 0);
        adopt_flags(v);
        v->dragging = TRUE;
        v->drag_anchor = v->anchor;
    } else if (n_press == 2) {
        OnPos a, b;
        on_doc_layout_word_at(v->layout, hit.pos, &a, &b);
        v->anchor = a;
        set_caret(v, b, TRUE);
        v->dragging = FALSE;
    } else if (n_press == 3) {
        OnPos a, b;
        on_doc_layout_line_at(v->layout, hit.pos, &a, &b);
        v->anchor = a;
        set_caret(v, b, TRUE);
        v->dragging = FALSE;
    }
    return FALSE;
}

static void
on_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
           gpointer data)
{
    OnNoteView *v = data;
    GtkEventController *ctl = GTK_EVENT_CONTROLLER(gesture);
    GdkEvent *event = gtk_event_controller_get_current_event(ctl);
    gboolean context = event != NULL && gdk_event_triggers_context_menu(event);
    if (press_at(v, x, y, n_press,
                 gtk_event_controller_get_current_event_state(ctl),
                 gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)),
                 context))
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_released(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
            gpointer data)
{
    (void)gesture; (void)n_press; (void)x; (void)y;
    ((OnNoteView *)data)->dragging = FALSE;
}

static void
on_drag_update(GtkGestureDrag *gesture, gdouble dx, gdouble dy, gpointer data)
{
    OnNoteView *v = data;
    if (!v->dragging)
        return;
    gdouble sx, sy;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    OnDocHitResult hit;
    hit_at(v, sx + dx, sy + dy, &hit);
    v->anchor = v->drag_anchor;
    set_caret(v, hit.pos, TRUE);
}

static void
on_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y,
          gpointer data)
{
    (void)controller;
    OnNoteView *v = data;
    OnDocHitResult hit;
    hit_at(v, x, y, &hit);
    if (v->hover_set && hit.kind == v->hover_kind)
        return;
    v->hover_kind = hit.kind;
    v->hover_set  = TRUE;
    gtk_widget_set_cursor_from_name(
        GTK_WIDGET(v),
        (hit.kind == ON_HIT_COPY || hit.kind == ON_HIT_IMAGE ||
         hit.kind == ON_HIT_CHECKBOX) ? "pointer"
        : hit.kind == ON_HIT_OBJECT ? "default" : "text");
}

/* ===========================================================================
 * the "view." actions
 * ======================================================================== */

/* ctx_image_bytes() — the image the context menu was opened on.            */
static GBytes *
ctx_image_bytes(OnNoteView *v)
{
    return on_note_view_image_png(v, v->ctx.image_ord);
}

static void
on_img_copy(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    OnNoteView *v = data;
    GdkTexture *tex = on_note_view_image_texture(v, v->ctx.image_ord);
    if (tex != NULL)
        gdk_clipboard_set_texture(gtk_widget_get_clipboard(GTK_WIDGET(v)),
                                  tex);
}

static void
on_img_open(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    on_note_image_open_external(ctx_image_bytes(data));
}

/* image_set_width() — the context image's display width.                   */
static void
image_set_width(OnNoteView *v, guint32 width)
{
    if (v->ctx.kind != ON_HIT_IMAGE)
        return;
    gint inline_ord = -1;
    if (v->ctx.inline_image) {
        OnBlock *b = on_document_block(v->doc, v->ctx.block);
        for (guint k = 0; b != NULL && b->text != NULL &&
                          k < b->text->images->len; k++)
            if (g_array_index(b->text->images, OnInlineImage, k).offset ==
                v->ctx.pos.offset)
                inline_ord = (gint)k;
        if (inline_ord < 0)
            return;
    }
    typing_end(v);
    if (on_document_set_image_width(v->doc, v->ctx.block, inline_ord, width))
        after_edit(v);
}

static void
on_img_full(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    OnNoteView *v = data;
    GdkTexture *tex = on_note_view_image_texture(v, v->ctx.image_ord);
    if (tex != NULL)
        image_set_width(v, (guint32)gdk_texture_get_width(tex));
}

static void
on_img_thumb(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    image_set_width(data, 0);
}

/* on_table_command() — the table reshapes, by action name.                 */
static void
on_table_command(GSimpleAction *action, GVariant *p, gpointer data)
{
    (void)p;
    OnNoteView *v = data;
    const gchar *name = g_action_get_name(G_ACTION(action));
    OnBlock *b = on_document_block(v->doc, v->ctx.block);
    if (b == NULL || b->kind != ON_BLOCK_TABLE)
        return;
    gint cell = v->ctx.pos.cell >= 0 ? v->ctx.pos.cell : 0;
    gint r = cell / b->cols, c = cell % b->cols;
    typing_end(v);
    gboolean did;
    if (strcmp(name, "table-row-add") == 0)
        did = on_document_table_insert_row(v->doc, v->ctx.block, r + 1);
    else if (strcmp(name, "table-row-del") == 0)
        did = on_document_table_remove_row(v->doc, v->ctx.block, r);
    else if (strcmp(name, "table-col-add") == 0)
        did = on_document_table_insert_col(v->doc, v->ctx.block, c + 1);
    else if (strcmp(name, "table-col-del") == 0)
        did = on_document_table_remove_col(v->doc, v->ctx.block, c);
    else {                           /* table-delete                        */
        OnPos a = { v->ctx.block, -1, 0 }, e = { v->ctx.block, -1, 1 }, out;
        did = on_document_delete_range(v->doc, a, e, &out);
        if (did)
            v->caret = v->anchor = clamp_pos(v, out);
    }
    if (did)
        after_edit(v);
}

static void
on_table_header_change_state(GSimpleAction *action, GVariant *value,
                             gpointer data)
{
    OnNoteView *v = data;
    OnBlock *b = on_document_block(v->doc, v->ctx.block);
    if (b == NULL || b->kind != ON_BLOCK_TABLE)
        return;
    typing_end(v);
    if (on_document_table_set_header(v->doc, v->ctx.block,
                                     g_variant_get_boolean(value))) {
        g_simple_action_set_state(action, value);
        after_edit(v);
    }
}

static void
on_cut(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    OnNoteView *v = data;
    if (!has_selection(v))
        return;
    copy_selection(v);
    typing_end(v);
    delete_selection(v);
    after_edit(v);
}

static void
on_copy(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    copy_selection(data);
}

static void
on_paste_action(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a; (void)p;
    paste(data);
}

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
    { "cut",           on_cut           },
    { "copy",          on_copy          },
    { "paste",         on_paste_action  },
};

static void
install_actions(OnNoteView *v)
{
    v->actions = g_simple_action_group_new();
    for (gsize i = 0; i < G_N_ELEMENTS(VIEW_ACTIONS); i++) {
        GSimpleAction *a = g_simple_action_new(VIEW_ACTIONS[i].name, NULL);
        g_signal_connect(a, "activate", G_CALLBACK(VIEW_ACTIONS[i].activate),
                         v);
        g_action_map_add_action(G_ACTION_MAP(v->actions), G_ACTION(a));
        g_object_unref(a);
    }
    GSimpleAction *hdr = g_simple_action_new_stateful(
        "table-header", NULL, g_variant_new_boolean(FALSE));
    g_signal_connect(hdr, "change-state",
                     G_CALLBACK(on_table_header_change_state), v);
    g_action_map_add_action(G_ACTION_MAP(v->actions), G_ACTION(hdr));
    g_object_unref(hdr);
    gtk_widget_insert_action_group(GTK_WIDGET(v), "view",
                                   G_ACTION_GROUP(v->actions));
}

GActionGroup *
on_note_view_action_group(OnNoteView *v)
{
    return G_ACTION_GROUP(v->actions);
}

/* ===========================================================================
 * in-note find
 * ======================================================================== */

/* find_in_text() — every case-insensitive match of `needle` in `t` as
 * OnDocHit entries appended to `hits`.                                      */
static void
find_in_text(const OnText *t, guint block, gint cell, const gchar *needle,
             GArray *hits)
{
    glong nlen = g_utf8_strlen(needle, -1);
    if (nlen == 0)
        return;
    gunichar *nchars = g_utf8_to_ucs4_fast(needle, -1, NULL);
    for (glong k = 0; k < nlen; k++)
        nchars[k] = g_unichar_tolower(nchars[k]);
    for (const gchar *p = t->text->str; *p != '\0'; p = g_utf8_next_char(p)) {
        const gchar *q = p;
        glong k = 0;
        for (; k < nlen && *q != '\0'; k++, q = g_utf8_next_char(q))
            if (g_unichar_tolower(g_utf8_get_char(q)) != nchars[k])
                break;
        if (k == nlen) {
            OnDocHit h = { block, cell, (gsize)(p - t->text->str),
                           (gsize)(q - p) };
            g_array_append_val(hits, h);
        }
    }
    g_free(nchars);
}

/* find_hits() — every match in the document.                                */
static GArray *
find_hits(OnNoteView *v, const gchar *needle)
{
    GArray *hits = g_array_new(FALSE, FALSE, sizeof(OnDocHit));
    for (guint i = 0; i < on_document_n_blocks(v->doc); i++) {
        OnBlock *b = on_document_block(v->doc, i);
        if (b->text != NULL)
            find_in_text(b->text, i, -1, needle, hits);
        else if (b->kind == ON_BLOCK_TABLE)
            for (guint c = 0; c < b->cells->len; c++)
                find_in_text(g_ptr_array_index(b->cells, c), i, (gint)c,
                             needle, hits);
    }
    return hits;
}

void
on_note_view_find(OnNoteView *v, const gchar *text)
{
    g_free(v->find_text);
    v->find_text = (text != NULL && *text != '\0') ? g_strdup(text) : NULL;
    if (v->find_text == NULL) {
        on_doc_layout_set_hits(v->layout, NULL);
    } else {
        GArray *hits = find_hits(v, v->find_text);
        on_doc_layout_set_hits(v->layout, hits);
        g_array_unref(hits);
        /* Select the first match at or after the caret and scroll to it,
         * so the query lands somewhere as it is typed; Next/Previous then
         * step from THAT selection.  Collapsing the selection to its start
         * first is what makes a match under the caret the first pick.    */
        OnPos a, b;
        sel_bounds(v, &a, &b);
        v->anchor = v->caret = a;
        on_note_view_find_step(v, v->find_text, TRUE);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v));
}

void
on_note_view_find_step(OnNoteView *v, const gchar *text, gboolean forward)
{
    if (text == NULL || *text == '\0')
        return;
    GArray *hits = find_hits(v, text);
    if (hits->len == 0) {
        g_array_unref(hits);
        return;
    }
    OnPos a, b;
    sel_bounds(v, &a, &b);
    OnPos from = forward ? b : a;
    gint pick = -1;
    for (guint k = 0; k < hits->len; k++) {
        const OnDocHit *h = &g_array_index(hits, OnDocHit, k);
        OnPos hp = { h->block, h->cell, h->start };
        if (forward ? on_pos_cmp(hp, from) >= 0 : on_pos_cmp(hp, from) < 0) {
            pick = (gint)k;
            if (forward)
                break;
        }
    }
    if (pick < 0)
        pick = forward ? 0 : (gint)hits->len - 1;   /* wrap around         */
    const OnDocHit *h = &g_array_index(hits, OnDocHit, pick);
    OnPos s = { h->block, h->cell, h->start };
    OnPos e = s;
    e.offset += h->len;
    v->anchor = s;
    set_caret(v, e, TRUE);
    g_array_unref(hits);
}

/* ===========================================================================
 * images by ordinal
 * ======================================================================== */

/* image_locate() — block and inline index of the `ord`-th image.           */
static gboolean
image_locate(OnNoteView *v, gint ord, guint *block, gint *inline_ord)
{
    if (ord < 0)
        return FALSE;
    for (guint i = 0; i < on_document_n_blocks(v->doc); i++) {
        OnBlock *b = on_document_block(v->doc, i);
        if (b->kind == ON_BLOCK_IMAGE) {
            if (ord-- == 0) {
                *block = i;
                *inline_ord = -1;
                return TRUE;
            }
        } else if (b->text != NULL) {
            if ((guint)ord < b->text->images->len) {
                *block = i;
                *inline_ord = ord;
                return TRUE;
            }
            ord -= (gint)b->text->images->len;
        }
    }
    return FALSE;
}

gint
on_note_view_image_count(OnNoteView *v)
{
    return on_document_image_count(v->doc);
}

GdkTexture *
on_note_view_image_texture(OnNoteView *v, gint ord)
{
    guint block;
    gint k;
    if (!image_locate(v, ord, &block, &k))
        return NULL;
    return on_doc_layout_image_texture(v->layout, block, k);
}

GBytes *
on_note_view_image_png(OnNoteView *v, gint ord)
{
    return on_document_image_nth(v->doc, ord, NULL);
}

/* reveal_pos() — the caret to `p`, scrolled a third of the way down the
 * window (like the old view); THE one placement both reveals share.      */
static void
reveal_pos(OnNoteView *v, OnPos p)
{
    set_caret(v, p, FALSE);
    if (v->vadj != NULL) {
        graphene_rect_t r;
        on_doc_layout_caret_rect(v->layout, p, &r);
        gtk_adjustment_set_value(
            v->vadj, MAX(0, r.origin.y -
                            gtk_adjustment_get_page_size(v->vadj) * 0.3));
    }
}

gboolean
on_note_view_image_reveal(OnNoteView *v, gint ord)
{
    guint block;
    gint k;
    if (!image_locate(v, ord, &block, &k))
        return FALSE;
    OnPos p = { block, -1, 0 };
    if (k >= 0)
        p.offset = g_array_index(on_document_block(v->doc, block)->text->images,
                                 OnInlineImage, k).offset;
    reveal_pos(v, p);
    return TRUE;
}

gboolean
on_note_view_action_reveal(OnNoteView *v, gint ord)
{
    GArray *blocks = on_document_action_blocks(v->doc);
    gboolean ok = ord >= 0 && (guint)ord < blocks->len;
    if (ok) {
        OnPos p = { g_array_index(blocks, guint, ord), -1, 1 };
        reveal_pos(v, p);            /* after the '!'                       */
    }
    g_array_unref(blocks);
    return ok;
}

void
on_note_image_open_external(GBytes *png)
{
    if (png == NULL)
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
    gsize n;
    const gchar *bytes = g_bytes_get_data(png, &n);
    if (!g_file_set_contents(path, bytes, (gssize)n, &err)) {
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
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                       NULL, &err)) {
        g_warning("cannot launch image viewer: %s", err->message);
        g_clear_error(&err);
    }
    g_free(viewer);
    g_free(path);
}

/* ===========================================================================
 * action items
 * ======================================================================== */

void
on_note_view_action_marks_hint(OnNoteView *v, GList *items)
{
    GArray *blocks = on_document_action_blocks(v->doc);
    guint i = 0;
    for (GList *l = items; l != NULL && i < blocks->len; l = l->next, i++) {
        OnBlock *b = on_document_block(v->doc, g_array_index(blocks, guint, i));
        if (b->action_uid != 0)
            ((OnActionItem *)l->data)->uid = b->action_uid;
    }
    g_array_unref(blocks);
}

void
on_note_view_action_marks_sync(OnNoteView *v, GList *items)
{
    GArray *blocks = on_document_action_blocks(v->doc);
    guint i = 0;
    for (GList *l = items; l != NULL && i < blocks->len; l = l->next, i++)
        on_document_block(v->doc, g_array_index(blocks, guint, i))->action_uid =
            ((OnActionItem *)l->data)->uid;
    g_array_unref(blocks);
}

gboolean
on_note_view_action_strike(OnNoteView *v, gint ord, gboolean done)
{
    typing_end(v);
    if (!on_document_action_strike(v->doc, ord, done))
        return FALSE;
    after_edit(v);
    return TRUE;
}

gboolean
on_note_view_action_due(OnNoteView *v, gint ord, gint64 due)
{
    typing_end(v);
    if (!on_document_action_due(v->doc, ord, due))
        return FALSE;
    after_edit(v);
    return TRUE;
}

gboolean
on_note_view_action_text(OnNoteView *v, gint ord, const gchar *text)
{
    typing_end(v);
    if (!on_document_action_text(v->doc, ord, text))
        return FALSE;
    after_edit(v);
    return TRUE;
}

/* ===========================================================================
 * accessibility — GtkAccessibleText over the note's plain text
 *
 * Offsets are CHARACTERS into on_document_plain_text() (one line per
 * block); the caret and selection are mapped onto that string.
 * ======================================================================== */

/* a11y_offset_of() — the character offset of a position in the plain text. */
static guint
a11y_offset_of(OnNoteView *v, OnPos p)
{
    guint off = 0;
    for (guint i = 0; i < p.block && i < on_document_n_blocks(v->doc); i++) {
        OnBlock *b = on_document_block(v->doc, i);
        if (b->text != NULL)
            off += (guint)g_utf8_strlen(b->text->text->str, -1);
        off++;                       /* the line break                      */
    }
    OnBlock *b = on_document_block(v->doc, p.block);
    if (b != NULL && b->text != NULL)
        off += (guint)g_utf8_strlen(b->text->text->str, (gssize)p.offset);
    return off;
}

static GBytes *
a11y_get_contents(GtkAccessibleText *self, guint start, guint end)
{
    OnNoteView *v = ON_NOTE_VIEW(self);
    gchar *text = on_document_plain_text(v->doc);
    glong len = g_utf8_strlen(text, -1);
    if (end == G_MAXUINT || end > (guint)len)
        end = (guint)len;
    if (start > end)
        start = end;
    gchar *part = g_utf8_substring(text, start, end);
    g_free(text);
    return g_bytes_new_take(part, strlen(part) + 1);
}

static GBytes *
a11y_get_contents_at(GtkAccessibleText *self, guint offset,
                     GtkAccessibleTextGranularity granularity, guint *start,
                     guint *end)
{
    (void)granularity;
    *start = offset;
    *end = offset + 1;
    return a11y_get_contents(self, offset, offset + 1);
}

static guint
a11y_get_caret_position(GtkAccessibleText *self)
{
    OnNoteView *v = ON_NOTE_VIEW(self);
    return a11y_offset_of(v, v->caret);
}

static gboolean
a11y_get_selection(GtkAccessibleText *self, gsize *n_ranges,
                   GtkAccessibleTextRange **ranges)
{
    OnNoteView *v = ON_NOTE_VIEW(self);
    if (!has_selection(v)) {
        *n_ranges = 0;
        return FALSE;
    }
    OnPos a, b;
    sel_bounds(v, &a, &b);
    *ranges = g_new(GtkAccessibleTextRange, 1);
    (*ranges)[0].start  = a11y_offset_of(v, a);
    (*ranges)[0].length = a11y_offset_of(v, b) - (*ranges)[0].start;
    *n_ranges = 1;
    return TRUE;
}

static gboolean
a11y_get_attributes(GtkAccessibleText *self, guint offset, gsize *n_ranges,
                    GtkAccessibleTextRange **ranges, gchar ***names,
                    gchar ***values)
{
    (void)self; (void)offset; (void)ranges;
    *n_ranges = 0;
    *names = NULL;
    *values = NULL;
    return FALSE;
}

static void
a11y_get_default_attributes(GtkAccessibleText *self, gchar ***names,
                            gchar ***values)
{
    (void)self;
    *names = NULL;
    *values = NULL;
}

static void
note_view_accessible_text_init(GtkAccessibleTextInterface *iface)
{
    iface->get_contents           = a11y_get_contents;
    iface->get_contents_at        = a11y_get_contents_at;
    iface->get_caret_position     = a11y_get_caret_position;
    iface->get_selection          = a11y_get_selection;
    iface->get_attributes         = a11y_get_attributes;
    iface->get_default_attributes = a11y_get_default_attributes;
}

/* ===========================================================================
 * construction, loading and teardown
 * ======================================================================== */

static void
on_note_view_init(OnNoteView *v)
{
    GtkWidget *w = GTK_WIDGET(v);
    v->doc = on_document_new();
    on_document_set_observer(v->doc, &OBSERVER, v);
    v->layout = on_doc_layout_new(v->doc, gtk_widget_get_pango_context(w));
    v->caret.cell = v->anchor.cell = -1;
    v->goal_x = -1;
    v->fresh = TRUE;
    v->hadj = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    v->vadj = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    g_object_ref_sink(v->hadj);
    g_object_ref_sink(v->vadj);

    gtk_widget_set_focusable(w, TRUE);
    gtk_widget_set_can_focus(w, TRUE);
    gtk_widget_set_overflow(w, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_cursor_from_name(w, "text");
    gtk_widget_set_hexpand(w, TRUE);
    gtk_widget_set_vexpand(w, TRUE);

    /* The input method: every key goes through it first.  Its client
     * widget is set on realize (see note_view_realize).                    */
    v->im = gtk_im_multicontext_new();
    g_signal_connect(v->im, "commit", G_CALLBACK(on_im_commit), v);
    g_signal_connect(v->im, "preedit-changed",
                     G_CALLBACK(on_im_preedit_changed), v);
    g_signal_connect(v->im, "retrieve-surrounding",
                     G_CALLBACK(on_im_retrieve_surrounding), v);
    g_signal_connect(v->im, "delete-surrounding",
                     G_CALLBACK(on_im_delete_surrounding), v);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(keys),
                                            v->im);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), v);
    gtk_widget_add_controller(w, keys);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), v);
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), v);
    gtk_widget_add_controller(w, focus);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), v);
    g_signal_connect(click, "released", G_CALLBACK(on_released), v);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    gtk_widget_add_controller(w, motion);

    install_actions(v);
}

static void
note_view_dispose(GObject *object)
{
    OnNoteView *v = ON_NOTE_VIEW(object);
    typing_end(v);
    blink_set(v, FALSE);
    if (v->tag_popup != NULL) {
        gtk_widget_unparent(v->tag_popup);
        v->tag_popup = NULL;
        v->tag_listbox = NULL;
    }
    if (v->emoji_chooser != NULL) {
        gtk_widget_unparent(v->emoji_chooser);
        v->emoji_chooser = NULL;
    }
    g_clear_object(&v->im);
    g_clear_object(&v->actions);
    G_OBJECT_CLASS(on_note_view_parent_class)->dispose(object);
}

static void
note_view_finalize(GObject *object)
{
    OnNoteView *v = ON_NOTE_VIEW(object);
    on_doc_layout_free(v->layout);
    on_document_free(v->doc);
    on_db_tag_list_free(v->tag_choices);
    g_free(v->preedit);
    g_free(v->find_text);
    g_clear_object(&v->hadj);
    g_clear_object(&v->vadj);
    G_OBJECT_CLASS(on_note_view_parent_class)->finalize(object);
}

static void
on_note_view_class_init(OnNoteViewClass *klass)
{
    GObjectClass   *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    oc->set_property = note_view_set_property;
    oc->get_property = note_view_get_property;
    oc->dispose      = note_view_dispose;
    oc->finalize     = note_view_finalize;
    wc->measure       = note_view_measure;
    wc->size_allocate = note_view_size_allocate;
    wc->snapshot      = note_view_snapshot;
    wc->realize       = note_view_realize;
    wc->unrealize     = note_view_unrealize;
    g_object_class_override_property(oc, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(oc, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(oc, PROP_HSCROLL_POLICY,
                                     "hscroll-policy");
    g_object_class_override_property(oc, PROP_VSCROLL_POLICY,
                                     "vscroll-policy");
    gtk_widget_class_set_css_name(wc, "notesview");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_TEXT_BOX);

    signals[SIG_EDITED] = g_signal_new(
        "edited", ON_TYPE_NOTE_VIEW, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
    signals[SIG_INLINE_FLAGS_CHANGED] = g_signal_new(
        "inline-flags-changed", ON_TYPE_NOTE_VIEW, G_SIGNAL_RUN_LAST, 0,
        NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIG_IMAGE_ACTIVATED] = g_signal_new(
        "image-activated", ON_TYPE_NOTE_VIEW, G_SIGNAL_RUN_LAST, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

GtkWidget *
on_note_view_new(OnApp *app)
{
    OnNoteView *v = g_object_new(ON_TYPE_NOTE_VIEW, NULL);
    v->app = app;
    style_from_app(v);
    return GTK_WIDGET(v);
}

void
on_note_view_load(OnNoteView *v, const guint8 *blob, gsize len)
{
    if (v->tag_capturing)
        tag_capture_end(v, FALSE);
    typing_end(v);
    im_reset(v);
    OnDocument *doc = on_document_from_bnbf(blob, len, NULL);
    on_document_set_observer(v->doc, NULL, NULL);
    on_document_free(v->doc);
    v->doc = doc;
    on_document_set_observer(v->doc, &OBSERVER, v);
    on_doc_layout_set_document(v->layout, v->doc);
    /* The caret starts on the first line of TEXT: under the first-line-
     * title setting, block 0 is the note's title and the body begins at
     * block 1 — an existing note opens ready to be read or added to, not
     * to be renamed.  A blank note (one empty block) and the setting off
     * both start at 0,0; clamp_pos keeps a one-block note in range.     */
    OnPos start = { 0, -1, 0 };
    if (v->app->first_line_title && on_document_n_blocks(v->doc) > 1)
        start.block = 1;
    v->caret = v->anchor = clamp_pos(v, start);
    v->goal_x = -1;
    v->inline_flags = 0;
    v->fresh = TRUE;
    on_document_take_tags_modified(v->doc);
    on_document_take_actions_modified(v->doc);
    if (v->find_text != NULL)
        on_note_view_find(v, v->find_text);
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

guint8 *
on_note_view_serialize(OnNoteView *v, gsize *out_len)
{
    return on_document_to_bnbf(v->doc, out_len);
}

gchar *
on_note_view_first_line(OnNoteView *v)
{
    return on_document_title(v->doc, ON_DEFAULT_NOTE_TITLE, ON_TITLE_MAX_CHARS);
}

gboolean
on_note_view_is_blank(OnNoteView *v)
{
    for (guint i = 0; i < on_document_n_blocks(v->doc); i++) {
        OnBlock *b = on_document_block(v->doc, i);
        if (b->text == NULL || b->kind == ON_BLOCK_CHECK ||
            b->text->images->len > 0)
            return FALSE;
        for (const gchar *p = b->text->text->str; *p != '\0';
             p = g_utf8_next_char(p))
            if (!g_unichar_isspace(g_utf8_get_char(p)))
                return FALSE;
    }
    return TRUE;
}

GList *
on_note_view_collect_tags(OnNoteView *v)
{
    return on_document_collect_tags(v->doc);
}

void
on_note_view_settings_changed(OnNoteView *v)
{
    style_from_app(v);
    gtk_widget_queue_resize(GTK_WIDGET(v));
}

gboolean
on_note_view_take_tags_modified(OnNoteView *v)
{
    return on_document_take_tags_modified(v->doc);
}

gboolean
on_note_view_take_actions_modified(OnNoteView *v)
{
    gboolean fresh = v->fresh;
    v->fresh = FALSE;
    return on_document_take_actions_modified(v->doc) || fresh;
}

/* ===========================================================================
 * the keyboard as function calls (see note_view.h)
 * ======================================================================== */

gboolean
on_note_view_feed_key(OnNoteView *v, guint keyval, GdkModifierType state)
{
    return on_key_pressed(NULL, keyval, 0, state, v);
}

gboolean
on_note_view_feed_click(OnNoteView *v, gdouble x, gdouble y, gint n_press,
                        GdkModifierType state, guint button)
{
    return press_at(v, x, y, n_press, state, button,
                    button == GDK_BUTTON_SECONDARY);
}

void
on_note_view_feed_text(OnNoteView *v, const gchar *text)
{
    on_im_commit(v->im, text, v);
}

OnPos
on_note_view_caret(OnNoteView *v)
{
    return v->caret;
}

OnDocument *
on_note_view_document(OnNoteView *v)
{
    return v->doc;
}
