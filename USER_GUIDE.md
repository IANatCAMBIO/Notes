# Notes — User Guide

The full reference for Notes. See [README.md](README.md) to install it and
[BUILD.md](BUILD.md) to build it.

- [Tags](#tags)
- [Working with Tasks (and agents) at the same time](#working-with-tasks-and-agents-at-the-same-time)
- [The day planner](#the-day-planner)
- [The previous meeting](#the-previous-meeting)
- [Where your data is](#where-your-data-is)
- [Backups](#backups)
- [Development](#development)

## Tags

Notes has no folders: **tags** organise the notes, and they are the same
tags Tasks uses (one set in the shared file).

- A note's tags are the **#tags in its text**, and they live on its **tag
  line**: the first line, above the title, small (9 pt) and flush right,
  always led by a 🏷️ that is part of the line's look, not its text (the
  caret, a selection and a delete never reach it; search and export never
  see it). The editor's status bar lists them too (`#Salt #Stride`).
- Type a tag straight into the tag line: a word gets its # as you start
  it, and only tags can be typed there; Return goes to the title.
  Anywhere else, typing **#** opens the list of known tags at once; keep
  typing to narrow it, Return or a click to choose. A tag's word follows
  the # with no space; a space straight after the # closes the list and
  leaves a plain #. Once a tag is set in the body — chosen from the list,
  or a new one ended with a space — it **moves to the tag line**, and the
  sentence closes up around where it was.
- A tag given from outside the text goes on the end of the tag line: drop
  notes on a tag in the sidebar. New Note never tags the note, even with
  a tag selected: it starts with an empty tag line, where the caret is,
  an empty title line and an empty body line, and is listed in All Notes.
  A note closed with nothing in it but tags is deleted, as an empty one
  is. A name of several words is joined, each word capitalized, and
  punctuation dropped: Tasks' "Old Projects" becomes #OldProjects — a tag
  of its own.
- The sidebar lists **All Notes**, **Pinned Notes**, **Action Items**,
  then every tag a note carries (with counts when Settings ▸ Appearance
  shows them), and the **Trash**. Drop notes on a tag to give them the
  tag, on the Trash to trash them.
- Right-click a tag, **Edit Tag…**: an emoji (a click in the slot opens
  the emoji picker; Tasks shows the emoji too) and whether it is a
  **favorite** — a button in the **Favorites bar** across the top of the
  window (View ▸ Hide Favorites Bar slides it away, as in Tasks); the
  tag's menu also offers **Add to Favorites**, a button's **Remove from
  Favorites**. Drag a button along the bar to put the favorites in your
  order (new ones join at the end, by name). Favorites are each app's
  own. A tag's name never changes.
- **All Notes**, **Pinned Notes** and **Action Items** go in the bar the
  same way: right-click the row, **Add to Favorites**. They start before
  the tags and drag anywhere among them; a view's button goes while its
  row does (nothing pinned, no action item), as a tag's goes with its last
  note.
- Right-click a tag, **Clear Tag from All Notes**: after a confirmation
  ("Are you sure you want to remove the tag … from N notes?") its #word is
  taken out of the text of every note that carries it, trashed ones too
  (the gap goes too, and a line left holding nothing else), so the tag
  leaves this sidebar. It stays in the tag lists while a task carries it;
  once no note and no task does, it is deleted.
- A tag lives while a note (trashed too) or a task carries it. A trashed
  note keeps its tags; Restore brings it back with them.
- Export writes one file per note into one folder, each starting with its
  tags (a `tags:` line in Markdown, a paragraph in HTML).

## Working with Tasks (and agents) at the same time

Both apps — and an AI agent using the shared-data skill — may have the
file open at once (SQLite WAL mode). Edits flow both ways:

- **What makes a line an action item** is a `!` that begins a word. It may
  open the line — with any amount of indentation before it, so items sit
  one tab in under a heading — or stand further along the line with the
  item's first letter or digit straight after it (`asked Ann !send the
  quote`), and then the words before it stay ordinary text, in their own
  colour. Anything else after a mid-line `!` leaves it punctuation, so
  `we shipped ! and went home`, `great work !!!` and `shipped it!` are all
  sentences. The first `!` that qualifies opens the item; everything after
  it is the item, a later `!` included. Bullet, numbered, task-box and
  code lines are never items.
- **In Notes**: a line's text, its strike-through and its `due <date>`
  are the task's title, done state and due date. A save writes to the task
  only the fields you changed in Notes, so a priority, note, tag or due
  date set in Tasks meanwhile is never overwritten. Removing a `!` line
  deletes its task; undo brings the same task back.
- **From Tasks**: Notes notices another app's changes within about a
  second (and whenever you switch to it) and rewrites the lines — even in
  notes that are not open, so note text, titles and search stay right. In
  an open note the change is an ordinary edit you can undo; your caret
  stays where it was. A task completed in Tasks is struck through; one
  deleted in Tasks turns its line back into plain text — the words stay,
  and the `!` is replaced by **✅** when the task had been completed
  before it went, or **🪦** when it was given up on while still open. So a
  note tells you afterwards which of its items were finished and which
  were dropped, and a completed action item Tasks deletes for age (its
  Cleanup setting) leaves the sentence you wrote, marked as done.
- **A note changed by another program while it is open**: an editor with
  no unsaved edits shows the new version; one with unsaved edits keeps
  yours and offers to reload.

## The day planner

**View → Show Day Planner** (⇧⌘D, or the calendar button in the toolbar)
slides in the same day planner Tasks has, at the right of the library: the
hours of a day, with the tasks you have scheduled and your meetings from
the macOS Calendar. **Today**, the calendar button and the arrows step
through the days; a red line marks the time.

Double-click a meeting to take notes on it: Notes makes a note titled
after the meeting, with an "Attendees:" line and the attendees as a
bulleted list under it, and opens it with the cursor two lines below the
list (a blank line sets it apart), ready to write. Double-clicking the
meeting again opens that same note (a new one is made only if you have
moved it to the Trash). Each occurrence of a recurring meeting gets its
own note.

The meetings are the macOS Calendar's, so every account set up in the
Calendar app — Google, iCloud, Exchange — shows, kept in sync by Calendar;
Notes has no sign-in of its own and does not need Tasks. Turn it on in
**Settings → Calendar** (macOS asks once for access to your calendars) and
choose the calendars to show. Tasks' Settings has the same section: the
two apps share the setting. The planner redraws as soon as the calendar
changes. Sessions are planned in Tasks; here the planner is read-only.

## The previous meeting

A weekly meeting leaves one note per occurrence, all with the same title
and different dates. When you open a meeting's note from the day planner
and an earlier note with the same title exists, a **clock button** appears
in the editor's toolbar, just left of the find field. Click it (or choose
**View → Open Previous Meeting**) to open that earlier note, and click the
clock there to keep walking back through the series.

The clock is a promise: it is shown only for a note made from the calendar
and only when there is a previous note to open, so no clock means there is
nothing earlier. The series is found by title, not by the calendar: a note
you titled by hand counts as an earlier occurrence, and a rescheduled
meeting still finds its predecessor.

## Where your data is

Notes keeps its notes in **one database file it shares with the Tasks
app**: a Notes action item (a `!` line) *is* a task in that file, tagged
"Action Items" ❗ in Tasks, and with the note's tags as they are when the
item is created, and with the item's line and the four lines above it
as the task's notes, so the task keeps its context. The task is made
once you have finished writing the line — when the caret leaves it, you
stop typing for 5 seconds, or you close the note — not on the first
keystroke. Changing the note's
tags or lines later does not change the task's.

| What | Where |
|---|---|
| The shared database (default) | `~/Library/Application Support/camb.io/shared_data.db` |
| Which file both apps open, when not the default | `~/Library/Application Support/camb.io/location` (one line: the path) |
| Backups (default) | `~/Library/Application Support/camb.io/backups/` |
| Notes' settings | `~/Library/Application Support/Notes/notes.ini` |
| App lock (one running copy per data folder) | `~/Library/Application Support/Notes/notes.lock` |

The database must be on a local disk, outside iCloud Drive, Dropbox and
other synced folders (the app refuses such a file). File ▸ Open Database
File… opens another shared database for this session, or with **Set as
Default** for both apps from now on. A file that is not a shared database
— an old `notes.db` or `tasks.db` — is refused untouched. A shared file
from before tags (schema 100) is backed up and converted the first time
either app opens it: every folder becomes a tag on the notes in it (all of
a note's ancestors too), a trashed folder's notes become trashed notes,
and each action item gets its folder's name before its title once.

`CAMBIO_SHARED_DB` names the database file for a run (development and
tests always set it, under `dev/`), and `NOTES_DATA_DIR` moves Notes' own
settings and lock. While `CAMBIO_SHARED_DB` is set the default cannot be
changed.

## Backups

Backups are the shared module's: a verified copy (`VACUUM INTO`) made by
whichever app is running when one is due, policy in the database itself,
so Settings ▸ Database in either app edits the same policy.

## Development

```sh
make seed     # a realistic test database: dev/data/shared_data.db
make run      # run the app on it
make test     # model, core and UI tests
make ui-check # the UI test alone; window snapshots in dev/snapshots
```

See [BUILD.md](BUILD.md) for the build and
[porting/INTERFACES.md](porting/INTERFACES.md) for how the code is
organised. The GTK sources this port is written from are in git at the
last GTK commit, `51ec5a4`.

The whole app is native AppKit: the library (sidebar, list, grid and
Action Items), the note editor with its own layout and rendering,
Settings, Search, the media browser. The document
model is the GTK build's; the database is the one shared with Tasks.
There is no command-line tool.
