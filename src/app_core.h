/* ===========================================================================
 * app_core.h — the application context shared by the core and the UI
 *
 * One OnApp exists per process.  The app delegate owns the real one and
 * hands it to every window; the command-line tool builds a zeroed one on
 * its stack holding only the database.
 *
 * LAYERING.  The core (db, document, serialize, export, backup, cli, ipc,
 * action_edit, search_job) never includes a UI header and never names a UI
 * function.  Everything it needs from the UI it asks for through the HOOKS
 * below, which the UI installs at startup.  These replace the GTK build's
 * four upward calls (cli.c -> editor window, ipc.c -> library window,
 * ipc.c -> editor window, app.c -> alert dialog).
 *
 * THE HEADLESS INVARIANT.  Headless = every hook is NULL.  The CLI's
 * stack OnApp is zeroed and never has a hook installed, and core code
 * tests the hook it is about to call — `app->live_document != NULL` is
 * what "is there a GUI that might have this note open?" means.  A hook is
 * either NULL or valid for the life of the process; the UI never clears
 * one while windows exist.
 *
 * Everything here is main-thread state.
 * =========================================================================== */

#ifndef ON_APP_CORE_H
#define ON_APP_CORE_H

#include <glib.h>
#include <stdio.h>

#include "config.h"
#include "db.h"
#include "document.h"
#include "main_service.h"

/* Semantic version, baked in by the Makefile (-DON_VERSION="x.y.z") from
 * the VERSION file, so the About panel and the bundle agree.                */
#ifndef ON_VERSION
#define ON_VERSION "dev"
#endif

/* ---------------------------------------------------------------------------
 * OnApp — the application context.
 *
 * State:
 *   db           — the open notes database (owned; on_app_shutdown closes
 *                  it), or NULL before one is open.  Main thread only: a
 *                  worker opens its own connection from db->path.
 *   db_dir       — custom directory holding the database (owned), or NULL
 *                  for the default (the data directory, config.h).
 *   db_transient — TRUE when the open database was chosen for this session
 *                  only (File > Open Database File… > Session Only), so
 *                  db_dir is not what the ini says.
 *
 * Preferences, loaded from notes.ini by on_app_prefs_load (the keys and
 * defaults are one table in app_core.c):
 *   code_copy_buttons      "code_copy_button"         1  code runs show "copy"
 *   code_line_numbers      "code_line_numbers"        1  gutter numbers
 *   sidebar_counts         "sidebar_counts"           0  counts in the sidebar
 *   sidebar_fit_content    "sidebar_fit_content"      1  sidebar fits its rows
 *   first_line_title       "first_line_title"         1  line 0 drawn as title
 *   compact_editor_toolbar "compact_editor_toolbar"   1  style/list menus
 *   comfortable_list       "list_density_comfortable" 1  title + preview rows
 *   statusbar_db_path      "statusbar_db_path"        0  db path in status bars
 *   statusbar_note_id      "statusbar_note_id"        0  "id:N" in editors
 *   show_done_actions      "show_done_actions"        1  list completed items
 *   bold_list_titles       "bold_list_titles"         1  bold comfortable titles
 *   ai_enabled             "ai_enabled"               0  AI summary available
 *   ai_command             "ai_command"       command run for a summary
 *                                             (owned; NULL when unset)
 *   ai_custom_prompt       "ai_custom_prompt" prompt for Custom-mode folders
 *                                             (owned; NULL when unset)
 *
 * Backups (backup.h):
 *   backup_running — a rotating-backup pass is on its worker; keeps a timer
 *                    tick off a manual "Back Up Now".
 *   backup_timer   — the periodic backup timer, or 0 when backups are off.
 *
 * Hooks (see the invariant above; each says who calls it):
 *   notify_notes_changed — the set of notes, folders, tags or action items
 *                    changed: rebuild the sidebar and the notes pane.
 *                    Called by ipc.c after a command that mutates, and by
 *                    the UI itself after structural changes.
 *   notify_note_saved — ONE note's content was saved; nothing structural
 *                    changed: update that note's row only.  Called by the
 *                    editor.
 *   notify_status  — show a one-line event message in the library's status
 *                    bar.  Post through on_app_status().
 *   notify_ai_changed — ai_enabled changed: show or hide the AI control.
 *                    Called by Settings.
 *   notify_db_health — the health verdict changed (a check started or
 *                    landed): repaint the Settings database plate.
 *   show_notice    — show a modal message the user must see (the integrity
 *                    check found damage).  Called by on_app_db_health_start.
 *   open_note      — open the editor for a note, or raise it if it is
 *                    already open, and bring it to the front.  Returns
 *                    FALSE when the note does not exist.  Called by ipc.c
 *                    for `notes note open`.
 *   quicknote      — THE quicknote action: a new note in the root folder,
 *                    its editor in front.  Returns the note's id, 0 on
 *                    failure.  Called by ipc.c for `notes quicknote`.
 *   live_document  — the document of note_id's OPEN editor, made ready for
 *                    an edit from outside (any typing undo group closed),
 *                    or NULL when that note is not open.  The caller edits
 *                    the returned document with on_document_* operations
 *                    only, then calls live_edited.  Called by action_edit.c.
 *   live_edited    — the document live_document returned was just edited:
 *                    the editor redraws, clamps its caret and queues its
 *                    autosave.  Called by action_edit.c, only when it did
 *                    change the document.
 *   save_editors   — every editor with unsaved edits saves NOW; the
 *                    windows stay open.  Returns FALSE when a save failed.
 *                    Called by ipc.c before a command that writes the
 *                    database, so the command works on the current text.
 *   reload_editors — every editor catches up with the database after an
 *                    outside command wrote it: an editor whose note is gone
 *                    closes without saving, an edit made through
 *                    live_document is saved, and an editor whose note was
 *                    rewritten reloads it.  Called by ipc.c after such a
 *                    command.
 *   flush_editors  — close every editor window NOW, each one saving its
 *                    unsaved edits (or deleting its note if it was left
 *                    blank) before the call returns.  Called by
 *                    on_app_shutdown and on_app_switch_db.  See
 *                    app_lifecycle.h for the contract.
 * ------------------------------------------------------------------------- */
typedef struct OnApp {
    OnDatabase *db;
    gchar      *db_dir;
    gboolean    db_transient;

    gboolean    code_copy_buttons;
    gboolean    code_line_numbers;
    gboolean    sidebar_counts;
    gboolean    sidebar_fit_content;
    gboolean    first_line_title;
    gboolean    compact_editor_toolbar;
    gboolean    comfortable_list;
    gboolean    statusbar_db_path;
    gboolean    statusbar_note_id;
    gboolean    show_done_actions;
    gboolean    bold_list_titles;
    gboolean    ai_enabled;
    gchar      *ai_command;
    gchar      *ai_custom_prompt;

    gboolean    backup_running;
    on_timer_id_t   backup_timer;

    void        (*notify_notes_changed)(struct OnApp *app);
    void        (*notify_note_saved)(struct OnApp *app, gint64 note_id);
    void        (*notify_status)(struct OnApp *app, const gchar *message);
    void        (*notify_ai_changed)(struct OnApp *app);
    void        (*notify_db_health)(struct OnApp *app);
    void        (*show_notice)(struct OnApp *app, const gchar *title,
                               const gchar *message);
    gboolean    (*open_note)(struct OnApp *app, gint64 note_id);
    gint64      (*quicknote)(struct OnApp *app);
    OnDocument *(*live_document)(struct OnApp *app, gint64 note_id);
    void        (*live_edited)(struct OnApp *app, gint64 note_id);
    gboolean    (*save_editors)(struct OnApp *app);
    void        (*reload_editors)(struct OnApp *app);
    void        (*flush_editors)(struct OnApp *app);
} OnApp;

/* ---------------------------------------------------------------------------
 * on_app_prefs_load() — (re)read every preference field of `app` from the
 * in-memory config, applying the default for a key that is unset.  Called
 * once at startup, and again by Settings after it has written a key with
 * on_app_config_set — so "what a preference is" has one reader.
 *
 * Inputs:
 *   app — the application context
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_app_prefs_load(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_status() — post a one-line event message to the library window's
 * status bar (printf-style).  A no-op while no library has installed
 * app->notify_status — headless included.
 *
 * Inputs:
 *   app — the application context
 *   fmt — printf-style format and its arguments
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_app_status(OnApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ---------------------------------------------------------------------------
 * on_app_location_text() — a folder path as a status bar shows it: with the
 * statusbar_db_path preference on, the database file's path is prepended as
 * one continuous path (a "/" inserted for the non-path views such as
 * "#tag"); otherwise the location as given.
 *
 * Inputs:
 *   app      — the application context
 *   location — the folder-path part ("/Work/Projects", "#tag", …)
 *
 * Output: a new string; g_free it.
 * ------------------------------------------------------------------------- */
gchar *on_app_location_text(OnApp *app, const gchar *location);

/* ---------------------------------------------------------------------------
 * on_app_db_health_start() — THE integrity check of a freshly opened
 * database: launch and File > Open Database File… both run it, every time,
 * with no switch (a check that can be skipped can only report a silence
 * that means "not looked").  It runs on a worker with its own read-only
 * connection; when the verdict lands on the main thread it posts a status
 * line, or calls app->show_notice if anything is wrong, and then
 * app->notify_db_health.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_app_db_health_start(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_read_stream() — read a whole stdio stream into a string: the CLI's
 * "-" (stdin) arguments, and a delegated command's captured output.
 *
 * Inputs:
 *   f            — the stream to drain
 *   rewind_first — TRUE to rewind() first (a temp capture file); FALSE to
 *                  read from the current position (stdin)
 *
 * Output: a new NUL-terminated string; g_free it.
 * ------------------------------------------------------------------------- */
gchar *on_app_read_stream(FILE *f, gboolean rewind_first);

#endif /* ON_APP_CORE_H */
