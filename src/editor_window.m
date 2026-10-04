#import "editor_window.h"
#import "note_view.h"

#include "identity.h"
#include "look.h"
#include "note_edit.h"
#include "serialize.h"

/*
 * editor_window.m — working STUB (work package D completes this file).
 * See editor_window.h for what is real and what is missing; the behaviour
 * to complete it from is porting/gtk_reference/src/editor_window.c.
 */

/* Seconds of quiet after the last edit before the note is saved. */
static const double autosave_delay = 1.2;

/* Open editors: note id -> window controller.  THE registry; it owns the
 * controllers (see OWNERSHIP in editor_window.h).  Main thread only. */
static NSMutableDictionary<NSNumber *, NSWindowController *> *editors;

@interface NotesEditorWindow : NSWindowController <NSWindowDelegate>
@property (nonatomic, assign) OnApp *app;
@property (nonatomic, assign) gint64 note_id;
@property (nonatomic, strong) NotesNoteView *view;
@property (nonatomic, assign) BOOL dirty;                 /* edits not yet saved */
@property (nonatomic, assign) BOOL sync_actions;          /* an action line changed since
                                                             the last save that succeeded */
@property (nonatomic, assign) on_timer_id_t autosave_timer;  /* 0 = none pending */
- (void)save;
@end

/*
 * autosave_fire — the autosave timer's callback: save the editor it was
 * armed for.  The timer is a one-shot: the service removes it when this
 * returns, so the editor only forgets its id.
 *
 * Inputs:
 *   data — the NotesEditorWindow, unretained: the editor removes its timer
 *          before it goes away, so this never runs for a dead one
 */
static void
autosave_fire(void *data)
{
    NotesEditorWindow *editor = (__bridge NotesEditorWindow *)data;
    editor.autosave_timer = 0;
    [editor save];
}

@implementation NotesEditorWindow

/*
 * init_with_app:note:title: — a window for one note, its content loaded.
 *
 * Inputs:
 *   app     — the application context
 *   note_id — the note (it exists)
 *   title   — its title, for the window
 *
 * Output: the controller; the caller registers and shows it.
 */
- (instancetype)
init_with_app:(OnApp *)app note:(gint64)note_id title:(NSString *)title
{
    gint w = (gint)on_look_editor_w;
    gint h = (gint)on_look_editor_h;
    on_app_config_get_size("editor_win_w", "editor_win_h", &w, &h);

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    win.title              = title;
    win.releasedWhenClosed = NO;     /* ARC and the registry own it */

    self = [super initWithWindow:win];
    if (!self) return nil;
    _app     = app;
    _note_id = note_id;
    win.delegate = self;

    _view = [[NotesNoteView alloc] init_with_app:app];
    __weak NotesEditorWindow *ws = self;
    _view.edited_handler = ^{ [ws queue_autosave]; };

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:win.contentView.bounds];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask    = NSViewWidthSizable | NSViewHeightSizable;
    _view.frame            = NSMakeRect(0, 0, scroll.contentSize.width, 0);
    _view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView    = _view;
    win.contentView        = scroll;

    [self load_content];
    [self place_bottom_right];
    return self;
}

/*
 * load_content — load the note into the view, first reconciled against its
 * task rows (the truth: a task changed while the note was closed re-renders
 * its line here).  A reconciled line is an unsaved change, so the autosave
 * is queued to write it back.
 */
- (void)
load_content
{
    OnDocument *doc    = on_note_document_load(self.app->db, self.note_id);
    gboolean    healed = on_note_document_reconcile(self.app->db, self.note_id, doc);
    on_note_view_load_document(self.view, doc);
    if (healed) [self queue_autosave];
}

/*
 * place_bottom_right — a new editor opens in the bottom-right corner of the
 * screen the library is on, clear of the Dock and the edges, so it does not
 * cover the library.
 */
- (void)
place_bottom_right
{
    NSScreen *screen = NSApp.mainWindow.screen ?: NSScreen.mainScreen;
    NSRect area  = screen.visibleFrame;
    NSRect frame = self.window.frame;
    CGFloat x = NSMaxX(area) - frame.size.width - on_look_editor_screen_margin;
    CGFloat y = NSMinY(area) + on_look_editor_screen_margin;
    [self.window setFrameOrigin:NSMakePoint(MAX(x, NSMinX(area)), y)];
}

/*
 * queue_autosave — there is something to save: (re)arm the autosave timer,
 * so the note is written once typing pauses.
 */
- (void)
queue_autosave
{
    self.dirty = YES;
    on_timer_remove(self.autosave_timer);
    self.autosave_timer = on_timer_add(autosave_delay, false, autosave_fire,
                                       (__bridge void *)self, NULL);
}

/*
 * save — persist the note through THE save (on_note_document_save: blob,
 * title, body text, and the task rows its '!' lines are — synced only when
 * an action line was touched since the last save), and its tag set when a
 * #tag was touched.  Then tell the library: the light one-row update
 * normally, the full refresh when a tag or a task changed.  If the save
 * fails the note STAYS dirty — the next edit or the close tries again —
 * and the failure is posted to the status bar.
 */
- (void)
save
{
    OnApp      *app = self.app;
    OnDocument *doc = on_note_view_document(self.view);
    gboolean actions_changed = FALSE;
    if (on_note_view_take_actions_modified(self.view)) self.sync_actions = YES;
    if (!on_note_document_save(app->db, self.note_id, doc, self.sync_actions, &actions_changed)) {
        on_app_status(app, "Could not save note %" G_GINT64_FORMAT, self.note_id);
        return;
    }
    self.sync_actions = NO;

    BOOL tags_changed = on_note_view_take_tags_modified(self.view);
    if (tags_changed) {
        GList *tags = on_document_collect_tags(doc);
        on_db_note_set_tags(app->db, self.note_id, tags);
        g_list_free_full(tags, g_free);
    }
    self.dirty = NO;

    gchar *title = on_document_title(doc, ON_DEFAULT_NOTE_TITLE, ON_TITLE_MAX_CHARS);
    self.window.title = @(title);
    g_free(title);

    if (tags_changed || actions_changed) {
        if (app->notify_notes_changed != NULL) app->notify_notes_changed(app);
    } else if (app->notify_note_saved != NULL) {
        app->notify_note_saved(app, self.note_id);
    }
}

/*
 * windowWillClose: — the editor is going away: stop its timer, then either
 * delete the note (it was left with no content — a New Note window closed
 * without typing leaves nothing behind) or flush the edits the autosave had
 * not written yet, and leave the registry.  Runs synchronously inside
 * -close, which is what makes on_editor_close_all a flush.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    OnApp *app = self.app;
    on_timer_remove(self.autosave_timer);
    self.autosave_timer      = 0;
    self.view.edited_handler = nil;

    if (on_note_edit_is_blank(on_note_view_document(self.view))) {
        gint64 note_id = self.note_id;
        on_db_notes_delete(app->db, &note_id, 1);
        on_app_status(app, "Deleted empty note");
        if (app->notify_notes_changed != NULL) app->notify_notes_changed(app);
    } else if (self.dirty) {
        [self save];
    }
    [editors removeObjectForKey:@(self.note_id)];   /* may release self: last */
}

@end

/*
 * editor_for — the open editor of a note.
 *
 * Inputs:
 *   note_id — the note
 *
 * Output: its window controller, or nil when the note is not open.
 */
static NotesEditorWindow *
editor_for(gint64 note_id)
{
    return (NotesEditorWindow *)editors[@(note_id)];
}

/*
 * on_editor_window_open — see editor_window.h.  (Stub: the reveal and the
 * search term reach the view once package C implements them.)
 */
BOOL
on_editor_window_open(OnApp *app, gint64 note_id, NotesEditorReveal reveal, NSInteger ordinal,
                      NSString *search_term)
{
    NotesEditorWindow *editor = editor_for(note_id);
    if (!editor) {
        OnNoteMeta *meta = on_db_note_get(app->db, note_id);
        if (meta == NULL) return NO;
        editor = [[NotesEditorWindow alloc] init_with_app:app note:note_id title:@(meta->title)];
        on_db_note_meta_free(meta);
        editors[@(note_id)] = editor;
    }
    if (reveal == NotesEditorRevealImage) on_note_view_image_reveal(editor.view, ordinal);
    if (reveal == NotesEditorRevealAction) on_note_view_action_reveal(editor.view, ordinal);
    if (search_term.length > 0) on_note_view_find(editor.view, search_term);
    [editor showWindow:nil];
    [NSApp activate];
    return YES;
}

/*
 * on_editor_close_notes — see editor_window.h.
 */
void
on_editor_close_notes(const gint64 *ids, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        [editor_for(ids[i]).window close];
    }
}

/*
 * on_editor_close_all — see editor_window.h.
 */
void
on_editor_close_all(void)
{
    /* Closing removes the editor from the registry: walk a copy. */
    for (NSWindowController *editor in editors.allValues) {
        [editor.window close];
    }
}

/*
 * on_editor_settings_changed — see editor_window.h.
 */
void
on_editor_settings_changed(void)
{
    for (NSWindowController *editor in editors.allValues) {
        on_note_view_settings_changed(((NotesEditorWindow *)editor).view);
    }
}

/*
 * hook_open_note — OnApp.open_note: `notes note open` from the command line.
 *
 * Inputs:
 *   app     — the application context
 *   note_id — the note
 *
 * Output: FALSE when the note does not exist.
 */
static gboolean
hook_open_note(OnApp *app, gint64 note_id)
{
    return on_editor_window_open(app, note_id, NotesEditorRevealNone, 0, nil);
}

/*
 * hook_live_document — OnApp.live_document: the document of a note's open
 * editor, readied for an edit from outside.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   note_id — the note
 *
 * Output: the document, or NULL when the note is not open.
 */
static OnDocument *
hook_live_document(OnApp *app, gint64 note_id)
{
    NotesEditorWindow *editor = editor_for(note_id);
    return editor ? on_note_view_begin_external_edit(editor.view) : NULL;
}

/*
 * hook_live_edited — OnApp.live_edited: the document hook_live_document
 * returned was edited; the view redraws and reports the edit, which queues
 * the autosave.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   note_id — the note (open: live_document just returned its document)
 */
static void
hook_live_edited(OnApp *app, gint64 note_id)
{
    on_note_view_end_external_edit(editor_for(note_id).view);
}

/*
 * hook_flush_editors — OnApp.flush_editors.
 *
 * Inputs:
 *   app — the application context (unused)
 */
static void
hook_flush_editors(OnApp *app)
{
    on_editor_close_all();
}

/*
 * on_editor_install — see editor_window.h.
 */
void
on_editor_install(OnApp *app)
{
    editors            = [NSMutableDictionary dictionary];
    app->open_note     = hook_open_note;
    app->live_document = hook_live_document;
    app->live_edited   = hook_live_edited;
    app->flush_editors = hook_flush_editors;
}
