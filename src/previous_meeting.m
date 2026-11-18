/*
 * previous_meeting.m — the earlier meeting of a series
 * (see previous_meeting.h for the contract and why the series is found by
 * title)
 */

#import "previous_meeting.h"

#include "db.h"


/*
 * on_previous_meeting_find — see previous_meeting.h.
 */
gint64
on_previous_meeting_find(OnDatabase *db, gint64 note_id, NSString *title)
{
    if (title.length == 0 || [title isEqualToString:@(SHARED_DB_NOTE_DEFAULT_TITLE)] ||
        !on_db_note_is_event_note(db, note_id))
        return 0;

    gint64      mine = 0;            /* this note's own place in the series */
    OnNoteMeta *meta = on_db_note_get(db, note_id);
    if (meta != NULL) {
        mine = meta->created_at;
        on_db_note_meta_free(meta);
    }
    return on_db_note_previous_titled(db, note_id, title.UTF8String, mine);
}
