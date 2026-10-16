#import "library_window.h"
#import "library_ai.h"
#import "library_favorites.h"
#import "library_notes.h"
#import "library_sidebar.h"
#import "search_window.h"
#import "ui_util.h"

#include "identity.h"
#include "look.h"

/* The toolbar's own items, beside the commands'. */
static NSToolbarItemIdentifier const search_item_ident = @"notes.search-field";

/*
 * toolbar_idents — the toolbar, left to right: command identifiers
 * (commands.h), spaces and the search field.  AI Summary, and the space
 * that sets it apart, are there only while AI features are enabled (GTK:
 * the button is hidden, not just disabled).
 *
 * Inputs:
 *   ai — YES to include AI Summary
 *
 * Output: the item identifiers.
 */
static NSArray<NSToolbarItemIdentifier> *
toolbar_idents(BOOL ai)
{
    NSArray<NSToolbarItemIdentifier> *head = @[
        @"toggle-sidebar", NSToolbarSpaceItemIdentifier,
        @"quicknote", @"new-note", @"delete-note", NSToolbarSpaceItemIdentifier,
        @"toggle-view", @"toggle-done", @"search", @"media", @"preferences",
    ];
    NSArray<NSToolbarItemIdentifier> *tail = @[NSToolbarFlexibleSpaceItemIdentifier, search_item_ident];
    return [[head arrayByAddingObjectsFromArray:ai ? @[NSToolbarSpaceItemIdentifier, @"ai-summary"] : @[]]
               arrayByAddingObjectsFromArray:tail];
}

/* The settings the sidebar's width and visibility are kept in, as Tasks
 * keeps them (sidebar_width, sidebar_visible). */
static const char *const sidebar_width_key   = "sidebar_width";
static const char *const sidebar_visible_key = "sidebar_visible";

/*
 * NotesSplitController — the split view controller for sidebar | notes,
 * which also remembers the sidebar's width: every time the divider moves
 * while the sidebar shows (a drag, a fit to content), the width is saved
 * for the next launch.
 */
@interface NotesSplitController : NSSplitViewController
@end

@implementation NotesSplitController

/*
 * splitViewDidResizeSubviews: — the divider moved: save the sidebar's
 * width, unless the sidebar is collapsed (its width is then no width).
 *
 * Inputs:
 *   note — the notification
 */
- (void)
splitViewDidResizeSubviews:(NSNotification *)note
{
    [super splitViewDidResizeSubviews:note];
    NSSplitViewItem *sidebar = self.splitViewItems.firstObject;
    if (sidebar.collapsed) return;
    gint width = (gint)round(NSWidth(sidebar.viewController.view.frame));
    if (width >= on_look_sidebar_min_w) on_app_config_set_int(sidebar_width_key, width);
}

@end

@interface NotesLibraryWindow : NSWindowController <NSWindowDelegate, NSToolbarDelegate,
                                                    NSUserInterfaceValidations>
@property (nonatomic, strong) NotesLibrary *lib;
@property (nonatomic, strong) NSSplitViewController *split;   /* sidebar | notes */
@property (nonatomic, strong) NSSplitViewItem *sidebar_item;
@property (nonatomic, strong) NSView *favorites_bar;
@property (nonatomic, strong) NSLayoutConstraint *favorites_height;   /* 0 while hidden */
@property (nonatomic, strong) NSTextField *status_path;       /* left: the selection */
@property (nonatomic, strong) NSTextField *status_event;      /* right: the last event */
@property (nonatomic, assign) on_timer_id_t status_timer;     /* clears status_event; 0 = none */
@end

/* The open library window's controller, or nil.  THE owner (see OWNERSHIP
 * in library_window.h). */
static NotesLibraryWindow *library_window;

/*
 * pane_item — a split-view item around a pane's view.
 *
 * Inputs:
 *   view      — the pane
 *   min_width — the narrowest it may be dragged
 *
 * Output: the item.
 */
static NSSplitViewItem *
pane_item(NSView *view, CGFloat min_width)
{
    NSViewController *controller = [[NSViewController alloc] init];
    controller.view = view;
    NSSplitViewItem *item = [NSSplitViewItem splitViewItemWithViewController:controller];
    item.minimumThickness = min_width;
    return item;
}

/*
 * status_clear — the status timer's callback: the event message has been up
 * for on_look_status_hold: fade it out over on_look_status_fade, then clear
 * it (as Tasks' status bar does).  A newer message meanwhile is shown at
 * full strength by show_status:, which this must not then clear.
 *
 * Inputs:
 *   data — the NotesLibraryWindow, unretained: the controller removes the
 *          timer when its window closes
 */
static void
status_clear(void *data)
{
    NotesLibraryWindow *controller = (__bridge NotesLibraryWindow *)data;
    NSTextField        *label      = controller.status_event;
    NSString           *shown      = label.stringValue;
    controller.status_timer = 0;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = on_look_status_fade;
        label.animator.alphaValue = 0.0;
    } completionHandler:^{
        if (![label.stringValue isEqualToString:shown]) return;   /* replaced meanwhile */
        label.stringValue = @"";
        label.alphaValue  = 1.0;
    }];
}

@implementation NotesLibraryWindow

/*
 * init_with_library: — build the window: toolbar, panes, status bar.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the controller.
 */
- (instancetype)
init_with_library:(NotesLibrary *)lib
{
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, on_look_library_w, on_look_library_h)
                  styleMask:style
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title              = @ON_APP_NAME;
    win.minSize            = NSMakeSize(on_look_library_min_w, on_look_library_min_h);
    win.toolbarStyle       = NSWindowToolbarStyleExpanded;
    win.releasedWhenClosed = NO;
    [win center];
    [win setFrameAutosaveName:@"notes.library"];

    self = [super initWithWindow:win];
    if (!self) return nil;
    _lib         = lib;
    win.delegate = self;
    lib.window   = win;

    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"notes.library"];
    toolbar.delegate    = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    win.toolbar         = toolbar;

    [self build_content];
    on_ui_use_ui_font(win.contentView);      /* the sidebar keeps its own size */
    [self update_location];
    return self;
}

/*
 * build_content — the Favorites bar across the window, the split view
 * (sidebar | notes over the AI pane) under it, above the status bar.
 */
- (void)
build_content
{
    /* The notes over the AI pane, with a divider between them; the AI pane
     * is hidden — collapsed — until a summary is asked for. */
    NSSplitView *main_pane = [[NSSplitView alloc] initWithFrame:NSZeroRect];
    main_pane.vertical     = NO;
    main_pane.dividerStyle = NSSplitViewDividerStyleThin;
    [main_pane addArrangedSubview:on_library_notes_new(self.lib)];
    [main_pane addArrangedSubview:on_library_ai_pane_new(self.lib)];
    [main_pane setHoldingPriority:NSLayoutPriorityDefaultLow - 1 forSubviewAtIndex:0];

    self.split        = [[NotesSplitController alloc] init];
    self.sidebar_item = pane_item(on_library_sidebar_new(self.lib), on_look_sidebar_min_w);
    self.sidebar_item.canCollapse     = YES;
    self.sidebar_item.collapsed       = !self.lib.app->sidebar_visible;  /* as it was left */
    self.sidebar_item.holdingPriority = NSLayoutPriorityDefaultLow + 1;  /* notes absorb resizes */
    [self.split addSplitViewItem:self.sidebar_item];
    [self.split addSplitViewItem:pane_item(main_pane, on_look_pane_min_w)];
    self.split.splitView.dividerStyle = NSSplitViewDividerStyleThin;

    NSBox *rule  = [[NSBox alloc] initWithFrame:NSZeroRect];
    rule.boxType = NSBoxSeparator;
    NSView *content   = self.window.contentView;
    NSView *panes     = self.split.view;
    NSView *favorites = on_library_favorites_new(self.lib);
    BOOL    shown     = self.lib.app->favorites_bar_visible;
    self.favorites_bar    = favorites;
    self.favorites_height = [favorites.heightAnchor
        constraintEqualToConstant:shown ? on_look_favorites_bar_h + 1 : 0];
    favorites.hidden      = !shown;
    for (NSView *view in @[favorites, panes, rule]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:view];
    }
    /* The status band: from under the rule to the window's bottom edge. */
    NSLayoutGuide *band = [[NSLayoutGuide alloc] init];
    [content addLayoutGuide:band];
    self.status_path  = on_ui_status_label(content, band.centerYAnchor, NSTextAlignmentCenter);
    self.status_event = on_ui_status_label(content, band.centerYAnchor, NSTextAlignmentRight);
    CGFloat inset = on_look_status_inset;
    [NSLayoutConstraint activateConstraints:@[
        [favorites.topAnchor constraintEqualToAnchor:content.topAnchor],
        [favorites.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [favorites.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        self.favorites_height,
        [panes.topAnchor constraintEqualToAnchor:favorites.bottomAnchor],
        [panes.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [panes.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [panes.bottomAnchor constraintEqualToAnchor:rule.topAnchor],
        [rule.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [rule.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-on_look_status_h],
        [band.topAnchor constraintEqualToAnchor:rule.bottomAnchor],
        [band.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        /* The location is centred in the bar, as in Files and Tasks; it
         * gives way (truncating) before it would run into the event. */
        [self.status_path.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
        [self.status_path.leadingAnchor constraintGreaterThanOrEqualToAnchor:content.leadingAnchor
                                                                   constant:inset],
        [self.status_event.trailingAnchor constraintEqualToAnchor:content.trailingAnchor
                                                         constant:-inset],
        [self.status_event.leadingAnchor
            constraintGreaterThanOrEqualToAnchor:self.status_path.trailingAnchor constant:inset],
    ]];
    /* The width it was last dragged to, at least the narrowest drag; the
     * initial width until the first drag. */
    gint saved = on_app_config_get_int(sidebar_width_key, 0);
    [self.split.splitView setPosition:saved > 0 ? MAX(saved, on_look_sidebar_min_w) : on_look_sidebar_w
                   ofDividerAtIndex:0];
}

/*
 * update_location — show where the selection is: in the status bar's left
 * side, and in the window title, "Notes — <tag or view>" as in Tasks (a
 * tag with its emoji, as the sidebar shows it).
 */
- (void)
update_location
{
    NotesLibrary *lib  = self.lib;
    NSString     *name = lib.sel_name;
    if (lib.sel_kind == on_sb_tag) {
        OnTag *tag = on_db_tag_get(lib.app->db, lib.sel_id);
        if (tag) {
            name = on_ui_tag_label(tag->emoji, tag->name);
            on_db_tag_free(tag);
        }
    }
    self.window.title = name.length ? [@ON_APP_NAME stringByAppendingFormat:@" \xe2\x80\x94 %@", name]
                                    : @ON_APP_NAME;
    self.status_path.stringValue = on_library_location_text(lib);
}

/*
 * show_status: — the status bar's right side: an event message at full
 * strength (a fade under way is cut short), held for on_look_status_hold
 * and then faded out (status_clear).
 *
 * Inputs:
 *   message — the message
 */
- (void)
show_status:(NSString *)message
{
    self.status_event.stringValue = message;
    self.status_event.alphaValue  = 1.0;
    on_timer_remove(self.status_timer);
    self.status_timer = on_timer_add(on_look_status_hold, false, status_clear,
                                     (__bridge void *)self, NULL);
}

/*
 * toggle_sidebar — slide the sidebar away, or back, and remember which
 * (the sidebar_visible setting).
 */
- (void)
toggle_sidebar
{
    BOOL show = self.sidebar_item.collapsed;
    on_app_config_set(sidebar_visible_key, show ? "1" : "0");
    on_app_prefs_load(self.lib.app);
    self.sidebar_item.animator.collapsed = !show;
}

/*
 * toggle_favorites_bar — slide the Favorites bar away, or back, and
 * remember which (the favorites_bar_visible setting).  Hidden at the end
 * of the slide, so a bar of no height takes no clicks.
 */
- (void)
toggle_favorites_bar
{
    OnApp *app  = self.lib.app;
    BOOL   show = !app->favorites_bar_visible;
    on_app_config_set("favorites_bar_visible", show ? "1" : "0");
    on_app_prefs_load(app);
    NSView *bar = self.favorites_bar;
    if (show) bar.hidden = NO;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.allowsImplicitAnimation = YES;
        self.favorites_height.animator.constant = show ? on_look_favorites_bar_h + 1 : 0;
        [self.window.contentView layoutSubtreeIfNeeded];
    } completionHandler:^{
        bar.hidden = self.favorites_height.constant == 0;
    }];
}

/*
 * search_entered: — Return in the toolbar's search field: open the search
 * window on that query.
 *
 * Inputs:
 *   sender — the search field
 */
- (void)
search_entered:(NSSearchField *)sender
{
    if (sender.stringValue.length > 0) on_search_window_open(self.lib, NO, sender.stringValue);
}

/*
 * run_command: — a window-scope command (commands.h) reached this window
 * through the responder chain.
 *
 * Inputs:
 *   sender — the menu or toolbar item
 */
- (void)
run_command:(id)sender
{
    on_library_run_command(self.lib, sender);
}

/*
 * validateUserInterfaceItem: — enable a command's item only while the
 * library has a handler for it that says it can run.
 *
 * Inputs:
 *   item — the menu or toolbar item
 *
 * Output: YES to enable it.
 */
- (BOOL)
validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (item.action == @selector(run_command:)) return on_library_validate_command(self.lib, item);
    return YES;
}

/*
 * toolbarAllowedItemIdentifiers: — every item the toolbar may hold.  The
 * toolbar is fixed, so this is the same list as the default one.
 *
 * Inputs:
 *   toolbar — the library toolbar (unused)
 *
 * Output: the item identifiers, left to right.
 */
- (NSArray<NSToolbarItemIdentifier> *)
toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    return toolbar_idents(YES);
}

/*
 * toolbarDefaultItemIdentifiers: — the items the toolbar shows.
 *
 * Inputs:
 *   toolbar — the library toolbar (unused)
 *
 * Output: the item identifiers, left to right.
 */
- (NSArray<NSToolbarItemIdentifier> *)
toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    return toolbar_idents(self.lib.app->ai_enabled);
}

/*
 * toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar: — a command's
 * item from the command table, or the search field.
 *
 * Inputs:
 *   toolbar — the library toolbar (unused)
 *   ident   — which item
 *   flag    — whether it is being inserted now (unused)
 *
 * Output: the item.
 */
- (NSToolbarItem *)
toolbar:(NSToolbar *)toolbar
itemForItemIdentifier:(NSToolbarItemIdentifier)ident
willBeInsertedIntoToolbar:(BOOL)flag
{
    if (![ident isEqualToString:search_item_ident]) return on_command_toolbar_item(ident);

    /* The system's search item, not a field in a plain item: a view item
     * makes the toolbar 3 pt taller than Tasks' (measured, macOS 26). */
    NSSearchToolbarItem *item = [[NSSearchToolbarItem alloc] initWithItemIdentifier:ident];
    NSSearchField       *field = item.searchField;
    field.placeholderString      = @"Search all notes";
    field.focusRingType          = NSFocusRingTypeNone;   /* INTERFACES.md: toolbar fields only */
    field.target                 = self;
    field.action                 = @selector(search_entered:);
    field.sendsWholeSearchString = YES;      /* act on Return, not per key */
    item.label                        = @"Search";
    item.preferredWidthForSearchField = on_look_toolbar_search_w;
    [field.widthAnchor constraintLessThanOrEqualToConstant:on_look_toolbar_search_w].active = YES;
    return item;
}

/*
 * windowWillClose: — the library window is going: stop its timer and let go
 * of the controller.  The app keeps running while editors are open.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    on_timer_remove(self.status_timer);
    self.status_timer = 0;
    on_library_notes_closed();
    library_window    = nil;         /* may release self: last */
}

@end

/*
 * hook_notes_changed — OnApp.notify_notes_changed: something structural
 * changed; rebuild both panes.
 *
 * Inputs:
 *   app — the application context (unused)
 */
static void
hook_notes_changed(OnApp *app)
{
    on_library_refresh_all(on_library());
}

/*
 * hook_note_saved — OnApp.notify_note_saved: one note's content was saved.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   note_id — the note
 */
static void
hook_note_saved(OnApp *app, gint64 note_id)
{
    if (library_window) on_library_notes_note_saved(on_library(), note_id);
}

/*
 * hook_status — OnApp.notify_status: an event message for the status bar.
 * Also logged, so a development run shows the core's events on its
 * terminal even with the library window closed.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   message — the message
 */
static void
hook_status(OnApp *app, const gchar *message)
{
    NSLog(@"[status] %@", @(message));
    [library_window show_status:@(message)];
}

/*
 * run_toggle_sidebar — "toggle-sidebar".
 *
 * Inputs:
 *   target — the NotesLibrary (unused: there is one window)
 *   sender — the item (unused)
 */
static void
run_toggle_sidebar(id target, id sender)
{
    [library_window toggle_sidebar];
}

/*
 * validate_toggle_sidebar — the item names what a click will do, and is
 * disabled while the library window is closed.
 *
 * Inputs:
 *   target — the NotesLibrary (unused)
 *   item   — the item
 *
 * Output: YES while the window is open.
 */
static BOOL
validate_toggle_sidebar(id target, id item)
{
    BOOL collapsed = library_window.sidebar_item.collapsed;
    on_command_item_update(item, collapsed ? @"Show Sidebar" : @"Hide Sidebar", nil, NO);
    return library_window != nil;
}

/*
 * run_toggle_favorites — "toggle-favorites".
 *
 * Inputs:
 *   target — the NotesLibrary (unused: there is one window)
 *   sender — the item (unused)
 */
static void
run_toggle_favorites(id target, id sender)
{
    [library_window toggle_favorites_bar];
}

/*
 * validate_toggle_favorites — the item names what a click will do, and is
 * disabled while the library window is closed.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item
 *
 * Output: YES while the window is open.
 */
static BOOL
validate_toggle_favorites(id target, id item)
{
    BOOL shown = ((NotesLibrary *)target).app->favorites_bar_visible;
    on_command_item_update(item, shown ? @"Hide Favorites Bar" : @"Show Favorites Bar", nil, NO);
    return library_window != nil;
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t handlers[] = {
    { @"toggle-sidebar",   run_toggle_sidebar,   validate_toggle_sidebar   },
    { @"toggle-favorites", run_toggle_favorites, validate_toggle_favorites },
};

/*
 * on_library_window_install — see library_window.h.
 */
void
on_library_window_install(NotesLibrary *lib)
{
    on_library_add_commands(handlers, sizeof handlers / sizeof handlers[0]);
    lib.app->notify_notes_changed = hook_notes_changed;
    lib.app->notify_note_saved    = hook_note_saved;
    lib.app->notify_status        = hook_status;
}

/*
 * on_library_window_show — see library_window.h.
 */
void
on_library_window_show(NotesLibrary *lib)
{
    if (!library_window) library_window = [[NotesLibraryWindow alloc] init_with_library:lib];
    [library_window showWindow:nil];
}

/*
 * on_library_window_ai_changed — see library_window.h.  The toolbar gains
 * or loses AI Summary and its space, at the place toolbar_idents gives
 * them.
 */
void
on_library_window_ai_changed(NotesLibrary *lib)
{
    NSToolbar *toolbar = library_window.window.toolbar;
    if (toolbar == nil) return;
    NSArray<NSString *> *idents = [toolbar.items valueForKey:@"itemIdentifier"];
    NSUInteger at = [idents indexOfObject:@"ai-summary"];
    if (lib.app->ai_enabled && at == NSNotFound) {
        NSInteger flex = (NSInteger)[idents indexOfObject:NSToolbarFlexibleSpaceItemIdentifier];
        [toolbar insertItemWithItemIdentifier:NSToolbarSpaceItemIdentifier atIndex:flex];
        [toolbar insertItemWithItemIdentifier:@"ai-summary" atIndex:flex + 1];
    } else if (!lib.app->ai_enabled && at != NSNotFound) {
        [toolbar removeItemAtIndex:(NSInteger)at];
        [toolbar removeItemAtIndex:(NSInteger)at - 1];              /* its space */
    }
}

/*
 * on_library_window_location_changed — see library_window.h.
 */
void
on_library_window_location_changed(NotesLibrary *lib)
{
    [library_window update_location];
    on_library_favorites_refresh(lib);
}
