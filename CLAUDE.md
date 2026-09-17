# Notes — project guide

Apple Notes–style app in **plain C + GTK4 (4.22) + SQLite** — the `gtk4`
branch; `main` is the GTK3 original it was ported from, and
`GTK4_MIGRATION.md` is the port's running record (its Decisions D1–D24 are
the measured GTK4 facts this file summarises).  Two window types:
a Library (folders/tags sidebar, notes as list or grid) and one editor
window per note (WYSIWYG rich text). No GNOME HeaderBars anywhere —
plain `GtkWindow` titlebars, formatted `"Notes - <thing>"`.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./notes
make test     # headless block-model tests (GLib only, no GTK)
make ui-test  # the note view driven from tests/ui/*.txt, rendered to PNGs
make run
make app      # dist/Notes.app (unversioned name; macOS; sips/iconutil;
              # composition.png → .icns; NOT self-contained — needs MacPorts GTK)
make deb      # dist/notes_<version>_<arch>.deb (needs dpkg-deb,
make rpm      # dist/notes-<version>-1.<arch>.rpm  needs rpmbuild —
              # build these ON the target Linux distro; they install to
              # /opt/notes + a /usr/bin wrapper script that execs by
              # absolute path so argv[0]-relative icons/defaults resolve)
```

The semantic version lives in the **`VERSION` file** at the repo root
(one line, e.g. `3.6.2`); the Makefile reads it into its `VERSION`
variable with `cat` (macOS ships GNU make 3.81, which has no
`$(file <…)`) and errors out if the file is missing or empty. Single
source: baked into the binary as `ON_VERSION` (About dialog), into the
.deb/.rpm filenames and into the .app bundle's Info.plist — the .app
bundle NAME is deliberately unversioned so the path in /Applications
never changes. Objects depend on both the Makefile and the VERSION file
so a version bump recompiles.

Dependencies (MacPorts): `gtk4 +quartz`, `sqlite3`, `pkgconf`.  The
native macOS menubar needs NO extra library: GTK's own quartz backend
exports `gtk_application_set_menubar()` to the NSMenu bar and builds the
app menu (About / Preferences / Quit) from the `app.about`,
`app.preferences`, `app.quit` actions — gtk-mac-integration (`HAVE_GTKOSX`)
was removed 2026-09 once that was measured.  librsvg is
OPTIONAL now that all app icons are PNGs — it only renders the bundled
`icons/theme/` symbolic arrows.  GTK4 renders through GL/Vulkan; on a
machine with software GL, `GSK_RENDERER=cairo` is the fallback.
**No deprecated GTK4 API is used** (since 2026-09-16): the tree views,
cell renderers and list/tree stores are gone (the library and search
windows are GtkColumnView / GtkListView / GtkGridView over GListModels of
the `list_rows.h` objects), GtkDialog is the `dialog_new` scaffold in
library_window.c over a plain GtkWindow, GtkComboBoxText is a
GtkDropDown, and the pixbuf→cairo bridge is `gdk_texture_download`.  The
build is `-Wdeprecated-declarations` clean with no suppression anywhere;
keep it that way — a new deprecation warning is a bug.
After toggling a dependency, run `make clean && make` so every object
sees the new flags.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config init; sets `icons/composition.png` as default window icon |
| `src/app.[ch]` | Shared `OnApp` context: db handle, open-editors map, icon loading, the shared toolbar-button factory (`on_app_tool_item_new`), THE transient context-menu scaffold (`on_app_menu_popup`: a GMenuModel → self-dropping GtkPopoverMenu, parented to the WINDOW's child box — never to the clicked widget, D14 — with the press translated into it; used by every right-click menu in the app), THE pixbuf → texture edge (`on_app_texture_for_pixbuf`, a memory texture over the pixbuf's own pixels), icons by NAME from the icon theme (`icons/` is an unthemed search path, so `new-folder` finds `icons/new-folder.png` and GTK does the HiDPI loading), the async file picker (`on_app_pick_path` + `OnPickFunc`) and fire-and-forget notice (`on_app_notice`, a GtkAlertDialog) and THE keyboard-shortcut table (`on_app_install_accels`, bound from the application's `startup` signal because `<Primary>` resolves through the display's keymap) |
| `src/db.[ch]` | SQLite: folders (nested), notes (content BLOB), tags, note_tags, counts, ordering |
| `src/bnbf.[ch]` | THE BNBF format, GLib only: `ON_FMT_*` flags and masks, the record reader (`on_bnbf_open/next`) and writer (`on_bnbf_write_*`), `OnTable`, and the two line parsers every layer shares (`on_list_prefix_chars`, `on_action_split_due`).  serialize.c and document.c both drive it; nothing else frames a record |
| `src/document.[ch]` | **The block model** (BLOCK_MODEL.md): `OnDocument` = blocks (PARA/H1/H2/BULLET/NUMBER/CHECK/CODE/IMAGE/TABLE), text blocks own `OnText` (UTF-8 + tiling runs of inline flags + inline images at U+FFFC), NO newline in a block, list prefixes and checkbox characters NOT in the text (the saver re-emits them).  `on_document_from_bnbf` (an `OnDocLoadReport` counts what it normalized) / `on_document_to_bnbf` round-trip the live database byte-identical (1315/1354, the rest explained — see BLOCK_MODEL.md).  Every mutation is an operation (`on_document_insert_text` … `table_remove_col`) that logs its inverse: undo/redo/the op itself are ONE code path (`apply_op`), groupable (`begin/end_group`), observed (`OnDocumentObserver`), with live tags/actions-modified flags.  `OnBlock.eol_flags` keeps the inline flags a newline carries in the blob — fidelity only.  GLib only, no GTK: `make test` proves it |
| `tests/test_document.c` | `make test` — 25 headless tests of the model: round trips of every construct, normalizations, ops, undo, a 400-op fuzz that undoes and redoes everything byte for byte (`--seed` reproduces a failure).  Zero leaks (`leaks --atExit`) |
| `tools/bnbf-scan.c` | `make bnbf-scan` → `build/bnbf-scan COPY.db [--diff ID \| --blocks ID]`: round-trips every blob, verdicts identical/normalized/unexplained/invalid/error, per-category totals, shape stats.  Run it on a COPY (`sqlite3 "file:…?mode=ro" ".backup copy.db"`), never the live file |
| `src/serialize.[ch]` | The blob-level helpers around a note (the format itself is bnbf.h; NO GtkTextBuffer anywhere any more — the editor is the drawn OnNoteView): the cheap image API the media browser needs — `on_note_count_images` (record walk, decodes nothing), `on_note_image_nth` (decode ONE image by ordinal, capped) and `on_note_image_nth_png` (the stored bytes verbatim), all on the `on_bnbf_*` reader, with `on_png_decode_capped` the ONE decode path (the grid thumbnail included) and `on_image_png_bytes` the ONE encoder; `on_note_document_load`, THE preamble of every offscreen consumer (export, CLI content commands, headless action rewrites, thumbnails): blob → OnDocument, no GTK, nothing decoded; and `on_note_extract` / `on_note_extract_text` / `on_note_extract_actions` — the body_text cache and the action_items mirror, derived THROUGH THE MODEL (`on_document_plain_text`, `on_document_action_blocks` + `on_document_block_action`) so an action line has ONE definition (`on_block_is_action`: a PARA/H1/H2 starting with `!`).  Until 2026-09-16 this was a second hand-rolled BNBF walker with its own byte-level rule, which counted bullet `!` lines the model did not — the table's ord n then named a different line than the model's strike/due/rename addressed.  Measured on a copy of the 1354-note database: 117–144 ms for every note, no slower than the walk |
| `src/note_view.[ch]` | **THE rich-text engine**: `OnNoteView`, a drawn `GtkWidget` (GtkScrollable, GtkAccessibleText; 2.7 k lines) that owns the note as an `OnDocument` and paints it through `doc_layout.[ch]` — NO widget ever lives inside it (BLOCK_MODEL.md: the D14/D17/D23/D27/D29 family had nothing to attach to once that was true).  Caret and selection are `OnPos` pairs (block / cell / byte), so a selection crosses tables and images like text; every edit is a document operation (undo = the document's op log, grouped by a 1 s typing pause and 5 sentence enders); typed text goes through `GtkIMContext` (preedit spliced into the block's layout); the clipboard carries `application/x-notes-bnbf` beside plain text so a table or an image pastes as itself; `#tag` capture with a non-focusable GtkPopover at the '#'; Enter/Backspace policy (lists continue, empty item ends the list, headings are one line, Backspace at an item's start makes it body text); the `view.` group for its context menus (`img-*`, `table-*`, cut/copy/paste; the window inserts it on itself, D14); three signals `edited`, `inline-flags-changed`, `image-activated`.  **The keyboard and pointer are also function calls** (`on_note_view_feed_key/text/click`) so `tests/ui_probe.c` can drive it.  Never references the window |
| `src/doc_layout.[ch]` | The GEOMETRY of a document on screen: one PangoLayout per text block (and per table cell), heights as a prefix sum, invalidated per block by the document observer the view relays; hit testing (`on_doc_layout_hit`: text position, image, checkbox, copy word, table border), cursor movement (`on_doc_layout_move`: grapheme/word/line/block/document steps by Pango's log attrs, with a goal x for vertical runs), caret rects, and the painter (`on_doc_layout_snapshot`: code shading + gutter numbers + "copy" word, list prefixes, drawn task boxes, tables with fitted columns, block and inline images as textures, selection rects, caret).  Derived looks live here as Pango attributes — title line (block 0: centred, H1-sized), '!' action tint, #tag colour, macOS emoji padding, find hits.  **Images are SIZED from the PNG header and decoded only when drawn** (the 615-block note lays out in 18 ms; decoding its 17 screenshots first cost 220) |
| `src/editor_window.[ch]` | The editor WINDOW (1.7 k lines) hosting one `OnNoteView`: a GtkApplicationWindow (`show-menubar` FALSE, so Linux draws File/View only in the library), the icons-only toolbar (a `GtkBox.toolbar` of `on_app_tool_item_new` buttons; B/I/U/S are GtkToggleButtons mirrored from the view's `inline-flags-changed`; "Aa" Styles / "≡" Lists / "+" Insert are GtkMenuButtons over GMenus naming `win.para::h1` … `win.insert-image`), the `win.` actions (new-note, find, undo, redo, inline(s), para(s), insert-*) that the accelerators name and that call the view API, the editing GATE (D19: the editing actions are disabled while the focus is in the find entry — the one other widget that owns keys — via `editor_gate_widget`), debounced autosave (`dirty`, the serialize + db write, `note_tags` from `on_note_view_take_tags_modified`, `action_items` from `last_actions` vs the fresh extract with mark hint/sync), the modal image viewer host (`editor_viewer_ops` address images by ORDINAL through `on_note_view_image_count/texture/png/reveal`; `image-activated` opens it; the window's CAPTURE-phase key controller offers every key to it first and swallows the rest while it is up, quirk #23), the status bar, and open/destroy (`on_editor_window_open()` etc. — unchanged public API; the async insert-image continuation re-finds the editor by note id through `EditorRef`).  Windows are placed by the compositor: GTK4 has no window positioning (the GTK3 bottom-right placement, quirk #21, is gone) |
| `src/list_rows.[ch]` | The item objects behind the list widgets: `OnSbRow` (sidebar; `children` GListStore for GtkTreeListModel), `OnNoteRow` (notes list/grid and the search results), `OnActionRow` — plain GObjects with public fields, no properties, because every consumer binds by hand in C.  `on_row_touch(store, row)` is THE in-place update protocol: mutate the fields, then it emits the row's "changed" (the factories from `on_row_factory_new` rebind on it — the list item manager reuses a re-added item's widget without rebinding, gtklistfactorywidget.c binds only when `item != old_item`) and items-changed(pos, 1, 1) on the store (a sort model re-sorts, a GtkMultiSelection keeps the row selected by identity, gtkmultiselection.c items_changed_cb re-adds the same object) |
| `src/library_window.[ch]` | **GTK4 list widgets throughout (2026-09-16, D33)**: the sidebar is a GtkListView over a GtkTreeListModel of `OnSbRow` (list_rows.h; a row's `children` store is what expands, a folder with no subfolders is a LEAF with no arrow), the notes pane is ONE `GListStore` of `OnNoteRow` → GtkSortListModel (fed by the column view's own sorter, so a header click re-sorts the grid too) → ONE GtkMultiSelection shared by the GtkColumnView (list) and the GtkGridView (grid), so the two cannot disagree about the selection; Action Items is a GtkColumnView of `OnActionRow`.  Every row is rendered by a factory from `on_row_factory_new` (`on_*_setup` builds the widget once and installs the row's controllers with the GtkListItem stashed as "on-item", `on_*_bind` fills it and runs AGAIN whenever the bound row emits "changed"; the density-dependent title/preview is `on_title_bind`), an in-place change is `on_row_touch` ("changed" so every bound widget rebinds — GTK's item manager reuses the widget of a re-added item WITHOUT rebinding it, which is why the first cut's grid never showed a thumbnail — then items-changed for that one item so the sort model re-sorts it and the multi-selection keeps it by object identity), and a rebuild is one `g_list_store_splice`.  A drop's `refresh_all` is DEFERRED to an idle: rebuilding inside the drop handler replaced the row widget the drop target sat on before GTK finished the drop, which reported it cancelled and floated the drag icon back to its origin.  Sidebar (folders+counts+emoji prefix, tags+counts): expansion state survives a rebuild keyed by kind+id (the flattened model lists exactly the on-screen rows, an expanded row is on screen by definition), the selection is restored by `sb_reveal` (walks the OnSbRow tree for the row and expands its ANCESTORS, never the row itself), and the Tags header is unselectable by REVERTING a selection that lands on it.  Notes list: Title/Path/Modified/Created, resizable + sortable, Path and Created hidden by default, widths GTK's own (a column view sizes columns to their content; the tree view's autofit measuring pass and its `list_autofit` key are gone), alternating row tint by CSS `row:nth-child(even):not(:selected)`; Comfortable density renders a bold title + small dimmed body-text preview; notes sorted Modified-newest-first by default (a note drag is a move to a folder, never a reorder), folder context menu has Sort Subfolders Alphabetically (one level, `on_db_folder_reorder`), DnD (notes→folder incl. multi-select; single folder rows re-nest INTO / reorder BEFORE-AFTER / trash / drag-restore via `on_db_folder_move`+`on_db_folder_reorder`; drag icons: folder.png, file.png for one note, documents.png for 2+, via `on_app_icon_paintable`; the mechanism is GTK4's: a GtkDragSource on every row (installed by the factory) whose `prepare` hands over ONE boxed `OnDragRows` {kind, ids} (D8) and a GtkDropTarget on every SIDEBAR row — `motion` validates (`sidebar_drop_target`: never onto itself or into its own subtree, notes coerced INTO) and paints the indicator as a CSS class on the row (`drop-into`/`-before`/`-after`, by thirds of the row height), `leave` clears, `drop` acts; a press on a selected row that becomes a drag keeps the multi-selection because GTK4's list items select on RELEASE — quirk #15 and its veto are gone), sortable headers, context menus (a CAPTURE-phase right-click gesture per row, `row_click_gesture`; column headers use `gtk_column_view_column_set_header_menu` with the stateful `win.column-<cfg>-<key>` actions), one unified toolbar (folder area \| notes area \| Sidebar, List/Grid, Hide/Show Completed, Search, Media, Settings — the sidebar toggle sits LEFT of the List/Grid button since both change what the window shows, not the folders; the completed-items toggle (`win.toggle-done`, the Tasks app's button: hidden.png while completed action items are listed, visible.png while hidden, re-pointed by `done_button_sync()` from `refresh_notes` so the Settings checkbox for the same `show_done_actions` key keeps it honest; its View-menu twin is the sidebar item's two-item device, "Hide Completed" on `app.done-hide` / "Show Completed" on `app.done-show`, `done_menu_sync()` enabling the one that names what a click will do; every route changes the key through `done_set_shown()`) flips what the Action Items view lists; the List/Grid toggle pictures the view a click switches TO rather than the one on screen — grid.png while the list is up, list.png while the grid is, re-pointed by `view_button_sync()` off the stack's own `notify::visible-child-name` (NOT from the five places that set the child, so a sixth cannot skip it) and synced once after `show_all` because the toolbar is built after the stack; `view_shows_grid()` is THE one reading of the mode, shared by that icon and by the click, so the picture cannot promise a switch the click will not make, and it answers for the THIRD stack child too — in Action Items, neither list nor grid is up, so it reads `grid_pref`, the mode the pane will come back to.  `on_app_tool_item_set_icon()` is the re-pointing call, sharing `tool_icon_widget()` with `on_app_tool_item_new` so a built button and a re-pointed one cannot disagree about the fallback glyph; the single view.png it used until 2026-09-12 is now the sidebar
button's icons/sidebar.png.  Its Quicknote button (archive.png) calls `on_library_quicknote()` — a note in the ROOT folder whatever is selected, editor to the front; THE one implementation, also behind the `notes quicknote` CLI/IPC command \| Search …; the About button that used to sit at the far right, with its expanding spacer and per-style child swap, was removed 2026-08 — About lives in the File menu only), menubar (File/View — ONE separator in each: File is New Note / New Folder / the two Export All items, rule, then Open Database File… / Settings / About / Quit, i.e. what acts on the NOTES above and what is about the app or its file below; View is Notes as List / Notes as Grid / Show-Hide Sidebar / Hide-Show Completed, rule, then Media… / Search Notes…, i.e. what the WINDOW looks like above and the two window-openers below.  A rule between every pair of items divides nothing — that is what both menus had until 2026-09-12, matched to the sister Tasks app.  The sidebar item is an ACTION whose LABEL names what a click does: TWO model items, "Hide Sidebar" on `app.sidebar-hide` and "Show Sidebar" on `app.sidebar-show`, each `hidden-when=action-disabled`, and `sidebar_menu_sync()` enables exactly one from the pane's LIVE visibility — synced once after `show_all`, since nothing is visible before it.  Never by editing the model: see "Actions, menus and shortcuts".  Those two, the toolbar's Folders button (`app.toggle-sidebar`) and everything else that changes the pane route through `sidebar_set_visible()`, THE one place that does, so the label cannot drift from the pane), the menubar MODEL (`build_menubar`, `GMenu` over `app.` actions; `on_library_apply_native_menubar` chooses between `gtk_application_set_menubar` — native on macOS, drawn by the GtkApplicationWindow on Linux — and an in-window `gtk_menu_bar_new_from_model` for the setting's OFF state, macOS only), the action tables (`APP_COMMANDS`/`WIN_COMMANDS`: name → `void (*)(OnLibrary *)`, installed by `library_install_actions`), bottom status bar (left: selection path; selecting notes posts a transient "N files selected" event from both views' selection signals; right: latest event — post from anywhere via `on_app_status()`, printf-style, no-op until the library installs `app->notify_status`) |
| `src/search_query.[ch]` | THE query language, shared by the search window's worker and the CLI's `search` (it replaced `on_note_text_matches`, which knew only one literal needle): `on_query_new` parses the text into terms — bare words ANDed, `"quoted phrases"` matched whole, `-word`/`-"phrase"` excluding — and `on_query_matches` tests one note's title+body against all of them, folding the note ONCE however many terms there are. A '-' with space after it and an empty `""` are ordinary text/nothing; an unclosed quote runs to the end; curly quotes count as quotes (macOS input methods). A query of nothing but exclusions matches every note that avoids them, and one that parses to NO terms reports `on_query_is_empty` so callers can prompt instead of returning zero hits. Regex mode has no term syntax at all — the query is one pattern, compiled here so a bad one is caught before any searching. `on_query_highlight_term` (first positive term, unquoted) is what an opened result seeds the editor's in-note search with |
| `src/search_window.[ch]` | Search over titles + full text on a worker thread (results: a GtkColumnView over a GListStore of `OnNoteRow` — id, path, modified — activated by position) (spinner while running); scope = All Notes / live library selection; case + regex options; the query goes through `search_query.[ch]`, and the parsed OnQuery — immutable, so the worker matches with it freely — IS what the job carries |
| `src/image_viewer.[ch]` | THE modal image viewer, shared by the media browser and the editor: a GtkOverlay child (a dark GtkBox covering the whole overlay whose GtkGestureClick swallows every press meant for the widget behind), a `GtkPicture` (GTK_CONTENT_FIT_SCALE_DOWN — never up, aspect kept; sharp on HiDPI because the texture keeps every pixel) wrapped in a render-node paintable whose intrinsic size is the fit to the overlay less `ON_IMAGE_VIEWER_INSET` and the two label rows, so the caption/action row sits under the picture's corner; a caption + host action link row; and a centred "Previous \| Next" row (ONE GtkLabel carrying both words, split by MIDLINE, quirk #24; NO wrap-around — a dead-end word is dim via the CSS alpha class and the key is consumed).  Click anywhere / Escape closes; Left/Right and the words go through `img_step`.  Hosts are DECOUPLED by `OnImageViewerOps` — `count`/`render`/`caption`/`action`, images addressed by INDEX, `count()` re-read on every use; `render()` returns a GdkPaintable (the full texture) the panel takes over, NULL closes it; `action()` runs AFTER the panel has closed itself.  A tick callback that runs only while the panel is open notices the overlay changing size and asks the host to render again after a 150 ms settle.  **The panel NEVER takes the keyboard focus** (`can-focus` FALSE, no link labels; words are hit-tested from the panel's own gesture with `gtk_widget_compute_bounds`, the hand cursor is `gtk_widget_set_cursor_from_name` from its motion controller) — quirk #23.  `on_image_viewer_key_press(v, keyval, state)` is called from the host WINDOW's CAPTURE-phase key controller.  The panel holds its OWN reference to its widget: GTK4 emits a window's `destroy` AFTER its dispose has torn the child tree down (D21), so `on_image_viewer_free` works in either order and never touches the overlay |
| `src/media_window.[ch]` | Media browser (library toolbar's images.png button, View \| Media…, Ctrl/Cmd+M): every image embedded in the notes the pane is listing, as a GtkFlowBox of square-boxed thumbnails captioned with their note. Note set comes from `notes_for_selection()` in library_window.c — THE one selection→note-list mapping, shared with `refresh_notes` — and is SNAPSHOTTED at open (id+title only). Scanning runs in 40 ms idle slices (`media_scan_idle`), one image per step, holding the current note's blob across yields; image counts come from `on_note_count_images` (record walk, no decode) and each thumbnail from `on_note_image_nth` at thumb size. Capped at MEDIA_MAX_IMAGES (500) with the truncation said in the status line — every cell holds its own decoded pixels. A single click opens the SHARED modal viewer (`image_viewer.[ch]`, which owns the panel, the clicks, the keys and the Previous | Next row); this window supplies `media_viewer_ops` — cells addressed by `MediaCell.idx` (grid order, append-only), a caption, and the "Show in source note" action link, whose render op re-reads the note's blob at panel size rather than upscaling the thumbnail. Getting to the note is that LINK under the image's bottom-right corner, NOT a double click — the first press of a double click dismisses the panel its second press was aimed at, and before the modal design the same collision made a double click open the wrong note (the in-grid expand reflowed the grid under the pointer). The words are plain labels hit-tested by the panel's own gesture (`img_hit`). `media_viewer_action` calls `on_editor_window_open_image()`. `media_add_cell` calls `on_image_viewer_nav_sync` when a cell lands directly after the one on show — a greyed-out "Next" going live mid-scan, the only nav change a scan can cause |
| `src/settings_window.[ch]` | List density, sidebar counts, code copy/line-number toggles, first-line-H1, image viewer, native macOS menubar (macOS-only checkbox), and the Database section — a health PLATE (a bordered frame over a GtkGrid, so the five values share one x) saying Health / Current database / Data / Size on disk / SHA-256, with ONE `Update` button under the whole plate because it renews every line of it, over the rotating-backup controls.  There is NO control for WHERE the database lives and there must not be one again: that is File → Open Database File… only (see the Database section's own comment) |
| `src/backup.[ch]` | Optional rotating database backups, OFF by default: a worker thread copies the live DB through `on_db_backup_to()`, VERIFIES the copy with `on_db_verify_file()`, discards it if it fails, and only THEN prunes beyond `backup_keep`.  A pass whose source is unchanged since the last one (the `backup_source_stamp` ini key: destination + source path + size:mtime) writes nothing.  The timer carries the db path, so `on_backup_auto_start()` must be re-called after File → Open Database File… |
| `src/export.[ch]` | HTML + Markdown export (all notes mirroring folder tree, or single note), walking the note's OnDocument block by block (2026-09; the old buffer walk read a line's style off its first character, which for a LOADED task line was the untagged checkbox anchor — so every exported/`note cat --md` task line came out as a plain paragraph with a leading space.  Fixed by the move; 35 s → 2 s for the 700 MB library because nothing is decoded) |
| `src/cli.[ch]` | Headless subcommand interface (runs before GTK in main; tags/folders/notes CRUD, backup, export); folders by path, notes by id. Agent-ready surface: `note cat [--md]` (plain text from the body_text cache / Markdown via `on_export_note_markdown`, images as `![image N]()` placeholders), `note append`/`note set` (plain text in; `set` REPLACES content and clears the tag links), `search TEXT [--regex]` (case-insensitive titles+bodies via `on_db_note_body_map`, prints id/modified/path; TEXT takes the same operators as the search window — ANDed words, "quoted phrases", -exclusions — so quote the whole query for the shell, and an empty one exits 2), `note tags`/`tag`/`untag` + `tag notes` (`note tag` appends the literal `#name` span under the on-tag text tag and rewrites note_tags from the buffer, so GUI saves keep it), `note restore`, `action list/show/done/undone/due/text` (items addressed `NOTEID:ORD` **or** by stable `UID` — `action_token_parse` tells them apart BY SHAPE, a ':' means the positional form, a bare decimal means a uid; `action list --uid` prepends the uid as a further FIRST column, leaving the default output byte-identical because text must stay last, and an unknown flag exits 1 so a caller can probe an older build; done/due/text rewrite the '!' line via the on_editor_action_* helpers — headless OnApp has editors==NULL so they take the document path (`on_document_action_*`).  `action show UID` prints ONE item as the same UID-first record `list --uid` emits (`action_print_line` is the one definition of that layout) — the O(1) read-back a mirror needs for a pinned item, instead of listing and diffing the whole table.  `action text UID TEXT` RENAMES an item: `action_text_ord` replaces only the span `on_action_split_due` marks off, so the '!' prefix, the line's own spacing and any trailing `due <date>` survive, and the new run is given the old text's strike state explicitly (a plain insert would inherit it from the preceding character).  The uid survives a rename on BOTH paths — the editor's identity mark sits at the LINE START, outside the replaced span, so `action_marks_prune` leaves it alone and the hint pass re-matches; headless, the ord pass does, since a rename moves no line.  A blank or newline-containing text is REFUSED (either would stop the line being that item and retire its uid), and a new text ending in a parseable `due X` sets the due date, exactly as typing it in the editor would); `note new/append/set` and `action text` all accept `-` = stdin (shipped over the socket by `on_cli_command_reads_stdin`).  **GUI parity (2026-09)**: `folder info/rename/move/emoji/ai-mode/sort/restore`, `note info/pin/unpin`, `note list --recent|--pinned`, `trash list`/`trash empty --yes`, `stats` — all thin wrappers over db functions the GUI already used.  Two things about them: `folder restore` takes an ID, not a path, because `on_db_folder_list` filters trashed rows so a trashed folder's path no longer resolves (`trash list` is where the id comes from); and `trash empty` REFUSES without `--yes`, being the one CLI command with no undo.  `folder sort` and the GUI's Sort Subfolders Alphabetically are now ONE function, `on_db_folder_sort_children`.  **Images out**: `note images ID` lists ord/bytes/WxH (dimensions from each PNG's header via `on_png_probe_size` — nothing is decoded) and `note image ID N FILE` writes one out through `on_note_image_nth_png`, i.e. the stored bytes VERBATIM, never decode+re-encode.  N is **1-based** on both, matching the `![image N]()` placeholders `note cat --md` writes.  **`--json`** on every record-printing command (note list/info/cat/tags/images, folder list/info, tag list/notes, action list/show, search, trash list, stats): one array, or one object for the single-record commands.  The reason is escaping — a title or action text may contain a tab, which silently shifts the plain form's columns.  `cli_json_take` strips the flag from a COPY of argv before dispatch and only for commands `cli_json_capable` names, so a note whose content is literally `--json` still survives `note new`; it ASSIGNS `cli_json` every dispatch, since inside a GUI instance serving remote calls the static outlives the command.  JSON records carry more than the plain ones (path, folder_id, created, pinned; the action uid always) — a JSON note listing must therefore always be given the folder-path map, which is what `cli_paths_for` is for |
| `icons/` | custom PNG toolbar icons + `composition.png` app logo (window icon + About dialog, and the .icns/Linux hicolor packaging icon), loaded by basename; see icons/README.md |
| `tools/import-apple-notes.sh` | Apple Notes migration (AppleScript export → CLI import; keeps modification dates) |
| `tools/icon-prep.py` | Normalizes a dropped-in toolbar icon: fits it to 512×512 and keys the flat white background out to alpha.  Icon exports keep arriving at 2048² as PNG colortype 2 (no alpha), which renders as a white TILE on the toolbar.  The key is a flood fill inward FROM THE BORDER, never a global white threshold — the artwork's own paper-white interior (~233 in this set) has to survive, and it does because the outline walls the fill off.  Re-running is safe: it recomputes alpha from the colour planes rather than compounding the last pass |

## Actions, menus and shortcuts

Every command is a `GAction` (2026-09, GTK4 migration Phase 2 — see
`GTK4_MIGRATION.md`); menus, toolbar buttons and keyboard shortcuts only
NAME actions.  There are no `GtkMenuItem` callbacks, no hand-rolled
key-press switches for modifier shortcuts, and no toolbar "clicked"
handlers that duplicate a menu item.

- **`app.`** — what the menubar references.  It must work from whichever
  window is focused (on macOS the native menubar is the only menubar), so
  it lives on the `GtkApplication` and acts on THE library window,
  re-creating it if it was closed (`on_app_command`, via the weak pointer
  `app->library_window` — closing the library with editors open leaves it
  NULL, never dangling).  `app.about`, `app.preferences`, `app.quit` are
  the names GTK's quartz backend binds its own app menu to.
- **`win.`** — what is invoked only from inside a window: toolbar buttons
  (`gtk_actionable_set_action_name`), context menus, and EVERY keyboard
  shortcut.  Both window types are `GtkApplicationWindow`s (that is what
  gives them a `win.` group and accelerator dispatch; the editors set
  `show-menubar` FALSE so Linux does not draw File/View atop each one).
  The same key may mean different things per window — ⌘F is the search
  window in the library and the in-note find box in an editor, ⌘M is the
  media browser vs a code block — GTK activates whichever of an accel's
  actions the focused window has AND has enabled.  Where a shortcut and a
  menubar item mean the same thing (`new-note`, `find`, `media`) the
  `win.` name binds the SAME function.  GTK4 parses `<Primary>` as CONTROL on every platform (D13), so
  `on_app_install_accels` spells the table's `<Primary>` as `<Meta>` (Cmd)
  on macOS and `<Control>` elsewhere; Ctrl+key no longer works on the Mac
  (deliberate).
- **Editor EDITING actions are disabled while the focus is in the find
  entry** — the one other widget that owns keys — and enabled everywhere
  else (`editor_gate_widget`, a focus controller on it: enter disables,
  leave enables; D19).  A
  window accel fires whatever has the focus, and a disabled action is
  skipped by the accel lookup, so ⌘B in the find box is a plain key again.
  The gate is NOT the view's own focus: an open popover menu takes the
  keyboard focus, and gating on the view greyed out the very Insert /
  Styles items being opened.  The context-menu actions are the VIEW's own
  `view.` group (`view.img-*`, `view.table-*`, `view.cut/copy/paste`),
  never gated.  Everything else the keyboard does in a note — arrows,
  Home/End, Enter, Backspace, Tab, ⌘A/C/X/V — is the view's own key
  controller (`on_key_pressed`), after the input method.
- **Context-menu targets**: the library's note menu carries them as action
  targets (`win.note-open(x)` the CLICKED id, `win.note-pin(b)` the state
  the clicked note implies); the note view stashes the layout's hit
  result for the last right-click as `v->ctx` (block, cell, image
  ordinal) and its `view.img-*`/`view.table-*` actions act on that.  The
  menu itself is built per press (`context_menu`: image items when on an
  image, table items when in a table, then Cut/Copy/Paste) and shown by
  `on_app_menu_popup`.  Column menus create their
  stateful `win.column-<key>` actions on demand at popup, state = visible,
  enabled = not the last visible one.
- **Stateful booleans (`autofit`, `column-*`, `table-header`) use the
  `change-state` path** and call `g_simple_action_set_state` themselves —
  an `activate` handler on a stateful action does not flip the state, so
  the check mark would never move.
- **A dynamic menu label is two items with `hidden-when=action-disabled`**
  (the View menu's Hide/Show Sidebar on `app.sidebar-hide`/`-show`), never a
  runtime `g_menu_remove`/`g_menu_insert`: on the GTK3 build the latter
  tripped a misplaced guard in MacPorts' GTK; on GTK4 the declarative form
  is simply what GtkPopoverMenu expects.  `quartz_log_filter` in main.c
  still drops the two GTK3-era messages — harmless if they never match —
  and GLib's `poll(2) failed due to: Undefined error: 0` WARNING, which
  is current: no poll failed — GDK's macOS `poll_func`
  (gdkmacoseventsource.c, the same design as GTK3's quartz one) returns
  -1 with errno untouched when Cocoa re-entered the main loop inside
  `nextEventMatchingMask:` and the fd array it was handed went stale
  (seen on the code blocks' copy link: the clipboard write's pasteboard
  round-trip is the presumed re-entry; the copy lands); GLib then just
  re-runs the iteration.  The errno in the text is stale garbage from an
  earlier syscall ("Undefined error: 0", "Invalid argument", "No such file
  or directory" all seen), so the whole prefix is dropped.

## Data & formats

- DB: `~/.local/share/notes/notes.db` (GLib user-data dir; the filename is
  `ON_DB_FILENAME` in db.h).  The dir went `orange-notes` → `blue_notes`
  pre-release → `records` → `notes` (2026-08), and the file `notes.db` →
  `records.db` → `notes.db` again.  **Do not rename either again without a
  shim**: user data lives there, and a rename with no pickup path looks
  exactly like "my notes vanished".  The 2026-08 rename shipped one
  (`on_db_adopt_legacy_path()`: rename a leftover `records.db` into place,
  either beside the expected path or from the old default dir); it was
  REMOVED days later once the sole user's database had been adopted, the
  same way the pre-1.4 `notes.db` shim went in 2026-07.  Consequence to
  know before opening an old backup: a `records.db` is no longer picked
  up — rename the file to `notes.db` by hand.
- Note content: **BNBF v5** blobs (magic `BNBF`; the pre-rename `ONBF`
  magic was retired 2026-07 after an offline scan found zero such blobs)
  (see header comment in `serialize.h`).
  TEXT records = styled runs (flag bits ↔ named GtkTextTags via one
  shared table); IMAGE = full-resolution PNG + display width; TABLE;
  CHECK. All older versions (1–4) still parse.
- **A blob can be rewritten WITHOUT deserializing it**, and a format
  migration MUST be written that way: a full deserialize decodes every PNG
  in the database (seconds per hundred MB, and the DB is ~600 MB).  Walk
  records like `on_note_extract_text` does, copy image payloads verbatim,
  change only the flag words you mean to, preserve the blob's own version
  field, and return "nothing changed" so untouched notes are never
  rewritten.  Write back through a content-ONLY UPDATE — a formatting
  cleanup must not bump `updated_at`, or the whole library reorders
  itself.  The 2026-08 first-line-H1 strip
  (`on_note_strip_first_line_h1` + `on_app_first_line_h1_strip` +
  `on_db_note_set_content`) was the worked example; all three were removed
  after it ran on the sole user's database and found zero notes to change
  (verified independently: 0 of 1296 notes carried any paragraph style on
  line 0).  `git log` has it if the shape is ever needed again.
- **`PRAGMA user_version` 4 IS CONSUMED** even though nothing claims it any
  more: the removed H1 strip stamped it, so the user's database sits at 4
  while a freshly created one stops at 3.  The next one-time migration must
  therefore be **5 or higher** — numbering it 4 would silently skip the one
  database that matters.
- **Task checkboxes are CHECK blocks** (`ON_BLOCK_CHECK`, `checked` on
  the block; BNBF v5 CHECK records, re-emitted by the saver as the CHECK
  record + " " + the text, exactly as the GtkTextBuffer era wrote them).
  The editor draws the box itself and toggles it on click.  (The pre-v5
  glyph format and its load-time migration were removed 2026-07 after a
  blob scan verified zero glyph notes remained.)
- **Images are blocks** (`ON_BLOCK_IMAGE`: the stored PNG bytes verbatim
  + display width) — or, when an IMAGE record shares a line with text,
  INLINE images (`OnInlineImage` at a U+FFFC in the block's text, drawn
  through a `PangoAttrShape`; 3 notes in the live library).  The view
  decodes a `GdkTexture` when it first draws one and caches it on the
  block (`OnBlock.pixels`); export/thumbnails/CLI read the bytes off the
  OnDocument and decode nothing they do not show.  Default thumbnail
  display fits a 200×125 box (`IMAGE_THUMB_W/H` in doc_layout.c, aspect
  kept, never upscaled); the image's context menu switches thumbnail /
  full size (`on_document_set_image_width`).
- ALL UI settings live in the ini (`[notes]` group), loaded into
  memory ONCE by `on_app_config_init()` and written through on change
  (`on_app_config_get/set`); the file is never re-read while running.
  The ini normally sits NEXT TO THE BINARY (portable mode); when no
  binary-adjacent ini exists AND that directory is unwritable (system
  installs: .deb/.rpm in /opt, .app in /Applications) it falls back to
  `~/.config/notes/notes.ini` instead.
  On first launch (no ini) it is seeded from `notes.ini.defaults`
  next to the binary (committed; empty `db_dir` = default DB location).
  The live ini is gitignored — its rewrites drop comments and carry
  per-machine values.  (It was NOT actually ignored until 2026-08: the
  .gitignore still named the pre-2026-07 `blue_notes`/`blue_notes.ini`, so
  the binary and a machine-specific `db_dir` were committed.  A rename must
  update .gitignore too.)  The rename's ini pickup (`config_adopt_legacy()`,
  which republished every `[records]` key under `[notes]`) was REMOVED once
  the sole install had been adopted — a stray `records.ini` is now simply
  ignored, which means the app falls back to `notes.ini.defaults` and loses
  the configured `db_dir`.  That is the failure mode any future rename of
  this file must budget for.
  Keys: `db_dir`, `code_copy_button` (`1|0`),
  `code_line_numbers` (`1|0`), `native_menubar` (`1|0`),
  `backup_enabled`/`backup_dir`/`backup_interval_min`/`backup_keep`/
  `backup_source_stamp` (the rotating backups — see `backup.h` for all
  five),
  `sidebar_counts` (`1|0`, default 0 — folder/tag counts in the
  sidebar),
  `sidebar_fit_content` (`1|0`, default 1 — the sidebar sizes itself to the
  rows ON SHOW: expanding a folder widens the divider so the revealed rows
  are not ellipsized, collapsing one gives the width back
  (`sidebar_fit_apply`, symmetric by deliberate choice — while the setting
  is on the divider belongs to the app, so a width the user dragged does
  not survive the next expand or collapse.  That is the deal the setting
  makes, and it is ON by default — turn it off to keep a hand-set width;
  unticking leaves the width where the last fit put it rather than
  restoring anything).  Bounded by
  `SB_FIT_MIN_WIDTH` (also the startup fit's floor — ONE constant, not two
  spellings of 160) and `SB_FIT_MAX_PERCENT` of the paned, so neither a
  library of short names nor a deep branch can make the other pane
  unusable.  Triggered by the flattened GtkTreeListModel's `items-changed`
  (a row expanded or collapsed, or a rebuild) and by the list's `map`
  (the one-shot startup fit, whatever the setting says), both through
  `sidebar_fit_queue`, which runs the fit from the frame clock's
  AFTER-PAINT, once: the list view creates and garbage-collects its row
  widgets in its size_allocate, which the frame clock runs on the next
  frame, so an idle measured the OLD rows (a collapse never narrowed the
  pane; the startup idle saw a 27 px placeholder).  The measurement is
  the list view's OWN natural width: a list view realizes only the rows
  on screen and an ellipsizing label's natural width is its full text, so
  `gtk_widget_measure` IS "the widest row the user can see" — the Pango
  walk of the model went with the tree view.  The scrolled window's
  vertical scrollbar is added when it shows, so nothing reads the current
  allocation), `first_line_title` (`1|0`, default 1 — treat line 0 as the
  note's title: the editor CENTERS it and renders it heading-sized,
  unconditionally and whatever it holds, so a line becomes the title just
  by landing on line 0 (delete the title line and the body line under it
  takes over).  Both are EDITOR-ONLY derived tags (`on-title-center`,
  `on-title-size`), never serialized — nothing is written into the note,
  which is exactly what lets the setting turn the look off again.  Off = a
  plain left-aligned body line; line 0 still supplies the note title
  either way.  Applies live via `on_editor_title_refresh_all`.  The old
  auto-H1 behaviour (real, SERIALIZED H1 on the first line of a brand-new
  note, `ed->auto_h1`) was removed with this — it could not be undone by
  the setting, and it stacked with the derived scale.  A stored H1 on line 0
  (applied by hand, or by a build predating all this) is harmless: the
  derived size stands down there (see quirk 20), so it looks identical —
  the only tell is that turning the setting OFF leaves that one line big.
  Replaced the old
  `first_line_h1` key 2026-08 — a stale `first_line_h1=` line in an
  existing ini is simply ignored),
  `compact_editor_toolbar` (`1|0`, default 1
  — collapse the editor's H1/H2/¶ buttons into an "Aa" Styles menu
  button and the list buttons into a "≡" Lists one; applies live via
  `on_editor_rebuild_toolbars_all`), `touch_assist` (`1|0`, default 0 =
  DISABLED — GTK's touch aids in the GtkTextViews and entries that
  remain (the note view is drawn and has none): the teardrop drag
  handles under text selections/the cursor and the selection magnifier,
  which some Linux input stacks (VM tablets) show for plain mouse input;
  GTK has no API for them.  One lever, live: CSS in
  `on_app_apply_touch_assist` (`cursor-handle` collapsed,
  `popover.magnifier` transparent).  The GTK3 build also set
  GDK_CORE_DEVICE_EVENTS=1 at start to suppress the tap cut/copy/paste
  bubble; GTK4 has no equivalent, so nothing about this setting needs a
  restart and the UI no longer says so), `image_viewer` (program
  path; unset = system default),
  `media_win_w`/`media_win_h` (last media-window size, same contract as
  the search window's),
  `search_win_w`/`search_win_h` (last search-window size, the default
  for the next one), `editor_win_w`/`editor_win_h` (default editor
  window CLIENT size, 640×509 when unset — read at editor open only,
  deliberately NOT written back on resize, unlike the search window's),
  `statusbar_db_path` (`1|0`, default 1 — prefix the
  folder path in the library/editor status bars with the DB file's path,
  formatted by `on_app_location_text`; applies live from Settings),
  `statusbar_note_id` (`1|0`, default 0 — show "id:N" at the right edge
  of each editor's status bar; the label is no-show-all so its updater
  owns visibility; applies live via `on_editor_status_refresh_all`),
  `show_done_actions` (`1|0`, default 1 — list completed items in the
  library's Action Items view; hidden mode also drops a row the moment
  its checkbox is ticked; applies live via the full notify),
  `list_columns` (list-view column layout,
  `key:vis` pairs in display order, default
  `path:0,title:1,modified:1,created:0`
  — written on every header drag/toggle, applied at window
  construction; right-click a column header for the show/hide menu),
  (`list_autofit` was REMOVED 2026-09-16 with the tree view: a
  GtkColumnView sizes every column to its content and gives the expanding
  Title the rest, which is what autofit measured by hand; a stale line in
  an existing ini is simply ignored),
  `list_density_comfortable` (`1|0`, default 1 — Comfortable list
  density: tall rows with a bold title and a small dimmed preview of the
  first body-text line after the title, rendered by `on_title_bind`, the
  preview dimmed by CSS opacity rather than a fixed colour so it stays
  readable on the selection highlight; applies live via
  `notify_notes_changed`). The old DB settings table is
  GONE (dropped from the schema and the live DB 2026-07); all
  preferences live in the ini.
- **Custom DB location** (shared-folder support) lives in the CONFIG
  FILE `notes.ini` NEXT TO THE BINARY (`[notes] db_dir=`;
  resolved from argv[0] by `on_app_config_init()`, which must run before
  any config read — main() calls it first thing), never in the DB.
  `on_app_switch_database()` switches live: closes all editors (flushing
  saves), swaps the handle, copies the current file to the target if no
  notes.db exists there (or overwrites it at the user's choice);
  either way the original file is deleted on success, persists, refreshes
  the library. Failure reverts to the old DB. If the configured DB can't be opened at
  startup, main.c ERRORS OUT — deliberately NO fallback to the default
  location: a silent fallback once made a user's notes "disappear" and
  strands writes in the wrong file (the trigger was a relaunch racing
  the dying instance's final flush past the 5 s busy timeout). One
  configured database, or a clear error. When no notes.db EXISTS at
  the expected location (first launch / emptied dir),
  `startup_first_run()` asks — "Open a notes.db File" (persists the
  new db_dir) or "Create a New notes.db" — instead of silently creating
  an empty DB.  **RENAMING OR MOVING THE DB DIRECTORY BY HAND puts you
  here**: `db_dir` still names the old path, nothing is found, and the
  Welcome dialog appears — answering "Create a New notes.db" at that
  point leaves the real database orphaned at its new path.  Fix the
  `db_dir` line in the ini (or use "Open a notes.db File", which
  persists it) rather than creating one.
- **Folder emoji** (`folders.emoji` TEXT, default `''`): each folder
  can have an optional emoji set via its Info dialog (right-click a
  folder → Info, or via New Folder). GTK's built-in emoji chooser opens
  on click; the chosen emoji is stored in the DB and displayed as a
  sidebar prefix with a two-space separator (`🎉  Folder Name`).
  `on_db_folder_get/set_emoji` in db.c read/write the column;
  `add_folder_rows()` in library_window.c builds the display string.
  A migration (`ALTER TABLE folders ADD COLUMN emoji TEXT NOT NULL
  DEFAULT ''`) backfills existing databases transparently.
- **Trash is a soft-delete flag, not a folder**: `notes.trashed` /
  `folders.trashed` columns + the `trash_folder_ids` view (recursive
  closure — only the TOP deleted folder is flagged; its subtree stays
  attached and is implicitly trashed via the view). folder_id/parent_id
  are untouched by deletion — they ARE the restore location; restore
  clears the flag and re-parents to top level only when the original
  location is itself still trashed. Moving notes (`on_db_notes_move`)
  always clears the flag (drag out of Trash = restore-to-folder). All
  normal listings/counts filter through `NOTE_VISIBLE_SQL`; search's
  All-scope uses `on_db_note_list_all(db, TRUE)` to keep deleted notes
  findable; export/CLI pass FALSE. CLI note/folder delete TRASHES by
  default (one bulk trash call for notes); `--permanent` deletes
  outright and `note restore` un-trashes — safe for agent use.
  Sidebar: "Pinned Notes" on top (only while any are pinned; the
  selection-restore fallback reads the FIRST row's kind from the model
  rather than assuming All Notes), then "All Notes" (`SB_KIND_ALL`,
  newest-first), then "Action Items", then the folder tree and Tags,
  and the "Trash" section at the bottom only while non-empty
  (`SB_KIND_TRASH`, trashed folders as `SB_KIND_TRASH_FOLDER` children;
  `on_db_folder_list_trashed` fed those rows through
  `run_folder_query`, which reads its columns POSITIONALLY as
  `FOLDER_COLS` — the query hand-wrote a five-column list instead, so
  every trashed folder showed its parent's id as its name and its real
  name as its emoji, in the sidebar as well as in `trash list`.  Fixed
  2026-09 by using the macro: never hand-write that column list);
  in trash views the Delete paths turn permanent (with confirm) and the
  note menu becomes Open/Restore/Delete Permanently; GUI note/folder
  deletes elsewhere just trash with a status message, no dialog.
- **Action items are '!' lines**: a line whose FIRST character is '!'
  (outside code blocks; anchors occupy the first slot like a character)
  is an action item — text = rest of line trimmed, DONE = the whole rest
  struck through (ON_FMT_STRIKE, which serializes; that is the persisted
  state).  The one definition lives in `on_note_extract_actions`
  (serialize.c, a cheap record walk like body_text extraction).  The
  editor tints action lines blue via the derived, editor-only
  `on-action` tag (priority 0 so #tags keep their orange; re-derived on
  insert/delete/paragraph-format/load/undo like the emoji padding) —
  nothing about "actionness" is stored in BNBF.  The `action_items`
  table (note_id/ord/text/done/due/uid, ON DELETE CASCADE) is a queryable
  mirror like note_tags: editor_save rewrites it only when the
  extracted set differs from `ed->last_actions` (full library notify
  then, light otherwise); CLI saves sync unconditionally; a one-time
  backfill for pre-feature notes is gated by `PRAGMA user_version`
  (`on_app_actions_backfill`, run after every long-lived `on_db_open` —
  GUI start, CLI, db switch).  Library: "Action Items" sidebar
  row directly under All Notes, ABOVE the folder tree (visible while
  items exist; optional count = OPEN items) shows a third notes-pane
  stack child ("actions"): untitled checkbox column + "Action" text
  column (done rows struck).  Toggling
  writes the db row, then `on_editor_action_set_done` strikes/un-strikes
  the '!' line's text — in the live buffer + autosave when the note is
  open, on the note's OnDocument (`on_document_action_strike/due/text`)
  with an immediate save otherwise (ord = position among the note's
  REAL action lines; bare "!" lines don't count).  DUE DATES live
  in the line text as a trailing "due <date>" — ISO "YYYY-MM-DD" is the
  written form, the parser (`on_action_split_due`, shared by extractor
  and editor like on_list_prefix_chars) also reads "M/D/YY[YY]"; the
  LAST word-boundary "due" that parses wins, so "send due diligence
  report due 12/31/26" keeps its text.  A line that is only "! due X"
  is no item.  Double-clicking the Due Date CELL opens a GtkCalendar
  dialog → `on_editor_action_set_due` rewrites the suffix (appended
  text inherits the item's strike state so done items stay done);
  double-clicking elsewhere opens the owning note.  The view follows
  the notes list's column conventions — the layout machinery is
  view-generic (`view_columns_persist/apply`, config key + count +
  default carried as object data on the VIEW, header buttons carry
  "on-view"); `action_columns` ini key, default `done:1,action:1,due:1`;
  headers sort (Action alpha, Due soonest-first with undated last, Done
  by state).  The Due Date cell is tinted by urgency via a cell data
  func (draw-time, so it rolls over at midnight): overdue red, today
  dark yellow, ahead green — darkened for the striped row backgrounds;
  no-due rows must reset "foreground-set" (shared renderer).
  `due` is in the CREATE TABLE (so new databases have it immediately)
  and a guarded ALTER migration backfills existing databases on open.
- **An action item's STABLE identity is `action_items.uid`** — `ord` is a
  POSITION and shifts whenever a '!' line is added or removed, so it can
  never be a reference an external mirror (the Lists app) holds onto.
  The uid is assigned once, invisible to the user, and NOT stored in the
  note text (that was considered and rejected: a token in the prose
  leaks into `note cat`, both exports, the body_text search cache and the
  list previews, duplicates itself on copy/paste, and is deletable by
  ordinary editing).  Since `on_db_note_set_actions` rebuilds a note's
  rows DELETE-then-INSERT, an AUTOINCREMENT column would be reissued on
  every save; instead the OLD rows are read first and matched against the
  new set in four passes, strongest evidence first — identical text (an
  item that only moved), then the live editor's per-line mark hint (the
  ONLY signal that survives a reword), then the same ord (the headless
  reword), then a fresh id from the `action_uid_seq` one-row high-water
  mark (only ever incremented, so a retired uid is never handed out
  again).  Each pass completes before the next, so a strong match cannot
  lose its row to a positional guess made earlier in the list.  The
  editor's hint is the BLOCK: every '!' block carries its item's uid
  (`OnBlock.action_uid`, seeded at load by `on_note_view_action_marks_sync`,
  re-set after any save that rewrote the table), and a block struct
  survives every edit of its text — a rewording keeps its identity with
  nothing to protect (the GtkTextMark-and-prune machinery of the buffer
  era is gone with the buffer).  Undo restores a removed block WITH its
  uid; a cut-and-paste reorder makes a new block (uid 0) and is caught by
  the text pass instead: the two signals cover each other's blind spot.
  Migration: `uid` is in the CREATE TABLE plus a guarded ALTER,
  and `on_app_action_uids_backfill` (user_version 3) fills existing rows.
  That backfill ALSO re-runs whenever any row has uid 0 — an older build
  writing to an already-migrated database inserts without the column, and
  the version stamp alone would leave those rows unidentified forever;
  the probe is an indexed existence check, so the normal case is free.
  NOTE the index on `action_items(uid)` is created AFTER the ALTER
  migrations (with the Trash view), never in the schema string: indexing
  a column that does not exist yet fails the whole batch, which makes
  `on_db_open` return NULL and the app refuse to start on every existing
  database.
  `grid_pref` restores list/grid when leaving the view.
- **`on_db_backup_to()`** (db.c) uses SQLite's online backup API on the
  live DB.  It is THE copy path, shared by the CLI's `notes backup
  FILE.db` and by the rotating backups (`backup.[ch]`); the File menu
  backup/restore items were removed.
- **The startup integrity check runs EVERY launch and has no off switch**
  (`on_app_db_health_start()`, called by `startup_finish` in main.c AND by
  File → Open Database File…, over `on_db_health_check_async()`: a worker
  thread with its own read-only connection, so the library window is up
  while `PRAGMA integrity_check` walks the file — 1.7 s per 600 MB on a
  warm local disk, longer from a cold iCloud Drive file, and it used to be
  all of that before the window appeared.  The verdict lands on the main
  thread through an idle: a status line, the warning dialog if anything is
  wrong, and `app->notify_db_health` for the Settings plate, which shows
  "Checking…" with a white LED meanwhile.  Closing the connection abandons
  a running pass — the job outlives it and frees itself).
  The `db_integrity_check` ini key that once gated it was REMOVED 2026-09
  along with the Settings checkbox: a health check that can be switched
  off can only ever report silence that means "not looked", which is the
  one answer it must never give.  A stale `db_integrity_check=` line in an
  existing ini is simply ignored.  The verdict is held IN MEMORY on the
  connection (`OnDatabase.health`, read back with `on_db_health()`) and
  deliberately never written down — a verdict stamped into the file would
  make a health check MUTATE THE FILE IT WAS MEASURING, so the SHA-256 the
  Settings window shows beside it would move on every press of Update.
  `db_check_pragmas()` is the ONE spelling of the two PRAGMAs, shared by
  that and by `on_db_verify_file()` (a file nothing has open); it tells
  "found problems" apart from "could not run" by whether the checks came
  back with ANYTHING, because a corrupt file both reports damage AND
  returns an error.
- **Moving the database is File → Open Database File… only.**  The Settings
  checkbox that stored a custom folder, and `on_app_switch_database()`
  behind it, were REMOVED 2026-09: they were a second flow that MOVED the
  file rather than opening one, so the same question was answered in two
  places by two different mechanisms.  `db_dir` still exists as an ini key
  — File → Open Database File… → Set as Default is what writes it now.
- **CLI ↔ GUI coexistence is socket-based, not lock-based**: a running
  GUI serves later CLI invocations over a unix socket (`src/ipc.c`), so
  the two never write the DB concurrently. The old in-DB `in_use`
  instance lock and the read-only mode (`app->read_only`, `PRAGMA
  query_only`, `on_app_db_acquire/release`) were REMOVED with that
  change. SIGTERM
  (pkill) destroys all windows so editor autosaves flush and the loop
  ends cleanly.

## Hard-won GTK quirks (do not re-learn these)

**This branch is GTK4.**  The numbered items below were derived against
GTK3; each one's GTK4 status is stated up front, and the GTK4 facts that
replaced the superseded ones are D5–D24 in `GTK4_MIGRATION.md` (short
form in "GTK4 quirks" after the list).  Superseded ones are kept because
they explain shapes that survive in the code.  **Everything about
GtkTextView — quirks 1–4, 9, 10, 17–20, 22, 23 and the GTK4 items
D15–D17, D19–D20, D23–D25, D27, D29–D30 — is HISTORY since 2026-09-15:
the editor is a drawn widget over an OnDocument (BLOCK_MODEL.md) and no
GtkTextView exists in the app.**

| GTK3 quirk | On this branch |
|---|---|
| 1, 2 (text-window children) | Superseded: overlays in buffer coordinates, GTK adds the top margin every allocation (D6) |
| 3, 4 (copy link size / line y) | Still apply |
| 5 (Retina blur via device scale) | Superseded: textures carry their pixels; grid thumbnails are 1× (D18) |
| 6, 7, 9, 10 | Still apply (6: a toolbar is a `GtkBox.toolbar` now) |
| 8 (multi-row drag) | Superseded: the row's drag source reads the selection model, D33 |
| 11 (clearing a store zeroes the scrollbar) | Mostly gone: a list view keeps its scroll anchor across one items-changed; `scroll_keep_queue` stays as the belt for a full splice, D33 |
| 12 (emoji padding) | Changed: the EMOJI only, D24 |
| 13 (drop protocol) | Superseded twice: GtkDropTarget (D5), now one per sidebar ROW with a CSS-class indicator, D33 |
| 14 (expand_all, drag icon) | Superseded: expansion is GtkTreeListRow state restored by kind+id; `gtk_drag_source_set_icon` in `prepare` |
| 15 (multi-select collapse on press) | GONE: GTK4 list items select on RELEASE (gtklistfactorywidget.c), so a drag from a selected row keeps the selection with no veto, D33 |
| 16 (type-ahead search column) | Gone with the tree view |
| 17–20 | Still apply |
| 21 (window placement) | Gone with the feature |
| 22 (text-window cursor owner) | Superseded: `gtk_widget_set_cursor_from_name` per widget |
| 23 (backdrop grey after focus grab) | Design kept (panel never takes focus); not re-measured on GTK4 |
| 24, 25 | Still apply |

1. **Text-window children are BUFFER-anchored.** Children added via
   `gtk_text_view_add_child_in_window(GTK_TEXT_WINDOW_TEXT)` take their
   position in buffer coordinates — they ride scrolling at 1x on their
   own. The view's top margin is re-added ONLY on the initial
   allocation of a freshly added child; positions set later via
   `gtk_text_view_move_child()` land as-is (verified on screen), so add
   at 0,0 and position everything through move_child with plain buffer
   coordinates. Probing tip: use `gdk_window_get_origin` on a realized
   window — offscreen pixel-scans do NOT composite these children.
2. **Never reposition those children on scroll.** A `move_child()`
   issued while scrolled doesn't take effect until the next
   validate/allocate cycle, so scroll-driven "corrections" (especially
   ones computed via `buffer_to_window_coords`, which double-apply the
   scroll) land late and misalign the widget by the scroll delta.
   Reposition only when content or geometry changes: rebuild on buffer
   changes (idle-coalesced) and view `size-allocate`. No off-screen
   hiding is needed — they scroll and clip naturally.
3. The floating copy button must be **pinned to an exact size in CSS for
   all states** (`button, button:hover, button:active { min-width/height;
   padding:0 }`) — theme hover styling otherwise changes its allocation
   and it jumps under the pointer. Position math uses the constant
   `CODE_BTN_SIZE`, never live allocations.
4. Anchor the button's y to `gtk_text_view_get_line_yrange()` (line top =
   where paragraph-background shading starts), not the char rect (which
   sits below pixels-above-lines).
5. **Retina blur**: raw pixbufs render 1 buffer-pixel = 1 logical px.
   Anything that must be sharp goes through cairo surfaces with device
   scale: editor images, grid thumbnails
   (`cairo_surface_set_device_scale`, list-store column type
   `CAIRO_GOBJECT_TYPE_SURFACE`, icon-view pixbuf renderer bound to the
   "surface" attribute), and toolbar icons (`on_app_icon_image_sized`
   rasterizes SVGs at size × monitor scale factor and wraps them via
   `gdk_cairo_surface_create_from_pixbuf`).
6. **Toolbars are ICONS-ONLY, and that is not a setting.** Every toolbar
   sets `GTK_TOOLBAR_ICONS` at build time; buttons come from
   `on_app_tool_item_new` (icon file, or a Pango-markup glyph as the
   icon_widget when there is no file). GtkToolbar natively supports
   TEXT/ICONS/BOTH and the app once exposed all three per family
   (`ON_TOOLBAR_LIBRARY/EDITOR`) as two Settings dropdowns, two ini keys
   (`toolbar_style_library`/`toolbar_style_editor`) and a right-click
   style menu on any toolbar, all backed by a registry of live toolbars in
   `OnApp`. ALL of that was removed 2026-09 — text-mode toolbars were
   never what the app wanted, and the icons carry the design. Stale
   `toolbar_style_*` lines in an existing ini are simply ignored. `git log`
   has the registry if a live restyle is ever needed again.
7. **Editor letter buttons (B/I/U/S) are markup glyphs on purpose** —
   elementary's symbolic SVGs are 16px light-grey and look fuzzy/washed
   next to Pango-rendered glyphs. Icon field NULL → fallback markup is
   the primary look. H1/H2/¶/•/1./{ } are glyphs too.
8. Multi-row drag to a folder: GtkTreeView drags a single
   GTK_TREE_MODEL_ROW; on drop, if the dragged note is in the current
   multi-selection, move the whole selection.
9. Paragraph-style tags must cover the trailing newline (see
   `line_span()`) so typing at line end inherits them; list items carry a
   literal "• "/"N. " prefix plus an indent tag; Enter continues lists,
   Enter on an empty item ends them; numbered blocks renumber.
10. Inline typing follows `ed->inline_flags` (word-processor model),
    enforced in the after-handler of `insert-text` for insertions ≤2
    chars (longer pastes keep their own tags).
11. **Clearing a tree/list store zeroes its view's scrollbar.** Every
    model rebuild (refresh_sidebar, refresh_notes) must capture the
    scrolled window's vadjustment value first and restore it via
    `scroll_keep_queue()` (idle-deferred so the rebuilt view re-validates
    its height before the value is clamped). The sidebar always
    restores; the notes pane only when re-showing the same selection
    (`shown_kind/shown_id`) so navigation still starts at the top.
12. **Emoji padding is MEASURED, and goes on the emoji only.**  Apple
    Color Emoji drawn through cairo's CoreText path inks WIDER than the
    advance Pango reserves (13 pt: ink 22 px over a 17 px advance, all of
    it to the right), so the next character lands on the glyph.
    `on_emoji_pad(ctx)` (app.c) lays out a sample emoji in the caller's
    font and returns `2 × (overhang + ON_EMOJI_GAP)` of letter spacing —
    Pango puts half on each side of the emoji's run — or 0 when the font
    fits (Linux Noto, the fontconfig backend), in which case nothing is
    tagged.  No `#ifdef __APPLE__` any more: measurement decides.  ONE
    rule, two renderers: the drawn view's letter-spacing attribute
    (doc_layout.c `text_layout_new`, `L->emoji_pad` from the view's
    context; the caret after a line's LAST emoji moves `emoji_caret_pad`
    = half the spacing less `ON_EMOJI_GAP` right, because Pango drops the
    trailing half at a line end and the glyph overdraws by exactly that
    much) and `on_markup_escape_emoji(text, pad)` for everything drawn
    from markup — the notes list's title + preview, the grid titles and
    the Action Items text (`emoji_text_cell_func`, pad = `lw->emoji_pad`
    from the window's context).  Until 2026-09-15 the rule was a fixed
    5 px over the emoji AND the following character: that put 2.5 + 2.5
    in the gap — exactly the overhang, so no clearance at all — and the
    follower's other half opened the word after it ("W orld"); the gtk4
    branch's interim fixed 9 px (D24) was the same finding, unmeasured.
    The padded span must run through `on_is_emoji_joiner` characters
    (U+FE0F etc.): a letter-spacing boundary is an itemization boundary,
    and a variation selector split off from its base shapes as a visible
    hex box (measured: "❤️" rendered as a heart and a "FE0F" tile).
    `on_is_emoji_char` / `on_is_emoji_joiner` (app.c) are THE detectors —
    doc_layout.c's private copies went with the merge.
    The only other platform-specific code is the
    `native_menubar` checkbox and its in-window fallback bar (`__APPLE__`,
    see "Actions, menus and shortcuts"); everything else is portable GTK4.
13. **A custom GTK_TREE_MODEL_ROW drop handler must own the WHOLE dest
    protocol.**  GtkTreeView's default `drag-motion` handler validates
    row drops by requesting the drag DATA on every motion
    (`set_status_pending` + `gtk_drag_get_data`), so
    `"drag-data-received"` fires repeatedly MID-DRAG.  On quartz the
    reply arrives before the release, so a received-handler that treats
    every delivery as a drop runs with stale coordinates (0,0 → the top
    sidebar row) and `gtk_drag_finish()`es the drag while the button is
    still down — drops only land when an X11-style late reply slips
    past the release.  Fix (see the sidebar in library_window.c):
    connect `drag-motion` (compute + validate the target yourself,
    `gtk_tree_view_set_drag_dest_row` + `gdk_drag_status`, return TRUE
    to block the class closure), `drag-leave` (clear the indicator),
    and `drag-drop` (request the data, return TRUE); then
    `drag-data-received` fires exactly once, at drop time, with real
    coordinates.  Costs the built-in drag auto-scroll/auto-expand.
14. **`gtk_tree_view_expand_all` after every model rebuild re-expands
    folders the user collapsed.**  refresh_sidebar snapshots the
    expanded rows before the clear (keyed kind+id — paths shift when
    folders move) and restores that state in its selection-restore walk;
    only the first population expands everything.  Custom drag icons go
    on with `g_signal_connect_after("drag-begin")` — the class handler
    sets its own row-snapshot icon in the class closure, so a normal
    connection gets overridden.

15. **GTK 3.24's GtkTreeView collapses a multi-selection on PRESS.**
    Its multipress gesture does CLEAR_AND_SELECT on any unmodified
    primary press — no drag deferral — so dragging a multi-selection is
    impossible out of the box (GtkIconView is fine: it defers via
    `last_single_clicked`).  You can't just consume the press: the
    multipress AND row-drag gestures both run in the BUBBLE phase, so a
    TRUE from a button-press handler kills drag initiation too.  Fix
    (notes list): on press over an already-selected row with ≥2
    selected, install a veto select-function; a drag-begin lifts the
    veto keeping the selection, a plain button-release lifts it and
    applies the collapse via gtk_tree_view_set_cursor.

16. **GtkTreeView type-ahead search auto-picks a useless column**:
    `gtk_tree_view_set_model` sets the search column to the first model
    column transformable to string — our stores lead with the int64 id,
    so typing in a focused view popped a search box that matched
    nothing.  Every tree view disables it
    (`gtk_tree_view_set_enable_search(view, FALSE)`); to bring it back
    usefully, point `gtk_tree_view_set_search_column` at a text column
    (e.g. NL_TITLE) instead.

17. **Anchored children sit with their BOTTOM on the text baseline**, so
    a widget taller than the font's ascent (the task checkboxes) rides
    visually high next to its line's text.  Widget margins cannot be
    negative and CSS padding on the `check` node can only move the
    indicator UP relative to the baseline, never down — the working
    lever is a negative `rise` on a GtkTextTag covering the anchor
    CHARACTER (editor-only `on-check-drop` tag, −3 px, applied in
    attach_checkbox_widget): GtkTextView honors Pango rise when placing
    child segments.  A theme-padding-stripping CSS pin stays on the
    button itself so the box is the bare indicator on themes that do
    pad it (macOS Adwaita already doesn't).

18. **`notify::cursor-position` fires INSIDE the insert-text class
    handler** — after the character lands in the buffer but BEFORE any
    after-handlers run.  So a cursor-moved handler that adopts the style
    of the char left of the cursor reads the brand-new, still-untagged
    character and wiped `ed->inline_flags` before the insert
    after-handler could apply it (broke arming bold with no selection:
    Ctrl/Cmd+B, then type).  Fix: an insert-text BEFORE-handler sets
    `ed->typing_insert` for ≤2-char (typed) insertions; on_cursor_moved
    skips style adoption while it's up; the after-handler clears it.
    Real navigation (clicks, arrows) still adopts.

19. **A tag can't style an EMPTY line, so the caret there can't be styled
    by tags at all.**  `gtk_text_buffer_apply_tag` over a zero-length span
    is a silent no-op — which is why the old auto-H1 re-applied itself per
    keystroke instead of pre-styling the line.  The caret's height and
    x-position on an unwritten line therefore come from the view's
    DEFAULTS, and the title line needs both (see `title_line_sync`):
    justification via
    `gtk_text_view_set_justification`, font size via a style class the
    function toggles on the view (`textview.on-title-empty
    { font-size: 160% }`, matching ON_TAGNAME_H1's 1.6 scale).  Enlarging
    the whole view is safe ONLY because it is done exclusively while the
    buffer is empty — an empty buffer IS line 0 and nothing else.  The
    class must select the `textview` node, NOT its `text` child:
    `gtk_text_view_set_attributes_from_style` reads the default font off
    the widget's own style context and takes only letter-spacing from the
    text node.  A line that is empty but has a NEWLINE (a blank first
    line) is the in-between case: its newline is the one character it owns,
    so spans there run THROUGH the newline while spans on a line with text
    stop before it — covering the newline would let the next line inherit
    the tag from text typed after it.

20. **GtkTextTag "scale" values MULTIPLY when tags overlap** —
    `_gtk_text_attributes_fill_from_tags` does `dest->font_scale *=
    vals->font_scale` per tag, which is why an H1 line that is also H2
    renders at 2.08x.  So a DERIVED scale tag must never be laid over text
    that might already carry a real one: `title_line_sync` applies
    `on-title-size` only where line 0 has no ON_FMT_PARA_MASK style of its
    own (H1 there is already title-sized; H2/code/list is a style the user
    chose).  Weight and justification don't compound this way — only
    scale does.

21. **What `gtk_window_move()` positions is PLATFORM-DEPENDENT, and window
    gravity is not the way out.**  Measured on GTK 3.24: on quartz the
    coordinate is the CLIENT origin (the frame extends the titlebar's 28 px
    ABOVE it — `gdk_window_get_frame_extents` on a realized-but-unmapped
    window reports `y = -28`), while X11's documented behaviour is the
    frame's top-left.  `GDK_GRAVITY_SOUTH_EAST` looks like the fix —
    "move the bottom-right corner to this point" — and IS honoured on
    quartz, but GTK computes it from the client size it knows before
    mapping, so a request for `corner − 12` landed the frame flush IN the
    corner with the margin silently swallowed.  Reliable recipe (see
    `editor_place_bottom_right`): keep default gravity, move using the
    CLIENT size for the first placement, then correct ONCE in a `map-event`
    handler from the real `gdk_window_get_frame_extents` — shift by the
    leftover delta via `gtk_window_get_position` + `gtk_window_move`, which
    share a coordinate space whatever the convention, and disconnect the
    handler.  On macOS the residual is 0, so nothing visibly moves.
    Positions must be measured against `gdk_monitor_get_workarea`, never
    the monitor rect: the work area already excludes the menu bar, Dock and
    Linux panels.

23. **A GtkTextView that loses the focus renders its text GREY, and on
    quartz that grey rendering OUTLIVES the thing that took the focus.**  The
    modal image viewer (`image_viewer.c`) used to `gtk_widget_grab_focus()`
    its own panel so its keys would reach the host window's handler.  Closing
    it then left the whole editor's text grey — looking exactly like an
    unfocused window — until the user clicked to another window and back.
    Measured with the panel closed: GTK reports the focus restored to the
    view (`state-flags 0x80 -> 0xa0 FOCUSED`), resolves the text colour to
    pure black, and emits a FULL-CLIP draw.  The screen keeps the grey
    anyway.  Nothing at the GTK level clears it — not
    `gtk_widget_queue_draw()` on the view, not
    `gdk_window_invalidate_rect(win, NULL, TRUE)` on the toplevel, not
    `gtk_widget_reset_style()` on either the view or the window; only
    SCROLLING (which re-lays-out lines, and fixes it region by region as you
    go) or a real toplevel activation change.  Grey is provably the BACKDROP
    colour: the same instrumentation shows `state=0xc0 BACKDROP` resolving to
    `0.20,0.20,0.20` while `FOCUSED` gives `0.00,0.00,0.00`.
    **So do not repair it — do not create it.**  The panel is
    `can_focus FALSE`, never grabs the focus, and the host window's
    `key-press-event` handler (which runs before GtkWindow forwards to the
    focus widget) serves its keys off whatever still has focus.  The host
    then MUST swallow every other key while the panel is open, or typing
    would edit the note blind behind it.  The corollary that bit next:
    **nothing inside such a panel may be a GtkLabel `<a>` link**, because a
    link label owns an input-only window and grabs the focus when clicked —
    reintroducing the bug on the first Next.  Hit-test plain labels from the
    panel's own button-press handler instead (same remedy as quirk #22),
    and serve their hover cursor from the panel's `motion-notify-event`
    (`img_motion` -> `img_cursor`): the panel's event box genuinely owns
    its GdkWindow, so unlike quirk #22's text window there is no rival
    owner — but it must be given `GDK_POINTER_MOTION_MASK` explicitly,
    since a GtkEventBox asks only for BUTTON_MOTION.  `img_nav_delta` is
    THE one reading of the nav row, shared by the press and the cursor,
    so the pointer can never promise a step a click will not deliver.

24. **A row of two small GtkLabel links has dead gaps, and a click in one is
    silently lost.**  The viewer's "Previous | Next" started as ONE GtkLabel
    carrying both `<a>` links; a press landing on the label but in the
    spacing, on the separator, or just past a word's last glyph activated
    nothing and did not close the panel either, so stepping through images
    intermittently took two clicks.  Word-exact hit-testing has the same
    hole.  The row is now split by MIDLINE — the whole row is claimed, left
    half steps back, right half forward — so there is no gap to miss and a
    near-miss can never dismiss the picture.

22. **The text window's CURSOR has exactly one owner, so a clickable
    region inside a GtkTextView must be served from the VIEW's own
    handlers, not from a wrapper widget.**  The code blocks' "copy" links
    were once a GtkLabel inside a `gtk_event_box_set_visible_window(FALSE)`
    GtkEventBox whose realize handler set the "pointer" cursor, on the
    theory that the event box owns a GdkWindow the cursor could be scoped
    to.  It does not: `visible_window` FALSE clears the widget's
    has-window flag, so `gtk_widget_get_window()` returns the PARENT —
    the view's bin window (verified: `has_window=0`, and get_window ==
    `gtk_text_view_get_window(GTK_TEXT_WINDOW_TEXT)`).  The realize
    handler was therefore setting the cursor for the WHOLE text area, and
    `on_view_motion_notify` (connected after, so it wins) put the text
    cursor straight back on the next motion — the hand cursor never
    appeared anywhere.  Motion reaches the view even over the event box
    because GtkEventBox's input-only window asks for BUTTON_MOTION_MASK
    only, never POINTER_MOTION_MASK.  So: the links are plain GtkLabels
    with no input window at all, and both the click
    (`on_view_button_press`) and the hover cursor (`on_view_motion_notify`)
    hit-test them via `code_link_at_view_pos`.
    Hit-test against the child's **ALLOCATION**: in-window children are
    POSITIONED in buffer coordinates (quirk #1), but GTK re-allocates them
    as the view scrolls (measured: `move_child(300,200)` with a 12 px top
    margin allocates at y=212, and at y=92 once scrolled 120), so an
    allocation is always in the same window coordinates `event->x/y`
    arrives in — no `window_to_buffer_coords` conversion, and correct at
    any scroll offset.  Consume the press (return TRUE) so the caret does
    not move under the link.

25. **`g_ptr_array_sort` hands its comparator POINTERS TO THE ELEMENTS**, so
    `g_ptr_array_sort(a, (GCompareFunc)g_strcmp0)` over an array of strings
    compares the POINTER VALUES as if they were the strings.  The cast
    silences the compiler and the result looks like a plausible ordering, so
    nothing complains — it bit the backup prune (`backup.c`), which picks its
    victims by that order: MEASURED on a four-file rotation with keep=3, it
    deleted the second-oldest and kept the oldest.  Write a comparator that
    dereferences (`backup_name_cmp`), or use `g_ptr_array_sort_values`, whose
    whole point is that it passes the elements.  (This is not GTK-specific
    and applies anywhere in the app.  The sister Tasks app's `src/backup.c`
    had the identical line — it is where this module was ported from — and
    was fixed the same day; its CLAUDE.md carries the same note as gotcha
    31.)  **The rule: never cast a function to `GCompareFunc` to make a
    sort compile.**  If the types do not already match, the comparator is
    wrong.

### GTK4 quirks (all measured on 4.22 — details and reproducers in GTK4_MIGRATION.md)

- **Tooltips are `on_app_set_tooltip`, never `gtk_widget_set_tooltip_text`**
  (D32): GTK reuses one popup surface for every tooltip and the macOS
  backend's tile layer does not follow a resize of that surface while it
  is hidden, so a tooltip shown within a second of another of a different
  size was drawn at the OLD width, cut off.  The helper refuses a tooltip
  asked for within 550 ms of the previous one hiding and asks again after,
  so consecutive tooltips come a beat slower and whole.  It also shows
  NO tooltip in a window that is not active (D35): the macOS backend
  picks the surface for a pointer event by its own content-rect hit test,
  so a pointer on an editor's TITLE BAR hovers the library behind it, and
  the library's tooltip popup — a child NSWindow shown with `orderFront:`
  — brought the library up over the editor.  The hover itself (toolbar
  buttons lighting up under a title bar) is GDK's and cannot be helped.
- **Releasing a modifier after a click turns the focus ring ON** (D36):
  GtkWindow remembers the focus widget at a key press and shows
  focus-visible on the release if the focus moved meanwhile — a
  Shift/Cmd-click on another row.  The rows and grid cards have no focus
  ring (library CSS #11); the selection is the keyboard's position.
- **An input method's client widget is set at REALIZE** (D31): the macOS
  method resolves the widget's surface when told, so a widget told at
  construction (no root yet) never gets a key.  `note_view_realize`.

- **GtkColumnView's row widget is not reachable from a column's factory**
  (D33): a controller that should cover the whole row (the note drag
  source, the right-click menu) is installed on EVERY column's cell
  (`note_cell_controllers`); the effect is the same.  The sidebar's
  GtkListView hands the factory the whole row, so its controllers are one
  per row.
- **A double-click is counted by the app, never by `n_press`** (D34):
  the macOS backend stamps a motion event with the buttons held WHEN IT
  IS TRANSLATED, so the tiny drag inside a quick first click can reach
  GTK as a motion with no button down, and GtkGestureSingle RESETS every
  active click gesture on that — the count restarts and the second press
  is a first one.  `on_app_double_click_watch` (app.h) keeps the last
  press outside the gesture and pairs the next by the double-click
  settings; every row/cell that opens on double-click uses it.
- **A GtkListView measures only its REALIZED rows** (D33): the sidebar
  fit reads the list's natural width, which is exactly the on-screen
  rows, but it is meaningless before the list is mapped and laid out —
  queue the first fit from `map`, not from the constructor.
- **`<Primary>` is Control everywhere** (D13); Command is `<Meta>`.
- **A popover parented to a GtkTreeView corrupts its CSS node chain and a
  GtkTextView disposing with a foreign child never returns** (D14):
  context popovers are parented to the window's child box.
- **Drawing over/under text is `snapshot_layer`, in buffer coordinates;
  a `snapshot` override never shows** (D15).
- **No paragraph background on a newline-only line** (D16): the editor
  shades empty code lines itself.
- **An overlay can never be removed from a GtkTextView**
  (`gtk_text_view_remove` warns "is not a child", D17): pool them.
- **One overlay makes every anchored child unclickable** (D29): GTK wraps
  the overlays in a GtkTextViewChild the size of the text area, parented
  LAST, and picks walk last-to-first — a note with a code block (a copy
  link) lost every click on its table cells and images.  The link's
  parent is made non-targetable right after `add_overlay`; the links'
  clicks and hover cursor are the view's own handlers.
- **A paragraph tag on the buffer's LAST line has no newline to cover**
  (D30): `apply_paragraph_format` gives it one, or Enter there breaks the
  block.
- **Joining lines keeps the SECOND line's newline and its paragraph tag**
  (D20): the editor re-asserts the first line's style after a join.
- **A window's `destroy` fires AFTER its dispose has torn the child tree
  down** (D21), the reverse of GTK3: a destroy handler may only touch what
  it holds a reference to.
- **GtkTextView's press handler grabs the focus unconditionally and does
  not claim a plain press** (D23): presses in an anchored child text view
  must be stopped at the child, or the parent takes the focus back.  A
  claim from a CAPTURE-phase ancestor cancels the child's sequences.
- **Pango keeps the run-edge half of letter-spacing** (D24): pad the
  emoji only.  And it drops the spacing at a LINE end, so a trailing
  emoji sits 2 px under the caret until the next character is typed.
- **`GtkTextHistory` stores plain text**: no tags, no anchors.  A rich
  buffer needs its own undo; GTK's is disabled on the note buffer.
- **The `native_menubar` toggle** is `gtk_application_set_menubar(model)`
  vs an in-window `gtk_popover_menu_bar_new_from_model` over the same
  model, macOS only; Linux always renders it in the GtkApplicationWindow.

## Performance decisions

- The media browser never deserializes a note: counting a note's images
  is a record walk that skips PNG payloads, and each thumbnail decodes
  exactly one image at thumbnail size (`on_note_image_nth`, which caps
  the decode via the loader's size-prepared, so a 12 MP screenshot is
  never inflated).  The scan yields every 40 ms and keeps the current
  note's blob across yields, so a note with twenty screenshots is read
  from SQLite once.  Measured on the 1296-note/600 MB database: All
  Notes fills 500 thumbnails in ~30 s with the window fully responsive
  throughout.  The 500 cap is a MEMORY bound, not a speed one — every
  cell holds its own decoded pixels.
- Images reach widgets as textures exactly ONE way: `on_app_texture_for_pixbuf`,
  a memory texture over the pixbuf's own pixels (no copy, no re-decode; the
  bytes reference keeps the pixbuf alive).  The anchored GtkPicture, Copy
  Image, the modal viewer, the media cells and the grid cards all use it;
  `gdk_texture_new_from_bytes` over the cached PNG would decode pixels the
  pixbuf already holds.
- Grid thumbnails render ONLY while grid view is visible (`want_thumbs`
  in refresh_notes; on_view_grid refreshes) — the thumb cache keys on
  updated_at, so without the gate the edited note re-rendered on every
  autosave.  And they render ASYNCHRONOUSLY: refresh_notes only sets
  thumbnails found fresh in the cache; every stale/missing one is queued
  as a ThumbJob (row reference + id + updated_at) and rendered by
  `thumb_fill_idle` in 40 ms time slices.  Rendering them inline once
  hung the GUI ~37 s (measured, 1266 notes / 617 MB): deleting a note's
  last #tag pruned the orphaned tag, the sidebar selection on that tag
  row fell back to All Notes, and the grid rendered every thumbnail —
  every PNG in the DB decoded — in one synchronous pass.
- Sidebar counts come from two GROUP BY maps (`on_db_note_count_map` /
  `on_db_tag_count_map`), not per-row COUNTs — per-query latency hurts
  on shared/network DBs.  The list view's Path column likewise reads
  `on_db_folder_path_map` (all folders in one query, paths built in
  memory), never per-note `on_db_folder_path`.
- Editor saves use the LIGHT notify (`app->notify_note_saved` →
  refresh_notes only): editing a note can't change folder counts, so
  the sidebar isn't rebuilt per autosave/close. The full
  `notify_notes_changed` (sidebar + notes) fires only when the save
  changed the note's tag set — tracked LIVE by `ed->tags_modified`
  (set in tag_capture_end / on_tag_row_activated on creation, the
  before-handler on delete-range when the doomed range touches an
  on-tag span, and the insert after-handler when typing inside one) —
  never by scanning the buffer at save time. note_tags is rewritten
  only when that flag is set. Create/move/delete run in the library,
  which refreshes itself directly; db switch uses the full notify.
- `ed->dirty` (set by editor_queue_autosave, cleared by editor_save)
  gates the close-time flush: closing a window whose last autosave
  already ran skips serialization entirely.
- **Images are never re-encoded — on save OR on export**: the PNG bytes
  are cached on the pixbuf as `"on-png"` GBytes (attached from the blob at
  full-res deserialize, or on the first encode of a pasted/inserted image),
  and `on_image_png_bytes()` is THE accessor — cache hit, or encode once and
  cache.  `on_note_serialize` and `export.c`'s `emit_image` both go through
  it, so the bytes written out are the ones the database holds; the
  exporter and every other offscreen consumer now read `OnBlock.png` —
  the stored bytes themselves — off the document.  Before this, every autosave of an image-heavy
  note re-compressed every PNG on the main loop; the EXPORTER kept doing it
  until 2026-09 (`gdk_pixbuf_save`/`save_to_buffer` per image, for both the
  HTML data-URIs and the Markdown side-car files), which measured 3.15 s to
  export one note holding 20 screenshots and 0.71 s afterwards — the whole
  difference being PNG compression the database had already done.
- code_buttons_rebuild has a fast path: when block-start offsets match
  the existing buttons' marks, it only repositions (no widget churn per
  keystroke).
- Cross-note search reads the `notes.body_text` cache column (filled by
  every save via `on_note_extract_text`, a record-walk over the BNBF
  blob that skips image payloads entirely) — fetched as ONE query for
  the whole table (`on_db_note_body_map`), not per note: per-query
  latency is what hurts on shared/network DBs. NULL rows (pre-column
  saves) fall back to the extractor and write back. Measured: full cold
  extraction of 1260 notes / 616 MB of blobs = 183 ms; the warm path
  reads ~1 MB of text. The old path deserialized every note into a
  GtkTextBuffer, decoding every PNG, per search.
- Search runs on a worker thread (GtkSpinner in the window), never the
  GTK main loop. The worker opens its OWN SQLite connection (one
  connection must not cross threads); scope is resolved on the main
  thread first (it reads library widgets); results come back via
  g_idle_add. A SearchJob owns everything and frees itself on the main
  thread after checking its atomic `cancelled` flag — set when the
  window closes or a newer search starts, so it never touches a dead
  window. GRegex is immutable ⇒ compile on main (instant bad-pattern
  errors), match on worker.
- refresh_sidebar keeps its `populating` guard up through the
  selection restore, so the restore's select_iter can't fire the
  changed handler and rebuild the notes pane a second time — every
  caller pairs it with an explicit refresh_notes. If the old selection
  no longer exists it falls back to the root and refreshes the notes
  pane itself.
- Note deletes/moves go through the BULK `on_db_notes_delete` /
  `on_db_notes_move` (one transaction + one orphan-tag prune) — the
  old per-note variants fsynced per call, froze the GUI on big drops,
  and were REMOVED (pass `&id, 1` for one note).  The drop handler
  also calls `gtk_drag_finish` BEFORE its refreshes so the DnD
  handshake isn't stalled by the model rebuilds.  Autofit column
  measuring rides refresh_notes' population loop (one PangoLayout, one
  measurement per unique folder path, skipped while the grid is the
  visible view — on_view_list re-measures on switch); it never does a
  second model walk. The `#tag` autocomplete queries the tag
  list ONCE per capture (`ed->tag_choices`) and filters in memory per
  keystroke. `on_app_config_set` skips the ini rewrite when the value
  is unchanged. The startup/exit DB hash streams through `GChecksum`
  (never loads the file whole). Exports uniquify names within the run
  only, so re-exporting to the same directory overwrites (a mirror),
  not duplicates.
- Deliberately NOT done: WAL journal or synchronous=NORMAL pragmas —
  unsafe/risky on network filesystems, which the shared-DB feature
  targets.

## Environment gotchas

- Corporate TLS interception: MacPorts curl fails on github.com
  (self-signed cert in chain) — **use `/usr/bin/curl`** (macOS keychain
  trusts the proxy CA). gitlab.gnome.org and deb.debian.org work either
  way.
- clangd shows "gtk/gtk.h not found" diagnostics on every file — noise
  (no compile_commands.json); trust `make`, which builds `-Wall -Wextra`
  clean.
- **NEVER run `./notes` from the repo root, and never with no arguments
  anywhere**: the binary-adjacent `notes.ini` names the user's REAL
  database, and a bare `./notes` opens the GUI on it.  Development runs
  go through the sandbox ONLY — `make run-dev` builds, creates `dev/`
  (symlinked binary + icons, its own `notes.ini` with `db_dir=dev/db`, a
  throwaway database seeded with a few notes, folders, tags and action
  items) and runs `dev/notes`; CLI experiments are `cd dev && ./notes …`.
  `make clean-dev` discards the sandbox.  To launch it in the background
  for the user: `make run-dev > /tmp/notes-dev.log 2>&1 & disown` after
  `pkill -x notes`.  The IPC socket is per DATABASE (a hash of the
  configured path in its name), so a dev instance and the real one can
  run side by side without either's CLI commands reaching the other.
  Look CLI syntax up in `src/cli.h`, not by running the binary.

## Common task patterns

When making a targeted change, start by reading the files in the "Read" column,
then change the files in the "Change" column.

| Task | Read first | Change |
|---|---|---|
| Add a note field (metadata) | `db.h`, `db.c` | `db.h`, `db.c` (schema + ALTER migration) |
| Add a note field (content/format) | `bnbf.h`, `document.h` | `bnbf.[ch]`, `document.[ch]` (loader + saver + a round-trip test) |
| Add a new sidebar row or section | `library_window.c` (`SB_KIND_*`, `refresh_sidebar`) | `library_window.c`, `app.h` |
| Add a new CLI command | `cli.h` (synopsis), `cli.c` (`cmd_*`, `cli_dispatch_verbs`) | `cli.h`, `cli.c` |
| Make a CLI command print JSON too | `cli.c` (`json_str`/`json_time`/`json_array_*`, `print_note_line`) | `cli.c` (add the branch + the name to `cli_json_capable`) |
| Add a new toolbar button | `app.c` (`on_app_tool_item_new`), target window .c | `app.h` (if new kind), target window .c |
| Add a new ini setting | `app.h` (`OnApp` struct + block comment), `main.c` (bool loading block) | `app.h`, `main.c`, `settings_window.c` |
| Add a new DB column | `db.c` (schema + ALTER migration section around line 223) | `db.h`, `db.c` |
| Modify the BNBF format | `bnbf.h` (format spec), `bnbf.c` | `bnbf.[ch]` (bump `ON_BNBF_VERSION`, add the `ON_REC_*`), `document.c` (loader + saver) |
| Change editor window layout | `editor_window.c` (`editor_build_layout`, `editor_build_view`) | `editor_window.c` |
| Change how a note is EDITED (keys, selection, clipboard, tags, Enter/Backspace policy) | `note_view.h` (the API and signals), `note_view.c` | `note_view.c` (+ `note_view.h` for a new API call), then `tests/ui/*.txt` |
| Change how a note LOOKS (fonts, spacing, colours, prefixes, tables, code shading) or where a click lands | `doc_layout.h`, `doc_layout.c` | `doc_layout.c` |
| Change what the model can hold or do (a block kind, an operation) | `document.h`, `BLOCK_MODEL.md` | `document.[ch]`, `tests/test_document.c`, then the saver/loader if the format carries it |
| Add an editor command with a shortcut | `editor_window.c` (`EDITOR_ACTIONS`), `app.c` (`on_app_install_accels`) | both, plus the view API it calls |
| Change library window layout | `library_window.c` (builder functions: `library_build_*`) | `library_window.c` |
| Change AI summary behaviour | `library_window.c` (`run_ai_summary`, `build_ai_pane`) | `library_window.c` |
| Modify export output | `export.c` | `export.c` |
| Modify search behaviour | `search_window.c` | `search_window.c` |
| Change the search QUERY LANGUAGE (both surfaces) | `search_query.h` (the syntax), `search_query.c` | `search_query.c` |
| Modify the media browser | `media_window.c`, `serialize.h` (image API) | `media_window.c` |
| Change the modal image viewer (BOTH hosts) | `image_viewer.h` (the ops contract), `image_viewer.c` | `image_viewer.c` |
| Add a THIRD image-viewer host | `image_viewer.h`, `media_window.c` (`media_viewer_ops`) | new host .c only |
| Change the Settings Database section | `settings_window.c` (`DbSection`, `db_section_refresh`, `bk_section_refresh`), `backup.h` | `settings_window.c` |
| Change backup behaviour | `backup.h` (the contract), `backup.c` | `backup.c` |

## Rename cleanup TODO

Two renames — Blue Notes → Records (2026-07-31), Records → Notes
(2026-08) — left a few things intentionally unchanged.  The second rename
covered every identifier and string that literally said "records", plus the
on-disk names (with the two adopt shims documented above), the binary,
the .app bundle, the packages and .gitignore.  What was deliberately NOT
touched, and should be cleaned up eventually:

- **Internal C naming**: `on_` / `On` / `ON_` prefixes throughout all
  source files (originally stood for "Orange Notes" → carried through
  Blue Notes → Records → Notes; safe to rename but a large mechanical
  change, and they never said "records", which is why the 2026-08 sweep
  left them alone).
- **The word "records" as a NOUN in the format docs** — `serialize.[ch]`,
  the BNBF sections here, "PNG image records", "typed records" — refers to
  BNBF records, not the app.  A rename sweep MUST protect these (and the
  verb, as in "the CLI records the request", and the Blue Note Records
  credit below); do it with an explicit protect-list and assert the
  protected strings survive, never a bare find-and-replace.
- **Header guards**: `BLUE_DB_H`, `BLUE_IPC_H`, `BLUE_CLI_H` etc. in
  the `#ifndef` guards — purely cosmetic, zero runtime impact.
- **BNBF format name**: `serialize.h` still has a note that BNBF stood
  for "Blue Notes Binary Format". The magic bytes `BNBF` are stored in
  every note blob and cannot be changed without a migration; the comment
  is just a historical footnote.
- **About dialog authors string**: "And thanks to Blue Note Records…" —
  an acknowledgment of the jazz label, intentionally kept and deliberately
  excluded from both rename sweeps ("Records" there is the label's name).
  The app is called Notes again, so "Note" overlaps once more, though the
  original Blue Notes double-entendre is still gone. Worth rewording.
- **REFACTORING.md historical paths**: references to
  `~/.local/share/blue_notes/pre-heal-backup-20260709.db` and
  `~/.local/share/blue_notes/pre-onbf-migration-20260709.db` — those
  backup files physically exist at those paths; update the doc if/when
  the files are moved or deleted.

## Conventions

- Every function gets a banner comment: purpose, params, return; comment
  non-obvious variables. Column-aligned trailing comments, ~78-col lines.
- `on_` prefix for public symbols; `On` prefix for types.
- UI strings use UTF-8 escapes for …, •, ✕ etc. in source.
- No GtkHeaderBar. Window titles `"Notes - <name>"`.
- Scrollbars: overlay scrolling disabled globally
  (`GTK_OVERLAY_SCROLLING=0` in main) + per-scrolled-window; vertical
  policy AUTOMATIC.
