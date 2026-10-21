#import "ui_util.h"

#include "config.h"
#include "identity.h"
#include "image_io.h"

NSPasteboardType const on_pasteboard_type_fragment = @ON_BUNDLE_ID ".fragment";
NSPasteboardType const on_pasteboard_type_rows     = @ON_BUNDLE_ID ".rows";

/* The bundled image that is the app's icon: its file name (identity.h). */
static NSString * const app_icon_name = @ON_APP_ICON;


/*
 * on_ui_thumbnail — image bytes decoded for a display box.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
on_ui_thumbnail(const uint8_t *bytes, size_t n, CGFloat box_w, CGFloat box_h, CGFloat scale)
{
    int        max_px = (int)ceil(MAX(box_w, box_h) * scale);
    CGImageRef cg     = on_image_decode_capped(bytes, n, max_px);
    if (cg == NULL) return nil;

    /* Fit the box in points, aspect kept, never larger than the pixels. */
    CGFloat w   = (CGFloat)CGImageGetWidth(cg) / scale;
    CGFloat h   = (CGFloat)CGImageGetHeight(cg) / scale;
    CGFloat fit = MIN(1.0, MIN(box_w / w, box_h / h));
    NSImage *image = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w * fit, h * fit)];
    CGImageRelease(cg);              /* the NSImage holds its own reference */
    return image;
}

/*
 * on_ui_app_icon — the app's artwork at the About size.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
on_ui_app_icon(void)
{
    return shared_ui_app_icon(app_icon_name, on_look_about_icon);
}

/*
 * on_ui_temp_file_from — a temporary file, written by the caller, quarantined.
 * Inputs/output are documented in ui_util.h.
 */
NSURL *
on_ui_temp_file_from(NSString *name, BOOL (^write)(NSString *path))
{
    if (name.length == 0) return nil;
    NSString *dir = [[NSTemporaryDirectory() stringByAppendingPathComponent:@ON_BUNDLE_ID]
                        stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                  attributes:nil error:NULL])
        return nil;
    NSString *path = [dir stringByAppendingPathComponent:name];
    if (!write(path)) return nil;
    NSURL *url = [NSURL fileURLWithPath:path];
    /* Marked as downloaded, so double-clicking an attached program or script
     * gets the system's usual check instead of running unasked. */
    [url setResourceValue:@{ (__bridge NSString *)kLSQuarantineTypeKey:
                                 (__bridge NSString *)kLSQuarantineTypeOtherDownload }
                   forKey:NSURLQuarantinePropertiesKey error:NULL];
    return url;
}

/*
 * on_ui_temp_file — a note's bytes as a quarantined temporary file.
 * Inputs/output are documented in ui_util.h.
 */
NSURL *
on_ui_temp_file(NSData *data, NSString *name)
{
    if (data == nil) return nil;
    return on_ui_temp_file_from(name, ^BOOL(NSString *path) {
        return [data writeToFile:path atomically:YES];
    });
}

/*
 * on_ui_open_image_external — a note's image in an outside viewer.
 * Inputs/output are documented in ui_util.h.
 */
BOOL
on_ui_open_image_external(NSData *png)
{
    NSURL *url = on_ui_temp_file(png, @"image.png");
    if (url == nil) return NO;

    gchar *viewer = on_app_config_get("image_viewer");
    BOOL   ok;
    if (viewer != NULL && *viewer != '\0') {
        NSTask *task        = [[NSTask alloc] init];
        task.executableURL  = [NSURL fileURLWithPath:@"/usr/bin/open"];
        task.arguments      = @[@"-a", @(viewer), url.path];
        ok = [task launchAndReturnError:NULL];
    } else {
        ok = [NSWorkspace.sharedWorkspace openURL:url];
    }
    g_free(viewer);
    return ok;
}

/*
 * filter_takes — does a window's key filter take this event?
 *
 * Inputs:
 *   filter — the window's key_filter, or nil
 *   event  — any event for the window
 *
 * Output: YES for a key-down the filter took; it goes no further.
 */
static BOOL
filter_takes(BOOL (^filter)(NSEvent *event), NSEvent *event)
{
    return event.type == NSEventTypeKeyDown && filter != nil && filter(event);
}

@implementation NotesKeyFilterWindow

/*
 * sendEvent: — a key-down the filter takes goes no further.
 *
 * Inputs:
 *   event — any event for the window
 */
- (void)
sendEvent:(NSEvent *)event
{
    if (filter_takes(self.key_filter, event)) return;
    [super sendEvent:event];
}

@end

/* Controllers on_ui_keep_until_closed owns, until their windows close. */
static NSMutableSet<NSWindowController *> *kept_controllers;

/*
 * on_ui_keep_until_closed — own a controller until its window closes.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_keep_until_closed(NSWindowController *controller)
{
    if (!kept_controllers) kept_controllers = [NSMutableSet set];
    [kept_controllers addObject:controller];
    __block id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSWindowWillCloseNotification object:controller.window queue:nil
                usingBlock:^(NSNotification *note) {
        [NSNotificationCenter.defaultCenter removeObserver:observer];
        observer = nil;
        /* Not now: the window's own windowWillClose: may still be due. */
        dispatch_async(dispatch_get_main_queue(), ^{
            [kept_controllers removeObject:controller];
        });
    }];
}

/*
 * NotesCellLabel — the label of an on_ui_label_cell: an ordinary label
 * that shows attributed text carrying its own colour in the selection's
 * text colour while its row is selected (AppKit swaps the colour of a
 * plain label, not of a colour an attributed string brought along).
 */
@interface NotesCellLabel : NSTextField
@property (nonatomic, copy) NSAttributedString *tinted;   /* the attributed text as given */
@end

@implementation NotesCellLabel

/*
 * setAttributedStringValue: — keep the text as given and show it for the
 * current emphasis.
 *
 * Inputs:
 *   value — the attributed text
 */
- (void)
setAttributedStringValue:(NSAttributedString *)value
{
    self.tinted = value;
    [self apply_emphasis];
}

/*
 * setStringValue: — plain text: AppKit handles its emphasis; nothing kept.
 *
 * Inputs:
 *   value — the text
 */
- (void)
setStringValue:(NSString *)value
{
    [super setStringValue:value];
    self.tinted = nil;
}

/*
 * apply_emphasis — show the kept attributed text, every colour in it
 * replaced by the selection's text colour while the row is emphasized.
 */
- (void)
apply_emphasis
{
    NSAttributedString *shown = self.tinted;
    if (shown == nil) return;
    if (self.cell.backgroundStyle == NSBackgroundStyleEmphasized) {
        NSMutableAttributedString *white = [shown mutableCopy];
        [white addAttribute:NSForegroundColorAttributeName
                      value:NSColor.alternateSelectedControlTextColor
                      range:NSMakeRange(0, white.length)];
        shown = white;
    }
    [super setAttributedStringValue:shown];
}

@end

/*
 * NotesLabelCell — an on_ui_label_cell: tells its label when the row's
 * emphasis changes (NSTableCellView passes the style to the label's cell,
 * which recolours plain text only).
 */
@interface NotesLabelCell : NSTableCellView
@end

@implementation NotesLabelCell

/*
 * setBackgroundStyle: — the row became, or stopped being, a selected row
 * of the focused table: the label shows its tinted text for it.
 *
 * Inputs:
 *   style — the new style
 */
- (void)
setBackgroundStyle:(NSBackgroundStyle)style
{
    [super setBackgroundStyle:style];
    [(NotesCellLabel *)self.textField apply_emphasis];
}

@end

/*
 * on_ui_label_cell — a vertically centred text cell.
 * Inputs/output are documented in ui_util.h.
 */
NSTableCellView *
on_ui_label_cell(NSTableView *table, NSString *ident)
{
    NSTableCellView *cell = [table makeViewWithIdentifier:ident owner:nil];
    if (cell) return cell;
    cell = [[NotesLabelCell alloc] initWithFrame:NSZeroRect];
    cell.identifier = ident;
    NSTextField *label = [NotesCellLabel labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.font = [NSFont systemFontOfSize:shared_look_ui_font];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [cell addSubview:label];
    cell.textField = label;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:shared_look_cell_pad_h],
        [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-shared_look_cell_pad_h],
        [label.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

/*
 * on_ui_ids — NSNumber ids as a plain array.
 * Inputs/output are documented in ui_util.h.
 */
gint64 *
on_ui_ids(NSArray<NSNumber *> *ids)
{
    if (ids.count == 0) return NULL;
    gint64 *out = malloc(ids.count * sizeof *out);
    for (NSUInteger i = 0; i < ids.count; i++) out[i] = ids[i].longLongValue;
    return out;
}

