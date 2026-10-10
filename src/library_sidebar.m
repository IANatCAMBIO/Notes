/*
 * library_sidebar.m — the library's folder/tag sidebar.
 *
 * An NSOutlineView backed by an in-memory tree of SidebarItem objects.
 * The tree matches the GTK model: fixed rows at the top (Pinned Notes, All
 * Notes, Action Items, Notes root with its folder tree, Tags header with
 * tag rows, Trash with trashed-folder children).
 *
 * On a selection change lib.sel_kind / sel_id / sel_name are updated and
 * on_library_notes_refresh() is called — the sidebar owns the selection.
 *
 * The view is a drop target (notes and folders dragged in from the notes
 * pane or within the outline).  Folder drag-reorder lives here too.
 *
 * Fit-to-content: on_library_sidebar_fit sizes the NSSplitView divider so
 * the widest visible row fits, subject to the min/max from look.h.
 */

#import "library_sidebar.h"

#import "look.h"
#import "ui_util.h"

#include "db.h"
#include "app_core.h"

#import "library_notes.h"
#import "library_commands.h"

#include <string.h>

/* ── Pasteboard type for in-app drags ────────────────────────────────────── */

static NSString * const kDragRowType = @"io.notes-appkit.drag-rows";

/* ── SidebarItem ─────────────────────────────────────────────────────────── */

/*
 * SidebarItem — one row in the outline.
 *
 * An item is selectable unless its kind is on_sb_root or on_sb_tags_header;
 * those are purely structural parents.
 */
@interface SidebarItem : NSObject
@property (nonatomic, assign) on_sb_kind_t kind;
@property (nonatomic, assign) gint64       ident;
@property (nonatomic, copy)   NSString    *display;  /* shown in the outline  */
@property (nonatomic, copy)   NSString    *raw_name; /* bare name, no emoji   */
@property (nonatomic, strong) NSMutableArray<SidebarItem *> *children;
@property (nonatomic, assign) BOOL expandable;
- (instancetype)init_kind:(on_sb_kind_t)kind
                    ident:(gint64)ident
                  display:(NSString *)display
                 raw_name:(NSString *)raw_name
               expandable:(BOOL)expandable;
@end

@implementation SidebarItem

/*
 * init_kind:ident:display:raw_name:expandable: — build one item.
 *
 * Inputs:
 *   kind       — the row kind (on_sb_kind_t)
 *   ident      — folder/tag id; 0 for fixed rows
 *   display    — the string shown in the outline cell
 *   raw_name   — the bare name (without emoji, without count)
 *   expandable — whether to show a disclosure triangle
 *
 * Output: the item.
 */
- (instancetype)
init_kind:(on_sb_kind_t)kind
    ident:(gint64)ident
  display:(NSString *)display
 raw_name:(NSString *)raw_name
expandable:(BOOL)expandable
{
    self = [super init];
    _kind       = kind;
    _ident      = ident;
    _display    = [display copy];
    _raw_name   = [raw_name copy];
    _expandable = expandable;
    _children   = expandable ? [NSMutableArray array] : nil;
    return self;
}

/*
 * is_header — is this a non-selectable section header?
 *
 * Output: YES for the Notes root and the Tags header.
 */
- (BOOL)is_header
{
    return _kind == on_sb_root || _kind == on_sb_tags_header;
}

@end

/* ── NotesSidebarController ─────────────────────────────────────────────── */

/*
 * NotesSidebarController — NSOutlineView delegate and data source.
 *
 * Owns the item tree and translates outline events into library state
 * mutations.  It is strongly held by the NSView returned by
 * on_library_sidebar_new; the library window holds that view.
 */
@interface NotesSidebarController : NSObject
    <NSOutlineViewDelegate, NSOutlineViewDataSource,
     NSTextFieldDelegate>

/* Back-references — not retained. */
@property (nonatomic, assign) NotesLibrary *lib;
@property (nonatomic, weak)   NSOutlineView *outline;
@property (nonatomic, weak)   NSSplitView   *split;

/* The root items (visible at the top level of the outline). */
@property (nonatomic, strong) NSMutableArray<SidebarItem *> *roots;

/* The item being renamed inline; nil when no rename is active. */
@property (nonatomic, strong) SidebarItem *renaming;

/* YES while we are repopulating (suppresses selection-change side effects). */
@property (nonatomic, assign) BOOL populating;

- (instancetype)init_lib:(NotesLibrary *)lib;
- (void)refresh;
- (void)select_kind:(on_sb_kind_t)kind ident:(gint64)ident;
- (void)fit_to_content;
- (SidebarItem *)find_kind:(on_sb_kind_t)kind ident:(gint64)ident
                   in_children:(NSArray<SidebarItem *> *)children;
- (SidebarItem *)find_kind:(on_sb_kind_t)kind ident:(gint64)ident;
@end

@implementation NotesSidebarController

/*
 * init_lib: — build the controller for the given library.
 *
 * Inputs:
 *   lib — the library state
 *
 * Output: the controller.
 */
- (instancetype)init_lib:(NotesLibrary *)lib
{
    self = [super init];
    _lib      = lib;
    _roots    = [NSMutableArray array];
    _populating = NO;
    return self;
}

/* ── Tree building ──────────────────────────────────────────────────────── */

/*
 * item_kind:ident:display:raw: — make a SidebarItem (non-expandable leaf).
 *
 * Inputs:
 *   kind    — the row kind
 *   ident   — the folder/tag id; 0 for fixed rows
 *   display — label shown in the outline
 *   raw     — bare name without emoji or count
 *
 * Output: a new leaf item.
 */
static SidebarItem *
item_kind(on_sb_kind_t kind, gint64 ident, NSString *display, NSString *raw)
{
    return [[SidebarItem alloc] init_kind:kind ident:ident
                                  display:display raw_name:raw expandable:NO];
}

/*
 * item_section — a non-leaf section item (Notes root, Tags header, Trash).
 *
 * Output: a new expandable item.
 */
static SidebarItem *
item_section(on_sb_kind_t kind, gint64 ident, NSString *display, NSString *raw)
{
    return [[SidebarItem alloc] init_kind:kind ident:ident
                                  display:display raw_name:raw expandable:YES];
}

/*
 * add_folder_items — recursively build folder items under parent from a
 * GHashTable child map.  Mirrors the GTK add_folder_rows().
 *
 * Inputs:
 *   parent_id    — the folder whose children to build (0 = top level)
 *   into         — the array to append children to
 *   note_counts  — GHashTable mapping folder id → count, or NULL
 *   child_map    — GHashTable from on_db_folder_child_map
 *   sb_counts    — whether to show counts
 *   trash_mode   — YES to mark items as on_sb_trash_folder
 *
 * Output: none (appends to `into`).
 */
static void
add_folder_items(gint64 parent_id,
                 NSMutableArray<SidebarItem *> *into,
                 GHashTable *note_counts,
                 GHashTable *child_map,
                 BOOL sb_counts,
                 BOOL trash_mode)
{
    GList *children = on_db_folder_children(child_map, parent_id);
    for (GList *l = children; l != NULL; l = l->next) {
        OnFolder *f = l->data;
        GList *grand = on_db_folder_children(child_map, f->id);
        BOOL has_kids = grand != NULL;
        on_sb_kind_t kind = trash_mode ? on_sb_trash_folder : on_sb_folder;

        NSString *raw = @(f->name);
        NSString *display;
        BOOL has_emoji = f->emoji != NULL && *f->emoji != '\0';
        NSString *name_part = has_emoji
            ? [NSString stringWithFormat:@"%s  %s", f->emoji, f->name]
            : raw;
        if (sb_counts) {
            gint count = 0;
            if (note_counts != NULL) {
                gpointer val = g_hash_table_lookup(note_counts, &f->id);
                count = val ? GPOINTER_TO_INT(val) : 0;
            }
            display = [name_part stringByAppendingFormat:@" (%d)", count];
        } else {
            display = name_part;
        }

        SidebarItem *item = [[SidebarItem alloc]
            init_kind:kind ident:f->id display:display
             raw_name:raw expandable:has_kids];

        if (has_kids && !trash_mode)
            add_folder_items(f->id, item.children, note_counts,
                             child_map, sb_counts, NO);

        [into addObject:item];
    }
}

/*
 * refresh — rebuild the entire outline from the database, preserving
 * expansion state and restoring the selection.
 *
 * Output: none.
 */
- (void)refresh
{
    OnDatabase *db      = _lib.app->db;
    BOOL        counts  = _lib.app->sidebar_counts;

    /* Save expansion state by (kind, ident). */
    NSMutableSet<NSString *> *expanded = [NSMutableSet set];
    [self collect_expanded:_roots into:expanded];

    /* Save current selection target to restore after rebuild. */
    on_sb_kind_t want_kind = _lib.sel_kind;
    gint64       want_id   = _lib.sel_id;

    _populating = YES;
    [_roots removeAllObjects];

    /* --- Count maps (one query each, only when needed) --- */
    GHashTable *note_counts = counts ? on_db_note_count_map(db) : NULL;
    GHashTable *tag_counts  = counts ? on_db_tag_count_map(db)  : NULL;
    GHashTable *child_map   = on_db_folder_child_map(db);

    /* --- Pinned Notes --- */
    gint n_pinned = on_db_note_count_pinned(db);
    if (n_pinned > 0) {
        NSString *label = counts
            ? [NSString stringWithFormat:@"📌  Pinned Notes (%d)", n_pinned]
            : @"📌  Pinned Notes";
        [_roots addObject:item_kind(on_sb_pinned, 0, label, @"Pinned Notes")];
    }

    /* --- All Notes --- */
    {
        NSString *label = counts
            ? [NSString stringWithFormat:@"🔮  All Notes (%d)",
                        on_db_note_count_visible(db)]
            : @"🔮  All Notes";
        [_roots addObject:item_kind(on_sb_all, 0, label, @"All Notes")];
    }

    /* --- Action Items (only when any exist) --- */
    {
        gint n_actions = 0, n_open = 0;
        on_db_action_counts(db, &n_actions, &n_open);
        if (n_actions > 0) {
            NSString *label = counts
                ? [NSString stringWithFormat:@"❗  Action Items (%d)", n_open]
                : @"❗  Action Items";
            [_roots addObject:item_kind(on_sb_actions, 0, label, @"Action Items")];
        }
    }

    /* --- Notes root + folder tree --- */
    {
        NSString *root_label;
        if (counts) {
            gint root_count = 0;
            if (note_counts)
                root_count = GPOINTER_TO_INT(g_hash_table_lookup(note_counts, &(gint64){0}));
            root_label = [NSString stringWithFormat:@"📓  Notes (%d)", root_count];
        } else {
            root_label = @"📓  Notes";
        }
        SidebarItem *root = item_section(on_sb_root, 0, root_label, @"Notes");
        add_folder_items(0, root.children, note_counts, child_map, counts, NO);
        [_roots addObject:root];
    }

    /* --- Tags header + one item per tag --- */
    {
        GList *tags = on_db_tag_list(db);
        if (tags != NULL) {
            SidebarItem *header = item_section(on_sb_tags_header, 0,
                                               @"🏷️  Tags", @"Tags");
            for (GList *l = tags; l != NULL; l = l->next) {
                OnTag *t = l->data;
                NSString *raw = [NSString stringWithFormat:@"#%s", t->name];
                NSString *label = counts
                    ? [NSString stringWithFormat:@"#%s (%d)", t->name,
                                GPOINTER_TO_INT(g_hash_table_lookup(tag_counts,
                                                                    &t->id))]
                    : raw;
                [header.children addObject:item_kind(on_sb_tag, t->id, label, raw)];
            }
            [_roots addObject:header];
            on_db_tag_list_free(tags);
        }
    }

    /* --- Trash (only when it holds something) --- */
    {
        gint n_trash = on_db_trash_count(db);
        if (n_trash > 0) {
            NSString *label = counts
                ? [NSString stringWithFormat:@"🗑️  Trash (%d)", n_trash]
                : @"🗑️  Trash";
            GList *trashed = on_db_folder_list_trashed(db);
            SidebarItem *trash = item_section(on_sb_trash, 0, label, @"Trash");
            for (GList *l = trashed; l != NULL; l = l->next) {
                OnFolder *f = l->data;
                NSString *raw = @(f->name);
                NSString *disp = (f->emoji && *f->emoji)
                    ? [NSString stringWithFormat:@"%s  %s", f->emoji, f->name]
                    : raw;
                [trash.children addObject:item_kind(on_sb_trash_folder, f->id,
                                                    disp, raw)];
            }
            on_db_folder_list_free(trashed);
            [_roots addObject:trash];
        }
    }

    on_db_folder_child_map_free(child_map);
    if (note_counts) g_hash_table_destroy(note_counts);
    if (tag_counts)  g_hash_table_destroy(tag_counts);

    [_outline reloadData];

    /* Restore expansion state. */
    [self restore_expanded:_roots set:expanded];

    /* Restore selection — or fall back to the first selectable row. */
    SidebarItem *target = [self find_kind:want_kind ident:want_id];
    if (target == nil)
        target = [self first_selectable];
    _populating = NO;

    if (target != nil) {
        /* Reveal ancestors, then select. */
        [self expand_ancestors_of:target in:_roots];
        NSInteger row = [_outline rowForItem:target];
        if (row >= 0) {
            [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                  byExtendingSelection:NO];
            [_outline scrollRowToVisible:row];
        }
        _lib.sel_kind = target.kind;
        _lib.sel_id   = target.ident;
        _lib.sel_name = target.raw_name;
    }
}

/*
 * collect_expanded:into: — walk all items recursively and collect (kind,id)
 * strings for expanded items.
 *
 * Inputs:
 *   items — the items at one level
 *   into  — the set to add strings to
 *
 * Output: none.
 */
- (void)collect_expanded:(NSArray<SidebarItem *> *)items
                    into:(NSMutableSet<NSString *> *)into
{
    for (SidebarItem *it in items) {
        if (it.expandable && [_outline isItemExpanded:it])
            [into addObject:[NSString stringWithFormat:@"%d:%lld",
                             (int)it.kind, (long long)it.ident]];
        if (it.children.count > 0)
            [self collect_expanded:it.children into:into];
    }
}

/*
 * restore_expanded:set: — expand any item whose (kind,id) key is in `set`.
 *
 * Inputs:
 *   items — the items at one level
 *   set   — the keys of items to expand
 *
 * Output: none.
 */
- (void)restore_expanded:(NSArray<SidebarItem *> *)items
                     set:(NSSet<NSString *> *)set
{
    for (SidebarItem *it in items) {
        if (it.expandable) {
            NSString *key = [NSString stringWithFormat:@"%d:%lld",
                             (int)it.kind, (long long)it.ident];
            if ([set containsObject:key])
                [_outline expandItem:it];
        }
        if (it.children.count > 0)
            [self restore_expanded:it.children set:set];
    }
}

/*
 * find_kind:ident:in_children: — search for an item recursively.
 *
 * Inputs:
 *   kind     — the target kind
 *   ident    — the target id
 *   children — the items at this level
 *
 * Output: the item, or nil.
 */
- (SidebarItem *)find_kind:(on_sb_kind_t)kind
                     ident:(gint64)ident
              in_children:(NSArray<SidebarItem *> *)children
{
    for (SidebarItem *it in children) {
        if (it.kind == kind && it.ident == ident)
            return it;
        if (it.children.count > 0) {
            SidebarItem *found = [self find_kind:kind ident:ident
                                    in_children:it.children];
            if (found) return found;
        }
    }
    return nil;
}

/*
 * find_kind:ident: — find an item in the whole tree.
 *
 * Inputs:
 *   kind  — the target kind
 *   ident — the target id
 *
 * Output: the item, or nil.
 */
- (SidebarItem *)find_kind:(on_sb_kind_t)kind ident:(gint64)ident
{
    return [self find_kind:kind ident:ident in_children:_roots];
}

/*
 * first_selectable — the first non-header item in the roots.
 *
 * Output: the item, or nil.
 */
- (SidebarItem *)first_selectable
{
    for (SidebarItem *it in _roots)
        if (![it is_header]) return it;
    return nil;
}

/*
 * path_to:in:into: — the chain of items from this level down to `target`'s
 * parent, outermost first.
 *
 * Inputs:
 *   target — the item to find
 *   items  — the items at this level
 *   into   — receives the ancestors, outermost first
 *
 * Output: YES when target was found under this level.
 */
- (BOOL)path_to:(SidebarItem *)target
             in:(NSArray<SidebarItem *> *)items
           into:(NSMutableArray<SidebarItem *> *)into
{
    for (SidebarItem *it in items) {
        if (it == target) return YES;
        if (it.children.count == 0) continue;
        [into addObject:it];
        if ([self path_to:target in:it.children into:into]) return YES;
        [into removeLastObject];
    }
    return NO;
}

/*
 * expand_ancestors_of:in: — expand every ancestor of `target`, outermost
 * first (an item inside a collapsed parent cannot be expanded), so
 * `target` gets a row.
 *
 * Inputs:
 *   target — the item to reveal
 *   items  — the top-level items
 *
 * Output: YES when target was found.
 */
- (BOOL)expand_ancestors_of:(SidebarItem *)target
                         in:(NSArray<SidebarItem *> *)items
{
    NSMutableArray<SidebarItem *> *path = [NSMutableArray array];
    if (![self path_to:target in:items into:path]) return NO;
    for (SidebarItem *it in path) [_outline expandItem:it];
    return YES;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * select_kind:ident: — select a row from code, expanding ancestors and
 * scrolling it into view.  Updates lib.sel_kind/id/name and calls
 * on_library_notes_refresh.
 *
 * Inputs:
 *   kind  — the row's kind
 *   ident — its id
 *
 * Output: none.
 */
- (void)select_kind:(on_sb_kind_t)kind ident:(gint64)ident
{
    SidebarItem *it = [self find_kind:kind ident:ident];
    if (it == nil) return;
    [self expand_ancestors_of:it in:_roots];
    NSInteger row = [_outline rowForItem:it];
    if (row < 0) return;
    _populating = YES;
    [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
          byExtendingSelection:NO];
    [_outline scrollRowToVisible:row];
    _lib.sel_kind = kind;
    _lib.sel_id   = ident;
    _lib.sel_name = it.raw_name;
    _populating = NO;
    on_library_notes_refresh(_lib);
}

/*
 * fit_to_content — resize the sidebar divider so visible rows fit, subject
 * to on_look_sidebar_min_w and on_look_sidebar_max_fraction × split width.
 *
 * Output: none.
 */
- (void)fit_to_content
{
    if (!_lib.app->sidebar_fit_content) return;
    if (_split == nil) return;
    CGFloat nat_w = _outline.fittingSize.width + 20.0; /* scroll-bar room  */
    CGFloat min_w = on_look_sidebar_min_w;
    CGFloat max_w = _split.bounds.size.width * on_look_sidebar_max_fraction;
    CGFloat want  = MAX(min_w, MIN(nat_w, max_w));
    if (_split.subviews.count >= 2) {
        NSView *sidebar_pane = _split.subviews[0];
        [_split setPosition:want ofDividerAtIndex:0];
        (void)sidebar_pane;
    }
}

/* ── NSOutlineViewDataSource ────────────────────────────────────────────── */

- (NSInteger)outlineView:(NSOutlineView *)outline
  numberOfChildrenOfItem:(id)item
{
    if (item == nil) return (NSInteger)_roots.count;
    SidebarItem *si = item;
    return (NSInteger)si.children.count;
}

- (BOOL)outlineView:(NSOutlineView *)outline
   isItemExpandable:(id)item
{
    SidebarItem *si = item;
    return si.expandable;
}

- (id)outlineView:(NSOutlineView *)outline
            child:(NSInteger)index
           ofItem:(id)item
{
    if (item == nil) return _roots[(NSUInteger)index];
    SidebarItem *si = item;
    return si.children[(NSUInteger)index];
}

- (id)outlineView:(NSOutlineView *)outline
objectValueForTableColumn:(NSTableColumn *)col
           byItem:(id)item
{
    (void)col;
    SidebarItem *si = item;
    return si.display;
}

/* ── NSOutlineViewDelegate ──────────────────────────────────────────────── */

/*
 * outlineView:viewForTableColumn:item: — return the cell view for a row.
 *
 * Header rows are bold; folders and tags are regular-weight.  The Tags
 * header is italic to distinguish it from selectable rows.
 *
 * Inputs:
 *   outline — the outline view
 *   col     — the table column (only one column exists)
 *   item    — the SidebarItem
 *
 * Output: a configured NSTableCellView.
 */
- (NSView *)outlineView:(NSOutlineView *)outline
     viewForTableColumn:(NSTableColumn *)col
                   item:(id)item
{
    (void)col;
    SidebarItem *si = item;
    static NSString * const cell_id = @"SidebarCell";
    NSTableCellView *cell = [outline makeViewWithIdentifier:cell_id owner:self];
    if (cell == nil) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = cell_id;
        NSTextField *label = [NSTextField labelWithString:@""];
        label.identifier = @"label";
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        [cell addSubview:label];
        cell.textField = label;
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4],
            [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
            [label.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    cell.textField.stringValue = si.display;
    BOOL is_hdr = [si is_header];
    NSFontWeight weight = is_hdr ? NSFontWeightSemibold : NSFontWeightRegular;
    cell.textField.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize
                                            weight:weight];
    cell.textField.textColor = is_hdr ? NSColor.secondaryLabelColor
                                       : NSColor.labelColor;
    return cell;
}

- (CGFloat)outlineView:(NSOutlineView *)outline
     heightOfRowByItem:(id)item
{
    (void)outline; (void)item;
    return on_look_row_h;
}

/*
 * outlineView:shouldSelectItem: — the Notes root and Tags header are not
 * selectable.
 */
- (BOOL)outlineView:(NSOutlineView *)outline
   shouldSelectItem:(id)item
{
    SidebarItem *si = item;
    return ![si is_header];
}

/*
 * outlineViewSelectionDidChange: — the user selected a new row.  Update the
 * library state and refresh the notes pane.
 *
 * Inputs:
 *   note — the selection-change notification
 *
 * Output: none.
 */
- (void)outlineViewSelectionDidChange:(NSNotification *)note
{
    (void)note;
    if (_populating) return;
    NSInteger row = _outline.selectedRow;
    if (row < 0) return;
    SidebarItem *si = [_outline itemAtRow:row];
    if (si == nil || [si is_header]) return;

    _lib.sel_kind = si.kind;
    _lib.sel_id   = si.ident;
    _lib.sel_name = si.raw_name;
    on_library_notes_refresh(_lib);
}

/* ── Inline rename ──────────────────────────────────────────────────────── */

/*
 * start_rename: — begin inline rename for a folder row.  Places a text
 * field over the cell's label.
 *
 * Inputs:
 *   it — the item to rename (must be a folder)
 *
 * Output: none.
 */
- (void)start_rename:(SidebarItem *)it
{
    NSInteger row = [_outline rowForItem:it];
    if (row < 0) return;
    NSTableCellView *cell = [_outline viewAtColumn:0 row:row makeIfNecessary:NO];
    if (cell == nil) return;
    _renaming = it;
    NSTextField *tf = cell.textField;
    tf.editable = YES;
    tf.delegate = self;
    tf.stringValue = it.raw_name;
    [_outline.window makeFirstResponder:tf];
    [tf selectText:nil];
}

/*
 * control:textShouldEndEditing: — commit or cancel the rename when the
 * text field ends editing.
 *
 * Inputs:
 *   control — the text field
 *   editor  — the field editor
 *
 * Output: YES to allow ending.
 */
- (BOOL)control:(NSControl *)control
    textShouldEndEditing:(NSText *)editor
{
    if (_renaming == nil) return YES;
    NSString *new_name = [editor string];
    SidebarItem *it = _renaming;
    _renaming = nil;
    ((NSTextField *)control).editable = NO;
    ((NSTextField *)control).delegate = nil;
    if (new_name.length > 0 && ![new_name isEqualToString:it.raw_name]) {
        on_db_folder_rename(_lib.app->db, it.ident, new_name.UTF8String);
        on_library_refresh_all(_lib);
    }
    return YES;
}

/* ── Context menu ───────────────────────────────────────────────────────── */

/*
 * context_menu_for_item: — build a context menu for a sidebar row.
 *
 * Inputs:
 *   it — the item being right-clicked (may be nil)
 *
 * Output: the menu, or nil.
 */
- (NSMenu *)context_menu_for_item:(SidebarItem *)it
{
    if (it == nil) return nil;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    if (it.kind == on_sb_folder) {
        [menu addItemWithTitle:@"New Subfolder…"
                        action:@selector(cmd_new_subfolder:)
                 keyEquivalent:@""];
        [menu addItemWithTitle:@"Rename"
                        action:@selector(cmd_rename:)
                 keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItemWithTitle:@"Folder Info…"
                        action:@selector(cmd_folder_info:)
                 keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItemWithTitle:@"Sort Subfolders Alphabetically"
                        action:@selector(cmd_sort_subfolders:)
                 keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:on_command_menu_item(@"delete-folder")];
    } else if (it.kind == on_sb_trash_folder) {
        [menu addItem:on_command_menu_item(@"folder-restore")];
        [menu addItem:on_command_menu_item(@"delete-folder")];
    } else if (it.kind == on_sb_trash) {
        [menu addItem:on_command_menu_item(@"empty-trash")];
    }
    for (NSMenuItem *mi in menu.itemArray) {
        if (mi.action != @selector(run_command:)) mi.target = self;   /* commands go up the chain */
    }
    return menu.numberOfItems > 0 ? menu : nil;
}

- (NSMenu *)outlineView:(NSOutlineView *)outline
            menuForEvent:(NSEvent *)event
{
    NSPoint pt = [outline convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [outline rowAtPoint:pt];
    SidebarItem *it = (row >= 0) ? [outline itemAtRow:row] : nil;
    if (it != nil && ![it is_header]) {
        [outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
              byExtendingSelection:NO];
    }
    return [self context_menu_for_item:it];
}

/* ── Context menu actions ────────────────────────────────────────────────── */

- (void)cmd_rename:(id)sender
{
    (void)sender;
    NSInteger row = _outline.selectedRow;
    SidebarItem *it = (row >= 0) ? [_outline itemAtRow:row] : nil;
    if (it && it.kind == on_sb_folder) [self start_rename:it];
}

- (void)cmd_new_subfolder:(id)sender
{
    (void)sender;
    NSInteger row = _outline.selectedRow;
    SidebarItem *parent = (row >= 0) ? [_outline itemAtRow:row] : nil;
    if (!parent || parent.kind != on_sb_folder) return;
    gint64 new_id = on_db_folder_create(_lib.app->db, parent.ident,
                                         "New Folder", ON_AI_MODE_NORMAL, "");
    on_library_refresh_all(_lib);
    [self select_kind:on_sb_folder ident:new_id];
    SidebarItem *new_it = [self find_kind:on_sb_folder ident:new_id];
    if (new_it) [self start_rename:new_it];
}

- (void)cmd_folder_info:(id)sender
{
    (void)sender;
    /* Folder Info is a window command; route through the responder chain. */
    NSMenuItem *item = on_command_menu_item(@"folder-info");
    [NSApp sendAction:@selector(run_command:) to:nil from:item];
}

- (void)cmd_sort_subfolders:(id)sender
{
    (void)sender;
    NSInteger row = _outline.selectedRow;
    SidebarItem *it = (row >= 0) ? [_outline itemAtRow:row] : nil;
    if (!it || it.kind != on_sb_folder) return;
    on_db_folder_sort_children(_lib.app->db, it.ident);
    on_library_refresh_all(_lib);
}

/* ── Drag source (folder rows) ──────────────────────────────────────────── */

- (id<NSPasteboardWriting>)outlineView:(NSOutlineView *)outline
          pasteboardWriterForItem:(id)item
{
    SidebarItem *si = item;
    if (si.kind != on_sb_folder) return nil;
    NSPasteboardItem *pb = [[NSPasteboardItem alloc] init];
    NSString *payload = [NSString stringWithFormat:@"folder:%lld",
                         (long long)si.ident];
    [pb setString:payload forType:kDragRowType];
    return pb;
}

/* ── Drop target ────────────────────────────────────────────────────────── */

- (NSDragOperation)outlineView:(NSOutlineView *)outline
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)proposed_item
            proposedChildIndex:(NSInteger)idx
{
    NSPasteboard *pb = info.draggingPasteboard;
    NSString *payload = [pb stringForType:kDragRowType];
    if (payload == nil) return NSDragOperationNone;

    SidebarItem *target = proposed_item;
    if (target == nil) return NSDragOperationNone;

    if ([payload hasPrefix:@"folder:"]) {
        /* A folder drag: can drop onto another folder or the Notes root. */
        if (target.kind == on_sb_folder || target.kind == on_sb_root)
            return NSDragOperationMove;
    } else if ([payload hasPrefix:@"notes:"]) {
        /* Notes dragged from the notes pane; can drop onto a folder. */
        if (target.kind == on_sb_folder)
            return NSDragOperationMove;
    }
    return NSDragOperationNone;
}

- (BOOL)outlineView:(NSOutlineView *)outline
         acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item
         childIndex:(NSInteger)idx
{
    NSPasteboard *pb = info.draggingPasteboard;
    NSString *payload = [pb stringForType:kDragRowType];
    if (payload == nil) return NO;
    SidebarItem *target = item;
    if (target == nil) return NO;

    if ([payload hasPrefix:@"folder:"]) {
        gint64 folder_id = [payload substringFromIndex:7].longLongValue;
        gint64 new_parent = (target.kind == on_sb_root) ? 0 : target.ident;
        if (on_db_folder_move(_lib.app->db, folder_id, new_parent)) {
            on_library_refresh_all(_lib);
            [self select_kind:on_sb_folder ident:folder_id];
            return YES;
        }
    } else if ([payload hasPrefix:@"notes:"]) {
        /* Note drop: move notes to the target folder in one transaction. */
        NSString *ids_str = [payload substringFromIndex:6];
        NSArray<NSString *> *parts = [ids_str componentsSeparatedByString:@","];
        gsize n = parts.count;
        gint64 *ids_arr = g_new(gint64, n);
        for (gsize i = 0; i < n; i++)
            ids_arr[i] = parts[i].longLongValue;
        gboolean ok = on_db_notes_move(_lib.app->db, ids_arr, n, target.ident);
        g_free(ids_arr);
        if (ok) {
            on_library_refresh_all(_lib);
            return YES;
        }
    }
    return NO;
}

/* Register drop types. */
- (void)register_drop_types
{
    [_outline registerForDraggedTypes:@[kDragRowType]];
}

@end

/* ── File-static controller, one per library window ─────────────────────── */

/*
 * g_ctrl — the sidebar controller; one per library window, stored here so
 * on_library_sidebar_refresh and on_library_sidebar_select can reach it
 * without the caller passing extra state.
 *
 * This is a single-window app: one library window, one controller.
 */
static NotesSidebarController *g_ctrl;

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_library_sidebar_new — see library_sidebar.h.
 */
NSView *
on_library_sidebar_new(NotesLibrary *lib)
{
    NotesSidebarController *ctrl = [[NotesSidebarController alloc]
                                     init_lib:lib];
    g_ctrl = ctrl;

    /* Build the outline view. */
    NSOutlineView *outline = [[NSOutlineView alloc] initWithFrame:NSZeroRect];
    outline.headerView = nil;
    outline.allowsMultipleSelection = NO;
    outline.style = NSTableViewStyleSourceList;
    outline.rowHeight = on_look_row_h;
    outline.autosaveExpandedItems = NO;
    outline.indentationPerLevel = 14.0;
    outline.focusRingType = NSFocusRingTypeNone;
    outline.delegate   = ctrl;
    outline.dataSource = ctrl;
    outline.menu = nil;                  /* set per-row by menuForEvent:  */

    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"main"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [outline addTableColumn:col];
    outline.outlineTableColumn = col;
    ctrl.outline = outline;

    /* Wrap in a scroll view. */
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView          = outline;
    scroll.hasVerticalScroller   = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers    = YES;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    [ctrl register_drop_types];
    [ctrl refresh];

    return scroll;
}

/*
 * on_library_sidebar_refresh — see library_sidebar.h.
 */
void
on_library_sidebar_refresh(NotesLibrary *lib)
{
    (void)lib;
    if (g_ctrl == nil) return;
    [g_ctrl refresh];
    [g_ctrl fit_to_content];
}

/*
 * on_library_sidebar_select — see library_sidebar.h.
 */
void
on_library_sidebar_select(NotesLibrary *lib, on_sb_kind_t kind, gint64 ident)
{
    (void)lib;
    if (g_ctrl == nil) {
        lib.sel_kind = kind;
        lib.sel_id   = ident;
        return;
    }
    [g_ctrl select_kind:kind ident:ident];
}

/*
 * on_library_sidebar_fit — see library_sidebar.h.
 */
void
on_library_sidebar_fit(NotesLibrary *lib)
{
    (void)lib;
    if (g_ctrl == nil) return;
    [g_ctrl fit_to_content];
}
