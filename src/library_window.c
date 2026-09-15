/* ===========================================================================
 * library_window.c — the Notes Library window (implementation)
 *
 * See library_window.h for the layout overview.  Key mechanics:
 *
 *   sidebar    — a GtkTreeView over a GtkTreeStore holding the sections
 *                "All Notes", the folder hierarchy (rooted at a fixed
 *                "Notes" row), a flat "Tags" section, "Pinned Notes",
 *                and — while non-empty — "Trash" (trashed folders as its
 *                children).  Row kinds are distinguished by the SB_KIND
 *                column.
 *
 *   notes pane — a GtkListStore shown either as a GtkTreeView (list mode)
 *                or a GtkIconView (grid mode).  Both views stay attached
 *                to the same store; a GtkStack flips between them.
 *
 *   drag&drop  — both notes views are GtkDragSources handing over the
 *                selected note ids, and the sidebar is THE GtkDropTarget:
 *                a note drop moves the notes into the folder (or trashes
 *                them).  The sidebar is a drag source too: a folder row
 *                drops INTO a folder (re-nest), BETWEEN folders
 *                (reorder/re-nest beside the sibling), onto Trash (delete
 *                gesture), or out of Trash (restore).  One boxed value
 *                type, OnDragRows, is the whole content of every drag
 *                (GTK4_MIGRATION.md, D8).
 * =========================================================================== */

/* This file lives on the DEPRECATED GtkTreeView / GtkIconView family by
 * decision (GTK4_MIGRATION.md, "Tree views stay on the deprecated
 * GtkTreeView family"): the sidebar, the notes list, the Action Items view
 * and the grid keep their tree models and cell renderers until the GTK5
 * migration replaces them with list models.  The per-call deprecation
 * warnings are silenced for the whole file rather than wrapping a few
 * hundred call sites; every OTHER file still warns.                         */
#define GDK_DISABLE_DEPRECATION_WARNINGS
#define GTK_DISABLE_DEPRECATION_WARNINGS

#include "library_window.h"
#include "backup.h"
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
 * ONE definition: list_autofit_time_width() bounds the column width from
 * this same pattern, so the two can never disagree about the shape.         */
#define LIST_TIME_FORMAT "%b %e, %Y %H:%M"

/* Blank strip above the sidebar tree, to line its first row's text up with
 * the notes list's column-header text (see library_build_sidebar).          */
#define SB_TOP_PAD 3

/* How far the sidebar backdrop sits below the toolbar/window background it
 * is shaded from — a CSS shade() factor, < 1 darkens.  0.96 turns Adwaita's
 * rgb(246,245,244) into rgb(238,236,234).  A string, not a number: it is
 * pasted into one CSS declaration in library_install_css, shared by the
 * tree view and the spacer strip above it.                                  */
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

/* Sidebar GtkTreeStore columns.                                             */
enum {
    SB_KIND,                         /* gint: one of SB_KIND_*              */
    SB_ID,                           /* gint64: folder id or tag id         */
    SB_NAME,                         /* gchar*: display text (with count)   */
    SB_RAW,                          /* gchar*: bare name (no count suffix) */
    SB_N_COLS
};

/* Notes GtkListStore columns.                                               */
enum {
    NL_ID,                           /* gint64: note id                     */
    NL_TITLE,                        /* gchar*: note title                  */
    NL_MODIFIED,                     /* gchar*: formatted updated_at        */
    NL_THUMB,                        /* GdkTexture*: grid thumbnail         */
    NL_UPDATED,                      /* gint64: raw updated_at (sort key)   */
    NL_PATH,                         /* gchar*: "/Folder/Sub" location      */
    NL_CREATED,                      /* gchar*: formatted created_at        */
    NL_CREATED_RAW,                  /* gint64: raw created_at (sort key)   */
    NL_PREVIEW,                      /* gchar*: first line of body text      */
    NL_N_COLS
};

/* Columns of the Action Items list model (the third notes-pane view).       */
enum {
    AL_NOTE_ID,                      /* gint64: owning note id              */
    AL_ORD,                          /* gint: position among the note's
                                        action lines (addresses the item)   */
    AL_DONE,                         /* gboolean: checkbox state            */
    AL_TEXT,                         /* gchar*: the item text               */
    AL_DUE,                          /* gchar*: formatted due date ("")     */
    AL_DUE_RAW,                      /* gint64: due timestamp (sort key;
                                        0 = none, sorts after any date)     */
    AL_N_COLS
};

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
 *   sidebar_store — tree model behind the sidebar.
 *   sidebar       — the sidebar GtkTreeView.
 *   notes_store   — list model behind both notes views.
 *   notes_list    — list-mode view (GtkTreeView).
 *   notes_grid    — grid-mode view (GtkIconView).
 *   stack         — GtkStack switching between list and grid.
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
 *   column_menu_view — the list view whose column header was last
 *                   right-clicked; the "column-<key>" actions act on it.
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
    GtkTreeStore *sidebar_store;
    GtkTreeView  *sidebar;
    GtkListStore *notes_store;
    GtkTreeView  *notes_list;
    GtkIconView  *notes_grid;
    GtkListStore *actions_store;         /* Action Items model (AL_*)      */
    GtkTreeView  *actions_view;          /* Action Items list view         */
    gboolean      grid_pref;             /* the user's list/grid choice, so
                                            leaving the Action Items view
                                            restores the right mode        */
    GtkWidget    *stack;
    gint          sel_kind;
    gint64        sel_id;
    gchar        *sel_name;
    gboolean      list_autofit;          /* list columns auto-size to their
                                            contents on every refresh (ini
                                            key "list_autofit")            */
    gboolean      notes_sel_blocked;     /* selection changes vetoed for
                                            the span of a press on an
                                            already-selected list row, so
                                            a drag keeps the whole
                                            multi-selection (quirk #15,
                                            still true on GTK4: D7)        */
    GtkTreePath  *notes_press_path;      /* the row that press landed on
                                            (owned): a plain click
                                            collapses to it on release     */
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
    GtkTreeView  *column_menu_view;      /* view the column menu is up for  */
    GtkWidget    *sidebar_paned;         /* horizontal paned holding the sidebar */
    guint         sb_fit_idle;           /* pending sidebar_fit_grow(), or 0;
                                            coalesces the row-expanded burst
                                            a model rebuild's re-expansion
                                            walk fires                      */
    GtkWidget    *status_path;
    GtkWidget    *status_event;
    GtkWidget    *status_revealer;
    guint         status_timeout;
    GtkWidget    *view_btn;            /* List/Grid toggle; icon names the
                                        * view a click switches TO           */
    GtkWidget    *ai_btn;              /* microchip AI toolbar button          */
    GtkWidget    *ai_pane;             /* output pane below the notes stack    */
    GtkWidget    *ai_text;             /* non-editable text view inside it     */
    guint         ai_throbber_id;       /* g_timeout_add id while AI running    */
    gint          ai_throbber_step;    /* animation frame counter              */
    GtkPaned     *notes_paned;         /* vertical paned: stack / AI pane      */
    gboolean      ai_running;          /* TRUE while subprocess is in flight   */
    GCancellable *ai_cancel;           /* cancels the in-flight subprocess     */
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
 *   row        — where to deliver the texture (owned; safely goes
 *                invalid if the model is rebuilt or the row removed).
 *   id         — the note to render.
 *   updated_at — its updated_at when the row was populated (cache key).
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkTreeRowReference *row;
    gint64               id;
    gint64               updated_at;
} ThumbJob;

/* thumb_job_free() — release one pending-thumbnail job.                     */
static void
thumb_job_free(ThumbJob *job)
{
    gtk_tree_row_reference_free(job->row);
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
static void    status_path_update(OnLibrary *lw);
static GArray *selected_note_ids(OnLibrary *lw);
static void    list_autofit_set(OnLibrary *lw, PangoLayout *lay,
                                const gchar *key, gint content_w);
static gboolean list_column_shown(OnLibrary *lw, const gchar *key);
static gint    list_autofit_time_width(PangoLayout *lay);
static GtkTreePath *notes_sel_unblock(OnLibrary *lw, gboolean want_path);
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
 * sb_folder_append() — add one folder row to the sidebar, formatting its
 * display text the one way folder rows are formatted: an optional emoji
 * prefix separated by two spaces, and an optional "(n)" note count.  Shared
 * by the normal tree and the Trash section, which differ only in SB_KIND.
 *   parent_iter — row to nest under (the Notes root, a folder, or Trash).
 *   f           — the folder.
 *   kind        — SB_KIND_FOLDER or SB_KIND_TRASH_FOLDER.
 *   note_counts — count map, or NULL while counts are hidden.
 *   iter        — receives the new row (for recursing into it); may be NULL.
 * ------------------------------------------------------------------------- */
static void
sb_folder_append(OnLibrary *lw, GtkTreeIter *parent_iter, const OnFolder *f,
                 gint kind, GHashTable *note_counts, GtkTreeIter *iter)
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

    GtkTreeIter local;               /* used when the caller passed NULL    */
    if (iter == NULL)
        iter = &local;
    gtk_tree_store_append(lw->sidebar_store, iter, parent_iter);
    gtk_tree_store_set(lw->sidebar_store, iter,
                       SB_KIND, kind,
                       SB_ID,   f->id,
                       SB_NAME, display,
                       SB_RAW,  f->name,
                       -1);
    g_free(display);
}

static void
add_folder_rows(OnLibrary *lw, gint64 parent_id, GtkTreeIter *parent_iter,
                GHashTable *note_counts, GHashTable *children)
{
    /* Borrowed from the pre-fetched child map — one query for the whole
     * tree, instead of one per folder as this recursion used to do.         */
    GList *folders = on_db_folder_children(children, parent_id);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder   *f = l->data;     /* one child folder                    */
        GtkTreeIter iter;            /* its new row, to recurse under       */
        sb_folder_append(lw, parent_iter, f, SB_KIND_FOLDER, note_counts,
                         &iter);
        add_folder_rows(lw, f->id, &iter, note_counts, children);
    }
    /* `folders` belongs to the child map — nothing to free here.            */
}

/* sb_row_key() — hashable identity of a sidebar row for state that must
 * survive a model rebuild (paths shift when folders move; kind+id don't). */
static gint64
sb_row_key(gint kind, gint64 id)
{
    return id * 16 + kind;
}

/* SbExpandCtx — working state for the expansion-capture walk below.        */
typedef struct {
    OnLibrary  *lw;
    GHashTable *expanded;                /* set of sb_row_key()s            */
} SbExpandCtx;

/* sb_expand_capture() — gtk_tree_model_foreach() callback: record the
 * key of every currently-expanded row.                                     */
static gboolean
sb_expand_capture(GtkTreeModel *model, GtkTreePath *path,
                  GtkTreeIter *iter, gpointer data)
{
    SbExpandCtx *ctx = data;
    if (gtk_tree_view_row_expanded(ctx->lw->sidebar, path)) {
        gint   kind;                 /* row kind                            */
        gint64 id;                   /* row id                              */
        gtk_tree_model_get(model, iter, SB_KIND, &kind, SB_ID, &id, -1);
        gint64 *key = g_new(gint64, 1);
        *key = sb_row_key(kind, id);
        g_hash_table_add(ctx->expanded, key);
    }
    return FALSE;
}

/* sb_reveal_path() — expand the ANCESTORS of `path` so the row itself is
 * visible (never the row itself: a collapsed selected folder stays
 * collapsed).                                                               */
static void
sb_reveal_path(GtkTreeView *view, GtkTreePath *path)
{
    GtkTreePath *parent = gtk_tree_path_copy(path);
    if (gtk_tree_path_up(parent) && gtk_tree_path_get_depth(parent) > 0)
        gtk_tree_view_expand_to_path(view, parent);
    gtk_tree_path_free(parent);
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the whole sidebar model: the folder tree
 * under the fixed "Notes" root, then the "Tags" section.  Attempts to
 * restore the previous selection by (kind, id), and puts back which rows
 * were expanded (only the very first population expands everything) —
 * a drop used to re-expand every folder because every successful drag
 * refreshes the sidebar.
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
     * rebuild) before the clear wipes it.                                  */
    SbExpandCtx ectx = {
        lw, g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                  g_free, NULL) };
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_expand_capture, &ectx);

    /* Clearing the store zeroes the sidebar scrollbar; a sidebar rebuild
     * is never a navigation (counts changed, a folder was added, …), so
     * the position is always put back.                                     */
    GtkAdjustment *vadj      = view_vadjustment(GTK_WIDGET(lw->sidebar));
    gdouble        scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    gtk_tree_store_clear(lw->sidebar_store);

    /* Batched counts: one query for all folders, one for all tags —
     * refresh_sidebar runs after every autosave, so per-row COUNT
     * queries added up (especially against a shared/networked db).
     * Built ONLY when the counts are actually displayed: every reader below
     * sits inside a `sidebar_counts` branch, so with the setting off (the
     * default) these two GROUP BY queries were run and thrown away on
     * every rebuild.                                                        */
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
        GtkTreeIter iter;
        gtk_tree_store_append(lw->sidebar_store, &iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &iter,
                           SB_KIND, SB_KIND_PINNED,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Pinned Notes",
                           -1);
        g_free(label);
    }

    /* "All Notes" — a live view of every note outside the Trash.           */
    gchar *all_label = lw->app->sidebar_counts
        ? g_strdup_printf("\xf0\x9f\x94\xae\xc2\xa0 All Notes (%d)",
                          on_db_note_count_visible(lw->app->db))
        : g_strdup("\xf0\x9f\x94\xae\xc2\xa0 All Notes");
    GtkTreeIter all_iter;            /* the fixed "All Notes" row           */
    gtk_tree_store_append(lw->sidebar_store, &all_iter, NULL);
    gtk_tree_store_set(lw->sidebar_store, &all_iter,
                       SB_KIND, SB_KIND_ALL,
                       SB_ID,   (gint64)0,
                       SB_NAME, all_label,
                       SB_RAW,  "All Notes",
                       -1);
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
        GtkTreeIter iter;
        gtk_tree_store_append(lw->sidebar_store, &iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &iter,
                           SB_KIND, SB_KIND_ACTIONS,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Action Items",
                           -1);
        g_free(label);
    }

    /* "Notes" root — selecting it shows the top-level notes.               */
    gchar *root_label = lw->app->sidebar_counts
        ? g_strdup_printf("\xf0\x9f\x93\x93\xc2\xa0 Notes (%d)",
                          count_from_map(note_counts, 0))
        : g_strdup("\xf0\x9f\x93\x93\xc2\xa0 Notes");
    GtkTreeIter root;                /* the fixed root row                  */
    gtk_tree_store_append(lw->sidebar_store, &root, NULL);
    gtk_tree_store_set(lw->sidebar_store, &root,
                       SB_KIND, SB_KIND_ROOT,
                       SB_ID,   (gint64)0,
                       SB_NAME, root_label,
                       SB_RAW,  "Notes",
                       -1);
    g_free(root_label);
    add_folder_rows(lw, 0, &root, note_counts, children);

    /* "Tags" header + one row per known tag.                               */
    GList *tags = on_db_tag_list(lw->app->db);
    if (tags != NULL) {
        GtkTreeIter header;          /* the "Tags" section row              */
        gtk_tree_store_append(lw->sidebar_store, &header, NULL);
        gtk_tree_store_set(lw->sidebar_store, &header,
                           SB_KIND, SB_KIND_TAGS_HEADER,
                           SB_ID,   (gint64)0,
                           SB_NAME, "\xf0\x9f\x8f\xb7\xef\xb8\x8f\xc2\xa0 Tags",
                           SB_RAW,  "Tags",
                           -1);
        for (GList *l = tags; l != NULL; l = l->next) {
            OnTag *t = l->data;      /* one tag                             */
            gchar *raw   = g_strdup_printf("#%s", t->name);
            gchar *label = lw->app->sidebar_counts
                ? g_strdup_printf("#%s (%d)", t->name,
                                  count_from_map(tag_counts, t->id))
                : g_strdup(raw);
            GtkTreeIter iter;
            gtk_tree_store_append(lw->sidebar_store, &iter, &header);
            gtk_tree_store_set(lw->sidebar_store, &iter,
                               SB_KIND, SB_KIND_TAG,
                               SB_ID,   t->id,
                               SB_NAME, label,
                               SB_RAW,  raw,
                               -1);
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
        GtkTreeIter trash_iter;      /* the "Trash" section row             */
        gtk_tree_store_append(lw->sidebar_store, &trash_iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &trash_iter,
                           SB_KIND, SB_KIND_TRASH,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Trash",
                           -1);
        g_free(label);

        GList *trashed = on_db_folder_list_trashed(lw->app->db);
        for (GList *l = trashed; l != NULL; l = l->next)
            sb_folder_append(lw, &trash_iter, l->data,
                             SB_KIND_TRASH_FOLDER, note_counts, NULL);
        on_db_folder_list_free(trashed);
    }

    on_db_folder_child_map_free(children);
    if (note_counts != NULL)
        g_hash_table_destroy(note_counts);
    if (tag_counts != NULL)
        g_hash_table_destroy(tag_counts);

    /* Every rebuild (including the first) restores the captured expansion
     * state in the walk below; first launch has nothing captured so all
     * folders stay collapsed.                                               */

    /* Restore the previous selection, falling back to "All Notes".  The
     * populating guard stays up through the restore: the select_iter
     * below would otherwise fire the changed handler and rebuild the
     * notes pane a second time — every refresh_sidebar caller already
     * pairs it with an explicit refresh_notes.                             */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(lw->sidebar);
    GtkTreeIter iter;                /* candidate row while searching       */
    gboolean restored = FALSE;       /* did we find the old selection?      */

    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(lw->sidebar_store), &iter);
    /* Depth-first walk of the whole sidebar model.                         */
    GQueue queue = G_QUEUE_INIT;     /* pending iters (BFS is fine too)     */
    while (valid || !g_queue_is_empty(&queue)) {
        if (!valid) {
            GtkTreeIter *q = g_queue_pop_head(&queue);
            iter = *q;
            g_free(q);
            valid = TRUE;
        }
        gint   kind;                 /* row kind                            */
        gint64 id;                   /* row id                              */
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, SB_ID, &id, -1);
        GtkTreePath *row_path = gtk_tree_model_get_path(
            GTK_TREE_MODEL(lw->sidebar_store), &iter);

        /* Re-expand rows that were expanded before the rebuild.            */
        gint64 ekey = sb_row_key(kind, id);
        if (g_hash_table_contains(ectx.expanded, &ekey))
            gtk_tree_view_expand_row(lw->sidebar, row_path, FALSE);

        if (kind == want_kind && id == want_id && !restored) {
            /* The suppressed handler would have refreshed sel_name; do it
             * here so a renamed folder/tag keeps it current.               */
            gchar *raw = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                               SB_RAW, &raw, -1);
            g_free(lw->sel_name);
            lw->sel_name = raw;      /* ownership transferred               */
            sb_reveal_path(lw->sidebar, row_path);
            gtk_tree_selection_select_iter(sel, &iter);
            restored = TRUE;         /* no break: the walk must finish
                                        restoring the expansion state       */
        }
        gtk_tree_path_free(row_path);
        GtkTreeIter child;           /* first child, if any                 */
        if (gtk_tree_model_iter_children(GTK_TREE_MODEL(lw->sidebar_store),
                                         &child, &iter)) {
            GtkTreeIter *copy = g_new(GtkTreeIter, 1);
            *copy = child;
            g_queue_push_tail(&queue, copy);
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(lw->sidebar_store),
                                         &iter);
    }
    g_queue_clear_full(&queue, g_free);
    g_hash_table_destroy(ectx.expanded);

    if (!restored) {
        /* Fall back to the first row (Pinned Notes when any are pinned,
         * All Notes otherwise) — the state must match the row the
         * fallback highlights, so read it from the model.                  */
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(lw->sidebar_store), &iter)) {
            gint   kind;             /* the first row's identity            */
            gint64 id;
            gchar *raw = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                               SB_KIND, &kind, SB_ID, &id, SB_RAW, &raw,
                               -1);
            lw->sel_kind = kind;
            lw->sel_id   = id;
            g_free(lw->sel_name);
            lw->sel_name = raw;      /* ownership transferred               */
            gtk_tree_selection_select_iter(sel, &iter);
        }
    }
    lw->populating--;

    /* The old selection no longer exists (deleted folder/pruned tag), so
     * the notes pane still shows its contents: refresh for the new
     * fallback selection.  When the selection was restored, the caller's
     * own refresh_notes covers it.                                         */
    if (!restored)
        refresh_notes(lw);

    if (scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
}

/* ===========================================================================
 * notes pane population + grid thumbnails
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * render_note_thumb() — draw a square THUMB_SIZE card for one note: its
 * first embedded image (if any) above the beginning of its body text,
 * returned as a GdkTexture of exactly THUMB_SIZE pixels.  The size is
 * deliberately the LOGICAL one: GtkCellRendererPixbuf lays a texture out
 * at its pixel width (gdk_paintable_get_intrinsic_width), so a card
 * rendered at scale-factor resolution would draw twice as large on a
 * HiDPI display, not sharper.  The title is NOT drawn here — the grid
 * shows it as a real text label under the card.
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

    /* Draw with cairo at the card's pixel size (see the banner comment on
     * why it is not the scale-factor size).                                */
    const gint SZ = THUMB_SIZE;      /* square edge length in pixels        */
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, SZ, SZ);
    cairo_t *cr = cairo_create(surface);

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

        cairo_save(cr);
        cairo_translate(cr, (SZ - dw) / 2.0, y);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr),
                                 CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
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
                                 (gsize)stride * SZ);
    GdkTexture *texture = gdk_memory_texture_new(SZ, SZ, GDK_MEMORY_DEFAULT,
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
 * deliver each into its grid row.  Jobs whose row vanished (model rebuilt
 * mid-fill) are simply dropped — the rebuild queued fresh jobs.
 * ------------------------------------------------------------------------- */
static gboolean
thumb_fill_idle(gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 slice_start = g_get_monotonic_time();

    while (!g_queue_is_empty(&lw->thumb_pending)) {
        ThumbJob *job = g_queue_pop_head(&lw->thumb_pending);
        GtkTreePath *path = gtk_tree_row_reference_valid(job->row)
            ? gtk_tree_row_reference_get_path(job->row)
            : NULL;
        if (path != NULL) {
            GdkTexture *thumb =          /* borrowed from the cache         */
                get_note_thumb(lw, job->id, job->updated_at);
            GtkTreeIter iter;            /* the row to update               */
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                        &iter, path))
                gtk_list_store_set(lw->notes_store, &iter,
                                   NL_THUMB, thumb, -1);
            gtk_tree_path_free(path);
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
    GtkWidget *sw =                  /* the view's scrolled window          */
        gtk_widget_get_parent(GTK_WIDGET(lw->actions_view));
    GtkAdjustment *vadj = GTK_IS_SCROLLED_WINDOW(sw)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw))
        : NULL;
    gdouble scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    gtk_list_store_clear(lw->actions_store);
    GList *items = on_db_action_list(lw->app->db);
    for (GList *l = items; l != NULL; l = l->next) {
        OnActionItem *it = l->data;  /* one action item                     */
        if (it->done && !lw->app->show_done_actions)
            continue;                /* Settings: hide completed items      */
        gchar *when = NULL;          /* formatted due date, or NULL         */
        if (it->due != 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(it->due);
            when = g_date_time_format(dt, "%b %e, %Y");
            g_date_time_unref(dt);
        }
        GtkTreeIter iter;
        gtk_list_store_append(lw->actions_store, &iter);
        gtk_list_store_set(lw->actions_store, &iter,
                           AL_NOTE_ID, it->note_id,
                           AL_ORD,     it->ord,
                           AL_DONE,    it->done,
                           AL_TEXT,    it->text,
                           AL_DUE,     when != NULL ? when : "",
                           AL_DUE_RAW, it->due,
                           -1);
        g_free(when);
    }
    on_db_action_list_free(items);
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
 * close), not a navigation — the scroll position is preserved.
 * ------------------------------------------------------------------------- */
static void
refresh_notes(OnLibrary *lw)
{
    /* Whatever happens below, the rows any queued thumbnail jobs point
     * at are stale (or about to be cleared): drop them.                    */
    thumb_pending_clear(lw);

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
     * store rebuild.  Navigations start with no selection by design.        */
    GArray *sel_ids = keep_scroll ? selected_note_ids(lw) : NULL;

    lw->populating++;
    gtk_list_store_clear(lw->notes_store);

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
     * it, and the grid draws thumbnails and the title, never NL_PREVIEW.    */
    GHashTable *previews = (lw->app->comfortable_list && !want_thumbs)
        ? on_db_note_text_map(lw->app->db, NL_PREVIEW_CHARS) : NULL;

    /* Autofit measuring rides this population loop (no second model
     * walk, no re-fetching the strings) and only while the LIST is the
     * visible view — the grid doesn't show these columns, and switching
     * back to list re-measures (on_view_list).  Repeated folder paths
     * are measured once, keyed by folder id.
     *
     * Only VISIBLE columns are measured: list_autofit_set() throws away the
     * width of a hidden one, so measuring it was pure waste — and Path and
     * Created are hidden by default.  The two timestamp columns are not
     * measured here at all: every value shares LIST_TIME_FORMAT, so
     * list_autofit_time_width() bounds them once after the loop instead of
     * once per row (measured ~68 ms for 1300 rows, on every refresh).       */
    gboolean fit      = lw->list_autofit && !want_thumbs;
    gboolean fit_path = fit && list_column_shown(lw, "path");
    gboolean fit_mod  = fit && list_column_shown(lw, "modified");
    gboolean fit_cre  = fit && list_column_shown(lw, "created");
    PangoLayout *fit_lay  = NULL;    /* reused measuring layout             */
    GHashTable  *fit_seen = NULL;    /* folder id → measured path width     */
    gint fit_path_w = 0;             /* running Path content maximum        */
    if (fit)
        fit_lay = gtk_widget_create_pango_layout(
            GTK_WIDGET(lw->notes_list), NULL);
    if (fit_path)
        fit_seen = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, NULL);

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */

        /* Format the modification and creation times like
         * "Jun 3, 2026 14:05".                                             */
        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar *when = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);
        dt = g_date_time_new_from_unix_local(m->created_at);
        gchar *born = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);

        /* "/Folder/Sub" location, "/" for the top level — the same
         * format as the status bar's path label.                           */
        const gchar *fpath = m->folder_id != 0
            ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
        gchar *where = g_strdup_printf("/%s", fpath != NULL ? fpath : "");

        if (fit_path) {
            gint w;                  /* width of this note's path           */
            gpointer cached =
                g_hash_table_lookup(fit_seen, &m->folder_id);
            if (cached != NULL) {
                w = GPOINTER_TO_INT(cached);
            } else {
                pango_layout_set_text(fit_lay, where, -1);
                pango_layout_get_pixel_size(fit_lay, &w, NULL);
                gint64 *k = g_new(gint64, 1);
                *k = m->folder_id;
                g_hash_table_insert(fit_seen, k, GINT_TO_POINTER(w));
            }
            fit_path_w = MAX(fit_path_w, w);
        }

        /* Thumbnails: only what the cache already has goes in right away
         * — a stale entry still shows (better than a blank card) while
         * thumb_fill_idle (queued below) renders the replacement.
         * Rendering every stale/missing thumbnail here froze the GUI.      */
        GdkTexture *thumb = NULL;        /* borrowed from the cache         */
        gboolean thumb_todo = FALSE;     /* queue a render for this row?    */
        if (want_thumbs) {
            ThumbEntry *e = g_hash_table_lookup(lw->thumb_cache, &m->id);
            if (e != NULL)
                thumb = e->texture;
            thumb_todo = e == NULL || e->updated_at != m->updated_at;
        }

        gchar *preview = notes_preview_line(
            previews ? g_hash_table_lookup(previews, &m->id) : NULL);

        GtkTreeIter iter;
        gtk_list_store_append(lw->notes_store, &iter);
        gtk_list_store_set(lw->notes_store, &iter,
                           NL_ID,          m->id,
                           NL_TITLE,       m->title,
                           NL_MODIFIED,    when,
                           NL_THUMB,       thumb,
                           NL_UPDATED,     m->updated_at,
                           NL_PATH,        where,
                           NL_CREATED,     born,
                           NL_CREATED_RAW, m->created_at,
                           NL_PREVIEW,     preview,
                           -1);
        g_free(preview);
        if (thumb_todo) {
            GtkTreePath *path = gtk_tree_model_get_path(
                GTK_TREE_MODEL(lw->notes_store), &iter);
            ThumbJob *job = g_new0(ThumbJob, 1);
            job->row = gtk_tree_row_reference_new(
                GTK_TREE_MODEL(lw->notes_store), path);
            job->id         = m->id;
            job->updated_at = m->updated_at;
            g_queue_push_tail(&lw->thumb_pending, job);
            gtk_tree_path_free(path);
        }
        g_free(where);
        g_free(when);
        g_free(born);
    }
    /* paths == lw->folder_path_cache — kept alive for the next refresh.     */
    if (previews != NULL) g_hash_table_destroy(previews);
    on_db_note_list_free(notes);

    if (fit) {
        /* Both timestamp columns render the same format, so one bound
         * serves both.  Computed only if one of them is actually shown.     */
        gint time_w = (fit_mod || fit_cre)
                      ? list_autofit_time_width(fit_lay) : 0;
        if (fit_path)
            list_autofit_set(lw, fit_lay, "path",     fit_path_w);
        if (fit_mod)
            list_autofit_set(lw, fit_lay, "modified", time_w);
        if (fit_cre)
            list_autofit_set(lw, fit_lay, "created",  time_w);
        if (fit_seen != NULL)
            g_hash_table_destroy(fit_seen);
        g_object_unref(fit_lay);
    }
    lw->populating--;

    /* Restore the note selection that existed before the store rebuild.     */
    if (sel_ids != NULL) {
        if (sel_ids->len > 0) {
            const gchar *mode = gtk_stack_get_visible_child_name(
                GTK_STACK(lw->stack));
            gboolean in_grid  = g_strcmp0(mode, "grid") == 0;
            GtkTreeSelection *list_sel =
                gtk_tree_view_get_selection(lw->notes_list);
            GtkTreeIter it;
            gboolean valid = gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(lw->notes_store), &it);
            while (valid) {
                gint64 id;
                gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &it,
                                   NL_ID, &id, -1);
                for (guint k = 0; k < sel_ids->len; k++) {
                    if (g_array_index(sel_ids, gint64, k) == id) {
                        if (in_grid) {
                            GtkTreePath *p = gtk_tree_model_get_path(
                                GTK_TREE_MODEL(lw->notes_store), &it);
                            gtk_icon_view_select_path(lw->notes_grid, p);
                            gtk_tree_path_free(p);
                        } else {
                            gtk_tree_selection_select_iter(list_sel, &it);
                        }
                        break;
                    }
                }
                valid = gtk_tree_model_iter_next(
                    GTK_TREE_MODEL(lw->notes_store), &it);
            }
        }
        g_array_free(sel_ids, TRUE);
    }

    /* Render the queued (stale/missing) thumbnails in idle time slices.    */
    if (!g_queue_is_empty(&lw->thumb_pending) && lw->thumb_idle == 0)
        lw->thumb_idle = g_idle_add(thumb_fill_idle, lw);

    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);

    status_path_update(lw);
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

/* ---------------------------------------------------------------------------
 * on_sidebar_selection_changed() — a folder or tag was selected: remember
 * it and refresh the notes pane.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_selection_changed(GtkTreeSelection *sel, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating > 0)
        return;

    GtkTreeModel *model;             /* the sidebar model                   */
    GtkTreeIter iter;                /* selected row                        */
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;

    gint   kind;                     /* selected row kind                   */
    gint64 id;                       /* selected row id                     */
    gchar *raw = NULL;               /* bare name of the row                */
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, SB_ID, &id,
                       SB_RAW, &raw, -1);
    if (kind == SB_KIND_TAGS_HEADER) {
        g_free(raw);
        return;                      /* header row: not a real selection    */
    }

    lw->sel_kind = kind;
    lw->sel_id   = id;
    g_free(lw->sel_name);
    lw->sel_name = raw;              /* ownership transferred               */
    refresh_notes(lw);
}

/* sidebar_select_func() — forbid selecting the "Tags" header row.           */
static gboolean
sidebar_select_func(GtkTreeSelection *sel, GtkTreeModel *model,
                    GtkTreePath *path, gboolean currently_selected,
                    gpointer user_data)
{
    (void)sel; (void)currently_selected; (void)user_data;
    GtkTreeIter iter;                /* row being (de)selected              */
    gint kind;                       /* its kind                            */
    gtk_tree_model_get_iter(model, &iter, path);
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    return kind != SB_KIND_TAGS_HEADER;
}

/* ---------------------------------------------------------------------------
 * open_note_at_path() — open the editor for the note at `path` (NOT
 * owned) in the notes model; the shared tail of both views' activation
 * handlers.
 * ------------------------------------------------------------------------- */
static void
open_note_at_path(OnLibrary *lw, GtkTreePath *path)
{
    GtkTreeIter iter;                /* activated row                       */
    gint64 id;                       /* its note id                         */
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                 &iter, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                       NL_ID, &id, -1);
    on_editor_window_open(lw->app, id);
}

/* on_note_list_activated() — double-click/Enter in list mode opens it.      */
static void
on_note_list_activated(GtkTreeView *view, GtkTreePath *path,
                       GtkTreeViewColumn *col, gpointer user_data)
{
    (void)view; (void)col;
    open_note_at_path(user_data, path);
}

/* on_note_grid_activated() — double-click in grid mode opens the note.      */
static void
on_note_grid_activated(GtkIconView *view, GtkTreePath *path,
                       gpointer user_data)
{
    (void)view;
    open_note_at_path(user_data, path);
}

/* ---------------------------------------------------------------------------
 * on_action_toggled() — the Action Items checkbox column: flip the item's
 * done state everywhere — the model row (instant feedback), its
 * action_items row, and the note text itself (strikethrough) via
 * on_editor_action_set_done.
 * ------------------------------------------------------------------------- */
static void
on_action_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                  gpointer user_data)
{
    (void)cell;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeIter iter;                /* the clicked row                     */
    if (!gtk_tree_model_get_iter_from_string(
            GTK_TREE_MODEL(lw->actions_store), &iter, path_str))
        return;

    gint64   note_id;                /* the item's address                  */
    gint     ord;
    gboolean done;
    gtk_tree_model_get(GTK_TREE_MODEL(lw->actions_store), &iter,
                       AL_NOTE_ID, &note_id,
                       AL_ORD,     &ord,
                       AL_DONE,    &done,
                       -1);
    done = !done;

    /* A just-completed item disappears immediately when completed items
     * are hidden; otherwise the row simply re-renders checked + struck.    */
    if (done && !lw->app->show_done_actions)
        gtk_list_store_remove(lw->actions_store, &iter);
    else
        gtk_list_store_set(lw->actions_store, &iter, AL_DONE, done, -1);
    /* The content rewrite is authoritative and normally rebuilds the
     * mirror itself; only a LIVE editor defers that to its autosave, and
     * only then does the flag need writing here as well.                    */
    gboolean synced = FALSE;         /* did the rewrite update the table?   */
    if (on_editor_action_set_done(lw->app, note_id, ord, done, &synced) &&
        !synced)
        on_db_action_set_done(lw->app->db, note_id, ord, done);
    if (lw->app->sidebar_counts)
        refresh_sidebar(lw);         /* the section's open count changed    */
}

/* ---------------------------------------------------------------------------
 * DueDialog — what the due-date dialog's response callback needs, carried
 * as object data on the dialog.
 *
 * Fields:
 *   lw       — the library window.
 *   row      — the Action Items row the date is for (owned; goes invalid
 *              if the model is rebuilt while the dialog is up).
 *   note_id/
 *   ord      — the item's address in the note.
 *   cal      — the GtkCalendar in the dialog.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnLibrary           *lw;
    GtkTreeRowReference *row;
    gint64               note_id;
    gint                 ord;
    GtkWidget           *cal;
} DueDialog;

/* due_dialog_free() — GDestroyNotify for the DueDialog on the dialog.       */
static void
due_dialog_free(gpointer data)
{
    DueDialog *d = data;
    gtk_tree_row_reference_free(d->row);
    g_free(d);
}

/* ---------------------------------------------------------------------------
 * on_due_response() — the due-date dialog closed.  Set rewrites the
 * "due YYYY-MM-DD" suffix of the '!' line in the note text
 * (on_editor_action_set_due), Clear removes it; the store row updates
 * immediately, the durable action_items row follows from the content
 * rewrite.  A row that vanished meanwhile (the model was rebuilt under the
 * dialog) is covered by a repopulate, which reads the rewritten mirror.
 * ------------------------------------------------------------------------- */
static void
on_due_response(GtkDialog *dlg, gint response, gpointer user_data)
{
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
        on_editor_action_set_due(lw->app, d->note_id, d->ord, new_due)) {
        GtkTreePath *path = gtk_tree_row_reference_get_path(d->row);
        GtkTreeIter  iter;           /* the row, if it still exists         */
        if (path != NULL &&
            gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->actions_store),
                                    &iter, path)) {
            gchar *when = NULL;      /* formatted for the Due Date cell     */
            if (new_due != 0) {
                GDateTime *dt = g_date_time_new_from_unix_local(new_due);
                when = g_date_time_format(dt, "%b %e, %Y");
                g_date_time_unref(dt);
            }
            gtk_list_store_set(lw->actions_store, &iter,
                               AL_DUE,     when != NULL ? when : "",
                               AL_DUE_RAW, new_due,
                               -1);
            g_free(when);
        } else {
            refresh_notes(lw);
        }
        gtk_tree_path_free(path);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

/* ---------------------------------------------------------------------------
 * action_due_dialog() — modal calendar for one action item's due date;
 * on_due_response applies the choice.
 *   lw   — the library window.
 *   path — the Action Items row (NOT owned).
 * ------------------------------------------------------------------------- */
static void
action_due_dialog(OnLibrary *lw, GtkTreePath *path)
{
    GtkTreeModel *model = GTK_TREE_MODEL(lw->actions_store);
    GtkTreeIter iter;                /* the row                             */
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    DueDialog *d = g_new0(DueDialog, 1);
    d->lw  = lw;
    d->row = gtk_tree_row_reference_new(model, path);
    gint64 due;                      /* the item's current due date         */
    gtk_tree_model_get(model, &iter,
                       AL_NOTE_ID, &d->note_id,
                       AL_ORD,     &d->ord,
                       AL_DUE_RAW, &due,
                       -1);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Notes - Due Date", GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Clear",  1,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Set",    GTK_RESPONSE_OK,
        NULL);
    gtk_widget_add_css_class(dlg, "notes-dialog");   /* library_install_css */
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    d->cal = gtk_calendar_new();
    if (due != 0) {                  /* open on the current due date        */
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        gtk_calendar_select_day(GTK_CALENDAR(d->cal), dt);
        g_date_time_unref(dt);
    }
    gtk_widget_set_margin_start(d->cal, 8);
    gtk_widget_set_margin_end(d->cal, 8);
    gtk_widget_set_margin_top(d->cal, 8);
    gtk_widget_set_margin_bottom(d->cal, 8);
    gtk_widget_set_vexpand(d->cal, TRUE);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                   d->cal);

    g_object_set_data_full(G_OBJECT(dlg), "on-due", d, due_dialog_free);
    g_signal_connect(dlg, "response", G_CALLBACK(on_due_response), d);
    gtk_window_present(GTK_WINDOW(dlg));
}

/* on_action_row_activated() — double-click: the Due Date cell opens the
 * date selector; any other cell opens the item's note.                      */
static void
on_action_row_activated(GtkTreeView *view, GtkTreePath *path,
                        GtkTreeViewColumn *column, gpointer user_data)
{
    (void)view;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeIter iter;                /* the activated row                   */
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->actions_store),
                                 &iter, path))
        return;

    if (column != NULL &&
        g_strcmp0(g_object_get_data(G_OBJECT(column), "on-colkey"),
                  "due") == 0) {
        action_due_dialog(lw, path);
        return;
    }

    gint64 note_id;                  /* owning note                         */
    gtk_tree_model_get(GTK_TREE_MODEL(lw->actions_store), &iter,
                       AL_NOTE_ID, &note_id, -1);
    GtkWidget *win = on_editor_window_open(lw->app, note_id);
    if (win != NULL)
        gtk_window_present(GTK_WINDOW(win));
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

/* ---------------------------------------------------------------------------
 * SbFindCtx — working state for sb_find_row()'s model walk.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint        kind;                /* SB_KIND_* wanted                    */
    gint64      id;                  /* SB_ID wanted                        */
    GtkTreeIter iter;                /* the hit, when found                 */
    gboolean    found;
} SbFindCtx;

/* sb_find_cb() — gtk_tree_model_foreach() callback for sb_find_row().      */
static gboolean
sb_find_cb(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
           gpointer data)
{
    (void)path;
    SbFindCtx *ctx = data;
    gint   kind;                     /* this row's identity                 */
    gint64 id;
    gtk_tree_model_get(model, iter, SB_KIND, &kind, SB_ID, &id, -1);
    if (kind == ctx->kind && id == ctx->id) {
        ctx->iter  = *iter;
        ctx->found = TRUE;
    }
    return ctx->found;               /* TRUE stops the walk                 */
}

/* ---------------------------------------------------------------------------
 * sb_find_row() — the sidebar row carrying (kind, id).  A drag names its
 * folder by id (OnDragRows); the validation and the drop need its ROW, to
 * refuse a drop onto itself or into its own subtree.
 *   lw   — the library window.
 *   kind — SB_KIND_FOLDER or SB_KIND_TRASH_FOLDER.
 *   id   — the folder id.
 *   iter — receives the row.
 * Returns TRUE if the row exists.
 * ------------------------------------------------------------------------- */
static gboolean
sb_find_row(OnLibrary *lw, gint kind, gint64 id, GtkTreeIter *iter)
{
    SbFindCtx ctx = { kind, id, { 0 }, FALSE };
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store), sb_find_cb,
                           &ctx);
    if (ctx.found)
        *iter = ctx.iter;
    return ctx.found;
}

/* ---------------------------------------------------------------------------
 * sidebar_drop_target() — resolve and validate the drop target under the
 * pointer for a drag carrying `rows`.  Which rows are legal depends on
 * what is being dragged: a folder (from the sidebar itself) goes onto
 * folders, the root, or the Trash, never onto itself or into its own
 * subtree — and a folder already in the Trash cannot be dropped on Trash
 * again; notes (from either notes view) go onto any folder-ish row, with
 * the position coerced to INTO (a note drops *into* a folder, never
 * beside it).
 *   lw       — the library window.
 *   rows     — the drag content, or NULL (not loaded: refuse).
 *   x, y     — pointer position in the sidebar's widget coordinates.
 *   path_out — receives the target row (caller frees) when legal.
 *   pos_out  — receives the drop position when legal.
 *   src_iter — receives the dragged FOLDER's own row (folder drags only).
 * Returns TRUE when the drop is legal.
 * ------------------------------------------------------------------------- */
static gboolean
sidebar_drop_target(OnLibrary *lw, const OnDragRows *rows, gint x, gint y,
                    GtkTreePath **path_out, GtkTreeViewDropPosition *pos_out,
                    GtkTreeIter *src_iter)
{
    *path_out = NULL;
    *pos_out  = GTK_TREE_VIEW_DROP_BEFORE;
    if (rows == NULL)
        return FALSE;

    GtkTreePath *path = NULL;        /* row under the pointer               */
    GtkTreeViewDropPosition pos;     /* before/into/after                   */
    if (!gtk_tree_view_get_dest_row_at_pos(lw->sidebar, x, y, &path, &pos))
        return FALSE;

    GtkTreeModel *model = GTK_TREE_MODEL(lw->sidebar_store);
    GtkTreeIter iter;                /* target row                          */
    gint kind = -1;                  /* its kind                            */
    if (gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);

    gboolean ok = FALSE;             /* is this drop legal?                 */
    if (rows->kind == ON_DRAG_NOTES) {
        if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT ||
            kind == SB_KIND_TRASH) {
            ok  = TRUE;
            pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
        }
    } else {
        gint src_kind = (rows->kind == ON_DRAG_FOLDER)
            ? SB_KIND_FOLDER : SB_KIND_TRASH_FOLDER;
        if (sb_find_row(lw, src_kind, g_array_index(rows->ids, gint64, 0),
                        src_iter)) {
            GtkTreePath *src_path = gtk_tree_model_get_path(model, src_iter);
            if (gtk_tree_path_compare(src_path, path) != 0 &&
                !gtk_tree_path_is_descendant(path, src_path)) {
                if (kind == SB_KIND_FOLDER) {
                    ok = TRUE;       /* nest INTO or reorder beside it      */
                } else if (kind == SB_KIND_ROOT ||
                           (kind == SB_KIND_TRASH &&
                            src_kind == SB_KIND_FOLDER)) {
                    ok  = TRUE;
                    pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
                }
            }
            gtk_tree_path_free(src_path);
        }
    }

    if (ok) {
        *path_out = path;
        *pos_out  = pos;
    } else {
        gtk_tree_path_free(path);
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drop_motion() — GtkDropTarget "enter" AND "motion" (same
 * signature, one handler): validate the row under the pointer against the
 * drag's content and draw the indicator ourselves.  The content is
 * readable here because the target PRELOADS it and every drag is local:
 * for a local drag GtkDropTarget reads the value synchronously from the
 * content provider when the drop starts, so it is never NULL by the first
 * motion (gtkdroptarget.c, gtk_drop_target_load_local).
 * Returns the action offered (MOVE), or 0 to refuse.
 * ------------------------------------------------------------------------- */
static GdkDragAction
on_sidebar_drop_motion(GtkDropTarget *target, gdouble x, gdouble y,
                       gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    const GValue *value = gtk_drop_target_get_value(target);
    const OnDragRows *rows =         /* the drag's content, once loaded     */
        (value != NULL) ? g_value_get_boxed(value) : NULL;
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* indicator position                  */
    GtkTreeIter src_iter;            /* unused here                         */
    gboolean ok = sidebar_drop_target(lw, rows, (gint)x, (gint)y,
                                      &path, &pos, &src_iter);

    gtk_tree_view_set_drag_dest_row(lw->sidebar, ok ? path : NULL, pos);
    if (path != NULL)
        gtk_tree_path_free(path);
    return ok ? GDK_ACTION_MOVE : 0;
}

/* on_sidebar_drop_leave() — clear the drop indicator.                       */
static void
on_sidebar_drop_leave(GtkDropTarget *target, gpointer user_data)
{
    (void)target;
    OnLibrary *lw = user_data;       /* owning library window               */
    gtk_tree_view_set_drag_dest_row(lw->sidebar, NULL,
                                    GTK_TREE_VIEW_DROP_BEFORE);
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
 * on_sidebar_drag_prepare() — GtkDragSource "prepare" on the sidebar: a
 * folder row (in the tree or under Trash) starts a drag carrying its id;
 * any other row refuses, since nothing accepts it.
 *   x, y — the press, in the sidebar's widget coordinates.
 * Returns the drag content, or NULL for no drag.
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
on_sidebar_drag_prepare(GtkDragSource *source, gdouble x, gdouble y,
                        gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint bx, by;                     /* the press in bin-window coordinates */
    gtk_tree_view_convert_widget_to_bin_window_coords(lw->sidebar,
                                                      (gint)x, (gint)y,
                                                      &bx, &by);
    GtkTreePath *path = NULL;        /* row under the press                 */
    if (!gtk_tree_view_get_path_at_pos(lw->sidebar, bx, by, &path,
                                       NULL, NULL, NULL))
        return NULL;

    GtkTreeIter iter;                /* that row                            */
    gint   kind = -1;                /* its kind                            */
    gint64 id   = 0;                 /* its folder id                       */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                                path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, SB_ID, &id, -1);
    gtk_tree_path_free(path);
    if (kind != SB_KIND_FOLDER && kind != SB_KIND_TRASH_FOLDER)
        return NULL;

    OnDragRows *rows = on_drag_rows_new(kind == SB_KIND_FOLDER
                                        ? ON_DRAG_FOLDER
                                        : ON_DRAG_TRASHED_FOLDER);
    g_array_append_val(rows->ids, id);
    return drag_rows_content(lw, source, rows);
}

/* ---------------------------------------------------------------------------
 * notes_drag_rows() — the content of a note drag that started on the row
 * at `path` (owned — freed here): the whole selection when the pressed
 * note is part of it (multi-select drags), else just that note.
 * Returns a new OnDragRows.
 * ------------------------------------------------------------------------- */
static OnDragRows *
notes_drag_rows(OnLibrary *lw, GtkTreePath *path)
{
    GtkTreeIter iter;                /* the pressed row                     */
    gint64 note_id = 0;              /* its note id                         */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store), &iter,
                                path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &note_id, -1);
    gtk_tree_path_free(path);

    OnDragRows *rows = on_drag_rows_new(ON_DRAG_NOTES);
    GArray *sel = selected_note_ids(lw);
    gboolean in_selection = FALSE;   /* is the pressed note selected?       */
    for (guint i = 0; i < sel->len; i++)
        if (g_array_index(sel, gint64, i) == note_id)
            in_selection = TRUE;
    if (in_selection)
        g_array_append_vals(rows->ids, sel->data, sel->len);
    else
        g_array_append_val(rows->ids, note_id);
    g_array_free(sel, TRUE);
    return rows;
}

/* ---------------------------------------------------------------------------
 * on_notes_list_drag_prepare() — GtkDragSource "prepare" on the notes
 * list.  A drag from a vetoed press (quirk #15 / D7) keeps the whole
 * multi-selection: the veto is lifted here, and the matching release
 * lands in the DnD machinery, never as a click on the view.
 * Returns the drag content, or NULL when the press missed every row.
 * ------------------------------------------------------------------------- */
static GdkContentProvider *
on_notes_list_drag_prepare(GtkDragSource *source, gdouble x, gdouble y,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint bx, by;                     /* the press in bin-window coordinates */
    gtk_tree_view_convert_widget_to_bin_window_coords(lw->notes_list,
                                                      (gint)x, (gint)y,
                                                      &bx, &by);
    GtkTreePath *path = NULL;        /* row under the press                 */
    if (!gtk_tree_view_get_path_at_pos(lw->notes_list, bx, by, &path,
                                       NULL, NULL, NULL))
        return NULL;
    notes_sel_unblock(lw, FALSE);
    return drag_rows_content(lw, source, notes_drag_rows(lw, path));
}

/* ---------------------------------------------------------------------------
 * grid_path_at() — the grid item under a point given in the icon view's
 * WIDGET coordinates (what every controller hands over).  GTK4's
 * gtk_icon_view_get_path_at_pos() works in the scrolled content's
 * coordinates, so the adjustments are added first — GTK's own gesture does
 * the same (gtkiconview.c, _gtk_icon_view_get_item_at_widget_coords).
 * Returns the item's path (caller frees), or NULL.
 * ------------------------------------------------------------------------- */
static GtkTreePath *
grid_path_at(OnLibrary *lw, gdouble x, gdouble y)
{
    GtkScrollable *s = GTK_SCROLLABLE(lw->notes_grid);
    gdouble cx = x + gtk_adjustment_get_value(gtk_scrollable_get_hadjustment(s));
    gdouble cy = y + gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(s));
    return gtk_icon_view_get_path_at_pos(lw->notes_grid, (gint)cx, (gint)cy);
}

/* on_notes_grid_drag_prepare() — GtkDragSource "prepare" on the grid: the
 * same content as the list's, from the item under the press.               */
static GdkContentProvider *
on_notes_grid_drag_prepare(GtkDragSource *source, gdouble x, gdouble y,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = grid_path_at(lw, x, y);
    if (path == NULL)
        return NULL;
    return drag_rows_content(lw, source, notes_drag_rows(lw, path));
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drop() — GtkDropTarget "drop": the button was released over
 * the sidebar.  Fires exactly once per drag, with the release coordinates
 * (D5).  Either note ids from a notes view (move the notes into the target
 * folder, or trash them) or one of the sidebar's own folder rows (re-nest
 * INTO a folder, reorder BEFORE/AFTER a sibling, trash, or restore-by-drag
 * out of the Trash).  The row under the pointer is validated again: the
 * release may land where motion had refused.
 * Returns TRUE when something moved (GTK then finishes the drop as a MOVE).
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_drop(GtkDropTarget *target, const GValue *value,
                gdouble x, gdouble y, gpointer user_data)
{
    (void)target;
    OnLibrary *lw = user_data;       /* owning library window               */
    const OnDragRows *rows = g_value_get_boxed(value);
    gtk_tree_view_set_drag_dest_row(lw->sidebar, NULL,
                                    GTK_TREE_VIEW_DROP_BEFORE);

    GtkTreePath *dest_path = NULL;   /* the validated target row            */
    GtkTreeViewDropPosition pos;
    GtkTreeIter src_iter;            /* the dragged folder's row            */
    if (!sidebar_drop_target(lw, rows, (gint)x, (gint)y, &dest_path, &pos,
                             &src_iter))
        return FALSE;

    GtkTreeModel *sb_model = GTK_TREE_MODEL(lw->sidebar_store);
    GtkTreeIter dest_iter;           /* target sidebar row                  */
    gint   dest_kind = -1;           /* its kind                            */
    gint64 dest_id   = 0;            /* its folder id                       */
    gchar *dest_raw  = NULL;         /* its bare name                       */
    gtk_tree_model_get_iter(sb_model, &dest_iter, dest_path);
    gtk_tree_model_get(sb_model, &dest_iter,
                       SB_KIND, &dest_kind, SB_ID, &dest_id,
                       SB_RAW,  &dest_raw, -1);
    gtk_tree_path_free(dest_path);

    gboolean success = FALSE;        /* whether anything moved              */
    if (rows->kind == ON_DRAG_NOTES) {
        /* --- note ids from the notes pane ---------------------------------*/
        const gint64 *ids = (const gint64 *)rows->ids->data;
        guint n = rows->ids->len;
        if (dest_kind == SB_KIND_TRASH) {
            /* Dropping on Trash IS the delete gesture.                     */
            success = trash_notes_core(lw, ids, n);
        } else {
            /* ONE transaction for the whole selection: per-note moves
             * fsync per call and froze the GUI on big drops.              */
            success = on_db_notes_move(lw->app->db, ids, n, dest_id);
            if (success)
                on_app_status(lw->app,
                              "Moved %u note%s to \xe2\x80\x9c%s\xe2\x80\x9d",
                              n, n == 1 ? "" : "s", dest_raw);
        }
    } else {
        /* --- one of the sidebar's own folder rows --------------------------*/
        gint   src_kind  = (rows->kind == ON_DRAG_FOLDER)
                           ? SB_KIND_FOLDER : SB_KIND_TRASH_FOLDER;
        gint64 folder_id = g_array_index(rows->ids, gint64, 0);
        gchar *fname     = NULL;     /* its bare name                       */
        gtk_tree_model_get(sb_model, &src_iter, SB_RAW, &fname, -1);

        if (dest_kind == SB_KIND_TRASH) {
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
            if (dest_kind == SB_KIND_ROOT ||
                pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE ||
                pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER) {
                gint64 new_parent =  /* root drops land at top level        */
                    (dest_kind == SB_KIND_FOLDER) ? dest_id : 0;
                success = on_db_folder_move(lw->app->db, folder_id,
                                            new_parent);
                where = g_strdup(dest_raw);
            } else {
                /* The dest folder's parent row is the new parent: the
                 * "Notes" root maps to top level (0).                      */
                GtkTreeIter par_iter;        /* dest's parent row           */
                gint   par_kind = SB_KIND_ROOT;
                gint64 par_id   = 0;
                gchar *par_raw  = NULL;
                if (gtk_tree_model_iter_parent(sb_model, &par_iter,
                                               &dest_iter))
                    gtk_tree_model_get(sb_model, &par_iter,
                                       SB_KIND, &par_kind,
                                       SB_ID,   &par_id,
                                       SB_RAW,  &par_raw, -1);
                gint64 new_parent =
                    (par_kind == SB_KIND_FOLDER) ? par_id : 0;
                success = folder_move_beside(
                    lw, folder_id, new_parent, dest_id,
                    pos == GTK_TREE_VIEW_DROP_AFTER);
                where = par_raw ? par_raw : g_strdup("Notes");
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
    g_free(dest_raw);

    /* The models are rebuilt here, inside the drop handler: GTK finishes
     * the drop only after this returns, so the drag icon lingers for the
     * refresh (tens of ms).  There is no gtk_drag_finish() to call first
     * any more — the return value IS the finish.                          */
    if (success)
        refresh_all(lw);             /* tree shape and counts changed       */
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

/* ---------------------------------------------------------------------------
 * on_folder_prompt_response() — the folder dialog closed: read the fields
 * and hand them to the continuation on OK (an empty name is a cancel).
 * ------------------------------------------------------------------------- */
static void
on_folder_prompt_response(GtkDialog *dlg, gint response, gpointer user_data)
{
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
    gtk_window_destroy(GTK_WINDOW(dlg));
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
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK,
        NULL);
    gtk_widget_add_css_class(dialog, "notes-dialog"); /* library_install_css */
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    FolderPrompt *p = g_new0(FolderPrompt, 1);
    p->lw     = lw;
    p->folder = folder;
    p->done   = done;
    g_object_set_data_full(G_OBJECT(dialog), "on-prompt", p, g_free);

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

    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                   field_grid);

    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_folder_prompt_response), p);
    gtk_window_present(GTK_WINDOW(dialog));
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
    const gchar *mode =              /* "list" or "grid"                    */
        gtk_stack_get_visible_child_name(GTK_STACK(lw->stack));

    GList *paths;                    /* selected row paths                  */
    if (g_strcmp0(mode, "grid") == 0) {
        paths = gtk_icon_view_get_selected_items(lw->notes_grid);
    } else {
        paths = gtk_tree_selection_get_selected_rows(
            gtk_tree_view_get_selection(lw->notes_list), NULL);
    }

    for (GList *l = paths; l != NULL; l = l->next) {
        GtkTreeIter iter;            /* one selected row                    */
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                    &iter, l->data)) {
            gint64 id;               /* its note id                         */
            gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                               NL_ID, &id, -1);
            g_array_append_val(ids, id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
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
 * capture_click_gesture() — a GtkGestureClick on `widget` in the CAPTURE
 * phase, so its "pressed" runs BEFORE the widget's own bubble-phase click
 * gesture.  That order is the whole point for the tree views: GTK4's
 * GtkTreeView CLEAR_AND_SELECTs on the FIRST press of ANY button (measured
 * in gtk/deprecated/gtktreeview.c, gtk_tree_view_click_gesture_pressed),
 * so a right-click handler that wants to keep a multi-selection, and the
 * quirk-15 veto (D7), both have to get there first.  A handler that wants
 * the press for itself CLAIMs the sequence, which is what returning TRUE
 * from a button-press-event used to do.
 *   widget  — the widget to watch.
 *   button  — GDK_BUTTON_* to watch, or 0 for any.
 *   pressed — the "pressed" handler.
 *   data    — its user data.
 * Returns the gesture, for callers that connect more of its signals.
 * ------------------------------------------------------------------------- */
static GtkGesture *
capture_click_gesture(GtkWidget *widget, guint button, GCallback pressed,
                      gpointer data)
{
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), button);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", pressed, data);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
    return click;
}

/* ---------------------------------------------------------------------------
 * on_sidebar_pressed() — right click in the folder/tag tree: select the
 * row under the pointer and show a context menu mirroring the sidebar
 * toolbar (folder actions + scoped search).  The items name actions, so
 * the row kind only decides which items appear; the handlers read the
 * selection this press just made.  The press is claimed either way.
 *   x, y — the press in the sidebar's widget coordinates.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_pressed(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                   gpointer user_data)
{
    (void)n_press;
    OnLibrary *lw = user_data;       /* owning library window               */
    gint bx, by;                     /* the press in bin-window coordinates */
    gtk_tree_view_convert_widget_to_bin_window_coords(lw->sidebar,
                                                      (gint)x, (gint)y,
                                                      &bx, &by);
    GtkTreePath *path = NULL;        /* row under the pointer               */
    if (!gtk_tree_view_get_path_at_pos(lw->sidebar, bx, by, &path,
                                       NULL, NULL, NULL))
        return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);

    gtk_tree_selection_select_path(gtk_tree_view_get_selection(lw->sidebar),
                                   path);

    /* What kind of row was clicked decides which actions make sense.       */
    GtkTreeIter iter;                /* the clicked row                     */
    gint kind = SB_KIND_TAGS_HEADER; /* default: nothing to offer           */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->sidebar_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, -1);
    gtk_tree_path_free(path);
    if (kind == SB_KIND_TAGS_HEADER)
        return;                      /* consumed, but no menu               */

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

    on_app_menu_popup(GTK_WIDGET(lw->sidebar), G_MENU_MODEL(menu), x, y);
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
    if (lw->list_autofit)
        refresh_notes(lw);           /* re-measure the widths grid skipped  */
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
 * notes_ctx_popup() — shared right-click tail of both notes views: read
 * the note id at `path` (owned — freed here) and pop up the note context
 * menu for it, then CLAIM the press so the view's own gesture never sees
 * it (it would collapse the selection).
 *   g      — the press gesture.
 *   attach — the view the press landed in.
 *   path   — the clicked row (consumed).
 *   x, y   — the press, in `attach`'s coordinates.
 * ------------------------------------------------------------------------- */
static void
notes_ctx_popup(OnLibrary *lw, GtkGestureClick *g, GtkWidget *attach,
                GtkTreePath *path, gdouble x, gdouble y)
{
    GtkTreeIter iter;                /* the clicked row                     */
    gint64 id = 0;                   /* its note id                         */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &id, -1);
    gtk_tree_path_free(path);

    if (id != 0)
        show_note_context_menu(lw, attach, id, x, y);
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* notes_sel_block_func() — the temporary select function for the span of
 * a press on an already-selected list row: vetoes EVERY selection change. */
static gboolean
notes_sel_block_func(GtkTreeSelection *sel, GtkTreeModel *model,
                     GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)model; (void)path; (void)selected; (void)data;
    return FALSE;
}

/* notes_sel_unblock() — end a blocked press: selection changes work
 * again (the select function goes back to none); the press path is
 * optionally handed to the caller (transfer), otherwise freed.             */
static GtkTreePath *
notes_sel_unblock(OnLibrary *lw, gboolean want_path)
{
    if (!lw->notes_sel_blocked)
        return NULL;
    gtk_tree_selection_set_select_function(
        gtk_tree_view_get_selection(lw->notes_list), NULL, NULL, NULL);
    lw->notes_sel_blocked = FALSE;
    GtkTreePath *path = lw->notes_press_path;
    lw->notes_press_path = NULL;
    if (!want_path) {
        gtk_tree_path_free(path);
        path = NULL;
    }
    return path;
}

/* ---------------------------------------------------------------------------
 * on_notes_list_pressed() — CAPTURE-phase press on the notes list (see
 * capture_click_gesture), two jobs:
 *
 * 1. Unmodified primary press on an already-selected row of a
 *    multi-selection: the view's own gesture CLEAR_AND_SELECTs on press
 *    with no deferral for a possible drag (quirk #15, re-measured on GTK4
 *    as D7), which would collapse the selection before a multi-note drag
 *    could start.  Install a selection veto for the span of the press;
 *    on_notes_list_drag_prepare keeps the selection, a plain release
 *    applies the collapse GTK wanted.  The press is NOT claimed — the
 *    view's gesture and the drag source must still see it.
 *
 * 2. Right click: select the row under the pointer (an existing
 *    multi-selection is kept when clicked inside) and show the note menu;
 *    that press IS claimed.
 * ------------------------------------------------------------------------- */
static void
on_notes_list_pressed(GtkGestureClick *g, gint n_press, gdouble x,
                      gdouble y, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    GtkTreeView *view = lw->notes_list;
    gint bx, by;                     /* the press in bin-window coordinates */
    gtk_tree_view_convert_widget_to_bin_window_coords(view, (gint)x, (gint)y,
                                                      &bx, &by);
    GtkTreePath *path = NULL;        /* row under the pointer               */
    if (!gtk_tree_view_get_path_at_pos(view, bx, by, &path,
                                       NULL, NULL, NULL))
        return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);

    if (button == GDK_BUTTON_PRIMARY) {
        GdkModifierType state = gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(g));
        if (n_press == 1 &&
            !(state & gtk_accelerator_get_default_mod_mask()) &&
            gtk_tree_selection_path_is_selected(sel, path) &&
            gtk_tree_selection_count_selected_rows(sel) > 1) {
            gtk_tree_selection_set_select_function(
                sel, notes_sel_block_func, NULL, NULL);
            lw->notes_sel_blocked = TRUE;
            gtk_tree_path_free(lw->notes_press_path);
            lw->notes_press_path = path;         /* ownership taken         */
            return;
        }
        gtk_tree_path_free(path);
        return;
    }
    if (button != GDK_BUTTON_SECONDARY) {
        gtk_tree_path_free(path);
        return;
    }

    /* Right-clicking inside an existing multi-selection keeps it (so bulk
     * actions can target it); clicking elsewhere selects just that row.    */
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }
    notes_ctx_popup(lw, g, GTK_WIDGET(view), path, x, y);
}

/* on_notes_list_released() — a blocked press ended WITHOUT a drag: lift
 * the veto and apply the collapse GTK wanted on press (a plain click on a
 * selected row means "select just this one").                              */
static void
on_notes_list_released(GtkGestureClick *g, gint n_press, gdouble x,
                       gdouble y, gpointer user_data)
{
    (void)g; (void)n_press; (void)x; (void)y;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = notes_sel_unblock(lw, TRUE);
    if (path != NULL) {
        gtk_tree_view_set_cursor(lw->notes_list, path, NULL, FALSE);
        gtk_tree_path_free(path);
    }
}

/* on_notes_list_cancel() — the press gesture was cancelled (the drag
 * source claimed the sequence, or the press left the widget): drop the
 * veto without collapsing.  The drag path lifts it in prepare as well;
 * this covers the cancellations that never become a drag.                  */
static void
on_notes_list_cancel(GtkGesture *g, GdkEventSequence *seq,
                     gpointer user_data)
{
    (void)g; (void)seq;
    notes_sel_unblock(user_data, FALSE);
}

/* ---------------------------------------------------------------------------
 * on_notes_grid_pressed() — right click in grid mode: same as the list's
 * for the icon view (its own gesture only selects on the primary button,
 * but the press is claimed for symmetry).
 * ------------------------------------------------------------------------- */
static void
on_notes_grid_pressed(GtkGestureClick *g, gint n_press, gdouble x,
                      gdouble y, gpointer user_data)
{
    (void)n_press;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = grid_path_at(lw, x, y);
    if (path == NULL)
        return;

    /* Keep an existing multi-selection when right-clicking inside it.      */
    if (!gtk_icon_view_path_is_selected(lw->notes_grid, path)) {
        gtk_icon_view_unselect_all(lw->notes_grid);
        gtk_icon_view_select_path(lw->notes_grid, path);
    }
    notes_ctx_popup(lw, g, GTK_WIDGET(lw->notes_grid), path, x, y);
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
on_notes_selection_status(gpointer view_or_selection, gpointer user_data)
{
    (void)view_or_selection;
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
    GtkTreeModel *model = GTK_TREE_MODEL(lw->notes_store);
    GtkTreeIter   iter;              /* the saved note's row                */
    gboolean      found = FALSE;
    gboolean      valid = gtk_tree_model_get_iter_first(model, &iter);
    /* NOT a for loop: its increment would run after the match and advance
     * `iter` off the row we just found — invalidating it outright when the
     * match is the last row, which is where a just-saved note usually is.   */
    while (valid && !found) {
        gint64 id;                   /* this row's note id                  */
        gtk_tree_model_get(model, &iter, NL_ID, &id, -1);
        if (id == note_id)
            found = TRUE;            /* leave `iter` ON the match           */
        else
            valid = gtk_tree_model_iter_next(model, &iter);
    }
    if (!found)
        return;

    OnNoteMeta *m = on_db_note_get(lw->app->db, note_id);
    if (m == NULL)
        return;

    GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
    gchar *when = g_date_time_format(dt, LIST_TIME_FORMAT);
    g_date_time_unref(dt);

    /* Preview only where it is shown (Comfortable density, list view).      */
    gchar *preview = NULL;
    if (lw->app->comfortable_list) {
        gchar *body = on_db_note_body_text(lw->app->db, note_id);
        preview = notes_preview_line(body);
        g_free(body);
    }

    /* Setting NL_UPDATED is what re-sorts the row to the top under the
     * default Modified ordering — the same place the old repopulate's
     * "ORDER BY updated_at DESC" put it.  populating stays DOWN: this is a
     * real content change and the selection handlers should see it.         */
    gtk_list_store_set(lw->notes_store, &iter,
                       NL_TITLE,    m->title,
                       NL_MODIFIED, when,
                       NL_UPDATED,  m->updated_at,
                       NL_PREVIEW,  preview,
                       -1);

    /* The grid's thumbnail for this note is now stale; re-render it in idle
     * time exactly as a repopulate would have (list mode pays nothing).     */
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        ThumbJob *job = g_new0(ThumbJob, 1);
        job->row        = gtk_tree_row_reference_new(model, path);
        job->id         = note_id;
        job->updated_at = m->updated_at;
        g_queue_push_tail(&lw->thumb_pending, job);
        gtk_tree_path_free(path);
        if (lw->thumb_idle == 0)
            lw->thumb_idle = g_idle_add(thumb_fill_idle, lw);
    }

    g_free(preview);
    g_free(when);
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

/* ---------------------------------------------------------------------------
 * sort_by_title() — case-insensitive alphabetical sort for the Title
 * header.
 * ------------------------------------------------------------------------- */
static gint
sort_by_title(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
              gpointer user_data)
{
    (void)user_data;
    gchar *ta, *tb;                  /* the two titles                      */
    gtk_tree_model_get(model, a, NL_TITLE, &ta, -1);
    gtk_tree_model_get(model, b, NL_TITLE, &tb, -1);
    gint result = utf8_casecmp(ta, tb);
    g_free(ta); g_free(tb);
    return result;
}

/* ===========================================================================
 * list-view column layout (order + visibility)
 *
 * Headers drag to reorder (GtkTreeViewColumn reorderable) and right-click
 * for a show/hide menu.  The layout persists in the ini as
 * "<cfgkey>=key:vis,key:vis,..." in display order; each column carries
 * its stable key as object data ("on-colkey").  The machinery is shared
 * by the notes list and the Action Items list: each VIEW carries its ini
 * key ("on-colcfg"), its expected column count ("on-ncols") and its
 * default layout ("on-coldefault") as object data.
 * =========================================================================== */

/* How many columns the notes list owns (Title, Path, Modified, Created) —
 * keep in sync with the COLS[] table in the window constructor.             */
#define N_LIST_COLUMNS 4

/* ---------------------------------------------------------------------------
 * view_columns_persist() — write a list view's current column order and
 * visibility to the ini.  A partial column list (the view tearing down
 * removes columns one by one) is never persisted.
 * ------------------------------------------------------------------------- */
static void
view_columns_persist(GtkTreeView *view)
{
    const gchar *cfg_key =           /* the view's ini key                  */
        g_object_get_data(G_OBJECT(view), "on-colcfg");
    gint n_cols =                    /* the view's full column count        */
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "on-ncols"));
    GList *cols = gtk_tree_view_get_columns(view);
    if (cfg_key == NULL || g_list_length(cols) != (guint)n_cols) {
        g_list_free(cols);
        return;
    }
    GString *s = g_string_new(NULL);
    for (GList *l = cols; l != NULL; l = l->next) {
        const gchar *key =           /* stable column identity              */
            g_object_get_data(G_OBJECT(l->data), "on-colkey");
        if (key == NULL)
            continue;
        if (s->len > 0)
            g_string_append_c(s, ',');
        g_string_append_printf(s, "%s:%d", key,
            gtk_tree_view_column_get_visible(l->data) ? 1 : 0);
    }
    g_list_free(cols);
    on_app_config_set(cfg_key, s->str);
    g_string_free(s, TRUE);
}

/* view_column_by_key() — the view's column carrying `key`, or NULL.         */
static GtkTreeViewColumn *
view_column_by_key(GtkTreeView *view, const gchar *key)
{
    GtkTreeViewColumn *found = NULL;
    GList *cols = gtk_tree_view_get_columns(view);
    for (GList *l = cols; l != NULL && found == NULL; l = l->next)
        if (g_strcmp0(g_object_get_data(G_OBJECT(l->data), "on-colkey"),
                      key) == 0)
            found = l->data;
    g_list_free(cols);
    return found;
}

/* list_column_by_key() — notes-list shorthand (autofit uses it a lot).      */
static GtkTreeViewColumn *
list_column_by_key(OnLibrary *lw, const gchar *key)
{
    return view_column_by_key(lw->notes_list, key);
}

/* list_column_shown() — is the notes list's `key` column visible right now?
 * Autofit consults this BEFORE measuring: list_autofit_set() discards the
 * width of a hidden column, so measuring one is wasted work.                */
static gboolean
list_column_shown(OnLibrary *lw, const gchar *key)
{
    GtkTreeViewColumn *c = list_column_by_key(lw, key);
    return c != NULL && gtk_tree_view_column_get_visible(c);
}

/* ---------------------------------------------------------------------------
 * list_autofit_time_width() — an upper bound, in pixels, on any timestamp
 * the Modified or Created column can hold.
 *
 * Both columns render one fixed pattern (LIST_TIME_FORMAT), so every value
 * has the same shape and differs only in which month abbreviation and which
 * digits it contains.  Measuring the widest localized month against the
 * widest digit therefore bounds the whole column in ~22 measurements instead
 * of one per row — which at a thousand-plus notes was the most expensive
 * thing refresh_notes() did, repeated on every autosave-driven refresh.
 *
 * A bound rather than the exact maximum is the right answer here: the caller
 * adds AUTOFIT_CELL_EXTRA px of cell chrome on top anyway, and erring wide
 * only pads the column, while erring narrow would clip text (these columns
 * do not ellipsize under autofit).
 *   lay — the measuring layout, in the view's font.
 * Returns the bound in pixels.
 * ------------------------------------------------------------------------- */
static gint
list_autofit_time_width(PangoLayout *lay)
{
    /* Widest digit glyph — proportional fonts do vary ('1' vs '8').         */
    gchar widest  = '0';             /* digit with the largest advance      */
    gint  digit_w = 0;               /* its width                           */
    for (gchar d = '0'; d <= '9'; d++) {
        gint w;                      /* width of this digit                 */
        pango_layout_set_text(lay, &d, 1);
        pango_layout_get_pixel_size(lay, &w, NULL);
        if (w > digit_w) {
            digit_w = w;
            widest  = d;
        }
    }

    /* Each month's name in a full synthetic stamp whose every digit is the
     * widest one, so no real timestamp can come out wider.                  */
    gint max_w = 0;                  /* running maximum                     */
    for (gint month = 1; month <= 12; month++) {
        GDateTime *dt = g_date_time_new_local(2026, month, 28, 23, 59, 0);
        if (dt == NULL)
            continue;
        gchar *stamp = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);
        if (stamp == NULL)
            continue;
        for (gchar *p = stamp; *p != '\0'; p++)
            if (g_ascii_isdigit(*p))
                *p = widest;
        gint w;                      /* width of this month's stamp         */
        pango_layout_set_text(lay, stamp, -1);
        pango_layout_get_pixel_size(lay, &w, NULL);
        max_w = MAX(max_w, w);
        g_free(stamp);
    }
    return max_w;
}

/* ---------------------------------------------------------------------------
 * view_columns_apply() — put a view's saved column order and visibility
 * back (falling back to its "on-coldefault" layout).  Unknown keys are
 * skipped, missing ones keep their built order and visibility, and at
 * least one column is forced visible (a hand-edited ini can't blank the
 * view).
 * ------------------------------------------------------------------------- */
static void
view_columns_apply(GtkTreeView *view)
{
    const gchar *cfg_key =           /* the view's ini key                  */
        g_object_get_data(G_OBJECT(view), "on-colcfg");
    gchar *cfg = cfg_key != NULL ? on_app_config_get(cfg_key) : NULL;
    if (cfg == NULL || *cfg == '\0') {
        g_free(cfg);
        cfg = g_strdup(g_object_get_data(G_OBJECT(view), "on-coldefault"));
    }

    GtkTreeViewColumn *prev = NULL;  /* each entry moves after the last     */
    gchar **entries = g_strsplit(cfg, ",", -1);
    for (gsize i = 0; entries[i] != NULL; i++) {
        gchar **kv = g_strsplit(entries[i], ":", 2);
        GtkTreeViewColumn *c =
            kv[0] != NULL ? view_column_by_key(view, kv[0]) : NULL;
        if (c != NULL) {
            gtk_tree_view_move_column_after(view, c, prev);
            gtk_tree_view_column_set_visible(
                c, kv[1] == NULL || g_strcmp0(kv[1], "0") != 0);
            prev = c;
        }
        g_strfreev(kv);
    }
    g_strfreev(entries);
    g_free(cfg);

    GList *cols = gtk_tree_view_get_columns(view);
    gboolean any_visible = FALSE;    /* is at least one column shown?       */
    for (GList *l = cols; l != NULL; l = l->next)
        any_visible |= gtk_tree_view_column_get_visible(l->data);
    if (!any_visible && cols != NULL)
        gtk_tree_view_column_set_visible(cols->data, TRUE);
    g_list_free(cols);
}

/* on_view_columns_changed() — a header drag reordered the columns:
 * persist the new layout.  Fires per-column during teardown too, when
 * the library state may already be gone — bail out then.                    */
static void
on_view_columns_changed(GtkTreeView *view, gpointer user_data)
{
    (void)user_data;
    if (gtk_widget_in_destruction(GTK_WIDGET(view)))
        return;
    view_columns_persist(view);
}

/* ---------------------------------------------------------------------------
 * list_autofit_apply() — put every column into (or out of) autofit mode.
 *
 * Autofit does NOT use GTK_TREE_VIEW_COLUMN_AUTOSIZE: tree-view columns
 * cache resized/requested widths that override it (they neither grow to
 * long content nor shrink back for short content).  Instead every column
 * goes FIXED, and refresh_notes() MEASURES the content with a
 * PangoLayout as it populates the model (see list_autofit_set) and
 * sets the exact widths — same technique as the sidebar width fit.
 *
 *   ON  — Path and Modified: no ellipsize, FIXED at their measured
 *         content width (grips off — the next refresh would reclaim
 *         them anyway).  Title takes the ellipsis + expand: it fills
 *         the remaining space and is the one column that truncates.
 *   OFF — back to the built modes: Title GROW_ONLY + expand, no
 *         ellipsize; Path FIXED at its current width, ellipsized;
 *         Modified GROW_ONLY; all user-resizable.
 * ------------------------------------------------------------------------- */
static void
list_autofit_apply(OnLibrary *lw)
{
    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    for (GList *l = cols; l != NULL; l = l->next) {
        GtkTreeViewColumn *c = l->data;  /* one column                      */
        const gchar *key =               /* stable column identity          */
            g_object_get_data(G_OBJECT(c), "on-colkey");
        GtkCellRenderer *cell =          /* its text renderer               */
            g_object_get_data(G_OBJECT(c), "on-cell");
        gboolean title = g_strcmp0(key, "title") == 0;
        gboolean path  = g_strcmp0(key, "path")  == 0;

        if (lw->list_autofit) {
            g_object_set(cell, "ellipsize",
                         title ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE,
                         NULL);
            gtk_tree_view_column_set_resizable(c, FALSE);
            gtk_tree_view_column_set_sizing(
                c, GTK_TREE_VIEW_COLUMN_FIXED);
            if (title)                   /* floor; expand fills the rest    */
                gtk_tree_view_column_set_fixed_width(c, 100);
            gtk_tree_view_column_set_expand(c, title);
        } else {
            g_object_set(cell, "ellipsize",
                         path ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE,
                         NULL);
            if (path) {
                gint w = gtk_tree_view_column_get_width(c);
                gtk_tree_view_column_set_sizing(
                    c, GTK_TREE_VIEW_COLUMN_FIXED);
                gtk_tree_view_column_set_fixed_width(c, w > 0 ? w : 180);
            } else {
                gtk_tree_view_column_set_sizing(
                    c, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
            }
            gtk_tree_view_column_set_expand(c, title);
            gtk_tree_view_column_set_resizable(c, TRUE);
        }
        gtk_tree_view_column_queue_resize(c);
    }
    g_list_free(cols);
}

/* Extra pixels beyond the raw text: the renderer's xpad (10 a side)
 * plus tree-view cell chrome; headers get room for the sort arrow.        */
#define AUTOFIT_CELL_EXTRA   30
#define AUTOFIT_HEADER_EXTRA 40

/* ---------------------------------------------------------------------------
 * list_autofit_set() — apply a measured content width to one column
 * (bounded below by what its header label needs).  The measuring itself
 * rides refresh_notes()'s population loop — no second model walk.
 * ------------------------------------------------------------------------- */
static void
list_autofit_set(OnLibrary *lw, PangoLayout *lay, const gchar *key,
                 gint content_w)
{
    GtkTreeViewColumn *c = list_column_by_key(lw, key);
    if (c == NULL || !gtk_tree_view_column_get_visible(c))
        return;
    gint w = 0;                      /* header label width                  */
    pango_layout_set_text(lay, gtk_tree_view_column_get_title(c), -1);
    pango_layout_get_pixel_size(lay, &w, NULL);
    gtk_tree_view_column_set_fixed_width(
        c, MAX(content_w + AUTOFIT_CELL_EXTRA, w + AUTOFIT_HEADER_EXTRA));
}

/* on_autofit_change_state() — "win.autofit" (stateful boolean, the header
 * menu's "Autofit Column Widths" check item) flipped: remember, persist
 * and apply.                                                                */
static void
on_autofit_change_state(GSimpleAction *action, GVariant *value,
                        gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    g_simple_action_set_state(action, value);
    lw->list_autofit = g_variant_get_boolean(value);
    on_app_config_set("list_autofit", lw->list_autofit ? "1" : "0");
    list_autofit_apply(lw);
    refresh_notes(lw);               /* measuring rides the populate loop   */
}

/* The "column-<key>" actions: one stateful boolean per column of the view
 * whose header was last right-clicked, named after the column's stable
 * key ("on-colkey").  Created on demand by column_menu_action().           */
#define COLUMN_ACTION_PREFIX "column-"

/* on_column_change_state() — a column's check item flipped: show/hide the
 * column of lw->column_menu_view carrying that key and persist.  Autofit
 * re-measuring applies to the notes list only (the Action Items list
 * doesn't autofit).                                                         */
static void
on_column_change_state(GSimpleAction *action, GVariant *value,
                       gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    const gchar *key =               /* the column key after the prefix     */
        g_action_get_name(G_ACTION(action)) + strlen(COLUMN_ACTION_PREFIX);
    GtkTreeView *view = lw->column_menu_view;
    if (view == NULL)
        return;
    g_simple_action_set_state(action, value);

    GList *cols = gtk_tree_view_get_columns(view);
    for (GList *l = cols; l != NULL; l = l->next) {
        if (g_strcmp0(g_object_get_data(G_OBJECT(l->data), "on-colkey"),
                      key) == 0)
            gtk_tree_view_column_set_visible(l->data,
                                             g_variant_get_boolean(value));
    }
    g_list_free(cols);
    view_columns_persist(view);
    if (view == lw->notes_list && lw->list_autofit)
        refresh_notes(lw);           /* a re-shown column needs its width   */
}

/* ---------------------------------------------------------------------------
 * column_menu_action() — the "column-<key>" action for one column, created
 * on the window the first time that column's menu is opened and brought up
 * to date every time: state = the column's visibility, enabled unless it
 * is the only visible column (so the view can't go empty).
 *   lw      — the library window.
 *   key     — the column's stable key.
 *   visible — whether the column is currently shown.
 *   last    — TRUE when it is the only visible one.
 * Returns the detailed action name for the menu item (g_free it).
 * ------------------------------------------------------------------------- */
static gchar *
column_menu_action(OnLibrary *lw, const gchar *key, gboolean visible,
                   gboolean last)
{
    gchar *name = g_strconcat(COLUMN_ACTION_PREFIX, key, NULL);
    GActionMap *map = G_ACTION_MAP(lw->window);
    GSimpleAction *action =
        G_SIMPLE_ACTION(g_action_map_lookup_action(map, name));
    if (action == NULL) {
        action = g_simple_action_new_stateful(
            name, NULL, g_variant_new_boolean(visible));
        g_signal_connect(action, "change-state",
                         G_CALLBACK(on_column_change_state), lw);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
    } else {
        g_simple_action_set_state(action, g_variant_new_boolean(visible));
    }
    g_simple_action_set_enabled(action, !(visible && last));

    gchar *detailed = g_strconcat("win.", name, NULL);
    g_free(name);
    return detailed;
}

/* ---------------------------------------------------------------------------
 * on_column_header_pressed() — right click on a list-view column header:
 * a menu of check items showing/hiding each column of the header's view
 * (the button carries its view as "on-view").  The only remaining
 * visible column's item is disabled so the view can't go empty; the
 * notes list's menu also offers the autofit toggle.  The press is claimed
 * so the header button does not also act on it.
 *   x, y — the press, in the header button's coordinates.
 * ------------------------------------------------------------------------- */
static void
on_column_header_pressed(GtkGestureClick *g, gint n_press, gdouble x,
                         gdouble y, gpointer user_data)
{
    (void)n_press;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkWidget *button =              /* the header button pressed           */
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkTreeView *view =              /* the view this header belongs to     */
        g_object_get_data(G_OBJECT(button), "on-view");
    if (view == NULL)
        return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    lw->column_menu_view = view;     /* what the column actions act on      */

    GMenu *menu    = g_menu_new();
    GMenu *section = g_menu_new();

    GList *cols = gtk_tree_view_get_columns(view);
    gint n_visible = 0;              /* how many columns are shown          */
    for (GList *l = cols; l != NULL; l = l->next)
        if (gtk_tree_view_column_get_visible(l->data))
            n_visible++;

    for (GList *l = cols; l != NULL; l = l->next) {
        GtkTreeViewColumn *c = l->data;  /* one column                      */
        gboolean visible = gtk_tree_view_column_get_visible(c);
        const gchar *label =         /* a titleless column (the checkbox
                                        one) still needs a menu label       */
            g_object_get_data(G_OBJECT(c), "on-collabel");
        if (label == NULL)
            label = gtk_tree_view_column_get_title(c);
        gchar *detailed = column_menu_action(
            lw, g_object_get_data(G_OBJECT(c), "on-colkey"),
            visible, n_visible == 1);
        g_menu_append(section, label, detailed);
        g_free(detailed);
    }
    g_list_free(cols);

    if (view == lw->notes_list) {
        menu_section_end(menu, &section);
        g_menu_append(section, "Autofit Column Widths", "win.autofit");
    }
    menu_section_end(menu, &section);
    g_object_unref(section);

    on_app_menu_popup(button, G_MENU_MODEL(menu), x, y);
}

/* ---------------------------------------------------------------------------
 * column_header_menu_add() — wire one column's header for the layout
 * machinery: reorderable by drag, and its header button right-clicks into
 * on_column_header_pressed for `view`'s show/hide menu.  Shared by the
 * notes list and the Action Items list.
 *   lw   — the library window.
 *   col  — the column.
 *   view — the view it belongs to.
 * ------------------------------------------------------------------------- */
static void
column_header_menu_add(OnLibrary *lw, GtkTreeViewColumn *col,
                       GtkTreeView *view)
{
    gtk_tree_view_column_set_reorderable(col, TRUE);
    GtkWidget *btn = gtk_tree_view_column_get_button(col);
    g_object_set_data(G_OBJECT(btn), "on-view", view);
    capture_click_gesture(btn, GDK_BUTTON_SECONDARY,
                          G_CALLBACK(on_column_header_pressed), lw);
}

/* ---------------------------------------------------------------------------
 * sort_by_path() — case-insensitive alphabetical sort for the Path
 * header; equal paths fall back to the title so folders group cleanly.
 * ------------------------------------------------------------------------- */
static gint
sort_by_path(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
             gpointer user_data)
{
    (void)user_data;
    gchar *pa, *pb;                  /* the two paths                       */
    gtk_tree_model_get(model, a, NL_PATH, &pa, -1);
    gtk_tree_model_get(model, b, NL_PATH, &pb, -1);
    gint result = utf8_casecmp(pa, pb);
    g_free(pa); g_free(pb);
    return result != 0 ? result : sort_by_title(model, a, b, NULL);
}

/* ---------------------------------------------------------------------------
 * sort_by_time() — sort for the Modified and Created headers; the model
 * column holding the raw timestamp (NL_UPDATED or NL_CREATED_RAW) rides
 * in as GINT_TO_POINTER user_data.  Deliberately inverted so the FIRST
 * click on either header shows the newest notes on top.
 * ------------------------------------------------------------------------- */
static gint
sort_by_time(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
             gpointer user_data)
{
    gint col = GPOINTER_TO_INT(user_data);  /* the timestamp column         */
    gint64 ta, tb;                   /* the two timestamps                  */
    gtk_tree_model_get(model, a, col, &ta, -1);
    gtk_tree_model_get(model, b, col, &tb, -1);
    return (tb > ta) - (tb < ta);
}

/* sort_actions_by_text() — alphabetical sort for the Action header.         */
static gint
sort_actions_by_text(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                     gpointer user_data)
{
    (void)user_data;
    gchar *ta, *tb;                  /* the two item texts                  */
    gtk_tree_model_get(model, a, AL_TEXT, &ta, -1);
    gtk_tree_model_get(model, b, AL_TEXT, &tb, -1);
    gint result = utf8_casecmp(ta, tb);
    g_free(ta); g_free(tb);
    return result;
}

/* ---------------------------------------------------------------------------
 * action_due_color_func() — cell data function tinting the Due Date cell
 * by urgency: already passed = red, today = yellow, still ahead = green
 * (each darkened enough to read on the white/light-blue row stripes).
 * Runs at draw time, so the colors roll over at midnight without a
 * refresh.  Rows without a due date reset to the theme color — the
 * renderer is shared, so a stale "foreground" would leak across rows.
 * ------------------------------------------------------------------------- */
static void
action_due_color_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter,
                      gpointer user_data)
{
    (void)col; (void)user_data;
    gint64 due;                      /* the row's due timestamp             */
    gtk_tree_model_get(model, iter, AL_DUE_RAW, &due, -1);
    if (due == 0) {
        g_object_set(cell, "foreground-set", FALSE, NULL);
        return;
    }

    /* Compare calendar DAYS in local time (the stored value is already
     * local midnight, but day-level compare keeps this DST-proof).         */
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt  = g_date_time_new_from_unix_local(due);
    gint today = g_date_time_get_year(now) * 10000 +
                 g_date_time_get_month(now) * 100 +
                 g_date_time_get_day_of_month(now);
    gint day   = g_date_time_get_year(dt) * 10000 +
                 g_date_time_get_month(dt) * 100 +
                 g_date_time_get_day_of_month(dt);
    g_date_time_unref(now);
    g_date_time_unref(dt);

    const gchar *color = day < today   ? "#c01c28"   /* overdue: red       */
                       : day == today  ? "#d19a00"   /* today: gold        */
                                       : "#26a269";  /* ahead: green       */
    g_object_set(cell, "foreground", color, NULL);
}

/* sort_actions_by_due() — Due Date header: the first click shows the
 * soonest deadline on top; items without a due date sort after every
 * dated one.                                                                */
static gint
sort_actions_by_due(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                    gpointer user_data)
{
    (void)user_data;
    gint64 da, db;                   /* the two due timestamps              */
    gtk_tree_model_get(model, a, AL_DUE_RAW, &da, -1);
    gtk_tree_model_get(model, b, AL_DUE_RAW, &db, -1);
    if (da == 0) da = G_MAXINT64;    /* undated: always last                */
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* ---------------------------------------------------------------------------
 * notes_title_cell_func() — cell data function for the Title column.
 * Handles the alternating row tint AND the density-dependent rendering:
 *   Compact      — plain text title, minimal ypad.
 *   Comfortable  — bold Pango markup title with a small dimmed body-text
 *                  preview on the second line, generous ypad.
 * Alpha-based dimming for the preview keeps it readable on both the
 * selection highlight and the plain row background (fixed grey fails on
 * the blue selection — see hacienda task_desc_markup for the same rule).
 * ------------------------------------------------------------------------- */
static void
notes_title_cell_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter,
                      gpointer user_data)
{
    (void)col;
    OnLibrary *lw = user_data;         /* owning library window               */

    /* Alternating row tint — same as notes_row_bg_func.                    */
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even = (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    g_object_set(cell, "cell-background", even ? NULL : ROW_TINT, NULL);

    gchar *title   = NULL;
    gchar *preview = NULL;
    gtk_tree_model_get(model, iter,
                       NL_TITLE,   &title,
                       NL_PREVIEW, &preview,
                       -1);

    if (lw->app->comfortable_list) {
        gchar *esc = g_markup_escape_text(
            title != NULL && *title != '\0' ? title : "Untitled", -1);
        gchar *markup;
        gboolean bold = lw->app->bold_list_titles;
        if (preview != NULL && *preview != '\0') {
            gchar *esc_prev = g_markup_escape_text(preview, -1);
            markup = g_strdup_printf(
                bold ? "<b>%s</b>\n<small><span alpha=\"65%%\">%s</span></small>"
                     :    "%s\n<small><span alpha=\"65%%\">%s</span></small>",
                esc, esc_prev);
            g_free(esc_prev);
        } else {
            markup = g_strdup_printf(bold ? "<b>%s</b>" : "%s", esc);
        }
        g_object_set(cell, "markup", markup, "ypad", 7, NULL);
        g_free(markup);
        g_free(esc);
    } else {
        /* Always drive via "markup" so the Pango attribute list (set when
         * comfortable mode was last active) gets replaced, not left behind
         * to render the plain title in bold.                                */
        gchar *esc = g_markup_escape_text(title != NULL ? title : "", -1);
        g_object_set(cell, "markup", esc, "ypad", 2, NULL);
        g_free(esc);
    }
    g_free(title);
    g_free(preview);
}

/* ---------------------------------------------------------------------------
 * notes_row_bg_func() — cell data function giving list rows alternating
 * white / light-blue backgrounds regardless of theme.
 * ------------------------------------------------------------------------- */
static void
notes_row_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                  GtkTreeModel *model, GtkTreeIter *iter,
                  gpointer user_data)
{
    (void)col; (void)user_data;
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even =                  /* row parity drives the tint          */
        (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    g_object_set(cell,
                 "cell-background", even ? NULL : ROW_TINT,
                 NULL);
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
 * process reuses them), the "win." commands, the two parameterised note
 * actions and the stateful autofit toggle on the window.  The per-column
 * actions are added lazily by column_menu_action().
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

    action = g_simple_action_new_stateful(
        "autofit", NULL, g_variant_new_boolean(lw->list_autofit));
    g_signal_connect(action, "change-state",
                     G_CALLBACK(on_autofit_change_state), lw);
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
 * a folder-actions area, a drawn separator, a note-actions area (ending
 * with the List/Grid toggle and Media), another separator, Settings and
 * Search, a third separator, the AI Summary button (shown only while AI is
 * enabled) — and the search entry pinned to the right edge by an expanding
 * spacer.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_action_bar(OnLibrary *lw)
{
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(toolbar, "toolbar");

    /* --- folder area ---------------------------------------------------- */
    add_tool_button(lw, toolbar, "sidebar", "\xe2\x97\xa7",
                    "Folders", "Show or hide the folder pane",
                    "app.toggle-sidebar");
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
    /* Icon, label and tooltip are all set by view_button_sync() below,
     * from the view actually showing; these are only what it is built
     * with before the stack can be read.                                   */
    lw->view_btn = add_tool_button(lw, toolbar, "grid", "\xe2\x8a\x9e",
                    "Grid", "Switch to grid view",
                    "win.toggle-view");
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

/* ---------------------------------------------------------------------------
 * sidebar_name_cell_func() — bold the sidebar's section rows (the Notes
 * root, the Tags header, and Pinned Notes); folders and tags render at
 * normal weight.  Runs per row draw, keyed on SB_KIND.
 * ------------------------------------------------------------------------- */
static void
sidebar_name_cell_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                       GtkTreeModel *model, GtkTreeIter *iter,
                       gpointer user_data)
{
    (void)col; (void)user_data;
    gint kind;                       /* SB_KIND_* of this row               */
    gtk_tree_model_get(model, iter, SB_KIND, &kind, -1);
    g_object_set(cell, "weight",
                 sb_kind_is_section(kind) ? PANGO_WEIGHT_BOLD
                                          : PANGO_WEIGHT_NORMAL, NULL);
}

/* library_free() — destructor for the OnLibrary attached to the window.     */
static void
library_free(gpointer data)
{
    OnLibrary *lw = data;
    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    if (lw->sb_fit_idle != 0)
        g_source_remove(lw->sb_fit_idle);
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
    if (lw->notes_press_path != NULL)
        gtk_tree_path_free(lw->notes_press_path);
    /* GTK keeps its own reference to the menubar model while it renders
     * it; these are just ours.                                            */
    g_clear_object(&lw->menubar_model);
    g_free(lw->sel_name);
    g_free(lw);
}

/* Narrowest the sidebar is ever fitted to: a library of short folder names
 * must not leave a sliver of a pane.  Shared by the one-shot startup fit
 * and by sidebar_fit_apply().                                              */
#define SB_FIT_MIN_WIDTH 160

/* The most of the paned the sidebar may take when fitting itself to its
 * content.  A deeply nested branch can be arbitrarily wide, and the notes
 * pane still has to be usable.                                             */
#define SB_FIT_MAX_PERCENT 50

/* sb_fit_ctx — working state passed through gtk_tree_model_foreach() for
 * the sidebar width measurement walk.                                       */
typedef struct {
    PangoLayout *lay;   /* reused layout (same font as the sidebar)          */
    gint         max_w; /* running maximum row pixel width                   */
    GtkTreeView *view;  /* set to measure only rows the user can SEE (every
                         * ancestor expanded); NULL measures the whole model */
} SbFitCtx;

/* sb_row_onscreen() — TRUE when every ANCESTOR of `path` is expanded, i.e.
 * the row is actually drawn.  gtk_tree_view_row_expanded() answers only for
 * the node itself, and GTK remembers the expanded flag of a row nested
 * inside a collapsed parent, so the whole chain has to be walked.
 *   view — the sidebar tree view.
 *   path — the row to test.
 * Returns TRUE if the row is on screen (top-level rows always are).         */
static gboolean
sb_row_onscreen(GtkTreeView *view, GtkTreePath *path)
{
    GtkTreePath *up  = gtk_tree_path_copy(path);
    gboolean     vis = TRUE;
    while (gtk_tree_path_get_depth(up) > 1) {
        gtk_tree_path_up(up);
        if (!gtk_tree_view_row_expanded(view, up)) {
            vis = FALSE;
            break;
        }
    }
    gtk_tree_path_free(up);
    return vis;
}

/* sb_fit_measure() — foreach callback: measure one sidebar row and update
 * the running maximum in ctx->max_w.  Rows hidden inside a collapsed
 * parent are skipped when ctx->view is set.
 *   model / path / iter — standard foreach signature.
 *   data                — SbFitCtx *.
 * Returns FALSE to continue the walk.                                       */
static gboolean
sb_fit_measure(GtkTreeModel *model, GtkTreePath *path,
               GtkTreeIter *iter, gpointer data)
{
    SbFitCtx *ctx  = data;
    gchar    *name = NULL;
    gint      kind;

    if (ctx->view != NULL && !sb_row_onscreen(ctx->view, path))
        return FALSE;

    gtk_tree_model_get(model, iter, SB_NAME, &name, SB_KIND, &kind, -1);
    if (name && *name) {
        PangoAttrList *al = pango_attr_list_new();
        if (sb_kind_is_section(kind))
            pango_attr_list_insert(al,
                pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_layout_set_attributes(ctx->lay, al);
        pango_attr_list_unref(al);
        pango_layout_set_text(ctx->lay, name, -1);

        gint tw, th;
        pango_layout_get_pixel_size(ctx->lay, &tw, &th);

        /* 22 px per depth level covers the expander column width +
         * level-indentation; 10 px base for cell left/right padding.
         * Measured on GTK3; unverified against GTK4's Default theme.       */
        gint depth = gtk_tree_path_get_depth(path);
        gint row_w = tw + depth * 22 + 10;
        if (row_w > ctx->max_w)
            ctx->max_w = row_w;
    }
    g_free(name);
    return FALSE;
}

/* on_sidebar_fit_to_content() — idle callback: set the paned divider to the
 * sidebar tree view's content width, measured with Pango so that ellipsizing
 * on the cell renderer doesn't cause get_preferred_width() to return a tiny
 * value.  Runs once after the window is realized and the model is populated. */
static gboolean
on_sidebar_fit_to_content(gpointer user_data)
{
    OnLibrary *lw = user_data;
    SbFitCtx ctx;
    ctx.lay   = gtk_widget_create_pango_layout(GTK_WIDGET(lw->sidebar), NULL);
    ctx.max_w = SB_FIT_MIN_WIDTH;   /* never collapse to an unusable width */
    ctx.view  = NULL;  /* first show: size to the whole tree, collapsed or not */
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_fit_measure, &ctx);
    g_object_unref(ctx.lay);
    gtk_paned_set_position(GTK_PANED(lw->sidebar_paned), ctx.max_w);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * sidebar_fit_apply() — size the sidebar divider so the VISIBLE rows fit
 * exactly, when the "fit to content" setting is on.
 *
 * Symmetric: expanding a folder widens the pane, collapsing one gives the
 * width back.  Turning the setting ON therefore hands the divider over to
 * this function, and a width the user dragged is not preserved across the
 * next expand or collapse — that is the deal the setting makes, and it is
 * why it ships off.
 *
 *   lw — library window state.
 * ------------------------------------------------------------------------- */
static void
sidebar_fit_apply(OnLibrary *lw)
{
    if (!lw->app->sidebar_fit_content)
        return;

    SbFitCtx ctx;
    ctx.lay   = gtk_widget_create_pango_layout(GTK_WIDGET(lw->sidebar), NULL);
    ctx.max_w = SB_FIT_MIN_WIDTH;    /* the floor doubles as the start      */
    ctx.view  = lw->sidebar;         /* on-screen rows only                 */
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_fit_measure, &ctx);
    g_object_unref(ctx.lay);

    gint avail = gtk_widget_get_width(GTK_WIDGET(lw->sidebar));
    if (avail <= 1)
        return;                      /* not realized yet                    */

    /* The DIFFERENCE is applied to the current divider position rather than
     * the measured width being used as the position: whatever sits between
     * the pane edge and the tree view — the scrolled window's vertical
     * scrollbar, which comes and goes as folders are expanded and collapsed
     * — is then carried along without being measured.  Correct in both
     * directions, and it reads the scrollbar as it IS: this runs from a
     * default-priority idle, which GTK services after its own resize
     * (HIGH_IDLE+10) has re-laid-out the tree.                             */
    gint pos  = gtk_paned_get_position(GTK_PANED(lw->sidebar_paned));
    gint want = pos + (ctx.max_w - avail);

    gint full = gtk_widget_get_width(lw->sidebar_paned);
    if (full > 0) {
        gint cap = full * SB_FIT_MAX_PERCENT / 100;
        if (want > cap)
            want = cap;
    }
    if (want < SB_FIT_MIN_WIDTH)
        want = SB_FIT_MIN_WIDTH;
    if (want != pos)
        gtk_paned_set_position(GTK_PANED(lw->sidebar_paned), want);
}

/* sidebar_fit_idle() — idle body of sidebar_fit_queue().                    */
static gboolean
sidebar_fit_idle(gpointer user_data)
{
    OnLibrary *lw = user_data;
    lw->sb_fit_idle = 0;
    sidebar_fit_apply(lw);
    return G_SOURCE_REMOVE;
}

/* sidebar_fit_queue() — run sidebar_fit_apply() once the expand or collapse
 * has been laid out.  Deferred because the signals fire before the tree view
 * has re-measured, and coalesced because a model rebuild's expansion-restore
 * walk emits row-expanded once per restored row.
 *   lw — library window state.                                             */
static void
sidebar_fit_queue(OnLibrary *lw)
{
    if (!lw->app->sidebar_fit_content || lw->sb_fit_idle != 0)
        return;
    lw->sb_fit_idle = g_idle_add(sidebar_fit_idle, lw);
}

/* on_sidebar_row_toggled() — "row-expanded" and "row-collapsed" handler:
 * between them, THE trigger for fitting the sidebar to its content.  The
 * expanded half also covers a model rebuild, whose expansion-restore walk
 * expands rows through the same signal.                                    */
static void
on_sidebar_row_toggled(GtkTreeView *view, GtkTreeIter *iter,
                       GtkTreePath *path, gpointer user_data)
{
    (void)view;
    (void)iter;
    (void)path;
    sidebar_fit_queue(user_data);
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
        sidebar_fit_queue(lw);
}

/* ---------------------------------------------------------------------------
 * library_build_sidebar() — build lw->sidebar (GtkTreeView) and
 * lw->sidebar_box (its scroll container), ready to be packed into the paned.
 * ------------------------------------------------------------------------- */
static void
library_build_sidebar(OnLibrary *lw)
{
    lw->sidebar = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->sidebar_store)));
    gtk_tree_view_set_headers_visible(lw->sidebar, FALSE);
    /* No GTK type-ahead popup: set_model auto-picks the first column
     * transformable to string as the search column — here the int id,
     * so typing raised a search box that matched nothing.               */
    gtk_tree_view_set_enable_search(lw->sidebar, FALSE);
    {
        /* Ellipsizing names keeps the pane's MINIMUM width small: without
         * it the widest row dictates the minimum and the divider can't be
         * dragged past it.                                                 */
        GtkCellRenderer *name_cell = gtk_cell_renderer_text_new();
        g_object_set(name_cell,
                     "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *name_col =
            gtk_tree_view_column_new_with_attributes(
                "Name", name_cell, "text", SB_NAME, NULL);
        /* Section rows (Notes root, Tags, Pinned Notes) render bold.       */
        gtk_tree_view_column_set_cell_data_func(
            name_col, name_cell, sidebar_name_cell_func, NULL, NULL);
        gtk_tree_view_column_set_sizing(name_col,
                                        GTK_TREE_VIEW_COLUMN_AUTOSIZE);
        gtk_tree_view_append_column(lw->sidebar, name_col);
    }

    /* Sidebar palette and drop indicator: the "notes-sidebar" rules in
     * library_install_css (see its banner for the colours).                */
    gtk_widget_add_css_class(GTK_WIDGET(lw->sidebar), "notes-sidebar");

    GtkTreeSelection *sb_sel = gtk_tree_view_get_selection(lw->sidebar);
    gtk_tree_selection_set_select_function(sb_sel, sidebar_select_func,
                                           NULL, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_selection_changed), lw);

    /* Drag and drop (see the DnD section): folder rows are a drag source,
     * and the sidebar is THE drop target — for note rows from either notes
     * view and for its own folder rows.  Everything is our own controllers;
     * the deprecated model DnD is not used for content at all.            */
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_sidebar_drag_prepare),
                     lw);
    gtk_widget_add_controller(GTK_WIDGET(lw->sidebar),
                              GTK_EVENT_CONTROLLER(drag));

    /* D5 (measured on GTK 4.22.4): gtk_tree_view_set_drag_dest_row(), the
     * only way to show the drop indicator, SEGFAULTS on the next paint
     * unless enable_model_drag_dest has run — only that creates the
     * "dndtarget" CSS node the indicator is drawn through.  Enabled with an
     * EMPTY format set: the built-in GtkDropTargetAsync then matches
     * nothing and never fires, while the node exists for our target.      */
    GdkContentFormats *none = gdk_content_formats_new(NULL, 0);
    gtk_tree_view_enable_model_drag_dest(lw->sidebar, none, 0);
    gdk_content_formats_unref(none);

    GtkDropTarget *drop = gtk_drop_target_new(ON_TYPE_DRAG_ROWS,
                                              GDK_ACTION_MOVE);
    /* Preload: the content is read when the drag enters, so every motion
     * can validate against what is actually being dragged (a local drag
     * loads synchronously — see on_sidebar_drop_motion).                  */
    gtk_drop_target_set_preload(drop, TRUE);
    g_signal_connect(drop, "enter",  G_CALLBACK(on_sidebar_drop_motion), lw);
    g_signal_connect(drop, "motion", G_CALLBACK(on_sidebar_drop_motion), lw);
    g_signal_connect(drop, "leave",  G_CALLBACK(on_sidebar_drop_leave),  lw);
    g_signal_connect(drop, "drop",   G_CALLBACK(on_sidebar_drop),        lw);
    gtk_widget_add_controller(GTK_WIDGET(lw->sidebar),
                              GTK_EVENT_CONTROLLER(drop));

    capture_click_gesture(GTK_WIDGET(lw->sidebar), GDK_BUTTON_SECONDARY,
                          G_CALLBACK(on_sidebar_pressed), lw);
    g_signal_connect(lw->sidebar, "row-expanded",
                     G_CALLBACK(on_sidebar_row_toggled), lw);
    g_signal_connect(lw->sidebar, "row-collapsed",
                     G_CALLBACK(on_sidebar_row_toggled), lw);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(sidebar_scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                  GTK_WIDGET(lw->sidebar));
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);

    /* Sidebar column: a fixed spacer, then the tree (all buttons live in
     * the unified toolbar above the paned).  Its minimum width is whatever
     * the tree content needs — the scrolled window never scrolls
     * horizontally, so it requests the tree's full natural width.          */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Top padding, so the first row's text sits level with the text in the
     * notes list's column headers (the sidebar has no headers of its own).
     * It is a SPACER WIDGET rather than CSS padding: GtkScrolledWindow
     * ignores padding when allocating its child, and a margin on the tree
     * view would scroll away with it.  Painted in the sidebar grey so the
     * strip reads as part of the pane: a GtkBox has no background of its
     * own, so library_install_css gives it the tree view's backdrop from
     * the ONE declaration both share.                                      */
    GtkWidget *sidebar_pad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar_pad, -1, SB_TOP_PAD);
    gtk_widget_add_css_class(sidebar_pad, "notes-sidebar-pad");
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_pad);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_scroll);
    lw->sidebar_box = sidebar_box;   /* for the toolbar show/hide toggle    */
}

/* ---------------------------------------------------------------------------
 * library_build_notes_list() — build lw->notes_list (GtkTreeView) with its
 * four columns, sort functions, column layout, gestures and drag source;
 * returns the scroll container ready to be added to the notes stack.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_notes_list(OnLibrary *lw)
{
    lw->notes_list = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    /* No GTK type-ahead popup (auto-picked search column, see quirk 16).  */
    gtk_tree_view_set_enable_search(lw->notes_list, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(lw->notes_list), "notes-columns");
    {
        /* Title: no static attribute binding — the cell data function drives
         * both the row tint and the compact/comfortable rendering.          */
        GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
        g_object_set(r1, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *c1 = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(c1, "Title");
        gtk_tree_view_column_pack_start(c1, r1, TRUE);
        gtk_tree_view_column_set_cell_data_func(c1, r1,
                                                notes_title_cell_func,
                                                lw, NULL);
        gtk_tree_view_column_set_resizable(c1, TRUE);
        gtk_tree_view_append_column(lw->notes_list, c1);

        /* Path column: the note's folder location ("/Work/Projects").
         * Fixed width + ellipsize so deep trees can't blow the layout
         * out; the divider is user-draggable like the others.             */
        GtkCellRenderer *rp = gtk_cell_renderer_text_new();
        g_object_set(rp,
                     "ellipsize", PANGO_ELLIPSIZE_END,
                     "xpad",      10,
                     NULL);
        GtkTreeViewColumn *cp =
            gtk_tree_view_column_new_with_attributes("Path", rp,
                                                     "text", NL_PATH,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(cp, rp, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_sizing(cp, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(cp, 180);
        gtk_tree_view_column_set_resizable(cp, TRUE);
        gtk_tree_view_append_column(lw->notes_list, cp);

        GtkCellRenderer *r2 = gtk_cell_renderer_text_new();
        /* Horizontal padding so the timestamps don't hug the column
         * edges.                                                           */
        g_object_set(r2, "xpad", 10, NULL);
        GtkTreeViewColumn *c2 =
            gtk_tree_view_column_new_with_attributes("Modified", r2,
                                                     "text", NL_MODIFIED,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(c2, r2, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_resizable(c2, TRUE);
        gtk_tree_view_append_column(lw->notes_list, c2);

        /* Created column — built HIDDEN: a saved list_columns without a
         * "created" entry (every pre-existing ini) keeps the built state,
         * so the column stays off until toggled in the header menu.        */
        GtkCellRenderer *rc = gtk_cell_renderer_text_new();
        g_object_set(rc, "xpad", 10, NULL);
        GtkTreeViewColumn *cc =
            gtk_tree_view_column_new_with_attributes("Created", rc,
                                                     "text", NL_CREATED,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(cc, rc, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_resizable(cc, TRUE);
        gtk_tree_view_column_set_visible(cc, FALSE);
        gtk_tree_view_append_column(lw->notes_list, cc);
        gtk_tree_view_column_set_expand(c1, TRUE);

        /* Clickable headers: Title sorts alphabetically, Modified sorts
         * most-recent-first (drag reordering works while unsorted).        */
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_TITLE,
            sort_by_title, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_UPDATED,
            sort_by_time, GINT_TO_POINTER(NL_UPDATED), NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_PATH,
            sort_by_path, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_CREATED_RAW,
            sort_by_time, GINT_TO_POINTER(NL_CREATED_RAW), NULL);
        gtk_tree_view_column_set_sort_column_id(c1, NL_TITLE);
        gtk_tree_view_column_set_sort_column_id(cp, NL_PATH);
        gtk_tree_view_column_set_sort_column_id(c2, NL_UPDATED);
        gtk_tree_view_column_set_sort_column_id(cc, NL_CREATED_RAW);

        /* Default sort: Modified with the most recent on top
         * (sort_by_time is deliberately inverted, so ASCENDING =
         * newest first).  The headers only ever cycle ascending and
         * descending, so the list is ALWAYS sorted; in-list drag
         * reordering, which a sorted list store refuses, is therefore
         * not offered at all — a note drag is a move to a folder.        */
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(lw->notes_store), NL_UPDATED,
            GTK_SORT_ASCENDING);

        /* Column layout management: drag a header to reorder, right-click
         * one for the show/hide menu; the layout persists in the ini
         * (applied below once all columns exist).  Each column carries
         * its renderer so autofit can move the ellipsis around.           */
        struct { GtkTreeViewColumn *col; GtkCellRenderer *cell;
                 const gchar *key; } COLS[N_LIST_COLUMNS] = {
            { c1, r1, "title" }, { cp, rp, "path" }, { c2, r2, "modified" },
            { cc, rc, "created" },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(COLS); i++) {
            g_object_set_data(G_OBJECT(COLS[i].col), "on-colkey",
                              (gpointer)COLS[i].key);
            g_object_set_data(G_OBJECT(COLS[i].col), "on-cell",
                              COLS[i].cell);
            column_header_menu_add(lw, COLS[i].col, lw->notes_list);
        }
    }
    g_object_set_data(G_OBJECT(lw->notes_list), "on-colcfg",
                      (gpointer)"list_columns");
    g_object_set_data(G_OBJECT(lw->notes_list), "on-ncols",
                      GINT_TO_POINTER(N_LIST_COLUMNS));
    g_object_set_data(G_OBJECT(lw->notes_list), "on-coldefault",
                      (gpointer)"path:0,title:1,modified:1,created:0");
    view_columns_apply(lw->notes_list);  /* saved order + visibility        */
    {
        /* Autofit is the default; only an explicit "0" turns it off.       */
        lw->list_autofit = on_app_config_get_bool("list_autofit", TRUE);
        if (lw->list_autofit)
            list_autofit_apply(lw);
    }
    /* Connected only now, so applying the saved layout doesn't re-persist
     * it; from here on every header drag writes the ini.                   */
    g_signal_connect(lw->notes_list, "columns-changed",
                     G_CALLBACK(on_view_columns_changed), lw);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(lw->notes_list),
        GTK_SELECTION_MULTIPLE);
    g_signal_connect(gtk_tree_view_get_selection(lw->notes_list),
                     "changed",
                     G_CALLBACK(on_notes_selection_status), lw);

    g_signal_connect(lw->notes_list, "row-activated",
                     G_CALLBACK(on_note_list_activated), lw);

    /* One capture-phase click gesture for ANY button: the quirk-15 veto on
     * a primary press and the right-click menu (see on_notes_list_pressed);
     * released / cancel end the veto.                                      */
    GtkGesture *click = capture_click_gesture(
        GTK_WIDGET(lw->notes_list), 0, G_CALLBACK(on_notes_list_pressed), lw);
    g_signal_connect(click, "released",
                     G_CALLBACK(on_notes_list_released), lw);
    g_signal_connect(click, "cancel", G_CALLBACK(on_notes_list_cancel), lw);

    /* Drag source: the selected notes, dropped on a sidebar folder.        */
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare",
                     G_CALLBACK(on_notes_list_drag_prepare), lw);
    gtk_widget_add_controller(GTK_WIDGET(lw->notes_list),
                              GTK_EVENT_CONTROLLER(drag));

    GtkWidget *list_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(list_scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll),
                                  GTK_WIDGET(lw->notes_list));
    return list_scroll;
}

/* ---------------------------------------------------------------------------
 * library_build_notes_grid() — build lw->notes_grid (GtkIconView) with its
 * thumbnail + title cell layout, gesture and drag source; returns the
 * scroll container.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_notes_grid(OnLibrary *lw)
{
    lw->notes_grid = GTK_ICON_VIEW(
        gtk_icon_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    gtk_widget_add_css_class(GTK_WIDGET(lw->notes_grid), "notes-grid");
    {
        /* Custom cell layout: the thumbnail texture with the note title
         * as a real text label underneath.                                 */
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   pix, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       pix, "texture", NL_THUMB, NULL);

        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        g_object_set(txt,
                     "xalign",      0.5,
                     "alignment",   PANGO_ALIGN_CENTER,
                     "wrap-mode",   PANGO_WRAP_WORD_CHAR,
                     "wrap-width",  THUMB_SIZE,
                     "weight",      PANGO_WEIGHT_BOLD,
                     NULL);
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   txt, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       txt, "text", NL_TITLE, NULL);
    }
    gtk_icon_view_set_item_width(lw->notes_grid, THUMB_SIZE);
    gtk_icon_view_set_selection_mode(lw->notes_grid,
                                     GTK_SELECTION_MULTIPLE);
    g_signal_connect(lw->notes_grid, "selection-changed",
                     G_CALLBACK(on_notes_selection_status), lw);
    g_signal_connect(lw->notes_grid, "item-activated",
                     G_CALLBACK(on_note_grid_activated), lw);
    capture_click_gesture(GTK_WIDGET(lw->notes_grid), GDK_BUTTON_SECONDARY,
                          G_CALLBACK(on_notes_grid_pressed), lw);

    /* Drag source: the selected notes, dropped on a sidebar folder.  The
     * icon view's own model drag source is NOT enabled, so its built-in
     * drag handling stays out of the way (it only runs when it is).       */
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare",
                     G_CALLBACK(on_notes_grid_drag_prepare), lw);
    gtk_widget_add_controller(GTK_WIDGET(lw->notes_grid),
                              GTK_EVENT_CONTROLLER(drag));

    GtkWidget *grid_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(grid_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(grid_scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(grid_scroll),
                                  GTK_WIDGET(lw->notes_grid));
    return grid_scroll;
}

/* ---------------------------------------------------------------------------
 * library_build_actions_view() — build lw->actions_store and lw->actions_view
 * with their three columns, sort functions, and column layout; returns the
 * scroll container ready to be added to the notes stack.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_actions_view(OnLibrary *lw)
{
    lw->actions_store = gtk_list_store_new(AL_N_COLS,
                                           G_TYPE_INT64,   /* AL_NOTE_ID   */
                                           G_TYPE_INT,     /* AL_ORD       */
                                           G_TYPE_BOOLEAN, /* AL_DONE      */
                                           G_TYPE_STRING,  /* AL_TEXT      */
                                           G_TYPE_STRING,  /* AL_DUE       */
                                           G_TYPE_INT64);  /* AL_DUE_RAW   */
    lw->actions_view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->actions_store)));
    gtk_tree_view_set_enable_search(lw->actions_view, FALSE); /* quirk 16   */
    gtk_widget_add_css_class(GTK_WIDGET(lw->actions_view), "notes-columns");
    {
        /* Untitled checkbox column + the item text + the due date; done
         * rows also render struck through, matching the editor.            */
        GtkCellRenderer *tog = gtk_cell_renderer_toggle_new();
        g_signal_connect(tog, "toggled",
                         G_CALLBACK(on_action_toggled), lw);
        GtkTreeViewColumn *cd = gtk_tree_view_column_new_with_attributes(
            "", tog, "active", AL_DONE, NULL);
        gtk_tree_view_append_column(lw->actions_view, cd);

        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 10,
                     NULL);
        GtkTreeViewColumn *ca = gtk_tree_view_column_new_with_attributes(
            "Action", txt,
            "text",          AL_TEXT,
            "strikethrough", AL_DONE,
            NULL);
        gtk_tree_view_column_set_expand(ca, TRUE);
        gtk_tree_view_column_set_resizable(ca, TRUE);
        gtk_tree_view_append_column(lw->actions_view, ca);

        GtkCellRenderer *rdue = gtk_cell_renderer_text_new();
        g_object_set(rdue, "xpad", 10, NULL);
        GtkTreeViewColumn *cdue = gtk_tree_view_column_new_with_attributes(
            "Due Date", rdue,
            "text",          AL_DUE,
            "strikethrough", AL_DONE,
            NULL);
        gtk_tree_view_column_set_cell_data_func(cdue, rdue,
            action_due_color_func, NULL, NULL);
        gtk_tree_view_column_set_resizable(cdue, TRUE);
        gtk_tree_view_append_column(lw->actions_view, cdue);

        /* Clickable headers, notes-list style.  AL_DONE sorts with the
         * default boolean compare.                                         */
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->actions_store), AL_TEXT,
            sort_actions_by_text, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->actions_store), AL_DUE_RAW,
            sort_actions_by_due, NULL, NULL);
        gtk_tree_view_column_set_sort_column_id(cd, AL_DONE);
        gtk_tree_view_column_set_sort_column_id(ca, AL_TEXT);
        gtk_tree_view_column_set_sort_column_id(cdue, AL_DUE_RAW);

        /* Column layout: the notes list's conventions — drag a header to
         * reorder, right-click for show/hide; persists per view.           */
        struct { GtkTreeViewColumn *col; const gchar *key;
                 const gchar *label; } ACOLS[N_ACTION_COLUMNS] = {
            { cd,   "done",   "Done" },   /* titleless: needs a menu label */
            { ca,   "action", NULL },
            { cdue, "due",    NULL },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(ACOLS); i++) {
            g_object_set_data(G_OBJECT(ACOLS[i].col), "on-colkey",
                              (gpointer)ACOLS[i].key);
            if (ACOLS[i].label != NULL)
                g_object_set_data(G_OBJECT(ACOLS[i].col), "on-collabel",
                                  (gpointer)ACOLS[i].label);
            column_header_menu_add(lw, ACOLS[i].col, lw->actions_view);
        }
    }
    g_object_set_data(G_OBJECT(lw->actions_view), "on-colcfg",
                      (gpointer)"action_columns");
    g_object_set_data(G_OBJECT(lw->actions_view), "on-ncols",
                      GINT_TO_POINTER(N_ACTION_COLUMNS));
    g_object_set_data(G_OBJECT(lw->actions_view), "on-coldefault",
                      (gpointer)"done:1,action:1,due:1");
    view_columns_apply(lw->actions_view);
    g_signal_connect(lw->actions_view, "columns-changed",
                     G_CALLBACK(on_view_columns_changed), lw);
    g_signal_connect(lw->actions_view, "row-activated",
                     G_CALLBACK(on_action_row_activated), lw);

    GtkWidget *actions_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(actions_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(actions_scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(actions_scroll),
                                  GTK_WIDGET(lw->actions_view));
    return actions_scroll;
}

/* ---------------------------------------------------------------------------
 * library_build_notes_pane() — build the three note views (list, grid,
 * actions) and assemble them into lw->stack, ready to be packed into the
 * notes paned.
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
 * 1. Grid thumbnails.  GtkCellRendererPixbuf hands a texture to GTK's icon
 *    helper, which paints a paintable at MIN(cell width, -gtk-icon-size) —
 *    and -gtk-icon-size is 16px unless CSS says otherwise (measured on
 *    4.22 with a pixel probe: a 140 px texture painted 16 px square while
 *    the cell reserved 140).  The renderer saves the icon view's style
 *    context with the "image" class for that paint, so the rule targets
 *    `iconview.notes-grid.image`.
 * 2. Dialog buttons.  GtkDialog's action area has no padding of its own
 *    in GTK4 (the old action-area border went with gtk_dialog_get_action_area),
 *    so the buttons sat flush against the bottom-right corner.
 * 3. Grid hover.  The icon view paints each item's background and frame
 *    on its own node saved with the "cell" class and the :hover state for
 *    the item under the pointer; GTK3's rendering gave that a visible
 *    outline, GTK4's theme has no rule for it, so the outline is ours.
 * 4. Sidebar palette.  The backdrop (rows AND the empty area below them —
 *    the tree view paints the whole widget) is the theme's window/toolbar
 *    background taken down a step (SB_BG_SHADE), so the pane sits just
 *    behind the toolbar above it and reads as distinct from the white
 *    notes list without pinning a grey of its own; a tree view left alone
 *    paints the white theme BASE colour.  The spacer strip above the tree
 *    (library_build_sidebar) shares the declaration.  Then muted grey text
 *    and a blue selection bar with white text.  Verified on GTK 4.22's
 *    compiled Default theme: it still defines @theme_bg_color (#f6f5f4
 *    light) and still parses shade() — both DEPRECATED since 4.16 (they
 *    warn only under GTK_DEBUG=css) but the theme exports no CSS variables
 *    to replace them with.  Beware that an UNDEFINED colour name is NOT a
 *    parse error — it silently renders transparent.
 * 5. Sidebar drop indicator.  GTK4 draws it as a "dndtarget" sub-node of
 *    the tree view carrying a position class (before / after / into) with
 *    the :drop(active) state, framing the row under the pointer — a 2px
 *    line in the selection blue: top edge for BEFORE, bottom for AFTER, a
 *    full box for INTO.  The node exists only once
 *    enable_model_drag_dest has run (D5).
 * 6. The emoji entry of the folder dialog: one emoji wide — the theme's
 *    entry min-width would otherwise span the dialog (D22).
 * 7. The AI pane's two compact header buttons.
 * 8. The sidebar/notes divider: a 6 px handle (wide-handle mode gives the
 *    separator node a 5 px theme floor; min-WIDTH is the lever on a
 *    horizontal paned).
 * 9. The notes list / Action Items headers: no left border on the first
 *    visible column, which would double the divider's edge line.
 * ------------------------------------------------------------------------- */
static void
library_install_css(void)
{
    static gboolean installed = FALSE;
    if (installed)
        return;
    installed = TRUE;
    gchar *css = g_strdup_printf(
        "iconview.notes-grid.image { -gtk-icon-size: %dpx; }"
        "window.notes-dialog .dialog-action-area {"
        "  padding: 0 12px 12px 12px;"
        "}"
        /* The outline on every hovered cell, selected or not; the tint
         * only on an UNSELECTED one: this rule outranks the theme's
         * iconview:selected (more specific), and a near-transparent tint
         * under text the selected state has turned white was an invisible
         * title until the mouse left the cell.                           */
        "iconview.notes-grid.cell:hover {"
        "  border: 1px solid alpha(black, 0.4);"   /* not currentColor: that is white on a selected cell */
        "  border-radius: 4px;"
        "}"
        "iconview.notes-grid.cell:hover:not(:selected) {"
        "  background-color: alpha(currentColor, 0.06);"
        "}"
        "treeview.notes-sidebar, box.notes-sidebar-pad {"
        "  background-color: shade(@theme_bg_color, " SB_BG_SHADE ");"
        "}"
        "treeview.notes-sidebar { color: rgb(65,65,65); }"
        "treeview.notes-sidebar:selected {"
        "  background-color: rgb(86,131,224);"
        "  color: white;"
        "}"
        "treeview.notes-sidebar > dndtarget:drop(active) {"
        "  border-color: rgb(86,131,224);"
        "  border-width: 2px;"
        "  border-style: solid;"
        "}"
        "treeview.notes-sidebar > dndtarget:drop(active).before {"
        "  border-style: solid none none none;"
        "}"
        "treeview.notes-sidebar > dndtarget:drop(active).after {"
        "  border-style: none none solid none;"
        "}"
        "entry.notes-emoji-entry { font-size: 18px; min-width: 0; }"
        "button.notes-ai-button {"
        "  padding: 0 4px; min-height: 0; font-size: 85%%;"
        "}"
        "paned.notes-split > separator { min-width: 6px; }"
        /* The theme gives every column header a LEFT + bottom border
         * (`border-style: none none solid solid`), so the first column's
         * sat 4 px from the divider's own edge line as a second line.
         * Hidden columns do not count for :first-child (invisible CSS
         * nodes are skipped), so this is the first VISIBLE header.      */
        "treeview.notes-columns > header > button:first-child {"
        "  border-left-style: none;"
        "}",
        THUMB_SIZE);
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

    /* --- models -----------------------------------------------------------*/
    lw->sidebar_store = gtk_tree_store_new(SB_N_COLS,
                                           G_TYPE_INT,     /* SB_KIND      */
                                           G_TYPE_INT64,   /* SB_ID        */
                                           G_TYPE_STRING,  /* SB_NAME      */
                                           G_TYPE_STRING); /* SB_RAW       */
    lw->notes_store = gtk_list_store_new(
        NL_N_COLS,
        G_TYPE_INT64,                    /* NL_ID                          */
        G_TYPE_STRING,                   /* NL_TITLE                       */
        G_TYPE_STRING,                   /* NL_MODIFIED                    */
        GDK_TYPE_TEXTURE,                /* NL_THUMB                       */
        G_TYPE_INT64,                    /* NL_UPDATED                     */
        G_TYPE_STRING,                   /* NL_PATH                        */
        G_TYPE_STRING,                   /* NL_CREATED                     */
        G_TYPE_INT64,                    /* NL_CREATED_RAW                 */
        G_TYPE_STRING);                  /* NL_PREVIEW                     */

    /* --- panes + status bar -----------------------------------------------*/
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
    /* The actions must exist before anything that names them is built —
     * the menubar, the toolbar — so their items come up sensitive.        */
    library_install_actions(lw);
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

    /* Fit the sidebar pane to its content width on first show.  Done in an
     * idle so the tree view is fully realized and has measured its rows.     */
    g_idle_add(on_sidebar_fit_to_content, lw);

    return lw->window;
}
