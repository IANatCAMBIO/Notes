# GTK reference — NOT part of the build

Everything in this folder is the GTK4 app this fork was made from: its UI
sources (`src/`), its probe (`tests/`), its Makefile, its `notes.ini.defaults`
and its documentation (`docs/`, `skills/`).  Nothing here is compiled.  It is
kept as the **behavioural specification** the AppKit work packages are
written from, and it is deleted when the port is integrated.

Two warnings:

- The documents in `docs/` and `skills/` describe the **GTK app and the
  owner's real installation** — real paths, the real database, a binary at
  `~/salt_development/notes/notes`.  None of that applies to this fork.
  **Do not run anything they tell you to run.**  This fork's rules are in
  `porting/INTERFACES.md` §1.
- `skills/notes/SKILL.md` is the Claude skill for the GTK app's CLI.  The
  fork's CLI is command-compatible (`make golden` proves it), so the skill
  can be pointed at `NotesAppKit.app/Contents/MacOS/notes` at integration —
  that is not done yet.
