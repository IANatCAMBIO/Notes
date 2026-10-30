#import "commands.h"

#include "identity.h"

/* Shorthand for the table's modifier column. */
enum {
    cmd       = NSEventModifierFlagCommand,
    cmd_shift = NSEventModifierFlagCommand | NSEventModifierFlagShift,
    cmd_ctrl  = NSEventModifierFlagCommand | NSEventModifierFlagControl,
    cmd_opt   = NSEventModifierFlagCommand | NSEventModifierFlagOption,
    cmd_ctrl_opt = NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption,
    app       = SharedCommandScopeApp,
    win       = SharedCommandScopeWindow,
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
static const shared_command_t commands[] = {
    /* ── App scope: works from any window, acts on the library ── */
    { @"about",            @"About " ON_APP_NAME,      @"",  0,         app, nil, nil },
    { @"preferences",      @"Settings…",               @",", cmd,       app, @"gearshape",
      @"Open the settings window" },
    { @"new-note",         @"New Note",                @"n", cmd_ctrl_opt, app, @"plus",
      @"Create a note, with the selected tag" },
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
    { @"toggle-day-planner", @"Hide Day Planner",      @"d", cmd_shift, app, @"calendar.badge.clock",
      @"Show or hide the day planner" },
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
    { @"add-favorite",     @"Add to Favorites",        @"",  0,         win, nil, nil },
    { @"remove-favorite",  @"Remove from Favorites",   @"",  0,         win, nil, nil },
    { @"clear-tag",        @"Clear Tag from All Notes",    @"",  0,         win, nil, nil },
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
    { @"keep-on-top",      @"Keep on Top",             @"",  0,         win, nil,
      @"Keep this window on top" },
    { @"meeting-summary",  @"Summarize Last Meeting",  @"",  0,         win, @"clock.arrow.circlepath",
      @"Summarize the previous meeting: the most recent earlier note with this title" },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

/*
 * on_commands_install — see commands.h.
 */
void
on_commands_install(void)
{
    shared_commands_install(commands, COMMAND_COUNT);
}
