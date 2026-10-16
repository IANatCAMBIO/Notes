/*
 * library_sidebar.m — the library's sidebar.
 *
 * An NSOutlineView over a flat list of SidebarItem objects, laid out and
 * formatted as Tasks' sidebar: Pinned Notes and All Notes, Action Items,
 * the Tags header (a group row) followed by one row per tag the notes
 * carry (its emoji, its name, and its count at the right when counts are
 * shown), and the Trash.  Nothing nests or collapses; only the Tags header
 * cannot be selected.
 *
 * Selecting a row writes lib's selection and refreshes the notes pane
 * (which also updates the status bar).  A right click selects the row and
 * shows its menu, built from commands (commands.h) so the menu bar, the
 * toolbar and the menu run the same handlers.
 *
 * Drag and drop (on_pasteboard_type_rows, library_state.h): notes onto a
 * tag (a #tag on their bottom lines) or onto the Trash.
 *
 * Fit to content: the divider follows the widest row on show (the
 * sidebar_fit_content setting; once at startup whatever it says).
 */

#import "library_sidebar.h"

#import "commands.h"
#import "library_commands.h"
#import "library_notes.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "db.h"

#include <string.h>

/* ── SidebarItem ─────────────────────────────────────────────────────────── */

/*
 * SidebarItem — one row of the sidebar: its kind and id (the identity that
 * survives a rebuild), the text it shows, its bare name and its count.
 */
@interface SidebarItem : NSObject
@property (nonatomic, assign) on_sb_kind_t kind;
@property (nonatomic, assign) gint64       ident;
@property (nonatomic, copy)   NSString    *display;   /* shown in the outline */
@property (nonatomic, copy)   NSString    *raw_name;  /* bare name            */
@property (nonatomic, assign) gint         count;     /* shown at the right; -1 = none */
@end

@implementation SidebarItem
@end

/* The count label's identifier in a cell, and the gap before it. */
static NSString * const count_id  = @"sidebar.count";
static const CGFloat    count_gap = 4.0;

/*
 * count_font — the count's font: small, even-width digits (Tasks' count).
 *
 * Output: the font.
 */
static NSFont *
count_font(void)
{
    return [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
}

/*
 * item_new — a sidebar row.
 *
 * Inputs:
 *   kind       — the row's kind
 *   ident      — its tag id; 0 for the fixed rows
 *   display    — what it shows
 *   raw        — its bare name (the selection's name, status messages)
 *   count      — the count shown at the row's right, or -1 for none
 *
 * Output: the item.
 */
static SidebarItem *
item_new(on_sb_kind_t kind, gint64 ident, NSString *display, NSString *raw, gint count)
{
    SidebarItem *it = [[SidebarItem alloc] init];
    it.kind     = kind;
    it.ident    = ident;
    it.display  = display;
    it.raw_name = raw;
    it.count    = count;
    return it;
}

/*
 * row_font — a row's font, as Tasks' sidebar sets it: the Tags header in
 * the small bold system font, every other row (the views and the tags) in
 * the regular system font.
 *
 * Inputs:
 *   kind — the row's kind
 *
 * Output: the font.
 */
static NSFont *
row_font(on_sb_kind_t kind)
{
    return kind == on_sb_tags_header ? [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize]
                                     : [NSFont systemFontOfSize:NSFont.systemFontSize];
}

/*
 * section_label — a section row's text: its emoji, two spaces, its name.
 *
 * Inputs:
 *   emoji — the emoji
 *   name  — the name
 *
 * Output: the text.
 */
static NSString *
section_label(NSString *emoji, NSString *name)
{
    return [NSString stringWithFormat:@"%@  %@", emoji, name];
}

/*
 * tag_item — a tag's row: its emoji before its name (two spaces apart, as
 * Tasks shows it) and, when counts are shown, the count of notes carrying
 * it at the row's right.
 *
 * Inputs:
 *   t          — the tag
 *   tag_counts — tag id → count, or NULL while counts are hidden
 *
 * Output: the item.
 */
static SidebarItem *
tag_item(const OnTag *t, GHashTable *tag_counts)
{
    gint count = tag_counts ? GPOINTER_TO_INT(g_hash_table_lookup(tag_counts, &t->id)) : -1;
    return item_new(on_sb_tag, t->id, on_ui_tag_label(t->emoji, t->name), @(t->name), count);
}

/* ── The outline ─────────────────────────────────────────────────────────── */

@class NotesSidebarController;

/*
 * NotesSidebarOutline — the outline view; a click selects its row on the
 * mouse-down, and a right click (or Control-click) selects the row under it
 * and shows that row's menu.
 */
@interface NotesSidebarOutline : NSOutlineView
@property (nonatomic, weak) NotesSidebarController *controller;
@end

/* ── The controller ─────────────────────────────────────────────────────── */

@interface NotesSidebarController : NSObject <NSOutlineViewDelegate, NSOutlineViewDataSource>
@property (nonatomic, weak)   NotesLibrary        *lib;
@property (nonatomic, weak)   NotesSidebarOutline *outline;
@property (nonatomic, strong) NSMutableArray<SidebarItem *> *roots;
@property (nonatomic, assign) BOOL                 fit_queued;
@property (nonatomic, assign) BOOL                 fit_force;
- (NSMenu *)menu_for_row:(NSInteger)row;
- (void)queue_fit:(BOOL)force;
@end

@implementation NotesSidebarOutline

/*
 * mouseDown: — a plain click selects its row at once
 * (on_ui_select_on_mouse_down); NSOutlineView then tracks the click as
 * usual.
 *
 * Inputs:
 *   event — the mouse event
 */
- (void)
mouseDown:(NSEvent *)event
{
    on_ui_select_on_mouse_down(self, event);
    [super mouseDown:event];
}

/*
 * menuForEvent: — select the row clicked and show its menu (none for the
 * Tags header or empty space).
 *
 * Inputs:
 *   event — the right click or Control-click
 *
 * Output: the menu, or nil.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    NSInteger row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    return [self.controller menu_for_row:row];
}

/*
 * viewDidMoveToWindow — on screen for the first time: the one-shot fit,
 * whatever the setting says (GTK: on_sidebar_mapped).
 */
- (void)
viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    if (self.window) [self.controller queue_fit:YES];
}

@end

@implementation NotesSidebarController

/* ── Finding rows ──────────────────────────────────────────────────────── */

/*
 * row_of_kind:ident: — the row (kind, ident).
 *
 * Inputs:
 *   kind  — the row's kind
 *   ident — its id
 *
 * Output: its row index, or -1 when there is no such row.
 */
- (NSInteger)
row_of_kind:(on_sb_kind_t)kind ident:(gint64)ident
{
    for (NSUInteger i = 0; i < self.roots.count; i++)
        if (self.roots[i].kind == kind && self.roots[i].ident == ident) return (NSInteger)i;
    return -1;
}

/*
 * select_row: — select a row from code; the library's selection follows.
 * The notes pane is not refreshed: the caller does that when it needs to.
 *
 * Inputs:
 *   row — the row index
 */
- (void)
select_row:(NSInteger)row
{
    SidebarItem *it = [self.outline itemAtRow:row];
    if (it == nil) return;
    self.lib.populating++;
    [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    self.lib.populating--;
    self.lib.sel_kind = it.kind;
    self.lib.sel_id   = it.ident;
    self.lib.sel_name = it.raw_name;
}

/* ── Building ─────────────────────────────────────────────────────────── */

/*
 * build_roots — the rows from the database: the views, the tags the notes
 * carry, the Trash when it holds anything.
 */
- (void)
build_roots
{
    OnDatabase *db     = self.lib.app->db;
    BOOL        counts = self.lib.app->sidebar_counts;
    GHashTable *tag_counts = counts ? on_db_tag_count_map(db) : NULL;
    NSMutableArray<SidebarItem *> *roots = [NSMutableArray array];

    gint n_pinned = on_db_note_count_pinned(db);
    if (n_pinned > 0)
        [roots addObject:item_new(on_sb_pinned, 0, section_label(@"📌", @"Pinned Notes"),
                                  @"Pinned Notes", counts ? n_pinned : -1)];
    [roots addObject:item_new(on_sb_all, 0, section_label(@"🔮", @"All Notes"), @"All Notes",
                              counts ? on_db_note_count_visible(db) : -1)];
    gint n_actions = 0, n_open = 0;
    on_db_action_counts(db, &n_actions, &n_open);
    if (n_actions > 0)
        [roots addObject:item_new(on_sb_actions, 0, section_label(@"❗", @"Action Items"),
                                  @"Action Items", counts ? n_open : -1)];

    GList *tags = on_db_tag_list(db, ON_TAGS_CARRIED);
    if (tags != NULL) {
        [roots addObject:item_new(on_sb_tags_header, 0, @"Tags", @"Tags", -1)];
        for (GList *l = tags; l != NULL; l = l->next)
            [roots addObject:tag_item(l->data, tag_counts)];
    }
    on_db_tag_list_free(tags);

    gint n_trash = on_db_trash_count(db);
    if (n_trash > 0)
        [roots addObject:item_new(on_sb_trash, 0, section_label(@"🗑", @"Trash"), @"Trash",
                                  counts ? n_trash : -1)];

    if (tag_counts) g_hash_table_destroy(tag_counts);
    self.roots = roots;
}

/*
 * refresh — rebuild the rows, keeping the scroll position and the
 * selection.  A selection
 * that no longer exists (a tag no note carries any more) falls back to
 * All Notes, and then the notes pane is refreshed for it.
 *
 * Output: YES when the notes pane was refreshed (the selection moved).
 */
- (BOOL)
refresh
{
    NSClipView *clip   = self.outline.enclosingScrollView.contentView;
    NSPoint     scroll = clip.bounds.origin;

    self.lib.populating++;
    [self build_roots];
    [self.outline reloadData];
    NSInteger row      = [self row_of_kind:self.lib.sel_kind ident:self.lib.sel_id];
    BOOL      restored = row >= 0;
    [self select_row:restored ? row : [self row_of_kind:on_sb_all ident:0]];
    self.lib.populating--;

    if (clip) [clip scrollToPoint:scroll];
    if (!restored) on_library_notes_refresh(self.lib);
    [self queue_fit:NO];
    return !restored;
}

/*
 * select_kind:ident: — select a row from code and list it in the notes
 * pane.
 *
 * Inputs:
 *   kind  — the row's kind
 *   ident — its id
 */
- (void)
select_kind:(on_sb_kind_t)kind ident:(gint64)ident
{
    NSInteger row = [self row_of_kind:kind ident:ident];
    if (row < 0) return;
    [self select_row:row];
    [self.outline scrollRowToVisible:row];
    on_library_notes_refresh(self.lib);
}

/* ── Fit to content ───────────────────────────────────────────────────── */

/*
 * queue_fit: — fit the divider to the rows once they are laid out (after
 * a rebuild); with `force`, whatever the setting.
 *
 * Inputs:
 *   force — YES for the one-shot startup fit
 */
- (void)
queue_fit:(BOOL)force
{
    if (!force && !self.lib.app->sidebar_fit_content) return;
    self.fit_force |= force;
    if (self.fit_queued) return;
    self.fit_queued = YES;
    __weak NotesSidebarController *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        NotesSidebarController *s = ws;
        if (!s) return;
        s.fit_queued = NO;
        [s fit:s.fit_force];
        s.fit_force = NO;
    });
}

/*
 * fit: — size the sidebar to its widest visible row plus a shown
 * scrollbar, at least on_look_sidebar_min_w and at most
 * on_look_sidebar_max_fraction of the split (GTK: sidebar_fit_apply).
 *
 * Inputs:
 *   force — YES to fit whatever the setting says
 */
- (void)
fit:(BOOL)force
{
    if (!force && !self.lib.app->sidebar_fit_content) return;
    NSSplitView *split = nil;
    for (NSView *v = self.outline.superview; v && !split; v = v.superview)
        if ([v isKindOfClass:NSSplitView.class]) split = (NSSplitView *)v;
    if (split == nil || split.bounds.size.width <= 0) return;

    CGFloat want = 0;
    for (NSInteger row = 0; row < self.outline.numberOfRows; row++) {
        SidebarItem *it   = [self.outline itemAtRow:row];
        NSRect       cell = [self.outline frameOfCellAtColumn:0 row:row];
        CGFloat text = [it.display sizeWithAttributes:@{ NSFontAttributeName: row_font(it.kind) }].width;
        if (it.count >= 0)
            text += count_gap + [[NSString stringWithFormat:@"%d", it.count]
                                    sizeWithAttributes:@{ NSFontAttributeName: count_font() }].width;
        want = MAX(want, NSMinX(cell) + on_look_sidebar_text_pad + on_look_sidebar_count_pad + ceil(text));
    }
    NSScrollView *scroll = self.outline.enclosingScrollView;
    if (scroll.scrollerStyle == NSScrollerStyleLegacy && !scroll.verticalScroller.hidden)
        want += [NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy];
    want = MAX(want, on_look_sidebar_min_w);
    want = MIN(want, split.bounds.size.width * on_look_sidebar_max_fraction);
    [split setPosition:round(want) ofDividerAtIndex:0];
}

/* ── Menus ─────────────────────────────────────────────────────────────── */

/*
 * command_item — a menu item for a command, its title replaced.
 *
 * Inputs:
 *   ident — the command
 *   title — the title for this menu, or nil for the command's own
 *
 * Output: the item.
 */
static NSMenuItem *
command_item(NSString *ident, NSString *title)
{
    NSMenuItem *item = on_command_menu_item(ident);
    if (title) item.title = title;
    return item;
}

/*
 * menu_for_row: — select a row and build its menu: the Trash empties; a
 * tag is edited (its emoji and Notes' favorite flag); every row but the
 * Trash searches within itself.  The items are commands, run by the
 * library through the responder chain.
 *
 * Inputs:
 *   row — the row clicked, or -1
 *
 * Output: the menu, or nil (empty space, the Tags header).
 */
- (NSMenu *)
menu_for_row:(NSInteger)row
{
    SidebarItem *it = row >= 0 ? [self.outline itemAtRow:row] : nil;
    if (it == nil || it.kind == on_sb_tags_header) return nil;
    if (self.outline.selectedRow != row) {
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    }
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    if (it.kind == on_sb_trash) {
        [menu addItem:command_item(@"empty-trash", nil)];
        return menu;
    }
    if (it.kind == on_sb_tag) {
        [menu addItem:command_item(@"edit-tag", nil)];
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItem:command_item(@"search-here", nil)];
    return menu;
}

/* ── Data source ───────────────────────────────────────────────────────── */

/*
 * outlineView:numberOfChildrenOfItem: — how many rows under an item.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the item, or nil for the top level
 *
 * Output: the count.
 */
- (NSInteger)
outlineView:(NSOutlineView *)outline numberOfChildrenOfItem:(id)item
{
    return item ? 0 : (NSInteger)self.roots.count;
}

/*
 * outlineView:isItemExpandable: — no row has children: the sidebar is a
 * flat list, as Tasks' is.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the item (unused)
 *
 * Output: NO.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline isItemExpandable:(id)item
{
    return NO;
}

/*
 * outlineView:isGroupItem: — the Tags header is a group row, drawn as a
 * section heading, as in Tasks' sidebar.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the item
 *
 * Output: YES for the header.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline isGroupItem:(id)item
{
    return ((SidebarItem *)item).kind == on_sb_tags_header;
}

/*
 * outlineView:child:ofItem: — a child row.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   index   — which
 *   item    — the parent, or nil for the top level
 *
 * Output: the item.
 */
- (id)
outlineView:(NSOutlineView *)outline child:(NSInteger)index ofItem:(id)item
{
    return self.roots[(NSUInteger)index];
}

/* ── Delegate ──────────────────────────────────────────────────────────── */

/*
 * outlineView:viewForTableColumn:item: — a row's cell, formatted as Tasks'
 * sidebar formats its rows (row_font; the Tags header grey);
 * while counts are shown, the count at the row's right in a small grey
 * label with even-width digits, as Tasks shows its tags' counts.
 *
 * Inputs:
 *   outline — the outline
 *   column  — the column (unused)
 *   item    — the item
 *
 * Output: the cell.
 */
- (NSView *)
outlineView:(NSOutlineView *)outline viewForTableColumn:(NSTableColumn *)column item:(id)item
{
    SidebarItem *it = item;
    NSTableCellView *cell = [outline makeViewWithIdentifier:@"sidebar" owner:self];
    if (cell == nil) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"sidebar";
        NSTextField *label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        label.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:label];
        cell.textField = label;
        NSTextField *count = [NSTextField labelWithString:@""];
        count.identifier = count_id;
        count.font       = count_font();
        count.textColor  = NSColor.secondaryLabelColor;
        count.alignment  = NSTextAlignmentRight;
        count.translatesAutoresizingMaskIntoConstraints = NO;
        [count setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
        [cell addSubview:count];
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:on_look_sidebar_text_pad],
            [label.trailingAnchor constraintLessThanOrEqualToAnchor:count.leadingAnchor constant:-count_gap],
            [label.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
            [count.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-on_look_sidebar_count_pad],
            [count.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    for (NSView *v in cell.subviews) {
        if ([v.identifier isEqualToString:count_id])
            ((NSTextField *)v).stringValue = it.count >= 0 ? [NSString stringWithFormat:@"%d", it.count] : @"";
    }
    cell.textField.stringValue = it.display;
    cell.textField.font      = row_font(it.kind);
    cell.textField.textColor = it.kind == on_sb_tags_header ? NSColor.secondaryLabelColor
                                                            : NSColor.labelColor;
    return cell;
}

/*
 * outlineView:shouldSelectItem: — every row but the Tags header.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the item
 *
 * Output: YES to allow it.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline shouldSelectItem:(id)item
{
    return ((SidebarItem *)item).kind != on_sb_tags_header;
}

/*
 * outlineViewSelectionDidChange: — the user selected a row: it becomes the
 * library's selection and the notes pane lists it.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
outlineViewSelectionDidChange:(NSNotification *)note
{
    if (self.lib.populating > 0) return;
    SidebarItem *it = self.outline.selectedRow >= 0 ? [self.outline itemAtRow:self.outline.selectedRow] : nil;
    if (it == nil) return;
    self.lib.sel_kind = it.kind;
    self.lib.sel_id   = it.ident;
    self.lib.sel_name = it.raw_name;
    on_library_notes_refresh(self.lib);
}

/* ── Drag and drop ─────────────────────────────────────────────────────── */

/*
 * drop_target:info: — the row a drag of notes would land on: a tag or the
 * Trash under the pointer.
 *
 * Inputs:
 *   info — the drag
 *
 * Output: the row, or nil when the drag holds no notes or no such row is
 *         under the pointer.
 */
- (SidebarItem *)
drop_target:(id<NSDraggingInfo>)info
{
    if (on_library_drag_read(info.draggingPasteboard).count == 0) return nil;
    NSPoint      at    = [self.outline convertPoint:info.draggingLocation fromView:nil];
    SidebarItem *under = [self.outline itemAtRow:[self.outline rowAtPoint:at]];
    return under.kind == on_sb_tag || under.kind == on_sb_trash ? under : nil;
}

/*
 * outlineView:validateDrop:proposedItem:proposedChildIndex: — accept notes
 * onto a tag or the Trash, retargeted onto the row under the pointer.
 *
 * Inputs:
 *   outline — the outline
 *   info    — the drag
 *   item    — the proposed parent (unused: the row under the pointer counts)
 *   index   — the proposed child index (unused)
 *
 * Output: copy for a tag (the notes keep their other tags), move for the
 *         Trash, or none.
 */
- (NSDragOperation)
outlineView:(NSOutlineView *)outline validateDrop:(id<NSDraggingInfo>)info
proposedItem:(id)item proposedChildIndex:(NSInteger)index
{
    SidebarItem *target = [self drop_target:info];
    if (target == nil) return NSDragOperationNone;
    [outline setDropItem:target dropChildIndex:NSOutlineViewDropOnItemIndex];
    return target.kind == on_sb_tag ? NSDragOperationCopy : NSDragOperationMove;
}

/*
 * outlineView:acceptDrop:item:childIndex: — do the drop: the notes get the
 * tag on their bottom lines (on_library_tag_notes), or go to the Trash.  The rebuild waits until
 * the drag has finished.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   info    — the drag
 *   item    — the drop's row (unused: drop_target says)
 *   index   — its child index (unused)
 *
 * Output: YES when something changed.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline acceptDrop:(id<NSDraggingInfo>)info item:(id)item childIndex:(NSInteger)index
{
    SidebarItem *target = [self drop_target:info];
    if (target == nil) return NO;
    NSArray<NSNumber *> *ids = on_library_drag_read(info.draggingPasteboard);
    NotesLibrary *lib = self.lib;
    BOOL          ok;

    if (target.kind == on_sb_trash) {
        gint64 *note_ids = on_ui_ids(ids);
        ok = on_library_trash_notes(lib, note_ids, ids.count);
        free(note_ids);
    } else {
        ok = on_library_tag_notes(lib, ids, target.raw_name);
    }
    if (ok) dispatch_async(dispatch_get_main_queue(), ^{ on_library_refresh_all(lib); });
    return ok;
}

@end

/* ── The one sidebar ────────────────────────────────────────────────────── */

/* The sidebar of the (one) library window; its view owns it. */
static __weak NotesSidebarController *sidebar;

/*
 * NotesSidebarScroll — the sidebar's scroll view, which owns the
 * controller for as long as the sidebar is on screen.
 */
@interface NotesSidebarScroll : NSScrollView
@property (nonatomic, strong) NotesSidebarController *controller;
@end

@implementation NotesSidebarScroll
@end

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_library_sidebar_new — see library_sidebar.h.
 */
NSView *
on_library_sidebar_new(NotesLibrary *lib)
{
    NotesSidebarController *ctrl = [[NotesSidebarController alloc] init];
    ctrl.lib   = lib;
    ctrl.roots = [NSMutableArray array];
    sidebar    = ctrl;

    NotesSidebarOutline *outline = [[NotesSidebarOutline alloc] initWithFrame:NSZeroRect];
    outline.controller            = ctrl;
    outline.headerView            = nil;
    outline.allowsMultipleSelection = NO;
    outline.style                 = NSTableViewStyleSourceList;
    outline.rowHeight             = on_look_sidebar_row_h;
    outline.autosaveExpandedItems = NO;
    outline.focusRingType         = NSFocusRingTypeNone;
    outline.delegate              = ctrl;
    outline.dataSource            = ctrl;
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"main"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [outline addTableColumn:column];
    outline.outlineTableColumn = column;
    [outline registerForDraggedTypes:@[on_pasteboard_type_rows]];
    ctrl.outline = outline;

    NotesSidebarScroll *scroll = [[NotesSidebarScroll alloc] initWithFrame:NSZeroRect];
    scroll.controller            = ctrl;
    scroll.documentView          = outline;
    scroll.hasVerticalScroller   = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers    = YES;
    scroll.drawsBackground       = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    outline.backgroundColor      = NSColor.clearColor;

    /* The sidebar material BEHIND the list, a sibling under it, as Tasks'
     * sidebar has it: the rows then draw in the window's own appearance,
     * labelColor as Tasks' rows.  (With the material as an ancestor — or
     * the source list's own background, which the clear colour above turns
     * off — the text is drawn vibrant, brighter than Tasks'.)            */
    NSView             *host = [[NSView alloc] initWithFrame:NSZeroRect];
    NSVisualEffectView *back = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    back.material     = NSVisualEffectMaterialSidebar;
    back.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    host.translatesAutoresizingMaskIntoConstraints = NO;
    back.translatesAutoresizingMaskIntoConstraints = NO;
    [host addSubview:back];
    [host addSubview:scroll];
    for (NSView *v in @[back, scroll]) {
        [NSLayoutConstraint activateConstraints:@[
            [v.topAnchor constraintEqualToAnchor:host.topAnchor],
            [v.leadingAnchor constraintEqualToAnchor:host.leadingAnchor],
            [v.trailingAnchor constraintEqualToAnchor:host.trailingAnchor],
            [v.bottomAnchor constraintEqualToAnchor:host.bottomAnchor],
        ]];
    }

    [ctrl refresh];
    return host;
}

/*
 * on_library_sidebar_refresh — see library_sidebar.h.
 */
BOOL
on_library_sidebar_refresh(NotesLibrary *lib)
{
    return [sidebar refresh];
}

/*
 * on_library_sidebar_select — see library_sidebar.h.
 */
void
on_library_sidebar_select(NotesLibrary *lib, on_sb_kind_t kind, gint64 ident)
{
    if (sidebar == nil) {
        lib.sel_kind = kind;
        lib.sel_id   = ident;
        return;
    }
    [sidebar select_kind:kind ident:ident];
}

/*
 * on_library_sidebar_fit — see library_sidebar.h.
 */
void
on_library_sidebar_fit(NotesLibrary *lib)
{
    [sidebar queue_fit:NO];
}
