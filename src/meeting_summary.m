/*
 * meeting_summary.m — the summary of the last meeting of a series
 * (see meeting_summary.h for the contract and why the series is found by
 * title)
 *
 * One NotesMeetingSummary is one panel on screen: the tool panel, the text
 * view it fills and the command while it runs.  The module's file-static
 * `summaries` is the only owner — the panel's window closing is what drops
 * it, and that also terminates a command still running, so a summary the
 * user walked away from stops costing anything.
 */

#import "meeting_summary.h"

#import "ai_run.h"
#import "look.h"
#import "shared_tool_panel.h"
#import "shared_ui.h"

#include "app_core.h"
#include "db.h"

#include <glib.h>

/* What the command is asked to do with the previous meeting's note. */
static NSString * const summary_request =
    @"Summarize the notes below from the previous meeting in a recurring series, for someone who is about "
     "to attend the next meeting. Cover what was decided, what is still open, and the action items with "
     "their owners. Keep it short.\n\n";

/* While the command runs. */
static NSString * const summary_waiting = @"Summarizing the last meeting…";

/*
 * previous_note — the note this one follows in its series: the most recent
 * note with the same title from BEFORE it.  "Before" is (created_at, id),
 * so two notes made in the same second are still ordered, a note never
 * finds itself, and opening an older note of a series looks further back
 * rather than forward.  Trashed notes are not in the listing and so are
 * never summarised.
 *
 * Inputs:
 *   db      — the database
 *   note_id — the note being written now (it has a row: an editor is open
 *             on it)
 *   title   — its title as it stands, which may not be the title its row
 *             still holds
 *   when    — receives the predecessor's created_at, 0 when there is none
 *
 * Output: the predecessor's note id, or 0 when the series has no earlier
 *         note.
 */
static gint64
previous_note(OnDatabase *db, gint64 note_id, NSString *title, gint64 *when)
{
    const char *want   = title.UTF8String;
    gint64      mine   = 0;          /* this note's own place in the series */
    OnNoteMeta *meta   = on_db_note_get(db, note_id);
    if (meta != NULL) {
        mine = meta->created_at;
        on_db_note_meta_free(meta);
    }

    gint64 best = 0, best_at = 0;
    GList *notes = on_db_note_list_recent(db);
    for (GList *l = notes; l != NULL; l = l->next) {
        const OnNoteMeta *m = l->data;
        if (m->id == note_id || g_strcmp0(m->title, want) != 0) continue;
        if (m->created_at > mine || (m->created_at == mine && m->id > note_id)) continue;
        if (m->created_at > best_at || (m->created_at == best_at && m->id > best)) {
            best    = m->id;
            best_at = m->created_at;
        }
    }
    on_db_note_list_free(notes);
    *when = best_at;
    return best;
}

/*
 * panel_title — the panel's title bar: the meeting, the day of the note
 * being summarised and what the panel holds.
 *
 * Inputs:
 *   title — the meeting's title
 *   when  — the summarised note's created_at
 *
 * Output: the line.
 */
static NSString *
panel_title(NSString *title, gint64 when)
{
    return [NSString stringWithFormat:@"%@ — %@ — Summary", title,
                                      shared_ui_format_date(when, shared_ui_date_day)];
}

/* ── One summary on screen ──────────────────────────────────────────────── */

/*
 * NotesMeetingSummary — the panel, the text view it fills and the command
 * while it runs.  Held by `summaries` from the moment it is shown until
 * its window closes.
 */
@interface NotesMeetingSummary : NSObject
@property (nonatomic, assign) gint64           note_id;  /* whose editor asked */
@property (nonatomic, strong) SharedToolPanel *panel;
@property (nonatomic, strong) NSTextView      *text;
@property (nonatomic, strong) NSTask          *task;     /* the command, while it runs */
@end

/* The summaries on screen.  Main thread only. */
static NSMutableArray<NotesMeetingSummary *> *summaries;

/*
 * summary_for — the summary already on screen for a note, if there is one.
 *
 * Inputs:
 *   note_id — the note whose editor asked
 *
 * Output: the summary, or nil.
 */
static NotesMeetingSummary *
summary_for(gint64 note_id)
{
    for (NotesMeetingSummary *s in summaries) if (s.note_id == note_id) return s;
    return nil;
}

@implementation NotesMeetingSummary

/*
 * init_with_note:title:when:over: — build the panel round a read-only
 * scrolling text view, show it over a window and say that the summary is
 * coming.  The panel has one button, Close; the text is selectable, so ⌘C
 * copies it.  The window does not resize, so the text view carries the
 * size.
 *
 * Inputs:
 *   note_id — the note whose editor asked, so a second click on its clock
 *             finds this panel instead of starting another command
 *   title   — the meeting's title
 *   when    — the summarised note's created_at
 *   over    — the window to centre over; nil = the screen
 *
 * Output: the summary, its panel on screen.
 */
- (instancetype)
init_with_note:(gint64)note_id title:(NSString *)title when:(gint64)when over:(NSWindow *)over
{
    if (!(self = [super init])) return nil;
    _note_id = note_id;

    NSScrollView *scroll = [NSTextView scrollableTextView];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.widthAnchor  constraintEqualToConstant:on_look_summary_w],
        [scroll.heightAnchor constraintEqualToConstant:on_look_summary_h],
    ]];
    _text = scroll.documentView;
    _text.identifier         = @"meeting-summary-text";
    _text.editable           = NO;
    _text.richText           = NO;
    _text.font               = [NSFont systemFontOfSize:shared_look_ui_font];
    _text.textContainerInset = NSMakeSize(6, 4);
    _text.string             = summary_waiting;

    _panel = [SharedToolPanel panel_with_title:panel_title(title, when)
                                      headline:@""          /* the title bar says what it is */
                                      subtitle:nil
                                        symbol:nil
                                       content:scroll
                                       buttons:@[@"Close"]
                                   destructive:-1
                                       handler:^NSString *(NSInteger index) { return nil; }];
    /* The panel answers no one about being dismissed, so the window's own
     * close notification is what tells this summary it is over. */
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(window_closed:)
                                               name:NSWindowWillCloseNotification object:_panel.window];
    [_panel show_over:over];
    return self;
}

/*
 * run:prompt: — start the command with the prompt on its stdin; what it
 * writes lands in the text view.  A command that will not start says so
 * there instead.
 *
 * Inputs:
 *   command — the command line (run by /bin/sh)
 *   prompt  — the text on its stdin
 */
- (void)
run:(NSString *)command prompt:(NSString *)prompt
{
    __weak NotesMeetingSummary *ws = self;
    NSError *error = nil;
    self.task = on_ai_run(command, prompt, &error, ^(NSTask *task, NSString *output) {
        [ws finished:task output:output];
    });
    if (self.task == nil)
        [self show:[NSString stringWithFormat:@"Could not start the summary command:\n\n%@\n\n%@\n\n"
                                               "Settings → AI Features sets the command.",
                                              command, error.localizedDescription]];
}

/*
 * show: — put a line of text in the panel, from the top.
 *
 * Inputs:
 *   text — what the panel says
 */
- (void)
show:(NSString *)text
{
    self.text.string = text;
    [self.text scrollRangeToVisible:NSMakeRange(0, 0)];
}

/*
 * finished:output: — the command exited: show what it wrote.  A panel
 * that has closed meanwhile has let go of its task and drops the result.
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
    [self show:output ?: @"The summary command wrote nothing."];
}

/*
 * window_closed: — the panel's window closed (Close, Escape or the close
 * box): stop observing, terminate a command still running and let the
 * module's owner go of this summary.  Safe to run once only, which is all
 * one window close gives it.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
window_closed:(NSNotification *)note
{
    [NSNotificationCenter.defaultCenter removeObserver:self name:NSWindowWillCloseNotification
                                                object:self.panel.window];
    [self.task terminate];
    self.task  = nil;
    self.panel = nil;
    self.text  = nil;
    [summaries removeObject:self];    /* last: may release self */
}

@end

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_meeting_summary_enabled — see meeting_summary.h.
 */
BOOL
on_meeting_summary_enabled(OnApp *app)
{
    return app->ai_enabled && app->meeting_summary;
}

/*
 * on_meeting_summary_can_run — see meeting_summary.h.
 */
BOOL
on_meeting_summary_can_run(OnApp *app, NSString *title)
{
    return on_meeting_summary_enabled(app) && title.length > 0 &&
           ![title isEqualToString:@(SHARED_DB_NOTE_DEFAULT_TITLE)];
}

/*
 * on_meeting_summary_show — see meeting_summary.h.
 */
void
on_meeting_summary_show(OnApp *app, gint64 note_id, NSString *title, NSWindow *over)
{
    NotesMeetingSummary *open = summary_for(note_id);
    if (open != nil) {                   /* one summary per note: no second command */
        [open.panel.window makeKeyAndOrderFront:nil];
        return;
    }
    gint64 when = 0;
    gint64 last = previous_note(app->db, note_id, title, &when);
    if (last == 0) {
        shared_ui_notice(over, @"No earlier meeting",
                         [NSString stringWithFormat:@"There is no older note titled “%@”. The summary is of the "
                                                     "most recent note with the same title, which is how the "
                                                     "earlier meetings of a series are found.", title], nil);
        return;
    }
    NSString *prompt = on_ai_prompt(summary_request, app->db, @[@(last)]);
    if (prompt == nil) {
        shared_ui_notice(over, @"Could not read the last meeting",
                         @"The note of the previous meeting could not be read.", nil);
        return;
    }
    if (summaries == nil) summaries = [NSMutableArray array];
    NotesMeetingSummary *summary = [[NotesMeetingSummary alloc] init_with_note:note_id title:title
                                                                           when:when over:over];
    [summaries addObject:summary];
    [summary run:@(app->meeting_command) prompt:prompt];
}
