# Notes AppKit — icons

`composition.png` is the app's icon (the bundle icon and the About
panel's).  It is the one image `make` copies into the bundle's
`Contents/Resources`; the app loads it with `on_ui_app_icon()`
(`src/ui_util.h`), which decodes it **at the size it is drawn** — never at
the file's full size (see "Memory and ownership" in `porting/INTERFACES.md`).

## Toolbar glyphs

The toolbar shows SF Symbols, not artwork, as the Tasks app does: template
images that the toolbar tints for light and dark mode and for the disabled
look.  They come from `on_ui_symbol(name, points)` (`src/ui_util.h`); a
command's symbol is the `icon` column of the command table in
`src/commands.m`, and a toggle's second face is named in its validate
function.  To change a glyph, change the symbol name there; no file is
involved.

| Command                        | SF Symbol                                   | Retired PNG          |
|--------------------------------|---------------------------------------------|----------------------|
| New Folder                     | `folder.badge.plus`                         | `new-folder.png`     |
| Move Folder to Trash           | `folder.badge.minus`                        | `delete-folder.png`  |
| Quick Note                     | `square.and.pencil`                         | `archive.png`        |
| New Note                       | `plus`                                      | `newnote.png`        |
| Move to Trash (notes)          | `trash`                                     | `deletenote.png`     |
| Hide/Show Sidebar              | `sidebar.left`                              | `sidebar.png`        |
| List or Grid                   | `square.grid.2x2` (list shown) / `list.bullet` (grid shown) | `grid.png` / `list.png` |
| Hide/Show Completed            | `eye.slash` (shown) / `eye` (hidden)        | `hidden.png` / `visible.png` |
| Search Notes…                  | `magnifyingglass`                           | `search.png`         |
| Media…                         | `photo.on.rectangle`                        | `images.png`         |
| Settings…                      | `gearshape`                                 | `settings.png`       |
| AI Summary                     | `cpu`                                       | `microchip.png`      |

A toggle's glyph names what a click will DO, not the state on screen.

## unused/

Artwork no longer in the app, kept for reference: the toolbar PNGs the
glyphs above replaced.  Nothing loads or bundles these files.
