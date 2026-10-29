/* ===========================================================================
 * app_core.h — the application context shared by the core and the UI
 *
 * One OnApp exists per process.  The app delegate owns the real one and
 * hands it to every window; the development seeder and the tests build a
 * zeroed one holding only the database.
 *
 * LAYERING.  The core (db, document, serialize, export, backup,
 * action_edit, search_job) never includes a UI header and never names a UI
 * function.  Everything it needs from the UI it asks for through the HOOKS
 * below, which the UI installs at startup.  These replace the GTK build's
 * upward calls from the core into the UI.
 *
 * THE HEADLESS INVARIANT.  Headless = every hook is NULL.  A seeder's or
 * a test's OnApp is zeroed and never has a hook installed, and core code
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
 *   db           — the open shared database (owned; on_app_shutdown closes
 *                  it), or NULL before one is open.  Main thread only: a
 *                  worker opens its own connection from db->path.
 *
 * Preferences, loaded from notes.ini by on_app_prefs_load (the keys and
 * defaults are one table in app_core.c):
 *   code_copy_buttons      "code_copy_button"         1  code runs show "copy"
 *   code_line_numbers      "code_line_numbers"        1  gutter numbers
 *   drop_files_last_line   "drop_files_last_line"     0  dropped files go to the note's last line
 *   sidebar_counts         "sidebar_counts"           1  counts in the sidebar (Tasks' key too)
 *   sidebar_fit_content    "sidebar_fit_content"      1  sidebar fits its rows
 *   sidebar_visible        "sidebar_visible"          1  the sidebar shows
 *   favorites_bar_visible  "favorites_bar_visible"    1  the Favorites bar shows
 * (and "sidebar_width", a number the library window reads itself: the
 * width the sidebar was last dragged to; and "favorites_order", tag ids
 * the Favorites bar reads itself: its buttons as last dragged into order)
 *   first_line_title       "first_line_title"         1  title line drawn as title
 *   compact_editor_toolbar "compact_editor_toolbar"   1  style/list menus
 *   comfortable_list       "list_density_comfortable" 1  title + preview rows
 *   statusbar_db_path      "statusbar_db_path"        0  db path in status bars
 *   statusbar_note_id      "statusbar_note_id"        0  "id:N" in editors
 *   show_done_actions      "show_done_actions"        1  list completed items
 *   bold_list_titles       "bold_list_titles"         1  bold comfortable titles
 *   ai_enabled             "ai_enabled"               0  AI summary available
 *   new_note_hotkey        "new_note_hotkey"          1  New Note works from any app
 *   ai_command             "ai_command"       command run for a summary
 *                                             (owned; NULL when unset)
 *   grid_group             "grid_group"       "none" (default), "day", "week",
 *                                             "month" or "year": the grid's cards in
 *                                             sections by the date the list
 *                                             is sorted by (on_grid_group_t)
 *
 * Backups (backup.h) and the pull (action_sync.h):
 *   backup_running — a backup pass is on its worker; keeps a timer tick
 *                    off a manual "Back Up Now".
 *   backup_timer   — the periodic backup poll, or 0 before it is armed.
 *   sync_timer     — the poll for outside changes, or 0 before it is armed.
 *   rev_tasks      — the change_rev 'tasks' revision the pull last acted
 *                    on (or that only Notes' own commits moved since).
 *   rev_notes      — likewise 'notes', for the library's refresh.
 *   pull_failed    — the last pull left a note unsynced: the next poll
 *                    tick pulls again, outside commit or not.
 *   commit_look_posted — the look after Notes' own commit is posted and
 *                    has not run yet (one post for any number of commits).
 *
 * Hooks (see the invariant above; each says who calls it):
 *   notify_notes_changed — the set of notes, tags or action items
 *                    changed: rebuild the sidebar and the notes pane.
 *                    Called by the UI after structural changes and by
 *                    on_app_switch_db.
 *   notify_note_saved — ONE note's content was saved; nothing structural
 *                    changed: update that note's row only.  Called by the
 *                    editor.
 *   notify_status  — show a one-line event message in the library's status
 *                    bar.  Post through on_app_status().
 *   notify_ai_changed — ai_enabled changed: show or hide the AI control.
 *                    Called by Settings.
 *   notify_db_state — the health verdict changed (a check started or
 *                    landed) or a backup pass ended: repaint the Settings
 *                    database plate and backup line.  Called by
 *                    on_app_db_health_start and backup.c.
 *   show_notice    — show a modal message the user must see (the integrity
 *                    check found damage).  Called by on_app_db_health_start.
 *   open_note      — open the editor for a note, or raise it if it is
 *                    already open, and bring it to the front.  Returns
 *                    FALSE when the note does not exist.  Called by the
 *                    notes pane.
 *   live_document  — the document of note_id's OPEN editor, made ready for
 *                    an edit from outside (any typing undo group closed),
 *                    or NULL when that note is not open.  The caller edits
 *                    the returned document with on_document_* operations
 *                    only, then calls live_edited.  Called by the pull
 *                    (action_sync.c), for its own and for action edits.
 *   live_edited    — the document live_document returned was edited:
 *                    the editor redraws (caret and selection clamped, no
 *                    scroll) and queues its autosave.  Called by the pull
 *                    (action_sync.c) when its transaction has ended, only
 *                    when it did change the document.
 *   live_held      — note_id's open editor waits for the user to answer
 *                    its "changed outside" panel: the pull leaves the
 *                    note alone until then (the answer runs the pull).
 *                    FALSE when the note is not open or not waiting.
 *                    Called by the pull (action_sync.c).
 *   live_save      — write the open editor of note_id NOW (its autosave
 *                    cancelled, every action line pushed), inside the
 *                    caller's transaction: the pull's rewrite of a live
 *                    document is saved in the same transaction as the
 *                    seen_* it moved.  The editor counts as saved only if
 *                    that transaction commits (on_db_txn_on_end); if not,
 *                    it stays dirty.  FALSE when the write failed or was
 *                    refused because another writer rewrote the note.
 *   check_editors  — the database changed outside the app: every editor
 *                    whose note another writer rewrote reloads it (no
 *                    unsaved edits) or says so (unsaved edits kept); one
 *                    whose note is gone closes.  Called by the pull,
 *                    outside any transaction.
 *   flush_editors  — close every editor window NOW, each one saving its
 *                    unsaved edits (or deleting its note if it was left
 *                    blank) before the call returns.  An editor whose save
 *                    fails stays open and the hook returns FALSE.  Called
 *                    by on_app_shutdown and on_app_switch_db.  See
 *                    app_lifecycle.h for the contract.
 * ------------------------------------------------------------------------- */
/* How the grid groups its cards (the grid_group preference). */
typedef enum {
    on_grid_group_none,
    on_grid_group_day,
    on_grid_group_week,
    on_grid_group_month,
    on_grid_group_year,
} on_grid_group_t;

typedef struct OnApp {
    OnDatabase *db;

    gboolean    code_copy_buttons;
    gboolean    code_line_numbers;
    gboolean    sidebar_counts;
    gboolean    sidebar_fit_content;
    gboolean    sidebar_visible;
    gboolean    favorites_bar_visible;
    gboolean    first_line_title;
    gboolean    compact_editor_toolbar;
    gboolean    comfortable_list;
    gboolean    statusbar_db_path;
    gboolean    statusbar_note_id;
    gboolean    show_done_actions;
    gboolean    bold_list_titles;
    gboolean    drop_files_last_line;
    gboolean    ai_enabled;
    gboolean    new_note_hotkey;
    gchar      *ai_command;
    on_grid_group_t grid_group;

    gboolean    backup_running;
    on_timer_id_t   backup_timer;
    on_timer_id_t   sync_timer;
    gint64      rev_tasks;
    gint64      rev_notes;
    gboolean    pull_failed;
    gboolean    commit_look_posted;

    void        (*notify_notes_changed)(struct OnApp *app);
    void        (*notify_note_saved)(struct OnApp *app, gint64 note_id);
    void        (*notify_status)(struct OnApp *app, const gchar *message);
    void        (*notify_ai_changed)(struct OnApp *app);
    void        (*notify_db_state)(struct OnApp *app);
    void        (*show_notice)(struct OnApp *app, const gchar *title,
                               const gchar *message);
    gboolean    (*open_note)(struct OnApp *app, gint64 note_id);
    OnDocument *(*live_document)(struct OnApp *app, gint64 note_id);
    void        (*live_edited)(struct OnApp *app, gint64 note_id);
    gboolean    (*live_held)(struct OnApp *app, gint64 note_id);
    gboolean    (*live_save)(struct OnApp *app, gint64 note_id);
    void        (*check_editors)(struct OnApp *app);
    gboolean    (*flush_editors)(struct OnApp *app);
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
 * on_app_location_text() — a view's name as a status bar shows it: with
 * the statusbar_db_path preference on, the database file's path is
 * prepended as one continuous path ("/" between); otherwise the location
 * as given.
 *
 * Inputs:
 *   app      — the application context
 *   location — the view ("#Stride", "Pinned Notes", …)
 *
 * Output: a new string; g_free it.
 * ------------------------------------------------------------------------- */
gchar *on_app_location_text(OnApp *app, const gchar *location);

/* ---------------------------------------------------------------------------
 * on_app_db_health_start() — THE integrity check of the open database:
 * launch and File > Open Database File… run it every time, with no switch
 * (a check that can be skipped can only report a silence that means "not
 * looked"), and Settings' Update on request.  It runs on a worker with its own read-only
 * connection; when the verdict lands on the main thread it posts a status
 * line, or calls app->show_notice if anything is wrong, and then
 * app->notify_db_state.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_app_db_health_start(OnApp *app);

#endif /* ON_APP_CORE_H */
