#import "library_planner.h"
#import "editor_window.h"
#import "library_notes.h"
#import "library_sidebar.h"

#include "db.h"
#include "serialize.h"

/*
 * NotesCalendarSettings — the calendar settings Notes shares with Tasks,
 * read and written in the shared database's shared_settings through the
 * library's open connection.
 */
@interface NotesCalendarSettings : NSObject <SharedCalendarSettings>
@property (nonatomic, weak) NotesLibrary *lib;
@end

@implementation NotesCalendarSettings

/*
 * calendar_setting: — see shared_calendar.h.
 */
- (NSString *)
calendar_setting:(const char *)key
{
    OnDatabase *db    = self.lib.app->db;
    char       *value = NULL;
    if (db == NULL || shared_settings_get(db->shared, key, &value, NULL) != 1) return nil;
    NSString *text = @(value);
    free(value);
    return text;
}

/*
 * set_calendar_setting:value: — see shared_calendar.h.  A write that
 * fails is reported in the status bar.
 */
- (void)
set_calendar_setting:(const char *)key value:(NSString *)value
{
    OnDatabase *db  = self.lib.app->db;
    char       *err = NULL;
    if (db == NULL) return;
    if (shared_settings_set(db->shared, key, value.UTF8String, &err) != 0)
        on_app_status(self.lib.app, "The calendar setting was not saved: %s", err != NULL ? err : "?");
    free(err);
}

@end

/*
 * on_calendar_settings — see library_planner.h.
 */
id<SharedCalendarSettings>
on_calendar_settings(NotesLibrary *lib)
{
    NotesCalendarSettings *settings = [[NotesCalendarSettings alloc] init];
    settings.lib = lib;
    return settings;
}

@interface NotesPlannerHost ()
@property (nonatomic, weak)   NotesLibrary               *lib;
@property (nonatomic, strong) id<SharedCalendarSettings> settings;
@property (nonatomic, strong) id                         observer;   /* the calendar's changes */
@end

@implementation NotesPlannerHost

/*
 * init_with_library: — build the panel.
 * Inputs/output are documented in library_planner.h.
 */
- (instancetype)
init_with_library:(NotesLibrary *)lib
{
    self = [super init];
    if (!self) return nil;
    _lib             = lib;
    _settings        = on_calendar_settings(lib);
    _view            = [[SharedDayPlanner alloc] init_with_delegate:self];
    _view.event_hint = @"Double-click to open its note";
    __weak SharedDayPlanner *planner = _view;
    _observer = shared_calendar_observe(^{ if (!planner.hidden) [planner reload]; });
    return self;
}

/*
 * shut_down — see library_planner.h.
 */
- (void)
shut_down
{
    shared_calendar_unobserve(self.observer);
    self.observer = nil;
    [self.view shut_down];
}

/*
 * day_planner:items_from:to: — the day's scheduled tasks
 * (on_db_calendar_day) and its calendar events (shared_calendar_day_events:
 * nothing while the calendar is off) but for Tasks' session events, drawn
 * as their tasks.  A day that cannot be read shows no tasks.
 *
 * Inputs:
 *   planner — the panel (unused)
 *   lo, hi  — the day's local midnight and the next
 *
 * Output: the items.
 */
- (NSArray<SharedDayItem *> *)
day_planner:(SharedDayPlanner *)planner items_from:(int64_t)lo to:(int64_t)hi
{
    NSMutableArray<SharedDayItem *> *items = [NSMutableArray array];
    OnDatabase *db = self.lib.app->db;
    if (db == NULL) return items;

    on_cal_day_t day;
    on_db_calendar_day(db, lo, hi, &day);
    for (size_t i = 0; i < day.n_tasks; i++) {
        const on_cal_task_t *t    = &day.tasks[i];
        SharedDayItem       *item = [[SharedDayItem alloc] init];
        item.task_id    = t->task_id;
        item.title      = *t->title != '\0' ? @(t->title) : @"Untitled";
        item.start_unix = t->start;
        item.duration   = t->duration;
        item.done       = t->done;
        [items addObject:item];
    }
    NSMutableSet<NSString *> *sessions = [NSMutableSet set];
    for (size_t i = 0; i < day.n_session_events; i++) [sessions addObject:@(day.session_events[i])];
    on_db_calendar_day_free(&day);
    [items addObjectsFromArray:shared_calendar_day_events(self.settings, lo, hi, sessions)];
    return items;
}

/*
 * day_planner:open_event: — a double-click on an event: open its note,
 * making it first when there is none (on_note_for_event).  A new note is
 * untagged, so the library counts it in All Notes and lists it there, and
 * opens with the caret on its empty last line, a blank line below the
 * attendees.
 *
 * Inputs:
 *   planner — the panel (unused)
 *   event   — the event
 */
- (void)
day_planner:(SharedDayPlanner *)planner open_event:(SharedDayItem *)event
{
    NotesLibrary *lib = self.lib;
    if (lib.app->db == NULL) return;
    size_t       n     = event.attendees.count;
    const char **names = n > 0 ? malloc(n * sizeof *names) : NULL;
    for (size_t i = 0; i < n && names != NULL; i++) names[i] = event.attendees[i].UTF8String;
    bool    created = false;
    int64_t note_id = n == 0 || names != NULL
        ? on_note_for_event(lib.app->db, event.event_id.UTF8String, event.occurrence,
                            event.title.UTF8String, names, n, &created)
        : 0;
    free(names);
    if (note_id == 0) {
        on_app_status(lib.app, "Could not make a note for \xe2\x80\x9c%s\xe2\x80\x9d",
                      event.title.UTF8String);
        return;
    }
    if (created) {
        on_library_sidebar_note_added(lib);
        on_library_notes_note_added(lib, note_id);
    }
    on_editor_window_open(lib.app, note_id, created ? NotesEditorRevealEnd : NotesEditorRevealNone,
                          0, nil);
}

@end
