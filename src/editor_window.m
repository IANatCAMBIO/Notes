/* ===========================================================================
 * editor_window.m — note editor window (work package D)
 *
 * One NSWindowController per open note.  The window's content view is an
 * NSScrollView whose documentView is the NotesNoteView (work package C).
 * The format toolbar is an NSToolbar; the status bar is a bottom
 * NSTitlebarAccessoryViewController.  The modal image viewer (image_viewer.h)
 * is layered over the scroll view while a picture is on show.
 *
 * See editor_window.h for the public API and ownership contract.
 * =========================================================================== */

#import "editor_window.h"
#import "image_viewer.h"
#import "note_view.h"
#import "ui_util.h"

#include "app_core.h"
#include "config.h"
#include "db.h"
#include "document.h"
#include "identity.h"
#include "look.h"
#include "note_edit.h"
#include "serialize.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <glib.h>

/* ── Timing ─────────────────────────────────────────────────────────────── */

/* Seconds of quiet after the last edit before the note is saved. */
static const double autosave_delay = 1.2;

/* ── Toolbar identifier ─────────────────────────────────────────────────── */

static NSToolbarIdentifier const editor_toolbar_id = @"NotesEditor";

/* ── Toolbar item identifiers ───────────────────────────────────────────── */

static NSToolbarItemIdentifier const item_bold      = @"bold";
static NSToolbarItemIdentifier const item_italic    = @"italic";
static NSToolbarItemIdentifier const item_underline = @"underline";
static NSToolbarItemIdentifier const item_strike    = @"strike";
static NSToolbarItemIdentifier const item_code      = @"code";
static NSToolbarItemIdentifier const item_h1        = @"h1";
static NSToolbarItemIdentifier const item_h2        = @"h2";
static NSToolbarItemIdentifier const item_body      = @"body";
static NSToolbarItemIdentifier const item_bullet    = @"bullet";
static NSToolbarItemIdentifier const item_number    = @"number";
static NSToolbarItemIdentifier const item_check     = @"check";
static NSToolbarItemIdentifier const item_insert    = @"insert";
static NSToolbarItemIdentifier const item_find      = @"find-field";

/* Open editors: note id -> window controller.  Main thread only. */
static NSMutableDictionary<NSNumber *, NSWindowController *> *editors;

/* ── Status bar accessory ───────────────────────────────────────────────── */

/*
 * NotesStatusBar — the bottom status bar: note path (left) and save-state
 * dot (right).  Lives as a NSTitlebarAccessoryViewController.
 */
@interface NotesStatusBar : NSTitlebarAccessoryViewController
@property (nonatomic, weak) NSTextField *path_label;
@property (nonatomic, weak) NSTextField *dot_label;
- (void)update_path:(NSString *)path dirty:(BOOL)dirty;
@end

@implementation NotesStatusBar

/*
 * loadView — build the status bar view: path label left, dot right.
 *
 * Inputs: none
 * Output: sets self.view.
 */
- (void)
loadView
{
    NSView *bar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, on_look_status_h)];

    NSTextField *path = [NSTextField labelWithString:@""];
    path.font              = [NSFont systemFontOfSize:on_look_status_font];
    path.lineBreakMode     = NSLineBreakByTruncatingMiddle;
    path.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:path];
    self.path_label = path;

    NSTextField *dot = [NSTextField labelWithString:@""];
    dot.font           = [NSFont systemFontOfSize:on_look_status_font];
    dot.alignment      = NSTextAlignmentRight;
    dot.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:dot];
    self.dot_label = dot;

    NSLayoutConstraint *constraints[] = {
        [path.leadingAnchor  constraintEqualToAnchor:bar.leadingAnchor  constant:on_look_status_inset],
        [path.centerYAnchor  constraintEqualToAnchor:bar.centerYAnchor],
        [dot.trailingAnchor  constraintEqualToAnchor:bar.trailingAnchor constant:-(CGFloat)on_look_status_inset],
        [dot.centerYAnchor   constraintEqualToAnchor:bar.centerYAnchor],
        [path.trailingAnchor constraintLessThanOrEqualToAnchor:dot.leadingAnchor constant:-4],
        [bar.heightAnchor    constraintEqualToConstant:on_look_status_h],
    };
    [NSLayoutConstraint activateConstraints:
        [NSArray arrayWithObjects:constraints count:sizeof constraints / sizeof *constraints]];

    self.view            = bar;
    self.layoutAttribute = NSLayoutAttributeBottom;
    self.fullScreenMinHeight = on_look_status_h;
}

/*
 * update_path:dirty: — refresh the path label and dirty dot.
 *
 * Inputs:
 *   path  — the note's folder path (may be empty)
 *   dirty — YES when there are unsaved edits
 *
 * Output: none.
 */
- (void)
update_path:(NSString *)path dirty:(BOOL)dirty
{
    self.path_label.stringValue = path ?: @"";
    self.dot_label.stringValue  = dirty ? @"●" : @"";
    self.dot_label.textColor    = dirty ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

@end

/* ── Window controller ───────────────────────────────────────────────────── */

@interface NotesEditorWindow : NSWindowController
    <NSWindowDelegate, NSToolbarDelegate>

@property (nonatomic, assign) OnApp           *app;
@property (nonatomic, assign) gint64           note_id;
@property (nonatomic, strong) NotesNoteView   *note_view;
@property (nonatomic, strong) NotesStatusBar  *status_bar;
@property (nonatomic, strong) NotesImageViewer *image_viewer;

/* Toolbar toggle buttons, kept to mirror the inline format state. */
@property (nonatomic, weak)   NSButton *btn_bold;
@property (nonatomic, weak)   NSButton *btn_italic;
@property (nonatomic, weak)   NSButton *btn_underline;
@property (nonatomic, weak)   NSButton *btn_strike;

/* In-note search field pinned to the toolbar's right edge. */
@property (nonatomic, weak)   NSSearchField *search_field;

@property (nonatomic, assign) BOOL    dirty;          /* unsaved edits       */
@property (nonatomic, assign) BOOL    sync_actions;   /* action line touched */
@property (nonatomic, strong) NSTimer *autosave_timer;/* nil = none          */

- (void)save;
- (void)update_status;
@end

@implementation NotesEditorWindow

/* ── Construction ──────────────────────────────────────────────────────── */

/*
 * init_with_app:note:title:search_term:reveal:ordinal: — create the editor
 * for one note; the caller registers and shows it.
 *
 * Inputs:
 *   app         — application context
 *   note_id     — the note
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
        title:(NSString *)title
  search_term:(NSString *)search_term
       reveal:(NotesEditorReveal)reveal
      ordinal:(NSInteger)ordinal
{
    gint win_w = (gint)on_look_editor_w;
    gint win_h = (gint)on_look_editor_h;
    on_app_config_get_size("editor_win_w", "editor_win_h", &win_w, &win_h);
    NSRect frame = NSMakeRect(0, 0, win_w, win_h);

    NSWindow *window = [[NSWindow alloc]
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
    [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    self.app     = app;
    self.note_id = note_id;

    [self build_note_view];
    [self build_toolbar];
    [self build_status_bar];
    [self build_image_viewer];
    [self load_content];

    window.delegate = self;

    /* Deferred: reveal / search need a populated view. */
    if (search_term.length > 0) {
        NSString *term = [search_term copy];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self apply_search:term];
        });
    }
    if (reveal != NotesEditorRevealNone) {
        NSInteger         ord = ordinal;
        NotesEditorReveal rv  = reveal;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (rv == NotesEditorRevealImage)  on_note_view_image_reveal(self.note_view, ord);
            if (rv == NotesEditorRevealAction) on_note_view_action_reveal(self.note_view, ord);
        });
    }
    return self;
}

/*
 * build_note_view — create the NotesNoteView inside an NSScrollView, set as
 * the window's content view, and wire the editing handlers.
 *
 * Inputs: none  Output: none.
 */
- (void)
build_note_view
{
    NotesNoteView *view = [[NotesNoteView alloc] init_with_app:self.app];
    self.note_view = view;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:self.window.contentView.bounds];
    scroll.documentView        = view;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.drawsBackground     = NO;
    scroll.focusRingType       = NSFocusRingTypeNone;
    self.window.contentView    = scroll;

    __weak NotesEditorWindow *ws = self;
    view.edited_handler = ^{
        NotesEditorWindow *s = ws;
        if (!s) return;
        [s queue_autosave];
    };
    view.inline_flags_handler = ^{
        NotesEditorWindow *s = ws;
        if (!s) return;
        [s sync_format_buttons];
    };
    view.image_activated_handler = ^(NSInteger ordinal) {
        NotesEditorWindow *s = ws;
        if (!s) return;
        on_image_viewer_open(s.image_viewer, ordinal);
    };
}

/*
 * build_toolbar — install an NSToolbar with format buttons and find field.
 *
 * Inputs: none  Output: none.
 */
- (void)
build_toolbar
{
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:editor_toolbar_id];
    toolbar.delegate                = self;
    toolbar.displayMode             = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    [self.window setToolbar:toolbar];
}

/*
 * build_status_bar — add the bottom status bar as a titlebar accessory.
 *
 * Inputs: none  Output: none.
 */
- (void)
build_status_bar
{
    NotesStatusBar *bar = [[NotesStatusBar alloc] init];
    [self.window addTitlebarAccessoryViewController:bar];
    self.status_bar = bar;
}

/*
 * build_image_viewer — add the image viewer overlay over the scroll view.
 *
 * Inputs: none  Output: none.
 */
- (void)
build_image_viewer
{
    NSScrollView *scroll = (NSScrollView *)self.window.contentView;
    __weak NotesEditorWindow *ws = self;

    NotesImageViewer *viewer = on_image_viewer_new(scroll, nil, nil);
    viewer.count_handler = ^NSInteger {
        return on_note_view_image_count(ws.note_view);
    };
    viewer.image_handler = ^NSImage *(NSInteger idx, NSSize box) {
        NotesEditorWindow *s = ws;
        if (!s) return nil;
        GBytes *png = on_note_view_image_png(s.note_view, idx);
        if (!png) return nil;
        gsize n;
        const uint8_t *bytes = g_bytes_get_data(png, &n);
        CGFloat scale = s.window.backingScaleFactor;
        return on_ui_thumbnail(bytes, n, box.width, box.height, scale);
    };
    viewer.caption_handler = ^NSString *(NSInteger idx) {
        (void)idx;
        return @"Image";
    };
    viewer.action_handler = nil; /* no cross-note action from the editor */
    self.image_viewer = viewer;
}

/*
 * load_content — load the note's document into the note view.
 *
 * Inputs: none  Output: none.
 */
- (void)
load_content
{
    OnApp    *app = self.app;
    gint64    nid = self.note_id;
    OnDocument *doc = on_note_document_load(app->db, nid);
    if (on_note_document_reconcile(app->db, nid, doc)) {
        /* Reconciliation rewrote an action line; queue a save so the change
         * is not lost if the user closes without typing anything. */
        [self queue_autosave];
    }
    on_note_view_load_document(self.note_view, doc);
    /* doc is now owned by the view */
    [self update_status];
}

/* ── NSToolbarDelegate ──────────────────────────────────────────────────── */

/*
 * toolbarDefaultItemIdentifiers: — default item order.
 *
 * Inputs:
 *   toolbar — the toolbar (unused)
 *
 * Output: ordered identifier list.
 */
- (NSArray<NSToolbarItemIdentifier> *)
toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return @[
        item_bold, item_italic, item_underline, item_strike,
        NSToolbarSpaceItemIdentifier,
        item_code,
        NSToolbarSpaceItemIdentifier,
        item_h1, item_h2, item_body,
        NSToolbarSpaceItemIdentifier,
        item_bullet, item_number, item_check,
        NSToolbarSpaceItemIdentifier,
        item_insert,
        NSToolbarFlexibleSpaceItemIdentifier,
        item_find,
    ];
}

/*
 * toolbarAllowedItemIdentifiers: — allowed items (same as default here).
 *
 * Inputs:
 *   toolbar — the toolbar
 *
 * Output: all allowed identifiers.
 */
- (NSArray<NSToolbarItemIdentifier> *)
toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    return [self toolbarDefaultItemIdentifiers:toolbar];
}

/*
 * toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar: — create an item.
 *
 * Inputs:
 *   toolbar    — the toolbar (unused)
 *   ident      — the item's identifier
 *   flag       — whether it is being inserted (unused)
 *
 * Output: the toolbar item, or nil for unknown identifiers.
 */
- (NSToolbarItem *)
toolbar:(NSToolbar *)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)ident
        willBeInsertedIntoToolbar:(BOOL)flag
{
    (void)toolbar; (void)flag;

    if ([ident isEqual:item_find]) {
        NSSearchField *field = on_ui_toolbar_search_field(@"Find in note", 180);
        field.sendsWholeSearchString       = NO;
        field.sendsSearchStringImmediately = YES;
        field.target = self;
        field.action = @selector(search_field_changed:);
        self.search_field = field;
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
        item.label = @"Find";
        item.view  = field;
        return item;
    }

    if ([ident isEqual:item_insert]) {
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Insert"];
        [menu addItemWithTitle:@"Image…"  action:@selector(insert_image:)  keyEquivalent:@""];
        [menu addItemWithTitle:@"Table"         action:@selector(insert_table:)  keyEquivalent:@""];
        [menu addItemWithTitle:@"Date"          action:@selector(insert_date:)   keyEquivalent:@""];
        NSPopUpButton *btn = [[NSPopUpButton alloc]
                              initWithFrame:NSMakeRect(0, 0, 60, 24) pullsDown:YES];
        btn.menu = menu;
        [btn.menu insertItemWithTitle:@"+" action:nil keyEquivalent:@"" atIndex:0];
        btn.bezelStyle = NSBezelStyleToolbar;
        btn.font       = [NSFont systemFontOfSize:14];
        btn.target     = self;
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
        item.label = @"Insert";
        item.view  = btn;
        return item;
    }

    /* Inline style toggle buttons — Bold / Italic / Underline / Strikethrough */
    struct {
        NSToolbarItemIdentifier ident;
        NSString *label;
        NSString *sym;
        SEL       action;
    } inline_map[] = {
        { item_bold,      @"Bold",          @"bold",          @selector(toggle_bold:)      },
        { item_italic,    @"Italic",        @"italic",        @selector(toggle_italic:)    },
        { item_underline, @"Underline",     @"underline",     @selector(toggle_underline:) },
        { item_strike,    @"Strikethrough", @"strikethrough", @selector(toggle_strike:)    },
    };
    for (size_t i = 0; i < sizeof inline_map / sizeof *inline_map; i++) {
        if (![ident isEqual:inline_map[i].ident]) continue;
        NSButton *btn = [NSButton buttonWithTitle:inline_map[i].label
                                          target:self
                                          action:inline_map[i].action];
        NSImage *img = on_ui_icon(inline_map[i].sym, on_look_toolbar_icon);
        if (img) { btn.image = img; btn.imagePosition = NSImageOnly; }
        btn.bezelStyle = NSBezelStyleToolbar;
        [btn setButtonType:NSButtonTypeToggle];
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
        item.label = inline_map[i].label;
        item.view  = btn;
        if ([ident isEqual:item_bold])      self.btn_bold      = btn;
        if ([ident isEqual:item_italic])    self.btn_italic    = btn;
        if ([ident isEqual:item_underline]) self.btn_underline = btn;
        if ([ident isEqual:item_strike])    self.btn_strike    = btn;
        return item;
    }

    /* Paragraph style buttons — Code / H1 / H2 / Body / Bullet / Number / Check */
    struct {
        NSToolbarItemIdentifier ident;
        NSString *label;
        guint32   flag;
        SEL       action;
    } para_map[] = {
        { item_code,   @"Code",    ON_FMT_CODEBLOCK,   @selector(toggle_code:)   },
        { item_h1,     @"H1",      ON_FMT_H1,          @selector(para_h1:)       },
        { item_h2,     @"H2",      ON_FMT_H2,          @selector(para_h2:)       },
        { item_body,   @"Body",    0,                  @selector(para_body:)     },
        { item_bullet, @"Bullets", ON_FMT_LIST_BULLET, @selector(para_bullet:)   },
        { item_number, @"Numbers", ON_FMT_LIST_NUMBER, @selector(para_number:)   },
        { item_check,  @"Tasks",   ON_FMT_LIST_CHECK,  @selector(para_check:)    },
    };
    for (size_t i = 0; i < sizeof para_map / sizeof *para_map; i++) {
        if (![ident isEqual:para_map[i].ident]) continue;
        NSButton *btn = [NSButton buttonWithTitle:para_map[i].label
                                          target:self
                                          action:para_map[i].action];
        btn.bezelStyle = NSBezelStyleToolbar;
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
        item.label = para_map[i].label;
        item.view  = btn;
        return item;
    }

    return nil;
}

/* ── Format toolbar state ───────────────────────────────────────────────── */

/*
 * sync_format_buttons — mirror the note view's current inline flags into
 * the toolbar toggle buttons.
 *
 * Inputs: none  Output: none.
 */
- (void)
sync_format_buttons
{
    guint32 flags = on_note_view_inline_flags(self.note_view);
    self.btn_bold.state      = (flags & ON_FMT_BOLD)      ? NSControlStateValueOn : NSControlStateValueOff;
    self.btn_italic.state    = (flags & ON_FMT_ITALIC)    ? NSControlStateValueOn : NSControlStateValueOff;
    self.btn_underline.state = (flags & ON_FMT_UNDERLINE) ? NSControlStateValueOn : NSControlStateValueOff;
    self.btn_strike.state    = (flags & ON_FMT_STRIKE)    ? NSControlStateValueOn : NSControlStateValueOff;
}

/* ── Toolbar actions ────────────────────────────────────────────────────── */

/*
 * toggle_bold: — toggle the Bold inline style at the cursor.
 *
 * Inputs:
 *   sender — the toolbar button (unused)
 *
 * Output: none.
 */
- (void)toggle_bold:(id)sender      { (void)sender; on_note_view_toggle_inline(self.note_view, ON_FMT_BOLD);           [self.window makeFirstResponder:self.note_view]; }
- (void)toggle_italic:(id)sender    { (void)sender; on_note_view_toggle_inline(self.note_view, ON_FMT_ITALIC);         [self.window makeFirstResponder:self.note_view]; }
- (void)toggle_underline:(id)sender { (void)sender; on_note_view_toggle_inline(self.note_view, ON_FMT_UNDERLINE);      [self.window makeFirstResponder:self.note_view]; }
- (void)toggle_strike:(id)sender    { (void)sender; on_note_view_toggle_inline(self.note_view, ON_FMT_STRIKE);         [self.window makeFirstResponder:self.note_view]; }
- (void)toggle_code:(id)sender      { (void)sender; on_note_view_toggle_paragraph(self.note_view, ON_FMT_CODEBLOCK);   [self.window makeFirstResponder:self.note_view]; }
- (void)para_h1:(id)sender          { (void)sender; on_note_view_toggle_paragraph(self.note_view, ON_FMT_H1);          [self.window makeFirstResponder:self.note_view]; }
- (void)para_h2:(id)sender          { (void)sender; on_note_view_toggle_paragraph(self.note_view, ON_FMT_H2);          [self.window makeFirstResponder:self.note_view]; }
- (void)para_body:(id)sender        { (void)sender; on_note_view_toggle_paragraph(self.note_view, 0);                  [self.window makeFirstResponder:self.note_view]; }
- (void)para_bullet:(id)sender      { (void)sender; on_note_view_toggle_paragraph(self.note_view, ON_FMT_LIST_BULLET); [self.window makeFirstResponder:self.note_view]; }
- (void)para_number:(id)sender      { (void)sender; on_note_view_toggle_paragraph(self.note_view, ON_FMT_LIST_NUMBER); [self.window makeFirstResponder:self.note_view]; }
- (void)para_check:(id)sender       { (void)sender; on_note_view_toggle_paragraph(self.note_view, ON_FMT_LIST_CHECK);  [self.window makeFirstResponder:self.note_view]; }

/*
 * insert_image: — let the user pick an image file and embed it at the cursor.
 *
 * Inputs:
 *   sender — menu item (unused)
 *
 * Output: none.
 */
- (void)
insert_image:(id)sender
{
    (void)sender;
    __weak NotesEditorWindow *ws = self;
    on_ui_pick_path(NotesPickOpenFile, @"Insert Image", @"Insert",
                    @[UTTypeImage], nil, self.window, ^(NSString *path) {
        NotesEditorWindow *s = ws;
        if (!s || !path) return;
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (!data) return;
        on_note_view_insert_image(s.note_view, (const uint8_t *)data.bytes, data.length);
    });
}

/*
 * insert_table: — insert a new table at the cursor.
 *
 * Inputs:
 *   sender — menu item (unused)
 *
 * Output: none.
 */
- (void)
insert_table:(id)sender
{
    (void)sender;
    on_note_view_insert_table(self.note_view);
    [self.window makeFirstResponder:self.note_view];
}

/*
 * insert_date: — insert today's date at the cursor.
 *
 * Inputs:
 *   sender — menu item (unused)
 *
 * Output: none.
 */
- (void)
insert_date:(id)sender
{
    (void)sender;
    on_note_view_insert_date(self.note_view);
    [self.window makeFirstResponder:self.note_view];
}

/* ── Find field ─────────────────────────────────────────────────────────── */

/*
 * search_field_changed: — the find field changed; highlight every match.
 *
 * Inputs:
 *   sender — the search field
 *
 * Output: none.
 */
- (void)
search_field_changed:(NSSearchField *)sender
{
    on_note_view_find(self.note_view, sender.stringValue);
}

/*
 * apply_search: — put text in the find field and highlight the matches.
 *
 * Inputs:
 *   term — search text
 *
 * Output: none.
 */
- (void)
apply_search:(NSString *)term
{
    if (self.search_field) self.search_field.stringValue = term;
    on_note_view_find(self.note_view, term);
}

/* ── Key events ──────────────────────────────────────────────────────────── */

/*
 * keyDown: — offer the key to the image viewer first; eat all keys while
 * the viewer is open so the note behind is not edited blind.
 *
 * Inputs:
 *   event — the key event
 *
 * Output: none.
 */
- (void)
keyDown:(NSEvent *)event
{
    if (on_image_viewer_key_down(self.image_viewer, event)) return;
    if (on_image_viewer_is_open(self.image_viewer)) return;
    [super keyDown:event];
}

/* ── Saving ─────────────────────────────────────────────────────────────── */

/*
 * queue_autosave — (re)arm the autosave debounce timer; mark dirty.
 *
 * Inputs: none  Output: none.
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
        (void)t;
        NotesEditorWindow *s = ws;
        if (!s) return;
        s.autosave_timer = nil;
        [s save];
    }];
}

/*
 * save — persist the note to the database and notify the library.
 *
 * Inputs: none
 * Output: none.
 */
- (void)
save
{
    OnApp      *app = self.app;
    OnDocument *doc = on_note_view_document(self.note_view);

    if (on_note_view_take_actions_modified(self.note_view)) self.sync_actions = YES;

    gboolean actions_changed = FALSE;
    if (!on_note_document_save(app->db, self.note_id, doc, self.sync_actions,
                               &actions_changed)) {
        on_app_status(app, "Could not save note %" G_GINT64_FORMAT, self.note_id);
        return;
    }
    self.sync_actions = NO;

    if (on_note_view_take_tags_modified(self.note_view)) {
        GList *tags = on_document_collect_tags(doc);
        on_db_note_set_tags(app->db, self.note_id, tags);
        g_list_free_full(tags, g_free);
    }
    self.dirty = NO;

    gchar *title = on_document_title(doc, ON_DEFAULT_NOTE_TITLE, ON_TITLE_MAX_CHARS);
    self.window.title = @(title);
    g_free(title);

    [self update_status];

    if (actions_changed) {
        if (app->notify_notes_changed != NULL) app->notify_notes_changed(app);
    } else {
        if (app->notify_note_saved != NULL) app->notify_note_saved(app, self.note_id);
    }
}

/*
 * update_status — refresh the status bar labels.
 *
 * Inputs: none  Output: none.
 */
- (void)
update_status
{
    OnApp    *app  = self.app;
    NSString *path = @"";
    if (app->statusbar_db_path) {
        gchar *db_dir = on_app_config_get("db_dir");
        if (db_dir) { path = @(db_dir); g_free(db_dir); }
    }
    [self.status_bar update_path:path dirty:self.dirty];
}

/* ── NSWindowDelegate ───────────────────────────────────────────────────── */

/*
 * windowWillClose: — teardown: stop timer, save or delete, leave registry.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 *
 * Output: none.
 */
- (void)
windowWillClose:(NSNotification *)note
{
    (void)note;
    OnApp *app = self.app;

    [self.autosave_timer invalidate];
    self.autosave_timer                    = nil;
    self.note_view.edited_handler          = nil;
    self.note_view.inline_flags_handler    = nil;
    self.note_view.image_activated_handler = nil;

    OnDocument *doc = on_note_view_document(self.note_view);
    if (on_note_edit_is_blank(doc)) {
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
 * on_settings_changed — re-read preferences and refresh the view and toolbar.
 *
 * Inputs: none  Output: none.
 */
- (void)
on_settings_changed
{
    on_note_view_settings_changed(self.note_view);
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
        if (reveal == NotesEditorRevealImage)  on_note_view_image_reveal(editor.note_view, ordinal);
        if (reveal == NotesEditorRevealAction) on_note_view_action_reveal(editor.note_view, ordinal);
        if (search_term.length > 0) [editor apply_search:search_term];
        [editor showWindow:nil];
        [NSApp activate];
        return YES;
    }
    OnNoteMeta *meta = on_db_note_get(app->db, note_id);
    if (meta == NULL) return NO;
    NSString *title = @(meta->title);
    on_db_note_meta_free(meta);

    editor = [[NotesEditorWindow alloc]
              init_with_app:app note:note_id title:title
              search_term:search_term reveal:reveal ordinal:ordinal];
    editors[@(note_id)] = editor;
    [editor showWindow:nil];
    [NSApp activate];
    return YES;
}

/*
 * on_editor_close_notes — see editor_window.h.
 */
void
on_editor_close_notes(const gint64 *ids, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        [editor_for(ids[i]).window close];
    }
}

/*
 * on_editor_close_all — see editor_window.h.
 */
void
on_editor_close_all(void)
{
    /* Snapshot keys before the loop, since closing modifies editors. */
    NSArray<NSNumber *> *keys = editors.allKeys;
    for (NSNumber *k in keys) {
        [((NotesEditorWindow *)editors[k]).window close];
    }
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

static gboolean
hook_open_note(OnApp *app, gint64 note_id)
{
    return on_editor_window_open(app, note_id, NotesEditorRevealNone, 0, nil);
}

static OnDocument *
hook_live_document(OnApp *app, gint64 note_id)
{
    (void)app;
    NotesEditorWindow *editor = editor_for(note_id);
    return editor ? on_note_view_begin_external_edit(editor.note_view) : NULL;
}

static void
hook_live_edited(OnApp *app, gint64 note_id)
{
    (void)app;
    on_note_view_end_external_edit(editor_for(note_id).note_view);
}

static void
hook_flush_editors(OnApp *app)
{
    (void)app;
    on_editor_close_all();
}

/*
 * on_editor_install — see editor_window.h.
 */
void
on_editor_install(OnApp *app)
{
    editors            = [NSMutableDictionary dictionary];
    app->open_note     = hook_open_note;
    app->live_document = hook_live_document;
    app->live_edited   = hook_live_edited;
    app->flush_editors = hook_flush_editors;
}
