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
 *                 picture in the shared modal panel (image_viewer.[ch]),
 *                 which covers the whole grid and swallows all clicks, so
 *                 only one image is ever shown and the grid never reflows
 *                 underneath.  This window supplies the panel with an image
 *                 set (its cells, addressed by index), a caption and one
 *                 action link; the panel owns the clicks, the keys and the
 *                 Previous | Next row.  That one image is re-read at panel
 *                 size and dropped again on close, so the window's memory is
 *                 the thumbnails plus at most one big picture.  Getting from
 *                 a picture to its note is a "Show in source note" LINK
 *                 under the image's bottom-right corner, not a double click:
 *                 with a single click already opening and closing the
 *                 viewer, a double click has nowhere to land — its first
 *                 press would have dismissed whatever its second press was
 *                 aimed at.
 *
 *   addressing  — a cell is (note id, image ordinal).  The ordinal is the
 *                 image's position among the note's IMAGE records, which is
 *                 also the order the editor's image anchors appear in — so
 *                 on_editor_window_open_image() can scroll straight to it.
 * =========================================================================== */

#include "media_window.h"
#include "editor_window.h"
#include "image_viewer.h"
#include "serialize.h"

#include <cairo-gobject.h>

/* Logical pixel box a thumbnail is fitted into (aspect kept, no upscaling). */
#define MEDIA_THUMB_BOX 144

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

/* Style class for the thumbnails (see media_css_install).                    */
#define MEDIA_CSS_CELL "on-media-cell"

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
 *   viewer      — the shared modal image panel over that overlay; it holds
 *                 which cell is on show, so this window keeps no "is the
 *                 viewer open" state of its own.
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
 *   win_w/win_h — the window's live size, persisted on close so the next
 *                 media window opens at the size this one was left at.
 * ------------------------------------------------------------------------- */
struct OnMedia {
    OnApp      *app;
    GtkWidget  *window;
    GtkWidget  *overlay;
    GtkWidget  *scroll;
    GtkWidget  *flow;
    OnImageViewer *viewer;
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
 * media_css_install() — install the thumbnails' hover tint once per screen.
 * Scoped to MEDIA_CSS_CELL, so nothing else on screen is affected (the
 * viewer panel's backdrop is image_viewer.c's own business).
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
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gtk_widget_get_screen(window), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
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
            surface = on_image_viewer_fit(mw->window, pix, box_w, box_h);
            g_object_unref(pix);
        }
    }
    g_free(own);
    return surface;
}

/* ===========================================================================
 * the viewer panel — this window's side of the shared image viewer
 *
 * The panel addresses images by INDEX into mw->cells, which is grid order.
 * Because it asks count() afresh every time, a set that is still growing
 * mid-scan needs no invalidation beyond the one nav sync in media_add_cell.
 * =========================================================================== */

/* media_viewer_count() — how many pictures the panel may walk.              */
static gint
media_viewer_count(gpointer host)
{
    OnMedia *mw = host;              /* owning media window                 */
    return (gint)mw->cells->len;
}

/* ---------------------------------------------------------------------------
 * media_viewer_render() — the panel's image, decoded from the note fresh at
 * panel size (NULL blob) rather than scaled up from the thumbnail, and
 * dropped again when the panel closes: the window's memory stays the
 * thumbnails plus at most one big picture.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
media_viewer_render(gpointer host, gint idx, gint box_w, gint box_h)
{
    OnMedia *mw = host;              /* owning media window                 */
    if (idx < 0 || idx >= (gint)mw->cells->len)
        return NULL;
    MediaCell *c = g_ptr_array_index(mw->cells, idx);
    return media_render(mw, c->note_id, c->ord, box_w, box_h, NULL, 0);
}

/* media_viewer_caption() — "<note title> — image N of M"; the panel adds its
 * own key hint.                                                             */
static gchar *
media_viewer_caption(gpointer host, gint idx)
{
    OnMedia *mw = host;              /* owning media window                 */
    if (idx < 0 || idx >= (gint)mw->cells->len)
        return NULL;
    MediaCell *c = g_ptr_array_index(mw->cells, idx);
    return g_strdup_printf("%s \xe2\x80\x94 image %d of %d",
                           c->title, c->ord + 1, c->n_img);
}

/* ---------------------------------------------------------------------------
 * media_viewer_action() — the "Show in source note" link: open the note the
 * shown picture came from, scrolled to that picture.  The panel has already
 * closed itself, so the cell is read here and nothing is left pointing at
 * panel state.
 * ------------------------------------------------------------------------- */
static void
media_viewer_action(gpointer host, gint idx)
{
    OnMedia *mw = host;              /* owning media window                 */
    if (idx < 0 || idx >= (gint)mw->cells->len)
        return;
    MediaCell *c = g_ptr_array_index(mw->cells, idx);
    on_editor_window_open_image(mw->app, c->note_id, c->ord);
}

static const OnImageViewerOps media_viewer_ops = {
    .count   = media_viewer_count,
    .render  = media_viewer_render,
    .caption = media_viewer_caption,
    .action  = media_viewer_action,
};

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
    on_image_viewer_open(c->mw->viewer, c->idx);
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
    if (on_image_viewer_index(mw->viewer) == c->idx - 1)
        on_image_viewer_nav_sync(mw->viewer);
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

/* on_media_key_press() — offer every key to the viewer panel first (Escape
 * closes it, the arrows walk the grid), then swallow the rest while it is
 * open: the panel is modal and the focus stays behind it, so a key it does
 * not want must not reach the grid either.  It takes nothing while closed,
 * so the grid keeps all of its own.                                         */
static gboolean
on_media_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void)widget;
    OnMedia *mw = user_data;         /* owning media window                 */
    if (on_image_viewer_key_press(mw->viewer, event))
        return TRUE;
    return on_image_viewer_is_open(mw->viewer);
}

/* on_media_configure() — track the live size, which is persisted at close so
 * the next media window opens at the size this one was left at.  Re-fitting
 * an open viewer is NOT wired here: the panel watches its own overlay.       */
static gboolean
on_media_configure(GtkWidget *widget, GdkEventConfigure *event,
                   gpointer user_data)
{
    (void)widget;
    OnMedia *mw = user_data;         /* owning media window                 */
    mw->win_w = event->width;
    mw->win_h = event->height;
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
    on_image_viewer_free(mw->viewer);
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
    gtk_box_pack_start(GTK_BOX(vbox), mw->overlay, TRUE, TRUE, 0);
    mw->viewer = on_image_viewer_new(
        mw->overlay, &media_viewer_ops, mw, "Show in source note",
        "Open the note this image is in, scrolled to the image");

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
