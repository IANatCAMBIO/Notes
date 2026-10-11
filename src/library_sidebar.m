/*
 * library_sidebar.m — the library's sidebar.
 *
 * An NSOutlineView over an in-memory tree of SidebarItem objects, built the
 * way the GTK app builds its sidebar (library_window.c, refresh_sidebar):
 * Pinned Notes and All Notes, Action Items, the Notes root with the folder
 * tree, the Tags header with one row per tag, and the Trash with its
 * trashed folders.  The root is a row like any other — selecting it lists
 * the top-level notes; only the Tags header cannot be selected.
 *
 * Selecting a row writes lib's selection and refreshes the notes pane
 * (which also updates the status bar).  A right click selects the row and
 * shows its menu, built from commands (commands.h) so the menu bar, the
 * toolbar and the menu run the same handlers.
 *
 * Drag and drop (on_pasteboard_type_rows, library_state.h): notes onto a
 * folder, the root or the Trash; a folder into another, the root or the
 * Trash, or between two folders to reorder; a trashed folder out of the
 * Trash to restore it there.
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
 * SidebarItem — one row of the outline: its kind and id (the identity that
 * survives a rebuild), the text it shows, its bare name, and its children.
 */
@interface SidebarItem : NSObject
@property (nonatomic, assign) on_sb_kind_t kind;
@property (nonatomic, assign) gint64       ident;
@property (nonatomic, copy)   NSString    *display;   /* shown in the outline */
@property (nonatomic, copy)   NSString    *raw_name;  /* bare name            */
@property (nonatomic, strong) NSMutableArray<SidebarItem *> *children;   /* nil = a leaf */
@end

@implementation SidebarItem
@end

/*
 * item_new — a sidebar row.
 *
 * Inputs:
 *   kind       — the row's kind
 *   ident      — its folder or tag id; 0 for the fixed rows
 *   display    — what it shows
 *   raw        — its bare name (the selection's name, status messages)
 *   expandable — YES for a row with children
 *
 * Output: the item.
 */
static SidebarItem *
item_new(on_sb_kind_t kind, gint64 ident, NSString *display, NSString *raw, BOOL expandable)
{
    SidebarItem *it = [[SidebarItem alloc] init];
    it.kind     = kind;
    it.ident    = ident;
    it.display  = display;
    it.raw_name = raw;
    it.children = expandable ? [NSMutableArray array] : nil;
    return it;
}

/*
 * is_section — is this one of the bold section rows (GTK:
 * sb_kind_is_section)?
 *
 * Inputs:
 *   kind — the row's kind
 *
 * Output: YES for the root, the Tags header, Pinned, All, Action Items and
 *         the Trash.
 */
static BOOL
is_section(on_sb_kind_t kind)
{
    return kind == on_sb_root || kind == on_sb_tags_header || kind == on_sb_pinned ||
           kind == on_sb_all || kind == on_sb_trash || kind == on_sb_actions;
}

/*
 * section_label — a section row's text: its emoji, a no-break space and a
 * space, its name, and the count when counts are shown.
 *
 * Inputs:
 *   emoji — the emoji
 *   name  — the name
 *   count — the count, or -1 for none
 *
 * Output: the text.
 */
static NSString *
section_label(NSString *emoji, NSString *name, gint count)
{
    return count >= 0 ? [NSString stringWithFormat:@"%@  %@ (%d)", emoji, name, count]
                      : [NSString stringWithFormat:@"%@  %@", emoji, name];
}

/*
 * folder_item — a folder row, normal or in the Trash: an optional emoji
 * before the name (two spaces apart) and the note count when counts are
 * shown (GTK: sb_folder_row).
 *
 * Inputs:
 *   f           — the folder
 *   kind        — on_sb_folder or on_sb_trash_folder
 *   note_counts — folder id → count, or NULL while counts are hidden
 *   expandable  — YES when it has subfolders to show
 *
 * Output: the item.
 */
static SidebarItem *
folder_item(const OnFolder *f, on_sb_kind_t kind, GHashTable *note_counts, BOOL expandable)
{
    /* %@ and @(...), never %s: an NSString format reads %s in the system
     * encoding, not UTF-8, and an emoji came out as "üß™". */
    NSString *name = (f->emoji && *f->emoji) ? [NSString stringWithFormat:@"%@  %@", @(f->emoji), @(f->name)]
                                             : @(f->name);
    if (note_counts) {
        gint count = GPOINTER_TO_INT(g_hash_table_lookup(note_counts, &f->id));
        name = [name stringByAppendingFormat:@" (%d)", count];
    }
    return item_new(kind, f->id, name, @(f->name), expandable);
}

/*
 * add_folder_items — the subfolders of a folder, recursively; a folder
 * with none is a leaf (no disclosure triangle).
 *
 * Inputs:
 *   parent_id   — whose children (0 = the top level)
 *   into        — where to append them
 *   note_counts — folder id → count, or NULL
 *   child_map   — the whole tree, from on_db_folder_child_map
 */
static void
add_folder_items(gint64 parent_id, NSMutableArray<SidebarItem *> *into,
                 GHashTable *note_counts, GHashTable *child_map)
{
    for (GList *l = on_db_folder_children(child_map, parent_id); l != NULL; l = l->next) {
        const OnFolder *f = l->data;
        BOOL has_kids = on_db_folder_children(child_map, f->id) != NULL;
        SidebarItem *it = folder_item(f, on_sb_folder, note_counts, has_kids);
        if (has_kids) add_folder_items(f->id, it.children, note_counts, child_map);
        [into addObject:it];
    }
}

/* ── The outline ─────────────────────────────────────────────────────────── */

@class NotesSidebarController;

/*
 * NotesSidebarOutline — the outline view; a right click (or Control-click)
 * selects the row under it and shows that row's menu.
 */
@interface NotesSidebarOutline : NSOutlineView
@property (nonatomic, weak) NotesSidebarController *controller;
@end

/* Where a drop goes, worked out once for validation and for the drop. */
typedef enum { drop_none, drop_into, drop_before, drop_after } drop_place_t;

typedef struct {
    drop_place_t           place;
    on_drag_kind_t         kind;
    __unsafe_unretained SidebarItem *target;   /* INTO it, or BEFORE/AFTER it */
    __unsafe_unretained SidebarItem *parent;   /* BEFORE/AFTER: the new parent */
} drop_plan_t;

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
 * chain_in:kind:ident:into: — the items from this level down to the row
 * (kind, ident), the row last.
 *
 * Inputs:
 *   items — the items at this level
 *   kind  — the row's kind
 *   ident — its id
 *   chain — receives the chain
 *
 * Output: YES when found.
 */
- (BOOL)
chain_in:(NSArray<SidebarItem *> *)items kind:(on_sb_kind_t)kind ident:(gint64)ident
    into:(NSMutableArray<SidebarItem *> *)chain
{
    for (SidebarItem *it in items) {
        [chain addObject:it];
        if (it.kind == kind && it.ident == ident) return YES;
        if (it.children && [self chain_in:it.children kind:kind ident:ident into:chain]) return YES;
        [chain removeLastObject];
    }
    return NO;
}

/*
 * reveal_kind:ident: — make the row (kind, ident) visible by expanding its
 * ancestors, outermost first, never the row itself (GTK: sb_reveal).
 *
 * Inputs:
 *   kind  — the row's kind
 *   ident — its id
 *
 * Output: its row index, or -1 when there is no such row.
 */
- (NSInteger)
reveal_kind:(on_sb_kind_t)kind ident:(gint64)ident
{
    NSMutableArray<SidebarItem *> *chain = [NSMutableArray array];
    if (![self chain_in:self.roots kind:kind ident:ident into:chain]) return -1;
    for (NSUInteger i = 0; i + 1 < chain.count; i++) [self.outline expandItem:chain[i]];
    return [self.outline rowForItem:chain.lastObject];
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
 * build_roots — the rows from the database, as refresh_sidebar builds them.
 */
- (void)
build_roots
{
    OnDatabase *db     = self.lib.app->db;
    BOOL        counts = self.lib.app->sidebar_counts;
    GHashTable *note_counts = counts ? on_db_note_count_map(db) : NULL;
    GHashTable *tag_counts  = counts ? on_db_tag_count_map(db)  : NULL;
    GHashTable *child_map   = on_db_folder_child_map(db);
    NSMutableArray<SidebarItem *> *roots = [NSMutableArray array];

    gint n_pinned = on_db_note_count_pinned(db);
    if (n_pinned > 0)
        [roots addObject:item_new(on_sb_pinned, 0, section_label(@"📌", @"Pinned Notes", counts ? n_pinned : -1),
                                  @"Pinned Notes", NO)];
    [roots addObject:item_new(on_sb_all, 0,
                              section_label(@"🔮", @"All Notes", counts ? on_db_note_count_visible(db) : -1),
                              @"All Notes", NO)];
    gint n_actions = 0, n_open = 0;
    on_db_action_counts(db, &n_actions, &n_open);
    if (n_actions > 0)
        [roots addObject:item_new(on_sb_actions, 0, section_label(@"❗", @"Action Items", counts ? n_open : -1),
                                  @"Action Items", NO)];

    gint root_count = note_counts ? GPOINTER_TO_INT(g_hash_table_lookup(note_counts, &(gint64){ 0 })) : -1;
    SidebarItem *root = item_new(on_sb_root, 0, section_label(@"📓", @"Notes", root_count), @"Notes", YES);
    add_folder_items(0, root.children, note_counts, child_map);
    [roots addObject:root];

    GList *tags = on_db_tag_list(db);
    if (tags != NULL) {
        SidebarItem *header = item_new(on_sb_tags_header, 0, section_label(@"🏷️", @"Tags", -1), @"Tags", YES);
        for (GList *l = tags; l != NULL; l = l->next) {
            const OnTag *t   = l->data;
            NSString    *raw = [@"#" stringByAppendingString:@(t->name)];
            NSString    *label = tag_counts
                ? [raw stringByAppendingFormat:@" (%d)", GPOINTER_TO_INT(g_hash_table_lookup(tag_counts, &t->id))]
                : raw;
            [header.children addObject:item_new(on_sb_tag, t->id, label, raw, NO)];
        }
        [roots addObject:header];
    }
    on_db_tag_list_free(tags);

    gint n_trash = on_db_trash_count(db);
    if (n_trash > 0) {
        GList *trashed = on_db_folder_list_trashed(db);
        SidebarItem *trash = item_new(on_sb_trash, 0, section_label(@"🗑", @"Trash", counts ? n_trash : -1),
                                      @"Trash", trashed != NULL);
        for (GList *l = trashed; l != NULL; l = l->next)
            [trash.children addObject:folder_item(l->data, on_sb_trash_folder, note_counts, NO)];
        on_db_folder_list_free(trashed);
        [roots addObject:trash];
    }

    on_db_folder_child_map_free(child_map);
    if (note_counts) g_hash_table_destroy(note_counts);
    if (tag_counts)  g_hash_table_destroy(tag_counts);
    self.roots = roots;
}

/*
 * row_key — a row's identity as a string: kind and id, which survive a
 * rebuild (positions do not).
 *
 * Inputs:
 *   it — the row
 *
 * Output: the key.
 */
static NSString *
row_key(SidebarItem *it)
{
    return [NSString stringWithFormat:@"%d:%" G_GINT64_FORMAT, it.kind, it.ident];
}

/*
 * expanded_keys:into: — the (kind, id) of every expanded row, which
 * survive a rebuild.
 *
 * Inputs:
 *   items — the items at one level
 *   into  — receives "kind:id" keys
 */
- (void)
expanded_keys:(NSArray<SidebarItem *> *)items into:(NSMutableSet<NSString *> *)into
{
    for (SidebarItem *it in items) {
        if (!it.children) continue;
        if ([self.outline isItemExpanded:it]) [into addObject:row_key(it)];
        [self expanded_keys:it.children into:into];
    }
}

/*
 * expand_keys:in: — expand the rows whose keys are in `keys`, outermost
 * first.
 *
 * Inputs:
 *   keys  — "kind:id" keys
 *   items — the items at one level
 */
- (void)
expand_keys:(NSSet<NSString *> *)keys in:(NSArray<SidebarItem *> *)items
{
    for (SidebarItem *it in items) {
        if (!it.children) continue;
        if ([keys containsObject:row_key(it)]) {
            [self.outline expandItem:it];
            [self expand_keys:keys in:it.children];
        }
    }
}

/*
 * refresh — rebuild the rows (GTK: refresh_sidebar), keeping which rows
 * are expanded, the scroll position and the selection.  A selection that
 * no longer exists falls back to the first row, and then the notes pane
 * is refreshed for it.
 */
- (void)
refresh
{
    NSMutableSet<NSString *> *expanded = [NSMutableSet set];
    [self expanded_keys:self.roots into:expanded];
    NSClipView *clip   = self.outline.enclosingScrollView.contentView;
    NSPoint     scroll = clip.bounds.origin;

    self.lib.populating++;
    [self build_roots];
    [self.outline reloadData];
    [self expand_keys:expanded in:self.roots];
    NSInteger row      = [self reveal_kind:self.lib.sel_kind ident:self.lib.sel_id];
    BOOL      restored = row >= 0;
    [self select_row:restored ? row : 0];
    self.lib.populating--;

    if (clip) [clip scrollToPoint:scroll];
    if (!restored) on_library_notes_refresh(self.lib);
    [self queue_fit:NO];
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
    NSInteger row = [self reveal_kind:kind ident:ident];
    if (row < 0) return;
    [self select_row:row];
    [self.outline scrollRowToVisible:row];
    on_library_notes_refresh(self.lib);
}

/* ── Fit to content ───────────────────────────────────────────────────── */

/*
 * queue_fit: — fit the divider to the rows once they are laid out (after
 * an expand, a collapse, a rebuild); with `force`, whatever the setting.
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
        NSFont      *font = [NSFont systemFontOfSize:on_look_sidebar_font
                                              weight:is_section(it.kind) ? NSFontWeightBold : NSFontWeightRegular];
        CGFloat text = [it.display sizeWithAttributes:@{ NSFontAttributeName: font }].width;
        want = MAX(want, NSMinX(cell) + 2 * on_look_sidebar_cell_pad + ceil(text));
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
 * menu_for_row: — select a row and build its menu (GTK:
 * on_sidebar_pressed): the Trash empties; a trashed folder restores or is
 * deleted; a folder makes a subfolder, shows its info, goes to the Trash
 * and sorts its subfolders; the root makes a folder and sorts; every row
 * but the Trash searches within itself.  The items are commands, run by
 * the library through the responder chain.
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
    switch (it.kind) {
    case on_sb_trash:
        [menu addItem:command_item(@"empty-trash", nil)];
        return menu;
    case on_sb_trash_folder:
        [menu addItem:command_item(@"folder-restore", nil)];
        [menu addItem:command_item(@"delete-folder", nil)];
        [menu addItem:NSMenuItem.separatorItem];
        break;
    case on_sb_folder:
        [menu addItem:command_item(@"new-folder", @"New Subfolder…")];
        [menu addItem:command_item(@"folder-info", @"Info…")];
        [menu addItem:command_item(@"delete-folder", nil)];
        [menu addItem:command_item(@"sort-subfolders", nil)];
        [menu addItem:NSMenuItem.separatorItem];
        break;
    case on_sb_root:
        [menu addItem:command_item(@"new-folder", nil)];
        [menu addItem:command_item(@"sort-subfolders", nil)];
        [menu addItem:NSMenuItem.separatorItem];
        break;
    default:
        break;
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
    return (NSInteger)(item ? ((SidebarItem *)item).children.count : self.roots.count);
}

/*
 * outlineView:isItemExpandable: — does a row have children?
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the item
 *
 * Output: YES for a row with a children list.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline isItemExpandable:(id)item
{
    return ((SidebarItem *)item).children != nil;
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
    return (item ? ((SidebarItem *)item).children : self.roots)[(NSUInteger)index];
}

/* ── Delegate ──────────────────────────────────────────────────────────── */

/*
 * outlineView:viewForTableColumn:item: — a row's cell: section rows bold.
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
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:on_look_sidebar_cell_pad],
            [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-on_look_sidebar_cell_pad],
            [label.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    cell.textField.stringValue = it.display;
    cell.textField.font = [NSFont systemFontOfSize:on_look_sidebar_font
                                            weight:is_section(it.kind) ? NSFontWeightBold : NSFontWeightRegular];
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

/*
 * outlineViewItemDidExpand: / outlineViewItemDidCollapse: — rows came or
 * went: the fit follows.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
outlineViewItemDidExpand:(NSNotification *)note
{
    [self queue_fit:NO];
}

- (void)
outlineViewItemDidCollapse:(NSNotification *)note
{
    [self queue_fit:NO];
}

/* ── Drag and drop ─────────────────────────────────────────────────────── */

/*
 * outlineView:pasteboardWriterForItem: — folders and trashed folders can
 * be dragged.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   item    — the item
 *
 * Output: the drag's pasteboard item, or nil for a row that does not move.
 */
- (id<NSPasteboardWriting>)
outlineView:(NSOutlineView *)outline pasteboardWriterForItem:(id)item
{
    SidebarItem *it = item;
    if (it.kind == on_sb_folder)       return on_library_drag_item(on_drag_folder, @[@(it.ident)]);
    if (it.kind == on_sb_trash_folder) return on_library_drag_item(on_drag_trashed_folder, @[@(it.ident)]);
    return nil;
}

/*
 * inside_dragged — is `item`, or one of its ancestors, the dragged folder?
 *
 * Inputs:
 *   outline — the outline
 *   item    — the row
 *   kind    — the dragged folder's row kind
 *   ident   — its id
 *
 * Output: YES when dropping there would put the folder inside itself.
 */
static BOOL
inside_dragged(NSOutlineView *outline, SidebarItem *item, on_sb_kind_t kind, gint64 ident)
{
    for (SidebarItem *it = item; it != nil; it = [outline parentForItem:it])
        if (it.kind == kind && it.ident == ident) return YES;
    return NO;
}

/*
 * plan_drop:item:index: — where a drop would go, and whether it may (GTK:
 * sidebar_drop_target).  Notes go INTO the folder, root or Trash row under
 * the pointer.  A folder goes INTO a folder, the root or (from the tree)
 * the Trash, or BEFORE/AFTER a folder between two rows — never into itself
 * or its own subtree.
 *
 * Inputs:
 *   info  — the drag
 *   item  — the outline's proposed parent
 *   index — the proposed child index, or NSOutlineViewDropOnItemIndex
 *
 * Output: the plan; place drop_none when refused.
 */
- (drop_plan_t)
plan_drop:(id<NSDraggingInfo>)info item:(SidebarItem *)item index:(NSInteger)index
{
    drop_plan_t plan = { drop_none, on_drag_notes, nil, nil };
    on_drag_kind_t kind;
    NSArray<NSNumber *> *ids = on_library_drag_read(info.draggingPasteboard, &kind);
    if (ids.count == 0) return plan;
    plan.kind = kind;

    if (kind == on_drag_notes) {
        NSPoint at = [self.outline convertPoint:info.draggingLocation fromView:nil];
        SidebarItem *under = [self.outline itemAtRow:[self.outline rowAtPoint:at]];
        if (under.kind == on_sb_folder || under.kind == on_sb_root || under.kind == on_sb_trash) {
            plan.place  = drop_into;
            plan.target = under;
        }
        return plan;
    }

    on_sb_kind_t src_kind = kind == on_drag_folder ? on_sb_folder : on_sb_trash_folder;
    gint64       src_id   = ids.firstObject.longLongValue;
    if (index == NSOutlineViewDropOnItemIndex) {
        if (item == nil || inside_dragged(self.outline, item, src_kind, src_id)) return plan;
        if (item.kind == on_sb_folder || item.kind == on_sb_root ||
            (item.kind == on_sb_trash && src_kind == on_sb_folder)) {
            plan.place  = drop_into;
            plan.target = item;
        }
        return plan;
    }
    if (item.kind == on_sb_trash && src_kind == on_sb_folder) {   /* between trashed folders */
        plan.place  = drop_into;
        plan.target = item;
        return plan;
    }
    if ((item.kind != on_sb_folder && item.kind != on_sb_root) || item.children.count == 0 ||
        inside_dragged(self.outline, item, src_kind, src_id))
        return plan;
    BOOL after = (NSUInteger)index >= item.children.count;
    SidebarItem *anchor = item.children[after ? item.children.count - 1 : (NSUInteger)index];
    if (anchor.kind == src_kind && anchor.ident == src_id) return plan;
    plan.place  = after ? drop_after : drop_before;
    plan.target = anchor;
    plan.parent = item;
    return plan;
}

/*
 * outlineView:validateDrop:proposedItem:proposedChildIndex: — accept a
 * drop where plan_drop says it may go; notes are retargeted onto the row
 * under the pointer.
 *
 * Inputs:
 *   outline — the outline
 *   info    — the drag
 *   item    — the proposed parent
 *   index   — the proposed child index
 *
 * Output: NSDragOperationMove, or none.
 */
- (NSDragOperation)
outlineView:(NSOutlineView *)outline validateDrop:(id<NSDraggingInfo>)info
proposedItem:(id)item proposedChildIndex:(NSInteger)index
{
    drop_plan_t plan = [self plan_drop:info item:item index:index];
    if (plan.place == drop_none) return NSDragOperationNone;
    if (plan.place == drop_into)
        [outline setDropItem:plan.target dropChildIndex:NSOutlineViewDropOnItemIndex];
    return NSDragOperationMove;
}

/*
 * folder_move_beside — re-parent a folder and slot it just before or after
 * a sibling (GTK: folder_move_beside).
 *
 * Inputs:
 *   db        — the database
 *   folder_id — the folder
 *   parent    — its new parent (0 = the top level)
 *   anchor_id — the sibling it goes beside
 *   after     — YES for after it
 *
 * Output: YES on success.
 */
static BOOL
folder_move_beside(OnDatabase *db, gint64 folder_id, gint64 parent, gint64 anchor_id, BOOL after)
{
    if (!on_db_folder_move(db, folder_id, parent)) return NO;
    GList  *sibs  = on_db_folder_list(db, parent);
    gint64 *order = malloc((g_list_length(sibs) + 1) * sizeof *order);
    size_t  n     = 0;
    for (GList *l = sibs; l != NULL; l = l->next) {
        const OnFolder *f = l->data;
        if (f->id == folder_id) continue;
        if (f->id == anchor_id && !after) order[n++] = folder_id;
        order[n++] = f->id;
        if (f->id == anchor_id && after) order[n++] = folder_id;
    }
    on_db_folder_list_free(sibs);
    gboolean ok = on_db_folder_reorder(db, order, n);
    free(order);
    return ok;
}

/*
 * outlineView:acceptDrop:item:childIndex: — do the drop (GTK:
 * on_sidebar_drop): notes move into a folder (one transaction) or go to
 * the Trash; a folder moves, reorders, goes to the Trash, or comes out of
 * it.  The rebuild waits until the drag has finished.
 *
 * Inputs:
 *   outline — the outline (unused)
 *   info    — the drag
 *   item    — the drop's parent
 *   index   — its child index
 *
 * Output: YES when something moved.
 */
- (BOOL)
outlineView:(NSOutlineView *)outline acceptDrop:(id<NSDraggingInfo>)info item:(id)item childIndex:(NSInteger)index
{
    drop_plan_t plan = [self plan_drop:info item:item index:index];
    if (plan.place == drop_none) return NO;
    on_drag_kind_t kind;
    NSArray<NSNumber *> *ids = on_library_drag_read(info.draggingPasteboard, &kind);
    NotesLibrary *lib = self.lib;
    OnDatabase   *db  = lib.app->db;
    BOOL ok;

    if (plan.kind == on_drag_notes) {
        gint64 *note_ids = on_ui_ids(ids);
        size_t  n        = ids.count;
        if (plan.target.kind == on_sb_trash) {
            ok = on_library_trash_notes(lib, note_ids, n);
        } else {
            ok = on_db_notes_move(db, note_ids, n,
                                  plan.target.kind == on_sb_folder ? plan.target.ident : 0);
            if (ok) on_app_status(lib.app, "Moved %zu note%s to “%s”", n,
                                  n == 1 ? "" : "s", plan.target.raw_name.UTF8String);
        }
        free(note_ids);
    } else {
        on_sb_kind_t src_kind = plan.kind == on_drag_folder ? on_sb_folder : on_sb_trash_folder;
        gint64       src_id   = ids.firstObject.longLongValue;
        NSMutableArray<SidebarItem *> *chain = [NSMutableArray array];
        NSString *name = [self chain_in:self.roots kind:src_kind ident:src_id into:chain]
                       ? chain.lastObject.raw_name : @"";
        NSString *where;
        if (plan.target.kind == on_sb_trash) {
            ok    = on_library_trash_folder(lib, src_id, name);
            where = nil;
        } else if (plan.place == drop_into) {
            ok    = on_db_folder_move(db, src_id, plan.target.kind == on_sb_folder ? plan.target.ident : 0);
            where = plan.target.raw_name;
        } else {
            ok    = folder_move_beside(db, src_id, plan.parent.kind == on_sb_folder ? plan.parent.ident : 0,
                                       plan.target.ident, plan.place == drop_after);
            where = plan.parent.raw_name;
        }
        if (ok && where) {
            on_app_status(lib.app, src_kind == on_sb_trash_folder ? "Folder “%s” restored to “%s”"
                                                                  : "Moved folder “%s” to “%s”",
                          name.UTF8String, where.UTF8String);
            if (lib.sel_kind == on_sb_trash_folder && lib.sel_id == src_id) lib.sel_kind = on_sb_folder;
        }
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
    outline.rowHeight             = on_look_row_h;
    outline.autosaveExpandedItems = NO;
    outline.focusRingType         = NSFocusRingTypeNone;
    outline.delegate              = ctrl;
    outline.dataSource            = ctrl;
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"main"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [outline addTableColumn:column];
    outline.outlineTableColumn = column;
    [outline registerForDraggedTypes:@[on_pasteboard_type_rows]];
    [outline setDraggingSourceOperationMask:NSDragOperationMove forLocal:YES];
    ctrl.outline = outline;

    NotesSidebarScroll *scroll = [[NotesSidebarScroll alloc] initWithFrame:NSZeroRect];
    scroll.controller            = ctrl;
    scroll.documentView          = outline;
    scroll.hasVerticalScroller   = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers    = YES;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    [ctrl refresh];
    return scroll;
}

/*
 * on_library_sidebar_refresh — see library_sidebar.h.
 */
void
on_library_sidebar_refresh(NotesLibrary *lib)
{
    [sidebar refresh];
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
