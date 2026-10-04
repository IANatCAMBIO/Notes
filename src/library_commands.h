#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * library_commands — the library's commands that have no view of their own
 * (work package E): new note and folder, the folder sheet, trash / delete /
 * restore / empty, pin, sort, export, Open Database File.
 *
 * STATUS: partly real.  New Note, Quick Note and the two trash functions
 * are implemented; the rest of the commands are still to be written from
 * porting/gtk_reference/src/library_window.c ("commands" sections) and are
 * disabled until they are.  This header is FROZEN.
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
 * Output: YES when the database move succeeded.
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
 * Output: YES when the database move succeeded.
 */
BOOL on_library_trash_folder(NotesLibrary *lib, gint64 folder_id, NSString *name);
