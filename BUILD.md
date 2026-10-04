# Building Notes AppKit

## Toolchain and dependencies

| What | Version used | Where from | Notes |
|---|---|---|---|
| clang | Apple clang 21 | Xcode command-line tools | C11 and Objective-C with ARC |
| make | GNU make 3.81 | macOS | |
| GLib | 2.88 | MacPorts: `sudo port install glib2 pkgconf` | the ONE non-system dependency; the core still uses it (containers, strings, GKeyFile, GRegex) and it is to be removed in a later phase |
| SQLite | 3.51 (system) | macOS: SDK header, `/usr/lib/libsqlite3.dylib` | not MacPorts' — see below |
| Cocoa, CoreText, CoreGraphics, ImageIO, UniformTypeIdentifiers | system | macOS SDK | |
| GTK 4 | 4.22 | MacPorts: `gtk4 +quartz` | **only** for `make golden`, which builds the GTK reference binary; not needed to build or run the app |

```sh
export PATH=/opt/local/bin:$PATH    # MacPorts pkg-config
make
```

No GTK, GDK, Pango, cairo or gdk-pixbuf is compiled or linked.  `otool -L`
on either executable shows `libglib-2.0` as the only library outside
`/usr/lib` and `/System`.

**System SQLite on purpose.**  pkg-config's `-I/opt/local/include` and
`-L/opt/local/lib` would silently pick up MacPorts' SQLite; the Makefile
keeps only GLib's own include directories and links GLib by path.

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
| `make` | builds `NotesAppKit.app`: the app (`Contents/MacOS/NotesAppKit`), the CLI (`Contents/MacOS/notes`), `Info.plist`, the icons and README in `Contents/Resources`; signs it |
| `make run` | runs the app in the foreground on `dev/data` (seeding it first if needed); `NOTES_DATA_DIR` is set for you |
| `make seed` | wipes and rebuilds the development database `dev/data/notes.db` |
| `make test` | `build/test_document` (the model, GLib only), every `tests/*_test.c` (core), every `tests/*_test.m` (UI, in-process), then `make golden` |
| `make golden` | this build's CLI against the GTK build's, byte for byte (needs MacPorts gtk4; says SKIPPED without it) |
| `make ui-check` | the in-process UI test alone: window snapshots (light and dark) in `dev/snapshots`, ownership and memory checks |
| `make bnbf-scan` | `build/bnbf-scan`, the blob round-trip scan for a database COPY |
| `make install` | copies the bundle to `~/Applications`, replacing an installed copy (a running one is asked to quit, which saves open notes, and is reopened) |
| `make clean` | removes `build/` and the bundle |
| `make clean-dev` | removes `dev/` (development data, test data, snapshots, the GTK reference binary) |

## What is built from what

```
NotesAppKit.app/Contents/
  Info.plist            resources/Info.plist + names from src/identity.h,
                        version from VERSION, NotesBuildDate, LSMinimumSystemVersion
  MacOS/NotesAppKit     every src/*.c except cli_main.c, and every src/*.m
  MacOS/notes           the core (CORE_SRCS in the Makefile) + src/cli_main.c
  Resources/            icons/*.png, README.md
build/                  objects, dependency files, test programs, dev_seed
dev/                    (gitignored) data/, test/, snapshots/, golden/, gtk_ref/
```

Every `src/*.c` and `src/*.m` is compiled — delete a file you no longer use.
A `.c` file is part of the core (and so of the CLI and the tests) only if it
is listed in `CORE_SRCS`.

The names — app name, bundle name, bundle identifier, CLI name, data folder,
environment variable, socket file — are defined once, in `src/identity.h`;
the Makefile reads them from there.

## Version

`VERSION` (one line) is compiled in as `ON_VERSION` and written to the
bundle's `CFBundleShortVersionString` / `CFBundleVersion`.

## Code signing

As in the Files app: the bundle is signed with the first "Apple Development"
identity in the keychain, or ad hoc (`-`) if there is none; override with
`make CODESIGN_IDENTITY="…"`.  The nested `notes` tool is signed first, then
the bundle.  No hardened runtime, no entitlements, no notarisation — a
locally built personal app.  `build/codesign.id` records the identity so a
change forces a re-sign.

## Tests

| Program | Covers |
|---|---|
| `build/test_document` | the block model: 35 tests, links `document.c` + `bnbf.c` + GLib only |
| `config_test` | the data-directory policy (override, default, `$HOME`/XDG ignored), ini round trip through a second process |
| `main_service_test` | main-thread posts, timers, workers; the async integrity check and a backup pass landing under the Cocoa-side run loop |
| `image_io_test` | capped decode, header probe, PNG encode, file → PNG, a note's image by ordinal |
| `ipc_test` | the socket between real processes: wire format byte for byte, the real `notes` tool served by a stand-in app, the app lock (busy, stale socket, lock held with nothing accepting → the tool refuses), a data directory longer than a socket path |
| `lifted_logic_test` | `action_edit` (headless and live), `search_job`, open policy, shutdown order |
| `utf_index_test` | UTF-8 byte ↔ UTF-16 unit offsets |
| `ui_smoke_test` (app objects, in-process; also `make ui-check`) | library and editor windows on a test database: hooks installed, save on close, flush, blank-note deletion, closed windows deallocated, no heap growth over 40 open/close cycles, no megabyte image regions (with a control); writes light and dark snapshots to `dev/snapshots` |
| `tests/cli_golden.sh` | ~740 CLI comparisons against the GTK build, plain and `--json`, reads and mutations, on databases seeded by each build |

Each test runs with `NOTES_DATA_DIR` set to its own folder under
`dev/test/`; `tests/check.h` refuses to run without it.

**The golden test and the GTK reference binary.**  `tests/cli_golden.sh`
exports this repository's own `main` branch with `git archive` into a
scratch folder, builds the GTK app there, keeps only the binary — at
`dev/gtk_ref/notes`, without its execute bit — and deletes the scratch
folder (so no GTK Makefile, whose `install` target replaces the installed
GTK app, is left around).  The binary only ever runs as a *copy* inside a
per-case folder under `dev/golden` that already holds a `notes.ini` pointing
into the same folder — under `env -i` with `HOME` and the XDG directories
redirected into `dev/golden`, and after confirming with `stats` which
database it opened.  The executable copies are deleted when the script
exits.  Do not run that binary any other way.

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

- arm64 macOS; not sandboxed (arbitrary database locations, a Unix socket in
  Application Support, spawning helper commands).
- The app must run from its bundle; the CLI finds the app executable beside
  itself in `Contents/MacOS`.
- `[NSApp activate]` is macOS 14 API and is not caught by the availability
  check (messages to `NSApp` are not checked): keep the compile target at
  14.0 or review such calls by hand.
