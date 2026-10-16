/*
 * search_window.m — the search window.
 *
 * Specification: the GTK build's src/search_window.c.
 *
 *   [ query                              ] [Search] (spinner)
 *   (•) All Notes  ( ) Selected Tag
 *   [ ] Case sensitive  [ ] Regular expression
 *   ┌ Title ──────────────── Tags ───────── Modified ┐
 *   └────────────────────────────────────────────────┘
 *   3 matches in “#Work”
 *
 * A search runs when Return is pressed in the query or Search is clicked,
 * never per keystroke.  The query is parsed on the main thread
 * (search_query.h: a bad pattern is reported at once); the matching runs on
 * a worker (search_job.h) and a newer search, an empty query or closing the
 * window abandons the one in flight.  "Selected Tag" is read from
 * the library's selection each time a search starts.  A result opens on a
 * double-click or Return, with the query's first positive term highlighted
 * in the note.  Several search windows may be open; each opens at the size
 * the last one was left at.
 */

#import "search_window.h"

#import "activation.h"
#import "editor_window.h"
#import "library_sidebar.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "commands.h"
#include "config.h"
#include "db.h"
#include "search_job.h"
#include "search_query.h"

/* One result: the note and its three columns. */
@interface NotesSearchHit : NSObject
@property (nonatomic, assign) gint64    note_id;
@property (nonatomic, copy)   NSString *title;
@property (nonatomic, copy)   NSString *tags;
@property (nonatomic, copy)   NSString *when;
@end

@implementation NotesSearchHit
@end

@interface NotesSearchWindow : NSWindowController <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, weak)   NotesLibrary         *lib;
@property (nonatomic, strong) NSTextField          *query;
@property (nonatomic, strong) NSButton             *scope_all;
@property (nonatomic, strong) NSButton             *scope_selection;
@property (nonatomic, strong) NSButton             *case_sensitive;
@property (nonatomic, strong) NSButton             *regex;
@property (nonatomic, strong) NSProgressIndicator  *spinner;
@property (nonatomic, strong) SharedTable *results;
@property (nonatomic, strong) NSTextField          *status;
@property (nonatomic, strong) NSArray<NotesSearchHit *> *hits;
@property (nonatomic, copy)   NSString             *hits_db;      /* the database they are ids in */
@property (nonatomic, copy)   NSString             *highlight;    /* the parsed query's term */
@property (nonatomic, copy)   NSString             *scope_desc;   /* " in “name”", or nil */
@property (nonatomic, assign) on_search_job_t      *job;          /* in flight, or NULL */
- (void)run_search;
@end

/*
 * search_done — a job finished (main thread): show its hits and the count.
 *
 * Inputs:
 *   error — why the search failed, or NULL
 *   hits  — the matches (borrowed)
 *   n     — how many
 *   data  — the window controller (unretained: a closing window cancels its
 *           job, so this never runs for a gone window)
 */
static void
search_done(const gchar *error, const on_search_hit_t *const *hits, size_t n, gpointer data)
{
    NotesSearchWindow *s = (__bridge NotesSearchWindow *)data;
    s.job = NULL;
    [s.spinner stopAnimation:nil];
    s.spinner.hidden = YES;
    if (error) {
        s.status.stringValue = @(error);
        return;
    }
    NSMutableArray<NotesSearchHit *> *rows = [NSMutableArray arrayWithCapacity:n];
    for (size_t i = 0; i < n; i++) {
        NotesSearchHit *h = [[NotesSearchHit alloc] init];
        h.note_id = hits[i]->id;
        h.title   = @(hits[i]->title ?: "");
        h.tags    = @(hits[i]->tags ?: "");
        h.when    = shared_ui_format_date(hits[i]->updated_at, shared_ui_date_list);
        [rows addObject:h];
    }
    s.hits = rows;
    [s.results reloadData];
    s.status.stringValue = [NSString stringWithFormat:@"%zu match%@%@", n, n == 1 ? @"" : @"es",
                            s.scope_desc ?: @""];
}

@implementation NotesSearchWindow

/*
 * init_with_lib:scoped: — build the window, a tool window with the small
 * title bar, at its minimum size; the query field has the focus.
 *
 * Inputs:
 *   lib    — the library
 *   scoped — YES preselects Selected Tag
 *
 * Output: the controller (the window not yet shown).
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib scoped:(BOOL)scoped
{
    /* A tool window: the small utility title bar, as Settings and the
     * tool panels have it.  It does not float, so an editor opened from a
     * result comes in front of it; it stays up when the app is inactive. */
    NSSize   min = NSMakeSize(on_look_search_win_w, on_look_search_win_h);
    NSPanel *win = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, min.width, min.height)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
                                                backing:NSBackingStoreBuffered defer:NO];
    win.title              = @"Search Notes";
    win.floatingPanel      = NO;
    win.hidesOnDeactivate  = NO;
    win.releasedWhenClosed = NO;
    win.contentMinSize     = min;      /* and it opens at that size, every time */
    if (!(self = [super initWithWindow:win])) return nil;
    _lib  = lib;
    _hits = @[];
    win.delegate = self;

    self.query = [NSTextField textFieldWithString:@""];
    self.query.identifier = @"search-query";
    self.query.placeholderString = @"Search titles and note text…";
    self.query.toolTip = @"Every word must appear somewhere in the note.\n\"in quotes\" matches the whole phrase; "
                          "-word excludes notes that contain it.\nRegular expression mode takes the query as one "
                          "pattern instead.";
    self.query.target = self;
    self.query.action = @selector(search_clicked:);
    self.query.cell.sendsActionOnEndEditing = NO;   /* Return searches; leaving the field does not */
    [self.query setContentHuggingPriority:NSLayoutPriorityDefaultLow
                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSButton *search = [NSButton buttonWithTitle:@"Search" target:self action:@selector(search_clicked:)];
    self.spinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    self.spinner.style       = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.hidden      = YES;

    self.scope_all       = [NSButton radioButtonWithTitle:@"All Notes" target:self action:@selector(scope_clicked:)];
    self.scope_selection = [NSButton radioButtonWithTitle:@"Selected Tag" target:self
                                                   action:@selector(scope_clicked:)];
    self.scope_selection.toolTip = @"Search only the notes of the tag selected in the library when you press "
                                    "Search";
    (scoped ? self.scope_selection : self.scope_all).state = NSControlStateValueOn;

    self.case_sensitive = [NSButton checkboxWithTitle:@"Case sensitive" target:nil action:NULL];
    self.regex          = [NSButton checkboxWithTitle:@"Regular expression" target:nil action:NULL];
    self.regex.toolTip  = @"Match the whole query as one pattern; quoting and -exclusions keep their "
                           "regular-expression meaning instead";

    self.results = [[SharedTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *title = [[NSTableColumn alloc] initWithIdentifier:@"title"];
    title.title        = @"Title";
    title.width        = 250;
    title.resizingMask = NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask;
    NSTableColumn *tags = [[NSTableColumn alloc] initWithIdentifier:@"tags"];
    tags.title        = @"Tags";
    tags.width        = 130;
    tags.resizingMask = NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask;
    NSTableColumn *when = [[NSTableColumn alloc] initWithIdentifier:@"when"];
    when.title        = @"Modified";
    when.width        = 130;
    when.resizingMask = NSTableColumnUserResizingMask;
    [self.results addTableColumn:title];
    [self.results addTableColumn:tags];
    [self.results addTableColumn:when];
    self.results.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    self.results.style                   = NSTableViewStylePlain;
    self.results.rowHeight               = on_look_row_h;   /* the library list's compact rows */
    self.results.usesAlternatingRowBackgroundColors = YES;
    self.results.dataSource   = self;
    self.results.delegate     = self;
    self.results.target       = self;
    self.results.doubleAction = @selector(open_results:);
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = self.results;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.borderType          = NSBezelBorder;

    self.status = [NSTextField labelWithString:@""];
    self.status.identifier = @"search-status";
    self.status.lineBreakMode = NSLineBreakByTruncatingTail;

    NSStackView *query_row = [NSStackView stackViewWithViews:@[self.query, search, self.spinner]];
    NSStackView *scope_row = [NSStackView stackViewWithViews:@[self.scope_all, self.scope_selection]];
    NSStackView *opt_row   = [NSStackView stackViewWithViews:@[self.case_sensitive, self.regex]];
    scope_row.spacing = opt_row.spacing = 12;
    NSStackView *column = [NSStackView stackViewWithViews:@[query_row, scope_row, opt_row, scroll, self.status]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment   = NSLayoutAttributeLeading;
    column.spacing     = 6;
    column.edgeInsets  = NSEdgeInsetsMake(10, 10, 10, 10);
    column.translatesAutoresizingMaskIntoConstraints = NO;
    [column setHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    NSView *content = win.contentView;
    [content addSubview:column];
    shared_ui_pin_edges(column, content);
    [NSLayoutConstraint activateConstraints:@[
        [query_row.widthAnchor   constraintEqualToAnchor:column.widthAnchor constant:-20],
        [scroll.widthAnchor      constraintEqualToAnchor:column.widthAnchor constant:-20],
        [self.status.widthAnchor constraintEqualToAnchor:column.widthAnchor constant:-20],
    ]];
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    shared_ui_use_ui_font(content);
    [win center];
    win.initialFirstResponder = self.query;
    return self;
}

/* ── Searching ─────────────────────────────────────────────────────────── */

/*
 * cancel_job — abandon the search in flight, if any.
 */
- (void)
cancel_job
{
    if (self.job) on_search_job_cancel(self.job);
    self.job = NULL;
    [self.spinner stopAnimation:nil];
    self.spinner.hidden = YES;
}

/*
 * run_search — start a search for the query (GTK run_search): the old one
 * is abandoned and the results cleared; an empty query or a bad pattern
 * is reported in the status line instead.
 */
- (void)
run_search
{
    [self cancel_job];
    self.hits    = @[];
    self.hits_db = @(self.lib.app->db->path);
    [self.results reloadData];
    NSString *text = self.query.stringValue;
    if (text.length == 0) {
        self.status.stringValue = @"Type something to search for.";
        return;
    }
    GError  *err    = NULL;
    OnQuery *parsed = on_query_new(text.UTF8String, self.case_sensitive.state == NSControlStateValueOn,
                                   self.regex.state == NSControlStateValueOn, &err);
    if (parsed == NULL) {
        self.status.stringValue = [NSString stringWithFormat:@"Bad pattern: %@", @(err ? err->message : "?")];
        g_clear_error(&err);
        return;
    }
    if (on_query_is_empty(parsed)) {
        self.status.stringValue = @"Type something to search for.";
        on_query_free(parsed);
        return;
    }
    const gchar *term = on_query_highlight_term(parsed);
    self.highlight = term ? @(term) : nil;

    BOOL     scoped = self.scope_selection.state == NSControlStateValueOn;
    gint64   tag_id = 0;
    NSString *where = on_library_search_scope(self.lib, &tag_id);
    if (!scoped) tag_id = 0;
    self.scope_desc = tag_id != 0 ? [NSString stringWithFormat:@" in “%@”", where] : nil;
    self.status.stringValue = @"Searching…";
    self.spinner.hidden = NO;
    [self.spinner startAnimation:nil];
    self.job = on_search_job_start(self.lib.app->db->path, parsed, tag_id, search_done,
                                   (__bridge gpointer)self);
}

/*
 * search_clicked: — Return in the query, or the Search button.
 *
 * Inputs:
 *   sender — the field or button (unused)
 */
- (void)
search_clicked:(id)sender
{
    [self run_search];
}

/*
 * scope_clicked: — a scope radio: the two act as one group.
 *
 * Inputs:
 *   sender — the radio clicked
 */
- (void)
scope_clicked:(NSButton *)sender
{
    shared_ui_select_radio(@[self.scope_all, self.scope_selection], sender);
}

/*
 * open_results: — a double-click or Return on results: open each note
 * with the query's term highlighted.  Results from a database no longer
 * open are ids of another file: they are cleared instead.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
open_results:(id)sender
{
    if (![self.hits_db isEqualToString:@(self.lib.app->db->path)]) {
        [self cancel_job];
        self.hits = @[];
        [self.results reloadData];
        self.status.stringValue = @"Another database is open now. Search again.";
        return;
    }
    [shared_ui_activated_rows(self.results) enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        if (i < self.hits.count)
            on_editor_window_open(self.lib.app, self.hits[i].note_id, NotesEditorRevealNone, 0, self.highlight);
    }];
}

/* ── Table ─────────────────────────────────────────────────────────────── */

/*
 * numberOfRowsInTableView: — the hits.
 *
 * Inputs:
 *   table — the results (unused)
 *
 * Output: the count.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)table
{
    return (NSInteger)self.hits.count;
}

/*
 * tableView:viewForTableColumn:row: — a result's title, tags or time.
 *
 * Inputs:
 *   table  — the results
 *   column — which column
 *   row    — which hit
 *
 * Output: the cell.
 */
- (NSView *)
tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSTableCellView *cell = on_ui_label_cell(table, column.identifier);
    NotesSearchHit  *h    = self.hits[(NSUInteger)row];
    cell.textField.stringValue = [column.identifier isEqualToString:@"title"] ? h.title
                               : [column.identifier isEqualToString:@"tags"]  ? h.tags : h.when;
    return cell;
}

/* ── Window ────────────────────────────────────────────────────────────── */

/*
 * windowWillClose: — abandon the search.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self cancel_job];
}

@end

/* ── Commands ───────────────────────────────────────────────────────────── */

/*
 * selection_is_narrow — is the library's selection a tag, so the window
 * should start scoped to it (GTK on_open_search)?
 *
 * Inputs:
 *   lib — the library
 *
 * Output: YES for a tag.
 */
static BOOL
selection_is_narrow(NotesLibrary *lib)
{
    return lib.sel_kind == on_sb_tag;
}

/*
 * run_search — "search" and "find": a search window, scoped to the
 * selection when that is a tag.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_search(id target, id sender)
{
    on_search_window_open(target, selection_is_narrow(target), nil);
}

/*
 * run_search_here — "search-here": a search window scoped to the sidebar
 * row the item names, which is selected first (a search scope is the
 * library's selection), else to the selected row.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item
 */
static void
run_search_here(id target, id sender)
{
    NotesLibrary *lib = target;
    on_sb_ref_t   ref;
    if (on_library_sb_ref(sender, &ref) && (ref.kind != lib.sel_kind || ref.ident != lib.sel_id))
        on_library_sidebar_select(lib, ref.kind, ref.ident);
    on_search_window_open(lib, YES, nil);
}

/* This module's commands.  Handlers receive the NotesLibrary. */
static const on_command_handler_t search_handlers[] = {
    { @"search",      run_search,      NULL },
    { @"find",        run_search,      NULL },
    { @"search-here", run_search_here, NULL },
};

/*
 * on_search_install — see search_window.h.
 */
void
on_search_install(NotesLibrary *lib)
{
    on_library_add_commands(search_handlers, sizeof search_handlers / sizeof *search_handlers);
}

/*
 * on_search_window_open — see search_window.h.
 */
void
on_search_window_open(NotesLibrary *lib, BOOL scope_to_selection, NSString *query)
{
    NotesSearchWindow *s = [[NotesSearchWindow alloc] init_with_lib:lib scoped:scope_to_selection];
    on_ui_keep_until_closed(s);
    [s showWindow:nil];
    if (query.length) {
        s.query.stringValue = query;
        [s run_search];
    }
}
