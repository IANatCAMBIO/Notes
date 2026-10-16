#import <Cocoa/Cocoa.h>

/*
 * activation — cards that stand for something open it on a double-click
 * and on Return / keypad Enter (porting/INTERFACES.md §11, "Activation").
 * An NSCollectionView has no double-click at all; this subclass is the
 * app's one way of adding it (a table's is SharedTable, shared_ui.h).
 * Main thread only.
 */

/*
 * NotesActivatingGrid — a collection view that reports a click on an item
 * (the activate_clicks-th: 2 by default, 1 for a grid where one click
 * opens, as in the media browser), and Return/Enter on the selection, to
 * activate_handler.
 */
@interface NotesActivatingGrid : NSCollectionView
@property (nonatomic, assign) NSInteger activate_clicks;
@property (nonatomic, copy)   void (^activate_handler)(NSSet<NSIndexPath *> *paths);
/* The menu for a right click (or Control-click) on an item, given the
 * item's path (nil for empty space); nil = no menu. */
@property (nonatomic, copy)   NSMenu *(^menu_handler)(NSIndexPath *path);
@end
