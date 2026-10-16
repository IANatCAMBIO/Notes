#import "library_commands.h"
#import "editor_window.h"
#import "library_notes.h"
#import "library_sidebar.h"
#import "tool_panel.h"
#import "ui_util.h"

#include "action_sync.h"
#include "serialize.h"
#include "app_lifecycle.h"
#include "db.h"
#include "export.h"
#include "identity.h"
#include "look.h"

/*
 * library_commands.m — see library_commands.h for what is implemented.
 */

/*
 * open_new_note — create a new note: an empty title line and an empty body
 * line, then the #tag on a line of its own when a tag is given
 * (on_note_new_from_text); show it in the library and open its editor in
 * front, the caret on the title line.
 *
 * Inputs:
 *   lib — the library
 *   tag — the tag's name, or nil for none
 *
 * Output: none; nothing happens when the note cannot be created.
 */
static void
open_new_note(NotesLibrary *lib, NSString *tag)
{
    gint64 note_id = on_note_new_from_text(lib.app->db, tag.UTF8String, "\n");
    if (note_id == 0) return;
    /* The full refresh: a new note changes the counts as well as the
     * notes pane. */
    on_library_refresh_all(lib);
    on_editor_window_open(lib.app, note_id, NotesEditorRevealNone, 0, nil);
}

/*
 * run_new_note — "new-note": a note with the selected tag (none when the
 * selection is not a tag).
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_new_note(id target, id sender)
{
    NotesLibrary *lib = target;
    open_new_note(lib, on_library_current_tag(lib));
}

/*
 * run_quicknote — "quicknote": a note with no tag whatever is selected,
 * its editor in front.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_quicknote(id target, id sender)
{
    open_new_note(target, nil);
}

/*
 * NotesTagFields — the controls of the Edit Tag panel, and what they hold.
 */
@interface NotesTagFields : NSObject
@property (nonatomic, strong) NSView      *view;       /* the grid, for the panel */
@property (nonatomic, strong) NSTextField *emoji;
@property (nonatomic, strong) NSButton    *favorite;
@end

@implementation NotesTagFields

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

@end

/*
 * tag_fields — the Edit Tag panel's controls: the tag's name (fixed), an
 * optional emoji with a picker, and Favorite, filled in from the tag.
 *
 * Inputs:
 *   tag — the tag
 *
 * Output: the fields, their grid in .view.
 */
static NotesTagFields *
tag_fields(const OnTag *tag)
{
    NotesTagFields *f    = [[NotesTagFields alloc] init];
    NSFont         *font = [NSFont systemFontOfSize:on_look_ui_font];

    f.emoji = [NSTextField textFieldWithString:@(tag->emoji)];
    f.emoji.font       = [NSFont systemFontOfSize:on_look_ui_font + 3];
    f.emoji.alignment  = NSTextAlignmentCenter;
    f.emoji.toolTip    = @"Optional emoji";
    f.emoji.identifier = @"tag-emoji";
    [f.emoji.widthAnchor constraintEqualToConstant:on_look_tool_emoji_w].active = YES;
    NSButton *picker = [NSButton buttonWithTitle:@"Pick…" target:f action:@selector(pick_emoji:)];
    picker.font = font;
    NSStackView *emoji_row = [NSStackView stackViewWithViews:@[f.emoji, picker]];

    f.favorite = [NSButton checkboxWithTitle:@"Show in the Favorites bar" target:nil action:nil];
    f.favorite.font       = font;
    f.favorite.identifier = @"tag-favorite";
    f.favorite.state      = tag->favorite ? NSControlStateValueOn : NSControlStateValueOff;

    NSTextField *(^label)(NSString *) = ^NSTextField *(NSString *text) {
        NSTextField *l = [NSTextField labelWithString:text];
        l.font = font;
        return l;
    };
    NSTextField *name = label(@(tag->name));
    name.font = [NSFont boldSystemFontOfSize:on_look_ui_font];
    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[label(@"Tag:"),               name],
        @[label(@"Emoji (optional):"),  emoji_row],
        @[label(@"Favorite:"),          f.favorite],
    ]];
    grid.rowSpacing    = on_look_tool_btn_gap;
    grid.columnSpacing = on_look_tool_btn_gap;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger r = 0; r < grid.numberOfRows; r++) [grid rowAtIndex:r].yPlacement = NSGridCellPlacementCenter;
    f.view = grid;
    return f;
}

/*
 * on_library_edit_tag — see library_commands.h.
 */
void
on_library_edit_tag(NotesLibrary *lib, gint64 tag_id)
{
    OnTag *tag = on_db_tag_get(lib.app->db, tag_id);
    if (tag == NULL) return;
    NotesTagFields *f     = tag_fields(tag);
    NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NSString       *path  = @(lib.app->db->path);
    NotesToolPanel *panel = [NotesToolPanel
        panel_with_title:@"Edit Tag" hero_title:@"Edit Tag"
                subtitle:@"The emoji shows in Notes and Tasks; a tag's name never changes."
                  symbol:@"tag" content:f.view buttons:@[@"OK", @"Cancel"]
                 handler:^NSString *(NSInteger index) {
        if (index != 0) return nil;                  /* Cancel */
        /* The panel is not modal: the database may have been switched. */
        if (lib.app->db == NULL || ![@(lib.app->db->path) isEqualToString:path]) return nil;
        NSString *emoji = [f.emoji.stringValue stringByTrimmingCharactersInSet:space];
        if (!on_db_tag_set_emoji(lib.app->db, tag_id, emoji.UTF8String) ||
            !on_db_tag_set_favorite(lib.app->db, tag_id, f.favorite.state == NSControlStateValueOn))
            return @"The tag could not be changed.";
        on_library_refresh_all(lib);
        return nil;
    }];
    on_db_tag_free(tag);
    [panel show_over:lib.window];
    [panel.window makeFirstResponder:f.emoji];
}

/*
 * run_edit_tag — "edit-tag": the Edit Tag panel for the tag the item
 * names (the sidebar's menu), else the selected tag.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 */
static void
run_edit_tag(id target, id sender)
{
    NotesLibrary *lib = target;
    on_sb_ref_t   ref = { lib.sel_kind, lib.sel_id };
    on_library_sb_ref(sender, &ref);
    if (ref.kind == on_sb_tag) on_library_edit_tag(lib, ref.ident);
}

/*
 * validate_tag_selected — the item names a tag, or a tag is selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item
 *
 * Output: YES to enable it.
 */
static BOOL
validate_tag_selected(id target, id item)
{
    NotesLibrary *lib = target;
    on_sb_ref_t   ref = { lib.sel_kind, lib.sel_id };
    on_library_sb_ref(item, &ref);
    return ref.kind == on_sb_tag;
}

/*
 * on_library_open_db — see library_commands.h.
 */
void
on_library_open_db(NotesLibrary *lib, NSString *path)
{
    NSWindow *win = lib.window;
    if ([path.stringByStandardizingPath isEqualToString:@(lib.app->db->path).stringByStandardizingPath])
        return;
    NSString *message = [NSString stringWithFormat:@"Open “%@” as the default database of Notes "
                                                    @"and Tasks, or for this session only?",
                                                    path.lastPathComponent];
    NSArray<NSString *> *buttons = @[@"Set as Default", @"Session Only", @"Cancel"];
    [NotesToolPanel show_message:@"Open Database" message:message symbol:@"cylinder"
                         buttons:buttons
                            over:win
                         handler:^(NSInteger index) {
        if (index < 0 || [buttons[(NSUInteger)index] isEqualToString:@"Cancel"]) return;
        BOOL set_default = [buttons[(NSUInteger)index] isEqualToString:@"Set as Default"];
        gchar *why = NULL;
        on_switch_result_t result = on_app_switch_db(lib.app, path.UTF8String, set_default, &why);
        NSString *reason = why ? @(why) : @"Unknown error.";
        g_free(why);
        if (result == ON_SWITCH_DONE) return;
        if (result == ON_SWITCH_UNSAVED) {
            on_editor_report_unsaved(@"Database not opened", win);
            return;
        }
        if (lib.app->db == NULL) {              /* not even the old one reopened */
            [NSApp terminate:nil];
            return;
        }
        on_ui_show_notice(@"Database Error",
                          [NSString stringWithFormat:@"Could not open:\n%@\n\n%@", path, reason],
                          win, nil);
    }];
}

/*
 * run_open_db — "open-db": pick a .db file and hand it to
 * on_library_open_db (GTK on_open_db).
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu or toolbar item (unused)
 */
static void
run_open_db(id target, id sender)
{
    NotesLibrary *lib = target;
    UTType       *db  = [UTType typeWithFilenameExtension:@"db"];
    on_ui_pick_path(NotesPickOpenFile, @"Open Database", @"Open", db ? @[db] : nil, nil,
                    ^(NSString *path) {
        if (path != nil) on_library_open_db(lib, path);
    });
}

/*
 * on_library_export — see library_commands.h.
 */
gint
on_library_export(NotesLibrary *lib, OnExportFormat format, NSArray<NSNumber *> *ids, NSString *dir)
{
    gchar *err = NULL;
    gint   n   = 0;
    if (ids == nil) {
        n = on_export_all(lib.app, dir.UTF8String, format, &err);
    } else {
        for (NSNumber *nid in ids)
            if (on_export_note(lib.app, nid.longLongValue, dir.UTF8String, format)) n++;
    }
    if (n >= 0)
        on_ui_show_notice([NSString stringWithFormat:@"Exported %d note%@.", n, n == 1 ? @"" : @"s"],
                          [NSString stringWithFormat:@"Files written to:\n%@", dir], lib.window, nil);
    else
        on_ui_show_notice(@"Export failed", err ? @(err) : @"An unknown error occurred.", lib.window, nil);
    g_free(err);
    return n;
}

/*
 * run_export — pick a destination folder, then export notes there
 * (on_library_export): the ones given, or every note.
 *
 * Inputs:
 *   lib    — the library
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN
 *   ids    — the notes (NSNumber ids), or nil for all of them
 */
static void
run_export(NotesLibrary *lib, OnExportFormat format, NSArray<NSNumber *> *ids)
{
    on_ui_pick_path(NotesPickFolder,
                    format == ON_EXPORT_HTML ? @"Choose Export Folder for HTML"
                                             : @"Choose Export Folder for Markdown",
                    @"Export", nil, nil, ^(NSString *path) {
        if (path) on_library_export(lib, format, ids, path);
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
    run_export(target, ON_EXPORT_HTML, nil);
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
    run_export(target, ON_EXPORT_MARKDOWN, nil);
}

/*
 * close_editors_for_ids — close (each saving first) the editors of notes
 * held in a GArray of gint64, as on_db_trash_note_ids returns them; the
 * array is freed.  When an editor
 * cannot save, a tool panel says the operation was stopped.
 *
 * Inputs:
 *   lib     — the library (its window centres the panel)
 *   ids     — the note ids (owned: freed here)
 *   stopped — the panel's title: what did not happen
 *
 * Output: YES when every such editor closed; NO when one is still open,
 * and the caller must leave the notes alone.
 */
static BOOL
close_editors_for_ids(NotesLibrary *lib, GArray *ids, NSString *stopped)
{
    BOOL ok = on_editor_close_notes((const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
    if (!ok) on_editor_report_unsaved(stopped, lib.window);
    return ok;
}

/*
 * confirm_permanent — ask before a permanent deletion; the handler runs only
 * if the user confirms AND the database the question was about is still
 * the one open (the panel is not modal: File ▸ Open Database can switch it
 * underneath, and ids mean nothing in another file).  The handler must
 * itself re-check that what it deletes is still in the Trash.
 *
 * Inputs:
 *   lib      — the library
 *   question — the panel's headline
 *   action   — the confirming button's title
 *   handler  — the deletion
 */
static void
confirm_permanent(NotesLibrary *lib, NSString *question, NSString *action, void (^handler)(void))
{
    NSString *path = @(lib.app->db->path);
    on_ui_confirm(question, nil, action, lib.window, ^(BOOL confirmed) {
        if (!confirmed || lib.app->db == NULL || ![@(lib.app->db->path) isEqualToString:path]) return;
        handler();
    });
}

/*
 * run_empty_trash — "empty-trash": after one confirmation, close the
 * editors of every note the purge takes and delete everything in the
 * Trash.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_empty_trash(id target, id sender)
{
    NotesLibrary *lib = target;
    confirm_permanent(lib, @"Permanently delete everything in the Trash?", @"Empty Trash", ^{
        if (!close_editors_for_ids(lib, on_db_trash_note_ids(lib.app->db), @"Nothing was deleted")) return;
        if (!on_db_trash_empty(lib.app->db)) return;
        on_app_status(lib.app, "Trash emptied");
        if (on_library_in_trash_view(lib)) on_library_sidebar_select(lib, on_sb_all, 0);
        on_library_refresh_all(lib);
    });
}

/*
 * run_note_export_html / run_note_export_md — "note-export-html" /
 * "note-export-md": export the selected notes.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_note_export_html(id target, id sender)
{
    run_export(target, ON_EXPORT_HTML, on_library_notes_selected_ids(target));
}

static void
run_note_export_md(id target, id sender)
{
    run_export(target, ON_EXPORT_MARKDOWN, on_library_notes_selected_ids(target));
}

/*
 * validate_notes_selected — notes are selected, outside the Trash view.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_notes_selected(id target, id item)
{
    return !on_library_in_trash_view(target) && on_library_notes_selected_ids(target).count > 0;
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t handlers[] = {
    { @"new-note",    run_new_note,    NULL },
    { @"quicknote",   run_quicknote,   NULL },
    { @"edit-tag",    run_edit_tag,    validate_tag_selected },
    { @"open-db",     run_open_db,     NULL },
    { @"export-html", run_export_html, NULL },
    { @"export-md",   run_export_md,   NULL },
    { @"note-export-html", run_note_export_html, validate_notes_selected },
    { @"note-export-md",   run_note_export_md,   validate_notes_selected },
    { @"empty-trash", run_empty_trash, NULL },
};

/*
 * on_library_commands_install — see library_commands.h.
 */
void
on_library_commands_install(NotesLibrary *lib)
{
    on_library_add_commands(handlers, sizeof handlers / sizeof handlers[0]);
}

/*
 * on_library_trash_notes — see library_commands.h.
 */
BOOL
on_library_trash_notes(NotesLibrary *lib, const gint64 *ids, size_t n)
{
    if (!on_editor_close_notes(ids, n)) {
        on_editor_report_unsaved(@"Nothing was moved to the Trash", lib.window);
        return NO;
    }
    if (!on_db_notes_trash(lib.app->db, ids, n)) return NO;
    on_app_status(lib.app, "Moved %zu note%s to Trash", n, n == 1 ? "" : "s");
    return YES;
}

/*
 * on_library_tag_notes — see library_commands.h.
 */
BOOL
on_library_tag_notes(NotesLibrary *lib, NSArray<NSNumber *> *ids, NSString *name)
{
    OnDatabase *db = lib.app->db;
    gboolean    own;
    if (ids.count == 0 || !on_db_txn_begin(db, &own)) return NO;
    BOOL ok = YES;
    const gchar *tag = name.UTF8String;
    for (NSNumber *nid in ids)
        ok = ok && on_note_append_tags(lib.app, nid.longLongValue, &tag, 1);
    if (!on_db_txn_end(db, own, ok)) return NO;
    on_app_status(lib.app, "Tagged %lu note%s “%s”", (unsigned long)ids.count,
                  ids.count == 1 ? "" : "s", name.UTF8String);
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
    confirm_permanent(lib, question, @"Delete", ^{
        /* Only what is still in the Trash: a note restored while the
         * question was up is not deleted.                                  */
        gint64 *note_ids = on_ui_ids(ids);
        size_t  n        = 0;
        for (NSUInteger i = 0; i < ids.count; i++)
            if (on_db_note_is_trashed(lib.app->db, note_ids[i])) note_ids[n++] = note_ids[i];
        if (on_editor_close_notes(note_ids, n)) {
            on_db_notes_delete(lib.app->db, note_ids, n);
        } else {
            on_editor_report_unsaved(@"Nothing was deleted", lib.window);
        }
        free(note_ids);
        on_library_refresh_all(lib);
    });
}
