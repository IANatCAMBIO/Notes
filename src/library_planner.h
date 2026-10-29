#import <Cocoa/Cocoa.h>

#import "library_state.h"
#import "shared_calendar.h"

/*
 * library_planner — Notes' side of the day planner (shared_day_planner.h,
 * the panel Tasks has too): the right-side pane of the library window
 * showing the hours of one day, with the tasks scheduled on it and the
 * events of the macOS Calendar (shared_calendar.h: the accounts set up in
 * the Calendar app, which keeps them in sync) while the calendar is
 * switched on in Settings — a setting Notes shares with Tasks.  It
 * redraws when the calendar changes.  Read-only: sessions are planned in
 * Tasks.
 *
 * A double-click on an event opens its note — made the first time
 * (on_note_for_event: the event's title, its attendees as a bulleted list)
 * and opened again on every later double-click while it is out of the
 * Trash.  Each occurrence of a recurring event has its own.
 *
 * The library window owns the host and puts its view in the split view.
 */
@interface NotesPlannerHost : NSObject <SharedDayPlannerDelegate>

/*
 * init_with_library: — build the panel and its delegate.
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the host.
 */
- (instancetype)init_with_library:(NotesLibrary *)lib NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/* The panel. */
@property (nonatomic, readonly) SharedDayPlanner *view;

/*
 * shut_down — stop watching the calendar and the clock.  Call when the
 * library window closes.
 */
- (void)shut_down;

@end

/*
 * on_calendar_settings — the calendar settings Notes shares with Tasks
 * (shared_calendar.h), through the library's open database.
 *
 * Inputs:
 *   lib — the library (held weakly)
 *
 * Output: the settings.
 */
id<SharedCalendarSettings> on_calendar_settings(NotesLibrary *lib);
