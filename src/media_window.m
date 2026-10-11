/*
 * media_window.m — the media browser.
 *
 * Specification: the GTK build's src/media_window.c.  Every image in
 * the notes the library was listing when the window opened (a snapshot of
 * on_library_notes_for_selection), as a grid of square thumbnails with the
 * note's title under each, and the shared image viewer over the grid.
 *
 *   scanning   — the notes are walked in 40 ms slices on the main thread,
 *                one image per step, so a big selection fills the grid
 *                progressively and never freezes it.  A note's blob is read
 *                once and kept across slices; image counts come from a
 *                record walk that decodes nothing (on_note_count_images);
 *                only the one PNG behind each thumbnail is decoded, capped
 *                at thumbnail size.  At most media_max_images cells.
 *   the viewer — one click (or Return) on a thumbnail shows the picture in
 *                the viewer, decoded afresh at the viewer's size and
 *                dropped when it closes; its "Show in source note" link
 *                opens the note scrolled to the image.  The viewer takes the
 *                window's keys while it is up.
 *   addressing — a cell is (note id, image ordinal); the grid index is the
 *                viewer's index, and cells are only ever appended.
 */

#import "media_window.h"

#import "activation.h"
#import "editor_window.h"
#import "image_viewer.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "commands.h"
#include "config.h"
#include "db.h"
#include "serialize.h"

/* How many thumbnails one window builds at most (each holds its pixels). */
static const NSUInteger media_max_images = 500;

/* How often the scan hands its new thumbnails to the grid (seconds). */
static const double media_batch_s = 0.1;

/* The caption's height under a thumbnail, and the cell's padding. */
static const CGFloat caption_h = 16.0;
static const CGFloat cell_pad  = 3.0;

/* One note in the scan queue. */
@interface NotesMediaNote : NSObject
@property (nonatomic, assign) gint64    note_id;
@property (nonatomic, copy)   NSString *title;
@end

@implementation NotesMediaNote
@end

/* One thumbnail: the image's address and what the captions say. */
@interface NotesMediaCell : NSObject
@property (nonatomic, assign) gint64    note_id;
@property (nonatomic, assign) gint      ord;
@property (nonatomic, assign) gint      n_img;
@property (nonatomic, copy)   NSString *title;
@property (nonatomic, strong) NSImage  *thumb;
@end

@implementation NotesMediaCell
@end

/* A scan's stop flag, shared with its worker (an atomic property). */
@interface NotesMediaScan : NSObject
@property (atomic, assign) BOOL cancelled;
@end

@implementation NotesMediaScan
@end

/* ── The cell view ─────────────────────────────────────────────────────── */

/*
 * NotesMediaItem — a thumbnail square with the note's title under it,
 * small and dimmed.
 */
@interface NotesMediaItem : NSCollectionViewItem
@property (nonatomic, strong) NSTextField *caption;
@end

@implementation NotesMediaItem

/*
 * loadView — the square image view (made at its final size) and the
 * caption.
 */
- (void)
loadView
{
    CGFloat      s    = on_look_media_thumb;
    NSView      *cell = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, s + 2 * cell_pad, s + caption_h + 2 * cell_pad)];
    NSImageView *iv   = on_ui_image_view(nil, s, s);
    iv.imageScaling = NSImageScaleProportionallyDown;
    iv.frame        = NSMakeRect(cell_pad, caption_h + cell_pad, s, s);
    self.caption = [NSTextField labelWithString:@""];
    self.caption.frame         = NSMakeRect(cell_pad, cell_pad, s, caption_h);
    self.caption.font          = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    self.caption.textColor     = NSColor.secondaryLabelColor;
    self.caption.alignment     = NSTextAlignmentCenter;
    self.caption.lineBreakMode = NSLineBreakByTruncatingTail;
    [cell addSubview:iv];
    [cell addSubview:self.caption];
    self.view      = cell;
    self.imageView = iv;
}

@end

/* ── The window ────────────────────────────────────────────────────────── */

@interface NotesMediaWindow : NSWindowController <NSWindowDelegate, NSCollectionViewDataSource>
@property (nonatomic, weak)   NotesLibrary        *lib;
@property (nonatomic, copy)   NSString            *scope;
@property (nonatomic, strong) NotesActivatingGrid *grid;
@property (nonatomic, strong) NotesImageViewer    *viewer;
@property (nonatomic, strong) NSTextField         *status;
@property (nonatomic, strong) NSProgressIndicator *spinner;
@property (nonatomic, strong) NSArray<NotesMediaNote *>        *notes;
@property (nonatomic, strong) NSMutableArray<NotesMediaCell *> *cells;
@property (nonatomic, strong) NotesMediaScan *scan;     /* in flight, or nil      */
@property (nonatomic, assign) BOOL       scanning;
@property (nonatomic, assign) NSInteger  n_notes;       /* notes with an image    */
@property (nonatomic, assign) BOOL       truncated;
@end

@implementation NotesMediaWindow

/*
 * init_with_lib: — snapshot the listed notes, build the window at the last
 * media window's size, and start the scan.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the controller.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    gint w = (gint)on_look_media_w, h = (gint)on_look_media_h;
    on_app_config_get_size("media_win_w", "media_win_h", &w, &h);
    NotesKeyFilterWindow *win = [[NotesKeyFilterWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, w, h)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered defer:NO];
    win.releasedWhenClosed = NO;
    win.contentMinSize     = NSMakeSize(300, 200);
    if (!(self = [super initWithWindow:win])) return nil;
    _lib   = lib;
    _cells = [NSMutableArray array];
    win.delegate = self;

    /* Action Items is not a place: its media set is every note's. */
    _scope = lib.sel_kind == on_sb_actions ? @"All Notes" : lib.sel_name.length ? lib.sel_name : @"Notes";
    win.title = [NSString stringWithFormat:@"Media in %@", _scope];
    NSMutableArray<NotesMediaNote *> *notes = [NSMutableArray array];
    GList *list = on_library_notes_for_selection(lib);
    for (GList *l = list; l != NULL; l = l->next) {
        const OnNoteMeta *m = l->data;
        NotesMediaNote   *n = [[NotesMediaNote alloc] init];
        n.note_id = m->id;
        n.title   = @(m->title ?: "");
        [notes addObject:n];
    }
    on_db_note_list_free(list);
    _notes = notes;

    [self build];
    __weak NotesMediaWindow *ws = self;
    win.key_filter = ^BOOL(NSEvent *event) {
        NotesMediaWindow *s = ws;
        if (s == nil || !on_image_viewer_is_open(s.viewer)) return NO;
        on_image_viewer_key_down(s.viewer, event);
        return YES;                  /* modal: the grid behind sees no key */
    };
    on_ui_use_ui_font(win.contentView);
    [win center];
    return self;
}

/*
 * build — the grid, the viewer over it, and the status line with its
 * spinner.
 */
- (void)
build
{
    CGFloat s = on_look_media_thumb;
    NSCollectionViewFlowLayout *layout = [[NSCollectionViewFlowLayout alloc] init];
    layout.itemSize                = NSMakeSize(s + 2 * cell_pad, s + caption_h + 2 * cell_pad);
    layout.minimumInteritemSpacing = 6;
    layout.minimumLineSpacing      = 6;
    layout.sectionInset            = NSEdgeInsetsMake(8, 8, 8, 8);
    self.grid = [[NotesActivatingGrid alloc] initWithFrame:NSZeroRect];
    self.grid.collectionViewLayout = layout;
    self.grid.dataSource           = self;
    self.grid.selectable           = YES;
    self.grid.activate_clicks      = 1;      /* one click opens, as in GTK */
    [self.grid registerClass:NotesMediaItem.class forItemWithIdentifier:@"media"];
    __weak NotesMediaWindow *ws = self;
    self.grid.activate_handler = ^(NSSet<NSIndexPath *> *paths) {
        NSIndexPath *path = paths.anyObject;
        if (path) on_image_viewer_open(ws.viewer, path.item);
    };

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = self.grid;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;

    self.spinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    self.spinner.style       = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.status = [NSTextField labelWithString:@""];
    self.status.identifier    = @"media-status";
    self.status.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *status_row = [NSStackView stackViewWithViews:@[self.spinner, self.status]];
    status_row.spacing = 6;

    NSStackView *column = [NSStackView stackViewWithViews:@[scroll, status_row]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment   = NSLayoutAttributeLeading;
    column.spacing     = 6;
    column.edgeInsets  = NSEdgeInsetsMake(8, 8, 8, 8);
    column.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *content = self.window.contentView;
    [content addSubview:column];
    on_ui_pin_edges(column, content);
    [NSLayoutConstraint activateConstraints:@[
        [scroll.widthAnchor    constraintEqualToAnchor:column.widthAnchor constant:-16],
        [status_row.widthAnchor constraintEqualToAnchor:column.widthAnchor constant:-16],
    ]];
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];

    self.viewer = on_image_viewer_new(scroll, @"Show in source note",
                                      @"Open the note this image is in, scrolled to the image");
    self.viewer.count_handler = ^NSInteger {
        return (NSInteger)ws.cells.count;
    };
    self.viewer.image_handler = ^NSImage *(NSInteger idx, NSSize box) {
        return [ws image_at:idx box:box];
    };
    self.viewer.caption_handler = ^NSString *(NSInteger idx) {
        NotesMediaWindow *s = ws;
        if (s == nil || idx < 0 || (NSUInteger)idx >= s.cells.count) return @"";
        NotesMediaCell *c = s.cells[(NSUInteger)idx];
        return [NSString stringWithFormat:@"%@ — image %d of %d", c.title, c.ord + 1, c.n_img];
    };
    self.viewer.action_handler = ^(NSInteger idx) {
        NotesMediaWindow *s = ws;
        if (s == nil || idx < 0 || (NSUInteger)idx >= s.cells.count) return;
        NotesMediaCell *c = s.cells[(NSUInteger)idx];
        on_editor_window_open(s.lib.app, c.note_id, NotesEditorRevealImage, c.ord, nil);
    };
}

/* ── Images ────────────────────────────────────────────────────────────── */

/*
 * image_at:box: — one cell's image for the viewer, read fresh and decoded
 * capped at the box at the window's scale.
 *
 * Inputs:
 *   idx — the cell
 *   box — the box in points
 *
 * Output: the image, or nil when it is gone or will not decode.
 */
- (NSImage *)
image_at:(NSInteger)idx box:(NSSize)box
{
    if (idx < 0 || (NSUInteger)idx >= self.cells.count) return nil;
    NotesMediaCell *c    = self.cells[(NSUInteger)idx];
    guint8         *blob = NULL;
    gsize           len  = 0;
    if (!on_db_note_load(self.lib.app->db, c.note_id, &blob, &len)) return nil;
    GBytes *png = blob ? on_note_image_nth_png(blob, len, c.ord) : NULL;
    g_free(blob);
    if (png == NULL) return nil;
    gsize         n;
    const guint8 *bytes = g_bytes_get_data(png, &n);
    NSImage *image = on_ui_thumbnail(bytes, n, box.width, box.height, self.window.backingScaleFactor ?: 2.0);
    g_bytes_unref(png);
    return image;
}

/* ── Scanning ──────────────────────────────────────────────────────────── */

/*
 * start_scan — fill the grid from a worker (GTK media_scan_idle, off the
 * main thread as §7 asks): it reads each listed note over its own
 * connection and decodes its images at thumbnail size, handing the new
 * cells over every media_batch_s; at most media_max_images.  Closing the
 * window stops it.
 */
- (void)
start_scan
{
    self.scanning = YES;
    self.scan     = [[NotesMediaScan alloc] init];
    self.spinner.hidden = NO;
    [self.spinner startAnimation:nil];
    [self show_status];

    NotesMediaScan          *scan  = self.scan;
    NSArray<NotesMediaNote *> *notes = self.notes;
    NSString                *path  = @(self.lib.app->db->path);
    CGFloat                  side  = on_look_media_thumb;
    CGFloat                  scale = self.window.backingScaleFactor ?: 2.0;
    __weak NotesMediaWindow *ws    = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        OnDatabase *db = on_db_open(path.fileSystemRepresentation);
        NSMutableArray<NotesMediaCell *> *batch = [NSMutableArray array];
        NSUInteger     total = 0, with_images = 0;
        BOOL           truncated = NO;
        CFAbsoluteTime handed    = CFAbsoluteTimeGetCurrent();
        for (NotesMediaNote *note in notes) {
            if (scan.cancelled || truncated || db == NULL) break;
            @autoreleasepool {
                guint8 *blob = NULL;
                gsize   len  = 0;
                if (!on_db_note_load(db, note.note_id, &blob, &len) || blob == NULL) continue;
                gint n   = on_note_count_images(blob, len);
                BOOL any = NO;
                for (gint ord = 0; ord < n; ord++) {
                    if (total >= media_max_images) {
                        truncated = YES;
                        break;
                    }
                    GBytes *png = on_note_image_nth_png(blob, len, ord);
                    if (png == NULL) continue;
                    gsize         pn;
                    const guint8 *pb    = g_bytes_get_data(png, &pn);
                    NSImage      *thumb = on_ui_thumbnail(pb, pn, side, side, scale);
                    g_bytes_unref(png);
                    if (thumb == nil) continue;
                    NotesMediaCell *c = [[NotesMediaCell alloc] init];
                    c.note_id = note.note_id;
                    c.ord     = ord;
                    c.n_img   = n;
                    c.title   = note.title;
                    c.thumb   = thumb;
                    [batch addObject:c];
                    total++;
                    any = YES;
                }
                g_free(blob);
                if (any) with_images++;
            }
            if (batch.count > 0 && CFAbsoluteTimeGetCurrent() - handed >= media_batch_s) {
                NSArray<NotesMediaCell *> *cells = [batch copy];
                NSUInteger                 noted = with_images;
                dispatch_async(dispatch_get_main_queue(), ^{ [ws take_cells:cells notes:noted scan:scan]; });
                [batch removeAllObjects];
                handed = CFAbsoluteTimeGetCurrent();
            }
        }
        on_db_close(db);
        NSArray<NotesMediaCell *> *cells = [batch copy];
        NSUInteger                 noted = with_images;
        dispatch_async(dispatch_get_main_queue(), ^{
            NotesMediaWindow *s = ws;
            [s take_cells:cells notes:noted scan:scan];
            [s finish_scan:scan truncated:truncated];
        });
    });
}

/*
 * take_cells:notes:scan: — thumbnails from the worker: into the grid, the
 * picture on show getting its Next when one follows it.
 *
 * Inputs:
 *   cells — the new cells, in order
 *   notes — how many notes have had an image so far
 *   scan  — the scan they come from; ignored unless it is the current one
 */
- (void)
take_cells:(NSArray<NotesMediaCell *> *)cells notes:(NSUInteger)notes scan:(NotesMediaScan *)scan
{
    if (scan != self.scan || scan.cancelled) return;
    self.n_notes = (NSInteger)notes;
    if (cells.count > 0) {
        NSUInteger first = self.cells.count;
        [self.cells addObjectsFromArray:cells];
        NSMutableSet<NSIndexPath *> *paths = [NSMutableSet set];
        for (NSUInteger i = first; i < self.cells.count; i++)
            [paths addObject:[NSIndexPath indexPathForItem:(NSInteger)i inSection:0]];
        [self.grid insertItemsAtIndexPaths:paths];
        NSInteger shown = on_image_viewer_index(self.viewer);
        if (shown >= 0 && (NSUInteger)shown + 1 >= first) on_image_viewer_nav_sync(self.viewer);
    }
    [self show_status];
}

/*
 * finish_scan:truncated: — the worker is done.
 *
 * Inputs:
 *   scan      — the scan that finished; ignored unless it is the current one
 *   truncated — YES when it stopped at media_max_images
 */
- (void)
finish_scan:(NotesMediaScan *)scan truncated:(BOOL)truncated
{
    if (scan != self.scan) return;
    self.truncated = truncated;
    self.scanning  = NO;
    [self.spinner stopAnimation:nil];
    self.spinner.hidden = YES;
    [self show_status];
}

/*
 * show_status — the status line from the counts (GTK media_status_update).
 */
- (void)
show_status
{
    NSUInteger n = self.cells.count;
    if (self.scanning)
        self.status.stringValue = [NSString stringWithFormat:@"Scanning… %lu image%@ so far",
                                   (unsigned long)n, n == 1 ? @"" : @"s"];
    else if (n == 0)
        self.status.stringValue = [NSString stringWithFormat:@"No images in “%@”", self.scope];
    else
        self.status.stringValue = [NSString stringWithFormat:@"%lu image%@ in %ld note%@%@",
                                   (unsigned long)n, n == 1 ? @"" : @"s", (long)self.n_notes,
                                   self.n_notes == 1 ? @"" : @"s",
                                   self.truncated ? @" — more were found than can be shown; browse a smaller "
                                                     "folder to see the rest" : @""];
}

/* ── Grid ──────────────────────────────────────────────────────────────── */

/*
 * collectionView:numberOfItemsInSection: — the cells built so far.
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
    return (NSInteger)self.cells.count;
}

/*
 * collectionView:itemForRepresentedObjectAtIndexPath: — a cell: its
 * thumbnail, caption and tooltip.
 *
 * Inputs:
 *   cv   — the grid
 *   path — which cell
 *
 * Output: the item.
 */
- (NSCollectionViewItem *)
collectionView:(NSCollectionView *)cv itemForRepresentedObjectAtIndexPath:(NSIndexPath *)path
{
    NotesMediaItem *item = [cv makeItemWithIdentifier:@"media" forIndexPath:path];
    NotesMediaCell *c    = self.cells[(NSUInteger)path.item];
    item.imageView.image      = c.thumb;
    item.caption.stringValue  = c.title;
    item.view.toolTip = [NSString stringWithFormat:@"%@\nImage %d of %d — click to view", c.title, c.ord + 1, c.n_img];
    return item;
}

/* ── Window ────────────────────────────────────────────────────────────── */

/*
 * windowWillClose: — stop the scan, and keep the size for the next media
 * window.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    self.scan.cancelled = YES;
    self.scanning = NO;
    on_image_viewer_close(self.viewer);
    NSSize size = self.window.contentView.frame.size;
    on_app_config_set_size("media_win_w", "media_win_h", (gint)size.width, (gint)size.height);
}

@end

/* ── Command ────────────────────────────────────────────────────────────── */

/*
 * run_media — "media": a media browser over the notes the library lists.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_media(id target, id sender)
{
    NotesMediaWindow *m = [[NotesMediaWindow alloc] init_with_lib:target];
    on_ui_keep_until_closed(m);
    [m showWindow:nil];
    [m start_scan];
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t media_handlers[] = {
    { @"media", run_media, NULL },
};

/*
 * on_media_install — see media_window.h.
 */
void
on_media_install(NotesLibrary *lib)
{
    on_library_add_commands(media_handlers, sizeof media_handlers / sizeof *media_handlers);
}
