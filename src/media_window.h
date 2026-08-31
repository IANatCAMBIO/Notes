/* ===========================================================================
 * media_window.h — the folder media browser
 *
 * A window opened from the library toolbar's Media button: every image
 * embedded in the notes the library is currently listing, laid out as a
 * reflowing grid of thumbnails.
 *
 *   +--------------------------------------------------+     +---------------+
 *   | [thumb] [thumb] [thumb] [thumb] [thumb] [thumb]  |     |  +---------+  |
 *   | [thumb] [thumb] [thumb] [thumb] [thumb] [thumb]  | --> |  |  image  |  |
 *   | [thumb] [thumb] [thumb] [thumb] [thumb] [thumb]  |     |  +---------+  |
 *   |--------------------------------------------------|     | cap    [link] |
 *   | 37 images in 12 notes                            |     | Previous|Next |
 *   +--------------------------------------------------+     +---------------+
 *
 * A single click on a thumbnail shows that picture in a modal panel over the
 * whole grid, fitted to the window less a 20 px margin on every side; the
 * thumbnails themselves never change size.  Clicking the panel (or Escape)
 * closes it, and it holds one picture at a time.  A "Show in source note"
 * link under the picture's bottom-right corner opens the note the image is
 * in, scrolled to that image.
 *
 * The open panel walks the grid without going back to it: the Left and Right
 * arrow keys, or the "Previous | Next" links centred under the picture, put
 * the neighbouring cell's image in the panel.  Neither wraps around — at
 * either end the word is dim plain text and the key does nothing.
 *
 * Images are addressed as (note id, ordinal): the position of the image
 * among that note's embedded images.  Nothing is copied into the window but
 * the pixels it displays, so the grid never holds a note's content open.
 * =========================================================================== */

#ifndef BLUE_MEDIA_WINDOW_H
#define BLUE_MEDIA_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * on_media_window_open() — show a media window over one set of notes.
 *
 * The note set is a SNAPSHOT taken by the caller (the library passes
 * exactly what its notes pane is listing), so the grid keeps showing what
 * was there when the button was pressed even if the sidebar selection
 * changes afterwards.  Scanning runs in idle time slices, so the window
 * appears immediately and fills progressively however many notes it was
 * handed.
 *
 *   app         — global application context.
 *   scope_label — what is being browsed, for the window title and status
 *                 line (e.g. a folder or tag name); NULL falls back to
 *                 "Notes".
 *   notes       — GList of OnNoteMeta* to scan; BORROWED, read only for
 *                 the duration of the call (the window keeps its own copy
 *                 of each note's id and title).
 * ------------------------------------------------------------------------- */
void on_media_window_open(OnApp *app, const gchar *scope_label,
                          GList *notes);

#endif /* BLUE_MEDIA_WINDOW_H */
