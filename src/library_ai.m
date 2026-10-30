/*
 * library_ai.m — the AI summary: the pane under the notes
 * pane and the command that fills it.
 *
 * Specification: the GTK build's src/library_window.c, the AI
 * section (run_ai_summary, build_ai_pane, on_ai_button_clicked).
 *
 * The AI Summary command summarises the SELECTED notes: their text and
 * action items go, after a short request, to the configured command's
 * stdin, and what it writes to stdout — or to stderr, when stdout stays
 * empty — is shown in the pane.
 * The prompt and the command are ai_run.h's, which the editor's
 * last-meeting summary shares.  The command runs off the main thread;
 * while it does, the status bar shows a throbber and a second request is
 * ignored.  The pane sits under the notes in a split the user can resize,
 * with Copy and ✕ in its header.
 */

#import "library_ai.h"

#import "library_notes.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "ai_run.h"
#include "app_core.h"
#include "commands.h"
#include "db.h"

/* How often the status-bar throbber moves (seconds). */
static const double throbber_s = 0.12;

/* The pane's height when it first opens, if the notes have room. */
static const CGFloat first_pane_h = 220.0;

/* The prompt's opening (GTK run_ai_summary's Normal mode). */
static NSString * const prompt_normal = @"Provide me a brief summary of these notes.\n\n";

/* ── The pane ───────────────────────────────────────────────────────────── */

@interface NotesAIPane : NSView
@property (nonatomic, weak)   NotesLibrary *lib;
@property (nonatomic, strong) NSTextView   *text;
@property (nonatomic, strong) NSTask       *task;       /* the command, while it runs */
@property (nonatomic, strong) NSTimer      *throbber;
@property (nonatomic, assign) NSUInteger    throb_step;
@end

/* The pane of the (one) library window. */
static __weak NotesAIPane *ai_pane;

@implementation NotesAIPane

/*
 * init_with_lib: — the header (title, Copy, ✕) over the read-only text;
 * hidden until a summary is asked for.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the pane.
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib
{
    if (!(self = [super initWithFrame:NSZeroRect])) return nil;
    _lib = lib;
    self.hidden = YES;

    NSTextField *title = [NSTextField labelWithString:@"AI Summary"];
    title.font = [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize];
    NSButton *copy  = [NSButton buttonWithTitle:@"Copy" target:self action:@selector(copy_text:)];
    NSButton *close = [NSButton buttonWithTitle:@"✕" target:self action:@selector(close_pane:)];
    copy.toolTip  = @"Copy summary to clipboard";
    close.toolTip = @"Close AI summary";
    for (NSButton *b in @[copy, close]) {
        b.bordered    = NO;
        b.controlSize = NSControlSizeSmall;
        b.font        = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    }
    NSStackView *header = [NSStackView stackViewWithViews:@[title]];
    [header addView:copy inGravity:NSStackViewGravityTrailing];      /* Copy, then ✕ at the edge */
    [header addView:close inGravity:NSStackViewGravityTrailing];
    header.spacing    = 4;
    header.edgeInsets = NSEdgeInsetsMake(1, 6, 1, 2);

    NSScrollView *scroll = [NSTextView scrollableTextView];
    self.text = scroll.documentView;
    self.text.identifier         = @"ai-text";
    self.text.editable           = NO;
    self.text.richText           = NO;
    self.text.font               = [NSFont systemFontOfSize:shared_look_ui_font];
    self.text.textContainerInset = NSMakeSize(8, 4);

    NSBox *rule = [[NSBox alloc] initWithFrame:NSZeroRect];
    rule.boxType = NSBoxSeparator;
    for (NSView *v in @[rule, header, scroll]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [rule.topAnchor        constraintEqualToAnchor:self.topAnchor],
        [rule.leadingAnchor    constraintEqualToAnchor:self.leadingAnchor],
        [rule.trailingAnchor   constraintEqualToAnchor:self.trailingAnchor],
        [header.topAnchor      constraintEqualToAnchor:rule.bottomAnchor],
        [header.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [scroll.topAnchor      constraintEqualToAnchor:header.bottomAnchor],
        [scroll.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [scroll.bottomAnchor   constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

/*
 * show — open the pane, at a comfortable height the first time.
 */
- (void)
show
{
    if (!self.hidden) return;
    self.hidden = NO;
    NSSplitView *split = (NSSplitView *)self.superview;
    if (![split isKindOfClass:NSSplitView.class]) return;
    [split layoutSubtreeIfNeeded];
    CGFloat total = NSHeight(split.bounds);
    if (total > 300) [split setPosition:total - first_pane_h ofDividerAtIndex:0];
}

/*
 * throb: — move the status-bar throbber on.
 *
 * Inputs:
 *   timer — the throbber (unused)
 */
- (void)
throb:(NSTimer *)timer
{
    static NSString * const frames[] = { @"⣾", @"⣽", @"⣻", @"⢿", @"⡿", @"⣟", @"⣯", @"⣷" };
    on_app_status(self.lib.app, "Generating AI Summary %s", frames[self.throb_step++ % 8].UTF8String);
}

/*
 * stop_throbber — the status bar stops moving.
 */
- (void)
stop_throbber
{
    [self.throbber invalidate];
    self.throbber = nil;
}

/*
 * fail: — say why there is no summary.
 *
 * Inputs:
 *   message — the reason
 */
- (void)
fail:(NSString *)message
{
    [self stop_throbber];
    self.text.string = message;
}

/*
 * summarise — run a summary of the selected notes (GTK on_ai_button_clicked
 * and run_ai_summary).  Ignored while one is running.
 */
- (void)
summarise
{
    if (self.task) return;
    [self show];
    self.text.string = @"";
    self.throb_step  = 0;
    [self throb:nil];
    self.throbber = [NSTimer scheduledTimerWithTimeInterval:throbber_s target:self selector:@selector(throb:)
                                                   userInfo:nil repeats:YES];

    NotesLibrary *lib = self.lib;
    OnApp        *app = lib.app;
    if (app->ai_command == NULL || *app->ai_command == '\0') {
        [self fail:@"No AI command configured. Please set one in Settings → AI Features."];
        return;
    }
    if (lib.sel_kind == on_sb_trash || lib.sel_kind == on_sb_actions) {
        [self fail:@"Select notes to summarize."];
        return;
    }
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    if (ids.count == 0) {
        [self fail:@"Select one or more notes to summarize."];
        return;
    }
    NSString *prompt = on_ai_prompt(prompt_normal, app->db, ids);
    if (prompt == nil) {
        [self fail:@"No notes to summarize."];
        return;
    }
    __weak NotesAIPane *ws = self;
    NSError *error = nil;
    self.task = on_ai_run(@(app->ai_command), prompt, &error, ^(NSTask *task, NSString *output) {
        [ws finished:task output:output];
    });
    if (self.task == nil)
        [self fail:[NSString stringWithFormat:@"Failed to start AI command: %@", error.localizedDescription]];
}

/*
 * finished:output: — the command exited: show what it wrote, from the
 * top.  A pane whose window closed meanwhile, or whose summary was
 * replaced by a later one, drops the result.
 *
 * Inputs:
 *   task   — the finished command
 *   output — its answer (ai_run.h), or nil when it wrote nothing
 */
- (void)
finished:(NSTask *)task output:(NSString *)output
{
    if (task != self.task) return;
    self.task = nil;
    [self stop_throbber];
    on_app_status(self.lib.app, "Summary generation complete");
    self.text.string = output ?: @"(no output)";
    [self.text scrollRangeToVisible:NSMakeRange(0, 0)];
}

/*
 * copy_text: — the summary onto the clipboard.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
copy_text:(id)sender
{
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:self.text.string forType:NSPasteboardTypeString];
}

/*
 * close_pane: — ✕ hides the pane (a running summary still lands in it).
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
close_pane:(id)sender
{
    self.hidden = YES;
}

/*
 * viewDidMoveToWindow — a pane leaving its window stops its command.
 */
- (void)
viewDidMoveToWindow
{
    if (self.window != nil) return;
    [self stop_throbber];
    [self.task terminate];
    self.task = nil;
}

@end

/* ── Command and hook ───────────────────────────────────────────────────── */

/*
 * run_ai_summary — "ai-summary": summarise the selected notes in the pane.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_ai_summary(id target, id sender)
{
    [ai_pane summarise];
}

/*
 * validate_ai_summary — usable only while AI is enabled (with the library
 * closed, the command opens it first: library_state.m).
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: YES to enable it.
 */
static BOOL
validate_ai_summary(id target, id item, SharedCommandState *state)
{
    return ((NotesLibrary *)target).app->ai_enabled;
}

/*
 * ai_changed — OnApp.notify_ai_changed: with AI turned off the pane goes
 * away and so does the toolbar's AI Summary; turned on, the button is
 * back.
 *
 * Inputs:
 *   app — the application context
 */
static void
ai_changed(OnApp *app)
{
    if (!app->ai_enabled) ai_pane.hidden = YES;
    on_library_window_ai_changed(on_library());
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const shared_command_handler_t ai_handlers[] = {
    { @"ai-summary", run_ai_summary, validate_ai_summary },
};

/*
 * on_library_ai_install — see library_ai.h.
 */
void
on_library_ai_install(NotesLibrary *lib)
{
    on_library_add_commands(ai_handlers, sizeof ai_handlers / sizeof *ai_handlers);
    lib.app->notify_ai_changed = ai_changed;
}

/*
 * on_library_ai_pane_new — see library_ai.h.
 */
NSView *
on_library_ai_pane_new(NotesLibrary *lib)
{
    NotesAIPane *pane = [[NotesAIPane alloc] init_with_lib:lib];
    ai_pane = pane;
    return pane;
}
