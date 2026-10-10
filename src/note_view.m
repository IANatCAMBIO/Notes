/* ===========================================================================
 * note_view.m — the note editor's view (work package C)
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
#import "ui_util.h"

#include "doc_layout.h"
#include "look.h"
#include "note_edit.h"
#include "utf_index.h"

#include <string.h>
#include <time.h>

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
@property (nonatomic, copy) NSString *preedit;
@property (nonatomic, assign) size_t preedit_cursor; /* byte offset in preedit */

/* What was under the last context click (for context-menu actions). */
@property (nonatomic, assign) OnDocHitResult ctx_hit;

/* Layout style in effect. */
@property (nonatomic, assign) OnDocLayoutStyle style;

/* The key event keyDown: is interpreting (nil otherwise): which key a
 * command came from (home_end_command). */
@property (nonatomic, strong) NSEvent *key_event;

@end

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
 * doc_end — the position after the last character of the document.
 *
 * Inputs:
 *   doc — the document
 *
 * Output: the end position.
 */
static OnPos
doc_end(const OnDocument *doc)
{
    guint n = on_document_n_blocks(doc);
    if (n == 0)
        return (OnPos){ 0, -1, 0 };
    const OnBlock *b = on_document_block(doc, n - 1);
    OnPos p = { n - 1, -1, 0 };
    if (b->text != NULL)
        p.offset = b->text->text->len;
    else
        p.offset = 1;
    return p;
}

/*
 * clamp_pos — nearest valid position to p in the current document.
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
    if (n == 0)
        return (OnPos){ 0, -1, 0 };
    if (p.block >= n) {
        p = doc_end(doc);
        return p;
    }
    const OnBlock *b = on_document_block(doc, p.block);
    if (b->kind == ON_BLOCK_IMAGE) {
        p.cell   = -1;
        p.offset = MIN(p.offset, 1);
        return p;
    }
    if (b->kind == ON_BLOCK_TABLE) {
        gint cells = b->rows * b->cols;
        if (p.cell >= cells)
            p.cell = cells - 1;
        OnText *t = on_document_text_at(doc, p);
        if (t != NULL)
            p.offset = MIN(p.offset, t->text->len);
        return p;
    }
    /* Text block. */
    p.cell = -1;
    if (b->text != NULL)
        p.offset = MIN(p.offset, b->text->text->len);
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
        .dark         = on_ui_is_dark(v),
    };
    return s;
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
    v.style = s;
    on_doc_layout_set_style(v.layout, &s);
    on_doc_layout_set_width(v.layout, layout_width(v));
    NSSize sz = v.frame.size;
    sz.height = MAX(on_doc_layout_height(v.layout), 1.0);
    [v setFrameSize:sz];
    v.needsDisplay = YES;
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
        sv.needsDisplay = YES;
    }];
    v.needsDisplay = YES;
}

/*
 * im_sync_location — tell the input method where the caret is on screen,
 * so it can place its candidate window correctly.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
im_sync_location(NotesNoteView *v)
{
    /* NSTextInputClient handles this through firstRectForCharacterRange:. */
    (void)v;
}

/*
 * after_edit — call after every mutation: resize the view, scroll to the
 * caret, fire the edited handler, reset the blink.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
after_edit(NotesNoteView *v)
{
    double w = layout_width(v);
    on_doc_layout_set_width(v.layout, w);
    double h = on_doc_layout_height(v.layout);
    NSSize sz = v.frame.size;
    sz.height = MAX(h, 1.0);
    [v setFrameSize:sz];
    scroll_to_caret(v);
    v.needsDisplay = YES;
    im_sync_location(v);
    blink_restart(v);
    if (v.edited_handler) v.edited_handler();
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
    v.anchor = (OnPos){ 0, -1, 0 };
    v.caret  = doc_end(v.doc);
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
 * insert_typed — typed text at the caret: replaces a selection, carries
 * the armed inline flags, extends the typing group.
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
    OnPos p = v.caret;
    guint32 flags = v.inline_flags;
    /* Extend a #tag span if typing inside one. */
    OnText *t = caret_text(v);
    if (t != NULL && p.offset > 0 && p.offset <= t->text->len &&
        (on_text_flags_at(t, p.offset - 1) & ON_FMT_TAG) &&
        (p.offset == t->text->len || (on_text_flags_at(t, p.offset) & ON_FMT_TAG)))
        flags |= ON_FMT_TAG;
    if (on_document_insert_text(v.doc, p, text, n, flags)) {
        OnPos c = v.caret;
        c.offset += n;
        v.caret = c;
    }
    v.anchor = v.caret;
    /* Sentence enders cap a typing group. */
    if (memchr(text, '.', n) || memchr(text, '?', n))
        if (++v.sentences >= max_sentences) typing_end(v);
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
        BOOL list = b->kind == ON_BLOCK_BULLET || b->kind == ON_BLOCK_NUMBER ||
                    b->kind == ON_BLOCK_CHECK;
        if (list && b->text->text->len == 0) {
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
        BOOL list = b->kind == ON_BLOCK_BULLET || b->kind == ON_BLOCK_NUMBER ||
                    b->kind == ON_BLOCK_CHECK;
        if (list) {
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
 * page_move — Page Up or Page Down by a screen's height.
 *
 * Inputs:
 *   v      — the view
 *   down   — YES for Page Down
 *   extend — YES when Shift is held
 *
 * Output: none.
 */
static void
page_move(NotesNoteView *v, BOOL down, BOOL extend)
{
    NSScrollView *sv = (NSScrollView *)v.enclosingScrollView;
    double page = sv ? sv.documentVisibleRect.size.height : 400.0;
    on_rect_t r;
    on_doc_layout_caret_rect(v.layout, v.caret, &r);
    double target_y = r.y + (down ? page : -page);
    if (v.goal_x < 0)
        v.goal_x = r.x;
    OnDocHitResult hit;
    on_doc_layout_hit(v.layout, v.goal_x, MAX(0, target_y), &hit);
    double gx = v.goal_x;
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
 * insert_block_at_caret — insert an object block at the caret position.
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
 * copy_selection — copy the selected range to the pasteboard as BNBF and
 * plain text.
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
    on_document_free(frag);

    NSPasteboard *pb = v.pasteboard;
    [pb clearContents];
    NSData *data = [NSData dataWithBytesNoCopy:blob length:n freeWhenDone:NO];
    [pb setData:data forType:on_pasteboard_type_fragment];
    [pb setString:@(text) forType:NSPasteboardTypeString];
    free(blob);
    g_free(text);
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
 * paste — paste from the pasteboard: prefer our BNBF type, then PNG, then
 * plain text.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
paste(NotesNoteView *v)
{
    NSPasteboard *pb = v.pasteboard;
    /* Prefer our fragment type. */
    NSData *frag_data = [pb dataForType:on_pasteboard_type_fragment];
    if (frag_data) {
        OnDocument *frag = on_document_from_bnbf(
            (const guint8 *)frag_data.bytes, frag_data.length, NULL);
        if (frag) {
            paste_fragment(v, frag, NO);
            on_document_free(frag);
            return;
        }
    }
    /* PNG image. */
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (!png) png = [pb dataForType:NSPasteboardTypeTIFF];
    if (png) {
        GBytes *gbytes = g_bytes_new(png.bytes, png.length);
        OnDocument *frag = on_document_new_from_block(on_block_new_image(gbytes, 0));
        g_bytes_unref(gbytes);
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

/* ── Linear index (NSTextInputClient needs UTF-16) ──────────────────────── */

/*
 * pos_to_utf16 — convert an OnPos to a UTF-16 code-unit offset in the
 * linear text representation.
 *
 * Inputs:
 *   doc — the document
 *   pos — the position
 *
 * Output: the UTF-16 offset.
 */
static NSUInteger
pos_to_utf16(const OnDocument *doc, OnPos pos)
{
    size_t byte_off = 0;
    guint n = on_document_n_blocks(doc);
    for (guint i = 0; i < n && i <= pos.block; i++) {
        const OnBlock *b = on_document_block(doc, i);
        if (i == pos.block) {
            /* Add the byte offset within this block. */
            const char *text = (b->text != NULL) ? b->text->text->str : "";
            size_t add = (pos.cell < 0) ? pos.offset : pos.offset;
            byte_off += on_utf16_from_utf8(text, add);
            break;
        }
        /* Add the block's full text + separator. */
        if (b->text != NULL)
            byte_off += on_utf16_from_utf8(b->text->text->str,
                                           b->text->text->len);
        else
            byte_off += 1;
        byte_off++;                 /* newline separator */
    }
    return (NSUInteger)byte_off;
}

/* ── NSTextInputClient linear UTF-8 for attributedSubstring ─────────────── */

/*
 * linear_utf8 — the document as plain UTF-8 (one line per block, newline-
 * separated), borrowed from the caller's buffer.  Caller g_free()s.
 *
 * Inputs:
 *   doc — the document
 *
 * Output: UTF-8 string (g_free it).
 */
static gchar *
linear_utf8(const OnDocument *doc)
{
    return on_document_plain_text(doc);
}

/* ── find hits ───────────────────────────────────────────────────────────── */

/*
 * compute_hits — find case-insensitive hits of needle in the document.
 *
 * Inputs:
 *   doc    — the document
 *   needle — the string to find (nil or "" clears)
 *
 * Output: newly allocated array of OnDocHit (g_free), count in *n_out.
 */
static OnDocHit *
compute_hits(const OnDocument *doc, NSString *needle, size_t *n_out)
{
    *n_out = 0;
    if (!needle || needle.length == 0)
        return NULL;
    const char *n8 = needle.UTF8String;
    if (!n8) return NULL;
    size_t nl = strlen(n8);

    /* Simple case-insensitive search block by block. */
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(OnDocHit));
    guint nb = on_document_n_blocks(doc);
    for (guint i = 0; i < nb; i++) {
        const OnBlock *b = on_document_block(doc, i);
        if (b->text == NULL) continue;
        const char *hay = b->text->text->str;
        size_t hl = b->text->text->len;
        for (size_t j = 0; j + nl <= hl; j++) {
            if (g_ascii_strncasecmp(hay + j, n8, nl) == 0) {
                OnDocHit h = { .block = i, .cell = -1, .start = j, .len = nl };
                g_array_append_val(arr, h);
            }
        }
    }
    *n_out = arr->len;
    OnDocHit *hits = (OnDocHit *)g_array_free(arr, FALSE);
    return hits;
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
    GString *text = g_string_new(NULL);
    for (guint i = first; i < first + count; i++) {
        if (i > first) g_string_append_c(text, '\n');
        g_string_append(text, on_document_block(v.doc, i)->text->text->str);
    }
    [v.pasteboard clearContents];
    [v.pasteboard setString:@(text->str) forType:NSPasteboardTypeString];
    g_string_free(text, TRUE);
    if (v.app) on_app_status(v.app, "Code copied");
}

/*
 * press_at — a primary press at a point in the view: THE click handling,
 * for mouseDown: and the probe alike (GTK: press_at).  One click on a
 * code run's "copy" word copies the run, on a task's box ticks it, on an
 * image opens it; otherwise one click places the caret (Shift extends) and
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
    OnDocHitResult hit;
    on_doc_layout_hit(v.layout, x, y, &hit);
    typing_end(v);
    v.dragging = NO;

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
    OnDocHitResult hit = v.ctx_hit;
    OnBlock *b = on_document_block(v.doc, hit.block);
    if (b == NULL) return 0;
    if (!hit.inline_image) return b->display_width;
    for (guint k = 0; b->text != NULL && k < b->text->images->len; k++) {
        const OnInlineImage *img = &g_array_index(b->text->images, OnInlineImage, k);
        if (img->offset == hit.pos.offset) return img->display_width;
    }
    return 0;
}

/*
 * context_menu — the menu for what a secondary click landed on (GTK:
 * context_menu): image items on an image, table items on a table, then
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
    if (!has_selection(v) || hit.kind != ON_HIT_TEXT) set_caret(v, hit.pos, NO);
    v.ctx_hit = hit;

    NSMenu  *menu = [[NSMenu alloc] initWithTitle:@""];
    OnBlock *b    = on_document_block(v.doc, hit.block);
    if (hit.kind == ON_HIT_IMAGE) {
        [menu addItemWithTitle:@"Copy Image" action:@selector(image_copy:) keyEquivalent:@""];
        [menu addItemWithTitle:@"Open"       action:@selector(image_open:) keyEquivalent:@""];
        CGImageRef image = on_note_view_image(v, hit.image_ord);
        guint32    shown = context_image_width(v);
        BOOL full = image != NULL && shown > 0 && shown >= CGImageGetWidth(image);
        [menu addItemWithTitle:full ? @"Display as Thumbnail" : @"Display Full Size"
                        action:full ? @selector(image_thumbnail:) : @selector(image_full_size:)
                 keyEquivalent:@""];
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
 * updateTrackingAreas — a single tracking area covering the view.
 *
 * Output: none.
 */
- (void)
updateTrackingAreas
{
    for (NSTrackingArea *ta in self.trackingAreas.copy)
        [self removeTrackingArea:ta];
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited |
                                 NSTrackingMouseMoved |
                                 NSTrackingActiveInKeyWindow;
    NSTrackingArea *ta = [[NSTrackingArea alloc]
        initWithRect:self.bounds options:opts owner:self userInfo:nil];
    [self addTrackingArea:ta];
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

/*
 * drawRect: — fill the page, then delegate to doc_layout.
 *
 * Inputs:
 *   dirty — the clip rectangle (the full bounds is redrawn)
 *
 * Output: none.
 */
- (void)
drawRect:(NSRect)dirty
{
    CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;

    /* Page background. */
    NSColor *page = on_ui_color(on_look_page);
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
    GBytes *png = on_note_view_image_png(self, _ctx_hit.image_ord);
    if (png == NULL) return;
    gsize n;
    const void *bytes = g_bytes_get_data(png, &n);
    on_ui_open_image_external([NSData dataWithBytes:bytes length:n]);
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
    gint inline_ord = -1;
    if (_ctx_hit.inline_image) {
        OnBlock *b = on_document_block(_doc, _ctx_hit.block);
        for (guint k = 0; b != NULL && b->text != NULL && k < b->text->images->len; k++)
            if (g_array_index(b->text->images, OnInlineImage, k).offset == _ctx_hit.pos.offset)
                inline_ord = (gint)k;
        if (inline_ord < 0) return;
    }
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
    CGImageRef image = on_note_view_image(self, _ctx_hit.image_ord);
    if (image) [self set_context_image_width:(guint32)CGImageGetWidth(image)];
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
    if (command == @selector(table_row_add:))         did = on_document_table_insert_row(_doc, _ctx_hit.block, r + 1);
    else if (command == @selector(table_row_remove:)) did = on_document_table_remove_row(_doc, _ctx_hit.block, r);
    else if (command == @selector(table_col_add:))    did = on_document_table_insert_col(_doc, _ctx_hit.block, c + 1);
    else if (command == @selector(table_col_remove:)) did = on_document_table_remove_col(_doc, _ctx_hit.block, c);
    else {
        OnPos a = { _ctx_hit.block, -1, 0 }, e = { _ctx_hit.block, -1, 1 }, out;
        did = on_document_delete_range(_doc, a, e, &out);
        if (did) _caret = _anchor = clamp_pos(_doc, out);
    }
    if (did) after_edit(self);
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
    if (!run_command(self, home_end_command(self.key_event, sel))) [super doCommandBySelector:sel];
}

/* ── NSTextInputClient ───────────────────────────────────────────────────── */

/*
 * insertText:replacementRange: — committed text from the input method or
 * direct typing.  Convert the replacement range from UTF-16 to document
 * positions, delete it, then insert the text.
 *
 * Inputs:
 *   string           — NSString or NSAttributedString
 *   replacementRange — UTF-16 range in the "document string"
 *
 * Output: none.
 */
- (void)
insertText:(id)string replacementRange:(NSRange)replacementRange
{
    NSString *text = [string isKindOfClass:[NSAttributedString class]]
                   ? ((NSAttributedString *)string).string : string;

    /* If the range is not {NSNotFound, 0}, it overrides the selection. */
    if (replacementRange.location != NSNotFound) {
        /* Convert the UTF-16 range to document positions and select it. */
        gchar *plain = linear_utf8(_doc);
        size_t blen  = plain ? strlen(plain) : 0;
        size_t ba = on_utf8_from_utf16(plain ? plain : "", blen,
                                       replacementRange.location);
        size_t bb = on_utf8_from_utf16(plain ? plain : "", blen,
                                       replacementRange.location + replacementRange.length);
        g_free(plain);
        /* Walk to the positions (simplified: treat doc as linear for now). */
        (void)ba; (void)bb;
        /* Fall through to insert at the current caret after clearing. */
    }

    /* Clear any preedit. */
    if (_preedit) {
        on_doc_layout_set_preedit(_layout, _caret, NULL, 0);
        _preedit = nil;
    }

    if (text.length == 0) return;
    const char *u8 = text.UTF8String;
    if (!u8) return;
    insert_typed(self, u8, strlen(u8));
}

/*
 * setMarkedText:selectedRange:replacementRange: — IME preedit text.
 *
 * Inputs:
 *   string           — the preedit string
 *   selectedRange    — selected range within the preedit (ignored)
 *   replacementRange — range in the document to replace (ignored here)
 *
 * Output: none.
 */
- (void)
setMarkedText:(id)string
 selectedRange:(NSRange)selectedRange
replacementRange:(NSRange)replacementRange
{
    (void)selectedRange; (void)replacementRange;
    NSString *text = [string isKindOfClass:[NSAttributedString class]]
                   ? ((NSAttributedString *)string).string : string;
    _preedit = (text.length > 0) ? text : nil;
    const char *p8 = _preedit ? _preedit.UTF8String : NULL;
    on_doc_layout_set_preedit(_layout, _caret, p8, 0);
    self.needsDisplay = YES;
}

/*
 * unmarkText — commit the current preedit.
 *
 * Output: none.
 */
- (void)
unmarkText
{
    if (_preedit) {
        NSString *text = _preedit;
        _preedit = nil;
        on_doc_layout_set_preedit(_layout, _caret, NULL, 0);
        const char *u8 = text.UTF8String;
        if (u8 && strlen(u8) > 0)
            insert_typed(self, u8, strlen(u8));
    }
}

/*
 * selectedRange — the current selection in UTF-16 code units.
 *
 * Output: the range; {NSNotFound, 0} when there is no selection.
 */
- (NSRange)
selectedRange
{
    if (!has_selection(self)) {
        NSUInteger loc = pos_to_utf16(_doc, _caret);
        return NSMakeRange(loc, 0);
    }
    OnPos a, b;
    sel_bounds(self, &a, &b);
    NSUInteger ua = pos_to_utf16(_doc, a);
    NSUInteger ub = pos_to_utf16(_doc, b);
    return NSMakeRange(ua, ub - ua);
}

/*
 * markedRange — the preedit range; {NSNotFound, 0} when none.
 *
 * Output: the range.
 */
- (NSRange)
markedRange
{
    return NSMakeRange(NSNotFound, 0);
}

/*
 * hasMarkedText — whether preedit is active.
 *
 * Output: YES when there is preedit text.
 */
- (BOOL)
hasMarkedText
{
    return _preedit != nil;
}

/*
 * attributedSubstringForProposedRange:actualRange: — a substring for the
 * spell checker and accessibility.
 *
 * Inputs:
 *   range       — the UTF-16 range requested
 *   actualRange — receives the range actually returned (may be nil)
 *
 * Output: the attributed string, or nil.
 */
- (NSAttributedString *)
attributedSubstringForProposedRange:(NSRange)range
                        actualRange:(NSRangePointer)actualRange
{
    gchar *plain = linear_utf8(_doc);
    if (!plain) {
        if (actualRange) *actualRange = NSMakeRange(0, 0);
        return nil;
    }
    NSString *str = @(plain);
    g_free(plain);
    NSRange clamped = NSMakeRange(MIN(range.location, str.length),
                                  0);
    clamped.length = MIN(range.length, str.length - clamped.location);
    if (actualRange) *actualRange = clamped;
    if (clamped.length == 0) return nil;
    NSString *sub = [str substringWithRange:clamped];
    return [[NSAttributedString alloc] initWithString:sub];
}

/*
 * validAttributesForMarkedText — attributes we understand in preedit.
 *
 * Output: empty array (we handle preedit as plain text only).
 */
- (NSArray<NSAttributedStringKey> *)
validAttributesForMarkedText
{
    return @[];
}

/*
 * firstRectForCharacterRange:actualRange: — the caret rectangle in screen
 * coordinates, for the IME window.
 *
 * Inputs:
 *   range       — the character range
 *   actualRange — receives the range actually used
 *
 * Output: the screen rectangle.
 */
- (NSRect)
firstRectForCharacterRange:(NSRange)range
               actualRange:(NSRangePointer)actualRange
{
    (void)range;
    if (actualRange) *actualRange = self.selectedRange;
    on_rect_t r;
    on_doc_layout_caret_rect(_layout, _caret, &r);
    NSRect view_rect = NSMakeRect(r.x, r.y, r.w, r.h);
    return [self.window convertRectToScreen:[self convertRect:view_rect toView:nil]];
}

/*
 * characterIndexForPoint: — the UTF-16 index nearest a screen point.
 *
 * Inputs:
 *   point — screen coordinates
 *
 * Output: the UTF-16 index.
 */
- (NSUInteger)
characterIndexForPoint:(NSPoint)point
{
    NSPoint vpt = [self convertPoint:[self.window convertPointFromScreen:point]
                            fromView:nil];
    OnDocHitResult hit;
    on_doc_layout_hit(_layout, vpt.x, vpt.y, &hit);
    return pos_to_utf16(_doc, hit.pos);
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
 * on_note_view_load_document — see note_view.h.
 */
void
on_note_view_load_document(NotesNoteView *view, OnDocument *doc)
{
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

    /* The caret starts on the first line of TEXT: under the first-line-
     * title setting block 0 is the title, so an existing note opens ready
     * to be read or added to, not renamed (GTK: load_document). */
    OnPos start = { 0, -1, 0 };
    if (view.app->first_line_title && on_document_n_blocks(doc) > 1) start.block = 1;
    view.caret = view.anchor = clamp_pos(doc, start);
    on_document_take_tags_modified(doc);
    on_document_take_actions_modified(doc);
    if (view.find_text != nil) on_note_view_find(view, view.find_text);
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
    view.caret = clamp_pos(view.doc, view.caret);
    view.anchor = view.caret;
    after_edit(view);
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
            if (blk->text == NULL) continue;
            if (i == a.block && a.cell >= 0 && a.cell == b.cell) {
                on_document_set_flags(view.doc, a, b.offset - a.offset, flag, !has);
                break;
            }
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
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[16];
    strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    insert_typed(view, buf, strlen(buf));
}

/*
 * on_note_view_undo — see note_view.h.
 */
void
on_note_view_undo(NotesNoteView *view)
{
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
    size_t n;
    OnDocHit *hits = compute_hits(view.doc, view.find_text, &n);
    on_doc_layout_set_hits(view.layout, hits, n);
    g_free(hits);
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
    if (n == 0) { g_free(hits); return; }
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
    g_free(hits);
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
 * on_note_view_image — see note_view.h.
 */
CGImageRef
on_note_view_image(NotesNoteView *view, NSInteger ordinal)
{
    /* Walk blocks to find the ordinal-th image block. */
    gint ord = 0;
    guint nb = on_document_n_blocks(view.doc);
    for (guint i = 0; i < nb; i++) {
        const OnBlock *b = on_document_block(view.doc, i);
        if (b->kind == ON_BLOCK_IMAGE) {
            if (ord == (gint)ordinal)
                return on_doc_layout_image(view.layout, i, -1);
            ord++;
        }
    }
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
 * on_note_view_image_reveal — see note_view.h.
 */
BOOL
on_note_view_image_reveal(NotesNoteView *view, NSInteger ordinal)
{
    gint ord = 0;
    guint nb = on_document_n_blocks(view.doc);
    for (guint i = 0; i < nb; i++) {
        const OnBlock *b = on_document_block(view.doc, i);
        if (b->kind == ON_BLOCK_IMAGE) {
            if (ord == (gint)ordinal) {
                OnPos p = { i, -1, 0 };
                set_caret(view, p, NO);
                scroll_to_caret(view);
                return YES;
            }
            ord++;
        }
    }
    return NO;
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
    OnPos p = { block_idx, -1, 0 };
    set_caret(view, p, NO);
    scroll_to_caret(view);
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
 * on_note_view_feed_key — see note_view.h.
 */
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
    if (command) return run_command(view, command);

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

    /* A printable character, typed: encode as UTF-8. */
    char buf[8] = { 0 };
    if (character < 0x80) {
        buf[0] = (char)character;
    } else if (character < 0x800) {
        buf[0] = (char)(0xC0 | (character >> 6));
        buf[1] = (char)(0x80 | (character & 0x3F));
    } else if (character < 0x10000) {
        buf[0] = (char)(0xE0 | (character >> 12));
        buf[1] = (char)(0x80 | ((character >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (character & 0x3F));
    } else {
        buf[0] = (char)(0xF0 | (character >> 18));
        buf[1] = (char)(0x80 | ((character >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((character >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (character & 0x3F));
    }
    size_t n = strlen(buf);
    if (n == 0) return NO;
    insert_typed(view, buf, n);
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
