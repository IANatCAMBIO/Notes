/*
 * settings_window.m — the Settings window (⌘,).
 *
 * Three tabs: Appearance (display preferences), Backup (rotating-backup
 * controls), About (version, database path).  Every control writes through
 * to the in-memory config via on_app_config_set and reloads the preference
 * into OnApp with on_app_prefs_load so callers see the change immediately;
 * there is no OK/Apply button.
 *
 * A single file-static instance is kept alive for the process lifetime
 * once the window is first opened.  Closing hides it; a second "preferences"
 * command brings it back.
 *
 * The module registers the "preferences" command with the library at
 * startup (on_settings_install).
 */

#import "settings_window.h"

#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "backup.h"
#include "commands.h"
#include "config.h"
#include "db.h"
#include "editor_window.h"
#include "library_state.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

static const CGFloat kContentW   = 380.0;
static const CGFloat kSectionGap = 12.0;
static const CGFloat kControlGap = 6.0;

/* ── Forward declarations ───────────────────────────────────────────────── */

@interface NotesSettingsWindow : NSWindowController <NSWindowDelegate>
@property (nonatomic, weak) NotesLibrary *lib;
- (instancetype)initWithLibrary:(NotesLibrary *)lib;
@end

/* ── File-static instance ───────────────────────────────────────────────── */

static NotesSettingsWindow *settings_window;

/* ── Layout helpers ─────────────────────────────────────────────────────── */

/*
 * section_label — a bold section heading label.
 *
 * Inputs:
 *   text — the heading string
 *
 * Output: configured non-editable label.
 */
static NSTextField *
section_label(NSString *text)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize];
    return label;
}

/*
 * vstack — a vertical NSStackView with leading alignment.
 *
 * Inputs:
 *   views   — the arranged subviews
 *   spacing — point gap between them
 *
 * Output: the stack view.
 */
static NSStackView *
vstack(NSArray<NSView *> *views, CGFloat spacing)
{
    NSStackView *sv = [NSStackView stackViewWithViews:views];
    sv.orientation = NSUserInterfaceLayoutOrientationVertical;
    sv.alignment   = NSLayoutAttributeLeading;
    sv.spacing     = spacing;
    return sv;
}

/*
 * bool_check — a checkbox wired to one bool preference.
 *
 * Reads the current value from config and sets the initial state.
 * Stores the ini key as the button's identifier so action handlers can
 * retrieve it without a lookup table.
 *
 * Inputs:
 *   title    — the checkbox label
 *   key      — the ini key to read and write (UTF-8 C string)
 *   target   — the action receiver
 *   selector — the action method on target
 *
 * Output: configured checkbox.
 */
static NSButton *
bool_check(NSString *title, const gchar *key, id target, SEL selector)
{
    BOOL on = on_app_config_get_bool(key, NO);
    NSButton *btn = [NSButton checkboxWithTitle:title target:target
                                         action:selector];
    btn.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    btn.identifier = @(key);
    return btn;
}

/* ── NotesSettingsWindow ────────────────────────────────────────────────── */

@implementation NotesSettingsWindow {
    NSTabView    *_tab_view;
    /* Backup tab controls. */
    NSButton     *_backup_enabled;
    NSTextField  *_backup_dir_field;
    NSTextField  *_backup_interval_field;
    NSTextField  *_backup_keep_field;
    NSTextField  *_backup_status;
}

/*
 * initWithLibrary: — build the Settings window.
 *
 * Inputs:
 *   lib — the library (app state and DB path accessed through it)
 *
 * Output: the window controller.
 */
- (instancetype)
initWithLibrary:(NotesLibrary *)lib
{
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, kContentW + 40, 460)
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = @"Settings";
    win.releasedWhenClosed = NO;
    [win center];

    self = [super initWithWindow:win];
    if (!self) return nil;

    _lib = lib;
    win.delegate = self;

    _tab_view = [[NSTabView alloc] init];
    [_tab_view addTabViewItem:[self appearance_tab]];
    [_tab_view addTabViewItem:[self backup_tab]];
    [_tab_view addTabViewItem:[self about_tab]];

    NSView *content = win.contentView;
    [content addSubview:_tab_view];
    _tab_view.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [_tab_view.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor  constant:16],
        [_tab_view.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],
        [_tab_view.topAnchor      constraintEqualToAnchor:content.topAnchor      constant:16],
        [_tab_view.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor   constant:-16],
    ]];

    return self;
}

/* ── Tab builders ───────────────────────────────────────────────────────── */

/*
 * appearance_tab — build the Appearance tab (display + editor + list density).
 *
 * Output: configured NSTabViewItem.
 */
- (NSTabViewItem *)
appearance_tab
{
    NSMutableArray<NSView *> *items = [NSMutableArray array];
    [items addObject:section_label(@"Display")];

    static const struct { const char *title; const char *key; } display_prefs[] = {
        { "Show note counts next to folders and tags", "sidebar_counts"      },
        { "Fit sidebar width to folder names",         "sidebar_fit_content" },
        { "Bold titles in list",                       "bold_list_titles"    },
        { "Show completed action items",               "show_done_actions"   },
        { "Show database path in status bar",          "statusbar_db_path"   },
    };
    for (size_t i = 0; i < sizeof(display_prefs)/sizeof(*display_prefs); i++) {
        [items addObject:bool_check(@(display_prefs[i].title),
                                   display_prefs[i].key,
                                   self,
                                   @selector(appearance_bool_changed:))];
    }

    [items addObject:[NSView new]];
    [items addObject:section_label(@"Editor")];

    static const struct { const char *title; const char *key; } editor_prefs[] = {
        { "Show copy button on code blocks",  "code_copy_button"       },
        { "Show line numbers in code blocks", "code_line_numbers"      },
        { "First line is Title formatted",    "first_line_title"       },
        { "Compact toolbar",                  "compact_editor_toolbar" },
        { "Show note id in editor status bar","statusbar_note_id"      },
    };
    for (size_t i = 0; i < sizeof(editor_prefs)/sizeof(*editor_prefs); i++) {
        [items addObject:bool_check(@(editor_prefs[i].title),
                                   editor_prefs[i].key,
                                   self,
                                   @selector(editor_bool_changed:))];
    }

    [items addObject:[NSView new]];
    [items addObject:section_label(@"Notes list")];
    [items addObject:bool_check(@"Comfortable rows (title + preview line)",
                                "list_density_comfortable",
                                self,
                                @selector(list_density_changed:))];

    NSStackView *sv = vstack(items, kControlGap);
    sv.edgeInsets = NSEdgeInsetsMake(kSectionGap, kSectionGap, kSectionGap, kSectionGap);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView = sv;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;

    NSTabViewItem *item = [[NSTabViewItem alloc] init];
    item.label = @"Appearance";
    item.view  = scroll;
    return item;
}

/*
 * backup_tab — build the Backup tab.
 *
 * Output: configured NSTabViewItem.
 */
- (NSTabViewItem *)
backup_tab
{
    _backup_enabled = [NSButton checkboxWithTitle:@"Automatic rotating backups"
                                           target:self
                                           action:@selector(backup_enabled_changed:)];
    _backup_enabled.state = on_app_config_get_bool("backup_enabled", NO)
        ? NSControlStateValueOn : NSControlStateValueOff;

    NSTextField *dir_label = [NSTextField labelWithString:@"Backup folder:"];
    _backup_dir_field = [NSTextField textFieldWithString:@""];
    _backup_dir_field.placeholderString = @"(default: backups/ in data folder)";
    gchar *dir = on_app_config_get("backup_dir");
    if (dir) { _backup_dir_field.stringValue = @(dir); g_free(dir); }
    _backup_dir_field.target = self;
    _backup_dir_field.action = @selector(backup_dir_changed:);

    NSButton *choose = [NSButton buttonWithTitle:@"Choose…"
                                          target:self
                                          action:@selector(backup_choose_dir:)];
    NSStackView *dir_row = [NSStackView stackViewWithViews:@[_backup_dir_field, choose]];
    dir_row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    dir_row.spacing = 6;

    NSTextField *interval_label = [NSTextField labelWithString:@"Back up every (minutes):"];
    _backup_interval_field = [NSTextField textFieldWithString:@""];
    _backup_interval_field.placeholderString = @"60";
    gchar *interval_str = on_app_config_get("backup_interval_min");
    if (interval_str) { _backup_interval_field.stringValue = @(interval_str); g_free(interval_str); }
    else _backup_interval_field.stringValue = @"60";
    _backup_interval_field.target = self;
    _backup_interval_field.action = @selector(backup_interval_changed:);
    [_backup_interval_field.widthAnchor constraintEqualToConstant:60].active = YES;

    NSTextField *keep_label = [NSTextField labelWithString:@"Keep backups:"];
    _backup_keep_field = [NSTextField textFieldWithString:@""];
    _backup_keep_field.placeholderString = @"10";
    gchar *keep_str = on_app_config_get("backup_keep");
    if (keep_str) { _backup_keep_field.stringValue = @(keep_str); g_free(keep_str); }
    else _backup_keep_field.stringValue = @"10";
    _backup_keep_field.target = self;
    _backup_keep_field.action = @selector(backup_keep_changed:);
    [_backup_keep_field.widthAnchor constraintEqualToConstant:60].active = YES;

    _backup_status = [NSTextField labelWithString:@""];
    _backup_status.textColor = NSColor.secondaryLabelColor;
    _backup_status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

    NSButton *run_now = [NSButton buttonWithTitle:@"Back Up Now"
                                           target:self
                                           action:@selector(backup_run_now:)];

    NSArray<NSView *> *items = @[
        _backup_enabled,
        vstack(@[dir_label, dir_row], 4),
        vstack(@[interval_label, _backup_interval_field], 4),
        vstack(@[keep_label, _backup_keep_field], 4),
        vstack(@[_backup_status, run_now], 6),
    ];
    NSStackView *sv = vstack(items, kSectionGap);
    sv.edgeInsets = NSEdgeInsetsMake(kSectionGap, kSectionGap, kSectionGap, kSectionGap);

    NSTabViewItem *item = [[NSTabViewItem alloc] init];
    item.label = @"Backup";
    item.view  = sv;
    return item;
}

/*
 * about_tab — build the About tab: app name, bundle version, database path.
 *
 * Output: configured NSTabViewItem.
 */
- (NSTabViewItem *)
about_tab
{
    NSTextField *name_label = [NSTextField labelWithString:@"Notes AppKit"];
    name_label.font = [NSFont boldSystemFontOfSize:18];

    NSString *ver = [NSBundle mainBundle].infoDictionary[@"CFBundleShortVersionString"]
        ?: @"(dev build)";
    NSTextField *ver_label =
        [NSTextField labelWithString:[@"Version " stringByAppendingString:ver]];
    ver_label.textColor = NSColor.secondaryLabelColor;

    NSTextField *db_heading = section_label(@"Database");
    OnApp *app = _lib.app;
    NSString *db_path = (app && app->db) ? @(app->db->path) : @"(no database open)";
    NSTextField *db_label = [NSTextField wrappingLabelWithString:db_path];
    db_label.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize
                                                weight:NSFontWeightRegular];
    db_label.preferredMaxLayoutWidth = kContentW;

    NSStackView *sv = vstack(@[name_label, ver_label, [NSView new], db_heading, db_label],
                             kControlGap);
    sv.edgeInsets = NSEdgeInsetsMake(kSectionGap, kSectionGap, kSectionGap, kSectionGap);

    NSTabViewItem *item = [[NSTabViewItem alloc] init];
    item.label = @"About";
    item.view  = sv;
    return item;
}

/* ── Appearance actions ─────────────────────────────────────────────────── */

/*
 * appearance_bool_changed: — a display checkbox was toggled.
 *
 * Inputs:
 *   sender — the checkbox; identifier holds the ini key
 *
 * Output: writes the key, reloads prefs, notifies notes-changed.
 */
- (void)
appearance_bool_changed:(NSButton *)sender
{
    BOOL on = (sender.state == NSControlStateValueOn);
    on_app_config_set(sender.identifier.UTF8String, on ? "1" : "0");
    OnApp *app = _lib.app;
    on_app_prefs_load(app);
    if (app->notify_notes_changed) app->notify_notes_changed(app);
}

/*
 * editor_bool_changed: — an editor preference checkbox was toggled.
 *
 * Inputs:
 *   sender — the checkbox; identifier holds the ini key
 *
 * Output: writes the key, reloads prefs, notifies all editors.
 */
- (void)
editor_bool_changed:(NSButton *)sender
{
    BOOL on = (sender.state == NSControlStateValueOn);
    on_app_config_set(sender.identifier.UTF8String, on ? "1" : "0");
    OnApp *app = _lib.app;
    on_app_prefs_load(app);
    on_editor_settings_changed();
}

/*
 * list_density_changed: — the comfortable-rows checkbox was toggled.
 *
 * Inputs:
 *   sender — the checkbox
 *
 * Output: writes "list_density_comfortable", reloads prefs, notifies notes-changed.
 */
- (void)
list_density_changed:(NSButton *)sender
{
    BOOL on = (sender.state == NSControlStateValueOn);
    on_app_config_set("list_density_comfortable", on ? "1" : "0");
    OnApp *app = _lib.app;
    on_app_prefs_load(app);
    if (app->notify_notes_changed) app->notify_notes_changed(app);
}

/* ── Backup actions ─────────────────────────────────────────────────────── */

/*
 * backup_enabled_changed: — master backup switch toggled.
 *
 * Inputs:
 *   sender — the checkbox
 *
 * Output: writes "backup_enabled", rearms or stops the auto timer.
 */
- (void)
backup_enabled_changed:(NSButton *)sender
{
    BOOL on = (sender.state == NSControlStateValueOn);
    on_app_config_set("backup_enabled", on ? "1" : "0");
    OnApp *app = _lib.app;
    if (app->db) on_backup_auto_start(app, app->db->path);
}

/*
 * backup_dir_changed: — backup directory field committed.
 *
 * Inputs:
 *   sender — the text field
 *
 * Output: writes "backup_dir".
 */
- (void)
backup_dir_changed:(NSTextField *)sender
{
    NSString *val = sender.stringValue;
    on_app_config_set("backup_dir", val.length ? val.UTF8String : NULL);
}

/*
 * backup_choose_dir: — open-panel to pick the backup directory.
 *
 * Inputs:
 *   sender — unused
 *
 * Output: updates the text field and writes "backup_dir" on selection.
 */
- (void)
backup_choose_dir:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;
    panel.message = @"Choose a folder for automatic backups.";
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = panel.URL.path;
        self->_backup_dir_field.stringValue = path;
        on_app_config_set("backup_dir", path.UTF8String);
    }];
}

/*
 * backup_interval_changed: — interval field committed.
 *
 * Inputs:
 *   sender — the text field (integer minutes)
 *
 * Output: writes "backup_interval_min", rearms the timer.
 */
- (void)
backup_interval_changed:(NSTextField *)sender
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)sender.intValue);
    on_app_config_set("backup_interval_min", buf);
    OnApp *app = _lib.app;
    if (app->db) on_backup_auto_start(app, app->db->path);
}

/*
 * backup_keep_changed: — keep-N field committed.
 *
 * Inputs:
 *   sender — the text field (integer count)
 *
 * Output: writes "backup_keep".
 */
- (void)
backup_keep_changed:(NSTextField *)sender
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)sender.intValue);
    on_app_config_set("backup_keep", buf);
}

/*
 * backup_run_now: — run one backup pass immediately.
 *
 * Inputs:
 *   sender — unused
 *
 * Output: starts a backup if ready; updates the status label.
 */
- (void)
backup_run_now:(id)sender
{
    (void)sender;
    gchar *reason = NULL;
    if (!on_backup_ready(&reason)) {
        _backup_status.stringValue = reason ? @(reason) : @"Backup not ready.";
        g_free(reason);
        return;
    }
    _backup_status.stringValue = @"Backup started…";
    OnApp *app = _lib.app;
    if (app->db) on_backup_start(app, app->db->path);
}

/* ── NSWindowDelegate ───────────────────────────────────────────────────── */

/*
 * windowWillClose: — clear the db-health hook and nil the static instance.
 *
 * Inputs:
 *   notification — unused
 */
- (void)
windowWillClose:(NSNotification *)notification
{
    (void)notification;
    OnApp *app = _lib.app;
    if (app) app->notify_db_health = NULL;
    settings_window = nil;
}

@end

/* ── Command handler ────────────────────────────────────────────────────── */

/*
 * run_preferences — open or raise the Settings window.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — unused
 */
static void
run_preferences(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    if (!settings_window) {
        settings_window = [[NotesSettingsWindow alloc] initWithLibrary:lib];
    }
    [settings_window showWindow:nil];
    [settings_window.window makeKeyAndOrderFront:nil];
}

static const on_command_handler_t settings_handlers[] = {
    { @"preferences", run_preferences, NULL },
};

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_settings_install — see settings_window.h.
 */
void
on_settings_install(NotesLibrary *lib)
{
    on_library_add_commands(settings_handlers,
                            sizeof settings_handlers / sizeof *settings_handlers);
    (void)lib;
}
