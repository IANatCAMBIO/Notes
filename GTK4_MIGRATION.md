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
   is GTK3-legal and is done on `main` on purpose).  THIS FILE is edited on
   `gtk4` only — a Phase 2 session on `main` ticks its boxes here after the
   rebase, so there is never a second diverging copy.
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
2. ~~Native macOS menubar~~ — **WRONG, struck 2026-09-14.**  GTK's own
   quartz backend (`gtkapplication-quartz.c`, present in 3.24 AND 4.22)
   exports `gtk_application_set_menubar()` to the native bar and builds
   the app menu from `app.about`/`app.preferences`/`app.quit`.
   gtk-mac-integration was never needed; Phase 2 removed it on `main`.
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

- [x] Install `gtk4 +quartz` (MacPorts) — 4.22.4
- [x] Spike builds and runs on macOS (`tools/gtk4-spike/`, kept on the
      branch as the reference for the three recipes below)
- [x] Deprecated tree-view + DropTarget: drop lands once, with real
      coordinates, on quartz — YES (found a GTK crash on the way, see D5)
- [x] Multi-selection survives a press-and-drag — NOT by itself (quirk 15
      persists) but the GTK3 remedy ports cleanly, see D7
- [x] `add_overlay` child rides scrolling at 1×, top margin behaviour noted
      — YES, see D6
- [x] Findings written to Decisions below
- [x] Verdict: **GO** (2026-09-14)

### Phase 1 — mechanical sweep — DONE 2026-09-14/15 as the per-file pass (0b7d275)

The Phase 1/3/4/5/6 checklists below were written before the "Port recipe"
section changed the unit of work to one file at a time; the per-file pass
did all of them, and the runtime rounds of 2026-09-15 verified them by hand
(D13–D25 are what those rounds found).  They are ticked as records.

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

- [x] `Makefile` (pkg-config module, drop GTKOSX, `-DGDK_DISABLE_DEPRECATED` OFF — we use deprecated tree views on purpose)
- [x] `app.c`, `main.c`
- [x] `settings_window.c`
- [x] `search_window.c`
- [x] `media_window.c`
- [x] `image_viewer.c`
- [x] `editor_window.c`
- [x] `library_window.c`
- [x] `serialize.c` (pixbuf stays internal; only widget-facing edges)
- [x] `export.c`, `cli.c`
- [x] `make` clean; branch compiles (does not run)

### Phase 2 — actions and menus — DONE 2026-09-14 on `main` (facad9e)

Every menu is a `GMenu` over `GAction`s, on GTK3; CLAUDE.md's "Actions,
menus and shortcuts" section is the design record.  It was done BEFORE
Phase 1 so the sweep never touches GtkMenu code (the rebase was clean).

- [x] Action tables: `app.` on the application (menubar; delegate to the
      library through a weak pointer), `win.` on both windows (toolbar,
      context menus, every shortcut); windows are GtkApplicationWindows
- [x] Library menubar as a model → `gtk_application_set_menubar`
- [x] Sidebar / note / column context menus as per-popup models through
      one `on_app_menu_popup`
- [x] Editor: image items (`editor_image_menu` — the model Phase 6 hands
      to `set_extra_menu`), table-cell menu, Styles/Lists/Insert menu
      buttons on models; editing actions gated on the view's focus
- [x] Sidebar Hide/Show: two `hidden-when` items, not a model edit (D10)
- [x] Accelerators via `gtk_application_set_accels_for_action`, Cmd-only
      on macOS (user decision)
- [x] gtk-mac-integration deleted everywhere (Makefile, main, settings)
- [x] Merged to `main`; `gtk4` rebased

What Phase 1's sweep still meets in this code, and how: `gtk_menu_new_from_model`
(`on_app_menu_popup`) → `gtk_popover_menu_new_from_model`;
`gtk_menu_bar_new_from_model` (macOS in-window fallback) →
`gtk_popover_menu_bar_new_from_model`; `menu_shell_prepend_model` +
`populate-popup` → `gtk_text_view_set_extra_menu(editor_image_menu())`
rebuilt from a right-click gesture; `gtk_menu_button_set_use_popover(FALSE)`
→ delete (always a popover).  Nothing else in the menu layer is GTK3-specific.

### Phase 3 — dialogs go async (3–4 days)

Each `gtk_dialog_run` becomes modal + `::response` callback.  The tail of
the calling function moves into the callback; state it needs is carried
on the dialog as object data.

- [x] `main.c:110–150` `startup_first_run` — the hard one: it blocks BEFORE
      the library window exists.  Becomes a callback chain gating
      `on_library_window_open`; the "Create" branch and the "Open" chooser
      each end in the same continuation.
- [x] `app.c:46–80` `on_app_notice` + `on_app_pick_path` — shared helpers
      with 8 callers; give each a completion callback and convert the
      callers with them
- [x] `library_window.c:1543` `action_due_dialog` (calendar), `:2137`
      `prompt_for_folder` (new folder / info + emoji), `:2263` `confirm`
      (returns a bool to 5+ callers — each caller's tail becomes a
      callback), `:2720` `on_open_db` chooser, `:2826` `on_about`
- [x] `editor_window.c:1875` image file chooser → `GtkFileDialog`
- [x] `settings_window.c:553` backup dir chooser → `GtkFileDialog` (folder mode)
- [x] Emoji chooser hookup at `library_window.c:2092–2114` — the
      `"gtk-emoji-chooser"` object-data name is GTK3-private; find the GTK4 way

### Phase 4 — event controllers (1 week) — the branch RUNS after this

- [x] `library_window.c` ×8: 6 `button-press` (context menus, quirk-15 veto,
      icon-view right-click), 1 release, 1 key
- [x] `editor_window.c` ×8: view press (code links, image click), key ×2
      (view, window-level for the modal viewer), motion (link cursor),
      enter-notify, focus-in, map-event (delete with placement)
- [x] `image_viewer.c` ×3: press (backdrop close + `img_hit`), motion
      (`img_cursor`), scroll — plus `GtkEventBox` → `GtkBox` with
      `GtkGestureClick`; `GdkEventKey *` in `on_image_viewer_key_press`
      becomes `(keyval, state)`
- [x] `media_window.c` ×2 + `configure-event` → `notify::default-width/height`
- [x] `search_window.c` `configure-event` → same
- [x] Quirk 23: the panel still never takes focus; no grey text observed after closing the viewer in three rounds of hand testing on 4.22

### Phase 5 — drag and drop (1–2 weeks) — highest risk

All in `library_window.c`.  Quirks 13/14/15 are re-derived here and
written into Decisions the day they are measured.

- [x] Sidebar as drop target: notes → folder (multi-select), folder re-nest
      INTO / reorder BEFORE-AFTER / trash / restore.  `GtkDropTarget` with
      `GTK_TREE_MODEL_ROW`-equivalent content (a `GValue` holding ids —
      decide the content type ONCE, see Decisions)
- [x] Sidebar as drag source (single folder row)
- [x] Notes list as drag source (multi-row; quirk-15 veto if still needed)
- [x] Icon view as drag source (`gtk_icon_view_enable_model_drag_source`
      still exists on the deprecated widget — check it interoperates with
      a `GtkDropTarget`)
- [x] Drag icons (folder.png / file.png / documents.png) via
      `gtk_drag_source_set_icon` with a `GdkPaintable`
- [x] Drop indicator (`gtk_tree_view_set_drag_dest_row`) still works
- [x] Sorted lists refuse row drops (was: list stores refuse)

### Phase 6 — editor internals (1–2 weeks)

- [x] Code-block copy links: `add_child_in_window` → `add_overlay`; the
      rebuild/reposition logic at `editor_window.c:~830–1000` simplifies
      (quirks 1/2 gone) — remove the `buffer_to_window_coords` dance
- [x] `on_view_draw` (line numbers, `:1015`) → `NotesTextView` subclass
      with a `snapshot` vfunc that chains up then draws; this is the one
      GObject subclass the port introduces
- [x] `populate-popup` → `gtk_text_view_set_extra_menu(editor_image_menu())`,
      set from a right-click gesture (the model already exists; only the
      GTK3 glue `menu_shell_prepend_model` goes)
- [x] Clipboard: `gtk_clipboard_set_text` ×3 → `gdk_clipboard_set_text`;
      `set_image` → `gdk_clipboard_set_texture`; the macOS image-atom
      probing at `:1818–1860` → `gdk_clipboard_get_formats` +
      `read_texture_async` — re-verify the Apple-private-UTI problem
      exists on GTK4 quartz before porting the workaround
- [x] `#tag` popup → `GtkPopover` pointing at the caret rect
- [x] Image anchors: `GtkImage` from `GdkTexture`, HiDPI via the texture
      (drop the cairo device-scale path); `on_image_viewer_fit` returns a
      `GdkTexture`/`GtkPicture`, not a cairo surface
- [x] `editor_place_bottom_right` + map-event: delete
- [x] `gdk_window_set_cursor` ×3 → `gtk_widget_set_cursor_from_name`

### Phase 7 — platform and packaging (1 week)

- [x] `.app` bundle: the recipe copies the binary, icons and defaults and links MacPorts dynamically — nothing GTK3-specific; `make app` builds (bundle never launched from here: a bundle reads ~/.config, i.e. the REAL ini)
- [ ] `make deb` / `make rpm` on an XFCE box; runtime deps become `libgtk-4-1`
- [x] CSS sweep (cdac73b): every selector checked against the theme
      compiled into 4.22; the touch-assist CSS turned out CORRECT
      (`cursor-handle`, `popover.magnifier`, and `-gtk-icon-source` still
      exists and paints the handle); per-widget providers gone — one
      display stylesheet per module, rules on `notes-*` classes
- [x] `GDK_CORE_DEVICE_EVENTS` / `GTK_OVERLAY_SCROLLING` env deleted (per-file pass)
- [x] CLAUDE.md rewritten for the branch: GTK4 build/deps, a note_view
      row, the GTK3 quirks marked superseded/kept, a "GTK4 quirks" list
      (D5–D25 in short form), actions section, task patterns
- [x] BUILD.md (new) / README.md dependency lists
- [x] **Extract the note view** (233ca43: `src/note_view.[ch]`, `OnNoteView`).  `editor_window.c` (5.8 k lines) is two
      things braided together: the window (chrome, toolbar, actions,
      autosave, status bar, the modal viewer host) and the rich-text
      ENGINE (`NotesTextView` + everything that edits its buffer: inline
      and paragraph styles, list continuation, code blocks with gutter and
      copy links, image/table/checkbox anchors, #tag capture, emoji
      padding, the derived title/action tints, in-note find, snapshot
      undo).  Move the engine into `src/note_view.[ch]` as a proper GObject
      — `NotesView`, a `GtkTextView` subclass with a real API: `load(blob)`
      / `serialize()`, `toggle_inline(flag)` / `toggle_para(flag)`,
      `insert_image/table/checkbox/date`, `find(text)`, `undo/redo`,
      signals for "changed", "tags-changed", "actions-changed" — and leave
      `editor_window.c` as the window that hosts one.  NOT a GTK fork: a
      subclass using public API is the extension mechanism GTK provides,
      and this engine never needed GTK's internals (the three places it
      met private behaviour — D14, D16, D17 — are GTK bugs to file, with
      the pixel-probe programs as reproducers).  Same code, one boundary;
      doable on `main` first, and it makes the GTK5 GtkTextView surface
      one file wide.

## Port recipe — the per-file pass (Phase 1 as actually run)

Phase 1 turned out not to be a sweep that leaves a compiling tree: a file
compiles under GTK4 only when EVERYTHING GTK4 removed is gone from it, so
the unit of work is "one file, everything the GTK4 compiler rejects",
against the header contracts below (`app.h`, `image_viewer.h` — already
rewritten; every other header is unchanged).  Files are ported in parallel,
each verified with `make build/<name>.o`, and joined by one `make`.
Runtime behaviour is verified AFTERWARDS, in the `dev/` sandbox, by hand —
that is where Phases 4–6's re-derived quirks get written.

Rules for the pass:

1. Read CLAUDE.md first — its GTK3 quirks are wrong for GTK4 exactly where
   the table above says; its "Actions, menus and shortcuts" section is
   current and stays true.  Keep the house style: banner comment on every
   function, `snake_case`, K&R, no dead code, no near-duplicates.
2. Port the whole file: delete the GTK3 path in the same change, never
   leave both.  Where a behaviour cannot exist in GTK4 (window placement),
   delete it and say so in the report; do not emulate.
3. Deprecated-but-present API is allowed ONLY where named below; wrap each
   such call site in `G_GNUC_BEGIN_IGNORE_DEPRECATIONS` /
   `G_GNUC_END_IGNORE_DEPRECATIONS`.  A file that lives on the GtkTreeView
   family (`library_window.c`) instead defines
   `GDK_DISABLE_DEPRECATION_WARNINGS` and `GTK_DISABLE_DEPRECATION_WARNINGS`
   before its includes, with a comment.  `make build/<name>.o` must be
   `-Wall -Wextra` clean.
4. Never run the binary.  Nothing links until every file is done; the
   sandbox rule (D12) applies to whoever runs it afterwards.
5. Do not commit.  Report: what changed, every deletion of behaviour, every
   place you were unsure, and any header change you NEEDED (you may not make
   one — say so and stub locally).

Mapping (GTK3 → GTK4):

| GTK3 | GTK4 |
|---|---|
| `gtk_widget_show_all`, `gtk_widget_set_no_show_all` | delete (visible by default; a widget whose visibility an updater owns gets `gtk_widget_set_visible(w, FALSE)` at build) |
| `gtk_container_add(parent, c)` | `gtk_box_append`, `gtk_window_set_child`, `gtk_scrolled_window_set_child`, `gtk_frame_set_child`, `gtk_overlay_set_child`/`add_overlay`, `gtk_button_set_child`, `gtk_menu_button_set_child`, `gtk_grid_attach` — by parent type |
| `gtk_box_pack_start(b, c, expand, fill, pad)` | `gtk_box_append` (or `prepend`) + `gtk_widget_set_hexpand/vexpand(c, expand)` + margins for `pad` |
| `gtk_paned_pack1/pack2(p, c, resize, shrink)` | `gtk_paned_set_start_child/end_child` + `set_resize_start_child`/`set_shrink_start_child` (and `_end_`) |
| `gtk_widget_destroy(w)` | toplevel: `gtk_window_destroy`; child: the parent's remove (`gtk_box_remove`, …) or `gtk_widget_unparent` for a widget you parented yourself |
| `gtk_window_new(GTK_WINDOW_TOPLEVEL)` | `gtk_window_new()`; `GtkApplicationWindow` stays; `GTK_WINDOW_POPUP` → `GtkPopover` |
| `gtk_window_move`, `get_position`, `gdk_window_get_frame_extents`, `gdk_monitor_get_workarea` | **delete** (no window placement in GTK4; `editor_place_bottom_right` + `editor_workarea` + the map-event go) |
| `gtk_dialog_run` | never.  Confirmations → `GtkAlertDialog` + `gtk_alert_dialog_choose` (async, callback).  File/folder picks → `on_app_pick_path` (async).  Custom-widget dialogs (folder prompt, calendar) → `GtkDialog` (deprecated, wrapped) shown with `gtk_window_present`, `::response` callback; the caller's tail moves into the callback, its state carried as object data on the dialog |
| `gtk_message_dialog_new` + run | `on_app_notice` (fire-and-forget) |
| `gtk_about_dialog_new` + run | same dialog, `gtk_window_present`; logo via `gtk_about_dialog_set_logo(GdkPaintable)` |
| `"button-press-event"`/`"button-release-event"` | `GtkGestureClick` (`gtk_gesture_single_set_button` 0 = any); `pressed(n_press, x, y)`; right button = `gtk_gesture_single_get_current_button() == GDK_BUTTON_SECONDARY`; modifiers `gtk_event_controller_get_current_event_state`; `gtk_gesture_set_state(CLAIMED)` where the old handler returned TRUE |
| `"key-press-event"` | `GtkEventControllerKey` `key-pressed(keyval, keycode, state)` → TRUE to stop; on the window use `gtk_event_controller_set_propagation_phase(GTK_PHASE_CAPTURE)` where the old handler had to run before the focus widget |
| `"motion-notify-event"`, `enter/leave` | `GtkEventControllerMotion` (`motion(x, y)`, `enter`, `leave`) |
| `"scroll-event"` | `GtkEventControllerScroll` |
| `"focus-in-event"`/`"focus-out-event"` | `GtkEventControllerFocus` `enter`/`leave` (editor: D11 keeps its gate) |
| window `"focus-in-event"` (status refresh) | `notify::is-active` on the window |
| `"configure-event"` (size persistence) | `notify::default-width` / `notify::default-height` on the window |
| `"size-allocate"`, `"draw"` | a GtkWidget SUBCLASS overriding `size_allocate` / `snapshot` (chain up, then draw with `gtk_snapshot_append_layout` / cairo via `gtk_snapshot_append_cairo`).  The editor's `NotesTextView` (a `GtkTextView` subclass) is the one such subclass: line numbers in `snapshot`, code-button relayout in `size_allocate` |
| `"populate-popup"` | `gtk_text_view_set_extra_menu(view, editor_image_menu())`, set from the right-click gesture BEFORE the view opens its menu (`ed->ctx_offset` stashed there too); `menu_shell_prepend_model` deleted |
| `gtk_menu_new_from_model` (in `on_app_menu_popup`) | `gtk_popover_menu_new_from_model`, parented to the widget, `set_pointing_to` at (x,y), `set_has_arrow(FALSE)`, `gtk_popover_popup`; on `closed` → idle unparent |
| `gtk_menu_bar_new_from_model` | `gtk_popover_menu_bar_new_from_model` |
| `gtk_menu_button_set_use_popover` | delete; `gtk_menu_button_set_menu_model` stays; face via `gtk_menu_button_set_child(label)` |
| `GtkToolbar`, `GtkToolItem`, `gtk_toolbar_insert`, separators | `GtkBox` (horizontal) with `gtk_widget_add_css_class("toolbar")`; items are what `on_app_tool_item_new` returns, `gtk_separator_new(GTK_ORIENTATION_VERTICAL)`, an expanding spacer is any widget with `hexpand`; `gtk_widget_set_focus_on_click(FALSE)` on the buttons |
| `GtkToggleToolButton` active/set_active | `GtkToggleButton` |
| `gtk_actionable_*` on tool buttons | unchanged (`GtkButton` is actionable) |
| `GtkEventBox` | the child itself, or a `GtkBox`, with controllers |
| `gtk_text_view_add_child_in_window(view, c, TEXT, x, y)` / `move_child` | `gtk_text_view_add_overlay(view, c, x, y)` / `gtk_text_view_move_overlay` — buffer coordinates; the top margin is added by GTK on every allocation (D6); `gtk_text_view_remove(view, c)` to take it out |
| `gtk_text_view_add_child_at_anchor` | unchanged |
| child at anchor: `GtkImage` from surface | `GtkPicture` (`gtk_picture_new_for_paintable`) of a `GdkTexture` from the PNG bytes (`gdk_texture_new_from_bytes(on_image_png_bytes(pix))`), `gtk_picture_set_can_shrink(FALSE)`, `gtk_widget_set_size_request(display_w, h)`; scaled loads → `on_app_texture_for_pixbuf` |
| `gtk_image_new_from_pixbuf/surface`, `gtk_image_set_from_*` | `gtk_image_new_from_paintable` / `gtk_image_set_from_paintable`, textures from `on_app_texture_for_pixbuf` |
| `cairo_surface_t` thumbnails in list stores (`CAIRO_GOBJECT_TYPE_SURFACE`) | `GDK_TYPE_TEXTURE` column; `GtkCellRendererPixbuf` bound to its `"texture"` attribute |
| `gdk_cairo_surface_create_from_pixbuf`, `cairo_surface_set_device_scale` (quirk #5) | delete — textures carry their pixels, GTK scales |
| `on_image_viewer_fit` | deleted; a render op returns the texture (see image_viewer.h) |
| `gtk_drag_set_icon_surface` | `gtk_drag_source_set_icon(source, on_app_icon_paintable(), hx, hy)` |
| tree/icon-view DnD (`enable_model_drag_*`, `drag-*` signals, `gtk_drag_*`, `GdkDragContext`, `GtkTargetEntry`) | `GtkDragSource` on each view (`prepare(x, y)` → row at the point → `gdk_content_provider_new_typed` over ONE boxed GType `OnDragRows` {kind, ids} defined in library_window.c — D8), `GtkDropTarget` on the sidebar (`motion` validates + `gtk_tree_view_set_drag_dest_row`, `leave` clears, `drop(value, x, y)` acts).  MUST call `gtk_tree_view_enable_model_drag_dest(view, gdk_content_formats_new(NULL, 0), 0)` once on the sidebar (D5, the crash).  Multi-select press veto: D7 (`tools/gtk4-spike/spike.c` has the working code) |
| `gtk_tree_view_get_path_at_pos` from a gesture | convert first: `gtk_tree_view_convert_widget_to_bin_window_coords` |
| `GtkClipboard` | `gdk_clipboard_set_text(gtk_widget_get_clipboard(w), s)`, `gdk_clipboard_set_texture`; paste probe → `gdk_clipboard_get_formats` + `gdk_content_formats_contain_mime_type("image/png")`, read with `gdk_clipboard_read_texture_async` — then `gdk_texture_save_to_png_bytes` → pixbuf via the loader, so serialize.c's pixbuf contract is untouched |
| `gdk_window_set_cursor`, `gdk_cursor_new_from_name` | `gtk_widget_set_cursor_from_name(w, "pointer"/"text"/NULL)` on the widget that should show it (quirk #22 is moot: every widget owns its cursor) |
| `gtk_widget_get_window`, `GdkWindow` | `gtk_widget_get_native` + `gtk_native_get_surface` if truly needed (rarely); `gdk_display_get_monitor_at_surface` |
| `gtk_widget_get_toplevel` | `gtk_widget_get_root` |
| `gtk_css_provider_load_from_data(p, s, -1, NULL)` | `gtk_css_provider_load_from_string(p, s)` |
| `gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), …)` | `gtk_style_context_add_provider_for_display(gdk_display_get_default(), …)` |
| `gtk_widget_get_style_context` + `add_class` | `gtk_widget_add_css_class` |
| `on_app_widget_add_css` | keep the function; its body may use the deprecated per-widget provider, wrapped |
| `gtk_window_set_default_icon_from_file` | copy `icons/composition.png` to `icons/theme/hicolor/512x512/apps/notes.png` (the .deb already installs that name), `gtk_icon_theme_add_search_path(theme, icons_dir/theme)`, `gtk_window_set_default_icon_name("notes")` |
| `gtk_icon_theme_get_default`, `prepend_search_path` | `gtk_icon_theme_get_for_display(gdk_display_get_default())`, `gtk_icon_theme_add_search_path` |
| `GTK_OVERLAY_SCROLLING=0`, `GDK_CORE_DEVICE_EVENTS` env | delete; `gtk_scrolled_window_set_overlay_scrolling(sw, FALSE)` per scrolled window (already there) |
| `on_app_apply_touch_assist` CSS (`cursor-handle`, `popover.magnifier`) | keep the function and setting; GTK4 node names are `cursor-handle` still and the magnifier is `magnifier`; leave a comment that it is unverified on GTK4 |
| `gtk_entry_get_text/set_text` | `gtk_editable_get_text/set_text` |
| `gtk_calendar_get_date(c, &y, &m, &d)` | `GDateTime *dt = gtk_calendar_get_date(c)` |
| `gtk_widget_set_events`, `gtk_widget_add_events` | delete |
| `gtk_widget_get_allocation` | `gtk_widget_get_width/height`, `gtk_widget_compute_bounds` / `compute_point` for positions relative to another widget |
| `gtk_widget_set_can_focus`, `grab_focus` | unchanged (`gtk_widget_set_focusable` also exists) |
| `gtk_label_set_line_wrap` | `gtk_label_set_wrap` |
| `gtk_button_box`, `gtk_dialog_get_action_area` | delete; dialog buttons via `gtk_dialog_add_button` only |
| `GtkComboBoxText`, `GtkStatusbar`, `GtkDialog`, `GtkTreeView`/`GtkListStore`/`GtkTreeStore`/`GtkIconView` and their cell renderers | ALLOWED, deprecated, wrapped (rule 3) |
| `gtk_tree_view_column_get_button` + press handler (column header menu) | `GtkGestureClick` on that button |
| `gtk_emoji_chooser` reached via `"gtk-emoji-chooser"` object data | delete that hookup; the folder dialog's emoji entry uses `gtk_entry_set_input_hints(GTK_INPUT_HINT_EMOJI)` and the entry's own chooser; drop the resize-after-close handler |
| `gtk_widget_queue_draw`, `gtk_widget_queue_resize` | unchanged |
| `g_signal_connect(win, "destroy", …)` on windows | unchanged |

File assignments (one agent each; every agent reads this section, CLAUDE.md,
and the whole file it owns before editing):

| Agent | Files | Notes |
|---|---|---|
| A | `src/app.c`, `src/main.c` | implements the new `app.h` contract; `main.c`: dialogs in `startup_first_run` async (the "Create/Open" continuation gates `on_library_window_create`), env vars, icon theme, `quartz_log_filter` (message texts may differ — keep both filters, they are harmless if they never match) |
| B | `src/settings_window.c` | one `on_app_pick_path` caller, `gtk_container_add`/`pack_start` sweep, clipboard, GtkComboBoxText wrapped, checkbox visibility |
| C | `src/search_window.c`, `src/media_window.c` | `configure-event` → notify; media: `GtkFlowBox` stays, cells hold `GdkTexture`s, viewer ops return textures, click via `GtkGestureClick`, key controller, `GtkEventBox` gone |
| D | `src/image_viewer.c` | implements the new `image_viewer.h`: `GtkPicture` with `SCALE_DOWN`, labels hit-tested from a `GtkGestureClick` on the backdrop, `GtkEventControllerMotion` for the cursor, key handler takes keyval/state; overlay size tracking via `notify::` on the overlay's allocation → a `size_allocate` override is NOT available on a plain widget: use `gtk_widget_add_tick_callback` once after `gtk_widget_queue_allocate` or simply re-fit in the render path, since `GtkPicture` now fits by itself (most of the old fit/debounce machinery DELETES) |
| E | `src/editor_window.c` | the biggest: `NotesTextView` subclass (snapshot + size_allocate), overlays, controllers, clipboard, `GtkPopover` tag popup pointing at the caret (`gtk_text_view_get_iter_location` + `buffer_to_window_coords`), delete placement, `set_extra_menu`, toolbar box, `GtkPicture` images, checkbox/table anchored children unchanged, D11 gate via `GtkEventControllerFocus`, window key controller in CAPTURE for the viewer |
| F | `src/library_window.c` | second biggest: `G*_DISABLE_DEPRECATION_WARNINGS` at top; toolbar box; every dialog async (`confirm` → `GtkAlertDialog` with a continuation callback — its 5+ callers each split at the call); DnD per the table (both views as sources, sidebar as target, `OnDragRows`); column-header gestures; `GDK_TYPE_TEXTURE` thumbnails (`ThumbEntry` holds a texture); grid `GtkIconView` stays (deprecated); `gtk_menu_bar_new_from_model` → popover bar; `on_app_menu_popup(x, y)` |
| G | `src/serialize.c`, `src/export.c`, `src/cli.c` | mostly compile fixes: `gtk_init` calls, offscreen `GtkTextBuffer` use is unchanged; `on_note_deserialize*` image anchors carry pixbufs still; anything creating widgets offscreen (export's checkbox glyphs?) — check |

The join (me, after all seven report): `make`, fix link errors, then
`make run-dev` in the worktree's own `dev/` and the Phase 4–6 runtime
checklist by hand.

### Phase 8 — use GTK4 where it now does the job (after Phase 7)

The GTK3 app hand-rolled things GTK3 lacked.  GTK4 grew some of them, and
keeping a private copy of what the toolkit already links in is both
wasted memory and a second implementation to maintain.  Each subsystem
gets a verdict — ADOPT GTK's, or KEEP OURS with the measured reason —
and the reason goes into CLAUDE.md so the question is not re-asked.
Verdicts already established while porting:

| Subsystem | Verdict | Why |
|---|---|---|
| Undo / redo (snapshot stacks, ~600 lines) | **KEEP** | `GtkTextHistory` records inserts/deletes as PLAIN TEXT (`gtk_text_iter_get_slice`: an image is a bare U+FFFC) and replays them with `gtk_text_buffer_insert` — no tags, no anchors (gtktextbuffer.c `gtk_text_buffer_history_insert`, 4.22.4).  Undoing a deleted image would restore an empty placeholder; undoing a style change is impossible.  `gtk_text_buffer_set_enable_undo(FALSE)` stays, or GTK's copy shadows ours on Ctrl+Z AND records everything twice. |
| Emoji chooser | ADOPTED | `insert-emoji` |
| File / message dialogs | ADOPTED | `GtkFileDialog`, `GtkAlertDialog` |
| Image paste (macOS pasteboard atoms) | ADOPTED | GDK's content deserializers decode any image type to a texture |
| Image fitting / HiDPI (cairo device scale) | ADOPTED | `GtkPicture` + textures |
| Context menus, menubar, shortcuts | ADOPTED | `GMenu` + `GAction`, native macOS bar from GTK's quartz backend |
| Hover cursors (quirk #22's hit-tested motion handler) | ADOPTED | every widget owns its cursor |
| In-note find | KEEP (scan is GTK's) | `gtk_text_iter_forward_search` does the matching; the highlight tag and entry UI have no GTK equivalent |
| Lists, checkboxes, tables, images, code blocks, #tags, title/action tints | KEEP | `GtkTextView` has none of these; `GtkSourceView` has gutters/line numbers but is another library and still no lists/anchors |
| Tag autocomplete popover | KEEP | no completion for text views in GTK4 |
| Window size persistence, ini config | KEEP | GTK has no settings store |

To audit (unverified candidates, in order of likely payoff):

- [x] Per-widget CSS providers → classes + one display stylesheet per
      module (cdac73b).
- [x] Toolbar icons through the icon theme (12c9789): `icons/` is an
      UNTHEMED search path (GTK 4.22 still scans a search-path directory's
      top level by basename — verified in gtkicontheme.c), so the
      swap-a-PNG contract survives and GTK does the scale-factor loading
      and caching.
- [x] `image_texture()` deleted: it re-decoded PNG bytes whose pixels the
      anchor's pixbuf already held.  `on_app_texture_for_pixbuf` (a memory
      texture over those pixels, no copy — the bytes reference keeps the
      pixbuf alive) is THE edge; D4's "prefer from_bytes" was wrong for a
      pixbuf that already exists and is corrected in app.h.
- [x] `editor_rederive` — no measurement needed: every pass is bounded by
      the edited range (start-1 … end), not the buffer.
- [ ] `render_note_thumb` draws cards with cairo — snapshotting an
      `OnNoteView` instead needs a realized widget (GtkWidgetPaintable
      renders only mapped widgets), i.e. an offscreen window per render;
      not worth it for a 140 px card.  Leave unless thumbnails and the
      editor visibly disagree.
- [ ] Grid + notes list + sidebar on the deprecated tree-view family:
      the GTK5 item ("After this port") — `GtkColumnView` autosizes
      columns, retiring `list_autofit`'s PangoLayout measuring.

## After this port

GTK5 removes `GtkTreeView`/`GtkIconView`/`GtkListStore`/`GtkTreeStore`.
That migration (sidebar, notes list, Action Items, grid → `GListStore` +
`GtkColumnView`/`GtkGridView`/`GtkTreeListModel`) is a separate project of
comparable size to this one and is NOT in scope here.  Doing it now would
double the port; doing it never means the app dies with GTK4.  Decide when
GTK5 has a date.

The note ENGINE is the other project: replacing the `GtkTextView`
subclass with an `OnDocument` block model and a drawn view, so no widget
ever lives inside another widget again (D14/D17/D23/D27/D29 are all that
one thing).  Specified in **BLOCK_MODEL.md**; its step 1 is headless and
can start on this branch at any time.

## Decisions

Append-only.  One entry per non-obvious mapping, with the reason.  A later
session that finds an entry wrong REPLACES it and says why — it does not
add a second idiom.

- **2026-09-14 — Tree views stay on the deprecated `GtkTreeView` family.**
  Verified deprecated-not-removed in 4.10+.  The Makefile must NOT define
  `GTK_DISABLE_DEPRECATED`; deprecation warnings are silenced with
  `-Wno-deprecated-declarations` for the two files that use them, NOT
  globally — everything else should still warn.  **Reversed 2026-09-16
  (D33): the tree views are gone, and with them every deprecation
  suppression in the tree — the build must stay
  `-Wdeprecated-declarations` clean.**
- **2026-09-14 — Phase 2 runs on `main` in GTK3.**  `GMenu`/`GAction` are
  GLib and GTK3 renders them; nothing about it is GTK4-specific, and it
  makes the menu code better whether or not the port proceeds.
- **2026-09-14 — Window placement code is deleted, not emulated.**  No
  backend-specific fallback (NSWindow on quartz); one code path.
- **2026-09-14 — `GdkPixbuf` stays the in-memory image type inside
  `serialize.c`** (the `"on-png"` GBytes cache, `png_decode_capped`, the
  PNG-bytes-verbatim contract all live on it).  Textures are made at the
  widget edge and never cached — the pixbuf is the one source of truth, as
  today.  **`gdk_texture_new_for_pixbuf` is DEPRECATED since 4.20** (the
  spike build says so), so the edge is: `gdk_texture_new_from_bytes` over
  the cached `"on-png"` GBytes where they exist (full-res editor images —
  GTK decodes the PNG itself, no pixbuf round trip), and
  `gdk_memory_texture_new` over the pixbuf's pixels for capped/scaled
  decodes (thumbnails, viewer fits) whose bytes no longer match the PNG.

- **D5 · 2026-09-14 — Tree-view drop targets: our own `GtkDropTarget`, PLUS
  `gtk_tree_view_enable_model_drag_dest()` with an EMPTY format set.**
  Measured on 4.22.4/quartz: a `GtkDropTarget` on the deprecated
  `GtkTreeView` works exactly as wanted — `::motion` per pointer move,
  `::drop` ONCE at release with the release coordinates (quirk 13's mid-drag
  data requests do not exist in GTK4).  But `gtk_tree_view_set_drag_dest_row()`
  — public API, the only way to show the drop indicator — SEGFAULTS on the
  next paint unless `enable_model_drag_dest` has run: the indicator branch
  of `gtk_tree_view_bin_snapshot` reads `TreeViewDragInfo->cssnode`, and only
  `enable_model_drag_dest` creates that struct and its `dndtarget` CSS node
  (GTK3 drew the indicator with a style class on the widget, hence no such
  dependency).  Verified in the disassembly (NULL+0x20 after
  `g_object_get_data("gtk-tree-view-drag-info")`) and in
  `gtk/deprecated/gtktreeview.c` 4.22.4.  Workaround: call
  `enable_model_drag_dest(view, gdk_content_formats_new(NULL, 0), 0)` once at
  build — its built-in `GtkDropTargetAsync` matches nothing and never fires,
  our target does all the work, the node exists.  Worth filing upstream.
- **D6 · 2026-09-14 — `gtk_text_view_add_overlay` replaces
  `add_child_in_window` outright; quirks 1 and 2 reduce to "add the top
  margin".**  Measured: the child is allocated at `buffer_y + top_margin −
  scroll` on EVERY allocation (settled readings: delta = 12 = the margin, at
  every scroll position), so it rides scrolling at 1× and there is no
  first-allocation special case.  Place with `add_overlay(view, w, x,
  buffer_y)` — GTK adds the margin — and `move_overlay` the same way.
  Measurement note for anyone repeating it: `vadjustment::value-changed`
  fires BEFORE the re-layout, so a reading taken there shows the previous
  frame's position; read in a tick callback or after settle.
- **D7 · 2026-09-14 — Quirk 15 persists on GTK4 and the GTK3 remedy ports
  as-is.**  The deprecated tree view's own click gesture still collapses a
  multi-selection on an unmodified press (measured: `1 row(s) selected` at
  drag start without the fix).  Recipe (spike, `on_press_capture` et al.):
  a `GtkGestureClick` in the CAPTURE phase (runs before the view's own
  bubble-phase gesture) that, on an unmodified press over an already-selected
  row with ≥2 selected, installs a vetoing `GtkTreeSelectionFunc`; lifted in
  `GtkDragSource::prepare` (a drag began — selection kept, measured 2 at
  prepare AND 2 at drop), in `::released` (a plain click — collapse via
  `gtk_tree_view_set_cursor`), and in `::cancel`.  Modifier state comes from
  `gtk_event_controller_get_current_event_state`.
- **D8 · 2026-09-14 — In-process drag content is a typed `GValue`.**
  `gdk_content_provider_new_typed(G_TYPE_INT64, id)` + `gtk_drop_target_new
  (G_TYPE_INT64, …)` transfers within the process with no serialization.  The
  app needs kind + ids (one folder, or N notes): Phase 5 defines ONE boxed
  GType for that and both views use it.  No MIME types, no `GdkContentFormats`
  matching, nothing cross-process — the sidebar never accepts external drops.

- **D9 · 2026-09-14 — The native macOS menubar is GTK's, not a library's.**
  Measured: `gtk-shell-shows-menubar` = 1 on quartz, `gtkapplication-quartz.c`
  in both 3.24.52 and 4.22.4, the app menu bound to `app.about` /
  `app.preferences` / `app.quit` by `gtkapplication-quartz.ui`.  So
  `gtk_application_set_menubar` is THE menubar on every platform: native on
  macOS, drawn by the GtkApplicationWindow on XFCE (editors set
  `show-menubar` FALSE).  The `native_menubar` setting survives as a macOS
  choice between that and an in-window bar over the same model.
- **D10 · 2026-09-14 — Dynamic menu labels are `hidden-when` items, never
  model edits.**  MacPorts' gtk3 patch (`patch-gtk-menu-crash.diff`) guards
  `*change_point != NULL` at the top of `gtk_menu_tracker_remove_items()`,
  which every append to a live section legitimately trips (a Gtk-CRITICAL,
  harmless — reproduced in 40 lines, in-window and native).  Two items with
  `hidden-when=action-disabled` go through the tracker's visibility path
  instead, and GTK4's GtkPopoverMenu honours the attribute the same way.
  GTK's own `set_menubar` append still prints one; `quartz_log_filter`
  drops it.
- **D11 · 2026-09-14 — Editor editing actions are gated on the view's
  focus** (`editor_actions_set_editing`), because a window accelerator
  fires for any focus widget and a disabled action is skipped by the accel
  lookup.  Carries straight into GTK4 (`GtkEventControllerFocus` on the
  view instead of `focus-in/out-event`).
- **D12 · 2026-09-14 — Development runs use `make run-dev` only.**  The
  binary-adjacent ini names the user's real database; the sandbox `dev/`
  has its own ini + seeded throwaway db, and the IPC socket is per database
  so a dev instance and the real one coexist.  Applies to every phase.

- **D13 · 2026-09-15 — `<Primary>` is Control on GTK4, everywhere.**
  `gtkaccelgroup.c: is_primary → GDK_CONTROL_MASK`, no platform branch;
  Command is `GDK_META_MASK`.  `on_app_install_accels` spells the table's
  `<Primary>` as `<Meta>` on macOS and `<Control>` elsewhere.
- **D14 · 2026-09-15 — Context popovers are parented to the window's child
  box, never to the widget that was clicked.**  Measured: a popover
  parented to a deprecated GtkTreeView trips `gtk_css_node_insert_after`
  on every popup (the view keeps its header buttons under a private
  sub-node) and came up squashed with a scrollbar; parented to a GtkBox it
  gets its natural size even in a cramped window.  A GtkTextView disposing
  with a foreign child never returns.  `on_app_menu_popup` translates the
  point with `gtk_widget_compute_point`, holds its own reference and drops
  the popover on the parent's `unrealize` if the window goes first.
- **D15 · 2026-09-15 — Drawing over/under the text is `snapshot_layer`,
  not a `snapshot` override.**  GtkTextView draws through an internal
  child, so a cairo node appended after chaining up `snapshot` never showed.
  `snapshot_layer(BELOW/ABOVE_TEXT)` is called under the same translation
  as the text: BUFFER coordinates, no conversion.
- **D16 · 2026-09-15 — GTK4 does not paint a paragraph background on a
  line holding only its newline** (pixel-probed; GTK3 did).  The editor
  shades empty code lines itself in the BELOW_TEXT layer, from the line's
  first-character x to the view width less the tag's right margin — the
  rectangle GTK uses for the text lines.
- **D17 · 2026-09-15 — An overlay can never be removed from a GtkTextView
  in 4.22.**  `gtk_text_view_remove` walks anchored children only and warns
  "is not a child" for an overlay (the removal that would work is in the
  private GtkTextViewChild).  The code-block copy links are POOLED: hidden
  when their block goes, reused when one appears.
- **D18 · 2026-09-15 — GtkCellRendererPixbuf paints a texture at
  `-gtk-icon-size`, 16 px by default, whatever the cell reserves.**
  Pixel-probed: a 140 px texture painted 16 px square.  The rule
  `iconview.<class>.image { -gtk-icon-size: Npx }` (the renderer saves the
  view's context with the "image" class) fixes it; textures stay 1× since
  the renderer lays a texture out at its pixel size.  The hovered cell has
  no theme rule either — `iconview.<class>.cell:hover` supplies one.
- **D19 · 2026-09-15 — The editing-action gate hangs on the widgets that
  own keys, not on the view's focus.**  A popover menu takes the keyboard
  focus while open, so gating on the view greyed out the very Insert /
  Styles items being opened.  `editor_gate_widget` puts a focus controller
  on the find entry and every table cell: enter disables, leave enables.
- **D20 · 2026-09-15 — Joining lines keeps the FIRST line's paragraph
  style** (`join_para`, delete-range before/after).  GtkTextBuffer keeps
  the second line's newline — and its tag — so backspacing an emptied code
  line into the body line above turned that line into the code block.  A
  latent GTK3 bug too, invisible there because empty code lines were not
  shaded; port back to `main`.
- **D21 · 2026-09-15 — GTK4 emits a window's "destroy" AFTER its dispose
  has torn the child tree down** (gtkwindow.c → gtkwidget.c), the reverse
  of GTK3.  Anything a destroy handler touches must be its own reference
  (the image viewer holds its panel), never a widget looked up from the
  window.
- **D22 · 2026-09-15 — Theme differences papered over per widget:** every
  GtkFrame is rounded 8 px (table cells get `border-radius: 0`); GtkDialog's
  action area has no padding (`window.notes-dialog .dialog-action-area`);
  GtkEntry's min-width ignores width-chars (`min-width: 0` on the emoji
  entry); `-gtk-icon-size` above.  Apple Color Emoji still overdraws its
  advance and the letter-spacing pad still works — but Pango drops the
  spacing at a line end, so a TRAILING emoji sits 2 px under the caret
  until the next character is typed (GTK3 identical).

- **D23 · 2026-09-15 — Presses in an anchored child GtkTextView must be
  stopped at the child.**  GtkTextView's own press handler calls
  `gtk_widget_grab_focus` unconditionally and does NOT claim a plain press
  (gtktextview.c `gtk_text_view_click_gesture_pressed`), so after a table
  cell took the focus the OUTER view's handler ran and took it back —
  clicks into cells "took" every third or fourth time.  A bubble-phase
  legacy controller on the cell returns TRUE for button press/release
  once the cell's own gesture has run (all of a widget's controllers run
  before the verdict, gtkwidget.c `gtk_widget_run_controllers`).  Claiming
  instead would deny the cell's own gesture — and a claim from a
  CAPTURE-phase ancestor cancels the CHILD's sequences
  (`gtk_widget_propagate_event_sequence_state`).
- **D24 · 2026-09-15 — Emoji padding tags the emoji ONLY.**  Measured on
  4.22's Pango: letter-spacing on the emoji alone widens its advance by the
  full spacing and clears the overdraw; tagging the following character too
  (the GTK3 recipe, from when Pango dropped the run-edge half) widens the
  follower as well — a hole before the next letter.  CLAUDE.md quirk #12's
  "self + the following char" is therefore wrong for this branch.  In the
  drawn view (doc_layout.c) the spacing is 9 px, not 5: Pango puts half
  of it on each side of the glyph, drops the trailing half at a line end,
  and Apple Color Emoji overdraws its advance by ~4 px, so at 5 the next
  letter touched the emoji and the caret after a line-final emoji stood
  inside it.  9 clears the follower, and the caret after a line's last
  emoji is drawn EMOJI_CARET_PAD (4 px) further right on top of that.
  **Superseded on merge to main (2026-09-15)**: the amount is now
  MEASURED — `on_emoji_pad(ctx)` (app.c) reads the ink overhang off a
  sample emoji (13 pt: 5 px) and returns `2 × (overhang + ON_EMOJI_GAP)`;
  the caret pad is derived from it (`L->emoji_caret_pad`).  Same rule,
  same detectors (`on_is_emoji_char`/`on_is_emoji_joiner`), as the
  library's cells — CLAUDE.md quirk #12.

- **D25 · 2026-09-15 — A GtkTextView no longer requests its content
  WIDTH.**  `gtk_text_view_measure` (4.22) reports margins plus anchored
  children horizontally — GTK3 reported the layout width — while the
  vertical measure still uses the layout height.  A table cell with a long
  line therefore sat at its 64 px minimum and hid the overflow.  The table
  sizes its columns itself (`table_fit_columns`: widest line per column by
  PangoLayout, capped at 320 px, cells wrap past the cap and rows grow).

- **D26 · 2026-09-15 — Text on macOS: cairo renderer, metric hinting
  off.**  Compared side by side on a 2x display: GTK's default
  `gtk-hint-font-metrics` rounds glyph advances to whole logical pixels
  (uneven text); off, the GL renderer's glyph cache clips the left column
  of glyphs at fractional positions (a "G" loses a pixel) and its subpixel
  path avoids that only by drawing heavier; the cairo renderer draws
  glyphs straight through Pango — even, intact, the same grayscale weight.
  `main.c` sets `GSK_RENDERER=cairo` (unless set) on macOS and
  `gtk-hint-font-metrics` FALSE.  GTK 4.22's settings never request
  subpixel antialiasing (`settings_update_font_options`: NONE/GRAY only),
  and `gtk_widget_set_font_options` is deprecated — not used.
- **D27 · 2026-09-15 — An anchored child's height is measured when the
  layout validates the line, and a table's cells (GtkTextViews) report a
  stub height until THEY validate — a race the outer layout loses about
  one run in three.**  A table inserted on an empty last line was drawn
  past the bottom with no scroll range.  `table_fit_columns` therefore
  sets every cell's HEIGHT (text wrapped at the column width, measured by
  PangoLayout) as a size request too, so the grid's size never depends on
  validation order; and the anchor inserts scroll from the view's
  `size_allocate` (`scroll_on_allocate`), never from an idle, which raced
  GTK's validation idle.  Probe: 8/8 correct, was ~4/6.
- **D28 · 2026-09-15 — Grid hover CSS must not outrank the selection.**  A
  `.cell:hover` rule beats the theme's `iconview:selected`; a translucent
  background there hid the white selected text.  Tint only
  `:hover:not(:selected)`; the outline uses a fixed dark colour, not
  `currentColor` (white on a selected cell).
- **D29 · 2026-09-15 — One text-view overlay makes every anchored child
  unclickable.**  `gtk_text_view_add_overlay` wraps its overlays in a
  GtkTextViewChild covering the whole text area, parented AFTER the
  anchored children (`ensure_child`, gtktextview.c); it does not override
  `contains`, and `gtk_widget_pick` walks children LAST to FIRST, so once
  a note had a code block (a "copy" link = an overlay) every table cell
  and image under it picked as the GtkTextViewChild — clicks never reached
  the cell.  A note without code blocks was fine, which is why a freshly
  inserted table "worked" and a loaded one "didn't".  Measured with the
  real editor window headless: `pick` on cell 0 = GtkTextViewChild
  (bounds 0,45 958x441).  Fix: `gtk_widget_set_can_target(FALSE)` on the
  link's parent right after `add_overlay`; the links never took their own
  clicks anyway (`on_view_pressed` hit-tests them), so the only thing lost
  is the label's cursor, now served by the view's motion controller
  (`on_view_motion`).  Upstream-worthy with the probe.
- **D30 · 2026-09-15 — A paragraph style on the LAST line needs a newline
  to live on.**  Line-spanning tags cover the trailing "\n"
  (`line_span`); the buffer's last line has none, so H1/H2/code applied
  there ended at the last character and a typed Enter — which inherits its
  tags from both neighbours — came out untagged, breaking the block.
  `apply_paragraph_format` now gives a last line without a newline one
  (cursor kept in place), the same as it already did for an EMPTY last
  line.  Harness: "hello" + code + Enter → a second code line.

- **D31 · 2026-09-15 — An input method's client widget is set at REALIZE,
  never at construction.**  The macOS method resolves the widget's root
  surface the moment it is told (`gtkimcontextquartz.c`
  `quartz_set_client_surface`: `gtk_widget_get_root` → `gtk_native_get_
  surface`); a widget not yet in a window yields NULL, and
  `quartz_filter_keypress` then refuses every key for the widget's whole
  life — the drawn note view could not be typed into at all.  GtkTextView
  sets it in `realize` and clears it in `unrealize`; the note view does the
  same (`note_view_realize`).  System Events keystrokes DO reach GTK4
  windows (they doubled every other character in a GtkEntry too — an
  automation artefact, not a widget bug); accessibility CLICKS do not.

- **D32 · 2026-09-15 — Tooltips are all one width (`on_app_set_tooltip`),
  because the macOS backend does not follow a resize of the hidden
  tooltip popup.**  Sweeping along a toolbar, the second tooltip came out
  CUT OFF: the popup NSWindow had the new width (CGWindowList: 311) but
  the drawing was the previous tooltip's width (140), and GTK's own
  numbers were all right — logged from a custom tooltip widget after it
  mapped: surface 311, window allocation 311, label allocation 289.  So
  GTK drew 311 px and the screen showed 140: the view's tile layer was
  never re-laid out.  `GdkMacosView` marks its layer dirty only in its
  own `setFrame:` override; a hidden window's content view is resized by
  AppKit through autoresizing, which bypasses it, and the layer keeps the
  old tiling until something else re-lays it out (about a second later —
  a tooltip shown ≥ 1 s after the previous one hid was fine, one shown
  within GTK's browse-mode window was not).  No GTK-side call helps:
  queue_draw / queue_resize / gdk_surface_queue_render at 50–400 ms after
  map left it cut; hide+show corrupts the surface (Gdk-CRITICAL).  Two
  workarounds measured clean: a fixed tooltip width (the surface never
  resizes) and a DELAY — refusing a tooltip asked for within 550 ms of
  the previous one hiding and asking again after (past GTK's 500 ms
  browse window, so it shows through the normal hover delay, about a
  second after the last one hid; 400 ms, shown at 464, was still cut).
  The delay shipped (natural widths kept; consecutive tooltips a beat
  slower than browse mode).  All 24 tooltip sites use the helper.
  Upstream: GdkMacosView should mark the layer dirty from `setFrameSize:`
  too.

- **D33 · 2026-09-16 — The list widgets: GtkColumnView / GtkListView /
  GtkGridView over GListModels, no GtkTreeView anywhere.**  Item objects
  are the three plain GObjects in `list_rows.h`; rows are built by
  GtkSignalListItemFactory setup/bind pairs, an in-place change is
  `on_row_touch` (items-changed for the one item — the views rebind it,
  a GtkSortListModel re-sorts it, a GtkMultiSelection keeps it selected
  by object identity, verified in gtkmultiselection.c), a rebuild is one
  `g_list_store_splice`.  What the port bought: (1) the notes list and
  grid share ONE GtkMultiSelection and ONE sorted model, so a header
  click sorts the grid and a selection made in one view is the other's;
  (2) quirk #15 is gone — GtkListFactoryWidget selects on RELEASE, so a
  press on a selected row that becomes a drag never collapses the
  selection, and the capture-phase veto (D7) is deleted; (3) the autofit
  measuring pass is gone — a column view sizes columns to content; (4)
  the drop indicator is a CSS class on the sidebar row under the pointer
  (one GtkDropTarget per row), so the D5 `enable_model_drag_dest`
  workaround is gone; (5) header menus are
  `gtk_column_view_column_set_header_menu` over stateful
  `win.column-<cfg>-<key>` actions; (6) the sidebar fit reads the list
  view's natural width instead of walking the model with Pango.  Two
  things measured on the way: a column's factory cannot reach the row
  widget, so row-wide controllers go on every cell; and a list view
  measures only realized rows, so the startup fit is queued from `map`
  (an idle from the constructor saw a 27 px placeholder) — and, since
  the list view creates and collects row widgets in its size_allocate,
  every fit waits for the frame clock's after-paint (an idle after a
  collapse still measured the old rows); the item manager reuses the
  widget of an item re-added by items-changed WITHOUT rebinding it
  (gtklistfactorywidget.c: `item != old_item`), so an in-place change
  also emits the row's "changed" signal, which the factories from
  `on_row_factory_new` rebind on; and a drop handler must not rebuild
  the models it is dropping onto — GTK reports a drop whose target
  widget vanished as cancelled (the icon floated back), so refresh_all
  is deferred to an idle.  GtkDialog
  (two uses) became the `dialog_new` scaffold over a plain GtkWindow, the
  density GtkComboBoxText a GtkDropDown, and the thumbnail card's
  `gdk_cairo_set_source_pixbuf` a `gdk_texture_download` into a cairo
  surface.  Behaviour changes to know: the sidebar's startup fit sizes to
  the VISIBLE rows (the tree view measured the whole collapsed model);
  the Due Date urgency colour is set at bind time rather than at draw
  time, so it rolls over at midnight on the next refresh rather than on
  the next paint.

- **D34 · 2026-09-16 — Double-clicks are counted by the app, not by
  GtkGestureClick's n_press** (`on_app_double_click_watch`, app.h).
  Reported as "sometimes a double-click opens the note, sometimes it
  takes three or four".  Root cause, in GDK's macOS backend
  (gdkmacosdisplay-translate.c, `fill_motion_event`): a motion event's
  button state is read from `[NSEvent pressedMouseButtons]` when the
  event is TRANSLATED, not taken from the NSEvent — so the small drag a
  finger makes during the first click, if GTK gets to it after the
  button is already up (the release already queued behind it), arrives
  as a motion with NO button held.  GtkGestureSingle resets an active
  gesture on exactly that (`button == 0` → `gtk_event_controller_reset`),
  which zeroes GtkGestureClick's count; the second press is then a first
  press.  Reproduced in a bare GtkDrawingArea (press n=1, motion 2 px,
  STOPPED, CANCEL, then n=1 again) with a synthetic down → 2 px drag → up
  posted back-to-back, and in the app (GTK counted n=1 on the second
  press while the watcher paired it at 122 ms / 2 px and opened the
  note).  So the count lives outside any gesture: a capture-phase click
  gesture per row/cell remembers the last press's time and position as
  its own data, pairs the next press by the gtk-double-click-time and
  -distance settings, and CLAIMS the sequence so GTK's own activation
  cannot open the note a second time when its count did survive.  Wired
  on the notes list cells, the grid cards, the Action Items text and due
  cells (due → calendar, text → open at the item) and the search results;
  the views' "activate" signals stay for Enter.  A drag beyond the DnD
  threshold still starts a drag and never double-clicks — that is a
  drag.

- **D35 · 2026-09-16 — No tooltip in an inactive window** (the
  `gtk_window_is_active` gate in `on_query_tooltip`, app.c).  Reported
  as: with an editor over the library, hovering the editor's TITLE BAR
  lit up the library's toolbar buttons underneath, and a moment later
  the library came to the front over the editor being typed in.  Two
  backend facts, measured with an emission hook on `GdkSurface::event`
  (the only place that sees every event — GTK's own handler returns
  TRUE, so a plain signal connection sees only what GTK left alone):
  (1) the macOS backend does not trust the NSEvent's window; it picks
  the surface itself (`find_surface_under_pointer`,
  gdkmacosdisplay-translate.c) from its front-to-back surface list by
  CONTENT rectangle — a title bar is outside every surface, so a pointer
  on the editor's title bar is delivered to the library behind it, with
  coordinates translated into the library.  That list is a cache rebuilt
  from `[NSApp orderedWindows]` only when a window becomes key/main or a
  surface shows/hides, so a raise that changes neither (an Accessibility
  AXRaise) leaves EVERY pointer event, presses included, going to the
  window behind until the next key change.  (2) GTK shows a tooltip as a
  popup surface, on macOS a CHILD NSWindow of the hovered window shown
  with `orderFront:` — and AppKit brings the parent up with it: the
  library's tooltip, triggered through the editor's title bar, ordered
  the library above the editor (three windows in the list: the tooltip,
  the library, the editor).  Nothing app-side can change where GDK
  delivers a hover — the toolbar buttons still light up under the title
  bar — but the raise came from the tooltip, so a window that is not
  active shows none.  A press on the title bar is safe: the backend drops
  presses above the content (`*y < 0`).

## Session log

One line per session: date, phase, item, outcome.

- 2026-09-16 — D35: no tooltip in an inactive window — the library's
  tooltip, hovered through an editor's title bar, raised the library.
- 2026-09-16 — D34: double-clicks counted by the app; the GDK macOS
  backend's buttonless drag motion resets every GtkGestureClick.
- 2026-09-14 — plan written; survey numbers above.
- 2026-09-15 — BLOCK_MODEL.md written: the OnDocument + drawn-view design
  that retires the anchored-widget family; no code yet.
- 2026-09-15 — Block model step 1: `bnbf.[ch]` + `document.[ch]` (GLib
  only), `make test` (25 tests, fuzzed undo/redo), `make bnbf-scan`; the
  live database (copy) round-trips 1315/1354 byte-identical, the rest
  explained, 0 unexplained.  Findings in BLOCK_MODEL.md "Step 1 — done".
- 2026-09-15 — Block model step 2: export, CLI content commands, headless
  action rewrites and the grid thumbnail read the OnDocument; outputs
  checked byte-for-byte against the old binary (the one difference is an
  old exporter bug: task lines lost their box).  Export 35 s → 2 s.
- 2026-09-15 — Block model step 3: `doc_layout.[ch]` + `note_view.[ch]`
  rewritten as a drawn widget; `serialize.[ch]` down to the blob walks;
  `make ui-test` (tests/ui_probe.c) drives the view from scripts.  No
  GtkTextView remains in the app; D15–D17, D19–D20, D23–D25, D27, D29–D30
  describe a widget no longer used.  D31: typing was dead until the IM's
  client widget moved to realize.
- 2026-09-15 — Loaded tables unclickable (D29, the overlay container),
  code block on the last line (D30), `make run-dev` re-seeding the sandbox
  every run (order-only prerequisites).
- 2026-09-15 — Text rendering (D26), table sizing and anchor scroll
  (D27), grid hover (D28), status colour; all verified by hand.
- 2026-09-15 — Table column fit (D25), CLAUDE.md rewritten, README +
  BUILD.md, `make app` builds against gtk4.  Everything but XFCE done.
- 2026-09-15 — CSS sweep, icon theme, note-view extraction (D23, D24);
  all verified by hand in the sandbox.
- 2026-09-15 — Per-file port joined and running in the sandbox; first two
  rounds of hand verification on macOS: menus, shortcuts, DnD, dialogs,
  grid, tables, code blocks, emoji, viewer all pass after D13–D22.
- 2026-09-14 — Phase 2 complete on `main` (facad9e), verified in the dev
  sandbox on macOS; D9–D12; "Lost permanently #2" struck.  gtk4 rebased.
- 2026-09-14 — Phase 0 complete, verdict GO.  gtk4 4.22.4 +quartz installed;
  spike measured Q1/Q2/Q3 (D5–D8); one GTK crash found and worked around
  (D5); `gdk_texture_new_for_pixbuf` deprecation folded into D4.
