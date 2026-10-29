/* ===========================================================================
 * note_view.m — the note editor's view
 *
 * NotesNoteView is a custom NSView implementing NSTextInputClient.  It owns
 * an OnDocument (the note), lays it out with doc_layout.h and draws text,
 * images, tables and code blocks itself.  It is NOT an NSTextView —
 * BLOCK_MODEL.md explains why.
 *
 * Input path: keyDown: → interpretKeyEvents: → insertText:replacementRange:
 * or doCommandBySelector:, then the editing primitives that wrap
 * on_document_* operations.
 * =========================================================================== */

#import "note_view.h"
#import "shared_tag_popup.h"
#import "ui_util.h"

#include "db.h"
#include "doc_layout.h"
#include "image_io.h"
#include "look.h"
#include "note_edit.h"
#include "utf_index.h"

#include <string.h>
#include <time.h>

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

/* ── Timing ─────────────────────────────────────────────────────────────── */

/* A typing undo group closes after this many ms of quiet. */
static const NSTimeInterval typing_undo_ms  = 1.0;

/* A typing burst of this many sentence-enders caps a group. */
static const NSInteger max_sentences = 5;

/* Caret blink interval. */
static const NSTimeInterval blink_interval = 0.56;

/* ── Forward declarations ────────────────────────────────────────────────── */

@interface NotesNoteView () <NSTextInputClient>

@property (nonatomic, assign) OnApp      *app;
@property (nonatomic, assign) OnDocument *doc;          /* owned */
@property (nonatomic, assign) OnDocLayout *layout;      /* owned */

/* Selection: caret is the moving end, anchor the fixed end.
 * Equal means no selection. */
@property (nonatomic, assign) OnPos caret;
@property (nonatomic, assign) OnPos anchor;

/* Inline flags armed for the next typed character. */
@property (nonatomic, assign) guint32 inline_flags;

/* Vertical-movement goal column (-1 = derive from caret). */
@property (nonatomic, assign) double goal_x;

/* Typing undo group. */
@property (nonatomic, assign) BOOL typing;
@property (nonatomic, assign) NSInteger sentences;
@property (nonatomic, strong) NSTimer *typing_timer;

/* Caret blink. */
@property (nonatomic, strong) NSTimer *blink_timer;
@property (nonatomic, assign) BOOL caret_on;

/* Selection drag in progress. */
@property (nonatomic, assign) BOOL dragging;
@property (nonatomic, assign) OnPos drag_anchor;

/* Whether no save has happened since load (first take_actions_modified
 * call should answer YES). */
@property (nonatomic, assign) BOOL fresh;

/* Find state. */
@property (nonatomic, copy) NSString *find_text;

/* Input method preedit. */
@property (nonatomic, copy) NSString *preedit;     /* marked text, or nil      */
@property (nonatomic, assign) NSRange preedit_sel; /* the IME's caret inside it */

/* What was under the last context click (for context-menu actions). */
@property (nonatomic, assign) OnDocHitResult ctx_hit;

/* The key event keyDown: is interpreting (nil otherwise): which key a
 * command came from (home_end_command). */
@property (nonatomic, strong) NSEvent *key_event;

/* #tag capture: on while a "#word" is being typed; where its '#' is; the
 * known tags (fetched once per capture); the suggestion list. */
@property (nonatomic, assign) BOOL           tag_capturing;
@property (nonatomic, assign) OnPos          tag_start;
@property (nonatomic, assign) GList         *tag_choices;
@property (nonatomic, strong) SharedTagPopup *tag_popup;

@end

/* The #tag capture, defined with its helpers below; the editing
 * primitives call into it. */
static void tag_capture_end(NotesNoteView *v, BOOL apply);
static void tag_capture_consider(NotesNoteView *v, const char *text, size_t n);
static void tag_popup_update(NotesNoteView *v);
static void tag_choose(NotesNoteView *v, NSString *name);

/* ── Document observer ───────────────────────────────────────────────────── */

static void
obs_block_changed(OnDocument *doc, guint i, gpointer data)
{
    (void)doc;
    NotesNoteView *v = (__bridge NotesNoteView *)data;
    on_doc_layout_block_changed(v.layout, i);
}

static void
obs_blocks_inserted(OnDocument *doc, guint i, guint n, gpointer data)
{
    (void)doc;
    NotesNoteView *v = (__bridge NotesNoteView *)data;
    on_doc_layout_blocks_inserted(v.layout, i, n);
}

static void
obs_blocks_removed(OnDocument *doc, guint i, guint n, gpointer data)
{
    (void)doc;
    NotesNoteView *v = (__bridge NotesNoteView *)data;
    on_doc_layout_blocks_removed(v.layout, i, n);
}

static const OnDocumentObserver OBSERVER = {
    .block_changed    = obs_block_changed,
    .blocks_inserted  = obs_blocks_inserted,
    .blocks_removed   = obs_blocks_removed,
};

/* ── Small helpers ───────────────────────────────────────────────────────── */

/*
 * pos_eq — two positions the same.
 *
 * Inputs:
 *   a, b — positions to compare
 *
 * Output: YES when equal.
 */
static BOOL
pos_eq(OnPos a, OnPos b)
{
    return on_pos_cmp(a, b) == 0;
}

/*
 * has_selection — the caret and anchor differ.
 *
 * Inputs:
 *   v — the view
 *
 * Output: YES when there is a selection.
 */
static BOOL
has_selection(NotesNoteView *v)
{
    return !pos_eq(v.caret, v.anchor);
}

/*
 * sel_bounds — the selection in document order.
 *
 * Inputs:
 *   v — the view
 *   a — receives the earlier end
 *   b — receives the later end
 *
 * Output: none.
 */
static void
sel_bounds(NotesNoteView *v, OnPos *a, OnPos *b)
{
    if (on_pos_cmp(v.anchor, v.caret) <= 0) {
        *a = v.anchor;
        *b = v.caret;
    } else {
        *a = v.caret;
        *b = v.anchor;
    }
}

/*
 * clamp_pos — the valid position nearest to p in the document (GTK:
 * clamp_pos): past the end means the end; beside an image or a table at
 * offset 0 or 1; in a cell that exists; within its text, never inside a
 * UTF-8 sequence.
 *
 * Inputs:
 *   doc — the document
 *   p   — the position to clamp
 *
 * Output: the clamped position.
 */
static OnPos
clamp_pos(const OnDocument *doc, OnPos p)
{
    guint n = on_document_n_blocks(doc);
    if (n == 0) return (OnPos){ 0, -1, 0 };
    if (p.block >= n) {
        p.block  = n - 1;
        p.cell   = -1;
        p.offset = G_MAXSIZE;
    }
    const OnBlock *b = on_document_block(doc, p.block);
    if (b->kind == ON_BLOCK_IMAGE || (b->kind == ON_BLOCK_TABLE && p.cell < 0)) {
        p.cell   = -1;
        p.offset = MIN(p.offset, 1);
        return p;
    }
    if (b->kind == ON_BLOCK_TABLE) {
        if (p.cell >= b->rows * b->cols) p.cell = b->rows * b->cols - 1;
    } else {
        p.cell = -1;
    }
    OnText *t = on_document_text_at(doc, p);
    if (t == NULL) return (OnPos){ p.block, -1, 0 };
    if (p.offset > t->text->len) p.offset = t->text->len;
    while (p.offset > 0 && p.offset < t->text->len &&
           ((guchar)t->text->str[p.offset] & 0xc0) == 0x80)
        p.offset--;
    return p;
}

/*
 * caret_block — the block the caret sits in.
 *
 * Inputs:
 *   v — the view
 *
 * Output: the block, never NULL (document always has at least one).
 */
static const OnBlock *
caret_block(NotesNoteView *v)
{
    return on_document_block(v.doc, v.caret.block);
}

/*
 * caret_text — the OnText the caret addresses, or NULL for an image block.
 *
 * Inputs:
 *   v — the view
 *
 * Output: the text, or NULL.
 */
static OnText *
caret_text(NotesNoteView *v)
{
    return on_document_text_at(v.doc, v.caret);
}

/*
 * caret_ensure_text — make the caret sit in text: beside an object (an
 * image, or a table as a whole — NOT inside a cell, which is text), open a
 * new paragraph after (or before) the object and move the caret into it.
 * The caret's TEXT is tested, not its block: a table block has no text of
 * its own, and testing it sent typing aimed at a cell out of the table.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
caret_ensure_text(NotesNoteView *v)
{
    if (caret_text(v) != NULL)
        return;
    guint at = v.caret.block + (v.caret.offset > 0 ? 1 : 0);
    on_document_insert_block(v.doc, at, on_block_new_text(ON_BLOCK_PARA));
    OnPos p = { at, -1, 0 };
    v.caret = v.anchor = p;
}

/* ── Layout width ────────────────────────────────────────────────────────── */

/*
 * layout_width — the width the layout lays the page out in: the view's
 * whole width (the layout keeps the page margins itself, and its
 * coordinates are the view's).
 *
 * Inputs:
 *   v — the view
 *
 * Output: points.
 */
static double
layout_width(NotesNoteView *v)
{
    return MAX(v.frame.size.width, 1.0);
}

/* ── Style helpers ───────────────────────────────────────────────────────── */

/*
 * current_style — the layout style the settings and the appearance ask
 * for (GTK: style_from_app).
 *
 * Inputs:
 *   v — the view
 *
 * Output: the style.
 */
static OnDocLayoutStyle
current_style(NotesNoteView *v)
{
    OnDocLayoutStyle s = {
        .title_line   = v.app->first_line_title,
        .code_numbers = v.app->code_line_numbers,
        .code_copy    = v.app->code_copy_buttons,
        .dark         = shared_ui_is_dark(v),
    };
    return s;
}

/*
 * fit_height — make the view as tall as the laid-out note (the width
 * stays: it follows the window).
 *
 * Inputs:
 *   v — the view
 */
static void
fit_height(NotesNoteView *v)
{
    NSSize sz = v.frame.size;
    sz.height = MAX(on_doc_layout_height(v.layout), 1.0);
    [v setFrameSize:sz];
    v.needsDisplay = YES;
}

/*
 * apply_style — give the layout the current style and fit the view's
 * height to it (a title line or code numbers change the geometry).
 *
 * Inputs:
 *   v — the view
 */
static void
apply_style(NotesNoteView *v)
{
    OnDocLayoutStyle s = current_style(v);
    on_doc_layout_set_style(v.layout, &s);
    on_doc_layout_set_width(v.layout, layout_width(v));
    fit_height(v);
}

/* ── After-edit bookkeeping ─────────────────────────────────────────────── */

/*
 * scroll_to_caret — scroll the enclosing scroll view so the caret is
 * visible.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
scroll_to_caret(NotesNoteView *v)
{
    on_rect_t r;
    on_doc_layout_caret_rect(v.layout, v.caret, &r);
    /* Add a small margin so the caret is not right at the edge. */
    NSRect visible = NSMakeRect(r.x - 4, r.y - 4,
                                r.w + 8, r.h + 8);
    [v scrollRectToVisible:visible];
}

/*
 * blink_restart — show the caret now and, while the view has the focus,
 * blink it from the "on" state (without the focus there is no caret and
 * no timer).
 *
 * Inputs:
 *   v — the view
 */
static void
blink_restart(NotesNoteView *v)
{
    [v.blink_timer invalidate];
    v.blink_timer = nil;
    v.caret_on    = YES;
    v.needsDisplay = YES;
    if (v.window.firstResponder != v) return;      /* blinks only with the focus */
    __weak NotesNoteView *wv = v;
    v.blink_timer = [NSTimer scheduledTimerWithTimeInterval:blink_interval
                                                    repeats:YES
                                                      block:^(NSTimer *t) {
        NotesNoteView *sv = wv;
        if (!sv) { [t invalidate]; return; }
        sv.caret_on = !sv.caret_on;
        on_rect_t r;                  /* a blink redraws the caret's line only */
        on_doc_layout_caret_rect(sv.layout, sv.caret, &r);
        [sv setNeedsDisplayInRect:NSInsetRect(NSMakeRect(r.x, r.y, r.w, r.h), -2, -2)];
    }];
    v.needsDisplay = YES;
}

/*
 * im_sync_location — the caret moved: the input method asks again where
 * it is (firstRectForCharacterRange:) for its candidate window.
 *
 * Inputs:
 *   v — the view
 */
static void
im_sync_location(NotesNoteView *v)
{
    [v.inputContext invalidateCharacterCoordinates];
}

/*
 * im_reset — the caret is being moved by something other than the input
 * method (a click, a paste, a navigation key): its marked text goes.
 *
 * Inputs:
 *   v — the view
 */
static void
im_reset(NotesNoteView *v)
{
    if (!v.preedit) return;
    v.preedit = nil;
    on_doc_layout_set_preedit(v.layout, v.caret, NULL, 0);
    [v.inputContext discardMarkedText];
}


static OnDocHit *compute_hits(const OnDocument *doc, NSString *needle, size_t *n_out);

/*
 * refresh_hits — recompute the find highlights for the current find text
 * (after an edit or a load), without moving the selection.
 *
 * Inputs:
 *   v — the view
 */
static void
refresh_hits(NotesNoteView *v)
{
    size_t n = 0;
    OnDocHit *hits = compute_hits(v.doc, v.find_text, &n);
    on_doc_layout_set_hits(v.layout, hits, n);
    free(hits);
}

/* ── Accessibility text ──────────────────────────────────────────────────── */

/*
 * access_text — what VoiceOver reads for one block: its text; a table's
 * cells with a tab between cells and a newline between rows; an image
 * block as one object character.
 *
 * Inputs:
 *   b — the block
 *
 * Output: the text.
 */
static NSString *
access_text(const OnBlock *b)
{
    if (b->kind == ON_BLOCK_IMAGE) return @"\uFFFC";
    if (b->kind != ON_BLOCK_TABLE) return @(b->text->text->str);
    NSMutableString *s = [NSMutableString string];
    for (gint i = 0; i < b->rows * b->cols; i++) {
        if (i > 0) [s appendString:i % b->cols == 0 ? @"\n" : @"\t"];
        [s appendString:@(((const OnText *)g_ptr_array_index(b->cells, (guint)i))->text->str)];
    }
    return s;
}

/*
 * access_offset — a document position as an offset into the accessible
 * text (the blocks' access_text joined by newlines), in UTF-16 units.
 *
 * Inputs:
 *   doc — the document
 *   p   — the position
 *
 * Output: the offset.
 */
static NSUInteger
access_offset(const OnDocument *doc, OnPos p)
{
    NSUInteger at = 0;
    for (guint i = 0; i < p.block; i++) at += access_text(on_document_block(doc, i)).length + 1;
    const OnBlock *b = on_document_block(doc, p.block);
    if (b->kind == ON_BLOCK_IMAGE) return at + MIN(p.offset, 1);
    if (b->kind != ON_BLOCK_TABLE) return at + on_utf16_from_utf8(b->text->text->str, p.offset);
    gint cell = MAX(p.cell, 0);
    for (gint i = 0; i < cell; i++)
        at += @(((const OnText *)g_ptr_array_index(b->cells, (guint)i))->text->str).length + 1;
    const OnText *t = g_ptr_array_index(b->cells, (guint)cell);
    return at + (p.cell >= 0 ? on_utf16_from_utf8(t->text->str, p.offset) : 0);
}

/*
 * edit_settled — the bookkeeping after a mutation: clamp the caret and
 * anchor, resize the view, scroll to the caret when asked, fire the edited
 * handler, reset the blink.
 *
 * Inputs:
 *   v      — the view
 *   scroll — YES to bring the caret into view (the user's own edits); NO
 *            to leave the scroll position alone (an edit from outside)
 *
 * Output: none.
 */
static void
edit_settled(NotesNoteView *v, BOOL scroll)
{
    /* Every position the view holds is valid after an edit: the layout
     * reads text at the caret's byte offset with no other bound.          */
    v.caret  = clamp_pos(v.doc, v.caret);
    v.anchor = clamp_pos(v.doc, v.anchor);
    on_doc_layout_set_width(v.layout, layout_width(v));
    refresh_hits(v);                    /* matches move with the edit */
    fit_height(v);
    if (scroll) scroll_to_caret(v);
    im_sync_location(v);
    blink_restart(v);
    NSAccessibilityPostNotification(v, NSAccessibilityValueChangedNotification);
    if (v.edited_handler) v.edited_handler();
}

/*
 * after_edit — call after every mutation the user made: edit_settled,
 * scrolling the caret into view.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
after_edit(NotesNoteView *v)
{
    edit_settled(v, YES);
}

/* ── Undo group ──────────────────────────────────────────────────────────── */

/*
 * typing_begin — open or extend the typing undo group; restart its timer.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
typing_begin(NotesNoteView *v)
{
    [v.typing_timer invalidate];
    if (!v.typing) {
        on_document_begin_group(v.doc);
        v.typing = YES;
        v.sentences = 0;
    }
    __weak NotesNoteView *wv = v;
    v.typing_timer = [NSTimer scheduledTimerWithTimeInterval:typing_undo_ms
                                                     repeats:NO
                                                       block:^(NSTimer *t) {
        (void)t;
        NotesNoteView *sv = wv;
        if (sv && sv.typing) {
            on_document_end_group(sv.doc);
            sv.typing = NO;
        }
    }];
}

/*
 * typing_end — close the open typing group if any.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
typing_end(NotesNoteView *v)
{
    [v.typing_timer invalidate];
    v.typing_timer = nil;
    if (v.typing) {
        on_document_end_group(v.doc);
        v.typing = NO;
    }
}

/* ── Selection management ────────────────────────────────────────────────── */

/*
 * set_caret — move the caret (clamped into the document), and the anchor
 * too unless extending; the caret is scrolled into view, blinks from "on"
 * and the input method is told where it is (GTK: set_caret).
 *
 * Inputs:
 *   v      — the view
 *   p      — the new caret position
 *   extend — YES to extend the selection, NO to collapse it
 */
static void
set_caret(NotesNoteView *v, OnPos p, BOOL extend)
{
    v.caret = clamp_pos(v.doc, p);
    if (!extend)
        v.anchor = v.caret;
    blink_restart(v);
    scroll_to_caret(v);
    im_sync_location(v);
    NSAccessibilityPostNotification(v, NSAccessibilitySelectedTextChangedNotification);
}

/*
 * adopt_flags — take the inline flags from the character before the caret.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
adopt_flags(NotesNoteView *v)
{
    OnText *t = caret_text(v);
    guint32 flags = 0;
    if (t != NULL && t->text->len > 0) {
        gsize off = v.caret.offset > 0 ? v.caret.offset - 1 : 0;
        flags = on_text_flags_at(t, off) & ON_FMT_INLINE_MASK;
    }
    if (flags != v.inline_flags) {
        v.inline_flags = flags;
        if (v.inline_flags_handler) v.inline_flags_handler();
    }
}

/*
 * select_all — select the entire document.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
select_all(NotesNoteView *v)
{
    double gx    = -1;
    OnPos  start = { 0, -1, 0 };
    v.anchor = on_doc_layout_move(v.layout, start, ON_MOVE_DOC_START, &gx);
    v.caret  = on_doc_layout_move(v.layout, start, ON_MOVE_DOC_END, &gx);
    adopt_flags(v);
    v.needsDisplay = YES;
}

/*
 * delete_selection — delete the selected range (a no-op when empty).
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
delete_selection(NotesNoteView *v)
{
    if (!has_selection(v)) return;
    OnPos a, b, out;
    sel_bounds(v, &a, &b);
    on_document_delete_range(v.doc, a, b, &out);
    v.caret = v.anchor = clamp_pos(v.doc, out);
}

/* ── Editing primitives ──────────────────────────────────────────────────── */

/*
 * tag_line_text — what typing `n` bytes at the caret puts on the tag line:
 * spaces, '#' at a word's start and tag characters (on_document_tag_char),
 * a word begun with a tag character getting its '#'; anything else is
 * dropped.
 *
 * Inputs:
 *   v    — the view, its caret on the tag line
 *   text — UTF-8 bytes
 *   n    — byte count
 *
 * Output: a new GString; g_string_free it.
 */
static GString *
tag_line_text(NotesNoteView *v, const char *text, size_t n)
{
    OnText  *t    = caret_text(v);
    gunichar prev = v.caret.offset > 0                  /* the char before  */
        ? g_utf8_get_char(g_utf8_prev_char(t->text->str + v.caret.offset)) : ' ';
    GString *out  = g_string_new(NULL);
    for (const gchar *p = text; p < text + n; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c == ' ' || (c == '#' && prev == ' ')) {
            g_string_append_c(out, (gchar)c);
        } else if (on_document_tag_char(c)) {
            if (prev == ' ') g_string_append_c(out, '#');
            g_string_append_unichar(out, c);
        } else {
            continue;
        }
        prev = c;
    }
    return out;
}

/*
 * insert_typed — typed text at the caret: replaces a selection, carries
 * the armed inline flags, extends the typing group.  On the tag line only
 * tags can be typed (tag_line_text).
 *
 * Inputs:
 *   v    — the view
 *   text — UTF-8 bytes
 *   n    — byte count
 *
 * Output: none.
 */
static void
insert_typed(NotesNoteView *v, const char *text, size_t n)
{
    if (n == 0) return;
    typing_begin(v);
    delete_selection(v);
    caret_ensure_text(v);
    GString *line = caret_block(v)->kind == ON_BLOCK_TAGS ? tag_line_text(v, text, n) : NULL;
    if (line != NULL) {
        text = line->str;
        n    = line->len;
    }
    OnPos p = v.caret;
    guint32 flags = v.inline_flags;
    /* Extend a #tag span if typing inside one. */
    OnText *t = caret_text(v);
    if (t != NULL && p.offset > 0 && p.offset <= t->text->len &&
        (on_text_flags_at(t, p.offset - 1) & ON_FMT_TAG) &&
        (p.offset == t->text->len || (on_text_flags_at(t, p.offset) & ON_FMT_TAG)))
        flags |= ON_FMT_TAG;
    if (n > 0 && on_document_insert_text(v.doc, p, text, n, flags)) {
        OnPos c = v.caret;
        c.offset += n;
        v.caret = c;
        tag_capture_consider(v, text, n);
        /* Sentence enders cap a typing group. */
        if (memchr(text, '.', n) || memchr(text, '?', n))
            if (++v.sentences >= max_sentences) typing_end(v);
    }
    if (line != NULL) g_string_free(line, TRUE);
    v.anchor = v.caret;
    after_edit(v);
}

/*
 * enter_pressed — Enter: list continuation and block splitting.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
enter_pressed(NotesNoteView *v)
{
    tag_capture_end(v, YES);
    typing_end(v);
    on_document_begin_group(v.doc);
    delete_selection(v);
    const OnBlock *b = caret_block(v);
    OnPos p = v.caret;
    if (p.cell >= 0) {
        /* Inside a table cell: line break. */
        on_document_insert_text(v.doc, p, "\n", 1, 0);
        OnPos c = p;
        c.offset++;
        v.caret = v.anchor = c;
    } else if (b->text == NULL) {
        /* Beside an object block. */
        guint at = p.block + (p.offset > 0 ? 1 : 0);
        on_document_insert_block(v.doc, at, on_block_new_text(ON_BLOCK_PARA));
        OnPos np = { at, -1, 0 };
        v.caret = v.anchor = np;
    } else {
        if (on_block_kind_is_list(b->kind) && b->text->text->len == 0) {
            /* Enter on empty item ends the list. */
            on_document_set_kind(v.doc, p.block, ON_BLOCK_PARA);
        } else {
            OnBlockKind kind = b->kind;
            on_document_split_block(v.doc, p, v.inline_flags);
            OnPos np = { p.block + 1, -1, 0 };
            v.caret = v.anchor = np;
            if (kind == ON_BLOCK_H1 || kind == ON_BLOCK_H2)
                on_document_set_kind(v.doc, np.block, ON_BLOCK_PARA);
        }
    }
    on_document_end_group(v.doc);
    after_edit(v);
}

/*
 * delete_key — a delete key: the selection if there is one, else from the
 * caret to where `how` would move it — a character (Backspace, Delete), a
 * word (⌥⌫, ⌥⌦), the line start (⌘⌫) or the block end (⌃K) — never
 * across a block or cell.  At a block's edge it joins blocks, unlists a
 * list item or removes an object, as Backspace and Delete do.
 *
 * Inputs:
 *   v   — the view
 *   how — ON_MOVE_CHAR_LEFT/RIGHT, WORD_LEFT/RIGHT, LINE_START or
 *         BLOCK_END
 */
static void
delete_key(NotesNoteView *v, OnDocMove how)
{
    BOOL backward = how == ON_MOVE_CHAR_LEFT || how == ON_MOVE_WORD_LEFT ||
                    how == ON_MOVE_LINE_START;
    if (!backward) tag_capture_end(v, YES);
    if (has_selection(v)) {
        typing_end(v);
        delete_selection(v);
        after_edit(v);
        return;
    }
    const OnBlock *b = caret_block(v);
    OnPos p = v.caret;
    typing_begin(v);

    if (b->text == NULL && p.cell < 0) {
        /* Beside an object: remove it when the key points at it. */
        if ((backward && p.offset == 1) || (!backward && p.offset == 0)) {
            OnPos a = { p.block, -1, 0 }, e = { p.block, -1, 1 }, out;
            on_document_delete_range(v.doc, a, e, &out);
            v.caret = v.anchor = clamp_pos(v.doc, out);
        } else {
            double gx = -1;
            OnPos q = on_doc_layout_move(v.layout, p, how, &gx);
            if (!pos_eq(q, p)) {
                OnPos out;
                on_document_delete_range(v.doc,
                                        backward ? q : p,
                                        backward ? p : q, &out);
                v.caret = v.anchor = clamp_pos(v.doc, out);
            }
        }
        after_edit(v);
        return;
    }

    OnText *t = caret_text(v);
    if (backward && p.offset == 0 && p.cell < 0) {
        if (on_block_kind_is_list(b->kind)) {
            on_document_set_kind(v.doc, p.block, ON_BLOCK_PARA);
        } else if (p.block > 0) {
            const OnBlock *prev = on_document_block(v.doc, p.block - 1);
            if (prev->text != NULL) {
                gsize len = prev->text->text->len;
                on_document_join_blocks(v.doc, p.block - 1);
                OnPos np = { p.block - 1, -1, len };
                v.caret = v.anchor = np;
            } else if (t != NULL && t->text->len == 0) {
                on_document_remove_block(v.doc, p.block);
                OnPos np = { p.block - 1, -1, 1 };
                v.caret = v.anchor = np;
            } else {
                OnPos np = { p.block - 1, -1, 1 };
                v.caret = v.anchor = np;
            }
        }
        after_edit(v);
        return;
    }
    if (!backward && t != NULL && p.offset >= t->text->len && p.cell < 0) {
        if (p.block + 1 < on_document_n_blocks(v.doc)) {
            const OnBlock *next = on_document_block(v.doc, p.block + 1);
            if (next->text != NULL) {
                on_document_join_blocks(v.doc, p.block);
            } else if (t->text->len == 0) {
                on_document_remove_block(v.doc, p.block);
                OnPos np = { p.block, -1, 0 };
                v.caret = v.anchor = np;
            } else {
                OnPos np = { p.block + 1, -1, 0 };
                v.caret = v.anchor = np;
            }
        }
        after_edit(v);
        return;
    }

    double gx = -1;
    OnPos q = on_doc_layout_move(v.layout, p, how, &gx);
    /* Never cross a block or cell boundary. */
    if (q.block != p.block || q.cell != p.cell) q = p;
    OnPos a = backward ? q : p, e = backward ? p : q;
    if (e.offset > a.offset)
        on_document_delete_text(v.doc, a, e.offset - a.offset);
    v.caret = v.anchor = a;
    tag_popup_update(v);                /* the word shrank, or the '#' went */
    after_edit(v);
}

/*
 * move_caret — a navigation key: extend when extend is YES.
 *
 * Inputs:
 *   v      — the view
 *   how    — the movement
 *   extend — YES when Shift is held
 *
 * Output: none.
 */
static void
move_caret(NotesNoteView *v, OnDocMove how, BOOL extend)
{
    if (how != ON_MOVE_LINE_UP && how != ON_MOVE_LINE_DOWN) tag_capture_end(v, YES);
    im_reset(v);
    typing_end(v);
    /* Left/Right with a selection and no Shift collapses to its edge. */
    if (!extend && has_selection(v) &&
        (how == ON_MOVE_CHAR_LEFT || how == ON_MOVE_CHAR_RIGHT)) {
        OnPos a, b;
        sel_bounds(v, &a, &b);
        v.goal_x = -1;
        set_caret(v, how == ON_MOVE_CHAR_LEFT ? a : b, NO);
        adopt_flags(v);
        return;
    }
    double gx = v.goal_x;
    OnPos p = on_doc_layout_move(v.layout, v.caret, how, &gx);
    v.goal_x = gx;
    /* Down off the last code line at the end of the doc: open a body line. */
    if (how == ON_MOVE_LINE_DOWN && !extend && pos_eq(p, v.caret) &&
        caret_block(v)->kind == ON_BLOCK_CODE &&
        v.caret.block + 1 == on_document_n_blocks(v.doc)) {
        guint at = v.caret.block + 1;
        on_document_insert_block(v.doc, at, on_block_new_text(ON_BLOCK_PARA));
        OnPos np = { at, -1, 0 };
        v.caret = v.anchor = np;
        v.goal_x = -1;
        after_edit(v);
        return;
    }
    set_caret(v, p, extend);
    adopt_flags(v);
}

/*
 * page_move — Page Up or Page Down: the caret moves a page's height, in
 * its column, and the view scrolls by the same page (GTK: page_move).
 *
 * Inputs:
 *   v      — the view
 *   down   — YES for Page Down
 *   extend — YES when Shift is held
 */
static void
page_move(NotesNoteView *v, BOOL down, BOOL extend)
{
    NSScrollView *sv   = v.enclosingScrollView;
    NSRect        seen = sv ? sv.documentVisibleRect : NSMakeRect(0, 0, 0, 400.0);
    double        page = seen.size.height;
    on_rect_t r;
    on_doc_layout_caret_rect(v.layout, v.caret, &r);
    double target_y = r.y + (down ? page : -page);
    if (v.goal_x < 0)
        v.goal_x = r.x;
    OnDocHitResult hit;
    on_doc_layout_hit(v.layout, v.goal_x, MAX(0, target_y), &hit);
    double gx = v.goal_x;
    if (sv) {
        double top = MAX(0.0, MIN(seen.origin.y + (down ? page : -page),
                                  v.frame.size.height - page));
        [sv.contentView scrollToPoint:NSMakePoint(seen.origin.x, top)];
        [sv reflectScrolledClipView:sv.contentView];
    }
    set_caret(v, hit.pos, extend);
    v.goal_x = gx;
}

/*
 * tab_pressed — Tab moves between table cells; elsewhere inserts a tab.
 *
 * Inputs:
 *   v        — the view
 *   backward — YES for Shift+Tab
 *
 * Output: none.
 */
static void
tab_pressed(NotesNoteView *v, BOOL backward)
{
    const OnBlock *b = caret_block(v);
    if (b->kind == ON_BLOCK_TABLE && v.caret.cell >= 0) {
        gint n = b->rows * b->cols;
        gint cell = v.caret.cell + (backward ? -1 : 1);
        if (cell < 0 || cell >= n) return;
        OnPos p = { v.caret.block, cell, 0 };
        OnText *t = on_document_text_at(v.doc, p);
        v.anchor = p;
        p.offset = t ? t->text->len : 0;
        set_caret(v, p, YES);
        return;
    }
    if (!backward)
        insert_typed(v, "\t", 1);
}

/* ── Insert block ────────────────────────────────────────────────────────── */

/*
 * insert_block_at_caret — insert an object block at the caret position;
 * on the tag line, which holds only tags, nothing is inserted.
 *
 * Inputs:
 *   v     — the view
 *   block — the block (ownership transferred)
 *
 * Output: none.
 */
static void
insert_block_at_caret(NotesNoteView *v, OnBlock *block)
{
    if (caret_block(v)->kind == ON_BLOCK_TAGS) {
        on_block_free(block);
        return;
    }
    typing_end(v);
    on_document_begin_group(v.doc);
    delete_selection(v);
    OnPos out = v.caret;
    if (caret_text(v) == NULL || v.caret.cell >= 0) {
        guint at = v.caret.block + (v.caret.offset > 0 ? 1 : 0);
        if (v.caret.cell >= 0) at = v.caret.block + 1;
        on_document_insert_block(v.doc, at, block);
        out = (OnPos){ at, -1, 1 };
    } else {
        OnDocument *frag = on_document_new_from_block(block);
        on_document_insert_fragment(v.doc, v.caret, frag, &out);
        on_document_free(frag);
    }
    on_document_end_group(v.doc);
    v.caret = v.anchor = out;
    after_edit(v);
}

/* ── Clipboard ───────────────────────────────────────────────────────────── */

/*
 * file_object_url — an attached file as a temporary file with its own name,
 * streamed out of the database.
 *
 * Inputs:
 *   v   — the view (its database)
 *   obj — the FILE object
 *
 * Output: the file's URL, or nil when its row is missing or the file could
 *         not be written.
 */
static NSURL *
file_object_url(NotesNoteView *v, const OnInlineObject *obj)
{
    OnDatabase *db = v.app->db;
    gint64      id = obj->file_id;
    return on_ui_temp_file_from(@(obj->file_name), ^BOOL(NSString *path) {
        return on_db_note_file_export(db, id, path.fileSystemRepresentation);
    });
}

/*
 * copy_selection — copy the selected range to the pasteboard as BNBF and
 * plain text, and each attached file in it as a file.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
copy_selection(NotesNoteView *v)
{
    if (!has_selection(v)) return;
    OnPos a, b;
    sel_bounds(v, &a, &b);
    OnDocument *frag = on_document_copy_range(v.doc, a, b);
    gsize n;
    guint8 *blob = on_document_to_bnbf(frag, &n);
    gchar *text  = on_document_plain_text(frag);

    NSPasteboardItem *note = [[NSPasteboardItem alloc] init];
    [note setData:[NSData dataWithBytes:blob length:n] forType:on_pasteboard_type_fragment];
    [note setString:@(text) forType:NSPasteboardTypeString];
    NSMutableArray *items = [NSMutableArray arrayWithObject:note];
    /* An attached file also goes out as a file, for the Finder and any app
     * that takes files. */
    for (guint i = 0; i < on_document_n_blocks(frag); i++) {
        const OnText *t = on_document_block(frag, i)->text;
        for (guint k = 0; t != NULL && k < t->objects->len; k++) {
            const OnInlineObject *obj = &g_array_index(t->objects, OnInlineObject, k);
            NSURL *url = obj->kind == ON_OBJECT_FILE ? file_object_url(v, obj) : nil;
            if (url != nil) [items addObject:url];
        }
    }
    on_document_free(frag);
    g_free(blob);                       /* on_document_to_bnbf: g_free */
    g_free(text);

    [v.pasteboard clearContents];
    [v.pasteboard writeObjects:items];
}

/*
 * paste_fragment — insert a BNBF fragment at the caret.
 *
 * Inputs:
 *   v     — the view
 *   frag  — the fragment document (caller frees)
 *   plain — YES to force code blocks into code style
 *
 * Output: none.
 */
static void
paste_fragment(NotesNoteView *v, OnDocument *frag, BOOL plain)
{
    typing_end(v);
    on_document_begin_group(v.doc);
    delete_selection(v);
    caret_ensure_text(v);
    if (plain && caret_block(v)->kind == ON_BLOCK_CODE)
        for (guint i = 0; i < on_document_n_blocks(frag); i++)
            on_document_set_kind(frag, i, ON_BLOCK_CODE);
    OnPos out;
    if (on_document_insert_fragment(v.doc, v.caret, frag, &out))
        v.caret = v.anchor = clamp_pos(v.doc, out);
    on_document_end_group(v.doc);
    after_edit(v);
}

/*
 * pasteboard_png — the pasteboard's image as PNG bytes: its PNG data when
 * it has some, else any image AppKit can read there (TIFF, JPEG, HEIC …)
 * encoded once to PNG, the one format a note stores.
 *
 * Inputs:
 *   pb — the pasteboard
 *
 * Output: the bytes (g_bytes_unref), or NULL when there is no image.
 */
static GBytes *
pasteboard_png(NSPasteboard *pb)
{
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (png) return g_bytes_new(png.bytes, png.length);
    NSImage *image = [pb readObjectsForClasses:@[NSImage.class] options:nil].firstObject;
    CGImageRef cg  = [image CGImageForProposedRect:NULL context:nil hints:nil];
    size_t     n   = 0;
    uint8_t   *buf = cg ? on_image_png_encode(cg, &n) : NULL;
    if (buf == NULL) return NULL;
    GBytes *bytes = g_bytes_new(buf, n);
    free(buf);
    return bytes;
}

/*
 * pasteboard_file_urls — the files a pasteboard carries (a copy or a drag
 * from the Finder).
 *
 * Inputs:
 *   pb — the pasteboard
 *
 * Output: their file URLs; empty when it carries none.
 */
static NSArray<NSURL *> *
pasteboard_file_urls(NSPasteboard *pb)
{
    NSArray *urls = [pb readObjectsForClasses:@[NSURL.class]
                                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    return urls ?: @[];
}

/*
 * file_image_block — the standard image block for a picture ImageIO reads
 * (stored as PNG, once, in the note).
 *
 * Inputs:
 *   url — the file
 *
 * Output: the block (ownership passes to the caller), or NULL when the file
 *         is not a picture ImageIO can read.
 */
static OnBlock *
file_image_block(NSURL *url)
{
    UTType *type = [UTType typeWithFilenameExtension:url.pathExtension];
    if (type == nil || ![type conformsToType:UTTypeImage]) return NULL;
    size_t   n   = 0;
    uint8_t *png = on_image_file_to_png(url.fileSystemRepresentation, &n);
    if (png == NULL) return NULL;
    GBytes *bytes = g_bytes_new(png, n);
    free(png);
    OnBlock *block = on_block_new_image(bytes, 0);
    g_bytes_unref(bytes);
    return block;
}

/*
 * file_store — store a file in a note_file row of the note (streamed, at
 * once).
 *
 * Inputs:
 *   v      — the view (its database and note)
 *   url    — the file
 *   reason — receives why it was not stored (a short phrase)
 *
 * Output: the row's id, or 0 with *reason set: a folder, an unreadable file,
 *         one over ON_FILE_MAX_BYTES, a view with no note.
 */
static gint64
file_store(NotesNoteView *v, NSURL *url, NSString **reason)
{
    NSNumber *is_dir = nil, *size = nil;
    [url getResourceValue:&is_dir forKey:NSURLIsDirectoryKey error:NULL];
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:NULL];
    if (is_dir.boolValue) {
        *reason = @"a folder cannot be attached";
        return 0;
    }
    if (size == nil) {
        *reason = @"it could not be read";
        return 0;
    }
    if (size.unsignedLongLongValue > ON_FILE_MAX_BYTES) {
        *reason = [NSString stringWithFormat:@"it is over %d MB", ON_FILE_MAX_BYTES / 1000000];
        return 0;
    }
    if (v.note_id == 0) {
        *reason = @"this note cannot take files";
        return 0;
    }
    gint64 id = on_db_note_file_add(v.app->db, v.note_id, url.fileSystemRepresentation,
                                    url.lastPathComponent.UTF8String);
    if (id == 0) *reason = @"it could not be stored";
    return id;
}

/*
 * insert_files — put files into the note at the caret as one undo step.  A
 * picture becomes an image block; every other file becomes a tile in the
 * line, side by side with the tiles dropped with it (a run of files is one
 * insertion).  The ones that cannot be attached are named in a notice.
 *
 * Inputs:
 *   v    — the view
 *   urls — the files, in order
 *
 * Output: none.
 */
static void
insert_files(NotesNoteView *v, NSArray<NSURL *> *urls)
{
    NSMutableArray<NSString *> *refused = [NSMutableArray array];
    OnBlock *tiles = NULL;               /* the run of files being gathered  */
    on_document_begin_group(v.doc);
    for (NSURL *url in urls) {
        OnBlock *image = file_image_block(url);
        if (image != NULL) {
            if (tiles != NULL) insert_block_at_caret(v, tiles);
            tiles = NULL;
            insert_block_at_caret(v, image);
            continue;
        }
        NSString *reason = nil;
        gint64    id     = file_store(v, url, &reason);
        if (id == 0) {
            [refused addObject:[NSString stringWithFormat:@"%@ — %@", url.lastPathComponent, reason]];
            continue;
        }
        if (tiles == NULL) tiles = on_block_new_text(ON_BLOCK_PARA);
        gsize at = tiles->text->text->len;
        on_text_append(tiles->text, ON_OBJ_CHAR, ON_OBJ_CHAR_LEN, 0);
        on_text_add_file(tiles->text, at, id, url.lastPathComponent.UTF8String);
    }
    if (tiles != NULL) insert_block_at_caret(v, tiles);
    on_document_end_group(v.doc);
    if (refused.count > 0)
        shared_ui_notice(v.window, @"Not attached",
                         [refused componentsJoinedByString:@"\n"], nil);
}

/*
 * select_object — select one inline object (an attached file's tile).
 *
 * Inputs:
 *   v      — the view
 *   block  — its block
 *   offset — the byte offset of its U+FFFC
 *
 * Output: none.
 */
static void
select_object(NotesNoteView *v, guint block, gsize offset)
{
    set_caret(v, (OnPos){ block, -1, offset + ON_OBJ_CHAR_LEN }, NO);
    v.anchor = (OnPos){ block, -1, offset };
}

/*
 * open_file_object — open an attached file in its default app.
 *
 * Inputs:
 *   v      — the view
 *   block  — the block of the line it is in
 *   object — its index among that block's inline objects
 *
 * Output: none.
 */
static void
open_file_object(NotesNoteView *v, guint block, gint object)
{
    const OnBlock *b = on_document_block(v.doc, block);
    if (b == NULL || b->text == NULL || object < 0 || (guint)object >= b->text->objects->len)
        return;
    const OnInlineObject *obj = &g_array_index(b->text->objects, OnInlineObject, object);
    if (obj->kind != ON_OBJECT_FILE) return;
    NSURL *url = file_object_url(v, obj);
    if (url == nil)
        shared_ui_notice(v.window, @"Cannot open the file",
                         [NSString stringWithFormat:@"%@ is missing from the database.",
                                                    @(obj->file_name)], nil);
    else
        [NSWorkspace.sharedWorkspace openURL:url];
}

/*
 * copy_file_row — on_document_remap_files' map for a pasted fragment: a copy,
 * for this note, of the file row the fragment names.
 *
 * Inputs:
 *   id   — the row the fragment names
 *   data — the view (unretained)
 *
 * Output: the new row's id, or 0 when the row is gone or the view has no
 *         note.
 */
static gint64
copy_file_row(gint64 id, gpointer data)
{
    NotesNoteView *v = (__bridge NotesNoteView *)data;
    return v.note_id != 0 ? on_db_note_file_copy(v.app->db, id, v.note_id) : 0;
}

/*
 * paste — paste from the pasteboard: a note fragment (our own type) first,
 * then files, then an image, then plain text.
 *
 * Inputs:
 *   v — the view
 */
static void
paste(NotesNoteView *v)
{
    im_reset(v);
    NSPasteboard *pb = v.pasteboard;
    /* Prefer our fragment type. */
    NSArray<NSURL *> *files = pasteboard_file_urls(pb);
    NSData *frag_data = [pb dataForType:on_pasteboard_type_fragment];
    if (frag_data) {
        OnDocument *frag = on_document_from_bnbf(
            (const guint8 *)frag_data.bytes, frag_data.length, NULL);
        if (frag) {
            /* Its files are rows of the note it came from: this note gets
             * rows of its own.  A row that is gone (the source note let it
             * go) cannot come; the files copied beside the fragment can. */
            GArray *named = on_document_file_ids(frag, NO);
            on_document_remap_files(frag, copy_file_row, (__bridge void *)v);
            GArray *kept = on_document_file_ids(frag, NO);
            BOOL lost = kept->len < named->len;
            g_array_unref(named);
            g_array_unref(kept);
            if (lost && files.count > 0) {
                on_document_free(frag);
                insert_files(v, files);
                return;
            }
            paste_fragment(v, frag, NO);
            on_document_free(frag);
            return;
        }
    }
    /* Files: a picture becomes an image, anything else an attachment. */
    if (files.count > 0) {
        insert_files(v, files);
        return;
    }
    /* An image, stored as PNG. */
    GBytes *png = pasteboard_png(pb);
    if (png) {
        OnDocument *frag = on_document_new_from_block(on_block_new_image(png, 0));
        g_bytes_unref(png);
        paste_fragment(v, frag, NO);
        on_document_free(frag);
        return;
    }
    /* Plain text. */
    NSString *str = [pb stringForType:NSPasteboardTypeString];
    if (str) {
        OnDocument *frag = on_document_from_text(str.UTF8String);
        if (frag) {
            paste_fragment(v, frag, YES);
            on_document_free(frag);
        }
    }
}

/* ── find hits ───────────────────────────────────────────────────────────── */

/*
 * find_in_text — every case-insensitive match of a needle in one text (a
 * paragraph or a table cell), appended as OnDocHit (GTK: find_in_text).
 * Characters compare lowercased, whole Unicode, not only ASCII.
 *
 * Inputs:
 *   t      — the text
 *   block  — its block
 *   cell   — its cell, or -1
 *   needle — what to find (UTF-8, not empty)
 *   hits   — the OnDocHit records to append to
 */
static void
find_in_text(const OnText *t, guint block, gint cell, const char *needle, NSMutableData *hits)
{
    glong     nlen   = g_utf8_strlen(needle, -1);
    gunichar *nchars = g_utf8_to_ucs4_fast(needle, -1, NULL);
    for (glong k = 0; k < nlen; k++) nchars[k] = g_unichar_tolower(nchars[k]);
    for (const gchar *p = t->text->str; *p != '\0'; p = g_utf8_next_char(p)) {
        const gchar *q = p;
        glong k = 0;
        for (; k < nlen && *q != '\0'; k++, q = g_utf8_next_char(q))
            if (g_unichar_tolower(g_utf8_get_char(q)) != nchars[k]) break;
        if (k == nlen) {
            OnDocHit h = { .block = block, .cell = cell,
                           .start = (gsize)(p - t->text->str), .len = (gsize)(q - p) };
            [hits appendBytes:&h length:sizeof h];
        }
    }
    g_free(nchars);
}

/*
 * compute_hits — every match in the document, paragraphs and table cells
 * alike, in document order.
 *
 * Inputs:
 *   doc    — the document
 *   needle — what to find; nil or empty finds nothing
 *   n_out  — receives the count
 *
 * Output: the hits (free()), or NULL for none.
 */
static OnDocHit *
compute_hits(const OnDocument *doc, NSString *needle, size_t *n_out)
{
    *n_out = 0;
    const char *n8 = needle.UTF8String;
    if (n8 == NULL || *n8 == '\0') return NULL;
    NSMutableData *hits = [NSMutableData data];
    for (guint i = 0; i < on_document_n_blocks(doc); i++) {
        const OnBlock *b = on_document_block(doc, i);
        if (b->text != NULL) {
            find_in_text(b->text, i, -1, n8, hits);
        } else if (b->kind == ON_BLOCK_TABLE) {
            for (guint c = 0; c < b->cells->len; c++)
                find_in_text(g_ptr_array_index(b->cells, c), i, (gint)c, n8, hits);
        }
    }
    *n_out = hits.length / sizeof(OnDocHit);
    if (*n_out == 0) return NULL;
    OnDocHit *out = malloc(hits.length);
    memcpy(out, hits.bytes, hits.length);
    return out;
}

/* ── #tag capture and the suggestion list ───────────────────────────────── */

/*
 * Typing '#' at a word boundary starts a capture: where the '#' is is
 * remembered, the known tags are fetched once, and the suggestion list
 * shows the ones that start with what has been typed since.  A space,
 * Return, a click elsewhere, a caret move or the focus leaving END the
 * capture and style the "#word" as a tag; Escape ends it without.  While
 * the list shows, Up/Down move in it and Return/Tab choose (GTK: the same).
 * A tag set on a body line moves to the tag line (tag_lift).
 */

/*
 * tag_span — the capture's "#word" span.
 *
 * Inputs:
 *   v     — the view
 *   start — receives the '#'
 *   end   — receives the end of the word
 *
 * Output: NO when the text at the capture no longer starts with '#'.
 */
static BOOL
tag_span(NotesNoteView *v, OnPos *start, OnPos *end)
{
    OnText *t = on_document_text_at(v.doc, v.tag_start);
    if (t == NULL || v.tag_start.offset >= t->text->len || t->text->str[v.tag_start.offset] != '#')
        return NO;
    *start = v.tag_start;
    *end   = *start;
    const gchar *p = t->text->str + start->offset + 1;
    while (*p != '\0' && on_document_tag_char(g_utf8_get_char(p))) p = g_utf8_next_char(p);
    end->offset = (gsize)(p - t->text->str);
    return YES;
}

/*
 * tag_lift — a #tag was just set at `at`: on a body line it moves to the
 * tag line (on_document_lift_tag), the caret with it; on the tag line it
 * stays.
 *
 * Inputs:
 *   v  — the view
 *   at — a position inside the tag
 */
static void
tag_lift(NotesNoteView *v, OnPos at)
{
    if (at.block == 0 || on_document_title_block(v.doc) == 0) return;
    BOOL  collapsed = pos_eq(v.caret, v.anchor);
    OnPos caret     = v.caret;
    if (!on_document_lift_tag(v.doc, at, &caret)) return;
    v.caret  = caret;
    v.anchor = collapsed ? caret : clamp_pos(v.doc, v.anchor);
}

/*
 * tag_capture_end — end a capture: with `apply`, a "#word" of at least one
 * letter is styled as a tag, and on a body line moved to the tag line.
 *
 * Inputs:
 *   v     — the view
 *   apply — NO for Escape (the text stays plain)
 */
static void
tag_capture_end(NotesNoteView *v, BOOL apply)
{
    if (!v.tag_capturing) return;
    OnPos s, e;
    if (apply && tag_span(v, &s, &e) && e.offset - s.offset >= 2) {
        on_document_set_flags(v.doc, s, e.offset - s.offset, ON_FMT_TAG, TRUE);
        tag_lift(v, s);
    }
    v.tag_capturing = NO;
    on_db_tag_list_free(v.tag_choices);
    v.tag_choices = NULL;
    [v.tag_popup hide];
    if (apply) {
        v.needsDisplay = YES;
        if (v.edited_handler) v.edited_handler();
    }
}

/*
 * tag_popup_update — show the known tags (the two apps share them) that
 * start, case-insensitively, with what follows the '#', under the '#';
 * end the capture if the '#' is gone.  Each is offered as its #word
 * (on_document_tag_word: Tasks' "Old Projects" is OldProjects), once.
 *
 * Inputs:
 *   v — the view
 */
static void
tag_popup_update(NotesNoteView *v)
{
    if (!v.tag_capturing) return;
    OnPos s, e;
    if (!tag_span(v, &s, &e)) {
        tag_capture_end(v, NO);
        return;
    }
    OnText   *t     = on_document_text_at(v.doc, s);
    NSString *typed = [[NSString alloc] initWithBytes:t->text->str + s.offset + 1
                                               length:e.offset - s.offset - 1
                                             encoding:NSUTF8StringEncoding] ?: @"";
    /* The names that start with the typed text first, then those that
     * contain it (as Tasks' tag field offers them), at most
     * shared_look_tag_suggestions_max; each tag's word once. */
    NSMutableArray<NSString *> *starts = [NSMutableArray array];
    NSMutableArray<NSString *> *inside = [NSMutableArray array];
    for (GList *l = v.tag_choices; l != NULL; l = l->next) {
        gchar *word = on_document_tag_word(((const OnTag *)l->data)->name);
        if (word == NULL) continue;
        NSString *name = @(word);
        g_free(word);
        BOOL offered = NO;           /* another tag's word already          */
        for (NSString *have in [starts arrayByAddingObjectsFromArray:inside])
            offered = offered || [have caseInsensitiveCompare:name] == NSOrderedSame;
        if (offered) continue;
        /* An empty string is found nowhere: the bare '#' lists them all.   */
        NSRange found = typed.length == 0 ? NSMakeRange(0, 0)
                      : [name rangeOfString:typed options:NSCaseInsensitiveSearch];
        if (found.location == NSNotFound) continue;
        [found.location == 0 ? starts : inside addObject:name];
    }
    NSArray<NSString *> *names = [starts arrayByAddingObjectsFromArray:inside];
    if (names.count > (NSUInteger)shared_look_tag_suggestions_max)
        names = [names subarrayWithRange:NSMakeRange(0, (NSUInteger)shared_look_tag_suggestions_max)];
    on_rect_t r;
    on_doc_layout_caret_rect(v.layout, s, &r);
    if (v.tag_popup == nil) {
        v.tag_popup = [[SharedTagPopup alloc] init];
        __weak NotesNoteView *wv = v;
        v.tag_popup.chosen_handler = ^(NSString *name) { tag_choose(wv, name); };
    }
    [v.tag_popup show_names:names under:NSMakeRect(r.x, r.y, r.w, r.h) of:v];
}

/*
 * tag_choose — a suggestion was chosen: the partial word becomes "#name",
 * styled as a tag, followed by a space — on a body line moved to the tag
 * line (tag_lift); the capture ends.
 *
 * Inputs:
 *   v    — the view (nil does nothing)
 *   name — the tag's name, without '#'
 */
static void
tag_choose(NotesNoteView *v, NSString *name)
{
    OnPos s, e;
    if (v == nil || !v.tag_capturing || !tag_span(v, &s, &e)) {
        tag_capture_end(v, NO);
        return;
    }
    typing_end(v);
    on_document_begin_group(v.doc);
    on_document_delete_text(v.doc, s, e.offset - s.offset);
    NSString   *full = [@"#" stringByAppendingString:name];
    const char *u8   = full.UTF8String;
    on_document_insert_text(v.doc, s, u8, strlen(u8), ON_FMT_TAG);
    OnPos after = s;
    after.offset += strlen(u8);
    on_document_insert_text(v.doc, after, " ", 1, 0);
    after.offset++;
    v.caret = v.anchor = after;
    tag_lift(v, s);
    on_document_end_group(v.doc);
    v.tag_capturing = NO;
    on_db_tag_list_free(v.tag_choices);
    v.tag_choices = NULL;
    [v.tag_popup hide];
    after_edit(v);
}

/*
 * tag_capture_consider — `n` bytes of `text` were just typed and now end
 * at the caret: walk them, starting a capture on a '#' at a word boundary
 * (not in code or a table cell), ending one on a character a tag cannot
 * hold, and refreshing the list otherwise.
 *
 * Inputs:
 *   v    — the view
 *   text — the typed text (UTF-8)
 *   n    — its byte length
 */
static void
tag_capture_consider(NotesNoteView *v, const char *text, size_t n)
{
    OnText *t = caret_text(v);
    if (t == NULL || caret_block(v)->kind == ON_BLOCK_CODE || v.caret.cell >= 0) return;
    gsize start = v.caret.offset - n;
    for (const gchar *p = text; p < text + n; p = g_utf8_next_char(p)) {
        gunichar c  = g_utf8_get_char(p);
        gsize    at = start + (gsize)(p - text);
        if (v.tag_capturing) {
            if (!on_document_tag_char(c)) tag_capture_end(v, YES);
            continue;
        }
        if (c != '#') continue;
        BOOL boundary = at == 0;
        if (!boundary) {
            gunichar pc = g_utf8_get_char(g_utf8_prev_char(t->text->str + at));
            boundary = g_unichar_isspace(pc) || g_unichar_ispunct(pc);
        }
        if (!boundary) continue;
        v.tag_capturing = YES;
        OnPos at_pos = v.caret;
        at_pos.offset = at;
        v.tag_start = at_pos;
        on_db_tag_list_free(v.tag_choices);
        v.tag_choices = v.app && v.app->db ? on_db_tag_list(v.app->db, ON_TAGS_ALL) : NULL;
    }
    if (v.tag_capturing) tag_popup_update(v);
}

/*
 * tag_popup_command — the suggestion list's keys, while it shows (and
 * Escape while capturing): Up/Down move, Return/Tab choose, Escape ends
 * the capture plain.
 *
 * Inputs:
 *   v   — the view
 *   sel — the key's command
 *
 * Output: YES when the list took the key.
 */
static BOOL
tag_popup_command(NotesNoteView *v, SEL sel)
{
    if (!v.tag_capturing) return NO;
    if (sel == @selector(cancelOperation:)) {
        tag_capture_end(v, NO);
        return YES;
    }
    if (![v.tag_popup is_visible]) return NO;
    if (sel == @selector(moveDown:)) { [v.tag_popup move_selection:+1]; return YES; }
    if (sel == @selector(moveUp:))   { [v.tag_popup move_selection:-1]; return YES; }
    if (sel == @selector(insertNewline:) || sel == @selector(insertTab:)) {
        NSString *name = [v.tag_popup selected_name];
        if (name == nil) return NO;
        tag_choose(v, name);
        return YES;
    }
    return NO;
}

/* ── Editing commands ───────────────────────────────────────────────────── */

/* A caret movement command: where it goes, and whether it extends. */
typedef struct {
    const char *name;
    OnDocMove   how;
    BOOL        extend;
} move_command_t;

/*
 * The movement commands of the standard key bindings.  A "paragraph" is a
 * block.  (Home and End are not here: see home_end_command.)
 */
static const move_command_t move_commands[] = {
    { "moveLeft:",                                     ON_MOVE_CHAR_LEFT,   NO  },
    { "moveBackward:",                                 ON_MOVE_CHAR_LEFT,   NO  },
    { "moveRight:",                                    ON_MOVE_CHAR_RIGHT,  NO  },
    { "moveForward:",                                  ON_MOVE_CHAR_RIGHT,  NO  },
    { "moveUp:",                                       ON_MOVE_LINE_UP,     NO  },
    { "moveDown:",                                     ON_MOVE_LINE_DOWN,   NO  },
    { "moveLeftAndModifySelection:",                   ON_MOVE_CHAR_LEFT,   YES },
    { "moveBackwardAndModifySelection:",               ON_MOVE_CHAR_LEFT,   YES },
    { "moveRightAndModifySelection:",                  ON_MOVE_CHAR_RIGHT,  YES },
    { "moveForwardAndModifySelection:",                ON_MOVE_CHAR_RIGHT,  YES },
    { "moveUpAndModifySelection:",                     ON_MOVE_LINE_UP,     YES },
    { "moveDownAndModifySelection:",                   ON_MOVE_LINE_DOWN,   YES },
    { "moveWordLeft:",                                 ON_MOVE_WORD_LEFT,   NO  },
    { "moveWordBackward:",                             ON_MOVE_WORD_LEFT,   NO  },
    { "moveWordRight:",                                ON_MOVE_WORD_RIGHT,  NO  },
    { "moveWordForward:",                              ON_MOVE_WORD_RIGHT,  NO  },
    { "moveWordLeftAndModifySelection:",               ON_MOVE_WORD_LEFT,   YES },
    { "moveWordBackwardAndModifySelection:",           ON_MOVE_WORD_LEFT,   YES },
    { "moveWordRightAndModifySelection:",              ON_MOVE_WORD_RIGHT,  YES },
    { "moveWordForwardAndModifySelection:",            ON_MOVE_WORD_RIGHT,  YES },
    { "moveToLeftEndOfLine:",                          ON_MOVE_LINE_START,  NO  },
    { "moveToBeginningOfLine:",                        ON_MOVE_LINE_START,  NO  },
    { "moveToRightEndOfLine:",                         ON_MOVE_LINE_END,    NO  },
    { "moveToEndOfLine:",                              ON_MOVE_LINE_END,    NO  },
    { "moveToLeftEndOfLineAndModifySelection:",        ON_MOVE_LINE_START,  YES },
    { "moveToBeginningOfLineAndModifySelection:",      ON_MOVE_LINE_START,  YES },
    { "moveToRightEndOfLineAndModifySelection:",       ON_MOVE_LINE_END,    YES },
    { "moveToEndOfLineAndModifySelection:",            ON_MOVE_LINE_END,    YES },
    { "moveToBeginningOfParagraph:",                   ON_MOVE_BLOCK_START, NO  },
    { "moveToEndOfParagraph:",                         ON_MOVE_BLOCK_END,   NO  },
    { "moveToBeginningOfParagraphAndModifySelection:", ON_MOVE_BLOCK_START, YES },
    { "moveToEndOfParagraphAndModifySelection:",       ON_MOVE_BLOCK_END,   YES },
    { "moveParagraphBackwardAndModifySelection:",      ON_MOVE_BLOCK_START, YES },
    { "moveParagraphForwardAndModifySelection:",       ON_MOVE_BLOCK_END,   YES },
    { "moveToBeginningOfDocument:",                    ON_MOVE_DOC_START,   NO  },
    { "moveToEndOfDocument:",                          ON_MOVE_DOC_END,     NO  },
    { "moveToBeginningOfDocumentAndModifySelection:",  ON_MOVE_DOC_START,   YES },
    { "moveToEndOfDocumentAndModifySelection:",        ON_MOVE_DOC_END,     YES },
};

/* The delete commands, and how far each reaches. */
static const struct {
    const char *name;
    OnDocMove   how;
} delete_commands[] = {
    { "deleteBackward:",                               ON_MOVE_CHAR_LEFT  },
    { "deleteBackwardByDecomposingPreviousCharacter:", ON_MOVE_CHAR_LEFT  },
    { "deleteForward:",                                ON_MOVE_CHAR_RIGHT },
    { "deleteWordBackward:",                           ON_MOVE_WORD_LEFT  },
    { "deleteWordForward:",                            ON_MOVE_WORD_RIGHT },
    { "deleteToBeginningOfLine:",                      ON_MOVE_LINE_START },
    { "deleteToEndOfParagraph:",                       ON_MOVE_BLOCK_END  },
    { "deleteToEndOfLine:",                            ON_MOVE_BLOCK_END  },
};

/*
 * cut_selection — Cut: copy the selection, then delete it.
 *
 * Inputs:
 *   v — the view
 *
 * Output: NO when nothing was selected.
 */
static BOOL
cut_selection(NotesNoteView *v)
{
    if (!has_selection(v)) return NO;
    copy_selection(v);
    typing_end(v);
    delete_selection(v);
    after_edit(v);
    return YES;
}

/*
 * run_command — THE editing-command dispatcher: what a key binding's
 * selector does in the view.  doCommandBySelector:, the Edit menu's items
 * and the probe's on_note_view_feed_key all come here.
 *
 * Inputs:
 *   v   — the view
 *   sel — the command
 *
 * Output: YES when the view handled it.
 */
static BOOL
run_command(NotesNoteView *v, SEL sel)
{
    const char *name = sel_getName(sel);
    for (size_t i = 0; i < sizeof move_commands / sizeof *move_commands; i++) {
        if (strcmp(name, move_commands[i].name) == 0) {
            move_caret(v, move_commands[i].how, move_commands[i].extend);
            return YES;
        }
    }
    for (size_t i = 0; i < sizeof delete_commands / sizeof *delete_commands; i++) {
        if (strcmp(name, delete_commands[i].name) == 0) {
            delete_key(v, delete_commands[i].how);
            return YES;
        }
    }
    if (sel == @selector(scrollPageUp:) || sel == @selector(pageUp:)) {
        page_move(v, NO, NO);
    } else if (sel == @selector(scrollPageDown:) || sel == @selector(pageDown:)) {
        page_move(v, YES, NO);
    } else if (sel == @selector(pageUpAndModifySelection:)) {
        page_move(v, NO, YES);
    } else if (sel == @selector(pageDownAndModifySelection:)) {
        page_move(v, YES, YES);
    } else if (sel == @selector(insertNewline:) || sel == @selector(insertLineBreak:) ||
               sel == @selector(insertNewlineIgnoringFieldEditor:)) {
        enter_pressed(v);
    } else if (sel == @selector(insertTab:)) {
        tab_pressed(v, NO);
    } else if (sel == @selector(insertBacktab:)) {
        tab_pressed(v, YES);
    } else if (sel == @selector(cancelOperation:)) {
        if (!has_selection(v)) return NO;      /* Escape goes on up the chain */
        set_caret(v, v.caret, NO);
    } else if (sel == @selector(selectAll:)) {
        select_all(v);
    } else if (sel == @selector(copy:)) {
        copy_selection(v);
    } else if (sel == @selector(cut:)) {
        return cut_selection(v);
    } else if (sel == @selector(paste:)) {
        paste(v);
    } else if (sel == @selector(undo:)) {
        on_note_view_undo(v);
    } else if (sel == @selector(redo:)) {
        on_note_view_redo(v);
    } else {
        return NO;
    }
    return YES;
}

/* ── Pointer ─────────────────────────────────────────────────────────────── */

/*
 * event_mods — an event's modifiers as on_mod_t bits.
 *
 * Inputs:
 *   event — a mouse or key event
 *
 * Output: the bits.
 */
static unsigned
event_mods(NSEvent *event)
{
    NSEventModifierFlags f = event.modifierFlags;
    return ((f & NSEventModifierFlagShift)   ? on_mod_shift   : 0) |
           ((f & NSEventModifierFlagCommand) ? on_mod_command : 0) |
           ((f & NSEventModifierFlagOption)  ? on_mod_option  : 0);
}

/*
 * copy_code_run — the "copy" word: put a code run's text (its lines
 * joined by newlines) on the pasteboard.
 *
 * Inputs:
 *   v     — the view
 *   block — a block of the run
 */
static void
copy_code_run(NotesNoteView *v, guint block)
{
    guint first, count;
    on_doc_layout_code_run(v.layout, block, &first, &count);
    NSMutableString *text = [NSMutableString string];
    for (guint i = first; i < first + count; i++) {
        if (i > first) [text appendString:@"\n"];
        [text appendString:@(on_document_block(v.doc, i)->text->text->str)];
    }
    [v.pasteboard clearContents];
    [v.pasteboard setString:text forType:NSPasteboardTypeString];
    if (v.app) on_app_status(v.app, "Code copied");
}

/*
 * press_at — a primary press at a point in the view: THE click handling,
 * for mouseDown: and the probe alike (GTK: press_at).  One click on a
 * code run's "copy" word copies the run, on a task's box ticks it, on an
 * image opens it, on an attached file selects it (two clicks open it);
 * otherwise one click places the caret (Shift extends) and
 * starts a drag, two select a word, three a line.
 *
 * Inputs:
 *   v      — the view
 *   x, y   — the point, in the view's coordinates
 *   clicks — the click count
 *   mods   — on_mod_t bits
 *
 * Output: YES when something other than text took the press.
 */
static BOOL
press_at(NotesNoteView *v, double x, double y, NSInteger clicks, unsigned mods)
{
    tag_capture_end(v, YES);         /* first: it may move the line's text  */
    OnDocHitResult hit;
    on_doc_layout_hit(v.layout, x, y, &hit);
    im_reset(v);
    typing_end(v);
    v.dragging = NO;

    if (hit.kind == ON_HIT_FILE) {
        if (clicks >= 2) open_file_object(v, hit.block, hit.object);
        else             select_object(v, hit.block, hit.pos.offset);
        return YES;
    }
    if (clicks == 1) {
        switch (hit.kind) {
        case ON_HIT_COPY:
            copy_code_run(v, hit.block);
            return YES;
        case ON_HIT_CHECKBOX:
            on_document_set_checked(v.doc, hit.block, !on_document_block(v.doc, hit.block)->checked);
            after_edit(v);
            return YES;
        case ON_HIT_IMAGE:
            if (!(mods & on_mod_shift)) {
                set_caret(v, hit.pos, NO);
                if (v.image_activated_handler) v.image_activated_handler(hit.image_ord);
                return YES;
            }
            break;
        default:
            break;
        }
        v.goal_x = -1;
        set_caret(v, hit.pos, (mods & on_mod_shift) != 0);
        adopt_flags(v);
        v.dragging    = YES;
        v.drag_anchor = v.anchor;
    } else if (clicks == 2 || clicks == 3) {
        OnPos a, b;
        if (clicks == 2) on_doc_layout_word_at(v.layout, hit.pos, &a, &b);
        else             on_doc_layout_line_at(v.layout, hit.pos, &a, &b);
        v.anchor = a;
        set_caret(v, b, YES);
        adopt_flags(v);
    }
    return NO;
}

/*
 * context_image_index — which image of its block the context menu is on.
 *
 * Inputs:
 *   v — the view
 *
 * Output: -1 for an image block, the inline image's index in its line's
 *         images otherwise; -2 when there is no such image (any more).
 */
static gint
context_image_index(NotesNoteView *v)
{
    OnDocHitResult hit = v.ctx_hit;
    OnBlock *b = on_document_block(v.doc, hit.block);
    if (b == NULL) return -2;
    if (!hit.inline_image) return -1;
    for (guint k = 0; b->text != NULL && k < b->text->objects->len; k++)
        if (g_array_index(b->text->objects, OnInlineObject, k).offset == hit.pos.offset)
            return (gint)k;
    return -2;
}

/*
 * context_image_width — the stored display width of the image the context
 * menu is on (0 = the default thumbnail size).
 *
 * Inputs:
 *   v — the view
 *
 * Output: the width in pixels.
 */
static guint32
context_image_width(NotesNoteView *v)
{
    gint     k = context_image_index(v);
    OnBlock *b = on_document_block(v.doc, v.ctx_hit.block);
    if (k == -1) return b->display_width;
    return k >= 0 ? g_array_index(b->text->objects, OnInlineObject, k).display_width : 0;
}

/*
 * context_menu — the menu for what a secondary click landed on (GTK:
 * context_menu): image items on an image, Open on an attached file, table
 * items on a table, then
 * Cut, Copy and Paste.  The caret moves to the click unless it was on the
 * selected text.
 *
 * Inputs:
 *   v    — the view
 *   x, y — the click, in the view's coordinates
 *
 * Output: the menu; its items target the view.
 */
static NSMenu *
context_menu(NotesNoteView *v, double x, double y)
{
    OnDocHitResult hit;
    on_doc_layout_hit(v.layout, x, y, &hit);
    typing_end(v);
    if (hit.kind == ON_HIT_FILE)
        select_object(v, hit.block, hit.pos.offset);
    else if (!has_selection(v) || hit.kind != ON_HIT_TEXT)
        set_caret(v, hit.pos, NO);
    v.ctx_hit = hit;

    NSMenu  *menu = [[NSMenu alloc] initWithTitle:@""];
    OnBlock *b    = on_document_block(v.doc, hit.block);
    if (hit.kind == ON_HIT_IMAGE) {
        [menu addItemWithTitle:@"Copy Image" action:@selector(image_copy:) keyEquivalent:@""];
        [menu addItemWithTitle:@"Open"       action:@selector(image_open:) keyEquivalent:@""];
        int     iw = 0, ih = 0;
        guint32 shown = context_image_width(v);
        BOOL full = on_note_view_image_size(v, hit.image_ord, &iw, &ih) && shown > 0 &&
                    shown >= (guint32)iw;
        [menu addItemWithTitle:full ? @"Display as Thumbnail" : @"Display Full Size"
                        action:full ? @selector(image_thumbnail:) : @selector(image_full_size:)
                 keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
    }
    if (hit.kind == ON_HIT_FILE) {
        [menu addItemWithTitle:@"Open" action:@selector(file_open:) keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
    }
    if (b != NULL && b->kind == ON_BLOCK_TABLE) {
        [menu addItemWithTitle:@"Add Row"       action:@selector(table_row_add:) keyEquivalent:@""];
        [menu addItemWithTitle:@"Add Column"    action:@selector(table_col_add:) keyEquivalent:@""];
        [menu addItemWithTitle:@"Remove Row"    action:@selector(table_row_remove:) keyEquivalent:@""];
        [menu addItemWithTitle:@"Remove Column" action:@selector(table_col_remove:) keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem *header = [menu addItemWithTitle:@"Header Row" action:@selector(table_header:)
                                      keyEquivalent:@""];
        header.state = b->header ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItemWithTitle:@"Delete Table" action:@selector(table_delete:) keyEquivalent:@""];
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItemWithTitle:@"Cut"   action:@selector(cut:)   keyEquivalent:@""];
    [menu addItemWithTitle:@"Copy"  action:@selector(copy:)  keyEquivalent:@""];
    [menu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@""];
    for (NSMenuItem *item in menu.itemArray) item.target = v;
    return menu;
}

@implementation NotesNoteView

/* ── Initialisation ─────────────────────────────────────────────────────── */

/*
 * init_with_app: — see note_view.h.
 */
- (instancetype)
init_with_app:(OnApp *)app
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    _app    = app;
    _doc    = on_document_new();
    _layout = on_doc_layout_new(_doc, on_look_font_size);
    _caret  = (OnPos){ 0, -1, 0 };
    _anchor = (OnPos){ 0, -1, 0 };
    _goal_x = -1;
    _caret_on = YES;
    _fresh  = YES;
    _pasteboard = NSPasteboard.generalPasteboard;
    on_document_set_observer(_doc, &OBSERVER, (__bridge void *)self);
    [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    apply_style(self);
    [self updateTrackingAreas];
    return self;
}

/*
 * viewDidChangeEffectiveAppearance — light/dark changed: the layout's
 * colours follow.
 */
- (void)
viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    apply_style(self);
}

/*
 * dealloc — release owned resources.
 */
- (void)
dealloc
{
    [_blink_timer invalidate];
    [_typing_timer invalidate];
    [_tag_popup hide];
    on_db_tag_list_free(_tag_choices);
    on_document_set_observer(_doc, NULL, NULL);
    on_doc_layout_free(_layout);
    on_document_free(_doc);
}

/* ── View properties ────────────────────────────────────────────────────── */

/*
 * isFlipped — y grows downward, matching doc_layout coordinates.
 *
 * Output: YES.
 */
- (BOOL)
isFlipped
{
    return YES;
}

/* ── Accessibility ─────────────────────────────────────────────────────── */

/*
 * isAccessibilityElement — VoiceOver sees the note as one text area (GTK:
 * the view implements accessible text).
 *
 * Output: YES.
 */
- (BOOL)
isAccessibilityElement
{
    return YES;
}

/*
 * accessibilityRole — a text area.
 *
 * Output: the role.
 */
- (NSAccessibilityRole)
accessibilityRole
{
    return NSAccessibilityTextAreaRole;
}

/*
 * accessibilityLabel — what the area is.
 *
 * Output: the label.
 */
- (NSString *)
accessibilityLabel
{
    return @"Note";
}

/*
 * accessibilityValue — the note's text: its blocks joined by newlines
 * (access_text).
 *
 * Output: the text.
 */
- (id)
accessibilityValue
{
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (guint i = 0; i < on_document_n_blocks(self.doc); i++)
        [lines addObject:access_text(on_document_block(self.doc, i))];
    return [lines componentsJoinedByString:@"\n"];
}

/*
 * accessibilityNumberOfCharacters — the length of the value.
 *
 * Output: UTF-16 units.
 */
- (NSInteger)
accessibilityNumberOfCharacters
{
    return (NSInteger)((NSString *)self.accessibilityValue).length;
}

/*
 * accessibilitySelectedTextRange — the selection (or the caret) in the
 * value.
 *
 * Output: the range.
 */
- (NSRange)
accessibilitySelectedTextRange
{
    NSUInteger a = access_offset(self.doc, self.anchor), c = access_offset(self.doc, self.caret);
    return NSMakeRange(MIN(a, c), a > c ? a - c : c - a);
}

/*
 * accessibilitySelectedText — the selected text.
 *
 * Output: the text; empty with no selection.
 */
- (NSString *)
accessibilitySelectedText
{
    NSString *value = self.accessibilityValue;
    NSRange   range = self.accessibilitySelectedTextRange;
    return NSMaxRange(range) <= value.length ? [value substringWithRange:range] : @"";
}

/*
 * accessibilityInsertionPointLineNumber — the caret's line: its block.
 *
 * Output: the line number.
 */
- (NSInteger)
accessibilityInsertionPointLineNumber
{
    return (NSInteger)self.caret.block;
}

/*
 * accessibilityVisibleCharacterRange — all of it (the view is as tall as
 * the note; the scroll view clips it).
 *
 * Output: the range.
 */
- (NSRange)
accessibilityVisibleCharacterRange
{
    return NSMakeRange(0, (NSUInteger)self.accessibilityNumberOfCharacters);
}

/*
 * acceptsFirstResponder — the view can receive keyboard focus.
 *
 * Output: YES.
 */
- (BOOL)
acceptsFirstResponder
{
    return YES;
}

/*
 * becomeFirstResponder — start blinking when focused.
 *
 * Output: YES.
 */
- (BOOL)
becomeFirstResponder
{
    BOOL ok = [super becomeFirstResponder];
    if (ok) {
        blink_restart(self);
        self.needsDisplay = YES;
    }
    return ok;
}

/*
 * resignFirstResponder — stop blinking (the caret is not drawn without
 * the focus) and close the typing undo group.
 *
 * Output: YES.
 */
- (BOOL)
resignFirstResponder
{
    BOOL ok = [super resignFirstResponder];
    if (ok) {
        [_blink_timer invalidate];
        _blink_timer = nil;
        _caret_on = YES;
        tag_capture_end(self, YES);
        typing_end(self);
        self.needsDisplay = YES;
    }
    return ok;
}

/*
 * setFrameSize: — a new width re-wraps the text, changing height.
 *
 * Inputs:
 *   size — proposed size
 *
 * Output: none.
 */
- (void)
setFrameSize:(NSSize)size
{
    BOOL changed = size.width != self.frame.size.width;
    [super setFrameSize:size];
    if (changed && _layout) {
        on_doc_layout_set_width(_layout, layout_width(self));
        double h = on_doc_layout_height(_layout);
        NSSize s = size;
        s.height = MAX(h, 1.0);
        [super setFrameSize:s];
        self.needsDisplay = YES;
    }
}

/* ── Tracking areas (mouse cursor) ─────────────────────────────────────── */

/*
 * updateTrackingAreas — one tracking area over the whole view, for the
 * pointer's shape (it follows the view's size).
 */
- (void)
updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea *ta in self.trackingAreas.copy)
        [self removeTrackingArea:ta];
    NSTrackingAreaOptions opts = NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:opts
                                                          owner:self userInfo:nil]];
}

/*
 * cursor_at — the pointer's shape over a point: an I-beam over text, a
 * pointing hand over what a click acts on (an image, a task's box, the
 * code "copy" word), the arrow over a table's frame (GTK: on_motion).
 *
 * Inputs:
 *   pt — the point, in the view's coordinates
 *
 * Output: the cursor.
 */
- (NSCursor *)
cursor_at:(NSPoint)pt
{
    OnDocHitResult hit;
    on_doc_layout_hit(_layout, pt.x, pt.y, &hit);
    switch (hit.kind) {
    case ON_HIT_IMAGE:
    case ON_HIT_CHECKBOX:
    case ON_HIT_COPY:   return NSCursor.pointingHandCursor;
    case ON_HIT_FILE:
    case ON_HIT_OBJECT: return NSCursor.arrowCursor;
    default:            return NSCursor.IBeamCursor;
    }
}

/*
 * cursorUpdate: — the pointer entered the view: set its shape.
 *
 * Inputs:
 *   event — the event
 */
- (void)
cursorUpdate:(NSEvent *)event
{
    [[self cursor_at:[self convertPoint:event.locationInWindow fromView:nil]] set];
}

/*
 * mouseMoved: — the pointer moved over the view: its shape follows what
 * is under it.
 *
 * Inputs:
 *   event — the event
 */
- (void)
mouseMoved:(NSEvent *)event
{
    [[self cursor_at:[self convertPoint:event.locationInWindow fromView:nil]] set];
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

/*
 * drawRect: — fill the page, then delegate to doc_layout.
 *
 * Inputs:
 *   dirty — the clip rectangle: only the blocks crossing it are drawn
 *
 * Output: none.
 */
- (void)
drawRect:(NSRect)dirty
{
    CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;

    /* Page background. */
    NSColor *page = shared_ui_color(on_look_page);
    [page setFill];
    NSRectFill(dirty);


    /* The layout's coordinates are the view's (page margins included). */
    CGContextSaveGState(ctx);

    BOOL focused = (self.window.firstResponder == self);
    OnPos sel_a, sel_b;
    sel_bounds(self, &sel_a, &sel_b);

    on_rect_t clip = {
        .x = dirty.origin.x,
        .y = dirty.origin.y,
        .w = dirty.size.width,
        .h = dirty.size.height,
    };

    OnDocPaint paint = {
        .clip         = clip,
        .has_sel      = has_selection(self),
        .sel_a        = sel_a,
        .sel_b        = sel_b,
        .caret_visible = _caret_on && focused && !has_selection(self),
        .caret        = _caret,
        .focused      = focused,
    };
    on_doc_layout_draw(_layout, ctx, &paint);
    CGContextRestoreGState(ctx);
}

/* ── Mouse handling ─────────────────────────────────────────────────────── */

/*
 * hit_at_event — map a mouse event's location to a document hit result.
 *
 * Inputs:
 *   event — the mouse event
 *   out   — receives the hit
 *
 * Output: none.
 */
- (void)
hit_at_event:(NSEvent *)event out:(OnDocHitResult *)out
{
    NSPoint pt = [self convertPoint:event.locationInWindow fromView:nil];
    on_doc_layout_hit(_layout, pt.x, pt.y, out);
}

/*
 * mouseDown: — a primary press: take the focus and hand it to press_at.
 *
 * Inputs:
 *   event — the mouse-down event
 */
- (void)
mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self];
    NSPoint pt = [self convertPoint:event.locationInWindow fromView:nil];
    press_at(self, pt.x, pt.y, event.clickCount, event_mods(event));
}

/*
 * menuForEvent: — the context menu, for a right click or a Control-click.
 *
 * Inputs:
 *   event — the click
 *
 * Output: the menu.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    [self.window makeFirstResponder:self];
    NSPoint pt = [self convertPoint:event.locationInWindow fromView:nil];
    return context_menu(self, pt.x, pt.y);
}

/*
 * file_open: — Open: the context file in its default app.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
file_open:(id)sender
{
    open_file_object(self, _ctx_hit.block, _ctx_hit.object);
}

/*
 * draggingEntered: / draggingUpdated: — files dragged over the note are
 * copied in; anything else is refused.
 *
 * Inputs:
 *   sender — the drag
 *
 * Output: copy when the drag carries files, else none.
 */
- (NSDragOperation)
draggingEntered:(id<NSDraggingInfo>)sender
{
    return pasteboard_file_urls(sender.draggingPasteboard).count > 0 ? NSDragOperationCopy
                                                                      : NSDragOperationNone;
}

- (NSDragOperation)
draggingUpdated:(id<NSDraggingInfo>)sender
{
    return [self draggingEntered:sender];
}

/*
 * performDragOperation: — drop files into the note where they were
 * dropped: the caret goes to the drop point (an existing selection is not
 * replaced), then each file becomes a block there.
 *
 * Inputs:
 *   sender — the drag
 *
 * Output: YES when files were taken.
 */
- (BOOL)
performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray<NSURL *> *files = pasteboard_file_urls(sender.draggingPasteboard);
    if (files.count == 0) return NO;
    on_note_view_drop_files(self, files,
                            [self convertPoint:sender.draggingLocation fromView:nil]);
    return YES;
}

/*
 * image_copy: — Copy Image: the context image on the pasteboard.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
image_copy:(id)sender
{
    GBytes *png = on_note_view_image_png(self, _ctx_hit.image_ord);
    if (png == NULL) return;
    gsize n;
    const void *bytes = g_bytes_get_data(png, &n);
    NSImage *image = [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes length:n]];
    if (image == nil) return;
    [_pasteboard clearContents];
    [_pasteboard writeObjects:@[image]];
}

/*
 * image_open: — Open: the context image in the outside viewer.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
image_open:(id)sender
{
    on_note_view_image_open(self, _ctx_hit.image_ord);
}

/*
 * set_context_image_width: — store the context image's display width.
 *
 * Inputs:
 *   width — pixels; 0 = the default thumbnail size
 */
- (void)
set_context_image_width:(guint32)width
{
    gint inline_ord = context_image_index(self);
    if (inline_ord < -1) return;
    typing_end(self);
    if (on_document_set_image_width(_doc, _ctx_hit.block, inline_ord, width)) after_edit(self);
}

/*
 * image_full_size: / image_thumbnail: — Display Full Size / as Thumbnail.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
image_full_size:(id)sender
{
    int w = 0, h = 0;
    if (on_note_view_image_size(self, _ctx_hit.image_ord, &w, &h))
        [self set_context_image_width:(guint32)w];
}

- (void)
image_thumbnail:(id)sender
{
    [self set_context_image_width:0];
}

/*
 * table_reshape: — the table items of the context menu, by selector: add
 * or remove the row or column of the clicked cell, or delete the table.
 *
 * Inputs:
 *   command — which reshape
 */
- (void)
table_reshape:(SEL)command
{
    OnBlock *b = on_document_block(_doc, _ctx_hit.block);
    if (b == NULL || b->kind != ON_BLOCK_TABLE) return;
    gint cell = _ctx_hit.pos.cell >= 0 ? _ctx_hit.pos.cell : 0;
    gint r = cell / b->cols, c = cell % b->cols;
    typing_end(self);
    gboolean did;
    gboolean table_kept = YES;
    if (command == @selector(table_row_add:))         did = on_document_table_insert_row(_doc, _ctx_hit.block, r + 1);
    else if (command == @selector(table_row_remove:)) did = on_document_table_remove_row(_doc, _ctx_hit.block, r);
    else if (command == @selector(table_col_add:))    did = on_document_table_insert_col(_doc, _ctx_hit.block, c + 1);
    else if (command == @selector(table_col_remove:)) did = on_document_table_remove_col(_doc, _ctx_hit.block, c);
    else {
        OnPos a = { _ctx_hit.block, -1, 0 }, e = { _ctx_hit.block, -1, 1 }, out;
        did = on_document_delete_range(_doc, a, e, &out);
        if (did) _caret = _anchor = clamp_pos(_doc, out);
        table_kept = NO;
    }
    if (!did) return;
    if (table_kept) {
        /* The cells were renumbered: the caret goes to the clicked cell's
         * row and column in the new grid (or the nearest one left), at its
         * start — an index or offset from the old grid may not exist.     */
        b = on_document_block(_doc, _ctx_hit.block);
        gint row = MIN(r, b->rows - 1), col = MIN(c, b->cols - 1);
        _caret = _anchor = clamp_pos(_doc, (OnPos){ _ctx_hit.block, row * b->cols + col, 0 });
    }
    after_edit(self);
}

/*
 * table_row_add: … table_delete: — the context menu's table items.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void) table_row_add:(id)sender    { [self table_reshape:_cmd]; }
- (void) table_row_remove:(id)sender { [self table_reshape:_cmd]; }
- (void) table_col_add:(id)sender    { [self table_reshape:_cmd]; }
- (void) table_col_remove:(id)sender { [self table_reshape:_cmd]; }
- (void) table_delete:(id)sender     { [self table_reshape:_cmd]; }

/*
 * table_header: — Header Row: turn the clicked table's header row on or
 * off.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
table_header:(id)sender
{
    OnBlock *b = on_document_block(_doc, _ctx_hit.block);
    if (b == NULL || b->kind != ON_BLOCK_TABLE) return;
    typing_end(self);
    if (on_document_table_set_header(_doc, _ctx_hit.block, !b->header)) after_edit(self);
}

/*
 * mouseDragged: — extend a selection drag.
 *
 * Inputs:
 *   event — the mouse-dragged event
 *
 * Output: none.
 */
- (void)
mouseDragged:(NSEvent *)event
{
    if (!_dragging) return;
    OnDocHitResult hit;
    [self hit_at_event:event out:&hit];
    _anchor = _drag_anchor;
    set_caret(self, hit.pos, YES);
    self.needsDisplay = YES;
}

/*
 * mouseUp: — end a drag.
 *
 * Inputs:
 *   event — the mouse-up event
 *
 * Output: none.
 */
- (void)
mouseUp:(NSEvent *)event
{
    (void)event;
    _dragging = NO;
}

/* ── Keyboard handling ───────────────────────────────────────────────────── */

/*
 * keyDown: — route through the input method first, then doCommandBySelector:.
 *
 * Inputs:
 *   event — the key event
 *
 * Output: none.
 */
- (void)
keyDown:(NSEvent *)event
{
    self.key_event = event;
    [self interpretKeyEvents:@[event]];
    self.key_event = nil;
}

/* The Home and End keys' virtual key codes (Carbon's kVK_Home, kVK_End). */
enum { home_key_code = 0x73, end_key_code = 0x77 };

/*
 * home_end_command — Home and End move to the line's start and end, Shift
 * extending, as in the GTK app.  AppKit's bindings would only scroll for
 * Home/End and send Shift+End as ⌘⇧↓ (select to the document's end), so
 * the keys themselves are recognised.
 *
 * Inputs:
 *   event — the key event being interpreted, or nil
 *   sel   — the command AppKit chose for it
 *
 * Output: the line command for Home or End, else `sel` unchanged.
 */
static SEL
home_end_command(NSEvent *event, SEL sel)
{
    if (event.type != NSEventTypeKeyDown) return sel;
    BOOL shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    if (event.keyCode == home_key_code)
        return shift ? @selector(moveToBeginningOfLineAndModifySelection:) : @selector(moveToBeginningOfLine:);
    if (event.keyCode == end_key_code)
        return shift ? @selector(moveToEndOfLineAndModifySelection:) : @selector(moveToEndOfLine:);
    return sel;
}

/*
 * doCommandBySelector: — AppKit sends this for movement and edit commands
 * after the input method passes on the key (the standard key bindings:
 * ⌘← moveToLeftEndOfLine:, ⌥⌫ deleteWordBackward:, ⌃K
 * deleteToEndOfParagraph: …).  A command the view does not know goes to
 * NSResponder, which beeps.
 *
 * Inputs:
 *   sel — the selector for the command
 */
- (void)
doCommandBySelector:(SEL)sel
{
    if (tag_popup_command(self, sel)) return;
    if (!run_command(self, home_end_command(self.key_event, sel))) [super doCommandBySelector:sel];
}

/* ── NSTextInputClient ───────────────────────────────────────────────────── */

/*
 * The input method works on the text the caret is in — a paragraph or a
 * table cell — and its ranges are UTF-16 indexes into that text: enough
 * for accents (press and hold), reconversion and marked text, and the same
 * in a cell as in a paragraph.  The marked (uncommitted) text is not in the
 * document: the layout splices it in at the caret (on_doc_layout_set_preedit).
 */

/*
 * input_select — make a UTF-16 range of the caret's text the selection,
 * the start becoming the anchor.
 *
 * Inputs:
 *   range — the range; nothing happens for NSNotFound or no caret text
 */
- (void)
input_select:(NSRange)range
{
    OnText *t = caret_text(self);
    if (t == NULL || range.location == NSNotFound) return;
    OnPos a = _caret, b = _caret;
    a.offset = on_utf8_from_utf16(t->text->str, t->text->len, range.location);
    b.offset = on_utf8_from_utf16(t->text->str, t->text->len, NSMaxRange(range));
    _anchor = clamp_pos(_doc, a);
    _caret  = clamp_pos(_doc, b);
}

/*
 * insertText:replacementRange: — committed text, typed or from the input
 * method.  The replacement range (an accent replacing the letter before
 * it, a reconversion) becomes the selection the text replaces; a committed
 * newline is Return, and text of several lines goes in as lines.
 *
 * Inputs:
 *   string           — NSString or NSAttributedString
 *   replacementRange — UTF-16 range of the caret's text, or NSNotFound
 */
- (void)
insertText:(id)string replacementRange:(NSRange)replacementRange
{
    NSString *text = [string isKindOfClass:NSAttributedString.class]
                   ? ((NSAttributedString *)string).string : string;
    [self clear_marked_text];
    [self input_select:replacementRange];
    if ([text isEqualToString:@"\n"] || [text isEqualToString:@"\r"]) {
        enter_pressed(self);
        return;
    }
    const char *u8 = text.UTF8String;
    if (u8 == NULL || *u8 == '\0') return;
    if (on_document_text_has_line_break(u8, strlen(u8)) || strstr(u8, ON_OBJ_CHAR) != NULL) {
        /* Several lines at once (dictation, Writing Tools): a line of
         * text cannot hold them, so they go in as a plain paste does.   */
        OnDocument *frag = on_document_from_text(u8);
        paste_fragment(self, frag, YES);
        on_document_free(frag);
        return;
    }
    insert_typed(self, u8, strlen(u8));
}

/*
 * setMarkedText:selectedRange:replacementRange: — the input method's
 * uncommitted text, shown at the caret.  A replacement range, or else the
 * selection, is deleted first (the marked text takes its place).
 *
 * Inputs:
 *   string           — the marked text
 *   selectedRange    — the input method's caret or selection inside it
 *   replacementRange — UTF-16 range of the caret's text, or NSNotFound
 */
- (void)
setMarkedText:(id)string
 selectedRange:(NSRange)selectedRange
replacementRange:(NSRange)replacementRange
{
    NSString *text = [string isKindOfClass:NSAttributedString.class]
                   ? ((NSAttributedString *)string).string : string;
    [self input_select:replacementRange];
    if (has_selection(self)) {
        typing_end(self);
        delete_selection(self);
        after_edit(self);
    }
    caret_ensure_text(self);
    _preedit     = text.length > 0 ? [text copy] : nil;
    _preedit_sel = selectedRange;
    NSString *before = _preedit ? [_preedit substringToIndex:MIN(selectedRange.location, _preedit.length)]
                                : nil;
    on_doc_layout_set_preedit(_layout, _caret, _preedit.UTF8String,
                              before ? strlen(before.UTF8String) : 0);
    fit_height(self);
}

/*
 * unmarkText — commit the marked text as it stands.
 */
- (void)
unmarkText
{
    NSString *text = _preedit;
    [self clear_marked_text];
    const char *u8 = text.UTF8String;
    if (u8 != NULL && *u8 != '\0') insert_typed(self, u8, strlen(u8));
}

/*
 * clear_marked_text — drop the marked text from the layout (it was never
 * in the document).
 */
- (void)
clear_marked_text
{
    if (_preedit == nil) return;
    _preedit = nil;
    on_doc_layout_set_preedit(_layout, _caret, NULL, 0);
    fit_height(self);
}

/*
 * caret_u16 — the caret as a UTF-16 index of its text.
 *
 * Output: the index; 0 when the caret is not in text.
 */
- (NSUInteger)
caret_u16
{
    OnText *t = caret_text(self);
    return t ? on_utf16_from_utf8(t->text->str, MIN(_caret.offset, t->text->len)) : 0;
}

/*
 * selectedRange — the selection (or the caret) in the caret's text; inside
 * the marked text while there is some.
 *
 * Output: the range.
 */
- (NSRange)
selectedRange
{
    NSUInteger at = [self caret_u16];
    if (_preedit) return NSMakeRange(at + _preedit_sel.location, _preedit_sel.length);
    OnPos a, b;
    sel_bounds(self, &a, &b);
    OnText *t = caret_text(self);
    if (t == NULL || a.block != b.block || a.cell != b.cell) return NSMakeRange(at, 0);
    NSUInteger ua = on_utf16_from_utf8(t->text->str, a.offset);
    NSUInteger ub = on_utf16_from_utf8(t->text->str, b.offset);
    return NSMakeRange(ua, ub - ua);
}

/*
 * markedRange — where the marked text sits in the caret's text.
 *
 * Output: the range; {NSNotFound, 0} when there is none.
 */
- (NSRange)
markedRange
{
    return _preedit ? NSMakeRange([self caret_u16], _preedit.length) : NSMakeRange(NSNotFound, 0);
}

/*
 * hasMarkedText — is there marked text?
 *
 * Output: YES while there is.
 */
- (BOOL)
hasMarkedText
{
    return _preedit != nil;
}

/*
 * attributedSubstringForProposedRange:actualRange: — part of the caret's
 * text, for the input method (reconversion, accents).
 *
 * Inputs:
 *   range       — the UTF-16 range requested
 *   actualRange — receives the range returned (may be NULL)
 *
 * Output: the text, or nil for an empty range.
 */
- (NSAttributedString *)
attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    OnText   *t   = caret_text(self);
    NSString *str = t ? @(t->text->str) : @"";
    NSRange   r   = NSMakeRange(MIN(range.location, str.length), 0);
    r.length = MIN(range.length, str.length - r.location);
    if (actualRange) *actualRange = r;
    return r.length > 0 ? [[NSAttributedString alloc] initWithString:[str substringWithRange:r]] : nil;
}

/*
 * validAttributesForMarkedText — marked-text attributes understood.
 *
 * Output: none: marked text is drawn underlined, in the text's own style.
 */
- (NSArray<NSAttributedStringKey> *)
validAttributesForMarkedText
{
    return @[];
}

/*
 * firstRectForCharacterRange:actualRange: — where a range of the caret's
 * text is on screen, for the input method's candidate window.
 *
 * Inputs:
 *   range       — the UTF-16 range
 *   actualRange — receives the range used (may be NULL)
 *
 * Output: the screen rectangle of its first character's caret.
 */
- (NSRect)
firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    if (actualRange) *actualRange = range;
    OnText *t   = caret_text(self);
    OnPos   pos = _caret;
    if (t && range.location != NSNotFound)
        pos.offset = on_utf8_from_utf16(t->text->str, t->text->len, range.location);
    on_rect_t r;
    on_doc_layout_caret_rect(_layout, pos, &r);
    NSRect rect = [self convertRect:NSMakeRect(r.x, r.y, r.w, r.h) toView:nil];
    return self.window ? [self.window convertRectToScreen:rect] : rect;
}

/*
 * characterIndexForPoint: — the UTF-16 index of the caret's text under a
 * screen point.
 *
 * Inputs:
 *   point — screen coordinates
 *
 * Output: the index, or NSNotFound when the point is not on that text.
 */
- (NSUInteger)
characterIndexForPoint:(NSPoint)point
{
    NSPoint p = [self convertPoint:[self.window convertPointFromScreen:point] fromView:nil];
    OnDocHitResult hit;
    on_doc_layout_hit(_layout, p.x, p.y, &hit);
    OnText *t = caret_text(self);
    if (t == NULL || hit.kind != ON_HIT_TEXT || hit.pos.block != _caret.block || hit.pos.cell != _caret.cell)
        return NSNotFound;
    return on_utf16_from_utf8(t->text->str, MIN(hit.pos.offset, t->text->len));
}

/* ── NSView copy/paste/undo selectors ───────────────────────────────────── */

/*
 * copy: / cut: / paste: / selectAll: / undo: / redo: — the Edit menu's
 * items, sent up the responder chain; each is the command of the same
 * name (run_command).
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void) copy:(id)sender      { run_command(self, _cmd); }
- (void) cut:(id)sender       { run_command(self, _cmd); }
- (void) paste:(id)sender     { run_command(self, _cmd); }
- (void) selectAll:(id)sender { run_command(self, _cmd); }
- (void) undo:(id)sender      { run_command(self, _cmd); }
- (void) redo:(id)sender      { run_command(self, _cmd); }

- (BOOL)
validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    SEL sel = item.action;
    if (sel == @selector(copy:) || sel == @selector(cut:))
        return has_selection(self);
    if (sel == @selector(undo:))
        return on_document_can_undo(_doc);
    if (sel == @selector(redo:))
        return on_document_can_redo(_doc);
    return YES;
}

@end

/* ── C API (note_view.h) ────────────────────────────────────────────────── */

/*
 * load_document — the body of on_note_view_load_document and
 * on_note_view_reload_document: take over `doc`, with the caret on the
 * first text line, or where it was.
 *
 * Inputs:
 *   view       — the view
 *   doc        — the document; ownership passes to the view
 *   keep_place — YES to keep the caret and selection (clamped), NO for
 *                the start of the note
 */
static void
load_document(NotesNoteView *view, OnDocument *doc, BOOL keep_place)
{
    OnPos caret  = view.caret;
    OnPos anchor = view.anchor;
    tag_capture_end(view, NO);
    im_reset(view);
    typing_end(view);
    on_document_set_observer(view.doc, NULL, NULL);
    on_doc_layout_free(view.layout);
    on_document_free(view.doc);

    view.doc    = doc;
    view.layout = on_doc_layout_new(doc, on_look_font_size);
    view.caret  = (OnPos){ 0, -1, 0 };
    view.anchor = (OnPos){ 0, -1, 0 };
    view.goal_x = -1;
    view.fresh  = YES;
    view.inline_flags = 0;
    on_document_clear_undo(doc);
    on_document_set_observer(doc, &OBSERVER, (__bridge void *)view);
    apply_style(view);

    /* The caret starts on the title line, after the tag line, or under
     * the first-line-title setting on the line after the title, so an
     * existing note opens ready to be read or added to, not renamed (GTK:
     * load_document).  A note with an empty title (a new note) starts on
     * its tag line instead, at its end, ready for a tag. */
    guint          title_at = on_document_title_block(doc);
    OnPos          start    = { title_at, -1, 0 };
    const OnBlock *title    = on_document_block(doc, title_at);
    BOOL           named    = title == NULL || title->text == NULL || title->text->text->len > 0;
    if (!named && title_at > 0)
        start = (OnPos){ 0, -1, on_document_block(doc, 0)->text->text->len };
    else if (view.app->first_line_title && named && on_document_n_blocks(doc) > title_at + 1)
        start.block = title_at + 1;
    view.caret  = clamp_pos(doc, keep_place ? caret : start);
    view.anchor = clamp_pos(doc, keep_place ? anchor : start);
    on_document_take_tags_modified(doc);
    on_document_take_actions_modified(doc);
    if (view.find_text != nil) on_note_view_find(view, view.find_text);
    if (view.window != nil) {           /* a reload: lay the new document out now */
        on_doc_layout_set_width(view.layout, layout_width(view));
        fit_height(view);
        view.needsDisplay = YES;
    }
}

/*
 * on_note_view_load_document — see note_view.h.
 */
void
on_note_view_load_document(NotesNoteView *view, OnDocument *doc)
{
    load_document(view, doc, NO);
}

/*
 * on_note_view_reload_document — see note_view.h.
 */
void
on_note_view_reload_document(NotesNoteView *view, OnDocument *doc)
{
    load_document(view, doc, YES);
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
 * on_note_view_begin_external_edit — see note_view.h.
 */
OnDocument *
on_note_view_begin_external_edit(NotesNoteView *view)
{
    typing_end(view);
    return view.doc;
}

/*
 * on_note_view_end_external_edit — see note_view.h.
 */
void
on_note_view_end_external_edit(NotesNoteView *view)
{
    edit_settled(view, NO);             /* fires edited_handler, once */
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
    BOOL f = view.fresh;
    view.fresh = NO;
    return f || on_document_take_actions_modified(view.doc);
}

/*
 * on_note_view_toggle_inline — see note_view.h.
 */
void
on_note_view_toggle_inline(NotesNoteView *view, OnFormatFlags flag)
{
    if (has_selection(view)) {
        typing_end(view);
        OnPos a, b;
        sel_bounds(view, &a, &b);
        OnText *t = on_document_text_at(view.doc, a);
        BOOL has = t != NULL && (on_text_flags_at(t, a.offset) & flag);
        on_document_begin_group(view.doc);
        for (guint i = a.block; i <= b.block; i++) {
            const OnBlock *blk = on_document_block(view.doc, i);
            if (i == a.block && a.cell >= 0 && a.cell == b.cell) {
                /* Inside one table cell (a table block has no text).    */
                if (b.offset > a.offset)
                    on_document_set_flags(view.doc, a, b.offset - a.offset, flag, !has);
                break;
            }
            if (blk->text == NULL) continue;
            OnPos p = { i, -1, (i == a.block) ? a.offset : 0 };
            gsize end = (i == b.block) ? b.offset : blk->text->text->len;
            if (end > p.offset)
                on_document_set_flags(view.doc, p, end - p.offset, flag, !has);
        }
        on_document_end_group(view.doc);
        view.inline_flags = has ? (view.inline_flags & ~(guint32)flag)
                                : (view.inline_flags | (guint32)flag);
        after_edit(view);
    } else {
        view.inline_flags ^= (guint32)flag;
    }
    if (view.inline_flags_handler) view.inline_flags_handler();
    [view.window makeFirstResponder:view];
}

/*
 * on_note_view_inline_flags — see note_view.h.
 */
guint32
on_note_view_inline_flags(NotesNoteView *view)
{
    return view.inline_flags;
}

/*
 * on_note_view_toggle_paragraph — see note_view.h.
 */
void
on_note_view_toggle_paragraph(NotesNoteView *view, guint32 flag)
{
    static const struct { guint32 flag; OnBlockKind kind; } map[] = {
        { ON_FMT_H1,          ON_BLOCK_H1     },
        { ON_FMT_H2,          ON_BLOCK_H2     },
        { ON_FMT_CODEBLOCK,   ON_BLOCK_CODE   },
        { ON_FMT_LIST_BULLET, ON_BLOCK_BULLET },
        { ON_FMT_LIST_NUMBER, ON_BLOCK_NUMBER },
        { ON_FMT_LIST_CHECK,  ON_BLOCK_CHECK  },
        { 0, ON_BLOCK_PARA },
    };
    OnBlockKind kind = ON_BLOCK_PARA;
    for (int i = 0; map[i].flag; i++)
        if (map[i].flag == flag) { kind = map[i].kind; break; }

    OnPos a, b;
    sel_bounds(view, &a, &b);
    BOOL all = YES;
    for (guint i = a.block; i <= b.block; i++) {
        const OnBlock *blk = on_document_block(view.doc, i);
        if (blk->text != NULL && blk->kind != kind) { all = NO; break; }
    }
    if (all && flag != 0) kind = ON_BLOCK_PARA;
    typing_end(view);
    on_document_begin_group(view.doc);
    for (guint i = a.block; i <= b.block; i++) {
        const OnBlock *blk = on_document_block(view.doc, i);
        if (blk->text == NULL) continue;
        on_document_set_kind(view.doc, i, kind);
        if (kind == ON_BLOCK_CODE) {
            gsize len = blk->text->text->len;
            OnPos p = { i, -1, 0 };
            if (len > 0)
                on_document_set_flags(view.doc, p, len, ON_FMT_TAG, FALSE);
        }
    }
    on_document_end_group(view.doc);
    after_edit(view);
    [view.window makeFirstResponder:view];
}

/*
 * on_note_view_insert_image — see note_view.h.
 */
void
on_note_view_insert_image(NotesNoteView *view, const uint8_t *png, size_t n)
{
    GBytes *bytes = g_bytes_new(png, n);
    insert_block_at_caret(view, on_block_new_image(bytes, 0));
    g_bytes_unref(bytes);
}

/*
 * on_note_view_drop_files — see note_view.h.
 */
void
on_note_view_drop_files(NotesNoteView *view, NSArray<NSURL *> *urls, NSPoint point)
{
    OnDocHitResult hit;
    on_doc_layout_hit(view.layout, point.x, point.y, &hit);
    OnPos at = hit.pos;
    if (view.app->drop_files_last_line)
        at = (OnPos){ on_document_n_blocks(view.doc) - 1, -1, G_MAXSIZE };   /* clamped to the end */
    [view.window makeFirstResponder:view];
    im_reset(view);
    typing_end(view);
    set_caret(view, at, NO);
    insert_files(view, urls);
}

/*
 * on_note_view_insert_table — see note_view.h.
 */
void
on_note_view_insert_table(NotesNoteView *view)
{
    insert_block_at_caret(view, on_block_new_table(3, 3));
}

/*
 * on_note_view_insert_date — see note_view.h.
 */
void
on_note_view_insert_date(NotesNoteView *view)
{
    const char *day = shared_ui_format_date(time(NULL), shared_ui_date_iso).UTF8String;
    insert_typed(view, day, strlen(day));
}

/*
 * on_note_view_undo — see note_view.h.
 */
void
on_note_view_undo(NotesNoteView *view)
{
    tag_capture_end(view, NO);
    typing_end(view);
    if (!on_document_undo(view.doc)) return;
    view.caret = view.anchor = clamp_pos(view.doc, on_document_last_change(view.doc));
    adopt_flags(view);
    after_edit(view);
}

/*
 * on_note_view_redo — see note_view.h.
 */
void
on_note_view_redo(NotesNoteView *view)
{
    tag_capture_end(view, NO);
    typing_end(view);
    if (!on_document_redo(view.doc)) return;
    view.caret = view.anchor = clamp_pos(view.doc, on_document_last_change(view.doc));
    adopt_flags(view);
    after_edit(view);
}

/*
 * on_note_view_find — see note_view.h.
 */
void
on_note_view_find(NotesNoteView *view, NSString *text)
{
    view.find_text = (text.length > 0) ? text : nil;
    refresh_hits(view);
    if (view.find_text != nil) {
        /* Select the first match at or after the caret, so the query
         * lands somewhere as it is typed; Next/Previous step from that
         * selection.  Collapsing to the selection's start first makes a
         * match under the caret the first pick (GTK: on_note_view_find). */
        OnPos a, b;
        sel_bounds(view, &a, &b);
        view.anchor = view.caret = a;
        on_note_view_find_step(view, view.find_text, YES);
    }
    view.needsDisplay = YES;
}

/*
 * on_note_view_find_step — see note_view.h.
 */
void
on_note_view_find_step(NotesNoteView *view, NSString *text, BOOL forward)
{
    if (!text || text.length == 0) return;
    size_t n;
    OnDocHit *hits = compute_hits(view.doc, text, &n);
    if (n == 0) return;
    /* Forward: the first hit starting at or after the selection's end (the
     * caret when nothing is selected); back: the last one starting before
     * the selection's start — so a selected hit is stepped off, not found
     * again.  Wraps at either end. */
    OnPos lo, hi;
    sel_bounds(view, &lo, &hi);
    size_t pick = forward ? 0 : n - 1;
    for (size_t i = 0; i < n; i++) {
        OnPos p = { hits[i].block, hits[i].cell, hits[i].start };
        if (forward && on_pos_cmp(p, hi) >= 0) { pick = i; break; }
        if (!forward && on_pos_cmp(p, lo) < 0) pick = i;
    }
    OnDocHit *h = &hits[pick];
    OnPos a = { h->block, h->cell, h->start };
    OnPos b = { h->block, h->cell, h->start + h->len };
    view.anchor = a;
    set_caret(view, b, YES);
    free(hits);
    scroll_to_caret(view);
}

/*
 * on_note_view_selected_text — see note_view.h.
 */
NSString *
on_note_view_selected_text(NotesNoteView *view)
{
    if (!has_selection(view)) return nil;
    OnPos a, b;
    sel_bounds(view, &a, &b);
    OnDocument *frag = on_document_copy_range(view.doc, a, b);
    gchar      *text = on_document_plain_text(frag);
    on_document_free(frag);
    NSString *result = @(text ?: "");
    g_free(text);
    return result;
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
 * image_locate — where the note's ordinal-th image is: image blocks and
 * inline images counted together in document order, as everywhere else
 * that addresses images (GTK: image_locate).
 *
 * Inputs:
 *   doc        — the document
 *   ordinal    — the image ordinal
 *   block      — receives its block
 *   inline_ord — receives its index among the block's inline images, or
 *                -1 for an image block
 *
 * Output: NO when there is no such image.
 */
static BOOL
image_locate(const OnDocument *doc, NSInteger ordinal, guint *block, gint *inline_ord)
{
    if (ordinal < 0) return NO;
    for (guint i = 0; i < on_document_n_blocks(doc); i++) {
        const OnBlock *b = on_document_block(doc, i);
        if (b->kind == ON_BLOCK_IMAGE) {
            if (ordinal-- == 0) {
                *block = i;
                *inline_ord = -1;
                return YES;
            }
        } else if (b->text != NULL) {
            for (guint k = 0; k < b->text->objects->len; k++) {
                if (g_array_index(b->text->objects, OnInlineObject, k).kind != ON_OBJECT_IMAGE ||
                    ordinal-- != 0)
                    continue;
                *block = i;
                *inline_ord = (gint)k;
                return YES;
            }
        }
    }
    return NO;
}

/*
 * reveal_pos — the caret to a position, scrolled a third of the way down
 * the window: the one placement both reveals share (GTK: reveal_pos).
 *
 * Inputs:
 *   v — the view
 *   p — the position
 */
static void
reveal_pos(NotesNoteView *v, OnPos p)
{
    set_caret(v, p, NO);
    NSScrollView *sv = v.enclosingScrollView;
    if (sv == nil) return;
    on_rect_t r;
    on_doc_layout_caret_rect(v.layout, p, &r);
    double page = sv.documentVisibleRect.size.height;
    double top  = MAX(0.0, MIN(r.y - page * 0.3, v.frame.size.height - page));
    [sv.contentView scrollToPoint:NSMakePoint(0, top)];
    [sv reflectScrolledClipView:sv.contentView];
}

/*
 * on_note_view_image_size — see note_view.h.
 */
BOOL
on_note_view_image_size(NotesNoteView *view, NSInteger ordinal, int *w, int *h)
{
    GBytes *png = on_note_view_image_png(view, ordinal);
    if (png == NULL) return NO;
    gsize n;
    const guint8 *bytes = g_bytes_get_data(png, &n);
    return on_image_probe_size(bytes, n, w, h);
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
 * on_note_view_image_open — see note_view.h.
 */
BOOL
on_note_view_image_open(NotesNoteView *view, NSInteger ordinal)
{
    GBytes *png = on_note_view_image_png(view, ordinal);
    if (png == NULL) return NO;
    gsize n;
    const void *bytes = g_bytes_get_data(png, &n);
    return on_ui_open_image_external([NSData dataWithBytes:bytes length:n]);
}

/*
 * on_note_view_image_reveal — see note_view.h.
 */
BOOL
on_note_view_image_reveal(NotesNoteView *view, NSInteger ordinal)
{
    guint block;
    gint  k;
    if (!image_locate(view.doc, ordinal, &block, &k)) return NO;
    OnPos p = { block, -1, 0 };
    if (k >= 0)
        p.offset = g_array_index(on_document_block(view.doc, block)->text->objects,
                                 OnInlineObject, k).offset;
    reveal_pos(view, p);
    return YES;
}

/*
 * on_note_view_action_reveal — see note_view.h.
 */
BOOL
on_note_view_action_reveal(NotesNoteView *view, NSInteger ordinal)
{
    GArray *blocks = on_document_action_blocks(view.doc);
    if (!blocks || (gint)ordinal >= (gint)blocks->len) {
        if (blocks) g_array_free(blocks, TRUE);
        return NO;
    }
    guint block_idx = g_array_index(blocks, guint, ordinal);
    g_array_free(blocks, TRUE);
    OnPos p = { block_idx, -1, 1 };          /* just after the '!' */
    reveal_pos(view, p);
    return YES;
}

/*
 * on_note_view_settings_changed — see note_view.h.
 */
void
on_note_view_settings_changed(NotesNoteView *view)
{
    apply_style(view);
}

/* ── The probe interface ─────────────────────────────────────────────────── */

/*
 * key_command — the command macOS's standard key bindings send for a key,
 * so the probe drives the same dispatcher a real key press does.
 *
 * Inputs:
 *   key  — the key
 *   mods — on_mod_t bits
 *
 * Output: the command's selector, or NULL for a key that types.
 */
static SEL
key_command(on_key_t key, unsigned mods)
{
    BOOL shift   = (mods & on_mod_shift)   != 0;
    BOOL option  = (mods & on_mod_option)  != 0;
    BOOL command = (mods & on_mod_command) != 0;
    const char *base = NULL;
    switch (key) {
    case on_key_left:  base = command ? "moveToLeftEndOfLine"  : option ? "moveWordLeft"  : "moveLeft";  break;
    case on_key_right: base = command ? "moveToRightEndOfLine" : option ? "moveWordRight" : "moveRight"; break;
    case on_key_up:
        if (option) return shift ? @selector(moveParagraphBackwardAndModifySelection:)
                                 : @selector(moveToBeginningOfParagraph:);
        base = command ? "moveToBeginningOfDocument" : "moveUp";
        break;
    case on_key_down:
        if (option) return shift ? @selector(moveParagraphForwardAndModifySelection:)
                                 : @selector(moveToEndOfParagraph:);
        base = command ? "moveToEndOfDocument" : "moveDown";
        break;
    case on_key_home:            /* as home_end_command makes of the key */
        return shift ? @selector(moveToBeginningOfLineAndModifySelection:)
                     : @selector(moveToBeginningOfLine:);
    case on_key_end:
        return shift ? @selector(moveToEndOfLineAndModifySelection:)
                     : @selector(moveToEndOfLine:);
    case on_key_page_up:
        return shift ? @selector(pageUpAndModifySelection:) : @selector(scrollPageUp:);
    case on_key_page_down:
        return shift ? @selector(pageDownAndModifySelection:) : @selector(scrollPageDown:);
    case on_key_return:
        return @selector(insertNewline:);
    case on_key_backspace:
        return command ? @selector(deleteToBeginningOfLine:)
                       : option ? @selector(deleteWordBackward:) : @selector(deleteBackward:);
    case on_key_delete:
        return option ? @selector(deleteWordForward:) : @selector(deleteForward:);
    case on_key_tab:
        return shift ? @selector(insertBacktab:) : @selector(insertTab:);
    case on_key_escape:
        return @selector(cancelOperation:);
    case on_key_character:
        return NULL;
    }
    if (base == NULL) return NULL;
    char name[96];
    snprintf(name, sizeof name, shift ? "%sAndModifySelection:" : "%s:", base);
    return sel_registerName(name);
}

/*
 * on_note_view_feed_key — see note_view.h.
 */
BOOL
on_note_view_feed_key(NotesNoteView *view, on_key_t key, uint32_t character,
                      unsigned mods)
{
    SEL command = key_command(key, mods);
    if (command) return tag_popup_command(view, command) || run_command(view, command);

    /* ⌘-letters: the Edit menu's shortcuts. */
    if (mods & on_mod_command) {
        switch (character | 0x20) {
        case 'a': return run_command(view, @selector(selectAll:));
        case 'c': return run_command(view, @selector(copy:));
        case 'v': return run_command(view, @selector(paste:));
        case 'x': return run_command(view, @selector(cut:));
        case 'z': return run_command(view, (mods & on_mod_shift) ? @selector(redo:)
                                                                 : @selector(undo:));
        default:  return NO;
        }
    }

    /* A printable character, typed: as UTF-8. */
    uint32_t    le  = NSSwapHostIntToLittle(character);
    const char *u8  = [[NSString alloc] initWithBytes:&le length:sizeof le
                                             encoding:NSUTF32LittleEndianStringEncoding].UTF8String;
    size_t      n   = u8 != NULL ? strlen(u8) : 0;
    if (n == 0) return NO;
    insert_typed(view, u8, n);
    return YES;
}

/*
 * on_note_view_feed_text — see note_view.h.
 */
void
on_note_view_feed_text(NotesNoteView *view, const char *text)
{
    if (text && text[0])
        insert_typed(view, text, strlen(text));
}

/*
 * on_note_view_feed_click — see note_view.h.
 */
BOOL
on_note_view_feed_click(NotesNoteView *view, CGFloat x, CGFloat y,
                        NSInteger clicks, unsigned mods, BOOL secondary)
{
    if (secondary) {
        (void)context_menu(view, x, y);
        return YES;
    }
    return press_at(view, x, y, clicks, mods);
}

/*
 * on_note_view_caret — see note_view.h.
 */
OnPos
on_note_view_caret(NotesNoteView *view)
{
    return view.caret;
}
