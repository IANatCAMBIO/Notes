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
    { @"about",            @"About " ON_APP_NAME,      @"",  0,         app, nil, nil },
    { @"preferences",      @"Settings…",               @",", cmd,       app, @"gearshape",
      @"Open the settings window" },
    { @"new-note",         @"New Note",                @"n", cmd,       app, @"plus",
      @"Create a note with the selected tag" },
    { @"quicknote",        @"Quick Note",              @"",  0,         app, @"square.and.pencil",
      @"Create a note with no tag" },
    { @"open-db",          @"Open Database File…",     @"o", cmd,       app, nil, nil },
    { @"export-html",      @"Export All as HTML…",     @"",  0,         app, nil, nil },
    { @"export-md",        @"Export All as Markdown…", @"",  0,         app, nil, nil },
    { @"view-list",        @"as List",                 @"1", cmd,       app, nil, nil },
    { @"view-grid",        @"as Grid",                 @"2", cmd,       app, nil, nil },
    { @"toggle-view",      @"List or Grid",            @"",  0,         app, @"square.grid.2x2",
      @"Switch between the list and the grid" },
    { @"toggle-sidebar",   @"Hide Sidebar",            @"s", cmd_ctrl,  app, @"sidebar.left",
      @"Show or hide the sidebar" },
    { @"toggle-favorites", @"Hide Favorites Bar",      @"",  0,         app, nil, nil },
    { @"toggle-done",      @"Hide Completed",          @"",  0,         app, @"eye.slash",
      @"Show or hide completed action items" },
    { @"media",            @"Media…",                  @"i", cmd_shift, app, @"photo.on.rectangle",
      @"Show every image in the listed notes as thumbnails" },
    { @"search",           @"Search Notes…",           @"f", cmd_shift, app, @"magnifyingglass",
      @"Open search window" },
    { @"ai-summary",       @"AI Summary",              @"",  0,         app, @"cpu",
      @"Summarize notes with AI" },
    { @"show-library",     @"Library",                 @"0", cmd,       app, nil, nil },

    /* ── Window scope, library ── */
    { @"delete-note",      @"Move to Trash",           KEY_BACKSPACE, cmd, win, @"trash",
      @"Move the selected notes to the Trash" },
    { @"empty-trash",      @"Empty Trash…",            @"",  0,         win, nil, nil },
    { @"edit-tag",         @"Edit Tag…",               @"",  0,         win, nil, nil },
    { @"search-here",      @"Search Here…",            @"",  0,         win, nil, nil },
    { @"note-pin",         @"Pin",                     @"",  0,         win, nil, nil },
    { @"note-restore",     @"Restore",                 @"",  0,         win, nil, nil },
    { @"note-export-html", @"Export as HTML…",         @"",  0,         win, nil, nil },
    { @"note-export-md",   @"Export as Markdown…",     @"",  0,         win, nil, nil },

    /* ── Window scope, both windows: the library searches, an editor finds ── */
    { @"find",             @"Find…",                   @"f", cmd,       win, nil, nil },

    /* ── Window scope, editor ── */
    { @"find-next",        @"Find Next",               @"g", cmd,       win, nil,
      @"Next match (Return)" },
    { @"find-previous",    @"Find Previous",           @"g", cmd_shift, win, nil,
      @"Previous match" },
    { @"find-selection",   @"Use Selection for Find",  @"e", cmd,       win, nil, nil },
    { @"inline-bold",      @"Bold",                    @"b", cmd,       win, nil, nil },
    { @"inline-italic",    @"Italic",                  @"i", cmd,       win, nil, nil },
    { @"inline-underline", @"Underline",               @"u", cmd,       win, nil, nil },
    { @"inline-strike",    @"Strikethrough",           @"x", cmd_shift, win, nil, nil },
    { @"para-h1",          @"Heading 1",               @"",  0,         win, nil, nil },
    { @"para-h2",          @"Heading 2",               @"",  0,         win, nil, nil },
    { @"para-body",        @"Body",                    @"",  0,         win, nil,
      @"Plain body text" },
    { @"para-bullet",      @"Bulleted List",           @"",  0,         win, nil, nil },
    { @"para-number",      @"Numbered List",           @"",  0,         win, nil, nil },
    { @"para-check",       @"Task List",               @"",  0,         win, nil,
      @"Task list with checkboxes (click a box to toggle)" },
    { @"para-code",        @"Code Block",              @"m", cmd_shift, win, nil, nil },
    { @"insert-image",     @"Image…",                  @"",  0,         win, nil, nil },
    { @"insert-table",     @"Table",                   @"t", cmd_opt,   win, nil, nil },
    { @"insert-emoji",     @"Emoji…",                  @"",  0,         win, nil, nil },
    { @"insert-date",      @"Date",                    @"d", cmd,       win, nil, nil },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

/*
 * command_find — a command's row.
 *
 * Inputs:
 *   ident — the command's identifier
 *
 * Output: the row, or NULL for an unknown identifier.
 */
static const on_command_t *
command_find(NSString *ident)
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
    const on_command_t *command = command_find(ident);
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
 * on_command_tooltip — see commands.h.
 */
NSString *
on_command_tooltip(NSString *ident)
{
    const on_command_t *command = command_find(ident);
    NSCAssert(command != NULL, @"unknown command %@", ident);
    return command->tooltip ?: command->title;
}

/*
 * on_command_toolbar_item — toolbar item from the table.
 * Inputs/output are documented in commands.h.
 */
NSToolbarItem *
on_command_toolbar_item(NSString *ident)
{
    const on_command_t *command = command_find(ident);
    NSCAssert(command != NULL && command->icon != nil, @"no toolbar command %@", ident);
    id  target = nil;
    SEL action = command_action(command, &target);
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:command->ident];
    item.label    = command->title;
    item.toolTip  = on_command_tooltip(ident);
    item.image    = shared_ui_symbol(command->icon, 0.0);
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
    const on_command_t *command = command_find(on_command_ident(item));
    if (command == NULL) return;
    NSString *shown = title ?: command->title;
    if ([item isKindOfClass:NSMenuItem.class]) {
        NSMenuItem *menu_item = item;
        menu_item.title = shown;
        menu_item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    } else if ([item isKindOfClass:NSToolbarItem.class]) {
        NSToolbarItem *toolbar_item = item;
        NSString *tip  = title ?: command->tooltip ?: command->title;
        NSImage *image = shared_ui_symbol(icon ?: command->icon, 0.0);
        /* Only what differs is set: AppKit validates after every event, and
         * an item given an image (even the same one) is redrawn.  Glyphs
         * are cached, so an unchanged one is the same object. */
        if (![toolbar_item.label isEqualToString:shown]) toolbar_item.label = shown;
        if (![toolbar_item.toolTip isEqualToString:tip]) toolbar_item.toolTip = tip;
        if (toolbar_item.image != image) toolbar_item.image = image;
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
