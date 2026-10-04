# Notes AppKit

An experimental, macOS-only AppKit port of **Notes** — an Apple Notes–style
app in C over SQLite: nested folders, tags, rich-text notes with images,
tables, code blocks, task lists, and `!` action items with due dates.  The
original is a GTK4 app; this fork replaces its user interface with native
AppKit while keeping its document model, database and command-line tool.

**Status: foundation only.**  The non-UI core is complete and tested with no
GTK in it.  The app launches as a skeleton: the standard menu bar, one
library window with placeholder panes showing real data from the database,
editor windows that show a note as plain text (nothing can be typed yet),
and the command-line tool talking to the running app.  The real editor,
sidebar, notes list, and the Settings, Search and Media windows are being
written by separate work packages against the interfaces in
[`porting/INTERFACES.md`](porting/INTERFACES.md).

It is a separate application from the GTK Notes: its own bundle
(`NotesAppKit.app`, `com.cambio.notes-appkit`), its own data folder and its
own socket.  It never reads or writes the GTK app's database, settings or
socket, and the two can be installed side by side.

## Prerequisites

- macOS 14 or later to compile against; the built app currently requires the
  macOS version your MacPorts GLib was built for (see BUILD.md)
- Xcode command-line tools (`xcode-select --install`)
- MacPorts `glib2` and `pkgconf` (`sudo port install glib2 pkgconf`)

## Install

```sh
export PATH=/opt/local/bin:$PATH
make install        # builds NotesAppKit.app and copies it to ~/Applications
```

`make install` replaces an installed copy (asking a running one to quit
first, which saves every open note).  Settings and notes live outside the
bundle, so reinstalling never touches them.

## Where your data is

| What | Where |
|---|---|
| Settings | `~/Library/Application Support/NotesAppKit/notes.ini` |
| Database (default) | `~/Library/Application Support/NotesAppKit/notes.db` |
| Backups (default, when enabled) | `~/Library/Application Support/NotesAppKit/backups/` |
| App ↔ command-line socket and lock | `~/Library/Application Support/NotesAppKit/notes-appkit.sock`, `notes-appkit.lock` |

Setting the environment variable `NOTES_DATA_DIR` moves all of them to another
folder; development and tests always do (`dev/` inside the source tree).
The database can also be kept elsewhere — a synced folder, say — through
`db_dir` in `notes.ini`; a window for choosing it (File ▸ Open Database
File…) is part of the work still to come.

## First run

With no database in the data folder the app asks whether to **create a new
database** there or **open an existing `notes.db`** (whose folder is then
remembered).  It never creates one silently, and it never falls back to
another location if the configured database cannot be opened.

## The command-line tool

`NotesAppKit.app/Contents/MacOS/notes` is the same CLI the GTK app has —
every command, flag, output format and exit code (`notes help` lists them;
`--json` on every listing).  While the app is running, a command is carried
out *by the app* over its socket, so there is only ever one writer and open
windows stay current; otherwise the tool works on the database directly.

```sh
N=~/Applications/NotesAppKit.app/Contents/MacOS/notes
$N stats
$N note list --all --json
$N action list --open
printf 'Title\n\n! Call the bank due 2026-11-02' | $N note new --folder Work -
$N quicknote            # starts the app if needed, opens a new note
```

Two differences from the GTK tool, both about starting the app: `quicknote`
and `note open` launch the app and return at once instead of becoming it,
and an unknown command is an error rather than starting the GUI.

## Development

```sh
make seed     # a realistic test database in dev/data
make run      # run the app on it
make test     # model, core, IPC, UI smoke and golden CLI tests
make ui-check # the UI test alone; window snapshots in dev/snapshots
```

See [BUILD.md](BUILD.md) for the build and [porting/INTERFACES.md](porting/INTERFACES.md)
for how the code is organised.  The GTK sources this port is written from
are kept, uncompiled, in `porting/gtk_reference/`.

## License

BSD 3-Clause.
