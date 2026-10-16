#import "shared_commands.h"

/*
 * commands — THE table of the app's commands (commands.m), installed into
 * shared_commands.h at launch, which is the one way they are dispatched
 * and validated: menu items, toolbar items and context menu items are all
 * made FROM the table (shared_command_menu_item, shared_command_toolbar_item),
 * so a command's wording and shortcut are written once — the table is the
 * app's keyboard map.
 *
 * Identifiers are the GTK build's action names ("new-note",
 * "toggle-sidebar", …).  App-scope commands act on the library whichever
 * window is in front (the app delegate hands them to
 * on_library_run_command); window-scope ones reach the key window's
 * controller through the responder chain, so ⌘F searches in the library
 * and finds in an editor.  A module implements its commands in a
 * shared_command_handler_t table registered with the library
 * (library_state.h: on_library_add_commands) or, for an editor window,
 * run from its own run_command: and validateUserInterfaceItem:.  A
 * command nobody has registered a handler for is simply disabled.
 */

/*
 * on_commands_install — install the table.  Once, at launch, before any
 * item is made from it.
 */
void on_commands_install(void);
