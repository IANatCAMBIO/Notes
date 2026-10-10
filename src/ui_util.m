#import "ui_util.h"
#import "tool_panel.h"

#include "config.h"
#include "identity.h"
#include "image_io.h"

NSPasteboardType const on_pasteboard_type_fragment = @ON_BUNDLE_ID ".fragment";
NSPasteboardType const on_pasteboard_type_rows     = @ON_BUNDLE_ID ".rows";

/* The bundled image that is the app's icon. */
static NSString * const app_icon_name = @"composition";

/*
 * on_ui_is_dark — dark appearance test.
 * Inputs/output are documented in ui_util.h.
 */
BOOL
on_ui_is_dark(NSView *view)
{
    NSAppearanceName match = [view.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
}

/*
 * ns_color — one on_color_t as an NSColor.
 *
 * Inputs:
 *   c — the colour
 *
 * Output: an sRGB NSColor.
 */
static NSColor *
ns_color(on_color_t c)
{
    return [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:c.a];
}

/*
 * on_ui_color — appearance-following NSColor for a look colour.
 * Inputs/output are documented in ui_util.h.
 */
NSColor *
on_ui_color(on_look_color_t color)
{
    NSColor *light = ns_color(color.light);
    NSColor *dark  = ns_color(color.dark);
    return [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
        NSAppearanceName match = [appearance
            bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        return [match isEqualToString:NSAppearanceNameDarkAqua] ? dark : light;
    }];
}

/*
 * sharpest_scale — the largest backing scale among the screens, so an image
 * decoded once is sharp on whichever screen its window moves to.
 *
 * Output: the scale; 2 when there is no screen (a headless test).
 */
static CGFloat
sharpest_scale(void)
{
    CGFloat scale = 0.0;
    for (NSScreen *screen in NSScreen.screens) {
        scale = MAX(scale, screen.backingScaleFactor);
    }
    return scale > 0.0 ? scale : 2.0;
}

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
 * on_ui_icon — bundled icon at its display size, cached.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
on_ui_icon(NSString *name, CGFloat points)
{
    static NSMutableDictionary<NSString *, NSImage *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMutableDictionary dictionary]; });

    NSString *key  = [NSString stringWithFormat:@"%@@%g", name, points];
    NSImage *image = cache[key];
    if (image) return image;

    NSData *data = [NSData dataWithContentsOfURL:
        [NSBundle.mainBundle URLForResource:name withExtension:@"png"]];
    if (!data) return nil;
    image = on_ui_thumbnail(data.bytes, data.length, points, points, sharpest_scale());
    if (!image) return nil;
    image.size = NSMakeSize(points, points);   /* icons are square artwork */
    cache[key] = image;
    return image;
}

/*
 * on_ui_app_icon — the app's artwork.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
on_ui_app_icon(void)
{
    return on_ui_icon(app_icon_name, on_look_about_icon);
}

/*
 * on_ui_image_view — image view created at its final size.
 * Inputs/output are documented in ui_util.h.
 */
NSImageView *
on_ui_image_view(NSImage *image, CGFloat width, CGFloat height)
{
    NSImageView *view = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    view.imageScaling = NSImageScaleProportionallyDown;
    view.translatesAutoresizingMaskIntoConstraints = NO;
    [view.widthAnchor constraintEqualToConstant:width].active   = YES;
    [view.heightAnchor constraintEqualToConstant:height].active = YES;
    view.image = image;              /* last: the view already has its frame */
    return view;
}

/*
 * on_ui_show_notice — one-button message in a tool panel.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_show_notice(NSString *title, NSString *message, NSWindow *window, void (^done)(void))
{
    [NotesToolPanel show_message:title message:message
                          symbol:@"exclamationmark.triangle.fill"
                         buttons:@[@"OK"] over:window
                         handler:^(NSInteger index) { (void)index; if (done) done(); }];
}

/*
 * on_ui_confirm — confirmation in a tool panel, Cancel the default.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_confirm(NSString *question, NSString *detail, NSString *action, NSWindow *window,
              void (^handler)(BOOL confirmed))
{
    [NotesToolPanel show_message:question message:detail ?: @"This cannot be undone."
                          symbol:@"exclamationmark.triangle.fill"
                         buttons:@[@"Cancel", action]   /* [0] = default (Return) */
                            over:window
                         handler:^(NSInteger index) { handler(index == 1); }];
}

/*
 * on_ui_open_image_external — a note's image in an outside viewer.
 * Inputs/output are documented in ui_util.h.
 */
BOOL
on_ui_open_image_external(NSData *png)
{
    if (png == nil) return NO;
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@ON_BUNDLE_ID];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES
                                             attributes:nil error:NULL];
    NSString *name = [NSString stringWithFormat:@"image-%@.png", NSUUID.UUID.UUIDString];
    NSURL    *url  = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:name]];
    if (![png writeToURL:url atomically:YES]) return NO;

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
 * on_ui_pick_path — open or save panel.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_pick_path(NotesPickKind kind, NSString *title, NSString *prompt, NSArray<UTType *> *types,
                NSString *start_dir, NSWindow *window, void (^handler)(NSString *path))
{
    NSSavePanel *panel;
    if (kind == NotesPickSaveFile) {
        panel = [NSSavePanel savePanel];
    } else {
        NSOpenPanel *open            = [NSOpenPanel openPanel];
        open.canChooseFiles          = kind == NotesPickOpenFile;
        open.canChooseDirectories    = kind == NotesPickFolder;
        open.canCreateDirectories    = kind == NotesPickFolder;
        open.allowsMultipleSelection = NO;
        panel = open;
    }
    panel.message = title;
    panel.prompt  = prompt;
    if (types) panel.allowedContentTypes = types;
    if (start_dir) panel.directoryURL = [NSURL fileURLWithPath:start_dir isDirectory:YES];

    void (^done)(NSModalResponse) = ^(NSModalResponse response) {
        handler(response == NSModalResponseOK ? panel.URL.path : nil);
    };
    if (window) {
        [panel beginSheetModalForWindow:window completionHandler:done];
    } else {
        done([panel runModal]);
    }
}

/*
 * on_ui_toolbar_search_field — toolbar search field without a focus ring.
 * Inputs/output are documented in ui_util.h.
 */
NSSearchField *
on_ui_toolbar_search_field(NSString *placeholder, CGFloat width)
{
    NSSearchField *field = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(0, 0, width, on_look_toolbar_field_h)];
    field.placeholderString = placeholder;
    field.focusRingType     = NSFocusRingTypeNone;
    field.translatesAutoresizingMaskIntoConstraints = NO;
    [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

/*
 * on_ui_format_bytes — Finder-style byte count via one shared formatter.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
on_ui_format_bytes(int64_t bytes)
{
    static NSByteCountFormatter *fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        fmt = [[NSByteCountFormatter alloc] init];
        fmt.countStyle = NSByteCountFormatterCountStyleFile;
    });
    return [fmt stringFromByteCount:bytes];
}

/*
 * on_ui_format_time — date and time via one shared formatter.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
on_ui_format_time(int64_t unix_time)
{
    static NSDateFormatter *fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        fmt = [[NSDateFormatter alloc] init];
        fmt.dateStyle = NSDateFormatterMediumStyle;
        fmt.timeStyle = NSDateFormatterShortStyle;
    });
    if (unix_time == 0) return @"—";
    return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)unix_time]];
}
