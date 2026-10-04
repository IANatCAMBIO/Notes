/* ===========================================================================
 * config.c — the data-directory policy and the notes.ini settings
 * (see config.h)
 *
 * The path policy is plain C.  The ini itself is still a GKeyFile, carried
 * from the GTK build's app.c so the file format is byte-compatible; it goes
 * when GLib does.
 * =========================================================================== */

#include "config.h"
#include "identity.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The one group every key lives under.                                      */
#define CONFIG_GROUP "notes"

/* Where the default data directory sits under the home directory.           */
#define DATA_DIR_PARENT "Library/Application Support"

static char     *config_ini_path = NULL;   /* resolved ini path (owned)      */
static GKeyFile *config_kf       = NULL;   /* in-memory settings             */

/*
 * path_join — "<dir>/<name>" as a new string.
 *
 * Inputs:
 *   dir  — a directory path with no trailing slash
 *   name — what to append
 *
 * Output: a new string; free() it.
 */
static char *
path_join(const char *dir, const char *name)
{
    size_t len  = strlen(dir) + 1 + strlen(name) + 1;
    char  *path = malloc(len);
    snprintf(path, len, "%s/%s", dir, name);
    return path;
}

/*
 * paths_fatal — the data directory cannot be determined.  Guessing would
 * mean reading or writing somewhere the user did not choose, which is the
 * one thing this module exists to prevent, so the process stops.
 *
 * Inputs:
 *   why — what went wrong, for stderr
 *
 * Output: does not return (exit status 2).
 */
static void
paths_fatal(const char *why)
{
    fprintf(stderr, "notes: cannot locate the data directory: %s\n", why);
    exit(2);
}

char *
on_paths_data_dir(void)
{
    const char *override = getenv(ON_DATA_DIR_ENV);
    if (override != NULL && override[0] != '\0') {
        if (override[0] == '/') {
            return strdup(override);
        }
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd) == NULL) {
            paths_fatal("no current directory for a relative " ON_DATA_DIR_ENV);
        }
        return path_join(cwd, override);
    }

    /* The password database, not $HOME: the default must not move because
     * a shell or a test harness exported a different home.                 */
    const struct passwd *pw = getpwuid(getuid());
    if (pw == NULL || pw->pw_dir == NULL) {
        paths_fatal("this user has no home directory");
    }
    char *parent = path_join(pw->pw_dir, DATA_DIR_PARENT);
    char *dir    = path_join(parent, ON_DATA_DIR_NAME);
    free(parent);
    return dir;
}

char *
on_paths_data_file(const char *name)
{
    char *dir  = on_paths_data_dir();
    char *path = path_join(dir, name);
    free(dir);
    return path;
}

void
on_app_config_init(void)
{
    if (config_kf != NULL) {
        return;                      /* already resolved and loaded          */
    }
    char *dir = on_paths_data_dir();
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        fprintf(stderr, "notes: cannot create the data directory %s: %s\n",
                dir, strerror(errno));
    }
    free(dir);

    config_ini_path = on_paths_data_file(ON_CONFIG_FILENAME);
    config_kf       = g_key_file_new();
    g_key_file_load_from_file(config_kf, config_ini_path, G_KEY_FILE_NONE,
                              NULL);       /* an absent file is fine         */
}

gchar *
on_app_config_get(const gchar *key)
{
    if (config_kf == NULL) {
        return NULL;
    }
    gchar *value = g_key_file_get_string(config_kf, CONFIG_GROUP, key, NULL);
    if (value != NULL && *value == '\0') {
        g_free(value);
        value = NULL;
    }
    return value;
}

gboolean
on_app_config_get_bool(const gchar *key, gboolean def)
{
    gchar   *v = on_app_config_get(key);
    gboolean r = (v == NULL) ? def : g_strcmp0(v, "0") != 0;
    g_free(v);
    return r;
}

void
on_app_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL) {
        return;
    }
    gchar   *cur  = g_key_file_get_string(config_kf, CONFIG_GROUP, key, NULL);
    gboolean same = g_strcmp0(cur, value) == 0;
    g_free(cur);
    if (same) {
        return;
    }

    if (value != NULL) {
        g_key_file_set_string(config_kf, CONFIG_GROUP, key, value);
    } else {
        g_key_file_remove_key(config_kf, CONFIG_GROUP, key, NULL);
    }
    GError *err = NULL;
    if (!g_key_file_save_to_file(config_kf, config_ini_path, &err)) {
        fprintf(stderr, "notes: cannot save %s: %s\n", config_ini_path,
                err->message);
        g_clear_error(&err);
    }
}

void
on_app_config_get_size(const gchar *key_w, const gchar *key_h,
                       gint *w, gint *h)
{
    gchar *w_str = on_app_config_get(key_w);
    gchar *h_str = on_app_config_get(key_h);
    if (w_str != NULL && h_str != NULL) {
        gint pw = (gint)g_ascii_strtoll(w_str, NULL, 10);
        gint ph = (gint)g_ascii_strtoll(h_str, NULL, 10);
        if (pw > 0 && ph > 0) {
            *w = pw;
            *h = ph;
        }
    }
    g_free(w_str);
    g_free(h_str);
}

gchar *
on_app_config_load_db_dir(void)
{
    return on_app_config_get("db_dir");
}
