#import "library_state.h"
#import "library_favorites.h"
#import "library_notes.h"
#import "library_sidebar.h"
#import "library_window.h"
#import "ui_util.h"

#include <string.h>

/* How many modules may register library commands (window frame, commands,
 * sidebar, notes, settings, search, media — with room to spare). */
#define MAX_COMMAND_TABLES 12

/* The key of the property list a note drag carries. */
static NSString * const drag_key_ids  = @"ids";

/* The registered handler tables.  Static tables, kept by reference. */
static struct {
    const shared_command_handler_t *table;
    size_t                      n;
} command_tables[MAX_COMMAND_TABLES];
static size_t command_table_count;

@implementation NotesLibrary

/*
 * init_with_app: — see library_state.h.
 */
- (instancetype)
init_with_app:(OnApp *)app
{
    self = [super init];
    if (!self) return nil;
    _app      = app;
    _sel_kind = on_sb_all;
    _sel_name = on_library_sb_row_name(on_sb_all);
    return self;
}

@end

/* The app's one library state (on_library_create). */
static NotesLibrary *library;

/*
 * on_library_create — see library_state.h.
 */
NotesLibrary *
on_library_create(OnApp *app)
{
    library = [[NotesLibrary alloc] init_with_app:app];
    return library;
}

/*
 * on_library — see library_state.h.
 */
NotesLibrary *
on_library(void)
{
    return library;
}

/*
 * on_library_view — see library_state.h.
 */
on_library_view_t
on_library_view(NotesLibrary *lib)
{
    if (lib.sel_kind == on_sb_actions) return on_library_view_actions;
    return lib.grid_pref ? on_library_view_grid : on_library_view_list;
}

/*
 * on_library_in_trash_view — see library_state.h.
 */
BOOL
on_library_in_trash_view(NotesLibrary *lib)
{
    return lib.sel_kind == on_sb_trash;
}

/*
 * on_library_notes_for_selection — see library_state.h.
 */
GList *
on_library_notes_for_selection(NotesLibrary *lib)
{
    OnDatabase *db = lib.app->db;
    switch (lib.sel_kind) {
    case on_sb_tag:
        return on_db_notes_by_tag(db, lib.sel_id);
    case on_sb_pinned:
        return on_db_note_list_pinned(db);
    case on_sb_trash:
        return on_db_note_list_trashed(db);
    case on_sb_all:
    case on_sb_actions:              /* Action Items spans every note       */
    case on_sb_tags_header:          /* never selected; listed for the enum */
        return on_db_note_list_recent(db);
    }
}

/*
 * on_library_location_text — see library_state.h.
 */
NSString *
on_library_location_text(NotesLibrary *lib)
{
    NSString *location = lib.sel_kind == on_sb_tag ? [@"#" stringByAppendingString:lib.sel_name ?: @""]
                                                   : lib.sel_name ?: @"";
    gchar    *full = on_app_location_text(lib.app, location.UTF8String);
    NSString *text = @(full);
    g_free(full);
    return text;
}

/*
 * on_library_search_scope — see library_state.h.
 */
NSString *
on_library_search_scope(NotesLibrary *lib, gint64 *tag_id)
{
    BOOL tag = lib.sel_kind == on_sb_tag;
    *tag_id = tag ? lib.sel_id : 0;
    return tag ? [@"#" stringByAppendingString:lib.sel_name ?: @""] : @"All Notes";
}

/* ── The sidebar's rows ─────────────────────────────────────────────────── */

/* The fixed rows that can be favorites, in the sidebar's order. */
const on_sb_kind_t on_library_sb_fav_rows[ON_SB_FAV_ROWS] = {
    on_sb_all, on_sb_pinned, on_sb_actions,
};

/*
 * on_library_sb_row_name — see library_state.h.
 */
NSString *
on_library_sb_row_name(on_sb_kind_t kind)
{
    switch (kind) {
    case on_sb_all:         return @"All Notes";
    case on_sb_pinned:      return @"Pinned Notes";
    case on_sb_actions:     return @"Action Items";
    case on_sb_tags_header: return @"Tags";
    case on_sb_trash:       return @"Trash";
    case on_sb_tag:         return nil;      /* the tag's own name          */
    }
}

/*
 * on_library_sb_row_label — see library_state.h.
 */
NSString *
on_library_sb_row_label(on_sb_kind_t kind)
{
    NSString *emoji = nil;
    switch (kind) {
    case on_sb_all:         emoji = @"🔮";  break;
    case on_sb_pinned:      emoji = @"📌";  break;
    case on_sb_actions:     emoji = @"❗";  break;
    case on_sb_trash:       emoji = @"🗑️"; break;
    case on_sb_tags_header:                  /* a heading: no emoji        */
    case on_sb_tag:         break;
    }
    NSString *name = on_library_sb_row_name(kind);
    return name == nil ? nil : shared_ui_tag_label(emoji, name);
}

/*
 * on_library_sb_row_shown — see library_state.h.
 */
BOOL
on_library_sb_row_shown(NotesLibrary *lib, on_sb_kind_t kind)
{
    OnDatabase *db = lib.app->db;
    gint        n_actions = 0, n_open = 0;
    switch (kind) {
    case on_sb_all:
        return YES;
    case on_sb_pinned:
        return on_db_note_count_pinned(db) > 0;
    case on_sb_actions:
        on_db_action_counts(db, &n_actions, &n_open);
        return n_actions > 0;
    case on_sb_trash:
        return on_db_trash_count(db) > 0;
    case on_sb_tags_header:
    case on_sb_tag:
        return NO;
    }
}

/*
 * on_library_sb_fav_id — see library_state.h.
 */
gint64
on_library_sb_fav_id(on_sb_ref_t ref)
{
    switch (ref.kind) {
    case on_sb_all:
    case on_sb_pinned:
    case on_sb_actions:
        return -(gint64)ref.kind - 1;        /* -1, -2, -3: never a tag's  */
    case on_sb_tag:
        return ref.ident;
    case on_sb_tags_header:
    case on_sb_trash:
        return 0;
    }
}

/*
 * on_library_sb_fav_ref — see library_state.h.
 */
on_sb_ref_t
on_library_sb_fav_ref(gint64 fav_id)
{
    for (size_t i = 0; i < ON_SB_FAV_ROWS; i++) {
        on_sb_ref_t row = { on_library_sb_fav_rows[i], 0 };
        if (on_library_sb_fav_id(row) == fav_id) return row;
    }
    on_sb_ref_t tag = { on_sb_tag, fav_id };
    return tag;
}

/* ── Refreshing ─────────────────────────────────────────────────────────── */

/*
 * on_library_refresh_all — see library_state.h.
 */
void
on_library_refresh_all(NotesLibrary *lib)
{
    if (!lib.window) return;
    /* The bar's buttons first: the notes refresh that follows (here, or
     * the sidebar's fallback to All Notes) lights the selected one. */
    on_library_favorites_refresh(lib);
    if (!on_library_sidebar_refresh(lib)) on_library_notes_refresh(lib);
}

/*
 * on_library_add_commands — see library_state.h.
 */
void
on_library_add_commands(const shared_command_handler_t *table, size_t n)
{
    NSCAssert(command_table_count < MAX_COMMAND_TABLES, @"raise MAX_COMMAND_TABLES");
    command_tables[command_table_count].table = table;
    command_tables[command_table_count].n     = n;
    command_table_count++;
}

/*
 * acts_on_library — does a command act on what the library window shows,
 * so that from the menu bar with the window closed it must open it first?
 * (The view and sidebar toggles and Media act on the window itself.)
 *
 * Inputs:
 *   sender — the menu or toolbar item
 *
 * Output: YES for those commands.
 */
static BOOL
acts_on_library(id sender)
{
    static NSSet<NSString *> *idents;
    if (idents == nil)
        idents = [NSSet setWithArray:@[@"view-list", @"view-grid", @"toggle-view",
                                       @"toggle-sidebar", @"toggle-favorites", @"toggle-done", @"media"]];
    NSString *ident = shared_command_ident(sender);
    return ident != nil && [idents containsObject:ident];
}

/*
 * on_library_run_command — see library_state.h.  A command that acts on
 * the library window opens it first when it is not on screen (GTK: the
 * library window is always there).
 */
BOOL
on_library_run_command(NotesLibrary *lib, id sender)
{
    if (!lib.window.visible && acts_on_library(sender)) on_library_window_show(lib);
    for (size_t i = 0; i < command_table_count; i++) {
        if (shared_command_run(command_tables[i].table, command_tables[i].n, lib, sender)) return YES;
    }
    return NO;
}

/*
 * on_library_validate_command — see library_state.h.
 */
BOOL
on_library_validate_command(NotesLibrary *lib, id item)
{
    for (size_t i = 0; i < command_table_count; i++) {
        BOOL handled = NO;
        BOOL enabled = shared_command_validate(command_tables[i].table, command_tables[i].n,
                                           lib, item, &handled);
        if (handled) return enabled;
    }
    return NO;
}

/*
 * on_library_sb_ref_value — see library_state.h.
 */
NSValue *
on_library_sb_ref_value(on_sb_kind_t kind, gint64 ident)
{
    on_sb_ref_t ref = { kind, ident };
    return [NSValue valueWithBytes:&ref objCType:@encode(on_sb_ref_t)];
}

/*
 * on_library_sb_ref — see library_state.h.
 */
BOOL
on_library_sb_ref(id sender, on_sb_ref_t *ref)
{
    if (![sender isKindOfClass:NSMenuItem.class]) return NO;
    id value = ((NSMenuItem *)sender).representedObject;
    if (![value isKindOfClass:NSValue.class] ||
        strcmp([value objCType], @encode(on_sb_ref_t)) != 0) return NO;
    [value getValue:ref size:sizeof *ref];
    return YES;
}

/*
 * on_library_drag_item — see library_state.h.
 */
NSPasteboardItem *
on_library_drag_item(NSArray<NSNumber *> *ids)
{
    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setPropertyList:@{ drag_key_ids: ids } forType:on_pasteboard_type_rows];
    return item;
}

/*
 * on_library_drag_read — see library_state.h.
 */
NSArray<NSNumber *> *
on_library_drag_read(NSPasteboard *pasteboard)
{
    /* A table writes one item per dragged row: gather the ids of all.    */
    NSMutableArray<NSNumber *> *all = nil;
    for (NSPasteboardItem *item in pasteboard.pasteboardItems) {
        NSDictionary *plist = [item propertyListForType:on_pasteboard_type_rows];
        if (![plist isKindOfClass:NSDictionary.class]) continue;
        NSArray *ids = plist[drag_key_ids];
        if (![ids isKindOfClass:NSArray.class]) continue;
        if (all == nil) all = [NSMutableArray array];
        [all addObjectsFromArray:ids];
    }
    return all;
}
