/* ===========================================================================
 * editor_window.m — note editor window
 *
 * One NSWindowController per open note.  The window has an ordinary title
 * bar and nothing in it — never an NSToolbar (a unified "headerbar") or a
 * titlebar accessory.  Its body is, top to bottom: the format bar, the
 * NSScrollView whose documentView is the NotesNoteView,
 * and the status bar.  The modal image viewer (image_viewer.h) is layered
 * over the scroll view while a picture is on show.
 *
 * COMMANDS.  Every editor command (find, the inline and paragraph styles,
 * insert) is a window-scope command of commands.h, run by this window's
 * controller from run_command: — the menu bar, the format bar's buttons
 * and menus and every shortcut all reach the same handler table below.
 * While the find field has the keyboard focus the editing commands are
 * disabled, so ⌘B and the like are plain keys in the field (GTK's "gate").
 *
 * Specification: the GTK build's src/editor_window.c.
 * See editor_window.h for the public API and ownership contract.
 * =========================================================================== */

#import "editor_window.h"
#import "commands.h"
#import "image_viewer.h"
#import "library_state.h"
#import "note_view.h"
#import "ui_util.h"

#include "app_core.h"
#include "config.h"
#include "db.h"
#include "document.h"
#include "identity.h"
#include "image_io.h"
#include "look.h"
#include "note_edit.h"
#include "serialize.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <glib.h>

/* ── Timing ─────────────────────────────────────────────────────────────── */

/* Seconds of quiet after the last edit before the note is saved. */
static const double autosave_delay = 1.2;

/* Open editors: note id -> window controller.  Main thread only. */
static NSMutableDictionary<NSNumber *, NSWindowController *> *editors;

/* ── Status bar ─────────────────────────────────────────────────────────── */

/*
 * NotesStatusBar — the strip along the window's bottom edge: the note's
 * location (left), its id when the setting asks for it, and the save-state
 * dot (right): red with unsaved edits, green once they are written.
 */
@interface NotesStatusBar : NSView
@property (nonatomic, strong) NSTextField *path_label;
@property (nonatomic, strong) NSTextField *id_label;
@property (nonatomic, strong) NSTextField *dot_label;
- (void)update_path:(NSString *)path note_id:(NSString *)note_id dirty:(BOOL)dirty;
@end

@implementation NotesStatusBar

/*
 * initWithFrame: — build the bar: location left, then id and dot right,
 * at a fixed height.
 *
 * Inputs:
 *   frame — initial frame (Auto Layout places the bar)
 *
 * Output: the bar.
 */
- (instancetype)
initWithFrame:(NSRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *path    = on_ui_status_label(self, self.centerYAnchor, NSTextAlignmentLeft);
    NSTextField *note_id = on_ui_status_label(self, self.centerYAnchor, NSTextAlignmentRight);
    NSTextField *dot     = on_ui_status_label(self, self.centerYAnchor, NSTextAlignmentRight);
    self.path_label = path;
    self.id_label   = note_id;
    self.dot_label  = dot;

    [NSLayoutConstraint activateConstraints:@[
        [path.leadingAnchor     constraintEqualToAnchor:self.leadingAnchor constant:on_look_status_inset],
        [dot.trailingAnchor     constraintEqualToAnchor:self.trailingAnchor constant:-on_look_status_inset],
        [note_id.trailingAnchor constraintEqualToAnchor:dot.leadingAnchor constant:-on_look_status_inset],
        [path.trailingAnchor    constraintLessThanOrEqualToAnchor:note_id.leadingAnchor
                                                         constant:-on_look_status_inset],
        [self.heightAnchor      constraintEqualToConstant:on_look_status_h],
    ]];
    return self;
}

/*
 * update_path:note_id:dirty: — refresh the three labels.
 *
 * Inputs:
 *   path    — the note's location, as on_app_location_text gives it
 *   note_id — "id:N", or nil to hide the id
 *   dirty   — YES when there are unsaved edits
 */
- (void)
update_path:(NSString *)path note_id:(NSString *)note_id dirty:(BOOL)dirty
{
    self.path_label.stringValue = path ?: @"";
    self.id_label.stringValue   = note_id ?: @"";
    self.id_label.hidden        = note_id == nil;
    self.dot_label.stringValue  = @"●";
    self.dot_label.textColor    = dirty ? NSColor.systemRedColor : NSColor.systemGreenColor;
    self.dot_label.toolTip      = dirty ? @"Unsaved changes" : @"Saved";
}

@end

/* ── Format bar helpers ─────────────────────────────────────────────────── */

/*
 * format_button — one button of the format bar: an SF Symbol or a short
 * text label, in the recessed style of the Files app's bookmark bar.  It
 * runs its command through the responder chain like a menu item does.
 *
 * Inputs:
 *   symbol  — SF Symbols name; nil = show `face` as text
 *   face    — the text label when there is no symbol
 *   ident   — the command (commands.h) it runs; its tip is the hover help
 *   toggle  — YES for an on/off button (the inline styles)
 *
 * Output: the button, with a fixed height.
 */
static NSButton *
format_button(NSString *symbol, NSString *face, NSString *ident, BOOL toggle)
{
    NSString *tooltip = on_command_tip(ident);
    NSButton *b = [NSButton buttonWithTitle:face target:nil action:@selector(run_command:)];
    b.identifier = ident;
    b.bezelStyle = NSBezelStyleRecessed;
    b.showsBorderOnlyWhileMouseInside = YES;
    b.toolTip    = tooltip;
    b.refusesFirstResponder = YES;
    if (toggle) [b setButtonType:NSButtonTypePushOnPushOff];
    NSImage *img = symbol ? [NSImage imageWithSystemSymbolName:symbol
                                      accessibilityDescription:tooltip] : nil;
    if (img) {
        b.image = [img imageWithSymbolConfiguration:
                   [NSImageSymbolConfiguration configurationWithPointSize:on_look_format_icon - 3
                                                                   weight:NSFontWeightRegular]];
        b.imagePosition = NSImageOnly;
    }
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b.heightAnchor constraintEqualToConstant:on_look_format_btn_h].active = YES;
    return b;
}

/*
 * format_menu — a pull-down of the format bar whose items are commands
 * (with their shortcuts shown, as in the menu bar).
 *
 * Inputs:
 *   face    — the pull-down's own title, e.g. @"Insert"
 *   tooltip — hover help
 *   idents  — the commands, in order
 *
 * Output: the pop-up button.
 */
static NSPopUpButton *
format_menu(NSString *face, NSString *tooltip, NSArray<NSString *> *idents)
{
    NSPopUpButton *menu = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    menu.bezelStyle = NSBezelStyleRecessed;
    menu.showsBorderOnlyWhileMouseInside = YES;
    menu.font       = [NSFont systemFontOfSize:on_look_ui_font];
    menu.toolTip    = tooltip;
    menu.refusesFirstResponder = YES;
    [menu addItemWithTitle:face];                  /* the pull-down's own title */
    for (NSString *ident in idents) [menu.menu addItem:on_command_menu_item(ident)];
    menu.translatesAutoresizingMaskIntoConstraints = NO;
    [menu.heightAnchor constraintEqualToConstant:on_look_format_btn_h].active = YES;
    return menu;
}

/*
 * group_separator — the short vertical rule between groups of the format
 * bar.
 *
 * Output: the view, for Auto Layout.
 */
static NSBox *
group_separator(void)
{
    NSBox *sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    [sep.widthAnchor constraintEqualToConstant:1].active = YES;
    [sep.heightAnchor constraintEqualToConstant:on_look_format_btn_h - 6].active = YES;
    return sep;
}

/*
 * separator — a 1 pt horizontal rule.
 *
 * Output: the view, for Auto Layout.
 */
static NSBox *
separator(void)
{
    NSBox *sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    return sep;
}

/* ── Window controller ───────────────────────────────────────────────────── */

@interface NotesEditorWindow : NSWindowController <NSWindowDelegate, NSSearchFieldDelegate>

@property (nonatomic, assign) OnApp            *app;
@property (nonatomic, assign) gint64            note_id;
@property (nonatomic, strong) NotesNoteView    *note_view;
@property (nonatomic, strong) NSScrollView     *scroll;       /* holds note_view      */
@property (nonatomic, strong) NSView           *format_bar;
@property (nonatomic, strong) NSStackView      *format_groups;/* rebuilt on settings  */
@property (nonatomic, strong) NotesStatusBar   *status_bar;
@property (nonatomic, strong) NotesImageViewer *image_viewer;

/* Format bar toggle buttons, kept to mirror the inline format state. */
@property (nonatomic, copy)   NSArray<NSButton *> *inline_buttons;

/* In-note find field at the format bar's right edge. */
@property (nonatomic, strong) NSSearchField *search_field;

@property (nonatomic, assign) BOOL    dirty;          /* unsaved edits       */
@property (nonatomic, assign) BOOL    discard;        /* close without saving:
                                                         the note is gone, or
                                                         the user chose to   */
@property (nonatomic, assign) BOOL    sync_actions;   /* action line touched */
@property (nonatomic, assign) BOOL    sync_tags;      /* tag links to write  */
@property (nonatomic, strong) NSTimer *autosave_timer;/* nil = none          */

- (BOOL)save;
- (BOOL)close_saved;
- (void)update_status;
- (void)reload_if_changed;
@end

/* ── Commands ───────────────────────────────────────────────────────────── */

/* The style each inline / paragraph command applies. */
static const struct {
    NSString * __unsafe_unretained ident;
    guint32                         flag;
} style_commands[] = {
    { @"inline-bold",      ON_FMT_BOLD        },
    { @"inline-italic",    ON_FMT_ITALIC      },
    { @"inline-underline", ON_FMT_UNDERLINE   },
    { @"inline-strike",    ON_FMT_STRIKE      },
    { @"para-h1",          ON_FMT_H1          },
    { @"para-h2",          ON_FMT_H2          },
    { @"para-body",        0                  },
    { @"para-bullet",      ON_FMT_LIST_BULLET },
    { @"para-number",      ON_FMT_LIST_NUMBER },
    { @"para-check",       ON_FMT_LIST_CHECK  },
    { @"para-code",        ON_FMT_CODEBLOCK   },
};

/*
 * style_flag — the style a command applies.
 *
 * Inputs:
 *   ident — the command
 *
 * Output: its flag (0 for Body); NO in *found for a command not in the
 *         table.
 */
static guint32
style_flag(NSString *ident, BOOL *found)
{
    for (size_t i = 0; i < sizeof style_commands / sizeof *style_commands; i++) {
        if ([style_commands[i].ident isEqualToString:ident]) {
            *found = YES;
            return style_commands[i].flag;
        }
    }
    *found = NO;
    return 0;
}

/*
 * find_has_focus — is the find field being typed in?  (The gate: editing
 * commands are off then, so their shortcuts stay keys of the field.)
 *
 * Inputs:
 *   ed — the editor
 *
 * Output: YES while the field's editor is the first responder.
 */
static BOOL
find_has_focus(NotesEditorWindow *ed)
{
    NSResponder *first = ed.window.firstResponder;
    return [first isKindOfClass:NSText.class] &&
           ((NSText *)first).delegate == (id)ed.search_field;
}

/*
 * run_style — the inline-* and para-* commands: apply the style at the
 * caret, then give the note the focus back (a format bar button was
 * clicked).
 *
 * Inputs:
 *   target — the editor
 *   sender — the item; its identifier names the command
 */
static void
run_style(id target, id sender)
{
    NotesEditorWindow *ed = target;
    NSString *ident = on_command_ident(sender);
    BOOL found;
    guint32 flag = style_flag(ident, &found);
    if (!found) return;
    if ([ident hasPrefix:@"inline-"]) {
        on_note_view_toggle_inline(ed.note_view, flag);
    } else {
        on_note_view_toggle_paragraph(ed.note_view, flag);
    }
    [ed.window makeFirstResponder:ed.note_view];
}

/*
 * validate_editing — the editing commands run while the find field does
 * not have the focus.
 *
 * Inputs:
 *   target — the editor
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_editing(id target, id item)
{
    return !find_has_focus(target);
}

/*
 * run_insert_image — "insert-image": pick an image file and embed it at the
 * caret.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item (unused)
 */
static void
run_insert_image(id target, id sender)
{
    __weak NotesEditorWindow *ws = target;
    on_ui_pick_path(NotesPickOpenFile, @"Insert Image", @"Insert",
                    @[UTTypeImage], nil, ^(NSString *path) {
        NotesEditorWindow *ed = ws;
        if (!ed || !path) return;
        size_t   n   = 0;
        uint8_t *png = on_image_file_to_png(path.fileSystemRepresentation, &n);   /* any format */
        if (png) {
            on_note_view_insert_image(ed.note_view, png, n);
            free(png);
        } else {
            on_ui_show_notice(@"That file is not an image", path.lastPathComponent, ed.window, nil);
        }
        [ed.window makeFirstResponder:ed.note_view];
    });
}

/*
 * run_insert_table — "insert-table": a new table at the caret.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item (unused)
 */
static void
run_insert_table(id target, id sender)
{
    NotesEditorWindow *ed = target;
    on_note_view_insert_table(ed.note_view);
    [ed.window makeFirstResponder:ed.note_view];
}

/*
 * run_insert_emoji — "insert-emoji": AppKit's character palette, which
 * inserts its choice through the note view's text input.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item (unused)
 */
static void
run_insert_emoji(id target, id sender)
{
    NotesEditorWindow *ed = target;
    [ed.window makeFirstResponder:ed.note_view];
    [NSApp orderFrontCharacterPalette:sender];
}

/*
 * run_insert_date — "insert-date": today's date at the caret.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item (unused)
 */
static void
run_insert_date(id target, id sender)
{
    NotesEditorWindow *ed = target;
    on_note_view_insert_date(ed.note_view);
    [ed.window makeFirstResponder:ed.note_view];
}

/*
 * run_find — "find" (⌘F): into the find field, its text selected.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item (unused)
 */
static void
run_find(id target, id sender)
{
    NotesEditorWindow *ed = target;
    [ed.window makeFirstResponder:ed.search_field];
    [ed.search_field selectText:nil];
}

/*
 * run_find_step — "find-next" / "find-previous" (⌘G, ⇧⌘G, the arrow
 * buttons): select the next or previous match of the field's text,
 * wrapping at the ends.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item; its identifier says which way
 */
static void
run_find_step(id target, id sender)
{
    NotesEditorWindow *ed = target;
    BOOL forward = ![on_command_ident(sender) isEqualToString:@"find-previous"];
    on_note_view_find_step(ed.note_view, ed.search_field.stringValue, forward);
}

/*
 * validate_find_step — stepping needs something to find.
 *
 * Inputs:
 *   target — the editor
 *   item   — the item (unused)
 *
 * Output: YES when the field has text.
 */
static BOOL
validate_find_step(id target, id item)
{
    return ((NotesEditorWindow *)target).search_field.stringValue.length > 0;
}

/*
 * run_find_selection — "find-selection" (⌘E): the selected text becomes
 * the find text, its matches highlighted.
 *
 * Inputs:
 *   target — the editor
 *   sender — the item (unused)
 */
static void
run_find_selection(id target, id sender)
{
    NotesEditorWindow *ed = target;
    NSString *text = on_note_view_selected_text(ed.note_view);
    if (text.length == 0) return;
    ed.search_field.stringValue = text;
    on_note_view_find(ed.note_view, text);
}

/*
 * validate_find_selection — needs a selection in the note.
 *
 * Inputs:
 *   target — the editor
 *   item   — the item (unused)
 *
 * Output: YES when text is selected.
 */
static BOOL
validate_find_selection(id target, id item)
{
    NotesEditorWindow *ed = target;
    return !find_has_focus(ed) && on_note_view_selected_text(ed.note_view).length > 0;
}

/* Every command an editor runs. */
static const on_command_handler_t editor_commands[] = {
    { @"find",             run_find,           NULL                    },
    { @"find-next",        run_find_step,      validate_find_step      },
    { @"find-previous",    run_find_step,      validate_find_step      },
    { @"find-selection",   run_find_selection, validate_find_selection },
    { @"inline-bold",      run_style,          validate_editing        },
    { @"inline-italic",    run_style,          validate_editing        },
    { @"inline-underline", run_style,          validate_editing        },
    { @"inline-strike",    run_style,          validate_editing        },
    { @"para-h1",          run_style,          validate_editing        },
    { @"para-h2",          run_style,          validate_editing        },
    { @"para-body",        run_style,          validate_editing        },
    { @"para-bullet",      run_style,          validate_editing        },
    { @"para-number",      run_style,          validate_editing        },
    { @"para-check",       run_style,          validate_editing        },
    { @"para-code",        run_style,          validate_editing        },
    { @"insert-image",     run_insert_image,   validate_editing        },
    { @"insert-table",     run_insert_table,   validate_editing        },
    { @"insert-emoji",     run_insert_emoji,   validate_editing        },
    { @"insert-date",      run_insert_date,    validate_editing        },
};
#define EDITOR_COMMAND_COUNT (sizeof editor_commands / sizeof *editor_commands)

@implementation NotesEditorWindow

/* ── Construction ──────────────────────────────────────────────────────── */

/*
 * init_with_app:note:document:title:search_term:reveal:ordinal: — create
 * the editor for one note; the caller registers and shows it.
 *
 * Inputs:
 *   app         — application context
 *   note_id     — the note
 *   doc         — its loaded document; the editor's view takes ownership
 *   title       — the note's title for the window
 *   search_term — pre-populate the find field (nil = none)
 *   reveal      — what to scroll to on open
 *   ordinal     — which image or action item (0 for RevealNone)
 *
 * Output: the controller.
 */
- (instancetype)
init_with_app:(OnApp *)app
         note:(gint64)note_id
     document:(OnDocument *)doc
        title:(NSString *)title
  search_term:(NSString *)search_term
       reveal:(NotesEditorReveal)reveal
      ordinal:(NSInteger)ordinal
{
    gint win_w = (gint)on_look_editor_w;
    gint win_h = (gint)on_look_editor_h;
    on_app_config_get_size("editor_win_w", "editor_win_h", &win_w, &win_h);
    NSRect frame = NSMakeRect(0, 0, win_w, win_h);

    NotesKeyFilterWindow *window = [[NotesKeyFilterWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable |
                             NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title              = title;
    window.releasedWhenClosed = NO;
    window.minSize            = NSMakeSize(480, 360);

    self = [super initWithWindow:window];
    if (!self) return nil;

    self.app     = app;
    self.note_id = note_id;

    [self build_note_view];
    [self build_layout];
    [self build_image_viewer];
    on_ui_use_ui_font(window.contentView);   /* the note's own text is drawn by the note view */
    [self load_content:doc];

    __weak NotesEditorWindow *ws = self;
    window.key_filter = ^BOOL(NSEvent *event) {
        NotesEditorWindow *s = ws;
        if (!s || !on_image_viewer_is_open(s.image_viewer)) return NO;
        on_image_viewer_key_down(s.image_viewer, event);
        return YES;                  /* modal: the note behind sees no key */
    };
    window.delegate = self;
    [self place_window];
    [window makeFirstResponder:self.note_view];

    /* Deferred: reveal / search need a laid-out view. */
    if (search_term.length > 0) {
        NSString *term = [search_term copy];
        dispatch_async(dispatch_get_main_queue(), ^{
            [ws apply_search:term];
        });
    }
    if (reveal != NotesEditorRevealNone) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [ws reveal:reveal ordinal:ordinal];
        });
    }
    return self;
}

/*
 * place_window — put a new editor at the bottom-right of the library's
 * screen (its own when there is no library), on_look_editor_screen_margin
 * clear of the work area's edges, its title bar always on screen.
 */
- (void)
place_window
{
    NSScreen *screen = on_library().window.screen ?: NSScreen.mainScreen;
    if (!screen) return;
    NSRect area  = screen.visibleFrame;
    NSRect frame = self.window.frame;
    NSPoint origin = {
        MAX(NSMaxX(area) - on_look_editor_screen_margin - frame.size.width, area.origin.x),
        MIN(area.origin.y + on_look_editor_screen_margin, NSMaxY(area) - frame.size.height),
    };
    [self.window setFrameOrigin:origin];
}

/*
 * build_note_view — create the NotesNoteView inside an NSScrollView
 * (placed by build_layout) and wire the editing handlers.
 */
- (void)
build_note_view
{
    NotesNoteView *view = [[NotesNoteView alloc] init_with_app:self.app];
    view.autoresizingMask = NSViewWidthSizable;   /* the text wraps at the window's width;
                                                   * the view sets its own height */
    self.note_view = view;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = view;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.drawsBackground     = NO;
    scroll.focusRingType       = NSFocusRingTypeNone;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    self.scroll = scroll;

    __weak NotesEditorWindow *ws = self;
    view.edited_handler = ^{
        [ws queue_autosave];
    };
    view.inline_flags_handler = ^{
        [ws sync_format_buttons];
    };
    view.image_activated_handler = ^(NSInteger ordinal) {
        NotesEditorWindow *s = ws;
        if (s) on_image_viewer_open(s.image_viewer, ordinal);
    };
}

/*
 * build_layout — fill the window under its ordinary title bar: the format
 * bar, a rule, the note, a rule and the status bar.  Nothing is put in the
 * title bar (no NSToolbar, no titlebar accessory): the GTK editor's
 * formatting row was part of the window body, and so is this one.
 */
- (void)
build_layout
{
    NSView *content = self.window.contentView;
    NSView *bar     = [self build_format_bar];
    NSBox  *top_sep = separator();
    NSBox  *bot_sep = separator();
    NotesStatusBar *status = [[NotesStatusBar alloc] initWithFrame:NSZeroRect];
    self.status_bar = status;

    for (NSView *v in @[bar, top_sep, self.scroll, bot_sep, status]) [content addSubview:v];

    [NSLayoutConstraint activateConstraints:@[
        [bar.topAnchor          constraintEqualToAnchor:content.topAnchor],
        [bar.leadingAnchor      constraintEqualToAnchor:content.leadingAnchor],
        [bar.trailingAnchor     constraintEqualToAnchor:content.trailingAnchor],
        [bar.heightAnchor       constraintEqualToConstant:on_look_format_bar_h],

        [top_sep.topAnchor      constraintEqualToAnchor:bar.bottomAnchor],
        [top_sep.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [top_sep.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],

        [self.scroll.topAnchor      constraintEqualToAnchor:top_sep.bottomAnchor],
        [self.scroll.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [self.scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [self.scroll.bottomAnchor   constraintEqualToAnchor:bot_sep.topAnchor],

        [bot_sep.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [bot_sep.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [bot_sep.bottomAnchor   constraintEqualToAnchor:status.topAnchor],

        [status.leadingAnchor   constraintEqualToAnchor:content.leadingAnchor],
        [status.trailingAnchor  constraintEqualToAnchor:content.trailingAnchor],
        [status.bottomAnchor    constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

/*
 * build_format_bar — the formatting row: the style groups (rebuilt by
 * rebuild_format_groups) and, at the right edge, the find field with its
 * previous / next buttons.  The window's minimum width keeps every group
 * on show, as the GTK toolbar's natural width did.
 *
 * Output: the bar, for build_layout to place.
 */
- (NSView *)
build_format_bar
{
    NSSearchField *field = [[NSSearchField alloc] initWithFrame:NSZeroRect];
    field.placeholderString            = @"Find in note";
    field.controlSize                  = NSControlSizeSmall;
    field.sendsWholeSearchString       = NO;
    field.sendsSearchStringImmediately = YES;
    field.target   = self;
    field.action   = @selector(search_field_changed:);
    field.delegate = self;
    field.translatesAutoresizingMaskIntoConstraints = NO;
    self.search_field = field;

    NSStackView *find = [NSStackView stackViewWithViews:@[
        field,
        format_button(@"chevron.up",   @"", @"find-previous", NO),
        format_button(@"chevron.down", @"", @"find-next",     NO),
    ]];
    find.spacing = on_look_format_gap;
    find.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *bar = [[NSView alloc] initWithFrame:NSZeroRect];
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:find];
    self.format_bar = bar;
    [NSLayoutConstraint activateConstraints:@[
        [find.trailingAnchor  constraintEqualToAnchor:bar.trailingAnchor constant:-on_look_status_inset],
        [find.centerYAnchor   constraintEqualToAnchor:bar.centerYAnchor],
        [field.widthAnchor    constraintEqualToConstant:on_look_find_w],
    ]];
    [self rebuild_format_groups];
    return bar;
}

/*
 * rebuild_format_groups — (re)make the style groups for the current
 * compact_editor_toolbar setting: in compact mode the heading/body buttons
 * become one "Aa" menu and the three list buttons one "≡" menu.  The find
 * field is not touched, so a search survives.
 */
- (void)
rebuild_format_groups
{
    [self.format_groups removeFromSuperview];

    NSButton *bold      = format_button(@"bold",          @"", @"inline-bold",      YES);
    NSButton *italic    = format_button(@"italic",        @"", @"inline-italic",    YES);
    NSButton *underline = format_button(@"underline",     @"", @"inline-underline", YES);
    NSButton *strike    = format_button(@"strikethrough", @"", @"inline-strike",    YES);
    self.inline_buttons = @[bold, italic, underline, strike];

    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithArray:self.inline_buttons];
    [views addObject:format_button(@"curlybraces", @"", @"para-code", NO)];
    [views addObject:group_separator()];
    if (self.app->compact_editor_toolbar) {
        [views addObject:format_menu(@"Aa", @"Styles — paragraph style: heading or body text",
                                     @[@"para-h1", @"para-h2", @"para-body"])];
        [views addObject:group_separator()];
        [views addObject:format_menu(@"≡", @"Lists — bullets, numbers, or task checkboxes",
                                     @[@"para-bullet", @"para-number", @"para-check"])];
    } else {
        [views addObject:format_button(nil, @"H1",   @"para-h1",   NO)];
        [views addObject:format_button(nil, @"H2",   @"para-h2",   NO)];
        [views addObject:format_button(nil, @"Body", @"para-body", NO)];
        [views addObject:group_separator()];
        [views addObject:format_button(@"list.bullet", @"", @"para-bullet", NO)];
        [views addObject:format_button(@"list.number", @"", @"para-number", NO)];
        [views addObject:format_button(@"checklist",   @"", @"para-check",  NO)];
    }
    [views addObject:group_separator()];
    [views addObject:format_menu(@"+", @"Insert an image, a table, an emoji, or today's date at the cursor",
                                 @[@"insert-image", @"insert-table", @"insert-emoji", @"insert-date"])];

    NSStackView *groups = [NSStackView stackViewWithViews:views];
    groups.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    groups.spacing     = on_look_format_gap;
    groups.translatesAutoresizingMaskIntoConstraints = NO;
    [self.format_bar addSubview:groups];
    self.format_groups = groups;
    on_ui_use_ui_font(groups);

    NSView *find = self.search_field.superview;
    [NSLayoutConstraint activateConstraints:@[
        [groups.leadingAnchor constraintEqualToAnchor:self.format_bar.leadingAnchor
                                             constant:on_look_status_inset],
        [groups.centerYAnchor constraintEqualToAnchor:self.format_bar.centerYAnchor],
        [find.leadingAnchor   constraintGreaterThanOrEqualToAnchor:groups.trailingAnchor
                                                          constant:on_look_format_group_gap],
    ]];
    [self sync_format_buttons];
}

/*
 * build_image_viewer — the image viewer over the scroll view: it walks the
 * note's images, names the note and the picture's size, and offers "Open
 * in Image Viewer" (GTK: editor_viewer_*).
 */
- (void)
build_image_viewer
{
    __weak NotesEditorWindow *ws = self;

    NotesImageViewer *viewer = on_image_viewer_new(self.scroll, @"Open in Image Viewer",
        @"Open this image at full size in an external viewer");
    viewer.count_handler = ^NSInteger {
        return on_note_view_image_count(ws.note_view);
    };
    viewer.image_handler = ^NSImage *(NSInteger idx, NSSize box) {
        NotesEditorWindow *s = ws;
        GBytes *png = s ? on_note_view_image_png(s.note_view, idx) : NULL;
        if (!png) return nil;
        gsize n;
        const uint8_t *bytes = g_bytes_get_data(png, &n);
        return on_ui_thumbnail(bytes, n, box.width, box.height, s.window.backingScaleFactor);
    };
    viewer.caption_handler = ^NSString *(NSInteger idx) {
        NotesEditorWindow *s = ws;
        CGImageRef image = s ? on_note_view_image(s.note_view, idx) : NULL;
        if (!image) return nil;
        gchar *title = on_document_title(on_note_view_document(s.note_view),
                                         ON_DEFAULT_NOTE_TITLE, ON_TITLE_MAX_CHARS);
        NSString *caption = [NSString stringWithFormat:@"%@ — image %ld of %ld — %zu × %zu",
                             @(title), (long)idx + 1, (long)on_note_view_image_count(s.note_view),
                             CGImageGetWidth(image), CGImageGetHeight(image)];
        g_free(title);
        return caption;
    };
    viewer.action_handler = ^(NSInteger idx) {
        NotesEditorWindow *s = ws;
        if (s) on_note_view_image_open(s.note_view, idx);
    };
    self.image_viewer = viewer;
}

/*
 * load_content: — put the note's document, reconciled against its task
 * rows, into the note view.  A reconciled line is an unsaved change, so
 * the autosave is queued to write it back.
 *
 * Inputs:
 *   doc — the loaded document; the note view takes ownership
 */
- (void)
load_content:(OnDocument *)doc
{
    OnApp      *app    = self.app;
    gboolean    healed = on_note_document_reconcile(app->db, self.note_id, doc);
    on_note_view_load_document(self.note_view, doc);   /* the view owns doc now */
    if (healed) [self queue_autosave];
    [self update_status];
}

/* ── Commands ──────────────────────────────────────────────────────────── */

/*
 * run_command: — a window-scope command (commands.h) reached this window
 * through the responder chain: from the menu bar, a shortcut, or the
 * format bar.
 *
 * Inputs:
 *   sender — the menu item or button
 */
- (void)
run_command:(id)sender
{
    if (on_image_viewer_is_open(self.image_viewer)) return;
    on_command_run(editor_commands, EDITOR_COMMAND_COUNT, self, sender);
}

/*
 * validateUserInterfaceItem: — enable a command's item while this editor
 * can run it; other items are AppKit's to judge.
 *
 * Inputs:
 *   item — the menu item being validated
 *
 * Output: YES to enable it.
 */
- (BOOL)
validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (item.action != @selector(run_command:)) return YES;
    if (on_image_viewer_is_open(self.image_viewer)) return NO;
    BOOL handled;
    BOOL ok = on_command_validate(editor_commands, EDITOR_COMMAND_COUNT, self, item, &handled);
    return handled && ok;
}

/*
 * sync_format_buttons — mirror the note view's current inline flags into
 * the format bar's toggle buttons.
 */
- (void)
sync_format_buttons
{
    guint32 flags = on_note_view_inline_flags(self.note_view);
    for (NSButton *b in self.inline_buttons) {
        BOOL found;
        guint32 flag = style_flag(b.identifier, &found);
        b.state = (flags & flag) ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

/* ── Find field ─────────────────────────────────────────────────────────── */

/*
 * search_field_changed: — the find field changed: highlight every match.
 *
 * Inputs:
 *   sender — the search field
 */
- (void)
search_field_changed:(NSSearchField *)sender
{
    on_note_view_find(self.note_view, sender.stringValue);
}

/*
 * control:textView:doCommandBySelector: — the find field's keys: Return
 * and Down step to the next match, Up to the previous, Escape goes back to
 * the note (keeping the search, as GTK's stop-search does).
 *
 * Inputs:
 *   control   — the find field
 *   text_view — its field editor (unused)
 *   command   — the key's command selector
 *
 * Output: YES when the key was handled here.
 */
- (BOOL)
control:(NSControl *)control textView:(NSTextView *)text_view doCommandBySelector:(SEL)command
{
    if (control != self.search_field) return NO;
    if (command == @selector(insertNewline:) || command == @selector(moveDown:)) {
        on_note_view_find_step(self.note_view, self.search_field.stringValue, YES);
        return YES;
    }
    if (command == @selector(moveUp:)) {
        on_note_view_find_step(self.note_view, self.search_field.stringValue, NO);
        return YES;
    }
    if (command == @selector(cancelOperation:)) {
        [self.window makeFirstResponder:self.note_view];
        return YES;
    }
    return NO;
}

/*
 * apply_search: — put text in the find field, highlight the matches and
 * select the first one (a note opened from the search window).
 *
 * Inputs:
 *   term — search text
 */
- (void)
apply_search:(NSString *)term
{
    self.search_field.stringValue = term;
    on_note_view_find(self.note_view, term);
}

/*
 * reveal:ordinal: — scroll to the note's ordinal-th image or action item.
 *
 * Inputs:
 *   reveal  — what to show
 *   ordinal — which one
 */
- (void)
reveal:(NotesEditorReveal)reveal ordinal:(NSInteger)ordinal
{
    if (reveal == NotesEditorRevealImage)  on_note_view_image_reveal(self.note_view, ordinal);
    if (reveal == NotesEditorRevealAction) on_note_view_action_reveal(self.note_view, ordinal);
}

/* ── Saving ─────────────────────────────────────────────────────────────── */

/*
 * queue_autosave — (re)arm the autosave debounce timer; mark dirty.
 */
- (void)
queue_autosave
{
    if (!self.dirty) {
        self.dirty = YES;
        [self update_status];
    }
    [self.autosave_timer invalidate];
    __weak NotesEditorWindow *ws = self;
    self.autosave_timer =
        [NSTimer scheduledTimerWithTimeInterval:autosave_delay
                                        repeats:NO
                                          block:^(NSTimer *t) {
        NotesEditorWindow *s = ws;
        if (!s) return;
        s.autosave_timer = nil;
        if (![s save]) [s queue_autosave];   /* still unsaved: try again */
    }];
}

/*
 * save — persist the note, its tag set when that changed, and tell the
 * library: the full refresh when a tag or a task row changed (the sidebar
 * shows them), the light one-note refresh otherwise.
 *
 * Output: YES when the note was written; NO when the save failed (the
 * edits are still unsaved).
 */
- (BOOL)
save
{
    OnApp      *app = self.app;
    OnDocument *doc = on_note_view_document(self.note_view);

    if (on_note_view_take_actions_modified(self.note_view)) self.sync_actions = YES;

    gboolean actions_changed = FALSE;
    if (!on_note_document_save(app->db, self.note_id, doc, self.sync_actions,
                               &actions_changed)) {
        on_app_status(app, "Could not save note %" G_GINT64_FORMAT, self.note_id);
        return NO;
    }
    self.sync_actions = NO;

    if (on_note_view_take_tags_modified(self.note_view)) self.sync_tags = YES;
    BOOL tags_changed = self.sync_tags;
    if (tags_changed) {
        GList   *tags = on_document_collect_tags(doc);
        gboolean ok   = on_db_note_set_tags(app->db, self.note_id, tags);
        g_list_free_full(tags, g_free);
        if (!ok) {                     /* kept pending: the retry writes them */
            on_app_status(app, "Could not save the tags of note %" G_GINT64_FORMAT, self.note_id);
            return NO;
        }
        self.sync_tags = NO;
    }
    self.dirty = NO;

    gchar *title = on_document_title(doc, ON_DEFAULT_NOTE_TITLE, ON_TITLE_MAX_CHARS);
    self.window.title = @(title);
    g_free(title);

    [self update_status];

    if (tags_changed || actions_changed) {
        if (app->notify_notes_changed != NULL) app->notify_notes_changed(app);
    } else {
        if (app->notify_note_saved != NULL) app->notify_note_saved(app, self.note_id);
    }
    return YES;
}

/*
 * reload_if_changed — after an outside command wrote the database: an
 * editor whose note is gone closes without saving; one holding an edit
 * made through live_document saves it; one whose note was rewritten in
 * the database (its stored blob is no longer the editor's document)
 * reloads it, so the editor never writes a stale copy over the change.
 */
- (void)
reload_if_changed
{
    OnApp      *app  = self.app;
    OnNoteMeta *meta = on_db_note_get(app->db, self.note_id);
    if (meta == NULL) {
        self.discard = YES;
        [self.window close];
        return;
    }
    on_db_note_meta_free(meta);
    if (self.dirty) {
        [self.autosave_timer invalidate];
        self.autosave_timer = nil;
        [self save];
        return;
    }

    guint8 *stored = NULL;
    gsize   stored_len = 0;
    if (!on_db_note_load(app->db, self.note_id, &stored, &stored_len)) return;
    gsize   mine_len = 0;
    guint8 *mine = on_document_to_bnbf(on_note_view_document(self.note_view), &mine_len);
    BOOL same = stored_len == mine_len && memcmp(stored, mine, mine_len) == 0;
    g_free(mine);
    g_free(stored);
    if (same) return;

    OnDocument *doc = on_note_document_load(app->db, self.note_id);
    if (doc == NULL) {                 /* now unreadable: leave it alone */
        self.discard = YES;
        [self.window close];
        return;
    }
    [self load_content:doc];
    gchar *title = on_document_title(doc, ON_DEFAULT_NOTE_TITLE, ON_TITLE_MAX_CHARS);
    self.window.title = @(title);
    g_free(title);
}

/*
 * update_status — refresh the status bar: the note's folder path ("/" at
 * the top level, "/Folder/Sub" below; with the database path in front when
 * statusbar_db_path is on), "id:N" when statusbar_note_id is on, and the
 * save-state dot.
 */
- (void)
update_status
{
    OnApp      *app  = self.app;
    NSString   *path = @"";
    OnNoteMeta *meta = on_db_note_get(app->db, self.note_id);
    if (meta != NULL) {
        gchar *full = on_app_folder_location(app, meta->folder_id);
        path = @(full);
        g_free(full);
        on_db_note_meta_free(meta);
    }
    NSString *note_id = app->statusbar_note_id
        ? [NSString stringWithFormat:@"id:%" G_GINT64_FORMAT, self.note_id] : nil;
    [self.status_bar update_path:path note_id:note_id dirty:self.dirty];
}

/* ── NSWindowDelegate ───────────────────────────────────────────────────── */

/*
 * windowDidBecomeKey: — the editor is in front again: re-read the note's
 * location, so a move or folder rename made in the library while it sat
 * in the background shows.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowDidBecomeKey:(NSNotification *)note
{
    [self update_status];
}

/*
 * windowShouldClose: — the user closes the window: unsaved edits are saved
 * first, and a save that fails keeps the window open, with a tool panel
 * offering to close without saving.
 *
 * Inputs:
 *   sender — the window
 *
 * Output: YES when the window may close now.
 */
- (BOOL)
windowShouldClose:(NSWindow *)sender
{
    if (!self.dirty || self.discard || [self save]) return YES;
    __weak NotesEditorWindow *ws = self;
    on_ui_confirm([NSString stringWithFormat:@"Could not save \u201C%@\u201D", self.window.title],
                  @"Your changes are still in the window. Closing it now loses them.",
                  @"Close Without Saving", self.window, ^(BOOL confirmed) {
        NotesEditorWindow *s = ws;
        if (!s || !confirmed) return;
        s.discard = YES;
        [s.window close];
    });
    return NO;
}

/*
 * close_saved — close the window unless it holds edits that cannot be
 * saved now (programmatic closes: quit, database switch, trash, delete).
 *
 * Output: YES when the window closed; NO when it stays open because its
 * save failed.
 */
- (BOOL)
close_saved
{
    if (self.dirty && !self.discard) {
        [self.autosave_timer invalidate];
        self.autosave_timer = nil;
        if (![self save]) {
            [self queue_autosave];
            return NO;
        }
    }
    [self.window close];
    return YES;
}

/*
 * windowWillClose: — teardown: stop the timer, save or (a blank note)
 * delete, leave the registry.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    OnApp *app = self.app;

    [self.autosave_timer invalidate];
    self.autosave_timer                    = nil;
    self.note_view.edited_handler          = nil;
    self.note_view.inline_flags_handler    = nil;
    self.note_view.image_activated_handler = nil;
    ((NotesKeyFilterWindow *)self.window).key_filter = nil;

    OnDocument *doc = on_note_view_document(self.note_view);
    if (self.discard) {
        /* The note is gone or unreadable, or the user let the edits go. */
    } else if (on_note_edit_is_blank(doc)) {
        gint64 nid = self.note_id;
        on_db_notes_delete(app->db, &nid, 1);
        on_app_status(app, "Deleted empty note");
        if (app->notify_notes_changed != NULL) app->notify_notes_changed(app);
    } else if (self.dirty) {
        [self save];
    }
    [editors removeObjectForKey:@(self.note_id)]; /* last: may release self */
}

/*
 * on_settings_changed — re-read preferences: the view's (title line, code
 * line numbers, …), the format bar's compact mode, the status bar's path
 * and note id.
 */
- (void)
on_settings_changed
{
    on_note_view_settings_changed(self.note_view);
    [self rebuild_format_groups];
    [self update_status];
}

@end

/* ── Module-level helpers ────────────────────────────────────────────────── */

/*
 * editor_for — the open editor of a note, or nil.
 *
 * Inputs:
 *   note_id — the note
 *
 * Output: the window controller, or nil.
 */
static NotesEditorWindow *
editor_for(gint64 note_id)
{
    return (NotesEditorWindow *)editors[@(note_id)];
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * on_editor_window_open — see editor_window.h.
 */
BOOL
on_editor_window_open(OnApp *app, gint64 note_id, NotesEditorReveal reveal,
                      NSInteger ordinal, NSString *search_term)
{
    NotesEditorWindow *editor = editor_for(note_id);
    if (editor) {
        [editor reveal:reveal ordinal:ordinal];
        if (search_term.length > 0) [editor apply_search:search_term];
        [editor showWindow:nil];
        [NSApp activate];
        return YES;
    }
    OnNoteMeta *meta = on_db_note_get(app->db, note_id);
    if (meta == NULL) return NO;
    NSString *title = @(meta->title);
    on_db_note_meta_free(meta);

    /* A note that cannot be read is never shown as an empty one: an empty
     * editor would delete it on close, and a save would overwrite it.     */
    OnDocument *doc = on_note_document_load(app->db, note_id);
    if (doc == NULL) {
        on_ui_show_notice([NSString stringWithFormat:@"Could not open \u201C%@\u201D", title],
                          @"The note could not be read. It has not been changed.", nil, nil);
        return NO;
    }

    editor = [[NotesEditorWindow alloc]
              init_with_app:app note:note_id document:doc title:title
              search_term:search_term reveal:reveal ordinal:ordinal];
    editors[@(note_id)] = editor;
    [editor showWindow:nil];
    [NSApp activate];
    return YES;
}

/*
 * on_editor_close_notes — see editor_window.h.
 */
BOOL
on_editor_close_notes(const gint64 *ids, size_t n)
{
    BOOL ok = YES;
    for (size_t i = 0; i < n; i++) {
        NotesEditorWindow *editor = editor_for(ids[i]);
        if (editor && ![editor close_saved]) ok = NO;
    }
    return ok;
}

/*
 * on_editor_report_unsaved — see editor_window.h.
 */
void
on_editor_report_unsaved(NSString *title, NSWindow *window)
{
    on_ui_show_notice(title, @"A note could not be saved. Its window is still open with your changes.",
                      window, nil);
}

/*
 * on_editor_close_all — see editor_window.h.
 */
BOOL
on_editor_close_all(void)
{
    BOOL ok = YES;
    for (NotesEditorWindow *editor in editors.allValues) {   /* a copy: closing edits editors */
        if (![editor close_saved]) ok = NO;
    }
    return ok;
}

/*
 * on_editor_settings_changed — see editor_window.h.
 */
void
on_editor_settings_changed(void)
{
    for (NotesEditorWindow *editor in editors.allValues) {
        [editor on_settings_changed];
    }
}

/* ── Core hook implementations ───────────────────────────────────────────── */

/*
 * hook_open_note — OnApp.open_note: open (or raise) a note's editor.
 *
 * Inputs:
 *   app     — the application context
 *   note_id — the note
 *
 * Output: FALSE when the note does not exist.
 */
static gboolean
hook_open_note(OnApp *app, gint64 note_id)
{
    return on_editor_window_open(app, note_id, NotesEditorRevealNone, 0, nil);
}

/*
 * hook_live_document — OnApp.live_document: an open editor's document, for
 * an edit made from outside (an action ticked in the library or the CLI).
 *
 * Inputs:
 *   app     — the application context (unused)
 *   note_id — the note
 *
 * Output: the document, or NULL when the note is not open.
 */
static OnDocument *
hook_live_document(OnApp *app, gint64 note_id)
{
    NotesEditorWindow *editor = editor_for(note_id);
    return editor ? on_note_view_begin_external_edit(editor.note_view) : NULL;
}

/*
 * hook_live_edited — OnApp.live_edited: the outside edit is done; the view
 * redraws and the autosave picks it up.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   note_id — the note
 */
static void
hook_live_edited(OnApp *app, gint64 note_id)
{
    on_note_view_end_external_edit(editor_for(note_id).note_view);
}

/*
 * hook_save_editors — OnApp.save_editors: every editor with unsaved edits
 * saves now; the windows stay open.
 *
 * Inputs:
 *   app — the application context (unused)
 *
 * Output: FALSE when any save failed.
 */
static gboolean
hook_save_editors(OnApp *app)
{
    gboolean ok = TRUE;
    for (NotesEditorWindow *editor in editors.allValues) {
        if (!editor.dirty) continue;
        [editor.autosave_timer invalidate];
        editor.autosave_timer = nil;
        if (![editor save]) ok = FALSE;
    }
    return ok;
}

/*
 * hook_reload_editors — OnApp.reload_editors: every editor catches up with
 * a database an outside command just wrote (reload_if_changed).
 *
 * Inputs:
 *   app — the application context (unused)
 */
static void
hook_reload_editors(OnApp *app)
{
    for (NotesEditorWindow *editor in editors.allValues) {   /* a copy: closing edits editors */
        [editor reload_if_changed];
    }
}

/*
 * hook_flush_editors — OnApp.flush_editors: close every editor, each
 * saving first (the flush-on-quit contract).
 *
 * Inputs:
 *   app — the application context (unused)
 *
 * Output: FALSE when an editor could not save and is still open.
 */
static gboolean
hook_flush_editors(OnApp *app)
{
    return on_editor_close_all();
}

/*
 * on_editor_install — see editor_window.h.
 */
void
on_editor_install(OnApp *app)
{
    editors             = [NSMutableDictionary dictionary];
    app->open_note      = hook_open_note;
    app->live_document  = hook_live_document;
    app->live_edited    = hook_live_edited;
    app->save_editors   = hook_save_editors;
    app->reload_editors = hook_reload_editors;
    app->flush_editors  = hook_flush_editors;
}
