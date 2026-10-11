/* ===========================================================================
 * config.h — where this build keeps its files, and the notes.ini settings
 *
 * LOCATION POLICY (the rule the whole fork's data safety rests on)
 *
 * Every file this app keeps — everything but a database the user
 * explicitly opened elsewhere — lives in ONE directory, the data directory:
 *
 *     $NOTES_DATA_DIR                                  when that is set
 *     ~/Library/Application Support/NotesAppKit        otherwise
 *
 *     <data dir>/notes.ini            the settings (this module)
 *     <data dir>/notes.db             the default database (db.h)
 *     <data dir>/backups/             the default backup folder (backup.h)
 *     <data dir>/notes-appkit.sock    the CLI <-> GUI socket (ipc.h)
 *
 * Nothing is ever resolved relative to argv[0], the bundle, or the XDG
 * directories the GTK build used (~/.config/notes, ~/.local/share/notes,
 * ~/.cache), so this build cannot find the GTK build's ini, database or
 * socket whatever it is launched from.  The home directory comes from the
 * password database, not $HOME, so a stray environment cannot move the
 * default either; NOTES_DATA_DIR is the one override, and `make run`,
 * `make seed` and `make test` always set it to a folder under dev/.
 *
 * The only way out of the data directory is the `db_dir` setting, written
 * by File > Open Database File… > Set as Default: a decision the user makes
 * in the app, never a default.
 *
 * Not in the data directory, and not this module's: the anonymous temporary
 * files ipc.c captures a command's output in (tmpfile), and what AppKit
 * keeps for any app under its bundle identifier (~/Library/Preferences/
 * <bundle id>.plist, saved window state).  Neither holds notes.
 *
 * SETTINGS
 *
 * One [notes] group of key=value lines (GKeyFile format: the GTK build's,
 * so a hand-copied ini reads the same).  Loaded into memory ONCE by
 * on_app_config_init(); every read is served from memory and the file is
 * rewritten only when a value actually changes.  There is no defaults
 * file: a key that is absent takes the default its reader passes, and the
 * preference defaults are one table in app_core.c.  Main thread only.
 * =========================================================================== */

#ifndef ON_CONFIG_H
#define ON_CONFIG_H

#include <glib.h>

/* ---------------------------------------------------------------------------
 * on_paths_data_dir() — the data directory (see the policy above).  A
 * relative NOTES_DATA_DIR is resolved against the current directory, so
 * the CLI and the app it launches agree.  Nothing is created.
 *
 * Output: a new absolute path; free() it.  When no directory can be
 *         determined (no password entry, or a relative override with no
 *         current directory) the process exits with status 2 rather than
 *         guess: a guessed location is how notes "vanish".
 * ------------------------------------------------------------------------- */
char *on_paths_data_dir(void);

/* ---------------------------------------------------------------------------
 * on_paths_data_file() — the path of a file inside the data directory.
 *
 * Inputs:
 *   name — the file's name (ON_CONFIG_FILENAME, ON_DB_FILENAME, …)
 *
 * Output: a new absolute path; free() it.
 * ------------------------------------------------------------------------- */
char *on_paths_data_file(const char *name);

/* ---------------------------------------------------------------------------
 * on_app_config_init() — create the data directory if it is missing (mode
 * 0700) and load notes.ini from it into memory; a missing ini is an empty
 * configuration.  Must run before any other config call; a second call is
 * a no-op.
 *
 * An ini that exists but cannot be read (a malformed hand edit) is moved
 * aside as notes.ini.bad-<time> — or, if it cannot be moved, left alone
 * and never saved over this run — and the configuration starts empty;
 * on_app_config_problem() then says so.
 *
 * Output: none.  A directory that cannot be created is reported on stderr;
 *         the settings then live in memory for this run only.
 * ------------------------------------------------------------------------- */
void on_app_config_init(void);

/* ---------------------------------------------------------------------------
 * on_app_config_problem() — why notes.ini could not be loaded at init, for
 * the app to tell the user (the CLI has printed it on stderr).
 *
 * Output: the message (owned by the module), or NULL when the ini loaded
 *         or did not exist.
 * ------------------------------------------------------------------------- */
const gchar *on_app_config_problem(void);

/* ---------------------------------------------------------------------------
 * on_app_config_get() — read one setting from the in-memory config.
 *
 * Inputs:
 *   key — the setting's name
 *
 * Output: a new string (g_free it), or NULL when unset or empty.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_get(const gchar *key);

/* ---------------------------------------------------------------------------
 * on_app_config_get_bool() — read a 0/1 setting.  The app only ever writes
 * "0" or "1", so any other stored value reads as TRUE.
 *
 * Inputs:
 *   key — the setting's name
 *   def — the answer when the key is unset
 *
 * Output: the setting.
 * ------------------------------------------------------------------------- */
gboolean on_app_config_get_bool(const gchar *key, gboolean def);

/* ---------------------------------------------------------------------------
 * on_app_config_set() — change one setting: the in-memory config AND the
 * ini file, which is rewritten whole.  An unchanged value writes nothing
 * (some callers fire per keystroke).
 *
 * Inputs:
 *   key   — the setting's name
 *   value — the new value; NULL removes the key
 *
 * Output: none.  A failed write is reported on stderr; the in-memory value
 *         still changes.
 * ------------------------------------------------------------------------- */
void on_app_config_set(const gchar *key, const gchar *value);

/* ---------------------------------------------------------------------------
 * on_app_config_get_size() — read a persisted window size from two keys.
 *
 * Inputs:
 *   key_w, key_h — the two keys (e.g. "search_win_w"/"search_win_h")
 *   w, h         — in/out: the caller's defaults
 *
 * Output: *w and *h are replaced only when BOTH keys are present and both
 *         parse to a positive number — never half a size.
 * ------------------------------------------------------------------------- */
void on_app_config_get_size(const gchar *key_w, const gchar *key_h,
                            gint *w, gint *h);

/* ---------------------------------------------------------------------------
 * on_app_config_load_db_dir() — the custom database directory (`db_dir`).
 *
 * Output: a new string (g_free it), or NULL when the default location (the
 *         data directory) is in use.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_load_db_dir(void);

#endif /* ON_CONFIG_H */
