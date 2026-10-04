#import "library_notes.h"

/*
 * library_notes.m — STUB (work package G replaces this file).
 *
 * A placeholder pane naming the selection and listing its notes' titles,
 * so the selection-to-notes mapping and the refresh hooks can be seen
 * working.  Nothing can be selected in it.
 */

/* How many titles the placeholder lists. */
#define STUB_TITLE_LIMIT 30

/* The placeholder's label, while the library window is open. */
static __weak NSTextField *notes_label;

/*
 * on_library_notes_install — see library_notes.h (stub: no commands yet).
 */
void
on_library_notes_install(NotesLibrary *lib)
{
}

/*
 * on_library_notes_new — see library_notes.h.
 */
NSView *
on_library_notes_new(NotesLibrary *lib)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *pane = [[NSView alloc] initWithFrame:NSZeroRect];
    [pane addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.topAnchor constraintEqualToAnchor:pane.topAnchor constant:12],
        [label.leadingAnchor constraintEqualToAnchor:pane.leadingAnchor constant:12],
        [label.trailingAnchor constraintEqualToAnchor:pane.trailingAnchor constant:-12],
    ]];
    notes_label = label;
    on_library_notes_refresh(lib);
    return pane;
}

/*
 * on_library_notes_refresh — see library_notes.h (stub: titles as text).
 */
void
on_library_notes_refresh(NotesLibrary *lib)
{
    GList *notes = on_library_notes_for_selection(lib);
    guint count = g_list_length(notes);
    NSMutableString *text = [NSMutableString stringWithFormat:
        @"Notes pane (work package G)\n\n%@: %u note%s\n", lib.sel_name, count,
        count == 1 ? "" : "s"];
    NSInteger listed = 0;
    for (GList *l = notes; l != NULL && listed < STUB_TITLE_LIMIT; l = l->next, listed++) {
        const OnNoteMeta *meta = l->data;
        [text appendFormat:@"\n%lld  %@", (long long)meta->id, @(meta->title)];
    }
    on_db_note_list_free(notes);
    notes_label.stringValue = text;
}

/*
 * on_library_notes_note_saved — see library_notes.h (stub: a full relist).
 */
void
on_library_notes_note_saved(NotesLibrary *lib, gint64 note_id)
{
    on_library_notes_refresh(lib);
}

/*
 * on_library_notes_selected_ids — see library_notes.h (stub: no selection).
 */
NSArray<NSNumber *> *
on_library_notes_selected_ids(NotesLibrary *lib)
{
    return @[];
}
