/* ===========================================================================
 * app.c — shared application helpers (implementation)
 *
 * Icon loading and the shared toolbar-button factory.  Icons live in a
 * plain folder of SVG/PNG files next to the executable ("icons/"), named
 * by the usual freedesktop action names (edit-delete.svg, list-add.svg,
 * …) so users can swap any of them by replacing the file.
 * =========================================================================== */

#include "app.h"
#include "serialize.h"               /* on_note_extract_actions (backfill)  */

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void
on_app_status(OnApp *app, const gchar *fmt, ...)
{
    if (app->notify_status == NULL)
        return;
    va_list args;                    /* printf-style arguments              */
    va_start(args, fmt);
    gchar *message = g_strdup_vprintf(fmt, args);
    va_end(args);
    app->notify_status(app, message);
    g_free(message);
}

gboolean
on_is_emoji_char(gunichar c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) ||   /* emoji + symbols planes    */
           (c >= 0x2600  && c <= 0x27BF)  ||   /* misc symbols, dingbats    */
           (c >= 0x1F1E6 && c <= 0x1F1FF) ||   /* regional indicators       */
           c == 0x2B50 || c == 0x2B55;         /* star, circle              */
}

gboolean
on_is_emoji_joiner(gunichar c)
{
    return c == 0xFE0E || c == 0xFE0F ||       /* variation selectors 15/16 */
           c == 0x200D ||                      /* zero width joiner         */
           c == 0x20E3 ||                      /* combining keycap          */
           (c >= 0xE0020 && c <= 0xE007F);     /* tag characters            */
}

gint
on_emoji_pad(PangoContext *ctx)
{
    PangoLayout *layout = pango_layout_new(ctx);
    pango_layout_set_text(layout, "\xf0\x9f\x8e\x89", -1);   /* party popper */
    PangoRectangle ink, logical;     /* the sample's extents, in px         */
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    g_object_unref(layout);

    gint overhang = ink.x + ink.width - logical.width;
    if (overhang <= 0)
        return 0;                    /* the font fits its advance           */
    return 2 * (overhang + ON_EMOJI_GAP) * PANGO_SCALE;
}

gchar *
on_markup_escape_emoji(const gchar *text, gint pad)
{
    if (text == NULL)
        text = "";
    if (pad == 0)
        return g_markup_escape_text(text, -1);

    GString *out = g_string_new(NULL); /* the markup being built           */
    const gchar *p = text;           /* scan cursor                         */
    const gchar *plain = text;       /* start of the pending unpadded run   */
    while (*p != '\0') {
        if (!on_is_emoji_char(g_utf8_get_char(p))) {
            p = g_utf8_next_char(p);
            continue;
        }
        const gchar *run = p;        /* start of the emoji run              */
        while (*p != '\0' && (on_is_emoji_char(g_utf8_get_char(p)) ||
                               on_is_emoji_joiner(g_utf8_get_char(p))))
            p = g_utf8_next_char(p);
        gchar *esc = g_markup_escape_text(plain, run - plain);
        g_string_append(out, esc);
        g_free(esc);
        esc = g_markup_escape_text(run, p - run);
        g_string_append_printf(out, "<span letter_spacing=\"%d\">%s</span>",
                               pad, esc);
        g_free(esc);
        plain = p;
    }
    gchar *esc = g_markup_escape_text(plain, -1);
    g_string_append(out, esc);
    g_free(esc);
    return g_string_free(out, FALSE);
}

gchar *
on_app_location_text(OnApp *app, const gchar *location)
{
    if (!app->statusbar_db_path || app->db == NULL ||
        app->db->path == NULL)
        return g_strdup(location);
    /* One continuous path: folder locations already start with "/"; the
     * non-path views ("#tag", "Pinned Notes", …) get one inserted.        */
    return g_strdup_printf("%s%s%s", app->db->path,
                           location != NULL && location[0] == '/' ? ""
                                                                  : "/",
                           location);
}

/* db_health_done() — the async pass landed: report it.                     */
static void
db_health_done(OnDatabase *db, gpointer data)
{
    OnApp *app = data;
    const OnDbHealth *h = on_db_health(db);
    if (h != NULL && h->ok) {
        on_app_status(app, "DB at %s loaded, integrity check passed",
                      db->path);
    } else {
        on_app_notice(NULL, "Notes - Database Integrity Check",
                      "%s\n\n%s",
                      h != NULL && h->ran
                          ? "The database integrity check found issues:"
                          : "The database integrity check could not be "
                            "completed:",
                      h != NULL && h->detail != NULL ? h->detail
                                                     : "no detail reported");
    }
    if (app->notify_db_health != NULL)
        app->notify_db_health(app);
}

void
on_app_db_health_start(OnApp *app)
{
    on_db_health_check_async(app->db, db_health_done, app);
    if (app->notify_db_health != NULL)
        app->notify_db_health(app);  /* the plate shows "checking"          */
}

void
on_app_notice(GtkWindow *parent, const gchar *title, const gchar *fmt, ...)
{
    va_list args;                    /* printf-style arguments              */
    va_start(args, fmt);
    gchar *message = g_strdup_vprintf(fmt, args);
    va_end(args);

    /* The heading is the title when there is one, with the message as the
     * detail line beneath it; without a title the message IS the heading.
     * gtk_alert_dialog_show copies everything into the window it presents,
     * so the dialog object is not needed once it is up.                    */
    GtkAlertDialog *dialog =
        gtk_alert_dialog_new("%s", title != NULL ? title : message);
    if (title != NULL)
        gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_show(dialog, parent);
    g_object_unref(dialog);
    g_free(message);
}

/* PickJob — what on_app_pick_path() carries across the async gap: the
 * chooser's kind (which decides the *_finish call) and the completion.    */
typedef struct {
    OnPickKind  kind;                /* open / save / folder                */
    OnPickFunc  done;                /* the caller's continuation           */
    gpointer    user_data;           /* passed to done                      */
} PickJob;

/* ---------------------------------------------------------------------------
 * pick_path_done() — GAsyncReadyCallback for on_app_pick_path(): turn the
 * chooser's result into a filesystem path (NULL when cancelled or
 * dismissed) and hand it to the job's completion, then drop the dialog and
 * the job.
 *   source    — the GtkFileDialog.
 *   result    — the async result.
 *   user_data — the PickJob.
 * ------------------------------------------------------------------------- */
static void
pick_path_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    PickJob       *job    = user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile         *file;             /* the selection, NULL if cancelled    */

    /* A cancel comes back as GTK_DIALOG_ERROR_DISMISSED with a NULL file —
     * that is the answer, not an error worth reporting.                    */
    switch (job->kind) {
    case ON_PICK_OPEN:
        file = gtk_file_dialog_open_finish(dialog, result, NULL);
        break;
    case ON_PICK_SAVE:
        file = gtk_file_dialog_save_finish(dialog, result, NULL);
        break;
    default:
        file = gtk_file_dialog_select_folder_finish(dialog, result, NULL);
        break;
    }
    gchar *path = (file != NULL) ? g_file_get_path(file) : NULL;
    g_clear_object(&file);

    job->done(path, job->user_data);  /* the completion owns path           */
    g_object_unref(dialog);          /* the ref on_app_pick_path took       */
    g_free(job);
}

void
on_app_pick_path(GtkWindow *parent, const gchar *title,
                 OnPickKind kind, const gchar *accept_label,
                 const gchar *filter_name, const gchar *filter_pattern,
                 const gchar *start_dir,
                 OnPickFunc done, gpointer user_data)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_set_accept_label(dialog, accept_label);
    if (start_dir != NULL) {
        GFile *folder = g_file_new_for_path(start_dir);
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    if (filter_name != NULL) {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filter_name);
        if (filter_pattern != NULL) {
            gtk_file_filter_add_pattern(filter, filter_pattern);
        } else {
            /* Every format the gdk-pixbuf loaders decode — which is what
             * the caller will load the pick with — by MIME type (the
             * one-call gtk_file_filter_add_pixbuf_formats is deprecated). */
            GSList *formats = gdk_pixbuf_get_formats();
            for (GSList *l = formats; l != NULL; l = l->next) {
                gchar **mimes = gdk_pixbuf_format_get_mime_types(l->data);
                for (gchar **m = mimes; m != NULL && *m != NULL; m++)
                    gtk_file_filter_add_mime_type(filter, *m);
                g_strfreev(mimes);
            }
            g_slist_free(formats);
        }
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, filter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filters);
        g_object_unref(filter);
    }

    PickJob *job   = g_new0(PickJob, 1);
    job->kind      = kind;
    job->done      = done;
    job->user_data = user_data;
    /* The dialog stays referenced until pick_path_done runs.               */
    switch (kind) {
    case ON_PICK_OPEN:
        gtk_file_dialog_open(dialog, parent, NULL, pick_path_done, job);
        break;
    case ON_PICK_SAVE:
        gtk_file_dialog_save(dialog, parent, NULL, pick_path_done, job);
        break;
    default:
        gtk_file_dialog_select_folder(dialog, parent, NULL, pick_path_done,
                                      job);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * menu_popup_drop() — take on_app_menu_popup()'s popover down for good:
 * stop watching its parent, unparent it if it still has one, and release
 * the reference the popup took.  Safe to call twice (the second call finds
 * nothing to do).
 *   popover — the popover.
 * ------------------------------------------------------------------------- */
static void
menu_popup_drop(GtkWidget *popover)
{
    guint idle = GPOINTER_TO_UINT(
        g_object_steal_data(G_OBJECT(popover), "on-popup-idle"));
    if (idle != 0)
        g_source_remove(idle);
    gulong handler = GPOINTER_TO_SIZE(
        g_object_steal_data(G_OBJECT(popover), "on-popup-unrealize"));
    GtkWidget *parent = gtk_widget_get_parent(popover);
    if (parent != NULL) {
        if (handler != 0)
            g_signal_handler_disconnect(parent, handler);
        gtk_widget_unparent(popover);
    }
    if (g_object_steal_data(G_OBJECT(popover), "on-popup-ref") != NULL)
        g_object_unref(popover);
}

/* menu_popup_idle() — idle continuation of menu_popup_closed().            */
static gboolean
menu_popup_idle(gpointer data)
{
    g_object_set_data(G_OBJECT(data), "on-popup-idle", NULL);
    menu_popup_drop(data);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * menu_popup_closed() — "closed" handler for on_app_menu_popup()'s
 * popover.  The teardown is deferred to an idle: "closed" fires from
 * inside the popover's own popdown, and the chosen item's action may
 * still be on the stack.
 *   popover   — the popover that closed.
 *   user_data — unused.
 * ------------------------------------------------------------------------- */
static void
menu_popup_closed(GtkPopover *popover, gpointer user_data)
{
    (void)user_data;
    g_object_set_data(G_OBJECT(popover), "on-popup-idle",
                      GUINT_TO_POINTER(g_idle_add(menu_popup_idle, popover)));
}

/* ---------------------------------------------------------------------------
 * menu_popup_parent_unrealize() — the popover's parent is being torn down
 * (its window destroyed) while the popover is still up or still waiting
 * for its idle.  Drop the popover NOW, so the idle never unparents from a
 * dead widget.
 *   parent    — the widget losing its realization.
 *   user_data — the popover.
 * ------------------------------------------------------------------------- */
static void
menu_popup_parent_unrealize(GtkWidget *parent, gpointer user_data)
{
    (void)parent;
    menu_popup_drop(user_data);
}

void
on_app_menu_popup(GtkWidget *attach, GMenuModel *model, gdouble x, gdouble y)
{
    /* The popover's PARENT is the window's own child box, not `attach`.
     * Measured on 4.22: a popover parented to a deprecated GtkTreeView
     * trips gtk_css_node_insert_after (the view keeps its header buttons
     * under a private sub-node, so a foreign child breaks its sibling
     * chain) and comes up the wrong size; a GtkTextView disposing with a
     * foreign child never gets past it.  A GtkBox takes a popover cleanly
     * — layout skips GtkNative children — and every window's child is one.
     * The press is translated into that box's coordinates.                 */
    GtkRoot *root = gtk_widget_get_root(attach);
    GtkWidget *parent = gtk_window_get_child(GTK_WINDOW(root));
    graphene_point_t at_parent;      /* the press, in the parent's space    */
    if (!gtk_widget_compute_point(attach, parent,
                                  &GRAPHENE_POINT_INIT((float)x, (float)y),
                                  &at_parent))
        at_parent = GRAPHENE_POINT_INIT((float)x, (float)y);

    GtkWidget *popover = gtk_popover_menu_new_from_model(model);
    g_object_unref(model);           /* the popover holds its own reference */
    gtk_widget_set_parent(popover, parent);
    /* Our own reference outlives the parent's, so the popover stays a valid
     * object until menu_popup_drop has run whichever way it is reached.     */
    g_object_set_data(G_OBJECT(popover), "on-popup-ref", g_object_ref(popover));
    g_object_set_data(G_OBJECT(popover), "on-popup-unrealize",
        GSIZE_TO_POINTER(g_signal_connect(parent, "unrealize",
            G_CALLBACK(menu_popup_parent_unrealize), popover)));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    GdkRectangle at = { (gint)at_parent.x, (gint)at_parent.y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &at);
    g_signal_connect(popover, "closed", G_CALLBACK(menu_popup_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

void
on_app_install_accels(GtkApplication *gtk_app)
{
    /* One action may take several keys (redo), and one key may serve
     * several actions (Primary+M) — GTK tries an accel's actions in the
     * order they were registered and takes the first the focused window
     * has and has enabled.  Detailed names with a target ("win.para::code")
     * are accepted as-is.                                                   */
    static const struct {
        const gchar *action;         /* detailed action name                */
        const gchar *accel;          /* accelerator string                  */
    } ACCELS[] = {
        /* app-level: the two the macOS app menu binds itself anyway       */
        { "app.quit",              "<Primary>q"        },
        { "app.preferences",       "<Primary>comma"    },
        /* both windows                                                     */
        { "win.new-note",          "<Primary>n"        },
        { "win.find",              "<Primary>f"        },
        /* library                                                          */
        { "win.media",             "<Primary>m"        },
        /* editor                                                           */
        { "win.para::code",        "<Primary>m"        },
        { "win.undo",              "<Primary>z"        },
        { "win.redo",              "<Primary><Shift>z" },
        { "win.redo",              "<Primary>y"        },
        { "win.inline::bold",      "<Primary>b"        },
        { "win.inline::italic",    "<Primary>i"        },
        { "win.inline::underline", "<Primary>u"        },
        { "win.insert-date",       "<Primary>d"        },
        { "win.insert-emoji",      "<Primary>e"        },
    };

    /* GTK4 parses "<Primary>" as Control on EVERY platform (gtkaccelgroup.c:
     * is_primary → GDK_CONTROL_MASK); the Command key is GDK_META_MASK.
     * The table is written with <Primary> and spelled out here per
     * platform — Command on macOS, Control elsewhere.                       */
#ifdef __APPLE__
    static const gchar *const PRIMARY = "<Meta>";
#else
    static const gchar *const PRIMARY = "<Control>";
#endif
    for (gsize i = 0; i < G_N_ELEMENTS(ACCELS); i++) {
        /* Collect every accel already bound to this action so a second row
         * for the same action adds a key rather than replacing the first. */
        gchar **have = gtk_application_get_accels_for_action(
            gtk_app, ACCELS[i].action);
        gsize n = g_strv_length(have);
        gchar **all = g_new0(gchar *, n + 2);
        for (gsize j = 0; j < n; j++)
            all[j] = have[j];
        gchar *accel = g_strdup(ACCELS[i].accel);   /* platform-spelled     */
        if (g_str_has_prefix(accel, "<Primary>")) {
            gchar *spelled = g_strconcat(PRIMARY, accel + strlen("<Primary>"),
                                         NULL);
            g_free(accel);
            accel = spelled;
        }
        all[n] = accel;
        gtk_application_set_accels_for_action(gtk_app, ACCELS[i].action,
                                              (const gchar *const *)all);
        g_strfreev(have);
        g_free(accel);
        g_free(all);
    }
}

/* ---------------------------------------------------------------------------
 * exe_dir_from_argv0() — the directory containing the executable; when
 * launched via a bare name from PATH there is no directory part, so fall
 * back to the current working directory.  Returns a new string.
 * ------------------------------------------------------------------------- */
static gchar *
exe_dir_from_argv0(const gchar *argv0)
{
    if (argv0 != NULL && strchr(argv0, G_DIR_SEPARATOR) != NULL) {
        gchar *abs = g_canonicalize_filename(argv0, NULL);
        gchar *dir = g_path_get_dirname(abs);
        g_free(abs);
        return dir;
    }
    return g_get_current_dir();
}

void
on_app_init_icons_dir(OnApp *app, const gchar *argv0)
{
    gchar *exe_dir = exe_dir_from_argv0(argv0);
    app->icons_dir = g_build_filename(exe_dir, "icons", NULL);
    g_free(exe_dir);
}

GdkTexture *
on_app_texture_for_pixbuf(GdkPixbuf *pixbuf)
{
    /* gdk-pixbuf is 8-bit RGB(A), unpremultiplied — exactly R8G8B8(A8).
     * read_pixel_bytes shares the pixbuf's buffer, so nothing is copied. */
    GBytes *bytes = gdk_pixbuf_read_pixel_bytes(pixbuf);
    GdkTexture *texture = gdk_memory_texture_new(
        gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
        gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8
                                         : GDK_MEMORY_R8G8B8,
        bytes, (gsize)gdk_pixbuf_get_rowstride(pixbuf));
    g_bytes_unref(bytes);
    return texture;
}

/* ---------------------------------------------------------------------------
 * display_scale_factor() — the integer scale factor of the display's
 * first monitor (2 on Retina), 1 when no display or monitor is known yet.
 * GTK4 has no "primary" monitor; the first listed one is the app's home
 * for the purpose of rasterizing icons sharply.
 * ------------------------------------------------------------------------- */
static gint
display_scale_factor(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL)
        return 1;
    GdkMonitor *monitor =            /* a new reference                     */
        g_list_model_get_item(gdk_display_get_monitors(display), 0);
    if (monitor == NULL)
        return 1;
    gint sf = gdk_monitor_get_scale_factor(monitor);
    g_object_unref(monitor);
    return sf;
}

/* icon_theme_has() — does the icon theme know `name`?  Both lookups below
 * go through the theme (main.c adds icons/ as a search path, where GTK
 * picks the PNGs up as "unthemed" icons by basename), so this is THE test
 * for "a file exists and loads" — GTK would otherwise hand back its
 * missing-image placeholder, and the caller wants the text fallback.       */
static gboolean
icon_theme_has(const gchar *name)
{
    return gtk_icon_theme_has_icon(
        gtk_icon_theme_get_for_display(gdk_display_get_default()), name);
}

GdkPaintable *
on_app_icon_paintable(OnApp *app, const gchar *name, gint size)
{
    (void)app;                       /* the theme knows the directory       */
    if (!icon_theme_has(name))
        return NULL;
    /* Looked up at the display's scale factor, so a drag icon stays sharp
     * on HiDPI: GtkIconPaintable renders the PNG at size × scale pixels
     * and reports the logical size.                                         */
    GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(
        gtk_icon_theme_get_for_display(gdk_display_get_default()),
        name, NULL, size, display_scale_factor(), GTK_TEXT_DIR_NONE, 0);
    return GDK_PAINTABLE(icon);
}

GtkWidget *
on_app_icon_image_sized(OnApp *app, const gchar *name, gint size)
{
    (void)app;
    if (!icon_theme_has(name))
        return NULL;
    /* A themed image: GTK loads the file at the widget's own scale factor
     * (and reloads it if the widget moves to another display), caches it,
     * and draws it at the logical pixel size.                              */
    GtkWidget *image = gtk_image_new_from_icon_name(name);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

/* on_app_icon_image() — toolbar-default (24 px) icon variant.               */
static GtkWidget *
on_app_icon_image(OnApp *app, const gchar *name)
{
    return on_app_icon_image_sized(app, name, 24);
}

/*
 * tool_icon_widget — the icon widget for a toolbar button: the local image
 * if one loads, else the fallback markup rendered as a label standing in
 * for it.  THE one place that rule lives, shared by on_app_tool_item_new
 * and on_app_tool_item_set_icon, so a button built with an icon and a
 * button RE-pointed at one cannot come to disagree about the fallback.
 *
 * Inputs:
 *   app             — application context (holds icons_dir)
 *   icon_name       — icon file basename, or NULL for markup only
 *   fallback_markup — Pango markup when the file does not load, or NULL
 *   label           — last-resort text when both are absent
 *
 * Output:
 *   a floating GtkWidget for the caller to parent.  Never NULL.
 */
static GtkWidget *
tool_icon_widget(OnApp *app, const gchar *icon_name,
                 const gchar *fallback_markup, const gchar *label)
{
    GtkWidget *icon = (icon_name != NULL)
                      ? on_app_icon_image(app, icon_name) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup
                                                     : label);
    }
    return icon;
}

/* ---------------------------------------------------------------------------
 * tooltips (see on_app_set_tooltip in app.h for why this exists)
 * ------------------------------------------------------------------------- */

/* A tooltip asked for within this long of the previous one hiding is
 * refused and asked for again once the time has passed.  550 ms is past
 * GTK's browse-mode window (500 ms), so the re-ask goes through the
 * normal hover delay and the popup shows about a second after the last
 * one hid — measured clean; 400 ms (shown at 464) was still cut.       */
#define TOOLTIP_MIN_GAP_MS 550

/* Object-data key: the label that shows the widget's tooltip, built once
 * per widget and reused, so GTK sees the same custom widget every query. */
#define TOOLTIP_LABEL_KEY "on-tooltip-label"

static GtkWidget *tooltip_mapped;    /* the label on show right now, if any */
static gint64     tooltip_hidden_at; /* when the last one unmapped          */

static void
on_tooltip_label_map(GtkWidget *label, gpointer data)
{
    (void)data;
    tooltip_mapped = label;
}

static void
on_tooltip_label_unmap(GtkWidget *label, gpointer data)
{
    (void)data;
    if (tooltip_mapped == label)
        tooltip_mapped = NULL;
    tooltip_hidden_at = g_get_monotonic_time();
}

/* tooltip_ask_again() — the deferred re-query (holds a widget ref).       */
static gboolean
tooltip_ask_again(gpointer widget)
{
    gtk_widget_trigger_tooltip_query(widget);
    g_object_unref(widget);
    return G_SOURCE_REMOVE;
}

/* on_query_tooltip() — hand GTK the widget's label as the tooltip, unless
 * the previous tooltip hid a moment ago: then refuse and ask again later. */
static gboolean
on_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard,
                 GtkTooltip *tooltip, gpointer data)
{
    (void)x; (void)y; (void)keyboard; (void)data;
    GtkWidget *label = g_object_get_data(G_OBJECT(widget), TOOLTIP_LABEL_KEY);
    if (label == NULL)
        return FALSE;
    if (tooltip_mapped == NULL) {
        gint64 gap = g_get_monotonic_time() - tooltip_hidden_at;
        if (gap < TOOLTIP_MIN_GAP_MS * 1000) {
            g_timeout_add((guint)((TOOLTIP_MIN_GAP_MS * 1000 - gap) / 1000) + 1,
                          tooltip_ask_again, g_object_ref(widget));
            return FALSE;
        }
    }
    gtk_tooltip_set_custom(tooltip, label);
    return TRUE;
}

void
on_app_set_tooltip(GtkWidget *widget, const gchar *text)
{
    if (text == NULL || *text == '\0') {
        g_object_set_data(G_OBJECT(widget), TOOLTIP_LABEL_KEY, NULL);
        gtk_widget_set_has_tooltip(widget, FALSE);
        return;
    }
    GtkWidget *label = g_object_get_data(G_OBJECT(widget), TOOLTIP_LABEL_KEY);
    if (label == NULL) {
        label = gtk_label_new(text);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        g_signal_connect(label, "map", G_CALLBACK(on_tooltip_label_map), NULL);
        g_signal_connect(label, "unmap", G_CALLBACK(on_tooltip_label_unmap),
                         NULL);
        g_object_set_data_full(G_OBJECT(widget), TOOLTIP_LABEL_KEY,
                               g_object_ref_sink(label), g_object_unref);
        g_signal_connect(widget, "query-tooltip",
                         G_CALLBACK(on_query_tooltip), NULL);
    } else {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
    gtk_widget_set_has_tooltip(widget, TRUE);
}

/* Object-data key under which a toolbar button keeps its accessible label,
 * so on_app_tool_item_set_icon can rebuild the fallback glyph from it.     */
#define TOOL_LABEL_KEY "on-tool-label"

GtkWidget *
on_app_tool_item_new(OnApp *app, gboolean toggle, const gchar *icon_name,
                     const gchar *fallback_markup, const gchar *label,
                     const gchar *tooltip)
{
    GtkWidget *button = toggle ? gtk_toggle_button_new() : gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);   /* flat         */
    /* A toolbar press must not steal the focus from the text view: the
     * editor's editing actions are gated on that focus (D11).             */
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_button_set_child(GTK_BUTTON(button),
        tool_icon_widget(app, icon_name, fallback_markup, label));
    on_app_set_tooltip(button, tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                   -1);
    g_object_set_data_full(G_OBJECT(button), TOOL_LABEL_KEY,
                           g_strdup(label), g_free);
    return button;
}

void
on_app_tool_item_set_icon(OnApp *app, GtkWidget *button,
                          const gchar *icon_name,
                          const gchar *fallback_markup)
{
    /* set_child unparents and drops the old icon widget, which held the
     * only reference to it, so the previous image is freed by this call.  */
    gtk_button_set_child(GTK_BUTTON(button),
        tool_icon_widget(app, icon_name, fallback_markup,
                         g_object_get_data(G_OBJECT(button),
                                           TOOL_LABEL_KEY)));
}

/* ---------------------------------------------------------------------------
 * The application config: "notes.ini" in the same directory as
 * the binary.  on_app_config_init() resolves the path and loads the
 * whole file into memory ONCE; every read is served from memory and the
 * file is only touched again to write a modification through.  All keys
 * live under one [notes] group.
 * ------------------------------------------------------------------------- */
static gchar    *config_ini_path = NULL;   /* resolved ini path             */
static GKeyFile *config_kf       = NULL;   /* in-memory settings            */

#define CONFIG_GROUP "notes"

void
on_app_config_init(const gchar *argv0)
{
    if (config_kf != NULL)
        return;                      /* already resolved and loaded         */
    gchar *exe_dir = exe_dir_from_argv0(argv0);
    config_ini_path = g_build_filename(exe_dir, "notes.ini", NULL);

    /* Portable mode (the usual case: binary run from its build/unpack
     * directory) keeps the ini next to the binary.  System installs
     * (.deb/.rpm/.app in /Applications: read-only binary dir) would fail
     * every write-through, so when no binary-adjacent ini exists AND the
     * directory is unwritable, the ini lives in the user config dir
     * instead (~/.config/notes/notes.ini).                             */
    if (!g_file_test(config_ini_path, G_FILE_TEST_EXISTS) &&
        g_access(exe_dir, W_OK) != 0) {
        g_free(config_ini_path);
        gchar *cfg_dir = g_build_filename(g_get_user_config_dir(),
                                          "notes", NULL);
        g_mkdir_with_parents(cfg_dir, 0755);
        config_ini_path = g_build_filename(cfg_dir, "notes.ini",
                                           NULL);
        g_free(cfg_dir);
    }

    /* First launch (no ini yet): seed it from the defaults file shipped
     * next to the binary, so a fresh install starts with sane settings.    */
    if (!g_file_test(config_ini_path, G_FILE_TEST_EXISTS)) {
        gchar *defaults_path = g_build_filename(
            exe_dir, "notes.ini.defaults", NULL);
        gchar *contents = NULL;      /* defaults file body                  */
        gsize  len = 0;
        if (g_file_get_contents(defaults_path, &contents, &len, NULL)) {
            GError *err = NULL;
            if (!g_file_set_contents(config_ini_path, contents, len,
                                     &err)) {
                g_warning("config: cannot seed %s: %s", config_ini_path,
                          err->message);
                g_clear_error(&err);
            }
            g_free(contents);
        }
        g_free(defaults_path);
    }
    g_free(exe_dir);

    config_kf = g_key_file_new();
    g_key_file_load_from_file(config_kf, config_ini_path,
                              G_KEY_FILE_NONE, NULL);  /* absent file OK    */
}

/* config_write() — flush the in-memory settings to the ini file.            */
static void
config_write(void)
{
    GError *err = NULL;
    if (!g_key_file_save_to_file(config_kf, config_ini_path, &err)) {
        g_warning("config: cannot save %s: %s", config_ini_path,
                  err->message);
        g_clear_error(&err);
    }
}

gchar *
on_app_config_get(const gchar *key)
{
    if (config_kf == NULL)
        return NULL;
    gchar *value = g_key_file_get_string(config_kf, CONFIG_GROUP, key,
                                         NULL);
    if (value != NULL && *value == '\0') {
        g_free(value);
        value = NULL;
    }
    return value;
}

gboolean
on_app_config_get_bool(const gchar *key, gboolean def)
{
    gchar *v = on_app_config_get(key);
    gboolean r = (v == NULL) ? def : g_strcmp0(v, "0") != 0;
    g_free(v);
    return r;
}

void
on_app_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL)
        return;

    /* Skip the file rewrite when the value isn't changing — some callers
     * fire per keystroke (the image-viewer entry).                         */
    gchar *cur = g_key_file_get_string(config_kf, CONFIG_GROUP, key, NULL);
    gboolean same = g_strcmp0(cur, value) == 0;
    g_free(cur);
    if (same)
        return;

    if (value != NULL)
        g_key_file_set_string(config_kf, CONFIG_GROUP, key, value);
    else
        g_key_file_remove_key(config_kf, CONFIG_GROUP, key, NULL);
    config_write();
}

void
on_app_config_get_size(const gchar *key_w, const gchar *key_h,
                       gint *w, gint *h)
{
    gchar *w_str = on_app_config_get(key_w);
    gchar *h_str = on_app_config_get(key_h);
    if (w_str != NULL && h_str != NULL) {
        gint pw = (gint)g_ascii_strtoll(w_str, NULL, 10);  /* parsed width  */
        gint ph = (gint)g_ascii_strtoll(h_str, NULL, 10);  /* parsed height */
        if (pw > 0 && ph > 0) {      /* both or neither: never a half size  */
            *w = pw;
            *h = ph;
        }
    }
    g_free(w_str);
    g_free(h_str);
}

gchar *
on_app_read_stream(FILE *f, gboolean rewind_first)
{
    GString *s = g_string_new(NULL);
    if (rewind_first)
        rewind(f);
    gchar  buf[4096];                /* read chunk                          */
    gsize  n;                        /* bytes in this chunk                 */
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        g_string_append_len(s, buf, (gssize)n);
    return g_string_free(s, FALSE);
}

gchar *
on_app_config_load_db_dir(void)
{
    return on_app_config_get("db_dir");
}

void
on_app_install_css(void)
{
    static gboolean installed = FALSE;
    if (installed)
        return;
    installed = TRUE;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "label.notes-status-label { font-size: 85%; }"
        "label.notes-dot-label { font-size: 70%; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void
on_app_apply_touch_assist(OnApp *app)
{
    gboolean assist =                /* default: disabled                   */
        on_app_config_get_bool("touch_assist", FALSE);

    if (!assist && app->touch_css == NULL) {
        /* Node names VERIFIED against GTK 4.22.4: GtkTextHandle's CSS name
         * is "cursor-handle" (gtktexthandle.c, and the Default theme
         * styles it), and GtkTextView adds the "magnifier" class to the
         * GtkPopover holding its GtkMagnifier (gtktextview.c).            */
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css,
            /* Selection/cursor handles: collapse the nodes entirely — no
             * themed teardrop graphic and a 0x0 allocation, so they
             * neither draw nor grab pointer input.  -gtk-icon-source is
             * still the property that paints the teardrop on 4.22
             * (gtk_text_handle_snapshot -> gtk_css_style_snapshot_icon;
             * the theme sets it per handle class), so "none" is what
             * removes the graphic.                                          */
            "cursor-handle {"
            "  -gtk-icon-source: none;"
            "  background: none;"
            "  border: none;"
            "  box-shadow: none;"
            "  min-width: 0;"
            "  min-height: 0;"
            "  padding: 0;"
            "  margin: 0;"
            "}"
            /* Touch-selection magnifier: its popover cannot be collapsed
             * (GtkTextView puts a hard size request on the content), so
             * render the whole thing fully transparent instead.            */
            "popover.magnifier {"
            "  opacity: 0;"
            "  background: none;"
            "  border: none;"
            "  box-shadow: none;"
            "}");
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        app->touch_css = css;
    } else if (assist && app->touch_css != NULL) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(app->touch_css));
        g_object_unref(app->touch_css);
        app->touch_css = NULL;
    }
}

void
on_app_close_all_editors(OnApp *app)
{
    /* Destroying an editor removes it from the table, so copy the window
     * list first.                                                          */
    GList *windows = g_hash_table_get_values(app->editors);
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_window_destroy(GTK_WINDOW(l->data));
    g_list_free(windows);
}

/* The user_version that says the action_items table has been backfilled
 * from pre-existing note content.  2: re-indexed once after the
 * due-date-blind change comparison left due-only edits stale in the
 * table (2026-07).                                                          */
#define DB_VERSION_ACTIONS 2

/* The user_version that says every action_items row carries a stable uid
 * (2026-08).  Strictly above DB_VERSION_ACTIONS: the rows must exist
 * before they can be given ids.                                            */
#define DB_VERSION_ACTION_UIDS 3

void
on_app_actions_backfill(OnDatabase *db)
{
    if (db == NULL || on_db_user_version(db) >= DB_VERSION_ACTIONS)
        return;

    /* Trashed notes are included so a later restore is already indexed;
     * the library's queries filter them out.  Same cheap record walk as
     * the body_text extraction (image payloads skipped) — one-time cost.   */
    GList *notes = on_db_note_list_all(db, TRUE);
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */
        gsize   blob_len = 0;        /* stored blob size                    */
        guint8 *blob = on_db_note_load(db, m->id, &blob_len);
        if (blob == NULL)
            continue;
        GList *actions = on_note_extract_actions(blob, blob_len);
        if (actions != NULL)         /* empty sets have no rows to write    */
            on_db_note_set_actions(db, m->id, actions);
        on_db_action_list_free(actions);
        g_free(blob);
    }
    on_db_note_list_free(notes);
    on_db_set_user_version(db, DB_VERSION_ACTIONS);
}

void
on_app_action_uids_backfill(OnDatabase *db)
{
    if (db == NULL)
        return;
    /* Normally a one-shot gated by the version stamp.  It also re-runs
     * whenever any row has no uid, which happens when an OLDER build
     * (whose INSERT predates the column) saves a note in a database this
     * one has already migrated: without the self-heal those rows would
     * keep uid 0 forever, since the stamp is already current.  The probe
     * is an indexed existence check, so the common case costs nothing.    */
    if (on_db_user_version(db) >= DB_VERSION_ACTION_UIDS &&
        !on_db_action_uids_missing(db))
        return;
    if (on_db_action_uids_fill(db))
        on_db_set_user_version(db, DB_VERSION_ACTION_UIDS);
}

