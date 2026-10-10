/*
 * media_window.m — the media browser window.
 *
 * Shows every embedded image across the notes the library is currently
 * listing, as a flow grid of thumbnails decoded at on_look_media_thumb ×
 * on_look_media_thumb points.  Clicking a thumbnail (or Return on the
 * selected one) opens NotesImageViewer over the grid, with the image
 * decoded afresh at the panel's size; the viewer takes the window's keys
 * first while it is up, and its action link opens the source note in an
 * editor.
 *
 * The scan runs on the main thread at open time: for each note returned by
 * on_library_notes_for_selection it loads the BNBF blob, counts images with
 * on_note_count_images, then records (note_id, image_ordinal) pairs.
 * Thumbnails are decoded lazily in collectionView:itemForRepresentedObject:
 * using on_ui_thumbnail, which is safe on any thread.
 *
 * The module registers the "media" command at startup (on_media_install).
 */

#import "media_window.h"

#import "activation.h"
#import "image_viewer.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "commands.h"
#include "db.h"
#include "editor_window.h"
#include "library_state.h"
#include "serialize.h"

/* ── Types ──────────────────────────────────────────────────────────────── */

/* One image's address inside the note set. */
typedef struct {
    gint64 note_id;
    gint   ordinal;  /* 0-based index within the note */
} media_entry_t;

/* ── Collection item ────────────────────────────────────────────────────── */

static NSString * const kMediaItemID = @"MediaItem";

/*
 * NotesMediaItem — a single thumbnail cell in the collection view.
 */
@interface NotesMediaItem : NSCollectionViewItem
@end

@implementation NotesMediaItem

/*
 * loadView — build the cell: an image view sized to the thumbnail constant.
 */
- (void)
loadView
{
    CGFloat s = (CGFloat)on_look_media_thumb;
    NSImageView *iv = on_ui_image_view(nil, s, s);
    iv.imageScaling = NSImageScaleProportionallyUpOrDown;
    self.view = iv;
    self.imageView = iv;
}

@end

/* ── Window controller ──────────────────────────────────────────────────── */

@interface NotesMediaWindow : NSWindowController
    <NSWindowDelegate, NSCollectionViewDataSource>
- (instancetype)initWithLibrary:(NotesLibrary *)lib;
@end

@implementation NotesMediaWindow {
    NotesLibrary        *_lib;
    NotesActivatingGrid *_collection;
    NSTextField         *_status;
    NotesImageViewer    *_viewer;

    /* Parallel arrays: all images found across the selection. */
    media_entry_t       *_entries;     /* malloc'd, freed in dealloc */
    NSUInteger           _count;

    /* Per-entry grid thumbnail cache (index → NSImage or NSNull); the
     * viewer's full-size images are never cached here. */
    NSMutableArray      *_thumb_cache;
}

/*
 * initWithLibrary: — scan the current selection and show the window.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the window controller.
 */
- (instancetype)
initWithLibrary:(NotesLibrary *)lib
{
    NotesKeyFilterWindow *win = [[NotesKeyFilterWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, on_look_media_w, on_look_media_h)
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = @"Media Browser";
    win.releasedWhenClosed = NO;
    win.minSize = NSMakeSize(300, 200);
    [win center];

    self = [super initWithWindow:win];
    if (!self) return nil;

    _lib = lib;
    win.delegate = self;

    [self scan_library];
    [self build_ui];

    __weak NotesMediaWindow *ws = self;
    win.key_filter = ^BOOL(NSEvent *event) {
        NotesMediaWindow *s = ws;
        if (!s || !on_image_viewer_is_open(s->_viewer)) return NO;
        on_image_viewer_key_down(s->_viewer, event);
        return YES;                  /* modal: the grid behind sees no key */
    };

    [win makeKeyAndOrderFront:nil];
    return self;
}

/*
 * dealloc — release the malloc'd entries array.
 */
- (void)
dealloc
{
    free(_entries);
}

/* ── Scan ───────────────────────────────────────────────────────────────── */

/*
 * scan_library — walk every note in the current selection, load its blob,
 * and record (note_id, ordinal) for each embedded image.
 *
 * Populates _entries and _count.  Heavy notes are expected to be rare;
 * scanning happens once at open time.
 */
- (void)
scan_library
{
    GList *notes = on_library_notes_for_selection(_lib);
    OnApp *app = _lib.app;
    if (!notes || !app || !app->db) {
        on_db_note_list_free(notes);
        return;
    }

    /* First pass: count total images so we can allocate once. */
    NSUInteger total = 0;
    for (GList *l = notes; l; l = l->next) {
        OnNoteMeta *meta = l->data;
        gsize len = 0;
        guint8 *blob = on_db_note_load(app->db, meta->id, &len);
        if (!blob) continue;
        gint n = on_note_count_images(blob, (gsize)len);
        g_free(blob);
        if (n > 0) total += (NSUInteger)n;
    }

    if (total == 0) {
        on_db_note_list_free(notes);
        return;
    }

    _entries = malloc(sizeof(media_entry_t) * total);
    if (!_entries) { on_db_note_list_free(notes); return; }

    /* Second pass: fill entries. */
    NSUInteger idx = 0;
    for (GList *l = notes; l && idx < total; l = l->next) {
        OnNoteMeta *meta = l->data;
        gsize len = 0;
        guint8 *blob = on_db_note_load(app->db, meta->id, &len);
        if (!blob) continue;
        gint n = on_note_count_images(blob, (gsize)len);
        g_free(blob);
        for (gint o = 0; o < n && idx < total; o++) {
            _entries[idx].note_id = meta->id;
            _entries[idx].ordinal = o;
            idx++;
        }
    }
    _count = idx;
    _thumb_cache = [[NSMutableArray alloc] initWithCapacity:_count];
    for (NSUInteger i = 0; i < _count; i++) [_thumb_cache addObject:[NSNull null]];

    on_db_note_list_free(notes);
}

/* ── UI ─────────────────────────────────────────────────────────────────── */

/*
 * build_ui — lay out the collection view, image viewer, and status label.
 */
- (void)
build_ui
{
    /* Flow layout */
    NSCollectionViewFlowLayout *layout = [[NSCollectionViewFlowLayout alloc] init];
    CGFloat thumb = (CGFloat)on_look_media_thumb;
    layout.itemSize = NSMakeSize(thumb, thumb);
    layout.minimumInteritemSpacing = 4;
    layout.minimumLineSpacing = 4;
    layout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);

    _collection = [[NotesActivatingGrid alloc] init];
    _collection.collectionViewLayout = layout;
    _collection.dataSource      = self;
    _collection.selectable      = YES;
    _collection.activate_clicks = 1;     /* one click opens, as in GTK */
    __weak NotesMediaWindow *ws = self;
    _collection.activate_handler = ^(NSSet<NSIndexPath *> *paths) {
        NotesMediaWindow *s = ws;
        NSIndexPath *path = paths.anyObject;
        if (s && path) on_image_viewer_open(s->_viewer, path.item);
    };
    [_collection registerClass:[NotesMediaItem class]
            forItemWithIdentifier:kMediaItemID];

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView = _collection;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;

    _status = [NSTextField labelWithString:[self status_text]];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

    NSStackView *sv = [NSStackView stackViewWithViews:@[scroll, _status]];
    sv.orientation = NSUserInterfaceLayoutOrientationVertical;
    sv.alignment   = NSLayoutAttributeLeading;
    sv.spacing = 6;
    sv.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                           forOrientation:NSLayoutConstraintOrientationVertical];

    NSView *content = self.window.contentView;
    [content addSubview:sv];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [sv.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [sv.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [sv.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [sv.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
    ]];

    /* Image viewer overlay, covering the scroll view. */
    _viewer = on_image_viewer_new(scroll, @"Show in source note",
                                  @"Open the note this image is from");

    _viewer.count_handler = ^NSInteger {
        NotesMediaWindow *s = ws;
        return s ? (NSInteger)s->_count : 0;
    };
    _viewer.image_handler = ^NSImage *(NSInteger idx, NSSize box) {
        return [ws decode_at:(NSUInteger)idx box:box];
    };
    _viewer.caption_handler = ^NSString *(NSInteger idx) {
        return [ws caption_at:(NSUInteger)idx];
    };
    _viewer.action_handler = ^(NSInteger idx) {
        [ws open_source_note_at:(NSUInteger)idx];
    };
}

/*
 * status_text — generate the status bar string.
 *
 * Output: e.g. "12 images across 4 notes" or "No images".
 */
- (NSString *)
status_text
{
    if (_count == 0) return @"No images";
    char buf[80];
    snprintf(buf, sizeof buf, "%zu image%s",
             (size_t)_count, _count == 1 ? "" : "s");
    return @(buf);
}

/* ── Image helpers ──────────────────────────────────────────────────────── */

/*
 * decode_at:box: — decode one image from its note's stored blob, capped
 * at a box at the window's backing scale.  Nothing is cached.
 *
 * Inputs:
 *   idx — the flat entry index
 *   box — the bounding box in points
 *
 * Output: the image, or nil when the entry is gone or does not decode.
 */
- (NSImage *)
decode_at:(NSUInteger)idx box:(NSSize)box
{
    OnApp *app = _lib.app;
    if (idx >= _count || !app || !app->db) return nil;

    media_entry_t *e = &_entries[idx];
    gsize len = 0;
    guint8 *blob = on_db_note_load(app->db, e->note_id, &len);
    if (!blob) return nil;
    GBytes *png = on_note_image_nth_png(blob, len, e->ordinal);
    g_free(blob);
    if (!png) return nil;

    gsize plen = 0;
    gconstpointer pdata = g_bytes_get_data(png, &plen);
    NSImage *img = on_ui_thumbnail((const uint8_t *)pdata, (size_t)plen, box.width, box.height,
                                   self.window.backingScaleFactor ?: 1.0);
    g_bytes_unref(png);
    return img;
}

/*
 * thumbnail_at: — the grid's thumbnail of one image, decoded once.
 *
 * Inputs:
 *   idx — the flat entry index
 *
 * Output: the image, or nil when it does not decode.
 */
- (NSImage *)
thumbnail_at:(NSUInteger)idx
{
    if (idx >= _count) return nil;
    id cached = _thumb_cache[idx];
    if (![cached isKindOfClass:NSNull.class]) return cached;
    CGFloat  thumb = (CGFloat)on_look_media_thumb;
    NSImage *img   = [self decode_at:idx box:NSMakeSize(thumb, thumb)];
    if (img) _thumb_cache[idx] = img;
    return img;
}

/*
 * caption_at: — the viewer caption for one image.
 *
 * Inputs:
 *   idx — the entry index
 *
 * Output: "Image N of M in note" or similar.
 */
- (NSString *)
caption_at:(NSUInteger)idx
{
    if (idx >= _count) return @"";
    char buf[64];
    snprintf(buf, sizeof buf, "Image %zu of %zu", idx + 1, (size_t)_count);
    return @(buf);
}

/*
 * open_source_note_at: — open the note that contains the image at idx.
 *
 * Inputs:
 *   idx — the entry index
 */
- (void)
open_source_note_at:(NSUInteger)idx
{
    if (idx >= _count) return;
    on_editor_window_open(_lib.app, _entries[idx].note_id,
                          NotesEditorRevealImage, _entries[idx].ordinal, nil);
}

/* ── NSCollectionViewDataSource ─────────────────────────────────────────── */

/*
 * numberOfItemsInSection: — the total image count.
 *
 * Inputs:
 *   cv      — unused
 *   section — unused
 *
 * Output: _count.
 */
- (NSInteger)
collectionView:(NSCollectionView *)cv
    numberOfItemsInSection:(NSInteger)section
{
    (void)cv;
    (void)section;
    return (NSInteger)_count;
}

/*
 * collectionView:itemForRepresentedObjectAtIndexPath: — provide a thumbnail cell.
 *
 * Inputs:
 *   cv        — the collection view
 *   indexPath — the item index
 *
 * Output: a NotesMediaItem with its thumbnail image set.
 */
- (NSCollectionViewItem *)
collectionView:(NSCollectionView *)cv
    itemForRepresentedObjectAtIndexPath:(NSIndexPath *)indexPath
{
    NotesMediaItem *cell = (NotesMediaItem *)[cv makeItemWithIdentifier:kMediaItemID
                                                           forIndexPath:indexPath];
    cell.imageView.image = [self thumbnail_at:(NSUInteger)indexPath.item];
    return cell;
}

/* ── NSWindowDelegate ───────────────────────────────────────────────────── */

/*
 * windowWillClose: — nothing to cancel; the window is released by ARC.
 *
 * Inputs:
 *   notification — unused
 */
- (void)
windowWillClose:(NSNotification *)notification
{
    (void)notification;
}

@end

/* ── Command handler ────────────────────────────────────────────────────── */

/*
 * run_media — open a media browser for the current library selection.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — unused
 */
static void
run_media(id target, id sender)
{
    (void)sender;
    on_ui_keep_until_closed([[NotesMediaWindow alloc] initWithLibrary:target]);
}

/*
 * validate_media — disable if no database is open.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — unused
 *
 * Output: YES when a database is open.
 */
static BOOL
validate_media(id target, id item)
{
    (void)item;
    NotesLibrary *lib = target;
    return lib.app && lib.app->db != NULL;
}

static const on_command_handler_t media_handlers[] = {
    { @"media", run_media, validate_media },
};

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_media_install — see media_window.h.
 */
void
on_media_install(NotesLibrary *lib)
{
    on_library_add_commands(media_handlers,
                            sizeof media_handlers / sizeof *media_handlers);
    (void)lib;
}
