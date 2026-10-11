#import <Cocoa/Cocoa.h>

#include "app_core.h"
#include "search_job.h"              /* OnSearchScope */

#import "commands.h"

/*
 * library_state — what the pieces of the library window share.
 *
 * The library window is built by several modules (work packages E to H):
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
 *   - on_library_refresh_all      after anything structural: notes or
 *                                 folders created, moved, trashed, deleted;
 *                                 the tag set or an action item changed;
 *                                 the database switched.
 *   - on_library_notes_note_saved after an editor saved ONE note's content
 *                                 (library_notes.h): that row only — the
 *                                 sidebar is not rebuilt per autosave.
 *   - while `populating` > 0 a module is rewriting its rows in code:
 *     selection-change handlers must do nothing.
 */

/* What a sidebar row is; also what kind of selection the library has. */
typedef enum {
    on_sb_root,                      /* the fixed "Notes" root row          */
    on_sb_folder,                    /* a folder                            */
    on_sb_tags_header,               /* the "Tags" header (not selectable)  */
    on_sb_tag,                       /* one tag                             */
    on_sb_pinned,                    /* "Pinned Notes"                      */
    on_sb_all,                       /* "All Notes"                         */
    on_sb_trash,                     /* "Trash"                             */
    on_sb_trash_folder,              /* a trashed folder under Trash        */
    on_sb_actions,                   /* "Action Items"                      */
} on_sb_kind_t;

/* What the notes pane shows. */
typedef enum {
    on_library_view_list,
    on_library_view_grid,
    on_library_view_actions,         /* while Action Items is selected      */
} on_library_view_t;

/* What a drag of library rows carries. */
typedef enum {
    on_drag_notes,                   /* one or more notes                   */
    on_drag_folder,                  /* one folder                          */
    on_drag_trashed_folder,          /* one folder dragged out of the Trash */
} on_drag_kind_t;

@interface NotesLibrary : NSObject

/* The application context; not owned. */
@property (nonatomic, assign, readonly) OnApp *app;

/* The library window while it is open, else nil.  Sheets attach to it. */
@property (nonatomic, weak) NSWindow *window;

/* The sidebar selection: which notes are listed.  Set by the sidebar
 * (library_sidebar.m) when the user selects a row; everyone else reads. */
@property (nonatomic, assign) on_sb_kind_t sel_kind;
@property (nonatomic, assign) gint64 sel_id;        /* folder id or tag id; 0 otherwise */
@property (nonatomic, copy) NSString *sel_name;     /* the row's bare name */

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
 * Output: the state, with the Notes root selected.
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
 * on_library_in_trash_view — is the notes pane showing Trash contents (the
 * Trash row or a trashed folder)?  Deleting there is permanent, and the
 * note menu offers Restore.
 *
 * Output: YES in a Trash view.
 */
BOOL on_library_in_trash_view(NotesLibrary *lib);

/*
 * on_library_current_folder_id — the folder a new note or folder lands in.
 *
 * Output: the selected folder's id, or 0 (the top level) when anything
 *         else is selected.
 */
gint64 on_library_current_folder_id(NotesLibrary *lib);

/*
 * on_library_notes_for_selection — the notes the selection lists, in
 * display order.  THE one mapping from a selection to a note set: the notes
 * pane, the media browser and the AI summary all use it, so they cannot
 * disagree about what "this folder" means.
 *
 * Output: a GList of OnNoteMeta *; free with on_db_note_list_free.
 */
GList *on_library_notes_for_selection(NotesLibrary *lib);

/*
 * on_library_location_text — the selection as the status bar shows it:
 * "/Folder/Sub" for a folder, the row's name for the other views, with the
 * database path in front when that preference is on.
 *
 * Output: the text.
 */
NSString *on_library_location_text(NotesLibrary *lib);

/*
 * on_library_search_scope — the selection as a search scope, for the search
 * window's "Selected Folder/Tag".
 *
 * Inputs:
 *   lib   — the library
 *   scope — receives folder or tag
 *   ident — receives the folder or tag id (0 = the root)
 *
 * Output: the scope's display name.  Selections that are neither a folder
 *         nor a tag (All Notes, Pinned, Trash, Action Items) give the root.
 */
NSString *on_library_search_scope(NotesLibrary *lib, OnSearchScope *scope, gint64 *ident);

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
void on_library_add_commands(const on_command_handler_t *table, size_t n);

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

/* ── Drags of rows (library_state.m) ────────────────────────────────────── */

/*
 * on_library_drag_item — what a dragged note selection or folder puts on
 * the pasteboard (type on_pasteboard_type_rows).  In-app only.
 *
 * Inputs:
 *   kind — what is dragged
 *   ids  — the note ids, or the one folder id
 *
 * Output: a pasteboard item for the drag.
 */
NSPasteboardItem *on_library_drag_item(on_drag_kind_t kind, NSArray<NSNumber *> *ids);

/*
 * on_library_drag_read — read a drag of rows back: the ids of every
 * pasteboard item of the first item's kind (a table drag writes one item
 * per row).
 *
 * Inputs:
 *   pasteboard — the dragging pasteboard
 *   kind       — receives what is dragged
 *
 * Output: the ids, or nil when the pasteboard holds no row drag.
 */
NSArray<NSNumber *> *on_library_drag_read(NSPasteboard *pasteboard, on_drag_kind_t *kind);
