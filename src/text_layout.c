/* ===========================================================================
 * text_layout.c — CoreText backend for on_text_layout_t.  See text_layout.h.
 *
 * One layout = one block (or one table cell) of styled text, wrapped to a
 * width and cached as an array of CTLineRefs.  Every UTF-8 byte index that
 * crosses the boundary is converted to/from CoreText's UTF-16 offset at the
 * start and end of each public function — nowhere else.
 *
 * Drawing happens in a FLIPPED context (y down): each line is drawn after a
 * local CTM flip so that CTLineDraw sees the correct orientation.
 * =========================================================================== */

#include "text_layout.h"
#include "utf_index.h"
#include "look.h"
#include "paint.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Font cache ─────────────────────────────────────────────────────────── */

/* Every font a layout asks for, by its exact size, face and traits: a few
 * sizes (body, headings, the code line numbers, the copy word) in a few
 * faces, so a small table that is never flushed; past FONT_CACHE_MAX the
 * oldest entry makes room.  Main thread only, like every layout.           */
#define FONT_CACHE_MAX 32

typedef struct {
    double               size;       /* points, the scale applied           */
    bool                 mono;
    CTFontSymbolicTraits traits;     /* bold / italic                       */
    CTFontRef            font;
} font_entry_t;

static font_entry_t g_fonts[FONT_CACHE_MAX];
static int          g_fonts_n;       /* entries in use                      */
static int          g_fonts_next;    /* the slot the next miss replaces once
                                        the table is full                    */

/*
 * get_font_traits — a font of a size, face and traits, made once.
 *
 * Inputs:
 *   size   — points
 *   mono   — the monospaced font (Menlo) rather than the system font
 *   traits — kCTFontTraitBold / kCTFontTraitItalic, or 0
 *
 * Output: the font, +1 (CFRelease it).
 */
static CTFontRef
get_font_traits(double size, bool mono, CTFontSymbolicTraits traits)
{
    for (int i = 0; i < g_fonts_n; i++) {
        font_entry_t *e = &g_fonts[i];
        if (e->size == size && e->mono == mono && e->traits == traits) {
            CFRetain(e->font);
            return e->font;
        }
    }
    CTFontRef font = mono ? CTFontCreateWithName(CFSTR("Menlo"), size, NULL)
                          : CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    if (traits != 0) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
            font, 0.0, NULL, traits, kCTFontTraitBold | kCTFontTraitItalic);
        if (styled) {
            CFRelease(font);
            font = styled;
        }
    }
    int slot = g_fonts_n < FONT_CACHE_MAX ? g_fonts_n++ : g_fonts_next++ % FONT_CACHE_MAX;
    if (g_fonts[slot].font) CFRelease(g_fonts[slot].font);
    g_fonts[slot] = (font_entry_t){ size, mono, traits, font };
    CFRetain(font);
    return font;
}

/*
 * get_font — a font for a style: body size times its scale, regular or
 * bold.
 *
 * Inputs:
 *   font_size — body size in points
 *   scale     — heading scale (1.0, on_look_h2_scale, on_look_h1_scale, …)
 *   mono      — use the monospaced font
 *   bold      — bold weight
 *
 * Output: the font, +1 (CFRelease it).
 */
static CTFontRef
get_font(double font_size, double scale, bool mono, bool bold)
{
    return get_font_traits(font_size * scale, mono, bold ? kCTFontTraitBold : 0);
}

/* ── Inline box delegates ───────────────────────────────────────────────── */

typedef struct {
    double ascent;
    double descent;
    double width;
} box_metrics_t;

static CGFloat
box_ascent(void *ref)
{
    return (CGFloat)((box_metrics_t *)ref)->ascent;
}

static CGFloat
box_descent(void *ref)
{
    return (CGFloat)((box_metrics_t *)ref)->descent;
}

static CGFloat
box_width(void *ref)
{
    return (CGFloat)((box_metrics_t *)ref)->width;
}

static void
box_dealloc(void *ref)
{
    free(ref);
}

static CTRunDelegateRef
make_box_delegate(double width, double height)
{
    box_metrics_t *m = malloc(sizeof *m);
    m->ascent  = height * 0.8;
    m->descent = height * 0.2;
    m->width   = width;
    CTRunDelegateCallbacks cb = {
        kCTRunDelegateCurrentVersion, box_dealloc, box_ascent, box_descent, box_width
    };
    CTRunDelegateRef d = CTRunDelegateCreate(&cb, m);
    return d;
}

/* ── Strike spans (CoreText has no native CF attribute) ─────────────────── */

typedef struct {
    CFIndex cf_start, cf_end;
    shared_rgba_t color;
} strike_span_t;

/* ── The layout struct ──────────────────────────────────────────────────── */

struct on_text_layout {
    char           *utf8;          /* copy of content.text                   */
    size_t          len;           /* byte length                             */
    CFStringRef     cf_str;        /* UTF-16 form                             */

    CTLineRef      *lines;
    CFIndex        *line_cf_start; /* UTF-16 start of each line               */
    size_t         *line_byte_start; /* UTF-8 byte offset                     */
    double         *line_y;        /* top of each line, y-down, layout coords */
    double         *line_asc;      /* ascent                                  */
    double         *line_h;        /* ascent + descent + leading              */
    double         *line_x;        /* left of each line: > 0 when centred     */
    size_t          n_lines;

    double          total_w;
    double          total_h;

    /* box rects — indexed by content.boxes order */
    on_rect_t      *box_rects;
    size_t          n_boxes;
    CFIndex        *box_cf_offsets; /* U+FFFC UTF-16 position for each box    */

    /* strike spans for manual drawing */
    strike_span_t  *strikes;
    size_t          n_strikes;

    /* style kept for drawing */
    bool            dark;
};

/* ── Building the attributed string ────────────────────────────────────── */

/*
 * set_attr — apply one CF attribute over a UTF-16 range.
 *
 * Inputs:
 *   ms    — mutable attributed string
 *   key   — CF attribute key
 *   value — CF value (borrowed; the AS retains it)
 *   s, e  — UTF-16 start and end
 *
 * Output: none.
 */
static void
set_attr(CFMutableAttributedStringRef ms, CFStringRef key, CFTypeRef value,
         CFIndex s, CFIndex e)
{
    CFRange r = { s, e - s };
    CFAttributedStringSetAttribute(ms, r, key, value);
}

/*
 * set_color_attr — apply a foreground colour over a UTF-16 range.
 *
 * Inputs:
 *   ms      — mutable string
 *   c       — the colour
 *   s, e    — range
 *
 * Output: none.
 */
static void
set_color_attr(CFMutableAttributedStringRef ms, shared_rgba_t c,
               CFIndex s, CFIndex e)
{
    CGColorSpaceRef sp = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGFloat comp[] = { c.r, c.g, c.b, c.a };
    CGColorRef color = CGColorCreate(sp, comp);
    set_attr(ms, kCTForegroundColorAttributeName, color, s, e);
    CGColorRelease(color);
    CGColorSpaceRelease(sp);
}

/* ── on_text_layout_new ─────────────────────────────────────────────────── */

/*
 * on_text_layout_new — see text_layout.h.
 */
on_text_layout_t *
on_text_layout_new(const on_text_content_t *content,
                   const on_text_style_t *style)
{
    on_text_layout_t *L = calloc(1, sizeof *L);
    L->dark = style->dark;

    /* Copy UTF-8 text. */
    L->len  = content->len;
    L->utf8 = malloc(L->len + 1);
    memcpy(L->utf8, content->text, L->len);
    L->utf8[L->len] = '\0';

    /* Build CFString (UTF-16). */
    L->cf_str = CFStringCreateWithBytes(NULL,
        (const UInt8 *)content->text, (CFIndex)content->len,
        kCFStringEncodingUTF8, false);
    if (L->cf_str == NULL) {
        /* Fall back on an empty string if the bytes are invalid. */
        L->cf_str = CFStringCreateWithCString(NULL, "", kCFStringEncodingUTF8);
    }
    CFIndex cf_len = CFStringGetLength(L->cf_str);

    /* Create mutable attributed string. */
    CFMutableAttributedStringRef ms =
        CFAttributedStringCreateMutable(NULL, cf_len);
    CFAttributedStringReplaceString(ms, CFRangeMake(0, 0), L->cf_str);

    /* Base font for all text. */
    bool bold_base = style->bold ||
                     (style->scale >= on_look_h1_scale - 0.01) ||
                     (style->scale >= on_look_h2_scale - 0.01);
    CTFontRef base_font = get_font(style->font_size, style->scale,
                                   style->mono, bold_base);
    if (cf_len > 0) {
        CFRange all = { 0, cf_len };
        CFAttributedStringSetAttribute(ms, all, kCTFontAttributeName, base_font);
    }
    CFRelease(base_font);

    /* Text color. */
    shared_rgba_t text_color = shared_look_pick(style->muted ? on_look_code_number : on_look_text,
                                         style->dark);
    if (cf_len > 0)
        set_color_attr(ms, text_color, 0, cf_len);

    /* Action colour + semibold over entire string. */
    if (style->action && cf_len > 0) {
        shared_rgba_t ac = shared_look_pick(on_look_action, style->dark);
        set_color_attr(ms, ac, 0, cf_len);
        CTFontRef sf = get_font(style->font_size, style->scale, false, true);
        CFAttributedStringSetAttribute(ms, CFRangeMake(0, cf_len),
                                       kCTFontAttributeName, sf);
        CFRelease(sf);
    }

    /* Run-level inline format flags (not applied in code/plain mode). */
    if (!style->plain && cf_len > 0) {
        size_t at = 0;
        for (size_t k = 0; k < content->n_runs; k++) {
            const OnRun *r = &content->runs[k];
            if (r->len == 0) { at += r->len; continue; }
            CFIndex cs = (CFIndex)on_utf16_from_utf8(content->text, at);
            CFIndex ce = (CFIndex)on_utf16_from_utf8(content->text, at + r->len);

            /* Bold (additive to what's already there). */
            if (r->flags & ON_FMT_BOLD) {
                CTFontRef bf = get_font(style->font_size, style->scale,
                                        style->mono, true);
                set_attr(ms, kCTFontAttributeName, bf, cs, ce);
                CFRelease(bf);
            }
            /* Italic. */
            if (r->flags & ON_FMT_ITALIC) {
                /* CoreText italic: use a proper italic variant of the font. */
                CTFontSymbolicTraits want = kCTFontTraitItalic;
                if (bold_base || (r->flags & ON_FMT_BOLD))
                    want |= kCTFontTraitBold;
                CTFontRef ifont = get_font_traits(style->font_size * style->scale,
                                                  style->mono, want);
                set_attr(ms, kCTFontAttributeName, ifont, cs, ce);
                CFRelease(ifont);
            }
            /* Underline. */
            if (r->flags & ON_FMT_UNDERLINE) {
                CFIndex ul = kCTUnderlineStyleSingle;
                CFNumberRef n = CFNumberCreate(NULL, kCFNumberCFIndexType, &ul);
                set_attr(ms, kCTUnderlineStyleAttributeName, n, cs, ce);
                CFRelease(n);
            }
            /* Tag colour. */
            if (r->flags & ON_FMT_TAG) {
                shared_rgba_t tc = shared_look_pick(on_look_tag, style->dark);
                set_color_attr(ms, tc, cs, ce);
                /* Semibold. */
                CTFontRef tf = get_font(style->font_size, style->scale,
                                        style->mono, true);
                set_attr(ms, kCTFontAttributeName, tf, cs, ce);
                CFRelease(tf);
            }
            at += r->len;
        }
    }

    /* Inline boxes: CTRunDelegate at each U+FFFC position. */
    L->n_boxes       = content->n_boxes;
    L->box_rects     = calloc(content->n_boxes + 1, sizeof *L->box_rects);
    L->box_cf_offsets = calloc(content->n_boxes + 1, sizeof *L->box_cf_offsets);
    for (size_t b = 0; b < content->n_boxes; b++) {
        const on_text_box_t *box = &content->boxes[b];
        CFIndex cf_off = (CFIndex)on_utf16_from_utf8(content->text, box->offset);
        L->box_cf_offsets[b] = cf_off;
        CTRunDelegateRef d = make_box_delegate(box->width, box->height);
        CFAttributedStringSetAttribute(ms, CFRangeMake(cf_off, 1),
                                       kCTRunDelegateAttributeName, d);
        CFRelease(d);
    }

    /* Find highlight: background colour. */
    if (content->n_hits > 0) {
        shared_rgba_t hc = shared_look_pick(on_look_find_hit, style->dark);
        CGColorSpaceRef sp = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGFloat comp[] = { hc.r, hc.g, hc.b, hc.a };
        CGColorRef cgc = CGColorCreate(sp, comp);
        for (size_t h = 0; h < content->n_hits; h++) {
            CFIndex hs = (CFIndex)on_utf16_from_utf8(content->text,
                                                      content->hits[h].start);
            CFIndex he = (CFIndex)on_utf16_from_utf8(content->text,
                content->hits[h].start + content->hits[h].len);
            /* Use background colour attribute for CoreText. */
            CFAttributedStringSetAttribute(ms, CFRangeMake(hs, he - hs),
                kCTBackgroundColorAttributeName, cgc);
        }
        CGColorRelease(cgc);
        CGColorSpaceRelease(sp);
    }

    /* Preedit (marked text): single underline over the marked range. */
    if (content->marked.len > 0) {
        CFIndex ms_start = (CFIndex)on_utf16_from_utf8(content->text,
                                                        content->marked.start);
        CFIndex ms_end   = (CFIndex)on_utf16_from_utf8(content->text,
            content->marked.start + content->marked.len);
        CFIndex ul = kCTUnderlineStyleSingle;
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberCFIndexType, &ul);
        set_attr(ms, kCTUnderlineStyleAttributeName, n, ms_start, ms_end);
        CFRelease(n);
    }

    /* Collect strike spans before creating the typesetter. */
    if (!style->plain) {
        size_t cap = 0, cnt = 0;
        size_t at = 0;
        for (size_t k = 0; k < content->n_runs; k++) {
            const OnRun *r = &content->runs[k];
            if ((r->flags & ON_FMT_STRIKE) && r->len > 0) cap++;
            at += r->len;
        }
        L->strikes = malloc((cap + 1) * sizeof *L->strikes);
        at = 0;
        shared_rgba_t tc = shared_look_pick(on_look_text, style->dark);
        if (style->action) tc = shared_look_pick(on_look_action, style->dark);
        for (size_t k = 0; k < content->n_runs; k++) {
            const OnRun *r = &content->runs[k];
            if ((r->flags & ON_FMT_STRIKE) && r->len > 0) {
                L->strikes[cnt].cf_start = (CFIndex)on_utf16_from_utf8(
                    content->text, at);
                L->strikes[cnt].cf_end   = (CFIndex)on_utf16_from_utf8(
                    content->text, at + r->len);
                shared_rgba_t sc = (r->flags & ON_FMT_TAG)
                    ? shared_look_pick(on_look_tag, style->dark) : tc;
                L->strikes[cnt].color = sc;
                cnt++;
            }
            at += r->len;
        }
        L->n_strikes = cnt;
    }

    /* Build typesetter and break lines. */
    CTTypesetterRef ts = CTTypesetterCreateWithAttributedString(ms);
    CFRelease(ms);

    /* Lay the lines. */
    CFIndex pos = 0;
    size_t cap = 8;
    L->lines          = malloc(cap * sizeof *L->lines);
    L->line_cf_start  = malloc(cap * sizeof *L->line_cf_start);
    L->line_byte_start= malloc(cap * sizeof *L->line_byte_start);
    L->line_y         = malloc(cap * sizeof *L->line_y);
    L->line_asc       = malloc(cap * sizeof *L->line_asc);
    L->line_h         = malloc(cap * sizeof *L->line_h);
    L->line_x         = malloc(cap * sizeof *L->line_x);

    double y_top = 0.0, max_w = 0.0;
    while (pos <= cf_len) {
        if (L->n_lines >= cap) {
            cap *= 2;
            L->lines           = realloc(L->lines, cap * sizeof *L->lines);
            L->line_cf_start   = realloc(L->line_cf_start, cap * sizeof *L->line_cf_start);
            L->line_byte_start = realloc(L->line_byte_start, cap * sizeof *L->line_byte_start);
            L->line_y          = realloc(L->line_y, cap * sizeof *L->line_y);
            L->line_asc        = realloc(L->line_asc, cap * sizeof *L->line_asc);
            L->line_h          = realloc(L->line_h, cap * sizeof *L->line_h);
            L->line_x          = realloc(L->line_x, cap * sizeof *L->line_x);
        }
        size_t idx = L->n_lines;
        L->line_cf_start[idx]   = pos;
        L->line_byte_start[idx] = on_utf8_from_utf16(L->utf8, L->len, (size_t)pos);

        /* Suggest a line break. */
        CFIndex remaining = cf_len - pos;
        CFIndex count;
        if (style->width > 0 && remaining > 0) {
            count = CTTypesetterSuggestLineBreak(ts, pos,
                                                 (double)style->width);
            if (count <= 0) count = 1;
        } else {
            count = remaining;
        }
        if (count <= 0 || pos >= cf_len) {
            /* Empty text, or text ending in a newline (a table cell): one
             * more, empty line with the font's metrics, for the caret. */
            bool trailing_newline = cf_len > 0 &&
                CFStringGetCharacterAtIndex(L->cf_str, cf_len - 1) == '\n';
            if (L->n_lines == 0 || trailing_newline) {
                CTFontRef ef = get_font(style->font_size, style->scale,
                                        style->mono, bold_base);
                double asc  = CTFontGetAscent(ef);
                double desc = CTFontGetDescent(ef);
                double lead = CTFontGetLeading(ef);
                CFRelease(ef);
                L->line_y[idx]  = y_top;
                L->line_x[idx]  = style->width > 0 ? style->width * style->flush : 0.0;
                L->line_asc[idx]= asc;
                L->line_h[idx]  = ceil(asc + desc + lead);
                L->lines[idx]   = NULL;
                L->n_lines++;
                y_top += L->line_h[idx];
            }
            break;
        }

        CTLineRef line = CTTypesetterCreateLine(ts, CFRangeMake(pos, count));
        CGFloat asc, desc, lead;
        double lw = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
        double lh = ceil(asc + desc + lead);

        L->lines[idx]  = line;
        L->line_y[idx] = y_top;
        /* A centred or right-aligned line is drawn, measured and hit at
         * its flush offset (trailing whitespace hangs outside it). */
        L->line_x[idx] = style->flush > 0 && style->width > 0
            ? (double)CTLineGetPenOffsetForFlush(line, style->flush, style->width) : 0.0;
        L->line_asc[idx] = (double)asc;
        L->line_h[idx]   = lh;
        L->n_lines++;

        if (lw > max_w) max_w = lw;
        y_top += lh;
        pos   += count;
    }
    CFRelease(ts);

    L->total_w = ceil(max_w);
    L->total_h = y_top;

    /* Compute box_rects from line positions. */
    for (size_t b = 0; b < L->n_boxes; b++) {
        CFIndex cf_off = L->box_cf_offsets[b];
        /* Find which line contains this offset. */
        for (size_t li = 0; li < L->n_lines; li++) {
            if (L->lines[li] == NULL) continue;
            CFRange lr = CTLineGetStringRange(L->lines[li]);
            if (cf_off >= lr.location && cf_off < lr.location + lr.length) {
                CGFloat xo = (CGFloat)CTLineGetOffsetForStringIndex(
                    L->lines[li], cf_off, NULL);
                /* box size: use the delegate metrics we stored */
                const on_text_box_t *bx = &content->boxes[b];
                on_rect_t r = {
                    .x = L->line_x[li] + (double)xo,
                    .y = L->line_y[li],
                    .w = bx->width,
                    .h = bx->height,
                };
                L->box_rects[b] = r;
                break;
            }
        }
    }

    return L;
}

/* ── on_text_layout_free ────────────────────────────────────────────────── */

/*
 * on_text_layout_free — see text_layout.h.
 */
void
on_text_layout_free(on_text_layout_t *layout)
{
    if (layout == NULL)
        return;
    for (size_t i = 0; i < layout->n_lines; i++)
        if (layout->lines[i]) CFRelease(layout->lines[i]);
    free(layout->lines);
    free(layout->line_cf_start);
    free(layout->line_byte_start);
    free(layout->line_y);
    free(layout->line_asc);
    free(layout->line_h);
    free(layout->line_x);
    free(layout->box_rects);
    free(layout->box_cf_offsets);
    free(layout->strikes);
    if (layout->cf_str) CFRelease(layout->cf_str);
    free(layout->utf8);
    free(layout);
}

/* ── Size and line count ────────────────────────────────────────────────── */

/*
 * on_text_layout_size — see text_layout.h.
 */
void
on_text_layout_size(const on_text_layout_t *layout, double *w, double *h)
{
    *w = layout->total_w;
    *h = layout->total_h;
}

/*
 * on_text_layout_line_count — see text_layout.h.
 */
size_t
on_text_layout_line_count(const on_text_layout_t *layout)
{
    return layout->n_lines > 0 ? layout->n_lines : 1;
}

/* ── Line query ─────────────────────────────────────────────────────────── */

/*
 * on_text_layout_line — see text_layout.h.
 */
void
on_text_layout_line(const on_text_layout_t *layout, size_t line,
                    on_text_span_t *span, on_rect_t *rect)
{
    if (line >= layout->n_lines) line = layout->n_lines > 0 ? layout->n_lines - 1 : 0;
    if (span != NULL) {
        size_t start = layout->line_byte_start[line];
        size_t end   = (line + 1 < layout->n_lines)
            ? layout->line_byte_start[line + 1] : layout->len;
        span->start = start;
        span->len   = end - start;
    }
    if (rect != NULL) {
        double lw = 0.0, lh = layout->line_h[line];
        if (layout->lines[line]) {
            CGFloat asc, desc, lead;
            lw = CTLineGetTypographicBounds(layout->lines[line], &asc, &desc, &lead);
        }
        rect->x = layout->line_x[line];
        rect->y = layout->line_y[line];
        rect->w = lw;
        rect->h = lh;
    }
}

/* ── Cursor positions ───────────────────────────────────────────────────── */

/*
 * on_text_layout_cursor_rect — see text_layout.h.
 */
void
on_text_layout_cursor_rect(const on_text_layout_t *layout, size_t byte,
                           on_rect_t *out)
{
    out->w = 1.0;
    if (layout->n_lines == 0) { *out = (on_rect_t){ 0, 0, 1, layout->total_h }; return; }

    /* Find which line the byte belongs to (at a wrap point → next line). */
    CFIndex cf_off = (CFIndex)on_utf16_from_utf8(layout->utf8, byte);
    size_t  li = 0;
    for (size_t i = layout->n_lines; i-- > 0; ) {
        if (layout->line_cf_start[i] <= cf_off) { li = i; break; }
    }
    /* If the offset is at the start of the next line, prefer that. */
    if (li + 1 < layout->n_lines && cf_off == layout->line_cf_start[li + 1])
        li++;

    double x_off = 0.0;
    if (layout->lines[li]) {
        x_off = (double)CTLineGetOffsetForStringIndex(layout->lines[li],
                                                       cf_off, NULL);
    }
    out->x = layout->line_x[li] + x_off;
    out->y = layout->line_y[li];
    out->h = layout->line_h[li];
}

/*
 * on_text_layout_index_at — see text_layout.h.
 */
size_t
on_text_layout_index_at(const on_text_layout_t *layout, double x, double y)
{
    if (layout->n_lines == 0) return 0;

    /* Clamp y to valid range. */
    size_t li = 0;
    for (size_t i = 0; i < layout->n_lines; i++) {
        if (y >= layout->line_y[i]) li = i;
    }
    if (!layout->lines[li])
        return layout->line_byte_start[li];

    CGPoint pt = { (CGFloat)(x - layout->line_x[li]), (CGFloat)layout->line_asc[li] };
    CFIndex idx = CTLineGetStringIndexForPosition(layout->lines[li], pt);
    if (idx < 0) idx = 0;
    if (idx > (CFIndex)CFStringGetLength(layout->cf_str))
        idx = (CFIndex)CFStringGetLength(layout->cf_str);
    return on_utf8_from_utf16(layout->utf8, layout->len, (size_t)idx);
}

/* ── Character and word navigation ─────────────────────────────────────── */

/*
 * on_text_layout_next_cursor — see text_layout.h.
 */
size_t
on_text_layout_next_cursor(const on_text_layout_t *layout, size_t byte)
{
    if (byte >= layout->len) return layout->len;
    CFIndex cf_off = (CFIndex)on_utf16_from_utf8(layout->utf8, byte);
    /* CFStringGetRangeOfComposedCharactersAtIndex returns the grapheme
     * cluster range starting at or enclosing cf_off.                       */
    CFRange r = CFStringGetRangeOfComposedCharactersAtIndex(layout->cf_str, cf_off);
    CFIndex next = r.location + r.length;
    return on_utf8_from_utf16(layout->utf8, layout->len, (size_t)next);
}

/*
 * on_text_layout_prev_cursor — see text_layout.h.
 */
size_t
on_text_layout_prev_cursor(const on_text_layout_t *layout, size_t byte)
{
    if (byte == 0) return 0;
    CFIndex cf_off = (CFIndex)on_utf16_from_utf8(layout->utf8, byte);
    /* Step one code unit back to land inside the previous cluster,
     * then get that cluster's range.                                        */
    if (cf_off > 0) cf_off--;
    CFRange r = CFStringGetRangeOfComposedCharactersAtIndex(layout->cf_str, cf_off);
    return on_utf8_from_utf16(layout->utf8, layout->len, (size_t)r.location);
}

/*
 * word_boundary — the nearest word end after a UTF-16 index (forward), or
 * the nearest word start before it (backward), walking the string's word
 * tokens.  Spaces and punctuation are not words, so a step from just after
 * a space still finds the word before it (Pango's is_word_start/end).
 *
 * Inputs:
 *   layout  — the layout
 *   cf_off  — the UTF-16 index to step from
 *   forward — YES for the next word end
 *
 * Output: the boundary's UTF-16 index; the string's end (forward) or 0
 *         (backward) when there is none.
 */
static CFIndex
word_boundary(const on_text_layout_t *layout, CFIndex cf_off, bool forward)
{
    CFIndex           n   = CFStringGetLength(layout->cf_str);
    CFStringTokenizerRef tok = CFStringTokenizerCreate(NULL, layout->cf_str, CFRangeMake(0, n),
                                                       kCFStringTokenizerUnitWord, NULL);
    CFIndex found = forward ? n : 0;
    while (CFStringTokenizerAdvanceToNextToken(tok) != kCFStringTokenizerTokenNone) {
        CFRange tr = CFStringTokenizerGetCurrentTokenRange(tok);
        if (forward) {
            if (tr.location + tr.length > cf_off) { found = tr.location + tr.length; break; }
        } else {
            if (tr.location >= cf_off) break;
            found = tr.location;
        }
    }
    CFRelease(tok);
    return found;
}

/*
 * on_text_layout_next_word — see text_layout.h.
 */
size_t
on_text_layout_next_word(const on_text_layout_t *layout, size_t byte)
{
    if (byte >= layout->len) return layout->len;
    CFIndex cf_off = (CFIndex)on_utf16_from_utf8(layout->utf8, byte);
    return on_utf8_from_utf16(layout->utf8, layout->len,
                              (size_t)word_boundary(layout, cf_off, true));
}

/*
 * on_text_layout_prev_word — see text_layout.h.
 */
size_t
on_text_layout_prev_word(const on_text_layout_t *layout, size_t byte)
{
    if (byte == 0) return 0;
    CFIndex cf_off = (CFIndex)on_utf16_from_utf8(layout->utf8, byte);
    return on_utf8_from_utf16(layout->utf8, layout->len,
                              (size_t)word_boundary(layout, cf_off, false));
}

/*
 * on_text_layout_word_at — see text_layout.h.
 */
void
on_text_layout_word_at(const on_text_layout_t *layout, size_t byte,
                       on_text_span_t *span)
{
    CFIndex cf_off = (CFIndex)on_utf16_from_utf8(layout->utf8, byte);
    CFStringTokenizerRef tok = CFStringTokenizerCreate(
        NULL, layout->cf_str, CFRangeMake(0, CFStringGetLength(layout->cf_str)),
        kCFStringTokenizerUnitWord, NULL);
    CFStringTokenizerGoToTokenAtIndex(tok, cf_off);
    CFRange tr = CFStringTokenizerGetCurrentTokenRange(tok);
    CFRelease(tok);
    if (tr.length > 0 &&
        cf_off >= tr.location && cf_off <= tr.location + tr.length) {
        span->start = on_utf8_from_utf16(layout->utf8, layout->len,
                                          (size_t)tr.location);
        size_t e    = on_utf8_from_utf16(layout->utf8, layout->len,
                                          (size_t)(tr.location + tr.length));
        span->len   = e - span->start;
    } else {
        span->start = byte;
        span->len   = 0;
    }
}

/* ── Range rects ────────────────────────────────────────────────────────── */

/*
 * on_text_layout_range_rects — see text_layout.h.
 */
size_t
on_text_layout_range_rects(const on_text_layout_t *layout,
                           on_text_span_t span, on_rect_t *rects, size_t max)
{
    CFIndex cs = (CFIndex)on_utf16_from_utf8(layout->utf8, span.start);
    CFIndex ce = (CFIndex)on_utf16_from_utf8(layout->utf8, span.start + span.len);
    size_t count = 0;
    for (size_t li = 0; li < layout->n_lines; li++) {
        if (!layout->lines[li]) {
            /* An empty line inside the range: a zero-width rectangle (the
             * caller draws a marker so the line shows as selected). */
            if (layout->line_cf_start[li] >= cs && layout->line_cf_start[li] < ce) {
                if (count < max)
                    rects[count] = (on_rect_t){ layout->line_x[li], layout->line_y[li],
                                                0.0, layout->line_h[li] };
                count++;
            }
            continue;
        }
        CFRange lr = CTLineGetStringRange(layout->lines[li]);
        CFIndex ls = lr.location, le = lr.location + lr.length;
        if (ce <= ls || cs >= le) continue; /* no overlap */
        CFIndex rs = (cs > ls) ? cs : ls;
        CFIndex re = (ce < le) ? ce : le;
        CGFloat xA, xB;
        xA = (CGFloat)CTLineGetOffsetForStringIndex(layout->lines[li], rs, NULL);
        xB = (CGFloat)CTLineGetOffsetForStringIndex(layout->lines[li], re, NULL);
        if (xB < xA) { CGFloat t = xA; xA = xB; xB = t; }
        if (count < max) {
            rects[count] = (on_rect_t){
                .x = layout->line_x[li] + (double)xA,
                .y = layout->line_y[li],
                .w = (double)(xB - xA),
                .h = layout->line_h[li],
            };
        }
        count++;
    }
    return count;
}

/* ── Box rect ───────────────────────────────────────────────────────────── */

/*
 * on_text_layout_box_rect — see text_layout.h.
 */
void
on_text_layout_box_rect(const on_text_layout_t *layout, size_t box,
                        on_rect_t *out)
{
    if (box < layout->n_boxes)
        *out = layout->box_rects[box];
    else
        *out = (on_rect_t){ 0 };
}

/* ── Drawing ────────────────────────────────────────────────────────────── */

/*
 * strike_y — where a strikethrough goes: through the middle of the
 * lowercase letters (half the x-height above the baseline) of the font at
 * a string index, as AppKit and Pango draw it.  Half the line's ascent,
 * used before, measured from the line's top, which sits above the capitals
 * and so put the line near the top of the text.
 *
 * Inputs:
 *   line     — the CoreText line
 *   at       — a string index inside it (the strike's start)
 *   baseline — the line's baseline, in drawing coordinates (y down)
 *
 * Output: the y to draw at.
 */
static double
strike_y(CTLineRef line, CFIndex at, double baseline)
{
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, i);
        CFRange  r   = CTRunGetStringRange(run);
        if (at < r.location || at >= r.location + r.length) continue;
        CTFontRef font = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        if (font) return baseline - CTFontGetXHeight(font) / 2.0;
    }
    return baseline;
}

/*
 * on_text_layout_draw — see text_layout.h.
 */
void
on_text_layout_draw(const on_text_layout_t *layout, CGContextRef ctx,
                    double x, double y)
{
    for (size_t li = 0; li < layout->n_lines; li++) {
        if (!layout->lines[li]) continue;
        double ly = y + layout->line_y[li];
        double baseline = ly + layout->line_asc[li];

        /* Draw the line: flip y for CoreText, then restore. */
        CGContextSaveGState(ctx);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        CGContextTranslateCTM(ctx, x + layout->line_x[li], baseline);
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextSetTextPosition(ctx, 0.0, 0.0);
        CTLineDraw(layout->lines[li], ctx);
        CGContextRestoreGState(ctx);

        /* Strikethrough: drawn manually after the text. */
        CFRange lr = CTLineGetStringRange(layout->lines[li]);
        for (size_t si = 0; si < layout->n_strikes; si++) {
            const strike_span_t *sp = &layout->strikes[si];
            CFIndex ss = sp->cf_start, se = sp->cf_end;
            /* Clamp to this line's range. */
            CFIndex ls = lr.location, le = lr.location + lr.length;
            if (se <= ls || ss >= le) continue;
            CFIndex rs = (ss > ls) ? ss : ls;
            CFIndex re = (se < le) ? se : le;
            CGFloat xa = (CGFloat)CTLineGetOffsetForStringIndex(
                layout->lines[li], rs, NULL);
            CGFloat xb = (CGFloat)CTLineGetOffsetForStringIndex(
                layout->lines[li], re, NULL);
            double mid_y = strike_y(layout->lines[li], rs, baseline);
            on_paint_line(ctx, x + layout->line_x[li] + xa, mid_y, x + layout->line_x[li] + xb, mid_y,
                          sp->color, 1.0);
        }
    }
}
