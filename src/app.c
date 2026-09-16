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

void
on_app_notice(GtkWindow *parent, GtkMessageType type,
              const gchar *title, const gchar *fmt, ...)
{
    va_list args;                    /* printf-style arguments              */
    va_start(args, fmt);
    gchar *message = g_strdup_vprintf(fmt, args);
    va_end(args);

    GtkWidget *dialog = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message);
    if (title != NULL)
        gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(message);
}

gchar *
on_app_pick_path(GtkWindow *parent, const gchar *title,
                 GtkFileChooserAction action, const gchar *accept_label,
                 const gchar *filter_name, const gchar *filter_pattern)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        title, parent, action,
        "_Cancel",    GTK_RESPONSE_CANCEL,
        accept_label, GTK_RESPONSE_ACCEPT,
        NULL);
    if (filter_name != NULL) {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filter_name);
        gtk_file_filter_add_pattern(filter, filter_pattern);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    }
    gchar *path = NULL;              /* the selection, NULL if cancelled    */
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return path;
}

void
on_app_widget_add_css(GtkWidget *widget, const gchar *css_text)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, css_text, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
                                   GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

void
on_app_menu_popup(GtkWidget *attach, GMenuModel *model,
                  GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new_from_model(model);
    g_object_unref(model);           /* the menu holds its own reference    */
    gtk_menu_attach_to_widget(GTK_MENU(menu), attach, NULL);
    /* "selection-done" fires AFTER the chosen item has activated, so the
     * destroy never races the action.                                       */
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
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

    for (gsize i = 0; i < G_N_ELEMENTS(ACCELS); i++) {
        /* Collect every accel already bound to this action so a second row
         * for the same action adds a key rather than replacing the first. */
        gchar **have = gtk_application_get_accels_for_action(
            gtk_app, ACCELS[i].action);
        gsize n = g_strv_length(have);
        gchar **all = g_new0(gchar *, n + 2);
        for (gsize j = 0; j < n; j++)
            all[j] = have[j];
        all[n] = (gchar *)ACCELS[i].accel;
        gtk_application_set_accels_for_action(gtk_app, ACCELS[i].action,
                                              (const gchar *const *)all);
        g_strfreev(have);
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

cairo_surface_t *
on_app_icon_surface(OnApp *app, const gchar *name, gint size)
{
    static const gchar *EXTS[] = { "svg", "png" };

    /* Rasterize at the display's scale factor so icons stay sharp on
     * HiDPI/Retina screens: `size` is the LOGICAL size, the backing
     * pixels are size × sf, and the cairo surface's device scale maps
     * between the two.                                                     */
    gint sf = 1;                     /* display scale factor                */
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if (monitor == NULL)
            monitor = gdk_display_get_monitor(display, 0);
        if (monitor != NULL)
            sf = gdk_monitor_get_scale_factor(monitor);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(EXTS); i++) {
        gchar *path = g_strdup_printf("%s%c%s.%s",
                                      app->icons_dir, G_DIR_SEPARATOR,
                                      name, EXTS[i]);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            /* Verify the file actually decodes (SVGs need the librsvg
             * pixbuf loader) — a broken-image icon is worse than the
             * text fallback the caller provides.                           */
            GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_size(
                path, size * sf, size * sf, NULL);
            if (pix != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
                g_object_unref(pix);
                g_free(path);
                return surface;
            }
        }
        g_free(path);
    }
    return NULL;
}

GtkWidget *
on_app_icon_image_sized(OnApp *app, const gchar *name, gint size)
{
    cairo_surface_t *surface = on_app_icon_surface(app, name, size);
    if (surface == NULL)
        return NULL;
    GtkWidget *image = gtk_image_new_from_surface(surface);
    cairo_surface_destroy(surface);
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
 *   a shown, floating GtkWidget for the caller to parent.  Never NULL.
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
    gtk_widget_show(icon);
    return icon;
}

GtkToolItem *
on_app_tool_item_new(OnApp *app, gboolean toggle, const gchar *icon_name,
                     const gchar *fallback_markup, const gchar *label,
                     const gchar *tooltip)
{
    GtkToolItem *item = toggle
        ? GTK_TOOL_ITEM(gtk_toggle_tool_button_new())
        : GTK_TOOL_ITEM(gtk_tool_button_new(NULL, NULL));
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), label);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item),
        tool_icon_widget(app, icon_name, fallback_markup, label));
    gtk_tool_item_set_tooltip_text(item, tooltip);
    return item;
}

void
on_app_tool_item_set_icon(OnApp *app, GtkToolItem *item,
                          const gchar *icon_name,
                          const gchar *fallback_markup)
{
    /* GTK unparents and drops the old icon widget, which held the only
     * reference to it, so the previous image is freed by this call.       */
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item),
        tool_icon_widget(app, icon_name, fallback_markup,
                         gtk_tool_button_get_label(GTK_TOOL_BUTTON(item))));
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
on_app_apply_touch_assist(OnApp *app)
{
    gboolean assist =                /* default: disabled                   */
        on_app_config_get_bool("touch_assist", FALSE);

    if (!assist && app->touch_css == NULL) {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            /* Selection/cursor handles: collapse the nodes entirely — no
             * themed teardrop graphic and a 0x0 allocation, so they
             * neither draw nor grab pointer input.                         */
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
            "}",
            -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        app->touch_css = css;
    } else if (assist && app->touch_css != NULL) {
        gtk_style_context_remove_provider_for_screen(
            gdk_screen_get_default(),
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
        gtk_widget_destroy(GTK_WIDGET(l->data));
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

