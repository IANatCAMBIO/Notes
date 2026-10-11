# Notes AppKit — icons

PNG artwork for the toolbar and the app icon.  `make` copies every
`icons/*.png` into the bundle's `Contents/Resources`, and the app loads an
icon by its file name with `on_ui_icon(name, points)` (`src/ui_util.h`),
which decodes it **at the size it is drawn** — never at the file's full
512 px (see "Memory and ownership" in `porting/INTERFACES.md`).

## Replacing icons

The file names name the button's ROLE, not the artwork: a new image is
installed UNDER the existing name, so there is exactly one file per button
and no code change to re-point it.  Which command uses which file is the
`icon` column of the command table in `src/commands.m`.

**Run `tools/icon-prep.py` over anything you drop in here.**  Exports tend
to arrive at 2048×2048 with the background painted flat white rather than
left transparent, which shows on the toolbar as a white tile around the
artwork; the script fits the file to the 512×512 everything else uses and
keys that background out.  It rewrites in place and is safe to re-run.

| File                | Used for                                              |
|---------------------|-------------------------------------------------------|
| `newnote.png`       | New Note                                              |
| `deletenote.png`    | Move to Trash (notes)                                 |
| `new-folder.png`    | New Folder                                            |
| `delete-folder.png` | Delete Folder                                         |
| `archive.png`       | Quick Note (new note at the top level)                |
| `sidebar.png`       | Show/hide the sidebar                                 |
| `grid.png`          | List/Grid toggle, while the LIST is showing           |
| `list.png`          | List/Grid toggle, while the GRID is showing           |
| `hidden.png`        | Completed-items toggle, while completed items are SHOWN |
| `visible.png`       | Completed-items toggle, while they are HIDDEN         |
| `search.png`        | Search Notes                                          |
| `images.png`        | Media browser                                         |
| `settings.png`      | Settings                                              |
| `microchip.png`     | AI Summary                                            |
| `composition.png`   | The app icon (bundle icon, About panel)               |

The two toggles take TWO files each because their icon names what a click
will DO, not the state on screen.  Replace both or the button will look
inconsistent halfway through a toggle.
