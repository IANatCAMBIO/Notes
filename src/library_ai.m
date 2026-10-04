/*
 * library_ai.m — the AI summary pane and its command.
 *
 * The pane sits under the notes list inside the library window.  It is
 * hidden until the user triggers the "ai-summary" command, at which point
 * it shows a spinner while a subprocess (ai_command from settings) runs,
 * then displays the result in a scrolling read-only text view.
 *
 * Phase 1: subprocess integration is stubbed — the pane appears with the
 * spinner and a placeholder message.  The command registration, the
 * ai_enabled gate, and the notify_ai_changed hook are all wired up so the
 * library window can show/hide the toolbar item correctly.
 *
 * The module registers the "ai-summary" command and sets
 * OnApp.notify_ai_changed at startup (on_library_ai_install).
 */

#import "library_ai.h"

#import "look.h"

#include "app_core.h"
#include "commands.h"
#include "config.h"
#include "library_state.h"

/* ── Forward declarations ───────────────────────────────────────────────── */

@interface NotesAIPane : NSView
@property (nonatomic, weak) NotesLibrary *lib;
- (instancetype)initWithLibrary:(NotesLibrary *)lib;
- (void)start_summary;
- (void)clear;
@end

/* ── File-static pane registry ──────────────────────────────────────────── */

/* All live panes, kept so on_library_ai_changed can update them all. */
static NSMutableArray<NotesAIPane *> *ai_panes;

/* ── NotesAIPane ────────────────────────────────────────────────────────── */

@implementation NotesAIPane {
    NSProgressIndicator *_spinner;
    NSScrollView        *_scroll;
    NSTextView          *_text_view;
    NSButton            *_close_btn;
}

/*
 * initWithLibrary: — build the AI summary pane (initially hidden).
 *
 * Inputs:
 *   lib — the library (app, selection accessed through it)
 *
 * Output: the pane view.
 */
- (instancetype)
initWithLibrary:(NotesLibrary *)lib
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _lib = lib;
    self.hidden = YES;

    /* Spinner */
    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;

    /* Read-only text view */
    _scroll = [[NSScrollView alloc] init];
    _text_view = [[NSTextView alloc] init];
    _text_view.editable = NO;
    _text_view.selectable = YES;
    _text_view.drawsBackground = NO;
    _text_view.font = [NSFont systemFontOfSize:(CGFloat)on_look_font_size];
    _text_view.textContainerInset = NSMakeSize(4, 4);
    _scroll.documentView = _scroll.documentView ?: _text_view;
    _scroll.documentView = _text_view;
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers  = YES;
    _scroll.drawsBackground = NO;

    /* Close button */
    _close_btn = [NSButton buttonWithTitle:@"✕"
                                    target:self
                                    action:@selector(close_pane:)];
    _close_btn.bezelStyle = NSBezelStyleInline;
    _close_btn.controlSize = NSControlSizeSmall;

    /* Header: spinner + title + close */
    NSTextField *title_label = [NSTextField labelWithString:@"AI Summary"];
    title_label.font = [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize];

    NSStackView *header = [NSStackView stackViewWithViews:@[_spinner, title_label]];
    header.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    header.alignment   = NSLayoutAttributeCenterY;
    header.spacing = 4;

    NSView *spacer = [NSView new];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    [header addArrangedSubview:spacer];
    [header addArrangedSubview:_close_btn];

    NSStackView *sv = [NSStackView stackViewWithViews:@[header, _scroll]];
    sv.orientation = NSUserInterfaceLayoutOrientationVertical;
    sv.alignment   = NSLayoutAttributeLeading;
    sv.spacing = 4;
    sv.edgeInsets = NSEdgeInsetsMake(4, 8, 4, 8);
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                            forOrientation:NSLayoutConstraintOrientationVertical];

    [self addSubview:sv];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [sv.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [sv.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [sv.topAnchor      constraintEqualToAnchor:self.topAnchor],
        [sv.bottomAnchor   constraintEqualToAnchor:self.bottomAnchor],
    ]];

    /* Height constraint: pane is at most 200 pt tall. */
    [self.heightAnchor constraintLessThanOrEqualToConstant:200].active = YES;

    return self;
}

/* ── Control ─────────────────────────────────────────────────────────────── */

/*
 * start_summary — show the pane, start the spinner, and run the AI subprocess.
 *
 * Phase 1: the subprocess is not implemented; a placeholder message is shown
 * after a brief delay to exercise the show/spinner/text path.
 */
- (void)
start_summary
{
    self.hidden = NO;
    [_text_view setString:@""];
    [_spinner startAnimation:nil];

    /* Phase 1 placeholder — replace with real subprocess when available. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [self->_spinner stopAnimation:nil];
        OnApp *app = self->_lib.app;
        if (app && !app->ai_enabled) {
            [self->_text_view setString:@"AI summary is disabled. Enable it in Settings."];
        } else if (app && (!app->ai_command || !app->ai_command[0])) {
            [self->_text_view setString:@"No AI command configured. Set one in Settings → AI."];
        } else {
            [self->_text_view setString:@"(AI summary subprocess not yet wired up.)"];
        }
    });
}

/*
 * clear — hide the pane and reset the text.
 */
- (void)
clear
{
    [_spinner stopAnimation:nil];
    [_text_view setString:@""];
    self.hidden = YES;
}

/*
 * close_pane: — the close button: hide the pane.
 *
 * Inputs:
 *   sender — unused
 */
- (void)
close_pane:(id)sender
{
    (void)sender;
    [self clear];
}

@end

/* ── notify_ai_changed hook ─────────────────────────────────────────────── */

/*
 * ai_changed — OnApp.notify_ai_changed: update every live pane when the
 * ai_enabled preference changes.
 *
 * Inputs:
 *   app — the application context
 */
static void
ai_changed(OnApp *app)
{
    for (NotesAIPane *pane in ai_panes) {
        if (!app->ai_enabled && !pane.hidden) [pane clear];
    }
}

/* ── Command handlers ───────────────────────────────────────────────────── */

/*
 * run_ai_summary — start an AI summary for the currently selected note.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — unused
 */
static void
run_ai_summary(id target, id sender)
{
    (void)sender;
    NotesLibrary *lib = target;
    /* Find the pane for this library instance. */
    for (NotesAIPane *pane in ai_panes) {
        if (pane.lib == lib) {
            [pane start_summary];
            return;
        }
    }
}

/*
 * validate_ai_summary — enabled only when ai_enabled is set and a DB is open.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — unused
 *
 * Output: YES when usable.
 */
static BOOL
validate_ai_summary(id target, id item)
{
    (void)item;
    NotesLibrary *lib = target;
    OnApp *app = lib.app;
    return app && app->db && app->ai_enabled;
}

static const on_command_handler_t ai_handlers[] = {
    { @"ai-summary", run_ai_summary, validate_ai_summary },
};

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_library_ai_install — see library_ai.h.
 */
void
on_library_ai_install(NotesLibrary *lib)
{
    if (!ai_panes) ai_panes = [NSMutableArray array];

    on_library_add_commands(ai_handlers,
                            sizeof ai_handlers / sizeof *ai_handlers);

    OnApp *app = lib.app;
    if (app) app->notify_ai_changed = ai_changed;
}

/*
 * on_library_ai_pane_new — see library_ai.h.
 */
NSView *
on_library_ai_pane_new(NotesLibrary *lib)
{
    NotesAIPane *pane = [[NotesAIPane alloc] initWithLibrary:lib];
    [ai_panes addObject:pane];
    return pane;
}
