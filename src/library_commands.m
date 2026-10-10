#import "library_commands.h"
#import "editor_window.h"
#import "library_sidebar.h"
#import "tool_panel.h"
#import "ui_util.h"

#include "app_lifecycle.h"
#include "export.h"
#include "identity.h"
#include "look.h"

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

/*
 * run_new_folder — "new-folder": a tool panel over the library window that
 * asks for a folder name, then creates it as a child of the current
 * selection's folder.  An empty name keeps the panel open with the problem
 * shown in its hero strip.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_new_folder(id target, id sender)
{
    NotesLibrary *lib    = target;
    gint64        parent = on_library_current_folder_id(lib);

    NSTextField *field = [NSTextField textFieldWithString:@""];
    field.placeholderString = @"Folder Name";
    field.font              = [NSFont systemFontOfSize:on_look_tool_font];
    [field.widthAnchor constraintEqualToConstant:on_look_tool_field_w].active = YES;

    NotesToolPanel *panel = [NotesToolPanel
        panel_with_title:@"New Folder"
              hero_title:@"New Folder"
                subtitle:@"Enter a name for the new folder."
                  symbol:@"folder.badge.plus"
                 content:field
           primary_title:@"Create"
         primary_handler:^NSString * {
        NSString *name = [field.stringValue stringByTrimmingCharactersInSet:
                          NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (name.length == 0) return @"The folder needs a name.";
        if (on_db_folder_create(lib.app->db, parent, name.UTF8String, 0, NULL) == 0)
            return @"The folder could not be created.";
        on_library_refresh_all(lib);
        return nil;
    }];
    [panel show_over:lib.window];
    [panel.window makeFirstResponder:field];
}

/*
 * run_open_db — "open-db": pick a .db file and switch to it either for this
 * session or permanently.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_open_db(id target, id sender)
{
    NotesLibrary *lib = target;
    NSWindow     *win = lib.window;

    on_ui_pick_path(NotesPickOpenFile,
                    @"Choose a Notes Database File",
                    @"Open",
                    nil, nil, win,
                    ^(NSString *path) {
        if (!path) return;
        NSString *message = [NSString stringWithFormat:
                             @"Open \"%@\" for this session only, or set it as your default?",
                             path.lastPathComponent];
        [NotesToolPanel show_message:@"Open Database" message:message symbol:@"cylinder"
                             buttons:@[@"Set as Default", @"Session Only", @"Cancel"]
                                over:win
                             handler:^(NSInteger index) {
            if (index != 0 && index != 1) return;   /* Cancel, or dismissed */
            on_app_switch_db(lib.app, path.UTF8String, index == 0);
        }];
    });
}

/*
 * run_export — pick a destination directory and export notes in the given
 * format.  A notice reports how many were written or what failed.
 *
 * Inputs:
 *   lib    — the library
 *   win    — the window to attach sheets to
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN
 */
static void
run_export(NotesLibrary *lib, NSWindow *win, OnExportFormat format)
{
    on_ui_pick_path(NotesPickFolder,
                    format == ON_EXPORT_HTML
                        ? @"Choose Export Folder for HTML"
                        : @"Choose Export Folder for Markdown",
                    @"Export",
                    nil, nil, win,
                    ^(NSString *path) {
        if (!path) return;
        gchar *err = NULL;
        gint   n   = on_export_all(lib.app, path.UTF8String, format, &err);
        if (n >= 0) {
            on_ui_show_notice([NSString stringWithFormat:
                               @"Exported %d note%s.", n, n == 1 ? "" : "s"],
                              [NSString stringWithFormat:@"Files written to:\n%@", path],
                              win, nil);
        } else {
            on_ui_show_notice(@"Export failed",
                              err ? @(err) : @"An unknown error occurred.",
                              win, nil);
        }
        g_free(err);
    });
}

/*
 * run_export_html — "export-html": export every note as HTML.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_export_html(id target, id sender)
{
    NotesLibrary *lib = target;
    run_export(lib, lib.window, ON_EXPORT_HTML);
}

/*
 * run_export_md — "export-md": export every note as Markdown.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_export_md(id target, id sender)
{
    NotesLibrary *lib = target;
    run_export(lib, lib.window, ON_EXPORT_MARKDOWN);
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t handlers[] = {
    { @"new-note",    run_new_note,    NULL },
    { @"quicknote",   run_quicknote,   NULL },
    { @"new-folder",  run_new_folder,  NULL },
    { @"open-db",     run_open_db,     NULL },
    { @"export-html", run_export_html, NULL },
    { @"export-md",   run_export_md,   NULL },
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
