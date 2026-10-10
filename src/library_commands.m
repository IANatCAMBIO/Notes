#import "library_commands.h"
#import "editor_window.h"
#import "library_sidebar.h"
#import "tool_panel.h"
#import "ui_util.h"

#include "app_lifecycle.h"
#include "db.h"
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
 * NotesFolderFields — the controls of the folder prompt (New Folder,
 * Folder Info), and what they hold.
 */
@interface NotesFolderFields : NSObject
@property (nonatomic, strong) NSView      *view;     /* the grid, for the panel */
@property (nonatomic, strong) NSTextField *emoji;
@property (nonatomic, strong) NSTextField *name;
@property (nonatomic, strong) NSArray<NSButton *> *modes;   /* Normal, Project, Custom */
@end

@implementation NotesFolderFields

/*
 * mode_clicked: — the AI mode radios act as one group.
 *
 * Inputs:
 *   sender — the radio clicked
 */
- (void)
mode_clicked:(NSButton *)sender
{
    for (NSButton *b in self.modes) b.state = b == sender ? NSControlStateValueOn : NSControlStateValueOff;
}

/*
 * pick_emoji: — the emoji field's button: the character palette, typing
 * into the emoji field.
 *
 * Inputs:
 *   sender — the button
 */
- (void)
pick_emoji:(id)sender
{
    [self.emoji.window makeFirstResponder:self.emoji];
    [NSApp orderFrontCharacterPalette:sender];
}

/*
 * ai_mode — the AI summary mode the radios say.
 *
 * Output: ON_AI_MODE_NORMAL, _PROJECT or _CUSTOM.
 */
- (gint)
ai_mode
{
    for (NSUInteger i = 0; i < self.modes.count; i++)
        if (self.modes[i].state == NSControlStateValueOn) return (gint)i;
    return ON_AI_MODE_NORMAL;
}

@end

/*
 * folder_fields — the folder prompt's controls (GTK: prompt_for_folder):
 * an optional emoji with a picker, the folder's name, and the AI summary
 * mode (Normal / Project / Custom), filled in from what the folder has.
 *
 * Inputs:
 *   name  — the name to show, or nil
 *   emoji — the emoji to show, or nil
 *   mode  — the AI mode to select (ON_AI_MODE_*)
 *
 * Output: the fields, their grid in .view.
 */
static NotesFolderFields *
folder_fields(NSString *name, NSString *emoji, gint mode)
{
    NotesFolderFields *f = [[NotesFolderFields alloc] init];
    NSFont *font = [NSFont systemFontOfSize:on_look_tool_font];

    f.emoji = [NSTextField textFieldWithString:emoji ?: @""];
    f.emoji.font      = [NSFont systemFontOfSize:on_look_tool_font + 3];
    f.emoji.alignment = NSTextAlignmentCenter;
    f.emoji.toolTip   = @"Optional emoji";
    f.emoji.identifier = @"folder-emoji";
    [f.emoji.widthAnchor constraintEqualToConstant:on_look_tool_emoji_w].active = YES;
    NSButton *picker = [NSButton buttonWithTitle:@"Pick…" target:f action:@selector(pick_emoji:)];
    picker.font = font;
    NSStackView *emoji_row = [NSStackView stackViewWithViews:@[f.emoji, picker]];

    f.name = [NSTextField textFieldWithString:name ?: @""];
    f.name.placeholderString = @"Folder Name";
    f.name.identifier = @"folder-name";
    f.name.font = font;
    [f.name.widthAnchor constraintEqualToConstant:on_look_tool_field_w].active = YES;

    NSMutableArray<NSButton *> *modes = [NSMutableArray array];
    for (NSString *title in @[@"Normal", @"Project", @"Custom"]) {
        NSButton *radio = [NSButton radioButtonWithTitle:title target:f action:@selector(mode_clicked:)];
        radio.font = font;
        [modes addObject:radio];
    }
    f.modes = modes;
    [f mode_clicked:modes[(NSUInteger)MAX(0, MIN(mode, 2))]];
    NSStackView *mode_row = [NSStackView stackViewWithViews:modes];
    mode_row.spacing = on_look_tool_btn_gap;

    NSTextField *(^label)(NSString *) = ^NSTextField *(NSString *text) {
        NSTextField *l = [NSTextField labelWithString:text];
        l.font = font;
        return l;
    };
    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[label(@"Emoji (optional):"), emoji_row],
        @[label(@"Folder Name:"),      f.name],
        @[label(@"AI summary mode:"),  mode_row],
    ]];
    grid.rowSpacing    = on_look_tool_btn_gap;
    grid.columnSpacing = on_look_tool_btn_gap;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger r = 0; r < grid.numberOfRows; r++) [grid rowAtIndex:r].yPlacement = NSGridCellPlacementCenter;
    f.view = grid;
    return f;
}

/*
 * prompt_for_folder — the folder prompt in a tool panel.  OK hands the
 * trimmed name, the AI mode and the trimmed emoji to `done`; an empty name
 * keeps the panel open with the problem shown.
 *
 * Inputs:
 *   lib     — the library (the panel sits over its window)
 *   title   — "New Folder" or "Folder Info"
 *   symbol  — the hero's SF Symbol
 *   name, emoji, mode — what the fields start with
 *   done    — writes the answer; returns nil, or a problem to show
 */
static void
prompt_for_folder(NotesLibrary *lib, NSString *title, NSString *symbol,
                  NSString *name, NSString *emoji, gint mode,
                  NSString *(^done)(NSString *name, gint mode, NSString *emoji))
{
    NotesFolderFields *f = folder_fields(name, emoji, mode);
    NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NotesToolPanel *panel = [NotesToolPanel
        panel_with_title:title hero_title:title
                subtitle:@"A folder's emoji shows before its name in the sidebar."
                  symbol:symbol content:f.view primary_title:@"OK"
         primary_handler:^NSString * {
        NSString *trimmed = [f.name.stringValue stringByTrimmingCharactersInSet:space];
        if (trimmed.length == 0) return @"The folder needs a name.";
        return done(trimmed, [f ai_mode], [f.emoji.stringValue stringByTrimmingCharactersInSet:space]);
    }];
    [panel show_over:lib.window];
    [panel.window makeFirstResponder:f.name];
}

/*
 * run_new_folder — "new-folder" (also New Subfolder… on a folder's menu):
 * the folder prompt, then the folder is made under the selected folder
 * (the top level otherwise) — the parent is taken now, as the selection
 * may move while the prompt is up.
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
    prompt_for_folder(lib, @"New Folder", @"folder.badge.plus", nil, nil, ON_AI_MODE_NORMAL,
                      ^NSString *(NSString *name, gint mode, NSString *emoji) {
        if (on_db_folder_create(lib.app->db, parent, name.UTF8String, mode, emoji.UTF8String) == 0)
            return @"The folder could not be created.";
        on_library_refresh_all(lib);
        return nil;
    });
}

/*
 * run_folder_info — "folder-info": the folder prompt over the selected
 * folder's name, emoji and AI mode; OK writes all three.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_folder_info(id target, id sender)
{
    NotesLibrary *lib = target;
    if (lib.sel_kind != on_sb_folder) return;
    gint64  folder = lib.sel_id;
    gchar  *emoji  = on_db_folder_get_emoji(lib.app->db, folder);
    gint    mode   = on_db_folder_get_ai_mode(lib.app->db, folder);
    prompt_for_folder(lib, @"Folder Info", @"folder", lib.sel_name, @(emoji), mode,
                      ^NSString *(NSString *name, gint new_mode, NSString *new_emoji) {
        if (!on_db_folder_update(lib.app->db, folder, name.UTF8String, new_mode, new_emoji.UTF8String))
            return @"The folder could not be changed.";
        on_library_refresh_all(lib);
        return nil;
    });
    g_free(emoji);
}

/*
 * validate_folder_selected — a folder (not one in the Trash) is selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_folder_selected(id target, id item)
{
    return ((NotesLibrary *)target).sel_kind == on_sb_folder;
}

/*
 * run_sort_subfolders — "sort-subfolders": the selected folder's (or the
 * Notes root's) direct children, sorted alphabetically and kept that way.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_sort_subfolders(id target, id sender)
{
    NotesLibrary *lib = target;
    if (lib.sel_kind != on_sb_folder && lib.sel_kind != on_sb_root) return;
    gint n = on_db_folder_sort_children(lib.app->db, lib.sel_kind == on_sb_folder ? lib.sel_id : 0);
    if (n > 1) {
        on_app_status(lib.app, "Sorted %d subfolders alphabetically", n);
        on_library_refresh_all(lib);
    }
}

/*
 * validate_sort_subfolders — a folder or the Notes root is selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_sort_subfolders(id target, id item)
{
    NotesLibrary *lib = target;
    return lib.sel_kind == on_sb_folder || lib.sel_kind == on_sb_root;
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
                               @"Exported %d note%@.", n, n == 1 ? @"" : @"s"],
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

/*
 * close_editors_for_ids — close (each saving first) the editors of notes
 * held in a GArray of gint64, as on_db_folder_note_ids and
 * on_db_trash_note_ids return them; the array is freed.
 *
 * Inputs:
 *   ids — the note ids (owned: freed here)
 */
static void
close_editors_for_ids(GArray *ids)
{
    on_editor_close_notes((const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
}

/*
 * run_delete_folder — "delete-folder": the selected folder (with its
 * subtree) goes to the Trash, no question asked — it can be restored; a
 * folder already in the Trash is deleted permanently after one
 * confirmation.  Editors on its notes are closed first either way.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_delete_folder(id target, id sender)
{
    NotesLibrary *lib = target;
    if (lib.sel_kind == on_sb_folder) {
        if (on_library_trash_folder(lib, lib.sel_id, lib.sel_name)) on_library_refresh_all(lib);
        return;
    }
    if (lib.sel_kind != on_sb_trash_folder) return;
    gint64 folder_id = lib.sel_id;
    on_ui_confirm(@"Permanently delete this folder and everything inside it?", nil, @"Delete",
                  lib.window, ^(BOOL confirmed) {
        if (!confirmed) return;
        close_editors_for_ids(on_db_folder_note_ids(lib.app->db, folder_id));
        on_db_folder_delete(lib.app->db, folder_id);
        if (lib.sel_kind == on_sb_trash_folder && lib.sel_id == folder_id)
            on_library_sidebar_select(lib, on_sb_trash, 0);
        on_library_refresh_all(lib);
    });
}

/*
 * validate_delete_folder — a folder, or a folder in the Trash, is selected;
 * for the latter the item says "Delete Permanently".
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the menu or toolbar item
 *
 * Output: YES to enable it.
 */
static BOOL
validate_delete_folder(id target, id item)
{
    NotesLibrary *lib = target;
    on_command_item_update(item, lib.sel_kind == on_sb_trash_folder ? @"Delete Folder Permanently"
                                                                    : nil, nil, NO);
    return lib.sel_kind == on_sb_folder || lib.sel_kind == on_sb_trash_folder;
}

/*
 * run_folder_restore — "folder-restore": the selected trashed folder (with
 * its subtree) goes back where it was deleted from; the selection follows.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_folder_restore(id target, id sender)
{
    NotesLibrary *lib = target;
    if (lib.sel_kind != on_sb_trash_folder) return;
    gint64 folder_id = lib.sel_id;
    if (!on_db_folder_restore(lib.app->db, folder_id)) return;
    on_app_status(lib.app, "Folder “%s” restored", lib.sel_name.UTF8String);
    on_library_refresh_all(lib);
    on_library_sidebar_select(lib, on_sb_folder, folder_id);
}

/*
 * validate_folder_restore — a folder in the Trash is selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_folder_restore(id target, id item)
{
    return ((NotesLibrary *)target).sel_kind == on_sb_trash_folder;
}

/*
 * run_empty_trash — "empty-trash": after one confirmation, close the
 * editors of every note the purge takes (trashed folders' notes too) and
 * delete everything in the Trash.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_empty_trash(id target, id sender)
{
    NotesLibrary *lib = target;
    on_ui_confirm(@"Permanently delete everything in the Trash?", nil, @"Empty Trash",
                  lib.window, ^(BOOL confirmed) {
        if (!confirmed) return;
        close_editors_for_ids(on_db_trash_note_ids(lib.app->db));
        if (!on_db_trash_empty(lib.app->db)) return;
        on_app_status(lib.app, "Trash emptied");
        if (on_library_in_trash_view(lib)) on_library_sidebar_select(lib, on_sb_trash, 0);
        on_library_refresh_all(lib);
    });
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t handlers[] = {
    { @"new-note",    run_new_note,    NULL },
    { @"quicknote",   run_quicknote,   NULL },
    { @"new-folder",  run_new_folder,  NULL },
    { @"open-db",     run_open_db,     NULL },
    { @"export-html", run_export_html, NULL },
    { @"export-md",   run_export_md,   NULL },
    { @"delete-folder",  run_delete_folder,  validate_delete_folder  },
    { @"folder-info",     run_folder_info,     validate_folder_selected  },
    { @"sort-subfolders", run_sort_subfolders, validate_sort_subfolders  },
    { @"folder-restore", run_folder_restore, validate_folder_restore },
    { @"empty-trash",    run_empty_trash,    NULL                    },
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

/*
 * on_library_delete_notes — see library_commands.h.
 */
void
on_library_delete_notes(NotesLibrary *lib, NSArray<NSNumber *> *ids)
{
    if (ids.count == 0) return;
    NSString *question = ids.count == 1
        ? @"Permanently delete this note?"
        : [NSString stringWithFormat:@"Permanently delete these %lu notes?", (unsigned long)ids.count];
    on_ui_confirm(question, nil, @"Delete", lib.window, ^(BOOL confirmed) {
        if (!confirmed) return;
        GArray *note_ids = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)ids.count);
        for (NSNumber *n in ids) {
            gint64 nid = n.longLongValue;
            g_array_append_val(note_ids, nid);
        }
        on_editor_close_notes((const gint64 *)note_ids->data, note_ids->len);
        on_db_notes_delete(lib.app->db, (const gint64 *)note_ids->data, note_ids->len);
        g_array_free(note_ids, TRUE);
        on_library_refresh_all(lib);
    });
}
