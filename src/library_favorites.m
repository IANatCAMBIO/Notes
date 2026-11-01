/*
 * library_favorites.m — the library's Favorites bar (library_favorites.h).
 *
 * A horizontal scroll view holding a strip of SharedFavoriteButtons, one
 * per favorite row — the favorite views, then the favorite tags; a
 * vertical wheel gesture pans the strip, as Tasks' bar does.  The buttons
 * are rebuilt from one tag query when the rows change (with the sidebar's),
 * in the order the user dragged them into (the favorites_order setting),
 * new favorites at the end; a change of selection only relights them.
 *
 * A button carries its row's favorite id (on_library_sb_fav_id): a tag's
 * id, or a negative number standing for a view, so one strip and one saved
 * order hold both.
 */

#import "library_favorites.h"

#import "library_commands.h"
#import "library_sidebar.h"
#import "look.h"
#import "ui_util.h"

#include "config.h"
#include "db.h"
#include "shared_order.h"

/* The setting holding the bar's order: row ids (on_library_sb_fav_id),
 * comma separated. */
static const char * const order_key = "favorites_order";

/* What the bar says while nothing is a favorite. */
static NSString * const empty_hint =
    @"No favorites yet — right-click a tag or a view in the sidebar and choose Add to Favorites";

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
 * clicked: — a favorite was clicked: its row becomes the selection.
 *
 * Inputs:
 *   sender — the button
 */
- (void)
clicked:(SharedFavoriteButton *)sender
{
    NotesLibrary *lib = self.lib;
    if (lib == nil) return;
    on_sb_ref_t ref = on_library_sb_fav_ref(sender.tag_id);
    on_library_sidebar_select(lib, ref.kind, ref.ident);
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
 * menu_for: — a right-click on a favorite: Edit Tag… when it stands for a
 * tag (a view has nothing to edit), and Remove from Favorites (the
 * remove-favorite command, carrying the row).
 *
 * Inputs:
 *   button — the button
 *
 * Output: the menu.
 */
- (NSMenu *)
menu_for:(SharedFavoriteButton *)button
{
    on_sb_ref_t ref  = on_library_sb_fav_ref(button.tag_id);
    NSMenu     *menu = [[NSMenu alloc] initWithTitle:@""];
    if (ref.kind == on_sb_tag) {
        NSMenuItem *item = [menu addItemWithTitle:@"Edit Tag…" action:@selector(edit_tag:)
                                    keyEquivalent:@""];
        item.target            = self;
        item.representedObject = @(ref.ident);
    }
    [menu addItem:shared_command_menu_item(@"remove-favorite",
                                           on_library_sb_ref_value(ref.kind, ref.ident))];
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
 * update_selection — light the selected row's button.
 */
- (void)
update_selection
{
    NotesLibrary *lib = self.lib;
    if (lib == nil) return;
    on_sb_ref_t ref      = { lib.sel_kind, lib.sel_id };
    gint64      selected = on_library_sb_fav_id(ref);
    for (SharedFavoriteButton *btn in self.strip.subviews) {
        btn.state = btn.tag_id == selected ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

/*
 * button_for:title: — one button of the strip: the row's favorite id, its
 * label, and the bar's click, drag and menu behaviour.
 *
 * Inputs:
 *   fav_id — the row's id in the bar (on_library_sb_fav_id)
 *   title  — the row's label
 *
 * Output: the button, not yet placed or lit.
 */
- (SharedFavoriteButton *)
button_for:(gint64)fav_id title:(NSString *)title
{
    __weak NotesFavoritesBar *ws = self;
    SharedFavoriteButton *btn = [[SharedFavoriteButton alloc] init_with_title:title tag_id:fav_id];
    btn.target       = self;
    btn.action       = @selector(clicked:);
    btn.reordered    = ^(NSArray<SharedFavoriteButton *> *order) { [ws save_order:order]; };
    btn.context_menu = ^NSMenu *(SharedFavoriteButton *button) { return [ws menu_for:button]; };
    return btn;
}

/*
 * reload — the buttons from the settings and the database, in the saved
 * order, the selected row's lit.
 */
- (void)
reload
{
    NotesLibrary *lib = self.lib;
    if (lib == nil || lib.app->db == NULL) return;
    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    NSMutableArray<SharedFavoriteButton *> *buttons = [NSMutableArray array];
    /* The views the user put in the bar, in the sidebar's order: one the
     * sidebar is not listing (no pinned note, no action item) has no
     * button here, as a favorite tag no note carries has none.          */
    for (size_t i = 0; i < ON_SB_FAV_ROWS; i++) {
        on_sb_ref_t ref = { on_library_sb_fav_rows[i], 0 };
        if (!on_library_favorite_row(ref.kind) || !on_library_sb_row_shown(lib, ref.kind)) continue;
        [buttons addObject:[self button_for:on_library_sb_fav_id(ref)
                                      title:on_library_sb_row_label(ref.kind)]];
    }
    /* Then the tags Notes shows (the sidebar's): a favorite no note
     * carries, Tasks' alone or gone, has no button here.                */
    GList *tags = on_db_tag_list(lib.app->db, ON_TAGS_CARRIED);
    for (GList *l = tags; l != NULL; l = l->next) {
        const OnTag *t = l->data;
        if (!t->favorite) continue;
        on_sb_ref_t ref = { on_sb_tag, t->id };
        [buttons addObject:[self button_for:on_library_sb_fav_id(ref)
                                      title:shared_ui_tag_label(@(t->emoji), @(t->name))]];
    }
    on_db_tag_list_free(tags);

    /* The views, then the tags by name from the database; the saved order
     * first, the rest after.                                            */
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
    [self update_selection];
}

@end

/* ── The favorite views ──────────────────────────────────────────────── */

/*
 * fixed_row_key — the setting holding whether a fixed row is in the bar.
 *
 * Inputs:
 *   kind — the row's kind
 *
 * Output: the key, or NULL for a row that cannot be a favorite.
 */
static const char *
fixed_row_key(on_sb_kind_t kind)
{
    switch (kind) {
    case on_sb_all:         return "favorite_all_notes";
    case on_sb_pinned:      return "favorite_pinned_notes";
    case on_sb_actions:     return "favorite_action_items";
    case on_sb_tag:                          /* the database's flag        */
    case on_sb_tags_header:
    case on_sb_trash:       return NULL;
    }
}

/*
 * on_library_favorite_row — see library_favorites.h.
 */
BOOL
on_library_favorite_row(on_sb_kind_t kind)
{
    const char *key = fixed_row_key(kind);
    return key != NULL && on_app_config_get_bool(key, FALSE);
}

/*
 * on_library_favorite_row_set — see library_favorites.h.
 */
void
on_library_favorite_row_set(on_sb_kind_t kind, BOOL favorite)
{
    const char *key = fixed_row_key(kind);
    if (key != NULL) on_app_config_set(key, favorite ? "1" : "0");
}

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

/*
 * on_library_favorites_update_selection — see library_favorites.h.
 */
void
on_library_favorites_update_selection(NotesLibrary *lib)
{
    [favorites_bar update_selection];
}
