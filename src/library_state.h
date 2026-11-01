#import <Cocoa/Cocoa.h>

#include "app_core.h"

#import "commands.h"

/*
 * library_state — what the pieces of the library window share.
 *
 * The library window is built by several modules:
 * the window frame and its commands, the sidebar, the notes pane with the
 * Action Items list, the AI pane, and the secondary windows that ask the
 * library what is selected.  They never reach into each other.  What they
 * share is ONE NotesLibrary object — the sidebar selection and the view
 * mode — and the functions below, each implemented in exactly one module
 * (named at each declaration).
 *
 * There is one NotesLibrary per app, created by the app delegate and alive
 * until the app quits, whether or not the library WINDOW is open: commands
 * from the menu bar reach it with an editor in front or with no window at
 * all.
 *
 * REFRESH RULES (the GTK build's, kept):
 *   - on_library_refresh_all      after anything structural: notes
 *                                 created, trashed, deleted; a note's tags,
 *                                 a tag's emoji or favorite flag, or an
 *                                 action item changed; the database
 *                                 switched.
 *   - on_library_notes_note_saved after an editor saved ONE note's content
 *                                 (library_notes.h): that row only — the
 *                                 sidebar is not rebuilt per autosave.
 *   - while `populating` > 0 a module is rewriting its rows in code:
 *     selection-change handlers must do nothing.
 */

/* What a sidebar row is; also what kind of selection the library has.
 * In the sidebar's order. */
typedef enum {
    on_sb_all,                       /* "All Notes"                         */
    on_sb_pinned,                    /* "Pinned Notes"                      */
    on_sb_actions,                   /* "Action Items"                      */
    on_sb_tags_header,               /* the "Tags" header (not selectable)  */
    on_sb_tag,                       /* one tag                             */
    on_sb_trash,                     /* "Trash"                             */
} on_sb_kind_t;

/* A sidebar row named by a menu item: the row a context menu was opened
 * on, which need not be the selected one (a right click does not select). */
typedef struct {
    on_sb_kind_t kind;
    gint64       ident;              /* the tag id; 0 for the fixed rows    */
} on_sb_ref_t;

/* What the notes pane shows. */
typedef enum {
    on_library_view_list,
    on_library_view_grid,
    on_library_view_actions,         /* while Action Items is selected      */
} on_library_view_t;

@interface NotesLibrary : NSObject

/* The application context; not owned. */
@property (nonatomic, assign, readonly) OnApp *app;

/* The library window while it is open, else nil.  Sheets attach to it. */
@property (nonatomic, weak) NSWindow *window;

/* The sidebar selection: which notes are listed.  Set by the sidebar
 * (library_sidebar.m) when the user selects a row; everyone else reads. */
@property (nonatomic, assign) on_sb_kind_t sel_kind;
@property (nonatomic, assign) gint64 sel_id;        /* the tag id; 0 otherwise */
@property (nonatomic, copy) NSString *sel_name;     /* the row's bare name (a tag's
                                                     * name, no '#')        */

/* The user's list-or-grid choice, kept while Action Items is showing so
 * leaving it returns to the right one. */
@property (nonatomic, assign) BOOL grid_pref;

/* > 0 while a module is rebuilding its rows in code (see REFRESH RULES). */
@property (nonatomic, assign) NSInteger populating;

/*
 * init_with_app: — the library state for an app.
 *
 * Inputs:
 *   app — the application context, its database open
 *
 * Output: the state, with All Notes selected.
 */
- (instancetype)init_with_app:(OnApp *)app NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

/*
 * on_library_create — make the app's one library state.  Called once, by
 * the app delegate, after the database is open.
 *
 * Inputs:
 *   app — the application context
 *
 * Output: the library; it lives until the app quits.
 */
NotesLibrary *on_library_create(OnApp *app);

/*
 * on_library — the app's library state: how an OnApp hook, which is handed
 * only the OnApp, finds it.
 *
 * Output: the library, or nil before on_library_create.
 */
NotesLibrary *on_library(void);

/* ── Reading the state (library_state.m) ────────────────────────────────── */

/*
 * on_library_view — what the notes pane shows for the current selection.
 *
 * Output: the Action Items list while that row is selected, else the
 *         user's list or grid.
 */
on_library_view_t on_library_view(NotesLibrary *lib);

/*
 * on_library_in_trash_view — is the notes pane showing the Trash?
 * Deleting there is permanent, and the note menu offers Restore.
 *
 * Output: YES in a Trash view.
 */
BOOL on_library_in_trash_view(NotesLibrary *lib);

/*
 * on_library_notes_for_selection — the notes the selection lists, in
 * display order.  THE one mapping from a selection to a note set: the notes
 * pane, the media browser and the AI summary all use it, so they cannot
 * disagree about what "this tag" means.
 *
 * Output: a GList of OnNoteMeta *; free with on_db_note_list_free.
 */
GList *on_library_notes_for_selection(NotesLibrary *lib);

/*
 * on_library_location_text — the selection as the status bar shows it:
 * "#tag" for a tag, the row's name for the other views, with the database
 * path in front when that preference is on.
 *
 * Output: the text.
 */
NSString *on_library_location_text(NotesLibrary *lib);

/*
 * on_library_search_scope — the selection as a search scope, for the search
 * window's "Selected Tag".
 *
 * Inputs:
 *   lib    — the library
 *   tag_id — receives the selected tag's id, or 0 (every note) when no tag
 *            is selected
 *
 * Output: the scope's display name: "#tag", or "All Notes".
 */
NSString *on_library_search_scope(NotesLibrary *lib, gint64 *tag_id);

/* ── The sidebar's rows (library_state.m) ───────────────────────────────── */

/*
 * on_library_sb_row_name — a fixed row's bare name ("All Notes", "Pinned
 * Notes", "Action Items", "Tags", "Trash"), defined ONCE here for the
 * sidebar's rows, the Favorites bar's buttons and the first selection.
 *
 * Inputs:
 *   kind — the row's kind
 *
 * Output: the name, or nil for a tag (a tag's name is its own).
 */
NSString *on_library_sb_row_name(on_sb_kind_t kind);

/*
 * on_library_sb_row_label — a fixed row as the sidebar and the Favorites
 * bar show it: its emoji, two spaces and its name
 * (shared_ui_tag_label), the name alone for the Tags header.
 *
 * Inputs:
 *   kind — the row's kind
 *
 * Output: the label, or nil for a tag.
 */
NSString *on_library_sb_row_label(on_sb_kind_t kind);

/*
 * on_library_sb_row_shown — does the sidebar list this fixed row now?
 * All Notes always; Pinned Notes with a pinned note, Action Items with an
 * action item, the Trash with a trashed note.  The sidebar builds its rows
 * with this, and the Favorites bar leaves out the button of a row the
 * sidebar is not listing, as it leaves out a favorite tag no note carries.
 *
 * Inputs:
 *   lib  — the library, its database open
 *   kind — the row's kind
 *
 * Output: YES while the row is listed; NO for a tag or the Tags header,
 *         which follow the tags the notes carry (on_db_tag_list).
 */
BOOL on_library_sb_row_shown(NotesLibrary *lib, on_sb_kind_t kind);

/* The fixed rows that can be favorites — go in the Favorites bar beside
 * the tags — in the sidebar's order. */
#define ON_SB_FAV_ROWS 3
extern const on_sb_kind_t on_library_sb_fav_rows[ON_SB_FAV_ROWS];

/*
 * on_library_sb_fav_id — the id a Favorites bar button carries for a row:
 * a tag's own id, or a NEGATIVE number standing for one of the fixed rows
 * above, so a view and a tag can never collide in the bar's order (the
 * favorites_order setting).
 *
 * Inputs:
 *   ref — the row
 *
 * Output: the id, or 0 for a row that cannot be a favorite (the Tags
 *         header, the Trash).
 */
gint64 on_library_sb_fav_id(on_sb_ref_t ref);

/*
 * on_library_sb_fav_ref — the row a Favorites bar button's id stands for:
 * on_library_sb_fav_id read back.
 *
 * Inputs:
 *   fav_id — the button's id
 *
 * Output: the row; a positive id is a tag's, whether or not that tag
 *         still exists.
 */
on_sb_ref_t on_library_sb_fav_ref(gint64 fav_id);

/* ── Refreshing (library_state.m, calling the panes) ────────────────────── */

/*
 * on_library_refresh_all — rebuild the sidebar and the notes pane from the
 * database, keeping the selection where it still exists.  A no-op while the
 * library window is closed (it is rebuilt when it opens).
 */
void on_library_refresh_all(NotesLibrary *lib);

/* ── Commands (library_state.m) ─────────────────────────────────────────── */

/*
 * on_library_add_commands — register a module's library commands (see
 * commands.h).  Handlers receive the NotesLibrary as their target.  Called
 * once per module, at startup, from its install function.
 *
 * Inputs:
 *   table — a STATIC handler table (kept by reference)
 *   n     — how many handlers
 */
void on_library_add_commands(const shared_command_handler_t *table, size_t n);

/*
 * on_library_run_command — run a library command.  Called by the app
 * delegate (app scope) and the library window's controller (window scope).
 *
 * Inputs:
 *   lib    — the library
 *   sender — the menu or toolbar item
 *
 * Output: YES when a registered handler ran.
 */
BOOL on_library_run_command(NotesLibrary *lib, id sender);

/*
 * on_library_validate_command — whether an item's library command can run
 * now.  An item no module has a handler for is disabled.
 *
 * Inputs:
 *   lib  — the library
 *   item — the item being validated
 *
 * Output: YES to enable it.
 */
BOOL on_library_validate_command(NotesLibrary *lib, id item);

/*
 * on_library_sb_ref_value — a sidebar row as a menu item's
 * representedObject, so a context menu's commands act on the row they
 * were opened on.
 *
 * Inputs:
 *   kind  — the row's kind
 *   ident — its tag id, 0 for the fixed rows
 *
 * Output: the value to set on the item.
 */
NSValue *on_library_sb_ref_value(on_sb_kind_t kind, gint64 ident);

/*
 * on_library_sb_ref — the sidebar row an item names, if any.  Handlers of
 * the sidebar's menu commands ask this first and fall back to the
 * library's selection (the menu bar's items carry no row).
 *
 * Inputs:
 *   sender — the menu item, or any other sender
 *   ref    — receives the row when the item carries one
 *
 * Output: YES when the item carries a row.
 */
BOOL on_library_sb_ref(id sender, on_sb_ref_t *ref);

/* ── Drags of rows (library_state.m) ────────────────────────────────────── */

/*
 * on_library_drag_item — what a dragged note selection puts on the
 * pasteboard (type on_pasteboard_type_rows).  In-app only.
 *
 * Inputs:
 *   ids — the note ids
 *
 * Output: a pasteboard item for the drag.
 */
NSPasteboardItem *on_library_drag_item(NSArray<NSNumber *> *ids);

/*
 * on_library_drag_read — read a drag of notes back: the ids of every
 * pasteboard item (a table drag writes one item per row).
 *
 * Inputs:
 *   pasteboard — the dragging pasteboard
 *
 * Output: the ids, or nil when the pasteboard holds no note drag.
 */
NSArray<NSNumber *> *on_library_drag_read(NSPasteboard *pasteboard);
