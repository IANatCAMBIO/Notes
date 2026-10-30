#import <Cocoa/Cocoa.h>

#include "app_core.h"

/*
 * meeting_summary — "what happened at the last one?", from the note you
 * are taking now.
 *
 * A weekly meeting leaves one note per occurrence, every one of them
 * titled after the event (the day planner names a meeting's note after it:
 * serialize.h on_note_for_event), so a series is a run of notes with the
 * SAME TITLE and different dates.  The clock button in an editor's format
 * bar ("meeting-summary", commands.m) takes the most recent note with this
 * note's title from BEFORE it, sends that note to the configured command
 * (ai_run.h) and shows what comes back in a tool panel titled after the
 * meeting and its date.
 *
 * The series is found by title and creation date, not by the calendar
 * binding (db.h on_db_event_note), so a note typed by hand with the same
 * title counts as an occurrence too, and a note from an event that was
 * rescheduled or recreated still finds its predecessor.
 *
 * Nothing here holds state between summaries except the panels on screen,
 * which the module owns and drops as each one closes.
 */

/*
 * on_meeting_summary_enabled — is the clock button part of the editor's
 * format bar at all?  The AI master switch (ai_enabled) and the
 * meeting_summary preference must both be on; with AI off, no AI command
 * runs anywhere.  Read while the format bar is built, so a change to
 * either setting needs on_editor_settings_changed.
 *
 * Inputs:
 *   app — the application context
 *
 * Output: YES when the button is shown.
 */
BOOL on_meeting_summary_enabled(OnApp *app);

/*
 * on_meeting_summary_can_run — may the button run now?  Only while it is
 * enabled and the note has a title of its own: every untitled note carries
 * the same fallback title, and matching on that would summarise an
 * unrelated note.
 *
 * Inputs:
 *   app   — the application context
 *   title — the note's title as it stands (an editor's window title)
 *
 * Output: YES when the command may run.
 */
BOOL on_meeting_summary_can_run(OnApp *app, NSString *title);

/*
 * on_meeting_summary_show — summarise the last meeting of this note's
 * series.  Opens the panel at once (so there is somewhere for the news
 * that the command failed), runs the command off the main thread and fills
 * the panel when it exits.  Closing the panel terminates a command still
 * running.  A note whose summary is already on screen raises that panel
 * instead of running a second command, and one with no earlier namesake
 * says so in a notice and runs nothing.
 *
 * Inputs:
 *   app     — the application context
 *   note_id — the note being written now; it is never its own predecessor
 *   title   — its title as it stands (an editor's window title)
 *   over    — the window to centre the panel over; nil = the screen
 */
void on_meeting_summary_show(OnApp *app, gint64 note_id, NSString *title, NSWindow *over);
