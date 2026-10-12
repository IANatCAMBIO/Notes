# Notes AppKit

An experimental, macOS-only AppKit port of **Notes** — an Apple Notes–style
app in C over SQLite: nested folders, tags, rich-text notes with images,
tables, code blocks, task lists, and `!` action items with due dates.  The
original is a GTK4 app; this fork replaces its user interface with native
AppKit while keeping its document model, database and command-line tool.

The whole app is native AppKit: the library (sidebar, list, grid and Action
Items), the note editor with its own layout and rendering, Settings,
Search, the media browser, the AI summary, and the command-line tool
talking to the running app.  The document model, database and CLI are the
GTK build's, and the CLI's output is checked byte for byte against it.

It is a separate application from the GTK Notes: its own bundle identifier
(`com.cambio.notes-appkit`), its own data folder and its own socket.  It
never reads or writes the GTK app's database, settings or socket.  Both
bundles are called `Notes.app`, so `make install` replaces a GTK copy in
`~/Applications` (the GTK build is on branch `gtk4`).

## Prerequisites

- macOS 14 or later to compile against; the built app currently requires the
  macOS version your MacPorts GLib was built for (see BUILD.md)
- Xcode command-line tools (`xcode-select --install`)
- MacPorts `glib2` and `pkgconf` (`sudo port install glib2 pkgconf`)

## Install

```sh
export PATH=/opt/local/bin:$PATH
make install        # builds Notes.app and copies it to ~/Applications
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
The database can also be kept elsewhere — a synced folder, say: File ▸ Open
Database File… opens one for the session, or makes a `notes.db` the default
(its folder is stored as `db_dir` in `notes.ini`).

## First run

With no database in the data folder the app asks whether to **create a new
database** there or **open an existing `notes.db`** (whose folder is then
remembered).  It never creates one silently, and it never falls back to
another location if the configured database cannot be opened.

## The command-line tool

`Notes.app/Contents/Helpers/notes` is the same CLI the GTK app has —
every command, flag, output format and exit code (`notes help` lists them;
`--json` on every listing).  While the app is running, a command is carried
out *by the app* over its socket, so there is only ever one writer and open
windows stay current; otherwise the tool works on the database directly.

```sh
N=~/Applications/Notes.app/Contents/Helpers/notes
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
are in git at the last GTK commit (`GTK_COMMIT` in `tests/cli_golden.sh`).

## License

BSD 3-Clause.
