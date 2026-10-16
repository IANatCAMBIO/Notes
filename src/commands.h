#import <Cocoa/Cocoa.h>

/*
 * commands — THE table of the app's commands, and the one way they are
 * dispatched and validated.
 *
 * A command is a stable identifier (the GTK build's action name:
 * "new-note", "toggle-sidebar", …) with a title, a keyboard shortcut and,
 * for toolbar commands, a glyph (an SF Symbol).  Menu items, toolbar
 * items and context menu items are all made FROM this table
 * (on_command_menu_item, on_command_toolbar_item), so a command's wording
 * and shortcut are written once — the table in commands.m is the app's
 * keyboard map.
 *
 * WHO RUNS A COMMAND.  Each command has a scope:
 *
 *   app     the command works from whichever window is in front (New Note,
 *           Settings…, Search Notes…).  Its items target the app delegate,
 *           which hands it to the library.
 *   window  the command belongs to the window in front (Find…, Bold, Move
 *           to Trash).  Its items have no target: AppKit sends
 *           run_command: up the responder chain, where the key window's
 *           controller picks it up.  The same shortcut can therefore mean
 *           different things in the library and in an editor (⌘F), and an
 *           item whose window does not handle the command is disabled.
 *
 * A module that implements commands declares a static table of
 * on_command_handler_t and either registers it with the library
 * (library_state.h: on_library_add_commands) or, for an editor window,
 * passes it to on_command_run / on_command_validate from its own
 * run_command: and validateUserInterfaceItem:.  A command nobody has
 * registered a handler for is simply disabled — which is how the app runs
 * while packages are still being written.
 *
 * The standard Edit items (Undo, Redo, Cut, Copy, Paste, Select All) are
 * NOT commands: they use AppKit's own selectors with no target, so they
 * work in every text field as well as in the note view.
 */

/* Where a command's menu and toolbar items send it. */
typedef NS_ENUM(NSInteger, NotesCommandScope) {
    NotesCommandScopeApp,
    NotesCommandScopeWindow,
};

/* One command.  A row of the table in commands.m. */
typedef struct {
    NSString * __unsafe_unretained ident;   /* stable id, e.g. @"new-note"            */
    NSString * __unsafe_unretained title;   /* menu title; also the toolbar label     */
    NSString * __unsafe_unretained key;     /* key equivalent; @"" = none             */
    NSEventModifierFlags           mods;    /* the shortcut's modifiers               */
    NotesCommandScope              scope;
    NSString * __unsafe_unretained icon;    /* SF Symbol of its toolbar item
                                               (ui_util.h: on_ui_symbol);
                                               nil = not a toolbar command            */
    NSString * __unsafe_unretained tooltip; /* the tooltip of its toolbar item or
                                               format-bar button, saying what it does
                                               (GTK's); nil = the title               */
} on_command_t;

/* One command's implementation, in the module that owns it.
 *   run      — do it.  `target` is the object the dispatcher passes: the
 *              NotesLibrary for library commands, the editor's window
 *              controller for editor commands.  `sender` is the item.
 *   validate — may the command run now?  It may also adjust the item for
 *              the current state with on_command_item_update (a checkmark,
 *              "Hide Sidebar" vs "Show Sidebar", the list/grid icon).
 *              NULL = always enabled. */
typedef struct {
    NSString * __unsafe_unretained ident;
    void (*run)(id target, id sender);
    BOOL (*validate)(id target, id item);
} on_command_handler_t;

/*
 * on_command_menu_item — a menu item for a command: title, shortcut,
 * target and action per its scope.  For the main menu and context menus
 * alike.
 *
 * Inputs:
 *   ident — the command's identifier (must be in the table)
 *
 * Output: a new menu item.
 */
NSMenuItem *on_command_menu_item(NSString *ident);

/*
 * on_command_tooltip — the hover help of a control that runs a command (a
 * toolbar item, a format-bar button): its tip, else its title.
 *
 * Inputs:
 *   ident — the command's identifier (must be in the table)
 *
 * Output: the text.
 */
NSString *on_command_tooltip(NSString *ident);

/*
 * on_command_toolbar_item — a toolbar item for a command: its glyph (an SF
 * Symbol template, sized by the toolbar), its title as label and tooltip,
 * target and action per its scope.  The item's identifier is the
 * command's.
 *
 * Inputs:
 *   ident — the command's identifier (must be in the table, with an icon)
 *
 * Output: a new toolbar item.
 */
NSToolbarItem *on_command_toolbar_item(NSString *ident);

/*
 * on_command_ident — which command an item stands for.
 *
 * Inputs:
 *   item — a menu item or toolbar item made by the two functions above,
 *          or a control (a button in a window's own bar) whose identifier
 *          is set to the command's
 *
 * Output: the command's identifier, or nil for any other object.
 */
NSString *on_command_ident(id item);

/*
 * on_command_item_update — show a command's current state on its item,
 * whichever kind of item it is.  For validate functions.
 *
 * Inputs:
 *   item    — the menu or toolbar item being validated
 *   title   — a title for this state; nil keeps the table's
 *   icon    — an SF Symbol name for this state (toolbar items); nil keeps
 *             the table's
 *   checked — YES to show a checkmark (menu items)
 */
void on_command_item_update(id item, NSString *title, NSString *icon, BOOL checked);

/*
 * on_command_run — run the command an item stands for, if a table has it.
 *
 * Inputs:
 *   table  — a module's handlers
 *   n      — how many
 *   target — passed to the handler
 *   sender — the item
 *
 * Output: YES when a handler was found and run.
 */
BOOL on_command_run(const on_command_handler_t *table, size_t n, id target, id sender);

/*
 * on_command_validate — whether an item's command can run now, per a table;
 * the handler's validate function may also update the item.
 *
 * Inputs:
 *   table   — a module's handlers
 *   n       — how many
 *   target  — passed to the handler
 *   item    — the item being validated
 *   handled — receives YES when the table has the item's command; may be
 *             NULL
 *
 * Output: YES when the command is in the table and enabled.
 */
BOOL on_command_validate(const on_command_handler_t *table, size_t n, id target, id item,
                         BOOL *handled);
