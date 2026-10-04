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
  `~/Applications/Notes.app`, the sibling repos `../notes`, `../tasks`,
  `../files`.  Never run a GTK `notes` binary (only `make golden` does, in
  a sandbox).
- Every run uses test data: `make run`, `make seed`, `make test`.  By hand:
  `NOTES_DATA_DIR=$PWD/dev/data NotesAppKit.app/Contents/MacOS/notes …`.
- Do not change `document.c`, `bnbf.c`, `db.c` behaviour.  The CLI's output
  is a contract (`make golden`).
- No new GLib in new code; nothing pumps a GLib main loop — use
  `main_service.h`.
- Do NOT run `leaks`, `vmmap`, `footprint`, `sample`, `lldb` or
  `screencapture`: developer mode is off on this Mac, so they put an
  authorisation prompt on the owner's screen and hang.  Use `make ui-check`
  (in-process ownership, heap and image checks; snapshots in
  `dev/snapshots`).
- Branch `appkit`, no remotes, no pushes.

## Build and verify

```sh
export PATH=/opt/local/bin:$PATH
make clean && make      # zero warnings
make test               # model, core, IPC, UI smoke, golden CLI
make ui-check           # the UI test alone; light/dark snapshots in dev/snapshots
make seed && make run   # the app on dev/data
```

## Where things are

- `src/` — flat, one module per `.h` + `.c`/`.m`.  Core C modules are listed
  in the Makefile's `CORE_SRCS`; everything else is the app.
- `src/identity.h` — every name of this build, defined once.
- `porting/gtk_reference/` — the GTK UI sources and the GTK-era docs: the
  behavioural SPECIFICATION, not compiled, deleted at integration.  Its
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
