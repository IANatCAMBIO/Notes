/* ===========================================================================
 * list_rows.c — the item objects behind the GTK4 list widgets
 *
 * See list_rows.h.  Three GObject classes with public fields and a
 * finalizer each; on_row_touch is the one in-place update path.
 * =========================================================================== */

#include "list_rows.h"

/* ---------------------------------------------------------------------------
 * OnRow — the base class: the "changed" signal.
 * ------------------------------------------------------------------------- */
G_DEFINE_ABSTRACT_TYPE(OnRow, on_row, G_TYPE_OBJECT)

static guint row_changed_signal;     /* OnRow::changed                      */

static void
on_row_class_init(OnRowClass *klass)
{
    row_changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                      NULL, G_TYPE_NONE, 0);
}

static void
on_row_init(OnRow *r)
{
    (void)r;
}

/* ---------------------------------------------------------------------------
 * OnSbRow
 * ------------------------------------------------------------------------- */
G_DEFINE_TYPE(OnSbRow, on_sb_row, ON_TYPE_ROW)

static void
on_sb_row_finalize(GObject *object)
{
    OnSbRow *r = ON_SB_ROW(object);
    g_free(r->name);
    g_free(r->raw);
    g_clear_object(&r->children);
    G_OBJECT_CLASS(on_sb_row_parent_class)->finalize(object);
}

static void
on_sb_row_class_init(OnSbRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = on_sb_row_finalize;
}

static void
on_sb_row_init(OnSbRow *r)
{
    (void)r;
}

OnSbRow *
on_sb_row_new(gint kind, gint64 id, const gchar *name, const gchar *raw,
              gboolean expandable)
{
    OnSbRow *r = g_object_new(ON_TYPE_SB_ROW, NULL);
    r->kind = kind;
    r->id   = id;
    r->name = g_strdup(name);
    r->raw  = g_strdup(raw);
    if (expandable)
        r->children = g_list_store_new(ON_TYPE_SB_ROW);
    return r;
}

/* ---------------------------------------------------------------------------
 * OnNoteRow
 * ------------------------------------------------------------------------- */
G_DEFINE_TYPE(OnNoteRow, on_note_row, ON_TYPE_ROW)

static void
on_note_row_finalize(GObject *object)
{
    OnNoteRow *r = ON_NOTE_ROW(object);
    g_free(r->title);
    g_free(r->modified);
    g_free(r->path);
    g_free(r->created);
    g_free(r->preview);
    g_clear_object(&r->thumb);
    G_OBJECT_CLASS(on_note_row_parent_class)->finalize(object);
}

static void
on_note_row_class_init(OnNoteRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = on_note_row_finalize;
}

static void
on_note_row_init(OnNoteRow *r)
{
    (void)r;
}

OnNoteRow *
on_note_row_new(void)
{
    return g_object_new(ON_TYPE_NOTE_ROW, NULL);
}

/* ---------------------------------------------------------------------------
 * OnActionRow
 * ------------------------------------------------------------------------- */
G_DEFINE_TYPE(OnActionRow, on_action_row, ON_TYPE_ROW)

static void
on_action_row_finalize(GObject *object)
{
    OnActionRow *r = ON_ACTION_ROW(object);
    g_free(r->text);
    g_free(r->due);
    G_OBJECT_CLASS(on_action_row_parent_class)->finalize(object);
}

static void
on_action_row_class_init(OnActionRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = on_action_row_finalize;
}

static void
on_action_row_init(OnActionRow *r)
{
    (void)r;
}

OnActionRow *
on_action_row_new(void)
{
    return g_object_new(ON_TYPE_ACTION_ROW, NULL);
}

/* ---------------------------------------------------------------------------
 * on_row_touch()
 * ------------------------------------------------------------------------- */
gboolean
on_row_touch(GListStore *store, gpointer row)
{
    g_signal_emit(row, row_changed_signal, 0);
    guint pos;                       /* the row's position in the store     */
    if (!g_list_store_find(store, row, &pos))
        return FALSE;
    g_list_model_items_changed(G_LIST_MODEL(store), pos, 1, 1);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * on_row_factory_new() and its trampolines.  The factory carries the
 * caller's bind and data; each list item carries the handler id of its
 * "changed" connection while bound.
 * ------------------------------------------------------------------------- */
typedef struct {
    GCallback bind;                  /* the caller's bind handler           */
    gpointer  user_data;
} RowFactory;

static void row_factory_bind_again(GtkListItem *item);

/* row_factory_bind() — the caller's bind, then listen for "changed".       */
static void
row_factory_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    RowFactory *rf = data;
    ((void (*)(GtkListItemFactory *, GtkListItem *, gpointer))rf->bind)(
        f, item, rf->user_data);
    gpointer row = gtk_list_item_get_item(item);
    if (ON_IS_ROW(row)) {
        gulong id = g_signal_connect_swapped(row, "changed",
                                             G_CALLBACK(row_factory_bind_again),
                                             item);
        g_object_set_data(G_OBJECT(item), "on-changed-id",
                          GSIZE_TO_POINTER(id));
        g_object_set_data(G_OBJECT(item), "on-factory", f);
    }
}

/* row_factory_bind_again() — the bound row changed: bind again.            */
static void
row_factory_bind_again(GtkListItem *item)
{
    GtkListItemFactory *f = g_object_get_data(G_OBJECT(item), "on-factory");
    RowFactory *rf = g_object_get_data(G_OBJECT(f), "on-row-factory");
    ((void (*)(GtkListItemFactory *, GtkListItem *, gpointer))rf->bind)(
        f, item, rf->user_data);
}

/* row_factory_unbind() — stop listening; the item is about to change.    */
static void
row_factory_unbind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(item),
                                                    "on-changed-id"));
    if (id != 0) {
        g_signal_handler_disconnect(gtk_list_item_get_item(item), id);
        g_object_set_data(G_OBJECT(item), "on-changed-id", NULL);
    }
}

GtkListItemFactory *
on_row_factory_new(GCallback setup, GCallback bind, gpointer user_data)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    RowFactory *rf = g_new0(RowFactory, 1);
    rf->bind      = bind;
    rf->user_data = user_data;
    g_object_set_data_full(G_OBJECT(f), "on-row-factory", rf, g_free);
    g_signal_connect(f, "setup",  setup, user_data);
    g_signal_connect(f, "bind",   G_CALLBACK(row_factory_bind),   rf);
    g_signal_connect(f, "unbind", G_CALLBACK(row_factory_unbind), rf);
    return f;
}
