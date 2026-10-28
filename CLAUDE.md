# Notes AppKit — project guide

An experimental macOS-only **AppKit port** of the GTK4 Notes app (plain C +
SQLite + a custom block document model with its own layout and rendering).
The non-UI core is done; the UI is being written in parallel work packages.

**Read `porting/INTERFACES.md` before touching anything.**  It is the
contract: hard rules, file ownership, the frozen headers, commands and
keyboard map, threading, the byte-offset rule, memory and ownership, test
data, code style, verification.

## Hard rules (short form)

- NEVER touch the owner's live data, not even to read:
  `~/Library/Mobile Documents/com~apple~CloudDocs/Notes/`,
  `~/.local/share/notes/`, `~/.config/notes/`, `~/.cache/notes-*.sock`,
  the GTK build's `notes` binary, `notes.ini`
  and `dist/` left at the repo root (ignored; `notes.ini` points at the
  real notes), the sibling repos `../tasks`, `../files`.  Never run a GTK
  `notes` binary.
- The notes live in the database Notes SHARES with Tasks
  (`../shared_data`, its own repository — read `../shared_data/PLAN.md`;
  never edit it from here).  The owner's live copy,
  `~/Library/Application Support/{Notes,camb.io,Tasks}/`, is off limits
  too, not even to read.
- Every run uses test data: `make run`, `make seed`, `make test` set
  `NOTES_DATA_DIR` and `CAMBIO_SHARED_DB` under `dev/`.  By hand:
  `NOTES_DATA_DIR=$PWD/dev/data CAMBIO_SHARED_DB=$PWD/dev/data/shared_data.db Notes.app/Contents/MacOS/Notes`.
- Do not change `document.c`, `bnbf.c`, `db.c` behaviour lightly.  There
  is no command-line tool; the core API's behaviour is pinned by
  `tests/core_api_test.c`, the sync with Tasks by
  `tests/action_sync_test.c`.
- A task is written only by the push (a save) and the pull
  (`action_sync.h`), each one transaction with the note; only the fields
  that changed on Notes' side are pushed (`note_task.seen_*`).
- No new GLib in new code; nothing pumps a GLib main loop — use
  `main_service.h`.
- Do NOT run `leaks`, `vmmap`, `footprint`, `sample`, `lldb` or
  `screencapture`: developer mode is off on this Mac, so they put an
  authorisation prompt on the owner's screen and hang.  Use `make ui-check`
  (in-process ownership, heap and image checks; snapshots in
  `dev/snapshots`).
- Branch `main` (the GTK build is on `gtk4`); remote `origin` is `IANatCAMBIO/Notes` — push only when asked.
- No header bars, no native dialogs: ordinary title bars (only the library
  has an `NSToolbar`, expanded style); every message or question is a tool
  panel (`shared_tool_panel.h`, in `../shared_ui`), never `NSAlert` or a sheet.  Details:
  `porting/INTERFACES.md` §11.

## Build and verify

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config, glib2, sqlite3
make clean && make      # zero warnings (needs ../shared_data)
make test               # model, core, UI smoke
make ui-check           # the UI test alone; light/dark snapshots in dev/snapshots
make seed && make run   # the app on dev/data
```

## Where things are

- `src/` — flat, one module per `.h` + `.c`/`.m`.  Core C modules are listed
  in the Makefile's `CORE_SRCS`; everything else is the app.
- `src/identity.h` — every name of this build, defined once.
- `../shared_data/src/shared_db.h` — the shared database's frozen API
  (compiled into the core from `SHARED_DATA_DIR`).
- `../shared_ui/src/` — the interface elements shared with Tasks
  (`shared_look.h`, `shared_ui.h`, `shared_commands.h`, `shared_tool_panel.h`; compiled in
  from `SHARED_UI_DIR`): change them there, for both apps.
- `src/action_sync.[ch]` — the pull of outside changes; `db.c`
  `on_db_note_save_actions` — the push.
- The GTK build — the behavioural SPECIFICATION this port was written
  from — is in git at its last commit, `51ec5a4`:
  `git show 51ec5a4:src/note_view.c`.  Its
  `docs/CLAUDE.md` describes the GTK app, not this tree.
- `porting/GTK_PORTING_MAP.md` — what the GTK app does, feature by feature.
- `porting/FILES_PLAYBOOK.md` — how AppKit code is written here (the Files
  app is the style reference).
- `BLOCK_MODEL.md` — why the document model is the single mutation path.

## Style (the owner's rules)

snake_case for our own functions, methods, variables, properties, files and
new types; classes `Notes…` PascalCase; AppKit overrides keep Apple's names;
the carried core keeps its `on_*` / `On*` names.  K&R braces, return type
and opening brace of a function on their own lines.  Every function and
method has a comment block: purpose, inputs, output/error convention.  No
dead code, no commented-out code, no near-duplicate helpers.  ARC
Objective-C in a C style; no nibs, no categories.  Details and the memory
rules: `porting/INTERFACES.md` §9 and §11.
