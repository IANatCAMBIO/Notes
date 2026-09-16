/* ===========================================================================
 * main.c — Notes application entry point
 *
 * Wires everything together: opens the SQLite database, creates the
 * shared OnApp context, and shows the library window when the
 * GtkApplication activates.  Editor windows are opened from the library
 * (see library_window.c / editor_window.c).
 * =========================================================================== */

#include <gtk/gtk.h>
#include <glib-unix.h>

#include "app.h"
#include "backup.h"
#include "cli.h"
#include "db.h"
#include "ipc.h"
#include "library_window.h"

#ifdef __APPLE__
/* ---------------------------------------------------------------------------
 * quartz_log_filter() — GLogFunc that drops three specific, benign
 * messages emitted on macOS and forwards everything else unchanged.
 *
 * 1. When GTK enumerates the clipboard's targets (the "TARGETS" atom — done
 * whenever the right-click/selection menus appear, on rich-text paste, and in
 * drag negotiation), the GDK Quartz backend converts each NSPasteboard type
 * to a GdkAtom via gdk_atom_intern(uti.preferredMIMEType.UTF8String)
 * (gdk/quartz/gdkselection-quartz.c).  Modern macOS pasteboards routinely
 * carry Apple-private types whose UTI has no MIME string, so UTF8String is
 * NULL and gdk_atom_intern trips its "atom_name != NULL" g_return_if_fail.
 * The check is non-fatal — gdk_atom_intern returns GDK_NONE and the
 * enumeration keeps going with the valid types — but it prints a Gdk-CRITICAL
 * on every affected paste/menu.  We cannot reach the upstream call site, so we
 * silence just this message and pass all other logs through untouched.
 *
 * 2. MacPorts' gtk3 carries patch-gtk-menu-crash.diff, which puts
 * `g_return_if_fail (*change_point != NULL)` at the top of
 * gtk_menu_tracker_remove_items() — but a change point at the END of a
 * tracked section is exactly what every append to a live menu model has,
 * with nothing to remove.  So GTK's own gtk_application_set_menubar()
 * (the quartz backend appends the menubar to its combined model) prints
 * one Gtk-CRITICAL per call and nothing is wrong.  Upstream GTK has no such
 * check; a real tracker fault would abort in the loop below it.
 *
 * 3. "poll(2) failed due to: Undefined error: 0" (GLib-WARNING) — no poll
 * failed.  GDK's macOS poll function (gdk/macos/gdkmacoseventsource.c,
 * poll_func — the same design as GTK3's gdkeventloop-quartz.c) hands the
 * fds to a select thread and blocks in -[NSApp nextEventMatchingMask:];
 * if Cocoa re-enters the GLib main loop from inside that call, the outer
 * call's fd array may be stale, so it deliberately skips the collect and
 * returns -1 WITHOUT setting errno — hence "error: 0" — and GLib's check
 * pass sees the changed fd set and simply re-runs the iteration.  Seen on
 * the code blocks' "copy" link (the clipboard write's round-trip to the
 * pasteboard server is the presumed re-entry; the copy itself lands).  A
 * real poll failure carries a real errno string, so only the errno-0
 * spelling is dropped.
 *
 * The first two message texts are GTK3's (GTK4 has no GdkAtom at all, and
 * its menu tracker is the same code under the same MacPorts patch); they
 * are kept as-is across the port since a filter that never matches costs
 * nothing, and the GTK4 texts, if any, are re-measured in the dev sandbox.
 *   domain  — log domain ("Gdk"/"Gtk"/"GLib" for the offending messages).
 *   level   — log level flags.
 *   message — the formatted log text.
 *   data    — unused.
 * ------------------------------------------------------------------------- */
static void
quartz_log_filter(const gchar   *domain,
                  GLogLevelFlags level,
                  const gchar   *message,
                  gpointer       data)
{
    (void)data;
    if (message != NULL &&
        strstr(message, "gdk_atom_intern") != NULL &&
        strstr(message, "atom_name != NULL") != NULL)
        return;                      /* benign macOS pasteboard artifact     */
    if (message != NULL &&
        strstr(message, "gtk_menu_tracker_remove_items") != NULL &&
        strstr(message, "*change_point != NULL") != NULL)
        return;                      /* MacPorts' misplaced tracker guard    */
    if (message != NULL &&
        strstr(message, "poll(2) failed due to: Undefined error: 0") != NULL)
        return;                      /* GDK-Quartz's stale-fd bail-out       */
    g_log_default_handler(domain, level, message, data);
}
#endif /* __APPLE__ */

/* ---------------------------------------------------------------------------
 * startup_integrity_check() — verify the database at launch and show a
 * warning dialog when the checks find (or cannot reach) the truth.
 *
 * It runs EVERY launch and has no off switch, deliberately: a health check
 * that can be switched off can only ever report silence that means "not
 * looked", which is the one answer it must never give.  The verdict is
 * recorded on the connection, so Settings → Database can say when it was
 * reached without checking again.
 *
 * Inputs:
 *   app — the application context, its database already open.
 *
 * Output:
 *   TRUE when both checks ran and both passed.
 * ------------------------------------------------------------------------- */
static gboolean
startup_integrity_check(OnApp *app)
{
    if (on_db_health_check(app->db))
        return TRUE;

    const OnDbHealth *h = on_db_health(app->db);
    on_app_notice(NULL, "Notes - Database Integrity Check",
                  "%s\n\n%s",
                  h != NULL && h->ran
                      ? "The database integrity check found issues:"
                      : "The database integrity check could not be "
                        "completed:",
                  h != NULL && h->detail != NULL ? h->detail
                                                 : "no detail reported");
    return FALSE;
}

/* Exit status main() returns when startup fails INSIDE the main loop —
 * the database open that runs after the Welcome dialog.  The synchronous
 * open in main() returns its own 1; this covers the deferred path.        */
static int startup_status = 0;

/* TRUE while the Welcome dialog chain is up, so a second activation
 * (a second launch while it shows) cannot start a second chain.           */
static gboolean first_run_pending = FALSE;

/* ---------------------------------------------------------------------------
 * startup_db_path() — where the database is expected: the configured
 * db_dir plus the fixed filename, or the per-user default.
 *   app — the application context (reads app->db_dir).
 * Returns a new string; g_free() it.
 * ------------------------------------------------------------------------- */
static gchar *
startup_db_path(OnApp *app)
{
    return app->db_dir != NULL
           ? g_build_filename(app->db_dir, ON_DB_FILENAME, NULL)
           : on_db_default_path();
}

/* ---------------------------------------------------------------------------
 * startup_open_db() — open (creating if necessary) the database at
 * startup_db_path() and run the one-time migrations on it.  There is
 * deliberately NO fallback to another location: silently opening a
 * different database once made a user's notes "disappear" (a startup
 * racing the previous instance's shutdown flush).  One configured
 * database, or a clear error on stderr.
 *   app — the application context; app->db is set on success.
 * Returns TRUE when the database is open.
 * ------------------------------------------------------------------------- */
static gboolean
startup_open_db(OnApp *app)
{
    gchar *db_path = startup_db_path(app);
    app->db = on_db_open(db_path);
    if (app->db == NULL) {
        g_printerr("notes: could not open the notes database at "
                   "%s\n(if another instance is still shutting down, "
                   "try again in a few seconds)\n", db_path);
        g_free(db_path);
        return FALSE;
    }
    g_free(db_path);
    on_app_actions_backfill(app->db);     /* one-time '!'-line index (gated) */
    on_app_action_uids_backfill(app->db); /* then give those rows stable ids */
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * startup_finish() — everything activation does once the database is
 * open: the integrity check, the library window, the backup timer and the
 * IPC server.  Reached directly when the database was already there, or
 * at the end of the Welcome dialog chain.
 *   app — the application context, its database open.
 * ------------------------------------------------------------------------- */
static void
startup_finish(OnApp *app)
{
    /* DB integrity check: run PRAGMA integrity_check + foreign_key_check.
     * Every launch, with no switch — see startup_integrity_check().        */
    gboolean db_ok = startup_integrity_check(app);

    on_library_window_create(app);

    if (db_ok)
        on_app_status(app, "DB at %s loaded, integrity check passed",
                      app->db->path);

    /* Arm the rotating backup timer (a no-op while backups are off).  It
     * carries the db path, so it must be re-armed after File → Open
     * Database… — which is what on_backup_auto_start is for.            */
    on_backup_auto_start(app, app->db->path);

    /* Listen for "quicknote"/"note open" from later CLI invocations, then
     * run any action a CLI already queued because no instance was running.  */
    on_ipc_server_start(app);
    on_ipc_run_pending(app);
}

static void startup_first_run(OnApp *app);

/* ---------------------------------------------------------------------------
 * first_run_proceed() — the Welcome chain's one exit: open the database
 * at its (possibly just chosen) location and carry on with activation, or
 * fail the launch.  Either way the hold on_activate() took is released:
 * the library window keeps the application alive from here, and without
 * one the main loop ends.
 *   app — the application context.
 * ------------------------------------------------------------------------- */
static void
first_run_proceed(OnApp *app)
{
    first_run_pending = FALSE;
    if (startup_open_db(app))
        startup_finish(app);
    else
        startup_status = 1;
    g_application_release(G_APPLICATION(app->gtk_app));
}

/* ---------------------------------------------------------------------------
 * first_run_picked() — OnPickFunc for the Welcome dialog's "Open a
 * notes.db File": remember the file's directory as db_dir (persisted to
 * the ini) and proceed.  A cancelled chooser returns to the choice dialog.
 *   path      — the chosen file (owned), or NULL when cancelled.
 *   user_data — the OnApp context.
 * ------------------------------------------------------------------------- */
static void
first_run_picked(gchar *path, gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */
    if (path == NULL) {
        startup_first_run(app);      /* cancelled — back to the choice      */
        return;
    }
    gchar *dir = g_path_get_dirname(path);
    g_free(path);
    on_app_config_set("db_dir", dir);
    g_free(app->db_dir);
    app->db_dir = dir;               /* ownership transferred               */
    first_run_proceed(app);
}

/* ---------------------------------------------------------------------------
 * first_run_chosen() — GAsyncReadyCallback for the Welcome dialog: button
 * 0 opens the file chooser, button 1 creates the database where it was
 * expected (on_db_open() creates it), and a dismissed dialog quits.
 *   source    — the GtkAlertDialog.
 *   result    — the async result.
 *   user_data — the OnApp context.
 * ------------------------------------------------------------------------- */
static void
first_run_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */
    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
    /* Dismissal (Escape, the close button) comes back as an error with no
     * button index, since the dialog has no cancel button.                 */
    gint button = gtk_alert_dialog_choose_finish(dialog, result, NULL);
    g_object_unref(dialog);

    if (button == 0) {
        /* The app's model is a directory + the fixed name notes.db (the
         * ini stores db_dir only), so only that name is openable.          */
        on_app_pick_path(NULL, "Notes - Open Database", ON_PICK_OPEN,
                         "_Open", "Notes Database (" ON_DB_FILENAME ")",
                         ON_DB_FILENAME, NULL, first_run_picked, app);
    } else if (button == 1) {
        first_run_proceed(app);
    } else {
        first_run_pending = FALSE;
        g_application_release(G_APPLICATION(app->gtk_app));
    }
}

/* ---------------------------------------------------------------------------
 * startup_first_run() — no notes.db exists at the expected location: ask
 * whether to open an existing file or create a new one there, instead of
 * silently creating an empty database (a user pointing at a shared folder
 * usually means to OPEN a file that is already there).  Asynchronous —
 * the answer arrives in first_run_chosen(), and the chain ends in
 * first_run_proceed() or in a quit.
 *   app — the application context (app->db_dir names the location).
 * ------------------------------------------------------------------------- */
static void
startup_first_run(OnApp *app)
{
    static const gchar *const BUTTONS[] = {
        "_Open a notes.db File", "Create a _New notes.db", NULL
    };
    gchar *expected = startup_db_path(app);  /* where the db was looked for */
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Notes - Welcome");
    gchar *detail = g_strdup_printf("No notes database was found at\n%s",
                                    expected);
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_set_buttons(dialog, BUTTONS);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    gtk_alert_dialog_choose(dialog, NULL, NULL, first_run_chosen, app);
    g_free(detail);
    g_free(expected);
}

/* ---------------------------------------------------------------------------
 * on_startup() — GtkApplication "startup" handler, run once after GTK has
 * a display: bind the keyboard shortcuts (their <Primary> modifier is
 * resolved through the display's keymap, so this cannot run earlier), and
 * do the one-time display setup — the icon theme and the touch aids.
 *   gtk_app   — the application.
 *   user_data — the OnApp context created in main().
 * ------------------------------------------------------------------------- */
static void
on_startup(GtkApplication *gtk_app, gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */
    on_app_install_accels(gtk_app);

    /* Text rendering, measured on a 2x display (GTK 4.22, macOS):
     * GTK's default rounds every glyph advance to a whole LOGICAL pixel
     * (gtk-hint-font-metrics), which puts letters up to a device pixel off
     * their true position — uneven text.  Off, the layout keeps fractional
     * advances.  (The GL renderer then clips the left column of some
     * glyphs — its glyph cache rounds ink extents at fractional positions;
     * see main() for why the cairo renderer is the default on macOS.)     */
    g_object_set(gtk_settings_get_default(),
                 "gtk-hint-font-metrics", FALSE, NULL);

    /* The icon theme serves every image the app draws by name:
     *  - icons/ itself, where the toolbar PNGs sit flat by basename — GTK
     *    picks up files at the top of a search-path directory as UNTHEMED
     *    icons, so "new-folder" finds icons/new-folder.png, loaded at the
     *    display's scale factor and cached (on_app_icon_image_sized);
     *  - icons/theme/hicolor/…: the SVG pan-*-symbolic arrows so tree
     *    expanders render crisply on HiDPI, and the app logo as
     *    512x512/apps/notes.png — the name the .deb installs and the
     *    default window icon below (GTK4 takes window icons by THEME
     *    NAME only, so the logo has to be reachable as one).             */
    GtkIconTheme *theme =
        gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_search_path(theme, app->icons_dir);
    gchar *theme_dir = g_build_filename(app->icons_dir, "theme", NULL);
    gtk_icon_theme_add_search_path(theme, theme_dir);
    g_free(theme_dir);
    gtk_window_set_default_icon_name("notes");

    /* The app-wide stylesheet (the classes every window shares), then hide
     * the touch aids (selection handles, magnifier) unless enabled.        */
    on_app_install_css();
    on_app_apply_touch_assist(app);
}

/* ---------------------------------------------------------------------------
 * on_activate() — GtkApplication "activate" handler: show the library
 * window, or just raise it if the app is activated a second time.  When
 * main() found no database at the expected location the Welcome dialog
 * runs first, and the library window follows from its continuation.
 *   gtk_app   — the application.
 *   user_data — the OnApp context created in main().
 * ------------------------------------------------------------------------- */
static void
on_activate(GtkApplication *gtk_app, gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */

    if (app->library_window != NULL) {
        gtk_window_present(GTK_WINDOW(app->library_window));
        return;
    }
    if (first_run_pending)
        return;                      /* the Welcome dialog is already up    */

    if (app->db == NULL) {
        /* No window exists yet, so nothing keeps the application alive
         * while the dialog is up: hold it until the chain ends.            */
        first_run_pending = TRUE;
        g_application_hold(G_APPLICATION(gtk_app));
        startup_first_run(app);
        return;
    }
    startup_finish(app);
}

/* ---------------------------------------------------------------------------
 * on_sigterm() — terminate gracefully on SIGTERM (pkill, logout, system
 * shutdown): destroying every window flushes editor autosaves and lets
 * the main loop end cleanly.
 * ------------------------------------------------------------------------- */
static gboolean
on_sigterm(gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */
    GList *windows =                 /* copy: destroying mutates the list   */
        g_list_copy(gtk_application_get_windows(app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_window_destroy(GTK_WINDOW(l->data));
    g_list_free(windows);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * main() — set up the application context, run the GTK main loop, and
 * tear everything down afterwards.
 *   argc/argv — standard program arguments (GTK consumes its own flags).
 * Returns the process exit status.
 * ------------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
#ifdef __APPLE__
    /* Silence three benign macOS-only messages — GDK-Quartz's clipboard
     * critical, MacPorts' menu-tracker one and GLib's errno-0 poll warning
     * (see quartz_log_filter).  Installed before GTK so it covers every
     * paste and the first menubar.                                         */
    g_log_set_handler("Gdk",
                      G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
    g_log_set_handler("Gtk",
                      G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
    g_log_set_handler("GLib",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
#endif

    /* The application config (notes.ini) lives next to the
     * binary; resolve its location before anything reads it.               */
    on_app_config_init(argv[0]);

    /* Headless automation: a recognized subcommand runs and exits
     * without ever creating windows (see cli.h for the command list).      */
    int cli_status = on_cli_run(argc, argv);
    if (cli_status >= 0)
        return cli_status;

    /* The shared context handed to every window.  A custom database
     * location (e.g. a shared folder) may be configured in the config
     * file; the database itself is opened below.                          */
    OnApp app = {
        .gtk_app              = NULL,
        .db                   = NULL,
        /* Map of note id -> open editor window.  Keys are heap-allocated
         * gint64s owned (and freed) by the table itself.                   */
        .editors              = g_hash_table_new_full(g_int64_hash,
                                                      g_int64_equal,
                                                      g_free, NULL),
        .library_window       = NULL,
        .notify_notes_changed = NULL,
        .icons_dir            = NULL,
        .db_dir               = on_app_config_load_db_dir(),
    };
    on_app_init_icons_dir(&app, argv[0]);

    /* Boolean preferences (second argument = default when unset).          */
    app.code_copy_buttons =
        on_app_config_get_bool("code_copy_button",       TRUE);
    app.code_line_numbers =
        on_app_config_get_bool("code_line_numbers",      FALSE);
    app.sidebar_counts =
        on_app_config_get_bool("sidebar_counts",         FALSE);
    app.sidebar_fit_content =
        on_app_config_get_bool("sidebar_fit_content",    TRUE);
    app.first_line_title =
        on_app_config_get_bool("first_line_title",       TRUE);
    app.compact_editor_toolbar =
        on_app_config_get_bool("compact_editor_toolbar", TRUE);
    app.comfortable_list =
        on_app_config_get_bool("list_density_comfortable", FALSE);
    app.statusbar_db_path =
        on_app_config_get_bool("statusbar_db_path",      FALSE);
    app.statusbar_note_id =
        on_app_config_get_bool("statusbar_note_id",      FALSE);
    app.show_done_actions =
        on_app_config_get_bool("show_done_actions",      TRUE);
    app.bold_list_titles =
        on_app_config_get_bool("bold_list_titles",       TRUE);
    app.ai_enabled =
        on_app_config_get_bool("ai_enabled",             FALSE);
    app.ai_command       = on_app_config_get("ai_command");
    app.ai_custom_prompt = on_app_config_get("ai_custom_prompt");

    /* Open the notes database first — without it there is nothing to
     * show.  First launch (or an emptied configured directory) is the
     * exception: no database at the expected location.  Then the user is
     * asked before one is created — they may mean to open an existing
     * file elsewhere — and since that dialog needs the main loop, the
     * open waits for on_activate() (app.db stays NULL until then).         */
    gchar *expected = startup_db_path(&app);  /* where the db is looked for */
    gboolean db_present = g_file_test(expected, G_FILE_TEST_EXISTS);
    g_free(expected);
    if (db_present && !startup_open_db(&app)) {
        g_hash_table_destroy(app.editors);
        g_free(app.icons_dir);
        g_free(app.db_dir);
        return 1;
    }

#ifdef __APPLE__
    /* The cairo renderer, unless GSK_RENDERER says otherwise.  Compared
     * side by side on a Retina display (GTK 4.22): the GL renderer's glyph
     * cache clips the left column of glyphs drawn at fractional positions
     * (a "G" loses its leftmost pixel once gtk-hint-font-metrics is off),
     * and its subpixel-antialiased path avoids that only by drawing
     * heavier text.  Cairo draws glyphs straight through Pango — no cache,
     * fractional positions, the same grayscale rasterizer — and looked
     * best.  This app's costs are PNG decode and SQLite, not compositing,
     * so the GPU renderer buys it nothing measurable.  Must be set before
     * GTK creates its first renderer, i.e. before g_application_run.      */
    g_setenv("GSK_RENDERER", "cairo", FALSE);
#endif
    app.gtk_app = gtk_application_new("org.example.notes",
                                      G_APPLICATION_DEFAULT_FLAGS);
    /* register-session: Dock → Quit and logout then route through
     * "app.quit", which destroys every window so editor autosaves flush.
     * Without it AppKit's default terminate exits the process at once.    */
    g_object_set(app.gtk_app, "register-session", TRUE, NULL);
    g_signal_connect(app.gtk_app, "startup",
                     G_CALLBACK(on_startup), &app);
    g_signal_connect(app.gtk_app, "activate",
                     G_CALLBACK(on_activate), &app);
    g_unix_signal_add(SIGTERM, on_sigterm, &app);

    /* A "quicknote"/"note open" that found no running instance falls
     * through to here to start the GUI.  Those leftover words are not GTK
     * options — run with a clean argv so GApplication doesn't try to open
     * them as files ("This application can not open files").                */
    int status;                      /* main-loop exit status               */
    if (on_ipc_has_pending()) {
        char *solo_argv[] = { argv[0], NULL };
        status = g_application_run(G_APPLICATION(app.gtk_app), 1, solo_argv);
    } else {
        status = g_application_run(G_APPLICATION(app.gtk_app), argc, argv);
    }

    /* Stop serving CLI commands and unlink the socket before teardown.      */
    on_ipc_server_stop();

    /* Windows (and their final autosaves) are done by the time run()
     * returns, so the database can be closed and hashed safely now.        */
    g_object_unref(app.gtk_app);
    g_hash_table_destroy(app.editors);

    on_db_close(app.db);

    g_free(app.icons_dir);
    g_free(app.db_dir);
    return startup_status != 0 ? startup_status : status;
}
