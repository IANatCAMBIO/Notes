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
 * Updating a row IN PLACE: change its fields, then emit items-changed
 * (position, 1, 1) on the store that holds it (on_row_touch).  The views
 * rebind that item, a GtkSortListModel re-sorts it, and a GtkMultiSelection
 * keeps it selected — it tracks selection by object identity, so the SAME
 * object re-added in place stays selected (gtkmultiselection.c,
 * items_changed_cb).  That is the whole in-place update protocol; there is
 * no per-field notification.
 * =========================================================================== */

#ifndef ON_LIST_ROWS_H
#define ON_LIST_ROWS_H

#include <gtk/gtk.h>

/* ---------------------------------------------------------------------------
 * OnSbRow — one sidebar row: a section ("All Notes", "Tags", …), a folder,
 * a tag, or a trashed folder.  `children` is the model a GtkTreeListModel
 * expands into (NULL for a leaf).
 * ------------------------------------------------------------------------- */
#define ON_TYPE_SB_ROW (on_sb_row_get_type())
G_DECLARE_FINAL_TYPE(OnSbRow, on_sb_row, ON, SB_ROW, GObject)

struct _OnSbRow {
    GObject     parent;
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
G_DECLARE_FINAL_TYPE(OnNoteRow, on_note_row, ON, NOTE_ROW, GObject)

struct _OnNoteRow {
    GObject     parent;
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
G_DECLARE_FINAL_TYPE(OnActionRow, on_action_row, ON, ACTION_ROW, GObject)

struct _OnActionRow {
    GObject   parent;
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
 * on_row_touch() — a row's fields changed: tell the store so every view
 * rebinds it (and a sorted model re-sorts it).  Finds the row by identity.
 *   store — the GListStore holding `row`.
 *   row   — the row (any of the three classes).
 * Returns FALSE when the row is not in the store (nothing emitted).
 * ------------------------------------------------------------------------- */
gboolean on_row_touch(GListStore *store, gpointer row);

#endif /* ON_LIST_ROWS_H */
