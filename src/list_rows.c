/* ===========================================================================
 * list_rows.c — the item objects behind the GTK4 list widgets
 *
 * See list_rows.h.  Three GObject classes with public fields and a
 * finalizer each; on_row_touch is the one in-place update path.
 * =========================================================================== */

#include "list_rows.h"

/* ---------------------------------------------------------------------------
 * OnSbRow
 * ------------------------------------------------------------------------- */
G_DEFINE_TYPE(OnSbRow, on_sb_row, G_TYPE_OBJECT)

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
G_DEFINE_TYPE(OnNoteRow, on_note_row, G_TYPE_OBJECT)

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
G_DEFINE_TYPE(OnActionRow, on_action_row, G_TYPE_OBJECT)

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
    guint pos;                       /* the row's position in the store     */
    if (!g_list_store_find(store, row, &pos))
        return FALSE;
    g_list_model_items_changed(G_LIST_MODEL(store), pos, 1, 1);
    return TRUE;
}
