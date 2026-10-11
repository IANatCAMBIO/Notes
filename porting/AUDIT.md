# Port audit — what the AppKit build lacks against the GTK app

Audit of 2026-10-10: every package read against `porting/gtk_reference/src`
(the specification) and `GTK_PORTING_MAP.md`.  Line numbers: `lw.c` =
gtk_reference `library_window.c`; port files are under `src/`.  Each item is
ticked only when it is fixed AND covered by a test that drives the behaviour
(events, database state) — never by a "does not crash" check.

Severity: **H** = noticed in normal use, **M** = noticed sometimes, **L** = cosmetic or rare.

## Done (2026-10-10)

Step 1 — data loss and dead windows:
- [x] H Image viewer (editor and media) stayed at zero size: an NSScrollView does not autoresize extra subviews; it now follows the covered view's frame
- [x] M Programmatic sidebar selection of a nested folder failed: ancestors were expanded innermost first
- [x] M ⌘⌫ (and every Move to Trash) is off while a library text field is being typed in

Step 2 — the editor view (part 1):
- [x] H Typing into a table cell made a new paragraph before the table (caret_ensure_text tested the block, not the caret's text)
- [x] H Reshaping a table crashed: the layout freed cells by the document's new size (also wrong after block removal)
- [x] M Home/End and Shift+Home/End work on the line (AppKit sends Shift+End as "select to document end")
- [x] M One click path (press_at) for the mouse and the probe; one key dispatcher (run_command) for key bindings, the Edit menu and the probe

Step 2 — the editor view (part 2):
- [x] M Find highlights were capped at 32 per block; Insert Image stored a JPEG/HEIC file's bytes as if PNG
- [x] M Insert Image and pasted images are converted to PNG once

Step 3 — sidebar and folders:
- [x] H Folder emojis showed as mojibake ("üß™"): NSString formats read %s in the system encoding, not UTF-8 (also the image caption)
- [x] M New Subfolder… is New Folder on a folder's menu (the dead new-subfolder command is gone); the inline rename (not in GTK) is gone — Info… renames

Earlier:

- [x] H Double-click / Return opens a note: list, grid, Action Items (grid was not even selectable)
- [x] H Editor: ordinary title bar, format bar and status bar in the body (no NSToolbar / titlebar accessory)
- [x] H No native dialogs: tool panels (`tool_panel.h`) for every alert; New Folder in a tool panel
- [x] H Editor commands: find, find next/previous, use selection, B/I/U/S, paragraph styles, insert — menu, shortcuts and format bar share one handler table; gated while the find field has focus
- [x] M Editor find field: Return/Down next, Up previous, Escape back to the note; previous/next buttons
- [x] M Editor status bar: folder location, optional `id:N`, red/green save dot with tooltip; refreshed when the window comes forward
- [x] M Editor compact toolbar setting ("Aa" and "≡" menus), rebuilt live from Settings
- [x] M Editor placement bottom-right of the library's screen; note focused on open
- [x] M Image viewer in the editor: keys reach it before the note (was edited blind); caption names note, index, size; "Open in Image Viewer"
- [x] M Insert → Emoji… (character palette)
- [x] M Find Previous re-selected the current match
- [x] M Save: a tag change now triggers the full library refresh (sidebar tags)
- [x] M Note text did not wrap to the window width

## Editor view (note_view.m, doc_layout.c, text_layout.c)

- [x] H Probe runner skips `click image find step step-back settle redo select-all date width` — fix the harness first; it hides real bugs (ui_probe_test.m:229-318 vs gtk ui_probe.c:227-311); run with GTK's settings and 640 px
- [x] H Margins applied twice (layout already adds them): text indented 32/24 pt, 64 pt less wrap width (note_view.m:295, 1229, 1270, 2254, 1689)
- [x] H `current_style` ignores first_line_title, code_line_numbers, code_copy_buttons (note_view.m:312)
- [x] H Centred title never centred (no flush offset in text_layout.c)
- [x] H Find does not select the first match at/after the caret (note_view.m:1979)
- [x] H Cocoa key bindings not handled: ⌘←/→ (+shift), Home/End, ⌃A/⌃E, ⌘⌫ … beep (doCommandBySelector)
- [x] H Option-← / Option-⌫ jump/delete to block start (on_text_layout_prev_word)
- [x] H Keyboard moves never scroll the caret into view (set_caret)
- [x] H Checkbox click does nothing (no ON_HIT_CHECKBOX; set_kind instead of set_checked)
- [x] H No context menu: image Copy/Open/Full size, table rows/columns/header/delete, Cut/Copy/Paste
- [x] H IME replacement range ignored (press-and-hold accents insert instead of replace)
- [x] H #tag capture and tag autocomplete absent — typing "#foo " never makes a tag
- [x] H Inline images never laid out or drawn
- [x] M Page Up/Down do not scroll a page; End on a wrapped line lands on the next line
- [x] M Marked text: markedRange, preedit cursor, reset on caret move/click/paste, committed "\n"
- [x] M Cursor shapes (I-beam / pointer over image, checkbox, copy word)
- [x] M Click does not reset goal_x / end typing group; image opens twice on double-click
- [x] M Code-block "copy" word absent
- [x] M Paste: TIFF stored as PNG bytes; JPEG/HEIC refused
- [x] M Find misses table cells; ASCII-only case folding; highlights stale after edits/loads
- [x] M Caret drawn when unfocused / with a selection; opening puts caret on the title line
- [x] M Enter at end of a table cell shows no new line
- [x] M Image ordinals: image/reveal count blocks only, count/png count inline too
- [x] M pos_to_utf16 reads out of bounds in table cells
- [x] L select_all/doc_end/clamp_pos with tables; end_external_edit fires edited twice; empty selected lines unmarked; action_reveal offset 0 not 1 and no 30 % scroll; Escape swallowed
- [ ] L Accessibility (GTK implements accessible text; VoiceOver sees nothing in the note view)
- [ ] L note_edit.[ch] holds only on_note_edit_is_blank; the editing policy its header promises is in note_view.m

## Notes list / grid / Action Items (library_notes.m)

- [x] H Action Items checkbox: objectValue overwrites state; toggle sends uid 0/1 — wrong item edited
- [x] H Endless thumbnail reload loop for notes without images; no in-flight tracking
- [x] H Selection/scroll restore by index not id; navigation keeps old selection, no scroll reset
- [x] H Delete bypasses on_library_trash_notes (editors left open, no status); no permanent delete in Trash
- [x] H No context menu (Open, Pin/Unpin, Export, Move to Trash; Trash: Open, Restore, Delete Permanently)
- [x] H toggle-done has no handler; completed items always listed
- [x] H Action Items: four sortable columns (Done, Context, Action, Due), `action_columns` persistence, strike-through when done
- [x] H Density (Compact/Comfortable), bold titles, preview line ignored
- [x] H List columns Path/Title/Modified/Created, sort header, `list_columns` persistence (known)
- [x] H Grid thumbnails: white card, border, image + body text; selected cards look unselected
- [x] H Note drag source (known); due-date tool panel (known)
- [x] M Shared selection between list and grid; default sort Modified newest-first drives grid
- [x] M One-row update is a full reload; no re-sort, no preview
- [x] M Due colours use UTC days; date formats; GTK colours (#c01c28/#d19a00/#26a269)
- [x] M Pin affects first note only; label never "Unpin"
- [x] M Ticking a box reloads the whole list; sidebar count not refreshed
- [x] M Thumbnails shared between list and grid sizes; scale hard-coded 2.0; list rows should have no thumbnails
- [x] M Alternating row tint; "N files selected" status
- [x] L Stale shown-state after Action Items; restore status; toggle-view label; dead code and missing comment blocks

## Sidebar (library_sidebar.m)

- [x] H Context menu never appears (`outlineView:menuForEvent:` is not a delegate method)
- [x] H "Notes" root row cannot be selected (treated as a header); start selection should be root
- [x] H Note drops never work (private pasteboard type); no drops on root/Trash; folder onto Trash
- [x] H Folder commands without handlers: folder-info, sort-subfolders, delete-folder, folder-restore, empty-trash
- [x] H Folder prompt lacks emoji and AI mode (New Folder, Folder Info share one tool panel in GTK)
- [x] H Fit-to-content never runs (`_split` never set)
- [x] H Restore Folder moves to top level (should be on_db_folder_restore)
- [x] H Trash folder: wrong confirm ("cannot be undone"), editors left open, wrong selection
- [x] H Permanent delete / Empty Trash leave editors open; no status
- [x] M Search Here… missing from menus; root menu; menus should be built from commands
- [x] M New Subfolder creates "New Folder" immediately instead of prompting
- [x] M Folder reorder before/after; between-row drops; drag trashed folders out to restore; drop status messages
- [x] M Selection restore refreshes notes twice; status path not updated on navigation
- [x] L Section styling, Trash disclosure, trashed counts, emoji spacing, scroll jump, rename extras

## Shell, menus, commands (app_delegate, library_window, library_commands)

- [ ] H note-export-html / note-export-md have no handler — handlers added and enabled in the note menu (step 4); not yet driven by a test (the folder picker)
- [x] H Status bar location never follows navigation
- [ ] M open-db: no .db filter, no same-db no-op, failure not reported
- [ ] M search / find ignore the selected folder or tag as default scope
- [ ] M App-scope commands do not reopen a closed library window
- [x] M ⌘⌫ may trash notes while typing in a field (verify)
- [ ] M Second instance should raise the running one
- [ ] L About details; toolbar labels/tooltips/separator; AI button hidden when AI off; window title; status fade

## Settings (settings_window.m)

- [ ] H AI section missing (ai_enabled, ai_command, ai_custom_prompt) — AI can never be turned on
- [ ] H Database health plate missing; notify_db_health never set
- [ ] H Checkboxes start unticked for keys whose default is on (read OnApp, not config with NO)
- [ ] H Most settings have no reader (density, bold titles, show done, code/title options)
- [ ] M Image viewer setting; backup interval/keep validation; "Backing up to …" line and warning; timer re-arm; folder picker is a sheet and duplicates on_ui_pick_path
- [ ] L Labels and tooltips; title; layout; stale comments

## Search window (search_window.m)

- [x] H Window controller not retained (ARC): typing probably does nothing; job callback may use freed memory
- [ ] H Case-sensitive and regex options missing
- [ ] M Search on Enter/Search button, not as-you-type; highlight term from the parsed query; Return opens; default scope; empty query; size persistence
- [ ] L Status line, title, column, placeholder

## Media window (media_window.m)

- [x] H Window controller not retained
- [x] H Thumbnails not selectable — click does nothing
- [x] H Viewer shows the 144 px thumbnail, not the image
- [ ] M Synchronous scan on the main thread, no cap; captions/tooltips; viewer caption; size persistence (viewer keys done)
- [ ] L Title/status scope; blank cells; Retina scale

## AI summary (library_ai.m)

- [ ] H Entire summary is a stub (placeholder text)
- [ ] M Status throbber; re-run guard; Copy button; resizable pane
- [ ] L Button hidden when AI off; panes never released

## Tests

- [ ] H Heap check: ~45–70 KB per editor open/close cycle, on the old and new code alike (budget 48 KB) — a real leak to find, not a budget to raise
- [ ] M `/ops/fuzz` fails about 1 run in 10 ("duplicate action uid") — core document model
