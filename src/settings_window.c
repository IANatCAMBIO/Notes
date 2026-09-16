/* ===========================================================================
 * settings_window.c — the application settings window (implementation)
 *
 * See settings_window.h for the overview.  Every control writes through
 * to the OnApp setters, which persist to the notes.ini config file
 * and live-update all open windows, so there is no OK/Apply button.
 * =========================================================================== */

#include "settings_window.h"
#include "editor_window.h"
#include "library_window.h"

#include "app.h"
#include "backup.h"
#include "db.h"

#include <glib/gstdio.h>            /* g_stat, GStatBuf        */
#include <stddef.h>
#include <stdlib.h>             /* atoi                    */
#include <string.h>

/* ---------------------------------------------------------------------------
 * svg_loader_available() — TRUE if gdk-pixbuf can decode SVG files (i.e.
 * the librsvg loader is installed).  Used to warn about the elementary
 * theme, which is SVG-only.
 * ------------------------------------------------------------------------- */
static gboolean
svg_loader_available(void)
{
    GSList *formats = gdk_pixbuf_get_formats();
    gboolean found = FALSE;          /* did we see an svg decoder?          */
    for (GSList *l = formats; l != NULL && !found; l = l->next) {
        gchar *name = gdk_pixbuf_format_get_name(l->data);
        found = g_ascii_strcasecmp(name, "svg") == 0;
        g_free(name);
    }
    g_slist_free(formats);
    return found;
}

/* ---------------------------------------------------------------------------
 * on_density_combo_changed() — List Density combo changed: update the field,
 * persist, and trigger a full notes-list refresh so row heights update.
 * ------------------------------------------------------------------------- */
static void
on_density_combo_changed(GtkDropDown *combo, GParamSpec *pspec,
                         gpointer user_data)
{
    (void)pspec;
    OnApp *app = user_data;            /* application context                 */
    app->comfortable_list = (gtk_drop_down_get_selected(combo) == 1);
    on_app_config_set("list_density_comfortable",
                      app->comfortable_list ? "1" : "0");
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
}

/* apply_notes_changed() — fire the library's full refresh, if installed.    */
static void
apply_notes_changed(OnApp *app)
{
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
}

/* apply_statusbar_db_path() — re-render every window's status-bar path.     */
static void
apply_statusbar_db_path(OnApp *app)
{
    apply_notes_changed(app);        /* the library's path label            */
    on_editor_status_refresh_all(app);
}

/* ---------------------------------------------------------------------------
 * BOOL_SETTINGS — the simple on/off preferences: one checkbox each, one
 * OnApp gboolean field each, persisted under `key`, with an optional
 * live-apply hook.  bool_check_new() builds the checkbox; on_bool_toggled
 * (shared by all of them) reads the spec back off the widget.  Settings
 * with extra behavior (touch assist's inverted sense, the native macOS
 * menubar) stay hand-written below.
 * ------------------------------------------------------------------------- */
typedef enum {
    BS_SIDEBAR_COUNTS,               /* Appearance                          */
    BS_SIDEBAR_FIT,
    BS_BOLD_LIST_TITLES,
    BS_SHOW_DONE_ACTIONS,
    BS_CODE_COPY,                    /* Editor                              */
    BS_CODE_LINES,
    BS_FIRST_LINE_TITLE,
    BS_COMPACT_TOOLBAR,
    BS_STATUSBAR_NOTE_ID,
    BS_STATUSBAR_DB_PATH,            /* Appearance                          */
} BoolSettingId;

typedef struct {
    const gchar *label;              /* checkbox text                       */
    const gchar *key;                /* ini key                             */
    gsize        field_off;          /* offsetof(OnApp, <field>)            */
    void       (*apply)(OnApp *app); /* live-apply hook, or NULL            */
} BoolSetting;

static const BoolSetting BOOL_SETTINGS[] = {
    [BS_SIDEBAR_COUNTS] = {
        "Show note counts next to folders and tags",
        "sidebar_counts", offsetof(OnApp, sidebar_counts),
        apply_notes_changed },
    [BS_SIDEBAR_FIT] = {
        "Fit the sidebar width to the folders on show",
        "sidebar_fit_content", offsetof(OnApp, sidebar_fit_content),
        on_library_sidebar_fit },
    [BS_BOLD_LIST_TITLES] = {
        "Bold titles in comfortable list density",
        "bold_list_titles", offsetof(OnApp, bold_list_titles),
        apply_notes_changed },
    [BS_SHOW_DONE_ACTIONS] = {
        "Show completed action items",
        "show_done_actions", offsetof(OnApp, show_done_actions),
        apply_notes_changed },
    [BS_CODE_COPY] = {
        "Show copy button on code blocks",
        "code_copy_button", offsetof(OnApp, code_copy_buttons),
        on_editor_rebuild_code_buttons_all },
    [BS_CODE_LINES] = {
        "Show line numbers in code blocks",
        "code_line_numbers", offsetof(OnApp, code_line_numbers),
        on_editor_apply_line_numbers_all },
    [BS_FIRST_LINE_TITLE] = {
        "First line is Title formatted",
        "first_line_title", offsetof(OnApp, first_line_title),
        on_editor_title_refresh_all },
    [BS_COMPACT_TOOLBAR] = {
        "Compact toolbar (group paragraph styles and lists into menus)",
        "compact_editor_toolbar", offsetof(OnApp, compact_editor_toolbar),
        on_editor_rebuild_toolbars_all },
    [BS_STATUSBAR_NOTE_ID] = {
        "Show note id in the editor status bar",
        "statusbar_note_id", offsetof(OnApp, statusbar_note_id),
        on_editor_status_refresh_all },
    [BS_STATUSBAR_DB_PATH] = {
        "Show database path prefix in status bar",
        "statusbar_db_path", offsetof(OnApp, statusbar_db_path),
        apply_statusbar_db_path },
};

/* on_bool_toggled() — shared handler: flip the field, persist, apply.       */
static void
on_bool_toggled(GtkCheckButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    const BoolSetting *bs = g_object_get_data(G_OBJECT(check), "on-spec");
    gboolean *field = (gboolean *)((gchar *)app + bs->field_off);
    *field = gtk_check_button_get_active(check);
    on_app_config_set(bs->key, *field ? "1" : "0");
    if (bs->apply != NULL)
        bs->apply(app);
}

/* bool_check_new() — build the checkbox for one BOOL_SETTINGS entry.        */
static GtkWidget *
bool_check_new(OnApp *app, BoolSettingId id)
{
    const BoolSetting *bs = &BOOL_SETTINGS[id];
    GtkWidget *check = gtk_check_button_new_with_label(bs->label);
    gtk_widget_set_margin_start(check, 12);
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(check),
        *(gboolean *)((gchar *)app + bs->field_off));
    g_object_set_data(G_OBJECT(check), "on-spec", (gpointer)bs);
    g_signal_connect(check, "toggled", G_CALLBACK(on_bool_toggled), app);
    return check;
}

#ifdef __APPLE__
/* on_native_menubar_toggled() — move the library menu into (or out of)
 * the native macOS menu bar, live.                                          */
static void
on_native_menubar_toggled(GtkCheckButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    gboolean native = gtk_check_button_get_active(check);
    on_app_config_set("native_menubar", native ? "1" : "0");
    on_library_apply_native_menubar(app, native);
}
#endif /* __APPLE__ */

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the "Database" settings block, kept alive so the
 * handlers can update them after a check, a backup or a folder change.
 *
 * Fields:
 *   app          — application context.
 *   path_label   — the active database file path.
 *   data_label   — "1296 notes in 32 folders".
 *   size_label   — the file's size on disk.
 *   led_label    — the round health indicator.
 *   health_label — the verdict and when it was reached.
 *   sha_btn      — the digest; clicking copies the whole of it.
 *   update_btn   — renews every line on the plate.
 *   bk_*         — the rotating-backup controls (see backup.h).
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp     *app;
    /* The health block: five facts about the file, all refreshed together
     * because they are all answers about the SAME database.               */
    GtkWidget *path_label;
    GtkWidget *data_label;
    GtkWidget *size_label;
    GtkWidget *led_label;
    GtkWidget *health_label;
    GtkWidget *sha_btn;
    GtkWidget *update_btn;
    /* Rotating backups (backup.h) — off by default.                       */
    GtkWidget *bk_check;             /* master switch                       */
    GtkWidget *bk_choose_btn;        /* destination folder chooser          */
    GtkWidget *bk_path_label;        /* where the copies land               */
    GtkWidget *bk_interval_spin;     /* minutes; 0 = manual only            */
    GtkWidget *bk_keep_spin;         /* how many to retain                  */
    GtkWidget *bk_now_btn;           /* "Back Up Now"                       */
} DbSection;

/* ---------------------------------------------------------------------------
 * dir_shares_fate() — TRUE when `dir` is `other`, or sits INSIDE it.
 *
 * The test behind the backup destination's warning, and it is a containment
 * test rather than a comparison for a reason the default makes plain:
 * backups land in a `backups/` folder INSIDE the database's own directory,
 * so string equality would answer "different folder" for the one
 * arrangement the warning most needs to describe.  A subfolder shares its
 * parent's fate exactly — a whole directory going to the trash takes its
 * children at the same moment.
 *
 * Canonicalised first so "/a/b" and "/a/./b" are one answer, and the
 * separator is required after the prefix so "/a/backups-old" is not read as
 * living inside "/a/backups".  Symlinks are NOT resolved: that needs the
 * paths to exist and would make the label's answer depend on whether a
 * removable disk happens to be plugged in.
 *
 * Inputs:
 *   dir, other — two directory paths; either may be NULL.
 *
 * Output:
 *   TRUE when `dir` shares `other`'s fate; FALSE otherwise, and whenever
 *   either path is NULL.
 * ------------------------------------------------------------------------- */
static gboolean
dir_shares_fate(const gchar *dir, const gchar *other)
{
    if (dir == NULL || other == NULL)
        return FALSE;
    gchar *a = g_canonicalize_filename(dir, NULL);
    gchar *b = g_canonicalize_filename(other, NULL);
    gboolean same = (g_strcmp0(a, b) == 0);
    if (!same) {
        gchar *pre = g_strconcat(b, G_DIR_SEPARATOR_S, NULL);
        same = g_str_has_prefix(a, pre);
        g_free(pre);
    }
    g_free(a);
    g_free(b);
    return same;
}

/* ---------------------------------------------------------------------------
 * bk_section_refresh() — mirror the backup settings into the widgets and
 * grey out everything the master switch does not apply to.
 *
 * The destination label is ONE SENTENCE, "Backing up to <folder>", and it
 * names the RESOLVED folder in every case — there is no "(default)" or "not
 * chosen yet" variant, because there is no state in which backups go
 * nowhere: on_backup_dir falls back to a folder beside the default
 * database, so the honest summary is simply where they land.
 * ------------------------------------------------------------------------- */
static void
bk_section_refresh(DbSection *s)
{
    gboolean on = on_app_config_get_bool("backup_enabled", FALSE);
    gtk_widget_set_sensitive(s->bk_choose_btn,    on);
    gtk_widget_set_sensitive(s->bk_interval_spin, on);
    gtk_widget_set_sensitive(s->bk_keep_spin,     on);
    gtk_widget_set_sensitive(s->bk_now_btn,       on);

    /* Always the RESOLVED destination, from the same call the worker uses,
     * so the label cannot promise a folder the backups do not go to.      */
    gchar *dir    = on_backup_dir();
    gchar *db_dir = g_path_get_dirname(s->app->db->path);
    gchar *markup;                   /* the label's one sentence            */
    if (dir_shares_fate(dir, db_dir))
        /* The one case worth a second line.  Backups here are still real
         * backups — a separate file, snapshotted and verified — but they
         * cannot survive losing the folder, and this is the DEFAULT, so
         * saying nothing would let the arrangement most in need of changing
         * look like the one nobody need think about.                      */
        markup = g_markup_printf_escaped(
            "<small>Backing up to %s\n<i>\xe2\x9a\xa0 alongside the database "
            "\xe2\x80\x94 change it to survive losing that folder</i></small>",
            dir);
    else
        markup = g_markup_printf_escaped(
            "<small>Backing up to %s</small>", dir);
    gtk_label_set_markup(GTK_LABEL(s->bk_path_label), markup);
    g_free(markup);
    g_free(db_dir);
    g_free(dir);
}

/* ---------------------------------------------------------------------------
 * The health block's three states, said in one place.
 *
 * The LED is GREEN only for a check that ran and passed.  There is no green
 * for "not checked yet": an indicator that goes green before anything has
 * looked is exactly the "checked, all good, when nothing was checked"
 * answer the startup check exists to prevent.
 * ------------------------------------------------------------------------- */
#define LED_OK      "\xf0\x9f\x9f\xa2"   /* green circle                    */
#define LED_BAD     "\xf0\x9f\x94\xb4"   /* red circle                      */
#define LED_UNKNOWN "\xe2\x9a\xaa"       /* white circle: nothing has run   */

/* SHA_HEAD/SHA_TAIL — how much of the 64-char digest is shown.  The whole
 * thing is on the tooltip and on the clipboard; this is the part that fits
 * beside its label without wrapping the row.                               */
#define SHA_HEAD 8
#define SHA_TAIL 4

/* ---------------------------------------------------------------------------
 * health_stamp() — "13:24 today", "Sep 8 at 09:02": the moment a check was
 * made, in as few words as carry it.
 *
 * Inputs:
 *   when — unix time of the check.
 *
 * Output:
 *   a new string (g_free); never NULL.
 * ------------------------------------------------------------------------- */
static gchar *
health_stamp(gint64 when)
{
    GDateTime *dt = g_date_time_new_from_unix_local(when);
    if (dt == NULL)
        return g_strdup("an unknown time");

    GDateTime *now = g_date_time_new_now_local();
    gboolean today = now != NULL &&
        g_date_time_get_year(dt) == g_date_time_get_year(now) &&
        g_date_time_get_day_of_year(dt) == g_date_time_get_day_of_year(now);
    /* %H:%M, not %l — GLib pads the 12-hour form with a FIGURE SPACE that
     * g_strstrip does not remove.                                         */
    gchar *out = today ? g_date_time_format(dt, "%H:%M today")
                       : g_date_time_format(dt, "%b %-d at %H:%M");
    if (now != NULL)
        g_date_time_unref(now);
    g_date_time_unref(dt);
    return out != NULL ? out : g_strdup("an unknown time");
}

/* ---------------------------------------------------------------------------
 * db_health_refresh() — paint the LED, the verdict and its tooltip from
 * whatever the last recorded check found.
 *
 * Reads the STAMP rather than checking anything: a health block that ran a
 * PRAGMA pass every time the window was drawn would be a health check
 * nobody asked for.  The startup pass and the Update button write it.
 * ------------------------------------------------------------------------- */
static void
db_health_refresh(DbSection *s)
{
    const OnDbHealth *h = on_db_health(s->app->db);

    const gchar *led;                /* which circle                        */
    gchar       *text;               /* the verdict beside it               */
    if (h == NULL) {
        led  = LED_UNKNOWN;
        text = g_strdup("Not checked");
    } else if (h->pending) {
        /* The launch's pass is still walking the file (it runs behind the
         * window — see on_app_db_health_start).  White, like "not
         * checked": nothing has finished looking yet.                    */
        led  = LED_UNKNOWN;
        text = g_strdup("Checking\xe2\x80\xa6");
    } else {
        gchar *when = health_stamp(h->when);
        if (h->ok) {
            led  = LED_OK;
            text = g_strdup_printf("Healthy \xe2\x80\x94 checked %s", when);
        } else if (h->ran) {
            led  = LED_BAD;
            text = g_strdup_printf("Problems found \xe2\x80\x94 %s", when);
        } else {
            /* The checks could not be RUN.  A different sentence from
             * "problems found", and the one worth exposing: a locked or
             * unreadable file reading as merely unhealthy sends someone
             * looking for corruption that may not be there.               */
            led  = LED_BAD;
            text = g_strdup_printf("Check did not complete \xe2\x80\x94 %s",
                                   when);
        }
        g_free(when);
    }
    gtk_label_set_text(GTK_LABEL(s->led_label), led);

    gchar *markup = g_markup_printf_escaped("<small>%s</small>", text);
    gtk_label_set_markup(GTK_LABEL(s->health_label), markup);
    g_free(markup);
    /* The detail is sqlite's own words and can run to many lines, so it
     * lives on the tooltip: the row says WHAT, hovering says which.       */
    on_app_set_tooltip(s->health_label,
        h != NULL && h->pending ? "PRAGMA integrity_check is running on a "
                                  "worker thread; the verdict lands here."
      : h != NULL && h->detail != NULL ? h->detail
      : h != NULL ? "PRAGMA integrity_check and PRAGMA foreign_key_check "
                    "both passed against this file."
                  : "Nothing has verified this database yet.  Press Update.");
    g_free(text);
}

/* ---------------------------------------------------------------------------
 * db_sha_refresh() — hash the database file and show the digest.
 *
 * Hashed HERE rather than read off the health stamp, because a digest
 * stored inside the file it describes is wrong the moment it is stored —
 * writing it changes the file.  So this is the fingerprint of the file as
 * it stands, which is the only form of it a user can check against
 * `shasum -a 256` or against a backup.
 * ------------------------------------------------------------------------- */
static void
db_sha_refresh(DbSection *s)
{
    GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(s->sha_btn));
    gchar     *sha = on_db_file_sha256(s->app->db->path);

    if (sha != NULL && strlen(sha) > SHA_HEAD + SHA_TAIL) {
        gchar *shown = g_strdup_printf(
            "%.*s\xe2\x80\xa6%s", SHA_HEAD, sha,
            sha + strlen(sha) - SHA_TAIL);
        gchar *m = g_markup_printf_escaped("<small>%s</small>", shown);
        gtk_label_set_markup(GTK_LABEL(lbl), m);
        g_free(m);
        g_free(shown);
        gchar *tip = g_strdup_printf(
            "%s\n\nThe file as it stands.  A database in use changes with "
            "the next edit, so this moves.\n\nClick to copy.", sha);
        on_app_set_tooltip(s->sha_btn, tip);
        g_free(tip);
        gtk_widget_set_sensitive(s->sha_btn, TRUE);
        /* The full digest rides the button, so the click that copies it
         * does not go back to the disk for a file that has moved on.      */
        g_object_set_data_full(G_OBJECT(s->sha_btn), "on-sha", sha, g_free);
    } else {
        gtk_label_set_markup(GTK_LABEL(lbl), "<small>\xe2\x80\x94</small>");
        on_app_set_tooltip(s->sha_btn,
                                    "The database file could not be read.");
        gtk_widget_set_sensitive(s->sha_btn, FALSE);
        g_object_set_data(G_OBJECT(s->sha_btn), "on-sha", NULL);
        g_free(sha);
    }
}

/* db_section_refresh() — sync every line of the health plate with reality. */
static void
db_section_refresh(DbSection *s)
{
    gchar *markup = g_markup_printf_escaped("<small>%s</small>",
                                            s->app->db->path);
    gtk_label_set_markup(GTK_LABEL(s->path_label), markup);
    g_free(markup);

    gint n_notes, n_folders;         /* totals across the database          */
    on_db_totals(s->app->db, &n_notes, &n_folders, NULL);
    gchar *data = g_strdup_printf(
        "<small>%d note%s in %d folder%s</small>",
        n_notes,   n_notes   == 1 ? "" : "s",
        n_folders, n_folders == 1 ? "" : "s");
    gtk_label_set_markup(GTK_LABEL(s->data_label), data);
    g_free(data);

    /* g_stat, not the page count: what the user is being told is how much
     * room the file takes, which includes free pages a VACUUM would give
     * back.                                                               */
    GStatBuf st;                     /* for the database file size          */
    gchar *size = (g_stat(s->app->db->path, &st) == 0)
                  ? g_format_size((guint64)st.st_size)
                  : g_strdup("unknown");
    gchar *size_m = g_markup_printf_escaped("<small>%s</small>", size);
    gtk_label_set_markup(GTK_LABEL(s->size_label), size_m);
    g_free(size_m);
    g_free(size);

    db_health_refresh(s);
    db_sha_refresh(s);
}

/* settings_notify_db_health() — app->notify_db_health while the window is
 * open: the async pass changed state, repaint the plate.                 */
static void
settings_notify_db_health(OnApp *app)
{
    if (app->settings_db_section != NULL)
        db_section_refresh(app->settings_db_section);
}

/* on_settings_destroy() — the window is going: unhook the plate.          */
static void
on_settings_destroy(GtkWidget *window, gpointer user_data)
{
    (void)window;
    OnApp *app = user_data;
    app->notify_db_health    = NULL;
    app->settings_db_section = NULL;
}

/* ---------------------------------------------------------------------------
 * on_db_update_clicked() — "Update": renew every line on the plate.
 *
 * Runs a health pass and then re-reads the WHOLE block, not just the
 * verdict: the plate is one statement about one file, and a button under it
 * that refreshed two of its five lines would leave the other three saying
 * whatever they said when the window opened.  The counts and the size go
 * stale on their own — anything done in the library since moves both — so
 * they are exactly what someone presses this for.
 *
 * Reports in the status bar as well, because a check that comes back clean
 * changes nothing visible and would otherwise look like a button that does
 * nothing.
 * ------------------------------------------------------------------------- */
static void
on_db_update_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;        /* the database section                */
    gboolean ok = on_db_health_check(s->app->db);
    db_section_refresh(s);
    const OnDbHealth *h = on_db_health(s->app->db);
    on_app_status(s->app, "%s", ok
        ? "Database check passed"
        : h != NULL && h->ran ? "Database check found problems"
                              : "Database check did not complete");
}

/* on_db_sha_clicked() — put the full digest on the clipboard.              */
static void
on_db_sha_clicked(GtkButton *btn, gpointer user_data)
{
    DbSection *s = user_data;        /* the database section                */
    const gchar *sha = g_object_get_data(G_OBJECT(btn), "on-sha");
    if (sha == NULL)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(btn)), sha);
    on_app_status(s->app, "SHA-256 copied to the clipboard");
}

/* on_bk_toggled() — the backup master switch: persist and re-arm the timer.
 * No folder prompt: on_backup_dir falls back to a usable folder, so
 * switching this on always does something.  Choosing a folder is an
 * improvement, not a prerequisite.                                         */
static void
on_bk_toggled(GtkCheckButton *check, gpointer user_data)
{
    DbSection *s = user_data;        /* the database section                */
    on_app_config_set("backup_enabled",
                      gtk_check_button_get_active(check) ? "1" : "0");
    on_backup_auto_start(s->app, s->app->db->path);
    bk_section_refresh(s);
}

/* ---------------------------------------------------------------------------
 * on_bk_folder_picked() — on_app_pick_path()'s continuation for the backup
 * destination: persist the new folder, re-arm the timer against it and
 * re-render the block.
 *
 * Inputs:
 *   dir       — the chosen folder (owned here, freed), or NULL if the
 *               chooser was cancelled — nothing changes then.
 *   user_data — the DbSection.  The chooser is modal over the settings
 *               window, which is what keeps the section alive until this
 *               runs.
 *
 * Output:
 *   none.
 * ------------------------------------------------------------------------- */
static void
on_bk_folder_picked(gchar *dir, gpointer user_data)
{
    DbSection *s = user_data;        /* the database section                */
    if (dir == NULL)
        return;
    on_app_config_set("backup_dir", dir);
    g_free(dir);
    on_backup_auto_start(s->app, s->app->db->path);
    bk_section_refresh(s);
}

/* on_bk_choose_clicked() — re-pick the destination folder.  Asynchronous:
 * on_bk_folder_picked does the rest once the chooser closes.               */
static void
on_bk_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;        /* the database section                */
    /* Start where backups go NOW: re-picking from wherever the chooser
     * last was is how backups end up in two places.                      */
    gchar *current = on_backup_dir();
    on_app_pick_path(GTK_WINDOW(gtk_widget_get_root(s->bk_check)),
                     "Choose Backup Folder", ON_PICK_FOLDER, "_Select",
                     NULL, NULL, current, on_bk_folder_picked, s);
    g_free(current);
}

/* on_bk_interval_changed() — persist the cadence and re-arm the timer.     */
static void
on_bk_interval_changed(GtkSpinButton *spin, gpointer user_data)
{
    DbSection *s = user_data;        /* the database section                */
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(spin));
    on_app_config_set("backup_interval_min", v);
    g_free(v);
    on_backup_auto_start(s->app, s->app->db->path);
}

/* on_bk_keep_changed() — persist the retention bound.  No re-arm: the
 * cadence has not moved, and the bound is read at the start of each pass.  */
static void
on_bk_keep_changed(GtkSpinButton *spin, gpointer user_data)
{
    (void)user_data;
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(spin));
    on_app_config_set("backup_keep", v);
    g_free(v);
}

/* on_bk_now_clicked() — "Back Up Now": one pass, reported in the status
 * bar.  Also the only way to exercise the feature without waiting for a
 * timer, which is why it is worth a button.                                */
static void
on_bk_now_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;        /* the database section                */
    on_backup_start(s->app, s->app->db->path);
}

/* on_viewer_entry_changed() — persist the image-viewer program path as
 * the user types; an empty entry clears the setting so "Open" on an
 * image falls back to the system default viewer.                            */
static void
on_viewer_entry_changed(GtkEditable *editable, gpointer user_data)
{
    (void)user_data;
    const gchar *text = gtk_editable_get_text(editable);
    if (text != NULL && *text != '\0')
        on_app_config_set("image_viewer", text);
    else
        on_app_config_set("image_viewer", NULL);
}

/* on_viewer_picked() — on_app_pick_path()'s continuation for the viewer
 * program: put the path in the entry, whose "changed" handler does the
 * persisting.  `file` is owned here (freed); NULL = cancelled, no change.  */
static void
on_viewer_picked(gchar *file, gpointer user_data)
{
    GtkWidget *entry = user_data;    /* the viewer-path entry               */
    if (file == NULL)
        return;
    gtk_editable_set_text(GTK_EDITABLE(entry), file);
    g_free(file);
}

/* on_viewer_browse_clicked() — pick the viewer program with a file
 * chooser.  Asynchronous: on_viewer_picked finishes the job.               */
static void
on_viewer_browse_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *entry = user_data;    /* the viewer-path entry               */
    on_app_pick_path(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))),
                     "Choose Image Viewer", ON_PICK_OPEN, "_Select",
                     NULL, NULL, NULL, on_viewer_picked, entry);
}

/* on_touch_assist_toggled() — the checkbox DISABLES the touch aids, so
 * active = touch_assist off.  Applies live: it is CSS over the handles
 * and the magnifier (on_app_apply_touch_assist).  The GTK3 build also
 * suppressed the tap cut/copy/paste bubble through an env var read at
 * start-up; GTK4 has no such lever, so nothing here needs a restart.       */
static void
on_touch_assist_toggled(GtkCheckButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    on_app_config_set("touch_assist",
                      gtk_check_button_get_active(check) ? "0" : "1");
    on_app_apply_touch_assist(app);
}

/* ---------------------------------------------------------------------------
 * small_button() — a text button at about half the theme's default bulk,
 * and the one spelling of a PUSH BUTTON in the Database section, so its
 * three buttons cannot come to be three sizes.  (The SHA-256 row's is not
 * one of them and should not become one — it is a relief-less label that
 * happens to be clickable, and it strips its box entirely to keep the
 * digest on the grid's value column.)
 *
 * These buttons sit UNDER the lines they act on rather than beside them, so
 * a full-size button would read as the loudest thing in a block whose point
 * is the text above it.  Shrunk by padding and font size rather than by a
 * shorter label: the words are what say what the button does.
 *
 * min-height/min-width are named (settings_install_css) because the theme
 * floors both, so trimming the padding alone moves nothing.
 * ------------------------------------------------------------------------- */
static GtkWidget *
small_button(const gchar *label)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "notes-small-button");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    return btn;
}

/* ---------------------------------------------------------------------------
 * small_spin() — a spin button no wider than the digits it can hold.
 *   lo, hi, step — the range and its increment.
 *   chars        — digits to size the entry for.
 *
 * A default GtkSpinButton is enormous for a three-digit number, and the
 * lever is NOT the obvious one: width_chars alone moves almost
 * nothing, because the theme floors the spin button's size and pads it
 * 8 px a side, and a floor beats a request.  Both levers are kept because
 * they do different jobs: the CSS (settings_install_css) removes the
 * floor, and `chars` is what then decides the width — sized to the RANGE,
 * so the widest value a user can reach still fits without the entry
 * scrolling under them.
 * ------------------------------------------------------------------------- */
static GtkWidget *
small_spin(gdouble lo, gdouble hi, gdouble step, gint chars)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_editable_set_width_chars(GTK_EDITABLE(spin), chars);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(spin), chars);
    gtk_widget_add_css_class(spin, "notes-small-spin");
    return spin;
}

/* ---------------------------------------------------------------------------
 * info_row_attach() / info_row() — one "Label:  value" line of the Database
 * section's health block.
 *
 * The label column is what makes the block read as one statement about one
 * file: attaching to a grid puts every value at the same x for free, where
 * five separately packed lines would each start wherever their own words
 * ended.
 *
 * info_row_attach takes a WIDGET, for the two rows whose value is more than
 * text; info_row is the plain case and hands back the label to fill in
 * later.  Both are <small> — the size the path line has always been.
 * ------------------------------------------------------------------------- */
static void
info_row_attach(GtkWidget *grid, gint row, const gchar *name,
                GtkWidget *value)
{
    GtkWidget *key = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<small>%s</small>", name);
    gtk_label_set_markup(GTK_LABEL(key), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(key), 0.0);
    gtk_widget_set_valign(key, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
    gtk_widget_set_halign(value, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static GtkWidget *
info_row(GtkWidget *grid, gint row, const gchar *name)
{
    GtkWidget *value = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(value), 0.0);
    /* The path is the long one and the reason for both calls; on a short
     * value they cost nothing.                                            */
    gtk_label_set_wrap(GTK_LABEL(value), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(value), 44);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    /* Selectable labels come up with the whole text selected, which reads
     * as five highlighted rows the moment the window opens.               */
    gtk_label_select_region(GTK_LABEL(value), 0, 0);
    info_row_attach(grid, row, name, value);
    return value;
}

/* ---------------------------------------------------------------------------
 * section_rule() — the horizontal rule between two settings sections, with
 * the 4 px of breathing room above and below that the box's own spacing
 * does not give it.
 *
 * Output:
 *   a new GtkSeparator, unparented.
 * ------------------------------------------------------------------------- */
static GtkWidget *
section_rule(void)
{
    GtkWidget *rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(rule, 4);
    gtk_widget_set_margin_bottom(rule, 4);
    return rule;
}

/* ---------------------------------------------------------------------------
 * section_label() — bold section header for the settings layout.
 * ------------------------------------------------------------------------- */
static GtkWidget *
section_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

/* on_ai_enabled_toggled() — master AI kill switch.                          */
static void
on_ai_enabled_toggled(GtkCheckButton *check, gpointer user_data)
{
    OnApp *app = user_data;
    app->ai_enabled = gtk_check_button_get_active(check);
    on_app_config_set("ai_enabled", app->ai_enabled ? "1" : "0");
    GtkWidget *sub = g_object_get_data(G_OBJECT(check), "on-ai-sub");
    if (sub != NULL)
        gtk_widget_set_sensitive(sub, app->ai_enabled);
    if (app->notify_ai_changed != NULL)
        app->notify_ai_changed(app);
}

/* on_ai_custom_prompt_changed() — persist the custom AI prompt as it changes.*/
static void
on_ai_custom_prompt_changed(GtkTextBuffer *buf, gpointer user_data)
{
    OnApp *app = user_data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    g_free(app->ai_custom_prompt);
    app->ai_custom_prompt = (*text != '\0') ? g_strdup(text) : NULL;
    g_free(text);
    on_app_config_set("ai_custom_prompt", app->ai_custom_prompt);
}

/* on_ai_command_changed() — persist the AI command path as the user types.  */
static void
on_ai_command_changed(GtkEditable *editable, gpointer user_data)
{
    OnApp *app = user_data;
    const gchar *text = gtk_editable_get_text(editable);
    g_free(app->ai_command);
    app->ai_command = (text != NULL && *text != '\0')
        ? g_strdup(text) : NULL;
    on_app_config_set("ai_command",
                      app->ai_command != NULL ? app->ai_command : NULL);
}

/* ---------------------------------------------------------------------------
 * settings_install_css() — the Settings window's DISPLAY-level stylesheet,
 * installed once per process (application priority, so every rule outranks
 * the theme's in any widget state).  One rule per "notes-" class the
 * window puts on its widgets:
 *
 * 1. small_button: a compact button — the theme floors min-height and
 *    min-width, so both are named or trimming the padding moves nothing.
 * 2. small_spin: a spin button no wider than its digits.  GTK4 keeps the
 *    floor and the 8 px side padding on the `spinbutton` node itself
 *    (Default theme: `spinbutton:not(.vertical), entry { min-height: 32px;
 *    padding-left: 8px; padding-right: 8px }`), with a `text` child for the
 *    digits and `button` children for the steppers — GTK3 had them on an
 *    `entry` child, which no longer exists.
 * 3. The Database health plate: a bordered frame in the theme's base
 *    colour (see the plate comment in on_settings_window_open for why
 *    named colours).  Verified on GTK 4.22's compiled Default theme:
 *    @theme_base_color and @borders are both still defined (deprecated
 *    since 4.16, warning only under GTK_DEBUG=css; the theme exports no
 *    CSS variables to use instead).
 * 4. The SHA-256 button: a relief-less label that happens to be clickable,
 *    stripped of its box so the digest sits on the grid's value column.
 * ------------------------------------------------------------------------- */
static void
settings_install_css(void)
{
    static gboolean installed = FALSE;
    if (installed)
        return;
    installed = TRUE;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "button.notes-small-button {"
        "  padding: 1px 8px; min-height: 0; min-width: 0;"
        "}"
        "button.notes-small-button > label { font-size: 85%; }"
        "spinbutton.notes-small-spin {"
        "  min-width: 0; min-height: 0; padding: 1px 2px;"
        "}"
        "spinbutton.notes-small-spin > text { min-width: 0; min-height: 0; }"
        "spinbutton.notes-small-spin > button {"
        "  min-width: 0; min-height: 0; padding: 0 2px;"
        "}"
        "frame.notes-plate {"
        "  background-color: @theme_base_color;"
        "  border: 1px solid @borders;"
        "  border-radius: 6px;"
        "  padding: 8px 10px;"
        "}"
        "button.notes-sha-button {"
        "  padding: 0; margin: 0; border: none; min-height: 0; min-width: 0;"
        "}"
        "button.notes-sha-button > label { font-family: monospace; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void
on_settings_window_open(OnApp *app)
{
    settings_install_css();

    GtkWidget *window = gtk_window_new();
    /* An application window, so the "app." accelerators (Quit,
     * Preferences) work while it has the focus.                            */
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(window));
    gtk_window_set_title(GTK_WINDOW(window), "Notes - Settings");
    /* No default width: a default size is what a window opens AT, natural
     * size or not, and 210 px was narrower than the content — the entries
     * ran under the scrollbar.  The scrolled window propagates the
     * content's natural width, so the window opens exactly as wide as
     * that.                                                                */
    gtk_window_set_transient_for(GTK_WINDOW(window),
                                 GTK_WINDOW(app->library_window));
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    /* Wrap in a scrolled window so the settings are reachable on low-res
     * screens.  propagate_natural_height + max_content_height lets the
     * window size to content when content fits, and scroll when it doesn't. */
    GtkWidget *outer_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(outer_scroll), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(outer_scroll), TRUE);
    /* And the natural WIDTH: without it the window came up at its default
     * width while the content wanted more, and the right-hand widgets
     * ran under the scrollbar with their margin clipped off.             */
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(outer_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(outer_scroll), 600);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_scroll), vbox);
    gtk_window_set_child(GTK_WINDOW(window), outer_scroll);

    /* --- appearance ----------------------------------------------------------*/
    gtk_box_append(GTK_BOX(vbox), section_label("Appearance"));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 12);

    GtkWidget *density_label = gtk_label_new("List density:");
    gtk_label_set_xalign(GTK_LABEL(density_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), density_label, 0, 0, 1, 1);
    static const gchar *const DENSITIES[] = { "Compact", "Comfortable",
                                              NULL };
    GtkWidget *density_combo = gtk_drop_down_new_from_strings(DENSITIES);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(density_combo),
                               app->comfortable_list ? 1 : 0);
    g_signal_connect(density_combo, "notify::selected",
                     G_CALLBACK(on_density_combo_changed), app);
    gtk_grid_attach(GTK_GRID(grid), density_combo, 1, 0, 1, 1);

    gtk_box_append(GTK_BOX(vbox), grid);

    gtk_box_append(GTK_BOX(vbox), bool_check_new(app, BS_SIDEBAR_COUNTS));
    gtk_box_append(GTK_BOX(vbox), bool_check_new(app, BS_SIDEBAR_FIT));
    gtk_box_append(GTK_BOX(vbox), bool_check_new(app, BS_BOLD_LIST_TITLES));
    gtk_box_append(GTK_BOX(vbox), bool_check_new(app, BS_SHOW_DONE_ACTIONS));
    gtk_box_append(GTK_BOX(vbox), bool_check_new(app, BS_STATUSBAR_DB_PATH));

#ifdef __APPLE__
    /* Native macOS menu bar belongs with the other appearance choices.
     * GTK's own quartz backend renders the application menubar natively;
     * this only chooses between that and the in-window bar.  Elsewhere
     * there is no shell menubar to choose, so no checkbox.                */
    GtkWidget *mac_check = gtk_check_button_new_with_label(
        "Use the native macOS menu bar (hide the in-window menu)");
    gtk_widget_set_margin_start(mac_check, 12);
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(mac_check),
        on_app_config_get_bool("native_menubar", FALSE));
    g_signal_connect(mac_check, "toggled",
                     G_CALLBACK(on_native_menubar_toggled), app);
    gtk_box_append(GTK_BOX(vbox), mac_check);
#endif /* __APPLE__ */

    /* The bundled symbolic tree arrows are still SVG: mention the loader
     * when it is missing.  Everything still works without it — those
     * icons just fall back to the theme defaults.                          */
    if (!svg_loader_available()) {
        GtkWidget *warn = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(warn),
            "<small><i>Some icons (tree arrows) render best "
            "with the librsvg loader:\nsudo port install librsvg "
            "(then restart Notes)</i></small>");
        gtk_label_set_xalign(GTK_LABEL(warn), 0.0);
        gtk_label_set_wrap(GTK_LABEL(warn), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(warn), 40);
        gtk_widget_set_margin_start(warn, 12);
        gtk_widget_set_margin_top(warn, 2);
        gtk_widget_set_margin_bottom(warn, 2);
        gtk_box_append(GTK_BOX(vbox), warn);
    }

    gtk_box_append(GTK_BOX(vbox), section_rule());

    /* --- editor options ------------------------------------------------------*/
    gtk_box_append(GTK_BOX(vbox), section_label("Editor"));

    /* The five table-driven editor checkboxes, in display order.           */
    static const BoolSettingId EDITOR_CHECKS[] = {
        BS_CODE_COPY, BS_CODE_LINES, BS_FIRST_LINE_TITLE, BS_COMPACT_TOOLBAR,
        BS_STATUSBAR_NOTE_ID,
    };
    for (gsize i = 0; i < G_N_ELEMENTS(EDITOR_CHECKS); i++)
        gtk_box_append(GTK_BOX(vbox), bool_check_new(app, EDITOR_CHECKS[i]));

    GtkWidget *touch_check = gtk_check_button_new_with_label(
        "Disable touch assistance (selection handles, magnifier)");
    on_app_set_tooltip(touch_check,
        "Hides the touch aids GTK pops up under text selections.");
    gtk_widget_set_margin_start(touch_check, 12);
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(touch_check),
        on_app_config_get_bool("touch_assist", FALSE));
    g_signal_connect(touch_check, "toggled",
                     G_CALLBACK(on_touch_assist_toggled), app);
    gtk_box_append(GTK_BOX(vbox), touch_check);

    /* Program used by an image's "Open" action; empty = system default.   */
    GtkWidget *viewer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(viewer_row, 12);
    GtkWidget *viewer_label = gtk_label_new("Image viewer:");
    gtk_box_append(GTK_BOX(viewer_row), viewer_label);

    GtkWidget *viewer_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(viewer_entry),
                                   "System default");
    {
        gchar *viewer = on_app_config_get("image_viewer");
        if (viewer != NULL) {
            gtk_editable_set_text(GTK_EDITABLE(viewer_entry), viewer);
            g_free(viewer);
        }
    }
    g_signal_connect(viewer_entry, "changed",
                     G_CALLBACK(on_viewer_entry_changed), app);
    gtk_widget_set_hexpand(viewer_entry, TRUE);
    gtk_box_append(GTK_BOX(viewer_row), viewer_entry);

    GtkWidget *viewer_btn = gtk_button_new_with_label(
        "Browse\xe2\x80\xa6");
    g_signal_connect(viewer_btn, "clicked",
                     G_CALLBACK(on_viewer_browse_clicked), viewer_entry);
    gtk_box_append(GTK_BOX(viewer_row), viewer_btn);
    gtk_box_append(GTK_BOX(vbox), viewer_row);

    gtk_box_append(GTK_BOX(vbox), section_rule());

    /* --- Database ------------------------------------------------------------*/
    gtk_box_append(GTK_BOX(vbox), section_label("Database"));

    DbSection *dbs = g_new0(DbSection, 1);
    dbs->app = app;
    /* Freed with the window.                                               */
    g_object_set_data_full(G_OBJECT(window), "on-db-section", dbs, g_free);
    /* The async health pass repaints the plate through this hook while the
     * window is up; the window's destroy takes it down again.            */
    app->settings_db_section = dbs;
    app->notify_db_health    = settings_notify_db_health;
    g_signal_connect(window, "destroy", G_CALLBACK(on_settings_destroy), app);

    /* --- What this database IS, before anything that changes it ----------
     * A GtkGrid rather than a stack of lines with a size group: the values
     * have to start at one x or the five rows read as five unrelated
     * sentences, and a grid is where GTK already keeps that rule.  Every
     * value is <small>, the size the path line has always been.           */
    GtkWidget *info = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(info), 8);
    gtk_grid_set_row_spacing(GTK_GRID(info), 2);

    /* The five facts sit on a plate of their own, so what the database IS
     * is visibly a different kind of thing from the controls below that
     * CHANGE it.
     *
     * A GtkFrame: CSS padding and border sit on it properly, so the text
     * is not hard against the border.  The CSS border REPLACES the
     * theme's frame edge rather than doubling it up, since the snippet
     * restates the whole `border` property.
     *
     * The colours are NAMED theme colours, never literals (the rule is in
     * settings_install_css).
     * @theme_base_color is the white a light theme paints its entries and
     * lists with, and it follows the theme into dark instead of leaving a
     * white slab there.  Named colours also mean GTK re-resolves them
     * itself on a light/dark switch.  A theme naming neither colour drops
     * the declarations and leaves the plate flat, which is a plain look
     * rather than an unreadable one.                                      */
    GtkWidget *plate = gtk_frame_new(NULL);
    gtk_widget_add_css_class(plate, "notes-plate");
    gtk_frame_set_child(GTK_FRAME(plate), info);

    /* Plate and button in a box of their own, and the SECTION MARGINS GO
     * ON THE BOX rather than on the plate: that is what makes the two
     * share one right edge.  With the margins on the plate, aligning the
     * button to the box would land it 12 px outside the plate's border —
     * lined up with nothing a user can see.  The box's 5 px spacing is the
     * gap between them.                                                   */
    GtkWidget *plate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(plate_box, 12);
    gtk_widget_set_margin_end(plate_box, 12);
    gtk_widget_set_margin_bottom(plate_box, 8);
    gtk_box_append(GTK_BOX(plate_box), plate);

    dbs->update_btn = small_button("Update");
    on_app_set_tooltip(dbs->update_btn,
        "Re-read every line above: run PRAGMA integrity_check and PRAGMA "
        "foreign_key_check against this database, then re-count its notes "
        "and folders and re-read its size and SHA-256.");
    /* Right edge against the plate's, which is what the shared margins
     * above are for.                                                      */
    gtk_widget_set_halign(dbs->update_btn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(plate_box), dbs->update_btn);

    /* Ordered by what someone is actually asking.  Health leads: it is the
     * one line that can be BAD NEWS, and a block whose verdict is fourth
     * makes someone read three facts before finding out whether any of
     * them are worth having.  The path, the two quantities and the digest
     * follow as what the verdict is ABOUT — the digest last, because it is
     * the one line nobody reads unless they came looking for it.
     *
     * The LED, its verdict and the button that renews both are one value
     * in three widgets, so they share the value column rather than taking
     * a column each — a column apiece would push the block past the width
     * the window asks for.  The Update button is NOT on this row: it
     * renews every line of the plate, so it belongs under the whole plate
     * rather than beside the one line it used to refresh.                 */
    GtkWidget *health_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    dbs->led_label = gtk_label_new(LED_UNKNOWN);
    gtk_widget_add_css_class(dbs->led_label, "notes-dot-label");
    gtk_box_append(GTK_BOX(health_row), dbs->led_label);
    dbs->health_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->health_label), 0.0);
    gtk_box_append(GTK_BOX(health_row), dbs->health_label);
    info_row_attach(info, 0, "Health:", health_row);

    dbs->path_label = info_row(info, 1, "Current database:");
    dbs->data_label = info_row(info, 2, "Data:");
    dbs->size_label = info_row(info, 3, "Size on disk:");

    dbs->sha_btn = gtk_button_new_with_label("\xe2\x80\x94");
    gtk_button_set_has_frame(GTK_BUTTON(dbs->sha_btn), FALSE);
    /* Every trace of the button's own box goes (settings_install_css):
     * padding, border and margin all offset the label, and the digest has
     * to start at the same x as the four values above it or the column the
     * grid exists to make is broken by the one row that is not a plain
     * label.                                                              */
    gtk_widget_add_css_class(dbs->sha_btn, "notes-sha-button");
    /* Attached straight to the grid: this row is one widget, and a box
     * holding a single child is a box that says nothing.                  */
    info_row_attach(info, 4, "SHA-256:", dbs->sha_btn);

    gtk_box_append(GTK_BOX(vbox), plate_box);

    g_signal_connect(dbs->sha_btn, "clicked",
                     G_CALLBACK(on_db_sha_clicked), dbs);
    g_signal_connect(dbs->update_btn, "clicked",
                     G_CALLBACK(on_db_update_clicked), dbs);

    /* There is NO control here for WHERE the database is kept, and there
     * must not be one again: the file lives at the default location, and a
     * database somewhere else is opened through File → Open Database… —
     * which is also what a launch with nothing at the default location
     * offers.  A second way in was a second flow that MOVED the file
     * rather than opening one, so the same question was answered in two
     * places by two different mechanisms.                                 */
    db_section_refresh(dbs);

    /* --- Rotating backups (off by default) -------------------------------- */
    dbs->bk_check = gtk_check_button_new_with_label(
        "Back up the database automatically");
    gtk_widget_set_margin_start(dbs->bk_check, 12);
    gtk_widget_set_margin_top(dbs->bk_check, 6);
    on_app_set_tooltip(dbs->bk_check,
        "Writes a verified copy of the database into a folder of your "
        "choice on a timer, keeping only the most recent few.  Worth "
        "pointing at a disk INDEPENDENT of wherever the database itself "
        "lives, so one mishap cannot take both.");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dbs->bk_check),
        on_app_config_get_bool("backup_enabled", FALSE));
    gtk_box_append(GTK_BOX(vbox), dbs->bk_check);

    /* Interval and retention, DIRECTLY under the switch: they are the
     * schedule that switch turns on, where the destination and the two
     * buttons below are about WHERE it lands.  The retention cap is the
     * "don't fill the disk" guarantee, so it is a spin button with a hard
     * floor of 1 — a rotation that keeps nothing is not a rotation.       */
    GtkWidget *bk_opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_opts, 12);
    gtk_box_append(GTK_BOX(bk_opts), gtk_label_new("Every"));
    dbs->bk_interval_spin = small_spin(0, 10080, 15, 5);
    on_app_set_tooltip(dbs->bk_interval_spin,
        "Minutes between backups.  0 backs up only when you press "
        "Back Up Now.  A pass whose database has not changed since the "
        "last backup writes nothing.");
    gchar *bkiv = on_app_config_get("backup_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dbs->bk_interval_spin),
        bkiv != NULL ? atoi(bkiv) : ON_BACKUP_INTERVAL_DEFAULT);
    g_free(bkiv);
    gtk_box_append(GTK_BOX(bk_opts), dbs->bk_interval_spin);
    gtk_box_append(GTK_BOX(bk_opts), gtk_label_new("minutes, keeping"));
    dbs->bk_keep_spin = small_spin(1, 500, 1, 3);
    on_app_set_tooltip(dbs->bk_keep_spin,
        "How many backup files to retain.  The oldest are removed once a "
        "NEW backup has been verified, never before.");
    gchar *bkkeep = on_app_config_get("backup_keep");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dbs->bk_keep_spin),
        bkkeep != NULL ? atoi(bkkeep) : ON_BACKUP_KEEP_DEFAULT);
    g_free(bkkeep);
    gtk_box_append(GTK_BOX(bk_opts), dbs->bk_keep_spin);
    gtk_box_append(GTK_BOX(bk_opts), gtk_label_new("files"));
    gtk_box_append(GTK_BOX(vbox), bk_opts);

    /* Where they land, said once at the foot of the block — and the two
     * buttons that change it directly under, so the line and the control
     * that answers it read together.                                      */
    dbs->bk_path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->bk_path_label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(dbs->bk_path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->bk_path_label), 40);
    gtk_widget_set_margin_start(dbs->bk_path_label, 12);
    gtk_widget_set_margin_end(dbs->bk_path_label, 12);
    gtk_widget_set_margin_top(dbs->bk_path_label, 6);
    gtk_box_append(GTK_BOX(vbox), dbs->bk_path_label);

    /* RIGHT-ALIGNED, and the right edge is the UPDATE button's: both rows
     * carry margin_end 12 and hug the right, so the section has one right
     * edge running down it rather than two that are nearly the same.
     * halign END shrinks the row to its natural width and parks it there,
     * which is what puts "Back Up Now" — packed last, so rightmost — flush
     * with Update above.  Do not swap this for pack_end on a full-width
     * row: that reverses the pair, and the folder is chosen before the
     * backup is taken.                                                    */
    GtkWidget *bk_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bk_btns, 12);
    gtk_widget_set_margin_end(bk_btns, 12);
    gtk_widget_set_halign(bk_btns, GTK_ALIGN_END);
    dbs->bk_choose_btn = small_button("Change Folder\xe2\x80\xa6");
    gtk_box_append(GTK_BOX(bk_btns), dbs->bk_choose_btn);
    dbs->bk_now_btn = small_button("Back Up Now");
    gtk_box_append(GTK_BOX(bk_btns), dbs->bk_now_btn);
    gtk_box_append(GTK_BOX(vbox), bk_btns);

    bk_section_refresh(dbs);
    g_signal_connect(dbs->bk_check, "toggled",
                     G_CALLBACK(on_bk_toggled), dbs);
    g_signal_connect(dbs->bk_choose_btn, "clicked",
                     G_CALLBACK(on_bk_choose_clicked), dbs);
    g_signal_connect(dbs->bk_interval_spin, "value-changed",
                     G_CALLBACK(on_bk_interval_changed), dbs);
    g_signal_connect(dbs->bk_keep_spin, "value-changed",
                     G_CALLBACK(on_bk_keep_changed), dbs);
    g_signal_connect(dbs->bk_now_btn, "clicked",
                     G_CALLBACK(on_bk_now_clicked), dbs);

    /* No "check integrity on startup" switch: the check runs every launch
     * (see startup_integrity_check in main.c).  A health check with an off
     * switch can only ever report silence that means "not looked", which
     * is the one answer it must never give.                               */

    gtk_box_append(GTK_BOX(vbox), section_rule());

    /* --- AI features ---------------------------------------------------------*/
    gtk_box_append(GTK_BOX(vbox), section_label("AI Features"));

    GtkWidget *ai_desc = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ai_desc),
        "<small>The AI button in the library toolbar generates a summary of "
        "the current folder\xe2\x80\x99s notes using an external AI command. "
        "Normal or Project mode is set per-folder in the New/Rename Folder "
        "dialog. This switch is a master kill switch: disabling it hides the "
        "button and prevents all AI commands from running.</small>");
    gtk_label_set_xalign(GTK_LABEL(ai_desc), 0.0);
    gtk_label_set_wrap(GTK_LABEL(ai_desc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ai_desc), 40);
    gtk_widget_set_margin_start(ai_desc, 12);
    gtk_widget_set_margin_top(ai_desc, 2);
    gtk_widget_set_margin_bottom(ai_desc, 2);
    gtk_box_append(GTK_BOX(vbox), ai_desc);

    GtkWidget *ai_check = gtk_check_button_new_with_label(
        "Enable AI features");
    gtk_widget_set_margin_start(ai_check, 12);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ai_check), app->ai_enabled);
    gtk_box_append(GTK_BOX(vbox), ai_check);

    /* Sub-group (command), sensitive only when AI is enabled.                */
    GtkWidget *ai_sub = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(ai_sub, 24);
    gtk_widget_set_sensitive(ai_sub, app->ai_enabled);
    gtk_box_append(GTK_BOX(vbox), ai_sub);

    GtkWidget *cmd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *cmd_label = gtk_label_new("AI command:");
    gtk_label_set_xalign(GTK_LABEL(cmd_label), 0.0);
    gtk_box_append(GTK_BOX(cmd_row), cmd_label);

    GtkWidget *cmd_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cmd_entry),
                                   "e.g. /usr/local/bin/claude");
    if (app->ai_command != NULL)
        gtk_editable_set_text(GTK_EDITABLE(cmd_entry), app->ai_command);
    gtk_widget_set_hexpand(cmd_entry, TRUE);
    gtk_box_append(GTK_BOX(cmd_row), cmd_entry);
    gtk_box_append(GTK_BOX(ai_sub), cmd_row);

    GtkWidget *cmd_hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(cmd_hint),
        "<small><i>Run <b>which claude</b> in a terminal to find your "
        "Claude Code command path. The command must read the prompt from "
        "stdin and write the response to stdout.</i></small>");
    gtk_label_set_xalign(GTK_LABEL(cmd_hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(cmd_hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(cmd_hint), 40);
    gtk_box_append(GTK_BOX(ai_sub), cmd_hint);

    GtkWidget *custom_lbl = gtk_label_new("Custom prompt:");
    gtk_label_set_xalign(GTK_LABEL(custom_lbl), 0.0);
    gtk_widget_set_margin_top(custom_lbl, 4);
    gtk_box_append(GTK_BOX(ai_sub), custom_lbl);

    GtkWidget *custom_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(custom_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(custom_view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(custom_view), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(custom_view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(custom_view), 4);
    GtkTextBuffer *custom_buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(custom_view));
    if (app->ai_custom_prompt != NULL)
        gtk_text_buffer_set_text(custom_buf, app->ai_custom_prompt, -1);
    GtkWidget *custom_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(custom_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(custom_scroll), FALSE);
    gtk_widget_set_size_request(custom_scroll, -1, 72);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(custom_scroll),
                                  custom_view);
    gtk_box_append(GTK_BOX(ai_sub), custom_scroll);

    GtkWidget *custom_hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(custom_hint),
        "<small><i>Used when a folder\xe2\x80\x99s AI mode is set to "
        "Custom. Notes and action items are appended after this prompt."
        "</i></small>");
    gtk_label_set_xalign(GTK_LABEL(custom_hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(custom_hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(custom_hint), 40);
    gtk_box_append(GTK_BOX(ai_sub), custom_hint);

    g_signal_connect(ai_check, "toggled",
                     G_CALLBACK(on_ai_enabled_toggled), app);
    g_object_set_data(G_OBJECT(ai_check), "on-ai-sub", ai_sub);
    g_signal_connect(cmd_entry, "changed",
                     G_CALLBACK(on_ai_command_changed), app);
    g_signal_connect(custom_buf, "changed",
                     G_CALLBACK(on_ai_custom_prompt_changed), app);

    gtk_window_present(GTK_WINDOW(window));
}
