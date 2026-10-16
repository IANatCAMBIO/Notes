#import "app_delegate.h"
#import "commands.h"
#import "editor_window.h"
#import "library_ai.h"
#import "library_commands.h"
#import "library_notes.h"
#import "library_state.h"
#import "library_window.h"
#import "media_window.h"
#import "search_window.h"
#import "settings_window.h"
#import "ui_util.h"

#include "action_sync.h"
#include "app_lifecycle.h"
#include "identity.h"

#include <signal.h>

/* Info.plist key the Makefile writes the build time under (UTC, ISO 8601). */
static NSString * const build_date_key = @"NotesBuildDate";

/* A separator in the menu layouts below. */
static NSString * const menu_rule = @"-";

@interface NotesAppDelegate () <NSUserInterfaceValidations>
@property (nonatomic, strong) NotesLibrary *lib;
@property (nonatomic, copy) NSArray<dispatch_source_t> *signal_sources;  /* live for the process */
@end

/*
 * hook_show_notice — OnApp.show_notice: the core has something the user
 * must see (the integrity check found damage).
 *
 * Inputs:
 *   app     — the application context (unused)
 *   title   — the headline
 *   message — the detail
 */
static void
hook_show_notice(OnApp *app, const gchar *title, const gchar *message)
{
    shared_ui_notice(on_library().window, @(title), @(message), nil);
}

/* Links in the About panel's credits: the author's site and the license
 * (as the Files and Tasks apps have them; GTK: author and BSD license). */

/*
 * run_about — "about": the standard About panel, as Files and Tasks show
 * it.  Its version line reads "Version <VERSION> (built <date>)" from
 * Info.plist; the credits say which database is open and what it holds,
 * then "camb.io · BSD License", each a link.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_about(id target, id sender)
{
    NotesLibrary *lib = target;
    OnDatabase   *db  = lib.app->db;
    gint notes = 0, tags = 0;
    on_db_totals(db, &notes, &tags);
    NSNumber *size = [NSFileManager.defaultManager attributesOfItemAtPath:@(db->path)
                                                                    error:nil][NSFileSize];
    shared_ui_show_about([NSString stringWithFormat:@"%@\n%d notes, %d tags — %@",
                          @(db->path), notes, tags, shared_ui_format_bytes(size.longLongValue)],
                         on_ui_app_icon(), build_date_key);
}

/*
 * run_show_library — "show-library": the library window, to the front.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item (unused)
 */
static void
run_show_library(id target, id sender)
{
    on_library_window_show(target);
}

/*
 * raise_running_copy — this copy found another one serving the same data
 * folder: bring that one to the front (GTK: a second launch activates the
 * first).
 *
 * Output: YES when another running copy of this app was found and asked to
 *         come forward.
 */
static BOOL
raise_running_copy(void)
{
    pid_t me = NSProcessInfo.processInfo.processIdentifier;
    for (NSRunningApplication *other in
         [NSRunningApplication runningApplicationsWithBundleIdentifier:@ON_BUNDLE_ID]) {
        if (other.processIdentifier == me) continue;
        [other activateWithOptions:NSApplicationActivateAllWindows];
        return YES;
    }
    return NO;
}

/* The app delegate's own commands.  Handlers receive the NotesLibrary. */
static const shared_command_handler_t handlers[] = {
    { @"about",        run_about,        NULL },
    { @"show-library", run_show_library, NULL },
};

@implementation NotesAppDelegate {
    OnApp _app;                      /* THE application context; zeroed by alloc */
}

/* ── Launch ──────────────────────────────────────────────────────────────── */

/*
 * applicationWillFinishLaunching: — set the running app's icon.  macOS 26
 * draws a bundle icon that is not its rounded-square shape on a grey
 * backdrop; the Dock tile of the running app, alerts and the About panel
 * are handed the artwork itself instead (ui_util.h: on_ui_app_icon).
 *
 * Inputs:
 *   n — NSApplicationWillFinishLaunchingNotification (unused)
 */
- (void)
applicationWillFinishLaunching:(NSNotification *)n
{
    if (on_ui_app_icon()) NSApp.applicationIconImage = on_ui_app_icon();
}

/*
 * applicationDidFinishLaunching: — steps 3 to 7 of the launch sequence in
 * app_lifecycle.h: preferences, hooks, the database (asking first when
 * there is none), the library window, the services.
 *
 * Inputs:
 *   n — NSApplicationDidFinishLaunchingNotification (unused)
 */
- (void)
applicationDidFinishLaunching:(NSNotification *)n
{
    on_commands_install();
    [self build_menu_bar];
    [self watch_signals];

    /* First of all: is this the one copy for this data folder?  A second
     * copy never opens the database, nor shows a window to type into.     */
    if (on_app_lock_take() == on_app_lock_busy) {
        if (raise_running_copy()) {
            [NSApp terminate:nil];       /* the running copy is in front now */
        } else {
            [NSApp activate];
            shared_ui_notice(nil, @ON_APP_NAME @" is already running", @"Another copy is using this data folder. This one will quit.", ^{ [NSApp terminate:nil]; });
        }
        return;
    }

    on_app_prefs_load(&_app);
    _app.show_notice = hook_show_notice;

    gchar *why      = NULL;
    char  *data_dir = on_paths_data_dir();
    gchar *db_path  = on_app_db_path(&why);
    NSLog(@"[launch] data directory %@, database %@", @(data_dir), db_path ? @(db_path) : @"(none)");
    BOOL db_exists = db_path != NULL && [NSFileManager.defaultManager fileExistsAtPath:@(db_path)];
    free(data_dir);
    g_free(db_path);

    [NSApp activate];
    if (why != NULL) {
        shared_ui_notice(nil, @"Could not find the shared database", @(why), ^{ [NSApp terminate:nil]; });
        g_free(why);
    } else if (db_exists) {
        [self open_database_and_start];
    } else {
        [self ask_first_run];
    }
    if (on_app_config_problem() != NULL) {
        shared_ui_notice(nil, @"Settings could not be read", @(on_app_config_problem()), nil);
    }
}

/*
 * ask_first_run — no database exists where one is expected: ask whether to
 * open an existing shared database or create a new one there, instead of
 * silently creating an empty one.  (Notes and Tasks share one file; a
 * user whose file lives elsewhere usually means to OPEN it — creating a
 * fresh one would look like every note had vanished.)  Cancelling the file
 * panel returns here.
 */
- (void)
ask_first_run
{
    gchar    *expected = on_app_db_path(NULL);
    NSString *message  = [NSString stringWithFormat:
        @"No database was found at\n%@\n\n" ON_APP_NAME @" keeps your notes in the database "
        @"it shares with Tasks. Create a new one there, or open an existing shared database "
        @"file, which then becomes the default for both apps.", expected ? @(expected) : @"?"];
    g_free(expected);

    [SharedToolPanel show_message:@"Welcome to " ON_APP_NAME message:message
                          symbol:@"note.text"
                         buttons:@[@"Create a New Database", @"Open a Database File…", @"Quit"]
                     destructive:-1 over:nil
                         handler:^(NSInteger index) {
        if (index == 0) {
            [self open_database_and_start];
        } else if (index == 1) {
            shared_ui_pick_path(SharedPickOpenFile, @"Choose the shared database file to open", @"Open",
                            @[[UTType typeWithFilenameExtension:@"db"]], nil,
                            ^(NSString *path) { [self first_run_picked:path]; });
        } else {
            [NSApp terminate:nil];   /* Quit, or the panel dismissed */
        }
    }];
}

/*
 * first_run_picked: — the first-run file panel closed.  The chosen file is
 * tried first (the shared module refuses anything that is not a shared
 * database, untouched) and made the default of both apps only if it opens:
 * a file that cannot be opened must not become the configured one.
 *
 * Inputs:
 *   path — the chosen file, or nil when the panel was cancelled
 */
- (void)
first_run_picked:(NSString *)path
{
    if (!path) {
        [self ask_first_run];
        return;
    }
    gchar      *why   = NULL;
    OnDatabase *trial = on_db_open_existing(path.fileSystemRepresentation, &why);
    char       *set_err = NULL;
    if (trial != NULL) {
        on_db_close(trial);
        if (shared_db_set_default_path(path.fileSystemRepresentation, &set_err) == 0) {
            [self open_database_and_start];
            return;
        }
    }
    NSString *reason = why ? @(why) : set_err ? @(set_err) : @"Unknown error.";
    g_free(why);
    free(set_err);
    /* The welcome panel comes back once this one is answered. */
    shared_ui_notice(nil, @"That file cannot be opened", reason, ^{ [self ask_first_run]; });
}

/*
 * open_database_and_start — open the database where it is expected
 * (creating it when the user chose to) and bring the app up: hooks,
 * library, window, services.  A database that cannot be opened ends the
 * launch with a message — there is deliberately no fallback to another
 * location.
 *
 * Output: YES when the database opened; NO when the app is terminating.
 */
- (BOOL)
open_database_and_start
{
    gchar *why = NULL;
    if (!on_app_open_db(&_app, &why)) {
        shared_ui_notice(nil, @"Could not open the database", why ? @(why) : @"Unknown error.", ^{ [NSApp terminate:nil]; });
        g_free(why);
        return NO;
    }

    /* Every module installs its own hooks and commands, once. */
    self.lib = on_library_create(&_app);
    on_library_add_commands(handlers, sizeof handlers / sizeof handlers[0]);
    on_editor_install(&_app);
    on_library_window_install(self.lib);
    on_library_commands_install(self.lib);
    on_library_notes_install(self.lib);
    on_settings_install(self.lib);
    on_search_install(self.lib);
    on_media_install(self.lib);
    on_library_ai_install(self.lib);

    on_library_window_show(self.lib);

    on_app_start_services(&_app);
    return YES;
}

/* ── Quit ────────────────────────────────────────────────────────────────── */

/*
 * watch_signals — turn SIGTERM (make install, pkill, logout), SIGINT
 * (Ctrl-C on `make run`) and SIGHUP (its terminal closing) into an ordinary
 * Quit, so they go through applicationShouldTerminate: and every editor is
 * flushed.  The default action — instant death — would lose the edits typed
 * since the last autosave.  The sources live for the whole process and are
 * never cancelled.
 */
- (void)
watch_signals
{
    static const int signals[] = { SIGTERM, SIGINT, SIGHUP };
    NSMutableArray<dispatch_source_t> *sources = [NSMutableArray array];
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        signal(signals[i], SIG_IGN);     /* the dispatch source sees it instead */
        dispatch_source_t source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signals[i], 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(source, ^{
            [NSApp terminate:nil];
        });
        dispatch_resume(source);
        [sources addObject:source];
    }
    self.signal_sources = sources;
}

/*
 * applicationShouldTerminate: — THE way out (Quit, the Dock, logout,
 * SIGTERM, SIGINT and SIGHUP all arrive here): on_app_shutdown closes every
 * editor — each saving its edits first — closes the database and lets go
 * of the app lock.  See
 * "the flush-on-quit contract" in app_lifecycle.h.
 *
 * Inputs:
 *   sender — NSApplication (unused)
 *
 * Output: NSTerminateNow, once everything is on disk; NSTerminateCancel
 *         (with a tool panel) when a note could not be saved.
 */
- (NSApplicationTerminateReply)
applicationShouldTerminate:(NSApplication *)sender
{
    if (!on_app_shutdown(&_app)) {
        on_editor_report_unsaved(@"Could not quit", nil);
        return NSTerminateCancel;
    }
    NSLog(@"[quit] editors flushed, database closed");
    return NSTerminateNow;
}

/*
 * applicationDidBecomeActive: — coming back to the app is when another
 * app's edits are most likely waiting: look for them now rather than at
 * the next poll (action_sync.h).
 *
 * Inputs:
 *   n — NSApplicationDidBecomeActiveNotification (unused)
 */
- (void)
applicationDidBecomeActive:(NSNotification *)n
{
    on_action_sync_poll(&_app);
}

/*
 * applicationShouldTerminateAfterLastWindowClosed: — return NO so the app
 * keeps running after the library window closes while editors are open.
 * The Dock-icon handler below re-shows the library when needed.
 *
 * Inputs:
 *   sender — NSApplication (unused)
 *
 * Output: NO.
 */
- (BOOL)
applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    return NO;
}

/*
 * applicationShouldHandleReopen:hasVisibleWindows: — clicking the Dock icon
 * brings the library back if it was closed (an editor may be the only
 * window open).
 *
 * Inputs:
 *   sender  — NSApplication (unused)
 *   visible — whether any window is visible (unused: an editor counts)
 *
 * Output: YES, so AppKit also does its own handling (un-minimising).
 */
- (BOOL)
applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible
{
    if (self.lib) on_library_window_show(self.lib);
    return YES;
}

/* ── Commands ────────────────────────────────────────────────────────────── */

/*
 * run_app_command: — an app-scope command (commands.h): it acts on the
 * library whichever window is in front.
 *
 * Inputs:
 *   sender — the menu or toolbar item
 */
- (void)
run_app_command:(id)sender
{
    on_library_run_command(self.lib, sender);
}

/*
 * validateUserInterfaceItem: — an app-scope command's item is enabled only
 * while some module has a handler for it that says it can run.
 *
 * Inputs:
 *   item — the menu or toolbar item
 *
 * Output: YES to enable it.
 */
- (BOOL)
validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (item.action == @selector(run_app_command:)) {
        return self.lib != nil && on_library_validate_command(self.lib, item);
    }
    return YES;
}

/*
 * show_help: — Help ▸ Notes Help: open the README that ships in the
 * bundle.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
show_help:(id)sender
{
    NSURL *readme = [NSBundle.mainBundle URLForResource:@"README" withExtension:@"md"];
    if (readme) [NSWorkspace.sharedWorkspace openURL:readme];
}

/* ── Menu bar ────────────────────────────────────────────────────────────── */

/*
 * make_item — a menu item that sends a standard AppKit action up the
 * responder chain.
 *
 * Inputs:
 *   title  — the item's title
 *   action — the selector
 *   key    — key equivalent; @"" for none
 *   mods   — the shortcut's modifiers
 *
 * Output: the item.
 */
static NSMenuItem *
make_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = mods;
    return item;
}

/*
 * add_commands — append command items (commands.h) to a menu.
 *
 * Inputs:
 *   menu   — the menu
 *   idents — command identifiers in order; menu_rule for a separator
 */
static void
add_commands(NSMenu *menu, NSArray<NSString *> *idents)
{
    for (NSString *ident in idents) {
        [menu addItem:[ident isEqualToString:menu_rule] ? NSMenuItem.separatorItem
                                                        : shared_command_menu_item(ident, nil)];
    }
}

/*
 * add_menu — append a titled top-level menu to the menu bar.
 *
 * Inputs:
 *   bar   — the menu bar
 *   title — the menu's title
 *
 * Output: the new, empty menu.
 */
static NSMenu *
add_menu(NSMenu *bar, NSString *title)
{
    NSMenuItem *holder = [[NSMenuItem alloc] init];
    NSMenu     *menu   = [[NSMenu alloc] initWithTitle:title];
    holder.submenu     = menu;
    [bar addItem:holder];
    return menu;
}

/*
 * build_menu_bar — the whole main menu, in code: the standard macOS menus
 * (App, File, Edit, Format, View, Window, Help).  Command items come from
 * the command table; the Edit menu's Undo / Cut / Copy / Paste / Select All
 * use AppKit's own selectors with no target, so they work in every text
 * field as well as in the note view.
 */
- (void)
build_menu_bar
{
    NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSMenu *bar = [[NSMenu alloc] init];

    /* ── App menu ── */
    NSMenu *app_menu = add_menu(bar, @ON_APP_NAME);
    add_commands(app_menu, @[@"about", menu_rule, @"preferences", menu_rule]);
    NSMenuItem *services = make_item(@"Services", NULL, @"", 0);
    services.submenu     = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu   = services.submenu;
    [app_menu addItem:services];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:make_item(@"Hide " ON_APP_NAME, @selector(hide:), @"h", cmd)];
    [app_menu addItem:make_item(@"Hide Others", @selector(hideOtherApplications:), @"h",
                                cmd | NSEventModifierFlagOption)];
    [app_menu addItem:make_item(@"Show All", @selector(unhideAllApplications:), @"", 0)];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItem:make_item(@"Quit " ON_APP_NAME, @selector(terminate:), @"q", cmd)];

    /* ── File menu ── */
    NSMenu *file_menu = add_menu(bar, @"File");
    add_commands(file_menu, @[
        @"new-note", @"quicknote", menu_rule,
        @"open-db", menu_rule,
        @"export-html", @"export-md", @"note-export-html", @"note-export-md", menu_rule,
        @"edit-tag", menu_rule,
        @"delete-note", @"note-restore", @"empty-trash",
        menu_rule,
    ]);
    [file_menu addItem:make_item(@"Close Window", @selector(performClose:), @"w", cmd)];

    /* ── Edit menu ── */
    NSMenu *edit_menu = add_menu(bar, @"Edit");
    [edit_menu addItem:make_item(@"Undo", @selector(undo:), @"z", cmd)];
    [edit_menu addItem:make_item(@"Redo", @selector(redo:), @"z", cmd | NSEventModifierFlagShift)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    [edit_menu addItem:make_item(@"Cut", @selector(cut:), @"x", cmd)];
    [edit_menu addItem:make_item(@"Copy", @selector(copy:), @"c", cmd)];
    [edit_menu addItem:make_item(@"Paste", @selector(paste:), @"v", cmd)];
    [edit_menu addItem:make_item(@"Select All", @selector(selectAll:), @"a", cmd)];
    [edit_menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *find = make_item(@"Find", NULL, @"", 0);
    find.submenu     = [[NSMenu alloc] initWithTitle:@"Find"];
    add_commands(find.submenu, @[
        @"find", @"find-next", @"find-previous", @"find-selection", menu_rule, @"search",
    ]);
    [edit_menu addItem:find];

    /* ── Format menu ── */
    NSMenu *format_menu = add_menu(bar, @"Format");
    add_commands(format_menu, @[
        @"inline-bold", @"inline-italic", @"inline-underline", @"inline-strike", menu_rule,
        @"para-h1", @"para-h2", @"para-body", menu_rule,
        @"para-bullet", @"para-number", @"para-check", @"para-code", menu_rule,
    ]);
    NSMenuItem *insert = make_item(@"Insert", NULL, @"", 0);
    insert.submenu     = [[NSMenu alloc] initWithTitle:@"Insert"];
    add_commands(insert.submenu, @[@"insert-image", @"insert-table", @"insert-emoji", @"insert-date"]);
    [format_menu addItem:insert];

    /* ── View menu ── */
    NSMenu *view_menu = add_menu(bar, @"View");
    add_commands(view_menu, @[
        @"view-list", @"view-grid", menu_rule,
        @"toggle-sidebar", @"toggle-favorites", @"toggle-done", menu_rule,
        @"media", @"ai-summary", menu_rule,
    ]);
    [view_menu addItem:make_item(@"Enter Full Screen", @selector(toggleFullScreen:), @"f",
                                 cmd | NSEventModifierFlagControl)];

    /* ── Window menu ── */
    NSMenu *window_menu = add_menu(bar, @"Window");
    [window_menu addItem:make_item(@"Minimize", @selector(performMiniaturize:), @"m", cmd)];
    [window_menu addItem:make_item(@"Zoom", @selector(performZoom:), @"", 0)];
    add_commands(window_menu, @[menu_rule, @"show-library", menu_rule]);
    [window_menu addItem:make_item(@"Bring All to Front", @selector(arrangeInFront:), @"", 0)];
    NSApp.windowsMenu = window_menu;

    /* ── Help menu ── */
    NSMenu *help_menu = add_menu(bar, @"Help");
    NSMenuItem *help  = make_item(@ON_APP_NAME @" Help", @selector(show_help:), @"?", cmd);
    help.target       = self;
    [help_menu addItem:help];
    NSApp.helpMenu = help_menu;

    NSApp.mainMenu = bar;
}

@end
