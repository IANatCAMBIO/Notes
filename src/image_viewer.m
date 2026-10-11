/* ===========================================================================
 * image_viewer.m — the modal image viewer (work package D)
 *
 * Specification: porting/gtk_reference/src/image_viewer.c.  One picture
 * shown big over its host view, on a dark backdrop:
 *
 *   ┌─────────────────────────────────────────────┐
 *   │                                             │
 *   │          ┌───────────────────────┐          │ ← fitted to the host,
 *   │          │        picture        │          │   never enlarged
 *   │          └───────────────────────┘          │
 *   │          caption, key hint     action link  │ ← as wide as the picture
 *   │               Previous  |  Next             │ ← only with 2+ pictures
 *   │                                             │
 *   └─────────────────────────────────────────────┘
 *
 *   layout  — frames, computed here (arrange): the picture's view is sized
 *             to the fitted picture, so the caption row under it is exactly
 *             as wide as the picture and the link sits under its bottom-right
 *             corner; the group is centred in the host.  The host is asked
 *             for the picture again once a resize has settled, since it may
 *             decode to the box it is offered.
 *   clicks  — the words are plain labels, hit-tested here (mouseDown:): the
 *             Previous | Next row splits at its MIDLINE (left half back,
 *             right half forward — no dead gaps between small words), the
 *             action link runs the host's action, and a press anywhere else
 *             closes the viewer.  A step past either end does nothing — no
 *             wrap-around — and that word is dimmed.  The hand cursor shows
 *             over exactly what a click would act on.
 *   focus   — the viewer never takes the keyboard: the host window offers
 *             each key to on_image_viewer_key_down first and swallows the
 *             rest while it is open.
 * =========================================================================== */

#import "image_viewer.h"

#import "ui_util.h"

#include "look.h"

/* Key codes the viewer acts on. */
static const unsigned short key_escape = 53;
static const unsigned short key_left   = 123;
static const unsigned short key_right  = 124;

/* Quiet after a resize before the host is asked for the picture again (s). */
static const double resize_settle_s = 0.15;

/* The longest caption, in characters, before it truncates (GTK: 60). */
static const NSUInteger caption_max_chars = 60;

@interface NotesImageViewer ()
@property (nonatomic, assign) NSInteger    idx;              /* -1 = closed          */
@property (nonatomic, strong) NSImageView *picture;
@property (nonatomic, strong) NSTextField *caption;
@property (nonatomic, strong) NSTextField *link;             /* nil = no action      */
@property (nonatomic, strong) NSView      *nav;              /* Previous | Next      */
@property (nonatomic, strong) NSTextField *nav_prev;
@property (nonatomic, strong) NSTextField *nav_next;
@property (nonatomic, strong) NSTextField *nav_bar;          /* the dim "|"          */
@property (nonatomic, assign) NSSize       fitted;           /* the picture's size   */
@property (nonatomic, assign) NSSize       fit_box;          /* the box it fitted    */
@property (nonatomic, strong) NSTimer     *resize_timer;
@property (nonatomic, strong) id           frame_observer;   /* the host's frame     */
@end

/*
 * viewer_label — a small label in the viewer's light text colour.
 *
 * Inputs:
 *   text — the text
 *
 * Output: the label (frame-laid-out by the viewer).
 */
static NSTextField *
viewer_label(NSString *text)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font      = [NSFont systemFontOfSize:on_look_ui_font];
    label.textColor = on_ui_color(on_look_viewer_text);
    return label;
}

/*
 * link_look — show a word as live (blue, underlined) or as a dead end
 * (dimmed).  Only the look: mouseDown: decides what a click does.
 *
 * Inputs:
 *   label — the word
 *   live  — YES when a click on it does something
 */
static void
link_look(NSTextField *label, BOOL live)
{
    NSDictionary *attrs = live
        ? @{ NSFontAttributeName: label.font,
             NSForegroundColorAttributeName: on_ui_color(on_look_viewer_link),
             NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle) }
        : @{ NSFontAttributeName: label.font,
             NSForegroundColorAttributeName: [on_ui_color(on_look_viewer_text)
                                              colorWithAlphaComponent:on_look_viewer_dim_alpha] };
    label.attributedStringValue = [[NSAttributedString alloc] initWithString:label.stringValue
                                                                   attributes:attrs];
}

@implementation NotesImageViewer

/* ── NSView ──────────────────────────────────────────────────────────── */

/*
 * acceptsFirstResponder — never: the keyboard stays with the view behind.
 *
 * Output: NO.
 */
- (BOOL)
acceptsFirstResponder
{
    return NO;
}

/*
 * drawRect: — the dark backdrop.
 *
 * Inputs:
 *   dirty — the area to draw (unused: all of it)
 */
- (void)
drawRect:(NSRect)dirty
{
    [on_ui_color(on_look_viewer_backdrop) setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
}

/*
 * dealloc — stop following the host's frame.
 */
- (void)
dealloc
{
    if (_frame_observer) [NSNotificationCenter.defaultCenter removeObserver:_frame_observer];
    [_resize_timer invalidate];
}

/*
 * resizeSubviewsWithOldSize: — the host changed size: re-arrange at once
 * (the picture only ever shrinks inside its view), and ask the host for
 * the picture again once the size has settled.
 *
 * Inputs:
 *   old_size — the previous size (unused)
 */
- (void)
resizeSubviewsWithOldSize:(NSSize)old_size
{
    if (self.idx < 0) return;
    [self arrange];
    [self.resize_timer invalidate];
    __weak NotesImageViewer *wv = self;
    self.resize_timer = [NSTimer scheduledTimerWithTimeInterval:resize_settle_s repeats:NO
                                                          block:^(NSTimer *t) {
        NotesImageViewer *v = wv;
        v.resize_timer = nil;
        if (v.idx >= 0 && !NSEqualSizes([v image_box], v.fit_box)) [v render];
    }];
}

/* ── Geometry ─────────────────────────────────────────────────────────── */

/*
 * image_box — the room the picture may fill: the viewer inset on every
 * side, less the caption and navigation rows, never below the minimum.
 *
 * Output: the size.
 */
- (NSSize)
image_box
{
    const CGFloat in = on_look_viewer_inset;
    return NSMakeSize(MAX(NSWidth(self.bounds) - 2 * in, on_look_viewer_min_box),
                      MAX(NSHeight(self.bounds) - 2 * in - on_look_viewer_caption_h - on_look_viewer_nav_h,
                          on_look_viewer_min_box));
}

/*
 * arrange — lay the group out, centred: the picture at its fitted size
 * (shrunk further if the viewer has since become smaller), the caption row
 * as wide as the picture (wider only for a short picture's caption, never
 * wider than the viewer), the navigation row centred under it.
 */
- (void)
arrange
{
    NSSize  box   = [self image_box];
    CGFloat k     = MIN(MIN(box.width / MAX(self.fitted.width, 1), box.height / MAX(self.fitted.height, 1)), 1.0);
    NSSize  pic   = NSMakeSize(floor(self.fitted.width * k), floor(self.fitted.height * k));
    CGFloat link_w = self.link ? self.link.fittingSize.width : 0;
    CGFloat gap    = self.link ? on_look_viewer_gap : 0;
    CGFloat row_w  = MIN(box.width, MAX(pic.width, MIN(self.caption.fittingSize.width, box.width) + gap + link_w));
    BOOL    has_nav = !self.nav.hidden;
    CGFloat cap_h  = self.caption.fittingSize.height;
    CGFloat height = pic.height + on_look_viewer_row_gap + cap_h +
                     (has_nav ? on_look_viewer_row_gap + self.nav.frame.size.height : 0);

    CGFloat top  = floor((NSHeight(self.bounds) + height) / 2.0);            /* y up */
    CGFloat left = floor((NSWidth(self.bounds) - row_w) / 2.0);
    self.picture.frame = NSMakeRect(floor((NSWidth(self.bounds) - pic.width) / 2.0), top - pic.height,
                                    pic.width, pic.height);
    CGFloat row_y = top - pic.height - on_look_viewer_row_gap - cap_h;
    self.caption.frame = NSMakeRect(left, row_y, MAX(row_w - gap - link_w, 0), cap_h);
    self.link.frame    = NSMakeRect(left + row_w - link_w, row_y, link_w, cap_h);
    NSSize nav = self.nav.frame.size;
    [self.nav setFrameOrigin:NSMakePoint(floor((NSWidth(self.bounds) - nav.width) / 2.0),
                                         row_y - on_look_viewer_row_gap - nav.height)];
    [self.window invalidateCursorRectsForView:self];
}

/*
 * render — ask the host for the picture on show, decoded for the room on
 * offer, and lay it out; a picture the host can no longer give closes the
 * viewer.
 */
- (void)
render
{
    NSSize   box   = [self image_box];
    NSImage *image = self.image_handler ? self.image_handler(self.idx, box) : nil;
    if (image == nil || image.size.width <= 0 || image.size.height <= 0) {
        on_image_viewer_close(self);
        return;
    }
    /* As GTK: up to the picture's PIXEL size in points (a 1000 px picture
     * shows 1000 pt wide when there is room), scaled down to fit — not its
     * Retina point size, which would show it at half that. */
    NSImageRep *rep  = image.representations.firstObject;
    NSSize      full = rep.pixelsWide > 0 ? NSMakeSize(rep.pixelsWide, rep.pixelsHigh) : image.size;
    CGFloat k = MIN(MIN(box.width / full.width, box.height / full.height), 1.0);
    self.fitted  = NSMakeSize(floor(full.width * k), floor(full.height * k));
    self.fit_box = box;
    [self arrange];                       /* the view is at its size before it gets the image */
    self.picture.image = image;
}

/* ── Clicks ───────────────────────────────────────────────────────────── */

/*
 * nav_delta:live: — what a point in the Previous | Next row means: the
 * left half steps back, the right half forward (THE one reading of the
 * row, for the click and the cursor alike).
 *
 * Inputs:
 *   x    — the point's x, in the viewer
 *   live — receives whether that way leads anywhere
 *
 * Output: -1 or +1.
 */
- (NSInteger)
nav_delta:(CGFloat)x live:(BOOL *)live
{
    NSInteger delta = x < NSMidX(self.nav.frame) ? -1 : +1;
    NSInteger count = self.count_handler ? self.count_handler() : 0;
    *live = delta < 0 ? self.idx > 0 : self.idx + 1 < count;
    return delta;
}

/*
 * step: — move by delta pictures; past either end nothing happens.
 *
 * Inputs:
 *   delta — -1 back, +1 forward
 */
- (void)
step:(NSInteger)delta
{
    NSInteger to    = self.idx + delta;
    NSInteger count = self.count_handler ? self.count_handler() : 0;
    if (self.idx >= 0 && to >= 0 && to < count) on_image_viewer_open(self, to);
}

/*
 * hits:at: — is a point on a visible part of the viewer?
 *
 * Inputs:
 *   view — a label or the row, or nil
 *   p    — the point, in the viewer
 *
 * Output: YES when it is.
 */
- (BOOL)
hits:(NSView *)view at:(NSPoint)p
{
    return view != nil && !view.hidden && NSPointInRect(p, view.frame);
}

/*
 * mouseDown: — the row steps, the link runs the host's action (the
 * viewer closes first, so the host may do anything), anywhere else closes.
 * Every press counts, so a quick Next, Next is two steps.
 *
 * Inputs:
 *   event — the press
 */
- (void)
mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if ([self hits:self.nav at:p]) {
        BOOL live;
        NSInteger delta = [self nav_delta:p.x live:&live];
        if (live) [self step:delta];
        return;
    }
    if ([self hits:self.link at:p]) {
        NSInteger idx = self.idx;
        on_image_viewer_close(self);
        self.action_handler(idx);
        return;
    }
    on_image_viewer_close(self);
}

/*
 * scrollWheel: — swallowed: the view hidden behind does not scroll.
 *
 * Inputs:
 *   event — the scroll (unused)
 */
- (void)
scrollWheel:(NSEvent *)event
{
}

/*
 * resetCursorRects — the hand over the link and over the live half of the
 * row, exactly where a click acts.
 */
- (void)
resetCursorRects
{
    if (self.idx < 0) return;
    if (self.link && !self.link.hidden) [self addCursorRect:self.link.frame cursor:NSCursor.pointingHandCursor];
    if (self.nav.hidden) return;
    NSRect left, right;
    NSDivideRect(self.nav.frame, &left, &right, NSWidth(self.nav.frame) / 2.0, NSMinXEdge);
    BOOL live;
    [self nav_delta:NSMidX(left) live:&live];
    if (live) [self addCursorRect:left cursor:NSCursor.pointingHandCursor];
    [self nav_delta:NSMidX(right) live:&live];
    if (live) [self addCursorRect:right cursor:NSCursor.pointingHandCursor];
}

/* ── Building ─────────────────────────────────────────────────────────── */

/*
 * build_action:tip: — the picture's view, the caption, the action link and
 * the Previous | Next row, all laid out by frame (arrange).
 *
 * Inputs:
 *   label — the action link's text, or nil for none
 *   tip   — its tooltip, or nil
 */
- (void)
build_action:(NSString *)label tip:(NSString *)tip
{
    /* Made at a real size before it ever gets a picture (ui_util.h: THE
     * RULE); arrange sizes it to the fitted picture before render sets it. */
    self.picture = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, on_look_viewer_min_box,
                                                                 on_look_viewer_min_box)];
    self.picture.imageScaling = NSImageScaleProportionallyUpOrDown;   /* the view IS the fitted size */
    [self addSubview:self.picture];

    self.caption = viewer_label(@"");
    self.caption.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:self.caption];

    if (label) {
        self.link = viewer_label(label);
        self.link.toolTip = tip;
        [self addSubview:self.link];
    }

    self.nav_prev = viewer_label(@"Previous");
    self.nav_next = viewer_label(@"Next");
    self.nav_bar  = viewer_label(@"|");
    NSStackView *row = [NSStackView stackViewWithViews:@[self.nav_prev, self.nav_bar, self.nav_next]];
    row.spacing = on_look_viewer_nav_gap;
    row.toolTip = @"Show the previous or next image (or press the ← and → keys)";
    row.hidden  = YES;
    [row setFrameSize:NSMakeSize(row.fittingSize.width, MAX(row.fittingSize.height, on_look_viewer_nav_h))];
    self.nav = row;
    [self addSubview:row];
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
    viewer.wantsLayer = YES;             /* above the host's own layers (a scroll view's) */
    viewer.hidden = YES;
    viewer.idx    = -1;
    [over addSubview:viewer];

    /* Cover `over` whatever its size: it may be laid out by constraints from
     * a zero frame after this, and an NSScrollView tiles its own parts
     * without resizing other subviews. */
    over.postsFrameChangedNotifications = YES;
    __weak NotesImageViewer *wv = viewer;
    __weak NSView           *wo = over;
    viewer.frame_observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSViewFrameDidChangeNotification object:over queue:nil
                usingBlock:^(NSNotification *note) {
        if (wv && wo) wv.frame = wo.bounds;
    }];
    [viewer build_action:action_label tip:action_tip];
    return viewer;
}

/*
 * on_image_viewer_open — see image_viewer.h.  The caption is the host's
 * description and the viewer's own key hint (the arrows only when there
 * is somewhere to go).
 */
void
on_image_viewer_open(NotesImageViewer *viewer, NSInteger index)
{
    NSInteger count = viewer.count_handler ? viewer.count_handler() : 0;
    if (viewer == nil || index < 0 || index >= count) return;
    viewer.idx   = index;
    viewer.frame = viewer.superview.bounds;
    NSString *what = viewer.caption_handler ? viewer.caption_handler(index) : nil;
    if (what.length > caption_max_chars)
        what = [[what substringToIndex:caption_max_chars] stringByAppendingString:@"…"];
    viewer.caption.stringValue = [NSString stringWithFormat:@"%@      %@ESC to close", what ?: @"",
                                  count > 1 ? @"← → to move, " : @""];
    viewer.caption.textColor = on_ui_color(on_look_viewer_text);
    viewer.link.hidden = viewer.action_handler == nil;
    if (viewer.link) link_look(viewer.link, YES);
    link_look(viewer.nav_bar, NO);
    on_image_viewer_nav_sync(viewer);
    viewer.hidden = NO;
    [viewer render];
    viewer.needsDisplay = YES;
}

/*
 * on_image_viewer_close — see image_viewer.h.
 */
void
on_image_viewer_close(NotesImageViewer *viewer)
{
    if (viewer == nil || viewer.idx < 0) return;
    [viewer.resize_timer invalidate];
    viewer.resize_timer  = nil;
    viewer.idx           = -1;
    viewer.picture.image = nil;
    viewer.hidden        = YES;
    [viewer.window invalidateCursorRectsForView:viewer];
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
 * on_image_viewer_key_down — see image_viewer.h.  An arrow at either end
 * is taken and does nothing, so it cannot move the caret behind.
 */
BOOL
on_image_viewer_key_down(NotesImageViewer *viewer, NSEvent *event)
{
    if (!on_image_viewer_is_open(viewer)) return NO;
    switch (event.keyCode) {
    case key_escape: on_image_viewer_close(viewer); return YES;
    case key_left:   [viewer step:-1];              return YES;
    case key_right:  [viewer step:+1];              return YES;
    default:         return NO;
    }
}

/*
 * on_image_viewer_nav_sync — see image_viewer.h.  At either end the word
 * stays where it is, dimmed, so the row never changes width under the
 * pointer.
 */
void
on_image_viewer_nav_sync(NotesImageViewer *viewer)
{
    if (!on_image_viewer_is_open(viewer)) return;
    NSInteger count = viewer.count_handler ? viewer.count_handler() : 0;
    viewer.nav.hidden = count <= 1;
    if (count > 1) {
        link_look(viewer.nav_prev, viewer.idx > 0);
        link_look(viewer.nav_next, viewer.idx + 1 < count);
    }
    [viewer arrange];
}
