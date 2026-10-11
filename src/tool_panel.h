/*
 * tool_panel.h — the app's own dialog: a small floating tool window, in the
 * style of the Files and Tasks apps.  Wherever a native dialog (NSAlert,
 * an alert sheet) would appear, Notes uses this instead.
 *
 * A tool panel is a utility-titlebar panel with a solid blue hero strip
 * (icon, bold title, subtitle) at the top, the caller's controls below it,
 * and a button bar at the bottom.  It is not modal: it floats above
 * ordinary windows until the user clicks a button, presses Return (the
 * default button), or presses Escape or the close box (which dismiss it).
 * All methods must be called on the main thread.
 */
#import <Cocoa/Cocoa.h>

@interface NotesToolPanel : NSObject

/*
 * panel_with_title:hero_title:subtitle:symbol:content:primary_title:primary_handler: —
 * build a panel that asks for something: the caller's controls above
 * Cancel and a primary button.  show_over: displays it; it keeps itself
 * alive while it is on screen.
 *
 * Inputs:
 *   window_title    — title bar text
 *   hero_title      — bold line in the hero strip
 *   subtitle        — second line in the hero strip; problems show there too
 *   symbol          — SF Symbols name for the hero icon; may be nil
 *   content         — the controls, sized by their own fittingSize (fixed
 *                     widths and heights); not retained after close
 *   primary_title   — the default button's label, e.g. @"Create"
 *   primary_handler — called when the primary button is clicked; returns
 *                     nil to accept (the panel closes) or a one-line problem
 *                     to show in the hero strip (the panel stays open)
 *
 * Output: the panel, not yet shown.
 */
+ (instancetype)panel_with_title:(NSString *)window_title
                      hero_title:(NSString *)hero_title
                        subtitle:(NSString *)subtitle
                          symbol:(NSString *)symbol
                         content:(NSView *)content
                   primary_title:(NSString *)primary_title
                 primary_handler:(NSString *(^)(void))primary_handler;

/*
 * panel_with_title:hero_title:subtitle:symbol:content:buttons:handler: —
 * build a panel with the caller's controls and any buttons (the due-date
 * panel's Set / Clear / Cancel).
 *
 * Inputs:
 *   window_title, hero_title, subtitle, symbol, content — as above
 *   buttons — button labels, at least one: [0] is the default (Return) and
 *             sits rightmost, the rest follow to its left
 *   handler — called with the index of the clicked button; returns nil to
 *             close the panel or a one-line problem to show (the panel
 *             stays open)
 *
 * Output: the panel, not yet shown.
 */
+ (instancetype)panel_with_title:(NSString *)window_title
                      hero_title:(NSString *)hero_title
                        subtitle:(NSString *)subtitle
                          symbol:(NSString *)symbol
                         content:(NSView *)content
                         buttons:(NSArray<NSString *> *)buttons
                         handler:(NSString *(^)(NSInteger index))handler;

/*
 * show_message:message:symbol:buttons:over:handler: — show a message in a
 * tool panel and return at once: the replacement for a native alert.
 *
 * Inputs:
 *   title   — headline, in the hero strip
 *   message — wrapped body text (long text scrolls); may be empty
 *   symbol  — SF Symbols name for the hero icon; may be nil
 *   buttons — button labels, at least one: [0] is the default (Return) and
 *             sits rightmost, the rest follow to its left
 *   parent  — window to centre over; nil = the screen
 *   handler — called once on the main thread with the index of the clicked
 *             button, or -1 when the panel was dismissed (Escape or the
 *             close box) without one; may be nil
 */
+ (void)show_message:(NSString *)title
             message:(NSString *)message
              symbol:(NSString *)symbol
             buttons:(NSArray<NSString *> *)buttons
                over:(NSWindow *)parent
             handler:(void (^)(NSInteger index))handler;

/*
 * show_over: — centre the panel over a window (the screen if nil), bring it
 * forward and make it key.
 *
 * Inputs:
 *   parent — window to centre over; may be nil
 */
- (void)show_over:(NSWindow *)parent;

/*
 * window — the underlying panel, e.g. to set the first responder.
 *
 * Output: the NSPanel.
 */
- (NSPanel *)window;

@end
