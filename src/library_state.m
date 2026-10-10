#import "library_state.h"
#import "library_notes.h"
#import "library_sidebar.h"
#import "ui_util.h"

/* How many modules may register library commands (window frame, commands,
 * sidebar, notes, settings, search, media, AI — with room to spare). */
#define MAX_COMMAND_TABLES 12

/* Keys of the property list a row drag carries. */
static NSString * const drag_key_kind = @"kind";
static NSString * const drag_key_ids  = @"ids";

/* The registered handler tables.  Static tables, kept by reference. */
static struct {
    const on_command_handler_t *table;
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
    _sel_kind = on_sb_root;              /* the GTK app starts on the Notes root */
    _sel_name = @"Notes";
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
    return lib.sel_kind == on_sb_trash || lib.sel_kind == on_sb_trash_folder;
}

/*
 * on_library_current_folder_id — see library_state.h.
 */
gint64
on_library_current_folder_id(NotesLibrary *lib)
{
    return lib.sel_kind == on_sb_folder ? lib.sel_id : 0;
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
    case on_sb_root:
    case on_sb_folder:
    case on_sb_trash_folder:
        return on_db_note_list(db, lib.sel_id);
    }
}

/*
 * on_library_location_text — see library_state.h.
 */
NSString *
on_library_location_text(NotesLibrary *lib)
{
    NSString *location;
    switch (lib.sel_kind) {
    case on_sb_root:
    case on_sb_folder: {
        gchar *path = on_db_folder_path(lib.app->db, lib.sel_id);
        location    = [@"/" stringByAppendingString:@(path)];
        g_free(path);
        break;
    }
    case on_sb_trash_folder:
        location = [@"Trash/" stringByAppendingString:lib.sel_name ?: @""];
        break;
    default:
        location = lib.sel_name ?: @"";
        break;
    }
    gchar    *full = on_app_location_text(lib.app, location.UTF8String);
    NSString *text = @(full);
    g_free(full);
    return text;
}

/*
 * on_library_search_scope — see library_state.h.
 */
NSString *
on_library_search_scope(NotesLibrary *lib, OnSearchScope *scope, gint64 *ident)
{
    BOOL scoped = lib.sel_kind == on_sb_tag || lib.sel_kind == on_sb_folder ||
                  lib.sel_kind == on_sb_trash_folder || lib.sel_kind == on_sb_root;
    *scope = (scoped && lib.sel_kind == on_sb_tag) ? ON_SCOPE_TAG : ON_SCOPE_FOLDER;
    *ident = scoped ? lib.sel_id : 0;
    return (scoped && lib.sel_name) ? lib.sel_name : @"Notes";
}

/*
 * on_library_refresh_all — see library_state.h.
 */
void
on_library_refresh_all(NotesLibrary *lib)
{
    if (!lib.window) return;
    on_library_sidebar_refresh(lib);
    on_library_notes_refresh(lib);
}

/*
 * on_library_add_commands — see library_state.h.
 */
void
on_library_add_commands(const on_command_handler_t *table, size_t n)
{
    NSCAssert(command_table_count < MAX_COMMAND_TABLES, @"raise MAX_COMMAND_TABLES");
    command_tables[command_table_count].table = table;
    command_tables[command_table_count].n     = n;
    command_table_count++;
}

/*
 * on_library_run_command — see library_state.h.
 */
BOOL
on_library_run_command(NotesLibrary *lib, id sender)
{
    for (size_t i = 0; i < command_table_count; i++) {
        if (on_command_run(command_tables[i].table, command_tables[i].n, lib, sender)) return YES;
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
        BOOL enabled = on_command_validate(command_tables[i].table, command_tables[i].n,
                                           lib, item, &handled);
        if (handled) return enabled;
    }
    return NO;
}

/*
 * on_library_drag_item — see library_state.h.
 */
NSPasteboardItem *
on_library_drag_item(on_drag_kind_t kind, NSArray<NSNumber *> *ids)
{
    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setPropertyList:@{ drag_key_kind: @(kind), drag_key_ids: ids }
                  forType:on_pasteboard_type_rows];
    return item;
}

/*
 * on_library_drag_read — see library_state.h.
 */
NSArray<NSNumber *> *
on_library_drag_read(NSPasteboard *pasteboard, on_drag_kind_t *kind)
{
    NSDictionary *plist = [pasteboard propertyListForType:on_pasteboard_type_rows];
    if (![plist isKindOfClass:NSDictionary.class]) return nil;
    NSNumber *boxed_kind = plist[drag_key_kind];
    NSArray  *ids        = plist[drag_key_ids];
    if (![boxed_kind isKindOfClass:NSNumber.class] || ![ids isKindOfClass:NSArray.class]) return nil;
    *kind = (on_drag_kind_t)boxed_kind.integerValue;
    return ids;
}
