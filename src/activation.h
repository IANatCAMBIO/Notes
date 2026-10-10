#import <Cocoa/Cocoa.h>

/*
 * activation — rows and cards that stand for something open it on a
 * double-click and on Return / keypad Enter (porting/INTERFACES.md §11,
 * "Activation").  An NSTableView does nothing on either until told to and
 * an NSCollectionView has no double-click at all; these two subclasses are
 * the app's one way of adding it.  Main thread only.
 */

/*
 * on_activation_is_key — is this key event Return or keypad Enter, with no
 * Command, Option or Control?
 *
 * Inputs:
 *   event — a key-down event
 *
 * Output: YES for an activating key.
 */
BOOL on_activation_is_key(NSEvent *event);

/*
 * NotesActivatingTable — a table whose Return/Enter key sends its
 * doubleAction, as a double-click does.  The action reads which rows it
 * applies to with on_activated_rows.
 */
@interface NotesActivatingTable : NSTableView
@property (nonatomic, readonly) BOOL by_key;   /* YES while Return/Enter sends */
@end

/*
 * on_activated_rows — the rows a table's doubleAction applies to: the
 * double-clicked row, or the selection when Return/Enter sent it.
 *
 * Inputs:
 *   table — the table that sent the action
 *
 * Output: the row indexes; empty for a double-click on no row.
 */
NSIndexSet *on_activated_rows(NotesActivatingTable *table);

/*
 * NotesActivatingGrid — a collection view that reports a click on an item
 * (the activate_clicks-th: 2 by default, 1 for a grid where one click
 * opens, as in the media browser), and Return/Enter on the selection, to
 * activate_handler.
 */
@interface NotesActivatingGrid : NSCollectionView
@property (nonatomic, assign) NSInteger activate_clicks;
@property (nonatomic, copy)   void (^activate_handler)(NSSet<NSIndexPath *> *paths);
@end
