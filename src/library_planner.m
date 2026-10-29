#import "library_planner.h"
#import "editor_window.h"
#import "library_notes.h"
#import "library_sidebar.h"

#include "db.h"
#include "serialize.h"

@interface NotesPlannerHost ()
@property (nonatomic, weak) NotesLibrary *lib;
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
    _view            = [[SharedDayPlanner alloc] init_with_delegate:self];
    _view.event_hint = @"Double-click to open its note";
    return self;
}

/*
 * day_planner:items_from:to: — the day's scheduled tasks and calendar
 * events (on_db_calendar_day).  A day that cannot be read shows empty.
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
    for (size_t i = 0; i < day.n_events; i++) {
        const on_cal_event_t *e    = &day.events[i];
        SharedDayItem        *item = [[SharedDayItem alloc] init];
        item.calendar_id = @(e->calendar_id);
        item.event_id    = @(e->event_id);
        item.title       = @(e->title);
        item.start_unix  = e->start;
        item.duration    = (int)((e->end - e->start) / 60);
        item.all_day     = e->all_day;
        item.color       = @(e->color);
        [items addObject:item];
    }
    on_db_calendar_day_free(&day);
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
    bool    created = false;
    int64_t note_id = on_note_for_event(lib.app->db, event.calendar_id.UTF8String,
                                        event.event_id.UTF8String, event.title.UTF8String,
                                        &created);
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
