#import "image_viewer.h"

/*
 * image_viewer.m — STUB (work package D replaces this file).
 *
 * A viewer that never opens, so its hosts can be written and linked before
 * it exists.  See image_viewer.h for the contract.
 */

@implementation NotesImageViewer
@end

/*
 * on_image_viewer_new — see image_viewer.h.
 */
NotesImageViewer *
on_image_viewer_new(NSView *over, NSString *action_label, NSString *action_tip)
{
    NotesImageViewer *viewer = [[NotesImageViewer alloc] initWithFrame:over.bounds];
    viewer.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    viewer.hidden           = YES;
    [over addSubview:viewer];
    return viewer;
}

/*
 * on_image_viewer_open — see image_viewer.h (stub: stays closed).
 */
void
on_image_viewer_open(NotesImageViewer *viewer, NSInteger index)
{
}

/*
 * on_image_viewer_close — see image_viewer.h (stub: never open).
 */
void
on_image_viewer_close(NotesImageViewer *viewer)
{
}

/*
 * on_image_viewer_is_open — see image_viewer.h (stub: never open).
 */
BOOL
on_image_viewer_is_open(NotesImageViewer *viewer)
{
    return NO;
}

/*
 * on_image_viewer_index — see image_viewer.h (stub: closed).
 */
NSInteger
on_image_viewer_index(NotesImageViewer *viewer)
{
    return -1;
}

/*
 * on_image_viewer_key_down — see image_viewer.h (stub: takes no key).
 */
BOOL
on_image_viewer_key_down(NotesImageViewer *viewer, NSEvent *event)
{
    return NO;
}

/*
 * on_image_viewer_nav_sync — see image_viewer.h (stub: nothing to redraw).
 */
void
on_image_viewer_nav_sync(NotesImageViewer *viewer)
{
}
