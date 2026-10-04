/*
 * search_window.m — the full-text search window.
 *
 * Each call to on_search_window_open creates a new NSWindow; windows are
 * independent and may exist in parallel (one per search the user has
 * running).  The window presents:
 *
 *   [ scope pop-up ]  [ search field ]
 *   ─────────────────────────────────
 *   result rows: path · note-modified-date
 *   ─────────────────────────────────
 *   status bar ("3 matches · 0.1 s")
 *
 * Typing is debounced by 200 ms.  A running job is cancelled before a new
 * one starts.  Double-clicking a row opens the note.
 *
 * The module registers "search" (Find Notes…, ⌥⌘F), "find" (Find in
 * library, ⌘F when the library window is key), and "search-here"
 * (Search in Selection…, ⌥⌘G) with the library at startup.
 */

#import "search_window.h"

#import "look.h"

#include "app_core.h"
#include "commands.h"
#include "db.h"
#include "editor_window.h"
#include "library_state.h"
#include "search_job.h"
#include "search_query.h"

#include <mach/mach_time.h>
#include <stdio.h>

/* ── Column identifiers ─────────────────────────────────────────────────── */

static NSString * const kColPath = @"path";
static NSString * const kColWhen = @"when";

/* ── Forward declarations ───────────────────────────────────────────────── */

@interface NotesSearchWindow : NSWindowController
    <NSWindowDelegate, NSTableViewDelegate, NSTableViewDataSource>
- (instancetype)initWithLibrary:(NotesLibrary *)lib
                scopeToSelection:(BOOL)scope_to_selection
                           query:(NSString *)query;
@end

/* ── NotesSearchWindow ──────────────────────────────────────────────────── */

@implementation NotesSearchWindow {
    NotesLibrary           *_lib;
    NSSearchField          *_search_field;
    NSPopUpButton          *_scope_pop;
    NSTableView            *_table;
    NSTextField            *_status;

    /* Search state */
    on_search_job_t        *_job;           /* nil when idle */
    NSMutableArray<NSArray *> *_hits; /* {NSNumber(id), path, when} triples */
    NSTimer                *_debounce;
    uint64_t                _job_start;     /* mach_absolute_time at job start */
}

/*
 * initWithLibrary:scopeToSelection:query: — build and show a search window.
 *
 * Inputs:
 *   lib              — the library
 *   scope_to_selection — YES to pre-select "Selected Folder/Tag" scope
 *   query            — pre-populate the search field; nil/@"" opens blank
 *
 * Output: the window controller.
 */
- (instancetype)
initWithLibrary:(NotesLibrary *)lib
  scopeToSelection:(BOOL)scope_to_selection
             query:(NSString *)query
{
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, on_look_search_w, on_look_search_h)
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable |
                             NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = @"Search Notes";
    win.releasedWhenClosed = NO;
    win.minSize = NSMakeSize(400, 260);
    [win center];

    self = [super initWithWindow:win];
    if (!self) return nil;

    _lib  = lib;
    _hits = [NSMutableArray array];
    win.delegate = self;

    [self build_ui:scope_to_selection query:query];
    [win makeKeyAndOrderFront:nil];
    if (query.length) [self start_search];

    return self;
}

/*
 * build_ui:query: — lay out the window's controls.
 *
 * Inputs:
 *   scope_to_selection — initial scope pop selection
 *   query              — initial search-field text
 */
- (void)
build_ui:(BOOL)scope_to_selection
    query:(NSString *)query
{
    /* Scope pop-up */
    _scope_pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_scope_pop addItemWithTitle:@"All Notes"];
    [_scope_pop addItemWithTitle:@"Selected Folder / Tag"];
    if (scope_to_selection) [_scope_pop selectItemAtIndex:1];
    _scope_pop.target = self;
    _scope_pop.action = @selector(scope_changed:);

    /* Search field */
    _search_field = [[NSSearchField alloc] init];
    _search_field.placeholderString = @"Search";
    _search_field.stringValue = query ?: @"";
    _search_field.target = self;
    _search_field.action = @selector(search_field_changed:);
    _search_field.sendsWholeSearchString = NO;
    ((NSSearchFieldCell *)_search_field.cell).sendsSearchStringImmediately = YES;
    [_search_field setContentHuggingPriority:NSLayoutPriorityDefaultLow
                              forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* Top bar */
    NSStackView *top_bar = [NSStackView stackViewWithViews:@[_scope_pop, _search_field]];
    top_bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    top_bar.spacing = 8;

    /* Results table */
    _table = [[NSTableView alloc] init];
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.delegate   = self;
    _table.dataSource = self;
    _table.target  = self;
    _table.doubleAction = @selector(open_selected:);

    NSTableColumn *path_col = [[NSTableColumn alloc] initWithIdentifier:kColPath];
    path_col.title = @"Note";
    path_col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:path_col];

    NSTableColumn *when_col = [[NSTableColumn alloc] initWithIdentifier:kColWhen];
    when_col.title = @"Modified";
    when_col.width = 110;
    when_col.resizingMask = NSTableColumnNoResizing;
    [_table addTableColumn:when_col];

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationVertical];

    /* Status bar */
    _status = [NSTextField labelWithString:@"Type to search"];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

    /* Main stack */
    NSStackView *sv = [NSStackView stackViewWithViews:@[top_bar, scroll, _status]];
    sv.orientation = NSUserInterfaceLayoutOrientationVertical;
    sv.alignment   = NSLayoutAttributeLeading;
    sv.spacing = 8;
    sv.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);

    [sv setHuggingPriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationVertical];
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                           forOrientation:NSLayoutConstraintOrientationVertical];

    NSView *content = self.window.contentView;
    [content addSubview:sv];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [sv.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [sv.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [sv.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [sv.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

/* ── Search ─────────────────────────────────────────────────────────────── */

/*
 * search_field_changed: — the search field's text changed.
 *
 * Debounces: cancels any pending timer and schedules a 200 ms one.
 *
 * Inputs:
 *   sender — unused
 */
- (void)
search_field_changed:(id)sender
{
    (void)sender;
    [_debounce invalidate];
    _debounce = [NSTimer scheduledTimerWithTimeInterval:0.20
                                                 target:self
                                               selector:@selector(debounce_fired:)
                                               userInfo:nil
                                                repeats:NO];
}

/*
 * scope_changed: — the scope pop-up selection changed.
 *
 * Inputs:
 *   sender — unused
 *
 * Output: restarts the search with the new scope.
 */
- (void)
scope_changed:(id)sender
{
    (void)sender;
    [self start_search];
}

/*
 * debounce_fired: — the 200 ms debounce timer fired.
 *
 * Inputs:
 *   timer — invalidated on entry
 */
- (void)
debounce_fired:(NSTimer *)timer
{
    (void)timer;
    _debounce = nil;
    [self start_search];
}

/*
 * start_search — cancel any running job and launch a new one.
 *
 * Reads the search field and scope pop.  A blank or empty query clears
 * results and shows the placeholder status.
 */
- (void)
start_search
{
    [_debounce invalidate];
    _debounce = nil;

    if (_job) {
        on_search_job_cancel(_job);
        _job = NULL;
    }

    NSString *text = _search_field.stringValue;
    if (!text.length) {
        [_hits removeAllObjects];
        [_table reloadData];
        _status.stringValue = @"Type to search";
        return;
    }

    GError *err = NULL;
    OnQuery *q = on_query_new(text.UTF8String, NO, NO, &err);
    if (!q) {
        NSString *msg = err ? @(err->message) : @"Invalid search.";
        if (err) g_error_free(err);
        _status.stringValue = msg;
        return;
    }

    BOOL scoped = (_scope_pop.indexOfSelectedItem == 1);
    OnSearchScope scope = ON_SCOPE_FOLDER;
    gint64 scope_id = 0;
    if (scoped) on_library_search_scope(_lib, &scope, &scope_id);

    OnApp *app = _lib.app;
    _job_start = mach_absolute_time();
    _job = on_search_job_start(app->db->path, q,
                               scoped, scope, scope_id,
                               search_done, (__bridge void *)self);
    _status.stringValue = @"Searching…";
}

/*
 * search_done — job completion callback, dispatched onto the main thread.
 *
 * Inputs:
 *   error — error message or NULL
 *   hits  — array of matching notes (borrowed)
 *   n     — hit count
 *   data  — the NotesSearchWindow, bridge-transferred
 */
static void
search_done(const gchar *error,
            const on_search_hit_t *const *hits, size_t n,
            gpointer data)
{
    NotesSearchWindow *self = (__bridge NotesSearchWindow *)data;
    self->_job = NULL;

    if (error) {
        self->_status.stringValue = @(error);
        [self->_hits removeAllObjects];
        [self->_table reloadData];
        return;
    }

    [self->_hits removeAllObjects];
    for (size_t i = 0; i < n; i++) {
        NSNumber *nid  = @(hits[i]->id);
        NSString *path = @(hits[i]->path ?: "");
        NSString *when = @(hits[i]->when ?: "");
        [self->_hits addObject:@[nid, path, when]];
    }
    [self->_table reloadData];

    /* Elapsed time in milliseconds */
    uint64_t elapsed = mach_absolute_time() - self->_job_start;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ms = (double)elapsed * tb.numer / tb.denom / 1e6;

    if (n == 0) {
        self->_status.stringValue = @"No matches";
    } else {
        char buf[80];
        snprintf(buf, sizeof buf, "%zu match%s · %.0f ms",
                 n, n == 1 ? "" : "es", ms);
        self->_status.stringValue = @(buf);
    }
}

/* ── Table open ─────────────────────────────────────────────────────────── */

/*
 * open_selected: — double-click on a result row: open the note in an editor.
 *
 * Inputs:
 *   sender — unused
 */
- (void)
open_selected:(id)sender
{
    (void)sender;
    NSInteger row = _table.clickedRow;
    if (row < 0 || row >= (NSInteger)_hits.count) return;

    NSArray *hit = _hits[(NSUInteger)row];
    gint64 note_id = [hit[0] longLongValue];
    if (note_id <= 0) return;

    NSString *term = _search_field.stringValue.length ? _search_field.stringValue : nil;
    on_editor_window_open(_lib.app, note_id, NotesEditorRevealNone, 0, term);
}

/* ── NSTableViewDataSource ──────────────────────────────────────────────── */

/*
 * numberOfRowsInTableView: — result count.
 *
 * Inputs:
 *   tv — unused
 *
 * Output: number of hits.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return (NSInteger)_hits.count;
}

/* ── NSTableViewDelegate ────────────────────────────────────────────────── */

/*
 * tableView:viewForTableColumn:row: — provide a cell view for a result.
 *
 * Inputs:
 *   tv     — the table
 *   col    — the column (path or when)
 *   row    — the row index
 *
 * Output: an NSTextField cell.
 */
- (NSView *)
tableView:(NSTableView *)tv
  viewForTableColumn:(NSTableColumn *)col
                 row:(NSInteger)row
{
    (void)tv;
    NSArray *hit = _hits[(NSUInteger)row];
    /* hit[0] = NSNumber(id), hit[1] = path, hit[2] = when */
    NSString *value = [col.identifier isEqualToString:kColPath] ? hit[1] : hit[2];

    NSTextField *cell = [tv makeViewWithIdentifier:col.identifier owner:self];
    if (!cell) {
        cell = [NSTextField labelWithString:value];
        cell.identifier = col.identifier;
        cell.lineBreakMode = NSLineBreakByTruncatingMiddle;
    } else {
        cell.stringValue = value;
    }
    return cell;
}

/* ── NSWindowDelegate ───────────────────────────────────────────────────── */

/*
 * windowWillClose: — cancel any in-flight search job.
 *
 * Inputs:
 *   notification — unused
 */
- (void)
windowWillClose:(NSNotification *)notification
{
    (void)notification;
    [_debounce invalidate];
    _debounce = nil;
    if (_job) {
        on_search_job_cancel(_job);
        _job = NULL;
    }
}

@end

/* ── Command handlers ───────────────────────────────────────────────────── */

/*
 * run_search — open a search window for all notes.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — unused
 */
static void
run_search(id target, id sender)
{
    (void)sender;
    on_search_window_open(target, NO, nil);
}

/*
 * run_search_here — open a search window scoped to the library selection.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — unused
 */
static void
run_search_here(id target, id sender)
{
    (void)sender;
    on_search_window_open(target, YES, nil);
}

/*
 * validate_search — disable if no database is open.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   item   — unused
 *
 * Output: YES when a database is open.
 */
static BOOL
validate_search(id target, id item)
{
    (void)item;
    NotesLibrary *lib = target;
    return lib.app && lib.app->db != NULL;
}

static const on_command_handler_t search_handlers[] = {
    { @"search",      run_search,      validate_search },
    { @"find",        run_search,      validate_search },
    { @"search-here", run_search_here, validate_search },
};

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * on_search_install — see search_window.h.
 */
void
on_search_install(NotesLibrary *lib)
{
    on_library_add_commands(search_handlers,
                            sizeof search_handlers / sizeof *search_handlers);
    (void)lib;
}

/*
 * on_search_window_open — see search_window.h.
 */
void
on_search_window_open(NotesLibrary *lib, BOOL scope_to_selection, NSString *query)
{
    /* Each call creates an independent window. */
    (void)[[NotesSearchWindow alloc] initWithLibrary:lib
                                    scopeToSelection:scope_to_selection
                                               query:query];
}
