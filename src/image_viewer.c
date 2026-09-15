/* ===========================================================================
 * image_viewer.c — the modal image viewer panel (implementation)
 *
 * See image_viewer.h for the layout and the host contract.  The mechanics
 * worth knowing before changing anything here:
 *
 *   sizing   — the host hands over ONE full-resolution paintable and the
 *              panel never touches its pixels: img_fit_paintable wraps it in
 *              a render-node paintable whose intrinsic size is the fit to the
 *              OVERLAY's current size (inset by ON_IMAGE_VIEWER_INSET, less
 *              the two label rows), so the GtkPicture's natural size IS the
 *              drawn size and the caption/link row under it is exactly as
 *              wide as the picture.  (A raw texture would not do: GtkPicture
 *              reports its natural width as the texture's intrinsic width,
 *              so a height-limited picture would sit letterboxed in a widget
 *              wider than itself and the action link would drift off its
 *              corner.)  The scaling itself happens in the renderer, so a
 *              12 MP screenshot costs no CPU here — and the picture is still
 *              GTK_CONTENT_FIT_SCALE_DOWN, so a box shrinking under it mid-
 *              resize only ever shrinks the drawing, never overflows.
 *              GTK4 has no size-allocate signal, so a resize is noticed from
 *              a frame-clock tick that runs only while the panel is open
 *              (img_tick, two integer reads a frame) and the host's render()
 *              is asked again once the size has settled (IMG_RESIZE_MS) —
 *              again, not per frame, because a host may decode on demand
 *              and cap the decode to the box it is offered.
 *
 *   focus    — the panel NEVER takes the keyboard focus, and nothing inside
 *              it is focusable.  This is not a detail: a GtkTextView that
 *              loses focus renders its text in the unfocused (grey) colour,
 *              and on GTK3/quartz that grey rendering SURVIVED the panel
 *              closing — GTK reported the focus restored and redrew black at
 *              full clip, yet the screen kept the grey until the toplevel
 *              was re-activated (CLAUDE.md quirk #23; to be re-measured on
 *              GTK4, but the rule costs nothing and stays).  So the cure is
 *              to never create the grey rendering: the focus stays where it
 *              was, and the panel's keys are served from a CAPTURE-phase key
 *              controller on the HOST WINDOW (which runs before the focus
 *              widget sees the key), not from the panel.  A host must
 *              swallow every other key while the panel is up — see
 *              on_image_viewer_key_press.
 *
 *   clicks   — the backdrop is a plain GtkBox covering the overlay, so GTK
 *              picks it for every press meant for the widget behind it and a
 *              click anywhere on it closes the panel.  Its "links" are
 *              therefore PLAIN LABELS hit-tested by bounds from img_press,
 *              never GtkLabel `<a>` links: a link label grabs the focus when
 *              clicked, which is exactly what the focus rule above forbids.
 *              Hit-testing also fixes what the links got wrong — a press
 *              landing in the gaps inside the row (spacing, the separator,
 *              past the end of a word) silently did nothing, so stepping
 *              sometimes took two clicks.  The hover cursor is served the
 *              same way from the panel's own motion controller (img_motion),
 *              and set on the PANEL, which its labels inherit.
 *
 *   walking  — Left/Right and the Previous | Next row all go through
 *              img_step(), which asks the host's count() every time.  There
 *              is no wrap-around: at either end the word is dim and inert.
 *              The row is split by MIDLINE, not by word: the left half steps
 *              back and the right half forward, so there is no dead gap
 *              between two small words (quirk #24).
 *
 *   lifetime — on GTK4 a window's "destroy" fires AFTER its child tree has
 *              been unparented (gtk_window_dispose unparents the child, then
 *              GtkWidget's dispose emits the signal), so by the time a host
 *              frees the panel from that handler the overlay may be gone.
 *              The panel therefore keeps its OWN reference to its widget,
 *              never touches the overlay after close, and on_image_viewer_free
 *              takes the panel out of whatever parent it still has.
 * =========================================================================== */

#include "image_viewer.h"
#include "app.h"                     /* on_app_set_tooltip                  */

/* Height reserved under the image for the caption + action-link row, and for
 * the Previous | Next row under that (logical px).                          */
#define IMG_CAPTION_H 30
#define IMG_NAV_H     24

/* The smallest box the picture is ever fitted to: a window shrunk to almost
 * nothing still shows something rather than an empty panel.                 */
#define IMG_BOX_MIN   32

/* Settle time before an open panel asks the host to render again for a new
 * size (ms).                                                                */
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
 *   overlay  — the GtkOverlay the panel sits in; its size is what the image
 *              is fitted to (not owned; only read while the panel is open,
 *              when it is necessarily alive).
 *   panel    — the dark backdrop box; hidden whenever nothing is on show.
 *              OWNED (one reference), so it is safe to touch from
 *              on_image_viewer_free whatever has happened to the overlay.
 *   image    — the GtkPicture inside it.
 *   caption  — the caption label, bottom left.
 *   link     — the action label, bottom right; NULL when the host asked for
 *              none.
 *   nav      — the "Previous | Next" row (a GtkBox), hit-tested by midline.
 *   nav_prev/nav_next — the two words, restyled per picture by
 *              on_image_viewer_nav_sync; only their LOOK lives here, the
 *              clicking is img_press's business.
 *   idx      — the image on show, or -1 when the panel is closed.  This IS
 *              the open/closed flag.
 *   hand_cursor — whether the panel currently shows the "pointer" cursor,
 *              so plain motion over the backdrop does not build a GdkCursor
 *              per event.
 *   tick     — the frame-clock tick watching the overlay's size while the
 *              panel is open, 0 when closed.
 *   resize   — pending re-render timer after a host resize, 0 if none.
 *   fit_w/fit_h — the box the picture on show was last fitted to, so a tick
 *              that finds the same box costs nothing.
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
    guint                   tick;
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
 *   ref — any widget on the display to style.
 * ------------------------------------------------------------------------- */
static void
img_css_install(GtkWidget *ref)
{
    static gboolean done = FALSE;    /* one provider per process            */
    if (done)
        return;
    done = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
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
        "}");
    gtk_style_context_add_provider_for_display(
        gtk_widget_get_display(ref), GTK_STYLE_PROVIDER(css),
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
    gtk_widget_remove_css_class(label, live ? IMG_CSS_DIM : IMG_CSS_LINK);
    gtk_widget_add_css_class(label, live ? IMG_CSS_LINK : IMG_CSS_DIM);
}

/* ---------------------------------------------------------------------------
 * img_cursor() — show the hand ("pointer") cursor over the panel's clickable
 * words, and the plain inherited one everywhere else.  Set on the PANEL:
 * its labels carry no cursor of their own, so they inherit it (every widget
 * owns its cursor on GTK4 — nothing else can put the text cursor back).
 * NULL makes the panel inherit its parent's cursor again.
 * ------------------------------------------------------------------------- */
static void
img_cursor(OnImageViewer *v, gboolean hand)
{
    if (v->hand_cursor == hand)
        return;
    gtk_widget_set_cursor_from_name(v->panel, hand ? "pointer" : NULL);
    v->hand_cursor = hand;
}

/* ---------------------------------------------------------------------------
 * img_hit() — does a point on the panel land on this child?
 *   child — a label or row inside the panel, or NULL.
 *   x, y  — the point in the PANEL's coordinates (what its controllers
 *           deliver); the child's bounds are computed in the same space.
 * A missing or hidden child is never hit, whatever bounds it still carries.
 * ------------------------------------------------------------------------- */
static gboolean
img_hit(OnImageViewer *v, GtkWidget *child, gdouble x, gdouble y)
{
    if (child == NULL || !gtk_widget_get_visible(child))
        return FALSE;

    graphene_rect_t bounds;          /* where the child sits                */
    if (!gtk_widget_compute_bounds(child, v->panel, &bounds))
        return FALSE;
    return graphene_rect_contains_point(&bounds,
                                        &GRAPHENE_POINT_INIT(x, y));
}

/* ---------------------------------------------------------------------------
 * img_box() — the box the picture is fitted to, right now: the overlay's
 * size inset by ON_IMAGE_VIEWER_INSET on every side, less the two label
 * rows, floored at IMG_BOX_MIN.
 *   w_out, h_out — filled with the box (logical px).
 * ------------------------------------------------------------------------- */
static void
img_box(OnImageViewer *v, gint *w_out, gint *h_out)
{
    *w_out = MAX(gtk_widget_get_width(v->overlay)
                 - 2 * ON_IMAGE_VIEWER_INSET, IMG_BOX_MIN);
    *h_out = MAX(gtk_widget_get_height(v->overlay)
                 - 2 * ON_IMAGE_VIEWER_INSET - IMG_CAPTION_H - IMG_NAV_H,
                 IMG_BOX_MIN);
}

/* ---------------------------------------------------------------------------
 * img_fit_paintable() — the host's paintable, sized to the box: a render-node
 * paintable whose intrinsic size is `src` scaled to fit box_w × box_h with
 * its aspect kept and never enlarged.  No pixels are touched — the node
 * draws `src` at that size and the renderer does the scaling — so the
 * GtkPicture's natural size becomes the drawn size (see the file header).
 *   src — the host's paintable; a reference is TAKEN (the node keeps it).
 * Returns a new paintable, or NULL when `src` has no usable size.
 * ------------------------------------------------------------------------- */
static GdkPaintable *
img_fit_paintable(GdkPaintable *src, gint box_w, gint box_h)
{
    gint iw = gdk_paintable_get_intrinsic_width(src);
    gint ih = gdk_paintable_get_intrinsic_height(src);
    if (iw <= 0 || ih <= 0) {
        g_object_unref(src);
        return NULL;
    }

    gdouble scale = MIN((gdouble)box_w / iw, (gdouble)box_h / ih);
    scale = MIN(scale, 1.0);         /* never upscale past the source       */
    gint fw = MAX(1, (gint)(iw * scale));
    gint fh = MAX(1, (gint)(ih * scale));

    GtkSnapshot *snap = gtk_snapshot_new();
    gdk_paintable_snapshot(src, snap, fw, fh);
    g_object_unref(src);
    return gtk_snapshot_free_to_paintable(snap, &GRAPHENE_SIZE_INIT(fw, fh));
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
    if (v->tick != 0) {
        gtk_widget_remove_tick_callback(v->panel, v->tick);
        v->tick = 0;
    }
    v->idx   = -1;
    v->fit_w = 0;
    v->fit_h = 0;
    gtk_picture_set_paintable(GTK_PICTURE(v->image), NULL);
    img_cursor(v, FALSE);            /* before it hides: no stuck hand      */
    gtk_widget_set_visible(v->panel, FALSE);
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
        gtk_widget_set_visible(v->nav, FALSE);
        return;
    }

    /* At either end the word STAYS PUT, dimmed: the row never changes width
     * under the pointer, and a dead end is visible instead of silent.  Only
     * the LOOK changes here — img_press decides what a click does, from the
     * same count(), so the two can never disagree.                         */
    img_link_look(v->nav_prev, v->idx > 0);
    img_link_look(v->nav_next, v->idx + 1 < n);
    gtk_widget_set_visible(v->nav, TRUE);
}

/* ---------------------------------------------------------------------------
 * img_render() — ask the host for the picture on show and fit it to the
 * overlay's current box (img_box).  Closes the panel when the host can no
 * longer produce the image.
 * ------------------------------------------------------------------------- */
static void
img_render(OnImageViewer *v)
{
    if (v->idx < 0)
        return;

    gint box_w, box_h;               /* the room on offer                   */
    img_box(v, &box_w, &box_h);

    GdkPaintable *big = v->ops->render(v->host, v->idx, box_w, box_h);
    GdkPaintable *fit = (big != NULL) ? img_fit_paintable(big, box_w, box_h)
                                      : NULL;
    if (fit == NULL) {
        on_image_viewer_close(v);
        return;
    }
    v->fit_w = box_w;
    v->fit_h = box_h;
    gtk_picture_set_paintable(GTK_PICTURE(v->image), fit);
    g_object_unref(fit);             /* the GtkPicture holds its own ref    */
}

/* img_resize_done() — the host has stopped changing size: render again.     */
static gboolean
img_resize_done(gpointer user_data)
{
    OnImageViewer *v = user_data;    /* the panel                           */
    v->resize = 0;
    img_render(v);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * img_tick() — one frame while the panel is open: has the box the picture
 * was fitted to changed?  If so, (re)start the settle timer; the render
 * itself waits for IMG_RESIZE_MS of quiet, because a drag-resize changes
 * the box every frame and a host may decode the picture again per render.
 * The comparison is what makes this cheap: two integer reads a frame, and
 * nothing at all when the size holds.
 * ------------------------------------------------------------------------- */
static gboolean
img_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    (void)widget; (void)clock;
    OnImageViewer *v = user_data;    /* the panel                           */

    gint box_w, box_h;               /* the room on offer now               */
    img_box(v, &box_w, &box_h);
    if (box_w == v->fit_w && box_h == v->fit_h)
        return G_SOURCE_CONTINUE;

    if (v->resize != 0)
        g_source_remove(v->resize);
    v->resize = g_timeout_add(IMG_RESIZE_MS, img_resize_done, v);
    return G_SOURCE_CONTINUE;
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

    gtk_widget_set_visible(v->panel, TRUE);
    /* The size we fit FROM is the OVERLAY's, which is already allocated, so
     * render straight away; the tick catches up with anything that was not
     * (a box read before the first layout re-renders once it settles).     */
    img_render(v);
    if (v->idx >= 0 && v->tick == 0)
        v->tick = gtk_widget_add_tick_callback(v->panel, img_tick, v, NULL);
    /* Deliberately NO grab_focus: the focus stays exactly where the host
     * left it, so nothing behind the panel ever renders unfocused.  The
     * keys arrive through the host window's controller instead.             */
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
on_image_viewer_key_press(OnImageViewer *v, guint keyval,
                          GdkModifierType state)
{
    (void)state;                     /* the keys are unmodified ones        */
    if (v == NULL || v->idx < 0)
        return FALSE;

    switch (keyval) {
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
 * controller handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * img_nav_delta() — what a position inside the Previous | Next row means.
 * Resolved by MIDLINE rather than by which word was hit: the two words are
 * small and the row has spacing and a separator between them, so word-exact
 * hit-testing left gaps where a click did nothing and stepping took two
 * tries.  Left half means back, right half forward.
 *   x        — position in the panel's coordinates.
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
    graphene_rect_t r;               /* the row, in panel coordinates       */
    gdouble mid = 0;                 /* its midline                         */
    if (gtk_widget_compute_bounds(v->nav, v->panel, &r))
        mid = r.origin.x + r.size.width / 2.0;
    gint delta = (x < mid) ? -1 : +1;
    if (live_out != NULL)
        *live_out = (delta < 0) ? (v->idx > 0)
                                : (v->idx + 1 < v->ops->count(v->host));
    return delta;
}

/* img_nav_press() — a press in the row: step, unless that way is a dead end.
 * Returns nothing; the caller has already claimed the press, so a near-miss
 * never dismisses the picture.                                              */
static void
img_nav_press(OnImageViewer *v, gdouble x)
{
    gboolean live;                   /* does that way lead anywhere?        */
    gint delta = img_nav_delta(v, x, &live);
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
 * img_press() — a primary press anywhere on the panel closes it, except on
 * the link labels (see the file header for why the geometry is checked).
 * Every press counts, whatever n_press says: GTK4 reports the second press
 * of a double click ONLY as n_press 2, so gating on 1 would swallow a quick
 * Next, Next.  The double-click-out-of-habit case takes care of itself —
 * the first press hides the panel, so the second lands on the host widget
 * behind it (which may re-open the picture), and there is no third event.
 * The sequence is claimed so no ancestor's gesture acts on it as well.
 * ------------------------------------------------------------------------- */
static void
img_press(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
          gpointer user_data)
{
    (void)n_press;
    OnImageViewer *v = user_data;    /* the panel                           */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    /* The labels take no input of their own, so every press lands here and
     * this is the ONE place that decides.                                  */
    if (img_hit(v, v->nav, x, y)) {
        img_nav_press(v, x);
        return;
    }
    if (img_hit(v, v->link, x, y)) {
        img_action_press(v);
        return;
    }
    on_image_viewer_close(v);
}

/* ---------------------------------------------------------------------------
 * img_motion() — hand cursor over the action label and over the LIVE half of
 * the Previous | Next row.  Hit-tested exactly like the press (img_hit +
 * img_nav_delta), so the pointer promises precisely what a click delivers:
 * no hand over a dimmed dead end.
 * ------------------------------------------------------------------------- */
static void
img_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y,
           gpointer user_data)
{
    (void)controller;
    OnImageViewer *v = user_data;    /* the panel                           */
    gboolean hand = img_hit(v, v->link, x, y);  /* over a target?          */
    if (!hand && img_hit(v, v->nav, x, y))
        img_nav_delta(v, x, &hand);
    img_cursor(v, hand);
}

/* ---------------------------------------------------------------------------
 * img_scroll() — swallow the wheel over the panel.  The widget hidden behind
 * it is a SIBLING under the overlay, so GTK4's propagation never reaches it
 * from here anyway; this guards a host that scrolls from an ANCESTOR of the
 * overlay, which would otherwise see the wheel bubble up through the panel.
 * ------------------------------------------------------------------------- */
static gboolean
img_scroll(GtkEventControllerScroll *controller, gdouble dx, gdouble dy,
           gpointer user_data)
{
    (void)controller; (void)dx; (void)dy; (void)user_data;
    return TRUE;
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

    /* Fits the pre-fitted paintable (img_fit_paintable) exactly, and only
     * ever shrinks it further — a box collapsing under an open panel mid-
     * resize shrinks the drawing; a grid made wider by a long caption does
     * not enlarge it.                                                      */
    v->image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(v->image),
                                GTK_CONTENT_FIT_SCALE_DOWN);

    v->caption = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(v->caption), PANGO_ELLIPSIZE_END);
    /* Bounded natural width (ellipsize makes max-width-chars the cap), so a
     * long description cannot make the strip wider than the picture.       */
    gtk_label_set_max_width_chars(GTK_LABEL(v->caption), 60);
    gtk_label_set_xalign(GTK_LABEL(v->caption), 0.0);
    gtk_widget_set_hexpand(v->caption, TRUE);
    img_label_small(v->caption);

    /* A plain label wearing the link LOOK, clicked via img_press.  Not a
     * GtkLabel `<a>` link: that grabs the focus when clicked, which the
     * focus rule in the file header forbids.                               */
    if (action_label != NULL && ops->action != NULL) {
        v->link = gtk_label_new(action_label);
        gtk_widget_set_halign(v->link, GTK_ALIGN_END);
        if (action_tip != NULL)
            on_app_set_tooltip(v->link, action_tip);
        img_label_small(v->link);
        img_link_look(v->link, TRUE);
    }

    /* The "Previous | Next" row: a BOX of three plain labels.  It is claimed
     * and split as a whole by img_press, so the words take no input of their
     * own and there are no gaps between them where a click would be
     * swallowed.  Its visibility belongs to on_image_viewer_nav_sync, so it
     * starts hidden.                                                       */
    v->nav_prev = gtk_label_new("Previous");
    v->nav_next = gtk_label_new("Next");
    GtkWidget *nav_sep = gtk_label_new("|");
    img_label_small(v->nav_prev);
    img_label_small(v->nav_next);
    img_label_small(nav_sep);
    img_link_look(v->nav_prev, TRUE);
    img_link_look(v->nav_next, TRUE);
    gtk_widget_add_css_class(nav_sep, IMG_CSS_DIM);

    v->nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(v->nav), v->nav_prev);
    gtk_box_append(GTK_BOX(v->nav), nav_sep);
    gtk_box_append(GTK_BOX(v->nav), v->nav_next);
    gtk_widget_set_visible(v->nav, FALSE);
    gtk_widget_set_halign(v->nav, GTK_ALIGN_CENTER);
    on_app_set_tooltip(v->nav,
        "Show the previous or next image (or press the \xe2\x86\x90 and "
        "\xe2\x86\x92 keys)");

    /* A GtkGrid, not a box: the image spans both columns, so the grid is
     * exactly as wide as the picture and the right-aligned action link
     * lands under the picture's bottom-right corner.  The caption takes the
     * slack (hexpand), and the grid's own EXPLICIT hexpand blocks that flag
     * from propagating outwards — otherwise the grid would stretch to the
     * whole panel and take the link with it.  vexpand is what lets the
     * backdrop box hand the grid its full height, which GTK_ALIGN_CENTER
     * then centres the grid's natural height in.                           */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(grid, FALSE);   /* stop the caption's expand here */
    gtk_widget_set_vexpand(grid, TRUE);
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

    /* The backdrop: a plain box GTK picks for every press over the overlay
     * (default halign/valign FILL as an overlay child covers all of it).
     * Its own expand flags are set EXPLICITLY so the grid's vexpand stops
     * here and cannot leak into the host's layout through the overlay.     */
    v->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(v->panel, FALSE);
    gtk_widget_set_vexpand(v->panel, FALSE);
    gtk_widget_set_visible(v->panel, FALSE);
    /* NOT focusable, nor anything inside it — see the focus rule in the
     * file header.  can-focus FALSE covers the whole subtree on GTK4.      */
    gtk_widget_set_can_focus(v->panel, FALSE);
    gtk_box_append(GTK_BOX(v->panel), grid);
    gtk_widget_add_css_class(v->panel, IMG_CSS_VIEWER);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(img_press), v);
    gtk_widget_add_controller(v->panel, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(img_motion), v);
    gtk_widget_add_controller(v->panel, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(img_scroll), v);
    gtk_widget_add_controller(v->panel, scroll);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), v->panel);
    /* Our own reference, so on_image_viewer_free can still reach the panel
     * after the overlay has let go of it (see "lifetime", file header).    */
    g_object_ref(v->panel);
    return v;
}

void
on_image_viewer_free(OnImageViewer *v)
{
    if (v == NULL)
        return;
    /* Stops the tick and the timer, so neither can reach freed memory.  The
     * panel is ours to touch whatever the overlay's state (own reference). */
    on_image_viewer_close(v);
    /* Still parented means the overlay is alive: take the panel out of it,
     * so the controllers pointing at this struct never fire again.  Already
     * unparented (the GTK4 destroy order) means there is nothing to leave. */
    GtkWidget *parent = gtk_widget_get_parent(v->panel);
    if (parent != NULL)
        gtk_overlay_remove_overlay(GTK_OVERLAY(parent), v->panel);
    g_object_unref(v->panel);
    g_free(v);
}
