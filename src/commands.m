#import "commands.h"
#import "ui_util.h"

#include "identity.h"

/* Shorthand for the table's modifier column. */
enum {
    cmd       = NSEventModifierFlagCommand,
    cmd_shift = NSEventModifierFlagCommand | NSEventModifierFlagShift,
    cmd_ctrl  = NSEventModifierFlagCommand | NSEventModifierFlagControl,
    cmd_opt   = NSEventModifierFlagCommand | NSEventModifierFlagOption,
    app       = NotesCommandScopeApp,
    win       = NotesCommandScopeWindow,
};

/* Backspace as a key equivalent: ⌘⌫ shows as the delete key in the menu. */
#define KEY_BACKSPACE @"\x08"

/*
 * THE COMMAND TABLE — and the app's keyboard map.  Identifiers are the GTK
 * build's action names where the command existed there.  Shortcuts follow
 * macOS: where a GTK binding collided with a system one (⌘M, ⌘E, ⌘Y) the
 * system meaning won and the command moved; porting/INTERFACES.md lists
 * every change and the reason.
 */
static const on_command_t commands[] = {
    /* ── App scope: works from any window, acts on the library ── */
    { @"about",            @"About " ON_APP_NAME,      @"",  0,         app, nil },
    { @"preferences",      @"Settings…",               @",", cmd,       app, @"settings" },
    { @"new-note",         @"New Note",                @"n", cmd,       app, @"newnote" },
    { @"new-folder",       @"New Folder…",             @"n", cmd_shift, app, @"new-folder" },
    { @"quicknote",        @"Quick Note",              @"",  0,         app, @"archive" },
    { @"open-db",          @"Open Database File…",     @"",  0,         app, nil },
    { @"export-html",      @"Export All as HTML…",     @"",  0,         app, nil },
    { @"export-md",        @"Export All as Markdown…", @"",  0,         app, nil },
    { @"view-list",        @"as List",                 @"1", cmd,       app, nil },
    { @"view-grid",        @"as Grid",                 @"2", cmd,       app, nil },
    { @"toggle-view",      @"List or Grid",            @"",  0,         app, @"grid" },
    { @"toggle-sidebar",   @"Hide Sidebar",            @"s", cmd_ctrl,  app, @"sidebar" },
    { @"toggle-done",      @"Hide Completed",          @"",  0,         app, @"hidden" },
    { @"media",            @"Media…",                  @"i", cmd_shift, app, @"images" },
    { @"search",           @"Search Notes…",           @"f", cmd_shift, app, @"search" },
    { @"ai-summary",       @"AI Summary",              @"",  0,         app, @"microchip" },
    { @"show-library",     @"Library",                 @"0", cmd,       app, nil },

    /* ── Window scope, library ── */
    { @"delete-note",      @"Move to Trash",           KEY_BACKSPACE, cmd, win, @"deletenote" },
    { @"delete-folder",    @"Move Folder to Trash",    @"",  0,         win, @"delete-folder" },
    { @"empty-trash",      @"Empty Trash…",            @"",  0,         win, nil },
    { @"folder-info",      @"Folder Info…",            @"",  0,         win, nil },
    { @"folder-restore",   @"Restore Folder",          @"",  0,         win, nil },
    { @"sort-subfolders",  @"Sort Subfolders Alphabetically", @"", 0,   win, nil },
    { @"search-here",      @"Search Here…",            @"",  0,         win, nil },
    { @"note-open",        @"Open",                    @"",  0,         win, nil },
    { @"note-pin",         @"Pin",                     @"",  0,         win, nil },
    { @"note-restore",     @"Restore",                 @"",  0,         win, nil },
    { @"note-export-html", @"Export as HTML…",         @"",  0,         win, nil },
    { @"note-export-md",   @"Export as Markdown…",     @"",  0,         win, nil },

    /* ── Window scope, both windows: the library searches, an editor finds ── */
    { @"find",             @"Find…",                   @"f", cmd,       win, nil },

    /* ── Window scope, editor ── */
    { @"find-next",        @"Find Next",               @"g", cmd,       win, nil },
    { @"find-previous",    @"Find Previous",           @"g", cmd_shift, win, nil },
    { @"find-selection",   @"Use Selection for Find",  @"e", cmd,       win, nil },
    { @"inline-bold",      @"Bold",                    @"b", cmd,       win, nil },
    { @"inline-italic",    @"Italic",                  @"i", cmd,       win, nil },
    { @"inline-underline", @"Underline",               @"u", cmd,       win, nil },
    { @"inline-strike",    @"Strikethrough",           @"x", cmd_shift, win, nil },
    { @"para-h1",          @"Heading 1",               @"",  0,         win, nil },
    { @"para-h2",          @"Heading 2",               @"",  0,         win, nil },
    { @"para-body",        @"Body",                    @"",  0,         win, nil },
    { @"para-bullet",      @"Bulleted List",           @"",  0,         win, nil },
    { @"para-number",      @"Numbered List",           @"",  0,         win, nil },
    { @"para-check",       @"Task List",               @"",  0,         win, nil },
    { @"para-code",        @"Code Block",              @"m", cmd_shift, win, nil },
    { @"insert-image",     @"Image…",                  @"",  0,         win, nil },
    { @"insert-table",     @"Table",                   @"t", cmd_opt,   win, nil },
    { @"insert-emoji",     @"Emoji…",                  @"",  0,         win, nil },
    { @"insert-date",      @"Date",                    @"d", cmd,       win, nil },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

/*
 * on_command_find — table lookup.
 * Inputs/output are documented in commands.h.
 */
const on_command_t *
on_command_find(NSString *ident)
{
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if ([commands[i].ident isEqualToString:ident]) return &commands[i];
    }
    return NULL;
}

/*
 * command_action — the selector a command's items send: run_app_command:
 * to the app delegate for app scope, run_command: up the responder chain
 * for window scope.
 *
 * Inputs:
 *   command — the command
 *   target  — receives the items' target (nil for the responder chain)
 *
 * Output: the selector.
 */
static SEL
command_action(const on_command_t *command, id *target)
{
    if (command->scope == NotesCommandScopeApp) {
        *target = NSApp.delegate;
        return @selector(run_app_command:);
    }
    *target = nil;
    return @selector(run_command:);
}

/*
 * on_command_menu_item — menu item from the table.
 * Inputs/output are documented in commands.h.
 */
NSMenuItem *
on_command_menu_item(NSString *ident)
{
    const on_command_t *command = on_command_find(ident);
    NSCAssert(command != NULL, @"unknown command %@", ident);
    id  target = nil;
    SEL action = command_action(command, &target);
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:command->title
                                                  action:action
                                           keyEquivalent:command->key];
    item.keyEquivalentModifierMask = command->mods;
    item.target     = target;
    item.identifier = command->ident;
    return item;
}

/*
 * on_command_toolbar_item — toolbar item from the table.
 * Inputs/output are documented in commands.h.
 */
NSToolbarItem *
on_command_toolbar_item(NSString *ident)
{
    const on_command_t *command = on_command_find(ident);
    NSCAssert(command != NULL && command->icon != nil, @"no toolbar command %@", ident);
    id  target = nil;
    SEL action = command_action(command, &target);
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:command->ident];
    item.label    = command->title;
    item.toolTip  = command->title;
    item.image    = on_ui_icon(command->icon, on_look_toolbar_icon);
    item.target   = target;
    item.action   = action;
    item.bordered = YES;
    return item;
}

/*
 * on_command_ident — the command behind an item.
 * Inputs/output are documented in commands.h.
 */
NSString *
on_command_ident(id item)
{
    if ([item isKindOfClass:NSToolbarItem.class]) return ((NSToolbarItem *)item).itemIdentifier;
    if ([item isKindOfClass:NSMenuItem.class] || [item isKindOfClass:NSControl.class])
        return ((id<NSUserInterfaceItemIdentification>)item).identifier;
    return nil;
}

/*
 * on_command_item_update — a command's current state shown on its item.
 * Inputs/output are documented in commands.h.
 */
void
on_command_item_update(id item, NSString *title, NSString *icon, BOOL checked)
{
    const on_command_t *command = on_command_find(on_command_ident(item));
    if (command == NULL) return;
    NSString *shown = title ?: command->title;
    if ([item isKindOfClass:NSMenuItem.class]) {
        NSMenuItem *menu_item = item;
        menu_item.title = shown;
        menu_item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    } else if ([item isKindOfClass:NSToolbarItem.class]) {
        NSToolbarItem *toolbar_item = item;
        toolbar_item.label   = shown;
        toolbar_item.toolTip = shown;
        toolbar_item.image   = on_ui_icon(icon ?: command->icon, on_look_toolbar_icon);
    }
}

/*
 * handler_for — the handler of an item's command in a table.
 *
 * Inputs:
 *   table — a module's handlers
 *   n     — how many
 *   item  — the item
 *
 * Output: the handler, or NULL when the table does not have the command.
 */
static const on_command_handler_t *
handler_for(const on_command_handler_t *table, size_t n, id item)
{
    NSString *ident = on_command_ident(item);
    if (!ident) return NULL;
    for (size_t i = 0; i < n; i++) {
        if ([table[i].ident isEqualToString:ident]) return &table[i];
    }
    return NULL;
}

/*
 * on_command_run — dispatch through a handler table.
 * Inputs/output are documented in commands.h.
 */
BOOL
on_command_run(const on_command_handler_t *table, size_t n, id target, id sender)
{
    const on_command_handler_t *handler = handler_for(table, n, sender);
    if (handler == NULL) return NO;
    handler->run(target, sender);
    return YES;
}

/*
 * on_command_validate — validation through a handler table.
 * Inputs/output are documented in commands.h.
 */
BOOL
on_command_validate(const on_command_handler_t *table, size_t n, id target, id item,
                    BOOL *handled)
{
    const on_command_handler_t *handler = handler_for(table, n, item);
    if (handled) *handled = handler != NULL;
    if (handler == NULL) return NO;
    return handler->validate == NULL || handler->validate(target, item);
}
