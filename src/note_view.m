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
 * caret_ensure_text — if the caret is beside an image/table, insert a
 * paragraph after it and move the caret there.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
caret_ensure_text(NotesNoteView *v)
{
    const OnBlock *b = caret_block(v);
    if (b->text != NULL)
        return;
    guint at = v.caret.block + (v.caret.offset > 0 ? 1 : 0);
    on_document_insert_block(v.doc, at, on_block_new_text(ON_BLOCK_PARA));
    OnPos p = { at, -1, 0 };
    v.caret = v.anchor = p;
}

/* ── Layout width ────────────────────────────────────────────────────────── */

/*
 * layout_text_width — the width available for text inside the view margins.
 *
 * Inputs:
 *   v — the view
 *
 * Output: points.
 */
static double
layout_text_width(NotesNoteView *v)
{
    double w = v.frame.size.width - on_look_margin_left - on_look_margin_right;
    return MAX(w, 1.0);
}

/* ── Style helpers ───────────────────────────────────────────────────────── */

/*
 * current_style — the layout style from the app's config and appearance.
 *
 * Inputs:
 *   v — the view
 *
 * Output: the style.
 */
static OnDocLayoutStyle
current_style(NotesNoteView *v)
{
    OnDocLayoutStyle s = { 0 };
    s.dark = on_ui_is_dark(v);
    /* title_line, code_numbers, code_copy — defaults off (config wires
     * these when settings_changed fires; for now they are always off). */
    return s;
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
 * blink_restart — restart the caret blink from the "on" state.
 *
 * Inputs:
 *   v — the view
 *
 * Output: none.
 */
static void
blink_restart(NotesNoteView *v)
{
    [v.blink_timer invalidate];
    v.caret_on = YES;
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
    double w = layout_text_width(v);
    on_doc_layout_set_width(v.layout, w);
    double h = on_doc_layout_height(v.layout) + on_look_margin_top + on_look_margin_bottom;
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
 * set_caret — move the caret; extend the selection if extend is YES,
 * otherwise collapse anchor to caret.
 *
 * Inputs:
 *   v      — the view
 *   p      — the new caret position
 *   extend — YES to extend, NO to collapse
 *
 * Output: none.
 */
static void
set_caret(NotesNoteView *v, OnPos p, BOOL extend)
{
    v.caret = p;
    if (!extend)
        v.anchor = p;
    v.needsDisplay = YES;
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
 * delete_key — Backspace or Delete; optionally word-wise.
 *
 * Inputs:
 *   v        — the view
 *   backward — YES for Backspace
 *   word     — YES for word deletion
 *
 * Output: none.
 */
static void
delete_key(NotesNoteView *v, BOOL backward, BOOL word)
{
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
            OnPos q = on_doc_layout_move(v.layout, p,
                                        backward ? ON_MOVE_CHAR_LEFT : ON_MOVE_CHAR_RIGHT,
                                        &gx);
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
    OnDocMove how = backward ? (word ? ON_MOVE_WORD_LEFT  : ON_MOVE_CHAR_LEFT)
                             : (word ? ON_MOVE_WORD_RIGHT : ON_MOVE_CHAR_RIGHT);
    OnPos q = on_doc_layout_move(v.layout, p, how, &gx);
    /* Never cross a block boundary in a single-char delete. */
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

    NSPasteboard *pb = [NSPasteboard generalPasteboard];
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
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
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
    on_document_set_observer(_doc, &OBSERVER, (__bridge void *)self);
    [self updateTrackingAreas];
    return self;
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
 * resignFirstResponder — stop blinking, keep caret visible.
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
        on_doc_layout_set_width(_layout, layout_text_width(self));
        double h = on_doc_layout_height(_layout) + on_look_margin_top + on_look_margin_bottom;
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

    /* Update style if appearance changed. */
    OnDocLayoutStyle s = current_style(self);
    if (memcmp(&s, &_style, sizeof s) != 0) {
        _style = s;
        on_doc_layout_set_style(_layout, &_style);
    }

    /* Translate so doc_layout coordinates have margin_left, margin_top
     * as their origin. */
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, on_look_margin_left, on_look_margin_top);

    BOOL focused = (self.window.firstResponder == self);
    OnPos sel_a, sel_b;
    sel_bounds(self, &sel_a, &sel_b);

    on_rect_t clip = {
        .x = dirty.origin.x - on_look_margin_left,
        .y = dirty.origin.y - on_look_margin_top,
        .w = dirty.size.width,
        .h = dirty.size.height,
    };

    OnDocPaint paint = {
        .clip         = clip,
        .has_sel      = has_selection(self),
        .sel_a        = sel_a,
        .sel_b        = sel_b,
        .caret_visible = _caret_on,
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
    double x = pt.x - on_look_margin_left;
    double y = pt.y - on_look_margin_top;
    on_doc_layout_hit(_layout, x, y, out);
}

/*
 * mouseDown: — begin a click or drag.
 *
 * Inputs:
 *   event — the mouse-down event
 *
 * Output: none.
 */
- (void)
mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self];
    OnDocHitResult hit;
    [self hit_at_event:event out:&hit];
    _ctx_hit = hit;

    NSInteger clicks = event.clickCount;
    BOOL extend = (event.modifierFlags & NSEventModifierFlagShift) != 0;

    if (hit.kind == ON_HIT_IMAGE && !extend) {
        /* Image click: fire the handler and position caret beside it. */
        if (self.image_activated_handler)
            self.image_activated_handler(hit.image_ord);
        set_caret(self, hit.pos, NO);
        self.needsDisplay = YES;
        return;
    }
    if (hit.kind == ON_HIT_CHECKBOX && !extend) {
        /* Toggle the task checkbox. */
        const OnBlock *b = on_document_block(_doc, hit.block);
        on_document_set_kind(_doc, hit.block,
                             b->kind == ON_BLOCK_CHECK ? ON_BLOCK_CHECK : ON_BLOCK_CHECK);
        /* Actually just set the checked state (block remains CHECK kind). */
        /* The checkbox state is in eol_flags bit 0 in this build. */
        (void)b;
        after_edit(self);
        return;
    }

    if (clicks == 3) {
        /* Triple click: select the line. */
        OnPos a, b2;
        on_doc_layout_line_at(_layout, hit.pos, &a, &b2);
        _anchor = a;
        set_caret(self, b2, YES);
    } else if (clicks == 2) {
        /* Double click: select the word. */
        OnPos a, b2;
        on_doc_layout_word_at(_layout, hit.pos, &a, &b2);
        _anchor = a;
        set_caret(self, b2, YES);
    } else {
        if (extend) {
            set_caret(self, hit.pos, YES);
        } else {
            set_caret(self, hit.pos, NO);
        }
        _dragging   = YES;
        _drag_anchor = _anchor;
    }
    adopt_flags(self);
    self.needsDisplay = YES;
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
    [self interpretKeyEvents:@[event]];
}

/*
 * doCommandBySelector: — AppKit sends this for movement and edit commands
 * after the input method passes on the key.
 *
 * Inputs:
 *   sel — the selector for the command
 *
 * Output: none.
 */
- (void)
doCommandBySelector:(SEL)sel
{
    NSEventModifierFlags mods = NSApp.currentEvent.modifierFlags;
    BOOL shift   = (mods & NSEventModifierFlagShift)   != 0;
    BOOL option  = (mods & NSEventModifierFlagOption)  != 0;   /* word */
    BOOL command = (mods & NSEventModifierFlagCommand) != 0;   /* line/doc */

    if (sel == @selector(moveLeft:) || sel == @selector(moveBackward:)) {
        move_caret(self, command ? ON_MOVE_LINE_START
                                 : option ? ON_MOVE_WORD_LEFT : ON_MOVE_CHAR_LEFT, NO);
    } else if (sel == @selector(moveRight:) || sel == @selector(moveForward:)) {
        move_caret(self, command ? ON_MOVE_LINE_END
                                 : option ? ON_MOVE_WORD_RIGHT : ON_MOVE_CHAR_RIGHT, NO);
    } else if (sel == @selector(moveUp:)) {
        move_caret(self, command ? ON_MOVE_DOC_START : ON_MOVE_LINE_UP, NO);
    } else if (sel == @selector(moveDown:)) {
        move_caret(self, command ? ON_MOVE_DOC_END : ON_MOVE_LINE_DOWN, NO);
    } else if (sel == @selector(moveLeftAndModifySelection:)) {
        move_caret(self, command ? ON_MOVE_LINE_START
                                 : option ? ON_MOVE_WORD_LEFT : ON_MOVE_CHAR_LEFT, YES);
    } else if (sel == @selector(moveRightAndModifySelection:)) {
        move_caret(self, command ? ON_MOVE_LINE_END
                                 : option ? ON_MOVE_WORD_RIGHT : ON_MOVE_CHAR_RIGHT, YES);
    } else if (sel == @selector(moveUpAndModifySelection:)) {
        move_caret(self, command ? ON_MOVE_DOC_START : ON_MOVE_LINE_UP, YES);
    } else if (sel == @selector(moveDownAndModifySelection:)) {
        move_caret(self, command ? ON_MOVE_DOC_END : ON_MOVE_LINE_DOWN, YES);
    } else if (sel == @selector(moveWordLeft:) || sel == @selector(moveWordBackward:)) {
        move_caret(self, ON_MOVE_WORD_LEFT, NO);
    } else if (sel == @selector(moveWordRight:) || sel == @selector(moveWordForward:)) {
        move_caret(self, ON_MOVE_WORD_RIGHT, NO);
    } else if (sel == @selector(moveWordLeftAndModifySelection:)) {
        move_caret(self, ON_MOVE_WORD_LEFT, YES);
    } else if (sel == @selector(moveWordRightAndModifySelection:)) {
        move_caret(self, ON_MOVE_WORD_RIGHT, YES);
    } else if (sel == @selector(moveToBeginningOfLine:)) {
        move_caret(self, ON_MOVE_LINE_START, NO);
    } else if (sel == @selector(moveToEndOfLine:)) {
        move_caret(self, ON_MOVE_LINE_END, NO);
    } else if (sel == @selector(moveToBeginningOfLineAndModifySelection:)) {
        move_caret(self, ON_MOVE_LINE_START, YES);
    } else if (sel == @selector(moveToEndOfLineAndModifySelection:)) {
        move_caret(self, ON_MOVE_LINE_END, YES);
    } else if (sel == @selector(moveToBeginningOfDocument:)) {
        move_caret(self, ON_MOVE_DOC_START, NO);
    } else if (sel == @selector(moveToEndOfDocument:)) {
        move_caret(self, ON_MOVE_DOC_END, NO);
    } else if (sel == @selector(moveToBeginningOfDocumentAndModifySelection:)) {
        move_caret(self, ON_MOVE_DOC_START, YES);
    } else if (sel == @selector(moveToEndOfDocumentAndModifySelection:)) {
        move_caret(self, ON_MOVE_DOC_END, YES);
    } else if (sel == @selector(scrollPageUp:)) {
        page_move(self, NO, NO);
    } else if (sel == @selector(scrollPageDown:)) {
        page_move(self, YES, NO);
    } else if (sel == @selector(pageUpAndModifySelection:)) {
        page_move(self, NO, YES);
    } else if (sel == @selector(pageDownAndModifySelection:)) {
        page_move(self, YES, YES);
    } else if (sel == @selector(insertNewline:)) {
        enter_pressed(self);
    } else if (sel == @selector(deleteBackward:)) {
        delete_key(self, YES, NO);
    } else if (sel == @selector(deleteForward:)) {
        delete_key(self, NO, NO);
    } else if (sel == @selector(deleteWordBackward:)) {
        delete_key(self, YES, YES);
    } else if (sel == @selector(deleteWordForward:)) {
        delete_key(self, NO, YES);
    } else if (sel == @selector(insertTab:)) {
        tab_pressed(self, NO);
    } else if (sel == @selector(insertBacktab:)) {
        tab_pressed(self, YES);
    } else if (sel == @selector(cancelOperation:)) {
        if (has_selection(self)) {
            set_caret(self, _caret, NO);
        }
    } else if (sel == @selector(selectAll:)) {
        select_all(self);
    } else if (sel == @selector(copy:)) {
        copy_selection(self);
    } else if (sel == @selector(cut:)) {
        if (has_selection(self)) {
            copy_selection(self);
            typing_end(self);
            delete_selection(self);
            after_edit(self);
        }
    } else if (sel == @selector(paste:)) {
        paste(self);
    } else if (sel == @selector(undo:)) {
        on_note_view_undo(self);
    } else if (sel == @selector(redo:)) {
        on_note_view_redo(self);
    } else {
        [super doCommandBySelector:sel];
    }
    (void)shift;
    (void)option;
    (void)command;
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
    NSRect view_rect = NSMakeRect(r.x + on_look_margin_left,
                                  r.y + on_look_margin_top,
                                  r.w, r.h);
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
    double x = vpt.x - on_look_margin_left;
    double y = vpt.y - on_look_margin_top;
    OnDocHitResult hit;
    on_doc_layout_hit(_layout, x, y, &hit);
    return pos_to_utf16(_doc, hit.pos);
}

/* ── NSView copy/paste/undo selectors ───────────────────────────────────── */

- (void) copy:(id)sender  { (void)sender; copy_selection(self); }
- (void) cut:(id)sender   { (void)sender;
    if (!has_selection(self)) return;
    copy_selection(self); typing_end(self); delete_selection(self); after_edit(self); }
- (void) paste:(id)sender { (void)sender; paste(self); }
- (void) selectAll:(id)sender { (void)sender; select_all(self); }
- (void) undo:(id)sender  { (void)sender; on_note_view_undo(self); }
- (void) redo:(id)sender  { (void)sender; on_note_view_redo(self); }

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
    on_doc_layout_set_width(view.layout, layout_text_width(view));
    OnDocLayoutStyle s = current_style(view);
    view.style = s;
    on_doc_layout_set_style(view.layout, &s);
    double h = on_doc_layout_height(view.layout) + on_look_margin_top + on_look_margin_bottom;
    NSSize sz = view.frame.size;
    sz.height = MAX(h, 1.0);
    [view setFrameSize:sz];
    view.needsDisplay = YES;
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
    /* Find the next hit after the caret. */
    size_t pick = forward ? 0 : n - 1;
    for (size_t i = 0; i < n; i++) {
        OnPos p = { hits[i].block, hits[i].cell, hits[i].start };
        int cmp = on_pos_cmp(p, view.caret);
        if (forward && cmp > 0) { pick = i; break; }
        if (!forward && cmp < 0) pick = i;
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
    OnDocLayoutStyle s = current_style(view);
    view.style = s;
    on_doc_layout_set_style(view.layout, &s);
    view.needsDisplay = YES;
}

/* ── The probe interface ─────────────────────────────────────────────────── */

/*
 * on_note_view_feed_key — see note_view.h.
 */
BOOL
on_note_view_feed_key(NotesNoteView *view, on_key_t key, uint32_t character,
                      unsigned mods)
{
    BOOL shift   = (mods & on_mod_shift)   != 0;
    BOOL option  = (mods & on_mod_option)  != 0;
    BOOL command = (mods & on_mod_command) != 0;

    switch (key) {
    case on_key_left:
        move_caret(view, command ? ON_MOVE_LINE_START
                                 : option ? ON_MOVE_WORD_LEFT : ON_MOVE_CHAR_LEFT, shift);
        return YES;
    case on_key_right:
        move_caret(view, command ? ON_MOVE_LINE_END
                                 : option ? ON_MOVE_WORD_RIGHT : ON_MOVE_CHAR_RIGHT, shift);
        return YES;
    case on_key_up:
        move_caret(view, command ? ON_MOVE_DOC_START : ON_MOVE_LINE_UP, shift);
        return YES;
    case on_key_down:
        move_caret(view, command ? ON_MOVE_DOC_END : ON_MOVE_LINE_DOWN, shift);
        return YES;
    case on_key_home:
        move_caret(view, command ? ON_MOVE_DOC_START : ON_MOVE_LINE_START, shift);
        return YES;
    case on_key_end:
        move_caret(view, command ? ON_MOVE_DOC_END : ON_MOVE_LINE_END, shift);
        return YES;
    case on_key_page_up:
        page_move(view, NO, shift);
        return YES;
    case on_key_page_down:
        page_move(view, YES, shift);
        return YES;
    case on_key_return:
        enter_pressed(view);
        return YES;
    case on_key_backspace:
        delete_key(view, YES, option);
        return YES;
    case on_key_delete:
        delete_key(view, NO, option);
        return YES;
    case on_key_tab:
        tab_pressed(view, shift);
        return YES;
    case on_key_escape:
        if (has_selection(view)) {
            set_caret(view, view.caret, NO);
            return YES;
        }
        return NO;
    case on_key_character: {
        /* Command+key: standard AppKit shortcuts. */
        if (command) {
            uint32_t lo = character | 0x20; /* lowercase */
            if (lo == 'a') { select_all(view); return YES; }
            if (lo == 'c') { copy_selection(view); return YES; }
            if (lo == 'v') { paste(view); return YES; }
            if (lo == 'x') {
                if (!has_selection(view)) return NO;
                copy_selection(view);
                typing_end(view);
                delete_selection(view);
                after_edit(view);
                return YES;
            }
            if (lo == 'z') {
                if (shift) on_note_view_redo(view); else on_note_view_undo(view);
                return YES;
            }
            return NO;
        }
        /* Single printable character — encode as UTF-8. */
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
        if (n > 0) { insert_typed(view, buf, n); return YES; }
        return NO;
    }
    }
    (void)shift; (void)option; (void)command;
    return NO;
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
    (void)secondary;
    BOOL extend = (mods & on_mod_shift) != 0;
    OnDocHitResult hit;
    on_doc_layout_hit(view.layout, x - on_look_margin_left,
                      y - on_look_margin_top, &hit);
    if (hit.kind == ON_HIT_IMAGE) {
        if (view.image_activated_handler)
            view.image_activated_handler(hit.image_ord);
        return YES;
    }
    if (clicks == 3) {
        OnPos a, b;
        on_doc_layout_line_at(view.layout, hit.pos, &a, &b);
        view.anchor = a;
        set_caret(view, b, YES);
        return NO;
    }
    if (clicks == 2) {
        OnPos a, b;
        on_doc_layout_word_at(view.layout, hit.pos, &a, &b);
        view.anchor = a;
        set_caret(view, b, YES);
        return NO;
    }
    set_caret(view, hit.pos, extend);
    adopt_flags(view);
    return NO;
}

/*
 * on_note_view_caret — see note_view.h.
 */
OnPos
on_note_view_caret(NotesNoteView *view)
{
    return view.caret;
}
