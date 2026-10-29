#import <Cocoa/Cocoa.h>

#import "library_state.h"
#import "shared_day_planner.h"

/*
 * library_planner — Notes' side of the day planner (shared_day_planner.h,
 * the panel Tasks has too): the right-side pane of the library window
 * showing the hours of one day, with the tasks scheduled on it and its
 * Google Calendar events.  The events are the cache Tasks' calendar sync
 * keeps in the shared database (Notes has no Google account of its own),
 * so the panel shows what Tasks last pulled, and nothing while Tasks is
 * not signed in.  Read-only: sessions are planned in Tasks.
 *
 * A double-click on an event opens its note — made the first time
 * (on_note_for_event: the event's title, its attendees as a bulleted list)
 * and opened again on every later double-click while it is out of the
 * Trash.
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

@end
