#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <glib.h>

#include "look.h"

#import "shared_tool_panel.h"
#import "shared_ui.h"

/*
 * ui_util — the small AppKit helpers that are this app's own; what both
 * apps share (colours, glyphs, dialogs, the file picker, status labels,
 * tables, formatting) is in shared_ui.h, compiled in from ../shared_ui.
 *
 * IMAGES: THE RULE (shared_ui.h).  An image shown in a view is decoded at
 * the size it is shown, and the view that shows it is created at that
 * size (shared_ui_image_view).  Here: on_ui_app_icon, the bundled icon at
 * the About size, and on_ui_thumbnail, a note's image decoded capped at
 * its display size.  See porting/INTERFACES.md, "Memory and ownership".
 *
 * Everything here is main-thread only unless it says otherwise.
 */

/* ── Pasteboard types (private to this app; see identity.h) ─────────────── */

/* A selection of a note, as a BNBF sub-document, so a table or an image
 * pastes as itself.  Written beside NSPasteboardTypeString. */
extern NSPasteboardType const on_pasteboard_type_fragment;

/* A drag of library rows: notes (library_state.h). */
extern NSPasteboardType const on_pasteboard_type_rows;

/* ── Images ─────────────────────────────────────────────────────────────── */

/*
 * on_ui_app_icon — the app's icon as drawn (composition.png, the one bundled
 * image) at the About panel's size (shared_ui_app_icon): the place that
 * knows the artwork's name.
 *
 * Output: the image, or nil if the resource is missing.
 */
NSImage *on_ui_app_icon(void);

/*
 * on_ui_thumbnail — encoded image bytes (a note's stored PNG) as an image
 * for a box of the given size: decoded capped at the box's longest side in
 * PIXELS (image_io.h: on_image_decode_capped), never at full size.  Thread
 * safe: thumbnails are decoded on a worker and shown on the main thread.
 *
 * Inputs:
 *   bytes  — the encoded image
 *   n      — its length
 *   box_w  — the display box, in points
 *   box_h
 *   scale  — the backing scale of the screen it is for (the window's
 *            backingScaleFactor, read on the main thread)
 *
 * Output: an image whose point size fits the box, aspect kept, never
 *         upscaled; nil when the bytes will not decode.
 */
NSImage *on_ui_thumbnail(const uint8_t *bytes, size_t n, CGFloat box_w,
                         CGFloat box_h, CGFloat scale);

/*
 * on_ui_open_image_external — show a note's image in an outside viewer: the
 * bytes are written to a temporary file which is opened with the app named
 * by the "image_viewer" setting (an app name or path), or with the
 * system's default app for PNGs when it is unset.
 *
 * Inputs:
 *   png — the image's stored bytes; nil does nothing
 *
 * Output: NO when the file could not be written or opened.
 */
BOOL on_ui_open_image_external(NSData *png);

/* ── Window lifetime ────────────────────────────────────────────────────── */

/*
 * NotesKeyFilterWindow — a window that offers every key-down to key_filter
 * BEFORE the first responder sees it; a key the filter takes goes no
 * further.  For the image viewer, which claims keys off a view that keeps
 * the focus while a picture is up (GTK: a capture-phase key controller on
 * the window).
 */
@interface NotesKeyFilterWindow : NSWindow
@property (nonatomic, copy) BOOL (^key_filter)(NSEvent *event);
@end

/*
 * NotesKeyFilterPanel — NotesKeyFilterWindow as a panel, for a tool window
 * with the utility title bar (the media window).  AppKit gives that title
 * bar only to an NSPanel, and the editor must stay an NSWindow (a panel
 * cannot be the main window), so the filter has a class of each kind;
 * both run the same filter code.
 */
@interface NotesKeyFilterPanel : NSPanel
@property (nonatomic, copy) BOOL (^key_filter)(NSEvent *event);
@end

/*
 * on_ui_keep_until_closed — own a window controller until its window
 * closes.  For windows that may be open several at a time and that nothing
 * else holds (search, media): an NSWindow does not keep its controller
 * alive, so without an owner the controller — the window's delegate, data
 * source and target — is freed as soon as it is made.  Released on the
 * main queue just after the window's windowWillClose: notifications.
 *
 * Inputs:
 *   controller — the controller, its window already made
 */
void on_ui_keep_until_closed(NSWindowController *controller);

/* ── Controls ───────────────────────────────────────────────────────────── */

/*
 * on_ui_label_cell — a table's reusable one-line text cell: a label in the
 * cell font, truncated at the tail, centred vertically in its row.  (A
 * bare NSTextField returned as a cell sits at the top of the row.)
 *
 * Inputs:
 *   table — the table
 *   ident — the cell's reuse identifier (the column's, usually)
 *
 * Output: the cell; its textField is the label, which shows attributed
 *         text that carries its own colour (a due date's tint) in the
 *         selection's text colour while the row is selected, as a plain
 *         label is.
 */
NSTableCellView *on_ui_label_cell(NSTableView *table, NSString *ident);

/* ── Formatting ─────────────────────────────────────────────────────────── */

/*
 * on_ui_ids — note ids held as NSNumbers (a selection, a drag),
 * as the plain array the core's calls take.
 *
 * Inputs:
 *   ids — the ids
 *
 * Output: a new array of ids.count ids, in order (free() it); NULL when
 *         ids is empty.
 */
gint64 *on_ui_ids(NSArray<NSNumber *> *ids);

