/*
 * tool_panel.m — see tool_panel.h.  Ported from the Files app's
 * tool_panel.m; Escape dismisses the panel (handler index -1) instead of
 * clicking a button, so Escape never answers a destructive choice.  The
 * same code as Tasks' tool_panel.m apart from the prefix: keep them alike.
 */
#import "tool_panel.h"
#import "ui_util.h"

#include "identity.h"
#include "look.h"

/* ── The hero strip ─────────────────────────────────────────────────── */

@interface NotesHero ()
@property (nonatomic, strong, readwrite) NSTextField *title_label;
@property (nonatomic, strong, readwrite) NSTextField *subtitle_label;
@end

/*
 * hero_label — one line of white-on-blue hero text that truncates rather
 * than widen the strip.
 *
 * Inputs:
 *   font  — its font
 *   color — its colour
 *
 * Output: new, empty label.
 */
static NSTextField *
hero_label(NSFont *font, NSColor *color)
{
    NSTextField *label = [NSTextField labelWithString:@""];

    label.font                 = font;
    label.textColor            = color;
    label.lineBreakMode        = NSLineBreakByTruncatingTail;
    label.maximumNumberOfLines = 1;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

@implementation NotesHero

/*
 * init_with_symbol: — the strip: the symbol, then the title over the
 * subtitle, centred on the strip's height.
 * Inputs/output are documented in tool_panel.h.
 */
- (instancetype)
init_with_symbol:(NSString *)symbol
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    NSImage *sym = symbol ? [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil]
                          : nil;
    sym = [sym imageWithSymbolConfiguration:
           [NSImageSymbolConfiguration configurationWithPointSize:on_look_hero_icon
                                                           weight:NSFontWeightMedium]];
    NSImageView *icon     = on_ui_image_view(sym, on_look_hero_icon, on_look_hero_icon);
    icon.contentTintColor = NSColor.whiteColor;
    icon.hidden           = sym == nil;

    _title_label    = hero_label([NSFont boldSystemFontOfSize:on_look_ui_font], NSColor.whiteColor);
    _subtitle_label = hero_label([NSFont systemFontOfSize:on_look_ui_font], on_ui_color(on_look_hero_sub));
    _subtitle_label.hidden = YES;

    /* A hidden subtitle leaves the stack, so a lone title is centred. */
    NSStackView *text = [NSStackView stackViewWithViews:@[_title_label, _subtitle_label]];
    text.orientation  = NSUserInterfaceLayoutOrientationVertical;
    text.alignment    = NSLayoutAttributeLeading;
    text.spacing      = on_look_hero_gap;

    NSStackView *row = [NSStackView stackViewWithViews:@[icon, text]];
    row.orientation  = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment    = NSLayoutAttributeCenterY;
    row.spacing      = on_look_hero_icon_gap;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:row];

    [NSLayoutConstraint activateConstraints:@[
        [self.heightAnchor constraintEqualToConstant:on_look_hero_h],
        [row.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:on_look_hero_pad],
        [row.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor
                                                     constant:-on_look_hero_pad],
        [row.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

/*
 * set_title:subtitle: — what the strip says.
 * Inputs are documented in tool_panel.h.
 */
- (void)
set_title:(NSString *)title subtitle:(NSString *)subtitle
{
    self.title_label.stringValue    = title ?: @"";
    self.subtitle_label.stringValue = subtitle ?: @"";
    self.subtitle_label.textColor   = on_ui_color(on_look_hero_sub);
    self.subtitle_label.hidden      = subtitle.length == 0;
}

/*
 * show_problem: — a highlighted line in the subtitle's place.
 * Inputs are documented in tool_panel.h.
 */
- (void)
show_problem:(NSString *)problem
{
    self.subtitle_label.stringValue = problem ?: @"";
    self.subtitle_label.textColor   = on_ui_color(on_look_hero_problem);
    self.subtitle_label.hidden      = problem.length == 0;
}

/*
 * isOpaque — the strip fills its whole frame.
 *
 * Output: YES.
 */
- (BOOL)
isOpaque
{
    return YES;
}

/*
 * drawRect: — fill the strip with the hero blue.
 *
 * Inputs:
 *   dirtyRect — the area to redraw (unused: the whole strip is filled)
 */
- (void)
drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    [on_ui_color(on_look_hero) setFill];
    NSRectFill(self.bounds);
}

@end

/* ── The panel ──────────────────────────────────────────────────────── */

/*
 * NotesToolWindow — the panel's window: Escape closes it, which reports a
 * dismissal to its NotesToolPanel.
 */
@interface NotesToolWindow : NSPanel
@end

@implementation NotesToolWindow

/*
 * cancelOperation: — Escape, from the panel or a field in it: close.
 *
 * Inputs:
 *   sender — the responder that sent it (unused)
 */
- (void)
cancelOperation:(id)sender
{
    (void)sender;
    [self close];
}

@end

@interface NotesToolPanel () <NSWindowDelegate>
@property (nonatomic, strong) NotesToolWindow *panel;
@property (nonatomic, strong) NotesHero       *hero;
@property (nonatomic, copy)   NSString        *hero_title;
@property (nonatomic, copy)   NSString        *subtitle;
@property (nonatomic, copy)   NSString *(^button_handler)(NSInteger index);
@property (nonatomic, copy)   void (^dismissed_handler)(void);
@property (nonatomic, assign) BOOL             chosen;     /* a button answered */
@end

/* Panels on screen: each keeps itself alive until its window closes. */
static NSMutableSet<NotesToolPanel *> *live_panels;

/*
 * message_view — the wrapped, selectable message in a fixed-size view;
 * text taller than on_look_tool_msg_max_h scrolls.
 *
 * Inputs:
 *   message — the text
 *
 * Output: a view on_look_tool_msg_w wide.
 */
static NSView *
message_view(NSString *message)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:message];
    label.font         = [NSFont systemFontOfSize:on_look_ui_font];
    label.selectable   = YES;
    label.preferredMaxLayoutWidth = on_look_tool_msg_w;
    CGFloat h = ceil([label.cell cellSizeForBounds:
                      NSMakeRect(0, 0, on_look_tool_msg_w, CGFLOAT_MAX)].height);
    CGFloat shown_h = MIN(h, on_look_tool_msg_max_h);

    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, on_look_tool_msg_w, shown_h)];
    if (h <= on_look_tool_msg_max_h) {
        label.frame = box.bounds;
        [box addSubview:label];
    } else {
        label.frame = NSMakeRect(0, 0, on_look_tool_msg_w, h);
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:box.bounds];
        scroll.hasVerticalScroller = YES;
        scroll.drawsBackground     = NO;
        scroll.documentView        = label;
        [box addSubview:scroll];
    }
    [NSLayoutConstraint activateConstraints:@[
        [box.widthAnchor  constraintEqualToConstant:on_look_tool_msg_w],
        [box.heightAnchor constraintEqualToConstant:shown_h],
    ]];
    return box;
}

/*
 * button_w — the width a tool-panel button is given: its title's, padded,
 * never narrower than on_look_tool_btn_w.
 *
 * Inputs:
 *   button — the button
 *
 * Output: the width in points.
 */
static CGFloat
button_w(NSButton *button)
{
    return MAX(on_look_tool_btn_w, button.fittingSize.width + on_look_tool_btn_pad);
}

@implementation NotesToolPanel

/*
 * build_with_title:… — the panel behind both public constructors: lay out
 * the hero strip, the content and one button per entry of buttons.
 *
 * Inputs:
 *   window_title — title bar text
 *   hero_title   — bold line in the hero strip
 *   subtitle     — second line in the hero strip; empty = title only
 *   symbol       — SF Symbols name for the hero icon; may be nil
 *   content      — the controls, sized by their own fittingSize
 *   buttons      — button labels, at least one: [0] is the default button
 *                  at the far right, the rest follow to its left
 *   destructive  — index of the button with the destructive look; -1 none
 *   handler      — called with the index of the clicked button; returns
 *                  nil to close the panel or a one-line problem to show in
 *                  the hero strip (the panel stays open)
 *
 * Output: the panel, not yet shown.
 */
+ (instancetype)
build_with_title:(NSString *)window_title
      hero_title:(NSString *)hero_title
        subtitle:(NSString *)subtitle
          symbol:(NSString *)symbol
         content:(NSView *)content
         buttons:(NSArray<NSString *> *)buttons
     destructive:(NSInteger)destructive
         handler:(NSString *(^)(NSInteger index))handler
{
    NotesToolPanel *p = [[NotesToolPanel alloc] init];
    p.hero_title     = hero_title;
    p.subtitle       = subtitle;
    p.button_handler = handler;

    NSMutableArray<NSButton *> *controls = [NSMutableArray array];
    CGFloat bar_w = 2 * on_look_tool_btn_margin - on_look_tool_btn_gap;
    for (NSUInteger i = 0; i < buttons.count; i++) {
        NSButton *b = [NSButton buttonWithTitle:buttons[i] target:p
                                         action:@selector(button_clicked:)];
        b.tag                  = (NSInteger)i;
        b.font                 = [NSFont systemFontOfSize:on_look_ui_font];
        b.hasDestructiveAction = (NSInteger)i == destructive;
        b.translatesAutoresizingMaskIntoConstraints = NO;
        [controls addObject:b];
        bar_w += on_look_tool_btn_gap + button_w(b);
    }
    /* No button takes Escape: it dismisses the panel (NotesToolWindow). */
    controls.firstObject.keyEquivalent = @"\r";

    [content layoutSubtreeIfNeeded];
    NSSize  cs = content.fittingSize;
    CGFloat w  = MAX(MAX(on_look_tool_min_w, cs.width + 2 * on_look_tool_pad), bar_w);
    CGFloat h  = on_look_hero_h + on_look_tool_pad + cs.height + on_look_tool_pad +
                 1.0 + on_look_tool_bar_h;

    NotesToolWindow *window = [[NotesToolWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, w, h)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title                  = window_title;
    window.floatingPanel          = YES;
    window.hidesOnDeactivate      = NO;
    window.becomesKeyOnlyIfNeeded = NO;
    window.releasedWhenClosed     = NO;
    window.delegate               = p;
    p.panel                       = window;
    NSView *cv = window.contentView;

    NotesHero *hero = [[NotesHero alloc] init_with_symbol:symbol];
    [hero set_title:hero_title subtitle:subtitle];
    p.hero = hero;
    [cv addSubview:hero];

    NSBox *rule = [[NSBox alloc] init];
    rule.boxType = NSBoxSeparator;
    rule.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:rule];

    content.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:content];

    [NSLayoutConstraint activateConstraints:@[
        [hero.topAnchor        constraintEqualToAnchor:cv.topAnchor],
        [hero.leadingAnchor    constraintEqualToAnchor:cv.leadingAnchor],
        [hero.trailingAnchor   constraintEqualToAnchor:cv.trailingAnchor],
        [content.topAnchor     constraintEqualToAnchor:hero.bottomAnchor constant:on_look_tool_pad],
        [content.centerXAnchor constraintEqualToAnchor:cv.centerXAnchor],
        [rule.leadingAnchor    constraintEqualToAnchor:cv.leadingAnchor],
        [rule.trailingAnchor   constraintEqualToAnchor:cv.trailingAnchor],
        [rule.bottomAnchor     constraintEqualToAnchor:cv.bottomAnchor constant:-on_look_tool_bar_h],
        [rule.heightAnchor     constraintEqualToConstant:1.0],
    ]];

    /* The buttons, right-aligned: [0] rightmost, the rest to its left. */
    NSButton *right = nil;
    for (NSButton *b in controls) {
        [cv addSubview:b];
        [NSLayoutConstraint activateConstraints:@[
            [b.trailingAnchor constraintEqualToAnchor:right ? right.leadingAnchor : cv.trailingAnchor
                                             constant:right ? -on_look_tool_btn_gap
                                                            : -on_look_tool_btn_margin],
            [b.bottomAnchor   constraintEqualToAnchor:cv.bottomAnchor
                                             constant:-(on_look_tool_bar_h - on_look_tool_btn_h) / 2.0],
            [b.widthAnchor    constraintEqualToConstant:button_w(b)],
            [b.heightAnchor   constraintEqualToConstant:on_look_tool_btn_h],
        ]];
        right = b;
    }
    return p;
}

/*
 * button_clicked: — run the button handler: nil closes the panel, a
 * problem is shown in the hero strip and the panel stays.
 *
 * Inputs:
 *   sender — the clicked button; its tag is the index in buttons
 */
- (void)
button_clicked:(NSButton *)sender
{
    [self.hero set_title:self.hero_title subtitle:self.subtitle];
    NSString *problem = self.button_handler ? self.button_handler(sender.tag) : nil;
    if (problem) {
        [self.hero show_problem:problem];
        NSBeep();
        return;
    }
    self.chosen = YES;
    [self.panel close];
}

/*
 * panel_with_title:…:handler: — see tool_panel.h.
 */
+ (instancetype)
panel_with_title:(NSString *)window_title
      hero_title:(NSString *)hero_title
        subtitle:(NSString *)subtitle
          symbol:(NSString *)symbol
         content:(NSView *)content
         buttons:(NSArray<NSString *> *)buttons
     destructive:(NSInteger)destructive
         handler:(NSString *(^)(NSInteger index))handler
{
    return [self build_with_title:window_title hero_title:hero_title subtitle:subtitle
                           symbol:symbol content:content buttons:buttons
                      destructive:destructive handler:handler];
}

/*
 * show_message:… — see tool_panel.h.
 */
+ (void)
show_message:(NSString *)title
     message:(NSString *)message
      symbol:(NSString *)symbol
     buttons:(NSArray<NSString *> *)buttons
 destructive:(NSInteger)destructive
        over:(NSWindow *)parent
     handler:(void (^)(NSInteger index))handler
{
    NotesToolPanel *p = [self build_with_title:@ON_APP_NAME hero_title:title subtitle:@""
                                        symbol:symbol content:message_view(message ?: @"")
                                       buttons:buttons destructive:destructive
                                       handler:^NSString *(NSInteger index) {
        if (handler) handler(index);
        return nil;
    }];
    p.dismissed_handler = ^{ if (handler) handler(-1); };
    [p show_over:parent];
}

/*
 * show_over: — see tool_panel.h.
 */
- (void)
show_over:(NSWindow *)parent
{
    if (!live_panels) live_panels = [NSMutableSet set];
    [live_panels addObject:self];
    if (parent) {
        NSRect pf = parent.frame, f = self.panel.frame;
        [self.panel setFrameOrigin:NSMakePoint(NSMidX(pf) - f.size.width / 2.0,
                                               NSMidY(pf) - f.size.height / 2.0)];
    } else {
        [self.panel center];
    }
    [NSApp activate];
    [self.panel makeKeyAndOrderFront:nil];
}

/*
 * window — see tool_panel.h.
 */
- (NSPanel *)
window
{
    return self.panel;
}

/*
 * windowWillClose: — drop the handlers (which may reference the caller's
 * objects) and the panel's self-retention, and report a close without a
 * button click to the dismissed handler.
 *
 * Inputs:
 *   note — close notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    (void)note;
    void (^dismissed)(void) = self.chosen ? nil : self.dismissed_handler;
    self.button_handler    = nil;
    self.dismissed_handler = nil;
    [live_panels removeObject:self];
    if (dismissed) dismissed();
}

@end
