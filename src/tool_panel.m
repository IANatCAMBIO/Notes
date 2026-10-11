/*
 * tool_panel.m — see tool_panel.h.  Ported from the Files app's
 * tool_panel.m; the one difference is Escape, which dismisses the panel
 * (handler index -1) instead of clicking a button, so Escape never answers
 * a destructive choice.
 */
#import "tool_panel.h"
#import "ui_util.h"

#include "identity.h"
#include "look.h"

/* Panels on screen: each keeps itself alive until its window closes. */
static NSMutableSet<NotesToolPanel *> *live_panels;

/*
 * NotesToolHeroView — the solid blue background behind the hero labels.
 */
@interface NotesToolHeroView : NSView
@end

@implementation NotesToolHeroView

/*
 * isOpaque — the hero fills its whole frame.
 *
 * Output: YES.
 */
- (BOOL)
isOpaque
{
    return YES;
}

/*
 * drawRect: — fill the hero with the tool panels' blue.
 *
 * Inputs:
 *   dirtyRect — area to redraw (unused: the whole view is filled)
 */
- (void)
drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    [on_ui_color(on_look_tool_hero) setFill];
    NSRectFill(self.bounds);
}

@end

/*
 * NotesToolWindow — the panel itself: Escape closes it, which reports a
 * dismissal to the owning NotesToolPanel.
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
@property (nonatomic, strong) NSTextField     *subtitle_label;
@property (nonatomic, copy)   NSString        *subtitle;
@property (nonatomic, copy)   NSString *(^button_handler)(NSInteger index);
@property (nonatomic, copy)   void (^dismissed_handler)(void);
@property (nonatomic)         BOOL             chosen;
@end

@implementation NotesToolPanel

/*
 * build_with_title:… — the panel behind every public constructor: lay out
 * the hero strip, the content and one button per entry of button_titles.
 *
 * Inputs:
 *   window_title   — title bar text
 *   hero_title     — bold line in the hero strip
 *   subtitle       — second line in the hero strip; empty = title only
 *   symbol         — SF Symbols name for the hero icon; may be nil
 *   content        — the controls, sized by their own fittingSize
 *   button_titles  — button labels, at least one: [0] is the default button
 *                    at the far right, the rest follow to its left
 *   button_handler — called with the index of the clicked button; returns
 *                    nil to close the panel or a one-line problem to show in
 *                    the hero strip (the panel stays open)
 *
 * Output: the panel, not yet shown.
 */
+ (instancetype)
build_with_title:(NSString *)window_title
      hero_title:(NSString *)hero_title
        subtitle:(NSString *)subtitle
          symbol:(NSString *)symbol
         content:(NSView *)content
   button_titles:(NSArray<NSString *> *)button_titles
  button_handler:(NSString *(^)(NSInteger index))button_handler
{
    NotesToolPanel *p = [[NotesToolPanel alloc] init];
    p.subtitle        = subtitle;
    p.button_handler  = button_handler;

    NSMutableArray<NSButton *> *buttons = [NSMutableArray array];
    CGFloat buttons_w = 2 * on_look_tool_btn_margin - on_look_tool_btn_gap;
    for (NSUInteger i = 0; i < button_titles.count; i++) {
        NSButton *b = [NSButton buttonWithTitle:button_titles[i] target:p
                                         action:@selector(button_clicked:)];
        b.tag  = (NSInteger)i;
        b.font = [NSFont systemFontOfSize:on_look_ui_font];
        b.translatesAutoresizingMaskIntoConstraints = NO;
        [buttons addObject:b];
        buttons_w += on_look_tool_btn_gap +
                     MAX(on_look_tool_btn_w, b.fittingSize.width + on_look_tool_btn_pad);
    }

    [content layoutSubtreeIfNeeded];
    NSSize  cs = content.fittingSize;
    CGFloat w  = MAX(MAX(on_look_tool_min_w, cs.width + 2 * on_look_tool_pad), buttons_w);
    CGFloat h  = on_look_tool_hero_h + on_look_tool_pad + cs.height + on_look_tool_pad +
                 1.0 + on_look_tool_bar_h;

    NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskUtilityWindow;
    NotesToolWindow *panel = [[NotesToolWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                                                styleMask:mask
                                                                  backing:NSBackingStoreBuffered
                                                                    defer:NO];
    panel.title                  = window_title;
    panel.floatingPanel          = YES;
    panel.hidesOnDeactivate      = NO;
    panel.becomesKeyOnlyIfNeeded = NO;
    panel.releasedWhenClosed     = NO;
    panel.delegate               = p;
    p.panel                      = panel;
    NSView *cv = panel.contentView;

    /* Hero strip */
    NotesToolHeroView *hero = [[NotesToolHeroView alloc] init];
    hero.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:hero];

    NSImageView *icon = [[NSImageView alloc] init];
    NSImage *sym = symbol ? [NSImage imageWithSystemSymbolName:symbol
                                      accessibilityDescription:nil] : nil;
    if (sym) {
        icon.image = [sym imageWithSymbolConfiguration:
            [NSImageSymbolConfiguration configurationWithPointSize:on_look_tool_icon
                                                            weight:NSFontWeightMedium]];
    }
    icon.contentTintColor = NSColor.whiteColor;
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    [hero addSubview:icon];

    NSTextField *title = [NSTextField labelWithString:hero_title];
    title.font          = [NSFont boldSystemFontOfSize:on_look_ui_font];
    title.textColor     = NSColor.whiteColor;
    title.lineBreakMode = NSLineBreakByTruncatingMiddle;
    title.translatesAutoresizingMaskIntoConstraints = NO;
    [hero addSubview:title];

    NSTextField *sub = [NSTextField labelWithString:subtitle];
    sub.font          = [NSFont systemFontOfSize:on_look_ui_font];
    sub.lineBreakMode = NSLineBreakByTruncatingTail;
    sub.translatesAutoresizingMaskIntoConstraints = NO;
    [hero addSubview:sub];
    p.subtitle_label = sub;
    sub.hidden = subtitle.length == 0;
    [p show_subtitle:subtitle problem:NO];

    [NSLayoutConstraint activateConstraints:@[
        [hero.topAnchor      constraintEqualToAnchor:cv.topAnchor],
        [hero.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor],
        [hero.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor],
        [hero.heightAnchor   constraintEqualToConstant:on_look_tool_hero_h],

        [icon.leadingAnchor  constraintEqualToAnchor:hero.leadingAnchor constant:on_look_tool_hero_pad],
        [icon.centerYAnchor  constraintEqualToAnchor:hero.centerYAnchor],
        [icon.widthAnchor    constraintEqualToConstant:on_look_tool_icon],
        [icon.heightAnchor   constraintEqualToConstant:on_look_tool_icon],

        [title.leadingAnchor  constraintEqualToAnchor:icon.trailingAnchor constant:10.0],
        [title.trailingAnchor constraintEqualToAnchor:hero.trailingAnchor constant:-on_look_tool_hero_pad],
        [title.bottomAnchor   constraintEqualToAnchor:hero.centerYAnchor
                                             constant:subtitle.length ? -on_look_tool_hero_gap / 2.0
                                                                      : on_look_ui_font / 2.0],

        [sub.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [sub.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [sub.topAnchor      constraintEqualToAnchor:hero.centerYAnchor
                                           constant:on_look_tool_hero_gap / 2.0],
    ]];

    /* Button bar: right-aligned, the default button rightmost.  No button
     * takes Escape: it dismisses the panel (NotesToolWindow). */
    buttons.firstObject.keyEquivalent = @"\r";
    for (NSButton *b in buttons) [cv addSubview:b];

    NSBox *sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:sep];

    content.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:content];

    NSButton *right = nil;
    for (NSButton *b in buttons) {
        [NSLayoutConstraint activateConstraints:@[
            [b.trailingAnchor constraintEqualToAnchor:right ? right.leadingAnchor
                                                            : cv.trailingAnchor
                                             constant:right ? -on_look_tool_btn_gap
                                                            : -on_look_tool_btn_margin],
            [b.bottomAnchor   constraintEqualToAnchor:cv.bottomAnchor
                                             constant:-(on_look_tool_bar_h - on_look_tool_btn_h) / 2.0],
            [b.widthAnchor    constraintEqualToConstant:MAX(on_look_tool_btn_w,
                                                            b.fittingSize.width + on_look_tool_btn_pad)],
            [b.heightAnchor   constraintEqualToConstant:on_look_tool_btn_h],
        ]];
        right = b;
    }

    [NSLayoutConstraint activateConstraints:@[
        [sep.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor],
        [sep.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor],
        [sep.bottomAnchor   constraintEqualToAnchor:cv.bottomAnchor constant:-on_look_tool_bar_h],
        [sep.heightAnchor   constraintEqualToConstant:1.0],

        [content.topAnchor     constraintEqualToAnchor:hero.bottomAnchor constant:on_look_tool_pad],
        [content.centerXAnchor constraintEqualToAnchor:cv.centerXAnchor],
    ]];
    return p;
}

/*
 * show_subtitle:problem: — set the hero's second line.
 *
 * Inputs:
 *   text    — line to show
 *   problem — YES: highlighted as a problem; NO: the normal subtitle style
 */
- (void)
show_subtitle:(NSString *)text problem:(BOOL)problem
{
    self.subtitle_label.stringValue = text;
    self.subtitle_label.hidden      = text.length == 0;
    self.subtitle_label.textColor   = on_ui_color(problem ? on_look_tool_hero_problem
                                                          : on_look_tool_hero_sub);
}

/*
 * button_clicked: — run the button handler: nil closes the panel, a
 * problem is shown in the hero strip and the panel stays.
 *
 * Inputs:
 *   sender — the clicked button; its tag is the index in button_titles
 */
- (void)
button_clicked:(NSButton *)sender
{
    [self show_subtitle:self.subtitle problem:NO];
    NSString *problem = self.button_handler ? self.button_handler(sender.tag) : nil;
    if (problem) {
        [self show_subtitle:problem problem:YES];
        NSBeep();
        return;
    }
    self.chosen = YES;
    [self.panel close];
}

/*
 * panel_with_title:… — see tool_panel.h.
 */
+ (instancetype)
panel_with_title:(NSString *)window_title
      hero_title:(NSString *)hero_title
        subtitle:(NSString *)subtitle
          symbol:(NSString *)symbol
         content:(NSView *)content
   primary_title:(NSString *)primary_title
 primary_handler:(NSString *(^)(void))primary_handler
{
    return [self build_with_title:window_title hero_title:hero_title subtitle:subtitle
                           symbol:symbol content:content
                    button_titles:@[primary_title, @"Cancel"]
                   button_handler:^NSString *(NSInteger index) {
        return index == 0 && primary_handler ? primary_handler() : nil;
    }];
}

/*
 * panel_with_title:…:buttons:handler: — see tool_panel.h.
 */
+ (instancetype)
panel_with_title:(NSString *)window_title
      hero_title:(NSString *)hero_title
        subtitle:(NSString *)subtitle
          symbol:(NSString *)symbol
         content:(NSView *)content
         buttons:(NSArray<NSString *> *)buttons
         handler:(NSString *(^)(NSInteger index))handler
{
    return [self build_with_title:window_title hero_title:hero_title subtitle:subtitle
                           symbol:symbol content:content button_titles:buttons
                   button_handler:handler];
}

/*
 * message_content — the wrapped, selectable message text in a fixed-size
 * view; messages taller than on_look_tool_msg_max_h scroll.
 *
 * Inputs:
 *   message — text to show
 *
 * Output: a view on_look_tool_msg_w wide.
 */
static NSView *
message_content(NSString *message)
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
 * show_message:… — see tool_panel.h.
 */
+ (void)
show_message:(NSString *)title
     message:(NSString *)message
      symbol:(NSString *)symbol
     buttons:(NSArray<NSString *> *)buttons
        over:(NSWindow *)parent
     handler:(void (^)(NSInteger index))handler
{
    NotesToolPanel *p = [self build_with_title:@ON_APP_NAME hero_title:title subtitle:@""
                                        symbol:symbol content:message_content(message ?: @"")
                                 button_titles:buttons
                                button_handler:^NSString *(NSInteger index) {
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
