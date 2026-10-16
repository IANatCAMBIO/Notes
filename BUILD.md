# Building Notes

## Toolchain and dependencies

| What | Version used | Where from | Notes |
|---|---|---|---|
| clang | Apple clang 21 | Xcode command-line tools | C11 and Objective-C with ARC |
| make | GNU make 3.81 | macOS | |
| GLib | 2.88 | MacPorts: `sudo port install glib2 pkgconf` | the core still uses it (containers, strings, GKeyFile, GRegex); to be removed in a later phase |
| SQLite | 3.53 | MacPorts: `sudo port install sqlite3` | the SAME build Tasks links — see below |
| shared_data | its `main` | the repository beside this one (`../shared_data`) | `src/shared_db.c` is compiled into the core — see below |
| shared_ui | its `main` | the repository beside this one (`../shared_ui`) | `src/*.m` are compiled into the app — see below |
| Cocoa, CoreText, CoreGraphics, ImageIO, UniformTypeIdentifiers | system | macOS SDK | |

```sh
export PATH=/opt/local/bin:$PATH    # MacPorts pkg-config
make
```

No GTK, GDK, Pango, cairo or gdk-pixbuf is compiled or linked.  `otool -L`
on the executable shows `libglib-2.0` (with `libintl`) and `libsqlite3`
from `/opt/local/lib` as the only libraries outside `/usr/lib` and
`/System`.

**MacPorts SQLite on purpose.**  Notes and Tasks open one database file at
the same time (WAL mode), so both link the same SQLite: both Makefiles take
`pkg-config --cflags --libs glib-2.0 sqlite3`.

**The shared database module.**  `SHARED_DATA_DIR ?= $(CURDIR)/../shared_data`;
the Makefile compiles `$(SHARED_DATA_DIR)/src/shared_db.c` with the core's
flags into `build/shared_db.o` (part of the core) and puts
`$(SHARED_DATA_DIR)/src` on the include path.  It holds the schema, opens
every connection, resolves the file's location and makes the backups
(`shared_data/README.md`).  The build stops with a message when the file
is not there; point `SHARED_DATA_DIR` elsewhere to use another checkout.

**The shared interface module.**  `SHARED_UI_DIR ?= $(CURDIR)/../shared_ui`;
the Makefile compiles every `$(SHARED_UI_DIR)/src/*.m` with the app's
Objective-C flags into `build/` beside the app's own objects and puts
`$(SHARED_UI_DIR)/src` on the include path.  It holds what Notes and Tasks
show the same way — `shared_look.h`, the tool panel, the sidebar's split
view controller, the table, status labels, the emoji slot
(`shared_ui/README.md`).  The build stops with a message when the sources
are not there; point `SHARED_UI_DIR` elsewhere to use another checkout.

**Deployment target.**  Sources are compiled with
`-mmacosx-version-min=14.0`, so using an API newer than macOS 14 without an
`@available` check is a build error (`-Werror=unguarded-availability`).  The
executables are *linked* for the macOS version MacPorts built GLib for (read
from the dylib; 26.0 on the machine this was written on), and
`LSMinimumSystemVersion` says the same — while GLib is a dependency that is
the oldest system the app can really run on.  The bundle is not
self-contained: it needs MacPorts' GLib at `/opt/local/lib`.

## Flags

```
-O2 -Wall -Wextra -Werror=unguarded-availability -mmacosx-version-min=14.0
-I src -MMD -MP -DON_VERSION="<VERSION>"        common
-std=c11                                         C
-fobjc-arc -Wno-unused-parameter                 Objective-C
```

`-Wno-unused-parameter` applies to Objective-C only: the unused parameters
there are ones AppKit's delegate, action and callback signatures require.
The C core is built with the warning on.  The build is warning-free; keep it
so.

`-MMD -MP` writes `build/*.d`, so editing a header rebuilds what includes
it; objects also depend on the Makefile and on `VERSION`.

## Targets

| Target | Does |
|---|---|
| `make` | builds `Notes.app`: the app (`Contents/MacOS/Notes`), `Info.plist`, the app icon and README in `Contents/Resources`; signs it |
| `make run` | runs the app in the foreground on `dev/data` (seeding it first if needed); `NOTES_DATA_DIR=dev/data` and `CAMBIO_SHARED_DB=dev/data/shared_data.db` are set for you |
| `make seed` | wipes and rebuilds the development database `dev/data/shared_data.db` with `build/dev_seed` (refuses while an app holds `dev/data`) |
| `make test` | `build/test_document` (the model, GLib only), every `tests/*_test.c` (core), every `tests/*_test.m` (UI, in-process); each with its own `dev/test/<name>/` and `CAMBIO_SHARED_DB` inside it |
| `make ui-check` | the in-process UI test alone: window snapshots (light and dark) in `dev/snapshots`, ownership and memory checks |
| `make bnbf-scan` | `build/bnbf-scan`, the blob round-trip scan for a database COPY |
| `make install` | copies the bundle to `~/Applications`, replacing an installed copy (a running one is asked to quit, which saves open notes, and is reopened) |
| `make clean` | removes `build/` and the bundle |
| `make clean-dev` | removes `dev/` (development data, test data, snapshots) |

## What is built from what

```
Notes.app/Contents/
  Info.plist            resources/Info.plist + names from src/identity.h,
                        version from VERSION, NotesBuildDate, LSMinimumSystemVersion
  MacOS/Notes           every src/*.c and every src/*.m
  Resources/            icons/composition.png (the app icon), README.md
build/                  objects, dependency files, test programs, dev_seed
dev/                    (gitignored) data/, test/, snapshots/
```

Every `src/*.c` and `src/*.m` is compiled — delete a file you no longer use.
A `.c` file is part of the core (and so of the seeder and the tests) only if
it is listed in `CORE_SRCS`; `../shared_data/src/shared_db.c` is added to
the core as `build/shared_db.o`.

The names — app name, bundle name, bundle identifier, data folder,
environment variable, lock file — are defined once, in `src/identity.h`;
the Makefile reads them from there.

## Version

`VERSION` (one line) is compiled in as `ON_VERSION` and written to the
bundle's `CFBundleShortVersionString` / `CFBundleVersion`.

## Code signing

As in the Files app: the bundle is signed with the first "Apple Development"
identity in the keychain, or ad hoc (`-`) if there is none; override with
`make CODESIGN_IDENTITY="…"`.  No hardened runtime, no entitlements, no notarisation — a
locally built personal app.  `build/codesign.id` records the identity so a
change forces a re-sign.

## Tests

| Program | Covers |
|---|---|
| `build/test_document` | the block model: operations, undo, load/save round trips, repairs (bad UTF-8), a random-operations fuzz; links `document.c` + `bnbf.c` + GLib only |
| `config_test` | the data-directory policy (override, default, `$HOME`/XDG ignored), ini round trip through a second process, a malformed ini kept aside |
| `main_service_test` | main-thread posts, timers, workers; the async integrity check (`shared_db_check`) and Back Up Now (`shared_db_backup` on a worker) landing under the Cocoa-side run loop; the backup policy in `shared_settings` (read through `shared_backup_policy_get`), the default folder beside the open file, UTC backup names, retention, the live file's `-wal` untouched, the poll timer |
| `action_sync_test` | the two-way sync with the shared tasks: the push (new task defaults and the Action Items tag, only the fields Notes changed, status rules, tags set in Tasks stick, tombstone and undo revival with the subtasks tombstoned alongside, trash vs hard delete, a failed or rolled-back save leaving no uid behind, a new note from text in one transaction) and the pull (rename / complete / re-date / delete by another connection AND by a forked process reach closed notes' lines, titles, body text and the pane after one poll; unmark; refused titles — blank, due-like, any line break, NUL, invalid UTF-8; no loop; concurrent edits all survive; the poll acting only on the change revs that moved; a failed pull retried on the next tick) |
| `image_io_test` | capped decode, header probe, PNG encode, file → PNG, a note's image bytes by ordinal |
| `core_api_test` | the core API end to end on one database: notes from text, the `!` action-line rules and their task rows (uids, ISO and M/D/YY due dates, done/undone/due, a renamed row re-rendered into the line), tags (given tags appended as CamelCase #words, the old Tags field written into the text once, emoji, favorites, the two scopes, per-note names), pins, the Trash (tags kept), the search query language, the whole-database counts, images by ordinal, Markdown/HTML export, backup |
| `lifted_logic_test` | the app lock between real processes (busy while held, free once the holder dies, released by shutdown), `action_edit` (headless and live), `search_job`, the open policy (the file `$CAMBIO_SHARED_DB` names, WAL; an old `notes.db` refused byte for byte untouched; Set as Default refused under `$CAMBIO_SHARED_DB`; Session Only), shutdown order (and its refusal while an editor cannot save), an unreadable note never treated as empty, export file names and the flat export's tags line, the tag-scoped search |
| `utf_index_test` | UTF-8 byte ↔ UTF-16 unit offsets |
| `ui_probe_test` (app objects, in-process) | the `tests/ui/*.txt` scripts replayed against the note view: typing, keys, clicks, the input method, tables, find, tags |
| `ui_smoke_test` (app objects, in-process; also `make ui-check`) | library and editor windows on a test database: hooks installed, save on close, flush, blank-note deletion, an unreadable note refused, Quick Note, a failed save never dropped, an outside rename reconciled into an open editor (undoable, caret/selection/scroll kept), an outside rewrite of an open note reloaded or (unsaved edits) offered for Reload, a same-second same-size rewrite caught by the blob hash before the autosave, refused titles pushed back from an open note, a rolled-back pull leaving the editor dirty, no library refresh inside a transaction, Open Database refusing an old `notes.db`, permanent delete only of what is still in the Trash, closed windows deallocated, no heap growth over 40 open/close cycles, no megabyte image regions (with a control); writes light and dark snapshots to `dev/snapshots` |

Each test runs with `NOTES_DATA_DIR` set to its own folder under
`dev/test/` and `CAMBIO_SHARED_DB` to `shared_data.db` inside it;
`tests/check.h` refuses to run unless both are under `dev/test/`.  Run one `make test`
at a time: concurrent runs share those folders.

**Tools.**  `tools/dev_seed.c` and `tools/seed-dev.sh` fill
`dev/data/shared_data.db` (`make seed`) through the core API, holding the
app lock while they write (both refuse without `NOTES_DATA_DIR` and
`CAMBIO_SHARED_DB`);
`tools/bnbf-scan.c` round-trips every note of a database COPY through the
model and reports what the loader repaired (`make bnbf-scan`).

**Icons.**  The one bundled image is the app icon, `icons/composition.png`.
Toolbar and format-bar glyphs are SF Symbols drawn by the system
(`shared_ui_symbol`, `src/ui_util.h`), so they need no files and no artwork
tools; the retired toolbar PNGs are kept in `icons/unused/` (see
`icons/README.md`).

**Memory tools.**  Apple's `leaks`, `vmmap`, `footprint`, `sample` and `lldb`
need to inspect another process; on a Mac with developer mode off
(`DevToolsSecurity -status`) each one puts an authorisation prompt on the
screen and waits for it.  They are therefore not part of any target.  The
checks an unattended run can make — closed windows released, heap growth
over open/close cycles, no full-size image held for a small view — are done
in-process by `ui_smoke_test` (`make ui-check`).  With developer mode on,
the outside tools remain the stronger check; point them at a running
`make run`.

## Platform notes

- arm64 macOS; not sandboxed (arbitrary database locations, a lock file in
  Application Support, spawning helper commands).
- The app must run from its bundle.
- `[NSApp activate]` is macOS 14 API and is not caught by the availability
  check (messages to `NSApp` are not checked): keep the compile target at
  14.0 or review such calls by hand.
