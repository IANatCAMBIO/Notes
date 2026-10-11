#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <glib.h>

#include "look.h"

/*
 * ui_util — small AppKit helpers shared across the app, so each exists
 * exactly once: colours from look.h, images at their display size, alerts,
 * the file pickers, formatting, the pasteboard types.  Most are Files'
 * util.m helpers under this app's prefix.
 *
 * IMAGES: THE RULE.  An image shown in a view is decoded at the size it is
 * shown, and the view that shows it is created at that size.  An
 * NSImageView created with a zero frame and given a large image before
 * layout makes AppKit render and cache the image's LARGEST representation
 * (4 MB for a 1024 px icon), and its layer keeps a second copy — tens of
 * megabytes for a screenful of icons.  So nothing in the app calls
 * [[NSImageView alloc] init] or +imageViewWithImage:, or hands a
 * full-size NSImage to a small view.  Use:
 *   on_ui_icon          a bundled icon, decoded at the point size asked for
 *   on_ui_thumbnail     a note's image, decoded capped at its display size
 *   on_ui_image_view    an image view created at its final frame
 * See porting/INTERFACES.md, "Memory and ownership".
 *
 * Everything here is main-thread only unless it says otherwise.
 */

/* ── Pasteboard types (private to this app; see identity.h) ─────────────── */

/* A selection of a note, as a BNBF sub-document, so a table or an image
 * pastes as itself.  Written beside NSPasteboardTypeString. */
extern NSPasteboardType const on_pasteboard_type_fragment;

/* A drag of library rows: notes or a folder (library_state.h). */
extern NSPasteboardType const on_pasteboard_type_rows;

/* ── Appearance ─────────────────────────────────────────────────────────── */

/*
 * on_ui_is_dark — whether a view is drawn under a dark appearance: the
 * `dark` that on_look_pick, OnDocLayoutStyle and on_text_style_t take.
 *
 * Inputs:
 *   view — the view being drawn
 *
 * Output: YES for a dark appearance.
 */
BOOL on_ui_is_dark(NSView *view);

/*
 * on_ui_color — a look.h colour as an NSColor that follows the appearance
 * it is drawn under.
 *
 * Inputs:
 *   color — the look colour
 *
 * Output: a dynamic NSColor.
 */
NSColor *on_ui_color(on_look_color_t color);

/* ── Images ─────────────────────────────────────────────────────────────── */

/*
 * on_ui_icon — a bundled icon (Contents/Resources/<name>.png, the files in
 * icons/), decoded at the size it will be drawn: `points` times the
 * sharpest screen's scale, never the file's full 512 px.  Cached per name
 * and size; the set of icons is fixed, so the cache is bounded.
 *
 * Inputs:
 *   name   — the file's base name, e.g. @"new-folder"
 *   points — the edge length it is shown at
 *
 * Output: the image, sized `points` square; nil if there is no such icon.
 *         Do not modify it (it is shared).
 */
NSImage *on_ui_icon(NSString *name, CGFloat points);

/*
 * on_ui_app_icon — the app's icon as drawn (composition.png), at the About
 * panel's size.  macOS 26 puts a bundle icon that is not its rounded-square
 * shape on a grey backdrop in alerts and the About panel; handing them this
 * image explicitly shows just the artwork.
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
 * on_ui_image_view — an image view created AT ITS FINAL SIZE (see THE RULE
 * above), scaling its image down proportionally.  The only way the app
 * makes an NSImageView.
 *
 * Inputs:
 *   image  — what to show (from on_ui_icon / on_ui_thumbnail); may be nil
 *            and set later, once the view has this frame
 *   width  — the view's size in points
 *   height
 *
 * Output: the view, with fixed width and height constraints so Auto Layout
 *         keeps the size.
 */
NSImageView *on_ui_image_view(NSImage *image, CGFloat width, CGFloat height);

/* ── Messages ───────────────────────────────────────────────────────────── */

/*
 * The app never shows a native dialog (NSAlert, an alert sheet): every
 * message and question is a tool panel (tool_panel.h).  These two cover
 * the common cases; anything else builds a NotesToolPanel directly.
 */

/*
 * on_ui_show_notice — a message with one OK button, in a tool panel.  The
 * only way a failure is shown: the title states what failed, the message
 * why.
 *
 * Inputs:
 *   title   — the headline, e.g. @"Could not open the database"
 *   message — the detail; may be nil
 *   window  — the window to centre over; nil = the screen
 *   done    — called once on the main thread when the panel goes (OK,
 *             Escape or the close box); may be nil.  The panel is not
 *             modal: whatever must wait for the user goes here.
 */
void on_ui_show_notice(NSString *title, NSString *message, NSWindow *window,
                       void (^done)(void));

/*
 * on_ui_confirm — ask, in a tool panel, before something that cannot be
 * undone.  The default button is Cancel and Escape dismisses, so neither
 * Return nor Escape ever destroys anything.
 *
 * Inputs:
 *   question — the headline, e.g. @"Delete 3 notes permanently?"
 *   detail   — the consequence; nil for "This cannot be undone."
 *   action   — the confirming button's title, e.g. @"Delete"
 *   window   — the window to centre over; nil = the screen
 *   handler  — called on the main thread with YES only if confirmed
 */
void on_ui_confirm(NSString *question, NSString *detail, NSString *action,
                   NSWindow *window, void (^handler)(BOOL confirmed));

/* ── File pickers ───────────────────────────────────────────────────────── */

/* What on_ui_pick_path asks for. */
typedef NS_ENUM(NSInteger, NotesPickKind) {
    NotesPickOpenFile,               /* an existing file                    */
    NotesPickSaveFile,               /* a file name to write                */
    NotesPickFolder,                 /* an existing folder (may create one) */
};

/*
 * on_ui_pick_path — run an open or save panel and hand over the choice.
 * The panel is always a window of its own, app-modal, as in the Files app:
 * never a sheet on another window (the app's rule: no sheets).
 *
 * Inputs:
 *   kind      — what to pick
 *   title     — the panel's message
 *   prompt    — the accept button's title, e.g. @"Open"
 *   types     — content types to allow (e.g. @[UTTypeImage]); nil = any
 *   start_dir — the folder to open in; nil = where the user last was
 *   handler   — called on the main thread, once, when the panel closes,
 *               with the chosen path, or nil when it was cancelled
 */
void on_ui_pick_path(NotesPickKind kind, NSString *title, NSString *prompt,
                     NSArray<UTType *> *types, NSString *start_dir,
                     void (^handler)(NSString *path));

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
 * on_ui_status_label — one label of a status bar (the library's, an
 * editor's): small (on_look_status_font), secondary colour, truncated in
 * the middle, added to `bar` and centred VISUALLY on `middle` — the middle
 * of its capital letters, not of its frame, which carries room for
 * descenders and would sit the text low.  The caller places it
 * horizontally.  on_ui_use_ui_font leaves it at its size.
 *
 * Inputs:
 *   bar       — the view it goes in
 *   middle    — the y the text is centred on (the bar's middle)
 *   alignment — its text alignment
 *
 * Output: the label.
 */
NSTextField *on_ui_status_label(NSView *bar, NSLayoutYAxisAnchor *middle, NSTextAlignment alignment);

/*
 * on_ui_use_ui_font — give everything under a view the app's text size
 * (look.h: on_look_ui_font), keeping each font's weight and slant; labelled
 * buttons, check boxes, radios, pop-ups, steppers and editable fields also
 * take the small control size, which is drawn for that size of text.  An
 * outline view (the sidebar) and what is under it, and status labels
 * (on_ui_status_label), keep their own.  Called
 * once a window's content is built; cells made later (table rows) set the
 * font themselves (on_ui_label_cell).
 *
 * Inputs:
 *   root — the view to start at (itself included)
 */
void on_ui_use_ui_font(NSView *root);

/*
 * on_ui_strike_key — an attribute for the text of an on_ui_label_cell:
 * the characters carrying it (any value) are struck through, at the
 * middle of the lowercase letters (half the x-height above the baseline),
 * as the note view strikes text.  AppKit's own NSStrikethroughStyle runs
 * a point or two lower, so the app's tables use this instead.
 */
extern NSAttributedStringKey const on_ui_strike_key;

/*
 * on_ui_label_cell — a table's reusable one-line text cell: a label in the
 * cell font, truncated at the tail, centred vertically in its row.  (A
 * bare NSTextField returned as a cell sits at the top of the row.)
 *
 * Inputs:
 *   table — the table
 *   ident — the cell's reuse identifier (the column's, usually)
 *
 * Output: the cell; its textField is the label, which strikes through
 *         text carrying on_ui_strike_key.
 */
NSTableCellView *on_ui_label_cell(NSTableView *table, NSString *ident);

/*
 * NotesFlippedView — a plain container whose y grows downward: the
 * document view of a scroll view whose content should start at the top
 * (a scroll view shows a non-flipped document's BOTTOM first).
 */
@interface NotesFlippedView : NSView
@end

/*
 * on_ui_toolbar_search_field — a search field for a toolbar: fixed width,
 * and NO focus ring.  A focused toolbar field's ring is clipped by the
 * toolbar and changes the field's measured width; this is the one place
 * the app turns a focus ring off — every other field keeps the system's.
 *
 * Inputs:
 *   placeholder — the grey prompt, e.g. @"Search all notes"
 *   width       — the field's width in points
 *
 * Output: the field.
 */
NSSearchField *on_ui_toolbar_search_field(NSString *placeholder, CGFloat width);

/* ── Formatting ─────────────────────────────────────────────────────────── */

/*
 * on_ui_format_bytes — a byte count as Finder shows it ("12.3 MB").
 * Thread-safe.
 *
 * Inputs:
 *   bytes — the count
 *
 * Output: the text.
 */
NSString *on_ui_format_bytes(int64_t bytes);

/*
 * on_ui_format_time — a UNIX time as a medium date and a short time in the
 * user's locale ("3 Oct 2026 at 14:05").  One shared formatter: never one
 * per table cell.  Thread-safe.
 *
 * Inputs:
 *   unix_time — seconds since the epoch; 0 yields "—"
 *
 * Output: the text.
 */
NSString *on_ui_format_time(int64_t unix_time);

/*
 * on_ui_ids — note or folder ids held as NSNumbers (a selection, a drag),
 * as the plain array the core's calls take.
 *
 * Inputs:
 *   ids — the ids
 *
 * Output: a new array of ids.count ids, in order (free() it); NULL when
 *         ids is empty.
 */
gint64 *on_ui_ids(NSArray<NSNumber *> *ids);
