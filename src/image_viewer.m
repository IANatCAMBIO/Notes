/* ===========================================================================
 * image_viewer.m — modal full-size image viewer (work package D)
 *
 * A dark overlay over a host view.  Clicking anywhere or pressing Escape
 * closes it.  Left/Right arrows step through the host's images.  The host
 * window calls on_image_viewer_key_down first for every key while the
 * viewer is open, and swallows the key itself when this returns NO — the
 * viewer NEVER takes keyboard focus.
 *
 * Layout (bottom-anchored, viewer's coordinate system, Y up):
 *
 *   ┌─────────────────────────────────────┐
 *   │              backdrop               │ ← click anywhere → close
 *   │   ┌───────────────────────┐         │
 *   │   │       NSImageView     │         │ ← scaled to fit inset box
 *   │   └───────────────────────┘         │
 *   │  caption text     [action link]     │ ← on_look_viewer_caption_h
 *   │       Previous  |  Next             │ ← on_look_viewer_nav_h
 *   └─────────────────────────────────────┘
 *
 * See image_viewer.h for the public API.
 * =========================================================================== */

#import "image_viewer.h"
#import "ui_util.h"
#include "look.h"

/* ── Key codes ──────────────────────────────────────────────────────────── */

static const unsigned short kEscape     = 53;
static const unsigned short kLeftArrow  = 123;
static const unsigned short kRightArrow = 124;

/* ── Resize settle timer ────────────────────────────────────────────────── */

/* Milliseconds of quiet after a resize before the image is re-requested. */
static const double resize_settle_s = 0.15;

/* ── Subview tags ───────────────────────────────────────────────────────── */

static const NSInteger tag_image_view   = 1;
static const NSInteger tag_caption      = 2;
static const NSInteger tag_action_link  = 3;
static const NSInteger tag_nav_prev     = 4;
static const NSInteger tag_nav_next     = 5;

/* ── NotesImageViewer ────────────────────────────────────────────────────── */

@interface NotesImageViewer ()
@property (nonatomic, assign) NSInteger  idx;           /* -1 = closed        */
@property (nonatomic, strong) NSString  *action_label;
@property (nonatomic, strong) NSString  *action_tip;
@property (nonatomic, strong) NSTimer   *resize_timer;
@property (nonatomic, strong) id         frame_observer; /* over's frame changes */
@property (nonatomic, weak)   NSImageView   *image_view;
@property (nonatomic, weak)   NSTextField   *caption_view;
@property (nonatomic, weak)   NSButton      *action_button;
@property (nonatomic, weak)   NSTextField   *nav_prev;
@property (nonatomic, weak)   NSTextField   *nav_next;
@end

@implementation NotesImageViewer

/* ── NSView overrides ──────────────────────────────────────────────────── */

/*
 * isOpaque — not opaque (has transparent corners outside the image).
 *
 * Inputs: none
 * Output: NO.
 */
- (BOOL) isOpaque { return NO; }

/*
 * wantsLayer — YES so the viewer draws above all host subviews.
 *
 * Inputs: none
 * Output: YES.
 */
- (BOOL) wantsLayer { return YES; }

/*
 * acceptsFirstResponder — NO: the viewer never takes keyboard focus.
 *
 * Inputs: none
 * Output: NO.
 */
- (BOOL) acceptsFirstResponder { return NO; }

/*
 * drawRect: — fill with the dark backdrop colour.
 *
 * Inputs:
 *   dirty — the dirty rect (unused; we redraw the whole view)
 *
 * Output: none.
 */
- (void)
drawRect:(NSRect)dirty
{
    (void)dirty;
    on_color_t c = on_look_pick(on_look_viewer_backdrop, on_ui_is_dark(self));
    [[NSColor colorWithRed:c.r green:c.g blue:c.b alpha:c.a] setFill];
    NSRectFill(self.bounds);
}

/*
 * dealloc — stop following the covered view's frame.
 */
- (void)
dealloc
{
    if (_frame_observer) [NSNotificationCenter.defaultCenter removeObserver:_frame_observer];
}

/*
 * mouseDown: — close the viewer on any click.
 *
 * Inputs:
 *   event — the mouse event
 *
 * Output: none.
 */
- (void)
mouseDown:(NSEvent *)event
{
    (void)event;
    on_image_viewer_close(self);
}

/*
 * resizeSubviewsWithOldSize: — re-request the image after a brief settle.
 *
 * Inputs:
 *   old_size — the previous size (unused)
 *
 * Output: none.
 */
- (void)
resizeSubviewsWithOldSize:(NSSize)old_size
{
    (void)old_size;
    [super resizeSubviewsWithOldSize:old_size];
    if (self.idx < 0) return;
    [self.resize_timer invalidate];
    __weak NotesImageViewer *wv = self;
    self.resize_timer = [NSTimer scheduledTimerWithTimeInterval:resize_settle_s
                                                        repeats:NO
                                                          block:^(NSTimer *t) {
        (void)t;
        NotesImageViewer *v = wv;
        if (!v || v.idx < 0) return;
        v.resize_timer = nil;
        [v load_image_at:v.idx];
    }];
}

/* ── Internal layout helpers ────────────────────────────────────────────── */

/*
 * image_box — the rect inside the overlay that the image may fill.
 *
 * Inputs: none
 * Output: the inset rect, never smaller than on_look_viewer_min_box square.
 */
- (NSRect)
image_box
{
    const CGFloat in = on_look_viewer_inset;
    const CGFloat bh = on_look_viewer_caption_h + on_look_viewer_nav_h;
    NSRect bounds = self.bounds;
    CGFloat w = MAX(bounds.size.width  - 2 * in, on_look_viewer_min_box);
    CGFloat h = MAX(bounds.size.height - 2 * in - bh, on_look_viewer_min_box);
    CGFloat x = (bounds.size.width  - w) / 2;
    CGFloat y = bh + in;
    return NSMakeRect(x, y, w, h);
}

/*
 * layout_chrome — position caption, action button, and nav labels.
 *
 * Inputs: none  Output: none.
 */
- (void)
layout_chrome
{
    const CGFloat in  = on_look_viewer_inset;
    const CGFloat ch  = on_look_viewer_caption_h;
    const CGFloat nh  = on_look_viewer_nav_h;
    CGFloat w = self.bounds.size.width;

    self.caption_view.frame = NSMakeRect(in, nh, w / 2 - in, ch);
    self.action_button.frame = NSMakeRect(w / 2, nh, w / 2 - in, ch);

    CGFloat nav_w = (w - 2 * in) / 2;
    self.nav_prev.frame = NSMakeRect(in, 0, nav_w, nh);
    self.nav_next.frame = NSMakeRect(in + nav_w, 0, nav_w, nh);
}

/*
 * update_nav — hide the nav row when there is only one image.
 *
 * Inputs: none  Output: none.
 */
- (void)
update_nav
{
    NSInteger count = self.count_handler ? self.count_handler() : 0;
    BOOL multi = (count > 1);
    self.nav_prev.hidden = !multi;
    self.nav_next.hidden = !multi;
}

/*
 * load_image_at: — request the image and update the image view.
 *
 * Inputs:
 *   index — the image ordinal to load
 *
 * Output: none.
 */
- (void)
load_image_at:(NSInteger)index
{
    if (!self.image_handler) {
        on_image_viewer_close(self);
        return;
    }
    NSRect box  = [self image_box];
    NSImage *img = self.image_handler(index, box.size);
    if (!img) {
        on_image_viewer_close(self);
        return;
    }
    /* Resize the image view to the displayed image size (aspect-fit). */
    NSSize  img_size = img.size;
    CGFloat scale_w  = box.size.width  / img_size.width;
    CGFloat scale_h  = box.size.height / img_size.height;
    CGFloat scale    = MIN(scale_w, scale_h);
    if (scale > 1.0) scale = 1.0;              /* never upscale */
    CGFloat disp_w = img_size.width  * scale;
    CGFloat disp_h = img_size.height * scale;
    CGFloat disp_x = box.origin.x + (box.size.width  - disp_w) / 2;
    CGFloat disp_y = box.origin.y + (box.size.height - disp_h) / 2;
    self.image_view.frame = NSMakeRect(disp_x, disp_y, disp_w, disp_h);
    self.image_view.image = img;

    NSString *cap = self.caption_handler ? self.caption_handler(index) : @"";
    self.caption_view.stringValue = cap ?: @"";
    self.action_button.hidden = (self.action_handler == nil);
}

/* ── Private construction (called by on_image_viewer_new) ─────────────── */

/*
 * build_subviews_label:tip: — add chrome subviews.
 *
 * Inputs:
 *   label — action link text (nil = no link)
 *   tip   — tooltip for the action link (nil = none)
 *
 * Output: none.
 */
- (void)
build_subviews_label:(NSString *)label tip:(NSString *)tip
{
    self.action_label = label;
    self.action_tip   = tip;

    /* Image view — created with a placeholder frame; load_image_at: sets the
     * real frame.  The frame must be non-zero before setting .image per
     * ui_util.h "THE RULE". */
    NSImageView *iv = on_ui_image_view(nil, on_look_viewer_min_box, on_look_viewer_min_box);
    iv.tag = tag_image_view;
    iv.imageScaling = NSImageScaleProportionallyUpOrDown;
    [self addSubview:iv];
    self.image_view = iv;

    /* Caption */
    NSTextField *cap = [NSTextField labelWithString:@""];
    cap.tag           = tag_caption;
    cap.font          = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    cap.textColor     = NSColor.secondaryLabelColor;
    cap.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:cap];
    self.caption_view = cap;

    /* Action link — an NSButton styled as a hyperlink */
    NSButton *act = [NSButton buttonWithTitle:label ?: @""
                                       target:self
                                       action:@selector(action_clicked:)];
    act.tag              = tag_action_link;
    act.bezelStyle       = NSBezelStyleInline;
    act.buttonType       = NSButtonTypeMomentaryLight;
    act.font             = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    act.alignment        = NSTextAlignmentRight;
    act.hidden           = (label == nil);
    if (tip) act.toolTip = tip;
    [self addSubview:act];
    self.action_button = act;

    /* Nav labels — left and right halves of the nav row; click areas are
     * handled by mouseDown:, but we use NSButton for the visual tap style. */
    NSButton *prev_btn = [NSButton buttonWithTitle:@"Previous"
                                            target:self
                                            action:@selector(nav_prev_clicked:)];
    prev_btn.tag        = tag_nav_prev;
    prev_btn.bezelStyle = NSBezelStyleInline;
    prev_btn.font       = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    prev_btn.alignment  = NSTextAlignmentCenter;
    [self addSubview:prev_btn];
    self.nav_prev = (NSTextField *)(NSView *)prev_btn;

    NSButton *next_btn = [NSButton buttonWithTitle:@"Next"
                                            target:self
                                            action:@selector(nav_next_clicked:)];
    next_btn.tag        = tag_nav_next;
    next_btn.bezelStyle = NSBezelStyleInline;
    next_btn.font       = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    next_btn.alignment  = NSTextAlignmentCenter;
    [self addSubview:next_btn];
    self.nav_next = (NSTextField *)(NSView *)next_btn;
}

/* ── Nav button targets ──────────────────────────────────────────────────── */

/*
 * action_clicked: — run the host's action and close.
 *
 * Inputs:
 *   sender — the action button (unused)
 *
 * Output: none.
 */
- (void)
action_clicked:(id)sender
{
    (void)sender;
    if (!self.action_handler) return;
    NSInteger idx = self.idx;
    on_image_viewer_close(self);          /* close first, as per the contract */
    self.action_handler(idx);
}

/*
 * nav_prev_clicked: / nav_next_clicked: — step to previous / next image.
 *
 * Inputs:
 *   sender — the button (unused)
 *
 * Output: none.
 */
- (void)
nav_prev_clicked:(id)sender
{
    (void)sender;
    NSInteger count = self.count_handler ? self.count_handler() : 0;
    if (count <= 1) return;
    NSInteger next = (self.idx - 1 + count) % count;
    on_image_viewer_open(self, next);
}

- (void)
nav_next_clicked:(id)sender
{
    (void)sender;
    NSInteger count = self.count_handler ? self.count_handler() : 0;
    if (count <= 1) return;
    NSInteger next = (self.idx + 1) % count;
    on_image_viewer_open(self, next);
}

@end

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * on_image_viewer_new — see image_viewer.h.
 */
NotesImageViewer *
on_image_viewer_new(NSView *over, NSString *action_label, NSString *action_tip)
{
    NotesImageViewer *viewer = [[NotesImageViewer alloc] initWithFrame:over.bounds];
    viewer.hidden           = YES;
    viewer.idx              = -1;
    [over addSubview:viewer];

    /* Cover `over` whatever its size.  Autoresizing is not enough: `over`
     * may be laid out by constraints after this, from a zero frame, and an
     * NSScrollView tiles its own parts without resizing other subviews. */
    over.postsFrameChangedNotifications = YES;
    __weak NotesImageViewer *wv = viewer;
    __weak NSView           *wo = over;
    viewer.frame_observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSViewFrameDidChangeNotification object:over queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        if (wv && wo) wv.frame = wo.bounds;
    }];
    [viewer build_subviews_label:action_label tip:action_tip];
    return viewer;
}

/*
 * on_image_viewer_open — see image_viewer.h.
 */
void
on_image_viewer_open(NotesImageViewer *viewer, NSInteger index)
{
    viewer.idx   = index;
    viewer.frame = viewer.superview.bounds;
    [viewer load_image_at:index];
    [viewer layout_chrome];
    [viewer update_nav];
    viewer.hidden = NO;
    [viewer setNeedsDisplay:YES];
}

/*
 * on_image_viewer_close — see image_viewer.h.
 */
void
on_image_viewer_close(NotesImageViewer *viewer)
{
    [viewer.resize_timer invalidate];
    viewer.resize_timer        = nil;
    viewer.idx                 = -1;
    viewer.image_view.image    = nil;
    viewer.hidden              = YES;
}

/*
 * on_image_viewer_is_open — see image_viewer.h.
 */
BOOL
on_image_viewer_is_open(NotesImageViewer *viewer)
{
    return viewer != nil && viewer.idx >= 0;
}

/*
 * on_image_viewer_index — see image_viewer.h.
 */
NSInteger
on_image_viewer_index(NotesImageViewer *viewer)
{
    return viewer ? viewer.idx : -1;
}

/*
 * on_image_viewer_key_down — see image_viewer.h.
 */
BOOL
on_image_viewer_key_down(NotesImageViewer *viewer, NSEvent *event)
{
    if (!viewer || viewer.idx < 0) return NO;

    unsigned short kc = event.keyCode;
    if (kc == kEscape) {
        on_image_viewer_close(viewer);
        return YES;
    }
    if (kc == kLeftArrow) {
        NSInteger count = viewer.count_handler ? viewer.count_handler() : 1;
        NSInteger next  = (viewer.idx - 1 + count) % count;
        on_image_viewer_open(viewer, next);
        return YES;
    }
    if (kc == kRightArrow) {
        NSInteger count = viewer.count_handler ? viewer.count_handler() : 1;
        NSInteger next  = (viewer.idx + 1) % count;
        on_image_viewer_open(viewer, next);
        return YES;
    }
    /* Viewer is open: swallow the key so typing doesn't reach the note. */
    return YES;
}

/*
 * on_image_viewer_nav_sync — see image_viewer.h.
 */
void
on_image_viewer_nav_sync(NotesImageViewer *viewer)
{
    if (viewer && viewer.idx >= 0) [viewer update_nav];
}
