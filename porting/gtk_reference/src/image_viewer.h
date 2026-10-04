/* ===========================================================================
 * image_viewer.h — the modal image viewer panel
 *
 * ONE picture, shown big over whatever the host window was showing.  Used by
 * the media browser (a thumbnail click) and by the note editor (a click on an
 * embedded image); both get the identical panel, keys and links, because it
 * is the identical code.
 *
 *   +-----------------------------------+
 *   |                                   |   click anywhere        -> close
 *   |          +-------------+          |   Escape                -> close
 *   |          |    image    |          |   Left / Right          -> step
 *   |          +-------------+          |   "Previous | Next"     -> step
 *   |  caption            [action link] |   the action link       -> host
 *   |          Previous | Next          |
 *   +-----------------------------------+
 *
 * The panel is a GtkOverlay child: a dark box covering the whole overlay
 * whose click gesture swallows every press meant for the widget behind it,
 * so only one picture is ever on show.  The image is a GtkPicture fitted
 * (scaled DOWN only, aspect kept) to the overlay less ON_IMAGE_VIEWER_INSET
 * on each side and the two label rows under it; a tick callback that runs
 * only while the panel is open notices the overlay changing size and asks
 * the host to render again after a short settle.  Nothing is held across a
 * close but the host's own data: the texture is dropped.
 *
 * The panel knows nothing about where its pictures come from.  A host fills
 * in OnImageViewerOps and addresses its images by INDEX — for the media
 * browser that is a cell's position in the grid, for the editor the image's
 * ordinal among the note's image anchors (the same ordinal the whole app
 * addresses a note's images by).  The panel calls count() every time it needs
 * to know how far it can walk, so a host whose set is still growing (the
 * media browser mid-scan) needs no invalidation beyond
 * on_image_viewer_nav_sync().
 * =========================================================================== */

#ifndef BLUE_IMAGE_VIEWER_H
#define BLUE_IMAGE_VIEWER_H

#include <gtk/gtk.h>

/* How far the image stays clear of the overlay's edge (logical px).  Public
 * because a host's own layout notes refer to it.                            */
#define ON_IMAGE_VIEWER_INSET 20

typedef struct OnImageViewer OnImageViewer;

/* ---------------------------------------------------------------------------
 * OnImageViewerOps — everything the panel needs from its host.  `host` is
 * the pointer handed to on_image_viewer_new(); every call gets it back.
 *
 *   count()   — how many images the panel may walk, right now.  0 or 1 turns
 *               the Previous | Next row off.
 *   render()  — the image at `idx` as a paintable the panel takes over
 *               (g_object_unref'd by it): normally the full-resolution
 *               GdkTexture, since the panel's GtkPicture scales DOWN to fit
 *               (GTK_CONTENT_FIT_SCALE_DOWN — never up, aspect kept, sharp
 *               on HiDPI because the texture keeps every pixel).  box_w ×
 *               box_h are the LOGICAL pixels on offer, a hint for a host
 *               that decodes on demand and wants to cap a huge decode.
 *               Returns NULL when the image is gone or will not decode —
 *               the panel then closes itself.
 *   caption() — a newly-allocated one-line description of image `idx` (the
 *               panel appends its own key hint and frees the string).
 *   action()  — run the action link for image `idx`.  The panel has ALREADY
 *               closed itself by the time this is called, so the host is
 *               free to open windows, destroy the host widget, or anything
 *               else; NULL for a host that wants no action link.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint             (*count)(gpointer host);
    GdkPaintable    *(*render)(gpointer host, gint idx,
                              gint box_w, gint box_h);
    gchar           *(*caption)(gpointer host, gint idx);
    void             (*action)(gpointer host, gint idx);
} OnImageViewerOps;

/* ---------------------------------------------------------------------------
 * on_image_viewer_new() — build the panel and add it to `overlay` as an
 * overlay child.  It starts hidden and is no-show-all, so a
 * gtk_widget_show_all() on the host window cannot reveal it.
 *
 *   overlay      — the GtkOverlay to sit in; its allocation is what the
 *                  image is fitted to.
 *   ops          — the host's callbacks; BORROWED, must outlive the panel
 *                  (a static struct).
 *   host         — passed back to every op.
 *   action_label — text for the action link (e.g. "Show in source note");
 *                  NULL, or a NULL ops->action, means no link at all.
 *   action_tip   — its tooltip, or NULL.
 * Returns the panel; on_image_viewer_free() it from the host's destroy
 * handler.
 * ------------------------------------------------------------------------- */
OnImageViewer *on_image_viewer_new(GtkWidget *overlay,
                                   const OnImageViewerOps *ops,
                                   gpointer host,
                                   const gchar *action_label,
                                   const gchar *action_tip);

/* on_image_viewer_free() — drop the panel's own state.  Call it from the
 * host's "destroy" handler: it stops the tick and the settle timer, takes
 * the panel out of the overlay if it is still in one, and releases the
 * panel's own reference.  GTK4 emits a window's "destroy" AFTER its dispose
 * has torn the child tree down (the reverse of GTK3), so the overlay may
 * already be gone by then — the panel holds its own reference to its widget
 * precisely so this works in either order, and never touches the overlay. */
void on_image_viewer_free(OnImageViewer *v);

/* on_image_viewer_open() — show image `idx`, replacing whatever was on show.
 * An out-of-range index, or one whose render() fails, leaves the panel
 * closed.                                                                   */
void on_image_viewer_open(OnImageViewer *v, gint idx);

/* on_image_viewer_close() — hide the panel and drop the big surface.        */
void on_image_viewer_close(OnImageViewer *v);

/* on_image_viewer_is_open() — TRUE while a picture is on show.  NULL-safe,
 * so a host can ask before it has built a panel.                            */
gboolean on_image_viewer_is_open(const OnImageViewer *v);

/* on_image_viewer_index() — the index on show, or -1 when closed.          */
gint on_image_viewer_index(const OnImageViewer *v);

/* ---------------------------------------------------------------------------
 * on_image_viewer_key_press() — offer a key to the open panel: Escape closes
 * it, Left/Right step through the host's images.  Returns TRUE when the
 * panel took the key.
 *
 * Call this FIRST from a GtkEventControllerKey in the CAPTURE phase on the
 * host WINDOW — not on the widget behind the panel, which KEEPS the
 * keyboard focus the whole time the panel is up (see the focus rule at the
 * top of image_viewer.c; the panel is never focusable, because a
 * GtkTextView rendered unfocused stays grey on quartz long after the panel
 * is gone).  The capture phase runs before the focus widget sees the key,
 * which is what lets the panel claim keys off it.  keyval/state are the
 * controller's "key-pressed" arguments.
 *
 * BECAUSE the focus stays behind the panel, a host MUST swallow every OTHER
 * key while the panel is open — return on_image_viewer_is_open() when this
 * returns FALSE — or typing would edit the note hidden underneath.  An arrow
 * at either end is taken here too, so it cannot leak through and scroll or
 * move the caret behind the panel.
 * ------------------------------------------------------------------------- */
gboolean on_image_viewer_key_press(OnImageViewer *v, guint keyval,
                                   GdkModifierType state);

/* on_image_viewer_nav_sync() — re-read count() and redraw the
 * Previous | Next row.  Only a host whose image set can GROW while the panel
 * is open needs to call this; everything else keeps the row in step itself.  */
void on_image_viewer_nav_sync(OnImageViewer *v);

#endif /* BLUE_IMAGE_VIEWER_H */
