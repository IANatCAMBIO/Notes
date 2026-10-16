#import <Cocoa/Cocoa.h>

#import "library_state.h"

#include "export.h"

/*
 * library_commands — the library's commands that have no view of their own:
 * new note, the Edit Tag panel, tagging dropped notes, trash / delete /
 * empty, export, Open Database File.
 *
 * The specification is the GTK build's src/library_window.c (its
 * "commands" sections).
 */

/*
 * on_library_commands_install — register these commands with the library
 * (Called once at startup by the app delegate.)
 *
 * Inputs:
 *   lib — the library
 */
void on_library_commands_install(NotesLibrary *lib);

/*
 * on_library_trash_notes — move notes to the Trash: their editors are
 * closed (each saving first), the notes are flagged in ONE transaction and
 * a status line is posted.  No confirmation: the move is reversible (the
 * notes keep their tags).  Refresh-free — the caller refreshes when it
 * suits it (a drop finishes its drag first).  Used by the Move to Trash
 * command and by drops on the Trash row.
 *
 * Inputs:
 *   lib — the library
 *   ids — the notes
 *   n   — how many
 *
 * Output: YES when the database move succeeded; NO also when an editor
 *         could not save (a tool panel says so, nothing moved).
 */
BOOL on_library_trash_notes(NotesLibrary *lib, const gint64 *ids, size_t n);

/*
 * on_library_tag_notes — put a tag in the Tags field of several notes, in
 * ONE transaction (all or none), and post a status line.  Refresh-free,
 * like on_library_trash_notes.  Used by drops on a tag's row.
 *
 * Inputs:
 *   lib  — the library
 *   ids  — the notes (NSNumber note ids)
 *   name — the tag's name (the existing tag of that name, in any case)
 *
 * Output: YES when every note got it.
 */
BOOL on_library_tag_notes(NotesLibrary *lib, NSArray<NSNumber *> *ids, NSString *name);

/*
 * on_library_edit_tag — the Edit Tag panel for a tag: its name (fixed),
 * an emoji (shown by Notes and Tasks) and whether it is a favorite in
 * Notes (a button in the Favorites bar).  OK writes both and refreshes the
 * library.  The sidebar's Edit Tag… and the Favorites bar's menu open it.
 *
 * Inputs:
 *   lib    — the library (the panel sits over its window)
 *   tag_id — the tag; a tag that has gone shows nothing
 */
void on_library_edit_tag(NotesLibrary *lib, gint64 tag_id);

/*
 * on_library_delete_notes — the Trash view's delete: ask once ("This cannot
 * be undone."), then close the notes' editors and delete permanently those
 * still in the Trash (none if the database was switched meanwhile), and
 * refresh the library.
 *
 * Inputs:
 *   lib — the library
 *   ids — the notes (NSNumber note ids); nothing happens for none
 */
void on_library_delete_notes(NotesLibrary *lib, NSArray<NSNumber *> *ids);

/*
 * on_library_open_db — switch to a shared database file already chosen: a
 * tool panel asks whether for this session only or as the new default of
 * both apps; a file the shared module refuses, or a default it will not
 * set (while $CAMBIO_SHARED_DB fixes the file for the run), or a file it
 * refuses (not a shared database, on a network or synced folder) is
 * reported in its words and the previous database stays open
 * (on_app_switch_db).  The file already open needs nothing.  The
 * Open Database command calls it once the file picker returns.
 *
 * Inputs:
 *   lib  — the library (its window is where the panels show)
 *   path — the database file
 */
void on_library_open_db(NotesLibrary *lib, NSString *path);

/*
 * on_library_export — export notes into a folder already chosen: the ones
 * given, or every note; a notice says how many were written, or what
 * failed.  The export commands call it once the folder picker returns.
 *
 * Inputs:
 *   lib    — the library (its window is where the notice shows)
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN
 *   ids    — the notes (NSNumber ids), or nil for all of them
 *   dir    — the destination folder
 *
 * Output: how many notes were written, or -1 when the export failed.
 */
gint on_library_export(NotesLibrary *lib, OnExportFormat format, NSArray<NSNumber *> *ids, NSString *dir);
