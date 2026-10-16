# Notes (GTK4) -> AppKit fork: porting map

Source tree: `/Users/ian_campbell/salt_development/notes` at `main` = `51ec5a4`
(v5.0.0, VERSION file). All `file:line` references are to that tree. Read-only
research; nothing in notes/, tasks/ or files/ was modified, nothing was built
or run (the installed GUI, pid 56318, was running on the real database the
whole time, so no CLI command was executed either).

This map describes the GTK app.  Its folders, folder AI modes and the
action-item "context" are gone from the port: tags shared with Tasks
replaced them (`porting/INTERFACES.md`, "Tags replace folders").

How this was produced: I read in full `document.h`, `bnbf.h`, `db.h`,
`serialize.[ch]`, `doc_layout.[ch]`, `note_view.[ch]`, `ipc.[ch]`, `main.c`,
`app.h`, `cli.h`, `export.h`, `backup.h`, `search_query.h`, `editor_window.h`,
the Makefile, BLOCK_MODEL.md, and most of CLAUDE.md; I read the relevant parts
of `db.c`, `cli.c`, `app.c`, `editor_window.c`. `library_window.c`,
`list_rows.[ch]`, `settings_window.c`, `search_window.c`, `media_window.c`,
`image_viewer.c`, the rest of `app.c`/`editor_window.c` were read in full by
two sub-agents whose findings are merged into section 4 (marked where a claim
is theirs and I did not re-verify it). `document.c`, `export.c`, `backup.c`
and most of `cli.c` and `db.c` were outlined and grepped, not read line by
line. See "Not verified" at the end.

---------------------------------------------------------------------------

## 0. Key numbers and the one-paragraph verdict

| | |
|---|---|
| `src/` | 20 `.c` (28,764 lines) + 19 `.h` (3,758 lines) = 32,522 lines |
| GTK/GDK/Pango/GSK-bearing `.c` files | 11 (17,575 lines): app, main, doc_layout, note_view, editor_window, library_window, list_rows, search_window, media_window, image_viewer, settings_window |
| "core" `.c` files (no `gtk_` call) | 9 (10,689 lines): bnbf, document, db, serialize, export, backup, cli, ipc, search_query |
| files with NO GLib | none. Every file in src/, tests/ and tools/ uses GLib types and functions |
| GLib call sites in the 9 core files | 1,129 `g_*(` calls (163 distinct functions) + ~2,065 GLib type tokens (`gchar`, `gint64`, `GString`...) |
| GTK calls (`gtk_*(`) | 1,239 total; library_window.c alone 493 |
| Pango calls | 94 in doc_layout.c, 13 in library_window.c, 4 media, 3 image_viewer, 3 app.c, 1 note_view.c |
| tests | `tests/test_document.c` 35 GLib-only tests (g_test); `tests/ui_probe.c` + 4 scripts (needs GTK + display) |
| deps | gtk4 4.22.4, glib 2.88.2, pango 1.57.1, cairo 1.18.4, gdk-pixbuf 2.44.6, sqlite 3.53.4 (all MacPorts, linked from /opt/local/lib) |

Verdict: the layering is good where it matters most. The document model
(`document.c` + `bnbf.c`) and the database (`db.c`) are toolkit-free apart
from GLib, proven by the Makefile (`MODEL_SRCS`, Makefile:167-180) and by
their include lists. The editor is NOT a GtkTextView: it is one custom-drawn
widget (`note_view.c`) over a geometry module (`doc_layout.c`) that owns every
Pango call. That seam is narrow and maps 1:1 onto CoreText. The rest of the
"core" (cli, ipc, export, backup, serialize) is GLib-only in its code but is
welded to GTK through headers (`app.h` includes `<gtk/gtk.h>`,
`serialize.h` includes gdk-pixbuf) and through four upward calls
(`cli.c` -> `on_editor_action_set_*`, `ipc.c` -> `on_library_quicknote` /
`on_editor_window_open` / `gtk_window_present`). Those are small, mechanical
cuts. The big cost is the UI rewrite itself: ~17.5k lines of GTK, of which
library_window.c is 6.2k.

---------------------------------------------------------------------------

## 1. Module inventory

Counts are from grep over the working tree: `g_calls` = `\bg_[a-z0-9_]+\(`
call sites; `gtypes` = GLib type tokens (gchar/gint/GString/GList/TRUE/FALSE...);
`gtk` = `gtk_*(` calls; `gdk` = `gdk_*(` (includes gdk_pixbuf_*); `pango`,
`cairo`, `gsk` (gsk_/graphene_/GRAPHENE_), `sqlite` = `sqlite3_*(`.

Classification: (a) GTK-free and GLib-free; (b) GLib/GIO only; (c) GTK/Pango/
Cairo/GDK UI code. Nothing is class (a).

### src/*.c

| File | Lines | g_calls | gtypes | gtk | gdk | pango | cairo | gsk | sqlite | Class | Purpose |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| bnbf.c | 410 | 34 | 144 | 0 | 0 | 0 | 0 | 0 | 0 | b | BNBF v6 record reader/writer, `on_list_prefix_chars`, `on_action_split_due`, `OnTable`. GLib: GByteArray, GPtrArray, GDateTime/g_date_valid_dmy (due-date parse, bnbf.c:119-126), g_ascii_*, g_str* |
| document.c | 2685 | 228 | 554 | 0 | 0 | 0 | 0 | 0 | 0 | b | The block model: OnText/OnBlock/OnDocument, loader (528-957), saver (958-1100), operations + undo log (1101-1911), ranges/fragments (1912-2348), derived reads incl. action rewrites (2349-2685). GLib: GPtrArray(68), GArray(53), GString(34), GBytes(10), GHashTable(7), GList, g_utf8_*, GDateTime (document.c:2656-2658) |
| db.c | 2625 | 179 | 484 | 0 | 0 | 0 | 0 | 0 | 262 | b | SQLite layer: schema + migrations (22-184, 404-504), stmt cache, folders/notes/tags/trash/counts, tasks tables + `note_task` + `action_items_ctx` view + tombstone trigger, health check (sync + worker thread, 747-851), SHA-256 (853-880), online backup (506-538). GLib: GHashTable(26), GString(26), GList(17), GArray, GChecksum, **g_thread_new + g_idle_add** (db.c:821-841), g_get_user_data_dir (396), g_get_real_time, g_usleep, g_fopen, g_mkdir_with_parents |
| search_query.c | 227 | 28 | 49 | 0 | 0 | 0 | 0 | 0 | 0 | b | Query language (ANDed words, "phrases", -exclusions, regex mode). GLib: **GRegex** (PCRE2 semantics), g_utf8_casefold, GArray, GString |
| serialize.c | 323 | 42 | 101 | 0 | 9 | 0 | 0 | 0 | 0 | b + gdk-pixbuf | Blob-side helpers: `on_note_document_load/save/reconcile` (218-323, pure GLib+db+document), image ordinal walks (88-141), and the **GdkPixbuf** decode/encode/probe (16-86, 143-187). No GTK widget code, but `serialize.h:18` includes `<gdk-pixbuf/gdk-pixbuf.h>` and three signatures use `GdkPixbuf *` |
| export.c | 612 | 142 | 106 | 0 | 0 | 0 | 0 | 0 | 0 | b (header leaks GTK) | HTML + Markdown export walking the OnDocument. Uses only `app->db` (5 sites). GLib: GString(81), g_markup_escape_text, g_base64_encode, g_file_set_contents, g_mkdir_with_parents, GHashTable. `export.h:19` includes `app.h` -> `<gtk/gtk.h>` |
| backup.c | 480 | 82 | 77 | 0 | 0 | 0 | 0 | 0 | 0 | b (header leaks GTK; uses main loop) | Rotating backups: worker **g_thread_new**, **g_idle_add** x4, **g_timeout_add_seconds_full** timer, g_dir_*, g_stat, GDateTime. Calls `on_app_status` x2, `on_app_config_get/_get_bool/_set` x7, touches `app->backup_running/backup_timer` |
| cli.c | 2621 | 269 | 394 | 0 | 1 | 0 | 0 | 0 | 0 | b (+1 gdk-pixbuf call; calls up into editor_window.c) | Noun-verb CLI, `--json`, dispatch. `gdk_pixbuf_new_from_file` (cli.c:1007, `note add-image`). Calls `on_editor_action_set_done/due/text` (cli.c:1764,1797,1846), `on_ipc_*`, `on_export_*`, `on_query_*`, `on_app_config_load_db_dir`, `on_app_read_stream`. Includes `app.h`, `editor_window.h` (cli.c:16-18) |
| ipc.c | 706 | 125 | 156 | 1 | 0 | 0 | 0 | 0 | 0 | b (GIO) + 3 UI calls | Unix-socket client/server. **GIO**: GSocket, GSocketService, GUnixSocketAddress, GSocketConnection, G{Input,Output}Stream (~40 call sites). UI: `on_library_quicknote` (253), `on_editor_window_open` (270), `gtk_window_present` (273), `app->notify_notes_changed` (447-449) |
| app.c | 1097 | 138 | 199 | 88 | 18 | 3 | 0 | 2 | 0 | c (mixed) | Shared context. Toolkit-free parts worth lifting: config/ini (862-1016, **GKeyFile**), `on_app_read_stream` (999-1010), `on_is_emoji_char/joiner` (31-47), `on_app_location_text`, `exe_dir_from_argv0` (589-599). GTK parts: accel table (518-582), icons, toolbar factory, tooltip hack, menu popup scaffold, select-on-press, double-click watch, file picker, notice, CSS |
| main.c | 516 | 42 | 51 | 17 | 2 | 0 | 0 | 0 | 0 | c | `main()`, GtkApplication, startup/activate, first-run Welcome chain, SIGTERM, macOS log filter |
| doc_layout.c | 1734 | 107 | 470 | 16 | 3 | 94 | 0 | 17 | 0 | c | Geometry + painter: one PangoLayout per text block/cell, prefix-sum y, hit test, cursor movement, snapshot painter |
| note_view.c | 3047 | 226 | 590 | 135 | 24 | 1 | 0 | 3 | 0 | c | `OnNoteView`, the custom-drawn editor widget (GtkScrollable + GtkAccessibleText), input, IME, clipboard, #tag popup, context menu, find |
| editor_window.c | 1735 | 89 | 226 | 101 | 5 | 0 | 0 | 0 | 0 | c | Editor window chrome: toolbar, `win.` actions, find bar, autosave, status bar, viewer host, and the toolkit-independent `action_apply` (484-575) |
| library_window.c | 6197 | 659 | 890 | 493 | 9 | 14 | 30 | 0 | 0 | c | Library window: sidebar, notes list/grid, action items, DnD, menus, dialogs, AI pane, thumbnails. Section map in 4.1 |
| list_rows.c | 220 | 35 | 25 | 3 | 0 | 0 | 0 | 0 | 0 | c (GObject) | Row GObjects (`OnSbRow`, `OnNoteRow`, `OnActionRow`), `on_row_touch`, `on_row_factory_new` |
| search_window.c | 645 | 71 | 79 | 83 | 0 | 0 | 0 | 0 | 0 | c | Search window + worker thread |
| media_window.c | 711 | 51 | 104 | 71 | 0 | 4 | 0 | 0 | 0 | c | Media browser (thumbnail flow box, idle scan) |
| image_viewer.c | 758 | 17 | 93 | 76 | 3 | 4 | 0 | 3 | 0 | c | Modal image viewer overlay shared by editor + media |
| settings_window.c | 1415 | 101 | 126 | 255 | 4 | 0 | 0 | 0 | 0 | c | Settings window incl. Database health plate + backups + AI |

### src/*.h (what each header drags in)

| Header | Lines | Includes | Exposes |
|---|---:|---|---|
| bnbf.h | 243 | `<glib.h>` | GLib types only (gchar, guint32, gint64, gboolean, GPtrArray in `OnTable`, GByteArray in `OnBnbfWriter`) |
| document.h | 543 | `<glib.h>`, bnbf.h | GLib only. Public structs contain `GString *`, `GArray *`, `GPtrArray *`, `GBytes *`, `GDestroyNotify` (document.h:48-104); `GList *` and `GArray *` returns; `GString *why` param |
| db.h | 675 | `<glib.h>`, `<sqlite3.h>` | GLib only. `OnDatabase` exposes `sqlite3 *` and `GHashTable *`. Returns `GList *` (folders/notes/tags/actions), `GHashTable *` (4 maps), `GArray *` (2) |
| search_query.h | 88 | `<glib.h>` | `GError **` in `on_query_new` |
| serialize.h | 174 | **`<gdk-pixbuf/gdk-pixbuf.h>`**, bnbf.h, document.h, db.h | `GdkPixbuf *` in `on_png_decode_capped`, `on_note_image_nth`, `on_image_png_bytes`; `GBytes *` |
| export.h | 62 | **app.h** | `OnApp *` (only `app->db` is used) |
| backup.h | 121 | **app.h** | `OnApp *` |
| cli.h | 139 | **app.h** | `OnApp *`, `OnDatabase *` |
| ipc.h | 80 | **app.h** | `OnApp *` |
| app.h | 583 | **`<gtk/gtk.h>`**, db.h | `OnApp` holds `GtkApplication *`, `GtkWidget *`, `GtkCssProvider *`, `GHashTable *editors`; `PangoContext *` in `on_emoji_pad`; GdkTexture/GdkPixbuf/GdkPaintable/GMenuModel/GtkListItem in helpers |
| doc_layout.h | 165 | **`<gtk/gtk.h>`**, document.h | `PangoContext *`, `PangoAttrList *`, `graphene_rect_t`, `GdkTexture *`, `GdkRGBA`, `GtkSnapshot *`, `GArray *` |
| note_view.h | 207 | app.h, document.h | `GtkWidget`, `GdkPixbuf *`, `GdkTexture *`, `GBytes *`, `GdkModifierType`, `GActionGroup *` |
| editor_window.h | 162 | app.h | `GtkWidget *` returns; `on_editor_action_set_*` are GLib-only signatures |
| library_window.h | 86 | app.h, search_window.h | `GtkWidget *` |
| list_rows.h | 130 | `<gtk/gtk.h>` | GObject types, `GListStore *`, `GtkListItemFactory *` |
| search_window.h 65 / media_window.h 57 / settings_window.h 26 | | app.h | `OnApp *` only |
| image_viewer.h | 152 | `<gtk/gtk.h>` | `GtkWidget`, `GdkPaintable *` in the ops vtable |

### tests/, tools/, dev/, skills/

| Path | Lines | Class | Purpose |
|---|---:|---|---|
| tests/test_document.c | 1846 | b (GLib only; 555 g_ calls, mostly `g_assert_*` + `g_test_*` + `g_rand_*`) | 35 headless tests registered at test_document.c:1810-1844: round trips of every construct, normalizations, text ops, document ops, groups, a 400-op undo/redo fuzz (`--seed`), derived reads, action rewrites, the ACTION record. Built from `tests/test_document.c + src/document.c + src/bnbf.c` against `glib-2.0` only (Makefile:167-176) |
| tests/ui_probe.c | 327 | c (gtk 18, gdk 4, gsk 3) | Drives a real `OnNoteView` from a script via `on_note_view_feed_key/text/click`, asserts block kind/text and caret, renders the window to PNG. Links all app objects except main.o (Makefile:185-189). Needs a display |
| tests/ui/{typing,blocks,objects,navigation}.txt | ~1-1.5 KB each | data | The probe scripts; they double as the editor behaviour spec. Verbs: `text`, `key NAME [mods]`, `inline`, `para`, `undo/redo/table/date/select-all`, `find/step/step-back`, `check BLOCK KIND TEXT`, `caret BLOCK OFFSET`, `print`, `width W` (ui_probe.c:1-25) |
| tools/bnbf-scan.c | 321 | b (GLib + sqlite, 11 sqlite calls) | Round-trips every blob of a database COPY through the model; verdicts identical / identical-old-header / normalized / unexplained / invalid / error; `--diff ID`, `--blocks ID` |
| tools/gtk4-spike/ (spike.c 659, Makefile, README, BUILD, built binary + dSYM) | | c | Throwaway GTK4 Phase-0 spike (tree-view DnD, text-view overlay, multi-select drag). Irrelevant to the fork; do not carry |
| tools/icon-prep.py | 119 | python | Normalises a dropped-in toolbar PNG to 512x512 with flood-filled alpha |
| tools/import-apple-notes.sh | 209 | sh + osascript | Apple Notes -> Notes via the CLI. Uses `folder add`, `note new --folder ... -`, **`note add-image`**, **`note set-modified`** (the latter two are NOT in cli.h's synopsis; they are in `usage()` cli.c:2113-2114 and dispatch cli.c:2343-2346) |
| dev/ (gitignored) | | sandbox | `make run-dev` output: symlinks `notes`, `icons`, `notes.ini.defaults`; its own `notes.ini` (`db_dir=<repo>/dev/db`) and throwaway `db/notes.db` (84 KB). The "1521 dev/notes" in a line count is the symlinked binary |
| skills/notes/SKILL.md | 8.1 KB | doc | The Claude skill (symlinked as `~/.claude/skills/notes`). See section 5.4 |
| icons/ | | assets | 22 toolbar/dialog PNGs loaded by basename, `icons/theme/hicolor` (8 symbolic SVG arrows + 512 px app icon), `icons/unused/` (14 retired PNGs, still copied into the .app by `cp -R icons`) |
| .claude/settings.local.json | 27 KB | | Stale permission allowlist from the orange_notes/blue_notes era. Not relevant |

---------------------------------------------------------------------------

## 2. Layering

### 2.1 Who calls whom

```
main.c ──> app.c, cli.c, db.c, ipc.c, backup.c, library_window.c
library_window.c ──> db, serialize, export, backup, editor_window, search_window,
                     media_window, settings_window, list_rows, app
editor_window.c ──> note_view, image_viewer, serialize, db, document, app
note_view.c ──> doc_layout, document, serialize (on_image_png_bytes), db (tag list), app
doc_layout.c ──> document, app.h (emoji rule only)
search_window.c ──> search_query, serialize, db, editor_window, library_window, list_rows
media_window.c ──> serialize, image_viewer, editor_window, db
settings_window.c ──> db, backup, editor_window, library_window, app
---- "core" ----
cli.c ──> db, document, serialize, export, search_query, ipc, app(config, read_stream)
          ──> editor_window.c (on_editor_action_set_done/due/text)      <-- UPWARD
ipc.c ──> cli (dispatch), db, app(config, read_stream)
          ──> library_window.c (on_library_quicknote)                    <-- UPWARD
          ──> editor_window.c (on_editor_window_open) + gtk_window_present <-- UPWARD
          ──> app->notify_notes_changed                                   (hook, fine)
backup.c ──> db, app (on_app_status, on_app_config_*, OnApp fields), GLib main loop
export.c ──> db, document, serialize (only app->db is read)
serialize.c ──> db, document, bnbf, gdk-pixbuf
db.c ──> sqlite3, GLib (g_thread + g_idle_add for the async health verdict)
document.c ──> bnbf.c ──> GLib only
search_query.c ──> GLib only (GRegex)
```

### 2.2 Does non-UI code call back into GTK? Exactly these places

1. `cli.c:1764, 1797, 1846` call `on_editor_action_set_done/due/text`, which
   live in `editor_window.c:484-575` (`action_apply`). The function itself is
   toolkit-free except for `editor_lookup(app, note_id)` and the three
   `on_note_view_action_*` calls on a live editor. Headless, `app->editors` is
   NULL and only the document path runs (editor_window.c:543-556). This logic
   must move to the core with a hook ("is this note open? then apply to the
   live view") so the CLI no longer links the editor window.
2. `ipc.c:253` `on_library_quicknote(app)`; `ipc.c:270-273`
   `on_editor_window_open` + `gtk_window_present`. Need two app-level hooks
   (`quicknote`, `open_note(id)`).
3. `ipc.c:447-449` and `editor_window.c:812-818` use the `OnApp` function
   pointers `notify_notes_changed` / `notify_note_saved` (app.h:128-130). These
   are already hooks; keep them.
4. `backup.c` posts status through `on_app_status` (a hook-backed printf) and
   schedules with `g_timeout_add_seconds_full` / `g_idle_add`; `db.c:825`
   delivers the health verdict with `g_idle_add`. Not GTK, but they assume a
   running GLib main loop. Under NSApplication there is no GLib loop unless
   one is pumped; these six call sites must become
   `dispatch_async(dispatch_get_main_queue(), ...)` / a dispatch timer (or a
   two-function "main thread shim" the core calls).
5. `serialize.c` / `cli.c:1007`: GdkPixbuf (11 call sites total) for decode
   with size cap, PNG encode, header probe, and file load. ImageIO
   (`CGImageSourceCreateThumbnailAtIndex` with
   `kCGImageSourceThumbnailMaxPixelSize`, `CGImageDestination` to PNG,
   `CGImageSourceCopyPropertiesAtIndex` for size) replaces all of it. Note
   `doc_layout.c:473-493` already reads PNG size from the IHDR by hand; the
   same could replace `on_png_probe_size`.
6. Header-only leaks: `export.h`, `backup.h`, `cli.h`, `ipc.h` include `app.h`
   (-> `<gtk/gtk.h>`), so none of the core compiles without GTK headers today
   even though only `OnApp *` is needed. Fix by splitting `app.h` into a
   toolkit-free `app_core.h` (OnApp's portable fields, config API, status
   hook, read_stream, emoji predicates, location text) and a UI header.

### 2.3 Public API of the non-UI core (what a UI engineer programs against)

All signatures use GLib scalar typedefs (`gchar`, `gint`, `gint64`,
`gboolean`, `gsize`, `guint8`, `guint32`, `gpointer`). None expose Pango or
GTK types EXCEPT serialize.h (GdkPixbuf) and the app.h dependency noted above.

**document.h** (GLib containers in public structs; UI code reads the structs
directly, e.g. `b->text->text->str`, `b->text->images`, `b->cells`):
- Types: `OnBlockKind` {PARA,H1,H2,BULLET,NUMBER,CHECK,CODE,IMAGE,TABLE};
  `OnRun {gsize len; guint32 flags}`; `OnInlineImage {offset, GBytes *png,
  display_width, gpointer pixels, GDestroyNotify pixels_free}`;
  `OnText {GString *text; GArray *runs; GArray *images}`;
  `OnBlock {kind, OnText *text, checked, eol_flags, GBytes *png,
  display_width, pixels, pixels_free, rows, cols, header, GPtrArray *cells,
  gint64 action_uid}`; opaque `OnDocument`; `OnPos {guint block; gint cell;
  gsize offset}` (**byte offsets**; image/table block: 0 = before, 1 = after);
  `OnDocLoadReport`; `OnDocumentObserver {block_changed, blocks_inserted,
  blocks_removed}`.
- Lifecycle/IO: `on_document_new`, `_new_from_block`, `_free`, `_n_blocks`,
  `_block`, `_check`, `on_document_from_bnbf(data,len,rep)`,
  `on_document_to_bnbf(d,&len)`, `on_document_from_text`.
- Operations (each logs its inverse, notifies the observer, returns FALSE on
  bad args): `insert_text`, `delete_text`, `set_flags`, `split_block`,
  `join_blocks`, `set_kind`, `set_checked`, `set_eol_flags`, `insert_block`,
  `remove_block`, `set_image_width`, `table_set_header`,
  `table_insert_row/remove_row/insert_col/remove_col`.
- Ranges: `on_pos_cmp`, `on_document_copy_range`, `_delete_range`,
  `_insert_fragment`, `_last_change`, `_text_at`.
- Undo: `begin_group/end_group` (nestable), `undo`, `redo`, `can_undo`,
  `can_redo`, `undo_depth`, `clear_undo`.
- Observer + change flags: `set_observer`, `take_tags_modified`,
  `take_actions_modified`.
- Derived: `on_block_is_action`, `on_document_title`, `_collect_tags`,
  `_action_blocks`, `_action_ord(uid)`, `_block_action`, `_action_strike/
  due/text`, `_plain_text`, `_image_count`, `_image_nth`.
- OnText/OnBlock primitives: `on_text_new/free/copy/insert/append/delete/
  set_flags/flags_at/add_image`, `on_block_new_text/image/table`,
  `on_block_free/copy/cell`, `on_block_kind_is_text`, `_para_flag`.

**bnbf.h**: `ON_FMT_*` flags and masks (bnbf.h:60-83), `ON_BNBF_VERSION 6`,
`ON_REC_*`, `OnTable`, reader (`on_bnbf_open/next`), writer
(`on_bnbf_writer_init`, `write_text/image/check/action/table_begin/
table_cell`, `writer_finish`), `on_list_prefix_chars`, `on_action_split_due`.

**db.h** (db.h:191-673; ~80 functions). Structs: `OnDatabase {sqlite3 *handle;
gchar *path; GHashTable *stmt_cache; OnDbHealth health; gpointer health_job}`,
`OnFolder {id,name,emoji}`, `OnNoteMeta {id,folder_id,title,updated_at,
created_at,pinned}`, `OnTag {id,name}`, `OnActionItem {note_id,ord,text,done,
due,uid,context}`, `OnDbHealth {ok,ran,when,detail,pending}`.
- lifecycle: `on_db_default_path`, `on_db_open(path|NULL)`, `on_db_close`,
  `on_db_backup_to`, `on_db_verify_file`, `on_db_health_check`,
  `on_db_health_check_async(db, done, data)`, `on_db_health`,
  `on_db_file_sha256`, `on_db_user_version/set_user_version`.
- folders: `create(parent,name,ai_mode,emoji)`, `update`, `rename`,
  `set/get_ai_mode`, `get/set_emoji`, `move`, `reorder(ids,n)`,
  `sort_children`, `delete`, `list(parent)` -> GList, `child_map` ->
  GHashTable, `children(map,parent)`, `path`, `path_map`, `note_ids`.
- notes: `create(folder)`, `notes_delete(ids,n)`, `notes_move(ids,n,folder)`,
  `note_save(id,title,blob,len,body_text)`, `note_load`, `note_get`,
  `note_body_text`, `note_set_body_text`, `note_set_updated_at`,
  `note_list(folder)`, `note_list_all(include_trash)`, `note_list_recent`,
  `note_list_pinned`, `note_set_pinned`, `note_count_pinned`,
  `note_count_visible`, `note_count_map`, `note_text_map(max_chars)`.
- tags: `tag_find`, `tag_delete`, `tag_list`, `note_set_tags(id, GList names)`,
  `notes_by_tag`, `note_tag_list`, `tag_count_map`.
- actions: `action_uid_alloc`, `note_save_actions(note, GList items,
  &changed)`, `action_get(uid)`, `action_set(uid,title,done,due)`,
  `action_uid_at(note,ord)`, `action_find_uid`, `action_list`,
  `action_list_for_note`, `action_counts(&total,&open)`.
- trash: `notes_trash`, `note_restore`, `folder_trash`, `folder_restore`,
  `folder_list_trashed`, `note_list_trashed`, `note_is_trashed`,
  `trash_count`, `trash_note_ids`, `trash_empty`.
- totals: `on_db_totals(&notes,&folders,&tags)`.

**serialize.h**: `on_note_document_load(db,id)`, `on_note_document_save(db,
id,doc,sync_actions,&changed)` (THE save: uids allocated, then blob + title +
body text, then task rows; serialize.c:228-279), `on_note_document_reconcile`
(task rows -> lines; 281-323), `on_note_extract_text`, `on_note_text_cached`,
`on_note_count_images`, `on_note_image_nth_png` (bytes verbatim),
and the three GdkPixbuf functions `on_png_decode_capped`, `on_note_image_nth`,
`on_image_png_bytes` (+ `on_png_probe_size`, pixbuf-loader inside).

**export.h**: `on_export_all(app, dir, ON_EXPORT_HTML|MARKDOWN, &err)` ->
count or -1; `on_export_note(app, id, dir, fmt)`;
`on_export_note_markdown(app, id)` -> string.

**backup.h**: `on_backup_dir()`, `on_backup_ready(&reason)`,
`on_backup_start(app, db_path)`, `on_backup_auto_start(app, db_path)`.

**search_query.h**: `on_query_new(text, case_sensitive, use_regex, GError**)`,
`on_query_free`, `on_query_is_empty`, `on_query_matches(q,title,body)`,
`on_query_highlight_term`.

**cli.h**: `on_cli_run(argc,argv)` (-1 = start GUI), `on_cli_dispatch(app,
argc,argv)`, `on_cli_resolve_folder_path`, `on_cli_set_stdin_data`,
`on_cli_command_reads_stdin`, `on_cli_command_mutates`.

**ipc.h**: `on_ipc_try_remote`, `on_ipc_try_remote_run`,
`on_ipc_set_pending_quicknote/open`, `on_ipc_run_pending`,
`on_ipc_has_pending`, `on_ipc_server_start/stop`.

**app.h, toolkit-free subset** (must be split out): `on_app_config_init(argv0)`,
`on_app_config_get/get_bool/set/get_size`, `on_app_config_load_db_dir`,
`on_app_read_stream`, `on_app_status` (printf -> `app->notify_status`),
`on_app_location_text`, `on_is_emoji_char/joiner`.

---------------------------------------------------------------------------

## 3. The editor / rendering stack

### 3.1 What it is

NOT a GtkTextView. `OnNoteView` is a `GtkWidget` subclass
(`G_DEFINE_FINAL_TYPE_WITH_CODE`, note_view.c:122-126) implementing
`GtkScrollable` and `GtkAccessibleText`, placed in a GtkScrolledWindow by
editor_window.c. It owns three things (note_view.c:71-117): the `OnDocument`,
an `OnDocLayout`, and input state (caret/anchor as `OnPos`, armed inline
flags, IME preedit, #tag capture, drag state, context-hit, find needle). It
has NO child widgets except two popovers (the #tag suggestion list and GTK's
emoji chooser).

- Drawing: one `snapshot` vfunc (note_view.c:568-599): white background,
  translate by -scroll, `on_doc_layout_snapshot(layout, snap, &paint)`, then
  the popover children. All ink is the layout's.
- Size: `measure` returns natural height = `on_doc_layout_height`
  (499-514); `size_allocate` pushes the width into the layout and configures
  the vertical adjustment (516-540).
- Every mutation funnels through `after_edit()` (361-380): clamp caret,
  restart blink, re-run find hits, queue resize, schedule scroll-to-caret,
  sync IME location, emit `"edited"`.
- The document observer is relayed to the layout (288-317) so exactly the
  changed block re-lays out.

`doc_layout.c` computes, per block (struct `BL`, doc_layout.c:57-68): a
`PangoLayout` (text kinds), `x`, `w`, `y` (prefix sum), `h` (incl. 2 px gap);
for IMAGE `img_h`; for TABLE `col_w[]`, `row_h[]` and one PangoLayout per
cell. `validate()` (806-824) lays out every stale block and re-sums y —
eager, no lazy heights (measured 18 ms for the 615-block worst case per
BLOCK_MODEL.md; not re-measured by me).

**Text measurement on `gtk4` is Pango** (pango 1.57 via `PangoLayout`,
HarfBuzz shaping, CoreText font backend on macOS through pangocairo), drawn
with `gtk_snapshot_append_layout`, and main.c:477 forces
`GSK_RENDERER=cairo` on macOS. There is **no FreeType code on `gtk4`**. The
remote branch `origin/freetype-rendering` (c49e425, 2026-07-28) is a stale
GTK3-era branch forked at `6ea13bc` (v3.3.7): a 56-line change adding a
`use_freetype` ini key that swaps `pango_cairo_font_map` between
`CAIRO_FONT_TYPE_FT` and CoreText and sets cairo font options. It predates
the GTK4 port and the block model; nothing of it is in main. Ignore it.

### 3.2 Feature by feature (all in note_view.c unless stated)

| Concern | How it works today | file:line |
|---|---|---|
| Keyboard | `GtkEventControllerKey` with the IM context attached; `on_key_pressed` is a hand-written switch: arrows, Home/End, PageUp/Down, Return, BackSpace/Delete (word variant with Alt on macOS / Ctrl elsewhere), Tab/Shift-Tab (table cell nav, else a tab char), Escape (collapse selection), and primary+A/C/X/V. Modifiers: macOS `primary`/`line` = Meta(Cmd), `word` = Alt | 1876-2002 |
| Formatting shortcuts | NOT in the view. They are `win.` GActions in editor_window.c bound by `on_app_install_accels` (app.c:518-582): Cmd+N new note, Cmd+F find, Cmd+M code block (editor) / media (library), Cmd+Z undo, Cmd+Shift+Z and Cmd+Y redo, Cmd+B/I/U, Cmd+D insert date, Cmd+E emoji, Cmd+Q quit, Cmd+, preferences | app.c:526-548 |
| Typing | IM `commit` -> `insert_typed` (replaces selection, arms inline flags, extends an ON_FMT_TAG run, opens/extends the typing undo group) then `tag_capture_consider` | 710-736, 1683-1696 |
| IME / input method | `gtk_im_multicontext_new`; signals `commit`, `preedit-changed` (preedit string + PangoAttrList + cursor spliced into the caret block via `on_doc_layout_set_preedit`), `retrieve-surrounding`, `delete-surrounding`; `im_reset` before any caret move; `im_sync_location` gives the caret rect; client widget set at realize | 548-566, 1683-1795, 2806-2819 |
| Enter / Backspace policy | lists continue; Enter on empty item ends list; headings are one line; Enter in a cell inserts '\n'; beside an object opens a paragraph; Backspace at item start -> body text; joins; object deletion | 739-890 |
| Caret | `OnPos`; rect from `on_doc_layout_caret_rect`; drawn by the layout painter as a 1 px rect in the text colour; blink via g_timeout using gtk-cursor-blink* settings (on 2/3, off 1/3, stops after timeout) | 603-673; doc_layout.c:932-970, 1729-1733 |
| Selection | anchor + caret; painted as filled rects per line via `pango_layout_line_get_x_ranges`; whole-block highlight for images/tables; cell-range = whole cells; focused/unfocused colours | doc_layout.c:1481-1545 |
| Mouse | `GtkGestureClick` (button 0 = any): 1 click caret (+Shift extends), 2 = word, 3 = visual line; hits on copy-word / checkbox / image are consumed; `GtkGestureDrag` extends selection (**no edge auto-scroll is implemented**, despite BLOCK_MODEL.md's spec); `GtkEventControllerMotion` sets cursor text/pointer/default by hit kind | 2071-2204 |
| Context menu | built per press from the hit: image section (Copy Image, Open, Display Full Size/Thumbnail), table section (Add Row/Column, Remove Row/Column, Header Row [stateful], Delete Table), then Cut/Copy/Paste; shown by `on_app_menu_popup`; actions are the view's own `view.` GSimpleActionGroup | 2015-2069, 2351-2394 |
| Clipboard copy | `GdkContentProvider` union: `application/x-notes-bnbf` (the selection as a BNBF sub-document) + `G_TYPE_STRING` plain text; copy-word copies a code run as text; Copy Image puts a texture | 1213-1237, 2096-2111, 2218-2228 |
| Clipboard paste | `gdk_clipboard_read_async` over an ordered mime list (bnbf, every image mime GDK can decode, text); bnbf spliced whole; images -> PNG bytes -> IMAGE block; text -> one PARA per line (CODE if pasting into code). Plus a macOS-only path reading NSPasteboard directly via `objc_msgSend` (`public.png`, `public.tiff`, `public.utf8-plain-text`) when GDK's cache is stale | 1127-1445 |
| Drag and drop | none in the editor (no drop target for files/images, no text DnD). All DnD is in library_window.c | grep: `gtk_drop_target`/`gtk_drag_source` only in library_window.c (9 hits) |
| Scrolling | GtkScrollable: the view only drives `vadj` (upper = content height); wheel/trackpad scrolling is the GtkScrolledWindow's; `scroll_to_caret` after caret moves; `reveal_pos` puts a target 30% down the page | 400-493, 2555-2566 |
| Images | block images and inline images (U+FFFC + `PangoAttrShape`); size from the PNG IHDR, decoded lazily to `GdkTexture` on first draw and cached on `OnBlock.pixels`; default fit 200x125 box, "full size" = stored display width; click emits `"image-activated"(ordinal)` -> the editor window's modal viewer; `on_note_image_open_external` writes a temp PNG and spawns `open`/configured viewer | doc_layout.c:452-513, 1587-1599, 1703-1720; note_view.c:2118-2125, 2596-2635 |
| Tables | TABLE block, cells are `OnText` (multi-line allowed); columns fitted to widest unwrapped line, min 40, max 320, shrunk proportionally if over width; row height = tallest cell + padding; header row shaded and bold; Tab/Shift-Tab selects the next/previous cell's whole text; arrows move between cells | doc_layout.c:722-776, 1601-1636; note_view.c:1855-1874 |
| Checkboxes | CHECK block; a drawn 14 px rounded box with a five-rect tick; click toggles `on_document_set_checked` | doc_layout.c:1011-1022, 1675-1700; note_view.c:2112-2117 |
| Action items | PARA/H1/H2 whose text starts with '!': derived blue + semibold tint in the layout (no stored style); uid on the block; `on_note_view_action_strike/due/text` are the live-editor side of the row-first rewrite; `on_note_view_action_reveal` | doc_layout.c:595-602; note_view.c:2583-2594, 2641-2669 |
| #tags | '#' at a word boundary starts a capture; non-focusable GtkPopover + GtkListBox of up to 8 prefix matches from `on_db_tag_list` (fetched once per capture); Up/Down/Tab/Enter pick, Escape cancels, a non-word char ends and styles the span ON_FMT_TAG; tag colour #c35a00 semibold | 1458-1676; doc_layout.c:618-624 |
| Links | **There is no hyperlink support.** No URL detection, no link attribute in BNBF. The only clickable "words" are the code run's "copy" and (in image_viewer.c) Previous/Next + the host action link | grep: no `uri`/`link` handling in note_view.c/doc_layout.c |
| Code blocks | consecutive CODE blocks = one shaded run (#f0f0f0), optional gutter line numbers, "copy" word top-right; monospace; inline flags and tags suppressed inside code; Down off a note-ending code block opens a body line | doc_layout.c:200-237, 1640-1656, 1722-1726; note_view.c:1815-1828 |
| Title line | block 0 if PARA and `first_line_title`: centred, H1-scale bold; derived only | doc_layout.c:784-791, 562-571 |
| Find in note | case-insensitive (g_unichar_tolower) scan of every block/cell -> `OnDocHit[]` -> background attr #ffec8b; only blocks whose hits changed re-lay out; step forward/back with wrap | 2400-2499; doc_layout.c:299-353, 661-672 |
| Undo/redo | the document's op log. View groups typing: a group stays open until 1000 ms pause or 5 sentence enders ('.' or '?'); every compound edit is a begin/end group; undo puts the caret at `on_document_last_change` | 31-34, 323-359, 1097-1121 |
| Emoji | GtkEmojiChooser popover at the caret; plus the macOS emoji letter-spacing hack (pad measured by `on_emoji_pad`, app.c:49-62) applied per emoji sequence, and a caret nudge after a line-final emoji | 1068-1091; doc_layout.c:641-659, 958-967 |
| Accessibility | GtkAccessibleText over the plain text: contents, caret, selection; attributes empty | 2671-2776 |
| Theming | hardcoded light palette (doc_layout.c:42-52, note_view.c:573, 587). No dark mode | |
| Probe API | `on_note_view_feed_key/text/click`, `on_note_view_caret`, `on_note_view_document` | 3017-3047 |

### 3.3 Exactly where the shaping / measuring / drawing seam is

Everything Pango is in `doc_layout.c`, reached only through `doc_layout.h`:

- **Build**: `text_layout_new()` (doc_layout.c:556-700) is the ONLY place a
  block's text becomes a PangoLayout: font (6 cached descriptions,
  `block_font` 391-402: plain/mono x regular/bold x 1.0/1.3/1.6), width,
  `PANGO_WRAP_WORD_CHAR`, centre alignment for the title, text (with the IME
  preedit spliced in), then attributes: action tint, run flags
  (bold/italic/underline/strike/tag), `PangoAttrShape` per inline image,
  emoji letter-spacing, find-hit background, preedit attrs. Three more tiny
  layouts: the "copy" word (995-1001), the bullet (1549-1556), numbers
  (1644-1654, 1668-1674).
- **Measure**: `pango_layout_get_pixel_size` (7 sites) for block height,
  table column/row fitting, glyph layouts.
- **Query**: `pango_layout_get_cursor_pos` (caret), `index_to_pos` (inline
  image boxes, goal x), `index_to_line_x`, `get_line_readonly`
  (`start_index`/`length`), `get_line_count`, `xy_to_index` (+ trailing),
  `get_log_attrs_readonly` (`is_cursor_position`, `is_word_start`,
  `is_word_end`), `line_get_x_ranges` + line extents via `PangoLayoutIter`
  (selection rects), `line_get_pixel_extents` (checkbox centring).
- **Draw**: `gtk_snapshot_append_layout` (1 site, `draw_layout` 1471-1479),
  `append_color` (rect fills, 5), `append_scaled_texture` (images, 2),
  rounded clip + `append_border` (checkbox), translate/save/restore.
- **Outside doc_layout.c** the only Pango in the editor stack is
  `on_emoji_pad(PangoContext*)` (app.c:49-62) and the `PangoAttrList *` the
  IM hands over (note_view.c:1699-1724). `note_view.c` otherwise talks to the
  layout only in `OnPos`, `graphene_rect_t`, `OnDocHitResult`, `OnDocMove`.

So the seam is: `doc_layout.h`'s 25 functions, whose signatures leak five
toolkit types — `PangoContext *` (new), `PangoAttrList *` (set_preedit),
`graphene_rect_t` (caret_rect, OnDocPaint.clip), `GdkTexture *`
(image_texture), `GdkRGBA` + `GtkSnapshot *` (snapshot). Roughly 40% of
doc_layout.c is toolkit-free already: invalidation/observer relays (147-278),
find-hit diffing (299-353), code-run logic (200-237), image fit (495-513),
table column fitting arithmetic, `block_at_y`, `sel_span`, and all of the
cross-block navigation in `on_doc_layout_move` (1238-1413).

One structural fact drives the port: **the document addresses text by UTF-8
BYTE offset** (`OnPos.offset`, `OnRun.len`, `OnDocHit.start/len`), and Pango
indexes layouts by UTF-8 byte too, so today the mapping is the identity
(apart from the preedit shift, `idx_of`/`off_of` doc_layout.c:515-534).
CoreText and NSString index by UTF-16 code unit. A CoreText backend needs a
per-layout byte<->UTF-16 map at exactly those two functions.

### 3.4 Recommendation: re-back doc_layout with CoreText and write a custom NSView. Do not use NSTextView.

Keep `document.c`/`bnbf.c` untouched; keep `doc_layout.c`'s structure; replace
`note_view.c` with a custom `NSView <NSTextInputClient>` in an NSScrollView.

Reasons:

1. **NSTextView would re-create the problem this codebase just solved.**
   BLOCK_MODEL.md ("The problem, stated once") documents why the GtkTextView
   engine (4.8k lines) was replaced: a text-widget's buffer cannot express
   block kinds, checkboxes, tables, images-as-blocks, action uids or the
   op-log undo, so every one was re-derived or hosted as a child widget.
   NSTextView/NSTextStorage has the same shape: one attributed string with
   paragraph attributes that include the newline. You would need a two-way
   converter OnDocument <-> NSAttributedString running on every edit (or
   abandon `OnDocument` operations as the single mutation path, which also
   abandons the 35 model tests, the fuzz, byte-identical round trips, uid
   identity through split/join, and the shared headless action rewrites the
   CLI depends on).
2. **Features that fight NSTextView**: tables (NSTextTable is TextKit 1
   only; a table drops NSTextView into TextKit 1 compatibility mode) with
   Tab cell navigation and "table is one unit from outside" selection;
   drawn checkboxes; code-run shading + gutter numbers + a clickable "copy"
   word; the derived centred title; "caret beside an object" positions;
   block images with thumbnail/full-size toggle; the custom clipboard
   flavour; undo owned by the document, not NSUndoManager.
3. **The seam is already there and it is small**: ~94 Pango calls using ~50
   distinct functions, all in one 1.7k-line file, with a direct CoreText
   equivalent for each (table below).
4. **Behaviour parity is testable**: `tests/ui/*.txt` scripts drive the view
   through `on_note_view_feed_key/text/click` and assert blocks and caret.
   A custom NSView can keep those three entry points, so the same scripts
   become the AppKit port's acceptance test. With NSTextView there is no
   such oracle.
5. What NSTextView would give for free — spell checking, system text
   services, autocorrect, Look Up, dictation polish, VoiceOver depth, dark
   mode — the GTK app does not have today, so they are not parity items.
   `NSTextInputClient` on a custom view still gets IME, the Character
   Viewer, dictation and press-and-hold accents.

Pango -> CoreText map for the new backend:

| Pango use | CoreText / CG equivalent | Note |
|---|---|---|
| PangoLayout (text + attrs + width + wrap) | CFAttributedString -> `CTTypesetter`; break lines with `CTTypesetterSuggestLineBreak` (word wrap with char fallback = WORD_CHAR), keep an array of `CTLineRef` + origin + ascent/descent/leading | own the line array; do not use CTFrame (need per-line metrics and centring) |
| get_pixel_size | sum of line heights, max typographic width | round the same way everywhere or hit-testing drifts |
| get_cursor_pos / index_to_pos | `CTLineGetOffsetForStringIndex` + line origin | secondary offset for bidi |
| xy_to_index (+trailing) | pick line by y, `CTLineGetStringIndexForPosition` | returns UTF-16 index -> map to bytes |
| log attrs: cursor positions | `CFStringGetRangeOfComposedCharactersAtIndex` (grapheme clusters) | |
| log attrs: word start/end | `CFStringTokenizer` (`kCFStringTokenizerUnitWordBoundary`) or `-[NSString enumerateSubstringsInRange:options:NSStringEnumerationByWords]` | word-step semantics will differ slightly from Pango's; the ui scripts will show where |
| line start_index/length, line count | `CTLineGetStringRange` | wrapped-line trailing-space rule at doc_layout.c:1302-1313 must be kept |
| line_get_x_ranges (selection) | two `CTLineGetOffsetForStringIndex` calls per line (LTR); per-run ranges for bidi | |
| weight / italic / mono / scale | CTFont variants via `CTFontCreateCopyWithSymbolicTraits` / `CTFontCreateWithName`; base = `[NSFont systemFontOfSize:]`, mono = `monospacedSystemFont` | "semibold" for tags/actions needs a weight trait, not just bold |
| underline | `kCTUnderlineStyleAttributeName` | drawn by CTLineDraw |
| **strikethrough** | **not drawn by CoreText** — draw a line per struck run from its typographic bounds | done items depend on it |
| **background (find hits)** | **not drawn by CoreText** — fill rects from the same x-range helper before drawing text | |
| foreground | `kCTForegroundColorAttributeName` | |
| PangoAttrShape (inline image box) | `CTRunDelegate` on the U+FFFC with ascent = h, width = w | |
| emoji letter-spacing pad + caret nudge | **delete** — it compensates for Pango/cairo's handling of Apple Color Emoji; CoreText advances are correct | `on_emoji_pad`, `emoji_caret_pad`, `ends_with_emoji` all go |
| preedit attrs (PangoAttrList) | marked-text range + underline; `setMarkedText:selectedRange:replacementRange:` gives the string and selection | change `on_doc_layout_set_preedit` to take (text, cursor) only |
| gtk_snapshot_append_layout | `CGContextSetTextPosition` + `CTLineDraw` per line in a flipped view (flip the text matrix) | |
| append_color / border / rounded clip | `CGContextFillRect`, `CGPath` rounded rect | |
| GdkTexture from PNG bytes | `CGImageSourceCreateWithData` -> `CGImageRef`, cached in `OnBlock.pixels` with `pixels_free = (GDestroyNotify)CGImageRelease` | the hook already exists (document.h:59-60, 90-94) |

View-side map (note_view.c -> NoteView.m):

| GTK | AppKit |
|---|---|
| GtkScrollable + vadjustment | NSScrollView document view with frame height = layout height (isFlipped YES); `scrollRectToVisible:` for caret; clip rect = `visibleRect` |
| key controller + hand switch | `keyDown:` -> `[self.inputContext handleEvent:]` / `interpretKeyEvents:` -> `doCommandBySelector:` (`moveLeft:`, `moveWordLeftAndModifySelection:`, `deleteBackward:`, `insertNewline:`, `insertTab:`, `moveToBeginningOfDocument:`...) mapped onto `OnDocMove`. This gives the standard macOS key bindings table instead of re-typing it |
| GtkIMContext | `NSTextInputClient`: `insertText:replacementRange:`, `setMarkedText:...`, `unmarkText`, `hasMarkedText`, `markedRange`, `selectedRange`, `firstRectForCharacterRange:`, `attributedSubstringForProposedRange:`, `characterIndexForPoint:`. Needs a linear (UTF-16) index over the document — the same mapping a11y uses today (note_view.c:2678-2693) |
| GtkGestureClick/Drag/Motion | `mouseDown:` (clickCount), `mouseDragged:` (+ autoscroll: — an improvement, GTK build has none), `mouseUp:`, tracking area / `cursorUpdate:`, `menuForEvent:` |
| GdkClipboard | NSPasteboard: write a custom type for the BNBF fragment + `NSPasteboardTypeString`; read custom, then `NSPasteboardTypePNG`/TIFF (convert to PNG via ImageIO), then string. The entire stale-cache workaround (note_view.c:1127-1209, 1418-1442) disappears |
| GtkPopover tag list | borderless non-activating child `NSPanel` (or NSPopover) with an NSTableView that never becomes key |
| GtkEmojiChooser | `[NSApp orderFrontCharacterPalette:]` (inserts through `insertText:`) |
| GSimpleActionGroup `view.*` | plain `NSMenu` built per right-click with target/action; `validateMenuItem:` |
| signals `edited`, `inline-flags-changed`, `image-activated` | three C callbacks or a small delegate protocol |
| GtkAccessibleText | `NSAccessibility` text protocol (role text area): value, selected range, insertion point line, range/bounds for range. Parity bar is low (contents/caret/selection only) |
| caret blink via gtk settings | NSTimer; `NSTextInsertionPointBlinkPeriodOn/Off` defaults |

Suggested structure for the fork (keeps the engineers apart):
`doc_layout.c` stays C and keeps everything that is geometry over blocks;
introduce a ~15-function `text_layout.h` (opaque `OnTextLayout`: new from
(OnText, style, width, preedit, hits), free, size, line_count, line_range,
cursor_rect(byte), index_at(x,y)->byte, next/prev cursor and word boundary,
x_ranges(byte a, byte b), image_rect(byte), draw(ctx, x, y)) implemented in
`text_layout_ct.m`, and a ~8-function `paint.h` (fill rect, rounded rect,
border, draw image, draw text layout). Then doc_layout's C logic is carried
nearly verbatim and CoreText is confined to one new file.

Effort signal (not a schedule): text_layout_ct.m is the hardest ~600-900
lines in the fork; NoteView.m replaces ~2,700 of note_view.c's 3,047 lines
but about half of that (editing policy: `insert_typed`, `enter_pressed`,
`delete_key`, toggles, paste_fragment, tag capture state machine, find,
image ordinals, action rewrites — note_view.c:707-1121, 1458-1676 logic,
2400-2594, 2641-2669) is toolkit-free C over `OnDocument` + `OnDocLayout`
and should be lifted into a `note_edit.c` "editor controller" that both the
view and the probe call, rather than rewritten in Objective-C.

---------------------------------------------------------------------------

## 4. UI feature inventory (parity checklist)

Sections 4.1-4.8 are from the two sub-agents' full reads of the files; I
spot-checked the items marked (✓) against the source. Line numbers are in
the named file. "Drops out" = GTK workaround that must NOT be ported.

Things that were asked about and do NOT exist in the code: printing (no
`gtk_print*` anywhere), hyperlinks in notes, spell check, text drag-and-drop
in the editor, file drops onto the editor, a read-only mode, a "database in
use" dialog (both removed with the socket IPC), `toolbar_style_*` settings
(stale keys in the live ini, zero readers), a note-rename or tag-rename/
delete UI, an Action Items context menu, dark-mode handling, a "Recent"
sidebar row.

### 4.1 library_window.c (6,197 lines) by section

| Lines | Section |
|---|---|
| 1-88 | header, includes, constants (`THUMB_SIZE 140`, `ROW_TINT "#e8f2fb"`, `NL_PREVIEW_CHARS 200`, `SB_KIND_*` enum 70-80) |
| 90-146 | `OnDragRows` boxed drag payload |
| 148-267 | `OnLibrary` struct (205-267) |
| 269-401 | thumbnail records; scroll-keep helper |
| 403-820 | sidebar population: `add_folder_rows`, `sb_reveal`, `refresh_sidebar` (619-820) |
| 822-1037 | grid thumbnails: `render_note_thumb` (840-961, cairo + pango), cache, 40 ms idle fill |
| 1039-1097 | Action Items population (`refresh_actions`) |
| 1099-1337 | notes pane population: `notes_for_selection`, `refresh_notes` (1164-1314), `refresh_all` |
| 1339-1441 | selection/activation, `on_action_toggled` |
| 1443-1552 | `dialog_new` scaffold |
| 1554-1700 | due-date dialog; row activations |
| 1702-2109 | drag and drop (validation 1802-1845, drop 2008-2109) |
| 2111-2423 | commands I: folder prompt dialog (2213-2302), confirm, new note, `on_library_quicknote` (2381), new folder |
| 2425-2701 | commands II: trash / delete / restore / empty trash / folder info |
| 2703-2760 | openers: search, media, settings |
| 2762-2913 | Open Database File flow |
| 2915-2992 | About, Quit |
| 2994-3134 | sidebar context menu |
| 3136-3400 | view mode, sidebar toggle, completed toggle, their button/menu syncs |
| 3402-3489 | export (all / selection) |
| 3491-3649 | note context menu |
| 3651-3749 | status bar runtime |
| 3751-4155 | AI summary (prompt build, subprocess, pane) |
| 4157-4311 | notify hooks installed on OnApp; `on_library_apply_native_menubar`; `on_library_get_scope` |
| 4313-4398 | sort comparators |
| 4400-4639 | column layout persistence + header menus (view-generic) |
| 4641-5045 | cell/row factories (title, text, grid card, action cells, sidebar row) |
| 5047-5201 | action tables `APP_COMMANDS` / `WIN_COMMANDS` and install |
| 5203-5284 | menubar model |
| 5286-5416 | toolbar |
| 5418-5454 | destructor |
| 5456-5582 | sidebar fit-to-content |
| 5584-5663 | sidebar build |
| 5665-5849 | notes list / grid / actions view build |
| 5851-5897 | status bar build |
| 5899-6072 | CSS (`library_install_css`) |
| 6074-6197 | constructor `on_library_window_create` |

Approximate weight: sidebar ~1,200; notes list+grid ~1,150; action items
~490; dialogs+commands ~1,100; AI ~405; shell (struct, toggles, status,
actions, menubar, toolbar, CSS, hooks, ctor) ~1,600. ~205 functions.

Public API (library_window.h): `on_library_window_create` (main.c:151),
`on_library_quicknote` (ipc.c:253), `on_library_get_scope`
(search_window.c:370), `on_library_sidebar_fit` (settings_window.c:110),
`on_library_apply_native_menubar` (settings_window.c:182). The constructor
installs `app->notify_notes_changed / notify_note_saved / notify_status /
notify_ai_changed` (6111-6114).

Calls out of library_window.c (comment-stripped counts): db.h 51 functions /
68 sites; app.h 19 functions / 58 sites (on_app_status x14, tooltip x6,
emoji markup x6, select-on-press x5, double-click x4, ...); editor_window
`open` x5, `open_action` x2, `action_set_done` x1, `action_set_due` x1, and
it reads `app->editors` directly (2462); export x2; backup x1;
serialize/document x5 (thumbnails only); search x3; media x1; settings x1;
list_rows `on_row_factory_new` x8, `on_row_touch` x4.

### 4.2 Library window

**Layout** (6080-6197; title and size ✓ at 6096-6098): GtkApplicationWindow "Notes - Library", fixed
default 900x620 (not persisted). Vertical: [in-window menubar, macOS only,
hidden when native] / toolbar / separator / horizontal GtkPaned
(sidebar | vertical GtkPaned (GtkStack "list"|"grid"|"actions" over AI pane))
/ separator / status bar. AppKit: NSSplitView nested, view swap for the
stack, NSToolbar or custom strip. NOT persisted today: window size, sidebar
visibility, list/grid mode, sort column, column widths, sidebar expansion
across launches — do not "add" autosave silently if strict parity matters.

**Sidebar** (GtkListView over GtkTreeListModel of `OnSbRow`). Rows in build
order (refresh_sidebar 619-820):

| Kind | Shown when | Text | Children |
|---|---|---|---|
| PINNED | pinned count > 0 | "📌 Pinned Notes" [+ (n)] | leaf |
| ALL | always | "🔮 All Notes" [+ (n)] | leaf |
| ACTIONS | any action item exists | "❗ Action Items" [+ (n OPEN)] | leaf |
| ROOT | always | "📓 Notes" [+ (n in root)] | folder tree |
| FOLDER | per folder | "[emoji  ]name[ (n)]" | subfolders; leaf if none |
| TAGS_HEADER | any tag | "🏷️ Tags" (unselectable: selection reverted, 1372-1378) | tags |
| TAG | per tag | "#name[ (n)]" | leaf |
| TRASH | trash count > 0 | "🗑 Trash" [+ (n)] | trashed folders |
| TRASH_FOLDER | per directly-trashed folder | like a folder | leaf |

Section rows bold. Counts only when `sidebar_counts` (two GROUP BY maps +
4 scalar queries; folder tree from one `on_db_folder_child_map`). Expansion
kept across rebuilds in memory keyed kind+id (nothing expanded on first
population — CLAUDE.md's quirk-14 text is stale). Selection restored by
`sb_reveal` (expands ancestors); falls back to row 0. Fit-to-content width
(5456-5582): natural width of realized rows, floor 160 px, cap 50% of the
paned, deferred to frame-clock after-paint; triggers: expand/collapse,
rebuild, map, Settings. AppKit: NSOutlineView; `shouldSelectItem:` for the
Tags header; expansion via `isItemExpanded:` before reload. **Hard part:
fit-to-content** — measure visible rows' string widths + indentation and
`setPosition:ofDividerAtIndex:` on expand/collapse notifications.

**Notes list** (GtkColumnView; one GListStore -> GtkSortListModel -> ONE
GtkMultiSelection shared with the grid). Columns: Title (expands), Path,
Modified, Created; all resizable, sortable (asc/desc only), reorderable.
Default sort Modified newest-first (5742). Layout persisted to ini
`list_columns` as `key:vis,...` in display order (4400-4639), default
`path:0,title:1,modified:1,created:0`; header right-click menu with one
check item per column (stateful `win.column-<cfg>-<key>`; last visible
column cannot be hidden). Density: Comfortable = optional bold title +
dimmed first-body-line preview (`on_db_note_text_map(db,200)` in one query);
Compact = 20 px rows. Alternating row tint #e8f2fb. Enter / double-click
opens the editor. Selection and scroll preserved across refreshes. Light
save path `library_notify_note_saved` (4192-4257) updates one row.
AppKit: view-based NSTableView + sortDescriptors + `headerView.menu`;
write the same `key:vis` string rather than `autosaveName`.

**Grid** (GtkGridView over the same selection): cards = 140x140 thumbnail +
bold wrapped title. Thumbnail (`render_note_thumb` 840-961): white card,
1 px grey border, first image (decoded capped at 512 px) fitted into
128x72, then body text "Sans 8" #444 wrapped/clipped; rendered with cairo +
pangocairo at scale factor into a GdkTexture. Cache: note id -> {updated_at,
texture}, memory only; filled asynchronously in 40 ms idle slices, only
while the grid is visible. AppKit: NSCollectionView; **the shared selection
is the piece GTK gave for free** — own one selected-id set and mirror it;
thumbnails via CGBitmapContext + CoreText off the main thread.

**Action Items view** (third stack child; own GtkColumnView + single
selection). Columns: done (checkbox, header blank, menu label "Done"),
Context (note's folder name), Action (text, struck when done), Due Date
("%b %e, %Y", coloured overdue #c01c28 / today #d19a00 / ahead #26a269).
Sorters per column; ini `action_columns`, code default
`done:1,context:1,action:1,due:1` (notes.ini.defaults still seeds only
three columns). Populate `on_db_action_list`, skipping done items when
`show_done_actions` is off. Checkbox -> `on_editor_action_set_done(app,
uid, done)` (1414-1441). Double-click on Due -> "Notes - Due Date" dialog
with GtkCalendar + Clear / Cancel / Set -> `on_editor_action_set_due`
(1564-1645). Enter / double-click on Context or Action ->
`on_editor_window_open_action(app, note_id, ord)`. Hide/Show Completed via
toolbar `win.toggle-done`, View menu `app.done-hide`/`app.done-show`, and
the Settings checkbox, all through `done_set_shown` (3355). No context
menu. AppKit: second NSTableView; NSDatePicker (graphical) sheet;
`doubleAction` + `clickedColumn`.

**Dialogs** (all in library_window.c unless noted): New Folder / Folder Info
(same prompt 2213-2302: emoji entry using GtkEntry's built-in chooser, name
entry, AI-mode radios Normal/Project/Custom; Cancel/OK); Due Date; yes/no
confirm (GtkAlertDialog "_No"/"_Yes", detail "This cannot be undone.") for
permanent note delete, permanent folder delete, Empty Trash; Open Database
step 1 (file chooser "*.db") and step 2 (alert: Cancel / Session Only / Set
as Default); Database Error notice; export folder chooser; export result
notice; About (GtkAboutDialog: version, logo, authors, "Compiled <date>",
database path, "N notes in N folders, N tags — size", BSD licence). Outside
this file: Welcome (main.c:255-271), integrity-check failure notice
(app.c:112-141), Insert Image chooser (editor_window.c:469-477), Settings'
folder and viewer choosers. Trashing shows no dialog, only a status line.
AppKit: NSAlert sheets, NSOpenPanel, a small custom sheet for the folder
prompt, `orderFrontStandardAboutPanelWithOptions:`.

**Menubar** (`build_menubar` 5211-5284), all `app.` actions:
File: New Note, New Folder…, Export All as HTML…, Export All as Markdown…,
—, Open Database File…, Settings…, About, Quit.
View: Notes as List, Notes as Grid, Hide Sidebar | Show Sidebar (two items,
one hidden), Hide Completed | Show Completed (same device), —, Media…,
Search Notes….
There is **no Edit menu** (Undo/Cut/Copy/Paste are key handlers and context
menus only). An AppKit app needs a real Edit menu for the responder chain,
the Character Viewer and standard shortcuts — add it deliberately.

**Context menus**: folder (New Subfolder…, Info…, Move to Trash, Sort
Subfolders Alphabetically, —, Search Here…); Notes root (New Folder…, Sort
Subfolders, —, Search Here…); trashed folder (Restore Folder, Delete
Permanently, —, Search Here…); Trash (Empty Trash…); tag / Pinned / All
Notes / Action Items (Search Here…); note in normal views (Open,
Pin|Unpin, —, Export as HTML…, Export as Markdown…, —, Move to Trash); note
in Trash views (Open, Restore, —, Delete Permanently); column headers (one
check item per column). Editor: image / table / Cut-Copy-Paste (3.2).

**Actions** (5072-5200): `app.` = new-note, new-folder, export-html,
export-md, open-db, preferences, about, quit, view-list, view-grid,
toggle-sidebar, sidebar-hide, sidebar-show, done-hide, done-show, media,
search. `win.` (library) = new-note, find, media, delete-folder, quicknote,
delete-note, toggle-view, toggle-done, folder-info, folder-restore,
sort-subfolders, search-here, empty-trash, note-export-html, note-export-md,
note-restore; parameterised `note-open(x)`, `note-pin(b)`; eight stateful
`column-<cfg>-<key>`. Not actions: the AI button (`clicked`) and the toolbar
search entry (`activate`).

**Keyboard shortcuts — complete table** (app.c:529-548 ✓): Cmd+Q quit;
Cmd+, preferences; Cmd+N new note (library and editor); Cmd+F search window
(library) / find box (editor); Cmd+M media (library) / code block (editor);
Cmd+Z undo; Cmd+Shift+Z and Cmd+Y redo; Cmd+B/I/U; Cmd+D insert date;
Cmd+E insert emoji. Plus the view's own keys (3.2). No Delete-key binding
for notes/folders; no shortcut for strike, headings, lists, image, table.
AppKit conflicts to decide: Cmd+M is Minimize, Cmd+E is "Use Selection for
Find".

**Drag and drop** (1702-2109): payload one boxed `OnDragRows {kind
NOTES|FOLDER|TRASHED_FOLDER, ids}`, in-process only. Sources: every note
cell and grid card (whole selection if the pressed note is in it), every
sidebar folder / trashed-folder row. Targets: sidebar rows only. Rules:
notes -> Folder / Notes root / Trash (always INTO); folder -> Folder
(INTO / BEFORE / AFTER by row **quarters**, 1853-1857 ✓ — CLAUDE.md says
thirds), Notes root (INTO), Trash (INTO, live folders only); never onto
itself or into its own subtree. Drop: `on_db_notes_move` (clears trashed =
restore), `trash_notes_core`, `trash_folder`, `on_db_folder_move`,
`folder_move_beside` (= move + `on_db_folder_reorder`). Indicator = CSS
class on the row; drag icons folder/file/documents PNG at 32 px. Refresh
deferred to an idle (drops out in AppKit). AppKit: private pasteboard type;
`outlineView:validateDrop:proposedItem:proposedChildIndex:` holds the same
rules.

**Toolbar** (5321-5416), icons-only, in order: New Folder (`new-folder`),
Delete Folder (`delete-folder`) | Quicknote (`archive`), New Note
(`newnote`), Delete Note (`deletenote`) | Sidebar (`sidebar`), List/Grid
(`grid`<->`list`, pictures the view a click switches TO), Hide/Show
Completed (`hidden`<->`visible`), Search (`search`), Media (`images`),
Settings (`settings`) | AI Summary (`microchip`, visible only when
`ai_enabled`) | spacer | search entry "Search all notes" (Enter opens the
search window with the query). Buttons are never disabled; commands no-op
when inapplicable.

**Status bar** (3651-3749, 5857-5897): left = selection path (through
`on_app_location_text`), right = last event message in a 600 ms crossfade
GtkRevealer, hidden after 4 s; "N file(s) selected" on selection change.
This crossfade, the AI throbber (braille frames at 120 ms) and the search /
media GtkSpinners are the **only animations** in the app.

**AI summary** (3751-4155): toolbar button shows the bottom pane and runs
`ai_command` (split with `g_shell_parse_argv`, no shell; spawned with
GSubprocess, prompt on stdin, stdout+stderr read async, cancellable ✓
3972-3992) over the **selected notes'** cached body text + their action
items; prompt header chosen by the selected folder's `ai_mode`
(Normal/Project/Custom, `ai_custom_prompt`). Pane: read-only GtkTextView +
Copy + close. AppKit: NSTask + NSPipe; needs a shell-word splitter. Known
bug (sub-agent): the "Invalid AI command" early return never stops the
throbber (3975-3982).

**Flows**: trash note(s) (no confirm) / permanent delete in Trash views
(confirm) / restore; trash folder (closes editors of
`on_db_folder_note_ids`) / permanent delete / restore; Empty Trash; pin /
unpin; new note (current folder else root); quicknote (root, editor to
front; works without a library window); sort subfolders; export all /
selection (synchronous on the main thread); Open Database File
(`open_db_switch` 2812-2849: close all editors, close db, open new, revert
on failure, Set as Default writes `db_dir`, re-arm backup, health check);
Quit destroys every window (editors flush).

**CSS** (one provider, 5957-6072): alert-dialog button look; grid card
padding/hover; no focus ring / no pressed shadow on rows and cards;
alternating rows `#e8f2fb`; preview opacity 0.65; row/compact cell padding;
due colours; sidebar background `shade(@theme_bg_color, 0.96)`, text
rgb(65,65,65), selection rgb(86,131,224)/white; drop-into/before/after
box-shadows; emoji entry; AI header buttons; 6 px paned handle; header
button borders. In AppKit these become colours/metrics constants; the
theme-fix rules drop out.

**Drops out (library)**: `on_app_set_tooltip`, `on_app_double_click_watch`,
`on_app_select_on_press`, per-cell controllers, `"on-item"` stashing,
`"on-binding"` checkbox guard, `scroll_keep_queue`, deferred
`refresh_all_idle`, after-paint fit queue, `on_row_touch` /
`on_row_factory_new` protocol, two-item hidden-when menu device,
`dialog_new` scaffold, `LwRef`, emoji padding/markup, sidebar top spacer,
native-menubar switch + in-window bar, GObject boxed drag type.

### 4.3 list_rows.[ch]

Three GObject row classes with public fields: `OnSbRow {kind, id, name,
raw, children}`, `OnNoteRow {id, title, modified, updated_at, path, created,
created_at, preview, thumb}`, `OnActionRow {uid, note_id, ord, done, text,
due, due_raw, context}`; `on_row_touch(store,row)` = in-place update
protocol; `on_row_factory_new` = rebind-on-"changed" factory. AppKit: plain
C structs as data-source items (a thin NSObject box for NSOutlineView
items); identity = note id / uid / (kind,id). The whole file is replaced.

### 4.4 Editor window (editor_window.c)

- GtkApplicationWindow, title "Notes - <title>" (rewritten after each
  save). Size from `editor_win_w/h` (default 640x509), never written back.
  macOS placement bottom-right of the library's screen, 12 px margin, via
  `objc_msgSend` on the NSWindow (1551-1603) — becomes five native lines.
  No cascade.
- Tree: toolbar / separator / GtkOverlay (scrolled OnNoteView + image-viewer
  panel) / separator / status bar.
- Toolbar (1105-1224): B I U S toggles (mirrored from the view's
  `inline-flags-changed`), Code `{ }`, | Styles "Aa" menu (Heading 1,
  Heading 2, Body) or three buttons, | Lists "≡" menu (Bulleted, Numbered,
  Task) or three buttons, | Insert "+" menu (Image…, Table, Emoji…, Date),
  spacer, find group (search entry "Find in note" + previous/next buttons).
  Every editor button renders a TEXT GLYPH today: the icon names it asks for
  (`code-block`, `heading-1`, ...) have no files in icons/ ✓.
  `compact_editor_toolbar` chooses menus vs buttons, rebuilt live.
- `win.` actions (1374-1385): new-note, find, undo, redo, inline(s),
  para(s) {h1,h2,body,bullet,number,check,code}, insert-image, insert-table,
  insert-emoji, insert-date. Editing actions are disabled while the find
  entry has focus ("the gate", 1394-1441) — drops out (responder chain).
- Find bar: search-changed -> `on_note_view_find`; Enter / down = next, up =
  previous, Escape returns focus to the view; Cmd+F focuses it.
- Status bar: folder path (`on_app_location_text`), optional `id:N`, and a
  save dot (red "Unsaved changes" / green "Saved").
- Autosave: 1200 ms debounce on `edited` (editor_window.c:40, 836-848 ✓);
  `editor_save` (776-819 ✓) = `on_note_document_save` with the view's
  actions-modified flag, tags rewritten only when the tags flag is set,
  title update, then full notify if tags/actions changed else light notify.
  On close (863-910): a BLANK note is hard-deleted ("Deleted empty note"),
  otherwise save if dirty. AppKit must flush in `windowWillClose:` and in
  `applicationShouldTerminate:`.
- Load: `on_note_document_load` -> `on_note_document_reconcile` ->
  `on_note_view_load_document`; a healed document queues an autosave.
- Open variants: plain, with search term (seeds find), at image ordinal, at
  action ordinal; an already-open note is presented and re-revealed.
  Open editors tracked in `app->editors` (note id -> window).
- Image viewer hosting: `editor_viewer_ops` (377-382) address images by
  ordinal; window-level capture-phase key handler gives keys to the viewer
  first and swallows the rest while it is open.
- Insert image: file chooser -> `gdk_pixbuf_new_from_file` ->
  `on_note_view_insert_image(GdkPixbuf*)`; failure is only a g_warning.
- Toolkit-free and liftable: `action_apply` (502-557), the `editor_save`
  sequence and notify rule, the load sequence, the blank-means-delete
  policy, the tables (`INLINE_TOGGLES`, `PARA_STYLES`, menu contents), the
  placement arithmetic.

### 4.5 Settings window (settings_window.c)

"Notes - Settings", a new window per open, every control writes through
immediately, no OK/Apply, scrolls past 600 px.
- Appearance: List density (Compact/Comfortable) `list_density_comfortable`;
  Show note counts `sidebar_counts`; Fit the sidebar width
  `sidebar_fit_content`; Bold titles `bold_list_titles`; Show completed
  action items `show_done_actions`; Show database path prefix
  `statusbar_db_path`; (macOS) Use the native macOS menu bar
  `native_menubar`; librsvg hint.
- Editor: copy button `code_copy_button`; line numbers `code_line_numbers`;
  First line is Title `first_line_title`; Compact toolbar
  `compact_editor_toolbar`; Show note id `statusbar_note_id`; Disable touch
  assistance `touch_assist` (inverted); Image viewer entry + Browse…
  `image_viewer`.
- Database: health plate (LED + verdict + time; Current database; Data
  "N notes in M folders"; Size on disk; SHA-256 button showing 8+4 hex,
  click copies) with one **Update** button (synchronous
  `on_db_health_check` + re-hash); backups: enable `backup_enabled`,
  interval spin (0-10080 step 15) `backup_interval_min`, keep spin (1-500)
  `backup_keep`, "Backing up to <dir>" with a shares-fate warning, Change
  Folder… `backup_dir`, Back Up Now. No control for the database location
  (deliberate).
- AI Features: Enable `ai_enabled`; AI command entry `ai_command`; Custom
  prompt text view `ai_custom_prompt`.
- No restart-required settings. Table-driven booleans (`BOOL_SETTINGS`
  102-143: label, key, `offsetof` into OnApp, apply hook) — port the table.
- AppKit: drop `native_menubar`, `touch_assist`, the librsvg hint, the
  small-button/spin CSS, the scrollbar-width fix. Consider moving Update's
  check + hash off the main thread.

Config keys, defaults and where read/written (sub-agent table, spot-checked
against main.c:423-448): db_dir (main.c:208, library_window.c:2835 write);
code_copy_button 1; code_line_numbers 0; sidebar_counts 0;
sidebar_fit_content 1; first_line_title 1; compact_editor_toolbar 1;
list_density_comfortable 0; statusbar_db_path 0; statusbar_note_id 0;
show_done_actions 1; bold_list_titles 1; ai_enabled 0; ai_command;
ai_custom_prompt; native_menubar 0; touch_assist 0; image_viewer
(note_view.c:2618); backup_enabled 0, backup_dir, backup_interval_min 60,
backup_keep 10, backup_source_stamp (backup.c); list_columns,
action_columns (library_window.c:4458/4601); search_win_w/h 575x360 (written
on close); media_win_w/h 780x560 (written on close); editor_win_w/h 640x509
(read only). notes.ini.defaults seeds different values for
code_line_numbers, native_menubar, list_density_comfortable, search_win_*.

### 4.6 Search window (search_window.c) + search_query.c

"Notes - Search": entry + Search button + spinner; scope radios All Notes /
Selected Folder/Tag (resolved at each search via `on_library_get_scope`);
Case sensitive; Regular expression; results GtkColumnView (Path, Modified;
no sorting); status label. No search-as-you-type. `run_search` parses on
the main thread (bad regex reported immediately), then a `g_thread` worker
with its OWN db connection matches (`on_db_note_list_all(db, TRUE)` for All
scope — Trash included; bulk `on_db_note_text_map` above 64 candidates),
checks an atomic `cancelled` per note, and delivers by `g_idle_add`.
Activation (Enter / double-click) -> `on_editor_window_open_search(app, id,
highlight term)`. Size persisted to `search_win_w/h` on close. The worker
(234-304) is pure db + `on_query_matches` and can be lifted to the core
behind a "post to main thread" seam; `OnSearchScope` should move to a core
header.

### 4.7 Media window (media_window.c)

"Notes - Media in <scope>": GtkFlowBox of 144 px square thumbnails captioned
with the note title; note set snapshotted at open from the library's
`notes_for_selection()`; scan = repeating idle doing 40 ms slices, one image
per step, blob held across yields (`on_note_count_images`,
`on_note_image_nth` at thumb size); cap 500 images with the truncation said
in the status line; single click opens the shared modal viewer; viewer
action link "Show in source note" -> `on_editor_window_open_image`. Size
persisted `media_win_w/h`. AppKit: NSCollectionView (non-selectable items)
+ background decode via ImageIO thumbnails.

### 4.8 Image viewer (image_viewer.c)

Overlay panel (88% #101010) covering the text area / media grid: picture
scaled DOWN only to (overlay - 2x20 px inset - 30 caption - 24 nav),
caption "<host caption>   [← → to move, ]ESC to close", optional action
link, "Previous | Next" (hidden when one image, dimmed at the ends, no
wrap, hit-tested by midline). Click anywhere / Escape closes; Left/Right
step; all other keys swallowed by the host while open; wheel swallowed;
re-render 150 ms after a resize settles. Hosts decoupled by
`OnImageViewerOps {count, render -> GdkPaintable*, caption, action}`
(image_viewer.h:70-76). The panel never takes focus. AppKit: an NSView over
the scroll view that refuses first responder; `render` must return a
toolkit-neutral image (CGImageRef / PNG bytes).

### 4.9 app.c helpers and theming summary

- CSS lives ONLY in C string literals: app.c:1026-1028 (status label 85%,
  dot label 70%), app.c:1047-1073 (touch aids), library_window.c:5957-6072,
  settings_window.c:892-913, media_window.c:195-203, image_viewer.c:162-178.
  No .css files, no theme switching; the editor's colours are constants in
  doc_layout.c:42-52.
- Icons: PNGs by basename from `<exe dir>/icons` through the GTK icon theme
  at 24 px (toolbars) / 32 px (drag) / 128 px (About); app icon
  `icons/composition.png` -> .icns. AppKit: copy to Contents/Resources,
  `NSImage` with explicit point size (sources are 512 px).
- Tags: created by typing `#word` in a note (ON_FMT_TAG run) -> `note_tags`
  rewritten on save when the view's tags flag is set; orphan tags pruned;
  sidebar Tags section; CLI `note tag/untag`, `tag list/notes/delete`. No
  GUI to rename or delete a tag.
- Export formats: HTML (images as base64 data URIs) and Markdown (images as
  sibling PNG files, or `![image N]()` placeholders for `note cat --md`);
  all-notes export mirrors the folder tree; single/selection export flat.
- Drops out (app.c/main.c/others): tooltip machinery, double-click watch,
  select-on-press, menu-popup scaffold, accel `<Primary>` rewriting, touch
  assist, pixbuf->texture helper, icon-theme setup, emoji padding,
  `quartz_log_filter`, `GSK_RENDERER`, `gtk-hint-font-metrics`,
  `register-session`, hold/release around Welcome, the editing gate, the
  objc_msgSend placement and pasteboard code, the image viewer's tick +
  snapshot-paintable wrapper, `notify::default-width` size tracking.

### 4.10 Stale documentation to distrust (both sub-agents + mine)

CLAUDE.md line 3 ("the `gtk4` branch; `main` is the GTK3 original");
Internals.md (says BNBF "currently 5", "GtkTextBuffer conversion", schema
without trashed/emoji/tasks tables); cli.h synopsis omits `note add-image` /
`note set-modified`; ipc.h says `notes.sock` (it is `notes-%08x.sock`);
CLAUDE.md "thirds" for drop bands (quarters); CLAUDE.md
`GTK_OVERLAY_SCROLLING=0 in main` (no such code); CLAUDE.md
`statusbar_db_path` "default 1" and `list_density_comfortable` "default 1"
(code defaults 0); app.h says `compact_editor_toolbar` default off (code:
on); BLOCK_MODEL.md promises drag auto-scroll and a `move-cursor` shortcut
controller (neither exists; keys are a hand switch); User_Guide.md's CLI
section omits `--json`, `--uid`, `action show/text`, and says the native
menubar needs gtk-mac-integration; README says GTK "4.12 or newer", BUILD.md
"4.10+".

---------------------------------------------------------------------------

## 5. Application plumbing

### 5.1 main() sequence (main.c:375-516)

1. macOS: install `quartz_log_filter` on Gdk/Gtk/GLib domains (378-392).
   GTK-only; delete in the fork.
2. `on_app_config_init(argv[0])` — resolve and load `notes.ini` (396).
3. `on_cli_run(argc, argv)`; if it returns >= 0 that is the exit code and no
   GTK is ever initialised (400-402). Returns -1 for: no subcommand, an
   unknown first word (falls through to GTK option parsing), or
   `quicknote` / `note open PATH` when no instance answered (a pending action
   is recorded for the GUI about to start).
4. Build a stack `OnApp`: `editors` = GHashTable(gint64* -> window),
   `db_dir` from ini, `icons_dir` = `<exe dir>/icons`, then 12 boolean
   prefs + 2 strings read from the ini (407-448). NOTE the defaults passed
   here differ from notes.ini.defaults for three keys:
   `code_line_numbers` default FALSE (defaults file: 1),
   `list_density_comfortable` FALSE (defaults: 1), `statusbar_db_path` FALSE
   (CLAUDE.md says "default 1"). The ini seeded from the defaults file wins
   on a fresh install; the in-code default applies only when a key is absent.
5. If the expected database file exists, open it now (`startup_open_db`,
   125-139): **no fallback** to another location on failure — print an error
   and exit 1 (456-464).
6. macOS: `g_setenv("GSK_RENDERER","cairo",FALSE)` (477). GTK-only.
7. `gtk_application_new("org.example.notes", G_APPLICATION_DEFAULT_FLAGS)`,
   `register-session = TRUE` (so Dock Quit / logout route through `app.quit`
   and editors flush), connect `startup` + `activate`,
   `g_unix_signal_add(SIGTERM, on_sigterm)` (479-489).
8. `g_application_run` — with a clean argv if a pending IPC action exists
   (495-501).
9. `startup` (281-319): install accels, `gtk-hint-font-metrics = FALSE`,
   icon-theme search paths, default window icon, app CSS, touch-assist CSS.
10. `activate` (329-350): if the library exists, present it. Else if no db is
    open -> hold the app and run the **Welcome** chain (`startup_first_run`,
    255-271): GtkAlertDialog "Notes - Welcome" / "No notes database was found
    at <path>" with buttons "Open a notes.db File" (file chooser filtered to
    the fixed name `notes.db`; persists `db_dir` = its directory) or "Create
    a New notes.db"; dismissal quits. Else `startup_finish`.
11. `startup_finish` (148-168): `on_library_window_create`, status "DB at …
    loaded", `on_app_db_health_start` (async integrity check, every launch),
    `on_backup_auto_start`, `on_ipc_server_start`, `on_ipc_run_pending`.
12. After the loop: `on_ipc_server_stop`, destroy editors table,
    `on_db_close`, return status (503-515).

SIGTERM (357-367): destroy every window so each editor's destroy handler
flushes its final save; the loop then ends.

**Single instance**: there is no enforced GUI single-instance on macOS.
`G_APPLICATION_DEFAULT_FLAGS` would give uniqueness over D-Bus on Linux, but
ipc.h:369-371 states D-Bus is unavailable on the macOS target, so the socket
is the only instance channel. A second GUI launch on macOS starts a second
process; its `on_ipc_server_start` finds the socket alive and silently
becomes a non-server (ipc.c:515-530). Two GUIs on one database are therefore
possible; only CLI<->GUI is arbitrated. (Not tested by launching; inferred
from the code.) An AppKit bundle gets single-instance for free from
LaunchServices when opened via Finder/`open`.

### 5.2 IPC (ipc.c)

- Transport: AF_UNIX stream socket at
  `g_get_user_runtime_dir()/notes-%08x.sock` where the hex is
  `g_str_hash(<db path>)` and the db path is `<db_dir>/notes.db` from the ini
  next to the caller's own binary, else `on_db_default_path()` (ipc.c:45-58).
  On this Mac `XDG_RUNTIME_DIR` is unset, so GLib falls back to the cache
  dir: the live sockets are `~/.cache/notes-0a5a7d69.sock` (current GUI) and
  a stale `~/.cache/notes-ae94c3a6.sock` (observed with `ls`). One socket
  PER DATABASE, so the dev sandbox and the real instance do not cross.
- Server: `GSocketService` "incoming" handler on the main loop, blocking
  reads with a 10 s socket timeout (ipc.c:472-503). Stale socket file is
  probed with a connect and unlinked if dead (513-532).
- Protocol A (one line each way): request `quicknote` or `open <path>`;
  reply `OK <message>\n` / `ERR <message>\n` (ipc.c:1-13, 280-288). `<path>`
  is a numeric note id or `Folder/Sub/Title` (exact title match, 168-238).
- Protocol B, `RUN` (delegated data command, 406-464, 643-706):
  request = `RUN\n`, `<nargs>\n`, then per arg `<len>\n<bytes>`, then stdin
  as `<len>\n<bytes>`; response = `<exit code>\n`, `<len>\n<stdout bytes>`,
  `<len>\n<stderr bytes>`. Limits: 256 args, 64 KiB line, 64 MiB blob.
  The server runs `on_cli_dispatch(app, ...)` with fds 1/2 `dup2`'d onto
  `tmpfile()`s to capture output (361-399), then calls
  `app->notify_notes_changed` if `on_cli_command_mutates` (447-449).
  The client's response wait is unbounded (682).
- Pending action: when no server answers `quicknote`/`note open`, the request
  is stored in statics and run after the GUI is up (294-344).

Port note: replace GIO with `socket/bind/listen/accept` + a
`dispatch_source` (or CFSocket) on the main queue; ~40 GIO call sites. To
stay wire- and path-compatible with the GTK build the fork must reproduce
(a) the directory (`$XDG_RUNTIME_DIR` else `$XDG_CACHE_HOME` else
`~/.cache`), and (b) `g_str_hash` (djb2 over SIGNED chars: `h = 5381; h =
h*33 + c`) of the exact db path string. If the two builds disagree on the
socket name, a CLI from one build will not find the other's GUI and will
open the database headless while that GUI is running — the single-writer
design silently breaks. Decide up front: same socket name (interoperable) or
a deliberately different app identity (then also a different ini and
database).

### 5.3 CLI (cli.c, 2,621 lines)

- Entry `on_cli_run` (2567-2621): `help`; GUI commands (`quicknote`,
  `note open`) via `on_ipc_try_remote` or pending; everything else that is a
  known noun (`tag folder note action trash`) or flat command (`search stats
  backup export-md export-html`) is first offered to a running instance via
  `on_ipc_try_remote_run`; only if none is listening does it open the
  database itself (`cli_open_db`, 152-168: `db_dir` from the ini, else
  default path) and run `on_cli_dispatch` with a zeroed stack `OnApp` holding
  only `db`.
- So: **GUI running => the GUI process executes the command** (one writer,
  live refresh, open editors updated); **no GUI => direct SQLite**.
- Output: tab-separated lines, or `--json` on record-printing commands
  (`cli_json_capable`, 2460-2478); exit 0 ok / 1 usage / 2 failure.
- stdin (`-`) for `note new/append/set` and `action text`
  (`on_cli_command_reads_stdin`, 2380-2395) is slurped by the client and
  shipped in the RUN frame.
- Action verbs take the real `OnApp` so `on_editor_action_set_*` can reach an
  open editor (cli.c:1759-1856). This is the one place the CLI calls GTK-file
  code (section 2.2).
- Hidden verbs not in cli.h's header comment: `note add-image ID FILE`
  (GdkPixbuf file load -> PNG), `note set-modified ID TS` (cli.c:2343-2346);
  both used by tools/import-apple-notes.sh.
- Full command list: cli.h:446-505 and `usage()` cli.c:2061-2180.

### 5.4 The Claude skill and what must be preserved

`skills/notes/SKILL.md` (symlinked at `~/.claude/skills/notes`) hard-codes
two binaries: `/Users/ian_campbell/salt_development/notes/notes` (real
database, via the repo-root `notes.ini`, which names the iCloud Drive db_dir)
and `.../notes/dev/notes` (sandbox). It relies on: every listed command and
flag; `--json` record shapes (action record `{uid, note_id, ord, done, due,
due_at, context, text}`); `action list --open|--done|--context NAME|--uid`;
`action show/done/undone/due/text UID|NOTEID:ORD`; `note list PATH|--all`;
`note info/cat [--md]/new --folder/append/move/tag/untag/pin/delete`;
`folder list/add/rename/move`; `search` operators; `tag list/notes`;
`stats`; `backup`; exit codes (1 = unknown flag/usage, 2 = failure/empty
query); and "a running GUI is fine" (the socket hand-off).

Consequences for the fork:
- `cli.c` should be carried as-is (it is the contract). Its behaviour must
  be byte-identical in both plain and JSON forms; a golden-output test
  against the GTK build on the dev sandbox is cheap and worth having.
- The repo-root binary the skill calls and the installed GUI
  (`~/Applications/Notes.app/Contents/MacOS/Notes`) are different files with
  different inis that happen to name the same `db_dir`; they meet only
  through the socket hash. If the AppKit fork lives in a new directory, the
  skill needs a new `NOTES=` path or the fork must be reachable by the old
  CLI over the same socket.
- The tasks app's (removed) Notes plugin also drove this CLI (section 9).

### 5.5 Config (notes.ini)

- Code: app.c:862-1016. One group `[notes]`, GKeyFile, loaded ONCE at
  `on_app_config_init`, written through on every changed `set` (skips the
  rewrite when the value is unchanged, 966-972). Never re-read while running.
- Path resolution (875-923): `<dir of argv[0]>/notes.ini` (argv[0] must
  contain a '/', else the cwd is used, app.c:589-599). If that file does not
  exist AND the directory is not writable -> `~/.config/notes/notes.ini`
  (`g_get_user_config_dir`). If the chosen ini does not exist it is seeded by
  copying `<exe dir>/notes.ini.defaults`.
- In the .app the ini lives INSIDE the bundle
  (`Notes.app/Contents/MacOS/notes.ini`) because `~/Applications` is
  writable; `make install` carries it across (Makefile:262-279). A signed /
  notarised AppKit bundle should not write into itself (it breaks the
  signature) — the fork needs a decision: `~/Library/Application Support/
  Notes/notes.ini` (or keep `~/.config/notes`), plus a one-time adopt of the
  bundle-adjacent ini so `db_dir` is not lost ("my notes vanished" is the
  documented failure mode, CLAUDE.md "Custom DB location").
- GKeyFile format details a hand-rolled reader must match if the same file
  is shared with the GTK build: `key=value` lines, `#` comments (dropped on
  rewrite), values are escaped (`\s` leading space, `\n`, `\t`, `\\`), no
  quoting. Real values contain spaces, `~`, `|` and `:` (e.g. `db_dir=
  /Users/.../Mobile Documents/com~apple~CloudDocs/Notes`,
  `backup_source_stamp=<dir>|<path>|<size>:<mtime>`).
- Keys observed (code + live ini): `db_dir`, `code_copy_button`,
  `code_line_numbers`, `native_menubar`, `sidebar_counts`,
  `sidebar_fit_content`, `first_line_title`, `compact_editor_toolbar`,
  `touch_assist`, `image_viewer`, `search_win_w/h`, `media_win_w/h`,
  `editor_win_w/h`, `list_columns`, `action_columns`, `statusbar_db_path`,
  `statusbar_note_id`, `show_done_actions`, `list_density_comfortable`,
  `bold_list_titles`, `ai_enabled`, `ai_command`, `ai_custom_prompt`,
  `backup_enabled`, `backup_dir`, `backup_interval_min`, `backup_keep`,
  `backup_source_stamp`; stale keys present in the live ini and ignored:
  `toolbar_style_library`, `toolbar_style_editor`, `list_autofit`. (Per-key
  file:line table in section 4.)
- GTK-only keys with no AppKit meaning: `touch_assist`, `native_menubar`.

### 5.6 Data paths, schema, versioning

- Default db: `g_get_user_data_dir()/notes/notes.db` = `~/.local/share/notes/
  notes.db` on this Mac (db.c:393-402; directory created 0700). Filename is
  fixed (`ON_DB_FILENAME`, db.h:194); only the directory is configurable.
  The user's real database is in iCloud Drive
  (`~/Library/Mobile Documents/com~apple~CloudDocs/Notes/notes.db`, per the
  repo-root notes.ini).
- Open (db.c:404-504): `sqlite3_open`, busy timeout 5000 ms, `SCHEMA_SQL`
  (`PRAGMA foreign_keys = ON` + CREATE IF NOT EXISTS for folders, notes,
  tags, note_tags, list_groups, lists, action_uid_seq, 3 indexes), then
  `TASKS_SCHEMA_FMT` (tasks, idx_tasks_list, note_task, idx_note_task_note),
  then six blind `ALTER TABLE ADD COLUMN` migrations (pinned, body_text,
  notes.trashed, folders.trashed, ai_mode, emoji), then `PRAGMA
  user_version`: < 5 with an `action_items` table present => **refuse to
  open** (needs one open with Notes 5.0.0); < 5 otherwise => stamp 5. Then
  `db_ensure` (compare stored CREATE text, recreate on mismatch) for the
  views `trash_folder_ids`, `action_items_ctx` and the trigger
  `note_task_tombstone`.
- No WAL, no synchronous=NORMAL (deliberate: shared/network folders).
- Format versions: BNBF v6 (bnbf.h:153); a pre-v6 build refuses v6 blobs.
  `user_version` 2-5 are consumed; the next migration is 6+.
- App version: `VERSION` file (5.0.0) -> `-DON_VERSION` (Makefile:36-59).

### 5.7 Timers, idles, threads (whole app)

| What | Mechanism | Where |
|---|---|---|
| Editor autosave debounce | `g_timeout_add(1200 ms)`, re-armed per edit; flush on window destroy; blank notes deleted on close | editor_window.c:40, 836-848, 863+ |
| Typing undo group | `g_timeout_add(1000 ms)` | note_view.c:33, 334-345 |
| Caret blink | `g_timeout_add` per phase | note_view.c:618-665 |
| DB integrity check | `g_thread_new("on-db-health")` with its own read-only connection -> `g_idle_add` | db.c:803-841; app.c:114-140 |
| Rotating backup | `g_timeout_add_seconds_full` timer carrying the db path; worker `g_thread_new` with its own connection (online backup API -> verify -> prune); 4 `g_idle_add` completions | backup.c:263-480 |
| Search | worker thread, own connection, atomic cancelled flag, results via g_idle_add (sub-agent, section 4) | search_window.c |
| Grid thumbnails, media scan, deferred refresh, sidebar fit, tooltip gap, image-viewer resize settle | idles / tick callbacks / 40 ms slices (section 4) | library_window.c, media_window.c, image_viewer.c, app.c |
| IPC | GSocketService on the main loop; blocking reads with 10 s timeout | ipc.c:472-503 |

Rule the code keeps: a SQLite connection never crosses threads; each worker
opens its own.

### 5.8 Backup (backup.c)

Off by default. `backup_dir` or `<default db dir>/backups`. Each pass:
skip if `backup_source_stamp` (dest|path|size:mtime) is unchanged; write
`notes-YYYYMMDD-HHMMSS.db` via `on_db_backup_to` (SQLite online backup, 5
retries x 200 ms on BUSY); `on_db_verify_file`; delete the copy if it fails;
only then prune to `backup_keep` (default 10). Interval `backup_interval_min`
(default 60; 0 = manual only). Must be re-armed after File -> Open Database
File… because the timer carries the path. Also `notes backup FILE.db` on the
CLI (same `on_db_backup_to`).

---------------------------------------------------------------------------

## 6. Build

- One Makefile (18 KB), GNU make 3.81-compatible, `cc`, `pkg-config`
  (`/opt/local/bin/pkg-config` fallback).
- `CFLAGS = -std=c11 -Wall -Wextra -g -DON_VERSION="\"$(VERSION)\""
  $(pkg-config --cflags gtk4 sqlite3)`; `LDFLAGS = $(pkg-config --libs gtk4
  sqlite3) -lm` + `-lobjc` on Darwin (Makefile:57-68). No `-O`.
- Every object depends on all headers + Makefile + VERSION (Makefile:109).
- **No resource embedding**: no GResource, no .ui files, no generated code.
  Icons and `notes.ini.defaults` are loose files resolved relative to
  argv[0]; CSS is C string literals in the sources.

| Target | Does | Lines |
|---|---|---|
| `all` / `notes` | build `./notes` (objects in build/) | 96-111 |
| `run` | `./notes` — on the REAL database (CLAUDE.md warns never to do this for development) | 114-115 |
| `run-dev`, `dev-check`, `clean-dev` | sandbox in `dev/` with its own ini + seeded db via CLI calls | 127-159 |
| `test` | `build/test_document` (GLib only) | 167-176 |
| `bnbf-scan` | `build/bnbf-scan` (GLib + sqlite) | 178-183 |
| `ui-probe`, `ui-test` | build the probe from all objects minus main.o; run each `tests/ui/*.txt` on a copy of the sandbox db, PNGs to build/ | 185-203 |
| `clean` | `rm -rf build notes dist` | 206-207 |
| `app` | `dist/Notes.app`: binary copied as `Contents/MacOS/Notes`, `icons/` and `notes.ini.defaults` beside it in MacOS/, `.icns` from `icons/composition.png` via sips+iconutil, Info.plist by printf (bundle id `org.example.notes`). NOT self-contained (links /opt/local/lib dylibs); not code-signed | 226-262 |
| `install` | replace `~/Applications/Notes.app`, **carrying `Contents/MacOS/notes.ini` across**; refuses while the installed binary is running | 264-294 |
| `pkgroot`, `deb`, `rpm` | Linux packaging to /opt/notes + /usr/bin wrapper + .desktop | 296-372 |

Tests: `make test` covers ONLY document.c + bnbf.c (35 tests; no db, cli,
serialize, export or search_query tests exist). `make ui-test` is the only
editor-behaviour test and needs GTK + a display. There is no CLI test
suite, no db test, no CI config in the repo.

Contrast with the sibling `../files` (the style the fork is to follow):
`clang -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter
-Werror=unguarded-availability -mmacosx-version-min=14.0 -I src -MMD -MP`,
`-framework Cocoa ...`, `src/*.m` wildcard, bundle assembled in the tree
with `plutil` edits to `resources/Info.plist`, images copied to
`Contents/Resources`, ad-hoc/Apple Development codesign, `make test` linking
`tests/*_test.m` against all objects but main.o. files uses no GLib and no
SQLite.

---------------------------------------------------------------------------

## 7. External dependencies

Versions from `pkg-config --modversion` (MacPorts) and `otool -L ./notes`.

| Dependency | Version here | Used for | Stock macOS? |
|---|---|---|---|
| GTK 4 (`gtk4 +quartz`) | 4.22.4 (min 4.10/4.12 per docs) | all UI | No — MacPorts only. **Goes away** |
| GLib / GObject / GIO | 2.88.2 | containers, strings, unicode, GKeyFile, GRegex, GChecksum, GDateTime, threads, main loop, GIO sockets | No — MacPorts only. Keep or replace (section 10) |
| Pango / PangoCairo | 1.57.1 | text layout | No. Replace with CoreText (system) |
| HarfBuzz | 14.2.1 (pkg-config) | via Pango | No. CoreText shapes |
| cairo | 1.18.4 | GSK cairo renderer; grid thumbnail rendering (library_window.c:877-954) | No. CoreGraphics (system) |
| gdk-pixbuf | 2.44.6 | PNG decode/encode/probe, file image load | No. ImageIO (system) |
| graphene | 1.10.8 | rect types in snapshot code | No. CGRect |
| librsvg (optional) | 2.58.5 | 8 symbolic SVG arrows only | No. Not needed (NSOutlineView draws its own disclosure triangles) |
| SQLite | MacPorts 3.53.4 (linked: /opt/local/lib/libsqlite3.0.dylib) | database | **Yes**: /usr/lib/libsqlite3.dylib 3.51.0 + SDK header `MacOSX.sdk/usr/include/sqlite3.h`. Features used (recursive CTE, views, triggers, online backup API, `PRAGMA integrity_check`) are all available. Link `-lsqlite3` |
| libobjc | system | `objc_msgSend` pasteboard + NSWindow placement hacks | Yes |
| libintl | via GLib | — | goes with GLib |
| freetype/fontconfig | installed (pkg-config reports them) | NOT used by main | n/a |
| Tools | Apple clang 21.0.0, GNU make 3.81, sips, iconutil, osascript/textutil (import script), python3 (icon-prep) | | Yes |
| OS | macOS 26.6.2 (Darwin 25.6.0), arm64 | | |

System replacements for GLib facilities: CommonCrypto `CC_SHA256`
(GChecksum), `NSRegularExpression`/ICU (GRegex — PCRE2 and ICU differ at the
edges; POSIX `regcomp` is NOT a compatible substitute), `pthread` + GCD
(threads/idle/timeouts), BSD sockets (GIO), Foundation/CoreFoundation
(case folding for search: `g_utf8_casefold` -> `CFStringFold` or
`-[NSString stringByFoldingWithOptions:]`), `localtime_r`/`mktime`/`strftime`
(GDateTime).

---------------------------------------------------------------------------

## 8. Git state (reported only)

notes (`/Users/ian_campbell/salt_development/notes`):
- Branch `main` at `51ec5a4` "Remove the v5 migration: the live database has
  crossed", tracking `origin/main`, up to date. 200 commits. Last commit
  2026-09-18 16:16 -0400. Tags include `v5.0.0` (also 1.5.2, 2.0.1, 3.0.1,
  3.3.7).
- Remote `origin` = `git@cambio.github.com:IANatCAMBIO/orange_notes.git`.
- Remote branches: `origin/main` (51ec5a4), `origin/freetype-rendering`
  (c49e425, 2026-07-28, forked at 6ea13bc — stale GTK3-era, section 3.1),
  `origin/testing_code_cleanup` (1b13d53, 2026-07-09, stale). No local `gtk4`
  branch exists any more (merged into main at 56e0df3) although CLAUDE.md's
  first paragraph still says "the `gtk4` branch; `main` is the GTK3
  original" — that sentence is out of date.
- Working tree: **clean** (`git status`: nothing to commit).
- One stash: `stash@{0}: WIP on freetype-rendering: c49e425` (4 files, 14
  insertions).
- Ignored, present on disk: `build/` (objects incl. a leftover
  `migrate_v5.o` whose source is gone, test binaries, ui PNGs), `./notes`
  (649 KB, built 2026-09-18), `notes.ini` (points at the iCloud database),
  `dev/`, `dist/Notes.app`, `.DS_Store` files.
- Installed app: `~/Applications/Notes.app` (2026-09-18), running (pid 56318)
  during this research.

tasks (`../tasks`, for section 9): branch `master` at `9ce1df8`, 1 ahead of
`origin/master`; modified `src/library_window.c`; untracked
`client_credentials.mk`, `plugins/`.
files (`../files`): branch `main` at `d8b0edd`; local branches
`code-review-fixes`, `optimizations`; modified README.md,
resources/Info.plist, src/default_viewer.{h,m}, src/prefs_window.m.

---------------------------------------------------------------------------

## 9. Integration between Notes and the Tasks app

Precisely, as of today's trees:

1. **Separate database files, no shared file.** Notes: `notes.db`
   (`~/.local/share/notes/` default; iCloud Drive in practice). Tasks:
   `tasks.db` under `~/.local/share/tasks/` (tasks/src/db.h:52-53,
   db.c:226-248). Neither app opens the other's file. A MERGE of the two
   databases is planned ("preparation for merging this database with the
   sister Tasks app's", CLAUDE.md:417-418) but not done.
2. **Notes embeds the Tasks schema verbatim.** notes/src/db.c:52-63 and
   80-103 contain Tasks' DDL for `list_groups`, `lists`, `tasks` and
   `idx_tasks_list`; I compared them against tasks/src/db.c:537-604 and the
   column lists and DEFAULTs are the same text (the three `%d` defaults come
   from `ON_TASK_RECUR_TIME_DEFAULT`/`_LEAD_`/`ON_TASK_DUE_TIME_DEFAULT`,
   db.h:497-499, equal to tasks' `TASK_*_DEFAULT`, tasks/src/db.h:163-181).
   Status encoding 0/1/2 NEW/IN_PROGRESS/DONE is shared (db.h:500-502).
   Notes does NOT create Tasks' other tables (`attachments`, `sync_state`,
   `gtasks_*`, `notes_task`, `notes_deleted`).
3. **"Action items are task rows" (Notes side, commit 28335bd).** A '!' line
   is the rendering of a row in Notes' own `tasks` table. Link table
   `note_task(uid PK, task_id UNIQUE -> tasks ON DELETE CASCADE, note_id ->
   notes ON DELETE CASCADE, ord)` (db.c:104-109); uid allocated from
   `action_uid_seq`; the uid is stored IN the note blob as a BNBF v6 ACTION
   record. All note-born tasks go into ONE managed list "Action Items" with
   emoji U+2757 (db.h:507-508), the name Tasks' Notes plugin gave its own
   managed list. Notes writes only title/status/due/completed_at/updated_at/
   deleted (`task_apply`, db.c:1902). Reads go through the view
   `action_items_ctx` (db.c:156-166: note_id, ord, text, done, due, uid,
   folder_id, updated_at, visible, context), which is documented as "the
   contract the sister Tasks app will read directly once the two databases
   merge". Hard deletes tombstone tasks via the `note_task_tombstone`
   BEFORE DELETE trigger (db.c:173-178). Save path:
   `on_note_document_save` -> `on_db_note_save_actions` (create / update /
   re-ord / tombstone). External edit path: row first
   (`on_db_action_set`), then the line (`action_apply`,
   editor_window.c:502-557). Reconcile on every editor load
   (`on_note_document_reconcile`).
4. **Runtime link today: none in the current Tasks source.** Tasks used to
   mirror Notes' action items through a dlopen plugin (`notes.so`) that
   spawned the `notes` CLI (never the db file) and kept `notes_task(task_id,
   uid, done, due)` + `notes_rule` tables in tasks.db. Tasks commit `9b86b50`
   (2026-09-21, "Remove the plugin system: the port carries the core only")
   deleted the plugin ABI and all four plugins; `tasks/src/plugins/` now
   holds only a `.DS_Store`, and the only remnants are the v9 migration
   that creates/moves `notes_task`/`notes_deleted` (tasks/src/db.c:742-826)
   and docs. The untracked `tasks/plugins/notes.so` + `notes.README.md`
   (2026-09-09/14) are old build products with no loader in the current
   binary. The commit message says the integrations "return as integrated
   features once the GTK4 port is complete". I did not inspect which Tasks
   build is installed at `~/Applications/Tasks.app` (dated 2026-09-28, i.e.
   after the removal).
5. **No IPC between the two apps.** Tasks has no socket code
   (grep of tasks/src for GSocket/ipc/sock: nothing). The former link was
   process spawn of the Notes CLI, which itself forwards to a running Notes
   GUI over Notes' socket.
6. **Shared code by copy, not by library**: tasks' `app.c`, `main.c`
   (first-run chain) and `backup.c` state they follow / are ports of the
   Notes modules (tasks/src/app.c:7, main.c:11, backup.c:143); Notes'
   `hidden.png`/`visible.png` icons and the completed-items toggle come from
   Tasks. `list_rows.[ch]` exists in both.
7. **Identity design for the merge**: `note_task.uid` (Notes) is what Tasks'
   `notes_task.uid` keyed on; `tasks.id` will collide between the two files
   and is to be renumbered on one side by rewriting `note_task` only, never a
   blob (CLAUDE.md:519-527).

Implications for a joint fork: (a) the tasks/lists/list_groups DDL is a
shared contract — if both forks keep a C `db.c`, put that DDL in one shared
header; (b) the CLI is the only cross-app API that ever shipped, so the
forked Notes CLI must keep `action list --uid --json`, `action show`,
`action done/undone/due/text` exactly; (c) if the forks are the moment the
databases merge, `action_items_ctx` is the read contract and `task_apply`'s
column discipline is the write contract.

---------------------------------------------------------------------------

## 10. Assessment

### 10.1 The decision that shapes everything: what to do about GLib

Nothing in the tree is GLib-free. `../files` (the style target) uses no GLib
at all: `clang -fobjc-arc`, system frameworks only. Three options:

| Option | Core source change | New code | Cost / risk |
|---|---|---|---|
| A. Keep GLib (libglib-2.0 + gobject/gio from MacPorts) as the only non-system dependency | near zero in bnbf/document/db/search_query/export/cli | main-loop bridge | Lowest risk to the model and CLI. But: still a MacPorts dylib in the bundle (not self-contained unless bundled and re-signed), and **nothing pumps a GLib main loop under NSApplication** — the 6 `g_idle_add`/`g_timeout` sites in db.c/backup.c and all of GIO's socket service would silently never fire unless replaced or a GLib-loop pump is added |
| B. Compatibility shim: a local `glib_compat.[ch]` implementing the subset actually used | none to call sites; headers include the shim instead of `<glib.h>` | ~110 non-GIO functions: GString, GArray, GPtrArray, GByteArray, GBytes, GHashTable (int64/str keys, iter), GList, g_str*/g_ascii*/g_utf8* helpers, g_strdup_printf, g_markup_escape_text, g_base64_encode, GDateTime (5 functions), GChecksum -> CC_SHA256, file helpers, g_new/g_free, G_GINT64_FORMAT etc. Plus hand ports of GKeyFile (config), GRegex (search), GIO sockets (ipc), threads/idle/timer | Keeps the 35 model tests and byte-identical CLI as the oracle while removing the dependency. The shim is ~1.5-2.5k lines of dull C that must be exact (UTF-8 validation, casefold) |
| C. Mechanical rewrite to plain C types | 1,129 call sites + ~2,065 type tokens in the 9 core files; 555 more in tests | none | Highest churn in the most valuable, best-tested code; every signature in section 2.3 changes; easy to introduce subtle bugs in the loader/saver |

Recommendation: **B** if "no MacPorts at runtime" is a goal (it is for
`files`), otherwise A for the model with the main-loop and GIO parts
replaced regardless. Either way, do NOT let each UI engineer decide
locally; fix the primitive types first because every shared header uses
them. Per the user's own code-style rules (snake_case, no near-duplicates),
C also conflicts less with A/B: one implementation of each container.

GLib facilities that need a real replacement under any option:
GIO sockets (ipc.c, ~40 sites) -> BSD sockets + dispatch source;
g_idle_add / g_timeout_add / g_thread_new (db.c 3, backup.c 7, plus every UI
timer) -> GCD; GKeyFile (app.c config, ~12 sites) -> hand-rolled ini that
round-trips GKeyFile escaping; GRegex (search_query.c 4 sites) ->
NSRegularExpression (ICU; closest to PCRE) behind a C wrapper; GChecksum
(db.c 4 sites) -> CommonCrypto; g_utf8_casefold (search_query.c, cli.c, 7
sites) -> CFStringFold; GSubprocess + g_shell_parse_argv (AI) -> NSTask +
a word splitter; g_spawn_async (image open) -> NSWorkspace; GdkPixbuf (11
sites) -> ImageIO.

### 10.2 File-by-file disposition

| File | Lines | Disposition | Work (GLib call sites / other) |
|---|---:|---|---|
| bnbf.c/h | 653 | **carry unchanged** (A/B) | 34 g_calls if C. GDateTime due-date parse (4 sites) needs exact local-midnight semantics |
| document.c/h | 3228 | **carry unchanged** (A/B) | 228 g_calls if C. `pixels`/`pixels_free` slot already toolkit-neutral |
| db.c/h | 3300 | **carry**, 3 edits | `g_thread_new` + `g_idle_add` in the async health check (db.c:821-841) -> pthread/GCD + main-thread shim; `g_get_user_data_dir` (396) -> explicit path policy; GChecksum. 179 g_calls if C. Use system sqlite3 |
| search_query.c/h | 315 | **carry**, replace GRegex (4 sites) and casefold | 28 g_calls |
| export.c/h | 674 | **carry**, header fix only (`app.h` -> core header) | 142 g_calls if C (81 are GString) |
| serialize.c/h | 497 | **split**: keep load/save/reconcile/extract/count/nth_png (≈170 lines) unchanged; **rewrite** the 5 pixbuf functions on ImageIO (≈110 lines) and change 3 signatures | 9 gdk_pixbuf sites; 42 g_calls |
| cli.c/h | 2760 | **carry**, 3 edits | `gdk_pixbuf_new_from_file` (1007) -> ImageIO; `on_editor_action_set_*` (3 sites) -> core `action_edit`; includes. 269 g_calls if C (150 are g_strcmp0). Output must stay byte-identical |
| backup.c/h | 601 | **carry**, replace main-loop use | 4 g_idle_add, 1 timer, 1 thread; OnApp fields move to the core context. 82 g_calls |
| ipc.c/h | 786 | **carry protocol, rewrite transport** | ~40 GIO sites -> BSD sockets; 3 UI calls -> hooks; keep framing, limits, socket naming. 125 g_calls |
| app.c/h | 1680 | **split**: lift config (862-1016), read_stream, status, location text, emoji predicates (≈200 lines) into core; **delete** the rest (GTK helpers/workarounds) and re-provide notice / pick-path / icons natively | GKeyFile ~12 sites |
| main.c | 516 | **rewrite** as main.m + app delegate (keep the order: config -> CLI -> db pre-open -> app; Welcome chain; startup_finish order; SIGTERM flush) | |
| doc_layout.c/h | 1899 | **re-back**: keep ≈40-55% (invalidation, hits diff, code runs, image fit, table fitting, block_at_y, sel_span, cross-block move) as C; replace `text_layout_new` + all Pango queries + painter with CoreText/CG behind `text_layout.h` | 94 pango + 16 gtk + 17 gsk/graphene + 3 gdk sites |
| note_view.c/h | 3254 | **rewrite view**, **lift policy**: ≈1,200-1,400 lines of editing policy (typing, Enter/Backspace, toggles, paste_fragment, tag capture, find, image ordinals, action rewrites, undo grouping) into a toolkit-free `note_edit.c`; new NoteView.m for NSView/NSTextInputClient/mouse/pasteboard/menus/a11y | 135 gtk + 24 gdk sites go |
| editor_window.c/h | 1897 | **rewrite**; lift `action_apply`, save/load/close policy, tables | |
| library_window.c/h | 6283 | **rewrite, split into 5 files** (4.1 / 10.5) | 493 gtk sites |
| list_rows.c/h | 350 | **delete**; plain C row structs | |
| search_window.c/h | 710 | **rewrite window**; lift `search_worker` + job to core | |
| media_window.c/h | 768 | **rewrite**; lift scan state machine | |
| image_viewer.c/h | 910 | **rewrite**; keep ops contract (neutral image type), geometry constants, nav rules | |
| settings_window.c/h | 1441 | **rewrite**; port `BOOL_SETTINGS` table, health-text logic, `dir_shares_fate` | |
| tests/test_document.c | 1846 | carry (A/B); with B, add `g_test_*`/`g_assert_*`/`g_rand_*` to the shim (555 sites) | |
| tests/ui_probe.c + tests/ui/*.txt | 327 | **rewrite probe** on the AppKit view; **keep the scripts** as the acceptance test (map GDK key names) | |
| tools/bnbf-scan.c | 321 | carry (A/B) | |
| tools/gtk4-spike | | drop | |
| tools/import-apple-notes.sh, icon-prep.py | | carry; fix the `BIN=` path | |
| Makefile | | **rewrite** in the `files` style; drop deb/rpm/pkgroot; keep `test`, `run-dev`/`dev-check`, `app`, `install` (with ini carry-over or a new ini location) | |
| skills/notes/SKILL.md | | update binary paths and the "GTK4" description; behaviour unchanged | |

Totals: ~9.9k lines carried (model, db, cli, export, query, tests/tools),
~1.5k lines of core touched (serialize image half, ipc transport, backup/db
scheduling, config), ~17.5k lines of GTK replaced by new Objective-C, of
which roughly 2-2.5k lines of logic can be lifted into C rather than
rewritten (note_edit, action_edit, search worker, media scan, save policy).

### 10.3 Risks and hidden couplings

1. **UTF-8 byte offsets vs UTF-16** (3.3). Every OnPos/OnRun/OnDocHit is in
   bytes; CoreText, NSTextInputClient ranges and NSAccessibility are UTF-16.
   One wrong conversion corrupts text mid-codepoint (`clamp_pos`
   note_view.c:185-223 defends against it today). Confine conversion to the
   text-layout backend and one linear-index helper.
2. **CLI <-> GUI single-writer contract depends on three things matching
   between any two builds that touch one database**: the ini both read
   `db_dir` from, the socket directory, and `g_str_hash(db path)`
   (ipc.c:45-58). A mismatch does not fail loudly: the CLI just goes
   headless and writes the database behind a running GUI, whose next
   autosave can overwrite the change. The skill and (historically) the Tasks
   plugin both rely on this.
3. **Config location**. Today the live ini sits inside
   `~/Applications/Notes.app/Contents/MacOS/`. A signed bundle cannot be
   written into; moving the ini loses `db_dir` unless adopted, which looks
   like "my notes vanished" + the Welcome dialog; answering "Create" there
   orphans the real database (CLAUDE.md "Custom DB location").
4. **Main-loop assumptions in the core** (2.2 item 4): with GLib kept but no
   GLib loop running, the health verdict never lands (Settings shows
   "Checking…" forever) and backups never finish (`backup_running` stays
   TRUE). Easy to miss because nothing crashes.
5. **Flush on quit**. Editors save from their window-destroy handler; Quit
   and SIGTERM work by destroying windows (library_window.c:2985-2992,
   main.c:357-367). AppKit needs `applicationShouldTerminate:` + a SIGTERM
   dispatch source that close every editor first; and the "blank note is
   deleted on close" rule must run there too.
6. **`OnApp` is shared by core and UI** and holds GTK pointers; `cli.c`
   builds a zeroed one on the stack and code tests `app->editors == NULL` to
   mean "headless" (editor_window.c:406-413). Keep that invariant explicit
   in the new core context.
7. **Image type at the core boundary**: `on_note_view_insert_image
   (GdkPixbuf*)`, `on_png_decode_capped`, `on_note_image_nth`,
   `on_image_png_bytes`, `OnImageViewerOps.render -> GdkPaintable*`, the
   `"on-png"` object-data cache on pixbufs. Pick ONE neutral image type
   (CGImageRef) and PNG-bytes-in/out functions before anyone writes a view.
8. **Search semantics**: GRegex (PCRE2) and `g_utf8_casefold` define what
   `notes search` and the search window match. A replacement changes results
   at the edges; the CLI is an agent-facing API.
9. **Due dates** are local-midnight UNIX timestamps produced by GDateTime
   (bnbf.c:119-126, document.c:2656-2658, the due dialog). Replacements must
   agree across DST or items shift a day and the reconcile will rewrite
   lines.
10. **Running as a bare binary**. Today one executable is both CLI and GUI,
    and the skill calls the repo-root binary. A Cocoa GUI started from an
    unbundled binary (the `quicknote` / `note open` fall-through that
    returns -1 and starts the GUI) has no Info.plist/bundle identity.
    Decide: the bare `notes` is CLI-only and launches the .app for the two
    GUI commands.
11. **Dev sandbox safety** (`make run-dev`, CLAUDE.md "NEVER run ./notes
    from the repo root") depends on the argv[0]-adjacent ini. Preserve an
    equivalent (the dev binary still reads the ini next to itself) or every
    test run opens the real iCloud database.
12. **No app sandbox / hardened-runtime assumptions**: arbitrary database
    paths (iCloud Drive), a unix socket in `~/.cache`, spawning the AI
    command and `open`, writing temp PNGs. An App Sandbox entitlement would
    break all of them.
13. **No Edit menu, Cmd+M, Cmd+E** (4.2): AppKit conventions collide with
    existing bindings; needs an owner decision, not an engineer's guess.
14. **Database refusal rule**: `on_db_open` refuses a pre-5.0 file that
    still has `action_items` (db.c:479-489). The fork inherits it; keep the
    message and do not "helpfully" stamp such a file.
15. **Tasks coupling** (section 9): Tasks' DDL is duplicated verbatim in
    notes/src/db.c; if the Tasks fork changes its schema the two diverge
    silently. The Tasks side of the integration currently does not exist in
    source (plugin removed 2026-09-21).
16. **Stale docs** (4.10) will mislead porters who trust CLAUDE.md /
    Internals.md over the source.
17. **Pasteboard interop** between a GTK build and the AppKit build for the
    `application/x-notes-bnbf` flavour depends on how GDK names custom
    types on NSPasteboard — not verified; treat cross-build rich paste as
    unsupported unless tested.

### 10.4 The five hardest pieces

1. **CoreText text-layout backend** (replacing 94 Pango call sites): line
   breaking parity (WORD_CHAR), byte<->UTF-16 mapping incl. preedit splice,
   caret/hit-test agreement at wrapped-line ends and after emoji clusters,
   inline-image run delegates, manual strikethrough and find-hit
   backgrounds, selection x-ranges, centred title line.
2. **NoteView input stack**: NSTextInputClient over a block model (marked
   text, linear ranges, firstRect), `doCommandBySelector:` mapping, mouse
   selection across blocks/tables/images, pasteboard read/write of BNBF /
   PNG / TIFF / text, the non-activating #tag popup, keeping the
   `feed_key/text/click` probe API so `tests/ui/*.txt` still run.
3. **Library notes pane**: one selection shared by NSTableView and
   NSCollectionView, sort + column order/visibility persisted in the
   existing ini format, in-place single-row update that preserves selection
   and scroll, async thumbnail rendering with a cache keyed on updated_at.
4. **Sidebar**: NSOutlineView with conditional sections, counts, expansion
   and selection restore across full rebuilds, drag-and-drop validation
   (into/before/after, subtree and trash rules, drag-out-of-trash restore),
   and fit-to-content divider sizing.
5. **Core extraction and process plumbing**: de-GTK the headers, replace GIO
   sockets while keeping the wire format and socket naming, main-thread
   shim, config relocation with adoption, byte-identical CLI output, flush
   on terminate — unglamorous, but every other package depends on it and
   its failures are silent data-safety failures.

### 10.5 Proposed work packages (8; file ownership is disjoint)

| WP | Scope | New/owned files | Replaces | Depends on |
|---|---|---|---|---|
| **A. Core & platform** (must lead; headers frozen in week 1) | GLib decision implemented; `app_core.h` (core OnApp, hooks); `config.c` (ini); main-thread/timer shim; `image_io` (ImageIO decode-capped / PNG encode / probe / file->PNG); `action_edit.c` (from `action_apply`); `search_job.c` (worker); `ipc.c` on BSD sockets; backup/db scheduling; cli.c edits; Makefile, bundle, `make test`, `run-dev`; golden CLI test vs the GTK build on the dev sandbox | core dir, Makefile, tests/ | app.c config half, serialize.c image half, ipc transport, main-loop sites | nothing |
| **B. Text layout** | `text_layout.h` + `text_layout_ct.m`; adapt `doc_layout.c/h` to neutral types (`on_rect`, colour, CG context); painter on CoreGraphics; image cache as CGImage | doc_layout.*, text_layout.* | Pango/GSK in doc_layout.c, `on_emoji_pad` | A (types, image_io); the frozen doc_layout.h |
| **C. Note view** | `note_edit.c` (lifted policy) + `note_view.m` (NSView, NSTextInputClient, mouse, pasteboard, caret, #tag popup, context menu, find, a11y); new `ui_probe.m`; run tests/ui scripts | note_view.*, note_edit.*, tests/ui_probe | note_view.c | doc_layout.h (B can be stubbed early), A |
| **D. Editor window + image viewer** | window controller: toolbar (compact/expanded), find field, status bar, autosave/flush/blank-delete, placement, open variants, `*_all` refreshers, viewer hosting; `image_viewer.m` overlay | editor_window.*, image_viewer.* | editor_window.c, image_viewer.c | note_view.h (C), A |
| **E. App shell + library shell + commands** | `main.m`, app delegate (CLI hand-off, Welcome chain, startup_finish order, terminate/SIGTERM flush, IPC hooks), main menu (File/View + a new Edit menu), library window frame (split views, toolbar, status bar, view-mode + sidebar/completed toggles, notify hooks), commands with no view: new note/folder + folder sheet, trash/delete/restore/empty, pin, sort, export, Open Database, About, quit; shared `library_state.h` | main.m, app_delegate.*, library_window.m, library_commands.m, library_state.h | main.c, library_window.c 2111-3489, 3651-3749, 4157-4311, 5047-5416, 6074-6197 | A; interfaces of F, G |
| **F. Library sidebar** | outline data source/delegate, counts, expansion + selection restore, fit-to-content, sidebar context menu, drop validation + drop handling, folder drag source | library_sidebar.m | library_window.c 403-820, 1339-1386, 1702-2109, 3085-3134, 5456-5663 | library_state.h (E), A |
| **G. Library notes + action items** | notes table + collection view with the shared selection, sorters, column-layout persistence helper (used by both tables), density/preview, thumbnails + cache + async fill, note context menu, note drag source, single-row save update; Action Items table, checkbox, due-date sheet, urgency colours | library_notes.m, library_actions.m, library_columns.m | library_window.c 822-1337, 1406-1700, 3491-3649, 4313-5045, 5665-5849 | library_state.h (E), `action_edit` (A), editor open (D) |
| **H. Secondary windows** | Settings (table-driven booleans, Database plate, backups, AI settings), Search window (over `search_job`), Media window (collection view + scan), AI summary pane + NSTask runner | settings_window.m, search_window.m, media_window.m, library_ai.m | settings_window.c, search_window.c, media_window.c, library_window.c 3751-4155 | A; image_viewer.h (D); library_state.h (E) |

If only 5-6 engineers: merge B+C (one text person, sequentially), F+G is too
large to merge — instead merge H into D's owner after the editor is up, and
have E's owner also take A's Makefile/bundle. B and C are the critical path;
start them first. E, F, G can develop against a stub editor
(`on_editor_window_open` that logs).

### 10.6 Interfaces to agree before work starts

1. **Primitive types / GLib policy** (10.1) — one header everyone includes.
2. **`app_core.h`**: core context (db, db_dir, db_transient, preference
   fields, backup_running/backup_timer) + hooks: `notify_notes_changed`,
   `notify_note_saved(id)`, `notify_status(msg)`, `notify_db_state`,
   `notify_ai_changed`, and NEW ones replacing the upward calls:
   `open_note(app, id, reveal_kind, ord, search_term)`, `quicknote(app)`,
   `live_document(app, note_id)` + `live_edited(app, note_id)` (for
   `action_edit`), `close_editors(ids, n)` / `close_all_editors`,
   `editors_settings_changed(what)`. Headless = all hooks NULL.
3. **Main-thread shim**: `on_main_post(fn, data)`, `on_timer_add(seconds,
   repeating, fn, data) -> id`, `on_timer_remove(id)`; rule: one SQLite
   connection per thread.
4. **Image API**: opaque image (CGImageRef) + `decode_capped(bytes, n,
   max_px)`, `png_encode(image) -> bytes`, `probe_size(bytes, n, &w, &h)`,
   `file_to_png(path) -> bytes`; `on_note_view_insert_image` takes PNG
   bytes; `OnImageViewerOps.render` returns the neutral image.
5. **`doc_layout.h` (neutral)**: `on_rect {x,y,w,h}` doubles, colour
   struct, `on_doc_layout_new(doc, font spec)`, `set_preedit(pos, text,
   cursor_bytes)` (no attr list), `draw(L, cg_context, const OnDocPaint *)`,
   `image(L, block, inline_ord)`; everything else as today (hit, move,
   caret_rect, word_at, line_at, code_run, set_hits, observer relays).
6. **`text_layout.h`** (inside WP B, but C's IME code depends on its
   byte-offset convention): all indices crossing it are UTF-8 bytes.
7. **`note_view.h` (C-callable)**: today's function list
   (note_view.h:54-205) with `GtkWidget*` -> opaque view pointer, callbacks
   struct `{edited, inline_flags_changed, image_activated}` instead of
   signals, `feed_key` taking a neutral key enum + modifier mask.
8. **`editor_window.h`**: the four open variants, the `*_all` refreshers,
   and the editor registry (who owns note id -> controller).
9. **`library_state.h`**: selection `(kind, id, name)`, `shown_kind/id`,
   view mode + `grid_pref`, `populating`; functions
   `library_refresh_sidebar/notes/all`, `library_selected_note_ids`,
   `library_notes_for_selection`, `library_in_trash_view`,
   `library_current_folder_id`, `library_set_selection(kind,id)`,
   `library_invalidate_folder_paths`, exported commands `trash_notes_core`
   and `trash_folder` (used by both menus and drops); the SB_KIND enum and
   plain-C row structs (ex list_rows.h); `OnSearchScope`.
10. **Command table**: the existing action names (4.2) as stable command
    ids -> C functions, used for NSMenu items, toolbar items and validation,
    so three engineers do not invent three dispatch schemes.
11. **Identity and paths**: bundle id, app/process name, ini location and
    adoption rule, default db dir, socket dir + hash, pasteboard type
    strings (BNBF fragment; drag rows), whether the fork shares a database
    and socket with the GTK build during the experiment.
12. **Look constants**: one header for the former CSS/layout numbers
    (colours in doc_layout.c:42-52, ROW_TINT, due colours, sidebar colours,
    sizes: THUMB_SIZE 140, MEDIA_THUMB_BOX 144, toolbar icon 24, margins).
13. **Keyboard map**: the full shortcut list (4.2) with decisions on Cmd+M,
    Cmd+E and the new Edit menu.

---------------------------------------------------------------------------

## Not verified

- Nothing was built or run; every behavioural statement is from reading
  source. No timing figure quoted from the docs (18 ms layout, 1.7 s
  integrity check, etc.) was re-measured.
- `document.c` (2,685 lines), `export.c`, `backup.c`, and most of `cli.c`
  and `db.c` were outlined and grepped, not read line by line. The
  operation semantics in section 2.3 are from `document.h`'s contracts.
- Sections 4.1-4.8 rest on two sub-agents' full reads; I spot-checked the
  items marked ✓ and the facts I reuse elsewhere, not every line reference.
- Whether GApplication de-duplicates a second GUI launch on macOS (5.1) was
  inferred from ipc.h's statement and the code, not tested.
- How GDK names the `application/x-notes-bnbf` type on NSPasteboard
  (cross-build paste compatibility).
- Which Tasks build is installed at `~/Applications/Tasks.app` and whether
  it still loads the old Notes plugin.
- GLib's exact fallback for `g_get_user_runtime_dir()` was observed
  (sockets present in `~/.cache`), not read from GLib source; likewise
  `g_str_hash`'s algorithm (djb2 over signed chars) is from memory of GLib,
  not checked against the installed 2.88 source.
- GKeyFile escaping rules (5.5) are from memory of GLib's format, not
  re-derived from its source.
- The claim that NSTextTable forces TextKit 1, and CoreText's lack of
  strikethrough/background drawing, are from platform knowledge, not tested
  here.
- pkg-config version strings were reported as printed (harfbuzz "14.2.1",
  freetype2 "26.6.20" look like non-release numbering) and not
  cross-checked against `port installed`.
- The remote branches were inspected from local remote-tracking refs; no
  `git fetch` was run, so the remote may have moved.

