/*
 * previous_meeting.m — the earlier meeting of a series
 * (see previous_meeting.h for the contract and why the series is found by
 * title)
 */

#import "previous_meeting.h"

#include "db.h"

#include <glib.h>

/*
 * on_previous_meeting_find — see previous_meeting.h.
 */
gint64
on_previous_meeting_find(OnDatabase *db, gint64 note_id, NSString *title)
{
    if (title.length == 0 || [title isEqualToString:@(SHARED_DB_NOTE_DEFAULT_TITLE)] ||
        !on_db_note_is_event_note(db, note_id))
        return 0;

    const char *want = title.UTF8String;
    gint64      mine = 0;            /* this note's own place in the series */
    OnNoteMeta *meta = on_db_note_get(db, note_id);
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
    return best;
}
