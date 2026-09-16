/* ===========================================================================
 * editor_window.c — WYSIWYG note editor window (implementation)
 *
 * See editor_window.h for the feature overview.  This file is the WINDOW
 * that hosts one OnNoteView (note_view.[ch], the rich-text engine that
 * owns everything inside the text): the chrome and the note's lifecycle.
 *
 *   toolbar + actions  — every command is a "win." GAction on the window;
 *                        the toolbar, the compact Styles/Lists/Insert
 *                        menus and the keyboard shortcuts only NAME them,
 *                        and each one calls the view's API.
 *
 *   the editing gate   — the editing actions are disabled while a widget
 *                        with keys of its own has the focus (D19).
 *
 *   autosave           — the view's "edited" signal re-arms a short timer;
 *                        when it fires the buffer is serialized to BNBF
 *                        and written to SQLite, and the note's tag set and
 *                        action items are synced from what the view says
 *                        may have changed.
 *
 *   the modal viewer   — a click on an embedded image (the view's
 *                        "image-activated") opens the shared image viewer
 *                        over the text; its ops read the view's image API.
 *
 *   status bar         — the note's location, its id, the save-state dot.
 * =========================================================================== */

#include "editor_window.h"
#include "image_viewer.h"
#include "note_view.h"
#include "serialize.h"

/* Milliseconds of idle time after the last edit before autosaving.         */
#define AUTOSAVE_DELAY_MS 1200

/* Default size of a new editor window (client area, excluding the
 * titlebar), used when the ini carries no editor_win_w/editor_win_h.       */
#define EDITOR_WIN_DEFAULT_W 640
#define EDITOR_WIN_DEFAULT_H 509

/* What an editor opened "at" something scrolls to once it is allocated
 * (editor_window_open_full's reveal): nothing, an image, an action item. */
typedef enum { ON_REVEAL_NONE, ON_REVEAL_IMAGE, ON_REVEAL_ACTION } OnReveal;

/* ---------------------------------------------------------------------------
 * OnEditor — all state for one open editor window.  Everything about the
 * TEXT — styles, anchors, tags, undo, the derived looks — is the view's
 * (see OnNoteView in note_view.c); this is the window around it.
 *
 * Fields:
 *   app             — global application context (not owned).
 *   note_id         — id of the note being edited.
 *   window          — the top-level GtkWindow.
 *   view            — the OnNoteView doing the editing; we hold our own
 *                     reference so the final save on window destroy can
 *                     still read it (GTK4 tears the child tree down
 *                     BEFORE "destroy", D21).
 *   autosave_source — GLib timeout id of the pending autosave, 0 if none.
 *   toggle_buttons  — the four inline-style GtkToggleButtons, indexed
 *                     in the same order as INLINE_TOGGLES[], used to
 *                     mirror the view's inline flags into the toolbar UI.
 *   toolbar         — the formatting toolbar.
 *   toolbar_box     — the vbox it sits in (for the live rebuild).
 *   overlay         — stacks img_viewer over the text.
 *   img_viewer      — the modal panel a click on an embedded image opens —
 *                     the same one the media browser uses.
 *   search_entry    — the in-note find entry at the toolbar's right edge.
 *   pending_search  — initial in-note query, applied once.
 *   initial_search_idle — idle source id of that deferred application.
 *   pending_reveal  — what to scroll to once the fresh window is
 *                     allocated (an image, from the media window's
 *                     double-click; an action item, from the library's
 *                     Action Items), with its ordinal in pending_ord.
 *   initial_reveal_idle — idle source id of that deferred reveal.
 *   dirty           — TRUE while the note has edits the database hasn't
 *                     seen (set whenever an autosave is queued, cleared
 *                     by editor_save); closing a window with no unsaved
 *                     edits skips the final save entirely.
 *   status_path     — status-bar label (bottom left): the note's folder
 *                     path, same format as the library window's.  Set at
 *                     open and refreshed when the window becomes active
 *                     again, so a move made in the library shows up on
 *                     return.
 *   status_note_id  — status-bar label (bottom right): "id:N", shown
 *                     only while the statusbar_note_id setting is on
 *                     (Settings applies it live through
 *                     on_editor_status_refresh_all).
 *   status_dirty    — status-bar label at the FAR right, past every other
 *                     status message: the save-state dot, red while
 *                     `dirty` is set and green once editor_save has run.
 *                     Always visible; driven from the two places that own
 *                     `dirty`.
 *   last_actions    — the action-item set last synced to the action_items
 *                     table (OnActionItem list) — editor_save rewrites the
 *                     table only when the freshly extracted set differs,
 *                     so ordinary saves do no action work (the view's
 *                     take_actions_modified says whether an edit could
 *                     even have changed the set; this is what it is
 *                     compared against).
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp          *app;
    gint64          note_id;
    GtkWidget      *window;
    OnNoteView     *view;

    guint           autosave_source;

    GtkWidget      *toggle_buttons[4];
    GtkWidget      *toolbar;
    GtkWidget      *toolbar_box;
    GtkWidget      *overlay;
    OnImageViewer  *img_viewer;

    GtkWidget      *search_entry;
    gchar          *pending_search;
    guint           initial_search_idle;
    OnReveal        pending_reveal;
    gint            pending_ord;
    guint           initial_reveal_idle;
    gboolean        dirty;
    GtkWidget      *status_path;
    GtkWidget      *status_note_id;
    GtkWidget      *status_dirty;
    GList          *last_actions;
} OnEditor;

/* ---------------------------------------------------------------------------
 * INLINE_TOGGLES — table describing the four inline-style toggle buttons:
 * their flag bit, the name "win.inline" takes as its target for the
 * shortcut (see on_app_install_accels), icon file, fallback markup, label
 * and tooltip.
 * ------------------------------------------------------------------------- */
static const struct {
    OnFormatFlags flag;              /* bit this button controls            */
    const gchar  *target;           /* "win.inline" target naming it       */
    const gchar  *icon;             /* local icon file basename            */
    const gchar  *markup;           /* icon fallback markup                */
    const gchar  *label;            /* button text label                   */
    const gchar  *tooltip;          /* hover help text                     */
} INLINE_TOGGLES[4] = {
    { ON_FMT_BOLD,      "bold",      NULL, "<b>B</b>", "Bold",
      "Bold" },
    { ON_FMT_ITALIC,    "italic",    NULL, "<i>I</i>", "Italic",
      "Italic" },
    { ON_FMT_UNDERLINE, "underline", NULL, "<u>U</u>", "Underline",
      "Underline" },
    { ON_FMT_STRIKE,    "strike",    NULL, "<s>S</s>", "Strike",
      "Strikethrough" },
};

/* Forward declarations for callbacks referenced before their definition.   */
static void     editor_gate_widget(OnEditor *ed, GtkWidget *widget);
static void     editor_save(OnEditor *ed);
static void     editor_queue_autosave(OnEditor *ed);
static void     editor_status_dirty_update(OnEditor *ed);

/* ===========================================================================
 * inline and paragraph formatting — the "win." actions over the view API
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * update_toggle_buttons() — mirror the view's inline flags into the four
 * toolbar toggle buttons without re-triggering their "toggled" handlers
 * (each handler is blocked by function+data while the state is pushed).
 * Connected to the view's "inline-flags-changed".
 * ------------------------------------------------------------------------- */
static void on_inline_toggle(GtkToggleButton *btn, gpointer user_data);

static void
update_toggle_buttons(OnEditor *ed)
{
    guint32 flags = on_note_view_inline_flags(ed->view);
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkToggleButton *btn = GTK_TOGGLE_BUTTON(ed->toggle_buttons[i]);
        g_signal_handlers_block_by_func(btn, on_inline_toggle, ed);
        gtk_toggle_button_set_active(
            btn, (flags & INLINE_TOGGLES[i].flag) != 0);
        g_signal_handlers_unblock_by_func(btn, on_inline_toggle, ed);
    }
}

/* on_view_flags_changed() — "inline-flags-changed" from the view.          */
static void
on_view_flags_changed(OnNoteView *view, gpointer user_data)
{
    (void)view;
    update_toggle_buttons(user_data);
}

/* on_inline() — "win.inline(s)": the Primary+B/I/U shortcuts, by the name
 * INLINE_TOGGLES gives the style.  The toolbar's four toggle buttons stay
 * plain toggles driven by update_toggle_buttons(): bound to an action they
 * would flip themselves before the action ran.                             */
static void
on_inline(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    OnEditor *ed = user_data;        /* owning editor                       */
    const gchar *name = g_variant_get_string(param, NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        if (g_strcmp0(INLINE_TOGGLES[i].target, name) == 0) {
            on_note_view_toggle_inline(ed->view, INLINE_TOGGLES[i].flag);
            return;
        }
    }
}

/* on_inline_toggle() — "toggled" handler for the four style buttons.  The
 * flag each button controls is stashed on it as object data "on-flag".     */
static void
on_inline_toggle(GtkToggleButton *btn, gpointer user_data)
{
    OnEditor *ed  = user_data;       /* owning editor                       */
    guint32  flag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn),
                                                       "on-flag"));
    on_note_view_toggle_inline(ed->view, (OnFormatFlags)flag);
}

/* The paragraph styles by the name "win.para" takes as its target: the
 * tool buttons, the compact Styles/Lists menus and the Primary+M shortcut
 * ("win.para::code") all name one of these.                                */
static const struct {
    const gchar *name;               /* the action target                   */
    guint32      flag;               /* the ON_FMT_* paragraph style        */
} PARA_STYLES[] = {
    { "h1",     ON_FMT_H1          },
    { "h2",     ON_FMT_H2          },
    { "body",   0                  },
    { "bullet", ON_FMT_LIST_BULLET },
    { "number", ON_FMT_LIST_NUMBER },
    { "check",  ON_FMT_LIST_CHECK  },
    { "code",   ON_FMT_CODEBLOCK   },
};

/* on_para() — "win.para(s)": toggle the named paragraph style.             */
static void
on_para(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    OnEditor *ed = user_data;        /* owning editor                       */
    const gchar *name = g_variant_get_string(param, NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(PARA_STYLES); i++) {
        if (g_strcmp0(PARA_STYLES[i].name, name) == 0) {
            on_note_view_toggle_paragraph(ed->view, PARA_STYLES[i].flag);
            return;
        }
    }
}

/* ===========================================================================
 * the open-editors table and the settings fan-outs
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editors_foreach() — run `fn` on every open editor.  The one walk of the
 * open-editors table; all the public *_all entry points go through it.
 * ------------------------------------------------------------------------- */
static void
editors_foreach(OnApp *app, void (*fn)(OnEditor *ed))
{
    GHashTableIter iter;             /* walk of the open-editors table      */
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->editors);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        OnEditor *ed =               /* editor state stashed on its window  */
            g_object_get_data(G_OBJECT(value), "on-editor");
        if (ed != NULL)
            fn(ed);
    }
}

/* editor_settings_changed() — the view re-reads every engine setting at
 * once (copy buttons, line numbers, first-line title); each of the three
 * public entry points below is a settings change, so all three land here. */
static void
editor_settings_changed(OnEditor *ed)
{
    on_note_view_settings_changed(ed->view);
}

void
on_editor_apply_line_numbers_all(OnApp *app)
{
    editors_foreach(app, editor_settings_changed);
}

void
on_editor_rebuild_code_buttons_all(OnApp *app)
{
    editors_foreach(app, editor_settings_changed);
}

void
on_editor_title_refresh_all(OnApp *app)
{
    editors_foreach(app, editor_settings_changed);
}

/* ===========================================================================
 * the modal image viewer — this window's side of image_viewer.[ch]
 *
 * The identical panel the media browser opens, over the text view instead of
 * over a thumbnail grid.  Pictures are addressed by ORDINAL (the view's
 * image API — note_view.h), looked up in the buffer on every call: the
 * panel holds no pixbuf of its own, so an edit underneath it can only make
 * an op return NULL — which closes the panel — never dangle.
 * =========================================================================== */

/* editor_viewer_count() — how many pictures the panel may walk.             */
static gint
editor_viewer_count(gpointer host)
{
    return on_note_view_image_count(((OnEditor *)host)->view);
}

/* ---------------------------------------------------------------------------
 * editor_viewer_render() — the panel's image: the full-resolution texture
 * of the anchor's pixbuf (the panel scales DOWN to fit, so the box size is
 * not needed), or NULL when the image is gone — which closes the panel.
 * ------------------------------------------------------------------------- */
static GdkPaintable *
editor_viewer_render(gpointer host, gint idx, gint box_w, gint box_h)
{
    (void)box_w; (void)box_h;
    GdkTexture *tex = on_note_view_image_texture(((OnEditor *)host)->view, idx);
    return tex != NULL ? GDK_PAINTABLE(g_object_ref(tex)) : NULL;
}

/* ---------------------------------------------------------------------------
 * editor_viewer_caption() — "<note> — image N of M — W x H".
 *
 * The note is NAMED even though its own titlebar is right there: "of M"
 * counts every image in the note, including ones scrolled out of sight, so
 * the panel can legitimately walk to a picture the user cannot currently see
 * — and without the note's name that looks indistinguishable from the panel
 * having wandered into another note.  It matches the media browser's caption,
 * which names the note for the same reason.  The pixel size is the one thing
 * the editor knows that the fitted picture no longer shows.
 * ------------------------------------------------------------------------- */
static gchar *
editor_viewer_caption(gpointer host, gint idx)
{
    OnEditor   *ed  = host;          /* owning editor                       */
    GdkTexture *tex = on_note_view_image_texture(ed->view, idx);
    if (tex == NULL)
        return NULL;

    gchar *note = on_note_view_first_line(ed->view);
    gchar *cap  = g_strdup_printf(
        "%s \xe2\x80\x94 image %d of %d \xe2\x80\x94 %d \xc3\x97 %d",
        (note != NULL && *note != '\0') ? note : "Untitled",
        idx + 1, on_note_view_image_count(ed->view),
        gdk_texture_get_width(tex), gdk_texture_get_height(tex));
    g_free(note);
    return cap;
}

/* editor_viewer_action() — "Open in image viewer": hand the picture to the
 * external viewer, exactly as the context menu's Open does.  "Show in source
 * note", the media browser's action, would be a no-op here — the source note
 * is the window the panel is sitting on.                                    */
static void
editor_viewer_action(gpointer host, gint idx)
{
    on_note_image_open_external(
        on_note_view_image_png(((OnEditor *)host)->view, idx));
}

static const OnImageViewerOps editor_viewer_ops = {
    .count   = editor_viewer_count,
    .render  = editor_viewer_render,
    .caption = editor_viewer_caption,
    .action  = editor_viewer_action,
};

/* on_view_image_activated() — "image-activated" from the view: show that
 * image big in the shared modal viewer.                                     */
static void
on_view_image_activated(OnNoteView *view, gint ord, gpointer user_data)
{
    (void)view;
    OnEditor *ed = user_data;        /* owning editor                       */
    on_image_viewer_open(ed->img_viewer, ord);
}

/* ===========================================================================
 * asynchronous completions
 *
 * The Insert Image file chooser finishes on a later main-loop iteration, by
 * which time the editor may have been closed.  It holds no pointer to it:
 * it carries the note id and looks the editor up again in the open-editors
 * table when it completes — an editor that is gone simply drops the image.
 * (The clipboard paste is the view's, with a weak reference of its own.)
 * =========================================================================== */

/* editor_lookup() — the open editor for `note_id`, or NULL.  NULL-safe on
 * a headless OnApp (the CLI has no editors table at all).                   */
static OnEditor *
editor_lookup(OnApp *app, gint64 note_id)
{
    GtkWidget *win = app->editors != NULL
        ? g_hash_table_lookup(app->editors, &note_id) : NULL;
    return win != NULL ? g_object_get_data(G_OBJECT(win), "on-editor")
                       : NULL;
}

/* EditorRef — what a completion carries instead of an OnEditor pointer.    */
typedef struct {
    OnApp  *app;
    gint64  note_id;
} EditorRef;

/* editor_ref_new() — a heap reference to `ed` for a completion callback.   */
static EditorRef *
editor_ref_new(OnEditor *ed)
{
    EditorRef *ref = g_new(EditorRef, 1);
    ref->app     = ed->app;
    ref->note_id = ed->note_id;
    return ref;
}

/* editor_ref_take() — resolve and free a reference: the editor, or NULL
 * when it has been closed since.                                            */
static OnEditor *
editor_ref_take(EditorRef *ref)
{
    OnEditor *ed = editor_lookup(ref->app, ref->note_id);
    g_free(ref);
    return ed;
}

/* on_insert_image_picked() — the Insert Image chooser closed: embed the
 * chosen file at the cursor (nothing on cancel or a closed editor).         */
static void
on_insert_image_picked(gchar *path, gpointer user_data)
{
    OnEditor *ed = editor_ref_take(user_data);
    if (path == NULL)
        return;
    if (ed != NULL) {
        GError *err = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
        if (pixbuf != NULL) {
            on_note_view_insert_image(ed->view, pixbuf);
            g_object_unref(pixbuf);
        } else {
            g_warning("editor: cannot load image %s: %s",
                      path, err->message);
            g_clear_error(&err);
        }
    }
    g_free(path);
}

/* ---------------------------------------------------------------------------
 * on_insert_image() — "win.insert-image" (Insert menu): pick an image file
 * and embed it at the cursor.  Asynchronous: the rest is
 * on_insert_image_picked.
 * ------------------------------------------------------------------------- */
static void
on_insert_image(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    on_app_pick_path(GTK_WINDOW(ed->window), "Insert Image", ON_PICK_OPEN,
                     "_Insert", "Images", NULL, NULL, on_insert_image_picked,
                     editor_ref_new(ed));
}

/* ===========================================================================
 * action items — the window's side: the action_items mirror and the
 * strike / due / text rewrites, live or headless
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * action_lists_equal() — same items, same order, same done flags, same
 * due dates?  Every OnActionItem field the table mirrors must be
 * compared here, or edits to that field never reach the library.
 * ------------------------------------------------------------------------- */
static gboolean
action_lists_equal(GList *a, GList *b)
{
    for (; a != NULL && b != NULL; a = a->next, b = b->next) {
        OnActionItem *x = a->data, *y = b->data;
        if (x->done != y->done || x->due != y->due ||
            g_strcmp0(x->text, y->text) != 0)
            return FALSE;
    }
    return a == NULL && b == NULL;
}

/* ---------------------------------------------------------------------------
 * action_apply_to_note() — run one line edit (strike, due rewrite or
 * rename) against note `note_id`: on the live view's buffer + autosave
 * when an editor is open (the save's extract-and-compare refreshes
 * action_items — the view's "edited" queues it), else on the note's
 * DOCUMENT with an immediate save + action_items resync.  The two paths
 * are the same edit: on_document_action_* under the extractor's ord
 * numbering, live or headless.
 *   which — the edit.
 *   arg   — its argument: a gboolean* (STRIKE), a gint64* (DUE) or the
 *           new text (TEXT); borrowed for the duration of the call.
 * Returns TRUE when the item was found and updated.
 * ------------------------------------------------------------------------- */
typedef enum { ACTION_STRIKE, ACTION_DUE, ACTION_TEXT } ActionEdit;

static gboolean
action_apply_to_note(OnApp *app, gint64 note_id, ActionEdit which,
                     gint ord, gconstpointer arg, gboolean *synced)
{
    if (synced != NULL)
        *synced = FALSE;

    OnEditor *ed = editor_lookup(app, note_id);   /* NULL headless / closed */
    if (ed != NULL) {
        /* Live buffer: the autosave writes content AND the mirror later.
         * A tag change emits no "changed", so the view reports nothing —
         * the autosave is queued here.                                      */
        switch (which) {
        case ACTION_STRIKE:
            return on_note_view_action_strike(ed->view, ord,
                                              *(const gboolean *)arg);
        case ACTION_DUE:
            return on_note_view_action_due(ed->view, ord,
                                           *(const gint64 *)arg);
        default:
            return on_note_view_action_text(ed->view, ord, arg);
        }
    }

    /* A note with no content is an empty document, where the edit finds
     * no action line and fails on its own — no special case needed.       */
    OnDocument *doc = on_note_document_load(app->db, note_id);
    gboolean ok;
    switch (which) {
    case ACTION_STRIKE:
        ok = on_document_action_strike(doc, ord, *(const gboolean *)arg);
        break;
    case ACTION_DUE:
        ok = on_document_action_due(doc, ord, *(const gint64 *)arg);
        break;
    default:
        ok = on_document_action_text(doc, ord, arg);
        break;
    }
    if (ok) {
        gsize   out_len;             /* re-serialized blob size             */
        guint8 *out = on_document_to_bnbf(doc, &out_len);
        gchar  *title = on_document_title(doc, ON_DEFAULT_NOTE_TITLE,
                                          ON_TITLE_MAX_CHARS);
        gchar  *body = NULL;         /* searchable plain text               */
        GList  *actions = NULL;      /* the rewritten '!' lines             */
        on_note_extract(out, out_len, &body, &actions);
        ok = on_db_note_save(app->db, note_id, title, out, out_len, body) &&
             on_db_note_set_actions(app->db, note_id, actions);
        if (ok && synced != NULL)
            *synced = TRUE;          /* the mirror is already up to date    */
        on_db_action_list_free(actions);
        g_free(body);
        g_free(title);
        g_free(out);
    }
    on_document_free(doc);
    return ok;
}

gboolean
on_editor_action_set_done(OnApp *app, gint64 note_id, gint ord,
                          gboolean done, gboolean *synced)
{
    return action_apply_to_note(app, note_id, ACTION_STRIKE, ord, &done,
                                synced);
}

gboolean
on_editor_action_set_due(OnApp *app, gint64 note_id, gint ord, gint64 due)
{
    return action_apply_to_note(app, note_id, ACTION_DUE, ord, &due, NULL);
}

gboolean
on_editor_action_set_text(OnApp *app, gint64 note_id, gint ord,
                          const gchar *text)
{
    return action_apply_to_note(app, note_id, ACTION_TEXT, ord, text, NULL);
}

/* ===========================================================================
 * the remaining "win." actions: new note, find, undo/redo, the Insert menu
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_new_note() — "win.new-note" (Primary+N): a new note in THIS note's
 * folder, opened in its own editor.  The library (if open) refreshes via
 * the full notify — the folder's count just grew.
 * ------------------------------------------------------------------------- */
static void
on_new_note(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    OnNoteMeta *meta = on_db_note_get(ed->app->db, ed->note_id);
    if (meta == NULL)
        return;
    gint64 id = on_db_note_create(ed->app->db, meta->folder_id);
    on_db_note_meta_free(meta);
    if (id != 0) {
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
        on_editor_window_open(ed->app, id);
    }
}

/* on_find() — "win.find" (Primary+F): jump into the in-note search box.   */
static void
on_find(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(ed->search_entry);
}

/* on_undo() / on_redo() — "win.undo" (Primary+Z) and "win.redo"
 * (Primary+Shift+Z, Primary+Y).                                             */
static void
on_undo(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    on_note_view_undo(((OnEditor *)user_data)->view);
}

static void
on_redo(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    on_note_view_redo(((OnEditor *)user_data)->view);
}

/* on_insert_table() / on_insert_emoji() / on_insert_date() — the Insert
 * menu's "win.insert-table", "win.insert-emoji" (Primary+E) and
 * "win.insert-date" (Primary+D): each is the view's operation by that name
 * (the image one is above — it needs the file chooser first).              */
static void
on_insert_table(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    on_note_view_insert_table(((OnEditor *)user_data)->view);
}

static void
on_insert_emoji(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    on_note_view_insert_emoji(((OnEditor *)user_data)->view);
}

static void
on_insert_date(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    on_note_view_insert_date(((OnEditor *)user_data)->view);
}

/* ===========================================================================
 * in-note search — the entry; the matching and the highlight are the view's
 *
 * The toolbar's right-edge entry highlights every match as you type and
 * selects the first one at or after the caret, scrolled into view; Enter
 * (or the arrow buttons) jumps to the next match, wrapping at the end.
 * Primary+F focuses the entry; Escape returns focus to the text.
 * =========================================================================== */

/* on_search_changed() — the entry's (debounced) "search-changed": the view
 * re-highlights every match of the current query.                          */
static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    on_note_view_find(ed->view, gtk_editable_get_text(GTK_EDITABLE(entry)));
}

/* editor_search_move() — step to the next/previous match of the entry's
 * LIVE text (not the last debounced one — Enter can beat the debounce).    */
static void
editor_search_move(OnEditor *ed, gboolean forward)
{
    on_note_view_find_step(
        ed->view, gtk_editable_get_text(GTK_EDITABLE(ed->search_entry)),
        forward);
}

/* on_search_next() / on_search_prev() — Enter in the entry and the two
 * arrow buttons (first argument ignored, so one handler serves both the
 * entry's "activate" and the buttons' "clicked").                           */
static void
on_search_next(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    editor_search_move((OnEditor *)user_data, TRUE);
}

static void
on_search_prev(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    editor_search_move((OnEditor *)user_data, FALSE);
}

/* on_search_stop() — Escape in the entry: back to the text view.            */
static void
on_search_stop(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * editor_apply_search_term() — put `term` in the in-note search box,
 * highlight every match and jump to the first.  Used when a note is opened
 * from the library search window so the searched-for text is highlighted at
 * once.  on_search_changed is called explicitly rather than waiting for the
 * GtkSearchEntry's debounced "search-changed".  NULL/empty is a no-op.
 * ------------------------------------------------------------------------- */
static void
editor_apply_search_term(OnEditor *ed, const gchar *term)
{
    if (term == NULL || *term == '\0')
        return;
    gtk_editable_set_text(GTK_EDITABLE(ed->search_entry), term);
    on_search_changed(GTK_SEARCH_ENTRY(ed->search_entry), ed);
}

/* on_initial_search_idle() — apply ed->pending_search once the freshly
 * opened window is realized and allocated (so the scroll-to-match lands,
 * mirroring the view's own deferred caret-follow scroll).                    */
static gboolean
on_initial_search_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->initial_search_idle = 0;
    editor_apply_search_term(ed, ed->pending_search);
    g_clear_pointer(&ed->pending_search, g_free);
    return G_SOURCE_REMOVE;
}

/* editor_reveal() — scroll the view to `what` number `ord` (an image or an
 * action item); THE dispatch both the deferred open and the already-open
 * re-open go through.                                                       */
static void
editor_reveal(OnEditor *ed, OnReveal what, gint ord)
{
    if (what == ON_REVEAL_IMAGE)
        on_note_view_image_reveal(ed->view, ord);
    else if (what == ON_REVEAL_ACTION)
        on_note_view_action_reveal(ed->view, ord);
}

/* on_initial_reveal_idle() — run the pending reveal once the freshly opened
 * window is realized and allocated, so the scroll lands (same deferral as
 * on_initial_search_idle).                                                   */
static gboolean
on_initial_reveal_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->initial_reveal_idle = 0;
    editor_reveal(ed, ed->pending_reveal, ed->pending_ord);
    ed->pending_reveal = ON_REVEAL_NONE;
    return G_SOURCE_REMOVE;
}

/* ===========================================================================
 * saving
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_save() — serialize the buffer and persist it: content blob,
 * derived title, and — only when the view's live tags-modified flag says a
 * #tag was created, renamed or deleted — the note's tag set.  Then pokes
 * the library: the light notes-pane refresh normally (editing a note can
 * never change folder counts), the full one (sidebar included) only for
 * tag changes.
 * ------------------------------------------------------------------------- */
static void
editor_save(OnEditor *ed)
{
    gsize   blob_len = 0;            /* BNBF blob size                      */
    guint8 *blob = on_note_view_serialize(ed->view, &blob_len);
    gchar  *title = on_note_view_first_line(ed->view);

    /* ONE walk of the fresh blob for both derived values.  The action list
     * is only wanted when the set might have changed (see below); asking for
     * it here costs nothing extra, since the walk happens either way.  The
     * view's flag is taken (and so cleared) on EVERY save, whatever
     * last_actions holds, exactly as it was reset after every extract.     */
    gboolean edited_actions =        /* could an edit have changed the set? */
        on_note_view_take_actions_modified(ed->view);
    gboolean want_actions = ed->last_actions != NULL || edited_actions;
    gchar  *body = NULL;             /* searchable plain text               */
    GList  *fresh_actions = NULL;    /* '!' lines, when wanted              */
    on_note_extract(blob, blob_len, &body,
                    want_actions ? &fresh_actions : NULL);

    on_db_note_save(ed->app->db, ed->note_id, title, blob, blob_len, body);
    g_free(body);

    /* Rewrite note_tags only when the tag set actually changed — flagged
     * live by the view while editing, so the common save does no tag work
     * at all.                                                              */
    gboolean tags_changed = on_note_view_take_tags_modified(ed->view);
    if (tags_changed) {
        GList *tags = on_note_view_collect_tags(ed->view);
        on_db_note_set_tags(ed->app->db, ed->note_id, tags);
        g_list_free_full(tags, g_free);
    }

    /* Mirror the '!' action lines into action_items only when they changed
     * since the last sync (cheap blob walk, no images decoded).  Skip the
     * walk entirely when the view reports no such edit AND last_actions is
     * NULL — no action line existed before and no insert/delete that could
     * have created one has occurred since the last extract.                 */
    gboolean actions_changed = FALSE;
    if (want_actions) {
        GList *actions = fresh_actions;   /* from the walk above            */
        actions_changed = !action_lists_equal(actions, ed->last_actions);
        if (actions_changed) {
            /* Hint from the marks (identifies reworded items), let the
             * rebuild assign the uids, then re-place the marks on the
             * lines as they now stand.                                     */
            on_note_view_action_marks_hint(ed->view, actions);
            on_db_note_set_actions(ed->app->db, ed->note_id, actions);
            on_note_view_action_marks_sync(ed->view, actions);
            on_db_action_list_free(ed->last_actions);
            ed->last_actions = actions;
        } else {
            on_db_action_list_free(actions);
        }
    }
    ed->dirty = FALSE;
    editor_status_dirty_update(ed);   /* dot turns green                     */

    /* Window title mirrors the note title.                                 */
    if (ed->window != NULL) {
        gchar *wtitle = g_strdup_printf("Notes - %s", title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    g_free(title);
    g_free(blob);

    /* Tag-set and action-set changes touch the sidebar (tag rows, the
     * Action Items section/count): full refresh.  Plain saves take the
     * light notes-pane-only path.                                          */
    if (tags_changed || actions_changed) {
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
    } else {
        if (ed->app->notify_note_saved != NULL)
            ed->app->notify_note_saved(ed->app, ed->note_id);
    }
}

/* on_autosave_timeout() — the debounce timer fired: save now.               */
static gboolean
on_autosave_timeout(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->autosave_source = 0;
    editor_save(ed);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * editor_queue_autosave() — (re)arm the autosave debounce timer.  Every
 * mutation the view makes arrives here through its "edited" signal; the
 * live action edits (action_apply_to_note) queue it directly.
 * ------------------------------------------------------------------------- */
static void
editor_queue_autosave(OnEditor *ed)
{
    if (!ed->dirty) {                /* transition only — this runs per
                                        keystroke, the label update doesn't  */
        ed->dirty = TRUE;            /* there is now something to save      */
        editor_status_dirty_update(ed);              /* dot turns red        */
    }
    if (ed->autosave_source != 0)
        g_source_remove(ed->autosave_source);
    ed->autosave_source = g_timeout_add(AUTOSAVE_DELAY_MS,
                                        on_autosave_timeout, ed);
}

/* ---------------------------------------------------------------------------
 * on_editor_destroy() — the window is going away: flush a final save (or
 * delete the note outright when it was left with no content), drop the
 * editor from the open-editors table, and free everything.
 *
 * In GTK4 a window's "destroy" is emitted from GtkWidget's dispose, AFTER
 * GtkWindow's dispose has unparented the child — so by now the toolbar,
 * the overlay and the status labels are gone, and nothing here may touch
 * them: the label pointers are cleared first so the save's status update
 * sees nothing to paint.  The view (and with it the buffer) survives on
 * the editor's own reference and is released LAST, after the save has
 * read it; its dispose frees everything the engine owns.
 * ------------------------------------------------------------------------- */
static void
on_editor_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnEditor *ed = user_data;        /* owning editor                       */

    ed->status_path    = NULL;       /* the labels died with the tree       */
    ed->status_note_id = NULL;
    ed->status_dirty   = NULL;

    if (ed->autosave_source != 0) {
        g_source_remove(ed->autosave_source);
        ed->autosave_source = 0;
    }
    if (ed->initial_search_idle != 0) {
        g_source_remove(ed->initial_search_idle);
        ed->initial_search_idle = 0;
    }
    if (ed->initial_reveal_idle != 0) {
        g_source_remove(ed->initial_reveal_idle);
        ed->initial_reveal_idle = 0;
    }
    on_image_viewer_free(ed->img_viewer);
    ed->img_viewer = NULL;
    g_clear_pointer(&ed->pending_search, g_free);

    ed->window = NULL;               /* don't touch the dying window        */
    if (on_note_view_is_blank(ed->view)) {
        /* A note closed with no content is discarded — permanently, an
         * empty note in the Trash would be clutter — so a Ctrl+N or
         * quicknote window closed without typing leaves nothing behind.
         * Deletion can change folder counts: full library notify.         */
        on_db_notes_delete(ed->app->db, &ed->note_id, 1);
        on_app_status(ed->app, "Deleted empty note");
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
    } else if (ed->dirty) {
        editor_save(ed);             /* flush edits the autosave missed
                                        (view kept alive by our ref);
                                        a clean note closes instantly —
                                        no serialize, no PNG encoding      */
    }

    g_hash_table_remove(ed->app->editors, &ed->note_id);
    on_db_action_list_free(ed->last_actions);
    g_object_unref(ed->view);        /* the engine's own teardown           */
    ed->view = NULL;
    g_free(ed);
}

/* ===========================================================================
 * status bar
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_status_path_update() — put the note's folder path in the status
 * bar's left label, same format as the library window's: "/" for the top
 * level, "/Folder/Sub" otherwise.
 * ------------------------------------------------------------------------- */
static void
editor_status_path_update(OnEditor *ed)
{
    if (ed->status_path == NULL)
        return;

    OnNoteMeta *meta = on_db_note_get(ed->app->db, ed->note_id);
    if (meta == NULL)
        return;

    gchar *path = on_db_folder_path(ed->app->db, meta->folder_id);
    gchar *text = g_strdup_printf("/%s", path);
    gchar *full = on_app_location_text(ed->app, text);
    gtk_label_set_text(GTK_LABEL(ed->status_path), full);
    g_free(full);
    g_free(text);
    g_free(path);
    on_db_note_meta_free(meta);
}

/* ---------------------------------------------------------------------------
 * editor_status_note_id_update() — show the note's database id in the
 * status bar's right label ("id:N"), or hide the label entirely while
 * the statusbar_note_id setting is off (this function fully owns its
 * visibility; the label is built hidden).
 * ------------------------------------------------------------------------- */
static void
editor_status_note_id_update(OnEditor *ed)
{
    if (ed->status_note_id == NULL)
        return;
    if (ed->app->statusbar_note_id) {
        gchar *text = g_strdup_printf("id:%" G_GINT64_FORMAT, ed->note_id);
        gtk_label_set_text(GTK_LABEL(ed->status_note_id), text);
        g_free(text);
    }
    gtk_widget_set_visible(ed->status_note_id, ed->app->statusbar_note_id);
}

/* ---------------------------------------------------------------------------
 * editor_status_dirty_update() — paint the far-right save-state dot: red
 * while the buffer holds edits the database hasn't seen, green once the
 * autosave (or the close-time flush) has written them.  Called from the two
 * places that own ed->dirty, and at window build for the initial green.
 * ------------------------------------------------------------------------- */
static void
editor_status_dirty_update(OnEditor *ed)
{
    if (ed->status_dirty == NULL)
        return;
    gtk_label_set_text(GTK_LABEL(ed->status_dirty),
                       ed->dirty ? "\xF0\x9F\x94\xB4"   /* 🔴 unsaved edits  */
                                 : "\xF0\x9F\x9F\xA2"); /* 🟢 saved         */
    on_app_set_tooltip(ed->status_dirty,
                                ed->dirty ? "Unsaved changes" : "Saved");
}

/* editor_status_update() — re-render all status-bar labels of one editor.   */
static void
editor_status_update(OnEditor *ed)
{
    editor_status_path_update(ed);
    editor_status_note_id_update(ed);
    editor_status_dirty_update(ed);
}

/* ---------------------------------------------------------------------------
 * on_editor_status_refresh_all() — re-render the status bar of every open
 * editor (Settings toggled the DB-path prefix or the note id live).
 * ------------------------------------------------------------------------- */
void
on_editor_status_refresh_all(OnApp *app)
{
    editors_foreach(app, editor_status_update);
}

/* on_editor_active_changed() — "notify::is-active": the window became the
 * active one again — re-read the note's location, so a move or folder
 * rename made in the library while this editor sat in the background
 * shows up on return.                                                       */
static void
on_editor_active_changed(GObject *window, GParamSpec *pspec,
                         gpointer user_data)
{
    (void)pspec;
    if (gtk_window_is_active(GTK_WINDOW(window)))
        editor_status_path_update(user_data);
}

/* ===========================================================================
 * toolbar + window construction
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * add_para_button() — helper: append a paragraph-style tool button.
 *   ed       — the editor.
 *   toolbar  — the toolbar box to append to.
 *   icon     — local icon file basename, or NULL.
 *   fallback — markup shown as the icon when the file is missing.
 *   label    — button text label.
 *   tooltip  — hover help.
 *   style    — the "win.para" target the button applies ("h1", "body", …).
 * ------------------------------------------------------------------------- */
static void
add_para_button(OnEditor *ed, GtkWidget *toolbar, const gchar *icon,
                const gchar *fallback, const gchar *label,
                const gchar *tooltip, const gchar *style)
{
    GtkWidget *item = on_app_tool_item_new(ed->app, FALSE, icon,
                                           fallback, label, tooltip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(item), "win.para");
    gtk_actionable_set_action_target(GTK_ACTIONABLE(item), "s", style);
    gtk_box_append(GTK_BOX(toolbar), item);
}

/* ---------------------------------------------------------------------------
 * menu_button_new() — helper: a glyph-faced, frameless GtkMenuButton over
 * a menu model.  `markup` is Pango markup rendered as the button face so
 * the compact menu buttons match the letter-glyph tool buttons.  The
 * button never takes the focus: the editing actions are enabled only
 * while the text view has it, and a click here must not disable the very
 * items it is opening.
 *   markup  — the button face.
 *   tooltip — hover help.
 *   model   — the items; OWNERSHIP IS TAKEN.
 * ------------------------------------------------------------------------- */
static GtkWidget *
menu_button_new(const gchar *markup, const gchar *tooltip,
                GMenuModel *model)
{
    GtkWidget *btn  = gtk_menu_button_new();
    GtkWidget *face = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(face), markup);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), face);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(btn), FALSE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn), model);
    g_object_unref(model);
    on_app_set_tooltip(btn, tooltip);
    return btn;
}

/* ---------------------------------------------------------------------------
 * para_menu() — a compact-toolbar menu of paragraph styles: one item per
 * (label, "win.para" target) pair.
 *   labels / styles / n — parallel arrays.
 * Returns a new model (menu_button_new takes it).
 * ------------------------------------------------------------------------- */
static GMenuModel *
para_menu(const gchar *const *labels, const gchar *const *styles, gsize n)
{
    GMenu *menu = g_menu_new();
    for (gsize i = 0; i < n; i++) {
        GMenuItem *item = g_menu_item_new(labels[i], NULL);
        g_menu_item_set_action_and_target(item, "win.para", "s", styles[i]);
        g_menu_append_item(menu, item);
        g_object_unref(item);
    }
    return G_MENU_MODEL(menu);
}

/* search_button_new() — a frameless icon button beside the in-note search
 * entry, wired to one of the two match-stepping handlers.                  */
static GtkWidget *
search_button_new(const gchar *icon_name, const gchar *tooltip,
                  GCallback clicked, OnEditor *ed)
{
    GtkWidget *btn = gtk_button_new_from_icon_name(icon_name);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    on_app_set_tooltip(btn, tooltip);
    g_signal_connect(btn, "clicked", clicked, ed);
    return btn;
}

/* ---------------------------------------------------------------------------
 * build_toolbar() — construct the formatting toolbar: a horizontal GtkBox
 * with the "toolbar" style class holding the inline-style toggles, the
 * paragraph-style buttons, code-block and image insertion.  In compact
 * mode (File → Settings…) the three paragraph-style buttons collapse into
 * an "Aa" Styles menu button and the three list buttons into a "≡" Lists
 * one.  A box demands its full natural width, which becomes the window's
 * minimum — items can never be silently clipped by narrowing the window.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_toolbar(OnEditor *ed)
{
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(toolbar, "toolbar");

    /* Inline style toggles (B, I, U, S).                                   */
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkWidget *item = on_app_tool_item_new(
            ed->app, TRUE, INLINE_TOGGLES[i].icon, INLINE_TOGGLES[i].markup,
            INLINE_TOGGLES[i].label, INLINE_TOGGLES[i].tooltip);
        g_object_set_data(G_OBJECT(item), "on-flag",
                          GUINT_TO_POINTER(INLINE_TOGGLES[i].flag));
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_inline_toggle), ed);
        gtk_box_append(GTK_BOX(toolbar), item);
        ed->toggle_buttons[i] = item;
    }

    add_para_button(ed, toolbar, "code-block", "{\xc2\xa0}", "Code",
                    "Code block", "code");

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* Paragraph styles.  These have no standard icons, so their "icons"
     * are text glyphs (still swappable by dropping a matching PNG — e.g.
     * heading-1.png — into the icons/ folder).                             */
    if (ed->app->compact_editor_toolbar) {
        static const gchar *const LABELS[] =
            { "Heading _1", "Heading _2", "_Body" };
        static const gchar *const STYLES[] = { "h1", "h2", "body" };
        gtk_box_append(GTK_BOX(toolbar),
                       menu_button_new("<b>A</b>a",
                           "Styles \xe2\x80\x94 paragraph style: "
                           "heading or body text",
                           para_menu(LABELS, STYLES, 3)));
    } else {
        add_para_button(ed, toolbar, "heading-1", "<b>H1</b>", "Heading 1",
                        "Heading 1", "h1");
        add_para_button(ed, toolbar, "heading-2", "<b>H2</b>", "Heading 2",
                        "Heading 2", "h2");
        add_para_button(ed, toolbar, "body-text", "\xc2\xb6", "Body",
                        "Plain body text", "body");
    }

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    if (ed->app->compact_editor_toolbar) {
        static const gchar *const LABELS[] =
            { "_Bulleted List", "_Numbered List", "_Task List" };
        static const gchar *const STYLES[] = { "bullet", "number", "check" };
        gtk_box_append(GTK_BOX(toolbar),
                       menu_button_new("\xe2\x89\xa1",
                           "Lists \xe2\x80\x94 bullets, numbers, or "
                           "task checkboxes",
                           para_menu(LABELS, STYLES, 3)));
    } else {
        add_para_button(ed, toolbar, "list-bullet", "\xe2\x80\xa2",
                        "Bullets", "Bulleted list", "bullet");
        add_para_button(ed, toolbar, "list-number", "1.", "Numbered",
                        "Numbered list", "number");
        /* Fallback glyph is a plain text square (U+25A1 □), not the ⬜
         * color emoji: it renders in the text font like the •/1. glyphs
         * and avoids the emoji's oversized advance in the toolbar.        */
        add_para_button(ed, toolbar, "list-check", "\xe2\x96\xa1", "Tasks",
                        "Task list with checkboxes (click a box to toggle)",
                        "check");
    }

    gtk_box_append(GTK_BOX(toolbar),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    /* One "Insert ▾" dropdown replaces the Image/Table/Emoji buttons.  The
     * shortcuts (Primary+E, Primary+D) show on the items by themselves.  */
    GMenu *insert_menu = g_menu_new();
    g_menu_append(insert_menu, "_Image\xe2\x80\xa6", "win.insert-image");
    g_menu_append(insert_menu, "_Table",             "win.insert-table");
    g_menu_append(insert_menu, "_Emoji\xe2\x80\xa6", "win.insert-emoji");
    g_menu_append(insert_menu, "_Date",              "win.insert-date");

    gtk_box_append(GTK_BOX(toolbar),
                   menu_button_new("+",
                       "Insert an image, a table, an emoji, or "
                       "today's date at the cursor",
                       G_MENU_MODEL(insert_menu)));

    /* In-note search, pinned to the toolbar's right edge (Primary+F) by an
     * expanding blank spacer.                                              */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);

    ed->search_entry = gtk_search_entry_new();
    editor_gate_widget(ed, ed->search_entry);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(ed->search_entry),
                                          "Find in note");
    gtk_editable_set_width_chars(GTK_EDITABLE(ed->search_entry), 18);
    g_signal_connect(ed->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), ed);
    g_signal_connect(ed->search_entry, "activate",
                     G_CALLBACK(on_search_next), ed);
    g_signal_connect(ed->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), ed);

    /* Entry + previous/next match buttons as one group.                    */
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(search_box), ed->search_entry);
    gtk_box_append(GTK_BOX(search_box),
                   search_button_new("go-up-symbolic", "Previous match",
                                     G_CALLBACK(on_search_prev), ed));
    gtk_box_append(GTK_BOX(search_box),
                   search_button_new("go-down-symbolic",
                                     "Next match (Enter)",
                                     G_CALLBACK(on_search_next), ed));
    gtk_box_append(GTK_BOX(toolbar), search_box);

    return toolbar;
}

/* editor_rebuild_toolbar() — swap one editor's formatting toolbar for a
 * freshly built one, refreshing the ed-> widget pointers (toggle_buttons,
 * search_entry).                                                           */
static void
editor_rebuild_toolbar(OnEditor *ed)
{
    if (ed->toolbar_box == NULL)
        return;
    gchar *query =                   /* in-note search survives the rebuild */
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ed->search_entry)));
    gtk_box_remove(GTK_BOX(ed->toolbar_box), ed->toolbar);
    ed->toolbar = build_toolbar(ed);
    gtk_box_prepend(GTK_BOX(ed->toolbar_box), ed->toolbar);
    update_toggle_buttons(ed);       /* fresh toggles: mirror inline_flags  */
    if (*query != '\0')              /* re-runs the search + highlights     */
        gtk_editable_set_text(GTK_EDITABLE(ed->search_entry), query);
    g_free(query);
}
void
on_editor_rebuild_toolbars_all(OnApp *app)
{
    editors_foreach(app, editor_rebuild_toolbar);
}

/* ---------------------------------------------------------------------------
 * editor_build_layout() — assemble the toolbar, text-view scroll, and status
 * bar into a vertical box and set it as ed->window's child.
 * ------------------------------------------------------------------------- */
static void
editor_build_layout(OnEditor *ed)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ed->toolbar_box = vbox;
    ed->toolbar     = build_toolbar(ed);
    gtk_box_append(GTK_BOX(vbox), ed->toolbar);
    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll),
                                              FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(ed->view));

    /* The modal image viewer stacks over the text, so the scroll goes in an
     * overlay.  Only the text area is covered: the toolbar and status bar
     * stay live, which is fine — the panel is about looking at a picture,
     * not about locking the window.                                        */
    ed->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(ed->overlay), scroll);
    gtk_widget_set_vexpand(ed->overlay, TRUE);
    gtk_box_append(GTK_BOX(vbox), ed->overlay);

    /* --- status bar: note location (left), note id, save-state dot ------ */
    ed->status_path = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(ed->status_path),
                            PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(ed->status_path, TRUE);
    gtk_widget_add_css_class(ed->status_path, "notes-status-label");

    /* Note id — built hidden: its updater owns visibility
     * (statusbar_note_id setting, default off).                            */
    ed->status_note_id = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_note_id), 1.0);
    gtk_widget_add_css_class(ed->status_note_id, "notes-status-label");
    gtk_widget_set_visible(ed->status_note_id, FALSE);

    /* Save-state dot — the very last thing on the bar, always shown.        */
    ed->status_dirty = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_dirty), 1.0);
    gtk_widget_add_css_class(ed->status_dirty, "notes-dot-label");

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 3);
    gtk_widget_set_margin_bottom(status_bar, 3);
    gtk_box_append(GTK_BOX(status_bar), ed->status_path);
    gtk_box_append(GTK_BOX(status_bar), ed->status_note_id);
    gtk_box_append(GTK_BOX(status_bar), ed->status_dirty);

    gtk_box_append(GTK_BOX(vbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(vbox), status_bar);
    editor_status_update(ed);

    gtk_window_set_child(GTK_WINDOW(ed->window), vbox);

    /* Built last, once the overlay can reach the toplevel: the panel installs
     * its styling for that window's display.                               */
    ed->img_viewer = on_image_viewer_new(
        ed->overlay, &editor_viewer_ops, ed, "Open in image viewer",
        "Open this image at full size in an external viewer");
}

/* ---------------------------------------------------------------------------
 * on_editor_window_key_pressed() — offer every key to the modal image
 * viewer first: Escape closes it, the arrows walk the note's images.  It
 * takes none while it is closed, so editing keeps all of its own keys.
 *
 * A key controller on the WINDOW in the CAPTURE phase: it runs before the
 * key reaches the focus widget, which is what lets the panel claim keys off
 * the text view.  The view KEEPS the focus while the panel is up
 * (deliberately — see image_viewer.c), so every key the panel does not want
 * is swallowed here as well: without that, typing at a picture would edit
 * the note blind behind it.
 * ------------------------------------------------------------------------- */
static gboolean
on_editor_window_key_pressed(GtkEventControllerKey *controller, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer user_data)
{
    (void)controller; (void)keycode;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (on_image_viewer_key_press(ed->img_viewer, keyval, state))
        return TRUE;
    return on_image_viewer_is_open(ed->img_viewer);   /* modal: eat the rest */
}

/* ===========================================================================
 * actions
 *
 * Every command the editor offers is a "win." GAction on its window; the
 * toolbar, the compact Styles/Lists/Insert menus and every keyboard
 * shortcut (on_app_install_accels) only NAME them.  Nothing here is
 * "app.": an editor's commands act on this note, so they are only ever
 * invoked from inside this window.  The image and table context menus are
 * NOT here: they name the view's own "view." group (note_view.c).
 *
 * The EDITING actions are disabled while the in-note search entry has the
 * focus.  A window accelerator fires whatever widget has the focus, and
 * the entry has keys of its own; a disabled action is skipped by the
 * accelerator lookup, so Primary+B there propagates to the entry as a key.
 * =========================================================================== */

/* Action name → handler, parameter type, gated on the view's focus.        */
typedef struct {
    const gchar *name;
    void       (*activate)(GSimpleAction *, GVariant *, gpointer);
    const gchar *param_type;         /* GVariant type string, or NULL       */
    gboolean     editing;            /* enabled only while the view has
                                        the focus                           */
} EditorAction;

static const EditorAction EDITOR_ACTIONS[] = {
    { "new-note",      on_new_note,      NULL, FALSE },
    { "find",          on_find,          NULL, FALSE },
    { "undo",          on_undo,          NULL, TRUE  },
    { "redo",          on_redo,          NULL, TRUE  },
    { "inline",        on_inline,        "s",  TRUE  },
    { "para",          on_para,          "s",  TRUE  },
    { "insert-image",  on_insert_image,  NULL, TRUE  },
    { "insert-table",  on_insert_table,  NULL, TRUE  },
    { "insert-emoji",  on_insert_emoji,  NULL, TRUE  },
    { "insert-date",   on_insert_date,   NULL, TRUE  },
};

/* ---------------------------------------------------------------------------
 * editor_actions_set_editing() — enable or disable the gated actions.
 * Tolerates a window that is gone or whose action map is already empty
 * (the focus leaves the view once more while the window is torn down).
 *   ed      — the editor.
 *   enabled — TRUE while the text view has the focus.
 * ------------------------------------------------------------------------- */
static void
editor_actions_set_editing(OnEditor *ed, gboolean enabled)
{
    if (ed->window == NULL)
        return;
    GActionMap *map = G_ACTION_MAP(ed->window);
    for (gsize i = 0; i < G_N_ELEMENTS(EDITOR_ACTIONS); i++) {
        if (!EDITOR_ACTIONS[i].editing)
            continue;
        GAction *action =            /* NULL once the map is torn down      */
            g_action_map_lookup_action(map, EDITOR_ACTIONS[i].name);
        if (action != NULL)
            g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
}

/* on_gate_enter() / on_gate_leave() — the gate (D11, D19), hung on the
 * widgets that have keys of THEIR OWN: the in-note search entry and every
 * table cell.  While the focus is inside one of them the editing actions
 * are off, so Primary+B there is a plain key again; everywhere else — the
 * view, a toolbar button, an open menu popover (which takes the keyboard
 * focus while it is up: gating on the VIEW's focus greyed out the very
 * Insert/Styles items being opened) — they are on.                          */
static void
on_gate_enter(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    editor_actions_set_editing(user_data, FALSE);
}

static void
on_gate_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    editor_actions_set_editing(user_data, TRUE);
}

/* editor_gate_widget() — close the editing gate while `widget` (or a
 * descendant) has the focus.  The one gate: the search entry, handed to it
 * at toolbar build.                                                        */
static void
editor_gate_widget(OnEditor *ed, GtkWidget *widget)
{
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_gate_enter), ed);
    g_signal_connect(focus, "leave", G_CALLBACK(on_gate_leave), ed);
    gtk_widget_add_controller(widget, focus);
}

/* ---------------------------------------------------------------------------
 * editor_install_actions() — add every action in the table above to the
 * window.  Must run before anything that names them is built (the
 * toolbar), so those items come up sensitive.
 * ------------------------------------------------------------------------- */
static void
editor_install_actions(OnEditor *ed)
{
    GActionMap *map = G_ACTION_MAP(ed->window);
    for (gsize i = 0; i < G_N_ELEMENTS(EDITOR_ACTIONS); i++) {
        const EditorAction *a = &EDITOR_ACTIONS[i];
        GSimpleAction *action = g_simple_action_new(
            a->name,
            a->param_type != NULL ? G_VARIANT_TYPE(a->param_type) : NULL);
        g_signal_connect(action, "activate", G_CALLBACK(a->activate), ed);
        g_action_map_add_action(map, G_ACTION(action));
        g_object_unref(action);      /* the map holds it now                */
    }
}

/* on_view_edited() — "edited" from the view: something to save.             */
static void
on_view_edited(OnNoteView *view, gpointer user_data)
{
    (void)view;
    editor_queue_autosave(user_data);
}

/* ---------------------------------------------------------------------------
 * editor_build_view() — create ed->view and connect its three signals.
 * None can fire during a load (the view reports nothing for it).
 * ------------------------------------------------------------------------- */
static void
editor_build_view(OnEditor *ed)
{
    /* The editor holds its own reference to the view, so the view's
     * dispose runs from on_editor_destroy after the final save, never from
     * the window's child teardown at a moment of GTK's choosing.           */
    ed->view = ON_NOTE_VIEW(g_object_ref_sink(on_note_view_new(ed->app)));
    g_signal_connect(ed->view, "edited",
                     G_CALLBACK(on_view_edited), ed);
    g_signal_connect(ed->view, "inline-flags-changed",
                     G_CALLBACK(on_view_flags_changed), ed);
    g_signal_connect(ed->view, "image-activated",
                     G_CALLBACK(on_view_image_activated), ed);
    /* The view's "view." context-menu actions, made reachable from the
     * whole window: on_app_menu_popup parents the table menu's popover to
     * the window's child box (D14), where the group inserted on the view
     * itself is out of scope.                                              */
    gtk_widget_insert_action_group(ed->window, "view",
                                   on_note_view_action_group(ed->view));
}

/* ---------------------------------------------------------------------------
 * editor_load_content() — load the stored BNBF blob into the view, snapshot
 * the action set the table holds (editor_save only rewrites when it
 * drifts) and seed the view's identity marks from the stored rows.
 * ------------------------------------------------------------------------- */
static void
editor_load_content(OnEditor *ed)
{
    gsize   blob_len = 0;
    guint8 *blob = on_db_note_load(ed->app->db, ed->note_id, &blob_len);
    on_note_view_load(ed->view, blob, blob_len);
    if (blob != NULL) {
        ed->last_actions = on_note_extract_actions(blob, blob_len);
        g_free(blob);

        /* Seed the identity marks from the stored rows, which are in the
         * same ord order as the lines just loaded — that is what lets a
         * reword in this session keep its uid.                             */
        GList *rows = on_db_action_list_for_note(ed->app->db, ed->note_id);
        on_note_view_action_marks_sync(ed->view, rows);
        on_db_action_list_free(rows);
    }
}

/* ---------------------------------------------------------------------------
 * editor_connect_signals() — install the window's key controller and its
 * lifecycle handlers.  The key controller is capture-phase, so the modal
 * image viewer is offered every key before the focus widget.
 * ------------------------------------------------------------------------- */
static void
editor_connect_signals(OnEditor *ed)
{
    GtkEventController *win_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(win_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(win_keys, "key-pressed",
                     G_CALLBACK(on_editor_window_key_pressed), ed);
    gtk_widget_add_controller(ed->window, win_keys);

    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);
    g_signal_connect(ed->window, "notify::is-active",
                     G_CALLBACK(on_editor_active_changed), ed);
}

/* ---------------------------------------------------------------------------
 * editor_window_open_full() — shared implementation behind the four public
 * open functions.  Both extras are optional and mutually independent:
 *   search_term — pre-populates the in-note search box and jumps to the
 *                 first match, so a note opened from the library search
 *                 window lands with its hit highlighted (NULL for none).
 *   reveal/ord  — scrolls to the `ord`-th image or action item of the
 *                 note, so a note opened from the media window lands on
 *                 the thumbnail that was double-clicked and one opened
 *                 from Action Items on that line (ON_REVEAL_NONE for
 *                 neither).
 * ------------------------------------------------------------------------- */
static GtkWidget *
editor_window_open_full(OnApp *app, gint64 note_id, const gchar *search_term,
                        OnReveal reveal, gint ord)
{
    /* Already open?  Just raise the existing window (and re-run the search
     * or the scroll if one was requested — the note may already be up from a
     * prior open).  Its buffer is long since allocated, so the reveal needs
     * no idle deferral here.                                               */
    OnEditor *existing = editor_lookup(app, note_id);
    if (existing != NULL) {
        gtk_window_present(GTK_WINDOW(existing->window));
        if (search_term != NULL && *search_term != '\0')
            editor_apply_search_term(existing, search_term);
        editor_reveal(existing, reveal, ord);
        return existing->window;
    }

    OnNoteMeta *meta = on_db_note_get(app->db, note_id);
    if (meta == NULL) {
        g_warning("editor: note %" G_GINT64_FORMAT " does not exist",
                  note_id);
        return NULL;
    }

    OnEditor *ed = g_new0(OnEditor, 1);
    ed->app     = app;
    ed->note_id = note_id;

    /* --- window: a plain GtkWindow, standard titlebar (no HeaderBar) ---- */
    /* A GtkApplicationWindow, for the "win." action group the toolbar,
     * menus and shortcuts name; it adds itself to the application.  The
     * application menubar is NOT rendered in-window here — on a desktop
     * with no shell menubar that would put File/View atop every editor. */
    ed->window = gtk_application_window_new(app->gtk_app);
    gtk_application_window_set_show_menubar(
        GTK_APPLICATION_WINDOW(ed->window), FALSE);

    /* Open at the configured default size (editor_win_w/editor_win_h in
     * the ini, fixed — unlike the search window's, these are not written
     * back on resize).                                                     */
    gint win_w = EDITOR_WIN_DEFAULT_W;
    gint win_h = EDITOR_WIN_DEFAULT_H;
    on_app_config_get_size("editor_win_w", "editor_win_h", &win_w, &win_h);
    gtk_window_set_default_size(GTK_WINDOW(ed->window), win_w, win_h);
    {
        gchar *wtitle = g_strdup_printf("Notes - %s", meta->title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    editor_build_view(ed);
    editor_install_actions(ed);      /* before the toolbar names them       */
    editor_load_content(ed);
    editor_build_layout(ed);
    editor_connect_signals(ed);

    /* Register in the open-editors table (key freed by the table), and
     * stash the editor state on its window for cross-module access.        */
    gint64 *key = g_new(gint64, 1);
    *key = note_id;
    g_hash_table_insert(app->editors, key, ed->window);
    g_object_set_data(G_OBJECT(ed->window), "on-editor", ed);

    /* Opened from search: apply the query once the window is realized and
     * allocated so the scroll-to-match lands (see on_initial_search_idle). */
    if (search_term != NULL && *search_term != '\0') {
        ed->pending_search = g_strdup(search_term);
        ed->initial_search_idle = g_idle_add(on_initial_search_idle, ed);
    }

    /* Opened AT an image or an action item: same deferral, so the scroll
     * lands on an allocated view.                                          */
    if (reveal != ON_REVEAL_NONE) {
        ed->pending_reveal = reveal;
        ed->pending_ord    = ord;
        ed->initial_reveal_idle = g_idle_add(on_initial_reveal_idle, ed);
    }

    on_db_note_meta_free(meta);
    gtk_window_present(GTK_WINDOW(ed->window));
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    return ed->window;
}

GtkWidget *
on_editor_window_open(OnApp *app, gint64 note_id)
{
    return editor_window_open_full(app, note_id, NULL, ON_REVEAL_NONE, -1);
}

GtkWidget *
on_editor_window_open_search(OnApp *app, gint64 note_id,
                             const gchar *search_term)
{
    return editor_window_open_full(app, note_id, search_term,
                                   ON_REVEAL_NONE, -1);
}

GtkWidget *
on_editor_window_open_image(OnApp *app, gint64 note_id, gint image_ord)
{
    return editor_window_open_full(app, note_id, NULL,
                                   image_ord >= 0 ? ON_REVEAL_IMAGE
                                                  : ON_REVEAL_NONE,
                                   image_ord);
}

GtkWidget *
on_editor_window_open_action(OnApp *app, gint64 note_id, gint action_ord)
{
    return editor_window_open_full(app, note_id, NULL,
                                   action_ord >= 0 ? ON_REVEAL_ACTION
                                                   : ON_REVEAL_NONE,
                                   action_ord);
}
