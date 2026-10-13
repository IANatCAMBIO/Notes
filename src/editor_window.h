#import <Cocoa/Cocoa.h>

#include "app_core.h"

/*
 * editor_window — the note editor window: one window per open note, around
 * one NotesNoteView (note_view.h).
 *
 * The window supplies the chrome — toolbar, find field, status bar, the
 * image viewer — and owns the note's LIFE while it is open: it loads the
 * note (outside changes to its items pulled in first, action_sync.h),
 * saves it a moment after every edit, and on close either saves the last edits or, if the note was left
 * blank, deletes it.  This module also keeps the REGISTRY of open editors
 * (note id -> window controller): it is how a note is never open twice,
 * how the core finds a note's live document, and how every editor is
 * flushed before the app quits.
 *
 * OWNERSHIP.  The registry holds each editor's window controller strongly
 * from open to close; nothing else does.  An editor leaves the registry in
 * its windowWillClose:, after its final save, having removed its autosave
 * timer and cleared the view's handler blocks.
 */

/* What an editor scrolls to when it opens. */
typedef NS_ENUM(NSInteger, NotesEditorReveal) {
    NotesEditorRevealNone,
    NotesEditorRevealImage,          /* the note's ordinal-th image         */
    NotesEditorRevealAction,         /* its ordinal-th action item          */
};

/*
 * on_editor_install — install the hooks the core reaches editors through:
 * OnApp.open_note, live_document, live_edited and flush_editors.  Called
 * once at startup, before on_app_start_services.
 *
 * Inputs:
 *   app — the application context
 */
void on_editor_install(OnApp *app);

/*
 * on_editor_window_open — open the editor for a note and bring it to the
 * front; if the note is already open, that window is brought forward (and
 * the reveal and search applied to it).
 *
 * Inputs:
 *   app         — the application context
 *   note_id     — the note
 *   reveal      — what to scroll to
 *   ordinal     — which image or action item (ignored for RevealNone)
 *   search_term — seeds the editor's find field and highlights the matches
 *                 (a note opened from the search window); nil for none
 *
 * Output: NO when the note does not exist.
 */
BOOL on_editor_window_open(OnApp *app, gint64 note_id, NotesEditorReveal reveal,
                           NSInteger ordinal, NSString *search_term);

/*
 * on_editor_close_notes — close the editors of these notes, each saving
 * its unsaved edits first.  Called before notes are trashed or deleted, so
 * no editor is left open on a note that is gone.  An editor whose save
 * fails stays open, and the caller must then leave its note alone.
 *
 * Inputs:
 *   ids — note ids; those with no open editor are skipped
 *   n   — how many
 *
 * Output: NO when an editor could not save and is still open.
 */
BOOL on_editor_close_notes(const gint64 *ids, size_t n);

/*
 * on_editor_report_unsaved — THE tool panel for "an editor could not save,
 * so this did not happen": its title names what was stopped.
 *
 * Inputs:
 *   title  — what did not happen, e.g. @"Could not quit"
 *   window — the window to centre over; nil = the screen
 */
void on_editor_report_unsaved(NSString *title, NSWindow *window);

/*
 * on_editor_close_all — close every editor NOW: each saves its unsaved
 * edits, or deletes its note if it was left blank, before this returns.
 * An editor whose save fails stays open.  This is the OnApp.flush_editors
 * hook (app_lifecycle.h, "the flush-on-quit contract").
 *
 * Output: NO when an editor could not save and is still open.
 */
BOOL on_editor_close_all(void);

/*
 * on_editor_settings_changed — a preference that editors render from
 * changed (title line, code line numbers, the copy word, the compact
 * toolbar, the status bar's path and note id): every open editor re-reads
 * the preferences from OnApp and applies them.  Called by Settings.
 */
void on_editor_settings_changed(void);
