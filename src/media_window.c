/* ===========================================================================
 * media_window.c — the folder media browser (implementation)
 *
 * See media_window.h for the layout overview.  Key mechanics:
 *
 *   scanning    — the note set is walked in IDLE TIME SLICES, one image at
 *                 a time (media_scan_idle), so a big selection fills the
 *                 grid progressively instead of freezing the GUI.  Image
 *                 counts come from a record walk over the BNBF blob that
 *                 decodes nothing (on_note_count_images); only the one PNG
 *                 behind each thumbnail is decoded, and only at thumbnail
 *                 size.  Same discipline as the library's grid thumbnails.
 *
 *   the viewer  — a single click does NOT resize the thumbnail: it shows the
 *                 picture in a modal panel (a GtkOverlay child) covering the
 *                 whole grid, the image fitted to the window inset by
 *                 MEDIA_VIEWER_INSET on every side.  The panel swallows all
 *                 clicks, so only one image is ever shown and the grid never
 *                 reflows underneath.  Clicking it (or Escape) closes it.
 *                 That one image is re-read at panel size and dropped again
 *                 on close, so the window's memory is the thumbnails plus at
 *                 most one big picture.  Getting from a picture to its note
 *                 is a "Show in source note" LINK under the image's
 *                 bottom-right corner, not a double click: with a single
 *                 click already opening and closing the viewer, a double
 *                 click has nowhere to land — its first press would have
 *                 dismissed whatever its second press was aimed at.
 *
 *   addressing  — a cell is (note id, image ordinal).  The ordinal is the
 *                 image's position among the note's IMAGE records, which is
 *                 also the order the editor's image anchors appear in — so
 *                 on_editor_window_open_image() can scroll straight to it.
 * =========================================================================== */

#include "media_window.h"
#include "editor_window.h"
#include "serialize.h"

#include <cairo-gobject.h>

/* Logical pixel box a thumbnail is fitted into (aspect kept, no upscaling). */
#define MEDIA_THUMB_BOX 144

/* How far the viewer's image stays clear of the window edge (logical px).    */
#define MEDIA_VIEWER_INSET 20

/* Height reserved under the viewer's image for its caption (logical px).    */
#define MEDIA_VIEWER_CAPTION_H 30

/* ...and for the "Previous | Next" row under that (logical px).             */
#define MEDIA_VIEWER_NAV_H 24

/* Hard cap on how many thumbnails one window will build.  Every cell holds
 * its own decoded pixels, so an unbounded "All Notes" browse of a database
 * full of screenshots would grow without limit; the status line says when
 * the cap truncated the grid.                                               */
#define MEDIA_MAX_IMAGES 500

/* How long one media_scan_idle() slice may run before yielding to the main
 * loop (µs) — the library's thumbnail filler uses the same budget.          */
#define MEDIA_SCAN_BUDGET_US (40 * 1000)

/* Fallback dimensions before any media window has been resized.             */
#define MEDIA_WIN_DEFAULT_W 780
#define MEDIA_WIN_DEFAULT_H 560

/* Settle time before the open viewer re-renders for a new window size (ms):
 * a drag-resize fires configure-event continuously, and each render decodes
 * the PNG again.                                                            */
#define MEDIA_VIEWER_RESIZE_MS 150

/* Style classes (see media_css_install).                                    */
#define MEDIA_CSS_CELL   "on-media-cell"
#define MEDIA_CSS_VIEWER "on-media-viewer"

typedef struct OnMedia OnMedia;

/* ---------------------------------------------------------------------------
 * MediaNote — one note in the scan queue: everything the window needs from
 * the OnNoteMeta list it was handed (which the caller owns and frees).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;                      /* the note                            */
    gchar  *title;                   /* its title (owned)                   */
} MediaNote;

/* ---------------------------------------------------------------------------
 * MediaCell — one thumbnail in the grid.
 *
 * Fields:
 *   mw       — owning window (not owned).
 *   note_id  — note the image lives in.
 *   ord      — the image's 0-based position among that note's images.
 *   n_img    — how many images that note holds (for the captions).
 *   idx      — the cell's own position in mw->cells, which is grid order and
 *              therefore the order the viewer's Previous/Next walk.  Cells
 *              are only ever APPENDED (the scan never removes one), so this
 *              is fixed for the cell's life.
 *   title    — the note's title (owned).
 *   thumb    — the thumbnail surface (owned).
 * Widgets are deliberately absent: a cell never has to touch its own after
 * construction, so nothing here can outlive the window's widget tree.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnMedia         *mw;
    gint64           note_id;
    gint             ord;
    gint             n_img;
    gint             idx;
    gchar           *title;
    cairo_surface_t *thumb;
} MediaCell;

/* ---------------------------------------------------------------------------
 * OnMedia — all state for one media window.
 *
 * Fields:
 *   app         — global application context (not owned).
 *   window      — the media window itself.
 *   overlay     — GtkOverlay stacking the viewer panel over the grid.
 *   scroll      — scrolled window around the grid.
 *   flow        — the GtkFlowBox holding the cells.
 *   viewer      — the modal panel; hidden whenever no image is shown.
 *   viewer_img  — the GtkImage inside it.
 *   viewer_cap  — the caption under that image.
 *   viewer_link — the "Show in source note" link beside that caption.
 *   viewer_nav  — the "Previous | Next" row centred under the image; one
 *                 label carrying both links (see media_viewer_nav_sync).
 *   viewer_cell — the cell currently on show, or NULL when closed.
 *   viewer_resize — pending re-render timer after a window resize, 0 if none.
 *   status      — status label under the grid.
 *   spinner     — spins while the scan is still running.
 *   scope       — what is being browsed, for the status line (owned).
 *   notes       — MediaNote* scan queue (owned).
 *   cells       — MediaCell* in grid order (owned).
 *   scan_i      — index in `notes` of the note being scanned.
 *   scan_blob   — that note's BNBF bytes while its images are being read
 *                 (owned), so yielding mid-note does not re-read it.
 *   scan_len    — length of scan_blob.
 *   scan_n_img  — how many images that note holds.
 *   scan_ord    — the next image ordinal to decode from it.
 *   scan_open   — TRUE while scan_i's blob has been loaded and counted.
 *   scan_idle   — the idle source doing the scanning, 0 when finished.
 *   n_notes     — how many notes contributed at least one image.
 *   truncated   — TRUE once MEDIA_MAX_IMAGES cut the scan short.
 *   win_w/win_h — the window's live size: the viewer sizes itself from it,
 *                 and it is persisted on close so the next media window
 *                 opens at the size this one was left at.
 * ------------------------------------------------------------------------- */
struct OnMedia {
    OnApp      *app;
    GtkWidget  *window;
    GtkWidget  *overlay;
    GtkWidget  *scroll;
    GtkWidget  *flow;
    GtkWidget  *viewer;
    GtkWidget  *viewer_img;
    GtkWidget  *viewer_cap;
    GtkWidget  *viewer_link;
    GtkWidget  *viewer_nav;
    MediaCell  *viewer_cell;
    guint       viewer_resize;
    GtkWidget  *status;
    GtkWidget  *spinner;
    gchar      *scope;
    GPtrArray  *notes;
    GPtrArray  *cells;
    guint       scan_i;
    guint8     *scan_blob;
    gsize       scan_len;
    gint        scan_n_img;
    gint        scan_ord;
    gboolean    scan_open;
    guint       scan_idle;
    gint        n_notes;
    gboolean    truncated;
    gint        win_w;
    gint        win_h;
};

/* ===========================================================================
 * small helpers
 * =========================================================================== */

/* media_note_free() — GDestroyNotify for the scan queue.                    */
static void
media_note_free(gpointer data)
{
    MediaNote *n = data;
    g_free(n->title);
    g_free(n);
}

/* media_cell_free() — GDestroyNotify for the cell array.                    */
static void
media_cell_free(gpointer data)
{
    MediaCell *c = data;
    if (c->thumb != NULL)
        cairo_surface_destroy(c->thumb);
    g_free(c->title);
    g_free(c);
}

/* ---------------------------------------------------------------------------
 * media_css_install() — install the window's styling once per screen: the
 * thumbnails' hover tint and the viewer panel's dark backdrop.  Scoped to
 * the two classes, so nothing else on screen is affected.
 * ------------------------------------------------------------------------- */
static void
media_css_install(GtkWidget *window)
{
    static gboolean done = FALSE;    /* one provider per process            */
    if (done)
        return;
    done = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "." MEDIA_CSS_CELL " {"
        "  border: 2px solid transparent;"
        "  border-radius: 4px;"
        "  padding: 3px;"
        "}"
        "." MEDIA_CSS_CELL ":hover {"
        "  background-color: alpha(currentColor, 0.07);"
        "}"
        "." MEDIA_CSS_VIEWER " {"
        "  background-color: alpha(#101010, 0.88);"
        "}"
        "." MEDIA_CSS_VIEWER " label {"
        "  color: #f2f2f2;"
        "}"
        /* The link's colour must be set AFTER the blanket label colour
         * above, which would otherwise flatten it into plain text.         */
        "." MEDIA_CSS_VIEWER " label:link,"
        "." MEDIA_CSS_VIEWER " label:visited {"
        "  color: #8ab4f8;"
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gtk_widget_get_screen(window), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---------------------------------------------------------------------------
 * media_surface_fit() — wrap a decoded pixbuf in a cairo surface carrying
 * the display's device scale, scaled to fit box_w × box_h LOGICAL pixels
 * with its aspect kept and never upscaled.  The device scale is what keeps
 * the picture pixel-sharp on Retina instead of letting the compositor
 * stretch it (quirk #5, as in the editor's image_widget_new).
 *   mw    — the window (for the scale factor and target GdkWindow).
 *   pix   — the decoded image.
 *   box_w/box_h — logical box to fit inside.
 * Returns a new surface; cairo_surface_destroy() it.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
media_surface_fit(OnMedia *mw, GdkPixbuf *pix, gint box_w, gint box_h)
{
    gint sf = gtk_widget_get_scale_factor(mw->window);
    gint iw = gdk_pixbuf_get_width(pix);
    gint ih = gdk_pixbuf_get_height(pix);

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
        backing, sf, gtk_widget_get_window(mw->window));
    g_object_unref(backing);
    return surface;
}

/* ---------------------------------------------------------------------------
 * media_render() — one image of one note, decoded and fitted to a logical
 * box.  `blob`/`len` may be bytes the caller already holds (the scan) or
 * NULL to read the note fresh (the viewer).  Only the ONE image is decoded,
 * and only as large as the box needs.
 * Returns a surface, or NULL when the image is gone or will not decode.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
media_render(OnMedia *mw, gint64 note_id, gint ord, gint box_w, gint box_h,
             const guint8 *blob, gsize len)
{
    guint8 *own = NULL;              /* blob read here, if any              */
    if (blob == NULL) {
        own = on_db_note_load(mw->app->db, note_id, &len);
        blob = own;
    }
    cairo_surface_t *surface = NULL;  /* the result                          */
    if (blob != NULL) {
        gint sf  = gtk_widget_get_scale_factor(mw->window);
        gint cap = MAX(box_w, box_h) * sf;
                                     /* the decode cap is on the LONGEST
                                        side; the fit does the rest        */
        GdkPixbuf *pix = on_note_image_nth(blob, len, ord, cap);
        if (pix != NULL) {
            surface = media_surface_fit(mw, pix, box_w, box_h);
            g_object_unref(pix);
        }
    }
    g_free(own);
    return surface;
}

/* ===========================================================================
 * the viewer panel
 * =========================================================================== */

/* media_viewer_close() — hide the panel and drop the big surface (only one
 * is ever alive, so it is not worth caching).                                */
static void
media_viewer_close(OnMedia *mw)
{
    if (mw->viewer_resize != 0) {
        g_source_remove(mw->viewer_resize);
        mw->viewer_resize = 0;
    }
    mw->viewer_cell = NULL;
    gtk_image_clear(GTK_IMAGE(mw->viewer_img));
    gtk_widget_hide(mw->viewer);
}

/* ---------------------------------------------------------------------------
 * media_viewer_render() — (re)draw the panel's image at the window's current
 * size: the whole client area inset by MEDIA_VIEWER_INSET on every side,
 * less the caption strip.  Called when the panel opens and after a resize.
 * Closes the panel if the image can no longer be decoded.
 * ------------------------------------------------------------------------- */
static void
media_viewer_render(OnMedia *mw)
{
    MediaCell *c = mw->viewer_cell;   /* the image on show                  */
    if (c == NULL)
        return;

    GtkAllocation alloc;              /* the overlay's client area          */
    gtk_widget_get_allocation(mw->overlay, &alloc);
    gint box_w = alloc.width  - 2 * MEDIA_VIEWER_INSET;
    gint box_h = alloc.height - 2 * MEDIA_VIEWER_INSET
                              - MEDIA_VIEWER_CAPTION_H
                              - MEDIA_VIEWER_NAV_H;
                                      /* the caption + link strip under it,
                                         then the Previous | Next row       */
    /* A window shrunk to almost nothing still shows something rather than
     * an empty panel.                                                      */
    box_w = MAX(box_w, 32);
    box_h = MAX(box_h, 32);

    cairo_surface_t *big =
        media_render(mw, c->note_id, c->ord, box_w, box_h, NULL, 0);
    if (big == NULL) {
        media_viewer_close(mw);
        return;
    }
    gtk_image_set_from_surface(GTK_IMAGE(mw->viewer_img), big);
    cairo_surface_destroy(big);        /* the GtkImage holds its own ref     */
}

/* media_viewer_resize_done() — the window has stopped changing size: redraw
 * the open panel for the new one.                                           */
static gboolean
media_viewer_resize_done(gpointer user_data)
{
    OnMedia *mw = user_data;          /* owning media window                */
    mw->viewer_resize = 0;
    media_viewer_render(mw);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * media_viewer_nav_sync() — rewrite the "Previous | Next" row for the picture
 * currently on show.  Each side is a live link only while the grid holds a
 * cell that way; at either end the word stays put as dim plain text, so the
 * row never changes width under the pointer and a dead end is visible rather
 * than silent.  With a single picture in the whole grid neither side could
 * ever lead anywhere, so the row is hidden outright.
 *
 * Called on every open (the row is per-picture) and, while the scan is still
 * running, when a cell lands directly after the one on show — that is the
 * one moment a greyed-out "Next" becomes reachable.
 * ------------------------------------------------------------------------- */
static void
media_viewer_nav_sync(OnMedia *mw)
{
    MediaCell *c = mw->viewer_cell;   /* the picture on show                */
    if (c == NULL)
        return;
    if (mw->cells->len <= 1) {
        gtk_widget_hide(mw->viewer_nav);
        return;
    }

    gboolean has_prev = c->idx > 0;
    gboolean has_next = c->idx + 1 < (gint)mw->cells->len;
    gchar *markup = g_strdup_printf(
        "%s   <span alpha=\"40%%\">|</span>   %s",
        has_prev ? "<a href=\"prev\">Previous</a>"
                 : "<span alpha=\"40%\">Previous</span>",
        has_next ? "<a href=\"next\">Next</a>"
                 : "<span alpha=\"40%\">Next</span>");
    gtk_label_set_markup(GTK_LABEL(mw->viewer_nav), markup);
    g_free(markup);
    gtk_widget_show(mw->viewer_nav);
}

/* ---------------------------------------------------------------------------
 * media_viewer_open() — show one cell's image in the modal panel, replacing
 * whatever was on show.  A picture that will not decode leaves the grid as
 * it was.
 * ------------------------------------------------------------------------- */
static void
media_viewer_open(OnMedia *mw, MediaCell *c)
{
    mw->viewer_cell = c;

    gchar *cap = g_strdup_printf(
        "%s \xe2\x80\x94 image %d of %d      "
        "\xe2\x86\x90 \xe2\x86\x92 to move, click to close",
        c->title, c->ord + 1, c->n_img);
    gtk_label_set_text(GTK_LABEL(mw->viewer_cap), cap);
    g_free(cap);
    media_viewer_nav_sync(mw);

    gtk_widget_show(mw->viewer);
    /* Rendering needs the panel's allocation, which the show above has not
     * produced yet on the first open; the size we size FROM is the
     * overlay's, which is already allocated, so render straight away.      */
    media_viewer_render(mw);
    if (mw->viewer_cell == NULL)      /* render failed and closed it again   */
        return;
    gtk_widget_grab_focus(mw->viewer);
}

/* ---------------------------------------------------------------------------
 * media_viewer_step() — move the open panel `delta` cells through the grid
 * (−1 = previous, +1 = next), in the grid's own order.  A step past either
 * end does nothing: the panel is never closed by navigating, and it never
 * wraps around — with up to MEDIA_MAX_IMAGES cells, jumping from the last
 * picture to the first reads as a glitch rather than a move.
 * ------------------------------------------------------------------------- */
static void
media_viewer_step(OnMedia *mw, gint delta)
{
    MediaCell *c = mw->viewer_cell;   /* the picture on show                */
    if (c == NULL)
        return;

    gint i = c->idx + delta;          /* where the step lands               */
    if (i < 0 || i >= (gint)mw->cells->len)
        return;
    media_viewer_open(mw, g_ptr_array_index(mw->cells, i));
}

/* ---------------------------------------------------------------------------
 * media_viewer_nav_link() — the "Previous"/"Next" links, told apart by the
 * href media_viewer_nav_sync gave them (one label carries both).
 * Returns TRUE so GtkLabel does not try to launch the href as a URI.
 * ------------------------------------------------------------------------- */
static gboolean
media_viewer_nav_link(GtkWidget *label, const gchar *uri, gpointer user_data)
{
    (void)label;
    OnMedia *mw = user_data;          /* owning media window                */
    media_viewer_step(mw, (g_strcmp0(uri, "prev") == 0) ? -1 : +1);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * media_viewer_link() — the "Show in source note" link: open the note the
 * shown picture came from, scrolled to that picture, and close the panel.
 * Returns TRUE so GtkLabel does not try to launch the href as a URI (the
 * href is a placeholder; GtkLabel needs one to draw a link at all).
 * ------------------------------------------------------------------------- */
static gboolean
media_viewer_link(GtkWidget *label, const gchar *uri, gpointer user_data)
{
    (void)label; (void)uri;
    OnMedia   *mw = user_data;        /* owning media window                */
    MediaCell *c  = mw->viewer_cell;  /* the picture on show                */
    if (c == NULL)
        return TRUE;

    gint64 note_id = c->note_id;      /* copied: closing clears viewer_cell */
    gint   ord     = c->ord;
    media_viewer_close(mw);
    on_editor_window_open_image(mw->app, note_id, ord);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * media_hit() — does a press on the viewer panel land on one of its link
 * labels?  Both the event and the child allocations are in the panel event
 * box's window coordinates (a visible-window GtkEventBox allocates its child
 * at its own border width), so they compare directly.  A hidden child is
 * never hit, whatever allocation it is still carrying.
 *   child — a label inside the panel.
 *   event — the press, in panel coordinates.
 * Returns TRUE when the press is inside the child.
 * ------------------------------------------------------------------------- */
static gboolean
media_hit(GtkWidget *child, const GdkEventButton *event)
{
    if (!gtk_widget_get_visible(child))
        return FALSE;

    GtkAllocation a;                  /* where the child sits               */
    gtk_widget_get_allocation(child, &a);
    return event->x >= a.x && event->x < a.x + a.width &&
           event->y >= a.y && event->y < a.y + a.height;
}

/* ---------------------------------------------------------------------------
 * media_viewer_press() — a click anywhere on the panel closes it, except on
 * the link labels.  Those are child labels with their own handling, so a
 * click there never reaches here — a GtkLabel carrying links puts its own
 * input-only window over itself and takes the press first.  Closing the
 * panel out from under a link would make it unclickable, though, so the
 * geometry is checked rather than trusted.  A dead-end "Previous"/"Next"
 * carries no link and so does reach here: it is left inert on purpose
 * rather than closing the panel the way the backdrop does.
 *
 * Only the plain press acts: a stray GDK_2BUTTON_PRESS (from someone
 * double-clicking out of habit) must not close a panel the first press has
 * already closed and the grid re-opened.
 * ------------------------------------------------------------------------- */
static gboolean
media_viewer_press(GtkWidget *widget, GdkEventButton *event,
                   gpointer user_data)
{
    (void)widget;
    OnMedia *mw = user_data;          /* owning media window                */
    if (event->button != GDK_BUTTON_PRIMARY ||
        event->type   != GDK_BUTTON_PRESS)
        return FALSE;

    if (media_hit(mw->viewer_link, event) ||
        media_hit(mw->viewer_nav,  event))
        return FALSE;

    media_viewer_close(mw);
    return TRUE;
}

/* media_viewer_scroll() — swallow the wheel over the panel, so the grid
 * hidden behind it cannot be scrolled out from under it.                     */
static gboolean
media_viewer_scroll(GtkWidget *widget, GdkEventScroll *event,
                    gpointer user_data)
{
    (void)widget; (void)event; (void)user_data;
    return TRUE;
}

/* ===========================================================================
 * the grid
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * media_cell_press() — a click on one thumbnail puts its picture in the
 * viewer panel.  The thumbnail itself never changes size, so the grid never
 * reflows under the pointer.
 * ------------------------------------------------------------------------- */
static gboolean
media_cell_press(GtkWidget *widget, GdkEventButton *event,
                 gpointer user_data)
{
    (void)widget;
    MediaCell *c = user_data;        /* the clicked cell                    */
    if (event->button != GDK_BUTTON_PRIMARY ||
        event->type   != GDK_BUTTON_PRESS)
        return FALSE;
    media_viewer_open(c->mw, c);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * media_add_cell() — build and append one thumbnail cell.
 *   note  — the note the image belongs to (for the caption and tooltip).
 *   ord   — the image's ordinal within that note.
 *   n_img — how many images that note has.
 *   blob/len — the note's already-loaded BNBF bytes.
 * Returns TRUE when a cell was added (FALSE if the image would not decode).
 * ------------------------------------------------------------------------- */
static gboolean
media_add_cell(OnMedia *mw, const MediaNote *note, gint ord, gint n_img,
               const guint8 *blob, gsize len)
{
    cairo_surface_t *thumb = media_render(mw, note->id, ord, MEDIA_THUMB_BOX,
                                          MEDIA_THUMB_BOX, blob, len);
    if (thumb == NULL)
        return FALSE;

    MediaCell *c = g_new0(MediaCell, 1);
    c->mw      = mw;
    c->note_id = note->id;
    c->ord     = ord;
    c->n_img   = n_img;
    c->idx     = (gint)mw->cells->len;  /* appended below; grid order        */
    c->title   = g_strdup(note->title);
    c->thumb   = thumb;

    GtkWidget *image = gtk_image_new_from_surface(thumb);
    /* Boxed to a square so the grid stays a grid whatever shape the
     * pictures are; the picture itself is centred inside it.               */
    gtk_widget_set_size_request(image, MEDIA_THUMB_BOX, MEDIA_THUMB_BOX);

    /* Caption: the owning note's title, ellipsized so a long one cannot
     * widen the cell.                                                      */
    GtkWidget *caption = gtk_label_new(note->title);
    gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(caption), 18);
    gtk_widget_set_size_request(caption, MEDIA_THUMB_BOX, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(caption),
                                "dim-label");
    {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_scale_new(PANGO_SCALE_SMALL));
        gtk_label_set_attributes(GTK_LABEL(caption), attrs);
        pango_attr_list_unref(attrs);
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(box), image,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), caption, FALSE, FALSE, 0);

    /* The event box is what takes the clicks; the flow-box child itself
     * has no handler, so its own selection machinery stays out of the way. */
    GtkWidget *frame = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(frame), TRUE);
    gtk_container_add(GTK_CONTAINER(frame), box);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame),
                                MEDIA_CSS_CELL);
    g_signal_connect(frame, "button-press-event",
                     G_CALLBACK(media_cell_press), c);
    {
        gchar *tip = g_strdup_printf(
            "%s\nImage %d of %d \xe2\x80\x94 click to view",
            note->title, ord + 1, n_img);
        gtk_widget_set_tooltip_text(frame, tip);
        g_free(tip);
    }

    GtkWidget *child = gtk_flow_box_child_new();
    /* Not focusable: the cell's own click handler drives everything, and a
     * focusable child gets focus as it is added — which makes the enclosing
     * scrolled window chase the newest cell, so a filling grid scrolls
     * itself to the bottom instead of staying at the top.                  */
    gtk_widget_set_can_focus(child, FALSE);
    gtk_container_add(GTK_CONTAINER(child), frame);
    gtk_flow_box_insert(GTK_FLOW_BOX(mw->flow), child, -1);
    gtk_widget_show_all(child);

    g_ptr_array_add(mw->cells, c);
    /* A picture landing directly after the one on show turns that panel's
     * greyed-out "Next" into a live link (and the second cell of all reveals
     * the row itself) — the only way the nav row changes while a scan runs.  */
    if (mw->viewer_cell != NULL && mw->viewer_cell->idx == c->idx - 1)
        media_viewer_nav_sync(mw);
    return TRUE;
}

/* ===========================================================================
 * scanning
 * =========================================================================== */

/* media_status_update() — rewrite the status line from the current counts.  */
static void
media_status_update(OnMedia *mw)
{
    guint n = mw->cells->len;        /* thumbnails built so far             */
    gchar *text;                     /* the message                         */

    if (mw->scan_idle != 0) {
        text = g_strdup_printf("Scanning\xe2\x80\xa6 %u image%s so far",
                               n, n == 1 ? "" : "s");
    } else if (n == 0) {
        text = g_strdup_printf("No images in \xe2\x80\x9c%s\xe2\x80\x9d",
                               mw->scope);
    } else {
        text = g_strdup_printf(
            "%u image%s in %d note%s%s", n, n == 1 ? "" : "s",
            mw->n_notes, mw->n_notes == 1 ? "" : "s",
            mw->truncated
                ? " \xe2\x80\x94 more were found than can be shown; "
                  "browse a smaller folder to see the rest"
                : "");
    }
    gtk_label_set_text(GTK_LABEL(mw->status), text);
    g_free(text);
}

/* media_scan_close_note() — done with the current note: drop its blob and
 * move the queue on.                                                        */
static void
media_scan_close_note(OnMedia *mw)
{
    g_clear_pointer(&mw->scan_blob, g_free);
    mw->scan_len   = 0;
    mw->scan_n_img = 0;
    mw->scan_ord   = 0;
    mw->scan_open  = FALSE;
    mw->scan_i++;
}

/* media_scan_finish() — the scan is over (queue drained or cap reached):
 * stop the spinner and settle the status line.                              */
static void
media_scan_finish(OnMedia *mw)
{
    media_scan_close_note(mw);
    mw->scan_idle = 0;
    gtk_spinner_stop(GTK_SPINNER(mw->spinner));
    gtk_widget_hide(mw->spinner);
    media_status_update(mw);
}

/* ---------------------------------------------------------------------------
 * media_scan_idle() — walk the note queue in time slices, adding one
 * thumbnail per step.  A note's blob is loaded once and kept across slices
 * (scan_blob), so a note holding twenty screenshots is read from SQLite
 * once however many yields its images take.
 * ------------------------------------------------------------------------- */
static gboolean
media_scan_idle(gpointer user_data)
{
    OnMedia *mw = user_data;         /* owning media window                 */
    gint64 slice_start = g_get_monotonic_time();

    do {
        if (mw->cells->len >= MEDIA_MAX_IMAGES) {
            mw->truncated = TRUE;
            media_scan_finish(mw);
            return G_SOURCE_REMOVE;
        }

        if (!mw->scan_open) {        /* open the next note                  */
            if (mw->scan_i >= mw->notes->len) {
                media_scan_finish(mw);
                return G_SOURCE_REMOVE;
            }
            MediaNote *note = g_ptr_array_index(mw->notes, mw->scan_i);
            mw->scan_blob = on_db_note_load(mw->app->db, note->id,
                                            &mw->scan_len);
            /* The count is a record walk that decodes nothing, so a note
             * with no images costs only its blob read.                     */
            mw->scan_n_img = (mw->scan_blob != NULL)
                ? on_note_count_images(mw->scan_blob, mw->scan_len) : 0;
            mw->scan_ord   = 0;
            mw->scan_open  = TRUE;
            if (mw->scan_n_img > 0)
                mw->n_notes++;
        }

        if (mw->scan_ord < mw->scan_n_img) {
            MediaNote *note = g_ptr_array_index(mw->notes, mw->scan_i);
            media_add_cell(mw, note, mw->scan_ord, mw->scan_n_img,
                           mw->scan_blob, mw->scan_len);
            mw->scan_ord++;
        }
        if (mw->scan_ord >= mw->scan_n_img)
            media_scan_close_note(mw);
    } while (g_get_monotonic_time() - slice_start < MEDIA_SCAN_BUDGET_US);

    media_status_update(mw);
    return G_SOURCE_CONTINUE;
}

/* ===========================================================================
 * window
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_media_key_press() — the viewer panel's keys: Escape closes it, Left and
 * Right walk the grid.  All of them act ONLY while a panel is open, so with
 * the grid on show every key is left to the rest of GTK.  A step past either
 * end is still consumed — the arrows belong to the panel while it is up, and
 * letting one through would scroll the grid hidden behind it.
 * ------------------------------------------------------------------------- */
static gboolean
on_media_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void)widget;
    OnMedia *mw = user_data;         /* owning media window                 */
    if (mw->viewer_cell == NULL)
        return FALSE;

    switch (event->keyval) {
    case GDK_KEY_Escape:
        media_viewer_close(mw);
        return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        media_viewer_step(mw, -1);
        return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        media_viewer_step(mw, +1);
        return TRUE;
    default:
        return FALSE;
    }
}

/* on_media_configure() — track the live size (persisted at close) and, when
 * the viewer is open, re-render it for the new size once the resizing has
 * settled.                                                                  */
static gboolean
on_media_configure(GtkWidget *widget, GdkEventConfigure *event,
                   gpointer user_data)
{
    (void)widget;
    OnMedia *mw = user_data;         /* owning media window                 */
    gboolean changed = mw->win_w != event->width ||
                       mw->win_h != event->height;
    mw->win_w = event->width;
    mw->win_h = event->height;

    if (changed && mw->viewer_cell != NULL) {
        if (mw->viewer_resize != 0)
            g_source_remove(mw->viewer_resize);
        mw->viewer_resize = g_timeout_add(MEDIA_VIEWER_RESIZE_MS,
                                          media_viewer_resize_done, mw);
    }
    return FALSE;                    /* never consume: default handling     */
}

/* on_media_destroy() — stop the scan, remember the window size for the next
 * media window, and free everything the window owns.                         */
static void
on_media_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnMedia *mw = user_data;         /* owning media window                 */

    if (mw->scan_idle != 0) {
        g_source_remove(mw->scan_idle);
        mw->scan_idle = 0;
    }
    if (mw->viewer_resize != 0) {
        g_source_remove(mw->viewer_resize);
        mw->viewer_resize = 0;
    }
    if (mw->win_w > 0 && mw->win_h > 0) {
        gchar *w = g_strdup_printf("%d", mw->win_w);
        gchar *h = g_strdup_printf("%d", mw->win_h);
        on_app_config_set("media_win_w", w);
        on_app_config_set("media_win_h", h);
        g_free(w);
        g_free(h);
    }
    g_free(mw->scan_blob);
    g_ptr_array_free(mw->notes, TRUE);
    g_ptr_array_free(mw->cells, TRUE);
    g_free(mw->scope);
    g_free(mw);
}

/* label_small() — give a label the small font size the panel's strip uses.  */
static void
label_small(GtkWidget *label)
{
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
    gtk_label_set_attributes(GTK_LABEL(label), attrs);
    pango_attr_list_unref(attrs);
}

/* ---------------------------------------------------------------------------
 * media_build_viewer() — the modal panel: a dark event box filling the whole
 * overlay (so it swallows every click meant for the grid behind it) holding
 * the centred picture, with a strip under it carrying the caption on the
 * left and the "Show in source note" link on the right.  Hidden until an
 * image is clicked, and no-show-all so gtk_widget_show_all() on the window
 * cannot reveal it.
 *
 * A GtkGrid, not a box: the image spans both columns, so the grid is exactly
 * as wide as the picture and the right-aligned link lands under the
 * picture's bottom-right corner.  The caption takes the slack (hexpand),
 * and the grid blocks that expand flag from propagating outwards — otherwise
 * the grid would stretch to the whole window and take the link with it.
 * ------------------------------------------------------------------------- */
static GtkWidget *
media_build_viewer(OnMedia *mw)
{
    mw->viewer_img = gtk_image_new();

    mw->viewer_cap = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(mw->viewer_cap), PANGO_ELLIPSIZE_END);
    /* Bounded natural width (ellipsize makes max-width-chars the cap), so a
     * long note title cannot make the strip wider than the picture.        */
    gtk_label_set_max_width_chars(GTK_LABEL(mw->viewer_cap), 60);
    gtk_label_set_xalign(GTK_LABEL(mw->viewer_cap), 0.0);
    gtk_widget_set_hexpand(mw->viewer_cap, TRUE);
    label_small(mw->viewer_cap);

    /* An in-app hyperlink: a GtkLabel with link markup and an activate-link
     * handler.  The href is a placeholder — GtkLabel needs one to render a
     * link, and the handler consumes the activation before GTK can try to
     * open it.                                                             */
    mw->viewer_link = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(mw->viewer_link),
                         "<a href=\"source\">Show in source note</a>");
    gtk_label_set_track_visited_links(GTK_LABEL(mw->viewer_link), FALSE);
    gtk_widget_set_halign(mw->viewer_link, GTK_ALIGN_END);
    gtk_widget_set_tooltip_text(mw->viewer_link,
        "Open the note this image is in, scrolled to the image");
    label_small(mw->viewer_link);
    g_signal_connect(mw->viewer_link, "activate-link",
                     G_CALLBACK(media_viewer_link), mw);

    /* The "Previous | Next" row: ONE label carrying both links, so the two
     * words and their separator can never drift apart and the whole row
     * centres as a single widget.  Its markup — and its visibility — belong
     * to media_viewer_nav_sync, hence no-show-all.                          */
    mw->viewer_nav = gtk_label_new(NULL);
    gtk_label_set_track_visited_links(GTK_LABEL(mw->viewer_nav), FALSE);
    gtk_widget_set_no_show_all(mw->viewer_nav, TRUE);
    gtk_widget_set_halign(mw->viewer_nav, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(mw->viewer_nav,
        "Show the previous or next image (or press the \xe2\x86\x90 and "
        "\xe2\x86\x92 keys)");
    label_small(mw->viewer_nav);
    g_signal_connect(mw->viewer_nav, "activate-link",
                     G_CALLBACK(media_viewer_nav_link), mw);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(grid, FALSE);   /* stop the caption's expand here */
    gtk_widget_set_margin_start(grid,  MEDIA_VIEWER_INSET);
    gtk_widget_set_margin_end(grid,    MEDIA_VIEWER_INSET);
    gtk_widget_set_margin_top(grid,    MEDIA_VIEWER_INSET);
    gtk_widget_set_margin_bottom(grid, MEDIA_VIEWER_INSET);
    gtk_grid_attach(GTK_GRID(grid), mw->viewer_img,  0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->viewer_cap,  0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mw->viewer_link, 1, 1, 1, 1);
    /* Spanning both columns, so the row is as wide as the picture above it
     * and GTK_ALIGN_CENTER puts the links under the picture's middle.       */
    gtk_grid_attach(GTK_GRID(grid), mw->viewer_nav,  0, 2, 2, 1);

    mw->viewer = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(mw->viewer), TRUE);
    gtk_widget_set_no_show_all(mw->viewer, TRUE);
    gtk_widget_set_can_focus(mw->viewer, TRUE);
    gtk_container_add(GTK_CONTAINER(mw->viewer), grid);
    gtk_style_context_add_class(gtk_widget_get_style_context(mw->viewer),
                                MEDIA_CSS_VIEWER);
    g_signal_connect(mw->viewer, "button-press-event",
                     G_CALLBACK(media_viewer_press), mw);
    g_signal_connect(mw->viewer, "scroll-event",
                     G_CALLBACK(media_viewer_scroll), mw);
    gtk_widget_show_all(grid);
    return mw->viewer;
}

void
on_media_window_open(OnApp *app, const gchar *scope_label, GList *notes)
{
    OnMedia *mw = g_new0(OnMedia, 1);
    mw->app   = app;
    mw->scope = g_strdup((scope_label != NULL && *scope_label != '\0')
                         ? scope_label : "Notes");
    mw->notes = g_ptr_array_new_with_free_func(media_note_free);
    mw->cells = g_ptr_array_new_with_free_func(media_cell_free);

    /* Snapshot the note set: the grid keeps showing what was listed when
     * the button was pressed, whatever the sidebar does next.              */
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one caller-owned note               */
        MediaNote *n = g_new0(MediaNote, 1);
        n->id    = m->id;
        n->title = g_strdup(m->title);
        g_ptr_array_add(mw->notes, n);
    }

    /* --- window (standard titlebar, no HeaderBar) ------------------------ */
    mw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    {
        gchar *title = g_strdup_printf("Notes - Media in %s", mw->scope);
        gtk_window_set_title(GTK_WINDOW(mw->window), title);
        g_free(title);
    }
    gint win_w = MEDIA_WIN_DEFAULT_W;
    gint win_h = MEDIA_WIN_DEFAULT_H;
    on_app_config_get_size("media_win_w", "media_win_h", &win_w, &win_h);
    gtk_window_set_default_size(GTK_WINDOW(mw->window), win_w, win_h);
    gtk_window_set_transient_for(GTK_WINDOW(mw->window),
                                 GTK_WINDOW(app->library_window));
    g_signal_connect(mw->window, "configure-event",
                     G_CALLBACK(on_media_configure), mw);
    g_signal_connect(mw->window, "key-press-event",
                     G_CALLBACK(on_media_key_press), mw);
    g_signal_connect(mw->window, "destroy",
                     G_CALLBACK(on_media_destroy), mw);
    media_css_install(mw->window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(mw->window), vbox);

    /* --- the grid --------------------------------------------------------- */
    mw->flow = gtk_flow_box_new();
    /* Selection off: a cell's own click handler drives everything, and a
     * highlighted "selected" cell would only add a second, meaningless
     * state next to "this is the one in the viewer".                       */
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(mw->flow),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(mw->flow), FALSE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(mw->flow), 100);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(mw->flow), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(mw->flow), 6);
    gtk_widget_set_valign(mw->flow, GTK_ALIGN_START);

    mw->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(mw->scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(mw->scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(mw->scroll), mw->flow);

    /* --- grid + viewer panel, stacked ------------------------------------- */
    mw->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(mw->overlay), mw->scroll);
    gtk_overlay_add_overlay(GTK_OVERLAY(mw->overlay),
                            media_build_viewer(mw));
    gtk_box_pack_start(GTK_BOX(vbox), mw->overlay, TRUE, TRUE, 0);

    /* --- status line ------------------------------------------------------ */
    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    mw->spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(mw->spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(status_row), mw->spinner, FALSE, FALSE, 0);
    mw->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(mw->status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(mw->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(status_row), mw->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_row, FALSE, FALSE, 0);

    gtk_widget_show_all(mw->window);

    /* Scanning starts only now: media_surface_fit reads the window's scale
     * factor and GdkWindow, both of which need it realized.                */
    mw->scan_idle = g_idle_add(media_scan_idle, mw);
    gtk_widget_show(mw->spinner);
    gtk_spinner_start(GTK_SPINNER(mw->spinner));
    media_status_update(mw);
}
