#import "library_commands.h"
#import "editor_window.h"
#import "library_sidebar.h"

/*
 * library_commands.m — see library_commands.h for what is implemented.
 */

/*
 * open_new_note — create an empty note in a folder, show it in the library
 * and open its editor in front.
 *
 * Inputs:
 *   lib       — the library
 *   folder_id — where the note goes (0 = the top level)
 *
 * Output: the new note's id, or 0 if it could not be created.
 */
static gint64
open_new_note(NotesLibrary *lib, gint64 folder_id)
{
    gint64 note_id = on_db_note_create(lib.app->db, folder_id);
    if (note_id == 0) return 0;
    /* The full refresh: a new note changes its folder's count as well as
     * the notes pane. */
    on_library_refresh_all(lib);
    on_editor_window_open(lib.app, note_id, NotesEditorRevealNone, 0, nil);
    return note_id;
}

/*
 * run_new_note — "new-note": a note in the selected folder (the top level
 * when the selection is not a folder).
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_new_note(id target, id sender)
{
    NotesLibrary *lib = target;
    open_new_note(lib, on_library_current_folder_id(lib));
}

/*
 * run_quicknote — "quicknote": a note at the top level whatever is
 * selected, its editor in front.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_quicknote(id target, id sender)
{
    open_new_note(target, 0);
}

/*
 * hook_quicknote — OnApp.quicknote: `notes quicknote` from the command
 * line.  The same action as the toolbar's.
 *
 * Inputs:
 *   app — the application context (unused: the library is the app's one)
 *
 * Output: the new note's id, or 0 on failure.
 */
static gint64
hook_quicknote(OnApp *app)
{
    return open_new_note(on_library(), 0);
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t handlers[] = {
    { @"new-note",  run_new_note,  NULL },
    { @"quicknote", run_quicknote, NULL },
};

/*
 * on_library_commands_install — see library_commands.h.
 */
void
on_library_commands_install(NotesLibrary *lib)
{
    on_library_add_commands(handlers, sizeof handlers / sizeof handlers[0]);
    lib.app->quicknote = hook_quicknote;
}

/*
 * on_library_trash_notes — see library_commands.h.
 */
BOOL
on_library_trash_notes(NotesLibrary *lib, const gint64 *ids, size_t n)
{
    on_editor_close_notes(ids, n);
    if (!on_db_notes_trash(lib.app->db, ids, n)) return NO;
    on_app_status(lib.app, "Moved %zu note%s to Trash", n, n == 1 ? "" : "s");
    return YES;
}

/*
 * on_library_trash_folder — see library_commands.h.
 */
BOOL
on_library_trash_folder(NotesLibrary *lib, gint64 folder_id, NSString *name)
{
    GArray *note_ids = on_db_folder_note_ids(lib.app->db, folder_id);
    on_editor_close_notes((const gint64 *)note_ids->data, note_ids->len);
    g_array_free(note_ids, TRUE);
    if (!on_db_folder_trash(lib.app->db, folder_id)) return NO;
    on_app_status(lib.app, "Folder “%s” moved to Trash", name.UTF8String);
    if (lib.sel_kind == on_sb_folder && lib.sel_id == folder_id) {
        on_library_sidebar_select(lib, on_sb_root, 0);
    }
    return YES;
}
