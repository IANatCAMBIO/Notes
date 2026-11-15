/* ===========================================================================
 * apple_notes.h — a one-way copy of every note into Apple Notes
 *
 * Apple Notes has no public API that writes rich text, so this module uses
 * the framework Notes.app's own extensions use, NotesShared (private,
 * loaded with dlopen at run time, never linked): ICNoteContext opens the
 * same NoteStore through Core Data, ICNote and ICTTMergeableString hold a
 * note as attributed text, and Notes.app, told of the save through its
 * cross-process coordinator, merges the change and uploads it to iCloud.
 * Every selector is checked before use; a macOS that lacks one turns the
 * feature off with a message rather than crashing.
 *
 * THE CARVE-OUT.  Everything this module writes lives in ONE folder of the
 * user's Apple Notes account, ON_APPLE_NOTES_FOLDER (identity.h), made
 * when it is missing.  No other folder or note is read or written: a note
 * of ours is found again only by the Apple identifier the binding map
 * recorded for it.  Hashtags are the one thing that shows outside the
 * folder: a tag on our tag line becomes a real Apple tag, so it is listed
 * in Apple Notes' tag browser (the user's choice).
 *
 * ONE WAY.  The copy is pushed; nothing is read back.  A sweep runs a few
 * times a minute while the setting is on: when the shared database's notes
 * revision moved, every note's stamp (db.h OnNoteStamp), pin, trash state,
 * creation date and tags are compared with what was pushed last, and the
 * notes that differ are pushed again, one every quarter second so the app
 * stays responsive and the library's status bar can count them ("Syncing 3
 * of 12 notes to Apple Notes").  A note that left the database (deleted
 * for good) has its copy moved to Apple's Recently Deleted.
 *
 * THE MAPPING, our block model (document.h) to Notes' paragraph styles:
 * the title line is Title; H1 Heading, H2 Subheading; bullets, numbers and
 * check items their lists, a check item's tick its to-do state; code lines
 * Monospaced paragraphs; bold, italic, underline and strike their
 * attributes; an action line ('!', document.h on_block_action_mark) is
 * coloured on_look_action from its mark to its end; an image or an
 * attached file is an attachment where it stood (an image shown at the
 * default thumbnail size is Small, any chosen width Large); a table is a
 * table attachment of plain-text cells.  The tag line becomes the line
 * under the title, led by 🏷️, then one Apple hashtag per tag.
 *
 * OFFSETS.  Nothing here converts an index (porting/INTERFACES.md §8): the
 * attributed string is built by appending each run's bytes as an NSString,
 * so the one place a position matters, the action mark, splits a run at a
 * byte offset the walk already stands on.
 *
 * NEEDS Full Disk Access: Notes' store is in a protected group container.
 * Without it the setting stays on but the status says what to grant.
 *
 * Main thread only (the store's context is Notes' main-queue context).
 * =========================================================================== */
#ifndef ON_APPLE_NOTES_H
#define ON_APPLE_NOTES_H

#import <Foundation/Foundation.h>
#include "app_core.h"
#include "document.h"

/* The ini key of the setting (OnApp.apple_notes).                           */
#define ON_APPLE_NOTES_KEY "apple_notes"

/* Notes' attribute keys on its attributed text and the paragraph styles the
 * converter writes — one list, shared with the test.                       */
#define ON_APPLE_ATTR_STYLE      @"TTStyle"         /* an ICTTParagraphStyle   */
#define ON_APPLE_ATTR_HINTS      @"TTHints"         /* bits: 1 bold, 2 italic  */
#define ON_APPLE_ATTR_UNDERLINE  @"TTUnderline"     /* @1                      */
#define ON_APPLE_ATTR_STRIKE     @"TTStrikethrough" /* @1                      */
#define ON_APPLE_ATTR_COLOR      @"TTColor"         /* a CGColor               */
#define ON_APPLE_ATTR_ATTACHMENT @"NSAttachment"    /* an ICTTAttachment       */
#define ON_APPLE_HINT_BOLD   1
#define ON_APPLE_HINT_ITALIC 2
typedef enum {
    on_apple_style_title      = 0,
    on_apple_style_heading    = 1,
    on_apple_style_subheading = 2,
    on_apple_style_body       = 3,
    on_apple_style_monospaced = 4,
    on_apple_style_bullet     = 100,
    on_apple_style_numbered   = 102,
    on_apple_style_checklist  = 103,
} on_apple_style_t;
#define ON_APPLE_UTI_HASHTAG @"com.apple.notes.inlinetextattachment.hashtag"
#define ON_APPLE_UTI_TABLE   @"com.apple.notes.table"
/* The character the tag line starts with, then a space.                     */
#define ON_APPLE_TAG_LINE_LEAD @"\U0001F3F7️ "

/*
 * apple_notes_objects — where the converter gets a note's objects: each
 * method makes the object in Notes (a hashtag, an attachment) and returns
 * the ICTTAttachment that stands for it in the text, or nil when Notes
 * would not make it (the converter then leaves the object out, a hashtag
 * becoming its plain "#name").  The live source is bound to the ICNote
 * being written; the test's makes nothing.
 */
@protocol apple_notes_objects <NSObject>
- (id)hashtag_attachment:(NSString *)name;
- (id)image_attachment:(NSData *)png small:(BOOL)small;
- (id)file_attachment:(int64_t)file_id name:(NSString *)name;
- (id)table_attachment:(NSArray<NSArray<NSString *> *> *)rows;
@end

/*
 * on_apple_notes_load_framework — load NotesShared (and NotesUI, whose
 * helpers the table builder needs) and check every class and selector
 * the module uses.  Idempotent.
 *
 * Inputs:
 *   why — out, optional: what is missing, when the answer is NO
 *
 * Output: YES when the framework is usable.
 */
BOOL on_apple_notes_load_framework(NSString **why);

/*
 * on_apple_notes_text — a note's document as Notes' attributed text, per
 * the mapping above.  Requires the framework loaded; needs no store.
 *
 * Inputs:
 *   doc     — the document
 *   objects — the object source
 *
 * Output: the text, paragraphs separated by "\n", none at the end.
 */
NSAttributedString *on_apple_notes_text(const OnDocument *doc,
                                        id<apple_notes_objects> objects);

/*
 * on_apple_notes_start — remember the app and, when the setting is on,
 * open the store and start the sweep.  Call once the database is open.
 *
 * Inputs:
 *   app — the application context (held)
 */
void on_apple_notes_start(OnApp *app);

/*
 * on_apple_notes_apply — the setting changed: start or stop accordingly.
 *
 * Inputs:
 *   app — the application context
 */
void on_apple_notes_apply(OnApp *app);

/*
 * on_apple_notes_push_all — forget what was pushed and copy every note
 * again (the Settings button).  Nothing happens while the setting is off.
 *
 * Inputs:
 *   app — the application context
 */
void on_apple_notes_push_all(OnApp *app);

/*
 * on_apple_notes_stop — stop the sweep and close the binding map.  Call
 * before the database closes.
 */
void on_apple_notes_stop(void);

/*
 * on_apple_notes_status — one line for Settings: off, what was pushed, or
 * what stands in the way (Full Disk Access, a missing selector).
 *
 * Output: the line.
 */
NSString *on_apple_notes_status(void);

/*
 * on_apple_notes_observe — run a block on the main thread whenever the
 * status line changes.  One observer at a time; nil removes it.
 *
 * Inputs:
 *   changed — the block, or nil
 */
void on_apple_notes_observe(void (^changed)(void));

/*
 * on_apple_notes_progress — how far a run of pushes has got, for a progress
 * line: the note in hand, counted from 1, of the notes the last sweep (or
 * Push All Now) queued.  Both 0 while nothing is being pushed.
 *
 * Inputs:
 *   current — set to the note in hand
 *   total   — set to the run's size
 */
void on_apple_notes_progress(int *current, int *total);

/*
 * on_apple_notes_observe_progress — run a block on the main thread whenever
 * the progress changes: before each push and when the run ends.  One
 * observer at a time (the library window); nil removes it.
 *
 * Inputs:
 *   changed — the block, or nil
 */
void on_apple_notes_observe_progress(void (^changed)(void));

#endif /* ON_APPLE_NOTES_H */
