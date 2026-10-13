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
 * fixed_color — one appearance's colour as an sRGB NSColor.
 *
 * Inputs:
 *   c — the colour
 *
 * Output: the colour.
 */
static NSColor *
fixed_color(on_color_t c)
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
    NSColor *light = fixed_color(color.light);
    NSColor *dark  = fixed_color(color.dark);
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
 * on_ui_symbol — SF Symbol template image, cached.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
on_ui_symbol(NSString *name, CGFloat points)
{
    static NSMutableDictionary<NSString *, NSImage *> *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSMutableDictionary dictionary]; });

    NSString *key  = [NSString stringWithFormat:@"%@@%g", name, points];
    NSImage *image = cache[key];
    if (image) return image;

    image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (image && points > 0.0) {
        image = [image imageWithSymbolConfiguration:
                    [NSImageSymbolConfiguration configurationWithPointSize:points
                                                                    weight:NSFontWeightRegular]];
    }
    if (image) cache[key] = image;
    return image;
}

/*
 * on_ui_app_icon — the app's artwork, decoded once at the About size.
 * Inputs/output are documented in ui_util.h.
 */
NSImage *
on_ui_app_icon(void)
{
    static NSImage *icon;
    if (icon) return icon;

    NSData *data = [NSData dataWithContentsOfURL:
        [NSBundle.mainBundle URLForResource:app_icon_name withExtension:@"png"]];
    if (!data) return nil;
    icon = on_ui_thumbnail(data.bytes, data.length, on_look_about_icon, on_look_about_icon,
                           sharpest_scale());
    icon.size = NSMakeSize(on_look_about_icon, on_look_about_icon);   /* square artwork */
    return icon;
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

@implementation NotesFlippedView

/*
 * isFlipped — y grows downward.
 *
 * Output: YES.
 */
- (BOOL)
isFlipped
{
    return YES;
}

@end

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

@implementation NotesKeyFilterPanel

/*
 * sendEvent: — a key-down the filter takes goes no further.
 *
 * Inputs:
 *   event — any event for the panel
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
 * on_ui_pick_path — open or save panel.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_pick_path(NotesPickKind kind, NSString *title, NSString *prompt, NSArray<UTType *> *types,
                NSString *start_dir, void (^handler)(NSString *path))
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

    handler([panel runModal] == NSModalResponseOK ? panel.URL.path : nil);
}

/*
 * NotesStatusLabel — a status bar's label (on_ui_status_label): its own
 * class only so that on_ui_use_ui_font can tell it apart.
 */
@interface NotesStatusLabel : NSTextField
@end

@implementation NotesStatusLabel
@end

/*
 * on_ui_status_label — a status bar label, centred on its capitals.
 * Inputs/output are documented in ui_util.h.
 */
NSTextField *
on_ui_status_label(NSView *bar, NSLayoutYAxisAnchor *middle, NSTextAlignment alignment)
{
    NotesStatusLabel *label = [NotesStatusLabel labelWithString:@""];
    label.font          = [NSFont systemFontOfSize:on_look_status_font];
    label.textColor     = NSColor.secondaryLabelColor;
    label.alignment     = alignment;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [bar addSubview:label];
    /* y grows downward for anchors: the baseline sits half a cap height
     * below the middle, so the capitals straddle it. */
    [label.firstBaselineAnchor constraintEqualToAnchor:middle
                                              constant:round(label.font.capHeight / 2.0)].active = YES;
    return label;
}

/*
 * on_ui_use_ui_font — the app's text size under a view.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_use_ui_font(NSView *root)
{
    if ([root isKindOfClass:NSOutlineView.class]) return;     /* the sidebar keeps its own */
    if ([root isKindOfClass:NotesStatusLabel.class]) return;  /* so do the status bars     */
    NSFontManager *fm = NSFontManager.sharedFontManager;
    if ([root isKindOfClass:NSControl.class] && ![root isKindOfClass:NSTableView.class]) {
        NSControl *c = (NSControl *)root;
        if (c.font) c.font = [fm convertFont:c.font toSize:on_look_ui_font];
        BOOL labelled = [c isKindOfClass:NSButton.class] && ((NSButton *)c).title.length > 0;
        BOOL editable = [c isKindOfClass:NSTextField.class] && ((NSTextField *)c).editable;
        if (labelled || editable || [c isKindOfClass:NSPopUpButton.class] || [c isKindOfClass:NSStepper.class])
            c.controlSize = NSControlSizeSmall;
    } else if ([root isKindOfClass:NSTextView.class]) {
        NSTextView *t = (NSTextView *)root;
        if (t.font) t.font = [fm convertFont:t.font toSize:on_look_ui_font];
    }
    for (NSView *v in root.subviews) on_ui_use_ui_font(v);
}

NSAttributedStringKey const on_ui_strike_key = @"notes.strike";

/*
 * NotesCellLabel — the label of an on_ui_label_cell: an ordinary label
 * that also strikes through its text carrying on_ui_strike_key.
 */
@interface NotesCellLabel : NSTextField
@end

@implementation NotesCellLabel

/*
 * drawRect: — the text, then a 1 pt line through each struck range, half
 * the x-height above the baseline, in the range's colour; a truncated
 * range stops at the label's edge.
 *
 * Inputs:
 *   dirty — the area to draw
 */
- (void)
drawRect:(NSRect)dirty
{
    [super drawRect:dirty];
    NSAttributedString *text  = self.attributedStringValue;
    NSRect              title = [self.cell titleRectForBounds:self.bounds];
    CGFloat             y     = self.firstBaselineOffsetFromTop;   /* a text field is flipped: y down */
    [text enumerateAttribute:on_ui_strike_key inRange:NSMakeRange(0, text.length) options:0
                  usingBlock:^(id value, NSRange range, BOOL *stop) {
        if (value == nil) return;
        NSFont  *font = [text attribute:NSFontAttributeName atIndex:range.location effectiveRange:NULL] ?: self.font;
        NSColor *ink  = [text attribute:NSForegroundColorAttributeName atIndex:range.location effectiveRange:NULL]
                        ?: self.textColor;
        CGFloat  x0   = NSMinX(title) + [text attributedSubstringFromRange:NSMakeRange(0, range.location)].size.width;
        CGFloat  x1   = MIN(NSMinX(title) + [text attributedSubstringFromRange:
                                             NSMakeRange(0, NSMaxRange(range))].size.width, NSMaxX(title));
        CGFloat  ly   = round(y - font.xHeight / 2.0) + 0.5;     /* on the pixel row */
        if (x1 <= x0) return;
        [ink setStroke];
        NSBezierPath *line = [NSBezierPath bezierPath];
        line.lineWidth = 1.0;
        [line moveToPoint:NSMakePoint(x0, ly)];
        [line lineToPoint:NSMakePoint(x1, ly)];
        [line stroke];
    }];
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
    cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cell.identifier = ident;
    NSTextField *label = [NotesCellLabel labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.font = [NSFont systemFontOfSize:on_look_ui_font];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [cell addSubview:label];
    cell.textField = label;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:4],
        [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
        [label.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

/*
 * vetted_selection — the selection a click on one row would make, after
 * the delegate's say; an outline's delegate is asked its own questions.
 *
 * Inputs:
 *   table — the table or outline
 *   row   — the clicked row, >= 0
 *
 * Output: the rows to select; empty when the delegate refuses.
 */
static NSIndexSet *
vetted_selection(NSTableView *table, NSInteger row)
{
    NSIndexSet *rows = [NSIndexSet indexSetWithIndex:(NSUInteger)row];
    id          d    = table.delegate;
    if ([table isKindOfClass:NSOutlineView.class]) {
        NSOutlineView *outline = (NSOutlineView *)table;
        if ([d respondsToSelector:@selector(selectionShouldChangeInOutlineView:)] &&
            ![d selectionShouldChangeInOutlineView:outline]) return [NSIndexSet indexSet];
        if ([d respondsToSelector:@selector(outlineView:selectionIndexesForProposedSelection:)])
            return [d outlineView:outline selectionIndexesForProposedSelection:rows];
        if ([d respondsToSelector:@selector(outlineView:shouldSelectItem:)] &&
            ![d outlineView:outline shouldSelectItem:[outline itemAtRow:row]]) return [NSIndexSet indexSet];
        return rows;
    }
    if ([d respondsToSelector:@selector(selectionShouldChangeInTableView:)] &&
        ![d selectionShouldChangeInTableView:table]) return [NSIndexSet indexSet];
    if ([d respondsToSelector:@selector(tableView:selectionIndexesForProposedSelection:)])
        return [d tableView:table selectionIndexesForProposedSelection:rows];
    if ([d respondsToSelector:@selector(tableView:shouldSelectRow:)] &&
        ![d tableView:table shouldSelectRow:row]) return [NSIndexSet indexSet];
    return rows;
}

/*
 * on_ui_select_on_mouse_down — select the clicked row before super tracks.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_select_on_mouse_down(NSTableView *table, NSEvent *event)
{
    NSEventModifierFlags mods = event.modifierFlags & (NSEventModifierFlagCommand |
                                                       NSEventModifierFlagShift |
                                                       NSEventModifierFlagControl);
    NSInteger row = [table rowAtPoint:[table convertPoint:event.locationInWindow fromView:nil]];
    if (mods != 0 || event.clickCount != 1 || row < 0 || [table isRowSelected:row]) return;
    NSIndexSet *rows = vetted_selection(table, row);
    if (rows.count > 0) [table selectRowIndexes:rows byExtendingSelection:NO];
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

NSString *const on_ui_date_list = @"MMM d, yyyy HH:mm";
NSString *const on_ui_date_day  = @"MMM d, yyyy";
NSString *const on_ui_date_iso  = @"yyyy-MM-dd";

/*
 * on_ui_format_date — a date via the shared formatter for its pattern.
 * Inputs/output are documented in ui_util.h.
 */
NSString *
on_ui_format_date(int64_t unix_time, NSString *format)
{
    static NSMutableDictionary<NSString *, NSDateFormatter *> *formatters;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ formatters = [NSMutableDictionary dictionary]; });
    NSDateFormatter *fmt;
    @synchronized (formatters) {
        fmt = formatters[format];
        if (fmt == nil) {
            fmt = [[NSDateFormatter alloc] init];
            fmt.dateFormat = format;
            formatters[format] = fmt;
        }
    }
    return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)unix_time]];
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

/*
 * on_ui_select_radio — one radio of a group on.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_select_radio(NSArray<NSButton *> *group, NSButton *chosen)
{
    for (NSButton *b in group) b.state = b == chosen ? NSControlStateValueOn : NSControlStateValueOff;
}

/*
 * on_ui_pin_edges — four edges on another view's.
 * Inputs/output are documented in ui_util.h.
 */
void
on_ui_pin_edges(NSView *view, NSView *to)
{
    [NSLayoutConstraint activateConstraints:@[
        [view.topAnchor      constraintEqualToAnchor:to.topAnchor],
        [view.bottomAnchor   constraintEqualToAnchor:to.bottomAnchor],
        [view.leadingAnchor  constraintEqualToAnchor:to.leadingAnchor],
        [view.trailingAnchor constraintEqualToAnchor:to.trailingAnchor],
    ]];
}
