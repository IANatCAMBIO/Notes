/*
 * search_window.m — the search window (work package H).
 *
 * Specification: porting/gtk_reference/src/search_window.c.
 *
 *   [ query                              ] [Search] (spinner)
 *   (•) All Notes  ( ) Selected Folder/Tag
 *   [ ] Case sensitive  [ ] Regular expression
 *   ┌ Path ─────────────────────────────── Modified ┐
 *   └───────────────────────────────────────────────┘
 *   3 matches in “Work”
 *
 * A search runs when Return is pressed in the query or Search is clicked,
 * never per keystroke.  The query is parsed on the main thread
 * (search_query.h: a bad pattern is reported at once); the matching runs on
 * a worker (search_job.h) and a newer search, an empty query or closing the
 * window abandons the one in flight.  "Selected Folder/Tag" is read from
 * the library's selection each time a search starts.  A result opens on a
 * double-click or Return, with the query's first positive term highlighted
 * in the note.  Several search windows may be open; each opens at the size
 * the last one was left at.
 */

#import "search_window.h"

#import "activation.h"
#import "editor_window.h"
#import "look.h"
#import "ui_util.h"

#include "app_core.h"
#include "commands.h"
#include "config.h"
#include "db.h"
#include "search_job.h"
#include "search_query.h"

/* One result: the note and its two columns. */
@interface NotesSearchHit : NSObject
@property (nonatomic, assign) gint64    note_id;
@property (nonatomic, copy)   NSString *path;
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
@property (nonatomic, strong) NotesActivatingTable *results;
@property (nonatomic, strong) NSTextField          *status;
@property (nonatomic, strong) NSArray<NotesSearchHit *> *hits;
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
        h.path    = @(hits[i]->path ?: "");
        h.when    = @(hits[i]->when ?: "");
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
 * title bar, at the last search window's size; the query field has the
 * focus.
 *
 * Inputs:
 *   lib    — the library
 *   scoped — YES preselects Selected Folder/Tag
 *
 * Output: the controller (the window not yet shown).
 */
- (instancetype)
init_with_lib:(NotesLibrary *)lib scoped:(BOOL)scoped
{
    gint w = (gint)on_look_search_w, h = (gint)on_look_search_h;
    on_app_config_get_size("search_win_w", "search_win_h", &w, &h);
    /* A tool window: the small utility title bar, as Settings and the
     * tool panels have it.  It does not float, so an editor opened from a
     * result comes in front of it; it stays up when the app is inactive. */
    NSPanel *win = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
                                                backing:NSBackingStoreBuffered defer:NO];
    win.title              = @"Search Notes";
    win.floatingPanel      = NO;
    win.hidesOnDeactivate  = NO;
    win.releasedWhenClosed = NO;
    win.contentMinSize     = NSMakeSize(400, 260);
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
    self.scope_selection = [NSButton radioButtonWithTitle:@"Selected Folder/Tag" target:self
                                                   action:@selector(scope_clicked:)];
    self.scope_selection.toolTip = @"Search only whatever folder or tag is selected in the library when you press "
                                    "Search";
    (scoped ? self.scope_selection : self.scope_all).state = NSControlStateValueOn;

    self.case_sensitive = [NSButton checkboxWithTitle:@"Case sensitive" target:nil action:NULL];
    self.regex          = [NSButton checkboxWithTitle:@"Regular expression" target:nil action:NULL];
    self.regex.toolTip  = @"Match the whole query as one pattern; quoting and -exclusions keep their "
                           "regular-expression meaning instead";

    self.results = [[NotesActivatingTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *path = [[NSTableColumn alloc] initWithIdentifier:@"path"];
    path.title        = @"Path";
    path.width        = 380;
    path.resizingMask = NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask;
    NSTableColumn *when = [[NSTableColumn alloc] initWithIdentifier:@"when"];
    when.title        = @"Modified";
    when.width        = 130;
    when.resizingMask = NSTableColumnUserResizingMask;
    [self.results addTableColumn:path];
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
    [NSLayoutConstraint activateConstraints:@[
        [column.topAnchor        constraintEqualToAnchor:content.topAnchor],
        [column.bottomAnchor     constraintEqualToAnchor:content.bottomAnchor],
        [column.leadingAnchor    constraintEqualToAnchor:content.leadingAnchor],
        [column.trailingAnchor   constraintEqualToAnchor:content.trailingAnchor],
        [query_row.widthAnchor   constraintEqualToAnchor:column.widthAnchor constant:-20],
        [scroll.widthAnchor      constraintEqualToAnchor:column.widthAnchor constant:-20],
        [self.status.widthAnchor constraintEqualToAnchor:column.widthAnchor constant:-20],
    ]];
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    on_ui_use_ui_font(content);
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
    self.hits = @[];
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
        self.status.stringValue = [NSString stringWithFormat:@"Bad pattern: %s", err ? err->message : "?"];
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

    BOOL          scoped   = self.scope_selection.state == NSControlStateValueOn;
    OnSearchScope scope    = ON_SCOPE_FOLDER;
    gint64        scope_id = 0;
    self.scope_desc = scoped ? [NSString stringWithFormat:@" in “%@”",
                                on_library_search_scope(self.lib, &scope, &scope_id)] : nil;
    self.status.stringValue = @"Searching…";
    self.spinner.hidden = NO;
    [self.spinner startAnimation:nil];
    self.job = on_search_job_start(self.lib.app->db->path, parsed, scoped, scope, scope_id, search_done,
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
    self.scope_all.state       = sender == self.scope_all ? NSControlStateValueOn : NSControlStateValueOff;
    self.scope_selection.state = sender == self.scope_selection ? NSControlStateValueOn : NSControlStateValueOff;
}

/*
 * open_results: — a double-click or Return on results: open each note
 * with the query's term highlighted.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
open_results:(id)sender
{
    [on_activated_rows(self.results) enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
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
 * tableView:viewForTableColumn:row: — a result's path or time.
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
    cell.textField.stringValue = [column.identifier isEqualToString:@"path"] ? h.path : h.when;
    return cell;
}

/* ── Window ────────────────────────────────────────────────────────────── */

/*
 * windowWillClose: — abandon the search, and remember the size for the
 * next search window.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self cancel_job];
    NSSize size = self.window.contentView.frame.size;
    on_app_config_set("search_win_w", @((int)size.width).stringValue.UTF8String);
    on_app_config_set("search_win_h", @((int)size.height).stringValue.UTF8String);
}

@end

/* ── Commands ───────────────────────────────────────────────────────────── */

/*
 * selection_is_narrow — is the library's selection a folder or a tag, so
 * the window should start scoped to it (GTK on_open_search)?
 *
 * Inputs:
 *   lib — the library
 *
 * Output: YES for a folder, tag or trashed folder.
 */
static BOOL
selection_is_narrow(NotesLibrary *lib)
{
    return lib.sel_kind == on_sb_folder || lib.sel_kind == on_sb_tag || lib.sel_kind == on_sb_trash_folder;
}

/*
 * run_search — "search" and "find": a search window, scoped to the
 * selection when that is a folder or a tag.
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
 * run_search_here — "search-here": a search window scoped to the row just
 * selected.
 *
 * Inputs:
 *   target — the NotesLibrary
 *   sender — the item (unused)
 */
static void
run_search_here(id target, id sender)
{
    on_search_window_open(target, YES, nil);
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
