/*
 * library_favorites.m — the library's Favorites bar (library_favorites.h).
 *
 * A horizontal scroll view holding a strip of buttons, one per favorite
 * tag; a vertical wheel gesture pans the strip, as Tasks' bar does.  The
 * buttons are rebuilt from one tag query on each refresh (there are few).
 */

#import "library_favorites.h"

#import "library_commands.h"
#import "library_sidebar.h"
#import "look.h"
#import "ui_util.h"

#include "db.h"

/* Horizontal padding around the strip and inside a button's title. */
static const CGFloat strip_pad    = 6.0;
static const CGFloat btn_text_pad = 16.0;

/* What the bar says while no tag is a favorite. */
static NSString * const empty_hint =
    @"No favorites yet — right-click a tag in the sidebar and choose Edit Tag…";

/* ── A favorite's button ─────────────────────────────────────────────── */

/*
 * NotesFavoriteButton — a button carrying the tag it stands for.  The lit
 * state is painted behind the title, since the accessory-bar bezel shows
 * its pressed look only while the mouse is held.
 */
@interface NotesFavoriteButton : NSButton
@property (nonatomic, assign) gint64        tag_id;
@property (nonatomic, weak)   NotesLibrary *lib;
@end

@implementation NotesFavoriteButton

/*
 * setState: — mark the button for redrawing: the lit background follows.
 *
 * Inputs:
 *   state — the new state
 */
- (void)
setState:(NSControlStateValue)state
{
    [super setState:state];
    [self setNeedsDisplay:YES];
}

/*
 * drawRect: — the lit background (rounded, inside the bezel) under the
 * button's own drawing.
 *
 * Inputs:
 *   dirty — the area to draw
 */
- (void)
drawRect:(NSRect)dirty
{
    if (self.state == NSControlStateValueOn) {
        [[NSColor.labelColor colorWithAlphaComponent:0.12] setFill];
        NSRect inner = [self.cell drawingRectForBounds:self.bounds];
        NSRect lit   = NSMakeRect(NSMinX(self.bounds), NSMinY(inner), NSWidth(self.bounds), NSHeight(inner));
        [[NSBezierPath bezierPathWithRoundedRect:lit xRadius:4.0 yRadius:4.0] fill];
    }
    [super drawRect:dirty];
}

/*
 * edit_tag: — the menu's Edit Tag…: the panel for this button's tag.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
edit_tag:(id)sender
{
    NotesLibrary *lib = self.lib;
    if (lib != nil) on_library_edit_tag(lib, self.tag_id);
}

/*
 * menuForEvent: — a right-click on a favorite: Edit Tag… for its tag, and
 * Remove from Favorites (the toggle-favorite command, carrying the tag).
 *
 * Inputs:
 *   event — the right mouse-down (unused)
 *
 * Output: the menu.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    NSMenu     *menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *item = [menu addItemWithTitle:@"Edit Tag…" action:@selector(edit_tag:) keyEquivalent:@""];
    item.target = self;
    [menu addItem:shared_command_menu_item(@"toggle-favorite",
                                           on_library_sb_ref_value(on_sb_tag, self.tag_id))];
    return menu;
}

@end

/* ── The scroll view ─────────────────────────────────────────────────── */

/*
 * NotesFavoritesScroll — a scroll view that turns a vertical wheel gesture
 * into horizontal panning, so a two-finger swipe moves the strip without
 * a visible scroller.
 */
@interface NotesFavoritesScroll : NSScrollView
@end

@implementation NotesFavoritesScroll

/*
 * scrollWheel: — pan horizontally when the vertical delta dominates;
 * otherwise AppKit's own handling.
 *
 * Inputs:
 *   event — the wheel event
 */
- (void)
scrollWheel:(NSEvent *)event
{
    CGFloat dx = event.scrollingDeltaX, dy = event.scrollingDeltaY;
    if (fabs(dy) <= fabs(dx)) {
        [super scrollWheel:event];
        return;
    }
    NSPoint origin = self.contentView.bounds.origin;
    CGFloat max_x  = NSWidth(self.documentView.frame) - NSWidth(self.contentView.bounds);
    origin.x -= dy * (event.hasPreciseScrollingDeltas ? 1.0 : 10.0);
    origin.x  = MAX(0.0, MIN(origin.x, MAX(0.0, max_x)));
    [self.contentView scrollToPoint:origin];
    [self reflectScrolledClipView:self.contentView];
}

@end

/* ── The bar ─────────────────────────────────────────────────────────── */

/*
 * NotesFavoritesBar — the strip, its hint and the rule along its bottom.
 */
@interface NotesFavoritesBar : NSView
@property (nonatomic, weak)   NotesLibrary         *lib;
@property (nonatomic, strong) NotesFavoritesScroll *scroll;
@property (nonatomic, strong) NSView               *strip;
@property (nonatomic, strong) NSTextField          *hint;
@property (nonatomic, assign) CGFloat               content_w;   /* buttons + padding */
@end

@implementation NotesFavoritesBar

/*
 * layout — keep the strip at least as wide as the bar, and the scroll
 * position inside it.
 */
- (void)
layout
{
    [super layout];
    CGFloat shown = NSWidth(self.scroll.bounds);
    NSRect  frame = self.strip.frame;
    frame.size.width = MAX(self.content_w, shown);
    self.strip.frame = frame;
    NSPoint origin = self.scroll.contentView.bounds.origin;
    if (origin.x > MAX(0.0, frame.size.width - shown)) {
        origin.x = MAX(0.0, frame.size.width - shown);
        [self.scroll.contentView scrollToPoint:origin];
    }
}

/*
 * clicked: — a favorite was clicked: its tag becomes the selection.
 *
 * Inputs:
 *   sender — the button
 */
- (void)
clicked:(NotesFavoriteButton *)sender
{
    NotesLibrary *lib = self.lib;
    if (lib != nil) on_library_sidebar_select(lib, on_sb_tag, sender.tag_id);
}

/*
 * reload — the buttons from the database, the selected tag's lit.
 */
- (void)
reload
{
    NotesLibrary *lib = self.lib;
    if (lib == nil || lib.app->db == NULL) return;
    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    NSFont       *font  = [NSFont systemFontOfSize:shared_look_ui_font];
    NSDictionary *attrs = @{ NSFontAttributeName: font };
    CGFloat       x     = strip_pad;
    CGFloat       y     = (shared_look_favorites_bar_h - shared_look_favorite_btn_h) / 2.0;
    GList        *tags  = on_db_tag_list(lib.app->db, ON_TAGS_ALL);
    for (GList *l = tags; l != NULL; l = l->next) {
        const OnTag *t = l->data;
        if (!t->favorite) continue;
        NSString *title = shared_ui_tag_label(@(t->emoji), @(t->name));
        CGFloat w = MAX(shared_look_favorite_btn_min_w, ceil([title sizeWithAttributes:attrs].width) + btn_text_pad);
        NotesFavoriteButton *btn = [[NotesFavoriteButton alloc]
                                      initWithFrame:NSMakeRect(x, y, w, shared_look_favorite_btn_h)];
        btn.title         = title;
        btn.bezelStyle    = NSBezelStyleAccessoryBar;
        btn.buttonType    = NSButtonTypeToggle;
        btn.imagePosition = NSNoImage;
        btn.font          = font;
        btn.target        = self;
        btn.action        = @selector(clicked:);
        btn.tag_id        = t->id;
        btn.lib           = lib;
        btn.state         = lib.sel_kind == on_sb_tag && lib.sel_id == t->id ? NSControlStateValueOn
                                                                             : NSControlStateValueOff;
        [self.strip addSubview:btn];
        x += w + shared_look_favorite_gap;
    }
    on_db_tag_list_free(tags);

    self.hint.hidden = self.strip.subviews.count > 0;
    self.content_w   = x + strip_pad;
    self.strip.frame = NSMakeRect(0, 0, MAX(self.content_w, NSWidth(self.scroll.bounds)),
                                  shared_look_favorites_bar_h);
}

@end

/* The bar of the (one) library window; its window owns it. */
static __weak NotesFavoritesBar *favorites_bar;

/*
 * on_library_favorites_new — see library_favorites.h.
 */
NSView *
on_library_favorites_new(NotesLibrary *lib)
{
    NotesFavoritesBar *bar = [[NotesFavoritesBar alloc] initWithFrame:NSZeroRect];
    bar.lib = lib;
    bar.translatesAutoresizingMaskIntoConstraints = NO;

    bar.scroll = [[NotesFavoritesScroll alloc] initWithFrame:NSZeroRect];
    bar.scroll.hasHorizontalScroller = NO;
    bar.scroll.hasVerticalScroller   = NO;
    bar.scroll.drawsBackground       = NO;
    bar.scroll.translatesAutoresizingMaskIntoConstraints = NO;
    bar.strip = [[NSView alloc] initWithFrame:NSZeroRect];
    bar.scroll.documentView = bar.strip;

    bar.hint = [NSTextField labelWithString:empty_hint];
    bar.hint.font      = [NSFont systemFontOfSize:shared_look_ui_font];
    bar.hint.textColor = NSColor.secondaryLabelColor;
    bar.hint.translatesAutoresizingMaskIntoConstraints = NO;

    /* Wider than tall, so the box is a horizontal rule.                  */
    NSBox *rule = [[NSBox alloc] initWithFrame:NSMakeRect(0, 0, 100, 1)];
    rule.boxType = NSBoxSeparator;
    rule.translatesAutoresizingMaskIntoConstraints = NO;

    /* The window sets the bar's height (0 while hidden): the strip keeps
     * its own, and the bar clips it while it slides.                     */
    bar.clipsToBounds = YES;
    for (NSView *v in @[bar.scroll, bar.hint, rule]) [bar addSubview:v];
    [NSLayoutConstraint activateConstraints:@[
        [bar.scroll.heightAnchor constraintEqualToConstant:shared_look_favorites_bar_h],
        [bar.scroll.topAnchor constraintEqualToAnchor:bar.topAnchor],
        [bar.scroll.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor],
        [bar.scroll.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor],
        [rule.topAnchor constraintEqualToAnchor:bar.scroll.bottomAnchor],
        [rule.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor],
        [rule.heightAnchor constraintEqualToConstant:1.0],
        [bar.hint.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:2 * strip_pad],
        [bar.hint.centerYAnchor constraintEqualToAnchor:bar.scroll.centerYAnchor],
    ]];
    favorites_bar = bar;
    [bar reload];
    return bar;
}

/*
 * on_library_favorites_refresh — see library_favorites.h.
 */
void
on_library_favorites_refresh(NotesLibrary *lib)
{
    [favorites_bar reload];
}
