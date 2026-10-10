#import <Cocoa/Cocoa.h>

/*
 * tag_popup — the list of known #tags shown under the caret while one is
 * being typed (GTK: the note view's tag popover).  A borderless child
 * panel that never takes the keyboard focus: the note view keeps the keys
 * and moves the selection (Up/Down) or chooses (Return/Tab) itself; a click
 * on a row chooses it.  Main thread only.
 */
@interface NotesTagPopup : NSObject

/* Called with the chosen tag's name (no '#') when a row is clicked. */
@property (nonatomic, copy) void (^chosen_handler)(NSString *name);

/*
 * show_names:under:of: — list the names (the first selected) under a
 * rectangle of a view, or hide the list when there are none.
 *
 * Inputs:
 *   names — tag names without '#', in display order
 *   rect  — where the tag starts, in the view's coordinates
 *   view  — the view (its window becomes the list's parent)
 */
- (void)show_names:(NSArray<NSString *> *)names under:(NSRect)rect of:(NSView *)view;

/*
 * move_selection: — select the row `delta` away, kept within the list.
 *
 * Inputs:
 *   delta — +1 for the next row, -1 for the previous
 */
- (void)move_selection:(NSInteger)delta;

/*
 * selected_name — the selected row's name.
 *
 * Output: the name, or nil when the list is not on screen.
 */
- (NSString *)selected_name;

/*
 * names — what the list shows.
 *
 * Output: the names, or an empty array when hidden.
 */
- (NSArray<NSString *> *)names;

/*
 * is_visible — is the list on screen?
 *
 * Output: YES while it is.
 */
- (BOOL)is_visible;

/*
 * hide — take the list off screen.
 */
- (void)hide;

@end
