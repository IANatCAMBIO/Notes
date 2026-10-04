#import "library_sidebar.h"

/*
 * library_sidebar.m — STUB (work package F replaces this file).
 *
 * A placeholder pane showing the database's counts, so the window has
 * something true in it and the refresh path can be seen working.
 */

/* The placeholder's label, while the library window is open. */
static __weak NSTextField *counts_label;

/*
 * on_library_sidebar_new — see library_sidebar.h.
 */
NSView *
on_library_sidebar_new(NotesLibrary *lib)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.font         = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor    = NSColor.secondaryLabelColor;
    label.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *pane = [[NSView alloc] initWithFrame:NSZeroRect];
    [pane addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.topAnchor constraintEqualToAnchor:pane.topAnchor constant:12],
        [label.leadingAnchor constraintEqualToAnchor:pane.leadingAnchor constant:12],
        [label.trailingAnchor constraintEqualToAnchor:pane.trailingAnchor constant:-12],
    ]];
    counts_label = label;
    on_library_sidebar_refresh(lib);
    return pane;
}

/*
 * on_library_sidebar_refresh — see library_sidebar.h (stub: the counts).
 */
void
on_library_sidebar_refresh(NotesLibrary *lib)
{
    OnDatabase *db = lib.app->db;
    gint notes = 0, folders = 0, tags = 0, actions = 0, open = 0;
    on_db_totals(db, &notes, &folders, &tags);
    on_db_action_counts(db, &actions, &open);
    counts_label.stringValue = [NSString stringWithFormat:
        @"Sidebar (work package F)\n\n"
        @"Notes: %d\nVisible: %d\nPinned: %d\nFolders: %d\nTags: %d\n"
        @"Action items: %d (%d open)\nIn Trash: %d",
        notes, on_db_note_count_visible(db), on_db_note_count_pinned(db), folders, tags,
        actions, open, on_db_trash_count(db)];
}

/*
 * on_library_sidebar_select — see library_sidebar.h (stub: the state only;
 * there are no rows to select).
 */
void
on_library_sidebar_select(NotesLibrary *lib, on_sb_kind_t kind, gint64 ident)
{
    lib.sel_kind = kind;
    lib.sel_id   = ident;
}

/*
 * on_library_sidebar_fit — see library_sidebar.h (stub: nothing to fit).
 */
void
on_library_sidebar_fit(NotesLibrary *lib)
{
}
