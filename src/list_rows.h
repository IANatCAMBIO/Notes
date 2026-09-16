/* ===========================================================================
 * list_rows.h — the item objects behind the GTK4 list widgets
 *
 * GtkColumnView, GtkListView and GtkGridView show a GListModel of
 * GObjects and render each through a GtkListItemFactory; the tree models
 * and cell renderers they replaced carried typed columns instead.  These
 * are the three item classes the library and search windows list: a
 * sidebar row (with its own children model, for GtkTreeListModel), a note
 * and an action item.  They are plain data holders — public fields, no
 * properties — because every consumer is C in this program and binds by
 * hand in its factory.
 *
 * Updating a row IN PLACE is on_row_touch: it emits the row's "changed"
 * signal, which the factories' bind handlers listen for (on_row_factory_new
 * wires that up), then items-changed (position, 1, 1) on the store that
 * holds it.  Both halves are needed: a GtkSortListModel re-sorts on
 * items-changed and a GtkMultiSelection keeps the SAME object selected
 * across it (gtkmultiselection.c, items_changed_cb) — but the list item
 * manager REUSES the widget of an item it finds re-added and does NOT
 * rebind it (gtklistfactorywidget.c: bind only when `item != old_item`),
 * so a field change is invisible without the signal.  That is the whole
 * protocol; there is no per-field notification.
 * =========================================================================== */

#ifndef ON_LIST_ROWS_H
#define ON_LIST_ROWS_H

#include <gtk/gtk.h>

/* ---------------------------------------------------------------------------
 * OnRow — the base of the three: nothing but the "changed" signal.
 * ------------------------------------------------------------------------- */
#define ON_TYPE_ROW (on_row_get_type())
G_DECLARE_DERIVABLE_TYPE(OnRow, on_row, ON, ROW, GObject)

struct _OnRowClass {
    GObjectClass parent_class;
};

/* ---------------------------------------------------------------------------
 * OnSbRow — one sidebar row: a section ("All Notes", "Tags", …), a folder,
 * a tag, or a trashed folder.  `children` is the model a GtkTreeListModel
 * expands into (NULL for a leaf).
 * ------------------------------------------------------------------------- */
#define ON_TYPE_SB_ROW (on_sb_row_get_type())
G_DECLARE_FINAL_TYPE(OnSbRow, on_sb_row, ON, SB_ROW, OnRow)

struct _OnSbRow {
    OnRow       parent;
    gint        kind;                /* SB_KIND_* (library_window.c)        */
    gint64      id;                  /* folder id or tag id                 */
    gchar      *name;                /* display text (emoji prefix, count)  */
    gchar      *raw;                 /* bare name (no count suffix)         */
    GListStore *children;            /* OnSbRow children, or NULL (leaf)    */
};

/* on_sb_row_new() — a row; `children` non-NULL makes it expandable.
 * The strings are copied.                                                   */
OnSbRow *on_sb_row_new(gint kind, gint64 id, const gchar *name,
                       const gchar *raw, gboolean expandable);

/* ---------------------------------------------------------------------------
 * OnNoteRow — one note in the notes list/grid or the search results.
 * ------------------------------------------------------------------------- */
#define ON_TYPE_NOTE_ROW (on_note_row_get_type())
G_DECLARE_FINAL_TYPE(OnNoteRow, on_note_row, ON, NOTE_ROW, OnRow)

struct _OnNoteRow {
    OnRow       parent;
    gint64      id;
    gchar      *title;
    gchar      *modified;            /* formatted updated_at                */
    gint64      updated_at;          /* raw (sort key)                      */
    gchar      *path;                /* "/Folder/Sub" location              */
    gchar      *created;             /* formatted created_at                */
    gint64      created_at;          /* raw (sort key)                      */
    gchar      *preview;             /* first body line, or NULL            */
    GdkTexture *thumb;               /* grid thumbnail, or NULL             */
};

/* on_note_row_new() — an empty row (id 0, every string NULL).               */
OnNoteRow *on_note_row_new(void);

/* ---------------------------------------------------------------------------
 * OnActionRow — one '!' action item in the library's Action Items view.
 * ------------------------------------------------------------------------- */
#define ON_TYPE_ACTION_ROW (on_action_row_get_type())
G_DECLARE_FINAL_TYPE(OnActionRow, on_action_row, ON, ACTION_ROW, OnRow)

struct _OnActionRow {
    OnRow     parent;
    gint64    note_id;               /* owning note                         */
    gint      ord;                   /* position among the note's items     */
    gboolean  done;
    gchar    *text;
    gchar    *due;                   /* formatted due date, or NULL         */
    gint64    due_raw;               /* 0 = none                            */
};

/* on_action_row_new() — an empty row.                                       */
OnActionRow *on_action_row_new(void);

/* ---------------------------------------------------------------------------
 * on_row_touch() — a row's fields changed: every bound widget rebinds
 * ("changed"), and the store re-announces the item so a sorted model
 * re-sorts it.  Finds the row by identity.
 *   store — the GListStore holding `row`.
 *   row   — the row (any of the three classes).
 * Returns FALSE when the row is not in the store (the signal is still
 * emitted; nothing else happens).
 * ------------------------------------------------------------------------- */
gboolean on_row_touch(GListStore *store, gpointer row);

/* ---------------------------------------------------------------------------
 * on_row_factory_new() — a GtkSignalListItemFactory whose `bind` runs
 * again whenever the bound row emits "changed" (and stops listening on
 * unbind).  THE factory constructor for every list of OnRow items; an item
 * that is not an OnRow (a GtkTreeListRow in a tree list) just binds once.
 *   setup     — the "setup" handler.
 *   bind      — the "bind" handler, also the rebind.
 *   user_data — passed to both.
 * Returns the factory (the caller hands it to a view or column).
 * ------------------------------------------------------------------------- */
GtkListItemFactory *on_row_factory_new(GCallback setup, GCallback bind,
                                       gpointer user_data);

#endif /* ON_LIST_ROWS_H */
