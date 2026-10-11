/* ===========================================================================
 * ui_smoke_test.m — the AppKit side, driven in-process: windows open, show
 * true data, save on close, and LET GO of what they own
 *
 * Linked against every app object except main.o and run from a small test
 * bundle (so NSBundle finds the icons).  It builds the library window and
 * editors on the test database and checks, without any outside tool:
 *
 *   OWNERSHIP  a closed editor's window controller, window and note view
 *              are deallocated (weak references go nil) — a retain cycle
 *              or a timer holding its target would keep them alive;
 *   FLUSH      closing an editor saves an unsaved live edit, and deletes a
 *              note left blank; on_app_shutdown does the same for all;
 *   HEAP       forty open/close cycles of an editor on an image-bearing
 *              note do not grow the heap (a leaked document would add
 *              ~400 KB per cycle);
 *   IMAGES     after the library and its toolbar icons are on screen, no
 *              image-backing memory region is megabytes large — the
 *              signature of an NSImageView given a full-size image with no
 *              frame (see ui_util.h, THE RULE);
 *
 * and writes evidence a person can look at: dev/snapshots/library-light.png,
 * library-dark.png, editor-light.png and editor-dark.png, the windows as
 * drawn under each appearance.
 *
 * It needs no Screen Recording, Accessibility or Developer Tools
 * permission.  The outside tools (`leaks`, `vmmap`, `footprint`) stop for an
 * authorisation prompt on a Mac with developer mode off, so these in-process
 * checks are the ones an unattended run can make.  Run alone: make ui-check.
 * =========================================================================== */

#import <Cocoa/Cocoa.h>

#include "check.h"

#import "activation.h"
#import "editor_window.h"
#import "image_viewer.h"
#import "library_ai.h"
#import "library_commands.h"
#import "library_notes.h"
#import "library_sidebar.h"
#import "library_state.h"
#import "library_window.h"
#import "media_window.h"
#import "note_view.h"
#import "search_window.h"
#import "settings_window.h"
#import "ui_util.h"

#include "action_edit.h"
#include "look.h"
#include "app_lifecycle.h"
#include "serialize.h"

#include <mach/mach.h>
#include <objc/message.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>

/* How many open/close cycles the heap check warms up with, and then
 * measures.  The warm-up is a full batch: AppKit's caches (and the
 * library window's, which every editor cycle touches) fill over the first
 * few dozen cycles — 30 to 70 KB a cycle, more after an appearance switch
 * — and level off after that (measured: the second batch grows 1–2 KB a
 * cycle, the third not at all).  A five-cycle warm-up measured the filling
 * and failed at random. */
#define HEAP_CYCLES 40

/* A leaked editor on the image note costs hundreds of KB a cycle (its
 * picture alone is 384 KB); after the warm-up the steady state is within
 * ±3 KB, so this catches any real leak with room for noise. */
#define HEAP_SLACK_PER_CYCLE (8 * 1024)

/* A decoded-image region this large means a full-size image was rendered
 * for a small view (toolbar icons are a few KB each; the bundled artwork
 * is 512 px square, 1 MB decoded). */
#define IMAGE_REGION_LIMIT (1000 * 1024)

static OnApp app;                    /* the test's application context */

/*
 * pump — let the run loop turn: timers, main-queue posts, drawing and the
 * autorelease pool.
 *
 * Inputs:
 *   seconds — how long
 */
static void
pump(double seconds)
{
    @autoreleasepool {
        check_pump(NULL, seconds);
    }
}

/*
 * heap_in_use — bytes the malloc zones have handed out.
 *
 * Output: the count.
 */
static size_t
heap_in_use(void)
{
    malloc_statistics_t stats = { 0 };
    malloc_zone_statistics(NULL, &stats);
    return stats.size_in_use;
}

/*
 * largest_image_region — the largest virtual-memory region of this process
 * holding DECODED IMAGE pixels (the CGImage, ImageIO and CoreUI tags that
 * `vmmap <pid>` prints as region types).  Layer backing stores are not
 * counted: a window's own layers are legitimately megabytes.
 *
 * Output: its size in bytes; 0 when there is none.
 */
static mach_vm_size_t
largest_image_region(void)
{
    mach_vm_address_t address = 0;
    mach_vm_size_t    largest = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        natural_t      depth = 2048;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) {
            break;
        }
        BOOL image = info.user_tag == VM_MEMORY_CGIMAGE || info.user_tag == VM_MEMORY_IMAGEIO ||
                     info.user_tag == VM_MEMORY_COREUI_CACHED_IMAGE_DATA;
        if (image && size > largest) largest = size;
        address += size;
    }
    return largest;
}

/*
 * snapshot_dir — where the PNG snapshots go: dev/snapshots, beside the
 * dev/test folder this test's data directory is in.
 *
 * Output: the folder, created if missing.
 */
static NSString *
snapshot_dir(void)
{
    NSString *dev = @(check_data_dir()).stringByDeletingLastPathComponent
                        .stringByDeletingLastPathComponent;
    NSString *dir = [dev stringByAppendingPathComponent:@"snapshots"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
    return dir;
}

/*
 * save_png — a window's content, as drawn, to a PNG file.
 *
 * Inputs:
 *   window — the window
 *   path   — where to write
 *
 * Output: YES when the file was written.
 */
static BOOL
save_png(NSWindow *window, NSString *path)
{
    /* Its own pool: the bitmap is megabytes and must be gone before the
     * image-region check looks. */
    @autoreleasepool {
        NSView *view = window.contentView;
        [view layoutSubtreeIfNeeded];
        NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
        /* The window's own background is not part of its content view:
         * paint it first, or light text lands on a transparent page. */
        [view.effectiveAppearance performAsCurrentDrawingAppearance:^{
            NSGraphicsContext *saved = NSGraphicsContext.currentContext;
            NSGraphicsContext.currentContext =
                [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
            [NSColor.windowBackgroundColor setFill];
            NSRectFill(NSMakeRect(0, 0, rep.pixelsWide, rep.pixelsHigh));
            NSGraphicsContext.currentContext = saved;
        }];
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
        return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
            writeToFile:path atomically:YES];
    }
}

/*
 * save_snapshots — a window's content as drawn under the light and the
 * dark appearance, to <name>-light.png and <name>-dark.png.
 *
 * Inputs:
 *   window — the window
 *   name   — the files' base name
 *
 * Output: YES when both files were written.
 */
static BOOL
save_snapshots(NSWindow *window, NSString *name)
{
    BOOL ok = YES;
    NSDictionary<NSString *, NSAppearanceName> *looks = @{
        @"light": NSAppearanceNameAqua, @"dark": NSAppearanceNameDarkAqua,
    };
    for (NSString *look in looks) {
        window.appearance = [NSAppearance appearanceNamed:looks[look]];
        pump(0.2);                   /* what redraws on its own: grid cards */
        [window displayIfNeeded];
        NSString *file = [NSString stringWithFormat:@"%@-%@.png", name, look];
        ok = save_png(window, [snapshot_dir() stringByAppendingPathComponent:file]) && ok;
    }
    window.appearance = nil;         /* back to the system's */
    return ok;
}

/*
 * editor_window_titled — an open editor's window, found the way a user
 * would: by its title.
 *
 * Inputs:
 *   title — the note's title
 *
 * Output: the window, or nil.
 */
static NSWindow *
editor_window_titled(NSString *title)
{
    for (NSWindow *window in NSApp.windows) {
        if (window.visible && [window.title isEqualToString:title]) return window;
    }
    return nil;
}

/*
 * find_view — the first view of a class in a view tree, depth first.
 *
 * Inputs:
 *   root    — where to start (itself included)
 *   cls     — the class wanted
 *   exclude — a subclass of cls to skip (e.g. NSOutlineView when looking
 *             for a plain table); Nil = none
 *
 * Output: the view, or nil.
 */
static NSView *
find_view(NSView *root, Class cls, Class exclude)
{
    if ([root isKindOfClass:cls] && !(exclude && [root isKindOfClass:exclude])) return root;
    for (NSView *sub in root.subviews) {
        NSView *found = find_view(sub, cls, exclude);
        if (found) return found;
    }
    return nil;
}

/*
 * return_key — a synthetic Return key-down for a window.
 *
 * Inputs:
 *   window — the window the event is for
 *
 * Output: the event.
 */
static NSEvent *
return_key(NSWindow *window)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
                           timestamp:0 windowNumber:window.windowNumber context:nil
                          characters:@"\r" charactersIgnoringModifiers:@"\r"
                           isARepeat:NO keyCode:36];
}

/*
 * new_note_in — a note in a folder with the given text, saved through
 * THE save.
 *
 * Inputs:
 *   folder_id — its folder (0 = the top level)
 *   text      — its content, one block per line
 *
 * Output: the note's id.
 */
static gint64
new_note_in(gint64 folder_id, const char *text)
{
    gint64      note_id = on_db_note_create(app.db, folder_id);
    OnDocument *doc     = on_document_from_text(text);
    on_note_document_save(app.db, note_id, doc, TRUE, NULL);
    on_document_free(doc);
    return note_id;
}

/*
 * new_note — a top-level note with the given text.
 *
 * Inputs:
 *   text — its content, one block per line
 *
 * Output: the note's id.
 */
static gint64
new_note(const char *text)
{
    return new_note_in(0, text);
}

/*
 * add_image — append an image block to a note (the app's own icon, at its
 * full 384 KB, so a leaked document is unmissable).
 *
 * Inputs:
 *   note_id — the note
 */
static void
add_image(gint64 note_id)
{
    NSData *png = [NSData dataWithContentsOfURL:
        [NSBundle.mainBundle URLForResource:@"composition" withExtension:@"png"]];
    GBytes     *bytes = g_bytes_new(png.bytes, png.length);
    OnDocument *doc   = on_note_document_load(app.db, note_id);
    on_document_insert_block(doc, on_document_n_blocks(doc), on_block_new_image(bytes, 0));
    on_note_document_save(app.db, note_id, doc, TRUE, NULL);
    on_document_free(doc);
    g_bytes_unref(bytes);
}

/* Forward declaration — defined after test_library which calls it. */

/*
 * test_library — the library window comes up with true data, and goes away
 * completely.
 *
 * Inputs:
 *   lib — the library
 */
static void
test_library(NotesLibrary *lib)
{
    __weak NSWindow *weak_window = nil;
    __weak id weak_controller    = nil;
    @autoreleasepool {
        on_library_window_show(lib);
        pump(0.6);
        NSWindow *window = lib.window;
        check(window != nil && window.visible, "the library window is on screen");
        check(lib.sel_kind == on_sb_root, "the library opens on the Notes root, as the GTK app does");
        check(window.toolbar.items.count >= 12, "its toolbar is built from the command table");
        NSImage *icon = on_ui_icon(@"newnote", on_look_toolbar_icon);
        check(icon != nil, "toolbar icons load from the bundle");
        NSBitmapImageRep *rep = (NSBitmapImageRep *)icon.representations.firstObject;
        check(icon.size.width == on_look_toolbar_icon && rep.pixelsWide <= on_look_toolbar_icon * 3,
              "an icon is decoded at its display size, not at the file's 512 px");
        check(on_ui_icon(@"newnote", on_look_toolbar_icon) == icon, "and is decoded once");
        check(save_snapshots(window, @"library"), "library snapshots written, light and dark");

        NSImageView *image_view = on_ui_image_view(icon, 24, 24);
        check(NSEqualSizes(image_view.frame.size, NSMakeSize(24, 24)),
              "on_ui_image_view makes its view at the final size");

        weak_window     = window;
        weak_controller = window.windowController;
        [window close];
    }
    pump(0.3);
    check(lib.window == nil && weak_window == nil && weak_controller == nil,
          "closing the library releases its window and controller");
    on_library_window_show(lib);
    pump(0.3);
    check(lib.window != nil, "and it can be shown again");
}

/*
 * test_editor — open, live edit, close: saved and released.
 */
static void
test_editor(void)
{
    gint64 note_id = new_note("Smoke note\n! An action to tick\nbody text");
    __weak NSWindow *weak_window = nil;
    __weak id weak_controller    = nil;
    __weak NotesNoteView *weak_view = nil;
    @autoreleasepool {
        check(on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil),
              "an editor opens on an existing note");
        check(!on_editor_window_open(&app, 999999, NotesEditorRevealNone, 0, nil),
              "and refuses a note that does not exist");
        pump(0.4);
        NSWindow *window = editor_window_titled(@"Smoke note");
        check(window != nil, "its window carries the note's title");
        weak_window     = window;
        weak_controller = window.windowController;
        NSScrollView *scroll = (NSScrollView *)find_view(window.contentView, NSScrollView.class, Nil);
        weak_view       = scroll.documentView;
        check([weak_view isKindOfClass:NotesNoteView.class], "the note view is its document view");
        check(window.toolbar == nil && window.titlebarAccessoryViewControllers.count == 0,
              "an editor has an ordinary title bar: no toolbar, no titlebar accessory");
        check(save_snapshots(window, @"editor"), "editor snapshots written, light and dark");

        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        NSInteger count = 0;
        for (NSWindow *w in NSApp.windows) {
            if (w.visible && [w.title isEqualToString:@"Smoke note"]) count++;
        }
        check(count == 1, "opening an open note does not open a second editor");

        /* An edit from outside, through the hooks: live, not yet stored. */
        GList *items = on_db_action_list_for_note(app.db, note_id);
        gint64 uid   = ((OnActionItem *)items->data)->uid;
        on_db_action_list_free(items);
        check(app.live_document(&app, note_id) == on_note_view_document(weak_view),
              "live_document returns the open editor's document");
        check(app.live_document(&app, note_id + 1000) == NULL, "and NULL for a note not open");
        check(on_action_edit_set_done(&app, uid, TRUE), "an action is completed through the editor");
        OnDocument *stored = on_note_document_load(app.db, note_id);
        gboolean done = TRUE;
        on_document_block_action(stored, 1, NULL, NULL, &done);
        on_document_free(stored);
        check(!done, "the stored note is untouched until the editor saves");

        on_editor_close_notes(&note_id, 1);
    }
    pump(0.3);
    OnDocument *stored = on_note_document_load(app.db, note_id);
    gboolean done = FALSE;
    on_document_block_action(stored, 1, NULL, NULL, &done);
    on_document_free(stored);
    check(done, "closing the editor saved the unsaved edit");
    check(weak_window == nil && weak_controller == nil && weak_view == nil,
          "a closed editor's controller, window and view are deallocated");

    /* Autosave: an edit is written by the timer, with the editor open. */
    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        GList *items = on_db_action_list_for_note(app.db, note_id);
        gint64 uid   = ((OnActionItem *)items->data)->uid;
        on_db_action_list_free(items);
        on_action_edit_set_text(&app, uid, "Renamed while open");
    }
    pump(1.8);
    gchar *body = on_db_note_body_text(app.db, note_id);
    check(body != NULL && strstr(body, "Renamed while open") != NULL,
          "the autosave timer writes an edit while the editor stays open");
    g_free(body);

    /* A note left blank is deleted on close; shutdown flushes everything. */
    gint64 blank = on_db_note_create(app.db, 0);
    @autoreleasepool {
        on_editor_window_open(&app, blank, NotesEditorRevealNone, 0, nil);
        GList *items = on_db_action_list_for_note(app.db, note_id);
        gint64 uid   = ((OnActionItem *)items->data)->uid;
        on_db_action_list_free(items);
        on_action_edit_set_text(&app, uid, "Flushed by close-all");
        app.flush_editors(&app);
    }
    pump(0.3);
    OnNoteMeta *gone = on_db_note_get(app.db, blank);
    check(gone == NULL, "an editor closed on a blank note deletes the note");
    body = on_db_note_body_text(app.db, note_id);
    check(body != NULL && strstr(body, "Flushed by close-all") != NULL,
          "flush_editors saves every open editor before it returns");
    g_free(body);
}

/*
 * folder_count — how many folders sit directly under a parent.
 *
 * Inputs:
 *   parent_id — the parent folder (0 = the top level)
 *
 * Output: the count.
 */
static guint
folder_count(gint64 parent_id)
{
    GList *folders = on_db_folder_list(app.db, parent_id);
    guint  n       = g_list_length(folders);
    on_db_folder_list_free(folders);
    return n;
}

/*
 * default_button — a window's Return-key button.
 *
 * Inputs:
 *   window — the window
 *
 * Output: the button, or nil.
 */
static NSButton *
default_button(NSWindow *window)
{
    NSMutableArray<NSView *> *todo = [NSMutableArray arrayWithObject:window.contentView];
    while (todo.count > 0) {
        NSView *v = todo.lastObject;
        [todo removeLastObject];
        if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).keyEquivalent isEqualToString:@"\r"])
            return (NSButton *)v;
        [todo addObjectsFromArray:v.subviews];
    }
    return nil;
}

/*
 * command_item — a menu item standing for a command, as the menu bar
 * makes it.
 *
 * Inputs:
 *   ident — the command
 *
 * Output: the item.
 */
static NSMenuItem *
command_item(NSString *ident)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:ident action:@selector(run_command:)
                                           keyEquivalent:@""];
    item.identifier = ident;
    return item;
}

/*
 * key_event — a synthetic key-down for a window, as the keyboard makes it.
 *
 * Inputs:
 *   window   — the window the event is for
 *   key_code — the virtual key code (Carbon kVK_*)
 *   chars    — the characters it types (NSEvent function-key characters
 *              for arrows, Home, End …)
 *   mods     — modifier flags
 *
 * Output: the event.
 */
static NSEvent *
key_event(NSWindow *window, unsigned short key_code, NSString *chars, NSEventModifierFlags mods)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:mods
                           timestamp:0 windowNumber:window.windowNumber context:nil
                          characters:chars charactersIgnoringModifiers:chars
                           isARepeat:NO keyCode:key_code];
}

/*
 * send_command — run a command the way a menu item with no target does:
 * up the responder chain from the window's first responder (the test app
 * has no key window for NSApp's own routing).
 *
 * Inputs:
 *   window — the window the command is for
 *   item   — the item standing for the command
 *
 * Output: YES when something in the chain ran it.
 */
static BOOL
send_command(NSWindow *window, NSMenuItem *item)
{
    return [window.firstResponder tryToPerform:@selector(run_command:) with:item];
}

/*
 * label_with_text — a label in a view tree showing exactly this text.
 *
 * Inputs:
 *   root — where to look
 *   text — the text
 *
 * Output: the label, or nil.
 */
static NSTextField *
label_with_text(NSView *root, NSString *text)
{
    if ([root isKindOfClass:NSTextField.class] && [((NSTextField *)root).stringValue isEqualToString:text])
        return (NSTextField *)root;
    for (NSView *sub in root.subviews) {
        NSTextField *found = label_with_text(sub, text);
        if (found) return found;
    }
    return nil;
}

/*
 * status_text_centred — is a status label 9 pt, with its capitals centred
 * in the status band: from the window's bottom edge up to the separator
 * above the bar?
 *
 * Inputs:
 *   label — the label
 *
 * Output: YES when it is (within half a point).
 */
static BOOL
status_text_centred(NSTextField *label)
{
    NSView *content = label.window.contentView;
    [content layoutSubtreeIfNeeded];
    CGFloat top = 0;                 /* the separator's bottom edge, the band's top */
    for (NSView *v in content.subviews) {
        NSRect line = [v alignmentRectForFrame:v.frame];   /* where layout put the rule */
        if ([v isKindOfClass:NSBox.class] && ((NSBox *)v).boxType == NSBoxSeparator &&
            NSMinY(line) < 40 && NSMinY(line) > top)
            top = NSMinY(line);
    }
    NSRect  frame    = [label convertRect:label.bounds toView:content];
    CGFloat baseline = NSMaxY(frame) - label.firstBaselineOffsetFromTop;
    CGFloat caps_mid = baseline + label.font.capHeight / 2.0;
    return label.font.pointSize == on_look_status_font && top > 0 && fabs(caps_mid - top / 2.0) <= 0.5;
}

/*
 * test_editor_commands — an editor runs its commands through the same
 * path the menu bar uses, gates them while the find field is typed in,
 * shows its location and save state, and opens at the bottom-right.
 */
static void
test_editor_commands(void)
{
    gint64 note_id = new_note("Commands note\nfind me here and find me there");
    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        NSWindow *window = editor_window_titled(@"Commands note");
        NSWindowController<NSUserInterfaceValidations> *ed = (id)window.windowController;
        NotesNoteView *view = (NotesNoteView *)find_view(window.contentView, NotesNoteView.class, Nil);
        check(window != nil && view != nil && window.firstResponder == view,
              "a new editor opens with the note focused");

        NSMenuItem *bold = command_item(@"inline-bold");
        check([ed validateUserInterfaceItem:bold], "Bold is enabled in an editor");
        send_command(window, bold);
        check((on_note_view_inline_flags(view) & ON_FMT_BOLD) != 0,
              "Bold from the menu reaches the note view");

        NSMenuItem *find = command_item(@"find");
        send_command(window, find);
        NSSearchField *field = (NSSearchField *)find_view(window.contentView, NSSearchField.class, Nil);
        check([window.firstResponder isKindOfClass:NSText.class] &&
              ((NSText *)window.firstResponder).delegate == (id)field, "Find puts the focus in the find field");
        check(![ed validateUserInterfaceItem:bold], "editing commands are off while the find field has the focus");

        field.stringValue = @"find";
        [NSApp sendAction:field.action to:field.target from:field];
        NSMenuItem *next = command_item(@"find-next");
        check([ed validateUserInterfaceItem:next], "Find Next is enabled with text in the field");
        OnPos first = on_note_view_caret(view);          /* typing selected the first */
        send_command(window, next);
        OnPos second = on_note_view_caret(view);
        send_command(window, command_item(@"find-previous"));
        OnPos back = on_note_view_caret(view);
        check(first.block == 1 && second.block == 1 && second.offset > first.offset &&
              back.offset == first.offset,
              "typing in the find field selects the first match; Next and Previous step from it");

        check(label_with_text(window.contentView, @"/") != nil,
              "the status bar shows the note's location");
        check(status_text_centred(label_with_text(window.contentView, @"/")),
              "the editor's status text is 9 pt and centred in its bar");
        NSTextField *dot = label_with_text(window.contentView, @"●");
        check(dot != nil && [dot.toolTip isEqualToString:@"Saved"], "and a green Saved dot");

        /* Real key events, through AppKit's key bindings into the view. */
        [window makeFirstResponder:view];
        unichar right_arrow = NSRightArrowFunctionKey, home = NSHomeFunctionKey;
        [view keyDown:key_event(window, 0x7C, [NSString stringWithCharacters:&right_arrow length:1],
                                NSEventModifierFlagCommand | NSEventModifierFlagFunction)];
        OnPos line_end = on_note_view_caret(view);
        [view keyDown:key_event(window, 0x73, [NSString stringWithCharacters:&home length:1],
                                NSEventModifierFlagFunction)];
        OnPos line_start = on_note_view_caret(view);
        check(line_end.offset > 0 && line_start.offset == 0 && line_start.block == line_end.block,
              "⌘→ and Home, as real key events, go to the line's end and start");
        [view keyDown:key_event(window, 0x7C, [NSString stringWithCharacters:&right_arrow length:1],
                                NSEventModifierFlagCommand | NSEventModifierFlagFunction)];
        gsize before_delete = on_document_block(on_note_view_document(view), line_end.block)->text->text->len;
        [view keyDown:key_event(window, 0x33, @"\x7f", NSEventModifierFlagOption)];
        gsize after_delete = on_document_block(on_note_view_document(view), line_end.block)->text->text->len;
        check(after_delete < before_delete && after_delete > 0,
              "⌥⌫ deletes one word, not the whole line");
        [NSApp sendAction:@selector(undo:) to:view from:nil];


        on_editor_close_notes(&note_id, 1);   /* saves first: then the image is added */
        add_image(note_id);
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        window = editor_window_titled(@"Commands note");
        NotesImageViewer *viewer = (NotesImageViewer *)find_view(window.contentView,
                                                                 NotesImageViewer.class, Nil);
        NSScrollView *scroll = (NSScrollView *)viewer.superview;
        on_image_viewer_open(viewer, 0);
        [window layoutIfNeeded];
        [window displayIfNeeded];
        NSImageView *big = (NSImageView *)find_view(viewer, NSImageView.class, Nil);
        check(on_image_viewer_is_open(viewer) && NSEqualSizes(viewer.frame.size, scroll.bounds.size) &&
              big.frame.size.width > 200 && big.frame.size.height > 200 &&
              NSContainsRect(viewer.bounds, big.frame),
              "the editor's image viewer covers the note and shows the picture big, inside it");
        NSTextField *hint    = nil;
        for (NSView *v in viewer.subviews)
            if ([v isKindOfClass:NSTextField.class] && [((NSTextField *)v).stringValue hasSuffix:@"ESC to close"])
                hint = (NSTextField *)v;
        NSTextField *open_it = label_with_text(viewer, @"Open in Image Viewer");
        __block NSColor *hint_rgb = nil, *link_rgb = nil;
        [[NSAppearance appearanceNamed:NSAppearanceNameAqua] performAsCurrentDrawingAppearance:^{
            hint_rgb = [hint.textColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            link_rgb = [[open_it.attributedStringValue attribute:NSForegroundColorAttributeName atIndex:0
                                                   effectiveRange:NULL]
                        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        }];
        check(hint != nil && hint_rgb.redComponent > 0.9 && NSMinY(hint.frame) < NSMinY(big.frame) &&
              open_it != nil && link_rgb.blueComponent > 0.9 && link_rgb.redComponent < 0.6 &&
              fabs(NSMaxX(open_it.frame) - NSMaxX(hint.frame) - on_look_viewer_gap - open_it.frame.size.width) < 1,
              "under the picture: a light caption with the key hint, and the action link in blue");
        check(save_snapshots(window, @"editor-viewer"), "editor image viewer snapshots written");
        on_image_viewer_close(viewer);

        NSRect area = window.screen.visibleFrame;
        check(fabs(NSMaxX(window.frame) - (NSMaxX(area) - on_look_editor_screen_margin)) < 1.0,
              "a new editor opens at the bottom-right of the screen");
        on_editor_close_notes(&note_id, 1);
    }
    pump(0.2);
}

/*
 * id_in — is a note or folder id in a GArray of gint64?  The array is
 * freed.
 *
 * Inputs:
 *   ids — the array (owned)
 *   id  — the id
 *
 * Output: YES when present.
 */
static BOOL
id_in(GArray *ids, gint64 id)
{
    BOOL found = NO;
    for (guint i = 0; i < ids->len; i++) found = found || g_array_index(ids, gint64, i) == id;
    g_array_free(ids, TRUE);
    return found;
}

/*
 * folder_under — is a folder a live child of a parent?
 *
 * Inputs:
 *   parent_id — the parent (0 = the top level)
 *   folder_id — the folder
 *
 * Output: YES when it is listed there.
 */
static BOOL
folder_under(gint64 parent_id, gint64 folder_id)
{
    GList *folders = on_db_folder_list(app.db, parent_id);
    BOOL   found   = NO;
    for (GList *l = folders; l; l = l->next) found = found || ((OnFolder *)l->data)->id == folder_id;
    on_db_folder_list_free(folders);
    return found;
}

/*
 * answer_panel — click a button of the tool panel on screen.
 *
 * Inputs:
 *   title — the button's title
 *
 * Output: YES when such a button was found and clicked.
 */
static BOOL
answer_panel(NSString *title)
{
    for (NSWindow *w in NSApp.windows) {
        if (!w.visible || ![w isKindOfClass:NSPanel.class]) continue;
        for (NSView *v in w.contentView.subviews) {
            if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).title isEqualToString:title]) {
                [(NSButton *)v performClick:nil];
                return YES;
            }
        }
    }
    return NO;
}

/*
 * test_trash — moving notes and folders to the Trash, restoring and
 * deleting them, always closes the editors open on them first; a restored
 * folder goes back where it was.
 *
 * Inputs:
 *   lib — the library (its window on screen)
 */
static void
test_trash(NotesLibrary *lib)
{
    gint64 parent = on_db_folder_create(app.db, 0, "Trash parent", 0, NULL);
    gint64 folder = on_db_folder_create(app.db, parent, "Trash child", 0, NULL);
    gint64 inside = new_note_in(folder, "Inside the folder\nbody");
    gint64 loose  = new_note("Loose note\nbody");
    on_library_refresh_all(lib);

    /* A note with an open editor, moved to the Trash from the list. */
    on_editor_window_open(&app, loose, NotesEditorRevealNone, 0, nil);
    pump(0.2);
    lib.sel_kind = on_sb_all;
    lib.sel_id   = 0;
    on_library_notes_refresh(lib);
    pump(0.2);
    NSTableView *table = (NSTableView *)find_view(lib.window.contentView, NSTableView.class,
                                                  NSOutlineView.class);
    NSInteger row = -1;
    for (NSInteger r = 0; r < table.numberOfRows; r++) {
        NSTableCellView *cell = [table viewAtColumn:[table columnWithIdentifier:@"title"] row:r
                                       makeIfNecessary:YES];
        if (label_with_text(cell, @"Loose note")) row = r;
    }
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    NSSearchField *library_search = (NSSearchField *)find_view(lib.window.contentView.superview,
                                                               NSSearchField.class, Nil);
    if (!library_search) {
        for (NSToolbarItem *ti in lib.window.toolbar.items) {
            if ([ti.view isKindOfClass:NSSearchField.class]) library_search = (NSSearchField *)ti.view;
        }
    }
    [lib.window makeFirstResponder:library_search];
    check(library_search != nil && !on_library_validate_command(lib, command_item(@"delete-note")),
          "⌘⌫ does not trash notes while the library's search field is being typed in");
    [lib.window makeFirstResponder:table];
    on_library_run_command(lib, command_item(@"delete-note"));
    pump(0.2);
    check(row >= 0 && id_in(on_db_trash_note_ids(app.db), loose) &&
          editor_window_titled(@"Loose note") == nil,
          "Move to Trash trashes the note and closes its open editor");

    /* A folder with an open editor on one of its notes. */
    on_editor_window_open(&app, inside, NotesEditorRevealNone, 0, nil);
    pump(0.2);
    on_library_sidebar_select(lib, on_sb_folder, folder);
    lib.sel_name = @"Trash child";
    on_library_run_command(lib, command_item(@"delete-folder"));
    pump(0.2);
    check(!folder_under(parent, folder) && editor_window_titled(@"Inside the folder") == nil &&
          lib.sel_kind == on_sb_root,
          "Delete Folder trashes it, closes its notes' editors and leaves the selection on Notes");

    on_library_sidebar_select(lib, on_sb_trash_folder, folder);
    lib.sel_name = @"Trash child";
    on_library_run_command(lib, command_item(@"folder-restore"));
    pump(0.2);
    check(folder_under(parent, folder) && lib.sel_kind == on_sb_folder && lib.sel_id == folder,
          "Restore Folder puts it back under its old parent and selects it");

    /* Empty Trash, with the loose note's editor open again. */
    on_editor_window_open(&app, loose, NotesEditorRevealNone, 0, nil);
    pump(0.2);
    on_library_run_command(lib, command_item(@"empty-trash"));
    pump(0.2);
    check(answer_panel(@"Empty Trash"), "Empty Trash asks first, in a tool panel");
    pump(0.3);
    check(!id_in(on_db_trash_note_ids(app.db), loose) && on_db_note_get(app.db, loose) == NULL &&
          editor_window_titled(@"Loose note") == nil,
          "and then deletes the Trash's notes, closing their editors");
}

/*
 * test_strike_snapshot — struck text (a body run, a heading run and a
 * completed action item), for the eye: the line goes through the middle
 * of the lowercase letters.
 */
static void
test_strike_snapshot(void)
{
    gint64      note_id = new_note("Strike note\nsome struck words here\nA struck heading\n! finished item");
    OnDocument *doc     = on_note_document_load(app.db, note_id);
    on_document_set_kind(doc, 2, ON_BLOCK_H2);
    on_document_set_flags(doc, (OnPos){ 1, -1, 5 }, strlen("struck words"), ON_FMT_STRIKE, TRUE);
    on_document_set_flags(doc, (OnPos){ 2, -1, 2 }, strlen("struck"), ON_FMT_STRIKE, TRUE);
    on_document_set_flags(doc, (OnPos){ 3, -1, 2 }, strlen("finished item"), ON_FMT_STRIKE, TRUE);
    on_note_document_save(app.db, note_id, doc, TRUE, NULL);
    on_document_free(doc);
    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        check(save_snapshots(editor_window_titled(@"Strike note"), @"editor-strike"),
              "strikethrough snapshots written");
        on_editor_close_all();
    }
    pump(0.2);
}

/*
 * test_accessibility — VoiceOver sees a note as a text area holding its
 * text, with the selection where the editor has it.
 */
static void
test_accessibility(void)
{
    gint64 note_id = new_note("Access note\nsome body text");
    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        NotesNoteView *view = (NotesNoteView *)find_view(editor_window_titled(@"Access note").contentView,
                                                         NotesNoteView.class, Nil);
        check(view.isAccessibilityElement && [view.accessibilityRole isEqualToString:NSAccessibilityTextAreaRole] &&
              [view.accessibilityValue isEqual:@"Access note\nsome body text"],
              "the note view is a text area holding the note's text");
        on_note_view_find(view, @"body");
        check([view.accessibilitySelectedText isEqualToString:@"body"] &&
              view.accessibilitySelectedTextRange.location == strlen("Access note\nsome ") &&
              view.accessibilityInsertionPointLineNumber == 1,
              "and reports the selection and the caret's line");
        on_editor_close_all();
    }
    pump(0.2);
}

/*
 * test_inline_image — an image inside a line of text (older notes have
 * them) is laid out in the line and can be opened by a click.
 */
static void
test_inline_image(void)
{
    gint64      note_id = new_note("Inline image\nbefore " ON_OBJ_CHAR " after");
    NSData     *png     = [NSData dataWithContentsOfURL:
        [NSBundle.mainBundle URLForResource:@"composition" withExtension:@"png"]];
    GBytes     *bytes   = g_bytes_new(png.bytes, png.length);
    OnDocument *doc     = on_note_document_load(app.db, note_id);
    on_text_add_image(on_document_block(doc, 1)->text, strlen("before "), bytes, 0);
    on_note_document_save(app.db, note_id, doc, TRUE, NULL);
    on_document_free(doc);
    g_bytes_unref(bytes);

    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        NSWindow      *window = editor_window_titled(@"Inline image");
        NotesNoteView *view   = (NotesNoteView *)find_view(window.contentView, NotesNoteView.class, Nil);
        check(view.frame.size.height > on_look_image_thumb_h,
              "an inline image takes its thumbnail's room in the line");
        check(save_snapshots(window, @"inline-image"), "inline image snapshots written");
        __block NSInteger opened = -1;
        void (^handler)(NSInteger) = view.image_activated_handler;
        view.image_activated_handler = ^(NSInteger ordinal) { opened = ordinal; };
        for (CGFloat x = 40; x < 400 && opened < 0; x += 20)
            on_note_view_feed_click(view, x, 120, 1, 0, NO);
        view.image_activated_handler = handler;
        check(opened == 0 && on_note_view_image(view, 0) != NULL && on_note_view_image_png(view, 0) != NULL &&
              on_note_view_image_reveal(view, 0) && on_note_view_caret(view).offset == strlen("before "),
              "a click on it opens it, and every image call finds it as image 0");

        /* A pasted TIFF image (what most apps put on the pasteboard) is
         * stored as PNG, the one format a note keeps. */
        view.pasteboard = [NSPasteboard pasteboardWithUniqueName];
        NSImage *picture = [[NSImage alloc] initWithData:png];
        [view.pasteboard clearContents];
        [view.pasteboard setData:picture.TIFFRepresentation forType:NSPasteboardTypeTIFF];
        on_note_view_feed_key(view, on_key_character, 'v', on_mod_command);
        OnDocument *pasted = on_note_view_document(view);
        BOOL png_block = NO;
        for (guint i = 0; i < on_document_n_blocks(pasted); i++) {
            const OnBlock *pb = on_document_block(pasted, i);
            if (pb->kind != ON_BLOCK_IMAGE) continue;
            gsize n;
            const guint8 *d = g_bytes_get_data(pb->png, &n);
            png_block = n > 8 && memcmp(d, "\x89PNG", 4) == 0;
        }
        check(png_block, "a pasted TIFF image is stored as PNG");
        [view.pasteboard releaseGlobally];
        on_editor_close_notes(&note_id, 1);
    }
    pump(0.2);
}

/*
 * mouse_moved — a synthetic mouse-moved event at a point of a view.
 *
 * Inputs:
 *   view — the view
 *   pt   — the point, in the view's coordinates
 *
 * Output: the event.
 */
static NSEvent *
mouse_moved(NSView *view, NSPoint pt)
{
    return [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                              location:[view convertPoint:pt toView:nil] modifierFlags:0
                             timestamp:0 windowNumber:view.window.windowNumber context:nil
                           eventNumber:0 clickCount:0 pressure:0];
}

/*
 * test_pointer — the pointer is a pointing hand over a task's box and an
 * I-beam over text.
 */
static void
test_pointer(void)
{
    gint64      note_id = new_note("Pointer note\nbuy milk");
    OnDocument *doc     = on_note_document_load(app.db, note_id);
    on_document_set_kind(doc, 1, ON_BLOCK_CHECK);
    on_note_document_save(app.db, note_id, doc, TRUE, NULL);
    on_document_free(doc);
    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        NSWindow      *window = editor_window_titled(@"Pointer note");
        NotesNoteView *view   = (NotesNoteView *)find_view(window.contentView, NotesNoteView.class, Nil);
        [view mouseMoved:mouse_moved(view, NSMakePoint(33, 47))];
        NSCursor *over_box = NSCursor.currentCursor;
        [view mouseMoved:mouse_moved(view, NSMakePoint(120, 47))];
        NSCursor *over_text = NSCursor.currentCursor;
        check(over_box == NSCursor.pointingHandCursor && over_text == NSCursor.IBeamCursor,
              "the pointer is a hand over a task's box and an I-beam over text");
        on_editor_close_notes(&note_id, 1);
    }
    pump(0.2);
}

/*
 * test_tag_suggestions — typing "#al" lists the known tags that start so,
 * under the caret; Down and Return choose one, which becomes a tag.
 */
static void
test_tag_suggestions(void)
{
    gint64 tagged = new_note("Tagged note\nbody");
    GList *tags = g_list_append(g_list_append(NULL, g_strdup("alpha")), g_strdup("alpine"));
    on_db_note_set_tags(app.db, tagged, tags);
    g_list_free_full(tags, g_free);
    gint64 note_id = new_note("Tag typing\n");
    @autoreleasepool {
        on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
        pump(0.3);
        NSWindow      *window = editor_window_titled(@"Tag typing");
        NotesNoteView *view   = (NotesNoteView *)find_view(window.contentView, NotesNoteView.class, Nil);
        on_note_view_feed_text(view, "#al");
        NSWindow *list = window.childWindows.firstObject;
        NSUInteger rows = 0;
        for (NSView *v in list.contentView.subviews) rows += [v isKindOfClass:NSButton.class];
        check(list.visible && rows == 2, "typing #al lists the two known tags starting with al");
        on_note_view_feed_key(view, on_key_down, 0, 0);
        on_note_view_feed_key(view, on_key_return, 0, 0);
        const OnBlock *b = on_document_block(on_note_view_document(view), 1);
        check(!list.visible && strcmp(b->text->text->str, "#alpine ") == 0 &&
              (on_text_flags_at(b->text, 1) & ON_FMT_TAG),
              "Down and Return choose #alpine, styled as a tag, and close the list");
        on_editor_close_notes(&note_id, 1);
    }
    pump(0.2);
}

/*
 * view_with_identifier — a view in a view tree, by its identifier.
 *
 * Inputs:
 *   root       — where to look
 *   identifier — the identifier
 *
 * Output: the view, or nil.
 */
static NSView *
view_with_identifier(NSView *root, NSString *identifier)
{
    if ([root.identifier isEqualToString:identifier]) return root;
    for (NSView *sub in root.subviews) {
        NSView *found = view_with_identifier(sub, identifier);
        if (found) return found;
    }
    return nil;
}

/*
 * button_titled — a button in a view tree, by its title.
 *
 * Inputs:
 *   root  — where to look
 *   title — the title
 *
 * Output: the button, or nil.
 */
static NSButton *
button_titled(NSView *root, NSString *title)
{
    if ([root isKindOfClass:NSButton.class] && [((NSButton *)root).title isEqualToString:title])
        return (NSButton *)root;
    for (NSView *sub in root.subviews) {
        NSButton *found = button_titled(sub, title);
        if (found) return found;
    }
    return nil;
}

/*
 * test_new_folder — New Folder asks in the app's own tool panel, never a
 * native alert or sheet; an empty name keeps it open, a name creates the
 * folder and closes it.
 *
 * Inputs:
 *   lib — the library (must have a window on screen)
 */
static void
test_new_folder(NotesLibrary *lib)
{
    gint64 parent = on_library_current_folder_id(lib);
    guint  before = folder_count(parent);
    __weak NSWindow *weak_panel = nil;
    @autoreleasepool {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"New Folder…" action:NULL
                                               keyEquivalent:@""];
        item.identifier = @"new-folder";
        on_library_run_command(lib, item);
        pump(0.3);

        NSWindow *panel = nil;
        for (NSWindow *w in NSApp.windows) {
            if (w.visible && [w.title isEqualToString:@"New Folder"]) panel = w;
        }
        check(panel != nil && [panel isKindOfClass:NSPanel.class] && lib.window.attachedSheet == nil,
              "New Folder asks in a tool panel, not an alert sheet");
        check(save_snapshots(panel, @"new-folder"), "new folder snapshots written, light and dark");

        NSTextField *field = (NSTextField *)view_with_identifier(panel.contentView, @"folder-name");
        NSTextField *emoji = (NSTextField *)view_with_identifier(panel.contentView, @"folder-emoji");
        NSButton *create = default_button(panel);
        NSButton *project = button_titled(panel.contentView, @"Project");
        check(field != nil && project != nil && [create.title isEqualToString:@"OK"],
              "it asks for an emoji, a name and the AI summary mode (GTK's folder prompt)");
        [create performClick:nil];
        pump(0.1);
        check(panel.visible && folder_count(parent) == before,
              "an empty name keeps the panel open and creates nothing");

        field.stringValue = @"  Smoke folder  ";
        [project performClick:nil];
        emoji.stringValue = @"🧪";
        [create performClick:nil];
        pump(0.2);
        check(!panel.visible && folder_count(parent) == before + 1,
              "a name creates the folder and closes the panel");
        GList *folders = on_db_folder_list(app.db, parent);
        gint64 made = 0;
        for (GList *l = folders; l; l = l->next)
            if (strcmp(((OnFolder *)l->data)->name, "Smoke folder") == 0) made = ((OnFolder *)l->data)->id;
        on_db_folder_list_free(folders);
        gchar *made_emoji = made ? on_db_folder_get_emoji(app.db, made) : g_strdup("");
        check(made != 0 && strcmp(made_emoji, "🧪") == 0 &&
              on_db_folder_get_ai_mode(app.db, made) == ON_AI_MODE_PROJECT,
              "the name is trimmed, and the emoji and AI mode are stored with it");
        g_free(made_emoji);
        weak_panel = panel;
    }
    pump(0.2);
    check(weak_panel == nil, "a closed tool panel is deallocated");
}

/*
 * NotesFakeDrag — just enough of an NSDraggingInfo for the sidebar's drop
 * methods: a pasteboard and a location.
 */
@interface NotesFakeDrag : NSObject
@property (nonatomic, strong) NSPasteboard *pasteboard;
@property (nonatomic, assign) NSPoint       location;
@end

@implementation NotesFakeDrag

/*
 * draggingPasteboard — the drag's pasteboard.
 *
 * Output: the pasteboard.
 */
- (NSPasteboard *)
draggingPasteboard
{
    return self.pasteboard;
}

/*
 * draggingLocation — where the pointer is, in window coordinates.
 *
 * Output: the point.
 */
- (NSPoint)
draggingLocation
{
    return self.location;
}

@end

/*
 * sidebar_row — the sidebar row with a name: its text without the emoji in
 * front (up to two spaces) and the count behind.
 *
 * Inputs:
 *   outline — the sidebar
 *   name    — the row's name (e.g. @"Notes", @"Crate")
 *
 * Output: the row, or -1.
 */
static NSInteger
sidebar_row(NSOutlineView *outline, NSString *name)
{
    for (NSInteger r = 0; r < outline.numberOfRows; r++) {
        NSString *text = ((NSTableCellView *)[outline viewAtColumn:0 row:r makeIfNecessary:YES]).textField.stringValue;
        NSRange   gap  = [text rangeOfString:@"  "];
        if (gap.location == NSNotFound) gap = [text rangeOfString:@"\u00a0 "];
        if (gap.location != NSNotFound) text = [text substringFromIndex:NSMaxRange(gap)];
        NSRange count = [text rangeOfString:@" (" options:NSBackwardsSearch];
        if (count.location != NSNotFound && [text hasSuffix:@")"]) text = [text substringToIndex:count.location];
        if ([text isEqualToString:name]) return r;
    }
    return -1;
}

/*
 * drag_of — a drag of library rows, on a private pasteboard, over a row.
 *
 * Inputs:
 *   outline — the sidebar
 *   kind    — what is dragged
 *   ids     — the ids
 *   row     — the row under the pointer
 *
 * Output: the drag (cast to NSDraggingInfo where used).
 */
static NotesFakeDrag *
drag_of(NSOutlineView *outline, on_drag_kind_t kind, NSArray<NSNumber *> *ids, NSInteger row)
{
    NotesFakeDrag *drag = [[NotesFakeDrag alloc] init];
    drag.pasteboard = [NSPasteboard pasteboardWithUniqueName];
    [drag.pasteboard clearContents];
    [drag.pasteboard writeObjects:@[on_library_drag_item(kind, ids)]];
    NSRect rect = [outline rectOfRow:row];
    drag.location = [outline convertPoint:NSMakePoint(NSMidX(rect), NSMidY(rect)) toView:nil];
    return drag;
}

/*
 * drop — offer a drag to the sidebar as AppKit does: validate, then
 * accept when valid.
 *
 * Inputs:
 *   outline — the sidebar
 *   drag    — the drag
 *   row     — the proposed row (the drop parent), or -1 for the top level
 *   index   — the child index, or NSOutlineViewDropOnItemIndex
 *
 * Output: YES when the drop was accepted.
 */
static BOOL
drop(NSOutlineView *outline, NotesFakeDrag *drag, NSInteger row, NSInteger index)
{
    id item = row >= 0 ? [outline itemAtRow:row] : nil;
    id<NSOutlineViewDataSource> ds = outline.dataSource;
    id<NSDraggingInfo> info = (id<NSDraggingInfo>)drag;
    if ([ds outlineView:outline validateDrop:info proposedItem:item proposedChildIndex:index] == NSDragOperationNone)
        return NO;
    BOOL ok = [ds outlineView:outline acceptDrop:info item:item childIndex:index];
    pump(0.2);
    [drag.pasteboard releaseGlobally];
    return ok;
}

/*
 * menu_titles — a menu's item titles, separators left out.
 *
 * Inputs:
 *   menu — the menu, or nil
 *
 * Output: the titles.
 */
static NSArray<NSString *> *
menu_titles(NSMenu *menu)
{
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    for (NSMenuItem *item in menu.itemArray)
        if (!item.separatorItem) [titles addObject:item.title];
    return titles;
}

/*
 * right_click — the menu a right click on a table row shows (a sidebar or
 * notes row).
 *
 * Inputs:
 *   outline — the table
 *   row     — the row
 *
 * Output: the menu, or nil.
 */
static NSMenu *
right_click(NSTableView *outline, NSInteger row)
{
    NSRect   rect  = [outline rectOfRow:row];
    NSEvent *event = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                        location:[outline convertPoint:NSMakePoint(NSMidX(rect), NSMidY(rect)) toView:nil]
                                   modifierFlags:0 timestamp:0 windowNumber:outline.window.windowNumber
                                         context:nil eventNumber:0 clickCount:1 pressure:1];
    return [outline menuForEvent:event];
}

/*
 * test_sidebar — the sidebar's rows, menus, commands and drops, as the
 * GTK app has them; the status bar follows the selection; the sidebar
 * fits its widest row.
 *
 * Inputs:
 *   lib — the library (its window on screen)
 */
static void
test_sidebar(NotesLibrary *lib)
{
    NSOutlineView *outline = (NSOutlineView *)find_view(lib.window.contentView, NSOutlineView.class, Nil);
    gint64 box   = on_db_folder_create(app.db, 0, "Box", 0, NULL);
    gint64 inner = on_db_folder_create(app.db, box, "Inner", 0, NULL);
    gint64 zeta  = on_db_folder_create(app.db, box, "Zeta", 0, NULL);
    gint64 alpha = on_db_folder_create(app.db, box, "Alpha", 0, NULL);
    gint64 loose = new_note("Loose for drops\nbody");
    on_library_refresh_all(lib);
    on_library_sidebar_select(lib, on_sb_folder, inner);
    pump(0.2);

    /* Menus, per kind of row. */
    NSInteger root = sidebar_row(outline, @"Notes");
    NSArray<NSString *> *root_menu = menu_titles(right_click(outline, root));
    check(lib.sel_kind == on_sb_root &&
          [root_menu isEqualToArray:@[@"New Folder…", @"Sort Subfolders Alphabetically", @"Search Here…"]],
          "a right click on the Notes root selects it and offers New Folder, Sort, Search Here");
    NSInteger all = sidebar_row(outline, @"All Notes");
    check([menu_titles(right_click(outline, all)) isEqualToArray:@[@"Search Here…"]],
          "All Notes offers Search Here");
    NSInteger box_row = sidebar_row(outline, @"Box");
    NSArray<NSString *> *folder_menu = menu_titles(right_click(outline, box_row));
    check(lib.sel_kind == on_sb_folder && lib.sel_id == box &&
          [folder_menu isEqualToArray:@[@"New Subfolder…", @"Info…", @"Move Folder to Trash",
                                        @"Sort Subfolders Alphabetically", @"Search Here…"]],
          "a right click on a folder selects it and offers its menu");
    check([lib.window.contentView.subviews count] > 0 && label_with_text(lib.window.contentView, @"/Box") != nil,
          "the status bar shows the selected folder's path");

    /* Sort Subfolders: Zeta, Alpha (and Inner) become Alpha, Inner, Zeta. */
    on_library_run_command(lib, command_item(@"sort-subfolders"));
    pump(0.1);
    GList *kids = on_db_folder_list(app.db, box);
    check(kids && ((OnFolder *)kids->data)->id == alpha && ((OnFolder *)g_list_last(kids)->data)->id == zeta,
          "Sort Subfolders orders a folder's children alphabetically");
    on_db_folder_list_free(kids);

    /* Folder Info: filled in from the folder, and saved. */
    on_library_run_command(lib, command_item(@"folder-info"));
    pump(0.2);
    NSWindow *info = nil;
    for (NSWindow *w in NSApp.windows) if (w.visible && [w.title isEqualToString:@"Folder Info"]) info = w;
    NSTextField *name = (NSTextField *)view_with_identifier(info.contentView, @"folder-name");
    check([name.stringValue isEqualToString:@"Box"], "Folder Info opens on the folder's name");
    name.stringValue = @"Crate";
    ((NSTextField *)view_with_identifier(info.contentView, @"folder-emoji")).stringValue = @"📦";
    [default_button(info) performClick:nil];
    pump(0.2);
    gchar *emoji = on_db_folder_get_emoji(app.db, box);
    check(strcmp(emoji, "📦") == 0 && sidebar_row(outline, @"Crate") >= 0 &&
          [((NSTableCellView *)[outline viewAtColumn:0 row:sidebar_row(outline, @"Crate") makeIfNecessary:YES]).textField.stringValue hasPrefix:@"📦  Crate"],
          "Folder Info renames the folder and gives it an emoji, shown in the sidebar");
    g_free(emoji);

    /* Drops. */
    NSInteger crate = sidebar_row(outline, @"Crate");
    [outline expandItem:[outline itemAtRow:crate]];
    check(drop(outline, drag_of(outline, on_drag_notes, @[@(loose)], crate), crate, NSOutlineViewDropOnItemIndex),
          "notes dropped on a folder are accepted");
    OnNoteMeta *meta = on_db_note_get(app.db, loose);
    check(meta && meta->folder_id == box, "and move into it");
    on_db_note_meta_free(meta);

    crate = sidebar_row(outline, @"Crate");
    NSInteger alpha_row = sidebar_row(outline, @"Alpha");
    check(!drop(outline, drag_of(outline, on_drag_folder, @[@(box)], alpha_row), alpha_row, NSOutlineViewDropOnItemIndex),
          "a folder cannot be dropped into its own subfolder");
    NSInteger zeta_row = sidebar_row(outline, @"Zeta");
    check(drop(outline, drag_of(outline, on_drag_folder, @[@(zeta)], crate), crate, 0),
          "a folder dropped between two rows is accepted");
    kids = on_db_folder_list(app.db, box);
    check(kids && ((OnFolder *)kids->data)->id == zeta, "and goes before the row it was dropped above");
    on_db_folder_list_free(kids);

    NSInteger trash_target = sidebar_row(outline, @"Notes");
    check(drop(outline, drag_of(outline, on_drag_folder, @[@(alpha)], trash_target), trash_target,
               NSOutlineViewDropOnItemIndex) && folder_under(0, alpha),
          "a folder dropped on the Notes root moves to the top level");
    (void)zeta_row;

    NSInteger root_row = sidebar_row(outline, @"Notes");
    check(drop(outline, drag_of(outline, on_drag_notes, @[@(loose)], root_row), root_row, NSOutlineViewDropOnItemIndex),
          "notes dropped on the Notes root are accepted");
    meta = on_db_note_get(app.db, loose);
    check(meta && meta->folder_id == 0, "and move to the top level");
    on_db_note_meta_free(meta);

    on_library_trash_folder(lib, alpha, @"Alpha");
    on_library_refresh_all(lib);
    pump(0.2);
    NSInteger trash_row = sidebar_row(outline, @"Trash");
    [outline expandItem:[outline itemAtRow:trash_row]];
    root_row = sidebar_row(outline, @"Notes");
    check(drop(outline, drag_of(outline, on_drag_trashed_folder, @[@(alpha)], root_row), root_row,
               NSOutlineViewDropOnItemIndex) && folder_under(0, alpha),
          "a folder dragged out of the Trash onto the root is restored there");
    trash_row = -1;
    pump(0.2);
    gint64 doomed = new_note("Doomed note\nbody");
    on_library_trash_notes(lib, &doomed, 1);
    on_library_refresh_all(lib);
    pump(0.2);
    gint64 dropped = new_note("Dropped on trash\nbody");
    on_library_refresh_all(lib);
    pump(0.2);
    trash_row = sidebar_row(outline, @"Trash");
    check(drop(outline, drag_of(outline, on_drag_notes, @[@(dropped)], trash_row), trash_row,
               NSOutlineViewDropOnItemIndex) && id_in(on_db_trash_note_ids(app.db), dropped),
          "notes dropped on the Trash are trashed");

    /* Fit to content: a long folder name widens the sidebar. */
    app.sidebar_fit_content = TRUE;
    on_db_folder_create(app.db, 0, "A folder with a really quite remarkably long name indeed", 0, NULL);
    on_library_refresh_all(lib);
    [outline expandItem:[outline itemAtRow:sidebar_row(outline, @"Notes")]];
    pump(0.3);
    check(outline.enclosingScrollView.frame.size.width > on_look_sidebar_min_w + 40,
          "with fit-to-content on, the sidebar widens to a long folder name");
    app.sidebar_fit_content = FALSE;
}

/*
 * pane_table — the notes pane's visible table (the list or Action Items).
 *
 * Inputs:
 *   lib — the library
 *
 * Output: the table, or nil while the grid shows.
 */
static NSTableView *
pane_table(NotesLibrary *lib)
{
    return (NSTableView *)find_view(lib.window.contentView, NSTableView.class, NSOutlineView.class);
}

/*
 * cell_text — the text of a table cell, by column key.
 *
 * Inputs:
 *   table — the table
 *   key   — the column's identifier
 *   row   — the row
 *
 * Output: the text, or nil.
 */
static NSString *
cell_text(NSTableView *table, NSString *key, NSInteger row)
{
    NSTableCellView *cell = [table viewAtColumn:[table columnWithIdentifier:key] row:row makeIfNecessary:YES];
    return cell.textField.stringValue;
}

/*
 * column_texts — one column's text, top to bottom.
 *
 * Inputs:
 *   table — the table
 *   key   — the column's identifier
 *
 * Output: the texts.
 */
static NSArray<NSString *> *
column_texts(NSTableView *table, NSString *key)
{
    NSMutableArray<NSString *> *texts = [NSMutableArray array];
    for (NSInteger r = 0; r < table.numberOfRows; r++) [texts addObject:cell_text(table, key, r) ?: @""];
    return texts;
}

/*
 * row_with_text — the first row whose cell in a column says this.
 *
 * Inputs:
 *   table — the table
 *   key   — the column's identifier
 *   text  — the text
 *
 * Output: the row, or -1.
 */
static NSInteger
row_with_text(NSTableView *table, NSString *key, NSString *text)
{
    NSUInteger i = [column_texts(table, key) indexOfObject:text];
    return i == NSNotFound ? -1 : (NSInteger)i;
}

/*
 * visible_columns — a table's shown column keys, in display order.
 *
 * Inputs:
 *   table — the table
 *
 * Output: the keys.
 */
static NSArray<NSString *> *
visible_columns(NSTableView *table)
{
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    for (NSTableColumn *c in table.tableColumns) if (!c.hidden) [keys addObject:c.identifier];
    return keys;
}

/*
 * validated_titles — a menu's item titles after each command item has
 * been validated (as AppKit does when the menu opens), with whether each
 * is enabled: "Title" or "(Title)" when disabled.
 *
 * Inputs:
 *   lib  — the library
 *   menu — the menu
 *
 * Output: the titles.
 */
static NSArray<NSString *> *
validated_titles(NotesLibrary *lib, NSMenu *menu)
{
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    for (NSMenuItem *item in menu.itemArray) {
        if (item.separatorItem) continue;
        BOOL on = item.target ? YES : on_library_validate_command(lib, item);
        [titles addObject:on ? item.title : [NSString stringWithFormat:@"(%@)", item.title]];
    }
    return titles;
}

/*
 * open_menu — what a menu shows once its delegate has filled it.
 *
 * Inputs:
 *   menu — the menu
 *
 * Output: the menu.
 */
static NSMenu *
open_menu(NSMenu *menu)
{
    [menu.delegate menuNeedsUpdate:menu];
    return menu;
}

/*
 * double_click — what a double-click on a table cell does: the table's
 * double action, with the clicked row and column.  (Synthetic mouse events
 * never reach a table here: the test's windows cannot become key without
 * activating the app on the owner's screen.)
 *
 * Inputs:
 *   table — the Action Items table
 *   row   — the row
 *   key   — the column's identifier
 */
static void
double_click(NSTableView *table, NSInteger row, NSString *key)
{
    SEL activate = NSSelectorFromString(@"activate_actions:column:");
    if (table.doubleAction == NULL || ![table.target respondsToSelector:activate]) return;
    ((void (*)(id, SEL, NSIndexSet *, NSString *))objc_msgSend)(
        table.target, activate, [NSIndexSet indexSetWithIndex:(NSUInteger)row], key);
}

/*
 * action_due — an action item's due date in the database.
 *
 * Inputs:
 *   text — the item's text
 *
 * Output: the due time, 0 for none, -1 when there is no such item.
 */
static gint64
action_due(const char *text)
{
    gint64 due   = -1;
    GList *items = on_db_action_list(app.db);
    for (GList *l = items; l != NULL; l = l->next) {
        const OnActionItem *it = l->data;
        if (g_strcmp0(it->text, text) == 0) due = it->due;
    }
    on_db_action_list_free(items);
    return due;
}

/*
 * test_notes_pane — the notes list, grid and Action Items, as the GTK app
 * has them: columns, sorting, the one selection, cards, menus, drags, the
 * one-row update on save, and Action Items' done and due dates.
 *
 * Inputs:
 *   lib — the library (its window on screen)
 */
static void
test_notes_pane(NotesLibrary *lib)
{
    gint64 folder  = on_db_folder_create(app.db, 0, "Pane", 0, NULL);
    gint64 bravo   = new_note_in(folder, "Bravo\nbravo body");
    gint64 alpha   = new_note_in(folder, "alpha\n\n   the preview line\nmore");
    gint64 charlie = new_note_in(folder, "Charlie\ncharlie body");
    add_image(charlie);
    lib.grid_pref         = NO;
    app.comfortable_list  = FALSE;
    on_library_refresh_all(lib);
    on_library_sidebar_select(lib, on_sb_folder, folder);
    pump(0.2);

    /* Columns. */
    NSTableView *table = pane_table(lib);
    check([visible_columns(table) isEqualToArray:@[@"title", @"modified"]],
          "the list shows Title and Modified by default (Path and Created hidden)");
    check(table.doubleAction != NULL && table.target != nil && table.allowsColumnReordering,
          "the list opens notes on a double-click and its headers reorder");
    NSMenu *header = open_menu(table.headerView.menu);
    check([menu_titles(header) isEqualToArray:@[@"Title", @"Path", @"Modified", @"Created"]],
          "the header menu lists every column");
    [NSApp sendAction:[header itemWithTitle:@"Path"].action to:[header itemWithTitle:@"Path"].target
                 from:[header itemWithTitle:@"Path"]];
    gchar *layout = on_app_config_get("list_columns");
    check([visible_columns(table) containsObject:@"path"] && layout && strstr(layout, "path:1") != NULL,
          "the header menu shows a column and the layout is saved");
    g_free(layout);
    check([cell_text(table, @"path", 0) isEqualToString:@"/Pane"], "the Path column shows the note's folder");
    header = open_menu(table.headerView.menu);
    [NSApp sendAction:@selector(toggle_column:) to:[header itemWithTitle:@"Path"].target
                 from:[header itemWithTitle:@"Path"]];
    header = open_menu(table.headerView.menu);
    [NSApp sendAction:@selector(toggle_column:) to:[header itemWithTitle:@"Modified"].target
                 from:[header itemWithTitle:@"Modified"]];
    header = open_menu(table.headerView.menu);
    check(![header itemWithTitle:@"Title"].enabled && [visible_columns(table) isEqualToArray:@[@"title"]],
          "the last visible column cannot be hidden");
    [NSApp sendAction:@selector(toggle_column:) to:[header itemWithTitle:@"Modified"].target
                 from:[header itemWithTitle:@"Modified"]];
    NSRect modified_rect = [table rectOfColumn:[table columnWithIdentifier:@"modified"]];
    check(NSContainsRect(table.enclosingScrollView.documentVisibleRect, modified_rect),
          "a column shown again from the header menu is in view, not pushed off the edge");

    /* Sorting, and the one-row update on save. */
    table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"title" ascending:YES]];
    check([column_texts(table, @"title") isEqualToArray:@[@"alpha", @"Bravo", @"Charlie"]],
          "a Title header click sorts by title, ignoring case");
    table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"title" ascending:NO]];
    check([column_texts(table, @"title") isEqualToArray:@[@"Charlie", @"Bravo", @"alpha"]],
          "a second click reverses it");
    table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"modified" ascending:YES]];
    pump(1.1);
    OnDocument *doc = on_document_from_text("Bravo renamed\nbravo body");
    on_note_document_save(app.db, bravo, doc, TRUE, NULL);
    on_document_free(doc);
    on_library_notes_note_saved(lib, bravo);
    check([cell_text(table, @"title", 0) isEqualToString:@"Bravo renamed"],
          "a saved note's row takes its new title and moves to the top (Modified, newest first)");

    /* The one selection: kept across refreshes and views, dropped on navigation. */
    NSInteger alpha_row = row_with_text(table, @"title", @"alpha");
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)alpha_row] byExtendingSelection:NO];
    check(label_with_text(lib.window.contentView, @"1 file selected") != nil,
          "the status bar counts the selected notes");
    on_library_refresh_all(lib);
    pump(0.1);
    NSArray<NSNumber *> *ids = on_library_notes_selected_ids(lib);
    check(ids.count == 1 && ids[0].longLongValue == alpha &&
          [cell_text(table, @"title", table.selectedRow) isEqualToString:@"alpha"],
          "a refresh keeps the selected note selected");
    [lib.window makeFirstResponder:table];
    [table keyDown:return_key(lib.window)];
    pump(0.3);
    check(editor_window_titled(@"alpha") != nil, "Return opens the selected note");
    on_editor_close_all();
    pump(0.2);

    lib.grid_pref = YES;
    on_library_notes_refresh(lib);
    pump(0.6);
    NotesActivatingGrid *grid = (NotesActivatingGrid *)find_view(lib.window.contentView,
                                                                 NSCollectionView.class, Nil);
    check(grid != nil && grid.selectionIndexPaths.count == 1 &&
          on_library_notes_selected_ids(lib).firstObject.longLongValue == alpha,
          "the grid shows the same selection");
    BOOL      all_cards   = grid.numberOfSections > 0 && [grid numberOfItemsInSection:0] == 3;
    NSInteger charlie_card = -1;
    NSMutableSet<NSString *> *card_titles = [NSMutableSet set];
    for (NSInteger i = 0; all_cards && i < 3; i++) {
        NSCollectionViewItem *item  = [grid itemAtIndexPath:[NSIndexPath indexPathForItem:i inSection:0]];
        NSImageView          *thumb = (NSImageView *)find_view(item.view, NSImageView.class, Nil);
        NSTextField          *title = (NSTextField *)find_view(item.view, NSTextField.class, Nil);
        all_cards = thumb.image != nil && thumb.image.size.width == on_look_thumb_size;
        if (title) [card_titles addObject:title.stringValue];
        if ([title.stringValue isEqualToString:@"Charlie"]) charlie_card = i;
    }
    all_cards = all_cards && [card_titles isEqualToSet:[NSSet setWithArray:@[@"Bravo renamed", @"Charlie", @"alpha"]]];
    check(all_cards, "every grid card is rendered, with the note's title under it");
    check(save_snapshots(lib.window, @"library-grid"), "grid snapshots written");
    BOOL follows = YES;
    for (NSInteger dark = 0; dark <= 1; dark++) {
        lib.window.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        pump(0.3);
        NSCollectionViewItem *item  = [grid itemAtIndexPath:[NSIndexPath indexPathForItem:0 inSection:0]];
        NSImageView          *thumb = (NSImageView *)find_view(item.view, NSImageView.class, Nil);
        NSBitmapImageRep     *rep   = (NSBitmapImageRep *)thumb.image.representations.firstObject;
        NSColor *corner = [[rep colorAtX:rep.pixelsWide / 2 y:rep.pixelsHigh - 8]
                           colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        follows = follows && corner != nil && (dark ? corner.redComponent < 0.5 : corner.redComponent > 0.5);
    }
    lib.window.appearance = nil;
    check(follows, "grid cards are re-rendered when the appearance changes");
    NSMenu *card_menu = grid.menu_handler([NSIndexPath indexPathForItem:charlie_card inSection:0]);
    check([validated_titles(lib, card_menu) isEqualToArray:@[@"Open", @"Pin", @"Export as HTML…",
                                                              @"Export as Markdown…", @"Move to Trash"]] &&
          on_library_notes_selected_ids(lib).firstObject.longLongValue == charlie,
          "a right click on a card selects it and offers Open, Pin, the exports and Move to Trash");
    on_library_run_command(lib, command_item(@"note-pin"));
    OnNoteMeta *m = on_db_note_get(app.db, charlie);
    NSMenuItem *pin = command_item(@"note-pin");
    on_library_validate_command(lib, pin);
    check(m && m->pinned && [pin.title isEqualToString:@"Unpin"], "Pin pins it, and the item then says Unpin");
    on_db_note_meta_free(m);
    on_library_run_command(lib, command_item(@"note-pin"));

    lib.grid_pref = NO;
    on_library_notes_refresh(lib);
    pump(0.1);
    table = pane_table(lib);
    check(table.selectedRowIndexes.count == 1 &&
          [cell_text(table, @"title", table.selectedRow) isEqualToString:@"Charlie"],
          "and back in the list the card's selection is the row's");

    /* The list's menu and drag. */
    NSMenu *row_menu = right_click(table, row_with_text(table, @"title", @"alpha"));
    check(row_menu != nil && [validated_titles(lib, open_menu(row_menu)) containsObject:@"Move to Trash"] &&
          on_library_notes_selected_ids(lib).firstObject.longLongValue == alpha,
          "a right click on a row selects it and shows the note menu");
    NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
    [pb clearContents];
    [pb writeObjects:@[[table.dataSource tableView:table pasteboardWriterForRow:0],
                       [table.dataSource tableView:table pasteboardWriterForRow:1]]];
    on_drag_kind_t kind;
    NSArray<NSNumber *> *dragged = on_library_drag_read(pb, &kind);
    check(kind == on_drag_notes && dragged.count == 2, "dragging two rows drags both notes");
    [pb releaseGlobally];

    /* Export: the selected note, then every note, into a folder (the
     * folder picker before it is the system's and is not driven here). */
    NSString *out = [@(check_data_dir()) stringByAppendingPathComponent:@"export-md"];
    [NSFileManager.defaultManager removeItemAtPath:out error:nil];
    [NSFileManager.defaultManager createDirectoryAtPath:out withIntermediateDirectories:YES attributes:nil error:nil];
    NSArray<NSNumber *> *chosen = on_library_notes_selected_ids(lib);
    gint written = on_library_export(lib, ON_EXPORT_MARKDOWN, chosen, out);
    NSArray<NSString *> *files = [NSFileManager.defaultManager subpathsOfDirectoryAtPath:out error:nil];
    BOOL alpha_md = NO;
    for (NSString *f in files) alpha_md |= [f.lastPathComponent hasPrefix:@"alpha"] && [f hasSuffix:@".md"];
    check(chosen.count == 1 && written == 1 && alpha_md && answer_panel(@"OK"),
          "Export as Markdown writes the selected note and says so in a tool panel");
    NSString *all = [@(check_data_dir()) stringByAppendingPathComponent:@"export-html"];
    [NSFileManager.defaultManager removeItemAtPath:all error:nil];
    [NSFileManager.defaultManager createDirectoryAtPath:all withIntermediateDirectories:YES attributes:nil error:nil];
    written = on_library_export(lib, ON_EXPORT_HTML, nil, all);
    NSUInteger html = 0;
    for (NSString *f in [NSFileManager.defaultManager subpathsOfDirectoryAtPath:all error:nil])
        html += [f hasSuffix:@".html"];
    check(written > 1 && html >= (NSUInteger)written && answer_panel(@"OK"),
          "Export All as HTML writes every note");

    /* Navigation drops the selection. */
    on_library_sidebar_select(lib, on_sb_all, 0);
    pump(0.1);
    check(on_library_notes_selected_ids(lib).count == 0, "selecting another sidebar row starts with nothing selected");

    /* Comfortable density. */
    app.comfortable_list = TRUE;
    on_library_sidebar_select(lib, on_sb_folder, folder);
    pump(0.1);
    table = pane_table(lib);
    NSInteger arow = row_with_text(table, @"title", @"alpha");
    check(table.rowHeight == on_look_row_comfortable_h &&
          label_with_text([table viewAtColumn:[table columnWithIdentifier:@"title"] row:arow makeIfNecessary:YES],
                          @"the preview line") != nil,
          "Comfortable rows show the first line after the title, dimmed under it");
    app.comfortable_list = FALSE;

    /* Trash: the menu, and Restore. */
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)arow] byExtendingSelection:NO];
    on_library_run_command(lib, command_item(@"delete-note"));
    on_library_sidebar_select(lib, on_sb_trash, 0);
    pump(0.1);
    table = pane_table(lib);
    NSMenu *trash_menu = right_click(table, row_with_text(table, @"title", @"alpha"));
    check([validated_titles(lib, open_menu(trash_menu)) isEqualToArray:@[@"Open", @"Restore", @"Delete Permanently"]],
          "in the Trash the note menu offers Open, Restore and Delete Permanently");
    on_library_run_command(lib, command_item(@"note-restore"));
    pump(0.1);
    check(!id_in(on_db_trash_note_ids(app.db), alpha) &&
          label_with_text(lib.window.contentView, @"Restored 1 note") != nil,
          "Restore takes the note out of the Trash and says so");

    /* Action Items. */
    new_note_in(folder, "Tasks\n! Zeta task\n! Alpha task");
    on_library_refresh_all(lib);
    on_library_sidebar_select(lib, on_sb_actions, 0);
    pump(0.2);
    table = pane_table(lib);
    check([visible_columns(table) isEqualToArray:@[@"done", @"context", @"action", @"due"]],
          "Action Items shows Done, Context, Action and Due Date");
    check(on_library_notes_selected_ids(lib).count == 0, "and has no note selection");
    table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"action" ascending:YES]];
    NSInteger a_row = row_with_text(table, @"action", @"Alpha task");
    NSInteger z_row = row_with_text(table, @"action", @"Zeta task");
    check(a_row >= 0 && z_row > a_row && [cell_text(table, @"context", a_row) isEqualToString:@"Pane"],
          "Action items sort by their text and show their folder as the context");

    NSButton *box = [table viewAtColumn:[table columnWithIdentifier:@"done"] row:a_row makeIfNecessary:YES];
    [box performClick:nil];
    pump(0.1);
    a_row = row_with_text(table, @"action", @"Alpha task");
    NSTableCellView *done_cell = [table viewAtColumn:[table columnWithIdentifier:@"action"] row:a_row
                                     makeIfNecessary:YES];
    check(app.show_done_actions && a_row >= 0 &&
          [done_cell.textField.attributedStringValue attribute:NSStrikethroughStyleAttributeName atIndex:0
                                                effectiveRange:NULL] != nil,
          "ticking an item strikes it through while completed items are shown");
    on_library_run_command(lib, command_item(@"toggle-done"));
    pump(0.1);
    table = pane_table(lib);
    NSMenuItem *done_item = command_item(@"toggle-done");
    on_library_validate_command(lib, done_item);
    check(row_with_text(table, @"action", @"Alpha task") < 0 && [done_item.title isEqualToString:@"Show Completed"],
          "Hide Completed hides it, and the item then says Show Completed");
    on_library_run_command(lib, command_item(@"toggle-done"));
    pump(0.1);
    table = pane_table(lib);

    z_row = row_with_text(table, @"action", @"Zeta task");
    double_click(table, z_row, @"due");
    pump(0.2);
    check(answer_panel(@"Set"), "a double-click on a due date asks for it in a tool panel");
    pump(0.1);
    gint64 midnight = (gint64)[NSCalendar.currentCalendar startOfDayForDate:NSDate.date].timeIntervalSince1970;
    z_row = row_with_text(table, @"action", @"Zeta task");
    NSTableCellView *due_cell = [table viewAtColumn:[table columnWithIdentifier:@"due"] row:z_row makeIfNecessary:YES];
    NSAttributedString *due_text = due_cell.textField.attributedStringValue;
    NSColor *tint = due_text.length ? [due_text attribute:NSForegroundColorAttributeName atIndex:0
                                           effectiveRange:NULL] : nil;
    __block NSColor *shown = nil;
    [[NSAppearance appearanceNamed:NSAppearanceNameAqua] performAsCurrentDrawingAppearance:^{
        shown = [tint colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    }];
    check(action_due("Zeta task") == midnight && shown != nil &&
          fabs(shown.redComponent - on_look_due_today.light.r) < 0.01 &&
          fabs(shown.greenComponent - on_look_due_today.light.g) < 0.01 &&
          fabs(shown.blueComponent - on_look_due_today.light.b) < 0.01,
          "Set gives it today's date, shown in the due-today colour");

    double_click(table, z_row, @"action");
    pump(0.3);
    check(editor_window_titled(@"Tasks") != nil, "a double-click elsewhere opens the item's note");
    on_editor_close_all();
    pump(0.2);
    on_library_sidebar_select(lib, on_sb_root, 0);
}

/*
 * test_shell — the library window's toolbar says what each button does,
 * and a command that acts on the library opens it when it is closed.
 *
 * Inputs:
 *   lib — the library (its window on screen)
 */
static void
test_shell(NotesLibrary *lib)
{
    NSToolbarItem *media = nil, *before_ai = nil, *previous = nil;
    for (NSToolbarItem *item in lib.window.toolbar.items) {
        if ([item.itemIdentifier isEqualToString:@"media"]) media = item;
        if ([item.itemIdentifier isEqualToString:@"ai-summary"]) before_ai = previous;
        previous = item;
    }
    NSTextField *path_label = nil;
    for (NSView *v in lib.window.contentView.subviews)
        if ([v isKindOfClass:NSTextField.class] && [((NSTextField *)v).stringValue hasPrefix:@"/"]) path_label = (NSTextField *)v;
    check(path_label != nil && status_text_centred(path_label),
          "the library's status text is 9 pt and centred in its bar");
    check([media.toolTip isEqualToString:@"Show every image in the listed notes as thumbnails"] &&
          [before_ai.itemIdentifier isEqualToString:NSToolbarSpaceItemIdentifier],
          "toolbar buttons have GTK's tooltips, and AI Summary sits apart");

    gboolean shown = app.show_done_actions;
    [lib.window close];
    pump(0.3);
    check(!lib.window.visible, "the library window can be closed");
    on_library_run_command(lib, command_item(@"toggle-done"));
    pump(0.3);
    check(lib.window.visible && app.show_done_actions == !shown,
          "a library command from the menu bar opens the closed library window, then runs");
    on_library_run_command(lib, command_item(@"toggle-done"));
}

/*
 * test_secondary_windows — the four WPH windows open, display correctly,
 * and release their resources on close.
 *
 * Inputs:
 *   lib — the library (must have a window on screen)
 */
static void
test_secondary_windows(NotesLibrary *lib)
{
    /* Settings window ─────────────────────────────────────────────────── */
    __weak NSWindow *weak_settings = nil;
    @autoreleasepool {
        /* Open via the registered command — its identifier is "preferences". */
        NSMenuItem *pref_item = [[NSMenuItem alloc] initWithTitle:@"Preferences…"
                                                           action:NULL
                                                    keyEquivalent:@""];
        pref_item.identifier = @"preferences";
        on_library_run_command(lib, pref_item);
        pump(0.3);

        /* Find it among visible windows. */
        NSWindow *settings_win = nil;
        for (NSWindow *w in NSApp.windows) {
            if (w.visible && [w.title isEqualToString:@"Settings"]) {
                settings_win = w;
                break;
            }
        }
        check(settings_win != nil, "the settings window opens on 'preferences' command");
        check([settings_win isKindOfClass:NSPanel.class] && (settings_win.styleMask & NSWindowStyleMaskUtilityWindow),
              "Settings is a tool window");
        NSView *root = settings_win.contentView;
        check(label_with_text(root, @"Appearance") && label_with_text(root, @"Editor") &&
              label_with_text(root, @"Database") && label_with_text(root, @"AI Features"),
              "it has the GTK sections: Appearance, Editor, Database, AI Features");
        check(save_snapshots(settings_win, @"settings"), "settings snapshots written");
        NSBox  *plate = (NSBox *)find_view(root, NSBox.class, Nil);
        NSView *path  = label_with_text(root, @(app.db->path));
        NSRect  path_in_plate = [path convertRect:path.bounds toView:plate];
        check(plate != nil && path != nil && NSContainsRect(plate.bounds, path_in_plate),
              "the database plate holds its lines");
        NSScrollView *settings_scroll = (NSScrollView *)root;
        check(settings_scroll.documentView.flipped && NSMinY(settings_scroll.documentVisibleRect) < 1 &&
              label_with_text(root, @"Appearance") != nil &&
              NSIntersectsRect(settings_scroll.documentVisibleRect,
                               [label_with_text(root, @"Appearance") convertRect:label_with_text(root, @"Appearance").bounds
                                                                         toView:settings_scroll.documentView]),
              "Settings opens scrolled to the top");

        NSButton *bold = button_titled(root, @"Bold titles in comfortable list density");
        check(app.bold_list_titles && bold.state == NSControlStateValueOn,
              "a checkbox whose default is on starts ticked");
        NSButton *counts = button_titled(root, @"Show note counts next to folders and tags");
        gboolean counts_was = app.sidebar_counts;
        [counts performClick:nil];
        check(app.sidebar_counts == !counts_was && on_app_config_get_bool("sidebar_counts", counts_was) == !counts_was,
              "ticking a checkbox writes its setting and OnApp follows");
        [counts performClick:nil];

        NSPopUpButton *density = (NSPopUpButton *)view_with_identifier(root, @"density");
        gboolean density_was = app.comfortable_list;
        [density selectItemAtIndex:density_was ? 0 : 1];
        [density sendAction:density.action to:density.target];
        check(app.comfortable_list == !density_was, "List density switches Compact and Comfortable");
        [density selectItemAtIndex:density_was ? 1 : 0];
        [density sendAction:density.action to:density.target];

        check(app.notify_db_health != NULL, "while Settings is open the health plate follows checks");
        NSTextField *health = (NSTextField *)view_with_identifier(root, @"health");
        [button_titled(root, @"Update") performClick:nil];
        check([health.stringValue hasPrefix:@"Healthy — checked"] &&
              label_with_text(lib.window.contentView, @"Database check passed") != nil,
              "Update runs the health check and says so");
        gint n_notes = 0, n_folders = 0;
        on_db_totals(app.db, &n_notes, &n_folders, NULL);
        NSTextField *data = (NSTextField *)view_with_identifier(root, @"data");
        check([data.stringValue isEqualToString:[NSString stringWithFormat:@"%d notes in %d folders",
                                                 n_notes, n_folders]],
              "the plate counts the notes and folders");
        NSTextField *backup_path = (NSTextField *)view_with_identifier(root, @"backup-path");
        check([backup_path.stringValue hasPrefix:@"Backing up to "] &&
              button_titled(root, @"Back Up Now").enabled == on_app_config_get_bool("backup_enabled", FALSE),
              "the backup line names the folder, and its buttons follow the switch");

        NSButton    *ai     = button_titled(root, @"Enable AI features");
        NSTextField *ai_cmd = (NSTextField *)view_with_identifier(root, @"ai-command");
        gboolean     ai_was = app.ai_enabled;
        check(ai_cmd.enabled == ai_was, "the AI command is usable only while AI is on");
        [ai performClick:nil];
        check(app.ai_enabled == !ai_was && ai_cmd.enabled == !ai_was, "Enable AI features turns AI on");
        ai_cmd.stringValue = @"/bin/cat";
        [(id<NSTextFieldDelegate>)ai_cmd.delegate controlTextDidChange:
            [NSNotification notificationWithName:NSControlTextDidChangeNotification object:ai_cmd]];
        NSTextView *prompt = (NSTextView *)view_with_identifier(root, @"ai-prompt");
        prompt.string = @"Summarise briefly.";
        [prompt.delegate textDidChange:[NSNotification notificationWithName:NSTextDidChangeNotification object:prompt]];
        check(g_strcmp0(app.ai_command, "/bin/cat") == 0 && g_strcmp0(app.ai_custom_prompt, "Summarise briefly.") == 0,
              "the AI command and custom prompt are written as they are typed");
        ai_cmd.stringValue = @"";
        [(id<NSTextFieldDelegate>)ai_cmd.delegate controlTextDidChange:
            [NSNotification notificationWithName:NSControlTextDidChangeNotification object:ai_cmd]];
        prompt.string = @"";
        [prompt.delegate textDidChange:[NSNotification notificationWithName:NSTextDidChangeNotification object:prompt]];
        [ai performClick:nil];
        check(app.ai_command == NULL && app.ai_custom_prompt == NULL && app.ai_enabled == ai_was,
              "and clearing them clears the settings");

        weak_settings = settings_win;
        [settings_win close];
    }
    pump(0.3);
    check(weak_settings == nil && app.notify_db_health == NULL,
          "closed settings window is deallocated and lets go of the health hook");

    /* Search window ───────────────────────────────────────────────────── */
    __weak NSWindow *weak_search = nil;
    @autoreleasepool {
        on_search_window_open(lib, NO, nil);
        pump(0.3);

        NSWindow *search_win = nil;
        for (NSWindow *w in NSApp.windows) {
            if (w.visible && [w.title isEqualToString:@"Search Notes"]) {
                search_win = w;
                break;
            }
        }
        check(search_win != nil, "the search window opens via on_search_window_open");
        check([search_win isKindOfClass:NSPanel.class] &&
              (search_win.styleMask & NSWindowStyleMaskUtilityWindow) && !((NSPanel *)search_win).floatingPanel,
              "it is a tool window with the small title bar, and does not float over editors");
        pump(0.2);
        check(search_win.windowController != nil, "its controller stays alive while it is open");
        NSView      *root   = search_win.contentView;
        NSTextField *query  = (NSTextField *)view_with_identifier(root, @"search-query");
        NSTextField *status = (NSTextField *)view_with_identifier(root, @"search-status");
        NSTableView *hits   = (NSTableView *)find_view(root, NSTableView.class, Nil);
        NSButton    *exact  = button_titled(root, @"Case sensitive");
        NSButton    *regex  = button_titled(root, @"Regular expression");
        BOOL (^search)(NSString *) = ^BOOL(NSString *text) {
            query.stringValue = text;
            [NSApp sendAction:query.action to:query.target from:query];
            for (int i = 0; i < 40 && [status.stringValue isEqualToString:@"Searching…"]; i++) pump(0.05);
            return ![status.stringValue isEqualToString:@"Searching…"];
        };
        check(exact != nil && regex != nil && button_titled(root, @"All Notes").state == NSControlStateValueOn,
              "it offers Case sensitive and Regular expression, and starts on All Notes");
        query.stringValue = @"Smoke";
        pump(0.5);
        check(hits.numberOfRows == 0, "typing alone does not search");
        check(search(@"smoke NOTE") && hits.numberOfRows > 0 && [status.stringValue hasSuffix:@"es"] == (hits.numberOfRows != 1),
              "Return searches every word, ignoring case, and counts the matches");
        check(save_snapshots(search_win, @"search"), "search snapshots written");
        exact.state = NSControlStateValueOn;
        check(search(@"smoke NOTE") && hits.numberOfRows == 0 && [status.stringValue isEqualToString:@"0 matches"],
              "Case sensitive finds nothing for the wrong case");
        exact.state = NSControlStateValueOff;
        regex.state = NSControlStateValueOn;
        check(search(@"Sm.ke n") && hits.numberOfRows > 0, "a regular expression matches");
        check(search(@"(") && [status.stringValue hasPrefix:@"Bad pattern:"], "a bad pattern is reported");
        regex.state = NSControlStateValueOff;
        check(search(@"") && [status.stringValue isEqualToString:@"Type something to search for."],
              "an empty query asks for one");
        search(@"smoke -nothingmatchesthis");
        NSTableCellView *first = hits.numberOfRows > 0 ? [hits viewAtColumn:0 row:0 makeIfNecessary:YES] : nil;
        NSString        *title = first.textField.stringValue.lastPathComponent;
        [first layoutSubtreeIfNeeded];
        check(first != nil && fabs(NSMidY(first.textField.frame) - NSMidY(first.bounds)) < 1.0,
              "a result's text is centred vertically in its row");
        check(hits.rowHeight == on_look_row_h && hits.intercellSpacing.height ==
              ((NSTableView *)find_view(lib.window.contentView, NSTableView.class, NSOutlineView.class)).intercellSpacing.height,
              "result rows are as tall as the library list's compact rows");
        [hits selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [search_win makeFirstResponder:query];
        [search_win makeFirstResponder:hits];
        check(hits.selectedRowIndexes.count == 1, "leaving the query for the results keeps the selected result");
        [hits keyDown:return_key(search_win)];
        pump(0.3);
        check(title.length > 0 && editor_window_titled(title) != nil,
              "Return opens the selected result in an editor");
        on_editor_close_all();
        pump(0.2);
        [search_win setContentSize:NSMakeSize(612, 333)];
        weak_search = search_win;
        [search_win close];
    }
    pump(0.3);
    gint saved_w = 0, saved_h = 0;
    on_app_config_get_size("search_win_w", "search_win_h", &saved_w, &saved_h);
    check(weak_search == nil && saved_w == 612 && saved_h == 333,
          "closed search window is deallocated and its size kept for the next one");

    /* Search Here and Find start on the selection when it is a folder. */
    @autoreleasepool {
        gint64 pane = 0;
        GList *folders = on_db_folder_list(app.db, 0);
        for (GList *l = folders; l != NULL; l = l->next)
            if (g_strcmp0(((OnFolder *)l->data)->name, "Pane") == 0) pane = ((OnFolder *)l->data)->id;
        on_db_folder_list_free(folders);
        on_library_sidebar_select(lib, on_sb_folder, pane);
        on_library_run_command(lib, command_item(@"find"));
        pump(0.3);
        NSWindow *scoped_win = nil;
        for (NSWindow *w in NSApp.windows)
            if (w.visible && [w.title isEqualToString:@"Search Notes"]) scoped_win = w;
        NSView      *root   = scoped_win.contentView;
        NSTextField *query  = (NSTextField *)view_with_identifier(root, @"search-query");
        NSTextField *status = (NSTextField *)view_with_identifier(root, @"search-status");
        query.stringValue = @"body";
        [NSApp sendAction:query.action to:query.target from:query];
        for (int i = 0; i < 40 && [status.stringValue isEqualToString:@"Searching…"]; i++) pump(0.05);
        check(pane != 0 && button_titled(root, @"Selected Folder/Tag").state == NSControlStateValueOn &&
              [status.stringValue hasSuffix:@" in “Pane”"],
              "Find on a selected folder searches that folder, and says so");
        [scoped_win close];
        on_library_sidebar_select(lib, on_sb_root, 0);
    }
    pump(0.3);

    /* With every autorelease pool drained, an open search window still
     * answers: something owns its controller besides the pool. */
    {
        __weak id weak_controller = nil;
        __weak NSWindow *weak_win = nil;
        @autoreleasepool {
            on_search_window_open(lib, NO, nil);
            for (NSWindow *w in NSApp.windows) {
                if (w.visible && [w.title isEqualToString:@"Search Notes"]) weak_win = w;
            }
            weak_controller = weak_win.windowController;
        }
        pump(0.3);
        check(weak_controller != nil, "an open search window's controller outlives the autorelease pool");
        [weak_win close];
        pump(0.2);
    }

    /* A second search window can open concurrently. */
    @autoreleasepool {
        on_search_window_open(lib, YES, @"hello");
        on_search_window_open(lib, NO, nil);
        pump(0.3);
        NSInteger search_count = 0;
        for (NSWindow *w in NSApp.windows) {
            if (w.visible && [w.title isEqualToString:@"Search Notes"]) search_count++;
        }
        check(search_count == 2, "two concurrent search windows are independent");
        for (NSWindow *w in NSApp.windows) {
            if ([w.title isEqualToString:@"Search Notes"]) [w close];
        }
    }
    pump(0.3);

    /* Media window ────────────────────────────────────────────────────── */
    gint64 media_note = new_note("Media note");
    add_image(media_note);
    on_library_sidebar_select(lib, on_sb_all, 0);     /* the media scan covers the selection */
    __weak NSWindow *weak_media = nil;
    @autoreleasepool {
        NSMenuItem *media_item = [[NSMenuItem alloc] initWithTitle:@"Media"
                                                            action:NULL
                                                     keyEquivalent:@""];
        media_item.identifier = @"media";
        on_library_run_command(lib, media_item);
        pump(0.3);

        NSWindow *media_win = nil;
        for (NSWindow *w in NSApp.windows) {
            if (w.visible && [w.title isEqualToString:@"Media in All Notes"]) {
                media_win = w;
                break;
            }
        }
        check(media_win != nil, "the media window opens on 'media' command, titled with what it browses");
        NSTextField *media_status = (NSTextField *)view_with_identifier(media_win.contentView, @"media-status");
        for (int i = 0; i < 60 && [media_status.stringValue hasPrefix:@"Scanning"]; i++) pump(0.05);
        check(media_win.windowController != nil, "its controller stays alive while it is open");

        NSCollectionView *grid = (NSCollectionView *)find_view(media_win.contentView,
                                                               NSCollectionView.class, Nil);
        NotesImageViewer *viewer = (NotesImageViewer *)find_view(media_win.contentView, NotesImageViewer.class, Nil);
        NSInteger n_images = [grid numberOfItemsInSection:0];
        check(n_images > 0 && [media_status.stringValue hasPrefix:
                                 [NSString stringWithFormat:@"%ld image%s in ", (long)n_images, n_images == 1 ? "" : "s"]],
              "the scan fills the grid and counts the images");
        NSInteger media_cell = -1;
        for (NSInteger i = 0; i < n_images; i++) {
            NSCollectionViewItem *item = [grid itemAtIndexPath:[NSIndexPath indexPathForItem:i inSection:0]];
            if (item && label_with_text(item.view, @"Media note")) media_cell = i;
        }
        NSCollectionViewItem *media_item_view = media_cell >= 0
            ? [grid itemAtIndexPath:[NSIndexPath indexPathForItem:media_cell inSection:0]] : nil;
        check(media_item_view != nil && [media_item_view.view.toolTip hasPrefix:@"Media note\nImage 1 of 1"] &&
              media_item_view.imageView.image.size.width <= on_look_media_thumb,
              "each thumbnail is captioned and labelled with its note, and decoded at thumbnail size");
        check([viewer.caption_handler(media_cell) isEqualToString:@"Media note — image 1 of 1"],
              "the viewer's caption names the note and the image");
        check(save_snapshots(media_win, @"media"), "media snapshots written");
        check(grid.selectable && n_images > 0, "its thumbnails can be selected");
        grid.selectionIndexPaths = [NSSet setWithObject:[NSIndexPath indexPathForItem:0 inSection:0]];
        [grid keyDown:return_key(media_win)];
        pump(0.2);
        NSImageView *big = (NSImageView *)find_view(viewer, NSImageView.class, Nil);
        check(on_image_viewer_is_open((NotesImageViewer *)viewer) &&
              big.frame.size.width > on_look_media_thumb && big.image.size.width > on_look_media_thumb,
              "Return on a thumbnail opens the viewer with the picture bigger than its thumbnail");
        check(save_snapshots(media_win, @"media-viewer"), "media image viewer snapshots written");
        NSEvent *(^key)(unsigned short) = ^NSEvent *(unsigned short code) {
            unichar c = code == 123 ? NSLeftArrowFunctionKey : NSRightArrowFunctionKey;
            NSString *chars = [NSString stringWithCharacters:&c length:1];
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                                windowNumber:media_win.windowNumber context:nil characters:chars
                 charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
        };
        [media_win sendEvent:key(123)];
        check(on_image_viewer_index(viewer) == 0, "Left on the first picture stays there: no wrap-around");
        [media_win sendEvent:key(124)];
        check(on_image_viewer_index(viewer) == 1, "Right steps to the next picture");
        NSEvent *(^press)(NSPoint) = ^NSEvent *(NSPoint p) {
            return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:[viewer convertPoint:p toView:nil]
                                 modifierFlags:0 timestamp:0 windowNumber:media_win.windowNumber context:nil
                                   eventNumber:0 clickCount:1 pressure:1];
        };
        NSView *nav_row = label_with_text(viewer, @"Next").superview;
        NSRect  nav     = [nav_row convertRect:nav_row.bounds toView:viewer];
        [viewer mouseDown:press(NSMakePoint(NSMaxX(nav) - 2, NSMidY(nav)))];
        check(!nav_row.hidden && on_image_viewer_index(viewer) == 2,
              "a click on the right half of Previous | Next steps forward");
        [viewer mouseDown:press(NSMakePoint(5, 5))];
        check(!on_image_viewer_is_open(viewer), "a click on the backdrop closes the viewer");
        on_image_viewer_open(viewer, 0);
        NSEvent *escape = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                      modifierFlags:0 timestamp:0
                                       windowNumber:media_win.windowNumber context:nil
                                         characters:@"\033" charactersIgnoringModifiers:@"\033"
                                          isARepeat:NO keyCode:53];
        [media_win sendEvent:escape];
        check(!on_image_viewer_is_open((NotesImageViewer *)viewer), "and Escape in the window closes it");
        weak_media = media_win;
        [media_win setContentSize:NSMakeSize(640, 480)];
        [media_win close];
    }
    pump(0.3);
    gint media_w = 0, media_h = 0;
    on_app_config_get_size("media_win_w", "media_win_h", &media_w, &media_h);
    check(weak_media == nil && media_w == 640 && media_h == 480,
          "closed media window is deallocated and its size kept");

    /* AI summary ──────────────────────────────────────────────────────── */
    @autoreleasepool {
        NSTextView *ai_text = (NSTextView *)view_with_identifier(lib.window.contentView, @"ai-text");
        NSView     *ai_pane = ai_text.enclosingScrollView.superview;
        check(ai_text != nil && ai_pane.hidden && [ai_pane.superview isKindOfClass:NSSplitView.class],
              "the AI pane sits hidden under the notes, in a split");
        gboolean ai_was = app.ai_enabled;
        app.ai_enabled = FALSE;
        check(!on_library_validate_command(lib, command_item(@"ai-summary")), "AI Summary is off while AI is");
        app.ai_enabled = TRUE;
        g_free(app.ai_command);
        app.ai_command = NULL;
        on_library_run_command(lib, command_item(@"ai-summary"));
        check(!ai_pane.hidden && [ai_text.string hasPrefix:@"No AI command configured."],
              "with no command it says where to set one");

        app.ai_command = g_strdup("/bin/cat");
        gint64 pane_folder = 0;
        GList *folders = on_db_folder_list(app.db, 0);
        for (GList *l = folders; l != NULL; l = l->next)
            if (g_strcmp0(((OnFolder *)l->data)->name, "Pane") == 0) pane_folder = ((OnFolder *)l->data)->id;
        on_db_folder_list_free(folders);
        on_library_sidebar_select(lib, on_sb_folder, pane_folder);
        pump(0.1);
        on_library_run_command(lib, command_item(@"ai-summary"));
        check([ai_text.string isEqualToString:@"Select one or more notes to summarize."],
              "it summarises the selected notes, and asks for some");
        NSTableView *list = (NSTableView *)find_view(lib.window.contentView, NSTableView.class, NSOutlineView.class);
        [list selectRowIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 2)] byExtendingSelection:NO];
        on_library_run_command(lib, command_item(@"ai-summary"));
        on_library_run_command(lib, command_item(@"ai-summary"));    /* ignored while running */
        for (int i = 0; i < 60 && ai_text.string.length == 0; i++) pump(0.05);
        NSString *summary = ai_text.string;
        check([summary hasPrefix:@"Provide me a brief summary of these notes.\n\nRespond in plain text only."] &&
              [summary containsString:@"=== Notes ===\n\n--- "] && [summary containsString:@"(modified "] &&
              label_with_text(lib.window.contentView, @"Summary generation complete") != nil,
              "the command gets the prompt and the notes on stdin, and its output fills the pane");
        NSRange first = [summary rangeOfString:@"=== Notes ==="];
        check(first.location != NSNotFound &&
              [summary rangeOfString:@"=== Notes ===" options:0
                               range:NSMakeRange(NSMaxRange(first), summary.length - NSMaxRange(first))].location == NSNotFound,
              "a second request while one runs is ignored");
        [button_titled(ai_pane, @"Copy") performClick:nil];
        check([[NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString] isEqualToString:summary],
              "Copy puts the summary on the clipboard");
        check(save_snapshots(lib.window, @"library-ai"), "AI pane snapshots written");
        [button_titled(ai_pane, @"✕") performClick:nil];
        check(ai_pane.hidden, "✕ closes the pane");
        g_free(app.ai_command);
        app.ai_command = NULL;
        app.ai_enabled = ai_was;
        on_library_sidebar_select(lib, on_sb_root, 0);
    }
}

/*
 * editor_cycles — open and close the editor of a note, again and again.
 *
 * Inputs:
 *   note_id — the note
 *   n       — how many times
 *   keep    — nil; or an array that is given each editor's controller,
 *             keeping it alive as a leak would (the check's control)
 */
static void
editor_cycles(gint64 note_id, int n, NSMutableArray *keep)
{
    for (int i = 0; i < n; i++) {
        @autoreleasepool {
            on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
            pump(0.05);
            if (keep) [keep addObject:editor_window_titled(@"Heap note").windowController];
            on_editor_close_all();
        }
        pump(0.05);
    }
}

/*
 * test_heap — once the caches are warm, open/close cycles on an
 * image-bearing note must not grow the heap.
 */
static void
test_heap(void)
{
    gint64 note_id = new_note("Heap note\nwith a picture");
    add_image(note_id);
    editor_cycles(note_id, HEAP_CYCLES, nil);      /* warm the caches (see HEAP_CYCLES) */
    size_t before = heap_in_use();
    editor_cycles(note_id, HEAP_CYCLES, nil);
    size_t after  = heap_in_use();
    long   growth = (long)after - (long)before;
    printf("     heap in use: %zu -> %zu bytes over %d cycles (%ld per cycle)\n",
           before, after, HEAP_CYCLES, growth / HEAP_CYCLES);
    check(growth < (long)HEAP_CYCLES * HEAP_SLACK_PER_CYCLE,
          "forty editor open/close cycles do not grow the heap");

    /* The control: the same cycles with every editor kept alive must fail
     * the same check.  A check that cannot fail proves nothing. */
    @autoreleasepool {
        NSMutableArray *leaked = [NSMutableArray array];
        before = heap_in_use();
        editor_cycles(note_id, HEAP_CYCLES, leaked);
        growth = (long)heap_in_use() - (long)before;
        printf("     control (every editor kept): %ld bytes per cycle\n", growth / HEAP_CYCLES);
        check(leaked.count == HEAP_CYCLES && growth >= (long)HEAP_CYCLES * HEAP_SLACK_PER_CYCLE,
              "control: leaking the editors does fail the heap check");
    }
    pump(0.3);
}

/*
 * test_image_rule_control — the control for the image-region check: do the
 * forbidden thing once (an image view with no frame, given the bundled
 * full-size artwork, sized only by constraints) and see that the check
 * notices.  A check that cannot fail proves nothing.
 *
 * Inputs:
 *   window — a window on screen to put the view in
 */
static void
test_image_rule_control(NSWindow *window)
{
    @autoreleasepool {
        NSImage *full = [[NSImage alloc] initWithContentsOfURL:
            [NSBundle.mainBundle URLForResource:@"composition" withExtension:@"png"]];
        NSImageView *bad = [[NSImageView alloc] init];
        bad.image = full;
        bad.translatesAutoresizingMaskIntoConstraints = NO;
        [window.contentView addSubview:bad];
        [NSLayoutConstraint activateConstraints:@[
            [bad.widthAnchor constraintEqualToConstant:24],
            [bad.heightAnchor constraintEqualToConstant:24],
            [bad.topAnchor constraintEqualToAnchor:window.contentView.topAnchor],
            [bad.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor],
        ]];
        [window displayIfNeeded];
        pump(0.5);
        mach_vm_size_t largest = largest_image_region();
        printf("     control (zero-frame image view, full-size image): %llu KB region\n",
               largest / 1024);
        check(largest >= IMAGE_REGION_LIMIT,
              "control: the forbidden pattern does produce a megabyte image region");
        [bad removeFromSuperview];
    }
}

/*
 * main — run the test.
 *
 * Output: 0 when every check held.
 */
int
main(void)
{
    @autoreleasepool {
        check_data_dir();
        on_app_config_init();
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        check(on_app_open_db(&app), "the test database opens");
        on_app_prefs_load(&app);
        new_note("First note\n! Something to do due 2026-12-01");

        NotesLibrary *lib = on_library_create(&app);
        on_editor_install(&app);
        on_library_window_install(lib);
        on_library_commands_install(lib);
        on_library_notes_install(lib);
        on_settings_install(lib);
        on_search_install(lib);
        on_media_install(lib);
        on_library_ai_install(lib);
        check(app.open_note != NULL && app.live_document != NULL && app.live_edited != NULL &&
              app.flush_editors != NULL && app.quicknote != NULL &&
              app.notify_notes_changed != NULL && app.notify_note_saved != NULL &&
              app.notify_status != NULL, "the modules install the hooks the core calls");

        test_library(lib);
        test_editor();
        test_editor_commands();
        test_inline_image();
        test_accessibility();
        test_strike_snapshot();
        test_tag_suggestions();
        test_pointer();
        test_new_folder(lib);
        test_trash(lib);
        test_sidebar(lib);
        test_notes_pane(lib);
        test_shell(lib);
        test_secondary_windows(lib);

        gint64 quick = app.quicknote(&app);
        pump(0.3);
        check(quick != 0 && editor_window_titled(@ON_DEFAULT_NOTE_TITLE) != nil,
              "the quicknote hook creates a note and opens its editor");
        on_editor_close_all();

        test_heap();

        pump(0.3);
        mach_vm_size_t largest = largest_image_region();
        printf("     largest decoded-image region: %llu KB\n", largest / 1024);
        check(largest < IMAGE_REGION_LIMIT,
              "no megabyte image region: icons are decoded at display size into framed views");
        test_image_rule_control(lib.window);

        on_app_shutdown(&app);
        check(app.db == NULL, "shutdown closes the database");
    }
    return check_result();
}
