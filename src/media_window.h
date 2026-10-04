#import <Cocoa/Cocoa.h>

#import "library_state.h"

/*
 * media_window — the media browser: every image in the notes the library is
 * listing, as thumbnails decoded at thumbnail size (on_note_image_nth with
 * a cap, never full size), with the shared image viewer.
 *
 * The module registers its own commands with the library, so the menus and
 * the toolbar reach it without any other module naming it; until it does,
 * those items are disabled.
 *
 * STATUS: media_window.m is a STUB for work package H that registers
 * nothing.  The specification is porting/gtk_reference/src/media_window.c.
 * This header is FROZEN.
 */

/*
 * on_media_install — register the "media" command, which opens a media
 * browser on on_library_notes_for_selection.  Called once at startup by the
 * app delegate.
 *
 * Inputs:
 *   lib — the library
 */
void on_media_install(NotesLibrary *lib);
