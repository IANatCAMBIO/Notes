/* ===========================================================================
 * library_window.c — the Notes Library window (implementation)
 *
 * See library_window.h for the layout overview.  Key mechanics:
 *
 *   sidebar    — a GtkListView over a GtkTreeListModel of OnSbRow
 *                (list_rows.h): the sections "All Notes", "Action Items",
 *                the folder hierarchy (rooted at a fixed "Notes" row), a
 *                flat "Tags" section, "Pinned Notes", and — while
 *                non-empty — "Trash" (trashed folders as its children).
 *                Row kinds are distinguished by OnSbRow.kind (SB_KIND_*).
 *
 *   notes pane — one GListStore of OnNoteRow, sorted by a GtkSortListModel
 *                that follows the column view's sorter, under ONE
 *                GtkMultiSelection shared by a GtkColumnView (list mode)
 *                and a GtkGridView (grid mode): the two views cannot
 *                disagree about what is selected.  A GtkStack flips
 *                between them and the Action Items GtkColumnView.
 *
 *   drag&drop  — every row of the notes views and every folder row of the
 *                sidebar is a GtkDragSource (installed by the row's
 *                factory), and every sidebar row is a GtkDropTarget: a
 *                note drop moves the notes into the folder (or trashes
 *                them); a folder row drops INTO a folder (re-nest),
 *                BETWEEN folders (reorder/re-nest beside the sibling),
 *                onto Trash (delete gesture), or out of Trash (restore).
 *                One boxed value type, OnDragRows, is the whole content of
 *                every drag (GTK4_MIGRATION.md, D8).
 * =========================================================================== */

#include "library_window.h"
#include "backup.h"
#include "list_rows.h"
#include "editor_window.h"
#include "export.h"
#include "media_window.h"
#include "search_window.h"
#include "serialize.h"
#include "settings_window.h"

#include <glib/gstdio.h>
#include <string.h>

/* Logical pixel size of the square grid-view thumbnails.                    */
#define THUMB_SIZE 140

/* Background for even rows in list view (odd rows stay white).              */
#define ROW_TINT "#e8f2fb"

/* How much of a note's body text is fetched for the Comfortable-density
 * preview: enough for the first non-blank line after the title.             */
#define NL_PREVIEW_CHARS 200

/* strftime format of the Modified and Created cells ("Jun 3, 2026 14:05").
 * ONE definition for the two columns.                                       */
#define LIST_TIME_FORMAT "%b %e, %Y %H:%M"

/* Blank strip above the sidebar tree, to line its first row's text up with
 * the notes list's column-header text (see library_build_sidebar).          */
#define SB_TOP_PAD 3

/* How far the sidebar backdrop sits below the toolbar/window background it
 * is shaded from — a CSS shade() factor, < 1 darkens.  0.96 turns Adwaita's
 * rgb(246,245,244) into rgb(238,236,234).  A string, not a number: it is
 * pasted into one CSS declaration in library_install_css, shared by the
 * list view and the spacer strip above it.                                  */
#define SB_BG_SHADE "0.96"

/* Sidebar row kinds (SB_KIND column).                                       */
enum {
    SB_KIND_ROOT = 0,                /* the fixed "Notes" root row          */
    SB_KIND_FOLDER,                  /* a real folder                       */
    SB_KIND_TAGS_HEADER,             /* the "Tags" section header           */
    SB_KIND_TAG,                     /* one tag                             */
    SB_KIND_PINNED,                  /* the "Pinned Notes" section          */
    SB_KIND_ALL,                     /* the "All Notes" section             */
    SB_KIND_TRASH,                   /* the "Trash" section                 */
    SB_KIND_TRASH_FOLDER,            /* a trashed folder under Trash        */
    SB_KIND_ACTIONS,                 /* the "Action Items" section          */
};

/* How many columns the notes list owns (Title, Path, Modified, Created) —
 * keep in sync with the COLS[] table in library_build_notes_list.           */
#define N_LIST_COLUMNS 4

/* How many columns the Action Items list owns (done, Action, Due Date).    */
#define N_ACTION_COLUMNS 3

/* ---------------------------------------------------------------------------
 * OnDragRows — THE content of every drag in this window (D8): what is being
 * dragged, as a boxed GValue that crosses the process-internal drag with no
 * serialization.  Both notes views and the sidebar produce one; the sidebar
 * is the only widget that accepts one.  No MIME type exists for it, so a
 * drop from any other application cannot carry the GType and the target
 * never sees it.
 *
 * Fields:
 *   kind — what the ids name (see OnDragKind).
 *   ids  — gint64 note ids, or exactly ONE folder id.
 * ------------------------------------------------------------------------- */
typedef enum {
    ON_DRAG_NOTES,                   /* note ids from the notes list/grid   */
    ON_DRAG_FOLDER,                  /* one folder row of the sidebar tree  */
    ON_DRAG_TRASHED_FOLDER,          /* one folder row under Trash: a drop
                                        outside Trash restores it           */
} OnDragKind;

typedef struct {
    OnDragKind kind;
    GArray    *ids;
} OnDragRows;

/* on_drag_rows_new() — an empty OnDragRows of `kind`.                       */
static OnDragRows *
on_drag_rows_new(OnDragKind kind)
{
    OnDragRows *rows = g_new(OnDragRows, 1);
    rows->kind = kind;
    rows->ids  = g_array_new(FALSE, FALSE, sizeof(gint64));
    return rows;
}

/* on_drag_rows_copy() — GBoxedCopyFunc: a deep copy (GValue semantics).    */
static OnDragRows *
on_drag_rows_copy(const OnDragRows *src)
{
    OnDragRows *rows = on_drag_rows_new(src->kind);
    g_array_append_vals(rows->ids, src->ids->data, src->ids->len);
    return rows;
}

/* on_drag_rows_free() — GBoxedFreeFunc.                                     */
static void
on_drag_rows_free(OnDragRows *rows)
{
    g_array_free(rows->ids, TRUE);
    g_free(rows);
}

/* The GType, registered once by G_DEFINE_BOXED_TYPE (which defines the
 * getter with external linkage — declared here so the symbol is on record). */
GType on_drag_rows_get_type(void);
G_DEFINE_BOXED_TYPE(OnDragRows, on_drag_rows, on_drag_rows_copy,
                    on_drag_rows_free)
#define ON_TYPE_DRAG_ROWS (on_drag_rows_get_type())

/* ---------------------------------------------------------------------------
 * OnLibrary — all state for the library window.
 *
 * Fields:
 *   app           — global application context (not owned).
 *   window        — the top-level GtkWindow.
 *   sb_store      — the sidebar's ROOT rows (OnSbRow; folders and tags
 *                   hang under them in their own children stores).
 *   sb_tree       — the GtkTreeListModel flattening that tree for the
 *                   list view: what is expanded is state on ITS rows.
 *   sb_sel        — the sidebar's GtkSingleSelection.
 *   sidebar       — the sidebar GtkListView.
 *   notes_store   — the notes (OnNoteRow), in the order the database
 *                   returned them.
 *   notes_sorted  — the same rows under the list view's column sorter;
 *                   what both notes views and the selection show.
 *   notes_sel     — THE selection of the notes pane, shared by the list
 *                   and the grid.
 *   notes_list    — list-mode view (GtkColumnView).
 *   notes_grid    — grid-mode view (GtkGridView).
 *   stack         — GtkStack switching between list, grid and actions.
 *   sel_kind      — SB_KIND_* of the current sidebar selection; controls
 *                   which notes are listed.
 *   sel_id        — folder id (for ROOT/FOLDER) or tag id (for TAG) of
 *                   the current selection.
 *   sel_name      — bare name of the current selection (owned string),
 *                   used for rename pre-fill and the search scope label.
 *   populating    — nesting counter; >0 while code (not the user) is
 *                   rewriting the models, so persistence signal handlers
 *                   know to stand down.
 *   thumb_cache   — note id (gint64*) → ThumbEntry*, so grid thumbnails
 *                   are only re-rendered when a note actually changed.
 *   shown_kind/
 *   shown_id      — the selection refresh_notes() last populated for;
 *                   a matching refresh (e.g. after an editor autosave)
 *                   keeps the notes-pane scroll position instead of
 *                   jumping back to the top.
 *   sidebar_box   — the whole folder/tag pane, so the toolbar's
 *                   show/hide toggle can flip its visibility.
 *   menubar_model — the File/View menu model (owned ref).  Handed to
 *                   gtk_application_set_menubar, which GTK renders in the
 *                   native macOS menu bar or, where the shell has none, at
 *                   the top of this GtkApplicationWindow.
 *   menubar       — an in-window GtkPopoverMenuBar over the same model,
 *                   macOS only: shown while the "native_menubar" setting
 *                   is off (see on_library_apply_native_menubar).  NULL
 *                   elsewhere.
 *   view_btn      — the toolbar's List/Grid toggle, kept so its ICON can
 *                   be re-pointed at whichever view a click switches TO
 *                   (see view_button_sync()).
 *   status_path   — status-bar label (bottom left): the path of the
 *                   current sidebar selection.
 *   status_event  — status-bar label (bottom right): the latest event
 *                   message ("DB saved", …); see on_app_status().  Lives
 *                   inside status_revealer (crossfade) and fades out
 *                   after STATUS_FADE_SECONDS via status_timeout.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp        *app;
    GtkWidget    *window;
    GListStore         *sb_store;
    GtkTreeListModel   *sb_tree;
    GtkSingleSelection *sb_sel;
    GtkListView        *sidebar;
    GListStore         *notes_store;
    GtkSortListModel   *notes_sorted;
    GtkMultiSelection  *notes_sel;
    GtkColumnView      *notes_list;
    GtkGridView        *notes_grid;
    GListStore         *actions_store;   /* Action Items rows (OnActionRow) */
    GtkSortListModel   *actions_sorted;  /* under the actions view's sorter */
    GtkColumnView      *actions_view;    /* Action Items list view         */
    gboolean      grid_pref;             /* the user's list/grid choice, so
                                            leaving the Action Items view
                                            restores the right mode        */
    GtkWidget    *stack;
    gint          sel_kind;
    gint64        sel_id;
    gchar        *sel_name;
    gint          populating;
    GHashTable   *thumb_cache;
    GHashTable   *folder_path_cache;     /* folder_id→path string; populated
                                            lazily by refresh_notes and
                                            invalidated by refresh_sidebar
                                            so autosave refreshes reuse it  */
    GQueue        thumb_pending;         /* ThumbJob* rows awaiting a
                                            render by thumb_fill_idle       */
    guint         thumb_idle;            /* the idle source doing it        */
    gint          shown_kind;
    gint64        shown_id;
    GtkWidget    *sidebar_box;
    GMenuModel   *menubar_model;         /* File/View menus (owned ref)     */
    GtkWidget    *menubar;               /* in-window bar (macOS only)      */
    GtkWidget    *sidebar_paned;         /* horizontal paned holding the sidebar */
    gulong        sb_fit_idle;           /* the frame clock's after-paint
                                            handler of a pending sidebar
                                            fit, or 0 (sidebar_fit_queue)  */
    GdkFrameClock *sb_fit_clock;         /* the clock it is connected to
                                            (an owned ref while pending)   */
    gboolean      sb_fit_force;          /* the pending fit is the startup
                                            one: runs whatever the setting */
    GtkWidget    *status_path;
    GtkWidget    *status_event;
    GtkWidget    *status_revealer;
    guint         status_timeout;
    GtkWidget    *view_btn;            /* List/Grid toggle; icon names the
                                        * view a click switches TO           */
    GtkWidget    *done_btn;            /* completed-items toggle; icon names
                                        * what a click DOES (done_button_sync) */
    GtkWidget    *ai_btn;              /* microchip AI toolbar button          */
    GtkWidget    *ai_pane;             /* output pane below the notes stack    */
    GtkWidget    *ai_text;             /* non-editable text view inside it     */
    guint         ai_throbber_id;       /* g_timeout_add id while AI running    */
    gint          ai_throbber_step;    /* animation frame counter              */
    GtkPaned     *notes_paned;         /* vertical paned: stack / AI pane      */
    gboolean      ai_running;          /* TRUE while subprocess is in flight   */
    GCancellable *ai_cancel;           /* cancels the in-flight subprocess     */
    gint          emoji_pad;           /* on_emoji_pad() for the window's UI
                                        * font, applied by every text cell   */
} OnLibrary;

/* How long a status-bar event message stays before fading out.              */
#define STATUS_FADE_SECONDS 4

/* ---------------------------------------------------------------------------
 * ThumbEntry — one cached grid thumbnail.
 *
 * Fields:
 *   updated_at — the note's updated_at when the thumbnail was rendered;
 *                a mismatch means the cache entry is stale.
 *   texture    — the rendered thumbnail (owned reference).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64      updated_at;
    GdkTexture *texture;
} ThumbEntry;

/* thumb_entry_free() — GDestroyNotify for cache values.                     */
static void
thumb_entry_free(gpointer data)
{
    ThumbEntry *e = data;
    g_clear_object(&e->texture);
    g_free(e);
}

/* ---------------------------------------------------------------------------
 * ThumbJob — one grid row whose thumbnail still has to be rendered.
 * refresh_notes() only sets thumbnails it finds fresh in the cache; every
 * stale/missing one is queued as a job and rendered by thumb_fill_idle()
 * in small time slices, so a cold cache (first grid showing of a big
 * folder or All Notes) can't freeze the GUI for the whole render — that
 * once hung the window for ~40 s on a 1200-note database.
 *
 * Fields:
 *   row        — where to deliver the texture (an owned reference; a row
 *                the store has dropped meanwhile is simply not found and
 *                the job is discarded).
 *   updated_at — its updated_at when the row was populated (cache key).
 * ------------------------------------------------------------------------- */
typedef struct {
    OnNoteRow *row;
    gint64     updated_at;
} ThumbJob;

/* thumb_job_free() — release one pending-thumbnail job.                     */
static void
thumb_job_free(ThumbJob *job)
{
    g_object_unref(job->row);
    g_free(job);
}

/* thumb_pending_clear() — drop every queued job and stop the fill idle;
 * run before any notes-model rebuild (the rows are about to vanish) and
 * at window teardown.                                                       */
static void
thumb_pending_clear(OnLibrary *lw)
{
    ThumbJob *job;                   /* one drained queue entry             */
    while ((job = g_queue_pop_head(&lw->thumb_pending)) != NULL)
        thumb_job_free(job);
    if (lw->thumb_idle != 0) {
        g_source_remove(lw->thumb_idle);
        lw->thumb_idle = 0;
    }
}

/* Forward declarations.                                                     */
static void    refresh_sidebar(OnLibrary *lw);
static void    refresh_notes(OnLibrary *lw);
static void    refresh_all(OnLibrary *lw);
static void    done_button_sync(OnLibrary *lw);
static void    done_menu_sync(OnLibrary *lw);
static void    status_path_update(OnLibrary *lw);
static GArray *selected_note_ids(OnLibrary *lw);
static void    sidebar_fit_queue(OnLibrary *lw, gboolean force);
static void    close_editors_for_ids(OnLibrary *lw, const gint64 *ids,
                                     gsize n);
static gboolean trash_notes_core(OnLibrary *lw, const gint64 *ids,
                                 guint n);
static gboolean trash_folder(OnLibrary *lw, gint64 folder_id,
                             const gchar *name);
static void    run_ai_summary(OnLibrary *lw);
static GtkWidget *build_ai_pane(OnLibrary *lw);
static void    on_ai_copy_clicked(GtkButton *btn, gpointer user_data);
static gboolean ai_throbber_tick(gpointer user_data);

/* ===========================================================================
 * scroll preservation
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ScrollKeep — one vadjustment position being restored after a model
 * rebuild (clearing a store zeroes its view's scrollbar).  The restore
 * runs at idle so the rebuilt view has re-validated its height first.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkAdjustment *adj;              /* the scrollbar to restore (owned ref)*/
    gdouble        value;            /* position before the rebuild         */
} ScrollKeep;

/* scroll_keep_restore() — idle callback: put the scrollbar back.            */
static gboolean
scroll_keep_restore(gpointer user_data)
{
    ScrollKeep *k = user_data;
    gtk_adjustment_set_value(k->adj, k->value);  /* clamps to the range     */
    g_object_unref(k->adj);
    g_free(k);
    return G_SOURCE_REMOVE;
}

/* scroll_keep_queue() — schedule `adj` to be put back at `value`.           */
static void
scroll_keep_queue(GtkAdjustment *adj, gdouble value)
{
    ScrollKeep *k = g_new(ScrollKeep, 1);
    k->adj   = g_object_ref(adj);
    k->value = value;
    g_idle_add(scroll_keep_restore, k);
}

/* view_vadjustment() — the vadjustment of the scrolled window holding
 * `view` (every scrollable view here sits directly in one).  Returns NULL
 * if the parent isn't realized as a GtkScrolledWindow yet.                  */
static GtkAdjustment *
view_vadjustment(GtkWidget *view)
{
    GtkWidget *parent = gtk_widget_get_parent(view);
    if (!GTK_IS_SCROLLED_WINDOW(parent))
        return NULL;
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent));
}

/* ===========================================================================
 * sidebar population
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * add_folder_rows() — recursively append the subfolders of `parent_id`
 * beneath tree row `parent_iter`.
 *   lw          — the library window.
 *   parent_id   — database folder id whose children to add (0 = roots).
 *   parent_iter — sidebar row to attach them under.
 * ------------------------------------------------------------------------- */
/* count_from_map() — look one id up in a count map (0 when absent, and 0
 * for a NULL map: the maps are only built while sidebar counts are shown). */
static gint
count_from_map(GHashTable *map, gint64 id)
{
    return (map != NULL)
        ? GPOINTER_TO_INT(g_hash_table_lookup(map, &id)) : 0;
}

/* in_trash_view() — is the notes pane showing Trash contents (the Trash
 * row itself or a trashed folder)?  Deletes there are permanent and the
 * note context menu switches to Restore/Delete Permanently.                 */
static gboolean
in_trash_view(OnLibrary *lw)
{
    return lw->sel_kind == SB_KIND_TRASH ||
           lw->sel_kind == SB_KIND_TRASH_FOLDER;
}

/* sb_kind_is_section() — is `kind` one of the bold sidebar section rows
 * (the Notes root, the Tags header, Pinned Notes, All Notes, Trash)?        */
static gboolean
sb_kind_is_section(gint kind)
{
    return kind == SB_KIND_ROOT ||
           kind == SB_KIND_TAGS_HEADER ||
           kind == SB_KIND_PINNED ||
           kind == SB_KIND_ALL ||
           kind == SB_KIND_TRASH ||
           kind == SB_KIND_ACTIONS;
}

/* ---------------------------------------------------------------------------
 * sb_folder_row() — one folder row for the sidebar, its display text
 * formatted the one way folder rows are formatted: an optional emoji
 * prefix separated by two spaces, and an optional "(n)" note count.  Shared
 * by the normal tree and the Trash section, which differ only in kind.
 *   f           — the folder.
 *   kind        — SB_KIND_FOLDER or SB_KIND_TRASH_FOLDER.
 *   note_counts — count map, or NULL while counts are hidden.
 *   expandable  — whether the row gets a children store.
 * Returns the new row (owned by the caller).
 * ------------------------------------------------------------------------- */
static OnSbRow *
sb_folder_row(OnLibrary *lw, const OnFolder *f, gint kind,
              GHashTable *note_counts, gboolean expandable)
{
    gboolean has_emoji = f->emoji != NULL && *f->emoji != '\0';
    gchar   *display;                /* name (+ emoji prefix, + count)      */
    if (lw->app->sidebar_counts) {
        gint count = count_from_map(note_counts, f->id);
        display = has_emoji
            ? g_strdup_printf("%s  %s (%d)", f->emoji, f->name, count)
            : g_strdup_printf("%s (%d)", f->name, count);
    } else {
        display = has_emoji
            ? g_strdup_printf("%s  %s", f->emoji, f->name)
            : g_strdup(f->name);
    }
    OnSbRow *row = on_sb_row_new(kind, f->id, display, f->name, expandable);
    g_free(display);
    return row;
}

/* ---------------------------------------------------------------------------
 * add_folder_rows() — recursively append the subfolders of `parent_id`
 * into `into` (a row's children store).  A folder with no subfolders is
 * a LEAF (no children store), so the list view shows it without an
 * expander — the tree view drew an arrow on every folder row.
 *   lw          — the library window.
 *   parent_id   — database folder id whose children to add (0 = roots).
 *   into        — the store to append them to.
 *   note_counts — count map, or NULL.
 *   children    — the pre-fetched child map (one query for the tree).
 * ------------------------------------------------------------------------- */
static void
add_folder_rows(OnLibrary *lw, gint64 parent_id, GListStore *into,
                GHashTable *note_counts, GHashTable *children)
{
    GList *folders = on_db_folder_children(children, parent_id);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one child folder                    */
        gboolean has_kids =
            on_db_folder_children(children, f->id) != NULL;
        OnSbRow *row = sb_folder_row(lw, f, SB_KIND_FOLDER, note_counts,
                                     has_kids);
        g_list_store_append(into, row);
        if (has_kids)
            add_folder_rows(lw, f->id, row->children, note_counts, children);
        g_object_unref(row);
    }
    /* `folders` belongs to the child map — nothing to free here.            */
}

/* sb_section_append() — append one section row (no children) to the root. */
static OnSbRow *
sb_section_append(OnLibrary *lw, gint kind, const gchar *name,
                  const gchar *raw, gboolean expandable)
{
    OnSbRow *row = on_sb_row_new(kind, 0, name, raw, expandable);
    g_list_store_append(lw->sb_store, row);
    g_object_unref(row);             /* the store holds it                  */
    return row;
}

/* sb_row_key() — hashable identity of a sidebar row for state that must
 * survive a model rebuild (positions shift when folders move; kind+id
 * don't).                                                                   */
static gint64
sb_row_key(gint kind, gint64 id)
{
    return id * 16 + kind;
}

/* sb_tree_row_at() — the GtkTreeListRow at position `pos` of the flattened
 * sidebar model, and its OnSbRow.  Returns the tree row (caller unrefs),
 * or NULL past the end.                                                     */
static GtkTreeListRow *
sb_tree_row_at(OnLibrary *lw, guint pos, OnSbRow **row)
{
    GtkTreeListRow *tr =
        g_list_model_get_item(G_LIST_MODEL(lw->sb_tree), pos);
    if (tr == NULL)
        return NULL;
    OnSbRow *r = gtk_tree_list_row_get_item(tr);   /* a ref: drop it, the
                                                      tree row keeps one */
    g_object_unref(r);
    if (row != NULL)
        *row = r;
    return tr;
}

/* ---------------------------------------------------------------------------
 * sb_find_chain() — find the row (kind, id) in the OnSbRow tree under
 * `store`, appending the rows from the top level down to it into `chain`
 * (the hit last).  Returns TRUE when found.
 * ------------------------------------------------------------------------- */
static gboolean
sb_find_chain(GListStore *store, gint kind, gint64 id, GPtrArray *chain)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
    for (guint i = 0; i < n; i++) {
        OnSbRow *r = g_list_model_get_item(G_LIST_MODEL(store), i);
        g_ptr_array_add(chain, r);   /* the array holds the ref             */
        if (r->kind == kind && r->id == id)
            return TRUE;
        if (r->children != NULL &&
            sb_find_chain(r->children, kind, id, chain))
            return TRUE;
        g_ptr_array_remove_index(chain, chain->len - 1);
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * sb_reveal() — make the sidebar row (kind, id) visible by expanding its
 * ANCESTORS (never the row itself: a collapsed selected folder stays
 * collapsed) and return its position in the flattened model, or
 * GTK_INVALID_LIST_POSITION when there is no such row.  Also hands back
 * the row.
 * ------------------------------------------------------------------------- */
static guint
sb_reveal(OnLibrary *lw, gint kind, gint64 id, OnSbRow **row_out)
{
    GPtrArray *chain = g_ptr_array_new_with_free_func(g_object_unref);
    guint pos = GTK_INVALID_LIST_POSITION;
    if (sb_find_chain(lw->sb_store, kind, id, chain)) {
        /* Walk the flattened model once per level: each ancestor is on
         * screen once the one above it is expanded.                       */
        guint from = 0;              /* where the next level can start      */
        for (guint level = 0; level < chain->len; level++) {
            OnSbRow *want = g_ptr_array_index(chain, level);
            for (guint i = from; ; i++) {
                OnSbRow *r;
                GtkTreeListRow *tr = sb_tree_row_at(lw, i, &r);
                if (tr == NULL)
                    break;
                if (r == want) {
                    if (level + 1 < chain->len)
                        gtk_tree_list_row_set_expanded(tr, TRUE);
                    else
                        pos = i;
                    from = i + 1;
                    g_object_unref(tr);
                    break;
                }
                g_object_unref(tr);
            }
        }
        if (row_out != NULL)
            *row_out = g_ptr_array_index(chain, chain->len - 1);
    }
    g_ptr_array_unref(chain);        /* the rows live on in the stores      */
    return pos;
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the whole sidebar model: the folder tree
 * under the fixed "Notes" root, then the "Tags" section.  Attempts to
 * restore the previous selection by (kind, id), and puts back which rows
 * were expanded (the very first population expands nothing) — a drop used
 * to re-expand every folder because every successful drag refreshes the
 * sidebar.
 * ------------------------------------------------------------------------- */
static void
refresh_sidebar(OnLibrary *lw)
{
    gint   want_kind = lw->sel_kind; /* selection to restore                */
    gint64 want_id   = lw->sel_id;

    /* Folder mutations flow through refresh_sidebar; invalidate the cached
     * path map so refresh_notes fetches a fresh one on the next call.       */
    if (lw->folder_path_cache != NULL) {
        g_hash_table_destroy(lw->folder_path_cache);
        lw->folder_path_cache = NULL;
    }

    /* Capture the expansion state (keyed by kind+id, which survive the
     * rebuild).  The flattened model lists exactly the rows that are on
     * screen, and an expanded row is on screen by definition.            */
    GHashTable *expanded = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                 g_free, NULL);
    for (guint i = 0; ; i++) {
        OnSbRow *r;
        GtkTreeListRow *tr = sb_tree_row_at(lw, i, &r);
        if (tr == NULL)
            break;
        if (gtk_tree_list_row_get_expanded(tr)) {
            gint64 *key = g_new(gint64, 1);
            *key = sb_row_key(r->kind, r->id);
            g_hash_table_add(expanded, key);
        }
        g_object_unref(tr);
    }

    /* The list view keeps its scroll anchor across an items-changed as far
     * as it can; a rebuild replaces every item, so the position is put
     * back explicitly — a sidebar rebuild is never a navigation.          */
    GtkAdjustment *vadj      = view_vadjustment(GTK_WIDGET(lw->sidebar));
    gdouble        scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    g_list_store_remove_all(lw->sb_store);

    /* Batched counts: one query for all folders, one for all tags —
     * refresh_sidebar runs after every autosave, so per-row COUNT
     * queries added up (especially against a shared/networked db).
     * Built ONLY when the counts are actually displayed.                    */
    GHashTable *note_counts = NULL;  /* folder id → note count, or NULL     */
    GHashTable *tag_counts  = NULL;  /* tag id → note count, or NULL        */
    if (lw->app->sidebar_counts) {
        note_counts = on_db_note_count_map(lw->app->db);
        tag_counts  = on_db_tag_count_map(lw->app->db);
    }

    /* The whole folder tree in one query; add_folder_rows walks it.         */
    GHashTable *children = on_db_folder_child_map(lw->app->db);

    /* "Pinned Notes" on top — a live view, not a real location; notes
     * stay in their folders.                                               */
    gint n_pinned = on_db_note_count_pinned(lw->app->db);
    if (n_pinned > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("\xf0\x9f\x93\x8c\xc2\xa0 Pinned Notes (%d)",
                              n_pinned)
            : g_strdup("\xf0\x9f\x93\x8c\xc2\xa0 Pinned Notes");
        sb_section_append(lw, SB_KIND_PINNED, label, "Pinned Notes", FALSE);
        g_free(label);
    }

    /* "All Notes" — a live view of every note outside the Trash.           */
    gchar *all_label = lw->app->sidebar_counts
        ? g_strdup_printf("\xf0\x9f\x94\xae\xc2\xa0 All Notes (%d)",
                          on_db_note_count_visible(lw->app->db))
        : g_strdup("\xf0\x9f\x94\xae\xc2\xa0 All Notes");
    sb_section_append(lw, SB_KIND_ALL, all_label, "All Notes", FALSE);
    g_free(all_label);

    /* "Action Items" directly under All Notes — every '!' line across the
     * visible notes (mirrored into the action_items table by saves).
     * Shown only while any exist; the optional count is the OPEN ones.     */
    gint n_actions = 0, n_open = 0;  /* all items / unchecked items         */
    on_db_action_counts(lw->app->db, &n_actions, &n_open);
    if (n_actions > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("\xe2\x9d\x97\xc2\xa0 Action Items (%d)", n_open)
            : g_strdup("\xe2\x9d\x97\xc2\xa0 Action Items");
        sb_section_append(lw, SB_KIND_ACTIONS, label, "Action Items", FALSE);
        g_free(label);
    }

    /* "Notes" root — selecting it shows the top-level notes.  Always
     * expandable, even with no folders yet: New Folder lands under it.    */
    gchar *root_label = lw->app->sidebar_counts
        ? g_strdup_printf("\xf0\x9f\x93\x93\xc2\xa0 Notes (%d)",
                          count_from_map(note_counts, 0))
        : g_strdup("\xf0\x9f\x93\x93\xc2\xa0 Notes");
    OnSbRow *root = sb_section_append(lw, SB_KIND_ROOT, root_label, "Notes",
                                      TRUE);
    g_free(root_label);
    add_folder_rows(lw, 0, root->children, note_counts, children);

    /* "Tags" header + one row per known tag.                               */
    GList *tags = on_db_tag_list(lw->app->db);
    if (tags != NULL) {
        OnSbRow *header = sb_section_append(
            lw, SB_KIND_TAGS_HEADER, "\xf0\x9f\x8f\xb7\xef\xb8\x8f\xc2\xa0 Tags",
            "Tags", TRUE);
        for (GList *l = tags; l != NULL; l = l->next) {
            OnTag *t = l->data;      /* one tag                             */
            gchar *raw   = g_strdup_printf("#%s", t->name);
            gchar *label = lw->app->sidebar_counts
                ? g_strdup_printf("#%s (%d)", t->name,
                                  count_from_map(tag_counts, t->id))
                : g_strdup(raw);
            OnSbRow *row = on_sb_row_new(SB_KIND_TAG, t->id, label, raw,
                                         FALSE);
            g_list_store_append(header->children, row);
            g_object_unref(row);
            g_free(label);
            g_free(raw);
        }
    }
    on_db_tag_list_free(tags);

    /* "Trash" at the bottom, only while it holds something.  Selecting
     * it lists the directly-trashed notes; trashed folders hang under it
     * as browsable children (their subtrees stay hidden until restore).    */
    gint n_trash = on_db_trash_count(lw->app->db);
    if (n_trash > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("\xf0\x9f\x97\x91\xc2\xa0 Trash (%d)", n_trash)
            : g_strdup("\xf0\x9f\x97\x91\xc2\xa0 Trash");
        GList *trashed = on_db_folder_list_trashed(lw->app->db);
        OnSbRow *trash = sb_section_append(lw, SB_KIND_TRASH, label, "Trash",
                                           trashed != NULL);
        g_free(label);
        for (GList *l = trashed; l != NULL; l = l->next) {
            OnSbRow *row = sb_folder_row(lw, l->data, SB_KIND_TRASH_FOLDER,
                                         note_counts, FALSE);
            g_list_store_append(trash->children, row);
            g_object_unref(row);
        }
        on_db_folder_list_free(trashed);
    }

    on_db_folder_child_map_free(children);
    if (note_counts != NULL)
        g_hash_table_destroy(note_counts);
    if (tag_counts != NULL)
        g_hash_table_destroy(tag_counts);

    /* Restore the expansion state and the previous selection, falling back
     * to the first row (Pinned Notes when any are pinned, All Notes
     * otherwise).  One walk over the flattened model: expanding a row
     * splices its children in right after it, so the walk visits them
     * next and the count grows under it.  The populating guard stays up
     * through the restore: selection-changed would otherwise rebuild the
     * notes pane a second time — every refresh_sidebar caller already
     * pairs it with an explicit refresh_notes.                            */
    for (guint i = 0; ; i++) {
        OnSbRow *r;
        GtkTreeListRow *tr = sb_tree_row_at(lw, i, &r);
        if (tr == NULL)
            break;
        gint64 ekey = sb_row_key(r->kind, r->id);
        if (g_hash_table_contains(expanded, &ekey))
            gtk_tree_list_row_set_expanded(tr, TRUE);
        g_object_unref(tr);
    }
    g_hash_table_destroy(expanded);

    OnSbRow *want = NULL;            /* the restored row                    */
    guint sel_pos = sb_reveal(lw, want_kind, want_id, &want);
    gboolean restored = sel_pos != GTK_INVALID_LIST_POSITION;
    if (restored) {
        /* The suppressed handler would have refreshed sel_name; do it
         * here so a renamed folder/tag keeps it current.                 */
        g_free(lw->sel_name);
        lw->sel_name = g_strdup(want->raw);
    } else {
        OnSbRow *first;
        GtkTreeListRow *tr = sb_tree_row_at(lw, 0, &first);
        if (tr != NULL) {
            lw->sel_kind = first->kind;
            lw->sel_id   = first->id;
            g_free(lw->sel_name);
            lw->sel_name = g_strdup(first->raw);
            sel_pos = 0;
            g_object_unref(tr);
        }
    }
    if (sel_pos != GTK_INVALID_LIST_POSITION)
        gtk_single_selection_set_selected(lw->sb_sel, sel_pos);
    lw->populating--;

    /* The old selection no longer exists (deleted folder/pruned tag), so
     * the notes pane still shows its contents: refresh for the new
     * fallback selection.  When the selection was restored, the caller's
     * own refresh_notes covers it.                                        */
    if (!restored)
        refresh_notes(lw);

    if (scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
    sidebar_fit_queue(lw, FALSE);    /* rows came and went                  */
}

/* ===========================================================================
 * notes pane population + grid thumbnails
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * render_note_thumb() — draw a square THUMB_SIZE card for one note: its
 * first embedded image (if any) above the beginning of its body text,
 * returned as a GdkTexture of THUMB_SIZE × the grid's scale factor pixels
 * — the GtkPicture that shows it is THUMB_SIZE logical px, so on a 2×
 * display the texture is drawn 1:1 and stays sharp (a 1× texture scaled
 * up was blurry).  Everything is drawn in LOGICAL units under one cairo
 * scale.  The title is NOT drawn here — the grid shows it as a real text
 * label under the card.
 *   lw — the library window (for the database).
 *   id — the note to render.
 * Returns a new texture reference.
 * ------------------------------------------------------------------------- */
static GdkTexture *
render_note_thumb(OnLibrary *lw, gint64 id)
{
    /* The note as a document: nothing decoded.  Its FIRST image, if any,
     * is decoded here capped at 512 px — the card preview is at most ~256
     * physical pixels wide, so a full-resolution decode (tens of MB per
     * screenshot) would be pure waste.                                     */
    OnDocument *doc = on_note_document_load(lw->app->db, id);
    GdkPixbuf *img = NULL;           /* preview image for the card (owned)  */
    GBytes *png = on_document_image_nth(doc, 0, NULL);
    if (png != NULL) {
        gsize n_png;
        const guint8 *bytes = g_bytes_get_data(png, &n_png);
        img = on_png_decode_capped(bytes, n_png, 512);
    }
    /* Body text: everything after the TITLE line.  The title is the first
     * non-empty line (matching on_document_title, which derives the name
     * shown under the card) — skipping only the literal first line used
     * to leave the title duplicated inside the thumbnail whenever a note
     * began with blank lines.                                              */
    gchar *text = on_document_plain_text(doc);
    const gchar *body = text;        /* start of the post-title content     */
    while (*body == '\n')
        body++;                      /* skip leading blank lines            */
    const gchar *nl = strchr(body, '\n');
    body = (nl != NULL) ? nl + 1 : "";
    while (*body == '\n')
        body++;                      /* don't lead the card with blanks     */
    /* Walk at most 300 UTF-8 codepoints — avoids a full g_utf8_strlen scan
     * of the whole body followed by a second g_utf8_substring walk.       */
    const gchar *p = body;
    gint n = 0;
    while (*p && n < 300) { p = g_utf8_next_char(p); n++; }
    gchar *body_cut = g_strndup(body, (gsize)(p - body));

    /* Draw with cairo in logical units at the display's scale factor.     */
    const gint SZ = THUMB_SIZE;      /* square edge length, logical px      */
    gint scale = gtk_widget_get_scale_factor(GTK_WIDGET(lw->notes_grid));
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, SZ * scale, SZ * scale);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);

    /* White background with a light border.                                */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, SZ - 1, SZ - 1);
    cairo_stroke(cr);

    gdouble y = 6;                   /* current vertical drawing position   */

    if (img != NULL) {
        /* Fit the image into the card's top area.                          */
        gint iw = gdk_pixbuf_get_width(img);
        gint ih = gdk_pixbuf_get_height(img);
        gdouble scale = MIN((gdouble)(SZ - 12) / iw, 72.0 / ih);
        scale = MIN(scale, 1.0);
        gdouble dw = iw * scale, dh = ih * scale;

        /* Through a texture: gdk_texture_download writes cairo's own
         * ARGB32 layout (premultiplied, native order) straight into an
         * image surface — the gdk-pixbuf cairo bridge is deprecated.     */
        GdkTexture *tex = on_app_texture_for_pixbuf(img);
        cairo_surface_t *src = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, iw, ih);
        gdk_texture_download(tex, cairo_image_surface_get_data(src),
                             (gsize)cairo_image_surface_get_stride(src));
        cairo_surface_mark_dirty(src);
        cairo_save(cr);
        cairo_translate(cr, (SZ - dw) / 2.0, y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr),
                                 CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(src);
        g_object_unref(tex);
        y += dh + 4;
    }

    /* Body preview (small grey), clipped to the card.                      */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_width(layout, (SZ - 12) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    gchar *markup = g_markup_printf_escaped(
        "<span foreground=\"#444444\">%s</span>", body_cut);
    pango_layout_set_markup(layout, markup, -1);
    g_free(markup);

    cairo_rectangle(cr, 6, y, SZ - 12, SZ - y - 6);
    cairo_clip(cr);
    cairo_move_to(cr, 6, y);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    /* Hand the pixels to a texture.  GDK_MEMORY_DEFAULT IS cairo's
     * ARGB32 layout (premultiplied, native byte order), so the buffer is
     * copied once and never converted.                                     */
    cairo_surface_flush(surface);
    gint stride = cairo_image_surface_get_stride(surface);
    GBytes *pixels = g_bytes_new(cairo_image_surface_get_data(surface),
                                 (gsize)stride * SZ * scale);
    GdkTexture *texture = gdk_memory_texture_new(SZ * scale, SZ * scale,
                                                 GDK_MEMORY_DEFAULT,
                                                 pixels, (gsize)stride);
    g_bytes_unref(pixels);
    cairo_surface_destroy(surface);

    g_free(body_cut);
    g_free(text);
    g_clear_object(&img);
    on_document_free(doc);
    return texture;
}

/* ---------------------------------------------------------------------------
 * get_note_thumb() — cached access to a note's thumbnail; re-renders only
 * when the note's updated_at changed since the cached render.
 * Returns a borrowed reference owned by the cache.
 * ------------------------------------------------------------------------- */
static GdkTexture *
get_note_thumb(OnLibrary *lw, gint64 id, gint64 updated_at)
{
    ThumbEntry *e = g_hash_table_lookup(lw->thumb_cache, &id);
    if (e != NULL && e->updated_at == updated_at)
        return e->texture;

    GdkTexture *thumb = render_note_thumb(lw, id);
    e = g_new0(ThumbEntry, 1);
    e->updated_at = updated_at;
    e->texture    = thumb;

    gint64 *key = g_new(gint64, 1);
    *key = id;
    g_hash_table_replace(lw->thumb_cache, key, e);
    return thumb;
}

/* How long one thumb_fill_idle() slice may run before yielding back to
 * the main loop (µs).  Big enough to batch several typical cards, small
 * enough to keep scrolling and typing smooth while a cold grid fills.       */
#define THUMB_IDLE_BUDGET_US (40 * 1000)

/* ---------------------------------------------------------------------------
 * thumb_fill_idle() — render queued thumbnails a time slice at a time and
 * deliver each into its grid row: the row's texture is set and the store
 * told (on_row_touch), which rebinds the grid item.  Jobs whose row is no
 * longer in the store (model rebuilt mid-fill) are simply dropped — the
 * rebuild queued fresh jobs.
 * ------------------------------------------------------------------------- */
static gboolean
thumb_fill_idle(gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 slice_start = g_get_monotonic_time();

    while (!g_queue_is_empty(&lw->thumb_pending)) {
        ThumbJob *job = g_queue_pop_head(&lw->thumb_pending);
        guint pos;                   /* the row's place in the store        */
        if (g_list_store_find(lw->notes_store, job->row, &pos)) {
            GdkTexture *thumb =      /* borrowed from the cache             */
                get_note_thumb(lw, job->row->id, job->updated_at);
            g_set_object(&job->row->thumb, thumb);
            on_row_touch(lw->notes_store, job->row);
        }
        thumb_job_free(job);

        if (g_get_monotonic_time() - slice_start > THUMB_IDLE_BUDGET_US)
            break;                   /* yield; the idle re-runs             */
    }

    if (g_queue_is_empty(&lw->thumb_pending)) {
        lw->thumb_idle = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* thumb_queue() — queue a (re)render of `row`'s thumbnail and make sure
 * the fill idle is running.  The job holds its own reference on the row. */
static void
thumb_queue(OnLibrary *lw, OnNoteRow *row, gint64 updated_at)
{
    ThumbJob *job = g_new0(ThumbJob, 1);
    job->row        = g_object_ref(row);
    job->updated_at = updated_at;
    g_queue_push_tail(&lw->thumb_pending, job);
    if (lw->thumb_idle == 0)
        lw->thumb_idle = g_idle_add(thumb_fill_idle, lw);
}

/* action_due_text() — the Due Date cell's text for a timestamp, or NULL
 * for none.  ONE spelling, shared by the populate and the due dialog.      */
static gchar *
action_due_text(gint64 due)
{
    if (due == 0)
        return NULL;
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *when = g_date_time_format(dt, "%b %e, %Y");
    g_date_time_unref(dt);
    return when;
}

/* ---------------------------------------------------------------------------
 * refresh_actions() — repopulate the Action Items model (the notes
 * pane's third view) and show it: one row per '!' line across every
 * visible note, newest note first.  Same scroll-keeping contract as
 * refresh_notes.
 * ------------------------------------------------------------------------- */
static void
refresh_actions(OnLibrary *lw)
{
    gboolean keep_scroll = lw->shown_kind == lw->sel_kind;
    GtkAdjustment *vadj = view_vadjustment(GTK_WIDGET(lw->actions_view));
    gdouble scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
    GList *items = on_db_action_list(lw->app->db);
    for (GList *l = items; l != NULL; l = l->next) {
        OnActionItem *it = l->data;  /* one action item                     */
        if (it->done && !lw->app->show_done_actions)
            continue;                /* Settings: hide completed items      */
        OnActionRow *row = on_action_row_new();
        row->note_id = it->note_id;
        row->ord     = it->ord;
        row->done    = it->done;
        row->text    = g_strdup(it->text);
        row->due     = action_due_text(it->due);
        row->due_raw = it->due;
        g_ptr_array_add(rows, row);
    }
    on_db_action_list_free(items);
    g_list_store_splice(lw->actions_store, 0,
                        g_list_model_get_n_items(
                            G_LIST_MODEL(lw->actions_store)),
                        rows->pdata, rows->len);
    g_ptr_array_unref(rows);
    lw->populating--;

    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "actions");
    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
    status_path_update(lw);
}

/* ---------------------------------------------------------------------------
 * notes_for_selection() — the notes the current sidebar selection lists, in
 * display order.  THE one place that maps a selection onto a note set: the
 * notes pane populates from it, and so does the media browser, so the two
 * can never disagree about what "this folder" means.
 *   lw — the library window.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().
 * ------------------------------------------------------------------------- */
static GList *
notes_for_selection(OnLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_TAG)
        return on_db_notes_by_tag(lw->app->db, lw->sel_id);
    if (lw->sel_kind == SB_KIND_PINNED)
        return on_db_note_list_pinned(lw->app->db);
    if (lw->sel_kind == SB_KIND_ALL)
        return on_db_note_list_recent(lw->app->db);
    if (lw->sel_kind == SB_KIND_TRASH)
        return on_db_note_list_trashed(lw->app->db);
    if (lw->sel_kind == SB_KIND_ACTIONS)
        return on_db_note_list_recent(lw->app->db);
                                     /* the Action Items view spans every
                                        note; refresh_notes never asks (it
                                        swaps in its own model first), but
                                        the media browser does             */
    return on_db_note_list(lw->app->db, lw->sel_id);
                                     /* root, folder, or trashed folder     */
}

/* ---------------------------------------------------------------------------
 * notes_preview_line() — the Comfortable-density preview for one note: the
 * first non-blank line of its body text AFTER the title line.  body_text
 * starts with the title followed by '\n', so that first line is skipped.
 *   body — the note's cached body text (may be NULL or truncated).
 * Returns a newly allocated line, or NULL when there is nothing to show.
 * ------------------------------------------------------------------------- */
static gchar *
notes_preview_line(const gchar *body)
{
    if (body == NULL)
        return NULL;
    const gchar *pos = strchr(body, '\n');
    if (pos != NULL)
        pos++;                       /* step past the title's newline        */
    while (pos != NULL && *pos != '\0') {
        const gchar *eol = strchr(pos, '\n');
        gchar *line = (eol != NULL) ? g_strndup(pos, eol - pos)
                                    : g_strdup(pos);
        g_strstrip(line);
        if (*line != '\0')
            return line;
        g_free(line);
        pos = (eol != NULL) ? eol + 1 : NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * refresh_notes() — repopulate the notes model from the current sidebar
 * selection (a folder's notes, or a tag's notes).  When the selection is
 * the same one already shown — a content refresh (autosave, editor
 * close), not a navigation — the scroll position and the selection are
 * preserved.
 * ------------------------------------------------------------------------- */
static void
refresh_notes(OnLibrary *lw)
{
    /* Whatever happens below, the rows any queued thumbnail jobs point
     * at are stale (or about to be replaced): drop them.                   */
    thumb_pending_clear(lw);
    done_button_sync(lw);            /* the setting may have changed — and
                                        the Action Items view, which the
                                        setting is ABOUT, returns early   */

    /* The Action Items selection swaps in its own view and model.          */
    if (lw->sel_kind == SB_KIND_ACTIONS) {
        refresh_actions(lw);
        return;
    }
    /* Leaving the actions view: restore the user's list/grid mode BEFORE
     * the thumbnail decision below reads the visible child.                */
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "actions") == 0)
        gtk_stack_set_visible_child_name(GTK_STACK(lw->stack),
                                         lw->grid_pref ? "grid" : "list");

    /* The list's density class, for the row-height CSS.                   */
    if (lw->app->comfortable_list)
        gtk_widget_remove_css_class(GTK_WIDGET(lw->notes_list), "notes-compact");
    else
        gtk_widget_add_css_class(GTK_WIDGET(lw->notes_list), "notes-compact");

    /* Thumbnails are only rendered while the grid is showing: list mode
     * never pays for them (they used to be regenerated for the edited
     * note on EVERY autosave), and switching to grid refreshes.            */
    gboolean want_thumbs =
        g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0;

    /* Same selection as last time?  Then remember where the visible
     * notes pane is scrolled so the rebuild doesn't jump to the top.       */
    gboolean keep_scroll = lw->shown_kind == lw->sel_kind &&
                           lw->shown_id   == lw->sel_id;
    GtkWidget     *vis_child = gtk_stack_get_visible_child(GTK_STACK(lw->stack));
    GtkAdjustment *vadj      = GTK_IS_SCROLLED_WINDOW(vis_child)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(vis_child))
        : NULL;
    gdouble scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    /* On a content refresh (autosave, editor close) — same view, not a
     * navigation — capture the selection so we can put it back after the
     * rebuild (new row objects: the selection model cannot follow them).
     * Navigations start with no selection by design.                       */
    GArray *sel_ids = keep_scroll ? selected_note_ids(lw) : NULL;

    lw->populating++;

    /* Pick the note list matching the selection.                           */
    GList *notes = notes_for_selection(lw);

    /* Folder paths for the list view's Path column — ONE query for all
     * folders, never per note (shared/network DBs).  The result is cached
     * in lw and reused across autosave-triggered refreshes; refresh_sidebar
     * clears it whenever folders change.                                    */
    if (lw->folder_path_cache == NULL)
        lw->folder_path_cache = on_db_folder_path_map(lw->app->db);
    GHashTable *paths = lw->folder_path_cache;

    /* Body-text previews for the Comfortable list density — ONE query, and
     * only where the preview is actually drawn: compact density never shows
     * it, and the grid draws thumbnails and the title, never the preview.  */
    GHashTable *previews = (lw->app->comfortable_list && !want_thumbs)
        ? on_db_note_text_map(lw->app->db, NL_PREVIEW_CHARS) : NULL;

    GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
    GPtrArray *todo = g_ptr_array_new();   /* rows needing a thumbnail   */
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */
        OnNoteRow *row = on_note_row_new();
        row->id         = m->id;
        row->title      = g_strdup(m->title);
        row->updated_at = m->updated_at;
        row->created_at = m->created_at;

        /* Format the modification and creation times like
         * "Jun 3, 2026 14:05".                                             */
        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        row->modified = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);
        dt = g_date_time_new_from_unix_local(m->created_at);
        row->created = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);

        /* "/Folder/Sub" location, "/" for the top level — the same
         * format as the status bar's path label.                           */
        const gchar *fpath = m->folder_id != 0
            ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
        row->path = g_strdup_printf("/%s", fpath != NULL ? fpath : "");

        /* Thumbnails: only what the cache already has goes in right away
         * — a stale entry still shows (better than a blank card) while
         * thumb_fill_idle renders the replacement.  Rendering every
         * stale/missing thumbnail here froze the GUI.                      */
        if (want_thumbs) {
            ThumbEntry *e = g_hash_table_lookup(lw->thumb_cache, &m->id);
            if (e != NULL)
                row->thumb = g_object_ref(e->texture);
            if (e == NULL || e->updated_at != m->updated_at)
                g_ptr_array_add(todo, row);
        }

        row->preview = notes_preview_line(
            previews ? g_hash_table_lookup(previews, &m->id) : NULL);
        g_ptr_array_add(rows, row);
    }
    /* paths == lw->folder_path_cache — kept alive for the next refresh.     */
    if (previews != NULL) g_hash_table_destroy(previews);
    on_db_note_list_free(notes);

    /* One items-changed for the whole rebuild.                             */
    g_list_store_splice(lw->notes_store, 0,
                        g_list_model_get_n_items(G_LIST_MODEL(lw->notes_store)),
                        rows->pdata, rows->len);
    for (guint i = 0; i < todo->len; i++) {
        OnNoteRow *row = g_ptr_array_index(todo, i);
        thumb_queue(lw, row, row->updated_at);
    }
    g_ptr_array_unref(todo);
    g_ptr_array_unref(rows);
    lw->populating--;

    /* Restore the note selection that existed before the rebuild.           */
    if (sel_ids != NULL) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->notes_sorted));
        for (guint i = 0; i < n && sel_ids->len > 0; i++) {
            OnNoteRow *row =
                g_list_model_get_item(G_LIST_MODEL(lw->notes_sorted), i);
            for (guint k = 0; k < sel_ids->len; k++) {
                if (g_array_index(sel_ids, gint64, k) == row->id) {
                    gtk_selection_model_select_item(
                        GTK_SELECTION_MODEL(lw->notes_sel), i, FALSE);
                    break;
                }
            }
            g_object_unref(row);
        }
        g_array_free(sel_ids, TRUE);
    }

    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);

    status_path_update(lw);
}

/* refresh_all_idle() — refresh_all from an idle (see on_sidebar_drop).    */
static gboolean
refresh_all_idle(gpointer user_data)
{
    refresh_all(user_data);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * refresh_all() — rebuild the sidebar and the notes pane together: the
 * standard follow-up to any change that can touch both (moves, deletes,
 * tag edits, …).  refresh_sidebar keeps its populating guard up through
 * the selection restore, so the explicit refresh_notes here is the one
 * that repopulates the pane (the sidebar's own fallback refresh — taken
 * when the old selection vanished — is redundant but harmless).
 * ------------------------------------------------------------------------- */
static void
refresh_all(OnLibrary *lw)
{
    refresh_sidebar(lw);
    refresh_notes(lw);
}

/* ===========================================================================
 * selection and activation
 * =========================================================================== */

/* sb_selected_row() — the sidebar's selected OnSbRow (borrowed), or NULL. */
static OnSbRow *
sb_selected_row(OnLibrary *lw)
{
    GtkTreeListRow *tr = gtk_single_selection_get_selected_item(lw->sb_sel);
    if (tr == NULL)
        return NULL;
    OnSbRow *r = gtk_tree_list_row_get_item(tr);
    g_object_unref(r);               /* the tree row keeps it alive         */
    return r;
}

/* ---------------------------------------------------------------------------
 * on_sidebar_selection_changed() — a folder or tag was selected: remember
 * it and refresh the notes pane.  The "Tags" header is not a selection:
 * a click on it puts the selection back where it was.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_selection_changed(GtkSelectionModel *sel, guint position,
                             guint n_items, gpointer user_data)
{
    (void)sel; (void)position; (void)n_items;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating > 0)
        return;

    OnSbRow *r = sb_selected_row(lw);
    if (r == NULL)
        return;
    if (r->kind == SB_KIND_TAGS_HEADER) {
        lw->populating++;            /* the revert is not a navigation      */
        guint back = sb_reveal(lw, lw->sel_kind, lw->sel_id, NULL);
        if (back != GTK_INVALID_LIST_POSITION)
            gtk_single_selection_set_selected(lw->sb_sel, back);
        lw->populating--;
        return;
    }

    lw->sel_kind = r->kind;
    lw->sel_id   = r->id;
    g_free(lw->sel_name);
    lw->sel_name = g_strdup(r->raw);
    refresh_notes(lw);
}

/* ---------------------------------------------------------------------------
 * on_note_activated() — double-click/Enter on a note in either view opens
 * it.  Both views' "activate" signals give the position in the SORTED
 * model, which is what they show.
 * ------------------------------------------------------------------------- */
static void
on_note_activated(GtkWidget *view, guint position, gpointer user_data)
{
    (void)view;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnNoteRow *row =
        g_list_model_get_item(G_LIST_MODEL(lw->notes_sorted), position);
    if (row == NULL)
        return;
    on_editor_window_open(lw->app, row->id);
    g_object_unref(row);
}

/* ---------------------------------------------------------------------------
 * on_action_toggled() — the Action Items checkbox: flip the item's done
 * state everywhere — the row (instant feedback), its action_items row,
 * and the note text itself (strikethrough) via on_editor_action_set_done.
 * The check button carries its GtkListItem as "on-item"; a toggle that
 * the BIND itself caused ("on-binding" set on the item) is not a click.
 * ------------------------------------------------------------------------- */
static void
on_action_toggled(GtkCheckButton *check, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkListItem *item = g_object_get_data(G_OBJECT(check), "on-item");
    if (item == NULL || g_object_get_data(G_OBJECT(item), "on-binding"))
        return;
    OnActionRow *row = gtk_list_item_get_item(item);
    if (row == NULL)
        return;
    gboolean done = gtk_check_button_get_active(check);
    if (done == row->done)
        return;
    row->done = done;

    /* A just-completed item disappears immediately when completed items
     * are hidden; otherwise the row simply re-renders checked + struck.    */
    guint pos;
    if (done && !lw->app->show_done_actions) {
        if (g_list_store_find(lw->actions_store, row, &pos))
            g_list_store_remove(lw->actions_store, pos);
    } else {
        on_row_touch(lw->actions_store, row);
    }
    /* The content rewrite is authoritative and normally rebuilds the
     * mirror itself; only a LIVE editor defers that to its autosave, and
     * only then does the flag need writing here as well.                    */
    gboolean synced = FALSE;         /* did the rewrite update the table?   */
    if (on_editor_action_set_done(lw->app, row->note_id, row->ord, done,
                                  &synced) && !synced)
        on_db_action_set_done(lw->app->db, row->note_id, row->ord, done);
    if (lw->app->sidebar_counts)
        refresh_sidebar(lw);         /* the section's open count changed    */
}

/* ===========================================================================
 * dialogs — THE one modal-dialog scaffold of this file, over a plain
 * GtkWindow (GtkDialog is deprecated since 4.10): a content widget above a
 * right-aligned row of buttons, each carrying a response id; a click, the
 * close button and Escape all reach one response function, and the window
 * is destroyed after it returns.  Entries with activates-default trigger
 * the default button, as they did on GtkDialog.
 * =========================================================================== */

/* DialogResponseFunc — what a dialog calls with the chosen response.
 *   dlg      — the window (about to be destroyed by the scaffold).
 *   response — the button's id, or GTK_RESPONSE_DELETE_EVENT for a close.
 *   data     — the caller's.                                                */
typedef void (*DialogResponseFunc)(GtkWindow *dlg, gint response,
                                   gpointer data);

/* DialogButton — one button of a dialog: its label, its response id and
 * whether it is the default (Enter).                                       */
typedef struct {
    const gchar *label;
    gint         response;
    gboolean     is_default;
} DialogButton;

/* dialog_respond() — deliver `response` to the dialog's function and
 * destroy the window.  The function may not be called twice: a button
 * click destroys the window, whose close-request never fires after.      */
static void
dialog_respond(GtkWindow *dlg, gint response)
{
    DialogResponseFunc fn = g_object_get_data(G_OBJECT(dlg), "on-respond");
    gpointer data = g_object_get_data(G_OBJECT(dlg), "on-respond-data");
    g_object_set_data(G_OBJECT(dlg), "on-respond", NULL);
    if (fn != NULL)
        fn(dlg, response, data);
    gtk_window_destroy(dlg);
}

/* on_dialog_button() — a dialog button was clicked.                        */
static void
on_dialog_button(GtkButton *button, gpointer user_data)
{
    dialog_respond(GTK_WINDOW(user_data),
                   GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
                                                     "on-response")));
}

/* on_dialog_close() — the window's close button or Escape.                 */
static gboolean
on_dialog_close(GtkWindow *dlg, gpointer user_data)
{
    (void)user_data;
    dialog_respond(dlg, GTK_RESPONSE_DELETE_EVENT);
    return TRUE;                     /* destroyed above                     */
}

/* ---------------------------------------------------------------------------
 * dialog_new() — build and present a modal dialog.
 *   parent   — the transient parent.
 *   title    — the window title.
 *   content  — the widget above the buttons (the dialog takes it).
 *   buttons  — the buttons, left to right.
 *   n        — how many.
 *   fn, data — the response function and its data (see dialog_respond).
 * ------------------------------------------------------------------------- */
static void
dialog_new(GtkWindow *parent, const gchar *title, GtkWidget *content,
           const DialogButton *buttons, gsize n, DialogResponseFunc fn,
           gpointer data)
{
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    g_object_set_data(G_OBJECT(dlg), "on-respond", (gpointer)fn);
    g_object_set_data(G_OBJECT(dlg), "on-respond-data", data);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_box_append(GTK_BOX(vbox), content);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    for (gsize i = 0; i < n; i++) {
        GtkWidget *b = gtk_button_new_with_mnemonic(buttons[i].label);
        g_object_set_data(G_OBJECT(b), "on-response",
                          GINT_TO_POINTER(buttons[i].response));
        g_signal_connect(b, "clicked", G_CALLBACK(on_dialog_button), dlg);
        gtk_box_append(GTK_BOX(row), b);
        if (buttons[i].is_default)
            gtk_window_set_default_widget(GTK_WINDOW(dlg), b);
    }
    gtk_box_append(GTK_BOX(vbox), row);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    /* Escape: GtkWindow binds it to window.close only for dialogs; bind it
     * here so this one behaves like one.                                   */
    GtkEventController *keys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(keys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                         gtk_named_action_new("window.close")));
    gtk_widget_add_controller(dlg, keys);
    g_signal_connect(dlg, "close-request", G_CALLBACK(on_dialog_close), NULL);
    gtk_window_present(GTK_WINDOW(dlg));
}

/* ---------------------------------------------------------------------------
 * DueDialog — what the due-date dialog's response callback needs, carried
 * as object data on the dialog.
 *
 * Fields:
 *   lw   — the library window.
 *   row  — the Action Items row the date is for (owned reference; a row
 *          the store has dropped meanwhile is simply not found).
 *   cal  — the GtkCalendar in the dialog.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnLibrary   *lw;
    OnActionRow *row;
    GtkWidget   *cal;
} DueDialog;

/* due_dialog_free() — GDestroyNotify for the DueDialog on the dialog.       */
static void
due_dialog_free(gpointer data)
{
    DueDialog *d = data;
    g_object_unref(d->row);
    g_free(d);
}

/* ---------------------------------------------------------------------------
 * on_due_response() — the due-date dialog closed.  Set rewrites the
 * "due YYYY-MM-DD" suffix of the '!' line in the note text
 * (on_editor_action_set_due), Clear removes it; the row updates
 * immediately, the durable action_items row follows from the content
 * rewrite.  A row that vanished meanwhile (the model was rebuilt under the
 * dialog) is covered by a repopulate, which reads the rewritten mirror.
 * ------------------------------------------------------------------------- */
static void
on_due_response(GtkWindow *dlg, gint response, gpointer user_data)
{
    (void)dlg;
    DueDialog *d  = user_data;       /* the dialog's state                  */
    OnLibrary *lw = d->lw;
    gint64 new_due = -1;             /* -1 = leave unchanged                */
    if (response == GTK_RESPONSE_OK) {
        GDateTime *picked = gtk_calendar_get_date(GTK_CALENDAR(d->cal));
        GDateTime *dt = g_date_time_new_local(   /* local midnight          */
            g_date_time_get_year(picked), g_date_time_get_month(picked),
            g_date_time_get_day_of_month(picked), 0, 0, 0);
        g_date_time_unref(picked);
        if (dt != NULL) {
            new_due = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
        }
    } else if (response == 1) {
        new_due = 0;                 /* Clear                               */
    }

    if (new_due >= 0 &&
        on_editor_action_set_due(lw->app, d->row->note_id, d->row->ord,
                                 new_due)) {
        g_free(d->row->due);
        d->row->due     = action_due_text(new_due);
        d->row->due_raw = new_due;
        if (!on_row_touch(lw->actions_store, d->row))
            refresh_notes(lw);
    }
}

/* ---------------------------------------------------------------------------
 * action_due_dialog() — modal calendar for one action item's due date;
 * on_due_response applies the choice.
 *   lw  — the library window.
 *   row — the Action Items row.
 * ------------------------------------------------------------------------- */
static void
action_due_dialog(OnLibrary *lw, OnActionRow *row)
{
    DueDialog *d = g_new0(DueDialog, 1);
    d->lw  = lw;
    d->row = g_object_ref(row);
    d->cal = gtk_calendar_new();
    if (row->due_raw != 0) {         /* open on the current due date        */
        GDateTime *dt = g_date_time_new_from_unix_local(row->due_raw);
        gtk_calendar_set_date(GTK_CALENDAR(d->cal), dt);
        g_date_time_unref(dt);
    }
    /* The state rides on the calendar, which the dialog takes.            */
    g_object_set_data_full(G_OBJECT(d->cal), "on-due", d, due_dialog_free);
    static const DialogButton BUTTONS[] = {
        { "_Clear",  1,                   FALSE },
        { "_Cancel", GTK_RESPONSE_CANCEL, FALSE },
        { "_Set",    GTK_RESPONSE_OK,     TRUE  },
    };
    dialog_new(GTK_WINDOW(lw->window), "Notes - Due Date", d->cal, BUTTONS,
               G_N_ELEMENTS(BUTTONS), on_due_response, d);
}

/* on_action_row_activated() — Enter on an Action Items row (and GTK's own
 * double-click, when its count survives — D34) opens the item's note AT
 * that line.  The cells count double-clicks themselves: the Due Date cell
 * opens the calendar, the text cell the note.                             */
static void
on_action_row_activated(GtkColumnView *view, guint position,
                        gpointer user_data)
{
    (void)view;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnActionRow *row =
        g_list_model_get_item(G_LIST_MODEL(lw->actions_sorted), position);
    if (row == NULL)
        return;
    on_editor_window_open_action(lw->app, row->note_id, row->ord);
    g_object_unref(row);
}

/* on_due_cell_double_clicked() — double-click on a Due Date cell: the
 * calendar (on_app_double_click_watch; the claim it makes keeps the row's
 * own activation from also opening the note).                              */
static void
on_due_cell_double_clicked(GtkWidget *label, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkListItem *item = g_object_get_data(G_OBJECT(label), "on-item");
    OnActionRow *row = item != NULL ? gtk_list_item_get_item(item) : NULL;
    if (row != NULL)
        action_due_dialog(lw, row);
}

/* on_action_cell_double_clicked() — double-click on any other Action Items
 * cell: the note, at the item (on_app_double_click_watch).               */
static void
on_action_cell_double_clicked(GtkWidget *cell, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkListItem *item = g_object_get_data(G_OBJECT(cell), "on-item");
    OnActionRow *row = item != NULL ? gtk_list_item_get_item(item) : NULL;
    if (row != NULL)
        on_editor_window_open_action(lw->app, row->note_id, row->ord);
}

/* on_note_double_clicked() — double-click on a note row/cell/card: open
 * the note (on_app_double_click_watch).                                    */
static void
on_note_double_clicked(GtkWidget *cell, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkListItem *item = g_object_get_data(G_OBJECT(cell), "on-item");
    OnNoteRow *row = item != NULL ? gtk_list_item_get_item(item) : NULL;
    if (row != NULL)
        on_editor_window_open(lw->app, row->id);
}

/* ===========================================================================
 * drag & drop: note → folder, folder → folder
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * folder_move_beside() — re-parent folder `folder_id` under `new_parent`
 * and slot it directly before/after sibling `anchor_id` (the row the drop
 * indicator pointed at).  The move appends at the end of the new parent;
 * the reorder then writes the full sibling sequence with the moved folder
 * re-inserted at the anchor.
 * ------------------------------------------------------------------------- */
static gboolean
folder_move_beside(OnLibrary *lw, gint64 folder_id, gint64 new_parent,
                   gint64 anchor_id, gboolean after)
{
    if (!on_db_folder_move(lw->app->db, folder_id, new_parent))
        return FALSE;

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GList *sibs = on_db_folder_list(lw->app->db, new_parent);
    for (GList *l = sibs; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one sibling                         */
        if (f->id == folder_id)
            continue;                /* re-inserted at the anchor below     */
        if (f->id == anchor_id && !after)
            g_array_append_val(ids, folder_id);
        g_array_append_val(ids, f->id);
        if (f->id == anchor_id && after)
            g_array_append_val(ids, folder_id);
    }
    on_db_folder_list_free(sibs);

    gboolean ok = on_db_folder_reorder(lw->app->db,
                                       (const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
    return ok;
}

/* Where in a sidebar row a drop lands: the top quarter is BEFORE the row,
 * the bottom quarter AFTER, the middle INTO.                                */
typedef enum { SB_DROP_BEFORE, SB_DROP_INTO, SB_DROP_AFTER } SbDropPos;

/* The CSS class each position paints the row with (library_install_css).  */
static const gchar *const SB_DROP_CLASS[] = {
    "drop-before", "drop-into", "drop-after",
};

/* row_item() — the GtkListItem a per-row controller was installed for
 * (stashed on the controller as "on-item" by the factory's setup).        */
static GtkListItem *
row_item(gpointer controller)
{
    return g_object_get_data(G_OBJECT(controller), "on-item");
}

/* sb_item_row() — the OnSbRow (borrowed) and its GtkTreeListRow (borrowed)
 * behind a sidebar list item; NULL when the item is unbound.               */
static OnSbRow *
sb_item_row(GtkListItem *item, GtkTreeListRow **tree_row)
{
    GtkTreeListRow *tr = gtk_list_item_get_item(item);
    if (tr == NULL)
        return NULL;
    if (tree_row != NULL)
        *tree_row = tr;
    OnSbRow *r = gtk_tree_list_row_get_item(tr);
    g_object_unref(r);
    return r;
}

/* sb_row_parent() — the OnSbRow (borrowed) one level above a tree row, or
 * NULL at the top level.                                                    */
static OnSbRow *
sb_row_parent(GtkTreeListRow *tr)
{
    GtkTreeListRow *up = gtk_tree_list_row_get_parent(tr);
    if (up == NULL)
        return NULL;
    OnSbRow *r = gtk_tree_list_row_get_item(up);
    g_object_unref(r);
    g_object_unref(up);              /* the model keeps the row alive       */
    return r;
}

/* ---------------------------------------------------------------------------
 * sidebar_drop_target() — validate a drop of `rows` onto the sidebar row
 * `target` at `pos`, and coerce the position.  Which rows are legal
 * depends on what is being dragged: a folder (from the sidebar itself)
 * goes onto folders, the root, or the Trash, never onto itself or into its
 * own subtree — and a folder already in the Trash cannot be dropped on
 * Trash again; notes (from either notes view) go onto any folder-ish row,
 * with the position coerced to INTO (a note drops *into* a folder, never
 * beside it).
 *   lw     — the library window.
 *   rows   — the drag content, or NULL (not loaded: refuse).
 *   target — the row under the pointer, its tree row in `tr`.
 *   pos    — in: where in the row; out: the coerced position.
 * Returns TRUE when the drop is legal.
 * ------------------------------------------------------------------------- */
static gboolean
sidebar_drop_target(OnLibrary *lw, const OnDragRows *rows, OnSbRow *target,
                    GtkTreeListRow *tr, SbDropPos *pos)
{
    (void)lw;
    if (rows == NULL || target == NULL)
        return FALSE;
    gint kind = target->kind;

    if (rows->kind == ON_DRAG_NOTES) {
        if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT ||
            kind == SB_KIND_TRASH) {
            *pos = SB_DROP_INTO;
            return TRUE;
        }
        return FALSE;
    }

    gint   src_kind = (rows->kind == ON_DRAG_FOLDER)
        ? SB_KIND_FOLDER : SB_KIND_TRASH_FOLDER;
    gint64 src_id   = g_array_index(rows->ids, gint64, 0);
    if (target->kind == src_kind && target->id == src_id)
        return FALSE;                /* onto itself                         */
    /* Into its own subtree: the dragged folder among the target's
     * ancestors.                                                           */
    for (GtkTreeListRow *up = gtk_tree_list_row_get_parent(tr);
         up != NULL; ) {
        OnSbRow *anc = gtk_tree_list_row_get_item(up);
        g_object_unref(anc);
        gboolean inside = anc->kind == src_kind && anc->id == src_id;
        GtkTreeListRow *next = gtk_tree_list_row_get_parent(up);
        g_object_unref(up);
        if (inside)
            return FALSE;
        up = next;
    }
    if (kind == SB_KIND_FOLDER)
        return TRUE;                 /* nest INTO or reorder beside it      */
    if (kind == SB_KIND_ROOT ||
        (kind == SB_KIND_TRASH && src_kind == SB_KIND_FOLDER)) {
        *pos = SB_DROP_INTO;
        return TRUE;
    }
    return FALSE;
}

/* sb_row_drop_pos() — where in a row widget a pointer y falls.             */
static SbDropPos
sb_row_drop_pos(GtkWidget *row_widget, gdouble y)
{
    gint h = gtk_widget_get_height(row_widget);
    if (h <= 0)
        return SB_DROP_INTO;
    if (y < h / 4.0)
        return SB_DROP_BEFORE;
    if (y > h * 3 / 4.0)
        return SB_DROP_AFTER;
    return SB_DROP_INTO;
}

/* sb_row_indicate() — paint (or clear, pos < 0) the drop indicator on a
 * row widget: one of the three CSS classes.                                */
static void
sb_row_indicate(GtkWidget *row_widget, gint pos)
{
    for (gsize i = 0; i < G_N_ELEMENTS(SB_DROP_CLASS); i++)
        gtk_widget_remove_css_class(row_widget, SB_DROP_CLASS[i]);
    if (pos >= 0)
        gtk_widget_add_css_class(row_widget, SB_DROP_CLASS[pos]);
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drop_motion() — GtkDropTarget "enter" AND "motion" on one
 * sidebar row (same signature, one handler): validate the row against the
 * drag's content and paint the indicator.  The content is readable here
 * because the target PRELOADS it and every drag is local: for a local
 * drag GtkDropTarget reads the value synchronously from the content
 * provider when the drop starts, so it is never NULL by the first motion
 * (gtkdroptarget.c, gtk_drop_target_load_local).
 * Returns the action offered (MOVE), or 0 to refuse.
 * ------------------------------------------------------------------------- */
static GdkDragAction
on_sidebar_drop_motion(GtkDropTarget *target, gdouble x, gdouble y,
                       gpointer user_data)
{
    (void)x;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkWidget *row_widget =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    const GValue *value = gtk_drop_target_get_value(target);
    const OnDragRows *rows =         /* the drag's content, once loaded     */
        (value != NULL) ? g_value_get_boxed(value) : NULL;
    GtkTreeListRow *tr = NULL;
    OnSbRow *r = sb_item_row(row_item(target), &tr);
    SbDropPos pos = sb_row_drop_pos(row_widget, y);
    gboolean ok = sidebar_drop_target(lw, rows, r, tr, &pos);
    sb_row_indicate(row_widget, ok ? (gint)pos : -1);
    return ok ? GDK_ACTION_MOVE : 0;
}

/* on_sidebar_drop_leave() — clear the row's drop indicator.                 */
static void
on_sidebar_drop_leave(GtkDropTarget *target, gpointer user_data)
{
    (void)user_data;
    sb_row_indicate(
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), -1);
}

/* Logical pixel size of the custom drag-under-cursor icons.                 */
#define DRAG_ICON_SIZE 32

/* ---------------------------------------------------------------------------
 * drag_rows_content() — the shared tail of every "prepare" handler: pick
 * the drag-under-cursor icon for `rows` (folder.png for a folder, file.png
 * for one note, documents.png for several), hand GTK a copy of the rows as
 * the drag's content, and release the caller's.  The icon is read by the
 * drag source AFTER prepare returns, so setting it here is in time.
 *   lw     — the library window.
 *   source — the drag source the icon goes on.
 *   rows   — what is being dragged (consumed).
 * Returns the content provider the prepare handler returns.
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
drag_rows_content(OnLibrary *lw, GtkDragSource *source, OnDragRows *rows)
{
    const gchar *icon =              /* icon file basename                  */
        rows->kind != ON_DRAG_NOTES ? "folder"
                                    : (rows->ids->len > 1 ? "documents"
                                                          : "file");
    GdkPaintable *paintable =
        on_app_icon_paintable(lw->app, icon, DRAG_ICON_SIZE);
    if (paintable != NULL) {
        gtk_drag_source_set_icon(source, paintable, 0, 0);
        g_object_unref(paintable);
    }
    GdkContentProvider *content =    /* holds its own copy of the rows      */
        gdk_content_provider_new_typed(ON_TYPE_DRAG_ROWS, rows);
    on_drag_rows_free(rows);
    return content;
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_prepare() — GtkDragSource "prepare" on one sidebar row:
 * a folder row (in the tree or under Trash) starts a drag carrying its
 * id; any other row refuses, since nothing accepts it.
 * Returns the drag content, or NULL for no drag.
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
on_sidebar_drag_prepare(GtkDragSource *source, gdouble x, gdouble y,
                        gpointer user_data)
{
    (void)x; (void)y;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnSbRow *r = sb_item_row(row_item(source), NULL);
    if (r == NULL ||
        (r->kind != SB_KIND_FOLDER && r->kind != SB_KIND_TRASH_FOLDER))
        return NULL;
    OnDragRows *rows = on_drag_rows_new(r->kind == SB_KIND_FOLDER
                                        ? ON_DRAG_FOLDER
                                        : ON_DRAG_TRASHED_FOLDER);
    g_array_append_val(rows->ids, r->id);
    return drag_rows_content(lw, source, rows);
}

/* ---------------------------------------------------------------------------
 * on_note_drag_prepare() — GtkDragSource "prepare" on one row of either
 * notes view: the whole selection when the pressed note is part of it
 * (multi-select drags), else just that note.  GTK4's list items select on
 * RELEASE (gtklistfactorywidget.c), so a press on a selected row that
 * turns into a drag never collapses the selection — quirk #15 is gone.
 * Returns the drag content, or NULL when the item is unbound.
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
on_note_drag_prepare(GtkDragSource *source, gdouble x, gdouble y,
                     gpointer user_data)
{
    (void)x; (void)y;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnNoteRow *row = gtk_list_item_get_item(row_item(source));
    if (row == NULL)
        return NULL;

    OnDragRows *rows = on_drag_rows_new(ON_DRAG_NOTES);
    GArray *sel = selected_note_ids(lw);
    gboolean in_selection = FALSE;   /* is the pressed note selected?       */
    for (guint i = 0; i < sel->len; i++)
        if (g_array_index(sel, gint64, i) == row->id)
            in_selection = TRUE;
    if (in_selection)
        g_array_append_vals(rows->ids, sel->data, sel->len);
    else
        g_array_append_val(rows->ids, row->id);
    g_array_free(sel, TRUE);
    return drag_rows_content(lw, source, rows);
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drop() — GtkDropTarget "drop" on one sidebar row: the button
 * was released over it.  Either note ids from a notes view (move the notes
 * into the target folder, or trash them) or one of the sidebar's own
 * folder rows (re-nest INTO a folder, reorder BEFORE/AFTER a sibling,
 * trash, or restore-by-drag out of the Trash).  The row is validated
 * again: the release may land where motion had refused.
 * Returns TRUE when something moved (GTK then finishes the drop as a MOVE).
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_drop(GtkDropTarget *target, const GValue *value,
                gdouble x, gdouble y, gpointer user_data)
{
    (void)x;
    OnLibrary *lw = user_data;       /* owning library window               */
    const OnDragRows *rows = g_value_get_boxed(value);
    GtkWidget *row_widget =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    sb_row_indicate(row_widget, -1);

    GtkTreeListRow *tr = NULL;
    OnSbRow *dest = sb_item_row(row_item(target), &tr);
    SbDropPos pos = sb_row_drop_pos(row_widget, y);
    if (!sidebar_drop_target(lw, rows, dest, tr, &pos))
        return FALSE;

    gboolean success = FALSE;        /* whether anything moved              */
    if (rows->kind == ON_DRAG_NOTES) {
        /* --- note ids from the notes pane ---------------------------------*/
        const gint64 *ids = (const gint64 *)rows->ids->data;
        guint n = rows->ids->len;
        if (dest->kind == SB_KIND_TRASH) {
            /* Dropping on Trash IS the delete gesture.                     */
            success = trash_notes_core(lw, ids, n);
        } else {
            /* ONE transaction for the whole selection: per-note moves
             * fsync per call and froze the GUI on big drops.              */
            success = on_db_notes_move(lw->app->db, ids, n, dest->id);
            if (success)
                on_app_status(lw->app,
                              "Moved %u note%s to \xe2\x80\x9c%s\xe2\x80\x9d",
                              n, n == 1 ? "" : "s", dest->raw);
        }
    } else {
        /* --- one of the sidebar's own folder rows --------------------------*/
        gint   src_kind  = (rows->kind == ON_DRAG_FOLDER)
                           ? SB_KIND_FOLDER : SB_KIND_TRASH_FOLDER;
        gint64 folder_id = g_array_index(rows->ids, gint64, 0);
        OnSbRow *src = NULL;         /* the dragged folder's row            */
        GPtrArray *chain = g_ptr_array_new_with_free_func(g_object_unref);
        if (sb_find_chain(lw->sb_store, src_kind, folder_id, chain))
            src = g_ptr_array_index(chain, chain->len - 1);
        gchar *fname = g_strdup(src != NULL ? src->raw : "");
        g_ptr_array_unref(chain);

        if (dest->kind == SB_KIND_TRASH) {
            /* Dropping on Trash IS the delete gesture (validation already
             * refused a folder that is in the Trash).                      */
            success = trash_folder(lw, folder_id, fname);
        } else {
            /* INTO a folder (or anywhere on the root) re-nests, appended
             * at the end; BEFORE/AFTER a folder row slots the dragged
             * folder beside that sibling (re-nesting when the sibling
             * lives under a different parent).  on_db_folder_move
             * re-checks the subtree rule against the database.            */
            gchar *where = NULL;     /* name for the status message         */
            if (dest->kind == SB_KIND_ROOT || pos == SB_DROP_INTO) {
                gint64 new_parent =  /* root drops land at top level        */
                    (dest->kind == SB_KIND_FOLDER) ? dest->id : 0;
                success = on_db_folder_move(lw->app->db, folder_id,
                                            new_parent);
                where = g_strdup(dest->raw);
            } else {
                /* The dest folder's parent row is the new parent: the
                 * "Notes" root maps to top level (0).                      */
                OnSbRow *par = sb_row_parent(tr);
                gint64 new_parent =
                    (par != NULL && par->kind == SB_KIND_FOLDER) ? par->id
                                                                 : 0;
                success = folder_move_beside(lw, folder_id, new_parent,
                                             dest->id, pos == SB_DROP_AFTER);
                where = g_strdup(par != NULL ? par->raw : "Notes");
            }
            if (success) {
                on_app_status(lw->app,
                              src_kind == SB_KIND_TRASH_FOLDER
                                  ? "Folder \xe2\x80\x9c%s\xe2\x80\x9d "
                                    "restored to \xe2\x80\x9c%s\xe2\x80\x9d"
                                  : "Moved folder \xe2\x80\x9c%s\xe2\x80\x9d "
                                    "to \xe2\x80\x9c%s\xe2\x80\x9d",
                              fname, where);
                /* A trashed folder dragged out re-enters the tree as a
                 * normal folder; keep the selection on it.                 */
                if (lw->sel_kind == SB_KIND_TRASH_FOLDER &&
                    lw->sel_id == folder_id)
                    lw->sel_kind = SB_KIND_FOLDER;
            }
            g_free(where);
        }
        g_free(fname);
    }

    /* The rebuild is DEFERRED past this handler: rebuilding here replaced
     * the very row widget this drop target sits on (and the notes row the
     * drag source sits on) before GTK had finished the drop, and a drop
     * whose target vanished is reported as cancelled — the drag icon
     * floated back to where the drag started.  The return value is the
     * finish; the idle runs right after it.                              */
    if (success)
        g_idle_add(refresh_all_idle, lw);   /* tree shape and counts       */
    return success;
}

/* ===========================================================================
 * commands: new note / new folder / delete / rename
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * current_folder_id() — the folder new notes/folders should land in:
 * the selected folder, or the top level when the root or a tag is
 * selected.
 * ------------------------------------------------------------------------- */
static gint64
current_folder_id(OnLibrary *lw)
{
    return (lw->sel_kind == SB_KIND_FOLDER) ? lw->sel_id : 0;
}

/* ---------------------------------------------------------------------------
 * FolderPromptFunc — what prompt_for_folder() calls with the result once
 * the user pressed OK with a non-empty name.  Never called on Cancel.
 *   lw      — the library window.
 *   folder  — the id the prompt was opened for (see prompt_for_folder).
 *   name    — the trimmed name (borrowed).
 *   ai_mode — the chosen ON_AI_MODE_*.
 *   emoji   — the trimmed emoji, possibly "" (borrowed).
 * ------------------------------------------------------------------------- */
typedef void (*FolderPromptFunc)(OnLibrary *lw, gint64 folder,
                                 const gchar *name, gint ai_mode,
                                 const gchar *emoji);

/* ---------------------------------------------------------------------------
 * FolderPrompt — the state of one folder dialog, carried as object data
 * on the dialog for its response handler.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnLibrary        *lw;
    gint64            folder;        /* handed back to `done` untouched     */
    GtkWidget        *name_entry;
    GtkWidget        *emoji_entry;
    GtkWidget        *project_radio;
    GtkWidget        *custom_radio;
    FolderPromptFunc  done;
} FolderPrompt;

/* on_emoji_entry_changed() — the emoji entry's caret is hidden while the
 * field holds text (CSS class "notes-emoji-full"): Apple Color Emoji inks
 * past its advance, so a caret after it stood inside the glyph.          */
static void
on_emoji_entry_changed(GtkEditable *entry, gpointer user_data)
{
    (void)user_data;
    const gchar *text = gtk_editable_get_text(entry);
    if (text != NULL && *text != '\0')
        gtk_widget_add_css_class(GTK_WIDGET(entry), "notes-emoji-full");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(entry), "notes-emoji-full");
}

/* ---------------------------------------------------------------------------
 * on_folder_prompt_response() — the folder dialog closed: read the fields
 * and hand them to the continuation on OK (an empty name is a cancel).
 * ------------------------------------------------------------------------- */
static void
on_folder_prompt_response(GtkWindow *dlg, gint response, gpointer user_data)
{
    (void)dlg;
    FolderPrompt *p = user_data;     /* the dialog's state                  */
    if (response == GTK_RESPONSE_OK) {
        gchar *name = g_strstrip(g_strdup(
            gtk_editable_get_text(GTK_EDITABLE(p->name_entry))));
        if (*name != '\0') {
            gint mode = ON_AI_MODE_NORMAL;   /* the radio that is down      */
            if (gtk_check_button_get_active(
                    GTK_CHECK_BUTTON(p->project_radio)))
                mode = ON_AI_MODE_PROJECT;
            else if (gtk_check_button_get_active(
                         GTK_CHECK_BUTTON(p->custom_radio)))
                mode = ON_AI_MODE_CUSTOM;
            gchar *emoji = g_strstrip(g_strdup(
                gtk_editable_get_text(GTK_EDITABLE(p->emoji_entry))));
            p->done(p->lw, p->folder, name, mode, emoji);
            g_free(emoji);
        }
        g_free(name);
    }
}

/* ---------------------------------------------------------------------------
 * prompt_for_folder() — modal dialog: name entry + Normal/Project/Custom
 * AI mode radios + emoji entry.  ASYNCHRONOUS: returns as soon as the
 * dialog is up; `done` runs from its response.  The dialog is destroyed
 * with the library window, so `done` can never see a dead `lw`.
 *   lw            — the library window (dialog parent).
 *   title         — dialog window title.
 *   folder        — the id `done` is about (the parent of a new folder,
 *                   or the folder being edited), captured NOW: the
 *                   selection can move while the dialog is up (an IPC
 *                   command served meanwhile).
 *   initial_name  — pre-filled name, or NULL.
 *   initial_mode  — ON_AI_MODE_* to pre-select.
 *   initial_emoji — pre-filled emoji string, or NULL / "".
 *   done          — the continuation (see FolderPromptFunc).
 * ------------------------------------------------------------------------- */
static void
prompt_for_folder(OnLibrary *lw, const gchar *title, gint64 folder,
                  const gchar *initial_name, gint initial_mode,
                  const gchar *initial_emoji, FolderPromptFunc done)
{
    FolderPrompt *p = g_new0(FolderPrompt, 1);
    p->lw     = lw;
    p->folder = folder;
    p->done   = done;

    /* ── Field grid: labelled emoji + name rows ─────────────────────────── */
    GtkWidget *field_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(field_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(field_grid), 6);
    gtk_widget_set_margin_start(field_grid, 12);
    gtk_widget_set_margin_end(field_grid, 12);
    gtk_widget_set_margin_top(field_grid, 12);
    gtk_widget_set_margin_bottom(field_grid, 10);

    /* Emoji row.  The entry's OWN emoji chooser serves the picking: the
     * input hint says what the field is for, and the emoji icon at its
     * end is the click-to-pick the tooltip promises (GTK opens the
     * chooser popover from it; Ctrl+. / the context menu work too).       */
    GtkWidget *emoji_lbl = gtk_label_new("Emoji (optional):");
    gtk_label_set_xalign(GTK_LABEL(emoji_lbl), 1.0);
    gtk_grid_attach(GTK_GRID(field_grid), emoji_lbl, 0, 0, 1, 1);

    p->emoji_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(p->emoji_entry), 4);
    /* One emoji wide (plus its chooser icon): both width bounds, and the
     * theme's entry min-width lifted, or the field spans the dialog.     */
    gtk_editable_set_width_chars(GTK_EDITABLE(p->emoji_entry), 3);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(p->emoji_entry), 3);
    gtk_entry_set_alignment(GTK_ENTRY(p->emoji_entry), 0.5);
    gtk_entry_set_input_hints(GTK_ENTRY(p->emoji_entry),
                              GTK_INPUT_HINT_EMOJI);
    g_object_set(p->emoji_entry, "show-emoji-icon", TRUE, NULL);
    on_app_set_tooltip(p->emoji_entry,
                                "Optional emoji \xe2\x80\x94 click to pick");
    gtk_widget_add_css_class(p->emoji_entry, "notes-emoji-entry");
    g_signal_connect(p->emoji_entry, "changed",
                     G_CALLBACK(on_emoji_entry_changed), NULL);
    if (initial_emoji != NULL && *initial_emoji != '\0')
        gtk_editable_set_text(GTK_EDITABLE(p->emoji_entry), initial_emoji);
    gtk_widget_set_halign(p->emoji_entry, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(field_grid), p->emoji_entry, 1, 0, 1, 1);

    /* Folder Name row */
    GtkWidget *name_lbl = gtk_label_new("Folder Name:");
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 1.0);
    gtk_grid_attach(GTK_GRID(field_grid), name_lbl, 0, 1, 1, 1);

    p->name_entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(p->name_entry), TRUE);
    if (initial_name != NULL)
        gtk_editable_set_text(GTK_EDITABLE(p->name_entry), initial_name);
    gtk_widget_set_hexpand(p->name_entry, TRUE);
    gtk_grid_attach(GTK_GRID(field_grid), p->name_entry, 1, 1, 1, 1);

    /* AI summary mode row (row 2 of field_grid — aligns with labels above)   */
    GtkWidget *mode_lbl = gtk_label_new("AI summary mode:");
    gtk_label_set_xalign(GTK_LABEL(mode_lbl), 1.0);
    gtk_grid_attach(GTK_GRID(field_grid), mode_lbl, 0, 2, 1, 1);

    GtkWidget *radios_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *normal_radio = gtk_check_button_new_with_label("Normal");
    p->project_radio = gtk_check_button_new_with_label("Project");
    p->custom_radio  = gtk_check_button_new_with_label("Custom");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(p->project_radio),
                               GTK_CHECK_BUTTON(normal_radio));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(p->custom_radio),
                               GTK_CHECK_BUTTON(normal_radio));
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(initial_mode == ON_AI_MODE_PROJECT ? p->project_radio
                       : initial_mode == ON_AI_MODE_CUSTOM  ? p->custom_radio
                                                            : normal_radio),
        TRUE);
    gtk_box_append(GTK_BOX(radios_box), normal_radio);
    gtk_box_append(GTK_BOX(radios_box), p->project_radio);
    gtk_box_append(GTK_BOX(radios_box), p->custom_radio);
    gtk_grid_attach(GTK_GRID(field_grid), radios_box, 1, 2, 1, 1);

    /* The state rides on the grid, which the dialog takes.                */
    g_object_set_data_full(G_OBJECT(field_grid), "on-prompt", p, g_free);
    static const DialogButton BUTTONS[] = {
        { "_Cancel", GTK_RESPONSE_CANCEL, FALSE },
        { "_OK",     GTK_RESPONSE_OK,     TRUE  },
    };
    dialog_new(GTK_WINDOW(lw->window), title, field_grid, BUTTONS,
               G_N_ELEMENTS(BUTTONS), on_folder_prompt_response, p);
}

/* ---------------------------------------------------------------------------
 * ConfirmFunc — the continuation of confirm(): runs once the user has
 * answered, with the verdict.  Always called (so `data` can be released
 * either way) — except when the library window is destroyed with the
 * question still up, in which case the dialog dies with it (GtkAlertDialog
 * sets destroy-with-parent) and the continuation never runs; the few
 * bytes of `data` go with the window.
 *   lw   — the library window.
 *   yes  — TRUE if the user accepted.
 *   data — the caller's state.
 * ------------------------------------------------------------------------- */
typedef void (*ConfirmFunc)(OnLibrary *lw, gboolean yes, gpointer data);

/* ConfirmCtx — what confirm_finished() needs.                               */
typedef struct {
    OnLibrary   *lw;
    ConfirmFunc  done;
    gpointer     data;
} ConfirmCtx;

/* confirm_finished() — GAsyncReadyCallback of the confirm alert: button 1
 * is Yes (see confirm), anything else — No, Escape, closed — is no.        */
static void
confirm_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ConfirmCtx *ctx = user_data;
    gint button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source),
                                                 result, NULL);
    ctx->done(ctx->lw, button == 1, ctx->data);
    g_free(ctx);
}

/* ---------------------------------------------------------------------------
 * confirm() — modal yes/no question: a centered primary question over a
 * secondary line, No/Yes buttons, No the default and the cancel.
 * ASYNCHRONOUS: returns at once; the caller's tail is `done`.
 *   lw        — the library window (dialog parent).
 *   primary   — the question (e.g. "Delete this note?").
 *   secondary — supporting line (e.g. "This cannot be undone."); may be
 *               NULL.
 *   done      — the continuation (see ConfirmFunc).
 *   data      — passed to `done`.
 * ------------------------------------------------------------------------- */
static void
confirm(OnLibrary *lw, const gchar *primary, const gchar *secondary,
        ConfirmFunc done, gpointer data)
{
    static const gchar *const buttons[] = { "_No", "_Yes", NULL };
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", primary);
    if (secondary != NULL)           /* set_detail refuses NULL             */
        gtk_alert_dialog_set_detail(dialog, secondary);
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_set_modal(dialog, TRUE);

    ConfirmCtx *ctx = g_new(ConfirmCtx, 1);
    ctx->lw   = lw;
    ctx->done = done;
    ctx->data = data;
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(lw->window), NULL,
                            confirm_finished, ctx);
    g_object_unref(dialog);          /* the pending choose holds its own    */
}

/* on_new_note() — create a note in the current folder and open it.          */
static void
on_new_note(OnLibrary *lw)
{
    gint64 id = on_db_note_create(lw->app->db, current_folder_id(lw));
    if (id != 0) {
        refresh_all(lw);             /* the folder's count just grew        */
        on_editor_window_open(lw->app, id);
    }
}

gint64
on_library_quicknote(OnApp *app)
{
    gint64 id = on_db_note_create(app->db, 0);    /* 0 = the root folder    */
    if (id == 0)
        return 0;

    /* The full notify: a new note changes the root's count as well as the
     * notes pane, and the library may not even be the caller (the CLI's
     * "quicknote" lands here too).                                         */
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);

    GtkWidget *win = on_editor_window_open(app, id);
    if (win != NULL)
        gtk_window_present(GTK_WINDOW(win));
    return id;
}

/* on_quicknote() — toolbar button: a note in the root folder, whatever is
 * selected, with its editor to the front.                                   */
static void
on_quicknote(OnLibrary *lw)
{
    on_library_quicknote(lw->app);
}

/* new_folder_done() — the New Folder prompt's continuation: create the
 * folder under `parent` (the selection when the prompt opened).            */
static void
new_folder_done(OnLibrary *lw, gint64 parent, const gchar *name,
                gint ai_mode, const gchar *emoji)
{
    on_db_folder_create(lw->app->db, parent, name, ai_mode, emoji);
    refresh_sidebar(lw);
}

/* on_new_folder() — prompt for a name and AI mode; create under current.    */
static void
on_new_folder(OnLibrary *lw)
{
    prompt_for_folder(lw, "New Folder", current_folder_id(lw), NULL,
                      ON_AI_MODE_NORMAL, NULL, new_folder_done);
}

/* ---------------------------------------------------------------------------
 * selected_note_ids() — the ids of every note selected in whichever notes
 * view is active (both views allow multi-selection).
 * Returns a GArray of gint64; free with g_array_free(ids, TRUE).
 * ------------------------------------------------------------------------- */
static GArray *
selected_note_ids(OnLibrary *lw)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkBitset *sel = gtk_selection_model_get_selection(
        GTK_SELECTION_MODEL(lw->notes_sel));
    GtkBitsetIter it;
    guint pos;
    if (gtk_bitset_iter_init_first(&it, sel, &pos)) {
        do {
            OnNoteRow *row =
                g_list_model_get_item(G_LIST_MODEL(lw->notes_sorted), pos);
            if (row != NULL) {
                g_array_append_val(ids, row->id);
                g_object_unref(row);
            }
        } while (gtk_bitset_iter_next(&it, &pos));
    }
    gtk_bitset_unref(sel);
    return ids;
}

/* ---------------------------------------------------------------------------
 * close_editors_for_ids() — destroy any open editor window for the notes
 * in `ids`; their destroy handlers flush pending autosaves first.
 * ------------------------------------------------------------------------- */
static void
close_editors_for_ids(OnLibrary *lw, const gint64 *ids, gsize n)
{
    for (gsize i = 0; i < n; i++) {
        gint64 note_id = ids[i];
        GtkWidget *editor =
            g_hash_table_lookup(lw->app->editors, &note_id);
        if (editor != NULL)
            gtk_window_destroy(GTK_WINDOW(editor));
    }
}

/* ---------------------------------------------------------------------------
 * trash_notes_core() — move the notes in `ids` to the Trash: close any
 * open editors, flip the trash flag (ONE transaction) and post the status
 * message.  No confirmation: the move is reversible.  Refresh-free — the
 * callers refresh on their own schedule (the drag path must finish the
 * DnD handshake first).
 *   lw  — the library window.
 *   ids — the note ids.
 *   n   — how many.
 * Returns TRUE when the database move succeeded.
 * ------------------------------------------------------------------------- */
static gboolean
trash_notes_core(OnLibrary *lw, const gint64 *ids, guint n)
{
    close_editors_for_ids(lw, ids, n);
    if (!on_db_notes_trash(lw->app->db, ids, n))
        return FALSE;
    on_app_status(lw->app, "Moved %u note%s to Trash",
                  n, n == 1 ? "" : "s");
    return TRUE;
}

/* trash_notes() — action/menu path of the note-trash gesture: the shared
 * core plus the immediate refresh.                                          */
static void
trash_notes(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0)
        return;
    if (trash_notes_core(lw, (const gint64 *)ids->data, ids->len))
        refresh_all(lw);             /* counts + the Trash section          */
}

/* ---------------------------------------------------------------------------
 * trash_folder() — move folder `folder_id` (with its whole subtree) to
 * the Trash: close any editors open on its notes, flip the trash flag,
 * post the status message, and pull the selection off the now-hidden
 * folder.  Refresh-free, like trash_notes_core above.
 *   lw        — the library window.
 *   folder_id — the folder to trash.
 *   name      — its bare name (for the status message).
 * Returns TRUE when the database move succeeded.
 * ------------------------------------------------------------------------- */
static gboolean
trash_folder(OnLibrary *lw, gint64 folder_id, const gchar *name)
{
    GArray *nids = on_db_folder_note_ids(lw->app->db, folder_id);
    close_editors_for_ids(lw, (const gint64 *)nids->data, nids->len);
    g_array_free(nids, TRUE);
    if (!on_db_folder_trash(lw->app->db, folder_id))
        return FALSE;
    on_app_status(lw->app,
                  "Folder \xe2\x80\x9c%s\xe2\x80\x9d moved to Trash", name);
    if (lw->sel_kind == SB_KIND_FOLDER && lw->sel_id == folder_id) {
        lw->sel_kind = SB_KIND_ROOT;
        lw->sel_id   = 0;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * delete_notes_confirmed() — delete_notes_permanently()'s continuation:
 * on Yes, permanently delete the notes (closing any open editors first).
 *   data — the GArray of note ids (owned: freed here).
 * ------------------------------------------------------------------------- */
static void
delete_notes_confirmed(OnLibrary *lw, gboolean yes, gpointer data)
{
    GArray *ids = data;              /* the notes to delete                 */
    if (yes) {
        const gint64 *note_ids = (const gint64 *)ids->data;
        close_editors_for_ids(lw, note_ids, ids->len);
        on_db_notes_delete(lw->app->db, note_ids, ids->len);
        /* Evict deleted entries from the thumbnail cache so their textures
         * are freed now rather than held until the window closes.         */
        for (gsize i = 0; i < ids->len; i++)
            g_hash_table_remove(lw->thumb_cache, &note_ids[i]);
        refresh_all(lw);             /* tag list/counts may have changed    */
    }
    g_array_free(ids, TRUE);
}

/* ---------------------------------------------------------------------------
 * delete_notes_permanently() — confirm once, then permanently delete every
 * note in `ids`.  This is the Trash-view delete; normal views go through
 * trash_notes().
 *   ids — the note ids; OWNERSHIP IS TAKEN (the continuation frees them).
 * ------------------------------------------------------------------------- */
static void
delete_notes_permanently(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0) {
        g_array_free(ids, TRUE);
        return;
    }
    gchar *question = (ids->len == 1)
        ? g_strdup("Permanently delete this note?")
        : g_strdup_printf("Permanently delete these %u notes?", ids->len);
    confirm(lw, question, "This cannot be undone.",
            delete_notes_confirmed, ids);
    g_free(question);
}

/* on_delete_note() — action-bar Delete: trash every selected note, or
 * permanently delete it when the Trash is what's being viewed.              */
static void
on_delete_note(OnLibrary *lw)
{
    GArray *ids = selected_note_ids(lw);
    if (in_trash_view(lw)) {
        delete_notes_permanently(lw, ids);   /* takes the ids               */
    } else {
        trash_notes(lw, ids);
        g_array_free(ids, TRUE);
    }
}

/* ---------------------------------------------------------------------------
 * delete_folder_confirmed() — on_delete_folder()'s continuation for a
 * folder in the Trash: on Yes, permanently delete it and its subtree.
 *   data — the folder id (a heap gint64, owned: freed here).
 * ------------------------------------------------------------------------- */
static void
delete_folder_confirmed(OnLibrary *lw, gboolean yes, gpointer data)
{
    gint64 folder_id = *(gint64 *)data;
    g_free(data);
    if (!yes)
        return;
    GArray *ids = on_db_folder_note_ids(lw->app->db, folder_id);
    close_editors_for_ids(lw, (const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
    on_db_folder_delete(lw->app->db, folder_id);
    if (lw->sel_kind == SB_KIND_TRASH_FOLDER && lw->sel_id == folder_id) {
        lw->sel_kind = SB_KIND_TRASH;
        lw->sel_id   = 0;
    }
    refresh_all(lw);
}

/* on_delete_folder() — sidebar-toolbar Delete: move the selected folder
 * (with its whole subtree) to the Trash; a folder already in the Trash is
 * permanently deleted instead (after confirmation).                         */
static void
on_delete_folder(OnLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_FOLDER) {
        if (trash_folder(lw, lw->sel_id, lw->sel_name))
            refresh_all(lw);
    } else if (lw->sel_kind == SB_KIND_TRASH_FOLDER) {
        gint64 *folder_id = g_new(gint64, 1);   /* captured for the answer */
        *folder_id = lw->sel_id;
        confirm(lw,
                "Permanently delete this folder and everything inside it?",
                "This cannot be undone.",
                delete_folder_confirmed, folder_id);
    }
}

/* on_restore_folder() — Trash context menu: put the selected trashed
 * folder (and its subtree) back where it was deleted from; the selection
 * follows it to its restored spot.                                          */
static void
on_restore_folder(OnLibrary *lw)
{
    if (lw->sel_kind != SB_KIND_TRASH_FOLDER)
        return;
    if (on_db_folder_restore(lw->app->db, lw->sel_id)) {
        on_app_status(lw->app,
                      "Folder \xe2\x80\x9c%s\xe2\x80\x9d restored",
                      lw->sel_name);
        lw->sel_kind = SB_KIND_FOLDER;   /* same id, back in the tree       */
        refresh_all(lw);
    }
}

/* empty_trash_confirmed() — on_empty_trash()'s continuation: on Yes, purge
 * the Trash.                                                                */
static void
empty_trash_confirmed(OnLibrary *lw, gboolean yes, gpointer data)
{
    (void)data;
    if (!yes)
        return;

    /* Close editors for every note the purge will take with it —
     * including notes inside trashed folder subtrees.  Evict from the
     * thumbnail cache at the same time so the textures are freed now.     */
    GArray *ids = on_db_trash_note_ids(lw->app->db);
    const gint64 *note_ids = (const gint64 *)ids->data;
    close_editors_for_ids(lw, note_ids, ids->len);
    for (gsize i = 0; i < ids->len; i++)
        g_hash_table_remove(lw->thumb_cache, &note_ids[i]);
    g_array_free(ids, TRUE);

    if (on_db_trash_empty(lw->app->db)) {
        on_app_status(lw->app, "Trash emptied");
        refresh_all(lw);             /* the Trash section disappears        */
    }
}

/* on_empty_trash() — Trash context menu: permanently delete everything in
 * the Trash after one confirmation.                                         */
static void
on_empty_trash(OnLibrary *lw)
{
    confirm(lw, "Permanently delete everything in the Trash?",
            "This cannot be undone.", empty_trash_confirmed, NULL);
}

/* folder_info_done() — the Folder Info prompt's continuation: write the
 * edited name, AI mode and emoji back to `folder`.                          */
static void
folder_info_done(OnLibrary *lw, gint64 folder, const gchar *name,
                 gint ai_mode, const gchar *emoji)
{
    on_db_folder_update(lw->app->db, folder, name, ai_mode, emoji);
    refresh_sidebar(lw);
}

/* on_rename_folder() — "Info…" menu: edit folder name and AI mode.          */
static void
on_rename_folder(OnLibrary *lw)
{
    if (lw->sel_kind != SB_KIND_FOLDER)
        return;
    gint   cur_mode  =               /* pre-fill radios with current setting */
        on_db_folder_get_ai_mode(lw->app->db, lw->sel_id);
    gchar *cur_emoji =               /* pre-fill emoji with current value    */
        on_db_folder_get_emoji(lw->app->db, lw->sel_id);
    prompt_for_folder(lw, "Folder Info", lw->sel_id, lw->sel_name,
                      cur_mode, cur_emoji, folder_info_done);
    g_free(cur_emoji);
}

/* on_open_search() — sidebar-toolbar Search: open the search window (it
 * reads the live library selection each time Search is pressed).  The
 * scope radio defaults to the selection when it is narrower than "all" —
 * a folder or tag; the All Notes/Trash/Pinned sections have no folder
 * scope, so they default to searching everything.                           */
static void
on_open_search(OnLibrary *lw)
{
    gboolean scoped = lw->sel_kind == SB_KIND_FOLDER ||
                      lw->sel_kind == SB_KIND_TAG ||
                      lw->sel_kind == SB_KIND_TRASH_FOLDER;
    on_search_window_open(lw->app, scoped);
}

/* ---------------------------------------------------------------------------
 * on_open_media() — toolbar Media: open the media browser over EXACTLY the
 * notes the notes pane is currently listing (notes_for_selection), so
 * "the images in this folder" means the same thing the pane does.  The
 * window takes a snapshot, so the list is fetched once here.
 * ------------------------------------------------------------------------- */
static void
on_open_media(OnLibrary *lw)
{
    /* The Action Items view is not a place, and its media set is every
     * note's — say so rather than naming a row that isn't a folder.        */
    const gchar *label = (lw->sel_kind == SB_KIND_ACTIONS)
        ? "All Notes"
        : (lw->sel_name != NULL ? lw->sel_name : "Notes");

    GList *notes = notes_for_selection(lw);
    on_media_window_open(lw->app, label, notes);
    on_db_note_list_free(notes);
}

/* ---------------------------------------------------------------------------
 * on_toolbar_search_activate() — Enter in the toolbar's search entry: open
 * a search window already loaded with the typed query and run it against
 * All Notes, case insensitively.  The text is left in the entry so the same
 * query can be fired again.
 *   entry     — the GtkSearchEntry that was activated.
 *   user_data — the owning library window.
 * ------------------------------------------------------------------------- */
static void
on_toolbar_search_activate(GtkSearchEntry *entry, gpointer user_data)
{
    OnLibrary *lw = user_data;        /* owning library window               */
    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (query == NULL || *query == '\0')
        return;                      /* nothing typed: no window            */
    on_search_window_open_query(lw->app, query);
}

/* on_open_settings() — File → Settings… (the macOS app menu's Preferences)  */
static void
on_open_settings(OnLibrary *lw)
{
    on_settings_window_open(lw->app);
}

/* ---------------------------------------------------------------------------
 * LwRef — a weak handle on the library for continuations that a native
 * file chooser (on_app_pick_path) may deliver AFTER the library window is
 * gone: the chooser is not destroyed with its parent, so its callback
 * cannot hold a raw OnLibrary pointer.  The handle is a weak pointer to
 * the window; lw_ref_take() resolves it to the live state or NULL.  The
 * in-process dialogs (GtkDialog, GtkAlertDialog) need none of this: they
 * are destroyed with the parent and their callbacks never run afterwards.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *window;               /* weak: NULL once the library is gone */
} LwRef;

/* lw_ref_new() — take a weak handle on `lw`'s window.                       */
static LwRef *
lw_ref_new(OnLibrary *lw)
{
    LwRef *ref = g_new0(LwRef, 1);
    ref->window = lw->window;
    g_object_add_weak_pointer(G_OBJECT(lw->window), (gpointer *)&ref->window);
    return ref;
}

/* lw_ref_take() — resolve and free a handle: the library, or NULL when its
 * window has been destroyed since.                                         */
static OnLibrary *
lw_ref_take(LwRef *ref)
{
    OnLibrary *lw = NULL;            /* the live state, if any              */
    if (ref->window != NULL) {
        lw = g_object_get_data(G_OBJECT(ref->window), "on-library");
        g_object_remove_weak_pointer(G_OBJECT(ref->window),
                                     (gpointer *)&ref->window);
    }
    g_free(ref);
    return lw;
}

/* OpenDb — the state of one File → Open Database… flow across its two
 * dialogs.                                                                  */
typedef struct {
    LwRef *ref;                      /* the library (weak)                  */
    gchar *path;                     /* the chosen file (owned)             */
} OpenDb;

/* ---------------------------------------------------------------------------
 * open_db_switch() — the last step of File → Open Database…: switch to
 * `path`, as the new default (persisted) or for this session only.
 * ------------------------------------------------------------------------- */
static void
open_db_switch(OnLibrary *lw, const gchar *path, gboolean set_default)
{
    OnApp *app = lw->app;
    on_app_close_all_editors(app);
    gchar *old_path = g_strdup(app->db->path);
    on_db_close(app->db);
    app->db = on_db_open(path);

    if (app->db == NULL) {
        on_app_notice(GTK_WINDOW(lw->window), "Notes - Database Error",
                      "Could not open:\n%s", path);
        /* Revert to old database. */
        app->db = on_db_open(old_path);
        g_free(old_path);
        return;
    }
    g_free(old_path);

    if (set_default) {
        gchar *new_dir = g_path_get_dirname(path);
        g_free(app->db_dir);
        app->db_dir = g_strdup(new_dir);
        app->db_transient = FALSE;
        on_app_config_set("db_dir",  new_dir);
        g_free(new_dir);
    } else {
        app->db_transient = TRUE;   /* session only: don't persist anything */
    }

    /* The backup timer carries the db path, so it must be re-armed onto
     * the file that is now open (see backup.h).                           */
    on_backup_auto_start(app, app->db->path);

    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
    on_app_status(app, "DB at %s loaded", app->db->path);
    on_app_db_health_start(app);     /* every opened database is checked    */
}

/* open_db_chosen() — step 2 answered (0 Cancel, 1 Session Only, 2 Set as
 * Default): switch, or drop the whole thing.                                */
static void
open_db_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    OpenDb *od = user_data;
    gint choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source),
                                                 result, NULL);
    OnLibrary *lw = lw_ref_take(od->ref);
    if (lw != NULL && choice > 0)
        open_db_switch(lw, od->path, choice == 2);
    g_free(od->path);
    g_free(od);
}

/* open_db_picked() — step 1 done: a file was chosen (or not).  A file that
 * is already open needs nothing; otherwise ask how to open it.              */
static void
open_db_picked(gchar *path, gpointer user_data)
{
    OpenDb *od = user_data;
    od->path = path;
    OnLibrary *lw = od->ref->window != NULL   /* peek: the handle lives on */
        ? g_object_get_data(G_OBJECT(od->ref->window), "on-library") : NULL;
    if (lw == NULL || path == NULL ||
        g_strcmp0(path, lw->app->db->path) == 0) {
        lw_ref_take(od->ref);
        g_free(od->path);
        g_free(od);
        return;
    }

    static const gchar *const buttons[] =
        { "_Cancel", "_Session Only", "Set as _Default", NULL };
    gchar *display = g_path_get_basename(path);
    GtkAlertDialog *dlg = gtk_alert_dialog_new(
        "Open \xe2\x80\x9c%s\xe2\x80\x9d as your new default database, or "
        "for this session only?", display);
    g_free(display);
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 2);
    gtk_alert_dialog_set_modal(dlg, TRUE);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(lw->window), NULL,
                            open_db_chosen, od);
    g_object_unref(dlg);
}

/* ---------------------------------------------------------------------------
 * on_open_db() — File → Open Database…: let the user pick any .db file and
 * open it, either as the new permanent default or for this session only.
 * Two dialogs, both asynchronous: the chooser (open_db_picked), then the
 * default-or-session question (open_db_chosen → open_db_switch).
 * ------------------------------------------------------------------------- */
static void
on_open_db(OnLibrary *lw)
{
    OpenDb *od = g_new0(OpenDb, 1);
    od->ref = lw_ref_new(lw);
    on_app_pick_path(GTK_WINDOW(lw->window), "Open Database",
                     ON_PICK_OPEN, "_Open", "SQLite Database (*.db)", "*.db",
                     NULL, open_db_picked, od);
}

/* ---------------------------------------------------------------------------
 * on_about() — File → About: the standard about dialog with the app icon,
 * author, build date and a link to the BSD license.  Presented and left to
 * its own close button (a GtkAboutDialog destroys itself on close).
 * ------------------------------------------------------------------------- */
static void
on_about(OnLibrary *lw)
{
    /* 128x128 logo from composition.png.  Decoded at that pixel size, not
     * the scale-factor multiple: the dialog's image draws a paintable at
     * its intrinsic size, so a 2x decode would show at 256 logical px.     */
    gchar *icon_path = g_build_filename(lw->app->icons_dir, "composition.png",
                                        NULL);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_size(icon_path, 128, 128,
                                                       NULL);
    g_free(icon_path);

    const gchar *authors[] = { "Ian Campbell", "Claude Sonnet 4.5", "And thanks to Blue Note Records. Both for the great double entendre and for amazing music that shaped my youth.", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      "Notes");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), ON_VERSION);
    if (logo != NULL) {
        GdkTexture *texture = on_app_texture_for_pixbuf(logo);
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog),
                                  GDK_PAINTABLE(texture));
        g_object_unref(texture);
        g_object_unref(logo);
    }
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

    /* Database vitals: entry counts, location, on-disk size.               */
    gint n_notes, n_folders, n_tags;     /* totals across the database      */
    on_db_totals(lw->app->db, &n_notes, &n_folders, &n_tags);
    GStatBuf st;                     /* for the database file size          */
    gchar *size_str = (g_stat(lw->app->db->path, &st) == 0)
                      ? g_format_size((guint64)st.st_size)
                      : g_strdup("unknown");

    /* __DATE__/__TIME__ expand when this file is compiled — the closest
     * portable thing to a "last compiled" stamp.                           */
    gchar *comments = g_strdup_printf(
        "Compiled " __DATE__ " " __TIME__ "\n\n"
        "Database: %s\n"
        "%d notes in %d folders, %d tags \xe2\x80\x94 %s on disk",
        lw->app->db->path, n_notes, n_folders, n_tags, size_str);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), comments);
    g_free(comments);
    g_free(size_str);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_BSD);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://opensource.org/license/bsd-3-clause");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                       "BSD License");

    gtk_window_present(GTK_WINDOW(dialog));
}

/* ---------------------------------------------------------------------------
 * on_quit() — File → Quit: destroy every application window.  Editor
 * windows flush their final autosave from their destroy handlers, and the
 * GTK main loop ends once the last window is gone.
 * ------------------------------------------------------------------------- */
static void
on_quit(OnLibrary *lw)
{
    GList *windows =                 /* copy: destroying mutates the list   */
        g_list_copy(gtk_application_get_windows(lw->app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_window_destroy(GTK_WINDOW(l->data));
    g_list_free(windows);
}

/* on_sidebar_search_here() — sidebar context menu Search: the clicked row
 * is already selected, so a scoped search targets it directly.              */
static void
on_sidebar_search_here(OnLibrary *lw)
{
    on_search_window_open(lw->app, TRUE);
}

/* utf8_casecmp() — case-insensitive UTF-8 string comparison (casefold
 * both sides; NULL compares as the empty string).                           */
static gint
utf8_casecmp(const gchar *a, const gchar *b)
{
    gchar *ca = g_utf8_casefold(a != NULL ? a : "", -1);
    gchar *cb = g_utf8_casefold(b != NULL ? b : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca);
    g_free(cb);
    return result;
}

/* ---------------------------------------------------------------------------
 * on_sort_subfolders() — folder context menu: order the selected
 * folder's (or the root's) DIRECT children alphabetically and persist
 * that as their sort_order.  One level only — each folder's own order
 * stays whatever the user made it.
 * ------------------------------------------------------------------------- */
static void
on_sort_subfolders(OnLibrary *lw)
{
    if (lw->sel_kind != SB_KIND_FOLDER && lw->sel_kind != SB_KIND_ROOT)
        return;
    gint64 parent =                  /* whose children get sorted           */
        (lw->sel_kind == SB_KIND_FOLDER) ? lw->sel_id : 0;

    gint n = on_db_folder_sort_children(lw->app->db, parent);
    if (n > 1) {
        on_app_status(lw->app, "Sorted %d subfolders alphabetically", n);
        refresh_all(lw);
    }
}

/* ---------------------------------------------------------------------------
 * menu_section_end() — close a section of a context menu: append it to
 * `menu` and start a fresh one.  Sections are what draw the separators.
 *   menu    — the menu being built.
 *   section — in/out: the section to close (unreffed), replaced by a new
 *             empty one.
 * ------------------------------------------------------------------------- */
static void
menu_section_end(GMenu *menu, GMenu **section)
{
    g_menu_append_section(menu, NULL, G_MENU_MODEL(*section));
    g_object_unref(*section);
    *section = g_menu_new();
}

/* ---------------------------------------------------------------------------
 * row_click_gesture() — a right-click gesture on one row widget of a list
 * view, installed by the factory's setup, carrying the row's GtkListItem
 * as "on-item" so the handler can read the row it was pressed on.  CAPTURE
 * phase, so it runs before the row's own gesture (which selects on
 * release); the handler CLAIMs the sequence when it takes the press.
 *   widget  — the row widget (the factory child).
 *   item    — its list item.
 *   pressed — the "pressed" handler.
 *   data    — its user data.
 * ------------------------------------------------------------------------- */
static void
row_click_gesture(GtkWidget *widget, GtkListItem *item, GCallback pressed,
                  gpointer data)
{
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_object_set_data(G_OBJECT(click), "on-item", item);
    g_signal_connect(click, "pressed", pressed, data);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
}

/* ---------------------------------------------------------------------------
 * on_sidebar_pressed() — right click on a folder/tag row: select it and
 * show a context menu mirroring the sidebar toolbar (folder actions +
 * scoped search).  The items name actions, so the row kind only decides
 * which items appear; the handlers read the selection this press just
 * made.  The press is claimed either way.
 *   x, y — the press in the row widget's coordinates.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                   gpointer user_data)
{
    (void)n_press;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkListItem *item = row_item(g);
    OnSbRow *r = sb_item_row(item, NULL);
    if (r == NULL)
        return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    gint kind = r->kind;
    if (kind == SB_KIND_TAGS_HEADER)
        return;                      /* consumed, but no menu               */
    gtk_single_selection_set_selected(lw->sb_sel,
                                      gtk_list_item_get_position(item));

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();

    if (kind == SB_KIND_TRASH) {
        /* The Trash row's only action: purge it.                           */
        g_menu_append(section, "_Empty Trash\xe2\x80\xa6", "win.empty-trash");
    } else {
        if (kind == SB_KIND_TRASH_FOLDER) {
            g_menu_append(section, "_Restore Folder", "win.folder-restore");
            g_menu_append(section, "Delete _Permanently",
                          "win.delete-folder");
            menu_section_end(menu, &section);
        } else if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT) {
            g_menu_append(section,
                          kind == SB_KIND_FOLDER ? "New _Subfolder\xe2\x80\xa6"
                                                 : "New _Folder\xe2\x80\xa6",
                          "app.new-folder");
            if (kind == SB_KIND_FOLDER) {
                g_menu_append(section, "Info\xe2\x80\xa6", "win.folder-info");
                g_menu_append(section, "Move to _Trash",
                              "win.delete-folder");
            }
            g_menu_append(section, "Sort Subfolders _Alphabetically",
                          "win.sort-subfolders");
            menu_section_end(menu, &section);
        }
        g_menu_append(section, "Search _Here\xe2\x80\xa6", "win.search-here");
    }
    menu_section_end(menu, &section);
    g_object_unref(section);

    on_app_menu_popup(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
                      G_MENU_MODEL(menu), x, y);
}

/* ===========================================================================
 * view mode & export
 * =========================================================================== */

/* on_view_list() / on_view_grid() — flip the stack between the modes.
 * Both record the choice in grid_pref so leaving the Action Items view
 * restores it; while that view is showing they only set the preference
 * (refresh_notes keeps the actions child on top for that selection).       */
static void
on_view_list(OnLibrary *lw)
{
    lw->grid_pref = FALSE;
    if (lw->sel_kind == SB_KIND_ACTIONS)
        return;
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "list");
}

static void
on_view_grid(OnLibrary *lw)
{
    lw->grid_pref = TRUE;
    if (lw->sel_kind == SB_KIND_ACTIONS)
        return;
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "grid");
    refresh_notes(lw);               /* fill the thumbnails list mode skips */
}

/* The View menu's sidebar item is an ACTION, not a state: its label names
 * what a click DOES.  Two items — "Hide Sidebar" on "app.sidebar-hide",
 * "Show Sidebar" on "app.sidebar-show" — each with hidden-when set to
 * action-disabled, so exactly the one whose action is enabled shows.  The
 * label is therefore chosen by enabling an action, never by editing the
 * model: a live model edit trips a misplaced assertion in MacPorts' GTK
 * (patch-gtk-menu-crash.diff guards *change_point before the loop, which
 * every append legitimately has NULL) — harmless, but a Gtk-CRITICAL on
 * every toggle.                                                            */
#define SIDEBAR_LABEL_TO_HIDE "Hide Sidebar"
#define SIDEBAR_LABEL_TO_SHOW "Show Sidebar"

/* ---------------------------------------------------------------------------
 * sidebar_menu_sync() — offer the View menu's sidebar item that fits the
 * pane's LIVE visibility: "Hide Sidebar" while the folder pane is up,
 * "Show Sidebar" while it is not, by enabling one action and disabling the
 * other.  The menus GTK rendered from the model (in-window and the native
 * macOS bar alike) show and hide the items themselves.  NULL-safe, so the
 * toolbar button works during construction, before the actions exist.
 *
 * Inputs:
 *   lw — the library window.
 *
 * Output:
 *   none.
 * ------------------------------------------------------------------------- */
static void
sidebar_menu_sync(OnLibrary *lw)
{
    GActionMap *map = G_ACTION_MAP(lw->app->gtk_app);
    GAction *hide = g_action_map_lookup_action(map, "sidebar-hide");
    GAction *show = g_action_map_lookup_action(map, "sidebar-show");
    if (hide == NULL || show == NULL)
        return;
    gboolean visible = gtk_widget_get_visible(lw->sidebar_box);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(hide), visible);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(show), !visible);
}

/* ---------------------------------------------------------------------------
 * sidebar_set_visible() — show or hide the folder/tag pane and keep the View
 * menu's label in step.  THE one place that changes that visibility: the
 * toolbar button and the menu item both route through here, so the two can
 * never come to disagree about what the sidebar is doing.
 *
 * Inputs:
 *   lw   — the library window.
 *   show — TRUE to show the pane, FALSE to hide it.
 *
 * Output:
 *   none.
 * ------------------------------------------------------------------------- */
static void
sidebar_set_visible(OnLibrary *lw, gboolean show)
{
    gtk_widget_set_visible(lw->sidebar_box, show);
    sidebar_menu_sync(lw);
}

/* on_toggle_sidebar() — the toolbar button AND the View menu item: the
 * notes view takes the whole window while the folder pane is hidden.        */
static void
on_toggle_sidebar(OnLibrary *lw)
{
    sidebar_set_visible(lw, !gtk_widget_get_visible(lw->sidebar_box));
}

/* on_sidebar_hide() / on_sidebar_show() — the View menu's two items.       */
static void
on_sidebar_hide(OnLibrary *lw)
{
    sidebar_set_visible(lw, FALSE);
}

static void
on_sidebar_show(OnLibrary *lw)
{
    sidebar_set_visible(lw, TRUE);
}

/* ---------------------------------------------------------------------------
 * view_shows_grid() — is the notes pane showing the GRID?
 *
 * THE one reading of that, shared by the toolbar button's icon and by the
 * click it performs, so the picture can never promise a switch the click
 * will not deliver (the same rule img_nav_delta keeps for the image
 * viewer's Previous/Next).
 *
 * The stack has a THIRD child: the Action Items view is neither list nor
 * grid, and the button there is about the mode the notes pane will come
 * BACK to — which is grid_pref, the preference on_view_list/on_view_grid
 * keep updating while that view is up.
 *
 * Inputs:
 *   lw — the library window.
 *
 * Output:
 *   TRUE when a click should switch to the list, FALSE when it should
 *   switch to the grid.
 * ------------------------------------------------------------------------- */
static gboolean
view_shows_grid(OnLibrary *lw)
{
    const gchar *mode =              /* "list", "grid" or "actions"         */
        gtk_stack_get_visible_child_name(GTK_STACK(lw->stack));
    return g_strcmp0(mode, "actions") == 0 ? lw->grid_pref
                                           : g_strcmp0(mode, "grid") == 0;
}

/* ---------------------------------------------------------------------------
 * view_button_sync() — point the toolbar's List/Grid button at the view a
 * click switches TO: grid.png while the list is showing, list.png while the
 * grid is.  The button pictures the DESTINATION, not the current state —
 * the same contract the View menu's Show/Hide Sidebar label keeps.
 *
 * Driven by the stack's own "notify::visible-child-name" rather than called
 * from each place that switches views: the child is set from five of them
 * (the two View actions, entering and leaving the Action Items view, and
 * construction), and a sixth added later would silently skip a call.
 *
 * NULL-safe, so the stack can be built before the toolbar.
 * ------------------------------------------------------------------------- */
static void
view_button_sync(OnLibrary *lw)
{
    if (lw->view_btn == NULL)
        return;
    gboolean grid = view_shows_grid(lw);   /* what is on screen now         */
    /* Fallback glyphs stand in for a missing file, so they must flip too:
     * \xe2\x98\xb0 is a list, \xe2\x8a\x9e a grid.                                 */
    on_app_tool_item_set_icon(lw->app, lw->view_btn,
                              grid ? "list" : "grid",
                              grid ? "\xe2\x98\xb0" : "\xe2\x8a\x9e");
    /* The accessible name is the button's "label" (on_app_tool_item_new
     * sets it the same way); the tooltip is the visible one.              */
    gtk_accessible_update_property(GTK_ACCESSIBLE(lw->view_btn),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   grid ? "List" : "Grid", -1);
    on_app_set_tooltip(lw->view_btn,
        grid ? "Switch to list view" : "Switch to grid view");
}

/* ---------------------------------------------------------------------------
 * done_button_sync() — point the toolbar's completed-items toggle at the
 * ACTION a click performs, the same rule as the List/Grid button (and the
 * sister Tasks app's identical button): hidden.png while completed action
 * items are listed (click to hide them), visible.png while they are hidden
 * (click to bring them back).  Runs from refresh_notes, so the Settings
 * checkbox for the same setting keeps it honest too.
 * ------------------------------------------------------------------------- */
static void
done_button_sync(OnLibrary *lw)
{
    if (lw->done_btn == NULL)
        return;
    gboolean show = lw->app->show_done_actions;
    on_app_tool_item_set_icon(lw->app, lw->done_btn,
                              show ? "hidden" : "visible",
                              "\xf0\x9f\x91\x81");   /* an eye either way */
    gtk_accessible_update_property(GTK_ACCESSIBLE(lw->done_btn),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   show ? "Hide Completed" : "Show Completed",
                                   -1);
    on_app_set_tooltip(lw->done_btn,
        show ? "Hide completed action items"
             : "Show completed action items");
    done_menu_sync(lw);
}

/* done_menu_sync() — offer the View menu's completed-items entry that fits
 * the live setting: "Hide Completed" while they are listed, "Show
 * Completed" while they are not, by enabling one of "app.done-hide" /
 * "app.done-show" and disabling the other (the sidebar item's device, see
 * sidebar_menu_sync).  NULL-safe during construction.                     */
static void
done_menu_sync(OnLibrary *lw)
{
    GActionMap *map = G_ACTION_MAP(lw->app->gtk_app);
    GAction *hide = g_action_map_lookup_action(map, "done-hide");
    GAction *show = g_action_map_lookup_action(map, "done-show");
    if (hide == NULL || show == NULL)
        return;
    gboolean shown = lw->app->show_done_actions;
    g_simple_action_set_enabled(G_SIMPLE_ACTION(hide), shown);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(show), !shown);
}

/* done_set_shown() — THE one place the show_done_actions setting changes
 * from this window (the toolbar toggle, the two menu items): persist and
 * rebuild; refresh_notes re-points the button and the menu.  Settings'
 * "Show completed action items" box writes the same key and notifies.   */
static void
done_set_shown(OnLibrary *lw, gboolean shown)
{
    lw->app->show_done_actions = shown;
    on_app_config_set("show_done_actions", shown ? "1" : "0");
    refresh_all(lw);
}

/* on_toggle_done() — "win.toggle-done", the toolbar button.                 */
static void
on_toggle_done(OnLibrary *lw)
{
    done_set_shown(lw, !lw->app->show_done_actions);
}

/* on_done_hide() / on_done_show() — the View menu's two items.              */
static void
on_done_hide(OnLibrary *lw)
{
    done_set_shown(lw, FALSE);
}

static void
on_done_show(OnLibrary *lw)
{
    done_set_shown(lw, TRUE);
}

/* on_view_stack_changed() — the stack switched children: re-point the
 * List/Grid button's icon.  One connection covers every route in.           */
static void
on_view_stack_changed(GObject *stack, GParamSpec *pspec, gpointer user_data)
{
    (void)stack; (void)pspec;
    view_button_sync(user_data);
}

/* on_toggle_view() — toolbar List/Grid button: switch to whichever notes
 * view the button is currently picturing.                                   */
static void
on_toggle_view(OnLibrary *lw)
{
    if (view_shows_grid(lw))
        on_view_list(lw);
    else
        on_view_grid(lw);
}

/* ---------------------------------------------------------------------------
 * ExportJob — one export flow across its folder chooser: what to export
 * and where the result goes.
 *
 * Fields:
 *   ref    — the library (weak: the chooser can outlive the window).
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN.
 *   ids    — the selected note ids (owned), or NULL for every note.
 * ------------------------------------------------------------------------- */
typedef struct {
    LwRef          *ref;
    OnExportFormat  format;
    GArray         *ids;
} ExportJob;

/* ---------------------------------------------------------------------------
 * export_dir_picked() — on_app_pick_path's continuation for every export
 * flow: write the notes into the chosen directory and report the result
 * (fire-and-forget notice: how many were written, or why the run failed).
 *   dir       — the chosen directory (owned), or NULL when cancelled.
 *   user_data — the ExportJob (owned: freed here).
 * ------------------------------------------------------------------------- */
static void
export_dir_picked(gchar *dir, gpointer user_data)
{
    ExportJob *job = user_data;
    OnLibrary *lw  = lw_ref_take(job->ref);
    if (lw != NULL && dir != NULL) {
        gchar *err = NULL;           /* exporter error message              */
        gint   n;                    /* notes written; negative = failed    */
        if (job->ids == NULL) {
            n = on_export_all(lw->app, dir, job->format, &err);
        } else {
            n = 0;
            for (guint i = 0; i < job->ids->len; i++)
                if (on_export_note(lw->app,
                                   g_array_index(job->ids, gint64, i),
                                   dir, job->format))
                    n++;
        }
        if (n >= 0)
            on_app_notice(GTK_WINDOW(lw->window), NULL,
                          "Exported %d note%s to\n%s",
                          n, n == 1 ? "" : "s", dir);
        else
            on_app_notice(GTK_WINDOW(lw->window), NULL,
                          "Export failed: %s",
                          err != NULL ? err : "unknown error");
        g_free(err);
    }
    if (job->ids != NULL)
        g_array_free(job->ids, TRUE);
    g_free(job);
    g_free(dir);
}

/* ---------------------------------------------------------------------------
 * run_export() — pick a destination directory and export the notes in
 * `ids` (or every note when NULL) in the requested format; the chooser is
 * asynchronous and export_dir_picked does the rest.
 *   lw     — the library window.
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN.
 *   ids    — the note ids (OWNERSHIP IS TAKEN), or NULL for all notes.
 * ------------------------------------------------------------------------- */
static void
run_export(OnLibrary *lw, OnExportFormat format, GArray *ids)
{
    ExportJob *job = g_new0(ExportJob, 1);
    job->ref    = lw_ref_new(lw);
    job->format = format;
    job->ids    = ids;
    on_app_pick_path(GTK_WINDOW(lw->window), "Choose Export Folder",
                     ON_PICK_FOLDER, "_Export", NULL, NULL, NULL,
                     export_dir_picked, job);
}

/* on_export_html() / on_export_markdown() — File-menu export entries.       */
static void
on_export_html(OnLibrary *lw)
{
    run_export(lw, ON_EXPORT_HTML, NULL);
}

static void
on_export_markdown(OnLibrary *lw)
{
    run_export(lw, ON_EXPORT_MARKDOWN, NULL);
}

/* ===========================================================================
 * per-note context menu (right click in list or grid)
 * =========================================================================== */

/* Context-menu item handlers.  Delete, export and restore act on the whole
 * selection; Open and Pin are the two that take a parameter — the CLICKED
 * note's id, and the pin state the clicked note's current state implies —
 * carried as the menu item's action target.                                 */

/* on_note_open() — "win.note-open(x)": open exactly the clicked note.       */
static void
on_note_open(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_editor_window_open(lw->app, g_variant_get_int64(param));
}

/* ctx_export_selection() — export every selected note to one directory.     */
static void
ctx_export_selection(OnLibrary *lw, OnExportFormat format)
{
    GArray *ids = selected_note_ids(lw);
    if (ids->len == 0) {
        g_array_free(ids, TRUE);
        return;
    }
    run_export(lw, format, ids);     /* takes the ids                       */
}

static void
on_note_export_html(OnLibrary *lw)
{
    ctx_export_selection(lw, ON_EXPORT_HTML);
}

static void
on_note_export_md(OnLibrary *lw)
{
    ctx_export_selection(lw, ON_EXPORT_MARKDOWN);
}

/* on_note_restore() — Trash-view context menu: put every selected note
 * back where it was deleted from (top level when that folder is itself
 * still in the Trash).                                                      */
static void
on_note_restore(OnLibrary *lw)
{
    GArray *ids = selected_note_ids(lw);
    if (ids->len > 0) {
        for (guint i = 0; i < ids->len; i++)
            on_db_note_restore(lw->app->db, g_array_index(ids, gint64, i));
        on_app_status(lw->app, "Restored %u note%s",
                      ids->len, ids->len == 1 ? "" : "s");
        refresh_all(lw);             /* counts + the Trash section          */
    }
    g_array_free(ids, TRUE);
}

/* on_note_pin() — "win.note-pin(b)": pin or unpin every selected note; the
 * target is the new state (the opposite of the clicked note's).             */
static void
on_note_pin(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    OnLibrary *lw = user_data;       /* owning library window               */
    gboolean pin = g_variant_get_boolean(param);   /* state for the set    */

    GArray *ids = selected_note_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        on_db_note_set_pinned(lw->app->db,
                              g_array_index(ids, gint64, i), pin);
    g_array_free(ids, TRUE);

    refresh_all(lw);                 /* the Pinned Notes count/section, and
                                        the pinned view may be showing      */
}

/* ---------------------------------------------------------------------------
 * show_note_context_menu() — build and pop up the per-note menu.
 *   lw      — the library window.
 *   attach  — the notes view the press landed in (the popover's parent).
 *   note_id — the note that was right-clicked.
 *   x, y    — the press, in `attach`'s coordinates.
 * ------------------------------------------------------------------------- */
static void
show_note_context_menu(OnLibrary *lw, GtkWidget *attach, gint64 note_id,
                       gdouble x, gdouble y)
{
    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();
    GMenuItem *item;                 /* the two items that carry a target   */

    /* Open acts on the CLICKED note, whatever the selection.               */
    item = g_menu_item_new("_Open", NULL);
    g_menu_item_set_action_and_target(item, "win.note-open", "x", note_id);
    g_menu_append_item(section, item);
    g_object_unref(item);

    if (in_trash_view(lw)) {
        /* The Trash view's restore/purge menu.                             */
        g_menu_append(section, "_Restore", "win.note-restore");
        menu_section_end(menu, &section);
        g_menu_append(section, "Delete _Permanently", "win.delete-note");
    } else {
        /* Pin/Unpin offers the opposite of the clicked note's state.       */
        OnNoteMeta *meta = on_db_note_get(lw->app->db, note_id);
        gboolean pinned = meta != NULL && meta->pinned;
        on_db_note_meta_free(meta);
        item = g_menu_item_new(pinned ? "Un_pin" : "_Pin", NULL);
        g_menu_item_set_action_and_target(item, "win.note-pin", "b",
                                          !pinned);
        g_menu_append_item(section, item);
        g_object_unref(item);
        menu_section_end(menu, &section);

        g_menu_append(section, "Export as _HTML\xe2\x80\xa6",
                      "win.note-export-html");
        g_menu_append(section, "Export as _Markdown\xe2\x80\xa6",
                      "win.note-export-md");
        menu_section_end(menu, &section);
        g_menu_append(section, "Move to _Trash", "win.delete-note");
    }
    menu_section_end(menu, &section);
    g_object_unref(section);

    on_app_menu_popup(attach, G_MENU_MODEL(menu), x, y);
}

/* ---------------------------------------------------------------------------
 * on_note_pressed() — right click on a row of either notes view: select
 * the row under the pointer (an existing multi-selection is kept when
 * clicked inside) and show the note menu; the press is claimed so the
 * row's own gesture never sees it (it would collapse the selection).
 *   x, y — the press, in the row widget's coordinates.
 * ------------------------------------------------------------------------- */
static void
on_note_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                gpointer user_data)
{
    (void)n_press;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkListItem *item = row_item(g);
    OnNoteRow *row = gtk_list_item_get_item(item);
    if (row == NULL)
        return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);

    /* Right-clicking inside an existing multi-selection keeps it (so bulk
     * actions can target it); clicking elsewhere selects just that row.    */
    guint pos = gtk_list_item_get_position(item);
    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(lw->notes_sel),
                                         pos))
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(lw->notes_sel),
                                        pos, TRUE);
    show_note_context_menu(
        lw, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
        row->id, x, y);
}

/* ===========================================================================
 * status bar
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * status_path_update() — put the current selection's location in the
 * status bar's left label: "/" for the root, "/Folder/Sub" for folders,
 * and the raw sidebar name ("#tag", "Pinned Notes") for the non-path
 * views.  Runs from refresh_notes(), so it tracks every navigation.
 * ------------------------------------------------------------------------- */
static void
status_path_update(OnLibrary *lw)
{
    if (lw->status_path == NULL)
        return;

    gchar *text;                     /* the location part                   */
    if (lw->sel_kind == SB_KIND_TAG || lw->sel_kind == SB_KIND_PINNED ||
        lw->sel_kind == SB_KIND_ALL || lw->sel_kind == SB_KIND_TRASH ||
        lw->sel_kind == SB_KIND_ACTIONS) {
        text = g_strdup(lw->sel_name != NULL ? lw->sel_name : "");
    } else if (lw->sel_kind == SB_KIND_TRASH_FOLDER) {
        text = g_strdup_printf("Trash/%s",
                               lw->sel_name != NULL ? lw->sel_name : "");
    } else {
        gchar *path = on_db_folder_path(lw->app->db, lw->sel_id);
        text = g_strdup_printf("/%s", path);
        g_free(path);
    }

    gchar *full = on_app_location_text(lw->app, text);
    gtk_label_set_text(GTK_LABEL(lw->status_path), full);
    g_free(full);
    g_free(text);
}

/* on_notes_selection_status() — selection changed in either notes view:
 * post "N files selected" as a transient event message (right label,
 * fades like any other).  Nothing is posted when the selection empties,
 * and refresh_notes() repopulation (which clears the selection row by
 * row) is skipped via the populating guard.                                 */
static void
on_notes_selection_status(GtkSelectionModel *model, guint position,
                          guint n_items, gpointer user_data)
{
    (void)model; (void)position; (void)n_items;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating != 0)
        return;
    GArray *sel = selected_note_ids(lw);
    if (sel->len > 0)
        on_app_status(lw->app, "%u file%s selected",
                      sel->len, sel->len == 1 ? "" : "s");
    g_array_free(sel, TRUE);
}

/* status_fade_timeout() — the display period ended: fade the event
 * message out (the revealer crossfades to nothing).                         */
static gboolean
status_fade_timeout(gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->status_timeout = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_revealer),
                                  FALSE);
    return G_SOURCE_REMOVE;
}

/* lw_from_app() — the library state stashed on the library window, or
 * NULL when the window (or the state on it) doesn't exist.                  */
static OnLibrary *
lw_from_app(OnApp *app)
{
    return (app->library_window != NULL)
        ? g_object_get_data(G_OBJECT(app->library_window), "on-library")
        : NULL;
}

/* ---------------------------------------------------------------------------
 * library_notify_status() — installed as app->notify_status; shows an
 * event message in the status bar's right label, fading it out after
 * STATUS_FADE_SECONDS (each new message restarts the clock).  Post
 * through on_app_status(), never directly.
 * ------------------------------------------------------------------------- */
static void
library_notify_status(OnApp *app, const gchar *message)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL || lw->status_event == NULL)
        return;

    gtk_label_set_text(GTK_LABEL(lw->status_event), message);
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_revealer), TRUE);

    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    lw->status_timeout = g_timeout_add_seconds(STATUS_FADE_SECONDS,
                                               status_fade_timeout, lw);
}

/* ===========================================================================
 * AI summary
 * =========================================================================== */

/* AiJobData — context passed to the subprocess completion callback.         */
typedef struct {
    OnLibrary    *lw;
    GCancellable *cancellable;   /* owned ref; unrefed in the callback       */
} AiJobData;

/* ai_throbber_tick() — g_timeout_add callback: cycle the status-bar dots
 * animation while an AI request is in flight.                                */
static gboolean
ai_throbber_tick(gpointer user_data)
{
    static const gchar * const frames[] = {
        "\xe2\xa3\xbe", "\xe2\xa3\xbd", "\xe2\xa3\xbb", "\xe2\xa2\xbf",
        "\xe2\xa1\xbf", "\xe2\xa3\x9f", "\xe2\xa3\xaf", "\xe2\xa3\xb7"
    };
    OnLibrary *lw = user_data;
    on_app_status(lw->app, "Generating AI Summary %s",
                  frames[lw->ai_throbber_step % 8]);
    lw->ai_throbber_step++;
    return G_SOURCE_CONTINUE;
}

/* ai_throbber_stop() — cancel the status-bar throbber if running.           */
static void
ai_throbber_stop(OnLibrary *lw)
{
    if (lw->ai_throbber_id) {
        g_source_remove(lw->ai_throbber_id);
        lw->ai_throbber_id = 0;
    }
}

/* on_ai_subprocess_done() — GAsyncReadyCallback: the AI subprocess has
 * exited; read its stdout and put it in the text view.                       */
static void
on_ai_subprocess_done(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    AiJobData    *job       = user_data;
    OnLibrary    *lw        = job->lw;
    gboolean      cancelled = g_cancellable_is_cancelled(job->cancellable);
    g_object_unref(job->cancellable);
    g_free(job);

    GBytes  *out   = NULL;             /* stdout bytes from the subprocess    */
    GError  *error = NULL;
    g_subprocess_communicate_finish(G_SUBPROCESS(source),
                                    result, &out, NULL, &error);

    /* When the library window was destroyed while the subprocess was
     * running, library_free() cancelled the GCancellable and freed lw.
     * Clean up the result and return without touching lw.                  */
    if (cancelled) {
        if (out   != NULL) g_bytes_unref(out);
        if (error != NULL) g_error_free(error);
        return;
    }

    lw->ai_running = FALSE;
    ai_throbber_stop(lw);
    g_clear_object(&lw->ai_cancel);
    on_app_status(lw->app, "Summary generation complete");

    GtkTextBuffer *buf =               /* the pane's text buffer              */
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));

    if (error != NULL) {
        gtk_text_buffer_set_text(buf, error->message, -1);
        g_error_free(error);
    } else if (out != NULL) {
        gsize        len  = 0;
        const gchar *data = g_bytes_get_data(out, &len);
        gtk_text_buffer_set_text(buf, data != NULL ? data : "", (gint)len);
        g_bytes_unref(out);
    } else {
        gtk_text_buffer_set_text(buf, "(no output)", -1);
    }

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(lw->ai_text),
                                 &start, 0.0, FALSE, 0.0, 0.0);
}

/* run_ai_summary() — collect note content for the current selection, build
 * the prompt (project or normal mode), spawn the configured AI command,
 * and show the output asynchronously.                                        */
/* ai_fail() — show msg in the AI pane, stop the throbber.  Call before
 * any resources are allocated so the caller can return immediately.         */
static void
ai_fail(GtkTextBuffer *buf, OnLibrary *lw, const gchar *msg)
{
    gtk_text_buffer_set_text(buf, msg, -1);
    ai_throbber_stop(lw);
}

static void
run_ai_summary(OnLibrary *lw)
{
    GtkTextBuffer *buf =               /* the pane's text buffer              */
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));

    if (lw->app->ai_command == NULL || *lw->app->ai_command == '\0') {
        ai_fail(buf, lw, "No AI command configured. Please set one in "
                         "File \xe2\x86\x92 Settings \xe2\x86\x92 AI Features.");
        return;
    }

    /* Reject views where summarization doesn't make sense before loading
     * any notes — keeps all early exits before the expensive allocations.   */
    if (lw->sel_kind == SB_KIND_TRASH ||
        lw->sel_kind == SB_KIND_TRASH_FOLDER ||
        lw->sel_kind == SB_KIND_ACTIONS) {
        ai_fail(buf, lw, "Select notes to summarize.");
        return;
    }

    /* Validate the per-folder AI mode before allocating anything.           */
    gint ai_mode = (lw->sel_kind == SB_KIND_FOLDER)
        ? on_db_folder_get_ai_mode(lw->app->db, lw->sel_id)
        : ON_AI_MODE_NORMAL;
    if (ai_mode == ON_AI_MODE_CUSTOM &&
            (lw->app->ai_custom_prompt == NULL ||
             *lw->app->ai_custom_prompt == '\0')) {
        ai_fail(buf, lw, "No custom AI prompt configured. Set one in "
                         "File \xe2\x86\x92 Settings \xe2\x86\x92 AI Features.");
        return;
    }

    GArray *sel_ids = selected_note_ids(lw);
    if (sel_ids->len == 0) {
        g_array_free(sel_ids, TRUE);
        ai_fail(buf, lw, "Select one or more notes to summarize.");
        return;
    }

    GList *notes = NULL;
    for (guint i = 0; i < sel_ids->len; i++) {
        gint64     id = g_array_index(sel_ids, gint64, i);
        OnNoteMeta *m = on_db_note_get(lw->app->db, id);
        if (m != NULL)
            notes = g_list_prepend(notes, m);
    }
    notes = g_list_reverse(notes);
    g_array_free(sel_ids, TRUE);

    if (notes == NULL) {
        ai_fail(buf, lw, "No notes to summarize.");
        return;
    }

    GString *prompt = g_string_new(NULL);

    /* Build the prompt header based on the folder's AI mode.               */
    if (ai_mode == ON_AI_MODE_PROJECT) {
        g_string_append(prompt,
            "This series of notes chronologically details the progress of a "
            "project. Provide me a summary of them all especially focusing on "
            "the current state and any remaining action items.\n\n");
    } else if (ai_mode == ON_AI_MODE_CUSTOM) {
        g_string_append(prompt, lw->app->ai_custom_prompt);
        g_string_append(prompt, "\n\n");
    } else {
        g_string_append(prompt,
            "Provide me a brief summary of these notes.\n\n");
    }

    g_string_append(prompt,
        "Respond in plain text only. Do not use markdown formatting "
        "such as headers, bold, italics, or bullet symbols.\n\n");
    g_string_append(prompt, "=== Notes ===\n\n");

    for (GList *nl = notes; nl != NULL; nl = nl->next) {
        OnNoteMeta *m  = nl->data;     /* one note's metadata                 */
        GDateTime  *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar      *mod = g_date_time_format(dt, "%Y-%m-%d");
        g_date_time_unref(dt);

        g_string_append_printf(prompt, "--- %s (modified %s) ---\n",
                               m->title != NULL ? m->title : "(untitled)",
                               mod);
        g_free(mod);

        /* Fetch body text for just this note — avoids loading the whole DB. */
        gchar *body = on_db_note_body_text(lw->app->db, m->id);
        if (body != NULL && *body != '\0')
            g_string_append_printf(prompt, "%s\n", body);
        else
            g_string_append(prompt, "(empty note)\n");
        g_free(body);

        /* Action items for just this note.                                  */
        GList   *note_actions = on_db_action_list_for_note(lw->app->db, m->id);
        gboolean first_action = TRUE;
        for (GList *al = note_actions; al != NULL; al = al->next) {
            OnActionItem *it = al->data;
            if (first_action) {
                g_string_append(prompt, "\nAction items:\n");
                first_action = FALSE;
            }
            const gchar *status = it->done ? "[done]" : "[    ]";
            g_string_append_printf(prompt, "  %s %s", status, it->text);
            if (it->due != 0) {
                GDateTime *ddt = g_date_time_new_from_unix_local(it->due);
                gchar     *ds  = g_date_time_format(ddt, "%Y-%m-%d");
                g_date_time_unref(ddt);
                g_string_append_printf(prompt, " (due %s)", ds);
                g_free(ds);
            }
            g_string_append_c(prompt, '\n');
        }
        on_db_action_list_free(note_actions);
        g_string_append_c(prompt, '\n');
    }

    on_db_note_list_free(notes);

    gint    argc_ai = 0;
    gchar **argv_ai = NULL;
    GError *error   = NULL;
    if (!g_shell_parse_argv(lw->app->ai_command, &argc_ai, &argv_ai, &error)) {
        gchar *msg = g_strdup_printf("Invalid AI command: %s", error->message);
        gtk_text_buffer_set_text(buf, msg, -1);
        g_free(msg);
        g_error_free(error);
        g_string_free(prompt, TRUE);
        return;
    }
    (void)argc_ai;

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE  |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_MERGE);
    GSubprocess *proc = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)argv_ai, &error);
    g_strfreev(argv_ai);
    g_object_unref(launcher);

    if (proc == NULL) {
        gchar *msg = g_strdup_printf("Failed to start AI command: %s",
                                     error->message);
        g_error_free(error);
        g_string_free(prompt, TRUE);
        gtk_text_buffer_set_text(buf, msg, -1);
        g_free(msg);
        ai_throbber_stop(lw);
        return;
    }

    lw->ai_running = TRUE;

    gsize  plen        = prompt->len;
    gchar *pstr        = g_string_free(prompt, FALSE);
    GBytes *stdin_bytes = g_bytes_new_take(pstr, plen);

    AiJobData *job        = g_new(AiJobData, 1);
    job->lw               = lw;
    job->cancellable      = g_cancellable_new();
    lw->ai_cancel         = g_object_ref(job->cancellable);

    g_subprocess_communicate_async(proc, stdin_bytes, job->cancellable,
                                   on_ai_subprocess_done, job);
    g_bytes_unref(stdin_bytes);
    g_object_unref(proc);
}

/* on_ai_copy_clicked() — copy the AI summary text to the clipboard.         */
static void
on_ai_copy_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    OnLibrary     *lw  = user_data;
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(lw->ai_text), text);
    g_free(text);
}

/* on_ai_close_clicked() — the pane's ✕ button: hide the pane.               */
static void
on_ai_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    gtk_widget_set_visible(GTK_WIDGET(user_data), FALSE);
}

/* on_ai_button_clicked() — toolbar AI button: show the pane and (re)run the
 * summary; re-clicking while a run is in progress is a no-op.               */
static void
on_ai_button_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    OnLibrary *lw = user_data;         /* owning library window               */

    if (lw->ai_running)
        return;

    /* Set a comfortable initial split the first time the pane opens.         */
    if (!gtk_widget_get_visible(lw->ai_pane)) {
        gint total = gtk_widget_get_height(GTK_WIDGET(lw->notes_paned));
        if (total > 300)
            gtk_paned_set_position(GTK_PANED(lw->notes_paned),
                                   total - 220);
    }
    gtk_widget_set_visible(lw->ai_pane, TRUE);

    /* Clear text and start the status-bar throbber for immediate feedback.   */
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));
    gtk_text_buffer_set_text(buf, "", -1);
    lw->ai_throbber_step = 0;
    on_app_status(lw->app, "Generating AI Summary \xe2\xa3\xbe");
    lw->ai_throbber_id = g_timeout_add(120, ai_throbber_tick, lw);

    run_ai_summary(lw);
}

/* build_ai_pane() — construct the AI output pane (hidden by default): a
 * header row with a copy button and close button, then a scrolled
 * non-editable text view.  lw->ai_text is set here.  The pane's
 * visibility belongs to the AI button and its ✕ from here on.              */
static GtkWidget *
build_ai_pane(OnLibrary *lw)
{
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_box_append(GTK_BOX(pane),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 2);
    gtk_widget_set_margin_top(header, 1);
    gtk_widget_set_margin_bottom(header, 1);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<small><b>AI Summary</b></small>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(header), title);

    /* The expanding title pushes both buttons to the right edge: Copy, then
     * close (✕) at the far right.  Both compact (library_install_css).       */
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_button_set_has_frame(GTK_BUTTON(copy_btn), FALSE);
    on_app_set_tooltip(copy_btn, "Copy summary to clipboard");
    gtk_widget_add_css_class(copy_btn, "notes-ai-button");
    g_signal_connect(copy_btn, "clicked",
                     G_CALLBACK(on_ai_copy_clicked), lw);
    gtk_box_append(GTK_BOX(header), copy_btn);

    GtkWidget *close_btn = gtk_button_new_with_label("\xe2\x9c\x95");
    gtk_button_set_has_frame(GTK_BUTTON(close_btn), FALSE);
    on_app_set_tooltip(close_btn, "Close AI summary");
    gtk_widget_add_css_class(close_btn, "notes-ai-button");
    g_signal_connect(close_btn, "clicked",
                     G_CALLBACK(on_ai_close_clicked), pane);
    gtk_box_append(GTK_BOX(header), close_btn);

    gtk_box_append(GTK_BOX(pane), header);

    lw->ai_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(lw->ai_text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(lw->ai_text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(lw->ai_text),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(lw->ai_text), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(lw->ai_text), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(lw->ai_text), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(lw->ai_text), 4);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), lw->ai_text);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(pane), scroll);

    gtk_widget_set_visible(pane, FALSE); /* hidden until AI is clicked      */
    return pane;
}

/* library_notify_ai_changed() — installed as app->notify_ai_changed;
 * shows or hides the AI toolbar button based on app->ai_enabled.            */
static void
library_notify_ai_changed(OnApp *app)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL || lw->ai_btn == NULL)
        return;
    gtk_widget_set_visible(lw->ai_btn, app->ai_enabled);
    if (!app->ai_enabled && lw->ai_pane != NULL)
        gtk_widget_set_visible(lw->ai_pane, FALSE);
}

/* ===========================================================================
 * refresh hook + construction
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * library_notify_notes_changed() — installed as app->notify_notes_changed;
 * editor windows call it after each save so titles, ordering and the tag
 * sidebar stay current.
 * ------------------------------------------------------------------------- */
static void
library_notify_notes_changed(OnApp *app)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw != NULL)
        refresh_all(lw);
}

/* ---------------------------------------------------------------------------
 * library_notify_note_saved() — installed as app->notify_note_saved: update
 * the ONE row the saved note owns, instead of rebuilding the notes pane.
 *
 * A save can change a note's title, its modified time and its preview line —
 * never WHICH notes are listed.  Membership in every view the light path can
 * be reached from is decided by things a save leaves alone: the folder tree,
 * the pinned flag, the trashed flag, and the tag set (a tag change takes the
 * full notify instead).  So the model, the selection and the scroll position
 * can all stay exactly as they are.
 *
 * That matters because this fires on every autosave — roughly every 1.2 s of
 * continuous typing.  The old full repopulate re-queried the note list,
 * re-formatted two timestamps and re-measured Pango widths for every row,
 * then walked the model again to restore the selection: ~93 ms of per-row
 * work at 1300 notes, for one changed row.
 * ------------------------------------------------------------------------- */
static void
library_notify_note_saved(OnApp *app, gint64 note_id)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL)
        return;

    /* The Action Items view lists items, not notes, and orders them by the
     * owning note's modified time — a save can reshuffle it, so that view
     * still takes the full repopulate (it is not the typing-hot case).      */
    if (lw->sel_kind == SB_KIND_ACTIONS) {
        refresh_notes(lw);
        return;
    }

    /* Find the row.  Absent means the note is not in the current view, and
     * a save cannot have changed that — nothing to do.                     */
    OnNoteRow *row = NULL;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(lw->notes_store));
    for (guint i = 0; i < n && row == NULL; i++) {
        OnNoteRow *r = g_list_model_get_item(G_LIST_MODEL(lw->notes_store), i);
        if (r->id == note_id)
            row = r;                 /* keep the ref until the update       */
        else
            g_object_unref(r);
    }
    if (row == NULL)
        return;

    OnNoteMeta *m = on_db_note_get(lw->app->db, note_id);
    if (m == NULL) {
        g_object_unref(row);
        return;
    }

    g_free(row->title);
    row->title = g_strdup(m->title);
    GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
    g_free(row->modified);
    row->modified   = g_date_time_format(dt, LIST_TIME_FORMAT);
    row->updated_at = m->updated_at;
    g_date_time_unref(dt);

    /* Preview only where it is shown (Comfortable density, list view).      */
    g_clear_pointer(&row->preview, g_free);
    if (lw->app->comfortable_list) {
        gchar *body = on_db_note_body_text(lw->app->db, note_id);
        row->preview = notes_preview_line(body);
        g_free(body);
    }

    /* Touching the row is what re-sorts it to the top under the default
     * Modified ordering (the sort model re-sorts the changed item) and
     * rebinds it in both views; the selection follows it by identity.
     * populating stays DOWN: this is a real content change and the
     * selection handlers should see it.                                    */
    on_row_touch(lw->notes_store, row);

    /* The grid's thumbnail for this note is now stale; re-render it in idle
     * time exactly as a repopulate would have (list mode pays nothing).     */
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0)
        thumb_queue(lw, row, m->updated_at);

    g_object_unref(row);
    on_db_note_meta_free(m);
}

void
on_library_apply_native_menubar(OnApp *app, gboolean native)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL)
        return;
#ifdef __APPLE__
    /* GTK's quartz backend exports the application menubar to the native
     * macOS bar (and builds the app menu — About/Preferences/Quit — from
     * our "app." actions) whenever one is set.  So "native" = set it and
     * hide the in-window rendering; "not native" = unset it and show the
     * in-window GtkPopoverMenuBar over the same model.                     */
    gtk_application_set_menubar(app->gtk_app,
                                native ? lw->menubar_model : NULL);
    gtk_widget_set_visible(lw->menubar, !native);
#else
    /* No shell menubar on Linux: the GtkApplicationWindow renders the
     * application menubar itself, so it is always set and the setting has
     * nothing to choose between.                                           */
    (void)native;
    gtk_application_set_menubar(app->gtk_app, lw->menubar_model);
#endif
}

void
on_library_get_scope(OnApp *app, OnSearchScope *scope, gint64 *id,
                     gchar **name)
{
    *scope = ON_SCOPE_FOLDER;
    *id    = 0;
    *name  = g_strdup("Notes");

    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL)
        return;

    /* Only tags and folder-like selections (including a trashed folder
     * browsed from the Trash section) make a meaningful scope; All Notes,
     * Trash and Pinned keep the "everything" default above.                 */
    if (lw->sel_kind != SB_KIND_TAG &&
        lw->sel_kind != SB_KIND_FOLDER &&
        lw->sel_kind != SB_KIND_TRASH_FOLDER &&
        lw->sel_kind != SB_KIND_ROOT)
        return;

    *scope = (lw->sel_kind == SB_KIND_TAG) ? ON_SCOPE_TAG
                                           : ON_SCOPE_FOLDER;
    *id    = lw->sel_id;
    if (lw->sel_name != NULL) {
        g_free(*name);
        *name = g_strdup(lw->sel_name);
    }
}

/* ===========================================================================
 * sorting — one comparator per sortable column, for GtkCustomSorter.
 * The column view's own sorter (gtk_column_view_get_sorter) drives the
 * GtkSortListModel both notes views show, so a header click re-sorts the
 * grid as well.
 * =========================================================================== */

/* cmp_title() — case-insensitive alphabetical, the Title header.           */
static gint
cmp_title(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    return utf8_casecmp(((const OnNoteRow *)a)->title,
                        ((const OnNoteRow *)b)->title);
}

/* cmp_path() — the Path header; equal paths fall back to the title so
 * folders group cleanly.                                                    */
static gint
cmp_path(gconstpointer a, gconstpointer b, gpointer user_data)
{
    gint result = utf8_casecmp(((const OnNoteRow *)a)->path,
                               ((const OnNoteRow *)b)->path);
    return result != 0 ? result : cmp_title(a, b, user_data);
}

/* cmp_updated() / cmp_created() — the two time headers.  Deliberately
 * inverted so the FIRST click (ascending) shows the newest notes on top. */
static gint
cmp_updated(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    gint64 ta = ((const OnNoteRow *)a)->updated_at;
    gint64 tb = ((const OnNoteRow *)b)->updated_at;
    return (tb > ta) - (tb < ta);
}

static gint
cmp_created(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    gint64 ta = ((const OnNoteRow *)a)->created_at;
    gint64 tb = ((const OnNoteRow *)b)->created_at;
    return (tb > ta) - (tb < ta);
}

/* cmp_action_text() — alphabetical, the Action header.                     */
static gint
cmp_action_text(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    return utf8_casecmp(((const OnActionRow *)a)->text,
                        ((const OnActionRow *)b)->text);
}

/* cmp_action_done() — the checkbox header: open items first.               */
static gint
cmp_action_done(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    return (gint)((const OnActionRow *)a)->done -
           (gint)((const OnActionRow *)b)->done;
}

/* cmp_action_due() — Due Date header: the first click shows the soonest
 * deadline on top; items without a due date sort after every dated one.   */
static gint
cmp_action_due(gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    gint64 da = ((const OnActionRow *)a)->due_raw;
    gint64 db = ((const OnActionRow *)b)->due_raw;
    if (da == 0) da = G_MAXINT64;    /* undated: always last                */
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* ===========================================================================
 * column layout (order + visibility)
 *
 * Headers drag to reorder (GtkColumnView reorderable) and right-click for
 * a show/hide menu (each column's built-in header menu).  The layout
 * persists in the ini as "<cfgkey>=key:vis,key:vis,..." in display order;
 * each column carries its stable key as object data ("on-colkey") and
 * each VIEW carries its ini key ("on-colcfg") and its default layout
 * ("on-coldefault").  The machinery is shared by the notes list and the
 * Action Items list.  Column widths are GTK's: a column view sizes every
 * column to its content and gives the expanding one (Title / Action) the
 * rest — the tree view's autofit measuring pass is gone with it.
 * =========================================================================== */

/* col_key() — a column's stable key.                                        */
static const gchar *
col_key(GtkColumnViewColumn *col)
{
    return g_object_get_data(G_OBJECT(col), "on-colkey");
}

/* view_column_by_key() — the view's column carrying `key`, or NULL.         */
static GtkColumnViewColumn *
view_column_by_key(GtkColumnView *view, const gchar *key)
{
    GListModel *cols = gtk_column_view_get_columns(view);
    guint n = g_list_model_get_n_items(cols);
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        g_object_unref(c);           /* the view keeps it                   */
        if (g_strcmp0(col_key(c), key) == 0)
            return c;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * view_columns_persist() — write a view's current column order and
 * visibility to the ini.
 * ------------------------------------------------------------------------- */
static void
view_columns_persist(GtkColumnView *view)
{
    const gchar *cfg_key =           /* the view's ini key                  */
        g_object_get_data(G_OBJECT(view), "on-colcfg");
    if (cfg_key == NULL)
        return;
    GListModel *cols = gtk_column_view_get_columns(view);
    guint n = g_list_model_get_n_items(cols);
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        if (s->len > 0)
            g_string_append_c(s, ',');
        g_string_append_printf(s, "%s:%d", col_key(c),
                               gtk_column_view_column_get_visible(c) ? 1 : 0);
        g_object_unref(c);
    }
    on_app_config_set(cfg_key, s->str);
    g_string_free(s, TRUE);
}

/* The "column-<cfg>-<key>" actions: one stateful boolean per column of
 * each view, named after the view's ini key and the column's stable key.  */
#define COLUMN_ACTION_PREFIX "column-"

/* column_action_name() — "column-<cfg>-<key>" (g_free it).                  */
static gchar *
column_action_name(GtkColumnView *view, const gchar *key)
{
    return g_strconcat(COLUMN_ACTION_PREFIX,
                       (const gchar *)g_object_get_data(G_OBJECT(view),
                                                        "on-colcfg"),
                       "-", key, NULL);
}

/* ---------------------------------------------------------------------------
 * column_actions_sync() — bring a view's column actions up to date: state =
 * the column's visibility, enabled unless it is the only visible column
 * (so the view can't go empty).  Run after every visibility change.
 * ------------------------------------------------------------------------- */
static void
column_actions_sync(OnLibrary *lw, GtkColumnView *view)
{
    GActionMap *map = G_ACTION_MAP(lw->window);
    GListModel *cols = gtk_column_view_get_columns(view);
    guint n = g_list_model_get_n_items(cols);
    gint n_visible = 0;              /* how many columns are shown          */
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        if (gtk_column_view_column_get_visible(c))
            n_visible++;
        g_object_unref(c);
    }
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        gboolean visible = gtk_column_view_column_get_visible(c);
        gchar *name = column_action_name(view, col_key(c));
        GSimpleAction *action =
            G_SIMPLE_ACTION(g_action_map_lookup_action(map, name));
        if (action != NULL) {
            g_simple_action_set_state(action, g_variant_new_boolean(visible));
            g_simple_action_set_enabled(action, !(visible && n_visible == 1));
        }
        g_free(name);
        g_object_unref(c);
    }
}

/* on_columns_reordered() — a header drag moved a column: persist.          */
static void
on_columns_reordered(GListModel *cols, guint position, guint removed,
                     guint added, gpointer view)
{
    (void)cols; (void)position; (void)removed; (void)added;
    view_columns_persist(view);
}

/* view_by_cfg() — the view whose ini key is `cfg`.                          */
static GtkColumnView *
view_by_cfg(OnLibrary *lw, const gchar *cfg)
{
    if (g_strcmp0(g_object_get_data(G_OBJECT(lw->notes_list), "on-colcfg"),
                  cfg) == 0)
        return lw->notes_list;
    return lw->actions_view;
}

/* on_column_change_state() — a column's check item flipped: show/hide the
 * column and persist.  The action name is "column-<cfg>-<key>".            */
static void
on_column_change_state(GSimpleAction *action, GVariant *value,
                       gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    const gchar *rest =              /* "<cfg>-<key>"                       */
        g_action_get_name(G_ACTION(action)) + strlen(COLUMN_ACTION_PREFIX);
    const gchar *dash = strrchr(rest, '-');
    if (dash == NULL)
        return;
    gchar *cfg = g_strndup(rest, (gsize)(dash - rest));
    GtkColumnView *view = view_by_cfg(lw, cfg);
    g_free(cfg);
    GtkColumnViewColumn *c = view_column_by_key(view, dash + 1);
    if (c == NULL)
        return;
    g_simple_action_set_state(action, value);
    gtk_column_view_column_set_visible(c, g_variant_get_boolean(value));
    view_columns_persist(view);
    column_actions_sync(lw, view);
}

/* ---------------------------------------------------------------------------
 * view_columns_install() — the layout machinery for one view whose columns
 * are all appended: register its "column-<cfg>-<key>" actions, give every
 * column the same header menu (a check item per column), put the saved
 * order and visibility back (falling back to "on-coldefault"; unknown keys
 * are skipped, missing ones keep their built state, and at least one
 * column is forced visible so a hand-edited ini can't blank the view),
 * and from then on persist every header reorder.
 *   lw     — the library window.
 *   view   — the column view, "on-colcfg" and "on-coldefault" set.
 *   labels — the menu label per column key, for a titleless column.
 * ------------------------------------------------------------------------- */
static void
view_columns_install(OnLibrary *lw, GtkColumnView *view)
{
    GActionMap *map = G_ACTION_MAP(lw->window);
    GListModel *cols = gtk_column_view_get_columns(view);
    guint n = g_list_model_get_n_items(cols);

    GMenu *menu = g_menu_new();      /* the shared header menu              */
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        gchar *name = column_action_name(view, col_key(c));
        GSimpleAction *action = g_simple_action_new_stateful(
            name, NULL, g_variant_new_boolean(TRUE));
        g_signal_connect(action, "change-state",
                         G_CALLBACK(on_column_change_state), lw);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
        const gchar *label =         /* a titleless column (the checkbox
                                        one) still needs a menu label       */
            g_object_get_data(G_OBJECT(c), "on-collabel");
        if (label == NULL)
            label = gtk_column_view_column_get_title(c);
        gchar *detailed = g_strconcat("win.", name, NULL);
        g_menu_append(menu, label, detailed);
        g_free(detailed);
        g_free(name);
        g_object_unref(c);
    }
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        gtk_column_view_column_set_header_menu(c, G_MENU_MODEL(menu));
        g_object_unref(c);
    }
    g_object_unref(menu);

    /* The saved layout.                                                    */
    const gchar *cfg_key = g_object_get_data(G_OBJECT(view), "on-colcfg");
    gchar *cfg = on_app_config_get(cfg_key);
    if (cfg == NULL || *cfg == '\0') {
        g_free(cfg);
        cfg = g_strdup(g_object_get_data(G_OBJECT(view), "on-coldefault"));
    }
    guint at = 0;                    /* where the next listed column goes   */
    gchar **entries = g_strsplit(cfg, ",", -1);
    for (gsize i = 0; entries[i] != NULL; i++) {
        gchar **kv = g_strsplit(entries[i], ":", 2);
        GtkColumnViewColumn *c =
            kv[0] != NULL ? view_column_by_key(view, kv[0]) : NULL;
        if (c != NULL) {
            gtk_column_view_insert_column(view, at++, c);
            gtk_column_view_column_set_visible(
                c, kv[1] == NULL || g_strcmp0(kv[1], "0") != 0);
        }
        g_strfreev(kv);
    }
    g_strfreev(entries);
    g_free(cfg);

    gboolean any_visible = FALSE;    /* is at least one column shown?       */
    for (guint i = 0; i < n; i++) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, i);
        any_visible |= gtk_column_view_column_get_visible(c);
        g_object_unref(c);
    }
    if (!any_visible && n > 0) {
        GtkColumnViewColumn *c = g_list_model_get_item(cols, 0);
        gtk_column_view_column_set_visible(c, TRUE);
        g_object_unref(c);
    }
    column_actions_sync(lw, view);

    /* Connected only now, so applying the saved layout doesn't re-persist
     * it; from here on every header drag writes the ini.                  */
    g_signal_connect(cols, "items-changed", G_CALLBACK(on_columns_reordered),
                     view);
}

/* ===========================================================================
 * cell factories — what one row of each view looks like.
 *
 * "setup" builds the widget once per recycled row and installs the row's
 * controllers, stashing the GtkListItem on each ("on-item") so a handler
 * can read whichever row the widget shows at the time; "bind" fills it
 * from the item.  Nothing is unbound explicitly: every bind overwrites.
 * =========================================================================== */

/* note_cell_controllers() — what every cell of a note row gets: the drag
 * source for note drops and the right-click menu.  Per CELL because a
 * column view's row widget is not reachable from a factory; the effect is
 * the same, a press anywhere on the row.                                  */
static void
note_cell_controllers(OnLibrary *lw, GtkWidget *widget, GtkListItem *item)
{
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_object_set_data(G_OBJECT(drag), "on-item", item);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_note_drag_prepare), lw);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
    row_click_gesture(widget, item, G_CALLBACK(on_note_pressed), lw);
    /* Double-click opens the note — counted here (on_app_double_click_watch,
     * D34); the view's own "activate" still serves Enter.                 */
    g_object_set_data(G_OBJECT(widget), "on-item", item);
    on_app_double_click_watch(widget, on_note_double_clicked, lw);
    on_app_select_on_press(widget, item);                       /* D37 */
}

/* cell_label_new() — a left-aligned cell label with the column's padding. */
static GtkWidget *
cell_label_new(gboolean ellipsize)
{
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    if (ellipsize)
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(label, 10);
    gtk_widget_set_margin_end(label, 10);
    return label;
}

/* --- notes list: Title ------------------------------------------------------*/

/* on_title_setup() — a title label over a small preview label.             */
static void
on_title_setup(GtkListItemFactory *f, GtkListItem *item, gpointer user_data)
{
    (void)f;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    GtkWidget *title = cell_label_new(TRUE);
    gtk_widget_set_margin_start(title, 0);
    gtk_widget_set_margin_end(title, 0);
    GtkWidget *preview = cell_label_new(TRUE);
    gtk_widget_set_margin_start(preview, 0);
    gtk_widget_set_margin_end(preview, 0);
    gtk_widget_add_css_class(preview, "notes-preview");
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), preview);
    gtk_list_item_set_child(item, box);
    note_cell_controllers(user_data, box, item);
}

/* on_title_bind() — the density-dependent rendering:
 *   Compact      — plain title, no preview line, minimal padding.
 *   Comfortable  — bold title with a small dimmed body-text preview under
 *                  it, generous padding.  The preview dims through a CSS
 *                  alpha (library_install_css) rather than a fixed colour,
 *                  so it stays readable on the selection highlight.        */
static void
on_title_bind(GtkListItemFactory *f, GtkListItem *item, gpointer user_data)
{
    (void)f;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnNoteRow *row = gtk_list_item_get_item(item);
    GtkWidget *box     = gtk_list_item_get_child(item);
    GtkWidget *title   = gtk_widget_get_first_child(box);
    GtkWidget *preview = gtk_widget_get_last_child(box);
    gboolean comfy = lw->app->comfortable_list;

    gchar *esc = on_markup_escape_emoji(
        row->title != NULL && *row->title != '\0' ? row->title
                                                  : (comfy ? "Untitled" : ""),
        lw->emoji_pad);
    gchar *markup = (comfy && lw->app->bold_list_titles)
        ? g_strdup_printf("<b>%s</b>", esc) : g_strdup(esc);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    g_free(esc);

    gboolean show_preview = comfy && row->preview != NULL &&
                            *row->preview != '\0';
    if (show_preview) {
        gchar *pesc = on_markup_escape_emoji(row->preview, lw->emoji_pad);
        gchar *pm = g_strdup_printf("<small>%s</small>", pesc);
        gtk_label_set_markup(GTK_LABEL(preview), pm);
        g_free(pm);
        g_free(pesc);
    }
    gtk_widget_set_visible(preview, show_preview);
    gtk_widget_set_margin_top(box, comfy ? 4 : 0);
    gtk_widget_set_margin_bottom(box, comfy ? 4 : 0);
}

/* --- notes list: Path / Modified / Created ---------------------------------*/

/* Which OnNoteRow string a plain text column shows (the factory's data). */
enum { NF_PATH, NF_MODIFIED, NF_CREATED };

/* on_note_text_setup() — a label; the row's controllers ride on it too.   */
static void
on_note_text_setup(GtkListItemFactory *f, GtkListItem *item,
                   gpointer user_data)
{
    (void)f;
    OnLibrary *lw = g_object_get_data(G_OBJECT(f), "on-lw");
    gint field = GPOINTER_TO_INT(user_data);
    GtkWidget *label = cell_label_new(field == NF_PATH);
    gtk_list_item_set_child(item, label);
    note_cell_controllers(lw, label, item);
}

static void
on_note_text_bind(GtkListItemFactory *f, GtkListItem *item,
                  gpointer user_data)
{
    (void)f;
    OnNoteRow *row = gtk_list_item_get_item(item);
    const gchar *text = GPOINTER_TO_INT(user_data) == NF_PATH     ? row->path
                      : GPOINTER_TO_INT(user_data) == NF_MODIFIED ? row->modified
                                                                  : row->created;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), text);
}

/* --- notes grid ---------------------------------------------------------------*/

/* on_grid_setup() — the thumbnail card with the title under it.            */
static void
on_grid_setup(GtkListItemFactory *f, GtkListItem *item, gpointer user_data)
{
    (void)f;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box, "notes-card");
    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(pic, THUMB_SIZE, THUMB_SIZE);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
    gtk_widget_set_size_request(label, THUMB_SIZE, -1);
    gtk_box_append(GTK_BOX(box), pic);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(item, box);
    note_cell_controllers(user_data, box, item);
}

static void
on_grid_bind(GtkListItemFactory *f, GtkListItem *item, gpointer user_data)
{
    (void)f;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnNoteRow *row = gtk_list_item_get_item(item);
    GtkWidget *box   = gtk_list_item_get_child(item);
    GtkWidget *pic   = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);
    gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(row->thumb));
    gchar *esc = on_markup_escape_emoji(row->title, lw->emoji_pad);
    gchar *markup = g_strdup_printf("<b>%s</b>", esc);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(esc);
}

/* --- Action Items ---------------------------------------------------------------*/

/* on_action_done_setup() — the checkbox; its toggles reach
 * on_action_toggled with the list item.                                   */
static void
on_action_done_setup(GtkListItemFactory *f, GtkListItem *item,
                     gpointer user_data)
{
    (void)f;
    GtkWidget *check = gtk_check_button_new();
    gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(check), "on-item", item);
    g_signal_connect(check, "toggled", G_CALLBACK(on_action_toggled),
                     user_data);
    gtk_list_item_set_child(item, check);
}

static void
on_action_done_bind(GtkListItemFactory *f, GtkListItem *item,
                    gpointer user_data)
{
    (void)f; (void)user_data;
    OnActionRow *row = gtk_list_item_get_item(item);
    g_object_set_data(G_OBJECT(item), "on-binding", GINT_TO_POINTER(1));
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(gtk_list_item_get_child(item)), row->done);
    g_object_set_data(G_OBJECT(item), "on-binding", NULL);
}

/* on_action_text_setup() — the item text.                                  */
static void
on_action_text_setup(GtkListItemFactory *f, GtkListItem *item,
                     gpointer user_data)
{
    (void)f;
    GtkWidget *label = cell_label_new(TRUE);
    g_object_set_data(G_OBJECT(label), "on-item", item);
    on_app_double_click_watch(label, on_action_cell_double_clicked,
                              user_data);
    on_app_select_on_press(label, item);                        /* D37 */
    gtk_list_item_set_child(item, label);
}

/* strike_attrs() — a strikethrough attribute list, or NULL for none.       */
static PangoAttrList *
strike_attrs(gboolean done)
{
    if (!done)
        return NULL;
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
    return al;
}

static void
on_action_text_bind(GtkListItemFactory *f, GtkListItem *item,
                    gpointer user_data)
{
    (void)f;
    OnLibrary *lw = user_data;       /* owning library window               */
    OnActionRow *row = gtk_list_item_get_item(item);
    GtkLabel *label = GTK_LABEL(gtk_list_item_get_child(item));
    gchar *markup = on_markup_escape_emoji(row->text, lw->emoji_pad);
    gtk_label_set_markup(label, markup);
    g_free(markup);
    PangoAttrList *al = strike_attrs(row->done);
    gtk_label_set_attributes(label, al);
    if (al != NULL)
        pango_attr_list_unref(al);
}

/* The three urgency classes of a Due Date cell (library_install_css).     */
static const gchar *const DUE_CLASS[] = {
    "due-overdue", "due-today", "due-ahead",
};

/* on_action_due_setup() — the due date; a double-click opens the calendar. */
static void
on_action_due_setup(GtkListItemFactory *f, GtkListItem *item,
                    gpointer user_data)
{
    (void)f;
    GtkWidget *label = cell_label_new(FALSE);
    g_object_set_data(G_OBJECT(label), "on-item", item);
    on_app_double_click_watch(label, on_due_cell_double_clicked, user_data);
    on_app_select_on_press(label, item);                        /* D37 */
    gtk_list_item_set_child(item, label);
}

/* on_action_due_bind() — the text, struck when done, tinted by urgency:
 * already passed = red, today = yellow, still ahead = green.  Calendar
 * DAYS are compared in local time (the stored value is already local
 * midnight, but day-level compare keeps this DST-proof).                  */
static void
on_action_due_bind(GtkListItemFactory *f, GtkListItem *item,
                   gpointer user_data)
{
    (void)f; (void)user_data;
    OnActionRow *row = gtk_list_item_get_item(item);
    GtkWidget *label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), row->due != NULL ? row->due : "");
    PangoAttrList *al = strike_attrs(row->done);
    gtk_label_set_attributes(GTK_LABEL(label), al);
    if (al != NULL)
        pango_attr_list_unref(al);

    for (gsize i = 0; i < G_N_ELEMENTS(DUE_CLASS); i++)
        gtk_widget_remove_css_class(label, DUE_CLASS[i]);
    if (row->due_raw == 0)
        return;
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt  = g_date_time_new_from_unix_local(row->due_raw);
    gint today = g_date_time_get_year(now) * 10000 +
                 g_date_time_get_month(now) * 100 +
                 g_date_time_get_day_of_month(now);
    gint day   = g_date_time_get_year(dt) * 10000 +
                 g_date_time_get_month(dt) * 100 +
                 g_date_time_get_day_of_month(dt);
    g_date_time_unref(now);
    g_date_time_unref(dt);
    gtk_widget_add_css_class(label, DUE_CLASS[day < today ? 0
                                              : day == today ? 1 : 2]);
}

/* --- sidebar ----------------------------------------------------------------------*/

/* on_sidebar_setup() — a tree expander (indent + arrow) around the name
 * label, with the row's drag source, drop target and right-click menu.   */
static void
on_sidebar_setup(GtkListItemFactory *f, GtkListItem *item,
                 gpointer user_data)
{
    (void)f;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkWidget *expander = gtk_tree_expander_new();
    gtk_tree_expander_set_indent_for_icon(GTK_TREE_EXPANDER(expander), FALSE);
    /* The same left inset the notes list's Title cell text has (its box
     * margin plus the theme's cell padding, 14 px), less the row's own
     * 4 px, so the two panes' text sits the same distance off its edge. */
    gtk_widget_set_margin_start(expander, 10);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    /* Ellipsizing names keeps the pane's MINIMUM width small: without it
     * the widest row dictates the minimum and the divider can't be
     * dragged past it.                                                     */
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    on_app_select_on_press(label, item);     /* the label, not the arrow  */
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
    gtk_list_item_set_child(item, expander);

    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_object_set_data(G_OBJECT(drag), "on-item", item);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_sidebar_drag_prepare),
                     lw);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(drag));

    GtkDropTarget *drop = gtk_drop_target_new(ON_TYPE_DRAG_ROWS,
                                              GDK_ACTION_MOVE);
    /* Preload: the content is read when the drag enters, so every motion
     * can validate against what is actually being dragged (a local drag
     * loads synchronously — see on_sidebar_drop_motion).                  */
    gtk_drop_target_set_preload(drop, TRUE);
    g_object_set_data(G_OBJECT(drop), "on-item", item);
    g_signal_connect(drop, "enter",  G_CALLBACK(on_sidebar_drop_motion), lw);
    g_signal_connect(drop, "motion", G_CALLBACK(on_sidebar_drop_motion), lw);
    g_signal_connect(drop, "leave",  G_CALLBACK(on_sidebar_drop_leave),  lw);
    g_signal_connect(drop, "drop",   G_CALLBACK(on_sidebar_drop),        lw);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(drop));

    row_click_gesture(expander, item, G_CALLBACK(on_sidebar_pressed), lw);
}

/* on_sidebar_bind() — the name (bold for section rows), and the tree row
 * for the expander.                                                        */
static void
on_sidebar_bind(GtkListItemFactory *f, GtkListItem *item, gpointer user_data)
{
    (void)f;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeListRow *tr = NULL;
    OnSbRow *r = sb_item_row(item, &tr);
    GtkWidget *expander = gtk_list_item_get_child(item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), tr);
    GtkWidget *label = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    gchar *esc = on_markup_escape_emoji(r->name, lw->emoji_pad);
    gchar *markup = sb_kind_is_section(r->kind)
        ? g_strdup_printf("<b>%s</b>", esc) : g_strdup(esc);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(esc);
}

/* ===========================================================================
 * actions
 *
 * Every command the library offers is a GAction, and menus, toolbar
 * buttons and keyboard shortcuts only NAME actions.  Two scopes:
 *
 *   "app." — what the menubar references.  It has to work from whichever
 *            window is focused (on macOS the native menubar is the only
 *            menubar), so these live on the GtkApplication and act on THE
 *            library window, creating it again if it was closed.
 *   "win." — what is only ever invoked from inside this window: toolbar
 *            buttons, context menus, and every keyboard shortcut (see
 *            on_app_install_accels — a key may mean something else in an
 *            editor window, which is what window scope is for).
 *
 * A command is a name plus the function that runs it for the library; the
 * three shortcuts that duplicate a menubar item (new-note, find, media)
 * simply bind the SAME function under a "win." name.
 * =========================================================================== */

typedef struct {
    const gchar *name;               /* action name, without the prefix     */
    void       (*run)(OnLibrary *lw);/* what it does                        */
} LibCommand;

static const LibCommand APP_COMMANDS[] = {
    { "new-note",       on_new_note        },
    { "new-folder",     on_new_folder      },
    { "export-html",    on_export_html     },
    { "export-md",      on_export_markdown },
    { "open-db",        on_open_db         },
    { "preferences",    on_open_settings   },  /* the macOS app menu's name */
    { "about",          on_about           },
    { "quit",           on_quit            },
    { "view-list",      on_view_list       },
    { "view-grid",      on_view_grid       },
    { "toggle-sidebar", on_toggle_sidebar  },
    { "sidebar-hide",   on_sidebar_hide    },
    { "sidebar-show",   on_sidebar_show    },
    { "done-hide",      on_done_hide       },
    { "done-show",      on_done_show       },
    { "media",          on_open_media      },
    { "search",         on_open_search     },
};

static const LibCommand WIN_COMMANDS[] = {
    /* shortcuts (see on_app_install_accels)                                */
    { "new-note",         on_new_note            },
    { "find",             on_open_search         },
    { "media",            on_open_media          },
    /* toolbar                                                              */
    { "delete-folder",    on_delete_folder       },
    { "quicknote",        on_quicknote           },
    { "delete-note",      on_delete_note         },
    { "toggle-view",      on_toggle_view         },
    { "toggle-done",      on_toggle_done         },
    /* sidebar context menu                                                 */
    { "folder-info",      on_rename_folder       },
    { "folder-restore",   on_restore_folder      },
    { "sort-subfolders",  on_sort_subfolders     },
    { "search-here",      on_sidebar_search_here },
    { "empty-trash",      on_empty_trash         },
    /* note context menu                                                    */
    { "note-export-html", on_note_export_html    },
    { "note-export-md",   on_note_export_md      },
    { "note-restore",     on_note_restore        },
};

/* command_run() — run the command `name` from `table` for `lw`.            */
static void
command_run(const LibCommand *table, gsize n, const gchar *name,
            OnLibrary *lw)
{
    for (gsize i = 0; i < n; i++) {
        if (g_strcmp0(table[i].name, name) == 0) {
            table[i].run(lw);
            return;
        }
    }
}

/* on_win_command() — "activate" of a "win." command: user_data is the
 * library the action lives on.                                              */
static void
on_win_command(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)param;
    command_run(WIN_COMMANDS, G_N_ELEMENTS(WIN_COMMANDS),
                g_action_get_name(G_ACTION(action)), user_data);
}

/* on_app_command() — "activate" of an "app." command: user_data is the
 * OnApp, since these outlive any one library window.  The library is
 * re-created if it has been closed (the native macOS menubar stays up
 * while editors are open), which is also what on_activate does.            */
static void
on_app_command(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)param;
    OnApp *app = user_data;          /* shared application context          */
    if (app->library_window == NULL)
        on_library_window_create(app);
    command_run(APP_COMMANDS, G_N_ELEMENTS(APP_COMMANDS),
                g_action_get_name(G_ACTION(action)), lw_from_app(app));
}

/* ---------------------------------------------------------------------------
 * commands_install() — add one table of parameterless actions to a map.
 *   map       — the GtkApplication or the library window.
 *   table / n — the commands.
 *   handler   — on_app_command or on_win_command.
 *   user_data — what that handler expects.
 * ------------------------------------------------------------------------- */
static void
commands_install(GActionMap *map, const LibCommand *table, gsize n,
                 GCallback handler, gpointer user_data)
{
    for (gsize i = 0; i < n; i++) {
        GSimpleAction *action = g_simple_action_new(table[i].name, NULL);
        g_signal_connect(action, "activate", handler, user_data);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
    }
}

/* ---------------------------------------------------------------------------
 * library_install_actions() — every action of the library: the "app."
 * commands on the application (once — a second library window in one
 * process reuses them), the "win." commands and the two parameterised
 * note actions on the window.  The per-column actions are added by
 * view_columns_install() when each column view is built.
 * ------------------------------------------------------------------------- */
static void
library_install_actions(OnLibrary *lw)
{
    GActionMap *app_map = G_ACTION_MAP(lw->app->gtk_app);
    GActionMap *win_map = G_ACTION_MAP(lw->window);

    if (g_action_map_lookup_action(app_map, APP_COMMANDS[0].name) == NULL)
        commands_install(app_map, APP_COMMANDS, G_N_ELEMENTS(APP_COMMANDS),
                         G_CALLBACK(on_app_command), lw->app);
    commands_install(win_map, WIN_COMMANDS, G_N_ELEMENTS(WIN_COMMANDS),
                     G_CALLBACK(on_win_command), lw);

    GSimpleAction *action;           /* the hand-made ones                  */
    action = g_simple_action_new("note-open", G_VARIANT_TYPE_INT64);
    g_signal_connect(action, "activate", G_CALLBACK(on_note_open), lw);
    g_action_map_add_action(win_map, G_ACTION(action));
    g_object_unref(action);

    action = g_simple_action_new("note-pin", G_VARIANT_TYPE_BOOLEAN);
    g_signal_connect(action, "activate", G_CALLBACK(on_note_pin), lw);
    g_action_map_add_action(win_map, G_ACTION(action));
    g_object_unref(action);
}

/* ---------------------------------------------------------------------------
 * build_menubar() — the File and View menus as a menu model, every item
 * naming an "app." action.  Rendered by GTK: in the native macOS menu bar,
 * or at the top of the GtkApplicationWindow where the shell has none (see
 * on_library_apply_native_menubar).
 * Returns the model (owned by the caller).
 * ------------------------------------------------------------------------- */
static GMenuModel *
build_menubar(void)
{
    GMenu *bar = g_menu_new();
    GMenu *menu;                     /* one top-level menu                  */
    GMenu *section;                  /* one group of its items              */

    /* File menu.
     *
     * ONE separator in this menu, and it goes after the group below.
     * What acts on the NOTES is New Note, New Folder and the two Export
     * All items; everything after the rule is about the app or the file it
     * keeps — the database, Settings, About, Quit.  A rule between every
     * pair of items (which is what this was) divides nothing, so it
     * stopped reading as grouping at all.  Same shape as the sister Tasks
     * app's File menu.  (On macOS, GTK also puts Settings, About and Quit
     * in the app menu it builds from the same three actions.)             */
    menu    = g_menu_new();
    section = g_menu_new();
    g_menu_append(section, "_New Note",                   "app.new-note");
    g_menu_append(section, "New _Folder\xe2\x80\xa6",          "app.new-folder");
    g_menu_append(section, "Export All as _HTML\xe2\x80\xa6",  "app.export-html");
    g_menu_append(section, "Export All as _Markdown\xe2\x80\xa6",
                  "app.export-md");
    menu_section_end(menu, &section);
    g_menu_append(section, "_Open Database File\xe2\x80\xa6",  "app.open-db");
    g_menu_append(section, "_Settings\xe2\x80\xa6",            "app.preferences");
    g_menu_append(section, "_About",                      "app.about");
    g_menu_append(section, "_Quit",                       "app.quit");
    menu_section_end(menu, &section);
    g_object_unref(section);
    g_menu_append_submenu(bar, "_File", G_MENU_MODEL(menu));
    g_object_unref(menu);

    /* View menu.  Above the rule is what the WINDOW looks like — the notes
     * pane's two modes, and whether the folder pane is up.  Show/Hide
     * Sidebar mirrors the toolbar's Folders button; both route through
     * sidebar_set_visible(), so the label cannot drift from the pane.  The
     * two sidebar items are both in the model; sidebar_menu_sync() decides
     * which one is on offer (see SIDEBAR_LABEL_TO_HIDE).
     * Below the rule are the two items that open a window of their own.    */
    menu    = g_menu_new();
    section = g_menu_new();
    g_menu_append(section, "Notes as _List",  "app.view-list");
    g_menu_append(section, "Notes as _Grid",  "app.view-grid");
    GMenuItem *item;                 /* the two hidden-when items           */
    item = g_menu_item_new(SIDEBAR_LABEL_TO_HIDE, "app.sidebar-hide");
    g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
    g_menu_append_item(section, item);
    g_object_unref(item);
    item = g_menu_item_new(SIDEBAR_LABEL_TO_SHOW, "app.sidebar-show");
    g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
    g_menu_append_item(section, item);
    g_object_unref(item);
    /* The completed-items twin of the toolbar toggle, the same two-item
     * device: done_menu_sync() enables the one that names what a click
     * will do.                                                            */
    item = g_menu_item_new("Hide _Completed", "app.done-hide");
    g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
    g_menu_append_item(section, item);
    g_object_unref(item);
    item = g_menu_item_new("Show _Completed", "app.done-show");
    g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
    g_menu_append_item(section, item);
    g_object_unref(item);
    menu_section_end(menu, &section);
    g_menu_append(section, "_Media\xe2\x80\xa6",         "app.media");
    g_menu_append(section, "_Search Notes\xe2\x80\xa6",  "app.search");
    menu_section_end(menu, &section);
    g_object_unref(section);
    g_menu_append_submenu(bar, "_View", G_MENU_MODEL(menu));
    g_object_unref(menu);

    return G_MENU_MODEL(bar);
}

/* ---------------------------------------------------------------------------
 * add_tool_button() — helper: append a toolbar button bound to an action.
 *   lw       — the library window.
 *   toolbar  — the toolbar box to append to.
 *   icon     — local icon file basename, or NULL.
 *   fallback — markup shown as the icon when the file is missing.
 *   label    — button text label.
 *   tooltip  — hover help.
 *   action   — detailed action name the click activates ("win.…"/"app.…").
 * Returns the button, for the callers that need to keep it (the List/Grid
 * toggle re-points its own icon); most ignore it.
 * ------------------------------------------------------------------------- */
static GtkWidget *
add_tool_button(OnLibrary *lw, GtkWidget *toolbar, const gchar *icon,
                const gchar *fallback, const gchar *label,
                const gchar *tooltip, const gchar *action)
{
    GtkWidget *button = on_app_tool_item_new(lw->app, FALSE, icon,
                                             fallback, label, tooltip);
    gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(button), action);
    gtk_box_append(GTK_BOX(toolbar), button);
    return button;
}

/* ---------------------------------------------------------------------------
 * build_action_bar() — the single unified toolbar spanning the window: a
 * GtkBox with the "toolbar" style class (GTK4 has no GtkToolbar) holding
 * a folder-actions area, a drawn separator, a note-actions area, another
 * separator, the window/app buttons (Sidebar, List/Grid, Search, Media,
 * Settings), a third separator, the AI Summary button (shown only while AI
 * is enabled) — and the search entry pinned to the right edge by an
 * expanding spacer.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_action_bar(OnLibrary *lw)
{
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(toolbar, "toolbar");

    /* --- folder area ---------------------------------------------------- */
    add_tool_button(lw, toolbar, "new-folder", "+\xf0\x9f\x93\x81",
                    "New Folder", "Create a folder inside the selection",
                    "app.new-folder");
    add_tool_button(lw, toolbar, "delete-folder", "\xe2\x9c\x95",
                    "Delete Folder",
                    "Move the selected folder to the Trash",
                    "win.delete-folder");
    /* Rename lives in the folder's right-click menu only.                  */

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* --- notes area ------------------------------------------------------*/
    add_tool_button(lw, toolbar, "archive", "\xe2\x9a\xa1", "Quicknote",
                    "Create a note in the root folder",
                    "win.quicknote");
    add_tool_button(lw, toolbar, "newnote", "+", "New Note",
                    "Create a note in the current folder",
                    "app.new-note");
    add_tool_button(lw, toolbar, "deletenote", "\xe2\x9c\x95",
                    "Delete Note",
                    "Move the selected notes to the Trash",
                    "win.delete-note");

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* --- app actions ------------------------------------------------------*/
    /* The two buttons that change what the WINDOW shows sit together, the
     * sidebar toggle first: the folder pane is not a folder action.        */
    add_tool_button(lw, toolbar, "sidebar", "\xe2\x97\xa7",
                    "Folders", "Show or hide the folder pane",
                    "app.toggle-sidebar");
    /* Icon, label and tooltip are all set by view_button_sync() below,
     * from the view actually showing; these are only what it is built
     * with before the stack can be read.                                   */
    lw->view_btn = add_tool_button(lw, toolbar, "grid", "\xe2\x8a\x9e",
                    "Grid", "Switch to grid view",
                    "win.toggle-view");
    /* Completed action items on/off; icon, label and tooltip are set by
     * done_button_sync() from the live setting.                          */
    lw->done_btn = add_tool_button(lw, toolbar, "hidden", "\xf0\x9f\x91\x81",
                    "Hide Completed", "Hide completed action items",
                    "win.toggle-done");
    add_tool_button(lw, toolbar, "search", "\xf0\x9f\x94\x8d",
                    "Search", "Open search window",
                    "app.search");
    add_tool_button(lw, toolbar, "images", "\xf0\x9f\x96\xbc",
                    "Media",
                    "Show every image in the listed notes as thumbnails",
                    "app.media");
    add_tool_button(lw, toolbar, "settings", "\xe2\x9a\x99",
                    "Settings", "Open the settings window",
                    "app.preferences");

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* Visibility is controlled entirely by ai_enabled (see
     * library_notify_ai_changed).                                          */
    lw->ai_btn = on_app_tool_item_new(lw->app, FALSE,
        "microchip", "\xf0\x9f\xa4\x96",
        "AI Summary", "Summarize notes with AI");
    g_signal_connect(lw->ai_btn, "clicked",
                     G_CALLBACK(on_ai_button_clicked), lw);
    gtk_widget_set_visible(lw->ai_btn, lw->app->ai_enabled);
    gtk_box_append(GTK_BOX(toolbar), lw->ai_btn);

    /* --- right edge: the query entry -------------------------------------
     * An expanding blank spacer pushes the rest of the toolbar left, the
     * same recipe the editor uses for its find-in-note entry.  Enter in the
     * entry opens the search window on the query (All Notes, case
     * insensitive); the Search button opens it empty as before.            */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);

    GtkWidget *entry = gtk_search_entry_new();
    g_object_set(entry, "placeholder-text", "Search all notes", NULL);
    on_app_set_tooltip(entry,
        "Search every note for this text (Enter)");
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 18);
    /* 5 px of air between the entry and the window edge.                    */
    gtk_widget_set_margin_end(entry, 5);
    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_toolbar_search_activate), lw);
    gtk_box_append(GTK_BOX(toolbar), entry);

    return toolbar;
}

/* library_free() — destructor for the OnLibrary attached to the window.     */
static void
library_free(gpointer data)
{
    OnLibrary *lw = data;
    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    if (lw->sb_fit_idle != 0) {
        g_signal_handler_disconnect(lw->sb_fit_clock, lw->sb_fit_idle);
        g_object_unref(lw->sb_fit_clock);
    }
    /* Cancel any in-flight AI subprocess before freeing lw.  The
     * GCancellable keeps the callback safe after the pointer is gone.    */
    ai_throbber_stop(lw);
    if (lw->ai_cancel != NULL) {
        g_cancellable_cancel(lw->ai_cancel);
        g_clear_object(&lw->ai_cancel);
    }
    thumb_pending_clear(lw);
    g_hash_table_destroy(lw->thumb_cache);
    if (lw->folder_path_cache != NULL)
        g_hash_table_destroy(lw->folder_path_cache);
    /* Our references on the models (the views hold their own; the
     * selection models handed to gtk_*_view_new were consumed by them). */
    g_clear_object(&lw->sb_store);
    g_clear_object(&lw->sb_tree);
    g_clear_object(&lw->notes_store);
    g_clear_object(&lw->notes_sorted);
    g_clear_object(&lw->notes_sel);
    g_clear_object(&lw->actions_store);
    g_clear_object(&lw->actions_sorted);
    /* GTK keeps its own reference to the menubar model while it renders
     * it; these are just ours.                                            */
    g_clear_object(&lw->menubar_model);
    g_free(lw->sel_name);
    g_free(lw);
}

/* Narrowest the sidebar is ever fitted to: a library of short folder names
 * must not leave a sliver of a pane.                                       */
#define SB_FIT_MIN_WIDTH 160

/* The most of the paned the sidebar may take when fitting itself to its
 * content.  A deeply nested branch can be arbitrarily wide, and the notes
 * pane still has to be usable.                                             */
#define SB_FIT_MAX_PERCENT 50

/* ---------------------------------------------------------------------------
 * sidebar_fit_apply() — size the sidebar divider so the VISIBLE rows fit
 * exactly.  The measurement is the list view's own natural width: a list
 * view realizes only the rows on screen, and each row's natural width is
 * its full text (the label ellipsizes only under its natural size), so
 * GTK's measure IS "the widest row the user can see" — the tree view
 * needed a Pango walk of the model for the same number.  The scrolled
 * window's vertical scrollbar, which comes and goes as folders open and
 * close, is added when it shows.  Nothing here reads the CURRENT
 * allocation, so the arithmetic is the same before and after the first
 * layout — it only waits for the list to be mapped, since unrealized
 * rows measure as nothing.
 *
 * Symmetric: expanding a folder widens the pane, collapsing one gives the
 * width back.  Turning the setting ON therefore hands the divider over to
 * this function, and a width the user dragged is not preserved across the
 * next expand or collapse — that is the deal the setting makes.  The
 * one-shot startup fit (`force`) runs whatever the setting says.
 * ------------------------------------------------------------------------- */
static void
sidebar_fit_apply(OnLibrary *lw, gboolean force)
{
    if (!force && !lw->app->sidebar_fit_content)
        return;
    if (!gtk_widget_get_mapped(GTK_WIDGET(lw->sidebar)))
        return;
    gint nat_w;                      /* the list view's natural width       */
    gtk_widget_measure(GTK_WIDGET(lw->sidebar), GTK_ORIENTATION_HORIZONTAL,
                       -1, NULL, &nat_w, NULL, NULL);
    gint want = MAX(nat_w, SB_FIT_MIN_WIDTH);
    GtkWidget *bar = gtk_scrolled_window_get_vscrollbar(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(GTK_WIDGET(lw->sidebar))));
    if (gtk_widget_get_visible(bar)) {
        gint bar_w;
        gtk_widget_measure(bar, GTK_ORIENTATION_HORIZONTAL, -1, NULL,
                           &bar_w, NULL, NULL);
        want += bar_w;
    }
    gint full = gtk_widget_get_width(lw->sidebar_paned);
    if (full > 0)
        want = MIN(want, full * SB_FIT_MAX_PERCENT / 100);
    if (want != gtk_paned_get_position(GTK_PANED(lw->sidebar_paned)))
        gtk_paned_set_position(GTK_PANED(lw->sidebar_paned), want);
}

/* sidebar_fit_after_paint() — the frame that laid the change out has been
 * painted: fit now, and stop listening.                                    */
static void
sidebar_fit_after_paint(GdkFrameClock *clock, gpointer user_data)
{
    OnLibrary *lw = user_data;
    g_signal_handler_disconnect(clock, lw->sb_fit_idle);
    lw->sb_fit_idle = 0;
    g_clear_object(&lw->sb_fit_clock);
    sidebar_fit_apply(lw, lw->sb_fit_force);
    lw->sb_fit_force = FALSE;
}

/* sidebar_fit_queue() — run sidebar_fit_apply() once the expand, collapse
 * or rebuild has been LAID OUT: the list view's width request counts the
 * row widgets it has, and it creates and garbage-collects those in its
 * size_allocate, which the frame clock runs on the next frame — an idle
 * ran before that frame and measured the OLD rows (a collapse never
 * narrowed the pane).  So the fit waits for the frame clock's after-paint,
 * once; a rebuild's expansion restore, which changes the model once per
 * restored row, coalesces onto that one frame.
 *   lw — library window state.                                             */
static void
sidebar_fit_queue(OnLibrary *lw, gboolean force)
{
    if (!force && !lw->app->sidebar_fit_content)
        return;
    lw->sb_fit_force |= force;
    if (lw->sb_fit_idle != 0)
        return;                      /* already waiting for the frame       */
    GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(lw->sidebar));
    if (clock == NULL)
        return;                      /* not mapped: the startup fit covers  */
    lw->sb_fit_clock = g_object_ref(clock);
    lw->sb_fit_idle = g_signal_connect(clock, "after-paint",
                                       G_CALLBACK(sidebar_fit_after_paint),
                                       lw);
    gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_PAINT);
}

/* on_sidebar_rows_changed() — the flattened sidebar model changed (a row
 * expanded or collapsed, or a rebuild): THE trigger for fitting the
 * sidebar to its content.                                                  */
static void
on_sidebar_rows_changed(GListModel *model, guint position, guint removed,
                        guint added, gpointer user_data)
{
    (void)model; (void)position; (void)removed; (void)added;
    sidebar_fit_queue(user_data, FALSE);
}

/* on_sidebar_mapped() — the list is on screen: the one-shot startup fit,
 * once its rows have been laid out and painted (whatever the setting).   */
static void
on_sidebar_mapped(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    sidebar_fit_queue(user_data, TRUE);
}

/* ---------------------------------------------------------------------------
 * on_library_sidebar_fit() — public: re-fit the library sidebar, used by
 * Settings when the "fit to content" box is ticked so it takes effect on
 * the spot.  No-op with the setting off, or with no library window.
 *   app — global application context.
 * ------------------------------------------------------------------------- */
void
on_library_sidebar_fit(OnApp *app)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw != NULL)
        sidebar_fit_queue(lw, FALSE);
}

/* sb_create_children() — GtkTreeListModel's child model for a row: its
 * children store (a ref), or NULL for a leaf.                              */
static GListModel *
sb_create_children(gpointer item, gpointer user_data)
{
    (void)user_data;
    OnSbRow *r = item;
    return r->children != NULL ? g_object_ref(G_LIST_MODEL(r->children))
                               : NULL;
}

/* scrolled() — a scrolled window around a view, the app's conventions.    */
static GtkWidget *
scrolled(GtkWidget *view, GtkPolicyType hpolicy)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), hpolicy,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(sw), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), view);
    return sw;
}

/* ---------------------------------------------------------------------------
 * library_build_sidebar() — build lw->sidebar (a GtkListView over the
 * flattened folder/tag tree) and lw->sidebar_box (its scroll container),
 * ready to be packed into the paned.
 * ------------------------------------------------------------------------- */
static void
library_build_sidebar(OnLibrary *lw)
{
    lw->sb_store = g_list_store_new(ON_TYPE_SB_ROW);
    lw->sb_tree  = gtk_tree_list_model_new(
        G_LIST_MODEL(g_object_ref(lw->sb_store)), FALSE, FALSE,
        sb_create_children, NULL, NULL);
    lw->sb_sel = gtk_single_selection_new(
        G_LIST_MODEL(g_object_ref(lw->sb_tree)));
    gtk_single_selection_set_autoselect(lw->sb_sel, FALSE);
    gtk_single_selection_set_can_unselect(lw->sb_sel, FALSE);
    g_signal_connect(lw->sb_sel, "selection-changed",
                     G_CALLBACK(on_sidebar_selection_changed), lw);
    g_signal_connect(lw->sb_tree, "items-changed",
                     G_CALLBACK(on_sidebar_rows_changed), lw);

    lw->sidebar = GTK_LIST_VIEW(gtk_list_view_new(
        GTK_SELECTION_MODEL(lw->sb_sel),
        on_row_factory_new(G_CALLBACK(on_sidebar_setup),
                    G_CALLBACK(on_sidebar_bind), lw)));
    /* Sidebar palette and drop indicator: the "notes-sidebar" rules in
     * library_install_css (see its banner for the colours).                */
    gtk_widget_add_css_class(GTK_WIDGET(lw->sidebar), "notes-sidebar");
    /* Fit the pane to its content on first show — from the list's "map",
     * the first moment its rows exist to be measured; connected here,
     * before anything can map it.                                         */
    g_signal_connect(lw->sidebar, "map", G_CALLBACK(on_sidebar_mapped), lw);

    GtkWidget *sidebar_scroll = scrolled(GTK_WIDGET(lw->sidebar),
                                         GTK_POLICY_NEVER);
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);

    /* Sidebar column: a fixed spacer, then the list (all buttons live in
     * the unified toolbar above the paned).  Its minimum width is whatever
     * the list content needs — the scrolled window never scrolls
     * horizontally, so it requests the list's full natural width.          */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Top padding, so the first row's text sits level with the text in the
     * notes list's column headers (the sidebar has no headers of its own).
     * It is a SPACER WIDGET rather than CSS padding: GtkScrolledWindow
     * ignores padding when allocating its child, and a margin on the list
     * would scroll away with it.  Painted in the sidebar grey so the
     * strip reads as part of the pane: a GtkBox has no background of its
     * own, so library_install_css gives it the list's backdrop from the
     * ONE declaration both share.                                          */
    GtkWidget *sidebar_pad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar_pad, -1, SB_TOP_PAD);
    gtk_widget_add_css_class(sidebar_pad, "notes-sidebar-pad");
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_pad);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_scroll);
    lw->sidebar_box = sidebar_box;   /* for the toolbar show/hide toggle    */
}

/* column_new() — one column of a column view: title, factory, sorter,
 * key, resizable; `expand` for the one that takes the leftover width.     */
static GtkColumnViewColumn *
column_new(GtkColumnView *view, const gchar *title, GtkListItemFactory *f,
           GCompareDataFunc cmp, const gchar *key, gboolean expand)
{
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
    GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(cmp, NULL, NULL));
    gtk_column_view_column_set_sorter(col, sorter);
    g_object_unref(sorter);
    gtk_column_view_column_set_resizable(col, TRUE);
    gtk_column_view_column_set_expand(col, expand);
    g_object_set_data(G_OBJECT(col), "on-colkey", (gpointer)key);
    gtk_column_view_append_column(view, col);
    g_object_unref(col);             /* the view holds it                   */
    return col;
}

/* ---------------------------------------------------------------------------
 * library_build_notes_list() — the notes models (store, sorted model and
 * THE selection, shared with the grid) and lw->notes_list, a GtkColumnView
 * with its four columns, sorters and column layout; returns the scroll
 * container ready to be added to the notes stack.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_notes_list(OnLibrary *lw)
{
    lw->notes_store  = g_list_store_new(ON_TYPE_NOTE_ROW);
    lw->notes_sorted = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(lw->notes_store)), NULL);
    lw->notes_sel    = gtk_multi_selection_new(
        G_LIST_MODEL(g_object_ref(lw->notes_sorted)));
    g_signal_connect(lw->notes_sel, "selection-changed",
                     G_CALLBACK(on_notes_selection_status), lw);

    lw->notes_list = GTK_COLUMN_VIEW(gtk_column_view_new(
        GTK_SELECTION_MODEL(g_object_ref(lw->notes_sel))));
    gtk_widget_add_css_class(GTK_WIDGET(lw->notes_list), "notes-columns");
    gtk_column_view_set_reorderable(lw->notes_list, TRUE);
    /* The view's sorter (what the headers set) drives the sorted model. */
    gtk_sort_list_model_set_sorter(
        lw->notes_sorted, gtk_column_view_get_sorter(lw->notes_list));

    GtkListItemFactory *f;
    GtkColumnViewColumn *c_mod = NULL;
    f = on_row_factory_new(G_CALLBACK(on_title_setup), G_CALLBACK(on_title_bind),
                    lw);
    column_new(lw->notes_list, "Title", f, cmp_title, "title", TRUE);
    /* The three plain text columns share one factory pair, told apart by
     * the field they show; the factory carries lw for the controllers.   */
    const struct { const gchar *title; gint field; GCompareDataFunc cmp;
                   const gchar *key; } TEXT_COLS[] = {
        { "Path",     NF_PATH,     cmp_path,    "path"     },
        { "Modified", NF_MODIFIED, cmp_updated, "modified" },
        { "Created",  NF_CREATED,  cmp_created, "created"  },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(TEXT_COLS); i++) {
        f = on_row_factory_new(G_CALLBACK(on_note_text_setup),
                               G_CALLBACK(on_note_text_bind),
                               GINT_TO_POINTER(TEXT_COLS[i].field));
        g_object_set_data(G_OBJECT(f), "on-lw", lw);
        GtkColumnViewColumn *c = column_new(lw->notes_list, TEXT_COLS[i].title,
                                            f, TEXT_COLS[i].cmp,
                                            TEXT_COLS[i].key, FALSE);
        if (TEXT_COLS[i].field == NF_MODIFIED)
            c_mod = c;
        /* Built HIDDEN: a saved list_columns without a "created" entry
         * (every pre-existing ini) keeps the built state, so the column
         * stays off until toggled in the header menu.                    */
        if (TEXT_COLS[i].field == NF_CREATED)
            gtk_column_view_column_set_visible(c, FALSE);
    }

    /* Default sort: Modified with the most recent on top (cmp_updated is
     * deliberately inverted, so ASCENDING = newest first).  The headers
     * only ever cycle ascending and descending, so the list is ALWAYS
     * sorted; a note drag is a move to a folder, never a reorder.        */
    gtk_column_view_sort_by_column(lw->notes_list, c_mod, GTK_SORT_ASCENDING);

    g_object_set_data(G_OBJECT(lw->notes_list), "on-colcfg",
                      (gpointer)"list_columns");
    g_object_set_data(G_OBJECT(lw->notes_list), "on-coldefault",
                      (gpointer)"path:0,title:1,modified:1,created:0");
    view_columns_install(lw, lw->notes_list);

    g_signal_connect(lw->notes_list, "activate",
                     G_CALLBACK(on_note_activated), lw);
    return scrolled(GTK_WIDGET(lw->notes_list), GTK_POLICY_AUTOMATIC);
}

/* ---------------------------------------------------------------------------
 * library_build_notes_grid() — lw->notes_grid, a GtkGridView over the same
 * sorted model and selection as the list; returns the scroll container.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_notes_grid(OnLibrary *lw)
{
    lw->notes_grid = GTK_GRID_VIEW(gtk_grid_view_new(
        GTK_SELECTION_MODEL(g_object_ref(lw->notes_sel)),
        on_row_factory_new(G_CALLBACK(on_grid_setup), G_CALLBACK(on_grid_bind),
                    lw)));
    gtk_widget_add_css_class(GTK_WIDGET(lw->notes_grid), "notes-grid");
    gtk_grid_view_set_max_columns(lw->notes_grid, 20);
    g_signal_connect(lw->notes_grid, "activate",
                     G_CALLBACK(on_note_activated), lw);
    return scrolled(GTK_WIDGET(lw->notes_grid), GTK_POLICY_AUTOMATIC);
}

/* ---------------------------------------------------------------------------
 * library_build_actions_view() — build lw->actions_store and
 * lw->actions_view (a GtkColumnView) with their three columns, sorters
 * and column layout; returns the scroll container.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_actions_view(OnLibrary *lw)
{
    lw->actions_store  = g_list_store_new(ON_TYPE_ACTION_ROW);
    lw->actions_sorted = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(lw->actions_store)), NULL);
    GtkSingleSelection *sel = gtk_single_selection_new(
        G_LIST_MODEL(g_object_ref(lw->actions_sorted)));
    gtk_single_selection_set_autoselect(sel, FALSE);

    lw->actions_view = GTK_COLUMN_VIEW(gtk_column_view_new(
        GTK_SELECTION_MODEL(sel)));
    gtk_widget_add_css_class(GTK_WIDGET(lw->actions_view), "notes-columns");
    gtk_column_view_set_reorderable(lw->actions_view, TRUE);
    gtk_sort_list_model_set_sorter(
        lw->actions_sorted, gtk_column_view_get_sorter(lw->actions_view));

    /* Untitled checkbox column + the item text + the due date; done rows
     * also render struck through, matching the editor.                    */
    GtkColumnViewColumn *cd = column_new(
        lw->actions_view, "",
        on_row_factory_new(G_CALLBACK(on_action_done_setup),
                    G_CALLBACK(on_action_done_bind), lw),
        cmp_action_done, "done", FALSE);
    g_object_set_data(G_OBJECT(cd), "on-collabel", (gpointer)"Done");
    column_new(lw->actions_view, "Action",
               on_row_factory_new(G_CALLBACK(on_action_text_setup),
                           G_CALLBACK(on_action_text_bind), lw),
               cmp_action_text, "action", TRUE);
    column_new(lw->actions_view, "Due Date",
               on_row_factory_new(G_CALLBACK(on_action_due_setup),
                           G_CALLBACK(on_action_due_bind), lw),
               cmp_action_due, "due", FALSE);

    g_object_set_data(G_OBJECT(lw->actions_view), "on-colcfg",
                      (gpointer)"action_columns");
    g_object_set_data(G_OBJECT(lw->actions_view), "on-coldefault",
                      (gpointer)"done:1,action:1,due:1");
    view_columns_install(lw, lw->actions_view);
    g_signal_connect(lw->actions_view, "activate",
                     G_CALLBACK(on_action_row_activated), lw);
    return scrolled(GTK_WIDGET(lw->actions_view), GTK_POLICY_AUTOMATIC);
}

/* ---------------------------------------------------------------------------
 * library_build_notes_pane() — build the three note views (list, grid,
 * actions) and assemble them into lw->stack, ready to be packed into the
 * notes paned.  The list is built first: it owns the models the grid
 * shares.
 * ------------------------------------------------------------------------- */
static void
library_build_notes_pane(OnLibrary *lw)
{
    GtkWidget *list_scroll    = library_build_notes_list(lw);
    GtkWidget *grid_scroll    = library_build_notes_grid(lw);
    GtkWidget *actions_scroll = library_build_actions_view(lw);

    /* --- stack: list <-> grid <-> action items ----------------------------*/
    lw->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(lw->stack), list_scroll,    "list");
    gtk_stack_add_named(GTK_STACK(lw->stack), grid_scroll,    "grid");
    gtk_stack_add_named(GTK_STACK(lw->stack), actions_scroll, "actions");
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "list");
    g_signal_connect(lw->stack, "notify::visible-child-name",
                     G_CALLBACK(on_view_stack_changed), lw);
}

/* ---------------------------------------------------------------------------
 * library_build_status_bar() — build the status bar with lw->status_path
 * (left) and lw->status_event in lw->status_revealer (right); returns the
 * assembled box widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_status_bar(OnLibrary *lw)
{
    /* --- status bar: selection path (left) + latest event (right) ----------*/
    lw->status_path = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lw->status_path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_path),
                            PANGO_ELLIPSIZE_MIDDLE);

    lw->status_event = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lw->status_event), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_event),
                            PANGO_ELLIPSIZE_MIDDLE);
    /* Same colour as the path label on the left — no "dim-label": the
     * message fades on its way OUT (the revealer), it does not start dim. */

    /* Event messages fade: the label sits in a crossfading revealer that
     * library_notify_status() opens and a timer closes.                     */
    lw->status_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(lw->status_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(lw->status_revealer),
                                         600);
    gtk_revealer_set_child(GTK_REVEALER(lw->status_revealer),
                           lw->status_event);

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 3);
    gtk_widget_set_margin_bottom(status_bar, 3);
    gtk_widget_set_hexpand(lw->status_path, TRUE);   /* pushes the event
                                                        label to the right */
    gtk_box_append(GTK_BOX(status_bar), lw->status_path);
    gtk_box_append(GTK_BOX(status_bar), lw->status_revealer);

    /* Both labels a step smaller than the UI font.                          */
    gtk_widget_add_css_class(lw->status_path,  "notes-status-label");
    gtk_widget_add_css_class(lw->status_event, "notes-status-label");

    return status_bar;
}

/* ---------------------------------------------------------------------------
 * library_install_css() — the library's DISPLAY-level stylesheet, installed
 * once per process at application priority (so every rule outranks the
 * theme's in any widget state), scoped by the "notes-" classes the window
 * puts on its widgets.
 *
 * 1. GTK's alert dialogs: ordinary, spaced buttons with a margin instead
 *    of the theme's joined full-width bar (window.dialog.message).
 * 2. Grid cards (`gridview.notes-grid > child`): padding, a hover outline
 *    on every card, and a tint only on an UNSELECTED one — a
 *    near-transparent tint under text the selected state has turned white
 *    was an invisible title until the mouse left the card.
 * 3. The notes list's alternating row tint, by `row:nth-child(even)`, and
 *    never over the selection highlight (`:not(:selected)`, since a later
 *    application-priority rule of equal specificity would win over the
 *    theme's `row:selected`).  The Comfortable preview's dimming (an
 *    opacity, so it stays readable on the highlight) and the Due Date
 *    urgency colours live here too.
 * 4. Sidebar palette.  The backdrop (rows AND the empty area below them —
 *    the list view paints the whole widget) is the theme's window/toolbar
 *    background taken down a step (SB_BG_SHADE), so the pane sits just
 *    behind the toolbar above it and reads as distinct from the white
 *    notes list without pinning a grey of its own.  The spacer strip above
 *    the list (library_build_sidebar) shares the declaration.  Then muted
 *    grey text and a blue selection bar with white text.  Verified on GTK
 *    4.22's compiled Default theme: it still defines @theme_bg_color
 *    (#f6f5f4 light) and still parses shade() — both DEPRECATED since 4.16
 *    (they warn only under GTK_DEBUG=css) but the theme exports no CSS
 *    variables to replace them with.  Beware that an UNDEFINED colour name
 *    is NOT a parse error — it silently renders transparent.
 * 5. Sidebar drop indicator: the row under the pointer carries one of the
 *    classes sb_row_indicate paints (drop-into / -before / -after), drawn
 *    as an inset box-shadow in the selection blue — a full frame for INTO,
 *    the top edge for BEFORE, the bottom for AFTER.
 * 6. The emoji entry of the folder dialog: one emoji wide — the theme's
 *    entry min-width would otherwise span the dialog (D22).
 * 7. The AI pane's two compact header buttons.
 * 8. The sidebar/notes divider: a 6 px handle (wide-handle mode gives the
 *    separator node a 5 px theme floor; min-WIDTH is the lever on a
 *    horizontal paned).
 * 9. The notes list / Action Items headers: only the bottom rule — the
 *    theme's per-button left border doubled the divider's edge line.
 * 10. The folder dialog's emoji entry hides its caret while it holds an
 *    emoji (class "notes-emoji-full", toggled by on_emoji_entry_changed).
 * 11. No focus ring on a list row or grid card: the selection highlight
 *    is where the keyboard is.  GtkWindow turns the ring on when a
 *    modifier key is RELEASED with the focus on a different widget than
 *    it was on at the press (_gtk_window_update_focus_visible) — which a
 *    Shift/Cmd-click on a row is, exactly — and the theme's 2 px outline
 *    then flashed along the top edge of the last row clicked (the rest of
 *    it clipped by the row) until GTK's 3 s timeout.
 * 12. No pressed-state shadow on a row or card either: the theme paints an
 *    inset top shadow on `row.activatable:active`, darker when the row is
 *    also selected — invisible while rows selected on the release, a
 *    dark rim along the top of the row once they select on the press
 *    (D37).
 * ------------------------------------------------------------------------- */
static void
library_install_css(void)
{
    static gboolean installed = FALSE;
    if (installed)
        return;
    installed = TRUE;
    gchar *css = g_strdup_printf(
        /* GTK's alert dialogs (confirm(), Open Database…'s Session Only /
         * Set as Default): the Default theme joins their buttons into one
         * full-width, flush bar of 10 px-tall buttons (its "csd" message
         * dialog).  Ordinary buttons, spaced, with the window margin the
         * dialog_new scaffold gives its own.                              */
        "window.dialog.message.csd .dialog-action-area button {"
        "  padding: 4px 14px; border-radius: 5px; border-style: solid;"
        "  margin: 0;"
        "}"
        "window.dialog.message .dialog-action-area {"
        "  padding: 0 12px 12px 12px; border-spacing: 6px;"
        "}"
        /* The grid's cards: a hover outline on every one, the tint only on
         * an UNSELECTED one — a near-transparent tint under text the
         * selected state has turned white was an invisible title until
         * the mouse left the cell.                                       */
        "gridview.notes-grid > child { padding: 6px; transition: none; }"
        /* 11: no focus ring on rows and cards (see the header).           */
        "columnview.notes-columns > listview > row:focus:focus-visible,"
        "listview.notes-sidebar > row:focus:focus-visible,"
        "columnview.search-results > listview > row:focus:focus-visible,"
        "gridview.notes-grid > child:focus:focus-visible {"
        "  outline-width: 0; transition: none;"
        "}"
        /* 12: no pressed-state shadow (see the header).                   */
        "columnview.notes-columns > listview > row:active,"
        "listview.notes-sidebar > row:active,"
        "columnview.search-results > listview > row:active,"
        "gridview.notes-grid > child:active {"
        "  box-shadow: none;"
        "}"
        "gridview.notes-grid > child:hover {"
        "  outline: 1px solid alpha(black, 0.4);"
        "  outline-offset: -1px;"
        "  border-radius: 4px;"
        "}"
        "gridview.notes-grid > child:hover:not(:selected) {"
        "  background-color: alpha(currentColor, 0.06);"
        "}"
        /* The notes list's alternating row tint (the even rows; odd stay
         * white), never over the selection highlight.                    */
        "columnview.notes-columns > listview > row:nth-child(even)"
        ":not(:selected) {"
        "  background-color: %s;"
        "}"
        /* The Comfortable preview dims through alpha, so it stays
         * readable on the selection highlight (a fixed grey did not).   */
        "columnview.notes-columns label.notes-preview { opacity: 0.65; }"
        /* Rows carry their own vertical spacing (on_title_bind's margins);
         * the theme's cell padding on top made them a quarter too tall. */
        "columnview.notes-columns > listview > row > cell {"
        "  padding-top: 2px; padding-bottom: 2px;"
        "}"
        /* Compact density: rows the height of the sidebar's, 20 px — a
         * 15 px label plus this, uneven because a half pixel rounds down
         * (the density class is set by refresh_notes; measured on 4.22's
         * Default theme).                                                */
        "columnview.notes-compact > listview > row > cell {"
        "  padding-top: 3px; padding-bottom: 2px;"
        "}"
        /* Due Date urgency (on_action_due_bind): overdue red, today gold,
         * ahead green — darkened enough to read on the row stripes.      */
        "label.due-overdue { color: #c01c28; }"
        "label.due-today   { color: #d19a00; }"
        "label.due-ahead   { color: #26a269; }"
        "listview.notes-sidebar, box.notes-sidebar-pad {"
        "  background-color: shade(@theme_bg_color, " SB_BG_SHADE ");"
        "}"
        "listview.notes-sidebar { color: rgb(65,65,65); }"
        "listview.notes-sidebar > row:selected {"
        "  background-color: rgb(86,131,224);"
        "  color: white;"
        "}"
        /* The drop indicator, painted by class on the row under the
         * pointer (sb_row_indicate): a frame for INTO, an edge for
         * BEFORE/AFTER.                                                  */
        "listview.notes-sidebar > row.drop-into {"
        "  box-shadow: inset 0 0 0 2px rgb(86,131,224);"
        "}"
        "listview.notes-sidebar > row.drop-before {"
        "  box-shadow: inset 0 2px 0 0 rgb(86,131,224);"
        "}"
        "listview.notes-sidebar > row.drop-after {"
        "  box-shadow: inset 0 -2px 0 0 rgb(86,131,224);"
        "}"
        "entry.notes-emoji-entry { font-size: 18px; min-width: 0; }"
        "button.notes-ai-button {"
        "  padding: 0 4px; min-height: 0; font-size: 85%%;"
        "}"
        "paned.notes-split > separator { min-width: 6px; }"
        /* Column headers: the bottom rule only.  The theme draws a left
         * border on each header button, which sat against the paned
         * divider as a second line.                                     */
        "columnview.notes-columns > header > button {"
        "  border-left-style: none; border-right-style: none;"
        "}"
        /* The emoji entry: no caret while it holds an emoji — Apple Color
         * Emoji inks past its advance and the caret stood inside it (the
         * editor pads the glyph; an entry cannot).                       */
        "entry.notes-emoji-entry.notes-emoji-full text { caret-color: transparent; }",
        ROW_TINT);
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_free(css);
}

/* ---------------------------------------------------------------------------
 * on_library_window_create() — build and show the library window.
 * Creates the OnLibrary state, models, and sub-panes via the builder helpers
 * above, assembles the layout, and triggers the initial data load.
 * ------------------------------------------------------------------------- */
GtkWidget *
on_library_window_create(OnApp *app)
{
    OnLibrary *lw = g_new0(OnLibrary, 1);
    lw->app      = app;
    lw->sel_kind = SB_KIND_ROOT;
    lw->sel_id   = 0;
    lw->sel_name = g_strdup("Notes");
    lw->thumb_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, thumb_entry_free);
    library_install_css();

    /* --- window (standard titlebar, no HeaderBar) ------------------------
     * A GtkApplicationWindow: that is what gives it the "win." action
     * group (context menus, toolbar, shortcuts) and, on a desktop with no
     * shell menubar, renders the application menubar at its top.  It adds
     * itself to the application.                                          */
    lw->window = gtk_application_window_new(app->gtk_app);
    gtk_window_set_title(GTK_WINDOW(lw->window), "Notes - Library");
    gtk_window_set_default_size(GTK_WINDOW(lw->window), 900, 620);
    g_object_set_data_full(G_OBJECT(lw->window), "on-library", lw,
                           library_free);
    /* Every text cell in this window renders in the window's UI font, so
     * the emoji padding is measured once here (see ON_EMOJI_GAP).        */
    lw->emoji_pad = on_emoji_pad(gtk_widget_get_pango_context(lw->window));

    /* A weak pointer: closing the library while editors are open must
     * leave app->library_window NULL, not dangling — the "app." actions
     * (native macOS menubar, alive as long as any window is) look it up.  */
    app->library_window       = lw->window;
    g_object_add_weak_pointer(G_OBJECT(lw->window),
                              (gpointer *)&app->library_window);
    app->notify_notes_changed = library_notify_notes_changed;
    app->notify_note_saved    = library_notify_note_saved;
    app->notify_status        = library_notify_status;
    app->notify_ai_changed    = library_notify_ai_changed;

    /* --- panes + status bar (each builds its own models) ------------------*/
    library_install_actions(lw);         /* the column views register
                                            actions on the window as they
                                            are built                       */
    library_build_sidebar(lw);           /* sets lw->sidebar, lw->sidebar_box */
    library_build_notes_pane(lw);        /* sets lw->notes_list, lw->notes_grid,
                                          * lw->actions_view, lw->stack       */
    GtkWidget *status_bar = library_build_status_bar(lw);

    /* --- assemble -----------------------------------------------------------*/
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    lw->sidebar_paned = paned;
    /* A 6 px divider: wide-handle switches GtkPaned off its hairline style,
     * and the exact width comes from CSS on the handle's own `separator`
     * node (this paned is horizontal, so its separator is vertical and
     * min-WIDTH is the lever) — the "notes-split" rule in
     * library_install_css.                                                 */
    gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
    gtk_widget_add_css_class(paned, "notes-split");
    gtk_paned_set_start_child(GTK_PANED(paned), lw->sidebar_box);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    lw->ai_pane = build_ai_pane(lw);
    /* Vertical paned so the user can drag the divider between the notes list
     * and the AI summary pane.  GtkPaned collapses the divider automatically
     * when the end child is hidden.                                           */
    GtkWidget *notes_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    lw->notes_paned = GTK_PANED(notes_paned);
    gtk_paned_set_start_child(GTK_PANED(notes_paned), lw->stack);
    gtk_paned_set_resize_start_child(GTK_PANED(notes_paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(notes_paned), FALSE);
    gtk_paned_set_end_child(GTK_PANED(notes_paned), lw->ai_pane);
    gtk_paned_set_resize_end_child(GTK_PANED(notes_paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(notes_paned), FALSE);
    gtk_paned_set_end_child(GTK_PANED(paned), notes_paned);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_vexpand(paned, TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* The actions exist (installed above, before the views) so the
     * menubar's and toolbar's items come up sensitive.                    */
    lw->menubar_model = build_menubar();
#ifdef __APPLE__
    /* The in-window rendering of the menubar, for the "native_menubar"
     * setting's OFF state; on_library_apply_native_menubar below decides
     * which of the two shows (built hidden: that call owns its
     * visibility).  Elsewhere the GtkApplicationWindow renders the
     * application menubar itself, so nothing is packed.                   */
    lw->menubar = gtk_popover_menu_bar_new_from_model(lw->menubar_model);
    gtk_widget_set_visible(lw->menubar, FALSE);
    gtk_box_append(GTK_BOX(vbox), lw->menubar);
#endif
    gtk_box_append(GTK_BOX(vbox), build_action_bar(lw));
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(vbox), paned);
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(vbox), status_bar);
    gtk_window_set_child(GTK_WINDOW(lw->window), vbox);

    /* --- initial population -------------------------------------------------*/
    refresh_all(lw);
    on_app_status(app, "DB at %s loaded", app->db->path);

    gtk_window_present(GTK_WINDOW(lw->window));
    on_library_apply_native_menubar(
        app, on_app_config_get_bool("native_menubar", FALSE));

    /* The View item's label is read from the pane's live visibility; the
     * label it was built with is only a placeholder, and the actions it
     * enables exist only since library_install_actions above.             */
    sidebar_menu_sync(lw);
    /* Likewise the List/Grid button: the toolbar is built after the stack,
     * so it missed the stack's construction-time child change.            */
    view_button_sync(lw);
    done_button_sync(lw);


    return lw->window;
}
