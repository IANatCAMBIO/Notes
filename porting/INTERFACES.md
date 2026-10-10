# Notes AppKit — the contract for the UI work packages

This is what the seven UI engineers (packages B–H) build on.  Package A
(core and platform) is done: the core compiles and runs with no GTK, the
app launches as a skeleton, and every UI module exists as a header plus a
stub.  **You replace your own stub files and nothing else; the app builds,
runs and passes `make test` at every commit.**

Read in this order: this file; `porting/GTK_PORTING_MAP.md` (what the GTK
app does, section by section); `porting/FILES_PLAYBOOK.md` §1, §3, §5 (how
AppKit code is written here); then your package's GTK source in
`porting/gtk_reference/src/` — that code is the behavioural specification
and is deleted when the port is integrated.

Contents: 1 hard rules · 2 layout of the tree · 3 the core you program
against · 4 frozen headers · 5 packages, ownership, start order · 6 commands
and the keyboard map · 7 threading · 8 the byte-offset rule · 9 memory and
ownership · 10 data and test data · 11 code style · 12 verification ·
13 deviations from the map's §10.6 · 14 known hazards.

---------------------------------------------------------------------------

## 1. Hard rules

1. **Never touch the owner's live data — not even to read it.**  The GTK
   Notes app is installed and probably running on the real database.  Off
   limits: `~/Library/Mobile Documents/com~apple~CloudDocs/Notes/`,
   `~/.local/share/notes/`, `~/.config/notes/`, `~/.cache/notes-*.sock`,
   `~/Applications/Notes.app`, and the sibling repositories `../notes`,
   `../tasks`, `../files` (read-only references; never build or run there).
   Never run a GTK `notes` binary except through `tests/cli_golden.sh`,
   which sandboxes it.
2. **Every run uses test data.**  `make run`, `make seed`, `make test` set
   `NOTES_DATA_DIR` to a folder under `dev/`.  If you start the app or the
   tool by hand, set it yourself:
   `NOTES_DATA_DIR=$PWD/dev/data NotesAppKit.app/Contents/MacOS/notes stats`
   (the last line of `stats` prints the database it opened — look at it).
   Without the variable this build uses its own real directory,
   `~/Library/Application Support/NotesAppKit/`; it cannot reach the GTK
   build's files either way (§10).
3. **`document.c`, `bnbf.c`, `db.c` are not yours to change.**  The document
   model is the single mutation path of a note (`BLOCK_MODEL.md`).  If you
   think the model needs a change, raise it.
4. **The CLI's output is a contract** (a Claude skill drives it).
   `make golden` compares it byte for byte with the GTK build's; it must
   stay green.
5. **No new GLib** in new code.  GLib stays in the carried core for this
   phase and is removed later.  Objective-C and new C use Foundation and
   plain C; you will still *call* GLib-typed core functions (`gchar *`,
   `GList *`, `g_free`) — that is fine, adding GLib containers or main-loop
   calls to UI code is not.  **Nothing pumps a GLib main loop**: `g_idle_add`,
   `g_timeout_add`, `g_thread` hand-backs never fire.  Use `main_service.h`.
6. Work on branch `appkit`; no remotes, no pushes.

---------------------------------------------------------------------------

## 2. Layout of the tree

```
src/                 flat; one module = one .h + one .c or .m
  identity.h         every name of this build (bundle id, data dir, socket)
  -- core (C, no UI; linked into the app, the `notes` tool and the tests) --
  document.[ch] bnbf.[ch]   the block model and its blob format   (carried)
  db.[ch] serialize.[ch] export.[ch] search_query.[ch] cli.[ch]   (carried)
  backup.[ch]               rotating backups, on main_service
  config.[ch]               the data-directory policy + notes.ini
  main_service.[ch]         main-thread posts, timers, workers (GCD)
  image_io.[ch]             the one decoded image type: CGImageRef
  ipc.[ch]                  CLI <-> app socket (BSD sockets)
  app_core.[ch]             OnApp: state, preferences, the hooks
  app_lifecycle.[ch]        open the database, start services, shut down
  action_edit.[ch]          edit an action item from outside its note
  search_job.[ch]           a search on a worker
  utf_index.[ch]            UTF-8 byte <-> UTF-16 unit offsets
  cli_main.c                the `notes` tool's main()
  -- UI (C and Objective-C; the app only) --
  look.h  paint.[ch]  text_layout.[ch]  doc_layout.[ch]          (B)
  note_edit.[ch]  note_view.[hm]                                 (C)
  editor_window.[hm]  image_viewer.[hm]                          (D)
  main.m  app_delegate.[hm]  commands.[hm]  library_state.[hm]
  library_window.[hm]  library_commands.[hm]  ui_util.[hm]       (E; A wrote them)
  library_sidebar.[hm]                                           (F)
  library_notes.[hm]                                             (G)
  settings_window.[hm] search_window.[hm] media_window.[hm] library_ai.[hm]  (H)
tests/               *_test.c (core), *_test.m (UI), cli_golden.sh, check.h, ui/*.txt
tools/               dev_seed.c, seed-dev.sh, bnbf-scan.c, …
resources/           Info.plist
icons/               PNG artwork, copied into Contents/Resources
porting/             this file, the two research documents, gtk_reference/
dev/                 (gitignored) data/, test/, snapshots/, golden/, gtk_ref/
```

The Makefile compiles **every** `src/*.c` and `src/*.m` (a file you add is
built; a file you orphan is still built — delete it).  A `.c` file listed in
`CORE_SRCS` is core; any other `.c` is UI.

---------------------------------------------------------------------------

## 3. The core you program against

Everything in map §2.3 is still there with the same signatures
(`document.h`, `db.h`, `serialize.h`, `export.h`, `backup.h`,
`search_query.h`).  What changed:

| Was (GTK) | Is now |
|---|---|
| `app.h` (GTK types in `OnApp`) | `app_core.h`: `OnApp` = db, db_dir, preference fields, backup state, **hooks** |
| `on_app_config_init(argv0)`, ini beside the binary, `notes.ini.defaults` | `config.h`: `on_app_config_init()`, one data directory; no defaults file — defaults are the table in `app_core.c` |
| `g_idle_add` / `g_timeout_add` / `g_thread_new` | `main_service.h`: `on_main_post`, `on_timer_add` / `on_timer_remove`, `on_worker_run` |
| `GdkPixbuf`, `on_png_decode_capped`, `on_image_png_bytes`, `on_png_probe_size`, `gdk_pixbuf_new_from_file` | `image_io.h`: `on_image_decode_capped` → `CGImageRef` (+1), `on_image_png_encode`, `on_image_probe_size`, `on_image_file_to_png`; `on_note_image_nth` returns a `CGImageRef` (+1) |
| `on_editor_action_set_done/due/text` (editor_window.c) | `action_edit.h`: `on_action_edit_set_done/due/text` |
| the search worker inside search_window.c, `OnSearchScope` | `search_job.h`: `on_search_job_start` / `_cancel`, `OnSearchScope` |
| main.c's sequence, `on_app_switch_database`, quit by destroying windows | `app_lifecycle.h`: `on_app_db_path`, `on_app_open_db`, `on_app_start_services`, `on_app_switch_db`, `on_app_shutdown` |
| `on_library_quicknote`, `on_editor_window_open` called from ipc.c | hooks `OnApp.quicknote`, `OnApp.open_note` |
| `app->editors` hash table, `app->editors == NULL` = headless | the editor registry is private to `editor_window.m`; **headless = every hook NULL** |

### The hooks (`OnApp`, `app_core.h`)

The core never includes a UI header.  What it needs from the UI it calls
through function pointers in `OnApp`, each installed once at startup by the
module that owns it and never cleared:

| Hook | Installed by | Called by | Meaning |
|---|---|---|---|
| `notify_notes_changed(app)` | library_window.m | ipc.c after a mutating command; UI after structural changes | full refresh: sidebar + notes pane |
| `notify_note_saved(app, id)` | library_window.m | the editor after a plain save | update that one row |
| `notify_status(app, msg)` | library_window.m | `on_app_status()` (printf-style) | status-bar event message |
| `notify_ai_changed(app)` | library_ai.m (H) | Settings | `ai_enabled` changed |
| `notify_db_health(app)` | settings_window.m (H), while open | `on_app_db_health_start`, its completion | repaint the health plate |
| `show_notice(app, title, msg)` | app_delegate.m | the integrity check, on damage | modal message |
| `open_note(app, id)` | editor_window.m | ipc.c (`notes note open`) | open/raise the editor |
| `quicknote(app)` | library_commands.m | ipc.c (`notes quicknote`) | new root note, editor in front |
| `live_document(app, id)` / `live_edited(app, id)` | editor_window.m | action_edit.c | edit an OPEN note's document |
| `flush_editors(app)` | editor_window.m | `on_app_shutdown`, `on_app_switch_db` | close every editor, each saving first |

A hook is handed only the `OnApp`; UI state is reached through
`on_library()` (library_state.h) or a module's own file-static.

### The flush-on-quit contract (`app_lifecycle.h`)

An editor holds unsaved edits for up to its autosave delay.  Nothing may end
the process, or close or switch the database, while one does.  So:
`on_app_shutdown()` is the one way out (stop the socket → `flush_editors` →
remove the backup timer → close the database); the app delegate calls it
from `applicationShouldTerminate:`; SIGTERM is turned into `[NSApp
terminate:]`; `flush_editors` closes every editor **synchronously**, and an
editor's `windowWillClose:` saves a dirty note or deletes a blank one before
it returns.  If you add a window that holds unsaved state, it must be
flushed by this path too.  Never call `exit()` with windows open.

---------------------------------------------------------------------------

## 4. Frozen headers

"Frozen" means other packages are written against it: do not change a
signature or a meaning without agreeing it with the packages named.  Adding
a function is fine.  Headers marked *owner may extend* are internal seams of
one package.

| Header | State of the `.c`/`.m` | Owner | Used by |
|---|---|---|---|
| `identity.h` | real | A | everyone |
| `app_core.h`, `app_lifecycle.h`, `config.h`, `main_service.h`, `image_io.h`, `ipc.h`, `action_edit.h`, `search_job.h`, `utf_index.h` | real, tested | A | everyone |
| `look.h` | real (constants) | A; add entries for your package | everyone |
| `ui_util.h` | real | A | everyone |
| `commands.h` | real (table + dispatch) | E owns the table | everyone |
| `paint.h` | real | B (*owner may extend*) | B |
| `text_layout.h` | **stub** | B (*owner may extend*; byte-offset rule frozen) | B |
| `doc_layout.h` | **stub** | B | C |
| `note_edit.h` | real but minimal | C (*owner may extend*) | C, D |
| `note_view.h` | **stub** (shows plain text) | C | D |
| `editor_window.h` | **working stub** (registry, load, autosave, close, flush are real) | D | E, F, G, H |
| `image_viewer.h` | **stub** (never opens) | D | D, H |
| `library_state.h` | real | E | E, F, G, H |
| `library_window.h` | working skeleton | E | E |
| `library_commands.h` | New Note, Quick Note, the two trash functions real; the rest to write | E | F, G |
| `library_sidebar.h` | **stub** (shows counts) | F | E, G, H |
| `library_notes.h` | **stub** (lists titles) | G | E, H |
| `settings_window.h`, `search_window.h`, `media_window.h`, `library_ai.h` | **stubs** (register nothing) | H | E |

Each header's file comment states its contract and its STATUS; each stub
`.m` names the GTK file that specifies the behaviour.

### How the UI modules fit together

- **API style.**  A module's public API is C functions named
  `on_<module>_…`, declared in its header.  A class appears in a header only
  when another module must hold a typed reference (`NotesNoteView`,
  `NotesImageViewer`, `NotesLibrary`).  Callbacks between modules are `copy`
  block properties named `…_handler` (note view, image viewer), never
  delegates or protocols.
- **One install per module.**  The app delegate calls, once, in
  `open_database_and_start`: `on_editor_install`, `on_library_window_install`,
  `on_library_commands_install`, `on_library_notes_install`,
  `on_settings_install`, `on_search_install`, `on_media_install`,
  `on_library_ai_install`.  Your install function registers your commands
  (§6) and sets your hooks.  You never edit another package's file to get
  wired in.
- **The library is one shared state object**, `NotesLibrary`
  (`library_state.h`): the sidebar selection (`sel_kind`, `sel_id`,
  `sel_name`), `grid_pref`, `populating`, the window (weak).  It exists from
  launch to quit even while the library window is closed.  The sidebar
  writes the selection; everyone else reads it.  `on_library_notes_for_selection`
  is THE mapping from a selection to notes.
- **Refresh rules**: structural change → `on_library_refresh_all`; one note
  saved → `on_library_notes_note_saved`; while `populating > 0`, selection
  handlers do nothing.
- **Rows.**  `NSOutlineView`/`NSTableView` items must be objects, so each
  data-source owner defines its own small record classes privately (F: the
  sidebar row; G: the note row and the action row).  Identity is the note id
  / uid / (kind, id) — reselect by identity after every reload.
- **Drags** between the notes pane (G, source), the sidebar (F, source and
  the only drop target): `on_library_drag_item` / `on_library_drag_read`
  with pasteboard type `on_pasteboard_type_rows`.  Drop actions call
  `on_library_trash_notes` / `on_library_trash_folder` (library_commands.h)
  and the `on_db_*` move functions, then `on_library_refresh_all`.
- **Editors** are opened with `on_editor_window_open(app, id, reveal,
  ordinal, search_term)`.  Before trashing or deleting notes call
  `on_editor_close_notes` (the two trash functions already do).  After a
  preference an editor renders from changes: `on_editor_settings_changed`.
- **The image viewer** is hosted by the editor window (over its scroll view)
  and by the media window; the host sets four handler blocks and forwards
  keys with `on_image_viewer_key_down`.

---------------------------------------------------------------------------

## 5. Packages, file ownership, start order

| Pkg | Scope | Files you own (replace / complete) | Specification (porting/gtk_reference/src) | Needs |
|---|---|---|---|---|
| **B** Text layout | CoreText backend; doc_layout in neutral form; painter | `text_layout.[ch]`, `doc_layout.[ch]`, `paint.[ch]` | `doc_layout.c` | image_io, look |
| **C** Note view | the editor view, the lifted editing policy, the probe | `note_view.[hm]`, `note_edit.[ch]`, new `tests/ui_probe_test.m` (+ `tests/ui/*.txt`) | `note_view.c`, `tests/ui_probe.c` | doc_layout.h (stub is enough to start) |
| **D** Editor window + image viewer | toolbar, find field, status bar, placement, viewer hosting; the viewer | `editor_window.[hm]`, `image_viewer.[hm]` | `editor_window.c`, `image_viewer.c` | note_view.h (stub is enough) |
| **E** App + library shell + commands | launch/quit, menus, library frame, toolbar, status bar, every command with no view of its own | `main.m`, `app_delegate.[hm]`, `commands.[hm]`, `library_state.[hm]`, `library_window.[hm]`, `library_commands.[hm]` (+ `ui_util` additions) | `main.c`, `library_window.c` 2111–3489, 3651–3749, 5047–5416, 6074–6197 | nothing |
| **F** Sidebar | outline, counts, expansion + selection restore, fit-to-content, context menu, drop target, folder drag source | `library_sidebar.[hm]` | `library_window.c` 403–820, 1339–1386, 1702–2109, 2994–3134, 5456–5663 | library_state.h |
| **G** Notes + Action Items | list + grid with one selection, sorting, column layout, density, thumbnails, note menu, note drag source, one-row update; Action Items list, due-date sheet | `library_notes.[hm]` (+ your own new files, e.g. `library_actions.[hm]`, `library_columns.[hm]`) | `library_window.c` 822–1337, 1406–1700, 3491–3649, 4313–5045, 5665–5849; `list_rows.[ch]` | library_state.h, action_edit.h |
| **H** Secondary windows | Settings, Search, Media, AI summary | `settings_window.[hm]`, `search_window.[hm]`, `media_window.[hm]`, `library_ai.[hm]` | `settings_window.c`, `search_window.c`, `media_window.c`, `library_window.c` 3751–4155 | library_state.h, search_job.h, image_viewer.h |

**Start order.**  Everyone can start at once: every dependency exists as a
header with a stub behind it.  The critical path is **B → C**: the editor is
not usable until both land, so start them first and keep `doc_layout.h`
stable.  C can write `note_edit.c` and the input handling against the stub
layout and switch when B lands.  D works against the stub view (it already
loads, saves and flushes).  E, F, G work against the working editor stub.  H
needs D's viewer only for the media window's click-to-enlarge; the rest of H
is independent.

**Shared files.**  Three files are touched by more than one package, by
appending only: `look.h` (add your constants, in your section),
`commands.m` (E owns the table: ask E for a new row; do not edit rows),
`ui_util.[hm]` (propose additions to E).  `app_delegate.m` is E's; a new
module's install call is the one line another package may ask E to add.

---------------------------------------------------------------------------

## 6. Commands and the keyboard map

`commands.h` is the one table of commands.  Menu items, toolbar items and
context-menu items are made from it (`on_command_menu_item`,
`on_command_toolbar_item`); nobody hand-builds an item for a command or
writes a second dispatch scheme.

- **App scope** — works from any window, acts on the library.  Items target
  the app delegate (`run_app_command:`), which calls
  `on_library_run_command`.
- **Window scope** — items have no target; AppKit sends `run_command:` up
  the responder chain to the key window's controller.  The library
  controller calls `on_library_run_command`; an editor controller calls
  `on_command_run` on its own table.  Controllers implement
  `validateUserInterfaceItem:` with `on_library_validate_command` /
  `on_command_validate`.
- **To implement a command**: write `static void run_x(id target, id
  sender)` (and optionally `static BOOL validate_x(id target, id item)`),
  add `{ @"ident", run_x, validate_x }` to your module's static
  `on_command_handler_t` table, and register the table in your install
  function with `on_library_add_commands` (library-target commands: the
  target is the `NotesLibrary`).  Editor commands (D) go in a table private
  to `editor_window.m`; their target is the editor's window controller.
- **A command with no registered handler is disabled.**  That is why most of
  the skeleton's menu is grey; your items light up when you register.
- **State on an item** (checkmark, "Hide"/"Show" wording, the list/grid
  icon): call `on_command_item_update(item, title, icon, checked)` from your
  validate function — never when toggling, or state changed elsewhere does
  not show.
- **Context menus**: build the `NSMenu` from the same identifiers; in
  `menuNeedsUpdate:` first select the clicked row (AppKit does not), so the
  command acts on the selection.
- **Edit ▸ Undo / Redo / Cut / Copy / Paste / Select All are not commands.**
  They use AppKit's selectors (`undo:`, `redo:`, `cut:`, `copy:`, `paste:`,
  `selectAll:`) with no target, so they work in every text field.  The note
  view implements those same selectors for the note (and
  `validateUserInterfaceItem:` for them).
- **A bare key is never destructive**; destructive commands are menu key
  equivalents with ⌘.  In `keyDown:` always mask and test the modifiers.

### Keyboard map (the `key`/`mods` columns of the table in commands.m)

| Shortcut | Command | Scope | vs. GTK build |
|---|---|---|---|
| ⌘N | New Note | app | same |
| ⇧⌘N | New Folder… | app | new |
| ⌘, | Settings… | app | same |
| ⌘Q | Quit | AppKit | same |
| ⌘W | Close Window | AppKit | new (standard) |
| ⌘H, ⌥⌘H | Hide, Hide Others | AppKit | new (standard) |
| ⌘M | Minimize | AppKit | **changed**: was Media (library) / Code Block (editor) |
| ⌃⌘F | Enter Full Screen | AppKit | new (standard) |
| ⌘0 | Library (bring the library window forward) | app | new |
| ⌘1 / ⌘2 | View as List / as Grid | app | new |
| ⌃⌘S | Hide/Show Sidebar | app | new (standard) |
| ⇧⌘I | Media… | app | **moved** from ⌘M |
| ⇧⌘F | Search Notes… | app | new (works from an editor too) |
| ⌘F | Find… — library: the search window; editor: the find field | window | same |
| ⌘G / ⇧⌘G | Find Next / Previous | editor | new (standard) |
| ⌘E | Use Selection for Find | editor | **changed**: was Insert Emoji |
| ⌃⌘Space | Emoji & Symbols (AppKit adds it to Edit) | AppKit | **moved** from ⌘E |
| ⌘⌫ | Move to Trash (notes) | library | new (GTK had no key) |
| ⌘Z / ⇧⌘Z | Undo / Redo | AppKit selectors | ⌘Y for Redo **dropped** |
| ⌘X ⌘C ⌘V ⌘A | Cut Copy Paste Select All | AppKit selectors | same |
| ⌘B ⌘I ⌘U | Bold Italic Underline | editor | same |
| ⇧⌘X | Strikethrough | editor | new |
| ⇧⌘M | Code Block | editor | **moved** from ⌘M |
| ⌥⌘T | Insert Table | editor | new |
| ⌘D | Insert Date | editor | same |
| ⌘? | Help | app | new |

Keys inside the note (arrows, ⌥←/→ word, ⌘←/→ line, ⌘↑/↓ document, Return,
⌫, ⌥⌫ word, Tab / ⇧Tab between table cells, Escape) are not in the table:
the note view gets them through `interpretKeyEvents:` →
`doCommandBySelector:`, i.e. the system's key bindings, mapped onto
`OnDocMove` (map §3.4).  Everything marked **changed/moved/new** is a
proposal for the owner; changing one is editing one row of the table.

---------------------------------------------------------------------------

## 7. Threading

- **One SQLite connection per thread.**  `app->db` belongs to the main
  thread.  A worker opens its own (`on_db_open(app->db->path)`), closes it,
  and only then posts its result.  Capture the path and everything else the
  worker needs as locals on the main thread before starting it.
- **Workers**: `on_worker_run(fn, data)` (concurrent, utility QoS, one
  autorelease pool per job).  From Objective-C you may equally use
  `dispatch_async` to a global queue — wrap the body in `@autoreleasepool`.
- **Back to the main thread**: `on_main_post(fn, data)` or
  `dispatch_async(dispatch_get_main_queue(), …)`.  UI, `OnApp`, the config
  and `app->db` are touched on the main thread only.
- **Every async result needs a staleness check** — a cancelled flag
  (`search_job.h`), a generation counter, or a weak reference that has gone
  nil.  A job that may be abandoned frees itself in its own hand-back.
- **Timers**: `on_timer_add` (C callback) or `NSTimer`.  A timer that must
  tick during menu tracking or a drag goes in `NSRunLoopCommonModes`.  A
  `dispatch_after` per event is not a debounce — re-arm one timer.
- **No synchronous long work on the main thread**: the integrity check,
  backups and search already run on workers; thumbnails and the media scan
  must too (decode with `on_ui_thumbnail` on a worker).  IPC commands are
  the deliberate exception: they run on the main thread, like the GTK build.
- The command-line tool runs no run loop: nothing it calls may rely on
  `on_main_post` or a timer.

---------------------------------------------------------------------------

## 8. The byte-offset rule

The document addresses text by **UTF-8 byte offset**: `OnPos.offset`,
`OnRun.len`, `OnDocHit.start/len`, and every index crossing `doc_layout.h`,
`text_layout.h`, `note_edit.h`, `note_view.h`.  CoreText, `NSString`,
`NSTextInputClient` ranges and NSAccessibility count UTF-16 units.

Conversion happens in exactly two places, both through `utf_index.h`
(tested at every character width):

1. inside the text-layout backend (`text_layout.c`), per layout;
2. in the note view's linear index for the input method and accessibility
   (document position ↔ one UTF-16 offset over the note's plain text).

No interface carries a UTF-16 index; no other code converts.  An offset is
always on a character boundary — clamp positions that come from outside
(`on_utf8_from_utf16` moves an index inside a surrogate pair back to the
character's start).

---------------------------------------------------------------------------

## 9. Memory and ownership

`leaks` on the Files app was clean while its footprint grew by 100 MB: the
rules below are about footprint as much as leaks.  `tests/ui_smoke_test.m`
enforces the first four in-process.

**Images — THE RULE.**  An image shown in a view is decoded at the size it
is shown, and the view that shows it is created at that size.  An
`NSImageView` created with a zero frame (`[[NSImageView alloc] init]`,
`+imageViewWithImage:`), sized only by constraints and given its image
before layout, makes AppKit render and cache the image's **largest**
representation and keep a second copy in the view's layer (measured here:
1.2 MB for one 550 px icon, 16 MB for a 2048 px one; correct use: 16 KB).
Therefore:

- never `[[NSImageView alloc] init]` or `+imageViewWithImage:`; make image
  views with **`on_ui_image_view(image, w, h)`** (created at the final
  frame, image set last);
- bundled icons come from **`on_ui_icon(name, points)`** (decoded at
  points × scale via ImageIO; cached);
- a note's image for a thumbnail comes from **`on_ui_thumbnail(bytes, n,
  box_w, box_h, scale)`** or `on_note_image_nth(blob, len, ord, max_px)` —
  capped decode, on a worker.  Never hand a full-size `NSImage` to a small
  view, a table cell, a collection item, a menu item or a button;
- the image viewer is the one place a full-size image is right; it drops it
  on close;
- table and collection cells: build the image view once per cell at its
  size (reuse identifier includes the size if sizes vary), set only `.image`
  on reuse;
- bound every cache (`NSCache.countLimit`; the thumbnail cache keys on
  note id + `updated_at` and holds one entry per note); release decoded
  images when their window closes.

**Who owns windows.**  `NSWindow.windowController` is unretained, so every
controller has exactly one named owner that drops it in `windowWillClose:`:

| Window | Owner |
|---|---|
| library | the file-static `library_window` in library_window.m |
| editors | the registry dictionary in editor_window.m |
| Settings / Search / Media | a file-static (single window) or a file-static array (several) in the module |

`win.releasedWhenClosed = NO` always.  `windowWillClose:` is **teardown**
(dealloc is not): remove timers, cancel jobs, remove observers, nil handler
blocks, flush state — then drop the owner's reference as the last statement
(it may deallocate `self`).  Make it safe to run once only.

**Blocks.**  A block that is stored or runs later captures `self` weakly and
re-checks it:
`__weak T *ws = self; … ^{ T *s = ws; if (!s) return; … }`.
Handler-block properties are `copy`; the owner of the object that holds them
nils them at teardown.

**Observers.**  `addObserver:selector:` observers are removed in
`windowWillClose:` (or `dealloc` for non-window objects).  Block observers
(`addObserverForName:…usingBlock:`) return a token: keep it, remove it at
teardown, and do not capture the token inside its own block.  Files'
singletons never remove theirs — do not copy that into per-window objects.

**Timers and dispatch sources.**  An `on_timer` callback gets an unretained
pointer (`(__bridge void *)self`); that is safe only because the owner calls
`on_timer_remove` in `windowWillClose:` — do so.  A repeating `NSTimer`
retains its target: invalidate it at teardown.  A dispatch source owns its
context and releases it in its cancel handler (`ipc.c` is the model);
cancel, then release.

**Core Foundation across the C/ObjC boundary** — the Create rule, no
exceptions:

- a C function that *returns* a `CGImageRef` returns it **+1**; the caller
  releases it (`CGImageRelease`) or hands it to an owner that will.  The
  model's cache slot is that owner: `block->pixels = image;
  block->pixels_free = (GDestroyNotify)CGImageRelease;`
- a function that *takes* a CF object borrows it for the call;
- under ARC, wrap and release in the same scope:
  `NSImage *i = [[NSImage alloc] initWithCGImage:cg size:s]; CGImageRelease(cg);`
- every `CTFont`, `CTLine`, `CTTypesetter`, `CFAttributedString`, `CGPath`,
  `CGColorSpace` you `Create`/`Copy` is released on every path — B: an
  `on_text_layout_t` owns its CoreText objects and `on_text_layout_free`
  releases all of them; cache `CTFont`s per (family, weight, size), do not
  create one per block;
- `__bridge` only (no ownership moves through a bridge) unless a comment
  says why; no ObjC object pointers inside C structs except
  `__unsafe_unretained` literals in `static const` tables;
- byte buffers from `image_io.h` are `malloc`'d → `free`; strings and lists
  from the core are GLib-allocated → `g_free`, `on_db_*_free`,
  `g_bytes_unref`.  A `gchar *` shown in the UI: `NSString *s = @(c);
  g_free(c);`.

**Background work** runs inside an autorelease pool (`on_worker_run`
provides one; with raw GCD write `@autoreleasepool { }` yourself), captures
its inputs as locals, and never reads `self.…` off the main thread.

---------------------------------------------------------------------------

## 10. Data and test data

- **One data directory** (`config.h`): `$NOTES_DATA_DIR`, else
  `~/Library/Application Support/NotesAppKit/`.  It holds `notes.ini`, the
  default `notes.db`, the default `backups/`, the socket
  `notes-appkit.sock` and the app lock `notes-appkit.lock` (held by the
  running app; the `notes` tool runs headless only when it is free).  Nothing is resolved from `argv[0]`, the bundle or
  the XDG directories; the home directory comes from the password database,
  not `$HOME`.  The only way out is the `db_dir` key, written by File ▸ Open
  Database File… ▸ Set as Default.
- **Preferences are `notes.ini`**, not `NSUserDefaults`: read with
  `on_app_config_get*`, written with `on_app_config_set`, mirrored in
  `OnApp` fields by `on_app_prefs_load` (call it again after a write).  The
  GTK build persisted little — window sizes of search/media, the column
  layouts — and this port persists the same, in the same keys (`list_columns`
  as `key:vis,…`, not `autosaveName`).  The keys `native_menubar` and
  `touch_assist` are GTK-only and gone.
- **`make seed`** rebuilds `dev/data/notes.db`: nested folders with emoji,
  six tags, pinned notes, trashed notes and a trashed folder, a note with
  every block kind (note 1), images, a 200-line note, 40 dated filler notes,
  and 13 action items — open, done, overdue, due today, undated — in several
  contexts.  It refuses to run while an app is serving `dev/data`.
- **`make run`** runs the app on `dev/data` in the foreground (its log, with
  `[status]` lines, on your terminal).
- **The `notes` tool** is `NotesAppKit.app/Contents/MacOS/notes`.  While the
  app runs on the same data directory, the tool's commands execute inside
  the app (and edit an open editor's live document).
- **Tests** get their own directory under `dev/test/<name>` (the Makefile
  sets it; `tests/check.h` refuses to run otherwise).

---------------------------------------------------------------------------

## 11. Code style (the owner's rules; reviewers check them)

- **snake_case** for our own functions, methods, variables, properties,
  files and new types (`on_rect_t`); macros `UPPER_SNAKE`; new enum members
  snake_case.  AppKit overrides keep Apple's names.  **Classes** are
  PascalCase with the `Notes` prefix (`NotesLibraryWindow`), as are
  `NS_ENUM` types and their members.  The carried core keeps its names
  (`on_*` functions, `On*` types, `ON_*` enum members) — do not rename it.
  All C functions, old and new, use the one prefix `on_`.
- **K&R braces**; a function's return type and its opening brace each on
  their own line — for Objective-C methods too (`- (void)` on its own line).
- **Every function and method has a comment block**: purpose (the *why*),
  `Inputs:`, `Output:` (return and error convention).  The full block lives
  at the declaration (the header for public API); the `.m` repeats one line
  and points back.  Only `/* */`.  A behaviour change is incomplete until
  the comment is right.
- **No dead code**, no commented-out code, no unused functions, no "just in
  case" paths; no near-duplicate helpers — one definition of each constant
  (look.h), struct and helper (ui_util.h).  When something is replaced, the
  old path goes in the same commit.  A stub you replace is deleted, not
  kept beside the real thing.
- ARC Objective-C in a C style: C functions for anything that needs no
  object state; classes only for views, window controllers, the app
  delegate, data records.  No nibs/xibs/storyboards, no categories, no
  custom protocols, no `@synthesize`, no bindings, no `#pragma mark`.
  Private state = properties in a class extension with explicit attributes.
  Designated initialisers are snake_case (`init_with_app:`), the inherited
  ones `NS_UNAVAILABLE`.
- **Focus ring**: off (`NSFocusRingTypeNone`) on toolbar text fields only —
  use `on_ui_toolbar_search_field`; everywhere else the system ring stays.
- **Mac conventions win over GTK parity**: standard menus and shortcuts,
  system colours for chrome, dark mode supported (every colour in look.h
  has a dark value).  GTK workarounds listed as "drops out" in the map are
  not ported.
- **No header bars.**  A window has an ordinary title bar.  The one
  `NSToolbar` is the library's, with `toolbarStyle =
  NSWindowToolbarStyleExpanded` (the title bar above it), as in the Files
  and Tasks main windows.  Every other window — the editor above all —
  puts its controls in the window body (the editor's format bar and status
  bar), never in an `NSToolbar` or an `NSTitlebarAccessoryViewController`.
- **No native dialogs.**  Never `NSAlert`, an alert sheet or a custom
  sheet: every message and question is a tool panel (`tool_panel.h`,
  ported from the Files app) — `on_ui_show_notice` and `on_ui_confirm` for
  the common cases, `NotesToolPanel` directly for anything with controls
  (New Folder) or several answers.  Tool panels are not modal: whatever
  must wait for the answer goes in the handler.  The one system panel kept
  is the file picker (`on_ui_pick_path`), as in Files.
- **Activation**: a row or card that stands for a note opens it on
  double-click and on Return/Enter, everywhere a list of notes or action
  items is shown.
- Zero warnings.  `-Wno-unused-parameter` applies to `.m` only (AppKit
  signatures); C keeps the warning.

---------------------------------------------------------------------------

## 12. Verification (run before you hand anything over)

```sh
export PATH=/opt/local/bin:$PATH      # MacPorts pkg-config
make clean && make                    # zero warnings
make test                             # model, core, IPC, UI smoke, golden CLI
make seed && make run                 # look at it, on dev/data
make ui-check                         # the UI test alone; snapshots in dev/snapshots
```

- `make test` = `build/test_document` (35 model tests, GLib only), every
  `tests/*_test.c` against the core, every `tests/*_test.m` against the app
  objects (run from a small bundle so icons load), then `make golden`.
- **`tests/ui_smoke_test.m`** builds the library window and editors
  in-process and checks: closed windows are deallocated (weak references go
  nil), an unsaved live edit is saved on close and by `flush_editors`, a
  blank note is deleted, forty editor open/close cycles do not grow the
  heap, and no decoded-image VM region is megabytes large (with a control
  that does the forbidden thing and must be detected).  It writes
  `dev/snapshots/library-light.png`, `library-dark.png`, `editor-light.png`
  and `editor-dark.png` — look at them.
  **Add your window to this test** (or a `tests/<module>_test.m` of your
  own in the same style): open it, close it, assert the weak reference is
  nil; cycle it for the heap check.
- **Do not run `leaks`, `vmmap`, `footprint`, `sample` or `lldb`.**  On
  this Mac developer mode is off: each of them puts an authorisation prompt
  on the owner's screen and hangs until it is answered.  Screenshots need
  Screen Recording permission and fail.  The in-process test above is the
  substitute: exact ownership checks, a heap budget, an image-region scan
  through `mach_vm_region`, and snapshots through `cacheDisplayInRect:`.
  If the owner enables developer mode, the outside checks are: `leaks
  <pid>`; `vmmap <pid> | grep -E '\[ *(4112|4096)K'` (a 4 MB region
  matters when its type is CoreAnimation, CG image, Image IO or
  VM_ALLOCATE — `MALLOC_SMALL` arenas are also 4096K and mean nothing; this
  app's artwork is 512 px, so the same mistake shows as ~1 MB image regions
  here); `footprint <pid>` before and after exercising the UI.
- **The editor (B, C)**: the acceptance test is `tests/ui/*.txt` replayed
  through `on_note_view_feed_key/text/click` by C's probe.
- Evidence that no live path is opened: `lsof -p <pid>` on the running app
  shows only `dev/…`, the bundle and system files; `tests/config_test.c`
  pins the path policy.

---------------------------------------------------------------------------

## 13. Deviations from the map's §10.6

| §10.6 item | What was done instead | Why |
|---|---|---|
| 2: hooks `close_editors(ids,n)`, `close_all_editors`, `editors_settings_changed(what)` | `flush_editors` is the only hook; `on_editor_close_notes`, `on_editor_close_all`, `on_editor_settings_changed()` are plain functions in editor_window.h | only the core needs a hook; UI modules may call the editor module directly.  One "settings changed" replaces five `*_all` refreshers |
| 2: `open_note(app, id, reveal_kind, ord, search_term)` | hook is `open_note(app, id)`; the full form is `on_editor_window_open` | the core (ipc.c) only ever opens plainly |
| 3: `on_timer_add(seconds, repeating, fn, data)` | plus a `destroy` callback | the backup timer owns its payload |
| 4: `on_note_view_insert_image` takes PNG bytes; `render` returns the neutral image | insert takes PNG bytes; the viewer's `image_handler` returns an `NSImage` | no CF ownership crosses a block; `on_note_view_image` (C function) still returns a borrowed `CGImageRef` |
| 5: `on_doc_layout_new(doc, font spec)`, `image(L, block, inline_ord)` | `font_size` (double); all geometry in `double`; `on_doc_layout_invalidate_all` dropped; `set_hits` takes an array + count | CoreText is fractional; `set_style`/`set_width` already invalidate everything; no GLib container in a new header |
| 6: `text_layout_ct.m` | `text_layout.c` | CoreText is a C API; plain C is the owner's preference where it fits |
| 7: view API with a callbacks struct | C functions + three `copy` handler blocks; `load`, `serialize`, `first_line`, `collect_tags` dropped; `action_strike/due/text` replaced by `begin/end_external_edit` | Files' callback convention; the dropped ones duplicated `on_document_*` calls on `on_note_view_document()`; action edits now live in the core |
| 8: four open variants, five `*_all` refreshers | one `on_editor_window_open` with reveal + search term; one `on_editor_settings_changed` | no near-duplicates |
| 9: plain-C row structs, `library_set_selection`, `trash_notes_core` in library_state.h | rows are private record classes of F and G; selection is set by `on_library_sidebar_select`; trash functions are in library_commands.h; `folder_path_cache` left to G | table/outline items must be objects; one module per function |
| 10: "existing action names as command ids" | kept, except: the `sidebar-hide/show` and `done-hide/show` pairs became `toggle-sidebar` / `toggle-done` with a state function; `inline(s)` / `para(s)` became one id per value; `new-subfolder`, `show-library`, `find-next`, `find-previous`, `find-selection`, `inline-strike` added; `quit` is AppKit's; `insert-emoji` (the editor's Insert menu, as in GTK) opens AppKit's character palette, which inserts through the note view's text input | the two-item device was a GTK workaround; AppKit items carry no parameter |
| 11: adoption of the bundle-adjacent ini; shared socket hash | none: new identity, new directory, fixed socket name per data directory | owner's decision: the fork must not be able to reach the GTK build |
| note_edit.h | minimal: key vocabulary + the blank-note rule | it is package C's internal seam; freezing forty signatures C must then reshape helps nobody |

Also not in the map: `utf_index.h`, `app_lifecycle.h`, `ui_util.h`'s image
helpers, the `NotesLibrary` singleton accessor `on_library()` (hooks receive
only the `OnApp`).

---------------------------------------------------------------------------

## 14. Known hazards (inherited or open)

1. **A note that fails to load looks blank, and a blank note is deleted on
   close.**  `on_note_document_load` returns an empty document when the blob
   cannot be read (as in the GTK build).  D: before applying the
   blank-means-delete rule, make sure the note really was empty or was
   edited in this window — do not delete on a failed read.
2. **Two app instances on one data directory.**  LaunchServices prevents it
   for Finder launches; started twice by path, the second finds the app
   lock held and quits — but it has already opened the database (read-only
   in effect: a current schema is not written on open).  A `notes` command
   run in the instant between an app taking the lock and a headless tool
   finishing can still overlap; the window is milliseconds.
3. **`notes note delete` of a note that is open** leaves its editor open (as
   in the GTK build); the next autosave writes to a trashed note.  E/D may
   want `notify_notes_changed` to close editors of notes that no longer
   exist.
4. **IPC commands run on the main thread** with a 10 s socket timeout; a
   long export blocks the UI for its duration (as in the GTK build).
5. **Search semantics** still come from GLib (`GRegex`, `g_utf8_casefold`);
   they change when GLib is removed, not before.
6. **LSMinimumSystemVersion** is the macOS version MacPorts built GLib for
   (26.0 here), not 14.0, until GLib is gone; sources still compile against
   a 14.0 deployment target, so newer-API use is caught now.
7. `icons/new.png` (2048 px, 2.1 MB) is bundled but unused.
