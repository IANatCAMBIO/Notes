/* ===========================================================================
 * identity.h — who this build is: the ONE place each name is spelled
 *
 * This fork is deliberately a different application from the GTK Notes it
 * was forked from: its own bundle, its own preferences and database
 * directory, its own lock.  Nothing here may ever name a path the GTK
 * build uses (~/.local/share/notes, ~/.config/notes, ~/.cache/notes-*.sock,
 * an ini next to the binary), so the two can never open each other's
 * files.  The bundle NAME is shared (Notes.app):
 * installing this build replaces a GTK copy in ~/Applications.
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
#define ON_APP_NAME "Notes"

/* The bundle is <ON_BUNDLE_NAME>.app and its GUI executable is
 * Contents/MacOS/<ON_BUNDLE_NAME>.                                          */
#define ON_BUNDLE_NAME "Notes"

/* CFBundleIdentifier; also the prefix of the pasteboard types (ui_util.h). */
#define ON_BUNDLE_ID "com.cambio.notes"

/* The data directory is ~/Library/Application Support/<ON_DATA_DIR_NAME>:
 * notes.ini and the app lock live in it (see config.h for the policy).  The
 * notes are in the database shared with Tasks (shared_db.h), not here.     */
#define ON_DATA_DIR_NAME "Notes"

/* Environment variable that replaces the data directory wholesale.  Every
 * development build, run and test sets it to a folder under dev/.           */
#define ON_DATA_DIR_ENV "NOTES_DATA_DIR"

/* File names inside the data directory.                                    */
#define ON_CONFIG_FILENAME "notes.ini"
#define ON_LOCK_FILENAME "notes.lock"   /* one app per data directory
                                           (app_lifecycle.h)                 */

#endif /* ON_IDENTITY_H */
