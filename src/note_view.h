#import <Cocoa/Cocoa.h>

#include "app_core.h"
#include "document.h"
#include "note_edit.h"

/*
 * note_view — the note editor's view: one NotesNoteView edits one note.
 *
 * The note is an OnDocument.  The view lays it out with doc_layout.h and
 * draws text, images, tables and code blocks itself — it is a custom view
 * over the block model, NOT an NSTextView (BLOCK_MODEL.md says why), and no
 * subview ever lives inside it.  The window that hosts it
 * (editor_window.m) supplies the chrome — toolbar, find field, autosave,
 * status bar, the image viewer — puts the view in an NSScrollView, and
 * never touches the document except through the functions below.
 *
 * The API is C functions, as in the GTK build (whose note_view.h this is,
 * with the toolkit types removed); the view reports back through three
 * handler blocks instead of GObject signals.
 */
@interface NotesNoteView : NSView

/* The note changed in a way that needs saving.  Main thread. */
@property (nonatomic, copy) void (^edited_handler)(void);

/* on_note_view_inline_flags() has a new answer (the toolbar's B/I/U/S
 * buttons mirror it).  Main thread. */
@property (nonatomic, copy) void (^inline_flags_handler)(void);

/* An image in the note was clicked; the argument is its ordinal.  Main
 * thread. */
@property (nonatomic, copy) void (^image_activated_handler)(NSInteger ordinal);

/* The note the view edits (note_file rows are owned by a note, so attached
 * files go to it).  0 until the host sets it; a view with no note refuses
 * files.  Set once by the host, before the user can drop anything. */
@property (nonatomic, assign) gint64 note_id;

/* The pasteboard Cut, Copy and Paste use: the general one, unless a test
 * gives the view a private one so it never touches the user's clipboard. */
@property (nonatomic, strong) NSPasteboard *pasteboard;

/*
 * init_with_app: — a fresh view holding an empty note.
 *
 * Inputs:
 *   app — the application context, read for the preferences and the
 *         database (the #tag suggestions); not owned
 *
 * Output: the view.  The host frames it and puts it in a scroll view.
 */
- (instancetype)init_with_app:(OnApp *)app NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithFrame:(NSRect)frame NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

@end

/*
 * on_note_view_load_document — replace the note with a document, which the
 * view takes over (and frees with itself, or at the next load): caret to
 * the start, undo history cleared, nothing reported as an edit.  A view
 * already in a window (a reload) lays the new document out at once.
 *
 * Inputs:
 *   view — the view
 *   doc  — the document; ownership passes to the view
 */
void on_note_view_load_document(NotesNoteView *view, OnDocument *doc);

/*
 * on_note_view_reload_document — the same, for ANOTHER VERSION of the note
 * already shown (another writer rewrote it): the caret and selection keep
 * their positions (clamped into the new document) and the scroll position
 * stays; the undo history is cleared.
 *
 * Inputs:
 *   view — the view
 *   doc  — the document; ownership passes to the view
 */
void on_note_view_reload_document(NotesNoteView *view, OnDocument *doc);

/*
 * on_note_view_document — the note itself.  The host reads it (title,
 * tags) and hands it to on_note_document_save; every change other than an
 * external edit (below) goes through the view's functions.
 *
 * Output: the document, borrowed; valid until the next load or the view's
 *         deallocation.
 */
OnDocument *on_note_view_document(NotesNoteView *view);

/*
 * on_note_view_begin_external_edit / on_note_view_end_external_edit — the
 * pair behind OnApp.live_document / live_edited: something outside the view
 * (the pull, action_sync.c: an edit from the Action Items list or from
 * another app) is about to change the open note with on_document_*
 * operations.  begin closes any typing undo group and returns the
 * document; end re-clamps the caret and the selection — keeping both and
 * the scroll position, since the user did not move them — redraws and
 * calls edited_handler.
 *
 * Output (begin): the document, borrowed.
 */
OnDocument *on_note_view_begin_external_edit(NotesNoteView *view);
void on_note_view_end_external_edit(NotesNoteView *view);

/*
 * on_note_view_take_tags_modified / on_note_view_take_actions_modified —
 * what the host needs at save time, kept live by the document's operations;
 * each read clears its flag.  Actions reads YES once after every load.
 *
 * Output: whether a #tag, or an action line, was touched since the last
 *         read.
 */
BOOL on_note_view_take_tags_modified(NotesNoteView *view);
BOOL on_note_view_take_actions_modified(NotesNoteView *view);

/*
 * on_note_view_toggle_inline — bold / italic / underline / strike.  With a
 * selection: toggle the flag across it.  Without: flip the bit in the
 * current inline flags, so the next typed text carries it.  Calls
 * inline_flags_handler and takes the keyboard focus.
 *
 * Inputs:
 *   view — the view
 *   flag — one ON_FMT_INLINE_MASK bit
 */
void on_note_view_toggle_inline(NotesNoteView *view, OnFormatFlags flag);

/*
 * on_note_view_inline_flags — the inline styles newly typed text will get.
 *
 * Output: ON_FMT_INLINE_MASK bits.
 */
guint32 on_note_view_inline_flags(NotesNoteView *view);

/*
 * on_note_view_toggle_paragraph — give the selected blocks (or the caret's)
 * a paragraph kind, or make them body text when every one already has it.
 *
 * Inputs:
 *   view — the view
 *   flag — one ON_FMT_PARA_MASK bit, or 0 for body text
 */
void on_note_view_toggle_paragraph(NotesNoteView *view, guint32 flag);

/*
 * on_note_view_insert_image — an image at the caret, on a line of its own,
 * shown at the thumbnail size.  The bytes are stored as given: the caller
 * encodes ONCE (image_io.h: on_image_file_to_png / on_image_png_encode).
 *
 * Inputs:
 *   view — the view
 *   png  — PNG bytes; copied
 *   n    — how many
 */
void on_note_view_insert_image(NotesNoteView *view, const uint8_t *png, size_t n);

/*
 * on_note_view_drop_files — files dropped on the note: they become tiles
 * (pictures, images) at the drop point, or at the end of the note's last
 * line when the "drop_files_last_line" setting is on.  An existing
 * selection is never replaced.  What performDragOperation: does.
 *
 * Inputs:
 *   view  — the view
 *   urls  — the dropped files, in order
 *   point — where they were dropped, in the view's coordinates
 */
void on_note_view_drop_files(NotesNoteView *view, NSArray<NSURL *> *urls, NSPoint point);

/*
 * on_note_view_insert_table — a fresh 3x3 table at the caret.
 * on_note_view_insert_date  — today's date (YYYY-MM-DD) as typed text.
 */
void on_note_view_insert_table(NotesNoteView *view);
void on_note_view_insert_date(NotesNoteView *view);

/*
 * on_note_view_undo / on_note_view_redo — step the document's history; the
 * caret lands where the step changed things.  (Whether a step exists is
 * on_document_can_undo / can_redo on on_note_view_document.)
 */
void on_note_view_undo(NotesNoteView *view);
void on_note_view_redo(NotesNoteView *view);

/*
 * on_note_view_find — highlight every case-insensitive match and select
 * the first one at or after the caret (scrolled into view), so a query
 * lands somewhere as it is typed.
 *
 * Inputs:
 *   view — the view
 *   text — what to find; nil or @"" clears the highlights
 */
void on_note_view_find(NotesNoteView *view, NSString *text);

/*
 * on_note_view_find_step — select and scroll to the next match after the
 * caret, or the previous one, wrapping at the ends.
 *
 * Inputs:
 *   view    — the view
 *   text    — what to find
 *   forward — NO for the previous match
 */
void on_note_view_find_step(NotesNoteView *view, NSString *text, BOOL forward);

/*
 * on_note_view_selected_text — the selection as plain text (Use Selection
 * for Find).
 *
 * Inputs:
 *   view — the view
 *
 * Output: the text, or nil when nothing is selected.
 */
NSString *on_note_view_selected_text(NotesNoteView *view);

/*
 * IMAGE ORDINALS — the note's images numbered in document order, block and
 * inline alike: the app's one way of addressing a note's images (the order
 * on_note_count_images walks a blob in).
 *
 * on_note_view_image_count  — how many.
 * on_note_view_image_size   — its pixel dimensions, from the stored
 *                             bytes' header (nothing decoded); NO when there
 *                             is no such image or the header will not parse.
 * on_note_view_image_png    — its stored bytes, borrowed, or NULL.
 * on_note_view_image_open   — open it in the outside image viewer; NO when
 *                             there is no such image or it did not open.
 * on_note_view_image_reveal — caret to the image, scrolled into view; NO
 *                             when there is no such image.
 */
NSInteger  on_note_view_image_count(NotesNoteView *view);
BOOL       on_note_view_image_size(NotesNoteView *view, NSInteger ordinal, int *w, int *h);
GBytes    *on_note_view_image_png(NotesNoteView *view, NSInteger ordinal);
BOOL       on_note_view_image_open(NotesNoteView *view, NSInteger ordinal);
BOOL       on_note_view_image_reveal(NotesNoteView *view, NSInteger ordinal);

/*
 * on_note_view_action_reveal — caret to the note's ordinal-th action item,
 * scrolled into view.
 *
 * Output: NO when there is no such item.
 */
BOOL on_note_view_action_reveal(NotesNoteView *view, NSInteger ordinal);

/*
 * on_note_view_settings_changed — re-read the preferences the view renders
 * from (title line, code line numbers, the "copy" word) and the appearance,
 * and apply them now.
 */
void on_note_view_settings_changed(NotesNoteView *view);

/*
 * THE KEYBOARD AND POINTER AS FUNCTION CALLS — what keyDown:, the input
 * method and mouseDown: do, callable, so the UI probe can replay the
 * scripts in tests/ui/ against the real view and check the document
 * and the caret.  They run the same code as real events.
 *
 * on_note_view_feed_key   — a key press after the input method passed on
 *                           it.  `character` is the code point for
 *                           on_key_character, else ignored; `mods` is
 *                           on_mod_t bits.  Returns YES when the key was
 *                           handled.
 * on_note_view_feed_text  — committed text, as the input method delivers
 *                           typed characters (UTF-8).
 * on_note_view_feed_click — a press at (x, y) in the view's coordinates:
 *                           `clicks` 1/2/3, `mods` on_mod_t bits,
 *                           `secondary` for the right button.  Returns YES
 *                           when the press was consumed by something that
 *                           is not text (a checkbox, a copy word, an image).
 * on_note_view_caret      — where the caret is.
 */
BOOL  on_note_view_feed_key(NotesNoteView *view, on_key_t key, uint32_t character,
                            unsigned mods);
void  on_note_view_feed_text(NotesNoteView *view, const char *text);
BOOL  on_note_view_feed_click(NotesNoteView *view, CGFloat x, CGFloat y,
                              NSInteger clicks, unsigned mods, BOOL secondary);
OnPos on_note_view_caret(NotesNoteView *view);
