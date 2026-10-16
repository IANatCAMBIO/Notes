# Files — AppKit playbook for porting the GTK4 task manager and notes app

Source: `/Users/ian_campbell/salt_development/files` (read-only; nothing in it was modified, built or run).
State read: `main` at `d8b0edd` plus the owner's uncommitted working-tree edits to `README.md`,
`resources/Info.plist`, `src/default_viewer.{h,m}` and `src/prefs_window.m` (all about the
default-file-browser feature; none touch a pattern described here). Line numbers refer to
the working tree as of 2026-10-03. Size: 13,269 lines in `src/` (17 `.m`, 16 `.h`),
276 lines of tests, 36 commits (`code-review-fixes` and `optimizations` are both ancestors
of `main`, so `main` holds all history).

How to read this: sections 1–2 are rules to follow, section 3 is the pattern catalogue
(what / where / how / gotchas / excerpt), section 4 is the copy list, section 5 is the
numbered lessons list, section 6 is the visual spec, section 7 is testing, section 8 is
what not to copy. "Unverified" items are collected at the very end.

Two things to know before anything else, because the owner's brief mentions them:

- **Focus rings.** Files contains **no custom focus-ring drawing at all** (no
  `drawFocusRingMask`, no `focusRingMaskBounds`, no layer borders). Its approach is: use
  stock bezeled `NSTextField`s and let AppKit draw the ring; turn the ring **off**
  (`NSFocusRingTypeNone`) on the two fields that live in the toolbar; and make sure the
  layout around a field is measured by *alignment rect*, not frame, so the focused
  state does not disturb neighbours. The hard-won part is the history: a custom
  box + borderless field + vertically-centring cell was written first and later deleted
  in favour of a stock rounded-bezel field. See 3.1.
- **Animated sliders.** There is no hand-written animation. Every slide is
  `NSSplitViewItem.animator.collapsed = …` on an `NSSplitViewController`. The hard-won
  part is the sequencing (add collapsed → expand on the next run-loop pass; tear down
  first → remove in the completion handler; re-entrancy flag). See 3.2.

---

## 1. Code conventions — Objective-C written "in C"

### 1.1 File layout

- One module = `src/<snake_name>.h` + `src/<snake_name>.m`. Flat `src/`, no subfolders.
  `main.m` has no header. Tests are `tests/<module>_test.m`.
- Header: `#import <Cocoa/Cocoa.h>` (or `<Foundation/Foundation.h>` for pure-C modules),
  C includes with `#include`, then a file-level block comment describing the module,
  then declarations, each preceded by its full comment block. No include guards
  (`#import` is used throughout). Forward-declare with `@class` instead of importing
  (`src/main_window.h:3`).
- Implementation file order (consistent across modules):
  1. `#import` own header first, then project headers alphabetically, then frameworks,
     then C headers (`src/browser_view.m:1-12`).
  2. File-scope constants (`static NSString * const`, `static const CGFloat`, `#define`).
  3. `typedef struct` tables and file-static C helper functions.
  4. Class extension `@interface X () <protocols>` with private properties.
  5. C callbacks that need the extension's properties.
  6. `@implementation`.
- Section dividers inside a file are box-drawing comment rules, never `#pragma mark`
  (grep finds none):

  ```c
  /* ── Inline rename ───────────────────────────────────────────────────── */
  ```

### 1.2 Naming

| Thing | Rule | Example |
|---|---|---|
| Files | snake_case | `browser_view.m`, `copy_progress.h` |
| ObjC classes | **PascalCase with app prefix** — the one documented exception to snake_case | `FilesMainWindow`, `FilesBrowserView`, `FilesCancelToken` |
| Own ObjC methods | snake_case, including initialisers and actions | `init_with_url:`, `navigate_to:`, `toggle_dual_pane:`, `show_rows:selecting:reveal:` |
| AppKit overrides / delegate methods | Apple's names, unchanged | `keyDown:`, `tableView:viewForTableColumn:row:`, `copy:`, `selectAll:` |
| Action methods | `<verb>_action:` (or `<noun>_clicked:` / `<noun>_changed:`) | `go_back_action:`, `refresh_clicked:`, `search_changed:` |
| Properties | snake_case | `address_bar`, `list_generation`, `pending_names` |
| Handler block properties | `<what>_handler`, `copy` | `navigation_handler`, `selection_handler` |
| ivars (when used) | `_snake_case` in an `@implementation { }` block | `_btn_container`, `_engine_lock` |
| Public C functions | `<app>_` prefix + snake_case | `files_format_bytes`, `files_run_tool` |
| File-static C functions | snake_case, no prefix | `listing_rows`, `make_item`, `button_item` |
| File-scope constants | snake_case `static const` | `static const CGFloat status_h = 20.0;` |
| Extern string constants | snake_case with app prefix | `files_pref_show_hidden` |
| Macros | UPPER_SNAKE | `HISTORY_LIMIT`, `FILES_TOOL_TIMEOUT`, `COLUMN_COUNT` |
| Struct typedefs | `snake_case_t` | `column_spec_t`, `copy_ctx_t` |
| Anonymous enums | snake_case members | `enum { row_kind, row_opens_with, row_size, row_count };` |
| Public `NS_ENUM`, block typedefs, notification names | PascalCase with prefix (they are ObjC-facing types) | `FilesConflictResolution`/`FilesConflictKeepBoth`, `FileCopyProgressBlock`, `FilesVolumesChangedNotification` |
| NSUserDefaults key strings | dotted lower-case with app prefix | `@"files.show_hidden_files"` |
| Toolbar/item identifiers | dotted with hyphens | `@"files.dual-pane"` |
| Log tags | `[snake_tag]` | `NSLog(@"[keep_running] …")` |

The first code review flagged the original `kCamelCase` constants and wrote
"ObjC class names are a reasonable exception — decide once and write it down"
(`CODE_REVIEW.md:129`). The decision taken: classes PascalCase+prefix, everything else
snake_case. For the new apps use the app's own prefix (`Tasks…`/`tasks_`, `Notes…`/`notes_`).

### 1.3 Function and method definition style

Return type on its own line, name on the next, opening brace on its own line (K&R for
functions); braces on the same line for statements. This is applied to ObjC methods too —
the `- (type)` goes on its own line:

```objc
/*
 * push_history: — appends url to a back/forward stack, dropping the oldest
 * entry beyond HISTORY_LIMIT.
 *
 * Inputs:
 *   url   — location to record; nil is ignored
 *   stack — back_stack or forward_stack
 */
- (void)
push_history:(NSURL *)url onto:(NSMutableArray<NSURL *> *)stack
{
    if (!url) return;
    [stack addObject:url];
    if (stack.count > HISTORY_LIMIT) [stack removeObjectAtIndex:0];
}
```
(`src/browser_view.m:770-784`)

Long AppKit selectors are split one keyword per line, flush left:

```objc
- (NSView *)
tableView:(NSTableView *)tv
viewForTableColumn:(NSTableColumn *)col
              row:(NSInteger)row
{
```
(`src/browser_view.m:1751-1755`)

Other habits visible everywhere:
- 4-space indent, ~100-column lines.
- One-line guards without braces are normal: `if (!self) return nil;`,
  `if (!s) return;`. Multi-statement bodies always get braces.
- Assignments in a run are column-aligned on `=` (`src/main_window.m:115-119`).
- Dot syntax for properties and for class "properties"
  (`NSUserDefaults.standardUserDefaults`, `NSColor.secondaryLabelColor`,
  `NSFileManager.defaultManager`); bracket syntax for messages.
- Lightweight generics on every collection (`NSArray<NSURL *> *`).
- Literals (`@[]`, `@{}`, `@(x)`), `?:` for nil-coalescing.
- Only `/* */` comments; no `//`.
- Explicit casts between `NSInteger`/`NSUInteger` at table boundaries
  (`(NSUInteger)row >= self.filtered_items.count`).

### 1.4 Function comment blocks

Every function and method has one — including AppKit overrides and one-line getters.
Shape: `name — purpose.` paragraph(s) explaining *why*, then `Inputs:` (aligned
`name — meaning`, noting "(unused)" for required-but-unused parameters), then `Output:`
(return and error conventions). The full block lives where the function is declared
(header for public API); the `.m` repeats a one-line summary and points back:

```c
/*
 * files_format_bytes — Finder-style byte count via one shared formatter.
 * Inputs/output are documented in util.h.
 */
```
(`src/util.m:347-350`)

Comments carry the reasons for non-obvious choices (these are the richest source of
lessons — see section 5). Properties get a comment above or beside them saying what they
hold and who sets them (`src/browser_view.m:253-334`).

`CODE_REVIEW.md:112-127` lists a dozen comments that had drifted from the code and calls
them "worse than missing". Treat a behaviour change as incomplete until the comment
block is updated.

### 1.5 ARC, properties, ivars

- **ARC** (`-fobjc-arc`, `Makefile:44`). No manual retain/release anywhere. Core
  Foundation crossings use `__bridge` / `CFBridgingRelease` (`src/util.m:235`), and CF
  objects created with `Create` are released by hand (`FSEventStreamRelease`,
  `src/browser_view.m:654`).
- **Private state = properties in a class extension**, always with explicit attributes
  (`nonatomic` + `strong`/`copy`/`weak`/`assign`), `copy` for `NSString`/`NSArray`/blocks,
  `weak` for views owned elsewhere (`src/main_window.m:43-91`). Accessed as `self.x`.
- **Small view classes use an ivar block instead** (`FilesBookmarksBar`,
  `FilesInfoSidebar`, `FilesSpeedGraph`, `FilesCopyProgress`): `@implementation X { … }`
  with `_snake` ivars, accessed directly. Use this when the state is plain C data
  (arrays, counters, locks) or never leaves the class (`src/speed_graph.m:9-17`,
  `src/copy_progress.m:34-56`).
- Public headers expose the minimum: handler block properties, the designated
  initialiser, and a handful of methods. Read-only public + read-write private is done
  by redeclaring in the extension (`status_text`, `src/browser_view.h:52` and
  `src/browser_view.m:334`).
- **Designated initialisers are snake_case and the inherited ones are marked
  unavailable**, which removes the need for dead `initWithCoder:` overrides:

  ```objc
  - (instancetype)init_with_frame:(NSRect)frame NS_DESIGNATED_INITIALIZER;
  - (instancetype)initWithFrame:(NSRect)frame NS_UNAVAILABLE;
  - (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;
  ```
  (`src/browser_view.h:63-65`; same in `main_window.h:29-32`, `bookmarks_bar.h:28-30`)
- **Singletons**: `+ (instancetype)shared` with `dispatch_once` (`src/prefs_window.m:75-84`);
  `init` marked `NS_UNAVAILABLE` where direct creation would be wrong
  (`src/volume_manager.h:19`). No `dealloc` on singletons (it never runs).
- No categories, no custom protocols, no `@synthesize`, no bindings, no `#pragma mark`
  (all verified by grep). One KVO observation in the whole app (window `firstResponder`,
  with a context pointer — `src/main_window.m:41, 126-127, 769-788`).

### 1.6 Blocks

- Callbacks between objects are **`copy` block properties named `*_handler`**, set by
  the owner, never delegates/protocols (`src/browser_view.h:15-46`).
- Any block that is stored or runs later captures `self` weakly and re-strongifies with
  a nil check. This is the house idiom — copy it exactly:

  ```objc
  __weak FilesMainWindow *ws = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(transient_status_sec * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
      FilesMainWindow *s = ws;
      if (!s || s.transient_status_id != msg_id) return;  /* superseded */
      s.transient_status_id = 0;
      [s show_pane_status];
  });
  ```
  (`src/main_window.m:588-595`)
- Completion blocks are always documented with the queue they are called on ("called on
  main queue"), and asynchronous APIs hop to main themselves before calling them
  (`src/file_ops.m:853-855`).
- Small local helper blocks are used to cut repetition when building menus and grids
  (`src/browser_view.m:568-574`, `src/info_panel.m:237-242`).

### 1.7 C and ObjC mixed

The split is deliberate:

- **Plain C functions** for anything that needs no object state: formatting, path
  helpers, table/menu/toolbar item factories, engines, parsers. File-static unless
  shared. Factories return configured AppKit objects:

  ```objc
  static NSMenuItem *
  make_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mod)
  ```
  (`src/app_delegate.m:532-540`; also `button_item` `main_window.m:847-859`,
  `make_label`/`make_value`/`make_copy_btn` `info_panel.m:32-89`, `section_label`/
  `note_label` `prefs_window.m:34-59`, `make_col` `mounts_panel.m:75-85`)
- **Pure functions for data preparation**, so they can run on any thread:
  `listing_rows(items, sort_key, ascending, …)` is "Pure and thread-safe, so listings
  are prepared off the main thread" (`src/browser_view.m:174-242`).
- **C structs for static tables** (`__unsafe_unretained` for literal strings in a
  `static const` table):

  ```objc
  typedef struct {
      NSString * __unsafe_unretained id;
      NSString * __unsafe_unretained title;   /* header title without sort arrow */
      CGFloat width;
      CGFloat min_width;
  } column_spec_t;

  static const column_spec_t column_specs[] = {
      { @"name",     @"Name",          260, 120 },
      …
  };
  #define COLUMN_COUNT (sizeof(column_specs) / sizeof(column_specs[0]))
  ```
  (`src/browser_view.m:25-40`) — and designated-initialiser arrays indexed by an enum
  (`src/info_sidebar.m:19-26`).
- **C structs for callback context** passed through `void *` (`copy_ctx_t`,
  `src/file_ops.m:205-217`).
- **ObjC classes** only for things that are views, window controllers, the app
  delegate, a data record shown in a table (`FilesMountRecord`, properties only,
  `mounts_panel.m:20-32`), or a lifetime-managed token (`FilesCancelToken`).
- A class with only class methods is used as a namespace for one engine
  (`FilesFileOps`); newer code prefers plain `files_*` C functions for the same job
  (`default_viewer.h`, `git_info.h`, `util.h`). Prefer the C-function form in new code.
- C11 `stdatomic.h` (`atomic_int`, `atomic_load`, `atomic_store`) for cross-thread
  flags; `os_unfair_lock` for small critical sections (`src/copy_progress.m:52, 341-344`).
  The deprecated `OSAtomic*` + `volatile` was explicitly replaced (`CODE_REVIEW.md:163`).

### 1.8 Error handling, logging, strings

- **Errors**: ObjC-facing async APIs deliver `NSError *` (nil on success) through a
  completion block on the main queue. C functions return small int codes documented in
  the header (`0` ok, `-1` I/O error, `-2` cancelled — `src/file_hash.h:20`) or an
  `errno`. Two tiny wrappers build `NSError`s: `posix_error(code, url)` and
  `described_error(text)` (`src/file_ops.m:95-100, 176-181`). User cancellation is
  `NSCocoaErrorDomain`/`NSUserCancelledError` and is filtered before showing an alert
  (`src/browser_view.m:2276`).
- **Showing errors**: always `files_show_error(title, err, window)` — a sheet on the
  window, app-modal if no window (`src/util.m:443-450`). Title is a plain statement of
  what failed ("Rename failed", "Could not create folder"); the body is
  `err.localizedDescription`. On failure the control that triggered it is reverted
  (`src/info_panel.m:637-644, 699-702`).
- **Never ignore an error result silently** on destructive paths — the first review's
  release blockers B4/B5 were both `error:nil` on a delete/list (`CODE_REVIEW.md:49-50`).
  Read-only resource lookups may pass `error:nil` and treat nil as "unknown" ("—").
- **Logging**: `NSLog` with a `[module]` tag, used sparingly (4 calls in the whole app:
  `app_delegate.m:79, 201, 207`, `default_viewer.m:845`). No `os_log`, no debug
  printf. User-visible logs are written as real files and opened
  (`default_viewer.m` → `~/Library/Logs/Files/…log`).
- **Strings**: `NSString` everywhere in app code. Convert to C only at a C API
  boundary with `.fileSystemRepresentation` (paths) or `.UTF8String`; convert back with
  `@(buf)` / `stringWithUTF8String:`. C buffers are stack arrays (`char buf[PATH_MAX]`)
  with `snprintf`. Long literals are split with adjacent-string concatenation.
  Typography: real "…", "—", "▸", curly quotes are avoided in favour of `\"name\"`.
  No localisation tables — all strings are literal English.
- **Placeholder for "no value" is `@"—"`** everywhere (`files_format_date`, cell text).

### 1.9 Dead code and duplication rules as enforced here

The first review deleted 406 lines of an unused `sidebar.m` that was still being
compiled by the `wildcard`, removed every `@available` fallback below the deployment
target, removed nib initialisers, and collapsed ~20 duplicated snippets into `util.*`
(`CODE_REVIEW.md:61-98`). Practical rules for the ports:

- One source of truth each for: preference keys, formatting, alerts, well-known URLs,
  cell text (shared by cell builder and width measurement), the menu/prefs option list.
- One code path each for: navigation (`load_location:`), showing rows (`show_rows:…`),
  reloading a panel (`reload_table_then:`), copy entry (`transfer_urls:…`).
- When something is replaced, the old path is deleted in the same commit.

---

## 2. App skeleton

### 2.1 `main()` and NSApplication — no nib/xib/storyboard (confirmed)

No `.xib`, `.nib`, `.storyboard`, `.xcodeproj`, `.xcassets`, `.entitlements` or
`.strings` exists in the tree, and `Info.plist` has no `NSMainNibFile` /
`NSMainStoryboardFile` (verified with `find` and `grep`). The whole entry point:

```objc
int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSApplication *app         = [NSApplication sharedApplication];
        FilesAppDelegate *delegate = [[FilesAppDelegate alloc] init];
        app.delegate               = delegate;
        [app run];
    }
    return 0;
}
```
(`src/main.m:13-23`)

There is no `setActivationPolicy:` call — the app runs from a bundle whose `Info.plist`
makes it a regular app. `delegate` is a local strong variable that lives for the whole
`[app run]`, which is what keeps the (weak) `app.delegate` alive.

### 2.2 App delegate lifecycle (`src/app_delegate.m`)

- `applicationWillFinishLaunching:` (38-48): register preference defaults **first**
  ("before anything reads preferences"), set `NSApp.applicationIconImage`, create the
  windows array, observe `NSWindowWillCloseNotification` (object nil) to drop closed
  window controllers. It runs before `application:openURLs:`, which a launch-to-open
  delivers *ahead of* `applicationDidFinishLaunching:`.
- `applicationDidFinishLaunching:` (62-90): build the menu bar, start observing
  defaults on the main queue, open the first window unless the launch already opened
  one, then `[NSApp activate]` (macOS 14+ API — the reason the deployment target is 14).
- **Window ownership**: the delegate holds `NSMutableArray<FilesMainWindow *> *windows`;
  `window_will_close:` removes the controller when its window closes (393-398).
  `front_window` = `NSApp.mainWindow.windowController` if it is one of ours, else the
  most recently opened (379-384). New windows cascade 24 pt down-right from the front
  one with `setFrameTopLeftPoint:` (289-292).
- `applicationShouldTerminateAfterLastWindowClosed:` returns a preference (410-414);
  `applicationShouldHandleReopen:hasVisibleWindows:` opens a window if none of *ours*
  is open (panels count as "visible windows", so the flag is ignored) (429-435).
- Launch checks are pushed to the next run-loop pass so the window draws first:
  `dispatch_async(dispatch_get_main_queue(), ^{ [self run_checks]; });` (115-119), and
  a guard flag (`synced`) stops preference-change handling until the first sync,
  because "the defaults written while the first window is built (its frame, the first
  bookmark list) are not preference changes" (155-168).
- About panel: `orderFrontStandardAboutPanelWithOptions:` with credits as an attributed
  string with links, explicit icon, and version text "built <date>" from a custom
  `Info.plist` key written by the Makefile (448-504).

### 2.3 Menu bar in code (`build_menu_bar`, `src/app_delegate.m:705-824`)

Pattern: one `NSMenu *bar`; per top-level menu an empty `NSMenuItem` whose `submenu` is
the titled menu; `NSApp.mainMenu = bar` at the end. Items come from one factory:

```objc
static NSMenuItem *
make_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mod)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key];
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand | mod;
    return item;
}
```

Rules that matter:

- **Target nil (first responder) for everything a view or window controller
  implements**; `item.target = self` only for the delegate's own actions (About,
  Preferences, New Window).
- **Edit menu uses the standard selectors** `undo:`, `redo:`, `cut:`, `copy:`,
  `paste:`, `selectAll:` (756-763). The content view implements the same selectors for
  its own objects (`src/browser_view.m:2223-2279, 2401-2405`). Result: when a text
  field is first responder its field editor gets them and they act on text; otherwise
  they reach the view. Release blocker B9 was custom `copy_action:` selectors that broke
  ⌘C/⌘V/⌘X/⌘A in every text field (`CODE_REVIEW.md:54`).
- ⌘⌫ is a menu key equivalent `@"\x08"` (768), not a `keyDown:` case, so bare Delete is
  never destructive (B10).
- ⌘F is a **custom** selector `find_action:` rather than `performFindPanelAction:` "so
  that a focused text field such as the address bar cannot intercept it"
  (`src/main_window.m:1176-1195`).
- **Preference checkboxes in a menu**: `make_pref_item` stores the defaults key in
  `representedObject`; one `toggle_pref:` flips it; `validateMenuItem:` sets the
  checkmark every time the menu opens, "so changes made elsewhere show too" (554-603).
- **The option list is data shared by the menu and the Preferences window**:
  `files_view_pref_groups()` returns groups of `@[title, key]` pairs; the View menu
  loops over it inserting separators between groups (782-789), and Preferences builds
  checkboxes from the same list (`src/prefs_window.m:12-24, 157-161`).
- **Dynamic menus** are rebuilt in `menuNeedsUpdate:` (`NSMenuDelegate`), not kept in
  sync by observers (642-667).
- AppKit renames an app-menu "Preferences…" item with ⌘, to "Settings…" at run time;
  `validateMenuItem:` sets the title back each time the menu opens (592-603).
- Menus present: App (About, Preferences…, Quit), File, Edit, View (Show Toolbar via
  `toggleToolbarShown:`, …), Go, Mounts. **There is no Window or Help menu, no
  Services/Hide/Hide Others/Show All items** — see section 8.

### 2.4 Window creation and frame persistence (`src/main_window.m:105-165`)

```objc
NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 940, 620)
                                            styleMask:style
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
win.title        = @"Files";
win.minSize      = NSMakeSize(700, 450);
win.toolbarStyle = NSWindowToolbarStyleExpanded;  /* titlebar above toolbar */
if (![win setFrameUsingName:@"FilesMainWindow"]) [win center];
win.frameAutosaveName = @"FilesMainWindow";

self = [super initWithWindow:win];
```

- Main windows are `NSWindowController` subclasses created with
  `init_with_url:` → `[super initWithWindow:win]`.
- **State restoration is frame autosave only.** `win.restorable = YES` existed in the
  first version and was removed; there is no `NSWindowRestoration`, no encode/restore
  of what the window shows. Every window uses the same autosave name. Sidebar
  visibility, dual-pane state, split positions and sort order are **not** persisted.
- Initial first responder is set on the next run-loop pass: "Deferred so it runs after
  the window's initial first-responder pass and once the toolbar has been laid out"
  (158-163).
- The controller tears down on `NSWindowWillCloseNotification` for its own window
  (`window_will_close:` → `shut_down` on each child view, 175-180) and removes
  observers in `dealloc` (185-191).

Utility windows (Preferences, Get Info, All Mounts, copy progress) are `NSPanel`s with
`NSWindowStyleMaskTitled | Closable | UtilityWindow` owned by an `NSWindowController`
singleton (see 3.12).

### 2.5 Preferences storage

- **`NSUserDefaults.standardUserDefaults` only** — no ini file, no plist of its own.
  Domain = bundle id (`com.cambio.files`).
- Keys are `extern NSString * const` declared once in `util.h` with a trailing comment
  each, defined once in `util.m:9-21` (`@"files.show_hidden_files"` etc.). Module-private
  keys are `static NSString * const` in that module (`bookmarks_bar.m:6-9`).
- Non-NO defaults are registered once at launch (`files_register_defaults`,
  `util.m:27-35`); a user-set value is never overridden.
- **Everything reacts to `NSUserDefaultsDidChangeNotification`**, observed with
  `addObserverForName:object:queue:usingBlock:` on `NSOperationQueue.mainQueue`
  (because "Defaults can change on any thread"). Each consumer keeps a mirror of the
  value it last applied and returns immediately if nothing relevant changed, since the
  notification fires for *any* default (including window-frame saves):

  ```objc
  BOOL show = [NSUserDefaults.standardUserDefaults boolForKey:files_pref_show_bookmarks];
  if (self.bookmarks_bar.hidden == !show) return;
  ```
  (`src/main_window.m:350-359`; fuller example `apply_view_prefs`,
  `src/browser_view.m:716-744`)
- Writers just write the default; nobody calls the consumers directly. That is how a
  toolbar button, a menu item and a Preferences checkbox stay in step across all
  windows.

### 2.6 Preferences window (`src/prefs_window.m`)

Singleton `NSWindowController` around an `NSPanel` (utility, titled, closable,
`hidesOnDeactivate = NO`). Content is one vertical `NSStackView` of section headings,
checkboxes, buttons and wrapped notes; the window is sized to the stack:

```objc
NSStackView *stack = [NSStackView stackViewWithViews:rows];
stack.orientation = NSUserInterfaceLayoutOrientationVertical;
stack.alignment   = NSLayoutAttributeLeading;
stack.spacing     = 6.0;
stack.edgeInsets  = NSEdgeInsetsMake(16, 20, 20, 20);
/* Space under each heading, and between the sections. */
[stack setCustomSpacing:8.0 afterView:view_heading];
…
self.window.contentView = stack;
[self.window setContentSize:stack.fittingSize];
```
(`prefs_window.m:193-205`)

A checkbox carries its defaults key in `identifier`; `toggle_pref_box:` writes it and
`refresh` (run on every defaults change and on show) sets every control from defaults
(134-143, 211-235). Changes apply immediately; there is no OK/Apply.

### 2.7 Info.plist (`resources/Info.plist`)

Hand-written, 55 lines. Keys: `CFBundleName`, `CFBundleDisplayName`,
`CFBundleIdentifier` (`com.cambio.files`), `CFBundlePackageType` `APPL`,
`CFBundleSignature` `????`, `NSPrincipalClass` `NSApplication`,
`NSHumanReadableCopyright`, `NSHighResolutionCapable` true,
`LSMinimumSystemVersion` `14.0`, `NSSupportsAutomaticTermination` false,
`NSRequiresAquaSystemAppearance` false (opts in to dark mode),
`CFBundleIconFile` `folder`, a usage description for Apple Events, and
`CFBundleDocumentTypes`. The Makefile adds `CFBundleExecutable`,
`CFBundleShortVersionString`, `CFBundleVersion` and a custom `FilesBuildDate` at build
time — a comment in the plist says so.

### 2.8 Icon

There is **no icon-generation step and no `.icns`**. `CFBundleIconFile` names a
512×512 PNG (`images/folder.png`) which the Makefile copies into `Contents/Resources`.
Because macOS 26 draws a non-squircle bundle icon on a grey rounded-square backdrop,
the app sets `NSApp.applicationIconImage` at launch and hands the image explicitly to
every alert and to the About panel (`files_app_icon`, `files_new_alert`,
`src/util.m:400-422`; comment `src/app_delegate.m:27-33`). Finder and the Dock while
the app is not running still show the backdrop; "only redrawn artwork avoids that".

### 2.9 Makefile (136 lines)

```make
APP      = Files
BUNDLE   = $(APP).app
CC       = clang
VERSION := $(strip $(shell cat VERSION))

CFLAGS   = -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter \
           -Werror=unguarded-availability \
           -mmacosx-version-min=14.0 -I src -MMD -MP
LDFLAGS  = -framework Cocoa -framework CoreServices -framework QuickLookThumbnailing \
           -framework UniformTypeIdentifiers -framework ServiceManagement -framework Quartz
SRCS     = $(wildcard src/*.m)
OBJS     = $(patsubst src/%.m,build/%.o,$(SRCS))
DEPS     = $(OBJS:.o=.d)

.PHONY: all run install clean test
all: $(BUNDLE)

IMAGES   = $(wildcard images/*.png images/*.icns)

$(BUNDLE): $(MACOS)/$(APP) resources/Info.plist VERSION $(IMAGES)
	@rm -rf $(RSRC) && mkdir -p $(RSRC)  # fresh, so removed images do not linger
	@cp resources/Info.plist $(CONTENTS)/Info.plist
	@plutil -replace CFBundleExecutable -string "$(APP)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleShortVersionString -string "$(VERSION)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleVersion -string "$(VERSION)" $(CONTENTS)/Info.plist
	@plutil -replace FilesBuildDate -string "$$(date -u +%Y-%m-%dT%H:%M:%SZ)" $(CONTENTS)/Info.plist
	@cp $(IMAGES) $(RSRC)/
	@codesign --force --sign "$(CODESIGN_IDENTITY)" $(BUNDLE)
	@touch $(BUNDLE)  # codesign rewrites the binary; keep the bundle newer than it

$(MACOS)/$(APP): $(OBJS)
	@mkdir -p $(MACOS)
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.m Makefile | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEPS)
```
(`Makefile:1-84`)

- **Flags**: ARC, `-O2`, `-Wall -Wextra`, deployment target 14.0, `-I src`.
  `-MMD -MP` + `-include $(DEPS)` give header dependency tracking; objects also depend
  on the `Makefile` so a flag change rebuilds everything. `-Werror=unguarded-availability`
  turns use of a too-new API into a build error (it cannot see messages to `__kindof`
  receivers such as `NSApp`). `-Wno-unused-parameter` is kept on purpose and documented:
  the unused parameters are all required by callback signatures. No `-g`, no modules.
- **Frameworks**: Cocoa is the only one the new apps certainly need. The others are
  feature-specific here (FSEvents, Quick Look thumbnails, UTType, login item, QL panel).
  The Notes port will add `-lsqlite3`.
- **Bundle assembly** is `mkdir` + `cp` + `plutil -replace` + `codesign`. The
  `Resources` folder is recreated from scratch each time.
- **Version**: one-line `VERSION` file → `CFBundleShortVersionString`/`CFBundleVersion`;
  build timestamp → custom plist key shown in About.
- **Code signing** (`Makefile:12-36, 72`): `CODESIGN_IDENTITY` defaults to the first
  "Apple Development" identity in the keychain, else `-` (ad hoc). Reason, from the
  Makefile comment: privacy grants (TCC) are tied to the signature — ad hoc means the
  exact build, so every rebuild loses the grant; a certificate means bundle id +
  certificate, which rebuilds keep. `build/codesign.id` records the identity; if it
  changes the bundle is back-dated (`touch -t 200001010000`) to force a re-sign. Only
  goals that build the bundle query the keychain (`SIGNING_GOALS` filters out `clean`
  and `test`). No hardened runtime, no entitlements, no notarisation (the built bundle
  shows `flags=0x0(none)`).
- **`make run`**: `open $(BUNDLE)`.
- **`make install`** (`Makefile:100-121`): per-user install to `~/Applications` (no
  sudo). If the installed copy is running it is sent SIGTERM (`pkill -TERM -xf`), waited
  for up to 5 s, the old bundle is `rm -rf`'d (because "`cp -r` into an existing bundle
  directory would nest the new one inside it"), the new one `cp -R`'d, and the app is
  reopened if it had been running.
- **`make test`** (`Makefile:125-133`): every `tests/*_test.m` is linked against all
  app objects except `main.o` and run; first failure stops.
- **`make clean`**: `rm -rf build $(BUNDLE)`.
- `.gitignore`: `Files.app/`, `build/`, `.DS_Store`.

### 2.10 Docs shipped

`README.md` (features, prerequisites, `make install`, first run, shortcuts table,
license), `BUILD.md` (toolchain, dependency table, target table, output tree, version,
incremental-build notes, source layout tree with one-line descriptions, signing,
platform notes), `CODE_REVIEW.md`, `LICENSE` (BSD 3-Clause), `VERSION`. Keep the
BUILD.md source-layout tree current — the first review found it listing 8 of 15
modules plus a dead one (`CODE_REVIEW.md:105`).

---

## 3. Reusable UI patterns

### 3.1 Text-field focus / selection highlight ring

**What Files does.** Three kinds of text field, three treatments:

| Field | Where | Treatment |
|---|---|---|
| Toolbar address bar | `main_window.m:941-970` | `NSTextFieldRoundedBezel`, `focusRingType = NSFocusRingTypeNone` |
| Toolbar search field (`NSSearchToolbarItem`) | `main_window.m:913-939` | `item.searchField.focusRingType = NSFocusRingTypeNone` ("No focus ring, like the address bar.") |
| Get Info name field | `info_panel.m:222-228` | stock `[NSTextField textFieldWithString:]` + `NSTextFieldRoundedBezel`; **default focus ring left on** |
| Inline rename in a table row | `browser_view.m:2437-2443` | the cell's own label made `editable`/`bordered`/`drawsBackground` with `NSColor.textBackgroundColor`; **default focus ring left on** |

So the "highlight around a text field" in Files is the system focus ring on a stock
bezeled field. Nothing is drawn by hand.

**Why it ended up this way (history).**
- The first Get Info panel built its name field from an `NSBox` (custom, corner radius
  6, 0.5 pt separator-colour border, `controlBackgroundColor` fill) containing a
  borderless field with `focusRingType = NSFocusRingTypeNone` and a custom
  `FilesVCenterCell` that overrode `drawingRectForBounds:`, `editWithFrame:…` and
  `selectWithFrame:…` to centre the text vertically and to stop the text jumping 2 pt
  when the field editor appeared (commit 9158896). Commit 9e83fa3 deleted all of it:
  "FilesVCenterCell and the name box are replaced by a rounded-bezel field" — the
  comment that remains says "A rounded-bezel field centres its text itself."
  (`info_panel.m:215`). **Lesson: do not imitate a bezeled field with a box, a
  borderless field and a custom cell. Use the stock bezel and let AppKit centre the
  text, place the field editor and draw the ring.**
- The toolbar search field's ring was on until d8b0edd. Focusing it made the search
  item slightly wider; the toolbar row was already 2 pt too wide because the address
  bar's padding had been measured from its *frame* while its width constraint sizes its
  *alignment rect* — so focus pushed an item into the overflow chevrons. Fix: measure
  with `alignmentRectForFrame:` and turn the ring off in the toolbar
  (`main_window.m:632-636, 920-921`).

**Rules for the ports.**
1. Standalone editing fields (dialogs, inspectors, inline edit): stock `NSTextField`
   with a bezel (`NSTextFieldRoundedBezel` is the house choice) and the default focus
   ring. Do not set `focusRingType` at all.
2. Fields inside the toolbar: `focusRingType = NSFocusRingTypeNone`.
3. When computing widths next to a bezeled control, use
   `[view alignmentRectForFrame:view.frame]`, never `view.frame` — "a bezeled field's
   frame is wider than it".
4. Do not let a field open with all its text highlighted unless that is intended:
   after showing a panel, `[self.window makeFirstResponder:nil]`
   (`info_panel.m:386-387`).
5. Return/Escape handling in a field goes through
   `control:textView:doCommandBySelector:` and ends with
   `[self.window makeFirstResponder:nil]` so focus leaves the field "instead of AppKit
   re-selecting all the text" (`info_panel.m:748-762`).

Excerpt (the complete toolbar text field recipe):

```objc
self.address_bar = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 400, 22)];
self.address_bar.placeholderString = @"Path";
self.address_bar.bezelStyle        = NSTextFieldRoundedBezel;
self.address_bar.font              = [NSFont monospacedSystemFontOfSize:12.0
                                                                 weight:NSFontWeightRegular];
self.address_bar.target            = self;
self.address_bar.action            = @selector(address_bar_action:);
self.address_bar.focusRingType     = NSFocusRingTypeNone;
/* One line, the end of the path kept in view when it is too long. */
self.address_bar.usesSingleLineMode = YES;
self.address_bar.cell.wraps         = NO;
self.address_bar.cell.scrollable    = YES;
self.address_bar.lineBreakMode      = NSLineBreakByTruncatingHead;
```
(`main_window.m:942-954`)

### 3.2 Animated sliding panes ("sliders")

**What.** The info sidebar slides in from the right; the second browser pane slides in
from the right and back out. Both are `NSSplitViewItem` collapse animations.

**Where.** `build_layout` (`main_window.m:279-326`), `toggle_sidebar_action:`
(1473-1483), `pane_item:` + `toggle_dual_pane:` (677-738).

**How.**
- Two nested `NSSplitViewController`s. `content_split_vc` = [browser area | sidebar];
  `pane_split_vc` = [primary pane | optional secondary pane] and is a child view
  controller of the browser area's controller. Each child is a bare
  `NSViewController` whose `view` is set to an existing `NSView` — no view-controller
  subclasses are written.
- The split view controller's `view` is added to a plain container with a frame and an
  autoresizing mask; divider style thin.
- Sidebar item: `canCollapse = YES`, `minimumThickness`, `holdingPriority =
  NSLayoutPriorityDefaultLow + 1` ("window resizes go to the browser, not the
  sidebar"), created `collapsed = YES`.
- Toggle is one line: `self.sidebar_item.animator.collapsed = !show;`

**The slide-in/out of a pane that does not exist yet** needs sequencing:

```objc
if (self.panes_animating) return;
self.panes_animating = YES;
__weak FilesMainWindow *ws = self;

if (self.secondary_browser) {
    /* Single-pane from now on; the closing pane only slides out. */
    NSSplitViewItem *item = self.secondary_item;
    [self.secondary_browser shut_down];
    self.secondary_browser = nil;
    self.secondary_item    = nil;
    [self set_active_pane:0];
    [self.window makeFirstResponder:self.primary_browser];
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        item.animator.collapsed = YES;
    } completionHandler:^{
        [ws.pane_split_vc removeSplitViewItem:item];
        ws.panes_animating = NO;
    }];
} else {
    …
    NSSplitViewItem *item = [self pane_item:self.secondary_browser];
    item.canCollapse = YES;
    item.collapsed   = YES;
    [self.pane_split_vc addSplitViewItem:item];
    …
    /* Expand on the next pass, once the item is laid out collapsed. */
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            item.animator.collapsed = NO;
        } completionHandler:^{
            ws.panes_animating = NO;
        }];
    });
}
```
(`main_window.m:699-736`)

**Gotchas (all learned the hard way — see the deleted code in commits db1dc02, afa8251,
e1f5df1).**
- Do not hand-roll collapse on a raw `NSSplitView`. The first version had ~80 lines of
  delegate code, a `_collapsing_sidebar` flag to keep the divider "live" during
  `setPosition:`, a deferred collapse after the first layout pass, and a bug where the
  divider could not be grabbed after showing the sidebar because subview frames had been
  set directly (no cursor rect until the next resize; the interim fix was
  `setPosition:ofDividerAtIndex:` + `invalidateCursorRectsForView:`). All replaced by
  `NSSplitViewController` + `NSSplitViewItem`, which gives collapse, minimum
  thickness, holding priority and animation for free (`CODE_REVIEW.md:157`).
- An item added already expanded just appears. Add it `collapsed = YES`, then expand in
  a `dispatch_async` to the main queue so it is first laid out collapsed.
- On close, do the model teardown *before* the animation (so the app is already
  "single-pane" logically) and remove the split item in the completion handler.
- Guard with an `animating` flag; ignore toggles while a slide is running.
- Give the new pane's view an initial frame of half the width (`floor(size.width / 2)`).
- Never reparent the permanent pane: single-pane mode is "a split with one item"
  ("primary_pane is never reparented"; the original comment cited CA-layer reparenting
  issues).
- Release heavy content when a pane is hidden (`[self.info_sidebar clear]`).
- No duration or timing function is set: the slides use AppKit's defaults.

**Other animation in Files** (the only other one): the copy panel's log disclosure
grows the window with `[self.window setFrame:wf display:YES animate:YES]` and shrinks it
with an explicit 0.2 s animator call (`copy_progress.m:220-251`). The window grows
*downward* (origin.y decreases by the same delta) so the top edge stays put.

### 3.3 Auto-sizing

Files has six distinct fit-to-content mechanisms. Pick by case.

**(a) Table columns fit their content and exactly fill the visible width**
— `apply_auto_fit`, `browser_view.m:1557-1615`.

```objc
CGFloat spacing    = self.table.intercellSpacing.width;
CGFloat name_extra = [self icon_size] + 4 + 2 + 2 + spacing; /* icon + gap + pads */
CGFloat cell_extra = 2 + 2 + spacing;                        /* l-pad + r-pad */
static const CGFloat header_pad = 10;                        /* header padding + margin */
…
CGFloat max_w = [col.title sizeWithAttributes:
    @{ NSFontAttributeName: col.headerCell.font }].width + header_pad;
…
    /* Text fields need whole points: 42.3 pt of text does not fit
     * in 42.3 pt, it is truncated. */
    CGFloat w = ceil([text sizeWithAttributes:attrs].width) + extra;
…
col.width = MAX(col.minWidth, max_w);
total    += col.width + self.table.intercellSpacing.width;
…
Class v_scroller = rows_h > NSHeight(self.scroll.frame) ? NSScroller.class : Nil;
CGFloat avail = [NSScrollView contentSizeForFrameSize:self.scroll.frame.size
                              horizontalScrollerClass:Nil
                                verticalScrollerClass:v_scroller
                                           borderType:self.scroll.borderType
                                          controlSize:NSControlSizeRegular
                                        scrollerStyle:self.scroll.scrollerStyle].width;
NSTableColumn *name_col = [self.table tableColumnWithIdentifier:col_name];
name_col.width = MAX(name_col.minWidth, name_col.width + (avail - total));
```

Gotchas, each one a past bug:
- `ceil()` every measured text width.
- Add the table's `intercellSpacing.width` to each column's padding: "the table takes
  [it] out of a column's width before laying out its cell view".
- Measure with the **same font the cell uses** — one `cell_font_for_column()` is shared
  by the cell builder and the measurement (the first version measured the monospaced
  column with the wrong font, `CODE_REVIEW.md:138`).
- Measure the same strings the cells show — one `cell_text_for_column:url:` is shared.
- After sizing each column, give the leftover (or take the overflow) to the flexible
  column so the columns exactly fill the visible width; otherwise a spurious horizontal
  scrollbar appears at launch.
- Compute the visible width with `contentSizeForFrameSize:…` and reserve the vertical
  scroller when the rows will overflow: "With legacy (always-visible) scrollers the
  vertical scroller takes width whenever the rows overflow, and it may not be tiled in
  yet on the first load".
- Cap the rows measured (`AUTO_FIT_ROW_LIMIT` 2000).
- Run it only on navigation, search start, the toggle being turned on, a row-height
  change or a column appearing/disappearing — **not on every data refresh**
  (`auto_fit_pending` flag, `browser_view.m:1153-1156`).
- For columns whose content arrives later, size for a representative widest string up
  front (`@"888.8 MB"`, `@"tracked"`).
- The table is set to `NSTableViewFirstColumnOnlyAutoresizingStyle` so manual window
  resizes also go to the Name column.
- When a column is hidden/shown, add/remove its width (+ spacing) from the flexible
  column so nothing is pushed off the right edge (`update_column_visibility`, 536-556).

**(b) Table columns + window fit the data (a panel that sizes itself)**
— `fit_columns` and `resize_to_fit`, `mounts_panel.m:353-379, 419-448`. Columns =
max(header, widest cell) with `cell_pad 16`, `head_pad 20`; window content =
sum(columns) + spacing + 2, clamped to [600, 90 % of screen width] × [150, 80 % of
screen height]; grows upward (`frame.origin.y -= dh`) so the top-left stays put. Done
once when the panel is shown, not on in-place refresh.

**(c) Form panel fits its visible rows** — `fit_window`, `info_panel.m:360-371`.

```objc
NSWindow *w = self.window;
[w.contentView layoutSubtreeIfNeeded];
NSSize fit = w.contentView.fittingSize;
NSRect c   = [w contentRectForFrameRect:w.frame];
CGFloat top = NSMaxY(c);
c.size      = NSMakeSize(panel_width, fit.height);
c.origin.y  = top - fit.height;
[w setFrame:[w frameRectForContentRect:c] display:YES];
```

Requires the content to be fully constrained: the root stack is pinned to all four
edges of the content view with a margin, and the content view has a fixed
`widthAnchor` constraint (`info_panel.m:332-341`). Hiding `NSGridRow`s
(`r.hidden = YES`) and calling `fit_window` again shrinks the panel
(`set_git_section_visible:`, 541-547). Value labels are given low horizontal
compression resistance so long text truncates instead of widening the panel
(`make_value`, 47-56).

**(d) Window fits a stack view** — `[self.window setContentSize:stack.fittingSize]`
with the stack as `contentView` (`prefs_window.m:204-205`). Wrapping labels need
`preferredMaxLayoutWidth` to have a fitting height (`note_label`, 51-59).

**(e) A toolbar text field that fills the leftover width** — `fit_address_bar`,
`main_window.m:625-665`. NSToolbar "sizes a view item to its fitting width and puts any
room left over after its last item"; flexible-space items do not stretch a view item.
So the field has a low-priority `widthAnchor == constant` constraint plus a required
`>= address_min_w`, and on every `NSWindowDidResizeNotification` (and once after first
layout) the constant is set to toolbar-row width − 2×margin − other items − padding.
Measurements are taken only when every item is laid out with the search field at full
width, and cached for use while items are squeezed or in overflow. After changing the
constant: `self.address_item.view = self.address_bar;` — "The toolbar measures an
item's view when it is set, and keeps that size until the window is resized again".
The search item gets a fixed width (`preferredWidthForSearchField` + a `<=`
constraint) so the address bar is the only flexible one. **This walks
`address_bar.superview.superview` (private toolbar view hierarchy); it works on the
macOS versions the owner runs but is fragile — only port it if a stretching toolbar
field is really needed.**

**(f) A strip of buttons exactly as wide as its buttons** — `rebuild_buttons`,
`bookmarks_bar.m:328-405`: each button `sizeToFit`, height clamped to bar − 4, laid out
left to right with a gap; the container's frame is set to the running x, and that
container is the scroll view's `documentView` — "Exactly as wide as the buttons, so the
bar scrolls only when the window is too narrow for them".

**(g) Custom drawing that sizes its own gutter** — the speed graph measures its three
Y-axis labels and makes the left margin `ceil(widest) + 2 × pad`, "so labels never
cross the view border whatever the scale" (`speed_graph.m:184-199`).

**(h) Centred label that yields to a right-hand label** — status bar,
`main_window.m:254-272`: centre-X constraint at `DefaultHigh` priority, low compression
resistance + tail truncation on the centre label, `>=` leading and `<=` trailing-to-
the-other-label constraints, required hugging/compression on the right label.

### 3.4 Tables

Everything is a **view-based `NSTableView`** (no cell-based tables, no `NSOutlineView`,
no `NSCollectionView` anywhere in Files — the ports' outline/collection needs are not
covered by precedent).

Setup (`build_table`, `browser_view.m:433-479`):

```objc
self.table = [[NSTableView alloc] initWithFrame:self.bounds];
self.table.style                              = NSTableViewStylePlain;
self.table.usesAlternatingRowBackgroundColors = YES;
self.table.allowsMultipleSelection            = YES;
self.table.allowsEmptySelection               = YES;
self.table.columnAutoresizingStyle            = NSTableViewFirstColumnOnlyAutoresizingStyle;
…
self.table.dataSource   = self;
self.table.delegate     = self;
self.table.target       = self;
self.table.doubleAction = @selector(row_double_clicked:);
…
self.scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
self.scroll.documentView          = self.table;
self.scroll.hasVerticalScroller   = YES;
self.scroll.hasHorizontalScroller = YES;
self.scroll.autohidesScrollers    = YES;
self.scroll.autoresizingMask      = NSViewWidthSizable | NSViewHeightSizable;
```

- **Columns from a static table** (`column_specs[]`), each with identifier, title,
  width, minWidth, `NSTableColumnUserResizingMask | NSTableColumnAutoresizingMask`.
- **Row reuse**: `makeViewWithIdentifier:owner:nil`; on miss build an `NSTableCellView`
  with a borderless non-editable `NSTextField` (and `NSImageView` in the first column)
  laid out with Auto Layout — 2 pt leading/trailing, centred vertically, icon→text gap
  4 — and set `cell.textField` / `cell.imageView`. Then only set values on reuse
  (`browser_view.m:1751-1809`). When a layout constant is baked into a cell (icon
  size), **include it in the reuse identifier**:

  ```objc
  /* Name cells are built for one icon size, so each size gets its own
   * reuse identifier. */
  NSString *reuse_id = [ident isEqualToString:col_name]
      ? [NSString stringWithFormat:@"%@.%g", ident, icon_sz] : ident;
  ```
- **Cells never touch slow APIs**: all values are prefetched with the listing; byte and
  date formatters are shared statics; icons come from an `NSCache`
  (`CODE_REVIEW.md:136, 144-145`).
- **Custom row views**: none (no `NSTableRowView` subclass). Row tinting uses the
  delegate hook: `tableView:didAddRowView:forRow:` sets `rowView.backgroundColor`;
  **`nil` restores the default alternating background** (`mounts_panel.m:613-626`).
- **Selection colours**: system default; nothing customised. Inactive pane is shown by
  dimming the whole pane view (`alphaValue = 0.45`, `main_window.m:495-502`), not by
  restyling selection.
- **Row height**: `table.rowHeight` (20 normal, 40 "Large Rows", 22 in the mounts
  panel); changing it needs `reloadData`.
- **Sorting, two styles.** (1) Manual: no `sortDescriptorPrototype`; handle
  `tableView:didClickTableColumn:`, keep `sort_key`/`sort_ascending`, re-sort, and show
  the direction by appending " ↑"/" ↓" to the column title because
  "`setIndicatorImage:inTableColumn:` is unreliable with NSTableViewStylePlain"
  (`browser_view.m:1457-1473, 1822-1835`). (2) Standard: `sortDescriptorPrototype` per
  column keyed on record property names, re-sort in
  `tableView:sortDescriptorsDidChange:` (`mounts_panel.m:193-206, 595-601`). Use (2)
  when rows are model objects with KVC-readable properties. String ordering is
  `localizedStandardCompare:` (Finder order: file2 < file10), nil-safe.
- **Selection survives reloads by identity, not row number**: "AppKit keeps selected
  row numbers across reloadData, which point at other items once rows are reordered or
  rebuilt" — so capture the selected items' keys before, `reloadData`, then
  `selectRowIndexes:byExtendingSelection:NO` for the rows whose key matches, and
  optionally `scrollRowToVisible:` (`show_rows:selecting:reveal:`, 1136-1158).
- **Partial reload** of one column when late data arrives:
  `reloadDataForRowIndexes:columnIndexes:` (1085-1089, 1235-1237).
- **Header context menu** for show/hide columns: `table.headerView.menu`; the hidden
  set is a defaults array, so every table in every window follows (489-556).
- **Inline rename/editing** — edit the cell's own text field in place:

  ```objc
  [self.table scrollRowToVisible:row];
  NSInteger col = [self.table columnWithIdentifier:col_name];
  NSTableCellView *cell = [self.table viewAtColumn:col row:row makeIfNecessary:YES];
  NSTextField *tf = cell.textField;
  …
  tf.stringValue     = name;
  tf.editable        = YES;
  tf.bordered        = YES;
  tf.drawsBackground = YES;
  tf.backgroundColor = NSColor.textBackgroundColor;
  tf.delegate        = self;
  [self.window makeFirstResponder:tf];
  …
  tf.currentEditor.selectedRange = NSMakeRange(0, len ? len : name.length);
  ```
  (`begin_rename:`, `browser_view.m:2420-2451`)

  Commit in `controlTextDidEndEditing:` (Return, Tab or click-away); cancel on Escape
  via `control:textView:doCommandBySelector:` with `cancelOperation:` →
  `abortEditing` + restore text; `finish_rename` puts the field back to label state
  (`editable/bordered/drawsBackground = NO`, `delegate = nil`) and returns focus to the
  table (2458-2542). Gotchas:
  - The first version floated a separate `NSTextField` over the row using three
    coordinate conversions; it was misplaced and did not scroll with the row
    (`CODE_REVIEW.md:159`, removed in 0541ef7). Edit the cell's own field.
  - **Do not reload the table while editing** — it destroys the cell. `reload_directory`,
    `show_rows:` and the timed column refresh all check `rename_url` and set
    `reload_deferred`; `finish_rename` runs the deferred reload (1026-1029, 1139-1142,
    1232, 2470-2473).
  - Hold the field `weak` and the edited item `strong`; clear both before doing anything
    else in the end-editing handler, because end-editing can re-enter.
  - Create-then-rename: record `pending_names`/`pending_rename`, reload, and start the
    edit when the listing that contains the new item lands (1279-1304, 2341-2348).

### 3.5 Sidebars and split views

See 3.2 for construction. Constants: sidebar initial width 180, minimum 140, browser
minimum 400 (`main_window.m:24-26`). The sidebar view itself (`FilesInfoSidebar`) is an
`NSView` with `isFlipped = YES` ("so the layout constants read as distances from the
top edge") and frame-based subviews with autoresizing masks; it draws its own
background and a 1 pt left separator in `drawRect:` (`info_sidebar.m:62-100`). No
`NSVisualEffectView`, no source-list style — the sidebar is an opaque
`windowBackgroundColor` panel.

### 3.6 Toolbar, bookmark bar, overflow

**Toolbar** (`main_window.m:199-207, 797-1005`):
- `NSToolbar` with `allowsUserCustomization = NO`, `displayMode =
  NSToolbarDisplayModeIconOnly`; window `toolbarStyle = NSWindowToolbarStyleExpanded`.
- One `toolbar_identifiers()` function returns the array used for both allowed and
  default identifiers.
- **Standard image items** via `button_item(ident, label, symbol, action, target)`:
  `label`, `toolTip`, SF Symbol image with accessibility description, `bordered = YES`.
  "Standard items get an overflow-menu representation and validation for free." The
  first version used custom-view `NSButton`s, which had neither
  (`CODE_REVIEW.md:158`).
- State is shown by swapping the image in `validateToolbarItem:` (`eye`/`eye.slash`,
  `rectangle.split.2x1`/`.fill`) and enabling/disabling there; call
  `[self.window.toolbar validateVisibleItems]` after state changes.
- `NSMenuToolbarItem` for a "+" pull-down; `NSSearchToolbarItem` for search (target/
  action on `item.searchField`, scope menu via
  `((NSSearchFieldCell *)item.searchField.cell).searchMenuTemplate`).
- ⌘F: `[(NSSearchToolbarItem *)item beginSearchInteraction]` after making the toolbar
  visible.
- Overflow: handled by AppKit for standard items; the only custom work is 3.3(e).

**Bookmark bar** (`bookmarks_bar.m`): a 28 pt `NSView` strip under the toolbar with a
fixed "add" button and a horizontal scroll view of recessed-bezel buttons
(`NSBezelStyleRecessed`, `NSButtonTypeMomentaryPushIn`, 11 pt, image left). Per-button
context menu via `btn.menu`. The fixed button sends its action up the responder chain
(`target = nil`). **Overflow is handled by scrolling**, see 3.7. Shown/hidden by a
preference: set `hidden` and give its height to the area below
(`main_window.m:350-359`). The list is shared across windows: after saving, the bar
posts a notification (object = itself) and the other bars reload, "so no bar later
saves an outdated copy over it" (`bookmarks_bar.m:194-206, 267-276`).

### 3.7 Scroll views and wheel handling

- Standard: `hasVerticalScroller`, `hasHorizontalScroller`, `autohidesScrollers = YES`,
  autoresizing to fill.
- **Vertical wheel on a horizontal-only scroller** — subclass `NSScrollView` and
  translate:

  ```objc
  - (void)
  scrollWheel:(NSEvent *)event
  {
      CGFloat dx    = event.scrollingDeltaX, dy = event.scrollingDeltaY;
      CGFloat delta = fabs(dx) > fabs(dy) ? dx : dy;
      if (!event.hasPreciseScrollingDeltas) delta *= wheel_line_w;
      NSClipView *clip = self.contentView;
      NSRect bounds    = clip.bounds;
      if (delta == 0 || NSWidth(self.documentView.frame) <= NSWidth(bounds)) return;

      bounds.origin.x -= delta;
      [clip scrollToPoint:[clip constrainBoundsRect:bounds].origin];
      [self reflectScrolledClipView:clip];
  }
  ```
  (`bookmarks_bar.m:132-145`; `wheel_line_w = 20.0`). Gotchas: a notched mouse wheel
  reports *lines*, a trackpad *points* (`hasPreciseScrollingDeltas`); clamp with
  `constrainBoundsRect:`; do nothing when the content already fits.
- Scrolling text log: `NSTextView` in an `NSScrollView` with `NSBezelBorder`,
  `verticallyResizable`, container width tracking, **`layoutManager.
  allowsNonContiguousLayout = YES`** ("Lay out only what is visible"), appended in one
  `beginEditing`/`endEditing`, capped at 1000 lines trimmed 100 at a time, and
  `scrollToEndOfDocument:` only while the log is visible (`copy_progress.m:174-205,
  285-313`).

### 3.8 Context menus

- Build once, assign to `view.menu` / `table.menu` / `button.menu` /
  `table.headerView.menu`. Items target `self` (`browser_view.m:563-594`).
- **Right-click selection behaviour** must be implemented — AppKit does not change the
  selection on right-click. In the menu delegate:

  ```objc
  - (void)
  menuNeedsUpdate:(NSMenu *)menu
  {
      NSInteger row = self.table.clickedRow;
      if (row < 0) {
          [self.table deselectAll:nil];
      } else if (![self.table isRowSelected:row]) {
          [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                  byExtendingSelection:NO];
      }
  }
  ```
  (`browser_view.m:1880-1890`). After that every command acts on the selection — the
  same items its menu-bar and keyboard versions use, so there is one implementation.
- Conditional items: `validateMenuItem:` sets `item.hidden`, `item.title`, `item.state`
  and returns enabled (`browser_view.m:1994-2022`).

### 3.9 Keyboard handling and first-responder management

- **Shortcuts with ⌘ are menu key equivalents**, never `keyDown:` cases.
- **Unmodified keys** are handled in the container view's `keyDown:` — the table passes
  what it does not use up the responder chain; the view returns
  `acceptsFirstResponder = YES`. Check modifier flags explicitly and call `super` for
  everything else:

  ```objc
  NSEventModifierFlags mods = event.modifierFlags & (NSEventModifierFlagCommand |
                                                     NSEventModifierFlagControl |
                                                     NSEventModifierFlagOption);
  if ([chars isEqualToString:@" "] && mods == 0) {
  ```
  (`browser_view.m:2050-2075`: Return = open, Space = Quick Look, ←/→ = back/forward.)
  Text fields keep their own arrows/Return because they are a different first
  responder.
- **Window-level keys** (Tab to switch pane) in the window controller's `keyDown:`
  (`main_window.m:747-758`); the controller is in the responder chain behind its views.
- **Actions that must work whatever has focus** (New, Go, toggles) are implemented on
  the window controller and forwarded to the active pane
  (`main_window.m:1115-1131`) — otherwise a focused toolbar field swallows them.
- **Tracking which pane is active**: KVO on the window's `firstResponder`, walk up
  `superview` from the responder to find which pane contains it
  (`main_window.m:769-788`). Registered with a context pointer and removed in `dealloc`.
- **Initial focus**: `dispatch_async` to main, then `makeFirstResponder:`.
- **Dropping focus**: `[window makeFirstResponder:nil]` after a field's action
  (`address_bar_action:` 1172) or after showing a panel.
- **Returning focus** after inline edit: `[self.window makeFirstResponder:self.table]`.
- **Escape on a panel button**: `button.keyEquivalent = @"\033"`; default button
  `@"\r"` (`copy_progress.m:169, 440`).
- **The search-field focus overflow fix**: see 3.1 / 3.3(e).
- Quick Look panel: implement `acceptsPreviewPanelControl:`,
  `beginPreviewPanelControl:`, `endPreviewPanelControl:` on the view in the responder
  chain; forward arrow keys from `previewPanel:handleEvent:` to the table
  (`browser_view.m:2085-2213`).

### 3.10 Drag and drop, pasteboard

- **Drag source**: return a pasteboard writer per row —
  `tableView:pasteboardWriterForRow:` returns the model object if it conforms to
  `NSPasteboardWriting` (`NSURL` here). Allowed operations via
  `setDraggingSourceOperationMask:forLocal:` for both YES and NO
  (`browser_view.m:462-465, 2556-2562`). For the ports' own row types use
  `NSPasteboardItem` with a private UTI string.
- **Drop target**: `registerForDraggedTypes:`; `tableView:validateDrop:proposedRow:
  proposedDropOperation:` reads objects with `readObjectsForClasses:options:`, decides
  the destination, retargets the highlight with `[tv setDropRow:-1
  dropOperation:NSTableViewDropOn]` for "whole table", refuses no-op drops, and
  returns Move unless the source only allows Copy (`drop_operation`, 2611-2618);
  `acceptDrop` starts the work asynchronously and returns YES (2635-2694).
- **General pasteboard**: `clearContents` then `writeObjects:` / `setString:forType:`.
  A **custom private type** carries app-only state (cut vs copy):
  `static NSString * const cut_flag_type = @"com.cambio.files.cut-flag";`
  (`file_ops.m:15, 743-750`). Reading: `readObjectsForClasses:options:` /
  `canReadObjectForClasses:options:`.
- Drop and paste go through the same transfer function — the first version had a
  separate drop path that had rotted into calling a selector that did not exist (B1).

### 3.11 Custom drawing

- **`drawRect:` with `NSBezierPath` and `NSColor` — no CALayer drawing, no
  `updateLayer`.** `wantsLayer` appears twice: on a container, and on a 12×12 legend
  swatch styled through `layer.backgroundColor/borderColor/cornerRadius`
  (`mounts_panel.m:242-253`).
- Hairline separators: either an `NSBox` with `boxType = NSBoxSeparator`
  (`main_window.m:274-277`) or a 1 pt line at y = 0.5 in `drawRect:` with
  `NSColor.separatorColor` (`bookmarks_bar.m:478-487`). Strokes are placed on half-point
  coordinates / `NSInsetRect(b, 0.5, 0.5)` to stay crisp.
- **`speed_graph.m`** (268 lines) is the model for a data plot:
  - Data in fixed C arrays in the ivar block (`double _bytes[512]`), bounded memory;
    when full, adjacent pairs are merged ("lose time resolution, never history").
  - `add_bytes:seconds:` updates the model and calls `setNeedsDisplay:YES`.
  - `drawRect:` always redraws the whole view (dirty rect ignored): fill
    `controlBackgroundColor`, compute scale, measure labels, draw dashed gridlines
    (`setLineDash:count:phase:`, 0.5 pt), fill the area path, draw the average line and
    its label kept inside the plot, plot border, view border.
  - About one data bucket per pixel column; O(width), not O(samples).
  - Text via `-[NSString drawAtPoint:withAttributes:]` and `sizeWithAttributes:`.
- **Composited images that follow light/dark**: build with
  `+[NSImage imageWithSize:flipped:drawingHandler:]`; "The handler runs whenever the
  image is rendered, under the appearance of the view drawing it"
  (`git_info.m:185-208`).
- Always `copy` a shared named image before changing its `size`
  (`[[NSImage imageNamed:NSImageNameTrashEmpty] copy]`, `main_window.m:466-467`).

### 3.12 Colours, fonts, dark mode, materials

- **Colours are semantic system colours only**: `labelColor`, `secondaryLabelColor`
  (by far the most used — 10 sites), `tertiaryLabelColor`, `separatorColor`,
  `windowBackgroundColor`, `controlBackgroundColor`, `textBackgroundColor`,
  `linkColor`, `systemGreenColor` (25 % alpha fill). Three hard-coded colours exist and
  are debt (section 8).
- **Dark mode**: no code. `NSRequiresAquaSystemAppearance = false` in Info.plist plus
  semantic colours and SF Symbols is the entire strategy. No `effectiveAppearance`
  checks, no `viewDidChangeEffectiveAppearance`.
- **Vibrancy/materials**: none. No `NSVisualEffectView` anywhere.
- **Fonts**: system font at explicit sizes (9–13), `NSFont.smallSystemFontSize` /
  `NSFont.systemFontSize` for form text, bold for headings and names,
  `monospacedSystemFontOfSize:weight:Regular` for paths, permissions, hashes, logs and
  tabular diagnostic tables. Full list in section 6.
- **Icons**: SF Symbols via `imageWithSystemSymbolName:accessibilityDescription:`;
  sized with `NSImageSymbolConfiguration configurationWithPointSize:weight:` when not
  in a toolbar.
- **Retina**: `NSHighResolutionCapable`; the only explicit scale handling is passing
  point size + `window.backingScaleFactor` to an API that wants both — and *not*
  pre-multiplying the size (`info_sidebar.m:190-200`).
- **Spacing constants** are `static const CGFloat` at the top of each file with a
  comment; derived positions are computed from them (`info_sidebar.m:6-16`).

### 3.13 Panels, sheets, alerts, popovers

> **SUPERSEDED (2026-10-10).**  Files has since replaced every alert with its
> own tool panel (`../shared_ui/src/shared_tool_panel.{h,m}`: "Wherever a native dialog would appear,
> Files uses this").  Notes follows the current Files, not this snapshot: no
> `NSAlert`, no alert sheets — see `shared_tool_panel.h`, in `../shared_ui` and INTERFACES.md §11.  The
> alert bullets below (and the alert rows in §4) describe the old Files only.

- **Alerts**: built with `files_new_alert()`, presented with
  `files_present_alert(alert, window, handler)` — sheet when there is a window,
  `runModal` otherwise (`util.m:415-450`). First button is the action, second is
  "Cancel"/"Not Now"; destructive confirmations set `alertStyle = NSAlertStyleWarning`
  and say "You cannot undo this action." (`browser_view.m:1630-1635`). The response is
  compared with `NSAlertFirstButtonReturn`.
- **Sheets** are only ever alert sheets. No custom sheet windows.
- **Popovers**: none in Files.
- **Utility panels**: `NSPanel` + `NSWindowStyleMaskUtilityWindow`, owned by a
  singleton `NSWindowController`; `show_…` refreshes then `showWindow:nil` +
  `makeKeyAndOrderFront:`. Per-panel flags in use: `hidesOnDeactivate = NO` (panels
  that should stay visible when the app is in the background), `releasedWhenClosed =
  NO` + `level = NSFloatingWindowLevel` (Get Info), `worksWhenModal = YES` +
  `becomesKeyOnlyIfNeeded = YES` (progress).
- **Progress panel** (`copy_progress.m`) — the model for a non-modal, per-operation
  window:
  - Created per operation by a class method; shown immediately so Cancel is available.
  - **Lifetime**: "NSWindow.windowController is unsafe_unretained; without this the
    panel could be freed mid-copy" — the controller keeps `_self_ref = self` and clears
    it in `windowWillClose:` (95, 536-541, 656-662).
  - Cancel = `atomic_store(&_cancel, 1)`; the worker polls the flag. Closing the
    window also sets it.
  - Worker thread → UI: the worker only records its latest state under an
    `os_unfair_lock`; a 0.25 s `NSTimer` on the main run loop drains it
    (`take_engine_updates`, 369-386). No main-queue hop per event.
  - The timer is added in `NSRunLoopCommonModes` (546-548) so it keeps firing during
    menu tracking and sheets, and is invalidated in `windowWillClose:`.
  - A background thread that needs a user decision blocks on a semaphore while a sheet
    is shown on the main queue, with `NSCAssert(!NSThread.isMainThread, …)`
    (592-630).
  - On finish the button becomes "Close" with Return as key equivalent; a cancelled
    operation just closes the panel.

### 3.14 Background work

The universal shape (`reload_directory`, `browser_view.m:1021-1093`):

```objc
NSUInteger generation = ++self.list_generation;
… capture every input the worker needs as locals (never read self.* off-main) …
__weak FilesBrowserView *ws = self;

dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    … slow work with the captured values, using pure functions …
    dispatch_async(dispatch_get_main_queue(), ^{
        FilesBrowserView *s = ws;
        if (!s || generation != s.list_generation || s.search_active) return;
        … apply …
    });
});
```

- **Staleness guard**: a per-purpose generation counter (`list_generation`,
  `search_generation`, `load_generation`, `space_generation`, `git_generation`) or the
  identity of a token object (`s.hash_job != job`, `s.size_token != token`,
  `s->_pending_request != req`). Newest request wins; old results are dropped silently.
- **Queues**: global `QOS_CLASS_USER_INITIATED` for things the user is waiting on;
  global `QOS_CLASS_UTILITY` for background reads; one app-wide **serial** utility
  queue (`files_background_queue()`) for the app's own housekeeping so two such jobs
  never overlap; a private serial queue where ordering matters (one-at-a-time folder
  counts; anything using a non-thread-safe API such as `NSAppleScript`).
- **Do not use a per-view serial queue for user-initiated loads** — one hung request
  blocks every later one (`CODE_REVIEW.md:233-234`).
- **If the sort/filter settings changed while the worker ran**, redo that part on the
  main thread instead of showing rows prepared for the old settings (`row_order`
  string compared before/after, 1001-1006, 1066).
- **Two-phase results**: show the fast part first, fill in the slow part with a second
  main-queue hop under the same generation (1068-1091).
- **Cancellation**: `FilesCancelToken` — an object wrapping an `atomic_int`; the worker
  block holds it strongly and polls `[token cancel_flag]`; the owner calls `cancel` and
  drops its reference (`util.m:452-474`; use in `info_panel.m:409-436`,
  `browser_view.m:1184-1215`). C workers take `const atomic_int *cancel` and return a
  distinct code for "cancelled".
- **External tools**: `files_run_tool(tool, args, timeout, &out)` — `NSTask` with stdin
  and stderr at `/dev/null`, stdout captured, terminated after a timeout by a
  `dispatch_after` (`util.m:147-169`). Always off the main thread.
- **Throttle / coalesce**: a `BOOL scheduled` flag + one `dispatch_after`; later calls
  within the window return immediately (see 3.15).

### 3.15 Timers

- **One-shot delays and coalescing use `dispatch_after` on the main queue with a
  pending flag**, not `NSTimer`:

  ```objc
  if (self.reload_scheduled) return;
  self.reload_scheduled = YES;
  __weak FilesBrowserView *ws = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(WATCH_COALESCE_SEC * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
      FilesBrowserView *s = ws;
      if (!s) return;
      s.reload_scheduled = NO;
      [s reload_directory];
  });
  ```
  (`browser_view.m:663-676`; same shape at 1221-1239 and `main_window.m:534-573`).
  The first version queued a new `dispatch_after` per event — "stacks, not debounced"
  (`CODE_REVIEW.md:137`).
- **Superseding a delayed action**: bump an id and compare in the block
  (`transient_status_id`, `main_window.m:583-596`).
- **Retry with back-off**: recursive `dispatch_after`, delay doubled up to a maximum
  (`watch_pending_unmount:`, 1366-1398).
- **Repeating UI tick**: `NSTimer timerWithTimeInterval:repeats:block:` with a weak
  self, added to the main run loop in `NSRunLoopCommonModes`; invalidated explicitly
  (`copy_progress.m:546-548, 409-414`).

### 3.16 Notifications, observers, teardown

- App-wide custom notifications: an `extern NSString * const` for cross-module
  (`FilesVolumesChangedNotification`), a file-static string for module-internal
  (`bookmarks_changed_note`). Always posted on the main queue; documented as such.
- **Selector-based observers** (`addObserver:selector:name:object:`) for view/controller
  objects, removed with `[NSNotificationCenter.defaultCenter removeObserver:self]` in
  `dealloc`.
- **Block-based observers** (`addObserverForName:object:queue:usingBlock:` with
  `NSOperationQueue.mainQueue` and a weak self) when the notification can arrive on any
  thread (defaults). The returned token is stored in a property and removed explicitly
  (`main_window.m:143-150, 189`; `browser_view.m:422-425, 686-689`). Process-lifetime
  singletons discard the token.
- **Explicit `shut_down`** on views that own watchers, queries or observers, called
  from the window's will-close handler *and* from `dealloc`; idempotent; sets an
  `is_shut_down` flag that later calls check; bumps the generation so in-flight results
  are dropped (`browser_view.m:683-704`). Needed because the app can stay running with
  no windows.
- **Block-observer retain cycle**: a block observer whose block (indirectly) captures
  its own token never dies. Fix from commit ec36e07 — nil the `__block` token when done:

  ```objc
  [NSNotificationCenter.defaultCenter removeObserver:observer];
  observer = nil;  /* the token's block retains finish, which retains observer */
  ```
  (`volume_manager.m:412-420`)
- **C callbacks with an unretained context** (`FSEventStreamContext.info =
  (__bridge void *)self`): the owner must stop/invalidate/release the stream before it
  is deallocated (`browser_view.m:629-657`).
- A process-lifetime shared watcher is created once behind `dispatch_once` and fans out
  by notification, instead of each window polling (`bookmarks_bar.m:91-107`).
- KVO: `addObserver:forKeyPath:options:context:` with a static context pointer;
  `removeObserver:forKeyPath:context:` in `dealloc`; `observeValueForKeyPath:` forwards
  to `super` for other contexts.

### 3.17 Transient status messages

`show_transient_status:` shows a message in the status bar for 4 s, then restores the
regular text; regular updates arriving meanwhile are stored and shown afterwards
(`main_window.m:583-596, 513-519`). Useful in both ports for "Saved", "Deleted",
"Copied" confirmations without alerts.

---

## 4. Helpers worth copying (copy, not link — rename the prefix)

Files must not be touched and the new apps are separate repos, so these are copied and
renamed (`files_` → `tasks_` / `notes_`, `Files…` → `Tasks…` / `Notes…`).

### 4.1 Copy as-is (generic)

| Helper | Signature | Location | Depends on | Notes |
|---|---|---|---|---|
| Register defaults | `void files_register_defaults(void);` | `util.h:36`, `util.m:27-35` | pref-key constants | Replace the dictionary with the app's non-NO defaults. Call first in `applicationWillFinishLaunching:`. |
| Pref-key pattern | `extern NSString * const files_pref_…;` | `util.h:12-27`, `util.m:9-21` | — | Pattern, not content. |
| Byte formatting | `NSString *files_format_bytes(int64_t bytes);` | `util.m:351-361` | Foundation | Shared `NSByteCountFormatter`, file style (base 1000). Notes app (attachment sizes) may want it. |
| Rate formatting | `NSString *files_format_rate(double bytes_per_sec);` | `util.m:367-372` | `files_format_bytes` | Only if a transfer UI exists. |
| Date formatting | `NSString *files_format_date(NSDate *date, NSDateFormatterStyle date_style);` | `util.m:378-394` | Foundation | Two cached formatters (short/medium date + short time); nil → "—". Both ports need this. |
| App icon | `NSImage *files_app_icon(void);` | `util.m:400-409` | image in `Resources` | `imageNamed:` once. |
| Alert factory | `NSAlert *files_new_alert(void);` | `util.m:415-422` | `files_app_icon` | Every alert goes through it. |
| Present alert | `void files_present_alert(NSAlert *alert, NSWindow *window, void (^handler)(NSModalResponse resp));` | `util.m:428-437` | AppKit | Sheet or app-modal. |
| Error alert | `void files_show_error(NSString *title, NSError *err, NSWindow *window);` | `util.m:443-450` | the two above | The only way errors are shown. |
| Cancel token | `@interface FilesCancelToken : NSObject`<br>`- (const atomic_int *)cancel_flag;`<br>`- (void)cancel;` | `util.h:290-304`, `util.m:452-474` | `<stdatomic.h>` | For any cancellable background job. |
| Run a tool | `BOOL files_run_tool(NSString *tool, NSArray<NSString *> *args, double timeout, NSData **out);`<br>`#define FILES_TOOL_TIMEOUT 15.0` | `util.h:121-142`, `util.m:147-169` | Foundation | Only if the port shells out. |
| Background queue | `dispatch_queue_t files_background_queue(void);` | `util.m:175-185` | GCD | One serial utility queue; rename the label. |
| Path equality | `BOOL files_same_path(NSURL *a, NSURL *b);` | `util.m:75-80` | Foundation | If the port compares file URLs. |
| Home / standard dirs | `NSURL *files_home_url(void);`<br>`NSURL *files_directory_url(NSSearchPathDirectory dir);` | `util.m:41-57` | Foundation | Notes app's data folder lookup (`NSApplicationSupportDirectory`). Note the Applications→local-domain special case. |

### 4.2 Copy and adapt (file-static factories — small, self-contained)

| Helper | Signature | Location |
|---|---|---|
| Menu item | `static NSMenuItem *make_item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mod)` | `app_delegate.m:532-540` |
| Pref checkbox menu item + `toggle_pref:` + `validateMenuItem:` | `static NSMenuItem *make_pref_item(NSString *title, NSString *key, id target)` | `app_delegate.m:554-603` |
| Shared option list for menu + prefs | `NSArray<NSArray<NSArray<NSString *> *> *> *files_view_pref_groups(void);` | `prefs_window.h:26`, `prefs_window.m:12-24` |
| Toolbar image item | `static NSToolbarItem *button_item(NSToolbarItemIdentifier ident, NSString *label, NSString *symbol, SEL action, id target)` | `main_window.m:847-859` |
| Prefs heading / note | `static NSTextField *section_label(NSString *title)`; `static NSTextField *note_label(NSString *text)` | `prefs_window.m:34-59` |
| Form key/value labels | `static NSTextField *make_label(NSString *text)`; `static NSTextField *make_value(void)` | `info_panel.m:32-56` |
| Link-style button | `static NSButton *make_copy_btn(id target, SEL action, NSInteger tag)` | `info_panel.m:70-89` |
| Table column | `static NSTableColumn *make_col(NSString *ident, NSString *title, CGFloat width, BOOL resizable)` | `mounts_panel.m:75-85` |
| About credits with links | `static NSAttributedString *about_credits(void)` + `show_about:` | `app_delegate.m:448-504` |
| NSError wrappers | `static NSError *posix_error(int code, NSURL *url)`; `static NSError *described_error(NSString *text)` | `file_ops.m:95-100, 176-181` |
| Timestamped log line | `static NSString *stamped(NSString *line)` | `copy_progress.m:261-271` |

### 4.3 Whole classes worth mirroring

| Class | File | Reuse |
|---|---|---|
| `FilesPrefsWindow` | `prefs_window.{h,m}` (270 lines) | Copy nearly verbatim; drop the Integration section and `default_viewer` import. |
| `FilesBookmarksScrollView` | `bookmarks_bar.m:118-147` | Any horizontal strip (tag chips, tab strip). Self-contained; needs `wheel_line_w`. |
| `FilesSpeedGraph` | `speed_graph.{h,m}` | Only as a template for a `drawRect:` chart; depends on `files_format_rate`. |
| `FilesInfoPanel` layout code | `info_panel.m:195-371` | Template for any inspector/detail form: grid + stacks + `fit_window`. Strip the file-specific rows. |
| `FilesCopyProgress` skeleton | `copy_progress.m` | Template for a self-retaining progress panel with lock + timer; only if a port has long operations (Notes import/export). |
| App delegate skeleton | `app_delegate.m:38-90, 283-435, 705-824` | Window list, front-window lookup, reopen handling, menu bar. |
| Main window skeleton | `main_window.m:105-342, 677-738, 797-1005, 1473-1483` | Window, toolbar, split controllers, status bar, slide toggles. |
| Makefile | whole file | Change `APP`, frameworks, the custom plist key name; add `-lsqlite3` for Notes. |
| Info.plist | whole file | Change names/ids; drop `CFBundleDocumentTypes` and the Apple Events string unless needed. |

### 4.4 Do not copy (file-manager specific)

`files_icon_for_url` (also pulls in `git_info.h` — `util.m:2, 122`), `files_kind_text`,
`files_opens_with_text`, `files_icloud_drive_url`, `files_has_full_disk_access`,
`files_sync_prefs` / `files_read_prefs` (other apps' preference domains), all of
`file_ops`, `file_hash`, `git_info`, `volume_manager`, `mounts_panel`, `default_viewer`,
`info_sidebar` (Quick Look), the FSEvents watcher.

---

## 5. Lessons learned / pitfalls ("we got this wrong first")

Sources: `CODE_REVIEW.md` (CR), commit history (hash), code comments (file:line).

### Build and process

1. **No header dependency tracking let a broken build ship.** A call to a selector that
   no longer existed linked only because a stale `.o` was reused; a drop would have
   crashed at run time. Fix: `-MMD -MP`, `-include $(DEPS)`, objects depend on the
   Makefile (CR B1, M1; db1dc02).
2. **`-Werror=unguarded-availability`** — but clang cannot check messages to `__kindof`
   receivers (`NSApp`). `[NSApp activate]` is macOS 14+ and crashed on 13 (CR B8).
   Keep `-mmacosx-version-min`, `LSMinimumSystemVersion` and the docs identical and
   review `NSApp` calls by hand (`BUILD.md:99-102`).
3. **`cp -r App.app /dest/App.app` onto an existing bundle nests the new one inside
   it**, so the installed binary never updates. `rm -rf` first (CR §5;
   `Makefile:93-94`).
4. **`make install` must handle a running copy**: terminate, wait, replace, reopen
   (`Makefile:103-121`).
5. **Ad-hoc signatures lose privacy (TCC) grants on every rebuild**; sign with a stable
   identity and re-sign only when the identity changes (6ef381d; `Makefile:12-36`).
6. `codesign` rewrites the binary — `touch` the bundle afterwards or make rebuilds it
   every time (`Makefile:73`).
7. **`$(wildcard src/*.m)` compiles dead files.** 406 lines of an unused module were
   being linked (CR §1). Delete, do not orphan.
8. Recreate `Contents/Resources` from scratch so deleted resources do not linger
   (`Makefile:65`).
9. Do not blanket-disable warnings: `-Wno-deprecated-declarations` hid real
   deprecations. Use a local `#pragma clang diagnostic` around the one deliberate use
   (`file_hash.m:38-41, 62`). `-Wno-unused-parameter` is the one accepted exception and
   is documented in the Makefile and BUILD.md.
10. `VERSION := $(shell …)` (evaluate once); goals that do not sign must not query the
    keychain (cb29a3d).
11. **Commit messages typed through a shell must not contain unquoted backticks.**
    Commit 9e83fa3's message contains ~70 lines of `hdiutil info` output where the
    text `` `hdiutil info` `` was command-substituted. Use `git commit -F file` or a
    quoted heredoc.
12. Give commits descriptive messages: c2bac09 ("good to go") changes 26 files,
    +1,835/−502 lines, including several of the fixes below, and cannot be searched.

### Menus, responder chain, focus

13. **Edit-menu items must use the standard selectors** (`cut:`, `copy:`, `paste:`,
    `selectAll:`, `undo:`, `redo:`) with nil target. Custom selectors broke ⌘C/⌘V/⌘X/⌘A
    in every text field, and ⌘C in a rename field copied *files* (CR B9; db1dc02).
14. **A bare key must never be destructive.** Plain Delete trashed the selection; the
    comment said "without modifiers" but modifiers were never checked. Destructive
    commands are menu key equivalents with ⌘ (CR B10).
15. Always mask and test `event.modifierFlags` when handling a key in `keyDown:`
    (`browser_view.m:2058-2061`).
16. **⌘F via `performFindPanelAction:` is intercepted by a focused text field**; use a
    custom selector on the window controller (6ef381d; `main_window.m:1176-1180`).
17. **Commands that must work when a toolbar field has focus belong on the window
    controller**, which forwards to the active view (9158896;
    `main_window.m:1108-1131`).
18. **AppKit retitles "Preferences…" to "Settings…"** for an app-menu item with ⌘,;
    restore the title in `validateMenuItem:` if the old wording is wanted (989b8ec).
19. **Set menu checkmarks in `validateMenuItem:`** from the source of truth, not when
    toggling — otherwise changes made elsewhere (another window, Preferences, code) do
    not show (b2c59cc).
20. **Rebuild dynamic menus in `menuNeedsUpdate:`** instead of observing and eagerly
    rebuilding (CR §3).
21. **AppKit does not change the selection on right-click**; do it in the context
    menu's `menuNeedsUpdate:` from `clickedRow` (0541ef7; `browser_view.m:1869-1890`).
22. **Set the initial first responder on the next run-loop pass**; done synchronously
    it is overridden by the window's own first-responder pass
    (`main_window.m:158-163`).
23. **A panel whose first control is a text field opens with all text highlighted**;
    `makeFirstResponder:nil` after showing (`info_panel.m:386-387`).
24. **After Return in a field AppKit re-selects all the text**; handle
    `insertNewline:` in `control:textView:doCommandBySelector:` and drop focus
    (9158896; `info_panel.m:736-762`).
25. A "double-fire guard" flag around a toggle was a workaround for the wrong control
    type (`NSButtonTypePushOnPushOff` in a custom toolbar view). Find the real cause;
    the real fix was standard toolbar items + `validateToolbarItem:` (CR §1 dead code;
    afa8251).

### Text fields and focus rings

26. **Do not fake a bezeled field** with `NSBox` + borderless field + custom
    vertically-centring cell. It needed `drawingRectForBounds:`, `editWithFrame:…` and
    `selectWithFrame:…` overrides plus a `_placing_editor` flag to stop the text
    jumping 2 pt when the field editor appeared — and was then deleted for a stock
    `NSTextFieldRoundedBezel` field (9158896 → 9e83fa3).
27. **Measure bezeled controls by alignment rect, not frame** — the frame is wider.
    A 2 pt error made the toolbar overflow when the search field took focus (d8b0edd;
    `main_window.m:632-636`).
28. **A focused `NSSearchToolbarItem` is slightly wider than an unfocused one**; leave
    no slack error in toolbar width maths, and turn the focus ring off for toolbar
    fields (d8b0edd).
29. **NSToolbar does not stretch a view item and flexible spaces do not make it fill**;
    spare room goes after the last item (fe440ed). See 3.3(e).
30. **The toolbar caches a view item's size when the view is set**; re-assign
    `item.view` to make a new width count immediately, otherwise items stay in the
    overflow menu after the window narrows (`main_window.m:660-664`).
31. Give a one-line path/URL field `usesSingleLineMode`, `cell.wraps = NO`,
    `cell.scrollable = YES`, head truncation — otherwise long text wraps inside the
    toolbar (fe440ed).

### Tables

32. **Never allocate formatters per cell.** A new `NSByteCountFormatter` and
    `NSDateFormatter` per cell, times rows × columns on every reload, was the top
    performance finding. Use `dispatch_once` statics (CR §3 High).
33. **No system calls per cell** (`lstat`, icon lookup). Prefetch with the listing;
    cache icons in an `NSCache` keyed by type (CR §3 Medium).
34. **Auto-fit must not run on every data refresh**, must use the cell's real font, and
    must cap the rows measured (CR §3 High; afa8251).
35. **Round measured text widths up** — "42.3 pt of text does not fit in 42.3 pt, it is
    truncated" (c2bac09; `browser_view.m:1590-1592`).
36. **Include `intercellSpacing` in column padding** — the table subtracts it from the
    column width before laying out the cell (c2bac09; `browser_view.m:1560-1565`).
37. **Fill the visible width after fitting** and reserve the legacy vertical scroller,
    or a spurious horizontal scrollbar appears at launch (db1dc02;
    `browser_view.m:1600-1614`).
38. **Size for content that arrives late** with a representative widest string
    (`browser_view.m:1579-1589`).
39. **`reloadData` keeps selected row numbers, not items** — reselect by identity after
    every reorder/rebuild (c2bac09; `browser_view.m:1124-1150`). Match by full path/id,
    not by display name (the first version matched by name).
40. **Reloading the table destroys a cell that is being edited** — defer every reload
    path while an inline edit is active (0541ef7, 47f1b74).
41. **Edit in the cell's own text field**; a floating overlay field needed three
    coordinate conversions, was misplaced and did not scroll (CR §3; 0541ef7).
42. **Cells built for a specific size need that size in their reuse identifier**
    (366d4c0; `browser_view.m:1761-1764`).
43. **`setIndicatorImage:inTableColumn:` is unreliable with `NSTableViewStylePlain`**
    (`browser_view.m:1457-1461`).
44. **Use `localizedStandardCompare:`** for user-visible name sorting;
    `compare:` with a nil argument can throw; a comparator must not do I/O (CR §3).
45. **One function produces a cell's text**, used by both the cell and the width
    measurement; two copies drifted (CR §1 duplicated).
46. **One reload path per table** — four slightly different reload sequences lost the
    sort order (CR §1; `mounts_panel.m:381-410`).
47. When hiding/showing a column, move its width to/from the flexible column
    (`browser_view.m:527-556`).
48. `rowView.backgroundColor = nil` restores alternating rows; setting a colour for
    "normal" rows breaks them (`mounts_panel.m:622-625`).
49. A header click during a search must sort the search results, not replace them with
    another data set — keep one `all_items` → `listing_rows` → `show_rows` pipeline for
    every source (CR §3; 4b03bf0).

### Split views, layout, animation

50. **Use `NSSplitViewController`/`NSSplitViewItem`, not `NSSplitView` delegate code**,
    for anything collapsible (CR §3; afa8251).
51. Setting split subview frames directly leaves the divider with no resize-cursor
    rect until the next window resize — it cannot be found or grabbed (db1dc02).
52. A split item cannot be collapsed synchronously at construction on a raw
    `NSSplitView` ("NSSplitView has not yet measured its subviews"); with
    `NSSplitViewItem` set `collapsed = YES` before adding.
53. **To animate an item in, add it collapsed and expand on the next run-loop pass**
    (e1f5df1; `main_window.m:728-735`).
54. **To animate an item out, tear down state first and remove the item in the
    animation's completion handler** (e1f5df1).
55. **Guard animated toggles against re-entry** with a flag cleared in the completion
    handler.
56. **Do not reparent a long-lived view between containers**; keep it in a permanent
    container and add/remove the sibling (initial commit comment; afa8251).
57. `holdingPriority` decides which pane absorbs window resizes
    (`main_window.m:314-315`).
58. **Replace frame arithmetic in forms with `NSGridView` + `NSStackView` +
    `fittingSize`** — the info panel's frame maths (and a duplicate blank "Name:" row
    it produced) was removed wholesale (CR §1 dead; 9e83fa3).
59. Hidden grid rows collapse; re-fit the window after toggling and keep the top edge
    fixed (`info_panel.m:356-371, 541-547`).
60. Grid rows of controls with captions below need `rowAlignment = None` +
    `yPlacement = Top`, not baseline alignment (`info_panel.m:268-272`).
61. Labels that may be long need low horizontal compression resistance and a truncation
    mode, or they widen the window (`info_panel.m:47-56`).
62. Wrapping labels need `preferredMaxLayoutWidth` for `fittingSize` to be right
    (`prefs_window.m:54-58`).
63. A window that grows to reveal content should grow away from its title bar: move
    `origin.y` down by the same delta (`copy_progress.m:228-234`,
    `mounts_panel.m:441-447`).
64. Pin "Close/Cancel" to the bottom-right with autoresizing margins so it stays put
    while the window grows (afa8251; `copy_progress.m:163-170`).

### Threads, timing, performance

65. **No synchronous I/O on the main thread** — listing, capacity queries, mount table,
    hashing, tool runs all moved off (CR §3 Medium; 4b03bf0, 9e83fa3).
66. **Every async result needs a staleness check** (generation or token identity) —
    late results from a superseded request overwrote newer state (afa8251, 47f1b74).
67. A stale-request check must compare *request identity*, not the request's argument
    (two requests for the same URL) (CR §4; `info_sidebar.m:201-212`).
68. **A serial queue per view lets one hung request block later ones**; use a
    concurrent global queue + generation (CR optimization pass).
69. **A `dispatch_after` per event is not debouncing** — it stacks. Use a pending flag
    (CR §3 High).
70. **Do not hop to the main queue per progress event** — thousands per second on a
    fast disk. Record under a lock; drain on a timer (CR §3 High; 55230b5).
71. **Timers that must tick during menus/sheets/drags go in `NSRunLoopCommonModes`**
    (c2bac09; `copy_progress.m:548`).
72. **`NSUserDefaultsDidChangeNotification` can arrive on any thread** and fires for
    every default; observe on the main queue and make the handler a cheap no-op when
    nothing relevant changed (989b8ec, 4b03bf0).
73. **Defaults written during window construction look like preference changes** —
    gate the handler until launch sync is done, or you get double work and stacked
    prompts (2299bfc).
74. **Run launch-time checks after the first window has drawn** (next run-loop pass)
    and prompt at most once per run (2299bfc).
75. Resource values are cached per `NSURL` instance — make a fresh URL to read current
    state rather than clearing a shared object's cache (9e83fa3;
    `info_panel.m:456-459`, `main_window.m:550`).
76. External tools need a timeout, stdin at `/dev/null` so they cannot prompt, and
    must not be `/usr/bin/git`-style shims that pop an "install developer tools"
    dialog when absent (2299bfc, 9e83fa3; `git_info.m:9-31`).
77. `NSRunningApplication.terminated` only updates on the main run loop; from a
    background queue probe the pid (`default_viewer.m` quit_finder comment).
78. A block that blocks its thread on a semaphore waiting for UI must assert it is not
    on the main thread (`copy_progress.m:597-598`).
79. Use C11 atomics, not deprecated `OSAtomic*` + `volatile` (CR §3).
80. Keep one background serial queue for the app's own housekeeping rather than one
    per feature (2299bfc).
81. Capture every input a worker needs as locals before dispatching; do not read
    `self.*` properties off the main thread (pattern throughout `reload_directory`).

### Memory and lifetime

82. **Stored blocks capture `self` weakly** — the review found no retain cycles
    because this was done consistently (CR §4).
83. **`NSWindow.windowController` is unsafe-unretained**: a window controller nobody
    else owns must retain itself until `windowWillClose:` (`copy_progress.m:536-541`),
    or be held in an owner's array and removed on the will-close notification
    (`app_delegate.m:393-398`).
84. **A block observer that captures its own token leaks**; nil the token (ec36e07).
85. **`dealloc` is not teardown.** When the app can outlive its windows, stop watchers,
    queries, timers and observers on window close (`shut_down`); make it idempotent and
    make later calls no-ops (4b03bf0, 47f1b74).
86. A `dispatch_after` block that strongly captures a view keeps it alive past
    teardown — capture weakly (CR §4).
87. A C callback with an unretained `self` requires stopping the source before
    dealloc.
88. **Bound every growing buffer**: sample ring with pairwise decimation, log capped at
    1000 lines, history capped at 100, `NSCache.countLimit` (CR §4; afa8251).
    Unchecked `realloc` was also flagged.
89. **Do not request images at points × scale *and* pass the scale** — 4× the pixels on
    Retina (CR §4).
90. Release heavy content (thumbnails, in-flight requests) when its pane is collapsed
    or its window closes (CR §4; `main_window.m:1480-1482`).
91. Cancel background jobs when the thing they were for changes or its window closes
    (CR §4; `info_panel.m:765-775`).
92. Load bundled images once (`dispatch_once`), not on every use (CR §4).
93. Singleton `dealloc`s never run; `initWithCoder:` overrides without nibs are dead;
    availability fallbacks below the deployment target are dead (CR §1).
94. Large `NSTextView` logs: non-contiguous layout, single batched edit, scroll to end
    only when visible (afa8251).

### Appearance and icons

95. For images that must follow light/dark, draw inside an `NSImage` drawing handler —
    it runs under the appearance of the view drawing it. (An earlier version shipped
    separate light and dark `.icns` files and chose with
    `NSAppearance.currentDrawingAppearance bestMatchFromAppearancesWithNames:`; c2bac09
    replaced that with a badge composited over the system icon.)
96. `copy` shared named images before resizing them.
97. macOS 26 puts non-squircle app icons on a grey backdrop in the Dock tile, alerts
    and the About panel; set `applicationIconImage`, `alert.icon` and
    `NSAboutPanelOptionApplicationIcon` explicitly (c2bac09; `app_delegate.m:27-33`).
98. Centre a badge on an icon's *artwork*, not its canvas (icons have empty margins)
    (`git_info.m:139-179`).

### Data handling and state (generalisable beyond files)

99. **Never destroy the old thing before the new thing is complete.** Stage, then
    swap; on failure put the old one back (CR B2, B3, B6).
100. **Never delete after an unverified or failed step**; check the result of every
     removal (CR B4, B5).
101. **Read and write the same attribute through the same lens** — reading with
     `lstat` and writing with `chmod`, or showing "Locked" from one flag and toggling
     another, corrupts state (CR B11, B12).
102. **One formatter, one unit base.** Two byte formatters disagreed (1024 vs 1000)
     (CR §1).
103. **One navigation/load path.** Copies of `navigate_to:` in Back/Forward missed
     steps (stale search results, wrong button state) (CR §1; afa8251).
104. **State shared by several windows must be re-read when another window saves it**,
     or the last writer silently overwrites (4b03bf0; `bookmarks_bar.m:194-206`).
105. **"Migration" code that runs on every launch undoes the user's choice** (a removed
     default item came back each launch). Use a one-time flag key, and only rewrite
     saved state when it actually changed (CR §1; `bookmarks_bar.m:6-9, 208-261`).
106. **Do not trust a cached "last applied" value for external state**; re-read and
     write only differences (26e9d16).
107. A failed/timeout result must not be cached as if it were an answer (47f1b74).
108. Pending "select this when it appears" state must be scoped to the context it was
     created in and cleared on navigation (47f1b74).
109. Refuse to navigate to the place already shown; cap history; do not push duplicates
     (CR §4).
110. Show *why* something is empty (an unreadable source said so in the status bar
     instead of looking empty) (4b03bf0).

### Documentation

111. **Wrong comments are worse than missing ones** — a dozen were found describing
     features that did not exist ("debounced", "indeterminate bar", "springs back to
     last width") (CR §2).
112. Keep README features and BUILD.md's source layout current; both had rotted within
     one development cycle (CR §2).
113. Decide naming exceptions once and write them down (CR §2 Style).

---

## 6. Look-and-feel spec (concrete numbers)

### Main window
| Item | Value | Source |
|---|---|---|
| Initial content size | 940 × 620 | `main_window.m:111` |
| Minimum size | 700 × 450 | `:116` |
| Style | titled, closable, miniaturizable, resizable; `NSWindowToolbarStyleExpanded` | `:108-117` |
| Title | name of what is shown (fallback app name) | `:609` |
| New-window cascade | +24 right, −24 down from front window's top-left | `app_delegate.m:291` |
| Toolbar | icon only, not customisable, SF Symbols, bordered items | `:202-206, 851-857` |
| Toolbar symbols | `chevron.left`, `chevron.right`, `arrow.up`, `arrow.clockwise`, `plus`, `eye`/`eye.slash`, `terminal`, `rectangle.split.2x1`(`.fill`), `sidebar.right` | `:877-910` |
| Toolbar text field | rounded bezel, frame 400 × 22, monospaced 12 regular, no focus ring, min width 200 | `:942-962` |
| Toolbar search field | fixed 220 wide, no focus ring | `:27, 916-921` |
| Secondary strip (bookmarks bar) height | 28 | `:22` |
| Status bar height | 20, plus 1 pt `NSBox` separator above | `:23, 274-277` |
| Status text | system 11, `secondaryLabelColor`, centred; right label right-aligned; 8 pt side insets | `:237-272` |
| Sidebar width | initial 180, minimum 140 | `:24-25` |
| Main pane minimum width | 400 | `:26` |
| Split dividers | `NSSplitViewDividerStyleThin` | `:289, 319` |
| Inactive pane | `alphaValue = 0.45` (active 1.0) | `:500-501` |
| Floating action button | 36 × 36, 14 pt from right/bottom of content area, `NSBezelStyleRegularSquare`, `NSButtonTypeMomentaryLight`, image scaled proportionally down; image 20 × 20 or symbol point size 16 regular | `:330-341, 466-476` |
| Transient status duration | 4.0 s | `:31` |

### Bookmarks / button strip
| Item | Value | Source |
|---|---|---|
| Button style | `NSBezelStyleRecessed`, momentary push-in, system 11, image left | `bookmarks_bar.m:363-369` |
| Icon in button | 14 × 14 | `:358, 360` |
| Button gap / left pad | 4 / 4 | `:10-11` |
| Fixed leading button | 26 wide, 3 pt gap after it | `:12-13` |
| Button height | min(natural, bar − 4), vertically centred | `:389-392` |
| Bottom separator | 1 pt `separatorColor` line at y = 0.5 | `:478-487` |
| Wheel step | 20 pt per notched line | `:110` |

### Tables
| Item | Value | Source |
|---|---|---|
| Style | `NSTableViewStylePlain`, alternating rows, multiple + empty selection | `browser_view.m:437-440` |
| Row height | 20 (large rows 40); mounts panel 22 | `:44, 754`; `mounts_panel.m:179` |
| Row icon | 16 (large 32), 2 pt from leading edge, 4 pt gap to text | `:45, 1789-1793` |
| Cell text | system 12; tabular/permissions column monospaced 11 regular | `:1539-1545` |
| Cell padding | 2 pt leading, 2 pt trailing, centred vertically | `:1794-1801` |
| Truncation | middle; numeric column right-aligned | `:1774-1776` |
| Column widths (w/min) | Name 260/120, Size 80/50, Kind 120/80, Permissions 90/70, Date 160/120, Git 60/40 | `:32-39` |
| Header fit padding | 10 | `:1566` |
| Sort indicator | " ↑" / " ↓" appended to the header title | `:1468` |
| Diagnostic table (mounts) | monospaced 11 everywhere, cell pad 16, header pad 20, min col 40 | `mounts_panel.m:356-377` |
| Low-space row tint | red (1.0, 0.3, 0.2) at 10 % alpha | `mounts_panel.m:623-625` |
| Empty value | "—"; pending value "…" | throughout |

### Inspector panel (Get Info)
| Item | Value | Source |
|---|---|---|
| Width | 360 fixed; height fits content | `info_panel.m:11, 337` |
| Outer margin | 16 | `:13` |
| Key column width | 110, trailing-aligned | `:12, 235-236` |
| Grid spacing | rows 6, columns 8; first-baseline row alignment | `:233-240` |
| Root stack spacing | 10; icon↔name spacing 12 | `:230, 327` |
| Icon | 64 × 64 | `:14` |
| Name field | rounded bezel, bold 13, middle truncation | `:222-226` |
| Key labels | bold `smallSystemFontSize`, `secondaryLabelColor` | `:35-37` |
| Value labels | `smallSystemFontSize`, middle truncation | `:50-52` |
| Section title | bold `systemFontSize` | `:303` |
| Hash/monospace values | monospaced 9 regular, selectable, tail truncation | `:282-287` |
| Link button | borderless, "copy", `linkColor`, system 9 | `:73-82` |
| Popups | monospaced 11; caption below system 9 `tertiaryLabelColor`; stack spacing 2; group spacing 6, fill equally | `:115-131, 263-264` |
| Window | utility panel, floating level, not released when closed | `:198-207` |

### Sidebar (info)
| Item | Value | Source |
|---|---|---|
| Padding | 8 | `info_sidebar.m:6` |
| Preview area height | 150; image scaled proportionally, centred; icon requested at 128 | `:7, 118-119, 254` |
| Name | bold 12, centred, up to 2 lines, middle truncation, 34 high | `:9, 130-136` |
| Metadata rows | 18 high; key column 62 wide, system 10, right-aligned, `secondaryLabelColor`; 4 pt gap; value system 11, tail truncation | `:8-11, 149-163` |
| Separators | `NSBox` separator lines at y 164 and 209; 1 pt left edge in `separatorColor` | `:13-16, 93-100` |
| Background | `windowBackgroundColor` | `:96` |

### Preferences
| Item | Value | Source |
|---|---|---|
| Panel | utility, titled + closable, initial 360 × 300 then sized to fit, centred | `prefs_window.m:96-111` |
| Stack | vertical, leading-aligned, spacing 6, insets top 16 / left 20 / bottom 20 / right 20 | `:193-197` |
| Heading | bold `systemFontSize`; 8 pt after heading; 18 pt between sections | `:38, 199-202` |
| Note text | `smallSystemFontSize`, `secondaryLabelColor`, wraps at 320 | `:6, 54-57` |

### Progress panel
| Item | Value | Source |
|---|---|---|
| Size | 460 × 210 content; side margins 12; top inset 10 | `copy_progress.m:76, 119-125` |
| Status line | system 12, middle truncation, 14 high | `:123-128` |
| Graph | 110 high, 8 pt below status | `:132-138` |
| Disclosure toggle | borderless button "▶  Transfer Log" / "▼  Transfer Log", system 11 | `:145-151, 224-226` |
| Elapsed label | system 11, `secondaryLabelColor`, right-aligned | `:155-160` |
| Button | 88 × 22, 12 pt from bottom and right; Esc = Cancel, Return = Close | `:13, 164-169, 437-440` |
| Log | 160 high, 8 pt gaps, bezel border, monospaced 10, `textBackgroundColor`, text inset 4 × 4 | `:11-12, 174-205` |
| Log cap | 1000 lines, trimmed in batches of 100 | `:17-18` |

### Chart (speed graph)
| Item | Value | Source |
|---|---|---|
| Background | `controlBackgroundColor` | `speed_graph.m:160` |
| Area fill | `systemGreenColor` at 25 % alpha | `:144` |
| Gridlines | 3 (25/50/75 %), `separatorColor`, 0.5 pt, dash 3/3 | `:187, 203-214` |
| Axis labels | system 10, `secondaryLabelColor`, 4 pt padding either side | `:179-186` |
| Average line | 1 pt `secondaryLabelColor`; label system 9, 3 pt from right, 2 pt above/below | `:231-249` |
| Plot border | 0.5 pt `separatorColor`; view border 1 pt white 0.55 | `:254-265` |
| Margins | bottom 4, right 4, top 4, left = label gutter | `:195-199` |
| Scale | max bucket × 1.1, floor 1 MB/s | `:7, 175-176` |

### Other
| Item | Value | Source |
|---|---|---|
| Menu item icons | 16 × 16 | `app_delegate.m:626` |
| Legend swatch | 12 × 12, corner radius 2, 1 pt border | `mounts_panel.m:242-253` |
| Panel bottom button row | 32 high; buttons 84 × 24, rounded bezel | `mounts_panel.m:169, 224-239` |
| About credits | `smallSystemFontSize`, centred, items separated by "  ·  " | `app_delegate.m:451-467` |
| Corner radii | only the swatch (2); everything else is system-drawn | grep |

### Animation and timing
| What | Value | Source |
|---|---|---|
| Sidebar / second-pane slide | `NSSplitViewItem.animator.collapsed`; **no duration or timing function set — AppKit default** | `main_window.m:712, 731, 1477` |
| Panel grows to show log | `setFrame:display:animate:YES` (AppKit-computed duration) | `copy_progress.m:234` |
| Panel shrinks | `NSAnimationContext` `duration = 0.2`, default timing | `copy_progress.m:246-249` |
| Custom timing functions | none anywhere (grep for `timingFunction`, `CAMediaTiming` finds nothing) | — |
| Event coalescing | 0.25 s (file events), 0.2 s (column refresh), 0.3 s (status-bar disk space) | `browser_view.m:54, 1227`; `main_window.m:540` |
| FSEvents latency | 0.3 s (folder), 0.5 s (trash) | `browser_view.m:636`; `bookmarks_bar.m:99` |
| Progress UI tick | 0.25 s | `copy_progress.m:9` |
| Tool timeout | 15 s | `util.h:122` |
| Retry back-off | 2 s doubling to 10 s | `main_window.m:34-35` |

---

## 7. Testing approach

- **One test program**: `tests/file_ops_test.m` (276 lines), a plain `main()` — no
  XCTest, no framework. Built by `make test`: each `tests/*_test.m` is compiled with
  the app's `CFLAGS` and linked against every app object except `main.o`
  (`Makefile:125-133`), then run; exit status non-zero on any failure.
- **Harness is three things** (`tests/file_ops_test.m:12-62`):
  - `static int failures;` and `check(BOOL ok, NSString *what)` which prints
    `ok   <what>` / `FAIL <what>` and counts.
  - A `transfer(...)` wrapper that calls the asynchronous engine and **spins the main
    run loop until the completion block fires**:
    ```objc
    while (!done) {
        [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode
                            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    ```
    — the way to test any API here that completes on the main queue.
  - Small fixtures (`write_file`, `read_file`, `flags_of`, `leftovers`, `log_has`).
- **What it covers**: the copy engine against real files in a scratch folder under
  `NSTemporaryDirectory()` and a small disk image it creates with `hdiutil` (locked
  items, keep-both, replace, duplicate in place, copy-into-self, same- and
  cross-volume moves, hash validation, folder size). It asserts on results on disk and
  on the engine's log lines, and cleans up after itself.
- **What is testable without a GUI** is exactly what was written as C functions or
  class methods with injected callbacks: engines that take blocks for conflict/
  progress/log instead of reaching for UI. That is the design lesson — keep the model
  layer free of AppKit views so it links into a test binary. (The whole app's objects
  are linked, so a test *could* instantiate views, but none does.)
- **UI validation: none is automated.** No screenshots, no accessibility-driven tests,
  no snapshot tests, no XCUITest. Accessibility work in the app is limited to
  `accessibilityDescription:` on SF Symbol images. UI was verified by hand; commit
  messages occasionally say what was checked ("Verified with Full Disk Access: launches
  after a change read back matching values and write nothing", 26e9d16).
- For the ports: the SQLite/model layers of both apps should get `tests/*_test.m`
  programs in this style. If UI checks are wanted they have to be invented — there is
  no precedent in Files to copy.

---

## 8. What not to transfer, and known debt

### Flagged by the reviews as left open or deliberately not done
- Replace-by-`RENAME_SWAP` was not adopted (needs a fallback for volumes without swap)
  (`CODE_REVIEW.md:220-222`).
- One module still writes defaults from a background queue; left because all observers
  hop to main (`CODE_REVIEW.md:260-262`).
- `-Wno-unused-parameter` kept (`CODE_REVIEW.md:282-283`).
- Only the engine has tests.

### Observed while reading (not in the reviews) — do not copy these
1. **No Window menu, no Help menu, no Services / Hide / Hide Others / Show All, no
   Enter Full Screen item, `NSApp.windowsMenu` never set.** A standard Mac app has
   these; the ports should add them. Minimise/Zoom have no menu items (grep finds no
   `performMiniaturize:`/`performZoom:`/`toggleFullScreen:`), so presumably no ⌘M.
2. **Frame-based layout in older modules** (`copy_progress.m build_views`,
   `info_sidebar.m`, `mounts_panel.m build_toolbar_row`, `main_window.m build_layout`)
   — hand-computed rects with autoresizing masks. The newer modules (info panel,
   preferences) use stack/grid views and are what the review pushed towards. Use the
   newer style for forms; frame + autoresizing is fine only for a few big regions.
3. **Three hard-coded colours** that do not adapt to appearance: the chart's outer
   border `[NSColor colorWithWhite:0.55 alpha:1.0]` (`speed_graph.m:264`), and the red
   low-space tint/swatch (`mounts_panel.m:244-251, 624`).
4. **`fit_address_bar` depends on private toolbar view hierarchy**
   (`address_bar.superview.superview`, counting `row.subviews` against
   `toolbar.items`). Works today; an OS update could break it.
5. **Sort direction shown by editing the header title** (" ↑"/" ↓") — a workaround.
   Where the standard `sortDescriptorPrototype` route works (as in the mounts panel),
   use that and get the native indicator.
6. **Emoji as a button title** ("⭐️", `bookmarks_bar.m:289`) while the comment and
   README call it "★"; an SF Symbol image would match the toolbar.
7. **Tag-as-index buttons and menu items** in the bookmarks bar (`btn.tag = i`), with
   the whole strip rebuilt on every change. Fine at this size; use
   `representedObject` for anything bigger.
8. **Only the window frame is persisted**, under one shared autosave name for all
   windows; sidebar visibility, dual-pane state, column widths and sort order reset
   every launch. The task and notes apps almost certainly want selection, sidebar
   width and sort persisted — that needs new code.
9. **No localisation, no `NSLocalizedString`.**
10. **`NSLog` only**, four call sites; no `os_log` subsystem/categories.
11. **App icon is a PNG named in `CFBundleIconFile`**, not an `.icns`, with run-time
    workarounds for the macOS 26 backdrop; `CFBundleSignature` is `????`. A properly
    shaped icon asset would remove the workarounds.
12. **No hardened runtime, entitlements or notarisation** — fine for a locally built
    personal app, not for distribution.
13. **Singletons' block observers are never removed** (`prefs_window.m:115-120`,
    `info_panel.m:346-351`). Correct only because they live for the whole process; do
    not copy that into per-window objects.
14. **Two styles of private state** (property-based class extension vs ivar block)
    coexist. Pick by the rule in 1.5 rather than at random.
15. **Comment drift still present** despite the review:
    - `info_sidebar.h:4` and `info_sidebar.m:72` say the sidebar is 220 pt wide; it is
      created at 180 (`main_window.m:24, 299-300`).
    - `browser_view.m:2027-2028` says the view takes focus "for its key handling
      (rename shortcuts)"; Return no longer renames (d5443b1).
    - `util.h:4-8`'s module blurb lists far less than the header now contains.
    - `mounts_panel.m` comment blocks carry "Output: none." lines that no other module
      uses.
    - `BUILD.md:69` lists resources as "folder.png, documents.png, ribbon.png,
      git96.png ← git badge" on one line; `ribbon.png` is bundled but unreferenced by
      any source (grep) — the review had flagged it as an unused asset, it was removed
      and then re-added "for an upcoming feature" (db55e74).
16. **`util.m` imports `git_info.h`** — the "generic helpers" module depends on a
    feature module for the icon badge. Keep the ports' util free of feature imports.
17. **`FilesFileOps` as an all-class-method class** — older style; newer modules use
    `files_*` C functions.
18. **`README.md` title is still `# files`** (lower case) after the rename to Files.
19. **Uncommitted work in the tree** (default-viewer changes) — the repo's HEAD is not
    what is running; irrelevant to patterns but worth knowing.

### File-manager specifics that simply do not apply
Copy engine and staging, FSEvents watchers, Spotlight queries, Quick Look panel and
thumbnails, volume mount/unmount, `git` integration, other apps' preference domains,
Full Disk Access checks, login item (`SMAppService`), `CFBundleDocumentTypes`,
`application:openURLs:` reveal handling, Apple Events to Finder.

### Things the ports need that Files gives no precedent for
`NSOutlineView` / source-list sidebars, `NSCollectionView`, rich `NSTextView` editing
(Files only has a read-only log view), popovers, date pickers, undo management
(`undo:`/`redo:` menu items exist but nothing registers undo actions outside text
fields), custom row views / selection drawing, drag-reordering rows within a table,
window state restoration, SQLite access, search within a text document.

---

## Unverified / could not confirm

- **Nothing was built or run** (the task forbade it). `make`, `make test`, and all
  runtime behaviour are described from source, comments and commit messages, not
  observed. Visual results (how the focus ring, slides and fits actually look) were
  not seen.
- **Default duration and easing of the `NSSplitViewItem` collapse animation** are not
  specified in Files; they are whatever AppKit uses. Not measured.
- **Duration of `setFrame:display:animate:YES`** is AppKit-computed
  (`animationResizeTime:`); not measured.
- Whether the Get Info name field and the inline-rename field visibly show the default
  focus ring is inferred from the absence of any `focusRingType` override on them, not
  observed.
- Commit c2bac09 ("good to go") is large and unlabelled; lessons attributed to it were
  identified from its diff, and some smaller changes in it may have been missed.
- The claim that `fit_address_bar` is fragile is my assessment; the code reviews do not
  say so.
- `default_viewer.m` (≈1,000 lines) was read at the head, tail and helper sections and
  outlined in between, not line by line; it is file-manager specific and no pattern
  above depends on its unread middle.
