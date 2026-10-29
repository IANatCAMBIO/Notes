# Notes AppKit

An experimental, macOS-only AppKit port of **Notes** — an Apple Notes–style
app in C over SQLite: tags shared with Tasks, rich-text notes with images,
tables, code blocks, task lists, and `!` action items with due dates.  The
original is a GTK4 app; this fork replaces its user interface with native
AppKit while keeping its document model and database.

The whole app is native AppKit: the library (sidebar, list, grid and Action
Items), the note editor with its own layout and rendering, Settings,
Search, the media browser and the AI summary.  The document model is the
GTK build's; the database is the one shared with Tasks (below).  There is no command-line tool.

It is a separate application from the GTK Notes: its own bundle identifier
(`com.cambio.notes`), its own settings folder, and the database it shares
with Tasks.  It never reads or writes the GTK app's database or settings.
Both bundles are called `Notes.app`, so `make install` replaces a GTK copy
in `~/Applications` (the GTK build is on branch `gtk4`).

## Prerequisites

- macOS 14 or later to compile against; the built app currently requires the
  macOS version your MacPorts GLib was built for (see BUILD.md)
- Xcode command-line tools (`xcode-select --install`)
- MacPorts `glib2`, `sqlite3` and `pkgconf`
  (`sudo port install glib2 sqlite3 pkgconf`)
- the `shared_data` repository checked out beside this one
  (`../shared_data`): the module that owns the database Notes shares with
  Tasks is compiled in from there

## Install

```sh
export PATH=/opt/local/bin:$PATH
make install        # builds Notes.app and copies it to ~/Applications
```

`make install` replaces an installed copy (asking a running one to quit
first, which saves every open note).  Settings and notes live outside the
bundle, so reinstalling never touches them.

## Where your data is

Notes keeps its notes in **one database file it shares with the Tasks app**:
a Notes action item (a `!` line) *is* a task in that file, tagged
"Action Items" ❗ in Tasks.

| What | Where |
|---|---|
| The shared database (default) | `~/Library/Application Support/camb.io/shared_data.db` |
| Which file both apps open, when not the default | `~/Library/Application Support/camb.io/location` (one line: the path) |
| Backups (default) | `~/Library/Application Support/camb.io/backups/` |
| Notes' settings | `~/Library/Application Support/Notes/notes.ini` |
| App lock (one running copy per data folder) | `~/Library/Application Support/Notes/notes.lock` |

The database must be on a local disk, outside iCloud Drive, Dropbox and
other synced folders (the app refuses such a file).  File ▸ Open Database
File… opens another shared database for this session, or with **Set as
Default** for both apps from now on.  A file that is not a shared database
— an old `notes.db` or `tasks.db` — is refused untouched.  A shared file
from before tags (schema 100) is backed up and converted the first time
either app opens it: every folder becomes a tag on the notes in it (all
of a note's ancestors too), a trashed folder's notes become trashed notes,
and each action item gets its folder's name before its title once.

Backups are the shared module's: a verified copy (`VACUUM INTO`) made by
whichever app is running when one is due, policy in the database itself, so
Settings ▸ Database in either app edits the same policy.

`CAMBIO_SHARED_DB` names the database file for a run (development and tests
always set it, under `dev/`), and `NOTES_DATA_DIR` moves Notes' own
settings and lock.  While `CAMBIO_SHARED_DB` is set the default cannot be
changed.

## The day planner

**View → Show Day Planner** (⇧⌘D, or the calendar button in the toolbar)
slides in the same day planner Tasks has, at the right of the library: the
hours of a day, with the tasks you have scheduled and your Google Calendar
meetings.  **Today**, the calendar button and the arrows step through the
days; a red line marks the time.

Double-click a meeting to take notes on it: Notes makes a note titled
after the meeting, with an "Attendees:" line and the attendees as a
bulleted list under it, and opens it with the cursor two lines below the
list (a blank line sets it apart), ready to write.
Double-clicking the meeting again opens that same note (a new one is made
only if you have moved it to the Trash).  Each occurrence of a recurring
meeting gets its own note.

Notes has no Google sign-in of its own: the meetings are the ones **Tasks**
syncs into the database the two apps share (Tasks → Settings → Google
Calendar).  The planner shows what Tasks last pulled, refreshes as soon as
Tasks writes, and shows no meetings while Tasks is not signed in.
Sessions are planned in Tasks; here the planner is read-only.

## Working with Tasks (and agents) at the same time

Both apps — and an AI agent using the shared-data skill — may have the file
open at once (SQLite WAL mode).  Edits flow both ways:

- **In Notes**: a line's text, its strike-through and its `due <date>`
  are the task's title, done state and due date.  A save writes to the task
  only the fields you changed in Notes, so a priority, note, tag or due
  date set in Tasks meanwhile is never overwritten.  Removing a `!` line
  deletes its task; undo brings the same task back.
- **From Tasks**: Notes notices another app's changes within about a second
  (and whenever you switch to it) and rewrites the lines — even in notes
  that are not open, so note text, titles and search stay right.  In an
  open note the change is an ordinary edit you can undo; your caret stays
  where it was.  A task completed in Tasks is struck through; one deleted
  in Tasks turns its line back into plain text (the `!` goes, the words
  stay).
- **A note changed by another program while it is open**: an editor with
  no unsaved edits shows the new version; one with unsaved edits keeps
  yours and offers to reload.

## Tags

Notes has no folders: **tags** organise the notes, and they are the same
tags Tasks uses (one set in the shared file).

- A note's tags are the **#tags in its text**, and they live on its **tag
  line**: the first line, above the title, small (9 pt) and flush right,
  always led by a 🏷️ that is part of the line's look, not its text (the
  caret, a selection and a delete never reach it; search and export never
  see it).
  The editor's status bar lists them too (`#Salt #Stride`).
- Type a tag straight into the tag line: a word gets its # as you start
  it, and only tags can be typed there; Return goes to the title.
  Anywhere else, typing **#** opens the list of known tags at once; keep
  typing to narrow it, Return or a click to choose.  A tag's word follows
  the # with no space; a space straight after the # closes the list and
  leaves a plain #.  Once a tag is set in the body — chosen from the list,
  or a new one ended with a space — it **moves to the tag line**, and the
  sentence closes up around where it was.
- A tag given from outside the text goes on the end of the tag line: drop
  notes on a tag in the sidebar.  New Note never tags the note, even with
  a tag selected: it starts with an empty tag line, where the caret is,
  an empty title line and an empty body line, and is listed in All Notes.
  A note closed with nothing in it but tags is deleted, as an empty one is.
  A name of several words is joined, each word capitalized, and
  punctuation dropped: Tasks' "Old Projects" becomes #OldProjects — a tag
  of its own.
- The sidebar lists **All Notes**, **Pinned Notes**, **Action Items**, then
  every tag a note carries (with counts when Settings ▸ Appearance shows
  them), and the **Trash**.  Drop notes on a tag to give them the tag, on
  the Trash to trash them.
- Right-click a tag, **Edit Tag…**: an emoji (a click in the slot opens
  the emoji picker; Tasks shows the emoji too) and whether it is a
  **favorite** — a button in the **Favorites bar** across the top of the
  window (View ▸ Hide Favorites Bar slides it away, as in Tasks); the
  tag's menu also offers **Add to Favorites**, a button's **Remove from
  Favorites**.  Drag a button along the bar to put the favorites in your
  order (new ones join at the end, by name).  Favorites are each app's
  own.  A tag's name never changes.
- Right-click a tag, **Clear Tag from All Notes**: after a confirmation
  ("Are you sure you want to remove the tag … from N notes?") its #word is taken out
  of the text of every note that carries it, trashed ones too (the gap
  goes too, and a line left holding nothing else), so the tag leaves this
  sidebar.  It stays in the tag lists while a task carries it; once no note
  and no task does, it is deleted.
- A tag lives while a note (trashed too) or a task carries it.  A trashed
  note keeps its tags; Restore brings it back with them.
- Export writes one file per note into one folder, each starting with its
  tags (a `tags:` line in Markdown, a paragraph in HTML).

## First run

With no database where one is expected the app asks whether to **create a
new database** there or **open an existing shared database file** (which
then becomes the default for both apps).  It never creates one silently,
and it never falls back to another location if the database cannot be
opened.

## Development

```sh
make seed     # a realistic test database: dev/data/shared_data.db
make run      # run the app on it
make test     # model, core and UI tests
make ui-check # the UI test alone; window snapshots in dev/snapshots
```

See [BUILD.md](BUILD.md) for the build and [porting/INTERFACES.md](porting/INTERFACES.md)
for how the code is organised.  The GTK sources this port is written from
are in git at the last GTK commit, `51ec5a4`.

## License

BSD 3-Clause.
