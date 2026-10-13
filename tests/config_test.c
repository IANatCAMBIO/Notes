/* ===========================================================================
 * config_test.c — the data-directory policy and the ini settings
 *
 * The policy half is the fork's data-safety guarantee: with nothing
 * configured the app resolves ONE directory, under ~/Library/Application
 * Support, and no path the GTK build uses can come out of it.  The ini
 * half checks that values survive a write and a fresh process's read.
 * =========================================================================== */

#include "check.h"

#include "config.h"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Values a real ini holds: spaces, a tilde, bars, colons, an equals sign,
 * a leading space, a hash, a tab, a line break, non-ASCII.                  */
static const char *const tricky[][2] = {
    { "folder",   "/Users/x/Library/Mobile Documents/com~apple~CloudDocs/N" },
    { "stamp",    "/a b/c|/d e/shared_data.db|123456:1791000000" },
    { "leading",  "  two leading spaces" },
    { "equals",   "a=b # not a comment" },
    { "control",  "tab\there\nsecond line" },
    { "unicode",  "caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x93\x93" },
};
#define TRICKY_COUNT (sizeof tricky / sizeof tricky[0])

/*
 * verify_child — the second process of the round trip: a fresh load of the
 * ini the parent wrote must return every value unchanged.
 *
 * Output: the child's exit status (0 = all equal).
 */
static int
verify_child(void)
{
    on_app_config_init();
    for (size_t i = 0; i < TRICKY_COUNT; i++) {
        gchar *v = on_app_config_get(tricky[i][0]);
        char what[128];
        snprintf(what, sizeof what, "a fresh process reads '%s' back unchanged",
                 tricky[i][0]);
        check(g_strcmp0(v, tricky[i][1]) == 0, what);
        g_free(v);
    }
    gchar *gone = on_app_config_get("removed");
    check(gone == NULL, "a removed key stays removed");
    g_free(gone);
    return check_result();
}

/*
 * malformed_child — a process started on an ini that cannot be parsed: the
 * problem is reported, and a setting saved afterwards writes a new ini.
 *
 * Output: the child's exit status (0 = all held).
 */
static int
malformed_child(void)
{
    on_app_config_init();
    check(on_app_config_problem() != NULL, "an unreadable ini is reported");
    gchar *dir = on_app_config_get("folder");
    check(dir == NULL, "and the settings start from their defaults");
    g_free(dir);
    on_app_config_set("flag", "1");
    return check_result();
}

/*
 * test_malformed — a malformed ini (a bad hand edit) is never overwritten:
 * it is kept beside the new one, every byte intact.
 *
 * Inputs:
 *   test_dir — this test's NOTES_DATA_DIR
 *   self     — this executable, to run again as the child
 */
static void
test_malformed(const char *test_dir, const char *self)
{
    static const char bad[] = "[notes]\nfolder=/Volumes/Work\nthis line is not a key\n";
    gchar *dir = g_build_filename(test_dir, "malformed", NULL);
    gchar *ini = g_build_filename(dir, "notes.ini", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(ini, bad, -1, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        setenv(ON_DATA_DIR_ENV, dir, 1);
        execl(self, self, "--malformed", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a process started on a malformed ini reports it and runs on defaults");

    gboolean kept = FALSE;
    GDir *d = g_dir_open(dir, 0, NULL);
    for (const gchar *name; d != NULL && (name = g_dir_read_name(d)) != NULL; ) {
        if (!g_str_has_prefix(name, "notes.ini.bad-")) continue;
        gchar *path = g_build_filename(dir, name, NULL);
        gchar *text = NULL;
        kept = g_file_get_contents(path, &text, NULL, NULL) && strcmp(text, bad) == 0;
        g_free(text);
        g_free(path);
    }
    if (d != NULL) g_dir_close(d);
    check(kept, "the malformed ini is kept aside, unchanged, not saved over");
    g_free(ini);
    g_free(dir);
}

/*
 * test_policy — where the data directory is, with and without the
 * override.  Pure: nothing is created.
 *
 * Inputs:
 *   test_dir — this test's NOTES_DATA_DIR
 */
static void
test_policy(const char *test_dir)
{
    char *dir = on_paths_data_dir();
    check(strcmp(dir, test_dir) == 0, "NOTES_DATA_DIR replaces the data directory");
    free(dir);

    char *ini = on_paths_data_file(ON_CONFIG_FILENAME);
    char expect[PATH_MAX];
    snprintf(expect, sizeof expect, "%s/notes.ini", test_dir);
    check(strcmp(ini, expect) == 0, "the ini is <data dir>/notes.ini");
    free(ini);

    setenv(ON_DATA_DIR_ENV, "relative/place", 1);
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof cwd);
    snprintf(expect, sizeof expect, "%s/relative/place", cwd);
    dir = on_paths_data_dir();
    check(strcmp(dir, expect) == 0, "a relative override is resolved against the cwd");
    free(dir);

    /* The default: computed, compared, never created.                      */
    unsetenv(ON_DATA_DIR_ENV);
    const struct passwd *pw = getpwuid(getuid());
    snprintf(expect, sizeof expect, "%s/Library/Application Support/%s",
             pw->pw_dir, ON_DATA_DIR_NAME);
    dir = on_paths_data_dir();
    check(strcmp(dir, expect) == 0,
          "the default is ~/Library/Application Support/Notes");
    check(strstr(dir, ".local/share") == NULL && strstr(dir, ".config") == NULL &&
          strstr(dir, ".cache") == NULL && strstr(dir, "Mobile Documents") == NULL &&
          strstr(dir, ".app") == NULL,
          "the default names no GTK-build, iCloud or in-bundle location");
    free(dir);

    setenv("HOME", "/tmp/not-my-home", 1);
    setenv("XDG_DATA_HOME", "/tmp/xdg-data", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-config", 1);
    dir = on_paths_data_dir();
    check(strcmp(dir, expect) == 0, "$HOME and the XDG variables do not move the default");
    free(dir);

    setenv(ON_DATA_DIR_ENV, "", 1);
    dir = on_paths_data_dir();
    check(strcmp(dir, expect) == 0, "an empty override is no override");
    free(dir);

    setenv(ON_DATA_DIR_ENV, test_dir, 1);
}

/*
 * test_settings — init, reads with defaults, writes, the no-op write, and
 * the round trip through a second process.
 *
 * Inputs:
 *   test_dir — this test's NOTES_DATA_DIR
 *   self     — this executable, to run again as the verifying child
 */
static void
test_settings(const char *test_dir, const char *self)
{
    /* A data directory that does not exist yet is created, private.        */
    char fresh[PATH_MAX];
    snprintf(fresh, sizeof fresh, "%s/fresh/nested", test_dir);
    setenv(ON_DATA_DIR_ENV, fresh, 1);
    on_app_config_init();
    struct stat sb;
    check(stat(fresh, &sb) == 0 && S_ISDIR(sb.st_mode) && (sb.st_mode & 0777) == 0700,
          "init creates the data directory with mode 0700");

    char ini[PATH_MAX];
    snprintf(ini, sizeof ini, "%s/notes.ini", fresh);
    check(stat(ini, &sb) != 0, "init writes no ini of its own (no defaults file)");

    gchar *unset = on_app_config_get("folder");
    check(unset == NULL, "an unset key reads as NULL");
    g_free(unset);
    check(on_app_config_get_bool("flag", TRUE) && !on_app_config_get_bool("flag", FALSE),
          "an unset boolean takes the caller's default");

    on_app_config_set("flag", "0");
    check(!on_app_config_get_bool("flag", TRUE), "a stored 0 is FALSE");
    on_app_config_set("flag", "1");
    check(on_app_config_get_bool("flag", FALSE), "a stored 1 is TRUE");

    gint w = 640, h = 509;
    on_app_config_set("win_w", "800");
    on_app_config_get_size("win_w", "win_h", &w, &h);
    check(w == 640 && h == 509, "half a size leaves the defaults alone");
    on_app_config_set("win_h", "600");
    on_app_config_get_size("win_w", "win_h", &w, &h);
    check(w == 800 && h == 600, "a whole size is read");

    for (size_t i = 0; i < TRICKY_COUNT; i++) {
        on_app_config_set(tricky[i][0], tricky[i][1]);
    }
    on_app_config_set("removed", "x");
    on_app_config_set("removed", NULL);

    /* An unchanged value must not rewrite the file (the save is a rename,
     * so a rewrite would show as a new inode).                             */
    stat(ini, &sb);
    ino_t before = sb.st_ino;
    on_app_config_set("flag", "1");
    stat(ini, &sb);
    check(sb.st_ino == before, "setting an unchanged value does not rewrite the ini");

    gchar *folder = on_app_config_get("folder");
    check(g_strcmp0(folder, tricky[0][1]) == 0, "a value is read back in this process");
    g_free(folder);

    pid_t pid = fork();
    if (pid == 0) {
        execl(self, self, "--verify", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the ini round-trips through a second process");
}

/*
 * main — run the test, or act as its verifying child.
 *
 * Inputs:
 *   argc, argv — "--verify" selects the child
 *
 * Output: 0 when every check held.
 */
int
main(int argc, char **argv)
{
    const char *test_dir = check_data_dir();
    if (argc > 1 && strcmp(argv[1], "--verify") == 0) {
        return verify_child();
    }
    if (argc > 1 && strcmp(argv[1], "--malformed") == 0) {
        return malformed_child();
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", test_dir);   /* setenv invalidates getenv's */
    test_policy(dir);
    test_settings(dir, argv[0]);
    test_malformed(dir, argv[0]);
    return check_result();
}
