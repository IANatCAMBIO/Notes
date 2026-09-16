/*
 * spike.c — GTK4 migration Phase 0 spike.
 *
 * Throwaway program answering the two questions the port estimate hangs
 * on, plus one CLAUDE.md quirk.  It prints what it measures; the human
 * reads the log and writes the verdict into GTK4_MIGRATION.md.
 *
 *   Q1  Does the DEPRECATED GtkTreeView work with the GTK4 DnD controllers
 *       (GtkDragSource / GtkDropTarget)?  Specifically: does ::drop fire
 *       exactly once, at release, with real pointer coordinates?
 *       (CLAUDE.md quirk #13 was a workaround for GTK3's drag-data-received
 *       firing mid-drag on quartz.)
 *   Q2  Does gtk_text_view_add_overlay() replace add_child_in_window()?
 *       A child placed at buffer coordinates must ride scrolling at 1x, and
 *       the view's top margin must be accounted for consistently.
 *       (CLAUDE.md quirks #1 and #2.)
 *   Q3  Does a multi-row selection in a GtkTreeView survive a press-and-drag
 *       on one of its rows?  (CLAUDE.md quirk #15.)
 *
 * Left pane: a folder tree, multi-select, drag any row onto / before / after
 * another.  Right pane: a text view with an embedded image at line 3, a
 * 12 px top margin, and a "copy" overlay label anchored to line 10.
 */

#include <gtk/gtk.h>
#include <stdio.h>

enum {
    COL_NAME,                        /* row label                           */
    COL_ID,                          /* stable integer id carried by drags  */
    N_COLS
};

#define VIEW_TOP_MARGIN   12         /* the editor's top margin, to test     */
#define OVERLAY_LINE      10         /* the line the overlay label anchors to */

/* Per-run state — one window, so file-scope is fine for a spike.            */
static GtkTreeStore *store;
static GtkTreeView  *tree;
static GtkTextView  *view;
static GtkWidget    *overlay_label;
static gint          overlay_buffer_y;   /* buffer y the label was placed at */
static guint         motion_calls;       /* ::motion count for one drag      */
static guint         drop_calls;         /* ::drop count for one drag        */
static gboolean      veto_armed;         /* selection veto is installed      */

/*
 * tree_add — append one row under parent (NULL = root).
 *
 * Inputs:
 *   parent — parent iter or NULL
 *   name   — row label
 *   id     — stable id
 *
 * Output:
 *   the new row's iter, by value.
 */
static GtkTreeIter
tree_add(GtkTreeIter *parent, const gchar *name, gint64 id)
{
    GtkTreeIter it;
    gtk_tree_store_append(store, &it, parent);
    gtk_tree_store_set(store, &it, COL_NAME, name, COL_ID, id, -1);
    return it;
}

/*
 * tree_find_id_under — recursive half of tree_find_id: search node and
 * its subtree for id.
 *
 * Inputs:
 *   node — subtree root
 *   id   — id to find
 *   out  — receives the iter on success
 *
 * Output:
 *   TRUE if found.
 */
static gboolean
tree_find_id_under(GtkTreeIter *node, gint64 id, GtkTreeIter *out)
{
    GtkTreeModel *m = GTK_TREE_MODEL(store);
    GtkTreeIter child;
    gint64 v;

    gtk_tree_model_get(m, node, COL_ID, &v, -1);
    if (v == id) {
        *out = *node;
        return TRUE;
    }
    if (gtk_tree_model_iter_children(m, &child, node)) {
        do {
            if (tree_find_id_under(&child, id, out))
                return TRUE;
        } while (gtk_tree_model_iter_next(m, &child));
    }
    return FALSE;
}

/*
 * tree_find_id — locate the row carrying id anywhere in the store.
 *
 * Inputs:
 *   id  — id to find
 *   out — receives the iter on success
 *
 * Output:
 *   TRUE if found.
 */
static gboolean
tree_find_id(gint64 id, GtkTreeIter *out)
{
    GtkTreeIter it;

    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it))
        return FALSE;
    do {
        if (tree_find_id_under(&it, id, out))
            return TRUE;
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it));
    return FALSE;
}

/*
 * veto_select — GtkTreeSelectionFunc that refuses every selection change.
 * Installed for the duration of a press on an already-selected row so the
 * tree view's own click gesture cannot collapse a multi-selection before a
 * drag has had the chance to start (CLAUDE.md quirk #15, GTK4 edition).
 *
 * Output:
 *   FALSE always — the selection may not change.
 */
static gboolean
veto_select(GtkTreeSelection *sel, GtkTreeModel *m, GtkTreePath *p,
            gboolean cur, gpointer data)
{
    (void)sel; (void)m; (void)p; (void)cur; (void)data;
    return FALSE;
}

/*
 * veto_lift — remove the veto if it is installed.
 */
static void
veto_lift(void)
{
    if (!veto_armed)
        return;
    gtk_tree_selection_set_select_function(gtk_tree_view_get_selection(tree),
                                           NULL, NULL, NULL);
    veto_armed = FALSE;
}

/*
 * on_press_capture — GtkGestureClick::pressed in the CAPTURE phase, so it
 * runs BEFORE the tree view's own bubble-phase click gesture.  Arms the
 * veto when an unmodified press lands on a row that is already part of a
 * multi-selection.
 *
 * Inputs:
 *   g       — the gesture
 *   n_press — click count
 *   x, y    — widget coordinates
 */
static void
on_press_capture(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                 gpointer data)
{
    (void)data;
    GtkTreePath *path = NULL;
    gint bx, by;

    if (n_press != 1)
        return;
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_META_MASK))
        return;

    gtk_tree_view_convert_widget_to_bin_window_coords(tree, (gint)x, (gint)y,
                                                      &bx, &by);
    if (!gtk_tree_view_get_path_at_pos(tree, bx, by, &path, NULL, NULL, NULL))
        return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(tree);
    if (gtk_tree_selection_path_is_selected(sel, path) &&
        gtk_tree_selection_count_selected_rows(sel) >= 2) {
        gtk_tree_selection_set_select_function(sel, veto_select, NULL, NULL);
        veto_armed = TRUE;
        g_print("Q3  press on a selected row with %d selected: veto armed\n",
                gtk_tree_selection_count_selected_rows(sel));
    }
    gtk_tree_path_free(path);
}

/*
 * on_release_capture — GtkGestureClick::released.  A plain click (no drag
 * began, so the veto is still armed) collapses the selection to the row
 * under the pointer, which is what the vetoed press would have done.
 */
static void
on_release_capture(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                   gpointer data)
{
    (void)g; (void)n_press; (void)data;
    GtkTreePath *path = NULL;
    gint bx, by;

    if (!veto_armed)
        return;
    veto_lift();
    gtk_tree_view_convert_widget_to_bin_window_coords(tree, (gint)x, (gint)y,
                                                      &bx, &by);
    if (gtk_tree_view_get_path_at_pos(tree, bx, by, &path, NULL, NULL, NULL)) {
        gtk_tree_view_set_cursor(tree, path, NULL, FALSE);
        gtk_tree_path_free(path);
        g_print("Q3  plain click: veto lifted, selection collapsed to 1\n");
    }
}

/*
 * on_cancel_capture — GtkGestureClick::cancel.  The drag source claiming
 * the sequence cancels this gesture; drop the veto without collapsing.
 */
static void
on_cancel_capture(GtkGesture *g, GdkEventSequence *seq, gpointer data)
{
    (void)g; (void)seq; (void)data;
    if (veto_armed) {
        veto_lift();
        g_print("Q3  click gesture cancelled (drag took over): veto lifted, "
                "selection kept\n");
    }
}

/*
 * on_drag_prepare — GtkDragSource::prepare.  Identify the row under the
 * pointer and hand its id over as the drag content.  Also answers Q3 by
 * reporting how many rows are selected at the moment the drag starts.
 *
 * Inputs:
 *   source — the drag source
 *   x, y   — widget coordinates of the press
 *
 * Output:
 *   a content provider carrying a G_TYPE_INT64, or NULL to refuse the drag.
 */
static GdkContentProvider *
on_drag_prepare(GtkDragSource *source, gdouble x, gdouble y, gpointer data)
{
    (void)source; (void)data;
    GtkTreePath *path = NULL;
    gint bx, by;

    gtk_tree_view_convert_widget_to_bin_window_coords(tree, (gint)x, (gint)y,
                                                      &bx, &by);
    if (!gtk_tree_view_get_path_at_pos(tree, bx, by, &path, NULL, NULL, NULL))
        return NULL;

    GtkTreeIter it;
    gint64 id;
    gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &it, path);
    gtk_tree_model_get(GTK_TREE_MODEL(store), &it, COL_ID, &id, -1);
    gtk_tree_path_free(path);

    veto_lift();                          /* a drag began: keep the selection */
    gint nsel = gtk_tree_selection_count_selected_rows(
        gtk_tree_view_get_selection(tree));
    g_print("Q3  prepare: dragging id=%" G_GINT64_FORMAT
            ", %d row(s) selected at drag start\n", id, nsel);

    motion_calls = 0;
    drop_calls = 0;
    return gdk_content_provider_new_typed(G_TYPE_INT64, id);
}

/*
 * on_drop_motion — GtkDropTarget::motion.  Compute and show the drop row
 * ourselves (the deprecated tree view's built-in dest handling is not used).
 *
 * Inputs:
 *   target — the drop target
 *   x, y   — widget coordinates of the pointer
 *
 * Output:
 *   the action we would perform here, or 0 to refuse.
 */
static GdkDragAction
on_drop_motion(GtkDropTarget *target, gdouble x, gdouble y, gpointer data)
{
    (void)target; (void)data;
    GtkTreePath *path = NULL;
    GtkTreeViewDropPosition pos;

    motion_calls++;
    if (!gtk_tree_view_get_dest_row_at_pos(tree, (gint)x, (gint)y, &path, &pos)) {
        gtk_tree_view_set_drag_dest_row(tree, NULL, GTK_TREE_VIEW_DROP_BEFORE);
        return 0;
    }
    gtk_tree_view_set_drag_dest_row(tree, path, pos);
    gtk_tree_path_free(path);
    return GDK_ACTION_MOVE;
}

/*
 * on_drop_leave — GtkDropTarget::leave.  Clear the indicator.
 */
static void
on_drop_leave(GtkDropTarget *target, gpointer data)
{
    (void)target; (void)data;
    gtk_tree_view_set_drag_dest_row(tree, NULL, GTK_TREE_VIEW_DROP_BEFORE);
}

/*
 * move_row — move the leaf row carrying id relative to dst.
 *
 * Inputs:
 *   id  — id of the row to move
 *   dst — target row (copied first: moving invalidates nothing in a
 *         GtkTreeStore, but dst may itself be the dragged row's sibling)
 *   pos — before / after / into
 *
 * Output:
 *   TRUE if the row was found and moved.
 */
static gboolean
move_row(gint64 id, GtkTreeIter *dst, GtkTreeViewDropPosition pos)
{
    GtkTreeIter src, moved;
    gchar *name;

    if (!tree_find_id(id, &src))
        return FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &src, COL_NAME, &name, -1);
    switch (pos) {
    case GTK_TREE_VIEW_DROP_BEFORE:
        gtk_tree_store_insert_before(store, &moved, NULL, dst); break;
    case GTK_TREE_VIEW_DROP_AFTER:
        gtk_tree_store_insert_after(store, &moved, NULL, dst);  break;
    default:
        gtk_tree_store_append(store, &moved, dst);              break;
    }
    gtk_tree_store_set(store, &moved, COL_NAME, name, COL_ID, id, -1);
    gtk_tree_store_remove(store, &src);
    g_free(name);
    return TRUE;
}

/*
 * on_drop — GtkDropTarget::drop.  THE measurement for Q1: log how many
 * times this ran for the drag and with what coordinates.  Then, as the
 * app does, move the WHOLE selection when the dragged row is part of it
 * (Q3 at drop time), else just the dragged row.  Leaf rows only.
 *
 * Inputs:
 *   target — the drop target
 *   value  — the dragged G_TYPE_INT64 id
 *   x, y   — widget coordinates of the release
 *
 * Output:
 *   TRUE if the drop was accepted.
 */
static gboolean
on_drop(GtkDropTarget *target, const GValue *value, gdouble x, gdouble y,
        gpointer data)
{
    (void)target; (void)data;
    GtkTreePath *path = NULL;
    GtkTreeViewDropPosition pos;
    GtkTreeModel *m = GTK_TREE_MODEL(store);

    drop_calls++;
    g_print("Q1  drop #%u at widget (%.0f,%.0f) after %u motion calls",
            drop_calls, x, y, motion_calls);
    gtk_tree_view_set_drag_dest_row(tree, NULL, GTK_TREE_VIEW_DROP_BEFORE);

    if (!gtk_tree_view_get_dest_row_at_pos(tree, (gint)x, (gint)y, &path, &pos)) {
        g_print(" — no row there, refused\n");
        return FALSE;
    }
    GtkTreeIter dst;
    gtk_tree_model_get_iter(m, &dst, path);

    /* Collect the ids to move: the selection if it contains the dragged
     * row, else the dragged row alone.                                    */
    gint64 dragged = g_value_get_int64(value);
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tree);
    GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
    gboolean in_sel = FALSE;
    for (GList *l = rows; l; l = l->next) {
        GtkTreeIter it;
        gint64 id;
        gtk_tree_model_get_iter(m, &it, l->data);
        gtk_tree_model_get(m, &it, COL_ID, &id, -1);
        g_array_append_val(ids, id);
        if (id == dragged)
            in_sel = TRUE;
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    if (!in_sel) {
        g_array_set_size(ids, 0);
        g_array_append_val(ids, dragged);
    }
    gtk_tree_path_free(path);

    /* The target must not be one of the moved rows.                      */
    gint64 dst_id;
    gtk_tree_model_get(m, &dst, COL_ID, &dst_id, -1);
    for (guint i = 0; i < ids->len; i++) {
        if (g_array_index(ids, gint64, i) == dst_id) {
            g_print(" — target is among the dragged rows, refused\n");
            g_array_free(ids, TRUE);
            return FALSE;
        }
    }

    /* Moving invalidates the dst iter only if dst's parent changes, which
     * a leaf move never does; re-find it by id after each move anyway.   */
    guint moved = 0;
    for (guint i = 0; i < ids->len; i++) {
        if (tree_find_id(dst_id, &dst) &&
            move_row(g_array_index(ids, gint64, i), &dst, pos))
            moved++;
    }
    g_print(" — moved %u of %u row(s) %s target (Q3: %u selected at DROP)\n",
            moved, ids->len,
            pos == GTK_TREE_VIEW_DROP_BEFORE ? "before" :
            pos == GTK_TREE_VIEW_DROP_AFTER  ? "after"  : "into",
            ids->len);
    g_array_free(ids, TRUE);
    gtk_tree_view_expand_all(tree);
    return TRUE;
}

/*
 * build_tree — the left pane: a GtkTreeStore-backed, multi-select tree
 * view with a drag source and a drop target attached as controllers.
 *
 * Output:
 *   the scrolled window holding the view.
 */
static GtkWidget *
build_tree(void)
{
    store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT64);
    GtkTreeIter work = tree_add(NULL, "Work", 1);
    tree_add(&work, "Projects", 2);
    tree_add(&work, "Meetings", 3);
    GtkTreeIter home = tree_add(NULL, "Home", 4);
    tree_add(&home, "Recipes", 5);
    tree_add(&home, "Garden", 6);
    tree_add(NULL, "Archive", 7);

    tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)));
    gtk_tree_view_append_column(tree,
        gtk_tree_view_column_new_with_attributes("Folder",
            gtk_cell_renderer_text_new(), "text", COL_NAME, NULL));
    gtk_tree_view_set_enable_search(tree, FALSE);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree),
                                GTK_SELECTION_MULTIPLE);
    gtk_tree_view_expand_all(tree);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed",  G_CALLBACK(on_press_capture),   NULL);
    g_signal_connect(click, "released", G_CALLBACK(on_release_capture), NULL);
    g_signal_connect(click, "cancel",   G_CALLBACK(on_cancel_capture),  NULL);
    gtk_widget_add_controller(GTK_WIDGET(tree), GTK_EVENT_CONTROLLER(click));

    GtkDragSource *src = gtk_drag_source_new();
    gtk_drag_source_set_actions(src, GDK_ACTION_MOVE);
    g_signal_connect(src, "prepare", G_CALLBACK(on_drag_prepare), NULL);
    gtk_widget_add_controller(GTK_WIDGET(tree), GTK_EVENT_CONTROLLER(src));

    /* GTK 4.22 bug, measured in this spike: gtk_tree_view_set_drag_dest_row()
     * segfaults on the next paint (NULL TreeViewDragInfo->cssnode in the
     * drop-indicator branch of gtk_tree_view_bin_snapshot) unless
     * gtk_tree_view_enable_model_drag_dest() has run, because only that
     * creates the "dndtarget" CSS node.  Enable it with an EMPTY format set:
     * its built-in GtkDropTargetAsync then matches nothing and never fires,
     * while the node exists for the indicator our own target asks for.     */
    GdkContentFormats *none = gdk_content_formats_new(NULL, 0);
    gtk_tree_view_enable_model_drag_dest(tree, none, 0);
    gdk_content_formats_unref(none);

    GtkDropTarget *dst = gtk_drop_target_new(G_TYPE_INT64, GDK_ACTION_MOVE);
    g_signal_connect(dst, "motion", G_CALLBACK(on_drop_motion), NULL);
    g_signal_connect(dst, "leave",  G_CALLBACK(on_drop_leave),  NULL);
    g_signal_connect(dst, "drop",   G_CALLBACK(on_drop),        NULL);
    gtk_widget_add_controller(GTK_WIDGET(tree), GTK_EVENT_CONTROLLER(dst));

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), GTK_WIDGET(tree));
    gtk_widget_set_size_request(sw, 200, -1);
    return sw;
}

/*
 * overlay_report — Q2 measurement.  Compare where GTK actually put the
 * overlay label against where it should be: buffer y minus the scroll
 * offset (plus whatever the view does with its top margin).
 *
 * Inputs:
 *   adj — the vertical adjustment that just changed
 */
static void
overlay_report(GtkAdjustment *adj, gpointer data)
{
    (void)data;
    graphene_rect_t r;
    if (!gtk_widget_compute_bounds(overlay_label, GTK_WIDGET(view), &r))
        return;
    gdouble scroll = gtk_adjustment_get_value(adj);
    gdouble expect = overlay_buffer_y - scroll;
    g_print("Q2  scroll=%.0f  overlay at view-y %.0f  buffer-y-minus-scroll %.0f"
            "  delta %.0f (top margin is %d)\n",
            scroll, (gdouble)r.origin.y, expect, r.origin.y - expect,
            VIEW_TOP_MARGIN);
}

/*
 * place_overlay — idle: once the layout is valid, anchor the overlay label
 * to OVERLAY_LINE's yrange in buffer coordinates and take a first reading.
 *
 * Output:
 *   G_SOURCE_REMOVE.
 */
static gboolean
place_overlay(gpointer data)
{
    GtkAdjustment *adj = data;
    GtkTextIter it;
    gint y, h;

    gtk_text_buffer_get_iter_at_line(gtk_text_view_get_buffer(view), &it,
                                     OVERLAY_LINE);
    gtk_text_view_get_line_yrange(view, &it, &y, &h);
    overlay_buffer_y = y;
    gtk_text_view_add_overlay(view, overlay_label, 300, y);
    g_print("Q2  overlay placed at buffer (300,%d) = line %d's top; "
            "scroll the right pane and watch delta stay constant\n",
            y, OVERLAY_LINE);
    overlay_report(adj, NULL);
    return G_SOURCE_REMOVE;
}

/*
 * make_image — a 160x100 solid-colour texture, so the spike needs no file.
 *
 * Output:
 *   a GtkImage widget showing it.
 */
static GtkWidget *
make_image(void)
{
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 160, 100);
    gdk_pixbuf_fill(pb, 0x3b82f6ff);
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    GtkWidget *img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
    gtk_image_set_pixel_size(GTK_IMAGE(img), 160);
    gtk_widget_set_size_request(img, 160, 100);
    g_object_unref(tex);
    g_object_unref(pb);
    return img;
}

/*
 * build_text — the right pane: a text view with a top margin, an anchored
 * image at line 3, sixty lines of filler, and the overlay label.
 *
 * Output:
 *   the scrolled window holding the view.
 */
static GtkWidget *
build_text(void)
{
    view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_top_margin(view, VIEW_TOP_MARGIN);
    gtk_text_view_set_left_margin(view, 12);
    gtk_text_view_set_wrap_mode(view, GTK_WRAP_WORD);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
    GString *s = g_string_new(NULL);
    for (gint i = 0; i < 60; i++)
        g_string_append_printf(s, "Line %d of the spike text view.\n", i);
    gtk_text_buffer_set_text(buf, s->str, -1);
    g_string_free(s, TRUE);

    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(buf, &it, 3);
    GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buf, &it);
    gtk_text_view_add_child_at_anchor(view, make_image(), anchor);

    overlay_label = gtk_label_new("copy");
    gtk_widget_add_css_class(overlay_label, "spike-overlay");

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), GTK_WIDGET(view));
    gtk_widget_set_hexpand(sw, TRUE);

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    g_signal_connect(adj, "value-changed", G_CALLBACK(overlay_report), NULL);
    g_idle_add(place_overlay, adj);
    return sw;
}

/*
 * on_activate — build the window.
 */
static void
on_activate(GtkApplication *app, gpointer data)
{
    (void)data;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".spike-overlay { background: #fde68a; color: #000; padding: 2px 6px; "
        "border-radius: 4px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Notes - GTK4 spike");
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 500);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), build_tree());
    gtk_paned_set_end_child(GTK_PANED(paned), build_text());
    gtk_window_set_child(GTK_WINDOW(win), paned);

    g_print("GTK %d.%d.%d, renderer via GSK_RENDERER=%s\n",
            gtk_get_major_version(), gtk_get_minor_version(),
            gtk_get_micro_version(),
            g_getenv("GSK_RENDERER") ? g_getenv("GSK_RENDERER") : "(default)");
    g_print("Q3  select TWO rows (Cmd/Ctrl-click), then press-and-drag one "
            "of them; expect \"2 row(s) selected\"\n");
    gtk_window_present(GTK_WINDOW(win));
}

/*
 * main — run the spike.
 *
 * Output:
 *   the GtkApplication exit status.
 */
int
main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);    /* log survives a crash when piped */
    GtkApplication *app = gtk_application_new("io.camb.notes.gtk4spike",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
