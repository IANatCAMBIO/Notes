/*
 * settings_window.m — the Settings tool window (⌘,).
 *
 * Specification: the GTK build's src/settings_window.c.  One column,
 * as the GTK window and the Files app's Preferences have it: Appearance,
 * Editor, Database (the health plate and the rotating backups) and AI
 * Features, scrolling once it is taller than on_look_settings_max_h.
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

#import "editor_window.h"
#import "library_sidebar.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "backup.h"
#include "commands.h"
#include "config.h"
#include "db.h"

#include <sys/stat.h>

/* ── Layout ─────────────────────────────────────────────────────────────── */

/* Width of wrapped text (descriptions, hints, the backup folder). */
static const CGFloat text_w = 340.0;

/* Indent of a section's controls under its heading, and of AI's sub-group. */
static const CGFloat indent    = 12.0;
static const CGFloat sub_inset = 24.0;

/* How much wider than its content the window opens, for breathing room. */
static const CGFloat extra_w = 20.0;

/* The custom prompt editor's height. */
static const CGFloat prompt_h = 72.0;

/* How much of the 64-character digest is shown (GTK SHA_HEAD / SHA_TAIL). */
static const NSUInteger sha_head = 8;
static const NSUInteger sha_tail = 4;

/* The health LED: green only for a check that ran and passed. */
static NSString * const led_ok      = @"\U0001F7E2";
static NSString * const led_bad     = @"\U0001F534";
static NSString * const led_unknown = @"⚪";

/* ── The on/off settings ────────────────────────────────────────────────── */

/* What changes on screen when a setting flips. */
typedef enum {
    apply_notes,                     /* the library's rows                  */
    apply_sidebar_fit,               /* the sidebar's width                 */
    apply_editors,                   /* every open editor                   */
    apply_status_path,               /* the library and editor status bars  */
} apply_t;

/* One checkbox: its label, its ini key and what it changes. */
typedef struct {
    const char *label;
    const char *key;
    apply_t     apply;
} bool_setting_t;

static const bool_setting_t appearance_settings[] = {
    { "Show note counts next to folders and tags",   "sidebar_counts",      apply_notes       },
    { "Fit the sidebar width to the folders on show", "sidebar_fit_content", apply_sidebar_fit },
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
};

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
    };
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
        if (strcmp(fields[i].key, key) == 0) return *(const gboolean *)((const char *)app + fields[i].off);
    return NO;
}

/* ── Small builders ─────────────────────────────────────────────────────── */

/*
 * heading — a bold section heading.
 *
 * Inputs:
 *   text — the heading
 *
 * Output: the label.
 */
static NSTextField *
heading(NSString *text)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont boldSystemFontOfSize:on_look_ui_font];
    return label;
}

/*
 * note_text — small secondary text, wrapped (GTK's <small> lines).
 *
 * Inputs:
 *   text — the text
 *
 * Output: the label.
 */
static NSTextField *
note_text(NSString *text)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.textColor = NSColor.secondaryLabelColor;
    label.preferredMaxLayoutWidth = text_w;
    return label;
}

/*
 * plate_type — the health plate's text at the status bars' size
 * (on_look_status_font), the SHA-256 in monospace.  Applied after the
 * window's on_ui_use_ui_font, which would make it 11 pt.
 *
 * Inputs:
 *   root — the plate's grid, or a view inside it
 */
static void
plate_type(NSView *root)
{
    if ([root isKindOfClass:NSButton.class])
        ((NSButton *)root).font = [NSFont monospacedSystemFontOfSize:on_look_status_font weight:NSFontWeightRegular];
    else if ([root isKindOfClass:NSTextField.class])
        ((NSTextField *)root).font = [NSFont systemFontOfSize:on_look_status_font];
    for (NSView *v in root.subviews) plate_type(v);
}

/*
 * small_button — a small push button (the Database section's buttons sit
 * under the lines they act on, so they are quiet).
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

/*
 * health_stamp — when a check was made, in as few words as carry it:
 * "13:24 today", "Sep 8 at 09:02".
 *
 * Inputs:
 *   when — UNIX time
 *
 * Output: the text.
 */
static NSString *
health_stamp(gint64 when)
{
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:(double)when];
    return on_ui_format_date(when, [NSCalendar.currentCalendar isDateInToday:date]
                                   ? @"HH:mm 'today'" : @"MMM d 'at' HH:mm");
}

/*
 * shares_fate — is `dir` the folder `other`, or inside it?  The test
 * behind the backup folder's warning: the default backups folder is
 * INSIDE the database's folder (GTK dir_shares_fate).
 *
 * Inputs:
 *   dir, other — two folder paths
 *
 * Output: YES when losing `other` loses `dir`.
 */
static BOOL
shares_fate(NSString *dir, NSString *other)
{
    NSString *a = dir.stringByStandardizingPath, *b = other.stringByStandardizingPath;
    return [a isEqualToString:b] || [a hasPrefix:[b stringByAppendingString:@"/"]];
}

/*
 * policy_value — one backup-policy integer as the shared module applies it
 * (shared_backup_policy_get: the stored value or its default, held to its
 * range).
 *
 * Inputs:
 *   app — the application context, its database open
 *   key — a SHARED_SETTING_BACKUP_* integer key
 *
 * Output: the value (the module's default when it cannot be read).
 */
static gint64
policy_value(OnApp *app, const char *key)
{
    int64_t value = 0;
    shared_backup_policy_get(app->db->shared, key, &value, NULL);
    return value;
}

/*
 * policy_store — write one backup-policy integer to shared_settings, where
 * both apps read it; a failure is posted as a status message.
 *
 * Inputs:
 *   app   — the application context, its database open
 *   key   — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN or _KEEP
 *   value — the new value
 */
static void
policy_store(OnApp *app, const char *key, gint64 value)
{
    char  text[32];                  /* the value as shared_settings holds it */
    char *why = NULL;                /* the module's message                */
    snprintf(text, sizeof text, "%lld", (long long)value);
    if (shared_settings_set(app->db->shared, key, text, &why) != 0) {
        on_app_status(app, "Could not store %s: %s", key, why != NULL ? why : "unknown error");
    }
    free(why);
}

/* ── The window ─────────────────────────────────────────────────────────── */

@interface NotesSettingsWindow : NSWindowController <NSWindowDelegate, NSTextFieldDelegate, NSTextViewDelegate>
@property (nonatomic, weak)   NotesLibrary *lib;
@property (nonatomic, strong) NSTextField  *led;
@property (nonatomic, strong) NSTextField  *health;
@property (nonatomic, strong) NSTextField  *db_path;
@property (nonatomic, strong) NSTextField  *db_data;
@property (nonatomic, strong) NSTextField  *db_size;
@property (nonatomic, strong) NSButton     *sha;
@property (nonatomic, copy)   NSString     *sha_full;
@property (nonatomic, assign) NSUInteger    sha_gen;      /* the hash in flight */
@property (nonatomic, strong) NSButton     *backup_on;
@property (nonatomic, strong) NSTextField  *interval;
@property (nonatomic, strong) NSStepper    *interval_step;
@property (nonatomic, strong) NSTextField  *keep;
@property (nonatomic, strong) NSStepper    *keep_step;
@property (nonatomic, strong) NSTextField  *backup_path;
@property (nonatomic, strong) NSButton     *backup_choose;
@property (nonatomic, strong) NSButton     *backup_now;
@property (nonatomic, strong) NSTextField  *viewer;
@property (nonatomic, strong) NSTextField  *ai_command;
@property (nonatomic, strong) NSTextView   *ai_prompt;
@property (nonatomic, strong) NSStackView  *ai_sub;
@property (nonatomic, strong) NSStackView  *plate_box;     /* the plate and Update   */
@property (nonatomic, strong) NSGridView   *plate_grid;    /* the plate's lines      */
@property (nonatomic, strong) NSStackView  *backup_buttons;
- (void)refresh_db;
- (void)refresh_backup;
- (void)show_sha:(NSString *)full;
@end

/* The open Settings window, or nil. */
static NotesSettingsWindow *settings_window;

/*
 * settings_notify_db_health — OnApp.notify_db_health while the window is
 * open: a check started or landed, repaint the plate.
 *
 * Inputs:
 *   app — the application context (unused)
 */
static void
settings_notify_db_health(OnApp *app)
{
    [settings_window refresh_db];
}

@implementation NotesSettingsWindow

/*
 * init_with_lib: — build the window: one column of sections in a scroll
 * view, the panel sized to it up to on_look_settings_max_h.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the controller.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 400, 400)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskUtilityWindow
                                                  backing:NSBackingStoreBuffered defer:NO];
    panel.title              = @"Settings";
    panel.hidesOnDeactivate  = NO;
    panel.releasedWhenClosed = NO;
    if (!(self = [super initWithWindow:panel])) return nil;
    _lib = lib;
    panel.delegate = self;

    NSMutableArray<NSView *> *rows     = [NSMutableArray array];
    NSMutableArray<NSView *> *headings = [NSMutableArray array];
    NSMutableArray<NSView *> *ends     = [NSMutableArray array];   /* last row of a section */
    [headings addObject:[self add_appearance:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_editor:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_database:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_ai:rows]];

    NSStackView *stack = [NSStackView stackViewWithViews:rows];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment   = NSLayoutAttributeLeading;
    stack.spacing     = 6;
    stack.edgeInsets  = NSEdgeInsetsMake(16, 20, 20, 20);
    for (NSView *v in headings) [stack setCustomSpacing:8 afterView:v];
    for (NSView *v in ends)     [stack setCustomSpacing:18 afterView:v];
    /* The plate, Update and the backup buttons share the column's right
     * edge, as in GTK. */
    for (NSView *v in @[self.plate_box, self.backup_buttons])
        [v.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor constant:-stack.edgeInsets.right].active = YES;

    on_ui_use_ui_font(stack);
    plate_type(self.plate_grid);
    NSSize fit = stack.fittingSize;
    NotesFlippedView *doc = [[NotesFlippedView alloc] initWithFrame:NSZeroRect];
    doc.translatesAutoresizingMaskIntoConstraints   = NO;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [doc addSubview:stack];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = doc;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.drawsBackground     = NO;
    on_ui_pin_edges(stack, doc);
    [NSLayoutConstraint activateConstraints:@[
        [doc.topAnchor        constraintEqualToAnchor:scroll.contentView.topAnchor],
        [doc.leadingAnchor    constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [doc.trailingAnchor   constraintEqualToAnchor:scroll.contentView.trailingAnchor],
    ]];
    panel.contentView = scroll;
    CGFloat bar = fit.height > on_look_settings_max_h
                ? [NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy] : 0;
    [panel setContentSize:NSMakeSize(fit.width + extra_w + bar, MIN(fit.height, on_look_settings_max_h))];
    [panel center];

    [self refresh_db];
    [self refresh_backup];
    [self refresh_ai];
    lib.app->notify_db_health = settings_notify_db_health;
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
    NSTextField *head = heading(@"Appearance");
    [rows addObject:head];
    NSPopUpButton *density = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [density addItemsWithTitles:@[@"Compact", @"Comfortable"]];
    [density selectItemAtIndex:self.lib.app->comfortable_list ? 1 : 0];
    density.identifier = @"density";
    density.target     = self;
    density.action     = @selector(density_changed:);
    [rows addObject:indented(row(@[[NSTextField labelWithString:@"List density:"], density]), indent)];
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
    NSTextField *head = heading(@"Editor");
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
 * add_database: — the health plate and its Update button, then the
 * rotating backups.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_database:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = heading(@"Database");
    [rows addObject:head];

    self.led    = [NSTextField labelWithString:led_unknown];
    self.health = [NSTextField labelWithString:@""];
    self.health.identifier = @"health";
    self.db_path = [NSTextField wrappingLabelWithString:@""];
    self.db_path.selectable = YES;
    self.db_path.preferredMaxLayoutWidth = text_w - 120;
    self.db_data = [NSTextField labelWithString:@""];
    self.db_data.identifier = @"data";
    self.db_size = [NSTextField labelWithString:@""];
    self.sha = [NSButton buttonWithTitle:@"—" target:self action:@selector(copy_sha:)];
    self.sha.bordered = NO;

    NSArray<NSArray<NSView *> *> *cells = @[
        @[[NSTextField labelWithString:@"Health:"],           row(@[self.led, self.health])],
        @[[NSTextField labelWithString:@"Current database:"], self.db_path],
        @[[NSTextField labelWithString:@"Data:"],             self.db_data],
        @[[NSTextField labelWithString:@"Size on disk:"],     self.db_size],
        @[[NSTextField labelWithString:@"SHA-256:"],          self.sha],
    ];
    NSGridView *grid = self.plate_grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing    = 2;
    grid.columnSpacing = 8;
    grid.rowAlignment  = NSGridRowAlignmentFirstBaseline;
    grid.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox *plate = [[NSBox alloc] initWithFrame:NSZeroRect];
    plate.boxType         = NSBoxCustom;
    plate.fillColor       = NSColor.controlBackgroundColor;
    plate.borderColor     = NSColor.separatorColor;
    plate.borderWidth     = 1;
    plate.cornerRadius    = 6;
    plate.contentViewMargins = NSMakeSize(10, 8);
    plate.titlePosition   = NSNoTitle;
    [plate.contentView addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor      constraintEqualToAnchor:plate.contentView.topAnchor],
        [grid.bottomAnchor   constraintEqualToAnchor:plate.contentView.bottomAnchor],
        [grid.leadingAnchor  constraintEqualToAnchor:plate.contentView.leadingAnchor],
        [grid.trailingAnchor constraintLessThanOrEqualToAnchor:plate.contentView.trailingAnchor],
    ]];

    NSButton *update = small_button(@"Update", self, @selector(update_health:));
    update.toolTip = @"Re-read every line above: run PRAGMA integrity_check and PRAGMA foreign_key_check "
                      "against this database, then re-count its notes and folders and re-read its size "
                      "and SHA-256.";
    NSStackView *plate_box = self.plate_box = [NSStackView stackViewWithViews:@[plate, update]];
    plate_box.orientation = NSUserInterfaceLayoutOrientationVertical;
    plate_box.alignment   = NSLayoutAttributeTrailing;
    plate_box.spacing     = 5;
    plate_box.edgeInsets  = NSEdgeInsetsMake(0, indent, 0, 0);
    [rows addObject:plate_box];

    OnApp *app = self.lib.app;
    self.backup_on = [NSButton checkboxWithTitle:@"Back up the database automatically" target:self
                                          action:@selector(backup_toggled:)];
    self.backup_on.state = policy_value(app, SHARED_SETTING_BACKUP_ENABLED) != 0
                           ? NSControlStateValueOn : NSControlStateValueOff;
    self.backup_on.toolTip = @"Writes a verified copy of the database Notes shares with Tasks into a "
                              "folder of your choice on a timer, keeping only the most recent few.  The "
                              "policy is stored in that database, so Tasks follows it too, and whichever "
                              "app is running makes the backup.  Worth pointing at a disk INDEPENDENT of "
                              "wherever the database itself lives, so one mishap cannot take both.";
    [rows addObject:indented(self.backup_on, indent)];

    self.interval = [self number_field:SHARED_SETTING_BACKUP_INTERVAL_MIN
                                 value:(int)policy_value(app, SHARED_SETTING_BACKUP_INTERVAL_MIN)
                                   min:0 max:SHARED_BACKUP_INTERVAL_MIN_MAX
                                  step:SHARED_BACKUP_INTERVAL_MIN_STEP stepper:&_interval_step];
    self.interval.toolTip = @"Minutes between backups.  0 backs up only when you press Back Up Now.";
    self.keep = [self number_field:SHARED_SETTING_BACKUP_KEEP
                             value:(int)policy_value(app, SHARED_SETTING_BACKUP_KEEP)
                               min:SHARED_BACKUP_KEEP_MIN max:SHARED_BACKUP_KEEP_MAX step:1
                           stepper:&_keep_step];
    self.keep.toolTip = @"How many backup files to retain.  The oldest are removed once a NEW backup has "
                         "been verified, never before.";
    [rows addObject:indented(row(@[[NSTextField labelWithString:@"Every"], self.interval, self.interval_step,
                                   [NSTextField labelWithString:@"minutes, keeping"], self.keep, self.keep_step,
                                   [NSTextField labelWithString:@"files"]]), indent)];

    self.backup_path = note_text(@"");
    self.backup_path.identifier = @"backup-path";
    [rows addObject:indented(self.backup_path, indent)];
    self.backup_choose = small_button(@"Change Folder…", self, @selector(choose_backup_folder:));
    self.backup_now    = small_button(@"Back Up Now", self, @selector(backup_now:));
    self.backup_buttons = row(@[]);
    [self.backup_buttons addView:self.backup_choose inGravity:NSStackViewGravityTrailing];
    [self.backup_buttons addView:self.backup_now inGravity:NSStackViewGravityTrailing];
    [rows addObject:self.backup_buttons];
    return head;
}

/*
 * number_field:value:min:max:step:stepper: — a small number field with
 * its stepper, for one integer backup setting; either writes the setting.
 *
 * Inputs:
 *   key      — the shared_settings key
 *   value    — its current value
 *   min, max — the range (the field clamps to it)
 *   step     — the stepper's increment
 *   stepper  — receives the stepper
 *
 * Output: the field (its identifier is the key).
 */
- (NSTextField *)
number_field:(const char *)key value:(int)value min:(int)min max:(int)max step:(int)step
     stepper:(NSStepper * __strong *)stepper
{
    NSNumberFormatter *fmt = [[NSNumberFormatter alloc] init];
    fmt.minimum = @(min);
    fmt.maximum = @(max);
    fmt.allowsFloats = NO;
    NSTextField *field = [NSTextField textFieldWithString:@""];
    field.formatter  = fmt;
    field.integerValue = MAX(min, MIN(max, value));
    field.identifier = @(key);
    field.alignment  = NSTextAlignmentRight;
    field.target     = self;
    field.action     = @selector(number_changed:);
    [field.widthAnchor constraintEqualToConstant:52].active = YES;
    NSStepper *s = [[NSStepper alloc] initWithFrame:NSZeroRect];
    s.minValue     = min;
    s.maxValue     = max;
    s.increment    = step;
    s.integerValue = field.integerValue;
    s.identifier   = @(key);
    s.target       = self;
    s.action       = @selector(number_changed:);
    *stepper = s;
    return field;
}

/*
 * add_ai: — the AI master switch and, enabled with it, the command and
 * the custom prompt.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_ai:(NSMutableArray<NSView *> *)rows
{
    OnApp *app = self.lib.app;
    NSTextField *head = heading(@"AI Features");
    [rows addObject:head];
    [rows addObject:indented(note_text(@"The AI button in the library toolbar generates a summary of the current "
                                        "folder’s notes using an external AI command. Normal or Project mode is "
                                        "set per-folder in the New/Rename Folder dialog. This switch is a master "
                                        "kill switch: disabling it hides the button and prevents all AI commands "
                                        "from running."), indent)];
    NSButton *enable = [NSButton checkboxWithTitle:@"Enable AI features" target:self action:@selector(ai_toggled:)];
    enable.state = app->ai_enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [rows addObject:indented(enable, indent)];

    self.ai_command = [NSTextField textFieldWithString:app->ai_command ? @(app->ai_command) : @""];
    self.ai_command.identifier        = @"ai-command";
    self.ai_command.placeholderString = @"e.g. /usr/local/bin/claude";
    self.ai_command.delegate          = self;
    [self.ai_command.widthAnchor constraintEqualToConstant:text_w - sub_inset - 90].active = YES;

    NSScrollView *prompt_scroll = [NSTextView scrollableTextView];
    self.ai_prompt = prompt_scroll.documentView;
    self.ai_prompt.identifier = @"ai-prompt";
    self.ai_prompt.string     = app->ai_custom_prompt ? @(app->ai_custom_prompt) : @"";
    self.ai_prompt.font       = [NSFont systemFontOfSize:on_look_ui_font];
    self.ai_prompt.richText   = NO;
    self.ai_prompt.textContainerInset = NSMakeSize(4, 4);
    self.ai_prompt.delegate   = self;
    prompt_scroll.borderType  = NSBezelBorder;
    prompt_scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [prompt_scroll.widthAnchor  constraintEqualToConstant:text_w - sub_inset].active = YES;
    [prompt_scroll.heightAnchor constraintEqualToConstant:prompt_h].active = YES;

    NSTextField *cmd_hint = note_text(@"Run “which claude” in a terminal to find your Claude Code command path. "
                                       "The command must read the prompt from stdin and write the response to "
                                       "stdout.");
    NSTextField *prompt_hint = note_text(@"Used when a folder’s AI mode is set to Custom. Notes and action items "
                                          "are appended after this prompt.");
    cmd_hint.preferredMaxLayoutWidth = prompt_hint.preferredMaxLayoutWidth = text_w - sub_inset;
    self.ai_sub = [NSStackView stackViewWithViews:@[
        row(@[[NSTextField labelWithString:@"AI command:"], self.ai_command]), cmd_hint,
        [NSTextField labelWithString:@"Custom prompt:"], prompt_scroll, prompt_hint,
    ]];
    self.ai_sub.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.ai_sub.alignment   = NSLayoutAttributeLeading;
    self.ai_sub.spacing     = 4;
    self.ai_sub.edgeInsets  = NSEdgeInsetsMake(0, sub_inset, 0, 0);
    [rows addObject:self.ai_sub];
    return head;
}

/* ── Refreshing ───────────────────────────────────────────────────────── */

/*
 * refresh_db — every line of the health plate from the file and the last
 * recorded check (reading the stamp, never running a check).
 */
- (void)
refresh_db
{
    OnDatabase       *db = self.lib.app->db;
    const OnDbHealth *h  = on_db_health(db);
    NSString *led, *text, *tip;
    if (h == NULL) {
        led  = led_unknown;
        text = @"Not checked";
        tip  = @"Nothing has verified this database yet.  Press Update.";
    } else if (h->pending) {
        led  = led_unknown;
        text = @"Checking…";
        tip  = @"PRAGMA integrity_check is running on a worker thread; the verdict lands here.";
    } else {
        NSString *when = health_stamp(h->when);
        led  = h->ok ? led_ok : led_bad;
        text = h->ok ? [NSString stringWithFormat:@"Healthy — checked %@", when]
             : h->ran ? [NSString stringWithFormat:@"Problems found — %@", when]
                      : [NSString stringWithFormat:@"Check did not complete — %@", when];
        tip  = h->detail ? @(h->detail)
             : @"PRAGMA integrity_check and PRAGMA foreign_key_check both passed against this file.";
    }
    self.led.stringValue    = led;
    self.health.stringValue = text;
    self.health.toolTip     = tip;
    self.db_path.stringValue = @(db->path);

    gint notes = 0, folders = 0;
    on_db_totals(db, &notes, &folders, NULL);
    self.db_data.stringValue = [NSString stringWithFormat:@"%d note%s in %d folder%s",
                                notes, notes == 1 ? "" : "s", folders, folders == 1 ? "" : "s"];
    struct stat st;
    self.db_size.stringValue = stat(db->path, &st) == 0 ? on_ui_format_bytes((int64_t)st.st_size) : @"unknown";

    /* The whole file is read for the digest: on a worker, so a large or
     * iCloud-held database never stalls the window.  A newer refresh
     * outdates the one in flight.                                          */
    self.sha.title   = @"…";
    self.sha.enabled = NO;
    NSUInteger gen  = ++self.sha_gen;
    NSString  *path = @(db->path);
    __weak NotesSettingsWindow *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            gchar    *sha  = on_db_file_sha256(path.fileSystemRepresentation);
            NSString *full = sha ? @(sha) : nil;
            g_free(sha);
            dispatch_async(dispatch_get_main_queue(), ^{
                NotesSettingsWindow *s = ws;
                if (s && s.sha_gen == gen) [s show_sha:full];
            });
        }
    });
}

/*
 * show_sha: — the digest line of the health plate: its head and tail,
 * the whole of it in the tooltip and on a click.
 *
 * Inputs:
 *   full — the hex digest; nil when the file could not be read
 */
- (void)
show_sha:(NSString *)full
{
    self.sha_full = full;
    if (self.sha_full.length > sha_head + sha_tail) {
        self.sha.title   = [NSString stringWithFormat:@"%@…%@", [self.sha_full substringToIndex:sha_head],
                            [self.sha_full substringFromIndex:self.sha_full.length - sha_tail]];
        self.sha.toolTip = [NSString stringWithFormat:@"%@\n\nThe file as it stands.  A database in use changes "
                            "with the next edit, so this moves.\n\nClick to copy.", self.sha_full];
        self.sha.enabled = YES;
    } else {
        self.sha.title   = @"—";
        self.sha.toolTip = @"The database file could not be read.";
        self.sha.enabled = NO;
    }
}

/*
 * refresh_backup — the backup controls follow the master switch, and the
 * folder line names where backups land, with a warning when that folder
 * goes with the database's (GTK bk_section_refresh).
 */
- (void)
refresh_backup
{
    BOOL on = self.backup_on.state == NSControlStateValueOn;
    for (NSControl *c in @[self.interval, self.interval_step, self.keep, self.keep_step,
                           self.backup_choose, self.backup_now])
        c.enabled = on;
    gchar *why   = NULL;
    gchar *dir_c = on_backup_dir(self.lib.app, &why);
    if (dir_c == NULL) {
        self.backup_path.stringValue = [NSString stringWithFormat:@"No backup folder: %@", @(why)];
        g_free(why);
        return;
    }
    NSString *dir = @(dir_c);
    g_free(dir_c);
    NSString *db_dir = @(self.lib.app->db->path).stringByDeletingLastPathComponent;
    NSString *text   = shares_fate(dir, db_dir)
        ? [NSString stringWithFormat:@"Backing up to %@\n⚠ alongside the database — change it to survive "
                                      "losing that folder", dir]
        : [NSString stringWithFormat:@"Backing up to %@", dir];
    char *last_error = NULL;         /* why the last backup failed, if it did */
    if (shared_settings_get(self.lib.app->db->shared, SHARED_SETTING_BACKUP_LAST_ERROR,
                            &last_error, NULL) == 1 && last_error[0] != '\0') {
        text = [text stringByAppendingFormat:@"\n⚠ The last backup failed: %@", @(last_error)];
    }
    free(last_error);
    self.backup_path.stringValue = text;
}

/*
 * refresh_ai — the command and prompt are usable only while AI is on.
 */
- (void)
refresh_ai
{
    BOOL on = self.lib.app->ai_enabled;
    self.ai_command.enabled  = on;
    self.ai_prompt.editable  = on;
    self.ai_prompt.textColor = on ? NSColor.textColor : NSColor.disabledControlTextColor;
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
 * bool_toggled: — a checkbox: write it, then apply it live.
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
 * controlTextDidChange: — the image viewer or AI command, written as it is
 * typed; empty clears the setting.
 *
 * Inputs:
 *   note — the notification (its object is the field)
 */
- (void)
controlTextDidChange:(NSNotification *)note
{
    NSTextField *field = note.object;
    const char  *value = field.stringValue.length ? field.stringValue.UTF8String : NULL;
    if (field == self.viewer)          [self write_setting:"image_viewer" value:value];
    else if (field == self.ai_command) [self write_setting:"ai_command" value:value];
}

/*
 * textDidChange: — the custom prompt, written as it is typed.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
textDidChange:(NSNotification *)note
{
    NSString *text = self.ai_prompt.string;
    [self write_setting:"ai_custom_prompt" value:text.length ? text.UTF8String : NULL];
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
    on_ui_pick_path(NotesPickOpenFile, @"Choose Image Viewer", @"Select", @[UTTypeApplicationBundle],
                    @"/Applications", ^(NSString *path) {
        if (path == nil || ws == nil) return;
        ws.viewer.stringValue = path;
        [ws write_setting:"image_viewer" value:path.UTF8String];
    });
}

/*
 * update_health: — Update: a health pass on a worker (THE check, as at
 * launch: its verdict lands in the status bar, problems in a tool panel,
 * and the plate refreshes through notify_db_health); the plate shows
 * "Checking…" meanwhile.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
update_health:(id)sender
{
    on_app_db_health_start(self.lib.app);   /* the verdict reaches the plate and the status bar */
    [self refresh_db];
}

/*
 * copy_sha: — the full digest onto the clipboard.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
copy_sha:(id)sender
{
    if (self.sha_full == nil) return;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:self.sha_full forType:NSPasteboardTypeString];
    on_app_status(self.lib.app, "SHA-256 copied to the clipboard");
}

/*
 * backup_toggled: — the backup switch, written to shared_settings (the
 * periodic poll reads it there; Tasks too).
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
backup_toggled:(NSButton *)sender
{
    policy_store(self.lib.app, SHARED_SETTING_BACKUP_ENABLED,
                 sender.state == NSControlStateValueOn ? 1 : 0);
    [self refresh_backup];
}

/*
 * number_changed: — the interval or retention field or stepper: the pair
 * agree and the value is written to shared_settings.
 *
 * Inputs:
 *   sender — the field or stepper (identifier = key)
 */
- (void)
number_changed:(NSControl *)sender
{
    BOOL         is_interval = [sender.identifier isEqualToString:@SHARED_SETTING_BACKUP_INTERVAL_MIN];
    NSTextField *field       = is_interval ? self.interval : self.keep;
    NSStepper   *stepper     = is_interval ? self.interval_step : self.keep_step;
    NSInteger    value       = MAX((NSInteger)stepper.minValue, MIN((NSInteger)stepper.maxValue, sender.integerValue));
    field.integerValue   = value;
    stepper.integerValue = value;
    policy_store(self.lib.app, sender.identifier.UTF8String, value);
}

/*
 * choose_backup_folder: — pick where backups go, starting where they go
 * now, and store it (a refused folder is reported in a tool panel).
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
choose_backup_folder:(id)sender
{
    gchar    *dir_c = on_backup_dir(self.lib.app, NULL);
    NSString *dir   = dir_c ? @(dir_c) : nil;
    g_free(dir_c);
    __weak NotesSettingsWindow *ws = self;
    on_ui_pick_path(NotesPickFolder, @"Choose Backup Folder", @"Select", nil, dir,
                    ^(NSString *path) {
        NotesSettingsWindow *s = ws;
        if (path == nil || s == nil) return;
        char *why = NULL;            /* the module's reason for refusing it */
        if (shared_db_set_backup_dir(s.lib.app->db->shared, path.fileSystemRepresentation, &why) != 0) {
            on_ui_show_notice(@"That folder cannot be used", why ? @(why) : @"Unknown error.", s.window, nil);
            free(why);
        }
        [s refresh_backup];
    });
}

/*
 * backup_now: — one backup pass; the outcome is reported in the status bar.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
backup_now:(id)sender
{
    on_backup_start(self.lib.app);
}

/*
 * ai_toggled: — the AI master switch: write it, enable the rest with it,
 * and let the library show or hide its AI control.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
ai_toggled:(NSButton *)sender
{
    [self write_setting:"ai_enabled" value:sender.state == NSControlStateValueOn ? "1" : "0"];
    [self refresh_ai];
    OnApp *app = self.lib.app;
    if (app->notify_ai_changed) app->notify_ai_changed(app);
}

/*
 * windowWillClose: — the plate's hook goes, and the window with it.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    if (self.lib.app->notify_db_health == settings_notify_db_health) self.lib.app->notify_db_health = NULL;
    settings_window = nil;
}

@end

/* ── Command ────────────────────────────────────────────────────────────── */

/*
 * run_preferences — "preferences": open the Settings window, or bring it
 * forward with its backup line re-read (the last backup's outcome).
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
    } else {
        [settings_window refresh_backup];   /* a pass may have failed since */
    }
    [settings_window showWindow:nil];
    [settings_window.window makeKeyAndOrderFront:nil];
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t settings_handlers[] = {
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
