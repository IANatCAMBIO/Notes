# Notes — toolbar icons

Custom PNG icons for the toolbars and dialogs.  The only SVGs left are
the bundled `theme/` symbolic arrows (see below); rendering those
requires the librsvg gdk-pixbuf loader: `sudo port install librsvg`
(then restart Notes). Without it, GTK falls back to its stock
arrows.

## Replacing icons

The app loads each icon by filename (`<name>.svg`, then `<name>.png`) —
drop in any 24×24-ish image with the right name to replace one. If a file
is missing or cannot be decoded, the button falls back to a text glyph.

**Run `tools/icon-prep.py` over anything you drop in here.**  Exports
tend to arrive at 2048×2048 with the background painted flat white
rather than left transparent, which shows on the toolbar as a white tile
around the artwork; the script fits the file to the 512×512 everything
else uses and keys that background out.  It rewrites in place and is
safe to re-run.

| File                     | Used for                       |
|--------------------------|--------------------------------|
| `newnote.png`            | New Note                       |
| `deletenote.png`         | Delete Note                    |
| `file.png`               | Drag icon: dragging one note   |
| `new-folder.png`         | New Folder                     |
| `delete-folder.png`      | Delete Folder                  |
| `grid.png`               | List/Grid toggle, while the LIST is showing |
| `list.png`               | List/Grid toggle, while the GRID is showing |
| `images.png`             | Media (image browser)          |
| `sidebar.png`            | Show/hide the folder pane (filing cabinet) |
| `search.png`             | Search                         |
| `settings.png`           | Settings                       |
| `copy.png`               | Code-block copy button         |
| `archive.png`          | Quicknote (new note in the root folder)      |
| `composition.png`      | App logo: window icon + About dialog         |
| `warning.png`            | Delete-confirmation dialogs    |
| `folder.png`             | Drag icon: dragging a folder   |
| `documents.png`          | Drag icon: dragging 2+ notes   |

`file.png` is ONLY the drag-under-cursor icon now.  New Note used to
share it, which meant restyling the button silently restyled the drag
cursor as well; the button has its own `newnote.png` since 2026-09-13
(`unused/addnote.png` is the corner-badged alternative, never shipped).

Replaced 2026-09-13, previous artwork in `unused/`: `delete-folder.png`
(`delete-folder-old.png`), `sidebar.png` (`sidebar-old.png`), and the
old Delete Note icon (`delete-note-old.png`, formerly `delete.png`).
`sidebar.png` changed again 2026-09-14, eye-folder to filing cabinet
(`unused/sidebar-eye.png`).

Note the filenames here name the button's ROLE, not the artwork — a new
image is installed UNDER the existing name (the app looks it up by that
name) rather than added beside it, so there is exactly one file per
button and no code change to re-point.

The List/Grid toggle takes TWO files because its icon names the view a
click switches TO, not the one on screen: `grid.png` while the list is
showing, `list.png` while the grid is.  Replace both or the button will
look inconsistent halfway through a toggle.  (`view.png`, the single
icon it used until 2026-09-12, is now `sidebar.png`.)

These names are looked up but have no bundled file — the editor's
formatting buttons deliberately use crisp Pango text glyphs (B/I/U/S,
H1/H2, ¶, •, 1., { }, ⬜) instead of icons.  Add a file with one of these
names to override a glyph: `heading-1`, `heading-2`, `body-text`,
`list-bullet`, `list-number`, `list-check`, `code-block`.

`theme/` is a minimal bundled icon THEME (prepended to GTK's search
path at startup): sharp symbolic replacements for the stock arrows GTK
itself draws — sidebar expanders (`pan-*`), the in-note search entry
(`edit-find`/`edit-clear`), and the find prev/next buttons
(`go-up`/`go-down`). These are looked up by GTK, not by the app's icon
loader; keep `theme/hicolor/index.theme` in sync if you add sizes.
