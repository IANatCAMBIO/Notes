#import "note_view.h"
#import "ui_util.h"

#include "look.h"

/*
 * note_view.m — STUB (work package C replaces this file).
 *
 * Shows the note as plain text so the editor window, the save path and the
 * flush-on-quit contract can be exercised before the real editor exists.
 * Nothing can be typed.  See note_view.h for the contract and
 * porting/gtk_reference/src/note_view.c for the behaviour to implement.
 */

@interface NotesNoteView ()
@property (nonatomic, assign) OnApp *app;
@property (nonatomic, assign) OnDocument *doc;      /* owned: freed at load and dealloc */
@end

@implementation NotesNoteView

/*
 * init_with_app: — see note_view.h.
 */
- (instancetype)
init_with_app:(OnApp *)app
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    _app = app;
    _doc = on_document_new();
    return self;
}

/*
 * dealloc — the view owns its document.
 */
- (void)
dealloc
{
    on_document_free(_doc);
}

/*
 * isFlipped — content coordinates run top-down, as doc_layout's do.
 *
 * Output: YES.
 */
- (BOOL)
isFlipped
{
    return YES;
}

/*
 * plain_text — the note as the stub shows it.
 *
 * Output: the document's plain text, one line per block.
 */
- (NSString *)
plain_text
{
    gchar    *text = on_document_plain_text(self.doc);
    NSString *s    = @(text);
    g_free(text);
    return s;
}

/*
 * text_attributes — how the stub draws: body font, the look's text colour.
 *
 * Output: attributes for NSString drawing.
 */
- (NSDictionary<NSAttributedStringKey, id> *)
text_attributes
{
    return @{
        NSFontAttributeName:            [NSFont systemFontOfSize:on_look_font_size],
        NSForegroundColorAttributeName: on_ui_color(on_look_text),
    };
}

/*
 * fit_height — make the view as tall as its text, so the scroll view
 * scrolls it.  Called after a load, an external edit and a width change.
 */
- (void)
fit_height
{
    CGFloat width = MAX(self.frame.size.width - on_look_margin_left - on_look_margin_right, 1.0);
    NSRect  box   = [[self plain_text] boundingRectWithSize:NSMakeSize(width, CGFLOAT_MAX)
                                                    options:NSStringDrawingUsesLineFragmentOrigin
                                                 attributes:[self text_attributes]];
    CGFloat height = ceil(box.size.height) + on_look_margin_top + on_look_margin_bottom;
    [super setFrameSize:NSMakeSize(self.frame.size.width, height)];
    self.needsDisplay = YES;
}

/*
 * setFrameSize: — a new width re-wraps the text, which changes the height.
 *
 * Inputs:
 *   size — the size the scroll view proposes
 */
- (void)
setFrameSize:(NSSize)size
{
    BOOL width_changed = size.width != self.frame.size.width;
    [super setFrameSize:size];
    if (width_changed) [self fit_height];
}

/*
 * drawRect: — the page, then the note's plain text inside the margins.
 *
 * Inputs:
 *   dirty — the area to repaint (the whole text is drawn)
 */
- (void)
drawRect:(NSRect)dirty
{
    [on_ui_color(on_look_page) setFill];
    NSRectFill(dirty);
    NSRect box = NSMakeRect(on_look_margin_left, on_look_margin_top,
                            self.bounds.size.width - on_look_margin_left - on_look_margin_right,
                            self.bounds.size.height - on_look_margin_top);
    [[self plain_text] drawWithRect:box
                            options:NSStringDrawingUsesLineFragmentOrigin
                         attributes:[self text_attributes]];
}

@end

/*
 * on_note_view_load_document — see note_view.h.
 */
void
on_note_view_load_document(NotesNoteView *view, OnDocument *doc)
{
    on_document_free(view.doc);
    view.doc = doc;
    on_document_clear_undo(doc);
    [view fit_height];
}

/*
 * on_note_view_document — see note_view.h.
 */
OnDocument *
on_note_view_document(NotesNoteView *view)
{
    return view.doc;
}

/*
 * on_note_view_begin_external_edit — see note_view.h.  (The stub has no
 * typing group to close.)
 */
OnDocument *
on_note_view_begin_external_edit(NotesNoteView *view)
{
    return view.doc;
}

/*
 * on_note_view_end_external_edit — see note_view.h.
 */
void
on_note_view_end_external_edit(NotesNoteView *view)
{
    [view fit_height];
    if (view.edited_handler) view.edited_handler();
}

/*
 * on_note_view_take_tags_modified — see note_view.h.
 */
BOOL
on_note_view_take_tags_modified(NotesNoteView *view)
{
    return on_document_take_tags_modified(view.doc);
}

/*
 * on_note_view_take_actions_modified — see note_view.h.
 */
BOOL
on_note_view_take_actions_modified(NotesNoteView *view)
{
    return on_document_take_actions_modified(view.doc);
}

/*
 * on_note_view_toggle_inline — see note_view.h (stub: no-op).
 */
void
on_note_view_toggle_inline(NotesNoteView *view, OnFormatFlags flag)
{
}

/*
 * on_note_view_inline_flags — see note_view.h (stub: none armed).
 */
guint32
on_note_view_inline_flags(NotesNoteView *view)
{
    return 0;
}

/*
 * on_note_view_toggle_paragraph — see note_view.h (stub: no-op).
 */
void
on_note_view_toggle_paragraph(NotesNoteView *view, guint32 flag)
{
}

/*
 * on_note_view_insert_image — see note_view.h (stub: no-op).
 */
void
on_note_view_insert_image(NotesNoteView *view, const uint8_t *png, size_t n)
{
}

/*
 * on_note_view_insert_table — see note_view.h (stub: no-op).
 */
void
on_note_view_insert_table(NotesNoteView *view)
{
}

/*
 * on_note_view_insert_date — see note_view.h (stub: no-op).
 */
void
on_note_view_insert_date(NotesNoteView *view)
{
}

/*
 * on_note_view_undo — see note_view.h (stub: no-op).
 */
void
on_note_view_undo(NotesNoteView *view)
{
}

/*
 * on_note_view_redo — see note_view.h (stub: no-op).
 */
void
on_note_view_redo(NotesNoteView *view)
{
}

/*
 * on_note_view_find — see note_view.h (stub: no-op).
 */
void
on_note_view_find(NotesNoteView *view, NSString *text)
{
}

/*
 * on_note_view_find_step — see note_view.h (stub: no-op).
 */
void
on_note_view_find_step(NotesNoteView *view, NSString *text, BOOL forward)
{
}

/*
 * on_note_view_image_count — see note_view.h.
 */
NSInteger
on_note_view_image_count(NotesNoteView *view)
{
    return on_document_image_count(view.doc);
}

/*
 * on_note_view_image — see note_view.h (stub: nothing is decoded).
 */
CGImageRef
on_note_view_image(NotesNoteView *view, NSInteger ordinal)
{
    return NULL;
}

/*
 * on_note_view_image_png — see note_view.h.
 */
GBytes *
on_note_view_image_png(NotesNoteView *view, NSInteger ordinal)
{
    return on_document_image_nth(view.doc, (gint)ordinal, NULL);
}

/*
 * on_note_view_image_reveal — see note_view.h (stub: nothing to scroll to).
 */
BOOL
on_note_view_image_reveal(NotesNoteView *view, NSInteger ordinal)
{
    return NO;
}

/*
 * on_note_view_action_reveal — see note_view.h (stub: nothing to scroll to).
 */
BOOL
on_note_view_action_reveal(NotesNoteView *view, NSInteger ordinal)
{
    return NO;
}

/*
 * on_note_view_settings_changed — see note_view.h.
 */
void
on_note_view_settings_changed(NotesNoteView *view)
{
    view.needsDisplay = YES;
}

/*
 * on_note_view_feed_key — see note_view.h (stub: no key is handled).
 */
BOOL
on_note_view_feed_key(NotesNoteView *view, on_key_t key, uint32_t character, unsigned mods)
{
    return NO;
}

/*
 * on_note_view_feed_text — see note_view.h (stub: no-op).
 */
void
on_note_view_feed_text(NotesNoteView *view, const char *text)
{
}

/*
 * on_note_view_feed_click — see note_view.h (stub: nothing is hit).
 */
BOOL
on_note_view_feed_click(NotesNoteView *view, CGFloat x, CGFloat y, NSInteger clicks,
                        unsigned mods, BOOL secondary)
{
    return NO;
}

/*
 * on_note_view_caret — see note_view.h (stub: the start of the note).
 */
OnPos
on_note_view_caret(NotesNoteView *view)
{
    return (OnPos){ 0, -1, 0 };
}
