/*
 * library_favorites.m — the library's Favorites bar (library_favorites.h).
 *
 * A horizontal scroll view holding a strip of SharedFavoriteButtons, one
 * per favorite tag; a vertical wheel gesture pans the strip, as Tasks' bar
 * does.  The buttons are rebuilt from one tag query on each refresh (there
 * are few), in the order the user dragged them into (the favorites_order
 * setting), new favorites at the end by name.
 */

#import "library_favorites.h"

#import "library_commands.h"
#import "library_sidebar.h"
#import "look.h"
#import "ui_util.h"

#include "config.h"
#include "db.h"
#include "shared_order.h"

/* The setting holding the bar's order: tag ids, comma separated. */
static const char * const order_key = "favorites_order";

/* What the bar says while no tag is a favorite. */
static NSString * const empty_hint =
    @"No favorites yet — right-click a tag in the sidebar and choose Add to Favorites";

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
clicked:(SharedFavoriteButton *)sender
{
    NotesLibrary *lib = self.lib;
    if (lib != nil) on_library_sidebar_select(lib, on_sb_tag, sender.tag_id);
}

/*
 * edit_tag: — a button menu's Edit Tag…: the panel for the tag the item
 * carries.
 *
 * Inputs:
 *   sender — the menu item; its representedObject is the tag's id
 */
- (void)
edit_tag:(NSMenuItem *)sender
{
    NotesLibrary *lib = self.lib;
    if (lib != nil) on_library_edit_tag(lib, [sender.representedObject longLongValue]);
}

/*
 * menu_for: — a right-click on a favorite: Edit Tag… for its tag, and
 * Remove from Favorites (the remove-favorite command, carrying the tag).
 *
 * Inputs:
 *   button — the button
 *
 * Output: the menu.
 */
- (NSMenu *)
menu_for:(SharedFavoriteButton *)button
{
    NSMenu     *menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *item = [menu addItemWithTitle:@"Edit Tag…" action:@selector(edit_tag:) keyEquivalent:@""];
    item.target            = self;
    item.representedObject = @(button.tag_id);
    [menu addItem:shared_command_menu_item(@"remove-favorite",
                                           on_library_sb_ref_value(on_sb_tag, button.tag_id))];
    return menu;
}

/*
 * save_order: — a button was dragged into place: remember the row's order
 * (the favorites_order setting).
 *
 * Inputs:
 *   order — the buttons, left to right
 */
- (void)
save_order:(NSArray<SharedFavoriteButton *> *)order
{
    int64_t *ids = g_new(int64_t, order.count + 1);
    for (NSUInteger i = 0; i < order.count; i++) ids[i] = order[i].tag_id;
    char *text = shared_order_text(ids, (int)order.count);
    on_app_config_set(order_key, text);
    free(text);
    g_free(ids);
}

/*
 * reload — the buttons from the database, in the saved order, the
 * selected tag's lit.
 */
- (void)
reload
{
    NotesLibrary *lib = self.lib;
    if (lib == nil || lib.app->db == NULL) return;
    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    __weak NotesFavoritesBar *ws = self;
    NSMutableArray<SharedFavoriteButton *> *buttons = [NSMutableArray array];
    /* The tags Notes shows (the sidebar's): a favorite no note carries,
     * Tasks' alone or gone, has no button here.                         */
    GList *tags = on_db_tag_list(lib.app->db, ON_TAGS_CARRIED);
    for (GList *l = tags; l != NULL; l = l->next) {
        const OnTag *t = l->data;
        if (!t->favorite) continue;
        SharedFavoriteButton *btn = [[SharedFavoriteButton alloc]
                                       init_with_title:shared_ui_tag_label(@(t->emoji), @(t->name))
                                                tag_id:t->id];
        btn.target       = self;
        btn.action       = @selector(clicked:);
        btn.reordered    = ^(NSArray<SharedFavoriteButton *> *order) { [ws save_order:order]; };
        btn.context_menu = ^NSMenu *(SharedFavoriteButton *button) { return [ws menu_for:button]; };
        btn.state        = lib.sel_kind == on_sb_tag && lib.sel_id == t->id ? NSControlStateValueOn
                                                                            : NSControlStateValueOff;
        [buttons addObject:btn];
    }
    on_db_tag_list_free(tags);

    /* By name from the database; the saved order first, the rest after. */
    int64_t *ids = g_new(int64_t, buttons.count + 1);
    for (NSUInteger i = 0; i < buttons.count; i++) ids[i] = buttons[i].tag_id;
    gchar *saved = on_app_config_get(order_key);
    int   *order = shared_order_permutation(ids, (int)buttons.count, saved);
    if (order != NULL) {
        NSArray<SharedFavoriteButton *> *by_name = buttons.copy;
        for (NSUInteger i = 0; i < by_name.count; i++) buttons[i] = by_name[(NSUInteger)order[i]];
    }
    free(order);
    g_free(saved);
    g_free(ids);

    for (SharedFavoriteButton *btn in buttons) [self.strip addSubview:btn];
    self.content_w   = shared_ui_place_favorites(buttons, NO);
    self.hint.hidden = buttons.count > 0;
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
        [bar.hint.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:2 * shared_look_favorites_pad],
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
