#import <Cocoa/Cocoa.h>

#import "library_state.h"

#include "export.h"

/*
 * library_commands — the library's commands that have no view of their own
 * (work package E): new note and folder, the folder tool panel, trash /
 * delete / restore / empty, sort, export, Open Database File.
 *
 * The specification is porting/gtk_reference/src/library_window.c (its
 * "commands" sections).  This header is FROZEN; functions may be added.
 */

/*
 * on_library_commands_install — register these commands with the library
 * and set OnApp.quicknote.  Called once at startup by the app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_library_commands_install(NotesLibrary *lib);

/*
 * on_library_trash_notes — move notes to the Trash: their editors are
 * closed (each saving first), the notes are flagged in ONE transaction and
 * a status line is posted.  No confirmation: the move is reversible.
 * Refresh-free — the caller refreshes when it suits it (a drop finishes
 * its drag first).  Used by the Move to Trash command and by drops on the
 * Trash row.
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
 * on_library_trash_folder — move a folder, with its whole subtree, to the
 * Trash: the editors of its notes are closed, the folder is flagged, a
 * status line is posted, and the selection is pulled off the folder if it
 * was on it.  Refresh-free, like on_library_trash_notes.
 *
 * Inputs:
 *   lib       — the library
 *   folder_id — the folder
 *   name      — its bare name, for the status line
 *
 * Output: YES when the database move succeeded; NO also when an editor
 *         could not save (a tool panel says so, nothing moved).
 */
BOOL on_library_trash_folder(NotesLibrary *lib, gint64 folder_id, NSString *name);

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
