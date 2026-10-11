/*
 * activation.m — see activation.h.
 */
#import "activation.h"

/*
 * activation_is_key — is this key event Return or keypad Enter, with no
 * Command, Option or Control?
 *
 * Inputs:
 *   event — a key-down event
 *
 * Output: YES for an activating key.
 */
static BOOL
activation_is_key(NSEvent *event)
{
    NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl);
    NSString *chars = event.charactersIgnoringModifiers;
    if (mods != 0 || chars.length != 1) return NO;
    unichar c = [chars characterAtIndex:0];
    return c == '\r' || c == NSEnterCharacter;
}

@implementation NotesActivatingTable

/*
 * keyDown: — Return or Enter with a selection sends the doubleAction;
 * every other key goes to NSTableView.
 *
 * Inputs:
 *   event — the key event
 */
- (void)
keyDown:(NSEvent *)event
{
    if (activation_is_key(event) && self.selectedRowIndexes.count > 0 && self.doubleAction) {
        _by_key = YES;
        [self sendAction:self.doubleAction to:self.target];
        _by_key = NO;
        return;
    }
    [super keyDown:event];
}

@end

/*
 * on_activated_rows — see activation.h.
 */
NSIndexSet *
on_activated_rows(NotesActivatingTable *table)
{
    if (table.by_key) return table.selectedRowIndexes;
    if (table.clickedRow >= 0) return [NSIndexSet indexSetWithIndex:(NSUInteger)table.clickedRow];
    return [NSIndexSet indexSet];         /* a double-click on empty space */
}

@implementation NotesActivatingGrid

/*
 * initWithFrame: — a grid that activates on a double-click until told
 * otherwise.
 *
 * Inputs:
 *   frame — the initial frame
 *
 * Output: the grid.
 */
- (instancetype)
initWithFrame:(NSRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    _activate_clicks = 2;
    return self;
}

/*
 * mouseDown: — NSCollectionView selects; the activate_clicks-th click on
 * an item then activates it.
 *
 * Inputs:
 *   event — the mouse event
 */
- (void)
mouseDown:(NSEvent *)event
{
    [super mouseDown:event];
    if (event.clickCount != self.activate_clicks || !self.activate_handler) return;
    NSPoint      at   = [self convertPoint:event.locationInWindow fromView:nil];
    NSIndexPath *path = [self indexPathForItemAtPoint:at];
    if (path) self.activate_handler([NSSet setWithObject:path]);
}

/*
 * menuForEvent: — the item under a right click gets menu_handler's menu.
 *
 * Inputs:
 *   event — the click
 *
 * Output: the menu, or nil.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    if (!self.menu_handler) return [super menuForEvent:event];
    NSPoint at = [self convertPoint:event.locationInWindow fromView:nil];
    return self.menu_handler([self indexPathForItemAtPoint:at]);
}

/*
 * keyDown: — Return or Enter activates the selection; every other key
 * goes to NSCollectionView.
 *
 * Inputs:
 *   event — the key event
 */
- (void)
keyDown:(NSEvent *)event
{
    if (activation_is_key(event) && self.selectionIndexPaths.count > 0 && self.activate_handler) {
        self.activate_handler(self.selectionIndexPaths);
        return;
    }
    [super keyDown:event];
}

@end
