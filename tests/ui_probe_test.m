/* ===========================================================================
 * ui_probe_test.m — script-driven acceptance tests for the note editor
 *
 * Replays every file in tests/ui/ (the "probe scripts") against a
 * NotesNoteView, 640 pt wide, with the settings the GTK runner uses
 * (first-line title, code line numbers, code copy buttons), through the
 * probe interface: on_note_view_feed_key, on_note_view_feed_text,
 * on_note_view_feed_click.  The view copies and pastes through a private
 * pasteboard, never the user's clipboard.  The command set is the GTK
 * runner's (porting/gtk_reference/tests/ui_probe.c); an unknown command is
 * a failure, and so is every failed expectation — the exit status says.
 *
 * Script format (see tests/ui/ — each .txt file):
 *   text <string>                — on_note_view_feed_text
 *   key <name> [shift] [alt] [ctrl] [primary]
 *                                — on_note_view_feed_key
 *   command <selector>           — a key binding's command, as AppKit sends
 *                                  it (e.g. deleteToEndOfParagraph:)
 *   click <x> <y> [n] [mods…]    — on_note_view_feed_click, view coordinates
 *   marked <text>                — the input method's marked text
 *   commit <text> [<loc> <len>]  — the input method commits text, replacing
 *                                  a UTF-16 range of the caret's text
 *                                  (\n escapes; "-" for no text)
 *   unmark                       — the input method commits its marked text
 *   context <x> <y>              — a secondary click: the context menu
 *   menu <action>                — choose the context menu's item with that
 *                                  action (e.g. table_row_add:)
 *   para <kind>                  — on_note_view_toggle_paragraph
 *   inline <style>               — on_note_view_toggle_inline
 *   table / date                 — insert a table / today's date
 *   image <path>                 — insert a PNG (path relative to the script)
 *   find <text>                  — on_note_view_find
 *   step <text> / step-back <text>
 *                                — on_note_view_find_step
 *   select-all                   — ⌘A
 *   undo / redo
 *   width <points>               — the view's width
 *   settle [ms]                  — run the main loop a while
 *   check <index> <kind> <text>  — assert block content
 *   caret <block> <offset>       — assert caret position
 *   checked <block> <0|1>        — assert a task block's tick
 *   tag <block> <offset> <0|1>   — assert the character at a byte offset is
 *                                  (or is not) part of a #tag
 *   table-size <block> <r> <c>   — assert a table's rows and columns
 *   clipboard <text>             — assert the pasteboard's text (\n escapes)
 *   where / print                — print the caret / document (debug aid)
 *   # …                          — comment, ignored
 * =========================================================================== */

#import <Cocoa/Cocoa.h>

#include "check.h"

#import "note_view.h"

#include "bnbf.h"
#include "document.h"
#include "note_edit.h"

#include <dirent.h>

/* The app context: only the editor settings the GTK runner turns on. */
static OnApp probe_app;

/* The view's width, as the GTK runner's window. */
static const CGFloat probe_width = 640.0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * kind_name — the OnBlockKind as a short ASCII label.
 *
 * Inputs:
 *   kind — block kind
 *
 * Output: a static string.
 */
static const char *
kind_name(OnBlockKind kind)
{
    switch (kind) {
    case ON_BLOCK_PARA:   return "para";
    case ON_BLOCK_H1:     return "h1";
    case ON_BLOCK_H2:     return "h2";
    case ON_BLOCK_BULLET: return "bullet";
    case ON_BLOCK_NUMBER: return "number";
    case ON_BLOCK_CHECK:  return "check";
    case ON_BLOCK_CODE:   return "code";
    case ON_BLOCK_IMAGE:  return "image";
    case ON_BLOCK_TABLE:  return "table";
    case ON_N_BLOCK_KINDS: break;
    }
    return "?";
}

/*
 * parse_kind — the OnBlockKind for a kind label.
 *
 * Inputs:
 *   s — label string
 *
 * Output: the kind, or ON_BLOCK_PARA when unknown.
 */
static OnBlockKind
parse_kind(const char *s)
{
    if (!strcmp(s, "h1"))     return ON_BLOCK_H1;
    if (!strcmp(s, "h2"))     return ON_BLOCK_H2;
    if (!strcmp(s, "bullet")) return ON_BLOCK_BULLET;
    if (!strcmp(s, "number")) return ON_BLOCK_NUMBER;
    if (!strcmp(s, "check"))  return ON_BLOCK_CHECK;
    if (!strcmp(s, "code"))   return ON_BLOCK_CODE;
    if (!strcmp(s, "image"))  return ON_BLOCK_IMAGE;
    if (!strcmp(s, "table"))  return ON_BLOCK_TABLE;
    return ON_BLOCK_PARA;
}

/*
 * para_flag — the ON_FMT_* flag that sets a kind via toggle_paragraph.
 *
 * Inputs:
 *   s — kind label
 *
 * Output: the flag; 0 for para (body text).
 */
static guint32
para_flag(const char *s)
{
    if (!strcmp(s, "h1"))     return ON_FMT_H1;
    if (!strcmp(s, "h2"))     return ON_FMT_H2;
    if (!strcmp(s, "bullet")) return ON_FMT_LIST_BULLET;
    if (!strcmp(s, "number")) return ON_FMT_LIST_NUMBER;
    if (!strcmp(s, "check"))  return ON_FMT_LIST_CHECK;
    if (!strcmp(s, "code"))   return ON_FMT_CODEBLOCK;
    return 0; /* para */
}

/*
 * inline_flag — the ON_FMT_* flag for a named inline style.
 *
 * Inputs:
 *   s — style label
 *
 * Output: the flag; 0 when unknown.
 */
static guint32
inline_flag(const char *s)
{
    if (!strcmp(s, "bold"))      return ON_FMT_BOLD;
    if (!strcmp(s, "italic"))    return ON_FMT_ITALIC;
    if (!strcmp(s, "underline")) return ON_FMT_UNDERLINE;
    if (!strcmp(s, "strike"))    return ON_FMT_STRIKE;
    return 0;
}

/*
 * parse_key — the on_key_t for a key name.
 *
 * Inputs:
 *   s — key name as written in the script
 *   ch — receives the Unicode code point when the key is on_key_character
 *
 * Output: the key; on_key_character for single printable keys.
 */
static on_key_t
parse_key(const char *s, uint32_t *ch)
{
    *ch = 0;
    if (!strcmp(s, "Left"))      return on_key_left;
    if (!strcmp(s, "Right"))     return on_key_right;
    if (!strcmp(s, "Up"))        return on_key_up;
    if (!strcmp(s, "Down"))      return on_key_down;
    if (!strcmp(s, "Home"))      return on_key_home;
    if (!strcmp(s, "End"))       return on_key_end;
    if (!strcmp(s, "PageUp"))    return on_key_page_up;
    if (!strcmp(s, "PageDown"))  return on_key_page_down;
    if (!strcmp(s, "Return"))    return on_key_return;
    if (!strcmp(s, "BackSpace")) return on_key_backspace;
    if (!strcmp(s, "Delete"))    return on_key_delete;
    if (!strcmp(s, "Tab"))       return on_key_tab;
    if (!strcmp(s, "Escape"))    return on_key_escape;
    /* Single printable character (ASCII). */
    if (s[0] != '\0' && s[1] == '\0') {
        *ch = (uint32_t)(unsigned char)s[0];
        return on_key_character;
    }
    return on_key_character;
}

/*
 * print_document — dump every block's kind and text to stdout.
 *
 * Inputs:
 *   doc  — the document
 *   file — the script file (context label)
 *   line — the script line number
 *
 * Output: none.
 */
static void
print_document(const OnDocument *doc, const char *file, int line)
{
    guint n = on_document_n_blocks(doc);
    printf("  doc at %s:%d (%u blocks):\n", file, line, n);
    for (guint i = 0; i < n; i++) {
        const OnBlock *b = on_document_block(doc, i);
        if (b->text != NULL)
            printf("    [%u] %s \"%s\"\n", i, kind_name(b->kind), b->text->text->str);
        else
            printf("    [%u] %s\n", i, kind_name(b->kind));
    }
}

/*
 * parse_mods — on_mod_t bits from the words after a key or click.
 *
 * Inputs:
 *   tok — the first modifier word, then strtok(NULL, " ") for the rest
 *
 * Output: the bits.
 */
static unsigned
parse_mods(char *tok)
{
    unsigned mods = 0;
    for (; tok; tok = strtok(NULL, " ")) {
        if (!strcmp(tok, "shift"))   mods |= on_mod_shift;
        if (!strcmp(tok, "alt"))     mods |= on_mod_option;
        if (!strcmp(tok, "ctrl"))    mods |= on_mod_option; /* ctrl = word on macOS */
        if (!strcmp(tok, "primary")) mods |= on_mod_command;
    }
    return mods;
}

/*
 * settle — run the main loop for a while, as the GTK runner lets its
 * frame clock and idles run.
 *
 * Inputs:
 *   ms — how long
 */
static void
settle(int ms)
{
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:ms / 1000.0]];
}

/* ── Script runner ───────────────────────────────────────────────────────── */

/*
 * run_script — load and execute one probe script.  Failures are recorded
 * through check().
 *
 * Inputs:
 *   path — the file's path
 *   view — the note view to drive (will be reset to an empty document)
 *
 * Output: none.
 */
static void
run_script(const char *path, NotesNoteView *view)
{
    /* Reset to a fresh empty document. */
    on_note_view_load_document(view, on_document_new());

    FILE *f = fopen(path, "r");
    if (!f) {
        check(false, path);
        return;
    }

    char line[4096];
    int line_num = 0;
    while (fgets(line, sizeof line, f)) {
        line_num++;
        /* Strip trailing newline. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines and comments. */
        if (len == 0 || line[0] == '#') continue;

        /* Split at the first space. */
        char *sp = strchr(line, ' ');
        char *arg = sp ? sp + 1 : (char *)(line + len);
        char cmd[32] = { 0 };
        size_t clen = sp ? (size_t)(sp - line) : len;
        if (clen >= sizeof cmd) clen = sizeof cmd - 1;
        memcpy(cmd, line, clen);

        OnDocument *doc = on_note_view_document(view);

        if (!strcmp(cmd, "text")) {
            on_note_view_feed_text(view, arg);

        } else if (!strcmp(cmd, "key")) {
            /* key <name> [shift] [alt] [ctrl] [primary] */
            char name[32] = { 0 };
            char *tok = strtok(arg, " ");
            if (tok) { strncpy(name, tok, sizeof name - 1); }
            unsigned mods = parse_mods(strtok(NULL, " "));
            uint32_t ch = 0;
            on_key_t key = parse_key(name, &ch);
            on_note_view_feed_key(view, key, ch, mods);

        } else if (!strcmp(cmd, "para")) {
            on_note_view_toggle_paragraph(view, para_flag(arg));

        } else if (!strcmp(cmd, "inline")) {
            on_note_view_toggle_inline(view, (OnFormatFlags)inline_flag(arg));

        } else if (!strcmp(cmd, "check")) {
            /* check <index> <kind> <text> — text may contain spaces */
            char *tok = strtok(arg, " ");
            int idx = tok ? atoi(tok) : -1;
            tok = strtok(NULL, " ");
            OnBlockKind want_kind = tok ? parse_kind(tok) : ON_BLOCK_PARA;
            /* Remaining text (may be empty). */
            tok = strtok(NULL, "");
            const char *want_text = tok ? tok : "";

            /* Build a label for the check() call. */
            char label[256];
            snprintf(label, sizeof label, "%s:%d check %d %s \"%s\"",
                     path, line_num, idx, kind_name(want_kind), want_text);

            guint nb = on_document_n_blocks(doc);
            if ((guint)idx >= nb) {
                check(false, label);
            } else {
                const OnBlock *b = on_document_block(doc, (guint)idx);
                gchar *actual = g_strcompress(b->text ? b->text->text->str : "");
                BOOL ok = (b->kind == want_kind) &&
                          (!strcmp(actual, want_text));
                if (!ok) {
                    printf("  expected: %s \"%s\"\n  got:      %s \"%s\"\n",
                           kind_name(want_kind), want_text,
                           kind_name(b->kind), actual);
                }
                check(ok, label);
                g_free(actual);
            }

        } else if (!strcmp(cmd, "caret")) {
            /* caret <block> <offset> */
            char *tok = strtok(arg, " ");
            int want_block = tok ? atoi(tok) : -1;
            tok = strtok(NULL, " ");
            int want_offset = tok ? atoi(tok) : -1;

            char label[256];
            snprintf(label, sizeof label, "%s:%d caret %d %d",
                     path, line_num, want_block, want_offset);

            OnPos caret = on_note_view_caret(view);
            BOOL ok = ((int)caret.block == want_block) &&
                      ((int)caret.offset == want_offset);
            if (!ok) {
                printf("  expected caret: block=%d offset=%d\n"
                       "  got:            block=%u offset=%zu\n",
                       want_block, want_offset, caret.block, caret.offset);
            }
            check(ok, label);

        } else if (!strcmp(cmd, "click")) {
            /* click <x> <y> [n] [mods…] — the view's own coordinates */
            char  *tok = strtok(arg, " ");
            double x   = tok ? atof(tok) : 0;
            tok = strtok(NULL, " ");
            double y   = tok ? atof(tok) : 0;
            tok = strtok(NULL, " ");
            NSInteger n = 1;
            if (tok && tok[0] >= '0' && tok[0] <= '9') {
                n   = atoi(tok);
                tok = strtok(NULL, " ");
            }
            on_note_view_feed_click(view, x, y, n, parse_mods(tok), NO);

        } else if (!strcmp(cmd, "command")) {
            [view doCommandBySelector:NSSelectorFromString(@(arg))];

        } else if (!strcmp(cmd, "marked")) {
            NSString *marked = @(arg);
            [(id<NSTextInputClient>)view setMarkedText:marked
                                         selectedRange:NSMakeRange(marked.length, 0)
                                      replacementRange:NSMakeRange(NSNotFound, 0)];

        } else if (!strcmp(cmd, "commit")) {
            char  *tok  = strtok(arg, " ");
            gchar *text = g_strcompress(tok && strcmp(tok, "-") ? tok : "");
            char  *loc  = strtok(NULL, " ");
            char  *len  = strtok(NULL, " ");
            NSRange range = loc && len ? NSMakeRange((NSUInteger)atoi(loc), (NSUInteger)atoi(len))
                                       : NSMakeRange(NSNotFound, 0);
            [(id<NSTextInputClient>)view insertText:@(text) replacementRange:range];
            g_free(text);

        } else if (!strcmp(cmd, "unmark")) {
            [(id<NSTextInputClient>)view unmarkText];

        } else if (!strcmp(cmd, "context")) {
            char  *tok = strtok(arg, " ");
            double x   = tok ? atof(tok) : 0;
            tok = strtok(NULL, " ");
            on_note_view_feed_click(view, x, tok ? atof(tok) : 0, 1, 0, YES);

        } else if (!strcmp(cmd, "menu")) {
            char label[256];
            snprintf(label, sizeof label, "%s:%d menu %s is handled", path, line_num, arg);
            check([NSApp sendAction:NSSelectorFromString(@(arg)) to:view from:nil], label);

        } else if (!strcmp(cmd, "checked")) {
            char *tok = strtok(arg, " ");
            guint idx = tok ? (guint)atoi(tok) : 0;
            tok = strtok(NULL, " ");
            gboolean want = tok ? atoi(tok) != 0 : FALSE;
            const OnBlock *b = idx < on_document_n_blocks(doc) ? on_document_block(doc, idx) : NULL;
            char label[256];
            snprintf(label, sizeof label, "%s:%d checked %u %d", path, line_num, idx, want);
            check(b != NULL && b->kind == ON_BLOCK_CHECK && !b->checked == !want, label);

        } else if (!strcmp(cmd, "tag")) {
            char *tok = strtok(arg, " ");
            guint idx = tok ? (guint)atoi(tok) : 0;
            tok = strtok(NULL, " ");
            gsize off = tok ? (gsize)atoi(tok) : 0;
            tok = strtok(NULL, " ");
            gboolean want = tok ? atoi(tok) != 0 : FALSE;
            const OnBlock *b = idx < on_document_n_blocks(doc) ? on_document_block(doc, idx) : NULL;
            gboolean have = b && b->text && (on_text_flags_at(b->text, off) & ON_FMT_TAG) != 0;
            char label[256];
            snprintf(label, sizeof label, "%s:%d tag %u %zu %d", path, line_num, idx, off, want);
            check(b != NULL && !have == !want, label);

        } else if (!strcmp(cmd, "table-size")) {
            char *tok = strtok(arg, " ");
            guint idx = tok ? (guint)atoi(tok) : 0;
            tok = strtok(NULL, " ");
            int rows = tok ? atoi(tok) : -1;
            tok = strtok(NULL, " ");
            int cols = tok ? atoi(tok) : -1;
            const OnBlock *b = idx < on_document_n_blocks(doc) ? on_document_block(doc, idx) : NULL;
            char label[256];
            snprintf(label, sizeof label, "%s:%d table-size %u %d %d", path, line_num, idx, rows, cols);
            if (b && b->kind == ON_BLOCK_TABLE && (b->rows != rows || b->cols != cols))
                printf("  got: %d x %d\n", b->rows, b->cols);
            check(b != NULL && b->kind == ON_BLOCK_TABLE && b->rows == rows && b->cols == cols, label);

        } else if (!strcmp(cmd, "clipboard")) {
            gchar    *want = g_strcompress(arg);
            NSString *have = [view.pasteboard stringForType:NSPasteboardTypeString];
            char label[256];
            snprintf(label, sizeof label, "%s:%d clipboard \"%s\"", path, line_num, arg);
            if (![have isEqualToString:@(want)]) printf("  got: \"%s\"\n", have.UTF8String);
            check([have isEqualToString:@(want)], label);
            g_free(want);

        } else if (!strcmp(cmd, "undo")) {
            on_note_view_undo(view);

        } else if (!strcmp(cmd, "redo")) {
            on_note_view_redo(view);

        } else if (!strcmp(cmd, "table")) {
            on_note_view_insert_table(view);

        } else if (!strcmp(cmd, "date")) {
            on_note_view_insert_date(view);

        } else if (!strcmp(cmd, "select-all")) {
            on_note_view_feed_key(view, on_key_character, 'a', on_mod_command);

        } else if (!strcmp(cmd, "find")) {
            on_note_view_find(view, @(arg));

        } else if (!strcmp(cmd, "step")) {
            on_note_view_find_step(view, @(arg), YES);

        } else if (!strcmp(cmd, "step-back")) {
            on_note_view_find_step(view, @(arg), NO);

        } else if (!strcmp(cmd, "width")) {
            [view setFrameSize:NSMakeSize(atof(arg), view.frame.size.height)];

        } else if (!strcmp(cmd, "settle")) {
            settle(*arg ? atoi(arg) : 200);

        } else if (!strcmp(cmd, "image")) {
            /* image <path> — relative to the script's folder */
            NSString *file = [[@(path) stringByDeletingLastPathComponent]
                              stringByAppendingPathComponent:@(arg)];
            NSData   *png  = [NSData dataWithContentsOfFile:file];
            char label[256];
            snprintf(label, sizeof label, "%s:%d image %s can be read", path, line_num, arg);
            check(png != nil, label);
            if (png) on_note_view_insert_image(view, png.bytes, png.length);

        } else if (!strcmp(cmd, "where")) {
            OnPos c = on_note_view_caret(view);
            printf("  %s:%d: caret %u:%d:%zu\n", path, line_num, c.block, c.cell, c.offset);

        } else if (!strcmp(cmd, "print")) {
            print_document(doc, path, line_num);

        } else {
            char label[256];
            snprintf(label, sizeof label, "%s:%d: known command '%s'", path, line_num, cmd);
            check(false, label);
        }
    }
    fclose(f);
}

/* ── main ────────────────────────────────────────────────────────────────── */

/*
 * main — find probe scripts under tests/ui/ and replay each one.
 *
 * Output: 0 when every expectation held, else non-zero.
 */
int
main(void)
{
    @autoreleasepool {
        check_data_dir();
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        memset(&probe_app, 0, sizeof probe_app);
        probe_app.first_line_title  = TRUE;
        probe_app.code_line_numbers = TRUE;
        probe_app.code_copy_buttons = TRUE;
        NotesNoteView *view = [[NotesNoteView alloc] init_with_app:&probe_app];
        view.pasteboard = [NSPasteboard pasteboardWithUniqueName];
        [view setFrameSize:NSMakeSize(probe_width, 800)];

        /* Find all *.txt files under tests/ui/ relative to CWD (the project
         * root, where make runs the test binary). */
        DIR *d = opendir("tests/ui");
        if (!d) {
            check(false, "tests/ui directory exists");
            return check_result();
        }

        /* Collect and sort the script names so the order is deterministic. */
        NSMutableArray *scripts = [NSMutableArray array];
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type != DT_REG) continue;
            const char *name = ent->d_name;
            size_t nlen = strlen(name);
            if (nlen < 4 || strcmp(name + nlen - 4, ".txt")) continue;
            [scripts addObject:@(name)];
        }
        closedir(d);
        [scripts sortUsingSelector:@selector(compare:)];

        for (NSString *name in scripts) {
            NSString *path = [NSString stringWithFormat:@"tests/ui/%@", name];
            printf("== %s\n", path.UTF8String);
            run_script(path.UTF8String, view);
        }

        [view.pasteboard releaseGlobally];
    }
    return check_result();
}
