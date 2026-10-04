/* ===========================================================================
 * identity.h — who this build is: the ONE place each name is spelled
 *
 * This fork is deliberately a different application from the GTK Notes it
 * was forked from: its own bundle, its own preferences and database
 * directory, its own IPC socket.  Nothing here may ever name a path the GTK
 * build uses (~/.local/share/notes, ~/.config/notes, ~/.cache/notes-*.sock,
 * an ini next to the binary), so the two can be installed side by side and
 * can never open, hand off to, or receive from each other.
 *
 * Renaming the app is an edit to this file and nothing else: the Makefile
 * reads the bundle name and identifier out of it (see `ident` there), and
 * every C and Objective-C module includes it.  Keep each definition on one
 * line in the form `#define NAME "value"` — that is what the Makefile
 * parses.
 * =========================================================================== */

#ifndef ON_IDENTITY_H
#define ON_IDENTITY_H

/* The name shown to the user: menu titles, window titles, the About panel. */
#define ON_APP_NAME "Notes AppKit"

/* The bundle is <ON_BUNDLE_NAME>.app and its GUI executable is
 * Contents/MacOS/<ON_BUNDLE_NAME>.                                          */
#define ON_BUNDLE_NAME "NotesAppKit"

/* CFBundleIdentifier; also the prefix of the pasteboard types (ui_util.h). */
#define ON_BUNDLE_ID "com.cambio.notes-appkit"

/* The command-line program, installed beside the GUI executable in
 * Contents/MacOS.  It is what the Claude skill and scripts call.           */
#define ON_CLI_NAME "notes"

/* The data directory is ~/Library/Application Support/<ON_DATA_DIR_NAME>:
 * notes.ini, the default notes.db, the default backups/ folder and the IPC
 * socket all live in it (see config.h for the policy).                      */
#define ON_DATA_DIR_NAME "NotesAppKit"

/* Environment variable that replaces the data directory wholesale.  Every
 * development build, run and test sets it to a folder under dev/.           */
#define ON_DATA_DIR_ENV "NOTES_DATA_DIR"

/* File names inside the data directory.  (The database file name is
 * ON_DB_FILENAME in db.h: it is part of the on-disk contract, not of this
 * build's identity.)                                                        */
#define ON_CONFIG_FILENAME "notes.ini"
#define ON_SOCKET_FILENAME "notes-appkit.sock"

#endif /* ON_IDENTITY_H */
