/*
 * library_notes.m — the library's notes pane: list view (NSTableView) and
 * grid view (NSCollectionView), plus the Action Items pane.  All three share
 * one selection.  The pane switches between them per on_library_view().
 *
 * Architecture:
 *   NotesNoteRecord — a value object for one row/card in either view.
 *   NotesNotesList  — the NSTableView controller (list mode).
 *   NotesNotesGrid  — the NSCollectionView controller (grid mode).
 *   NotesActionsPane — the Action Items list controller.
 *   NotesNotesPane  — the outer container that owns all three and switches.
 *
 * Thumbnails are decoded on a worker (on_ui_thumbnail) and cached in an
 * NSCache keyed on note_id + updated_at.  The cache is bounded and released
 * when the pane is torn down.
 */

#import "library_notes.h"
#import "library_commands.h"
#import "editor_window.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "db.h"
#include "serialize.h"
#include "action_edit.h"

#include <glib.h>
#include <stdint.h>
#include <time.h>

/* ── Sizes and layout ───────────────────────────────────────────────────── */

/* Thumbnail square side in points.  The thumbnail cache key uses updated_at
 * so a stale entry is replaced after the next save. */
static const CGFloat thumb_size   = 52.0;

/* Height of one list row. */
static const CGFloat row_h_normal = 52.0;

/* Grid card size.  A 3-column layout at 240 pt wide window fits two cards;
 * the collection view scrolls vertically. */
static const CGFloat card_w = 140.0;
static const CGFloat card_h = 160.0;

/* Action Items row height. */
static const CGFloat action_row_h = 44.0;

/* Columns in the list view. */
typedef NS_ENUM(NSInteger, NotesListColumn) {
    col_title = 0,
    col_modified,
    col_col_count,
};

/* ── Thumbnail cache ────────────────────────────────────────────────────── */

/* One cached thumbnail entry: image keyed by note_id + updated_at. */
@interface NotesThumbnailKey : NSObject <NSCopying>
@property (nonatomic, assign) gint64 note_id;
@property (nonatomic, assign) gint64 updated_at;
@end

@implementation NotesThumbnailKey

/*
 * init_with_note:updated: — build a cache key.
 *
 * Inputs:
 *   nid    — note primary key
 *   ts     — the note's updated_at timestamp
 *
 * Output: the key.
 */
- (instancetype)
init_with_note:(gint64)nid updated:(gint64)ts
{
    if (!(self = [super init])) return nil;
    _note_id    = nid;
    _updated_at = ts;
    return self;
}

- (id)copyWithZone:(NSZone *)zone { (void)zone; return self; }

- (BOOL)isEqual:(id)other
{
    if (![other isKindOfClass:[NotesThumbnailKey class]]) return NO;
    NotesThumbnailKey *o = other;
    return _note_id == o.note_id && _updated_at == o.updated_at;
}

- (NSUInteger)hash
{
    return (NSUInteger)(_note_id * 1000003 ^ _updated_at);
}

@end

/* ── Note record ────────────────────────────────────────────────────────── */

/* A row/card's data, built on the main thread from OnNoteMeta, immutable. */
@interface NotesNoteRecord : NSObject
@property (nonatomic, assign) gint64      note_id;
@property (nonatomic, copy)   NSString   *title;
@property (nonatomic, copy)   NSString   *modified;   /* formatted date */
@property (nonatomic, assign) gint64      updated_at;  /* for cache key */
@property (nonatomic, copy)   NSString   *preview;    /* first line of body */
@property (nonatomic, strong) NSImage    *thumbnail;  /* nil until decoded */
@end

@implementation NotesNoteRecord
@end

/* ── Shared thumbnail helper ────────────────────────────────────────────── */

/*
 * format_date — format a UNIX timestamp like "Jun 3, 2026 14:05".
 *
 * Inputs:
 *   ts — UNIX timestamp (seconds)
 *
 * Output: formatted string.
 */
static NSString *
format_date(gint64 ts)
{
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)ts];
    static NSDateFormatter *fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        fmt = [[NSDateFormatter alloc] init];
        fmt.dateStyle = NSDateFormatterMediumStyle;
        fmt.timeStyle = NSDateFormatterShortStyle;
    });
    return [fmt stringFromDate:date];
}

/*
 * build_records — build an array of NotesNoteRecord from a GList of OnNoteMeta.
 * Frees the GList.
 *
 * Inputs:
 *   notes — GList of OnNoteMeta *; freed by this call
 *
 * Output: array of NotesNoteRecord, in display order.
 */
static NSArray<NotesNoteRecord *> *
build_records(GList *notes)
{
    NSMutableArray<NotesNoteRecord *> *arr = [NSMutableArray array];
    for (GList *l = notes; l != NULL; l = l->next) {
        const OnNoteMeta *m = l->data;
        NotesNoteRecord *rec = [[NotesNoteRecord alloc] init];
        rec.note_id    = m->id;
        rec.title      = @(m->title ?: "");
        rec.modified   = format_date(m->updated_at);
        rec.updated_at = m->updated_at;
        /* Preview: loaded lazily; this avoids one query per note on refresh */
        rec.preview    = @"";
        [arr addObject:rec];
    }
    on_db_note_list_free(notes);
    return arr;
}

/* ── Activation: Enter or double-click opens ────────────────────────────── */

/*
 * is_activate_key — is this key event Return or keypad Enter (no Command,
 * Option or Control)?
 *
 * Inputs:
 *   event — a key-down event
 *
 * Output: YES for an activating key.
 */
static BOOL
is_activate_key(NSEvent *event)
{
    NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl);
    NSString *chars = event.charactersIgnoringModifiers;
    if (mods != 0 || chars.length != 1) return NO;
    unichar c = [chars characterAtIndex:0];
    return c == '\r' || c == NSEnterCharacter;
}

/*
 * NotesActivatingTable — a table whose Return/Enter key sends its
 * doubleAction, as a double-click does; activated_rows tells the two apart.
 */
@interface NotesActivatingTable : NSTableView
@property (nonatomic, readonly) BOOL by_key;   /* YES while Return/Enter sends */
@end

@implementation NotesActivatingTable

/*
 * keyDown: — Return or Enter with a selection sends the doubleAction;
 * every other key goes to NSTableView.
 *
 * Inputs:
 *   event — the key event
 */
- (void)
keyDown:(NSEvent *)event
{
    if (is_activate_key(event) && self.selectedRowIndexes.count > 0 && self.doubleAction) {
        _by_key = YES;
        [self sendAction:self.doubleAction to:self.target];
        _by_key = NO;
        return;
    }
    [super keyDown:event];
}

@end

/*
 * activated_rows — the rows a table's doubleAction applies to: the
 * double-clicked row, or the selection when Return/Enter sent it.
 *
 * Inputs:
 *   table — the table that sent the action
 *
 * Output: the row indexes; empty for a double-click on no row.
 */
static NSIndexSet *
activated_rows(NotesActivatingTable *table)
{
    if (table.by_key) return table.selectedRowIndexes;
    if (table.clickedRow >= 0) return [NSIndexSet indexSetWithIndex:(NSUInteger)table.clickedRow];
    return [NSIndexSet indexSet];         /* a double-click on empty space */
}

/*
 * NotesActivatingGrid — a collection view that reports a double-click on
 * an item, and Return/Enter on the selection, to activate_handler.
 */
@interface NotesActivatingGrid : NSCollectionView
@property (nonatomic, copy) void (^activate_handler)(NSSet<NSIndexPath *> *paths);
@end

@implementation NotesActivatingGrid

/*
 * mouseDown: — NSCollectionView selects; a second click on an item then
 * activates it.
 *
 * Inputs:
 *   event — the mouse event
 */
- (void)
mouseDown:(NSEvent *)event
{
    [super mouseDown:event];
    if (event.clickCount != 2 || !self.activate_handler) return;
    NSPoint      at   = [self convertPoint:event.locationInWindow fromView:nil];
    NSIndexPath *path = [self indexPathForItemAtPoint:at];
    if (path) self.activate_handler([NSSet setWithObject:path]);
}

/*
 * keyDown: — Return or Enter activates the selection; every other key
 * goes to NSCollectionView.
 *
 * Inputs:
 *   event — the key event
 */
- (void)
keyDown:(NSEvent *)event
{
    if (is_activate_key(event) && self.selectionIndexPaths.count > 0 && self.activate_handler) {
        self.activate_handler(self.selectionIndexPaths);
        return;
    }
    [super keyDown:event];
}

@end

/* ── Notes list (NSTableView) ───────────────────────────────────────────── */

static NSString * const list_cell_id      = @"note";
static NSString * const list_cell_thumb   = @"thumb";

@interface NotesNotesList : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, weak)   NotesLibrary               *lib;
@property (nonatomic, strong) NotesActivatingTable        *table;
@property (nonatomic, strong) NSScrollView                *scroll;
@property (nonatomic, strong) NSArray<NotesNoteRecord *>  *records;
@property (nonatomic, strong) NSCache                     *thumb_cache;
@end

@implementation NotesNotesList

/*
 * init_with_lib:cache: — build the list view.
 *
 * Inputs:
 *   lib   — the library state
 *   cache — the shared thumbnail cache
 *
 * Output: the list controller.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib cache:(NSCache *)cache
{
    if (!(self = [super init])) return nil;
    _lib         = lib;
    _records     = @[];
    _thumb_cache = cache;
    [self build_table];
    return self;
}

/*
 * build_table — construct the NSTableView and its scroll container.
 *
 * Output: none (sets self.table and self.scroll).
 */
- (void)
build_table
{
    _table = [[NotesActivatingTable alloc] initWithFrame:NSZeroRect];
    _table.dataSource          = self;
    _table.delegate            = self;
    _table.target              = self;
    _table.doubleAction        = @selector(open_activated:);
    _table.rowHeight           = row_h_normal;
    _table.intercellSpacing    = NSMakeSize(0, 0);
    _table.allowsMultipleSelection = YES;
    _table.allowsEmptySelection    = YES;
    _table.usesAlternatingRowBackgroundColors = NO;
    _table.headerView = nil;               /* header hidden: sort via menu  */
    _table.rowSizeStyle = NSTableViewRowSizeStyleCustom;

    /* Single "Note" column filling the full width. */
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"note"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView           = _table;
    _scroll.hasVerticalScroller    = YES;
    _scroll.hasHorizontalScroller  = NO;
    _scroll.autohidesScrollers     = YES;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
}

/*
 * reload_with: — replace the record array and reload the table, preserving
 * the scroll position and selection when the selection did not change.
 *
 * Inputs:
 *   records — new record array
 *   keep    — YES when this is a content refresh (same folder); NO on navigation
 *
 * Output: none.
 */
- (void)
reload_with:(NSArray<NotesNoteRecord *> *)records keep:(BOOL)keep
{
    NSIndexSet *sel = keep ? _table.selectedRowIndexes : nil;
    NSPoint scroll  = keep ? _scroll.documentVisibleRect.origin : NSZeroPoint;

    _records = records;
    [_table reloadData];

    if (keep && sel.count > 0 && scroll.y > 0) {
        [_table selectRowIndexes:sel byExtendingSelection:NO];
        [_scroll.documentView scrollPoint:scroll];
    }

    /* Kick off thumbnail loads for visible rows. */
    [self load_thumbnails_for_visible];
}

/*
 * update_note: — update just the row for note_id when found; used by the
 * one-row update path (on_library_notes_note_saved).
 *
 * Inputs:
 *   note_id — the note that was saved
 *   record  — the new record for that note (already in self.records)
 *
 * Output: none.
 */
- (void)
update_note:(gint64)note_id
{
    NSUInteger idx = [_records indexOfObjectPassingTest:^BOOL(NotesNoteRecord *r, NSUInteger i, BOOL *stop) {
        (void)i; *stop = r.note_id == note_id; return r.note_id == note_id;
    }];
    if (idx == NSNotFound) return;
    NSIndexSet *rows = [NSIndexSet indexSetWithIndex:idx];
    NSIndexSet *cols = [NSIndexSet indexSetWithIndex:0];
    [_table reloadDataForRowIndexes:rows columnIndexes:cols];
    [self load_thumbnail_for_row:idx];
}

/*
 * selected_ids — the note ids currently selected in the table.
 *
 * Output: array of NSNumber with note ids.
 */
- (NSArray<NSNumber *> *)
selected_ids
{
    NSMutableArray *ids = [NSMutableArray array];
    [_table.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL *stop) {
        (void)stop;
        if (idx < _records.count)
            [ids addObject:@(_records[idx].note_id)];
    }];
    return ids;
}

/* ── Thumbnail loading ──────────────────────────────────────────────────── */

/*
 * load_thumbnails_for_visible — enqueue thumbnail fetches for all visible
 * rows that have no cached thumbnail.
 *
 * Output: none.
 */
- (void)
load_thumbnails_for_visible
{
    NSRange visible = [_table rowsInRect:_scroll.documentVisibleRect];
    for (NSUInteger i = visible.location; i < NSMaxRange(visible) && i < _records.count; i++)
        [self load_thumbnail_for_row:i];
}

/*
 * load_thumbnail_for_row: — decode the thumbnail for the record at `row`
 * on a worker if not already cached, then reload the row on the main thread.
 *
 * Inputs:
 *   row — the table row index
 *
 * Output: none.
 */
- (void)
load_thumbnail_for_row:(NSUInteger)row
{
    if (row >= _records.count) return;
    NotesNoteRecord *rec = _records[row];
    NotesThumbnailKey *key = [[NotesThumbnailKey alloc]
        init_with_note:rec.note_id updated:rec.updated_at];
    NSImage *cached = [_thumb_cache objectForKey:key];
    if (cached) { rec.thumbnail = cached; return; }

    /* Capture inputs before going off-thread.  Workers open their own db
     * connection; never touch the main thread's OnDatabase from a worker. */
    gint64  note_id    = rec.note_id;
    gint64  updated_at = rec.updated_at;
    NSString *db_path  = @(_lib.app->db->path);
    __weak typeof(self) ws = self;
    NSCache *cache = _thumb_cache;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            OnDatabase *wdb = on_db_open(db_path.UTF8String);
            NSImage *thumb = nil;
            if (wdb) {
                gsize blob_len = 0;
                guint8 *blob = on_db_note_load(wdb, note_id, &blob_len);
                if (blob && blob_len > 0) {
                    GBytes *png = on_note_image_nth_png(blob, blob_len, 0);
                    if (png) {
                        gsize n;
                        const guint8 *bytes = g_bytes_get_data(png, &n);
                        thumb = on_ui_thumbnail(bytes, n, thumb_size, thumb_size, 2.0);
                        g_bytes_unref(png);
                    }
                    g_free(blob);
                }
                on_db_close(wdb);
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                NotesNotesList *s = ws;
                if (!s) return;
                if (thumb) {
                    NotesThumbnailKey *k = [[NotesThumbnailKey alloc]
                        init_with_note:note_id updated:updated_at];
                    [cache setObject:thumb forKey:k];
                }
                /* Find the record again: the array may have been replaced. */
                NSUInteger idx = [s.records indexOfObjectPassingTest:^BOOL(NotesNoteRecord *r, NSUInteger i, BOOL *stop) {
                    (void)i; *stop = r.note_id == note_id; return r.note_id == note_id;
                }];
                if (idx == NSNotFound) return;
                s.records[idx].thumbnail = thumb;
                NSIndexSet *rows = [NSIndexSet indexSetWithIndex:idx];
                NSIndexSet *cols = [NSIndexSet indexSetWithIndex:0];
                [s.table reloadDataForRowIndexes:rows columnIndexes:cols];
            });
        }
    });
}

/* ── NSTableViewDataSource ──────────────────────────────────────────────── */

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return (NSInteger)_records.count;
}

/* ── NSTableViewDelegate ────────────────────────────────────────────────── */

- (CGFloat)tableView:(NSTableView *)tv heightOfRow:(NSInteger)row
{
    (void)tv; (void)row;
    return row_h_normal;
}

- (NSView *)tableView:(NSTableView *)tv
   viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row
{
    (void)col;
    if (row < 0 || (NSUInteger)row >= _records.count) return nil;
    NotesNoteRecord *rec = _records[(NSUInteger)row];

    /* Reuse a cell that carries the thumb view, title label, date label. */
    NSTableCellView *cell = [tv makeViewWithIdentifier:list_cell_id owner:nil];
    if (!cell) {
        cell = [self make_list_cell];
    }

    /* Find subviews by tag. */
    NSImageView  *thumb_view = (NSImageView *)  [cell viewWithTag:1];
    NSTextField  *title_lbl  = (NSTextField *)  [cell viewWithTag:2];
    NSTextField  *date_lbl   = (NSTextField *)  [cell viewWithTag:3];

    title_lbl.stringValue = rec.title;
    date_lbl.stringValue  = rec.modified;

    if (rec.thumbnail) {
        thumb_view.image = rec.thumbnail;
        thumb_view.hidden = NO;
    } else {
        thumb_view.image  = nil;
        thumb_view.hidden = YES;
        [self load_thumbnail_for_row:(NSUInteger)row];
    }

    return cell;
}

/*
 * make_list_cell — build a fresh list cell with thumbnail, title and date.
 *
 * Output: the new cell.
 */
- (NSTableCellView *)
make_list_cell
{
    NSTableCellView *cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cell.identifier = list_cell_id;

    NSImageView *thumb = on_ui_image_view(nil, thumb_size, thumb_size);
    thumb.translatesAutoresizingMaskIntoConstraints = NO;
    thumb.imageScaling = NSImageScaleProportionallyUpOrDown;
    thumb.tag = 1;

    NSTextField *title = [NSTextField labelWithString:@""];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    title.lineBreakMode = NSLineBreakByTruncatingTail;
    title.tag = 2;

    NSTextField *date = [NSTextField labelWithString:@""];
    date.translatesAutoresizingMaskIntoConstraints = NO;
    date.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    date.textColor = NSColor.secondaryLabelColor;
    date.tag = 3;

    [cell addSubview:thumb];
    [cell addSubview:title];
    [cell addSubview:date];

    [NSLayoutConstraint activateConstraints:@[
        [thumb.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
        [thumb.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        [thumb.widthAnchor constraintEqualToConstant:thumb_size],
        [thumb.heightAnchor constraintEqualToConstant:thumb_size],

        [title.leadingAnchor constraintEqualToAnchor:thumb.trailingAnchor constant:8],
        [title.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
        [title.topAnchor constraintEqualToAnchor:cell.topAnchor constant:10],

        [date.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [date.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:2],
    ]];
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note
{
    (void)note;
    /* The outer pane observes the table's selection. */
}

- (BOOL)tableView:(NSTableView *)tv shouldSelectRow:(NSInteger)row
{
    (void)tv; (void)row;
    return YES;
}

/*
 * open_activated: — the table's doubleAction (double-click, or Return /
 * Enter): open the activated notes in the editor.
 *
 * Inputs:
 *   sender — the table (unused: activated_rows reads _table)
 */
- (void)
open_activated:(id)sender
{
    (void)sender;
    [activated_rows(_table) enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL *stop) {
        (void)stop;
        if (idx < self->_records.count && self->_lib.app->open_note)
            self->_lib.app->open_note(self->_lib.app, self->_records[idx].note_id);
    }];
}

@end

/* ── Notes grid (NSCollectionView) ──────────────────────────────────────── */

static NSString * const card_item_id = @"card";

/* A grid card showing the thumbnail and title. */
@interface NotesCardItem : NSCollectionViewItem
@property (nonatomic, strong) NSImageView *thumb_view;
@property (nonatomic, strong) NSTextField *title_lbl;
@end

@implementation NotesCardItem

- (void)loadView
{
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, card_w, card_h)];
    self.view = v;

    _thumb_view = on_ui_image_view(nil, card_w - 16, card_h - 32);
    _thumb_view.translatesAutoresizingMaskIntoConstraints = NO;
    _thumb_view.imageScaling = NSImageScaleProportionallyUpOrDown;
    _thumb_view.wantsLayer = YES;
    _thumb_view.layer.backgroundColor = NSColor.tertiaryLabelColor.CGColor;

    _title_lbl = [NSTextField labelWithString:@""];
    _title_lbl.translatesAutoresizingMaskIntoConstraints = NO;
    _title_lbl.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _title_lbl.lineBreakMode = NSLineBreakByTruncatingTail;
    _title_lbl.alignment = NSTextAlignmentCenter;

    [v addSubview:_thumb_view];
    [v addSubview:_title_lbl];

    [NSLayoutConstraint activateConstraints:@[
        [_thumb_view.topAnchor constraintEqualToAnchor:v.topAnchor constant:8],
        [_thumb_view.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:8],
        [_thumb_view.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-8],
        [_thumb_view.bottomAnchor constraintEqualToAnchor:_title_lbl.topAnchor constant:-4],

        [_title_lbl.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:4],
        [_title_lbl.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-4],
        [_title_lbl.bottomAnchor constraintEqualToAnchor:v.bottomAnchor constant:-4],
        [_title_lbl.heightAnchor constraintEqualToConstant:20],
    ]];
}

@end

@interface NotesNotesGrid : NSObject <NSCollectionViewDataSource, NSCollectionViewDelegate>
@property (nonatomic, weak)   NotesLibrary               *lib;
@property (nonatomic, strong) NSCollectionView            *grid;
@property (nonatomic, strong) NSScrollView                *scroll;
@property (nonatomic, strong) NSArray<NotesNoteRecord *>  *records;
@property (nonatomic, strong) NSCache                     *thumb_cache;
@end

@implementation NotesNotesGrid

/*
 * init_with_lib:cache: — build the grid view.
 *
 * Inputs:
 *   lib   — the library state
 *   cache — the shared thumbnail cache
 *
 * Output: the grid controller.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib cache:(NSCache *)cache
{
    if (!(self = [super init])) return nil;
    _lib         = lib;
    _records     = @[];
    _thumb_cache = cache;
    [self build_grid];
    return self;
}

/*
 * build_grid — construct the NSCollectionView and scroll container.
 *
 * Output: none.
 */
- (void)
build_grid
{
    NSCollectionViewFlowLayout *layout = [[NSCollectionViewFlowLayout alloc] init];
    layout.itemSize        = NSMakeSize(card_w, card_h);
    layout.minimumInteritemSpacing = 8;
    layout.minimumLineSpacing      = 8;
    layout.sectionInset = NSEdgeInsetsMake(12, 12, 12, 12);

    NotesActivatingGrid *grid = [[NotesActivatingGrid alloc] init];
    __weak NotesNotesGrid *ws = self;
    grid.activate_handler = ^(NSSet<NSIndexPath *> *paths) {
        NotesNotesGrid *s = ws;
        if (!s || !s.lib.app->open_note) return;
        for (NSIndexPath *ip in paths) {
            if (ip.item >= 0 && (NSUInteger)ip.item < s.records.count)
                s.lib.app->open_note(s.lib.app, s.records[(NSUInteger)ip.item].note_id);
        }
    };
    _grid = grid;
    _grid.collectionViewLayout = layout;
    _grid.dataSource = self;
    _grid.delegate   = self;
    _grid.selectable = YES;
    _grid.allowsMultipleSelection = YES;
    _grid.allowsEmptySelection    = YES;
    _grid.backgroundColors = @[NSColor.windowBackgroundColor];
    [_grid registerClass:[NotesCardItem class] forItemWithIdentifier:card_item_id];

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView          = _grid;
    _scroll.hasVerticalScroller   = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers    = YES;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
}

/*
 * reload_with: — replace the record array and reload the grid.
 *
 * Inputs:
 *   records — new record array
 *
 * Output: none.
 */
- (void)
reload_with:(NSArray<NotesNoteRecord *> *)records
{
    _records = records;
    [_grid reloadData];
}

/*
 * selected_ids — the note ids currently selected in the grid.
 *
 * Output: array of NSNumber with note ids.
 */
- (NSArray<NSNumber *> *)
selected_ids
{
    NSMutableArray *ids = [NSMutableArray array];
    for (NSIndexPath *ip in _grid.selectionIndexPaths) {
        NSUInteger idx = ip.item;
        if (idx < _records.count)
            [ids addObject:@(_records[idx].note_id)];
    }
    return ids;
}

/* ── NSCollectionViewDataSource ──────────────────────────────────────────── */

- (NSInteger)numberOfSectionsInCollectionView:(NSCollectionView *)cv
{
    (void)cv;
    return 1;
}

- (NSInteger)collectionView:(NSCollectionView *)cv numberOfItemsInSection:(NSInteger)section
{
    (void)cv; (void)section;
    return (NSInteger)_records.count;
}

- (NSCollectionViewItem *)collectionView:(NSCollectionView *)cv
     itemForRepresentedObjectAtIndexPath:(NSIndexPath *)indexPath
{
    NotesCardItem *item = (NotesCardItem *)[cv makeItemWithIdentifier:card_item_id
                                                         forIndexPath:indexPath];
    NSUInteger idx = indexPath.item;
    if (idx >= _records.count) return item;
    NotesNoteRecord *rec = _records[idx];

    item.title_lbl.stringValue = rec.title;

    NotesThumbnailKey *key = [[NotesThumbnailKey alloc]
        init_with_note:rec.note_id updated:rec.updated_at];
    NSImage *cached = [_thumb_cache objectForKey:key];
    if (cached) {
        item.thumb_view.image = cached;
    } else {
        item.thumb_view.image = nil;
        [self load_thumbnail_for_index:idx];
    }
    return item;
}

/*
 * load_thumbnail_for_index: — decode thumbnail on a worker and refresh
 * the card when done.
 *
 * Inputs:
 *   idx — index into self.records
 *
 * Output: none.
 */
- (void)
load_thumbnail_for_index:(NSUInteger)idx
{
    if (idx >= _records.count) return;
    NotesNoteRecord *rec = _records[idx];
    gint64  note_id    = rec.note_id;
    gint64  updated_at = rec.updated_at;
    NSString *db_path  = @(_lib.app->db->path);
    __weak typeof(self) ws = self;
    NSCache *cache = _thumb_cache;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            OnDatabase *wdb = on_db_open(db_path.UTF8String);
            NSImage *thumb = nil;
            if (wdb) {
                gsize blob_len = 0;
                guint8 *blob = on_db_note_load(wdb, note_id, &blob_len);
                if (blob && blob_len > 0) {
                    GBytes *png = on_note_image_nth_png(blob, blob_len, 0);
                    if (png) {
                        gsize n;
                        const guint8 *bytes = g_bytes_get_data(png, &n);
                        thumb = on_ui_thumbnail(bytes, n, card_w - 16, card_h - 32, 2.0);
                        g_bytes_unref(png);
                    }
                    g_free(blob);
                }
                on_db_close(wdb);
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                NotesNotesGrid *s = ws;
                if (!s || !thumb) return;
                NotesThumbnailKey *k = [[NotesThumbnailKey alloc]
                    init_with_note:note_id updated:updated_at];
                [cache setObject:thumb forKey:k];
                NSUInteger i = [s.records indexOfObjectPassingTest:^BOOL(NotesNoteRecord *r, NSUInteger j, BOOL *stop) {
                    (void)j; *stop = r.note_id == note_id; return r.note_id == note_id;
                }];
                if (i == NSNotFound) return;
                NSIndexPath *ip = [NSIndexPath indexPathForItem:(NSInteger)i inSection:0];
                NotesCardItem *item = (NotesCardItem *)[s.grid itemAtIndexPath:ip];
                if (item) item.thumb_view.image = thumb;
            });
        }
    });
}

@end

/* ── Action Items pane ───────────────────────────────────────────────────── */

/* One row in the Action Items list. */
@interface NotesActionRecord : NSObject
@property (nonatomic, assign) gint64   uid;
@property (nonatomic, assign) gint64   note_id;
@property (nonatomic, assign) gint     ord;    /* among the note's action lines */
@property (nonatomic, copy)   NSString *text;
@property (nonatomic, copy)   NSString *note_title;
@property (nonatomic, assign) gboolean done;
@property (nonatomic, assign) gint64   due;    /* 0 = no due date */
@end

@implementation NotesActionRecord
@end

static NSString * const action_cell_id = @"action";

@interface NotesActionsPane : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, weak)   NotesLibrary                  *lib;
@property (nonatomic, strong) NotesActivatingTable           *table;
@property (nonatomic, strong) NSScrollView                   *scroll;
@property (nonatomic, strong) NSArray<NotesActionRecord *>   *records;
@end

@implementation NotesActionsPane

/*
 * init_with_lib: — build the action items list.
 *
 * Inputs:
 *   lib — the library state
 *
 * Output: the actions pane controller.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    if (!(self = [super init])) return nil;
    _lib     = lib;
    _records = @[];
    [self build_table];
    return self;
}

/*
 * build_table — construct the NSTableView for action items.
 *
 * Output: none.
 */
- (void)
build_table
{
    _table = [[NotesActivatingTable alloc] initWithFrame:NSZeroRect];
    _table.dataSource       = self;
    _table.delegate         = self;
    _table.target           = self;
    _table.doubleAction     = @selector(open_activated:);
    _table.rowHeight        = action_row_h;
    _table.headerView       = nil;
    _table.allowsEmptySelection = YES;

    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"action"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView          = _table;
    _scroll.hasVerticalScroller   = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers    = YES;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
}

/*
 * reload — refresh the action items from the database.
 *
 * Output: none.
 */
- (void)
reload
{
    GList *items = on_db_action_list(_lib.app->db);
    NSMutableArray *recs = [NSMutableArray array];
    for (GList *l = items; l; l = l->next) {
        const OnActionItem *ai = l->data;
        NotesActionRecord *r = [[NotesActionRecord alloc] init];
        r.uid        = ai->uid;
        r.note_id    = ai->note_id;
        r.ord        = ai->ord;
        r.text       = @(ai->text ?: "");
        r.note_title = @(ai->context ?: "");
        r.done       = ai->done;
        r.due        = ai->due;
        [recs addObject:r];
    }
    on_db_action_list_free(items);
    _records = recs;
    [_table reloadData];
}

/* ── NSTableViewDataSource ───────────────────────────────────────────────── */

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return (NSInteger)_records.count;
}

/* ── NSTableViewDelegate ─────────────────────────────────────────────────── */

- (CGFloat)tableView:(NSTableView *)tv heightOfRow:(NSInteger)row
{
    (void)tv; (void)row;
    return action_row_h;
}

- (NSView *)tableView:(NSTableView *)tv
   viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row
{
    (void)col;
    if (row < 0 || (NSUInteger)row >= _records.count) return nil;
    NotesActionRecord *rec = _records[(NSUInteger)row];

    NSTableCellView *cell = [tv makeViewWithIdentifier:action_cell_id owner:nil];
    if (!cell) cell = [self make_action_cell];

    NSButton    *check    = (NSButton *)    [cell viewWithTag:1];
    NSTextField *text_lbl = (NSTextField *) [cell viewWithTag:2];
    NSTextField *due_lbl  = (NSTextField *) [cell viewWithTag:3];

    check.state = rec.done ? NSControlStateValueOn : NSControlStateValueOff;
    check.tag   = 1;
    /* Store uid in the button's objectValue for the toggle handler. */
    check.objectValue = @(rec.uid);

    text_lbl.stringValue = rec.text;
    if (rec.due > 0) {
        due_lbl.stringValue = format_date(rec.due);
        /* Colour-code by urgency: red if overdue, orange if due today. */
        time_t now = time(NULL);
        time_t midnight = now - (now % 86400);
        due_lbl.textColor = (rec.due < midnight)
            ? NSColor.systemRedColor
            : (rec.due < midnight + 86400 ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor);
        due_lbl.hidden = NO;
    } else {
        due_lbl.hidden = YES;
    }
    return cell;
}

/*
 * make_action_cell — build a fresh action items cell.
 *
 * Output: the new cell.
 */
- (NSTableCellView *)
make_action_cell
{
    NSTableCellView *cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cell.identifier = action_cell_id;

    NSButton *check = [NSButton checkboxWithTitle:@"" target:self action:@selector(toggle_action:)];
    check.translatesAutoresizingMaskIntoConstraints = NO;
    check.tag = 1;

    NSTextField *text = [NSTextField labelWithString:@""];
    text.translatesAutoresizingMaskIntoConstraints = NO;
    text.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    text.lineBreakMode = NSLineBreakByTruncatingTail;
    text.tag = 2;

    NSTextField *due = [NSTextField labelWithString:@""];
    due.translatesAutoresizingMaskIntoConstraints = NO;
    due.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    due.tag = 3;

    [cell addSubview:check];
    [cell addSubview:text];
    [cell addSubview:due];

    [NSLayoutConstraint activateConstraints:@[
        [check.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
        [check.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],

        [text.leadingAnchor constraintEqualToAnchor:check.trailingAnchor constant:6],
        [text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
        [text.topAnchor constraintEqualToAnchor:cell.topAnchor constant:8],

        [due.leadingAnchor constraintEqualToAnchor:text.leadingAnchor],
        [due.topAnchor constraintEqualToAnchor:text.bottomAnchor constant:2],
    ]];
    return cell;
}

/*
 * toggle_action: — checkbox toggled: flip the item's done state.
 *
 * Inputs:
 *   sender — the NSButton checkbox; objectValue is the item's uid
 *
 * Output: none.
 */
- (void)
toggle_action:(NSButton *)sender
{
    gint64 uid  = [sender.objectValue longLongValue];
    gboolean on = sender.state == NSControlStateValueOn;
    on_action_edit_set_done(_lib.app, uid, on);
    [self reload];
}

/*
 * open_activated: — the table's doubleAction (double-click, or Return /
 * Enter): open each activated item's note with the caret on the item.
 *
 * Inputs:
 *   sender — the table (unused: activated_rows reads _table)
 */
- (void)
open_activated:(id)sender
{
    (void)sender;
    [activated_rows(_table) enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL *stop) {
        (void)stop;
        if (idx >= self->_records.count) return;
        NotesActionRecord *rec = self->_records[idx];
        on_editor_window_open(self->_lib.app, rec.note_id, NotesEditorRevealAction, rec.ord, nil);
    }];
}

- (void)tableViewSelectionDidChange:(NSNotification *)note { (void)note; }

@end

/* ── Outer pane: switches between list, grid and actions ────────────────── */

@interface NotesNotesPane : NSObject
@property (nonatomic, weak)   NotesLibrary    *lib;
@property (nonatomic, strong) NSView          *view;   /* the pane's root view */
@property (nonatomic, strong) NotesNotesList  *list_ctrl;
@property (nonatomic, strong) NotesNotesGrid  *grid_ctrl;
@property (nonatomic, strong) NotesActionsPane *actions_ctrl;
@property (nonatomic, strong) NSCache          *thumb_cache;
@property (nonatomic, strong) NSArray<NotesNoteRecord *> *records;
/* Shown-state tracking for scroll/selection preservation. */
@property (nonatomic, assign) on_sb_kind_t shown_kind;
@property (nonatomic, assign) gint64       shown_id;
@end

@implementation NotesNotesPane

/*
 * init_with_lib: — build the outer pane.
 *
 * Inputs:
 *   lib — the library state
 *
 * Output: the pane.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    if (!(self = [super init])) return nil;
    _lib       = lib;
    _records   = @[];
    _shown_kind = on_sb_root;
    _shown_id   = -1;

    _thumb_cache = [[NSCache alloc] init];
    _thumb_cache.countLimit = 200;   /* bounded; ~200 decoded thumbnails     */

    _list_ctrl    = [[NotesNotesList alloc] init_with_lib:lib cache:_thumb_cache];
    _grid_ctrl    = [[NotesNotesGrid alloc] init_with_lib:lib cache:_thumb_cache];
    _actions_ctrl = [[NotesActionsPane alloc] init_with_lib:lib];

    /* The outer view fills the notes area; subviews are swapped per mode. */
    _view = [[NSView alloc] initWithFrame:NSZeroRect];
    _view.translatesAutoresizingMaskIntoConstraints = NO;
    return self;
}

/*
 * switch_to_view: — swap the visible child to list, grid, or actions.
 *
 * Inputs:
 *   mode — the desired view mode
 *
 * Output: none.
 */
- (void)
switch_to_view:(on_library_view_t)mode
{
    NSView *old = _view.subviews.firstObject;
    NSView *nv;
    switch (mode) {
    case on_library_view_list:    nv = _list_ctrl.scroll;    break;
    case on_library_view_grid:    nv = _grid_ctrl.scroll;    break;
    case on_library_view_actions: nv = _actions_ctrl.scroll; break;
    }
    if (old == nv) return;
    if (old) [old removeFromSuperview];
    [_view addSubview:nv];
    [NSLayoutConstraint activateConstraints:@[
        [nv.topAnchor    constraintEqualToAnchor:_view.topAnchor],
        [nv.bottomAnchor constraintEqualToAnchor:_view.bottomAnchor],
        [nv.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor],
        [nv.trailingAnchor constraintEqualToAnchor:_view.trailingAnchor],
    ]];
}

/*
 * refresh — reload the notes from the database and update the visible view.
 *
 * Output: none.
 */
- (void)
refresh
{
    on_library_view_t mode = on_library_view(_lib);

    if (mode == on_library_view_actions) {
        [self switch_to_view:on_library_view_actions];
        [_actions_ctrl reload];
        return;
    }

    [self switch_to_view:mode];

    BOOL keep = (_lib.sel_kind == _shown_kind && _lib.sel_id == _shown_id);
    GList *notes = on_library_notes_for_selection(_lib);
    _records = build_records(notes);          /* frees the GList */

    [_list_ctrl reload_with:_records keep:keep];
    if (mode == on_library_view_grid)
        [_grid_ctrl reload_with:_records];

    _shown_kind = _lib.sel_kind;
    _shown_id   = _lib.sel_id;
}

/*
 * refresh_note: — update just the row for one note that was saved.
 *
 * Inputs:
 *   note_id — the note that was saved
 *
 * Output: none.
 */
- (void)
refresh_note:(gint64)note_id
{
    if (on_library_view(_lib) == on_library_view_actions) {
        [_actions_ctrl reload];
        return;
    }

    /* Rebuild the single record and splice it into the array. */
    OnNoteMeta *m = on_db_note_get(_lib.app->db, note_id);
    if (!m) return;

    NSMutableArray *recs = [_records mutableCopy];
    NSUInteger idx = [recs indexOfObjectPassingTest:^BOOL(NotesNoteRecord *r, NSUInteger i, BOOL *stop) {
        (void)i; *stop = r.note_id == note_id; return r.note_id == note_id;
    }];
    if (idx == NSNotFound) {
        on_db_note_meta_free(m);
        return;
    }
    NotesNoteRecord *rec = [[NotesNoteRecord alloc] init];
    rec.note_id    = m->id;
    rec.title      = @(m->title ?: "");
    rec.modified   = format_date(m->updated_at);
    rec.updated_at = m->updated_at;
    rec.preview    = @"";
    on_db_note_meta_free(m);

    /* Invalidate the thumbnail cache entry for this note. */
    NotesThumbnailKey *key = [[NotesThumbnailKey alloc]
        init_with_note:note_id updated:_records[idx].updated_at];
    [_thumb_cache removeObjectForKey:key];

    recs[idx] = rec;
    _records  = recs;

    [_list_ctrl reload_with:_records keep:YES];
    [_list_ctrl update_note:note_id];

    if (on_library_view(_lib) == on_library_view_grid) {
        [_grid_ctrl reload_with:_records];
    }
}

/*
 * selected_ids — note ids currently selected in the active view.
 *
 * Output: array of NSNumber with note ids.
 */
- (NSArray<NSNumber *> *)
selected_ids
{
    switch (on_library_view(_lib)) {
    case on_library_view_list:    return [_list_ctrl selected_ids];
    case on_library_view_grid:    return [_grid_ctrl selected_ids];
    case on_library_view_actions: return @[];
    }
}

/*
 * release_resources — drop the thumbnail cache and all records; called
 * when the library window closes.
 *
 * Output: none.
 */
- (void)
release_resources
{
    [_thumb_cache removeAllObjects];
    _records = @[];
    _list_ctrl.records = @[];
    _grid_ctrl.records = @[];
}

@end

/* ── Commands ───────────────────────────────────────────────────────────── */

/*
 * run_view_list — switch the notes pane to list mode.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 *
 * Output: none.
 */
static void
run_view_list(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    lib.grid_pref = NO;
    on_library_notes_refresh(lib);
}

/*
 * run_view_grid — switch the notes pane to grid mode.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 *
 * Output: none.
 */
static void
run_view_grid(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    lib.grid_pref = YES;
    on_library_notes_refresh(lib);
}

/*
 * run_toggle_view — toggle between list and grid mode.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the toolbar item
 *
 * Output: none.
 */
static void
run_toggle_view(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    lib.grid_pref = !lib.grid_pref;
    on_library_notes_refresh(lib);
}

/*
 * validate_toggle_view — update the toolbar item's icon to show list or grid.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the toolbar item
 *
 * Output: YES (always enabled).
 */
static BOOL
validate_toggle_view(id target, id item)
{
    NotesLibrary *lib = target;
    NSString *icon = lib.grid_pref ? @"list" : @"grid";
    on_command_item_update(item, nil, icon, NO);
    return YES;
}

/*
 * run_note_open — open the selected note(s) in the editor.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 *
 * Output: none.
 */
static void
run_note_open(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    for (NSNumber *n in ids) {
        if (lib.app->open_note)
            lib.app->open_note(lib.app, n.longLongValue);
    }
}

/*
 * validate_note_open — enabled when at least one note is selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the menu item
 *
 * Output: YES when a note is selected.
 */
static BOOL
validate_note_open(id target, id item)
{
    (void)item;
    return on_library_notes_selected_ids(target).count > 0;
}

/*
 * run_delete_note — move selected notes to Trash.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 *
 * Output: none.
 */
static void
run_delete_note(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    if (!ids.count) return;

    NSUInteger n = ids.count;
    gint64 *buf = malloc(sizeof(gint64) * n);
    for (NSUInteger i = 0; i < n; i++)
        buf[i] = ids[i].longLongValue;
    on_db_notes_trash(lib.app->db, buf, n);
    free(buf);
    on_library_refresh_all(lib);
}

/*
 * validate_delete_note — enabled when notes are selected and not in the Trash view.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the menu item
 *
 * Output: YES when deletable notes are selected.
 */
static BOOL
validate_delete_note(id target, id item)
{
    (void)item;
    NotesLibrary *lib = target;
    if (on_library_in_trash_view(lib)) return NO;
    return on_library_notes_selected_ids(lib).count > 0;
}

/*
 * run_note_restore — restore selected note(s) from Trash.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 *
 * Output: none.
 */
static void
run_note_restore(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    for (NSNumber *n in on_library_notes_selected_ids(lib))
        on_db_note_restore(lib.app->db, n.longLongValue);
    on_library_refresh_all(lib);
}

/*
 * validate_note_restore — enabled only in a Trash view with selection.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the menu item
 *
 * Output: YES in a Trash view with selection.
 */
static BOOL
validate_note_restore(id target, id item)
{
    (void)item;
    NotesLibrary *lib = target;
    return on_library_in_trash_view(lib) && on_library_notes_selected_ids(lib).count > 0;
}

/*
 * run_note_pin — toggle the pin state of the selected note.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the menu item
 *
 * Output: none.
 */
static void
run_note_pin(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    if (!ids.count) return;
    gint64 nid = ids.firstObject.longLongValue;
    OnNoteMeta *m = on_db_note_get(lib.app->db, nid);
    if (!m) return;
    gboolean was = m->pinned;
    on_db_note_meta_free(m);
    on_db_note_set_pinned(lib.app->db, nid, !was);
    on_library_refresh_all(lib);
}

static BOOL
validate_note_pin(id target, id item)
{
    (void)item;
    return on_library_notes_selected_ids(target).count > 0;
}

/* The handler table for commands this module owns. */
static const on_command_handler_t notes_handlers[] = {
    { @"view-list",    run_view_list,    NULL               },
    { @"view-grid",    run_view_grid,    NULL               },
    { @"toggle-view",  run_toggle_view,  validate_toggle_view },
    { @"note-open",    run_note_open,    validate_note_open   },
    { @"delete-note",  run_delete_note,  validate_delete_note },
    { @"note-restore", run_note_restore, validate_note_restore },
    { @"note-pin",     run_note_pin,     validate_note_pin    },
};

/* ── File-static pane instance ───────────────────────────────────────────── */

/* One pane per library window; the library window is a singleton, so this
 * is a singleton too. */
static NotesNotesPane *notes_pane;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * on_library_notes_install — see library_notes.h.
 */
void
on_library_notes_install(NotesLibrary *lib)
{
    on_library_add_commands(notes_handlers,
                            sizeof notes_handlers / sizeof notes_handlers[0]);
    (void)lib;
}

/*
 * on_library_notes_new — see library_notes.h.
 */
NSView *
on_library_notes_new(NotesLibrary *lib)
{
    notes_pane = [[NotesNotesPane alloc] init_with_lib:lib];
    [notes_pane refresh];
    return notes_pane.view;
}

/*
 * on_library_notes_refresh — see library_notes.h.
 */
void
on_library_notes_refresh(NotesLibrary *lib)
{
    (void)lib;
    if (!notes_pane) return;
    [notes_pane refresh];
}

/*
 * on_library_notes_note_saved — see library_notes.h.
 */
void
on_library_notes_note_saved(NotesLibrary *lib, gint64 note_id)
{
    (void)lib;
    if (!notes_pane) return;
    [notes_pane refresh_note:note_id];
}

/*
 * on_library_notes_selected_ids — see library_notes.h.
 */
NSArray<NSNumber *> *
on_library_notes_selected_ids(NotesLibrary *lib)
{
    (void)lib;
    if (!notes_pane) return @[];
    return [notes_pane selected_ids];
}
