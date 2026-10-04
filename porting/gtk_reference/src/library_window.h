/* ===========================================================================
 * library_window.h — the Notes Library window
 *
 * The library is the app's main window (standard titlebar, no HeaderBar):
 *
 *   +--------------------------------------------------------------+
 *   | File  View                                   (menu bar)      |
 *   +----------------+---------------------------------------------+
 *   | v Notes        |  [New Note] [New Folder] [Delete] [List|Grid]|
 *   |   > Work       |                                             |
 *   |   > Personal   |   note   note   note        (list or grid)  |
 *   | v Tags         |   note   note                               |
 *   |   #todo        |                                             |
 *   +----------------+---------------------------------------------+
 *
 * The sidebar shows the nested folder hierarchy and, below it, every
 * known #tag.  Selecting a folder or tag shows its notes on the right,
 * as a list or a grid.  Notes can be drag-reordered within the list and
 * dragged onto sidebar folders to move them.  Double-clicking a note
 * opens it in its own editor window.
 * =========================================================================== */

#ifndef BLUE_LIBRARY_WINDOW_H
#define BLUE_LIBRARY_WINDOW_H

#include "app.h"
#include "search_window.h"

/* ---------------------------------------------------------------------------
 * on_library_window_create() — build and show the library window.
 *
 * Also installs app->notify_notes_changed so editor windows can trigger
 * refreshes, and stores the window in app->library_window.
 *
 *   app — global application context.
 * Returns the new GtkWindow.
 * ------------------------------------------------------------------------- */
GtkWidget *on_library_window_create(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_library_quicknote() — THE quicknote action: create an empty note in the
 * ROOT folder regardless of what the sidebar has selected, refresh the
 * library, and open its editor to the front.  Shared by the toolbar's
 * Quicknote button and the `notes quicknote` CLI/IPC command, so both behave
 * identically.  Safe with no library window open (the CLI can start the GUI
 * for exactly this).
 *   app — global application context.
 * Returns the new note's id, or 0 if it could not be created.
 * ------------------------------------------------------------------------- */
gint64 on_library_quicknote(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_library_get_scope() — the library's current sidebar selection, used
 * by the search window to resolve "Selected Folder/Tag" at search time.
 *   app   — global application context.
 *   scope — receives ON_SCOPE_FOLDER (folders and the root) or
 *           ON_SCOPE_TAG.
 *   id    — receives the folder/tag id (0 for the root).
 *   name  — receives the selection's display name (g_free() it).
 * ------------------------------------------------------------------------- */
void on_library_get_scope(OnApp *app, OnSearchScope *scope, gint64 *id,
                          gchar **name);

/* ---------------------------------------------------------------------------
 * on_library_sidebar_fit() — size the library sidebar so the folder rows
 * on show fit exactly, honouring the "sidebar_fit_content" setting (a
 * no-op while it is off, or with no library window open).  Settings calls
 * this on every toggle; ticking the box fits at once, unticking leaves the
 * width where the last fit put it.
 *   app — global application context.
 * ------------------------------------------------------------------------- */
void on_library_sidebar_fit(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_library_apply_native_menubar() — on macOS, move the library's menu
 * into the native menu bar (hiding the in-window one), or restore it.  GTK's
 * quartz backend does the exporting; this only decides whether the
 * application menubar is set.  Elsewhere the setting means nothing — the
 * GtkApplicationWindow renders the menubar itself — and this is a no-op
 * beyond making sure it is set.  Safe with no library window open.
 *   app    — global application context.
 *   native — TRUE for the macOS menu bar, FALSE for the in-window bar.
 * ------------------------------------------------------------------------- */
void on_library_apply_native_menubar(OnApp *app, gboolean native);

#endif /* BLUE_LIBRARY_WINDOW_H */
