/* ===========================================================================
 * app_core.c — the application context's toolkit-free helpers
 * (see app_core.h)
 * =========================================================================== */

#include "app_core.h"

#include <stdarg.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * The boolean preferences: ini key, default when the key is absent, and
 * the OnApp field it loads into.  THE one statement of each default — the
 * GTK build had two (the defaults passed in main.c and a notes.ini.defaults
 * file that seeded a fresh install with different values); these are the
 * values a fresh GTK install actually ran with.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar *key;
    gboolean     def;
    size_t       offset;             /* of a gboolean field in OnApp         */
} bool_pref_t;

static const bool_pref_t bool_prefs[] = {
    { "code_copy_button",         TRUE,  offsetof(OnApp, code_copy_buttons)      },
    { "code_line_numbers",        TRUE,  offsetof(OnApp, code_line_numbers)      },
    { "sidebar_counts",           FALSE, offsetof(OnApp, sidebar_counts)         },
    { "sidebar_fit_content",      TRUE,  offsetof(OnApp, sidebar_fit_content)    },
    { "first_line_title",         TRUE,  offsetof(OnApp, first_line_title)       },
    { "compact_editor_toolbar",   TRUE,  offsetof(OnApp, compact_editor_toolbar) },
    { "list_density_comfortable", TRUE,  offsetof(OnApp, comfortable_list)       },
    { "statusbar_db_path",        FALSE, offsetof(OnApp, statusbar_db_path)      },
    { "statusbar_note_id",        FALSE, offsetof(OnApp, statusbar_note_id)      },
    { "show_done_actions",        TRUE,  offsetof(OnApp, show_done_actions)      },
    { "bold_list_titles",         TRUE,  offsetof(OnApp, bold_list_titles)       },
    { "ai_enabled",               FALSE, offsetof(OnApp, ai_enabled)             },
};

/*
 * on_app_prefs_load — see app_core.h.
 */
void
on_app_prefs_load(OnApp *app)
{
    for (gsize i = 0; i < G_N_ELEMENTS(bool_prefs); i++) {
        gboolean *field = (gboolean *)((gchar *)app + bool_prefs[i].offset);
        *field = on_app_config_get_bool(bool_prefs[i].key, bool_prefs[i].def);
    }
    g_free(app->ai_command);
    g_free(app->ai_custom_prompt);
    app->ai_command       = on_app_config_get("ai_command");
    app->ai_custom_prompt = on_app_config_get("ai_custom_prompt");
}

/*
 * on_app_status — see app_core.h.
 */
void
on_app_status(OnApp *app, const gchar *fmt, ...)
{
    if (app->notify_status == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    gchar *message = g_strdup_vprintf(fmt, args);
    va_end(args);
    app->notify_status(app, message);
    g_free(message);
}

/*
 * on_app_location_text — see app_core.h.
 */
gchar *
on_app_location_text(OnApp *app, const gchar *location)
{
    if (!app->statusbar_db_path || app->db == NULL || app->db->path == NULL) {
        return g_strdup(location);
    }
    /* One continuous path: folder locations already start with "/"; the
     * non-path views ("#tag", "Pinned Notes", …) get one inserted.         */
    return g_strdup_printf("%s%s%s", app->db->path,
                           location != NULL && location[0] == '/' ? "" : "/",
                           location);
}

/*
 * on_app_folder_location — see app_core.h.
 */
gchar *
on_app_folder_location(OnApp *app, gint64 folder_id)
{
    gchar *folder   = on_db_folder_path(app->db, folder_id);
    gchar *location = g_strconcat("/", folder, NULL);
    gchar *text     = on_app_location_text(app, location);
    g_free(location);
    g_free(folder);
    return text;
}

/*
 * db_health_done — the async health pass landed: report it.
 *
 * Inputs:
 *   db   — the connection the verdict was recorded on
 *   data — the OnApp
 *
 * Output: none.
 */
static void
db_health_done(OnDatabase *db, gpointer data)
{
    OnApp *app = data;
    const OnDbHealth *h = on_db_health(db);
    if (h != NULL && h->ok) {
        on_app_status(app, "Database check passed");
    } else if (app->show_notice != NULL) {
        gchar *message = g_strdup_printf(
            "%s\n\n%s",
            h != NULL && h->ran
                ? "The database integrity check found issues:"
                : "The database integrity check could not be completed:",
            h != NULL && h->detail != NULL ? h->detail
                                           : "no detail reported");
        app->show_notice(app, "Database Integrity Check", message);
        g_free(message);
    }
    if (app->notify_db_health != NULL) {
        app->notify_db_health(app);
    }
}

/*
 * on_app_db_health_start — see app_core.h.
 */
void
on_app_db_health_start(OnApp *app)
{
    on_db_health_check_async(app->db, db_health_done, app);
    if (app->notify_db_health != NULL) {
        app->notify_db_health(app);  /* the plate shows "checking"           */
    }
}

/*
 * on_app_read_stream — see app_core.h.
 */
gchar *
on_app_read_stream(FILE *f, gboolean rewind_first)
{
    GString *s = g_string_new(NULL);
    if (rewind_first) {
        rewind(f);
    }
    gchar buf[4096];
    gsize n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        g_string_append_len(s, buf, (gssize)n);
    }
    return g_string_free(s, FALSE);
}
