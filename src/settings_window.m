/*
 * settings_window.m — the Settings tool window (⌘,).
 *
 * Specification: the GTK build's src/settings_window.c.  One column,
 * as the GTK window and the Files app's Preferences have it: Appearance,
 * Editor, Apple Calendar and Apple Notes, scrolling once it is taller
 * than on_look_settings_max_h.  The database's health and its backups are
 * camb.io Database Manager's (camb.io_db_mgr.app), not a section here.
 *
 * Every control writes through at once — on_app_config_set, then
 * on_app_prefs_load so OnApp (the one reader of a preference) follows,
 * then the setting's live-apply hook — so there is no OK or Apply.
 *
 * GTK-only settings are not carried: touch assistance (GTK's selection
 * handles), the native menu bar (AppKit always has it) and the SVG loader
 * warning.
 */

#import "settings_window.h"

#import "apple_notes.h"
#include "identity.h"

#import "editor_window.h"
#import "shared_hotkey.h"
#import "library_planner.h"
#import "library_sidebar.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "commands.h"
#include "config.h"

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Width of wrapped text (descriptions, hints). */
static const CGFloat text_w = 340.0;

/* Indent of a section's controls under its heading, and of AI's sub-group. */
static const CGFloat indent    = 12.0;

/* How much wider than its content the window opens, for breathing room. */
static const CGFloat extra_w = 20.0;

/* ── The on/off settings ────────────────────────────────────────────────── */

/* What changes on screen when a setting flips. */
typedef enum {
    apply_notes,                     /* the library's rows                  */
    apply_sidebar_fit,               /* the sidebar's width                 */
    apply_editors,                   /* every open editor                   */
    apply_status_path,               /* the library and editor status bars  */
    apply_hotkey,                    /* the system-wide New Note shortcut   */
    apply_apple_notes,               /* the copy into Apple Notes           */
    apply_none,                      /* read when it is used                */
} apply_t;

/* One checkbox: its label, its ini key and what it changes. */
typedef struct {
    const char *label;
    const char *key;
    apply_t     apply;
} bool_setting_t;

static const bool_setting_t appearance_settings[] = {
    { "Show note counts next to views and tags",      "sidebar_counts",      apply_notes       },
    { "Fit the sidebar width to the rows on show",    "sidebar_fit_content", apply_sidebar_fit },
    { "Bold titles in comfortable list density",      "bold_list_titles",    apply_notes       },
    { "Show completed action items",                  "show_done_actions",   apply_notes       },
    { "Show database path prefix in status bar",      "statusbar_db_path",   apply_status_path },
};

static const bool_setting_t editor_settings[] = {
    { "Show copy button on code blocks",                              "code_copy_button",       apply_editors },
    { "Show line numbers in code blocks",                             "code_line_numbers",      apply_editors },
    { "First line is Title formatted",                                "first_line_title",       apply_editors },
    { "Compact toolbar (group paragraph styles and lists into menus)", "compact_editor_toolbar", apply_editors },
    { "Show note id in the editor status bar",                        "statusbar_note_id",      apply_editors },
    { "Always put dropped files on the last line of the note",        "drop_files_last_line",   apply_none    },
    { "New Note (\u2303\u2325\u2318N) works from any app",             "new_note_hotkey",        apply_hotkey  },
};

static const bool_setting_t apple_notes_setting =
    { "Push Notes data into Apple Notes", ON_APPLE_NOTES_KEY, apply_apple_notes };

/*
 * setting_on — a checkbox setting's current value, read from OnApp (its
 * one reader, which knows each key's default).
 *
 * Inputs:
 *   app — the application context
 *   key — the ini key
 *
 * Output: the value.
 */
static BOOL
setting_on(const OnApp *app, const char *key)
{
    static const struct { const char *key; size_t off; } fields[] = {
        { "sidebar_counts",         offsetof(OnApp, sidebar_counts)         },
        { "sidebar_fit_content",    offsetof(OnApp, sidebar_fit_content)    },
        { "bold_list_titles",       offsetof(OnApp, bold_list_titles)       },
        { "show_done_actions",      offsetof(OnApp, show_done_actions)      },
        { "statusbar_db_path",      offsetof(OnApp, statusbar_db_path)      },
        { "code_copy_button",       offsetof(OnApp, code_copy_buttons)      },
        { "code_line_numbers",      offsetof(OnApp, code_line_numbers)      },
        { "first_line_title",       offsetof(OnApp, first_line_title)       },
        { "compact_editor_toolbar", offsetof(OnApp, compact_editor_toolbar) },
        { "statusbar_note_id",      offsetof(OnApp, statusbar_note_id)      },
        { "drop_files_last_line",   offsetof(OnApp, drop_files_last_line)   },
        { "new_note_hotkey",        offsetof(OnApp, new_note_hotkey)        },
        { ON_APPLE_NOTES_KEY,       offsetof(OnApp, apple_notes)            },
    };
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
        if (strcmp(fields[i].key, key) == 0) return *(const gboolean *)((const char *)app + fields[i].off);
    return NO;
}

/* ── Small builders ─────────────────────────────────────────────────────── */

/*
 * small_button — a small push button (the Apple Notes section's buttons
 * sit under the lines they act on, so they are quiet).
 *
 * Inputs:
 *   title  — its title
 *   target — the receiver
 *   action — the selector
 *
 * Output: the button.
 */
static NSButton *
small_button(NSString *title, id target, SEL action)
{
    NSButton *button = [NSButton buttonWithTitle:title target:target action:action];
    button.controlSize = NSControlSizeSmall;
    return button;
}

/*
 * row — views side by side.
 *
 * Inputs:
 *   views — the views
 *
 * Output: the stack.
 */
static NSStackView *
row(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing     = 6;
    return stack;
}

/*
 * indented — a view moved right under its heading.
 *
 * Inputs:
 *   view  — the view
 *   inset — how far
 *
 * Output: a stack holding it.
 */
static NSStackView *
indented(NSView *view, CGFloat inset)
{
    NSStackView *stack = [NSStackView stackViewWithViews:@[view]];
    stack.edgeInsets = NSEdgeInsetsMake(0, inset, 0, 0);
    return stack;
}

/* ── The window ─────────────────────────────────────────────────────────── */

@interface NotesSettingsWindow : NSWindowController <NSWindowDelegate, NSTextFieldDelegate>
@property (nonatomic, weak)   NotesLibrary *lib;
@property (nonatomic, strong) NSTextField  *viewer;
@property (nonatomic, strong) SharedCalendarSection *calendar; /* shared with Tasks' Settings */
@property (nonatomic, strong) NSTextField  *apple_status;  /* the Apple Notes line   */
@end

/* The open Settings window, or nil. */
static NotesSettingsWindow *settings_window;

@implementation NotesSettingsWindow

/*
 * init_with_lib: — build the window: one column of sections in a scroll
 * view, the window sized to it up to on_look_settings_max_h.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the controller.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 400)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered defer:NO];
    win.title              = @"Settings";
    win.releasedWhenClosed = NO;
    if (!(self = [super initWithWindow:win])) return nil;
    _lib = lib;
    win.delegate = self;

    NSMutableArray<NSView *> *rows     = [NSMutableArray array];
    NSMutableArray<NSView *> *headings = [NSMutableArray array];
    NSMutableArray<NSView *> *ends     = [NSMutableArray array];   /* last row of a section */
    [headings addObject:[self add_appearance:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_editor:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_calendar:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_apple_notes:rows]];

    NSStackView *stack = [NSStackView stackViewWithViews:rows];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment   = NSLayoutAttributeLeading;
    stack.spacing     = 6;
    stack.edgeInsets  = NSEdgeInsetsMake(16, 20, 20, 20);
    for (NSView *v in headings) [stack setCustomSpacing:8 afterView:v];
    for (NSView *v in ends)     [stack setCustomSpacing:18 afterView:v];

    shared_ui_use_ui_font(stack);
    [stack layoutSubtreeIfNeeded];
    NSSize fit = stack.fittingSize;
    SharedFlippedView *doc = [[SharedFlippedView alloc] initWithFrame:NSZeroRect];
    doc.translatesAutoresizingMaskIntoConstraints   = NO;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:stack];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = doc;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.drawsBackground     = NO;
    shared_ui_pin_edges(stack, doc);
    [NSLayoutConstraint activateConstraints:@[
        [doc.topAnchor        constraintEqualToAnchor:scroll.contentView.topAnchor],
        [doc.leadingAnchor    constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [doc.trailingAnchor   constraintEqualToAnchor:scroll.contentView.trailingAnchor],
    ]];
    win.contentView = scroll;
    CGFloat bar = fit.height > on_look_settings_max_h
                ? [NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy] : 0;
    [win setContentSize:NSMakeSize(fit.width + extra_w + bar, MIN(fit.height, on_look_settings_max_h))];
    [win center];
    return self;
}

/* ── Sections ─────────────────────────────────────────────────────────── */

/*
 * check_for: — a checkbox for one on/off setting, ticked from OnApp.
 *
 * Inputs:
 *   s — the setting
 *
 * Output: the checkbox (its identifier is the ini key).
 */
- (NSButton *)
check_for:(const bool_setting_t *)s
{
    NSButton *check = [NSButton checkboxWithTitle:@(s->label) target:self action:@selector(bool_toggled:)];
    check.identifier = @(s->key);
    check.tag        = s->apply;
    check.state      = setting_on(self.lib.app, s->key) ? NSControlStateValueOn : NSControlStateValueOff;
    return check;
}

/*
 * add_appearance: — List density and the appearance checkboxes.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_appearance:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = shared_ui_heading(@"Appearance");
    [rows addObject:head];
    NSPopUpButton *density = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [density addItemsWithTitles:@[@"Compact", @"Comfortable"]];
    [density selectItemAtIndex:self.lib.app->comfortable_list ? 1 : 0];
    density.identifier = @"density";
    density.target     = self;
    density.action     = @selector(density_changed:);
    [rows addObject:indented(row(@[[NSTextField labelWithString:@"List density:"], density]), indent)];
    NSPopUpButton *group = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [group addItemsWithTitles:@[@"None", @"Day", @"Week", @"Month", @"Year"]];
    [group selectItemAtIndex:(NSInteger)self.lib.app->grid_group];
    group.identifier = @"grid-group";
    group.target     = self;
    group.action     = @selector(grid_group_changed:);
    group.toolTip    = @"Group the grid's cards into sections by the date the list is sorted by";
    [rows addObject:indented(row(@[[NSTextField labelWithString:@"Group grid by:"], group]), indent)];
    for (size_t i = 0; i < sizeof appearance_settings / sizeof *appearance_settings; i++)
        [rows addObject:indented([self check_for:&appearance_settings[i]], indent)];
    return head;
}

/*
 * add_editor: — the editor checkboxes and the image viewer.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_editor:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = shared_ui_heading(@"Editor");
    [rows addObject:head];
    for (size_t i = 0; i < sizeof editor_settings / sizeof *editor_settings; i++)
        [rows addObject:indented([self check_for:&editor_settings[i]], indent)];

    self.viewer = [NSTextField textFieldWithString:@""];
    self.viewer.identifier        = @"image-viewer";
    self.viewer.placeholderString = @"System default";
    self.viewer.delegate          = self;
    gchar *viewer = on_app_config_get("image_viewer");
    if (viewer) self.viewer.stringValue = @(viewer);
    g_free(viewer);
    [self.viewer.widthAnchor constraintEqualToConstant:200].active = YES;
    NSButton *browse = [NSButton buttonWithTitle:@"Browse…" target:self action:@selector(browse_viewer:)];
    [rows addObject:indented(row(@[[NSTextField labelWithString:@"Image viewer:"], self.viewer, browse]), indent)];
    return head;
}

/*
 * add_calendar: — the Apple Calendar section (shared_calendar.h), the one Tasks'
 * Settings shows too, without the calendar for Tasks' sessions.  A change
 * redraws the library's day planner.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_calendar:(NSMutableArray<NSView *> *)rows
{
    NotesLibrary *lib  = self.lib;
    NSTextField  *head = shared_ui_heading(@"Apple Calendar");
    [rows addObject:head];
    self.calendar = [[SharedCalendarSection alloc]
        init_with_settings:on_calendar_settings(lib) tasks:NO width:text_w - indent
                   changed:^{ on_library_window_planner_changed(lib); }];
    [rows addObject:indented(self.calendar.view, indent)];
    return head;
}

/*
 * add_apple_notes: — the Apple Notes section (apple_notes.h): the switch,
 * the status line the module keeps, Push All Now, and the way to Full Disk
 * Access, which the copy needs.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_apple_notes:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = shared_ui_heading(@"Apple Notes");
    [rows addObject:head];
    [rows addObject:indented([self check_for:&apple_notes_setting], indent)];
    self.apple_status = shared_ui_note(on_apple_notes_status(), text_w - indent);
    self.apple_status.identifier = @"apple_status";
    [rows addObject:indented(self.apple_status, indent)];
    NSButton *push = small_button(@"Push All Now", self, @selector(push_all_apple_notes:));
    push.toolTip = @"Copy every note again, whether or not it changed.";
    NSButton *access = small_button(@"Open Full Disk Access\u2026", self, @selector(open_full_disk_access:));
    access.toolTip = @"Apple Notes keeps its notes in a protected folder: " @ON_APP_NAME
                     @" must be allowed Full Disk Access to write there.";
    [rows addObject:indented(row(@[push, access]), indent)];
    [rows addObject:indented(shared_ui_note(@"One way: notes are copied into Apple Notes and kept up to date there; "
                                            @"nothing is read back. Tags become Apple Notes tags.", text_w - indent), indent)];
    __weak NotesSettingsWindow *ws = self;
    on_apple_notes_observe(^{ ws.apple_status.stringValue = on_apple_notes_status(); });
    return head;
}

/* ── Actions ──────────────────────────────────────────────────────────── */

/*
 * write_setting:value: — persist a setting and reload the preferences.
 *
 * Inputs:
 *   key   — the ini key
 *   value — its value; NULL clears it
 */
- (void)
write_setting:(const char *)key value:(const char *)value
{
    on_app_config_set(key, value);
    on_app_prefs_load(self.lib.app);
}

/*
 * bool_toggled: — a checkbox: write it, then apply it live.  A shortcut
 * another app holds cannot be registered: its box goes back off, the
 * setting with it, and a notice says why.
 *
 * Inputs:
 *   sender — the checkbox (identifier = key, tag = apply_t)
 */
- (void)
bool_toggled:(NSButton *)sender
{
    [self write_setting:sender.identifier.UTF8String value:sender.state == NSControlStateValueOn ? "1" : "0"];
    switch ((apply_t)sender.tag) {
    case apply_notes:       on_library_refresh_all(self.lib); break;
    case apply_sidebar_fit: on_library_sidebar_fit(self.lib); break;
    case apply_editors:     on_editor_settings_changed();     break;
    case apply_status_path:
        on_library_refresh_all(self.lib);
        on_editor_settings_changed();
        break;
    case apply_hotkey:
        if (shared_hotkey_apply()) break;
        sender.state = NSControlStateValueOff;
        [self write_setting:sender.identifier.UTF8String value:"0"];
        shared_ui_notice(self.window, @"The shortcut is in use",
                         @"Another app has already taken \u2303\u2325\u2318N.", nil);
        break;
    case apply_apple_notes:
        on_apple_notes_apply(self.lib.app);
        self.apple_status.stringValue = on_apple_notes_status();
        break;
    case apply_none:        break;
    }
}

/*
 * density_changed: — List density: write it and rebuild the rows.
 *
 * Inputs:
 *   sender — the pop-up
 */
- (void)
density_changed:(NSPopUpButton *)sender
{
    [self write_setting:"list_density_comfortable" value:sender.indexOfSelectedItem == 1 ? "1" : "0"];
    on_library_refresh_all(self.lib);
}

/*
 * grid_group_changed: — Group grid by: write it and rebuild the grid.
 *
 * Inputs:
 *   sender — the pop-up, its items in on_grid_group_t order
 */
- (void)
grid_group_changed:(NSPopUpButton *)sender
{
    static const char *const names[] = { "none", "day", "week", "month", "year" };
    [self write_setting:"grid_group" value:names[sender.indexOfSelectedItem]];
    on_library_refresh_all(self.lib);
}

/*
 * controlTextDidChange: — the image viewer, written as it is typed; empty
 * clears the setting.
 *
 * Inputs:
 *   note — the notification (its object is the field)
 */
- (void)
controlTextDidChange:(NSNotification *)note
{
    NSTextField *field = note.object;
    const char  *value = field.stringValue.length ? field.stringValue.UTF8String : NULL;
    if (field == self.viewer) [self write_setting:"image_viewer" value:value];
}

/*
 * browse_viewer: — pick the viewer app; the field takes the path and is
 * written.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
browse_viewer:(id)sender
{
    __weak NotesSettingsWindow *ws = self;
    shared_ui_pick_path(SharedPickOpenFile, @"Choose Image Viewer", @"Select", @[UTTypeApplicationBundle],
                    @"/Applications", ^(NSString *path) {
        if (path == nil || ws == nil) return;
        ws.viewer.stringValue = path;
        [ws write_setting:"image_viewer" value:path.UTF8String];
    });
}

/*
 * push_all_apple_notes: — Push All Now: copy every note again.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
push_all_apple_notes:(id)sender
{
    if (!self.lib.app->apple_notes) {
        shared_ui_notice(self.window, @"Apple Notes is off", @"Turn the copy on first.", nil);
        return;
    }
    on_apple_notes_push_all(self.lib.app);
    self.apple_status.stringValue = on_apple_notes_status();
}

/*
 * open_full_disk_access: — show the Full Disk Access pane of System
 * Settings, where the app is allowed to reach Apple Notes' store.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
open_full_disk_access:(id)sender
{
    NSURL *pane = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
    [NSWorkspace.sharedWorkspace openURL:pane];
}

/*
 * windowWillClose: — the window goes.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self.calendar shut_down];
    on_apple_notes_observe(nil);
    settings_window = nil;
}

@end

/* ── Command ────────────────────────────────────────────────────────────── */

/*
 * run_preferences — "preferences": open the Settings window, or bring it
 * forward.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_preferences(id target, id sender)
{
    if (settings_window == nil) {
        settings_window = [[NotesSettingsWindow alloc] init_with_lib:target];
    }
    [settings_window showWindow:nil];
    [settings_window.window makeKeyAndOrderFront:nil];
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const shared_command_handler_t settings_handlers[] = {
    { @"preferences", run_preferences, NULL },
};

/*
 * on_settings_install — see settings_window.h.
 */
void
on_settings_install(NotesLibrary *lib)
{
    on_library_add_commands(settings_handlers, sizeof settings_handlers / sizeof *settings_handlers);
}
