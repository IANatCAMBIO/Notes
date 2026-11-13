#import <Cocoa/Cocoa.h>

#include "db.h"

/*
 * previous_meeting — "what happened at the last one?", one click from the
 * note you are taking now.
 *
 * A weekly meeting leaves one note per occurrence, every one of them
 * titled after the event (the day planner names a meeting's note after it:
 * serialize.h on_note_for_event), so a series is a run of notes with the
 * SAME TITLE and different dates.  The editor's button ("previous-meeting",
 * commands.m) opens the most recent note with this note's title from BEFORE
 * it, so the earlier meetings can be read and scrolled through.
 *
 * The button is there only as a promise: the note was made from the
 * calendar (cal_note, db.h on_db_event_note) AND an earlier note of the
 * series exists.  The series itself is found by title and creation date,
 * not by the calendar binding, so a note typed by hand with the same title
 * counts as an occurrence, and a note from an event that was rescheduled or
 * recreated still finds its predecessor.
 *
 * Nothing here holds state.
 */

/*
 * on_previous_meeting_find — the earlier meeting of this note's series, if
 * the note is one the button applies to.
 *
 * "Before" is (created_at, id), so two notes made in the same second are
 * still ordered, a note never finds itself, and opening an older note of a
 * series looks further back rather than forward.  Trashed notes are not in
 * the listing and so are never found.  A note with no title of its own is
 * never matched: every untitled note carries the same fallback title.
 *
 * Inputs:
 *   db      — the database
 *   note_id — the note being written now (it has a row: an editor is open
 *             on it)
 *   title   — its title as it stands, which may not be the title its row
 *             still holds
 *
 * Output: the earlier note's id, or 0 when the note was not made from the
 *         calendar or the series has no earlier note.
 */
gint64 on_previous_meeting_find(OnDatabase *db, gint64 note_id, NSString *title);
