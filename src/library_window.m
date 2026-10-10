#import "library_window.h"
#import "library_ai.h"
#import "library_notes.h"
#import "library_sidebar.h"
#import "search_window.h"
#import "ui_util.h"

#include "identity.h"

/* How long an event message stays in the status bar, in seconds. */
static const double status_message_seconds = 4.0;

/* The toolbar's own items, beside the commands'. */
static NSToolbarItemIdentifier const search_item_ident = @"notes.search-field";

/*
 * toolbar_idents — the toolbar, left to right: command identifiers
 * (commands.h), spaces and the search field.
 *
 * Output: the item identifiers.
 */
static NSArray<NSToolbarItemIdentifier> *
toolbar_idents(void)
{
    return @[
        @"new-folder", @"delete-folder", NSToolbarSpaceItemIdentifier,
        @"quicknote", @"new-note", @"delete-note", NSToolbarSpaceItemIdentifier,
        @"toggle-sidebar", @"toggle-view", @"toggle-done", @"search", @"media", @"preferences",
        @"ai-summary", NSToolbarFlexibleSpaceItemIdentifier, search_item_ident,
    ];
}

@interface NotesLibraryWindow : NSWindowController <NSWindowDelegate, NSToolbarDelegate,
                                                    NSUserInterfaceValidations>
@property (nonatomic, strong) NotesLibrary *lib;
@property (nonatomic, strong) NSSplitViewController *split;   /* sidebar | notes */
@property (nonatomic, strong) NSSplitViewItem *sidebar_item;
@property (nonatomic, strong) NSTextField *status_path;       /* left: the selection */
@property (nonatomic, strong) NSTextField *status_event;      /* right: the last event */
@property (nonatomic, assign) on_timer_id_t status_timer;     /* clears status_event; 0 = none */
@end

/* The open library window's controller, or nil.  THE owner (see OWNERSHIP
 * in library_window.h). */
static NotesLibraryWindow *library_window;

/*
 * status_label — one of the status bar's two labels.
 *
 * Inputs:
 *   alignment — which side its text sits on
 *
 * Output: the label.
 */
static NSTextField *
status_label(NSTextAlignment alignment)
{
    NSTextField *label = [NSTextField labelWithString:@""];
    label.font          = [NSFont systemFontOfSize:on_look_status_font];
    label.textColor     = NSColor.secondaryLabelColor;
    label.alignment     = alignment;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

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
 * long enough.
 *
 * Inputs:
 *   data — the NotesLibraryWindow, unretained: the controller removes the
 *          timer when its window closes
 */
static void
status_clear(void *data)
{
    NotesLibraryWindow *controller = (__bridge NotesLibraryWindow *)data;
    controller.status_timer = 0;
    controller.status_event.stringValue = @"";
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
    [self update_status_path];
    return self;
}

/*
 * build_content — the split view (sidebar | notes over the AI pane) above
 * the status bar.
 */
- (void)
build_content
{
    NSStackView *main_pane = [NSStackView stackViewWithViews:@[
        on_library_notes_new(self.lib), on_library_ai_pane_new(self.lib),
    ]];
    main_pane.orientation = NSUserInterfaceLayoutOrientationVertical;
    main_pane.spacing     = 0;
    for (NSView *pane in main_pane.arrangedSubviews) {   /* panes span the full width */
        [pane.widthAnchor constraintEqualToAnchor:main_pane.widthAnchor].active = YES;
    }

    self.split        = [[NSSplitViewController alloc] init];
    self.sidebar_item = pane_item(on_library_sidebar_new(self.lib), on_look_sidebar_min_w);
    self.sidebar_item.canCollapse     = YES;
    self.sidebar_item.holdingPriority = NSLayoutPriorityDefaultLow + 1;  /* notes absorb resizes */
    [self.split addSplitViewItem:self.sidebar_item];
    [self.split addSplitViewItem:pane_item(main_pane, on_look_main_pane_min_w)];
    self.split.splitView.dividerStyle = NSSplitViewDividerStyleThin;

    NSBox *rule  = [[NSBox alloc] initWithFrame:NSZeroRect];
    rule.boxType = NSBoxSeparator;
    self.status_path  = status_label(NSTextAlignmentLeft);
    self.status_event = status_label(NSTextAlignmentRight);

    NSView *content = self.window.contentView;
    NSView *panes   = self.split.view;
    for (NSView *view in @[panes, rule, self.status_path, self.status_event]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:view];
    }
    CGFloat inset = on_look_status_inset;
    [NSLayoutConstraint activateConstraints:@[
        [panes.topAnchor constraintEqualToAnchor:content.topAnchor],
        [panes.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [panes.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [panes.bottomAnchor constraintEqualToAnchor:rule.topAnchor],
        [rule.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [rule.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-on_look_status_h],
        [self.status_path.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:inset],
        [self.status_path.centerYAnchor constraintEqualToAnchor:content.bottomAnchor
                                                       constant:-on_look_status_h / 2.0],
        [self.status_event.trailingAnchor constraintEqualToAnchor:content.trailingAnchor
                                                         constant:-inset],
        [self.status_event.centerYAnchor constraintEqualToAnchor:self.status_path.centerYAnchor],
        [self.status_event.leadingAnchor
            constraintGreaterThanOrEqualToAnchor:self.status_path.trailingAnchor constant:inset],
    ]];
    [self.split.splitView setPosition:on_look_sidebar_w ofDividerAtIndex:0];
}

/*
 * update_status_path — the status bar's left side: where the selection is.
 */
- (void)
update_status_path
{
    self.status_path.stringValue = on_library_location_text(self.lib);
}

/*
 * show_status: — the status bar's right side: an event message, cleared
 * again after status_message_seconds.
 *
 * Inputs:
 *   message — the message
 */
- (void)
show_status:(NSString *)message
{
    self.status_event.stringValue = message;
    on_timer_remove(self.status_timer);
    self.status_timer = on_timer_add(status_message_seconds, false, status_clear,
                                     (__bridge void *)self, NULL);
}

/*
 * toggle_sidebar — slide the sidebar away, or back.
 */
- (void)
toggle_sidebar
{
    self.sidebar_item.animator.collapsed = !self.sidebar_item.collapsed;
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
    return toolbar_idents();
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
    return toolbar_idents();
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

    NSSearchField *field = on_ui_toolbar_search_field(@"Search all notes",
                                                      on_look_toolbar_search_w);
    field.target = self;
    field.action = @selector(search_entered:);
    field.sendsWholeSearchString = YES;      /* act on Return, not per key */
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    item.label = @"Search";
    item.view  = field;
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

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t handlers[] = {
    { @"toggle-sidebar", run_toggle_sidebar, validate_toggle_sidebar },
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
 * on_library_window_location_changed — see library_window.h.
 */
void
on_library_window_location_changed(NotesLibrary *lib)
{
    [library_window update_status_path];
}
