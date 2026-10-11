/*
 * library_notes.m — the library's notes pane (work package G): the notes
 * of the current selection as a list or a grid, and the Action Items list
 * that replaces them while that sidebar row is selected.
 *
 * Specification: porting/gtk_reference/src/library_window.c —
 * refresh_notes, render_note_thumb and the thumbnail fill, the column
 * layout, the per-note menu, refresh_actions and the due-date dialog.
 *
 *   List          Title, Path, Modified, Created; sortable headers (the
 *                 sort drives the grid too, Modified newest first by
 *                 default); headers reorder; a header menu shows and hides
 *                 columns; the layout persists in "list_columns".  Compact
 *                 or Comfortable density (bold title, a dimmed preview).
 *   Grid          square cards rendered as GTK renders them — a card, the
 *                 first image, the body text — on the main thread in short
 *                 slices, only while the grid shows; the bold title under
 *                 each card.
 *   Selection     ONE selection by note id, shared by both views, kept
 *                 across content refreshes; a navigation starts empty.
 *   Action Items  Done, Context, Action, Due Date; sortable, reorderable,
 *                 persisted in "action_columns"; done items struck through
 *                 (or hidden: show_done_actions); due dates tinted by
 *                 local day; a double-click on a due date sets it in a tool
 *                 panel, anywhere else opens the note at the item.
 *
 * Both views open a note on a double-click or Return (activation.h), drag
 * notes (on_library_drag_item) and show the per-note menu on a right click.
 */

#import "library_notes.h"

#import "activation.h"
#import "commands.h"
#import "editor_window.h"
#import "library_commands.h"
#import "library_sidebar.h"
#import "library_window.h"
#import "look.h"
#import "tool_panel.h"
#import "ui_util.h"

#include "action_edit.h"
#include "app_core.h"
#include "config.h"
#include "db.h"
#include "image_io.h"
#include "serialize.h"

#include <glib.h>

/* ── Sizes and limits ───────────────────────────────────────────────────── */

/* Body text the Comfortable preview line is cut from (GTK NL_PREVIEW_CHARS). */
static const gint preview_chars = 200;

/* Body text a grid card shows at most. */
static const gint card_text_chars = 300;

/* A card's first image is decoded at most this many pixels on a side. */
static const int card_decode_px = 512;

/* How long one thumbnail slice may run before yielding (seconds). */
static const double thumb_slice_s = 0.040;

/* A card's padding around the thumbnail, and its title's height (2 lines). */
static const CGFloat card_pad     = 6.0;
static const CGFloat card_title_h = 34.0;

/* ── Records ────────────────────────────────────────────────────────────── */

/* One note of the list and grid. */
@interface NotesNoteRecord : NSObject
@property (nonatomic, assign) gint64    note_id;
@property (nonatomic, copy)   NSString *title;
@property (nonatomic, copy)   NSString *path;       /* "/Folder/Sub", "/" at the top */
@property (nonatomic, copy)   NSString *modified;
@property (nonatomic, copy)   NSString *created;
@property (nonatomic, copy)   NSString *preview;    /* nil = none */
@property (nonatomic, assign) gint64    updated_at;
@property (nonatomic, assign) gint64    created_at;
@end

@implementation NotesNoteRecord
@end

/* One row of Action Items. */
@interface NotesActionRecord : NSObject
@property (nonatomic, assign) gint64    uid;
@property (nonatomic, assign) gint64    note_id;
@property (nonatomic, assign) gint      ord;
@property (nonatomic, copy)   NSString *text;
@property (nonatomic, copy)   NSString *context;
@property (nonatomic, assign) BOOL      done;
@property (nonatomic, assign) gint64    due;        /* local midnight; 0 = none */
@end

@implementation NotesActionRecord
@end

/* A rendered grid card, for the note's modification time and appearance. */
@interface NotesCardEntry : NSObject
@property (nonatomic, assign) gint64   updated_at;
@property (nonatomic, assign) BOOL     dark;
@property (nonatomic, strong) NSImage *image;
@end

@implementation NotesCardEntry
@end

/* ── Text helpers ──────────────────────────────────────────────────────── */

/*
 * list_time_text — a time as the list shows it, "Jun 3, 2026 14:05"
 * (GTK LIST_TIME_FORMAT).  One shared formatter.
 *
 * Inputs:
 *   t — UNIX time
 *
 * Output: the text.
 */
static NSString *
list_time_text(gint64 t)
{
    static NSDateFormatter *f;
    if (!f) {
        f = [[NSDateFormatter alloc] init];
        f.dateFormat = @"MMM d, yyyy HH:mm";
    }
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:(double)t]];
}

/*
 * due_text — a due date as Action Items shows it, "Jun 3, 2026".
 *
 * Inputs:
 *   due — local-midnight UNIX time; 0 = none
 *
 * Output: the text; empty for none.
 */
static NSString *
due_text(gint64 due)
{
    static NSDateFormatter *f;
    if (!f) {
        f = [[NSDateFormatter alloc] init];
        f.dateFormat = @"MMM d, yyyy";
    }
    return due ? [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:(double)due]] : @"";
}

/*
 * due_colour — a due date's tint by local calendar day: passed red, today
 * yellow, ahead green (GTK on_action_due_bind).
 *
 * Inputs:
 *   due — local-midnight UNIX time (not 0)
 *
 * Output: the dynamic colour.
 */
static NSColor *
due_colour(gint64 due)
{
    NSCalendar *cal   = NSCalendar.currentCalendar;
    NSDate     *today = [cal startOfDayForDate:NSDate.date];
    NSDate     *day   = [cal startOfDayForDate:[NSDate dateWithTimeIntervalSince1970:(double)due]];
    NSComparisonResult order = [day compare:today];
    return on_ui_color(order == NSOrderedAscending ? on_look_due_overdue
                     : order == NSOrderedSame      ? on_look_due_today : on_look_due_ahead);
}

/*
 * preview_line — the Comfortable preview: the first non-blank line after
 * the title line of a note's body text (GTK notes_preview_line).
 *
 * Inputs:
 *   body — the cached body text, or NULL
 *
 * Output: the line, or nil.
 */
static NSString *
preview_line(const gchar *body)
{
    const gchar *pos = body ? strchr(body, '\n') : NULL;
    while (pos != NULL && *pos != '\0') {
        pos++;
        const gchar *eol  = strchr(pos, '\n');
        gchar       *line = eol ? g_strndup(pos, (gsize)(eol - pos)) : g_strdup(pos);
        g_strstrip(line);
        NSString *found = *line ? @(line) : nil;
        g_free(line);
        if (found) return found;
        pos = eol;
    }
    return nil;
}

/*
 * casecmp — case-insensitive comparison, nil as empty.
 *
 * Inputs:
 *   a, b — the strings
 *
 * Output: the order.
 */
static NSComparisonResult
casecmp(NSString *a, NSString *b)
{
    return [a ?: @"" localizedCaseInsensitiveCompare:b ?: @""];
}

/*
 * note_order — the list's (and grid's) order for a column key.  The time
 * columns are inverted so that ascending — the first click — shows the
 * newest first (GTK cmp_updated); equal paths fall back to the title.
 *
 * Inputs:
 *   key — "title", "path", "modified" or "created"
 *   a,b — the records
 *
 * Output: the order.
 */
static NSComparisonResult
note_order(NSString *key, id ra, id rb)
{
    NotesNoteRecord *a = ra, *b = rb;
    if ([key isEqualToString:@"title"]) return casecmp(a.title, b.title);
    if ([key isEqualToString:@"path"]) {
        NSComparisonResult r = casecmp(a.path, b.path);
        return r != NSOrderedSame ? r : casecmp(a.title, b.title);
    }
    gint64 ta = [key isEqualToString:@"created"] ? a.created_at : a.updated_at;
    gint64 tb = [key isEqualToString:@"created"] ? b.created_at : b.updated_at;
    return tb > ta ? NSOrderedDescending : tb < ta ? NSOrderedAscending : NSOrderedSame;
}

/*
 * action_order — Action Items' order for a column key: open items first;
 * context and text alphabetically; the soonest due date first, undated
 * last (GTK cmp_action_*).
 *
 * Inputs:
 *   key — "done", "context", "action" or "due"
 *   a,b — the records
 *
 * Output: the order.
 */
static NSComparisonResult
action_order(NSString *key, id ra, id rb)
{
    NotesActionRecord *a = ra, *b = rb;
    if ([key isEqualToString:@"done"])
        return a.done == b.done ? NSOrderedSame : a.done ? NSOrderedDescending : NSOrderedAscending;
    if ([key isEqualToString:@"context"]) return casecmp(a.context, b.context);
    if ([key isEqualToString:@"action"])  return casecmp(a.text, b.text);
    gint64 da = a.due ? a.due : G_MAXINT64, db = b.due ? b.due : G_MAXINT64;
    return da < db ? NSOrderedAscending : da > db ? NSOrderedDescending : NSOrderedSame;
}

/*
 * sorted_by — records in a sort's order; a stable sort, so equal keys keep
 * the listing's order.
 *
 * Inputs:
 *   records — the records
 *   sd      — the sort (its key picks the column)
 *   order   — the column order (note_order or action_order)
 *
 * Output: the sorted records.
 */
static NSArray *
sorted_by(NSArray *records, NSSortDescriptor *sd, NSComparisonResult (*order)(NSString *, id, id))
{
    return [records sortedArrayWithOptions:NSSortStable usingComparator:^NSComparisonResult(id a, id b) {
        NSComparisonResult r = order(sd.key, a, b);
        return sd.ascending ? r : (NSComparisonResult)-r;
    }];
}

/* ── Column layout (shared by the list and Action Items) ─────────────────── */

/* One column of a table: its stable key, header, header-menu label, the
 * width it starts at, and whether it takes the leftover width. */
typedef struct {
    NSString * __unsafe_unretained key;
    NSString * __unsafe_unretained title;
    NSString * __unsafe_unretained menu_label;
    CGFloat                        width;
    BOOL                           expands;
} column_spec_t;

static const column_spec_t note_columns[] = {
    { @"title",    @"Title",    @"Title",    240, YES },
    { @"path",     @"Path",     @"Path",     140, NO  },
    { @"modified", @"Modified", @"Modified", 140, NO  },
    { @"created",  @"Created",  @"Created",  140, NO  },
};

static const column_spec_t action_columns[] = {
    { @"done",    @"",         @"Done",     24,  NO  },
    { @"context", @"Context",  @"Context",  120, NO  },
    { @"action",  @"Action",   @"Action",   260, YES },
    { @"due",     @"Due Date", @"Due Date", 110, NO  },
};

/*
 * add_columns — give a table its columns, each sortable by its key, the
 * headers reorderable, the rows flat and full width (as Files has them).
 *
 * Inputs:
 *   table — the table
 *   specs — the columns
 *   n     — how many
 */
static void
add_columns(NSTableView *table, const column_spec_t *specs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:specs[i].key];
        col.title        = specs[i].title;
        col.width        = specs[i].width;
        col.minWidth     = MIN(specs[i].width, 24);
        col.resizingMask = specs[i].expands ? NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask
                                            : NSTableColumnUserResizingMask;
        col.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:specs[i].key ascending:YES];
        [table addTableColumn:col];
    }
    table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    table.allowsColumnReordering  = YES;
    table.style                   = NSTableViewStylePlain;
}

/*
 * apply_layout — put back a table's saved column order and visibility
 * ("key:1,key:0,…" in display order), the default when none is saved;
 * unknown keys are skipped and at least one column stays visible (GTK
 * view_columns_install).
 *
 * Inputs:
 *   table    — the table
 *   cfg_key  — its ini key ("list_columns" / "action_columns")
 *   fallback — the default layout
 */
static void
apply_layout(NSTableView *table, const char *cfg_key, const char *fallback)
{
    gchar    *cfg    = on_app_config_get(cfg_key);
    NSString *layout = cfg && *cfg ? @(cfg) : @(fallback);
    g_free(cfg);
    NSInteger at = 0;
    for (NSString *entry in [layout componentsSeparatedByString:@","]) {
        NSArray<NSString *> *kv = [entry componentsSeparatedByString:@":"];
        NSInteger from = [table columnWithIdentifier:kv.firstObject];
        if (from < at) continue;          /* unknown, or listed twice */
        [table moveColumn:from toColumn:at];
        table.tableColumns[(NSUInteger)at].hidden = kv.count > 1 && [kv[1] isEqualToString:@"0"];
        at++;
    }
    BOOL any = NO;
    for (NSTableColumn *c in table.tableColumns) any |= !c.hidden;
    if (!any) table.tableColumns.firstObject.hidden = NO;
}

/*
 * persist_layout — write a table's column order and visibility to its ini
 * key.
 *
 * Inputs:
 *   table   — the table
 *   cfg_key — its ini key
 */
static void
persist_layout(NSTableView *table, const char *cfg_key)
{
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (NSTableColumn *c in table.tableColumns)
        [parts addObject:[NSString stringWithFormat:@"%@:%d", c.identifier, c.hidden ? 0 : 1]];
    on_app_config_set(cfg_key, [parts componentsJoinedByString:@","].UTF8String);
}

/* ── Grid cards ─────────────────────────────────────────────────────────── */

/*
 * card_body — what a card shows under its image: the note's text after
 * its title line, blank lines dropped at the start, at most
 * card_text_chars characters (GTK render_note_thumb).
 *
 * Inputs:
 *   doc — the note
 *
 * Output: the text.
 */
static NSString *
card_body(const OnDocument *doc)
{
    gchar       *text = on_document_plain_text(doc);
    const gchar *body = text;
    while (*body == '\n') body++;
    const gchar *nl = strchr(body, '\n');
    body = nl ? nl + 1 : "";
    while (*body == '\n') body++;
    const gchar *p = body;
    for (gint n = 0; *p && n < card_text_chars; n++) p = g_utf8_next_char(p);
    NSString *cut = [[NSString alloc] initWithBytes:body length:(NSUInteger)(p - body)
                                           encoding:NSUTF8StringEncoding] ?: @"";
    g_free(text);
    return cut;
}

/*
 * render_card — a note's grid card: a square on_look_thumb_size card in
 * the card colour with a 1 pt border, its first image (decoded capped at
 * card_decode_px) fitted into the top, never enlarged, then its body text,
 * small and grey, clipped to the card (GTK render_note_thumb).  Rendered
 * once into a bitmap at the screen's scale.
 *
 * Inputs:
 *   db      — the database (main thread)
 *   note_id — the note
 *   dark    — render for a dark appearance
 *   scale   — the backing scale factor
 *
 * Output: the image.
 */
static NSImage *
render_card(OnDatabase *db, gint64 note_id, BOOL dark, CGFloat scale)
{
    const CGFloat sz  = on_look_thumb_size;
    OnDocument   *doc = on_note_document_load(db, note_id);
    CGImageRef    img = NULL;
    GBytes       *png = on_document_image_nth(doc, 0, NULL);
    if (png) {
        gsize n;
        const guint8 *bytes = g_bytes_get_data(png, &n);
        img = on_image_decode_capped(bytes, n, card_decode_px);
    }
    NSString *body = card_body(doc);
    on_document_free(doc);

    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)(sz * scale) pixelsHigh:(NSInteger)(sz * scale)
                   bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    rep.size = NSMakeSize(sz, sz);
    [NSGraphicsContext saveGraphicsState];
    CGContextRef ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep].CGContext;
    CGContextTranslateCTM(ctx, 0, sz);            /* y down, as the GTK drawing */
    CGContextScaleCTM(ctx, 1, -1);
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];

    on_color_t bg = on_look_pick(on_look_card, dark), bd = on_look_pick(on_look_card_border, dark);
    CGContextSetRGBFillColor(ctx, bg.r, bg.g, bg.b, bg.a);
    CGContextFillRect(ctx, CGRectMake(0, 0, sz, sz));
    CGContextSetRGBStrokeColor(ctx, bd.r, bd.g, bd.b, bd.a);
    CGContextSetLineWidth(ctx, 1);
    CGContextStrokeRect(ctx, CGRectMake(0.5, 0.5, sz - 1, sz - 1));

    CGFloat y = card_pad;
    if (img) {
        CGFloat iw = CGImageGetWidth(img), ih = CGImageGetHeight(img);
        CGFloat k  = MIN(MIN(on_look_thumb_image_w / iw, on_look_thumb_image_h / ih), 1.0);
        CGFloat dw = iw * k, dh = ih * k;
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, (sz - dw) / 2.0, y + dh);
        CGContextScaleCTM(ctx, 1, -1);                /* images draw y-up */
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextDrawImage(ctx, CGRectMake(0, 0, dw, dh), img);
        CGContextRestoreGState(ctx);
        CGImageRelease(img);
        y += dh + 4;
    }

    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.lineBreakMode = NSLineBreakByWordWrapping;
    on_color_t tc  = on_look_pick(on_look_card_text, dark);
    NSRect     box = NSMakeRect(card_pad, y, sz - 2 * card_pad, sz - y - card_pad);
    CGContextClipToRect(ctx, box);
    [body drawWithRect:box options:NSStringDrawingUsesLineFragmentOrigin
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:on_look_thumb_text_font],
                          NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:tc.r green:tc.g
                                                                               blue:tc.b alpha:tc.a],
                          NSParagraphStyleAttributeName: para }];
    [NSGraphicsContext restoreGraphicsState];

    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(sz, sz)];
    [image addRepresentation:rep];
    return image;
}

static void cards_appearance_changed(void);

/*
 * NotesCardView — a grid card: the thumbnail and, under it, the bold
 * title; a selected card sits on the selection colour.
 */
@interface NotesCardView : NSView
@property (nonatomic, strong) NSImageView *thumb;
@property (nonatomic, strong) NSTextField *title;
@property (nonatomic, assign) BOOL         selected;
@end

@implementation NotesCardView

/*
 * initWithFrame: — the card's two parts.
 *
 * Inputs:
 *   frame — the item's frame
 *
 * Output: the view.
 */
- (instancetype)
initWithFrame:(NSRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    CGFloat sz = on_look_thumb_size;
    _thumb = on_ui_image_view(nil, sz, sz);
    _thumb.translatesAutoresizingMaskIntoConstraints = NO;
    _title = [NSTextField wrappingLabelWithString:@""];
    _title.font                 = [NSFont boldSystemFontOfSize:on_look_cell_font];
    _title.alignment            = NSTextAlignmentCenter;
    _title.maximumNumberOfLines = 2;
    _title.lineBreakMode        = NSLineBreakByTruncatingTail;
    _title.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_thumb];
    [self addSubview:_title];
    [NSLayoutConstraint activateConstraints:@[
        [_thumb.topAnchor     constraintEqualToAnchor:self.topAnchor constant:card_pad],
        [_thumb.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [_thumb.widthAnchor   constraintEqualToConstant:sz],
        [_thumb.heightAnchor  constraintEqualToConstant:sz],
        [_title.topAnchor     constraintEqualToAnchor:_thumb.bottomAnchor constant:4],
        [_title.widthAnchor   constraintEqualToConstant:sz],
        [_title.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
    ]];
    return self;
}

/*
 * drawRect: — the selection behind a selected card.
 *
 * Inputs:
 *   dirty — the area to draw (unused)
 */
- (void)
drawRect:(NSRect)dirty
{
    if (!self.selected) return;
    [NSColor.selectedContentBackgroundColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:6 yRadius:6] fill];
}

/*
 * viewDidChangeEffectiveAppearance — light and dark cards differ: the
 * grid re-renders its cards for the new appearance.
 */
- (void)
viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    cards_appearance_changed();
}

/*
 * setSelected: — show or stop showing the card selected.
 *
 * Inputs:
 *   selected — YES when selected
 */
- (void)
setSelected:(BOOL)selected
{
    _selected = selected;
    self.title.textColor = selected ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor;
    self.needsDisplay = YES;
}

@end

/*
 * NotesCardItem — the grid item of one note.
 */
@interface NotesCardItem : NSCollectionViewItem
@end

@implementation NotesCardItem

/*
 * loadView — the card view.
 */
- (void)
loadView
{
    self.view = [[NotesCardView alloc] initWithFrame:NSZeroRect];
}

/*
 * setSelected: — the card follows the item's selection.
 *
 * Inputs:
 *   selected — YES when selected
 */
- (void)
setSelected:(BOOL)selected
{
    [super setSelected:selected];
    ((NotesCardView *)self.view).selected = selected;
}

@end

/* ── The pane ───────────────────────────────────────────────────────────── */

@interface NotesNotesPane : NSObject <NSTableViewDataSource, NSTableViewDelegate,
                                      NSCollectionViewDataSource, NSCollectionViewDelegate,
                                      NSMenuDelegate>
@property (nonatomic, weak)   NotesLibrary         *lib;
@property (nonatomic, strong) NSView               *view;          /* the pane */
@property (nonatomic, strong) NotesActivatingTable *list;
@property (nonatomic, strong) NotesActivatingGrid  *grid;
@property (nonatomic, strong) NotesActivatingTable *actions;
@property (nonatomic, strong) NSScrollView         *list_scroll;
@property (nonatomic, strong) NSScrollView         *grid_scroll;
@property (nonatomic, strong) NSScrollView         *actions_scroll;
@property (nonatomic, strong) NSArray<NotesNoteRecord *>   *records;
@property (nonatomic, strong) NSArray<NotesActionRecord *> *action_records;
@property (nonatomic, strong) NSMutableOrderedSet<NSNumber *> *selected;   /* note ids */
@property (nonatomic, assign) on_sb_kind_t  shown_kind;
@property (nonatomic, assign) gint64        shown_id;
@property (nonatomic, assign) BOOL          shown_any;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NotesCardEntry *> *cards;
@property (nonatomic, strong) NSMutableOrderedSet<NSNumber *> *card_queue;
@property (nonatomic, assign) BOOL          card_fill_scheduled;
@end

/* The pane of the (one) library window. */
static NotesNotesPane *notes_pane;

/*
 * scroll_for — a view in a scroll view that fills the pane.
 *
 * Inputs:
 *   doc — the view
 *
 * Output: the scroll view.
 */
static NSScrollView *
scroll_for(NSView *doc)
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = doc;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    return scroll;
}

/*
 * label_cell — a reusable text cell of a table.
 *
 * Inputs:
 *   table — the table
 *   ident — the cell's reuse identifier
 *
 * Output: the cell (its textField set).
 */
static NSTableCellView *
label_cell(NSTableView *table, NSString *ident)
{
    NSTableCellView *cell = [table makeViewWithIdentifier:ident owner:nil];
    if (cell) return cell;
    cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cell.identifier = ident;
    NSTextField *label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.font = [NSFont systemFontOfSize:on_look_cell_font];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [cell addSubview:label];
    cell.textField = label;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:4],
        [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
        [label.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

/*
 * struck — text in the cell font, struck through when an item is done.
 *
 * Inputs:
 *   text   — the text
 *   done   — YES to strike it
 *   colour — its colour, or nil for the label colour
 *
 * Output: the attributed text.
 */
static NSAttributedString *
struck(NSString *text, BOOL done, NSColor *colour)
{
    NSMutableDictionary *attrs = [NSMutableDictionary dictionaryWithDictionary:@{
        NSFontAttributeName:            [NSFont systemFontOfSize:on_look_cell_font],
        NSForegroundColorAttributeName: colour ?: NSColor.labelColor,
    }];
    if (done) attrs[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    return [[NSAttributedString alloc] initWithString:text ?: @"" attributes:attrs];
}

@implementation NotesNotesPane

/* ── Building ──────────────────────────────────────────────────────────── */

/*
 * init_with_lib: — the three views and the pane holding the visible one.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the pane.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    if (!(self = [super init])) return nil;
    _lib            = lib;
    _records        = @[];
    _action_records = @[];
    _selected       = [NSMutableOrderedSet orderedSet];
    _cards          = [NSMutableDictionary dictionary];
    _card_queue     = [NSMutableOrderedSet orderedSet];

    _list = [[NotesActivatingTable alloc] initWithFrame:NSZeroRect];
    add_columns(_list, note_columns, sizeof note_columns / sizeof *note_columns);
    apply_layout(_list, "list_columns", "path:0,title:1,modified:1,created:0");
    _list.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"modified" ascending:YES]];
    _list.allowsMultipleSelection = YES;
    _list.usesAlternatingRowBackgroundColors = YES;
    _list.target       = self;
    _list.doubleAction = @selector(list_activated:);
    _list.menu          = [[NSMenu alloc] initWithTitle:@""];
    _list.menu.delegate = self;
    _list.headerView.menu          = [[NSMenu alloc] initWithTitle:@""];
    _list.headerView.menu.delegate = self;
    [_list setDraggingSourceOperationMask:NSDragOperationMove forLocal:YES];
    _list.dataSource = self;
    _list.delegate   = self;
    _list_scroll = scroll_for(_list);

    NSCollectionViewFlowLayout *layout = [[NSCollectionViewFlowLayout alloc] init];
    layout.itemSize = NSMakeSize(on_look_thumb_size + 2 * card_pad,
                                 on_look_thumb_size + 2 * card_pad + 4 + card_title_h);
    layout.minimumInteritemSpacing = card_pad;
    layout.minimumLineSpacing      = card_pad;
    layout.sectionInset = NSEdgeInsetsMake(2 * card_pad, 2 * card_pad, 2 * card_pad, 2 * card_pad);
    _grid = [[NotesActivatingGrid alloc] initWithFrame:NSZeroRect];
    _grid.collectionViewLayout    = layout;
    _grid.selectable              = YES;
    _grid.allowsMultipleSelection = YES;
    _grid.backgroundColors        = @[NSColor.controlBackgroundColor];
    [_grid registerClass:NotesCardItem.class forItemWithIdentifier:@"card"];
    [_grid setDraggingSourceOperationMask:NSDragOperationMove forLocal:YES];
    __weak NotesNotesPane *ws = self;
    _grid.activate_handler = ^(NSSet<NSIndexPath *> *paths) {
        for (NSIndexPath *ip in paths) [ws open_record:(NSUInteger)ip.item];
    };
    _grid.menu_handler = ^NSMenu *(NSIndexPath *path) {
        return [ws grid_menu:path];
    };
    _grid.dataSource = self;
    _grid.delegate   = self;
    _grid_scroll = scroll_for(_grid);

    _actions = [[NotesActivatingTable alloc] initWithFrame:NSZeroRect];
    add_columns(_actions, action_columns, sizeof action_columns / sizeof *action_columns);
    apply_layout(_actions, "action_columns", "done:1,context:1,action:1,due:1");
    _actions.usesAlternatingRowBackgroundColors = YES;
    _actions.target       = self;
    _actions.doubleAction = @selector(actions_activated:);
    _actions.headerView.menu          = [[NSMenu alloc] initWithTitle:@""];
    _actions.headerView.menu.delegate = self;
    _actions.dataSource = self;
    _actions.delegate   = self;
    _actions_scroll = scroll_for(_actions);

    _view = [[NSView alloc] initWithFrame:NSZeroRect];
    _view.translatesAutoresizingMaskIntoConstraints = NO;
    return self;
}

/*
 * show_view: — put one of the three views in the pane.
 *
 * Inputs:
 *   scroll — the view's scroll view
 */
- (void)
show_view:(NSScrollView *)scroll
{
    NSView *old = self.view.subviews.firstObject;
    if (old == scroll) return;
    [old removeFromSuperview];
    [self.view addSubview:scroll];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor      constraintEqualToAnchor:self.view.topAnchor],
        [scroll.bottomAnchor   constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor  constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    ]];
}

/* ── Populating ────────────────────────────────────────────────────────── */

/*
 * sort_records — order the notes by the list's sort, which the grid
 * shares (Modified, newest first, when there is none).
 */
- (void)
sort_records
{
    NSSortDescriptor *sd = self.list.sortDescriptors.firstObject
                        ?: [NSSortDescriptor sortDescriptorWithKey:@"modified" ascending:YES];
    self.records = sorted_by(self.records, sd, note_order);
}

/*
 * sort_actions — order Action Items by its sort; with none, the listing's
 * own order stays.
 */
- (void)
sort_actions
{
    NSSortDescriptor *sd = self.actions.sortDescriptors.firstObject;
    if (sd != nil)
        self.action_records = sorted_by(self.action_records, sd, action_order);
}

/*
 * load_records: — the notes of the selection, as rows: paths from one
 * folder-path query, preview lines from one text query (only in the
 * Comfortable list).
 *
 * Inputs:
 *   previews — YES to fetch the preview lines
 */
- (void)
load_records:(BOOL)previews
{
    OnDatabase *db    = self.lib.app->db;
    GList      *notes = on_library_notes_for_selection(self.lib);
    GHashTable *paths = on_db_folder_path_map(db);
    GHashTable *texts = previews ? on_db_note_text_map(db, preview_chars) : NULL;
    NSMutableArray<NotesNoteRecord *> *records = [NSMutableArray array];
    for (GList *l = notes; l != NULL; l = l->next) {
        const OnNoteMeta *m = l->data;
        NotesNoteRecord  *r = [[NotesNoteRecord alloc] init];
        r.note_id    = m->id;
        r.title      = @(m->title ?: "");
        r.updated_at = m->updated_at;
        r.created_at = m->created_at;
        r.modified   = list_time_text(m->updated_at);
        r.created    = list_time_text(m->created_at);
        const gchar *path = m->folder_id ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
        r.path    = [@"/" stringByAppendingString:path ? @(path) : @""];
        r.preview = texts ? preview_line(g_hash_table_lookup(texts, &m->id)) : nil;
        [records addObject:r];
    }
    on_db_note_list_free(notes);
    g_hash_table_destroy(paths);
    if (texts) g_hash_table_destroy(texts);
    self.records = records;
    [self sort_records];
}

/*
 * load_actions — every action item (completed ones only while they are
 * shown), as rows.
 */
- (void)
load_actions
{
    NSMutableArray<NotesActionRecord *> *rows = [NSMutableArray array];
    GList *items = on_db_action_list(self.lib.app->db);
    for (GList *l = items; l != NULL; l = l->next) {
        const OnActionItem *it = l->data;
        if (it->done && !self.lib.app->show_done_actions) continue;
        NotesActionRecord *r = [[NotesActionRecord alloc] init];
        r.uid     = it->uid;
        r.note_id = it->note_id;
        r.ord     = it->ord;
        r.text    = @(it->text ?: "");
        r.context = @(it->context ?: "");
        r.done    = it->done;
        r.due     = it->due;
        [rows addObject:r];
    }
    on_db_action_list_free(items);
    self.action_records = rows;
    [self sort_actions];
}

/*
 * index_of_note: — where a note is in the records.
 *
 * Inputs:
 *   note_id — the note
 *
 * Output: its index, or NSNotFound.
 */
- (NSUInteger)
index_of_note:(gint64)note_id
{
    return [self.records indexOfObjectPassingTest:^BOOL(NotesNoteRecord *r, NSUInteger i, BOOL *stop) {
        return r.note_id == note_id;
    }];
}

/*
 * show_selection — select, in both views, the notes of `selected` that
 * are listed; the others leave the selection.
 */
- (void)
show_selection
{
    NSMutableIndexSet   *rows  = [NSMutableIndexSet indexSet];
    NSMutableSet        *paths = [NSMutableSet set];
    NSMutableOrderedSet *kept  = [NSMutableOrderedSet orderedSet];
    for (NSNumber *nid in self.selected) {
        NSUInteger i = [self index_of_note:nid.longLongValue];
        if (i == NSNotFound) continue;
        [kept addObject:nid];
        [rows addIndex:i];
        [paths addObject:[NSIndexPath indexPathForItem:(NSInteger)i inSection:0]];
    }
    self.selected = kept;
    self.lib.populating++;
    [self.list selectRowIndexes:rows byExtendingSelection:NO];
    self.grid.selectionIndexPaths = paths;
    self.lib.populating--;
}

/*
 * reload_notes — both views from the records, the selection put back.
 */
- (void)
reload_notes
{
    self.lib.populating++;
    [self.list reloadData];
    [self.grid reloadData];
    self.lib.populating--;
    [self show_selection];
}

/*
 * refresh — repopulate the pane for the library's selection (GTK
 * refresh_notes): Action Items swaps in its own view; otherwise the list
 * or grid.  The same selection as last time is a content refresh — the
 * scroll position and the selected notes stay; a navigation starts at the
 * top with nothing selected.
 */
- (void)
refresh
{
    NotesLibrary      *lib  = self.lib;
    BOOL               keep = self.shown_any && lib.sel_kind == self.shown_kind && lib.sel_id == self.shown_id;
    on_library_view_t  mode = on_library_view(lib);
    NSScrollView *scroll = mode == on_library_view_actions ? self.actions_scroll
                         : mode == on_library_view_grid    ? self.grid_scroll : self.list_scroll;
    NSPoint origin = keep ? scroll.contentView.bounds.origin : NSZeroPoint;

    [self.card_queue removeAllObjects];
    if (mode == on_library_view_actions) {
        [self load_actions];
        self.lib.populating++;
        [self.actions reloadData];
        self.lib.populating--;
    } else {
        if (!keep) [self.selected removeAllObjects];
        [self load_records:mode == on_library_view_list && lib.app->comfortable_list];
        self.list.rowHeight = lib.app->comfortable_list ? on_look_row_comfortable_h : on_look_row_h;
        [self reload_notes];
        if (mode == on_library_view_grid) [self queue_cards];
    }
    [self show_view:scroll];
    [scroll.contentView scrollToPoint:origin];
    [scroll reflectScrolledClipView:scroll.contentView];
    self.shown_kind = lib.sel_kind;
    self.shown_id   = lib.sel_id;
    self.shown_any  = YES;
}

/*
 * refresh_note: — one note was saved (GTK library_notify_note_saved): its
 * row's title, time and preview are updated in place and it re-sorts; the
 * grid re-renders its card.  Action Items take the full refresh (a save
 * can add, drop or reorder items).
 *
 * Inputs:
 *   note_id — the note
 */
- (void)
refresh_note:(gint64)note_id
{
    on_library_view_t mode = on_library_view(self.lib);
    if (mode == on_library_view_actions) {
        [self refresh];
        return;
    }
    NSUInteger  i = [self index_of_note:note_id];
    OnNoteMeta *m = i != NSNotFound ? on_db_note_get(self.lib.app->db, note_id) : NULL;
    if (m == NULL) return;
    NotesNoteRecord *r = self.records[i];
    r.title      = @(m->title ?: "");
    r.updated_at = m->updated_at;
    r.modified   = list_time_text(m->updated_at);
    on_db_note_meta_free(m);
    if (mode == on_library_view_list && self.lib.app->comfortable_list) {
        gchar *body = on_db_note_body_text(self.lib.app->db, note_id);
        r.preview = preview_line(body);
        g_free(body);
    }
    [self sort_records];
    [self reload_notes];
    if (mode == on_library_view_grid) {
        [self.card_queue addObject:@(note_id)];
        [self schedule_cards];
    }
}

/* ── Grid cards ───────────────────────────────────────────────────────── */

/*
 * queue_cards — queue a render for every card missing or out of date (a
 * stale card still shows until its replacement is ready).
 */
- (void)
queue_cards
{
    BOOL dark = on_ui_is_dark(self.grid);
    for (NotesNoteRecord *r in self.records) {
        NotesCardEntry *e = self.cards[@(r.note_id)];
        if (e == nil || e.updated_at != r.updated_at || e.dark != dark) [self.card_queue addObject:@(r.note_id)];
    }
    [self schedule_cards];
}

/*
 * schedule_cards — make sure a fill slice is coming while cards are
 * queued.
 */
- (void)
schedule_cards
{
    if (self.card_fill_scheduled || self.card_queue.count == 0) return;
    self.card_fill_scheduled = YES;
    __weak NotesNotesPane *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [ws fill_cards]; });
}

/*
 * fill_cards — render queued cards for one slice (thumb_slice_s), hand
 * each to its item, and come back for the rest (GTK thumb_fill_idle).
 * Stops when the grid is no longer showing.
 */
- (void)
fill_cards
{
    self.card_fill_scheduled = NO;
    if (on_library_view(self.lib) != on_library_view_grid) {
        [self.card_queue removeAllObjects];
        return;
    }
    CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    BOOL    dark  = on_ui_is_dark(self.grid);
    CGFloat scale = self.grid.window.backingScaleFactor ?: 2.0;
    while (self.card_queue.count > 0 && CFAbsoluteTimeGetCurrent() - start < thumb_slice_s) {
        NSNumber *nid = self.card_queue.firstObject;
        [self.card_queue removeObjectAtIndex:0];
        NSUInteger i = [self index_of_note:nid.longLongValue];
        if (i == NSNotFound) continue;
        NotesCardEntry *e = [[NotesCardEntry alloc] init];
        e.updated_at = self.records[i].updated_at;
        e.dark       = dark;
        e.image      = render_card(self.lib.app->db, nid.longLongValue, dark, scale);
        self.cards[nid] = e;
        NSCollectionViewItem *item = [self.grid itemAtIndexPath:[NSIndexPath indexPathForItem:(NSInteger)i inSection:0]];
        ((NotesCardView *)item.view).thumb.image = e.image;
    }
    [self schedule_cards];
}

/* ── Opening ──────────────────────────────────────────────────────────── */

/*
 * open_record: — open a listed note in its editor.
 *
 * Inputs:
 *   i — its index in the records
 */
- (void)
open_record:(NSUInteger)i
{
    if (i < self.records.count && self.lib.app->open_note)
        self.lib.app->open_note(self.lib.app, self.records[i].note_id);
}

/*
 * list_activated: — the list's double-click or Return: open the notes.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
list_activated:(id)sender
{
    [on_activated_rows(self.list) enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        [self open_record:i];
    }];
}

/*
 * actions_activated: — Action Items' double-click or Return: hands the
 * rows and, for a double-click, the clicked column on.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
actions_activated:(id)sender
{
    NSInteger col = self.actions.by_key ? -1 : self.actions.clickedColumn;
    [self activate_actions:on_activated_rows(self.actions)
                    column:col >= 0 ? self.actions.tableColumns[(NSUInteger)col].identifier : nil];
}

/*
 * activate_actions:column: — activated action items: a double-click on a
 * due date sets it; otherwise each item's note opens at the item.
 *
 * Inputs:
 *   rows — the items' rows
 *   key  — the clicked column's key; nil for Return
 */
- (void)
activate_actions:(NSIndexSet *)rows column:(NSString *)key
{
    BOOL on_due = [key isEqualToString:@"due"];
    [rows enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        if (i >= self.action_records.count) return;
        NotesActionRecord *r = self.action_records[i];
        if (on_due) [self ask_due:r];
        else        on_editor_window_open(self.lib.app, r.note_id, NotesEditorRevealAction, r.ord, nil);
    }];
}

/* ── Action Items: done and due ───────────────────────────────────────── */

/*
 * toggle_action: — a row's checkbox: write the item's done state (task row
 * first, then its line); a completed item leaves the list at once while
 * completed items are hidden; the sidebar's open count follows.
 *
 * Inputs:
 *   sender — the checkbox (its row says which item)
 */
- (void)
toggle_action:(NSButton *)sender
{
    NSInteger row = [self.actions rowForView:sender];
    if (row < 0 || (NSUInteger)row >= self.action_records.count) return;
    NotesActionRecord *r = self.action_records[(NSUInteger)row];
    r.done = sender.state == NSControlStateValueOn;
    on_action_edit_set_done(self.lib.app, r.uid, r.done);
    NSIndexSet *rows = [NSIndexSet indexSetWithIndex:(NSUInteger)row];
    if (r.done && !self.lib.app->show_done_actions) {
        NSMutableArray *left = [self.action_records mutableCopy];
        [left removeObjectAtIndex:(NSUInteger)row];
        self.action_records = left;
        [self.actions removeRowsAtIndexes:rows withAnimation:NSTableViewAnimationEffectNone];
    } else {
        [self.actions reloadDataForRowIndexes:rows
                                columnIndexes:[NSIndexSet indexSetWithIndexesInRange:
                                               NSMakeRange(0, (NSUInteger)self.actions.numberOfColumns)]];
    }
    if (self.lib.app->sidebar_counts) on_library_sidebar_refresh(self.lib);
}

/*
 * ask_due: — an item's due date, in a tool panel with a calendar: Set
 * writes the picked day, Clear removes it (GTK action_due_dialog).
 *
 * Inputs:
 *   r — the item
 */
- (void)
ask_due:(NotesActionRecord *)r
{
    NSDatePicker *picker = [[NSDatePicker alloc] initWithFrame:NSZeroRect];
    picker.identifier         = @"due-picker";
    picker.datePickerStyle    = NSDatePickerStyleClockAndCalendar;
    picker.datePickerElements = NSDatePickerElementFlagYearMonthDay;
    picker.dateValue = r.due ? [NSDate dateWithTimeIntervalSince1970:(double)r.due] : NSDate.date;
    NSSize size = picker.fittingSize;
    picker.translatesAutoresizingMaskIntoConstraints = NO;
    [picker.widthAnchor  constraintEqualToConstant:size.width].active  = YES;
    [picker.heightAnchor constraintEqualToConstant:size.height].active = YES;

    __weak NotesNotesPane *ws = self;
    NotesToolPanel *panel = [NotesToolPanel
        panel_with_title:@"Due Date" hero_title:@"Due Date" subtitle:r.text symbol:@"calendar"
                 content:picker buttons:@[@"Set", @"Clear", @"Cancel"]
                 handler:^NSString *(NSInteger index) {
        NotesNotesPane *s = ws;
        if (s == nil || index == 2) return nil;
        gint64 due = 0;
        if (index == 0)
            due = (gint64)[NSCalendar.currentCalendar startOfDayForDate:picker.dateValue].timeIntervalSince1970;
        if (!on_action_edit_set_due(s.lib.app, r.uid, due)) return @"The due date could not be saved.";
        r.due = due;
        [s sort_actions];
        [s.actions reloadData];
        return nil;
    }];
    [panel show_over:self.lib.window];
}

/* ── Menus ─────────────────────────────────────────────────────────────── */

/*
 * fill_note_menu:note: — the per-note menu (GTK show_note_context_menu):
 * Open the clicked note; in the Trash Restore and Delete Permanently;
 * elsewhere Pin/Unpin, the two exports and Move to Trash — commands that
 * act on the selection.
 *
 * Inputs:
 *   menu    — the menu to fill (emptied first)
 *   note_id — the clicked note
 */
- (void)
fill_note_menu:(NSMenu *)menu note:(gint64)note_id
{
    [menu removeAllItems];
    NSMenuItem *open = [[NSMenuItem alloc] initWithTitle:@"Open" action:@selector(open_clicked:)
                                           keyEquivalent:@""];
    open.target            = self;
    open.representedObject = @(note_id);
    [menu addItem:open];
    if (on_library_in_trash_view(self.lib)) {
        [menu addItem:on_command_menu_item(@"note-restore")];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:on_command_menu_item(@"delete-note")];
        return;
    }
    [menu addItem:on_command_menu_item(@"note-pin")];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:on_command_menu_item(@"note-export-html")];
    [menu addItem:on_command_menu_item(@"note-export-md")];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:on_command_menu_item(@"delete-note")];
}

/*
 * open_clicked: — the menu's Open: just the clicked note.
 *
 * Inputs:
 *   sender — the menu item (its object is the note id)
 */
- (void)
open_clicked:(NSMenuItem *)sender
{
    if (self.lib.app->open_note)
        self.lib.app->open_note(self.lib.app, [sender.representedObject longLongValue]);
}

/*
 * select_clicked: — a right click on a note outside the selection selects
 * just that note; inside the selection the selection stays.
 *
 * Inputs:
 *   i — the clicked note's index
 */
- (void)
select_clicked:(NSUInteger)i
{
    NSNumber *nid = @(self.records[i].note_id);
    if ([self.selected containsObject:nid]) return;
    [self.selected removeAllObjects];
    [self.selected addObject:nid];
    [self show_selection];
}

/*
 * grid_menu: — the grid's right-click menu.
 *
 * Inputs:
 *   path — the card clicked, or nil
 *
 * Output: the menu, or nil.
 */
- (NSMenu *)
grid_menu:(NSIndexPath *)path
{
    if (path == nil || (NSUInteger)path.item >= self.records.count) return nil;
    [self select_clicked:(NSUInteger)path.item];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    [self fill_note_menu:menu note:self.records[(NSUInteger)path.item].note_id];
    return menu;
}

/*
 * menuNeedsUpdate: — the list's note menu (for the row clicked), or a
 * header's column menu: one check item per column; the last visible
 * column cannot be hidden.
 *
 * Inputs:
 *   menu — the menu about to open
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    [menu removeAllItems];
    if (menu == self.list.menu) {
        NSInteger row = self.list.clickedRow;
        if (row < 0 || (NSUInteger)row >= self.records.count) return;
        [self select_clicked:(NSUInteger)row];
        [self fill_note_menu:menu note:self.records[(NSUInteger)row].note_id];
        return;
    }
    BOOL                 is_list = menu == self.list.headerView.menu;
    NSTableView         *table   = is_list ? self.list : self.actions;
    const column_spec_t *specs   = is_list ? note_columns : action_columns;
    size_t               n       = is_list ? sizeof note_columns / sizeof *note_columns
                                           : sizeof action_columns / sizeof *action_columns;
    NSUInteger visible = 0;
    for (NSTableColumn *c in table.tableColumns) visible += !c.hidden;
    menu.autoenablesItems = NO;
    for (size_t i = 0; i < n; i++) {
        NSTableColumn *c    = [table tableColumnWithIdentifier:specs[i].key];
        NSMenuItem    *item = [menu addItemWithTitle:specs[i].menu_label action:@selector(toggle_column:)
                                       keyEquivalent:@""];
        item.target            = self;
        item.representedObject = c;
        item.state             = c.hidden ? NSControlStateValueOff : NSControlStateValueOn;
        item.enabled           = c.hidden || visible > 1;
    }
}

/*
 * toggle_column: — a header menu item: show or hide its column, fit the
 * columns to the table again (a column shown at the end would otherwise
 * sit past the edge), and keep the layout.
 *
 * Inputs:
 *   sender — the item (its object is the column)
 */
- (void)
toggle_column:(NSMenuItem *)sender
{
    NSTableColumn *c = sender.representedObject;
    c.hidden = !c.hidden;
    [c.tableView sizeToFit];
    persist_layout(c.tableView, c.tableView == self.list ? "list_columns" : "action_columns");
}

/* ── Table data source and delegate ──────────────────────────────────── */

/*
 * numberOfRowsInTableView: — the notes, or the action items.
 *
 * Inputs:
 *   table — which table
 *
 * Output: the count.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)table
{
    return (NSInteger)(table == self.list ? self.records.count : self.action_records.count);
}

/*
 * title_cell: — the list's Title cell: the title and, in Comfortable
 * density, the title (bold when that setting is on, "Untitled" when
 * empty) over a small dimmed preview.
 *
 * Inputs:
 *   r — the note
 *
 * Output: the cell.
 */
- (NSTableCellView *)
title_cell:(NotesNoteRecord *)r
{
    NSTableCellView *cell = [self.list makeViewWithIdentifier:@"title" owner:nil];
    if (cell == nil) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"title";
        NSTextField *title   = [NSTextField labelWithString:@""];
        NSTextField *preview = [NSTextField labelWithString:@""];
        preview.identifier = @"preview";
        preview.font       = [NSFont systemFontOfSize:on_look_preview_font];
        preview.alphaValue = on_look_preview_alpha;
        for (NSTextField *l in @[title, preview]) {
            l.lineBreakMode = NSLineBreakByTruncatingTail;
            [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
        }
        NSStackView *stack = [NSStackView stackViewWithViews:@[title, preview]];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment   = NSLayoutAttributeLeading;
        stack.spacing     = 0;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:stack];
        cell.textField = title;
        [NSLayoutConstraint activateConstraints:@[
            [stack.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:4],
            [stack.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
            [stack.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    NSTextField *preview = (NSTextField *)cell.textField.superview.subviews.lastObject;
    BOOL comfy = self.lib.app->comfortable_list;
    cell.textField.stringValue = r.title.length ? r.title : comfy ? @"Untitled" : @"";
    cell.textField.font = comfy && self.lib.app->bold_list_titles
                        ? [NSFont boldSystemFontOfSize:on_look_cell_font]
                        : [NSFont systemFontOfSize:on_look_cell_font];
    preview.stringValue = r.preview ?: @"";
    preview.hidden      = !(comfy && r.preview.length);
    return cell;
}

/*
 * tableView:viewForTableColumn:row: — a cell of either table.
 *
 * Inputs:
 *   table  — the table
 *   column — the column
 *   row    — the row
 *
 * Output: the cell.
 */
- (NSView *)
tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSString *key = column.identifier;
    if (table == self.list) {
        NotesNoteRecord *r = self.records[(NSUInteger)row];
        if ([key isEqualToString:@"title"]) return [self title_cell:r];
        NSTableCellView *cell = label_cell(table, key);
        cell.textField.stringValue = [key isEqualToString:@"path"]     ? r.path
                                   : [key isEqualToString:@"modified"] ? r.modified : r.created;
        return cell;
    }
    NotesActionRecord *r = self.action_records[(NSUInteger)row];
    if ([key isEqualToString:@"done"]) {
        NSButton *check = [table makeViewWithIdentifier:@"done" owner:nil];
        if (check == nil) {
            check = [NSButton checkboxWithTitle:@"" target:self action:@selector(toggle_action:)];
            check.identifier = @"done";
        }
        check.state = r.done ? NSControlStateValueOn : NSControlStateValueOff;
        return check;
    }
    NSTableCellView *cell = label_cell(table, key);
    NSString *text   = [key isEqualToString:@"context"] ? r.context
                     : [key isEqualToString:@"action"]  ? r.text : due_text(r.due);
    NSColor  *colour = [key isEqualToString:@"due"] && r.due ? due_colour(r.due) : nil;
    cell.textField.attributedStringValue = struck(text, r.done, colour);
    return cell;
}

/*
 * tableView:sortDescriptorsDidChange: — a header was clicked: re-sort
 * (the grid shares the list's order), keeping the selection.
 *
 * Inputs:
 *   table — the table
 *   old   — the previous sort (unused)
 */
- (void)
tableView:(NSTableView *)table sortDescriptorsDidChange:(NSArray<NSSortDescriptor *> *)old
{
    if (table == self.list) {
        [self sort_records];
        [self reload_notes];
    } else {
        [self sort_actions];
        [self.actions reloadData];
    }
}

/*
 * tableViewColumnDidMove: — a header was dragged: keep the layout.
 *
 * Inputs:
 *   note — the notification (its object is the table)
 */
- (void)
tableViewColumnDidMove:(NSNotification *)note
{
    NSTableView *table = note.object;
    persist_layout(table, table == self.list ? "list_columns" : "action_columns");
}

/*
 * note_selection_changed: — the user changed the selection: it is kept by
 * note id, and the status bar says how many are selected.
 *
 * Inputs:
 *   ids — the selected notes
 */
- (void)
note_selection_changed:(NSArray<NSNumber *> *)ids
{
    if (self.lib.populating > 0) return;
    self.selected = [NSMutableOrderedSet orderedSetWithArray:ids];
    if (ids.count > 0)
        on_app_status(self.lib.app, "%lu file%s selected", (unsigned long)ids.count, ids.count == 1 ? "" : "s");
}

/*
 * tableViewSelectionDidChange: — the list's selection changed.
 *
 * Inputs:
 *   note — the notification
 */
- (void)
tableViewSelectionDidChange:(NSNotification *)note
{
    if (note.object != self.list) return;
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    [self.list.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        if (i < self.records.count) [ids addObject:@(self.records[i].note_id)];
    }];
    [self note_selection_changed:ids];
}

/*
 * tableView:pasteboardWriterForRow: — a dragged note row (one pasteboard
 * item per row; the sidebar reads them all).
 *
 * Inputs:
 *   table — the table
 *   row   — the row
 *
 * Output: the item, or nil for Action Items.
 */
- (id<NSPasteboardWriting>)
tableView:(NSTableView *)table pasteboardWriterForRow:(NSInteger)row
{
    if (table != self.list || (NSUInteger)row >= self.records.count) return nil;
    return on_library_drag_item(on_drag_notes, @[@(self.records[(NSUInteger)row].note_id)]);
}

/* ── Grid data source and delegate ────────────────────────────────────── */

/*
 * collectionView:numberOfItemsInSection: — the notes.
 *
 * Inputs:
 *   cv      — the grid (unused)
 *   section — the section (unused)
 *
 * Output: the count.
 */
- (NSInteger)
collectionView:(NSCollectionView *)cv numberOfItemsInSection:(NSInteger)section
{
    return (NSInteger)self.records.count;
}

/*
 * collectionView:itemForRepresentedObjectAtIndexPath: — a card: its
 * rendered thumbnail when there is one (a stale one until replaced).
 *
 * Inputs:
 *   cv   — the grid
 *   path — which
 *
 * Output: the item.
 */
- (NSCollectionViewItem *)
collectionView:(NSCollectionView *)cv itemForRepresentedObjectAtIndexPath:(NSIndexPath *)path
{
    NSCollectionViewItem *item = [cv makeItemWithIdentifier:@"card" forIndexPath:path];
    NotesNoteRecord      *r    = self.records[(NSUInteger)path.item];
    NotesCardView        *card = (NotesCardView *)item.view;
    card.thumb.image       = self.cards[@(r.note_id)].image;
    card.title.stringValue = r.title;
    return item;
}

/*
 * grid_selection_changed — the grid's selection changed: keep it.
 */
- (void)
grid_selection_changed
{
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    for (NSIndexPath *ip in self.grid.selectionIndexPaths)
        if ((NSUInteger)ip.item < self.records.count) [ids addObject:@(self.records[(NSUInteger)ip.item].note_id)];
    [self note_selection_changed:ids];
}

/*
 * collectionView:didSelectItemsAtIndexPaths: — cards were selected.
 *
 * Inputs:
 *   cv    — the grid (unused)
 *   paths — the items (unused)
 */
- (void)
collectionView:(NSCollectionView *)cv didSelectItemsAtIndexPaths:(NSSet<NSIndexPath *> *)paths
{
    [self grid_selection_changed];
}

/*
 * collectionView:didDeselectItemsAtIndexPaths: — cards were deselected.
 *
 * Inputs:
 *   cv    — the grid (unused)
 *   paths — the items (unused)
 */
- (void)
collectionView:(NSCollectionView *)cv didDeselectItemsAtIndexPaths:(NSSet<NSIndexPath *> *)paths
{
    [self grid_selection_changed];
}

/*
 * collectionView:canDragItemsAtIndexPaths:withEvent: — cards can be
 * dragged.
 *
 * Inputs:
 *   cv, paths, event — unused
 *
 * Output: YES.
 */
- (BOOL)
collectionView:(NSCollectionView *)cv canDragItemsAtIndexPaths:(NSSet<NSIndexPath *> *)paths
     withEvent:(NSEvent *)event
{
    return YES;
}

/*
 * collectionView:pasteboardWriterForItemAtIndexPath: — a dragged card.
 *
 * Inputs:
 *   cv   — the grid (unused)
 *   path — which
 *
 * Output: the pasteboard item.
 */
- (id<NSPasteboardWriting>)
collectionView:(NSCollectionView *)cv pasteboardWriterForItemAtIndexPath:(NSIndexPath *)path
{
    return on_library_drag_item(on_drag_notes, @[@(self.records[(NSUInteger)path.item].note_id)]);
}

@end

/*
 * cards_appearance_changed — a card's appearance changed: queue the cards
 * that were rendered for the other one (a no-op unless the grid shows).
 */
static void
cards_appearance_changed(void)
{
    if (notes_pane && on_library_view(notes_pane.lib) == on_library_view_grid) [notes_pane queue_cards];
}

/* ── Commands ───────────────────────────────────────────────────────────── */

/*
 * run_view_list — "view-list": the list.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_view_list(id target, id sender)
{
    ((NotesLibrary *)target).grid_pref = NO;
    on_library_notes_refresh(target);
}

/*
 * run_view_grid — "view-grid": the grid.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_view_grid(id target, id sender)
{
    ((NotesLibrary *)target).grid_pref = YES;
    on_library_notes_refresh(target);
}

/*
 * run_toggle_view — "toggle-view": the other of list and grid.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_toggle_view(id target, id sender)
{
    NotesLibrary *lib = target;
    lib.grid_pref = !lib.grid_pref;
    on_library_notes_refresh(lib);
}

/*
 * validate_toggle_view — the item shows what a click does: the list icon
 * while the grid shows, and the other way round.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item
 *
 * Output: YES.
 */
static BOOL
validate_toggle_view(id target, id item)
{
    BOOL grid = ((NotesLibrary *)target).grid_pref;
    on_command_item_update(item, grid ? @"Show as List" : @"Show as Grid", grid ? @"list" : @"grid", NO);
    return YES;
}

/*
 * run_toggle_done — "toggle-done": hide or show completed action items;
 * the setting persists and everything is rebuilt (GTK done_set_shown).
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_toggle_done(id target, id sender)
{
    NotesLibrary *lib = target;
    lib.app->show_done_actions = !lib.app->show_done_actions;
    on_app_config_set("show_done_actions", lib.app->show_done_actions ? "1" : "0");
    on_library_refresh_all(lib);
}

/*
 * validate_toggle_done — the item says what a click does.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item
 *
 * Output: YES.
 */
static BOOL
validate_toggle_done(id target, id item)
{
    BOOL shown = ((NotesLibrary *)target).app->show_done_actions;
    on_command_item_update(item, shown ? @"Hide Completed" : @"Show Completed",
                           shown ? @"hidden" : @"visible", NO);
    return YES;
}

/*
 * run_note_open — "note-open": open the selected notes.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_note_open(id target, id sender)
{
    NotesLibrary *lib = target;
    for (NSNumber *n in on_library_notes_selected_ids(lib))
        if (lib.app->open_note) lib.app->open_note(lib.app, n.longLongValue);
}

/*
 * validate_some_selected — notes are selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_some_selected(id target, id item)
{
    return on_library_notes_selected_ids(target).count > 0;
}

/*
 * run_delete_note — "delete-note": move the selected notes to the Trash
 * (their editors closed first), or, in the Trash view, delete them
 * permanently after one confirmation.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_delete_note(id target, id sender)
{
    NotesLibrary *lib = target;
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    if (ids.count == 0) return;
    if (on_library_in_trash_view(lib)) {
        on_library_delete_notes(lib, ids);
        return;
    }
    GArray *note_ids = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)ids.count);
    for (NSNumber *n in ids) {
        gint64 nid = n.longLongValue;
        g_array_append_val(note_ids, nid);
    }
    if (on_library_trash_notes(lib, (const gint64 *)(void *)note_ids->data, note_ids->len))
        on_library_refresh_all(lib);
    g_array_free(note_ids, TRUE);
}

/*
 * validate_delete_note — notes are selected and no text field in the
 * library is being typed in (⌘⌫ there means "delete to the start of the
 * line", and a menu key equivalent would otherwise win over the field);
 * the item says "Delete Permanently" in the Trash view.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the menu or toolbar item
 *
 * Output: YES to enable it.
 */
static BOOL
validate_delete_note(id target, id item)
{
    NotesLibrary *lib = target;
    on_command_item_update(item, on_library_in_trash_view(lib) ? @"Delete Permanently" : nil, nil, NO);
    if ([lib.window.firstResponder isKindOfClass:NSText.class]) return NO;
    return on_library_notes_selected_ids(lib).count > 0;
}

/*
 * run_note_restore — "note-restore": the selected notes come back out of
 * the Trash.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_note_restore(id target, id sender)
{
    NotesLibrary *lib = target;
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    if (ids.count == 0) return;
    for (NSNumber *n in ids) on_db_note_restore(lib.app->db, n.longLongValue);
    on_app_status(lib.app, "Restored %lu note%s", (unsigned long)ids.count, ids.count == 1 ? "" : "s");
    on_library_refresh_all(lib);
}

/*
 * validate_note_restore — notes are selected in a Trash view.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *
 * Output: YES to enable it.
 */
static BOOL
validate_note_restore(id target, id item)
{
    return on_library_in_trash_view(target) && on_library_notes_selected_ids(target).count > 0;
}

/*
 * first_selected_pinned — is the first selected note pinned?  Pin/Unpin
 * offers the opposite, for the whole selection.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: YES when it is.
 */
static BOOL
first_selected_pinned(NotesLibrary *lib)
{
    NSNumber   *first = on_library_notes_selected_ids(lib).firstObject;
    OnNoteMeta *m     = first ? on_db_note_get(lib.app->db, first.longLongValue) : NULL;
    BOOL pinned = m && m->pinned;
    if (m) on_db_note_meta_free(m);
    return pinned;
}

/*
 * run_note_pin — "note-pin": pin every selected note, or unpin them all
 * when the first is pinned.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_note_pin(id target, id sender)
{
    NotesLibrary *lib = target;
    gboolean      pin = !first_selected_pinned(lib);
    for (NSNumber *n in on_library_notes_selected_ids(lib))
        on_db_note_set_pinned(lib.app->db, n.longLongValue, pin);
    on_library_refresh_all(lib);
}

/*
 * validate_note_pin — notes are selected outside the Trash; the item says
 * Pin or Unpin.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item
 *
 * Output: YES to enable it.
 */
static BOOL
validate_note_pin(id target, id item)
{
    NotesLibrary *lib = target;
    on_command_item_update(item, first_selected_pinned(lib) ? @"Unpin" : @"Pin", nil, NO);
    return !on_library_in_trash_view(lib) && on_library_notes_selected_ids(lib).count > 0;
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t notes_handlers[] = {
    { @"view-list",    run_view_list,    NULL                   },
    { @"view-grid",    run_view_grid,    NULL                   },
    { @"toggle-view",  run_toggle_view,  validate_toggle_view   },
    { @"toggle-done",  run_toggle_done,  validate_toggle_done   },
    { @"note-open",    run_note_open,    validate_some_selected },
    { @"delete-note",  run_delete_note,  validate_delete_note   },
    { @"note-restore", run_note_restore, validate_note_restore  },
    { @"note-pin",     run_note_pin,     validate_note_pin      },
};

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_library_notes_install — see library_notes.h.
 */
void
on_library_notes_install(NotesLibrary *lib)
{
    on_library_add_commands(notes_handlers, sizeof notes_handlers / sizeof notes_handlers[0]);
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
    if (!notes_pane) return;
    [notes_pane refresh];
    on_library_window_location_changed(lib);
}

/*
 * on_library_notes_note_saved — see library_notes.h.
 */
void
on_library_notes_note_saved(NotesLibrary *lib, gint64 note_id)
{
    [notes_pane refresh_note:note_id];
}

/*
 * on_library_notes_selected_ids — see library_notes.h.
 */
NSArray<NSNumber *> *
on_library_notes_selected_ids(NotesLibrary *lib)
{
    if (!notes_pane || on_library_view(lib) == on_library_view_actions) return @[];
    return notes_pane.selected.array;
}
