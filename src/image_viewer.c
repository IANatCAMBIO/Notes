/* ===========================================================================
 * image_viewer.c — the modal image viewer panel (implementation)
 *
 * See image_viewer.h for the layout and the host contract.  The mechanics
 * worth knowing before changing anything here:
 *
 *   sizing   — the image is fitted to the OVERLAY's allocation, not the
 *              panel's: on the first open the panel has not been allocated
 *              yet, while the overlay already has.  Re-fitting after a
 *              resize is debounced (IMG_RESIZE_MS), because a drag-resize
 *              fires size-allocate continuously and every re-fit decodes the
 *              picture again.
 *
 *   focus    — the panel NEVER takes the keyboard focus, and nothing inside
 *              it is focusable.  This is not a detail: a GtkTextView that
 *              loses focus renders its text in the unfocused (grey) colour,
 *              and on quartz that grey rendering SURVIVES the panel closing
 *              — GTK reports the focus restored and redraws black at full
 *              clip, yet the screen keeps the grey until the toplevel is
 *              re-activated.  Nothing at the GTK level clears it: not
 *              gtk_widget_queue_draw, not gdk_window_invalidate_rect with
 *              children, not gtk_widget_reset_style on the view or on the
 *              window; only scrolling (which re-lays-out lines) or clicking
 *              to another window and back.  So the cure is to never create
 *              the grey rendering: the focus stays where it was, and the
 *              panel's keys are served from the HOST WINDOW's
 *              key-press-event handler (which runs before GtkWindow forwards
 *              the key to the focus widget), not from the panel.  A host must
 *              swallow every other key while the panel is up — see
 *              on_image_viewer_key_press.
 *
 *   clicks   — the backdrop is a visible-window GtkEventBox, so it swallows
 *              every press meant for the widget behind it and a click
 *              anywhere on it closes the panel.  Its "links" are therefore
 *              PLAIN LABELS hit-tested by allocation from img_press, never
 *              GtkLabel `<a>` links: a GtkLabel carrying links owns an
 *              input-only window and grabs the focus when clicked, which is
 *              exactly what the focus rule above forbids.  Hit-testing also
 *              fixes what the links got wrong — a press landing in the gaps
 *              inside the row (spacing, the separator, past the end of a
 *              word) silently did nothing, so stepping sometimes took two
 *              clicks.  The same trick, and the same reason, as the editor's
 *              code-block copy links (quirk #22) — the hover cursor
 *              included, which img_motion serves from the panel's own
 *              window (the panel really does own one, so unlike the
 *              editor's text window there is nobody to fight over it).
 *
 *   walking  — Left/Right and the Previous | Next row all go through
 *              img_step(), which asks the host's count() every time.  There
 *              is no wrap-around: at either end the word is dim and inert.
 *              The row is split by MIDLINE, not by word: the left half steps
 *              back and the right half forward, so there is no dead gap
 *              between two small words.
 * =========================================================================== */

#include "image_viewer.h"

/* Height reserved under the image for the caption + action-link row, and for
 * the Previous | Next row under that (logical px).                          */
#define IMG_CAPTION_H 30
#define IMG_NAV_H     24

/* Settle time before an open panel re-fits for a new host size (ms).        */
#define IMG_RESIZE_MS 150

/* Style classes (see img_css_install): the backdrop, a clickable word, and
 * a word that leads nowhere from the picture on show.                       */
#define IMG_CSS_VIEWER "on-image-viewer"
#define IMG_CSS_LINK   "on-image-viewer-link"
#define IMG_CSS_DIM    "on-image-viewer-dim"

/* ---------------------------------------------------------------------------
 * OnImageViewer — all state for one panel.
 *
 * Fields:
 *   ops      — the host's callbacks (borrowed).
 *   host     — handed back to every op.
 *   overlay  — the GtkOverlay the panel sits in; its allocation is what the
 *              image is fitted to (not owned).
 *   panel    — the dark event box; hidden whenever nothing is on show.
 *   image    — the GtkImage inside it.
 *   caption  — the caption label, bottom left.
 *   link     — the action label, bottom right; NULL when the host asked for
 *              none.
 *   nav      — the "Previous | Next" row (a GtkBox), hit-tested by midline.
 *   nav_prev/nav_next — the two words, restyled per picture by
 *              on_image_viewer_nav_sync; only their LOOK lives here, the
 *              clicking is img_press's business.
 *   idx      — the image on show, or -1 when the panel is closed.  This IS
 *              the open/closed flag.
 *   hand_cursor — whether the panel's window currently shows the "pointer"
 *              cursor, so plain motion over the backdrop does not build a
 *              GdkCursor per event.
 *   resize   — pending re-fit timer after a host resize, 0 if none.
 *   fit_w/fit_h — the box the picture on show was last fitted to, so a
 *              size-allocate that changed nothing costs nothing.
 * ------------------------------------------------------------------------- */
struct OnImageViewer {
    const OnImageViewerOps *ops;
    gpointer                host;
    GtkWidget              *overlay;
    GtkWidget              *panel;
    GtkWidget              *image;
    GtkWidget              *caption;
    GtkWidget              *link;
    GtkWidget              *nav;
    GtkWidget              *nav_prev;
    GtkWidget              *nav_next;
    gint                    idx;
    gboolean                hand_cursor;
    guint                   resize;
    gint                    fit_w;
    gint                    fit_h;
};

/* ===========================================================================
 * small helpers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * img_css_install() — install the backdrop styling once per process, scoped
 * to IMG_CSS_VIEWER so nothing else on screen is affected.
 * ------------------------------------------------------------------------- */
static void
img_css_install(GtkWidget *ref)
{
    static gboolean done = FALSE;    /* one provider per process            */
    if (done)
        return;
    done = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "." IMG_CSS_VIEWER " {"
        "  background-color: alpha(#101010, 0.88);"
        "}"
        "." IMG_CSS_VIEWER " label {"
        "  color: #f2f2f2;"
        "}"
        /* The clickable words are PLAIN labels (see the file header), so
         * they are given the link look by class rather than by :link, and
         * AFTER the blanket label colour above.                            */
        "." IMG_CSS_VIEWER " label." IMG_CSS_LINK " {"
        "  color: #8ab4f8;"
        "  text-decoration-line: underline;"
        "}"
        "." IMG_CSS_VIEWER " label." IMG_CSS_DIM " {"
        "  color: alpha(#f2f2f2, 0.40);"
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gtk_widget_get_screen(ref), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* img_label_small() — give a label the small font the two strips use.       */
static void
img_label_small(GtkWidget *label)
{
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(label), attrs);
    pango_attr_list_unref(attrs);
}

/* ---------------------------------------------------------------------------
 * img_link_look() — make a label look clickable, or look like a dead end.
 * Purely cosmetic: what a click DOES is img_press's business.
 * ------------------------------------------------------------------------- */
static void
img_link_look(GtkWidget *label, gboolean live)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(label);
    gtk_style_context_remove_class(ctx, live ? IMG_CSS_DIM : IMG_CSS_LINK);
    gtk_style_context_add_class(ctx, live ? IMG_CSS_LINK : IMG_CSS_DIM);
}

/* ---------------------------------------------------------------------------
 * img_cursor() — show the hand ("pointer") cursor over the panel's clickable
 * words, and the plain inherited one everywhere else.  Scoped to the PANEL's
 * own GdkWindow, which it really does own (a visible-window GtkEventBox), so
 * unlike the editor's text window (quirk #22) there is no other owner to
 * fight with.  NULL makes the window inherit its parent's cursor again.
 * ------------------------------------------------------------------------- */
static void
img_cursor(OnImageViewer *v, gboolean hand)
{
    if (v->hand_cursor == hand)
        return;
    GdkWindow *win = gtk_widget_get_window(v->panel);
    if (win == NULL)
        return;

    GdkCursor *cursor = hand
        ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer")
        : NULL;
    gdk_window_set_cursor(win, cursor);
    if (cursor != NULL)
        g_object_unref(cursor);
    v->hand_cursor = hand;
}

/* ---------------------------------------------------------------------------
 * img_hit() — does a press on the panel land on this child?
 * Both the event and the child allocations are in the panel event box's
 * window coordinates (a visible-window GtkEventBox allocates its child at
 * its own border width), so they compare directly.  A missing or hidden
 * child is never hit, whatever allocation it is still carrying.
 * ------------------------------------------------------------------------- */
static gboolean
img_hit(GtkWidget *child, const GdkEventButton *event)
{
    if (child == NULL || !gtk_widget_get_visible(child))
        return FALSE;

    GtkAllocation a;                 /* where the child sits                */
    gtk_widget_get_allocation(child, &a);
    return event->x >= a.x && event->x < a.x + a.width &&
           event->y >= a.y && event->y < a.y + a.height;
}

cairo_surface_t *
on_image_viewer_fit(GtkWidget *ref, GdkPixbuf *pix, gint box_w, gint box_h)
{
    gint sf = gtk_widget_get_scale_factor(ref);
    gint iw = gdk_pixbuf_get_width(pix);
    gint ih = gdk_pixbuf_get_height(pix);
    if (iw <= 0 || ih <= 0)
        return NULL;

    /* Physical pixels the box holds; never upscale past the source.        */
    gdouble scale = MIN((gdouble)(box_w * sf) / iw,
                        (gdouble)(box_h * sf) / ih);
    scale = MIN(scale, 1.0);
    gint pw = MAX(1, (gint)(iw * scale));
    gint ph = MAX(1, (gint)(ih * scale));

    GdkPixbuf *backing =             /* pixels actually handed to cairo     */
        (pw < iw || ph < ih) ? gdk_pixbuf_scale_simple(pix, pw, ph,
                                                       GDK_INTERP_BILINEAR)
                             : g_object_ref(pix);
    cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(
        backing, sf, gtk_widget_get_window(ref));
    g_object_unref(backing);
    return surface;
}

/* ===========================================================================
 * showing, walking, closing
 * =========================================================================== */

void
on_image_viewer_close(OnImageViewer *v)
{
    if (v == NULL || v->idx < 0)
        return;
    if (v->resize != 0) {
        g_source_remove(v->resize);
        v->resize = 0;
    }
    v->idx   = -1;
    v->fit_w = 0;
    v->fit_h = 0;
    gtk_image_clear(GTK_IMAGE(v->image));
    img_cursor(v, FALSE);            /* before the window goes: no stuck hand */
    gtk_widget_hide(v->panel);
    /* No focus to hand back: the panel never took it (see the file header). */
}

gboolean
on_image_viewer_is_open(const OnImageViewer *v)
{
    return v != NULL && v->idx >= 0;
}

gint
on_image_viewer_index(const OnImageViewer *v)
{
    return (v != NULL) ? v->idx : -1;
}

void
on_image_viewer_nav_sync(OnImageViewer *v)
{
    if (v == NULL || v->idx < 0)
        return;

    gint n = v->ops->count(v->host);  /* images to walk, right now          */
    if (n <= 1) {
        gtk_widget_hide(v->nav);
        return;
    }

    /* At either end the word STAYS PUT, dimmed: the row never changes width
     * under the pointer, and a dead end is visible instead of silent.  Only
     * the LOOK changes here — img_press decides what a click does, from the
     * same count(), so the two can never disagree.                         */
    img_link_look(v->nav_prev, v->idx > 0);
    img_link_look(v->nav_next, v->idx + 1 < n);
    gtk_widget_show(v->nav);
}

/* ---------------------------------------------------------------------------
 * img_render() — (re)fit the picture on show to the overlay's current size:
 * the whole client area inset by ON_IMAGE_VIEWER_INSET on every side, less
 * the two label rows.  Closes the panel when the host can no longer produce
 * the image.
 * ------------------------------------------------------------------------- */
static void
img_render(OnImageViewer *v)
{
    if (v->idx < 0)
        return;

    GtkAllocation alloc;             /* the overlay's client area           */
    gtk_widget_get_allocation(v->overlay, &alloc);
    gint box_w = alloc.width  - 2 * ON_IMAGE_VIEWER_INSET;
    gint box_h = alloc.height - 2 * ON_IMAGE_VIEWER_INSET
                              - IMG_CAPTION_H - IMG_NAV_H;
    /* A window shrunk to almost nothing still shows something rather than
     * an empty panel.                                                      */
    box_w = MAX(box_w, 32);
    box_h = MAX(box_h, 32);

    cairo_surface_t *big = v->ops->render(v->host, v->idx, box_w, box_h);
    if (big == NULL) {
        on_image_viewer_close(v);
        return;
    }
    v->fit_w = box_w;
    v->fit_h = box_h;
    gtk_image_set_from_surface(GTK_IMAGE(v->image), big);
    cairo_surface_destroy(big);       /* the GtkImage holds its own ref      */
}

/* img_resize_done() — the host has stopped changing size: re-fit.           */
static gboolean
img_resize_done(gpointer user_data)
{
    OnImageViewer *v = user_data;    /* the panel                           */
    v->resize = 0;
    img_render(v);
    return G_SOURCE_REMOVE;
}

void
on_image_viewer_open(OnImageViewer *v, gint idx)
{
    if (v == NULL || idx < 0 || idx >= v->ops->count(v->host))
        return;
    v->idx = idx;

    /* The host describes the picture; the key hint is the panel's own, and
     * the arrows are only mentioned when there is somewhere to go.  The hint
     * names ESC rather than the click: a click closes it too, but saying so
     * invites clicking, and a click aimed at the nav row or the action link
     * is the one that must NOT close it.                                   */
    gchar *what = v->ops->caption(v->host, idx);
    gchar *cap  = g_strdup_printf(
        "%s      %sESC to close", (what != NULL) ? what : "",
        (v->ops->count(v->host) > 1)
            ? "\xe2\x86\x90 \xe2\x86\x92 to move, " : "");
    gtk_label_set_text(GTK_LABEL(v->caption), cap);
    g_free(cap);
    g_free(what);
    on_image_viewer_nav_sync(v);

    gtk_widget_show(v->panel);
    /* Rendering needs an allocation, which the show above has not produced
     * yet on the first open; the size we fit FROM is the OVERLAY's, which
     * is already allocated, so render straight away.                       */
    img_render(v);
    /* Deliberately NO grab_focus: the focus stays exactly where the host
     * left it, so nothing behind the panel ever renders unfocused.  The
     * keys arrive through the host window's handler instead.                */
}

/* ---------------------------------------------------------------------------
 * img_step() — move the open panel `delta` images (-1 = previous, +1 = next).
 * A step past either end does nothing: the panel is never closed by
 * navigating, and it never wraps around — with hundreds of images, jumping
 * from the last picture to the first reads as a glitch rather than a move.
 * ------------------------------------------------------------------------- */
static void
img_step(OnImageViewer *v, gint delta)
{
    if (v->idx < 0)
        return;
    gint i = v->idx + delta;         /* where the step lands                */
    if (i < 0 || i >= v->ops->count(v->host))
        return;
    on_image_viewer_open(v, i);
}

gboolean
on_image_viewer_key_press(OnImageViewer *v, GdkEventKey *event)
{
    if (v == NULL || v->idx < 0)
        return FALSE;

    switch (event->keyval) {
    case GDK_KEY_Escape:
        on_image_viewer_close(v);
        return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        img_step(v, -1);
        return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        img_step(v, +1);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ===========================================================================
 * signal handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * img_nav_delta() — what a position inside the Previous | Next row means.
 * Resolved by MIDLINE rather than by which word was hit: the two words are
 * small and the row has spacing and a separator between them, so word-exact
 * hit-testing left gaps where a click did nothing and stepping took two
 * tries.  Left half means back, right half forward.
 *   x        — position in the panel's window coordinates.
 *   live_out — filled with whether a step that way leads anywhere, so the
 *              press stays inert and the cursor stays plain on a dead end.
 * Returns -1 (back) or +1 (forward).
 *
 * THE one reading of the row: the press handler and the hover cursor both
 * come here, so what the pointer says can never disagree with what a click
 * does.
 * ------------------------------------------------------------------------- */
static gint
img_nav_delta(OnImageViewer *v, gdouble x, gboolean *live_out)
{
    GtkAllocation a;                 /* the row                             */
    gtk_widget_get_allocation(v->nav, &a);
    gint delta = (x < a.x + a.width / 2.0) ? -1 : +1;
    if (live_out != NULL)
        *live_out = (delta < 0) ? (v->idx > 0)
                                : (v->idx + 1 < v->ops->count(v->host));
    return delta;
}

/* img_nav_press() — a press in the row: step, unless that way is a dead end.
 * Returns nothing; the caller has already claimed the press, so a near-miss
 * never dismisses the picture.                                              */
static void
img_nav_press(OnImageViewer *v, const GdkEventButton *event)
{
    gboolean live;                   /* does that way lead anywhere?        */
    gint delta = img_nav_delta(v, event->x, &live);
    if (live)
        img_step(v, delta);
}

/* ---------------------------------------------------------------------------
 * img_action_press() — the host's action label.  The panel closes FIRST, so
 * the host may do anything at all from here (open a window, tear down the
 * host widget) without the panel touching its own state afterwards.
 * ------------------------------------------------------------------------- */
static void
img_action_press(OnImageViewer *v)
{
    gint idx = v->idx;               /* copied: closing clears it           */
    if (idx < 0)
        return;
    on_image_viewer_close(v);
    v->ops->action(v->host, idx);
}

/* ---------------------------------------------------------------------------
 * img_press() — a click anywhere on the panel closes it, except on the link
 * labels (see the file header for why the geometry is checked).  Only the
 * plain press acts: a stray GDK_2BUTTON_PRESS (from someone double-clicking
 * out of habit) must not close a panel the first press has already closed
 * and the host re-opened.
 * ------------------------------------------------------------------------- */
static gboolean
img_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)widget;
    OnImageViewer *v = user_data;    /* the panel                           */
    if (event->button != GDK_BUTTON_PRIMARY ||
        event->type   != GDK_BUTTON_PRESS)
        return FALSE;
    /* The labels have no input windows of their own any more, so every
     * press lands here and this is the ONE place that decides.             */
    if (img_hit(v->nav, event)) {
        img_nav_press(v, event);
        return TRUE;
    }
    if (img_hit(v->link, event)) {
        img_action_press(v);
        return TRUE;
    }
    on_image_viewer_close(v);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * img_motion() — hand cursor over the action label and over the LIVE half of
 * the Previous | Next row.  Hit-tested exactly like the press (img_hit +
 * img_nav_delta), so the pointer promises precisely what a click delivers:
 * no hand over a dimmed dead end.
 *
 * The panel needs GDK_POINTER_MOTION_MASK added explicitly — a GtkEventBox
 * asks for BUTTON_MOTION only (quirk #22), so without it this never fires.
 * ------------------------------------------------------------------------- */
static gboolean
img_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    (void)widget;
    OnImageViewer *v = user_data;    /* the panel                           */
    gboolean hand = FALSE;           /* is the pointer over a target?       */

    if (v->link != NULL && gtk_widget_get_visible(v->link)) {
        GtkAllocation a;             /* the action label                    */
        gtk_widget_get_allocation(v->link, &a);
        hand = event->x >= a.x && event->x < a.x + a.width &&
               event->y >= a.y && event->y < a.y + a.height;
    }
    if (!hand && gtk_widget_get_visible(v->nav)) {
        GtkAllocation a;             /* the nav row                         */
        gtk_widget_get_allocation(v->nav, &a);
        if (event->x >= a.x && event->x < a.x + a.width &&
            event->y >= a.y && event->y < a.y + a.height)
            img_nav_delta(v, event->x, &hand);
    }
    img_cursor(v, hand);
    return FALSE;                    /* never consume: default handling     */
}

/* img_scroll() — swallow the wheel over the panel, so the widget hidden
 * behind it cannot be scrolled out from under it.                           */
static gboolean
img_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
    (void)widget; (void)event; (void)user_data;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * img_overlay_allocate() — the host was resized: re-fit the open picture,
 * once the resizing has settled.  Connected here rather than left to the
 * host, so neither host has to wire a configure-event to the panel.
 *
 * The box comparison is what makes this cheap: size-allocate fires for all
 * sorts of reasons, and only a change to the box the picture was fitted to
 * is worth decoding it again.  There is no feedback loop to fear — GtkOverlay
 * measures only its MAIN child, so the fitted image never feeds back into
 * the overlay's own allocation.
 * ------------------------------------------------------------------------- */
static void
img_overlay_allocate(GtkWidget *widget, GdkRectangle *alloc,
                     gpointer user_data)
{
    (void)widget;
    OnImageViewer *v = user_data;    /* the panel                           */
    if (v->idx < 0)
        return;

    gint box_w = alloc->width  - 2 * ON_IMAGE_VIEWER_INSET;
    gint box_h = alloc->height - 2 * ON_IMAGE_VIEWER_INSET
                               - IMG_CAPTION_H - IMG_NAV_H;
    if (MAX(box_w, 32) == v->fit_w && MAX(box_h, 32) == v->fit_h)
        return;

    if (v->resize != 0)
        g_source_remove(v->resize);
    v->resize = g_timeout_add(IMG_RESIZE_MS, img_resize_done, v);
}

/* ===========================================================================
 * construction
 * =========================================================================== */

OnImageViewer *
on_image_viewer_new(GtkWidget *overlay, const OnImageViewerOps *ops,
                    gpointer host, const gchar *action_label,
                    const gchar *action_tip)
{
    OnImageViewer *v = g_new0(OnImageViewer, 1);
    v->ops     = ops;
    v->host    = host;
    v->overlay = overlay;
    v->idx     = -1;

    img_css_install(overlay);

    v->image = gtk_image_new();

    v->caption = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(v->caption), PANGO_ELLIPSIZE_END);
    /* Bounded natural width (ellipsize makes max-width-chars the cap), so a
     * long description cannot make the strip wider than the picture.       */
    gtk_label_set_max_width_chars(GTK_LABEL(v->caption), 60);
    gtk_label_set_xalign(GTK_LABEL(v->caption), 0.0);
    gtk_widget_set_hexpand(v->caption, TRUE);
    img_label_small(v->caption);

    /* A plain label wearing the link LOOK, clicked via img_press.  Not a
     * GtkLabel `<a>` link: that owns an input-only window and grabs the
     * focus when clicked, which the focus rule in the file header forbids.  */
    if (action_label != NULL && ops->action != NULL) {
        v->link = gtk_label_new(action_label);
        gtk_widget_set_halign(v->link, GTK_ALIGN_END);
        if (action_tip != NULL)
            gtk_widget_set_tooltip_text(v->link, action_tip);
        img_label_small(v->link);
        img_link_look(v->link, TRUE);
    }

    /* The "Previous | Next" row: a BOX of three plain labels.  It is claimed
     * and split as a whole by img_press, so the words need no input windows
     * of their own and there are no gaps between them where a click would be
     * swallowed.  Its visibility belongs to on_image_viewer_nav_sync, hence
     * no-show-all.                                                         */
    v->nav_prev = gtk_label_new("Previous");
    v->nav_next = gtk_label_new("Next");
    GtkWidget *nav_sep = gtk_label_new("|");
    img_label_small(v->nav_prev);
    img_label_small(v->nav_next);
    img_label_small(nav_sep);
    img_link_look(v->nav_prev, TRUE);
    img_link_look(v->nav_next, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(nav_sep),
                                IMG_CSS_DIM);

    v->nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(v->nav), v->nav_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v->nav), nav_sep,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v->nav), v->nav_next, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(v->nav, TRUE);
    gtk_widget_set_halign(v->nav, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(v->nav,
        "Show the previous or next image (or press the \xe2\x86\x90 and "
        "\xe2\x86\x92 keys)");
    /* show_all(grid) below would reveal the row; its own no-show-all keeps
     * that from happening, but the CHILDREN must be shown explicitly or the
     * box would come up empty when nav_sync shows it.                      */
    gtk_widget_show_all(v->nav_prev);
    gtk_widget_show_all(nav_sep);
    gtk_widget_show_all(v->nav_next);

    /* A GtkGrid, not a box: the image spans both columns, so the grid is
     * exactly as wide as the picture and the right-aligned action link
     * lands under the picture's bottom-right corner.  The caption takes the
     * slack (hexpand), and the grid blocks that expand flag from
     * propagating outwards — otherwise the grid would stretch to the whole
     * window and take the link with it.                                    */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(grid, FALSE);   /* stop the caption's expand here */
    gtk_widget_set_margin_start(grid,  ON_IMAGE_VIEWER_INSET);
    gtk_widget_set_margin_end(grid,    ON_IMAGE_VIEWER_INSET);
    gtk_widget_set_margin_top(grid,    ON_IMAGE_VIEWER_INSET);
    gtk_widget_set_margin_bottom(grid, ON_IMAGE_VIEWER_INSET);
    gtk_grid_attach(GTK_GRID(grid), v->image,   0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), v->caption, 0, 1, 1, 1);
    if (v->link != NULL)
        gtk_grid_attach(GTK_GRID(grid), v->link, 1, 1, 1, 1);
    /* Spanning both columns, so the row is as wide as the picture above it
     * and GTK_ALIGN_CENTER puts the links under the picture's middle.      */
    gtk_grid_attach(GTK_GRID(grid), v->nav,     0, 2, 2, 1);

    v->panel = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(v->panel), TRUE);
    gtk_widget_set_no_show_all(v->panel, TRUE);
    /* NOT focusable, on purpose — see the focus rule in the file header.    */
    gtk_widget_set_can_focus(v->panel, FALSE);
    gtk_container_add(GTK_CONTAINER(v->panel), grid);
    gtk_style_context_add_class(gtk_widget_get_style_context(v->panel),
                                IMG_CSS_VIEWER);
    gtk_widget_add_events(v->panel, GDK_POINTER_MOTION_MASK);
    g_signal_connect(v->panel, "button-press-event",
                     G_CALLBACK(img_press), v);
    g_signal_connect(v->panel, "motion-notify-event",
                     G_CALLBACK(img_motion), v);
    g_signal_connect(v->panel, "scroll-event", G_CALLBACK(img_scroll), v);
    gtk_widget_show_all(grid);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), v->panel);
    g_signal_connect(overlay, "size-allocate",
                     G_CALLBACK(img_overlay_allocate), v);
    return v;
}

void
on_image_viewer_free(OnImageViewer *v)
{
    if (v == NULL)
        return;
    if (v->resize != 0)
        g_source_remove(v->resize);
    /* The overlay may outlive this call by a moment (a host frees the panel
     * from its window's "destroy", which runs before the children are torn
     * down), and its size-allocate must not reach freed memory.            */
    g_signal_handlers_disconnect_by_data(v->overlay, v);
    g_free(v);
}
