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

#import "editor_window.h"
#import "library_ai.h"
#import "library_commands.h"
#import "library_notes.h"
#import "library_state.h"
#import "library_window.h"
#import "media_window.h"
#import "note_view.h"
#import "search_window.h"
#import "settings_window.h"
#import "ui_util.h"

#include "action_edit.h"
#include "app_lifecycle.h"
#include "serialize.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>

/* How many open/close cycles the heap check runs after warming up. */
#define HEAP_CYCLES 40

/* A leaked editor on the image note costs hundreds of KB per cycle; this
 * much is noise from AppKit's own caches. */
#define HEAP_SLACK_PER_CYCLE (24 * 1024)

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
 * new_note — a note with the given text, saved through THE save.
 *
 * Inputs:
 *   text — its content, one block per line
 *
 * Output: the note's id.
 */
static gint64
new_note(const char *text)
{
    gint64      note_id = on_db_note_create(app.db, 0);
    OnDocument *doc     = on_document_from_text(text);
    on_note_document_save(app.db, note_id, doc, TRUE, NULL);
    on_document_free(doc);
    return note_id;
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
        weak_view       = ((NSScrollView *)window.contentView).documentView;
        check([weak_view isKindOfClass:NotesNoteView.class], "the note view is its document view");
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
        NSTabView *tv = nil;
        for (NSView *v in settings_win.contentView.subviews) {
            if ([v isKindOfClass:[NSTabView class]]) { tv = (NSTabView *)v; break; }
        }
        check(tv != nil && tv.numberOfTabViewItems == 3,
              "the settings window has three tabs");

        weak_settings = settings_win;
        [settings_win close];
    }
    pump(0.3);
    check(weak_settings == nil, "closed settings window is deallocated (no retain cycle)");

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
        weak_search = search_win;
        [search_win close];
    }
    pump(0.3);
    check(weak_search == nil, "closed search window is deallocated");

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
            if (w.visible && [w.title isEqualToString:@"Media Browser"]) {
                media_win = w;
                break;
            }
        }
        check(media_win != nil, "the media window opens on 'media' command");
        weak_media = media_win;
        [media_win close];
    }
    pump(0.3);
    check(weak_media == nil, "closed media window is deallocated");

    /* AI pane ─────────────────────────────────────────────────────────── */
    @autoreleasepool {
        NSView *pane = on_library_ai_pane_new(lib);
        check(pane != nil, "on_library_ai_pane_new returns a view");
        check(pane.hidden, "the AI pane starts hidden");
    }
}

/*
 * test_heap — open/close cycles on an image-bearing note must not grow the
 * heap.
 */
static void
test_heap(void)
{
    gint64 note_id = new_note("Heap note\nwith a picture");
    add_image(note_id);
    for (int i = 0; i < 5; i++) {    /* warm AppKit's caches */
        @autoreleasepool {
            on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
            pump(0.05);
            on_editor_close_all();
        }
        pump(0.05);
    }
    size_t before = heap_in_use();
    for (int i = 0; i < HEAP_CYCLES; i++) {
        @autoreleasepool {
            on_editor_window_open(&app, note_id, NotesEditorRevealNone, 0, nil);
            pump(0.05);
            on_editor_close_all();
        }
        pump(0.05);
    }
    size_t after  = heap_in_use();
    long   growth = (long)after - (long)before;
    printf("     heap in use: %zu -> %zu bytes over %d cycles (%ld per cycle)\n",
           before, after, HEAP_CYCLES, growth / HEAP_CYCLES);
    check(growth < (long)HEAP_CYCLES * HEAP_SLACK_PER_CYCLE,
          "forty editor open/close cycles do not grow the heap");
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
