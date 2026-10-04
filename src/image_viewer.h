#import <Cocoa/Cocoa.h>

/*
 * image_viewer — the modal image viewer: ONE picture shown big over
 * whatever its host window was showing.  Used by the note editor (a click
 * on an embedded image) and the media browser (a thumbnail click); both get
 * the identical panel, keys and links.
 *
 *   +-----------------------------------+
 *   |                                   |   click anywhere        -> close
 *   |          +-------------+          |   Escape                -> close
 *   |          |    image    |          |   Left / Right          -> step
 *   |          +-------------+          |   "Previous | Next"     -> step
 *   |  caption            [action link] |   the action link       -> host
 *   |          Previous | Next          |
 *   +-----------------------------------+
 *
 * The viewer is a view laid over a host view (the editor's scroll view, the
 * media grid), covering it completely and swallowing every click.  It NEVER
 * becomes first responder: the view behind keeps the keyboard, and the host
 * window offers each key to the viewer first (on_image_viewer_key_down) and
 * swallows the rest while it is open.
 *
 * The viewer knows nothing about where pictures come from.  The host sets
 * four handler blocks and addresses its images by INDEX — the editor by the
 * note's image ordinal, the media browser by grid position.  count_handler
 * is asked every time, so a host whose set is still growing needs only
 * on_image_viewer_nav_sync.  The blocks must capture the host weakly: the
 * host owns the viewer.
 *
 * (The GTK build's render op returned a GdkPaintable; here it returns an
 * NSImage, so no Core Foundation ownership crosses a block.)
 *
 * STATUS: image_viewer.m is a STUB for work package D.  The specification
 * is porting/gtk_reference/src/image_viewer.c; the geometry constants are
 * in look.h (on_look_viewer_*).  This header is FROZEN: the media window
 * (package H) is written against it.
 */
@interface NotesImageViewer : NSView

/* How many images the viewer may walk, right now.  0 or 1 hides the
 * Previous | Next row. */
@property (nonatomic, copy) NSInteger (^count_handler)(void);

/* The image at an index, for a box of the given size in points: at full
 * resolution if the host has it, since the viewer only scales DOWN.  nil
 * when the image is gone or will not decode — the viewer then closes. */
@property (nonatomic, copy) NSImage *(^image_handler)(NSInteger index, NSSize box);

/* A one-line description of the image at an index (the viewer appends its
 * own key hint). */
@property (nonatomic, copy) NSString *(^caption_handler)(NSInteger index);

/* Run the action link for the image at an index.  The viewer has ALREADY
 * closed itself when this is called.  nil = no action link. */
@property (nonatomic, copy) void (^action_handler)(NSInteger index);

@end

/*
 * on_image_viewer_new — build a viewer over a host view.  It starts hidden.
 *
 * Inputs:
 *   over         — the view it covers; the viewer is added to it as a
 *                  subview that tracks its size
 *   action_label — the action link's text (e.g. @"Show in source note");
 *                  nil for no link
 *   action_tip   — its tooltip, or nil
 *
 * Output: the viewer (owned by `over` as its subview).
 */
NotesImageViewer *on_image_viewer_new(NSView *over, NSString *action_label,
                                      NSString *action_tip);

/*
 * on_image_viewer_open — show the image at an index, replacing whatever was
 * showing.  An index out of range, or one whose image_handler returns nil,
 * leaves the viewer closed.
 */
void on_image_viewer_open(NotesImageViewer *viewer, NSInteger index);

/*
 * on_image_viewer_close — hide the viewer and drop the image it holds.
 */
void on_image_viewer_close(NotesImageViewer *viewer);

/*
 * on_image_viewer_is_open — is a picture showing?
 *
 * Output: YES while one is.  nil-safe: NO for a nil viewer.
 */
BOOL on_image_viewer_is_open(NotesImageViewer *viewer);

/*
 * on_image_viewer_index — the index showing.
 *
 * Output: the index, or -1 when closed.
 */
NSInteger on_image_viewer_index(NotesImageViewer *viewer);

/*
 * on_image_viewer_key_down — offer a key to the open viewer: Escape closes
 * it, Left and Right step through the host's images (an arrow at either end
 * is taken too, so it cannot leak through and move the caret behind).
 *
 * The host window calls this FIRST for every key while the viewer is open,
 * and swallows the key itself when this returns NO — the keyboard focus
 * stays on the view behind the viewer, and typing must not edit a note
 * hidden underneath.
 *
 * Inputs:
 *   viewer — the viewer
 *   event  — the key-down event
 *
 * Output: YES when the viewer took the key.
 */
BOOL on_image_viewer_key_down(NotesImageViewer *viewer, NSEvent *event);

/*
 * on_image_viewer_nav_sync — count_handler has a new answer: redraw the
 * Previous | Next row.  Only a host whose image set grows while the viewer
 * is open needs this.
 */
void on_image_viewer_nav_sync(NotesImageViewer *viewer);
