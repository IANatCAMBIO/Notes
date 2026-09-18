---
name: notes
description: Operate the user's Notes app (the plain C + GTK4 + SQLite note-taking app at ~/salt_development/notes) through its CLI — answer questions about notes, folders, tags and '!' action items ("what are my tracked items for the Stride deployment?"), and make changes on request (rename, complete or re-date action items, bulk clean-ups like stripping context prefixes, create or append notes). Use whenever the user asks about their notes, action items, follow-ups, to-dos or a customer/deployment "context" they keep notes on.
---

# Notes — the CLI is the API

The app is at `/Users/ian_campbell/salt_development/notes`.  Everything
an agent needs is the **`notes` CLI**: every record-printing command
takes `--json`, action items have a stable `uid`, and writes go through
the same code the GUI uses.  There is no other supported way in — never
open the SQLite file directly, and never write to it.

```sh
NOTES=/Users/ian_campbell/salt_development/notes/notes    # the real database
NOTES=/Users/ian_campbell/salt_development/notes/dev/notes  # the throwaway sandbox
```

**Which one:** the real database unless the user says "sandbox", "dev"
or is testing a change to the app itself.  `dev/notes` exists only after
`make run-dev`; it holds seeded junk and is safe to trash.

**Never run the binary with no arguments** — that starts the GUI on the
real database.  Every call takes a subcommand.

**A running GUI is fine.**  CLI calls reach it over a socket, so the two
never write concurrently and a change made here shows up in the open
window.  You do not need to ask the user to close it.

## The data model in five lines

- **Folders** nest.  The **folder a note sits in** is its **context** —
  the customer, deployment or project: a note in
  `Salt/Deployments/Stride` has the context `Stride`, whatever is above.
  Notes at the root have context `""`.
- **Notes** are rich text; the first line is the title.  `note cat`
  gives plain text, `note cat --md` gives Markdown with formatting.
- **Action items** are lines in a note whose first character is `!`.
  Text = the rest of the line; **done** = struck through in the note;
  **due** = a trailing `due YYYY-MM-DD` on the line.  They are mirrored
  into a table you query — but the note line is the source of truth, so
  every action write is a rewrite of that line.
- **`uid`** is an item's stable identity: it survives edits, reordering
  and renumbering.  `NOTEID:ORD` is a position and does not.  Address
  items by uid.
- **Trash** is soft: deleted notes/folders are hidden, restorable, and
  excluded from every listing unless a command says otherwise.

## Reading

```sh
$NOTES action list --json                    # every open+done item, newest note first
$NOTES action list --open --json             # only open ones
$NOTES action list --context Stride --json   # one context (case-insensitive)
$NOTES action show UID --json                # one item back
$NOTES folder list --json                    # the tree with note counts
$NOTES note list Stride --json               # notes IN that folder only — not its subfolders
$NOTES note list --all --json | jq '.[] | select(.path | startswith("/Stride/"))'   # the whole subtree
$NOTES note list --all --json                # every note
$NOTES note info ID --json                   # title, path, dates, tags, counts
$NOTES note cat ID [--md]                    # the text
$NOTES search 'stride "load balancer" -draft' --json   # ANDed words, "phrases", -exclusions
$NOTES tag list --json ; $NOTES tag notes NAME --json
$NOTES stats --json
```

JSON action record: `{uid, note_id, ord, done, due ("YYYY-MM-DD"|null),
due_at (unix|0), context, text}`.  Always use `--json` — a title or item
text may contain a tab, which shifts the plain form's columns.

**"What am I tracking for X?"** — `action list --context X --open --json`
is the direct answer.  It matches the note's OWN folder only: items in
`Stride/Reports` have the context `Reports`, so for "everything under
Stride" also take `note list --all --json`, keep the paths under
`/Stride/`, and look those notes' items up.  If X matches nothing, check
`folder list` for the spelling and fall back to `search X`.  Present
items grouped by note, with
due dates, done state, and the uid available if the user wants to act on
one.

## Writing

```sh
$NOTES action done UID            # strike the line     |  action undone UID
$NOTES action due UID 2026-10-01  # set / '-' clears    |  action text UID "new text"
$NOTES action text UID -          # new text from stdin (safe for any characters)
printf 'Title line\n! first action\n' | $NOTES note new --folder Stride/Meetings -   # first line = title
$NOTES note append ID "text"      # on a fresh line; formatting of the rest is kept
$NOTES note move ID... PATH       # '/' = the root
$NOTES note tag ID NAME ; note untag ID NAME ; note pin ID ; note delete ID
$NOTES folder add PATH ; folder rename PATH NAME ; folder move PATH DEST
$NOTES backup FILE.db             # a consistent snapshot of the live database
```

Rules for writes, in order of importance:

1. **Back up before any bulk change** — anything touching more than a
   handful of items or notes:
   `$NOTES backup ~/notes-backup-$(date +%Y%m%d-%H%M%S).db` and tell the
   user where it went.  Restoring is File → Open Database File… in the
   app; there is no undo for CLI writes.
2. **Show the plan before a bulk change and get a yes.**  List exactly
   which items will change and what each becomes (uid, old → new).  For a
   handful of items just do it and report.
3. **`action text` is a rename of the item only**: it keeps the `!`, the
   done state and any trailing `due DATE` by itself — pass just the new
   text.  A new text that *ends* in `due DATE` sets the due date, as
   typing it would.  It refuses blank text and text with a newline.
4. **Never `note set` on a note you have not just created** — it replaces
   the whole content with plain text and loses formatting, images, tables
   and tags.  `note append` keeps them.
5. **Never `trash empty`.**  `note delete` / `folder delete` are trash
   (restorable); `--permanent` is not — don't use it unasked.
6. Re-read after writing when it matters: `action show UID --json`.

### Recipe: strip context prefixes from action items

The user used to type the context into each item by hand — "Stride - Ian
to rerun the reports" — before the Context column existed.  To clean up:

1. `action list --json` (open and done both; a prefix on a done item is
   still a prefix).
2. For each item, a prefix is the item's own `context`, case-insensitive,
   at the start of `text`, followed by a separator — `-`, `–`, `—`, `:`
   or `/` with optional spaces — e.g. `^\s*stride\s*[-–—:/]\s*`.  Do
   **not** strip a context that is merely mentioned ("Ask Stride about
   the SLA") or a different context's name (a Stride note whose item
   says "Acme - …" is a misfiled item to flag, not a prefix to remove).
   Items with context `""` have no prefix to strip.
3. Show the table (uid, context, old text → new text), back up, get a
   yes, then `action text UID -` per item with the stripped text on
   stdin.  Report the count and the backup path.

## Things that look like bugs and are not

- `--context` names a FOLDER, not a path: two folders called `Reports`
  under different customers both match `--context Reports`; tell them
  apart by the item's `note_id` → `note info` path.
- An item is a PLAIN or HEADING line whose first character is `!`.  A
  `!` at the start of a bullet, numbered or checkbox line is text (the
  prefix is the first character there), a `!` in a code block is text,
  and a line that is only `! due 2026-10-01` is no item at all.
- `note new` exits 2 if the folder path does not exist; `folder add PATH`
  is mkdir -p.
- Exit 2 from `search` means the query parsed to nothing (only
  exclusions with nothing to exclude from, or an empty string).
- Exit 1 with the usage text means a flag this build does not know — the
  full synopsis is in `src/cli.h`; do not guess flags.
