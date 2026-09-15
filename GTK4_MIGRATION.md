# GTK4 migration — plan, inventory and running state

This file is the durable state of the GTK3 → GTK4 port.  It exists because
the port spans many sessions and no single session's context survives it:
**every session starts by reading this file and ends by updating it**.
The Decisions section is the important one — it is where the API mapping
choices live, so that the seventh session ports the clipboard the same way
the second one did.

Written 2026-09-14 from a survey of the GTK3 code.  Numbers below are that
survey's; they drift as `main` moves, so re-grep before trusting a count.

## Why, and why not

The motive is XFCE + GTK4's GPU renderer.  Two things to keep in view:

- **XFCE does not need this.**  XFCE 4.18/4.20 is itself GTK3; `make deb` /
  `make rpm` already produce an installable GTK3 build.  Test that first.
- **The renderer will not touch the app's measured costs.**  Every number in
  CLAUDE.md's Performance section is PNG decode, SQLite blob IO or
  per-note deserialization — CPU work the GPU renderer never sees.  On a
  VM with software GL, GTK4 commonly renders SLOWER than GTK3's cairo path
  and gets run with `GSK_RENDERER=cairo` anyway.

The durable reason to port is that GTK3 is in maintenance and GTK4 is where
the toolkit's future work lands.  That is a real reason, but it sets its own
pace, and this plan is built so it can be paused after any phase.

## Ground rules for every session

1. Read this file.  Read CLAUDE.md, but see "Quirks that do not carry over"
   below before applying any of its GTK quirks to GTK4 code.
2. Work in the `gtk4` worktree, never on `main` (exception: Phase 2, which
   is GTK3-legal and is done on `main` on purpose).
3. Do ONE checklist item, `make` clean under `-Wall -Wextra`, tick the box,
   add every non-obvious mapping to Decisions, commit.  Then `/clear`.
   Compaction summaries lose exactly the decisions this file must keep.
4. A re-derived quirk (see Phase 5) is written into this file the day it is
   found, in the same "measured, not assumed" style as CLAUDE.md's.
5. GTK3 and GTK4 cannot link into one process, so the branch does not run
   until Phase 1 completes and does not run CORRECTLY until Phase 4.  Do not
   expect to test anything before then; expect to compile everything.

## Inventory — what the survey found

27 k lines; ~9 k touched.  Zero-GTK files (`db.c`, `backup.c`,
`search_query.c`, `ipc.c` — 3.5 k lines) are untouched.

| File | Lines | GTK/Gdk calls | What breaks |
|---|---|---|---|
| `library_window.c` | 6 056 | ~1 130 | toolbar, 2 menubars + 3 context menus, ALL DnD (23 sites), 5 `gtk_dialog_run`, 8 event signals, icon view |
| `editor_window.c` | 5 658 | ~1 000 | 4 context menus + `populate-popup`, 8 event signals, `add_child_in_window`, `"draw"` (line numbers), clipboard (image atoms), popup window, `gtk_window_move` ×2 |
| `settings_window.c` | 1 307 | 338 | 1 file chooser, `no_show_all`, container_add sweep |
| `image_viewer.c` | 708 | 138 | `GtkEventBox`, 3 event signals, cursor via `GdkWindow` |
| `media_window.c` | 680 | 90 | 2 event signals, `configure-event` for size persistence |
| `search_window.c` | 592 | 103 | `configure-event`, sweep |
| `app.c` + `main.c` | 916 | ~105 | 2 dialogs each (incl. `startup_first_run`), CSS providers, icon loading, tool-item factory, default window icon |
| `serialize.c` | 1 298 | 128 | pixbuf ↔ texture at the widget edge only; BNBF untouched |
| `export.c`, `cli.c` | 3 363 | 88 | offscreen `GtkTextBuffer` only — near zero |

Removed in GTK4 (no shim):

| GTK3 | Sites | GTK4 |
|---|---|---|
| `GtkToolbar` / `GtkToolItem` | 24 / 18, via `on_app_tool_item_new` | `GtkBox` + `.toolbar` style class |
| `GtkMenu` / `GtkMenuBar` / `GtkMenuItem` | 7 menus, 2 menubars, 2 `popup_at_pointer` | `GMenu` + `GAction` + `GtkPopoverMenu(Bar)` |
| `gtk_dialog_run` | 11 | modal dialog + `::response` (or `GtkAlertDialog` / `GtkFileDialog`, 4.10+) |
| `*-event` signals | 21 | `GtkGestureClick`, `GtkEventControllerKey/Motion/Scroll/Focus` |
| `GtkEventBox` | 5 | any widget + controller |
| `gtk_window_move` / `get_position` / `GTK_WINDOW_POPUP` | 7 + 1 | nothing (window placement); `GtkPopover` (tag popup) |
| `gtk_text_view_add_child_in_window` / `move_child` | 2 | `gtk_text_view_add_overlay` / `move_overlay` (buffer coords, ride scrolling) |
| DnD signals + `gtk_drag_*` + `GdkDragContext` | 23 | `GtkDragSource`, `GtkDropTarget`, `GdkContentProvider` |
| `GtkClipboard` | 10 | `GdkClipboard`, async, `GdkContentFormats` |
| `"draw"` signal | 1 (`on_view_draw`) | subclass `GtkTextView`, override `snapshot` |
| `"populate-popup"` | 1 | `gtk_text_view_set_extra_menu` (GMenuModel) |
| `gtk_container_add` / `show_all` / `no_show_all` / `widget_destroy` | 29 / 23 / 10 / 22 | per-widget append, visible-by-default, `gtk_window_destroy` |
| `GdkWindow` (cursor, origin, frame extents) | 19 | `gtk_widget_set_cursor`; `GdkSurface` for the rest |
| `gtk-mac-integration` (`HAVE_GTKOSX`) | 3 files | nothing — GTK3-only library |

Survives, DEPRECATED since 4.10 (gone in GTK5): `GtkTreeView`, `GtkListStore`,
`GtkTreeStore`, `GtkIconView`, `GtkDialog`, `GtkStatusbar`, `GtkComboBoxText`.
Verified against docs.gtk.org 2026-09-14.  This is the single largest saving
in the plan: the sidebar, notes list, Action Items view and grid port
largely intact.  The price is a second migration to `GListModel` +
`GtkColumnView`/`GtkGridView` before GTK5 — see "After this port".

Survives undeprecated: `GtkTextView` + tags + child anchors, `GtkOverlay`,
`GtkPaned`, `GtkStack`, `GtkFlowBox`, `GtkCalendar`, `GtkSpinner`,
`GtkSearchEntry`, the emoji chooser (as `GtkEmojiChooser` popover),
`GtkAboutDialog`, `GtkFileChooserDialog` (deprecated 4.10 for
`GtkFileDialog` — use the new one).

## Lost permanently

1. **Editor windows opening bottom-right** (CLAUDE.md quirk #21,
   `editor_place_bottom_right`, `editor_window.c:~5490–5545`).  GTK4 has no
   window positioning on any backend.  Delete the code and the quirk; the
   compositor places the window.
2. **Native macOS menubar** (`HAVE_GTKOSX`: `main.c:216`,
   `library_window.c:4264`, `settings_window.c:171,891`, the
   `native_menubar` ini key + Settings toggle).  No GTK4 port of
   gtk-mac-integration exists.  Delete all of it; the ini key becomes
   another silently-ignored stale line, like `toolbar_style_*`.
3. **The `#tag` autocomplete as a `GTK_WINDOW_POPUP`** at
   `editor_window.c:3241`, positioned by `gtk_window_move` at `:3346`.
   Becomes a `GtkPopover` on the text view with `set_pointing_to` at the
   caret rectangle — recoverable, but a rewrite.

## Quirks that do not carry over

CLAUDE.md's GTK3 quirks were derived empirically against GTK3 internals.
On the `gtk4` branch treat them as follows:

| Quirk | On GTK4 |
|---|---|
| 1, 2 (text-window children) | Superseded: `add_overlay` takes buffer coords natively.  Re-verify the top-margin behaviour; do not assume. |
| 3, 4 (copy button size/y) | Probably still apply (CSS + `get_line_yrange` both exist). |
| 5 (Retina) | Superseded: `GdkTexture` is scale-aware; drop `cairo_surface_set_device_scale` paths. |
| 13 (drop protocol) | **Does not exist** — `GtkDropTarget` has no mid-drag data request.  Delete, re-derive. |
| 14 (expand_all, drag icon) | Re-derive: `GtkDragSource::prepare`/`drag-begin` + `gtk_drag_source_set_icon`. |
| 15 (multi-select collapse on press) | **Re-measure** — tree-view gestures changed; may be gone. |
| 16 (type-ahead search) | Still applies to deprecated `GtkTreeView`. |
| 17 (anchor baseline `rise`) | Probably applies; re-measure. |
| 18, 19, 20 (buffer/tag semantics) | Buffer-level, carry over. |
| 21 (window placement) | Gone with the feature. |
| 22 (text-window cursor owner) | Superseded: `gtk_widget_set_cursor` on the label itself is the GTK4 answer. |
| 23 (backdrop grey after focus grab) | **Re-measure before designing around it.**  Keep `can_focus FALSE` regardless. |
| 24 (link row midline) | Design decision, carries over. |
| 25 (`g_ptr_array_sort`) | GLib, carries over. |

## Phases

Estimate: 6–10 weeks solo.  Phases 0 and 2 are safe to do and stop.

### Phase 0 — spike (3–4 days) — decides whether the rest happens

Throwaway program, NOT in `src/`, ~300 lines: a `GtkTreeStore`-backed
`GtkTreeView` with `GtkDragSource` + `GtkDropTarget` doing re-nest /
reorder between rows, and a `GtkTextView` with one anchored image and one
`add_overlay` child that must stay put across scrolling.  Answers the two
questions the estimate hangs on.  Stop the port here if either is ugly.

- [ ] Install `gtk4 +quartz` (MacPorts) — `sudo port install gtk4 +quartz`
- [ ] Spike builds and runs on macOS
- [ ] Deprecated tree-view + DropTarget: drop lands once, with real
      coordinates, on quartz (the quirk-13 question)
- [ ] Multi-selection survives a press-and-drag (the quirk-15 question)
- [ ] `add_overlay` child rides scrolling at 1×, top margin behaviour noted
- [ ] Findings written to Decisions below
- [ ] Verdict: go / no-go

### Phase 1 — mechanical sweep (1 week) — makes the branch compile

Per-file, independent, scriptable.  The one phase that fans out cleanly
(one agent per file with the same recipe; one `make` to join).  Nothing
runs at the end of it; everything compiles.

Recipe: `gtk_widget_show_all` → delete · `gtk_widget_set_no_show_all` →
delete, check the updater owns visibility · `gtk_container_add` →
`gtk_box_append` / `gtk_grid_attach` / `gtk_window_set_child` /
`gtk_scrolled_window_set_child` / `gtk_frame_set_child` by parent type ·
`gtk_widget_destroy` → `gtk_window_destroy` (toplevels) or parent-remove ·
`gtk_window_new(GTK_WINDOW_TOPLEVEL)` → `gtk_window_new()` ·
`gtk_css_provider_load_from_data(css, s, -1, NULL)` → `_load_from_string` ·
`gtk_style_context_add_provider_for_screen` → `_for_display` ·
`gtk_widget_get_style_context` + `add_class` → `gtk_widget_add_css_class` ·
`gtk_image_new_from_pixbuf/surface` → `gtk_image_new_from_paintable` over
`gdk_texture_new_for_pixbuf` · `gtk_window_set_default_icon` →
`gtk_window_set_default_icon_name` + icon theme path · Makefile:
`gtk+-3.0` → `gtk4`, `HAVE_GTKOSX` block deleted.

- [ ] `Makefile` (pkg-config module, drop GTKOSX, `-DGDK_DISABLE_DEPRECATED` OFF — we use deprecated tree views on purpose)
- [ ] `app.c`, `main.c`
- [ ] `settings_window.c`
- [ ] `search_window.c`
- [ ] `media_window.c`
- [ ] `image_viewer.c`
- [ ] `editor_window.c`
- [ ] `library_window.c`
- [ ] `serialize.c` (pixbuf stays internal; only widget-facing edges)
- [ ] `export.c`, `cli.c`
- [ ] `make` clean; branch compiles (does not run)

### Phase 2 — actions and menus (1 week) — done on `main`, in GTK3

Convert every menu to `GMenu` models over `GAction`s while still on GTK3:
GTK3's `gtk_menu_new_from_model` + `gtk_application_set_menubar` make this
legal, it improves the menu code on its own, and it removes the largest
single unit from the port.  Merge to `main`, then rebase `gtk4` on it.

- [ ] `GActionEntry` table on the application (File/View items) and on
      each window (`win.` scope: per-editor formatting, per-library view)
- [ ] Library menubar (`library_window.c` ~4200–4300, two `gtk_menu_bar_new`)
- [ ] Sidebar context menu (folder / tag / trash variants)
- [ ] Notes list + grid context menu (incl. Trash variants)
- [ ] Editor context menus ×4 (image, code block, text view `populate-popup`, tag)
- [ ] `sidebar_menu_sync` label re-pointing → action state / `g_menu_item_set_label` on a rebuilt section
- [ ] Accelerators via `gtk_application_set_accels_for_action` (replaces `gtk_accel_*`)
- [ ] Merged to `main`; `gtk4` rebased

### Phase 3 — dialogs go async (3–4 days)

Each `gtk_dialog_run` becomes modal + `::response` callback.  The tail of
the calling function moves into the callback; state it needs is carried
on the dialog as object data.

- [ ] `main.c:110–150` `startup_first_run` — the hard one: it blocks BEFORE
      the library window exists.  Becomes a callback chain gating
      `on_library_window_open`; the "Create" branch and the "Open" chooser
      each end in the same continuation.
- [ ] `app.c:46–80` `on_app_notice` + `on_app_pick_path` — shared helpers
      with 8 callers; give each a completion callback and convert the
      callers with them
- [ ] `library_window.c:1543` `action_due_dialog` (calendar), `:2137`
      `prompt_for_folder` (new folder / info + emoji), `:2263` `confirm`
      (returns a bool to 5+ callers — each caller's tail becomes a
      callback), `:2720` `on_open_db` chooser, `:2826` `on_about`
- [ ] `editor_window.c:1875` image file chooser → `GtkFileDialog`
- [ ] `settings_window.c:553` backup dir chooser → `GtkFileDialog` (folder mode)
- [ ] Emoji chooser hookup at `library_window.c:2092–2114` — the
      `"gtk-emoji-chooser"` object-data name is GTK3-private; find the GTK4 way

### Phase 4 — event controllers (1 week) — the branch RUNS after this

- [ ] `library_window.c` ×8: 6 `button-press` (context menus, quirk-15 veto,
      icon-view right-click), 1 release, 1 key
- [ ] `editor_window.c` ×8: view press (code links, image click), key ×2
      (view, window-level for the modal viewer), motion (link cursor),
      enter-notify, focus-in, map-event (delete with placement)
- [ ] `image_viewer.c` ×3: press (backdrop close + `img_hit`), motion
      (`img_cursor`), scroll — plus `GtkEventBox` → `GtkBox` with
      `GtkGestureClick`; `GdkEventKey *` in `on_image_viewer_key_press`
      becomes `(keyval, state)`
- [ ] `media_window.c` ×2 + `configure-event` → `notify::default-width/height`
- [ ] `search_window.c` `configure-event` → same
- [ ] Re-measure quirk 23 here, before touching the viewer's focus design

### Phase 5 — drag and drop (1–2 weeks) — highest risk

All in `library_window.c`.  Quirks 13/14/15 are re-derived here and
written into Decisions the day they are measured.

- [ ] Sidebar as drop target: notes → folder (multi-select), folder re-nest
      INTO / reorder BEFORE-AFTER / trash / restore.  `GtkDropTarget` with
      `GTK_TREE_MODEL_ROW`-equivalent content (a `GValue` holding ids —
      decide the content type ONCE, see Decisions)
- [ ] Sidebar as drag source (single folder row)
- [ ] Notes list as drag source (multi-row; quirk-15 veto if still needed)
- [ ] Icon view as drag source (`gtk_icon_view_enable_model_drag_source`
      still exists on the deprecated widget — check it interoperates with
      a `GtkDropTarget`)
- [ ] Drag icons (folder.png / file.png / documents.png) via
      `gtk_drag_source_set_icon` with a `GdkPaintable`
- [ ] Drop indicator (`gtk_tree_view_set_drag_dest_row`) still works
- [ ] Sorted lists refuse row drops (was: list stores refuse)

### Phase 6 — editor internals (1–2 weeks)

- [ ] Code-block copy links: `add_child_in_window` → `add_overlay`; the
      rebuild/reposition logic at `editor_window.c:~830–1000` simplifies
      (quirks 1/2 gone) — remove the `buffer_to_window_coords` dance
- [ ] `on_view_draw` (line numbers, `:1015`) → `NotesTextView` subclass
      with a `snapshot` vfunc that chains up then draws; this is the one
      GObject subclass the port introduces
- [ ] `populate-popup` → `gtk_text_view_set_extra_menu` with a `GMenu`
      rebuilt on right-click (image / code-block items are contextual)
- [ ] Clipboard: `gtk_clipboard_set_text` ×3 → `gdk_clipboard_set_text`;
      `set_image` → `gdk_clipboard_set_texture`; the macOS image-atom
      probing at `:1818–1860` → `gdk_clipboard_get_formats` +
      `read_texture_async` — re-verify the Apple-private-UTI problem
      exists on GTK4 quartz before porting the workaround
- [ ] `#tag` popup → `GtkPopover` pointing at the caret rect
- [ ] Image anchors: `GtkImage` from `GdkTexture`, HiDPI via the texture
      (drop the cairo device-scale path); `on_image_viewer_fit` returns a
      `GdkTexture`/`GtkPicture`, not a cairo surface
- [ ] `editor_place_bottom_right` + map-event: delete
- [ ] `gdk_window_set_cursor` ×3 → `gtk_widget_set_cursor_from_name`

### Phase 7 — platform and packaging (1 week)

- [ ] Delete `HAVE_GTKOSX` everywhere + the `native_menubar` setting UI
- [ ] `.app` bundle: GTK4 quartz needs its own loader/module paths — redo
      `make app` against the MacPorts gtk4 tree
- [ ] `make deb` / `make rpm` on an XFCE box; runtime deps become `libgtk-4-1`
- [ ] CSS sweep: node names changed (`textview > text`, `button`, `.toolbar`,
      `treeview.view`, `popover.emoji`), the touch-assist CSS in
      `on_app_apply_touch_assist` is almost certainly dead on GTK4
- [ ] `GDK_CORE_DEVICE_EVENTS` / `GTK_OVERLAY_SCROLLING` env in `main.c`:
      neither exists in GTK4 — delete; overlay scrolling is per-scrolled-window
      `gtk_scrolled_window_set_overlay_scrolling`
- [ ] CLAUDE.md rewritten for the branch: quirks table above applied, new
      quirks from Decisions promoted
- [ ] BUILD.md / README.md dependency lists

## After this port

GTK5 removes `GtkTreeView`/`GtkIconView`/`GtkListStore`/`GtkTreeStore`.
That migration (sidebar, notes list, Action Items, grid → `GListStore` +
`GtkColumnView`/`GtkGridView`/`GtkTreeListModel`) is a separate project of
comparable size to this one and is NOT in scope here.  Doing it now would
double the port; doing it never means the app dies with GTK4.  Decide when
GTK5 has a date.

## Decisions

Append-only.  One entry per non-obvious mapping, with the reason.  A later
session that finds an entry wrong REPLACES it and says why — it does not
add a second idiom.

- **2026-09-14 — Tree views stay on the deprecated `GtkTreeView` family.**
  Verified deprecated-not-removed in 4.10+.  The Makefile must NOT define
  `GTK_DISABLE_DEPRECATED`; deprecation warnings are silenced with
  `-Wno-deprecated-declarations` for the two files that use them, NOT
  globally — everything else should still warn.
- **2026-09-14 — Phase 2 runs on `main` in GTK3.**  `GMenu`/`GAction` are
  GLib and GTK3 renders them; nothing about it is GTK4-specific, and it
  makes the menu code better whether or not the port proceeds.
- **2026-09-14 — Window placement code is deleted, not emulated.**  No
  backend-specific fallback (NSWindow on quartz); one code path.
- **2026-09-14 — `GdkPixbuf` stays the in-memory image type inside
  `serialize.c`** (the `"on-png"` GBytes cache, `png_decode_capped`, the
  PNG-bytes-verbatim contract all live on it).  Textures are made at the
  widget edge (`gdk_texture_new_for_pixbuf`) and never cached — the pixbuf
  is the one source of truth, as today.

## Session log

One line per session: date, phase, item, outcome.

- 2026-09-14 — plan written; survey numbers above.
