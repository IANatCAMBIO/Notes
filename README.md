# Notes

![Notes: the library, the day planner and a search](images/screenshot.png)

Notes is a note-taking app that replaces Notes on macOS — a native AppKit
app with its own block document model, rich text with images, tables and
code blocks, and tags shared with the Tasks app. An `!` line in a note is
a real task in Tasks, with its due date and completion kept in step both
ways. There are no folders: tags organise everything, and the same tag set
serves both apps. It is an AppKit rewrite of a GTK4 app, still
experimental, and keeps that app's document model and database.

## Quickstart

**Prerequisites**

- macOS 14 (Sonoma) or later
- Xcode Command Line Tools: `xcode-select --install`
- MacPorts with `glib2`, `sqlite3` and `pkgconf`:

  ```sh
  sudo port install glib2 sqlite3 pkgconf
  ```

**Clone and build.** Notes compiles in the `shared_data` and `shared_ui`
modules, so check all three repositories out beside each other — the
build looks for `../shared_data` and `../shared_ui`, or the paths in
`SHARED_DATA_DIR` and `SHARED_UI_DIR`:

```sh
git clone https://github.com/IANatCAMBIO/Notes.git notes
git clone https://github.com/IANatCAMBIO/SharedData.git shared_data
git clone https://github.com/IANatCAMBIO/SharedUI.git shared_ui
cd notes
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make install
```

`make install` builds the bundle and copies it to `~/Applications/Notes.app`
(per-user; no `sudo`). A running copy is asked to quit first — which saves
every open note — and reopened afterwards. Settings and notes live outside
the bundle, so reinstalling never touches them.

| Command | What it does |
|---|---|
| `make` | build `Notes.app` in the repo root |
| `make seed` | create a test database in `dev/data` |
| `make run` | build, then run against `dev/data` — never your real notes |
| `make install` | build, then install to `~/Applications` |
| `make test` | release checks (build, unit, feature, UI) |
| `make clean` | remove `build/` and the bundle |

To try it without touching real data, `make seed && make run`.

The sources compile against macOS 14, but the binary is linked for the
macOS version your MacPorts GLib was built for, so that is the oldest
system it really runs on. See [BUILD.md](BUILD.md) for the toolchain,
dependencies, test targets and the platform notes.

## First run

Launch from `~/Applications`, or `make seed && make run` for the sandbox.

**The database.** Notes opens the database it shares with Tasks,
`~/Library/Application Support/camb.io/shared_data.db`, and never creates
one: that file is made and looked after by **camb.io Database Manager**
(`camb.io_db_mgr.app`, the `cambio_db` repository beside this one), a small
menu-bar app that also checks the database's integrity and keeps its
rotating backups. Install and start it first; with no database to open,
Notes says so and quits — it never falls back to another location. The
file must be on a local disk and outside iCloud Drive, Dropbox or any
other synced folder.

**Calendar access** is asked for once, the first time you turn on
Settings → Apple Calendar. It is off until you do, and the day planner is the
only thing that uses it.

**Apple Notes** (Settings → Apple Notes, off until you turn it on) copies
every note into one folder of your Apple Notes account, "Notes (camb.io)",
and keeps the copies up to date; nothing is read back, and no other Apple
note is touched. It needs **Full Disk Access** (System Settings → Privacy &
Security), because Apple Notes keeps its notes in a protected folder; the
Settings section says so until it is granted.

**It replaces a GTK copy.** This is a separate application from the GTK
Notes, with its own bundle identifier (`com.cambio.notes`), its own
settings folder and the database it shares with Tasks; it never reads or
writes the GTK app's database or settings. But both bundles are called
`Notes.app`, so `make install` replaces a GTK copy in `~/Applications`.

## Uninstalling

1. Quit Notes and delete `~/Applications/Notes.app`.
2. Your data is outside the bundle and survives, so Tasks goes on using
   it. To remove it too:

   ```sh
   rm -rf ~/Library/Application\ Support/Notes        # Notes' settings and lock
   rm -rf ~/Library/Application\ Support/camb.io      # the shared database and backups
   ```

   Delete `camb.io` only if you are removing Tasks and camb.io Database Manager as
   well — the tasks are in that same file, and that app manages it.

## Features

Compared with Apple Notes:

- **Action items that are real tasks** — write `! Call the vet due
  2026-12-01` and it appears in Tasks tagged ❗ Action Items and with the note's tags. The `!` may
  open the line, indented as deeply as you like under a heading, or stand
  inside it with the item's first letter straight after it (`asked Ann
  !send the quote`).
  Strike it
  through here and it completes there; complete it there and the line is
  struck through here, even in notes that are not open. Delete the task in
  Tasks and the line keeps its words, marked ✅ when it was finished or 🪦
  when it was dropped
- **Tags instead of folders** — a note's tags are the `#tags` in its text,
  living on a tag line above the title. Type `#` anywhere to pick from the
  known tags, and the tag moves itself up to that line. Select notes and
  right-click, **Add Tag**, to tag them all at once. The same tag set
  serves Tasks, emoji and all
- **One database with Tasks**, open in both apps at once (SQLite WAL), so
  edits flow both ways within a second and neither app overwrites a field
  the other changed
- **A day planner inside the note library** (⇧⌘D) — your macOS Calendar's
  meetings beside your notes. Double-click a meeting and Notes makes a
  note titled after it, with the attendees as a bulleted list, ready to
  write. Double-click it again and you get that same note back
- **The previous meeting** — a note made from a calendar meeting gets a
  clock in the editor's toolbar when an earlier note with the same title
  exists; one click opens it
- **A copy in Apple Notes**, optional and one way — every note, in one
  folder of your Apple Notes account, with its headings, lists, check
  items, code, images, files and tables, its tags as Apple Notes tags on
  a 🏷️ line under the title, and its action items in their colour. Written
  through the framework Notes.app's own extensions use, so the copies sync
  to your other devices like any other note.
- **Rich text that is a real document model** — images, tables, code
  blocks and check lists, with its own layout and rendering rather than a
  text view with attributes bolted on
- **A media browser** across every note's images and attachments, and
  search across all notes
- **Backups and integrity checks are not Notes' job**: camb.io Database Manager,
  the menu-bar app that owns the shared file (`../cambio_db`), makes the
  hourly verified copies and runs the checks for both apps
- **Export writes one file per note**, Markdown or HTML, each starting
  with its tags

Everything in detail — the tag model, the Tasks sync, the day planner and
where the data lives — is in
[USER_GUIDE.md](USER_GUIDE.md).

## License

BSD 3-Clause.
